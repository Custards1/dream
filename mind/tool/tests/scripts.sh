#!/bin/sh
# Build scripts, run by `mind` against packages on disk: that a package with
# none is built as before and nothing is written for it; that a script's
# generated modules, payloads and defines reach the compile, a dependency's
# scoped to it; that a second build reuses every answer, and editing one
# package's script runs that one again and no other; that a tool comes from
# `[build-dependencies]` and a variable only from `[build] env`; and that a
# script that fails says what failed.
set -u

dream=${dream:-build-dream/bin/dream}
mind=${mind:-build/mind}
dreams=${dreams:-build/dreams.dream}
root=$(pwd)
# Absolute, since each case runs from inside its own project.
case "$dream" in /*) ;; *) dream="$root/$dream" ;; esac
case "$mind" in /*) ;; *) mind="$root/$mind" ;; esac
case "$dreams" in /*) ;; *) dreams="$root/$dreams" ;; esac
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
export MIND_STDLIB="$root/mind" MIND_HOME="$tmp/home" DREAM="$dream"

fail=0
pass=0

# check NAME WANT_STATUS TEXT -- what the last `run` said.
check() {
    case "$out" in
        *"$3"*) if [ "$status" = "$2" ]; then echo "ok   $1"; pass=$((pass + 1)); return; fi ;;
    esac
    echo "FAIL $1: wanted $2 and \"$3\", got $status:"
    echo "$out" | sed 's/^/    /' | head -8
    fail=$((fail + 1))
}

# run DIR ARGS -- `mind run` in DIR, with the compiler under test.
run() {
    dir=$1; shift
    out=$(cd "$tmp/$dir" && "$dream" "$mind" run --compiler "$dreams" "$@" 2>&1)
    status=$?
}

# stamp FILE -- when it was last written, or "none".
stamp() { if [ -f "$1" ]; then ls -l --time-style=+%s.%N "$1" | awk '{print $6}'; else echo none; fi; }

mkdir -p "$tmp/plain"
printf '[package]\nname = "plain"\n' >"$tmp/plain/mind.toml"
printf 'import std.console;\nlet main! = console.print! "plain";\n' >"$tmp/plain/main.dr"
run plain
check "a package with no script builds as it always did" 0 "plain"
if [ -e "$tmp/plain/target/build" ]; then
    echo "FAIL and nothing is written for a script it does not have"; fail=$((fail + 1))
else
    echo "ok   and nothing is written for a script it does not have"; pass=$((pass + 1))
fi

mkdir -p "$tmp/p/app" "$tmp/p/lib" "$tmp/p/gen"
printf '[package]\nname = "gen"\nversion = "1.0.0"\n' >"$tmp/p/gen/mind.toml"
printf 'let line name = "let made_for = \\"" + name + "\\";";\n' >"$tmp/p/gen/source.dr"
printf '[package]\nname = "lib"\nversion = "0.2.0"\n' >"$tmp/p/lib/mind.toml"
cat >"$tmp/p/lib/build.dr" <<'EOF'
import std.build;
let plan ctx = build.empty |> build.define "tuned";
EOF
cat >"$tmp/p/lib/greet.dr" <<'EOF'
when tuned { let how = "tuned"; }
when not tuned { let how = "untuned"; }
EOF
cat >"$tmp/p/app/mind.toml" <<'EOF'
[package]
name = "app"
version = "1.4.0"

[build]
env = ["APP_GREETING"]

[dependencies]
lib = "../lib"

[build-dependencies]
gen = "../gen"
EOF
cat >"$tmp/p/app/build.dr" <<'EOF'
import std.build;
import gen.source;

let plan ctx =
    build.empty
    |> build.module "info" (build.write "info.dr" (source.line (build.Context.package ctx)
                                                   + "\nlet version = \"" + build.version ctx + "\";"
                                                   + "\nlet greeting = \"" + (build.Context.env ctx).["APP_GREETING" else "none"]
                                                   + "\";"))
    |> build.payload "notes" (build.write "notes.txt" "carried");
EOF
cat >"$tmp/p/app/main.dr" <<'EOF'
import std.console;
import app.info;
import lib.greet;
let main! = console.print! (info.made_for + " " + info.version + " " + greet.how + " "
                            + info.greeting + " " + to_string (data_count ()));
EOF

APP_GREETING=hi run p/app
check "generated modules, payloads, a tool and a dependency's define reach the compile" 0 "app 1.4.0 tuned hi 1"
a=$(stamp "$tmp/p/app/target/build/app/record")
l=$(stamp "$tmp/p/app/target/build/lib/record")

APP_GREETING=hi run p/app
if [ "$(stamp "$tmp/p/app/target/build/app/record")$(stamp "$tmp/p/app/target/build/lib/record")" = "$a$l" ]; then
    echo "ok   a second build runs no script"; pass=$((pass + 1))
else
    echo "FAIL a second build ran a script again"; fail=$((fail + 1))
fi

printf '\nlet unused = 1;\n' >>"$tmp/p/lib/build.dr"
APP_GREETING=hi run p/app
if [ "$(stamp "$tmp/p/app/target/build/app/record")" = "$a" ] && [ "$(stamp "$tmp/p/app/target/build/lib/record")" != "$l" ]; then
    echo "ok   editing one package's script runs that one alone"; pass=$((pass + 1))
else
    echo "FAIL editing lib's script did not run exactly lib's"; fail=$((fail + 1))
fi

APP_GREETING=bye run p/app
check "a variable the manifest lists is part of the context" 0 "app 1.4.0 tuned bye 1"

cat >"$tmp/p/lib/build.dr" <<'EOF'
import std.build;
import std.build.command;
let plan ctx = build.empty |> build.payload "x" (command.make "broken" "sh" ["-c", "echo it went wrong >&2; exit 3", command.out "x"]);
EOF
run p/app
check "a failing script says which step and what it printed" 1 "it went wrong"

echo
echo "$pass script cases passed, $fail failed"
[ "$fail" -eq 0 ]
