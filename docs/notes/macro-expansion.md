# What macro expansion costs

Expansion is a compile: the rounds of work that took its cost from per-call to per-program, and the phase meter that guided them. Moved out of `CLAUDE.md` on 2026-09-25; the prose is as it was, so a date or number in it is as of when it was written.

## Expanding a macro is a compile, and there used to be one per call

**What it does.** `expand twice n` is answered by *running* the macro, and the
only thing that knows how to run Dream is the VM — so expansion stages the
whole program, compiles it, appends the call as a temporary entry, emits an
image and hands it to `vm.eval_image!`, exactly as `comp!` does. That is a
compile of the program per call, and the compiler had been doing precisely
that: a 10-declaration file cost 1.13 s against 0.30 s for the same file with
the macros written out, and `mind/std/all.dr --test` — whose `std.macros` test
block is forty `expand`s in one declaration — cost **38.2 s**.

Two changes, each removing one of the two O(program) costs:

| | std tests | 10 declarations, one call each |
|---|---|---|
| before | 38.2 s | 1.13 s |
| one snapshot per *declaration*, wrapper linked incrementally | 7.3 s | 1.12 s |
| + one image per *round* rather than per call | 2.05 s | 1.16 s |
| + one snapshot for the whole program | **2.15 s** | **0.53 s** |

The two rows move different workloads, which is the thing to understand before
changing either: **the snapshot is per declaration and the image is per call**,
so a declaration with forty calls in it was paying forty images and one
snapshot, and forty declarations with one call each were paying forty of both.
The same file with no macros at all compiles in 0.27 s, so what is left of the
overhead on the second workload is 26 ms a call where it was 86.

**And that last sentence is how the remaining factor of two hid for a year.**
26 ms a call is what a *ten-declaration* file says, and the cost being
measured is the size of the **program**, not of the file -- so the only
workload in this table that could have shown it is one whose program is big,
and neither of them is. On `dreams` compiling itself, which is a megabyte,
the same overhead is **390 ms a declaration**, and macro expansion was 7 s of
a 13 s compile. Beware a benchmark that holds constant the thing the cost is
proportional to. The row below is the fix.

- **One image per round, not per call** (`evaluate!` in
  [dreams/expand.dr](../../dreams/expand.dr)). Every call of one declaration is
  compiled against the same snapshot, so the wrapper's body is a *list* of them
  and the arena is serialized once. `emit.to_binary` was 36% of the compile at
  that point and is the part whose cost is the whole program rather than the
  call. A *round* is the nesting of calls and not their number: a macro's
  answer is punched for `expand`s again and those go in the next image, so
  `expand with_ok x .. (expand with_ok y ..)` is two rounds and forty calls
  written side by side are one.
- **One snapshot for the whole program** (`prepare!`, and the retry in
  `expand!`). This is the one that needed an argument rather than a
  rearrangement. The snapshot goes stale the moment a declaration is expanded,
  and a macro *can* demand a declaration that has been — a macro that calls a
  helper whose own body was written with `expand`. What makes reuse safe is
  that going stale is never silent: an unexpanded call is staged as `1 / 0` and
  a macro is pure, so it cannot catch what that raises. So a declaration that
  fails *at all* is expanded a second time against a fresh snapshot, starting
  again from the state before the first attempt so that a failure which is real
  is still reported exactly once. A program whose macros all succeed builds one
  snapshot; one whose macro genuinely fails builds two and fails the same way,
  which is a compile that was not going to finish.
  `dreams/tests/macros.py` holds that line, and the test was checked against a
  build with the retry removed — without it, it fails with
  `macro evaluation failed: the compile-time expression raised`.

**Failure is the slow path, deliberately.** A batch has one entry and one
result, so a raise anywhere in it is one failure for the whole image and says
nothing about which call raised. The alternative — generate a `try` around each
call — means naming `strict!` and `to_string` in the wrapper, and a module is
free to declare a global of either, which would win (a builtin loses to a
global; see [dreams/builtins.dr](../../dreams/builtins.dr)). So a batch that fails is
thrown away and its calls are run one at a time, where the wrapper *is* the
call and every diagnostic is the one it gave before batching existed.

**What did not change**, and is worth checking after any change here: the image
a macro-using program compiles to. `mind/std/all.dr --test` and the benchmark
above both come out byte-identical to what the pre-change compiler emitted,
except for the `SPAN` entries of generated functions — expansion hands out
offsets from a reserved part of the 32-bit space and the order it hands them
out in moved. Eight bytes of an image of 216,736.

## A compile-time expression is the program with a different entry

The same mistake as the macro one above, in the other pass that runs the
compiler at compile time, and found the same way. `settle_comps!` evaluated
each `comp` by handing `vm.eval_image!` a freshly serialized image of the
whole program -- and every one of those images differs from every other in
exactly one field, the entry point, which the format keeps at a fixed offset
in the header. So the program is serialized once and each expression gets
those four bytes changed (`emit.with_entry`). A self-compile has about twenty
`comp`s and was paying twenty copies of itself for them.

Beside it, `settle_comp_order!` now settles the expressions in dependency
order, so a `comp` may read what another `comp` produced; the image is rebuilt
only when something still waiting depends on the one just settled, which for a
program whose compile-time expressions are independent -- nearly all of them --
means never.

## One image per round of the whole program

**What it does.** The section above got the image down to one per
*declaration* and stopped, and left a note saying the rest would need "an
image format that can be patched rather than rebuilt". It does not. What it
needed was for the round to belong to the program instead of to the
declaration.

The declarations of a program do not depend on one another's expansion: each
is staged against the same snapshot, in which every unexpanded `expand` is
the same `1 / 0`. So their rounds are the same rounds, and a round is one
image for the whole program. `rounds_all!` punches every declaration at once,
groups the calls by module -- a call resolves against the imports of the
module it was *written* in, so the modules cannot share a wrapper -- declares
every module's wrapper together, resolves once, and emits **one** image.
Each module then enters that image at its own wrapper by patching the four
header bytes that say where to start (`emit.with_entry`), which is the patch
the old note thought the format could not take. A program's round count is
the deepest nesting any one declaration has, which is almost always one.

| | dreams self-compile | 40 declarations | 10 declarations |
|---|---|---|---|
| one image per declaration | 13.34 s | 0.88 s | 0.48 s |
| one image per round | **7.06 s** | **0.43 s** | **0.37 s** |

Reductions for the self-compile went 402M -> 206M and `node_bytes` -- writing
node records into an image -- from **30.1% of everything the compiler did** to
7.4%. The gap widens with the number of declarations, which is the shape to
expect: the old cost was declarations times program, the new one is program.

**What licenses it** is the argument `prepare!` already rests on, asked of a
batch rather than of one declaration. A wrapper that needs something this
image does not have raises, because an unexpanded declaration is `1 / 0` and a
macro is pure and cannot catch it -- so it cannot answer *wrongly*, only fail.
A failure means one snapshot could not serve the whole program, and only the
original order can say whether that is staleness or a real error, so the batch
is thrown away and the program is expanded again the old way, declaration by
declaration from the original loader (`sequential!`). A build that was going
to fail fails identically, having spent the extra time only because it was
failing. `dreams/tests/macros.py` is what holds that, and the bootstrap's byte
equality is what holds the batch agreeing with the sequential path.

**Where the rest of it went.** Of the 4.0 s `--time` now charges to `parse` on
a self-compile, about half is parsing and half is the one snapshot -- a full
resolve and lower of the program, which is what "expanding a macro is a
compile" costs and is paid once. `vm.eval_image!` per module is what is left
of the per-call cost, and it is small enough not to show.

## A macro reaches a handful of declarations, not the program

**What it does.** The two sections above got the *image* down to one per round.
What they left was the snapshot: `prepare!` resolved and lowered the whole
program so that a transformer could call anything, and it did that whether the
macro called two helpers or two hundred. On a self-compile that was the whole
compiler -- 2,264 functions -- resolved and lowered a second time so that
eighteen one-line trees could be rewritten.

A transformer does not reach the program. It reaches its own definition and
whatever that names, which is a handful of declarations. `scope.resolve_reachable!`
walks out from this round's wrappers, resolving a body at a time and following
the references that body turned out to have, and resolves nothing else.

| | self-compile | `std --test` | one call, 1600 unrelated declarations |
|---|---|---|---|
| whole-program snapshot | 6768 ms | 3522 ms | tax 2048 ms |
| reachable only | **5238 ms** | **2903 ms** | tax **1059 ms** |

`--time` says where it went: `parse`, which is where expansion lives, is
3859 ms -> **2070 ms** on a self-compile, and the stages after it do not move.
The third column is the macro *tax* in isolation -- the same program with the
`expand` written out, subtracted -- and it halves. It is still linear in the
program, because what is left is the part that is not a body: `declare!`, and a
function record and a stub node for every declaration in the image.

**What "reaches" means is the resolver's answer, not a syntactic one.**
`check_body` already records what every name occurrence resolved to, so an
alias, a renamed selective import, a global that merely *names* a function
without calling it, a capture and a chain two modules deep are all edges, and a
builtin, a host member, a parameter and a local are all correctly not. Nothing
here re-implements name lookup, which is the mistake that would make this
subtly wrong in the cases nobody writes a test for.

**What makes it sound** is that a declaration the walk does not reach still gets
a body, and the body raises. It needs one at all because a global of kind
`function` names a function index and the VM rejects an image whose global names
none (`global names an out-of-range function`) -- so "do not resolve this" has
to be spelled as "resolve something trivial", and the only real question is
what. It is `1 / 0`: the same placeholder an unexpanded `expand` is staged as,
for the same reason. A macro is pure, so it cannot catch it. So an incomplete
reachability answer cannot be silently wrong -- it raises, the batch is thrown
away, and the round is run again with every declaration a root, which is the
whole program (`:whole` in [dreams/expand.dr](../../dreams/expand.dr), and every
fallback path sets it: `one_by_one!` and `sequential!` both).

**That was measured rather than argued.** A compiler built with the dependency
edge deleted -- a walk that finds *nothing*, so every declaration but the
wrapper is stubbed -- still passes all 47 macro and record cases and still
compiles itself, because every batch fails and every failure falls through. It
is slower and it is not wrong. What it produces differs from the batch path in
18 bytes of 374,456, which are the generated `SPAN` entries the section above
already says the two paths disagree about.

**What to expect of the images.** Byte-identical, everywhere: the bootstrap
reaches a fixpoint in one stage, and `mind/std/all.dr --test` comes out the same
file it came out before. That is the test worth running after any change here,
because expansion deciding differently is exactly the failure this pass could
have.

**One behaviour did change**, and it is an improvement rather than a
compatibility note: an error in a body no macro reaches no longer refuses the
expansion. It used to be reported twice -- once as `cannot compile macro: ...`
at the `expand`, and again as itself by the real compile -- because `run!`
refuses a round whose resolution has any error in it at all, and the resolution
was the whole program. Now the round only sees the bodies it resolved, so the
error is reported once, where it is.

## What a macro call actually costs, phase by phase

The three sections above each removed a whole-program cost from expansion, and
each was justified by one number: what `--time` charged to the `expand` stage.
One number cannot say which of the things a round does is the one still worth
removing -- and the change proposed next for this, a compile-time VM kept
loaded for the session and called rather than an image built and loaded per
round ([dreams/TODO.md](../../dreams/TODO.md)), is a redesign of two of them. So the
phases charge themselves now, and `dreams --time` prints the breakdown under
the stage table for any program with a macro in it.

What a round does, in order: **discover** the declarations with an `expand` in
them, **stage** the program (every `expand` replaced by `1 / 0`, every `macro`
read as the `let` it is), **snapshot** it (`scope.declare!`), **declare** this
round's wrappers, **resolve** what the transformers reach and stub the rest,
**lower** that to an arena, **emit** it as an image, **run** it on a VM, and
**install** what came back. `discover`, `stage` and `snapshot` are paid once
for the program; the rest once per round, and a round is the *nesting* of macro
calls rather than their number.

`stage` and `snapshot` were charged together as `snapshot` when the table below
was taken, and were split once that pair was 85% of the tax and "which of the
two?" was the only question left. The answer is that staging is 2-4 ms of it
and declaring is all the rest -- see "Declaring a program was mostly a search
for the word `core`" above for what happened next.

| | tax | discover | snapshot | declare | resolve | lower | emit | run | install |
|---|---|---|---|---|---|---|---|---|---|
| `dreams` compiling itself | 379 ms of 4786 | 52 | 49 | 26 | 93 | 100 | 51 | 5 | 3 |
| `std --test` | 392 ms of 2519 | 4 | 30 | 17 | 117 | 144 | 60 | 10 | 10 |
| 1,600 declarations, one call | 306 ms of 2353 | 0 | 124 | 0 | 76 | 79 | 27 | 0 | 0 |
| 3,200 declarations, one call | 587 ms of 4309 | 0 | 227 | 0 | 147 | 169 | 44 | 0 | 0 |

The last two are `dreams/tests/scale.py`'s program, which is a one-line
transformer called once in a program of N unrelated declarations -- the macro
tax in isolation, and the shape a large project has.

**It agrees with the measurement it replaces.** The tax used to be read as the
same program with the `expand` written out, subtracted. Three interleaved pairs
of the 1,600-declaration program: 309, 497 and 382 ms by subtraction, against
316, 429 and 295 ms by the meter. The same number to within what a two-second
compile on this machine can resolve -- and only the meter says where it went.

**Running the macro is not the cost, and neither is serializing the image.**
`run` -- a fresh `Runtime`, `Scheduler` and heap per call, which is the whole
of what `vm.eval_image!` does -- is 5 ms of a self-compile's 379 and 0 ms on
either generated program. `emit` is 7-15%. Together they are **8% of the tax on
a large program**, and they are precisely what a compile-time VM loaded once
and called would remove. Everything else would stay where it is, because a
session still has to be told what the transformer reaches, and being told is
`snapshot`, `resolve` and `lower`.

**The cost is per declaration, and it is the declarations nothing reaches.**
Those three are 83-92% of the tax and every one of them is linear in the size
of the program: double the declarations and the tax goes 306 ms to 587 with
each phase doubling under it. On the generated program a transformer reaches
exactly one declaration, so essentially all of that is what the *other* 3,199
cost -- a name declared, a stub body of `1 / 0` resolved, and that stub lowered
into a function record and its nodes, three thousand times over, so that one
one-line macro can be run. The stub exists because a global of kind `function`
must name a function the image has (see the section above); nothing says it
must name a *different* one for every declaration, and that is the next thing
to try.

**How to read it yourself.** `dreams --time FILE` on anything with a macro. The
meter is off unless a tool asks (`modules.metering`), and the reason is not the
clock reads: a phase is charged around a `strict!`, and in a lazy language
forcing a phase is a decision about *when* it happens. Expansion depends on
that in one place -- a round that refuses every module before running one never
forces the image, and so never builds one -- which a meter that charged the
image would take away. Each reading is taken twice and the second subtracted,
for the reason `main.dr`'s `timed!` gives: forcing a phase walks everything the
phases before it built, and the walk is not this phase's work.

Off, it costs nothing measurable: the same compiler with the meter in it and
the commit before it, alternating on `mind/std/all.dr --test`, came back
2534/2597/2593 ms against 2519/2568/2589 -- the metered build nominally faster
in all three, which is how a difference inside the noise floor looks. Every
image in this repository is byte-identical either way.

## The stub every unreached declaration shares

**What it does.** The section above resolves only what a transformer reaches
and stubs the rest, and the section before that says why a stub has to exist at
all: a global of kind `function` names a function index, and the VM rejects an
image whose global names none. So every declaration the walk did not reach got
a body of `1 / 0` -- *its own*, resolved into its own function record and
lowered into its own nodes, three thousand times over so that one one-line
macro could run.

Nothing says two globals may not name the same function. A `GLOB` is a name, a
kind and a `FUNC` index, and the VM checks only that the index is in range. So
the stub is declared once and every unreached global points at it
(`shared_stub` and the rewrite at the end of `resolve_reachable!` in
[dreams/scope.dr](../../dreams/scope.dr)), which takes the per-declaration work out of
three phases at once: `check_body` does not run, no function record is built,
and no nodes are lowered or serialized.

`dreams --time`, the meter the section above exists to provide:

| | expand | resolve | lower | emit | the macro's image |
|---|---|---|---|---|---|
| `dreams` compiling itself | 415 -> **242 ms** | 93 -> **25** | 97 -> **24** | 56 -> **29** | 223 KB -> **80 KB** |
| 1,600 declarations, one call | 302 -> **139 ms** | 81 -> **11** | 72 -> **4** | 22 -> **3** | 181 KB -> **48 KB** |
| 3,200 declarations, one call | 546 -> **270 ms** | 137 -> **22** | 130 -> **9** | 45 -> **8** | 351 KB -> **93 KB** |

Those three phases together are 312 ms of a 3,200-declaration program's macro
tax and are now 39 -- **87% of what the change was aimed at**, which is about
what the meter predicted when it was used to reorder
[dreams/TODO.md](../../dreams/TODO.md). Whole compiles, alternating against the same
VM running the commit before's compiler: the self-compile 4.86/4.86/4.96 s
against **4.62/4.75/4.58**, the 3,200-declaration program 4.37/4.50/4.67
against **4.08/4.23/4.14** -- the new build faster in all six, which is what
puts a 5-9% result outside the placement noise floor that "Two things that will
lie to you" describes.

**The stub is parameterless and pure, whatever the declarations pointing at it
were**, and that is the part to think about before changing it. A pure 0-arity
function is a `GLOBAL_VALUE`, so *naming* one evaluates it. A stub of the
declaration's own arity would hand back a closure instead, and a call of it
with too few arguments would hand back another one -- so a walk that missed an
edge would partially apply something and carry on, where this raises at the
first mention. That is the whole reason the body is a raise, and sharing it
must not quietly weaken it.

**What makes it sound** is unchanged from the section above, because the
soundness never rested on the stubs being distinct. An unreached declaration
still raises when named, a macro is pure and cannot catch what it raises, so an
incomplete reachability answer is still loud rather than wrong; the batch is
thrown away and the round runs again with every declaration a root. What the
sharing changes is only how many function records say so.

**What did not change: the image.** Every image in this repository is
byte-identical -- `dreams`, `lucid`, `mind`, and `mind/std/all.dr --test`,
which is the macro-heavy one -- and so is the 3,200-declaration program's
(877,144 bytes, 26,109 nodes either way). That is the test this change is held
to, and the reason is that nothing *reachable* changes: the stub is only ever
in the image handed to the macro VM and thrown away. The bootstrap reaches a
fixpoint in one stage.

**What it leaves**, which is the next thing to do and is now most of what a
macro costs: `snapshot` -- `scope.declare!`, which walks every declaration for
its *names*. It did not move at all (228 -> 231 ms at 3,200 declarations, 123
-> 120 at 1,600), and with the three phases around it gone it is **85-86% of
the tax** where it was 41-42%. Note what that means about the ordering: the
phase meter said to take the stub first and it was right, but the same meter
now says the next 200 ms are in one phase that nothing here has looked at.

## Declaring a program was mostly a search for the word `core`

**The finding, and it is the whole section in one line:** `scope.declare!` is
about *names* -- it binds them, numbers them and queues the bodies for later --
and it does not read a body anywhere. Except in one place. `declare_core` asked
"does this module mention `core`?", and asked it by walking every node of every
declaration the module has (`modules.mentions_name`, a structural search for
`[:name, "core", _]`). On a 3,200-declaration program that one question was
**four fifths of what declaring the program cost**, and nothing else in
`declare!` came close.

It is asked because `std.core` is available without an import, which is a
deliberate and good thing (see `declare_core`); what was wrong is that the
answer was recomputed from scratch every time anyone wanted it, and a compile
wants it two or three times over:

- the **loader**, to decide whether to open `std.core` at all
  (`modules.needs_core`),
- `scope.resolve!`, through `declare_module`,
- and, in a program with a macro in it, the *snapshot* -- because expansion
  declares the staged program before it resolves what a transformer reaches,
  which is a second whole `declare!`.

So the answer is worked out once, when the module is loaded, and carried on the
module record (`:mentions_core`, set by `modules.loaded`). `declare_core` is
handed it rather than deriving it, and `scope.mentions_core_owned` supplies it
for *effective* items -- which is exact rather than approximate, because
`effective_items` hands over every item of every module a module derives from,
so asking each owner once is the same question as asking each item. It is
still lazy, so a module whose question is settled another way -- `std.core`
itself, a module that binds `core` -- never pays the walk at all.

| | snapshot | macro tax | `resolve` | whole compile |
|---|---|---|---|---|
| 3,200 declarations, one call | 229 -> **51 ms** | 267 -> **99** | 903 -> **757** | 3934 -> **3705** |
| 6,400 declarations, one call | 431 -> **93 ms** | 518 -> **184** | 1785 -> **1441** | 8132 -> **7315** |

Whole compiles, alternating against the same VM running the commit before's
compiler: at 3,200 declarations 4231/4152/4260 ms against **3852/3816/3818**,
at 6,400 8598/8770/8833 against **7901/7919/8047**, and the same program with
the `expand` written out 3924/3795/3861 against **3715/3642/3772** -- the new
build faster in all nine, which is what puts a 4-9% result outside the
placement noise floor. Reductions fell 13.7% (114.5M -> 98.8M at 3,200, 226.5M
-> 195.2M at 6,400) and allocation 5%.

**The self-compile does not move at all** -- 4442/4469/4370/4479 ms against
4500/4380/4395/4616 -- and the reason is the useful half of this entry. The
walk is `list.any`, so it stops at the first declaration that mentions `core`,
and every module of this compiler uses `core` in its first few lines. `dreams`
was paying about 12 microseconds a declaration for it and the generated program
69, for the same walk: **the cost of a search is how far it has to look, and a
codebase that uses the thing it is searching for is the one place the search is
free.** That is also why no profile had ever named it -- `mentions_name` was
10.2% of a compile of the generated program and invisible on the one everybody
runs.

**What makes it sound.** The stored answer is a property of the module's items,
so the only risk is an item list that changes after the module is recorded, and
there is exactly one pass that does that: expansion. `expand.with_items`, which
stages an `expand` as `1 / 0` and reads a `macro` as the `let` it is, can only
*lose* a mention or keep it, so it carries the answer over. `apply_items` and
`expand_item!`, which put a transformer's *answer* back into the module, go
through `modules.with_new_items` instead and forget it -- a macro may hand back
a tree naming `core` where the source never did, and `core` is the one name a
module gets bound without asking. That is a handful of modules re-walked rather
than the program.

`declare_items!` -- session mode -- still asks the item walk, because it
declares a *slice* of one module and the module's stored answer is about all of
it. One statement is not a walk worth saving.

Every image in the repository is byte-identical, `mind/std/all.dr --test`
excepted for a reason that is not this change -- the section below -- and the
bootstrap reaches a fixpoint in one stage.

A gate sits in front of the walk that remains, and it is worth knowing about
before reading `item_mentions_core`: `modules.text_may_mention_core` asks
whether the file's *bytes* contain `core` at all, which is one machine scan and
is exact in the direction that costs -- every `[:name, "core", _]` was lexed
from an identifier spelled `core`, so text without those four bytes cannot
produce one. It only ever skips the walk; the converse (`x.core`, a comment, a
string) is not a use, which is why both callers spell it as `&&`.

**What it leaves.** `snapshot` is 51 ms of a 3,200-declaration compile's 99 ms
tax and 93 of 184 at 6,400: still the largest phase, still linear, and now
actually what its name says -- a name declared, numbered and recorded, about 16
microseconds each. The macro tax as a whole is 2.7% of that compile where it
was 6.8%.

## The image was the calls, and now it is the transformers

**The finding, which is about what an image is for.** Three rounds of work
above got the macro image down from one per call to one per round, and each
time the thing being removed was a *rebuild*. None of them asked why the image
had to be rebuilt at all, and the answer was in `call_body`: a call was turned
into an **expression** -- the macro's name applied to its arguments, each
argument quoted into the code that reconstructs it -- and a wrapper around that
expression was compiled and run. So the arguments were *in* the image. An image
carrying the arguments can answer one round of one program and nothing else,
and that is what made "one image per round" the floor rather than "one image".

The arguments were already values. Quoting them into a program so that running
the program would rebuild them was a way of getting data across a boundary that
had no other way across -- `vm.eval_image!` runs an entry point and takes no
arguments. So the boundary grew one: `vm.open_image!` loads an image and keeps
it, `vm.call_image!` enters `module.member` in it with arguments that cross as
data, `vm.close_image!` frees it (see "Compile-time evaluation" in
[docs/builtins.md](../../docs/builtins.md)). The image is a **library** now, not a
program written for the occasion.

A wrapper is still declared and is still what the reachability walk starts from
and what the resolver answers "which macro is this?" about -- nothing here
re-implements name lookup, which is the mistake that would make it subtly wrong
in the cases nobody writes a test for. Its body is just the names.

`dreams --time` on `mind/std/all.dr --test`, which is the macro-heavy build --
55 calls in two rounds:

| | declare | resolve | lower | emit | run | tax | image |
|---|---|---|---|---|---|---|---|
| the call, compiled in | 19 ms | 41 | 59 | 32 | 8 | 202 ms | 122 KB |
| the call, passed in | **0 ms** | **31** | **35** | **10** | 13 | **133 ms** | **88 KB** |

and on a self-compile, 14 calls in one round, 194 ms -> **147**, with the image
80 KB -> **63 KB** and the same columns moving: `declare` 24 -> 15, `lower`
22 -> 13, `emit` 27 -> **4**. Whole compiles of the std build, alternating
against the same VM running the committed compiler: 2178/2268/2226 ms against
**2162/2176/2164**, the new build faster in all three -- which is what a 68 ms
saving on a 2.2 s compile should look like, and is below what this machine can
resolve on the self-compile, where the tax is 3% of the whole.

**Read the `run` column, because it is the one that went up.** A batch was one
entry and one result, so a module's calls shared a VM start; a session makes one
call, so the std build does 55 starts where it did 2, and that is 5 ms. It buys
back three times its cost in `emit` alone, and it buys something that is not
milliseconds -- see the next paragraph -- but it is linear in *calls* where
everything it replaced was linear in the *program*, so a program with thousands
of macro calls would want them batched again. The place for that is a
`call_image!` taking a list of calls rather than one: a start is a `Scheduler`,
a worker thread and a process, and the image is already loaded.

**A failing call is now the call that failed.** This is the part worth having
even at equal cost. A batch says only that *something* in it raised, so the
answer was to throw it away and run its calls one at a time, where the wrapper
*was* the call and the diagnostic was the one it gave before batching existed --
which meant compiling the program again to find out which line was wrong. A
session makes one call, so there is nothing to attribute. What survives is the
retry that was never about attribution: a call may fail because the shared
snapshot has gone stale, which only the whole program can rule out, so a failure
is asked once more with every declaration a root (`:whole`) and a failure that
repeats there is the real one and is reported where it is written.
`dreams/tests/macros.py`'s 47 cases are what hold that.

**One thing the snapshot image did not have and now needs.** `lower_program`
links funcs, globals and imports; the module table is built by `link!`, which
only the real compile calls -- so the macro image had no `MODS` section at all.
That did not matter when the entry point was patched into the header by index,
and it is fatal when a member is found by the name of its module and its own:
the image loads, runs, and has no member of any name. `MODS` is the only
statement in the container of which globals are whose, and a global's name is
not unique in one -- two modules may each declare `helper`. See
`invocation_image` in [dreams/expand.dr](../../dreams/expand.dr).

**What the images did.** `dreams` changes, because its own source did. `lucid`,
`mind` and a generated 3,200-declaration program are byte-identical, and the
bootstrap reaches a fixpoint in one stage. `mind/std/all.dr --test` moves by
**12 bytes**, and they are not the 6 the section below is about: three
`<lambda>` function records' `span_start` and `span_end`, all of them above
2^31, which is the reserved part of the offset space expansion hands generated
code out of. The wrapper is smaller, so it consumes fewer offsets, so everything
generated after it shifts by the same 3,380. Check that the same way it was
checked here -- decode the differing offsets against the section table and the
`FUNC` stride rather than arguing about them.

**Where this leaves `.libdream`.** The session is the VM half of "a transformer
library loaded once for the session and called with syntax values", which
[dreams/TODO.md](../../dreams/TODO.md) has as the pairing for separate compilation's
smallest version. The half that is left is the *roots*: this image is still
lowered from the staged whole program, so it still costs a name declared and a
global emitted per declaration, and it is still thrown away at the end of the
round. Making it a function of the dependency package rather than of the
program is what makes it cacheable, and that is where the remaining tax is --
not in the calls, which now cost nothing to make. *(The roots moved two days
later, and the image is one per compile rather than one per round -- "One image
for the whole expansion" below, which also prices what keeping it across builds
would be worth.)*

**And one thing that was found by measuring rather than looked for.**
`discover` -- "is there anything to expand?" -- is now the *largest* phase of a
self-compile's macro tax, 48 ms of 147, where `snapshot` is 25. It is **0 ms on
a 3,200-declaration generated program**, which is the opposite shape to
everything else in these notes: the lexer records where the word `expand`
appears, so a module with no sites costs nothing at all, and what is left is
per item of a module that *has* one. `dreams` writes its macro calls in
`lower.dr` and `scope.dr`, its two largest modules, so it pays for all of both
to find eighteen one-line calls. The section below is what that turned out to
be.

## Discovery was a walk of two whole modules, because a `match` lied about where it ended

**The finding, and it is not about expansion at all.** The section above ends
by naming `discover` as the largest phase of a self-compile's macro tax -- 49 ms
of 152 -- and by saying the shape of it: the lexer records where the word
`expand` appears, so a module with no offsets costs nothing, and a module that
has one costs a walk of every declaration whose span an offset falls inside.
That is cheap when the offsets can be matched to declarations. When they cannot,
`work_of` falls back to walking every node of every declaration in the module,
and on a self-compile it was falling back for the two largest modules in the
compiler.

It was falling back because of a parser bug three years older than any of this.
`parse_match` ended a `match`'s span at `peek_span rest2` -- the first token
*after* the `{`, not the `}` that `match_arms` had consumed. So

```
let rec lower_expr e l =
    match e {
```

is a declaration whose span is those two lines and stops: `[16170, 16218]` for
a body running to 23,244. A declaration's span is `let` to the end of its body
(`ast.span_to sp (ast.span body)`), so every `let f = match ..` in this
codebase covered its header and nothing else. Two of `lower.dr`'s nine `expand`
offsets were inside such a declaration, the two-way check that guards the
picking therefore failed, and all 124 of its declarations were walked. The fix
is two lines: `match_arms` hands its closing brace back, exactly as
`comma_list` beside it already did and for the reason its comment already gave.

The second half was the same question asked of the wrong thing. An offset is a
position in a *file*, and a file is several modules -- a `mod name { .. }` and
every `group`/`struct`/`mapping` are hoisted into modules of their own that keep
the source they were written in. So a record submodule is handed its parent's
offsets, accounts for none of them, and walks all of its own declarations to
find expansions it does not have. Six of a self-compile's eleven modules with
offsets were record submodules doing exactly that. The accounting is pooled per
source now, which is the question the offsets actually pose.

| | discover | macro tax |
|---|---|---|
| before | 49 ms, 2.5 M reductions | 152 ms |
| the `match` span | 17 ms, 0.8 M | 119 ms |
| + accounting per file | **14 ms, 0.6 M** | **116 ms** |

`lucid`, which imports this compiler, moves the same way: 51 ms -> **14**, and
its macro tax 167 -> **133**. `mind/std/all.dr --test` does not move at all
(3-4 ms either way), and the reason is worth keeping: in `--test` mode
`std.macros`'s `when test` blocks survive, so its 55 offsets land in
declarations and it never fell back. The whole self-compile does not move
either -- 4387/4503/4481 ms against 4482/4082/4489, interleaved -- because
36 ms is 0.8% of it and the floor on this machine is 3%.

**What is left, and why it is left.** One module still falls back:
`std.macros` compiled *without* `--test`, whose 55 offsets are all inside
`when test` blocks the configuration dropped, so they are in the file and in no
surviving declaration. Measured by forcing the accounting true and rebuilding:
3 ms of the 14, 0.2 M reductions. Telling "dropped by `when`" from "the span
logic is broken" would need the loader to carry the spans it dropped, and 3 ms
does not buy that.

**What the images did.** Every one of them changed, in one field. `mind`,
`lucid` and `mind/std/all.dr --test` come out the same size with the same node
and function counts, differing in 63, 496 and 142 bytes -- and every one of
those bytes is offset 24 of a `FUNC` record, which is `span_end`. 39 of `mind`'s
505 functions, 284 of `lucid`'s 2,630 and 97 of the std build's 2,010 had a span
that stopped at their first arm. Check it that way rather than arguing about
it: decode the differing offsets against the section table and the record
stride. The bootstrap reaches a fixpoint in one stage, and the seed had to move
with it because a compiler that spans a `match` correctly compiles *itself*
with different spans.

**And the reason no profile had named it.** `--profile` counts reductions and
this was 2.5 M of 58 M, spread across `has_expansion` -- under 5%, in a function
that is *supposed* to walk syntax. What said it was wrong was not a profile but
printing what the pass decided: eleven modules with offsets, of which eight
could not account for their own. A pass with a fallback should be asked how
often it takes it, and `work_of` had been taking it for a year.

**One thing this deletes elsewhere.** `lucid/analysis.dr` had a `reach` walk --
follow the last part of a lambda, an `if`, an application, a `let`, a `match`'s
arms, and take the largest end -- built because "a cursor in a match arm looks
as though it is in no `match` at all", at a cost its comment records as eight
failing tests. That was this bug seen from the editor's end. The walk is gone
and `holds` asks the span, with the tests that found it kept exactly where they
were: they now hold the parser instead.

## One image for the whole expansion, and what it cost to buy that

**What it does.** The section above got the image down to one per *round* and
ended by naming what was left: the roots. The image was resolved from the
round's **wrappers**, so what was in it was whichever transformers this round
happened to call -- a function of the program, and rebuilt for the next round.
It is resolved from the **transformers** now (`expand.transformer_roots`), so
every round of a program shares one image and there is one per compile rather
than one per round.

Two things had to move for that. `scope.resolve_reachable!` took an *index* --
"the bodies queued after this point", which is how "the wrappers I just
declared" was spelled -- and takes a set of `root_key`s, a source index and an
offset, because a root is now named by where it was written. And the wrapper
itself had to get out of the image: it was declared as a global and resolved
with the transformers, which is a few nodes but is what made the image this
round's. `scope.resolve_names!` resolves it on its own and the caller keeps
`refs` and `r_diags` and throws the rest away, so nothing is declared and
nothing is lowered for it. It is still the thing the resolver answers "which
macro is this?" about, which is the one question expansion must not answer for
itself.

`dreams --time` on `mind/std/all.dr --test`, which is the macro-heavy build --
55 calls in two rounds -- and on a self-compile, which is 14 calls in one:

| | sites | resolve | lower | emit | tax | image |
|---|---|---|---|---|---|---|
| `std --test`, per round | -- | 32 ms | 36 | 11 | 134 ms | 2 of 88 KB |
| `std --test`, per program | 7 ms | **8** | **30** | **5** | **107 ms** | **1 of 51 KB** |
| self-compile, per round | -- | 19 ms | 12 | 4 | 110 ms | 1 of 63 KB |
| self-compile, per program | 19 ms | **7** | 28 | 7 | **118 ms** | 1 of 79 KB |

**Read the second pair, because it is the one that went the wrong way.** A
program with one round has no second round to amortize anything over, so all it
sees is the new cost: the image now holds every transformer in the program
whether or not this compile calls it, and lowering one is not free. The
self-compile is **8 ms worse** and its image is 16 KB bigger.

**What a root costs, measured on the axis that exists for it.**
`scale.py --macros N` writes a dependency declaring N transformers of which one
is called, which is the shape a package of macros has. Against the same VM,
the compiler before this change and the compiler after:

| transformers declared | 1 | 16 | 32 | 64 | 128 | 256 |
|---|---|---|---|---|---|---|
| roots are the calls | 23 ms | 21 | -- | 17 | 22 | 25 |
| roots are the transformers | 19 ms | 28 | 34 | 67 | 100 | **159** |

Flat against linear, at about **0.5 ms per trivial transformer**. That is not a
per-root overhead and it was worth checking rather than assuming: a syntax
transformer's body is a syntax *literal*, which is fifty-odd nodes, and the
same run lowers the whole program's 16,919 nodes in 238 ms -- 14 microseconds a
node, against the 12 these come out at. A root costs exactly its own body, and
what the change did was stop declining to lower it.

**So this is a down-payment, and it is written down as one.** What it buys
today is the std build's 30 ms, which is real but is only the second round not
paying for the first. What it is *for* is that an image which is a function of
the program's transformers is the same image on every build, and so is the
thing a build could keep -- where an image that is a function of the round can
only ever be built again. Until it is kept between builds, a one-round program
pays 8 ms and gets nothing back.

**What keeping it would be worth, so the next person does not have to guess.**
The phases a cache removes are `resolve` + `lower` + `emit`: **42 ms of a
4,286 ms self-compile (1%)**, 43 ms of 2,200 on the std build (2%), and 145 ms
of 1,129 on a 256-transformer program (**13%**). That is the whole case for
caching, and it says what kind of project it is for -- not this one. The rest of
a self-compile's 118 ms tax is `discover` 14, `stage` 15, `snapshot` 21 and
`sites` 19, none of which a cached image touches, because they are the *calling*
program being staged, declared and asked what its own call sites mean.

**One row is new and one moved into it.** `sites` is `resolve_names!`, the
wrapper's own resolution, and it is 19 ms on a self-compile against 0 on a
generated program. Most of that is not the fourteen names: it is forcing the
environments of the four modules they are written in, which `resolve` used to be
charged for because the wrappers were resolved with everything else. The pair is
26 ms where it was 38.

**What did not change.** The images: `dreams`, `lucid`, `mind` and
`mind/std/all.dr --test` are all four byte-identical to what the previous
compiler emitted -- including the std build, which the section above had to
excuse for twelve generated-span bytes and this one does not -- and the
bootstrap reaches a fixpoint in one stage. And the diagnostics, which is the
half worth checking deliberately, because resolving every transformer rather
than the called one moves where a broken macro is first noticed. Three shapes,
before and after, character for character the same: a macro that is never called
and names something that does not exist (reported once, by the real compile, at
the name), one that *is* called and names it (reported twice, as it always was
-- once against the `expand` and once at the name), and one that raises when it
runs.

## `mind/std/all.dr --test` is not byte-stable across compiler changes

Worth knowing before the next person spends an hour on it, because several
sections above tell you to check byte equality after a compiler change and this
is the one image that can move without anything being wrong.

`mind/std/all.dr` contains `let compiled_options = comp cli.parse_as
option_schema options_for_types [..]`, and the value of that is a *map* --
`%{ :values => .., :rest => .. }`, from `cli.parsed`. A compile-time expression
is evaluated by running it on a VM and quoting the answer back into the image,
and a map is quoted in the order its entries come out, which is by runtime atom
identity. **Runtime atom ids are per process, and the process is the compiler.**
`:values` is an atom the compiler itself has (number 96 of its own 345);
`:rest` is not, so in the VM that runs the `comp` it is interned fresh, past
the compiler's own atoms -- and it moves when the compiler does.

So the two entries can swap, and the image differs by **6 bytes of 374,456**,
all of them inside that one `comp`. Measured, so that it is not guessed at
again:

- it is stable across runs, across `-j`, across `DREAM_GC_THREADS`,
  `DREAM_GC_CONCURRENT` and `DREAM_MAX_HEAP` -- this is not a race;
- it is decided by the compiler image. A one-line `let x = :zzz;` added to
  `dreams/config.dr` and nothing else flips it, and so does a one-line
  `let x = "zzz";`, and so did the change in the section above -- **and all
  three produce the same image as each other**, which is the check to apply.

So when byte equality is the test, `dreams`, `lucid`, `mind` and a generated
program are the ones that answer it, and a 6-byte difference in the `std --test`
image at offset 289,565 is this and not a behaviour change. Confirm it the way
it was confirmed here: diff against a build with an unrelated one-line addition
rather than against the seed.
