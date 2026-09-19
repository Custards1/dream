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
