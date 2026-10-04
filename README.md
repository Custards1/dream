# Dream

Dream is a lazily evaluated, purely functional language with checked effects,
BEAM-style processes, and a compiler written in itself.

```dream
import std.console;
import std.list;

union Shape { circle(radius : :float), square(side : :float) }

let area shape : :float =
    match shape {
        [:circle, r] => 3.14159 * r * r,
        [:square, s] => s * s,
    };

let rec total !acc shapes =
    match shapes {
        [] => acc,
        [s, ..rest] => total (acc + area s) rest,
    };

let main! = {
    let shapes = [Shape.circle 1.0, Shape.square 2.0];
    let worker = spawn! $( total 0.0 shapes );
    console.print! ("total area: " + to_string (join! worker))

    // An infinite list, of which only five squares are ever computed.
    let squares = list.map (fn n -> n * n) (list.from 1);
    console.print! (list.take 5 squares)
};
```

```
total area: 7.14159
[1, 4, 9, 16, 25]
```

What is in that program:

- **Everything is lazy** until something needs it. `list.from 1` never ends,
  and nothing minds, because `take 5` only asks for five.
- **Effects are in the name.** `main!`, `spawn!` and `console.print!` end in
  `!`; `area` and `total` do not, and the compiler holds them to it: a pure
  function cannot reach an impure one, across every import.
- **Processes share nothing.** `spawn!` runs a suspended computation in a green
  process with its own heap; `join!` waits for its answer. Hundreds of
  thousands fit in memory, and the scheduler preempts every one of them.
- **Types are optional and checked where written.** `: :float` is a signature
  the compiler checks; `union` declares tagged values a `match` must cover.
  Code no signature touches is never rejected.
- **`!acc` is a strict parameter**: forced on entry, so the accumulator stays
  a number instead of a chain of suspensions, and the loop is one the JIT can
  compile.

Beyond that, the language has syntax macros run at compile time, records,
compile-time evaluation and contracts, unbounded integers, tensors with a GPU
backend, a C FFI with owned handles, TLS, and a standard library with servers,
supervisors, codecs and SQL. [docs/language-spec.md](docs/language-spec.md) is
the whole language.

## Getting started

You need a C++20 compiler, CMake 3.20, [`just`](https://github.com/casey/just),
**libffi** and **OpenSSL 3** -- `std.ffi` and `std.tls` are part of every VM.
LLVM is optional and gives the JIT. On Nix, `nix-shell` at the root provides
all of it.

```
just                 # the VM, then the compiler, built from the checked-in seed
just mind            # the build tool
just install         # the VM, mind, the compiler and the language server in ~/.mindv2
```

`./build.sh` does the same from a clean checkout and, when something is
missing, says what and what you lose without it.

Run a single file:

```
just run hello.dr
```

Or make a project:

```
mind new hello
cd hello
mind run
```

`just repl` (or `mind repl` inside a project) is an interactive session.

## What is here

| | |
|---|---|
| [`dream/`](dream/README.md) | **The VM**, in C++: a lazy graph-reduction interpreter, green processes, a generational collector and an LLVM JIT. |
| [`dreams/`](dreams/README.md) | **The compiler**, in Dream: `.dr` source to a `.dream` image. It builds itself from a checked-in seed. |
| [`mind/tool/`](mind/tool/README.md) | **`mind`**, the build tool: packages, dependencies, build scripts, tests. |
| [`mind/std/`](mind/std/README.md) | **The standard library.** |
| [`lucid/`](lucid/README.md) | **The language server**, which is the compiler, imported as a library. |
| [`editors/vscode/`](editors/vscode/README.md) | The VS Code extension. |
| [`examples/`](examples/README.md) | Example programs, each a test: its output is recorded beside it. |
| [`benchmark/`](benchmark/README.md) | Dream against CPython, and the tensor benchmarks. |
| [`docs/`](docs/README.md) | The language reference, the image format, the collector, the FFI -- and [`docs/notes/`](docs/notes/README.md), the log of what was measured and why things are the way they are. |

The pieces depend on each other in one direction. The VM runs images and knows
nothing about source. The compiler is an image the VM runs. `mind` is a Dream
program that finds packages and hands them to the compiler. `lucid` is the
compiler with a different front door.

## How it fits together

**Whole-program compilation.** A program is compiled with everything it
imports into one image, so a build is "find the packages, hand them to the
compiler". There is no object file and no link step. What a large build keeps
between runs is each module's parse, walk and lowering, in a unit cache keyed
by what they depend on, and the image written from them is byte for byte the
one a clean build writes.

**A portable image.** A `.dream` file is bytecode: fixed-width little-endian
integers, indices rather than pointers. The same file runs on Linux, macOS and
Windows ([docs/platforms.md](docs/platforms.md)). The VM maps it and reads it
where it lies. The JIT generates machine code locally, at run time.

**A self-hosting compiler.** `dreams/bootstrap/dreams.dream` is an image of the
compiler, checked in. It needs nothing but the VM to rebuild the compiler from
source, and the guarantee is byte equality: what it builds builds an identical
copy of itself. `just bootstrap-check` is that check.

## Testing

```
just test            # every suite, one group at a time
mind test            # the same suites at once (the repository is a workspace)
mind test dreams     # one package's
just test-all        # adds fuzzing, a heap-verified run and the no-JIT build
```

The groups and what each one asks are listed in [CLAUDE.md](CLAUDE.md#testing).
Close any editor running `lucid` first: the suite rewrites images that a
running server has mapped, and the server dies of `SIGBUS`.

## Platforms

Linux, macOS and Windows, 64-bit little-endian. Linux is where it is developed
and where every suite runs; [docs/platforms.md](docs/platforms.md) says what is
portable, what is not, and how it is checked on the other two.
