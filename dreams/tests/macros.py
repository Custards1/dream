#!/usr/bin/env python3
"""Compile real macro/record programs and check both VM execution paths."""
import argparse
import os
import pathlib
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--dream', default='build-dream/bin/dream')
parser.add_argument('--compiler', default='build/dreams.dream')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parents[2]
vm = str(pathlib.Path(args.dream).resolve())
compiler = str(pathlib.Path(args.compiler).resolve())

with tempfile.TemporaryDirectory(prefix='dream-macros-') as directory:
    temp = pathlib.Path(directory)
    count = 0

    def compile_source(source):
        path = temp / 'main.dr'
        path.write_text(source)
        return subprocess.run(
            [vm, compiler, '-L', str(root / 'mind'), '-L', str(root),
             str(path), '-o', str(temp / 'out.dream')],
            text=True, capture_output=True, timeout=60)

    def success(source, expected, compile_output=None):
        global count
        result = compile_source(source)
        assert result.returncode == 0, result.stdout + result.stderr
        if compile_output is not None:
            assert result.stdout == compile_output, result.stdout
        for mode in (['--no-jit'], ['--jit-threshold', '1']):
            run = subprocess.run([vm, *mode, str(temp / 'out.dream')],
                                 capture_output=True, text=True, timeout=20)
            assert run.returncode == 0, run.stderr
            assert run.stdout == expected, (run.stdout, expected)
        count += 1

    def failure(source, message, location=None):
        global count
        result = compile_source(source)
        assert result.returncode != 0, 'unexpected success: ' + source
        assert message in result.stderr, result.stderr
        if location:
            assert location in result.stderr, result.stderr
        count += 1

    (temp / 'helpers.dr').write_text('''
import std.list;
macro twice e = [:binary, :add, e, e, [0, 0]];
macro reverse [kind, xs, sp] = [kind, list.reverse xs, sp];
macro identity e = e;
group Point { x, y }
struct Vector { x, y }
mapping Named { second, first }
''')
    success('''
import std.console;
import helpers as h;
import helpers.{reverse as backwards, identity};
let local x = expand h.twice (x + 1);
let main! = {
    console.print! (local 5)
    console.print! (expand backwards [1, 2, 3])
    console.print! (expand identity (expand h.twice 7))
    let p = h.Point.make 1 2;
    let set = h.Point.set_x p;
    console.print! [p, set 9, h.Point.y p]
    let v = h.Vector.make 3 4;
    console.print! [type_of v, h.Vector.set_y v 8, v]
};
''', '12\n[3, 2, 1]\n14\n[[1, 2], [9, 2], 2]\n[:array, #[3, 8], #[3, 4]]\n')

    success('''
import std.console;
macro drop e = [:int, 42, [0, 0]];
macro constant = [:int, 7, [0, 0]];
let main! = {
    console.print! (expand drop (expand unknown missing))
    console.print! (expand constant)
};
''', '42\n7\n')

    # Copies of a lambda must get independent binding and capture locations.
    success('''
import std.console;
macro twice e = [:binary, :add, e, e, [0, 0]];
macro bind e = [:block, [[:let, "temp", [], e, false, [0, 0]],
                       [:name, "temp", [0, 0]]], [0, 0]];
let use n = expand twice ((fn x -> x + n) 2);
let main! = {
    console.print! (use 5)
    console.print! (expand bind ((fn x -> x * 2) 9))
};
''', '14\n18\n')

    # A macro may call a helper whose own body was written with `expand`, and
    # that is what the snapshot the macro VM runs against has to be fresh
    # enough for. One snapshot serves the whole program, so by the time `use`
    # runs, the snapshot's `helper` is the `1 / 0` an unexpanded call is staged
    # as -- and the retry against a fresh one is the whole reason this answers
    # 6 instead of reporting that the macro failed.
    success("""
import std.console;
macro twice e = [:binary, :add, e, e, [0, 0]];
let helper n = expand twice n;
macro use e = [:int, helper 3, [0, 0]];
let main! = console.print! (expand use 1);
""", '6\n')

    # Unrelated comp! expressions run once in the final program's comp pass.
    success('''
import std.console;
let traced = comp! { console.print! "compile effect"; 6 };
macro number e = [:int, comp (2 + 3), [0, 0]];
let main! = console.print! [expand number ignored, expand number ignored, traced];
''', '[5, 5, 6]\n', compile_output='compile effect\n')

    success('''
import std.console;
when false { macro broken x = missing; group Hidden { x, y } }
group Empty {}
struct EmptyArray {}
group Lazy { first, second }
let main! = {
    console.print! [Empty.make (), EmptyArray.make ()]
    console.print! (Lazy.first (Lazy.make 7 (1 / 0)))
};
''', '[[], #[]]\n7\n')

    # Map records use field-name atoms, preserve constructor order and remain
    # ordinary persistent maps. Both constructors and updates keep values lazy.
    success('''
import std.console;
import helpers;
import helpers.Named as Pair;
mod nested { mapping Empty {} }
when true { mapping Lazy { first, second, } }
when false { mapping Hidden { unused } }
let mapping n = n + 1;
let main! = {
    let pair = Pair.make 20 10;
    let changed = Pair.set_first pair;
    console.print! [type_of pair, pair == %{ :first => 10, :second => 20 }]
    console.print! [Pair.first pair, Pair.second pair, Pair.first (changed 30), Pair.first pair]
    console.print! (match pair { %{ :first => x, :second => y } => x + y })
    console.print! (nested.Empty.make () == %{})
    console.print! (Lazy.first (Lazy.make 7 (1 / 0)))
    console.print! (Lazy.first (Lazy.set_second (Lazy.make 8 9) (1 / 0)))
    console.print! (Pair.second (Pair.set_first %{ :second => 4, :extra => 5 } 3))
    console.print! ((Pair.set_first %{ :extra => 5 } 3).[:extra])
    console.print! (mapping 4)
};
''', '[:map, true]\n[10, 20, 30, 10]\n30\ntrue\n7\n8\n4\n5\n5\n')

    failure('macro id x = x; let main! = expand id;', 'expects 1 syntax arguments')
    failure('let id x = x; let main! = expand id 1;', 'declared macro')
    failure('macro bad x = [:error, \"expected a literal list\"]; let main! = expand bad 1;',
            'expected a literal list')
    failure('macro bad x = 42; let main! = expand bad 1;', 'valid expression syntax tree')
    failure('macro bad x = [:binary, :bogus, x, x, [0, 0]]; let main! = expand bad 1;',
            'valid expression syntax tree')
    failure('macro bad x = [:lambda, 1, x, [0, 0]]; let main! = expand bad 1;',
            'valid expression syntax tree')
    failure('macro bad x = 1 / 0; let main! = expand bad 1;', 'macro evaluation failed')
    failure('macro bad x = [:name, "missing", [0, 0]];\nlet main! = expand bad 1;\n',
            'cannot find `missing`', 'main.dr:2:13:')
    failure('''macro loop x = [:expand, [:apply, [:name, "loop", [0, 0]], [x], [0, 0]], [0, 0]];
let main! = expand loop 1;
''', 'exceeded 64 nested calls')
    for declaration in ('group Bad { make }', 'group Bad { x, x }',
                        'struct Bad { x, set_x }', 'struct Bad { set_x, x }',
                        'mapping Bad { make }', 'mapping Bad { x, x }',
                        'mapping Bad { x, set_x }', 'mapping Bad { set_x, x }'):
        failure(declaration, 'conflicts with a generated helper')
    failure('group Bad { x y }', 'expected `,` or `}`')
    failure('group Bad { x!', 'plain name')
    failure('mapping Bad { x y }', 'expected `,` or `}`')
    failure('mapping Bad { x!', 'plain name')
    # The library returns actionable syntax errors, with no runtime imports
    # needed by its generated expressions. Effects remain checked at the call.
    for expression, message in (
        ('expand m.pipe 1 2', 'pipe expects a literal list'),
        ('expand m.and_all true', 'and_all expects a literal list'),
        ('expand m.or_any false', 'or_any expects a literal list'),
        ('expand m.cond [[true]] 0', 'cond expects [condition, expression] pairs'),
        ('expand m.if_some 1 () 2 3', 'binding name'),
        ('expand m.with_some 1 () 2', 'binding name'),
        ('expand m.with_ok [x] [:ok, 1] x', 'binding name'),
    ):
        failure('import std.macros as m; let main! = ' + expression + ';', message)
    for expression in ('expand m.assert true "fine"', 'expand m.attempt 1'):
        failure('import std.macros as m; let pure () = ' + expression + ';', 'impure')
    failure('import std.macros as m; let main! = expand m.if_some x () x x;',
            'cannot find `x`')
    success('import std.console; import std.macros.{pipe as through, with_some}; '
            'let double n = n * 2; '
            'let main! = console.print! (expand through (expand with_some x 3 (x + 1)) [double]);',
            '8\n')

    # Embedded macro VMs must preserve the REPL's already-open input handle.
    session = subprocess.run(
        [vm, compiler, '--repl', '-L', str(root / 'mind'), '-L', str(root)],
        input='macro twice e = [:binary, :add, e, e, [0, 0]];\n'
              'expand twice 21\ngroup Point { x, y }\n'
              'Point.set_x (Point.make 1 2) 9\nmapping Named { x }\n'
              'Named.x (Named.set_x (Named.make 1) 73)\n:quit\n',
        env={**os.environ, 'DREAM': vm}, capture_output=True, text=True, timeout=30)
    assert session.returncode == 0, session.stderr
    assert '42' in session.stdout and '[9, 2]' in session.stdout, session.stdout
    assert '73' in session.stdout, session.stdout
    assert 'error:' not in session.stderr, session.stderr
    count += 1
    print(f'{count} macro/record cases passed (interpreter and JIT)')
