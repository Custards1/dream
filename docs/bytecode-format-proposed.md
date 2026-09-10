# Proposed bytecode-format changes

This file is the *proposal* for additions to `bytecode-format.md`, kept
separate until the format work lands. The changes documented here are not yet
reflected in the format spec.

## Why

`MODS` exists in the format already — it is emitted (`dreams/emit.dr:213`),
validated (`dream/src/image.cpp:284`), and read by the VM
(`dream/src/image.cpp:296-304`) — but it is missing from the section table in
`docs/bytecode-format.md`. Merging images (see `docs/dynamic-linking.md`) makes
a module's global range the identity that survives the merge, so the record
deserves documenting. Two additions:

1. A `MODS` row in the section table.
2. A module-record description beside "Global record".

## Published spec, current

From `docs/bytecode-format.md`:

| Kind   | Record                                              | Purpose |
|--------|-----------------------------------------------------|---------|
| `IMPT` | `u32 path, u32 alias`                               | imports; both are `KSTR` indices |
| `SPAN` | `u32 start, u32 end`                                 | per-node source spans, parallel to `NODE` |

## Proposed

| Kind   | Record                                              | Purpose |
|--------|-----------------------------------------------------|---------|
| `IMPT` | `u32 path, u32 alias`                               | imports; both are `KSTR` indices |
| `MODS` | 24 bytes (below)                                    | module records: which globals belong to which module |
| `SPAN` | `u32 start, u32 end`                                 | per-node source spans, parallel to `NODE` |

### Module record (24 bytes)

| Offset | Size | Field           | Notes |
|-------:|-----:|-----------------|-------|
| 0      | 4    | `name`          | `KSTR` index |
| 4      | 4    | `source`        | `KSTR` index; the original source path |
| 8      | 4    | `globals_start` | first global index belonging to this module |
| 12     | 4    | `globals_count` | |
| 16     | 4    | `flags`         | `0x01` host-provided, `0x02` declares virtuals |
| 20     | 4    | `derives`       | `MODS` index of the derived module, or `NO_NODE` |

A module's globals form a contiguous range `globals_start .. globals_start +
globals_count`; the range is what lets a reader say which module a global
belongs to without the `GLBL` records carrying a module id of their own.

The record matches `ModuleRec` in `dream/src/image.hpp` and `module_bytes` in
`dreams/emit.dr`. The `MODS` section is validated like any other record array at
load; unknown sections are skipped, so a reader predating this documentation is
unaffected.