# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# The record table cannot be made unfindable while it holds records.
#
# The drop refusals find the record table by its name in the extension's
# schema (isRecordTable). If the table could be renamed or moved out of that
# schema, DROP EXTENSION would drop it, and its records, unrecognized, with
# no override, and an old snapshot would then read the repacked table as
# empty. These statements would make the table unfindable, and each is
# refused while the table holds records:
# - ALTER TABLE ... RENAME TO;
# - ALTER TABLE ... SET SCHEMA, also after the table was detached from the
#   extension with ALTER EXTENSION ... DROP TABLE;
# - ALTER EXTENSION ... SET SCHEMA, with the table or after it was detached,
#   which PostgreSQL refuses for an extension with a fixed schema.
# After each attempt DROP EXTENSION is still refused and the old snapshot's
# read still gets the reader guard's error. ALTER SCHEMA ... RENAME of the
# extension's schema is allowed and leaves the guard in force.
# sql/protection.sql pins the messages and the superuser override.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib $FindBin::RealBin;
use RepackSafeTest;

my $guardRefusal =
  qr/cannot rename or move the pg_repack_safe record table while it holds records/;
my $coreRefusal = qr/extension "pg_repack_safe" does not support SET SCHEMA/;

my %attempts = (
    rename => [ 'ALTER TABLE repack_safe.rewrites RENAME TO x',
        $guardRefusal ],
    set_schema => [
        'ALTER TABLE repack_safe.rewrites SET SCHEMA elsewhere',
        $guardRefusal ],
    detach_and_set_schema => [ q{
        ALTER EXTENSION pg_repack_safe DROP TABLE repack_safe.rewrites;
        ALTER TABLE repack_safe.rewrites SET SCHEMA elsewhere},
        $guardRefusal ],
    move_extension => [
        'ALTER EXTENSION pg_repack_safe SET SCHEMA elsewhere', $coreRefusal ],
    detach_and_move_extension => [ q{
        ALTER EXTENSION pg_repack_safe DROP TABLE repack_safe.rewrites;
        ALTER EXTENSION pg_repack_safe SET SCHEMA elsewhere},
        $coreRefusal ]);

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
      $node->psql('postgres', $attempts{$name}[0], on_error_stop => 1);
    isnt($ret, 0, "$name: refused while the record table holds records");
    like($stderr, $attempts{$name}[1], "$name: the expected refusal");

    # Whatever the attempt left behind, the records are still protected.
    ($ret, $stdout, $stderr) =
      $node->psql('postgres', 'DROP EXTENSION pg_repack_safe');
    isnt($ret, 0, "$name: DROP EXTENSION is still refused");
    $node->psql('postgres', 'DROP TABLE repack_safe.rewrites');
    $node->psql('postgres', 'DROP TABLE elsewhere.rewrites');
    $node->psql('postgres', 'DROP TABLE repack_safe.x');

    my ($out, $err) = runIn($reader, 'SELECT count(*) FROM t');
    like(
        $err,
        qr/cannot read table "t" with this snapshot/,
        "$name: the old snapshot's read is still refused");

    $reader->quit;
    $node->stop;
}

# Renaming the extension's schema keeps the guard working: the extension
# finds its schema by OID (get_extension_schema) and the record table by its
# name in that schema, and the rename changes neither the table's name nor
# the schema it is in.
{
    my $node = preloadedNode('schema_rename');
    $node->start;
    $node->safe_psql('postgres', q{
        CREATE EXTENSION pg_repack_safe;
        CREATE TABLE t (id int PRIMARY KEY);
        INSERT INTO t SELECT generate_series(1, 5);
        CREATE TABLE u (id int PRIMARY KEY);
        INSERT INTO u SELECT generate_series(1, 5);
        CREATE TABLE other (id int);
    });

    my $reader = oldSnapshot($node);
    $node->safe_psql('postgres', 'REPACK (CONCURRENTLY) t');

    my ($ret, $stdout, $stderr) = $node->psql('postgres',
        'ALTER SCHEMA repack_safe RENAME TO guard', on_error_stop => 1);
    is($ret, 0, 'schema rename: allowed while the record table holds records')
      or diag($stderr);

    my ($out, $err) = runIn($reader, 'SELECT count(*) FROM t');
    like(
        $err,
        qr/cannot read table "t" with this snapshot/,
        "schema rename: the old snapshot's read is still refused");

    ($ret, $stdout, $stderr) =
      $node->psql('postgres', 'DROP EXTENSION pg_repack_safe');
    isnt($ret, 0, 'schema rename: DROP EXTENSION is still refused');

    $node->safe_psql('postgres', 'REPACK (CONCURRENTLY) u');
    ok( $node->safe_psql('postgres', q{
            SELECT count(*) > 0 FROM guard.rewrites
            WHERE relid = 'u'::regclass}) eq 't',
        'schema rename: REPACK (CONCURRENTLY) still writes a record');

    is($node->safe_psql('postgres', 'SELECT guard.status()'),
        't', 'schema rename: status() runs under the new name');
    ($ret, $stdout, $stderr) =
      $node->psql('postgres', 'SELECT guard.prune_records()');
    is($ret, 0, 'schema rename: prune_records() runs under the new name')
      or diag($stderr);

    $reader->quit;
    $node->stop;
}

# A server that preloads the library logs the library's version when it
# starts, once, from the postmaster.
my $logNode = preloadedNode('version_log');
$logNode->start;
my @loaded = slurp_file($logNode->logfile) =~
  /LOG:  pg_repack_safe \d+\.\d+\.\d+ loaded/g;
is(scalar @loaded, 1, 'the server logs the library version once at start');
$logNode->stop;

done_testing();
