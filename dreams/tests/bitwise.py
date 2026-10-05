#!/usr/bin/env python3
"""Integer bitwise operators across parsing, typing, bytecode and both VM tiers."""
import argparse
import pathlib
import random
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--dream', default='build-dream/bin/dream')
parser.add_argument('--compiler', default='build/dreams.dream')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parents[2]
vm = str(pathlib.Path(args.dream).resolve())
compiler = str(pathlib.Path(args.compiler).resolve())

with tempfile.TemporaryDirectory(prefix='dream-bitwise-') as directory:
    source = pathlib.Path(directory) / 'main.dr'
    image = pathlib.Path(directory) / 'main.dream'

    def compile_source(text):
        source.write_text(text)
        return subprocess.run([vm, compiler, '-L', str(root / 'mind'), str(source),
                               '-o', str(image)], text=True, capture_output=True, timeout=60)

    definitions = '''import std.console;
import std.error;
let band a b = a & b;
let bor a b = a | b;
let bxor a b = a ^ b;
let bnot a = ~a;
let shl a b = a << b;
let shr a b = a >> b;
let float_bits x = (x + 0.5) & 1;
let float_not x = ~(x + 0.5);
macro identity e = e;
let rec recursive_bits n = if n == 0 { 1 } else { recursive_bits (n - 1) << 1 };
let lazy_bits a b = [~a, a & b, a | b, a ^ b, a << b, a >> b];
let rec loop n !acc = if n == 0 { acc } else { loop (n - 1) (acc ^ (n << 2)) };
'''
    calls, expected = [], []

    def case(expr, value):
        calls.append('    console.print! (' + expr + ')')
        expected.append(str(value))

    rng = random.Random(7106)
    values = [0, 1, -1, 2, -3, (1 << 62) - 1, -(1 << 62), 1 << 62,
              -(1 << 62) - 1, (1 << 64) - 1, -(1 << 64), (1 << 127) + 1]
    values += [rng.getrandbits(rng.randrange(1, 513)) * rng.choice([-1, 1]) for _ in range(50)]
    for a in values:
        case(f'bnot ({a})', ~a)
        b = rng.choice(values)
        for name, value in [('band', a & b), ('bor', a | b), ('bxor', a ^ b)]:
            case(f'{name} ({a}) ({b})', value)
        for k in [0, 1, 62, 63, 64, 65, 127, 128, 511]:
            case(f'shl ({a}) {k}', a << k)
            case(f'shr ({a}) {k}', a >> k)
    case('shr (-3) 999999999999999999999999', -1)
    case('shr 3 999999999999999999999999', 0)
    case('shl 0 999999999999999999999999', 0)
    case('1 | 2 ^ 3 & 4 << 5 + 6', 1 | 2 ^ 3 & 4 << 5 + 6)
    case('3 & 1 == 1', 'true')
    case('1 << 3 << 2', 32)
    case('lazy_bits 5 2', '[-6, 0, 7, 7, 20, 1]')
    case('comp ((1 << 130) | 7)', (1 << 130) | 7)
    case('expand identity (~(3 ^ 5))', ~(3 ^ 5))
    case('expand identity ((3 & 1) | (8 >> 2))', (3 & 1) | (8 >> 2))
    case('5 & 1 :: []', '[1]')
    acc = 0
    for n in range(1, 12001):
        acc ^= n << 2
    case('loop 12000 0', acc)
    case('recursive_bits 70', 1 << 70)
    for expr, kind in [('shl 1 (-1)', ':out_of_bounds'), ('shr 1 (-1)', ':out_of_bounds'),
                       ('shl 1 4294967297', ':out_of_memory'),
                       ('shl 1 999999999999999999999999', ':out_of_memory'),
                       ('float_bits 1', ':type_error'), ('float_not 1', ':type_error'),
                       ('bnot 1.5', ':type_error'), ('band 1.0 1', ':type_error'),
                       ('bor true 1', ':type_error'), ('bxor 1 []', ':type_error'),
                       ('shl 1 2.0', ':type_error'), ('shr 1 true', ':type_error')]:
        case('try! { ' + expr + ' } catch e { error.kind e }', kind)
    result = compile_source(definitions + 'let main! = {\n' + '\n'.join(calls) + '\n};\n')
    assert result.returncode == 0, result.stdout + result.stderr
    wanted = '\n'.join(expected) + '\n'
    for mode in (['--no-jit'], ['--jit-threshold', '1']):
        run = subprocess.run([vm, *mode, str(image)], text=True, capture_output=True, timeout=60)
        assert run.returncode == 0, run.stderr
        if run.stdout != wanted:
            for i, (got, want) in enumerate(zip(run.stdout.splitlines(), expected)):
                assert got == want, (mode, calls[i], got, want)
            raise AssertionError(('output length', len(run.stdout.splitlines()), len(expected)))
    for name in ['band', 'bor', 'bxor', 'bnot', 'shl', 'shr', 'lazy_bits', 'loop', 'recursive_bits', 'float_bits', 'float_not']:
        dump = subprocess.run([vm, '--dump-jit', name, str(image)],
                              text=True, capture_output=True, timeout=60)
        if 'this build has no JIT' in dump.stderr + dump.stdout:
            break
        assert 'define ' in dump.stdout + dump.stderr, (name, dump.stdout, dump.stderr)

    for expr in ['1.0 & 1', '1 | true', '[] ^ 1', '~1.0', '1 << 2.0', '1 >> false']:
        result = compile_source('let bad : :integer = ' + expr + ';\nlet main! = bad;\n')
        assert result.returncode != 0, ('unexpected success', expr)
        assert 'should be `:integer`' in result.stderr, result.stderr
    print(f'bitwise: {len(expected)} results agree in both tiers; JIT and type checks passed')
