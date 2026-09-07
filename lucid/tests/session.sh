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

dreamc=${dreamc:-target/debug/dreamc}
dream=${dream:-build-dream/bin/dream}
image=${image:-/tmp/lucid-session.dream}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fail=0
say_fail() { echo "FAIL: $*"; fail=1; }

"$dreamc" lucid/main.dr -L mind -L . -o "$image" >/dev/null || exit 1

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

{
  msg '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":"file://'"$tmp"'/ws"}}'
  msg '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$uri"'","text":"import std.console;\nimport helper;\n\nlet main! = {\n    console.print! (to_string (helper.double 21))\n};\n"}}}'
  msg '{"jsonrpc":"2.0","id":2,"method":"textDocument/definition","params":{"textDocument":{"uri":"'"$uri"'"},"position":{"line":4,"character":38}}}'
  msg '{"jsonrpc":"2.0","id":3,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$uri"'"},"position":{"line":4,"character":38}}}'
  msg '{"jsonrpc":"2.0","id":4,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"'"$uri"'"}}}'
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

if [ "$fail" -eq 0 ]; then
    echo "the language server answers a whole conversation"
fi
exit "$fail"
