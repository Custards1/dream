#!/bin/sh
# Learn Dream (learn/) holds every example to what its fence says: a program
# that stops compiling, or starts printing something else than a failure it
# promised, fails the build here before it can mislead a reader.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
dream=$(cd "$root" && realpath "${dream:-build-dream/bin/dream}")
mind=$(realpath "${mind:-/tmp/dream-mind-graph.dream}")
dreams=$(cd "$root" && realpath "${dreams:-build/dreams.dream}")
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cd "$root/learn"
DREAM="$dream" DREAMS="$dreams" MIND_STDLIB="$root/mind" MIND_UNITS=off \
    "$dream" "$mind" docs --strict-examples -o "$out" >/dev/null 2>"$out.log" || { cat "$out.log"; rm -f "$out.log"; exit 1; }
rm -f "$out.log"
echo "Learn Dream: $(ls "$out/guides" | wc -l) chapters, every example as its fence says"
