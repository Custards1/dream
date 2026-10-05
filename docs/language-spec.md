# The Dream language

Dream is a lazily evaluated functional language. Effects are marked in names
and checked; concurrency is green processes that share nothing; types are
optional descriptions that the compiler checks wherever a program writes one.
This document is the language as the compiler in [`dreams/`](../dreams) and
the VM in [`dream/`](../dream) implement it today. The examples were compiled
and run against the current toolchain, and where one shows output or a
diagnostic, that is what it prints. Fragments write `..` for code left out.

It is a reference rather than a tutorial. For a tour, read the programs in
[`examples/`](../examples/README.md) in order. For the library, see
[builtins.md](builtins.md) and [`mind/std`](../mind/std/README.md). For how
the machine works, see the [VM's README](../dream/README.md).

## Contents

1. [A first program](#1-a-first-program)
2. [Lexical structure](#2-lexical-structure)
3. [Values](#3-values)
4. [Expressions](#4-expressions)
5. [Laziness and strictness](#5-laziness-and-strictness)
6. [Purity](#6-purity)
7. [Bindings and patterns](#7-bindings-and-patterns)
8. [Errors](#8-errors)
9. [Types](#9-types)
10. [Records and unions](#10-records-and-unions)
11. [Modules and packages](#11-modules-and-packages)
12. [Behaviours: `virtual` and `derive`](#12-behaviours-virtual-and-derive)
13. [Processes](#13-processes)
14. [Compile time: `comp`, `when` and macros](#14-compile-time-comp-when-and-macros)
15. [Libraries: `foreign` and `image`](#15-libraries-foreign-and-image)
16. [Tests](#16-tests)
17. [Running a program](#17-running-a-program)
18. [Writing code that runs fast](#18-writing-code-that-runs-fast)
19. [Grammar summary](#19-grammar-summary)

---

## 1. A first program

```dream
import std.console;
import std.list;

union Shape { circle(radius : :float), square(side : :float) }

let area shape : :float =
    match shape {
        [:circle, r] => 3.14159 * r * r,
        [:square, s] => s * s,
    };

let rec total !acc shapes =
    match shapes {
        [] => acc,
        [s, ..rest] => total (acc + area s) rest,
    };

let main! = {
    let shapes = [Shape.circle 1.0, Shape.square 2.0];
    let worker = spawn! $( total 0.0 shapes );
    console.print! ("total area: " + to_string (join! worker))

    // An infinite list, of which only five squares are ever computed.
    let squares = list.map (fn n -> n * n) (list.from 1);
    console.print! (list.take 5 squares)
};
```

```
$ just run first.dr
total area: 7.14159
[1, 4, 9, 16, 25]
```

Most of the language comes down to seven rules:

1. **Application is juxtaposition, and it binds tighter than every
   operator.** `f a b` calls `f` with `a` and `b`. `fac n - 1` is
   `(fac n) - 1`, so a recursive call needs `fac (n - 1)`.
2. **Functions are curried.** `f a b` is `(f a) b`, and `f a` is a function
   waiting for `b`.
3. **`x |> f a` passes `x` as the last argument**: it is `f a x`.
4. **A name ending in `!` is impure, and a pure function may not reach
   one.** The compiler checks this across every import.
5. **Everything is lazy** until something needs its value. The statements
   of an impure block run in order; most other things are computed only if
   they are used.
6. **`$( e )` suspends `e`** as a value, which is what `spawn!` runs.
7. **Types are optional.** A signature is checked where it is written, and
   code that no signature touches is never rejected.

---

## 2. Lexical structure

A source file is UTF-8 text with the extension `.dr`.

### Comments

```dream
// to the end of the line
/* a block, which does not nest */
```

By convention, `///` before a declaration documents it.

### Names

```
name  ::=  [A-Za-z_] [A-Za-z0-9_]* '!'?
```

The trailing `!` is **part of the name**: `print!` and `print` are different
names, and the `!` is what marks a binding impure ([§6](#6-purity)). It is
taken greedily, so `a!= b` is the name `a!` followed by `=`; write `a != b`.

`_` alone is the wildcard pattern. Names beginning with `_` followed by a
letter are the runtime's primitives (`_list_cons`), which belong to `std`
([§3](#builtins-and-primitives)).

### Keywords

```
let  let?  priv  rec  if  else  import  as  catch  true  false  not  try!
fn  virtual  derive  comp  comp!  when  mod  match  macro  expand  type  union
```

Some words are **contextual**: they mean something only where a declaration
or form can begin, and are ordinary names elsewhere.

| Word | Special where |
|---|---|
| `group`, `struct`, `mapping` | at the start of an item: a record ([§10](#10-records-and-unions)) |
| `foreign` | at the start of an item: a C library ([§15](#15-libraries-foreign-and-image)) |
| `image` | at the start of an item: another Dream image ([§15](#another-image-image)) |
| `dyn` | after `virtual` or a record's `derive` ([§12](#dispatch-virtual-dyn-and-derive-dyn)) |
| `where` | inside a type ([§9](#the-type-grammar)) |
| `strict` | in `(strict name)`, a strict parameter |

So `let group = 1` in a block, and `import std.foreign;` followed by
`foreign.call!`, both work, as does `image.installed` after `import std.image;`.

### Literals

| Form | Examples |
|---|---|
| integer | `42`, `1_000_000`, `0xFF`, `0b1010`, `0o755` |
| float | `1.5`, `2.5e-3`, `1e9`, `1_0.25` |
| string | `"hello\n"`, `"tab\there"`, `"\u{1F600}"` |
| interpolated string | `$"{name} is {age} years old"` |
| character | `'c'`, `'\n'`, `'\u{41}'` |
| atom | `:ok`, `:not_found` |
| boolean | `true`, `false` |
| unit | `()` |
| list | `[1, 2, 3]`, `[]` |
| array | `#[1, 2, 3]` |
| map | `%{ :name => "Ada", "age" => 36 }`, `%{}` |
| thunk | `$( expr )` |

- `_` separates digits in any base and is ignored.
- A float needs a digit on both sides of its point, so `1..2` is `1`, `..`,
  `2`, and `x.field` is never a number. `1e` is the number `1` followed by
  the name `e`.
- Integers have no size limit; a literal of any length is allowed.
- Escapes in strings and characters are `\n \t \r \0 \\ \' \"` and
  `\u{HEX}`, a Unicode scalar value. An unknown escape stands for the
  character itself (`\q` is `q`).
- A string may contain line breaks as written.
- In an interpolated string, `{` opens an expression and `\{` is a brace.
  A plain string's braces are text.
- An atom is `:` followed directly by a name.

### Line breaks end statements

Inside a block, a record body and at the top level, **a line break ends a
statement** unless one of these applies:

- **The next line is indented further** than the line the statement started
  on. This is what lets a call run over several lines:

  ```dream
  let total = add3 (1 + 1)
                   (2 + 2)
                   (3 + 3);
  ```

- **The next line begins with something that cannot begin a statement**: an
  infix operator, `|>`, `.`, `else` or `catch`. It continues the previous
  line whatever its indentation.

  ```dream
  let sum = 1
      + 2
  + 3;                              // still the same statement: 6

  let n = [1, 2, 3]
      |> list.map (fn x -> x * 10)
      |> list.sum;

  let v = if false { 1 }
  else { 2 };
  ```

- **It is inside brackets**: `( )`, `[ ]`, `#[ ]`, `%{ }` or `$( )`. Braces
  `{ }` are not in that list, because a block is a sequence of statements.

`;` ends a statement explicitly and is always allowed. A line at the same
indentation as the statement, or less, starts a new one:

```dream
let main! = {
    console.print! "a"     // two statements
    console.print! "b"
};
```

---

## 3. Values

Dream is dynamically typed: every value carries its kind, and `type_of`
answers it as an atom.

| `type_of` | Values | Written |
|---|---|---|
| `:integer` | integers of any size | `42`, `0xFF` |
| `:float` | IEEE 754 doubles | `1.5` |
| `:bool` | `true`, `false` | |
| `:char` | one Unicode scalar value | `'c'` |
| `:unit` | `()`, the value of an expression with nothing to say | `()` |
| `:atom` | an interned name, compared by identity | `:ok` |
| `:string` | UTF-8 text, immutable | `"text"` |
| `:list` | a chain of cells, lazy in head and tail | `[1, 2]`, `x :: xs` |
| `:array` | a flat sequence with constant-time indexing | `#[1, 2]` |
| `:map` | a hash trie from keys to values | `%{ k => v }` |
| `:pure_fn` | a function, closure or thunk | `fn x -> x`, `$( e )` |
| `:impure_fn` | a function whose name ends in `!` | `let f! x = ..` |
| `:error` | an error value: a kind and a payload | caught by `try!` |
| `:process` | a green process | `spawn! $( .. )` |
| `:module` | a module, as `import` binds it | |
| `:tensor` | packed numbers with a shape ([`std.tensor`](notes/tensors.md)) | `tensor.of_list [1.0]` |
| `:bigstr` | a string past 4 GiB, read from an image's payload ([large-data.md](notes/large-data.md)) | |

### Numbers

**Integers are unbounded.** One that fits in 63 bits is held unboxed; one
that does not is a bignum. The two are one type: `type_of` says `:integer`
for both, `==`, `compare`, map keys and `match` agree across them, and a
result that fits in 63 bits again is small again. A program never chooses.

```dream
9223372036854775807 * 10        // 92233720368547758070
7 / 2                           // 3     -- integer division truncates toward zero
-7 / 2                          // -3
-7 % 3                          // -1    -- `%` takes the sign of the dividend
7.0 / 2                         // 3.5
1 + 2.5                         // 3.5   -- an integer with a float is a float
```

`/` and `%` by an integer zero raise `:divide_by_zero`. Float division by zero
follows IEEE 754. `std.num` converts between numbers and text, and
`std.math` has the floating-point functions.

### Strings and characters

A string is immutable UTF-8 text, and a character is a value of its own, not
a one-character string. Strings are indexed by **byte**: `len`,
`str.byte_length`, `str.slice` and `str.find` work in bytes and cost O(1);
`str.length` and `str.chars` count characters and walk.

```dream
len "héllo"              // 6
str.length "héllo"       // 5
"ab" + "cd"              // "abcd"
```

An **interpolated string** is a `$` in front of the quote, and each `{expr}`
in it is any one expression, rendered as `to_string` renders it -- a string as
its text, anything else as it prints:

```dream
let n = 3;
$"{n} squared is {n * n}"      // "3 squared is 9"
$"items: {[1, 2]}"             // "items: [1, 2]"
$"a literal \{ brace"          // "a literal { brace"
```

The prefix is what opts in: a plain string keeps every `{` it has, since
strings that spell JSON or code are common. The pieces are evaluated when the
string is, so one that raises raises there. It compiles to one opcode,
`str_interp`, over the list of its pieces, and costs one copy of the result.

### Lists, arrays and maps

These are the three containers, and they differ in cost more than in what
they hold:

- **A list** is a chain of cells, each lazy in its head *and* its tail, so a
  list can be infinite and is built only as far as it is read. Reaching the
  front is one step; `len`, `.[n]` and `+` walk. Build one at the front (with
  `x :: xs` or `list.cons`) and reverse once at the end.
- **An array** reads any position in one step. It is the shape for a fixed
  record that is read more often than it is built. Changing an element
  copies the array.
- **A map** is a hash array mapped trie. Its keys are forced and its values
  are lazy. A change shares everything it did not touch, so building one key
  at a time is cheap. Numbers, strings, atoms, characters, booleans, unit and
  processes are compared as keys by value; anything else (a list, an array, a
  map, a function) is a key by identity, so an equal list built elsewhere
  does not find it. `1` and `1.0` are different keys.

Every value is immutable. "Changing" a container answers a new one and
leaves the original as it was ([`.[ ]`](#reading-and-changing-a-container)).

### Equality and order

`==` and `!=` compare structurally and never raise: `[1, 2] == [1, 2]`,
`%{ :a => 1 } == %{ :a => 1 }`, `1 == 1.0`. Values of different kinds are
unequal (`"1" == 1` is `false`).

`<`, `<=`, `>` and `>=` order numbers, characters and strings, and raise
`:type_error` on anything else. `compare a b` answers `-1`, `0` or `1` and
orders values of any kind, including lists, which is what `list.sort` uses.

### Functions and thunks

A function is a value. A closure captures the variables it uses. A thunk
written `$( e )` is a function of no arguments, and `type_of` reports it as
`:pure_fn`. `spawn!` runs a thunk in a new process, and `std.test` holds a
test case in one.

### Builtins and primitives

A few names are part of the language and need no import. A binding of the
same name shadows them.

| | |
|---|---|
| `spawn!`, `join!`, `send!`, `recv!`, `self!` | processes ([§13](#13-processes)) |
| `raise!` | raise an error ([§8](#8-errors)) |
| `strict!` | force a value all the way down ([§5](#5-laziness-and-strictness)) |
| `type_of` | a value's kind, as an atom |
| `to_string` | any value as text |
| `len` | the length of a list, array, map, or string (in bytes) |
| `compare` | the order of any two values: `-1`, `0` or `1` |
| `type_assert` | what a checked `types.enforce` compiles to |

Everything else the machine provides is a **primitive**, spelled with a
leading underscore (`_list_cons`, `_str_slice`) and declared in
[`dreams/builtins.dr`](../dreams/builtins.dr). Primitives are `std`'s to
call: every one has a `std` function that wraps it (`list.cons`,
`str.slice`) and compiles to the same operation. Naming a primitive outside
`std` compiles, with a warning:

```
warning: `_list_cons` is a primitive, meant to be called through `std`
    = write `list.cons x xs` instead
```

---

## 4. Expressions

### Precedence

From loosest to tightest:

| Operators | Associativity |
|---|---|
| `\|>` | left |
| `\|\|` | left |
| `&&` | left |
| `==` `!=` `<` `<=` `>` `>=` | left |
| `::` | **right** |
| `\|` | left |
| `^` | left |
| `&` | left |
| `<<` `>>` | left |
| `+` `-` | left |
| `*` `/` `%` `@` | left |
| prefix `-`, `~`, `not`, and `comp`, `comp!`, `expand` | prefix |
| application `f a b` | left |
| `.name`, `.[ ]` | postfix |

Consequences worth knowing:

```dream
fac n - 1          // (fac n) - 1
fac (n - 1)        // a recursive call
f a b + g c        // (f a b) + (g c)
not f x            // not (f x)
- 3 + 1            // -2
f x.y              // f (x.y)
1 :: 2 :: []       // [1, 2]
n - 1 :: rest      // (n - 1) :: rest
```

**A negative argument needs parentheses.** `-` cannot begin an argument, so
`f -1` is `f - 1`, a subtraction, and raises `:type_error` when it runs.
Write `f (-1)`.

### Application, currying and the pipe

```dream
let add a b = a + b;
let inc = add 1;                  // partial application
inc 41                            // 42

[1, 2, 3] |> list.map inc |> list.sum      // list.sum (list.map inc [1, 2, 3])
```

A parameter written `()` takes a slot but binds no name: `let now! () = ..`
is called as `now! ()`.

### Operators

The integer operators `&`, `|`, `^` and `~` use infinite two's complement:
`~x == -x - 1` and `-1 & x == x`. They accept integers only, including
arbitrarily large integers. `x << n` multiplies by `2^n`; `x >> n` divides
by `2^n` rounded toward negative infinity, so `-3 >> 1 == -2`. A negative
count raises `:out_of_bounds`; a nonzero left shift by more than `2^32` bits
raises `:out_of_memory` before allocation. Ordinary process heap limits
also apply. Huge right shifts yield `0` or `-1` according to the sign.

`x & 1 == 0` means `(x & 1) == 0`; `1 << n - 1` means `1 << (n - 1)`.


| | |
|---|---|
| `+` | adds numbers; joins two strings; joins two lists (without forcing their elements) |
| `- * / %` | arithmetic ([§3](#numbers)) |
| `+ - * / %` on tensors | elementwise; `a @ b` is the matrix product ([`std.tensor`](notes/tensors.md)) |
| `::` | `x :: xs`, a list with `x` in front of `xs`; the tail stays lazy |
| `==`, `!=`, `<` .. `>=` | [§3](#equality-and-order) |
| `&&`, `\|\|` | short-circuit: the right side is evaluated only if it decides the answer |
| `not` | boolean negation |

An operator applied to kinds it does not take raises `:type_error` at run time,
unless a signature lets the compiler see it first ([§9](#signatures)).

### `if`

```dream
if n < 0 { :negative } else if n == 0 { :zero } else { :positive }
```

The branches are blocks, so the braces are required. The condition is
evaluated; only the branch it picks is. Without an `else`, a false condition
gives `()`. In a condition, a `{` opens the branch rather than passing a
block as an argument.

### Blocks

```dream
{ let a = 1; let b = a + 1; a + b }        // 3
```

A block is a sequence of statements. Its value is the last one, or `()` when
it is empty. A `let` in a block is in scope for the statements after it.

### `match`

```dream
match value {
    0 => :zero,
    n if n < 0 => :negative,
    [x, ..rest] => [:list, x, rest],
    _ => :other,
}
```

Arms are tried in order and the first that matches wins. Patterns are
described in [§7](#patterns).

### Lambdas

```dream
fn x -> x + 1
fn a b -> a * b
fn [k, v] -> k + v             // a parameter may be a pattern
fn !acc x -> acc + x           // or strict
```

A lambda needs at least one parameter. Its body runs as far as the
expression goes, so a lambda passed as anything but the last argument needs
parentheses.

### Reading and changing a container

```dream
point.[:x]                    // the value at the key; :no_such_key when absent
point.[:x else 0]             // ..or 0, evaluated only when it is the answer
point.[:x => 5]               // a new map like `point`, with :x bound to 5
xs.[0]                        // a list or an array, by position
grid.[y].[x => 0]             // postfix, so it chains
```

`.[ ]` is one pair of operations over every container: a map is read and
changed by key, and an array or a list by position. A position past either
end raises `:out_of_bounds`. The dot is what tells it from application:
`f [0]` passes a list to `f`, and `xs.[0]` reads one.

A change answers a new container. A map shares everything the change did not
touch, an array is copied, and a list rebuilds the cells in front of the
position and shares the rest.

Nothing is forced that the operation does not need. The container and key
are; the element read is the answer, and its neighbours are not; a fallback
is evaluated only when it is used; and a stored value stays unevaluated.
`[1 / 0, 7].[1]` is `7`.

### Members

`m.name` reads the member `name` of a **module**, and nothing else. A name
with a `!` is spelled with it (`console.print!`). `.name` is not field
access: on a map it raises `:type_error`. Use `.[:name]` for a map's entry,
or a record's accessor ([§10](#10-records-and-unions)).

---

## 5. Laziness and strictness

Every argument, list element, map value and `let` binding starts out
**suspended**: a thunk that knows how to compute it. Forcing a thunk computes
the value and overwrites the thunk with it, so the work happens once and
everyone holding it sees the result.

```dream
let ones = list.repeat 1;          // an infinite list
list.take 3 ones                   // [1, 1, 1]

let [a, b] = [1 / 0, 2];           // b is 2, and nothing ever divides by zero
```

What is evaluated without being asked:

- the statements of an **impure block**, in order;
- the condition of an **`if`**, and the subject of a **`match`** as far as
  its patterns look;
- the body of a **`try!`**;
- a **strict parameter**, when the function is entered.

A pure statement whose value is not used is never evaluated, so it cannot
raise an error the program never asked for.

### `strict!`

`strict! e` evaluates `e` **all the way down** (every element of every list,
every value in every map) and answers it. Laziness has one sharp edge: an
effect in a suspended position does not happen until something forces it.

```dream
// Each spawn! is suspended in the list, and runs only when join! reaches it.
let ps = list.map (fn n -> spawn! $( work n )) jobs;

// Every process has started before the first join!.
let ps = strict! (list.map (fn n -> spawn! $( work n )) jobs);
```

`strict!` is impure by name, because forcing is when effects happen. Forcing
something already forced costs nothing. It takes a block too:
`strict! { let a = 2; [a, a * 2] }`.

**`strict!` inside a value is itself suspended.** `[:ok, strict! x]` builds
a list whose second element is a thunk of the force. To force `x` first, make
it a statement: `strict! x` on its own line, then `[:ok, x]`. The compiler
warns about a `strict!` written as an element of a list, array or map
literal (`dreams/lint.dr`).

### Strict parameters

A parameter written `!name`, or `(strict name)`, is forced when the function
is entered, before its body runs:

```dream
let rec sum_to !acc n = if n == 0 { acc } else { sum_to (acc + n) (n - 1) };
sum_to 0 1000000                   // 500000500000

list.fold (fn !acc x -> acc + x) 0 xs
```

This is the fix for an accumulator. Without the `!`, each call suspends
`acc + n` with a reference to the previous suspension, and a million-step
loop builds a million-link chain that overflows when it is finally read. A
strict parameter is forced even when the body never uses it, so
`let ignores !x y = y` raises if `x` does.

The compiler warns about the plainest case, a parameter that is only ever
handed on to the function's own recursive call, grown by `+`, `-` or `*`:

```
warning: `acc` is an accumulator nothing forces: each call of `count` hands it on suspended, and the chain overflows when it is read
    = make it strict with `!acc`, so each call forces it as it starts
```

A strict parameter is also how a loop gets compiled by the JIT
([§18](#18-writing-code-that-runs-fast)).

---

## 6. Purity

**A name ending in `!` is impure, and a pure function may not reach one.**

```dream
import std.console;
let greet name = console.print! name;
```

```
greet.dr:2:18: error: cannot use the impure function `console.print!` inside the pure function `greet`
    = mark the function impure by ending its name with `!`, e.g. `let f! x = ..`
```

What counts as impure is spelled with a `!`: printing and IO, `spawn!`,
`send!`, `recv!`, `join!`, `self!`, `raise!`, `try!`, `strict!`, and every
function a program names with one. The rule holds across imports, and
through `import m.{f!}` as through `m.f!`.

Purity is what makes laziness safe to reason about. A pure expression can be
computed late, early, once or not at all, and nothing observable changes.
Statements in an impure block run in order, because an impure block is where
order is visible.

A pure function can still fail: `1 / 0` raises wherever it is forced. What it
cannot do is *choose* to raise or catch. A pure function that can fail
answers a result value ([`let?`](#let-errors-as-values)).

---

## 7. Bindings and patterns

### `let`

```dream
let name = expr;                    // a value
let f a b = expr;                   // a function, curried
let rec loop n = ..;                // may refer to itself
let effect! x = ..;                 // impure, by the `!`
let [a, b] = pair;                  // destructuring
let area %{ :w => w, :h => h } = w * h;    // a parameter may be a pattern
priv let helper x = ..;             // not visible outside this module
```

**At the top level, every `let` is a global, and globals are mutually
visible**: they may refer to each other in any order, so mutual recursion
needs nothing special, and `rec` is optional (the standard library writes it
anyway, as documentation).

```dream
let even n = if n == 0 { true } else { odd (n - 1) };
let odd n = if n == 0 { false } else { even (n - 1) };
```

**In a block, a `let` is in scope only after it**, so one that refers to
itself needs `rec`:

```
error: cannot find `go` in this scope
```

A `let` may carry a signature ([§9](#signatures)).

### Patterns

| Pattern | Matches |
|---|---|
| `_` | anything, binding nothing |
| `x` | anything, binding it to `x` |
| `1`, `-1`, `1.5`, `'c'`, `"s"`, `:atom`, `true`, `()` | that literal, by `==` |
| `[]` | the empty list |
| `[a, b, c]` | a list of exactly three elements |
| `[x, ..rest]`, `x :: rest` | a non-empty list; `rest` is the tail |
| `[x, ..]` | a non-empty list, ignoring the tail |
| `#[a, b]`, `#[a, ..rest]` | an array of exactly two elements, or at least one |
| `%{ :k => p }` | a map with the key `:k` whose value matches `p`; other keys are ignored |
| `p as name` | `p`, also binding the whole value to `name` |
| `(p)` | `p`: parentheses only group |

Patterns nest, and a name may be bound once per pattern. `::` groups to the
right and binds tighter than `as`: `h :: t as whole` names the whole list,
and `h :: (t as rest)` names the tail. A map pattern's key is an expression.

A `match` arm may have a **guard**: `pattern if condition => body`. The guard
runs only after the pattern matched, and can use what it bound.

**A pattern forces only what it needs to decide.** `_` and a name force
nothing. Any other pattern forces the value to its outermost constructor,
and a nested pattern does the same along the path it inspects: `[x, ..rest]`
forces the first cell but neither `x` nor `rest`, which is what lets a
`match` walk a list that is still being produced.

A `match` with no arm that matches raises an error of kind `:match_error`
whose payload is `[value, path, line, col]`: what did not fit, and where the
`match` was written, the path relative to the search path the file was found
under. Uncaught, it is reported the way a compiler diagnostic is:

```
dream: uncaught error: shapes.dr:12:5: no pattern fits [:hexagon, 2]
```

Make a `match` total with a final `_` arm, or, on a declared union, by
covering every variant ([§10](#unions)).

### Destructuring

A `let`, a parameter and a lambda's parameter take the patterns an arm takes,
and bind the names in them:

```dream
let [first, ..rest] = xs;
let %{ :x => x, :y => y } = point;
let [a, b] as pair = line;
let add [a, b] = a + b;
let f ([x, ..] as whole) n = ..;          // an `as` goes in parentheses
list.map (fn [k, v] -> k * v) pairs
```

It is as lazy as any `let`. Nothing is checked where it is written. The first
time one of its names is used, the whole pattern is checked against the
value, once, and a value that does not fit raises the `:match_error` above. A
destructuring none of whose names is used never looks at its value.

A literal in a destructuring pattern is a compile error, because a `let` has
no other arm to fall through to:

```
error: `:ok` could fail to match, and a `let` has no other arm to try
    = use `match`, with an arm for whatever else the value can be
```

So is a pattern that binds no name (`let [_, _] = p`) and one with no shape
to take apart (`let (x as y) = v`).

### `let?`: errors as values

A pure function that can fail answers `[:ok, value]` or `[:error, reason]`.
A chain of such steps is written with `let?`:

```dream
let number s = if s == "x" { [:error, "not a number"] } else { [:ok, 2] };

let ratio a b = {
    let? x = number a;          // [:ok, x] binds x and goes on;
    let? y = number b;          // anything else is the block's answer
    [:ok, x + y]
};

ratio "a" "b"                   // [:ok, 4]
ratio "x" "b"                   // [:error, "not a number"]
```

`let? p = e; rest` means exactly

```dream
match e { [:ok, v] => { let p = v; rest }, other => other }
```

so `p` may be a pattern, and every value that is not `[:ok, _]` is passed on
unchanged, as `std.result` and `macros.with_ok` pass it. `let?` binds one
value with no parameters, `rec` or signature, and must be followed by the
statements it guards.

---

## 8. Errors

An error is a value with a **kind** (an atom) and a **payload** (anything).

```dream
raise! "something went wrong"              // kind :error, payload "something went wrong"
raise! (error.new :my_kind detail)         // a kind of your own (std.error)

try! { 12 / 0 } catch e { 0 }              // 0
```

`raise!` raises any value. An error value is raised as it is, and anything
else is raised as an error of kind `:error` with that value as its payload.
`try! { body } catch name { handler }` evaluates the body strictly, so the
error surfaces at the `try!` and not wherever the value would later have been
forced. The `catch` and its name are required. An error renders as
`<error :kind payload>`, and `std.error` reads one (`error.kind`,
`error.payload`) and makes one (`error.new`, `error.raise_as!`).

Both are impure. Code that must stay pure reports failure with result values
and `let?` ([§7](#let-errors-as-values)).

Kinds the runtime raises:

| Kind | Raised by |
|---|---|
| `:divide_by_zero` | integer `/` or `%` by zero |
| `:type_error` | an operation given a kind it does not take; `types.check` |
| `:not_a_function` | applying something that is not a function |
| `:no_such_key` | `m.[k]` where the map has no `k` |
| `:out_of_bounds` | a position past either end of a list or array |
| `:no_such_member` | a member a module does not have, found only at run time |
| `:loop` | a value whose computation needs itself |
| `:match_error` | a `match` no arm fits, or a destructuring the value does not; `[value, path, line, col]` |
| `:killed` | `proc.kill!`: the process was stopped from outside ([§13](#13-processes)) |
| `:stack_overflow`, `:out_of_memory` | a process past its limits (below) |
| `:not_found`, `:permission_denied`, `:io_error`, ... | IO; [builtins.md](builtins.md#runtime-error-atoms) has the list |

A failed `match` and a failed destructuring raise `:match_error`, with the
value and its position as the payload ([§7](#patterns)).

### Runaway processes

The runtime bounds what one process may use, and raises **in that process**
rather than failing the whole program:

| Limit | Default | Raises | Set with |
|---|---|---|---|
| pending continuations | 4,194,304 | `:stack_overflow` | `DREAM_MAX_DEPTH` |
| value stack entries | 4,194,304 | `:stack_overflow` | `DREAM_MAX_STACK` |
| heap bytes per process | 1 GiB | `:out_of_memory` | `DREAM_MAX_HEAP` |

These are ordinary errors: `try!` catches them, `join!` delivers them, and
other processes carry on. A tail call uses no stack, so a loop never comes
near the first limit. Runaway *non-tail* recursion does, and the usual cause
is the precedence rule:

```dream
let rec f n = f n - 1;      // (f n) - 1: the subtraction waits on every call
let rec f n = f (n - 1);    // what was meant
```

---

## 9. Types

Types in Dream are optional, and come in two layers that use one notation:

- A **description** is an ordinary value describing a set of values, which a
  program checks against when and where it chooses (`std.types`).
- A **signature** says what a name is, and the compiler checks the program
  against it. Code that no signature touches is never rejected.

Nothing is inferred into a value and nothing is coerced. A type changes how
a value is checked, never how it is represented.

### `type`

```dream
type Port      = :integer where fn n -> n >= 1 && n <= 65535;
type Ints      = [:integer];
type Pair a    = [a, a];
type Outcome v = [:ok, v] | [:error, :string];
```

`type Name params = description` is a `let` whose value is the description,
named, so a type follows `let`'s rules for imports, `priv`, currying and
local scope. A type name must be pure.

### The type grammar

The right side of a `type`, a signature, and a record field's annotation are
read in a grammar of their own. It is the only place a bracket means a type;
everywhere else `[:integer]` is still the list holding one atom.

| Written | Means |
|---|---|
| `:integer`, `:string`, `:pure_fn`, ... | a primitive: an atom `type_of` answers ([§3](#3-values)) |
| `:any`, `:never` | everything (without forcing it), and nothing |
| `:ok`, `"fast"`, `3`, `'c'`, `true`, `()` | a literal: that value and no other |
| `Name`, `mod.Name`, `Name arg` | a named type, and a parameterised one applied |
| `a`, `b`, ... | in a signature, a lowercase name nothing defines is a type variable |
| `a -> b` | a function; right-associative |
| `a \| b` | either |
| `[t]` | a list of `t`, of any length |
| `[a, b, ...]` | a list of exactly those, in order, which is why `[:ok, v]` reads as itself |
| `#[t]`, `#[a, b]` | the same two, for an array |
| `%{k => v}` | a map, when the key names a kind: `%{:string => :integer}` |
| `%{:host => t, ...}` | a record, when the keys are literals; the named keys must be present |
| `t where predicate` | `t`, and the predicate (an ordinary expression) answers `true` |
| `( t )` | grouping |

In a field annotation, put a space between the `:` and the type
(`x : :integer`), because `:integer` is itself one token.

### Checking against a description

```dream
import std.types;

type Step = :integer -> :integer;

types.accepts Port 80            // true
types.accepts Port 0             // false
types.check Port 70000           // raises :type_error carrying `Port`
types.enforce Step f             // f, checking each argument and answer
```

The type grammar is only read after `type`, a signature's `:` and a field's
`:`, so a description used in an expression is named first, as `Step` is
here.

`types.check T v` answers `v` or raises. A check forces only what membership
needs: a list type walks the spine, `[:any]` leaves the elements alone, and
a union stops at its first match. `std.types` also builds descriptions at
run time (`list_of`, `one_of`, `record`, `refine`, `enum`, `range`, ...),
producing the same data the grammar does.

### Signatures

```dream
let add : :integer -> :integer -> :integer;     // a signature on its own..
let add x y = x + y;                             // ..and its definition

let limit : :integer = 10;                       // a value and its type
let greet who : :string = "hi " + who;           // what a function answers

let first : [a] -> a;                            // `a` is a type variable
```

The type written after a function's parameters is what it *answers*, so
`greet` is `:any -> :string`. A signature on its own must be followed by a
`let` of the same name in the same module or block. Signatures work in
blocks as well as at the top level.

At compile time, the checker holds:

- the definition to its signature: the body, each branch of an `if`, each
  arm of a `match`, and each use of a parameter;
- every use of a signed name: each argument to its parameter, and the number
  of arguments to the number of arrows;
- the standard library's own signatures, so `list.map 5 xs` is an error in a
  program that annotated nothing;
- every `match` on a declared union, for a variant it does not handle
  ([§10](#unions)).

```
error: argument 2 of `add` should be `:integer`, but this is `"two"`
error: `add` takes 2 arguments, but it is given 3
error: `n` should be `:string`, but this is `:integer`
```

A lambda passed where a function type is expected takes its parameter types
from it, so in `list.map (fn w -> w * 2) words` with `words : [:string]`, the
checker knows `w` is a string and says `*` wants a number. A generic
function's type variables are solved from each call.

**Optional means optional.** A name with no signature is `:any`, which fits
everywhere. An operator given the wrong kinds is reported only when a
declared type is involved: `1 + "x"` compiles and raises when it runs (inside
a `try!`, that may be the point), while `s + 1` with `s : :string` is an error.
A refinement is checked as its base type, since its predicate can only run
on a value. There is no run-time check of a signed function's arguments.
`types.enforce` is that, where it is wanted.

**Narrowing.** The checker follows what a test proves. After an unguarded
`() => ..` arm, or in the branch where `x != ()`, `x` is not `()`; after
`type_of x == :string`, `x` is a string; comparing `x` (or `x.[0]`, or
`x.[:kind]`) with an atom narrows `x` to the variants that could carry it.
Inside a `match` arm, the subject is only what the pattern could match.

```dream
let find : :string -> :integer | :unit;

let next s = match find s { () => 0, n => n + 1 };          // fine: `n` is :integer
let twice s = { let r = find s; if r != () { r * 2 } else { 0 } };
let bad s = find s + 1;                                      // error: `:integer | :unit` and `1`
```

### Compile-time contracts

Where the compiler already *has* a value, it runs the predicates of the
refinements that value must satisfy:

```dream
type Port = :integer where fn n -> n >= 1 && n <= 65535;
let connect : Port -> :string;

connect 8080             // fine
connect 70000            // error
```

```
error: argument 1 of `connect` should be `Port`, but `70000` is not: a `where` it has to satisfy answered `false` at compile time
```

A value is known when it is a literal (including lists, arrays and maps of
literals), a local bound to one, or the result of `comp`. The predicate runs
on the VM against the program being built, the same function `types.check`
would apply at run time. This is what makes a signature a compile-time API:
`std.sql`'s `Statement` is `:string where` a SQL lint passes, so
`sql.query! db "SELEC 1"` is refused in every program that writes it.

Contracts run only in a build (`-o`), not in `--check`, and only when the
program has no other type errors. A refinement written inline in a signature
is not run; name the type. They add nothing to the image.

### What a signature costs

Nothing at run time. A signature over integers or floats is passed to the JIT
as a hint (it favours the integer fast paths, or unboxed doubles), and
otherwise leaves the image exactly as it was.

---

## 10. Records and unions

### `group`, `struct` and `mapping`

A record declaration makes a module of functions over an ordinary
collection: `group` over a list, `struct` over an array, `mapping` over a map
keyed by atoms named after the fields.

```dream
group Point { x, y = 0 }
struct Vec { x, y }
mapping Person {
    name
    greeting = "Hi"
    say_hi self = greeting self + " " + name self
}

Point.make 1 2                    // [1, 2]
Point.new 1                       // [1, 0]: the fields without defaults
Point.y [1]                       // 0: the default, where the collection has nothing
Point.set_x 9 (Point.make 1 2)    // [9, 2]
Vec.make 1 2                      // #[1, 2]
Person.new "Ada"                  // %{:greeting => "Hi", :name => "Ada"}
Person.say_hi (Person.new "Ada")  // "Hi Ada"
Point.make 1 2 |> Point.set_x 5 |> Point.set_y 6      // [5, 6]
```

For each field `f`, the module has `f record` and `set_f value record`. The
record comes last, as `list.map f xs` takes its list last, so setters chain
with `|>` and `Point.set_x 1` is a function from record to record. `make`
takes every field in order. `new` takes only the fields without a default,
and writes the defaults in.

- **Entries** are separated by `,` or by a line break, and an entry may run
  over as many lines as it is indented past.
- **A default** (`y = 0`) is what reading the field answers when the
  collection has nothing there. It is compiled inside the record's module,
  so it may not name anything from the enclosing one.
- **A member** is an entry with parameters (`say_hi self = ..`): an ordinary
  function compiled inside the record's module, where the accessors,
  setters and other members are in scope without an import. Having
  parameters is the whole of what tells a member from a field. `self` is
  just a name. A member may be impure; a field may not.
- **A field annotation** (`x : :integer`) contributes to `Point.type`, a
  description of the record, and gives the generated functions signatures
  that the checker uses.
- **A strict field** (`!x`, or `(strict x)`) is forced before it is stored:
  it is a [strict parameter](#strict-parameters) of `make`, `new` and
  `set_x`. A suspended field holds whatever its frame reached, and a record
  kept in a table then keeps the table it was computed from; `!` is the fix.
  A default is not forced, since it names nothing and so holds nothing.
  Only a field can be strict: `!` before a member is an error.

A record adds no tag or runtime type. Its values are the list, array or map,
and indexing, equality, patterns and `type_of` see exactly that. A record
can be declared wherever a module item can, including in `mod` and `when`.

### Unions

A union is a value that is exactly one of its variants, with the variant
written on the value:

```dream
union Shape {
    circle(radius : :float)
    rect(w : :float, h : :float)
    empty

    area self = match self {
        [:circle, r]  => 3.0 * r * r,
        [:rect, w, h] => w * h,
        :empty        => 0.0,
    }
}

union Option a { some(value : a), none }

Shape.circle 1.0                 // [:circle, 1.0]
Shape.empty                      // :empty
Shape.area (Shape.rect 2.0 3.0)  // 6.0
types.check Shape s              // the union's name is its description
```

A variant with fields is the list `[:tag, field, ...]`, and one without is the
atom `:tag`. That is the representation Dream code already uses by hand, so
`union Outcome v { ok(value : v), error(reason : :string) }` is exactly the
shape of every result in the standard library.

The declaration makes a module `Shape` (a constructor per variant, the
members, `Shape.type`, and a signature for each constructor) and a global
`Shape` holding the description, so `Shape` is a type wherever a type is
written. A field's type is optional. Parameters after the name make the
union generic.

`union struct Shape { .. }` is the same union with its variants that have
fields made **arrays**, `#[:circle, 1.0]`, and matched with array patterns --
what `struct` is to `group`. A field is then one step away instead of a walk
down the list, and a variant of `n` fields is one object instead of `n + 1`
cells. A variant with no fields is still the atom. `match` dispatches on the
tag of either kind in one step.

```dream
union struct Op { push(n : :integer), add, clamp(lo : :integer, hi : :integer) }

match op {
    #[:push, n] => n,
    :add => acc + 1,
    #[:clamp, lo, hi] => ..,
}
```

**A `match` on a declared union must handle every variant**, wherever the
checker knows the subject's type is that union:

```
error: this `match` on `Shape` does not handle `:empty`; add an arm for it, or `_ =>` to handle everything else
```

A wildcard or a bare name handles everything. An arm with a guard, or one
that takes a field apart with a nested pattern, counts as handling its
variant.

---

## 11. Modules and packages

### Modules

**A module is a file.** `import a.b.c` finds `a/b/c.dr`, or `a/b/c/mod.dr`
once the module has grown into a directory; importers do not change when it
does. A module's top-level `let`s are its members, and `priv let` keeps one
private:

```
error: member `m.hidden` is private
```

`mod` writes a module inside another, holding what a file holds, and nests:

```dream
mod util {
    let helper x = x + 1;
    mod deep { let answer = 42; }
}

util.helper 1          // 2
util.deep.answer       // 42
```

Inside module `m`, `mod util { .. }` *is* the module `m.util`, which `m`
imports, so a submodule and a file are the same thing to everything after
the parser. A module does not re-export what it imports. An import is
resolved relative to the current module first, so `import util.{helper}` in
a module that declares `mod util` means that submodule.

### `import`

```dream
import std.console;                   // binds `console`
import std.list as l;                 // binds `l`
import std.list.{map, sum as total};  // binds members, unqualified, renaming as it goes
import std;                           // a package: `std.list.map`
```

A member import resolves exactly as `path.name` would, purity included. A
member the module does not export is an error that lists what it does
export. A package import binds a namespace, not a value: `std.list.map` can
be passed around, but `std` and `std.list` alone cannot.

### Packages

**A package is a directory of modules with a name**, given by a `mind.toml`
at its root:

```toml
[package]
name = "textstats"
version = "0.1.0"
src = "."

[dependencies]
util = "../util"
```

`import textstats.report` means the module `report` in the package called
`textstats`, wherever that package is. A package's own modules are also
reachable from inside it unqualified (`import report`). `src` defaults to a
`src` directory when there is one. A directory with no manifest can still be
a package, named by the key that lists it.

The compiler is told where packages are with `-L DIR` (a package, or a
directory of packages) and `-L NAME=DIR` (the package at `DIR`, imported as
`NAME`), and follows path dependencies itself. [`mind`](../mind/tool/README.md)
does the rest: fetching, versions, build scripts and options.

**Compilation is whole-program.** Every module a program reaches is compiled
into one image, so a call across modules is resolved when the program is
compiled, not by name at run time. A module that no file provides is an error,
except for the VM's native modules (`std.io`, `std.os`, ...) and modules an
embedder declares with `--host-module NAME`.

---

## 12. Behaviours: `virtual` and `derive`

A `virtual` declares a hole in a module, and a module that `derive`s it fills
the hole. Everything else in the base module is written in terms of its
holes, and the deriving module gets all of it.

```dream
mod shape {
    virtual let area self;                       // must be filled
    virtual let name self = "shape";             // has a default
    let describe self = name self + " of area " + to_string (area self);
}

struct Rect derive shape {
    w
    h
    area self = w self * h self
    name self = "rect"
}

Rect.describe (Rect.make 3 4)                    // "rect of area 12"
```

A file derives with `derive path;` as an item, and a record with `derive path`
between its name and its body. Because Dream compiles whole programs,
`derive` specializes the base module's **syntax** against each deriving
module, so `Rect.describe` is ordinary code with `Rect.area` called directly:
no dispatch and no table. `std.seq` is the library's own example. It declares
`fold`, and `std.array` and `std.str` get `sum`, `count`, `any`, `contains`
and the rest by deriving it.

- A virtual needs at least one parameter.
- An implementation must be public and take as many parameters as the
  virtual. Names may differ; the `!` may not.
- A record's field accessor can satisfy a virtual:
  `mapping Measured derive shape { area }`.
- A module derives one base. Missing implementations and wrong arities are
  compile errors, whether or not anything calls them.

### Dispatch: `virtual dyn` and `derive dyn`

A function written once against a behaviour, for values of any record that
implements it, needs the value to say which implementation it has. That is
opt-in on both sides:

```dream
mod solid {
    virtual dyn let volume self;
    virtual dyn let label self = "solid";
    let report self = label self + ": " + to_string (volume self);
}

struct Cube derive dyn solid { edge, volume self = edge self * edge self * edge self }
mapping Slab derive dyn solid { w, d, h, volume self = w self * d self * h self }

list.map solid.report [Cube.make 3, Slab.make 2 3 4]    // ["solid: 27", "solid: 24"]
```

- **`virtual dyn let`** declares a virtual that, *called through the module
  that declares it*, dispatches on its **last** argument (by convention, a
  member's `self`). A deriving module still gets its own specialized copy.
- **`derive dyn path`** in a record header makes the record's values carry a
  table of their implementations in slot 0: the first element of a list or
  array, or key `0` of a map. The generated functions account for it, and a
  pattern over the raw collection sees it.

A value with no table gets the virtual's default, or an error for a hole. A
dispatched call costs a few reductions more than a direct one, and code that
asks for no `dyn` compiles exactly as before.

---

## 13. Processes

A **process** is the unit of concurrency, of failure, and of garbage
collection. Processes share no memory: a message is copied into the
receiver's heap, so nothing one process does to a value can be seen by
another.

| | |
|---|---|
| `spawn! $( e )` | run `e` in a new process; answers the process |
| `send! p v` | copy `v` into `p`'s mailbox |
| `recv! ()` | the next message, waiting until one arrives |
| `self! ()` | the current process |
| `join! p` | wait for `p` to finish and answer its result; a failure arrives as its error |

```dream
let worker! () = {
    let msg = recv! ();
    [:got, msg]
};

let main! = {
    let w = spawn! $( worker! () );
    send! w :hello
    console.print! (join! w)          // [:got, :hello]
};
```

Processes are cheap (a heap and two small stacks) and preemptively scheduled
across a pool of threads, so one that loops cannot starve the others, and
waiting on a message, a socket or a child process parks the process rather
than the thread. A process that fails and that nobody joins is reported when
the program ends. When every process is waiting on a message that cannot
arrive, the runtime says so rather than hanging.

Isolation is what makes the rest simple: collection is per process and never
stops the world, a thunk can be updated without locks because only one
process can force it, and one process failing cannot corrupt another. The
cost is that a value shared by copying is computed in each process that
forces it.

`recv!` takes the next message, whatever it is. `std.proc` adds the rest,
on a handful of `std.vm` natives:

| | |
|---|---|
| `proc.recv_where! wanted` | the first message `wanted` accepts; the others stay where they were, in order |
| `proc.recv_within! ms` | `[:ok, message]`, or `:timeout` after `ms` milliseconds |
| `proc.monitor! p` | be sent `[:down, p, outcome]` when `p` ends, without waiting for it |
| `proc.kill! p reason` | end `p` as a failure of kind `:killed`; it cannot catch it |
| `proc.sleep! ms` | wait |

```dream
let w = spawn! $( worker! () );
proc.monitor! w
match proc.recv_where_within! (fn m -> proc.is_down_of w m) 1000 {
    [:ok, [:down, _, outcome]] => outcome,
    :timeout => { proc.kill! w :too_slow; :gave_up },
}
```

`receive!` is the same selective receive written as syntax, with `match`'s
arms and an optional `after` for a timeout, and it needs no import:

```dream
receive! {
    [:reply, v] if v > 5 => v,
    [:error, why] => raise! why,
    after 1000 => :timeout,
}
```

The first message in the mailbox that one of the arms takes is taken out and
matched; every other message stays where it was, in order. Without `after`
it waits as long as it takes. Patterns and guards are tried once to choose a
message and again to bind it, so a guard is evaluated twice -- which no pure
expression can tell. `receive!` is only syntax when a `{` follows it, so a
function of that name still works as one.

A kill takes effect at the start of the target's next slice, which is as
prompt as preemption already is -- and a process inside one long force (a
`strict!` over something endless) is stopped where its slice would have
ended. A `try!` cannot catch it. A failure reported to a monitor counts as
handled, as one delivered to a joiner does.

The shapes built on these (servers that hold state, supervisors, registries,
the same server over a socket) are in the standard library:
`std.server`, `std.supervisor`, `std.registry`, `std.remote`. See
[`examples/14_servers.dr`](../examples/14_servers.dr) and after.

---

## 14. Compile time: `comp`, `when` and macros

### `comp` and `comp!`

```dream
let squares = comp list.map (fn n -> n * n) (list.range 1 5);    // [1, 4, 9, 16]
let built_on = comp! os.platform ();                              // :linux
```

`comp e` evaluates `e` while compiling and puts the value in the image.
`comp! e` may perform effects. Both run on a VM embedded in the compiler. A
`comp` binds like a prefix operator over an application, so `comp f x` folds
the whole call and `comp (1 + 2) * 10` is `30`. The result must be data: a
value holding a closure cannot be put in an image. A `comp` is typed by the
value it produced, and one that fails is reported where it is written.

### `when`

```dream
when test {
    import std.test;
    let tests = [ .. ];
}

when os == "linux" && not release {
    let trace! msg = console.error! msg;
}

when release let verbose = false;        // a single item needs no braces
```

`when` includes items only if its condition holds. It is resolved **before
anything is loaded**, so an import inside a false `when` is never followed.
The condition is written with `&&`, `||`, `not`, parentheses, `true`,
`false`, a bare flag, and `setting == "value"`.

A **flag** is defined or not, and a **setting** has a value (and also counts
as defined). The compiler sets `os` (`"linux"`, `"macos"`, `"windows"`) and
`family` (`"unix"`, `"windows"`). Everything else comes from the command
line: `-D name`, `-D name=value`, `--test` (defines `test`), `--release`
(`release`), `--debug-cfg` (`debug`). `dreams --print-cfg` prints what is
set.

A package's **options** are settings scoped to that package. `-D
sqlite:threads=off` sets one; inside `sqlite`, `when threads == "off"` reads
it, and anywhere else `when sqlite.threads == "off"` does. `mind` passes
every package's options this way ([mind's README](../mind/tool/README.md#options-and-profiles)).

### Macros

A macro is a function over syntax, run while compiling:

```dream
macro twice e = [:binary, :add, e, e, [0, 0]];

macro swap e = match e {
    [:list, [a, b], span] => [:list, [b, a], span],
    other => other,
};

let doubled n = expand twice n;          // n + n
expand swap [1, 2]                       // [2, 1]
```

`expand name args` calls the macro with its arguments **unevaluated**, as
syntax, and replaces the call with the syntax it answers. Syntax is ordinary
Dream data: tagged lists whose last element is a source span, in the forms
[`dreams/ast.dr`](../dreams/ast.dr) defines. `x + 1` arrives as
`[:binary, :add, [:name, "x", span], [:int, 1, span], span]`. A transformer
uses ordinary functions, imports and `match` to read and build it.

- `expand` takes a macro name (qualified or imported) and exactly as many
  arguments as the macro has parameters. It binds like `comp`:
  `expand twice x + 1` is `(expand twice x) + 1`.
- Expansion is outside-in. Arguments are not expanded first, and what a macro
  answers is expanded again, to a depth of 64.
- A macro answering `[:error, "message"]` reports that message at the call.
- Macros are not hygienic. Names in the answer resolve where the macro is
  called, and the imports generated code needs must be in the caller.
- Expansion runs on the embedded VM, as `comp!` does, and ordinary purity
  rules apply to the transformer.

`std.macros` has the standard ones (`when_true`, `cond`, `coalesce`,
`with_ok`, `clamp`, `attempt`, ...), and `std.sql`'s `expand sql.query
"SELECT .. {name}"` is a macro that checks SQL while compiling. See
[`examples/11_standard_macros.dr`](../examples/11_standard_macros.dr).

---

## 15. Libraries: `foreign` and `image`

```dream
foreign libc from "libc.so.6" {
    strlen : :cstr -> :size
    getpid! : :void -> :int
}

libc.strlen "hello"            // 5
libc.getpid! ()                // the process id
```

`foreign` declares a C library and the functions a program calls in it. Each
line is a C signature in the type grammar, bound to a name, with `= symbol`
when the C name is not the Dream name without its `!`. A name with `!` is
bound as an effect, and one without as a pure function, which only the
program can know a C function is. Each function is signed with its type on
the Dream side, so passing a `Stmt` where a `Db` is wanted is a compile error.

| Written | In C | In Dream |
|---|---|---|
| `:int`, `:long`, `:size`, `:i32`, `:u8`, ... | that integer | `:integer` |
| `:f32`, `:f64`, `:double` | that float | `:float` |
| `:cstr` | `const char *` | `:string` |
| `:void -> t` | no arguments | `:unit -> t` |
| `resource R = destructor` | an opaque pointer, freed by `destructor` | a handle of type `R` |
| `borrow R` | a pointer C keeps | `R` |
| `out t` | a `t *` C writes through | joins the result: `[result, out, ..]` |
| `struct P { x : :i32, .. }` | a C struct | `P.new!`, `.read!`, `.write!`, ... |
| `(a -> b)` | a function pointer | a pure Dream function |
| `t \| ()` | a result that may be `NULL` | `t \| :unit` |

A pointer C hands back is **owned** by the process that made the call, and is
freed when it is released or the process ends. The library is
`from "path"`, `from system "name"` (one installed on the system, searched
for; `from system "name" "0"` holds it to one ABI version), `from embedded
"name"` (a payload carried in the image), or `from (expression)`.
[ffi.md](ffi.md) is the guide.

### Another image: `image`

```dream
image fmt from installed "formatter" {
    pretty : :string -> :string
    check! : :string -> [:ok, :string] | [:error, :string] = formatter.lint.check!
}

fmt.pretty source
```

`image` declares another compiled Dream program and the functions this one
calls in it. Each line is a name and its Dream type, with `= module.member`
when the function is not the image's top-level `name`. The block is a
module: each name is bound, as an effect if it has a `!` and as a pure
function if not, and signed with the type written, so the checker holds
callers to it. A line with no arrow is a value of the image, computed there
once.

The image runs in a runtime of its own, and only **data** crosses: numbers,
strings, atoms, lists, arrays, maps and errors. A parameter that is a
function is a compile error, and an error the image raises is raised in the
caller. The image is `from "path"`, `from installed "name"` (found as
`dream NAME` finds one), `from embedded "name"` (carried in this image's
payload), or `from (expression)` -- `std.image`'s `any_of`, for one. It is
opened, and each function found in it, the first time it is used.
[images.md](images.md) is the guide.

---

## 16. Tests

A module's tests live in it, in a `when test` block, so they cost nothing in
an ordinary build:

```dream
when test {
    import std.test;

    let tests = [
        test.case "sum"     $( test.eq! 6 (list.sum [1, 2, 3]) ),
        test.case "reverse" $( test.eq! [3, 2, 1] (list.reverse [1, 2, 3]) ),
    ];
}
```

`dreams FILE --test` defines `test`, finds the `tests` binding of every
module the program loaded, and generates an entry point that runs them. Each
case runs in a process of its own, so a case that raises or loops is
isolated from the rest. The assertions (`test.eq!`, `test.ne!`,
`test.true!`, `test.false!`, `test.near!`) raise on failure, which ends the
case. `mind test` builds and runs every package's tests at once.

---

## 17. Running a program

A program's entry point is the global `main!`. Its value is computed and then
discarded. A program that ends normally exits with `0`; an error nothing
caught is printed (`dream: uncaught error: <error :kind payload>`) and exits
with `1`; `os.exit! n` ends the program at once with `n`. The command-line
arguments after the image are `os.args! ()`.

```
dreams -L mind main.dr -o main.dream      # compile (mind does this for a project)
dream main.dream arg1 arg2                # run
dream -e other! main.dream                # run another entry
```

[`dreams/README.md`](../dreams/README.md) is the compiler's command line,
[`dream/README.md`](../dream/README.md) the VM's, and
[`mind/tool/README.md`](../mind/tool/README.md) the build tool's. An image is
portable bytecode and runs unchanged on Linux, macOS and Windows
([platforms.md](platforms.md)). [bytecode-format.md](bytecode-format.md) is
its format, and the VM's [C API](../dream/README.md#embedding) is how to
run one from C.

---

## 18. Writing code that runs fast

The runtime charges for a few things, and each rule here comes from a
measurement ([notes/](notes/README.md) has them).

**Pick the container by how it is read.** A list is cheap at the front and
linear everywhere else: build with `::` or `list.cons` and reverse once, and
never grow an accumulator with `xs + [x]`, which copies the list each time. An
array reads any position in one step. A table searched more often than it is
built wants to be a map; this is the most often repeated lesson in this
codebase.

**Let the machine walk.** A walk the runtime does in one operation (`len`,
`.[n]`, `+` on lists, `str.span`, `str.find`) costs a fraction of the same
walk written as a Dream recursion. `std` already uses them, so prefer its
functions to hand-written loops.

**Force accumulators.** A strict parameter (`!acc`) keeps an accumulator a
value instead of a chain of suspensions. It is also what gets a loop
compiled. The JIT compiles a function once it is hot, but only where it can
prove that evaluating arguments early changes nothing, and a strict
parameter is that proof. `std`'s folds (`list.fold_strict`, `list.sum`) are
written this way.

**Write pipelines.** A chain of `std.list` combinators over a range
(`list.range 1 n |> list.map f |> list.filter p |> list.sum`) is compiled into
one loop that builds no list at all ([deforestation](notes/deforestation.md)).

**Force what you store.** A suspended value stored in a map holds everything
its computation refers to, including older versions of the map it is stored
in. Force values before storing them where they will live long.

**A suspended value carries its frame.** A thunk given to `spawn!` is copied
into the new process with everything it refers to. Build it in a small
function that refers only to what the process needs.

**Measure before believing.**

```
dreams --time FILE           # what each compiler stage cost
dream --profile 20 IMG       # the hottest functions, by reductions
dream --stats IMG            # reductions, collections, allocation, the JIT
```

---

## 19. Grammar summary

Informal; [`dreams/parser.dr`](../dreams/parser.dr) is the definition.

```
file        ::= item*
item        ::= 'import' path ( 'as' name | '.{' member (',' member)* '}' )?
              | 'priv'? 'let' binding
              | 'priv'? 'type' name param* '=' type
              | 'mod' name '{' item* '}'
              | 'derive' path
              | 'virtual' 'dyn'? 'let' name param+ ( '=' expr )?
              | 'when' cond ( '{' item* '}' | item )
              | 'macro' name param* '=' expr
              | ('group' | 'struct' | 'mapping') name ('derive' 'dyn'? path)? '{' entry* '}'
              | 'union' name param* '{' (variant | member)* '}'
              | 'foreign' name ('from' library)? '{' foreign_entry* '}'
              | 'image' name 'from' image '{' (name ':' type ('=' path)?)* '}'
binding     ::= 'rec'? name param* ( ':' type )? '=' expr
              | name ':' type
              | pattern '=' expr
param       ::= name | '!' name | '(' 'strict' name ')' | '()' | '[' .. ']' | '#[' .. ']' | '%{' .. '}'
              | '(' pattern (',' pattern)* ')'
statement   ::= binding-let | 'let?' (name | pattern) '=' expr | 'type' .. | expr
expr        ::= expr '|>' expr | expr binop expr | ('-' | 'not') expr
              | ('comp' | 'comp!' | 'expand') application | application
application ::= postfix postfix*
postfix     ::= primary ( '.' name | '.[' expr ( 'else' expr | '=>' expr )? ']' )*
primary     ::= literal | name | '(' expr ')' | '[' exprs ']' | '#[' exprs ']'
              | '%{' (expr '=>' expr),* '}' | '$(' expr ')' | block
              | 'if' expr block ('else' (block | 'if' ..))?
              | 'match' expr '{' (pattern ('if' expr)? '=>' expr),* '}'
              | 'fn' param+ '->' expr
              | 'try!' block 'catch' name block
block       ::= '{' (statement (';' | newline))* '}'
cond        ::= cond '||' cond | cond '&&' cond | 'not' cond | '(' cond ')'
              | 'true' | 'false' | dotted_name | dotted_name '==' string
```
