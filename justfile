# Dream: a lazily evaluated functional language.
#
#   dreamc/  the compiler (dreamc), in Rust   -- source to `.dream` bytecode
#   dream/  the VM (dream), in C++          -- interpreter, green processes, LLVM JIT
#   mind/   the build system and standard library, in Dream
#
# `just` with no target builds both halves.

set positional-arguments
install_dir :="~/.mindv2"
build_dir := "build-dream"
dreamc := "target/debug/dreamc"
dreamc_release := "target/release/dreamc"
dream := build_dir / "bin/dream"

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

# The build tool, itself a Dream program compiled by dreamc.
mind:
    mkdir -p build
    {{dreamc}} -L mind/std mind/tool/main.dr -o build/mind
# The dreeams , itself a Dream program compiled by either itself or the soon unsupported dreamc.
dreams:vm mind
    mkdir -p build
    cd dreams && {{dream}} build/mind -L mind/std mind/tool/main.dr -o build/mind
# `mind`'s own tests: path handling, manifest reading, dependency specs.
test-mind: build
    {{dreamc}} -L mind/std mind/tool/main.dr --test -o /tmp/dream-mind-tests.dream
    ./{{dream}} /tmp/dream-mind-tests.dream

clean:
    cargo clean
    rm -rf {{build_dir}} build-nojit build-tsan
    rm build/mind
    find . -name '*.dream' -delete


# --- running ----------------------------------------------------------------

# Compile and run a program: `just run examples/hello.dr`
run FILE *ARGS: build
    ./{{dreamc}} {{FILE}} -o /tmp/dream-run.dream
    ./{{dream}} /tmp/dream-run.dream {{ARGS}}

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
    ./{{dream}} /tmp/dream-jit.dream --dump-jit {{FN}}

# A directly runnable program: the image carries a `#!` line and the execute
# bit, and the VM skips the line when it loads it.
run-script FILE OUT: build
    ./{{dreamc}} {{FILE}} --shebang -o {{OUT}}
    ./{{OUT}}

install-artifacts:
    mv {{dreamc_release}} {{install_dir}}/bin || true
    mv {{dream}} {{install_dir}}/bin || true
    mv build/mind {{install_dir}}/bin || true

install: release mind
    just install-artifacts    


# --- testing ----------------------------------------------------------------

# Everything.
test: test-compiler test-vm test-e2e test-std test-mind test-dreams test-dreams-corpus test-dreams-modules test-dreams-scope test-dreams-lower test-examples

test-compiler:
    cargo test --offline -p dreamc

test-vm: vm
    ./{{build_dir}}/bin/dream_tests

# Real programs, run under both the interpreter and the JIT, which must agree.
test-e2e: build
    dream/tests/e2e.sh

# Every example program, compiled and run, output checked against what is
# recorded beside it. `just examples-bless` re-records after a deliberate change.
test-examples: build
    examples/run.sh

examples-bless: build
    examples/run.sh --bless

# The example package's own tests, which also exercise `virtual`/`derive`
# across files and a path dependency between two packages.
test-examples-std: build
    ./{{dreamc}} examples/textstats/main.dr --test -L mind -L examples -o /tmp/dream-ex-tests.dream
    ./{{dream}} /tmp/dream-ex-tests.dream

# `dreams`, the self-hosted compiler: the parts of it that exist so far.
# Built from `main.dr` so that every module it reaches has its tests collected.
test-dreams: build
    {{dreamc}} dreams/main.dr --test -L mind -L . -o /tmp/dream-dreams-tests.dream
    ./{{dream}} /tmp/dream-dreams-tests.dream

# Every Dream file in the repository must parse. The corpus is the real test of
# a parser: the standard library, the build tool, the examples, and `dreams`
# itself, which is the one that has to keep working for this to go anywhere.
#
# `--parse` rather than the default, because the question here is whether each
# file is well formed on its own -- a module in the middle of a package is not a
# program, and following its imports would be asking something else.
test-dreams-corpus: build
    {{dreamc}} dreams/main.dr -L mind -L . -o /tmp/dreams.dream
    @for f in mind/std/*.dr mind/tool/*.dr examples/*.dr examples/*/*.dr \
              dream/tests/programs/*.dr dreams/*.dr; do \
        ./{{dream}} /tmp/dreams.dream --parse "$f" || exit 1; \
    done
    @echo "every file in the corpus parses"
    {{dreamc}} dreams/ast.dr --test -L mind -L . -o /tmp/dream-ast-tests.dream
    ./{{dream}} /tmp/dream-ast-tests.dream
    {{dreamc}} dreams/parser.dr --test -L mind -L . -o /tmp/dream-parser-tests.dream
    ./{{dream}} /tmp/dream-parser-tests.dream

# `dreams`'s module loader against the one it replaces. Every program in the
# repository must resolve to the same modules, in the same order, under both --
# and the failures `dreamc` cannot report must be reported here.
test-dreams-modules: build
    dreamc={{dreamc}} dream={{dream}} dreams/tests/modules.sh

# `dreams`'s resolution and purity pass. Every program must get the same verdict
# from both compilers, and the broken ones must be rejected for the same reason.
test-dreams-scope: build
    dreamc={{dreamc}} dream={{dream}} dreams/tests/scope.sh

# `dreams`'s lowering. Every program the reference compiler accepts must lower
# to an execution tree, the compiler itself included -- which is the only
# program here big enough to notice a quadratic mistake before it becomes an
# out-of-memory.
test-dreams-lower: build
    dreamc={{dreamc}} dream={{dream}} dreams/tests/lower.sh

# The standard library's own tests, compiled with `--test`.
test-std: build
    ./{{dreamc}} mind/std/all.dr --test -L mind -o /tmp/dream-std-tests.dream
    ./{{dream}} /tmp/dream-std-tests.dream

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
    dream=build-tsan/bin/dream dream/tests/e2e.sh

# The slow, thorough set. What to run before believing a change is safe.
test-all: test fuzz test-heap vm-no-jit
    @echo "all checks passed"
