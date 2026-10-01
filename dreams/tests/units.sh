#!/bin/sh
# Compile units: a build that reads its parts back from `--units` must write
# the image a build that computed them writes. Each case compiles a program
# with a unit cache and without one and compares the two images, byte for
# byte: cold, warm, and after each kind of edit a unit's key has to notice --
# a body, a declaration added, a name another module uses renamed, a wrapper
# changed. A key that missed any of them would read back a stale part, and the
# two images would differ.
set -u

dream=${dream:-build-dream/bin/dream}
image=${image:-build/dreams.dream}
root=$(pwd)
case "$dream" in /*) ;; *) dream="$root/$dream" ;; esac
case "$image" in /*) ;; *) image="$root/$image" ;; esac
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0
pass=0

mkdir -p "$tmp/p"
cat >"$tmp/p/shapes.dr" <<'EOF'
let area w h = w * h;
let double x = area x 2;
let name = "shapes";
EOF
cat >"$tmp/p/util.dr" <<'EOF'
import shapes;
let label n = shapes.name + ":" + to_string n;
let twice n = shapes.double n;
EOF
cat >"$tmp/p/main.dr" <<'EOF'
import std.console;
import std.list;
import shapes;
import util;
let main! = console.print! (util.label (list.sum (list.map util.twice [1, 2, 3])) + " " + to_string (shapes.area 3 4));
EOF

# same NAME -- compile with the cache and without, run both, compare.
same() {
    ( cd "$tmp/p" && "$dream" "$image" -L "$root/mind" --units "$tmp/units" -o "$tmp/with.dream" main.dr >"$tmp/log1" 2>&1 )
    s1=$?
    ( cd "$tmp/p" && "$dream" "$image" -L "$root/mind" -o "$tmp/without.dream" main.dr >"$tmp/log2" 2>&1 )
    s2=$?
    if [ "$s1" = 0 ] && [ "$s2" = 0 ] && cmp -s "$tmp/with.dream" "$tmp/without.dream"; then
        echo "ok   $1: $("$dream" "$tmp/with.dream")"; pass=$((pass + 1))
    else
        echo "FAIL $1: the image built from units is not the image built without"
        cat "$tmp/log1" "$tmp/log2" | head -6 | sed 's/^/    /'
        fail=$((fail + 1))
    fi
}

same "a cold cache"
same "a warm one"
# made KIND -- how many units of that kind the cache holds.
made() { ls "$tmp/units" | grep -c "^$1-"; }
walks=$(made walk); checks=$(made check)
sed -i 's/let area w h = w \* h;/let area w h = h * w + 0;/' "$tmp/p/shapes.dr"
same "a body edited in one module"
# One module walked and checked again, and no other. Lowering is allowed
# more: `double` is a wrapper whose literal the edit moved, and a wrapper's
# callers are lowered against it (see `lower.reached_wrappers`).
if [ "$(made walk)" = "$((walks + 1))" ] && [ "$(made check)" = "$((checks + 1))" ]; then
    echo "ok   and only that module's part was walked and checked again"; pass=$((pass + 1))
else
    echo "FAIL an edit to one body made $(( $(made walk) - walks )) walks and $(( $(made check) - checks )) checks"
    fail=$((fail + 1))
fi
printf 'let extra = 1;\n' >>"$tmp/p/shapes.dr"
same "a declaration added"
# A module is seen through its submodules too: `util` names `shapes.extra.k`,
# and a declaration put in front of `k` moves the global `util` was walked
# against.
printf 'mod extra { let k = 3; }\n' >>"$tmp/p/shapes.dr"
printf 'let deep = shapes.extra.k;\n' >>"$tmp/p/util.dr"
same "a submodule another module reaches into"
sed -i 's/mod extra { let k = 3; }/mod extra { let before = 0; let k = 3; }/' "$tmp/p/shapes.dr"
same "a declaration added to it"
# A declaration walks again the parts that can see it and no others: `util`
# and `main`, which imports it, and not `shapes` or the standard library.
walks=$(made walk)
printf 'let unused n = n;\n' >>"$tmp/p/util.dr"
same "a declaration added to a module one other imports"
if [ "$(made walk)" = "$((walks + 2))" ]; then
    echo "ok   and only the two parts that can see it were walked again"; pass=$((pass + 1))
else
    echo "FAIL a declaration in util walked $(( $(made walk) - walks )) parts again, not 2"; fail=$((fail + 1))
fi
sed -i 's/let name = "shapes";/let title = "shapes";\nlet name = title;/' "$tmp/p/shapes.dr"
same "a wrapper introduced where a value was"
sed -i 's/shapes.name/shapes.title/' "$tmp/p/util.dr"
same "a name another module uses changed"

# A part's check is kept as well, and what it says of a body depends on
# signatures written elsewhere: here a signature added to `shapes` makes a body
# of `util` wrong without a character of `util` changing. A check read back
# from before it would say nothing.
printf 'let loose = shapes.double "x";\n' >>"$tmp/p/util.dr"
same "a body no signature constrains"
printf 'let double : :integer -> :integer;\n' >>"$tmp/p/shapes.dr"
refused() {
    ( cd "$tmp/p" && "$dream" "$image" -L "$root/mind" --units "$tmp/units" -o "$tmp/with.dream" main.dr >"$tmp/log1" 2>&1 )
    s1=$?
    ( cd "$tmp/p" && "$dream" "$image" -L "$root/mind" -o "$tmp/without.dream" main.dr >"$tmp/log2" 2>&1 )
    s2=$?
    if [ "$s1" != 0 ] && [ "$s2" != 0 ] && cmp -s "$tmp/log1" "$tmp/log2" && grep -q "$2" "$tmp/log1"; then
        echo "ok   $1"; pass=$((pass + 1))
    else
        echo "FAIL $1: with units $s1, without $s2"
        cat "$tmp/log1" "$tmp/log2" | head -8 | sed 's/^/    /'
        fail=$((fail + 1))
    fi
}
refused "a signature elsewhere that makes it wrong" "util.dr"
sed -i '/let loose/d' "$tmp/p/util.dr"
same "and the body taken away again"

# A file's parse is kept by its text, and a parse that failed is kept as well:
# read back, it must fail the way it did, twice over.
printf 'let broken = (1 +;\n' >>"$tmp/p/shapes.dr"
refused "a syntax error" "shapes.dr"
refused "the same syntax error, its parse read back" "shapes.dr"
sed -i '/let broken/d' "$tmp/p/shapes.dr"
same "and mended"
# A unit read back is written again once it is a day old, which is what lets
# `mind` tell a unit in use from one nothing asks for any more.
for f in "$tmp/units"/*; do touch -d '2 days ago' "$f"; done
same "a warm build over units two days old"
if [ -n "$(find "$tmp/units" -type f -newermt '1 hour ago' | head -1)" ]; then
    echo "ok   and the units it read were written again"; pass=$((pass + 1))
else
    echo "FAIL no unit read back was written again"; fail=$((fail + 1))
fi
if [ "$(made parse)" -gt 0 ]; then
    echo "ok   parses were kept"; pass=$((pass + 1))
else
    echo "FAIL no parse was kept"; fail=$((fail + 1))
fi

# The compiler itself, which is the program with the most parts there is.
( "$dream" "$image" -L "$root/mind" -L "$root" --units "$tmp/self" -o "$tmp/self1.dream" "$root/dreams/main.dr" >/dev/null 2>&1 &&
  "$dream" "$image" -L "$root/mind" -L "$root" --units "$tmp/self" -o "$tmp/self2.dream" "$root/dreams/main.dr" >/dev/null 2>&1 &&
  "$dream" "$image" -L "$root/mind" -L "$root" -o "$tmp/self3.dream" "$root/dreams/main.dr" >/dev/null 2>&1 )
if cmp -s "$tmp/self1.dream" "$tmp/self2.dream" && cmp -s "$tmp/self1.dream" "$tmp/self3.dream"; then
    echo "ok   the compiler, cold, warm and without units"; pass=$((pass + 1))
else
    echo "FAIL the compiler built from units is not the compiler built without"; fail=$((fail + 1))
fi

echo
echo "$pass unit cases passed, $fail failed"
[ "$fail" -eq 0 ]
