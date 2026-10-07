# dreams, the Dream compiler

`dreams` compiles a Dream program -- a file and everything it imports -- into
one `.dream` image the [VM](../dream/README.md) runs. It is written in Dream,
it is itself an image, and it builds itself.

```
just dreams                                   # build/dreams.dream, from the seed
dream build/dreams.dream -L mind main.dr -o main.dream
dream main.dream
```

Most of the time you do not call it directly: [`mind`](../mind/tool/README.md)
finds the packages and passes them on, and `just run FILE` compiles and runs
one file in this checkout.

## The bootstrap

`bootstrap/dreams.dream` is an image of this compiler, checked in. It needs
nothing but the VM to rebuild the compiler from this source, and the guarantee
is **byte equality**: the image the seed builds builds an identical copy of
itself.

```
just bootstrap          # seed -> build/dreams.dream
just bootstrap-check    # the seed, stage 2 and stage 3 are the same bytes
```

When you change the compiler, build it, then copy `build/dreams.dream` over
the seed. A change that adds atoms to the compiler can take two rounds: build
again with each new image until one compiles the source into itself, and keep
that one ("`mind/std/all.dr --test` is not byte-stable" in
[docs/notes/macro-expansion.md](../docs/notes/macro-expansion.md) says why).
`bootstrap-check` fails on a seed that has fallen behind the source, and says
which image to copy over it.

## The pipeline

A build is the same stages in the same order wherever it is asked for -- the
command line, the REPL, the language server -- because they all call
[`compile.dr`](compile.dr) rather than spelling the stages out.

1. **Load.** Lex and parse the entry file, follow its imports through the
   packages on the search path, expand records and `foreign` declarations
   into ordinary modules, and run macros (`expand`) on an embedded VM.
2. **Resolve.** Say what every name means and check purity: a pure function
   that can reach an impure one is an error.
3. **Lower and link.** Turn syntax trees into the execution-tree arena the
   image holds, evaluating `comp` expressions on the way, and fuse `std.list`
   pipelines into loops.
4. **Check types.** Hold the program to its signatures, and run compile-time
   contracts against the values the compiler already knows.
5. **Share.** One rebuild of the finished arena, sharing identical subtrees.
6. **Emit.** Lay out the image's sections and write the bytes.

Stages 2 and 3 run in **parts**, one per module, each in a process of its own,
so a large build uses every core. With `--units DIR` each part is kept between
builds, keyed by the compiler's digest, what the module can see and its own
bodies; an edit to one module walks and lowers that module and reads the rest
back. Parses and type checks are kept the same way. The image is byte for byte
the one a clean build writes, and `tests/units.sh` holds it to that. `mind`
passes `--units` on every build.

Everything after the impure stages is lazy: an image nobody asks for is never
optimized, and `--check` costs a resolve and nothing more.

## The command line

```
dreams [options] FILE
```

What to do (the default is `--check`):

| | |
|---|---|
| `-o, --output PATH` | compile, and write the image there |
| `--check`, `--no-emit` | resolve every name, check purity and types, write nothing |
| `--parse` | parse this file alone, following no imports |
| `--tokens`, `--ast`, `--ir` | print the tokens, the syntax tree, or the lowered execution trees |
| `--modules`, `--packages` | list what the program pulls in |
| `--symbols`, `--at LINE:COL` | what a file defines, and what is at a position -- the questions an editor asks |
| `--stats` | count what the program contains |
| `--time` | compile the whole way and say what each stage cost |
| `--repl` | an interactive session |
| `--fmt FILE..`, `--fmt-check FILE..` | lay files out in the house style in place, or name the ones that would change |

Where to look, and what to compile it as:

| | |
|---|---|
| `-L DIR`, `-L NAME=DIR` | a package, or a directory of packages; `NAME=` imports it under another name |
| `-I DIR` | a directory to search for modules |
| `-D NAME[=V]`, `-D KEY:NAME=V` | define a `when` flag, or set a package's option |
| `--target SPEC` | where the image may run: `linux`, `os=macos,arch=aarch64`, `any` |
| `--test` | collect every module's `when test` block and add a runner |
| `--release`, `--debug-cfg` | define `release` or `debug` |
| `--payload [NAME=]FILE` | carry a file's bytes in the image (`std.payload`) |
| `--units DIR` | keep each module's parts between builds |
| `--shebang [LINE]` | prefix the image with `#!` and make it executable |
| `--depfile PATH` | write the files the build read as a make rule |
| `--no-opt` | skip sharing; the image is the arena as lowered |

`dreams --help` is the full list.

## The REPL

`dreams --repl` (or `just repl`, or `mind repl`) is not the usual loop. A Dream
program is compiled whole, so there is no environment to extend one binding at
a time: the session is the *text* of what has been defined, every entry
recompiles it, and the result runs in a child VM. A definition survives because
its text does; a runtime value does not. [`repl.dr`](repl.dr) and
[docs/session-mode.md](../docs/session-mode.md) say the rest.

## Layout

| | |
|---|---|
| `main.dr` | the command line |
| `compile.dr` | the pipeline, as a library |
| `lexer.dr`, `parser.dr`, `ast.dr` | source to syntax trees |
| `modules.dr`, `package.dr`, `path.dr` | finding and loading modules and packages |
| `syntax.dr`, `expand.dr` | records, `foreign`, unions, and macro expansion |
| `config.dr`, `target.dr` | `when` conditions, and where an image may run |
| `scope.dr` | name resolution, purity, and `virtual`/`derive` dispatch |
| `builtins.dr` | the names the language provides, and the `_` primitives `std` wraps |
| `lower.dr`, `ir.dr` | syntax trees into the execution-tree arena |
| `fuse.dr` | deforestation: `std.list` pipelines as loops |
| `typecheck.dr`, `contract.dr` | signatures, and compile-time contracts |
| `lint.dr` | warnings for lazy accumulator chains, suspended forces, duplicate literal map keys, unreachable match arms, and literal zero divisors |
| `fmt.dr` | the formatter: reindents by the grammar's own line rules, and proves the tree did not change |
| `opt.dr` | sharing the finished arena |
| `emit.dr` | the image writer ([docs/bytecode-format.md](../docs/bytecode-format.md)) |
| `unit.dr` | compile units: the parts a build keeps |
| `diag.dr`, `source_range.dr` | diagnostics and where they point |
| `repl.dr`, `session.dr` | the interactive session |
| `build.dr` | the package's checks, for `mind test dreams` |
| `TODO.md` | what is next for the compiler's speed |

The comments at the top of each file explain why it is the way it is; read
them, and the section of [docs/notes/](../docs/notes/README.md) a comment
points to, before changing one.

## Tests

```
just test-dreams            # every `when test` block main.dr reaches
just test-dreams-corpus     # every .dr file in the repository parses
just test-dreams-compile    # programs compiled, run, and their output compared
just test-bootstrap         # the seed reproduces itself
mind test dreams            # all of the above, at once
```

`tests/` holds the end-to-end suites: `compile.sh` builds a compiler from the
seed and runs programs through it; `parts.sh`, `units.sh` and `target.sh` check
parallel builds, the unit cache and targets; the Python suites cover macros,
optional and static types, contracts, primitives and modules. `tests/scale.py`
generates programs of N declarations or modules, for finding what grows faster
than it should.
