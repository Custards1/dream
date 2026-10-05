# Dream by example

Each program here is a short tour of one part of the language, written to be
read top to bottom, with comments saying why as well as what. Each is also a
test: [`run.sh`](run.sh) compiles and runs it and compares the output with the
`.expected` file beside it, so a change that breaks one fails the build, and
what you read here is what the language does today.

```
just test-examples            # compile and run them all, checking output
examples/run.sh strings       # only the ones whose name matches
just examples-bless           # re-record the output after a deliberate change
```

To run one yourself:

```
just run examples/02_laziness.dr
```

## The tour

Read them in order the first time; each assumes the ones before it.

| | |
|---|---|
| [`01_basics.dr`](01_basics.dr) | literals, currying, the precedence ladder, `\|>` |
| [`02_laziness.dr`](02_laziness.dr) | what is *not* evaluated; infinite lists; thunks |
| [`03_collections.dr`](03_collections.dr) | lists, arrays and maps, and what each is for |
| [`04_strings.dr`](04_strings.dr) | `std.str`, and keeping characters apart from bytes |
| [`05_errors.dr`](05_errors.dr) | `raise!`, `try! .. catch`, and failure inside a process |
| [`06_processes.dr`](06_processes.dr) | `spawn!`, `send!`, `recv!`, `join!`, and isolation |
| [`07_modules.dr`](07_modules.dr) | `mod`, and every form of `import` |
| [`08_generic.dr`](08_generic.dr) | `virtual` and `derive`: generic code with no run-time cost |
| [`09_compile_time.dr`](09_compile_time.dr) | `comp`, `comp!`, and `when` |
| [`10_macros_records.dr`](10_macros_records.dr) | syntax macros, and the functions a record declaration generates |
| [`11_standard_macros.dr`](11_standard_macros.dr) | `std.macros`: branches, bindings, pipelines, updates and effects |
| [`12_behaviors.dr`](12_behaviors.dr) | behaviour contracts, record implementations, defaults and generic callers |
| [`13_codecs.dr`](13_codecs.dr) | `std.codec`: one walk over a value, two formats, framing, records on a wire |
| [`14_servers.dr`](14_servers.dr) | `std.server`: a process that holds state, its handlers tested without one, and supervision |
| [`15_remote.dr`](15_remote.dr) | `std.remote`: the same server over a socket, client and server in one program |
| [`16_registry.dr`](16_registry.dr) | `std.registry`: a name for a server, so a supervisor and a listener compose |
| [`17_tensors.dr`](17_tensors.dr) | `std.tensor`: elementwise arithmetic, `@`, fusion, and the GPU when there is one |
| [`18_time.dr`](18_time.dr) | `std.time`: instants and durations as integers, zones, calendar arithmetic, formats |
| [`19_text.dr`](19_text.dr) | `std.parse` and `std.regex`: grammars from functions, patterns checked while compiling |
| [`20_random.dr`](20_random.dr) | `std.random` and `std.property`: replayable generators, and counterexamples that shrink |
| [`21_http.dr`](21_http.dr) | `std.http`: a router, a server and a client in one program |
| [`22_ml.dr`](22_ml.dr) | `std.ml`: a classifier and a regression trained from scratch, a hand-written gradient, parameters as data |

## The packages

A package is any directory here with a `mind.toml`; its entry is `main.dr` and
its expected output is `<dir>.expected`.

**[`example/`](example)** is the smallest package there is, a manifest and a
`main!` -- what `mind new` makes.

**[`textstats/`](textstats)** is a word-frequency tool, split across files the
way a real one would be:

```
textstats/
  mind.toml          the manifest: this directory is a package named `textstats`
  tokenize.dr        text -> words
  counter.dr         words -> a tally, over a map
  report.dr          a GENERIC report: declares `virtual let row`, and writes
                     `render`, `body`, `width` and `pad` in terms of it
  report_plain.dr    `derive textstats.report` -- fills the holes one way
  report_bars.dr     `derive textstats.report` -- fills them another
  stats/mod.dr       a module that grew into a directory; importers do not care
  main.dr            wires them together
```

`report_plain` and `report_bars` share every line of their rendering logic and
print completely different output, because `derive` specializes the base
module against each one at compile time.

**[`wordfreq/`](wordfreq)** depends on `textstats`:

```toml
[dependencies]
textstats = { path = "../textstats" }
```

It shows two things a single package cannot: that a dependency is reached by
its name and not by where it sits on disk, and that `wordfreq.report` and
`textstats.report` are different modules that never collide.

The packages carry their own tests, including a check that the two derived
reporters really do render differently from the same code:

```
just test-examples-std
```
