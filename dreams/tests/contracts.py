#!/usr/bin/env python3
"""Compile-time contracts: a refinement run against a value the compiler has.

What is held here: that a `where` in a named type is run at compile time
against every literal argument, every local bound to one and every `comp`
result that meets it, and that a value it rejects is reported where it was
written; that the structure decides which predicates are asked -- a union is
accepted by any member, a list asks each element; that a `comp` global is
checked as the value it produced at every use; that a predicate which raises
is attributed to its own call and no other; and that none of it changes the
image a program compiles to."""
import argparse
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

with tempfile.TemporaryDirectory(prefix='dream-contracts-') as directory:
    temp = pathlib.Path(directory)
    count = 0

    def compile_source(source, *extra, out='out.dream', files=None):
        for name, text in (files or {}).items():
            (temp / name).write_text(text)
        path = temp / 'main.dr'
        path.write_text(source)
        return subprocess.run([vm, compiler, '-L', str(root / 'mind'), '-L', str(temp),
                               str(path), '-o', str(temp / out), *extra],
                              text=True, capture_output=True, timeout=60)

    def success(source, expected, files=None):
        global count
        result = compile_source(source, files=files)
        assert result.returncode == 0, result.stdout + result.stderr
        for mode in (['--no-jit'], ['--jit-threshold', '1']):
            run = subprocess.run([vm, *mode, str(temp / 'out.dream')],
                                 text=True, capture_output=True, timeout=30)
            assert run.returncode == 0, run.stderr
            assert run.stdout == expected, (run.stdout, expected)
        count += 1

    def failure(source, *messages, absent=(), files=None):
        global count
        result = compile_source(source, files=files)
        assert result.returncode != 0, 'unexpected success: ' + source
        for message in messages:
            assert message in result.stderr, (message, result.stderr)
        for message in absent:
            assert message not in result.stderr, (message, result.stderr)
        count += 1

    prelude = '''import std.console;
import std.list;
type Port = :integer where fn n -> n >= 1 && n <= 65535;
let connect : Port -> :string;
let connect p = "port " + to_string p;
'''
    rejected = 'is not: a `where` it has to satisfy answered `false` at compile time'

    # --- what is accepted ---------------------------------------------------------
    #
    # Every value here satisfies its refinement, so the program compiles and
    # runs as it always did. The union cases are the ones that show the
    # structure is read: `()` meets `Port | :unit` without the predicate being
    # asked, and a tag picks the one variant whose predicate matters.
    success(prelude + '''
type Name = :string where fn s -> len s > 0;
type Entry = [:port, Port] | [:name, Name] | :none;
let maybe : (Port | :unit) -> :string;
let maybe p = to_string p;
let describe : Entry -> :string;
let describe e = match e { [:port, p] => connect p, [:name, n] => n, :none => "none" };
let count : [Port] -> :integer;
let count ps = len ps;
let ports = comp list.map (fn n -> n * 1000) [1, 2, 3];
let main! = {
    let p = 443;
    console.print! (connect 8080)
    console.print! (connect p)
    console.print! (maybe ())
    console.print! (describe [:port, 22])
    console.print! (describe [:name, "ada"])
    console.print! (describe :none)
    console.print! (count ports)
    console.print! (count [1, 65535])
};
''', 'port 8080\nport 443\n()\nport 22\nada\nnone\n3\n2\n')

    # --- what is rejected, and where ------------------------------------------------
    failure(prelude + 'let main! = console.print! (connect 70000);',
            'main.dr:6:37: error: argument 1 of `connect` should be `Port`, but `70000` ' + rejected)
    failure(prelude + 'let main! = console.print! (connect (-3));',
            'argument 1 of `connect` should be `Port`, but `-3` ' + rejected)
    # Through a local: its type is the value it was bound to.
    failure(prelude + 'let main! = { let p = 99999; console.print! (connect p) };',
            'but `99999` ' + rejected)
    # A value binding with a signature is held to it where it is defined.
    failure(prelude + 'let limit : Port = 0; let main! = console.print! limit;',
            '`limit` should be `Port`, but `0` ' + rejected)
    # A list asks each element, and a union asks only the member that fits.
    failure(prelude + '''
let count : [Port] -> :integer;
let count ps = len ps;
let main! = console.print! (count [1, 2, 0]);
''', 'should be `[Port]`, but `[1, 2, 0]` ' + rejected)
    failure(prelude + '''
type Entry = [:port, Port] | :none;
let describe : Entry -> :string;
let describe e = "x";
let main! = console.print! (describe [:port, 0]);
''', 'should be `Entry`, but `[:port, 0]` ' + rejected)
    # A record's field, by key.
    failure(prelude + '''
type Config = %{ :host => :string, :port => Port };
let configure : Config -> :string;
let configure c = c.[:host];
let main! = console.print! (configure %{ :host => "h", :port => 0 });
''', 'but `%{:host => "h", :port => 0}` ' + rejected)
    # A recursive type is followed as far as the value goes.
    failure(prelude + '''
type Tree = :leaf | [:node, Port, Tree, Tree];
let size : Tree -> :integer;
let size t = 1;
let main! = console.print! (size [:node, 1, [:node, 2, :leaf, :leaf], [:node, 0, :leaf, :leaf]]);
''', rejected)
    # A `where` written on a record's own field is part of `Name.type`.
    failure('''import std.console;
mapping Config { host : :string, port : :integer where fn n -> n > 0 }
let configure : Config.type -> :string;
let configure c = "ok";
let main! = console.print! (configure %{ :host => "h", :port => 0 });
''', rejected)

    # Several in one program are one compile-time run, and each is reported at
    # its own call, in the order they were written.
    result = compile_source(prelude + 'let main! = {\n'
                            + ''.join('    console.print! (connect %d)\n' % (70000 + i) for i in range(40))
                            + '};\n')
    assert result.returncode != 0
    lines = [l for l in result.stderr.splitlines() if 'error' in l]
    assert len(lines) == 40, result.stderr
    assert all(('`%d`' % (70000 + i)) in lines[i] for i in range(40)), result.stderr
    count += 1

    # --- compile-time values ---------------------------------------------------------
    #
    # A `comp` is checked as what it produced, at the binding and at every use
    # of a global that is one.
    failure(prelude + 'let limit : Port = comp (70 * 1000); let main! = console.print! limit;',
            '`limit` should be `Port`, but `70000` ' + rejected)
    failure(prelude + '''
let big = comp list.map (fn n -> n * 100000) [1, 2, 3];
let count : [Port] -> :integer;
let count ps = len ps;
let main! = console.print! (count big);
''', 'but `[100000, 200000, 300000]` ' + rejected)
    # The structure is checked too, without a refinement in sight.
    failure(prelude + 'let word = comp "eighty"; let main! = console.print! (connect word);',
            'argument 1 of `connect` should be `Port`, but this is `"eighty"`')
    failure(prelude + 'let n : :string = comp (1 + 2); let main! = console.print! n;',
            '`n` should be `:string`, but this `comp` produced `3`')
    # And `comp!`, which runs on the same VM with effects allowed.
    failure(prelude + '''
import std.os;
let port : Port = comp! (len (os.args! ()) - 100);
let main! = console.print! port;
''', '`port` should be `Port`')
    # A `comp` that fails is reported where it is written, not nowhere.
    failure(prelude + 'let boom = comp (1 / 0); let main! = console.print! boom;',
            'main.dr:6:12: error: this compile-time expression could not be evaluated')

    # --- predicates that misbehave ---------------------------------------------------
    #
    # One that raises is that call's failure and no other's: the second call is
    # still answered, and says its own thing.
    failure('''import std.console;
type Odd = :integer where fn n -> 10 / (n - 5) > 0;
let f : Odd -> :integer;
let f n = n;
let main! = { console.print! (f 5) console.print! (f 1) };
''', 'but `5` is not: checking it raised at compile time',
         'but `1` ' + rejected)
    failure('''import std.console;
type Weird = :integer where fn n -> n;
let f : Weird -> :integer;
let f n = n;
let main! = console.print! (f 7);
''', 'answered `7` at compile time, which is not `true`')

    # --- across modules --------------------------------------------------------------
    #
    # A library's refinement guards its callers: this is the compile-time API.
    net = '''
type Port = :integer where fn n -> n >= 1 && n <= 65535;
let connect : Port -> :string;
let connect p = "port " + to_string p;
'''
    failure('import std.console; import net;\nlet main! = console.print! (net.connect 0);',
            'argument 1 of `net.connect` should be `Port`, but `0` ' + rejected,
            files={'net.dr': net})
    success('import std.console; import net;\nlet main! = console.print! (net.connect 80);',
            'port 80\n', files={'net.dr': net})

    # --- what is not a contract -------------------------------------------------------
    #
    # A value the compiler does not have is left to run time, as it always was.
    success(prelude + '''
import std.os;
let main! = console.print! (try! { connect (len (os.args! ()) - 5) } catch e { "caught" });
''', 'port -5\n')

    # --- the image ------------------------------------------------------------------------
    #
    # Contracts are run on a copy of the program and thrown away: an image with
    # contracts that hold is byte-identical to the same program compiled with
    # the types switched off. (A string, because a signature over integers or
    # floats does add something to an image -- the JIT's parameter hints.)
    source = '''import std.console;
type Name = :string where fn s -> len s > 0;
let greet : Name -> :string;
let greet n = "hi " + n;
let main! = { console.print! (greet "ada") console.print! (greet "bo") };
'''
    assert compile_source(source, out='typed.dream').returncode == 0
    assert compile_source(source, '--no-types', out='untyped.dream').returncode == 0
    assert (temp / 'typed.dream').read_bytes() == (temp / 'untyped.dream').read_bytes()
    count += 1

    print(f'{count} compile-time contract cases passed under interpreter and JIT')
