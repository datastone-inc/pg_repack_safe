# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# The record table cannot be made unfindable while it holds records.
#
# The drop refusals find the record table by its name in the extension's
# schema (isRecordTable). If the table could be renamed or moved out of that
# schema, DROP EXTENSION would drop it, and its records, unrecognized, with
# no override, and an old snapshot would then read the repacked table as
# empty. Three statements make the table unfindable, and each is refused
# while the table holds records:
# - ALTER TABLE ... RENAME TO;
# - ALTER TABLE ... SET SCHEMA;
# - ALTER EXTENSION ... SET SCHEMA after the table was detached from the
#   extension with ALTER EXTENSION ... DROP TABLE.
# After each attempt DROP EXTENSION is still refused and the old snapshot's
# read still gets the reader guard's error. sql/protection.sql pins the
# messages and the superuser override.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib $FindBin::RealBin;
use RepackSafeTest;

my %attempts = (
    rename => 'ALTER TABLE pg_repack_safe_rewrites RENAME TO x',
    set_schema => 'ALTER TABLE pg_repack_safe_rewrites SET SCHEMA elsewhere',
    detach_and_move => q{
        ALTER EXTENSION pg_repack_safe DROP TABLE pg_repack_safe_rewrites;
        ALTER EXTENSION pg_repack_safe SET SCHEMA elsewhere});

foreach my $name (sort keys %attempts)
{
    my $node = preloadedNode($name);
    $node->start;
    $node->safe_psql('postgres', q{
        CREATE EXTENSION pg_repack_safe;
        CREATE SCHEMA elsewhere;
        CREATE TABLE t (id int PRIMARY KEY);
        INSERT INTO t SELECT generate_series(1, 5);
        CREATE TABLE other (id int);
    });

    my $reader = oldSnapshot($node);
    $node->safe_psql('postgres', 'REPACK (CONCURRENTLY) t');

    my ($ret, $stdout, $stderr) =
      $node->psql('postgres', $attempts{$name}, on_error_stop => 1);
    isnt($ret, 0, "$name: refused while the record table holds records");
    like(
        $stderr,
        qr/cannot rename or move the pg_repack_safe record table while it holds records/,
        "$name: the refusal names the record table");

    # Whatever the attempt left behind, the records are still protected.
    ($ret, $stdout, $stderr) =
      $node->psql('postgres', 'DROP EXTENSION pg_repack_safe');
    isnt($ret, 0, "$name: DROP EXTENSION is still refused");
    $node->psql('postgres', 'DROP TABLE public.pg_repack_safe_rewrites');
    $node->psql('postgres', 'DROP TABLE elsewhere.pg_repack_safe_rewrites');
    $node->psql('postgres', 'DROP TABLE x');

    my ($out, $err) = runIn($reader, 'SELECT count(*) FROM t');
    like(
        $err,
        qr/cannot read table "t" with this snapshot/,
        "$name: the old snapshot's read is still refused");

    $reader->quit;
    $node->stop;
}

done_testing();
