# Build scripts and `std.build`

A package may carry a `build.dr`: a program `mind` runs before it compiles
the package, to make what the package needs and cannot write by hand. That
covers a C library to embed, a module generated from a grammar or a schema, or
a switch that depends on what the machine has installed. `std.build` is the
vocabulary a script is written in. Tools such as a C compiler, a parser
generator or `protoc` add to that vocabulary through one behavior, so each of
them extends the same build rather than bringing its own.

This document is the design. The image's target and the package graph are
built; the last section is the order to build the rest in and what is still
open.

## What it has to be

Four constraints decided most of what follows.

- **Dream's idioms, not a DSL.** A build is described with values, `|>`,
  records, unions and `derive`. Effects stay behind `!` names, as they do
  everywhere else. There is nothing a build script can say that a Dream
  program cannot.
- **Tools are extensions, not special cases.** `std.build` knows nothing
  about C. A tool is a module that derives `std.build.tool`, and it can live
  in `std`, in a package on the internet, or in the project itself. Whatever
  is true of `cc` has to be true of the tool someone writes next year.
- **Massive graphs.** `mind` will build packages that reach hundreds of
  others, some of them with scripts. A build where nothing changed must cost
  a `stat` per input: no script runs and no script is compiled. Work that
  does not depend on other work runs at the same time, and the same step
  asked for by two packages is done once.
- **An image says where it can run.** An image that carries a `.so` runs on
  Linux and nowhere else, and today nothing records that. The image now
  records its target, the VM checks it, and each layer can configure it.

## The shape of a script

A script is a **pure function from a context to a plan**:

```dream
// build.dr, beside mind.toml
import std.build;
import std.build.cc;

let sqlite : build.Context -> build.Artifact;
let sqlite ctx =
    cc.library "sqlite3" ["vendor/sqlite3.c"]
    |> cc.flags ["-O2", "-DSQLITE_THREADSAFE=0"]
    |> cc.shared ctx;

let plan : build.Context -> build.Plan;
let plan ctx =
    build.empty
    |> build.payload "sqlite" (sqlite ctx)
    |> build.module "version" (build.write "version.dr"
                                  ("let version = \"" + build.version ctx + "\";"));
```

There is no `main!`. The script says what the package needs, and `mind`
works out how, when and whether to make it. That split is what makes the
build cheap:

- A plan is data, and so it can be **compared**. Every step has a key worked
  out from what it was given, and a step whose key has not changed is not
  run.
- A plan is data, and so it can be **scheduled**. The graph is known before
  anything runs, and independent steps run on different processes.
- A plan is **lazy**. A payload the image does not end up using is never
  built, and neither is a branch of a `match` that did not match.
- A script is **importable**. `mind` compiles a small driver that imports
  the script and calls `build.run! script.plan`. Because the script has no
  entry point of its own, one driver can later import every package's script
  at once, as one program with one scheduler, which is the scaling item
  below.

Effects happen in exactly two places: inside a step's action, which the
runner calls, and in `build.given`, which feeds the result of a step back into
the plan (see "Decisions that need the machine").

## The vocabulary

`std.build` is `mind/std/build/mod.dr`, and the tools that ship with Dream
are the other files in `mind/std/build/`.

### Context

What a script is told, and all it may know about the world outside its own
files:

```dream
mapping Context {
    package             // :string, the key it is built under
    version             // :string, from its manifest
    root                // :string, the package's directory
    target : Target     // where the image will run
    host : Target       // where this build is running; differs when cross-building
    profile = :debug    // :debug | :release
    env = %{}           // only the variables the manifest names in [build] env
    options = %{}       // the package's options, settled ("Options")
    dependencies = %{}  // key => version, for everything it uses
}

mapping Target {
    os                  // :linux | :macos | :windows
    arch                // :x86_64 | :aarch64
}
```

`env` holds only what the manifest lists (`[build] env = ["CC",
"PKG_CONFIG_PATH"]`). That is what lets the runner decide whether a script's
answer can be reused: an environment variable the script cannot see cannot
change its plan. A script that reads `$CC` without saying so would be right
the first time and stale ever after, so it is not given the chance.

`build.shared_name ctx "sqlite3"` is `libsqlite3.so`, `libsqlite3.dylib` or
`sqlite3.dll` for the target, with `build.exe_name` and `build.object_name`
alongside it. Every tool needs these, so they are written once, here.

### Inputs and artifacts

```dream
union Input {
    file(path : :string)                    // in the package, relative to its root
    artifact(step : Step, name : :string)   // something a step makes
}
```

A bare string is accepted wherever an `Input` is and means `file`. An
artifact **holds the step that makes it**. That one decision is what makes
the graph: a step's inputs name the steps they come from, so the whole graph
is whatever is reachable from the plan. There is no registry and no global
state, the script stays pure, and an artifact that nothing reaches is never
built.

### Steps

```dream
mapping Step {
    tool                // :string, "cc"
    name                // :string, what this one makes, for people and logs
    inputs = []         // [Input]: their contents are part of the key
    config = ()         // data: everything else that changes the output
    outputs = []        // [:string]: files it makes in its own directory
    action              // Job -> result.Of :unit :string, called by the runner
}
```

A step's **key** is a digest of `tool`, `config` and its inputs, where a
file contributes the digest of its contents and an artifact contributes its
step's key. The name is not in the key, so two packages that ask for the same
thing by different names share the work. Keys are what the cache is indexed
by and what `build.given` waits on.

A step writes only into **its own directory**, which the runner makes, and
declares what it will write there. It never picks an output path. Two steps
therefore cannot collide, a half-finished step leaves nothing where a
finished one is expected (it runs in a scratch directory that is renamed into
place), and one cache can be shared by concurrent builds.

### Jobs

What an action receives:

```dream
mapping Job {
    dir                 // this step's directory
    ctx : Context
    paths = %{}         // every input, resolved to a path on disk
}
```

with `build.path job input`, `build.out job "name"`, `build.exec! job program
args`, which runs the program in `dir` and answers `[:error, output]` when it
fails, and `build.log! job text`. An action is the only code in a build that
touches the file system, and it touches only what it was handed.

### Plans

A plan is what a package contributes to the compile, and every contribution
takes the plan last so that they chain with `|>`:

```dream
mapping Plan {
    payloads = %{}      // name => Input, each becomes --payload NAME=PATH
    modules = []        // [Input], directories of generated modules
    defines = %{}       // name => value, `true` for a bare flag
    hosts = []          // host modules the package needs
    target = ()         // () lets the compiler decide; else [Target]
    steps = []          // made for their own sake, e.g. a file beside the image
    later = []          // build.given, below
}
```

| | |
|-|-|
| `build.empty` | contributes nothing |
| `build.payload name input` | embed a file in the image |
| `build.module name input` | a generated module, imported as `<package>.<name>` |
| `build.define name` / `build.set name value` | a `when` flag or setting |
| `build.host_module name` | a host module the package needs |
| `build.target targets` | where the result can run |
| `build.also step` | make it whether or not anything uses it |
| `build.all [plans]` | several plans, merged |

Merging is a union. Two payloads or two modules under one name are reported
with both origins, as a dependency conflict is, rather than one silently
winning.

`build.write name text` is a step that writes a string, keyed on the string.
Its text is pure and lazy, so generating a module is ordinary Dream code with
no template language.

### Decisions that need the machine

Some plans cannot be written until something has been asked of the machine.
Is `libsqlite3` installed? What does `pkg-config --cflags` say? These are
steps like any other, whose output is small and whose **answer changes the
plan**:

```dream
let plan ctx =
    build.given (probe.library ctx "sqlite3") (fn found -> match found {
        "" => build.empty |> build.payload "sqlite" (sqlite ctx) |> build.define "vendored",
        _  => build.empty,
    });
```

`build.given input f` waits for the input to be built, reads it as a string,
and merges `f`'s plan in, which may itself contain further `given`s. `f` is
pure, so the effect is still only in the step. The runner resolves `given`s
in waves, and a plan with none is a single graph known up front, which is the
common case and the fast one.

## Tools

A tool is a module that derives the behavior `std.build.tool`, which has four
holes to fill and gives back the step construction every tool would
otherwise write for itself:

```dream
// mind/std/build/tool.dr
virtual let name cfg;                   // "cc"
virtual let inputs cfg;                 // [Input]
virtual let outputs ctx cfg;            // [:string]; may depend on the target
virtual let run! job cfg;               // make them
virtual let version cfg = "1";          // change it to invalidate every step made before

/// The step this configuration describes. The key covers the tool's
/// version, the target and the whole configuration, so changing a flag
/// rebuilds and changing nothing does not.
let step ctx cfg = ..;

/// Its first output, as an artifact: what most callers want.
let artifact ctx cfg = build.artifact (step ctx cfg) (list.head (outputs ctx cfg));
```

A tool's configuration is its own record, built with its own `|>`
modifiers, and handed to `step` or `artifact` at the end. The C tool:

```dream
// mind/std/build/cc.dr
import std.build;
derive std.build.tool;

mapping Config { kind = :shared, name, sources = [], flags = [], defines = [], includes = [] }

let library name sources = Config.make :shared name sources [] [] [];
let flags fs c = Config.set_flags c (list.append (Config.flags c) fs);
let shared ctx c = artifact ctx c;

let name c = "cc";
let inputs c = Config.sources c;
let outputs ctx c = [build.shared_name ctx (Config.name c)];
let run! job c = build.exec! job (compiler job) (arguments job c);
```

That is all a tool is. It is why `cc` does not have to be in `std` to be as
good as one that is, and why the `derive` contract is checked: a tool that
forgets `outputs` is a compile error in the tool, not a mystery in a build.

What ships in `std.build`, because almost every build with native parts
needs it:

| | |
|-|-|
| `std.build.cc` | C and C++ to shared libraries and objects; `$CC`, `cl.exe` on Windows |
| `std.build.command` | any program: arguments are strings, inputs and `command.out "name"` |
| `std.build.probe` | ask the machine: a library, `pkg-config`, a program on the path |
| `std.build.fetch` | a URL pinned by digest; for vendored sources too large to check in |

`std.build.command` is the escape hatch that keeps the rest honest:

```dream
let parser = command.make "peg" "peg-gen" ["grammar.peg", "-o", command.out "parser.dr"];
build.module "parser" parser
```

Its arguments are a list whose inputs and outputs are marked, so the step's
inputs, outputs and key all come from the one list the program is run with.

Tools from packages are listed under `[build-dependencies]` and are compiled
into the script and not into the program. A tool is only code the script
imports, so `mind` treats it as a dependency like any other.

## Running a plan

`build.run! plan` is the driver's `main!`. It:

1. Reads the context `mind` wrote (`--context FILE`), or makes one for the
   host when it is run by hand. Running a script by hand is how it is
   debugged.
2. Evaluates `plan ctx`, collects every step reachable from it, and takes
   each once by key.
3. Runs the graph. A step whose inputs are ready runs in a process of its
   own, up to `ctx.jobs` at once. As the note on `spawn!` in CLAUDE.md warns,
   the thunk handed to `spawn!` is made in a small function so that it does
   not carry the runner's frame.
4. Skips a step whose directory already holds a record of its key. It runs
   anything else in a scratch directory, then renames that into place.
5. Resolves `given`s once their inputs are built, and goes back to 2 with
   the merged plan.
6. Writes the **outcome**: what the plan contributes, with every input
   resolved to a path, and every file that was read along with its stamp.

**File digests are cached behind a stamp**, which is git's index trick. A
file's `(size, modified)` is kept beside its digest, and the contents are
read only when the stamp has moved. Digesting in Dream would be the most
expensive thing a no-op build does, so the VM does it: this needs
`io.stat!`, `io.digest!` for a file and a pure `digest` for a string. None of
them exist yet.

## What `mind` does with it

For every package in the graph that has a `build.dr`, dependencies before
the packages that use them:

1. **Skip if nothing moved.** `target/build/<key>/outcome.json` records the
   script image's stamp, the context, the manifest's `env` values, and the
   stamp of every input the last run read. If none of them has changed, the
   outcome is reused and nothing is compiled or run. This is the no-op path,
   and it costs a `stat` per input.
2. **Compile the driver** if the script, or anything it imports, has
   changed. It is compiled with `std` and the package's
   `[build-dependencies]`, never its ordinary dependencies. The image goes to
   `target/build/<key>/script.dream`.
3. **Run it** with a context, a job count, and a timeout, since a script is a
   program and may diverge.
4. **Fold the outcome into the compile**:

   | Outcome | Compiler |
   |---|---|
   | payloads | `--payload NAME=PATH` |
   | modules | `-L KEY+=DIR`, an overlay: generated modules become the package's own |
   | defines | `-D KEY:name=value`, scoped to the package ("Options") |
   | hosts | `--host-module`, as a manifest's are |
   | target | intersected across packages, then `--target` |

Packages that do not depend on each other run their scripts at the same time.
Outputs live under the root project's `target/`, never in a dependency's
directory, because a fetched package is shared by every project that uses it
and is treated as read-only. `MIND_BUILD_CACHE` points every project at one
step cache. That is safe because a step's directory is named by its key and
filled atomically.

The manifest gains:

```toml
[build]
script = "build.dr"              # the default, when the file exists
env = ["CC", "PKG_CONFIG_PATH"]  # what the script may see
target = "any"                   # or "linux", or ["linux", "macos"]; see below

[build-dependencies]
protoc = "https://github.com/u/dream-protoc#v2"

[options]                        # what this package can be built as
vendored = { type = "bool", default = false }

[config.sqlite]                  # a dependency's options, however deep
threads = "off"

[profile.release]
defines = ["release"]
```

and the command line gains `--target`, `--profile`, `-D KEY:name=value` and
`-j N`. The next two sections are the graph and the options.

## The package graph

`mind` resolves one graph before anything is compiled or run, and every
question below is answered from it. Nothing is fetched twice, nothing is
built out of order, and anything that cannot be built is reported with the
chain of packages that led to it.

### Three kinds of edge

| Section | Edge | Compiled into |
|---|---|---|
| `[dependencies]` | P **uses** D | the program |
| `[dev-dependencies]` | P **tests with** D, for the root only | the test program |
| `[build-dependencies]` | P **builds with** T | P's build script |

Build dependencies are a **separate program**, so they are a separate
namespace. A tool may use `json 1.x` while the program uses `json 2.x`, and
neither sees the other, because they are never compiled together. A build
dependency's own `[dependencies]` come with it into the script, and its
`[build-dependencies]` into its own script, recursively. Every build script,
and the program, is a set of packages drawn from **one** graph whose nodes
are `(key, source, version)`. So a package that both sides need is fetched
once, and its script, if it has one, is run once.

All three edges mean "must be ready first". Before P's code compiles, every D
it uses has had its script run. Before P's script compiles, every T it builds
with is ready in the same sense. That is the order `mind` builds in, and it
is why cycles matter.

### Cycles

A cycle among these edges cannot be built. A package whose script needs a
tool that uses the package would need its own output in order to produce it.
The walk keeps the path it is on, and an edge back into that path is
reported with the whole loop and the kind of each edge:

```
mind: these packages need each other, so none of them can be built first:
  app   uses          sqlite   (app/mind.toml)
  sqlite builds with  codegen  (vendor/sqlite/mind.toml)
  codegen uses        app      (tools/codegen/mind.toml)
```

A cycle made only of `uses` edges, between packages with no build scripts,
is the one that could be compiled, since a program is compiled whole. It is
still refused. A package graph with a loop is two packages that are really
one, and allowing it would make "dependencies before the packages that use
them" mean nothing for scripts, options and targets. Module imports within
and across packages are unaffected, because this is about manifests.

The walk tells the two apart by where the key is. A key on the path the
walk is still descending is a cycle, and a key finished earlier is a diamond.
The path is kept by directory rather than by key, because it runs through
every namespace (`mind/tool/build.dr`).

### Versions

A dependency may say which versions it accepts, beside where the package is:

```toml
[dependencies]
json = { git = "https://github.com/u/dream-json", tag = "v1.4.0", version = "^1.2" }
util = { path = "../util", version = ">=0.3, <0.5" }
```

The requirement grammar is the usual one: `^1.2`, `~1.2.3`, `=1.2.3`,
comparisons joined by `,`, and `*`. It is a new std module, `std.version`,
because a build script wants it too (`build.version_at_least ctx "1.2"`).
`mind` checks every requirement against the `[package] version` of what was
fetched, and a miss is an error that names the requirement, who wrote it,
and what was found:

```
mind: `json` is 2.0.1 (https://github.com/u/dream-json#v2.0.1), but
  `http` requires ^1.2       (deps/http/mind.toml)
  this project requires ^2.0 (mind.toml)
a program has one `json`: bring the requirements together, or give one of
them another name
```

Because a program has one namespace of packages, **one key is one version**
per program. Two requirements that no single version satisfies are a
conflict and are reported as one. The same holds when two sources are
different revisions of one repository. Today that is reported as "two
different directories". It becomes "two revisions of `json`", with the
revision each one asked for, which is the error a person can act on. There
is no registry, so there is nothing to choose between: resolution is
checking. The requirement is kept as data (`std.version.Requirement`) so that
a registry, when one exists, adds a solver in `mind` without changing a
manifest.

`mind.lock` records what the graph resolved to: every key's source, version
and, for git, commit, in a section per program (`[program]`,
`[build.NAME]`). A content digest joins them once `io.digest!` exists. A later build that resolves differently says so
rather than quietly building something else. `mind update` is how the lock
moves.

## Options: what a package can be built as

A package **declares** the settings it accepts, with types, defaults and a
line saying what each one is for:

```toml
[options]
vendored = { type = "bool", default = false, doc = "build sqlite from source" }
threads  = { type = ["off", "single", "multi"], default = "multi" }
cache_mb = { type = "integer", default = 64 }
```

An option is a `when` setting **scoped to its package**. `when vendored` in
`sqlite` reads `sqlite`'s option, and `when threads == "off"` compares
against its value. A dependency's settings used to be dropped, because `-D
vendored` from `sqlite` would have switched on every `when vendored`
everywhere. Scoping is what lets them be kept. A condition in a package's
module reads that package's options first, then the program's settings
(`os`, `arch`, `family`, `test`, and the root's defines). A module may also
read another package's option, as in `when sqlite.vendored`, which is how a
program adapts to how a dependency was built. The condition grammar gains
dotted names for this.

An option is set, a later place winning over an earlier:

1. its default in `[options]`;
2. the package's own `[build] defines`, for a flag it always wants;
3. `[config.KEY]` in a manifest that depends on it. It is a table rather
   than a field of `[dependencies]` so that a dependency of a dependency,
   which the root never lists, can still be configured;
4. the active profile's `[profile.NAME.config.KEY]`;
5. `mind build -D KEY:name=value`.

Every value is checked against the declaration. An option the package does
not declare, a value of the wrong type, and a choice outside the list are
errors that give the declaration. A typo in a key or a name is the common
case, and a silently ignored setting is the worst answer to it. When two
dependencies configure a third differently and the root says nothing, that
is a conflict naming both. The root is always able to settle one, by saying
it itself.

Options are known before any script runs, and they are part of the script's
context (`ctx.options`). So a script decides what to build from what it was
asked for. `build.define` and `build.set` in a plan may set only declared
options. A setting a script makes reaches the package's code but not its own
script, which would otherwise be a cycle.

A dependency may hang on an option:

```toml
[dependencies]
sqlite_sys = { path = "../sqlite-sys", when = "not vendored" }
```

`when` here is the condition grammar. It is evaluated against the package's
options once they are settled, and an edge whose condition is false is not
in the graph: not fetched, not built, and not a conflict.

The compiler takes a scoped setting as `-D KEY:name=value`. It is a colon
because a key may contain dots and a name may not. `--print-cfg` shows each
package's settings under its key.

### Profiles

A profile is a named set of build options for the whole program:

```toml
[profile.release]
defines = ["release"]
flags = ["--no-types"]           # compiler flags
target = "linux"

[profile.release.config.sqlite]
threads = "multi"
```

`mind build --profile release` (and `--release` for that one) picks it.
`debug` is the default. The profile is `ctx.profile` in every script and part
of every step's key, so switching profiles and back reuses both builds. A
dependency's profiles are ignored: how the program is built is the root's
decision, as it is in every other build tool that has profiles.

## The image's target

### In the image

The header's `flags` word already has one bit, `0x1` for a span section.
Two more fields go in it:

| Bits | Field | Values |
|---|---|---|
| 8-15 | `os` | `0x01` linux, `0x02` macos, `0x04` windows |
| 16-23 | `arch` | `0x01` x86_64, `0x02` aarch64 |

Each field is a set of the targets the image may run on, and **zero means
any**. So every image written until now means what it always meant, and so
does every image whose program does not care. The bootstrap stays
byte-identical, because the compiler does not care.

The fields are in the header rather than in a section because the VM has to
decide before it trusts anything else in the file, and because a set of
platforms is one word. Constraints that are not one word, such as a minimum
glibc or a minimum VM version, will belong to a `TRGT` section when there is
a reason for one.

### Deciding it

In order, the first that answers wins:

1. `dreams --target os=linux,arch=x86_64`. Either half may be left out, it
   may be given more than once to allow several targets, and `--target any`
   clears it. A single `os` also sets the `os` setting, so
   `when os == "linux"` follows the target rather than the machine doing the
   compiling. This is what makes cross-building possible at all.
2. What `mind` passes: the manifest's `[build] target` for the root, meeting
   what every build script's plan said.
3. **What the program is made of**, with no configuration at all:
   - A payload that begins like a native binary contributes its platform:
     ELF is Linux, Mach-O is macOS and PE is Windows, and the machine field
     of each gives the arch. An image carrying a Linux `.so` and a Windows
     `.dll` gets both OSes, because that is exactly the program that chooses
     between them at run time.
   - A `when` that consulted `os` or `arch` pins the image to the value it
     was compiled with. The loader already evaluates every condition, so it
     records which settings a condition read.

A program that carries a library it only uses when present, or that carries
several and picks with `os.platform ()`, says `--target any`.

### In the VM

A mismatch is refused before anything runs:

```
dream: notes.dream was built for linux (x86_64); this is macos (aarch64).
Rebuild it for this platform, or run it anyway with --any-target.
```

`--any-target` is the override, for a program that knows better.
`vm.target ()` answers the image's target set for a program that wants to
say something friendlier.

## Scaling

What keeps a build of a thousand packages fast:

- **The no-op build runs nothing.** Outcome reuse is a comparison of stamps.
  No VM starts and no script is compiled.
- **Scripts are compiled once and kept.** A script image is rebuilt only when
  a file it was compiled from has moved, and `dreams --modules` gives that
  list.
- **Independent work is concurrent** at both levels: packages' scripts, and
  steps within a script.
- **Work is shared by key**, across packages, and across projects with
  `MIND_BUILD_CACHE`.
- **Next: one driver for every script.** Because a script is a
  `Context -> Plan` function, `mind` can compile a single driver that imports
  every package's script under its key. That is one compile and one VM
  instead of hundreds, one scheduler with one job limit, and cross-package
  deduplication in memory rather than through the cache. It is left for
  later because per-package drivers are simpler to debug and already get the
  no-op case right. It is the reason scripts have no `main!`.

What does not grow with the graph: nothing in `std.build` keeps a list it
searches. The step table, the stamp table and the outcome are maps keyed by
key or path, which the note in CLAUDE.md about lists searched more often than
they are built says is not optional.

## Order of work, and what is open

1. **Target flags. Built.** `dreams --target`, payload sniffing, `when` tracking,
   the header bits, the VM's check and `--any-target`, and
   `docs/bytecode-format.md`. This stands alone and is useful now, for any
   image with an embedded library.
2. **VM primitives.** `io.stat!`, `io.digest!` and `digest`.
3. **`std.build` core.** The vocabulary, keys, the runner (sequential
   first, then concurrent), the stamp cache, outcomes, `write`, `command`,
   and `when test` blocks for each.
4. **The graph. Built.** Cycle detection with the loop in the message, version
   requirements and `std.version`, revision conflicts reported as such,
   three kinds of edge, and `mind.lock`. None of this needs build scripts,
   and every project with dependencies wants it now.
5. **Options and profiles.** `[options]`, `[config.KEY]`, scoped
   `-D KEY:name=value`, conditions evaluated per package, dotted names in
   `when`, conditional dependencies, and `[profile.*]`.
6. **`mind` runs scripts.** Script discovery, `[build-dependencies]`, drivers, outcome
   reuse, folding outcomes into the compile, `--target` and `-j`.
7. **Tools.** `cc`, `probe` and `fetch`, and `dream/tests/ffi` rebuilt as a
   package whose C library is made by its own `build.dr` rather than by the
   test script.
8. **The single driver**, and the shared cache as a default.

Settled:

- **A dependency's defines** are kept, as declared options scoped to it,
  and a consumer configures them with `[config.KEY]` ("Options").
- **Generated modules are the package's own**, through `-L KEY+=DIR`: a
  second directory searched for one package's modules. The rejected
  alternative, a separate package per script, would have put
  `sqlite_gen.version` where `sqlite.version` was meant.
- **`cc` lives in `std`**, because embedding a C library is a documented
  feature and `dream/tests/ffi` needs a C compiler. The `derive` contract
  means a third-party tool is exactly as capable.
