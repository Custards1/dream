# Effects and errors

## The `!` is a promise

A name that ends in `!` may have **effects**: print, read a file, open a
socket, start a process, wait. A name without one may not, and the compiler
checks it. A pure function cannot call an impure one:

```dream fails
import std.console;

let shout word = console.print! (word + "!");

let main! = shout "hello";
```

The fix is to say what `shout` is. Name it `shout!`:

```dream
import std.console;

let shout! word = console.print! (word + "!");

let main! = shout! "hello";
```

This sounds like a small thing, and it is the most useful thing the compiler
knows. Every function without a `!` is a calculation: it answers the same
thing for the same arguments, you can call it anywhere, test it with no
setup, and run it in parallel. The `!` names are the few places where the
program touches the world, and you can see them at a glance.

In a block, an impure call on a line of its own is a **statement**: it runs
there, in order.

```dream
import std.console;
import std.file;

let main! = {
    let path = "/tmp/learn-dream-notes.txt";
    file.write! path "first line\nsecond line\n"
    let text = file.read! path;
    console.print! ("read back " + to_string (len text) + " bytes")
    console.print! text
};
```

## Raising and catching

`raise! value` stops what is happening, and `try! { .. } catch e { .. }`
catches it:

```dream
import std.console;
import std.error;

let checked_divide! a b = if b == 0 { raise! (error.new :divide_by_zero "b was zero") } else { a / b };

let main! = {
    console.print! (to_string (checked_divide! 10 2))
    let answer = try! { checked_divide! 1 0 } catch e { error.kind e };
    console.print! (to_string answer)
};
```

Both are impure: raising is how something that went wrong *in the world*
gets out. A file that cannot be read, a connection that drops, or a bug.

## Errors as values

A calculation that can fail, such as parsing a number or looking something
up, should not raise. It answers a **result**: `Result.ok value` when it
worked and `Result.error reason` when it did not. One that may find nothing
answers an **option**: `Option.some value` or `Option.none`. Both are
available in every module without an import.

```dream
import std.console;
import std.num;

let parse_age text = match num.parse_int text {
    () => Result.error ("not a number: " + text),
    n if n < 0 => Result.error "an age cannot be negative",
    n => Result.ok n,
};

let main! = {
    console.print! (to_string (parse_age "40"))
    console.print! (to_string (parse_age "forty"))
    console.print! (to_string (parse_age "-3"))
};
```

What to do with a result is a function on `Result`, each taking the result
last so they chain with `|>`:

```dream
import std.console;
import std.num;

let parse text = match num.parse_int text { () => Result.error ("not a number: " + text), n => Result.ok n };

let main! = {
    console.print! (to_string (parse "21" |> Result.map (fn n -> n * 2)))
    console.print! (to_string (parse "x" |> Result.unwrap_or 0))
    console.print! (to_string (parse "x" |> Result.map_error (fn why -> "bad input: " + why)))
};
```

## `let?`: one step at a time

A computation of several steps, any of which can fail, is written with
`let?`. When the step worked, `let?` binds what it holds and carries on.
When it did not, that failure is the block's answer and nothing after it
runs:

```dream
import std.console;
import std.num;

let parse text = match num.parse_int text { () => Result.error ("not a number: " + text), n => Result.ok n };

let ratio top bottom = {
    let? a = parse top;
    let? b = parse bottom;
    if b == 0 { Result.error "division by zero" } else { Result.ok (a / b) }
};

let main! = {
    console.print! (to_string (ratio "84" "2"))
    console.print! (to_string (ratio "84" "zero"))
    console.print! (to_string (ratio "84" "0"))
};
```

No nesting and no checking after every step. The failure path is written
once, where the failure is made.

Next: [Types](06-types.md).
