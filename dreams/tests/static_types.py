#!/usr/bin/env python3
"""Signatures, the compile-time checker, and discriminated unions.

What is held here: that a signature is checked where it is written and at
every use, that code no signature touches is never rejected, that a `match`
on a declared union must handle every variant, and that none of it changes
the executable code a program compiles to."""
import argparse
import pathlib
import subprocess
import struct
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--dream', default='build-dream/bin/dream')
parser.add_argument('--compiler', default='build/dreams.dream')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parents[2]
vm = str(pathlib.Path(args.dream).resolve())
compiler = str(pathlib.Path(args.compiler).resolve())

with tempfile.TemporaryDirectory(prefix='dream-static-') as directory:
    temp = pathlib.Path(directory)
    count = 0

    def compile_source(source, *extra, out='out.dream'):
        path = temp / 'main.dr'
        path.write_text(source)
        return subprocess.run([vm, compiler, '-L', str(root / 'mind'), '-L', str(root),
                               str(path), '-o', str(temp / out), *extra],
                              text=True, capture_output=True, timeout=60)

    def success(source, expected):
        global count
        result = compile_source(source)
        assert result.returncode == 0, result.stdout + result.stderr
        for mode in (['--no-jit'], ['--jit-threshold', '1']):
            run = subprocess.run([vm, *mode, str(temp / 'out.dream')],
                                 text=True, capture_output=True, timeout=30)
            assert run.returncode == 0, run.stderr
            assert run.stdout == expected, (run.stdout, expected)
        count += 1

    def failure(source, *messages):
        global count
        result = compile_source(source)
        assert result.returncode != 0, 'unexpected success: ' + source
        for message in messages:
            assert message in result.stderr, (message, result.stderr)
        count += 1

    prelude = 'import std.console;  import std.types;\n'

    # --- signatures ------------------------------------------------------------
    #
    # The three spellings, a generic function, and a local signature. Every
    # one of these compiles to nothing, so the output is the program's own.
    success(prelude + '''
let add : :integer -> :integer -> :integer;
let add x y = x + y;
let limit : :integer = 10;
let greet who : :string = "hi " + who;

let map : (a -> b) -> [a] -> [b];
let map f xs = match xs {
    [] => [],
    [x, ..rest] => list_cons (f x) (map f rest),
};

let lengths : [:string] -> [:integer];
let lengths xs = map (fn s -> len s) xs;

let main! = {
    let twice : :integer -> :integer;
    let twice n = n * 2;
    console.print! [add limit 2, greet "ada", twice 21]
    console.print! (lengths ["a", "bcd"])
    console.print! (map (fn n -> n + 1) [1, 2, 3])
};
''', '[12, "hi ada", 42]\n[1, 3]\n[2, 3, 4]\n')

    # A lambda is checked against the parameter it is passed to, and learns
    # its own parameter types from it -- including from arguments written
    # after it.
    failure(prelude + '''
let apply : (:integer -> :string) -> :integer -> :string;
let apply f n = f n;
let main! = console.print! (apply (fn n -> n + 1) 3);
''', 'what this function answers should be `:string`, but this is `:integer`')

    # --- what is reported --------------------------------------------------------
    failure(prelude + '''
let add : :integer -> :integer -> :integer;
let add x y = x + y;
let main! = console.print! (add "one" 2);
''', 'argument 1 of `add` should be `:integer`, but this is `"one"`')
    failure(prelude + '''
let add : :integer -> :integer -> :integer;
let add x y = x + y;
let main! = console.print! (add 1 2 3);
''', '`add` takes 2 arguments, but it is given 3')
    failure(prelude + 'let name : :string = 5; let main! = console.print! name;',
            '`name` should be `:string`, but this is `5`')
    failure(prelude + '''
let describe n : :string = if n > 0 { "positive" } else { n > 5 };
let main! = console.print! (describe 1);
''', 'should be `:string`, but this is `:bool`')
    failure(prelude + '''
let map : (a -> b) -> [a] -> [b];
let map f xs = match xs { [] => [], [x, ..rest] => list_cons (f x) (map f rest) };
let wrong : [:integer] -> [:string];
let wrong xs = map (fn n -> n + 1) xs;
let main! = console.print! (wrong [1]);
''', 'should be `[:string]`, but this is `[:integer]`')
    failure(prelude + '''
let id : a -> a;
let id x = 5;
let main! = console.print! (id 1);
''', 'should be `a`, but this is `5`')
    failure(prelude + 'let size : :string; let main! = console.print! 1;',
            'the signature of `size` has no definition')
    failure(prelude + 'let f : :integer; let f : :integer; let f = 1; let main! = console.print! f;',
            '`f` already has a signature')
    failure(prelude + 'let main! = { let n : :integer; console.print! 1 };',
            'the signature of `n` has no `let` after it in this block')
    failure(prelude + 'let f : Missing -> :integer; let f x = 1; let main! = console.print! (f 1);',
            'cannot find the type `Missing`')
    failure(prelude + 'let f : :integer; let f x = x; let main! = console.print! (f 1);',
            'this function takes 1 parameter, but it is expected to be `:integer`')
    failure(prelude + 'let f x : :integer; let main! = console.print! 1;',
            'a signature is the type of the whole of `f`, so it names no parameters')
    failure(prelude + '''
let name : :string = "ada";
let main! = console.print! (name + 1);
''', '`+` adds two numbers, joins two strings or joins two lists')

    # --- the standard library's own signatures ----------------------------------
    #
    # `std.list` says what its functions take, so a call of one is checked
    # without the program annotating anything -- and a lambda handed to one
    # learns its parameter's type from the list it will be given.
    failure(prelude + 'import std.list; let main! = console.print! (list.map 5 [1]);',
            'argument 1 of `list.map` should be `:integer -> b`, but this is `5`')
    failure(prelude + '''
import std.list;
let words : [:string] = ["a", "b"];
let main! = console.print! (list.map (fn w -> w * 2) words);
''', 'the left of `*` should be a number, but this is `:string`')
    success(prelude + '''
import std.list;
let main! = {
    console.print! (list.fold (fn acc x -> acc + [x * 2]) [] [1, 2, 3])
    console.print! (list.filter (fn x -> x > 1) (list.range 0 4))
};
''', '[2, 4, 6]\n[2, 3]\n')

    # --- what is not ----------------------------------------------------------------
    #
    # Code no signature touches is never an error, however obviously it would
    # raise: `try!` exists to catch exactly these, and a line nobody forces
    # never raises at all. So is anything the checker cannot see into -- an
    # unannotated function answers `:any`, and `:any` fits everywhere.
    success(prelude + '''
let add : :integer -> :integer -> :integer;
let add x y = x + y;
let opaque x = x;
let main! = {
    console.print! (error_kind (try! { 1 + "x" } catch e { e }))
    console.print! (error_kind (try! { (1) 2 } catch e { e }))
    console.print! (error_kind (try! { add (opaque "one") 2 } catch e { e }))
};
''', ':type_error\n:not_a_function\n:type_error\n')

    # `--no-types` skips checking and signature hints. Executable sections
    # stay identical; the typed image additionally records integer parameters.
    typed = prelude + '''
let add : :integer -> :integer -> :integer;
let add x y = x + y;
let main! = console.print! (add 1 2);
'''
    assert compile_source(typed, out='a.dream').returncode == 0
    assert compile_source(typed, '--no-types', out='b.dream').returncode == 0
    def sections(path):
        data = path.read_bytes()
        count, = struct.unpack_from('<I', data, 28)
        result = {}
        for i in range(count):
            kind, offset, length, entries = struct.unpack_from('<4sIII', data, 32 + i * 16)
            result[kind] = (entries, data[offset:offset + length])
        return result

    with_types = sections(temp / 'a.dream')
    without_types = sections(temp / 'b.dream')
    entries, hints = with_types.pop(b'ITYP')
    records = list(struct.iter_unpack('<IIQ', hints))
    assert len(records) == entries and all(reserved == 0 for _, reserved, _ in records)

    def function_name(fi):
        name_index, = struct.unpack_from('<I', with_types[b'FUNC'][1], fi * 32)
        offset, length = struct.unpack_from('<II', with_types[b'KSTR'][1], name_index * 8)
        return with_types[b'SBLB'][1][offset:offset + length]

    assert any(function_name(fi) == b'add' and mask == 3 for fi, _, mask in records)
    assert b'ITYP' not in without_types
    assert with_types == without_types
    broken = prelude + 'let n : :string = 5; let main! = console.print! n;'
    assert compile_source(broken, '--no-types').returncode == 0
    count += 2

    # --- records with typed fields -----------------------------------------------
    failure(prelude + '''
group Point { x : :integer, y : :integer = 0 }
let main! = console.print! (Point.make 1 "two");
''', 'argument 2 of `Point.make` should be `:integer`, but this is `"two"`')
    failure(prelude + '''
mapping Config { host : :string, retries : :integer = 3 }
let port : :string = Config.retries (Config.new "local");
let main! = console.print! port;
''', '`port` should be `:string`, but this is `:integer`')
    # A defaulted field may be missing, and one nobody annotated promises
    # nothing at all.
    success(prelude + '''
mapping Config { host : :string, retries : :integer = 3 }
mapping Loose { name, greeting = "Hi" }
let main! = console.print! [Config.retries %{}, Loose.greeting %{}];
''', '[3, "Hi"]\n')

    # --- unions -----------------------------------------------------------------------
    shapes = '''
union Shape {
    circle(radius : :float)
    rect(w : :float, h : :float)
    empty

    area self = match self {
        [:circle, r] => 3.0 * r * r,
        [:rect, w, h] => w * h,
        :empty => 0.0,
    }
}
'''
    # A variant is the tagged list programs already write, and one with no
    # fields is its atom. The union's own name is its description, and the
    # module of constructors beside it.
    success(prelude + shapes + '''
union Option a { some(value : a), none }
let main! = {
    console.print! [Shape.circle 1.0, Shape.rect 2.0 3.0, Shape.empty]
    console.print! [Shape.area (Shape.circle 1.0), Shape.area (Shape.rect 2.0 3.0), Shape.area Shape.empty]
    console.print! [types.accepts Shape [:rect, 1.0, 2.0], types.accepts Shape [:rect, 1],
                    types.accepts Shape :empty, types.accepts Shape :full]
    console.print! [Option.some 3, Option.none, types.accepts (Option :integer) [:some, 1],
                    types.accepts (Option :integer) [:some, "x"]]
};
''', '[[:circle, 1], [:rect, 2, 3], :empty]\n[3, 6, 0]\n[true, false, true, false]\n'
     '[[:some, 3], :none, true, false]\n')

    # The check that makes declaring one worth it.
    failure(prelude + shapes + '''
let perimeter : Shape -> :float;
let perimeter s = match s {
    [:circle, r] => 6.0 * r,
    [:rect, w, h] => 2.0 * (w + h),
};
let main! = console.print! (perimeter Shape.empty);
''', 'this `match` on `Shape` does not handle `:empty`')
    failure(prelude + shapes + '''
let label : Shape -> :string;
let label s = match s { :empty => "nothing" };
let main! = console.print! (label Shape.empty);
''', '`[:circle, _]` or `[:rect, _, _]`')
    # A wildcard, a binder, or a guard all count as handling.
    success(prelude + shapes + '''
let label : Shape -> :string;
let label s = match s { :empty => "nothing", _ => "something" };
let big : Shape -> :bool;
let big s = match s {
    [:circle, r] if r > 10.0 => true,
    [:circle, _] => false,
    [:rect, _, _] => false,
    other => false,
};
let main! = console.print! [label Shape.empty, label (Shape.circle 1.0), big (Shape.circle 20.0)];
''', '["nothing", "something", true]\n')
    # A pattern takes the variant it matched apart: `r` is the radius.
    failure(prelude + shapes + '''
let bad : Shape -> :string;
let bad s = match s { [:circle, r] => r + "cm", _ => "" };
let main! = console.print! (bad Shape.empty);
''', '`+` adds two numbers')
    failure(prelude + shapes + 'let main! = console.print! (Shape.circle 2);',
            'argument 1 of `Shape.circle` should be `:float`, but this is `2`')
    failure(prelude + shapes + 'let s : Shape = [:square, 1.0]; let main! = console.print! s;',
            '`s` should be `Shape`')

    # The idiom every result in the language already uses, declared: the
    # variants are `[:ok, v]` and `[:error, e]` exactly as written by hand, so
    # old code and new agree about them.
    success(prelude + '''
union Outcome v { ok(value : v), error(reason : :string) }
let parse : :string -> Outcome :integer;
let parse s = if s == "1" { Outcome.ok 1 } else { [:error, "not one: " + s] };
let show : Outcome :integer -> :string;
let show r = match r { [:ok, n] => to_string (n + 1), [:error, why] => why };
let main! = console.print! [show (parse "1"), show (parse "2")];
''', '["2", "not one: 2"]\n')

    failure('union U {}', 'has no variants')
    failure('union U { a, a }', '`a` is already a variant or a member of `U`')
    failure('union U { type }', '`type` is a keyword and cannot name a variant or a member')
    failure('union U { a(x, x) }', '`x` is already a field of this variant')

    print(f'{count} static type cases passed under interpreter and JIT')
