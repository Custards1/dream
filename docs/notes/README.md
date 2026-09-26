# Design notes

The long-form record behind `CLAUDE.md`: what was measured, what was built, what was tried and thrown away, and why. Each file is a log, not a manual. Its numbers are as of the date beside them, and a later section can overturn an earlier one (they say so where they do). Read the relevant file before changing the thing it describes; that is where the traps are written down.

Code comments and `dreams/TODO.md` refer to these sections by their titles, which are listed here so a title can be found by searching this page.

## [Making the VM faster](vm-performance.md)

The interpreter, the collector, the scheduler and the JIT: what was measured, what was kept, and what the measurements taught. Newest findings are nearer the end of each part.

- Making it faster
- A spurious deadlock, and why it hid behind a slow walk
- The tier's eager arguments can change *which* error a program raises
- Two collector bugs that presented as a segfault a long way from home
- Fixed: compiled code forcing a long thunk chain crashed
- Measured, and not kept
- Two things that will lie to you about a change to the interpreter
- Where the remaining time and memory are, and the plan
- Where it stands against CPython, and what the remaining gap is made of
- List and map code in the JIT
- A JIT that can allocate -- the plan
- Spilling compiled frames: measured, and not kept

## [Compiling in parallel](parallel-compile.md)

Spreading a self-compile across processes and cores, and what it does on fewer of them.

- The self-compile on four cores: 8.8 s -> 5.8 s, and what 3 s would take
- Resolving, lowering and checking in parts: 5.75 s -> 3.7 s
- Fewer cores than the machine has: 15.6 s -> 9.1 s on one

## [Compiling large programs](compiler-scaling.md)

The quadratics, and the memory held by unforced values, that decided how large a program `dreams` can compile.

- The compiler was quadratic in the size of the program
- A table indexed by module wants to be a map, not a list
- The three lists the loader still grew one entry at a time
- Sharing the arena
- A lazy value stored in a map pins the map it was made in
- The arena was a chain of its own versions, and `push_node` never saw it

## [What macro expansion costs](macro-expansion.md)

Expansion is a compile: the rounds of work that took its cost from per-call to per-program, and the phase meter that guided them.

- Expanding a macro is a compile, and there used to be one per call
- A compile-time expression is the program with a different entry
- One image per round of the whole program
- A macro reaches a handful of declarations, not the program
- What a macro call actually costs, phase by phase
- The stub every unreached declaration shares
- Declaring a program was mostly a search for the word `core`
- The image was the calls, and now it is the transformers
- Discovery was a walk of two whole modules, because a `match` lied about where it ended
- One image for the whole expansion, and what it cost to buy that
- `mind/std/all.dr --test` is not byte-stable across compiler changes

## [Deforestation](deforestation.md)

How a pipeline of `std.list` combinators becomes one loop the JIT can take.


## [Static types](static-types.md)

The checker, compile-time contracts, and what a signature buys the JIT.

- Compile-time contracts: a refinement run against a value the compiler has
- What a signature buys the compiled code

## [The language server](lucid.md)

How `lucid` answers an editor, and which tree answers which question.

- Completion is asked of something that is not a program
- Two trees, and which question goes to which
- A record is a declaration before it is a module
- A host module's members come from the host

## [Large data in an image](large-data.md)

The payload: data past the 4 GiB line, reached as views rather than copies.

