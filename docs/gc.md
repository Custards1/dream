# Garbage collection

Where the VM's memory management is, and where it is going. This file is the
design for the collector `dreams` grows into -- generational first, then
parallel, then concurrent -- and the log of how far each stage has got. If you
are resuming work on the collector, read this first and then the "Where we
are" section at the end.

The collector today is described in `dream/src/heap.hpp` and `dream/src/heap.cpp`.

## Status

- [x] Design written (2026-09-09).
- [x] Phase 1 -- generational collection within a process heap (2026-09-09).
- [x] Phase 2, step 1 -- the collection of one process divided across several
      threads, still stopping that process (2026-09-12).
- [ ] Phase 2, step 3 -- marking that overlaps the process's own reductions.
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
  collection has no business touching. Both are decided inline, in
  `forward_in`; only a young object, one already forwarded this cycle, or an
  indirection to fold away reaches `forward_slow`. That split was worth about a
  sixth of a compile on its own, because the inline half is two instructions
  and the call it replaced was not. (`forward` itself, the entry point a
  `RootSource` uses, is an ordinary call: it dispatches to one of the two
  compiled shapes of `forward_in`, and the roots are a few thousand.)

- **What this buys.** A minor collection touches only the young generation.
  Roughly 95% of the work in a typical compile is parse, and parse is
  allocation-soup: a steady stream of small lists and strings, most of which
  die before the next line. Where the old collector traced the whole live set
  -- the syntax tree, the module tables, the interned-name maps that are still
  needed at the end -- to find a handful of nursery-sized garbage, the common
  collection now copies only what has been allocated since the last one.
  `--stats` reports minor vs major collections, bytes promoted, and the
  milliseconds the process spent stopped in each kind, because a stall you
  cannot measure has not been diagnosed yet.

Nothing in old space ever moves, which is what makes collection safe at an
interpreter safepoint with no native-stack scanning: nothing the interpreter
holds in a C++ local ever moves (`alloc` never collects, and a major collection
moves nothing at all). `DREAM_VERIFY_HEAP=1` re-traces the whole graph after
every collection and aborts on the first inconsistency; after a minor the walk
additionally requires that no reachable object is still young. (That switch
used to be compiled out unless the build defined `DREAM_DEBUG` or
`DREAM_VERIFY_HEAP`, which no build here ever did -- so `just test-heap`
verified nothing for as long as it existed. It is a runtime check now, read
once into a `bool`, because one predictable branch per collection is not worth
a switch that is off by accident.)

## Collecting on more than one thread

A collection is divided across several threads when there is enough of it to
be worth the handshake. Everything above still holds -- the same generations,
the same promotion, the same sweep -- and the process being collected is still
stopped from its first reduction to its last. What changes is how many threads
are inside the pause.

- **Whose threads.** A small pool of its own
  ([gc_pool.hpp](dream/src/gc_pool.hpp)), started on first use and asleep
  otherwise. The plan below asks for "idle workers join in", and this is that
  with the workers kept apart from the scheduler's: waking a scheduler worker
  for
  something that is not a process would put the collector inside the run
  queue's handshake, the deadlock detector's idle count and `wait_for_all`,
  and would leave the unit tests and an embedding host -- neither of which has
  a scheduler -- with no collector threads at all. What the scheduler does
  decide is *how many*: it hands the pool a pointer to its own count of idle
  workers, and a collection recruits no more helpers than there are cores
  nobody is using. A machine whose workers are all busy running processes
  collects on one thread, which is the right answer.
- **One heap at a time.** The pool is taken by whichever heap is collecting;
  a second one does not queue for it, because queueing would make one
  process's collection wait on another's and the waiting is the thing being
  removed. It collects by itself instead. Every parallel path falls back on
  the single-threaded one, which is always correct.
- **How the work divides.** Each thread has its own grey set and its own
  old-space chunks to promote into. A thread whose queue is long gives half of
  it away to a stack the round shares; a thread with nothing takes a batch
  back. The trace is over when every thread has arrived and all of them have
  run out at once -- a grey object can only be on a thread's queue or on the
  shared stack, and then neither holds one. The sweep divides more simply:
  blocks are independent, so a thread takes the next one by an atomic
  increment, and only the block list itself -- rebuilt once at the end -- is
  anybody's to touch.
- **Promoting from several threads.** Two threads reaching one young object
  must not both copy it: two copies would be two objects, and a thunk forced
  through one of them would still look unforced through the other. So
  promotion is claimed: a compare-and-exchange flips the object's `gc` byte
  from `GC_YOUNG` to `GC_BUSY`, the winner copies and then *release*-stores
  `GC_FORWARDED`, and a loser spins until it sees that bit and uses the same
  copy. The claim is also why the object's header is copied field by field
  rather than inside the payload `memcpy`: `gc` is the one byte of an object
  that another thread may be writing at that moment, so nothing may read it
  plainly.
- **Publishing a copy.** A promoted object is reached two ways -- through the
  forwarding stub, and through whichever slot the collector rewrote to point
  at it -- and both need the same edge. The stub has it from the release above.
  The slot gets it because every slot the collector reads and writes during a
  parallel round is read with `acquire` and written with `release`. Without
  that a thread can see the pointer before it sees the bytes, and read a
  header that still says whatever the chunk held before it was reused. On
  x86-64 those are the same two instructions a plain load and store compile
  to; saying which is not a cost, it is the difference between a collector
  that works and one that works on this machine. `resolve` gets a collector's
  copy for the same reason (`gc_resolve`), because the chain it walks is made
  of exactly those slots.
- **The mark is the claim too.** During a major, `GC_MARK` is set with an
  atomic `fetch_or` and only the thread that saw the bit change scans the
  object. So an object several threads reach is still scanned once.
- **What does not divide.** A list. Scanning one cell yields exactly one more,
  so the queue stays at length one however many threads are watching it, and
  the trace is serial by construction. The collector notices this by itself --
  nothing ever grows long enough to share, the other threads find no work and
  go back to sleep -- which is the behaviour wanted, but it is worth knowing
  that the shape of the heap, not the number of cores, is what decides whether
  a pause shrinks.

Three knobs, all read once:

| Variable | What it does |
|---|---|
| `DREAM_GC_THREADS` | The most threads one collection may use, the collecting one included. Default `min(hardware_concurrency, 8)`; `1` turns the helpers off. |
| `DREAM_GC_PAR_MIN` | Bytes of work under which a collection is not divided. Default 1 MiB for a minor, four times that for a major. `0` divides every collection however small, which is how the race detector gets to see the parallel collector on a program small enough to run under one. |
| `DREAM_GC_TRACE` | Report every collection on stderr as it happens, with the per-thread split of a parallel round. `--stats` gives the totals; this gives their shape. |

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
- Only one process ever touches a heap's objects, so the *mutator's* barrier
  needs no atomic: the collector's threads are the only place atomics appear,
  and they run only while the mutator is stopped. The collection of one
  process must never stop another process: that is what "never stops the
  world" means here, and it is why the thread pool is taken rather than
  queued for.
- A young object is promoted exactly once. Everything the language says about
  sharing rests on it, and with several threads promoting it is a claim rather
  than a fact -- see `GC_BUSY`. A missing claim is not something a test can
  catch: the window is tens of nanoseconds wide. `just test-races` is what
  catches it, and does.
- A `Blackhole` and an `Indirect` are written into the *same* object size as
  the `Thunk` they replace (`static_assert` in
  [value.hpp](dream/src/value.hpp#L250)). Promotion must not change the shapes
  that this in-place overwrite relies on, and it does not: a chunk is copied
  with its bytes and sizes exactly as they were.

## Why generational, why parallel, why concurrent

The generational argument is above and measured: parse is allocation-soup, and
a minor collection's cost tracks what has been allocated since the last one,
not the whole program.

Parallelism divides a pause by the number of threads that can be spared for
it, and costs the mutator nothing, because the mutator is stopped either way.
It is the cheap half: the only thing it needs from the language is that the
roots are precise and that nothing moves except by promotion, both of which
Phase 1 already required.

Concurrency is the expensive half, and buys something different: it keeps a
process's pause from growing with its live set at all, rather than dividing
it. A worker is the only thread that may touch a process, so overlap means a
second thread reading the heap while the worker mutates it -- and that is a
coordination problem with the interpreter, not with the collector.

All three matter most for the long-lived, allocation-heavy processes -- the
compiler, the language server -- that made the decision to start here.

## Phase 2 -- step 1 done, the handshake still to come

The plan was three steps, in increasing order of ambition:

1. **Parallel, still stopping**: when a process collects at its safepoint,
   idle workers join in -- parallel mark, parallel sweep across blocks. Wall
   time shrinks, pause semantics unchanged.
2. **Incremental, same thread**: spend a budget of the mark per safepoint,
   carrying the grey set across slices. No second thread and no atomics, but
   no overlap with reductions either -- the pause is spread, not hidden.
3. **Stop-for-sweep**: marking overlaps the owning process's own reductions.

Step 1 is done, and is described under "Collecting on more than one thread"
above. Step 2 is not planned: it spreads a pause without shortening it, and
step 1 shortens it, so the only reason to want step 2 was as a stepping stone
to step 3 and it is not one.

### What step 1 cost to get right

Three things, each of which cost more than the parallelism was worth until it
was found -- the first of them enough to make the parallel collector slower
than the serial one -- and each found by measuring rather than by reading the
code. They are recorded because every one of them is the same mistake in a
different place: paying a coordination cost per object instead of per batch,
or paying it at all when nobody was waiting.

- **A lock per promoted object.** A thread promotes out of its own batch of
  free chunks and refills the batch under a lock. When a size class had no
  free chunks at all -- which is the common case once the free lists have been
  drained -- every single promotion took the lock to be told so again. A
  per-thread "this class is dry" bit fixed it, and is sound because nothing
  puts a chunk back on a list until the round is over. This one change was the
  difference between a parallel minor collection a quarter longer than the
  serial one and one a little over half its length.
- **Sleeping on an empty queue.** A thread that ran out of work waited on a
  condition variable. The thread that was about to hand work over was running
  *now*, so the wait was usually a fraction of a microsecond and the futex
  round trip was tens of them; on the smaller collections the helpers were
  still being woken when the trace ended. They spin first and sleep only after
  a couple of thousand spins, which keeps a long serial tail from burning
  seven cores to watch one.
- **Asking the wrong question before sharing.** Handing work over was keyed on
  the length of the local queue -- share when it exceeds some floor. But the
  cost of sharing is the wake-up, and the floor cannot see whether anyone
  needs waking. Keyed on "is any thread idle?" instead, with a much lower
  floor, the same collections went from 502 ms to 304 ms.

### What step 3 would still take

Marking must overlap the *reductions of the owning process*, which means a
thread reading the heap while the worker mutates it. What step 1 built is
useful here and not sufficient: the threads, the work splitting, the claim
protocol and the atomic mark bit all stay, but the mutator is no longer
stopped, and that is the hard half.

- **Start** at a safepoint: the worker snapshots the roots and hands the heap
  to the collector threads, then goes back to running the process.
- **During marking** the worker keeps allocating and mutating. The mark adopts
  snapshot semantics: it traces what was reachable when it started. To stay
  sound it needs
  - new allocations marked black (they are younger than the snapshot),
  - the write barrier, while marking, to re-enqueue any value stored into an
    object that has already been marked,
  - and the type flips (`Thunk`/`Blackhole`/`Indirect`) to be atomic or
    ordered, so the marker reads a whole type. This is the one the current
    design is furthest from: the mutator writes `o->type` in place, and while
    the collector only reads it, a torn read is exactly the class of bug
    `DREAM_VERIFY_HEAP` exists to catch.
- **Sweep** still stops the process: it mutates the block list and the free
  lists, which are the things a running mutator touches. So the pause becomes
  "sweep, plus the handshake" -- and the sweep is already the cheap half and
  already divided, which is worth knowing before starting: on the self-compile
  a parallel sweep is about a third of a major and a major is about half the
  collector's time, so step 3's best case is removing roughly the mark, and
  the mark is what step 1 already cut by a factor of two and a half.

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
  collections and bytes promoted.
- **Done:** Phase 2 step 1 -- one process's collection divided across a pool of
  threads, still stopping that process. Parallel promotion behind a claim,
  parallel mark behind an atomic mark bit, parallel sweep by block, per-thread
  chunk lists and carve blocks so that promotion needs no lock per object, and
  a fallback to the single-threaded collector whenever the pool cannot help.

  Covered by unit tests for the promotion path, the "a minor leaves old space
  alone" property, an old-to-young edge surviving only through the barrier
  (that last is exactly the reference a careful reader must trust the barrier
  for), and two cases that build a heap large enough to be collected in
  parallel and check that sharing and the live set come through it intact --
  plus `DREAM_VERIFY_HEAP` walking the graph after every collection, and
  `just test-races` over the whole end-to-end suite with `DREAM_GC_PAR_MIN=0`,
  which puts every collection in every program through the parallel path.
  ThreadSanitizer earned its place here: it found the two ordering bugs in the
  first working version -- a promoted copy published into a slot without a
  release, and `resolve` walking those slots without an acquire -- neither of
  which the verifier or a byte-identical bootstrap could see.
- **Measured (2026-09-12), before.** `dream --stats` on the self-compile: 214M
  reductions, 14 major and 45 minor collections, 1.50 GB allocated, 338 MB
  promoted, 276 MB live at the heap's peak, and **831 ms of a 3197 ms compile
  stopped in collection** -- 26% of the whole run, 344 ms of it in majors and
  487 ms in minors. `--stats` reports this from `os.exit!` as well as from the
  end of `main`, which is what made the numbers reachable at all: `dreams`
  ends by exiting, so anything only `main` printed was never printed for the
  run anyone measures. The pause figure is new here, and is the reason any of
  the rest of this could be judged.

  The number to read alongside it is the promotion: 338 MB out of 1.50 GB,
  against a peak live set of 276 MB. Almost everything promoted is *still live
  when the compile ends*, so it is not premature promotion and no nursery size
  fixes it -- growing the cap to 128 MB moves promotion by a tenth and wall
  time not at all. The collector's cost is scanning what genuinely survives,
  which is a reason to allocate less rather than to collect differently -- and,
  since it is work that has to happen, a reason to do it on more than one
  thread.

- **Measured (2026-09-12), after.** The same compile, same schedule of
  collections (14 major, 45 minor, byte-identical output), varying only how
  many threads a collection may use:

  | Threads | Stopped in collection | Wall |
  |---|---|---|
  | 1 | 831 ms (344 major, 487 minor) | 3194 ms |
  | 2 | 562 ms (263, 299) | 2947 ms |
  | 4 | 395 ms (193, 201) | 2773 ms |
  | 6 | 325 ms (163, 162) | 2735 ms |
  | 8 | 304 ms (153, 151) | 2700 ms |
  | 16 | 303 ms (141, 162) | 2759 ms |

  The unmodified collector runs the same compile in 3197 ms, and the new one
  at one thread in 3194 ms: the serial path is the serial path, which is the
  first thing to check of a change like this. At eight threads the collector
  costs **2.7x less** and the compile is **16% shorter**.

  Two things the table says that the totals do not. The curve flattens well
  before the core count -- eight and sixteen threads are the same compile --
  because tracing is bound by memory and not by arithmetic, which is why eight
  is the default cap and the threads past it are better spent on processes.
  And the *pause* falls further than the wall time does, which is the number
  that matters for `lucid`: what a user notices is the longest stall, not the
  throughput.

- **Next:** Phase 2 step 3, the handshake that lets marking overlap the
  process's own reductions. What it needs is written down above; the honest
  estimate of what it is worth is there too, and it is smaller now than it was
  before step 1.

The VM-side Phase 1 work sits alongside compiler work done in the same session:
a `dreams` bug in lowered guarded match arms (a guarded arm's failing pattern
was dropping the remaining arms) and a source-indexing slip in the loader's
parse cache, both fixed, past a byte-identical bootstrap. Neither is a
collector concern; they are recorded here only so a future reader does not
chase the same shadows.
