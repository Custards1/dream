#!/bin/sh
# Where an image may run: what the compiler records in the header, and what
# the VM does with it.
#
# Each case is one compile and one run, checked by exit status and by the
# first line of what was printed. The machine this runs on is whichever it
# is, so the cases that must be refused are aimed at the architecture this
# machine is not -- and at Windows through a payload that looks like a DLL,
# since the script itself needs a shell.
set -u

dream=${dream:-build-dream/bin/dream}
image=${image:-build/dreams.dream}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fail=0
pass=0

case "$(uname -m)" in
    x86_64|amd64) other=aarch64 ;;
    *) other=x86_64 ;;
esac

cat >"$tmp/hello.dr" <<'DR'
import std.console;
let main! = console.print! "hello";
DR
cat >"$tmp/pinned.dr" <<'DR'
import std.console;
when os == "windows" { let where = "windows"; }
when not (os == "windows") { let where = "elsewhere"; }
let main! = console.print! where;
DR
# The smallest file the compiler takes for a Windows library: `MZ`, the offset
# of the PE header at 0x3C, and the header with an x86-64 machine field.
printf 'MZ' >"$tmp/fake.dll"
head -c 58 /dev/zero >>"$tmp/fake.dll"
printf '\200\000\000\000' >>"$tmp/fake.dll"
head -c 64 /dev/zero >>"$tmp/fake.dll"
printf 'PE\000\000\144\206' >>"$tmp/fake.dll"
head -c 122 /dev/zero >>"$tmp/fake.dll"

# check NAME EXPECT_STATUS EXPECT_FIRST_LINE -- compile-args -- run-args
check() {
    name=$1; want_status=$2; want=$3; shift 3
    compile=""
    while [ "$1" != "--" ]; do compile="$compile $1"; shift; done
    shift
    # shellcheck disable=SC2086
    if ! "$dream" "$image" -L mind $compile -o "$tmp/out.dream" >"$tmp/build" 2>&1; then
        got_status=compile
        got=$(grep -v '^compiled' "$tmp/build" | head -1)
    else
        "$dream" "$@" "$tmp/out.dream" >"$tmp/run" 2>&1
        got_status=$?
        got=$(head -1 "$tmp/run")
    fi
    case "$got" in
        "$want"*)
            if [ "$got_status" = "$want_status" ]; then
                echo "ok   $name"; pass=$((pass + 1)); return
            fi ;;
    esac
    echo "FAIL $name: wanted $want_status \"$want\", got $got_status \"$got\""
    fail=$((fail + 1))
}

check "an image that says nothing runs anywhere" 0 "hello" \
    "$tmp/hello.dr" --
check "a Windows library pins the image to Windows" 1 "dream: $tmp/out.dream was built for windows (x86_64)" \
    "$tmp/hello.dr" --payload "lib=$tmp/fake.dll" --
check "--any-target runs it anyway" 0 "hello" \
    "$tmp/hello.dr" --payload "lib=$tmp/fake.dll" -- --any-target
check "--target any records nothing" 0 "hello" \
    "$tmp/hello.dr" --payload "lib=$tmp/fake.dll" --target any --
check "another architecture is refused" 1 "dream: $tmp/out.dream was built for $other;" \
    "$tmp/hello.dr" --target "$other" --
check "a when on os pins the image to this system" 0 "elsewhere" \
    "$tmp/pinned.dr" --
check "--target sets os for when" 0 "windows" \
    "$tmp/pinned.dr" --target windows -- --any-target
check "payloads and conditions that disagree are a compile error" compile "dreams: this program can run nowhere" \
    "$tmp/pinned.dr" --payload "lib=$tmp/fake.dll" --
check "an unknown platform is a compile error" compile "dreams: --target beos" \
    "$tmp/hello.dr" --target beos --

echo
echo "$pass target cases passed, $fail failed"
[ "$fail" -eq 0 ]
