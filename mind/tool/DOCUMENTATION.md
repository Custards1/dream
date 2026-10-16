# Documentation with Mind

Run `mind docs` from a project or one of its subdirectories. Open
`target/docs/index.html` in a browser, or upload the output directory to any
static host. The pages include guides, API reference, source links, a table
of contents and full-text search. Styles and search work offline; there are
no external fonts, scripts or services.

```sh
mind docs
mind docs -C path/to/project -o target/reference
mind docs --no-api
mind docs --compiler path/to/dreams.dream --example-timeout 2000
mind docs serve --watch      # build, serve at http://127.0.0.1:8000/, rebuild on change
```

Paths in `[docs]`, `--guides` and `--output` are relative to the project's
manifest. `-C` uses Mind's normal project discovery. A workspace includes API
pages from each listed member. Dependencies are available to examples, but
their API pages are not included automatically.

Every site also carries the standard library's reference, under `api/std/`,
read from the library Mind builds against. Its examples are not run again.
`std = false` under `[docs]` leaves it out, and the library's own site does
not include it twice.

```toml
[docs]
title = "My library"
guides = "docs"
output = "target/docs"
api = true
std = true               # the standard library's reference beside your own
example-timeout = 5000   # milliseconds per example, compile and run each
strict-examples = false  # true: an example that breaks its fence fails the build
```

## Names in code are links

An inline code span that names a module or a documented function becomes a
link to it, in guides and in `///` comments alike: `list.map`,
`std.http.server`, `http.server.static!`, or your own `config.load`. A name is
recognised whole (`std.list.map`), without its package (`http.server.static!`)
or by its module's last part (`server.static!`), the way code spells it after
an import. Where two places answer to one spelling equally well, neither is
linked. On a module's own page, its names link bare (`first_or`). Code
blocks, and code already inside a link, are left as they are.

Write guides as `.md` files under `docs/`. Subdirectories and assets keep
their relative paths, and local `.md` links become `.html` links. Supported
Markdown includes headings, paragraphs, fenced code, emphasis, links, images,
flat ordered/unordered lists, blockquotes and pipe tables. Raw HTML is escaped.
Heading links use lowercase words separated by hyphens; repeated headings
receive `-1`, `-2`, and so on. This is a deliberately small Markdown dialect,
not a complete CommonMark implementation.

Put `///` comments immediately before public top-level `let` or `type`
declarations. For a function with a separate type signature, put the comment
before its signature. Prose supports the same Markdown as guides. Private
functions, local bindings and declarations inside test blocks are excluded
from the API pages. Source is read without requiring the whole package to
compile. Signatures are taken from the source, not inferred by a type checker.

````text
/// Add a greeting to a name.
///
/// ```dream
/// import greeting;
/// import std.console;
///
/// let main! = console.print! (greeting.hello "Ada");
/// ```
let hello name = "Hello, " + name + "!";
````

Every fenced `dream` or `dr` block in a guide or API comment is an independent
program. Include its imports and `let main!`; examples do not share variables.
Mind compiles each with the package's normal dependencies and build settings,
then runs it in its own temporary directory. Code and actual captured output
appear together in the documentation, including the phase and exit status.
Standard output and standard error are both retained. Examples that fail to
compile, raise an error, exit unsuccessfully or time out **do not fail the
documentation build**. This lets comments demonstrate incorrect usage too:
fence such an example as ```` ```dream fails ````. An example whose outcome is
not the one its fence promises -- a plain `dream` example that fails, or one
marked `fails` that runs cleanly -- is listed with its page when the build
ends, and with `--strict-examples` (or `strict-examples = true` under
`[docs]`) the build fails instead.
A successful example that prints nothing is labeled “(no output)”.

Use a `dream no-run` fence for Dream that should be shown, and highlighted, but
not compiled: a few lines out of a larger program, a REPL session. Use a
`text` fence for anything else that should only be displayed. Each example
has a deadline, 5 seconds unless `example-timeout` says otherwise, which its
compile and its run each get in full. Displayed output is limited to 32 KiB per phase.
Examples run as ordinary local programs with the current user's access, so
building docs executes the example code. Temporary example directories are
removed after a successful documentation build.

The compiler follows Mind's usual `--compiler`, manifest, `DREAMS`, installed
compiler selection. `DREAM` selects the VM. Each generated site also includes
`search-index.json` for tooling.

The standard library's module functions, including private helpers, must have
`///` documentation. `python3 mind/tool/tests/doc_coverage.py` checks this rule;
it also runs as part of Mind's documentation tests. Local bindings and test
programs are outside that coverage check.

## Serving

`mind docs serve` builds the site and serves it at `http://127.0.0.1:8000/`.
With `--watch` it builds again whenever a guide, a module or `mind.toml`
changes, and with `--no-build` it serves what was built last. `mind serve DIR`
serves any directory the same way.

Both listen on 127.0.0.1, so only this machine can reach them, unless
`--host` names another address, and Mind warns when it does. They answer
only `GET` and `HEAD`, never serve dotfiles or follow symbolic links, send
large files a piece at a time, and answer `Range` and `If-None-Match` as
browsers expect. No one client may hold more than 32 connections, and the
server holds at most 256 in all. A connection idle for 15 seconds is closed.
HTTP/2 is off. `--port 0` picks a free port, `--list` lists a directory with
no `index.html`, and `-q` turns off the access log (`DREAM_LOG` sets its
level otherwise).
