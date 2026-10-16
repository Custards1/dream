# Where to go from here

You have seen the whole language in outline: values and functions, patterns,
laziness, effects and results, types, modules, processes, tests, and a
service built from them. What is left is depth.

## The reference

- **The standard library**, in this site's reference: every module, every
  documented function, with its signature and examples. Start with
  `std.list`, `std.str` and `std.map`, then whichever of `std.http`,
  `std.json`, `std.time`, `std.regex` or `std.parse` your program needs.
- **The language specification**, `docs/language-spec.md` in the repository:
  every construct, exactly. It is a reference rather than a tutorial, and
  it reads well once this book is behind you.
- **The examples**, in `examples/`: twenty-odd programs, in order, each with
  the output it prints recorded beside it.

## Things this book left out

- **Macros.** `macro` declares a function that runs at compile time on the
  syntax of its arguments. `std.macros` has the common ones.
- **Compile-time evaluation.** `comp expression` computes a value while
  compiling and builds it into the program.
- **Dispatch.** `virtual dyn` and `derive dyn` choose a function at run time
  by the record it is given.
- **Foreign libraries.** `foreign` binds a C library, signature by signature,
  checked like any other code.
- **Tensors and machine learning.** `std.tensor` and `std.ml`: packed numbers,
  the GPU, automatic differentiation and training.

## Writing code that runs fast

A few habits matter more than any others:

- Mark accumulators strict, `!acc`, as the laziness chapter showed.
- Build a list by putting things on the front and reversing it at the end,
  not with `xs + [x]`, which copies `xs` every time.
- A list you search more than you build wants to be a map.
- `dream --profile N program.dream` names the functions where the time goes,
  and `dream --stats` says how much was allocated and collected.

Welcome to Dream.
