# Dream examples

Every program here is compiled and run by [`run.sh`](run.sh), and its output is
checked against the `.expected` file beside it. They are tests, not sketches: a
change that breaks one fails the build.

```
just test-examples          # compile and run them all, checking output
examples/run.sh strings     # just the ones whose name matches
examples/run.sh --bless     # re-record output after a deliberate change
```

## The single-file programs

| | |
|-|-|
| [`01_basics.dr`](01_basics.dr) | literals, currying, the precedence ladder, `\|>` |
| [`02_laziness.dr`](02_laziness.dr) | what is *not* evaluated; infinite lists; thunks |
| [`03_collections.dr`](03_collections.dr) | lists, arrays and maps, and what each is for |
| [`04_strings.dr`](04_strings.dr) | `std.str`, and keeping characters apart from bytes |
| [`05_errors.dr`](05_errors.dr) | `raise!`, `try! .. catch`, and failure inside a process |
| [`06_processes.dr`](06_processes.dr) | `spawn!`/`send!`/`recv!`/`join!`, and isolation |
| [`07_modules.dr`](07_modules.dr) | `mod`, and all four forms of `import` |
| [`08_generic.dr`](08_generic.dr) | `virtual` and `derive`: generic code with no run-time cost |
| [`09_compile_time.dr`](09_compile_time.dr) | `comp`, `comp!`, and `when` |

## The packages

Two packages, because one package cannot show what a package is *for*.

**[`textstats/`](textstats)** — a word-frequency tool, split across files the
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
produce completely different output, because `derive` specializes the base
module's syntax tree against each one at compile time.

**[`wordfreq/`](wordfreq)** — a second package that *depends* on the first:

```toml
[dependencies]
textstats = { path = "../textstats" }
```

It exists to show two things a single package cannot. First, that a dependency
is reached by the name it gives itself, not by where it sits on disk. Second,
that `wordfreq.report` and `textstats.report` are different modules that never
collide — a package's own modules are reachable unqualified, another's need the
package name.

Both packages carry their own tests:

```
just test-examples-std
```

which runs the `tests` each module exports, including checks that the two
derived reporters really do render differently from the same shared code.
