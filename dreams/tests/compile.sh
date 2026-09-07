#!/bin/sh
# The end of the pipeline: programs compiled by `dreams`, run by the VM.
#
# Every other test in this directory checks that a stage agrees with something
# -- the reference compiler, or a recorded shape. This one checks the only thing
# that finally matters: that a program `dreams` compiled does what the program
# says. The examples come with their output recorded beside them, so the
# comparison is against what the language is documented to do rather than
# against another implementation of it.
set -u

dreamc=${dreamc:-target/debug/dreamc}
dream=${dream:-build-dream/bin/dream}
image=${image:-/tmp/dreams-compile.dream}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fail=0
pass=0

"$dreamc" dreams/main.dr -L mind -L . -o "$image" >/dev/null || exit 1

for src in examples/*.dr; do
    [ -f "$src" ] || continue
    expected="${src%.dr}.expected"
    [ -f "$expected" ] || continue
    name=$(basename "$src" .dr)

    if ! "$dream" "$image" -o "$tmp/out.dream" -L mind -L examples "$src" >"$tmp/build" 2>&1; then
        echo "FAIL $name (did not compile)"
        sed 's/^/    /' "$tmp/build" | head -4
        fail=$((fail + 1))
        continue
    fi
    if ! "$dream" "$tmp/out.dream" >"$tmp/run" 2>&1; then
        echo "FAIL $name (did not run)"
        sed 's/^/    /' "$tmp/run" | head -4
        fail=$((fail + 1))
        continue
    fi
    if ! diff -q "$expected" "$tmp/run" >/dev/null 2>&1; then
        echo "FAIL $name (output)"
        diff "$expected" "$tmp/run" | sed 's/^/    /' | head -6
        fail=$((fail + 1))
        continue
    fi
    echo "ok   $name"
    pass=$((pass + 1))
done

echo
echo "$pass compiled and ran correctly, $fail failed"
[ "$fail" -eq 0 ]
