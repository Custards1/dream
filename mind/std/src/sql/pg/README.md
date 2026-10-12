# std.sql.pg, PostgreSQL for Dream

A PostgreSQL client written in Dream: the version 3 wire protocol over the
VM's own sockets, with nothing to link against. It encrypts with `std.tls`,
logs in with SCRAM-SHA-256, MD5 or a cleartext password, and supports
parameterized queries, transactions, savepoints, prepared statements,
streaming, COPY, LISTEN/NOTIFY, cancellation and a connection pool.

What makes it a Dream library is how statements are written. SQL text and the
values in it stay apart from the start, a malformed literal statement is a
**compile error**, and a row arrives as a map a `mapping` record reads
directly.

```dream
import std.console;
import std.list;
import std.sql.pg.db;
import std.sql.pg.sql;

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

It is part of the standard library, so there is nothing to add to a manifest:
import it. [`examples/tasks.dr`](examples/tasks.dr) is a complete small
program.

## Two ways in

- **`std.sql.pg.db`** is PostgreSQL as itself: every feature above, results
  with PostgreSQL's own metadata, `$1`-style fragments from `std.sql.pg.sql`.
- **`std.sql.pg.driver`** is PostgreSQL as a [`std.sql`](../mod.dr)
  connection: the same API SQLite and any other driver offers, with its
  query builders, transactions, migrations and portable error codes.

```dream
import std.sql;
import std.sql.pg.driver;

let main! = sql.using! (driver.open! "postgres://ada@localhost/app") (fn conn -> {
    let min_id = 10;
    sql.query! conn (expand sql.query
        "SELECT id, name FROM users WHERE id > {min_id} ORDER BY id")
});
```

Use the driver for code that should not care which database it is talking
to, and `db` for the parts that are about PostgreSQL. [Through
`std.sql`](#through-stdsql) has the differences.

| Module | |
|---|---|
| `std.sql.pg.db` | a connection, and everything done over one |
| `std.sql.pg.sql` | statements as fragments, the `sql.query` macro, and builders |
| `std.sql.pg.driver` | PostgreSQL as a `std.sql.Connection` |
| `std.sql.pg.pool` | a pool of connections shared by processes (a `std.server`) |
| `std.sql.pg.errors` | failures as conditions to match on (`:unique_violation`, ...) |
| `std.sql.pg.config` | `Config`, from URLs, keyword strings and `PG*` variables |
| `std.sql.pg.value` | Dream values to PostgreSQL text and back |
| `std.sql.pg.wire`, `.crypto`, `.bytes` | the protocol, SCRAM and MD5, and byte arithmetic |

## Writing statements

### `expand sql.query`: SQL with names in it

```dream
let by_email! email conn =
    db.first! conn (expand sql.query "SELECT * FROM users WHERE email = {email}");
```

The macro runs at compile time. Each `{name}` becomes a parameter (`$1`,
`$2`, ...) holding the value `name` has where the macro is written; the value
is sent beside the statement and never placed inside its text. A placeholder
is a local name or a module member (`{config.limit}`). For anything more
complex, pass a list: string literals are SQL, and every other element is a
value.

```dream
expand sql.query ["SELECT * FROM t WHERE score > ", base * 2, " AND kind = ", kind]
```

Braces inside string constants, quoted identifiers, comments and
dollar-quoted bodies are left alone, so `'{1,2}'::int[]` keeps its meaning.
`{..name}` splices a fragment built elsewhere, and `expand sql.fragment`
builds one:

```dream
let recent = expand sql.fragment "created_at > now() - {age}::interval";
db.rows! conn (expand sql.query "SELECT * FROM events WHERE {..recent} AND kind = {kind}")
```

The macro lints the SQL as it expands, and each of these is a compile error at
the `expand`: a string, identifier, comment or dollar-quoted body never
closed; unbalanced parentheses; a second statement; a hand-written `$1`; a
first word that cannot begin a statement (`SELEC`).

### Plain strings are checked too

A statement with no parameters can be an ordinary string. `db.query!` takes a
`sql.Query` -- a fragment, or a `sql.Statement`, which is `:string where` the
same lint passes -- so the compiler checks a string literal at every call
site:

```
db.query! conn "SELEC 1"
error: argument 2 of `db.query!` should be `Query`, but `"SELEC 1"` is not:
       a `where` it has to satisfy answered `false` at compile time
```

The lint is lexical. Whether a column exists is the server's to say.
`sql.problem "..."` answers why a string fails it. `db.script!` runs several
statements through the simple protocol (for migrations) and takes a
`sql.Script`; `expand sql.script` checks a script literal and says what is
wrong with it.

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

In an insert `()` means `DEFAULT`, so a record whose `id` defaults to `()`
takes its value from the `serial`; in an update `()` means NULL. Identifiers
are quoted (`sql.ident`, `sql.qualified`), `"schema.table"` one part at a
time. `sql.concat`, `sql.join`, `sql.params`, `sql.and`, `sql.or` and
`sql.negate` combine fragments, and parameters are numbered only when the
whole statement is rendered. `sql.raw` is the one way to write unchecked text,
meant for text the program wrote itself; `sql.text_with "... $1 ..." [v]`
adopts numbered SQL from elsewhere as a fragment.

## Running them

| `std.sql.pg.db` | Answers |
|---|---|
| `query! conn q` | a `Result`: `rows`, `tuples`, `columns`, `command`, `count`, `status`, `notices`, `notifications` |
| `rows! conn q` | the rows, as maps |
| `one! conn q` | the one row; `:no_rows` or `:too_many_rows` otherwise |
| `first! conn q` | the first row, or `()` |
| `scalar! conn q` | the first column of the first row, or `()` |
| `column! conn q` | the first column of every row |
| `exec! conn q` | the number of rows affected |
| `query_as! T conn q` | the rows, each checked against the type `T` |
| `script! conn text` | a `Result` per statement |
| `fold! conn q n f init`, `each! conn q n f!` | the rows `n` at a time through a portal, in constant memory |
| `prepare!`, `execute!`, `deallocate!` | named prepared statements |
| `copy_in! conn table columns rows`, `copy_out! conn "COPY .. TO STDOUT"` | COPY, in text format |
| `listen!`, `notify!`, `next_notification!`, `subscribe!` | LISTEN/NOTIFY; `subscribe!` forwards notifications to a process |
| `cancel! conn` | ask the server to stop what `conn` is running |

A query is one round trip: Parse, Bind, Describe, Execute and Sync are written
together. A failed statement is raised only once the server is ready again,
so the connection stays usable.

### Rows

Read a row by **atom**: `row.[:email]`. A column is keyed by an atom when the
program has an atom of that name, and by its name as a string otherwise --
Dream never makes atoms from text it receives. Writing `:email` anywhere in
the program makes the atom, so reading by atom always works, and a `mapping`
whose fields are the columns reads a row directly.

| PostgreSQL | Dream |
|---|---|
| integers, `oid` | integer |
| `float4`, `float8` | float |
| `numeric` | an integer of any size when it is whole; its exact text otherwise |
| `bool` | bool |
| `json`, `jsonb` | the parsed value |
| `bytea` | its bytes, as a string |
| arrays | lists, nested |
| NULL | `()` |
| anything else (dates, `uuid`, ...) | the text the server sent |

Rows are decoded when they are read, and `Config.decoders` adds decoders by
type OID. As parameters, an integer is sent as `int8`, a list as an array, a
map as JSON and an atom as its name; anything else is sent untyped for the
server to infer. `value.json`, `value.jsonb`, `value.bytea` and
`value.typed oid text` say the type outright.

## Transactions

The body is a function of the connection: a lambda, or an impure function
whose last parameter is the connection, applied to the rest.

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
The body's result is forced completely before COMMIT, so no lazy part of it
runs after the transaction has ended. A retried body runs again whole, so its
effects outside the database may happen more than once.

## Errors

Every failure has kind `:postgres`, and its payload a `:condition`:

```dream
try! { db.exec! conn (sql.insert "users" user) } catch e {
    match errors.condition e {
        :unique_violation => 0,
        _ => raise! e,
    }
}
```

A server error's condition is its SQLSTATE's name (an unlisted code gets its
class's), and the payload carries the server's fields: `:code`, `:message`,
`:detail`, `:hint`, `:constraint`, `:table` and the rest. Failures on the
client side are `:connection_failure`, `:no_rows`, `:too_many_rows`,
`:bad_config`, `:unsupported_authentication`, `:ssl_refused` and the TLS
kinds below. `errors.is_retryable`, `errors.is_connection_lost` and
`errors.describe` answer the common questions.

## The pool

```dream
let p = pool.start! "postgres://app@db/app" 10;
pool.rows! p "SELECT * FROM users"
pool.with! p (report! month)            // exclusive use of one connection
pool.transaction! p (transfer! 1 2 100)
```

A pool is a `std.server` that lends connections out; queries never pass
through it. Connections open lazily, for the caller that needs one, and
callers wait in order when none is free. A connection whose body raised is
rolled back before it goes back; one that was lost is closed and its slot
freed. `pool.child!` makes the pool a supervised child.

## Configuration

`db.connect!` and `pool.start!` take a `config.Config` map (missing fields take
their defaults), a URL --
`postgres://user:pass@host:5432/db?application_name=x&search_path=s` -- or a
libpq keyword string, `host=h dbname=d`. `config.from_env!` reads
`DATABASE_URL` and the `PG*` variables. A parameter the client does not know,
such as `search_path`, `statement_timeout` or `TimeZone`, is sent to the
server at startup.

## TLS

`sslmode` means what it means to libpq:

| `sslmode` | Encrypted | Checked |
|---|---|---|
| `disable`, `allow` | no | -- |
| `prefer` (the default) | when the server offers it | nothing |
| `require` | yes, or the connection fails with `:ssl_refused` | nothing, unless roots are given (then as `verify-ca`) |
| `verify-ca` | yes | the certificate chains to a trusted root |
| `verify-full` | yes | that, and it names the host |

The trusted roots are `sslrootcert`, a PEM file, or the system's store when it
is not given. In a `Config` map they are `:ssl_root_cert` (a path) or
`:ssl_ca` (the PEM itself), with the mode as an atom:
`%{ :ssl_mode => :verify_full, :ssl_root_cert => "/etc/ca.pem" }`. A client
certificate is `sslidentity`, a **PKCS#12** file, with `sslpassword`;
revocation lists are `sslcrl`. libpq's PEM `sslcert` and `sslkey` are refused
with that advice, because PKCS#12 is the format every platform's TLS library
imports -- `openssl pkcs12 -export` converts. A certificate that fails is
reported by what was wrong: `:certificate_untrusted`, `:certificate_expired`,
`:certificate_revoked`, `:hostname_mismatch`. `db.tls_info! conn` says what
a connection negotiated, or `()` for plain text.

## Through `std.sql`

`driver.open!` takes the same configuration as `db.connect!`, and
`driver.wrap!` adapts an existing connection, including one borrowed inside
`pool.with!` -- closing the adapter closes the connection, so leave that to
the pool when borrowing. Use a connection from one process at a time, and run
no other query on it from inside a streaming callback.

The adapter is the whole `std.sql` API: builders, `RETURNING`, `exec_many!`
(each run of identical SQL prepared once), nested transactions, migrations,
and streaming `fold!`/`each!`, which close their portal if a callback raises.
What differs from `db`:

- `bytea` arrives tagged as `sql.blob bytes`, arrays' elements included, and
  blobs are sent with `sql.blob`; `db` uses raw bytes both ways.
- In the shared insert builders `()` is the column default and `sql.null` is
  NULL.
- `sql.positional text values` adopts `$1`-style SQL. There is no
  `:last_id`; ask for generated keys with `RETURNING`.
- Errors have kind `:sql_error`, with the shared codes (`:unique`,
  `:foreign_key`, `:serialization`, `:connection_lost`) from
  `sql.error_code`; `sql.error_field :state e` is the SQLSTATE, and the
  original PostgreSQL error is its `:cause`. An exception the program's own
  callback raised passes through unchanged.

Both use `std.sql.lint` to check statements. Use the shared builders and
macros with `std.sql`, and `std.sql.pg.sql`'s with `db`.

## Limits

- **No Unix-domain sockets.** The VM does not open them.
- **No Kerberos, GSSAPI or SSPI.**
- SCRAM passwords are not SASLprep-normalized, which matters only for
  passwords with unusual Unicode.
- A connection serves one process at a time; to share, use the pool.

## Tests

```
just test-pg
```

- **Units**: every module's `when test` block, with no server needed --
  including the RFC vectors for SHA-256, HMAC, PBKDF2, SCRAM, MD5 and base64.
- **Contracts** (`tests/contracts.sh`): the compiler refuses malformed SQL
  and accepts the rest.
- **Live** (`tests/live.sh`): the whole library against a throwaway cluster
  that uses all four login methods, skipped where PostgreSQL is not
  installed.

Under `mind test` the last two are the workspace's checks
`dream.sql_pg_contracts` and `dream.sql_pg_live`, and the units run with
the rest of `std`'s.
