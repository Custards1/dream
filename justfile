# Dream: a lazily evaluated functional language.
#
#   dreams/ the compiler, in Dream           -- source to `.dream` bytecode
#   dream/  the VM (dream), in C++           -- interpreter, green processes, LLVM JIT
#   mind/   the build system and standard library, in Dream
#
# `just` with no target builds the VM and then the compiler with itself. Nothing
# in that path is Rust: `dreams` builds from the checked-in seed, and the seed
# needs the VM and nothing else.

set positional-arguments
install_dir :="~/.mindv2"
build_dir := "build-dream"
dream := build_dir / "bin/dream"
seed := "dreams/bootstrap/dreams.dream"
image := "build/dreams.dream"
# The compiler, as a command. `dreams` is an image rather than a native program,
# so running it is handing it to the VM.
dreams := dream + " " + image

default: build

# --- building ---------------------------------------------------------------

build: vm dreams

release:
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

# The compiler, built by the compiler: the checked-in seed compiles this source
# into `build/dreams.dream`, which is what every other recipe here runs.
dreams: vm
    mkdir -p build
    ./{{dream}} {{seed}} -L mind -L . -o {{image}} dreams/main.dr

# The build tool, itself a Dream program.
mind: dreams
    mkdir -p build
    ./{{dreams}} -L mind/std mind/tool/main.dr --shebang -o build/mind

# The language server. It imports `dreams` as a library, so it is the one
# program here that is both built by the compiler and made of it.
lucid: dreams
    mkdir -p build
    ./{{dreams}} lucid/main.dr -L mind -L . -o build/lucid.dream
    @echo "built build/lucid.dream -- run it as: {{dream}} build/lucid.dream"

# Build `dreams` from the checked-in image.
#
# The image is a fixpoint: compiling this source with it produces a
# byte-identical copy of itself. To move the seed forward after changing the
# compiler, run this and keep `build/dreams.dream`.
bootstrap: vm
    mkdir -p build
    ./{{dream}} {{seed}} -L mind -L . -o build/dreams.dream dreams/main.dr

# The seed must still reproduce itself from this source: what it builds must
# build an identical third image. That equality is the whole guarantee -- it
# says the compiler in the tree and the compiler in the image agree.
bootstrap-check: vm
    ./{{dream}} {{seed}} -L mind -L . -o /tmp/dreams-stage2.dream dreams/main.dr
    ./{{dream}} /tmp/dreams-stage2.dream -L mind -L . -o /tmp/dreams-stage3.dream dreams/main.dr
    cmp /tmp/dreams-stage2.dream /tmp/dreams-stage3.dream
    @echo "the bootstrap image reproduces itself"

# `mind`'s own tests: path handling, manifest reading, dependency specs.
test-mind: build
    ./{{dreams}} -L mind/std mind/tool/main.dr --test -o /tmp/dream-mind-tests.dream
    ./{{dream}} /tmp/dream-mind-tests.dream

clean:
    rm -rf {{build_dir}} build-nojit build-tsan
    rm build/mind
    find . -name '*.dream' -delete


# --- running ----------------------------------------------------------------

# An interactive session: read an entry, compile the session, run it.
repl *ARGS: build
    ./{{dreams}} --repl -L mind -L . {{ARGS}}

# Compile and run a program: `just run examples/hello.dr`
run FILE *ARGS: build
    ./{{dreams}} {{FILE}} -o /tmp/dream-run.dream
    ./{{dream}} /tmp/dream-run.dream {{ARGS}}

# Compile only.
compile FILE *ARGS: dreams
    ./{{dreams}} {{FILE}} {{ARGS}}

# Type-, scope- and purity-check without writing an image.
check FILE: dreams
    ./{{dreams}} {{FILE}} --no-emit

# Show the execution trees a program compiles to.
dump FILE: build
    ./{{dreams}} {{FILE}} --ir

# Show the modules and packages a program pulls in.
modules FILE: dreams
    ./{{dreams}} {{FILE}} --modules

packages FILE: dreams
    ./{{dreams}} {{FILE}} --packages

# The LLVM IR generated for one function.
jit-ir FILE FN: build
    ./{{dreams}} {{FILE}} -o /tmp/dream-jit.dream
    ./{{dream}} /tmp/dream-jit.dream --dump-jit {{FN}}

# A directly runnable program: the image carries a `#!` line and the execute
# bit, and the VM skips the line when it loads it.
run-script FILE OUT: build
    ./{{dreams}} {{FILE}} --shebang -o {{OUT}}
    ./{{OUT}}

# What an installation is: the VM and `mind` on the path, and the images beside
# them in `$MINDV2_PATH` itself, which is where `dream -x NAME` looks. That is
# what makes `dream -x dreams` and `dream -x lucid` work from anywhere.
install-artifacts:
    mkdir -p {{install_dir}}/bin
    cp {{dream}} {{install_dir}}/bin/dream || true
    cp build/mind {{install_dir}}/bin/ || true
    cp {{image}} {{install_dir}}/dreams.dream
    cp build/lucid.dream {{install_dir}}/lucid.dream || true
    # The library ships with the compiler that was built against it. An
    # installation whose `std` is older than its `dreams` is a compiler that
    # cannot build anything using a function added since -- which is exactly
    # how this recipe came to copy it: the installed `std` shadowed the one in
    # the checkout and the compiler failed to build itself.
    rm -rf {{install_dir}}/std
    cp -r mind/std {{install_dir}}/std
install-artifactsv2:
    mkdir -p {{install_dir}}
    cp {{dream}} {{install_dir}}/bin/dream || true
    cp build/mind {{install_dir}}/mind.dream || true
    cp {{image}} {{install_dir}}/dreams.dream
    cp build/lucid.dream {{install_dir}}/lucid.dream || true
install: vm dreams mind lucid
    just install-artifacts


# --- testing ----------------------------------------------------------------

# Everything.
test: test-vm test-e2e test-std test-mind test-dreams test-dreams-corpus test-dreams-compile test-bootstrap test-lucid test-lucid-session test-examples

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
    ./{{dreams}} examples/textstats/main.dr --test -L mind -L examples -o /tmp/dream-ex-tests.dream
    ./{{dream}} /tmp/dream-ex-tests.dream

# `dreams`, the self-hosted compiler: the parts of it that exist so far.
# Built from `main.dr` so that every module it reaches has its tests collected.
test-dreams: build
    ./{{dreams}} dreams/main.dr --test -L mind -L . -o /tmp/dream-dreams-tests.dream
    ./{{dream}} /tmp/dream-dreams-tests.dream

# Every Dream file in the repository must parse. The corpus is the real test of
# a parser: the standard library, the build tool, the examples, and `dreams`
# itself, which is the one that has to keep working for this to go anywhere.
#
# `--parse` rather than the default, because the question here is whether each
# file is well formed on its own -- a module in the middle of a package is not a
# program, and following its imports would be asking something else.
test-dreams-corpus: build
    @for f in mind/std/*.dr mind/tool/*.dr examples/*.dr examples/*/*.dr \
              dream/tests/programs/*.dr dreams/*.dr; do \
        ./{{dreams}} --parse "$f" || exit 1; \
    done
    @echo "every file in the corpus parses"
    ./{{dreams}} dreams/ast.dr --test -L mind -L . -o /tmp/dream-ast-tests.dream
    ./{{dream}} /tmp/dream-ast-tests.dream
    ./{{dreams}} dreams/parser.dr --test -L mind -L . -o /tmp/dream-parser-tests.dream
    ./{{dream}} /tmp/dream-parser-tests.dream

# The end of the pipeline: programs `dreams` compiled, run by the VM, checked
# against the output recorded beside them. The other `dreams` tests ask whether
# a stage agrees with a recorded shape; this one asks the only question that
# finally matters, and it is the evidence that the self-hosted compiler works.
test-dreams-compile: build
    dream={{dream}} seed={{seed}} dreams/tests/compile.sh

# The VS Code extension's grammar, tokenized and checked against the scopes it
# promises. Needs `npm install` in editors/vscode first.
test-vscode:
    cd editors/vscode && npm test

# `lucid`'s own tests: positions, framing, and the URI/path boundary.
test-lucid: build
    ./{{dreams}} lucid/main.dr --test -L mind -L . -o /tmp/lucid-tests.dream
    ./{{dream}} /tmp/lucid-tests.dream

# And one whole conversation with it, which is the only place the server is
# checked as a running program rather than as a set of functions.
test-lucid-session: build
    dreams={{image}} dream={{dream}} MIND_STDLIB=mind lucid/tests/session.sh

# The bootstrap: the seed reproduces itself from this source.
test-bootstrap: bootstrap-check

# The standard library's own tests, compiled with `--test`.
test-std: build
    ./{{dreams}} mind/std/all.dr --test -L mind -o /tmp/dream-std-tests.dream
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
