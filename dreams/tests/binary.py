#!/usr/bin/env python3
"""Binary layout interoperability against Python's integer and IEEE encodings."""
import argparse
import math
import pathlib
import random
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
rng = random.Random(0xB17)
lines, expected = [], []

def case(expression, answer):
    lines.append(f'    console.print! ({expression})')
    expected.append(answer)

def raw(data):
    return 'str.of_bytes [' + ', '.join(map(str, data)) + ']'

for width in [1, 2, 3, 4, 8, 16, 33]:
    for signed in [False, True]:
        bits = width * 8
        lo, hi = (-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if signed else (0, (1 << bits) - 1)
        for n in [lo, hi, 0] + [rng.randint(lo, hi) for _ in range(12)]:
            for order in ['big', 'little']:
                c = f'b.{"sint" if signed else "uint"} :{"be" if order == "big" else "le"} {width}'
                encoded = n.to_bytes(width, order, signed=signed)
                case(f'show ({c}) ({n})', encoded.hex())
                case(f'read ({c}) ({raw(encoded)})', str(n))

floats = [0.0, -0.0, 1.0, -1.0, float('inf'), -float('inf'), float('nan'),
          65504.0, 65519.0, 65520.0, 1.00048828125, 1.00146484375,
          2**-24, 2**-25, 3*2**-25, 2**-14 - 2**-25, 2**-149, 2**-150]
floats += [struct.unpack('<d', rng.getrandbits(64).to_bytes(8, 'little'))[0] for _ in range(180)]
for value in floats:
    for fmt, width in [('e', 16), ('f', 32), ('d', 64)]:
        for order, endian in [('be', '>'), ('le', '<')]:
            try:
                encoded = struct.pack(endian + fmt, value)
            except OverflowError:
                encoded = struct.pack(endian + fmt, math.copysign(float('inf'), value))
            # binary16/32 deliberately canonicalize NaNs, keeping their sign.
            if math.isnan(value) and width != 64:
                nan = (0x7e00 if width == 16 else 0x7fc00000)
                if math.copysign(1, value) < 0:
                    nan |= 1 << (width - 1)
                encoded = nan.to_bytes(width // 8, 'big' if order == 'be' else 'little')
            case(f'show b.f{width}{order} (num.float_of_bytes ({raw(struct.pack("<d", value))}))', encoded.hex())

# Every finite half-precision representation, plus both infinities and zeros.
# Decode to double bytes: numeric text would hide rounding and signed zero.
for n in range(65536):
    if n & 0x7c00 == 0x7c00 and n & 0x3ff:
        continue
    expected.append(struct.pack('<d', struct.unpack('>e', n.to_bytes(2, 'big'))[0]).hex() + ':' + n.to_bytes(2, 'big').hex())

source = '''import std.binary as b;
import std.console;
import std.crypto;
import std.num;
import std.str;
let show c v = match b.encode c v { [:ok, s] => crypto.hex s, [:error, e] => to_string e };
let read c s = match b.decode c s { [:ok, v] => to_string v, [:error, e] => to_string e };
let rec sweep! n = if n == 65536 { () } else {
    if n & 31744 != 31744 || n & 1023 == 0 {
        let data = str.of_bytes [n >> 8, n & 255];
        match b.decode b.f16be data {
            [:ok, f] => console.print! (crypto.hex (num.float_bytes f) + ":" + show b.f16be f),
            [:error, e] => console.print! e,
        }
    }
    sweep! (n + 1)
};
let main! = {
''' + '\n'.join(lines) + '\n    sweep! 0\n};\n'

with tempfile.TemporaryDirectory(prefix='dream-binary-') as directory:
    path = pathlib.Path(directory)
    src, image = path / 'main.dr', path / 'main.dream'
    src.write_text(source)
    compile = subprocess.run([vm, compiler, '-L', str(root / 'mind'), str(src), '-o', str(image)],
                             capture_output=True, text=True, timeout=120)
    assert compile.returncode == 0, compile.stdout + compile.stderr
    unit_src, unit_image = path / 'units.dr', path / 'units.dream'
    unit_src.write_text('import std.binary.tests as t;\nimport std.test;\n'
                        'let main! = test.main_of! [["std.binary", t.tests]];\n')
    units = subprocess.run([vm, compiler, '-L', str(root / 'mind'), '-D', 'test',
                            str(unit_src), '-o', str(unit_image)],
                           capture_output=True, text=True, timeout=120)
    assert units.returncode == 0, units.stdout + units.stderr
    for mode in [['--no-jit'], ['--jit-threshold', '1']]:
        unit_run = subprocess.run([vm, *mode, str(unit_image)], capture_output=True, text=True, timeout=60)
        assert unit_run.returncode == 0, unit_run.stdout + unit_run.stderr
        run = subprocess.run([vm, *mode, str(image)], capture_output=True, text=True, timeout=180)
        assert run.returncode == 0, run.stderr
        got = run.stdout.splitlines()
        assert len(got) == len(expected), (mode, len(got), len(expected), run.stderr)
        for i, (actual, wanted) in enumerate(zip(got, expected)):
            assert actual == wanted, (mode, i, lines[i] if i < len(lines) else 'binary16 exhaustive decode', actual, wanted)
print(f'binary: {len(expected)} reference vectors agree in interpreter and JIT')
