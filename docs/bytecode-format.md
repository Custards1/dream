# Dream bytecode format (`.dream`), version 0.1

A `.dream` file holds one compiled module. It is designed around a single
constraint: **the VM evaluates lazily, so the program *is* a tree of nodes, and
loading must not rebuild that tree.**

Everything is therefore stored as flat arrays of fixed-size, little-endian
records, and every edge is an *index* rather than a pointer or an offset. A
loader can `mmap` the file, slice the `NODE` and `KIDS` sections, and begin
forcing nodes immediately — no parsing, no relocation, no allocation
proportional to program size.

All multi-byte integers are little-endian. All section offsets are 8-byte
aligned. The sentinel `0xFFFFFFFF` (`NO_NODE`) marks an absent edge.

## Layout

```
+--------------------------+  0
| Header                   |  32 bytes
+--------------------------+  32
| Section table            |  16 bytes x section_count
+--------------------------+
| Sections, in table order |  each padded to an 8-byte boundary
+--------------------------+
```

### Header (32 bytes)

| Offset | Size | Field           | Notes                                        |
|-------:|-----:|-----------------|----------------------------------------------|
| 0      | 8    | `magic`         | ASCII `DAGNCAAF`                             |
| 8      | 2    | `version_major` | `0`; a mismatch must be rejected             |
| 10     | 2    | `version_minor` | `1`; a higher minor is forward-compatible    |
| 12     | 4    | `flags`         | bit 0: a `SPAN` debug section is present     |
| 16     | 4    | `module_name`   | string-table index                           |
| 20     | 4    | `source_name`   | string-table index; the original source path |
| 24     | 4    | `entry`         | function index of `main!`, or `NO_NODE`      |
| 28     | 4    | `section_count` |                                              |

### Section table entry (16 bytes)

| Offset | Size | Field    | Notes                                          |
|-------:|-----:|----------|------------------------------------------------|
| 0      | 4    | `kind`   | four ASCII bytes, read as a little-endian `u32` |
| 4      | 4    | `offset` | absolute byte offset into the file              |
| 8      | 4    | `length` | byte length, excluding alignment padding        |
| 12     | 4    | `count`  | number of records (bytes, for `SBLB`)           |

Unknown section kinds must be skipped, not treated as an error.

## Sections

| Kind   | Record                                              | Purpose |
|--------|-----------------------------------------------------|---------|
| `KINT` | `i64`                                               | integer constants |
| `KFLT` | `f64` (raw bit pattern)                             | float constants |
| `KSTR` | `u32 offset, u32 length`                            | string table, into `SBLB` |
| `SBLB` | raw UTF-8 bytes                                     | string character data |
| `KATM` | `u32`                                               | atom table; each entry is a `KSTR` index |
| `NODE` | 16 bytes (below)                                    | the execution trees |
| `KIDS` | `u32`                                               | variadic child lists and capture descriptors |
| `FUNC` | 32 bytes (below)                                    | function records |
| `GLBL` | 16 bytes (below)                                    | module-level bindings |
| `IMPT` | `u32 path, u32 alias`                               | imports; both are `KSTR` indices |
| `SPAN` | `u32 start, u32 end`                                | per-node source spans, parallel to `NODE` |

Atoms are interned separately from strings so the VM can compare them by
identity: two occurrences of `:ok` anywhere in a module share one atom index.

### Node (16 bytes)

| Offset | Size | Field   |
|-------:|-----:|---------|
| 0      | 1    | `op`    |
| 1      | 1    | `flags` |
| 2      | 2    | `aux`   | reserved |
| 4      | 4    | `a`     |
| 8      | 4    | `b`     |
| 12     | 4    | `c`     |

**Flags.** These are the compiler's answer to "when does this run?", which a
lazy VM cannot work out for itself:

- `0x01 STRICT` — force this node where it appears. Set on block statements
  whose effects must be ordered, on `if` conditions, and on `try!` bodies
  (whose result must materialize while the handler is still installed).
- `0x02 IMPURE` — this node performs, or transitively contains, an effect.
- `0x04 TAIL` — a call in tail position; the VM may reuse the frame.

**Opcodes.**

| # | Op | `a` | `b` | `c` |
|--:|----|-----|-----|-----|
| 0 | `nop` | | | |
| 1 | `int` | `KINT` index | | |
| 2 | `float` | `KFLT` index | | |
| 3 | `str` | `KSTR` index | | |
| 4 | `char` | Unicode scalar value | | |
| 5 | `bool` | 0 or 1 | | |
| 6 | `atom` | `KATM` index | | |
| 7 | `unit` | | | |
| 8 | `local` | frame slot | | |
| 9 | `capture` | capture index | | |
| 10 | `global` | `GLBL` index | | |
| 11 | `builtin` | builtin id | | |
| 12 | `field` | object node | `KSTR` index of the name | |
| 13 | `apply` | callee node | `KIDS` offset | argument count |
| 14 | `if` | condition | then | else |
| 15 | `block` | `KIDS` offset | statement count | |
| 16 | `bind` | frame slot | value node | |
| 17 | `closure` | `FUNC` index | | |
| 18 | `thunk` | `FUNC` index | | |
| 19 | `try` | body | handler | slot bound to the error |
| 20 | `force` | node | | |
| 21–25 | `add` `sub` `mul` `div` `mod` | lhs | rhs | |
| 26–31 | `eq` `ne` `lt` `le` `gt` `ge` | lhs | rhs | |
| 32–33 | `and` `or` | lhs | rhs (forced only if needed) | |
| 34–35 | `neg` `not` | operand | | |
| 36–37 | `list` `array` | `KIDS` offset | element count | |
| 38 | `map` | `KIDS` offset | pair count (`2n` kids: `k0 v0 k1 v1 …`) | |

A `block` evaluates to its last statement. An empty block evaluates to unit.

### Function record (32 bytes)

| Offset | Size | Field          | Notes |
|-------:|-----:|----------------|-------|
| 0      | 4    | `name`         | `KSTR` index |
| 4      | 4    | `body`         | `NODE` index |
| 8      | 2    | `arity`        | declared parameters; they occupy slots `0..arity` |
| 10     | 2    | `flags`        | see below |
| 12     | 2    | `slots`        | frame size: parameters plus block-local bindings |
| 14     | 2    | `n_captures`   | |
| 16     | 4    | `captures_off` | `KIDS` offset of `n_captures` capture descriptors |
| 20     | 4    | `span_start`   | |
| 24     | 4    | `span_end`     | |
| 28     | 4    | reserved       | must be zero |

Function flags: `0x01 IMPURE`, `0x02 REC`, `0x04 THUNK` (a generated 0-arity
body), `0x08 GLOBAL_VALUE` (a pure top-level value: force once, then memoize).

A 0-arity function is a lazy value when pure and an action when impure. That
distinction is the whole difference between `let x = expensive ()` — computed at
most once — and `let go! = { print! "hi" }`, which must print on every call.

**Capture descriptors.** Each is a `u32` read at closure-creation time against
the *defining* frame:

- high bit clear: take slot `value` of the parent frame
- high bit set: take capture `value & 0x7FFFFFFF` of the parent's capture list

Chaining through the second form is what lets a deeply nested closure reach a
variable several frames up without the intermediate frames referring to it.
Captures bind the parent's *cell*, not its current value, so a `let rec` closure
can capture its own binding before it is filled in.

### Global record (16 bytes)

| Offset | Size | Field    | Notes |
|-------:|-----:|----------|-------|
| 0      | 4    | `name`   | `KSTR` index |
| 4      | 4    | `kind`   | 0 = function, 1 = module |
| 8      | 4    | `target` | `FUNC` index, or `IMPT` index for a module |
| 12     | 4    | `flags`  | `0x01` exported, `0x02` impure |

## Builtins

Resolved by index, so the order is append-only:

| id | name | signature |
|---:|------|-----------|
| 0 | `spawn!` | thunk → process |
| 1 | `join!` | process → value |
| 2 | `send!` | process → value → unit |
| 3 | `recv!` | unit → value |
| 4 | `self!` | unit → process |
| 5 | `raise!` | value → never |
| 6 | `type_of` | value → atom |
| 7 | `to_string` | value → string |
| 8 | `len` | list \| array \| map \| string → integer |

A binding of the same name shadows the builtin.

## Notes for the VM

- **Forcing.** Every node is a suspension. Force a node to weak head normal
  form; memoize the result in the node's activation, not in the node itself —
  the `NODE` section is shared across every activation and every thread.
- **Ordering.** Ignore evaluation order except where `STRICT` says otherwise.
  Statements in an impure block carry it; bindings deliberately do not, unless
  their value is effectful.
- **Processes.** `spawn!` takes a `thunk` node's closure and runs it as a new
  process. Whether captured state is shared or copied is the VM's choice: the
  reference implementation copies, giving each process an isolated heap, which
  is what lets it collect and force thunks without atomics. A VM that shares
  instead must blackhole or lock every thunk, because the compiler guarantees
  only that captures are indices — not that forcing is single-threaded.
  The `process` object type is one of the VM's builtin object types; its
  fields are documented in [builtins.md](builtins.md), and `type_of` names it
  from `bi_type_of` in `dream/src/builtins.cpp`.
- **Validation.** A loader should bounds-check every index against its
  section's `count` before use, since a `.dream` file may be untrusted.
