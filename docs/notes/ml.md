# std.ml: machine learning on std.tensor

The design behind `std.ml` -- autodiff, layers, losses, optimizers, datasets
and the training loop -- what it had to add underneath it, and what was
measured. The code is [mind/std/ml/](../../mind/std/ml/), one module per
piece, each with its design at the head of the file; this note is the why
across them. Numbers are from a 12-core Ryzen 5900X and an RTX 3070, as of
2026-10-04.

## The pieces

| module | what it is |
|---|---|
| `std.ml.ad` | reverse-mode autodiff over tensors and numbers |
| `std.ml.nn` | layers as records of `init` and `apply`, and models made of them |
| `std.ml.loss` | losses, the classification ones fused with their squashing |
| `std.ml.optim` | SGD (momentum, Nesterov), Adam, AdamW, RMSProp, Adagrad, clipping, schedules |
| `std.ml.metrics` | accuracy, top-k, confusion, precision/recall/F1, MSE/MAE/R² |
| `std.ml.data` | datasets, shuffling, batching, splits and folds, scalers, CSV, synthetic sets |
| `std.ml.init` | Glorot, He, LeCun, orthogonal, truncated normal |
| `std.ml.tree` | parameter trees: map, zip, flatten, move between devices |
| `std.ml.ops` | softmax, logsumexp, means along an axis, device matching |
| `std.ml` | `fit!`, `predict`, `classify`, `evaluate`, `save!`, `load!` |

Every one of them is `std.tensor` underneath and nothing else. That was the
rule the whole time: what the library needed and `std.tensor` lacked was
added to `std.tensor` (see "What machine learning needed" in
[tensors.md](tensors.md)), never reimplemented in Dream, so that it is fused,
runs on either device, and is there for the next library too.

## Autodiff without a tape

A variable is `#[:ad_node, value, links]`: what was computed, and for each
operand that is itself a variable, the operand and a function from this
node's gradient to the operand's share of it. A plain tensor or number is a
constant, and an operation with no variable operand answers a plain value,
so a model is written once and runs with or without gradients -- `nn.apply`
for a prediction is the same `apply` training uses, with nothing recorded.

The backward pass needs two things a tree of closures does not give: to
visit each node once, after every node that used it, and to add up what
arrives at a node used twice (`h` in `mul h h`, the input of a residual
block). Walking the links recursively instead would visit a shared node
once per path to it, which a stack of residual blocks makes exponential.

**The answer is that a map keys an array by identity.** A set of visited
nodes and a table of gradients are ordinary maps keyed by the variables
themselves, and an identity key keeps its hash across collections ("GC
evacuation" found and fixed the bug that would have broken this). So the
pass is: a depth-first walk into an order where every node comes before its
operands, then gradients flow down that order, each node's sent to its
operands and dropped. Nothing is threaded through the forward computation
-- no counter, no tape -- and nothing is mutated, so the whole thing is pure.

Every operation's gradient was checked against central differences
(`ad.numeric_grad`), including a whole convolutional network, a transformer
block and the three recurrent layers, to 1e-5 or better.

## Losses are single operations

`cross_entropy` and `binary_cross_entropy` take logits and are one node each
(`ad.node`, which is public so that any operation can be defined the same
way), with the gradient written out: `(softmax z - y) / n`. Composed from
`log` and `softmax`, the forward pass takes the logarithm of probabilities
that may have rounded to zero, and the backward is a dozen nodes for what
is one subtraction. A target may be classes or rows; classes become one-hot
rows on the device the logits are on.

## Where it runs

`fit!` takes `:device` -- `:gpu`, `:host`, or `:auto`, which picks the GPU
when there is one and the parameters times the batch reach four million.
Below that a step is a few small kernels, and launching them costs more than
the arithmetic. On the GPU the parameters, the optimizer state and the
dataset (when it is under a gigabyte) move there once; shuffling is a
`take` on the device and batching a `slice`, so a step uploads nothing and
keeps named built-in losses as one-element tensors until the epoch ends.
Metrics and custom loss functions can still read back each step.

Every model trains to the same answers on both devices within float32.
Measured per training step, the best epoch after the first (which compiles
kernels), `:auto` choosing the GPU for each:

| model | parameters | host | RTX 3070 | |
|---|---|---|---|---|
| MLP 64 wide, batch 256 | 21K | 3.6 ms | 0.90 ms | 4.0x |
| MLP 256 wide | 134K | 15.2 ms | 1.04 ms | 14.6x |
| MLP 1024 wide | 1.3M | 66 ms | 3.6 ms | 18.2x |
| MLP 2048 wide | 4.7M | 173 ms | 7.6 ms | 22.8x |
| CNN, 2 conv + 2 pool + 2 dense, 28 x 28, batch 128 | 422K | 179 ms | 13.8 ms | 13.0x |
| 2 transformer blocks, 32 steps, batch 64 | 407K | 410 ms | 17.5 ms | 23.5x |

The first numbers measured on the card were far from these -- the CNN was
2.3x, and a conv step 34 ms of which 32 were a bias gradient. What fixed
them was in `std.tensor`'s GPU backend, not here: see "On a real GPU" in
[tensors.md](tensors.md).

### Why `fit!` trains in a process of its own

A collection can only run where every C++ frame above the running code has
vouched for its locals, and a value forced from inside a native -- 
`console.print! [name, ml.fit! ..]`, where the printer forces the list --
runs unvouched: nothing it allocates is collected until it returns. Under
such a caller, training never collected once, and the device filled at
about a gigabyte a second until an 8 GB card refused a launch. Standalone,
the same model collected every step and peaked at 1 GB. `fit!` therefore
runs in a process it spawns and joins: a new process starts from the
scheduler with nothing above it, so it collects wherever it was called
from. The dataset crosses as a `memcpy` per tensor (a shared buffer for a
GPU one), and `join!` re-raises a failure, so nothing else changes. A
training loop of your own written with `ml.step` should do the same if it
may be called from inside a value something prints.

### When the model does not fit the heap

A process's heap is capped at a gigabyte (`DREAM_MAX_HEAP`), and Adam on a
4.7M-parameter network holds about half that at the moment a step replaces
the parameters and both moments, as doubles on the host. It failed deep in
the optimizer with nothing to say why. `fit!` catches `:out_of_memory` and
re-raises it saying what training holds and the two ways out -- the limit,
or the GPU, whose memory the heap does not count.

Catching it at all needed a fix in the VM: the limit is judged by the live
bytes the last collection measured, so once raised, every step after it
raised it again -- the handler's first allocation included -- until a
collection happened to run, and `try!` could not catch it in practice. It
is now raised once per collection (`Process::heap_limit_raised_at`);
`heap_limit_caught.dr` is the test.

## Shaping the Dream code for the JIT

The arithmetic is native, but each training step also walks the graph, the
parameter tree and the optimizer state in Dream -- about 4,500 reductions a
step for a three-layer network. When it was first written the JIT compiled
none of that. `DREAM_JIT_WHY=1` (added to jit.cpp for this) names the node
that refuses each function, and nearly every refusal was one thing:

- **A `match` on anything but a parameter.** `match` binds its subject
  strictly, and the tier takes a strict binding only of a parameter
  (`check_bind`). `match type_of x { .. }`, `match grads.[n else ()] { .. }`
  and `match list.filter .. { .. }` each refused their whole function.

and the rest were lambdas made inside a loop (`closure`), variables captured
by a local `let rec` (`capture`), and a module member passed as a value
(`field`). The hot walks were rewritten to suit: each loop a top-level
function calling itself, accumulators strict, a computed subject handed to a
helper that matches on its parameter. After it, every hot walk compiles --
the topological walk, the gradient flow, the tree maps, `node`,
`unbroadcast` -- and a 3,000-step run of a small network went from the JIT
helping nothing to **2.32 s with the JIT against 2.87 s without** (median of
five, interleaved).

The tier now also accepts strict bindings of computed values: it evaluates
them at the binding point and retains them in its per-invocation memo storage.
Computed match subjects can therefore compile without these rewrites.
`jit_strict_computed.dr` checks both the answers and that the functions compile.

## State and device-local training

`nn.batch_norm` normalizes over every axis except the last, channels axis.
Its trainable scale and bias belong to the parameter tree; its running mean
and population variance belong to a separate state tree. `nn.init_state`
creates that tree and `nn.forward_state` returns the output and updated state.
Sequential and residual layers carry their children's state through them.
Prediction uses the stored statistics without updating them.

`ml.fit!` retains this state, including alongside the best parameters when
early stopping restores them. Save `[ml.params fitted, ml.state fitted]` with
`ml.save!` and supply both `:params` and `:state` to resume. For a manual loop,
`ml.step_state` returns the new layer state beside the optimizer state.

Named built-in losses use `tensor.sum_tensor` on the GPU, keeping the loss
and its gradient on the device. The epoch's accumulated loss is read once.
Dropout uses `tensor.random_like`, whose counter-based GPU kernel generates
the mask on the device with the input's shape and dtype. Both random paths
use the same splitmix64 stream, subject to the device's floating-point precision.

## Measured: a training run

`examples/22_ml.dr` trains a 1,251-parameter network on three spirals to
100% held-out accuracy in 60 epochs, and recovers a noisy linear model's
weights to two decimals (R² 0.998). A three-class spiral run of 30 epochs
takes 0.48 s in all, compiling included.

## What is not done

- **Convolutions on the host** are slow beside the GPU (179 ms a step for
  the CNN above): `im2col` and a product per layer, with no host kernel of
  their own.
- **Float32 on the host**, mixed precision, and a faster GPU product -- see
  "What is not done yet" in tensors.md.
