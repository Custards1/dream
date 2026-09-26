# Dream

A lazily evaluated functional language. The repository holds a language, its
runtime, and — in progress — its self-hosted compiler.
## Core things to follow
Take advantage of the type system when writing or modifying dream code.
Use the std, dont repeat it, and use the helpful structures and patterns it provides.
Use type signitures.
Dont forget about strict parameters, use them when needed

## The pieces

| Directory | What it is | Written in |
|---|---|---|
| `dream/` | The VM: interpreter, green processes, LLVM JIT. | C++ |
| `mind/tool/` | `mind`, the build tool. Finds packages, shells out to a compiler. | Dream |
| `mind/std/` | The standard library. | Dream |
| `dreams/` | The compiler. `.dr` source to `.dream` bytecode. **The active work.** | Dream |
| `lucid/` | The language server. Imports `dreams` as a library. | Dream |
| `editors/vscode/` | The VS Code extension: an LSP client and a grammar. | JS |
| `examples/` | Example programs, each with its output recorded beside it. | Dream |
| `docs/` | `language-spec.md`, `builtins.md`, `ffi.md`, `gc.md`, `bytecode-format.md`. | — |
| `docs/notes/` | The design and performance log: what was measured, kept and thrown away. | — |

`docs/bytecode-format.md` is the container as it actually is; `docs/gc.md` and
`docs/dynamic-linking.md` are a design plus a log of how far it has got, and are
the first thing to read before touching either. The same goes for
[docs/notes/](docs/notes/README.md) and whatever part of the compiler or VM it
describes: that is where the traps are written down, and where a comment's
`see "Some title"` leads.

There is no `std.core` any more. The primitives (`list_cons`, `str_slice`,
`data_at`, ...) are plain builtins, declared in
[dreams/builtins.dr](dreams/builtins.dr) and implemented in
[dream/src/builtins.cpp](dream/src/builtins.cpp). Older notes still say
`core.head` and the like; read that as the builtin.

## Where this is going

`dreams` is the compiler. 

`dreams` builds from `dreams/bootstrap/dreams.dream`, an image of itself that is
checked in. The seed needs the VM and nothing else:

```
just dreams            # build/dreams.dream, the compiler every other recipe runs
just bootstrap         # the same build, said as what it is
just bootstrap-check   # the seed still reproduces itself from this source
```

The guarantee is byte equality: compiling this source with the seed produces an
identical image, and so does the stage after that. When you change the compiler,
run `just bootstrap` and copy `build/dreams.dream` over the seed.

The pipeline a build runs -- resolve, lower and link, types, contracts, share,
emit -- is written once, in [dreams/compile.dr](dreams/compile.dr):
`compile.build!` answers a `Build` whose image and bytes are lazy, and
`compile.check!`/`checked` are what `--check` and `lucid` ask. The command
line, the REPL's session and the language server all call it rather than
spelling the stages out, so a new stage is added in one place.

`mind` finds its compiler through `--compiler`, then `[build] compiler`, then
`$DREAMS`, then `dreams.dream` from the installation
([mind/tool/build.dr](mind/tool/build.dr#L139)). A name ending in `.dream`
is an image and is run by the VM; anything else is executed directly.

A dependency is used by listing it in `[dependencies]` and nothing else: the
key is its import name, a bare string is a path or a pinned URL, and a
directory with no `mind.toml` is still a package. `mind` walks the graph
(`build.graph!`), fetches, reports two different directories under one name as
a conflict, and hands each package to the compiler as `-L DIR`, or `-L KEY=DIR`
when the key is not the package's own name. The compiler half of that -- a key
as a second name for a package, and a manifest-less package -- is `alias!` and
`add_named!` in [dreams/package.dr](dreams/package.dr), so `dreams` follows a
path dependency the same way without `mind`. `mind add`/`remove` edit the
manifest a line at a time and leave the rest of the file as written.
[mind/tool/README.md](mind/tool/README.md) is the user-facing half.

## Building

```
just              # the VM, then the compiler built from the seed
just vm           # the VM only
just dreams       # build/dreams.dream, the compiler
just mind         # build/mind, the build tool
just lucid        # build/lucid.dream, the language server
just vm-pgo       # build-pgo/bin/dream, the VM trained on a self-compile
```

The binaries that matter:

- `build-dream/bin/dream` — the VM
- `build/dreams.dream` — the compiler, an image the VM runs

`just vm-pgo` is the VM again with GCC's branch weights fitted to a
self-compile: it instruments a build, runs the unit tests and two stages of the
bootstrap through it, and rebuilds against the counters. Worth **11-13%** — a
self-compile goes 2.85 s to 2.52 s interpreted, 3.00 s to 2.62 s by default —
and `just bench-self-compile build-pgo/bin/dream` is how to check that on
another machine.

It builds into `build-pgo`, and the separate directory is the point rather than
tidiness. `-fprofile-use` refuses a profile that no longer describes the
source, so a tree carrying one stops building the moment you edit `dream/src`,
and `just vm` does not pass `-DDREAM_PGO` — a cached `USE` survives it. That
combination cost a day once. Nothing but `vm-pgo` reads `build-pgo`, `just vm`
is always editable, and retraining is always from scratch.

One thing to expect: a trained build warns where an ordinary one does not,
because the profile shows GCC which paths are reachable. `core_str_le`'s
zero-width case was the first. Treat those as real — the build is meant to be
warning-free in both modes.

`build/` is a leftover CMake tree and is not the VM build directory —
`build-dream/` is. What `build/` holds now is what the Dream-side recipes write
into it: `dreams.dream`, `mind`, `lucid.dream`. Do not reach for a `dream`
binary under `build/`.

`./build.sh` builds from a clean checkout and reports what dependencies are
missing. libffi is required: `std.ffi` is a module every VM must provide, and a
VM built without it is not a valid VM. LLVM is optional and gives the JIT.

## Testing

`just test` runs everything. The groups, and what each one is actually asking:

| Recipe | Question |
|---|---|
| `test-vm` | The VM's own C++ checks (heap, images, atoms) |
| `test-e2e` | Real programs under both interpreter and JIT, which must agree |
| `test-examples` | Every example, output compared against what is recorded beside it |
| `test-std` | The standard library's `when test` blocks |
| `test-ffi` | `std.ffi`/`std.foreign` against a C library built from `dream/tests/ffi` |
| `test-mind` | `mind`'s path handling, manifests, dependency specs |
| `test-dreams` | Every `when test` block `dreams/main.dr` reaches |
| `test-dreams-corpus` | Every `.dr` file in the repository parses |
| `test-dreams-compile` | Programs `dreams` compiled, run, output compared |
| `test-bootstrap` | The seed still reproduces itself byte for byte |
| `test-lucid` | The language server's units: positions, framing, URIs, completion context |
| `test-lucid-session` | One whole LSP conversation, against a running server |

The `dreams/tests/*.sh` scripts run directly with no environment set; there is
one left, `compile.sh`, and it needs only the VM and the seed.

`just test-vscode` checks the TextMate grammar by tokenizing Dream with it. It
is **not** in `just test`, because it needs `npm install` in `editors/vscode`
first and the rest of the suite needs nothing from outside the repository.

**Close the editor before running the suite.** If a recipe dies with signal 7
(`SIGBUS`), suspect a running `lucid` rather than the change you just made. The
VM *maps* an image and reads it where it lies, and `just test` rewrites
`build/dreams.dream` and the `/tmp` images in place — truncating a file another
process has mapped turns every page past the new end into a SIGBUS for that
process. A `lucid` started by the VS Code extension is exactly such a process:
it is a long-lived VM holding `build/lucid.dream`, and it runs while you are not
thinking about it. The symptom is a recipe that fails once and passes forty
times in a row afterwards, which is the shape of a race and not of a bug.

`just test-all` adds fuzzing, a heap-verified run, and a no-JIT build.

## Running things

```
just repl              # an interactive session
just run FILE [args]   # compile and run
just check FILE        # scope- and purity-check, no image
just dump FILE         # the execution trees it compiles to
just modules FILE      # what it pulls in
```

`dreams --repl` is the interactive interpreter, and it is not the usual loop.
There is no environment to extend one binding at a time — a Dream program is
compiled whole — so the session is the *text* of what has been defined, and
every entry recompiles all of it. A definition survives because its text does;
a runtime value does not, because the result is run as a child VM (`$DREAM`).
[dreams/repl.dr](dreams/repl.dr) says the rest.

`mind repl` is the same session with a project's packages already on the search
path — the flags a session wants are the flags a build wants. It hands the
terminal over with `os.replace!` (`execvp`) rather than running a child, because
`exec!` gives its child pipes and a prompt needs a terminal. Outside a project
it still starts, with just the standard library.

The VM resolves an image four ways, nearest first: the name as written, that
name with `.dream` added, and both of those under `$MINDV2_PATH`. So `dream
mind` runs `./mind.dream` if there is one and the installed `mind.dream`
otherwise, and an arbitrary path still means that path. `dream -x NAME` is the
other half — the installation and nothing else, so a file in the working
directory cannot shadow an installed program. `just install` is what puts
`dreams.dream` and `lucid.dream` there.

## Making it faster

The full record is [docs/notes/vm-performance.md](docs/notes/vm-performance.md)
and its neighbours. What follows is what to know before measuring anything.

The tools:

```
dreams --time FILE           # what each stage of a compile cost, and each phase
                             # of macro expansion for a program with macros
dream --profile [N] IMG      # the hottest functions, by reductions
dream --stats IMG            # reductions, collections, bytes allocated and
                             # promoted, live set at the largest major by kind,
                             # functions the JIT took
benchmark/benchmark/run.sh   # seven workloads, Dream against CPython
dreams/tests/scale.py        # generated programs of N declarations or modules
```

A VM option goes **before** the image (`dream --stats build/dreams.dream ...`),
since everything after the image name belongs to the program. `--time` forces
each stage where it reads the clock, because an unforced lazy stage has not
run. `--profile` charges a native's work, and a compiled callee's, to the
caller. `--stats --profile 1` prints two totals that must agree.

How to measure, which is where most wrong conclusions have come from:

- **A/B against the commit before, in a worktree** (`git worktree add /tmp/base
  HEAD`), with the same VM binary, interleaving runs. Never compare against a
  number written in a note: the compiler grows, and the numbers move with it.
- **3% is the noise floor** for any edit to `step_eval`: code placement alone
  moves `fib` that much. Diff the instruction streams before believing less.
- **The collector's thresholds are geometric** (`live * 3` after each major), so
  allocating less can mean marking more. `DREAM_GC_TRACE=1` shows the series.
- **A peak heap is sampled where a collection happens to land**, so it varies
  by a third between runs of the same binary. Promotion is the steadier number.
- **Grow the input to find a quadratic.** Profile at N and at 2N and divide.
  This repository is too small to feel an n² anywhere, and that is how four
  of them survived.
- **Always put a timeout on a VM run.** A diverging program does not stop.

The lessons that keep coming back:

- A list searched more often than it is built wants to be a map. This has been
  found five times.
- `xs + [x]` in an accumulator is one reduction and a copy of `xs`. It is
  invisible in a reduction profile and quadratic in bytes. Cons, then reverse.
- A walk the machine can do (an opcode or builtin) is worth ten of the same
  walk written as a Dream recursion.
- A lazy value stored in a map or a table pins the version of the table it was
  made in. Force what you store (`ir.settled`, `scope.forced`). `--stats` shows
  it as map branches far outnumbering map entries.
- A reply, a message or a list element is suspended unless forced. A `strict!`
  inside `[:ok, strict! x]` is itself suspended; force it as a statement.
- A suspension carries its frame, so a thunk handed to `spawn!` copies
  everything that frame reaches. Make it in a small function of its own.
- The JIT only helps where it can take the loop. Deforestation and strict
  parameters (`!acc`) exist to hand it one.

## The language, briefly

- A `!` suffix marks an impure name. Purity is checked: a pure function cannot
  call an impure one. This is enforced across imports.
- `$( expr )` is a thunk.
- `let [a, b] = pair` and `let f %{ :x => x } = ..` destructure with `match`'s
  patterns, lazily: the pattern is checked once, when a name it binds is first
  used. A literal in such a pattern is a compile error — that is a `match`.
- `c.[k]`, `c.[k else d]` and `c.[k => v]` read and change a map (by key), an
  array or a list (by position). They are opcodes (`get`, `set`), and
  the builtins that read and write containers are written as them.
- Evaluation is lazy; `strict!` forces. A parameter written `!acc` or
  `(strict acc)` is forced when the function is entered, which is the fix for
  an accumulator that would otherwise become a chain of suspensions and
  overflow. It is also how to get a loop past the JIT's strictness test. See
  "Strict parameters" in docs/language-spec.md; the lowering is a strict
  `local` statement at the top of the body (`lower_body`).
  `std` uses it wherever an accumulator could grow (`list.fold_strict`,
  `list.sum`, the folds of `std.seq` and `std.map`).
- `when test { .. }` holds a module's tests, collected by compiling with
  `--test`. Tests are `test.case "name" $( test.eq! expected actual )`.
- `macro name params = ..` declares a syntax transformer and `expand name args`
  calls one. A macro receives its arguments as **syntax** — ordinary Dream
  lists, with a span last — and returns syntax, which is checked and then
  substituted. It runs on the same embedded VM `comp!` does, before name
  resolution; [dreams/expand.dr](dreams/expand.dr) and [docs/notes/macro-expansion.md](docs/notes/macro-expansion.md) says what
  that costs.
- `group P { x, y }`, `struct P { x, y }` and `mapping P { x, y }` declare a
  record: a module of generated functions — `P.make`, `P.new`, `P.x`, `P.set_x`
  — over a list, an array and a map respectively. The *loader* rewrites them
  (`syntax.record`), so nothing downstream knows a record from a `mod`; "A
  record is a declaration before it is a module" is what that costs a tool.
  A field may be given a default (`x = 0`), which is the `else` of the read its
  accessor compiles to; `new` is the constructor that takes only the fields
  without one. An entry *with parameters* is a **member** — `say_hi self = ..`
  — an ordinary function compiled inside the generated module, where the
  accessors are globals. Having parameters is the whole of what tells the two
  apart, and entries are separated by `,` or by a line break, as a block's
  statements are by `;`.
- `type Name params = description` names an **optional type**: a description
  that is an ordinary value, checked only where a program asks. Its
  right-hand side, and a record field's `: annotation`, are read in a grammar
  of their own -- `:integer -> :integer -> :integer`, `[:integer]`,
  `[:ok, value] | [:error, :string]`, `%{:string => :integer}`,
  `#[:float]`, `:integer where fn n -> n >= 0` -- which builds exactly the
  data `std.types` builds by hand. Everywhere else a bracket is still a list.
  `types.accepts` tests, `types.check` raises, `types.enforce` wraps a
  function against an arrow. Nothing is inferred or coerced, and a `type` is
  a `let`, so imports, `priv`, currying and local capture need no new rules.
  "Optional type descriptions" in [docs/language-spec.md](docs/language-spec.md)
  is the grammar and the one ambiguity it has to resolve.
- `let name : type` is a **signature**, checked at compile time
  ([dreams/typecheck.dr](dreams/typecheck.dr)); `let x : t = e` and
  `let f x : answer = e` are the inline forms. A free lowercase name in one is
  a type variable. A signature changes no node -- the one thing it adds to an
  image is a hint for the JIT -- code no signature touches is
  never rejected, and `--no-types` skips the pass. [docs/notes/static-types.md](docs/notes/static-types.md)
  is the design.
- `union Shape { circle(radius : :float), empty }` declares a **discriminated
  union**: a module of constructors whose values are `[:circle, r]` and
  `:empty` -- the tagged lists Dream already writes by hand -- plus a global
  `Shape` holding its description. A `match` on one must handle every
  variant. Lists back it; an array opt-in, as `struct` is to `group`, is
  planned and not built.
- `foreign lib from "libx.so" { resource Db = db_close; open! : :cstr -> out Db -> :int = db_open }`
  declares a C library: each line a C signature in the type grammar, which the
  loader (`syntax.foreign_decl`) binds through `std.ffi` *and* signs with its
  Dream type, so a wrong handle is a compile error. A pointer C hands back is
  a handle owned by the calling process and destroyed when it is released or
  the process ends; `foreign.run! server (fn () -> ..)` does work in a library
  server that owns what is made through it. `foreign` is contextual -- still
  the name of `std.foreign`. [docs/ffi.md](docs/ffi.md).
- Modules are files; `mod name { .. }` writes one inside another. `import a.{x}`
  and `import a.{x as y}` bring members in.
- Compilation is whole-program, which is why a build is just "find the packages,
  hand them to the compiler, run it".

## Conventions

In Dream, write `match` rather than a long `if`/`else if` chain, and convert
such a chain to a `match` when you come across one. There are plenty left.

Comments in this codebase explain *why a thing is the way it is*, usually in
full sentences, often several lines at the head of a section. Match that. The
existing prose is the style guide — read the top of
[dreams/emit.dr](dreams/emit.dr) or [dreams/scope.dr](dreams/scope.dr) before
adding to either.

Keep the build warning-free. A stray warning in the output reads as a failure.
