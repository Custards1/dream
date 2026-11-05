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

## Native

```
dream build/dreams.dream -L mind -L . -L dre=dre -o dre.dream dre/src/main.dr
dream build/dreams.dream --aot -L mind -o dre dre.dream
DRE=$PWD/dre dre/tests/programs.sh
```

dre builds with `dreams --aot`, and a dre built so is a VM with no C++ in its
evaluator. The native runtime makes atoms at run time for it (`vm_intern_atom!`
in mind/std/rt/main.dr) and works out a run-time record's written name and type
from its atom's name the first time one is made, as the VM does.

## What is shared

The compiled program -- every function's closures, the image and the tables -- is made
once when the image is loaded and never changes, and `spawn!` copies what a process's
work can reach, which through any closure of it was all of it. So it is compiled whole
at load and moved where every process reads it (`vm.share!`): a spawn copies a pointer
to it. What changes stays each process's own: frames, function values made while
running, and the program's top-level values, which live in the frame's `world`, one
table per process, as the VM keeps a process's globals apart.

For that to be possible nothing the program holds may be a suspension, so every
closure the compiler makes is made by a builder with strict parameters.

## What may perform an effect

A node's closure is `fn!` when the node may perform an effect, and a plain `fn` when it
cannot -- what the image says of the node (its IMPURE flag), and one thing more: an
application of a function not known until it runs may perform one whatever the image
says (`list.fold` is pure; the function it is handed need not be), unless it is a call of
a function of the image that cannot (worked out for every function at load, as a fixpoint
over the calls they make by name), or of a builtin or a native without a `!`. A node
holding such a child is one too, since its closure suspends the child's.

It matters because the VM's JIT compiles pure closures, and compiled code that parks is
run again from the top, making its suspensions again: a pure closure that suspended a
child performing an effect performed it twice (`fold_effects`, `jit_fold_park`). An
interpreted function's value is an `fn!` exactly when the VM would call it impure -- its
name's `!`, or a body that performs an effect -- so `type_of` answers what the VM does.

## Where it stands

On the VM, 98 of the 101 recorded programs print what the VM prints; built native, 98 of
the 100 the native runtime can run (`share_release` asks about an area it does not have).
What is left:

- **A suspension holds the frame it was made in**, and so everything the frame holds,
  where the VM's holds only the values it reads: pure Dream cannot move a slot's value
  into another structure without forcing it or suspending the read. A pattern's `x` in
  `list.map`'s `[x, ..rest]` is a suspension holding `list.map`'s frame, and through it
  the rest of the list; a process started with it carries the list too, and a fold over a
  generated list keeps all of it. `census` (80,000 processes) takes 6.9 GB on the VM,
  against the VM's 0.6 GB, and each `caf_race` process about 170 MB for a sum over
  300,000 numbers, more than the VM allows a heap when sixty-four run at once. What
  is done already: a closure's captures hold its parent's slot arrays rather than the
  parent's frame, and a process `spawn!` starts on a thunk makes a world of its own,
  of the program's values only, as a process the VM starts does.
- **Error traces** name dre's functions, not the image's (`root_failure_ends_run`).
- **Speed.** Native dre is about 9x slower than the VM on `benchmark/native/tree`.
