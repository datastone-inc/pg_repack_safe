#!/usr/bin/env bash
# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# Show what REPACK (CONCURRENTLY) does to a transaction whose snapshot is
# older than the repack, on stock PostgreSQL 19 and then with pg_repack_safe.
#
# The script creates a throwaway server in a temporary directory, runs the
# four scenarios in this directory against it twice (first without the
# library, then with it preloaded and the extension created) and removes the
# server again. It touches no existing server or database.
#
# Requirements: PostgreSQL 19 binaries (pg_config on PATH, or PG_CONFIG set)
# and, for the second half, pg_repack_safe installed in that PostgreSQL
# (make install).
#
# Usage: demo/demo.sh
#   DEMO_PORT  port for the throwaway server (default 54319)
#
# Each scenario is a psql script; the other session's statements are run
# from inside it with \!, so the order of events is fixed.

set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
pg_config=${PG_CONFIG:-pg_config}
bindir=$("$pg_config" --bindir)
pkglibdir=$("$pg_config" --pkglibdir)
version=$("$pg_config" --version)

case "$version" in
    "PostgreSQL 19"*) ;;
    *) echo "demo.sh needs PostgreSQL 19, found: $version" >&2; exit 1 ;;
esac

export PATH="$bindir:$PATH"
DEMO_DIR=$(mktemp -d)
export DEMO_DIR
export PGHOST="$DEMO_DIR"
export PGPORT="${DEMO_PORT:-54319}"
export PGDATABASE=postgres
unset PGUSER PGPASSWORD PGSERVICE PGOPTIONS
datadir="$DEMO_DIR/data"

cleanup()
{
    pg_ctl -D "$datadir" -m immediate stop >/dev/null 2>&1 || true
    rm -rf "$DEMO_DIR"
}
trap cleanup EXIT

heading()
{
    printf '\n========================================================================\n'
    printf '%s\n' "$1"
    printf '========================================================================\n'
}

runScenarios()
{
    local scenario

    # Run from this directory, so that psql's error messages name the
    # scenario file without its path.
    cd "$here"
    for scenario in [0-9]_*.sql
    do
        psql -X -q -f setup.sql
        printf '\n--- %s ---\n\n' "$scenario"
        # Echo each statement with its result, and leave out the file's two
        # header comments.
        psql -X -a -f "$scenario" 2>&1 | sed '1,/^ \*\/$/d' | sed '1,/^ \*\/$/d'
    done
}

initdb -D "$datadir" -A trust >/dev/null
cat >> "$datadir/postgresql.conf" <<CONF
listen_addresses = ''
unix_socket_directories = '$DEMO_DIR'
wal_level = replica
autovacuum = off
CONF

heading "Stock $version: wrong results, and no error"
pg_ctl -D "$datadir" -l "$DEMO_DIR/server.log" -w start >/dev/null
runScenarios

heading "$version with pg_repack_safe: an error instead"
if [ ! -e "$pkglibdir/pg_repack_safe.so" ] && [ ! -e "$pkglibdir/pg_repack_safe.dylib" ]
then
    echo "pg_repack_safe is not installed in $pkglibdir; run make install." >&2
    exit 1
fi
echo "shared_preload_libraries = 'pg_repack_safe'" >> "$datadir/postgresql.conf"
pg_ctl -D "$datadir" -l "$DEMO_DIR/server.log" -w restart >/dev/null
psql -X -q -c "CREATE EXTENSION pg_repack_safe"
runScenarios
