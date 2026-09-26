#!/usr/bin/env bash
# The same six workloads in Dream and in Python, side by side.
#
# `main.dr` and `bench.py` are transliterations of each other at the same sizes,
# and both split their output the same way: the `Result` blocks to stdout, one
# `name<TAB>milliseconds` line per workload to stderr. So this script has two
# jobs -- diff the stdouts to check the two runtimes computed the same thing,
# and report the stderrs side by side.
#
# Each workload runs in a fresh process, `--repeat` times, and the *best* run is
# what is reported. Best rather than mean because what is being compared is how
# long the work takes, and everything a repeat adds to that -- a scheduler
# decision, a page fault, another process on the machine -- is added, never
# subtracted. Read the numbers with the noise floor in mind: docs/notes/vm-performance.md, "Two
# things that will lie to you about a change to the interpreter".
#
#   ./run.sh                    # all six, three runs each
#   ./run.sh --repeat 7 fib     # one of them, more carefully
#
# The Dream side needs the VM and the compiler; from a built checkout that is
# `just` and nothing else.
set -uo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)

dream=${dream:-$root/build-dream/bin/dream}
dreams=${dreams:-$root/build/dreams.dream}
python=${python:-python3}
image=${image:-$here/target/benchmark.dream}
repeat=3
benches=()

while [ $# -gt 0 ]; do
    case "$1" in
        --repeat) repeat=$2; shift 2 ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) benches+=("$1"); shift ;;
    esac
done
[ ${#benches[@]} -eq 0 ] && benches=(fib sum collatz pi strbuild mapfilter bytescan)

for f in "$dream" "$dreams"; do
    if [ ! -e "$f" ]; then
        echo "missing $f -- run \`just\` in $root first" >&2
        exit 1
    fi
done

mkdir -p "$(dirname "$image")"
"$dream" "$dreams" "$here/main.dr" -L "$root/mind" -o "$image" >/dev/null || exit 1

# The best of `repeat` runs, in milliseconds, for one workload in one runtime.
# The timing lines are on stderr; stdout is kept for the agreement check.
best() {
    local out=$1 name=$2; shift 2
    local ms i line
    for ((i = 0; i < repeat; i++)); do
        line=$("$@" "$name" 2>&1 >"$out" | grep -oP "^$name\t\K[0-9.]+")
        [ -z "$line" ] && continue
        if [ -z "${ms:-}" ] || awk -v a="$line" -v b="$ms" 'BEGIN{exit !(a<b)}'; then ms=$line; fi
    done
    echo "${ms:-}"
}

printf '%-11s %10s %10s %8s  %s\n' workload dream python ratio agree
status=0
for b in "${benches[@]}"; do
    d=$(best "$here/target/$b.dream.out" "$b" "$dream" "$image")
    p=$(best "$here/target/$b.py.out"    "$b" "$python" "$here/bench.py")
    if cmp -s "$here/target/$b.dream.out" "$here/target/$b.py.out"; then
        agree=yes
    else
        # `pi` is floating point summed in a different order; the rest must match.
        agree="NO"
        [ "$b" = pi ] && agree="float"
        [ "$agree" = NO ] && status=1
    fi
    printf '%-11s %10s %10s %8s  %s\n' "$b" "${d:-?}" "${p:-?}" \
        "$(awk -v d="${d:-0}" -v p="${p:-0}" 'BEGIN{ if (p>0 && d>0) printf "%.2fx", d/p; else print "-" }')" \
        "$agree"
done
echo
echo "ratio is dream/python: below 1.00x is Dream ahead."
exit $status
