#!/usr/bin/env python3
"""Optional types: the syntax, and that checking means the same thing
whether it happens at run time or under `comp`."""
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

with tempfile.TemporaryDirectory(prefix='dream-types-') as directory:
    temp = pathlib.Path(directory)
    count = 0

    def compile_source(source):
        path = temp / 'main.dr'
        path.write_text(source)
        return subprocess.run([vm, compiler, '-L', str(root / 'mind'), '-L', str(root),
                               str(path), '-o', str(temp / 'out.dream')],
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

    def failure(source, message):
        global count
        result = compile_source(source)
        assert result.returncode != 0, 'unexpected success: ' + source
        assert message in result.stderr, result.stderr
        count += 1

    prelude = 'import std.types; import std.console; \n'

    # --- the shape of the syntax ---------------------------------------------
    #
    # A primitive is the atom `type_of` answers with; a list, array or map
    # literal describes a list, array or map; `|` is a union; `->` is a
    # function; `where` refines.
    success(prelude + '''
type Min      = :integer -> :integer -> :integer;
type Ints     = [:integer];
type Pair a b = [a, b];
type Floats   = #[:float];
type Doc      = %{ :string => :integer };
type Conf     = %{ :host => :string, :port => :integer };
type Number   = :integer | :float;
type Byte     = :integer where fn n -> n >= 0 && n <= 255;
let main! = {
    console.print! [types.accepts Min (fn a b -> a + b), types.accepts Min 3]
    console.print! [types.accepts Ints [1, 2], types.accepts Ints [1, "x"], types.accepts Ints #[1]]
    console.print! [types.accepts (Pair :integer :string) [1, "x"],
                    types.accepts (Pair :integer :string) [1, 2],
                    types.accepts (Pair :integer :string) [1, "x", 3]]
    console.print! [types.accepts Floats #[1.0], types.accepts Floats #[1],
                    types.accepts Floats [1.0]]
    console.print! [types.accepts Doc %{ "a" => 1 }, types.accepts Doc %{ :a => 1 }]
    console.print! [types.accepts Conf %{ :host => "h", :port => 1, :extra => () },
                    types.accepts Conf %{ :host => "h" }]
    console.print! [types.accepts Number 1, types.accepts Number 1.5, types.accepts Number "x"]
    console.print! [types.accepts Byte 0, types.accepts Byte 255, types.accepts Byte 256]
};
''', '[true, false]\n[true, false, false]\n[true, false, false]\n'
     '[true, false, false]\n[true, false]\n[true, false]\n'
     '[true, true, false]\n[true, true, false]\n')

    # A tagged tuple reads as itself, which is what the union of them that
    # every result in this language is made of depends on. `:error` names a
    # kind *and* is the commonest tag there is, so it has to work as both.
    success(prelude + '''
type Outcome v = [:ok, v] | [:error, :string];
type Boxed     = :error;
let main! = {
    console.print! [types.accepts (Outcome :integer) [:ok, 5],
                    types.accepts (Outcome :integer) [:error, "no"],
                    types.accepts (Outcome :integer) [:ok, "x"],
                    types.accepts (Outcome :integer) [:error, 5],
                    types.accepts (Outcome :integer) [:nope, 5]]
    console.print! [types.accepts Boxed (error_new :bad ()), types.accepts Boxed 3]
    console.print! [types.accepts (types.literal :list) :list,
                    types.accepts (types.literal :list) [1, 2]]
};
''', '[true, true, false, false, false]\n[true, false]\n[true, false]\n')

    # --- parameters, recursion, locals, privacy ------------------------------
    (temp / 'contracts.dr').write_text('''
import std.types;
type Int  = :integer;
type List t = [t];
priv type Hidden = :string;
''')
    success(prelude + '''
import contracts.{Int, List};
type Positive = Int where fn n -> n > 0;
type Tree     = :unit | [:integer, Tree, Tree];
let checked = types.check Positive;
let baked      = comp types.check (List Int) [1, 2, 3];
let descriptor = comp (List Int);
let main! = {
    type Local = types.optional :string;
    console.print! [checked 3, baked, types.accepts descriptor [4, 5]]
    console.print! [types.accepts Local (), types.accepts Local "hi", types.accepts Local 3]
    console.print! [types.accepts Tree (), types.accepts Tree [1, (), ()],
                    types.accepts Tree [1, [2, (), ()], ()], types.accepts Tree [1, ()]]
    console.print! (try! { checked 0 } catch e { error_kind e })
    console.print! (try! { checked "no" } catch e { types.name_of (error_payload e) })
};
''', '[3, [1, 2, 3], true]\n[true, true, false]\n[true, true, true, false]\n'
     ':type_error\nPositive\n')

    # A local type may close over a runtime value; a type is a value, so this
    # is ordinary capture rather than anything types add.
    success(prelude + '''
let below limit value = {
    type Bounded = :integer where fn n -> n < limit;
    types.accepts Bounded value
};
let main! = console.print! [below 5 3, below 2 3];
''', '[true, false]\n')

    # --- arrows are enforced by applying, not by inspecting ------------------
    success(prelude + '''
type Sum = :integer -> :integer -> :integer;
let main! = {
    let add = types.enforce Sum (fn a b -> a + b);
    console.print! (add 1 2)
    console.print! (try! { add "x" 2 } catch e { error_kind e })
    console.print! (try! { (types.enforce Sum (fn a b -> "no")) 1 2 }
                    catch e { error_kind e })
};
''', '3\n:type_error\n:type_error\n')

    # --- what a check does not force -----------------------------------------
    # Note `types.list_of :any` rather than `[:any]`: the bracket means a
    # list type only inside a `type`, and this is an ordinary expression.
    success(prelude + '''
import std.list;
let main! = {
    console.print! (types.accepts :any (1 / 0))
    console.print! (types.accepts (types.one_of [:any, :integer]) (1 / 0))
    let unused = types.check :integer "never demanded";
    let xs = types.check (types.list_of :any) [1 / 0];
    console.print! (len xs)
    console.print! (list.take 2 (types.check_each :integer (list.from 1)))
    console.print! (try! { type_assert false :never (1 / 0) }
                    catch e { error_payload e })
};
''', 'true\ntrue\n1\n[1, 2]\n:never\n')

    # --- records carry a description ------------------------------------------
    success(prelude + '''
group Point { x : :integer, y : :integer = 0 }
struct Vector { x : :float, y : :float }
mapping Config { host : :string, retries : :integer = 3 }
group Untyped { anything }
group Empty {}
group Listy { xs : [:integer], tag : :ok | :no }
let origin = comp types.check Point.type (Point.new 1);
// Nothing the type checker can see through: the constructors themselves do
// not check at run time, and holding them to the fields' types at compile
// time is `static_types.py`'s business.
let opaque x = x;
let main! = {
    console.print! [origin, types.accepts Point.type [1], types.accepts Point.type [],
                    types.accepts Point.type [1, 2, 3], types.accepts Point.type ["x", 2]]
    console.print! [types.accepts Vector.type (Vector.make 1.0 2.0),
                    types.accepts Vector.type [1.0, 2.0], types.accepts Vector.type #[1.0]]
    console.print! [types.accepts Config.type %{ :host => "local" },
                    types.accepts Config.type %{ :host => "local", :retries => "bad" },
                    types.accepts Config.type (Config.new "local")]
    console.print! [types.accepts Untyped.type (Untyped.make (1 / 0)),
                    types.accepts Empty.type []]
    console.print! [types.accepts Listy.type (Listy.make [1, 2] :ok),
                    types.accepts Listy.type (Listy.make [1, 2] (opaque :maybe))]
    console.print! (Point.make (opaque "still untyped") (opaque false))
    console.print! (try! { types.check Point.type (Point.set_x origin (opaque "bad")) }
                    catch e { error_kind e })
};
''', '[[1, 0], true, false, false, false]\n[true, false, false]\n[true, false, true]\n'
     '[true, true]\n[true, false]\n["still untyped", false]\n:type_error\n')

    # --- a failed check under comp is a failed compile -------------------------
    failure(prelude + 'type Int = :integer; let main! = comp types.check Int "bad";',
            'compile-time expression could not be evaluated')
    failure(prelude + 'struct V { x : :integer } let main! = comp types.check V.type #["bad"];',
            'compile-time expression could not be evaluated')

    # --- diagnostics -----------------------------------------------------------
    failure('group P { x : Missing } let main! = P.type;', 'cannot find')
    failure('type = :integer;', 'expected a name after `type`')
    failure('type Bad! = :integer;', 'a type name must be pure')
    failure('type T = ;', 'expected a type')
    failure('type T = fn;', '`fn` is a keyword and cannot be a type')
    failure('type T = %{ :string => :integer, :atom => :integer };',
            'a map type describes all of its keys with one `=>`')
    failure('type T = %{ :host => :string, :string => :integer };',
            "a record type's keys are literals")
    failure('import contracts.{Hidden}; let main! = Hidden;', 'private')
    print(f'{count} optional type cases passed under interpreter and JIT')
