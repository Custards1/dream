# Processes

A **process** is Dream's unit of concurrency. Processes are cheap: a program
can run hundreds of thousands. They share nothing. Each has its own memory,
and they cooperate by sending each other messages, which are copied. Nothing
one process does to a value can be seen by another, so there are no locks to
take and no data races to find.

## Starting one

`spawn! $( work )` runs `work` in a new process and answers the process.
`join! p` waits for it to finish and answers what it computed:

```dream
import std.console;
import std.list;

let slow_sum n = list.sum (list.range 1 n);

let main! = {
    let a = spawn! $( slow_sum 1000000 );
    let b = spawn! $( slow_sum 2000000 );
    console.print! (to_string [join! a, join! b])
};
```

The two sums run at the same time, on as many cores as the machine has.

## Messages

`send! p value` puts a value in `p`'s mailbox, and `receive!` takes the
first message one of its patterns matches. `self! ()` is the process doing
the asking, which is how a reply finds its way back:

```dream
import std.console;

let rec counter! total = receive! {
    [:add, n] => counter! (total + n),
    [:get, from] => { send! from [:total, total]; counter! total },
    :stop => total,
};

let main! = {
    let c = spawn! $( counter! 0 );
    send! c [:add, 5]
    send! c [:add, 37]
    send! c [:get, self! ()]
    receive! { [:total, t] => console.print! ("total is " + to_string t) }
    send! c :stop
    console.print! ("final " + to_string (join! c))
};
```

This is the shape of most stateful things in Dream: a process that loops,
holding its state as a parameter, and changes it by calling itself with a new
one. `std.server` and `std.agent` are that pattern, written once and
properly. Use them when you write a real one.

## Waiting, but not forever

`receive!` takes `after` for a deadline in milliseconds:

```dream
import std.console;

let main! = {
    let answer = receive! {
        [:reply, v] => v,
        after 100 => :nobody_answered,
    };
    console.print! (to_string answer)
};
```

## When a process fails

A failure stays in the process that failed. Joining a process that raised
gives the error to the joiner, where it can be caught. Nothing else is
disturbed:

```dream
import std.console;
import std.error;

let main! = {
    let bad = spawn! $( raise! (error.new :oops "something broke") );
    let good = spawn! $( 6 * 7 );
    let outcome = try! { join! bad } catch e { error.kind e };
    console.print! (to_string [outcome, join! good])
};
```

`std.supervisor` builds on this: it watches processes, and starts again the
ones that fail.

Next: [Testing](09-testing.md).
