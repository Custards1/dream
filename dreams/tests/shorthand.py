#!/usr/bin/env python3
"""Type-directed standard members, ambiguity, fusion, and cache invalidation."""
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


def run(*cmd, **options):
    return subprocess.run(cmd, text=True, capture_output=True, timeout=120, **options)


with tempfile.TemporaryDirectory(prefix='dream-shorthand-') as directory:
    temp = pathlib.Path(directory)
    source = temp / 'main.dr'
    image = temp / 'main.dream'

    def compile_source(text, *flags):
        source.write_text(text)
        return run(vm, compiler, '-L', str(root / 'mind'), *flags,
                   '-o', str(image), str(source))

    fixture = (root / 'dream/tests/programs/shorthand.dr').read_text()
    expected = (root / 'dream/tests/programs/shorthand.expected').read_text()
    for flags in ((), ('--no-opt',), ('--units', str(temp / 'units')),
                  ('--units', str(temp / 'units'))):
        compiled = compile_source(fixture, *flags)
        assert compiled.returncode == 0, compiled.stdout + compiled.stderr
        for jit in ((), ('--no-jit',)):
            result = run(vm, *jit, str(image))
            assert result.returncode == 0 and result.stdout == expected, result.stdout + result.stderr

    for mode in ('--check', '--no-emit', '--ir', '--stats'):
        checked = compile_source(fixture, mode)
        assert checked.returncode == 0, checked.stdout + checked.stderr

    cases = [
        ('import std.list as seq; import std.array as vec; '
         'let main! = console.print! (map (fn x -> x + 1) #[2]);', '#[3]\n'),
        ('import std.list; let map f xs = :own; '
         'let main! = console.print! (map (fn x -> x) [2]);', ':own\n'),
        ('import std.list; let main! = { let map f xs = :local; '
         'console.print! (map (fn x -> x) [2]) };', ':local\n'),
        ('import std.list.{map}; import std.array; '
         'let main! = console.print! (map (fn x -> x + 1) [2]);', '[3]\n'),
        ('import std.list; import std.array; mod nested { '
         'let value = map (fn x -> x + 1) #[2]; } '
         'let main! = console.print! nested.value;', '#[3]\n'),
        ('let apply : (:integer -> Option :integer) -> Option :integer; '
         'let apply f = f 3; let main! = console.print! (apply some);', 'Option.some(3)\n'),
        ('let choice : (:integer -> :integer) -> Option :integer -> Option :integer; '
         'let choice = map; let main! = console.print! (choice (fn x -> x + 1) (some 2));', 'Option.some(3)\n'),
        ('import std.list; import std.array; let choice : (a -> b) -> [a] -> [b]; '
         'let choice f xs = map f xs; let main! = console.print! (choice (fn x -> x + 1) [2]);', '[3]\n'),
        ('let main! = console.print! (unwrap_or 0 (otherwise (some 3) none));', '3\n'),
        ('let main! = console.print! (map_error (fn x -> x + "!") (err "bad"));', 'Result.error("bad!")\n'),
        ('let main! = console.print! (ok_or "missing" none);', 'Result.error("missing")\n'),
        ('let choose x = and_then (fn _ -> some 3) x; '
         'let main! = console.print! (choose (some 2));', 'Option.some(3)\n'),
        ('let choose x = or_else (fn _ -> ok 3) x; '
         'let main! = console.print! (choose (err "bad"));', 'Result.ok(3)\n'),
        ('let fallback : (:unit -> Option :integer) -> Option :integer -> Option :integer; '
         'let fallback = or_else; let main! = console.print! (fallback (fn _ -> some 3) none);', 'Option.some(3)\n'),
        # A seed that is a literal decides the accumulator before the lambda
        # can (`[]` is `[:never]`); what the lambda answers must still choose.
        ('import std.list; import std.map; import std.str; '
         'let backwards : :string -> [:char]; '
         'let backwards s = fold (fn acc c -> list.cons c acc) [] (str.chars s); '
         'let main! = console.print! (str.of_chars (backwards "abc"));', 'cba\n'),
        ('import std.list; import std.map; '
         'let split : [:integer] -> [[:integer]]; '
         'let split xs = { let [odd, even] = fold (fn [o, e] x -> '
         'if x % 2 == 1 { [list.cons x o, e] } else { [o, list.cons x e] }) [[], []] xs; [odd, even] }; '
         'let main! = console.print! (split [1, 2, 3]);', '[[3, 1], [2]]\n'),
        ('import std.list; macro bump e = { '
         'let values : [:integer] = [e.[1]]; '
         'let changed = map (fn x -> x + 1) values; '
         '[:int, list.head changed, [0, 0]] }; '
         'let main! = console.print! (expand bump 2);', '3\n'),
        ('macro bump e = { let value : Result :integer :string = ok e.[1]; '
         'let changed = map (fn x -> x + 1) value |> unwrap_or 0; '
         '[:int, changed, [0, 0]] }; '
         'let main! = console.print! (expand bump 2);', '3\n'),
    ]
    for body, wanted in cases:
        compiled = compile_source('import std.console; ' + body)
        assert compiled.returncode == 0, compiled.stdout + compiled.stderr
        result = run(vm, str(image))
        assert result.returncode == 0 and result.stdout == wanted, result.stdout + result.stderr

    for body, name in [
        ('let ambiguous = map;', 'map'),
        ('let ambiguous x = flatten x;', 'flatten'),
        ('import std.list; import std.array; let ambiguous xs = fold (fn a b -> a + b) 0 xs;', 'fold'),
        ('let ambiguous = all [];', 'all'),
        ('let wrong = map (fn x -> x) 42;', 'map'),
    ]:
        compiled = compile_source(body)
        message = compiled.stdout + compiled.stderr
        assert compiled.returncode != 0 and f'overload of `{name}`' in message, message
        assert 'write the member explicitly:' in message, message

    for mode in ('--check', '--no-emit', '--ir'):
        rejected = compile_source('let ambiguous = map;', mode)
        assert rejected.returncode != 0 and 'overload of `map`' in rejected.stdout + rejected.stderr

    # A cached walk must not reuse a list choice after an argument becomes an
    # array, nor reuse a success after a signature stops disambiguating it.
    for container, wanted in [('[2]', '[3]\n'), ('#[2]', '#[3]\n')]:
        body = ('import std.console; import std.list; import std.array; '
                f'let main! = console.print! (map (fn x -> x + 1) {container});')
        cached = compile_source(body, '--units', str(temp / 'edits'))
        assert cached.returncode == 0, cached.stdout + cached.stderr
        before = image.read_bytes()
        fresh = compile_source(body)
        assert fresh.returncode == 0 and image.read_bytes() == before, fresh.stdout + fresh.stderr
        result = run(vm, str(image))
        assert result.returncode == 0 and result.stdout == wanted, result.stdout + result.stderr
    typed = 'let chosen : (a -> b) -> Option a -> Option b; let chosen = map;'
    assert compile_source(typed, '--units', str(temp / 'edits')).returncode == 0
    ambiguous = compile_source('let chosen = map;', '--units', str(temp / 'edits'))
    assert ambiguous.returncode != 0 and 'cannot choose an overload' in ambiguous.stdout + ambiguous.stderr

    repl = run(vm, compiler, '--repl', '-L', str(root / 'mind'), input=(
        'import std.list;\nimport std.array;\n'
        'map (fn x -> x + 1) [2]\nmap (fn x -> x + 1) #[2]\n'
        'let flatten_result : Result (Result a e) e -> Result a e;\n'
        'let flatten_result = flatten;\nflatten_result (ok (ok 5))\n:quit\n'))
    warning = 'dreams: warning: the REPL is deprecated; use source files with `just run FILE` or `mind run` instead.\n'
    assert repl.returncode == 0 and repl.stderr == warning, repl.stdout + repl.stderr
    for wanted in ('[3]\n', '#[3]\n', 'Result.ok(5)\n'):
        assert wanted in repl.stdout, repl.stdout
    alias = run(vm, compiler, '-i', '-L', str(root / 'mind'), input=':quit\n')
    assert alias.returncode == 0 and alias.stderr == warning, alias.stdout + alias.stderr

print('standard shorthand, higher-order selection, ambiguity, folds, cached edits, and REPL passed')
