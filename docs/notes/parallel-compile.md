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
