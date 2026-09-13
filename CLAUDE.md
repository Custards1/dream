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
5. ~~**`dreams` learns to fuse.**~~ **Done.** The first optimization that is
   genuinely the compiler's own rather than the VM's: a pipeline of `std.list`
   combinators becomes one loop, and the list between them is never built. It
   did what it was built to do and it did it the way the plan said -- not by
   making the list cheaper but by producing no list, so that what remains is a
   loop the JIT already compiles. `sum` went from 13.0x CPython to **0.15x**
   and `mapfilter` from 13.1x to **0.75x**. What it is, what makes it sound,
   and the one workload it does not finish are under "Deforestation" in
   *Making it faster*.

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
benchmark/benchmark/run.sh   # six workloads, Dream against CPython
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

### Known and not fixed: compiled code forcing a long thunk chain crashes

A JIT-compiled function that forces a long chain of suspensions dies with
**SIGSEGV** where the interpreter raises `:stack_overflow`. Found 2026-09-13,
reproduced against the JIT exactly as it was before it learnt self recursion, so
it is not new -- but nothing here had looked for it before.

```
import std.console;
let rec loop_f f i n acc = if i > n { acc } else { loop_f f (i + 1) n (f acc i) };
let main! = { console.print! (loop_f (fn a b -> a + b) 1 10000000 0) };
```

`acc` is lazy, so this builds ten million suspended applications rather than
adding anything -- the trap `list.fold_strict` exists to avoid, and its note in
[mind/std/list.dr](mind/std/list.dr) explains it. Forcing that chain is supposed
to raise at `DREAM_MAX_DEPTH`, and with `--no-jit` it does, exactly:

```
dream: uncaught error: <error :stack_overflow recursion too deep: 4194305 pending frames>
```

With the JIT it segfaults. The reason is one line of the machine: the
interpreter forces a slot by pushing a **continuation**, so its recursion is
heap and its limit is a number it can check, while compiled code forces a slot
by *calling* -- `load_slot` -> `force` -> `dream_rt_force` -> `force_whnf`,
which runs a whole nested machine loop, which enters the compiled body again for
the next link. One C++ frame per link of the chain, against an 8 MB thread
stack. The lambda here is the whole trigger: `fn a b -> a + b` is arithmetic
over two strict parameters, which is precisely what this tier compiles.

The fix is not the depth argument that bounds compiled *self* recursion, because
the recursion here goes out through the runtime and back in. What it wants is
the same move `Jit::deoptimize` makes, on a different trigger: count the nested
`force_whnf` loops on a process, and have `enter_function` decline the compiled
tier past some depth. From there the interpreter handles the rest of the chain,
pushing continuations instead of C++ frames, and the walk finishes on the heap
where its limit can be enforced. That counter does not exist yet -- `force_pins`
is about vouching, not depth -- and none of it is written.

### Measured, and not kept

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

`benchmark/benchmark/run.sh` runs the six workloads of `main.dr` against the
transliteration of them in `bench.py`, best of `--repeat` runs each, and prints
the ratio. On this machine, 2026-09-13, best of five against CPython 3.13 (below
1.00x is Dream ahead; "was" is the same measurement before any of the work in
this section, "pre-fusion" is after the JIT work and before deforestation):

| workload | dream | python | ratio | pre-fusion | was |
|---|---|---|---|---|---|
| `sum` | 47 ms | 316 ms | **0.15x** | 13.0x | 14.3x |
| `fib` | 40 ms | 207 ms | **0.19x** | 0.19x | 2.62x |
| `collatz` | 205 ms | 687 ms | **0.30x** | 0.33x | 2.83x |
| `mapfilter` | 22 ms | 30 ms | **0.75x** | 13.1x | 14.8x |
| `pi` | 708 ms | 229 ms | 3.1x | 5.8x | 6.55x |
| `strbuild` | 73 ms | 1.0 ms | 70x | 82x | 72x |

Four of the six are now ahead of CPython, and the two that are not are each
behind for a reason that has nothing to do with lists. `pi` **is** fused -- it
builds no list at all -- and is still 3.1x, because its loop is floating point:
44% of what it allocates is boxed floats, so the JIT declines it (compiled code
may not allocate) and a fused but interpreted loop is what is left. That is the
"JIT that can allocate" bullet below, not a fusion gap. `strbuild` is a
different algorithm on each side and always was.

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
  `PinsTheHeap` says. So compiled code may not allocate, and a fold allocates
  three objects an element. Giving compiled frames a stack map is what would
  lift that, and it would lift the interpreter's 164 ns as well. It is the
  larger and riskier of the two, and the one below does not need it.

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
  JIT declines.

**What it did not reach.** `pi` fuses and is still 3.1x CPython, because its
loop is floating point: boxed floats are 44% of what it allocates, compiled code
may not allocate, and so the JIT refuses a loop that is otherwise exactly its
shape. Nothing more in `dreams` will move it; it wants the stack maps under "A
JIT that can allocate" above. That is the honest boundary of this work -- fusion
removes the list, and what is left is whatever the loop itself costs.

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
