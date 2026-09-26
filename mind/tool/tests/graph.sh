#!/bin/sh
# The dependency graph, against packages on disk: cycles, diamonds, version
# requirements, build dependencies in their own namespace, two revisions of
# one repository, and the lock. Each case is a directory of manifests, one
# `mind deps`, and the first line of what it said.
set -u

dream=${dream:-build-dream/bin/dream}
mind=${mind:-build/mind}
root=$(pwd)
# Absolute, since each case runs from inside its own project.
case "$dream" in /*) ;; *) dream="$root/$dream" ;; esac
case "$mind" in /*) ;; *) mind="$root/$mind" ;; esac
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
export MIND_STDLIB="$root/mind" MIND_HOME="$tmp/home"

fail=0
pass=0

# pkg DIR VERSION EXTRA -- a package with one module and a manifest.
pkg() {
    mkdir -p "$tmp/$1/src"
    printf '[package]\nname = "%s"\nversion = "%s"\n%b' "$(basename "$1")" "$2" "$3" >"$tmp/$1/mind.toml"
    echo 'let x = 1;' >"$tmp/$1/src/lib.dr"
}

# check NAME CASE STATUS TEXT [COMMAND] -- run `mind COMMAND` in CASE/app.
check() {
    name=$1; dir=$2; want_status=$3; want=$4; cmd=${5:-deps}
    out=$(cd "$tmp/$dir/app" && "$dream" "$mind" $cmd 2>&1)
    got_status=$?
    case "$out" in
        *"$want"*) if [ "$got_status" = "$want_status" ]; then
                       echo "ok   $name"; pass=$((pass + 1)); return
                   fi ;;
    esac
    echo "FAIL $name: wanted $want_status and \"$want\", got $got_status:"
    echo "$out" | sed 's/^/    /' | head -8
    fail=$((fail + 1))
}

pkg cyc/app 0.1.0 '[dependencies]\na = "../a"\n'
pkg cyc/a 1.0.0 '[dependencies]\napp = "../app"\n'
check "a cycle is refused" cyc 1 "  a    uses  app  (../a/mind.toml)"

pkg bcyc/app 0.1.0 '[dependencies]\nsqlite = "../sqlite"\n'
pkg bcyc/sqlite 3.0.0 '[build-dependencies]\ncodegen = "../codegen"\n'
pkg bcyc/codegen 1.0.0 '[dependencies]\napp = "../app"\n'
check "a cycle through a build script is refused" bcyc 1 "sqlite   builds with  codegen"

pkg dia/app 0.1.0 '[dependencies]\na = "../a"\nb = { path = "../b", version = "^2" }\n'
pkg dia/a 1.0.0 '[dependencies]\nc = { path = "../c", version = "^1.2" }\n'
pkg dia/b 2.1.0 '[dependencies]\nc = { path = "../c", version = ">=1.4, <2" }\n'
pkg dia/c 1.5.0 ''
check "a diamond whose requirements agree is one package" dia 0 "  c v1.5.0"

pkg miss/app 0.1.0 '[dependencies]\na = "../a"\nc = { path = "../c", version = "^1.6" }\n'
pkg miss/a 1.0.0 '[dependencies]\nc = { path = "../c", version = "^1.2" }\n'
pkg miss/c 1.5.0 ''
check "a version nobody accepts names every requirement" miss 1 "(mind.toml)  <- not 1.5.0"

pkg inc/app 0.1.0 '[dependencies]\na = "../a"\nc = { path = "../c", version = "^2" }\n'
pkg inc/a 1.0.0 '[dependencies]\nc = { path = "../c", version = "^1.2" }\n'
pkg inc/c 1.5.0 ''
check "requirements no version meets are said to be" inc 1 "no version meets all of these"

pkg bare/app 0.1.0 '[dependencies]\nc = { path = "../c", version = "^1" }\n'
mkdir -p "$tmp/bare/c"
check "a bare directory has no version to require" bare 1 "has no manifest, so it has no version"

pkg badreq/app 0.1.0 '[dependencies]\nc = { path = "../c", version = "^one" }\n'
pkg badreq/c 1.0.0 ''
check "a requirement that is not one is reported" badreq 1 "is not one: \`^one\`"

pkg ns/app 0.1.0 '[dependencies]\njson = { path = "../json2", version = "^2" }\n[build-dependencies]\njson = { path = "../json1", version = "^1" }\n'
pkg ns/json1 1.0.0 ''
pkg ns/json2 2.0.0 ''
check "a build script's packages are a namespace of their own" ns 0 "[build: this project]"
check "the tree shows them under [build]" ns 0 "  [build]" tree

# Git, over file://, for the two cases only a repository can make.
mkdir -p "$tmp/repo"
(cd "$tmp/repo" && git init -q && printf '[package]\nname = "json"\nversion = "1.0.0"\n' >mind.toml \
    && echo 'let v = 1;' >lib.dr && git add . && git -c user.email=t@t -c user.name=t commit -qm one \
    && git tag v1.0 && echo 'let w = 2;' >>lib.dr \
    && git -c user.email=t@t -c user.name=t commit -qam two && git tag v1.1)
url="file://$tmp/repo"

pkg rev/app 0.1.0 "[dependencies]\na = \"../a\"\njson = \"$url#v1.1\"\n"
pkg rev/a 1.0.0 "[dependencies]\njson = \"$url#v1.0\"\n"
check "two revisions of one repository are said to be" rev 1 "two revisions of \`json\`"

pkg lock/app 0.1.0 "[dependencies]\njson = \"$url#v1.0\"\n"
check "a resolved graph is locked" lock 0 "json v1.0.0"
(cd "$tmp/repo" && echo 'let z = 3;' >>lib.dr && git -c user.email=t@t -c user.name=t commit -qam three \
    && git tag -f v1.0 >/dev/null)
rm -rf "$tmp/home/cache"
check "a moved tag disagrees with the lock" lock 1 "resolved to a different commit than mind.lock"
check "update accepts it" lock 0 "updated json" update
check "and the lock agrees again" lock 0 "json v1.0.0"

echo
echo "$pass graph cases passed, $fail failed"
[ "$fail" -eq 0 ]
