# Handoff: what to build next, and how

For an agent picking up the supercompiler and debugger work. Read `CLAUDE.md`
at the root first; this assumes it. Everything below was learned the hard way
in the sessions that built what is already there, and each trap named here
cost real time.

## Where things stand (2026-10-19)

- **The supercompiler** (`dreams/src/{sc,drive,hoist,accum}.dr`) runs on
  release builds (`--release`, `--supercompile`). `accum.dr` rewrites slow
  shapes exactly: accumulators grown at the end, membership scans, counts,
  keyed searches, priority queues, tree recursion (memo tables), association
  lists updated by a `bump` (`upserted`) -- and, newest, **a design**:
  `accum.designed` keeps an association list as a trunk across every function
  it passes through (`assoc_op` reads lookups/replaces off the lowered
  `match`; `composite_of` finds the small functions between them and inlines
  them). Its corpus is `dreams/tests/sc/*.dr`, each with `.expected` and a
  `.ratio`. The design and measurements are in
  `dreams/docs/notes/supercompilation.md`, newest section "A design,
  rewritten".
- **Trunks** (`std.trunk`, `dream/src/trunk.cpp`): the one container the VM
  specializes. The supercompiler reaches them through `_trunk_*` builtins.
  Each version remembers its last lookup (`TrunkObj::memo_key`), which is what
  makes a replace after a lookup exact without a second search.
- **The debugger**: `dream --debug 'CMDS' IMAGE` (`dream/src/debug.{hpp,cpp}`).
  `break NAME|FILE:LINE`, `trace NAME`, `watch NATIVE`, `on NAME cmd, cmd`,
  `step/next/finish/continue`, `bt`, `locals`, `print NAME|sN|cN`. Locals are
  named when the image was built `--debug-info` (`debug = true` in a
  `mind.toml` `[build]` or profile), from the `LOCL` section
  (`dreams/src/debuginfo.dr`). Use it instead of writing probes into the VM.

## The plan

The user's direction: the supercompiler should "know better than the
programmer" and **turn badly written designs into good ones** -- whole-program
choices, not one loop at a time -- while staying **exact**. Five kinds of bad
design were named; the first (the wrong data structure across functions) is
done. Do the rest in this order, each as its own commit:

### 1. Many passes where one would do

`mean xs = sum xs / length xs`, then `variance` walking `xs` twice more, then
`maximum xs`, `minimum xs`: each a separate fold over the same list, each
paying the list's laziness and cells again.

- **Find**: in one function body, two or more calls of *fold shapes* on the
  **same** list variable. `accum.fold_shape` and `aggregate_shape` already
  recognize a function that folds a list (`[list position, accumulator
  position]`, `:whole` / `:headed` forms); `Program.folds` and
  `Program.aggregates` hold them by global.
- **Rewrite**: tuple them -- one loop carrying every accumulator, answering a
  record/list of results, each call site reading its part. This is classic
  tupling (Chin, "Towards an automated tupling strategy", PEPM 1993).
- **Exactness**: each fold forces the list's spine and the elements its step
  forces. The tupled loop forces the union of those, in one walk -- which
  differs from the original only if one fold would never have run (its result
  unused on some path) and the list is infinite or raises. So apply only when
  every tupled call is **demanded on the same path** (all in the function's
  strict positions, or all in one strict block), and keep each step's own
  laziness: a step that does not force an element must not start to.
- **Test**: a `tests/sc/stats.dr` with mean/variance/min/max over 200k
  elements, `.ratio` `1 3 time` or better; outputs identical.

### 2. Recomputation across calls

A helper rebuilding an index or table on every call, inside a loop over
something else: `lookup_price items name = (map.of_list (prices items)).[name]`
called once per order.

- **Find**: a call inside a loop whose argument subterm depends only on loop
  invariants (parameters handed on unchanged). `hoist.dr` already hoists loop
  invariants *within* a function; extend it **through a call**: specialize the
  callee on the invariant argument, the way `drive.dr` specializes on known
  arguments, and bind the invariant part once in the wrapper.
- **Exactness**: hoisting forces nothing if the hoisted value stays lazy
  (bind it with `Stmt.bind name 0 ..`, not strict). A lazy binding computed
  once and shared is call-by-need, which is the language's semantics.
- **Trap**: hoisting an expression out of a branch that may not run is fine
  only because the binding is lazy -- never make it strict.

### 3. Round trips

Keys built by string concatenation and split again, numbers turned into
strings and parsed back, `to_string` for comparison.

- **Find** pairs in the IR: `parse_int (to_string n)` -> `n` when `n` is an
  integer (exact only then -- prove it from the producer: arithmetic on
  integer constants/params with integer hints, `TYPE`/`ITYP`); `str.split sep
  (a + sep + b)` where neither part contains `sep` (provable only for
  constants or `to_string` of integers) -> `[a, b]`.
- Keep this small and provable; skip anything that needs reasoning about
  arbitrary strings.

### 4. An expensive operation for a cheap answer

`list.head (list.sort xs)` -> `minimum` (stable: the *first* minimal element,
which is what a stable sort's head is); `length (filter p xs)` -> a count;
`list.last (list.sort xs)` -> the last maximal element; `take 1 (sort ..)`.
`accum.heaped` already handles a sort read repeatedly as a priority queue --
read it first.

- **Exactness**: `sort` forces every element (`compare` on each pair);
  `minimum` forces every element too, so forcing matches. A value `compare`
  does not order totally (NaN, maps) makes `sort` and `minimum` differ: fall
  back to the code as written exactly the way `heaped` does.

### 5. The debugger, stage two: lines

`SPAN` is a reserved section (`dream/docs/bytecode-format.md`): per-node
source spans, parallel to `NODE`. Nodes carry no positions today -- lowering
(`dreams/src/lower.dr`) does not record them -- so:

- Add a span to what lowering emits for **statements and calls** only (a
  `bind`, a block statement, an `apply`): those are the places a person steps
  between. Thread the AST span (every AST node ends with its span) into the
  few `ir` constructors that make them, behind `Options.debug_info`, and
  write `SPAN` only then (an empty/zero span for every other node).
- VM: read `SPAN` in `image.cpp` (the parse arm exists); in `debug.cpp`, add
  `break FILE:LINE` at node granularity (stop when `p.node`'s span starts on
  that line) and `step` by line (stop when the line changes within the same
  frame). `diag.place_in` and the `places` table in `lower.dr` turn offsets
  into lines; the image only needs offsets plus the file, which `FuncRec`
  already names.

## How to work here

- **Build**: `nix-shell shell.nix --run 'just vm'`, then `just dreams`. Use
  the repository's `shell.nix`, not `nix-shell -p llvm`: the static `libffi`
  the build links is the one `shell.nix` provides. `shell.nix` sets
  `DREAM_PACKAGES`; tests must not depend on the package path.
- **Always** `ulimit -v 24000000` and a `timeout` on VM runs. A diverging or
  ballooning run will take the machine down.
- **The suites read the trees live.** Every `just test-*` recipe rebuilds the
  VM and `dreams/tests/sc.sh` compiles `dreams/src`. Editing sources while a
  suite runs gives false failures. Develop in a `git worktree` of the
  submodule (`git -C dreams worktree add /tmp/x HEAD`, a directory with
  `dreams -> /tmp/x` and `mind -> <repo>/mind` symlinks, and compile with
  `-L that-dir/mind -L that-dir`), and build a VM worktree into its own build
  directory.
- **Reseed** after any change to `dreams/src` or `mind/std` (`just reseed`);
  the seed is a supercompiled release build, so a supercompiler bug shows up
  in the compiler itself. `just test-bootstrap` must stay byte-identical.
- **`DREAMS_CACHE`**: under `just` the whole-build cache is on; a "warm" or
  timing comparison must set `DREAMS_CACHE=`.
- **Measure** at n and 2n (quadratic shows as 4x), A/B against the commit
  before in a worktree with the same VM, 3% is noise. `--stats` reports what
  every trunk became; `dream --debug 'watch _trunk_recall; continue'` shows a
  builtin's every call.

## Reading the supercompiler's input

Every shape matches `sc.Term`s (`dreams/src/sc.dr`: `var`, `prim(op, flags, a,
b, c, children)`, `block`, `lam`, `trying`). To see a function's terms, a
scratch program that imports `dreams.compile`, builds, runs `hoist.hoist`,
reads `sc.read img` and prints `sc.body_of rd f` for the function by name
works well (`to_string` of a `Term` is readable). Things that look one way
in the source are another in the terms:

- A `match` on a list is `type_is(x, 7, 0)` plus `list_is_cons`,
  `list_head`, `list_tail` **opcodes** (they were `_match_*` builtin calls
  until 2026-10-18; opt.dr rewrites them). Shapes must accept the opcodes.
  `accum.pair_holds`/`pair_path` evaluate a pattern's tests on a model rather
  than matching their exact form -- reuse them.
- A pattern that binds no name (`[[n, _], ..]`) has no binding for it.
- `list.cons` is the `:cons` op; `list.append a b` is a call of
  `Program.append`'s global; `a + [x]` is `:add`.
- A resolved name's key is `((context * 65536 + source) << 32) + start`
  (`scope.ref_key`): the file is the middle field.
- A build in parts keeps each part's references in the part
  (`scope.Resolution.parts`); the merged resolution's own are few.

## Exactness, the rule every rewrite keeps

Force nothing the program as written would not force, and raise the same
errors -- except for *which* of two independent failures is met first, which
the existing shapes accept and say so. When a fact cannot be proved
statically, check it at run time and fall back to the code as written (the
trunk path's "trunk or list" dispatch, `_set_lookup` answering `()` for "the
scan runs"). A rewrite that is not exact is a miscompile, however fast.

Write each rewrite's reasoning into the comment above it, in full sentences,
and a section in `dreams/docs/notes/supercompilation.md` with the table of
what it measured: that is the style of the codebase and what the next agent
will read.
