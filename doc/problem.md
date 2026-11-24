# The problem in detail

PostgreSQL 19's `REPACK (CONCURRENTLY)` rewrites a table without blocking
reads or writes. Its reference page carries this warning, under
["Notes on Concurrent Operation"](https://www.postgresql.org/docs/19/sql-repack.html#SQL-REPACK-NOTES-ON-CONCURRENTLY):

> [!WARNING]
> `REPACK` with the `CONCURRENTLY` option is not MVCC-safe. After the
> repacking transaction commits, the table will appear empty to concurrent
> transactions, if they are using a snapshot taken before repacking started.
> See [Section 13.6](https://www.postgresql.org/docs/19/mvcc-caveats.html)
> for more details.

In practice, after it commits, a transaction whose snapshot was taken
before it committed, and which had not yet touched the table, reads the
table as **empty**. Nothing raises an error. The same happens to `pg_dump`
(a dump with the table's rows missing), to cursors, and to logical
replication's initial table copy (a subscriber that never gets the table's
existing rows).

> [!IMPORTANT]
> **The warning understates who is affected.** It names only snapshots
> taken before the repack *started*. A snapshot taken while the repack
> runs, before it commits, also reads the table as empty. Section 13.6,
> "Caveats", has it right. This extension covers both cases
> (`t/001_repack_pg_dump.pl` tests a `pg_dump` that starts during the
> repack).

`REPACK (CONCURRENTLY)` is built on the
[pg_squeeze](https://github.com/cybertec-postgresql/pg_squeeze) extension,
not on [pg_repack](https://reorg.github.io/pg_repack/), and both of those
tools have the same caveat: pg_squeeze
documents it, pg_repack does not. Using an extension is a choice, but in
core it was a known trade-off, made for performance and simplicity with a
plan to fix it later. The REPACK (CONCURRENTLY) command's history and its
sources are in [doc/research/history.md](research/history.md).

## The demo

`demo/demo.sh` runs four small scenarios on a throwaway PostgreSQL 19
server, first without the extension and then with it. It needs only the
PostgreSQL 19 binaries and this extension installed, and it touches no
existing server.

```bash
make install
demo/demo.sh
```

The first scenario is a report that reads `customers`, then `orders`, in one
transaction, while another session repacks `orders` in between. The table
holds 1000 orders worth 10000.00, and the repack changes none of them, so
the right answer is `1000 | 10000.00`. On stock PostgreSQL 19 the report is
told there are no orders:

```text
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT count(*) AS customers FROM customers;
 customers
-----------
       100

-- Meanwhile another session repacks orders.
\! psql -X -q -c "REPACK (CONCURRENTLY) orders"

SELECT count(*) AS orders, sum(amount) AS revenue FROM orders;
 orders | revenue
--------+---------
      0 |
```

With pg_repack_safe, the same statement fails, and the report, run again in
a new transaction, gets the right answer:

```text
SELECT count(*) AS orders, sum(amount) AS revenue FROM orders;
ERROR:  cannot read table "orders" with this snapshot
DETAIL:  REPACK (CONCURRENTLY) rewrote the table after the snapshot was
taken, and row visibility for older snapshots was not preserved.
HINT:  Retry the transaction.
COMMIT;
ROLLBACK

-- The report is retried.
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT count(*) AS customers FROM customers;
 customers
-----------
       100

SELECT count(*) AS orders, sum(amount) AS revenue FROM orders;
 orders | revenue
--------+----------
   1000 | 10000.00

COMMIT;
```

The next two scenarios show an `UPDATE` that reports `UPDATE 0` for ten
matching rows, and a `DELETE` of a customer's orders that removes none of
them before the customer itself is deleted. With the extension both get the
same error.

**The last scenario is `pg_dump`, and it is the one most likely to bite.**
You do not have to choose `REPEATABLE READ` to be exposed: `pg_dump` runs in
a `REPEATABLE READ` transaction of its own. On stock PostgreSQL 19 the dump
succeeds, and the table's rows are missing from it:

```text
pg_dump --data-only --table=orders --snapshot=... > orders.dump
pg_dump exit status: 0
rows in the dump: 0
```

That dump is a backup that cannot restore the table. It is not corrupt in a
way anything detects: the exit status is 0, and the file is well-formed,
with an empty data section where the table's rows belong. Backup jobs and
monitoring report success. The loss shows only when someone restores the
backup and notices (or NOT!) that the restored table is empty, and by then
the source rows may have changed and the older backups that held them may
have been rotated out.

With pg_repack_safe the dump fails instead. A failed backup is noticed and
run again; a silently incomplete one is not:

```text
pg_dump: error: query failed: ERROR:  cannot read table "orders" with this snapshot
DETAIL:  REPACK (CONCURRENTLY) rewrote the table after the snapshot was
taken, and row visibility for older snapshots was not preserved.
HINT:  Retry the transaction.
pg_dump: detail: Query was: COPY public.orders (id, customer_id, amount, status) TO stdout;
pg_dump exit status: 1
rows in the dump: 0

-- The dump is run again, with a snapshot of its own.
pg_dump --data-only --table=orders > orders.dump
pg_dump exit status: 0
rows in the dump: 1000
```

The demo fixes the timing with an exported snapshot (`--snapshot`). Without
one, the exposed dump is one that starts while a `REPACK (CONCURRENTLY)` is
running: `pg_dump` takes its snapshot first and locks each table after, so
it reads a table the repack swapped in the meantime. Once `pg_dump` holds
its lock on a table, a repack of that table cannot swap until the dump
finishes. `t/001_repack_pg_dump.pl` tests both cases with the extension.
On stock PostgreSQL it tests that the incomplete dump restores without an
error, as an empty table.

`t/018_demo.pl` runs the demo and checks every one of these outputs.
