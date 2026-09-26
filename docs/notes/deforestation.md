# Deforestation

How a pipeline of `std.list` combinators becomes one loop the JIT can take. Moved out of `CLAUDE.md` on 2026-09-25; the prose is as it was, so a date or number in it is as of when it was written.

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

**Where it lives.** Three files. [dreams/fuse.dr](../../dreams/fuse.dr) is the
vocabulary -- which member plays which role, the shape of the plan, and the
syntactic rules. [dreams/scope.dr](../../dreams/scope.dr) decides *whether*, because
only a resolved name can say that the `range` in front of you really is
`std.list`'s and not a parameter, a shadow or another module's; the question
becomes "is this the global index `std.list` gave `range`?", which has one
answer. [dreams/lower.dr](../../dreams/lower.dr) builds the loop, beside the wrapper
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
[mind/std/list.dr](../../mind/std/list.dr) into a shape nobody reading it would
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
  stage that was given a name" in [dreams/fuse.dr](../../dreams/fuse.dr). Moving one
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
