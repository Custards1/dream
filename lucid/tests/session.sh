#!/bin/sh
# One conversation with the language server, from `initialize` to `exit`.
#
# The unit tests say that a position converts and that a header parses. What
# they cannot say is that the whole thing answers an editor: that a buffer which
# was never written to disk is the program the compiler sees, that a definition
# in another file is found, and that a diagnostic is withdrawn when the code it
# complained about is fixed. Those are properties of the server as a running
# program, and this is the only place they are checked.
set -u

dream=${dream:-build-dream/bin/dream}
dreams=${dreams:-build/dreams.dream}
image=${image:-/tmp/lucid-session.dream}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fail=0
say_fail() { echo "FAIL: $*"; fail=1; }

"$dream" "$dreams" lucid/main.dr -L mind -L . -o "$image" >/dev/null 2>&1 || exit 1

# --- a workspace, and a buffer that disagrees with it -----------------------

mkdir -p "$tmp/ws"
cat > "$tmp/ws/mind.toml" <<'TOML'
[package]
name = "ws"
version = "0.1.0"
src = "."
TOML
cat > "$tmp/ws/helper.dr" <<'DREAM'
let double n = n * 2;
let triple n = n * 3;
DREAM
cat > "$tmp/ws/app.dr" <<'DREAM'
import std.console;
import helper;

let main! = {
    console.print! (to_string (helper.double 21))
};
DREAM

uri="file://$tmp/ws/app.dr"

# `Content-Length` counts bytes, and everything here is ASCII.
msg() { printf 'Content-Length: %d\r\n\r\n%s' "$(printf '%s' "$1" | wc -c)" "$1"; }

# The buffer names a member that does not exist. It is never written to disk,
# so a server reading the file rather than the buffer sees nothing wrong.
broken='import std.console;\nimport helper;\n\nlet main! = {\n    console.print! (to_string (helper.missing 21))\n};\n'

# The buffer at the moment a completion is wanted: a dot with nothing after it.
# This does not parse, which is the whole point -- the server has to repair it
# before it can say anything at all.
mid_edit='import std.console;\nimport helper;\n\nlet main! = {\n    console.print! (to_string (helper.))\n};\n'

{
  msg '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":"file://'"$tmp"'/ws"}}'
  msg '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$uri"'","text":"import std.console;\nimport helper;\n\nlet main! = {\n    console.print! (to_string (helper.double 21))\n};\n"}}}'
  msg '{"jsonrpc":"2.0","id":2,"method":"textDocument/definition","params":{"textDocument":{"uri":"'"$uri"'"},"position":{"line":4,"character":38}}}'
  msg '{"jsonrpc":"2.0","id":3,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$uri"'"},"position":{"line":4,"character":38}}}'
  msg '{"jsonrpc":"2.0","id":4,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"'"$uri"'"}}}'
  msg '{"jsonrpc":"2.0","id":6,"method":"textDocument/completion","params":{"textDocument":{"uri":"'"$uri"'"},"position":{"line":4,"character":4}}}'
  msg '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"'"$uri"'"},"contentChanges":[{"text":"'"$mid_edit"'"}]}}'
  msg '{"jsonrpc":"2.0","id":7,"method":"textDocument/completion","params":{"textDocument":{"uri":"'"$uri"'"},"position":{"line":4,"character":38}}}'
  msg '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"'"$uri"'"},"contentChanges":[{"text":"'"$broken"'"}]}}'
  msg '{"jsonrpc":"2.0","id":5,"method":"shutdown","params":{}}'
  msg '{"jsonrpc":"2.0","method":"exit","params":{}}'
} > "$tmp/in"

MIND_STDLIB=${MIND_STDLIB:-mind} "$dream" "$image" < "$tmp/in" > "$tmp/out" 2> "$tmp/err"
status=$?

[ "$status" -eq 0 ] || say_fail "the server exited $status"
[ -s "$tmp/err" ] && say_fail "the server wrote to stderr: $(head -1 "$tmp/err")"

has() {
    if grep -q "$2" "$tmp/out"; then :; else say_fail "$1"; fi
}

has "it does not announce what it can do"        '"definitionProvider":true'
has "an opened document gets no diagnostics"     '"diagnostics":\[\]'
has "a definition in another file is not found"  'helper\.dr'
has "hover does not say where the name came from" 'of module .ws.helper'
has "the outline is missing"                     '"name":"main!"'
# The buffer, not the file: `missing` exists in neither, but only the buffer
# ever mentions it.
has "an unsaved edit is not what gets compiled"  'has no member .missing'
has "shutdown is not answered"                   '"id":5'

# Completion, asked twice. Request 6 sits on a bare name in a buffer that
# parses, and must reach what a bare name reaches.
has "a bare name offers no global of this module" '"label":"main!"'
has "a bare name offers no keyword"               '"label":"match"'

# Request 7 is the one that matters. Its buffer is `helper.` with nothing after
# the dot, which does not parse -- so there is no module, no environment and no
# global table to answer out of until the text is repaired. `triple` is only
# reachable through `helper`, and it is not written anywhere in the buffer, so
# finding it here is proof that the repaired text was analysed and that the
# chain in front of the cursor was resolved to the module it names.
has "a mid-edit buffer is never answered"         '"id":7'
has "a bare dot does not reach the module"        '"label":"triple"'
# And a member is offered with the parameters it was declared with, which is
# most of what a completion list is for.
has "a member comes without its parameters"       '"detail":"double n"'

if [ "$fail" -eq 0 ]; then
    echo "the language server answers a whole conversation"
fi
exit "$fail"
