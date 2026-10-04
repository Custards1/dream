# Dream for VS Code

Dream support for VS Code: syntax highlighting from a TextMate grammar, and
everything else from [`lucid`](../../lucid/README.md), the language server --
which is the Dream compiler, answering the editor out of the tables it builds
to compile.

- **Diagnostics** as you type. They are the errors a build gives, in the same
  words, because they are the same errors.
- **Go to definition**, across files and packages.
- **Hover**: what a name resolved to, and which module it came from.
- **Outline** of what a file declares.
- **Completion**: a module's members after a `.`, and everything in scope
  elsewhere, in the order the compiler would find them.

The server reads the buffer rather than the file on disk, so all of this works
on code you have not saved.

## Installing

You need the VM on your `PATH` (or named in `dream.vm.path`) and a built
server image. From the Dream checkout:

```
just install                          # the VM, mind, dreams.dream, lucid.dream and std into ~/.mindv2
(cd editors/vscode && npm install)    # once, for vscode-languageclient
just vscode                           # copy this extension into ~/.vscode/extensions
```

Put `~/.mindv2/bin` on your `PATH` and `~/.mindv2` in `$MINDV2_PATH`.

## Finding the server

`lucid` is an image, not a native program, so the extension starts it as
`dream lucid.dream -L <roots>` and speaks LSP to it over stdin and stdout. It
takes the first `lucid.dream` it finds:

1. `dream.server.path`, when it is set;
2. `build/lucid.dream` (or `build-dream/bin/lucid.dream`) in a workspace
   folder -- so a workspace that *is* the Dream checkout runs the server it
   just built, not an installed one;
3. `$LUCID_IMAGE`;
4. `$MINDV2_PATH/lucid.dream`;
5. wherever the VM says the installation is (`dream --mindv2-path`), which
   finds it even when the editor was not started with `$MINDV2_PATH` set --
   as under the Nix flake's wrapped `dream`.

`~` and `$VARS` are expanded in both paths. **Dream: Restart Language Server**
picks up a rebuilt image. Rebuild it with `just lucid` after changing the
language: `just` alone does not, and new syntax shows as a parse error until
you do.

## Settings

| | |
|---|---|
| `dream.vm.path` | the VM. Default `dream`. |
| `dream.server.path` | the `lucid.dream` to run. Found as above when empty. |
| `dream.packagePaths` | extra package roots, passed as `-L`. Every workspace folder is always one. |
| `dream.stdlib.path` | the directory holding `std`. When empty, `$MIND_STDLIB`, and failing that the installation's (`$MINDV2_PATH` or `~/.mindv2`). |
| `dream.trace.server` | log the LSP conversation: `off`, `messages` or `verbose`. |

**If every file is suddenly red**, the server cannot see the standard library:
every `import std.x` is an error, which looks like a broken extension rather
than a missing setting. Set `dream.stdlib.path` (to `mind` in the checkout, or
`~/.mindv2` for an installation).

## Developing

```
npm install
npm test            # tokenize Dream with the grammar and check the scopes
just test-vscode    # the same, from the root
```

`test/grammar.js` is worth keeping honest. A TextMate grammar is a pile of
regular expressions with no compiler behind it, and the usual way to find out
one is wrong is to squint at colours. Three rules were wrong when the test was
first written: `let` ate its own declaration, every builtin lost to the
impure-name rule, and `%{` was read as a modulo. It is not part of `just test`,
because it needs `npm install` and nothing else in the suite needs anything
from outside the repository.
