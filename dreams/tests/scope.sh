#!/bin/sh
# Resolution and purity, checked against the compiler being replaced.
#
# Two questions, and they are not the same one. First: does this pass accept the
# programs `dreamc` accepts? A checker that rejects working code is worse than
# no checker. Second: does it *reject* what `dreamc` rejects, with the same
# message? An analysis that accepts everything would pass the first check
# perfectly, so the second is the one that means anything.
set -u

dreamc=${dreamc:-target/debug/dreamc}
dream=${dream:-build-dream/bin/dream}
image=${image:-/tmp/dreams-scope.dream}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fail=0
say_fail() { echo "FAIL: $*"; fail=1; }

"$dreamc" dreams/main.dr -L mind -L . -o "$image" >/dev/null || exit 1

# --- the corpus must still be accepted --------------------------------------

checked=0
for f in mind/std/*.dr mind/tool/*.dr examples/*.dr examples/*/*.dr \
         dream/tests/programs/*.dr dreams/*.dr; do
    [ -f "$f" ] || continue
    "$dreamc" "$f" --no-emit -L mind -L . -L examples >/dev/null 2>&1
    want=$?
    "$dream" "$image" --check -L mind -L . -L examples "$f" >/dev/null 2>&1
    got=$?
    if [ "$want" -ne "$got" ]; then
        say_fail "$f: dreamc exited $want, dreams exited $got"
        "$dream" "$image" --check -L mind -L . -L examples "$f" 2>&1 | sed 's/^/    /' | head -5
    fi
    checked=$((checked + 1))
done
echo "$checked programs get the same verdict from both compilers"

# --- and the broken ones must be rejected, for the same reason --------------

mkdir -p "$tmp/pkg"
cat > "$tmp/pkg/mind.toml" <<'EOF'
[package]
name = "fixture"
src = "."
EOF

# Each case: a file, and the message both compilers must produce.
cat > "$tmp/pkg/unbound.dr"  <<'EOF'
let main! = nosuchname 1;
EOF
cat > "$tmp/pkg/impure.dr"   <<'EOF'
import std.console;
let shout x = console.print! x;
EOF
cat > "$tmp/pkg/member.dr"   <<'EOF'
import std.list;
let main! = list.no_such_member [1];
EOF
cat > "$tmp/pkg/modvalue.dr" <<'EOF'
import std.list;
let main! = list;
EOF
cat > "$tmp/pkg/duplet.dr"   <<'EOF'
let a = 1;
let a = 2;
EOF
cat > "$tmp/pkg/shallow.dr"  <<'EOF'
import std;
let main! = std.map (fn x -> x) [1];
EOF
cat > "$tmp/pkg/clash.dr"    <<'EOF'
import std.list.{map};
let map x = x;
EOF
# A base with a hole, and a module that derives it without filling the hole.
cat > "$tmp/pkg/shape.dr"    <<'EOF'
virtual let area s;
let describe s = area s;
EOF
cat > "$tmp/pkg/nofill.dr"   <<'EOF'
derive shape;
let main! = describe 1;
EOF

expect() {
    file=$1 want=$2
    out=$("$dream" "$image" --check -L mind "$tmp/pkg/$file" 2>&1)
    status=$?
    [ "$status" -eq 1 ] || say_fail "$file: exit status was $status, not 1"
    case "$out" in
        *"$want"*) ;;
        *) say_fail "$file: expected '$want', got: $out" ;;
    esac
    # The same complaint, from the compiler being replaced.
    ref=$("$dreamc" "$tmp/pkg/$file" --no-emit -L mind 2>&1)
    case "$ref" in
        *"$want"*) ;;
        *) say_fail "$file: dreamc does not say '$want' -- the check is wrong, not the code" ;;
    esac
}

expect unbound.dr  "cannot find \`nosuchname\` in this scope"
expect impure.dr   "cannot use the impure member \`print!\` inside the pure function \`shout\`"
expect member.dr   "module \`list\` has no member \`no_such_member\`"
expect modvalue.dr "\`list\` is a module, not a value"
expect duplet.dr   "\`a\` is already bound in this module"
expect shallow.dr  "\`std\` has no module \`map\`"
expect clash.dr    "\`map\` is already bound in this module"
expect nofill.dr   "does not implement \`area\`"

# A virtual with a default needs no implementation, and an override is fine.
cat > "$tmp/pkg/defaulted.dr" <<'EOF'
virtual let area s = s * 2;
let describe s = area s;
EOF
cat > "$tmp/pkg/usesdefault.dr" <<'EOF'
derive defaulted;
let main! = describe 3;
EOF
cat > "$tmp/pkg/overrides.dr" <<'EOF'
derive defaulted;
let area s = s * 10;
let main! = describe 3;
EOF
for ok in usesdefault.dr overrides.dr; do
    "$dream" "$image" --check -L mind "$tmp/pkg/$ok" >/dev/null 2>&1 \
        || say_fail "$ok should have been accepted"
done

[ "$fail" -eq 0 ] && echo "resolution and purity agree with dreamc, program by program"
exit "$fail"
