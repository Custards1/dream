#!/usr/bin/env bash
# End-to-end tests: compile each program with dawnc, run it under the VM, and
# compare against the recorded output.
#
# Every program runs twice, once with the JIT and once without. Both tiers must
# agree exactly -- a JIT that changes what a program prints is a broken JIT,
# and for a lazy language that is an easy mistake to make.

set -uo pipefail

DREAMC="${DREAMC:-}"
MINDV2="${MINDV2:-}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ -z "$DREAMC" ]]; then
  # Cargo puts workspace artifacts in the workspace root's target directory,
  # not in the crate's own. Take the most recently built one: preferring
  # release would silently test a stale binary.
  newest=""
  for c in "$HERE/../../target/release/dreamc" "$HERE/../../target/debug/dreamc" \
           "$HERE/../../dreamc/target/release/dreamc" "$HERE/../../dreamc/target/debug/dreamc"; do
    [[ -x "$c" ]] || continue
    if [[ -z "$newest" || "$c" -nt "$newest" ]]; then newest="$c"; fi
  done
  DREAMC="$newest"
fi
if [[ -z "$MINDV2" ]]; then
    for c in "$HERE/../../build-mindv2/bin/mindv2" "$HERE/../build-mindv2/bin/mindv2" \
           "$HERE/../build/bin/mindv2"; do
    [[ -x "$c" ]] && MINDV2="$c" && break
  done
fi

if [[ ! -x "${DREAMC:-}" ]]; then
  echo "e2e: cannot find dawnc; build it or set DREAMC" >&2
  exit 1
fi
if [[ ! -x "${MINDV2:-}" ]]; then
  echo "e2e: cannot find the mindv2 VM; build it or set MINDV2" >&2
  exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

pass=0
fail=0

for src in "$HERE"/programs/*.dr; do
  name="$(basename "$src" .dr)"
  expected="$HERE/programs/$name.expected"
  image="$WORK/$name.dream"

  if ! "$DREAMC" "$src" -o "$image" >"$WORK/$name.compile" 2>&1; then
    echo "FAIL $name (compile)"
    sed 's/^/    /' "$WORK/$name.compile"
    fail=$((fail + 1))
    continue
  fi

  jit_out="$("$MINDV2" "$image" 2>&1)"
  jit_rc=$?
  int_out="$("$MINDV2" --no-jit "$image" 2>&1)"
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

echo
echo "$pass passed, $fail failed"
[[ $fail -eq 0 ]]
