#!/usr/bin/env bash
# Programs shaped like programs, built native and run on the VM, best of three each, in ms.
# The VM's number is the image under the JIT; the native one is `dreams --aot`. The two
# outputs are compared, so a change that is faster because it is wrong shows as DIFF.
#
#   benchmark/native/run.sh                     # all of them
#   benchmark/native/run.sh procs maps          # some
#   image=/tmp/base/dreams.dream mind=/tmp/base/mind benchmark/native/run.sh   # A/B another compiler and runtime
#   vm=0 benchmark/native/run.sh                # native only
#
# What each one asks:
#   tree   allocation and a deep recursion: a binary tree of 2^20 leaves, and many of 2^16
#   maps   a large live heap of path-copied maps, which is what a copying collector pays for
#   sorts  sorting a million integers, then a pipeline over them
#   recs   records of floats, a thousand steps of a thousand particles
#   strs   making, joining, splitting and searching strings
#   procs  fifty thousand processes, each sent one message and answering one: switching and copying
#   fanout the same work split over a process per core: what more than one thread is for

set -uo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
dream="$(realpath "${DREAM:-$root/build-dream/bin/dream}")"
image="$(realpath "${image:-$root/build/dreams.dream}")"
mind="$(realpath "${mind:-$root/mind}")"
vm="${vm:-1}"
dir="$(mktemp -d)"
trap 'rm -rf "$dir"' EXIT

names=("$@")
[[ ${#names[@]} -eq 0 ]] && names=(tree maps sorts recs strs procs fanout)

best() {
  local b=999999 t0 t1 ms
  for _ in 1 2 3; do
    t0=$(date +%s%N)
    timeout -s KILL 120 "$@" >"$dir/out" 2>&1 </dev/null
    t1=$(date +%s%N)
    ms=$(( (t1 - t0) / 1000000 ))
    (( ms < b )) && b=$ms
  done
  echo "$b"
}

for w in "${names[@]}"; do
  src="$here/$w.dr"
  [[ -f "$src" ]] || { echo "$w: no such benchmark"; continue; }
  if ! timeout 120 "$dream" "$image" -L "$mind" -o "$dir/$w.dream" "$src" >"$dir/said" 2>&1; then
    echo "$w: does not compile"; head -5 "$dir/said"; continue
  fi
  if ! timeout 300 "$dream" "$image" --aot -L "$mind" -o "$dir/$w" "$dir/$w.dream" >"$dir/said" 2>&1; then
    echo "$w: does not build native"; grep error "$dir/said" | head -5; continue
  fi
  n=$(best "$dir/$w"); cp "$dir/out" "$dir/native.out"
  if [[ "$vm" == 1 ]]; then
    v=$(best "$dream" "$dir/$w.dream")
    if diff -q "$dir/out" "$dir/native.out" >/dev/null; then same=same; else same=DIFF; fi
    printf '%-7s vm %6s  native %6s  %s\n' "$w" "$v" "$n" "$same"
    if [[ "$same" == DIFF ]]; then echo "  vm:     $(head -c 200 "$dir/out")"; echo "  native: $(head -c 200 "$dir/native.out")"; fi
  else
    printf '%-7s native %6s  %s\n' "$w" "$n" "$(head -c 60 "$dir/native.out" | tr '\n' ' ')"
  fi
done
