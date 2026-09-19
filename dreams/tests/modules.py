#!/usr/bin/env python3
"""Resolve imports into inline modules from files and packages."""
import argparse
import pathlib
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--dream', default='build-dream/bin/dream')
parser.add_argument('--compiler', default='build/dreams.dream')
args = parser.parse_args()
vm = str(pathlib.Path(args.dream).resolve())
compiler = str(pathlib.Path(args.compiler).resolve())

with tempfile.TemporaryDirectory(prefix='dream-modules-') as directory:
    temp = pathlib.Path(directory)
    (temp / 'bot.dr').write_text('''
mod dot {
    let name = "cot";
    priv let secret = "hidden";
    mod deep { let value = "deep"; }
}
''')

    def check(imports, expression, expected='cot\n', error=None):
        (temp / 'main.dr').write_text(
            f'import std.console; {imports}\n'
            f'let main! = console.print! ({expression});\n')
        result = subprocess.run(
            [vm, compiler, str(temp / 'main.dr'), '-o', str(temp / 'out.dream')],
            capture_output=True, text=True, timeout=60)
        if error:
            assert result.returncode != 0, imports
            assert error in result.stderr, result.stderr
            return
        assert result.returncode == 0, result.stderr
        for mode in (['--no-jit'], ['--jit-threshold', '1']):
            run = subprocess.run([vm, *mode, str(temp / 'out.dream')],
                                 capture_output=True, text=True, timeout=20)
            assert run.returncode == 0, run.stderr
            assert run.stdout == expected, (imports, run.stdout, expected)

    # Every case starts a fresh compiler, so no earlier import can load bot.
    for prefix in ('bot', 'sample.bot'):
        if prefix.startswith('sample.'):
            (temp / 'mind.toml').write_text(
                '[package]\nname = "sample"\nversion = "0.1.0"\nsrc = "."\n')
        check(f'import {prefix}.dot;', 'dot.name')
        check(f'import {prefix}.dot as d;', 'd.name')
        check(f'import {prefix}.dot.name;', 'name')
        check(f'import {prefix}.dot.name as n;', 'n')
        check(f'import {prefix}.dot.{{name as n}};', 'n')
        check(f'import {prefix}.dot.deep;', 'deep.value', 'deep\n')
        check(f'import {prefix}.dot.deep.value;', 'value', 'deep\n')
        check(f'import {prefix}.dot.secret;', '"unused"', error='is private')
        check(f'import {prefix}.dot.missing;', 'missing', error='no member')
        check(f'import {prefix}.dot.missing;', '"unused"', error='no member')
        check(f'import {prefix}.dot.name.{{x}};', '"unused"', error='cannot find module')
        check(f'import {prefix}.missing.child;', '"unused"', error='cannot find module')
    # The short file spelling must reach blocks stored under their package name.
    check('import bot.dot.name;', 'name')
    check('import bot; import sample.bot.dot;', 'dot.name')
    check('import bot.dot; import bot.dot.name;', 'name')
    print('module imports passed (interpreter and JIT)')

    # Behaviors use the same specialization for modules and collection records.
    behavior = """
mod Named {
    virtual let name self;
    virtual let label self = "hello " + name self;
    let twice self = label self + " / " + label self;
}
"""
    for kind in ('group', 'struct', 'mapping'):
        check(behavior + f'{kind} Person derive Named {{ name }}',
              'Person.twice (Person.make "Ada")', 'hello Ada / hello Ada\n')
        check(behavior + f'{kind} Person derive Named {{ name, label self = name self }}',
              'Person.twice (Person.new "Ada")', 'Ada / Ada\n')
        check(behavior + f'{kind} Person derive Named {{ age }}',
              '()', error='does not implement `name`')
        check(behavior + f'{kind} Person derive Named {{ name, label a b = "bad" }}',
              '()', error='implementation of `label` requires 1 parameter, found 2')
    check(behavior + 'mod Person { derive Named; let name self = "Ada"; }',
          'Person.label ()', 'hello Ada\n')
    check(behavior + 'mod Person { derive Named; let name = "Ada"; }',
          '()', error='implementation of `name` requires 1 parameter, found 0')
    check(behavior + 'mod Person { derive Named; priv let name self = "Ada"; }',
          '()', error='implementation of `name` must be public')
    check(behavior + 'mod Person { derive Named; let name! self = "Ada"; }',
          '()', error='does not implement `name`')
    check(behavior + 'mod Person { derive Named; let name self = raise! self; }',
          '()', error='impure')
    check('mod B { virtual let f a = a; } mod C { derive B; virtual let f a b; }',
          '()', error='requires 2 parameters, found 1')
    check('mod B { virtual let f a; } struct C derive B { f self = 1 } '
          'mod D { derive C; let f a b = 2; }',
          '()', error='requires 1 parameter, found 2')
    check('mod B { virtual let f! x; let run! x = f! x; } '
          'struct C derive B { f! x = 42 }', 'C.run! ()', '42\n')
    check('mod B { virtual let f x; let safe x = f x; } '
          'struct C derive B { f x = 7 }', 'C.safe (1 / 0)', '7\n')
    (temp / 'behavior.dr').write_text('virtual let name self; let hello self = name self;')
    check('struct Person derive behavior { name }', 'Person.hello (Person.make "Ada")', 'Ada\n')
    check('when false { struct C derive nonexistent { x } }', '42', '42\n')
    print('behavior contracts and records passed (interpreter and JIT)')
