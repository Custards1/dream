# Dream

A lazily evaluated functional language. The repository holds a language, its
runtime, and — in progress — its self-hosted compiler.

## The pieces

| Directory | What it is | Written in |
|---|---|---|
| `dreamc/` | The old compiler, kept as a second opinion. | Rust |
| `dream/` | The VM: interpreter, green processes, LLVM JIT. | C++ |
| `mind/tool/` | `mind`, the build tool. Finds packages, shells out to a compiler. | Dream |
| `mind/std/` | The standard library. | Dream |
| `dreams/` | The compiler. `.dr` source to `.dream` bytecode. **The active work.** | Dream |
| `lucid/` | The language server. Imports `dreams` as a library. | Dream |
| `editors/vscode/` | The VS Code extension: an LSP client and a grammar. | JS |
| `examples/` | Example programs, each with its output recorded beside it. | Dream |
| `docs/` | `language-spec.md`, `builtins.md`. | — |

## Where this is going

`dreams` has replaced `dreamc`. The plan, in order:

1. ~~`dreams` reaches parity with `dreamc`~~ — every stage agrees on the corpus.
2. ~~An old `dreams` build bootstraps the new one~~ — **done**: `dreams` compiles
   itself to a fixpoint.
3. ~~`dreams` is the compiler~~ — **done**: nothing in a normal build or in
   `just test` runs `dreamc`. `mind`, `lucid`, the examples, the end-to-end
   programs and `dreams` itself are all compiled by `dreams`.
4. `mind` moves into `dreams`: `dreams` grows a CLI in `mind`'s shape (project
   commands, not just file-at-a-time flags) and takes over its role. **Next.**

`dreamc` is still in the tree for one reason: `just test-reference` asks both
compilers the same questions — which modules a program resolves to, what its
scope and purity verdicts are, whether it lowers — and compares the answers.
That is a second opinion, not a dependency. When it stops earning its keep,
`dreamc/` and the Rust workspace go.

`dreams` builds from `dreams/bootstrap/dreams.dream`, an image of itself that is
checked in. The seed needs the VM and nothing else, so building the compiler
does not involve Rust:

```
just dreams            # build/dreams.dream, the compiler every other recipe runs
just bootstrap         # the same build, said as what it is
just bootstrap-check   # the seed still reproduces itself from this source
```

The guarantee is byte equality: compiling this source with the seed produces an
identical image, and so does the stage after that. When you change the compiler,
run `just bootstrap` and copy `build/dreams.dream` over the seed.

`mind` finds its compiler through `--compiler`, then `[build] compiler`, then
`$DREAMS`, then `$DREAMC`, then `dreams.dream` from the installation
([mind/tool/build.dr:143](mind/tool/build.dr#L143)). A name ending in `.dream`
is an image and is run by the VM; anything else is executed directly, which is
how a native compiler still works there.

## Building

```
just              # the VM, then the compiler built from the seed
just vm           # the VM only
just dreams       # build/dreams.dream, the compiler
just mind         # build/mind, the build tool
just lucid        # build/lucid.dream, the language server
just compiler     # dreamc, the reference compiler -- only `test-reference` wants it
```

The binaries that matter:

- `build-dream/bin/dream` — the VM
- `build/dreams.dream` — the compiler, an image the VM runs

`build/` is a leftover CMake tree and is not the VM build directory —
`build-dream/` is. What `build/` holds now is what the Dream-side recipes write
into it: `dreams.dream`, `mind`, `lucid.dream`. Do not reach for a `dream`
binary under `build/`.

`./build.sh` builds from a clean checkout and reports what optional dependencies
are missing (LLVM gives the JIT, libffi gives `std.ffi`; neither is required).

## Testing

`just test` runs everything. The groups, and what each one is actually asking:

| Recipe | Question |
|---|---|
| `test-vm` | The VM's own C++ checks (heap, images, atoms) |
| `test-e2e` | Real programs under both interpreter and JIT, which must agree |
| `test-examples` | Every example, output compared against what is recorded beside it |
| `test-std` | The standard library's `when test` blocks |
| `test-mind` | `mind`'s path handling, manifests, dependency specs |
| `test-dreams` | Every `when test` block `dreams/main.dr` reaches |
| `test-dreams-corpus` | Every `.dr` file in the repository parses |
| `test-dreams-compile` | Programs `dreams` compiled, run, output compared |
| `test-bootstrap` | The seed still reproduces itself byte for byte |
| `test-lucid` | The language server's units: positions, framing, URIs |
| `test-lucid-session` | One whole LSP conversation, against a running server |

`just test-reference` is the group that still needs `dreamc`, and it is not part
of `just test`: `test-compiler` (`cargo test -p dreamc`), `test-dreams-modules`
(both loaders resolve the same modules in the same order), `test-dreams-scope`
(both reach the same verdict, and reject for the same reason) and
`test-dreams-lower` (everything `dreamc` accepts also lowers).

The four `dreams/tests/*.sh` scripts run directly with no environment set.
`compile.sh` needs only the VM and the seed; the other three are the differential
ones and default to `target/debug/dreamc`. Override with
`dreamc=... dream=... dreams/tests/scope.sh`.

`just test-vscode` checks the TextMate grammar by tokenizing Dream with it. It
is **not** in `just test`, because it needs `npm install` in `editors/vscode`
first and the rest of the suite needs nothing from outside the repository.

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

Two tools, both of which had to exist before any of the recent speedups could
be justified:

```
dreams --time FILE       # what each stage of a compile cost
dream --profile [N] IMG  # the hottest functions, by reductions
```

`--time` forces each stage where it reads the clock, because a lazy stage that
has not been forced has not run: bind and force on one line and every stage
looks free except the last. `--profile` attributes each reduction to the
function whose frame is current. Natives do not reduce, so work inside a builtin
is charged to its caller -- which is why a member of a host module (`core.head`)
can be expensive without appearing anywhere in the profile.

What has already been learnt from them, so it is not learnt twice:

- Building a string with `+` is quadratic. `str.concat_all` (and `join_str`,
  `repeat`, built on it) copies each piece once. The image writer works in
  strings for this reason: as a list of bytes it was four million objects to
  produce a megabyte.
- A record read more often than it is built wants to be an array. A token was a
  six-element list, and reading one was a fifth of everything the compiler did.
- A membership test over a list of names wants to be a map. The keyword check
  was a tenth of it.
- `strict!` is linear in *data*, not in paths, and only because objects carry a
  "deeply forced" bit -- see `AUX_DEEP_FORCED` in [dream/src/value.hpp](dream/src/value.hpp).
  Before that, forcing the compiler's own tables walked shared structure once
  per path to it.

Always put a timeout on a VM run. A Dream program that diverges does not stop on
its own, and the VM will happily sit there.

## The language server

`lucid` is the compiler answering an editor's questions. It imports `dreams` and
calls its resolver directly — there is no subprocess and no re-parsing of the
compiler's output, because a whole-program language makes "import the compiler"
an ordinary import.

```
just lucid          # build/lucid.dream, which the VS Code extension looks for
```

The extension starts it over stdio, and `vscode-languageclient` appends
`--stdio` to the command line by itself. `lucid` accepts that flag and ignores
it; a server that rejects an unknown option dies before it has read a byte, and
the editor reports only that the connection is erroring.

It analyses the editor's **buffer**, not the file on disk. That is what
`modules.load_overlaid!` is for: a map of path to text the loader reads instead
of the disk. Anything else would answer questions about a program the user is
not looking at.

Two things to know before changing it. A member access is recorded at the head
of its chain — `helper.double` is keyed where `helper` begins — so a cursor on
the field walks back over the dot. And the compiler counts **bytes** while LSP
counts **UTF-16 code units**; `lucid/pos.dr` is the only place that conversion
happens, and it should stay that way.

## The language, briefly

- A `!` suffix marks an impure name. Purity is checked: a pure function cannot
  call an impure one. This is enforced across imports.
- `$( expr )` is a thunk.
- Evaluation is lazy; `strict!` forces.
- `when test { .. }` holds a module's tests, collected by compiling with
  `--test`. Tests are `test.case "name" $( test.eq! expected actual )`.
- Modules are files; `mod name { .. }` writes one inside another. `import a.{x}`
  and `import a.{x as y}` bring members in.
- Compilation is whole-program, which is why a build is just "find the packages,
  hand them to the compiler, run it".

## Conventions

Comments in this codebase explain *why a thing is the way it is*, usually in
full sentences, often several lines at the head of a section. Match that. The
existing prose is the style guide — read the top of
[dreams/emit.dr](dreams/emit.dr) or [dreams/scope.dr](dreams/scope.dr) before
adding to either.

Keep the build warning-free. A stray warning in the output reads as a failure.
