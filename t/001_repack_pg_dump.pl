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
# the table with COPY ... TO stdout (pg_dump.c:2429), which scans the table
# without the executor (copyto.c:1362, copyto.c:1396), so these cases are
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

# The table itself is intact; only the dumps above were wrong.
is($node->safe_psql('postgres', 'SELECT count(*) FROM customers'), $rows,
    'customers still holds every row');

$node->stop;
done_testing();
