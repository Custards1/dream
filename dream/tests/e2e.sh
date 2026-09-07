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
# nothing but the VM this script is testing. `$DREAMC` still overrides, and a
# name that does not end in `.dream` is executed directly.
DREAMC="${DREAMC:-}"
if [[ -z "$DREAMC" ]]; then
  for c in "$ROOT/build/dreams.dream" "$ROOT/dreams/bootstrap/dreams.dream"; do
    [[ -f "$c" ]] && DREAMC="$c" && break
  done
fi
if [[ -z "$DREAMC" ]]; then
  echo "e2e: cannot find a compiler; run \`just dreams\` or set DREAMC" >&2
  exit 1
fi
if [[ "$DREAMC" == *.dream ]]; then compile=("$dream" "$DREAMC"); else compile=("$DREAMC"); fi

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

echo
echo "$pass passed, $fail failed"
[[ $fail -eq 0 ]]
