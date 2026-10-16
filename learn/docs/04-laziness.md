# Laziness

## Nothing is computed until it is needed

Dream evaluates lazily. A value is a promise to compute it, kept the first
time something asks, and remembered after that. Most of the time you will
not notice. Where you will is when the work never needs doing at all:

```dream
import std.console;
import std.list;

let expensive n = list.head [];          // would raise if it ever ran

let main! = {
    let unused = expensive 1;
    let choice = if true { "cheap" } else { expensive 2 };
    console.print! (choice + ", and " + to_string (len [expensive 3, expensive 4]) + " promises never kept")
};
```

`len` counts a list's elements without looking at them, so neither
`expensive 3` nor `expensive 4` ever runs.

## Infinite lists

Because the rest of a list is computed only when somebody walks to it, a list
can go on forever, and you take what you need:

```dream
import std.console;
import std.list;

let rec from n = list.cons n (from (n + 1));
let rec fibs a b = list.cons a (fibs b (a + b));

let main! = {
    console.print! (to_string (list.take 8 (from 1)))
    console.print! (to_string (list.take 12 (fibs 0 1)))
    let squares = list.map (fn n -> n * n) (from 1);
    console.print! (to_string (list.take_while (fn s -> s < 100) squares))
};
```

This changes how you write programs. Make the whole sequence of candidates,
then take the ones you want: the "loop" is in what you ask for, not in how
the list was made.

## Forcing

Sometimes you want a value now: before you measure how long something took,
or before you send it to another process. `strict! x` computes `x` all the
way down. It has a `!` because when things happen is an effect:

```dream
import std.console;
import std.list;

let main! = {
    let numbers = list.map (fn n -> n * 2) (list.range 1 5);
    let forced = strict! numbers;
    console.print! (to_string forced)
};
```

## The one trap: accumulators

Laziness has one cost to watch for. A function that adds to an accumulator
on every step builds a chain of promises, `((0 + 1) + 2) + 3 ..`, and only
adds them up at the end. Over a long enough list, that chain is too deep to
add up.

The fix is a **strict parameter**: write `!acc` and the argument is computed
before the function starts, at every step.

```dream
import std.console;

let rec total !acc n = if n == 0 { acc } else { total (acc + n) (n - 1) };

let main! = console.print! (to_string (total 0 1000000));
```

The library's folds over long lists already do this: `list.fold_strict` and
`list.sum` are strict where it matters. When you write a loop of your own
with an accumulator, mark it `!`. The compiler warns about an accumulator
nothing forces.

## Suspending on purpose

`$( expression )` makes a promise you can pass around, a **thunk**. Running
work in another process starts with one: `spawn! $( work () )` hands
over the work rather than its answer. The [processes
chapter](08-processes.md) is about that.

Next: [Effects and errors](05-effects-and-errors.md).
