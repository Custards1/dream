# Modules and packages

## A file is a module

Every `.dr` file is a module, named by its path: `src/shapes/area.dr` in a
project is `shapes.area`. `import` brings one in, and its names are then
written with the module's last part in front:

```dream
import std.console;
import std.str;
import std.list;

let main! = console.print! (str.join_str ", " (list.map str.upper ["a", "b"]));
```

`import a.{x, y}` brings names in to use bare, and `import a.{x as z}`
renames one on the way:

```dream
import std.console.{print!};
import std.list.{sum, range as numbers};

let main! = print! (to_string (sum (numbers 1 10)));
```

## Modules inside a file

`mod name { .. }` writes a module inside another. It sees the names of the
file around it, and the file reaches into it with the module's name:

```dream
import std.console;

mod temperature {
    let to_fahrenheit c = c * 9.0 / 5.0 + 32.0;
    let to_celsius f = (f - 32.0) * 5.0 / 9.0;
}

let main! = console.print! (to_string [temperature.to_fahrenheit 100.0, temperature.to_celsius 212.0]);
```

## Keeping things private

`priv let` is visible only inside its own module. Use it for helpers, so that
what a module offers is what it means to offer:

```dream
import std.console;

mod bank {
    priv let fee = 2;
    let withdraw amount balance = balance - amount - fee;
}

let main! = console.print! (to_string (bank.withdraw 50 100));
```

## Packages

A package is a directory with a `mind.toml`:

```toml
[package]
name = "greeter"
version = "0.1.0"
description = "Says hello, politely"

[dependencies]
colors = "../colors"
json = "https://github.com/someone/dream-json#v1.0"
```

Listing a dependency is all it takes to import it. The key, `colors` or
`json`, is the name you import it by. `mind add ../colors` writes that line
for you, `mind deps` lists what a project resolves to, and `mind build`
fetches what is missing.

`mind help` covers the rest: workspaces, build scripts, options and
packaging.

Next: [Processes](08-processes.md).
