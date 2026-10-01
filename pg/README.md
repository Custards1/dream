# pg — PostgreSQL for Dream

A PostgreSQL client written in Dream: the version 3 wire protocol over the VM's
own sockets, with nothing to link against. It logs in with SCRAM-SHA-256, MD5
or a cleartext password, and supports parameterized queries, transactions,
savepoints, prepared statements, streaming, COPY, LISTEN/NOTIFY, cancellation
and a connection pool.

What makes it a Dream library is how statements are written. SQL text and the
values in it stay separate from the start. A malformed literal statement is a
compile error, and a row arrives as a map that a `mapping` record can read
directly.

```dream
import std.console;
import std.list;
import pg.db;
import pg.sql;

mapping User { id : :integer, name : :string, email : :string | :unit = () }

let main! = {
    let conn = db.connect! "postgres://ada@localhost/app";
    let min_id = 10;
    let users = db.query_as! User.type conn
        (expand sql.query "SELECT id, name, email FROM users WHERE id > {min_id} ORDER BY id");
    strict! (list.map (fn u -> console.print! (User.name u)) users)
    db.close! conn
};
```

To use it, list it in your project's `mind.toml`:

```toml
[dependencies]
pg = { path = "../dream/pg" }
```

The modules:

| Module | What it is |
|---|---|
| `pg.db` | A connection and everything done over one. |
| `pg.sql` | Statements as fragments, the `sql.query` macro, and statement builders. |
| `pg.pool` | A pool of connections shared by processes (a `std.server`). |
| `pg.errors` | Failures as conditions you can match on (`:unique_violation`, ...). |
| `pg.config` | `Config`, read from URLs, keyword strings and `PG*` variables. |
| `pg.value` | Converting between Dream values and PostgreSQL text. |
| `pg.wire`, `pg.crypto`, `pg.bytes` | The protocol, SCRAM/MD5, and byte arithmetic. |

## Writing statements

### `expand sql.query`: SQL with names in it

```dream
let by_email! email conn =
    db.first! conn (expand sql.query "SELECT * FROM users WHERE email = {email}");
```

The macro runs at compile time. Each `{name}` becomes a parameter (`$1`, `$2`,
...) holding the value of `name` where the macro is written. The value is sent
next to the statement and is never placed inside its text. A placeholder is a
local name or a module member (`{config.limit}`). For anything more complex,
pass a list instead: string literals are SQL, and every other element is a
value.

```dream
expand sql.query ["SELECT * FROM t WHERE score > ", base * 2, " AND kind = ", kind]
```

Braces inside SQL string constants, quoted identifiers, comments and
dollar-quoted bodies are left alone, so `'{1,2}'::int[]` keeps its meaning.
`{..name}` splices in a fragment built elsewhere. `expand sql.fragment` builds
a piece of a statement:

```dream
let recent = expand sql.fragment "created_at > now() - {age}::interval";
db.rows! conn (expand sql.query "SELECT * FROM events WHERE {..recent} AND kind = {kind}")
```

The macro also lints the SQL while it expands. Each of these is a compile error
at the `expand`:

- a string, quoted identifier, comment or dollar-quoted body that is never
  closed;
- unbalanced parentheses;
- a second statement;
- a hand-written `$1`;
- a first word that cannot begin a statement (`SELEC`).

### Plain strings are checked too

A statement with no parameters can be an ordinary string. `db.query!` takes a
`sql.Query`, which is a fragment or a `sql.Statement`. A `Statement` is
`:string where` the same lint passes, so the compiler checks string literals at
every call site:

```
db.query! conn "SELEC 1"
error: argument 2 of `db.query!` should be `Query`, but `"SELEC 1"` is not:
       a `where` it has to satisfy answered `false` at compile time
```

The lint only checks SQL's lexical shape. It does not know whether a column
exists; the server reports that. `sql.problem "..."` returns the reason a
string fails the lint.

`db.script!` runs several statements through the simple protocol (useful for
migrations) and takes a `sql.Script`. `expand sql.script` checks a script
literal and gives the specific reason when it fails.

### Building statements from data

A row is a map from column to value, which is what a `mapping` record is:

```dream
sql.insert "users" (User.new 1 "ada")                     // INSERT INTO "users" ("id", "name", "email") VALUES ($1, $2, DEFAULT)
sql.insert_all "users" rows                                // one INSERT, many rows
sql.update "users" %{ :email => e } %{ :id => 1 }          // UPDATE .. SET "email" = $1 WHERE "id" = $2
sql.delete "users" %{ :id => 1 }
sql.select "users" [:id, :name] %{ :active => true }
sql.returning [:id] (sql.insert "users" row)
sql.where (sql.or [sql.eq :a 1, sql.eq :b ()])             // WHERE ("a" = $1) OR ("b" IS NULL)
```

In an insert, `()` means `DEFAULT`, so a record whose `id` defaults to `()`
gets its value from the `serial`. In an update, `()` means NULL. The builders
quote identifiers (`sql.ident`, `sql.qualified`), and `"schema.table"` is
quoted one part at a time. `sql.concat`, `sql.join`, `sql.params`, `sql.and`,
`sql.or` and `sql.negate` combine fragments, and parameters are numbered only
when the whole statement is rendered. `sql.raw` is the one way to write
unchecked text, and it is meant for text the program wrote itself.
`sql.text_with "... $1 ..." [v]` turns numbered SQL from elsewhere into a
fragment, which can then be combined with others.

## Running them

| `pg.db` | Returns |
|---|---|
| `query! conn q` | a `Result`: `rows`, `tuples`, `columns`, `command`, `count`, `status`, `notices`, `notifications` |
| `rows! conn q` | the rows, as maps |
| `one! conn q` | the one row; `:no_rows` / `:too_many_rows` otherwise |
| `first! conn q` | the first row, or `()` |
| `scalar! conn q` | the first column of the first row, or `()` |
| `column! conn q` | the first column of every row |
| `exec! conn q` | the number of rows affected |
| `query_as! T conn q` | the rows, each checked against the type `T` |
| `script! conn text` | a `Result` per statement |
| `fold! conn q n f init` / `each! conn q n f!` | reads the rows `n` at a time through a portal, so memory use stays constant |
| `prepare!` / `execute!` / `deallocate!` | named prepared statements |
| `copy_in! conn table columns rows` / `copy_out! conn "COPY .. TO STDOUT"` | COPY in text format |
| `listen!` / `notify!` / `next_notification!` / `subscribe!` | LISTEN/NOTIFY; `subscribe!` forwards notifications to a process |
| `cancel! conn` | asks the server to stop the statement `conn` is running |

Each query is one round trip: Parse, Bind, Describe, Execute and Sync are
written together. A failed statement is raised only after the server is ready
again, so the connection stays usable.

### Rows

Read a row by **atom**: `row.[:email]`. A column is keyed by an atom when the
program has an atom with that name, and by its name as a string otherwise.
Dream never creates atoms from text it receives (see `to_existing_atom`).
Writing `:email` anywhere in the program creates that atom, so reading by atom
always works. String keys only appear for columns the program never names.
Because of this, a `mapping` whose fields are the columns can read a row
directly.

Values are decoded by column type:

| PostgreSQL | Dream |
|---|---|
| integers, `oid` | integer (an `int8` too large for a fixnum stays text) |
| `float4`, `float8` | float |
| `numeric` | integer if it is a whole number that fits, otherwise its exact text |
| `bool` | bool |
| `json`, `jsonb` | the parsed value |
| `bytea` | its bytes, as a string |
| arrays | lists, nested |
| NULL | `()` |
| anything else (dates, `uuid`, ...) | the text the server sent |

Rows are decoded only when they are read. `Config.decoders` adds decoders by
type OID. For parameters, an integer is sent as `int8`, a list as an array, a
map as JSON and an atom as its name. Anything else is sent untyped so that the
server infers the type. `value.json`, `value.jsonb`, `value.bytea` and
`value.typed oid text` set the type explicitly.

## Transactions

The body is a function that takes the connection. It can be a lambda written
inside impure code, or an impure function whose last parameter is the
connection, applied to its other arguments:

```dream
db.transaction! conn (fn c -> db.exec! c "DELETE FROM sessions")

let transfer! from to amount conn = {
    db.exec! conn (expand sql.query "UPDATE account SET balance = balance - {amount} WHERE id = {from}")
    db.exec! conn (expand sql.query "UPDATE account SET balance = balance + {amount} WHERE id = {to}")
};

db.transaction! conn (transfer! 1 2 100)
db.transaction_with! conn %{ :isolation => :repeatable_read, :read_only => true } report!
db.serializable! conn 5 (transfer! 1 2 100)      // retried on serialization failure or deadlock
db.savepoint! conn risky!                        // nested; a failure undoes only this part
```

If the body raises, the transaction rolls back and the error is raised again.
The body's result is fully forced before COMMIT, so no lazy part of it runs
after the transaction has ended.

## Errors

Every failure has kind `:postgres`, and its payload contains a `:condition`:

```dream
try! { db.exec! conn (sql.insert "users" user) } catch e {
    match errors.condition e {
        :unique_violation => 0,
        _ => raise! e,
    }
}
```

For server errors, the condition is the SQLSTATE's name (an unlisted code gets
its class's name). The payload also holds the server's fields: `:code`,
`:message`, `:detail`, `:hint`, `:constraint`, `:table`, and others.
Client-side failures use `:connection_failure`, `:no_rows`, `:too_many_rows`,
`:bad_config` and `:unsupported_authentication`. `errors.is_retryable` and
`errors.is_connection_lost` answer the two common questions, and
`errors.describe` formats an error as one line.

## The pool

```dream
let p = pool.start! "postgres://app@db/app" 10;
pool.rows! p "SELECT * FROM users"
pool.with! p (report! month)            // exclusive use of one connection
pool.transaction! p (transfer! 1 2 100)
```

A pool is a `std.server` process that lends connections out; queries never go
through it. Connections are opened lazily, by the caller that needs one. When
none is free, callers wait in order. When a body raises, its connection is
rolled back before going back into the pool. A connection that was lost is
closed, and its slot is freed. `pool.child!` makes the pool a supervised child.

## Configuration

`db.connect!` and `pool.start!` accept a `config.Config` map (missing fields
take their defaults), a URL such as
`postgres://user:pass@host:5432/db?application_name=x&search_path=s`, or a
libpq keyword string such as `host=h dbname=d`. `config.from_env!` reads
`DATABASE_URL` and the `PG*` variables. Any parameter the client does not
recognize, such as `search_path`, `statement_timeout` or `TimeZone`, is sent to
the server at startup.

## Limits

- **No TLS.** The VM's sockets are plain TCP. `sslmode=require` and stricter
  settings are refused rather than connecting without encryption. Use a
  trusted network, an SSH tunnel, or a TLS proxy.
- **No Unix-domain sockets**, for the same reason.
- **No Kerberos, GSSAPI or SSPI.**
- SCRAM passwords are not SASLprep-normalized. That only matters for passwords
  with unusual Unicode characters.
- A connection serves one process at a time; to share connections, use the
  pool.

## Tests

```
just test-pg            # or: mind test pg
```

This runs three groups:

- **Units**: every module's `when test` block. These need no server, and
  include the RFC test vectors for SHA-256/HMAC/PBKDF2/SCRAM/MD5/base64.
- **Contracts** (`tests/contracts.sh`): checks that the compiler rejects
  malformed SQL and accepts valid SQL.
- **Live** (`tests/live.sh`): runs the whole library against a temporary
  cluster that uses all four login methods. If no PostgreSQL is installed, this
  group is skipped.

`examples/tasks.dr` is a complete small program.
