# Compiler performance TODO

## Macro expansion: stop compiling the program twice

Priority: was high. **The first two items below are done** -- see "A macro
reaches a handful of declarations, not the program" in `CLAUDE.md`, which is the
record of what landed, what it cost and what it did not buy. What is left here
is the part that is still linear in the program, and the two redesigns that
would remove the rest of it.

Where it stands, 2026-09-20, `build-dream/bin/dream`, default workers and JIT,
non-PGO, alternating runs of the same source:

| | self-compile total | `parse` (where expansion lives) | `std --test` | tax of 1 call, 1600 unrelated decls |
|---|---|---|---|---|
| whole-program snapshot | 6768-7005 ms | 3859-3894 ms | 3522-3875 ms | 2048 ms |
| reachable only | **5238-5265 ms** | **2070-2072 ms** | **2775-2903 ms** | **1059 ms** |

Every image is byte-identical to what the previous compiler emitted, and the
bootstrap reaches a fixpoint in one stage.

**What is still linear in the program**, and it is no longer the bodies:
`scope.declare!` walks every declaration, and every declaration the transformer
does not reach still costs a stub body, a function record and its nodes in the
image that is serialized and handed to the macro VM. That is what the remaining
~1 s of tax on the 1600-declaration benchmark is, and it is what the two
redesigns below would remove.

- [x] **Measure the macro tax directly.** Done to the extent the decision
  needed: `--time` separates `parse`, and the tax is measured as the same
  program with the `expand` written out, subtracted, at three program sizes.
  The finer breakdown this item asked for -- reductions, allocation, images
  serialized, VM starts, per phase -- was not built, because the first two
  numbers already said which change to make. Build it before the next one.

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

Reachable dependencies are done and bought roughly a quarter of a self-compile.
The next real step is the incremental compile-time VM: what is left of the cost
is building and serializing an image at all, and no further trimming of *what
goes in it* can remove that. Do not spend another round shaving the snapshot.
