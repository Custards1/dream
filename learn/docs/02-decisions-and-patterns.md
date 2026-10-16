# Decisions and patterns

## `if`

`if` is an expression: it has a value, the value of the branch it takes, so
it always has an `else`.

```dream
import std.console;

let parity n = if n % 2 == 0 { "even" } else { "odd" };

let main! = console.print! (parity 7 + " and " + parity 10);
```

## `match`

`match` compares a value against **patterns**, top to bottom, and answers
with the first arm that fits. Anything with more than two outcomes is a
`match`:

```dream
import std.console;
import std.list;

let describe n = match n {
    0 => "zero",
    1 => "one",
    n if n < 0 => "negative",
    n if n < 10 => "a few",
    _ => "many",
};

let main! = console.print! (to_string (list.map describe [0, 1, -4, 7, 300]));
```

A pattern can be a literal (`0`, `"yes"`, `:ok`), a name, which matches
anything and binds it (`n`), or `_`, which matches anything and binds
nothing. `if` after a pattern is a **guard**: the arm is taken only when the
guard is also true.

## Taking values apart

Patterns are shaped like the values they match, and the names inside them
are bound to the parts:

```dream
import std.console;

let first_two xs = match xs {
    [] => "empty",
    [only] => "just " + to_string only,
    [a, b, ..rest] => to_string a + " then " + to_string b + ", and " + to_string (len rest) + " more",
};

let main! = {
    console.print! (first_two [])
    console.print! (first_two [:north])
    console.print! (first_two [1, 2, 3, 4, 5])
};
```

`[a, b, ..rest]` is a list of at least two elements. `rest` is everything
after them. Map patterns take the keys they name and ignore the rest:

```dream
import std.console;

let greet person = match person {
    %{ :name => name, :title => title } => "Good day, " + title + " " + name,
    %{ :name => name } => "Hi, " + name,
    _ => "Hello, stranger",
};

let main! = {
    console.print! (greet %{ :name => "Lovelace", :title => "Countess" })
    console.print! (greet %{ :name => "Grace" })
    console.print! (greet %{})
};
```

`let` takes a pattern too, when the shape is certain:

```dream
import std.console;

let main! = {
    let [x, y] = [3, 4];
    let %{ :host => host, :port => port } = %{ :host => "localhost", :port => 8080 };
    console.print! (host + ":" + to_string port + " at " + to_string (x * y))
};
```

## Recursion

There are no loops. A function that repeats calls itself, usually with a
`match` that says when to stop. `let rec` marks a function that refers to
itself:

```dream
import std.console;
import std.list;

let rec length xs = match xs {
    [] => 0,
    [_, ..rest] => 1 + length rest,
};

let rec countdown n = match n {
    0 => ["liftoff"],
    n => list.cons (to_string n) (countdown (n - 1)),
};

let main! = {
    console.print! (to_string (length [:a, :b, :c]))
    console.print! (to_string (countdown 3))
};
```

Most of the time you will not write the recursion yourself: `list.map`,
`list.filter` and `list.fold` in the next chapter are recursions somebody
already wrote. When
you do write one, the [laziness chapter](04-laziness.md) shows how to keep it
from growing a stack.

Next: [Collections](03-collections.md).
