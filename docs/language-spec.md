# The Dream Language

A reference for the Dream language as it is actually implemented, plus one
clearly-marked section for a design that is not implemented yet.

Dream is **dynamically typed**, **lazily evaluated**, and **functional**, with
green processes for concurrency and a purity rule enforced by the spelling of a
name. Two programs implement it:

| | |
|-|-|
| [`dreams/`](../dreams) | the compiler, written in Dream — `.dr` source to a `.dream` image |
| [`dream/`](../dream) | the VM, `dream`, in C++ — interpreter, processes, LLVM JIT |
| [`mind/`](../mind) | the standard library and build system, written in Dream |
| [`dreamc/`](../dreamc) | the old compiler, in Rust, kept only as a second opinion |

The compiler is written in the language it compiles and builds from an image of
itself; `dreamc` compiled the first one and now only answers the differential
tests. Where this document points at a Rust file for a canonical list, that is
the older of two implementations that a test holds to each other.

> **Status legend.** Everything in this document is implemented and covered by
> tests unless it carries a **PROPOSED** marker. Only [destructuring `let` and
> parameters](#destructuring-let-and-parameters-proposed) is so marked.

---

## Contents

1. [Dream in sixty seconds](#1-dream-in-sixty-seconds)
2. [Lexical structure](#2-lexical-structure)
3. [Values and types](#3-values-and-types)
4. [Expressions](#4-expressions)
5. [Laziness and strictness](#5-laziness-and-strictness)
6. [Purity](#6-purity)
7. [Declarations](#7-declarations)
8. [Modules and packages](#8-modules-and-packages)
9. [Processes](#9-processes)
10. [Errors](#10-errors)
11. [Compile-time evaluation](#11-compile-time-evaluation)
12. [Pattern matching](#12-pattern-matching)
13. [The standard library](#13-the-standard-library)
14. [The toolchain](#14-the-toolchain)
15. [Embedding](#15-embedding)

---

## 1. Dream in sixty seconds

```dream
import std.console;

let rec fac n = if n <= 1 { 1 } else { n * fac (n - 1) };

let greet! who result = {
    who    |> console.print! "Hi, "
    result |> console.print! "The result is "
};

let main! = {
    let number = fac 5;
    spawn! $( greet! "process 2" number )
    let safe = try! { 12 / 0 } catch e { 0 };
    greet! "process 1" safe
};
```

Six rules carry most of the language:

1. **Application is juxtaposition and binds tighter than every operator.**
   `fac n - 1` is `(fac n) - 1`. The recursive call needs `fac (n - 1)`.
2. **Functions are curried.** `f a b` is `(f a) b`.
3. **`x |> f a` feeds `x` in as the _last_ argument**, giving `f a x`.
4. **A trailing `!` on a name means impure.** A pure function may not reach an
   impure one; the compiler rejects it.
5. **Everything is lazy** unless the compiler marked it strict — the statements
   of an impure block, an `if` condition, a `try!` body.
6. **`$( e )` suspends `e`** as a thunk, which is what `spawn!` runs.

---

## 2. Lexical structure

### Comments

```dream
// to end of line
/* block, which does not nest */
```

### Identifiers and the `!` suffix

```
ident  ::=  [A-Za-z_] [A-Za-z0-9_]* '!'?
```

The `!` is **part of the name**, not an operator: `print!` and `print` are
different identifiers, and the `!` is what marks the binding impure ([§6](#6-purity)).

Keywords, which may not be used as ordinary names:

```
let  rec  if  else  import  as  catch  true  false
not  try!  fn  virtual  derive  comp  comp!  when  mod
```

Of these, `let  rec  else  import  as  catch  virtual  derive  when  mod` can
never begin an expression, so encountering one ends an application's argument
list.

### Literals

| Form | Example |
|------|---------|
| integer | `42`, `1_000_000`, `0xFF`, `0b1010`, `0o755` |
| float | `1.5`, `1_0.25`, `2.5e-3`, `1e9` |
| string | `"hello\n"`, `"\u{1F600}"` |
| char | `'c'`, `'\n'`, `'\u{41}'` |
| atom | `:ok`, `:not_found` |
| bool | `true`, `false` |
| unit | `()` |
| list | `[1, 2, 3]` |
| array | `#[1, 2, 3]` |
| map | `%{ :k => 1, :j => 2 }` |
| thunk | `$( expr )` |

Escapes inside strings and chars: `\n \t \r \0 \\ \' \"` and `\u{HEX}`.
Underscores are permitted as digit separators in every numeric base.

### Newlines are significant

Inside a block and at top level, **a line break ends a statement**. Three rules
relax that:

- **A line indented past the statement it follows continues it.** This is what
  lets a call be spread over several lines, and it is the same cue a reader
  already goes by:

  ```dream
  let total = add3 (1 + 1)
                   (2 + 2)
                   (3 + 3);
  ```

  A line at the same indentation, or less, starts a new statement.
- A line that *begins* with an infix operator, `.`, `else`, or `catch`
  continues the previous line whatever its indentation, since none of those can
  start a statement.
- Newlines never end a statement inside `(`, `[`, `$(`, `#[`, or `%{`. Braces
  `{ }` are *not* in that list — a block is newline-sensitive.

```dream
let total = a
    + b            // continues: the line starts with an operator
    + c;

let names! = vm
    .modules! ();  // continues: the line starts with `.`

let main! = {
    print! "a"     // two statements: same indentation
    print! "b"
};
```

`;` ends a statement explicitly and is always allowed.

---

## 3. Values and types

`type_of v` returns the type's name as an atom. The canonical list lives in
[`dreamc/src/types.rs`](../dreamc/src/types.rs), the older of the two
implementations; the compiler, the VM's
`type_of`, and this table are checked against it by tests rather than kept in
step by hand.

### Scalars

| Type | |
|------|-|
| `integer` | a signed integer |
| `float` | double precision |
| `char` | one Unicode scalar value |
| `bool` | `true` or `false` |
| `unit` | `()`, the value of an expression with nothing to say |

### Object kinds

`object` is the umbrella type; these are its kinds.

| Kind | |
|------|-|
| `pure_fn` | a function with no effects |
| `impure_fn` | a function whose name ends in `!` |
| `module` | what `import` binds, and what `mod` declares |
| `list` | `[a, b, c]` — a cons chain, lazy in head *and* tail |
| `array` | `#[a, b, c]` — flat, constant-time indexing |
| `map` | `%{ k => v }` — keys forced, values lazy |
| `error` | a raised kind and payload, caught by `try!` |
| `process` | a green process (`thread` is an accepted alias) |
| `atom` | an interned name, `:like_this`; compares by identity |
| `string` | UTF-8 text |

### Representation

A value is one 64-bit word, tagged in the low bits:

| Pattern | Meaning |
|---------|---------|
| `....1` | fixnum — a 63-bit signed integer, `(int64)v >> 1` |
| `...000` | pointer to a heap object (8-byte aligned); `0` means "no value" |
| `...010` | immediate — unit, bool, char, atom, nil, builtin |

Integers get the one-bit tag because arithmetic is the hot path, and the
tagging preserves order so the JIT can compare two tagged fixnums directly.

Lists are cons cells and arrays are flat, both as you would expect. **Maps are a
hash array mapped trie** — a tree branching 32 ways on five bits of the key's
hash per level. That shape is chosen for the same reason the rest of the runtime
is: a value here is never updated, only succeeded. A flat table would have to be
copied on every `map_put` to leave the original standing, which makes building a
map an entry at a time quadratic; a trie shares everything the change does not
touch, so the update is `log32(n)` new nodes and the map it came from is
untouched and still cheap to use.

---

## 4. Expressions

### Precedence, loosest to tightest

| Level | Operators | Associativity |
|-------|-----------|---------------|
| lowest | `\|>` | left |
| 0 | `\|\|` | left |
| 1 | `&&` | left |
| 2 | `==` `!=` `<` `<=` `>` `>=` | left |
| 3 | `+` `-` | left |
| 4 | `*` `/` `%` | left |
| 5 | unary `-`, `not` | prefix |
| 6 | `comp`, `comp!` | prefix |
| tightest | **application** `f a b`, then postfix `.field` | left |

Every infix operator is left-associative. The single most important
consequence: **application binds tighter than everything**, so

```dream
fac n - 1      //  (fac n) - 1
fac (n - 1)    //  what a recursive call usually wants
f a b + g c    //  (f a b) + (g c)
```

`comp` sits between the operators and application, so `comp f x` folds the whole
call.

### Application and currying

```dream
let add a b = a + b;
let inc = add 1;        // partial application
inc 41                  // 42
```

`()` in a parameter list is a unit parameter that occupies a slot but binds no
name — `let now! () = ...` is called as `now! ()`.

### Pipe

`|>` feeds the left side in as the **last** argument of the right side:

```dream
x |> f a           // f a x
[1,2,3] |> list.map inc |> list.sum
```

The pipe is folded during lowering, not desugared into a nested call: `x |> f a`
becomes a **single** application of `f` to `[a, x]`. That is what lets
`v |> console.print! "got: "` land as one variadic call `print! "got: " v`, and
it is why `console.print!` puts its label first.

### Operators on non-numbers

`+` is overloaded by the runtime:

- two numbers — arithmetic
- two strings — concatenation
- two lists — concatenation, **without forcing the elements**

Integer arithmetic that overflows a fixnum falls through to `float` rather than
wrapping silently. `/` and `%` by an integer zero raise `:divide_by_zero`.

### `if`

```dream
if cond { then_expr } else { else_expr }
if a { .. } else if b { .. } else { .. }
```

The branches are blocks, so the braces are required. The condition is forced;
the taken branch is not, unless the surrounding context forces it. `else` is
optional, and a missing one yields `()`.

While parsing an `if` condition, `{` starts the branch body rather than a block
being passed as an argument.

### Blocks

```dream
{ stmt; stmt; last_expr }
```

A block's value is its last statement, or `()` when empty. Statements are
separated by newlines or `;`. A block may contain `let` declarations, which
scope to the rest of the block.

### Lambdas

```dream
fn x -> x + 1
fn a b -> a * b
```

`fn` needs at least one parameter. The body extends as far as the expression
grammar allows, so parenthesize when passing a lambda as a non-final argument.

### Thunks, and forcing them

`$( e )` suspends `e` as a first-class thunk object. It is what `spawn!` turns
into a process, and what `std.test` uses to hold a test case for later.

`strict! e` is the other direction: it evaluates `e` **all the way down** and
hands it back. Laziness is the default and usually right, but it has one sharp
edge — an effect in a lazy position does not happen until something forces it,
and "something" may be much later, or never:

```dream
// Every `spawn!` here is a thunk. They run one at a time, as each `join!`
// forces its element -- which is the opposite of what the code looks like.
let ps = list.map (fn n -> spawn! $( work n )) jobs;

// Now they have all started before the first `join!`.
let ps = strict! (list.map (fn n -> spawn! $( work n )) jobs);
```

It forces *deeply* rather than to weak head normal form, because forcing the
list without forcing its elements would leave the effects exactly where they
were. It is impure by name, which is right: forcing is when effects happen.
Forcing a value that is already forced costs nothing.

The block form reads well when several things have to happen first:

```dream
strict! { let a = expensive (); [a, a * 2] }
```

### Collections

```dream
[1, 2, 3]                     // list  — lazy head and tail
#[1, 2, 3]                    // array — constant-time indexing
%{ :a => 1, "b" => 2 }        // map   — keys forced, values lazy
```

**`obj.field` is module-member access and nothing else.** The field name may
carry a trailing `!` (`console.print!`). It is not map or record indexing —
applying it to anything but a module raises `:no_such_member`. Maps are read
with `core.map_get`, arrays with `core.array_get`:

```dream
core.map_get point :r ()        // the value at `:r`, or the default `()`
core.array_get arr 0
```

---

## 5. Laziness and strictness

Every argument, list element, map value and `let` binding starts as a **thunk**:
a node index plus the frame to evaluate it in. Forcing a thunk overwrites it in
place with an indirection to its result, so every holder sees the computed value
and the work happens once.

**The compiler decides where evaluation order is observable and says so in the
bytecode.** The VM forces a node only when it is marked `STRICT`:

- the statements of an **impure block**,
- an **`if` condition**,
- a **`try!` body**.

Everything else stays suspended. A discarded *pure* statement is not evaluated
at all, so it cannot raise an error the program never asked for.

```dream
let ones = list.repeat 1;      // an infinite list
list.take 5 ones               // fine: only five cells are ever built
```

Sharing is preserved by returning the *binding's* thunk rather than a fresh
wrapper, and the allocation is skipped entirely when a node is already a value
(constants, variable references, closures).

### Wrappers

A global whose body is one application of its own parameters -- `let head xs =
core.head xs`, `let kind t = core.array_get t 0` -- is a **wrapper**, and a
saturated call of one is compiled as the call it stands for. The frame that
disappears bound nothing but the arguments the inner call was going to be
given, and every argument stays the same thunk in the same place, so nothing is
evaluated that was not before and nothing twice.

The conditions are narrow on purpose: one application, every parameter passed
on exactly once, and literals for the rest. A call that is not saturated is
left alone, which is what keeps a variadic host function honest -- a variadic
native means "everything at this call site".

### What this means for the JIT

The JIT compiles only the strict numeric spine — arithmetic, comparisons,
branches, self tail recursion. That limit is a soundness requirement: compiled
code evaluates a self tail call's arguments eagerly, and doing that to an
argument the callee would never have forced turns a terminating program into one
that raises. A **strictness analysis** runs first, and a function is compiled
only if every parameter is provably forced on every path:

```
strict(Local i)      = {i}
strict(If c, t, e)   = strict(c) ∪ (strict(t) ∩ strict(e))
strict(a `binop` b)  = strict(a) ∪ strict(b)
strict(a && b)       = strict(a)          -- b is conditional
```

Functions that do not qualify stay interpreted, where laziness is explicit and
free.

---

## 6. Purity

**A name ending in `!` denotes an impure value, and a pure function may not
reach one.**

```
error: cannot use the impure member `print!` inside the pure function `greet`
 --> greet.dr:2:5
  |
2 |     console.print! name
  |     ^^^^^^^^^^^^^^
  = note: mark the function impure by ending its name with `!`, e.g. `let f! x = ..`
```

This is checked during lowering, and it is what makes `pure_fn` and `impure_fn`
genuinely distinct object types rather than a naming convention. It is also what
lets the compiler decide where evaluation order matters: statements in an impure
block are sequenced, everything else stays lazy.

Because every process operation ends in `!` (`spawn!`, `join!`, `send!`,
`recv!`, `self!`), **starting or addressing a process is an effect** and the
purity rule already keeps concurrency out of pure functions. The same holds for
`raise!` and for `try!`.

---

## 7. Declarations

### `let`

```dream
let name = expr;                    // a value
let f a b = expr;                   // a function, curried
let rec loop n = ...;               // may refer to itself
let impure! x = ...;                // impure, by the trailing `!`
```

`let` appears both at top level (as an item) and inside a block (as a
statement), and the two differ in one way:

- **Top-level `let`s are mutually visible.** Every one is a global, so they can
  refer to each other and to themselves in any order, and `rec` is optional.
- **A block-local `let` is in scope only for the statements after it.** A local
  that refers to itself needs `rec`, or the name is not yet bound:

  ```
  error: cannot find `go` in this scope
   --> f.dr:3:39
    |
  3 |     let go n = if n <= 0 { 0 } else { go (n - 1) };
    |                                       ^^
  ```

Writing `rec` at top level is still worth doing where it documents intent; the
standard library does.

### `import`

```dream
import std.console;                 // binds `console`
import std.list as l;               // binds `l`
import std.list.{map, filter};      // binds the members, unqualified
import std;                         // the whole package, as `std.list.map`
```

The alias defaults to the last path segment. All four forms, and what a package
namespace can and cannot be used for, are in
[§8](#8-modules-and-packages).

### `mod`

```dream
mod util {
    let helper x = x + 1;
}
```

A module written inside another, reached as `util.helper`. It holds the same
items a file does and nests freely; see [§8](#8-modules-and-packages).

### `virtual` and `derive`

A **virtual** declares a hole in a module; a module that `derive`s it fills the
hole. A default body makes filling it optional.

```dream
// shape.dr  -- in the package `shapes`
virtual let area s;                       // must be filled
virtual let name s = "shape";             // may be overridden
let describe s = name s + " of area " + to_string (area s);

// circle.dr
derive shapes.shape;
let area r = 3.14159 * r * r;
let name r = "circle";
```

`shapes.circle.describe 2.0` is then `"circle of area 12.56636"`.

A virtual needs at least one parameter — a parameterless virtual would be a
constant, not a hole. Because Dream compiles whole programs, `derive`
specializes the base module's *syntax tree* against the deriving module's
implementations, so there is no run-time dispatch.

### `when` — conditional compilation

```dream
when test {
    import std.vm;
    let verbose = true;
}

when os == "linux" && not release {
    let trace! msg = console.error! "[trace] " msg;
}

when release  let verbose = false;        // single item, no braces
```

The condition vocabulary is deliberately the language's own — `&&`, `||`, `not`,
parentheses, `true`, `false`, a bare flag, or `setting == "value"`. A `when` is
resolved **before anything is loaded**, so an import inside a false branch is
never even followed, and a module that only test builds need is never read.
`when` may contain any item, including another `when`.

A **flag** is either defined or not; a **setting** has a value. A setting that
has a value also counts as defined, so `when os { .. }` is true and
`when nonsense { .. }` is false.

| Setting | Known without being told |
|---------|--------------------------|
| `os` | `"linux"`, `"macos"`, … |
| `arch` | `"x86_64"`, `"aarch64"`, … |
| `family` | `"unix"`, `"windows"` |
| `dream_version` | the compiler's version |

Everything else comes from `-D name`, `-D name=value`, `--test` (defines
`test`), `--release` (defines `release`), and `--debug-cfg` (defines `debug`).
`dreams --print-cfg` prints the lot.

---

## 8. Modules and packages

**A module is one file** — or a `mod` block inside one. `import a.b.c` resolves
to `a/b/c.dr`, or to `a/b/c/mod.dr` if the module has grown into a directory —
importers do not change when it does. The only module extension is `.dr`.

### `mod` — a module written inside another

```dream
mod math {
    let square x = x * x;
    let cube x = x * square x;

    mod deep {
        let answer = 42;
    }
}

let main! = { math.square 5 };        // 25
                                      // math.deep.answer is 42
```

A `mod` body holds exactly what a file holds: `let`, `import`, `derive`,
`virtual`, `when`, and further `mod`s. That is not a coincidence — the loader
registers `mod util { .. }` inside module `m` as the module **`m.util`** and
leaves `m` importing it, so from that point on a submodule and a file are the
same thing to every later pass. Purity, `derive` and member lookup need no
special case.

Two consequences worth knowing:

- **A submodule does not re-export what it imports.** `mod text { import
  std.list; .. }` gives you `text`'s own members, not `text.list`. A module's
  aliases hold its imports as well as its declarations, so only a module
  actually named `text.<name>` counts as a submodule. (A *host* module is the
  exception, because it is a value as well as a namespace.)
- **An import resolves relative to the current module first,** so
  `import util.{ helper };` inside a module declaring `mod util { .. }` means
  that submodule rather than some file called `util.dr`.

**A package is a named group of modules,** marked by a `mind.toml` at its root
(`dusk.toml` is also accepted). A package declares its own name, so `import
std.list` means "the module `list` in the package called `std`", wherever that
package happens to sit on disk.

```toml
[package]
name = "std"
version = "0.1.0"
src = "."

[dependencies]
other = { path = "../other" }
```

`name` is required; `version` defaults to `0.0.0` and `src` to the manifest's
own directory. Only path dependencies are supported so far.

A project is itself a package, so its own modules are reachable both as
`mypkg.util` and, from inside, as plain `util`. Dropping a manifest into a
directory is the whole ceremony.

Dream **compiles whole programs**: every reachable module is parsed and lowered
into a single image, which is what makes a cross-module call resolve to a global
index at compile time rather than a name lookup at run time. A module path with
no file behind it is assumed to be host-provided; those are listed explicitly
(see [§13](#13-the-standard-library)) so a typo in an import is an error rather
than a mystery at run time.

### The three forms of `import`

```dream
import std.list;                      // binds `list`
import std.list as l;                 // binds `l`
import std.list.{map, filter};        // binds `map` and `filter`, unqualified
import std.list.{sum as total};       // ..renaming as it goes
import std;                           // the whole package: `std.list.map`
```

**`import path.{ a, b }`** binds members rather than the module. Each name
resolves exactly as `path.a` would, so the two spellings can never disagree —
including about purity, which is why `import std.console.{print!};` still keeps
`print!` out of a pure function. A name that the module does not export is an
error naming what it does export, and a name that collides with a `let` in the
importing module is an error too rather than the `let` quietly winning.

**`import <package>;`** names a package rather than a module and brings in
every module the package provides, reached through it:

```dream
import std;

let main! = { std.list.map (fn x -> x * 2) [1, 2, 3] };
```

The package name is a **namespace, not a value**: `std` and `std.list` are
compile-time names, and only `std.list.map` is something you can pass around.
Mentioning either on its own is an error that says so.

`-L DIR` adds a package search root; `dreams FILE --packages` and
`dreams FILE --modules` report what a program pulls in.

An **embedder's own host module** is declared with `--host-module PATH`, which
is what lets `import host;` compile against a module registered through
`dream_vm_register_module` ([§15](#15-embedding)). It is deliberately not a
wildcard — naming the module is what keeps a typo in an import a compile error
rather than a mystery at run time.

---

## 9. Processes

A **process** is the unit of concurrency, of failure, and of garbage collection.
Processes share no memory: `send!` deep-copies the message, so nothing one
process does to a value can be seen by another.

| Operation | Meaning |
|-----------|---------|
| `spawn! $( .. )` | run a suspended computation in a new process; returns it |
| `send! p v` | copy `v` into `p`'s mailbox |
| `recv! ()` | take the next message, parking until one arrives |
| `self! ()` | the current process |
| `join! p` | wait for `p` and take its result; a failure arrives as an error |

```dream
let worker! () = {
    let msg = recv! ();
    msg |> console.print! "got: "
};

let main! = {
    let w = spawn! $( worker! () );
    send! w :hello
    join! w
};
```

**Isolation buys three things:** collection never stops the world and never takes
a lock; thunk update needs no atomics, because only one process can force a
thunk; and one process failing cannot corrupt another. The cost is that a thunk
shared between two processes is evaluated twice — for a language with both
concurrency and laziness, isolation is the better trade.

**Scheduling** is per-worker run queues with work stealing. A process runs for a
fixed number of reductions and then goes back on a queue, whatever it is in the
middle of — including inside a JIT-compiled loop, which writes its loop-carried
values back to the frame and exits to the interpreter. When every worker is idle
and processes remain, they are all parked on messages that cannot arrive, and
the runtime says so rather than hanging.

A process that fails and that nobody joins is reported at shutdown. One that a
joiner is waiting for is that joiner's business, and is not reported twice.

---

## 10. Errors

An `error` is a value: a **kind** (an atom) and a **payload**.

```dream
raise! "something went wrong"           // raise any value
try! { 12 / 0 } catch e { 0 }           // catch it and supply a fallback
```

`try!` requires a `catch` with a binder — `try! { .. } catch e { .. }` — and the
body is strict, so the error surfaces where the `try!` is rather than wherever
the value later happens to be forced.

Well-known kinds the runtime raises:

| Kind | Raised by |
|------|-----------|
| `:divide_by_zero` | `/` or `%` with an integer zero on the right |
| `:type_error` | an operation applied to a type it does not accept |
| `:not_a_function` | applying arguments to something that is not callable |
| `:no_such_member` | `mod.name` where the module has no such member |
| `:out_of_bounds` | an array index outside the array |
| `:loop` | a value that depends on itself |
| `:stack_overflow` | recursion too deep — see **Runaway processes** below |
| `:out_of_memory` | a process's heap grew past its limit |
| `:killed`, `:timeout` | process failure |

`:normal` and `:ok` are used as success markers. An error renders as
`<error :kind message>`.

Both `raise!` and `try!` are impure, so error handling is an effect and stays
out of pure functions.

### Runaway processes

A process is the unit of failure, and that has to hold even when the failure is
running out of memory. So the runtime bounds what one process may use, and
**raises in the process that exceeded the bound** rather than letting the
allocator throw and lose the whole system:

| Limit | Default | Raises | Set with |
|-------|---------|--------|----------|
| pending continuations | 4,194,304 | `:stack_overflow` | `DREAM_MAX_DEPTH` |
| value stack entries | 4,194,304 | `:stack_overflow` | `DREAM_MAX_STACK` |
| heap bytes per process | 1 GiB | `:out_of_memory` | `DREAM_MAX_HEAP` |

These are ordinary errors: `try!` catches them, `join!` delivers them, and every
other process carries on.

A tail call pops its continuation, so a loop runs in constant space and never
approaches the first limit. What does is runaway **non-tail** recursion — and
the usual cause is the precedence rule in [§4](#4-expressions):

```dream
let rec f n = f n - 1;      // `(f n) - 1` -- the subtraction is always pending
let rec f n = f (n - 1);    // what was meant
```

The first leaves a continuation on every call and never returns. It now fails
with `:stack_overflow` naming the depth, instead of exhausting memory.

---

## 11. Compile-time evaluation

```dream
let squares = comp build 5;                    // evaluated by the compiler
let digits  = comp! core.str_chars "12345";    // evaluated by running it on the VM
```

`comp e` evaluates `e` at compile time and bakes the result into the image.
`comp! e` is the same but may perform effects, so it is evaluated by running it
on a real VM rather than by the compiler's own evaluator — the compiler embeds
the VM through the C API described in [§15](#15-embedding) and reads the value
back.

`comp` binds tighter than any operator but looser than application, so
`comp f x` folds the whole call and `comp (1 + 2) * 10` is `30`.

**`comp` cannot reach a host module.** `std.console`, `std.core` and the rest
are C++ in the VM, and the compiler's own evaluator has no VM to run them on:

```
error: `core` is not a Dream module, so `core.cons` is not available at compile time
  = note: host modules perform effects; `comp` cannot run them
```

That is what `comp!` is for — it hands the expression to a real VM. So the rule
is: `comp` for arithmetic and pure Dream code, `comp!` for anything that needs
the runtime.

---

## 12. Pattern matching

`match` is implemented and is used throughout the compiler itself. The one part
of the design still outstanding is destructuring in `let` and parameter lists,
which is marked below.

### Syntax

```dream
match expr {
    pattern => expr,
    pattern if guard => expr,
    _ => expr,
}
```

`=>` is already the map separator, and `{ }` already delimits every other
control form, so `match` introduces no new punctuation. Arms are separated by
commas; a trailing comma is allowed. Arms are tried **in source order** and the
first that matches wins.

### Patterns

| Pattern | Matches |
|---------|---------|
| `_` | anything, binding nothing |
| `x` | anything, binding it to `x` |
| `1`, `1.5`, `'c'`, `true`, `"s"`, `:atom`, `()` | that literal, by the same equality `==` uses |
| `[]` | the empty list |
| `[a, b, c]` | a list of exactly three elements |
| `[x, ..rest]` | a non-empty list; `rest` is the tail |
| `#[a, b]` | an array of exactly two elements |
| `#[a, ..rest]` | an array of at least one element |
| `%{ :k => v }` | a map containing key `:k`; other keys ignored |
| `p as name` | `p`, also binding the whole value to `name` |

Patterns nest. A name may be bound at most once per arm.

```dream
let rec sum xs = match xs {
    []          => 0,
    [x, ..rest] => x + sum rest,
};

let classify v = match v {
    0                => :zero,
    n if n < 0       => :negative,
    n                => :positive,
};

let route msg = match msg {
    %{ :kind => :get, :path => p }  => handle_get p,
    %{ :kind => :post } as m        => handle_post m,
    other                           => reject other,
};
```

### Forcing — the part that matters in a lazy language

**A pattern forces exactly as much of the scrutinee as it needs to decide.**

- `_` and a bare binder force nothing.
- Every other pattern forces the scrutinee to weak head normal form.
- A nested pattern forces its sub-position to WHNF, recursively, and only along
  the path it is inspecting: `[x, ..rest]` forces the first cell but neither
  `x` nor `rest`.
- A guard is evaluated strictly, but only after its arm's pattern has matched.

Arms are tried in order, so an earlier arm's forcing is observable by a later
one. This is the same bargain `if` already makes with its condition.

### Interaction with the rest of the language

- **Purity.** `match` is pure. The scrutinee, guards and arm bodies follow the
  ordinary rule: impure only inside an impure context.
- **Strictness analysis.** `strict(Match s, arms) = strict(s) ∪ ⋂ strict(armᵢ)` —
  the same shape as `If`, which is what lets a `match`-written loop stay
  JIT-eligible.
- **Exhaustiveness.** Dream is dynamically typed, so exhaustiveness cannot be
  checked in general. A `match` with no arm that matches raises `:match_error`
  carrying the unmatched value. A `_` arm is therefore the way to be total.

### Destructuring `let` and parameters (PROPOSED)

> **Not implemented.** `let [a, b] = pair;` is a parse error today: `let` takes
> a name. Everything else in this section is implemented.

The same pattern grammar, restricted to **irrefutable** patterns (`_`, binders,
`as`, and fixed-length `[..]` / `#[..]` / `%{..}` forms), extends `let` and
parameter lists:

```dream
let [a, b] = pair;
let f %{ :x => x, :y => y } = x + y;
```

A refutable pattern in either position is a compile error, naming the pattern
that could fail.

### How it is compiled

An arm is a decision chain of test-and-bind nodes, and the tests are ordinary
builtins -- `match_is_cons`, `match_head`, `match_tail`, `match_at`,
`match_key`. They are named that way on purpose: a builtin beats an imported
name, so calling one of them `head` would quietly shadow
`import std.list.{head}` in every module that had both.

The tests force exactly as far as the pattern looks. `[x, ..rest]` forces the
cell to know whether it is one, and does not force `x`; that is what lets a
`match`-written loop walk a list that is still being produced.

Bindings become frame slots exactly as parameters do, so scope resolution and
the purity check needed no changes for `match`, and the image format needed
none either: the arms lower to existing node kinds plus opcodes **appended** to
the table, because an opcode's position is its identity and inserting one would
invalidate every image already built.

---

## 13. The standard library

Two layers. **Native modules** are C++ in the VM and are listed explicitly by
both the compiler ([`dreams/modules.dr`](../dreams/modules.dr)) and the VM's
registry, with a test proving the two agree. **Dream modules** live in
[`mind/std/`](../mind/std) and are compiled like any other package.

### Builtins

Resolved directly, without an import, unless shadowed by a binding:

| | |
|-|-|
| `spawn!` | thunk → process |
| `join!` | process → value |
| `send!` | process → value → unit |
| `recv!` | unit → value |
| `self!` | unit → process |
| `raise!` | value → never |
| `type_of` | value → atom |
| `to_string` | value → string |
| `len` | list \| array \| map \| string → integer |
| `strict!` | value → the same value, evaluated all the way down |

### `std.core` — what the language cannot express in itself

Everything here is either a primitive the representation hides (a string's
bytes, a map's buckets) or something that must be a single machine step for the
rest of the library to be worth writing.

`str_concat` is the second kind. Building a string out of n pieces with `+`
copies everything written so far on every step, so it costs n² bytes of
copying; `str_concat` walks the list once and copies each piece once. Anything
that assembles a large output a piece at a time -- an image, a rendered
diagnostic -- goes through it, and `std.str` builds `concat_all`, `join_str`
and `repeat` on top of it.

| Area | Members |
|------|---------|
| lists | `head` `tail` `cons` `is_empty` |
| strings | `str_len` `str_chars` `str_of_chars` `str_of_bytes` `str_concat` `str_slice` `str_find` `str_byte` |
| chars | `char_code` `char_of_code` |
| numbers | `to_float` `to_int` `parse_int` `parse_float` `float_bytes` `float_of_bytes` |
| arrays | `array_new` `array_get` `array_set` `array_of_list` `array_to_list` |
| maps | `map_new` `map_get` `map_has` `map_put` `map_remove` `map_pairs` |
| ordering | `compare` |

### `std.console`

| | |
|-|-|
| `print! ..` | writes every argument, then a newline, to stdout |
| `write! ..` | the same without the trailing newline |
| `line! ..` | as `print!` |
| `error! ..` | as `print!`, to stderr |

**Every member is variadic**: it takes however many arguments the call site
passed, writes each in turn with no separator between them, and forces every
one.

```dream
console.print! "done"
console.print! "x = " x ", y = " y
x |> console.print! "x = "            // one application, so this still works
```

Variadic is possible here only because Dream lowers `a |> f b` to a *single*
application node, so a variadic native can take "everything at this call site"
as its meaning. The flip side is that **a variadic function is never partially
applied** — currying cannot tell `f a b` from a half-finished `f a b c` — so
`console.print! "label"` prints immediately rather than returning a function
waiting for a value.

### `std.math`

`sqrt` · `abs` · `floor`

### `std.os`

| | |
|-|-|
| `args! ()` | the arguments after the image on the command line |
| `env! name` / `set_env! name value` | environment variables; `()` when unset |
| `cwd! ()` / `chdir! path` | the working directory |
| `list_dir! path` | the names in a directory, sorted, without `.` and `..` |
| `exec! program args` | run a child to completion → `%{ :code, :out, :err, :timed_out }` |
| `exec_for! program args ms` | the same, killing the child after `ms` |
| `replace! program args` | **become** `program`: this VM is gone and it takes over the process |
| `monotonic! ()` | milliseconds from a fixed point, from a clock that never jumps |
| `now! ()` | milliseconds since the Unix epoch, from the wall clock |
| `pid! ()` · `platform ()` · `exit! code` | |

`replace!` is `execvp`. `exec!` gives its child pipes and reads them to the end,
which is right for a compiler and useless for anything that prompts, so a tool
handing over to something interactive -- an editor, a shell, a REPL -- wants
this instead: the child inherits the terminal because it inherits everything.

`monotonic!` is for durations and `now!` is for stamps, and they are not
interchangeable: the wall clock can jump, forwards or back. Timing anything in
this language means forcing it first, because a stage that has not been forced
has not run:

```dream
let value = stage ();          // builds a thunk; nothing has happened
let before = os.monotonic! ();
strict! value                  // this is where the work is
let after = os.monotonic! ();
```

**`exec!` parks the process, not the worker.** Waiting for a child on a worker
thread would block every process queued behind it, so the whole job — spawn,
read both pipes, reap — goes to a helper thread, and the calling process parks
through the same handshake `recv!` uses. Five children each sleeping a second
finish in one second on a single worker.

`exec_for!` matters for anything that runs other people's programs: a `git
clone` against an unreachable host would otherwise wait for ever. A child that
outlives the deadline is killed, and `timed_out` distinguishes that from an
ordinary non-zero exit.

### `std.vm` — the runtime describing itself

`processes! ()` · `reductions! ()` · `collections! ()` · `heap_bytes! ()` ·
`modules! ()` · `has_ffi ()` · `async_io ()`

And, for when a program has stopped doing what it looked like it would:

| | |
|-|-|
| `processes_info! ()` | every process: status, what it is waiting on, reductions, heap |
| `process_info! p` | one of them |
| `scheduler! ()` | workers, idle, runnable, queued, `io_waiters`, deadlocked |
| `io! ()` | every open handle, and which process is parked on it |
| `dump! ()` | all of the above, to stderr |

`waiting_on` is the part worth having. A process parked on a message and one
parked on a socket look identical from outside, and the difference is usually
the whole answer. `DREAM_STUCK_SECONDS=n` prints the same report from the
runtime when nothing has spent a reduction for that long — at which point no
Dream code can run to ask on its own.

### `std.ffi`

`open!` · `close!` · `bind!` · `load!` · `sizeof` · `alloc!` · `free!` ·
`read_cstr!` · `read_u8!` · `write_u8!`

Built only when libffi is found. Without it every member except `sizeof` raises,
and `vm.has_ffi ()` reports `false`, so a program can degrade rather than fail
to load.

### `mind/std` — the Dream-level library

Anything that can be written in Dream is written in Dream.

- **`std.list`** — the list library. Most of it works on lists that are never
  fully built; functions that must see the whole list to answer (`length`,
  `reverse`, `sort`) say so in their doc comment.
- **`std.seq`** — the generic sequence layer, described below.
- **`std.array`** — arrays. `derive`s `std.seq`, and adds `get`, `set`, `build`,
  `slice`, `map`, `sort` and the rest of what is specific to a flat, finite,
  constant-time-indexed sequence.
- **`std.str`** — text. Also `derive`s `std.seq`, over **characters**, and adds
  `split`, `trim`, `upper`, `find` and friends.
- **`std.json`** — JSON, parsed and written. `parse` answers `[:ok, value]` or
  `[:error, message]` rather than raising, so it stays usable from pure code.
- **`std.toml`** — the subset of TOML a manifest uses: comments, `[section]` and
  `[a.b]` headers, strings, numbers, booleans, arrays and inline tables. What is
  missing — `[[array-of-tables]]`, multi-line strings, dates — is an error
  naming the problem rather than a quietly wrong parse.
- **`std.cli`** — command lines. An option is described once — a spelling, a
  short form, whether it takes a value, and a line of help — and both the parser
  and the usage message read that one description, so an option cannot be
  parsed without being documented or documented without being parsed. Handles
  `--name value`, `--name=value`, `-o value`, `-ovalue`, repeated options that
  collect in order, and `--` to end the options.
- **`std.test`** — the test framework. Each case runs in **its own process**, so
  a case that raises or loops is isolated, and the failure reaches the runner as
  an ordinary value through `join!` rather than having already unwound the
  runner's stack.
- **`std.all`** — imports every module, so that adding a module there is all it
  takes for its tests to run, and a module that no longer compiles fails the
  build rather than being quietly skipped.

### `std.seq` — what `virtual` and `derive` are for

`std.seq` is the standard library's own use of the feature in
[§7](#virtual-and-derive). It declares **one hole**:

```dream
virtual let fold f init xs;
virtual let name xs = "sequence";     // optional, has a default
```

and writes `length`, `is_empty`, `sum`, `product`, `count`, `any`, `all`,
`contains`, `minimum`, `maximum`, `minimum_by`, `maximum_by`, `to_list`, `join`
and `describe` in terms of it. A container becomes a full sequence by answering
that one question:

```dream
// std/array.dr
derive std.seq;
let fold f init xs = { .. };          // a counted loop
let name xs = "array";
let length xs = len xs;               // override: an array knows its own size
```

```dream
array.sum #[1, 2, 3]                  // 6      -- from std.seq
str.count (fn c -> c == 'l') "hello"  // 2      -- from std.seq, over characters
array.describe #[1, 2, 3]             // "array of 3"
```

Because Dream compiles whole programs, `derive` specializes `std.seq`'s syntax
tree against each module's `fold`, so `array.sum` is ordinary code with the
array loop inlined into it. **The generality costs nothing at run time** — there
is no dispatch and no wrapper.

Two things follow from having only `fold`, and the module says both out loud
rather than leaving them to be discovered:

- **Nothing in `std.seq` is lazy.** `fold` walks the whole container, so every
  operation is strict and terminates only on a finite one. This is exactly why
  `std.list` keeps its own implementations instead of deriving: `take 5 (from 1)`
  has to work on an infinite list, and it cannot through a fold.
- **`any` and `all` do not short-circuit.** A fold has no way to stop early, so
  they fold the whole sequence either way.

`std.str` also keeps **characters and bytes apart by name**: `length` and
everything inherited from `std.seq` count characters, while `byte_length`,
`slice` and `find` work in bytes, because the runtime's string primitives are
byte-indexed. `length "héllo"` is `5` and `byte_length "héllo"` is `6`.

### Writing tests

A module opts into the test runner by defining a **parameterless `tests`
binding**, conventionally inside a `when test { .. }` so it costs nothing in a
normal build:

```dream
when test {
    import std.test;

    let tests = [
        test.case "sum"     $( test.eq! 6 (list.sum [1, 2, 3]) ),
        test.case "reverse" $( test.eq! [3, 2, 1] (list.reverse [1, 2, 3]) ),
    ];
}
```

`dreams FILE --test` then scans **the modules it actually loaded** for that
binding and generates an entry point that runs each suite it found. There is no
registry to keep in step, and no test that is silently never run; a program with
no `tests` anywhere still compiles, and reports that there was nothing to run.

Assertions — `test.eq!`, `test.ne!`, `test.true!`, `test.false!`, `test.near!` —
raise on failure, which is what ends a case at its first failure and what the
runner catches.

```
just test-std                       # the standard library's own suite
dreams mind/std/all.dr --test -L mind -o t.dream && dream t.dream
```

---

## 14. The toolchain

### `dreams` — the compiler

```
dreams FILE [-o OUT.dream] [options]
```

| Flag | |
|------|-|
| `-o`, `--output PATH` | where to write the image |
| `-I`, `--include DIR` | add a module search directory |
| `-L`, `--package-path DIR` | add a package search root |
| `--host-module PATH` | a module the host registers at run time (repeatable) |
| `-D`, `--define NAME[=VALUE]` | define a `when` flag |
| `--test` | define `test` and generate a runner (see below) |
| `--release` / `--debug-cfg` | define `release` / `debug` |
| `--print-cfg` | print the flags that are defined |
| `--modules` / `--packages` | report what the program pulls in |
| `--ir` | print the execution trees the program lowers to |
| `--ast` / `--tokens` | print the syntax tree, or the tokens |
| `--parse` | parse this file alone, following no imports |
| `--symbols` / `--at LINE:COL` | what a file declares; what is at a position |
| `--stats` / `--time` | count what a program contains; say what each stage cost |
| `-i`, `--repl` | an interactive session |
| `--shebang [LINE]` | prefix the image with a `#!` line and make it executable |
| `--check`, `--no-emit` | check only: scope and purity, across the whole program |

A file is compiled without being told where the standard library is: a root
file's own directory, `$MINDV2_PATH`, and the nearest enclosing `mind`
directory holding a `std` are searched without being named.

### `dream` — the VM

```
dream PROGRAM.dream [options]
```

| Flag | |
|------|-|
| `-e`, `--entry NAME` | entry point (default `main!`) |
| `-j`, `--workers N` | scheduler threads; `0` means one per hardware thread |
| `--no-jit` | interpreter only |
| `--jit-threshold N` | calls before a function is compiled (default 32) |
| `--dump-jit FN` | print the LLVM IR generated for one function |
| `-x`, `--exec NAME` | run `$MINDV2_PATH/NAME.dream`, and nothing from here |
| `--dump` | disassemble the loaded image |
| `--stats` | reductions and collections |
| `--profile [N]` | count reductions per function and print the hottest |

An image is named the way a program is: `dream mind` tries `mind`, then
`mind.dream`, then both of those under `$MINDV2_PATH`. `-x` is the half of that
without the working directory, so a stray file cannot shadow an installed
program.

`--profile` attributes every reduction to the function whose frame was current,
which is how the compiler was made to say where its own time went. Natives do
not reduce, so work done inside a builtin shows up against its caller.

### `just`

```
just                 # the VM, then the compiler built from its own seed
just run FILE        # compile and run
just repl            # an interactive session
just check FILE      # scope, purity and verification, no image
just test            # VM, end-to-end, examples, library, compiler and server tests
just test-reference  # the differential tests, which are what `dreamc` is still for
just test-all        # the above plus fuzzing, heap verification, no-JIT build
```

`DREAM_VERIFY_HEAP=1` verifies the heap after every collection.

### The pipeline

```
.dr source
   │  lexer.dr     a lazy token stream, newline and column tracking
   │  parser.dr    recursive descent, precedence climbing
   │  modules.dr   whole-program module and package resolution
   │  scope.dr     name resolution, closure conversion, purity checking
   │  lower.dr     execution trees, `derive` specialization, `comp`
   │  ir.dr        node arena, opcodes, constant pool
   │  emit.dr      container writer
   ▼
.dream image        a flat arena of 16-byte nodes linked by index
   │                (format: dreamc/docs/bytecode-format.md)
   ▼
dream              image.cpp loads and revalidates; interp.cpp reduces;
                    jit.cpp compiles the strict numeric spine
```

Every one of those is a module of `dreams`, which is an ordinary Dream program,
so the language server imports the front end as a library rather than parsing a
compiler's output back out of a pipe.

`--shebang` writes an interpreter line before the image and sets the execute
bit, so a compiled program can be run as a command:

```
dreams hello.dr --shebang -o hello    # `#!/usr/bin/env dream` by default
./hello
```

The VM skips a leading `#!` line on any image it loads, so such a file is still
an ordinary image — `dream hello` works too. An image is binary and its magic
number begins with `D`, so a leading `#` is never ambiguous.

An image is a flat arena of 16-byte execution-tree nodes linked by index, so the
VM can map the file and start forcing nodes without rebuilding a tree. The VM
**revalidates every image it loads** — a malformed one is rejected, never
crashed on.

---

## 15. Embedding

[`dream/include/dream/dream.h`](../dream/include/dream/dream.h) is a C API:
create a VM, load an image, register host modules, run an entry point.

```c
dream_vm* vm = dream_vm_new();

const char* names[]           = {"shout!", "total!"};
const uint32_t arities[]      = {1, DREAM_VARIADIC};
const uint32_t strict[]       = {1, 0};
const dream_native_fn fns[]   = {host_shout, host_total};
dream_vm_register_module(vm, "host", names, arities, strict, fns, 2);

dream_vm_load_file(vm, "program.dream", err, sizeof err);
dream_vm_run(vm, NULL);           /* runs main! */
puts(dream_vm_result_text(vm));
dream_vm_free(vm);
```

The Dream side is compiled with the module named, so that the import resolves:

```
dreams program.dr --host-module host -o program.dream
```

[`dream/examples/embed.c`](../dream/examples/embed.c) is this example in full,
and it is built as part of the VM.

The surface falls into six groups:

| Group | |
|-------|-|
| lifecycle | `dream_vm_new` `dream_vm_free` `dream_vm_load_file` `dream_vm_load_bytes` |
| configuration | `dream_vm_set_workers` `dream_vm_set_jit` `dream_vm_register_module` |
| running | `dream_vm_run` `dream_vm_run_value` `dream_vm_result_text` `dream_vm_failed` |
| reading values | `dream_value_type` and the `dream_value_*` accessors; `dream_force` `dream_vm_force_deep` |
| building values | `dream_make_*`, `dream_array_set`, `dream_map_insert`, `dream_map_get` |
| introspection | `dream_vm_reductions` `dream_vm_collections` `dream_vm_process_count` `dream_vm_module_count` `dream_vm_native_module_count` |

Four things are worth knowing:

- **Host functions receive forced arguments** unless the registration cleared
  their bit in `strict_mask`. One trampoline serves every registered function —
  the C function pointer travels on the function value itself — so the number of
  host functions is unlimited rather than capped by a table of generated thunks.
- **An arity of `DREAM_VARIADIC`** makes a member take however many arguments
  its call site passed, with every one forced — `strict_mask` is a 32-bit map of
  argument positions, and an unbounded list has no fixed positions to map. Such
  a member is never partially applied, for the reason given under
  [`std.console`](#stdconsole). This is how `console.print!` is registered.
- **`dream_vm_run_value` is for tools that want the answer rather than a
  transcript.** The result is forced all the way down before it is handed over,
  so every nested value is safe to inspect. This is how the compiler's `comp!`
  works.
- **Values are only meaningful relative to the process that owns them,** and a
  borrowed string pointer is valid only until the next allocation.

`dream_vm_native_module_count` / `_name` exist so a test can prove the
compiler's list of native modules and the runtime's registry still agree.
