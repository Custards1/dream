# Collections

## Lists

A list is the collection you will use most, and `std.list` is its module:
`list.map`, `list.filter`, `list.fold` and everything else you would expect.

```dream
import std.console;
import std.list;

let main! = {
    let scores = [72, 95, 58, 88, 100, 64];
    console.print! (to_string (list.filter (fn s -> s >= 70) scores))
    console.print! (to_string (list.map (fn s -> s / 10) scores))
    console.print! (to_string (list.sort scores))
    console.print! (to_string [list.sum scores, list.maximum scores, list.length scores])
    console.print! (to_string (list.take 3 (list.reverse (list.sort scores))))
};
```

`list.fold` is the one the others are made of. It walks a list with an
accumulator: `list.fold f start xs` calls `f acc x` for each element, and
each call's answer becomes the next `acc`.

```dream
import std.console;
import std.list;

let main! = {
    let words = ["lazy", "evaluation", "is", "fun"];
    let longest = list.fold (fn best w -> if len w > len best { w } else { best }) "" words;
    console.print! longest
};
```

## Maps

A map holds values by key. `m.[k]` reads one, `m.[k else d]` reads one with a
default for when it is missing, and `m.[k => v]` makes a new map with `k` set
to `v`:

```dream
import std.console;
import std.map;

let main! = {
    let stock = %{ "apples" => 12, "pears" => 4 };
    let restocked = stock.["pears" => 20].["plums" => 9];
    console.print! (to_string stock.["apples"])
    console.print! (to_string restocked.["kiwis" else 0])
    console.print! (to_string (map.keys restocked))
    console.print! (to_string (map.has "pears" stock))
};
```

`stock` is unchanged by making `restocked`: every collection in Dream is a
value, and "changing" one makes a new one that shares what it can with the
old.

Counting is the classic job for a map and a fold together:

```dream
import std.console;
import std.list;
import std.str;
import std.map;

let count_words text = list.fold (fn counts w -> counts.[w => counts.[w else 0] + 1]) %{} (str.words text);

let main! = {
    let counts = count_words "the cat and the hat and the bat";
    console.print! (to_string counts.["the"])
    console.print! (to_string (map.pairs counts))
};
```

## Strings

`std.str` covers what you would expect. Strings are UTF-8: `len` counts
bytes, and `str.length` counts characters.

```dream
import std.console;
import std.str;

let main! = {
    let line = "  Dream: lazy, typed, concurrent  ";
    console.print! (str.trim line)
    console.print! (to_string (str.split ", " (str.trim line)))
    console.print! (str.join_str " / " ["a", "b", "c"])
    console.print! (to_string [str.starts_with "Dr" "Dream", str.contains_str "lazy" line])
    console.print! (to_string [len "héllo", str.length "héllo"])
};
```

## Arrays

An array, `#[..]`, is a list's sibling that reads any position at once,
where a list walks to it. Use one when you index by position. `.[i]` reads
position `i` of either.

```dream
import std.console;
import std.array;

let main! = {
    let days = #["mon", "tue", "wed", "thu", "fri"];
    console.print! (days.[2] + " of " + to_string (len days))
    console.print! (to_string (array.to_list days))
};
```

Next: [Laziness](04-laziness.md).
