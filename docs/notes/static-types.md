# Static types

The checker, compile-time contracts, and what a signature buys the JIT. Moved out of `CLAUDE.md` on 2026-09-25; the prose is as it was, so a date or number in it is as of when it was written.

`let add : :integer -> :integer -> :integer` is checked at compile time, by
[dreams/typecheck.dr](../../dreams/typecheck.dr), which runs after `scope.resolve!`
and before lowering and produces diagnostics and nothing else. Its header is
the design; what is here is what to know before changing it.

**The three rules, and every false positive so far broke one of them.**
Unannotated code is never an error (a name with no signature is `:any`); only
a definite mismatch is reported (`sub` answers "could this be that?"); and the
pass never changes the program. Three things that looked like checks and were
not, each found by running the checker over this repository:

- `1 + "x"` is not an error. `examples/05_errors.dr` and
  `dream/tests/programs/errors.dr` write it on purpose inside `try!`, and in a
  lazy language a line nobody forces never raises. So an operator mismatch, or
  a value applied as a function, is reported only when a *declared* type is
  part of it (`from_literals`): a literal is evidence of nothing but itself.
- A record nobody annotated gets no signatures. `Person.greeting %{}` reads a
  defaulted field of an empty map and is `"Hi"`; a signature saying the
  accessor wants a whole `Person` rejected it. A `mapping`'s accessor now asks
  only for the field it reads, and for any map when that field has a default.
- A type variable *solved* from an argument is not a promise.
  `list.fold (fn acc j -> acc + [j]) [] xs` solves the accumulator from `[]`,
  and holding the lambda's answer to "an empty list" rejected half the
  compiler. A lambda passed as an argument takes its *parameter* types from
  what was solved (`solved_inputs`), and its answer is only bound, never
  checked against a solved variable.
- `()` is "nothing there", and a union with `:unit` in it is how the library
  says "maybe". Two halves. A *test* narrows: an arm below `() =>` sees the
  rest of the scrutinee's union (and so does a scrutinee that is a name),
  and `x == ()`/`x != ()` under `if`, `&&`, `||`, `not` or a guard narrows
  `x` in the branch the outcome decides; `type_of x == :kind`, and a `match`
  on `type_of x` whose arms are kinds, narrow to that kind or away from it;
  `x == :atom` picks one value out of a union (a fieldless `union` variant is
  one), and `list.head x`, `x.[0]` or `x.[:kind]` compared with an atom picks
  out the tagged lists or records that could carry it, which is how a
  `union`'s variants with fields are told apart; and inside a `match` arm the
  scrutinee is cut down to what the arm's pattern could match. Only atoms,
  booleans and `()` narrow -- `3 == 3.0`, and a `bigstr` is `==` its string,
  so a number or a string literal says less than it looks. A read with no
  fallback that raises for a member drops that member from *both* branches:
  `s.[0] == :circle` false leaves the square, not `:empty`. `list.head` is
  known by its global index, as fusion knows `range` (`heads_of`).
  `x` may be a global (keyed by its index, so a local of the same spelling
  under the test is not it) but not an impure name, and `:any` is never
  narrowed, so rule 1 still holds of unannotated code -- "narrowing" in
  [dreams/typecheck.dr](../../dreams/typecheck.dr). It only ever removes members it
  can see a value cannot be, so an incomplete answer narrows less and never
  reports more. Before it, `if r == () { 0 } else { r + 1 }` was a report.
  And a *default* is not a maybe: `map.get () k ages` solving `v` as
  `:integer | :unit` made every use of the answer a report, though the
  default was written so that nothing need test. A `()` argument never decides a variable
  (`solve`), fits one wherever it was written even once another argument has
  decided it (`admit_unit`), and fits a declared variable in an answer
  (`sub`), which is what lets `list.minimum` be `[a] -> a`. Two things std
  still answers `:any` for, deliberately: a read by *position* (`head`,
  `nth`, `array.get`), because a list here is as often a record as a
  sequence and a tuple's element type is the union of its fields; and a
  decoded message, because the reader knows its shape and the format's
  `Value` would only say it might be a list. The head of
  [mind/std/list.dr](../../mind/std/list.dr) says the same.

The check to run after any change here: every `.dr` in the repository must
check clean -- `std --test`, `dreams --test`, `lucid`, `mind`, the examples,
`dream/tests/programs` and the benchmarks -- because every report on code that
works is a report nobody asked for, and the first thing a user does with a
checker that cries wolf is turn it off.

**What it costs.** About 290 ms of a 5.5 s self-compile, ~5%, with every image
byte-identical. The first version cost 578 ms, and nearly all of the
difference was one case: most calls in any program are to something no
signature describes, and a callee of type `:any` now skips the argument
bookkeeping entirely (`infer_apply`). `infer`'s arms are ordered by how common
each node is, because a `match` tries them in turn -- worth another 15%.

**Where it reads from.** A name in an expression is the resolver's answer
(`scope.resolution`), so the checker never re-implements name lookup. A name
in a *signature* is the exception: signatures are never resolved, because they
never run, so `type_ref` looks a type name up in the module's environment the
way `scope` would -- a module alias first (that is how `Shape` names a union
and `Point` a record), then a global, then a selective import.

**Named types stay folded** (`[:named, shown, gi, args]`) and unfold on demand,
because `type Tree = :unit | [:integer, Tree, Tree]` is recursive. `sub` spends
fuel on each unfold, and runs out to *true*: an answer the checker cannot reach
is not a mismatch.

**Exhaustiveness is deliberately narrow.** Only a scrutinee whose type is a
*name* -- a union somebody declared -- is held to it, and a variant is reported
only when every arm certainly misses it. A guarded arm, or one that takes a
field apart with a nested pattern, counts as handling. A false report here
teaches people to write `_ =>` everywhere, which is worse than no check.

**Bootstrap order.** The seed must parse a signature before `std` may contain
one, because the compiler imports `std`. There is no way to switch the
checker off (`--no-types` was removed on 2026-10-03), so the compiler's own
source must also satisfy the seed's checker: a change that makes the checker
accept something the seed rejects lands in two steps -- the checker first,
reseeded, and only then the source that relies on it.

## What an unsigned function answers: inferred, and only ever a warning

Added 2026-10-04. A signature reached only the code that named it directly,
and most code names functions that have none -- so `let label n = "item " +
to_string n` was `:any` to every caller, and `half (label 3)` with `half :
:integer -> :integer` passed. Now the check runs **twice**. The first is the
check as it was, and the walk it makes of each unsigned body already returns
what the body answers (`check_decl_inferring`). Those answers become the uses
of their globals, parameters still `:any`, and a second check holds every
body that names one of them (`check_again`). What only the second finds is a
**warning** with a note saying how it was found (`typecheck.warned`); an
error is still only what a signature promised, so rule 1 holds.

What an answer is loosened to (`answer_type`): literals widened all the way
down -- a body answering `1` answers an integer -- except atoms, which are
tags; anything mentioning a type variable dropped; and **the `()` of an answer
that is sometimes something else dropped**. The last is the one that decides
whether this is usable. Without it the repository had forty warnings, every
one a test helper's `match parse xs { [:ok, c] => c, _ => () }` handed to a
typed function -- the same "nothing there" `solve` already declines to report
for `map.get () k m + 1`. With it the whole repository, compiler, `std`,
`mind`, `lucid`, examples and every `when test`, had two, and both were worth
fixing: `target.dr` read a list containing `()` after testing for one (the
arch functions now answer a list, empty for unknown), and `cc.Target.kind`
was `:atom` where it meant `Kind`.

One round, not a fixed point: what an unsigned function answers is computed
seeing every other unsigned function as `:any`. A second round would reach
further for another whole check.

**Cost.** The second check is a check of most bodies again -- a body is
skipped only when it names no newly typed global, and in a program written
mostly without signatures most bodies name one. On a self-compile `types`
went 183 ms to 355 ms sequentially (`--time`), and the parallel build 2.17 s
to 2.34 s. Measured, not yet reduced; the obvious next step is to recheck
only the positions where a type is consumed (a typed call's argument, an
operand, a scrutinee), which the skip test does not yet distinguish.

**Found on the way:** `solve` learnt nothing from a union, so `list.contains
:map ks` with `ks` a union of lists solved `a` as `:map` and held every list
to it -- an error a signature-only program could hit too. A union whose
members all have `p`'s shape now solves member by member; one with a member of
another shape is left alone, so the message about it is still `[a]`.

## Compile-time contracts: a refinement run against a value the compiler has

Added 2026-09-25. A `where` in a named type used to be checked as its base,
always, because a predicate is a function. Where the value is known while
compiling -- a literal argument, a local bound to one, a `comp` result -- it is
now *run*, and a value it rejects is a compile error at the place it was
written. That is what lets a library turn a signature into a compile-time API.
Language-level description: "Compile-time contracts" in
[docs/language-spec.md](../../docs/language-spec.md). The design, in the order the
compiler meets it:

- **`lower` keeps what each `comp` came to** (`lower.comp_values`, keyed by
  source and span start), because the checker runs after `settle_comps!`
  rather than before it. A `comp` is typed by its value, and a signature-less
  global whose body is a `comp` is that value's type at every use
  (`typecheck.with_comp_globals`).
- **The checker writes a contract, not a verdict.** Where a known value fits a
  type that could refine it, `contract_of` builds a *formula* over the
  predicates: `true`/`false` settled from structure, `[:pred, gi, path,
  datum]`, `[:all, ..]`, `[:any, ..]`. Structure decides which predicates
  matter -- a union is `any`, a list `all` -- so `()` meets `Port | :unit`
  without running anything. Compile-time values are spelled as *datums*
  (atoms, lists, arrays, maps tagged) because an atom the compiler never
  interned cannot be made into one. Contracts ride in the diagnostics list as
  lists (a diagnostic is a map) and `analyze` takes them back out; threading a
  second accumulator through the whole walk was the alternative.
- **The predicate is reached through the type's value.** `type Port = t where
  p` is `let Port = [:named, "Port", [:refine, t, p]]`, so `p` is at path
  `[2, 2]` of global `Port`, and the path is read off the declaration's syntax,
  which has the value's shape. So nothing is re-resolved or recompiled: the
  answer is the closure `types.check` would apply at run time.
- **[dreams/contract.dr](../../dreams/contract.dr) runs them all at once**: one
  function appended to a *copy* of the linked arena answers a list, one
  element per contract, each inside its own `try`, so a raising predicate is
  its own call's failure. One image and one VM start per program, and none for
  a program without contracts. The written image never changes -- the test
  holds that byte for byte.
- **The cost is gated.** `mentions_refine` marks each named type once when the
  checker is built, and `refines` asks that before any datum is made, so a
  program with no refinement pays a lookup per checked literal. With the
  `comp` typing, the type pass on a self-compile went from 14.87M to 15.18M
  reductions (0.16% of the compile), and every image in the repository is
  byte-identical. The checker is now also built once for the diagnostics, the
  contracts and both JIT parameter masks (`typecheck.analyze`) where it was
  built three times; that saving is smaller than the new work and not visible
  on its own.

What it does not reach: a `where` written inline in a signature (never
compiled), a parameterised type (its value is a function), `--check` (nothing
runs), and a value only known at run time. `dreams/tests/contracts.py` holds
the cases. A `comp` that fails to evaluate is now reported at its own span; it
used to be reported with none.

## What a signature buys the compiled code

The first use of the types for speed, 2026-09-25, and the rule it rests on is
the one to keep: **the types are gradual, so a signature is a hint and never a
promise.** An unannotated caller may hand a `:float` parameter an integer, the
checker cannot object (`:any` fits everywhere), and the program is owed the
answer the interpreter gives. So nothing is *trusted* on a signature's say-so;
it chooses a representation, and the representation is guarded.

What flows: `typecheck.float_params` reads which parameters each top-level
signature declares `:float` (a name for one, a float literal and a union of
floats count; `:number` and a type variable do not), `main.dr` maps globals to
functions, and the image carries a `TYPE` section of `[func, mask]` records
(docs/bytecode-format.md). A program with no float in a signature has no
section and is byte-identical to before -- which is why the bootstrap seed only
moved because the compiler's own source did.

What the JIT does with it, all in [dream/src/jit.cpp](../../dream/src/jit.cpp):

- **A declared float starts the float fixpoint as Float.** That is what reaches
  `integrate (x + dx) hi dx (acc + x * dx)`, a loop with no float literal in its
  self call, where inference alone never says `x` is one. Only parameters the
  body forces on every path are seeded: a merely carried one may arrive as a
  suspension nobody is allowed to force, and would bail every call.
- **The first iteration is peeled** when the entry cannot force the float slots
  in the body's order. That rule used to drop the whole specialization, so it
  matters for untyped loops too; now the first iteration runs over tagged slots
  and only its back-edge crosses into doubles, as a guard (`enter_loop`). A
  carried slot is re-read at the back-edge, because the argument after it is
  usually what forced it -- a raw read taken earlier is the caller's thunk, and
  that was a 20x slowdown before it was found.
- **A declared-float peer gets a typed variant**, taking doubles, beside the
  generic one; a call site passes doubles straight through, checks a tagged
  argument for a float box, and falls to the generic variant otherwise.
- **A peer whose body is a float returns a raw double**, typed or not. The bits
  ride in the return register and are read only after the status says
  `JIT_OK`.
- **A run of bails gives the function up.** A guard that fails hands the call to
  the interpreter; sixteen in a row (`Jit::note_bail`) deoptimize it, since a
  peeled loop fed integers would otherwise pay a wasted first iteration per
  interpreted iteration.

| | untyped | typed | allocated, typed |
|---|---|---|---|
| `integrate`, 3M steps, no float literal in the self call | 213 ms | **23-32 ms** | 192 MB -> 230 KB |
| `term` helper called from a loop, 3M calls | 135 ms (99 now, from the double return) | **39 ms** | 192 MB -> 297 KB |
| escape-time loop, floats already inferred | 63 ms | 62 ms | -- |

The benchmark's seven rows did not move. `dream/tests/programs/jit_types.dr`
holds the tier agreement, including integers passed to declared floats through
an unannotated caller, a first iteration that raises, and both give-ups.

**What a type cannot do here, measured rather than assumed:** admit a function.
The admission rule is strictness -- every parameter forced on every path -- and
a type says what a value is, not whether it is evaluated. The escape-time loop
written `if i >= limit { i } else if zr * zr + zi * zi > 4.0 ..` compiles
nothing typed or untyped, because `zr` is not forced on the first path; the
same loop with the two tests swapped is compiled either way.

## Records are values of their own

2026-10-09. A `group` was a list, a `struct` an array, and a union's variant
the tagged list `[:circle, r]` (or the atom `:empty`). That was what made
records cheap to add and what kept them from being types: `type_of` on a
`User` said `:array`, the checker typed a read by position as `:any` because
a list here was as often a record as a sequence, a union's tag could collide
with any list that began with the same atom, and "is this a `User`?" could
only mean "is it the right length". Now a `group`, a `struct` and every
union variant is a value of its own; a `mapping` is still a map, because it is
built from map literals and decoded JSON and is meant to be.

**What a record is.** `RecordObj` in dream/src/value.hpp: the fields, lazy,
by position, and one atom naming the declaration, qualified by its module --
`app.Point` for a record, `app.Shape.circle` for a variant, which a flag bit
marks. `type_of` answers the declaration's own name (`:Point`, `:Shape`),
printing writes `Point(3, 4)` and `Shape.empty`, equality wants the same atom
and equal fields, `compare` orders by the atom and then the fields, a variant
with no fields is a map key by value as an atom is, and `send!`, `std.wire`
(`WIRE_RECORD`), `std.image` and a `comp`'s answer carry one across as itself.
Fields are read and changed through `get` and `set`, the opcodes an array's
are, so the accessors `syntax.record` generates did not change at all -- and
the interpreter's accessor shortcut, `thunk_for`'s read and the JIT's
`dream_rt_peek` take a record where they took an array.

**How one is made.** A constructor's body is `_record_of :app.Point false
#[x, y]` (`syntax.constructed`), an ordinary call of a primitive, so that the
resolver, the checker, the linter, the formatter and the language server see
nothing new. Lowering recognises that exact shape and emits one `record_make`
node instead (`lower.lower_apply`; opcode 81, `a`/`b` the fields, `c` the
atom with the variant bit on top). The resolver files every constructor in
its wrapper table as `[[:record, id, variant, rewritable, fields], plan,
arity]` (`scope.constructor_of`), which does two things: a saturated call of a
constructor whose fields are lazy and passed through in order becomes the
`record_make` itself, as a call of an accessor becomes its `get`; and every
part of a parallel build -- lowering's and the checker's -- can find the id of
a constructor another part walked, because the wrapper table is the one thing
each part is given whole. A constructor with a strict field is filed but not
rewritten: its frame is what forces the field. Merging parts renumbers
`record_make`'s atom as it renumbers `const_atom`'s (`opt.remap_field`, and
`ShareArenas::rebuild_node` in C++); that was missed the first time, and a
`Point` built in one part came out named after whatever atom had that index
in another.

**How one is matched.** `Shape.circle(r)`, `Shape.empty`, `Point(x, y)` --
`[:record_p, ctor, fields, span]`, the constructor an expression resolved
like any other, `Point(..)` meaning `Point.make`. It lowers to `_record_is id
subject` and then each field by `_match_at`, as an array pattern's elements
are; a name that is no constructor or the wrong number of fields is a compile
error. A dotted name in a pattern was an error before, so any `a.b` there can
begin one; a capitalised name needs its `(` to touch it, so `let f P (x)` still
reads as it did. A member is compiled inside the declaration's own module,
where `Shape` names nothing, so the loader reads `Shape.circle` in a member as
the local `circle` (`syntax.own_names`) -- otherwise a member would have had
to write a bare `empty`, which in a pattern binds anything.

**What the checker does with it.** A description `[:constructed, id, [field
types]]` becomes `[:nominal, id, [t, ..]]`, and `sub` holds it exactly: the
same id and fields that fit, and nothing else -- not a tuple, not an array.
A `group` and a `struct` now always give their generated functions
signatures, so `Point.x [1, 2]` is an error where it used to be allowed. A
constructor pattern binds its fields to that variant's field types, covers
what that constructor made for exhaustiveness, and an arm whose pattern no
value of a declared type could fit -- `[:circle, r]` against a `Shape` -- is
reported, which is what found most of the migration below.
`resolved_pattern` asks the resolution once per arm for every constructor in
a pattern, because `covers`, `may_match` and `arm_misses` have only a pattern
and a type to go on.

**The bootstrap took two reseeds.** The seed has to parse whatever the
compiler imports, and has to know a primitive by name before `std` may call
it. So the first seed carried the VM's record kind, the four primitives and
the lowering, with nothing in the compiler's own reach using any of it; the
second could then parse constructor patterns in `std`. Four records the
compiler used to describe *syntax* -- `Span`, `RecordField`, `RecordMember`
and the IR's `Node` -- became plain list functions instead: syntax is lists
that macros build and read, and an IR node is read by position in C++ by
`vm.share_arenas`.

**What moving to it found.** Most of the migration was mechanical -- `std.build`'s
`Input`/`Op`/`Goal`, `std.build.cc`'s `Setting`, the PostgreSQL driver's
`Message`, `ship`'s `Item`, `sleep`'s `Stage` -- but three things were not:

- `std.sql.pg.db`'s `forced set p !v = set p v` was called with the value
  second and the record third, so `!v` forced the record and left the list it
  existed to force suspended. Invisible while a `Pending` was a list too; a
  type error the day it was a record.
- `random.split` hashed `to_string rng`, so its streams depended on how a
  generator printed. It hashes the text it always hashed, spelled out, so a
  recorded seed still replays.
- `std.build`'s key encoding fell through to `"?" + type` for anything it did
  not know, which would have given every record of a type the same key. A
  record now encodes as its id and its fields.

**What it costs.** A record is one word bigger than the array it replaced,
for its id. A loop that makes a `struct`, reads both fields and sets one,
three million times, same VM, compiled by the compiler before and after:

| | before (array) | after (record) | |
|---|---|---|---|
| interpreted | 884 ms | 778 ms | -12% |
| compiled | 106-114 ms | 111-113 ms | level |
| reductions | 7.07 M | 4.08 M | -42% |
| bytes allocated | 203 MB | 248 MB | +22% |

The interpreter is faster because a constructor call is now the
`record_make` itself rather than a call of `make`. The compiled loop was 20%
*slower* until `Emitter::get` learnt to read a record's field inline, as it
read an array's -- 24 bytes in rather than 16, which a `static_assert` beside
`RecordObj` holds it to; before that every accessor in compiled code was a
call of `dream_rt_get`. The self-compile, whose tokens and contexts are
records now, is 2.59 s against 2.36-2.44 s before, with 3% more reductions and
allocation -- over a compiler that also grew 3% in nodes in the same change,
so the two are not separated here.

**What is not done.** A record's field types cannot name another record
declared beside it, because they are resolved inside the record's own module
(`std.time.zone` writes `:any` where it means `Period`). `mapping` is still
nominal only in name. And the dispatch on `type_of` that generic code does
-- `match type_of v { :list => .. }` -- now sees `:Point` and falls to its
default arm, which is right for a record and is the thing to check first when
a walker that used to handle records stops.
