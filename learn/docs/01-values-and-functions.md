# Values and functions

## Values

Dream has a handful of kinds of value, and `type_of` names each one:

```dream
import std.console;

let main! = {
    console.print! (to_string [type_of 42, type_of 1.5, type_of "text", type_of 'c'])
    console.print! (to_string [type_of true, type_of :ready, type_of (), type_of [1, 2]])
    console.print! (to_string [type_of #[1, 2], type_of %{ :a => 1 }, type_of (fn x -> x)])
};
```

- **Numbers** are integers of any size, and floats. `7 / 2` is `3`, and
  `7.0 / 2.0` is `3.5`.
- **Strings** are text in double quotes. A **character** is a value of its
  own, in single quotes.
- **Atoms** are names that stand for themselves: `:ready`, `:error`,
  `:north`. They are cheap to compare, and Dream uses them where other
  languages use enumerations or symbols.
- **Unit**, `()`, is the value that says "nothing in particular".
- **Lists** `[1, 2, 3]`, **arrays** `#[1, 2, 3]` and **maps**
  `%{ "one" => 1 }` hold other values.

`to_string` turns any value into text, and `+` joins strings:

```dream
import std.console;

let main! = {
    let name = "Ada";
    let year = 1815;
    console.print! (name + " was born in " + to_string year)
    console.print! $"{name} was born in {year}, {2026 - year} years ago"
};
```

A string with `$` in front is **interpolated**: each `{expression}` in it is
replaced by that expression, rendered as `to_string` would.

## Naming things

`let` names a value. A name is fixed once it is given. Nothing in Dream is
changed in place: you make new values from old ones.

```dream
import std.console;

let width = 6;
let height = 7;
let area = width * height;

let main! = console.print! (to_string area);
```

## Functions

A function is a `let` with parameters. Calling one is writing its name and
its arguments, separated by spaces, with no parentheses or commas:

```dream
import std.console;

let square x = x * x;
let hypotenuse a b = square a + square b;

let main! = console.print! (to_string (hypotenuse 3 4));
```

**Application binds tighter than any operator.** `square a + square b` is
`(square a) + (square b)`, and `square 3 + 1` is `10`, not `16`. When an
argument is itself an expression, put it in parentheses: `square (3 + 1)`.

## Every function takes one argument

`hypotenuse 3 4` is really `(hypotenuse 3) 4`: give a function fewer arguments
than it has parameters and you get a function waiting for the rest.

```dream
import std.console;
import std.list;

let add a b = a + b;
let add_ten = add 10;

let main! = {
    console.print! (to_string (add_ten 5))
    console.print! (to_string (list.map add_ten [1, 2, 3]))
};
```

This is called **partial application**, and it is why Dream puts the thing a
function works on last. `list.map f xs` takes the list last, so
`list.map add_ten` is itself a useful function.

## Lambdas and pipes

`fn x -> body` is a function without a name. The pipe `x |> f` passes `x` to
`f` as its last argument, so a chain of steps reads in the order it happens:

```dream
import std.console;
import std.list;

let main! = {
    let total = [1, 2, 3, 4, 5, 6]
        |> list.filter (fn n -> n % 2 == 0)
        |> list.map (fn n -> n * n)
        |> list.sum;
    console.print! (to_string total)
};
```

Read it as: the numbers, then only the even ones, then each squared, then
their sum.

## Blocks

A block, `{ .. }`, is a sequence of statements whose value is its last
expression. A `let` inside a block is visible to the statements after it.
That is how a function computes in steps:

```dream
import std.console;
import std.str;

let describe word = {
    let shout = str.upper word;
    let length = str.length word;
    $"{shout} has {length} letters"
};

let main! = console.print! (describe "dream");
```

Next: [Decisions and patterns](02-decisions-and-patterns.md).
