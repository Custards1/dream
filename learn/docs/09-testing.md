# Testing

A module's tests live in the module, in a `when test` block. An ordinary
build leaves them out entirely, and `mind test` builds them in and runs
them:

```dream no-run
import std.list;

let average xs = list.sum xs / list.length xs;

when test {
    import std.test;

    let tests = [
        test.case "the average of a few" $( test.eq! 4 (average [2, 4, 6]) ),
        test.case "one number is its own average" $( test.eq! 9 (average [9]) ),
    ];
}
```

`test.case name $( .. )` is one test. The `$( )` makes it a thunk, so it runs
when the runner gets to it and not before. The assertions are `test.eq!`
(expected first, then actual), `test.ne!`, `test.true!`, `test.false!` and
`test.near!` for floats. Each case runs in a process of its own, so one that
raises or never finishes is reported and the rest carry on.

```sh
mind test              # every test in the project, and its build script's checks
mind test greeter      # one package of a workspace
```

## Tests that try many inputs

`std.property` checks that something holds for many generated values, and
when it finds one that does not, shrinks it to the smallest that still
fails:

```dream no-run
import std.list;

when test {
    import std.property;
    import std.random;

    let ints = random.list_of (random.int (-1000) 1000);

    let tests = [
        property.check "reversing twice changes nothing" ints
            (fn xs -> list.reverse (list.reverse xs) == xs),
        property.check "sorting keeps the length" ints
            (fn xs -> list.length (list.sort xs) == list.length xs),
    ];
}
```

A property is a case like any other, so it goes in the same list. When one
fails, the report shows the smallest input that breaks it and the seed that
replays it: `DREAM_SEED=n mind test`.

## Examples are tests too

A `dream` code block in a `///` comment or a guide is compiled and run
when `mind docs` builds the site, and its output is shown beneath it, as
every example in this book was. `mind docs --strict-examples` fails the
build when one does not do what its fence says, so documentation that has
gone stale cannot be published by accident.

Next: [A small web server](10-a-web-server.md).
