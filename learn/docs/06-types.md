# Types

Types in Dream are optional, and worth writing. A **signature** says what a
name is, and the compiler checks the program against it. Code no signature
touches is never rejected, so you can start without types and add them where
they help.

## Signatures

```dream
import std.console;

let area : :float -> :float -> :float;
let area width height = width * height;

let label : :string -> :integer -> :string;
let label name count = name + ": " + to_string count;

let main! = console.print! (label "tiles" 12 + ", " + to_string (area 2.5 4.0));
```

`:integer`, `:float`, `:string`, `:bool` and the rest are the kinds
`type_of` answers. `a -> b` is a function from `a` to `b`, and a function of
two parameters is `a -> b -> c`, because it takes one and answers a function
taking the other. `[t]` is a list of `t`, and a lowercase name like `a` is a
**type variable**, any type, the same one each time it appears:

```dream
import std.console;

let pair_up : a -> b -> [a, b];
let pair_up x y = [x, y];

let main! = console.print! (to_string (pair_up :north 3));
```

When a signature and a use disagree, the program does not compile, and the
message says where:

```dream fails
let label : :string -> :integer -> :string;
let label name count = name + ": " + to_string count;

let main! = label 12 "tiles";
```

The standard library is fully signed, so even a program that writes no types
of its own gets its calls into the library checked.

## Naming types

`type` gives a description a name. `|` is "either", and `where` adds a
condition, checked at compile time when the value is known then:

```dream
import std.console;

type Port = :integer where fn n -> n >= 1 && n <= 65535;
type Answer = :yes | :no | :maybe;

let open_on : Port -> :string;
let open_on port = "listening on " + to_string port;

let main! = console.print! (open_on 8080);
```

## Records

A record is a value with named fields. `struct` declares one and makes a
module of functions for it: `make` to build one, an accessor for each field,
and a setter for each field that makes a changed copy:

```dream
import std.console;

struct Point {
    x : :float
    y : :float
    norm self = x self * x self + y self * y self
}

let main! = {
    let p = Point.make 3.0 4.0;
    let moved = p |> Point.set_x 6.0;
    console.print! (to_string [p, moved])
    console.print! (to_string [Point.x moved, Point.norm p])
    let Point(a, b) = moved;
    console.print! (to_string (a + b))
};
```

An entry with parameters, like `norm self`, is a **member**: a function
written inside the record, where the fields' accessors are in scope. A record
prints as itself and is taken apart by its own pattern, `Point(a, b)`.

A field may have a default, and `new` takes only the fields without one. That
is how the library writes a function with many settings:

```dream
import std.console;

mapping Fetch {
    retries : :integer = 3
    timeout : :integer = 30
    verbose : :bool = false
}

let describe : Fetch -> :string;
let describe o = $"{Fetch.retries o} retries, {Fetch.timeout o}s, verbose {Fetch.verbose o}";

let main! = {
    console.print! (describe (Fetch.new ()))
    console.print! (describe (Fetch.new () |> Fetch.set_timeout 5 |> Fetch.set_verbose true))
};
```

## Unions

A **union** is a value that is one of several shapes, each with a name. A
`match` on one must handle every shape, and the compiler checks that it
does:

```dream
import std.console;
import std.list;

union Shape {
    circle(radius : :float),
    rectangle(width : :float, height : :float),
    point,
}

let area : Shape -> :float;
let area shape = match shape {
    Shape.circle(r) => 3.14159 * r * r,
    Shape.rectangle(w, h) => w * h,
    Shape.point => 0.0,
};

let main! = {
    let shapes = [Shape.circle 1.0, Shape.rectangle 2.0 3.0, Shape.point];
    console.print! (to_string (list.map area shapes))
    console.print! (to_string (list.sum (list.map area shapes)))
};
```

Leave out an arm and the program does not compile:

```dream fails
union Light { red, amber, green }

let next : Light -> Light;
let next light = match light {
    Light.red => Light.green,
    Light.green => Light.amber,
};

let main! = next Light.red;
```

The checker holds a `match` to the union only when it knows the value is
one, which is what the signature on `next` tells it. One more reason to write
signatures: they are what lets the compiler check the rest.

`Result` and `Option` from the last chapter are unions like these, declared
in the prelude.

Next: [Modules and packages](07-modules-and-packages.md).
