#!/bin/sh
# The library against a real server: a throwaway cluster, made for this run
# and removed after it.
#
# The cluster listens on 127.0.0.1 at a free port, and its pg_hba.conf gives
# each login method a user of its own -- `u_md5`, `u_scram`, `u_clear` -- so
# every way `pg.db` can authenticate is exercised against the server's own
# implementation of it. `live.dr` is the suite.
#
# PostgreSQL is not a dependency of this repository, so a machine without it
# skips this rather than failing: the binaries are found through `pg_config`,
# then in the usual Debian and Homebrew places, and `PG_BIN` overrides both.
# As root the server is run as the `postgres` user, because it refuses to run
# as root.
set -u

dream=${dream:-build-dream/bin/dream}
dreams=${dreams:-build/dreams.dream}

find_bin() {
    if [ -n "${PG_BIN:-}" ]; then echo "$PG_BIN"; return; fi
    if command -v pg_config >/dev/null 2>&1; then
        d=$(pg_config --bindir 2>/dev/null)
        if [ -x "$d/initdb" ]; then echo "$d"; return; fi
    fi
    for d in $(ls -d /usr/lib/postgresql/*/bin 2>/dev/null | sort -V -r) /usr/local/bin /opt/homebrew/bin; do
        if [ -x "$d/initdb" ] && [ -x "$d/pg_ctl" ]; then echo "$d"; return; fi
    done
}

bin=$(find_bin)
if [ -z "$bin" ]; then
    echo "pg live: skipped -- no PostgreSQL server binaries (set PG_BIN to their directory)"
    exit 0
fi
psql=$bin/psql
[ -x "$psql" ] || psql=$(command -v psql || true)
if [ -z "$psql" ]; then
    echo "pg live: skipped -- no psql to set the cluster up with"
    exit 0
fi

tmp=$(mktemp -d)
# A free port: ask the kernel for one, by binding port 0 and reading it back.
port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])' 2>/dev/null || echo 54321)

as_owner=""
if [ "$(id -u)" -eq 0 ]; then
    chown postgres "$tmp" 2>/dev/null || { echo "pg live: skipped -- running as root and there is no postgres user"; exit 0; }
    as_owner="runuser -u postgres --"
    command -v runuser >/dev/null 2>&1 || as_owner="su postgres -s /bin/sh -c"
fi

run_as_owner() {
    if [ -z "$as_owner" ]; then "$@"
    elif [ "$as_owner" = "su postgres -s /bin/sh -c" ]; then su postgres -s /bin/sh -c "$*"
    else $as_owner "$@"
    fi
}

cleanup() {
    run_as_owner "$bin/pg_ctl" -D "$tmp/data" -m immediate stop >/dev/null 2>&1
    rm -rf "$tmp"
}
trap cleanup EXIT

run_as_owner "$bin/initdb" -D "$tmp/data" -A trust -U postgres >"$tmp/initdb.log" 2>&1 || {
    echo "pg live: initdb failed"; sed 's/^/    /' "$tmp/initdb.log" | tail -5; exit 1; }

cat >"$tmp/data/pg_hba.conf" <<HBA
host all u_md5   127.0.0.1/32 md5
host all u_scram 127.0.0.1/32 scram-sha-256
host all u_clear 127.0.0.1/32 password
host all all     127.0.0.1/32 trust
local all all trust
HBA

run_as_owner "$bin/pg_ctl" -D "$tmp/data" -l "$tmp/server.log" -w \
    -o "-p $port -k $tmp -c listen_addresses=127.0.0.1 -c fsync=off" start >/dev/null 2>&1 || {
    echo "pg live: the server did not start"; sed 's/^/    /' "$tmp/server.log" | tail -5; exit 1; }

"$psql" -q -h 127.0.0.1 -p "$port" -U postgres -d postgres \
    -c "SET password_encryption = 'md5'; CREATE USER u_md5 PASSWORD 'md5pass';" \
    -c "SET password_encryption = 'scram-sha-256'; CREATE USER u_scram PASSWORD 'scrampass'; CREATE USER u_clear PASSWORD 'clearpass';" \
    -c "CREATE DATABASE app;" >"$tmp/setup.log" 2>&1 || {
    echo "pg live: could not set the cluster up"; sed 's/^/    /' "$tmp/setup.log"; exit 1; }

"$dream" "$dreams" pg/tests/live.dr -L mind -L . -o "$tmp/live.dream" >"$tmp/build.log" 2>&1 || {
    echo "pg live: the suite did not compile"; sed 's/^/    /' "$tmp/build.log" | head -10; exit 1; }

PG_TEST_PORT=$port timeout 300 "$dream" "$tmp/live.dream"
