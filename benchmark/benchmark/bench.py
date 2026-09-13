#!/usr/bin/env python3
"""The same benchmark as main.dr, in Python, so the two runtimes can be compared.

Each workload is a straight transliteration of the Dream one, with the same
sizes, so the results agree with what Dream prints (up to float rounding in
`pi`, which is beside the point -- what is being compared is how long the same
algorithm takes, not the last digit of a sum) and the two scripts' stdout can
be diffed:

Result        # line 1: "Result"
    fib           # line 2: the name
    	      # line 3: a bare tab
    2178309       # line 4: the value

Dream's result blocks are four lines -- "Result", the name, a bare tab, the
value -- and what is on the tab line above is that literal tab. This mirrors it
byte for byte. Timing goes to stderr, one `name<TAB>milliseconds` line per
benchmark, and Dream's `debug.bench!` lines also go to stderr, so stdout is the
diffable comparison and stderr the timings.

Where Dream iterates a lazy range with a fold, Python iterates `range` in a
plain loop; where Dream builds lists (mapfilter), so does the shape here stay a
walk over the elements. There is deliberately no `sum()` over a generator or
`functools.reduce` -- the idea is the same algorithm at the same sizes, not how
optimized a different idiom is.
"""

import sys
import time


def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


def collatz_steps(n):
    if n == 1:
        return 0
    return 1 + collatz_steps(n // 2 if n % 2 == 0 else 3 * n + 1)


def collatz_total(n):
    total = 0
    for x in range(1, n + 1):
        total += collatz_steps(x)
    return total


def sum_range(n):
    total = 0
    for x in range(1, n + 1):
        total += x
    return total


def pi_terms(n):
    total = 0.0
    for k in range(1, n + 1):
        total += 1.0 / (4 * k - 3) - 1.0 / (4 * k - 1)
    return total


def strbuild(n):
    return len("hello " * n)


def mapfilter(n):
    total = 0
    for x in range(1, n + 1):
        sq = x * x
        if sq % 2 == 1:
            total += sq
    return total


FIB_N = 32
SUM_N = 10_000_000
COLLATZ_N = 100_000
PI_N = 2_000_000
STRBUILD_N = 400_000
MAPFILTER_N = 500_000


def bench(name, work):
    before = time.perf_counter()
    value = work()
    after = time.perf_counter()
    ms = (after - before) * 1000
    print(f"{name}\t{ms:.3f}", file=sys.stderr)
    return value


def result(name, value):
    print("Result")
    print(name)
    print("\t")
    print(value)


def main():
    args = sys.argv[1:]
    run_all = not args

    def want(name):
        return run_all or name in args

    if want("fib"):
        value = bench("fib", lambda: fib(FIB_N))
        result("fib", value)
    if want("sum"):
        value = bench("sum", lambda: sum_range(SUM_N))
        result("sum", value)
    if want("collatz"):
        value = bench("collatz", lambda: collatz_total(COLLATZ_N))
        result("collatz", value)
    if want("pi"):
        value = bench("pi", lambda: pi_terms(PI_N))
        result("pi", value)
    if want("strbuild"):
        value = bench("strbuild", lambda: strbuild(STRBUILD_N))
        result("strbuild", value)
    if want("mapfilter"):
        value = bench("mapfilter", lambda: mapfilter(MAPFILTER_N))
        result("mapfilter", value)


if __name__ == "__main__":
    main()