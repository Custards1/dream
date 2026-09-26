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
| `mind options` | every package's options, and what this build sets them to |
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
  not, except for the options it declares: a program-wide `when` flag is a
  decision about the whole program, and a package's options are how it is
  configured instead (below).
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

### Versions

A dependency may say which versions it accepts, in the long form:

```toml
[dependencies]
json = { git = "https://github.com/u/dream-json", tag = "v1.4.0", version = "^1.2" }
util = { path = "../util", version = ">=0.3, <0.5" }
```

`^1.2` is compatible with 1.2 (below 2.0; below 1.0 the minor is the
breaking number, so `^0.3` is below 0.4), `~1.2.3` takes patch releases,
`=`, `<`, `<=`, `>`, `>=` compare, `,` joins, `*` is anything, and a bare
`1.2` means `^1.2`. `std.version` is the grammar, for a build script too.

There is no registry, so there is nothing to choose between: a program has
one package under each key, and `mind` checks that the version it fetched --
its `[package] version` -- is one every requirement on it accepts. When one
is not, it says what was found and lists every requirement with where it was
written, and whether any version could have met them all. A directory with
no manifest has no version, so a requirement on one is an error.

### Build dependencies

`[build-dependencies]` are what a package's build script is compiled with
(docs/build.md). The script is a program of its own, so its packages are a
namespace of their own: a tool may use `json` 1.x while the program uses 2.x.
`mind deps` marks them `[build: ...]` and `mind tree` hangs them under
`[build]`.

### Cycles

Every kind of dependency means "must be ready first", so a loop cannot be
built, including one through a build script (a package whose script needs a
tool that uses the package). It is reported as the whole loop, each edge with
the manifest that made it:

```
mind: these packages need each other, so none of them can be built first:
  app      uses         sqlite   (mind.toml)
  sqlite   builds with  codegen  (../sqlite/mind.toml)
  codegen  uses         app      (../codegen/mind.toml)
```

### `mind.lock`

What the graph resolved to is written to `mind.lock` beside the manifest:
every package of the program and of each build script, with its source, its
version, and for git the commit that was checked out. Commit it. A build
that resolves a package to a different commit from the same source -- a tag
moved, a cache edited -- stops and says so; `mind update` fetches again and
accepts what it finds. Changing a dependency in the manifest is not drift,
and the lock simply follows. The lock is only rewritten when it changes.

Fetched packages go in `$MIND_HOME/cache` (default `~/.mind/cache`), keyed by
URL and revision, and are shared between projects — a repository at a given
revision is the same bytes whoever asked for it. A cached dependency is never
re-fetched; `mind update` (or `mind update NAME`) throws the cached copies away
and fetches them again, which is how a branch-pinned dependency moves.

Fetching shells out to `git`, `curl` and `tar` rather than speaking those
protocols. They are already installed, already know about proxies and
credentials and certificate stores, and none of that is a build tool's
business. It is also why `mind` needs no TLS in the VM.

## Options

A package declares what it can be built as. Each option has a type -- `bool`,
`integer`, `string`, or a list of choices -- and a default; a bare value
declares its own type:

```toml
[options]
vendored = { type = "bool", default = false, doc = "build sqlite from source" }
threads  = { type = ["off", "single", "multi"], default = "multi" }
cache_mb = 64
```

`when vendored` in that package's code reads the option, and `when
sqlite.vendored` reads it from anywhere else. An option is set, a later place
winning:

1. its default;
2. the package's own `[build] defines` (`defines = ["vendored"]`);
3. `[config.KEY]` in any manifest that depends on it, however deep;
4. the profile's `[profile.NAME.config.KEY]`;
5. `mind build -D KEY:name=value`.

```toml
[config.sqlite]
threads = "off"
```

The root's `[config.KEY]` wins over its dependencies'. Two dependencies that
configure a third two ways are a conflict, and `mind` names both; the root
settles it by saying which. Every name and value is checked against the
declaration, so a typo is an error that lists what the package accepts, not a
setting that is quietly ignored. The root declares options the same way, and
`-D name` sets one of them rather than defining a program-wide flag.
`mind options` shows every package's options and where this build left them.

A dependency may hang on an option of the package that lists it:

```toml
[dependencies]
sqlite_sys = { path = "../sqlite-sys", when = "not vendored" }
```

The condition is the compiler's grammar (`not`, `&&`, `||`, `==`, `!=`,
parentheses, dotted names), read against the package's settled options and
the program's settings (`os`, `family`, `test`, the root's defines). An edge
whose condition is false is not in the graph: not fetched, not built, and not
a conflict.

### Profiles

A profile is a named way to build the whole program:

```toml
[profile.release]
defines = ["release"]
flags   = ["--no-types"]    # compiler flags
target  = "linux"           # --target

[profile.release.config.sqlite]
threads = "multi"
```

`--profile NAME` picks one, `--release` is `--profile release`, and `debug` is
the default. `debug` and `release` exist whether declared or not; an
undeclared `release` defines `release`, as `--release` always has. A
dependency's profiles are ignored: how the program is built is the root's
decision. `[build] target` is the target when neither `--target` nor the
profile says.

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
| `lock.dr` | `mind.lock`: writing it, reading it, and what counts as drift |
| `util.dr` | paths and files |

Run its own tests with `just test-mind`.
