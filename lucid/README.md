# lucid, the Dream language server

A lucid dream is one you know you are inside of. `lucid` is the compiler,
aware of the program while you are still writing it.

It speaks the Language Server Protocol on stdin and stdout, and it is not a
second implementation of anything. Dream compiles whole programs, so importing
the compiler is an ordinary import: `lucid` links [`dreams`](../dreams/README.md)
in and calls its loader, resolver and checker directly, with no subprocess per
keystroke and no compiler output to parse back out of a pipe. An editor that
disagrees with the compiler is worse than one that says nothing, and this one
cannot disagree, because its answers come from the same tables.

| | |
|---|---|
| **Diagnostics** | the errors and warnings a build would give, in the same words, as you type |
| **Go to definition** | across files and packages, from the resolver's table of what each name means |
| **Hover** | what a name resolved to, and which module it came from |
| **Outline** | what a file declares |
| **Completion** | a module's members after `.`, and the names in scope everywhere else |

It analyses the editor's **buffer**, not the file on disk -- the loader reads
open documents from a map of path to text instead -- so it answers about the
program you are looking at, saved or not.

## Running it

```
just lucid                                   # build/lucid.dream
dream build/lucid.dream -L DIR ...           # an LSP server on stdin/stdout
```

It is an image, so the VM runs it. `-L` adds a package root, as it does for the
compiler; the standard library is found through `$MIND_STDLIB`. `--stdio` is
accepted and ignored, because `vscode-languageclient` adds it whether asked to
or not, and a server that refused it would die before reading a byte.

[`editors/vscode`](../editors/vscode/README.md) is the client that starts it.
Any LSP client can: point it at `dream /path/to/lucid.dream`.

**Rebuild it after changing the language.** `just` does not build the server,
so new syntax in the compiler reads as a parse error in the editor until
`just lucid` runs again. And close the editor before `just test`: the suite
rewrites images in place, and a running `lucid` with one mapped dies of
`SIGBUS`.

## How it is put together

| | |
|---|---|
| `main.dr` | the server loop: documents, requests, and what it keeps between them |
| `analysis.dr` | each question answered out of the compiler's tables |
| `complete.dr` | completion, which is asked of a buffer that does not yet parse |
| `pos.dr` | the one place byte offsets become UTF-16 positions and back |
| `rpc.dr` | JSON-RPC, framed as LSP frames it |

Two things to know before changing it. The compiler counts **bytes** and LSP
counts **UTF-16 code units**; `pos.dr` is the only conversion and it should
stay the only one. And completion has to offer the *locals* in scope, which is
the one thing the compiler does not record, so `analysis.dr` reads the binders
back off the syntax tree -- those scoping rules are written twice, and the
section that does it says which parts of `dreams/scope.dr` it transcribes.

Parses are kept between requests, since a file either changed or it did not;
everything derived from them is recomputed, since an analysis that is kept is
one that can be stale. [docs/notes/lucid.md](../docs/notes/lucid.md) is the
design: which tree answers which question, and why a record is a declaration
before it is a module.

## Tests

```
just test-lucid             # the units: positions, framing, URIs, completion context
just test-lucid-session     # one whole conversation, against a running server
mind test lucid             # both
```

`tests/session.sh` is the only place the server is checked as a running
program: that an unsaved buffer is what the compiler sees, that a definition in
another file is found, and that a diagnostic is withdrawn once its cause is
fixed.
