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
- [x] Phase 2, step 3 -- a major's marking overlaps the process's own
      reductions (2026-09-12).
- [x] A nested force collects when the frames below it have vouched for their
      locals, which is what took peak RSS from 859 MB to 511 MB (2026-09-12).
      See "Collecting underneath a native".
- [ ] Later -- moving (copying or compacting) collection. This is now the
      whole of what is left: 182 MB live sits in 386 MB of blocks, and no
      threshold reaches a hole.

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
| `DREAM_GC_THREADS` | The most threads one collection may use, the collecting one included. Default `min(cores, 8)`, where the cores are the ones this process may actually use (affinity mask and cgroup quota, `DREAM_CORES` to override; see `dream/src/cores.hpp`); `1` turns the helpers off. |
| `DREAM_GC_PAR_MIN` | Bytes of work under which a collection is not divided. Default 1 MiB for a minor, four times that for a major. `0` divides every collection however small, which is how the race detector gets to see the parallel collector on a program small enough to run under one. |
| `DREAM_GC_TRACE` | Report every collection on stderr as it happens, with the per-thread split of a parallel round. `--stats` gives the totals; this gives their shape. |

## What a new design must not break

- `alloc` must still never collect. Whatever the collector becomes, a native
  or the interpreter holding a raw `Value` across several allocations must
  never lose it mid-call. Collection starts at the per-reduction safepoint in
  `run_process`, and -- under the rule below -- at the one inside a nested
  force.
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

## Collecting underneath a native

A native that walks a lazy structure forces the next piece by running the
machine underneath itself, on the C++ stack (`force_whnf`). For most of the
VM's life that nested loop could not collect, and the comment saying so was
right about the reason: a native holding a raw `Value` across the force would
have had its object promoted out from under it.

The price was not paid in correctness. It was paid in **memory**, and the bill
was large enough to be the single biggest thing in a self-compile's peak. One
`str.concat_all` over the compiler's own image allocated **354 MB of nursery**
that no safepoint could reach, and a minor collection at the end of it found
87% of that already dead. Peak RSS was 859 MB against a live set of about
180 MB; the nursery spike alone was 414 MB of it.

So the rule is earned rather than assumed:

> A nested force may collect exactly when every C++ frame between it and
> `run_process` has said its locals survive one.

A frame says so with `VouchesForGc` ([interp.hpp](dream/src/interp.hpp)), and
the promise it makes is specific: anything it holds across a force lives in
`p.stack` or `p.pins`, where the collector rewrites it, and is re-read
afterwards rather than kept in a C++ local. `force_whnf` *consumes* the vouch
and clears it, so what the forced expression goes on to reach starts unvouched
like anything else; a force nobody vouched for raises `Process::force_pins`
for as long as it runs, and a collection needs that counter at zero. That last
part is what makes the rule hold for a *chain* of frames: an unvouched native
with a vouched one inside it still cannot collect, which is the right answer,
because the unvouched frame's locals are what would break.

`Process::pins` is the other half -- a vector of `Value` the collector visits
like any other root, for the handful of frames that genuinely cannot keep a
reference anywhere else. There are two: the callee a native call must still
name if the native parks, and the spare arguments `do_apply` sets aside when a
native is over-applied.

Three natives vouch today, and all three were already written for it -- they
kept their position on the value stack and re-read the cell after every force,
with comments saying why. They are `str_concat`, `str_of_bytes` and
`str_of_chars`. What changed is that the discipline they were already keeping
now buys something.

**Where a vouch may go**, which is the part that is easy to get wrong. A native
reached through `resume_native` has a known chain above it -- `run_process`,
`step_eval`, `do_apply`, `resume_native` -- and that chain is audited: the two
frames in it that hold a `Value` across the call keep it in `pins`. A helper the
*machine* reaches directly does not. `concat_lists` is the list `+`; it is
written exactly as the three above are, and it still must not vouch, because
`finish_binop` calls it holding both operands in C++ locals, and JIT-compiled
code calls it through `dream_rt_arith` holding its entire frame **in machine
registers**, which the collector cannot see, cannot rewrite, and has no way to
even know about. The frame that cannot survive a collection is the frame that
decides, and it is not always the one nearest the force.

`PinsTheHeap` is the other half of the rule and exists for that case: the three
`dream_rt_*` helpers that can run the machine raise `force_pins` for their
duration, so a compiled frame is an unvouched frame by construction and stays
one however deep the call goes or whatever somebody vouches for later.

What it bought, on the self-compile: peak RSS **859 MB -> 511 MB**, the
nursery's worst overshoot **414 MB -> 34 MB**, and the compile got *faster*
rather than slower, because a heap that fits is a heap that does not page and
does not scan what it is about to throw away. The image is byte-identical.

`dream/tests/programs/collect_under_native.dr` is the regression test: every
list it hands to a native is built *by the walk that consumes it*, and its
`.env` shrinks the nursery so that a program small enough to be a test still
collects ninety times inside those natives. It runs under `test-heap`, so the
verifier walks the graph after each of them, and under `test-races`. The
answers do not depend on when collection happens -- running it with a nursery
large enough that none of it collects mid-walk prints the same bytes.

### What this does *not* fix

Two things, both measured, both recorded so the next reader does not spend the
afternoon proving them again.

**Collecting more often does not help.** Not at all, and this was tested four
ways. The old-space threshold is `live * 3`; tightening the multiplier buys
nothing:

| `live x N` | Held from the OS | Peak RSS | Stopped in collection |
|---|---|---|---|
| 3.0 (today) | 415 MB | 519 MB | 250 ms |
| 2.0 | 390 MB | 519 MB | 263 ms |
| 1.5 | 383 MB | 502 MB | 295 ms |
| 1.25 | 376 MB | 504 MB | 340 ms |

Three times the majors, 36% more time stopped, 9% of the memory back. Capping
the *absolute* growth between majors instead (`live + min(live*2, N)`) behaves
the same -- at a 64 MB cap, held falls to 375 MB and RSS does not fall at all.
Turning the concurrent mark off, which is the one window in which a process is
forbidden to collect, changes neither.

The reason is in the second number `--stats` now prints: **88% of the peak is
allocated**. 363 MB of the 421 MB the heap holds at its high-water mark is
objects that exist -- live, or merely not yet proven dead -- and only 58 MB is
headroom and holes. A trigger can only recover the 58 MB, which is why every
trigger policy lands in the same place. What makes the *allocated* figure so
much larger than the ~182 MB that is genuinely live is that a tighter threshold
collects sooner, leaves its holes behind, and the holes are not consumed before
the next collection needs new blocks: the equilibrium simply re-forms one
collection earlier.

**A bigger nursery does not help either.** Survival through a minor runs 32-58%,
which looks like a nursery too small to let things die -- but it is not.
Doubling the cap to 64 MB moves promotion from 369 MB to 363 MB and costs
23 MB of RSS; 256 MB moves it to 348 MB and costs 221 MB. The survivors are
long-lived, and 32 MB stays the right cap.

### What the live set is made of

Everything in the section below is about the *holes* -- the gap between what a
compile holds and what the OS is asked for -- and it was measured against a
live set nobody could describe. `--stats` reports one now, by object kind, as
of the largest major collection, with the object count and average size beside
each share:

```
; 222356000 live at the largest major, by kind: map 57% (745534 at 169 B)
  list 15% (1412025 at 24 B) frame 11% (219134 at 114 B) map entry 10% (552026 at 40 B) ...
```

A *major*, because a minor never looks at old space, so its survivors include
everything old space happens to be carrying; the figure is therefore usually
smaller than `bytes_peak` on the line above it, and the two are printed
separately so that neither is read as the other. `DREAM_GC_TRACE=1` puts the
top three kinds on every major's own line, which is what shows the shape over a
whole run.

It was built to ask why a large compile runs out of heap, and it answered
immediately and not as expected: more than half of a self-compile's live set
was map *branches*, at five times the count and three times the size a map of
that many entries should have. The cause is not the collector's and is written
up in `docs/notes/compiler-scaling.md` under "A lazy value stored in a map pins the map it was made
in" -- a lazily stored value holds the frame that would compute it, and in a
fold that threads a persistent map that frame holds the map one version ago.
Worth knowing here for two reasons: the ratio of map branches to map entries
is a one-number test for it (0.35 healthy, 1.5 in the compiler before the
fix), and a third of the peak this section calls irreducible was that.

### What is left: the holes

After the change, peak RSS is 511 MB, of which 412 MB is blocks held from the
OS (`--stats` reports this now) and the rest is the VM's own floor -- about
38 MB of it before a program has run at all -- plus the C++ side.

The last full collection of a self-compile finds **182 MB live in 386 MB of
blocks**: 47% occupancy, with 182 MB of that sitting on the free lists (45 MB
of it in the 24-byte class, 46 MB in the 288-byte class, and the rest spread
over twenty more). Only 422 of 5848 blocks are *entirely* empty, so
sweeping cannot hand the memory back -- a 64 KiB block holds around 1300 small
objects, and with 45% of them surviving, essentially no block ever empties. The
dead space is interleaved with the live, one chunk at a time, on 55 segregated
free lists that can neither merge nor move.

That is fragmentation, and the only real answer to it is a collector that
moves old objects: compaction, or the opportunistic evacuation of sparse
blocks. Which is what "Later -- moving collections" below is about, and the
vouching rule above is most of what it needs, because "every frame between here
and `run_process` has vouched" is exactly the precondition for moving an
object a C++ frame might be holding.

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

## Phase 2 -- both steps done

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

### What step 3 took

Marking now overlaps the *reductions of the owning process*: a pool thread
reads the heap while the worker mutates it. What step 1 built carried over
entirely -- the threads, the work splitting, the claim protocol, the atomic
mark bit -- but the mutator is no longer stopped, and that is the whole of
the difference. The design it settled on:

- **Snapshot semantics, young skipped.** At a safepoint the process snapshots
  its roots and seeds the helpers with them; the helpers grey the old graph as
  it was then. Young objects are skipped -- they move at the finalize, so a
  mark on one would name a corpse -- and are reached again there, from the live
  roots. A big object born during the mark has its payload filled in after its
  header is published, so it is born *marked* rather than grey (a helper could
  otherwise scan a half-built body) and is logged for the finalize to scan
  once the fills are done.
- **The write barrier pays the delta.** The mutator keeps storing while the
  helpers work. A store into an object they have not marked yet needs nothing:
  their scan is still to come and will see it. A store into one they have
  already drawn must re-enqueue it, or an edge the snapshot never had is lost.
  `remember_if_old` already sits on every in-place store the runtime makes
  (interpreter, JIT spill slot, C API); during a mark it logs the object on a
  re-walk list *and* hands it to the helpers' shared stack at once, so the
  newly uncovered subgraph is drawn in the background rather than at the
  finalize. The finalize re-walks the log either way, because a store can land
  after the last helper has gone to sleep.
- **The store sites the helpers read became atomic.** Everything a helper can
  read while the mutator writes it needs spelling out: the slot stores (frame
  binds, `force_deep`'s in-place decorations, the JIT spill store, the C API),
  and -- the one the earlier design note always suspected -- the type byte a
  thunk flips to Blackhole, to Indirect, and back, which a helper reads to
  decide what a grey object holds. Reads stay plain where the mutator is the
  only writer; only helper-read/mutator-write pairs are atomic
  (`value_slot_store`/`read_slot<true>`). The one *ordering* edge is the thunk
  update: the target goes in first, and the type flip that publishes it goes
  second with a release, so a helper that sees an Indirect sees its target.
- **The finalize is a minor's shape on a major's mark.** The process may not
  run its own collection of any kind between start and finalize --
  `should_collect` says "finalize" the moment a mark is in flight, and twice
  the usual nursery mark is the only other prompt. The finalize joins the
  helpers, then is the rest of a major: one more trace from the *living* roots
  over the remembered set (an old object already carrying the mark costs a
  claim-skip, so the graph is not walked again), the sweep, the nursery
  emptied, the thresholds refit. The paused phase is that trace plus the
  sweep -- and the sweep is the part that has to stop anyway, since it
  rebuilds the lists the mutator allocates from.

Two things cost more than the reading of the design above suggests. The
round's `size` counts participants, and when the caller does not run the body
it must count the helpers *minus* the caller -- an off-by-one is a trace that
never finishes, waiting for an arrival that was never promised. And the type
byte is read atomically by a helper not because a torn byte matters -- it is
whole one way or the other -- but because a plain read against the mutator's
atomic store is the data race ThreadSanitizer is paid to find. It found that
race first in the test's own mutation loop before the runtime was even
involved: the mutator's *own* store sites had to use the atomic spelling too.

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
- **Done:** Phase 2 step 3 -- a major's *marking* overlaps the process's own
  reductions. The pool's helpers seed from a roots snapshot taken at the
  safepoint and mark the old graph as it then was; young objects wait for the
  finalize, hot-payload big objects are born marked; the write barrier logs
  every store that lands in a marked object and hands it to the helpers at
  once; the store sites the helpers can read are atomic, with the thunk
  update's publish-release the one ordered edge; and the finalize -- the only
  collection the process may run while a mark is in flight -- joins the
  helpers and does the rest of the major in one pass over the delta.
  `DREAM_GC_CONCURRENT` forces (1), disables (0), or lets size decide (default)
  whether a major takes the path, and `--stats` reports overlapped majors and
  the overlap window separately from the stopped time.

  Covered by a unit test that tenures a wide graph, starts a mark, mutates it
  while the helpers are out, finalizes and walks the result; by `test-heap`,
  which with `DREAM_GC_CONCURRENT=1` puts every major of every end-to-end
  program through the path under `DREAM_VERIFY_HEAP`; and by `test-races`,
  which runs the same suite under ThreadSanitizer. The detector's first
  verdict on the first version was a real catch, and a telling one: the plain
  store in the unit test's own mutation loop raced the helpers' atomic read
  of the same slot -- the exact spelling `value_slot_store` exists to enforce,
  before any of the runtime's own sites were even in the picture.
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

- **Measured (2026-09-12), after step 3.** The same self-compile, byte-identical
  output either way, so the two configurations differ only in whether a major
  may hand its marking to the helpers:

  | Config | Stopped in collection | Wall |
  |---|---|---|
  | Off (`DREAM_GC_CONCURRENT=0`) | 324 ms (165 major, 159 minor) | 2618 ms |
  | On (default heuristic) | 312 ms (164 major, 148 minor) | 2529 ms |

  Five of the fourteen majors ran their marking on the helpers, with 10 ms of
  it off the critical path. That smallness is the honest finding, and the
  number that says what step 3 is worth: the mark was already parallel, so the
  overlap only hides the remainder, and the finalize owes a pass over the
  delta that can cost nearly what the overlap saved on a small major. The size
  gate exists for exactly that reason -- the early, small majors of a compile
  still collect the old way, and the late big ones are where the 90 ms comes
  from.

- **Done:** a nested force collects when every C++ frame between it and
  `run_process` has vouched for its locals (`VouchesForGc`, `Process::pins`).
  Peak RSS on the self-compile 859 MB -> 511 MB, the nursery's worst overshoot
  414 MB -> 34 MB, wall time 2.78 s -> 2.66 s, image byte-identical. Covered by
  `dream/tests/programs/collect_under_native.dr` under `test-heap` and
  `test-races`, and by the whole suite. Two things it does *not* fix are
  written down under "Collecting underneath a native", with the numbers:
  collecting more often, and a bigger nursery. Neither helps, and both cost.

- **Next:** moving collections -- the design for which is written down under
  "Later" below, and which is now the only lever left on memory. The 47%
  occupancy of old space at the end of a self-compile is the number to beat.

The VM-side Phase 1 work sits alongside compiler work done in the same session:
a `dreams` bug in lowered guarded match arms (a guarded arm's failing pattern
was dropping the remaining arms) and a source-indexing slip in the loader's
parse cache, both fixed, past a byte-identical bootstrap. Neither is a
collector concern; they are recorded here only so a future reader does not
chase the same shadows.
