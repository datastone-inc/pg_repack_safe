# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# pg_dump against REPACK (CONCURRENTLY) on stock PostgreSQL 19.
#
# pg_dump reads every table under one REPEATABLE READ snapshot. A table that
# REPACK (CONCURRENTLY) rewrites and commits after that snapshot was taken,
# but before pg_dump locks the table, is dumped with no rows: pg_dump exits 0
# and the dump silently lacks the table's data.
#
# The tests marked "WRONG RESULT" pin the stock behaviour; they assert the
# empty dump. When pg_repack_safe prevents it, they flip to assert either a
# complete dump or a refused REPACK.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run;

my $rows = 5;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init(allows_streaming => 1);
$node->start;

$node->safe_psql('postgres', qq{
    CREATE TABLE customers (id int PRIMARY KEY, name text NOT NULL);
    INSERT INTO customers SELECT g, 'customer ' || g FROM generate_series(1, $rows) g;
});

my $repack_sql = 'REPACK (CONCURRENTLY) customers USING INDEX customers_pkey;';

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

    my ($out, $err) = run_command([dumpArgs("--snapshot=$snapshot")]);
    is($err, '', 'exported snapshot: pg_dump reports no error');
    is(dumpedRows($out), 0,
        'WRONG RESULT: dump with a pre-REPACK exported snapshot has no rows');

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

    $node->poll_query_until('postgres', q{
        SELECT EXISTS (SELECT 1 FROM pg_locks
                       WHERE relation = 'customers'::regclass
                         AND mode = 'AccessExclusiveLock' AND NOT granted)
    }) or die 'REPACK never queued for ACCESS EXCLUSIVE on customers';

    my ($out, $err) = ('', '');
    my $dump = IPC::Run::start([dumpArgs()], '>', \$out, '2>', \$err);

    $node->poll_query_until('postgres', q{
        SELECT EXISTS (SELECT 1 FROM pg_locks
                       WHERE relation = 'customers'::regclass
                         AND mode = 'AccessShareLock' AND NOT granted)
    }) or die 'pg_dump never queued for ACCESS SHARE on customers';

    $blocker->query_safe('COMMIT');
    $blocker->quit;

    $dump->finish;
    is($dump->result(0), 0, 'concurrent: pg_dump exits 0');
    is($err, '', 'concurrent: pg_dump reports no error');
    is(dumpedRows($out), 0,
        'WRONG RESULT: pg_dump started during REPACK dumps no rows');

    $repacker->quit;
}

# The table itself is intact; only the dumps above were wrong.
is($node->safe_psql('postgres', 'SELECT count(*) FROM customers'), $rows,
    'customers still holds every row');

$node->stop;
done_testing();
