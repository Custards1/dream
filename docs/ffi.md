# Wrapping a C library

Dream reaches C through two modules. `std.ffi` is the VM's half: it binds a
symbol to a signature, owns what C hands back, and checks every pointer that
crosses. `std.foreign` is the half a wrapper is written against. This page is
about the second, and [builtins.md](builtins.md#stdffi) is the reference for
the first.

The design adds no syntax. A C function is described by data, and what the
description answers is an ordinary Dream function, so a wrapper is an ordinary
module and every tool that works on Dream works on it.

## A first binding

```dream
import std.foreign;

let libm = foreign.library "libm.so.6";

let sqrt = foreign.pure_function libm "sqrt" [:double] :double;
let puts! = foreign.function foreign.this_program "puts" [:cstr] :int;

let main! = puts! (to_string (sqrt 2.0));
```

`function` and `pure_function` differ only in what the answer is called, and
that is the whole of how purity works here, as it is everywhere in Dream: a
function bound to a `!` name is an effect, and one that is not may be called
from pure code. Only the program can know whether a C function is pure. Say
`pure_function` of one that is not, and it will be run lazily, perhaps twice,
perhaps never.

Nothing is loaded until a name is first used, and a library is opened once per
VM however many processes use it.

## Signatures are types

The vocabulary (`:int`, `[:out, t]`, `[:own, tag, destructor]` and the rest)
is declared in `std.foreign` as the types `Arg` and `Result`, and `function` is
signed with them. A signature is nearly always written as a literal, so the
compiler checks it:

```
error: argument 3 of `foreign.pure_function` should be `[Arg]`, but this is `[:f46]`
error: argument 4 of `foreign.function` should be `Result`, but this is `[:own, "db"]`
```

The C-named scalars (`:int`, `:long`, `:size`, ...) have the platform's widths,
settled by the VM, so a binding written from a header is right on each platform
it runs on. There is no `:float`: in Dream that is a double, and a signature is
exactly where the two readings would get confused. Write `:f32`.

## Pointers are owned

A raw pointer is the source of almost every way a C binding goes wrong: kept
after it is freed, freed twice, passed where another kind was wanted. So a
pointer C hands back is not given to Dream at all:

```dream
type Db   = foreign.Handle "sqlite3";
type Stmt = foreign.Handle "sqlite3_stmt";

let open! = foreign.function lib "sqlite3_open"
                [:cstr, foreign.out (foreign.own "sqlite3" "sqlite3_close")] :int;
let prepare! = foreign.function lib "sqlite3_prepare_v2"
                [foreign.handle "sqlite3", :cstr, :int,
                 foreign.out (foreign.own "sqlite3_stmt" "sqlite3_finalize"), :ptr] :int;
```

`[:own, tag, destructor]` makes the result a handle, `[:foreign, "sqlite3", 17]`,
and keeps the pointer in a table the calling process owns. A handle is only a
name for an entry, so:

- a handle that has been released is an `:ffi_error`, not a use-after-free;
- `[:handle, "sqlite3"]` refuses a statement handle;
- a handle names nothing in any process but the one that owns it;
- releasing twice is an error, not a double free.

The two tags are two types, so `Db` and `Stmt` are also told apart by the
checker wherever it can see both.

**When the destructor runs.** When the program calls `foreign.release!`; or,
for whatever it did not release, when the owning process ends, whether it
returned, raised or called `os.exit!`. Resources are destroyed newest first.

**What was made from what.** A resource made by a call that took handles
records them as its parents, and releasing a parent releases its children
first. Above, `prepare!` took the database, so releasing the database finalizes
its statements before it closes, rather than leaving them pointing into freed
memory.

`foreign.using! h f` is the scoped form: `f h`, then `h` released, whether `f`
answered or raised. The answer is forced before the release, because a lazy
answer that read through `h` would otherwise be read later, from a handle that
is gone.

Out-parameters (`[:out, t]`) are not passed. C is given somewhere to write,
and the call answers `[result, out1, out2, ..]`, so the C idiom of a status
code plus a pointer through `**` reads as `let [status, db] = open! "notes.db";`.

## A library as a process

Most C libraries were not written for a program with many threads in it, and
most own things that must be let go. Both are what a process is for here:

```dream
let db_server = foreign.start! ();
let [status, db] = foreign.run! db_server open! ["notes.db"];
foreign.run! db_server exec! [db, "create table t (x)"]
foreign.stop! db_server        // every destructor runs
```

The server owns every resource made through it. Calls reach the library one at
a time, so a library with global state needs no lock. A call that raises comes
back to the caller as the caller's error, and the server carries on. When the
server stops (asked, crashed, or restarted by a supervisor, via
`foreign.child!`), everything it owned is released. Its clients hold handles
that name its resources and mean nothing anywhere else, so a handle from a
server that has been restarted raises rather than reaching freed memory.

`run!` takes either a server or `foreign.here`, so a wrapper written over a
place serves both uses:

```dream
let open_db! place path = foreign.run! place open! [path];

open_db! foreign.here "a.db"      // in this process
open_db! db_server "b.db"         // in the library's
```

Buffers in a server are reached with `buffer_in!`, `peek_in!`, `poke_in!`,
`read_in!`, `write_in!`, `release_in!`, `alive_in!` and `owned_in!`, which take
the same place.

A foreign function is a value like any other and can be sent in a message, so
a program can also write its own server around `ffi.call! f args`.

## Memory and structs

`foreign.buffer! n` is `n` zeroed bytes this process owns, of a known size, so
every `peek!`, `poke!`, `read!` and `write!` is checked against it. A pointer C
hands back has no size, and Dream does not read one directly, because that
would mean trusting C about where it ends. `:buffer` in a signature passes
one.

A struct is laid out as C lays it out: each field at the next offset its
alignment allows, and the whole padded to its widest alignment. `layout` is
pure, so `comp` settles it while compiling:

```dream
let point = comp foreign.layout [[:x, :i32], [:y, :double], [:tag, :u8]];

let b = foreign.struct_buffer! point;
fill! b 3 1.5
foreign.read_struct! b 0 point               // %{ :x => 3, :y => 1.5, :tag => 7 }
foreign.write_struct! b 0 point %{ :x => 10 }
```

`array_of! t xs` and `read_array! b at t n` do the same for a C array.

## Callbacks

`[:callback, [args], result]` passes a Dream function where C wants a function
pointer, so `qsort` and `sqlite3_exec`-style iteration work:

```dream
let fold_range = foreign.pure_function lib "fold_range"
                     [:long, [:callback, [:long, :long], :long], :long] :long;

fold_range 10 (fn acc i -> acc + i * i) 0      // 385
```

A callback runs Dream while C is on the stack, which is why it has rules of
its own. Each rule is checked rather than trusted:

- **It must be pure.** An impure one is refused before C is called. Nothing a
  callback does can be ordered against the C code around it, and a callback
  that waited on a message would have to be resumed with C's frames still on
  the stack, which cannot be done.
- **It lives for one call.** A library that keeps a callback and calls it
  later (a signal handler, a completion callback) cannot be given one. That
  later call would jump into freed memory, and this is the one rule nothing
  can detect.
- **It runs on the calling thread.** A call from another thread is refused and
  answered with zero.
- **An error does not unwind through C.** The callback answers zero from then
  on, and the error is raised once the C function has returned.

## Carrying the library in the image

```
dreams --payload sqlite=libsqlite3.so -o notes.dream main.dr
```

puts the file's bytes at the end of the image, and `foreign.embedded "sqlite"`
names them. On Linux the library is loaded straight from memory
(`memfd_create`), so nothing is written to disk. Elsewhere it is written to
the temporary directory and loaded from there, and removed at once where the
platform allows that, which is everywhere but Windows. A program that needs a
particular build of a library can carry exactly that build. The name is stored
in the image's `LNAM` section ([bytecode-format.md](bytecode-format.md)).

## What this cannot make safe

A signature that does not match the C function corrupts the call, exactly as
it would in C, and nothing can see it. `:ptr` is a raw address and checks
nothing; it is the escape hatch, and so are `foreign.address!` and the
original `alloc!`/`read_u8!` members of `std.ffi`. Everything else is checked
at the boundary: argument kinds, handle liveness, ownership and tag, and buffer
bounds.

[dream/tests/ffi](../dream/tests/ffi) is a C library written to be wrapped,
together with a program that exercises all of the above, under both the
interpreter and the JIT (`just test-ffi`).
