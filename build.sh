#!/usr/bin/env bash
# Build Dream from a clean checkout.
#
# The aim is that this either produces a working toolchain or tells you exactly
# what is missing and what you lose without it. Two dependencies are optional
# and the build adapts to both: LLVM gives the JIT, libffi gives `std.ffi`.
# Neither is required to get a working compiler and interpreter.
#
#   ./build.sh                 build and test
#   ./build.sh --release       optimized
#   ./build.sh --no-tests      just build
#   ./build.sh --clean         start from scratch
#   ./build.sh --prefix DIR    also install there
#   ./build.sh --no-jit        skip LLVM even if present
#   ./build.sh --no-ffi        skip libffi even if present

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

BUILD_TYPE=RelWithDebInfo
BUILD_DIR=build-dream
RUN_TESTS=1
DO_CLEAN=0
PREFIX=""
ENABLE_JIT=ON
ENABLE_FFI=ON
CARGO_PROFILE=""      # empty means debug
CARGO_DIR=debug

while [[ $# -gt 0 ]]; do
  case "$1" in
    --release)   BUILD_TYPE=Release; CARGO_PROFILE=--release; CARGO_DIR=release ;;
    --debug)     BUILD_TYPE=Debug ;;
    --no-tests)  RUN_TESTS=0 ;;
    --clean)     DO_CLEAN=1 ;;
    --no-jit)    ENABLE_JIT=OFF ;;
    --no-ffi)    ENABLE_FFI=OFF ;;
    --prefix)    PREFIX="${2:?--prefix needs a directory}"; shift ;;
    --build-dir) BUILD_DIR="${2:?--build-dir needs a directory}"; shift ;;
    -h|--help)   sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "build.sh: unknown option $1" >&2; exit 2 ;;
  esac
  shift
done

# --- output helpers ---------------------------------------------------------

if [[ -t 1 ]]; then
  BOLD=$'\033[1m'; DIM=$'\033[2m'; RED=$'\033[31m'; GREEN=$'\033[32m'
  YELLOW=$'\033[33m'; RESET=$'\033[0m'
else
  BOLD=""; DIM=""; RED=""; GREEN=""; YELLOW=""; RESET=""
fi

step()  { printf '%s==>%s %s\n' "$BOLD" "$RESET" "$*"; }
ok()    { printf '    %s%s%s\n' "$GREEN" "$*" "$RESET"; }
warn()  { printf '    %s%s%s\n' "$YELLOW" "$*" "$RESET"; }
die()   { printf '%serror:%s %s\n' "$RED" "$RESET" "$*" >&2; exit 1; }
note()  { printf '    %s%s%s\n' "$DIM" "$*" "$RESET"; }

# --- prerequisites ----------------------------------------------------------

step "Checking prerequisites"

missing=0
need() {
  local tool="$1" why="$2"
  if command -v "$tool" >/dev/null 2>&1; then
    ok "$tool  $($tool --version 2>&1 | head -1)"
  else
    printf '    %smissing: %s  (%s)%s\n' "$RED" "$tool" "$why" "$RESET"
    missing=1
  fi
}

need cmake "builds the VM"
need cargo "builds the compiler"

if command -v c++ >/dev/null 2>&1 || command -v g++ >/dev/null 2>&1 || \
   command -v clang++ >/dev/null 2>&1; then
  ok "C++ compiler found"
else
  printf '    %smissing: a C++20 compiler%s\n' "$RED" "$RESET"
  missing=1
fi

[[ $missing -eq 0 ]] || die "install the tools above and run this again"

# Optional pieces. Report them now rather than letting CMake bury the news.
if [[ "$ENABLE_JIT" == ON ]]; then
  if command -v llvm-config >/dev/null 2>&1; then
    ok "LLVM $(llvm-config --version)  -- the JIT will be built"
  elif compgen -G "/nix/store/*llvm-*-dev/bin/llvm-config" >/dev/null; then
    ok "LLVM found in the Nix store -- the JIT will be built"
  else
    warn "no LLVM: building interpreter-only (still correct, just slower)"
  fi
else
  note "JIT disabled by --no-jit"
fi

if [[ "$ENABLE_FFI" == ON ]]; then
  if compgen -G "/nix/store/*libffi-*/lib/libffi.so" >/dev/null || \
     [[ -f /usr/include/ffi.h || -f /usr/local/include/ffi.h ]]; then
    ok "libffi found -- std.ffi will be able to call C"
  else
    warn "no libffi: std.ffi will report that C is unreachable"
  fi
else
  note "FFI disabled by --no-ffi"
fi

# --- clean ------------------------------------------------------------------

if [[ $DO_CLEAN -eq 1 ]]; then
  step "Cleaning"
  rm -rf "$BUILD_DIR" build-nojit build-tsan
  cargo clean 2>/dev/null || true
  ok "removed build directories and cargo artifacts"
fi

# --- compiler ---------------------------------------------------------------

step "Building the compiler (dreamc)"
# The compiler is the only Cargo project; the VM is C++ and is built below.
cargo build --offline -p dreamc ${CARGO_PROFILE} 2>&1 | sed 's/^/    /' || \
  cargo build -p dreamc ${CARGO_PROFILE} 2>&1 | sed 's/^/    /'
DREAMC="target/${CARGO_DIR}/dreamc"
[[ -x "$DREAMC" ]] || die "cargo finished but $DREAMC is missing"
ok "$DREAMC"

# --- VM ---------------------------------------------------------------------

step "Building the VM (libdream, dream)"
cmake -S . -B "$BUILD_DIR" \
      -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
      -DDREAM_ENABLE_JIT="$ENABLE_JIT" \
      -DDREAM_ENABLE_FFI="$ENABLE_FFI" \
      -DDREAM_BUILD_COMPILER=OFF 2>&1 | sed -n 's/^-- Dream: /    /p'
cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || echo 4)" 2>&1 | \
  grep -E 'error|warning:' | sed 's/^/    /' || true

dream="$BUILD_DIR/bin/dream"
[[ -x "$dream" ]] || die "cmake finished but $dream is missing"
ok "$dream"
ok "$BUILD_DIR/lib/libdream.so"

# --- smoke test -------------------------------------------------------------

step "Checking the toolchain works"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cat > "$TMP/hello.dr" <<'DREAM'
import std.console;
let rec fac n = if n <= 1 { 1 } else { n * fac (n - 1) };
let main! = { fac 10 |> console.print! "fac 10 = " }
DREAM
"$DREAMC" "$TMP/hello.dr" -o "$TMP/hello.dream" >/dev/null 2>&1 || die "the compiler could not compile a trivial program"
result="$("$dream" "$TMP/hello.dream" 2>&1)" || die "the VM could not run a trivial program: $result"
[[ "$result" == "fac 10 = 3628800" ]] || die "unexpected result: $result"
ok "compiled and ran a program end to end"

# --- tests ------------------------------------------------------------------

if [[ $RUN_TESTS -eq 1 ]]; then
  step "Running tests"
  cargo test --offline -p dreamc ${CARGO_PROFILE} 2>&1 | grep -E '^test result' | sed 's/^/    compiler: /'
  "$BUILD_DIR/bin/dream_tests" 2>&1 | tail -1 | sed 's/^/    vm: /'
  DREAMC="$DREAMC" dream="$dream" dream/tests/e2e.sh 2>&1 | tail -1 | sed 's/^/    programs: /'
fi

# --- install ----------------------------------------------------------------

if [[ -n "$PREFIX" ]]; then
  step "Installing into $PREFIX"
  cmake --install "$BUILD_DIR" --prefix "$PREFIX" >/dev/null
  install -Dm755 "$DREAMC" "$PREFIX/bin/dreamc"
  if [[ -d mind/std ]]; then
    mkdir -p "$PREFIX/share/dream"
    cp -r mind "$PREFIX/share/dream/"
  fi
  ok "installed"
  note "add $PREFIX/bin to PATH, and set DREAM_PACKAGES=$PREFIX/share/dream/mind"
fi

# --- summary ----------------------------------------------------------------

echo
printf '%sDream is built.%s\n' "$BOLD" "$RESET"
printf '  compiler  %s\n' "$DREAMC"
printf '  vm        %s\n' "$dream"
printf '  library   %s\n' "$BUILD_DIR/lib/libdream.so"
echo
printf '  %s./%s program.dr -o program.dream && ./%s program.dream%s\n' \
       "$DIM" "$DREAMC" "$dream" "$RESET"
printf '  %sjust --list  for the other tasks%s\n' "$DIM" "$RESET"
