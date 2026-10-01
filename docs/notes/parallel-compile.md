# Compiling in parallel

Spreading a self-compile across processes and cores, and what it does on fewer of them. Moved out of `CLAUDE.md` on 2026-09-25; the prose is as it was, so a date or number in it is as of when it was written.

## The self-compile on four cores: 8.8 s -> 5.8 s, and what 3 s would take

Done 2026-09-25 against a stated goal of a self-compile in three seconds, on a
four-core container where the commit before measured **8.6-9.0 s** (its own VM,
its own compiler, interleaved). It is now **5.8 s**, and **5.1 s** under
`just vm-pgo`. The goal is not met; the last paragraph of this section says
what is left and why none of it is an edit. In the order the wins were found:

- **A yielding process was moved to another core every 4000 reductions.**
  `run_slice` ended in `enqueue`, which places round-robin, so a program of one
  green process hopped workers forty thousand times a compile, each hop a futex
  wake of a sleeping thread and a cold cache. `Scheduler::requeue` puts it back
  on its own worker's queue and wakes nobody unless something is already waiting
  behind it. **~2 s**, the largest single item here, and every stage paid it --
  the collector's pause alone fell from 1.3 s to 0.56 s. It is invisible to
  `--profile` and to `-j 1`, which is where it hid.
- **A `comp` ran against an image of the whole unshared arena.** Six one-line
  compile-time expressions paid for serializing 142,000 nodes. `comp_image` in
  [dreams/lower.dr](../../dreams/lower.dr) gives every function the expressions
  cannot reach one shared raising body and runs the optimizer, which copies only
  what a body reaches: **~0.9 s -> 0.17 s**. Incomplete reachability is loud,
  not wrong, for the reason the macro stub is.
- **Parsing is spread across processes.** Before a file is parsed its
  top-level `import` lines are read off the text, and every file they lead to is
  read and handed to a process of its own (`prefetch!` in
  [dreams/modules.dr](../../dreams/modules.dr)); the depth-first walk is unchanged
  and joins each parse when it reaches the file. Loading went 2.2 s -> 0.75 s.
  Two things had to be fixed first, and both are general:
  - `Heap::copy_between` searched a vector for every object it had already
    copied -- quadratic in what crossed, nothing for a message and minutes for
    a syntax tree -- and recursed down a list's tail. It is an open-addressing
    table and a loop along the spine now.
  - A suspension carries its frame, so a `spawn!` written inside the loader's
    walk copied the whole loader into the child. The thunk handed to `spawn!`
    is made in a function of its own (`parse_elsewhere!`) whose frame holds the
    text and nothing else. Read that before writing the next `spawn!` in the
    compiler.
- **The arena is shared in a child while the checker runs.** They need
  nothing from each other; `share_elsewhere!` in
  [dreams/compile.dr](../../dreams/compile.dr) hands the program's *tables* over
  (the child flattens them, `opt.optimize_program`), and the checker's half
  second comes off the critical path. Splitting the share itself in two and
  walking the second half's arena into the first's table was built and gives
  the same bytes -- the table is canonical -- but the merge costs as much as
  the half it saves, so it is not kept.
- **A rename is a wrapper.** `let node_op = Node.op;` was a global read and a
  closure application at every call; `rename_wrapper` in
  [dreams/scope.dr](../../dreams/scope.dr) records it as the wrapper it amounts to,
  and an empty `[]` or `%{}` now counts as a literal, which makes every
  `p.[:nodes else (%{})]` accessor a wrapper too. 4%.
- **Forcing a small tree is `to_string`.** `ir.settled` and `scope.forced`
  were a dozen reductions a node; one native that cannot answer without
  evaluating everything does the same job. 5%.
- **The JIT compiles on idle cycles** (`SCHED_IDLE`). With the compiler's work
  now spread over every core, LLVM's thread made a compile slower with the JIT
  on than off. When a compiled body arrives is not observable, so it may as
  well arrive when nothing else wants the core.

**A JIT bug this exposed, fixed.** A compiled lazy `fold` answering its
accumulator forced it before returning, and forcing it ran the fold's function,
which could park (`join!`); a park under compiled code retries the whole call
from its frame, which builds the chain of suspensions afresh and performs every
effect in it again. A fold whose function spawned and joined printed each step
twice, and the loader's walk spun for ever. A loop now answers a parameter in
tail position as it stands and `enter_function` forces it, where a park
suspends rather than retries. `impure_callee` also looks at a lambda's body
flag, since a lambda is never impure by spelling.

**`just vm-pgo` trains with `DREAM_JIT_SYNC=1`.** A run ends in `os.exit!`
whatever the background compile thread is doing, and its counters are then
not flow-consistent, which `-fprofile-use` rejects for `jit.cpp`.

**What 3 s would take.** The critical path is now load 0.75, expand 0.22,
resolve 1.0, lower 2.4, then the share child 1.3 (beside the checker's 0.53),
then emit 0.2. Lowering is **~240 reductions per node it emits, uniformly** --
no body is an outlier, so there is no quadratic left to find there -- and the
arena it emits is four times the one that survives sharing, 70,000 of its
142,000 nodes being leaves of which 2,600 are distinct. Halving each of
resolve, lower and share is a rewrite of their hot paths, not a finding. Two
routes that would each take a second off and are designed but not built:
sharing each function as soon as lowering finishes it (the share child fed by
message, with the functions whose bodies hold a `comp` placeholder shared last,
which changes the image's order but not its meaning), and interning leaves at
emission, which is blocked on `set_node_flag` mutating a node in place -- 1,275
leaves carry a flag, and a shared one would hand it to every user. Measure
against a VM built from the commit before, in a worktree; this machine moves by
10% between minutes.

## Resolving, lowering and checking in parts: 5.75 s -> 3.7 s

Done 2026-09-25, the next round after the one above, on the same four-core
machine (the commit before measured 5.75 s here). Lowering was 2.4 s of the
critical path and every body's lowering depends on the resolution and on
nothing else, so it runs in four processes (`lower.link_parallel!`, used by
`compile.build!` whenever the build is optimized). "Lowering in parts" in
[dreams/lower.dr](../../dreams/lower.dr) is the design; what is worth carrying away:

- **The split was worth nothing until copying stopped.** Four processes lower
  in 0.62 s against 2.5 s for one, but handing each of them the resolution by
  `spawn!` copied it -- 0.43 s, most of the saving. So the VM grew a
  **shared area** (`SharedArea` in [dream/src/heap.hpp](../../dream/src/heap.hpp),
  `vm.share!` in docs/builtins.md): a value forced all the way down and copied
  once into memory the runtime owns, marked `GC_SHARED`, which every collector
  stops at and every cross-heap copy passes by pointer. Sharing the resolution
  is 40-90 ms; a spawn that holds it is a few. What makes it sound is that
  nothing in the area can change (a suspension is refused rather than copied
  in, and every object is born `AUX_DEEP_FORCED`) and nothing in it points
  out. What it costs is that nothing in it is freed before the runtime is, so
  only the command line uses it: `modules.load_shared!` for the parses and
  `link_parallel!` for the resolution and the parts. The REPL builds with
  `compile.scratch`, which is not optimized and so lowers whole, and `lucid`
  never builds at all -- both would leak a program's worth per request.
- **Deal the bodies round-robin.** Contiguous slices of equal source were a
  third apart in nodes -- a byte of `typecheck.dr` lowers to three times what a
  byte of `lexer.dr` does -- and the slowest slice is the stage. Dealt one at a
  time the four parts come out within 12% of each other. The count is fixed at
  four rather than the number of cores, because which part a body lands in
  decides how the merged constant pools are numbered, and the image must not
  depend on the machine.
- **Share inside a part; do not share across them at the merge.** Each process
  runs the optimizer over its own part (36K nodes -> 11K), and the merge lays
  the parts end to end, renaming each part's constants into the program's pools
  (`opt.renumber_part`) -- 0.35 s. A merge that deduplicated across parts was
  built first, as the optimizer reading several arenas, and cost 0.8 s: at
  ~17 us per node, a rebuild whose input is already shared is dearer per node
  than one whose input is mostly leaves. The cross-part sharing still happens,
  in the rebuild `compile.build!` already runs in a child while the types are
  checked, so the image is as small as before (33,247 nodes against 32,898).
- **A `comp` placeholder is `unit fi`, not `unit`.** Parts are optimized
  before compile-time expressions are settled, and a bare `unit` would be
  shared with every other one and then overwritten with them all. Carrying the
  function index keeps it itself and makes it findable after the merge.
- **The bootstrap needs two stages to settle.** The parts intern atoms in a
  different order than the whole-program walk, and a `comp` that builds an
  atom-keyed map quotes it in the compiling VM's atom order (the same effect as
  "`mind/std/all.dr --test` is not byte-stable" below). So the stage the old
  seed builds and the stage after it differ in 434 bytes, and the stage after
  that is the fixpoint. It is deterministic run to run.

- **The types pass is per body too.** `typecheck.analyze` builds one checker
  from the signatures (40 ms) and then checks every body against it (600 ms),
  and a body's check only ever *adds* to the diagnostics threaded through it.
  So `typecheck.prepared` and `concluded` split the two, and
  `compile.analyze_parallel!` shares the checker and deals the bodies to four
  processes: 0.6 s -> 0.37 s, for identical diagnostics by construction.
- **Compile-time contracts run against a pruned image**, as a `comp` does
  (`lower.comp_image`), rather than a serialization of the whole arena.
- **What did not work: sharing at the merge, twice.** Once as the optimizer
  reading several arenas, once as a linear pass exploiting that a shared part
  lists every node after its children (no recursion, no visit memo). Both
  cost 0.8 s for 42,800 nodes. The price is not the walk but the per-node map
  work -- keying the node, looking it up, recording where it went -- at ~200
  reductions a node, and no rearrangement of the walk changes that. Sharing
  the whole arena therefore stays in the child, where it overlaps the types.

- **Resolving is per body too, and so is everything after it.** Declaring the
  program's names is 40 ms; walking 2,205 bodies is the other 950. A walk reads
  the declarations and nothing another body's walk wrote, except three
  counters -- function indices, deforestation's invented globals, and the
  queue of bodies -- so `scope.resolve_parts!` walks the bodies in four
  processes from one shared starting state, each numbering its own functions
  and globals from where the declarations left off, and `merge_parts` moves
  part k's past the parts before it. The name tables (32K uses, 10K binders)
  are **not merged at all**: lowering and checking a body only ever ask about
  that body's names, so each part is lowered and checked in a process holding
  the part that walked it (`scope.parts`), and the merged resolution carries
  only what is about the whole program -- functions, globals, bodies,
  diagnostics, wrappers. The renumbering happens once, where the arena is laid
  out (`opt.renumber_part`: closures, thunks, placeholders, invented globals).
  Wrappers are the one table every part reads and deciding one reads the
  wrapper's own body, so a part says what each body can say about itself
  (`wrapper_candidate`) and the merge finishes it (`wrapper_from`). 0.95 s ->
  0.45 s, and the functions come out in part order rather than body order,
  which changes the image and nothing it does.
- **A merged resolution cannot be lowered whole.** It has no name tables, so a
  `--no-opt` build (which lowers the arena whole) must resolve whole too;
  `compile.build!` resolves in parts only when it will lower in parts. That was
  a bug for one commit, caught writing `dreams/tests/parts.sh`, which holds the
  parts to the whole: the same diagnostics as `--check`, in the same order, and
  a program that prints the same built both ways.

- **Sharing across the parts starts the moment they are joined.** The
  re-share of the whole arena used to wait for the merge and the `comp`s, then
  took 0.8 s. Now a child shares the four parts straight into one arena
  (`opt.optimize_parts`, the rebuild reading several arenas, with leaves
  memoized because a shared part is a DAG and not a tree) while the parent
  merges, settles the `comp`s and checks the types; the parent then patches
  the `comp` values into the child's image (`lower.patched`). The patch lowers
  each value again, unshared, which is why the image is 1% more nodes than a
  whole-arena share would give -- 34,428 against 33,860. 0.25 s.

- **The parent no longer lays the parts out at all.** Its merge (0.4 s)
  existed only to give it a runnable arena for the `comp`s and contracts, and
  those need only what they reach. So their images are made from the parts
  (`lower.parts_image`: reachability walked across parts, every other function
  the shared stub, a settled `comp` substituted for its placeholder by the
  rebuild through `:filled`), and the full image is the sharing child's. The
  parent's critical path after the join is now the link head (pools,
  function records), the `comp`s and the types.
- **Each body is shared into its part as soon as it is lowered** -- the first
  of the two changes the previous round designed and did not build. A body
  lowers into an arena of its own, where flags can still be set late; once it
  is finished the optimizer rebuilds it into the part's growing table and the
  arena is dropped. The per-part optimizer run is gone and no table of a whole
  part's unshared nodes is built. With it, "is any child impure?" -- asked of
  every node lowering emits -- is answered by a per-arena flag while no node
  in the arena is impure (`ir.has_impure`), which is most bodies. A part went
  0.92 s -> 0.75 s measured alone.
- **Two ways to check a change like these, and both were used.** The images
  must be byte-identical to what the previous compiler emits from the same
  source (`dreams` and `lucid`), since sharing is canonical in function order
  whatever the parts looked like inside. And one part's lowering can be timed
  alone in a driver, which the wall clock -- ±0.3 s between identical runs on
  this machine now -- cannot resolve. A driver that times a lazy value must
  force it before reading the clock; three measurements this round read 0 ms
  for that reason before it was noticed.
- **Measured, and not kept: making the sharing pass leaner.** It is ~280
  reductions and ~6 KB a node, a third of it collection, and removing a fifth
  of the reductions (a double reversal per run, re-specializing parts) moved
  its time by nothing measurable -- the cost is the map inserts and what they
  promote. A larger nursery did not help either. Only the reversal change was
  kept, because it is simpler and the output is byte-identical.

`Options.parallel` is what turns all of this on, and only the command line
sets it; see the `Options` doc in [dreams/compile.dr](../../dreams/compile.dr).

Where the time is now, wall clock from the start of a self-compile: loaded
1.0 s (parsing 0.7, spread over processes and bound by 1.8 s of parse CPU;
expansion 0.24), resolved 1.45 s, parts joined ~2.4 s, `comp`s settled and
types checked ~3.0 s, the sharing child joined and patched ~3.45 s, written
~3.65 s. The sharing child (0.8-0.9 s from the join) is the critical path at
the end; loading is the largest stage before it.

**Under `just vm-pgo` the same compile is 3.05-3.2 s** (five runs, best
3.06; `DREAM_GC_THREADS=1` once reached 3.00, within the noise). One thing to
know before running that recipe in a fresh container: a fresh `cmake`
configure picks whichever `llvm-config` is first on the path, which here is
LLVM 18, and the JIT needs 20 -- `build-dream` only works because its cache
carries `-DDREAM_LLVM_CONFIG=/usr/lib/llvm-20/bin/llvm-config`. Pass the same
flag to both `cmake` steps of `vm-pgo` or it fails in `jit.cpp`.

**What is left, measured, for whoever takes it to 3 s without PGO:**

- *The sharing child* is the end of the critical path: ~0.9 s from the join
  under contention, 0.75 alone, of which a third is collection. It could
  overlap lowering instead of following it: the parts are dealt round-robin,
  so a sharing process that took bodies in body order, as each part sends
  them, would produce the same deterministic image while the parts are still
  lowering -- at the price of a message protocol and of agreeing the constant
  pools with the parent. Estimated 0.3 s.
- *Dropping the cross-part share* would take ~0.4 s off, and costs 24% more
  nodes in the image (42.8K against 34.6K). That is a decision about the
  output, not a finding, so it has not been made.
- *Parsing* is ~250 reductions a token (lexing ~60 of them), 1.8 s of CPU for
  the compiler's own source; it bounds loading at ~0.7 s on four cores.
- *Expansion* is 0.25 s with one core busy, for 14 macro calls.

## Fewer cores than the machine has: 15.6 s -> 9.1 s on one

Done 2026-09-25. Everything above was measured with four cores to spare, and a
self-compile had never been timed on fewer. `taskset -c 0` -- which is what a
CI slot, a `docker --cpus=1` or a small VM amounts to -- found two things, both
of which the four-core numbers could not show:

- **The VM sized itself by the machine, not by what it was given.**
  `std::thread::hardware_concurrency()` ignores the affinity mask and the
  cgroup quota, so a VM pinned to one core of four still started four workers
  and four collector helpers, and they spent their time taking turns: 15.6 s
  where the same VM told `-j 1` took 11.7. `usable_cores()`
  ([dream/src/cores.hpp](../../dream/src/cores.hpp)) is the smallest of the machine,
  the affinity mask and the CFS quota (cgroup v2 or v1), and the scheduler's
  default and `GcPool` both ask it. `DREAM_CORES=N` overrides it, which is how
  to ask what a smaller machine does without owning one -- though `taskset` is
  the honest version, because it also takes the cores away. 15.6 -> 11.5 s on
  one core, 8.0 -> 6.6 s on two, four unchanged.
- **`SCHED_IDLE` means "never" on a machine with no idle cycles.** The JIT's
  compile thread only ran on idle cycles (see "Four hundred compiles cost more
  than they buy"), so pinned to one core a self-compile compiled **2** of the
  400 functions it made hot, and every benchmark ran interpreted from start to
  finish: `fib` 55 ms -> 900, `collatz` 115 -> 2500, `pi` 50 -> 1100. The same
  happens on any number of cores once a program keeps all of them busy. The
  thread now runs at normal priority while its own CPU time is inside a budget
  -- 100 ms, plus a tenth of what the rest of the process has spent
  (`DREAM_JIT_SHARE` is the tenth, as a percentage) -- and drops to `SCHED_IDLE`
  past it; an idle thread still gets the odd slice, which is where it notices
  the budget has grown back. One core now runs the benchmarks at 1.2-1.8x the
  four-core time instead of 10-20x, and the self-compile 11.5 -> **9.1 s**,
  because compiled code pays for itself even when its compiles are not free.
  Four cores do not move: shares of 0, 5, 10 and 25% were all inside this
  machine's noise there.

**Measured, and not kept: eight parts instead of four.** `lower.part_count` is
fixed so that an image does not depend on the machine, so the only way to give
an eight-core machine more to do is to raise it for everyone. At eight, one
and two cores are unchanged (10.8/10.4 s against 10.7/10.3, 6.7/6.4 against
6.8/6.9) and four are **7-10% slower** (5.06/5.18 against 4.77/4.53) -- the
cross-part share, which is the end of the critical path, has twice the parts
to merge. That is a loss on a machine anyone can measure for a gain on one
nobody here has, so it waits for someone with eight cores to measure it.

## Compile units: a module's walk and lowering, kept between builds

Done 2026-09-30. `dreams --units DIR` keeps each module's part of a parallel
build -- its walk (resolution) and its lowering -- and reads it back when
nothing it was computed from has moved; `mind` passes it on every build
(`$MIND_HOME/units`). On a self-compile with four cores:

| | |
|---|---|
| no cache | 7.8 s |
| cold cache (computes and keeps every part) | 7.9-8.8 s |
| warm, nothing changed | 5.2-5.5 s |
| one body edited in one module | 5.4 s |

and every image equals the one an uncached build of the same source writes,
byte for byte, which `dreams/tests/units.sh` holds after each kind of edit a
key has to notice. What is left of the warm build is what is not cached:
loading and expanding, declaring, the merge, checking (always run), sharing and
emitting.

How it is built, and why each piece is the way it is:

- **Parts are modules.** A body used to go to part `index % 4`, which is
  balanced and says nothing about what changed. Now each module's bodies are a
  part, walked and lowered in a process of its own; that alone costs nothing
  measurable (7.7 s against 7.7 s), and it makes a part a unit an edit either
  touches or does not.
- **Declaring stays whole.** Macros run transformers from any module, `derive`
  specializes a base module's *syntax*, and every part's lowering reads the
  whole program's wrappers, so a module is not compiled alone: the program is
  loaded and declared as before, and what is kept is the per-body work, which
  is most of a build.
- **A walk is kept as what it added** to the state every part starts from
  (`unit.delta`), and read back by laying that over this build's state
  (`unit.restore`). Sound because that state is in the key -- minus `defs`,
  the outline with every declaration's span, which no walk reads and which
  moved every key on every keystroke -- with the modules' names, which the
  walk reads from the loader.
- **A lowering's key is the walk's plus the wrappers the part reaches**, closed
  over what each wrapper stands for. Keyed on the whole table, a wrapper
  moving anywhere re-lowered every part: a wrapper's literal keeps its span,
  and a call site lowered from it carries the span into the image, so its
  callers do change -- but only its callers.
- **Positions are relative to the part.** A part names bodies by where they
  stand in the program's queue, and a body added to an earlier module moves
  every later one.
- **Keys start with the compiler's digest** (`vm.image_digest`), so no unit is
  ever read by a compiler that did not write it.
- **It needed the wire format in C++.** A unit is a hundred thousand nodes, and
  `std.wire` in Dream took 70 s to read one that size; `vm.wire_encode` and
  `vm.wire_decode` write the same bytes in milliseconds.

The trap it walked into, written down because it is the one CLAUDE.md warns
about and it still cost an hour: the keys handed to each part's `spawn!` were
unforced -- even with no cache, when they were `()` -- and a suspension carries
its frame. Every one of the sixty-odd part processes was given a copy of the
whole resolving frame, the loader included: 1.7 GB more copied and twice the
time, invisible to `--time` because `--time` runs serially. `!cache` and `!k`
on `walk_elsewhere!` and `part_elsewhere!` are the fix.

### `--time` timed a pipeline no build runs

It resolved, checked and lowered in one process, in its own order, and so it
could not see a unit cache at all: a warm build and a cold one read the same.
It now forces `compile.build!`'s stages one at a time -- in parts, from
`--units`, with the sharing process -- and says what those cost. Two traps
were found on the way. The first: `strict!` on a lowering made in parts
forces its lazy `:image`, the parts shared in this process should the sharing
process fail, which no build ever asks for. That was three seconds charged to
`lower` that a build never spends, so the timer leaves that key out. The
second: a clock read inside a list element is read when the list is printed,
not where it is written, so the reading is bound first and forced as a
statement.

### Checking kept per part, and the checker built in a tenth of the time

Read honestly, a warm build's largest stage was the type checker: 2.1 s of
it, against 0.7 s for lowering. Its per-body half is now a unit like the
others -- keyed on the part's walk and on a digest of what every body is
checked against (`typecheck.tables`: the signatures, named types, what the
`comp`s came to), so a signature changed in one module checks every part
again and a body changed without one checks its own. `units.sh` has the case
that proves the second half of the key is needed: a signature added to one
module makes an untouched body of another wrong.

That took the bodies from 0.5 s to 5 ms and the stage only to 1.7 s, because
the rest was building the checker, which every build pays. 14% of a warm
build's reductions were `mentions_refine`: whether a named type reaches a
`where` through the names it mentions, walked afresh for each named type down
every path to a depth of eight. `refined_table` builds the same answers a
depth at a time, each depth reading the one below as a table, and building
the checker went from 1.5 s to 0.13 s. That is a gain for every build, cached or
not.

A self-compile with the same VM, runs interleaved, old compiler against new:
no cache 8.5 s -> 7.5 s, and a warm cache 6.5 s -> 4.9 s. Under `--time`, a
warm build is now parse 1.4 s, expand 0.5, resolve 0.6, lower 0.7, types
0.2, share 0.4-1.1, emit 0.35.

### The parse kept per file

Then the parse was the largest stage, and it is the easiest unit of all:
`parser.parse_module` reads nothing but the text, so the key is the compiler
and the text's digest (`modules.parse_unit!`). It is kept by the process that
parses the file ahead of the walk, so a parse read back is read on another
worker too. A parse that failed is kept as well and fails the same way when it
is read back; `units.sh` asks that twice. Warm, parse went from 1.4 s to 0.47
s, and `--time` puts the warm self-compile at 4.1 s: parse 0.47, expand 0.5,
resolve 0.57, lower 0.8, types 0.2, share 0.4-1.2, emit 0.34.

### A cache that forgets

Every edit keeps new units, and a unit is never wrong -- it is keyed on what it
was made from -- only never asked for again, so a cache left alone only grows.
There is no `touch` in the VM, and a build should not need one: a unit read
back is written again once it is a day old (`unit.refresh!`), so its
modification time is within a day of the last build that wanted it, and `mind`
removes the units no build has used in 30 days (`build.forget_units!`). It
looks the directory over at most once a day, by a stamp in it, since a scan
at every build would cost what the cache saves. Two `mind`s forgetting at once
remove the same files, and a unit removed while a build reads it is computed
again.

### A part keyed on what it can see

Until here a walk's key held the whole declared program, so adding a function
to any module walked every module again: 9 s on a self-compile, a cold build's
cost, for the commonest edit there is. Two things in the declared state moved
with every declaration, found by printing a digest of each field and adding one
declaration to the last module:

- **`next_global`**, where the globals a part invents (deforestation's loops)
  are numbered from. Parts now invent from `scope.invent_base`, 2^26, and
  `merge_parts` hands each part a `go` of `g0 - invent_base` plus the parts
  before it -- the same offset every consumer (`opt.remap_leaf`,
  `lower.reach_parts`) already added to a global at or past `invented_from`,
  which is now `invent_base`. A part's walk no longer depends on how many
  declarations there are.
- **The environments.** A part resolves names in its own module's
  environment and, through what that names -- an alias, an `import a.{x}`
  selection, a namespace -- in the environments of those modules and their
  submodules, and nowhere else. `scope.part_key` reads the module references
  off the environment whole (`reached_modules`) and keys the part on those
  environments only.

Also out of the key: the declaring pass's diagnostics and `module_recs`, which
a walk carries and never reads. Before trusting it, every walk read back was
walked again and compared, over a cold build, a warm one, a declaration added
to `main`, to `lower`, to `std.list`, a name renamed across modules, an import
added and a declaration removed: no mismatch, and every image the image built
without units.

A declaration added to the last module now walks one part (4.3 s against a
warm 4.1 s, from 6.4 s); one added to `lower` walks thirteen (5.7 s, from
7.2 s). One added early -- to `std.list` -- still walks everything, because
globals are numbered in module order and every later module's numbers move.
Numbering each module's globals from a base of its own would fix that, at
the cost of a relocation in every consumer of a global index.

### The key was quadratic in modules

The part key came in with a quadratic of its own, which this repository's 67
modules could not show: to find a module's submodules, `part_key` held every
module's name against every prefix the part could see, for every part. On
`scale.py`'s programs of four declarations a module, with `--units`:

| modules | no cache | cold | warm | one body | one declaration |
|---|---|---|---|---|---|
| 400 | 2.5 s | 2.8 s | 1.8 s | 1.8 s | 1.9 s |
| 800 | 4.6 s | 10.5 s | 8.1 s | 7.8 s | 7.8 s |

A warm build of 800 modules was slower than no cache at all. `modules_under`
now indexes every module by each dotted prefix of its name once per build,
and a part's key reads that:

| modules | no cache | cold | warm | one body | one declaration |
|---|---|---|---|---|---|
| 400 | 1.3 s | 1.4 s | 1.1 s | 1.0 s | 1.1 s |
| 800 | 3.3 s | 3.7 s | 2.1 s | 2.1 s | 2.5 s |
| 1600 | 8.2 s | 7.0 s | 4.3 s | 4.7 s | 5.0 s |
| 3200 | 16.3 s | 15.6 s | 9.2 s | 9.4 s | 10.9 s |

Linear, and what is left of a warm build at 1,600 modules is the whole-program
work no unit holds: loading 1,600 files and reading their parses back (1.1 s),
merging the parts (0.9 s), linking (0.9 s), sharing (0.7 s) and emitting
(0.3 s), none of it dominated by one function. Making those per-module too is
dynamic linking's question (docs/dynamic-linking.md), not the unit cache's.

### The whole-program stages, taken down

Then those, one at a time, on the same 1,600-module program, warm:

- **Loading** was path arithmetic. `path.dr` found an extension and a
  directory a character at a time (`last_index` hops between occurrences with
  `str_find` now), and `canonical_name!` made every package's source absolute
  again for every file it named -- three normalisations per package per file.
  The loader carries the packages' roots, worked out once. 25.4M reductions
  became 13.8M.
- **Three loops over the parts read parts by index from lists** --
  `link_head`, `link_parallel!`'s notes and `merge_parts` -- each a walk to
  the part, so each loop quadratic in modules. Arrays.
- **Every part's lowering key was worked out by the process that starts the
  parts**, one after another before any began. Each part works out its own.
- **Sharing the parts** is a whole-program walk no unit holds: rename each
  part's constants, functions and invented globals, and keep each node once by
  its record. In Dream it was a map insert and a `to_string` per node,
  threaded through a state record. `vm.share_arenas` is the same walk in the
  VM -- the Dream walk stays as `optimize_parts_by_hand`, and the compiler it
  built and the compiler `vm.share_arenas` built compile this repository into
  the same bytes. 650 ms became one that finishes while the checker is still
  running.
- **`patched` turned the shared arena back into tables** to fill in the
  `comp` placeholders, and flattened it again: fifty thousand map writes for a
  build with no `comp` at all. Settling writes nothing it does not append or
  overwrite, so it writes into empty tables counting from the arena's end
  now, laid over the arena's lists afterwards.

- **Sorting** was `std.list`'s merge sort in Dream, and `sort_on` asked for
  each key at every comparison. The `sort_keyed` builtin sorts an array by
  keys forced once each, in `compare`'s order, stably; `sort` and `sort_on`
  are it.
- **The node and kid sections** were a function call and a `str_le` a
  field, tens of thousands of times. `vm.node_section` and
  `vm.index_section` write the same bytes, a section a call, and `emit`'s
  first tests hold them to the Dream writers.

Each native came with its Dream walk kept as the definition and a test
between the two, and with the check that a compiler built either way compiles
this repository and the 1,600-module program into the same bytes.

Two things measured and not kept. Sharing only part of the resolution with
the lowering and checking processes saved nothing a real build could see.
Working each walk's key out in its own process, as lowering's is, saved
nothing either: the 285 ms that spawning the walks takes is the walks
themselves, already spread across the cores, not the keys. That attempt also
found the trap again: a key's parts handed in lazily carried the unshared
loader into all 1,600 processes, and the machine killed the build for memory.

Old compiler on the old tree against new on the new, same VM, three
interleaved runs, the least of each:

| | no cache | warm |
|---|---|---|
| the compiler, before | 3.77 s | 2.23 s |
| the compiler, after | 3.18 s | 1.30 s |
| 1,600 modules, before | 5.39 s | 4.33 s |
| 1,600 modules, after | 3.74 s | 2.34 s |

What a warm build of 1,600 modules is now made of, by a timeline put on a real
build: loading 0.7 s, declaring 0.18 s, reading the walks back 0.3 s, merging
them 0.2 s, lowering 0.55 s, checking 0.2 s, writing 0.3 s. Reductions went from
90M to 45M, and nothing left is more than a twentieth of them.
