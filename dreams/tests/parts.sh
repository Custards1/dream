#!/bin/sh
# A program resolved, lowered and checked in parts (`scope.resolve_parts!`,
# `lower.link_parallel!`, `compile.analyze_parallel!`) must be the program
# resolved whole: the same diagnostics, in the same order, and code that does
# the same thing. `--check` never builds, so it is always the whole walk, and
# `--no-opt` lowers the arena whole -- which is exactly the pair to hold the
# parts to. See "resolving in parts" in dreams/scope.dr.
set -u

dream=${dream:-build-dream/bin/dream}
image=${image:-build/dreams.dream}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0

for name in names types; do
    src=dreams/tests/parts/$name.dr
    "$dream" "$image" -L mind -o "$tmp/out.dream" "$src" >"$tmp/built" 2>&1
    "$dream" "$image" --check -L mind "$src" >"$tmp/checked" 2>&1
    if [ ! -s "$tmp/built" ] || ! diff -q "$tmp/built" "$tmp/checked" >/dev/null; then
        echo "FAIL $name: a build in parts does not say what --check says"
        diff "$tmp/checked" "$tmp/built" | sed 's/^/    /' | head -8
        fail=1
    else
        echo "ok   $name: $(wc -l <"$tmp/built") diagnostics, the same in parts and whole"
    fi
done

src=dreams/tests/parts/program.dr
if "$dream" "$image" -L mind -o "$tmp/parts.dream" "$src" >"$tmp/b1" 2>&1 &&
   "$dream" "$image" --no-opt -L mind -o "$tmp/whole.dream" "$src" >"$tmp/b2" 2>&1 &&
   "$dream" "$tmp/parts.dream" >"$tmp/r1" 2>&1 && "$dream" "$tmp/whole.dream" >"$tmp/r2" 2>&1 &&
   [ -s "$tmp/r1" ] && diff -q "$tmp/r1" "$tmp/r2" >/dev/null; then
    echo "ok   program: the same output built in parts and whole"
else
    echo "FAIL program: built in parts and whole, it does not do the same thing"
    cat "$tmp/b1" "$tmp/b2" "$tmp/r1" "$tmp/r2" | sed 's/^/    /' | head -12
    fail=1
fi

[ "$fail" -eq 0 ]
