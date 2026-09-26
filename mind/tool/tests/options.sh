#!/bin/sh
# Options, configuration and profiles, against packages on disk: what each
# package declares, who configures it, the conflicts and typos that are
# refused, dependencies that hang on an option, and the program that comes
# out reading its own package's settings. Each case is one `mind` command in
# CASE/app and a line of what it should say.
set -u

dream=${dream:-build-dream/bin/dream}
mind=${mind:-build/mind}
dreams=${dreams:-build/dreams.dream}
root=$(pwd)
case "$dream" in /*) ;; *) dream="$root/$dream" ;; esac
case "$mind" in /*) ;; *) mind="$root/$mind" ;; esac
case "$dreams" in /*) ;; *) dreams="$root/$dreams" ;; esac
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
export MIND_STDLIB="$root/mind" MIND_HOME="$tmp/home" DREAM="$dream" DREAMS="$dreams"

fail=0
pass=0

# pkg DIR TOML [MODULE] -- a package: its manifest's lines after [package],
# and the text of its one module.
pkg() {
    mkdir -p "$tmp/$1"
    printf '[package]\nname = "%s"\nversion = "1.0.0"\n%b' "$(basename "$1")" "$2" >"$tmp/$1/mind.toml"
    printf '%b' "${3:-let x = 1;\n}" >"$tmp/$1/lib.dr"
}

# check NAME CASE STATUS TEXT COMMAND... -- run `mind COMMAND` in CASE/app.
check() {
    name=$1; dir=$2; want_status=$3; want=$4; shift 4
    out=$(cd "$tmp/$dir/app" && timeout 60 "$dream" "$mind" "$@" 2>&1)
    got_status=$?
    case "$out" in
        *"$want"*) if [ "$got_status" = "$want_status" ]; then
                       echo "ok   $name"; pass=$((pass + 1)); return
                   fi ;;
    esac
    echo "FAIL $name: wanted $want_status and \"$want\", got $got_status:"
    echo "$out" | sed 's/^/    /' | head -10
    fail=$((fail + 1))
}

# A library with options, whose code reads them, and a program that prints
# what it was built as.
sq_opts='[options]\nvendored = { type = "bool", default = false, doc = "build from source" }\nthreads = { type = ["off", "single", "multi"], default = "multi" }\ncache_mb = 64\n'
sq_code='when vendored { let how = "vendored"; }\nwhen not vendored { let how = "system"; }\nwhen threads == "off" { let threads = "no threads"; }\nwhen not (threads == "off") { let threads = "threads"; }\n'
app_code='import std.console;\nimport sq.lib;\nwhen sq.vendored { let seen = "sees vendored"; }\nwhen not sq.vendored { let seen = "sees system"; }\nwhen vendored { let leak = " LEAKED"; }\nwhen not vendored { let leak = ""; }\nlet main! = console.print! (lib.how + ", " + lib.threads + ", " + seen + leak);\n'

app() {
    pkg "$1/app" "$2" "$app_code"
    mv "$tmp/$1/app/lib.dr" "$tmp/$1/app/main.dr"
}

app plain '[dependencies]\nsq = "../sq"\n'
pkg plain/sq "$sq_opts" "$sq_code"
check "defaults are what a package starts from" plain 0 "  threads = \"multi\"" options
check "and the program is built with them" plain 0 "system, threads, sees system" run
check "-D KEY:name sets one option" plain 0 "vendored, threads, sees vendored" run -D sq:vendored
check "a program-wide define does not reach a package's option" plain 0 "system, threads, sees system" run -D vendored

app cfg '[dependencies]\nsq = "../sq"\n[config.sq]\nthreads = "off"\n'
pkg cfg/sq "$sq_opts" "$sq_code"
check "[config.KEY] configures a dependency" cfg 0 "system, no threads, sees system" run
check "-D wins over [config.KEY]" cfg 0 "system, threads" run -D sq:threads=multi

app deep '[dependencies]\na = "../a"\nsq = "../sq"\n'
pkg deep/a '[dependencies]\nsq = "../sq"\n[config.sq]\nvendored = true\n'
pkg deep/sq "$sq_opts" "$sq_code"
check "a dependency configures a package it shares with the root" deep 0 "vendored, threads, sees vendored" run

app clash '[dependencies]\na = "../a"\nb = "../b"\nsq = "../sq"\n'
pkg clash/a '[dependencies]\nsq = "../sq"\n[config.sq]\nthreads = "off"\n'
pkg clash/b '[dependencies]\nsq = "../sq"\n[config.sq]\nthreads = "single"\n'
pkg clash/sq "$sq_opts" "$sq_code"
check "two packages configuring a third two ways is a conflict" clash 1 "\`sq\` is configured two ways" deps
printf '[config.sq]\nthreads = "multi"\n' >>"$tmp/clash/app/mind.toml"
check "which the root settles by saying" clash 0 "threads = \"multi\"" options

app typo '[dependencies]\nsq = "../sq"\n[config.sq]\nvendord = true\n'
pkg typo/sq "$sq_opts" "$sq_code"
check "an option nobody declared is refused" typo 1 "\`sq\` has no option \`vendord\`" deps
check "with what is declared" typo 1 "vendored (bool, default false)  build from source" deps
check "a value of the wrong type is refused" plain 1 "\`cache_mb\` is integer, and \"lots\" is not" deps -D sq:cache_mb=lots
check "a choice outside the list is refused" plain 1 "\`threads\` is one of \"off\", \"single\", \"multi\"" deps -D sq:threads=all
check "a key that names no package is refused" plain 1 "-D sqlite:vendored: no package here is called \`sqlite\`" deps -D sqlite:vendored

app nokey '[dependencies]\nsq = "../sq"\n[config.sqlite]\nvendored = true\n'
pkg nokey/sq "$sq_opts" "$sq_code"
check "so is a [config.KEY] for one" nokey 1 "[config.sqlite] in mind.toml: no package here is called \`sqlite\`" deps

# A dependency that hangs on an option: `sys` is only there when `sq` is not
# vendored. Turning it off takes a second walk, since the walk that finds `sq`
# has not yet seen who configures it.
app cond '[dependencies]\nsq = "../sq"\n'
pkg cond/sq "$sq_opts"'[dependencies]\nsys = { path = "../sys", when = "not vendored" }\n' "$sq_code"
pkg cond/sys ''
check "a conditional dependency is there when its condition holds" cond 0 "  sys v1.0.0" deps
check "and gone when it does not" cond 0 "  sq v1.0.0" deps -D sq:vendored
out=$(cd "$tmp/cond/app" && "$dream" "$mind" deps -D sq:vendored 2>&1)
case "$out" in *sys*) echo "FAIL a dependency that is off is not listed:"; echo "$out" | sed 's/^/    /'; fail=$((fail + 1)) ;;
               *) echo "ok   a dependency that is off is not listed"; pass=$((pass + 1)) ;; esac
printf '[config.sq]\nvendored = true\n' >>"$tmp/cond/app/mind.toml"
out=$(cd "$tmp/cond/app" && "$dream" "$mind" deps 2>&1)
case "$out" in *sys*) echo "FAIL a condition settled by configuration takes a second walk:"; echo "$out" | sed 's/^/    /'; fail=$((fail + 1)) ;;
               *) echo "ok   a condition settled by configuration takes a second walk"; pass=$((pass + 1)) ;; esac
check "and [config.KEY] for the package that is off is not a typo" cond 0 "sq v1.0.0" deps

app badcond '[dependencies]\nsq = { path = "../sq", when = "not (vendored" }\n'
pkg badcond/sq "$sq_opts" "$sq_code"
check "a condition that does not parse is reported" badcond 1 "is not closed" deps

# Profiles.
app prof '[dependencies]\nsq = "../sq"\n[config.sq]\nthreads = "off"\n[profile.release.config.sq]\nthreads = "multi"\n[profile.fast]\ndefines = ["fast"]\n'
pkg prof/sq "$sq_opts" "$sq_code"
check "the default profile is debug" prof 0 "  profile  debug" info
check "a profile configures a dependency" prof 0 "system, threads" run --release
check "and the default build does not see it" prof 0 "system, no threads" run
check "a profile's defines reach the compiler" prof 0 "-D fast" info --profile fast
check "a profile nobody declared is refused" prof 1 "no profile is called \`nope\`; this project has debug, release, fast" deps --profile nope

# The root's own options are scoped to it, set by its defines and by -D.
app own '[options]\nvendored = false\n[dependencies]\nsq = "../sq"\n'
pkg own/sq "$sq_opts" "$sq_code"
check "the root's options are passed under its name" own 0 "-D app:vendored=false" info
check "and a plain -D of one sets it rather than defining a flag" own 0 "-D app:vendored=true" info -D vendored

echo
echo "$pass option cases passed, $fail failed"
[ "$fail" -eq 0 ]
