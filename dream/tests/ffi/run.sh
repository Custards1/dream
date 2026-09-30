#!/bin/sh
# std.ffi and std.foreign against a C library built for them.
#
# This directory is a package, and its `build.dr` makes the library from
# `sample.c` with `std.build.cc` and embeds it as a payload -- so this checks
# `mind` running a build script and the embedding both, and needs nothing
# installed but a C compiler. It is built from a copy, so that nothing is
# written into the source tree. The program runs under the interpreter and
# under the JIT, and both must print exactly `ffi.expected`: a callback is
# Dream running underneath C, which is precisely the kind of re-entry the two
# tiers could disagree about.
#
# A machine with no C compiler skips rather than fails, since the library
# cannot be built. There is no VM without libffi to skip for: `std.ffi` is a
# required part of the VM, and the build refuses to make one that lacks it.
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

root=$(cd "$here/../../.." && pwd)
case "$dream" in /*) ;; *) dream="$root/$dream" ;; esac
case "$dreams" in /*) ;; *) dreams="$root/$dreams" ;; esac
mkdir -p "$tmp/pkg"
cp "$here/mind.toml" "$here/build.dr" "$here/ffi.dr" "$here/sample.c" "$tmp/pkg/"
timeout 300 "$dream" "$dreams" -L "$root/mind/std" "$root/mind/tool/main.dr" -o "$tmp/mind.dream" >"$tmp/build" 2>&1 \
    || { cat "$tmp/build"; exit 1; }
(cd "$tmp/pkg" && CC="$cc" DREAM="$dream" MIND_STDLIB="$root/mind" MIND_HOME="$tmp/home" \
    timeout 300 "$dream" "$tmp/mind.dream" build --compiler "$dreams" -o "$tmp/ffi.dream") >"$tmp/build" 2>&1 \
    || { cat "$tmp/build"; exit 1; }

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
