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
  the script and calls `run.run! script.plan` (`std.build.run`). Because the script has no
  entry point of its own, one driver can later import every package's script
  at once, as one program with one scheduler, which is the scaling item
  below.

Effects happen in exactly one place: the runner. A step's action is pure too,
and answers what is to be done as data -- write this text there, run this
program with these arguments (`build.Op`) -- which the runner carries out.
That is not a stylistic choice: a pure function cannot so much as name an
impure one, so an action that did its own work could not be made by a pure
script at all. `build.given` feeds the result of a step back into the plan
through a pure function of what the step made (see "Decisions that need the
machine").

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
    profile = "debug"   // the profile's name: "debug", "release", or one the project declares
    env = %{}           // only the variables the manifest names in [build] env
    options = %{}       // the package's options, settled ("Options")
    dependencies = %{}  // key => version, for everything it uses
    jobs = 1            // the runner's: how many steps at once
    cache = "target/build/steps"  // the runner's: where step directories live
    testing = false     // `mind test`'s run, which runs the plan's checks
    vm, compiler        // the VM and the compiler command the build uses
    only = []           // the checks a test run is asked for; every one when empty
    goals = []          // the goals `mind build GOAL..` asked for
}

mapping Target {
    os                  // :linux | :macos | :windows
    arch                // :x86_64 | :aarch64
}
```

The context reaches a script as `std.wire` bytes, and `wire` answers a name
only with an atom the program already has. So the fields' descriptions spell
out every atom they can hold (`os : :linux | :macos | :windows | :unknown`),
which is what makes each decodable, and the runner checks what it read with
`build.is_context` before a script sees it.

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
    tree(dir : :string, suffix : :string)   // every file under dir ending in suffix
    artifact(step : Step, name : :string)   // something a step makes
}
```

A bare string is accepted wherever an `Input` is and means `file`. A `tree`
is for what a step walks by itself, such as a directory of tests a script
runs: every file in it is part of the key, so adding one changes the key as
surely as editing one. An
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
    action              // Job -> [Op], carried out by the runner
    discovers = false   // it names what it read in deps.d ("Discovered inputs")
    cache = :shared     // :shared, or :never for one run every time it is asked for
    timeout = 0         // milliseconds, 0 for none: a step is a program and may diverge
}
```

A step's **key** is a digest of `tool`, `config` and its inputs, where a
file contributes the digest of its contents and an artifact contributes its
step's **result**: its key, and for a step that discovers its inputs, the
digests of what it read. The name is not in the key, so two packages that ask
for the same thing by different names share the work. Keys are what the cache
is indexed by and what `build.given` waits on. `build.discovering`,
`build.uncached` and `build.within ms` set the last three, which say how a
step is run and are in neither the key nor the shape.

A step run every time (`cache = :never`) declares nothing it reads, so its
key never moves. What reads it is keyed on what it wrote instead: an answer
that changed has to reach the steps that read it.

A step writes only into **its own directory**, which the runner makes, and
declares what it will write there. It never picks an output path. Two steps
therefore cannot collide, a half-finished step leaves nothing where a
finished one is expected (it runs in a scratch directory that is renamed into
place), and one cache can be shared by concurrent builds.

### Discovered inputs

Some steps cannot say what they read until they have read it. A C file's
headers are the familiar case. A Dream program is the same: `dreams main.dr`
reads whatever `main.dr` imports, and the list changes whenever an `import`
does. Declaring these by hand is the thing everyone gets wrong, and a `tree`
over the whole source directory is right but rebuilds too much.

A step with `discovers = true` names the files it read in a **depfile**,
`deps.d` in its directory: `cc` asks the compiler for one (`-MD`, or
`/showIncludes` from MSVC, turned into one), and `std.build.dream` asks
`dreams --depfile`. The runner keeps the list, with each file's digest, in
the step's record. The step is current when its key matches **and** every
file on the list still has the digest it had, which costs a `stat` per file
while the stamps hold. This is ninja's `deps`, and it is the only way a no-op
build of a C++ program costs a `stat` per header rather than a compile per
file.

The key still comes from the declared inputs alone, since the discovered
ones are not known until the step has run. One record is kept per key, and a
run with other reads replaces it. That is why a step downstream is keyed on
the **result** and not the key: an object whose header changed is compiled
again under the same key, and the program linked from it must see a different
input or it would not be relinked. It is also why keys are worked out as steps
become ready rather than all at once beforehand. The tests that pin this are
`std.build.run`'s own, with a step that copies its source and names a header,
and `mind/std/build/tests/incremental.dr`, which builds a library and a
program with the machine's compiler and changes a header only the program
includes.

### Jobs

What an action receives:

```dream
mapping Job {
    dir                 // this step's directory
    ctx : Context
    paths = %{}         // every input, resolved to a path on disk
}
```

with `build.path job input` and `build.out job "name"`. What an action answers
is a list of operations, done in order in the step's directory and stopping
at the first that fails:

```dream
union Op {
    write(name : :string, text : :string)
    copy(from : :string, name : :string)
    link(from : :string, name : :string)        // a copy sharing the bytes
    exec(program : :string, args : [:string])   // run in the step's directory
    run(dir, env, program, args)                // run elsewhere, with variables of its own
    run_to(dir, env, program, args, name)       // and keep everything it printed in `name`
    observe(dir, env, program, args, name)      // the same, whatever it exits with
    find(program, name)                         // where it is on PATH, or ""
    capture(program, args, name)                // what it printed when it succeeded, or ""
    attempt(program, args, name)                // "yes" or "no"
    list(dir, suffix, name)                     // the files under dir, one to a line
    verify(name, digest)                        // fail unless the SHA-256 is this
    transform(from : [:string], name, f)        // f of some files' texts, written as another
    expect(name, path, bless)                   // fail unless name holds what path does
    same(a, b)                                  // fail unless the two are byte for byte one
    empty(name, why)                            // fail, showing it, unless name is empty
}
```

An action touches only what it names, and only inside its own directory.
`exec` runs through `os.exec_in!`, which gives the child its own working
directory: `chdir!` is the whole VM's, and steps run at once. `run` is
`os.exec_with!`, which lays variables over the environment for the child
alone, and `run_to` and `observe` are `os.exec_joined!`, which writes the
child's errors in among its output as a terminal shows them -- what a check
comparing a program's output against a recorded file needs. The questions
(`find` to `list`) never fail, since "no" is an answer. `transform` takes a
pure function, which is how a tool turns what a program printed into what it
needs without an effect of its own: MSVC's `/showIncludes` into a depfile,
`dumpbin`'s listing into a `.def`, six answers of `llvm-config` into one
record. The runner makes the directory each declared output goes in before
the action runs.

The action is called in the runner, and what it answers is forced all the way
down before it crosses into the step's process. A suspension carries its
frame, and an action's frame holds the step's inputs, each of which holds the
step it comes from: the whole graph upstream, copied for every step run.

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
    checks = %{}        // name => Step, run by `mind test`
    goals = %{}         // name => Goal, made by `mind build NAME`
    notes = %{}         // name => a line saying what a goal or check is for
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
| `build.check name step` | a check `mind test` runs ("Checks, and `mind test`") |
| `build.goal name goal`, `build.note name text` | something to ask for by name ("Goals") |
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
and merges `f`'s plan in, which may itself contain further `given`s.
`build.given_files dir suffix f` is the same for what a directory holds: `f`
is handed the paths, from a listing that runs every time. `f` is
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
virtual let run job cfg;                // the operations that make them
virtual let version cfg = "1";          // change it to invalidate every step made before
virtual let settings ctx cfg = ();      // what it found that the config does not say, e.g. the compiler
virtual let label cfg = name cfg;       // what a step is called in a log

/// The step this configuration describes. The key covers the tool's
/// version, the target and the whole configuration, so changing a flag
/// rebuilds and changing nothing does not.
let step ctx cfg = ..;

/// Its first output, as an artifact: what most callers want.
let artifact ctx cfg = build.artifact (step ctx cfg) (list.head (outputs ctx cfg));
```

A tool's configuration is its own record, built with its own `|>`
modifiers, and handed to `step` or `artifact` at the end. A parser
generator:

```dream
// a package's own tool
import std.build;
derive std.build.tool;

mapping Config { grammar : :string, module = "parser", flags = [] }

let grammar g = Config.new g;
let flags fs c = Config.set_flags c (list.append (Config.flags c) fs);

let name c = "peg";
let inputs c = [Config.grammar c];
let outputs ctx c = [Config.module c + ".dr"];
let run job c = [build.Op.exec "peg-gen" (list.concat [Config.flags c, [build.path job (Config.grammar c)],
                                                       ["-o", Config.module c + ".dr"]])];
```

That is all a one-step tool is. It is why `cc` does not have to be in `std` to be as
good as one that is, and why the `derive` contract is checked: a tool that
forgets `outputs` is a compile error in the tool, not a mystery in a build.

What ships in `std.build`, because almost every build with native parts
needs it:

| | |
|-|-|
| `std.build.cc` | C and C++, object by object, with GCC, Clang or MSVC ("Native code") |
| `std.build.command` | any program: arguments are strings, inputs and `command.out "name"` |
| `std.build.check` | checks that declare what they read, kept like any step |
| `std.build.dream` | Dream programs, by a compiler that may be something the build made |
| `std.build.probe` | ask the machine: a library, `pkg-config`, a program on the path, LLVM |
| `std.build.fetch` | a URL pinned by digest; for vendored sources too large to check in |

`cc` and `dream` do not derive `std.build.tool`: each makes a graph of steps
rather than one -- a compile per source and a link, a compiler compiling the
compiler -- and is plain functions over `build.step`.

`std.build.command` is the escape hatch that keeps the rest honest:

```dream
let parser = command.make "peg" "peg-gen" [command.file "grammar.peg", "-o", command.out "parser.dr"];
build.module "parser" parser
```

Its arguments are a list whose inputs and outputs are marked, so the step's
inputs, outputs and key all come from the one list the program is run with.

Tools from packages are listed under `[build-dependencies]` and are compiled
into the script and not into the program. A tool is only code the script
imports, so `mind` treats it as a dependency like any other.

## Native code: `std.build.cc`

`cc` builds C and C++ with **the machine's own compiler**, whichever that is,
from one description:

```dream
let lib = cc.shared "demo" ["src/demo.cpp"]
          |> cc.standard :cxx20 |> cc.optimize 2 |> cc.warnings :extra
          |> cc.public_include "include" |> cc.threads
          |> cc.flags_for :msvc ["/permissive-"];
let app = cc.executable "demo" ["src/main.cpp"] |> cc.use lib;

let plan ctx = steps.with_toolchain ctx (fn tc ->
    build.empty |> build.payload "demo" (steps.artifact tc app));
```

A target is data in a vocabulary of settings (`cc.Setting`: a standard, an
optimization level, a define, an include directory, `rtti`, a sanitizer,
`threads`, `export_all`, a system library, ..), with a modifier for each that
takes the target last. A setting is in `settings` (this target's sources),
`public` (this target and everything that uses it) or a single source's own
(`cc.source_with "jit.cpp" [cc.Setting.rtti false]`). `cc.use x` brings what
it takes to use `x`: its public settings, and its output to link.

Each family translates the vocabulary: `std.build.cc.gnu` for GCC and Clang,
`std.build.cc.msvc` for `cl.exe` and `clang-cl`, and `std.build.cc.cmake`,
which writes a `CMakeLists.txt` for the same targets (`cmake.export`). What
is about one compiler says which -- `cc.flags_for :gcc [..]`, `:clang`,
`:gnu` for both, `:msvc` -- and a toolchain of another family leaves it out.
None of the translation runs anything, so all of it is tested on any machine,
and `mind/std/build/tests/toolchains.dr` builds one library and program with
GCC, with Clang and through CMake and runs each.

What differs between compilers is `cc`'s problem, not the script's:

| What differs | Who deals with it |
|---|---|
| flag spelling (`-O3`, `/O2`) | the family's translation of `cc.optimize 3` |
| file names (`libx.so`, `x.dll` + `x.lib`, `x.o`, `x.obj`) | `cc.file_name`, `cc.import_name`, `cc.object_name` |
| exporting from a DLL | `cc.export_all`: for MSVC, `dumpbin /symbols` and a `transform` write a `.def`, as CMake's `WINDOWS_EXPORT_ALL_SYMBOLS` does |
| finding a shared library at run time | a run path for GNU; on Windows the DLL goes in `bin/` beside the program |
| headers read | `-MD -MF` for GNU; `/showIncludes` for MSVC, taken apart into a depfile |
| system libraries | `cc.system "ws2_32"` is `-lws2_32` or `ws2_32.lib` |

**The toolchain is a probe's answer.** `steps.with_toolchain ctx f` asks the
C++ compiler what it is -- `$CXX` and `$CC` when the manifest lets the script
see them (`[build] env = ["CC", "CXX"]`), `c++`, `clang++` or `cl.exe`
otherwise -- and hands `f` a `cc.Toolchain` naming the family and the version
it printed. The toolchain is in every compile step's key, so a new compiler
under the same name rebuilds everything and nothing else does.

`steps.artifact tc target` is the target's file, and behind it:

- **one step per source file**, keyed on the toolchain, the settings that
  reach that one file and its contents, discovering its headers. A header
  changed recompiles what includes it and no others, and two targets that
  compile a file alike share the object.
- **one step to link**, laid out as it will be installed: `lib/libx.so`,
  `bin/x`, and a program carries the shared libraries it uses (`lib/`, with a
  run path of `$ORIGIN/../lib`; `bin/` on Windows). Every artifact runs where
  it lies, and a placed one runs where it is placed.

The compiler runs in the package's directory, so the paths a script writes
mean what they say there, and writes into the step's.

## Dream programs: `std.build.dream`

```dream
let stage2 = dream.program "dreams" "dreams/main.dr" seed vm |> dream.paths ["mind", "."];
let stage3 = dream.program "dreams" "dreams/main.dr" (dream.artifact stage2) vm |> dream.paths ["mind", "."];
```

The compiler is an **input**, so it may be an artifact, and compiling the
compiler with what it just built is a second program. So is the VM that runs
an image compiler. A program's modules are discovered inputs, from `dreams
--depfile`, so editing a module rebuilds exactly the images that import it.
`dream.no_warnings` fails a compile that printed one, as the examples want.

## Running a plan

`run.run! plan` is the driver's `main!`. It:

1. Reads the context `mind` wrote (`--context FILE`), or makes one for the
   host when it is run by hand. Running a script by hand is how it is
   debugged.
2. Evaluates `plan ctx`, collects every step reachable from it, and takes
   each once by key.
3. Runs the graph. A step starts as soon as everything it reads is made, in
   a process of its own, up to `ctx.jobs` at once, and is keyed then. As the
   note on `spawn!` in CLAUDE.md warns, the thunk handed to `spawn!` is made in
   a small function and carries only the step's forced operations, not the
   runner's frame or the step. A build stops starting steps at its first
   failure and raises it once what is running has finished; a test run keeps
   going and reports each check.
4. Skips a step whose directory holds a record of its key whose discovered
   reads still hold. It runs anything else in a scratch directory, then
   renames that into place.
5. Resolves `given`s once their inputs are built, and goes back to 2 with
   the merged plan.
6. Writes the **outcome**: what the plan contributes, with every input
   resolved to a path, and every file that was read along with its stamp.

**File digests are cached behind a stamp**, which is git's index trick. A
file's `(size, modified)` is kept beside its digest, and the contents are
read only when the stamp has moved. Digesting in Dream would be the most
expensive thing a no-op build does, so the VM does it: `io.stat!` gives the
stamp, `io.digest!` the SHA-256 of a file and the pure `io.digest` that of a
string, and the two agree on the same bytes.

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
and is treated as read-only. Every project shares one step cache,
`$MIND_HOME/build` unless `MIND_BUILD_CACHE` says otherwise. That is safe
because a step's directory is named by its key and filled atomically.

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

## Checks, and `mind test`

A plan may declare **checks**: `build.check name step`. A check is a step
that passes when its operations succeed, and it is run only by `mind test`,
and every time -- it is a question, not something made -- after whatever it
reads has been built. A test run's context says so (`testing`) and names the
VM and the compiler the build uses (`vm`, `compiler`), and its outcome carries
each check's result (`Outcome.checks`: passed, milliseconds, and what it
printed when it failed).

A suite written as a script is run with `command.check`, in the package's
directory, or `command.check_in` somewhere else, with variables laid over the
environment: `Op.run`, which is `os.exec_with!`. It is the one operation that
reaches outside the step's directory, and only a check has a use for it.

`mind test` runs each package's units -- its `when test` blocks, compiled
with `--test` from `[test] entry` or its entry, and run -- and its checks, all
at once, and names each `package.name`. A workspace (`[workspace] members`)
does that for every member and for its own checks. A test run writes no
record, so it never stands in for a build's answer.

## Goals

A plan may name what there is to ask for: `build.goal name goal`, with a
line about it from `build.note`. A goal is

| | |
|-|-|
| `build.make input` | build it, and say where it is |
| `build.place path input` | build it and put it at `path` in the package -- a whole step's outputs beneath it for `artifact s ""` |
| `build.each [names]` | several goals under one name |
| `build.effect step` | run a step every time it is asked for |

`mind build GOAL..` runs the package's script with the goals in its context
(`goals`), and the outcome says where each one is (`Outcome.goals`). A run
asked for goals runs every time and writes no record, as a test run does. A
goal is only built when it is asked for: a plan with goals costs nothing more
to a compile or a test. `build.place` is the one way a build writes outside
its cache, and it never writes into the file it replaces -- a copy beside it,
renamed over it -- so a VM running the old image keeps the pages it had. A
name the plan does not have is answered with the goals it does, and their
notes.

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

`mind` hands the compiler every declared option of every package, defaults
included, and the root's under its own name. Passing the defaults is what
makes an option scoped: a package's `vendored = false` shadows a program-wide
`vendored` where a missing one would fall through to it. An option whose
value is `false` is off. A plain `-D name` for an option the root declares
sets the option rather than defining a program-wide flag.

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

The two decide each other: an edge reads the options of the package listing
it, and those are set by the packages depending on it, which are only known
once the graph is. So `mind` walks with the options it assumes (the defaults
at first), settles them over the graph that came out, and walks again if a
condition read a value that settling changed. A real graph settles in two
walks; one that has not settled in eight is reported as a loop, an edge that
configures its own condition off. A `[config.KEY]` for a package whose edge is
off is not a typo. A package has one set of options per build, whichever
program it is compiled into, so an edge in a build script's graph reads the
program's settled values.

The compiler takes a scoped setting as `-D KEY:name=value`. It is a colon
because a key may contain dots and a name may not. `--print-cfg` shows each
package's settings under its key.

### Profiles

A profile is a named set of build options for the whole program:

```toml
[profile.release]
defines = ["release"]
flags = ["--no-opt"]           # compiler flags
target = "linux"

[profile.release.config.sqlite]
threads = "multi"
```

`mind build --profile release` (and `--release` for that one) picks it.
`debug` is the default. `debug` and `release` exist whether they are declared
or not, and an undeclared `release` defines `release`, which is what
`--release` meant before profiles existed. `--debug` is still just `-D debug`. The profile is `ctx.profile` in every script and part
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
- **One driver per set of build dependencies.** Because a script is a
  `Context -> Plan` function, `mind` compiles one driver that imports every
  script sharing build dependencies, each under a name of its own. That is one
  compile and one VM instead of one per package, and it is the reason scripts
  have no `main!`. Scripts with different build dependencies cannot share a
  program, since each is a namespace of its own; see item 8 below.

What does not grow with the graph: nothing in `std.build` keeps a list it
searches. The step table, the stamp table and the outcome are maps keyed by
key or path, which the note in CLAUDE.md about lists searched more often than
they are built says is not optional.

## Order of work, and what is open

1. **Target flags. Built.** `dreams --target`, payload sniffing, `when` tracking,
   the header bits, the VM's check and `--any-target`, and
   `docs/bytecode-format.md`. This stands alone and is useful now, for any
   image with an embedded library.
2. **VM primitives. Built.** `io.stat!`, `io.digest!` and `io.digest`, which is
   SHA-256 so that the digest pinning a fetched source is the one its
   publisher printed. The pure one is in `std.io` rather than among the
   builtins: it is `digest!` asked of bytes already in hand, and a builtin
   costs an opcode.
3. **`std.build` core. Built.** The vocabulary (`std.build`), keys, the
   runner (`std.build.run`: each step as soon as its inputs are made, `jobs`
   at a time), the stamp
   cache, outcomes, `write`, `std.build.command`, and `std.file` beneath them,
   each with its `when test` block. Actions answer operations rather than
   performing them (see "Jobs"). The `std.build.tool` behavior waits for
   item 7, which is its first user besides `command`.
4. **The graph. Built.** Cycle detection with the loop in the message, version
   requirements and `std.version`, revision conflicts reported as such,
   three kinds of edge, and `mind.lock`. None of this needs build scripts,
   and every project with dependencies wants it now.
5. **Options and profiles. Built.** `[options]`, `[config.KEY]`, scoped
   `-D KEY:name=value`, conditions evaluated per package, dotted names in
   `when`, conditional dependencies, `[profile.*]`, `[build] target`, and
   `mind options`. What is left for item 6 is `ctx.options` and a script's
   `build.define`, since there are no scripts to hand them to yet.
6. **`mind` runs scripts. Built.** Script discovery (`build.dr`, or `[build]
   script`), `[build-dependencies]`, drivers, outcome reuse (`record`: the
   context's digest and the stamps of the script's sources and of everything
   it read), folding outcomes into the compile, `--target` and `-j`, and
   `mind/tool/tests/scripts.sh`. The overlay is `-L KEY+=DIR` in
   `dreams/package.dr`. A driver is written into a directory with a manifest
   of its own, so that `import build` finds the script it is for and not the
   enclosing project's; and the outcome is a record in `std.build`
   (`Outcome`), because it crosses to `mind` as `std.wire` and a reader has to
   name every key it accepts.
7. **Tools. Built.** `std.build.tool` (the behavior, with `settings` and
   `label` beside the four holes), `cc`, `probe` and `fetch`, and
   `dream/tests/ffi` rebuilt as a package whose C library is made by its own
   `build.dr` -- `test-ffi` now builds it with `mind`. Asking the machine
   needed operations that answer rather than fail (`find`, `capture`,
   `attempt`) and one that pins (`verify`). A tool's configuration may hold
   artifacts, and a step's key reads it through `build.keyed`, which puts each
   artifact's step shape where the step was; and `Job.paths` is keyed by
   `build.input_id`, a string, because a map compares list keys by identity.
8. **The single driver, and the shared cache as a default. Built.** Scripts
   whose build dependencies are the same are one driver, one compile and one
   VM (`run.run_all!`), each script in a process of its own and the stamp
   table read once and written once. Scripts with different build
   dependencies get a driver each, because each script's packages are a
   namespace of its own and one program has one: that is why it is a driver
   per set of build dependencies rather than one per build. Every script is a
   `build.dr`, so a driver names each package for its own compile
   (`-L mind_script_N=DIR` and the overlay `-L mind_script_N+=DIR`, which finds
   the script beside the manifest when the package's modules are in `src/`)
   and imports it `as script_N`. A package's record keeps its own script's
   sources, from `dreams --modules` on the script, so editing one script runs
   that one again and no other; the driver is recompiled when a stale
   script's sources moved. Steps are cached in `$MIND_HOME/build` unless
   `MIND_BUILD_CACHE` says otherwise.
9. **Checks, workspaces and `mind test`. Built.** See "Checks, and `mind
   test`" above. The repository is a workspace whose tests are what `just
   test` runs.
10. **Discovered inputs, goals and native code. Built.** Two lines of work
   on this design met here, and this is what the second added to the first:
   `discovers`, records of what a step read, and keying on a step's result
   ("Discovered inputs"), with `dreams --depfile`; a scheduler that starts each
   step when its inputs are made and keys it then, where the first ran by
   depth and keyed everything beforehand, which discovered inputs cannot do;
   `tree` inputs, `given_files`, `cache`, `timeout`; goals and `mind build
   GOAL`; `std.build.cc` as targets in a vocabulary of settings, translated by
   `cc.gnu`, `cc.msvc` and `cc.cmake`, built object by object
   (`cc.steps`); `std.build.check`, `std.build.dream`, `probe.llvm`; the
   operations those needed (`link`, `run_to`, `observe`, `list`, `transform`,
   `expect`, `same`, `empty`); and the file natives under `std.file`
   (`mkdir_all!`, `remove_all!`, `copy!`, `link!`, `chmod!`, `walk!`) and
   `os.exec_joined!`. The repository's `build.dr` builds the VM, the compiler,
   `mind` and `lucid` as goals (`mind build default`).

Open:

- **MSVC's environment.** `cl.exe` needs the `INCLUDE`, `LIB` and `PATH`
  `vcvarsall.bat` sets up. The plan is a probe that finds Visual Studio with
  `vswhere`, captures that environment once, and runs each step with it; until
  then a build with MSVC is run from a developer prompt. The MSVC translation
  is tested on its own and has not been run against `cl.exe`.
- **Long command lines.** A response file (`@args`) when a line would pass
  Windows' 32K limit.
- **Profile-guided builds** as artifacts (`cc.profile_generate`, a training
  step, `cc.profile_use`), which would make `vm-pgo` a goal. The flags that
  make a profile survive objects compiled in scratch directories were
  measured with GCC 15: `-fprofile-prefix-path`, `-fprofile-dir`, and objects
  named by their source (`dream#src#interp.cpp.o`), which `cc` already does.
- **Step workspaces**: a directory kept between runs, named by a step's
  identity, for a program that is incremental by itself (`cmake --build`).
  That is what driving another project's CMake build wants; `cc.cmake` today
  only writes the `CMakeLists.txt`.
- **A step's output shown when it succeeds.** A compile's warnings are seen
  only when it fails; the build is meant to be warning-free, and a log kept
  beside each step would let `mind` say so.
- **The repository's checks, per program.** `e2e` and `examples` are each one
  suite. A check per program and tier, kept by key, would rerun only what an
  edit reaches; `std.build.check` can say it, and what holds it back is the
  rest of what `e2e.sh` checks (the JIT's IR, shebang and payload images).

Settled:

- **A dependency's defines** are kept, as declared options scoped to it,
  and a consumer configures them with `[config.KEY]` ("Options").
- **Generated modules are the package's own**, through `-L KEY+=DIR`: a
  second directory searched for one package's modules. The rejected
  alternative, a separate package per script, would have put
  `sqlite_gen.version` where `sqlite.version` was meant.
- **`cc` lives in `std`**, because embedding a C library is a documented
  feature and `dream/tests/ffi` needs a C compiler. It is plain functions
  over `build.step` rather than a `std.build.tool`, because it makes a graph
  rather than a step, which a third-party tool can do the same way.
- **Actions answer operations** rather than performing them. The other line
  of work had an action answer a suspended call to an impure function, which
  a pure script may make; operations won because they are data -- a step's
  work can be shown, compared and moved to a worker without the frame it was
  written in -- and everything the other needed fitted in a few more of them.

## Building the VM's dependencies

The repository's `build.dr` prefers static LLVM component archives and a PIC
`libffi.a`, matching CMake's default `DREAM_LINK_DEPS=PREFER_STATIC`. It tests
archives with the selected compiler by linking a shared library that references
the dependency. If an archive is absent, has incompatible relocations, or needs
unavailable system libraries, the default falls back to shared linkage.

Use `mind build vm -D link_deps=static` to require usable archives, or
`-D link_deps=shared` to request shared dependencies. The CMake equivalents are
`-DDREAM_LINK_DEPS=STATIC` and `-DDREAM_LINK_DEPS=SHARED`. These settings govern
LLVM and libffi; `libdream` remains shared, and OpenSSL and LLVM's system
libraries may still be runtime dependencies. LLVM stays optional, and
`-D jit=off` skips LLVM discovery and validation.

The Dream build finds LLVM through `llvm-config` on `PATH`, and libffi through
`pkg-config`, including its `libdir`. Use `nix-shell` for the repository's full
development environment, including PIC libffi, LLVM and zlib. CMake additionally
accepts `DREAM_LLVM_CONFIG`, `LLVM_DIR`, `DREAM_FFI_INCLUDE` and `DREAM_FFI_LIB`.
The Dream build's automatic static-libffi selection supports GNU-style
compilers; use CMake for a strict static build with MSVC.

Explicit archive files are build inputs. Replacing one at the same path relinks
its consumers. `std.build.probe.llvm_link` exposes LLVM's linkage selection,
and `std.build.probe.shared_link` tests whether dependency flags and a reference
program can form a shared library. `just test-build` checks PIC rejection,
unresolved symbols and incremental relinking alongside the compiler tests.
