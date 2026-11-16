# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# Hook chaining with another module that installs the same hooks.
#
# pg_stat_statements installs ProcessUtility_hook and ExecutorStart_hook. With
# both libraries preloaded, in either order, each module's hooks must still
# run:
# - pg_stat_statements records a REPACK only if its ProcessUtility hook ran,
#   and records a query only if its ExecutorStart hook ran (that hook sets up
#   the timing its ExecutorEnd hook requires);
# - pg_repack_safe classifies REPACK (CONCURRENTLY) only if its ProcessUtility
#   hook ran, and refuses an old snapshot's query of the repacked table only
#   if its ExecutorStart hook ran.
#
# object_access_hook has no second user among the installed stock modules, so
# its chaining is not covered here.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my %orders = (
    stats_first => 'pg_stat_statements, pg_repack_safe',
    stats_last => 'pg_repack_safe, pg_stat_statements');

foreach my $name (sort keys %orders)
{
    # REPACK (CONCURRENTLY) needs wal_level = replica or higher.
    my $node = PostgreSQL::Test::Cluster->new($name);
    $node->init(allows_streaming => 1);
    $node->append_conf('postgresql.conf',
        "shared_preload_libraries = '$orders{$name}'\n");
    $node->start;

    $node->safe_psql('postgres', q{
        CREATE EXTENSION pg_stat_statements;
        CREATE EXTENSION pg_repack_safe;
        CREATE TABLE chain_t (id int PRIMARY KEY);
        INSERT INTO chain_t SELECT generate_series(1, 3);
        CREATE TABLE chain_other (id int);
        SELECT pg_stat_statements_reset();
    });

    # A snapshot taken before the REPACK, without touching chain_t.
    my $reader = $node->background_psql('postgres', on_error_stop => 0);
    $reader->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
    $reader->query_safe('SELECT count(*) FROM chain_other');

    # ProcessUtility: both modules see the same REPACK.
    my ($ret, $stdout, $stderr) = $node->psql('postgres', q{
        SET pg_repack_safe.trace_storage_changes = on;
        REPACK (CONCURRENTLY) chain_t;
    });
    is($ret, 0, "$name: REPACK (CONCURRENTLY) succeeds");
    like(
        $stderr,
        qr/storage of "chain_t" replaced, old storage has 1 blocks, unsafe rewriter: yes/,
        "$name: pg_repack_safe classified the REPACK");
    is( $node->safe_psql('postgres', q{
            SELECT sum(calls) FROM pg_stat_statements
            WHERE query = 'REPACK (CONCURRENTLY) chain_t'}),
        '1',
        "$name: pg_stat_statements recorded the REPACK");

    # ExecutorStart: both modules see the same queries.
    $node->safe_psql('postgres', 'SELECT count(*) AS chain_probe FROM chain_t');
    is( $node->safe_psql('postgres', q{
            SELECT sum(calls) FROM pg_stat_statements
            WHERE query = 'SELECT count(*) AS chain_probe FROM chain_t'}),
        '1',
        "$name: pg_stat_statements recorded the query");

    # The old snapshot's read of chain_t is refused only if
    # pg_repack_safe's ExecutorStart hook ran.
    $reader->{stderr} = '';
    $reader->query('SELECT count(*) FROM chain_t');
    like(
        $reader->{stderr},
        qr/cannot read table "chain_t" with this snapshot/,
        "$name: pg_repack_safe refused the old snapshot's query");
    # BackgroundPsql keeps stderr across queries; clear the expected error.
    $reader->{stderr} = '';
    $reader->query_safe('ROLLBACK');
    $reader->quit;

    # The refused query still ends cleanly for both modules. The query has
    # its own shape: pg_stat_statements does not tell queries apart by
    # column alias, so a count(*) here would join chain_probe's entry.
    is($node->safe_psql('postgres', 'SELECT max(id) FROM chain_t'),
        '3', "$name: a new snapshot's query passes");
    is( $node->safe_psql('postgres', q{
            SELECT sum(calls) FROM pg_stat_statements
            WHERE query = 'SELECT max(id) FROM chain_t'}),
        '1',
        "$name: pg_stat_statements recorded the query that passed");

    $node->stop;
}

done_testing();
