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

# A buffer with a macro in it, and one with a dot on a host module. Both are
# questions the compiler alone cannot answer. Expansion *replaces* the call, so
# the name `twice` is in no tree the resolver ever walks; and a host module's
# members belong to the runtime, which is the VM this server is running on.
macros='import std.console;\nimport std.vm;\n\nmacro twice e = [:binary, :add, e, e, [0, 0]];\n\nlet double n = expand twice n;\n\nlet main! = {\n    console.print! (double 21)\n};\n'
host_dot='import std.console;\nimport std.vm;\n\nmacro twice e = [:binary, :add, e, e, [0, 0]];\n\nlet double n = expand twice n;\n\nlet main! = {\n    console.print! (vm.)\n};\n'

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
  msg '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"'"$uri"'"},"contentChanges":[{"text":"'"$macros"'"}]}}'
  msg '{"jsonrpc":"2.0","id":8,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$uri"'"},"position":{"line":5,"character":22}}}'
  msg '{"jsonrpc":"2.0","id":9,"method":"textDocument/definition","params":{"textDocument":{"uri":"'"$uri"'"},"position":{"line":5,"character":22}}}'
  msg '{"jsonrpc":"2.0","id":10,"method":"textDocument/completion","params":{"textDocument":{"uri":"'"$uri"'"},"position":{"line":5,"character":27}}}'
  msg '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"'"$uri"'"},"contentChanges":[{"text":"'"$host_dot"'"}]}}'
  msg '{"jsonrpc":"2.0","id":11,"method":"textDocument/completion","params":{"textDocument":{"uri":"'"$uri"'"},"position":{"line":8,"character":23}}}'
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

# One response, by the id it answers. The stream is a run of `Content-Length:`
# headers each followed by its body, so splitting on the header is splitting on
# the message -- which is what lets a test say "not in *this* list" without
# every other list in the conversation answering for it.
response() { awk -v RS='Content-Length:' -v id="\"id\":$1" 'index($0, id) { print }' "$tmp/out"; }
has_in() {
    if response "$2" | grep -q "$3"; then :; else say_fail "$1"; fi
}
lacks_in() {
    if response "$2" | grep -q "$3"; then say_fail "$1"; else :; fi
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

# Requests 8 to 11 are about the two things the resolved program does not say.
#
# `twice` at its call site is a name expansion consumed: it was resolved, and
# then the call it belonged to was replaced by what the macro returned. What
# holds these is that the pass which did resolve it writes the occurrence down
# (`modules.expansions`) and the resolver seeds its table with them, so hover
# and go-to-definition are the ordinary lookups and not a second mechanism.
has_in "a macro's name at its call site says nothing"     8  'a macro of module'
has_in "a macro's name does not lead to its declaration" 9  '"line":3'
# Only a macro may be written after `expand`, so nothing else is offered --
# `double` is a global of this very module and must not be in that list.
has_in   "a name after expand offers a macro"                10 '"label":"twice"'
lacks_in "a name after expand offers what cannot go there"   10 '"label":"double"'
# A host module's members belong to the runtime, which is the VM this is
# running on. Nothing was offered here at all before it was asked.
has_in "a host module offers no members"                 11 '"label":"stats!"'
has_in "a host member comes without its arity"           11 '"detail":"stats! (1 argument)"'

if [ "$fail" -eq 0 ]; then
    echo "the language server answers a whole conversation"
fi
exit "$fail"
