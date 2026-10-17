# Start here

Dream is a lazily evaluated functional language. A value is computed when
something needs it, and not before. A function that has effects says so in
its name, and the compiler holds every other function to that promise.
Concurrency is processes that share nothing and talk by sending messages.
Types are optional: write a signature and the compiler checks the program
against it; leave it out and nothing is rejected.

This book teaches the language from the first program to a small web server,
one idea a chapter. Read it in order: each chapter uses what came before.

## How to read this book

Every example in these pages is a whole program, and it was compiled and run
when this site was built. What it printed is shown under it. If a
chapter says a program prints `42`, you are looking at the `42` it printed.

Names in code are links. `list.map` goes to that function in the standard
library's reference, and `std.http.server` to that module, so you can always
look past what a chapter says.

## Getting Dream

Dream is three programs and a library:

| | |
|---|---|
| `dream` | the virtual machine, which runs compiled programs |
| `dreams` | the compiler, itself a Dream program |
| `mind` | the build tool: projects, dependencies, tests, documentation |
| `std` | the standard library, which every program can import |

From a checkout of the repository, `./build.sh` builds the VM and says what
is missing if it cannot, and `just install` puts `dreams`, `mind` and the
library where the VM finds them. After that, `mind` is the only command you
need day to day.

## Your first program

```dream
import std.console;

let main! = console.print! "Hello, Dream!";
```

A program is a file of declarations. `main!` is the one that runs. The `!` at
the end of its name says it has effects. Printing is an effect, and only a
`!` name may print.

Save it as `hello.dr`, compile it to an image, and run the image:

```sh
dream -x dreams hello.dr -o hello.dream
dream hello.dream
```

`dream -x dreams` runs the installed compiler, and `-o` says where to write
the image. An image is the whole program, compiled. It runs on any machine
with a `dream`, whatever its system.

## A project

Anything larger than a file is a project: a directory with a `mind.toml` and
its modules in `src/`.

```sh
mind new greeter
cd greeter
mind run
```

`mind new` writes the manifest and a `src/main.dr` that prints a greeting.
`mind build` compiles without running, `mind test` runs the tests, and
`mind docs serve` builds a site like this one from your comments and guides
and serves it on your machine.

## Trying things out

The quickest loop is a project and `mind run`: change a file, run it, read
what it prints. `mind check` compiles without writing an image, which is the
fastest way to hear what the compiler thinks of a change, types and effects
included. For a lone file, `dream -x dreams hello.dr` does the same.

Next: [Values and functions](01-values-and-functions.md).
