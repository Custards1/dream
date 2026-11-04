# dre

The Dream VM, written in Dream.

```
dre IMAGE [ARGS..]
```

runs a compiled image as `dream IMAGE` does. It runs on whatever runs it: the VM,
or itself built with `dreams --aot`. The second is the point. A native program
that can run images is what a native compiler needs for `comp` and macros, which
evaluate code at compile time, and `dreams --aot` refuses the compiler today for
exactly the `std.vm` natives that do that (`eval_image!`, `open_image!`,
`call_image!`).

## How it works

[src/run.dr](src/run.dr) is the machine. An image's functions are compiled,
once each and when first reached, into closures of the language dre is written
in: a node becomes a function from the frame it runs in to its value, and its
operands are found then rather than every time it runs.

The program's values are dre's own values. An integer, a string, a list, a map,
a record, an error and a process are each the thing itself, so a builtin or a
native handed one is the one the machine running dre has, and `spawn!`,
`send!`, `try!`, laziness and the collector come with them. A function of the
image is a function here too, of the same arity, so a native that calls one --
`spawn!`, whose argument is a thunk -- can. What dre adds is only what an image
says and a value does not: frames, captures, globals, and which statements run
and in what order (the image's STRICT flag, kept by writing them as statements
of an impure block).

Two tables connect it to the machine underneath:

- `std.builtins` -- every builtin as a value, by ID, in the compiler's order.
- [src/natives.dr](src/natives.dr) -- every member of the VM's own modules,
  generated from what the VM says it has (`vm.host_members`) by
  [tools/natives.dr](tools/natives.dr).

An image's atoms are made atoms by `vm.intern_atom!`, which is what loading an
image does and the one thing a program could not do for itself.

## Testing

```
dre/tests/programs.sh [PATTERN]     # every recorded program, run by dre
```

compiles each program in dream/tests/programs and examples/ with the compiler
and holds what dre prints to what is recorded. `DRE=` names a dre to test
(an image, or a native build).

## Where it stands

95 of the 101 recorded programs print what the VM prints. What is left:

- **A process's copy of the program.** A `spawn!` copies what its work reaches,
  and a closure dre compiled reaches the program's tables, so every process
  carries a copy of the compiled program and of the globals it has forced.
  The image's bytes are shared (`vm.share!`); the closures cannot be.
  `census` (80,000 processes) runs out of memory, and `caf_race` does too,
  because each child's answer holds a global of its own copy and the parent
  forces each one. The VM keeps one global per runtime.
- **Error traces** name dre's functions, not the image's
  (`root_failure_ends_run`).
- **Speed.** Not measured yet; no JIT, by design.
