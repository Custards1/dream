# Design notes

The long-form record of how Dream got the way it is: what was measured, what
was built, what was tried and thrown away, and why. Each file is a log, not a
manual. Its numbers are as of the date beside them, and a later section can
overturn an earlier one (it says so where it does). Read the file that
describes a thing before changing it; that is where the traps are written
down.

Each note lives with the component it describes: the VM's in
[`dream/docs/notes/`](../../dream/docs/notes), the compiler's in
[`dreams/docs/notes/`](../../dreams/docs/notes), the library's in
[`mind/std/docs/notes/`](../../mind/std/docs/notes) and the language server's
in [`lucid/docs/notes/`](../../lucid/docs/notes). This is their index. Code comments and `dreams/TODO.md` point into them by
section title (`see "Some title"`). Every title is listed below, so searching
this page finds the file.

**Before believing a measurement**, read "Two things that will lie to you
about a change to the interpreter" in vm-performance.md. The short version:
A/B against the commit before, in a worktree, with the same VM; 3% is noise
for an edit to the interpreter loop; and never compare against a number
written in a note, since the compiler it was measured on has since grown.

## [Making the VM faster](../../dream/docs/notes/vm-performance.md)

The interpreter, the collector, the scheduler and the JIT: what was measured,
what was kept, and what the measurements taught. Newer findings are nearer the
end.

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
- Building ahead: a producer makes a run of cells a call
- Frames freed on return: the most it could save
- Strict bindings are loads
- Reusing the frame of a self tail call: measured, and not built
- A JIT that can allocate -- the plan, and how it was done instead
- Spilling compiled frames: measured, and not kept
- Strict arguments are evaluated where they stand
- A pinned heap grew past its cap
- Strict by default: the experiment
- The timeline, and what a parallel compile was waiting on

The collector has a document of its own, [../gc.md](../../dream/docs/gc.md), and "Collecting under compiled code" there is where the JIT and the collector meet.

## [Results became a union](../../dreams/docs/notes/results.md)

`[:ok, v]` to the prelude's `Result`: the prelude, why `let?` has no bridge, the
image flag and the two-step reseed it forced, and what a span-exact rewrite
still got wrong.

- The prelude
- No bridge
- Natives, and why the seed needed two steps
- The rewrite, and what it could not tell
- What is still a tagged list, and why

## [Bignums, and the cryptography beside them](../../dream/docs/notes/bignums.md)

Unbounded integers at no cost to fixnum code, why their limbs live outside the
heap like a GPU tensor's numbers, and `std.crypto` as natives.

- Integers past 63 bits (2026-10-03)
- What it costs a program that never makes one: nothing measurable
- What it costs a program that does: CPython's ballpark, and ahead on products
- The first version was 6x slower on `fib`, and the heap was why
- Literals
- Semantics that changed
- `std.crypto` (2026-10-03)

## [Compiling in parallel](../../dreams/docs/notes/parallel-compile.md)

Spreading a build across processes and cores, what it does on fewer of them,
and the compile units that let a build redo only what changed.

- The self-compile on four cores: 8.8 s -> 5.8 s, and what 3 s would take
- Resolving, lowering and checking in parts: 5.75 s -> 3.7 s
- Fewer cores than the machine has: 15.6 s -> 9.1 s on one
- Compile units: a module's walk and lowering, kept between builds
- `--time` timed a pipeline no build runs
- Checking kept per part, and the checker built in a tenth of the time
- The parse kept per file
- A cache that forgets
- A part keyed on what it can see
- The key was quadratic in modules
- The whole-program stages, taken down

## [Compiling large programs](../../dreams/docs/notes/compiler-scaling.md)

The quadratics, and the memory held by unforced values, that decided how large
a program `dreams` can compile.

- The compiler was quadratic in the size of the program
- A table indexed by module wants to be a map, not a list
- The three lists the loader still grew one entry at a time
- Sharing the arena
- A lazy value stored in a map pins the map it was made in
- The arena was a chain of its own versions, and `push_node` never saw it
- The item list was appended to

## [What macro expansion costs](../../dreams/docs/notes/macro-expansion.md)

Expansion is a compile: the rounds of work that took its cost from per call to
per program, and the phase meter that guided them.

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
- `mind/std/src/all.dr --test` is not byte-stable across compiler changes
- A program that does not resolve ran the machine out of memory

## [Deforestation](../../dreams/docs/notes/deforestation.md)

How a pipeline of `std.list` combinators becomes one loop the JIT can take,
and why `sum` went from four seconds to under fifty milliseconds.

## [Static types](../../dreams/docs/notes/static-types.md)

The checker, compile-time contracts, and what a signature buys the JIT.

- Compile-time contracts: a refinement run against a value the compiler has
- What a signature buys the compiled code
- Records are values of their own
- A pipeline of setters is one copy

## [The language server](../../lucid/docs/notes/lucid.md)

How `lucid` answers an editor, and which tree answers which question.

- Completion is asked of something that is not a program
- Two trees, and which question goes to which
- A record is a declaration before it is a module
- A host module's members come from the host

## [Machine learning](../../mind/std/docs/notes/ml.md)

`std.ml`: autodiff, layers, losses, optimizers and the training loop on
`std.tensor`, and shaping its Dream code for the JIT.

- The pieces
- Autodiff without a tape
- Losses are single operations
- Where it runs
- Why `fit!` trains in a process of its own
- When the model does not fit the heap
- Shaping the Dream code for the JIT
- Measured: a training run
- What is not done

## [Tensors](../../dream/docs/notes/tensors.md)

Packed numeric arrays, `@` and the GPU: what made the product fast, what it
took to free device memory, and how a chain of operations runs as one pass.

- Why a new kind of value
- Operators, and why `@` is a builtin
- The JIT's float invariant, and the bail that keeps it
- The matrix product
- The GPU
- Releasing device memory
- Fusion
- Deferring
- Running a program
- Fusing into the product
- Measuring a fresh tensor, and the pool for large blocks
- Transposes, and chains into a product
- Finishing fusion
- What machine learning needed
- A GPU program could crash as it exited
- On a real GPU
- Considered: a server process that owns the tensors and mutates them
- What is not done yet

## [Large data in an image](../../dream/docs/notes/large-data.md)

The payload: data past the 4 GiB line an image otherwise stops at, reached as
views rather than copies.

## [Inlining, measured](../../dreams/docs/notes/inlining.md)

Writing small functions out where they are called: built, measured, and not
enabled, and why.

## [Supercompilation](../../dreams/docs/notes/supercompilation.md)

The plan for running programs at compile time to remove the suspensions and
frames their intermediates cost, and the log: the round trip through its core,
and what that found.

- Inlining, measured
- What was built
- What it did
- Why it did not pay

## [Trunks](../../dream/docs/notes/trunks.md)

`std.trunk`: one container, a sequence whose representation the VM chooses
from what is done with it -- a buffer growing at both ends, Baker's version
tree so linear use writes in place, and indexes that force nothing a scan
would not.

- Why one container
- The representation
- Indexes, and forcing nothing extra
- What it bought
- What is not done

## [The browser VM](../../dream/docs/notes/browser-vm.md)

The VM compiled to WebAssembly, pumped a slice at a time from a Web Worker,
and the desktop host that shows its page in a GTK window: what ordinary Dream
needs from a page, and what WebKitGTK refused without saying so.

- The browser VM
- One thread, pumped
- Failures are reported at the end, as natively
- What cannot be reached
- Dream's library, in a page
- The bridge is a native module
- WebKitGTK, 2.54
- Order
- The window and the program
- Renders are patches
