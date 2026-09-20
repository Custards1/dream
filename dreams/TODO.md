# Compiler performance TODO

## Macro expansion: stop compiling the program twice

Priority: was high. **The first two items below are done**, and a separate
round has taken the compiler from quadratic to linear in the number of
declarations -- see "The compiler was quadratic in the size of the program" in
`CLAUDE.md`. On a generated 6,400-declaration program that is 40.2 s and 6.7 GB
down to **9.8 s and 1.7 GB**, with byte-identical output. Macro cost is no
longer the thing that stops a large project; peak heap is.

The macro work itself is recorded under "A macro reaches a handful of
declarations, not the program" in `CLAUDE.md` -- what landed, what it cost and
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
about any of this work and are written up in `CLAUDE.md` -- and the bootstrap
reaches a fixpoint in one stage.

**What is still linear in the program** is one phase, and it is now smaller
than the two rounds before it left it. It was two: every declaration the
transformer does not reach cost a stub body, a function record and its nodes,
*and* `scope.declare!` walks every declaration for its name. The first of those
is gone -- every unreached global names one shared stub (see "The stub every
unreached declaration shares" in `CLAUDE.md`). The second turned out not to be
a walk for *names* at all: four fifths of it was `declare_core` asking "does
this module mention `core`?" by walking every node of every declaration, an
answer the loader had already worked out and thrown away. It is carried on the
module record now -- "Declaring a program was mostly a search for the word
`core`" in `CLAUDE.md` -- which took `snapshot` from 229 ms to **51** at 3,200
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
  `CLAUDE.md` for the numbers and for why the meter is off by default.

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
  `1 / 0` and recompiling the program to find out. Report the failing call once.

- [ ] **Radical option: a dedicated incremental compile-time VM.** Keep a
  transformer executable loaded for the compilation session. Add reachable
  functions incrementally and invoke a function directly with syntax values.
  Eliminate generated wrapper ASTs, full `.dream` serialization, header patching,
  and image reloads from the expansion loop. Prototype an in-memory module/call
  API against the existing VM before considering a separate AST interpreter,
  which would introduce a second implementation of Dream evaluation semantics.
  Define GC ownership, exception cleanup, and state isolation explicitly; reuse
  executable code without accidentally sharing per-call mutable execution state.

  **Measured before building: its ceiling is 8%**, and that is the whole of why
  it is no longer first. What it removes is `emit` and `run`, and on a
  1,600-declaration program with one macro call those are 27 ms and 0 ms of a
  306 ms tax; on a self-compile, 51 ms and 5 ms of 379. A fresh `Runtime`,
  `Scheduler` and heap per call -- the thing this item is mostly written about
  -- does not show up at all at these image sizes. Everything else a round
  spends is telling the session what the transformer reaches, which a session
  needs told exactly as much as an image does. Worth doing after the stub cost
  below, and not before it.

- [ ] **Cache compiled transformers across builds.** After dependency tracking
  works, key cached artifacts by transformer source, transitive dependencies,
  compiler/VM format version, target configuration, and relevant build options.
  Validate cold builds, warm builds, and invalidation after helper/import edits.
  Start with executable caching; caching expansion results requires a separate
  account of inputs, generated spans, and compile-time effects.

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

**What forces it, measured.** Peak live heap is 2.5x per doubling of the
program and the default per-process cap is 1 GB, so a compile stops somewhere
around 4,000-5,000 declarations and `DREAM_MAX_HEAP` is the only lever. Part of
that is fixable (see "What is still superlinear" in `CLAUDE.md`) and part is
not: the compiler is lazy end to end, so the source, the tokens, the syntax,
the effective items, the staged copy for macros, the resolver's tables and the
arena are all live at once, by construction. Holding one package at a time is
the only thing that changes the shape of that.

- [x] **Generate the benchmark this is missing.** Done:
  `dreams/tests/scale.py`, which varies declarations *or* modules and prints the
  ratio between consecutive sizes. It immediately found a cubic -- `set_env`
  rebuilt the whole module-environment list with a `list.nth` per element, and a
  400-module program went 6744 ms to 4880 ms when it became the `set` opcode.

- [x] **Make `envs` indexable.** Done, and it did not buy what this list
  predicted -- read "A table indexed by module wants to be a map, not a list" in
  `CLAUDE.md` before the next one, because the reason is the useful part. The
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
  loader still grew one entry at a time" in `CLAUDE.md` -- the transferable
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
  nothing links *into* it), it forces the export-table and relocation questions
  on something small, and it removes the last whole-program cost expansion has.
  Pair it with the incremental compile-time VM below: a transformer library
  loaded once for the session and called with syntax values is the same thing
  from the VM's side.


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

1. ~~**One stub, not one per declaration.**~~ **Done** (2026-09-20), and it was
   worth what the meter said it would be. Every unreached global names one
   shared stub, so `check_body`, the function record and the nodes are paid
   once instead of N times. On a 3,200-declaration program expanding one
   one-line macro, `resolve` + `lower` + `emit` went 312 ms to **39** -- 87% of
   what it was aimed at -- and the whole macro tax 546 ms to **275**; the
   self-compile's expand row 415 ms to **242**. Whole compiles are 5-9% faster,
   the new build winning all six interleaved rounds. Every image in the
   repository is byte-identical and the bootstrap is a fixpoint in one stage.
   See "The stub every unreached declaration shares" in `CLAUDE.md` for the
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
   "Declaring a program was mostly a search for the word `core`" in `CLAUDE.md`.

   What is left of `snapshot` really is names -- one declared, numbered and
   recorded, ~16 microseconds each -- and the macro tax is 2.7% of a
   3,200-declaration compile where it was 6.8%. Nothing in it is above 51 ms.
   **The transferable part is the method**: the phase meter said `snapshot` was
   86% of the tax and stopped there, and only a `--profile` at two program
   sizes said *what inside it*, by naming a function (`mentions_name`) that was
   10% of the whole compile and had never appeared in a profile of this
   repository -- because on this repository it costs nothing.
3. **`.libdream` for transformers**, which is separate compilation's smallest
   honest version and is what removes 1 and 2 rather than shrinking them: a
   dependency's declarations are not in this program at all, so there is
   nothing to declare and nothing to stub.
4. **`.libdream` for everything**, if the above has not already moved the wall
   past where anyone is standing.
5. **The incremental compile-time VM**, last, because its ceiling is measured
   and it is 8%. It is still the right shape for 3 -- a transformer library
   loaded once for a session and called with syntax values is the same thing
   from the VM's side -- so it is worth doing *with* that and not before it.

**And one warning about the next profile.** `mentions_name` was invisible for
the whole life of this compiler because `dreams` is the one codebase where the
search it makes is free. Profile the *generated* program as well as this one --
`dreams/tests/scale.py` writes it -- and compare the two profiles rather than
reading either alone. A cost that is large on a program of unrelated
declarations and zero here is exactly the cost a large project would hit and
nobody here would ever feel.

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
