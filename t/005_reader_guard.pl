# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# The reader guard's error, as a client sees it.
#
# A REPEATABLE READ transaction takes its snapshot without touching t, then
# REPACK (CONCURRENTLY) t commits. On stock PostgreSQL 19 the transaction
# then reads t as empty. With pg_repack_safe it gets SQLSTATE 40001 with the
# DETAIL and HINT below; isolationtester and pg_dump show only the primary
# message, so they are pinned here with psql's VERBOSITY verbose.
#
# Also covered:
# - EXPLAIN without ANALYZE reads no rows and passes; EXPLAIN ANALYZE reads
#   and is refused;
# - a direct read of the TOAST relation t has after the swap is refused,
#   since the writer records that relation too;
# - a new snapshot reads every row.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib $FindBin::RealBin;
use RepackSafeTest;

my $node = preloadedNode('main');
$node->start;

$node->safe_psql('postgres', q{
    CREATE EXTENSION pg_repack_safe;
    CREATE TABLE t (id int PRIMARY KEY, v text);
    INSERT INTO t SELECT g, repeat('x', 3000) || g FROM generate_series(1, 5) g;
    CREATE TABLE other (id int);
});

# Two old snapshots, taken before the REPACK without touching t.
my @readers;
foreach my $i (0 .. 1)
{
    my $reader = oldSnapshot($node);
    $reader->query_safe('\set VERBOSITY verbose');
    push @readers, $reader;
}

$node->safe_psql('postgres',
    'REPACK (CONCURRENTLY) t USING INDEX t_pkey');

my ($out, $err) = runIn($readers[0], 'EXPLAIN SELECT count(*) FROM t');
is($err, '', 'EXPLAIN without ANALYZE passes with the old snapshot');

($out, $err) = runIn($readers[0], 'SELECT count(*) FROM t');
like($err, qr/ERROR:  40001: cannot read table "t" with this snapshot$/m,
    'old snapshot: SQLSTATE 40001 and the primary message');
like($err,
    qr/^DETAIL:  REPACK \(CONCURRENTLY\) rewrote the table after the snapshot was taken, and row visibility for older snapshots was not preserved\.$/m,
    'old snapshot: DETAIL');
like($err, qr/^HINT:  Retry the transaction\.$/m, 'old snapshot: HINT');
runIn($readers[0], 'ROLLBACK');

($out, $err) = runIn($readers[1], 'EXPLAIN ANALYZE SELECT count(*) FROM t');
like($err, qr/ERROR:  40001: cannot read table "t" with this snapshot$/m,
    'EXPLAIN ANALYZE is refused with the old snapshot');
runIn($readers[1], 'ROLLBACK');
runIn($readers[1], 'BEGIN ISOLATION LEVEL REPEATABLE READ');

# A fresh transaction, but its snapshot is taken now: it sees the REPACK.
($out, $err) = runIn($readers[1], 'SELECT count(*) FROM t');
is($err, '', 'new snapshot: no error');
is($out, '5', 'new snapshot: every row');

# The TOAST relation, read directly by an old snapshot.
$readers[1]->quit;
my $toastReader = oldSnapshot($node);
$toastReader->query_safe('\set VERBOSITY verbose');
$node->safe_psql('postgres',
    'REPACK (CONCURRENTLY) t USING INDEX t_pkey');
my $toast = $node->safe_psql('postgres',
    q{SELECT reltoastrelid::regclass FROM pg_class WHERE oid = 't'::regclass});
(my $toastName = $toast) =~ s/^pg_toast\.//;
($out, $err) = runIn($toastReader, "SELECT count(*) FROM $toast");
like($err,
    qr/ERROR:  40001: cannot read table "\Q$toastName\E" with this snapshot$/m,
    'old snapshot: direct read of the post-swap TOAST relation is refused');

$toastReader->quit;
$readers[0]->quit;
$node->stop;
done_testing();
