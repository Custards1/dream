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

dream="${dream:-}"
if [[ -z "$dream" ]]; then
  for c in "$ROOT/build-dream/bin/dream" "$ROOT/build/bin/dream"; do
    [[ -x "$c" ]] && dream="$c" && break
  done
fi
if [[ ! -x "${dream:-}" ]]; then
  echo "examples: cannot find dream; run \`just vm\` or set dream" >&2
  exit 1
fi

# The compiler is `dreams`, which is an image rather than a native program: a
# freshly built one if there is one, and otherwise the checked-in seed, which
# needs nothing but the VM. `$DREAMS` overrides the choice, and a name that
# does not end in `.dream` is run directly -- that is how a native compiler is
# still usable here without this script knowing anything about which one it is.
DREAMS="${DREAMS:-}"
if [[ -z "$DREAMS" ]]; then
  for c in "$ROOT/build/dreams.dream" "$ROOT/dreams/bootstrap/dreams.dream"; do
    [[ -f "$c" ]] && DREAMS="$c" && break
  done
fi
if [[ -z "$DREAMS" ]]; then
  echo "examples: cannot find a compiler; run \`just dreams\` or set DREAMS" >&2
  exit 1
fi
if [[ "$DREAMS" == *.dream ]]; then compile=("$dream" "$DREAMS"); else compile=("$DREAMS"); fi

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

  if ! "${compile[@]}" "$src" "${LIBS[@]}" -o "$image" >"$WORK/$name.compile" 2>&1; then
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

  if ! "$dream" "$image" >"$WORK/$name.out" 2>&1; then
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
