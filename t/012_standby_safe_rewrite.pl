# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# KNOWN GAP, pinned: VACUUM FULL and REPACK without CONCURRENTLY on the
# primary, against a hot standby without hot_standby_feedback
# (doc/research/fix-design.md, open question 1).
#
# These rewrites keep every row version a snapshot on the primary may need,
# but nothing a standby snapshot needs: rows deleted after the standby
# snapshot was taken are left out of the new copy. No recovery conflict is
# raised: the rewrite logs no snapshot-conflict record, and the replayed
# AccessExclusiveLock conflicts only with standby queries holding a lock on
# the table (high-availability.sgml, "Handling Query Conflicts"). So a
# standby REPEATABLE READ snapshot that has not read the table yet reads it
# with those rows missing. This is stock PostgreSQL behaviour, reported on
# pgsql-hackers in 2012 and not fixed; it happens without this extension.
#
# Expected here: on the standby without feedback, 3 rows instead of 5 (the
# wrong result, pinned); on the standby with hot_standby_feedback = on, 5,
# because the primary then keeps what that standby's snapshot needs.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Each setting gets its own primary: a feedback standby's xmin holds back
# the primary's horizon for every standby attached to it.
foreach my $feedback ('off', 'on')
{
    my $primary = PostgreSQL::Test::Cluster->new("primary_$feedback");
    $primary->init(allows_streaming => 1);
    $primary->append_conf('postgresql.conf', q{
shared_preload_libraries = 'pg_repack_safe'
autovacuum = off
});
    $primary->start;
    $primary->safe_psql('postgres', q{
        CREATE EXTENSION pg_repack_safe;
        CREATE TABLE t (id int PRIMARY KEY);
        INSERT INTO t SELECT generate_series(1, 5);
        CREATE TABLE u (id int PRIMARY KEY);
        INSERT INTO u SELECT generate_series(1, 5);
        CREATE TABLE other (id int);
    });
    $primary->backup('base');

    my $standby = PostgreSQL::Test::Cluster->new("standby_$feedback");
    $standby->init_from_backup($primary, 'base', has_streaming => 1);
    $standby->append_conf('postgresql.conf', qq{
hot_standby_feedback = $feedback
wal_receiver_status_interval = 1s
max_standby_streaming_delay = 200ms
});
    $standby->start;
    $primary->wait_for_catchup($standby);

    foreach my $case (['VACUUM FULL', 't'], ['REPACK', 'u'])
    {
        my ($command, $table) = @$case;

        # A standby snapshot taken before the DELETE, without touching the
        # table.
        my $reader = $standby->background_psql('postgres',
            on_error_stop => 0);
        $reader->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
        $reader->query_safe('SELECT count(*) FROM other');
        if ($feedback eq 'on')
        {
            # The primary has the standby's xmin before it deletes.
            $primary->poll_query_until('postgres',
                'SELECT backend_xmin IS NOT NULL FROM pg_stat_replication')
              or die 'no feedback xmin from the standby';
        }

        $primary->safe_psql('postgres', "DELETE FROM $table WHERE id <= 2");
        $primary->safe_psql('postgres', "$command $table");
        $primary->wait_for_catchup($standby);

        my $out = $reader->query_safe("SELECT count(*) FROM $table");
        if ($feedback eq 'off')
        {
            is($out, '3',
                "KNOWN GAP: $command, standby without feedback reads 3 rows of 5");
        }
        else
        {
            is($out, '5',
                "$command, standby with hot_standby_feedback reads all 5");
        }
        $reader->quit;
    }

    $standby->stop;
    $primary->stop;
}

done_testing();
