# mind

The build tool for Dream — what Cargo is to rustc.

`mind` is itself written in Dream and compiled by `dreams`, like any other
program in this repository. It has no compiler of its own: Dream compiles whole
programs, so a build is *find the packages, hand them to `dreams`, run it*.
There is no object file, no link step, and nothing to cache.

```
just mind                 # build it to build/mind
dream build/mind help
```

## Commands

| | |
|-|-|
| `mind new <name>` | create a project in `./<name>` |
| `mind build` | compile to `target/<name>.dream` |
| `mind run [args]` | compile, then run with the arguments given |
| `mind test` | compile with `--test` and run the result |
| `mind check` | compile without writing an image |
| `mind add <source>` | add a dependency: a directory, a git URL, or a tarball |
| `mind remove <name>` | take one out again |
| `mind fetch` | download dependencies without building |
| `mind update [name]` | fetch git and url dependencies again |
| `mind deps` | list every package the project resolves to |
| `mind tree` | the same, as the tree of who needs what |
| `mind clean` | remove `target/` |

Every command but `new` looks for a `mind.toml` here or above, so they work
from anywhere inside a project — the way `git` finds its root.

## Dependencies

**Listing a dependency is the whole of using it.** Nothing goes in `[build]`
to make a dependency importable:

```toml
[package]
name = "app"
version = "0.1.0"

[dependencies]
util  = "../util"                                   # a directory
json  = "https://github.com/u/dream-json#v1.0"      # git, pinned after the #
http  = { git = "https://github.com/u/http", branch = "main" }
quiet = "https://example.com/quiet-1.0.tar.gz"      # a tarball

[dev-dependencies]                                  # only `mind test` sees these
bench = "../bench"
```

and then `import util.strings`, `import json.parse`. Or let `mind` write the
line:

```
mind add ../util
mind add https://github.com/u/dream-json --tag v1.0
mind add --dev ../bench
mind remove bench
```

What that buys, and each was something you used to have to do by hand:

- **The key is the import name.** `json = ..` is imported as `json`, whatever
  the package's own `[package] name` says. The compiler records the key as a
  second name for the package (`-L json=DIR`), so a package whose repository
  calls it `dream-json` or `jsonlib` is still `json` to you.
- **A bare string is a source.** A path, or a URL: `url#tag`, `url#branch` and
  `url#<commit>` are git (a 7-40 digit hex ref is a commit), a `.tar.gz` is a
  tarball. The inline-table form says the same thing longhand.
- **A dependency needs no manifest.** A directory of `.dr` files -- local,
  cloned or unpacked -- is a package named by its key. Its modules are in
  `src/` when there is one, as for any package.
- **What a dependency needs travels with it.** Its own `[build] includes`
  and `host-modules` are added to every build that uses it. Its `defines` are
  not: a `when` flag is a decision about the whole program.
- **Transitive dependencies are found.** Every dependency's own
  `[dependencies]` is followed; a package reached twice (a diamond) is
  compiled once.
- **Conflicts are reported, not guessed at.** A program has one namespace of
  packages, so two *different* directories under one name cannot both be
  built. `mind` says which two and who asked for each.

The details:

- **`path`** is resolved relative to the manifest that named it, so moving a
  project does not change what its dependencies mean. `mind add` takes a path
  relative to where you are and writes it relative to the manifest.
- **`git`** must be pinned with `tag`, `rev` or `branch`. An unpinned
  dependency is refused: "whatever is on the default branch today" is not a
  dependency, it is a moving target, and the build that breaks next week will
  not say why.
- **`url`** is a `.tar.gz` whose single top-level directory holds the package.

Fetched packages go in `$MIND_HOME/cache` (default `~/.mind/cache`), keyed by
URL and revision, and are shared between projects — a repository at a given
revision is the same bytes whoever asked for it. A cached dependency is never
re-fetched; `mind update` (or `mind update NAME`) throws the cached copies away
and fetches them again, which is how a branch-pinned dependency moves.

Fetching shells out to `git`, `curl` and `tar` rather than speaking those
protocols. They are already installed, already know about proxies and
credentials and certificate stores, and none of that is a build tool's
business. It is also why `mind` needs no TLS in the VM.

## Environment

| | |
|-|-|
| `DREAMS` | the compiler to call (default: `dreams.dream` from `$MINDV2_PATH`) |
| `DREAM` | the VM to run images with (default: `dream`) |
| `MIND_STDLIB` | where the standard library lives (default: `mind`) |
| `MIND_HOME` | where fetched packages are cached (default: `~/.mind`) |

## Layout

| | |
|-|-|
| `main.dr` | the commands, and argument parsing |
| `manifest.dr` | reading `mind.toml`, and the line edits `add` and `remove` make |
| `fetch.dr` | resolving a dependency to a directory, fetching if needed |
| `build.dr` | the dependency graph, and calling the compiler |
| `util.dr` | paths and files |

Run its own tests with `just test-mind`.
