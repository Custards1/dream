#!/bin/sh
# Lowering, over every program in the repository.
#
# The unit tests say that a `match` becomes the right shape and that a tail call
# is marked. What they cannot say is that lowering survives real code: every
# construct the language has, in the combinations people actually write, at a
# size where a quadratic mistake shows up as a machine running out of memory
# rather than as a slow test.
#
# So this lowers the whole corpus and checks two things per program: that it
# produces an arena at all, and that it produces one without complaining. A
# program `dreamc` accepts must lower.
set -u

dreamc=${dreamc:-target/debug/dreamc}
dream=${dream:-build-dream/bin/dream}
image=${image:-/tmp/dreams-lower.dream}

fail=0
say_fail() { echo "FAIL: $*"; fail=1; }

"$dreamc" dreams/main.dr -L mind -L . -o "$image" >/dev/null || exit 1

checked=0
total=0
for f in mind/std/*.dr mind/tool/*.dr examples/*.dr examples/*/*.dr \
         dream/tests/programs/*.dr dreams/*.dr; do
    [ -f "$f" ] || continue
    # Only programs the reference compiler accepts; anything else is a question
    # for `--check`, not for lowering.
    "$dreamc" "$f" --no-emit -L mind -L . -L examples >/dev/null 2>&1 || continue

    out=$("$dream" "$image" --stats -L mind -L . -L examples "$f" 2>&1)
    status=$?
    if [ "$status" -ne 0 ]; then
        say_fail "$f: lowering exited $status"
        printf '%s\n' "$out" | sed 's/^/    /' | head -5
        continue
    fi
    nodes=$(printf '%s\n' "$out" | awk '$1 == "nodes" { print $2 }')
    globals=$(printf '%s\n' "$out" | awk '$1 == "globals" { print $2 }')
    case "$nodes$globals" in
        ''|*[!0-9]*) say_fail "$f: no counts in the report"; continue ;;
    esac
    # A module that declares nothing lowers to nothing, and an empty file is a
    # legitimate module. What would be wrong is declarations with no code.
    if [ "$globals" -gt 0 ] && [ "$nodes" -le 0 ]; then
        say_fail "$f: has $globals globals but lowered to no nodes"
        continue
    fi
    total=$((total + nodes))
    checked=$((checked + 1))
done

echo "$checked programs lowered, $total execution-tree nodes in total"

# And the one that matters most: the compiler lowering itself. It is the largest
# program here, and the first one big enough to notice if the arena or the
# resolver ever goes quadratic again.
self=$("$dream" "$image" --stats -L mind -L . dreams/main.dr 2>&1) || {
    say_fail "dreams cannot lower itself"
    printf '%s\n' "$self" | sed 's/^/    /' | head -5
}
case "$self" in
    *nodes*) ;;
    *) say_fail "lowering itself reported no nodes" ;;
esac

[ "$fail" -eq 0 ] && echo "every program the reference compiler accepts also lowers"
exit "$fail"
