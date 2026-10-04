# Benchmarks

## Dream against CPython: [`benchmark/`](benchmark)

Seven small workloads, written twice: [`main.dr`](benchmark/main.dr) in Dream
and [`bench.py`](benchmark/bench.py) as a line-for-line transliteration in
Python, at the same sizes.

| | |
|---|---|
| `fib` | naive recursive Fibonacci of 32: calls and integer arithmetic |
| `sum` | a fold over ten million integers: what deforestation and the JIT do to a pipeline |
| `collatz` | Collatz chain lengths up to 100,000: a tight numeric loop |
| `pi` | a two-million-term series: floating point |
| `mapfilter` | `map` and `filter` over half a million elements |
| `bytescan` | a walk over three megabytes of text |
| `strbuild` | building a string from 400,000 pieces |

```
benchmark/benchmark/run.sh                  # all of them, best of three each
benchmark/benchmark/run.sh --repeat 7 fib   # one, more carefully
```

It needs a built checkout (`just`) and `python3`. Each workload runs in a
fresh process and the best run is reported, since everything a repeat adds --
a scheduling decision, a page fault, another process -- only ever adds time.
The script also diffs the two programs' results, so a speed-up that changed
the answer shows up as a failure rather than a win. `dream=` and `dreams=`
pick another VM or compiler, which is how to A/B a change: build the commit
before in a worktree and run both.

Where the numbers stand and what the gaps are made of is "Where it stands
against CPython, and what the remaining gap is made of" in
[docs/notes/vm-performance.md](../docs/notes/vm-performance.md). The short
version: numeric and list loops the JIT takes run several times faster than
CPython. `strbuild` is far slower, and is not a runtime comparison at all:
Python's `"hello " * n` is a single `memcpy` loop, where the Dream program
builds a 400,000-element list and concatenates it -- the same answer by a
different algorithm.

## Tensors: [`tensor/`](tensor)

A matrix product three ways -- in Dream over arrays of floats, with `@` on the
CPU, and with `@` on the GPU -- reporting milliseconds and GFLOP/s for each
and checking the three agree. `fusion.dr` measures an elementwise chain fused
into one pass against the same chain forced a step at a time, and
`operands.dr` a product reading transposed and computed operands in place
against computing them into memory first.

```
dream build/dreams.dream -L mind benchmark/tensor/main.dr -o /tmp/tb.dream
dream /tmp/tb.dream [N]
```

[docs/notes/tensors.md](../docs/notes/tensors.md) is the record.

## The compiler

Two more live elsewhere, because they measure the toolchain rather than
programs:

```
just bench-self-compile [VM]      # a self-compile, five runs, the median
python3 dreams/tests/scale.py     # generated programs of N declarations or modules
```

The self-compile is the end-to-end number for the VM. `scale.py` is the one
for the compiler: grow the input and divide, because a quadratic is invisible
at the size of this repository.
