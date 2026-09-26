# The language server

How `lucid` answers an editor, and which tree answers which question. Moved out of `CLAUDE.md` on 2026-09-25; the prose is as it was, so a date or number in it is as of when it was written.

`lucid` is the compiler answering an editor's questions. It imports `dreams` and
calls its resolver directly — there is no subprocess and no re-parsing of the
compiler's output, because a whole-program language makes "import the compiler"
an ordinary import.

```
just lucid          # build/lucid.dream, which the VS Code extension looks for
```

The extension starts it over stdio, and `vscode-languageclient` appends
`--stdio` to the command line by itself. `lucid` accepts that flag and ignores
it; a server that rejects an unknown option dies before it has read a byte, and
the editor reports only that the connection is erroring.

It analyses the editor's **buffer**, not the file on disk. That is what
`modules.load_overlaid!` is for: a map of path to text the loader reads instead
of the disk. Anything else would answer questions about a program the user is
not looking at.

Two things to know before changing it. A member access is recorded at the head
of its chain — `helper.double` is keyed where `helper` begins — so a cursor on
the field walks back over the dot. And the compiler counts **bytes** while LSP
counts **UTF-16 code units**; `lucid/pos.dr` is the only place that conversion
happens, and it should stay that way.

## Completion is asked of something that is not a program

Every other request is asked of a program. Completion is asked of a buffer
mid-keystroke, and that difference decides the design — it is the thing to
understand before touching [lucid/complete.dr](../../lucid/complete.dr).

At the moment a completion is wanted the buffer usually says `console.`, and
**that does not parse**. Measured rather than assumed: the loader reports zero
modules for it, so there is no environment, no global table and no resolution —
nothing to complete *from*. So the text is tried twice. The buffer as written
comes first, because where it already parses that answer is exact and inserting
anything into it can only be wrong. When that yields no module, a name nothing
would write is inserted at the cursor and the repaired text is analysed instead.

The two fail in *different* places, which is why both are kept. A bare name is a
statement but it is not a declaration, so a cursor on a blank line between two
top-level `let`s is exactly where the repair breaks a file that was fine — found
by asking `dreams/lower.dr` for completions at line 300 and getting nineteen
keywords. Everything read here is before the cursor, so both candidates agree
about every offset that matters.

What is offered comes from the compiler's tables and is ranked in the order
resolution would reach it: a local, a global of this module, an import, a
builtin, a keyword. After a dot it is `scope.exports` — the same list the
compiler quotes back when a member is misspelled — and the chain in front of the
cursor is resolved by handing a synthesized expression to `scope.as_namespace`,
so "is `list` a module, an import, a package or a local" has one answer and not
two. A host module offers nothing, because the host owns its member table at run
time and the compiler has never seen the names.

**The locals are the exception, and the only place this server re-implements
anything.** The resolver knows what is in scope at every point of its walk —
that is what a frame stack is — but it pushes and pops as it goes, and when
`resolve!` returns nothing survives saying what was visible at a given byte.
So `analysis.dr` reads the binders back off the syntax tree, and the scoping
rules are therefore written twice. That section is deliberately literal: every
case is a transcription of the matching case of `check_expr`, `check_block` or
`check_arm`, in the same order, so the two can be read side by side. Where
`scope.dr` exports the rule itself it is called rather than copied —
`scope.pattern_bindings` is what says which names a pattern binds, here as
there. The one case that is easy to get wrong is `comp`, which is walked with an
empty frame stack because it runs before the program does, and so drops
everything the enclosing constructs had put in scope.

Hover reads the same walk. It shows a function with the parameters it was
declared with — `fold f acc xs` rather than `fold` — and takes them from the
declaration rather than from the resolver, which keeps arity and not names.
They are sliced out of the source, so a parameter that is a pattern reads as it
was typed. Hovering a name at its own *definition* still answers nothing, which
is not new: `refs` records uses, and a binder is not one.

One cost worth knowing: a completion is a whole-program analysis, as hover and
go-to-definition already are. On `dreams/lower.dr`, which pulls in the whole
compiler, that is roughly a second per request.

## Two trees, and which question goes to which

`lucid` holds the program twice: as the editor wrote it, and after macro
expansion. Which one answers is not a matter of convenience — it is exact, and
getting it wrong is silent.

- **What a name means** is asked of the **expanded** program. That program is
  what runs, and there is nothing else for a name to mean.
- **What is written** is asked of the **source**: which locals a cursor can
  see, what parameters a declaration was given, whether it was a `macro`.

The reason is that expansion *replaces* an `expand` with generated syntax
carrying offsets from a reserved part of the 32-bit space. Every byte the user
is looking at inside `expand twice counter` is then inside no node at all, so a
walk of the expanded tree finds nothing there — and `let double counter =
expand twice counter` offers no `counter`, which is precisely the local the
person typing it wants. `analysis.source` is the second loader, and
`source_items` is what the binder walk, `find_decl` and `params_of` read.

An `expand`'s own name is the other half of the same problem, and it is the
compiler that fixes it rather than the server. That name is a name occurrence
like any other, but the only pass that ever resolves it is the one that
replaces it, so the resolver walks a program the name is not in and nothing
would ever record what it meant. So expansion writes each one down
(`modules.expansions`) and `scope.resolve!` seeds its occurrence table with
them — which makes hover and go-to-definition on a macro the *ordinary*
lookups, not a second mechanism. Nothing downstream reads those entries:
lowering asks about nodes, and the nodes at those offsets are gone.

Only a macro may be written after `expand`, so that is all completion offers
there — not a local, not an ordinary global, not a builtin, not a keyword, all
of which are things that cannot be written in that position. The keyword in
front of the cursor is found lexically (`word_before`), for the reason the
section above gives: at the moment a completion is wanted, `expand tw` does not
parse.

## A record is a declaration before it is a module

`group Point { x, y }`, `struct`, and `mapping` are rewritten by the *loader*
into a module of generated functions — a constructor, an accessor and a setter
per field ([syntax.record](../../dreams/syntax.dr)). That happens before anything
resolves, which is what makes records cost the rest of the compiler nothing;
and it is why a language server had nothing true to say about one.

Three things are gone by the time the resolver runs, and each was a visible
defect:

- **The declaration is not among the module's globals.** `Point` became a
  module of its own and the parent got an import of that name, so
  `scope.module_defs` has no `Point` in it and the outline of a file full of
  records was empty.
- **The members carry spans into no file.** Several of them stand where one
  declaration was written, so `syntax.record` freshens them into a reserved
  part of the offset space. Go-to-definition on `Point.x` clamped to the end of
  the file, and the signature — which lucid built by *slicing the source* at
  each parameter's span — came out `make  ` and `x `.
- **The kind and the fields are gone.** `:list`/`:array`/`:map` is what the
  rewrite builds from; `group`/`struct`/`mapping` is what the reader typed.

`modules.records` is what the rewrite knew, kept: `[owner, kind, name, fields,
members, span]` per declaration, collected by `record_items` alongside
`expand_items` and under the same `when` conditions, so a record a configuration
switches off is no module and no record either. **Nothing in the compiler reads
it** — it is there for `lucid`, the same way `modules.expansions` is, and for the
same reason: the pass that rewrites something is the only one that ever sees what
was written.

What it buys, all in [lucid/analysis.dr](../../lucid/analysis.dr)'s "records" section:
the outline lists each record with its fields and members underneath it, in
source order; hover says `` `x record` -- reads `x` of the group `Point` ``,
`` `set_y record value` -- replaces `y` in the struct `Vector` `` and
`` `new host` -- builds the mapping `C`, defaulting `retries` ``; and
go-to-definition on a generated name lands on **the field**, in the module the
record was written in — not on the record's own module, whose "path" is a dotted
name that `location` would have made a URI out of.

One fix that is not about records and should be kept in mind for any generated
code: `signature` now shows a parameter by its **name**, and only slices the
source for one that is a pattern, which has no name. Slicing was never right for
a declaration the compiler made up, and a record's generated helpers are all of
them.

**A member is the exception, and it is the one that needed no work.** Everything
above exists because the rewrite throws the author's spans away. It throws away
only the spans it *made up*: `syntax.record` freshens the generated declarations
and appends the members after, un-freshened, so a member's body and parameters
are still at the offsets they were typed at. That is not tidiness — `fresh`
exists because resolution keys names and binders by span, and the generated
declarations need new ones because several stand where one declaration was
written and a default is emitted twice (in the accessor's `else` and in `new`'s
body). A member is emitted once, so it needs nothing. The consequences are worth
stating because each is a thing that had to be built for every other generated
name: a "cannot find" inside a member body is reported **where it is written**,
go-to-definition lands on the member itself, and `shown_signature` reads the
real parameters out of the file, so `self` is shown because the author chose it.
Generated spans live above 2^31 and written ones below, which is what keeps the
two from colliding.

## A host module's members come from the host

`import std.vm` compiles to a lookup that happens while the program runs, so
`dreams` knows such a module by name and by nothing else — which is why
completion after a dot on one used to offer nothing at all. But `lucid` runs
*on* the VM that owns the table, so it asks: `vm.host_members "std.vm"` answers
`[name, arity]` for each member, in declaration order, and `()` for a module
that is not registered. A variadic member's arity is `-1`.

Two things that follow. A module an embedder registered and named to the
compiler with `--host-module` is not in *this* VM, and `host_members` answers
`()` for it — nothing offered, which is what was offered before. And the
compiler still does not check host member names at compile time: it could now,
but a program may be compiled for a VM other than the one compiling it, and
that is the bargain `is_host` makes on purpose.

The *builtins* had the same shape of problem for a smaller reason: what each
one takes and answers was written as a comment beside its name, and a comment
is no use to a server, so completion on `len` could show nothing but `len`.
`builtins.signatures` is that comment moved into the program, as a map rather
than a second list so that adding a name and forgetting its line is a failing
test.
