# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# Records carry full (64-bit) transaction IDs.
#
# The cluster is moved to XID epoch 1 with pg_resetwal, so a full XID and
# its low 32 bits differ. Transaction X commits after a REPEATABLE READ
# reader took its snapshot. A record with X's low 32 bits in epoch 0 is
# about 2^32 transactions old, so it precedes the reader's snapshot and must
# not refuse it, although a 32-bit XID would name X. A record with X's full
# XID must refuse it. A record with a transaction ID not yet assigned cannot
# come from the writer and refuses every read of its table.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf',
    "shared_preload_libraries = 'pg_repack_safe'\n");
command_ok([ 'pg_resetwal', '--epoch' => '1', $node->data_dir ],
    'move the cluster to XID epoch 1');
$node->start;

$node->safe_psql('postgres', q{
    CREATE EXTENSION pg_repack_safe;
    CREATE TABLE t (id int PRIMARY KEY);
    INSERT INTO t SELECT generate_series(1, 5);
    CREATE TABLE other (id int);
});

# X is running when the reader takes its snapshot, and commits afterwards.
my $holder = $node->background_psql('postgres');
$holder->query_safe('BEGIN');
my $x = $holder->query_safe('SELECT pg_current_xact_id()');
cmp_ok($x, '>', 4294967296, "X ($x) is in epoch 1");

my $reader = $node->background_psql('postgres', on_error_stop => 0);
$reader->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
$reader->query_safe('SELECT count(*) FROM other');
$holder->query_safe('COMMIT');
$holder->quit;

sub readT
{
    $reader->{stderr} = '';
    my $out = $reader->query('SELECT count(*) FROM t');
    return ($out, $reader->{stderr});
}

$node->safe_psql('postgres', qq{
    INSERT INTO pg_repack_safe_rewrites
        VALUES ('t'::regclass, '@{[ $x - 4294967296 ]}');
});
my ($out, $err) = readT();
is($err, '', "epoch-0 record with X's low 32 bits: no error");
is($out, '5', "epoch-0 record with X's low 32 bits: every row");

$node->safe_psql('postgres', qq{
    INSERT INTO pg_repack_safe_rewrites VALUES ('t'::regclass, '$x');
});
($out, $err) = readT();
like($err, qr/cannot read table "t" with this snapshot/,
    "record with X's full XID refuses the reader");
$reader->{stderr} = '';
$reader->query_safe('ROLLBACK');
$reader->quit;

# A record from a transaction ID not yet assigned.
$node->safe_psql('postgres', q{
    TRUNCATE pg_repack_safe_rewrites;
    INSERT INTO pg_repack_safe_rewrites
        SELECT 't'::regclass, (pg_current_xact_id()::text::bigint + 1000000)::text::xid8;
});
my ($ret, $stdout, $stderr) = $node->psql('postgres', 'SELECT count(*) FROM t');
isnt($ret, 0, 'record with an unassigned XID refuses the read');
like($stderr,
    qr/pg_repack_safe found an unsafe-rewrite record with a transaction ID that has not been assigned/,
    'record with an unassigned XID: error names the cause');

$node->stop;
done_testing();
