#!/usr/bin/env python3
"""Pitfall diagnostics remain warnings, have source locations, and avoid guesses."""
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
root = pathlib.Path(__file__).resolve().parents[2]

with tempfile.TemporaryDirectory(prefix='dream-warnings-') as directory:
    source = pathlib.Path(directory) / 'pitfalls.dr'
    image = pathlib.Path(directory) / 'pitfalls.dream'

    def check(body, *messages):
        source.write_text(body + '\n')
        result = subprocess.run([vm, compiler, '-L', str(root / 'mind'),
                                 str(source), '--no-emit'],
                                text=True, capture_output=True, timeout=120)
        assert result.returncode == 0, result.stderr
        assert result.stderr.count('warning:') == len(messages), result.stderr
        for message in messages:
            assert message in result.stderr, result.stderr
        if messages:
            assert 'pitfalls.dr:1:' in result.stderr, result.stderr
            assert '    = ' in result.stderr, result.stderr

    check('let f = %{ :a => 1, :a => 2, :a => 3 };',
          *(['duplicate literal map key'] * 2))
    check('let f = %{ "x" => 1, "x" => 2 };', 'duplicate literal map key')
    check('let f x = %{ x => 1, :x => 2, "x" => 3 };')
    check('let f = [%{ :a => 1 }, %{ :a => 2 }];')
    check('let f x = match x { _ => 1, :a => 2, :b => 3 };',
          *(['unreachable match arm'] * 2))
    check('let f x = match x { y => y, :a => :b };', 'unreachable match arm')
    check('let f x = match x { :a => 1, :a => 2, _ => 3 };', 'unreachable match arm')
    check('let f x = match x { :a => 1, :a if false => 2, _ => 3 };',
          'unreachable match arm')
    check('let f x = match x { _ if false => 1, :a if false => 2, :a => 3, _ => 4 };')
    check('let f x = match x { [a] => a, [b] => b, _ => 0 };')
    check('let f x = [x / 0, x % 0, x / -0.0];',
          'division by a literal zero', 'remainder by a literal zero',
          'division by a literal zero')
    check('let f x = [x / 1, x % 2, 0 / x];')
    check('let f x = { let g y = y / 0; g x };', 'division by a literal zero')

    # Warnings must not prevent emitting or running the program.
    source.write_text('import std.console;\n'
                      'let main! = console.print! (%{ :a => 1, :a => 2 }).[:a];\n')
    result = subprocess.run([vm, compiler, '-L', str(root / 'mind'), str(source),
                             '-o', str(image)], text=True, capture_output=True, timeout=120)
    assert result.returncode == 0, result.stderr
    assert 'duplicate literal map key' in result.stderr, result.stderr
    result = subprocess.run([vm, str(image)], text=True, capture_output=True, timeout=120)
    assert result.returncode == 0, result.stderr
    assert result.stdout == '2\n', result.stdout

print('compiler warning tests passed')
