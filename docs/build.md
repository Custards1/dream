# Build scripts and `std.build`

A package may carry a `build.dr`: a program `mind` runs before it compiles
the package, to make what the package needs and cannot write by hand. That
covers a C library to embed, a module generated from a grammar or a schema, or
a switch that depends on what the machine has installed. `std.build` is the
vocabulary a script is written in. Tools such as a C compiler, a parser
generator or `protoc` add to that vocabulary through one behavior, so each of
them extends the same build rather than bringing its own.

The same vocabulary is meant to be **this repository's build**. The VM, the
compiler's bootstrap, `mind`, `lucid`, the examples and every test group are
today a `justfile`, a CMake project and `build.sh`. They become one
`build.dr` at the root. That is the harder test of the design, and most of
what was added after the first draft comes from it: native programs built
object by object (`std.build.cc`), steps that find their inputs as they run,
goals and checks a person asks for by name, and a bootstrap that starts with
nothing but a C++ compiler ("Building this repository").

This document is the design. The image's target and the package graph are
built; the last section is the order to build the rest in and what is still
open.

## What it has to be

Five constraints decided most of what follows.

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
- **It builds Dream itself.** A C++ program with an optional LLVM, a
  profile-guided rebuild, a compiler that compiles itself until it stops
  changing, and a dozen test groups: if `std.build` can say all of that
  without an escape into shell, it can say what a package needs. And it must
  not need CMake to do it. CMake is something `std.build` can drive, for a
  library that only comes with a `CMakeLists.txt`. It is not what C is built
  with.

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
    jobs = 1            // how many steps may run at once
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

A context is a value, and a script is a function of it. So a build that
wants the same thing made two ways calls the same function with two
contexts. `vm (build.with_option "jit" "off" ctx)` is the interpreter-only
VM beside the ordinary one, in the same build, sharing every object file the
two have in common. There is no second build directory to keep apart from
the first, because the steps are told apart by key.

`build.option ctx "jit"` reads a settled option, and `build.profile ctx`
the profile.

`build.shared_name ctx "sqlite3"` is `libsqlite3.so`, `libsqlite3.dylib` or
`sqlite3.dll` for the target, with `build.exe_name` and `build.object_name`
alongside it. Every tool needs these, so they are written once, here.

### Inputs and artifacts

```dream
union Input {
    file(path : :string)                    // in the package, relative to its root
    tree(dir : :string, pattern : :string)  // every file under dir that matches
    artifact(step : Step, name : :string)   // something a step makes
}
```

A `tree` is for the input that is a directory in all but name: a
`dream/include` handed to a compile, or `mind/std` handed to `dreams`. Its
contribution to a key is the digest of every matching file, and the runner
finds them. The script never lists a directory, because listing one is
asking the machine something, and a pure plan cannot ask. A plan whose
**shape** depends on a listing uses `build.given (build.glob "examples/*.dr")
(fn paths -> ..)`, a `given` like any other, whose answer is a stamp on the
directory.

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
    action              // Job -> $( [:ok, log] | [:error, log] ), run by the runner
    discovers = false   // it reports the files it read ("Discovered inputs")
    workspace = false   // it keeps a directory between runs ("Workspaces")
    cache = :shared     // :shared | :local | :never
    timeout = 600000    // milliseconds; a step is a program and may diverge
    cores = 1           // what it takes of ctx.jobs; :all for a build of its own
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

`cache` says how far a result may travel. `:shared` is the default and the
reason keys exist. `:local` keeps it to this project, for an output that
records a path on this machine (a `compile_commands.json`). `:never` runs
every time it is asked for, which is right for installing and for a check
whose point is to be run again. A step with `cores = :all` runs alone,
because it is a build of its own (`make`, `cmake --build`) that spends the
job count itself.

### Discovered inputs

Some steps cannot say what they read until they have read it. A C file's
headers are the familiar case. A Dream program is the same: `dreams main.dr`
reads whatever `main.dr` imports, and the list changes whenever an `import`
does. Declaring these by hand is the thing everyone gets wrong, and a
`tree` over the whole source directory is right but rebuilds too much.

A step with `discovers = true` names the files it read, in a **depfile**
beside its outputs: `cc` asks the compiler for one (`-MD`, or
`/showIncludes` from MSVC) and the `dream` tool asks `dreams --depfile`.
The runner keeps the list with the step's record. The step is current when
its key matches **and** every file on the list has the stamp it had, and
when a stamp has moved, the contents it had. This is ninja's `deps`, and it
is the only way a no-op build of a C++ program costs a `stat` per header
rather than a compile per file.

The key still comes from the declared inputs alone, since the discovered
ones are not known until the step has run. So a key can have several records
in a shared cache, one for each set of discovered files it was seen with, and
the runner takes the one whose files all match. (The runner as built keeps
one record per key, and replaces it when the reads differ.)

What a step **downstream** is keyed on is therefore not the key but the
step's **result**: the key and the digests of what it read. An object whose
header changed is compiled again under the same key, and the program linked
from it must still see a different input, or it would not be relinked. The
test that found this is `mind/std/build/tests/incremental.dr`, which changes
a header only the program includes and checks what it prints.

### Workspaces

A step with `workspace = true` keeps a directory between runs. That is for a
program that is incremental by itself and would be slowed down by being
handed an empty directory each time: `cmake --build`, `npm install`, a
`make` in a vendored tree. The workspace is named by the step's **identity**,
which is the key without the inputs' contents: the tool, the whole
configuration and the target. The same configuration always finds the same
workspace, and a changed one finds a fresh one. No workspace is reused under
a configuration it was not made with, and the `justfile`'s `build-pgo`,
`build-nojit` and `build-tsan` are what it cost to learn that by hand.

A workspace step runs when its key has changed or when a discovered input
has moved, and runs in its workspace rather than a scratch directory. Its
outputs are then copied into a directory named by its key, so a step
downstream sees an immutable artifact as it would from any other step. A
workspace is never shared between projects, and `cache` is at most `:local`
for its step.

### Jobs

An action is a function from a job to a **thunk**: `fn job -> $( exec! job
[..] )`. Dream has no impure lambda, and a pure function may not name an
impure one, but it may suspend a call to one. So describing a step stays
pure, and a plan is still a pure function of its context; the effect happens
when the runner runs the thunk, which it does in a process of its own
(`join! (spawn! ..)`). An action that raises is then the step failing, with
the error as its log, not the build stopping. A tool's `run!` hole is used
the same way: the behavior's `step` wraps it as `fn job -> $( run! job cfg )`.

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

`build.exec!` runs its program with an **environment the step chose**: the
variables in the step's configuration, and nothing inherited but `PATH`. A
variable that changes the output has to be in the configuration to be set at
all, and so it is in the key. `DREAM_JIT_SYNC=1` for a training run is a
field of that run's configuration, not something the shell happened to have.
The program's output goes to a log in the step's directory. It is shown when
the step fails and is kept when it succeeds, so a long compile can be
followed with `mind log STEP` rather than lost into a pipe.

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
    goals = %{}         // name => Goal, what `mind build NAME` makes
    checks = %{}        // name => Step, what `mind test` runs
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
| `build.goal name goal` | something to ask for by name ("Goals and checks") |
| `build.check name step` | a test ("Goals and checks") |
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

`build.given input f` waits for the input to be built, reads it as data
(a probe writes JSON, and a plain file is a string), and merges `f`'s plan
in. That plan may itself contain further `given`s. `f` is pure, so the effect
is still only in the step. The runner resolves `given`s in waves, and a plan
with none is a single graph known up front, which is the common case and the
fast one.

`given` also answers an **input**: `build.given_input probe (fn answer ->
input)` is an artifact whose step is not known until the probe has run. That
is what a tool uses when the answer decides what gets built, such as
`jit.cpp` when LLVM is found and `jit_stub.cpp` when it is not. The plan
around it does not have to be rewritten as a `given` of its own.

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
virtual let env cfg = %{};              // what build.exec! sets for it
virtual let discovers cfg = false;      // Step's fields, when the tool wants them
virtual let workspace cfg = false;

/// The step this configuration describes. The key covers the tool's
/// version, the target and the whole configuration, so changing a flag
/// rebuilds and changing nothing does not.
let step ctx cfg = ..;

/// Its first output, as an artifact: what most callers want.
let artifact ctx cfg = build.artifact (step ctx cfg) (list.head (outputs ctx cfg));
```

A tool's configuration is its own record, built with its own `|>`
modifiers, and handed to `step` or `artifact` at the end. A tool that makes
more than one step, as `cc` makes one per object and one to link them, is
written as several small tools whose artifacts feed each other, and exports
the function that assembles them. The runner sees only steps.

That is all a tool is. It is why `cc` does not have to be in `std` to be as
good as one that is, and why the `derive` contract is checked: a tool that
forgets `outputs` is a compile error in the tool, not a mystery in a build.

What ships in `std.build`:

| | |
|-|-|
| `std.build.cc` | C and C++ with the machine's own compiler: objects, static and shared libraries, executables ("Native code") |
| `std.build.dream` | a Dream program, compiled by a compiler that may itself be an artifact ("Dream programs") |
| `std.build.check` | tests: a program that must succeed, output that must match, two files that must agree |
| `std.build.command` | any program: arguments are strings, inputs and `command.out "name"` |
| `std.build.probe` | ask the machine: a program on the path, a library, `pkg-config`, `llvm-config`, a flag the compiler takes |
| `std.build.fetch` | a URL pinned by digest; for vendored sources too large to check in |
| `std.build.files` | copy, place and install: the only steps that write outside `target/` |
| `std.build.cmake` | a project that comes with a `CMakeLists.txt`, driven rather than rewritten |

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

## Native code: `std.build.cc`

`cc` builds C and C++ with **the machine's own compiler**, whichever that is.
The name is the language family's and not the program's. Nothing in it runs
a binary called `cc` unless that is what the machine's default turns out to
be.

### The toolchain

A compiler is a behavior, as a tool is:

```dream
// mind/std/build/cc/toolchain.dr
virtual let family tc;                          // :gnu | :msvc, the flag dialect
virtual let compile tc source object opts;      // the argument list for one object
virtual let link tc kind objects out opts;      // kind: :executable | :shared | :static
virtual let headers tc object;                  // how it says what it included
virtual let profile_generate tc opts;           // "Profile-guided builds"
virtual let profile_use tc profile opts;
```

`std` ships `cc.gcc`, `cc.clang` (which derives `cc.gcc` and overrides the
few holes where they differ), `cc.msvc`, and `cc.cmake` ("CMake as a
backend"). A cross compiler, `zig cc`,
Emscripten or a vendor's compiler is another module deriving the same
behavior, in a package or in the project. It is as good as the ones in
`std`, for the same reason a tool is.

**The default is found, not configured.** `cc.default ctx` is a probe step.
It takes `$CXX` or `$CC` when the manifest lets the script see them, and
otherwise the platform's own: `c++` and `cc` on Linux and the BSDs,
`clang++` on macOS, and on Windows `cl.exe` from the newest Visual Studio
`vswhere` finds, then `clang-cl`, then a MinGW `g++`. It
asks the program for its version, and from the answer learns its family.
That answer (path, family and version) is an input of every compile step,
so installing a new compiler rebuilds everything and nothing else does.
`cc.with_toolchain (cc.gnu "clang++-19")` names one outright.

The toolchain is chosen **when a step runs, not when the plan is written**.
A plan says `cc.optimize 3`; the action, which has the probe's answer, turns
that into `-O3` or `/O2`. So an ordinary C build has no `given` in it at
all, and its graph is known before anything has been asked of the machine.

### One script, every compiler

A `build.dr` is meant to build unchanged with GCC, Clang, MSVC, or through
CMake. What differs between them is `cc`'s problem and the toolchain's, not
the script's:

| What differs | Who deals with it |
|---|---|
| flag spelling (`-O3`, `/O2`) | the vocabulary: `cc.optimize 3`, translated by the toolchain |
| file names (`libx.so`, `x.dll` + `x.lib`, `x.o`, `x.obj`) | `build.shared_name` and the toolchain's link hole |
| exporting from a shared library | `cc.export_all`: nothing for GNU, where everything is exported; for MSVC, a step lists the objects' external symbols and writes a `.def`, as CMake's `WINDOWS_EXPORT_ALL_SYMBOLS` does |
| finding a shared library at run time | a run path for GNU; for Windows, which has none, the DLL goes in `bin/` beside the executable |
| the developer environment | MSVC needs `INCLUDE`, `LIB` and a `PATH` that `vcvarsall.bat` sets up. The probe finds Visual Studio with `vswhere`, captures that environment once, and each step is run with it (`os.run!`'s `:env`). No developer prompt needed |
| headers read | `-MD -MF` for GNU; `/showIncludes` for MSVC, whose prefix is translated into the installed language, so the probe learns it by compiling one file |
| long command lines | a response file (`@args`) whenever a line would pass Windows' 32K limit, for both families |
| system libraries | `cc.system "ws2_32"` is `-lws2_32` or `ws2_32.lib` |
| things one compiler cannot do | the toolchain says so: `cc.sanitize :thread` under MSVC is an error naming the toolchain, and PGO without support is a warning and an ordinary build |

What stays in a script is what is really about one compiler, and it says
which: `cc.flags_for :msvc [..]`, `cc.define_for :msvc
"_CRT_SECURE_NO_WARNINGS"`. The selector is a family, `:gnu` (GCC and Clang,
which take the same flags) or `:msvc` (`cl` and `clang-cl`), or one
compiler, `:gcc` or `:clang`, for what only one of them takes, such as
GCC's `--param`s. A toolchain that does not match leaves the line out.

The toolchain is picked by `[build] toolchain` in the manifest or `mind
build --toolchain NAME`: `auto` (the default above), `gcc`, `clang`,
`msvc`, `cmake`, or the name of a toolchain module a package provides. It
is part of every step's key, so building with two compilers keeps two sets
of objects and switching between them rebuilds nothing.

### CMake as a backend

`--toolchain cmake` builds the same targets through CMake. `cc` writes a
`CMakeLists.txt` from them and hands it to the `cmake` tool as one workspace
step. So a machine where CMake is how a compiler is found (a Visual Studio
install, an IDE, a cross toolchain file) builds a `build.dr` without anyone
porting it.

The translation is direct, because the vocabulary was chosen to have one:

| `cc` | CMake |
|---|---|
| `cc.shared`, `cc.static`, `cc.executable` | `add_library(.. SHARED/STATIC)`, `add_executable` |
| `cc.standard :cxx20` | `CXX_STANDARD 20`, `CXX_STANDARD_REQUIRED ON` |
| `cc.optimize`, `cc.debug_info` | per-target flags, generated for the compiler CMake found |
| `cc.define`, `cc.include`, `cc.public_include` | `target_compile_definitions`, `target_include_directories` (`PRIVATE`/`PUBLIC`) |
| `cc.use target` | `target_link_libraries` |
| `cc.use probe_answer` | the paths the probe already found, written in as they are |
| `cc.flags_for :gnu [..]` | `$<$<CXX_COMPILER_ID:GNU,Clang>:..>` |
| `cc.when_accepted [..]` | `check_cxx_compiler_flag` |
| `cc.export_all` | `WINDOWS_EXPORT_ALL_SYMBOLS` |

Probes are not translated into `find_package`. They run first, as in any
other build, and CMake is handed their answers. There is one way of
finding LLVM, and it is `probe.llvm`, whichever backend builds.

What the backend gives up is what CMake does not have: objects shared by
key across packages, and headers discovered per object. It rebuilds what
CMake's own tracking says to, inside its workspace.

The same translation is `cc.cmake_export [targets]`, a step whose output is
a `CMakeLists.txt` that needs nothing from `mind`. That is how the
embedders' CMake file for the VM is made ("What happens to CMake"): it is
generated, so it cannot fall out of step with the build.

### Targets

Built, with the translations below it: `mind/std/build/cc/`.

```dream
union Setting {
    standard(language : :atom)              // :c11 :c17 :cxx17 :cxx20 :cxx23
    optimize(level : :any)                  // 0 to 3, or :size
    debug_info(on : :bool)
    warnings(level : :atom)                 // :none :default :all :extra
    define(name : :string, value : :any)
    include(dir : :string)
    rtti(on : :bool)
    sanitize(kind : :atom)
    threads
    flags_for(selector : :atom, flags : [:string])
    system_for(os : :atom, library : :string)
    export_all
    ..
}

mapping Target {
    kind : :atom        // :executable | :shared | :static | :external
    name : :string
    sources = []        // [[path, [Setting]]]: each file, and its own settings
    settings = []       // [Setting], for every source of this target
    public = []         // [Setting], for this target and every target using it
    uses = []           // [Target]
}
```

with a modifier for each setting, taking the target last: `cc.standard
:cxx20`, `cc.optimize 3`, `cc.debug_info true`, `cc.warnings :all`,
`cc.define "NAME"`, `cc.include "dir"`, `cc.public_include "dir"`,
`cc.rtti false`, `cc.sanitize :thread`, `cc.source_with "jit.cpp"
[Setting.rtti false]`. A flag with no word of its own is `cc.flags_for :gnu
[..]`, which a toolchain of another family leaves out rather than choking on.

A setting is data, so a family's translation is a function from a setting to
flags (`gnu.compile_flags`, `msvc.compile_flags`), and a family's command
lines are functions from a target and a toolchain description to argument
lists (`gnu.compile`, `gnu.link`, `msvc.compile`, `msvc.link`). None of it
runs anything, so all of it is tested on any machine. `just test-build` then
builds one description of a library and a program with GCC, with Clang, and
through CMake, and runs each result.

`cc.use x` is the one idea from CMake worth keeping. `x` brings what it takes
to use it: a target's `public` options and its output to link, or a probe's
answer (`probe.library "ffi" "ffi.h"`, `probe.pkg_config "zstd"`,
`probe.llvm ctx ["orcjit", "native"]`). The consumer does not repeat
another target's include directories, and a library that moves its headers
moves them in one place.

`cc.when_accepted ["-fno-semantic-interposition", "-fno-plt"]` keeps the
flags the compiler accepts. That is a probe too, one step keyed on the
toolchain and the list, whose answer is read by the compile actions. An
answer that changes only a command line is an input. Only an answer that
changes the **shape** of the graph, such as which files are compiled, needs a
`given`, and `cc.given` is `build.given` for a target:
`cc.given answer (fn a -> fn t -> ..)`.

### Steps

`cc.executable`, `cc.shared` and `cc.static` each give an artifact, and
behind it:

- **one step per source file**, keyed on the toolchain, the options that
  apply to that file and the file's contents, with `discovers = true` for its
  headers. A header change recompiles the files that include it and no
  others. An object depends on nothing about its target but its options, so
  a shared and a static build of one library with the same options share
  every object, and so do two packages that vendor the same `sqlite3.c`.
- **one step to link**, whose inputs are the objects and what the target
  uses.

An executable that links a shared library from the build gets that library
in its own output (`bin/dream` beside `lib/libdream.so`, with a run path of
`$ORIGIN/../lib`, or `@loader_path` on macOS). Every artifact runs where it
lies, and a placed one runs where it is placed.

`cc.compile_commands [targets]` writes a `compile_commands.json` for clangd
and editors, and is a goal like any other.

### Profile-guided builds

A profile is an artifact:

```dream
let pgo ctx = {
    let trained = libdream ctx |> cc.profile_generate;
    let profile =
        cc.train "self-compile" (vm_with trained) [seed, "-L", "mind", "-L", ".",
                                                   "-o", command.out "stage2.dream",
                                                   "dreams/main.dr"]
        |> command.env "DREAM_JIT_SYNC" "1";
    vm_with (libdream ctx |> cc.profile_use profile)
};
```

`cc.train` runs the instrumented program in its own directory, gathers
what it wrote, and hands it to the toolchain's `profile_use`. For GCC that is
the `.gcda` files; for Clang it is `llvm-profdata merge` first, which is why
the toolchain owns both halves. The instrumented build, the training and the
optimized build are three sets of steps with three keys. So `vm-pgo`'s rule,
build somewhere of its own and always from scratch, is simply what happens:
a profile describes one set of sources, and different sources are a
different key.

GCC files a profile under the path of the object it counts, and a `cc`
object is compiled in a scratch directory that no later step will share.
Three flags make that not matter. This was checked with GCC 15, with two
objects each compiled in a directory of its own, trained from `/`, and used
from two new directories:

| | |
|-|-|
| `-fprofile-prefix-path=SCRATCH` | the profile is filed under the object's name within its step, not its whole path |
| `-fprofile-dir=%q{DREAM_PROFILE_DIR}` | the instrumented program writes wherever the training step's environment says, decided when it runs, not when it was compiled |
| objects named by their source (`dream#src#interp.cpp.o`) | the name is the same in every step that compiles that file, so the use build finds it. The extension stays, so `a.c` and `a.cpp` are two objects |

The training run wrote `src#hot.gcda` and `src#main.gcda` into its own
directory. The use compiles, each with its own scratch directory as the
prefix, found them under `-Werror=missing-profile` and read precise counts.
A control compile without the prefix looked for the whole scratch path and
failed. Clang files its profile by function rather than by object, so it
needs none of this.

## Driving other build systems: `std.build.cmake`

For a library that comes with a `CMakeLists.txt` and is not worth
rewriting, `cmake` drives it and hands back something `cc.use` accepts:

```dream
let zstd ctx =
    cmake.project "vendor/zstd/build/cmake"
    |> cmake.set "ZSTD_BUILD_SHARED" true
    |> cmake.targets ["libzstd_shared"]
    |> cmake.library ctx "zstd" ["lib/libzstd.so"] "vendor/zstd/lib";
```

It is one **workspace** step with `cores = :all`, and CMake does its own
incremental build inside it:

- **Every setting on every configure**, and the toolchain from the same
  `cc.default` probe (`CMAKE_C_COMPILER`, `CMAKE_CXX_COMPILER`), so a
  CMake-built library and a `cc`-built program are compiled by one compiler.
  The build type follows `ctx.profile` unless it is set.
- **The workspace is the configuration's.** A changed setting gets a new
  build tree rather than editing an old tree's cache, so a setting that the
  script has stopped passing cannot live on in `CMakeCache.txt`.
- **Discovered inputs from CMake itself.** The File API (`codemodel-v2`,
  `cmakeFiles-v1`) lists every source and every CMake file the configure
  read, and Ninja's deps log (`ninja -t deps`) adds the headers. When those
  cannot be read, as with another generator, the step runs whenever it is
  asked and a no-op falls to CMake's own check.
- `cmake.tests` turns the project's CTest tests into checks.

`cmake` is an adapter. The Dream VM is not built with it ("Building this
repository"), and nothing in `std.build` needs it installed until a script
imports it.

## Dream programs: `std.build.dream`

```dream
let compiler ctx seed =
    dream.program "dreams" "dreams/main.dr"
    |> dream.compiler seed          // an image, or a native program
    |> dream.vm (bin (vm ctx))      // what runs an image compiler, and the result
    |> dream.paths ["mind", "."]
    |> dream.image ctx;
```

The compiler is an **input**, so it may be an artifact, and compiling the
compiler with what it just built is a second call. A program's modules are
discovered inputs, from `dreams --depfile`, so editing a module rebuilds
exactly the images that import it. The default compiler is the one `mind`
itself was given (`--compiler`, `[build] compiler`, `$DREAMS`, the
installation).

`dream.package ctx "lucid"` builds a package directory the way `mind build`
would, with its manifest, dependencies and options. Its action runs the
`mind` that is running the build (`ctx.mind`) on that directory, so there
is one implementation of what a manifest means. `dream.run ctx image args`
is a step that runs an image with the build's VM, which is what most checks
are made of.

## Goals and checks

A package's image is what `mind build` makes when nothing else is asked
for. A plan may also name other things to make, and tests to run:

```dream
union Goal {
    make(input : Input)                     // build it, then place it in target/<profile>/<name>
    place(path : :string, input : Input)    // build it, then place it here
    each(goals : [:string])                 // several goals under one name
    effect(step : Step)                     // run it every time it is asked for
}
```

| | |
|-|-|
| `mind build [GOAL..]` | `default` if the plan declares one, else the package's image |
| `mind goals` | every goal, with the line of doc above it in the script |
| `mind run GOAL [args]` | build it, then become it (`os.replace!`), terminal and all |
| `mind test [PATTERN]` | every check whose name starts with the pattern |
| `mind test --all` | slow checks as well |
| `mind test --bless` | rewrite the expected output of every check that has one |

Goals are lazy like the rest of a plan, so asking for `vm` builds the VM and
forces nothing else. `effect` is for installing and anything else whose point
is a change outside `target/`. It is never cached and never run unless asked
for by name.

**Placing never writes a file in place.** The artifact is copied beside its
destination and renamed over it. A process that has the old file mapped,
which is exactly what a VM does with an image, keeps the pages it had rather
than taking a `SIGBUS` when the file is truncated under it. The trap in
CLAUDE.md about closing the editor before `just test` is gone with it.

A **check** is a step whose success is the test:

| | |
|-|-|
| `check.succeeds program args` | exits with 0 |
| `check.output run expected` | its output is the file `expected` |
| `check.same a b` | two artifacts are byte-identical |
| `check.agree [runs]` | several runs print the same thing |
| `check.slow c` | only under `--all` |
| `check.again n c` | run it `n` times, uncached: a flake hunt |

A check is cached like any step. One that passed with these inputs is
reported as passed without being run again, so `mind test` after a change to
`lucid` runs `lucid`'s tests and not the VM's. `--rerun` ignores that.
`check.output` is the only step that may write into the source tree, and only
under `--bless`. A package's `when test` blocks are a check `mind` adds by
itself.

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
4. Skips a step whose directory already holds a record of its key, and,
   for a step that discovers its inputs, whose recorded reads still match.
   It runs anything else in a scratch directory, then renames that into
   place. A workspace step runs in its workspace instead, and its outputs
   are copied out.
5. Resolves `given`s once their inputs are built, and goes back to 2 with
   the merged plan.
6. Writes the **outcome**: what the plan contributes, with every input
   resolved to a path, and every file that was read along with its stamp.

**File digests are cached behind a stamp**, which is git's index trick. A
file's `(size, modified)` is kept beside its digest, and the contents are
read only when the stamp has moved. Digesting in Dream would be the most
expensive thing a no-op build does, so the VM does it.

What the runner and the tools need from the VM:

| | |
|-|-|
| `io.stat!` | kind, size and modified time in one call, and `()` for nothing there. **Built** |
| `io.digest!`, `io.digest` | a file's SHA-256 and a string's. **Built** |
| `io.rename!` | renaming into place, which is atomic and never truncates. It already was |
| `io.link!` | a hard link, with a copy where there is none, for placing and for workspace outputs. **Built** |
| `io.chmod!` | an executable output. **Built** |
| `io.walk!` | every file under a directory, for `tree` and `glob`; patterns are matched in Dream. **Built** |
| `io.mkdir_all!`, `io.remove_all!` | a step's directory made, and a scratch one cleared. **Built** |
| `os.run!` | `exec!` with a working directory, an environment, output to a file, an input and a timeout, in one map of options. **Built** |

`exec!` could not be given an environment or a directory. That is the first
thing a real build hits, which is why `os.run!` came first. It also closes
every pipe on exec, so a child started while another is running cannot hold
the other's output open and make its reader wait for both.

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
flags = ["--no-types"]           # compiler flags
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

## Building this repository

The end state is a `mind.toml` and a `build.dr` at the root. They replace
the `justfile` and CMake, and shrink `build.sh` to the stage-0 bootstrap.

### Stage 0

`mind` is a Dream program, so something has to exist before it can run: a VM,
and the seed to compile `mind` with. `build.sh` makes those two and hands
over:

1. It compiles an **interpreter-only VM** with the default C++ compiler in
   one command: every file in `dream/src` but `jit.cpp`, `-O2`, and `-lffi`.
   There is no CMake and no LLVM, and a missing libffi is the one thing it
   can fail on. That is still reported as it is today.
2. It compiles `mind` with the seed, on that VM.
3. It runs `mind build`, which builds the real VM with `cc` and everything
   else with that VM. Nothing after this point uses the stage-0 VM.

On Windows the same three steps are a `build.ps1` calling `cl.exe`.

### The script

```toml
[package]
name = "dream"
version = "0.1.0"

[options]
jit      = { type = ["auto", "on", "off"], default = "auto", doc = "the LLVM JIT" }
poller   = { type = ["native", "portable"], default = "native" }
sanitize = { type = ["none", "thread", "address"], default = "none" }

[profile.tsan.config.dream]      # what `test-races` builds with
sanitize = "thread"
jit = "off"
```

```dream
// build.dr
import std.build;
import std.build.cc;
import std.build.check;
import std.build.dream;
import std.build.files;
import std.build.probe;
import std.list;

let seed = build.file "dreams/bootstrap/dreams.dream";

/// The VM's sources, less the JIT, which depends on what the machine has.
let vm_sources = ["value", "image", "heap", "cores", "gc_pool", "process",
                  "interp", "builtins", "ffi", "io", "os", "scheduler",
                  "runtime", "capi", "jit_rt"]
                 |> list.map (fn s -> "dream/src/" + s + ".cpp");

/// The VM as a library: what `dream/CMakeLists.txt` says, with the finding
/// of LLVM and libffi moved into probes. The notes on the inliner's
/// ceilings and on `-fno-semantic-interposition` come with the flags.
let libdream : build.Context -> cc.Target;
let libdream ctx =
    cc.shared "dream" vm_sources
    |> cc.standard :cxx20
    |> cc.optimize (match build.profile ctx { "debug" => 0, _ => 3 })
    |> cc.debug_info true
    |> cc.warnings :all
    |> cc.public_include "dream/include"
    |> cc.include "dream/src"
    |> cc.use (probe.library "ffi" "ffi.h")
    |> cc.use cc.threads
    |> cc.use (cc.system_for :windows "ws2_32")
    |> cc.export_all
    |> cc.when_accepted ["-fno-semantic-interposition", "-fno-plt"]
    |> cc.flags_for :gcc inliner_params                  // GCC's inliner only
    |> cc.flags_for :msvc ["/permissive-", "/utf-8", "/wd4100"]
    |> cc.define_for :msvc "_CRT_SECURE_NO_WARNINGS"
    |> cc.sanitize (build.option ctx "sanitize")
    |> jit ctx;

/// `jit.cpp` when LLVM is there, the stub when it is not or was not wanted.
/// `on` makes a missing LLVM an error rather than a slower VM.
let jit ctx t = match build.option ctx "jit" {
    "off" => cc.source "dream/src/jit_stub.cpp" t,
    want  => cc.given (probe.llvm ctx llvm_parts) (fn llvm -> match [llvm, want] {
        [(), "on"] => cc.fail "jit = on, and no llvm-config was found",
        [(), _]    => cc.source "dream/src/jit_stub.cpp",
        _          => fn t -> t
                        |> cc.source_with "dream/src/jit.cpp" [cc.rtti (probe.llvm_rtti llvm)]
                        |> cc.define "DREAM_HAVE_LLVM"
                        |> cc.use llvm,
    }) t,
};

/// `dream`, the program, over a given build of the library.
let vm_with lib = cc.executable "dream" ["dream/src/cli.cpp"]
                  |> cc.include "dream/src"
                  |> cc.use lib;

let vm ctx = vm_with (libdream ctx);

/// One stage of the bootstrap: `compiler` compiling this source.
let stage ctx compiler =
    dream.program "dreams" "dreams/main.dr"
    |> dream.compiler compiler
    |> dream.vm (cc.artifact ctx (vm ctx))
    |> dream.paths ["mind", "."]
    |> dream.image ctx;

let plan ctx = {
    let dreams = stage ctx seed;
    build.empty
    |> build.goal "vm"      (build.place "build-dream" (cc.bundle ctx (vm ctx)))
    |> build.goal "dreams"  (build.place "build/dreams.dream" dreams)
    |> build.goal "seed"    (build.place "dreams/bootstrap/dreams.dream" dreams)
    |> build.goal "mind"    (build.place "build/mind" (dream.package ctx "mind/tool"))
    |> build.goal "lucid"   (build.place "build/lucid.dream" (dream.package ctx "lucid"))
    |> build.goal "vm-pgo"  (build.place "build-pgo" (cc.bundle ctx (pgo ctx)))
    |> build.goal "default" (build.each ["vm", "dreams", "mind", "lucid"])
    |> build.check "bootstrap"      (check.same dreams (stage ctx dreams))
    |> build.check "bootstrap.seed" (check.same seed dreams)
    |> build.all [vm_checks ctx, example_checks ctx dreams, compiler_checks ctx dreams]
};
```

Four lines of it name a compiler, and they are the four that
`dream/CMakeLists.txt` wraps in `if(MSVC)` and `if(GNU)` today. The rest
builds as written with GCC, Clang, MSVC, or `--toolchain cmake`.

The helpers not shown (`inliner_params`, `pgo`, the three groups of
checks) are more of the same. `example_checks` is a `given` over
`build.glob "examples/*.dr"` that makes one `check.output` per example
against its `.expected` file, so a new example is picked up without editing
the script.

`default` includes `lucid`. It could not before, because a build of
everything was slow enough to leave out what was not needed. With a no-op
that costs a `stat` per input, it is no longer slow, and the stale language
server that follows new syntax around stops happening.

### From recipes to goals

| `just` | `mind` |
|---|---|
| `just`, `build` | `mind build` |
| `vm`, `release` | `mind build vm`, `mind build vm --release` |
| `vm-no-jit` | `mind build vm -D jit=off` |
| `vm-pgo` | `mind build vm-pgo` |
| `dreams`, `bootstrap` | `mind build dreams` |
| copying `build/dreams.dream` over the seed | `mind build seed` |
| `mind`, `lucid` | `mind build mind lucid` |
| `install`, `vscode` | `mind build install`, `mind build vscode`, as `effect` goals |
| `bench-self-compile` | `mind build bench`, an `effect` goal |
| `test` | `mind test` |
| `test-X` | `mind test X` |
| `test-all`, `fuzz`, `test-heap` | `mind test --all`, where fuzzing and the verified heap are slow checks |
| `test-races` | `mind test e2e --profile tsan` |
| `test-vscode` | a slow check that is skipped, with a line saying why, when `probe.program "npm"` finds nothing |
| `run`, `check`, `dump`, `modules`, `jit-ir` | not build goals. These are the compiler's own commands, and they stay `dreams` flags |

The test harnesses written in Python and shell stay what they are for now:
`check.succeeds (probe.program "python3") [..]`. Porting them to Dream is a
separate job and nothing here depends on it.

What this retires, each of which is written down somewhere in this
repository as a trap:

- **`build-pgo` and the cached `USE`.** Configurations are keys, and a
  profile is an artifact, so there is no build tree for a stale setting to
  survive in.
- **`SIGBUS` from a running `lucid`.** Placing renames and never truncates.
- **A stale seed that `bootstrap-check` does not notice.** `bootstrap.seed`
  compares the seed with what it builds, which is the question that check
  never asked, and `mind build seed` is the fix it points to.
- **Tests sharing `/tmp/dream-*.dream`.** Every image is in its step's own
  directory, so two runs of the suite cannot overwrite each other's.
- **`CPATH` and `LIBRARY_PATH` outside the Nix shell.** `probe.llvm` and
  `probe.library` search the way `dream/CMakeLists.txt` does now, including
  the Nix store, and hand the paths to the steps that need them.

### What happens to CMake

The full `dream/CMakeLists.txt` stays until `cc` builds the VM on Linux,
macOS and Windows, the `just test` suite passes against that VM, and `cc`
writes a `compile_commands.json` for editors. Then it is cut down to what
an **embedder** needs, and `mind build` is the build.

What an embedder needs is small: `add_subdirectory(dream)` or
`find_package(dream)` gives a `dream` library target with the public header,
built from the sources and linked against libffi and threads. That file is
**generated**: `mind build embed-cmake` runs `cc.cmake_export` on the
interpreter-only VM (`vm (build.with_option "jit" "off" ctx)`) and writes
`dream/CMakeLists.txt`, with a header saying it is generated and which
`mind` goal makes it. There is no second description of the build to keep
in step, because there is only one. `embed.cmake`, a slow check, generates
the file, configures it and builds it, so a change that breaks the export is
a failing test rather than an embedder's bug report. `bootstrap.cmake-current`
checks that the file in the tree is the one the goal would write.

The `justfile` shrinks to one line per recipe (`vm: mind build vm`) while
habits catch up, and then goes too.

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
2. **VM primitives. Built**, but for `dreams --depfile`: the table under
   "Running a plan".
3. **`std.build` core. Built, sequentially** (`mind/std/build/mod.dr`):
   contexts, inputs, steps and their identities, keys and results, the
   stamp cache, discovered inputs, scratch directories renamed into place,
   `write` and `command`; and `std.build.cc.steps`, which makes a `cc` target
   a compile step per source and a link step. `just test-build` builds a
   library and a program through it and checks, change by change, that
   exactly the right steps rerun, with several steps running at once. `just
   vm-cc` builds the VM this way, JIT included, from
   `mind/std/build/tests/vm.dr`: 21 steps in 21 s on 24 jobs against 25.6 s
   for a clean CMake build, no warnings, 203 ms for a build with nothing to
   do, and the result passes `dream_tests` and every e2e program. What is
   left: workspaces, `cache = :local`, probes as steps, plans with goals and
   checks, and `given`.
4. **The graph. Built.** Cycle detection with the loop in the message, version
   requirements and `std.version`, revision conflicts reported as such,
   three kinds of edge, and `mind.lock`. None of this needs build scripts,
   and every project with dependencies wants it now.
5. **Options and profiles. Built.** `[options]`, `[config.KEY]`, scoped
   `-D KEY:name=value`, conditions evaluated per package, dotted names in
   `when`, conditional dependencies, `[profile.*]`, `[build] target`, and
   `mind options`. What is left for item 6 is `ctx.options` and a script's
   `build.define`, since there are no scripts to hand them to yet.
6. **`mind` runs scripts.** Script discovery, `[build-dependencies]`,
   drivers, outcome reuse, folding outcomes into the compile, `--target` and
   `-j`, and the goal and check commands (`mind build GOAL`, `mind goals`,
   `mind test PATTERN`).
7. **Tools.** `probe`, then `cc`, then `dream` and `check`. `cc`'s
   vocabulary and its translations for GCC, Clang, MSVC and CMake are
   **built** and tested (`std.build.cc`, `.gnu`, `.msvc`, `.cmake`, and `just
   test-build`); what is left of it is the steps that run them, which need
   item 3. `dream/tests/ffi` is rebuilt as a package whose C library is made
   by its own `build.dr` rather than by the test script. That is the first
   real user, and it is small.
8. **This repository.** Stage 0 in `build.sh`, the root `build.dr` with the
   VM built by `cc`, then the goals and the checks group by group, each
   checked against its `just` recipe until the two agree. Then PGO, then
   `cc.msvc` and macOS, then CMake is cut down to the embedders' file and
   most of the `justfile` is removed.
9. **`cmake`**, when something wants to vendor a CMake project. Nothing in
   this repository does.
10. **The single driver**, and the shared cache as a default.

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
- **The default compiler, not CMake, builds C.** `cc` finds whatever the
  machine's compiler is, and a toolchain is a behavior anyone can add.
  CMake is an adapter for projects that already have a `CMakeLists.txt`.
  A build that needed CMake to build C would need it installed to build
  Dream, and would describe the VM in a language `mind` cannot see into:
  no keys, no shared objects, no discovered headers.
- **Plans stay abstract and actions translate.** `cc.optimize 3` becomes
  `-O3` or `/O2` when the step runs. That keeps ordinary C builds free of
  `given`, so their graph is known before anything has run.

Open:

- **Jobs inside a step.** `cores = :all` is blunt. A step running `make` or
  `ninja` could take part in a GNU make jobserver instead, and give tokens
  back as its own jobs finish.
- **`dream.package` runs `mind`.** That keeps one implementation of what a
  manifest means, at the cost of a process per package. The alternative is
  moving the graph out of `mind/tool/build.dr` into `std.build.graph`, which
  item 10 wants anyway.
- **Several programs in one project.** Goals cover this repository, since
  its programs are listed in one script. Whether a `[workspace]` of member
  packages is still worth having, so that `mind build` inside `lucid/`
  knows it is part of something larger, can wait until goals have been used.
- **Stage 0 on Windows**, and whether a `build.ps1` or the stage-0 command
  written into the seed as a Dream program is the better answer.
