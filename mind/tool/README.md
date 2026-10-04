# mind, the Dream build tool

`mind` is to `dreams` what Cargo is to `rustc`: it reads a manifest, finds and
fetches the packages a project needs, runs their build scripts, and hands the
lot to the compiler. It is written in Dream and compiled by `dreams`, like any
other program here.

There is no object file and no link step. Dream compiles whole programs, so a
build is *find the packages, hand them to the compiler, run it*. What is kept
between builds is what the compiler keeps of each module (its parse, walk,
lowering and type check) and what build scripts make.

```
just mind                       # build/mind, from this checkout
just install                    # and into ~/.mindv2/bin, with everything it needs
```

## A project

```
mind new hello                  # hello/mind.toml and hello/main.dr
cd hello
mind run                        # build target/debug/hello.dream and run it
mind add ../util                # depend on a directory
mind test                       # every test, at once
```

```toml
# mind.toml
[package]
name = "hello"
version = "0.1.0"

[dependencies]
util = "../util"
```

Every command but `new` looks for `mind.toml` here or above, so they work from
anywhere inside a project, the way `git` finds its root. `-C DIR` names
another.

## Commands

| | |
|---|---|
| `mind new NAME` | create a project in `./NAME` |
| `mind build` | compile to `target/<profile>/<name>.dream` |
| `mind build GOAL..` | make what the build script names instead ([Goals](#goals)) |
| `mind run [-- args]` | compile, then run with the arguments given |
| `mind test [NAME..]` | run every test at once: each package's units and its build script's checks |
| `mind check` | compile without writing an image |
| `mind repl` | an interactive session with this project's packages on the path |
| `mind add SOURCE` | add a dependency: a directory, a git URL, or a tarball |
| `mind remove NAME` | take one out again |
| `mind fetch` | download dependencies without building |
| `mind update [NAME]` | fetch git and URL dependencies again |
| `mind deps`, `mind tree` | every package the project resolves to, as a list or as who needs what |
| `mind info` | what this build would run, without running it |
| `mind options` | every package's options, and what this build sets them to |
| `mind clean` | remove `target/` |

`mind help` lists the flags. The ones a build takes are the compiler's
(`-L`, `-D`, `--target`, `--release`), plus `--profile`, `--compiler`,
`-j` and `-f ARG` to pass anything else through as written.

## Dependencies

**Listing a dependency is the whole of using it.** Nothing else goes in the
manifest to make one importable:

```toml
[dependencies]
util  = "../util"                                   # a directory
json  = "https://github.com/u/dream-json#v1.0"      # git, pinned after the #
http  = { git = "https://github.com/u/http", branch = "main" }
quiet = "https://example.com/quiet-1.0.tar.gz"      # a tarball

[dev-dependencies]                                  # only `mind test` sees these
bench = "../bench"
```

then `import util.strings`, `import json.parse`. Or let `mind` write the line,
which it does without disturbing the rest of the file:

```
mind add ../util
mind add https://github.com/u/dream-json --tag v1.0
mind add --dev ../bench
mind remove bench
```

- **The key is the import name.** `json = ..` is imported as `json`, whatever
  the package calls itself, so a repository named `dream-json` is still `json`
  to you. (`-L json=DIR` is how the compiler is told.)
- **A bare string is a source.** A path, or a URL: `url#tag`, `url#branch` and
  `url#<commit>` are git (7 to 40 hex digits is a commit), a `.tar.gz` is a
  tarball. The table form says the same thing longhand.
- **A package needs no manifest.** A directory of `.dr` files is a package
  named by its key; its modules are in `src/` when there is one.
- **Paths are relative to the manifest that wrote them**, so moving a project
  does not change what its dependencies mean.
- **Git must be pinned** with `tag`, `rev` or `branch`. "Whatever is on the
  default branch today" is a moving target, and the build it breaks next week
  will not say why.
- **What a dependency needs travels with it**: its `[build] includes` and
  `host-modules`, and its options (below) -- but not its program-wide
  `defines`, which are the root's to decide.
- **Transitive dependencies are followed**, and a package reached twice is
  compiled once.
- **Conflicts are reported, not guessed at.** A program has one namespace of
  packages, so two different directories under one name cannot both be
  built; `mind` says which two, and who asked for each.

### Versions

There is no registry, so there is nothing to choose between: a program has one
package under each key. A requirement is checked against the `version` the
fetched package declares:

```toml
json = { git = "https://github.com/u/dream-json", tag = "v1.4.0", version = "^1.2" }
util = { path = "../util", version = ">=0.3, <0.5" }
```

`^1.2` is compatible with 1.2 (below 1.0 the minor is the breaking number),
`~1.2.3` takes patch releases, `= < <= > >=` compare, `,` joins, `*` is
anything, and a bare `1.2` means `^1.2`. A mismatch lists every requirement
with where it was written, and whether any version could have met them all.
`std.version` is the grammar.

### `mind.lock`

What the graph resolved to -- every package, its source, its version, and for
git the commit checked out -- is written to `mind.lock`. Commit it. A build
that finds a different commit behind the same source (a tag moved, a cache
edited) stops and says so; `mind update` fetches again and accepts what it
finds. Editing a dependency in the manifest is not drift; the lock follows.

Fetched packages are cached in `$MIND_HOME/cache` by URL and revision and
shared between projects. Fetching shells out to `git`, `curl` and `tar`,
which already know about proxies, credentials and certificate stores.

### Cycles

Every kind of dependency means "must be ready first", so a loop cannot be
built -- including one through a build script. It is reported as the whole
loop, each edge with the manifest that made it:

```
mind: these packages need each other, so none of them can be built first:
  app      uses         sqlite   (mind.toml)
  sqlite   builds with  codegen  (../sqlite/mind.toml)
  codegen  uses         app      (../codegen/mind.toml)
```

## Build scripts

A package may carry a `build.dr` beside its manifest, for what it needs made
before it compiles and cannot write by hand: a generated module, a file to
embed, a C library, a switch that depends on the machine. **It is optional**;
a package without one builds exactly as if scripts did not exist.

A script is a pure function from a context to a plan, written with
`std.build`:

```dream
import std.build;
import std.build.command;

let plan ctx =
    build.empty
    |> build.module "version" (build.write "version.dr" ("let v = \"" + build.version ctx + "\";"))
    |> build.payload "table" (command.make "table" "gen-table" [command.file "table.csv", command.out "t.bin"])
    |> build.define "generated";
```

`build.module` makes `<package>.version` importable; `build.payload` embeds a
file in the image; `build.define` sets a `when` flag for this package alone.
`std.build.cc` compiles C and C++ with GCC, Clang, MSVC or through CMake from
one description.

Every step is cached by what it was given and the contents of what it reads,
in one cache shared by every project on the machine, so a build where nothing
changed runs no script and starts no VM -- it costs a `stat` per input. Steps
run `-j` at once.

A script sees only the environment variables its manifest lists, and is
compiled against `[build-dependencies]`, a namespace of its own:

```toml
[build]
env = ["CC", "PKG_CONFIG_PATH"]
script = "tools/build.dr"        # when it is not build.dr

[build-dependencies]
codegen = "../codegen"
```

[docs/build.md](../../docs/build.md) is the whole design.

### Goals

A plan can name things to make on request -- `build.goal name goal` -- and
`mind build NAME` makes them instead of the program. The repository's own
`build.dr` is the example: `mind build vm` builds the VM with `std.build.cc`,
and `mind build default` the VM, the compiler and `mind`.

## Tests

`mind test` runs every test a project has, at once: each package's **units**
(its `when test` blocks, compiled with `--test` and run) and the **checks** its
build script declares. A test is named for its package, and a name narrows:

```
mind test                       # everything
mind test dreams                # one package's
mind test dreams.bootstrap      # one check
```

A check is a step that passes when it succeeds, run only by `mind test` and
every time:

```dream
let plan ctx =
    build.empty
    |> build.check "golden" (command.check ctx "golden" (command.toolchain ctx) "tests/golden.sh" []);
```

`command.toolchain ctx` hands the check the VM and compiler this build uses,
as `DREAM` and `DREAMS`. A library with no program names the file its tests
are gathered from: `[test] entry = "all.dr"`.

A **workspace** tests several packages as one project. The repository's own
`mind.toml` is one:

```toml
[workspace]
name = "dream"
members = ["dreams", "lucid", "mind/std", "mind/tool"]

[build]
compiler = "build/dreams.dream"
```

## Options and profiles

A package declares what it can be built as. Each option has a type -- `bool`,
`integer`, `string`, or a list of choices -- and a default:

```toml
[options]
vendored = { type = "bool", default = false, doc = "build sqlite from source" }
threads  = { type = ["off", "single", "multi"], default = "multi" }
cache_mb = 64
```

`when vendored` reads it inside the package and `when sqlite.vendored` from
anywhere else. A setting is decided by, the later winning: the default; the
package's `[build] defines`; `[config.KEY]` in any manifest that depends on
it; the profile's `[profile.NAME.config.KEY]`; and `-D KEY:name=value`. Two
dependencies that configure a third two ways are a conflict for the root to
settle. A name or value the package does not declare is an error that lists
what it accepts, never a setting quietly ignored.

A dependency can hang on an option -- `sqlite_sys = { path = "..", when = "not vendored" }`
-- in the compiler's `when` grammar. An edge whose condition is false is not
fetched, not built and not a conflict.

A **profile** is a named way to build the whole program:

```toml
[profile.release]
defines = ["release"]
flags   = ["--no-opt"]
target  = "linux"

[profile.release.config.sqlite]
threads = "multi"
```

`--profile NAME` picks one, `--release` is `--profile release`, `debug` is
the default, and only the root's profiles count. The image goes to
`target/<profile>/<name>.dream`, with the platform first when a target is
stated (`target/linux/release/`), so no build overwrites another's image.

## Finding the compiler and the library

The compiler is `--compiler`, then `[build] compiler`, then `$DREAMS`, then
`dreams.dream` from the installation (`$MINDV2_PATH`). A name ending in
`.dream` is run by the VM (`$DREAM`, default `dream`); anything else is run
directly.

The standard library is `$MIND_STDLIB`, then `$MIND_HOME/std`, then a `mind/`
directory holding `std` at or above the working directory -- which is what
makes this repository work with no configuration -- and last the
installation's copy: the first directory of `$MINDV2_PATH` (or `~/.mindv2`)
holding `std`, which is where `just install` puts it. A checkout comes before
the installation, so an installed library never shadows the one being worked
on.

## Environment

| | |
|---|---|
| `DREAMS` | the compiler |
| `DREAM` | the VM that runs images (default `dream`) |
| `MINDV2_PATH` | the installation: where `dreams.dream` is found |
| `MIND_STDLIB` | the directory holding `std` |
| `MIND_HOME` | fetched packages, the build cache and the unit cache (default `~/.mind`) |
| `MIND_BUILD_CACHE` | where build steps are cached (default `$MIND_HOME/build`) |
| `MIND_UNITS` | where the compiler keeps each module's work between builds (default `$MIND_HOME/units`; `off` for none). Entries unused for 30 days are removed |

## Layout

| | |
|---|---|
| `main.dr` | the commands and their arguments |
| `manifest.dr` | reading `mind.toml`, and the line edits `add` and `remove` make |
| `fetch.dr` | a dependency to a directory, fetching when needed |
| `build.dr` | the dependency graph, options, and calling the compiler |
| `script.dr` | running build scripts and folding what they make into the compile |
| `lock.dr` | `mind.lock`, and what counts as drift |
| `config.dr`, `util.dr` | settings, paths and files |

```
just test-mind          # the units, and the graph, options and script suites in tests/
mind test mind          # the same
```
