# Session mode: incremental compilation for the REPL

The REPL today compiles the whole session from scratch on every entry
([dreams/repl.dr](dreams/repl.dr#L190)): write the accumulated text to a scratch
file, `modules.load!` from a fresh loader, `scope.resolve!`, `lower.link!`,
write the image, optionally run it. Every entry re-parses every reachable module
— including `std.console` — and renumbers every node from zero.

Session mode keeps the compile state across entries: the loader, the scope
state, the resolver, and the lowering arena live in the session record, and each
entry appends to them. This document is the design, and it leans on the two
properties of the current pipeline that make the whole thing possible:

1. **Globals are numbered program-wide, one contiguous range per module, and
   the root module is last.** Appending definitions to the root module extends
   *its* range only; every other module keeps its `globals_start` and
   `globals_count` exactly.
2. **Functions are numbered the same way** — an append-only counter, handed out
   while bodies are walked in the same dependency-first order.

What is *not* stable is the node arena: it starts empty and is rebuilt from
source every time. Carrying it forward is the real work.

---

## What is already stable

### Global indices — stable by construction

The one counter and the reason it is safe
([dreams/scope.dr](dreams/scope.dr#L110)):

```
let state = %{ :envs => [], :pending => [], :diags => [], :next_global => 0, ... };
let take_global s = [core.map_put s :next_global (next_global s + 1), next_global s];
```

`declare_module` captures its offset before doing anything else
([dreams/scope.dr](dreams/scope.dr#L527)):

```
let globals_from = next_global s0;
```

...hands out a fresh index per declaration in order, and records the module as a
*range* ([dreams/scope.dr](dreams/scope.dr#L590)):

```
core.map_put checked :module_recs
    (list.append (s_module_recs checked)
                 [[mi, globals_from, next_global checked - globals_from, ...]])
```

A global's index is the VM's, flat and program-wide; the image stores a module
as `globals_start`/`globals_count`
([dream/src/image.hpp](dream/src/image.hpp#L120)), and the `Global` opcode's `a`
operand is that flat index ([dream/src/interp.cpp](dream/src/interp.cpp#L808)).
Cross-module references already carry the flat program-wide index by the time
scope finishes ([dreams/scope.dr](dreams/scope.dr#L1061),
[dreams/scope.dr](dreams/scope.dr#L966)).

Modules are appended only after their dependencies
([dreams/modules.dr](dreams/modules.dr#L68)), so the order is *dependencies
first, root last*, and the root is the last module loaded
([dreams/lower.dr](dreams/lower.dr#L878)). The consequence for a session:

- a new `let` in the root module takes the next sequential index — which is
  inside the root's range, **after** everything before it;
- no earlier module's globals renumber;
- nothing earlier references a root global (a dependency does not know the root
  exists; only the root references dependencies), so nothing earlier needs
  touching when the root grows.

The same append-only reasoning holds for the import records (`take_import`,
[dreams/scope.dr](dreams/scope.dr#L128)) and for the per-module `core` global
`declare_core` takes only when a module mentions `core`
([dreams/scope.dr](dreams/scope.dr#L396)) — all of them are declared inside the
module that needs them, and the root is last.

### Function indices — stable by construction

`begin_func` hands out indices from an append-only counter
([dreams/scope.dr](dreams/scope.dr#L759)):

```
let fi = func_count r;
let with_func =
    core.map_put (core.map_put r :funcs
                      (core.map_put (func_map r) fi (func_rec ...)))
                 :func_count (fi + 1);
```

The comment says it outright: *"The index is stable: functions are only ever
appended."* Functions are reserved during the check phase, which walks
`pending` in declaration order — dependencies first, root last
([dreams/scope.dr](dreams/scope.dr#L1343)). New root bodies walk last, so they
get the highest function indices and nothing earlier shifts.

### Constant pools — stable if carried over

The integer, float, string and atom pools are `intern` maps: a value keeps its
first-seen index ([dreams/ir.dr](dreams/ir.dr#L259)). Carrying the pools forward
means appending new constants does not disturb the indices of existing ones.

---

## What is not stable — and why it is the work

### The node arena

`ir.program` starts empty ([dreams/ir.dr](dreams/ir.dr#L197)) and `lower_program`
walks every body in order, appending to one shared arena
([dreams/lower.dr](dreams/lower.dr#L906)). Node indices are assigned by
`push_node` from `node_count` ([dreams/ir.dr](dreams/ir.dr#L242)), so they are a
position in the whole program. A fresh build renumbers everything. To make nodes
stable, the arena itself must be carried forward and only appended to.

The good news inside that: nodes are *referenced*, not position-relative. A
parent holds a child index, a call holds a target function index, a flag is
`set_node_flag` *in place* ([dreams/ir.dr](dreams/ir.dr#L276)), a compile-time
placeholder is replaced *in place* by `set_node` at the same index
([dreams/lower.dr](dreams/lower.dr#L1025)). Appending nodes never moves an
existing one. So an arena that is only ever appended to is safe by this design;
the entire reset happens because `lower_program` builds a *fresh* `ir.program`.

### The link tables

`link_funcs` walks `scope.funcs r` in index order and builds a `FuncRec` per
function; `link_globals` walks `scope.r_global_recs r` in declaration order
([dreams/lower.dr](dreams/lower.dr#L812),
[dreams/lower.dr](dreams/lower.dr#L838)). Both produce lists. As long as the
*lists are carried forward and appended to* — not rebuilt from the resolver each
time — the records for earlier modules keep their positions. `place_captures`
appends capture runs to the shared kids pool
([dreams/lower.dr](dreams/lower.dr#L799)), same rule.

One asymmetry to respect: `scope.funcs r` regenerates the whole list on demand
([dreams/scope.dr](dreams/scope.dr#L670)). `link_funcs` must be fed the carried
`funcs` list, not a freshly materialised view, or re-linking would re-derive
(identical but rebuilt) records. The records for a session are append-only
just like the globals.

### The loader and parser

`modules.load!` builds a fresh loader from scratch every call
([dreams/modules.dr](dreams/modules.dr#L643)), so the parse cache does not
survive. The loader already *has* the machinery session mode needs — modules are
deduplicated by `:by_name`/`:by_path`, and a persisted loader's `:parsed` map
means a dependency whose text is unchanged is not lexed or parsed again
([dreams/modules.dr](dreams/modules.dr#L78)). `load_overlaid_cached!`
([dreams/modules.dr](dreams/modules.dr#L662)) is exactly the shape to build on:
it threads a previous loader's parse cache in and hands a loader back out. A
session does the same, but with the whole loader — modules, by-name, by-path,
host modules, and parsed map — reused rather than just the parse cache.

---

## The session state

Extend the session record
([dreams/repl.dr](dreams/repl.dr#L51)) with what a compile leaves behind:

```
let session imports defs =
    %{ :imports => imports, :defs => defs,
       :loader => (), :scope_state => (), :resolver => (),
       :arena => ir.program, :pools => ..., ... };
```

The pieces and where each one came from:

| Field | What it is | Filled by |
|---|---|---|
| `loader` | the `modules.load!` result, kept | `modules.load!` |
| `scope_state` | `scope.state` after the declare phase | `declare_module` |
| `resolver` | `scope.resolve!` result | the check phase |
| `arena` | `ir.program` under construction | `lower_program` / `settle_comps!` |
| `funcs`, `globals`, `imports`, `modules` (image lists) | the link output | `link_*` |

The watermarks that make "append only" checkable:

| Watermark | Meaning |
|---|---|
| node/kid count | where the root module's bodies began in the arena |
| `next_global` | where the root's globals began |
| `func_count` | where the root's functions began |
| root module index | the position of the session module in `modules` |

---

## The incremental pipeline

### Case A — the common entry: header unchanged, one statement appended

The session header (imports) is unchanged, and the new entry is a `let`/`mod` /
an evaluation. Then the module list does not change and nothing before the root
moves. Concretely:

1. **Grow the root module.** Re-parse only the session text (the root module's
   own file was re-written anyway), and extend the root module's `:items`, or —
   simpler and equivalent — append the new statement to the loader-held root
   module's `:items` list and re-parse nothing else.
2. **Declare.** Re-run `declare_module` for the root module only, seeded with the
   carried `scope_state`. `next_global` continues, so the new statements take
   the next indices. Existing envs, imports, global records and module records
   are untouched. The root's `module_rec` is re-appended with its grown
   `globals_count`; its `globals_start` is unchanged.
3. **Resolve.** Re-run the check phase for just the new pending bodies. The
   resolver is carried, so `func_count` continues and the new functions take the
   next indices. `refs` are keyed per occurrence
   ([dreams/scope.dr](dreams/scope.dr#L700)) and newly recorded keys are new;
   nothing earlier is overwritten.
4. **Lower.** Re-run `lower_expr`/`mark_tail` for just the new bodies into the
   carried arena. `push_node`/`push_kids` continue from the watermark. Update
   `func_bodies` for the new functions' body nodes. Existing nodes are untouched.
5. **Link.** Append `FuncRec`/`GlobalRec`/`ImportRec`/`ModuleRec` records for the
   new functions and globals to the *carried* image lists (not re-derived).
   `place_captures` for the new functions appends to the kids pool.
6. **Compile-time values.** New `comp` placeholders are in the arena; run
   `settle_comps!` for the new pending comps only. Earlier comps were already
   settled — their placeholder was replaced in place and the resulting nodes are
   in the arena, so they are not re-run.
7. **Finish and run.** `ir.finish` reads the accumulated arena
   ([dreams/ir.dr](dreams/ir.dr#L295)) and produces the image. Recompute `entry`
   (`find_entry`, [dreams/lower.dr](dreams/lower.dr#L878)),
   `module_name`/`source_name` from the root module.

Steps 2–5 each cost only the new content, and their union is: **one production
module rewalked, nothing else**.

### Case B — the header changed: a new `import`

A new import is a new dependency, and dependencies are loaded *before* the root.
Inserting one between the previous dependencies and the root shifts the root's
module index and its `globals_start`. The design rule for this case is narrow:

- Load the new dependency (it is appended to `modules` after the previous
  dependencies, in their position — before the root).
- Only the root changes: re-declare it (a new module, from its current index),
  re-resolve *all* of its bodies, re-lower them, drop its old arena nodes and
  its old function records.
- Truncate the arena back to the root's node watermark and drop the root's
  `func_bodies` entries (`fi >= root_first_func`). Nothing outside the root
  references those nodes: a dependency has no handles into the root, and the
  root's own nodes all live in that range. The pools may keep entries that only
  the dropped nodes referenced — harmless waste, and interning keeps indices of
  surviving values stable.

This is why the session keeps `imports` separate from `defs`
([dreams/repl.dr](dreams/repl.dr#L45)): the header is a small, explicitly
tracked thing, and a new import is the one edit that costs a re-resolve of the
root. Everything else reuses outright.

A `:reset` (see below) or a *changed* existing import is treated as case B with
a wiped root.

---

## Image writing

The image sections are rendered independently in `emit.dr`
([dreams/emit.dr](dreams/emit.dr#L198)): ints, floats, strings blob+table,
atoms, nodes, kids, funcs, globals, imports, modules. With the pools carried,
the first sections are byte-identical between compiles and only
nodes/kids/funcs/globals/imports/modules change. Two options:

- **cheap:** write the whole image each time, as today. Correct, and the
  constant pools' bytes are recovered by the VM naturally. (Zero risk, keep the
  old writer.)
- **cheaper (later):** cache each section's rendered bytes and re-render only
  the changed ones. The section table carries lengths, so concatenating the
  cached prefix with fresh tail sections is all the writer would do.

The run step (`os.exec!` on the image,
[dreams/repl.dr](dreams/repl.dr#L211)) is unchanged — the image is still a file
and a child VM still runs it. Byte equality with a fresh build of the same text
is *not* a goal of session mode (comp value nodes land after the new bodies'
nodes instead of interleaved), and it does not need to be: correctness is
"indices never change meaning", which the append-only invariants deliver.

---

## Interaction with the rest of the REPL

- **The entry point is always a fresh `let main!`.** Every entry compiles the
  session with a new `echo`/`quiet_main` body —
  [dreams/repl.dr](dreams/repl.dr#L74). The root module grows by one statement
  per entry, and `main!` is not accumulated in `defs`, so nothing gets
  redefined. In session mode the new `main!` is just another appended let: case
  A.
- **`with_import` dedupes** ([dreams/repl.dr](dreams/repl.dr#L60)) so the header
  only grows, which is the only direction case B handles cheaply. A session that
  wants to drop an import is a `:reset`.
- **`:reset`** wipes the session; it also resets loader/scope/resolver/arena to
  empty and reloads `std.console`. It is the one place that returns to a fresh
  build from nothing — the current code path, kept.
- **`:load`** enters a file's declarations through `define!`
  ([dreams/repl.dr](dreams/repl.dr#L289)); each line is an appended statement,
  so it is N applications of case A.
- **Errors keep being whole-session errors.** Session mode still reports a
  diagnostic against the *whole* session text (`as_session`, and the scratch
  file path is rendered per [dreams/repl.dr](dreams/repl.dr#L186)), unchanged
  from today. The failed entry is not kept
  ([dreams/repl.dr](dreams/repl.dr#L269)).
- **Diagnostics.** Loader and resolver diagnostics for earlier modules are kept
  and replayed (`modules.diags`, `scope.r_diags`); only the new statements add
  to them.

---

## Hazards

- **Anything that reorders the module list shifts the root.** The rule "root is
  last" is load order, not an invariant of a file. Session mode owns its loader
  so nothing else can reorder it — but *mod* declarations inside the session
  text put a nested module into the same root file. A nested module is a
  separate module recorded *while the root file is still loading*, i.e. it lands
  before the root in the list but after the previous dependencies. Its globals
  come from the same counter; the root is still last; it behaves like case B
  with a new module that is not an import. Worth naming in the implementation:
  the "root segment" that case B re-resolves is *everything from the first
  module the session text itself declares onward*, not literally the single
  last record.
- **`derive`.** A derived body is re-walked once per derived module
  ([dreams/scope.dr](dreams/scope.dr#L695)); if the session derives something,
  its effective items change when the session changes and the derived module's
  segment must follow the root-segment rule. First version: treat any session
  containing `derive` as case B for the deriving module.
- **Wrapper globals.** Lowering rewrites calls to wrapper globals into what they
  stand for ([dreams/lower.dr](dreams/lower.dr#L209)). The wrappers map is keyed
  by global index and lives in the resolver; carried forward it stays correct,
  and a *new* wrapper in the root only affects the root's own newly lowered
  nodes. But a wrapper's `target` is a function index, so `link_globals` for a
  newly declared wrapper must be re-run for that record only — already covered
  by step 5.
- **`virtual` / `unimplemented`** in the root fills a virtual at declare time
  ([dreams/scope.dr](dreams/scope.dr#L557)) and is handled by case A/B exactly
  like any other let.
- **Timers and divergence.** Session mode does not change the rule in
  [CLAUDE.md](../CLAUDE.md): every VM run still needs a timeout, because
  evaluating an entry is running a program.

---

## Landing it

The append-only invariants are already in the code; the work is plumbing, not
re-architecture. Suggested order:

1. **Thread a loader.** Add load-overlaid-cached-style persistence to `compile!`
   in the REPL: keep the loader across entries, re-parse only the session file.
   Measure — this removes the std library re-parse at once.
2. **Carry scope state and arena.** Do case A end to end with a verifying
   harness: for a session, compile incrementally and compile fresh, and diff the
   *stable* parts (all globals except the root's tail, all functions, all nodes
   up to the root's watermark). The diff harness is the correctness test.
3. **Carry image lists.** Append to `funcs`/`globals`/`imports`/`modules` and
   stop re-deriving them. The `--time` and `--profile` tools
   ([CLAUDE.md](../CLAUDE.md)) measure the win.
4. **Case B** (new imports, `mod`, `derive` in the session) with the
   truncate-root-segment rule.
5. **Section-level image write caching**, if the measurements justify it.