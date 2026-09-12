# Dream

A lazily evaluated functional language. The repository holds a language, its
runtime, and — in progress — its self-hosted compiler.

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
| `docs/` | `language-spec.md`, `builtins.md`, `gc.md`, `bytecode-format.md`. | — |

`docs/bytecode-format.md` is the container as it actually is; `docs/gc.md` and
`docs/dynamic-linking.md` are a design plus a log of how far it has got, and are
the first thing to read before touching either.

## Where this is going

`dreams` is the compiler. The plan, in order:

1. ~~`dreams` reaches parity with the compiler it replaces~~ — every stage agrees
   on the corpus.
2. ~~An old `dreams` build bootstraps the new one~~ — **done**: `dreams` compiles
   itself to a fixpoint.
3. ~~`dreams` is the compiler~~ — **done**: `mind`, `lucid`, the examples, the
   end-to-end programs and `dreams` itself are all compiled by `dreams`; the
   old Rust compiler, `dreamc`, is gone.
4. `mind` moves into `dreams`: `dreams` grows a CLI in `mind`'s shape (project
   commands, not just file-at-a-time flags) and takes over its role. **Next.**

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

`mind` finds its compiler through `--compiler`, then `[build] compiler`, then
`$DREAMS`, then `dreams.dream` from the installation
([mind/tool/build.dr](mind/tool/build.dr#L139)). A name ending in `.dream`
is an image and is run by the VM; anything else is executed directly.

## Building

```
just              # the VM, then the compiler built from the seed
just vm           # the VM only
just dreams       # build/dreams.dream, the compiler
just mind         # build/mind, the build tool
just lucid        # build/lucid.dream, the language server
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

Three tools, all of which had to exist before any of the speedups below could
be justified:

```
dreams --time FILE       # what each stage of a compile cost
dream --profile [N] IMG  # the hottest functions, by reductions
dream --stats IMG        # reductions, collections, bytes allocated and
                         # promoted, and milliseconds stopped in collection
```

A VM option goes **before** the image: `dream --stats build/dreams.dream ...`,
because everything after the image name belongs to the program. All three
report from `os.exit!` as well as from the end of `main`, which matters because
every tool here ends by exiting -- a number only printed on the way out of
`main` is never printed for the runs worth measuring.

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
- A wrapper that only passes its arguments on -- `let head xs = core.head xs` --
  is emitted as the call it stands for. `scope` records which globals are
  wrappers (see "wrappers" there); `lower` rewrites saturated calls of them.
  Roughly 13% of the reductions in a compile, and ~8% off any program that runs.
  A body that is one `get` or `set` of the parameters is a wrapper too
  (`let map_get m k d = m.[k else d]`), and a call of it is the opcode.
  The rule is deliberately narrow: one application, every parameter passed on
  exactly once, literals allowed. "At most once" is not enough -- an argument
  the wrapper ignores would never be lowered, and a lambda in that position was
  already given a function record, so the image would carry a function with no
  body.
- `strict!` is linear in *data*, not in paths, and only because objects carry a
  "deeply forced" bit -- see `AUX_DEEP_FORCED` in [dream/src/value.hpp](dream/src/value.hpp).
  Before that, forcing the compiler's own tables walked shared structure once
  per path to it.
- **A linear walk the machine can do is worth ten of the same walk in Dream.**
  This is the largest single lesson so far: `list.append` and `list.nth`,
  written as the obvious recursions, were between them *a third of a
  self-compile*. Both are now the operation they stand for -- `xs + ys` and
  `xs.[n else ()]` -- and `list.length` is the `len` builtin. Same complexity,
  a tenth of the constant, because a recursion pays a call, a frame and a
  couple of natives per element where an opcode pays one machine step for the
  whole walk. The same argument gave the lexer `core.str_span` and
  `core.str_upto`: a byte class scanned a byte at a time in Dream is a dozen
  reductions per byte, and a line's indentation is one operation.
  Before reaching for a recursion over a list or a string, ask what opcode or
  builtin already means it.
- **What the profile says and what it costs are different questions.**
  `--profile` counts reductions and attributes them to the function whose frame
  is current, which is what found `append` and `nth`. It says nothing about the
  runtime's own time -- collection, allocation, the dispatch loop -- so a C++
  profile is the other half. On the self-compile the split is roughly 60%
  interpreter, 20% collector, 20% natives.
- **What the runtime rebuilt per call, and no longer does.** Each of these was
  found the same way, by counting calls rather than guessing: evaluating a
  `:name` constant interned the atom by *name*, taking a mutex and hashing a
  `std::string`, forty million times in one fold -- image atoms are mapped to
  runtime ids once, at load. Calling any `std` member built a `StrObj` for the
  name and a `NativeObj` around the function pointer at *every call* -- both
  are decided by which member it is, so a process builds each once
  (`Process::native_cache`). A string or float literal allocated a fresh object
  every time it was reached; both are immutable and now have one copy per
  process. And the allocator answered "which size class?" with a binary search
  three hundred million times, where the table inverts into 512 bytes.
- **Naming a thing is a read, not an evaluation.** The machine's shape is "push
  a continuation, evaluate the part, come back", which is what makes it
  interruptible -- but a global, a builtin, a parameter, a member of an
  imported module and a constant are none of them expressions. `operand_value`
  and `callee_operand` in [dream/src/interp.cpp](dream/src/interp.cpp) answer
  "can I just read it?", and the operators, `if`, `.[ ]` and every call site
  ask before falling back to the general path. `acc + x` over two bound locals
  went from five machine steps to one.
- **The JIT must be free when it is not helping.** A function LLVM had refused
  took the JIT's global mutex and two hash lookups on *every entry, for ever*,
  to be told again that it could not be compiled -- in a program that enters a
  hundred and eighty million functions that is not a slow path, it is the
  program. The tier decision is now one inlined atomic load with three states
  (compiled, rejected, still cold): see `Jit::tier`. The JIT is roughly neutral
  on the benchmarks either way, which is its own finding.
- **A quarter of a self-compile was the collector, and now a ninth is.**
  `--stats` reports the pause, which is what made the question askable: 831 ms
  of a 3197 ms compile. The collection of one process now divides across a
  pool of threads -- promotion behind a claim, marking behind an atomic mark
  bit, the sweep by block -- which takes it to 304 ms and the compile to
  2700 ms. [docs/gc.md](docs/gc.md) is the design and the log; what is worth
  carrying away from it is that the three bugs which made the first parallel
  collector *slower* than the serial one were all the same mistake -- paying a
  synchronization cost per object instead of per batch -- and that none of
  them were visible by reading the code.
- **The VM is a shared library, and that is not free.** Without
  `-fno-semantic-interposition` a compiler must assume any global function in a
  `.so` can be interposed at load time, so every cross-TU call goes through the
  PLT and none of them inline -- and the interpreter's hot path is nothing but
  cross-TU calls. Link-time optimization on top of that was measured and bought
  nothing, so it is not enabled.

Always put a timeout on a VM run. A Dream program that diverges does not stop on
its own, and the VM will happily sit there.

## Large data in an image

An image addresses everything with `u32` — a section offset, a `KSTR` record,
`StrObj::len` — so a `.dream` file and every string in it stop at 4 GiB. That is
plenty for a program and nothing for the datum a program is *about*. The payload
is the one region past that line:

```
dreams --payload FILE [--payload FILE ...] -o out.dream main.dr
```

Each file becomes one entry of the `LDAT` table, in the order given, and the
bytes go in `PAYL` at the very end of the file. A program reaches them with
`core.data_count ()` and `core.data_at i`.

What comes back is **not a string**. It is a `bigstr`: a length and a pointer
into the mapped image, built in constant time and copied by nobody — not on
materialization, not by the collector, not by `spawn!`, which all share the view
because the bytes belong to the runtime's image and outlive every process in it.
So `len`, `str.byte`, `str.slice` (another view), `==`, the ordering operators,
use as a map key, and `io.write!` all work without a copy; `+`, `to_string`,
`str.concat_all`, `str.find` and `str.chars` refuse, because each would have to
build a `StrObj` and so would undo both halves of the point. `type_of` says
`:bigstr` rather than `:string` precisely so that no code path written for one
is handed the other.

A `bigstr` and a `string` of the same bytes are `==` and hash alike, which is
what makes `str.slice 0 4 data == "%PDF"` mean what it looks like.

Two consequences worth knowing before changing any of it. The compiler never
holds a payload — a Dream string is capped at the same 4 GiB — so `build!`
measures each file with `io.size!`, lays the table out from the sizes, writes
the ordinary image, and only then streams the files through a bounded buffer
(`copy_payload!` in [dreams/main.dr](dreams/main.dr)). And a build with no
`--payload` emits neither section, so it writes the bytes it wrote before any of
this existed — which is what the bootstrap compares.

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
- `let [a, b] = pair` and `let f %{ :x => x } = ..` destructure with `match`'s
  patterns, lazily: the pattern is checked once, when a name it binds is first
  used. A literal in such a pattern is a compile error — that is a `match`.
- `c.[k]`, `c.[k else d]` and `c.[k => v]` read and change a map (by key), an
  array or a list (by position). They are opcodes (`get`, `set`), and
  `core.head`/`map_get`/`map_put`/`array_get`/`array_set` are written as them in
  [mind/std/core.dr](mind/std/core.dr). `std.core` is Dream; what it cannot say
  it re-exports from the host module `std.native`.
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
