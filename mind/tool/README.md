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
| `mind fetch` | download dependencies without building |
| `mind deps` | show what the project resolves to |
| `mind clean` | remove `target/` |

Every command but `new` looks for a `mind.toml` here or above, so they work
from anywhere inside a project — the way `git` finds its root.

## Dependencies

There is **no index and no registry**. A dependency says where it is, and
`mind` puts a copy where the compiler can find it. That is Go's model rather
than Cargo's: no central name to squat, nothing to publish to, and a URL that
means the same thing to everyone who reads it.

```toml
[package]
name = "app"
version = "0.1.0"
src = "."

[dependencies]
util  = { path = "../util" }
shout = { git = "https://github.com/u/shout", tag = "v1.0.0" }
quiet = { url = "https://example.com/quiet-1.0.tar.gz" }
```

- **`path`** is resolved relative to the manifest that named it, so moving a
  project does not change what its dependencies mean.
- **`git`** must be pinned with `tag`, `rev` or `branch`. An unpinned
  dependency is refused: "whatever is on the default branch today" is not a
  dependency, it is a moving target, and the build that breaks next week will
  not say why.
- **`url`** is a `.tar.gz` whose single top-level directory holds the package.

Fetched packages go in `$MIND_HOME/cache` (default `~/.mind/cache`), keyed by
URL and revision, and are shared between projects — a repository at a given
revision is the same bytes whoever asked for it. A cached dependency is never
re-fetched; delete the directory to change that.

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
| `manifest.dr` | reading `mind.toml` |
| `fetch.dr` | resolving a dependency to a directory, fetching if needed |
| `build.dr` | the package graph, and calling the compiler |
| `util.dr` | paths and files |

Run its own tests with `just test-mind`.
