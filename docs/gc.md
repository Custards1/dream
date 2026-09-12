# Garbage collection

Where the VM's memory management is, and where it is going. This file is the
design for the collector `dreams` grows into -- generational first, then
concurrent -- and the log of how far each stage has got. If you are resuming
work on the collector, read this first and then the "Where we are" section at
the end.

The collector today is described in `dream/src/heap.hpp` and `dream/src/heap.cpp`.

## Status

- [x] Design written (2026-09-09).
- [x] Phase 1 -- generational collection within a process heap (2026-09-09).
- [ ] Phase 2 -- concurrent marking.
- [ ] Later -- moving (copying or compacting) collection.

## The collector today

Every process owns its heap outright, and nothing in one heap is reachable
from another: messages are deep-copied on send ([scheduler.cpp:337](dream/src/scheduler.cpp#L337))
into a throwaway `Message` heap, and `spawn!` copies the closure in. That is
the whole source of the current simplicity -- no object ever needs a lock or
an atomic field, and lazy thunk update is safe because only one process can
force a given thunk.

The heap is **generational**: two generations inside one process heap, a
**nursery** and **old space**.

- **Nursery allocation.** New objects bump-allocate from dedicated nursery
  blocks. Collection can never run mid-reduction, so bump allocation is safe:
  nothing decides anything about a safepoint at allocation time. When the
  current nursery block runs out the *next* one is taken, and only when there
  is no next one is a block added (never a collection, and never a failure);
  the high-water mark is what *a safepoint check* uses to decide a minor
  collection is due. The cursor is an index rather than a pointer for exactly
  that reason -- a minor collection empties every block and rewinds it to the
  first, and a cursor left at the end would allocate a fresh block per
  collection while the ones it had just emptied went untouched. Nursery-soared
  objects carry a `GC_YOUNG` bit; the rest carry `GC_OLD`. Both live in the
  `gc` byte alongside `GC_MARK` and `GC_FREE`. Large objects (past the top of
  the class table, 4 KiB) allocate straight into old space and tenure
  immediately: they are rare, often long-lived, and copying one once to "save"
  the next copy is a bad swap.
- **How big the nursery is.** Not chosen -- *earned*. A nursery is a bet that
  most objects die young, and its size is how long they are given to do it: too
  small and a collection promotes objects that were about to die anyway, which
  is the expensive mistake, because promotion is a copy and everything copied
  has to be scanned now and swept later. Too large and the collection's working
  set leaves cache, and every process pays for memory it is not using -- which
  a language expecting hundreds of thousands of processes cannot afford as a
  default. So every heap starts at 64 KiB, and a minor collection that promoted
  more than a quarter of what it looked at doubles it, up to 32 MiB
  (`DREAM_NURSERY_MAX`). A process that allocates little never grows at all; a
  compiler chewing through a syntax tree reaches the cap in the first second.
  32 MiB is where the self-compile stops improving: below it collections are
  more frequent for the same promotion, above it the extra memory buys nothing.
- **Trigger.** `run_process` checks `should_collect()` (old space past its
  threshold, or nursery past its high-water mark) once per reduction, at the
  top of the loop ([interp.cpp:1301](dream/src/interp.cpp#L1301)). This is the
  only place a collection can start. `Heap::alloc` never collects
  ([heap.hpp:49](dream/src/heap.hpp#L49)): growing there would invalidate raw
  pointers held in C++ locals by the caller.
- **Roots.** Precise, because the machine has no state on the C++ stack. The
  live set at a safepoint is exactly `frame`, `result`, `exit_value`, `stack`,
  `conts[].v1` and `globals`
  ([process.cpp:88](dream/src/process.cpp#L88)).
- **Minor collection.** Everything reachable in the nursery is **promoted**: its
  whole chunk is copied to old space (header, `aux`, cached hash and all), the
  young bit becomes the old bit, and the slot that held it is rewritten to the
  copy in place. The trace starts from the process roots *and* the remembered
  set (below); old space is neither marked nor swept. Because promotion copies
  every reachable young object -- including everything reachable from old space
  through the remembered set -- the nursery is empty when a minor collection
  ends: there are no old-to-young references left, so the remembered set is
  cleared and the next minor collection needs only what the barrier has logged
  since. This is the assignment-less shape: no survivor ages, no tenuring
  thresholds. A young object that survives a minor collection is old, full
  stop. If a workload wants otherwise, ages in `aux` are the knob to add later.
- **Major collection.** The old design's full-heap **mark-sweep**, unchanged
  except that it sweeps the nursery blocks as well, and nursery blocks empty of
  survivors are handed back whole. Tri-color from the roots into a worklist,
  collapsing `Indirect` chains; then a walk over every block that un-marks the
  survivors, recounts the live set, and threads each dead object onto one of 55
  segregated free lists. Big objects get a block to themselves and the whole
  block is returned to the OS when it dies. The remembered set is ignored (the
  whole heap is the trace). After a collection the old-space threshold is
  `live * 3 + initial`, so a program with a large live set does not collect
  continuously.
- **The write barrier.** Old objects can come to point at young ones, so every
  mutating store to an existing object logs the destination when it would. The
  only in-place stores in the whole VM are:

  1. **Thunk update** -- overwriting a `Thunk`/`Blackhole` in place with an
     `Indirect` pointing at the result ([interp.cpp:948](dream/src/interp.cpp#L948)).
     A thunk that has survived into old space, forced at last, gains a young
     target. This is the big one for a lazy language.
  2. **Frame slot binds** -- `FrameObj::slots()[i] = v` as the block runs
     ([interp.cpp:491](dream/src/interp.cpp#L491), [interp.cpp:856](dream/src/interp.cpp#L856),
     [interp.cpp:1113](dream/src/interp.cpp#L1113)). A frame does not usually
     survive its own call, but a frame held by a long-lived thunk can.
  3. **The list builder's in-place cells** -- the interpreter's last-cons trick
     writes `head`/`tail` on an existing cons to forge a lazy list
     ([interp.cpp:1490](dream/src/interp.cpp#L1490)). The JIT emits the same
     store; its spill slot keeps the barrier too
     ([jit_rt.cpp:52](dream/src/jit_rt.cpp#L52)), as does the C API's
     `dream_array_set` ([capi.cpp:385](dream/src/capi.cpp#L385)).

  Everything else writes only into objects created moments earlier (fresh
  frames, fresh map branches in `builtins.cpp`, `copy_between`'s fresh copies),
  which by definition cannot be old.

  The barrier is therefore a helper, `remember_if_old(dst, value)`
  ([heap.hpp:116](dream/src/heap.hpp#L116)):

  ```cpp
  if ((dst->gc & GC_OLD) && is_ptr(value) && (as_obj(value)->gc & GC_YOUNG))
      remembered_.push_back(dst);
  ```

  It is cheap, it never needs a lock, and the remembered set is per-heap. `dst`
  is logged only once per store; the set is drained (not deduplicated) at minor
  collection by scanning each logged object for young fields.

- **Handing a reference to the collector.** `forward` is asked about every slot
  of every object it promotes and every root, which in one self-compile is tens
  of millions of times, and for most of them the answer is nothing: the slot
  holds an immediate, or an object that is already old and that a minor
  collection has no business touching. Both are decided inline in `heap.hpp`;
  only a young object, one already forwarded this cycle, an indirection to fold
  away, or the verifier recording a root reaches `forward_slow`. That split was
  worth about a sixth of a compile on its own, because the inline half is two
  instructions and the call it replaced was not.

- **What this buys.** A minor collection touches only the young generation.
  Roughly 95% of the work in a typical compile is parse, and parse is
  allocation-soup: a steady stream of small lists and strings, most of which
  die before the next line. Where the old collector traced the whole live set
  -- the syntax tree, the module tables, the interned-name maps that are still
  needed at the end -- to find a handful of nursery-sized garbage, the common
  collection now copies only what has been allocated since the last one.
  `--stats` reports minor vs major collections and bytes promoted, because a
  stall you cannot measure has not been diagnosed yet.

Nothing in old space ever moves, which is what makes collection safe at an
interpreter safepoint with no native-stack scanning: nothing the interpreter
holds in a C++ local ever moves (`alloc` never collects, and a major collection
moves nothing at all). `DREAM_VERIFY_HEAP=1` re-traces the whole graph after
every collection and aborts on the first inconsistency; after a minor the walk
additionally requires that no reachable object is still young.

## What a new design must not break

- `alloc` must still never collect. Whatever the collector becomes, a native
  or the interpreter holding a raw `Value` across several allocations must
  never lose it mid-call. Collection may only start at the per-reduction
  safepoint in `run_process`.
- The header stays one word: `type(1) gc(1) aux(2) bytes(4)`, and `aux` is
  what carries `AUX_DEEP_FORCED`, which a `deep_force` relies on surviving a
  collection. Anything the collector does to an object must preserve `aux`
  and the cached string hash; promotion copies the header and first payload
  word wholesale, so both travel with the object.
- Roots stay precise. The moment a collector moves or copies, every live
  reference at the safepoint is a root and must be rewritten; today the roots
  are exactly the locations in the root source, plus the remembered set, and
  short-lived C++ locals that never span a safepoint.
- Only one process ever touches a heap's objects, so no barrier needs to be
  atomic. The collection of one process must never stop another process:
  that is what "never stops the world" means here.
- A `Blackhole` and an `Indirect` are written into the *same* object size as
  the `Thunk` they replace (`static_assert` in
  [value.hpp](dream/src/value.hpp#L250)). Promotion must not change the shapes
  that this in-place overwrite relies on, and it does not: a chunk is copied
  with its bytes and sizes exactly as they were.

## Why generational, why concurrent

The generational argument is above and measured: parse is allocation-soup, and
a minor collection's cost tracks what has been allocated since the last one,
not the whole program. Concurrency keeps a process's GC pause from growing
with its live set. That is a different and harder problem than parallelism: a
worker is the only thread that may touch a process, so overlap means a *second*
thread reading the heap while the worker mutates it. Phase 2 is where that
coordination happens.

Both matter most for the long-lived, allocation-heavy processes -- the
compiler, the language server -- that made the decision to start here.

## Phase 2 -- concurrent marking

The honest shape: marking must overlap the *reductions of the owning process*,
which requires a second thread reading the heap while the worker mutates it.
The current design is not ready for that, because the worker may flip
`o->type` between `Thunk`, `Blackhole` and `Indirect` -- the marker reading a
type while it changes is a torn read, and torn reads are precisely the class
of bug `DREAM_VERIFY_HEAP` exists to catch.

The plan is a stop-for-sweep collector:

- **Start** at a safepoint: the worker snapshots the roots and hands the heap
  to a shared collector thread, then goes back to running the process.
- **During marking** the worker keeps allocating and mutating. The mark
  adopts snapshot semantics: it traces what was reachable when it started.
  To stay sound it needs
  - new allocations marked black (they are younger than the snapshot),
  - the write barrier, while marking, to re-enqueue any value stored into an
    object that has already been marked,
  - and type flips (`Thunk`/`Blackhole`/`Indirect`) to be atomic or ordered
    so the marker reads a whole type.
- **Sweep** still stops the process: it mutates the block list and the free
  lists, which are the things a running mutator touches. So the pause becomes
  "sweep, plus the handshake," and the pause no longer grows with the live set
  that survives.

Simpler stepping stones if the handshake proves too much, in increasing order
of ambition:

1. **Parallel, still stopping**: when a process collects at its safepoint,
   idle workers join in -- parallel mark, parallel sweep across blocks. Wall
   time shrinks, pause semantics unchanged, zero atomics.
2. **Incremental, same thread**: spend a budget of the mark per safepoint,
   carrying the grey set across slices. No second thread, no atomics, but no
   overlap with reductions either -- the pause is spread, not hidden.
3. The stop-for-sweep design above.

Phase 2 starts as (1), which is a strictly additive step that shares the
mutator coordination of nothing, and graduates to (3) when the atomics and
the barrier are in place.

## Later -- moving collections

Nothing about the language demands immutability of address. The roots are
precise and collection only starts at safepoints, so a *copying young* GC --
nursery as a pair of semispaces -- is a plausible endgame now that generations
exist and their promotion path is proved correct. A fully compacting old
space is a bigger ask: it turns every raw `Value` held across a safepoint
into a hazard, which today is safe only because nothing in old space moves.
The "alloc never collects" rule keeps such pointers out of harm's way during a
call; the implication, that a moving collector must mark or scan before any
C++ local escapes, is what `--verify` will be asked to test first.

## Where we are

- **Done:** Phase 1 -- generational collection within a process heap. Nursery
  and old space, the write barrier at every in-place store site (interpreter,
  JIT spill slot, C API), minor collection at the existing safepoint via the
  nursery high-water mark, promotion preserving `aux` and the cached hash,
  majors sweeping nursery blocks whole, and `--stats` reporting minor vs major
  collections and bytes promoted. Covered by unit tests for the promotion path,
  the "a minor leaves old space alone" property, and an old-to-young edge
  surviving only through the barrier (the last is exactly the reference a
  careful reader must trust the barrier for), plus `DREAM_VERIFY_HEAP` walking
  the graph after every collection.
- **Measured (2026-09-12).** `dream --stats` on the self-compile: 214M
  reductions, 13 major and 48 minor collections, 1.54 GB allocated, 369 MB
  promoted, 320 MB live at the heap's peak. `--stats` reports this from `os.exit!` as
  well as from the end of `main`, which is what made the numbers reachable at
  all: `dreams` ends by exiting, so anything only `main` printed was never
  printed for the run anyone measures.

  The number to read is the promotion: 369 MB out of 1.54 GB, against a peak
  live set of 320 MB. Almost everything promoted is *still live when the
  compile ends*, so it is not premature promotion and no nursery size fixes it
  -- growing the cap to 128 MB moves promotion by a tenth and wall time not at
  all. The collector's remaining cost is scanning what genuinely survives,
  which is a reason to allocate less rather than to collect differently.
- **Next:** Phase 2, starting with stop-the-world parallel mark/sweep across
  idle workers (step 1 above), then the handshake. On the numbers above a
  compile spends roughly a fifth of its time collecting, nearly all of it in
  the scan of promoted objects -- which is the part step 1 parallelizes.

The VM-side Phase 1 work sits alongside compiler work done in the same session:
a `dreams` bug in lowered guarded match arms (a guarded arm's failing pattern
was dropping the remaining arms) and a source-indexing slip in the loader's
parse cache, both fixed, past a byte-identical bootstrap. Neither is a
collector concern; they are recorded here only so a future reader does not
chase the same shadows.