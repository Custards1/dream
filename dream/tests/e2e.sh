#!/usr/bin/env bash
# End-to-end tests: compile each program with `dreams`, run it under the VM, and
# compare against the recorded output.
#
# Every program runs twice, once with the JIT and once without. Both tiers must
# agree exactly -- a JIT that changes what a program prints is a broken JIT,
# and for a lazy language that is an easy mistake to make.

set -uo pipefail

# `DREAM` is the conventional spelling; the lowercase `dream` is accepted too
# because it is what this script used first, and a lowercase environment
# variable is easy to set by accident from a shell where `dream` is also a
# path or an alias.
dream="${DREAM:-${dream:-}}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

if [[ -z "$dream" ]]; then
    for c in "$ROOT/build-dream/bin/dream" "$HERE/../build-dream/bin/dream" \
           "$ROOT/build/bin/dream"; do
    [[ -x "$c" ]] && dream="$c" && break
  done
fi

if [[ ! -x "${dream:-}" ]]; then
  echo "e2e: cannot find the dream VM; build it or set dream" >&2
  exit 1
fi

# The compiler is `dreams`, an image rather than a native program: a freshly
# built one when there is one, and otherwise the checked-in seed, which needs
# nothing but the VM this script is testing. `$DREAMS` overrides the choice,
# and a name that does not end in `.dream` is executed directly.
DREAMS="${DREAMS:-}"
if [[ -z "$DREAMS" ]]; then
  for c in "$ROOT/build/dreams.dream" "$ROOT/dreams/bootstrap/dreams.dream"; do
    [[ -f "$c" ]] && DREAMS="$c" && break
  done
fi
if [[ -z "$DREAMS" ]]; then
  echo "e2e: cannot find a compiler; run \`just dreams\` or set DREAMS" >&2
  exit 1
fi
if [[ "$DREAMS" == *.dream ]]; then compile=("$dream" "$DREAMS"); else compile=("$DREAMS"); fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

pass=0
fail=0

for src in "$HERE"/programs/*.dr; do
  name="$(basename "$src" .dr)"
  expected="$HERE/programs/$name.expected"
  image="$WORK/$name.dream"

  if ! "${compile[@]}" "$src" -o "$image" >"$WORK/$name.compile" 2>&1; then
    echo "FAIL $name (compile)"
    sed 's/^/    /' "$WORK/$name.compile"
    fail=$((fail + 1))
    continue
  fi

  # A program may set VM environment variables in `<name>.env`, one
  # `NAME=value` per line. It is how a test that has to trip a runtime limit
  # asks for a small one, rather than spending the seconds it would take to
  # reach the real one.
  env_args=()
  if [[ -f "$HERE/programs/$name.env" ]]; then
    while IFS= read -r line; do
      [[ -z "$line" || "$line" == \#* ]] && continue
      env_args+=("$line")
    done < "$HERE/programs/$name.env"
  fi

  jit_out="$(env "${env_args[@]}" "$dream" "$image" 2>&1)"
  jit_rc=$?
  int_out="$(env "${env_args[@]}" "$dream" --no-jit "$image" 2>&1)"
  int_rc=$?

  if [[ "$jit_out" != "$int_out" || "$jit_rc" != "$int_rc" ]]; then
    echo "FAIL $name (the two tiers disagree)"
    diff <(printf '%s\n' "$int_out") <(printf '%s\n' "$jit_out") | sed 's/^/    /'
    fail=$((fail + 1))
    continue
  fi

  if ! diff -q <(printf '%s\n' "$jit_out") "$expected" >/dev/null; then
    echo "FAIL $name (output)"
    diff "$expected" <(printf '%s\n' "$jit_out") | sed 's/^/    /'
    fail=$((fail + 1))
    continue
  fi

  echo "ok   $name"
  pass=$((pass + 1))
done

# --- shebang images ---------------------------------------------------------
#
# `--shebang` writes an interpreter line before the image and sets the
# execute bit. The VM skips such a line, so the file has to work both ways: run
# as a command, and loaded as an ordinary image. Testing only the first would
# miss an image the VM can no longer read.

shebang_src="$(mktemp -d)"
trap 'rm -rf "$shebang_src"' EXIT
cat > "$shebang_src/hello.dr" <<'EOF'
import std.console;
let main! = console.print! "shebang";
EOF

if "${compile[@]}" "$shebang_src/hello.dr" --shebang "$(cd "$(dirname "$dream")" && pwd)/$(basename "$dream")" \
     -o "$shebang_src/hello" >/dev/null 2>&1; then
  direct_out="$("$shebang_src/hello" 2>&1)"
  loaded_out="$("$dream" "$shebang_src/hello" 2>&1)"
  first_line="$(head -c 2 "$shebang_src/hello")"
  if [[ ! -x "$shebang_src/hello" ]]; then
    echo "FAIL shebang (the image was not made executable)"
    fail=$((fail + 1))
  elif [[ "$first_line" != "#!" ]]; then
    echo "FAIL shebang (no interpreter line was written)"
    fail=$((fail + 1))
  elif [[ "$direct_out" != "shebang" ]]; then
    echo "FAIL shebang (running it directly printed: $direct_out)"
    fail=$((fail + 1))
  elif [[ "$loaded_out" != "shebang" ]]; then
    echo "FAIL shebang (loading it as an image printed: $loaded_out)"
    fail=$((fail + 1))
  else
    echo "ok   shebang"
    pass=$((pass + 1))
  fi
else
  echo "FAIL shebang (could not compile with --shebang)"
  fail=$((fail + 1))
fi

# --- payloads ---------------------------------------------------------------
#
# `--payload` puts a file's bytes in the image past everything the rest of the
# container can address, and `core.data_at` hands them back as a view rather
# than a string. Its own block rather than a program in the loop above, because
# it is the compile that differs: the flag, and the files it names.
#
# What this is really checking is that nothing copies. There is no way to assert
# that from inside the language, so it is checked from the outside instead: the
# bytes must survive the round trip through the image exactly, including the
# ones that are not valid UTF-8 and would not survive being decoded.

payload_dir="$(mktemp -d)"
trap 'rm -rf "$WORK" "$shebang_src" "$payload_dir"' EXIT

printf 'the quick brown fox' > "$payload_dir/a.bin"
# Deliberately not text: a byte no decoder would let through unchanged.
printf 'head\x00\x80\xfftail' > "$payload_dir/b.bin"

cat > "$payload_dir/payload.dr" <<'EOF'
// `std.core` and no more: the programs here are compiled without a package
// path, so `std.str` -- which is Dream source in `mind/std` -- is not reachable.
// `str.byte` and `str.slice` are one-line wrappers over these two anyway.
import std.console;
import std.core;
import std.io;

let main! = {
    let a = core.data_at 0;
    let b = core.data_at 1;
    console.print! ("count " + to_string (core.data_count ()))
    console.print! ("type " + to_string (type_of a))
    console.print! ("lens " + to_string (len a) + " " + to_string (len b))
    console.print! ("byte " + to_string (core.str_byte a 4))
    console.print! ("slice " + to_string (core.str_slice a 4 5 == "quick"))
    console.print! ("whole " + to_string (a == "the quick brown fox"))
    console.print! ("order " + to_string (b < a))
    // A slice is a view too, so slicing one again must land in the same place.
    console.print! ("again " + to_string (core.str_slice (core.str_slice a 4 9) 0 5 == "quick"))
    // The bytes cross a process boundary as a view, not as a copy.
    console.print! ("child " + to_string (join! (spawn! $( len b ))))
    // Concatenating one is refused, and the message says what to do instead.
    console.print! ("refused " + to_string (type_of (try! { a + "x" } catch e { e })))
    io.write! (io.stdout! ()) b
};
EOF

{
  printf 'count 2\ntype :bigstr\nlens 19 11\nbyte 113\nslice true\nwhole true\n'
  printf 'order true\nagain true\nchild 11\nrefused :error\n'
  cat "$payload_dir/b.bin"
} > "$payload_dir/expected"

if "${compile[@]}" "$payload_dir/payload.dr" \
     --payload "$payload_dir/a.bin" --payload "$payload_dir/b.bin" \
     -o "$payload_dir/payload.dream" >"$payload_dir/compile" 2>&1; then
  # Through files rather than `$(...)`: the payload this program writes out
  # contains a NUL, and a command substitution drops it -- which would quietly
  # turn the one check that proves the bytes are untouched into a check that
  # they are untouched apart from the interesting one.
  "$dream" "$payload_dir/payload.dream" >"$payload_dir/jit.out" 2>&1
  "$dream" --no-jit "$payload_dir/payload.dream" >"$payload_dir/int.out" 2>&1
  # The payload is the last thing in the file, unpadded, in the order given.
  tail_bytes="$(tail -c 30 "$payload_dir/payload.dream" | od -An -tx1 | tr -d ' \n')"
  want_bytes="$(cat "$payload_dir/a.bin" "$payload_dir/b.bin" | od -An -tx1 | tr -d ' \n')"
  if ! cmp -s "$payload_dir/jit.out" "$payload_dir/int.out"; then
    echo "FAIL payload (the two tiers disagree)"
    diff <(od -c "$payload_dir/int.out") <(od -c "$payload_dir/jit.out") | sed 's/^/    /'
    fail=$((fail + 1))
  elif ! cmp -s "$payload_dir/jit.out" "$payload_dir/expected"; then
    echo "FAIL payload (output)"
    diff <(od -c "$payload_dir/expected") <(od -c "$payload_dir/jit.out") | sed 's/^/    /'
    fail=$((fail + 1))
  elif [[ "$tail_bytes" != "$want_bytes" ]]; then
    echo "FAIL payload (the image does not end with the payload bytes)"
    echo "    want $want_bytes"
    echo "    got  $tail_bytes"
    fail=$((fail + 1))
  else
    echo "ok   payload"
    pass=$((pass + 1))
  fi
else
  echo "FAIL payload (could not compile with --payload)"
  sed 's/^/    /' "$payload_dir/compile"
  fail=$((fail + 1))
fi

# A build with no payload must write the bytes it wrote before the feature
# existed. That is what the bootstrap's byte-equality rests on, so it is worth
# asserting directly rather than only noticing when the seed stops reproducing.
"${compile[@]}" "$shebang_src/hello.dr" -o "$payload_dir/plain1.dream" >/dev/null 2>&1
"${compile[@]}" "$shebang_src/hello.dr" -o "$payload_dir/plain2.dream" >/dev/null 2>&1
if cmp -s "$payload_dir/plain1.dream" "$payload_dir/plain2.dream" &&
   ! grep -qa 'PAYL' "$payload_dir/plain1.dream"; then
  echo "ok   no-payload build carries no payload sections"
  pass=$((pass + 1))
else
  echo "FAIL no-payload build (a PAYL section appeared, or the build is not deterministic)"
  fail=$((fail + 1))
fi

echo
echo "$pass passed, $fail failed"
[[ $fail -eq 0 ]]
