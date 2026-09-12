# Dream Built-in Functions Reference

This document covers every built-in function available in Dream, organized by module. There are three layers:

1. **Language builtins** — available everywhere, no import needed
2. **Native modules** — C++ modules imported with `import std.<name>`
3. **Dream stdlib** — modules written in Dream, in `mind/std/`

---

## Language Builtins

These are resolved by the compiler without any import. They can be shadowed by a local binding.

| Name | Signature | Description |
|------|-----------|-------------|
| `spawn!` | `thunk → process` | Starts a new process to evaluate `thunk`. The argument is **not** forced before being handed off — the new process does the work. |
| `join!` | `process → value` | Waits for a process to finish and returns its result. If the process raised, the error propagates here. Parks the calling process, not the worker thread. |
| `send!` | `process → value → unit` | Deep-copies `value` into the mailbox of `process`. The message is fully evaluated before leaving — thunks carrying this process's heap cannot be shared. |
| `recv!` | `unit → value` | Takes the next message from this process's mailbox. Parks until one arrives. |
| `self!` | `unit → process` | Returns the current process's handle. |
| `raise!` | `value → never` | Raises `value` as an error. If `value` is already an error box it is re-raised as-is; otherwise it is wrapped in one. Never returns. |
| `type_of` | `value → atom` | Returns an atom naming the type of its argument: `:integer`, `:float`, `:char`, `:bool`, `:unit`, `:string`, `:atom`, `:list`, `:array`, `:map`, `:module`, `:error`, `:process`, `:pure_fn`, `:impure_fn`, or `:bigstr` (a view into the image's payload — see [Large data](#large-data) below; deliberately *not* `:string`, so that no code path written for a string is handed one). |
| `to_string` | `value → string` | Renders any value as a human-readable string. Lists print as `[a, b, c]`, arrays as `#[a, b, c]`, maps as `%{:k => v}`, strings are quoted, chars as `'c'`. Raises `:type_error` on a bigstr, which by definition may not fit in a string; one nested inside a larger value renders as `<big string, N bytes>` rather than losing the rest of the structure. |
| `len` | `list\|array\|map\|string\|bigstr → integer` | Returns the number of elements (list), slots (array), entries (map), or bytes (string or bigstr). Raises `:type_error` for anything else. For lists, walks the entire spine. |
| `strict!` | `value → value` | Forces `value` all the way to normal form (deeply, not just to weak head normal form), then returns it unchanged. Use this when laziness would defer an effect — `list.map (fn x -> spawn! ..) xs` builds thunks; `strict! (list.map ...)` runs the spawns immediately. |

### Pattern-match internals

These are emitted by the compiler for `match` expressions. They are technically builtins but are not normally called directly.

| Name | Description |
|------|-------------|
| `match_is_cons v` | Returns `true` if `v` is a cons cell (a non-empty list). |
| `match_head v` | Returns the head of a cons cell, unforced. |
| `match_tail v` | Returns the tail of a cons cell, unforced. |
| `match_at v i` | Returns element `i` of array `v`, unforced. |
| `match_key map key` | Returns `[value]` if `key` is in `map`, `[]` if absent. |

---

## `std.core`

Primitives that the language cannot express in itself — things the runtime representation hides (string bytes, map buckets) or operations that must be a single machine step.

```dream
import std.core;
```

### Lists

| Name | Signature | Description |
|------|-----------|-------------|
| `head` | `list → value` | The first element. Raises `:type_error` on an empty list. |
| `tail` | `list → list` | Everything after the first element. Raises `:type_error` on an empty list. |
| `cons` | `value → list → list` | A new cons cell with the given head and tail. Both sides stay lazy. |
| `is_empty` | `list → bool` | `true` if the list is `[]`. |

### Strings

Strings are byte-indexed internally (UTF-8 storage). Offsets in the functions below are **byte** offsets, not character offsets.

| Name | Signature | Description |
|------|-----------|-------------|
| `str_len` | `string\|bigstr → integer` | The byte length of the string. |
| `str_chars` | `string → list of char` | Decodes the string to a list of Unicode codepoints (characters). Bytes that do not spell a Unicode scalar value — a stray or truncated sequence, an overlong form, a surrogate, anything past U+10FFFF — each become U+FFFD, one per byte, so every char it yields is one `char_of_code` would accept. Raises `:type_error` on a bigstr. |
| `str_of_chars` | `list of char → string` | Encodes a list of characters into a UTF-8 string. |
| `str_of_bytes` | `list of integer → string` | Builds a string from raw byte values, each `0`–`255`. The inverse of `str_byte`, and the way to produce **binary** output: `str_of_chars` UTF-8-encodes its input, so byte `0x80` would become two bytes. Raises `:type_error` for a non-integer or a value outside `0`–`255`. A `0` byte is an ordinary byte and does not end the string. |
| `str_slice` | `string\|bigstr → start:integer → len:integer → string\|bigstr` | Returns `len` bytes starting at byte offset `start`. Clamped silently — running past the end is how string-walking loops finish. A slice of a bigstr is another bigstr view, however small: no copy, at any size. |
| `str_find` | `haystack:string → needle:string → from:integer → integer` | Returns the byte offset of the first occurrence of `needle` at or after `from`, or `-1` if not found. Raises `:type_error` on a bigstr. |
| `str_byte` | `string\|bigstr → index:integer → integer` | The raw byte value (0–255) at byte `index`, or `-1` if out of range. |
| `str_concat` | `list of string → string` | Joins the parts, copying each exactly once. Raises `:type_error` on a bigstr. |

### Chars

| Name | Signature | Description |
|------|-----------|-------------|
| `char_code` | `char → integer` | The Unicode scalar value (codepoint) of a character. |
| `char_of_code` | `integer → char` | The character for a Unicode scalar value. Raises `:type_error` if the integer is not a valid Unicode scalar (must be 0–0x10FFFF, excluding surrogates). |

### Numbers

| Name | Signature | Description |
|------|-----------|-------------|
| `to_float` | `integer\|float → float` | Converts a number to `float`. Returns a `float` unchanged. |
| `to_int` | `integer\|float → integer` | Converts a number to `integer`. Truncates toward zero (like C cast). For floor-division behavior use `math.floor` first. |
| `parse_int` | `string → integer\|unit` | Parses a base-10 integer from a string. Returns `unit` on failure. The entire string must be a valid integer (trailing non-numeric characters cause failure). |
| `parse_float` | `string → float\|unit` | Parses a floating-point number from a string. Returns `unit` on failure. |

### Arrays

Arrays are fixed-length, eagerly allocated sequences. Indexing is O(1). All update operations return a new array (values are immutable).

| Name | Signature | Description |
|------|-----------|-------------|
| `array_new` | `length:integer → fill:value → array` | Creates a new array of `length` slots, each initialized to `fill`. |
| `array_get` | `array → index:integer → value` | Returns the element at `index` (forced to WHNF). Raises `:out_of_bounds` if index is out of range. |
| `array_set` | `array → index:integer → value → array` | Returns a new array with the element at `index` replaced. The original is unchanged. Raises `:out_of_bounds` if out of range. |
| `array_of_list` | `list → array` | Converts a list to an array. Elements remain lazy. |
| `array_to_list` | `array → list` | Converts an array to a list. Elements remain lazy. |

### Maps

Maps are persistent hash maps — a hash array mapped trie, branching 32 ways on five bits of the key's hash at a time. Keys are compared by value for flat types (integers, floats, strings, atoms, chars, bools, pids) and by identity for everything else. All updates return a new map.

Persistent means *shared*, not copied: `map_put` rebuilds only the path from the root to the entry it changes — about `log32(n)` nodes — and the map it was given keeps every other node and stays valid. So the ordinary functional way to build a map, folding `map_put` over a sequence, costs `O(n log n)` in total rather than the `O(n²)` a copy-on-write table would. `len` is constant time: every node knows how many entries hang below it.

| Name | Signature | Description |
|------|-----------|-------------|
| `map_new` | `unit → map` | Creates a new empty map. |
| `map_get` | `map → key → default → value` | Returns the value for `key`, or `default` if not present. Both `key` and `default` are forced to WHNF. |
| `map_has` | `map → key → bool` | Returns `true` if `key` is in the map. |
| `map_put` | `map → key → value → map` | Returns a new map with `key` mapped to `value`. The original is unchanged. |
| `map_remove` | `map → key → map` | Returns a new map with `key` removed. |
| `map_pairs` | `map → list` | Returns a list of `[key, value]` pairs in unspecified order. |

### Ordering

| Name | Signature | Description |
|------|-----------|-------------|
| `compare` | `a → b → integer` | Total order comparison. Returns `-1`, `0`, or `1`. Works on flat types in this rank order: integers and floats (numerically), chars, bools, atoms, strings, unit. A bigstr ranks with the strings and compares by its bytes. Values of different types order by their type rank. |

### Large data

The payload region of the image: bytes put there at compile time with
`dreams --payload FILE`, one entry per file in the order given. See
[bytecode-format.md](bytecode-format.md) for the `LDAT` and `PAYL` sections
these read.

| Name | Signature | Description |
|------|-----------|-------------|
| `data_count` | `unit → integer` | How many large data this image carries. `0` for an image built without `--payload`, which is every ordinary image. |
| `data_at` | `integer → bigstr` | Datum `i`, as a **bigstr**: a length and a pointer into the mapped image. Constant time and copies nothing, whatever its size. Raises `:type_error` for an index outside `0 .. data_count () - 1`. |

Both are pure. The payload is fixed when the image is written and nothing can
alter it, so asking for datum `i` is a function of `i` in the way `str_byte` is
a function of its index.

**What a bigstr is for.** A `.dream` file addresses itself with 32-bit offsets,
so the file and every string in it stop at 4 GiB. A bigstr is the exception: it
is not a string but a *view*, and it is never copied — not when it is made, not
when the collector promotes it, and not when `spawn!` hands it to another
process, all of which share the pointer because the bytes belong to the
runtime's image and outlive every process in it.

Operations that need only to read it all work, and none of them copies:

| Works | |
|-------|--|
| `len`, `str.byte_length` | the 64-bit length, as an integer |
| `str.byte` | one byte; pointer arithmetic |
| `str.slice` | another view, clamped like the string case |
| `==`, `!=`, `<`, `<=`, `>`, `>=` | by bytes |
| a map key | hashed once, then remembered on the value |
| `io.write!` | written straight from the mapping, with no intermediate buffer |

Operations that would have to *build* a string out of it raise `:type_error`
with a message naming what to reach for instead: `+`, `to_string`,
`str.concat_all`, `str.find` and `str.chars`. Each would copy the bytes into the
heap and each would be capped at the 4 GiB a string can count, which between
them is the reason the payload is not a string in the first place.

**A bigstr and a string of the same bytes are equal, and hash alike.** That is
what makes the first thing anyone writes — `str.slice 0 4 data == "%PDF"` — mean
what it looks like. `type_of` still tells them apart, so a branch written for
`:string` is never handed one.

```dream
import std.core;
import std.io;

let main! = {
    let data = core.data_at 0;
    if str.slice 0 4 data == "%PDF" {
        io.write! (io.stdout! ()) data       // the whole of it, never in memory
    } else { console.error! "not a PDF" }
};
```

---

## `std.console`

Output to stdout and stderr. All members are **variadic** — they accept any number of arguments, force each, and render them in sequence with no separator.

```dream
import std.console;
```

| Name | Description |
|------|-------------|
| `print! ..` | Writes all arguments to stdout, then a newline. |
| `write! ..` | Writes all arguments to stdout, without a trailing newline. |
| `line! ..` | Alias for `print!`. |
| `error! ..` | Writes all arguments to stderr, then a newline. |

Because these are variadic, they are **never partially applied** — `console.print! "x = "` prints immediately, it does not return a function waiting for a second argument.

```dream
console.print! "hello, world"
console.print! "x = " x ", y = " y
x |> console.print! "x = "   // still one application, prints immediately
```

---

## `std.math`

```dream
import std.math;
```

| Name | Signature | Description |
|------|-----------|-------------|
| `sqrt` | `number → float` | Square root. |
| `abs` | `integer\|float → integer\|float` | Absolute value. Returns the same type as the input. |
| `floor` | `integer\|float → integer` | Rounds down to the nearest integer. |

---

## `std.io`

File and stream I/O. Handles are opaque integers with generation tracking. A stale or closed handle raises `:io_closed` rather than operating on an unrelated file.

On Linux, stream reads and writes that would block park the **calling process**, not the worker thread, so other Dream processes continue running.

```dream
import std.io;
```

### Opening and closing

| Name | Signature | Description |
|------|-----------|-------------|
| `open!` | `path:string → mode:atom → handle` | Opens a file. `mode` must be one of `:read`, `:write`, `:append`, or `:update`. `:write` truncates; `:update` is read-write without truncating. |
| `close!` | `handle → unit` | Closes a handle. Safe to call from a different process than the one that opened it. |
| `is_open!` | `handle → bool` | Returns whether a handle is still open. |

### Reading and writing

| Name | Signature | Description |
|------|-----------|-------------|
| `read!` | `handle → bytes:integer → string` | Reads at most `bytes` bytes. Returns an empty string `""` at end of file. A caller loops until it sees `""`. |
| `write!` | `handle → data:string → integer` | Writes as much of `data` as the descriptor will accept and returns how many bytes were written. A short write is not an error — use a loop (or `write_all!` from the Dream stdlib) to ensure all bytes go out. |
| `flush!` | `handle → unit` | Flushes kernel buffers for regular files (fsync). A no-op for sockets and pipes. |

### Positioning

| Name | Signature | Description |
|------|-----------|-------------|
| `seek!` | `handle → offset:integer → integer` | Seeks to `offset` bytes from the start of the file. Returns the new position. Raises on streams that are not seekable. |
| `size!` | `handle → integer` | Returns the file size in bytes via `fstat`. |

### Querying

| Name | Signature | Description |
|------|-----------|-------------|
| `kind!` | `handle → atom` | Returns `:file`, `:stream`, or `:listener`. |
| `stdin!` | `unit → handle` | A handle for fd 0. Not owned — do not close it. |
| `stdout!` | `unit → handle` | A handle for fd 1. Not owned — do not close it. |
| `stderr!` | `unit → handle` | A handle for fd 2. Not owned — do not close it. |
| `async` | `unit → bool` | Whether epoll-based async I/O is available on this platform. |

### Filesystem operations

| Name | Signature | Description |
|------|-----------|-------------|
| `exists!` | `path:string → bool` | Returns whether a path exists. |
| `is_dir!` | `path:string → bool` | Returns whether a path is a directory. |
| `remove!` | `path:string → unit` | Deletes a file. Raises on failure. |
| `rename!` | `from:string → to:string → unit` | Renames or moves a file. Raises on failure. |
| `mkdir!` | `path:string → unit` | Creates a directory. Silently succeeds if it already exists. |

### Error atoms

`:io_closed` · `:not_found` · `:permission_denied` · `:already_exists` · `:wrong_kind` · `:io_error` · `:type_error`

---

## `std.net`

TCP networking. Sockets are regular I/O handles — `io.read!` and `io.write!` work on them directly. This module adds only the TCP-specific operations.

All blocking operations park the calling process, not the worker thread.

```dream
import std.net;
```

| Name | Signature | Description |
|------|-----------|-------------|
| `listen!` | `port:integer → handle` | Binds a TCP listener on `port`. Use port `0` to let the OS pick a port, then read it back with `port!`. |
| `accept!` | `listener → handle` | Waits for an incoming connection and returns a stream handle. Parks until a connection arrives. |
| `connect!` | `host:string → port:integer → handle` | Connects to a TCP server. Parks during the connect. Raises `:connection_refused` if the server is not listening. |
| `peer!` | `socket → string` | Returns the remote address as `"host:port"`. |
| `port!` | `socket\|listener → integer` | Returns the local port number. Useful after binding to port `0`. |
| `shutdown!` | `socket → unit` | Half-closes the connection (stops sending). The peer sees end-of-file on their read, but this end can still receive. |

### Error atoms

`:connection_refused` · `:connection_lost` · `:address_in_use` · `:timed_out` · `:io_closed` · `:io_error`

---

## `std.os`

Operating system interface: arguments, environment, filesystem traversal, subprocesses.

```dream
import std.os;
```

### Environment

| Name | Signature | Description |
|------|-----------|-------------|
| `args!` | `unit → list of string` | The command-line arguments that follow the image name. |
| `env!` | `name:string → string\|unit` | The value of an environment variable, or `unit` if unset. |
| `set_env!` | `name:string → value:string → unit` | Sets an environment variable for the current process. |

### Directories

| Name | Signature | Description |
|------|-----------|-------------|
| `cwd!` | `unit → string` | The current working directory. |
| `chdir!` | `path:string → unit` | Changes the working directory. Raises `:not_found` or `:os_error` on failure. |
| `list_dir!` | `path:string → list of string` | The entries in a directory, sorted alphabetically, without `.` or `..`. Raises `:not_found` if the path does not exist. |

### Subprocesses

| Name | Signature | Description |
|------|-----------|-------------|
| `exec!` | `program:string → args:list of string → map` | Runs `program` to completion and returns a map `%{ :code, :out, :err, :timed_out }`. `program` is resolved via `PATH`. Parks the calling process — not the worker thread — while the child runs. |
| `exec_for!` | `program:string → args:list of string → timeout_ms:integer → map` | Same as `exec!` but kills the child after `timeout_ms` milliseconds. Sets `:timed_out true` in the result map when the deadline is hit, so the caller can distinguish that from an ordinary non-zero exit code. |
| `replace!` | `program:string → args:list of string → never` | **Becomes** `program`: `execvp`, so this VM — image, heap and every thread — is gone and the named program takes over the process, inheriting the terminal and every open descriptor. Stdio is flushed first. It returns only by failing, raising `:not_found` when the program cannot be run. Use it to hand over to something interactive; `exec!` gives its child pipes, which is right for a compiler and useless for anything that prompts. |

The result map fields:

| Field | Type | Description |
|-------|------|-------------|
| `:code` | `integer` | Exit code. 128 + signal number if killed by a signal. |
| `:out` | `string` | Everything the child wrote to stdout. |
| `:err` | `string` | Everything the child wrote to stderr. |
| `:timed_out` | `bool` | `true` only when killed by `exec_for!`'s timeout. |

### Process and platform

| Name | Signature | Description |
|------|-----------|-------------|
| `monotonic!` | `unit → integer` | Milliseconds from a fixed point in this process's life, from a steady clock that never jumps. Only *differences* between two readings mean anything — that difference is a duration. Timing anything lazy means forcing it first: an unforced value has not run, so a reading around one times the building of a thunk. |
| `now!` | `unit → integer` | Milliseconds since the Unix epoch, from the wall clock. It can jump, forwards or back, so it is what to stamp a log line with and never what to measure a duration with. |
| `pid!` | `unit → integer` | The OS process ID of the running VM. |
| `platform` | `unit → atom` | The current platform: `:linux`, `:macos`, `:windows`, or `:unknown`. |
| `exit!` | `code:integer → never` | Terminates the entire VM immediately with the given exit code. Flushes stdio first. Never returns. |

---

## `std.vm`

Runtime introspection. Useful for monitoring, debugging, and detecting deadlocks.

```dream
import std.vm;
```

### Per-process stats

These report figures for the **calling** process.

| Name | Signature | Description |
|------|-----------|-------------|
| `processes!` | `unit → integer` | The total number of live processes in the runtime right now. |
| `reductions!` | `unit → integer` | The number of reduction steps this process has taken. |
| `collections!` | `unit → integer` | The number of GC collections on this process's heap. |
| `heap_bytes!` | `unit → integer` | Bytes currently allocated in this process's heap. |

### Runtime-wide

| Name | Signature | Description |
|------|-----------|-------------|
| `modules!` | `unit → list of string` | The names of all loaded modules. |
| `has_ffi` | `unit → bool` | Whether the VM was built with libffi support. |
| `async_io` | `unit → bool` | Whether epoll-based async I/O is available. |

### Inspection

| Name | Signature | Description |
|------|-----------|-------------|
| `processes_info!` | `unit → list of map` | A snapshot of every live process. |
| `process_info!` | `process\|integer → map\|unit` | Info for one process by handle or numeric ID. Returns `unit` if the process is not found. |
| `scheduler!` | `unit → map` | Scheduler state: `:workers`, `:idle`, `:live`, `:runnable`, `:queued`, `:io_waiters`, `:reductions`, `:deadlocked`. |
| `io!` | `unit → list of map` | Every open I/O handle: `:handle`, `:fd`, `:kind`, `:busy`, `:waiting` (pid or `unit`). |
| `dump!` | `unit → unit` | Prints the full VM state (scheduler + all processes + all handles) to stderr. Same output as `DREAM_STUCK_SECONDS`. |

Process info map fields: `:id` · `:status` (`:runnable`, `:running`, `:waiting`, `:finished`, `:failed`) · `:waiting_on` (`:recv`, `:join`, `:io`, `:none`) · `:fd` · `:reductions` · `:heap_bytes` · `:collections` · `:mailbox` (count) · `:failed`

---

## `std.ffi`

Foreign function interface for calling C libraries. Requires libffi at build time. Check `vm.has_ffi ()` before using; without it every function except `sizeof` raises. `import std.ffi` always compiles — the failure is at call time, not at load time.

```dream
import std.ffi;
```

### Loading libraries

| Name | Signature | Description |
|------|-----------|-------------|
| `open!` | `name:string\|unit → handle` | Opens a shared library by name. Pass `unit` or `""` to get the current program (everything already linked, which includes the C library on any normal system). |
| `close!` | `handle → unit` | Marks a library handle as closed. Does **not** unmap the library — existing bound functions remain valid to avoid dangling pointers. |

### Binding functions

| Name | Signature | Description |
|------|-----------|-------------|
| `bind!` | `handle → symbol:string → arg_types:list of atom → ret_type:atom → fn` | Resolves `symbol` in `handle` and returns a callable native function with the given type signature. |
| `load!` | `name → symbol → arg_types → ret_type → fn` | Convenience: `open!` then `bind!` in one call. |

**Type atoms** for `bind!` and `sizeof`:

| Atom | C type |
|------|--------|
| `:void` | `void` (return only) |
| `:bool` | `_Bool` / `uint8_t` |
| `:i8` / `:u8` | `int8_t` / `uint8_t` |
| `:i16` / `:u16` | `int16_t` / `uint16_t` |
| `:i32` / `:u32` | `int32_t` / `uint32_t` |
| `:i64` / `:u64` | `int64_t` / `uint64_t` |
| `:f32` | `float` |
| `:f64` | `double` |
| `:ptr` | `void*` (passed as an integer address) |
| `:cstr` | `const char*` (Dream string → NUL-terminated pointer; return → Dream string) |

### Raw memory

| Name | Signature | Description |
|------|-----------|-------------|
| `sizeof` | `type:atom → integer` | Size in bytes for a type atom. Works even without libffi. |
| `alloc!` | `size:integer → integer` | Allocates `size` zero-initialized bytes with `calloc` and returns the address as an integer. |
| `free!` | `address:integer → unit` | Frees memory previously allocated with `alloc!`. |
| `read_cstr!` | `address:integer → string\|unit` | Reads a NUL-terminated C string from `address`. Returns `unit` for a null pointer. |
| `read_u8!` | `base:integer → offset:integer → integer` | Reads one byte at `base + offset`. |
| `write_u8!` | `base:integer → offset:integer → byte:integer → unit` | Writes one byte at `base + offset`. |

---

## Dream-level stdlib (`mind/std/`)

These modules are written in Dream and ship with the compiler. They are built on the native primitives above.

### `std.list`

```dream
import std.list;
```

A lazy singly-linked list. Most operations work on infinite lists. Functions that must see the entire list (`length`, `reverse`, `sort`) say so in their descriptions.

| Name | Description |
|------|-------------|
| `empty` | The empty list `[]`. |
| `head xs` | First element. Raises on empty; see `first_or`. |
| `tail xs` | Everything after the first element. |
| `cons x xs` | Prepends `x`. Lazy in both arguments. |
| `is_empty xs` | `true` if empty. |
| `first_or default xs` | First element, or `default` if empty. |
| `length xs` | Forces the entire spine. Does not terminate on infinite lists. |
| `nth n xs` | Element at index `n` (0-based), or `unit` if out of range. |
| `last xs` | Last element, or `unit` if empty. Forces the entire spine. |
| `append xs ys` | Concatenates two lists. Lazy — `ys` is not touched until the end of `xs` is reached. |
| `reverse xs` | Forces the entire spine. |
| `range from until` | `[from, from+1, ..., until-1]`. The end is exclusive. |
| `from n` | Infinite list `n, n+1, n+2, ...` |
| `repeat x` | Infinite list of `x`. |
| `replicate n x` | `x` repeated `n` times. |
| `map f xs` | Apply `f` to every element. Lazy. |
| `filter keep xs` | Elements for which `keep` returns `true`. |
| `fold f acc xs` | Left fold. Forces the entire spine. |
| `fold_right f acc xs` | Right fold. |
| `take n xs` | The first `n` elements. Terminates on infinite lists. |
| `drop n xs` | Everything after the first `n` elements. |
| `take_while keep xs` | Elements while `keep` holds, then stops. |
| `drop_while skip xs` | Drops elements while `skip` holds, then returns the rest. |
| `zip xs ys` | Pairs of `[x, y]`. Stops at the shorter list. |
| `concat xss` | Flattens a list of lists. |
| `flat_map f xs` | `map` then `concat`. |
| `enumerate xs` | Pairs each element with its index: `[[0, x], [1, y], ...]`. |
| `any pred xs` | `true` if any element satisfies `pred`. Short-circuits. |
| `all pred xs` | `true` if every element satisfies `pred`. Short-circuits. |
| `contains x xs` | `true` if `x` appears in the list. |
| `count pred xs` | Number of elements satisfying `pred`. |
| `index_of x xs` | Index of the first occurrence of `x`, or `-1`. |
| `sum xs` | Sum of all elements. |
| `product xs` | Product of all elements. |
| `minimum xs` | Smallest element by `core.compare`, or `unit` if empty. |
| `maximum xs` | Largest element by `core.compare`, or `unit` if empty. |
| `partition pred xs` | `[passing, failing]` — two lists. |
| `unique xs` | Removes duplicates, keeping the first occurrence. |
| `sort xs` | Stable sort in ascending order by `core.compare`. Forces the entire spine. |
| `sort_by before xs` | Stable sort with a custom comparator `before a b → bool`. |
| `sort_on key xs` | Stable sort ascending by `key` applied to each element. |
| `to_array xs` | Converts to an array (calls `core.array_of_list`). |
| `of_array a` | Converts an array to a list. |
| `force xs` | Forces every element. Useful before `send!`. |

**Accumulator-passing forms.** Several functions above are thin wrappers over a
recursive worker that carries its accumulator as the first argument. The worker
is reachable too, and is the form to use when you already have a partial result
to continue from.

| Name | Description |
|------|-------------|
| `length_from acc xs` | `acc` plus the length of `xs`. `length` is `length_from 0`. |
| `reverse_from acc xs` | `xs` reversed, with `acc` left on the end: `reverse_from [9] [1,2,3]` is `[3, 2, 1, 9]`. This is `reverse` and `append` in one pass. |
| `index_of_from i x xs` | Index of the first `x`, counting as if `xs` started at index `i`; `-1` if absent. `i` is an offset added to the answer, **not** a position to start searching from. |
| `min_by_from best less xs` | The smallest of `best` and the elements of `xs`, by the comparator `less a b → bool`. Returns `best` unchanged on an empty list, which is how `minimum`/`maximum` get a seed without a special case for one-element lists. |
| `unique_from seen xs` | The elements of `xs` not already in the list `seen`, with duplicates removed. Elements in `seen` are dropped from the output. |
| `merge_by before xs ys` | Merges two lists that are **already sorted** by `before`, preserving order. The merge step of `sort_by`; on unsorted input it interleaves rather than sorts. |

---

### `std.str`

```dream
import std.str;
```

UTF-8 text. Derives `std.seq`, so it also exposes `sum`, `any`, `all`, `contains`, `minimum`, `maximum`, `join`, `describe`, and `count` over characters.

**Characters and bytes are distinct.** `length` counts characters; `byte_length`, `slice`, and `find` operate on bytes.

| Name | Description |
|------|-------------|
| `length s` | Number of Unicode characters (decodes the string). |
| `byte_length s` | Number of bytes. Constant time. |
| `is_empty s` | `true` if `s` has zero bytes. |
| `chars s` | List of characters (decoded codepoints). |
| `of_chars cs` | String from a list of characters. |
| `first_or default s` | First character, or `default`. |
| `slice from count s` | `count` bytes from byte offset `from`. Clamped silently. |
| `byte i s` | The byte at offset `i` as an integer, or `-1`. Works on a bigstr, where it is the way in. |
| `find needle s` | Byte offset of `needle` in `s`, or `-1`. |
| `find_from from needle s` | Byte offset of `needle` at or after `from`, or `-1`. |
| `contains_str needle s` | `true` if `needle` appears anywhere in `s`. |
| `starts_with prefix s` | `true` if `s` begins with `prefix`. |
| `ends_with suffix s` | `true` if `s` ends with `suffix`. |
| `concat a b` | Concatenates two strings (same as `a + b`). |
| `join_str sep parts` | Joins a list of strings with `sep` between them. Unlike `seq.join`, does not call `to_string` on each part. |
| `repeat n s` | `s` repeated `n` times. |
| `split sep s` | Splits `s` on every occurrence of `sep`. An empty separator returns `[s]`. |
| `map f s` | Applies `f` to every character and rebuilds the string. |
| `filter keep s` | Keeps characters that satisfy `keep`. |
| `reverse s` | Reverses the string by characters (not bytes). |
| `upper s` | ASCII uppercase. Non-ASCII characters are unchanged. |
| `lower s` | ASCII lowercase. Non-ASCII characters are unchanged. |
| `trim s` | Removes leading and trailing whitespace (space, tab, newline, carriage return). |
| `trim_start s` | Removes leading whitespace. |
| `trim_end s` | Removes trailing whitespace. |

**Character-level helpers.** These take and return a `char`, not a string, and
are what `upper`, `lower` and the `trim` family are written in terms of.

| Name | Description |
|------|-------------|
| `is_space c` | `true` for space, tab, newline or carriage return. |
| `upper_char c` | ASCII uppercase of one character. Anything outside `a`–`z` is returned unchanged. |
| `lower_char c` | ASCII lowercase of one character. Anything outside `A`–`Z` is returned unchanged. |

---

### `std.array`

```dream
import std.array;
```

Fixed-length sequences with O(1) indexing, written `#[a, b, c]`. Derives `std.seq`, so it also exposes `sum`, `any`, `all`, `contains`, `minimum`, `maximum`, `join`, `count`, `describe`, and related functions.

| Name | Description |
|------|-------------|
| `length xs` | Number of elements. O(1). |
| `is_empty xs` | `true` if empty. |
| `get xs i` | Element at index `i`. Raises `:out_of_bounds` if out of range. |
| `get_or default xs i` | Element at index `i`, or `default` if out of range. |
| `first_or default xs` | First element, or `default`. |
| `last_or default xs` | Last element, or `default`. |
| `new n fill` | Array of `n` slots all set to `fill`. |
| `of_list xs` | Array from a list. |
| `to_list xs` | List from an array. |
| `build n f` | `#[f 0, f 1, ..., f (n-1)]`. |
| `set xs i v` | A copy of `xs` with index `i` replaced by `v`. |
| `map f xs` | Apply `f` to every element, return a new array. |
| `filter keep xs` | Elements satisfying `keep`, as a new (shorter) array. |
| `reverse xs` | Elements in reverse order. |
| `sort xs` | Sorted ascending by `core.compare`. |
| `sort_by before xs` | Sorted with a custom comparator. |
| `append xs ys` | Concatenation of two arrays. |
| `slice from until xs` | Elements from index `from` up to (not including) `until`. |

---

### `std.seq`

```dream
derive std.seq;
```

Generic sequence interface. Not imported directly — a module derives it and provides `fold`. `std.array` and `std.str` both do this. Operations over characters or array elements are all written in terms of `fold`.

| Name | Description |
|------|-------------|
| `fold f init xs` | *(virtual — must be implemented by the deriving module)* |
| `name xs` | *(virtual, optional — returns a string for use in `describe`)* |
| `length xs` | Count by folding (override in the deriving module if O(1) is available). |
| `is_empty xs` | `true` if `length` is 0. |
| `sum xs` | Sum of all elements. |
| `product xs` | Product of all elements. |
| `count keep xs` | Elements satisfying `keep`. |
| `any keep xs` | `true` if any element satisfies `keep`. Folds the whole sequence. |
| `all keep xs` | `true` if every element satisfies `keep`. |
| `contains wanted xs` | `true` if any element equals `wanted`. |
| `minimum xs` | Smallest element, or `unit` if empty. |
| `maximum xs` | Largest element, or `unit` if empty. |
| `minimum_by rank xs` | Element for which `rank` is smallest. |
| `maximum_by rank xs` | Element for which `rank` is largest. |
| `extreme_by better xs` | The element that `better a b → bool` prefers over every other, or `unit` if empty. `minimum` is `extreme_by (fn a b -> a < b)` and `maximum` is `extreme_by (fn a b -> a > b)`. |
| `to_list xs` | Convert to a list by folding. |
| `join sep xs` | Render each element with `to_string` and join with `sep`. |
| `describe xs` | `"<name> of <length>"`, e.g. `"array of 3"`. |

---

### `std.json`

```dream
import std.json;
```

JSON, parsed and rendered. **Nothing here raises.** A parser that raised could
not be called from a pure function, and reading a configuration file is not an
effect — so `parse` answers with a value describing the outcome instead.

| JSON | Dream |
|------|-------|
| object | `map`, with string keys |
| array | `list` |
| string | `string` |
| number | `integer` when it has no `.`, `e` or `E`; otherwise `float` |
| `true` / `false` | `bool` |
| `null` | `()` |

`null` and "absent" are therefore the same value, which is how the rest of the
library already answers a missing thing — and it means a round trip cannot tell
a member that was absent from one that was explicitly `null`. Neither can JSON.

#### Reading

| Name | Description |
|------|-------------|
| `parse text` | `[:ok, value]`, or `[:error, message]`. Trailing characters after the value are an error. |
| `ok result` | `true` if a `parse` result succeeded. |
| `value result` | The value out of a `parse` result. Meaningless unless `ok` is `true`. |
| `parse_or default text` | The parsed value, or `default` if `text` is not valid JSON. The form to use when a fallback is more useful than a diagnosis. |

```dream
let r = json.parse "{\"a\": 1, \"b\": [true, null, 2.5]}";
json.ok r                       // true
json.value r                    // %{"a" => 1, "b" => [true, (), 2.5]}

json.parse "{oops}"             // [:error, "not valid JSON"]
json.parse_or %{} "not json"    // %{}
```

#### Writing

| Name | Description |
|------|-------------|
| `write v` | `v` as compact JSON on one line, with no spaces. |
| `write_indented depth v` | `v` as JSON indented two spaces per level, starting at `depth`. |
| `pretty v` | `write_indented 0 v` — the usual entry point for readable output. |
| `quote s` | One string as a quoted, escaped JSON string, including the surrounding `"`. |
| `escape_char c` | One character as its JSON representation. Escapes `"`, `\`, and the characters with their own shorthand (`\n`, `\t`, `\r`, `\b`, `\f`); anything else below `0x20` becomes `\u00XX`. **Non-ASCII is left as itself**, so output stays UTF-8 rather than becoming escapes. Used by `quote`. |

```dream
json.write %{ "a" => 1, "b" => [1, true, ()] }   // {"a":1,"b":[1,true,null]}
json.pretty %{ "a" => [1, 2] }                   // multi-line, two-space indent
json.quote "he said \"hi\""                      // 16 characters, including the outer quotes
```

> **Map order is not insertion order.** A map is a hash table, so `write` emits
> members in whatever order the map holds them, and that order is not the one
> they were written in. JSON objects are unordered, so this is valid output —
> but it does mean two maps that compare equal can render as different text,
> and that output is not stable enough to compare byte-for-byte in a test. Sort
> `core.map_pairs` yourself if you need a canonical rendering.

#### Parser internals

The parser is recursive descent over a list of characters. Every step takes the
characters still to read and returns `[value, rest]` — or `()` when it does not
match — which is what lets the steps compose with no parser state threaded
through. These are not part of the interface, but they are reachable, and they
are the shape to copy when writing a parser of your own.

| Name | Description |
|------|-------------|
| `step value rest` | Builds the `[value, rest]` pair every step returns. |
| `step_value s` / `step_rest s` | The two halves back out of one. |
| `is_ws c` / `skip_ws cs` | Whitespace, and dropping a run of it. |
| `is_digit c` | `'0'`–`'9'`. |
| `starts cs word` | Does the character list `cs` begin with the characters of `word`? |
| `drop_n n cs` | Drops `n` characters. |
| `hex_value c` / `hex4 cs n acc` | One hex digit as a number (`-1` if it is not one), and the four digits of a `\uXXXX` escape accumulated into `acc` over `n` digits. |
| `string_body cs acc` | A string's characters up to the closing quote, handling escapes. |
| `number_chars cs acc floaty` | The characters of a number, and whether it had a `.` or an exponent. |
| `parse_number cs` · `parse_value cs` | The steps for a number and for any value. |
| `parse_object cs acc first` · `parse_array cs acc first` | The steps for `{…}` and `[…]`, accumulating into `acc`; `first` tracks whether a separating comma is required yet. |

---

### `std.toml`

```dream
import std.toml;
```

TOML — **deliberately a subset**, and the subset is the one a `mind.toml`
manifest uses: comments, `[section]` and `[dotted.section]` headers,
`key = value`, and values that are strings, integers, floats, booleans, arrays
or inline tables.

What is missing is what a manifest has no use for: array-of-table headers
`[[x]]`, multi-line strings, dates, and dotted keys inside a section. Using one
is an **error rather than a quietly wrong parse** — though the message says
only where the parse gave up, not which unsupported construct caused it:
`[[x]]` reports `"expected a key"`. Like `std.json`, nothing here raises.

The result is a map of section name to a map of that section's keys. Keys
written before any header are collected under the empty string `""`:

```toml
name = "top"           %{ ""             => %{ "name" => "top" },
[package]                 "package"      => %{ "name" => "demo", "ver" => 2 },
name = "demo"             "dependencies" => %{ "a" => %{ "path" => "../a" } } }
ver = 2
[dependencies]
a = { path = "../a" }
```

An inline table becomes a nested map, so `a = { path = "../a" }` reads back as a
map — which is what lets a dependency carry `path`, `git`, `tag` and the rest.

#### Reading

| Name | Description |
|------|-------------|
| `parse text` | `[:ok, table]`, or `[:error, message]`. |
| `ok result` / `value result` | As `std.json` — did it parse, and the table out of it. |
| `parse_or default text` | The parsed table, or `default`. |

#### Reading what was parsed

| Name | Description |
|------|-------------|
| `section table name` | The map for `[name]`, or an **empty map** if there is no such section. A missing section reads exactly like an empty one, so a manifest with no `[dependencies]` needs no special case. |
| `get default table sec key` | `table[sec][key]`, or `default` if either the section or the key is absent. |
| `sections table` | The name of every section. Includes `""` when the file had keys before its first header. |
| `entries table name` | One section's `key = value` pairs as `[key, value]` lists — `core.map_pairs` of that section. |

```dream
let t = toml.value (toml.parse text);
toml.get "0.0.0" t "package" "version"
toml.sections t                        // ["", "package", "dependencies"]
toml.entries t "package"               // [["name", "demo"], ["ver", 2]]
```

> **Order is the map's, not the file's.** `sections` and `entries` return
> whatever order the underlying map holds, which is not the order the file
> wrote them in. Read a manifest by name; do not depend on the sequence.

#### Parser internals

The same `[value, rest]` shape as `std.json`, over a list of characters.

| Name | Description |
|------|-------------|
| `is_ws c` · `is_digit c` · `is_bare c` | Character classes; a bare key is `[A-Za-z0-9_-]`. |
| `skip_ws cs` | Spaces and tabs — **not** newlines, which are significant here. |
| `skip_blank cs` | Whitespace, comments and line breaks: everything between one item and the next. |
| `drop_line cs` / `end_of_line cs` | Discarding a comment, and checking nothing but a comment follows a value. |
| `step` · `step_value` · `step_rest` · `starts` · `drop_n` | As in `std.json`. |
| `quoted_body cs acc` / `literal_body cs acc` | The body of a `"…"` and of a `'…'` string. |
| `number_chars` / `parse_number` | Numbers, with `_` accepted as a digit separator (`1_000` is `1000`). |
| `parse_key cs` | A key: bare, or quoted when it holds characters a bare key may not. |
| `parse_header cs` | A `[section]` or `[a.b]` header, as the name between the brackets. |
| `parse_value cs` · `parse_array cs acc` · `parse_inline cs acc first` | A value, an `[…]` array, and a `{…}` inline table. |
| `parse_items cs table section current` | The top-level loop: `table` is the sections finished so far, `section` the name of the one being read, and `current` its keys. |

---

### `std.test`

```dream
import std.test;
```

The test framework. **Each case runs in its own process.** That is not
ceremony: a case that raises, or that loops forever, is isolated from the rest
of the suite, and its failure arrives at the runner as an ordinary value
through `join!` rather than as something that has already unwound the runner's
own stack.

`std.test` imports only `std.console` and `std.core` — never `std.list`.
A module's own tests import this framework, so anything the framework depended
on could not have tests of its own; the import would be a cycle. Walking lists
with the primitives directly is the price of letting every module test itself.

#### Cases

A case is a name and a **suspended** computation. The `$( .. )` is required —
without it the body would run where it is written, not where the runner puts it.

| Name | Description |
|------|-------------|
| `case name body` | A case: `[name, body]`, where `body` is a thunk. |
| `case_name c` / `case_body c` | The two halves back out. |

#### Assertions

Each raises a message describing the difference. Raising is what ends a case at
its first failure, and what the runner catches.

| Name | Description |
|------|-------------|
| `eq! expected actual` | Equal by `==`. The most-used one. |
| `ne! unexpected actual` | Not equal. |
| `true! actual` / `false! actual` | Exactly `true` / exactly `false` — not merely truthy. |
| `near! tolerance expected actual` | Within `tolerance`, for floats that will not compare exactly. |
| `contains! x xs` | The list `xs` has an element equal to `x`. |
| `empty! xs` | The list is `[]`. |
| `raises! body` | The **thunk** `body` must raise. `test.raises! $( 1 / 0 )` — the `$( )` is what defers it, and the body runs in its own process, so an error that would kill the case cannot. |
| `fail! message` | Fail unconditionally, for a branch that should be unreachable. |
| `has_element x xs` | The predicate behind `contains!`. Returns a `bool` rather than raising, so it is usable in a condition. |

Note the argument order: **expected first, actual second**, so
`test.eq! 6 (list.sum [1, 2, 3])` reads as the claim being made.

#### Running

| Name | Description |
|------|-------------|
| `run_cases! passed failed cases` | Runs each case, printing a line per result. Returns `[passed, failed]`. |
| `suite! name cases` | Prints a heading, runs the cases, prints the tally. Returns the **number of failures**, so several suites can be summed. |
| `run_suites! failed suites` | Runs `[name, cases]` entries, returning the total failures, seeded with `failed`. |
| `main_of! suites` | The usual entry point: run everything, print a summary, and `raise! :tests_failed` if anything failed — which is what makes the process exit non-zero. |

```dream
let main! = test.main_of! [
    ["arith", [
        test.case "adds"    $( test.eq! 4 (2 + 2) ),
        test.case "divides" $( test.raises! $( 1 / 0 ) ),
    ]],
];
```

```
arith
  ok    adds
  ok    divides
  2 passed

all tests passed
```

A failing case prints the error beside its name and the run ends non-zero:

```
  FAIL  a failure on purpose
          <error :error expected 1, got 2>
  7 passed, 2 FAILED
```

#### The generated runner

Writing `main!` by hand as above is the explicit form. The usual way is to let
the compiler build it: a module opts in by defining a **parameterless `tests`
binding**, conventionally inside a `when test { .. }` so it costs nothing in an
ordinary build.

```dream
when test {
    import std.test;

    let tests = [
        test.case "sum" $( test.eq! 6 (list.sum [1, 2, 3]) ),
    ];
}
```

`dreams FILE --test` then scans **the modules it actually loaded** for that
binding and generates an entry point that runs every suite it found. There is
no registry to keep in step and no test that is silently never run; a program
with no `tests` anywhere still compiles, and reports that there was nothing to
run.

---

### `std.map`

```dream
import std.map;
```

Maps, and sets written as maps. `std.core` provides the operations that have to
be primitive; this is the grain most code actually wants — "the value there, or
this one, updated" is one call rather than three and a conditional.

A map is a value: `put` answers a new map and leaves the old one alone, and the
trie shares every branch the change did not touch. **Order is not part of a
map** — `pairs`, `keys` and `values` answer in whatever order the trie walks,
which depends on the hashes and not on the program. `sorted_keys` is the only
thing here that imposes one, and it costs a sort every time.

| Name | Description |
|------|-------------|
| `empty` | The empty map. |
| `from_pairs ps` | A map from `[key, value]` pairs. A later pair wins. |
| `merge a b` | Both, with `b` winning where they overlap. |
| `get default m k` · `has m k` | Look up, with a default; membership. |
| `pairs m` · `keys m` · `values m` · `size m` · `is_empty m` | What is in it. |
| `sorted_keys m` | The keys in order, for output that has to be stable. |
| `put m k v` · `remove m k` | A new map with that key set or gone. |
| `update default f m k` | Apply `f` to what is there, or to `default`. Counting is `update 0 (fn n -> n + 1)`. |
| `put_new m k v` | Add only if absent, so a fold keeps the first. |
| `map_values f m` · `filter keep m` · `fold f init m` | Over the pairs. `fold` sees `acc k v`. |
| `without m other` | Every key of `m` that `other` does not have. |
| `set_of xs` · `member s x` · `add s x` · `members s` | A set is a map whose values say nothing. |

### `std.result`

```dream
import std.result;
```

`[:ok, value]` and `[:error, reason]`, and what to do with them. This shape is
already the convention across the library — `json.parse`, `cli.parse`,
`toml.parse` — for one reason: **raising is an effect**, so a pure parser cannot
report a failure by raising and has to answer one instead.

| Name | Description |
|------|-------------|
| `ok v` · `error r` | Make one. |
| `is_ok r` · `is_error r` | Which it is. |
| `or_else default r` | The value, or `default`. The usual way out. |
| `reason default r` | The reason, or `default`. |
| `unwrap! r` | The value, raising the reason. Impure, because raising is. |
| `map f r` · `map_error f r` | Change one side, leave the other. |
| `and_then f r` | The next step, which may itself fail. This is what makes a pipeline read as one. |
| `or_try other r` | The first if it is ok, otherwise the second. |
| `all rs` | `[:ok, values]` when every one is ok, or the first error — and it stops there, so a lazy list of a thousand checks costs one when the first is wrong. |
| `oks rs` · `errors rs` | The values, or the reasons, of the ones that were. |
| `of_option reason v` · `to_option r` | Across from the other convention, where `()` means "nothing there". |

### `std.error`

```dream
import std.error;
```

The **kind** and the **payload** of a failure. `try! .. catch e` binds the error
itself, and until `std.core` grew `error_kind` and `error_payload` there was no
way into one: a caught error could be printed and nothing else. With them a
failure is an ordinary value to `match` on, and a program can raise failures as
distinguishable as the runtime's own.

```dream
match error.kind e {
    :divide_by_zero => ..,
    :not_found      => ..,
    _               => raise! e,        // not ours; pass it on
}
```

| Name | Description |
|------|-------------|
| `kind e` · `payload e` | The atom and the value, or `()` for anything that is not an error — so a `match` needs no type test first. |
| `is_error e` · `is_kind k e` · `is_any kinds e` | The questions a `catch` is usually asking. |
| `message e` | The payload as text; a string payload is used as it is, so a message does not gain quotes. |
| `describe e` | `kind: message`, the line to print when there is nothing better to say. |
| `new k p` | An error as a value, without raising it — how a pure function *returns* a typed failure. |
| `raise_as! k p` | Raise one of a named kind. The typed form of `raise!`, which otherwise wraps everything in `:error`. |
| `rethrow! e` | Re-raise unchanged, which is what keeps the original kind readable. |

### `std.proc`

```dream
import std.proc;
```

The shapes the process builtins leave to the caller. Three properties of the
runtime decide what is here: **a thunk runs once** (so anything restartable
takes a function of unit, not a thunk), **spawning is lazy** (so anything
starting more than one process forces the list, and that `strict!` is the
difference between parallel and sequential), and **there is no selective
receive and no way to kill** (so a server is written around a handler that sees
every message and decides).

| Name | Description |
|------|-------------|
| `start_all! thunks` | Start every one *now* and answer the processes. |
| `start_each! start! xs` | Start `start! x` for each `x`, in parallel. |
| `wait_all! ps` | Join every one, in list order. |
| `outcome! p` · `outcomes! ps` | `[:ok, v]` or `[:error, e]` — what `join!` would raise, as a value. |
| `parallel! thunks` | Run these at once and answer their values in order. |
| `map! f xs` · `try_map! f xs` | `list.map` with one process per element; the second keeps failures as values. |
| `call! target body` · `reply! m value` | Request and reply. The request carries the process to answer, because `recv!` hands over a message and nothing else. |
| `is_call m` · `call_from m` · `call_body m` | Reading a request. |
| `serve! state handle!` | A message loop. `handle!` answers `[:go, state]` or `[:stop, value]`. |
| `shutdown` · `is_shutdown m` | The conventional stop message, so a worker and its supervisor need not agree on a spelling. |

A process parked on `recv!` is a process the runtime is still waiting for: a
program that ends with one of those reports "every process is waiting for a
message that cannot arrive" rather than its answer. **Whoever starts a server
owns stopping it.**

### `std.supervisor`

```dream
import std.supervisor;
```

Children started together and restarted when they die. Failure here is local —
a process that raises takes itself down and nothing else — so a program made of
processes needs someone whose job is noticing.

```dream
let worker! () = proc.serve! 0 handle!;

let main! = {
    let sup = supervisor.start! :one_for_one [supervisor.child "worker" worker!];
    ..
    supervisor.stop! sup
};
```

| Name | Description |
|------|-------------|
| `child name start` · `child_with name start restart` | A child: a name, a **function of unit** to start it, and when to restart it. A thunk would not do — it runs once. |
| `start! strategy children` · `start_with! strategy children limits` | Start a supervisor and its children; answers its process. |
| `limits n window_ms` · `default_limits` | More than `n` restarts inside the window and the supervisor gives up. Default: five in five seconds. |
| `which! sup` | The names running, in order. |
| `restart! sup name` | Start a child that is not running. |
| `notify! sup` | Hear about every child that goes down from now on, as `[:down, name, outcome]`. |
| `stop! sup` | Stop every child, then the supervisor. |

Restart policies: `:permanent` (always), `:transient` (only after a failure),
`:temporary` (never). Strategies: `:one_for_one` (just that child),
`:one_for_all` (stop the rest and start them all again), `:rest_for_one` (it and
everything started after it).

Two things the runtime's shape forces, and they are worth knowing before
relying on any of this:

- **There is no monitor.** The only way to learn that a process finished is
  `join!`, which blocks, so each child gets a watcher process that joins it and
  reports. The supervisor itself does nothing but read its mailbox.
- **There is no kill.** A supervisor *asks* a child to stop, by sending
  `proc.shutdown`, and waits. A child that never reads its mailbox cannot be
  stopped by anyone, and a group strategy that has to stop its siblings will
  wait for it. Anything meant to be supervised should be written around
  `proc.serve!`.

---

### `std.all`

```dream
import std.all;
```

Every module in the standard library, imported in one place. Two things use it.

`dreams mind/std/all.dr --test` builds a runner from the `tests` each of those
modules exports, so **adding a module here is all it takes for its tests to
run**. Importing them is also a check in itself: a module that no longer
compiles fails the build rather than being quietly skipped.

| Name | Description |
|------|-------------|
| `version` | The library's own version as a string, so a program can report what it was built against. Currently `"0.1.0"`. |
| `modules` | The module names this build provides, as a list of strings. |

```dream
all.version     // "0.1.0"
all.modules     // ["std.array", "std.cli", "std.core", "std.error", "std.json",
                //  "std.list", "std.map", "std.proc", "std.result", "std.seq",
                //  "std.str", "std.streams", "std.supervisor", "std.test",
                //  "std.toml"]
```

`modules` is a hand-maintained list, so it names what the library intends to
ship rather than what happens to be on disk — a module that exists but is not
listed here is not part of the library's interface.

---

## Runtime error atoms

These atoms are raised by the built-in operations. A `match` on the error kind of a caught exception can distinguish them.

| Atom | Raised by |
|------|-----------|
| `:divide_by_zero` | Integer `/` or `%` with a zero divisor |
| `:type_error` | Wrong type passed to an operation |
| `:not_a_function` | Applying a non-callable value |
| `:no_such_member` | `module.name` where `name` is not exported |
| `:match_error` | A `match` with no arm that matched |
| `:out_of_bounds` | Array index outside the valid range |
| `:loop` | A thunk that depends on itself |
| `:stack_overflow` | Recursion exceeded the process's stack limit |
| `:out_of_memory` | Process heap exceeded its allocation limit |
| `:killed` | Process was killed externally |
| `:timeout` | Process timed out |
| `:not_found` | File or path does not exist |
| `:io_closed` | Operation on a closed or stale handle |
| `:io_error` | Generic I/O failure |
| `:permission_denied` | Filesystem permission denied |
| `:already_exists` | Tried to create something that already exists |
| `:wrong_kind` | Operation on the wrong kind of handle |
| `:connection_refused` | TCP connect failed |
| `:connection_lost` | Connection closed unexpectedly |
| `:address_in_use` | Port is already bound |
| `:os_error` | Unclassified OS error |
