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
  And a *default* is not a maybe: `map.get () ages k` solving `v` as
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
one, because the compiler imports `std`. Changing the checker so that it
accepts something the seed's checker rejects needs one build with `--no-types`
first: build the new compiler with the seed and `--no-types`, let *that*
compile the source twice, compare, and copy it over the seed.

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
