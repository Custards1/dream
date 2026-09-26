#!/bin/sh
# std.ffi and std.foreign against a C library built for them.
#
# The library is compiled here, from `sample.c`, and carried in the test's
# image as a payload -- so this checks the embedding too, and needs nothing
# installed but a C compiler. The program runs under the interpreter and under
# the JIT, and both must print exactly `ffi.expected`: a callback is Dream
# running underneath C, which is precisely the kind of re-entry the two tiers
# could disagree about.
#
# A VM built without libffi, or a machine with no C compiler, skips rather than
# fails: both are supported configurations, and `std.ffi` says so at the call.
set -u

dream=${dream:-build-dream/bin/dream}
dreams=${dreams:-build/dreams.dream}
here=$(dirname "$0")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cc=${CC:-cc}
if ! command -v "$cc" >/dev/null 2>&1; then
    echo "skip ffi: no C compiler"
    exit 0
fi

cat >"$tmp/probe.dr" <<'EOF'
import std.console;
import std.vm;
let main! = console.print! (vm.has_ffi ());
EOF
timeout 60 "$dream" "$dreams" -L mind -o "$tmp/probe.dream" "$tmp/probe.dr" >/dev/null 2>&1 || exit 1
if [ "$(timeout 10 "$dream" "$tmp/probe.dream")" != "true" ]; then
    echo "skip ffi: this VM was built without libffi"
    exit 0
fi

"$cc" -shared -fPIC -O2 -o "$tmp/libsample.so" "$here/sample.c" || exit 1
timeout 120 "$dream" "$dreams" -L mind --payload "sample=$tmp/libsample.so" \
    -o "$tmp/ffi.dream" "$here/ffi.dr" >"$tmp/build" 2>&1 || { cat "$tmp/build"; exit 1; }

fail=0
for tier in "" "--no-jit"; do
    # shellcheck disable=SC2086
    timeout 20 "$dream" $tier "$tmp/ffi.dream" >"$tmp/out" 2>&1
    if ! diff -u "$here/ffi.expected" "$tmp/out" >"$tmp/diff"; then
        echo "FAIL ffi ${tier:-(jit)}"
        head -40 "$tmp/diff"
        fail=1
    fi
done
[ "$fail" = 0 ] && echo "ffi: ok"
exit "$fail"
