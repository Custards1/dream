# Compiler performance TODO

## Macro expansion: stop compiling the program twice

Priority: was high. **The first two items below are done**, and a separate
round has taken the compiler from quadratic to linear in the number of
declarations -- see "The compiler was quadratic in the size of the program" in
`docs/notes/compiler-scaling.md`. On a generated 6,400-declaration program that is 40.2 s and 6.7 GB
down to **9.8 s and 1.7 GB**, with byte-identical output. Macro cost is no
longer the thing that stops a large project; peak heap is.

The macro work itself is recorded under "A macro reaches a handful of
declarations, not the program" in `docs/notes/macro-expansion.md` -- what landed, what it cost and
what it did not buy. What is left in this section is the part still linear in
the program, and the two redesigns that would remove the rest of it.

Where it stands, 2026-09-20, `build-dream/bin/dream`, default workers and JIT,
non-PGO, alternating runs of the same source:

| | self-compile total | `parse` (where expansion lives) | `std --test` | tax of 1 call, 1600 unrelated decls |
|---|---|---|---|---|
| whole-program snapshot | 6768-7005 ms | 3859-3894 ms | 3522-3875 ms | 2048 ms |
| reachable only | **5238-5265 ms** | **2070-2072 ms** | **2775-2903 ms** | **1059 ms** |

And the tax itself, at the sizes that can see it, before and after the `core`
walk was taken out of `scope.declare!`:

| | snapshot | whole macro tax | whole compile |
|---|---|---|---|
| 3,200 declarations, one call | 229 -> **51 ms** | 267 -> **99 ms** | 3934 -> **3705 ms** |
| 6,400 declarations, one call | 431 -> **93 ms** | 518 -> **184 ms** | 8132 -> **7315 ms** |

Every image is byte-identical to what the previous compiler emitted -- with the
one standing exception `mind/std/all.dr --test` is, for reasons that are not
about any of this work and are written up in `docs/notes/macro-expansion.md` -- and the bootstrap
reaches a fixpoint in one stage.

**What is still linear in the program** is one phase, and it is now smaller
than the two rounds before it left it. It was two: every declaration the
transformer does not reach cost a stub body, a function record and its nodes,
*and* `scope.declare!` walks every declaration for its name. The first of those
is gone -- every unreached global names one shared stub (see "The stub every
unreached declaration shares" in `docs/notes/macro-expansion.md`). The second turned out not to be
a walk for *names* at all: four fifths of it was `declare_core` asking "does
this module mention `core`?" by walking every node of every declaration, an
answer the loader had already worked out and thrown away. It is carried on the
module record now -- "Declaring a program was mostly a search for the word
`core`" in `docs/notes/macro-expansion.md` -- which took `snapshot` from 229 ms to **51** at 3,200
declarations and the whole macro tax from 267 ms to **99**, with every image
byte-identical.

What is left of `snapshot` is what its name says: a name declared, numbered and
recorded, about 16 microseconds each, linear in the declarations. At 3,200 it
is 51 ms of a 99 ms tax on a 3.7 s compile.

- [x] **Measure the macro tax directly.** Done twice. First to the extent the
  last decision needed: `--time` separates `expand` from `parse`, and the tax
  was read as the same program with the `expand` written out, subtracted, at
  three program sizes. Then properly, because this item said to build the finer
  breakdown before the next change and the next change is a redesign: expansion
  charges its own phases now -- discover, snapshot, declare, resolve, lower,
  emit, run, install, each with milliseconds, reductions and bytes, plus rounds,
  wrappers, calls, images, image bytes and VM starts -- and `dreams --time`
  prints it. See "What a macro call actually costs, phase by phase" in
  `docs/notes/macro-expansion.md` for the numbers and for why the meter is off by default.

  **It changed the order of everything below**, which is what it was for. On a
  1,600-declaration program with one macro call the tax is 306 ms, of which
  `emit` is 27 and `run` is 0. Building and loading an image is **8%** of what
  a macro costs a large program; declaring, resolving and lowering the
  declarations no transformer reaches is 83-92%.

- [x] **Compile only reachable transformer dependencies.**
  `scope.resolve_reachable!` walks out from the round's wrappers using the
  resolver's own occurrence table, so aliases, selective imports, helpers,
  closures, captures, recursion and a function named but never called are all
  edges and nothing is approximated syntactically. Unreached declarations are
  stubbed as `1 / 0` rather than dropped, which is what makes an incomplete
  answer loud instead of wrong; every fallback path re-runs the round with the
  whole program (`:whole`). Verified by deleting the dependency edge entirely
  and watching all 47 macro cases still pass, slowly.

- [ ] **Replace snapshot retries with explicit dependency handling.** Track
  unexpanded dependencies and detect cycles. Preserve the existing behavior
  when a macro discards an argument or never demands a dependency; a syntactic
  dependency alone must not force expansion or evaluation. Distinguish a missing
  staged dependency from a real macro exception instead of treating both as
  `1 / 0` and recompiling the program to find out.

  **"Report the failing call once" is done**, and it came free with the
  session rather than from anything on this list. A batch had one result, so a
  raise in it named no call and the program was compiled again to find out
  which; one call per invocation attributes itself. What is left of the retry
  is only the part this item is actually about -- telling a stale snapshot from
  a real exception -- and it is still spelled as "run the round again with the
  whole program a root". See "The image was the calls, and now it is the
  transformers" in `docs/notes/macro-expansion.md`.

- [~] **Radical option: a dedicated incremental compile-time VM.** *Half
  done.* The item said to "prototype an in-memory module/call API against the
  existing VM before considering a separate AST interpreter", and that API now
  exists and is what expansion runs on: `vm.open_image!`, `vm.call_image!`,
  `vm.close_image!` (see "Compile-time evaluation" in
  [docs/builtins.md](../docs/builtins.md), and "The image was the calls, and now
  it is the transformers" in `docs/notes/macro-expansion.md`). Generated wrapper ASTs for the
  *calls* are gone, header patching is gone from this path, and a call's
  arguments cross as values rather than as quoted code. GC ownership is
  explicit and is not the collector's: a handle is an integer naming a slot on
  the runtime that opened it, handles are never reused, and whatever a compile
  leaves open is freed with its runtime. Per-call state is not shared -- each
  call gets a process and a scheduler of its own in the session's runtime.

  **Its ceiling was measured at 8% and it beat that, because the measurement
  was of the wrong thing.** What the phase meter said this would remove was
  `emit` and `run`. What it actually removed was the *arguments from the
  image*, which is `emit` (32 ms -> 10 on the std build) plus the `lower` and
  `resolve` and `declare` of code that no longer exists: 202 ms -> 133 on
  `mind/std/all.dr --test`, 194 -> 147 on a self-compile. The lesson is the
  one this file keeps relearning from the other side -- a phase meter says
  where the time is, not what a change will reach.

  **What is left of this item** is the "incrementally" half: the image is still
  built per round from the staged whole program and thrown away at the end of
  it, so a session is not yet something a *build* keeps. That is item 3 below
  and not this one -- a session that outlives a round is only worth having if
  what is in it is a function of the dependency rather than of the program.

  **One thing to know before extending it.** A batch shared a VM start between
  a module's calls and a session does not, so the std build makes 55 starts
  where it made 2. That is 5 ms against the 79 the change saves, and it is the
  honest cost of per-call attribution -- but it is linear in calls, and a
  program with thousands of them would want `call_image!` to take a list of
  calls rather than one.

- [ ] **Cache compiled transformers across builds.** After dependency tracking
  works, key cached artifacts by transformer source, transitive dependencies,
  compiler/VM format version, target configuration, and relevant build options.
  Validate cold builds, warm builds, and invalidation after helper/import edits.
  Start with executable caching; caching expansion results requires a separate
  account of inputs, generated spans, and compile-time effects.

  **Priced, 2026-09-22, and the price is the reason it is still unchecked.**
  The image is now a function of the program's transformers rather than of a
  round, which is what would make it cacheable at all -- so what a perfect cache
  saves is exactly the phases that build it, `resolve` + `lower` + `emit`:
  **42 ms of a 4,286 ms self-compile (1%)**, 43 ms of 2,200 on `std --test`
  (2%), and 145 ms of 1,129 on a program whose dependency declares 256
  transformers (**13%**). A cache is a new way to be wrong -- a stale one
  expands the wrong program -- and 1-2% does not buy that. The row that would
  is the third one, and nobody here has that project. Note also what a cache
  does *not* reach: the rest of a self-compile's 118 ms tax is `discover` 14,
  `stage` 15, `snapshot` 21 and `sites` 19, which are the *calling* program
  being staged, declared and asked what its own call sites mean, and which only
  item 5 removes.

- [ ] **If implicit dependencies remain too costly, design an explicit
  compile-time module boundary.** Explore separately built macro libraries with
  declared dependencies and a syntax-value interface. This may require a language
  change: document compatibility, migration, and restrictions on calling ordinary
  helpers before choosing it. Compare its cold and warm costs with the incremental
  VM prototype, then choose one architecture rather than maintaining both.

- [ ] **Gate the replacement on correctness and measured cost.** Run macro,
  optional-type, compiler, and language-server tests, plus bootstrap fixpoint
  checks. Cover imported/nested macros, lazy arguments, generated-name resolution,
  helper expansions, cycles, failing calls, source locations, and unrelated
  `comp!` effects executing exactly once. For semantics-preserving changes,
  compare output with the current compiler. Require zero unrelated bodies lowered
  for macro execution and no whole-program serialization per expansion round.
  Proposed timing target: under 10% overhead for 100 trivial expansions versus
  equivalent written-out source; report cold and warm medians over at least five
  alternating runs, with an absolute millisecond budget set from the phase profile.

## Compiling a package at a time

Priority: medium, and it is the one the user named -- packages compiled to `.dream`
or a new `.libdream`, linked rather than recompiled. Everything above makes the
whole-program compile *cheaper*; this is what makes it *unnecessary*.

**What forces it, measured.** Peak live heap is ~2.0x per doubling of the
program and the default per-process cap is 1 GB, so a compile stops at some
size and `DREAM_MAX_HEAP` is the only lever.

**Where that wall is was re-measured on 2026-09-22 and it moved**, which is the
first thing to know before doing any of the work below. It used to be said as
"4,000-5,000 declarations, and the part that is not fixable is that the
compiler is lazy end to end, so every stage's intermediate is live at once, by
construction". The second half of that was an assumption, and a tool that could
break the live set down by object kind said it was wrong: **57% of a
self-compile's peak was map branches**, and not because the compiler holds many
maps -- because a lazily-valued entry pins the version of the map it was stored
in, so the arena was a chain of every version of itself. Forcing what goes into
it took the self-compile's peak from a very steady 409 MB to 225-297 MB (the
range is the collector's geometric thresholds, not sloppiness -- see the
section named below) and a 6,400-declaration program's from 926 MB to 755 MB,
at no cost in wall clock, and a generated
program of **8,000 declarations that could not compile under the default cap
now does** (9,600 too; 12,800 still cannot). See "A lazy value stored in a map
pins the map it was made in" in `docs/notes/compiler-scaling.md`.

So the wall is between 9,600 and 12,800 declarations, not 4,000-5,000, and
about a hundred megabytes of a self-compile's peak is *still* retained versions
that nothing can reach -- the number to watch is map branches divided by map
entries, which is 1.5 here and 0.35 in a map with nothing holding its history.
Holding one package at a time is still the thing that changes the *shape*;
it is no longer the only thing that moves the wall.

- [x] **Generate the benchmark this is missing.** Done:
  `dreams/tests/scale.py`, which varies declarations *or* modules and prints the
  ratio between consecutive sizes. It immediately found a cubic -- `set_env`
  rebuilt the whole module-environment list with a `list.nth` per element, and a
  400-module program went 6744 ms to 4880 ms when it became the `set` opcode.

- [x] **Make `envs` indexable.** Done, and it did not buy what this list
  predicted -- read "A table indexed by module wants to be a map, not a list" in
  `docs/notes/compiler-scaling.md` before the next one, because the reason is the useful part. The
  O(modules) *read* per name resolution, which is what this item was written
  about, is nearly free: it is the `get` opcode, one pointer chase per element.
  What cost was the O(modules) *write*: `set_env` was `list_set`, which
  allocates a cell per element in front of the one it replaces, once per
  declaration. `envs` is a map keyed by module index now.

  Peak live heap at 3,200 declarations over 3,200 modules: **1967 MB -> 594
  MB**, and under the default 1 GB cap that program went from not compiling at
  all to compiling. Time 8371 ms -> 7322 ms there and unmoved below 1,600 modules; the
  self-compile does not move; every image byte-identical and the bootstrap a
  fixpoint in one stage.

- [x] **Make `modules.modules` indexable.** Done, as a map keyed by module
  index plus a count, with `modules l` derived for the three callers that
  really do want the order. On its own it was worth **5.7% of a 3,200-module
  compile's allocation and nothing else** -- not the peak heap, not the clock
  -- against a prediction that it was most of the 24% the list share had grown
  to. The other 18% was `modules.files` and `scope.module_recs`, the same shape
  in two more places, and taking all three took peak heap on the modules axis
  from 623 MB to **409 MB** and made it flat: 364/385/393/409 across 400 to
  3,200 modules, where it had been 372/390/387/623. Read "The three lists the
  loader still grew one entry at a time" in `docs/notes/compiler-scaling.md` -- the transferable
  part is that the *named* candidate was the smallest of the three, and the
  two that mattered were found by measuring again rather than by acting on the
  earlier note.

- [ ] **Decide what a `.libdream` contains.** An image today is closed: every
  index in it is a whole-program index, validated on load
  ([docs/bytecode-format.md](../docs/bytecode-format.md)). A node's operands are
  `NODE` indices, a `closure` names a `FUNC` index, a constant is a `KINT`/
  `KSTR`/`KATM` index, a global names a `FUNC` index, a module record is a
  *range* of `GLOB`. So a package image is the same container with every one of
  those made relocatable, plus two tables the format has no place for yet: what
  this package **exports** (name -> global) and what it **imports unresolved**
  (name -> the site that needs patching). Linking is then a renumbering pass,
  which is a pass this compiler already has in another guise -- `opt.dr` rebuilds
  the whole arena into a fresh one with every index rewritten, and its two
  per-opcode tables are exactly the statement of which operand is an edge and
  which is not. Reuse those tables; do not write a second copy of that knowledge
  (getting one wrong is silent and the image still loads).

- [ ] **Decide what separate compilation costs the optimizations.** Each of
  these is currently whole-program and each needs an answer, not a shrug:
  deforestation asks "is this the global index `std.list` gave `range`?", which
  is cross-package; wrapper lowering rewrites a saturated call of a wrapper into
  what it stands for, which is cross-package inlining; `opt.dr`'s arena sharing
  dedupes across modules and would dedupe less per package; purity is checked
  across imports. The cheap answer for all four is "the linker re-runs it over
  the linked arena", which keeps them exactly as sound as they are now and
  makes a link cost more than a concatenation. Measure that before inventing
  anything cleverer.

- [ ] **Do macros first, as the smallest version of the whole problem.** A
  package's transformers are the one thing a dependent package must be able to
  *run*, not merely name -- which is what makes them the natural first
  `.libdream`, and the reason is the stated goal: a project should not compile
  its dependencies' bodies to expand its own macros. It is strictly smaller than
  general separate compilation (a transformer is a closed program already, and
  nothing links *into* it), and it removes the last whole-program cost
  expansion has.

  **Correction, 2026-09-21: it does not force the relocation question, and the
  ordering argument above was wrong about that.** A transformer library is
  closed and nothing links into it, so there is nothing to relocate: it is an
  ordinary `.dream` image, and the format needs no new section and no
  relocatable index. What it needs is an *export* mechanism and a way in, and
  both of those now exist and are in use -- `GLBL` already carries a name and
  `MODS` already says which globals belong to which module, which together
  make `module.member` a lookup, and `vm.call_image!` is the way in. So the
  relocation work is entirely the `.libdream` bullet above's, and what is left
  of *this* item is not
  a format question at all:

  - ~~build the image from the **transformers** as roots rather than from a
    round's wrappers~~ **Done** (2026-09-22). `scope.resolve_reachable!` takes
    a set of `root_key`s rather than an index, and the wrapper got out of the
    image altogether (`scope.resolve_names!` resolves it on its own and the
    caller keeps `refs` and throws the rest away). One image per *compile*
    rather than per round: `mind/std/all.dr --test` 134 ms -> **107** with its
    two rounds sharing one 51 KB image where they built two totalling 88 KB.
  - keep it across rounds ~~and then across builds~~. Across rounds is what the
    above is. **Across builds is priced and is not worth building yet** -- see
    the numbers under 3 below.

  Both are what is left of the incremental compile-time VM too, which is what
  "pair it with" meant and is now literally true: the call half of that item is
  done and this is the other half.

  **And the roots are a down-payment that has not been repaid.** A program with
  one round sees only the new cost, because the image holds every transformer
  in the program whether this compile calls it or not: the self-compile is
  **8 ms worse**, and on `scale.py --macros N` the tax goes flat-at-20 ms to
  19/28/34/67/100/**159** ms at 1/16/32/64/128/256 transformers. That is 0.5 ms
  per trivial transformer and it is the transformer's own body being lowered,
  not an overhead -- checked against the 12-14 microseconds a node costs to
  lower anywhere else in the same run. It is worth paying only because an image
  that is a function of the program's transformers is the same image on every
  build and so *can* be kept, where one that is a function of the round can
  only be built again. See "One image for the whole expansion" in `docs/notes/macro-expansion.md`.


## What to do next, in order

The modules axis is done: peak heap across it is flat, and the wall a large
project hits is now entirely the **declarations** axis -- 646 MB at 3,200
declarations, 2.5x per doubling, which is the lazy pipeline holding source,
tokens, syntax, the staged copy, the resolver's tables and the arena live at
once. That is not a quadratic anybody can delete, which is what makes 3 and 4
below the answer rather than another round of this.

**This list was reordered on 2026-09-20 by the phase meter**, which is the
whole point of having built it. It used to put the incremental compile-time VM
first, on the reasoning that building and serializing an image is the last
whole-program cost expansion has. That is true and it is 8% of the cost. What
the other 92% is, is a name declared, a stub resolved and a stub lowered for
every declaration in the program that no transformer reaches -- so the thing
to remove is the stub, not the image.

**And 6 was then done anyway, on 2026-09-21, out of order and for more than
8%** -- which is the one warning to take from this ordering exercise. The
meter costs *phases*, so it can only ever price a change as "the phases it
removes", and it priced the compile-time VM as `emit` plus `run`. What the
change actually did was take the **arguments** out of the image, which is a
change to `lower` and `resolve` and `declare` as well, none of which the
prediction mentioned. A phase meter says where the time is. It does not say
what a change will reach, and nothing but building the change says that.

**And item 4 is new, from 2026-09-22, and arrived the same way the `core` walk
did** -- not from any list, but from building the instrument that could see the
thing. Every memory number in this file until then was a total: how much was
live, never what it was. A breakdown of the live set by object kind said 57% of
a self-compile's peak was map branches, which nobody would have guessed and
nothing here could have found by reading code. Two lessons, and the second is
the one to carry: **a total is not a measurement**, and when a number has been
quoted for a year without anyone being able to decompose it, decomposing it is
the work.

1. ~~**One stub, not one per declaration.**~~ **Done** (2026-09-20), and it was
   worth what the meter said it would be. Every unreached global names one
   shared stub, so `check_body`, the function record and the nodes are paid
   once instead of N times. On a 3,200-declaration program expanding one
   one-line macro, `resolve` + `lower` + `emit` went 312 ms to **39** -- 87% of
   what it was aimed at -- and the whole macro tax 546 ms to **275**; the
   self-compile's expand row 415 ms to **242**. Whole compiles are 5-9% faster,
   the new build winning all six interleaved rounds. Every image in the
   repository is byte-identical and the bootstrap is a fixpoint in one stage.
   See "The stub every unreached declaration shares" in `docs/notes/macro-expansion.md` for the
   numbers and for the one design point worth keeping: the stub is
   parameterless and pure whatever the declarations pointing at it were, so
   naming one *raises* rather than handing back a closure.
2. ~~**`scope.declare!`, which is `snapshot`.**~~ **Done** (2026-09-20), and
   not where this item said to look. It guessed that declaring names might be
   shared with the resolve that follows, and that is not what the time was:
   four fifths of `declare!` was `declare_core` walking every node of every
   declaration for the word `core`, an answer the loader had already computed
   and discarded. Carried on the module record, `snapshot` went 229 ms ->
   **51** at 3,200 declarations and 431 -> **93** at 6,400, the macro tax 267
   -> **99** and 518 -> **184**, and whole compiles are 4-9% faster with the
   new build winning all nine interleaved rounds. The self-compile does not
   move, for a reason worth reading: `list.any` stops at the first mention, and
   every module of this compiler names `core` in its first few lines. See
   "Declaring a program was mostly a search for the word `core`" in `docs/notes/macro-expansion.md`.

   What is left of `snapshot` really is names -- one declared, numbered and
   recorded, ~16 microseconds each -- and the macro tax is 2.7% of a
   3,200-declaration compile where it was 6.8%. Nothing in it is above 51 ms.
   **The transferable part is the method**: the phase meter said `snapshot` was
   86% of the tax and stopped there, and only a `--profile` at two program
   sizes said *what inside it*, by naming a function (`mentions_name`) that was
   10% of the whole compile and had never appeared in a profile of this
   repository -- because on this repository it costs nothing.
3. ~~**`.libdream` for transformers**~~ **As far as it is worth taking on its
   own** (2026-09-22). Separate compilation's smallest honest version needed no
   relocation and no new section -- a transformer library is closed and nothing
   links into it -- so it was two things, choosing the roots and keeping the
   image. The roots are the transformers now, which makes the image a function
   of the program rather than of a round: one image per compile, and
   `std --test` 134 ms -> 107. Keeping it *across builds* is the other half and
   it is priced under "Cache compiled transformers across builds" above: 1% of
   a self-compile, 2% of the std build, 13% of a macro-heavy dependency. Do it
   when somebody has the third kind of project; a cache that can be stale is
   not worth 1%.

   **What the pricing also settled is that this was never going to remove 1 and
   2.** The argument for it was that "a dependency's declarations are not in
   this program at all, so there is nothing to declare and nothing to stub" --
   and that is true of the dependency's *bodies*, which is `resolve` and
   `lower`, and false of everything else. The calling program still has to be
   staged and declared, because a call site resolves against the program it is
   written in. `discover` + `stage` + `snapshot` + `sites` is 69 ms of a
   self-compile's 118 ms tax and no transformer library touches any of it. Only
   item 5 does.
4. **The map versions that are still retained.** New on 2026-09-22 and ahead
   of `.libdream` for everything, because it is a hundred megabytes for a
   day's work where that is a month's. `--stats` now breaks the live set down
   by kind at the largest major, with counts, and the number it puts in front
   of you is **map branches per map entry**: 0.35 in a map nothing holds the
   history of, 1.5 in a self-compile. The arena was most of that and is fixed
   (`ir.settled`); what is left has the same shape and the same cause -- a
   value stored into a persistent map before anything forces it, pinning the
   version it was stored in. [dreams/opt.dr](dreams/opt.dr)'s five tables are
   the named suspect: `keep` forces a node only when it happens to be
   shareable, because `to_string` is how it makes the sharing key, so every
   node that is not shareable goes in as a thunk. Measure the ratio before and
   after, not the megabytes -- the megabytes move with the collector's
   thresholds and the ratio does not.

5. **`.libdream` for everything**, if the above has not already moved the wall
   past where anyone is standing.
6. ~~**The incremental compile-time VM**~~ **Half done** (2026-09-21), and
   done out of order on purpose: the half that is the *call* -- an image loaded
   and entered by name with arguments that cross as values -- turned out not to
   depend on anything in 3 or 4, and doing it first is what takes the calls out
   of the image so that 3 has something to cache. It was worth more than the 8%
   ceiling the meter predicted for it, and the reason is worth carrying: the
   meter costed "stop rebuilding the image", and what the change actually did
   was stop *putting the calls in it*. The macro tax is 202 ms -> 133 on
   `mind/std/all.dr --test` and 194 -> 147 on a self-compile, with every image
   in the repository byte-identical bar the 12 generated-span bytes of
   `std --test`, and the bootstrap a fixpoint in one stage. See "The image was
   the calls, and now it is the transformers" in `docs/notes/macro-expansion.md`.

   **And the half that belonged to 3 landed the next day**: the session
   outlives the round now, because what is in it is a function of the program's
   transformers rather than of the round's calls. So a compile opens one image
   and one VM, whatever its macros nest to. What is left is keeping that image
   between *builds*, which is priced above and is not worth building yet.

7. ~~**`discover`, which nothing has ever looked at.**~~ **Done**
   (2026-09-21), and the guess in this item was wrong in an instructive way.
   It proposed "stop numbering and walking items whose span does not contain a
   site" -- which is what `picked` already did -- and then said to measure
   first, because `accounted` falls back to the whole module when the sites
   cannot be matched to items and *how often that happens is not known*. That
   was the question. It happened for **eight of a self-compile's eleven modules
   with sites**, including the two largest in the compiler, and for two
   unrelated reasons:

   - a `match`'s span ended at its first arm rather than at its `}`
     (`parse_match` read `peek_span rest2` where `comma_list` beside it hands
     the closing token back), so every `let f = match ..` was a declaration
     whose span covered its header -- and that is most of this compiler;
   - sites are keyed by *file* and a file is several modules, so a record
     submodule inherits its parent's sites, accounts for none, and walks itself
     to find expansions it does not have.

   `discover` 49 ms -> **14**, 2.5 M reductions -> **0.6 M**, the self-compile's
   macro tax 152 -> **116**, and `lucid` the same (51 -> 14, tax 167 -> 133).
   `mind/std/all.dr --test` does not move, because in `--test` mode
   `std.macros`'s sites are inside declarations that survive. Every image in the
   repository changed in exactly one field -- `span_end` of a `FUNC` record --
   and the bootstrap reaches a fixpoint in one stage. See "Discovery was a walk
   of two whole modules" in `docs/notes/macro-expansion.md`, and note what deleted itself with it:
   `lucid`'s `reach` walk existed solely to work around the same span.

   The axis this item asked for exists now: `scale.py --call-in-module` writes
   the one `expand` into the largest generated module. It says the axis is real
   and cheap -- 2, 3, 5 ms at 400, 800, 1,600 declarations, linear, and the
   *same before and after*, because a generated program never trips the
   fallback. This is the one cost in this file where `dreams` is the expensive
   workload and the generated program is free, which is the exact inverse of
   the warning below.

   What is left is 3 ms: `std.macros` compiled without `--test`, whose sites
   are all inside `when test` blocks the configuration dropped, so they are in
   the file and in no surviving declaration. Measured by forcing the accounting
   true; telling that apart from a real mismatch would need the loader to carry
   the spans it dropped, and 3 ms does not buy it.

**And one warning about the next profile.** `mentions_name` was invisible for
the whole life of this compiler because `dreams` is the one codebase where the
search it makes is free. Profile the *generated* program as well as this one --
`dreams/tests/scale.py` writes it -- and compare the two profiles rather than
reading either alone. A cost that is large on a program of unrelated
declarations and zero here is exactly the cost a large project would hit and
nobody here would ever feel.

**And the same warning read backwards**, which is what item 7 turned out to be.
`work_of`'s fallback was 35 ms on a self-compile and 0 on every generated
program, because what trips it is a `match` and nothing `scale.py` writes is
written as one. Neither program is the check; the pair is. And when a pass has
a fallback, the number to print is not how long it took but **how often it fell
back** -- eight modules of eleven, which no profile would ever have said.

If a fourth round of list-to-map is ever tempting, measure first and measure
the right thing: allocation by kind at two sizes says *whether* something is
growing, and only peak live heap says whether it is the wall. The three tables
above were 18% of allocation and 34% of the peak; `modules.modules` alone was
6% of the first and none of the second.

Do not spend another round on *which bodies* the snapshot resolves. That seam
is worked out: the walk reaches what a transformer reaches and stubs the rest,
and deleting the dependency edge entirely still compiles. What items 1 and 2
are about is the opposite question -- what a declaration costs when it is
stubbed, and what declaring its name costs -- and the meter says those are
where the time is. Item 1 has since answered the first half: a stubbed
declaration now costs a `GLOB` entry pointing at a function somebody else
already built, which is as close to nothing as the format allows. Only the
name is left.
