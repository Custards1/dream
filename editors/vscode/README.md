# Dream for VS Code

Syntax highlighting, and everything else from `lucid`, the Dream language
server — which is the Dream compiler, answering an editor's questions out of
the tables it already builds to compile with.

- **Diagnostics** as you type, from the compiler's front end. They are the
  errors a build gives you, in the same words, because they are the same errors.
- **Go to definition**, across files and packages.
- **Hover**, saying what a name resolved to and which module it came from.
- **Outline** of what a file declares.

The buffer is what gets analysed, not the file on disk — so this works on code
you have not saved, and on code you have never saved.

## Requirements

The Dream VM on your PATH (or set `dream.vm.path`), and a built server image.

```
just lucid          # writes build/lucid.dream
```

The extension looks for `build/lucid.dream` and `build-dream/bin/lucid.dream`
in the workspace, then `$LUCID_IMAGE`. Set `dream.server.path` to point
somewhere else.

`lucid` is a compiled `.dream` image rather than a native program, so it is run
by the VM: the extension starts `dream lucid.dream -L …` and talks LSP to it
over stdin and stdout.

## Settings

| | |
|-|-|
| `dream.vm.path` | the VM. Default `dream`. |
| `dream.server.path` | the `lucid.dream` to run. Found automatically when empty. |
| `dream.packagePaths` | extra `-L` roots. The workspace folder is always included. |
| `dream.stdlib.path` | where the `std` package lives; `$MIND_STDLIB` when empty. |
| `dream.trace.server` | log the LSP conversation. |

A project that cannot see `std` reports every import of it as an error, which
looks like a broken extension rather than a missing setting — so if every file
is suddenly red, that is the one to check.

## Developing

```
npm install
npm test            # tokenizes Dream with the grammar and checks the scopes
```

The grammar test is worth keeping honest. A TextMate grammar is a pile of
regular expressions with no compiler behind it, and the usual way to discover
one is wrong is to squint at colours. Three rules were wrong when the test was
first written: `let` ate its own declaration, every builtin lost to the
impure-name rule, and `%{` was read as a modulo.
