# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# The reader guard for COPY ... TO (doc/research/fix-design.md, step 8).
#
# COPY relation TO scans the relation, or each partition of a partitioned
# table, with the active snapshot and without the executor, so the guard
# checks it in the ProcessUtility hook. Each case pins what stock
# PostgreSQL 19 would do in a comment and asserts the guard's error:
# - interleaving 8: a REPEATABLE READ snapshot from before the REPACK;
# - READ COMMITTED: COPY takes its statement snapshot, then waits for its
#   lock behind the REPACK's AccessExclusiveLock request, and is granted the
#   lock after the REPACK commits (stock: an empty result);
# - a partitioned table whose partition was repacked;
# - the other spellings: a column list, a schema-qualified name with
#   options, COPY (query), and COPY of a relation under row-level security
#   (the last two run a query and are checked in ExecutorStart).
# New snapshots, and tables without a record, read every row.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf',
    "shared_preload_libraries = 'pg_repack_safe'\n");
$node->start;
$node->safe_psql('postgres', q{
    CREATE EXTENSION pg_repack_safe;
    CREATE TABLE t (id int PRIMARY KEY);
    INSERT INTO t SELECT generate_series(1, 5);
    CREATE TABLE p (id int PRIMARY KEY) PARTITION BY RANGE (id);
    CREATE TABLE p1 PARTITION OF p FOR VALUES FROM (1) TO (100);
    INSERT INTO p SELECT generate_series(1, 5);
    CREATE TABLE r (id int PRIMARY KEY);
    INSERT INTO r SELECT generate_series(1, 5);
    ALTER TABLE r ENABLE ROW LEVEL SECURITY;
    CREATE POLICY r_all ON r USING (true);
    CREATE ROLE reader LOGIN;
    GRANT SELECT ON t, p, p1, r TO reader;
    CREATE TABLE plain (id int PRIMARY KEY);
    INSERT INTO plain SELECT generate_series(1, 5);
    CREATE TABLE other (id int);
    GRANT SELECT ON plain, other TO reader;
});

my $guardError = qr/cannot read table "(\w+)" with this snapshot/;

# An old snapshot, taken without touching the tables read below.
sub oldSnapshot
{
    my ($user) = @_;
    my $connstr = $node->connstr('postgres');
    $connstr .= " user=$user" if defined $user;
    my $session = $node->background_psql('postgres',
        on_error_stop => 0, connstr => $connstr);
    $session->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
    $session->query_safe('SELECT count(*) FROM other');
    return $session;
}

# Run one statement in a session; return (stdout, stderr), then end the
# transaction.
sub runIn
{
    my ($session, $sql) = @_;
    $session->{stderr} = '';
    my $out = $session->query($sql);
    my $err = $session->{stderr};
    $session->{stderr} = '';
    $session->query_safe('ROLLBACK');
    return ($out, $err);
}

# Interleaving 8 and the spellings: one old snapshot per statement, all
# taken before the REPACKs.
my %copies = (
    'plain' => 'COPY t TO STDOUT',
    'column list' => 'COPY t (id) TO STDOUT',
    'qualified, CSV' => 'COPY public.t TO STDOUT WITH (FORMAT csv)',
    'query' => 'COPY (SELECT * FROM t) TO STDOUT',
    'partitioned' => 'COPY p TO STDOUT',
    'no record' => 'COPY plain TO STDOUT');
my %sessions = map { $_ => oldSnapshot() } keys %copies;
my $rlsSession = oldSnapshot('reader');

$node->safe_psql('postgres', q{
    REPACK (CONCURRENTLY) t;
    REPACK (CONCURRENTLY) p1;
    REPACK (CONCURRENTLY) r;
});

foreach my $name (sort keys %copies)
{
    my ($out, $err) = runIn($sessions{$name}, $copies{$name});
    if ($name eq 'no record')
    {
        is($err, '', "$name: no error");
        is(scalar(split /\n/, $out), 5, "$name: every row");
    }
    else
    {
        # Stock PostgreSQL 19: no error and no rows.
        like($err, $guardError, "$name: old snapshot gets the error");
    }
    $sessions{$name}->quit;
}

my ($out, $err) = runIn($sessions{'partitioned'} = oldSnapshot(),
    'COPY p TO STDOUT');
is($err, '', 'partitioned: a snapshot after the REPACK reads it');
$sessions{'partitioned'}->quit;

# Row-level security turns COPY r TO into a query, checked in ExecutorStart.
($out, $err) = runIn($rlsSession, 'COPY r TO STDOUT');
like($err, qr/cannot read table "r" with this snapshot/,
    'row-level security: old snapshot gets the error');
$rlsSession->quit;

# The partition's name in the error, for the partitioned case.
my $part = oldSnapshot();
$node->safe_psql('postgres', 'REPACK (CONCURRENTLY) p1');
($out, $err) = runIn($part, 'COPY p TO STDOUT');
like($err, qr/cannot read table "p1" with this snapshot/,
    'partitioned: the error names the repacked partition');
$part->quit;

# READ COMMITTED: COPY's statement snapshot predates the REPACK's commit,
# and its lock is granted after it.
my $blocker = $node->background_psql('postgres');
$blocker->query_safe('BEGIN');
$blocker->query_safe('SELECT count(*) FROM t');

my $repacker = $node->background_psql('postgres');
$repacker->query_until(qr/repack_started/,
    "\\echo repack_started\nREPACK (CONCURRENTLY) t;\n");
$node->poll_query_until('postgres', q{
    SELECT EXISTS (SELECT 1 FROM pg_locks
                   WHERE relation = 't'::regclass
                     AND mode = 'AccessExclusiveLock' AND NOT granted)
}) or die 'REPACK never queued for ACCESS EXCLUSIVE on t';

my ($copyOut, $copyErr) = ('', '');
my $copy = IPC::Run::start(
    [ 'psql', '-XAtq', '-d', $node->connstr('postgres'),
      '-c', 'COPY t TO STDOUT' ],
    '>', \$copyOut, '2>', \$copyErr);
$node->poll_query_until('postgres', q{
    SELECT EXISTS (SELECT 1 FROM pg_locks
                   WHERE relation = 't'::regclass
                     AND mode = 'AccessShareLock' AND NOT granted)
}) or die 'COPY never queued for ACCESS SHARE on t';

$blocker->query_safe('COMMIT');
$blocker->quit;
$copy->finish;
# Stock PostgreSQL 19: COPY succeeds with no rows.
like($copyErr, $guardError,
    'READ COMMITTED: COPY that waited for its lock gets the error');
is($copyOut, '', 'READ COMMITTED: COPY wrote no rows');
$repacker->quit;

# A new statement snapshot reads every row.
$out = $node->safe_psql('postgres', 'COPY t TO STDOUT');
is(scalar(split /\n/, $out), 5, 'new snapshot: COPY reads every row');

$node->stop;
done_testing();
