#!/usr/bin/env python3
"""Generate a program of N declarations and time the compiler on it.

This exists because the profile that answers "what is expensive?" cannot answer
"what grows faster than the input?", and only the second question finds a
quadratic. This repository is too small to contain one: `dreams` is fifty
modules of a hundred-odd declarations, and n^2 on a hundred is invisible. Four
quadratics lived here for the whole life of the compiler for exactly that
reason -- see "The compiler was quadratic in the size of the program" in
CLAUDE.md.

    dreams/tests/scale.py --sizes 800 1600 3200
    dreams/tests/scale.py --sizes 400 1600 --modules 50   # many modules, few decls each
    dreams/tests/scale.py --sizes 400 1600 --call-in-module  # the expand in a big module

Read the ratio column, not the milliseconds. Near 2.0 for a doubling is linear
and fine however large the absolute number; near 4.0 is a bug however small.
Nothing here is part of `just test`: it takes minutes and needs no fixture.

Read both columns. Time says whether something is quadratic; peak heap says
whether it is the wall a large project actually hits. They are different
questions and the modules axis has answered them differently twice -- the
three loader lists taken out in 2026-09-20 were 18% of allocation and 34% of
the peak, and `modules.modules`, the one a previous round had named, was 6% of
the first and none of the second.

The discovery axis is the newest and the only one on which *this* repository
is the expensive workload and a generated program is free -- the inverse of the
warning above, and the reason it went unmeasured. Discovery is per item of a
module that has a call written in it, so what it costs is the size of that
module: 2, 3 and 5 ms at 400, 800 and 1,600 declarations, linear and cheap.
What made `dreams` pay 49 ms for eighteen one-line calls was not that axis but
the fallback behind it -- a module whose offsets cannot be matched to its
declarations is walked node by node -- and a generated program never trips it,
because nothing generated here is written as a `match`. See "Discovery was a
walk of two whole modules" in CLAUDE.md.

The modules axis has to be run *far* out before it says anything. `envs` was a
list indexed by module for the whole life of the compiler, and at 400 modules
-- as far as anyone had taken this -- the list and the map that replaced it are
indistinguishable. At 3,200 the list is 1967 MB against 594, and does not
compile under the default heap cap at all. Take the last doubling you can
afford, not the first one that runs.
"""

import argparse, os, shutil, subprocess, sys, tempfile, time

def write_program(root, decls, modules, with_macro, call_in_module=False):
    """`modules` files sharing `decls` declarations between them, one `expand`.

    `call_in_module` writes that one call into the *largest* generated module
    instead of into `main.dr`, which is the third axis this file can vary and
    the one nobody had. Discovery -- "is there anything to expand?" -- is per
    item of a module that has a call written in it and free for every module
    that has none, so the size of the module the call lives in is the input it
    is linear in, and neither of the other two axes moves it. `dreams` writes
    its own macro calls in `lower.dr`, its largest module, which is why its
    discovery cost 49 ms where a generated program's was 0.
    """
    per = max(1, decls // modules)
    for m in range(modules):
        with open(os.path.join(root, f"m{m}.dr"), "w") as f:
            f.write("import std.list;\n")
            if m == 0 and call_in_module:
                f.write("import mac;\n\nlet add a b = a + b;\n")
            f.write("\n")
            for i in range(per):
                k = m * per + i
                f.write(f"let f{k} n = list.fold (fn a x -> a + x * {k+1}) "
                        f"n (list.range 0 (n + {k}));\n")
            # Last, so that every declaration in front of it is one the
            # discovery walk has to step over to reach the call.
            if m == 0 and call_in_module:
                f.write("let v0 = expand mac.twice 0;\n" if with_macro
                        else "let v0 = add 0 0;\n")
    with open(os.path.join(root, "mac.dr"), "w") as f:
        f.write('import std.list;\n\n'
                'macro twice e = [:apply, [:name, "add", [0, 0]], [e, e], [0, 0]];\n')
    with open(os.path.join(root, "main.dr"), "w") as f:
        f.write("import std.console;\n")
        if not call_in_module:
            f.write("import mac;\n")
        for m in range(modules):
            f.write(f"import m{m};\n")
        f.write("\nlet add a b = a + b;\n")
        # The macro call is the point of the `mac` import: a one-line transformer
        # in a program of any size, which is what the macro tax is measured as.
        if call_in_module:
            f.write("let v0 = m0.v0;\n")
        else:
            f.write("let v0 = expand mac.twice 0;\n" if with_macro else "let v0 = add 0 0;\n")
        f.write("let main! = { console.print! v0 };\n")

def run(vm, compiler, root, out, env):
    started = time.monotonic()
    p = subprocess.run([vm, "--stats", compiler, "-L", "mind", "-L", root,
                        "-o", out, os.path.join(root, "main.dr")],
                       capture_output=True, text=True, env=env)
    elapsed = time.monotonic() - started
    live = 0
    for line in (p.stdout + p.stderr).splitlines():
        # "; N bytes allocated, N promoted, N live at each heap's peak"
        if "live at each heap" in line:
            live = int(line.split(",")[-1].strip().split()[0])
    return elapsed, live, p.returncode, (p.stdout + p.stderr)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vm", default="build-dream/bin/dream")
    ap.add_argument("--compiler", default="build/dreams.dream")
    ap.add_argument("--sizes", type=int, nargs="+", default=[400, 1600, 3200])
    ap.add_argument("--modules", type=int, default=1,
                    help="spread the declarations over this many modules")
    ap.add_argument("--call-in-module", action="store_true",
                    help="write the one `expand` into the largest generated module "
                         "rather than into main.dr -- the discovery axis")
    ap.add_argument("--max-heap", default=str(8 * 1024**3),
                    help="DREAM_MAX_HEAP; the default 1 GB stops a big program")
    ap.add_argument("--keep", action="store_true", help="leave the generated trees")
    args = ap.parse_args()

    env = dict(os.environ, DREAM_MAX_HEAP=args.max_heap)
    temp = tempfile.mkdtemp(prefix="dream-scale-")
    print(f"{'decls':>7} {'modules':>8} {'macro':>9} {'none':>9} {'tax':>7} "
          f"{'live MB':>9} {'t ratio':>8} {'m ratio':>8}")
    last = None
    try:
        for n in args.sizes:
            row = {}
            for label, with_macro in (("macro", True), ("none", False)):
                root = os.path.join(temp, f"{label}{n}")
                os.makedirs(root, exist_ok=True)
                write_program(root, n, args.modules, with_macro, args.call_in_module)
                secs, live, rc, output = run(args.vm, args.compiler, root,
                                             os.path.join(temp, f"{label}{n}.dream"), env)
                if rc != 0 or "compiled ->" not in output:
                    print(f"{n:>7} FAILED: {output.strip().splitlines()[-1] if output.strip() else rc}")
                    row = None
                    break
                row[label] = (secs, live)
            if row is None:
                last = None
                continue
            tm, live = row["macro"]
            tn, _ = row["none"]
            tr = mr = float("nan")
            if last:
                tr, mr = tm / last[0], (live / last[1] if last[1] else float("nan"))
            print(f"{n:>7} {args.modules:>8} {tm*1000:>8.0f}ms {tn*1000:>8.0f}ms "
                  f"{(tm-tn)*1000:>6.0f}ms {live/1048576:>8.0f} {tr:>8.2f} {mr:>8.2f}")
            last = (tm, live)
    finally:
        if args.keep:
            print(f"\ngenerated trees left in {temp}")
        else:
            shutil.rmtree(temp, ignore_errors=True)

if __name__ == "__main__":
    sys.exit(main())
