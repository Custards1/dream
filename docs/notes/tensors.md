# Tensors: fast numeric arrays, on the CPU and the GPU

The design behind `std.tensor`, `@`, and arithmetic on tensors. The code is
[dream/src/tensor.cpp](../../dream/src/tensor.cpp) (checking and dispatch),
[tensor_kernels.inc](../../dream/src/tensor_kernels.inc) (the CPU kernels) and
[gpu.cpp](../../dream/src/gpu.cpp) (the GPU). Numbers are from a 4-core Xeon at
2.1 GHz (AVX-512 capable, kernels built for AVX2) and are as of when this was
written.

## Why a new kind of value

An array of floats in Dream is an array of pointers to float boxes. Every
element is its own sixteen-byte object, every product is another allocation,
and every read is a pointer chase. None of what makes numeric code fast --
contiguous loads, a vector unit, a blocked loop that keeps its working set in
cache -- can be done to that. Writing the matrix product as well as Dream
allows (arrays of arrays, a strict accumulator, indexing rather than walking)
measures **6.1 s for 256 x 256**. The same product as `a @ b` on tensors takes
**3 ms**.

So a tensor is a new heap object, `TensorObj`: a header holding a shape (up to
six axes, held inline so the data starts at a fixed offset) and then the
doubles themselves, row-major. It holds no references, so the collector treats
it as a leaf the way it treats a string. Anything past the largest size class
is born in old space and never copied by a minor collection, so a large tensor
is not moved by the collector at all.

It is a value like every other. Nothing mutates one; every operation answers
a new tensor. Sending one to another process copies its numbers in one
`memcpy`.

## Operators, and why `@` is a builtin

`+ - * / %` on tensors are elementwise. They work on a tensor and a tensor, or
a tensor and a number on either side. The shapes must be equal, or one must be
the trailing part of the other, which repeats it (a row added to every row).
That is numpy's broadcasting without stretching length-one axes; the other
cases raise `:shape_error` naming both shapes. They go through `arith`'s slow
path, reached only once the fixnum fast paths have declined, so ordinary
arithmetic pays nothing for them.

The operators do **not** take lists. `+` on two lists already concatenates
them. Every `std.tensor` function and `@` *do* take nested lists and arrays,
converting them on the way in, so `[1, 2, 3] @ [4, 5, 6]` is `32.0` with no
ceremony.

`a @ b` is the matrix product: a matrix and a matrix, a matrix and a vector in
either order, or two vectors (their dot product, a number). It binds like `*`.
It is not an opcode. The parser writes it as the builtin `tensor_matmul`
applied to both sides (`ast.matmul_e`), so nothing after the parser has a new
operator to learn. The arithmetic operators are opcodes because a loop of them
has to be a loop of machine instructions; this one does a matrix of work per
call, and the call costs nothing beside that.

One trap, found the first time it ran: the synthesized name must carry the
span of the `@` token, not of the whole expression. Names are known by where
they are written, the whole expression starts where its left operand does,
and `a @ b` was resolved as `a b` -- "a tensor is not a function".

## The JIT's float invariant, and the bail that keeps it

The JIT keeps floats in registers on one promise from `arith`: an operation
with a float on one side answers a float or raises (the "`Ty`" note at the
head of jit.cpp). Tensors break that promise, because `2.0 * t` is a tensor.

The JIT's type rule stays as it was. `dream_rt_arith_f`, the slow path that
unboxes an answer, now answers 2 when `arith` succeeded with something that is
not a number, and the emitted code turns that into `JIT_BAIL`. A compiled
body is pure and writes nothing but registers; the interpreter re-runs the
call from the frame it was entered with, and `note_bail` gives the function up
after a run of bails. This is the same soundness argument the summation
loop's bail already relied on. A function that scales tensors by floats is
therefore interpreted, which costs nothing that matters: its time is in the
kernels. `jit_tensor_bail.dr` in the e2e suite is the regression test, and
both tiers print the same thing.

A negated tensor is computed as `0 - t` in both tiers, because that is what
compiled code makes of a negation it cannot do in registers. The two tiers
must print the same numbers, `-0.0` included.

## The matrix product

The CPU kernel has the shape every fast GEMM has (Goto and van de Geijn):

- Pack a 256-deep panel of B into 8-column strips and a 96-row block of A
  into 6-row slivers.
- Compute a 6 x 8 tile of C with twelve vector accumulators held in registers
  while the loop walks k.
- Treat one-row and one-column products separately; packing to multiply by
  one column is all overhead.

| | 256 | 1024 | 2048 |
|---|---|---|---|
| Dream arrays (best a program can do) | 6088 ms | -- | -- |
| `@`, one thread | -- | -- | 880 ms (19.5 GFLOP/s) |
| `@`, four threads | 3 ms | 40 ms (54 GFLOP/s) | 246 ms (70 GFLOP/s) |

Above about 3e7 operations the rows of C are split into bands, one thread
each. Each band runs the whole blocked kernel and packs B for itself. The
threads are made for the call and joined before it returns, so the scheduler
never sees them. `DREAM_TENSOR_THREADS` caps them, and 1 turns this off.

**Why two translation units and not `target_clones`.** The first version put
`target_clones("arch=haswell", "default")` on the kernel. The cloned AVX2
kernel ran at **1 GFLOP/s** where the same source built with `-march=haswell`
ran at **33**. GCC will not inline a function compiled for one instruction set
into a clone compiled for another, so the micro-kernel was called out of line
and its accumulators spilled. Now `tensor_kernels.inc` is compiled twice --
`tensor_kernels.cpp` at the baseline and `tensor_kernels_avx2.cpp` with
`-mavx2 -mfma` -- each copy in its own namespace, and `tensor_kernels()` asks
the CPU once which table to hand out. The vector type is as wide as the
target's registers (two doubles at the baseline, four with AVX) to keep the
build warning-free: a 32-byte vector without AVX draws `-Wpsabi`.

`dot` and `sum` keep eight running sums rather than one. The compiler may not
split a floating-point sum, because that changes the rounding; writing the
split out is what lets it vectorize. Every BLAS makes the same trade. The VM
unit test checks both tables against the triple loop at sizes chosen to hit
every edge tile, using small integers so the answer is exact whatever the
summation order.

## The GPU

**What it looks like to a program.** `tensor.gpu t` moves a tensor to the
GPU. Everything done to it then -- `+`, `@`, `tensor.sum`, `tensor.relu` --
runs there, and `tensor.host t` brings an answer back. Which backend runs is
decided by where the operands are, never by the operation, so a program is
written once and moved with one call:

```dream
let w = tensor.gpu (tensor.random 1 [4096, 4096]);
let x = tensor.gpu (tensor.random 2 [4096, 64]);
let y = tensor.relu (w @ x + 1.0);          // all on the GPU
console.print! (tensor.sum y);              // one number comes back
```

An operation the GPU has no kernel for (`sum_axis`, today) still works on a
GPU tensor: it reads the numbers back, runs on the host, and puts the answer
on the GPU. A program is never wrong for having moved, only sometimes slower
than it could be. Mixing a host tensor and a GPU tensor in one operation
raises `:device_error`, which names the two ways out. Silently copying would
hide the one cost a GPU program has to manage.

**Float32 by default.** `tensor.gpu` makes float32 because that is what a GPU
is fast at: most consumer cards run doubles at a thirty-second of the rate.
`tensor.gpu64` asks for float64 where the device has it. A GPU tensor prints
as `gpu tensor[..]`, at float32's precision.

**OpenCL, loaded at run time.** It is the one API every vendor's GPU answers
(NVIDIA, AMD, Intel, Apple), and it has a CPU implementation (POCL), which is
how the tests exercise this code on machines without a GPU. It is loaded with
`dlopen`, and the twenty-odd types and constants used are declared in gpu.cpp
rather than taken from `<CL/cl.h>`. Building the VM therefore needs nothing
new, a machine with no OpenCL runs every program that does not ask for a GPU,
and one that asks gets `:no_gpu` naming what is missing. `DREAM_OPENCL_LIB`
points at a specific library. The device is the first GPU any platform
offers, falling back to any device; it is found on first use and kept.

The kernels are one OpenCL source, built once per element type. They are:

- elementwise, where `x[i % nx]` covers equal shapes and every broadcast in
  one kernel;
- scalar, unary and transpose;
- a 16 x 16 tiled matrix product through local memory, plus a one-row-per-item
  `matvec` for the product with one column;
- a two-stage reduction, finished on the host in double precision.

The operation codes are shared with the CPU kernels (`tensor_kernels.hpp`).

**Asynchronous.** Every operation is enqueued on one in-order queue and
returns at once. Only reading an answer back (`tensor.host`, `tensor.sum`,
printing) waits. A chain of products runs on the device back to back without
the host waiting on any of it.

### Releasing device memory

A GPU tensor is a 56-byte heap object holding a reference-counted handle to a
device buffer (`gpu::Buffer`). Freeing the buffer when the tensor dies is the
first thing in this language that needs a finalizer, and two separate things
were needed to make the loop below run in bounded memory:

```dream
let rec churn n !acc = if n == 0 { acc } else { churn (n - 1) (acc * 0.5 + 1.0) };
churn 1000 (tensor.gpu (tensor.zeros [1000, 1000]))    // 4 MB per step, 8 GB in all
```

1. **The heap keeps a list of objects that hold external memory**
   (`Heap::external_`). After a collection's trace, and before the nursery is
   reset or old space swept, `reap_external` reads the headers. A promoted
   object is followed to its copy; a young object that was not copied, or an
   old one left unmarked by a major, is dead, and its reference is released.
   Copying a GPU tensor to another heap retains the buffer, so both processes
   share one immutable buffer. The shared area refuses one, having no
   collector to release it. A heap's destructor releases what it still holds.

2. **External bytes count as collection pressure.** A GPU tensor is a small
   heap object and megabytes of device memory, so a loop fills the device long
   before it fills the nursery, and nothing ever collected. Measured before
   this: an 8 GB peak for an 8 MB live set. `minor_due` now also fires when the
   external bytes double (with a 64 MB floor). A minor that frees less than
   half of them found old objects holding them, so the next collection is a
   major.

That still measured 8 GB, for a reason outside the collector. OpenCL frees a
released buffer only once every queued command that reads it has run. The
host enqueues far faster than any device computes, so the whole loop was
queued before the first buffer could be freed. `Launch::throttle` now drops a
marker every 32 launches and waits for the one before it. The device always
has a full window queued, but what is in flight is bounded. **8 GB to
669 MB**, most of which is POCL itself and the deliberately bounded window.

## Fusion

`relu (w @ x + b) * 0.5` used to run four elementwise passes after the
product. Each pass read and wrote the whole tensor, and each but the last made
a temporary as large as the answer. For a tensor larger than cache that is
four trips through memory where one would do; on a GPU it is four kernel
launches where one would do. Fusion makes it one.

### Deferring

The elementwise operators and functions do not compute any more. They record
what they would have done and answer a **deferred tensor**: a `TensorObj` with
`TENSOR_DEFERRED` set, whose payload is a `TensorExpr` (value.hpp) rather than
numbers. That is a tiny stack program (load an input, push a constant, a binary
operator, a function), the inputs it loads, and a slot for the answer.

- **Same type, same shape.** `type_of`, `len`, `tensor.shape` and the type
  checker cannot tell a deferred tensor from a computed one, and none of them
  computes anything.
- **Programs are flat.** Building `x op y` from a deferred `x` copies `x`'s
  instructions into the new program instead of pointing at `x`. A chain is
  always one program over computed tensors, never a tree of deferred objects.
- **Errors stay where they were.** Shapes and devices are checked while the
  program is built, so `:shape_error` and `:device_error` are raised at the
  same point as before. The arithmetic is the only thing deferred, and it
  cannot fail. A GPU running out of memory is the one error that now arrives
  where the numbers are read.
- **Reading computes, once.** A product, an index, printing, `==`, a move to
  or from the GPU, or `strict!` computes the program. The answer goes into
  the `result` slot (with the write barrier, since the object may be old by
  then) and the inputs are dropped, so they can die. Every later read finds
  the answer.
- **Reductions never store.** `tensor.sum`, `mean`, `minimum`, `maximum` and
  `norm` evaluate a deferred program and fold it as they go.
  `tensor.norm t` is `t * t`, deferred, summed.
- **Long chains split.** A program has at most 64 instructions, 8 inputs, 16
  constants and a stack of 12. A chain that would pass one computes its
  longer operand and starts again from the answer. A loop such as
  `acc = acc * 0.5 + 1.0` therefore runs eight iterations per pass.
- **Small tensors don't defer.** Below 4096 numbers on the host the answer is
  computed at once: those are already in cache, and recording the program
  would cost more than the pass it saves. On the GPU everything defers,
  because there the saving is launches, and a launch costs the same however
  small the tensor.

### Running a program

**CPU.** The program runs over 256-number blocks, so every stack slot stays in
L1. The stack holds pointers, not copies:

- an input as long as the answer is read where it lies;
- a constant is carried as a scalar and meets a vector through the scalar
  kernel;
- only a broadcast input or an operation's result fills a buffer;
- the last instruction writes the answer's memory directly.

Each stack position has two buffers, so an operation never writes the buffer
it reads. The kernels take `__restrict` pointers, and an aliased one would be
undefined. Every block is the existing vectorized kernels (AVX2 where the CPU
has it). A range past 256K numbers is split across threads, which elementwise
work did not have before.

**GPU.** The program becomes OpenCL source: a `value(i)` function whose body is
the program as one expression, with a `map` kernel and a `reduce` kernel around
it. Constants are kernel arguments rather than literals, so `x * 0.5` and
`x * 0.25` share a kernel, and a loop that changes a scalar every iteration
builds nothing new. Built kernels are cached by their source. `relu` and
`sigmoid` are helper functions, so nesting them does not write the operand out
twice at every level. This replaced the separate elementwise, scalar, unary
and reduction kernels.

### Measured

A five-step chain over 32 MB tensors, with `strict!` after every step as the
unfused baseline in the same binary (`benchmark/tensor/fusion.dr`):

| | stepped | fused | |
|---|---|---|---|
| `relu (a * 2 + b - 1) * 0.5`, 4 threads | 32 ms | 10 ms | 3.2x |
| the same, 1 thread | 81 ms | 22 ms | 3.7x |
| `sum (a * b)`, 4 threads | 5 ms | 1 ms | 5x |
| the chain on the GPU (POCL), with the copy back | 14 ms | 10 ms | 1.4x |

The thousand-step GPU loop under "Releasing device memory" now runs as about
125 kernels instead of 2000. Its peak fell from 669 MB to 510 MB.

`tensor_fusion.dr` in the e2e suite checks fused answers exactly against the
same arithmetic done one float at a time. It covers broadcasting through a
deferred operand, chains past the program limits, a deferred tensor sent to
another process, and every reduction. It also passes under the heap verifier
with parallel and concurrent collection.

### Fusing into the product

`relu (w @ x + b)` was a product that wrote its answer, then one fused pass
over that answer for the `+ b` and `relu`. When the product is cheap beside
its answer -- the wide, shallow layers of a network -- that second pass is a
large share of the cost. Now the chain runs inside the product:

- **`a @ b` defers.** A large product (4096 numbers or more, or any on the
  GPU, but never the dot product of two vectors) answers a deferred tensor
  whose program is one new instruction, `FUSE_PRODUCT`. The two operands are
  kept in `TensorExpr::product_a` and `product_b`, which the collector traces
  and a send copies. Building `w @ x + b` from it appends to that program like
  any other.
- **At most one product per program, as long as the answer.** The chain is
  run on each finished piece of the product, which only works if the answer
  and the product are the same numbers in the same places. A second, different
  product in one program, or a product shorter than the answer (`v + w @ x`
  with `v` a matrix and the product a row repeated down it), makes `build`
  compute that operand first. The same product twice (`h * h + h`) is one
  product.
- **CPU: as each tile is finished.** The blocked kernel calls a
  `GemmEpilogue` when a `96 x 2048` block of C has had its last panel of K
  added in. That is the moment the block will not be touched again and is
  hottest in cache, and `epilogue_tile` runs the program over it in place,
  one row at a time. Rows are contiguous, and the program indexes by position
  in the whole answer, so a row is just a range. Bands on separate threads
  report their own tiles; tiles are disjoint, so epilogues run in parallel.
  Calling the kernel once per small band of rows instead looked simpler and
  was rejected on arithmetic: every call repacks B, which at 2048 x 2048 is
  gigabytes of copying to save a 64 MB pass.
- **GPU: in the store.** A program with a product generates the tiled product
  kernel (and the one-column `matvec`) with the store
  `C[i] = value(i, n, acc, ..)`. That makes `relu (w @ x + b)` one kernel,
  cached by its source like every fused kernel.
- **In-place care.** The epilogue rewrites C where it lies, so the last
  instruction may not write its block directly while its operands still point
  into it. `run_block` writes to scratch and the caller copies over.
- **Reductions compute products first.** `tensor.sum (w @ x)` computes the
  product with its epilogue, then folds. The fold has no tile to ride along
  with.

Measured on a 4096 x 64 by 64 x 4096 layer, `relu (w @ x + bias)`, against
the product computed first (`strict!`) and the chain fused after it:

| | product, then chain | fused into product |
|---|---|---|
| 4 threads | 93 ms | 76 ms |
| 1 thread | 279 ms | 212 ms |
| GPU (POCL), 1024 x 64 by 64 x 1024 | 28 ms | 27 ms |

POCL runs the GPU kernels on the same four cores, so its number says the path
works, not what a GPU would gain; there the saving is a kernel launch and a
full read and write of C.

**Reshape stays deferred** when no input is broadcast. Each input is then
read at exactly the position the program writes, so the program means the
same under any shape of the same size, product included.

`tensor_fusion.dr` checks every fused product exactly against the product
computed first. It covers matrix by matrix (one thread and banded across
threads), the one-row and one-column shapes, a product used twice, two
products, a product too short for the answer, reshape and reduction.
`tensor_gpu.dr` checks the generated product kernels against the host.

### Measuring a fresh tensor

One trap seen while measuring this. The first result written into freshly
mapped memory pays its page faults: about 40 ms for 32 MB here, against
8–10 ms once a major collection has freed earlier blocks for reuse. Whether a
benchmark's runs start on fresh or reused memory decides which of the two it
reports, every run of a process alike. Warm up, and compare processes in the
same state. Large tensors are born in old space and freed only by a major; a
pool for them would remove the faults, and is not built.

### What fusion does not do yet

- **Operations that are not elementwise.** `sum_axis` and `transpose` compute
  their operand first. A transpose could be a strided load in the program.
- **A product as an operand of another product.** `(a @ b) @ c` computes the
  inner product, which it must; but an elementwise chain *before* a product
  (`(x * 2.0) @ w`) is computed into memory rather than applied as the
  product's panels are packed.

## Considered: a server process that owns the tensors and mutates them

The question was whether tensor operations should run in a VM process (or
several) that owns the data and mutates it in place, since the data never
leaves C++ anyway.

**On the CPU, no.** Messages are copied, so every operation would copy its
operands in and its answer out -- more work than the one allocation per
operation it would save. Allocating the answer in the caller's own heap is
already the cheaper side of that trade.

**On the GPU, yes, and the backend already has that shape.** One owner (the
runtime's device and queue) holds the memory, mutation-free buffers are
shared by handle rather than copied, and work is queued to it rather than run
by the caller. It is part of the runtime rather than a Dream process, so
handles never go through message copying.

What mutation would actually buy is fewer allocations in a chain like
`a * 2.0 + b`. Fusion (above) gives that, without exposing mutation.

## What is not done yet

- **Fusing into the product** -- see "What fusion does not do yet".
- **Float32 on the host.** Twice the vector throughput, and the type a model
  is stored in. `dtype` is already in the header.
- **Batched products.** `@` refuses more than two axes.
- **A faster GPU product.** The tiled kernel is the textbook one, a long way
  from cuBLAS. Register tiling (each work-item computing a 4 x 4 or 8 x 8
  block) is the known next step. A CUDA or Metal backend would sit behind the
  same `gpu::` functions.
- **Sending a host tensor without copying it.** A large one could be
  reference-counted the way BEAM shares large binaries.
- **The wire format** (`std.remote`) and compile-time values (`comp`) do not
  carry tensors yet.
