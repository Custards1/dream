# Documentation

Two kinds of document live here. The **references** say what is true now and
are kept current. The **designs** are a plan plus a log of how far it has got,
and are the first thing to read before working on what they describe.

## The language

| | |
|---|---|
| [language-spec.md](language-spec.md) | **The language**, start to finish: syntax, values, laziness, purity, patterns, errors, types, records and unions, modules, behaviours, processes, compile-time evaluation, C libraries, tests, and writing code that runs fast. Start at "A first program". |
| [builtins.md](builtins.md) | Every builtin and every native module's members, with their types. |
| [console.md](console.md) | `std.console`: output, prompts, formatting, colour and logging. |
| [ffi.md](ffi.md) | Calling C: `std.ffi`, `std.foreign`, and `foreign` declarations. |
| [build.md](build.md) | Build scripts and `std.build`: plans, steps, caching, goals, checks, and C/C++ with any compiler. |
| [platforms.md](platforms.md) | What an image may assume about where it runs, and what stays platform-specific. |

## The machinery

| | |
|---|---|
| [bytecode-format.md](bytecode-format.md) | The `.dream` image, as it actually is: the header, every section, the target bits. |
| [gc.md](gc.md) | The collector: generational, parallel and concurrent, the rules a native must follow, and what was measured. Read before touching `heap.cpp`. |
| [dynamic-linking.md](dynamic-linking.md) | The plan for one runtime holding many images, and how far it has got. |
| [session-mode.md](session-mode.md), [session-work.md](session-work.md) | Incremental compilation for the REPL: the design, and where the work stands. |
| [bytecode-format-proposed.md](bytecode-format-proposed.md) | Format changes that have since landed in `bytecode-format.md`; kept for the reasoning. |

## The notes

[notes/](notes/README.md) is the design and performance log: what was
measured, what was kept, what was tried and thrown away, and why. Comments in
the code point into it by section title (`see "Some title"`), and it is where
the traps are written down.
