# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# pg_dump against REPACK (CONCURRENTLY).
#
# pg_dump reads every table under one REPEATABLE READ snapshot. A table that
# REPACK (CONCURRENTLY) rewrites and commits after that snapshot was taken,
# but before pg_dump locks the table, is dumped with no rows on stock
# PostgreSQL 19: pg_dump exits 0 and the dump silently lacks the table's data.
#
# With pg_repack_safe preloaded and created, the cases marked "GUARDED"
# expect pg_dump to fail with the reader guard's error instead. pg_dump reads
# the table with COPY ... TO stdout (pg_dump.c, dumpTableData_copy), which scans the table
# without the executor (copyto.c, DoCopyTo and CopyRelationTo), so these cases are
# caught by the COPY check in the ProcessUtility hook. pg_dump prints only
# the primary message, so the SQLSTATE is not pinned here.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib $FindBin::RealBin;
use RepackSafeTest;
use IPC::Run;

my $rows = 5;

my $node = preloadedNode('main');
$node->start;

$node->safe_psql('postgres', qq{
    CREATE EXTENSION pg_repack_safe;
    CREATE TABLE customers (id int PRIMARY KEY, name text NOT NULL);
    INSERT INTO customers SELECT g, 'customer ' || g FROM generate_series(1, $rows) g;
});

my $repack_sql = 'REPACK (CONCURRENTLY) customers USING INDEX customers_pkey;';
my $guard_error = qr/cannot read table "customers" with this snapshot/;

# Number of data rows in a plain-format, data-only dump of customers.
sub dumpedRows
{
    my ($dump) = @_;
    my ($copy) = $dump =~ /^COPY public\.customers .*? FROM stdin;\n(.*?)^\\\.$/ms;
    die "no COPY block for customers in dump:\n$dump" unless defined $copy;
    my @lines = split /\n/, $copy;
    return scalar @lines;
}

sub dumpArgs
{
    return ('pg_dump', '--data-only', '--table=customers',
        '--dbname=' . $node->connstr('postgres'), @_);
}

# Control: a dump with no concurrent REPACK holds every row.
{
    my ($out, $err) = run_command([dumpArgs()]);
    is(dumpedRows($out), $rows, 'control: dump without concurrent REPACK has all rows');
}

# Case 1: pg_dump --snapshot with a snapshot exported before REPACK starts.
# This is how a dump is made consistent with a logical replication slot or
# with other dumps. The exporting transaction never reads customers, so it
# holds no lock that would make REPACK wait.
{
    my $holder = $node->background_psql('postgres');
    $holder->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
    my $snapshot = $holder->query_safe('SELECT pg_export_snapshot()');
    chomp $snapshot;

    $node->safe_psql('postgres', $repack_sql);

    my ($out, $err) = ('', '');
    my $ok = IPC::Run::run([dumpArgs("--snapshot=$snapshot")],
        '>', \$out, '2>', \$err);
    ok(!$ok, 'GUARDED: pg_dump with a pre-REPACK exported snapshot fails');

    # The retry the error asks for: a dump with its own, newer snapshot.
    my ($retry) = run_command([dumpArgs()]);
    is(dumpedRows($retry), $rows,
        'GUARDED: exported snapshot: pg_dump run again dumps every row');
    like($err, $guard_error,
        'GUARDED: exported snapshot: pg_dump reports the guard error');

    $holder->query_safe('COMMIT');
    $holder->quit;
}

# Case 2: a plain pg_dump that starts while REPACK (CONCURRENTLY) is running.
# pg_dump takes its snapshot, then queues for ACCESS SHARE on customers behind
# REPACK's ACCESS EXCLUSIVE request at the swap. REPACK commits, pg_dump gets
# the lock and reads the new table with its older snapshot. To make the
# interleaving deterministic, a third session holds ACCESS SHARE on customers
# so REPACK stalls at the swap until that session commits.
{
    my $blocker = $node->background_psql('postgres');
    $blocker->query_safe('BEGIN');
    $blocker->query_safe('SELECT count(*) FROM customers');

    my $repacker = $node->background_psql('postgres');
    $repacker->query_until(qr/repack_started/,
        "\\echo repack_started\n$repack_sql\n");

    # REPACK queues for its swap.
    waitForLockRequest($node, 'customers', 'AccessExclusiveLock');

    my ($out, $err) = ('', '');
    my $dump = IPC::Run::start([dumpArgs()], '>', \$out, '2>', \$err);

    # pg_dump queues behind it.
    waitForLockRequest($node, 'customers', 'AccessShareLock');

    $blocker->query_safe('COMMIT');
    $blocker->quit;

    $dump->finish;
    isnt($dump->result(0), 0,
        'GUARDED: pg_dump started during REPACK fails');
    like($err, $guard_error,
        'GUARDED: concurrent: pg_dump reports the guard error');

    $repacker->quit;
}

# A dump of the whole database leaves the records behind: their relation
# OIDs and transaction IDs mean nothing in another database
# (doc/installation.md, "Dump and Restore").
{
    isnt($node->safe_psql('postgres',
            'SELECT count(*) FROM pg_repack_safe_rewrites'),
        '0', 'the record table holds records');
    my ($out, $err) = run_command(
        [ 'pg_dump', '--dbname=' . $node->connstr('postgres') ]);
    like($out, qr/^CREATE EXTENSION IF NOT EXISTS pg_repack_safe\b/m,
        'a full dump creates the extension');
    unlike($out, qr/pg_repack_safe_rewrites/,
        'a full dump does not carry the records');
}

# The table itself is intact; only the dumps above were wrong.
is($node->safe_psql('postgres', 'SELECT count(*) FROM customers'), $rows,
    'customers still holds every row');

$node->stop;

# Stock PostgreSQL 19, without the library: case 1 again. pg_dump succeeds
# and the dump holds no rows of customers. Restored, the dump gives an empty
# table without an error, so nothing between the dump and the restore says
# that the backup lacks the table's data.
{
    my $stock = preloadedNode('stock', libraries => '');
    $stock->start;
    $stock->safe_psql('postgres', qq{
        CREATE TABLE customers (id int PRIMARY KEY, name text NOT NULL);
        INSERT INTO customers SELECT g, 'customer ' || g FROM generate_series(1, $rows) g;
        CREATE DATABASE restored;
    });

    my $holder = $stock->background_psql('postgres');
    $holder->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
    my $snapshot = $holder->query_safe('SELECT pg_export_snapshot()');
    chomp $snapshot;

    $stock->safe_psql('postgres', $repack_sql);

    my ($out, $err) = ('', '');
    my $ok = IPC::Run::run(['pg_dump', '--data-only', '--table=customers',
            '--dbname=' . $stock->connstr('postgres'), "--snapshot=$snapshot"],
        '>', \$out, '2>', \$err);
    ok($ok, 'STOCK: pg_dump with a pre-REPACK exported snapshot exits 0')
        or diag($err);
    is(dumpedRows($out), 0, 'STOCK: the dump holds no rows of customers');

    $holder->query_safe('COMMIT');
    $holder->quit;

    $stock->safe_psql('restored',
        'CREATE TABLE customers (id int PRIMARY KEY, name text NOT NULL)');
    my ($restoreOut, $restoreErr) = ('', '');
    my $restored = IPC::Run::run(['psql', '-X', '-q', '-v', 'ON_ERROR_STOP=1',
            '--dbname=' . $stock->connstr('restored')],
        '<', \$out, '>', \$restoreOut, '2>', \$restoreErr);
    ok($restored, 'STOCK: the dump restores without an error')
        or diag($restoreErr);
    is($stock->safe_psql('restored', 'SELECT count(*) FROM customers'), '0',
        'STOCK: the restored customers table is empty');
    is($stock->safe_psql('postgres', 'SELECT count(*) FROM customers'), $rows,
        'STOCK: the source table still holds every row');

    $stock->stop;
}

done_testing();
