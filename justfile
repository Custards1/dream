# Dream: a lazily evaluated functional language.
#
#   dreamc/  the compiler (dreamc), in Rust   -- source to `.dream` bytecode
#   dream/  the VM (mindv2), in C++          -- interpreter, green processes, LLVM JIT
#   mind/   the build system and standard library, in Dream
#
# `just` with no target builds both halves.

set positional-arguments
install_dir :="~/.mindv2/bin"
build_dir := "build-mindv2"
dreamc := "target/debug/dreamc"
dreamc_release := "target/release/dreamc"
mindv2 := build_dir / "bin/mindv2"

default: build

# --- building ---------------------------------------------------------------

build: compiler vm

# The compiler. The VM is C++ and is built by the `vm` recipe.
compiler:
    cargo build --offline -p dreamc

release:
    cargo build --offline --release -p dreamc
    cmake -S . -B {{build_dir}} -DCMAKE_BUILD_TYPE=Release
    cmake --build {{build_dir}} -j

# The VM, with the JIT if LLVM can be found.
vm:
    cmake -S . -B {{build_dir}} -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build {{build_dir}} -j

# Interpreter only, to check the VM still builds without LLVM.
vm-no-jit:
    cmake -S . -B build-nojit -DDREAM_ENABLE_JIT=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build build-nojit -j

clean:
    cargo clean
    rm -rf {{build_dir}} build-nojit build-tsan
    find . -name '*.dream' -delete

# --- running ----------------------------------------------------------------

# Compile and run a program: `just run examples/hello.dr`
run FILE *ARGS: build
    ./{{dreamc}} {{FILE}} -o /tmp/dream-run.dream
    ./{{mindv2}} /tmp/dream-run.dream {{ARGS}}

# Compile only.
compile FILE *ARGS: compiler
    ./{{dreamc}} {{FILE}} {{ARGS}}

# Type-, scope- and purity-check without writing an image.
check FILE: compiler
    ./{{dreamc}} {{FILE}} --no-emit

# Show the execution trees a program compiles to.
dump FILE: build
    ./{{dreamc}} {{FILE}} -o /tmp/dream-dump.dream --no-emit --dump

# Show the modules and packages a program pulls in.
modules FILE: compiler
    ./{{dreamc}} {{FILE}} --no-emit --modules

packages FILE: compiler
    ./{{dreamc}} {{FILE}} --packages

# The LLVM IR generated for one function.
jit-ir FILE FN: build
    ./{{dreamc}} {{FILE}} -o /tmp/dream-jit.dream
    ./{{mindv2}} /tmp/dream-jit.dream --dump-jit {{FN}}

install: release
    mv {{dreamc_release}} {{install_dir}}
    mv {{mindv2}} {{install_dir}}

# --- testing ----------------------------------------------------------------

# Everything.
test: test-compiler test-vm test-e2e test-std

test-compiler:
    cargo test --offline -p dreamc

test-vm: vm
    ./{{build_dir}}/bin/mindv2_tests

# Real programs, run under both the interpreter and the JIT, which must agree.
test-e2e: build
    dream/tests/e2e.sh

# The standard library's own tests, compiled with `--test`.
test-std: build
    ./{{dreamc}} mind/std/all.dr --test -L mind -o /tmp/dream-std-tests.dream
    ./{{mindv2}} /tmp/dream-std-tests.dream

# Malformed images must be rejected, never crashed on.
fuzz ITERATIONS="400": build
    ITERATIONS={{ITERATIONS}} dream/tests/fuzz_image.sh

# Run the test programs with the heap verified after every collection.
test-heap: build
    DREAM_VERIFY_HEAP=1 dream/tests/e2e.sh

# Data races, on the concurrency-heavy programs.
test-races:
    cmake -S . -B build-tsan -DDREAM_ENABLE_JIT=OFF -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" \
      -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=thread"
    cmake --build build-tsan -j
    MINDV2=build-tsan/bin/mindv2 dream/tests/e2e.sh

# The slow, thorough set. What to run before believing a change is safe.
test-all: test fuzz test-heap vm-no-jit
    @echo "all checks passed"
