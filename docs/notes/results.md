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

Any change to what a native answers is this shape. The `receive!` scaffold
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
