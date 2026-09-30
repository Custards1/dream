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
kept=$(ls "$tmp/units" | wc -l)
sed -i 's/let area w h = w \* h;/let area w h = h * w + 0;/' "$tmp/p/shapes.dr"
same "a body edited in one module"
if [ "$(ls "$tmp/units" | wc -l)" -lt "$((kept + 5))" ]; then
    echo "ok   and only that module's parts were made again"; pass=$((pass + 1))
else
    echo "FAIL an edit to one body made $(( $(ls "$tmp/units" | wc -l) - kept )) new units"; fail=$((fail + 1))
fi
printf 'let extra = 1;\n' >>"$tmp/p/shapes.dr"
same "a declaration added"
sed -i 's/let name = "shapes";/let title = "shapes";\nlet name = title;/' "$tmp/p/shapes.dr"
same "a wrapper introduced where a value was"
sed -i 's/shapes.name/shapes.title/' "$tmp/p/util.dr"
same "a name another module uses changed"

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
