#!/usr/bin/env python3
"""Type-test bytecode, inline JIT checks, and parity with unfused code."""
import argparse
import pathlib
import struct
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--dream', default='build-dream/bin/dream')
parser.add_argument('--compiler', default='build/dreams.dream')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parents[2]
vm = str(pathlib.Path(args.dream).resolve())
compiler = str(pathlib.Path(args.compiler).resolve())
kinds = ['integer', 'float', 'char', 'bool', 'unit', 'string', 'atom', 'list', 'array', 'map']
values = ['0', '1.0', "'x'", 'true', '()', '"s"', ':tag', '[]', '#[]', '%{}',
          '[1 / 0]', '#[1 / 0]', '%{ :x => 1 / 0 }', '%{ :a => 1, :b => 2 }',
          '(fn x -> x)', '(4611686018427387903 + 1)']


def run(*cmd):
    result = subprocess.run(cmd, text=True, capture_output=True, timeout=120)
    assert result.returncode == 0, result.stdout + result.stderr
    return result.stdout


def nodes(path):
    data = path.read_bytes()
    count, = struct.unpack_from('<I', data, 28)
    for i in range(count):
        kind, offset, length, _ = struct.unpack_from('<4sIII', data, 32 + i * 16)
        if kind == b'NODE':
            return list(struct.iter_unpack('<BBHIII', data[offset:offset + length]))
    raise AssertionError('missing NODE')


with tempfile.TemporaryDirectory(prefix='dream-type-codegen-') as directory:
    temp = pathlib.Path(directory)
    source = ['import std.console;']
    for kind in kinds:
        source += [f'let is_{kind} x = type_of x == :{kind};',
                   f'let not_{kind} x = :{kind} != type_of x;']
    source += ['let main! = {']
    for kind in kinds:
        for prefix in ('is', 'not'):
            source += ['console.print! [' + ', '.join(f'{prefix}_{kind} {v}' for v in values) + ']']
    source += ['};']
    src = temp / 'matrix.dr'
    src.write_text('\n'.join(source))
    optimized, plain = temp / 'opt.dream', temp / 'plain.dream'
    for image, options in ((optimized, []), (plain, ['--no-opt'])):
        run(vm, compiler, '-L', str(root / 'mind'), str(src), '-o', str(image), *options)
    fused = [n for n in nodes(optimized) if n[0] == 43]
    assert {(n[4], n[5]) for n in fused} == {(i, inv) for i in range(10) for inv in (0, 1)}
    assert not any(n[0] == 43 for n in nodes(plain))
    assert len(nodes(optimized)) < len(nodes(plain))
    expected = run(vm, '--no-jit', str(plain))
    assert run(vm, '--no-jit', str(optimized)) == expected
    assert run(vm, '--jit-threshold', '1', str(optimized)) == expected
    has_jit = True
    for kind in kinds:
        result = subprocess.run([vm, '--dump-jit', 'is_' + kind, str(optimized)],
                                text=True, capture_output=True, timeout=30)
        if 'this build has no JIT' in result.stderr:
            has_jit = False
            break
        assert result.returncode == 0, result.stderr
        ir = result.stdout
        assert 'define ' in ir, ir
        assert 'call i32 @dream_rt_builtin' not in ir, ir
        assert 'call i32 @dream_rt_compare' not in ir, ir
    # Pattern tests, gradual callers, laziness and errors also match --no-opt.
    regression = root / 'dream/tests/programs/type_tests.dr'
    run(vm, compiler, '-L', str(root / 'mind'), str(regression), '-o', str(plain), '--no-opt')
    expected = regression.with_suffix('.expected').read_text()
    assert run(vm, '--no-jit', str(plain)) == expected
    run(vm, compiler, '-L', str(root / 'mind'), str(regression), '-o', str(optimized))
    assert run(vm, '--no-jit', str(optimized)) == expected
    assert run(vm, '--jit-threshold', '1', str(optimized)) == expected
    if has_jit:
        for name in regression.with_suffix('.jit').read_text().splitlines():
            ir = run(vm, '--dump-jit', name, str(optimized))
            assert 'define ' in ir, (name, ir)
            if name in ('typed_float', 'declared_float'):
                assert 'call i64 @dream_rt_float' not in ir, ir
                assert 'call i32 @dream_rt_builtin' not in ir, ir
print('type codegen: bytecode fusion, 320 matrix results and lazy/error parity passed'
      + ('; inline JIT verified' if has_jit else '; JIT unavailable'))
