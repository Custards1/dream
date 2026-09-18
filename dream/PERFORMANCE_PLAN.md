# Self-compilation below 1.25 seconds

## Status and scope

The target is not met. The GC fix, indexed heap verification, decoded array
accessors, benchmark gate, and optional GCC profile-guided build are implemented
in `dream/`. Compiler and standard library sources remain unchanged by this
work. Measurements are from this checkout on
2026-09-18, including the existing user edits in `dreams/ir.dr` and
`mind/std/dynamic.dr`. Those edits were preserved.

## Implementation results

The single-worker failure was a failed concurrent-mark launch leaving root
marks behind. The fallback full trace skipped those already-marked objects,
so their children could be freed. Failed launches now undo exactly the marks
acquired during seeding, including leaf roots. A unit regression covers a
declined launch, duplicate roots, and child survival through the fallback GC.

Heap verification formerly searched every allocation block for every object.
It now builds a sorted range index once per verification and checks ownership
before dereferencing a header. Full verified self-compilation completes and
reproduces stage 2 byte for byte.

The image loader now recognizes functions whose entire body reads a local
array argument at a constant or argument-supplied index. Saturated calls can
read an already-materialized array without allocating a callee frame. Suspended
inputs, invalid indexes, non-arrays, and fallbacks retain ordinary evaluation.
Selected elements are entered normally, so their laziness and errors remain
intact. No on-disk format change, unsafe frame reuse, or compiler change is
required. Profiling uses the ordinary path; the fast path accounts for the
body reduction and respects the remaining reduction budget.

Five-run single-worker interpreter medians before the combined final build:

| Configuration | Median | Maximum |
| --- | ---: | ---: |
| GC fix only | 3.092 s | 3.153 s |
| Bounded nested Return batching (discarded) | 3.115 s | 3.154 s |
| Recursive ready-field reads (discarded) | 3.178 s | 3.206 s |
| Decoded array accessors, ordinary build | 3.038 s | 3.071 s |
| GCC PGO, before accessor optimization, LLVM disabled | 2.577 s | 2.970 s |

An LTO-only trial also took about 3.10 seconds and was not retained. Native CPU
sampling using the installed gperftools library found distributed costs:
approximately 8.8% in evaluation dispatch, 6.9% resolving indirections, 6.0%
in continuation insertion, 5.2% in copying, and substantial GC work. These are
sampled locations (including inlined functions), not exclusive subsystem costs.
Single-worker GC pauses were about 0.83 seconds; the earlier 0.28–0.31-second
figures below describe default-worker parallel collection, not single-worker GC.

### Reproduce the benchmark

```sh
python3 dream/tests/self_compile_bench.py --no-jit --workers 1
python3 dream/tests/self_compile_bench.py
```

The script builds stage 2 from the seed, warms up, then performs at least five
sequential self-compiles, verifying byte equality each time. JSON includes the
CPU, build settings, image hashes, environment, individual wall times, median,
maximum, and gate status. Exit status 1 means the 1.25-second gate failed.
`--vm`, `--seed`, `--workers`, `--runs`, `--timeout`, and `--target` configure it.
First invocation is recorded separately without claiming OS caches are cold.

### Train a GCC build

Use the same fresh build directory for both phases. The chosen build type and
LLVM setting must remain unchanged between training and use:

```sh
cmake -S dream -B /tmp/dream-trained-build -DCMAKE_BUILD_TYPE=Release -DDREAM_PGO=GENERATE
cmake --build /tmp/dream-trained-build -j 4
/tmp/dream-trained-build/bin/dream_tests
/tmp/dream-trained-build/bin/dream dreams/bootstrap/dreams.dream -L mind -L . -o /tmp/dream-trained-stage2.dream dreams/main.dr
/tmp/dream-trained-build/bin/dream --no-jit -j 1 /tmp/dream-trained-stage2.dream -L mind -L . -o /tmp/dream-trained-stage3.dream dreams/main.dr
cmp /tmp/dream-trained-stage2.dream /tmp/dream-trained-stage3.dream
cmake -S dream -B /tmp/dream-trained-build -DDREAM_PGO=USE
cmake --build /tmp/dream-trained-build -j 4
python3 dream/tests/self_compile_bench.py --vm /tmp/dream-trained-build/bin/dream --no-jit --workers 1
```

PGO defaults to OFF and requires GCC when enabled. Training uses atomic
counters; `os.exit!` explicitly flushes them because `_Exit` skips GCC's normal
shutdown writer. USE builds have no instrumentation and reject missing or
incompatible profiles. After changing VM sources, retrain in a fresh build
directory or configure `-DDREAM_PGO=OFF` before rebuilding. Broader training
workloads may give different performance; this profile targets self-compilation.

The numbered sections below retain the original investigation and remaining
work. In particular, the 1.25-second acceptance gate remains open; broad frame
reuse and general instruction fusion have not been implemented or proven safe.

## Original measured baseline

The existing RelWithDebInfo build was confirmed current with
`cmake --build build-dream -j 4`.

Run from the repository root (VM options must precede the image):

```sh
time ./build-dream/bin/dream dreams/bootstrap/dreams.dream -L mind -L . -o /tmp/dream-perf-stage2.dream dreams/main.dr
time ./build-dream/bin/dream --stats /tmp/dream-perf-stage2.dream -L mind -L . -o /tmp/dream-perf-stage3.dream dreams/main.dr
cmp /tmp/dream-perf-stage2.dream /tmp/dream-perf-stage3.dream
```

- Seed compilation: 3.335 seconds wall time.
- Unprofiled stage-2 self-compilation, with statistics: 3.101 seconds.
- Default-worker interpreter-only self-compilation: 3.097 seconds.
- Instrumented function profile: 3.478 seconds; not a benchmark result.
- Stage 2, stage 3, and a repeated output were byte-identical: 485,800 bytes,
  27 modules, 19,667 nodes, 1,670 functions.
- Approximately 73.27 million reductions and 1.69 GB allocated per compile.
- Allocation by kind: frames 39%, thunks 20%, lists 19%, maps 18%.
- Seven major and 53 minor collections; 0.28–0.31 seconds stopped in GC.
- Only 19 functions compiled by the JIT. These runs show no meaningful JIT
  advantage; repeated controlled measurements are needed to quantify it.
- Largest reduction counts: `lower_expr` 7.7%, `check_expr` 5.7%,
  `skip_trivia` 4.6%, `map` 2.8%, `stream` 2.5%, `parse_apply` 2.5%.
  Reduction counts are not CPU-time samples.
- Existing native unit tests: 64,782 checks, zero failures.

This needs about a 2.5x speedup. Eliminating the measured GC pauses entirely
would still leave approximately 2.8 seconds. Nursery tuning alone cannot
deliver the target.

## 1. Resolve the single-worker correctness failure

Before the fix, the newly generated compiler repeatedly failed with:

```sh
./build-dream/bin/dream --no-jit -j 1 /tmp/dream-perf-stage2.dream -L mind -L . -o /tmp/dream-perf-interp.dream dreams/main.dr
```

Observed error: `not` needs a bool, got a list. With JIT enabled and `-j 1`,
one run exited with status 139. Default-worker runs succeeded in both modes.
The cause is unproven; do not attribute it to JIT or GC without diagnosis.

Reproduce with heap verification, then a debug/sanitized build and a debugger
backtrace. Compare worker counts and collection modes. Inspect root lifetimes
across `force_whnf`, native calls, and collection, as well as worker-dependent
state. Reduce the failure to a regression under `tests/programs/` or
`tests/test_main.cpp`; fix it before relying on worker-count tuning or changing
allocation lifetimes. Success means repeated byte-identical self-compiles with
one and default workers, in both execution modes.

## 2. Establish a repeatable timing gate

Add `dream/tests/self_compile_bench.sh` with configurable VM/image paths, a
private temporary directory, one warmup and at least five sequential measured
runs. Record wall time, median, maximum, build type, CPU, worker count, image
hash, and relevant environment settings. Include VM startup, image loading,
compilation, and output writing. Exclude building the C++ executable. Report
seed compilation separately from self-compilation by the generated image.

Check every exit code and output equality. Do not benchmark competing runs
in parallel or use profiler timings for acceptance. Final acceptance: all five
measured self-compiles below 1.25 seconds on the agreed reference machine;
report cold-start timing separately. Also require interpreter-only success,
since the requested improvement concerns the interpreter.

## 3. Profile native execution before choosing optimizations

Collect CPU samples for default and interpreter-only runs using a native
sampling profiler. `perf` was not found on this session's PATH. Separate time
in `step_eval`, `step_return`, `force_whnf`, allocation, container operations,
GC, and JIT compilation. The language function profile above is insufficient
to choose among these costs.

If needed, add opt-in counters for opcode/continuation pairs, frame sizes,
thunk forcing, and function entries, with no atomic increments in ordinary
execution. Rank candidates by measured total cost and expected removable
work. Avoid adding counters permanently to every reduction.

## 4. Reduce dispatch and allocation costs incrementally

First inspect `src/interp.cpp`'s nested `force_whnf` loop. The outer loop already
drains Return continuations; investigate a corresponding bounded fast path
inside nested forcing. Preserve the continuation floor, parking behavior,
limit checks, and GC opportunities. Some returns allocate, so skipping all
safepoints through arbitrary return chains needs a proved bound.

Next measure frame allocation in `do_apply` and direct-call paths. Introduce
frame reuse only where escape/alias analysis proves no closure or suspended
thunk retains it. Unconditional tail-call frame reuse is unsound for lazy
evaluation. Keep the allocating path for unknown cases.

Then consider an internal decoded instruction representation in `image.*` and
`interp.*`: specialize common local reads, direct calls and continuation
pairs at load time without changing the on-disk image or compiler. Account
for decoding startup cost in the 1.25-second gate. Preserve lazy argument
sharing, error order, reductions, and scheduler fairness.

Finally optimize allocator hot paths in `heap.*` only if sampling supports it:
specialize frequent frame/thunk sizes, reduce redundant initialization, and
retain required write barriers. Existing code already skips many trivial
thunks and uses allocation size-class lookup tables; measure beyond those
optimizations rather than reimplementing them.

Land each optimization separately with before/after timings. Reject changes
whose benefit disappears across repeated runs. If interpreter improvements
remain insufficient, evaluate broader JIT coverage as a separate follow-up;
do not silently redefine an interpreter-only target as a JIT benchmark.

## 5. Verification and completion

For each semantic change, run native unit tests and `dream/tests/e2e.sh`, which
compares interpreted and JIT results. Exercise verified heaps and the existing
parallel/concurrent GC modes after changes to roots or allocation. Add focused
regressions for newly affected lazy arguments, thunk sharing, effects executed
once, tail recursion, limits, and parking. Build without LLVM as well.

Finish with seed → stage 2 → stage 3 byte equality, successful self-compilation
in both modes and worker configurations, and the timing gate. Publish actual
results and remaining costs; there is no evidence yet that a small isolated
patch can achieve 1.25 seconds.
