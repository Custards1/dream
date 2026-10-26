# Dream

Dream is a lazily evaluated, purely functional language with checked effects,
BEAM-style processes, and a compiler written in itself.

```dream
import std.console;
import std.list;

union Shape { circle(radius : :float), square(side : :float) }

let area shape : :float =
    match shape {
        Shape.circle(r) => 3.14159 * r * r,
        Shape.square(s) => s * s,
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
supervisors, codecs and SQL. [dreams/docs/language-spec.md](https://github.com/dream-mindv2/dreams/blob/main/docs/language-spec.md) is
the whole language.

## Getting started

You need a C++20 compiler, CMake 3.20, [`just`](https://github.com/casey/just),
**libffi** and **OpenSSL 3** -- `std.ffi` and `std.tls` are part of every VM --
and **LLVM 15+** for the JIT, which a build leaves out only when told to
(`-DDREAM_ENABLE_JIT=OFF`).

This repository is the **workspace**: each component lives in a repository of
its own under [dream-mindv2](https://github.com/dream-mindv2) and is checked
out here as a submodule, so clone it with them:

```
git clone --recursive https://github.com/Custards1/dream
cd dream
just subs            # (or after a plain clone) fetch every component, each on its branch
```

```
just                 # the VM, then the compiler, built from the checked-in seed
just mind            # the build tool
just install         # the VM, mind, the compiler and the language server in ~/.mindv2
```

`./build.sh` does the same from a clean checkout and, when something is
missing, says what and what you lose without it.

With Nix 2.27 or newer, install the full toolchain from this checkout with
`nix profile add .`, or build it with `nix build .`. The flake includes the
component submodules automatically. On older Nix versions, pass
`'git+file://{absolute-checkout-path}?submodules=1'` instead of `.` (replacing
`{absolute-checkout-path}` with the full path to this checkout).

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

The interactive REPL (`just repl`, `mind repl`, or `dreams --repl` / `-i`)
is deprecated and warns on startup. Use source files with `just run FILE`
or `mind run` instead.

## What is here

| | | |
|---|---|---|
| [dream](https://github.com/dream-mindv2/dream) | `dream/` | **The VM**, in C++: a lazy graph-reduction interpreter, green processes, a generational collector and an LLVM JIT. Its references and performance log are in its `docs/`. |
| [dreams](https://github.com/dream-mindv2/dreams) | `dreams/` | **The compiler**, in Dream: `.dr` source to a `.dream` image. It builds itself from a checked-in seed. The language specification is in its `docs/`. |
| [std](https://github.com/dream-mindv2/std) | `mind/std/` | **The standard library.** |
| [mind](https://github.com/dream-mindv2/mind) | `mind/tool/` | **`mind`**, the build tool: packages, dependencies, build scripts, tests, documentation sites, serving and packaging. |
| [lucid](https://github.com/dream-mindv2/lucid) | `lucid/` | **The language server**, which is the compiler, imported as a library. |
| [vscode-dream](https://github.com/dream-mindv2/vscode-dream) | `editors/vscode/` | The VS Code extension. |
| [dream.nvim](https://github.com/dream-mindv2/dream.nvim) | `editors/nvim/` | The Neovim plugin. |
| [learn](https://github.com/dream-mindv2/learn) | `learn/` | **Learn Dream**, a book from a first program to a web server, every example run. Start here to learn the language. |

And in this repository itself:

| | |
|---|---|
| [`examples/`](examples/README.md) | Example programs, each a test: its output is recorded beside it. |
| [`benchmark/`](benchmark/README.md) | Dream against CPython, and the tensor benchmarks. |
| [`docs/`](docs/README.md) | The index of every document, each of which lives with its component -- and of [`docs/notes/`](docs/notes/README.md), the log of what was measured and why things are the way they are. |
| `justfile`, `build.dr`, `mind.toml` | How the components are built and tested together. `just subs-status`, `subs-pull`, `subs-push` and `subs-bump` work across all of them. |

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
integers, indices rather than pointers. The same file runs on Linux, macOS, Windows and
Android ([dream/docs/platforms.md](https://github.com/dream-mindv2/dream/blob/main/docs/platforms.md)). The VM maps it and reads it
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

Linux, macOS, Windows and Android, 64-bit little-endian. Linux is where it is developed
and where every suite runs; [dream/docs/platforms.md](https://github.com/dream-mindv2/dream/blob/main/docs/platforms.md) says what is
portable, what is not, and how it is checked on each platform.

Build release packages with `mind pack` ([formats and configuration](https://github.com/dream-mindv2/mind/blob/main/PACKAGING.md)).
Build guides and source API documentation with `mind docs` ([authoring docs and runnable examples](https://github.com/dream-mindv2/mind/blob/main/DOCUMENTATION.md)).
