#!/bin/sh
# The module loader, checked against the compiler it is replacing.
#
# Unit tests can say that a `mod` is hoisted and that a cycle is caught. What
# they cannot say is that this resolver and the Rust one agree about a real
# program -- which package a file belongs to, which file a dotted name found,
# and what order the modules come out in. That is what this checks, over every
# Dream file in the repository, because agreement on a corpus is the only
# evidence that matters for a resolver meant to replace another.
#
# Then the failures, which have no counterpart to compare against: a cycle and
# an unresolved import must be *reported*, and must not be reported as success.
set -u

dreamc=${dreamc:-target/debug/dreamc}
dream=${dream:-build-dream/bin/dream}
image=${image:-/tmp/dreams-modules.dream}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fail=0
say_fail() { echo "FAIL: $*"; fail=1; }

"$dreamc" dreams/main.dr -L mind -L . -o "$image" >/dev/null || exit 1

# --- the corpus -------------------------------------------------------------

checked=0
for f in mind/std/*.dr mind/tool/*.dr examples/*.dr examples/*/*.dr \
         dream/tests/programs/*.dr dreams/*.dr; do
    [ -f "$f" ] || continue
    want=$("$dreamc" "$f" --modules --no-emit -L mind -L . -L examples 2>/dev/null \
           | awk '{print $1}')
    # `dreamc` cannot report modules for a program it rejects for other
    # reasons; there is nothing to compare against then.
    [ -n "$want" ] || continue
    got=$("$dream" "$image" --modules -L mind -L . -L examples "$f" 2>/dev/null \
          | grep -v '^host: ' | awk '{print $1}')
    if [ "$want" != "$got" ]; then
        say_fail "$f resolves differently"
        printf '%s\n' "$want" > "$tmp/want"
        printf '%s\n' "$got" > "$tmp/got"
        diff "$tmp/want" "$tmp/got" | sed 's/^/    /'
    fi
    checked=$((checked + 1))
done
echo "$checked programs resolve to the same modules under both compilers"

# --- the failures -----------------------------------------------------------

mkdir -p "$tmp/pkg"
cat > "$tmp/pkg/mind.toml" <<'EOF'
[package]
name = "fixture"
src = "."
EOF
echo 'import b;'        > "$tmp/pkg/a.dr"
echo 'import a;'        > "$tmp/pkg/b.dr"
echo 'import nowhere;'  > "$tmp/pkg/missing.dr"
echo 'import itself;'   > "$tmp/pkg/itself.dr"

# Two files importing each other. The name on the stack is the canonical one
# (`fixture.a`) and the name written is not (`b`), so a resolver that only
# compares what was written recurses until it runs out of stack -- which is
# exactly what the Rust compiler does here. Catching it is the point.
check_reports() {
    label=$1 file=$2 expect=$3
    out=$("$dream" "$image" --modules -L mind "$file" 2>&1)
    status=$?
    case "$out" in
        *"$expect"*) ;;
        *) say_fail "$label: expected a message about '$expect', got: $out" ;;
    esac
    [ "$status" -eq 1 ] || say_fail "$label: exit status was $status, not 1"
}

check_reports "an import cycle"    "$tmp/pkg/a.dr"       "import cycle"
check_reports "a self-import"      "$tmp/pkg/itself.dr"  "import cycle"
check_reports "an unresolved import" "$tmp/pkg/missing.dr" "cannot find module"

# A program that resolves cleanly must not claim otherwise.
"$dream" "$image" --modules -L mind -L . dreams/main.dr >/dev/null 2>&1 \
    || say_fail "dreams cannot resolve its own modules"

[ "$fail" -eq 0 ] && echo "the module loader agrees with dreamc, and reports what dreamc cannot"
exit "$fail"
