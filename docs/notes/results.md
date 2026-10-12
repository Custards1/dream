# Results became a union

2026-10-12. A failure that is an answer used to be a list, `[:ok, value]` or
`[:error, reason]`, by convention: `std.result` named the matches, `let?`
unfolded to `[:ok, v]`, and the checker saw a tuple of an atom and something.
It is now `Result.ok value` / `Result.error reason`, a union in `std.prelude`
that every module has without an import. What follows is how it got there,
because most of what it took is a trap for the next change of the same shape.

## The prelude

`let?` is unfolded by the parser, before any name resolves, and it is
written in modules that import nothing. So the constructor it tests has to
resolve everywhere or nowhere. The loader loads `std.prelude` ahead of every
module (`modules.with_prelude!`) and `scope.lend_prelude` lends its public
names to each module that does not bind them -- the same mechanism a `mod`
uses to see its file's names, and consulted last: a module's own `Result`,
an imported one, or an enclosing module's all win, and declaring the name
after it was lent (a REPL's next entry) takes it back without a clash.

A first version matched the variant's id atom directly
(`std.result.Result.ok`, spelled in the pattern). It worked and it was ugly,
and it put a second way of naming a constructor into the language for one
user. The prelude is the general answer.

## No bridge

While the library was half moved, `let?` also read an old `[:ok, v]`, through
an inner `match` the parser wrote. It is gone: an `[:ok, v]` list is not a
result, and supporting both shapes kept two error conventions alive. What the
bridge taught is still worth having:

- **Synthetic spans.** Resolution keys a name by the offset it starts at, so
  every name the compiler writes needs an offset no source can have. They
  live past the end of any file, under 2^32 because keys are packed against
  it (`scope.ref_key`): each source offset owns 128 from
  `ast.generated_base`, shared by `union_decl` and `let?` because two of them
  never start at one offset. `receive!` has the quarter above.
- **A constructor's signature answers the whole union.** `Result.ok v`
  written by the compiler is typed `Result a e`, so a match on it carries an
  `error(:any)` variant that is never there. The bridge needed the checker to
  type such a call as its own variant; nothing does now, but the next piece of
  generated syntax that builds a union value will.

## Natives, and why the seed needed two steps

Five natives answer a result: the outcome in `[:down, p, outcome]`,
`vm.mailbox_peek!` (which every `receive!` reads), `wire_decode`,
`locate_image!` and `ffi.locate!`. They all go through `make_result`.

The seed is an image with its own compiled `std`, and it runs on the new VM.
So the VM answers a record only to an image whose header sets **bit 1**
(`emit.results_are_records`), and the old list otherwise. That is not enough
on its own: the first stage, new source compiled by the old seed, would carry
the new `std` (reading records) but the old `emit` (no flag) and the old
`receive!` scaffold (reading lists). What worked:

1. C1: the old source plus only the new flag and the new `receive!` scaffold,
   built by the seed. Unflagged, old `std`, lists throughout.
2. C2: the whole new source, built by C1. Flagged, new `std`, records.
3. C2 rebuilt until it is a fixed point; that is the seed.

Once the seed read records the flag had done its job, and the next change
took it out: Dream has had no release, so no image outside this repository
needs the lists, and the VM answers records to everything. Any later change
to what a native answers is this shape again -- a flag for the crossing, then
the flag removed. The `receive!` scaffold
takes the peeked record apart by position (`_match_at`), not by
`Result.ok(m)`: its names resolve where the `receive!` is written, and a
module may have a `Result` of its own.

## The rewrite, and what it could not tell

About 1,440 sites were rewritten by a tool that used the compiler's own
parser for exact spans (expression or pattern, parenthesised where it is an
argument). Then the type checker found most of what was left, the tests the
rest. What a span-exact rewrite still gets wrong:

- **A pair that starts with `:ok` is not always a result.** `get_and_update`
  and `server.answer!` take `[reply, state]`; tests compared `[tag, tag]`
  lists; SQL tokens and query pieces are tagged `:error` among `:text` and
  `:param`. Each was reverted by hand.
- **Positional reads.** `list.head r == :ok`, `r.[1]`, `list.nth 1 r`,
  `let [_, v] = r`. A signed producer makes the checker report them; an
  unsigned one does not, and **`list.head` of a record answers its first
  field instead of raising**, so some of these failed only as a wrong value
  far away. `Result.tag` exists for the tests that asked "which one".
- **A type-grammar span** that did not start at a bracket
  (`(:ok | [:error, t])`): check that each span's text is the form you
  matched before rewriting it.
- **Names.** Two libraries had a type called `Result` (`foreign`'s C return,
  `pg`'s statement reply), and `derive std.server` resolves the base's names
  in the deriving module. They are `Return` and `Reply` now.

## What is still a tagged list, and why

The parsers' own steps, `[:ok, item, rest]` (`parser.dr`, `lexer.dr`,
`std.parse`, `mind`'s condition parser): three elements, internal, never a
result anyone outside sees. Where one leaves as a result it is converted
(`parser.parse_expression`, `lexer.lex`, `fmt.format`).

## Option, and one `let?` for both

2026-10-12. `Option a` (`some(value)`, `none`) joined `Result` in the prelude,
and what to do with either is a member of its union with a signature
(`Result.map`, `Option.flatten`, ...). A union body may now sign a member
(`map : (a -> b) -> Result a e -> Result b e` on the line before it), which
is what lets the checker type a chain through the members; the seed could
not parse that, so the reseed was two steps again: C1 from a copy whose
prelude had its signatures stripped, then the real tree.

`let?` chains both. Unfolding it to two arms, `Result.ok(x)` and
`Option.some(x)`, would write the rest of the block twice per step -- 2^n for
a chain of n -- so it unfolds to one arm with a pattern of its own,
`ast.success_p`, which matches either variant *by id*
(`ast.success_ids`): a module with a `Result` of its own still chains the
prelude's. Scope binds through it, lowering tests the two ids and reads field
0, and the checker binds the payload to whatever the scrutinee's `ok`/`some`
holds and reports a `let?` on any other declared type. `let? x = e else alt;`
answers `alt` instead of passing the failure on.

`std.result`'s functions became the prelude's members at their call sites
(`result.or_else d r` is `Result.unwrap_or d r`; `result.reason d r` answers
an `Option` now), and `std.result` keeps short spellings -- `result.ok`,
`result.err`, `result.map` -- as wrappers, which compile to the member call.

The library's own "nothing there" is still `()` (`list.find`, `map` lookups
with `else ()`); `Option.of` and `Option.or_unit` cross between the two.

## Fusion and block cleanup

2026-10-12. Saturated prelude members are lowered into the caller's frame:
`map`, `map_error`, `and_then`, `or_else`, `otherwise`, `flatten`,
`unwrap_or`, and `unwrap_or_else`, where the union provides them. Resolution
records roles by global identity, including direct aliases; a local parameter
or a different union's member stays an ordinary call. Adjacent maps carry
lazy payload expressions to the final variant, and unchanged branches keep
the input record. Calls that can choose a new variant start another region,
so lowering does not copy the remaining chain into both branches at each
callback. Doubling an alternating callback chain grows its image linearly.

Scratch slots must remain distinct from source bindings and from each other.
The first attempt reused a slot while an `and_then` answer still carried a
lazy read of it: assigning the answer to that slot made it depend on itself.
Two binary tests caught it. A pipe also starts where its input starts, so
keying its scratch binding by the whole expression's start overwrote the
input's. Scratch slots now have their own resolution table, keyed by the
callee's ending offset, and each region reserves its own slot for the lifetime
of the frame. A single-character alias inside an index fallback exercises
the remaining case where a source binder shares the call's start.

The prelude also lends `ok value` and `err reason` as typed functions for
the two Result constructors. They work as values and through `let?` and `?`,
and a module's or local binding with either name wins in the usual way.

One interpreter run over 20,000 two-map results, comparing the fused function
with the same members passed through a parameter, answered 400060000 in both
cases. Fusion used 500049 reductions and 7232648 allocated bytes, against
880049 and 15796624 for the ordinary calls. These are a workload comparison,
not a promise about compiler throughput or wall time.

`defer! expression;` is a direct block statement. Lowering wraps the statements
after it in `ensure` (opcode 83), whose cleanup is the deferred expression.
Nested ensures give reverse registration order and register only after the
earlier statements have run. A final defer has a unit body. The interpreter
saves the body's answer or error in a rooted continuation while cleanup runs;
a cleanup error replaces a normal answer but preserves an existing error.
Both edges belong to image validation, sharing, and the JIT's reference walk.

Kills now unwind through ensures instead of discarding the machine state.
The kill is injected once, and a cleanup-depth counter lets deferred code
park and handle its own errors without allowing an enclosing body handler to
catch the kill. A pending kill waits through normal cleanup as well. The same
rule applies in a native's nested forcing loop, where an endless `strict!`
may never return to the scheduler. The process finishes and notifies its
joiners and monitors after its registered cleanup has run.
