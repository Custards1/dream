# Documentation

A document lives with the thing it describes, in that thing's repository: the
VM's in [`dream/docs/`](../dream/docs), the compiler's and the language
specification in [`dreams/docs/`](../dreams/docs), the standard library's in
[`mind/std/docs/`](../mind/std/docs), the language server's in
[`lucid/docs/`](../lucid/docs). This directory is the index of all of them.

Two kinds of document are kept. The **references** say what is true now and
are kept current. The **designs** are a plan plus a log of how far it has got,
and are the first thing to read before working on what they describe.

## The language — `dreams/docs/`

| | |
|---|---|
| [language-spec.md](../dreams/docs/language-spec.md) | **The language**, start to finish: syntax, values, laziness, purity, patterns, errors, types, records and unions, modules, behaviours, processes, compile-time evaluation, C libraries, tests, and writing code that runs fast. Start at "A first program". |
| [session-mode.md](../dreams/docs/session-mode.md), [session-work.md](../dreams/docs/session-work.md) | Retained implementation of the deprecated REPL: incremental compilation and design. |

For learning it rather than looking things up, read [Learn Dream](../learn/README.md).

## The machine — `dream/docs/`

| | |
|---|---|
| [builtins.md](../dream/docs/builtins.md) | Every builtin and every native module's members, with their types. |
| [bytecode-format.md](../dream/docs/bytecode-format.md) | The `.dream` image, as it actually is: the header, every section, the target bits. |
| [gc.md](../dream/docs/gc.md) | The collector: generational, parallel and concurrent, the rules a native must follow, and what was measured. Read before touching `heap.cpp`. |
| [ffi.md](../dream/docs/ffi.md) | Calling C: `std.ffi`, `std.foreign`, and `foreign` declarations. |
| [images.md](../dream/docs/images.md) | Calling another Dream image: `std.image`, and `image` declarations. |
| [platforms.md](../dream/docs/platforms.md) | What an image may assume about where it runs, and what stays platform-specific. |
| [security.md](../dream/docs/security.md) | What the VM does and does not protect against. |
| [dynamic-linking.md](../dream/docs/dynamic-linking.md) | The plan for one runtime holding many images, and how far it has got. |
| [bytecode-format-proposed.md](../dream/docs/bytecode-format-proposed.md) | Format changes that have since landed in `bytecode-format.md`; kept for the reasoning. |

## The library and the tools — `mind/std/docs/`, `mind/tool/`

| | |
|---|---|
| [console.md](../mind/std/docs/console.md) | `std.console`: output, prompts, formatting, colour and logging. |
| [binary.md](../mind/std/docs/binary.md) | `std.binary`: custom binary layouts, integers, floats, bit fields, bounds, and IO. |
| [build.md](../mind/std/docs/build.md) | Build scripts and `std.build`: plans, steps, caching, goals, checks, and C/C++ with any compiler. |
| [Generating documentation](../mind/tool/DOCUMENTATION.md) | `mind docs` and `mind serve`: guides, source comments, examples with captured output. |

## The notes

[notes/](notes/README.md) is the design and performance log: what was
measured, what was kept, what was tried and thrown away, and why. Each note
lives with its component too, in its `docs/notes/`, and the index lists every one by
section title, since comments in the code point into them that way (`see
"Some title"`).
