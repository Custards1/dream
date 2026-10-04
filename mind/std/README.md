# std, the Dream standard library

Every Dream program can import `std`. `mind` puts it on the search path
(`$MIND_STDLIB`, or the installation's copy); calling the compiler directly,
pass `-L` with the directory that holds it (`-L mind` in this checkout). It
has two layers:

- **Native modules**, written in C++ in the [VM](../../dream/README.md#native-modules):
  `std.io`, `std.os`, `std.net`, `std.tls`, `std.ffi`, `std.math`,
  `std.crypto`, `std.tensor`, `std.vm`. These are the edges of the machine:
  handles and bytes, processes, sockets, C.
- **Dream modules**, here, written on top of them and compiled like any
  other package.

[docs/builtins.md](../../docs/builtins.md) is the reference for both.

## Use std, and do not repeat it

The language's primitives are builtins spelled with a leading underscore --
`_list_cons`, `_str_slice`, `_data_at` -- and they are this library's to call.
Everything else calls the function here that wraps each one (`list.cons`,
`str.slice`, `payload.at`), which compiles to the same opcode; naming a
primitive outside `std` is a warning. The same goes for shapes: a result is
`[:ok, v]` or `[:error, why]` with `std.result` to handle it, a long-lived
process is a `std.server`, an option parser is `std.cli`. A program that
grows its own version of one of these is a program the next reader has to
learn.

## The modules

**Data**

| | |
|---|---|
| `list` | lists, lazy in head and tail; the combinators deforestation fuses into loops |
| `array` | flat sequences with constant-time indexing, `#[a, b, c]` |
| `map` | hash tries: `%{ k => v }` and the operations they make cheap |
| `str` | UTF-8 strings, character by character |
| `char`, `atom`, `num` | characters and code points, atoms by name, numbers to and from text |
| `seq` | the generic half: everything a container gets once it can `fold` |
| `dynamic` | the sequence operations, choosing list, array or string at run time |
| `result` | `[:ok, value]` and `[:error, reason]`, and what to do with them |
| `error` | an error's kind and payload, and errors of your own |
| `types` | optional type descriptions as values: `accepts`, `check`, `enforce` |

**Formats**

| | |
|---|---|
| `codec` | the generic half: everything a format gets once it can spell a value |
| `json` | JSON, parsed and rendered |
| `wire` | every Dream value, exactly, as bytes |
| `record` | records on a wire: a declared value's named form, and the way back |
| `toml` | the TOML `mind.toml` uses |
| `version` | versions, and which versions a requirement accepts (`^1.2`, `>=0.3, <0.5`) |

**Processes**

| | |
|---|---|
| `proc` | the shapes `spawn!`, `send!`, `recv!` and `join!` leave to the caller |
| `server` | a process that holds state and answers questions -- a `gen_server` |
| `agent` | a server whose whole job is to hold one value |
| `supervisor` | children started together and restarted when they die |
| `registry` | a name that means whichever process answers to it now |
| `remote` | the same server, over a socket |

**The outside world**

| | |
|---|---|
| `console` | printing, prompts, formatting, colour and logging, over `std.io` |
| `file` | whole files, and the directories they go in |
| `streams` | writing all of a string through a writer that may take less |
| `cli` | command lines: one description of an option, read, checked and printed as usage |
| `foreign` | wrapping a C library on `std.ffi`; what the `foreign` declaration builds on ([docs/ffi.md](../../docs/ffi.md)) |
| `payload` | the files an image carries (`dreams --payload`) |
| `sql` | SQL for any database: fragments, query builders, dialects, a pool, and SQLite |
| `sql.pg` | a PostgreSQL client in pure Dream ([README](sql/pg/README.md)) |

**Writing programs**

| | |
|---|---|
| `test` | the test framework: each case in its own process |
| `macros` | syntax conveniences called with `expand macros.name ...` |
| `debug` | where the time goes and where the memory goes, in a running program |
| `build` | what a package's `build.dr` is written in; `build.cc` builds C and C++ with GCC, Clang, MSVC or CMake ([docs/build.md](../../docs/build.md)) |
| `all` | every module, so that `--test` collects every module's tests |

## Tests

A module's tests live in it, in a `when test` block that compiles only under
`--test`:

```dream
when test {
    import std.test;

    let tests = [
        test.case "ok"    $( test.true! (is_ok (ok 1)) ),
        test.case "error" $( test.true! (is_error (error :e)) ),
    ];
}
```

```
just test-std         # every module's, through all.dr
just test-build       # std.build.cc against GCC, Clang and CMake, and the runner
just test-pg          # the PostgreSQL client
mind test std         # all of it at once
```

A new module goes in `all.dr` too, or its tests are never run.
