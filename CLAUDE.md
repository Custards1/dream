# Dream

A lazily evaluated functional language. The repository holds a language, its
runtime, and — in progress — its self-hosted compiler.

## The pieces

| Directory | What it is | Written in |
|---|---|---|
| `dreamc/` | The reference compiler. `.dr` source to `.dream` bytecode. | Rust |
| `dream/` | The VM: interpreter, green processes, LLVM JIT. | C++ |
| `mind/tool/` | `mind`, the build tool. Finds packages, shells out to a compiler. | Dream |
| `mind/std/` | The standard library. | Dream |
| `dreams/` | The self-hosted compiler. **The active work.** | Dream |
| `examples/` | Example programs, each with its output recorded beside it. | Dream |
| `docs/` | `language-spec.md`, `builtins.md`. | — |

## Where this is going

`dreams` is replacing `dreamc`. The plan, in order:

1. `dreams` reaches parity with `dreamc` — every stage agrees on the whole corpus.
2. An old `dreams` build bootstraps the new one, and `dreamc` drops out.
3. `mind` moves into `dreams`: `dreams` grows a CLI in `mind`'s shape (project
   commands, not just file-at-a-time flags) and takes over its role.

`dreams` is complete through the front end and lowering, and its image writer
works: `dreams/tests/compile.sh` compiles nine examples with `dreams` and runs
them. What is not done is `comp` (compile-time evaluation), which needs a VM to
evaluate on.

`mind` finds its compiler through the `DREAMC` environment variable
([mind/tool/build.dr:114](mind/tool/build.dr#L114)), defaulting to `dreamc` on
the path. That indirection is the seam the migration goes through — point it at
a `dreams` image and `mind` builds with the self-hosted compiler.

## Building

```
just              # both halves: cargo build -p dreamc, then cmake the VM
just compiler     # dreamc only
just vm           # the VM only
just mind         # build/mind, the build tool
```

The binaries that matter:

- `target/debug/dreamc` — the reference compiler
- `build-dream/bin/dream` — the VM

`build/` is a leftover CMake tree and is not the VM build directory —
`build-dream/` is. The only thing `build/` is used for now is `build/mind`,
which `just mind` writes. Do not reach for a `dream` binary under `build/`.

`./build.sh` builds from a clean checkout and reports what optional dependencies
are missing (LLVM gives the JIT, libffi gives `std.ffi`; neither is required).

## Testing

`just test` runs everything. The groups, and what each one is actually asking:

| Recipe | Question |
|---|---|
| `test-compiler` | `cargo test -p dreamc` |
| `test-vm` | The VM's own C++ checks (heap, images, atoms) |
| `test-e2e` | Real programs under both interpreter and JIT, which must agree |
| `test-examples` | Every example, output compared against what is recorded beside it |
| `test-std` | The standard library's `when test` blocks |
| `test-mind` | `mind`'s path handling, manifests, dependency specs |
| `test-dreams` | Every `when test` block `dreams/main.dr` reaches |
| `test-dreams-corpus` | Every `.dr` file in the repository parses |
| `test-dreams-modules` | `dreams`'s module loader resolves as `dreamc` does |
| `test-dreams-scope` | `dreams`'s resolution and purity agree, verdict by verdict |
| `test-dreams-lower` | Everything `dreamc` accepts also lowers |
| `test-dreams-compile` | Programs `dreams` compiled, run, output compared |

The four `dreams/tests/*.sh` scripts default to `target/debug/dreamc` and
`build-dream/bin/dream`, so they run directly with no environment set. Override
with `dreamc=... dream=... dreams/tests/scope.sh`.

`just test-all` adds fuzzing, a heap-verified run, and a no-JIT build.

## Running things

```
just run FILE [args]   # compile and run
just check FILE        # scope- and purity-check, no image
just dump FILE         # the execution trees it compiles to
just modules FILE      # what it pulls in
```

Always put a timeout on a VM run. A Dream program that diverges does not stop on
its own, and the VM will happily sit there.

## The language, briefly

- A `!` suffix marks an impure name. Purity is checked: a pure function cannot
  call an impure one. This is enforced across imports.
- `$( expr )` is a thunk.
- Evaluation is lazy; `strict!` forces.
- `when test { .. }` holds a module's tests, collected by compiling with
  `--test`. Tests are `test.case "name" $( test.eq! expected actual )`.
- Modules are files; `mod name { .. }` writes one inside another. `import a.{x}`
  and `import a.{x as y}` bring members in.
- Compilation is whole-program, which is why a build is just "find the packages,
  hand them to the compiler, run it".

## Conventions

Comments in this codebase explain *why a thing is the way it is*, usually in
full sentences, often several lines at the head of a section. Match that. The
existing prose is the style guide — read the top of
[dreams/emit.dr](dreams/emit.dr) or [dreams/scope.dr](dreams/scope.dr) before
adding to either.

Keep the build warning-free. A stray warning in the output reads as a failure.
