/*
 * Copyright (c) 2026 dataStone Inc.
 * SPDX-License-Identifier: MIT
 */

/**
 * @file 4_pg_dump.sql
 * @brief pg_dump with a snapshot taken before the repack
 */

-- A snapshot is exported for a dump, as is done to make a dump consistent
-- with a replication slot or with other dumps.
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT pg_export_snapshot() AS snapshot \gset
\setenv DEMO_SNAPSHOT :snapshot

-- Meanwhile another session repacks orders.
\! psql -X -q -c "REPACK (CONCURRENTLY) orders"

-- pg_dump dumps orders with that snapshot. The table has 1000 rows; this
-- prints pg_dump's exit status and the number of rows in the dump.
\! pg_dump --data-only --table=orders --snapshot="$DEMO_SNAPSHOT" > "$DEMO_DIR/orders.dump"; echo "pg_dump exit status: $?"; echo "rows in the dump: $(grep -c '^[0-9]' "$DEMO_DIR/orders.dump")"
COMMIT;

-- The dump runs again with a snapshot of its own. With the extension this is
-- the retry the failed dump calls for. On stock PostgreSQL nothing calls for
-- it, and the dump above has already been kept as a backup.
\! pg_dump --data-only --table=orders > "$DEMO_DIR/orders.dump"; echo "pg_dump exit status: $?"; echo "rows in the dump: $(grep -c '^[0-9]' "$DEMO_DIR/orders.dump")"
