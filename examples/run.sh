#!/usr/bin/env bash
# Compile and run every example, and check the output against what is recorded
# next to it in `<name>.expected`.
#
# Single-file examples are `examples/NN_name.dr`. A package is any directory
# with a `mind.toml`; its entry point is `main.dr` and its expected output is
# `<dir>.expected` beside the directory.
#
#   examples/run.sh            check every example
#   examples/run.sh --bless    record current output as expected
#   examples/run.sh basics     only examples whose name matches

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

DREAMC="${DREAMC:-}"
MINDV2="${MINDV2:-}"
# Take the most recently built compiler, not the release one: preferring
# release would silently check the examples against a stale binary, which is
# exactly the sort of bug these examples exist to catch.
if [[ -z "$DREAMC" ]]; then
  newest=""
  for c in "$ROOT/target/release/dreamc" "$ROOT/target/debug/dreamc"; do
    [[ -x "$c" ]] || continue
    if [[ -z "$newest" || "$c" -nt "$newest" ]]; then newest="$c"; fi
  done
  DREAMC="$newest"
fi
if [[ -z "$MINDV2" ]]; then
  for c in "$ROOT/build-mindv2/bin/mindv2" "$ROOT/build/bin/mindv2"; do
    [[ -x "$c" ]] && MINDV2="$c" && break
  done
fi
if [[ ! -x "${DREAMC:-}" ]]; then
  echo "examples: cannot find dreamc; run \`just build\` or set DREAMC" >&2
  exit 1
fi
if [[ ! -x "${MINDV2:-}" ]]; then
  echo "examples: cannot find mindv2; run \`just build\` or set MINDV2" >&2
  exit 1
fi

bless=0
filter=""
for arg in "$@"; do
  case "$arg" in
    --bless) bless=1 ;;
    *) filter="$arg" ;;
  esac
done

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# The standard library lives in mind/, the example packages in examples/.
LIBS=(-L "$ROOT/mind" -L "$HERE")

pass=0
fail=0

# check <name> <source> <expected>
check() {
  local name="$1" src="$2" expected="$3"
  local image="$WORK/$name.dream"

  if ! "$DREAMC" "$src" "${LIBS[@]}" -o "$image" >"$WORK/$name.compile" 2>&1; then
    echo "FAIL $name (compile)"
    sed 's/^/    /' "$WORK/$name.compile"
    fail=$((fail + 1))
    return
  fi
  # A warning is a failure here: the examples are meant to be clean.
  if grep -q '^warning:' "$WORK/$name.compile"; then
    echo "FAIL $name (compiled with warnings)"
    sed 's/^/    /' "$WORK/$name.compile"
    fail=$((fail + 1))
    return
  fi

  if ! "$MINDV2" "$image" >"$WORK/$name.out" 2>&1; then
    echo "FAIL $name (run)"
    sed 's/^/    /' "$WORK/$name.out"
    fail=$((fail + 1))
    return
  fi

  if (( bless )); then
    cp "$WORK/$name.out" "$expected"
    echo "bless $name"
    pass=$((pass + 1))
    return
  fi

  if [[ ! -f "$expected" ]]; then
    echo "FAIL $name (no $expected; run with --bless to record it)"
    fail=$((fail + 1))
    return
  fi
  if ! diff -u "$expected" "$WORK/$name.out" >"$WORK/$name.diff"; then
    echo "FAIL $name (output changed)"
    sed 's/^/    /' "$WORK/$name.diff"
    fail=$((fail + 1))
    return
  fi

  echo "ok   $name"
  pass=$((pass + 1))
}

# Single-file examples.
for src in "$HERE"/*.dr; do
  [[ -e "$src" ]] || continue
  name="$(basename "$src" .dr)"
  [[ -z "$filter" || "$name" == *"$filter"* ]] || continue
  check "$name" "$src" "$HERE/$name.expected"
done

# Packages: any directory with a manifest, entered through main.dr.
for manifest in "$HERE"/*/mind.toml; do
  [[ -e "$manifest" ]] || continue
  dir="$(dirname "$manifest")"
  name="$(basename "$dir")"
  [[ -z "$filter" || "$name" == *"$filter"* ]] || continue
  if [[ ! -f "$dir/main.dr" ]]; then
    echo "skip $name (no main.dr)"
    continue
  fi
  check "$name" "$dir/main.dr" "$HERE/$name.expected"
done

echo
echo "$pass passed, $fail failed"
[[ $fail -eq 0 ]]
