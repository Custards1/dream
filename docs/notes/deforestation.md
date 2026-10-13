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

**Where it lives.** Three files. [dreams/src/fuse.dr](../../dreams/src/fuse.dr) is the
vocabulary -- which member plays which role, the shape of the plan, and the
syntactic rules. [dreams/src/scope.dr](../../dreams/src/scope.dr) decides *whether*, because
only a resolved name can say that the `range` in front of you really is
`std.list`'s and not a parameter, a shadow or another module's; the question
becomes "is this the global index `std.list` gave `range`?", which has one
answer. [dreams/src/lower.dr](../../dreams/src/lower.dr) builds the loop, beside the wrapper
rewriting it resembles.

**What it covers.** Sources `range`, `replicate` and `enumerate`, and -- since
2026-10-12 -- any list or array the program already has; steps `map` and
`filter`, of `std.list` or of `std.array`; sinks `fold`, `fold_strict`,
`fold_lazy`, `sum`, `product`, `count`, `length` (over a chain with a step),
and the deciding `any`, `all` and `none`, of either, and `std.map`'s
`from_pairs`. A chain of `std.list` stages that ends in a list is one lazy
producer (`:collect`). A chain is one pipeline only while its pieces are one
module's: `array.fold` over `array.map` over an array, never `array.fold` over
`list.map`. Everything else -- `take`, `zip`, `flat_map` -- is *not*
implemented, and a member whose loop is not written simply does not fuse: the
call it always compiled to still works. Adding one means writing its loop by
hand in `lower`, which is the price of not having a `build`/`foldr` mechanism.

What each of 2026-10-12's additions bought, on a million or two elements (the
same answers before and after, under both tiers):

| shape | before | fused |
|---|---|---|
| `fold` over a list | 90 ms | 20 ms |
| `fold` over `map`/`filter` of a list | 470 ms | 30 ms |
| `fold` over an array | 355 ms | 38 ms |
| `any` | 92 ms | 21 ms |
| `all` over `map` | 320 ms | 21 ms |
| `count` | 178 ms | 24 ms |
| `length` over `filter` | 250 ms | 27 ms |
| `fold (fn acc [a, b] -> ..)` over pairs | 210 ms | 70 ms |
| `fold (fn acc [i, x] -> ..)` over `enumerate` | 365 ms | 38 ms |
| `map` over `filter`, answered as a list | 230 ms | 36 ms |

**Deciding sinks.** `any`, `all` and `none` stop at the first element that
decides them, and ask nothing after it -- which is what `list.any` does, and
what `std.seq`'s fold-built `any` does too, since its `||` asks `keep` of
nothing once something matched. `std.array` and `std.str` sign what they derive
from `std.seq` with their own container, because `std.seq` says `:any` for the
sequence and a member that takes a list as readily as an array could never be
told from `std.list`'s by its argument: shorthand `any` over an array was
always ambiguous.

**A pattern for the element.** `fn acc [k, v] -> ..` is how a fold over pairs is
written, and a pattern parameter was refused. The element may be one now --
taken apart in front of the step, checked as the lambda would check it, so a
pair of the wrong length still raises `:match_error` -- and the accumulator may
not. Over `enumerate` a two-name pattern is bound straight from the count and
the head, and the pair is never built.

**Names from the frame around it.** A step that read a local of the function it
was written in -- `fold (fn acc x -> acc + x * k) 0 xs`, `k` a parameter -- was
not fused, because a loop is a global and a global has no captures; most folds
in a real program are like that. Each such name is a parameter of the loop now,
after the three every loop has: read in the caller's frame at the call that
enters the loop (at an offset past the end of any file, so that its resolution
has a key of its own) and handed straight back at every turn, which is the
parameter the JIT keeps in place. Up to eight; past that the pipeline stays as
written. The compiler's own image went from 152 fused loops to 392 with it.

Its own compile did not get faster, and that is the honest boundary of all of
the above: the compiler's loops are maps and lists built a turn at a time, and
a loop that allocates is a loop the JIT does not take, so fusing one replaces
`strict_steps` calling a closure with the interpreter running the loop -- about
the same work. Fusion pays where the loop then compiles: numbers, and lists
walked rather than built.

**A chain that ends in a list** cannot be a strict loop: a list is lazy in its
spine, and an endless one would never end. It is one lazy producer instead --
a function over the source that at each cell asks the `filter`s and conses the
mapped element onto a suspended call for the rest, built as `list.map` builds
its cells -- so the elements and the rest are exactly as lazy as before, and
one list is built rather than one per stage. Two stages or more: a lone `map`
has no list to remove.

**A container as the source** (2026-10-12). A pipeline need not start with a
producer: `fold f acc (map g xs)` over a list someone already has built the list
`map` makes and threw it away, and `fold f acc xs` alone called `f` through a
closure for every element. Neither has a list to remove, but both have a loop to
write, and the loop is the point. Over a list the cursor is what is left of it;
over an array the cursor counts from zero and the array is carried. Nothing is
produced twice, so a named list is as good a source as one written in place. On
two million elements: a `fold` over a list 90 ms to 20, a `fold` over a `map`
and a `filter` of one 470 ms to 30, a `fold` over an array 355 ms to 38.
`from_pairs` did not move -- inserting into the map is its cost, not the list.

The element of a list or an array is not a slot: it is read where it is used,
by the substitution a `map`'s output already used. Making that common found two
bugs the `map` stage had had all along, both in what "where it is used" means:

- **A closure or a suspension made in the loop** copies its captures out of the
  frame when it is made, and the frame's copy of a substituted slot was never
  written. `fold (fn acc x -> list.cons ((fn k -> k + x) 0) acc) [] (map ..
  (range ..))` answered garbage. Such a loop is not fused now
  (`scope.captures_substituted`).
- **A nested function's body was lowered with the loop's substitution still in
  force**, and its own frame numbers slots from zero too: a lambda in a fold's
  step had its first parameter read as the loop's element. `lower_function`
  and a `comp` now lower their bodies with none.

**The seed of a strict fold is forced** (2026-10-12). It was handed to the first
callback unforced, so `fold (fn _ x -> x) (boom 0) [1]` was `1`; a fused loop
forces its accumulator at the top of every turn, the first included. The rule
moved rather than the loop -- a flag for the first turn costs a branch in every
strict loop to protect a seed nobody drops -- and `list.fold` and `array.fold`
force it too, so a fold means the same fused or not. `fold_lazy` is the fold for
a seed that must stay unforced.

Shortcut fusion (`foldr`/`build`, as Haskell does it) remains the general
answer if this ever needs to fuse arbitrary producers with arbitrary consumers.
It was not taken: it costs a rewrite-rule mechanism plus a rewrite of
[mind/std/src/list.dr](../../mind/std/src/list.dr) into a shape nobody reading it would
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
  stage that was given a name" in [dreams/src/fuse.dr](../../dreams/src/fuse.dr). Moving one
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
