#!/bin/sh
# What the compiler refuses: malformed SQL, written where a program writes SQL.
#
# `pg.sql` checks a statement twice over at compile time -- the `sql.query`
# macro lints its template while it expands, and a plain string handed to
# `db.query!` meets the `Statement` refinement, whose predicate the compiler
# runs on the literal. Neither can be tested from inside a program, because a
# program that fails them never exists; so each case here is a small program,
# compiled, and the diagnostic compared. Every accepted case is compiled too,
# so a check that refused everything would fail here as surely as one that
# refused nothing.
#
# Needs only the VM and a compiler: `dream` and `dreams` (an image), defaulting
# to the ones `just` builds. Run from the root of the repository.
set -u

dream=${dream:-build-dream/bin/dream}
dreams=${dreams:-build/dreams.dream}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pass=0
fail=0

# compile BODY -- the diagnostics of a program whose one function is BODY.
compile() {
    cat >"$tmp/case.dr" <<DR
import pg.db;
import pg.sql;
let run! conn a = $1;
let main! = ();
DR
    "$dream" "$dreams" "$tmp/case.dr" -L mind -L . -o "$tmp/case.dream" 2>&1
}

# refused BODY TEXT -- BODY must not compile, and say TEXT.
refused() {
    out=$(compile "$1")
    if [ $? -eq 0 ]; then
        echo "FAIL compiled, and should not have: $1"
        fail=$((fail + 1))
    elif ! printf '%s' "$out" | grep -qF -- "$2"; then
        echo "FAIL refused without saying \"$2\": $1"
        printf '%s\n' "$out" | sed 's/^/    /' | head -4
        fail=$((fail + 1))
    else
        pass=$((pass + 1))
    fi
}

# accepted BODY -- BODY must compile.
accepted() {
    out=$(compile "$1")
    if [ $? -ne 0 ]; then
        echo "FAIL refused, and should not have been: $1"
        printf '%s\n' "$out" | sed 's/^/    /' | head -4
        fail=$((fail + 1))
    else
        pass=$((pass + 1))
    fi
}

# A literal statement is held to the `Statement` refinement.
refused 'db.query! conn "SELEC 1"' 'should be `Query`'
refused 'db.query! conn "SELECT 1; DROP TABLE users"' 'should be `Query`'
refused 'db.query! conn "SELECT * FROM t WHERE id = $1"' 'should be `Query`'
refused "db.rows! conn \"SELECT 'unclosed\"" 'should be `Query`'
refused 'db.script! conn "CREATE TABLE t (a int"' 'should be `Script`'
accepted 'db.query! conn "SELECT 1;"'
accepted 'db.script! conn "CREATE TABLE t (a int); INSERT INTO t VALUES (1)"'
accepted 'db.query! conn "  with x as (select 1) select * from x"'

# The macro says what is wrong, where it is written.
refused 'expand sql.query "SELECT * FROM t WHERE a = {a + 1}"' 'must name a value'
refused "expand sql.query \"SELECT 'oops FROM t WHERE a = {a}\"" 'a string constant is never closed'
refused 'expand sql.query "DELETE FROM t WHERE id = $1"' 'is a numbered parameter'
refused 'expand sql.query "SELECT {a}; SELECT 2"' 'a query is one'
refused 'expand sql.query "SELECT (({a})"' 'is never closed'
refused 'expand sql.query "FROM t SELECT {a}"' 'does not begin an SQL statement'
refused 'expand sql.query "SELECT {a"' 'is never closed with'
refused 'expand sql.query a' 'expects a string literal'
refused 'expand sql.fragment "name = {a} AND note = '"'"'x"' 'a string constant is never closed'
refused 'expand sql.script "CREATE TABLE t (a int"' 'is never closed'
accepted 'expand sql.query "SELECT * FROM t WHERE a = {a} AND b @> '"'"'{1,2}'"'"' -- {not a placeholder}"'
accepted 'expand sql.query ["SELECT * FROM t WHERE a = ", a + 1, " AND b = ", a * 2]'
accepted 'expand sql.query "SELECT * FROM t WHERE {..a}"'
accepted 'expand sql.fragment "a = {a} AND ("'
accepted 'expand sql.query "SELECT $$ {braces} and ; inside $$, {a}"'

echo "pg contracts: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
