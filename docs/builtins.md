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
| `type_of` | `value → atom` | Returns an atom naming the type of its argument: `:integer`, `:float`, `:char`, `:bool`, `:unit`, `:string`, `:atom`, `:list`, `:array`, `:map`, `:module`, `:error`, `:process`, `:pure_fn`, or `:impure_fn`. |
| `to_string` | `value → string` | Renders any value as a human-readable string. Lists print as `[a, b, c]`, arrays as `#[a, b, c]`, maps as `%{:k => v}`, strings are quoted, chars as `'c'`. |
| `len` | `list\|array\|map\|string → integer` | Returns the number of elements (list), slots (array), entries (map), or bytes (string). Raises `:type_error` for anything else. For lists, walks the entire spine. |
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
| `str_len` | `string → integer` | The byte length of the string. |
| `str_chars` | `string → list of char` | Decodes the string to a list of Unicode codepoints (characters). |
| `str_of_chars` | `list of char → string` | Encodes a list of characters into a UTF-8 string. |
| `str_slice` | `string → start:integer → len:integer → string` | Returns `len` bytes starting at byte offset `start`. Clamped silently — running past the end is how string-walking loops finish. |
| `str_find` | `haystack:string → needle:string → from:integer → integer` | Returns the byte offset of the first occurrence of `needle` at or after `from`, or `-1` if not found. |
| `str_byte` | `string → index:integer → integer` | The raw byte value (0–255) at byte `index`, or `-1` if out of range. |

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

Maps are persistent hash maps. Keys are compared by value for flat types (integers, floats, strings, atoms, chars, bools, pids) and by identity for everything else. All updates return a new map.

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
| `compare` | `a → b → integer` | Total order comparison. Returns `-1`, `0`, or `1`. Works on flat types in this rank order: integers and floats (numerically), chars, bools, atoms, strings, unit. Values of different types order by their type rank. |

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
| `to_list xs` | Convert to a list by folding. |
| `join sep xs` | Render each element with `to_string` and join with `sep`. |
| `describe xs` | `"<name> of <length>"`, e.g. `"array of 3"`. |

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
