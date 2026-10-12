#!/usr/bin/env python3
"""std.path against the reference implementations.

Thousands of generated paths -- drives, UNC shares, device paths, doubled and
mixed separators, dotted and hidden names, `..` runs -- through every function
of `std.path.posix` and `std.path.windows`, compared with what Python's
`posixpath` and `ntpath` answer, and for `dirname`/`basename` with glibc's
libgen (POSIX's own definition) and `pathlib`.

The deliberate departures are written into the reference rather than skipped:
`join` drops an empty or `.` side, an extension is answered without its dot,
the extension of `c:.x` is pathlib's (none: the name is `.x`) rather than
`splitext`'s, and drives are compared as the system compares them, after
reading `\\` as `/`, where `ntpath.join` compares the raw spellings.
"""
import argparse
import ctypes
import ntpath
import pathlib
import posixpath
import random
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--dream', default='build-dream/bin/dream')
parser.add_argument('--compiler', default='build/dreams.dream')
parser.add_argument('--seed', type=int, default=1)
parser.add_argument('--count', type=int, default=6000)
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parents[2]
vm = str(pathlib.Path(args.dream).resolve())
compiler = str(pathlib.Path(args.compiler).resolve())

HARNESS = r'''
import std.path;
import std.file;
import std.str;
import std.list;

let segs ss = str.join_str "|" ss;
let opt v = match v { () => "<none>", ss => segs ss };
let b x = if x { "T" } else { "F" };

let one p = str.join_str "\t" [
    p,
    path.posix.anchor p, b (path.posix.is_absolute p), path.posix.normalize p,
    path.posix.extension p, path.posix.drop_extension p,
    path.posix.dirname p, path.posix.basename p, segs (path.posix.segments p),
    path.windows.anchor p, b (path.windows.is_absolute p), path.windows.normalize p,
    path.windows.extension p, path.windows.drop_extension p,
    path.windows.dirname p, path.windows.basename p, segs (path.windows.segments p),
];

let two line = match str.split "\t" line {
    [a, c] => str.join_str "\t" [a, c,
        path.posix.join a c, path.posix.relative a c, opt (path.posix.relative_to a c),
        path.windows.join a c, path.windows.relative a c, opt (path.windows.relative_to a c)],
    _ => "",
};

let lines_of! name =
    list.filter (fn l -> l != "") (str.split "\n" (match file.read! name { () => "", t => t }));

let main! = {
    file.write! "one.out" (str.join_str "\n" (list.map one (lines_of! "paths.txt")) + "\n")
    file.write! "two.out" (str.join_str "\n" (list.map two (lines_of! "pairs.txt")) + "\n")
};
'''

rng = random.Random(args.seed)
atoms = ['a', 'b', 'ab', '.', '..', '...', '.x', 'x.', 'a.b', 'a.tar.gz', '..f.d', 'C:', 'c:', 'Z:x',
         '?', 'UNC', 'srv', 'share', ' sp ', 'é', 'a\\b', '\\', '~', 'x y.z', '']
seps = ['/', '//', '///', '\\', '\\\\', '/\\']
starts = seps + ['C:/', 'C:\\', '\\\\?\\', '//?/UNC/', '//./', 'c:']

def generated():
    parts = [rng.choice(atoms) for _ in range(rng.randint(0, 5))]
    s = rng.choice(starts) if rng.random() < 0.5 else ''
    for i, p in enumerate(parts):
        s += p
        if i < len(parts) - 1 or rng.random() < 0.3:
            s += rng.choice(seps)
    return s

paths = sorted({generated() for _ in range(args.count)}
               | {'/', '//', '///', 'C:', 'C:/', '\\\\srv\\share', '//srv', '//?/C:/a/..'})
paths = [p for p in paths if p]
pairs = [(rng.choice(paths), rng.choice(paths)) for _ in range(args.count)]

with tempfile.TemporaryDirectory() as tmp:
    tmp = pathlib.Path(tmp)
    (tmp / 'harness.dr').write_text(HARNESS)
    (tmp / 'paths.txt').write_text('\n'.join(paths) + '\n')
    (tmp / 'pairs.txt').write_text('\n'.join(a + '\t' + c for a, c in pairs) + '\n')
    subprocess.run(['timeout', '300', vm, compiler, '-L', str(root / 'mind'), 'harness.dr',
                    '-o', 'harness.dream'], cwd=tmp, check=True, stdout=subprocess.DEVNULL)
    subprocess.run(['timeout', '300', vm, 'harness.dream'], cwd=tmp, check=True)
    one = (tmp / 'one.out').read_text().split('\n')
    two = (tmp / 'two.out').read_text().split('\n')

libc = ctypes.CDLL(None)
libc.dirname.restype = ctypes.c_char_p
# glibc's `basename` is the GNU one; libgen's is `__xpg_basename`.
libc.__xpg_basename.restype = ctypes.c_char_p

def libgen(fn, p):
    return fn(ctypes.create_string_buffer(p.encode())).decode()

def slashed(p):
    return p.replace('\\', '/')

def extension(mod, p):
    stem, e = mod.splitext(p)
    return (e[1:] if e else ''), stem

def posix_names(p):
    return [x for x in posixpath.splitroot(p)[2].split('/') if x not in ('', '.')]

def windows_names(p):
    return [x for x in slashed(ntpath.splitroot(p)[2]).split('/') if x not in ('', '.')]

def posix_join(a, c):
    if c in ('', '.'): return a
    if a in ('', '.'): return c
    return posixpath.join(a, c)

def windows_join(a, c):
    if c in ('', '.'): return slashed(a)
    if a in ('', '.'): return slashed(c)
    return slashed(ntpath.join(slashed(a), slashed(c)))

def relative(mod, names, a, c):
    a, c = mod.normpath(a), mod.normpath(c)
    windows = mod is ntpath
    fold = (lambda s: s.lower()) if windows else (lambda s: s)
    anchor = lambda s: slashed(''.join(mod.splitroot(s)[:2]))
    answer = slashed(c) if windows else c
    if fold(anchor(a)) != fold(anchor(c)):
        return answer
    xs, ys = names(a), names(c)
    i = 0
    while i < min(len(xs), len(ys)) and fold(xs[i]) == fold(ys[i]):
        i += 1
    if '..' in xs[i:]:
        return answer
    segs = ['..'] * (len(xs) - i) + ys[i:]
    if anchor(a) and not windows:
        assert posixpath.relpath(c, a) == ('/'.join(segs) or '.'), (a, c)
    return '/'.join(segs) or '.'

failures = {}
def check(what, case, want, got):
    if want != got:
        failures.setdefault(what, []).append((case, want, got))

for line in one:
    if not line: continue
    f = line.split('\t')
    p = f[0]
    check('posix.anchor', p, ''.join(posixpath.splitroot(p)[:2]), f[1])
    check('posix.is_absolute', p, 'T' if posixpath.isabs(p) else 'F', f[2])
    check('posix.normalize', p, posixpath.normpath(p), f[3])
    e, stem = extension(posixpath, p)
    check('posix.extension', p, e, f[4])
    check('posix.drop_extension', p, stem, f[5])
    check('posix.dirname', p, libgen(libc.dirname, p), f[6])
    check('posix.basename', p, libgen(libc.__xpg_basename, p), f[7])
    check('posix.segments', p, '|'.join(posix_names(p)), f[8])
    drive, rooted, _ = ntpath.splitroot(p)
    check('windows.anchor', p, slashed(drive + rooted), f[9])
    check('windows.is_absolute', p, 'T' if ntpath.isabs(p) else 'F', f[10])
    check('windows.normalize', p, slashed(ntpath.normpath(p)), f[11])
    e, stem = extension(ntpath, p[len(drive):])
    check('windows.extension', p, e, f[12])
    check('windows.drop_extension', p, p[:len(drive)] + stem, f[13])
    check('windows.segments', p, '|'.join(windows_names(p)), f[16])
    # pathlib folds `.` away, so compare where the last name is a real one.
    last = [x for x in slashed(p).split('/') if x]
    if windows_names(p) and last[-1] not in ('.', '..'):
        check('windows.basename', p, pathlib.PureWindowsPath(p).name, f[15])
        check('windows.dirname', p, slashed(str(pathlib.PureWindowsPath(p).parent)),
              slashed(str(pathlib.PureWindowsPath(f[14]))))

for line in two:
    if not line: continue
    f = line.split('\t')
    a, c = f[0], f[1]
    check('posix.join', (a, c), posix_join(a, c), f[2])
    check('posix.relative', (a, c), relative(posixpath, posix_names, a, c), f[3])
    check('windows.join', (a, c), windows_join(a, c), f[5])
    check('windows.relative', (a, c), relative(ntpath, windows_names, a, c), f[6])

for what, cases in sorted(failures.items()):
    print(f'{what}: {len(cases)} mismatches')
    for case, want, got in cases[:6]:
        print(f'    {case!r}: reference {want!r}, std.path {got!r}')
mismatches = sum(len(v) for v in failures.values())
print(f'std.path: {len(paths)} paths, {len(pairs)} pairs, {mismatches} mismatches')
raise SystemExit(1 if mismatches else 0)
