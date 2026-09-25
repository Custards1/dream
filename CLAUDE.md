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
# Code Requirements when writing in the dream language
Use match instead of massive if/else/else if chains. If you see an ugly if else chain in dream convert it to the match pattern. There are a lot of ugly offenders, so just update as you go.

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

Three tools, all of which had to exist before any of the speedups below could
be justified:

```
dreams --time FILE       # what each stage of a compile cost, and -- for a
                         # program with macros -- what each phase of expanding
                         # them cost
dream --profile [N] IMG  # the hottest functions, by reductions
dream --stats IMG        # reductions, collections, bytes allocated and
                         # promoted, milliseconds stopped in collection, and
                         # how many functions the JIT took
benchmark/benchmark/run.sh   # seven workloads, Dream against CPython
```

`--stats` answers two questions about memory that are easy to confuse. What a
program *allocated*, by kind, is what it made; what is *live at its largest
major*, by kind -- with the object count and average size beside each -- is
what it is still holding, which is the one that decides whether a large program
fits. They rarely name the same kind, and the gap between them is where a
representation problem lives: see "A lazy value stored in a map pins the map it
was made in" below, which is a finding nothing else here could have reached.

A VM option goes **before** the image: `dream --stats build/dreams.dream ...`,
because everything after the image name belongs to the program. All three
report from `os.exit!` as well as from the end of `main`, which matters because
every tool here ends by exiting -- a number only printed on the way out of
`main` is never printed for the runs worth measuring.

A compiled function that calls another compiled function makes the same kind of
hole a native does: the callee is a machine call inside the caller's compiled
body, so its reductions are charged to the caller and it does not appear in the
profile at all. `collatz_steps` under a fused `collatz` is the example.

`--time` forces each stage where it reads the clock, because a lazy stage that
has not been forced has not run: bind and force on one line and every stage
looks free except the last. `--profile` attributes each reduction to the
function whose frame is current. Natives do not reduce, so work inside a builtin
is charged to its caller -- which is why a member of a host module (`core.head`)
can be expensive without appearing anywhere in the profile.

A profile is only worth as much as its coverage, and this one's was wrong twice
over until 2026-09-12. The reductions a *nested* force makes -- which is most of
a compile, because `dreams` forces at every stage boundary -- were counted but
never attributed, and a JIT-compiled body was invisible however hot it got. The
first of those was hiding a quarter of a self-compile in one function. Check the
two totals agree: `--stats --profile 1` prints the attributed sum and the run's
own count, and they should be the same number.

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
- **A list searched more often than it is built wants to be a map** -- and the
  reason this appears twice in these notes is that it was found twice. The
  keyword check was the first. The second was `ir.find_op`, a linear scan of
  the 41-entry opcode table run once per node the lowerer emits and once per
  node an image is read back from: **a quarter of a self-compile**, 20M
  reductions, gone by indexing the same table three ways at module level. It
  had been there all along and no profile had ever named it, because the
  profiler could not see the loop it ran in.
- **The JIT was charging three reductions for every one it ran.** `--stats` said
  194M reductions for a compile that does 58M, because a compiled body folded
  the whole fall in the budget into the total -- including the reductions the
  interpreter it re-entered had already counted, and again for every compiled
  frame above them. Scheduling was never affected (the budget itself was always
  right), but every number derived from the count was. The JIT does 57.8M
  reductions where the interpreter does 60.6M, which is the honest measure of
  what it saves and was not previously knowable.
- **65% of what a program allocates is the call, not the work.** `--stats` says
  so by kind: on a self-compile, frames 34% and thunks 31%; on `fib 32` those
  two *are* the program. The cheapest part of that bill is an argument that is
  one addition, subtraction or multiplication of numbers already in hand --
  `f (n - 1)` -- which used to cost a Thunk, a Blackhole write, an Indirect
  write, a continuation and a reduction to save an add. `thunk_for` computes
  those instead, and `fib 32` lost 43% of its allocation and 35% of its time.
  It needs no strictness analysis and is not one: `operand_value` reads an
  operand only when it is *already* in normal form, so nothing is forced early,
  and two fixnums added cannot raise or diverge. Division is excluded because
  `x / 0` raises, and overflow falls back to the thunk --
  `dream/tests/programs/lazy_args.dr` is what holds that line. The remaining
  thunks are calls and data, and those *would* need the analysis.
- **A native's strict arguments are evaluated, not suspended.** A native
  declares which of its arguments it forces and `resume_native` forces exactly
  those -- but the call site had already built a Thunk for every argument it
  passed, so for a strict one that Thunk was a temporary and nothing else:
  blackholed, evaluated and overwritten with an Indirect on the very next step,
  never shared with anyone, an allocation and two writes spent carrying
  `(node, frame)` four steps. The machine's own continuation carries them
  instead -- `fill_native_args` and `ContKind::NativeArgs` in
  [dream/src/interp.cpp](dream/src/interp.cpp), which read the callee back out
  of its own node rather than carrying it, on the same grounds `BinRight` reads
  its right operand after forcing its left. A self-compile makes 3.35M fewer
  thunks (10.3M -> 7.0M) and allocates 6.3% less (1256 MB -> 1177 MB) for an
  identical reduction count and a byte-identical image. A strict argument of a
  shape `thunk_for` already answers without allocating -- a constant, a bound
  local, `n - 1`, an in-range array read -- is still left to it, because
  computing one of those is cheaper than evaluating it. What the change did
  *not* do is make the self-compile finish sooner, and why not is the next
  section, which is the more useful half of this entry.
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
  (compiled, rejected, still cold): see `Jit::tier`. When that was written the
  JIT was roughly neutral on the benchmarks either way, which was its own
  finding at the time; it is not true any more, and the two entries below say
  why -- what was neutral was a tier that could only compile a loop, and could
  not stay inside one for more than 4000 iterations.
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
- **The JIT compiles self recursion, not just self tail calls.** A tail call
  was a loop back-edge and everything else fell to the interpreter, which meant
  the two functions closest to being fast -- `fib` and `collatz_steps` -- were
  the two it refused. `fib (n - 1) + fib (n - 2)` is now a *machine* call, which
  requires the body to be a function of its arguments rather than of a heap
  frame: such a function is emitted twice, an inner body taking its parameters
  by value and an outer entry matching `CompiledFn` that unpacks the frame once.
  `fib 32` went from 593 ms to 45 ms and from **451 MB of allocation to 4.8 KB**
  -- 100% of what it allocated was frames, and a machine call has none. Against
  CPython 3.13 that is 0.20x where it was 2.8x. `collatz` went 1949 ms -> 265 ms.
  What it costs is machine stack, which is fixed where a heap continuation is
  not, so a compiled recursion carries its depth and returns `JIT_DEEP` rather
  than overflow; `enter_function` then runs the call interpreted and stops
  offering the compiled body (`Jit::deoptimize`), because a function that
  recurses deeper than the stack allows does it on every call. `recursion.dr`
  and `runaway.dr` are what hold that line -- `sum_to 100000` and a `blow` that
  never returns both still answer exactly what they answered before.
- **A compiled loop under a nested force yielded on every single iteration.**
  This is the largest thing a wall clock ever hid here. A compiled loop spends a
  reduction per iteration and yields to the interpreter when the budget runs
  out. But a *nested* force -- `force_whnf`, which is what `strict!`, `print!`
  and `to_string` all run -- cannot hand the process back, because the natives
  above it are waiting on the C++ stack, so it spends the budget below zero and
  leaves it there. After 4000 iterations `next > 0` is false for ever and every
  iteration yields: a heap frame allocated, the interpreter re-entered, the
  compiled body re-called. A ten-million-iteration loop took 429 ms and
  allocated 240 MB to do work that takes **43 ms and 122 KB**. So the nested
  loop re-arms the budget and records the slice as spent (`Process::slice_spent`)
  instead, and `run_process` ends the slice when the force returns. Note what
  this means about every JIT number taken before it: "a tail loop is 7x faster
  than CPython", which these notes carried for months, was only true when
  nothing was forcing the loop -- and a program that prints its answer is
  forcing it. It is true now.
- **`strict!` is the native that most needed to vouch.** `VouchesForGc` exists
  because a native running unbounded Dream work underneath itself pins the heap,
  and `strict!` is how a program says "do the whole of this now" -- so the work
  under it is not a detail of one native, it is the program. Without the vouch a
  fold of ten million elements grew a **2.3 GB nursery that no safepoint could
  reach**; with it the same fold holds 300 KB and runs 8-12% faster. The reason
  it could not simply be added is `force_deep`: its map case collected the
  entries into a `std::vector<Value>` and forced them one by one, and a
  collection rewrites what it can see -- which a C++ vector is not. The entries
  wait on the value stack now, like every list `force_deep` already walked there.
- **The VM is a shared library, and that is not free.** Without
  `-fno-semantic-interposition` a compiler must assume any global function in a
  `.so` can be interposed at load time, so every cross-TU call goes through the
  PLT and none of them inline -- and the interpreter's hot path is nothing but
  cross-TU calls. Link-time optimization on top of that was measured and bought
  nothing, so it is not enabled.
- **A compiled function may call another compiled function.** The tier used to
  compile exactly one Dream-level call -- this function calling itself -- and
  refuse any function that made another, which is why a fused `collatz` was
  interpreted while `collatz_steps` beside it was compiled: the loop body is
  `acc + collatz_steps x`, and that call refused the loop. The callee is now
  emitted into the same LLVM module, as a function of its arguments, and called
  with them in registers: no frame, no thunk, no trip through the interpreter.
  `collatz` went **205 ms -> 100 ms**, from 0.30x CPython to 0.15x, and from
  20.8 MB allocated over 317 minor collections to 570 KB over 8: the frame per
  element *was* the allocation, and a machine call has none. The self-compile does
  not move at all (2617-2662 ms before, 2609-2659 after, on the same machine in
  the same minute), which is the expected answer -- `dreams` is not a numeric
  loop anywhere -- and is worth having measured rather than assumed.
  What licenses it is the rule the tier already rested on -- a function is only
  compiled when it forces every parameter on every path -- asked of the callee
  rather than invented for the occasion, so the test is `Analyzer::run` and not
  a second set of rules. `PeerSet` in [dream/src/jit.cpp](dream/src/jit.cpp) is
  where that is written, along with the two things a callee may not be: a
  function that reads a slot no parameter owns (there is no frame for one), and
  a *loop* (a compiled loop stays preemptible by writing its state back to its
  frame, and a callee has none, so a callee that looped would hold its worker
  for as long as the loop ran). Recursion is fine, because the machine stack
  bounds it and `JIT_DEEP` hands the call back when it runs out. Mutual
  recursion works, because every function in the closure is declared before any
  of them is written. One consequence for `--profile`: a callee's reductions are
  charged to the function that called it, the way a native's already are.
- **A `let` is written where its name is read.** A block containing one used to
  refuse the whole function, because a `let` in a compiled body has nowhere to
  put its value: there are no thunks, and the frame's slots are registers. So
  the value is emitted at the *read* instead -- which is exactly where the
  interpreter would have forced the thunk, so nothing moves. A binding nobody
  reads is never evaluated, which is what an unforced thunk comes to; one read
  once runs at the read. What is lost is the thunk's memory of its own answer,
  so a binding read twice runs twice -- allowed only when its value is small and
  contains no call, since a duplicated call could be a duplicated recursion and
  nesting duplications is how a linear body becomes an exponential one.
  `dream/tests/programs/jit_let.dr` is what holds the laziness: a binding whose
  value divides by zero and is never named must not raise, and one named below
  something that raises first must not get there.
- **Integer `/` and `%` are two instructions, not a call.** They were the only
  arithmetic with no inline fast path, on the stated grounds that the zero check
  and the integer/float split were not worth duplicating. Measured, they were: a
  ten-million-iteration loop took 92 ms dividing where the same loop multiplying
  took 48, and the difference was one `dream_rt_arith` call per operation. Both
  are now 46-49 ms. `collatz` pays it on every step for `n % 2` and again on half
  of them for `n / 2`, and `mapfilter`'s guard is `x % 2 == 1`.
- **The interpreter disagreed with itself about one quotient.** Found by writing
  the JIT's division against `arith` and then asking both tiers the awkward
  questions. `-2^62 / -1` is `2^62`, one past the largest fixnum: `arith` falls
  through to the double path and answers a float, and `finish_binary` -- the
  two-fixnums fast path every operator takes first -- built the fixnum anyway,
  which wraps to the *smallest* one. So `x / y` gave `-4611686018427387904` or
  `4.611686018427388e+18` depending on whether both operands happened to be in
  hand at the operator, which is a property of how the argument arrived and not
  of the program. `Op::Add`, `Op::Sub` and `Op::Mul` beside it all checked
  `fixnum_fits`; `Op::Div` did not. It does now, and `Op::Mod` is written out
  separately beside it with the note that a remainder always fits.
- **A compiled function may call a host native.** The tier wrote out five
  numeric natives -- `to_float`, `to_int`, `sqrt`, `abs`, `floor` -- and refused
  the whole function for any other, which is every member of `std.native`. That
  refusal, not the strictness rule, is what kept the tier to loops somebody had
  shaped for it: a lexer reading a byte, a walk over a list through `tail` and
  `is_empty`, an ordering written over `compare` are all arithmetic with one
  call in the middle. Such a call is now made the way the interpreter makes one
  -- the callee value this process already built, the arguments on the process's
  own value stack, the native itself -- so there is one implementation of what a
  native means and both tiers reach it (`run_native` in
  [dream/src/jit_rt.cpp](dream/src/jit_rt.cpp)). `native_site` in
  [dream/src/jit.cpp](dream/src/jit.cpp) is the admission rule and it is four
  questions, of which the first does most of the work:
  - **Is it pure?** Purity is spelling in this language, so this is one look at
    the last character of the name. That single rule is what makes the rest
    simple: nothing has to order an effect against anything, and nothing can
    park -- every native that answers `NativeOutcome::Block` is impure, and so is
    everything reachable from the Dream work a pure one runs underneath itself.
  - **Is it saturated, of fixed arity, and four arguments or fewer?** The call is
    one helper with a fixed signature; a partial application has no callee to
    enter and a variadic native has no argument positions for a mask to describe.
  - **Does it vouch for the collector?** If so it is refused -- see below, which
    is the one measurement that changed the design.
  - **May every argument be produced here?** A position the native's strict mask
    claims is evaluated, which is what the interpreter does a step later anyway.
    A position it does not claim is *read and not evaluated*: a literal, or a
    slot handed over as it stands. That is exactly `thunk_for`, so it is not an
    eager evaluation at all, and anything of another shape refuses the call.

  What it is worth is one new benchmark row, `bytescan` -- three million bytes
  through `core.str_byte` -- which went **454 ms to 59 ms**, from 3.06x CPython
  to **0.41x**. The other six do not move. What it *costs* is at the end of this
  entry, because it is not nothing.
- **A parameter every self call hands back is carried, not evaluated.** Written
  with the above, and without it the above reaches almost nothing. The tier
  admits a function only when every parameter is forced on every path, and
  `f s (i + 1) n (acc + core.str_byte s i)` fails it for `s`: the base case
  answers `acc` and never looks at the string. That refused every loop that
  carries the thing it is walking, which is most of the loops a native call is
  good for.

  The rule was always stronger than the reason behind it. What the strictness is
  for is *eager evaluation*: compiled code evaluates a call's arguments before
  making the call, and doing that to one the callee would never force can raise
  in a program that was going to finish quietly. A parameter every self call
  passes straight back -- slot `i` to slot `i` -- is not evaluated by one. It is
  moved, which is precisely what the interpreter does with it, so it needs no
  licence. `Analyzer::carried` is the set, `Emitter::load_slot_raw` is the read
  that does not force, and `dream/tests/programs/jit_natives.dr` holds the line
  that matters: a loop carrying `1 / 0` it never looks at must not raise.

  Note who still has to be strict in everything. A **peer** is entered by a
  caller that evaluates its arguments, so nothing here is weakened for one. The
  **root** of a compile is entered from a heap frame the interpreter filled, so
  the only eager evaluation in its picture is its own self calls'. The two
  answers differ and `Analyzer::run` says why in the one place it decides.
- **A native that vouches for the collector is one compiled code may not call.**
  The measurement that decided it, and the reason the widening above is not a
  trade of memory for speed. `VouchesForGc` is how a native says "I walk a lazy
  structure underneath myself and I am written to survive a collection while I
  do" -- `str_concat`, `str_of_chars`, `str_of_bytes`, and `strict!`, which is
  impure and was already refused. A *compiled* caller cannot honour that: its
  values are in machine registers, so the call pins the heap however the native
  is written. A loop calling `str_concat` over a 300,000-element lazy list held
  **163 MB of peak heap against 45 MB interpreted**, and collected twice against
  334 times, which is the same failure "`strict!` is the native that most needed
  to vouch" describes from the other side. So `NativeDef::vouches` says which
  they are and the tier declines them. Declining costs nothing that was not
  already being paid -- a function containing one was not compiled at all before
  this -- and false is the safe default: a native wrongly left unmarked costs
  memory in one compiled loop, where one wrongly marked costs only the compile.
- **A wider tier is not free for a program that does not use it.** The
  self-compile got **2.7% slower** -- 2805 ms to 2882 ms, mean of seven
  interleaved rounds, and the new build was slower in all seven, so this is not
  the placement noise the section below describes. It compiles 19 functions where
  it compiled 10, and nine functions at the ~8.5 ms apiece LLVM charges is the
  whole of the difference: run both with the threshold set high enough that
  nothing is compiled and they are 2798 ms against 2823 ms, which *is* noise, and
  with `--no-jit` they are 2800 against 2790. `dreams` does not spend its time in
  the nine -- its own profile is AST walkers that build lists and maps, which
  this tier will never take -- so it pays for them and gets nothing back. The
  image is byte-identical and the reduction count is within 0.2%. `--stats` now
  reports the number of functions compiled from `print_stats` rather than from
  the end of `main`, which is what makes that question askable at all for a tool
  that ends in `os.exit!`.
- **An idle worker is not free, and the default was one per core.** A
  self-compile is one green process, so on a 24-core machine twenty-three
  workers had nothing to do -- and "nothing to do" was a 500us poll that walked
  every *other* worker's queue looking for something to steal, and then walked
  every worker's queue again to ask whether the system had deadlocked. One
  mutex acquisition apiece, both walks, every worker, five hundred microseconds
  apart: O(workers^2) lock operations per poll, contending with the one worker
  that was running the program. Measured across worker counts, the self-compile
  went 2.67 s at `-j 1`, 2.46 s at `-j 4`, and 2.70 s at `-j 24` -- the default
  was the worst setting available. It is now cores capped at 8, which is the
  same compromise `GcPool` already made and for the same reason, and `steal`
  answers "is anything queued anywhere?" from a counter instead of from the
  locks. Together, 3.10 s -> 2.81 s interpreted and 3.22 s -> 2.95 s by default.
  `-j` still means what it said, so a program that really does have
  twenty-four runnable processes still gets twenty-four workers.

  **The deadlock walk beside it cannot be replaced by that counter**, which is
  the part worth carrying away, because it is the obvious next edit and it is
  wrong. `take_local` moves a process out of a queue and into `runnable_` as
  two separate relaxed writes, and what makes the pair safe is that it holds
  the queue lock across both -- a walker blocks on that lock and never observes
  the instant when the process is counted in neither. A lock-free read observes
  exactly that instant and declares a deadlock in a program that is running
  fine. The comment in `take_local` had said so all along. It cost one test to
  find out: `console.dr` under `just test-races`, which failed within a few
  dozen reductions while the rest of the suite passed. If it is ever worth
  removing that walk, the move is a single counter of "queued or running"
  incremented by `enqueue` and decremented after `run_slice` -- one atomic, so
  there is no instant between two of them to observe.

- **`step_eval` is too big for the inliner, and everything it calls pays.** The
  largest single finding of the 2026-09-18 round, and it is about the build
  rather than the code. `step_eval` is one switch over every opcode, which puts
  it far past `large-function-insns` -- past which GCC will not grow a function
  at all. So every helper it calls is outlined *however small*: pushing a
  continuation is a compare and two stores, and `perf` showed it as **6.2% of a
  self-compile** in a symbol of its own. The helpers are not expensive; the one
  function they serve is too big to be allowed to absorb them, so the hot path
  pays a call per operand read and per continuation push. Raising the five
  `--param` ceilings in [dream/CMakeLists.txt](dream/CMakeLists.txt) is worth
  about 4% on its own. The lesson generalizes: in a switch-per-opcode
  interpreter, read the profile for *outlined helpers* before reading it for
  expensive ones, and check `nm`/`perf` rather than assuming `inline` did
  anything.
- **A `std::vector` cannot be appended to without a call.** `ContStack` in
  [dream/src/process.hpp](dream/src/process.hpp) replaced
  `std::vector<Cont> conts` because there is no way to ask a vector for "bump
  the size, I have already checked the capacity" -- so the push stayed a call
  even with the ceilings raised, and needed `always_inline` on top. Note what
  the measurement said about it though: on its own the change is **inside the
  noise floor**, because the work was always the stores rather than the call,
  and inlining relocates work rather than removing it. `step_eval` went 13.7%
  -> 16.4% as `push_back` went 6.2% -> 0. It is kept because it is the right
  shape and because it compounds with the ceilings, not because it was a win by
  itself.
- **A profile that names a symbol has not told you what removing it saves.**
  Said already under "What the profile says and what it costs are different
  questions", re-learnt the hard way here, and worth the repetition: three of
  the four changes in this round were individually indistinguishable from noise
  on a seven-round interleaved A/B, and the round as a whole is 4%. Build the
  before-binary and interleave the runs; a single before-and-after on this
  machine cannot resolve anything smaller than about 3%.
- **A `match` used to pay for every arm it missed.** The arms are tried in
  order, and a tagged-list arm that does not fit -- `[:name, n, sp]` against an
  `:apply` -- is a `type_of`, a cons test, a head read and an equality, about
  a dozen reductions, before the next arm is asked. The compiler is built out
  of matches of forty such arms, which is why ordering `infer`'s arms by
  frequency was once worth 15%. Now a `match` with three or more tags starts
  with `switch_head` / `switch_atom` (docs/bytecode-format.md), which jumps into
  the ordinary chain of arm tests at the first arm that could match that tag.
  A twenty-arm match reaching its last arm went 1645 ms -> 350 ms, and the
  self-compile 176.0M -> 156.0M reductions (-11.4%), 5733 -> 5394 ms on three
  interleaved rounds (-5.9%). It is `dispatch` in
  [dreams/lower.dr](dreams/lower.dr), and two properties make it safe. Every
  target is a suffix of the chain, so each arm is lowered once and a tag arm
  that does not match after all falls through as before. And `switch_head` is
  only emitted where the chain would have forced the cell's head first anyway.
  `dream/tests/programs/match_switch.dr` holds the orderings: a catch-all
  between tags, a head that raises, and a first arm that forces nothing. It
  needs no type: the patterns say which tags they take. The JIT does not
  compile a switch yet (`op_is_supported`), so a function containing one stays
  interpreted.
- **Where the self-compile now stands.** 3.10 s -> **2.32 s** interpreted,
  measured `--no-jit` with default workers against a VM built from the commit
  before any of this. **Do not trust this row as an absolute.** Re-measured
  2026-09-19 on the same machine and a `just vm` build, a self-compile is
  7 s -- and the two numbers were never measuring the same program, since
  `dreams` and `std` have both grown a great deal since. What the row is good
  for is its ratio; what it is not good for is deciding that today's compile
  has regressed. Measure the commit before, in a worktree, with the same
  binary -- `git worktree add /tmp/base HEAD` -- which is how the two
  image-per-call bugs below were found rather than argued about. The order of the wins is the opposite of where the effort
  went: PGO 12%, the inliner ceilings 4%, the scheduler's idle workers 9%, and
  every source-level interpreter change together under 1%.

### A spurious deadlock, and why it hid behind a slow walk

Found 2026-09-18 by `just test-races` failing on `console`, reported against a
process the dump described as `finished mode=halted` after 99 reductions --
which is not what a deadlocked process looks like, and was the clue.

`deadlocked_` is sticky. The detection happened at *startup* and the dump was
printed at exit, after the program had run correctly to completion. The window
is one line of `cli.cpp`:

```
sched.start();      // workers begin looking for work
sched.enqueue(root);
```

Between those two statements the root process is live, nothing is queued or
running, and every worker is idle -- which is precisely the condition the
deadlock check tests. Enqueueing first closes it, and there is no other instant
like it, because every later `enqueue` happens while something is already
running.

Two things kept it hidden. The check only runs when `idle_workers_ + 1 >=
workers_.size()`, which with one worker per core on a 24-core machine almost
never held -- capping the default at 8 made it hold routinely. And the check
used to walk every worker's queue under its lock, which took long enough that
the window had usually closed by the time it finished. Both of those are
accidents, not protections: **the check was always wrong and was being saved by
being slow.**

The walk is gone, and not because of speed. It could not be made correct: a
worker re-enqueueing its process pushes it onto a queue and only then
decrements `runnable_`, so a walk that has already passed that queue goes on to
read `runnable_` after the decrement and concludes that a healthy program is
deadlocked. Two counters cannot be read without observing a transition between
them. `Scheduler::active_` is one counter -- queued *or* running -- incremented
by `enqueue` and decremented when a slice ends without re-enqueueing, so a
slice that hands its process straight back increments before it decrements and
the count never dips. The check is one load.

### The tier's eager arguments can change *which* error a program raises

Found 2026-09-14, confirmed against the VM as it was before any of that day's
work, so it is old and it is not a consequence of anything above. It is written
down because the file header states the rule in a way that reads stronger than
the rule is.

Compiled code evaluates a self call's arguments before making the call, and what
licenses that is the admission test: every parameter is forced on every path, so
an argument evaluated here is one the callee was going to force anyway. That
keeps the guarantee the header claims -- a program that finishes quietly still
finishes quietly. It does **not** keep the argument being forced at the same
*point*, and when more than one error is reachable that decides which one the
program gets:

```
let rec ordered i n acc =
    if i > n { acc } else { ordered (i + 1) n (if i + () > 0 { acc } else { acc }) };
ordered 1 20000 0
```

`acc` is forced on every path, so the loop is compiled. Interpreted, the
accumulator is a chain of twenty thousand suspended `if`s and forcing it
evaluates the outermost condition first, so the error names `integer 20000`.
Compiled, each iteration evaluates its own condition, so the error names
`integer 32` -- the iteration the JIT threshold happened to fall on. Both raise,
both raise a `:type_error`, and the payloads differ. The same argument says a
program whose accumulator *diverges* where an earlier one raises can diverge
under one tier and raise under the other.

Not fixed, and the reason is the benchmark. The rule that would fix it is "the
argument must be forced before anything else that can raise", and `sum`'s fused
loop fails it -- its condition `i >= hi` is a comparison, which can raise a type
error, and it runs before the accumulator is forced. Refusing that would undo
the whole of the fusion result. This is the same trade every lazy language makes
under "imprecise exceptions"; what makes it worth writing down here rather than
shrugging at is that Dream has typed errors and `catch`, so a program *can* look
at which one it got.

### Two collector bugs that presented as a segfault a long way from home

Both were found in 2026-09 by a crash the fuzzer reached and nothing else did,
and both are the same species: a reference the collector could not see or could
not fix. Neither faults where the mistake is, which is what made them expensive.

**A pointer must not span a force.** `force_deep` walks a list in place. The
cell waits on the value stack precisely so that a collection can find it *and
rewrite the reference* -- the object moves. So a `ConsObj*` taken before a force
names the block the cell moved out of, and storing through it writes into
whatever that block was recycled into. `ConsObj::head` and `ThunkObj::node`
share an offset, so the usual shape of the damage is a thunk whose node field is
the low half of a pointer; the machine jumps to it some thousands of reductions
later and dies somewhere unrelated. The cell is now re-read from the stack at
every single use (`cell()` in [dream/src/interp.cpp](dream/src/interp.cpp)), and
the array case beside it does the same.

**An object born old is remembered at birth.** An allocation over
`kMaxClassSize` is born in the old generation, and what its caller fills it with
is young -- `make_array` hands back an array and the caller writes the elements
straight in. That is an old-to-young edge made by no store the write barrier
ever sees, so the next minor collection does not scan the array and frees the
elements it cannot find; a 3000-cell array of unforced thunks comes back holding
whatever the nursery has since put there. Such objects are pushed onto the
remembered set in `alloc_bare` ([dream/src/heap.cpp](dream/src/heap.cpp)). One
entry covers the whole object, and only allocations large enough to tenure pay
it. Nothing can collect between the allocation and the fill -- a collection runs
only at a safepoint, and there is none inside a reduction -- which is what makes
filling one safe without a barrier at every fill site.

The verifier now names the object's type and what it was reached *from* when it
reports a young object surviving a minor, because "a minor collection left a
young object reachable" without a path is a fact you cannot act on.

### Fixed: compiled code forcing a long thunk chain crashed

A JIT-compiled function that forced a long chain of suspensions died with
**SIGSEGV** where the interpreter raised `:stack_overflow`. Found 2026-09-13,
reproduced against the JIT exactly as it was before it learnt self recursion, so
it was never new -- nothing here had looked for it before. Fixed the same day;
`dream/tests/programs/force_chain.dr` is what holds the line.

```
import std.console;
let rec loop_f f i n acc = if i > n { acc } else { loop_f f (i + 1) n (f acc i) };
let main! = { console.print! (loop_f (fn a b -> a + b) 1 10000000 0) };
```

`acc` is lazy, so this builds ten million suspended applications rather than
adding anything -- the trap `list.fold_strict` exists to avoid, and its note in
[mind/std/list.dr](mind/std/list.dr) explains it. Forcing that chain is supposed
to raise at `DREAM_MAX_DEPTH`, and with `--no-jit` it always did, exactly:

```
dream: uncaught error: <error :stack_overflow recursion too deep: 4194305 pending frames>
```

With the JIT it segfaulted. The reason was one line of the machine: the
interpreter forces a slot by pushing a **continuation**, so its recursion is
heap and its limit is a number it can check, while compiled code forces a slot
by *calling* -- `load_slot` -> `force` -> `dream_rt_force` -> `force_whnf`,
which runs a whole nested machine loop, which enters the compiled body again for
the next link. One C++ frame per link of the chain, against an 8 MB thread
stack. The lambda was the whole trigger: `fn a b -> a + b` is arithmetic over
two strict parameters, which is precisely what this tier compiles.

The fix is not the depth argument that bounds compiled *self* recursion, because
the recursion here goes out through the runtime and back in. It is the same move
`Jit::deoptimize` makes, on a different trigger: `Process::force_nest` counts
the nested `force_whnf` loops on the process's machine stack, and
`enter_function` stops offering the compiled tier past
`kMaxForceNestForCompiled` (256). From there the interpreter handles the rest of
the chain, pushing continuations instead of C++ frames, and the walk finishes on
the heap where its limit can be enforced. Interpreted code never nests forces
deeply -- each one returns before the next -- so the bound costs every healthy
program one compare on function entry and nothing else.

The bound is free, which was worth measuring rather than asserting because
`enter_function` runs on every single call: one `uint32_t` load and a compare,
folded into the branch that was already testing whether the JIT exists. The six
benchmark workloads A/B'd against the same build without it on the same machine
came back 44/51/216/30/82/25 ms before and 43/49/217/29/82/25 after -- inside
the placement noise floor in "Two things that will lie to you about a change to
the interpreter" in every row.

What makes the test cheap enough to keep is `force_chain.env`: a small
`DREAM_MAX_DEPTH` and a 200,000-link chain, which is comfortably past the
roughly 16,000 links eight megabytes of stack allowed and finishes in a fraction
of a second. Note that the crash was *not* catchable -- `try!` did not see it --
which is why the e2e harness running every program under both tiers is the right
place for it: what it asserts is that the two tiers agree, and a segfault on one
side is the loudest possible disagreement.

### Measured, and not kept

- **A larger major-GC growth factor.** `gc_threshold_ = live * 3` makes the
  majors a geometric series whose last term dominates, so widening the ratio
  should mean less total marking. At 3, 4, 6 and 10 the self-compile does 7, 7,
  5 and 4 majors -- and the wall clock does not move (2.68 s, 2.75, 2.74, 2.74,
  with 3 nominally best). Peak heap does not improve either. This is the third
  GC policy lever measured here and the third that does nothing; see also the
  nursery note below. Whatever the collector costs this workload, it is not the
  number of collections.
- **`-march=native -mtune=native`.** 2.775 s against 2.737 for the same source
  without it -- nominally *worse*, certainly not better. The interpreter's hot
  loop is pointer chasing and indirect branches; there is no vector width or
  new instruction for it to find. Not worth the non-portable binary.
- **Turning the concurrent mark off** (`DREAM_GC_CONCURRENT=0`): 2.325 s
  against 2.360. Inside the noise floor, so the concurrent mark is neither
  earning its 7.8% of CPU samples nor costing anything measurable in wall
  clock. Left on.

- **The outer loop's run of Return steps, added to the nested loop.**
  `run_process` skips its safepoint over a run of Return steps, which is worth
  having because more than half of all steps are Returns. The nested loop
  (`force_whnf`) has no such run, and since that is the loop a program actually
  lives in, adding one looked free. It is 8% *slower* on a fold -- identical
  reductions, identical collections, identical bytes -- because a Return under a
  fold rarely comes in a run, so the extra loop condition is paid per step and
  the skipped checks are never skipped.
- **A bigger nursery for a program that allocates hard.** A fold that allocates
  4 GB collects tens of thousands of times with the default 64 KB nursery, so a
  floor on it looked obviously right. Measured at 64 KB, 256 KB, 1 MB, 4 MB and
  16 MB, the collection count falls from 70,459 to 276 and the wall clock does
  not move at all (4.0-4.4 s either way). The collector is 2-4% of that
  workload; what it costs is not what it collects.

### Two things that will lie to you about a change to the interpreter

Both of these were found on 2026-09-13, by a change that plainly does less work
and did not finish sooner -- the native arguments above. Neither is about that
change; both are about what a wall clock on this machine can and cannot resolve.

**Code placement moves a benchmark by 3%, in either direction.** `step_eval` is
one switch over every opcode, and adding a case to it re-allocates registers and
re-pads the whole function. Adding the native-argument path moved `fib` by 3% --
`fib`, which never reaches the new code, and whose `thunk_for` is a
byte-for-byte identical instruction stream in both builds. So 3% is the noise
floor for *any* edit to that function, and a CPU-benchmark result smaller than
that is about where the code landed rather than what it does. Diff the
instruction streams before believing one:
`objdump -dC build-dream/lib/libdream.so`.

**The collector's thresholds are geometric, so allocating less can mean marking
more.** `gc_threshold_` is refitted to `live * 3` after every major, so a run's
majors form a geometric series and the last one dominates everything before it.
Allocate 6% less and every crossing moves: on the self-compile the series went
from `60K, 253K, 889K, 3.9M, 13.9M, 41.9M, 111M` live to
`60K, 277K, 1.1M, 4.8M, 19.9M, 68.9M, 143M` -- seven majors either way, 238 MB
marked instead of 172 MB, and the last two majors getting almost no overlap out
of the concurrent mark where the shorter series got 24 ms. That is +3% on the
self-compile's wall clock and +250 ms of helper-thread CPU, and it is phase, not
cost: the same two binaries compiling `lucid` are within 1% of each other, and
the same self-compile with the collector's threads and the JIT out of the
picture (`DREAM_GC_THREADS=1 dream --no-jit -j 1 ...`) is *faster* -- 2% on the
best of seven runs, 1% on their mean, which is about as much as this machine can
resolve. `DREAM_GC_TRACE=1` prints the series, and it is the first thing to look
at when something that does less work takes longer.

### Where the remaining time and memory are, and the plan

Measured 2026-09-12, after the round of work that took the self-compile from
2.78 s to 2.02 s and its peak RSS from 859 MB to 499 MB. Both numbers now have
one dominant cause each, and neither is the collector.

*(Both halves of this section were overtaken on 2026-09-22, and by the same
tool. "Memory is fragmentation" was measured against a live set nobody could
break down -- 57% of it turned out to be map branches nothing could reach but
an unforced value, and removing a third of it moved the wall a large program
hits by 50%. Read "A lazy value stored in a map pins the map it was made in"
before acting on anything below.)*

**Memory is fragmentation, not policy.** The last full collection of a
self-compile finds 182 MB live in 386 MB of blocks, and only 422 of 5848 blocks
are entirely empty -- a 64 KiB block holds around 1300 objects and 45% of them
survive, so no block ever empties and sweeping cannot hand anything back. Four
different trigger policies were tried and all landed within 10% of each other;
`--stats` says why, by printing what was *allocated* at the high-water mark
alongside what was held (88% of it). The only thing that reaches a hole is a
collector that moves: compaction, or the opportunistic evacuation of sparse
blocks. See [docs/gc.md](docs/gc.md), "What is left: the holes".

**Time is the call, not the work.** `--stats` breaks allocation down by kind:
frames 40%, thunks 20%. Three fifths of everything a compile allocates is the
machinery of calling and suspending, not the data -- and the two have traded
places as each round of this work removed thunks and left the frames a larger
share of a smaller total. Counting the thunks by what they suspend, before and
after the two rounds so far:

| suspended node | count | what it is |
|---|---|---|
| `apply` | 8.2M -> 5.1M | an argument that is a function call |
| `get` | 2.6M -> 0.98M | an argument that is `c.[k]` or `c.[k else d]` |
| everything else | 1.2M -> 0.92M | `set`, `global`, `list`, `add`, `block`, `sub` |

`DREAM_PROBE_THUNK=1` with `--stats` is what prints that table: suspensions by
the kind of expression suspended. `--stats` alone says how much of a program's
allocation is thunks; this says what they are *of*, which is the question that
decides what to do about it.

So the order of work, most valuable first:

1. ~~**`get` arguments that cannot fail.**~~ **Done.** An in-range read of an
   array in normal form is a load, and the already-built prefix of a list is a
   pointer chase; `thunk_for` does both instead of suspending them. Neither
   needs a strictness analysis, for the same reason the arithmetic case does
   not: `operand_value` reads an operand only when it is already in normal
   form, and the list walk follows only cells that are already in normal form
   and gives up the instant it would have to force one -- which is what keeps
   an infinite list infinite. The depth is capped at eight because the walk
   runs whether or not the callee looks at the argument. `get` thunks fell from
   2.6M to 0.98M and a self-compile lost 6% of its allocation.
   `dream/tests/programs/lazy_args.dr` holds the line on all of it.
2. **NOT a strictness analysis.** This was the obvious next step and it was
   written, measured and thrown away; the numbers are here so it is not written
   again. Two versions were built against the image at load -- no compiler
   change and no image-format change are needed, because the VM has the whole
   IR -- and both were counted against a self-compile without changing
   behaviour:

   - The **sound** one answers for a single parameter: the one forced before
     anything else that could raise. That restriction is what makes acting on
     it invisible, because a caller evaluating such an argument raises exactly
     the error the callee would have raised, in the same place. It would
     remove **21,271** of the 8.2M `apply` thunks. 0.26%.
   - The **unsound** one is the classic set -- every parameter the body forces
     if nothing raises -- which is what most lazy languages ship under
     "imprecise exceptions" and which Dream cannot, because it has typed errors
     and `catch`. It would remove **44,912**. 0.55%.

   Both are worthless here, and the reason is visible once the thunks are
   attributed to where they are *made* rather than to what they suspend:

   | made at | count | |
   |---|---|---|
   | ~~a strict argument of a **native**~~ | ~~3.09M~~ | **done** -- the mask said so all along |
   | a `let` bound in a block | 2.14M | `let x = f y` |
   | an argument of a saturated call to a known function | 1.48M | the only place an analysis would apply |
   | a list, array or map literal's element | 0.54M | |

   The one worth taking was the first, and it needed no analysis at all: a
   native declares which arguments it forces, and the machine built a Thunk for
   one anyway, which `resume_native` then immediately blackholed, evaluated and
   overwrote. That is done -- see "A native's strict arguments are evaluated,
   not suspended" above. It was worth the 6% of allocation predicted here and
   3.35M of the thunks, which is more than this table's 3.09M because the
   `global`, `list` and `block` shapes turn up in that position too. It was
   worth none of the wall clock, for reasons that are not about the change and
   are written up under "Two things that will lie to you".

   What is left in the table is what an analysis would have to reach, and the
   measurements above say it would reach almost none of it.

3. **Frames, which are now 40%.** **Half done, from the other end.** A frame
   that never escapes its call could live on a stack rather than in the heap.
   That still needs escape analysis in the interpreter and is still the largest
   piece of the three -- but the JIT reaches one important case of it without
   any: a function it compiles has no frame at all, because its slots are
   registers, and now that a self call can be a machine call rather than a loop
   back-edge, a whole recursion can run with no frame anywhere. `fib 32` went
   from 451 MB to 4.8 KB for exactly that reason. What is left is every call the
   JIT cannot take, which is every call that allocates -- see the next section.

### Where it stands against CPython, and what the remaining gap is made of

`benchmark/benchmark/run.sh` runs the seven workloads of `main.dr` against the
transliteration of them in `bench.py`, best of `--repeat` runs each, and prints
the ratio. On this machine, 2026-09-15, best of five against CPython 3.13 (below
1.00x is Dream ahead; "was" is the same measurement before any of the work in
this section, "pre-fusion" is after the JIT work and before deforestation,
"pre-calls" is before the tier learnt to call another compiled function, and
"pre-natives" is before it learnt to call a host native):

| workload | dream | python | ratio | pre-natives | pre-calls | pre-fusion | was |
|---|---|---|---|---|---|---|---|
| `sum` | 42 ms | 310 ms | **0.14x** | 0.15x | 0.15x | 13.0x | 14.3x |
| `fib` | 40 ms | 214 ms | **0.19x** | 0.19x | 0.19x | 0.19x | 2.62x |
| `collatz` | 100 ms | 715 ms | **0.14x** | 0.15x | 0.30x | 0.33x | 2.83x |
| `mapfilter` | 22 ms | 29 ms | **0.77x** | 0.79x | 0.75x | 13.1x | 14.8x |
| `pi` | 28 ms | 230 ms | **0.12x** | 0.13x | 0.13x | 5.8x | 6.55x |
| `bytescan` | 59 ms | 144 ms | **0.41x** | 3.06x | -- | -- | -- |
| `strbuild` | 68 ms | 0.9 ms | 75x | 76x | 70x | 82x | 72x |

Six of the seven are ahead of CPython, and the seventh is a different algorithm
on each side. Read the six with the noise floor in mind: everything but
`bytescan` moved by less than the 3% that "Two things that will lie to you" says
a recompile of `step_eval` is worth on its own, and the `pre-natives` column is
an adjacent run of the same images under a VM built from the commit before.

`bytescan` is the newest row and the one the native-call work is judged by:
three million bytes read through `core.str_byte`, which is a wrapper the
compiler rewrites into `std.native`'s, so the loop body is one native call and
an addition. Before, a single such call refused the function and the whole scan
ran interpreted. Two things about how it is written are worth copying, because
both were found by getting them wrong: its accumulator is guarded (`if acc < 0`)
because otherwise it is three million suspended additions and forcing them runs
the machine out of depth -- the `fold_strict` trap -- and that guard sits
*after* the bounds test, because in front of it there is a path where `i` is
never looked at and the tier cannot admit the function at all.

`mapfilter` is the one row whose number is not about the loop. 500,000 elements
at 22 ms is 44 ns each, where `sum` runs at 5 -- and the difference is that the
JIT spends about **10 ms per function it compiles**, which at this size is most
of the measurement. Timed at ten times the size the per-element cost is 15 ns,
which is the loop's own: an `x % 2` and a guard. Measured by running the same
workload twice in one process, where the second run costs 6 ms against the
first's 25. That 10 ms is not the optimization pipeline -- `O1` and `O2` come
out the same -- so it is instruction selection and object emission, and nothing
short of a cheaper code path through LLJIT would move it.

`strbuild` is the one that went the wrong way, and it is worth knowing why: it
is the workload here whose data is *live* rather than garbage -- a 400,000-cell
list held from one end while a native walks it from the other -- so letting
`strict!` collect (above) buys it a 2 MB smaller peak for 20 ms of copying. The
folds, whose every cell dies the instant it is read, get 8-12% back for the same
change. That is the generational bet losing one hand and winning three. Its 82x
should not be read as a runtime comparison at all, either way: `"hello " * n` in
Python is a single C memcpy loop, and the Dream line beside it builds a
400,000-element list and concatenates it. Same answer, different algorithm.

The decomposition that made the case for fusion is kept here because it is what
predicted the result, and it predicted it closely. Before fusion the split was
two, not six: `fib` and `collatz` were recursion over numbers that the JIT
compiled, and the other four folded over a list `list.range` built one cell at a
time, an order of magnitude behind. Timing the same ten-million-element loop
with one thing added at a time, per element:

| | ns | what the step adds |
|---|---|---|
| a compiled loop | 4.6 | nothing: registers and a back-edge |
| an interpreted loop | 164 | frames, dispatch, the strictness dance |
| + a closure call | +52 | one more frame, entered and returned |
| + a lazy list | +256 | a `range` frame, a cons, a tail thunk, forcing it |

So the lazy list is three fifths of it, and the interpreter is the rest. Neither
is a constant factor anyone can tune away: producing one cons cell per element
costs a frame, a cell and a suspension **because that is what the program says**,
where `for x in range(...)` in CPython costs an increment in C. Closing it needed
one of two things, and both were projects rather than edits:

- ~~**Deforestation**, in `dreams`~~ -- **done**, the next section. A fused
  `sum` runs the top line of that table rather than the bottom one: 4.6 ns an
  element instead of 420, which is the whole of the distance between 13.0x and
  0.15x.
- **A JIT that can allocate.** The tier's scope is "the strict numeric spine"
  and the reason is not ambition, it is the collector: a compiled frame keeps
  its slots in registers the collector cannot find or rewrite, which is what
  `PinsTheHeap` says. So compiled code may not allocate, and until fusion
  removed it, a fold allocated three objects an element. What a compiled loop
  cannot do is *build* -- a list, a string, a map -- because building allocates
  and allocating means the collector will eventually run under it. The plan
  below is the response to this bullet, and it deliberately does **not** walk
  the machine stack with stack maps: a compiled frame is already a root (the
  interpreter put it on the value stack), so the move is to spill slots into
  that frame at every point where the collector can run and read them back
  after. That works with the concurrent collector and its helper threads, which
  a stack-map walk of the running thread's stack would not.

### Deforestation

**What it does.** `fold f acc (list.range 1 n)` built ten million cons cells and
threw each one away a step after building it. It no longer builds any: a
pipeline of `std.list` combinators is recognised as one loop, and the loop is
emitted as a global the JIT can take. `sum` went from 4032 ms to 47 ms and
`mapfilter` from 390 ms to 22 ms, both from behind CPython to ahead of it.

The saving is not the cells. It is what the loop *becomes*:

```
    fold (fn acc x -> acc + x) 0 (list.range 1 (n + 1))
```

fused, with the literal lambda inlined into the loop it now controls, is

```
    let rec loop i hi acc = if i >= hi { acc } else { loop (i + 1) hi (acc + i) };
```

a strict numeric tail loop, which is exactly what the JIT compiles. So the route
from 13x behind to ahead was never "make the list cheaper" but "produce no list,
and hand the JIT something it can already take" -- 4.6 ns an element instead of
420. A fused pipeline reports `list 0%` under `--stats` and `1 functions
compiled`; that pair is the check that it worked, and it is worth making, because
fusing without reaching the JIT is only a 2x.

**Where it lives.** Three files. [dreams/fuse.dr](dreams/fuse.dr) is the
vocabulary -- which member plays which role, the shape of the plan, and the
syntactic rules. [dreams/scope.dr](dreams/scope.dr) decides *whether*, because
only a resolved name can say that the `range` in front of you really is
`std.list`'s and not a parameter, a shadow or another module's; the question
becomes "is this the global index `std.list` gave `range`?", which has one
answer. [dreams/lower.dr](dreams/lower.dr) builds the loop, beside the wrapper
rewriting it resembles.

**What it covers.** Sources `range` and `replicate`; steps `map` and `filter`;
sinks `fold`, `fold_strict`, `sum` and `product`. Everything the original plan
listed beyond that -- `from`, `repeat`, `take`, `zip`, `length`, `any` -- is
*not* implemented, and a member whose loop is not written simply does not fuse:
the call it always compiled to still works. Adding one means writing its loop by
hand in `lower`, which is the price of not having a `build`/`foldr` mechanism.

Shortcut fusion (`foldr`/`build`, as Haskell does it) remains the general
answer if this ever needs to fuse arbitrary producers with arbitrary consumers.
It was not taken: it costs a rewrite-rule mechanism plus a rewrite of
[mind/std/list.dr](mind/std/list.dr) into a shape nobody reading it would
recognise, and it is poor at exactly what this codebase does most -- left folds
and `zip`.

**What makes it sound**, which in a lazy language is most of the work:

- **The list must be read exactly once.** This is the rule that took two goes.
  It was first written as "the list must not be named" -- fuse only where the
  producer is written directly as the consumer's argument -- and that refused
  the shape the pass was built for, because `mapfilter` names its stages:
  `let squares = ..; let odds = ..; fold .. odds`. The reason behind the rule is
  sharing, and sharing needs *two* reads. A binding read once is nobody else's
  list, so its body is written where it is read and the pipeline fuses. See "a
  stage that was given a name" in [dreams/fuse.dr](dreams/fuse.dr). Moving one
  is only sound with all four of: read once in the whole block, that read below
  the binding, that read in the same frame, and nothing the body names rebound
  in between. Three of those four were found by a test failing, and they are in
  `dream/tests/programs/fusion.dr` so they stay found.
- **An impure `let` is a statement, not a binding.** A pure `let` is lazy: it is
  not evaluated where it is written, so writing it elsewhere changes nothing. An
  impure one is forced in the order the block gives. `dreams` writes a payload
  as `let copied = list.fold_strict (fn ok p -> ok && copy_payload! handle ..)
  ..; io.close! handle`, and moving that binding put every write *after* the
  close -- which the runtime answered with `:io_closed`, in the one e2e test
  that builds a payload. Purity is spelling in this language, so the guard is
  one `is_impure_name` over the body.
- **Fuse the spine, not the elements.** `map f xs` suspends `f x`; the fused
  loop still builds that thunk where the original did. Fusing the spine is where
  the saving is anyway -- the cell, the tail thunk and the producer's frame --
  and leaving the elements alone means the rule needs no strictness analysis and
  cannot change what raises.
- **The order and count of effects must not move.** A fold whose function
  performs an effect is not fused at all: a fused loop is a global whose name is
  pure, and the machine would stop sequencing it.
- **A fold that was lazy in its accumulator stays lazy.** `fold` and
  `fold_strict` fuse to the same loop with one difference, and the seed of an
  empty source is handed back unforced.
- **The loop has to reach the JIT**, which is the point. It is emitted as a
  global -- the only callee the JIT recognises as a self call -- with three
  parameters and no captures, so that a `range`'s limit is an argument rather
  than a frame the JIT would have to keep. A fold's function must be a lambda
  written at the call site, because it is inlined; passed by name it would be a
  closure call in the loop body, and a loop that calls a closure is a loop the
  JIT declines. What the loop body *may* now name, which it could not when this
  was written, is another global function: `collatz`'s fused loop is
  `acc + collatz_steps x`, and the tier compiles that call rather than refusing
  the loop over it. A closure is still out -- it is the name that has to be
  resolvable, not the call.

**What it did not reach.** This used to say `pi`. `pi` fuses *and* compiles now:
loops that fuse no longer allocate per element, so the remaining row was a loop
whose *values* were floating point, and `float-unboxin` unboxed the accumulator
-- the last box a fused float loop made was the one it carried across a yield,
once per slice rather than once per element. The honest boundary left is loops
whose values are **objects** -- a list builder, a string fold, a map fold --
because compiled code may not allocate. That is the plan below, not a fusion gap.

Nothing more in `dreams` will move those; they want the JIT work under "A JIT
that can allocate" above.

**When changing it**, the bootstrap is the sharpest test there is: a compiler
that fuses compiles *itself* differently, so `just bootstrap` has to reach a
fixpoint again and the seed has to move with it. Build the seed with the
relative `-L` paths the recipe uses -- an image records them, so an absolute
path bakes your home directory into the checked-in seed.

Judge any change by more than one workload, and read the two traps below first:
on this machine `fib` alone cannot tell a 3% change from where the code landed.

One thing to expect when the JIT compiles a recursive function: **`--stats` will
report far fewer reductions than it used to**, because a compiled self call
spends one where the interpreter spent ten. `fib 32` reports 14.1M against the
interpreter's 70.5M. The two totals `--stats --profile 1` prints still agree
with each other, which is the check that matters.

Always put a timeout on a VM run. A Dream program that diverges does not stop on
its own, and the VM will happily sit there.

### Expanding a macro is a compile, and there used to be one per call

**What it does.** `expand twice n` is answered by *running* the macro, and the
only thing that knows how to run Dream is the VM — so expansion stages the
whole program, compiles it, appends the call as a temporary entry, emits an
image and hands it to `vm.eval_image!`, exactly as `comp!` does. That is a
compile of the program per call, and the compiler had been doing precisely
that: a 10-declaration file cost 1.13 s against 0.30 s for the same file with
the macros written out, and `mind/std/all.dr --test` — whose `std.macros` test
block is forty `expand`s in one declaration — cost **38.2 s**.

Two changes, each removing one of the two O(program) costs:

| | std tests | 10 declarations, one call each |
|---|---|---|
| before | 38.2 s | 1.13 s |
| one snapshot per *declaration*, wrapper linked incrementally | 7.3 s | 1.12 s |
| + one image per *round* rather than per call | 2.05 s | 1.16 s |
| + one snapshot for the whole program | **2.15 s** | **0.53 s** |

The two rows move different workloads, which is the thing to understand before
changing either: **the snapshot is per declaration and the image is per call**,
so a declaration with forty calls in it was paying forty images and one
snapshot, and forty declarations with one call each were paying forty of both.
The same file with no macros at all compiles in 0.27 s, so what is left of the
overhead on the second workload is 26 ms a call where it was 86.

**And that last sentence is how the remaining factor of two hid for a year.**
26 ms a call is what a *ten-declaration* file says, and the cost being
measured is the size of the **program**, not of the file -- so the only
workload in this table that could have shown it is one whose program is big,
and neither of them is. On `dreams` compiling itself, which is a megabyte,
the same overhead is **390 ms a declaration**, and macro expansion was 7 s of
a 13 s compile. Beware a benchmark that holds constant the thing the cost is
proportional to. The row below is the fix.

- **One image per round, not per call** (`evaluate!` in
  [dreams/expand.dr](dreams/expand.dr)). Every call of one declaration is
  compiled against the same snapshot, so the wrapper's body is a *list* of them
  and the arena is serialized once. `emit.to_binary` was 36% of the compile at
  that point and is the part whose cost is the whole program rather than the
  call. A *round* is the nesting of calls and not their number: a macro's
  answer is punched for `expand`s again and those go in the next image, so
  `expand with_ok x .. (expand with_ok y ..)` is two rounds and forty calls
  written side by side are one.
- **One snapshot for the whole program** (`prepare!`, and the retry in
  `expand!`). This is the one that needed an argument rather than a
  rearrangement. The snapshot goes stale the moment a declaration is expanded,
  and a macro *can* demand a declaration that has been — a macro that calls a
  helper whose own body was written with `expand`. What makes reuse safe is
  that going stale is never silent: an unexpanded call is staged as `1 / 0` and
  a macro is pure, so it cannot catch what that raises. So a declaration that
  fails *at all* is expanded a second time against a fresh snapshot, starting
  again from the state before the first attempt so that a failure which is real
  is still reported exactly once. A program whose macros all succeed builds one
  snapshot; one whose macro genuinely fails builds two and fails the same way,
  which is a compile that was not going to finish.
  `dreams/tests/macros.py` holds that line, and the test was checked against a
  build with the retry removed — without it, it fails with
  `macro evaluation failed: the compile-time expression raised`.

**Failure is the slow path, deliberately.** A batch has one entry and one
result, so a raise anywhere in it is one failure for the whole image and says
nothing about which call raised. The alternative — generate a `try` around each
call — means naming `strict!` and `to_string` in the wrapper, and a module is
free to declare a global of either, which would win (a builtin loses to a
global; see [dreams/builtins.dr](dreams/builtins.dr)). So a batch that fails is
thrown away and its calls are run one at a time, where the wrapper *is* the
call and every diagnostic is the one it gave before batching existed.

**What did not change**, and is worth checking after any change here: the image
a macro-using program compiles to. `mind/std/all.dr --test` and the benchmark
above both come out byte-identical to what the pre-change compiler emitted,
except for the `SPAN` entries of generated functions — expansion hands out
offsets from a reserved part of the 32-bit space and the order it hands them
out in moved. Eight bytes of an image of 216,736.

### A compile-time expression is the program with a different entry

The same mistake as the macro one above, in the other pass that runs the
compiler at compile time, and found the same way. `settle_comps!` evaluated
each `comp` by handing `vm.eval_image!` a freshly serialized image of the
whole program -- and every one of those images differs from every other in
exactly one field, the entry point, which the format keeps at a fixed offset
in the header. So the program is serialized once and each expression gets
those four bytes changed (`emit.with_entry`). A self-compile has about twenty
`comp`s and was paying twenty copies of itself for them.

Beside it, `settle_comp_order!` now settles the expressions in dependency
order, so a `comp` may read what another `comp` produced; the image is rebuilt
only when something still waiting depends on the one just settled, which for a
program whose compile-time expressions are independent -- nearly all of them --
means never.

### One image per round of the whole program

**What it does.** The section above got the image down to one per
*declaration* and stopped, and left a note saying the rest would need "an
image format that can be patched rather than rebuilt". It does not. What it
needed was for the round to belong to the program instead of to the
declaration.

The declarations of a program do not depend on one another's expansion: each
is staged against the same snapshot, in which every unexpanded `expand` is
the same `1 / 0`. So their rounds are the same rounds, and a round is one
image for the whole program. `rounds_all!` punches every declaration at once,
groups the calls by module -- a call resolves against the imports of the
module it was *written* in, so the modules cannot share a wrapper -- declares
every module's wrapper together, resolves once, and emits **one** image.
Each module then enters that image at its own wrapper by patching the four
header bytes that say where to start (`emit.with_entry`), which is the patch
the old note thought the format could not take. A program's round count is
the deepest nesting any one declaration has, which is almost always one.

| | dreams self-compile | 40 declarations | 10 declarations |
|---|---|---|---|
| one image per declaration | 13.34 s | 0.88 s | 0.48 s |
| one image per round | **7.06 s** | **0.43 s** | **0.37 s** |

Reductions for the self-compile went 402M -> 206M and `node_bytes` -- writing
node records into an image -- from **30.1% of everything the compiler did** to
7.4%. The gap widens with the number of declarations, which is the shape to
expect: the old cost was declarations times program, the new one is program.

**What licenses it** is the argument `prepare!` already rests on, asked of a
batch rather than of one declaration. A wrapper that needs something this
image does not have raises, because an unexpanded declaration is `1 / 0` and a
macro is pure and cannot catch it -- so it cannot answer *wrongly*, only fail.
A failure means one snapshot could not serve the whole program, and only the
original order can say whether that is staleness or a real error, so the batch
is thrown away and the program is expanded again the old way, declaration by
declaration from the original loader (`sequential!`). A build that was going
to fail fails identically, having spent the extra time only because it was
failing. `dreams/tests/macros.py` is what holds that, and the bootstrap's byte
equality is what holds the batch agreeing with the sequential path.

**Where the rest of it went.** Of the 4.0 s `--time` now charges to `parse` on
a self-compile, about half is parsing and half is the one snapshot -- a full
resolve and lower of the program, which is what "expanding a macro is a
compile" costs and is paid once. `vm.eval_image!` per module is what is left
of the per-call cost, and it is small enough not to show.

### A macro reaches a handful of declarations, not the program

**What it does.** The two sections above got the *image* down to one per round.
What they left was the snapshot: `prepare!` resolved and lowered the whole
program so that a transformer could call anything, and it did that whether the
macro called two helpers or two hundred. On a self-compile that was the whole
compiler -- 2,264 functions -- resolved and lowered a second time so that
eighteen one-line trees could be rewritten.

A transformer does not reach the program. It reaches its own definition and
whatever that names, which is a handful of declarations. `scope.resolve_reachable!`
walks out from this round's wrappers, resolving a body at a time and following
the references that body turned out to have, and resolves nothing else.

| | self-compile | `std --test` | one call, 1600 unrelated declarations |
|---|---|---|---|
| whole-program snapshot | 6768 ms | 3522 ms | tax 2048 ms |
| reachable only | **5238 ms** | **2903 ms** | tax **1059 ms** |

`--time` says where it went: `parse`, which is where expansion lives, is
3859 ms -> **2070 ms** on a self-compile, and the stages after it do not move.
The third column is the macro *tax* in isolation -- the same program with the
`expand` written out, subtracted -- and it halves. It is still linear in the
program, because what is left is the part that is not a body: `declare!`, and a
function record and a stub node for every declaration in the image.

**What "reaches" means is the resolver's answer, not a syntactic one.**
`check_body` already records what every name occurrence resolved to, so an
alias, a renamed selective import, a global that merely *names* a function
without calling it, a capture and a chain two modules deep are all edges, and a
builtin, a host member, a parameter and a local are all correctly not. Nothing
here re-implements name lookup, which is the mistake that would make this
subtly wrong in the cases nobody writes a test for.

**What makes it sound** is that a declaration the walk does not reach still gets
a body, and the body raises. It needs one at all because a global of kind
`function` names a function index and the VM rejects an image whose global names
none (`global names an out-of-range function`) -- so "do not resolve this" has
to be spelled as "resolve something trivial", and the only real question is
what. It is `1 / 0`: the same placeholder an unexpanded `expand` is staged as,
for the same reason. A macro is pure, so it cannot catch it. So an incomplete
reachability answer cannot be silently wrong -- it raises, the batch is thrown
away, and the round is run again with every declaration a root, which is the
whole program (`:whole` in [dreams/expand.dr](dreams/expand.dr), and every
fallback path sets it: `one_by_one!` and `sequential!` both).

**That was measured rather than argued.** A compiler built with the dependency
edge deleted -- a walk that finds *nothing*, so every declaration but the
wrapper is stubbed -- still passes all 47 macro and record cases and still
compiles itself, because every batch fails and every failure falls through. It
is slower and it is not wrong. What it produces differs from the batch path in
18 bytes of 374,456, which are the generated `SPAN` entries the section above
already says the two paths disagree about.

**What to expect of the images.** Byte-identical, everywhere: the bootstrap
reaches a fixpoint in one stage, and `mind/std/all.dr --test` comes out the same
file it came out before. That is the test worth running after any change here,
because expansion deciding differently is exactly the failure this pass could
have.

**One behaviour did change**, and it is an improvement rather than a
compatibility note: an error in a body no macro reaches no longer refuses the
expansion. It used to be reported twice -- once as `cannot compile macro: ...`
at the `expand`, and again as itself by the real compile -- because `run!`
refuses a round whose resolution has any error in it at all, and the resolution
was the whole program. Now the round only sees the bodies it resolved, so the
error is reported once, where it is.

### What a macro call actually costs, phase by phase

The three sections above each removed a whole-program cost from expansion, and
each was justified by one number: what `--time` charged to the `expand` stage.
One number cannot say which of the things a round does is the one still worth
removing -- and the change proposed next for this, a compile-time VM kept
loaded for the session and called rather than an image built and loaded per
round ([dreams/TODO.md](dreams/TODO.md)), is a redesign of two of them. So the
phases charge themselves now, and `dreams --time` prints the breakdown under
the stage table for any program with a macro in it.

What a round does, in order: **discover** the declarations with an `expand` in
them, **stage** the program (every `expand` replaced by `1 / 0`, every `macro`
read as the `let` it is), **snapshot** it (`scope.declare!`), **declare** this
round's wrappers, **resolve** what the transformers reach and stub the rest,
**lower** that to an arena, **emit** it as an image, **run** it on a VM, and
**install** what came back. `discover`, `stage` and `snapshot` are paid once
for the program; the rest once per round, and a round is the *nesting* of macro
calls rather than their number.

`stage` and `snapshot` were charged together as `snapshot` when the table below
was taken, and were split once that pair was 85% of the tax and "which of the
two?" was the only question left. The answer is that staging is 2-4 ms of it
and declaring is all the rest -- see "Declaring a program was mostly a search
for the word `core`" above for what happened next.

| | tax | discover | snapshot | declare | resolve | lower | emit | run | install |
|---|---|---|---|---|---|---|---|---|---|
| `dreams` compiling itself | 379 ms of 4786 | 52 | 49 | 26 | 93 | 100 | 51 | 5 | 3 |
| `std --test` | 392 ms of 2519 | 4 | 30 | 17 | 117 | 144 | 60 | 10 | 10 |
| 1,600 declarations, one call | 306 ms of 2353 | 0 | 124 | 0 | 76 | 79 | 27 | 0 | 0 |
| 3,200 declarations, one call | 587 ms of 4309 | 0 | 227 | 0 | 147 | 169 | 44 | 0 | 0 |

The last two are `dreams/tests/scale.py`'s program, which is a one-line
transformer called once in a program of N unrelated declarations -- the macro
tax in isolation, and the shape a large project has.

**It agrees with the measurement it replaces.** The tax used to be read as the
same program with the `expand` written out, subtracted. Three interleaved pairs
of the 1,600-declaration program: 309, 497 and 382 ms by subtraction, against
316, 429 and 295 ms by the meter. The same number to within what a two-second
compile on this machine can resolve -- and only the meter says where it went.

**Running the macro is not the cost, and neither is serializing the image.**
`run` -- a fresh `Runtime`, `Scheduler` and heap per call, which is the whole
of what `vm.eval_image!` does -- is 5 ms of a self-compile's 379 and 0 ms on
either generated program. `emit` is 7-15%. Together they are **8% of the tax on
a large program**, and they are precisely what a compile-time VM loaded once
and called would remove. Everything else would stay where it is, because a
session still has to be told what the transformer reaches, and being told is
`snapshot`, `resolve` and `lower`.

**The cost is per declaration, and it is the declarations nothing reaches.**
Those three are 83-92% of the tax and every one of them is linear in the size
of the program: double the declarations and the tax goes 306 ms to 587 with
each phase doubling under it. On the generated program a transformer reaches
exactly one declaration, so essentially all of that is what the *other* 3,199
cost -- a name declared, a stub body of `1 / 0` resolved, and that stub lowered
into a function record and its nodes, three thousand times over, so that one
one-line macro can be run. The stub exists because a global of kind `function`
must name a function the image has (see the section above); nothing says it
must name a *different* one for every declaration, and that is the next thing
to try.

**How to read it yourself.** `dreams --time FILE` on anything with a macro. The
meter is off unless a tool asks (`modules.metering`), and the reason is not the
clock reads: a phase is charged around a `strict!`, and in a lazy language
forcing a phase is a decision about *when* it happens. Expansion depends on
that in one place -- a round that refuses every module before running one never
forces the image, and so never builds one -- which a meter that charged the
image would take away. Each reading is taken twice and the second subtracted,
for the reason `main.dr`'s `timed!` gives: forcing a phase walks everything the
phases before it built, and the walk is not this phase's work.

Off, it costs nothing measurable: the same compiler with the meter in it and
the commit before it, alternating on `mind/std/all.dr --test`, came back
2534/2597/2593 ms against 2519/2568/2589 -- the metered build nominally faster
in all three, which is how a difference inside the noise floor looks. Every
image in this repository is byte-identical either way.

### The stub every unreached declaration shares

**What it does.** The section above resolves only what a transformer reaches
and stubs the rest, and the section before that says why a stub has to exist at
all: a global of kind `function` names a function index, and the VM rejects an
image whose global names none. So every declaration the walk did not reach got
a body of `1 / 0` -- *its own*, resolved into its own function record and
lowered into its own nodes, three thousand times over so that one one-line
macro could run.

Nothing says two globals may not name the same function. A `GLOB` is a name, a
kind and a `FUNC` index, and the VM checks only that the index is in range. So
the stub is declared once and every unreached global points at it
(`shared_stub` and the rewrite at the end of `resolve_reachable!` in
[dreams/scope.dr](dreams/scope.dr)), which takes the per-declaration work out of
three phases at once: `check_body` does not run, no function record is built,
and no nodes are lowered or serialized.

`dreams --time`, the meter the section above exists to provide:

| | expand | resolve | lower | emit | the macro's image |
|---|---|---|---|---|---|
| `dreams` compiling itself | 415 -> **242 ms** | 93 -> **25** | 97 -> **24** | 56 -> **29** | 223 KB -> **80 KB** |
| 1,600 declarations, one call | 302 -> **139 ms** | 81 -> **11** | 72 -> **4** | 22 -> **3** | 181 KB -> **48 KB** |
| 3,200 declarations, one call | 546 -> **270 ms** | 137 -> **22** | 130 -> **9** | 45 -> **8** | 351 KB -> **93 KB** |

Those three phases together are 312 ms of a 3,200-declaration program's macro
tax and are now 39 -- **87% of what the change was aimed at**, which is about
what the meter predicted when it was used to reorder
[dreams/TODO.md](dreams/TODO.md). Whole compiles, alternating against the same
VM running the commit before's compiler: the self-compile 4.86/4.86/4.96 s
against **4.62/4.75/4.58**, the 3,200-declaration program 4.37/4.50/4.67
against **4.08/4.23/4.14** -- the new build faster in all six, which is what
puts a 5-9% result outside the placement noise floor that "Two things that will
lie to you" describes.

**The stub is parameterless and pure, whatever the declarations pointing at it
were**, and that is the part to think about before changing it. A pure 0-arity
function is a `GLOBAL_VALUE`, so *naming* one evaluates it. A stub of the
declaration's own arity would hand back a closure instead, and a call of it
with too few arguments would hand back another one -- so a walk that missed an
edge would partially apply something and carry on, where this raises at the
first mention. That is the whole reason the body is a raise, and sharing it
must not quietly weaken it.

**What makes it sound** is unchanged from the section above, because the
soundness never rested on the stubs being distinct. An unreached declaration
still raises when named, a macro is pure and cannot catch what it raises, so an
incomplete reachability answer is still loud rather than wrong; the batch is
thrown away and the round runs again with every declaration a root. What the
sharing changes is only how many function records say so.

**What did not change: the image.** Every image in this repository is
byte-identical -- `dreams`, `lucid`, `mind`, and `mind/std/all.dr --test`,
which is the macro-heavy one -- and so is the 3,200-declaration program's
(877,144 bytes, 26,109 nodes either way). That is the test this change is held
to, and the reason is that nothing *reachable* changes: the stub is only ever
in the image handed to the macro VM and thrown away. The bootstrap reaches a
fixpoint in one stage.

**What it leaves**, which is the next thing to do and is now most of what a
macro costs: `snapshot` -- `scope.declare!`, which walks every declaration for
its *names*. It did not move at all (228 -> 231 ms at 3,200 declarations, 123
-> 120 at 1,600), and with the three phases around it gone it is **85-86% of
the tax** where it was 41-42%. Note what that means about the ordering: the
phase meter said to take the stub first and it was right, but the same meter
now says the next 200 ms are in one phase that nothing here has looked at.

### Declaring a program was mostly a search for the word `core`

**The finding, and it is the whole section in one line:** `scope.declare!` is
about *names* -- it binds them, numbers them and queues the bodies for later --
and it does not read a body anywhere. Except in one place. `declare_core` asked
"does this module mention `core`?", and asked it by walking every node of every
declaration the module has (`modules.mentions_name`, a structural search for
`[:name, "core", _]`). On a 3,200-declaration program that one question was
**four fifths of what declaring the program cost**, and nothing else in
`declare!` came close.

It is asked because `std.core` is available without an import, which is a
deliberate and good thing (see `declare_core`); what was wrong is that the
answer was recomputed from scratch every time anyone wanted it, and a compile
wants it two or three times over:

- the **loader**, to decide whether to open `std.core` at all
  (`modules.needs_core`),
- `scope.resolve!`, through `declare_module`,
- and, in a program with a macro in it, the *snapshot* -- because expansion
  declares the staged program before it resolves what a transformer reaches,
  which is a second whole `declare!`.

So the answer is worked out once, when the module is loaded, and carried on the
module record (`:mentions_core`, set by `modules.loaded`). `declare_core` is
handed it rather than deriving it, and `scope.mentions_core_owned` supplies it
for *effective* items -- which is exact rather than approximate, because
`effective_items` hands over every item of every module a module derives from,
so asking each owner once is the same question as asking each item. It is
still lazy, so a module whose question is settled another way -- `std.core`
itself, a module that binds `core` -- never pays the walk at all.

| | snapshot | macro tax | `resolve` | whole compile |
|---|---|---|---|---|
| 3,200 declarations, one call | 229 -> **51 ms** | 267 -> **99** | 903 -> **757** | 3934 -> **3705** |
| 6,400 declarations, one call | 431 -> **93 ms** | 518 -> **184** | 1785 -> **1441** | 8132 -> **7315** |

Whole compiles, alternating against the same VM running the commit before's
compiler: at 3,200 declarations 4231/4152/4260 ms against **3852/3816/3818**,
at 6,400 8598/8770/8833 against **7901/7919/8047**, and the same program with
the `expand` written out 3924/3795/3861 against **3715/3642/3772** -- the new
build faster in all nine, which is what puts a 4-9% result outside the
placement noise floor. Reductions fell 13.7% (114.5M -> 98.8M at 3,200, 226.5M
-> 195.2M at 6,400) and allocation 5%.

**The self-compile does not move at all** -- 4442/4469/4370/4479 ms against
4500/4380/4395/4616 -- and the reason is the useful half of this entry. The
walk is `list.any`, so it stops at the first declaration that mentions `core`,
and every module of this compiler uses `core` in its first few lines. `dreams`
was paying about 12 microseconds a declaration for it and the generated program
69, for the same walk: **the cost of a search is how far it has to look, and a
codebase that uses the thing it is searching for is the one place the search is
free.** That is also why no profile had ever named it -- `mentions_name` was
10.2% of a compile of the generated program and invisible on the one everybody
runs.

**What makes it sound.** The stored answer is a property of the module's items,
so the only risk is an item list that changes after the module is recorded, and
there is exactly one pass that does that: expansion. `expand.with_items`, which
stages an `expand` as `1 / 0` and reads a `macro` as the `let` it is, can only
*lose* a mention or keep it, so it carries the answer over. `apply_items` and
`expand_item!`, which put a transformer's *answer* back into the module, go
through `modules.with_new_items` instead and forget it -- a macro may hand back
a tree naming `core` where the source never did, and `core` is the one name a
module gets bound without asking. That is a handful of modules re-walked rather
than the program.

`declare_items!` -- session mode -- still asks the item walk, because it
declares a *slice* of one module and the module's stored answer is about all of
it. One statement is not a walk worth saving.

Every image in the repository is byte-identical, `mind/std/all.dr --test`
excepted for a reason that is not this change -- the section below -- and the
bootstrap reaches a fixpoint in one stage.

A gate sits in front of the walk that remains, and it is worth knowing about
before reading `item_mentions_core`: `modules.text_may_mention_core` asks
whether the file's *bytes* contain `core` at all, which is one machine scan and
is exact in the direction that costs -- every `[:name, "core", _]` was lexed
from an identifier spelled `core`, so text without those four bytes cannot
produce one. It only ever skips the walk; the converse (`x.core`, a comment, a
string) is not a use, which is why both callers spell it as `&&`.

**What it leaves.** `snapshot` is 51 ms of a 3,200-declaration compile's 99 ms
tax and 93 of 184 at 6,400: still the largest phase, still linear, and now
actually what its name says -- a name declared, numbered and recorded, about 16
microseconds each. The macro tax as a whole is 2.7% of that compile where it
was 6.8%.

### The image was the calls, and now it is the transformers

**The finding, which is about what an image is for.** Three rounds of work
above got the macro image down from one per call to one per round, and each
time the thing being removed was a *rebuild*. None of them asked why the image
had to be rebuilt at all, and the answer was in `call_body`: a call was turned
into an **expression** -- the macro's name applied to its arguments, each
argument quoted into the code that reconstructs it -- and a wrapper around that
expression was compiled and run. So the arguments were *in* the image. An image
carrying the arguments can answer one round of one program and nothing else,
and that is what made "one image per round" the floor rather than "one image".

The arguments were already values. Quoting them into a program so that running
the program would rebuild them was a way of getting data across a boundary that
had no other way across -- `vm.eval_image!` runs an entry point and takes no
arguments. So the boundary grew one: `vm.open_image!` loads an image and keeps
it, `vm.call_image!` enters `module.member` in it with arguments that cross as
data, `vm.close_image!` frees it (see "Compile-time evaluation" in
[docs/builtins.md](docs/builtins.md)). The image is a **library** now, not a
program written for the occasion.

A wrapper is still declared and is still what the reachability walk starts from
and what the resolver answers "which macro is this?" about -- nothing here
re-implements name lookup, which is the mistake that would make it subtly wrong
in the cases nobody writes a test for. Its body is just the names.

`dreams --time` on `mind/std/all.dr --test`, which is the macro-heavy build --
55 calls in two rounds:

| | declare | resolve | lower | emit | run | tax | image |
|---|---|---|---|---|---|---|---|
| the call, compiled in | 19 ms | 41 | 59 | 32 | 8 | 202 ms | 122 KB |
| the call, passed in | **0 ms** | **31** | **35** | **10** | 13 | **133 ms** | **88 KB** |

and on a self-compile, 14 calls in one round, 194 ms -> **147**, with the image
80 KB -> **63 KB** and the same columns moving: `declare` 24 -> 15, `lower`
22 -> 13, `emit` 27 -> **4**. Whole compiles of the std build, alternating
against the same VM running the committed compiler: 2178/2268/2226 ms against
**2162/2176/2164**, the new build faster in all three -- which is what a 68 ms
saving on a 2.2 s compile should look like, and is below what this machine can
resolve on the self-compile, where the tax is 3% of the whole.

**Read the `run` column, because it is the one that went up.** A batch was one
entry and one result, so a module's calls shared a VM start; a session makes one
call, so the std build does 55 starts where it did 2, and that is 5 ms. It buys
back three times its cost in `emit` alone, and it buys something that is not
milliseconds -- see the next paragraph -- but it is linear in *calls* where
everything it replaced was linear in the *program*, so a program with thousands
of macro calls would want them batched again. The place for that is a
`call_image!` taking a list of calls rather than one: a start is a `Scheduler`,
a worker thread and a process, and the image is already loaded.

**A failing call is now the call that failed.** This is the part worth having
even at equal cost. A batch says only that *something* in it raised, so the
answer was to throw it away and run its calls one at a time, where the wrapper
*was* the call and the diagnostic was the one it gave before batching existed --
which meant compiling the program again to find out which line was wrong. A
session makes one call, so there is nothing to attribute. What survives is the
retry that was never about attribution: a call may fail because the shared
snapshot has gone stale, which only the whole program can rule out, so a failure
is asked once more with every declaration a root (`:whole`) and a failure that
repeats there is the real one and is reported where it is written.
`dreams/tests/macros.py`'s 47 cases are what hold that.

**One thing the snapshot image did not have and now needs.** `lower_program`
links funcs, globals and imports; the module table is built by `link!`, which
only the real compile calls -- so the macro image had no `MODS` section at all.
That did not matter when the entry point was patched into the header by index,
and it is fatal when a member is found by the name of its module and its own:
the image loads, runs, and has no member of any name. `MODS` is the only
statement in the container of which globals are whose, and a global's name is
not unique in one -- two modules may each declare `helper`. See
`invocation_image` in [dreams/expand.dr](dreams/expand.dr).

**What the images did.** `dreams` changes, because its own source did. `lucid`,
`mind` and a generated 3,200-declaration program are byte-identical, and the
bootstrap reaches a fixpoint in one stage. `mind/std/all.dr --test` moves by
**12 bytes**, and they are not the 6 the section below is about: three
`<lambda>` function records' `span_start` and `span_end`, all of them above
2^31, which is the reserved part of the offset space expansion hands generated
code out of. The wrapper is smaller, so it consumes fewer offsets, so everything
generated after it shifts by the same 3,380. Check that the same way it was
checked here -- decode the differing offsets against the section table and the
`FUNC` stride rather than arguing about them.

**Where this leaves `.libdream`.** The session is the VM half of "a transformer
library loaded once for the session and called with syntax values", which
[dreams/TODO.md](dreams/TODO.md) has as the pairing for separate compilation's
smallest version. The half that is left is the *roots*: this image is still
lowered from the staged whole program, so it still costs a name declared and a
global emitted per declaration, and it is still thrown away at the end of the
round. Making it a function of the dependency package rather than of the
program is what makes it cacheable, and that is where the remaining tax is --
not in the calls, which now cost nothing to make. *(The roots moved two days
later, and the image is one per compile rather than one per round -- "One image
for the whole expansion" below, which also prices what keeping it across builds
would be worth.)*

**And one thing that was found by measuring rather than looked for.**
`discover` -- "is there anything to expand?" -- is now the *largest* phase of a
self-compile's macro tax, 48 ms of 147, where `snapshot` is 25. It is **0 ms on
a 3,200-declaration generated program**, which is the opposite shape to
everything else in these notes: the lexer records where the word `expand`
appears, so a module with no sites costs nothing at all, and what is left is
per item of a module that *has* one. `dreams` writes its macro calls in
`lower.dr` and `scope.dr`, its two largest modules, so it pays for all of both
to find eighteen one-line calls. The section below is what that turned out to
be.

### Discovery was a walk of two whole modules, because a `match` lied about where it ended

**The finding, and it is not about expansion at all.** The section above ends
by naming `discover` as the largest phase of a self-compile's macro tax -- 49 ms
of 152 -- and by saying the shape of it: the lexer records where the word
`expand` appears, so a module with no offsets costs nothing, and a module that
has one costs a walk of every declaration whose span an offset falls inside.
That is cheap when the offsets can be matched to declarations. When they cannot,
`work_of` falls back to walking every node of every declaration in the module,
and on a self-compile it was falling back for the two largest modules in the
compiler.

It was falling back because of a parser bug three years older than any of this.
`parse_match` ended a `match`'s span at `peek_span rest2` -- the first token
*after* the `{`, not the `}` that `match_arms` had consumed. So

```
let rec lower_expr e l =
    match e {
```

is a declaration whose span is those two lines and stops: `[16170, 16218]` for
a body running to 23,244. A declaration's span is `let` to the end of its body
(`ast.span_to sp (ast.span body)`), so every `let f = match ..` in this
codebase covered its header and nothing else. Two of `lower.dr`'s nine `expand`
offsets were inside such a declaration, the two-way check that guards the
picking therefore failed, and all 124 of its declarations were walked. The fix
is two lines: `match_arms` hands its closing brace back, exactly as
`comma_list` beside it already did and for the reason its comment already gave.

The second half was the same question asked of the wrong thing. An offset is a
position in a *file*, and a file is several modules -- a `mod name { .. }` and
every `group`/`struct`/`mapping` are hoisted into modules of their own that keep
the source they were written in. So a record submodule is handed its parent's
offsets, accounts for none of them, and walks all of its own declarations to
find expansions it does not have. Six of a self-compile's eleven modules with
offsets were record submodules doing exactly that. The accounting is pooled per
source now, which is the question the offsets actually pose.

| | discover | macro tax |
|---|---|---|
| before | 49 ms, 2.5 M reductions | 152 ms |
| the `match` span | 17 ms, 0.8 M | 119 ms |
| + accounting per file | **14 ms, 0.6 M** | **116 ms** |

`lucid`, which imports this compiler, moves the same way: 51 ms -> **14**, and
its macro tax 167 -> **133**. `mind/std/all.dr --test` does not move at all
(3-4 ms either way), and the reason is worth keeping: in `--test` mode
`std.macros`'s `when test` blocks survive, so its 55 offsets land in
declarations and it never fell back. The whole self-compile does not move
either -- 4387/4503/4481 ms against 4482/4082/4489, interleaved -- because
36 ms is 0.8% of it and the floor on this machine is 3%.

**What is left, and why it is left.** One module still falls back:
`std.macros` compiled *without* `--test`, whose 55 offsets are all inside
`when test` blocks the configuration dropped, so they are in the file and in no
surviving declaration. Measured by forcing the accounting true and rebuilding:
3 ms of the 14, 0.2 M reductions. Telling "dropped by `when`" from "the span
logic is broken" would need the loader to carry the spans it dropped, and 3 ms
does not buy that.

**What the images did.** Every one of them changed, in one field. `mind`,
`lucid` and `mind/std/all.dr --test` come out the same size with the same node
and function counts, differing in 63, 496 and 142 bytes -- and every one of
those bytes is offset 24 of a `FUNC` record, which is `span_end`. 39 of `mind`'s
505 functions, 284 of `lucid`'s 2,630 and 97 of the std build's 2,010 had a span
that stopped at their first arm. Check it that way rather than arguing about
it: decode the differing offsets against the section table and the record
stride. The bootstrap reaches a fixpoint in one stage, and the seed had to move
with it because a compiler that spans a `match` correctly compiles *itself*
with different spans.

**And the reason no profile had named it.** `--profile` counts reductions and
this was 2.5 M of 58 M, spread across `has_expansion` -- under 5%, in a function
that is *supposed* to walk syntax. What said it was wrong was not a profile but
printing what the pass decided: eleven modules with offsets, of which eight
could not account for their own. A pass with a fallback should be asked how
often it takes it, and `work_of` had been taking it for a year.

**One thing this deletes elsewhere.** `lucid/analysis.dr` had a `reach` walk --
follow the last part of a lambda, an `if`, an application, a `let`, a `match`'s
arms, and take the largest end -- built because "a cursor in a match arm looks
as though it is in no `match` at all", at a cost its comment records as eight
failing tests. That was this bug seen from the editor's end. The walk is gone
and `holds` asks the span, with the tests that found it kept exactly where they
were: they now hold the parser instead.

### One image for the whole expansion, and what it cost to buy that

**What it does.** The section above got the image down to one per *round* and
ended by naming what was left: the roots. The image was resolved from the
round's **wrappers**, so what was in it was whichever transformers this round
happened to call -- a function of the program, and rebuilt for the next round.
It is resolved from the **transformers** now (`expand.transformer_roots`), so
every round of a program shares one image and there is one per compile rather
than one per round.

Two things had to move for that. `scope.resolve_reachable!` took an *index* --
"the bodies queued after this point", which is how "the wrappers I just
declared" was spelled -- and takes a set of `root_key`s, a source index and an
offset, because a root is now named by where it was written. And the wrapper
itself had to get out of the image: it was declared as a global and resolved
with the transformers, which is a few nodes but is what made the image this
round's. `scope.resolve_names!` resolves it on its own and the caller keeps
`refs` and `r_diags` and throws the rest away, so nothing is declared and
nothing is lowered for it. It is still the thing the resolver answers "which
macro is this?" about, which is the one question expansion must not answer for
itself.

`dreams --time` on `mind/std/all.dr --test`, which is the macro-heavy build --
55 calls in two rounds -- and on a self-compile, which is 14 calls in one:

| | sites | resolve | lower | emit | tax | image |
|---|---|---|---|---|---|---|
| `std --test`, per round | -- | 32 ms | 36 | 11 | 134 ms | 2 of 88 KB |
| `std --test`, per program | 7 ms | **8** | **30** | **5** | **107 ms** | **1 of 51 KB** |
| self-compile, per round | -- | 19 ms | 12 | 4 | 110 ms | 1 of 63 KB |
| self-compile, per program | 19 ms | **7** | 28 | 7 | **118 ms** | 1 of 79 KB |

**Read the second pair, because it is the one that went the wrong way.** A
program with one round has no second round to amortize anything over, so all it
sees is the new cost: the image now holds every transformer in the program
whether or not this compile calls it, and lowering one is not free. The
self-compile is **8 ms worse** and its image is 16 KB bigger.

**What a root costs, measured on the axis that exists for it.**
`scale.py --macros N` writes a dependency declaring N transformers of which one
is called, which is the shape a package of macros has. Against the same VM,
the compiler before this change and the compiler after:

| transformers declared | 1 | 16 | 32 | 64 | 128 | 256 |
|---|---|---|---|---|---|---|
| roots are the calls | 23 ms | 21 | -- | 17 | 22 | 25 |
| roots are the transformers | 19 ms | 28 | 34 | 67 | 100 | **159** |

Flat against linear, at about **0.5 ms per trivial transformer**. That is not a
per-root overhead and it was worth checking rather than assuming: a syntax
transformer's body is a syntax *literal*, which is fifty-odd nodes, and the
same run lowers the whole program's 16,919 nodes in 238 ms -- 14 microseconds a
node, against the 12 these come out at. A root costs exactly its own body, and
what the change did was stop declining to lower it.

**So this is a down-payment, and it is written down as one.** What it buys
today is the std build's 30 ms, which is real but is only the second round not
paying for the first. What it is *for* is that an image which is a function of
the program's transformers is the same image on every build, and so is the
thing a build could keep -- where an image that is a function of the round can
only ever be built again. Until it is kept between builds, a one-round program
pays 8 ms and gets nothing back.

**What keeping it would be worth, so the next person does not have to guess.**
The phases a cache removes are `resolve` + `lower` + `emit`: **42 ms of a
4,286 ms self-compile (1%)**, 43 ms of 2,200 on the std build (2%), and 145 ms
of 1,129 on a 256-transformer program (**13%**). That is the whole case for
caching, and it says what kind of project it is for -- not this one. The rest of
a self-compile's 118 ms tax is `discover` 14, `stage` 15, `snapshot` 21 and
`sites` 19, none of which a cached image touches, because they are the *calling*
program being staged, declared and asked what its own call sites mean.

**One row is new and one moved into it.** `sites` is `resolve_names!`, the
wrapper's own resolution, and it is 19 ms on a self-compile against 0 on a
generated program. Most of that is not the fourteen names: it is forcing the
environments of the four modules they are written in, which `resolve` used to be
charged for because the wrappers were resolved with everything else. The pair is
26 ms where it was 38.

**What did not change.** The images: `dreams`, `lucid`, `mind` and
`mind/std/all.dr --test` are all four byte-identical to what the previous
compiler emitted -- including the std build, which the section above had to
excuse for twelve generated-span bytes and this one does not -- and the
bootstrap reaches a fixpoint in one stage. And the diagnostics, which is the
half worth checking deliberately, because resolving every transformer rather
than the called one moves where a broken macro is first noticed. Three shapes,
before and after, character for character the same: a macro that is never called
and names something that does not exist (reported once, by the real compile, at
the name), one that *is* called and names it (reported twice, as it always was
-- once against the `expand` and once at the name), and one that raises when it
runs.

### `mind/std/all.dr --test` is not byte-stable across compiler changes

Worth knowing before the next person spends an hour on it, because several
sections above tell you to check byte equality after a compiler change and this
is the one image that can move without anything being wrong.

`mind/std/all.dr` contains `let compiled_options = comp cli.parse_as
option_schema options_for_types [..]`, and the value of that is a *map* --
`%{ :values => .., :rest => .. }`, from `cli.parsed`. A compile-time expression
is evaluated by running it on a VM and quoting the answer back into the image,
and a map is quoted in the order its entries come out, which is by runtime atom
identity. **Runtime atom ids are per process, and the process is the compiler.**
`:values` is an atom the compiler itself has (number 96 of its own 345);
`:rest` is not, so in the VM that runs the `comp` it is interned fresh, past
the compiler's own atoms -- and it moves when the compiler does.

So the two entries can swap, and the image differs by **6 bytes of 374,456**,
all of them inside that one `comp`. Measured, so that it is not guessed at
again:

- it is stable across runs, across `-j`, across `DREAM_GC_THREADS`,
  `DREAM_GC_CONCURRENT` and `DREAM_MAX_HEAP` -- this is not a race;
- it is decided by the compiler image. A one-line `let x = :zzz;` added to
  `dreams/config.dr` and nothing else flips it, and so does a one-line
  `let x = "zzz";`, and so did the change in the section above -- **and all
  three produce the same image as each other**, which is the check to apply.

So when byte equality is the test, `dreams`, `lucid`, `mind` and a generated
program are the ones that answer it, and a 6-byte difference in the `std --test`
image at offset 289,565 is this and not a behaviour change. Confirm it the way
it was confirmed here: diff against a build with an unrelated one-line addition
rather than against the seed.

### The compiler was quadratic in the size of the program

**How it was found**, because the method is the transferable part. Everything in
*Making it faster* above was found by asking "what is expensive?" of one
program. That question cannot distinguish a large linear cost from a small
quadratic one, and a quadratic is the only kind that decides whether a big
project compiles at all. The question that can is **"what grows faster than the
input?"** -- run the same profile at two sizes and divide:

```
dream --stats --profile 12 build/dreams.dream -L mind -L proj -o /tmp/o.dream proj/main.dr
```

on a generated program of N and of 2N declarations. Anything near 2.0x is
linear and can be ignored however large it is; anything near 4.0x is the bug,
however small it is today. Four of them turned up in one sitting, and three had
been there for the whole life of the compiler.

| declarations | before | | after | |
|---|---|---|---|---|
| 400 | 1048 ms | 84 MB | **971 ms** | **53 MB** |
| 1,600 | 4671 ms | 600 MB | **2685 ms** | **247 MB** |
| 3,200 | 13144 ms | 1927 MB | **4775 ms** | **647 MB** |
| 6,400 | 40158 ms | 6680 MB | **9848 ms** | **1660 MB** |

(Peak live heap, `--stats`. Every image is byte-identical before and after, at
every size.) Time was 2.8x-3.1x per doubling and is now 1.8x-2.1x: **linear**.
Memory is 2.5x-2.6x per doubling, so it is better by 4x and still not linear --
what is left is under "What is still superlinear" below.

The self-compile moves from 5.1 s to 4.8 s and that is the whole of what this
is worth on *this* repository, which is the point worth taking away: `dreams`
has fifty modules of a hundred-odd declarations each, and n^2 on a hundred is
nothing. **A quadratic that this codebase cannot feel is still the reason a
large project would not build.** Generate the big program; do not wait for
someone to write one.

**1. A list searched more often than it is built wants to be a map** -- for the
fourth time in these notes, and the previous three are above. `collect_let`
gathers a module's `let`s and asks, per declaration, "have I got this name
already?" It asked a list, with a `list.map` that built an intermediate of
everything collected so far and a `list.append` that copied it. Gathering one
module was O(M^2) in the module's size. It is a `mapping Gather` now -- entries
by position, a slot per name, and the positions are what keep it ordered while
the map answers the question. A name redeclared by a module that derives this
one still replaces the earlier one *where it stood*, which is what the image's
global numbering rests on, and there are four cases in `scope.dr`'s `when test`
holding exactly that.

**2. `list.append xs [x]` is one reduction and a copy of `xs`.** This is the
big one, and it is the reason the first question is the wrong one. Appending is
an opcode -- `xs + ys`, a single machine walk, which is why "a linear walk the
machine can do is worth ten of the same walk in Dream" above recommends it --
so a `--profile` charges one reduction for copying a list of any length. An
accumulator built this way is therefore **invisible in a reduction profile and
quadratic in bytes**. `record_global`, `add_pending` and `check_body`'s queue of
bodies were 282 MB, 281 MB and 295 MB of a 3,200-declaration compile against
80 MB, 80 MB and 87 MB at half the size; the three `link_*` folds in
[dreams/lower.dr](dreams/lower.dr) were another 39% of everything allocated.
All are `core.cons` onto a reversed list now, with one `list.reverse` in the
accessor. Where a caller only wanted the length, it asks for a count and the
ordered list is never built (`scope.pending_count`, `bodies_count`,
`s_global_recs_count`).

**3. Deforestation's invented globals were the same mistake**, and the workload
that exposed it is the one to keep in mind: a program whose every declaration
folds over a range invents a fused loop per declaration, so `take_fused_global`
appended to a list as long as the program, once per declaration.

**4. Declaring a program was cubic in its module count.** The other axis, and
the one nothing here had ever varied: every measurement above changes the number
of *declarations*. `set_env` replaced one module's environment by rebuilding the
whole list with a `list.nth` per element -- O(modules^2) reductions -- and it is
called several times per module. It is `(envs s).[mi => e]` now, which is the
`set` opcode and one machine walk. On 3,200 declarations spread over 400 modules
that is 6744 ms -> **4880 ms**, and the macro tax alone 1505 ms -> **718 ms**.

**Generating the program is now one command**, because the advice above is
useless without it:

```
dreams/tests/scale.py --sizes 800 1600 3200               # declarations
dreams/tests/scale.py --sizes 3200 --modules 400          # modules
```

It prints the ratio between consecutive sizes for time and for peak heap, which
is the number to read. It is not in `just test`: it takes minutes.

**What is still superlinear**, measured and left. Peak heap is ~2.1x per
doubling of the *declarations* and time ~1.9x, so both are close to linear but
neither is quite there, and the compiler is lazy end to end, so every stage's
intermediate is retained by the stage that reads it: source, tokens, syntax,
effective items, the staged copy, the resolver's tables and the arena are all
live at once. That one is not a bug and cannot be tuned away -- it is the
argument for compiling a package at a time, which is what
[dreams/TODO.md](dreams/TODO.md) calls for.

*(Those last two sentences were written without being able to see what the live
set was made of, and they are wrong in an instructive way. A third of that peak
was not any stage's intermediate -- it was versions of the arena retained by
values nobody had forced, and it did tune away. "A lazy value stored in a map
pins the map it was made in" is what happened once the question became
askable.)* The *modules* axis is the next
section, and it was the one nobody had varied far enough.

### A table indexed by module wants to be a map, not a list

**The same finding as all four above**, on the axis they did not vary: this
section's benchmark changes the number of declarations, and `scale.py
--modules` changes how many modules they are spread over. Varied to 3,200
modules, peak heap was 3.4x per doubling -- which is what the declarations axis
looked like before any of the work above.

`envs` is the resolver's table of what each module binds, keyed by module
index, and it was a list. Two things followed, and the second is the one that
mattered:

- **Reading it was O(modules).** `env_at s mi`, `ctx_env` and `env_of` were
  each a `list.nth`, and one of those runs per name resolved. This is the half
  everyone notices and it is the half that is nearly free: `xs.[n else ()]` is
  the `get` opcode, a machine walk of one pointer chase per element, so 3,200
  declarations over 800 modules is about 2.5M chases and no measurable time.
- **Writing it allocated O(modules).** `set_env` was `(envs s).[mi => e]`, the
  `set` opcode -- one reduction, so a `--profile` said nothing -- and
  `list_set` builds a fresh cell for every element in front of the one it
  replaces. There is one `set_env` per declaration, so a compile allocated
  O(declarations x modules) cons cells for nothing. That is exactly the shape
  "`list.append xs [x]` is one reduction and a copy of `xs`" describes, and it
  had been hiding behind the same silence.

It is a map keyed by module index now. A put rebuilds the path to one leaf and
shares the rest, a read is a lookup, and nothing wanted the order -- every
reader was already indexing it, which is the whole argument. `has_env` replaced
the `mi < list.length (envs s)` guards, which were asking "has this module been
declared yet?" in the only spelling a list had.

Peak live heap, 3,200 declarations spread over N modules, `--max-heap` 8 GB so
that both finish:

| modules | before | after | |
|---|---|---|---|
| 200 | 354 MB | 359 MB | the map costs a little more when it cannot pay off |
| 400 | 384 MB | 360 MB | |
| 800 | 439 MB | 373 MB | |
| 1,600 | 575 MB | 360 MB | |
| 3,200 | **1967 MB** | **594 MB** | -70% |

**What that is worth is not the megabytes, it is the wall.** Under the *default*
1 GB `DREAM_MAX_HEAP`, the 3,200-module program did not compile at all -- it
died with `process heap grew past 1073741824 bytes`. It compiles now. The old
compiler's wall was between 2,400 and 3,200 modules; nothing here moved the
declarations axis, whose wall is still the 4,000-5,000 this section names.

Time is the small half and says so: 8371 ms -> 7322 ms at 3,200 modules, 6114
-> 5665 at 1,600, and nothing outside the noise floor below that. The
self-compile does not move at all -- 4890/4948/4867 ms against 4678/4882/4698,
9 majors either way with the same live series under `DREAM_GC_TRACE=1` -- which
is the expected answer for a repository of fifty modules, and is once again a
quadratic this codebase is too small to feel. Every image is byte-identical --
`dreams`, `lucid`, `mind` and the generated programs at every size -- and the
bootstrap reaches a fixpoint in one stage.

**What was next on this axis, measured.** `--stats` reports allocation by kind,
and the list share climbed where nothing else did: 9%, 10%, 15%, **24%** at 400,
800, 1,600 and 3,200 modules -- 212 MB, 250 MB, 406 MB, 972 MB, which is 2.4x
per doubling at the end. The named candidate was `modules.modules`, the same
table in the loader rather than in the resolver. It was one of three, and the
next section is what happened when all three were taken.

### The three lists the loader still grew one entry at a time

**What it does.** `modules.modules` was the obvious one and the section above
names it, so it was taken first, on its own, and measured on its own: **5.7%
of a 3,200-module compile's allocation, three points of the list share, and
nothing at all of the peak heap or the wall clock.** That is the whole of what
the predicted fix was worth, and the prediction had been that it was most of
the 24%.

The other 18% was two more tables of exactly the same shape, both found by
asking where the bytes actually went rather than by reading the earlier note
again:

- **`modules.files`**, the source text of every file, appended to per file --
  so loading a program copied the list once per module. It is also the table
  `expand.install!` rewrites one entry of, with the `set` opcode, once per
  macro call, which is the same `list_set` per expansion the `envs` note
  describes. Keyed by index now, with `file_at` for the readers that hold a
  diagnostic's source index (all of them but one) and a derived ordered list
  for `diag.render_all`, which is handed the whole program once at the end of
  a compile that has something to say.
- **`scope.module_recs`**, one record per module, appended to in
  `declare_module`. It is the fourth field of that state to become a reversed
  accumulator, beside the imports, the global records and the queue of bodies
  -- the note at the head of `scope.dr` already explained why, for three.

Two smaller things went with them. `expand.work_of` runs on **every** compile,
macros or not -- it is how "is there anything to expand?" is answered -- and it
walked each module's declarations with a `list.nth i items` inside a fold over
`0..length`, which is a walk per declaration. It carries the index alongside
the accumulator now. And `expand.replace_module`, the `list_set` the section
above names, is a map put.

Peak live heap and total allocation, 3,200 declarations spread over N modules,
`--max-heap` 8 GB:

| modules | allocated | | peak live | | list share | |
|---|---|---|---|---|---|---|
| 400 | 2.36 GB | 2.35 GB | 372 MB | 364 MB | 9% | 8% |
| 800 | 2.50 GB | 2.46 GB | 390 MB | 385 MB | 10% | 9% |
| 1,600 | 2.90 GB | **2.73 GB** | 387 MB | 393 MB | 14% | 10% |
| 3,200 | 4.05 GB | **3.34 GB** | 623 MB | **409 MB** | 24% | 13% |

**Peak heap on the modules axis is now flat** -- 364, 385, 393, 409 MB across
an eightfold spread of modules, against 372, 390, 387, **623**. The bytes
promoted at 3,200 fell with it, 1.12 GB to 758 MB, and that is the mechanism:
an appended spine survives the minor collection that catches it, so a table
grown one entry at a time is not merely churn, it is churn the nursery hands
to the old generation. Time is the small half again and says so: at 3,200
modules 7172 ms -> 6816 ms with a macro and 6256 -> 5648 without, and nothing
outside the noise floor below 1,600.

The **declarations** axis is unmoved in memory (646 MB against 655 at 3,200)
and a little faster (4831 ms against 5359 with a macro, 4096 against 4437
without). That is the expected answer and it is the one worth remembering: the
declarations axis's peak heap is the lazy pipeline holding every stage's
intermediate at once, which is the argument for compiling a package at a time
and is not a quadratic anybody can delete. The **self-compile does not move**
-- 4969/4922/4916 ms against 4912/4924/4861, alternating on the same machine
-- because fifty modules is not a number any of this can be felt at. Every
image is byte-identical, `mind` included, and the bootstrap reaches a fixpoint
in one stage.

**One trap, which cost the only debugging in this round.** The loader's record
still said `:files => []` after `add_file` had been rewritten to put into a
map, and `[] .[0 => f]` is a list set of an empty list -- but the field is
built lazily, so nothing raised until something *read* the table. Ordinary
compiles never do: a program with no diagnostics never looks at its own source
text. Only macro expansion reads it, in `install!`, so the whole test suite's
non-macro half passed and every macro program died with
`index 0 is past the end of a list of 0` and no span. **When a field changes
shape, the initializer is the thing to check first, and a lazy field will not
tell you that you missed it where you missed it.**

### Sharing the arena

**What it does.** `lower` emits a node wherever the source says one and never
asks whether it has emitted that node before. It has, constantly: `local 3` is
one 16-byte record and a self-compile writes ten thousand of them, `core.head`
as a callee is a `builtin` beside an `apply` and there are seven thousand of
those, and a condition written the same way in two modules is two identical
subtrees. [dreams/opt.dr](dreams/opt.dr) rebuilds the finished arena once,
bottom up, into a fresh one that keeps a table of every record it has already
written. On the three programs in this repository:

| | nodes | image | |
|---|---|---|---|
| `dreams` | 67,306 -> **19,323** | 1.29 MB -> **474 KB** | -63% |
| `lucid` | 52,157 -> **15,317** | 1.00 MB -> **373 KB** | -63% |
| `mind` | 10,715 -> **4,129** | 219 KB -> **107 KB** | -51% |

The sharing compounds because the children are rebuilt first: two subtrees that
mean the same thing arrive at the table with the same operands and so are the
same record. Flat, only 47% of the arena is duplicate records; bottom up it is
71%. The same walk answers two more questions for free, which is why they are
not passes of their own -- it starts from the function bodies, so a node nothing
reaches is never copied (wrapper lowering leaves these behind by the hundred),
and once the nodes are renumbered two runs of children holding the same indices
are one run, which is half the kids pool.

**What makes it sound** is that a node is a function of the frame it is
evaluated against and of nothing else. Sharing one between two functions makes
them share nothing at run time: each forces it against its own frame, and
`local 3` means "slot 3 of whoever is asking". Memoization belongs to the thunk,
which is a pair of a node and a frame, so two sites that share a node still get
a thunk each. Three things follow, and all three are in `opt.dr`'s `when test`
block so they stay found:

- **A flag is part of the record.** `strict`, `impure` and `tail` are the
  compiler's answer to "when does this run?", so two nodes differing only in one
  are two nodes. This is also why the pass cannot live in `ir.push_node`, which
  would be strictly cheaper -- there is no second walk and no memo. Flags are
  set *after* emission (`ir.set_node_flag`: tail position and impurity are
  discovered later), and a node shared at push time would hand a flag it
  acquired later to everyone sharing it. The pass runs on the *finished* arena
  for exactly this reason, and after `settle_comps!` besides, because a
  compile-time expression overwrites its placeholder.
- **An operand is not always an edge.** `a` is another node in `force`, a frame
  slot in `bind`, a function index in `closure`, a constant index in `int` and a
  Unicode scalar in `char`. The two tables at the top of `opt.dr` are written
  out per opcode rather than inferred, because getting one wrong is silent in
  the worst way: the image still loads, the section table still adds up, and
  some program reads a slot number as a node index.
- **Captures share the kids pool.** `place_captures` puts a function's capture
  descriptors in the same pool as call arguments, so a pass that rebuilds that
  pool from the nodes alone drops every one of them. The VM says `capture list
  extends past the kids pool` -- but only when the run happens to fall off the
  end, which on the first attempt here it did on the fourth program tried and
  not on the first three.

**What it costs, and what it does not buy.** The pass is 342 ms of a 2.7 s
self-compile: **+11% to compile, for -63% of the image**. It makes nothing
faster. Both halves of that were measured rather than assumed, and both are
worth knowing before anyone tries to claim the pass back as a speedup:

- **The six benchmark workloads do not move** (40/48/102/29/76/22 ms against
  40/48/100/29/76/21 before), which is the expected answer: they are tight
  loops, and a loop's nodes are in cache whatever the rest of the image weighs.
- **Neither does load time, and neither does the compiler running on itself.**
  A 474 KB image and a 1.29 MB image of the *same compiler* compiling the same
  program came back 2649 ms and 2599 ms -- the smaller one nominally slower,
  which is how a difference inside the placement noise floor looks. The reason
  is that the VM maps an image and reads it where it lies, so it never pays for
  bytes it does not touch. A smaller image is a smaller image.

**Measured, and not kept.** Constant folding, which is the obvious thing to add
to a pass that is already walking every node. There are 299 `if`s with a
constant condition in `dreams` and the branches they would delete come to 897
nodes -- 1.3% of the arena against the 71% above -- and taking them means
deciding what happens to the `if`'s own flags when its child takes its place,
which is the delicate part of the whole design. The constant pools were also
checked and are already fully interned: 108 of 108 ints, 280 of 280 atoms, and
42 unreferenced strings out of 1,871.

**Two things that were tuned, one of which mattered.** The memo -- old node
index to new -- is what stops a shared subtree being copied once per path to it,
and it is half as big as it looks like it should be, because **leaves are
deliberately not memoized**. The arena is very nearly a tree: walking `dreams`
with no memo at all visits 77,920 nodes against 66,336 distinct, so the whole
table buys 17% fewer visits and charges a lookup and an insert on a table the
size of the program for each one. Half those entries were leaves, and a leaf is
the one node a re-walk cannot make expensive -- it has no children, so visiting
it twice is twice one visit rather than twice a subtree. Dropping them took the
pass from 462 ms to 342. The memo is kept for everything else, because a deep
enough DAG without one is exponential and 1.17x on one program is not a promise.
What did *not* matter, measured the same way: settling the tables at every write
rather than once per node, and the `zip`/`range` pair that numbered a list the
walk already had in order -- together 19 ms of 481.

**When changing it**, the bootstrap is the test, as it is for fusion: a compiler
that shares nodes compiles *itself* smaller, so the seed moves and has to reach
a fixpoint again. Note the shape of that -- the old seed does not have the pass,
so it builds an *unoptimized* image of a compiler that does have it, and the
stage after that is the first optimized one. Run `just bootstrap` twice and keep
the second image; `bootstrap-check` then passes because every compiler built
from this source emits the same thing. `--no-opt` emits the arena as lowered,
which is what to reach for first when an image misbehaves.

### A lazy value stored in a map pins the map it was made in

**The tool first, because the finding was not reachable without it.** `--stats`
said how much a program *allocated*, by kind, and how much was *live* at its
peak -- but never what the live bytes were. For a lazy language the allocation
answer is always the same (frames and thunks: making a call is most of what any
program does), and the live answer is the one that decides whether a large
program compiles at all. So the major sweep, which already walks every live
object, tallies them:

```
; 222356000 live at the largest major, by kind: map 57% (745534 at 169 B)
  list 15% (1412025 at 24 B) frame 11% (219134 at 114 B) map entry 10% (552026 at 40 B) ...
```

A major, because only a major proves anything -- a minor never looks at old
space, so its survivors include whatever old space is carrying. The count and
the average size are there because "map 57%" reads very differently as a
thousand fat branches and as a million thin ones, and in this case the
difference *was* the finding. `DREAM_GC_TRACE=1` prints the top three kinds on
every major's line, which is how the shape over a whole compile gets read.

**What it said.** A self-compile's live set is **57% map branches and 10% map
entries** -- 1.35 branches per entry, averaging 169 bytes. Everything the notes
here had assumed about that peak was wrong: not the source, not the tokens,
not the syntax, not the arena's own size. And `--check`, which parses and
resolves and stops, holds **map 0%**, so all of it arrives in `lower`.

The number to compare against is what a map that size ought to weigh. Two
hundred thousand integer keys, built and held:

```
; 10797072 live at the largest major, by kind: map entry 71% (191101 at 40 B) map 29% (52327 at 60 B)
```

0.27 branches per entry at 60 bytes. The compiler's trie is **five times as
many branches, each nearly three times as fat**, for the same number of
entries -- fourteen times the bytes.

**Why, and it is a general fact about lazy code rather than anything to do with
compilers.** A map is persistent: a put shares the entries and copies the path
to the one it added. So *one* map is cheap and *every version of one* is not,
and what decides which you are holding is whether anything still points at the
old versions. A lazy value is exactly such a pointer. `m.[i => f x]` stores a
thunk, a thunk carries the frame that would compute it, and in a fold that
threads the map that frame holds **the map as it stood one step ago**. Store N
lazy values and the map holds N versions of itself: N frames, N thunks, and a
trie path-copied per entry instead of shared.

Measured on its own, the same hundred thousand entries three ways:

| | live | branches | frames |
|---|---|---|---|
| values stored unforced | 68.5 MB | 280,368 at 213 B | 77,188 |
| the spine forced, fields not | 46.2 MB | 200,502 at 169 B | 90,130 |
| values in normal form at the put | **10.4 MB** | **20,847 at 46 B** | **4** |

Three things in that table are worth keeping. It is **6.6x**, which is not a
constant factor anybody tunes away. Forcing the *spine* is less than half the
answer, because the fields are where the frames hang -- and that is the version
someone would write first. And `list.fold_strict` in place of `list.fold` makes
it **worse** (97 MB), which is the check that this is not the chain-of-
suspended-accumulators trap that `fold_strict` exists for: the fold was never
the problem, the stored value was.

**The fix, in the compiler.** `ir.push_node` stored the node it was handed, and
`ir.push_kids` the child indices, both unforced -- so the arena was a chain of
every version of itself, held together by nodes nobody had looked at yet.
`ir.settled` forces a node's five fields, and the shape is `list.fold_strict`'s
and for the reason given there: an argument is not evaluated, so
`push_node (settled n) p` suspends the call to `settled` and changes nothing.
A condition is the one place the language must evaluate something, `type_of` is
the cheapest builtin that cannot answer without evaluating, and `:nothing` is
not one of its answers -- so every clause is true, `&&` reaches the last, and
the caller's `else` is unreachable and says so.

| | peak live | largest major | live frames | thunks |
|---|---|---|---|---|
| self-compile | 409 MB -> **225-297 MB** | 222 -> **95-174 MB** | 219,134 -> **76,028** | 366,700 -> **122,736** |
| 1,600 declarations | 217 MB -> **172 MB** | | | |
| 6,400 declarations | 926 MB -> **755 MB** | | | |

The self-compile's range is not sloppiness and is worth understanding before
reading any peak figure here. Three interleaved runs of the old compiler came
back 409.9, 409.1 and 408.6 MB and of the new one 297.0, 297.0 and 225.1 -- the
*old* one is pinned to a tenth of a percent and the new one varies by a third,
because a peak is only ever sampled where a collection happens and
`gc_threshold_` is refitted to `live * 3` after every major. Allocate less and
every crossing in that geometric series moves, so which major lands on the
high-water mark becomes a coin toss. "Two things that will lie to you about a
change to the interpreter" says the same thing about the clock. The floor of
the new range is the honest reading of what is held; the ceiling is where a
collection happened to look.

**And the wall moved, which is what this is for.** Under the default 1 GB
`DREAM_MAX_HEAP`, a generated program of **8,000 declarations did not compile
and now does**, and so does one of 9,600; 12,800 still does not, either way.
That is a 50% larger program on the axis [dreams/TODO.md](dreams/TODO.md) names
as the one a large project actually hits.

**What it costs: nothing measurable.** Interleaved on the 1,600-declaration
program, 1852/1800/1778 ms before against 1870/1756/1862 after -- a wash, and
well inside the 3% floor that "Two things that will lie to you" describes.
Allocation falls 3% and promotion 14%, which is the mechanism showing up from
the other side: a version chain is not merely churn, it is churn the nursery
hands to the old generation.

**What did not change.** `mind` and `mind/std/all.dr --test` are byte-identical,
and so is the 6,400-declaration program's image. `dreams` and `lucid` differ,
for a reason that is not a behaviour change and is worth knowing before anyone
checks: both *contain* the compiler, so editing `ir.dr` edits their source --
26 nodes, which is the `if` this adds. The bootstrap reaches a fixpoint in one
stage and the seed moved with it.

**What is left, and the number to watch.** Still 1.5 branches per entry against
the 0.35 a settled map has, so roughly a hundred megabytes of a self-compile's
peak is *still* versions nobody can reach but a value nobody has forced. The
tool says so in one number now, which is the useful part: divide the map
branch count by the entry count, and anything much above a third means this.
[dreams/opt.dr](dreams/opt.dr) is the obvious next place -- its five tables are
the same shape as the arena's, and `keep` only forces a node when it happens to
be shareable, because `to_string` is how it makes the sharing key.

*(It was not `opt.dr`, and it was not a table of the compiler's at all -- it was
the arena. The section below is what the hundred megabytes turned out to be,
and how it was found, since guessing from the ratio got the place wrong.)*

### The arena was a chain of its own versions, and `push_node` never saw it

**The finding.** `ir.push_node` has always refused an unforced node --
`settled` is the whole of the section above, applied at the one place a node is
appended. There are two other writers. `ir.set_node_flag` reads a node back out
of the arena, puts a flag on it and writes it in again, and `with_flags` is a
`set` of a list, so what went in was a *suspended* one. Lowering sets a flag on
most of the nodes it emits -- tail position, strictness, impurity -- so most of
the arena was stored as a thunk, each holding the frame of whichever lowering
function asked for the flag, and each of those frames holding the lowering
state, which holds the arena as it stood at that node.

So the arena was a chain of tens of thousands of its own versions, held
together by nodes nobody had looked at yet, exactly as the section above
describes for a map built in a fold -- and past a guard written to prevent it.
It was **45% of a self-compile's live set**.

| | peak live | largest major | map branches / entries | promoted |
|---|---|---|---|---|
| before | 297 MB | 174 MB | 632,614 / 405,846 = **1.56** | 441 MB |
| `set_node_flag` settled | 202 MB | 95 MB | 227,416 / 328,345 = 0.69 | 396 MB |
| + `lower.set_func_body` | 235 MB | 142 MB | | 356 MB |
| + `scope`'s two tables | **204 MB** | 108 MB | 335,360 / 384,170 = **0.87** | **328 MB** |

Read `promoted` down that column rather than `peak live`: a version chain is
churn the nursery hands to the old generation, so promotion is what falls
monotonically, where a peak is only ever sampled where a collection happens and
moves by a third between runs of the *same* binary for the reason "Two things
that will lie to you" gives. `mind` and `mind/std/all.dr --test` are
byte-identical to what the previous compiler emitted, and the bootstrap reaches
a fixpoint in one stage. `lucid` differs in **3 bytes**, all of them `span_start`
and `span_end` of one `FUNC` record, because `lucid` imports `dreams` and so
the prose added to `scope.dr` is part of *its* source too -- the check that
matters is the one this repository has always used: decode the differing
offsets against the section table and the record stride rather than arguing
about them.

**And the wall moved by 40%, which is what this is for.** Under the default
1 GB `DREAM_MAX_HEAP`, on `scale.py`'s declarations axis: before, 8,000
declarations compiled and 9,600 did not; now **11,200 compiles** and 12,800 does
not. (The section above records 9,600 compiling, and it did when that was
written -- `dreams` has grown since, and the wall moves with it. Measure the
commit before in a worktree, as that section and "Where the self-compile now
stands" both say, rather than against a number in this file.) Each of the three changes is load-bearing for that -- `set_node_flag`
alone reaches 9,600 and fails at 11,200, and `set_func_body` is what takes it
the rest of the way. The self-compile is 1-2% *faster*, mean of three
interleaved rounds, which is inside the noise floor and is the answer that
matters: none of this costs anything.

The other two are the same rule at the two other places that break it. A
function's body index (`lower.set_func_body`) is one integer per function, but
an unforced one holds a whole arena apiece. `scope.record` and
`record_bind` are the resolver's real output, and a resolution is a small tree
(`[:member, [:dream, mi], field, gi]`) rather than a fixed list of fields, so
`scope.forced` is a walk where `ir.settled` is five `type_of`s -- and forcing
the spine is a second, separate job (`settle_refs`), because the two chains have
different causes: values hold versions, an unforced put holds the table before
it.

**How it was found, because the ratio pointed at the wrong place.** The branch
count over the entry count says *that* something is a version chain and says
nothing about which map. Three temporary probes in `dream/src/heap.cpp`, each
answering the next question, and none of them kept:

1. A histogram of live `MapObj::count` by log2. A healthy trie of N entries has
   exactly one node at the top bucket; this had 5,549, and a continuous spread
   of every intermediate size from 1K to 88K -- which is what a map that grew
   one entry at a time and kept every version looks like.
2. The type of the object pointing at each big map. All of them were map
   entries, so the big maps were *values* -- `:nodes` inside the program
   record, which is `:prog` inside the lowering state.
3. A walk from the roots keeping a parent for every object, attributing each
   live byte to the nearest frame above it, reported as function indices. That
   named `lower_arms`, `ir.set_node_flag` and `lower_expr`, and
   `dreams/tests/` has no tool for turning a function index into a name, so the
   image was decoded by hand against `FUNC`, `GLBL` and `MODS`.

The third of those is the one worth rebuilding if this comes up again: "which
Dream function is holding the heap" is the question, and nothing in the tree
answers it. The first two only narrow it.

**Measured, and not kept: `opt.dr`'s tables**, which is what the paragraph above
this section predicted. `keep` stores a node in `:out` unforced when it is not
shareable, `rebuild` stores an index in `:memo` and `keep_run` one in `:runs`,
so all three break the rule. Settling them is six lines and it buys **1% of
promotion on a self-compile and nothing at all on a generated program** -- 459
and 463 MB against 463 and 459, the two builds swapping places. The reason is
that `opt`'s one big table is `:out`, and `to_string` forces every node that
goes into it because that is how the sharing key is made; the memo and the run
table hold integers, and an integer version chain of 30,000 entries is not
where a hundred megabytes is. Worth knowing before reaching for it: with the
arena settled, `opt` costs **no peak heap at all** -- 202 MB with it and 204 MB
with `--no-opt` -- where before this it cost 61 MB, all of which was the pass
forcing the suspended nodes it was handed.

### A JIT that can allocate -- the plan

*The plan below was drafted by an AI coding assistant (2026-09-13), not by the
human author of these notes. It is a proposal, not a record: update it as the
work changes it, the way every other section here records what was learnt.*

**The shape of the problem, measured.** A compiled frame is already a root: the
interpreter pushes the frame object on the value stack and passes it into
`CompiledFn`, and the collector finds it and rewrites it. What makes compiled
code unsafe is that its live values live in LLVM allocas (registers), *not in
that frame*. So a collection underneath a compiled frame loses whatever the
frame's canonical copy no longer agrees with -- which is why the four runtime

helpers pin the heap. (The draft also said the tier "refuses every function that
allocates". That is not what the tier checks and not why those functions are
refused; see the second correction below.) The
route chosen here: make something the collector rewrites the source of truth at
every point where the collector can run -- **root the live slots before a
collect-capable helper, read them back after** -- and de-pin the helpers. No
machine-stack walking, which is what keeps this sound with the concurrent
collector's helper threads: they never see the process's stack, but they never
need to, because the roots are the process's own. The draft of this said "spill
into the frame", and that is wrong for a recursing function -- see the first
correction under that stage below, which is the single most useful thing in
this section.


**What the goal concretely is.** Let the JIT compile self-recursive loops whose
values are objects: forward list builders (`list.replicate`, manual `upto`-style
recursions), string-concat folds, map-building folds. `pi` is not the driver any
more -- see the table and "What it did not reach" above; it is already compiled.
The work is all in `dream/src`; there is no compiler change, so no bootstrap
moves.


**The draft of it in stages**, with one caveat about their order. The two
corrections below say that the de-pinning stage is not a prerequisite for the
widening stage, and the two together say something about sequence: done in the
drafted order, de-pinning lands machinery that no workload exercises, because
the tier as it stands is numeric and the forces it makes are shallow. There is
nothing to measure it with until the tier is wider, and "what each stage has to
prove" cannot be satisfied for it in isolation. Widening first gives de-pinning
a workload to be judged by.

*What has been widened since this was drafted*, none of it about allocation: a
compiled function may call another compiled function (`PeerSet`), a `let` in a
compiled body is written where its name is read, and integer `/` and `%` are
inline. All three are in "Making it faster" above. They do not move this plan --
the values are still numbers -- but they do mean the tier reaches ordinary code
rather than only a hand-shaped loop, so the workloads the de-pinning stage was
missing are now easier to write: anything whose helper forces a lazy structure
is one.

- ~~**Fix the known compiled-force SIGSEGV first.**~~ **Done** (2026-09-13). A
  `force_nest` counter on the process, incremented at the top of `force_whnf`,
  and `enter_function` declining the compiled tier past
  `kMaxForceNestForCompiled`. Same shape and trigger as the `JitTooDeep` deopt.
  It was right that this had to come first, and for the reason given: every
  stage after it sends more nested forces out through the runtime. See "Fixed:
  compiled code forcing a long thunk chain crashed" above, and
  `dream/tests/programs/force_chain.dr`, which is the tier-agreement test that
  holds it.
- **Spill/reload and de-pin.** Before each call to `dream_rt_force/arith/
  compare/arith_f`, root every live slot somewhere the collector rewrites;
  after the call returns, read them back, because a mid-call collection moved
  objects. Then the helpers stop pinning. **Two corrections to this stage, both
  found by reading the emitter rather than by writing any of it, and both of
  which would have cost a day each to find the other way:**

  - **The frame cannot be the spill target for a recursing function.** This is
    the one that matters. In the recursive shape (`Emitter::emit_body`) the
    frame is *not* per-invocation: `recursive_call` passes the same `frame_`
    argument down at every depth, because the whole point of that shape is that
    a self call allocates nothing and the only frame in the picture is the one
    the interpreter made for the outermost call. So a depth-N invocation
    spilling its slots into that frame overwrites the depth-0 loop-carried
    state, and the `emit_yield` that reads it back at depth 0 resumes the
    iteration with the wrong values. Spilling to the frame is sound only in the
    loop shape (`emit_loop`), where there is exactly one invocation. What the
    recursive shape wants is a *stack*, and there is already one the collector
    rewrites: `p.pins` (see `Pin` in [dream/src/interp.hpp](dream/src/interp.hpp)),
    which is precisely the mechanism a C++ frame uses to hold a `Value` across a
    force. Push the live slots, call, read back, pop. That also covers the case
    the frame never could: an intermediate LLVM temporary -- the left operand of
    `a + b` while the right is being forced -- which is live in a register and
    has no slot to be spilled into at all.
  - **Nothing here is a prerequisite for allocating.** The stage below reads as
    if de-pinning were what unlocks it. It is not. Allocation never collects: a
    collection runs only at a safepoint, and there are exactly two
    (`run_process`'s and `force_whnf`'s, the latter gated on `force_pins == 0`),
    neither of which an allocation passes through. `dream_rt_float` already
    allocates from compiled code for this reason and says so. What the pinning
    actually buys is that a collection cannot happen *underneath* a compiled
    frame while its values are in registers -- and de-pinning is what a compiled
    loop needs before it can force a long lazy structure without the nursery
    growing for the whole walk, which is the `strict!` failure in "Making it
    faster" a size away. So this stage is about memory under nested forcing, not
    about permission to allocate, and the stage below does not have to wait for
    it.

- **Admit object-allocating self-recursion.** The draft read as though this
  were a widening of `op_is_supported`. It is not, and the gate that actually
  refuses a list builder is worth knowing before any of it is written.

  Take the shape the stage is for:

  ```
  let rec upto i n acc = if i > n { acc } else { upto (i + 1) n (core.cons i acc) };
  ```

  Three separate things are true about it, and only the third is the blocker:

  - There is no `Op::Cons`. A list literal is `Op::MakeList`; a prepend is the
    `core.cons` **native**. So the list case is not an opcode at all, it is the
    old draft's third bullet, "natives compiled code calls".
  - `core.cons` is the easiest native in the table to admit: strict mask `0b0`,
    and a body that is one `make_cons` (`list_cons` in
    [dream/src/builtins.cpp](dream/src/builtins.cpp)). It forces nothing, raises
    nothing and allocates -- which, per the correction above, compiled code is
    already allowed to do.
  - **`Analyzer::run` refuses the function, and would still refuse it with
    `core.cons` admitted.** Every parameter must be forced on every path, and
    `acc` is not: `strict_of` of the `if` is the condition's `{i, n}` union the
    *intersection* of the two arms, the `then` arm gives `{acc}`, the `else` arm
    gives `{i, n}` because a native contributes only what its strict mask
    claims and cons's claims nothing -- so the intersection is empty and `acc`
    never joins the set. Measured rather than argued: compiling the loop above
    beside a numeric one and running it under `--stats` reports `1 functions
    compiled`, and the one is the numeric loop.

  So the stage is a change to the tier's *soundness argument*, not to a table.
  The rule "every parameter is forced on every path" is what licenses compiling
  a self tail call by evaluating its argument expressions eagerly. What a list
  builder needs is the weaker licence: a parameter may be non-strict provided
  every self-call argument in that position is *eagerly safe* -- cannot raise,
  diverge, or perform an effect -- which `core.cons i acc` is, because it builds
  a cell around two values it does not look at. Both halves have to move
  together with `load_slot`, which today *forces* every slot it reads: that is
  exactly what must not happen to a lazy cons's argument, so an unforced read is
  needed beside it -- and `strict_of` counting a slot read as a force is only
  true *because* `load_slot` forces. Change one without the other and the
  strictness claim the whole tier rests on quietly stops being true.

  What bounds the nursery once this lands is the yield: a compiled loop spends
  its slice and returns to the interpreter, which collects at its safepoint, so
  a loop that allocates per iteration allocates for one slice and no more.

- ~~**Generalize the callable set only if measurement justifies it.**~~ **Done**
  (2026-09-15), and done *first* rather than last, because the staged version
  above turned out not to be its prerequisite. What this stage guessed at was
  "call any host native exactly as the interpreter enters it, with the compiled
  caller's slots spilled"; what it needed was the first half of that and none of
  the second. No spill: the arguments go on the process's value stack, which the
  collector already walks, and the heap is pinned for the call exactly as
  `dream_rt_force` pins it. The vetting did shrink from an allowlist to a
  property, and the property is purity plus a saturated fixed arity plus not
  vouching for the collector -- see "A compiled function may call a host native"
  in *Making it faster* for the rule, the numbers, and the one measurement
  (`str_concat` under a compiled frame) that added the third clause.

  Two things it changes about the stage above, which is still open. Its second
  bullet -- "`core.cons` is the easiest native in the table to admit" -- is now
  true in the sense that `cons` *is* admitted, being pure, binary and
  non-vouching. Its third is untouched and remains the whole blocker: `upto`
  above is still refused, because `acc` is neither forced on any path nor
  *carried* (the widening that landed beside this one covers a parameter handed
  straight back, and `core.cons i acc` computes a new one). So the list builder
  still wants exactly what this bullet said it wanted -- the weaker licence for
  an eagerly safe self-call argument -- and now wants only that.

**What each stage has to prove.** A repeated `pi` row that does not move (a
regression there is a spill paid on the hot path instead of only at true
boundaries); new rows for list builder, string fold and map fold, judged
JIT-on against JIT-off (the placement-noise floor applies); equivalence between
the tiers under `just test-all` (heap-verified run plus fuzzer) for the new

`consbuild`, `strfold` and `mapfold` cases; and the long-thunk-chain repro
raising `:stack_overflow` byte-identically with and without `--no-jit`.

### Spilling compiled frames: measured, and not kept

Built and reverted 2026-09-13. The mechanism was correct -- every tier-agreement
test passed -- and it cost 21% on `fib` and 40% on `mapfilter`, so it is not in
the tree. All of it is written down because the three things the plan above got
wrong are the three things anyone doing this again would get wrong, and because
the reason for the cost is not the one anybody would guess.

**Where the roots live.** Not in the heap frame, which is what the plan said.
Two reasons. In the recursive shape *every depth shares one frame* -- it is
passed down and only depth zero ever yields through it -- so a frame spilled at
depth three is the frame depth two reloads from. And a frame slot needs
`dream_rt_frame_store` for the write barrier, which is a call per slot. So it is
`Process::jit_roots` instead: a plain array the collector walks, `top` bumped by
compiled code itself with no call and no allocation.

**What has to be spilled is not just the slots.** The plan said slots. An
operand already evaluated while the operand beside it is still being evaluated
is a live reference in a register and nothing else -- `a + f b` is the shape,
and so is every argument of a self call but the last. Those are held in allocas
by `Emitter::RootSet`, a stack that mirrors the emitter's own recursive descent,
and they travel with the slots. The frame reference is one of them too: a
collection moves the frame, and the yield writes through it afterwards.

**`arith` and `compare` cannot be de-pinned, and the plan was wrong to say they
could.** Making the *caller* safe is not enough: `arith` on two lists is
`concat_lists`, which forces a whole spine holding both operands in C++ locals,
and `arith` holds them too. Rooting at the `dream_rt_*` boundary does not reach
either. So only `dream_rt_force` vouches -- which is the one that mattered
anyway, being the helper a compiled loop spends unbounded time under. The other
three keep `PinsTheHeap`, and their comments now name the real cause.

**One thing the interpreter had to give up.** `enter_function` held the frame in
a C++ local across the compiled call, and three of the five outcomes hand that
frame straight to `eval_node`. It is `Pin`ned now.

**The cost, and the reason.** Best of five on this machine, against the same
build with the spill emission switched off:

| | baseline | spilling | |
|---|---|---|---|
| `fib` | 39 ms | 47 ms | +21% |
| `mapfilter` | 25 ms | 35 ms | +40% |
| `pi` | 32 ms | 35 ms | +9% |
| `collatz` | 205 ms | 219 ms | +7% |
| `sum` | 49 ms | 50 ms | +2% |
| `strbuild` | 81 ms | 82 ms | +1% |

`mapfilter` is the one that explains it, and it explains it in a way no guess
would have reached: **its spill code never runs**. Its slots hold fixnums, so
every `force` takes the inline fast path and the slow block is dead. The
optimized IR says what happens instead -- the reload writes *every* root, so
each of the twelve force sites in that loop body contributes a fresh PHI per
root at its join, and the loop carries a web of fifty-odd PHIs where it used to
carry four. The register allocator pays for that on the path that does run.
So the cost is not the stores; it is that a reload site is a *merge point for
everything the frame holds*.

Two rounds of tuning went in before the numbers above: the capacity check
hoisted to the function entry (a frame's region base is invariant, so no spill
site has to go and find it), and `dream_rt_roots_state` and
`dream_rt_reduction_slot` marked as the pure address computations they are, so
LLVM sinks them to the paths that use them rather than emitting both at every
entry of a recursive body. Those took `pi` from +34% to +9% and `mapfilter` from
+56% to +40%. What is left is the PHI web, and tuning will not move it.

**So the next step is to make the sites fewer, not the spill cheaper.** Two
candidates, both of which stand on their own merits:

- **Force the parameters at entry and stop forcing them in the body.** The
  analyzer already proves every parameter is forced on every path -- that is the
  admission test -- and `Analyzer::force_order`/`entry_forces` already computes
  the definite prefix of the order for the float slots. Extend that to every
  tagged slot it can pin down, and `load_slot` needs no force at all for those.
  A tail call only ever writes values that are already in WHNF, so from the
  second iteration on *no* slot needs forcing. For a fused loop that removes
  every spill site from the hot loop rather than making it cheaper.
- **Restart the iteration instead of reloading.** A compiled loop body is pure,
  and re-forcing an updated thunk is a resolve. So on a collection the loop
  could branch back to its header with the reloaded values, which collapses
  every per-site PHI into the loop header's existing ones. It needs a collection
  counter on the process to test against, or it re-forces for ever.

Neither was worth doing on spec, which is why this was reverted rather than
carried: nothing compiled allocates yet, so all the spill buys today is that a
compiled loop forcing a long lazy structure can collect instead of growing a
nursery no safepoint can reach -- real, but not visible in any measurement here,
and not worth 40% of `mapfilter` to have early. Do the site-count work first;
then this becomes cheap enough to be worth having, and stage three has something
to stand on.


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

## Static types

`let add : :integer -> :integer -> :integer` is checked at compile time, by
[dreams/typecheck.dr](dreams/typecheck.dr), which runs after `scope.resolve!`
and before lowering and produces diagnostics and nothing else. Its header is
the design; what is here is what to know before changing it.

**The three rules, and every false positive so far broke one of them.**
Unannotated code is never an error (a name with no signature is `:any`); only
a definite mismatch is reported (`sub` answers "could this be that?"); and the
pass never changes the program. Three things that looked like checks and were
not, each found by running the checker over this repository:

- `1 + "x"` is not an error. `examples/05_errors.dr` and
  `dream/tests/programs/errors.dr` write it on purpose inside `try!`, and in a
  lazy language a line nobody forces never raises. So an operator mismatch, or
  a value applied as a function, is reported only when a *declared* type is
  part of it (`from_literals`): a literal is evidence of nothing but itself.
- A record nobody annotated gets no signatures. `Person.greeting %{}` reads a
  defaulted field of an empty map and is `"Hi"`; a signature saying the
  accessor wants a whole `Person` rejected it. A `mapping`'s accessor now asks
  only for the field it reads, and for any map when that field has a default.
- A type variable *solved* from an argument is not a promise.
  `list.fold (fn acc j -> acc + [j]) [] xs` solves the accumulator from `[]`,
  and holding the lambda's answer to "an empty list" rejected half the
  compiler. A lambda passed as an argument takes its *parameter* types from
  what was solved (`solved_inputs`), and its answer is only bound, never
  checked against a solved variable.
- `()` is "nothing there", and the checker does not narrow a `()` arm away.
  So `map.get () ages k` solving `v` as `:integer | :unit` made every guarded
  use of the answer a report. A `()` argument now never decides a variable
  (`solve`), fits one wherever it was written even once another argument has
  decided it (`admit_unit`), and fits a declared variable in an answer
  (`sub`), which is what lets `list.minimum` be `[a] -> a`. Two things std
  still answers `:any` for, deliberately: a read by *position* (`head`,
  `nth`, `array.get`), because a list here is as often a record as a
  sequence and a tuple's element type is the union of its fields; and a
  decoded message, because the reader knows its shape and the format's
  `Value` would only say it might be a list. The head of
  [mind/std/list.dr](mind/std/list.dr) says the same.

The check to run after any change here: every `.dr` in the repository must
check clean -- `std --test`, `dreams --test`, `lucid`, `mind`, the examples,
`dream/tests/programs` and the benchmarks -- because every report on code that
works is a report nobody asked for, and the first thing a user does with a
checker that cries wolf is turn it off.

**What it costs.** About 290 ms of a 5.5 s self-compile, ~5%, with every image
byte-identical. The first version cost 578 ms, and nearly all of the
difference was one case: most calls in any program are to something no
signature describes, and a callee of type `:any` now skips the argument
bookkeeping entirely (`infer_apply`). `infer`'s arms are ordered by how common
each node is, because a `match` tries them in turn -- worth another 15%.

**Where it reads from.** A name in an expression is the resolver's answer
(`scope.resolution`), so the checker never re-implements name lookup. A name
in a *signature* is the exception: signatures are never resolved, because they
never run, so `type_ref` looks a type name up in the module's environment the
way `scope` would -- a module alias first (that is how `Shape` names a union
and `Point` a record), then a global, then a selective import.

**Named types stay folded** (`[:named, shown, gi, args]`) and unfold on demand,
because `type Tree = :unit | [:integer, Tree, Tree]` is recursive. `sub` spends
fuel on each unfold, and runs out to *true*: an answer the checker cannot reach
is not a mismatch.

**Exhaustiveness is deliberately narrow.** Only a scrutinee whose type is a
*name* -- a union somebody declared -- is held to it, and a variant is reported
only when every arm certainly misses it. A guarded arm, or one that takes a
field apart with a nested pattern, counts as handling. A false report here
teaches people to write `_ =>` everywhere, which is worse than no check.

**Bootstrap order.** The seed must parse a signature before `std` may contain
one, because the compiler imports `std`. Changing the checker so that it
accepts something the seed's checker rejects needs one build with `--no-types`
first: build the new compiler with the seed and `--no-types`, let *that*
compile the source twice, compare, and copy it over the seed.

### What a signature buys the compiled code

The first use of the types for speed, 2026-09-25, and the rule it rests on is
the one to keep: **the types are gradual, so a signature is a hint and never a
promise.** An unannotated caller may hand a `:float` parameter an integer, the
checker cannot object (`:any` fits everywhere), and the program is owed the
answer the interpreter gives. So nothing is *trusted* on a signature's say-so;
it chooses a representation, and the representation is guarded.

What flows: `typecheck.float_params` reads which parameters each top-level
signature declares `:float` (a name for one, a float literal and a union of
floats count; `:number` and a type variable do not), `main.dr` maps globals to
functions, and the image carries a `TYPE` section of `[func, mask]` records
(docs/bytecode-format.md). A program with no float in a signature has no
section and is byte-identical to before -- which is why the bootstrap seed only
moved because the compiler's own source did.

What the JIT does with it, all in [dream/src/jit.cpp](dream/src/jit.cpp):

- **A declared float starts the float fixpoint as Float.** That is what reaches
  `integrate (x + dx) hi dx (acc + x * dx)`, a loop with no float literal in its
  self call, where inference alone never says `x` is one. Only parameters the
  body forces on every path are seeded: a merely carried one may arrive as a
  suspension nobody is allowed to force, and would bail every call.
- **The first iteration is peeled** when the entry cannot force the float slots
  in the body's order. That rule used to drop the whole specialization, so it
  matters for untyped loops too; now the first iteration runs over tagged slots
  and only its back-edge crosses into doubles, as a guard (`enter_loop`). A
  carried slot is re-read at the back-edge, because the argument after it is
  usually what forced it -- a raw read taken earlier is the caller's thunk, and
  that was a 20x slowdown before it was found.
- **A declared-float peer gets a typed variant**, taking doubles, beside the
  generic one; a call site passes doubles straight through, checks a tagged
  argument for a float box, and falls to the generic variant otherwise.
- **A peer whose body is a float returns a raw double**, typed or not. The bits
  ride in the return register and are read only after the status says
  `JIT_OK`.
- **A run of bails gives the function up.** A guard that fails hands the call to
  the interpreter; sixteen in a row (`Jit::note_bail`) deoptimize it, since a
  peeled loop fed integers would otherwise pay a wasted first iteration per
  interpreted iteration.

| | untyped | typed | allocated, typed |
|---|---|---|---|
| `integrate`, 3M steps, no float literal in the self call | 213 ms | **23-32 ms** | 192 MB -> 230 KB |
| `term` helper called from a loop, 3M calls | 135 ms (99 now, from the double return) | **39 ms** | 192 MB -> 297 KB |
| escape-time loop, floats already inferred | 63 ms | 62 ms | -- |

The benchmark's seven rows did not move. `dream/tests/programs/jit_types.dr`
holds the tier agreement, including integers passed to declared floats through
an unannotated caller, a first iteration that raises, and both give-ups.

**What a type cannot do here, measured rather than assumed:** admit a function.
The admission rule is strictness -- every parameter forced on every path -- and
a type says what a value is, not whether it is evaluated. The escape-time loop
written `if i >= limit { i } else if zr * zr + zi * zi > 4.0 ..` compiles
nothing typed or untyped, because `zr` is not forced on the first path; the
same loop with the two tests swapped is compiled either way.

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

### Completion is asked of something that is not a program

Every other request is asked of a program. Completion is asked of a buffer
mid-keystroke, and that difference decides the design — it is the thing to
understand before touching [lucid/complete.dr](lucid/complete.dr).

At the moment a completion is wanted the buffer usually says `console.`, and
**that does not parse**. Measured rather than assumed: the loader reports zero
modules for it, so there is no environment, no global table and no resolution —
nothing to complete *from*. So the text is tried twice. The buffer as written
comes first, because where it already parses that answer is exact and inserting
anything into it can only be wrong. When that yields no module, a name nothing
would write is inserted at the cursor and the repaired text is analysed instead.

The two fail in *different* places, which is why both are kept. A bare name is a
statement but it is not a declaration, so a cursor on a blank line between two
top-level `let`s is exactly where the repair breaks a file that was fine — found
by asking `dreams/lower.dr` for completions at line 300 and getting nineteen
keywords. Everything read here is before the cursor, so both candidates agree
about every offset that matters.

What is offered comes from the compiler's tables and is ranked in the order
resolution would reach it: a local, a global of this module, an import, a
builtin, a keyword. After a dot it is `scope.exports` — the same list the
compiler quotes back when a member is misspelled — and the chain in front of the
cursor is resolved by handing a synthesized expression to `scope.as_namespace`,
so "is `list` a module, an import, a package or a local" has one answer and not
two. A host module offers nothing, because the host owns its member table at run
time and the compiler has never seen the names.

**The locals are the exception, and the only place this server re-implements
anything.** The resolver knows what is in scope at every point of its walk —
that is what a frame stack is — but it pushes and pops as it goes, and when
`resolve!` returns nothing survives saying what was visible at a given byte.
So `analysis.dr` reads the binders back off the syntax tree, and the scoping
rules are therefore written twice. That section is deliberately literal: every
case is a transcription of the matching case of `check_expr`, `check_block` or
`check_arm`, in the same order, so the two can be read side by side. Where
`scope.dr` exports the rule itself it is called rather than copied —
`scope.pattern_bindings` is what says which names a pattern binds, here as
there. The one case that is easy to get wrong is `comp`, which is walked with an
empty frame stack because it runs before the program does, and so drops
everything the enclosing constructs had put in scope.

Hover reads the same walk. It shows a function with the parameters it was
declared with — `fold f acc xs` rather than `fold` — and takes them from the
declaration rather than from the resolver, which keeps arity and not names.
They are sliced out of the source, so a parameter that is a pattern reads as it
was typed. Hovering a name at its own *definition* still answers nothing, which
is not new: `refs` records uses, and a binder is not one.

One cost worth knowing: a completion is a whole-program analysis, as hover and
go-to-definition already are. On `dreams/lower.dr`, which pulls in the whole
compiler, that is roughly a second per request.

### Two trees, and which question goes to which

`lucid` holds the program twice: as the editor wrote it, and after macro
expansion. Which one answers is not a matter of convenience — it is exact, and
getting it wrong is silent.

- **What a name means** is asked of the **expanded** program. That program is
  what runs, and there is nothing else for a name to mean.
- **What is written** is asked of the **source**: which locals a cursor can
  see, what parameters a declaration was given, whether it was a `macro`.

The reason is that expansion *replaces* an `expand` with generated syntax
carrying offsets from a reserved part of the 32-bit space. Every byte the user
is looking at inside `expand twice counter` is then inside no node at all, so a
walk of the expanded tree finds nothing there — and `let double counter =
expand twice counter` offers no `counter`, which is precisely the local the
person typing it wants. `analysis.source` is the second loader, and
`source_items` is what the binder walk, `find_decl` and `params_of` read.

An `expand`'s own name is the other half of the same problem, and it is the
compiler that fixes it rather than the server. That name is a name occurrence
like any other, but the only pass that ever resolves it is the one that
replaces it, so the resolver walks a program the name is not in and nothing
would ever record what it meant. So expansion writes each one down
(`modules.expansions`) and `scope.resolve!` seeds its occurrence table with
them — which makes hover and go-to-definition on a macro the *ordinary*
lookups, not a second mechanism. Nothing downstream reads those entries:
lowering asks about nodes, and the nodes at those offsets are gone.

Only a macro may be written after `expand`, so that is all completion offers
there — not a local, not an ordinary global, not a builtin, not a keyword, all
of which are things that cannot be written in that position. The keyword in
front of the cursor is found lexically (`word_before`), for the reason the
section above gives: at the moment a completion is wanted, `expand tw` does not
parse.

### A record is a declaration before it is a module

`group Point { x, y }`, `struct`, and `mapping` are rewritten by the *loader*
into a module of generated functions — a constructor, an accessor and a setter
per field ([syntax.record](dreams/syntax.dr)). That happens before anything
resolves, which is what makes records cost the rest of the compiler nothing;
and it is why a language server had nothing true to say about one.

Three things are gone by the time the resolver runs, and each was a visible
defect:

- **The declaration is not among the module's globals.** `Point` became a
  module of its own and the parent got an import of that name, so
  `scope.module_defs` has no `Point` in it and the outline of a file full of
  records was empty.
- **The members carry spans into no file.** Several of them stand where one
  declaration was written, so `syntax.record` freshens them into a reserved
  part of the offset space. Go-to-definition on `Point.x` clamped to the end of
  the file, and the signature — which lucid built by *slicing the source* at
  each parameter's span — came out `make  ` and `x `.
- **The kind and the fields are gone.** `:list`/`:array`/`:map` is what the
  rewrite builds from; `group`/`struct`/`mapping` is what the reader typed.

`modules.records` is what the rewrite knew, kept: `[owner, kind, name, fields,
members, span]` per declaration, collected by `record_items` alongside
`expand_items` and under the same `when` conditions, so a record a configuration
switches off is no module and no record either. **Nothing in the compiler reads
it** — it is there for `lucid`, the same way `modules.expansions` is, and for the
same reason: the pass that rewrites something is the only one that ever sees what
was written.

What it buys, all in [lucid/analysis.dr](lucid/analysis.dr)'s "records" section:
the outline lists each record with its fields and members underneath it, in
source order; hover says `` `x record` -- reads `x` of the group `Point` ``,
`` `set_y record value` -- replaces `y` in the struct `Vector` `` and
`` `new host` -- builds the mapping `C`, defaulting `retries` ``; and
go-to-definition on a generated name lands on **the field**, in the module the
record was written in — not on the record's own module, whose "path" is a dotted
name that `location` would have made a URI out of.

One fix that is not about records and should be kept in mind for any generated
code: `signature` now shows a parameter by its **name**, and only slices the
source for one that is a pattern, which has no name. Slicing was never right for
a declaration the compiler made up, and a record's generated helpers are all of
them.

**A member is the exception, and it is the one that needed no work.** Everything
above exists because the rewrite throws the author's spans away. It throws away
only the spans it *made up*: `syntax.record` freshens the generated declarations
and appends the members after, un-freshened, so a member's body and parameters
are still at the offsets they were typed at. That is not tidiness — `fresh`
exists because resolution keys names and binders by span, and the generated
declarations need new ones because several stand where one declaration was
written and a default is emitted twice (in the accessor's `else` and in `new`'s
body). A member is emitted once, so it needs nothing. The consequences are worth
stating because each is a thing that had to be built for every other generated
name: a "cannot find" inside a member body is reported **where it is written**,
go-to-definition lands on the member itself, and `shown_signature` reads the
real parameters out of the file, so `self` is shown because the author chose it.
Generated spans live above 2^31 and written ones below, which is what keeps the
two from colliding.

### A host module's members come from the host

`import std.vm` compiles to a lookup that happens while the program runs, so
`dreams` knows such a module by name and by nothing else — which is why
completion after a dot on one used to offer nothing at all. But `lucid` runs
*on* the VM that owns the table, so it asks: `vm.host_members "std.vm"` answers
`[name, arity]` for each member, in declaration order, and `()` for a module
that is not registered. A variadic member's arity is `-1`.

Two things that follow. A module an embedder registered and named to the
compiler with `--host-module` is not in *this* VM, and `host_members` answers
`()` for it — nothing offered, which is what was offered before. And the
compiler still does not check host member names at compile time: it could now,
but a program may be compiled for a VM other than the one compiling it, and
that is the bargain `is_host` makes on purpose.

The *builtins* had the same shape of problem for a smaller reason: what each
one takes and answers was written as a comment beside its name, and a comment
is no use to a server, so completion on `len` could show nothing but `len`.
`builtins.signatures` is that comment moved into the program, as a map rather
than a second list so that adding a name and forgetting its line is a failing
test.

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
- Evaluation is lazy; `strict!` forces. A parameter written `!acc` or
  `(strict acc)` is forced when the function is entered, which is the fix for
  an accumulator that would otherwise become a chain of suspensions and
  overflow. It is also how to get a loop past the JIT's strictness test. See
  "Strict parameters" in docs/language-spec.md; the lowering is a strict
  `local` statement at the top of the body (`lower_body`).
  `std` uses it wherever an accumulator could grow: `list.fold_strict`, which
  was a `type_of` test in a condition and is now `!acc`; `list.sum`,
  `product`, `count`, `minimum` and `maximum`, which were lazy; `array.fold`
  and `str.fold`, which is to say all of `std.seq`; and every fold in
  `std.map`. Each forces what `f` answered and never the seed, which is what
  `fold_strict` always did. On a million-element list, interpreted, peak live
  heap went 439 MB -> 53 MB and the run 2183 -> 1503 ms. The self-compile did
  not move, because the compiler already avoided these traps by hand.
- `when test { .. }` holds a module's tests, collected by compiling with
  `--test`. Tests are `test.case "name" $( test.eq! expected actual )`.
- `macro name params = ..` declares a syntax transformer and `expand name args`
  calls one. A macro receives its arguments as **syntax** — ordinary Dream
  lists, with a span last — and returns syntax, which is checked and then
  substituted. It runs on the same embedded VM `comp!` does, before name
  resolution; [dreams/expand.dr](dreams/expand.dr) and "Expanding a macro is a
  compile" say what that costs.
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
  image is a hint for the JIT, below -- code no signature touches is
  never rejected, and `--no-types` skips the pass. "Static types" below is the
  design.
- `union Shape { circle(radius : :float), empty }` declares a **discriminated
  union**: a module of constructors whose values are `[:circle, r]` and
  `:empty` -- the tagged lists Dream already writes by hand -- plus a global
  `Shape` holding its description. A `match` on one must handle every
  variant. Lists back it; an array opt-in, as `struct` is to `group`, is
  planned and not built.
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
