# dreamc

A bytecode compiler for **Dream**, a dynamically typed, lazily evaluated
functional language.

```
cargo build
./target/debug/dreamc examples/sample.dr --dump
```

## The language

Values are integers, floats, chars, bools, unit, and objects. The canonical
list of types lives in [src/types.rs](src/types.rs) — the compiler, the VM's
`type_of`, and this table are checked against it rather than kept in step by
hand.

| Scalars | |
|---------|-|
| `integer` | a signed integer |
| `float` | double precision |
| `char` | one Unicode scalar value |
| `bool` | `true` or `false` |
| `unit` | `()` |

| Object kinds | |
|--------------|-|
| `pure_fn` | a function with no effects |
| `impure_fn` | a function whose name ends in `!` |
| `module` | what `import` binds |
| `list` | `[a, b, c]` — lazy in head and tail |
| `array` | `#[a, b, c]` — constant-time indexing |
| `map` | `%{ k => v }` — keys forced, values lazy |
| `error` | a raised kind and payload, caught by `try!` |
| `process` | a green process (`thread` is accepted as an alias) |
| `atom` | an interned name, `:like_this` |
| `string` | UTF-8 text |

### Processes

A **process** is the unit of concurrency, of failure, and of garbage
collection. Processes share no memory: `send!` copies the message, so nothing
one process does to a value can be seen by another. `spawn!` takes a suspended
computation and returns the process running it.

```
let worker! () = {
    let msg = recv! ();
    msg |> console.print! "got: "
};

let main! = {
    let w = spawn! $( worker! () );
    send! w :hello
    join! w
}
```

Every process operation ends in `!`, so the purity rule below already keeps
concurrency out of pure functions — starting a process is an effect.

```
import std.console;

let rec fac n = if n <= 1 { 1 } else { n * fac (n - 1) };

let greet! who result = {
    who    |> console.print! "Hi, "
    result |> console.print! "The result is "
}

let main! = {
    let number = fac 5;
    spawn! $( greet! "thread 2" number )
    let safe = try! { 12 / 0 } catch e { 0 };
    greet! "thread 1" safe
}
```

Functions are curried and applied by juxtaposition, so `f a b` is `(f a) b` and
**application binds tighter than any operator** — `fac n - 1` means
`(fac n) - 1`, and the recursive call needs `fac (n - 1)`.

`x |> f a` feeds `x` in as the *last* argument, giving `f a x`.

`$( e )` suspends `e` as a thunk object, which is what `spawn!` turns into a
thread.

Inside a block, a newline ends a statement. A line that begins with an infix
operator, `.`, `else`, or `catch` continues the previous one instead, and
newlines never end a statement inside `(`, `[`, `$(`, `#[` or `%{`.

### Purity is part of the type

A name ending in `!` denotes an impure value, and a pure function may not reach
one:

```
error: cannot use the impure member `print!` inside the pure function `greet`
 --> greet.dr:2:5
  |
2 |     console.print! name
  |     ^^^^^^^^^^^^^^
  = note: mark the function impure by ending its name with `!`, e.g. `let f! x = ..`
```

That check is what makes `pure_fn` and `impure_fn` genuinely distinct object
types rather than a naming convention, and it is what lets the compiler decide
where evaluation order matters: statements in an impure block are sequenced,
everything else stays lazy.

## Output

`dreamc` writes a `.dream` image whose format is specified in
[docs/bytecode-format.md](docs/bytecode-format.md). It is a flat arena of
16-byte execution-tree nodes linked by index, so a VM can map the file and
start forcing nodes without rebuilding a tree.

`--dump` disassembles the image by reading it back, so a dump also proves the
container round-trips.

## Layout

| Path | |
|------|-|
| `src/lexer.rs`  | `logos` tokens, newline tracking |
| `src/parser.rs` | recursive descent with precedence climbing |
| `src/lower.rs`  | scope resolution, closure conversion, purity checking |
| `src/ir.rs`     | node arena, opcodes, constant pool |
| `src/emit.rs`   | container writer |
| `src/disasm.rs` | container reader and text dump |
