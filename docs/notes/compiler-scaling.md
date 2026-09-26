# Compiling large programs

The quadratics, and the memory held by unforced values, that decided how large a program `dreams` can compile. Moved out of `CLAUDE.md` on 2026-09-25; the prose is as it was, so a date or number in it is as of when it was written.

## The compiler was quadratic in the size of the program

**How it was found**, because the method is the transferable part. Everything in
*Making it faster* above was found by asking "what is expensive?" of one
program. That question cannot distinguish a large linear cost from a small
quadratic one, and a quadratic is the only kind that decides whether a big
project compiles at all. The question that can is **"what grows faster than the
input?"** -- run the same profile at two sizes and divide:

```
dream --stats --profile 12 build/dreams.dream -L mind -L proj -o /tmp/o.dream proj/main.dr
```

on a generated program of N and of 2N declarations. Anything near 2.0x is
linear and can be ignored however large it is; anything near 4.0x is the bug,
however small it is today. Four of them turned up in one sitting, and three had
been there for the whole life of the compiler.

| declarations | before | | after | |
|---|---|---|---|---|
| 400 | 1048 ms | 84 MB | **971 ms** | **53 MB** |
| 1,600 | 4671 ms | 600 MB | **2685 ms** | **247 MB** |
| 3,200 | 13144 ms | 1927 MB | **4775 ms** | **647 MB** |
| 6,400 | 40158 ms | 6680 MB | **9848 ms** | **1660 MB** |

(Peak live heap, `--stats`. Every image is byte-identical before and after, at
every size.) Time was 2.8x-3.1x per doubling and is now 1.8x-2.1x: **linear**.
Memory is 2.5x-2.6x per doubling, so it is better by 4x and still not linear --
what is left is under "What is still superlinear" below.

The self-compile moves from 5.1 s to 4.8 s and that is the whole of what this
is worth on *this* repository, which is the point worth taking away: `dreams`
has fifty modules of a hundred-odd declarations each, and n^2 on a hundred is
nothing. **A quadratic that this codebase cannot feel is still the reason a
large project would not build.** Generate the big program; do not wait for
someone to write one.

**1. A list searched more often than it is built wants to be a map** -- for the
fourth time in these notes, and the previous three are above. `collect_let`
gathers a module's `let`s and asks, per declaration, "have I got this name
already?" It asked a list, with a `list.map` that built an intermediate of
everything collected so far and a `list.append` that copied it. Gathering one
module was O(M^2) in the module's size. It is a `mapping Gather` now -- entries
by position, a slot per name, and the positions are what keep it ordered while
the map answers the question. A name redeclared by a module that derives this
one still replaces the earlier one *where it stood*, which is what the image's
global numbering rests on, and there are four cases in `scope.dr`'s `when test`
holding exactly that.

**2. `list.append xs [x]` is one reduction and a copy of `xs`.** This is the
big one, and it is the reason the first question is the wrong one. Appending is
an opcode -- `xs + ys`, a single machine walk, which is why "a linear walk the
machine can do is worth ten of the same walk in Dream" above recommends it --
so a `--profile` charges one reduction for copying a list of any length. An
accumulator built this way is therefore **invisible in a reduction profile and
quadratic in bytes**. `record_global`, `add_pending` and `check_body`'s queue of
bodies were 282 MB, 281 MB and 295 MB of a 3,200-declaration compile against
80 MB, 80 MB and 87 MB at half the size; the three `link_*` folds in
[dreams/lower.dr](../../dreams/lower.dr) were another 39% of everything allocated.
All are `core.cons` onto a reversed list now, with one `list.reverse` in the
accessor. Where a caller only wanted the length, it asks for a count and the
ordered list is never built (`scope.pending_count`, `bodies_count`,
`s_global_recs_count`).

**3. Deforestation's invented globals were the same mistake**, and the workload
that exposed it is the one to keep in mind: a program whose every declaration
folds over a range invents a fused loop per declaration, so `take_fused_global`
appended to a list as long as the program, once per declaration.

**4. Declaring a program was cubic in its module count.** The other axis, and
the one nothing here had ever varied: every measurement above changes the number
of *declarations*. `set_env` replaced one module's environment by rebuilding the
whole list with a `list.nth` per element -- O(modules^2) reductions -- and it is
called several times per module. It is `(envs s).[mi => e]` now, which is the
`set` opcode and one machine walk. On 3,200 declarations spread over 400 modules
that is 6744 ms -> **4880 ms**, and the macro tax alone 1505 ms -> **718 ms**.

**Generating the program is now one command**, because the advice above is
useless without it:

```
dreams/tests/scale.py --sizes 800 1600 3200               # declarations
dreams/tests/scale.py --sizes 3200 --modules 400          # modules
```

It prints the ratio between consecutive sizes for time and for peak heap, which
is the number to read. It is not in `just test`: it takes minutes.

**What is still superlinear**, measured and left. Peak heap is ~2.1x per
doubling of the *declarations* and time ~1.9x, so both are close to linear but
neither is quite there, and the compiler is lazy end to end, so every stage's
intermediate is retained by the stage that reads it: source, tokens, syntax,
effective items, the staged copy, the resolver's tables and the arena are all
live at once. That one is not a bug and cannot be tuned away -- it is the
argument for compiling a package at a time, which is what
[dreams/TODO.md](../../dreams/TODO.md) calls for.

*(Those last two sentences were written without being able to see what the live
set was made of, and they are wrong in an instructive way. A third of that peak
was not any stage's intermediate -- it was versions of the arena retained by
values nobody had forced, and it did tune away. "A lazy value stored in a map
pins the map it was made in" is what happened once the question became
askable.)* The *modules* axis is the next
section, and it was the one nobody had varied far enough.

## A table indexed by module wants to be a map, not a list

**The same finding as all four above**, on the axis they did not vary: this
section's benchmark changes the number of declarations, and `scale.py
--modules` changes how many modules they are spread over. Varied to 3,200
modules, peak heap was 3.4x per doubling -- which is what the declarations axis
looked like before any of the work above.

`envs` is the resolver's table of what each module binds, keyed by module
index, and it was a list. Two things followed, and the second is the one that
mattered:

- **Reading it was O(modules).** `env_at s mi`, `ctx_env` and `env_of` were
  each a `list.nth`, and one of those runs per name resolved. This is the half
  everyone notices and it is the half that is nearly free: `xs.[n else ()]` is
  the `get` opcode, a machine walk of one pointer chase per element, so 3,200
  declarations over 800 modules is about 2.5M chases and no measurable time.
- **Writing it allocated O(modules).** `set_env` was `(envs s).[mi => e]`, the
  `set` opcode -- one reduction, so a `--profile` said nothing -- and
  `list_set` builds a fresh cell for every element in front of the one it
  replaces. There is one `set_env` per declaration, so a compile allocated
  O(declarations x modules) cons cells for nothing. That is exactly the shape
  "`list.append xs [x]` is one reduction and a copy of `xs`" describes, and it
  had been hiding behind the same silence.

It is a map keyed by module index now. A put rebuilds the path to one leaf and
shares the rest, a read is a lookup, and nothing wanted the order -- every
reader was already indexing it, which is the whole argument. `has_env` replaced
the `mi < list.length (envs s)` guards, which were asking "has this module been
declared yet?" in the only spelling a list had.

Peak live heap, 3,200 declarations spread over N modules, `--max-heap` 8 GB so
that both finish:

| modules | before | after | |
|---|---|---|---|
| 200 | 354 MB | 359 MB | the map costs a little more when it cannot pay off |
| 400 | 384 MB | 360 MB | |
| 800 | 439 MB | 373 MB | |
| 1,600 | 575 MB | 360 MB | |
| 3,200 | **1967 MB** | **594 MB** | -70% |

**What that is worth is not the megabytes, it is the wall.** Under the *default*
1 GB `DREAM_MAX_HEAP`, the 3,200-module program did not compile at all -- it
died with `process heap grew past 1073741824 bytes`. It compiles now. The old
compiler's wall was between 2,400 and 3,200 modules; nothing here moved the
declarations axis, whose wall is still the 4,000-5,000 this section names.

Time is the small half and says so: 8371 ms -> 7322 ms at 3,200 modules, 6114
-> 5665 at 1,600, and nothing outside the noise floor below that. The
self-compile does not move at all -- 4890/4948/4867 ms against 4678/4882/4698,
9 majors either way with the same live series under `DREAM_GC_TRACE=1` -- which
is the expected answer for a repository of fifty modules, and is once again a
quadratic this codebase is too small to feel. Every image is byte-identical --
`dreams`, `lucid`, `mind` and the generated programs at every size -- and the
bootstrap reaches a fixpoint in one stage.

**What was next on this axis, measured.** `--stats` reports allocation by kind,
and the list share climbed where nothing else did: 9%, 10%, 15%, **24%** at 400,
800, 1,600 and 3,200 modules -- 212 MB, 250 MB, 406 MB, 972 MB, which is 2.4x
per doubling at the end. The named candidate was `modules.modules`, the same
table in the loader rather than in the resolver. It was one of three, and the
next section is what happened when all three were taken.

## The three lists the loader still grew one entry at a time

**What it does.** `modules.modules` was the obvious one and the section above
names it, so it was taken first, on its own, and measured on its own: **5.7%
of a 3,200-module compile's allocation, three points of the list share, and
nothing at all of the peak heap or the wall clock.** That is the whole of what
the predicted fix was worth, and the prediction had been that it was most of
the 24%.

The other 18% was two more tables of exactly the same shape, both found by
asking where the bytes actually went rather than by reading the earlier note
again:

- **`modules.files`**, the source text of every file, appended to per file --
  so loading a program copied the list once per module. It is also the table
  `expand.install!` rewrites one entry of, with the `set` opcode, once per
  macro call, which is the same `list_set` per expansion the `envs` note
  describes. Keyed by index now, with `file_at` for the readers that hold a
  diagnostic's source index (all of them but one) and a derived ordered list
  for `diag.render_all`, which is handed the whole program once at the end of
  a compile that has something to say.
- **`scope.module_recs`**, one record per module, appended to in
  `declare_module`. It is the fourth field of that state to become a reversed
  accumulator, beside the imports, the global records and the queue of bodies
  -- the note at the head of `scope.dr` already explained why, for three.

Two smaller things went with them. `expand.work_of` runs on **every** compile,
macros or not -- it is how "is there anything to expand?" is answered -- and it
walked each module's declarations with a `list.nth i items` inside a fold over
`0..length`, which is a walk per declaration. It carries the index alongside
the accumulator now. And `expand.replace_module`, the `list_set` the section
above names, is a map put.

Peak live heap and total allocation, 3,200 declarations spread over N modules,
`--max-heap` 8 GB:

| modules | allocated | | peak live | | list share | |
|---|---|---|---|---|---|---|
| 400 | 2.36 GB | 2.35 GB | 372 MB | 364 MB | 9% | 8% |
| 800 | 2.50 GB | 2.46 GB | 390 MB | 385 MB | 10% | 9% |
| 1,600 | 2.90 GB | **2.73 GB** | 387 MB | 393 MB | 14% | 10% |
| 3,200 | 4.05 GB | **3.34 GB** | 623 MB | **409 MB** | 24% | 13% |

**Peak heap on the modules axis is now flat** -- 364, 385, 393, 409 MB across
an eightfold spread of modules, against 372, 390, 387, **623**. The bytes
promoted at 3,200 fell with it, 1.12 GB to 758 MB, and that is the mechanism:
an appended spine survives the minor collection that catches it, so a table
grown one entry at a time is not merely churn, it is churn the nursery hands
to the old generation. Time is the small half again and says so: at 3,200
modules 7172 ms -> 6816 ms with a macro and 6256 -> 5648 without, and nothing
outside the noise floor below 1,600.

The **declarations** axis is unmoved in memory (646 MB against 655 at 3,200)
and a little faster (4831 ms against 5359 with a macro, 4096 against 4437
without). That is the expected answer and it is the one worth remembering: the
declarations axis's peak heap is the lazy pipeline holding every stage's
intermediate at once, which is the argument for compiling a package at a time
and is not a quadratic anybody can delete. The **self-compile does not move**
-- 4969/4922/4916 ms against 4912/4924/4861, alternating on the same machine
-- because fifty modules is not a number any of this can be felt at. Every
image is byte-identical, `mind` included, and the bootstrap reaches a fixpoint
in one stage.

**One trap, which cost the only debugging in this round.** The loader's record
still said `:files => []` after `add_file` had been rewritten to put into a
map, and `[] .[0 => f]` is a list set of an empty list -- but the field is
built lazily, so nothing raised until something *read* the table. Ordinary
compiles never do: a program with no diagnostics never looks at its own source
text. Only macro expansion reads it, in `install!`, so the whole test suite's
non-macro half passed and every macro program died with
`index 0 is past the end of a list of 0` and no span. **When a field changes
shape, the initializer is the thing to check first, and a lazy field will not
tell you that you missed it where you missed it.**

## Sharing the arena

**What it does.** `lower` emits a node wherever the source says one and never
asks whether it has emitted that node before. It has, constantly: `local 3` is
one 16-byte record and a self-compile writes ten thousand of them, `core.head`
as a callee is a `builtin` beside an `apply` and there are seven thousand of
those, and a condition written the same way in two modules is two identical
subtrees. [dreams/opt.dr](../../dreams/opt.dr) rebuilds the finished arena once,
bottom up, into a fresh one that keeps a table of every record it has already
written. On the three programs in this repository:

| | nodes | image | |
|---|---|---|---|
| `dreams` | 67,306 -> **19,323** | 1.29 MB -> **474 KB** | -63% |
| `lucid` | 52,157 -> **15,317** | 1.00 MB -> **373 KB** | -63% |
| `mind` | 10,715 -> **4,129** | 219 KB -> **107 KB** | -51% |

The sharing compounds because the children are rebuilt first: two subtrees that
mean the same thing arrive at the table with the same operands and so are the
same record. Flat, only 47% of the arena is duplicate records; bottom up it is
71%. The same walk answers two more questions for free, which is why they are
not passes of their own -- it starts from the function bodies, so a node nothing
reaches is never copied (wrapper lowering leaves these behind by the hundred),
and once the nodes are renumbered two runs of children holding the same indices
are one run, which is half the kids pool.

**What makes it sound** is that a node is a function of the frame it is
evaluated against and of nothing else. Sharing one between two functions makes
them share nothing at run time: each forces it against its own frame, and
`local 3` means "slot 3 of whoever is asking". Memoization belongs to the thunk,
which is a pair of a node and a frame, so two sites that share a node still get
a thunk each. Three things follow, and all three are in `opt.dr`'s `when test`
block so they stay found:

- **A flag is part of the record.** `strict`, `impure` and `tail` are the
  compiler's answer to "when does this run?", so two nodes differing only in one
  are two nodes. This is also why the pass cannot live in `ir.push_node`, which
  would be strictly cheaper -- there is no second walk and no memo. Flags are
  set *after* emission (`ir.set_node_flag`: tail position and impurity are
  discovered later), and a node shared at push time would hand a flag it
  acquired later to everyone sharing it. The pass runs on the *finished* arena
  for exactly this reason, and after `settle_comps!` besides, because a
  compile-time expression overwrites its placeholder.
- **An operand is not always an edge.** `a` is another node in `force`, a frame
  slot in `bind`, a function index in `closure`, a constant index in `int` and a
  Unicode scalar in `char`. The two tables at the top of `opt.dr` are written
  out per opcode rather than inferred, because getting one wrong is silent in
  the worst way: the image still loads, the section table still adds up, and
  some program reads a slot number as a node index.
- **Captures share the kids pool.** `place_captures` puts a function's capture
  descriptors in the same pool as call arguments, so a pass that rebuilds that
  pool from the nodes alone drops every one of them. The VM says `capture list
  extends past the kids pool` -- but only when the run happens to fall off the
  end, which on the first attempt here it did on the fourth program tried and
  not on the first three.

**What it costs, and what it does not buy.** The pass is 342 ms of a 2.7 s
self-compile: **+11% to compile, for -63% of the image**. It makes nothing
faster. Both halves of that were measured rather than assumed, and both are
worth knowing before anyone tries to claim the pass back as a speedup:

- **The six benchmark workloads do not move** (40/48/102/29/76/22 ms against
  40/48/100/29/76/21 before), which is the expected answer: they are tight
  loops, and a loop's nodes are in cache whatever the rest of the image weighs.
- **Neither does load time, and neither does the compiler running on itself.**
  A 474 KB image and a 1.29 MB image of the *same compiler* compiling the same
  program came back 2649 ms and 2599 ms -- the smaller one nominally slower,
  which is how a difference inside the placement noise floor looks. The reason
  is that the VM maps an image and reads it where it lies, so it never pays for
  bytes it does not touch. A smaller image is a smaller image.

**Measured, and not kept.** Constant folding, which is the obvious thing to add
to a pass that is already walking every node. There are 299 `if`s with a
constant condition in `dreams` and the branches they would delete come to 897
nodes -- 1.3% of the arena against the 71% above -- and taking them means
deciding what happens to the `if`'s own flags when its child takes its place,
which is the delicate part of the whole design. The constant pools were also
checked and are already fully interned: 108 of 108 ints, 280 of 280 atoms, and
42 unreferenced strings out of 1,871.

**Two things that were tuned, one of which mattered.** The memo -- old node
index to new -- is what stops a shared subtree being copied once per path to it,
and it is half as big as it looks like it should be, because **leaves are
deliberately not memoized**. The arena is very nearly a tree: walking `dreams`
with no memo at all visits 77,920 nodes against 66,336 distinct, so the whole
table buys 17% fewer visits and charges a lookup and an insert on a table the
size of the program for each one. Half those entries were leaves, and a leaf is
the one node a re-walk cannot make expensive -- it has no children, so visiting
it twice is twice one visit rather than twice a subtree. Dropping them took the
pass from 462 ms to 342. The memo is kept for everything else, because a deep
enough DAG without one is exponential and 1.17x on one program is not a promise.
What did *not* matter, measured the same way: settling the tables at every write
rather than once per node, and the `zip`/`range` pair that numbered a list the
walk already had in order -- together 19 ms of 481.

**When changing it**, the bootstrap is the test, as it is for fusion: a compiler
that shares nodes compiles *itself* smaller, so the seed moves and has to reach
a fixpoint again. Note the shape of that -- the old seed does not have the pass,
so it builds an *unoptimized* image of a compiler that does have it, and the
stage after that is the first optimized one. Run `just bootstrap` twice and keep
the second image; `bootstrap-check` then passes because every compiler built
from this source emits the same thing. `--no-opt` emits the arena as lowered,
which is what to reach for first when an image misbehaves.

## A lazy value stored in a map pins the map it was made in

**The tool first, because the finding was not reachable without it.** `--stats`
said how much a program *allocated*, by kind, and how much was *live* at its
peak -- but never what the live bytes were. For a lazy language the allocation
answer is always the same (frames and thunks: making a call is most of what any
program does), and the live answer is the one that decides whether a large
program compiles at all. So the major sweep, which already walks every live
object, tallies them:

```
; 222356000 live at the largest major, by kind: map 57% (745534 at 169 B)
  list 15% (1412025 at 24 B) frame 11% (219134 at 114 B) map entry 10% (552026 at 40 B) ...
```

A major, because only a major proves anything -- a minor never looks at old
space, so its survivors include whatever old space is carrying. The count and
the average size are there because "map 57%" reads very differently as a
thousand fat branches and as a million thin ones, and in this case the
difference *was* the finding. `DREAM_GC_TRACE=1` prints the top three kinds on
every major's line, which is how the shape over a whole compile gets read.

**What it said.** A self-compile's live set is **57% map branches and 10% map
entries** -- 1.35 branches per entry, averaging 169 bytes. Everything the notes
here had assumed about that peak was wrong: not the source, not the tokens,
not the syntax, not the arena's own size. And `--check`, which parses and
resolves and stops, holds **map 0%**, so all of it arrives in `lower`.

The number to compare against is what a map that size ought to weigh. Two
hundred thousand integer keys, built and held:

```
; 10797072 live at the largest major, by kind: map entry 71% (191101 at 40 B) map 29% (52327 at 60 B)
```

0.27 branches per entry at 60 bytes. The compiler's trie is **five times as
many branches, each nearly three times as fat**, for the same number of
entries -- fourteen times the bytes.

**Why, and it is a general fact about lazy code rather than anything to do with
compilers.** A map is persistent: a put shares the entries and copies the path
to the one it added. So *one* map is cheap and *every version of one* is not,
and what decides which you are holding is whether anything still points at the
old versions. A lazy value is exactly such a pointer. `m.[i => f x]` stores a
thunk, a thunk carries the frame that would compute it, and in a fold that
threads the map that frame holds **the map as it stood one step ago**. Store N
lazy values and the map holds N versions of itself: N frames, N thunks, and a
trie path-copied per entry instead of shared.

Measured on its own, the same hundred thousand entries three ways:

| | live | branches | frames |
|---|---|---|---|
| values stored unforced | 68.5 MB | 280,368 at 213 B | 77,188 |
| the spine forced, fields not | 46.2 MB | 200,502 at 169 B | 90,130 |
| values in normal form at the put | **10.4 MB** | **20,847 at 46 B** | **4** |

Three things in that table are worth keeping. It is **6.6x**, which is not a
constant factor anybody tunes away. Forcing the *spine* is less than half the
answer, because the fields are where the frames hang -- and that is the version
someone would write first. And `list.fold_strict` in place of `list.fold` makes
it **worse** (97 MB), which is the check that this is not the chain-of-
suspended-accumulators trap that `fold_strict` exists for: the fold was never
the problem, the stored value was.

**The fix, in the compiler.** `ir.push_node` stored the node it was handed, and
`ir.push_kids` the child indices, both unforced -- so the arena was a chain of
every version of itself, held together by nodes nobody had looked at yet.
`ir.settled` forces a node's five fields, and the shape is `list.fold_strict`'s
and for the reason given there: an argument is not evaluated, so
`push_node (settled n) p` suspends the call to `settled` and changes nothing.
A condition is the one place the language must evaluate something, `type_of` is
the cheapest builtin that cannot answer without evaluating, and `:nothing` is
not one of its answers -- so every clause is true, `&&` reaches the last, and
the caller's `else` is unreachable and says so.

| | peak live | largest major | live frames | thunks |
|---|---|---|---|---|
| self-compile | 409 MB -> **225-297 MB** | 222 -> **95-174 MB** | 219,134 -> **76,028** | 366,700 -> **122,736** |
| 1,600 declarations | 217 MB -> **172 MB** | | | |
| 6,400 declarations | 926 MB -> **755 MB** | | | |

The self-compile's range is not sloppiness and is worth understanding before
reading any peak figure here. Three interleaved runs of the old compiler came
back 409.9, 409.1 and 408.6 MB and of the new one 297.0, 297.0 and 225.1 -- the
*old* one is pinned to a tenth of a percent and the new one varies by a third,
because a peak is only ever sampled where a collection happens and
`gc_threshold_` is refitted to `live * 3` after every major. Allocate less and
every crossing in that geometric series moves, so which major lands on the
high-water mark becomes a coin toss. "Two things that will lie to you about a
change to the interpreter" says the same thing about the clock. The floor of
the new range is the honest reading of what is held; the ceiling is where a
collection happened to look.

**And the wall moved, which is what this is for.** Under the default 1 GB
`DREAM_MAX_HEAP`, a generated program of **8,000 declarations did not compile
and now does**, and so does one of 9,600; 12,800 still does not, either way.
That is a 50% larger program on the axis [dreams/TODO.md](../../dreams/TODO.md) names
as the one a large project actually hits.

**What it costs: nothing measurable.** Interleaved on the 1,600-declaration
program, 1852/1800/1778 ms before against 1870/1756/1862 after -- a wash, and
well inside the 3% floor that "Two things that will lie to you" describes.
Allocation falls 3% and promotion 14%, which is the mechanism showing up from
the other side: a version chain is not merely churn, it is churn the nursery
hands to the old generation.

**What did not change.** `mind` and `mind/std/all.dr --test` are byte-identical,
and so is the 6,400-declaration program's image. `dreams` and `lucid` differ,
for a reason that is not a behaviour change and is worth knowing before anyone
checks: both *contain* the compiler, so editing `ir.dr` edits their source --
26 nodes, which is the `if` this adds. The bootstrap reaches a fixpoint in one
stage and the seed moved with it.

**What is left, and the number to watch.** Still 1.5 branches per entry against
the 0.35 a settled map has, so roughly a hundred megabytes of a self-compile's
peak is *still* versions nobody can reach but a value nobody has forced. The
tool says so in one number now, which is the useful part: divide the map
branch count by the entry count, and anything much above a third means this.
[dreams/opt.dr](../../dreams/opt.dr) is the obvious next place -- its five tables are
the same shape as the arena's, and `keep` only forces a node when it happens to
be shareable, because `to_string` is how it makes the sharing key.

*(It was not `opt.dr`, and it was not a table of the compiler's at all -- it was
the arena. The section below is what the hundred megabytes turned out to be,
and how it was found, since guessing from the ratio got the place wrong.)*

## The arena was a chain of its own versions, and `push_node` never saw it

**The finding.** `ir.push_node` has always refused an unforced node --
`settled` is the whole of the section above, applied at the one place a node is
appended. There are two other writers. `ir.set_node_flag` reads a node back out
of the arena, puts a flag on it and writes it in again, and `with_flags` is a
`set` of a list, so what went in was a *suspended* one. Lowering sets a flag on
most of the nodes it emits -- tail position, strictness, impurity -- so most of
the arena was stored as a thunk, each holding the frame of whichever lowering
function asked for the flag, and each of those frames holding the lowering
state, which holds the arena as it stood at that node.

So the arena was a chain of tens of thousands of its own versions, held
together by nodes nobody had looked at yet, exactly as the section above
describes for a map built in a fold -- and past a guard written to prevent it.
It was **45% of a self-compile's live set**.

| | peak live | largest major | map branches / entries | promoted |
|---|---|---|---|---|
| before | 297 MB | 174 MB | 632,614 / 405,846 = **1.56** | 441 MB |
| `set_node_flag` settled | 202 MB | 95 MB | 227,416 / 328,345 = 0.69 | 396 MB |
| + `lower.set_func_body` | 235 MB | 142 MB | | 356 MB |
| + `scope`'s two tables | **204 MB** | 108 MB | 335,360 / 384,170 = **0.87** | **328 MB** |

Read `promoted` down that column rather than `peak live`: a version chain is
churn the nursery hands to the old generation, so promotion is what falls
monotonically, where a peak is only ever sampled where a collection happens and
moves by a third between runs of the *same* binary for the reason "Two things
that will lie to you" gives. `mind` and `mind/std/all.dr --test` are
byte-identical to what the previous compiler emitted, and the bootstrap reaches
a fixpoint in one stage. `lucid` differs in **3 bytes**, all of them `span_start`
and `span_end` of one `FUNC` record, because `lucid` imports `dreams` and so
the prose added to `scope.dr` is part of *its* source too -- the check that
matters is the one this repository has always used: decode the differing
offsets against the section table and the record stride rather than arguing
about them.

**And the wall moved by 40%, which is what this is for.** Under the default
1 GB `DREAM_MAX_HEAP`, on `scale.py`'s declarations axis: before, 8,000
declarations compiled and 9,600 did not; now **11,200 compiles** and 12,800 does
not. (The section above records 9,600 compiling, and it did when that was
written -- `dreams` has grown since, and the wall moves with it. Measure the
commit before in a worktree, as that section and "Where the self-compile now
stands" both say, rather than against a number in this file.) Each of the three changes is load-bearing for that -- `set_node_flag`
alone reaches 9,600 and fails at 11,200, and `set_func_body` is what takes it
the rest of the way. The self-compile is 1-2% *faster*, mean of three
interleaved rounds, which is inside the noise floor and is the answer that
matters: none of this costs anything.

The other two are the same rule at the two other places that break it. A
function's body index (`lower.set_func_body`) is one integer per function, but
an unforced one holds a whole arena apiece. `scope.record` and
`record_bind` are the resolver's real output, and a resolution is a small tree
(`[:member, [:dream, mi], field, gi]`) rather than a fixed list of fields, so
`scope.forced` is a walk where `ir.settled` is five `type_of`s -- and forcing
the spine is a second, separate job (`settle_refs`), because the two chains have
different causes: values hold versions, an unforced put holds the table before
it.

**How it was found, because the ratio pointed at the wrong place.** The branch
count over the entry count says *that* something is a version chain and says
nothing about which map. Three temporary probes in `dream/src/heap.cpp`, each
answering the next question, and none of them kept:

1. A histogram of live `MapObj::count` by log2. A healthy trie of N entries has
   exactly one node at the top bucket; this had 5,549, and a continuous spread
   of every intermediate size from 1K to 88K -- which is what a map that grew
   one entry at a time and kept every version looks like.
2. The type of the object pointing at each big map. All of them were map
   entries, so the big maps were *values* -- `:nodes` inside the program
   record, which is `:prog` inside the lowering state.
3. A walk from the roots keeping a parent for every object, attributing each
   live byte to the nearest frame above it, reported as function indices. That
   named `lower_arms`, `ir.set_node_flag` and `lower_expr`, and
   `dreams/tests/` has no tool for turning a function index into a name, so the
   image was decoded by hand against `FUNC`, `GLBL` and `MODS`.

The third of those is the one worth rebuilding if this comes up again: "which
Dream function is holding the heap" is the question, and nothing in the tree
answers it. The first two only narrow it.

**Measured, and not kept: `opt.dr`'s tables**, which is what the paragraph above
this section predicted. `keep` stores a node in `:out` unforced when it is not
shareable, `rebuild` stores an index in `:memo` and `keep_run` one in `:runs`,
so all three break the rule. Settling them is six lines and it buys **1% of
promotion on a self-compile and nothing at all on a generated program** -- 459
and 463 MB against 463 and 459, the two builds swapping places. The reason is
that `opt`'s one big table is `:out`, and `to_string` forces every node that
goes into it because that is how the sharing key is made; the memo and the run
table hold integers, and an integer version chain of 30,000 entries is not
where a hundred megabytes is. Worth knowing before reaching for it: with the
arena settled, `opt` costs **no peak heap at all** -- 202 MB with it and 204 MB
with `--no-opt` -- where before this it cost 61 MB, all of which was the pass
forcing the suspended nodes it was handed.
