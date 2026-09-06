# dream, the Dream VM

A virtual machine for [Dream](../dreamc), a dynamically typed, lazily evaluated
functional language -- see [docs/language-spec.md](../docs/language-spec.md) for
the language itself. The VM has three parts worth knowing about:

- a **lazy graph-reduction interpreter** written as an explicit state machine,
- **green processes** with isolated heaps and copied messages, in the style of
  the BEAM,
- an optional **LLVM JIT** that compiles the strict numeric spine of hot
  functions.

```
cmake -S . -B build && cmake --build build -j
./build-dream/bin/dream program.dream
```cd /home/blake/project/dream
sed -i 's|#include <atomic>|#include <algorithm>\n#include <atomic>|' dream/src/os.cpp
just build 2>&1 | grep -E "error:" -A 5 | head -20; echo "=== BUILT ==="
S=/tmp/nix-shell-140670-2475414227/claude-1000/-home-blake-project-dream/916c0e2f-75ef-4c7b-8714-dbe1054b4db5/scratchpad
cat > $S/os1.dr <<'EOF'
import std.console;
import std.core;
import std.os;
import std.list;
let main! = {
    console.print! "platform: " (os.platform ())
    console.print! "args:     " (os.args! ())
    console.print! "HOME set: " (os.env! "HOME" != ())
    console.print! "missing:  " (os.env! "DEFINITELY_NOT_SET_XYZ")
    console.print! "cwd ends: " (os.cwd! () != "")
    console.print! "pid > 0:  " (os.pid! () > 0)

    let r = os.exec! "echo" ["hello", "from", "exec"];
    console.print! "exec out: " (core.map_get r :out "")
    console.print! "exec code:" (core.map_get r :code (0 - 1))

    let f = os.exec! "false" [];
    console.print! "fail code:" (core.map_get f :code (0 - 1))

    let e = os.exec! "sh" ["-c", "echo oops >&2; exit 3"];
    console.print! "stderr:   " (core.map_get e :err "")
    console.print! "code:     " (core.map_get e :code (0 - 1))

    console.print! "missing:  " (try! { os.exec! "no_such_program_xyz" [] } catch err { err })
    console.print! "dir has:  " (list.contains "mind" (os.list_dir! "."))
};
EOF
./target/debug/dreamc $S/os1.dr -L mind -o $S/os1.dream >/dev/null 2>&1 && build-dream/bin/dream $S/os1.dream one twov

## Why the interpreter is a state machine

The obvious way to write an evaluator is a recursive `eval`. That choice would
have cost all three of the things above.

A process's entire state — where it is in the program, what it is waiting to do
with the result, and every value it can still reach — lives in two vectors it
owns: a value stack and a continuation stack. Nothing sits in a C++ local across
a step. That single decision buys:

- **Preemption.** A process can be stopped between any two reductions and
  resumed later on a different OS thread, because there is no native stack to
  save. That is what makes one process unable to starve the others.
- **Precise GC roots.** The collector enumerates a list, not a stack. There is
  no conservative scanning and no pinning.
- **Proper tail calls.** A tail call pops its continuation before entering the
  callee, so a tail-recursive loop runs in constant space. `count_down 2000000`
  finishes without growing anything.
- **Cheap processes.** A process is a heap and two small vectors, so hundreds of
  thousands of them fit in memory.

## Values

One 64-bit word, tagged in the low bits:

| Pattern  | Meaning |
|----------|---------|
| `....1`  | fixnum — a 63-bit signed integer, `(int64)v >> 1` |
| `...000` | pointer to a heap object (8-byte aligned); `0` means "no value" |
| `...010` | immediate — unit, bool, char, atom, nil, builtin |

Integers get the one-bit tag because arithmetic is the hot path: testing and
untagging cost a shift each, and the JIT can compare two tagged fixnums
directly, since the tagging preserves order.

## Laziness

Every argument, list element, map value and `let` binding starts as a **thunk**:
a node index plus the frame to evaluate it in. Forcing a thunk overwrites it in
place with an indirection to its result, so every holder of that pointer sees
the computed value and the work happens once.

The compiler decides where evaluation order is observable and says so in the
bytecode. The VM forces a node only when it is marked `STRICT` — the statements
of an impure block, an `if` condition, a `try!` body — and otherwise leaves
things suspended. A discarded pure statement is not evaluated at all, so it
cannot raise an error the program never asked for.

`thunk_for` skips the allocation when a node is already a value: constants,
variable references and closures are returned directly. Returning the
*binding's* thunk rather than a fresh wrapper is also what preserves sharing.

## Processes

Processes share no memory. Each owns a heap that nothing else can reach, and a
message is deep-copied on the way out. That is the BEAM bargain, and it buys the
same things here:

- collection never stops the world and never takes a lock,
- thunk update needs no atomics, because only one process can force a thunk,
- one process failing cannot corrupt another.

The cost is that a thunk shared between two processes is evaluated twice. For a
language with concurrency and laziness, isolation is the better trade.

| Operation | Meaning |
|-----------|---------|
| `spawn! $( .. )` | run a suspended computation in a new process; returns it |
| `send! p v` | copy `v` into `p`'s mailbox |
| `recv! ()` | take the next message, parking until one arrives |
| `self! ()` | the current process |
| `join! p` | wait for `p` and take its result; a failure arrives as an error |

A process that fails and that nobody joins is reported at shutdown. One that a
joiner is waiting for is that joiner's business, and is not reported twice.

**Running out of memory is a process's failure, not the runtime's.** A process
that recurses without bound, or allocates without bound, would otherwise grow
until `alloc` throws and `terminate` takes every other process with it. So the
runtime bounds pending continuations (`DREAM_MAX_DEPTH`), value-stack depth
(`DREAM_MAX_STACK`) and heap bytes (`DREAM_MAX_HEAP`) per process, and raises
`:stack_overflow` or `:out_of_memory` in the offender. The checks sit at
interpreter safepoints rather than in `push_cont` or `alloc`: there the process
is already consistent, and safepoints are one reduction apart, so a limit can
only be overshot by a bounded amount. Making the allocator itself fail would
mean every caller of `alloc` -- most of which hold raw object pointers -- had to
cope with a null.

**Scheduling** is per-worker run queues with work stealing. A process runs for
`REDUCTIONS_PER_SLICE` reductions and then goes back on a queue, whatever it is
in the middle of. When every worker is idle and processes remain, they are all
parked on messages that cannot arrive, and the runtime says so rather than
hanging.

**Parking is a handshake.** A blocking builtin does not park the process; it
asks to be parked, and the worker does it once it has stopped touching the
machine state. A waker that finds the process still running leaves a note that
the worker checks before it commits. Without that, a message arriving in the
window between "the mailbox is empty" and "mark me waiting" would be lost.

## Garbage collection

A Cheney-style copying collector, per process, running only at interpreter
safepoints where the roots are exactly the two stacks, the frame, the result and
the global cache.

Allocation never collects. That is deliberate: the caller of `alloc` usually
holds raw object pointers in C++ locals, and a collection would invalidate them.
Growing the heap between safepoints is bounded because safepoints are one
reduction apart.

Collection also collapses indirection chains left by thunk updates, so long-run
sharing does not accumulate hops.

## The JIT

Off by default only in the sense that it warms up: a function is compiled after
it has been entered `--jit-threshold` times (32 by default).

**What it compiles, and why the line is there.** It compiles the strict numeric
spine — arithmetic, comparisons, branches, and self tail recursion — and leaves
everything else interpreted. The limit is a soundness requirement, not a missing
feature. Compiled code evaluates a self tail call's arguments eagerly, and doing
that to an argument the callee would never have forced turns a program that
terminates quietly into one that raises. So a **strictness analysis** runs first
and the function is compiled only if every parameter is provably forced on every
path:

```
strict(Local i)      = {i}
strict(If c, t, e)   = strict(c) ∪ (strict(t) ∩ strict(e))
strict(a `binop` b)  = strict(a) ∪ strict(b)
strict(a && b)       = strict(a)          -- b is conditional
```

`let rec loop n = if n <= 0 { 0 } else { loop (n - 1) }` qualifies. A function
that passes an argument it might never use does not, and stays interpreted,
where laziness is explicit and free.

Every fast path is inline and every slow path calls the interpreter's own
helper, so the two tiers cannot drift apart about what an operation means. The
end-to-end tests run every program under both tiers and require identical
output.

**Compiled loops stay preemptible.** The back-edge spends a reduction, and when
the budget runs out the loop writes its loop-carried values back to the frame
and returns a "yield" status — an OSR exit. The interpreter picks the next
iteration up from the top of the body, and the scheduler preempts normally. A
compiled 40-million-iteration loop does not starve its neighbours.

On a tail-recursive counting loop the compiled version runs about 14× faster
than the interpreter and allocates nothing, where the interpreter allocated a
thunk per iteration.

`--dump-jit <function>` prints the generated LLVM IR.

## Embedding

`include/dream/dream.h` is a C API: create a VM, load an image, register host
modules, run an entry point. Host functions receive forced arguments and return
values or errors.

```c
dream_vm* vm = dream_vm_new();
dream_vm_load_file(vm, "program.dream", err, sizeof err);
dream_vm_run(vm, NULL);           /* runs main! */
puts(dream_vm_result_text(vm));
dream_vm_free(vm);
```

A member registered with an arity of `DREAM_VARIADIC` takes however many
arguments its call site passed, with every one forced. Because functions are
curried, such a member can never be partially applied -- `f a b` and a
half-finished `f a b c` are the same thing until the application node says
otherwise -- so it is the right shape for something like `console.print!` and
the wrong one for anything a caller might want to pre-fill.

## Layout

| Path | |
|------|-|
| `src/value.hpp`   | value tagging and heap object layouts |
| `src/heap.cpp`    | per-process heap and copying collector |
| `src/image.cpp`   | `.dream` loader and validator |
| `src/interp.cpp`  | the reduction machine |
| `src/process.cpp` | process state and mailbox |
| `src/scheduler.cpp` | workers, run queues, parking |
| `src/builtins.cpp` | builtins and the `std.*` modules |
| `src/jit.cpp`     | strictness analysis and LLVM code generation |
| `src/capi.cpp`    | the C embedding API |

## Tests

```
cmake --build build && ctest --test-dir build
```

`dream_tests` covers values, the collector, cross-heap copying, image validation
and maps. `tests/e2e.sh` compiles real programs and checks their output under
both tiers.
