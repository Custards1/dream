# Writing Dream the JIT compiles

The interpreter runs any Dream program. The JIT is different: a function is
compiled the first time its entry count crosses a threshold (32 calls by
default, `--jit-threshold` to change it), and only if it passes a fixed
admission test. A function that fails the test is marked rejected and runs
interpreted forever, however hot it is. "Write it so the JIT compiles it"
therefore means "write it so the admission test passes".

The test is the `Analyzer` in

[dream/src/jit.cpp](dream/src/jit.cpp). It rests on one fact about the machine:
**compiled code evaluates a self call's arguments before making the call, and
carries its values in registers with no frames and no thunks.** So the tier
accepts a function only when doing that eagerly cannot change what the program
does — which in practice means the shape the notes in this repository call "the
strict numeric spine": a tail recursion whose parameters are forced, whose body
is arithmetic, and whose calls name only things the tier can enter. Every rule
below is that test read backwards.

## Seeing what compiles

- `dream --stats IMG` ends with the number of functions compiled. Nothing else
  that prints from `--stats` tells you this.
- `DREAM_JIT_TRACE=1` prints one line per refused function with the first
  refusal reason. This is the fastest way to ask why not.
- `dream --dump-jit NAME` prints the LLVM IR the tier generated for one
  function, or the reason it refused.
- `--no-jit` runs everything interpreted. A compiled loop reports *far* fewer
  reductions than interpreted (a machine call spends one reduction where the
  interpreter spent ten), so a function did compile if `--stats --profile 1`
  shows different totals between the two modes but agreeing with each other.

Two things about reading the numbers. A function only gets *considered* once it
is hot, so a loop too short to cross the threshold never compiles and tests
nothing — the JIT tests in [dream/tests/programs/](dream/tests/programs/)
(`jit_natives.dr`, `jit_let.dr`, `recursion.dr`) are all long loops for exactly
that reason. And the tier does not move: *both* tiers must agree, and the e2e
harness runs every program under the JIT and under `--no-jit` and requires
byte-identical output. That agreement, not the rules here, is the contract.

## The compilable loop, as a template

```dream
let rec scan_bytes s i n acc =
    if i >= n { acc }
    else if acc < 0 { acc }
    else { scan_bytes s (i + 1) n (acc + core.str_byte s i) };
```

This is `bytescan` in [benchmark/benchmark/main.dr](../benchmark/benchmark/main.dr),
and it is the shape everything below produces. `i` and `n` are strict because
the bounds test reads them on every path. `acc` is strict because the base case
answers it and the guard tests it. `s` is **carried** — every self call hands it
straight back — so it does not have to be strict even though nothing reads it on
the path that answers early. Every rule below is a reason the tier could say no
to something like it, or to something less.

## Rule 1 — the loop is a tail self recursion

A function that passes the test compiles to one of two shapes: a **tail** self
call is a loop back-edge, and a **non-tail** self call is a machine call (bounded
by the machine stack; see the depth note at the end). Loops are `let rec`:

```dream
let rec loop i n acc = if i > n { acc } else { loop (i + 1) n (acc + i) };
```

That is the whole grammar of a hot inner loop: a condition, a base case, a
strict update. `fib` shows the other shape and compiles too — the tier's notes
in *Making it faster* measure `fib 32` at 0.19x CPython — but the workhorse is
the tail loop. A loop written some other way (an imperative walk, a fold over a
manually built list that does not fuse, an `Array`-indexed walk that reads
`a.[i]`) is a loop the JIT cannot see.

## Rule 2 — force every parameter on every path

This is the licence the tier rests on, and the rule most programs trip over. A
self call evaluates its arguments eagerly, so every parameter must be one the
function was going to force anyway. The tier computes "forced on every path"
over the body's shape:

- an `if` forces its **condition** and, from the arms, only what **both** arms
  force (the intersection);
- `a && b` and `a || b` force only the **left** operand (short-circuit);
- arithmetic and comparisons force both operands;
- a non-final statement of a block forces nothing unless it is impure.

The consequence for `if`: a parameter read in only one arm is not strict. The
canonical miss:

```dream
let rec frame i n acc = if i > n { acc } else { frame (i + 1) n (acc * i) };
```

This one passes — `acc` is read in the base arm and the recursion arm, any
`if`'s arms are intersected, and here both arms read it. The miss is a
parameter read on only *one* path, usually because an `if` without an `else` was
used to make it optional, or because a branch answers without touching it.
Reading it fixes it: touch the parameter in every arm, or move it into the
condition, which is always forced.

## Rule 3 — carry what you walk

The exception to Rule 2, and the reason a loop can walk (or ignore) a string,
list, array or map it does not read on every path. A parameter every self call
hands **straight back — the bare parameter, in the same position, at every call
site** is not evaluated by the recursive call: it is moved, a word from slot `i`
to slot `i`, which is exactly what the interpreter does with it. `scan_bytes`'s
`s` is the shape: read only by the iterating branch, untouched by the arms that
answer.

```dream
let rec sum_bytes s i n acc =
    if i >= n { acc } else { sum_bytes s (i + 1) n (acc + native.str_byte s i) };
```

Rules, and the "every call site" is the part most often missed:

- one self call site that *computes* a new value in the position (say
  `sum_bytes (trim s) ...`) destroys the carry, and the whole function is then
  refused for `s`;
- carried is for the **root** of a compile only. A **peer** — a separate global
  the root calls — is entered by a caller that evaluates its arguments, so a
  peer must force *every* parameter on *every* path; there is no carrying across
  a peer call;
- a carried parameter is still read unforced if the loop body looks at it, and
  it is never forced on entry — `jit_natives.dr`'s `first_byte`, called cold
  with `s = 1 / 0`, is the test that a hot+compiled call did not start forcing it.

## Rule 4 — order the guards so no path leaves a parameter unread

Rule 2 is about *which paths read a parameter*; the order of the conditions
decides what the paths are. In `scan_bytes` the bounds test comes first, and it
is not for speed. Put the `acc` guard in front of it and take the path where the
guard fails: it answers `acc` and never touches `i` or `n` — so `i` is not
strict, and the tier refuses the function. The body the tier can prove things
about is the one where every early answer still reads the parameters it must.
This same ordering is the error-order subtlety written up in *Making it faster*:
compiled code raises at the eager evaluation, so asking "is it past the end"
before "is the accumulator sane" is also how the two tiers agree about which
error a program gets.

## Rule 5 — the body is only arithmetic, literals and locals

The tier can emit exactly this set of opcodes (see `op_is_supported` in
[dream/src/jit.cpp](dream/src/jit.cpp)): fixnum/float/bool/char/unit literals,
parameter and `let` reads, `if`, block, `force`, `+ - * / %`, the comparisons
`== != < <= > >=`, `&& ||`, unary `neg` and `not`.

Everything else refuses the whole function, so a compiled body may not:

- **read or write a container in the loop.** `c.[k]`, `c.[k else d]` and
  `c.[k => v]` are opcodes and compile to nothing here, and neither do list,
  array or map literals or `core.cons`. A loop cannot build or index structure:
  pass the structure *in* as a parameter and ask its natives (Rule 6) questions
  about it.
- **read a global.** A top-level name is a `Global` node; a compiled body has no
  cache and no frame to read one through. `scan_text` in the benchmark is a
  global passed into the loop as an argument, not read by it.
- **make a closure, or a thunk it cannot name.** `Closure`, `Capture`, `ConstAtom`
  (`:foo`) and an integer literal too big for a fixnum all refuse.
- **nest too deeply.** The body may not be more than 256 nodes deep in the IR,
  and every `let` eats a slot from a budget of 64 (parameters included). A tail
  loop keeps the body flat — recursion is how "deeply nested" is avoided.
  A function with zero parameters refuses too (there is no call to write).

The only comfortable shape with a container *in* the loop is reading it through
a native that the tier admits and that returns a number — the `is_empty`/`tail`
walk in `jit_natives.dr`'s `spine` is the instance. A loop whose values are
objects (a list builder, a string fold) is the boundary of the tier; see
"A JIT that can allocate" in the notes.

## Rule 6 — calls must name a function the tier can enter

A compiled body's calls are exactly four kinds, tried in this order:

1. **this function** — the self call of Rule 1;
2. **the five numeric natives it writes out** — `to_float`, `to_int`, `sqrt`,
   `abs`, `floor` are emitted inline rather than called, so they are always fine;
3. **a host native passing Rule 7**;
4. **a peer** — a *saturated call to another global function* that itself passes
   the whole test.

Anything else refuses: a closure, a partial application, a call through a local
or a parameter. The refusal names it: "a call whose callee is not a name".

So when the hot loop needs a helper: **hoist it to a top-level global.** The
helper then compiles as a peer — at most 8 of them, the chain at most 8 deep,
the whole set closed under its own calls, and one more constraint: **a peer may
not itself be a tail-recursive loop.** A loop stays preemptible by writing its
state back to a frame it owns; a peer is entered as a machine call and has no
frame, so a looping peer would hold a worker for as long as the loop ran.
Recursion is fine, because the machine stack bounds it (`JIT_DEEP` hands the
call back). `collatz` is a fused loop calling `collatz_steps` as a peer, which
is why the tier's notes describe that call; `collatz_steps` itself is a
non-tail recursion, a permissible peer.

The inverse is the discipline: **do not take functions as arguments on the hot
path.** `map f xs` where `f` arrived as a parameter is a call through a local —
refused. A literal lambda written *at* the call site of a `std.list` stage can
be fused into the loop instead (last section).

## Rule 7 — host natives: pure, narrow, saturated, and their lazy arguments eager

A call to a `std.native` member (or a `std.core` wrapper that lowers to one) is
admitted on four questions, all in `native_site`:

1. **Pure.** Purity is spelling: a name ending in `!` is impure and refuses the
   function. No printing, writing, or effect inside a compiled loop — accumulate
   and do the effect after. `jit_natives.dr`'s `dots!` is declined for exactly
   this and still runs.
2. **Saturated, of fixed arity, four arguments or fewer.** A partial application
   has no callee to enter; a variadic (like `console.print!`) has no argument
   positions to describe.
3. **Does not vouch for the collector.** `str_concat`, `str_of_chars`,
   `str_of_bytes` and `strict!` walk lazy structures underneath themselves and
   are written to survive a collection; a compiled caller cannot honour that, so
   the tier declines them. A `str_concat` in a loop means the loop interprets.
4. **Every argument in a position the native does not force must be eagerly
   safe** — which is a literal, or a bare parameter/`let` read, *and nothing
   else*, because evaluating anything else could raise where the interpreter
   would never have looked. `map_get` does not force its third argument (the
   default), so `native.map_get m i 0` and `native.map_get m i d` compile while
   `native.map_get m 3 (1 / 0)` refuses the function — and must, or the loop
   would raise instead of answering. This is `jit_natives.dr`'s `defaults`.

The natives one actually meets in numeric loops are the easy ones: `str_byte`
(forgets nothing — both arguments strict), `tail`/`is_empty`/`head`,
`len`, `to_string`, `compare`, `array_get`. `to_string` runs Dream work
underneath itself and is still fine — that is not what makes a native declined.

One you may want but will not get: `core.cons` (and any list builder). It is
pure, binary, non-vouching, so it is *admitted* as a call — but it forces
nothing, so a loop like `upto i n acc = ...; upto (i + 1) n (native.cons i acc)`
fails Rule 2 for `acc`: `cons` claims no parameter, the recursion arm proves
nothing strict about it, and `cons i acc` computes a new value so `acc` is not
carried either. Building a list in a compiled loop is the current boundary of
the tier.

## Rule 8 — `let`s are written where they are read

A compiled body has no thunks, so a binding's value is written out at each place
its name is read:

- **read once or never** — always fine. Never-read is laziness preserved (the
  binding is simply never emitted); read once is evaluated at the read.
- **read more than once** — only when the value is small (at most 8 IR nodes)
  and contains **no call**. Twice-written, the value runs twice; a duplicated
  call could be a duplicated recursion, and nested duplication is how a linear
  body becomes an exponential one. `jit_let.dr` is entirely this rule, both
  sides of it.
- **an impure `let` refuses the function** — an impure binding is a statement,
  run where it stands because its effect has to happen there.

On the hot path, either read a binding once, or restructure so the shared value
is a parameter the loop carries. Note the shape of the rule in *Making it
faster*: a binding nobody reads is never evaluated, and one read twice is
allowed precisely when duplicating it costs nothing — a small expression that
cannot raise or diverge.

## Rule 9 — the compiler can write the loop for you

None of the above requires writing `let rec` by hand. `dreams` fuses a pipeline
of `std.list` combinators — a `range`/`replicate` source, `map`/`filter`
steps, a `fold`/`fold_strict`/`sum`/`product` sink — into a strict tail loop,
and that loop is exactly what this whole document describes:

```dream
let sum_range n = list.fold_strict (fn acc x -> acc + x) 0 (list.range 1 (n + 1));
```

fused, is `loop` from Rule 1: an `if i >= hi { acc } else { loop (i + 1) hi (acc + i) }`
global, which the JIT compiles. The two seams matter: the **function argument
must be a literal lambda written at the call site** (a function passed by name
is a closure call in the loop body, and the tier declines that), and the stages
must be `std.list`'s — resolved by name, so a `range` that a parameter or
another module's member shadows does not fuse. A fused pipeline that reached the
JIT reports `list 0%` under `--stats` and `1 functions compiled`; that pair is
the check it happened. See "Deforestation" in the project notes for what fuses,
what does not, and what the rules are. Mixing fusion with hand-written loops
works fine: `collatz_total` folds over `range` while the fold's function calls
`collatz_steps`, a compiled peer.

## The rules at a glance

| write... | instead of | because |
|---|---|---|
| `let rec` tail recursion | a fold that does not fuse, an `a.[i]` walk | only self-recursion is a loop the JIT owns |
| parameters forced on every path | a parameter read on one path | eager arguments need a callee that forces them (Rule 2) |
| carried walkers | the thing you walk computed per step | carried parameters are moved, not evaluated (Rule 3) |
| bounds test first | guard first | an early answer that skips a parameter unstrictens it (Rule 4) |
| arithmetic, literals, locals | `.[ ]`, container literals, globals, closures in the body | the opcode whitelist (Rule 5) |
| top-level helper globals | functions passed as arguments | a call must name what the tier can enter (Rule 6) |
| pure `std.native`, ≤ 4 args, eager-safe defaults | `!` natives, `map_get` with a computed default | the four native questions (Rule 7) |
| `let`s read once, or small and call-free | a `let` read twice whose value is a call | written-where-read substitutes (Rule 8) |
| a fused `list` pipeline | a manually built list under a fold | fusion *is* the loop (Rule 9) |

## Two things that are true but are not writing tips

- **Depth is now the machine stack.** A compiled recursion that runs the stack
  out returns `JIT_DEEP`, the function is deoptimised, and the rest of the run
  is interpreted — this is why `recursion.dr` and `dive` exist. The interpreter
  cannot run out: its recursion is on the heap, and `force_chain.dr` pins down
  the companion line (compiled code is only *offered* below a nested-force depth
  of 256; a long chain of suspensions finishes interpreted rather than jamming
  the machine stack).
- **The tier moves.** Every rule here is the tier as it stands, and it has
  already widened several times (peers, natives, `let`s, arithmetic division).
  The e2e suite runs every program under both tiers; when a rule changes, the
  tests that hold the old line change with it.