#!/usr/bin/env python3
"""Primitive lowering, lazy/first-class parity, and malformed opcode operands."""
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


def run(*cmd):
    result = subprocess.run(cmd, text=True, capture_output=True, timeout=120)
    assert result.returncode == 0, result.stdout + result.stderr
    return result.stdout


def section(data, wanted):
    count, = struct.unpack_from('<I', data, 28)
    for i in range(count):
        kind, offset, length, _ = struct.unpack_from('<4sIII', data, 32 + i * 16)
        if kind == wanted:
            return offset, length
    raise AssertionError(wanted)


# These are wire IDs, not inferred from the implementation under test.
primitives = [
    ('str_chars', 1), ('error_new', 2), ('error_kind', 1), ('error_payload', 1),
    ('str_slice', 3), ('str_find', 3), ('str_byte', 2), ('str_le', 2),
    ('str_span', 3), ('str_upto', 3), ('char_code', 1), ('char_of_code', 1),
    ('to_float', 1), ('float_bytes', 1), ('float_of_bytes', 1), ('to_int', 1),
    ('parse_int', 1), ('parse_float', 1), ('to_existing_atom', 1),
    ('array_new', 2), ('array_to_list', 1), ('map_has', 2), ('map_remove', 2),
    ('map_pairs', 1), ('data_count', 1), ('data_at', 1), ('compare', 2),
]
with tempfile.TemporaryDirectory(prefix='dream-primitives-') as directory:
    temp = pathlib.Path(directory)
    src = temp / 'matrix.dr'
    src.write_text('import std.console;\n' + '\n'.join(
        f'let probe_{name} ' + ' '.join('xyz'[:arity]) + f' = {name} ' + ' '.join('xyz'[:arity]) + ';'
        for name, arity in primitives) + '\nlet main! = console.print! 0;\n')
    image = temp / 'matrix.dream'
    run(vm, compiler, str(src), '-o', str(image))
    data = image.read_bytes()
    offset, length = section(data, b'NODE')
    nodes = list(struct.iter_unpack('<BBHIII', data[offset:offset + length]))
    assert {n[0] for n in nodes if n[0] >= 47} == set(range(47, 74))
    for opcode, (_, arity) in enumerate(primitives, 47):
        assert all(n[5] == arity for n in nodes if n[0] == opcode)
    assert run(vm, '--no-jit', str(image)) == '0\n'

    # Fixed primitive identity, arity, argument run, and cycles are validated.
    index = next(i for i, node in enumerate(nodes) if node[0] == 47)
    for field, value in ((4, 0), (8, 0xFFFFFFFF), (12, 2)):
        corrupt = bytearray(data)
        struct.pack_into('<I', corrupt, offset + index * 16 + field, value)
        bad = temp / 'bad.dream'
        bad.write_bytes(corrupt)
        result = subprocess.run([vm, str(bad)], capture_output=True, timeout=30)
        assert result.returncode > 0, result.stderr
    corrupt = bytearray(data)
    kids_offset, _ = section(data, b'KIDS')
    struct.pack_into('<I', corrupt, kids_offset + nodes[index][4] * 4, index)
    bad.write_bytes(corrupt)
    result = subprocess.run([vm, str(bad)], capture_output=True, timeout=30)
    assert result.returncode > 0, result.stderr

    fixture = root / 'dream/tests/programs/core_primitives.dr'
    expected = fixture.with_suffix('.expected').read_text()
    for options in ([], ['--no-opt']):
        run(vm, compiler, str(fixture), '-o', str(image), *options)
        assert run(vm, '--no-jit', str(image)) == expected
        assert run(vm, '--jit-threshold', '1', str(image)) == expected
    # A scalar primitive still compiles in the JIT after moving out of modules.
    run(vm, compiler, str(fixture), '-o', str(image))
    result = subprocess.run([vm, '--dump-jit', 'bytes', str(image)],
                            text=True, capture_output=True, timeout=30)
    if 'this build has no JIT' not in result.stderr:
        assert result.returncode == 0 and 'define ' in result.stdout, result.stderr

    for name in ('std.core', 'std.native'):
        src.write_text(f'import {name};\nlet main! = ();\n')
        result = subprocess.run([vm, compiler, str(src), '-o', str(image)],
                                text=True, capture_output=True, timeout=30)
        assert result.returncode > 0, name
    for name in ('map_get', 'map_put', 'array_get', 'array_set', 'map_new', 'str_len'):
        src.write_text(f'let removed = {name};\nlet main! = ();\n')
        result = subprocess.run([vm, compiler, str(src), '-o', str(image)],
                                text=True, capture_output=True, timeout=30)
        assert result.returncode > 0, name
print('primitive opcodes, lazy/first-class parity, validation, and module removal passed')
