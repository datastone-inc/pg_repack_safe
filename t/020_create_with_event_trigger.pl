# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# CREATE EXTENSION pg_repack_safe in a database with DDL event triggers
# whose functions run queries.
#
# Event triggers fire for each command of an extension script. The install
# script creates the record table in one command and its triggers in later
# ones, so a query an event trigger runs in between meets the extension
# without its record table (ddl_command_start of CREATE TABLE) and then the
# record table without its triggers (ddl_command_end of CREATE TABLE). The
# reader check refuses both states in any other statement
# (sql/reader_check.sql); while this backend runs the extension's own script
# it passes them (recordTableIncomplete), and CREATE EXTENSION succeeds.
#
# Afterwards the guard is in force: REPACK (CONCURRENTLY) is recorded and an
# old snapshot's read is refused.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib $FindBin::RealBin;
use RepackSafeTest;

my $node = preloadedNode('evt');
$node->start;

$node->safe_psql('postgres', q{
    CREATE TABLE ddl_log (event text, tag text);
    CREATE FUNCTION log_ddl() RETURNS event_trigger LANGUAGE plpgsql AS $$
    BEGIN
        INSERT INTO ddl_log VALUES (tg_event, tg_tag);
    END $$;
    CREATE EVENT TRIGGER log_ddl_start ON ddl_command_start
        EXECUTE FUNCTION log_ddl();
    CREATE EVENT TRIGGER log_ddl_end ON ddl_command_end
        EXECUTE FUNCTION log_ddl();
    CREATE TABLE t (id int PRIMARY KEY);
    INSERT INTO t SELECT generate_series(1, 5);
    CREATE TABLE other (id int);
    TRUNCATE ddl_log;
});

my ($ret, $stdout, $stderr) =
  $node->psql('postgres', 'CREATE EXTENSION pg_repack_safe');
is($ret, 0, 'CREATE EXTENSION succeeds with DDL event triggers')
  or diag($stderr);

# The triggers ran inside the script, in both of the incomplete states.
is( $node->safe_psql('postgres', q{
        SELECT count(*) FROM ddl_log
        WHERE event = 'ddl_command_start' AND tag = 'CREATE TABLE'}),
    '1', 'an event trigger query ran before the record table existed');
is( $node->safe_psql('postgres', q{
        SELECT count(*) FROM ddl_log
        WHERE event = 'ddl_command_end' AND tag = 'CREATE TABLE'}),
    '1', 'an event trigger query ran before the record table had triggers');

# The guard is in force afterwards.
my $reader = oldSnapshot($node);
$node->safe_psql('postgres', 'REPACK (CONCURRENTLY) t');
my ($out, $err) = runIn($reader, 'SELECT count(*) FROM t');
like(
    $err,
    qr/cannot read table "t" with this snapshot/,
    'the old snapshot is refused after the install');

$reader->quit;
$node->stop;
done_testing();
