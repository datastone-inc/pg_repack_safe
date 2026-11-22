# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# A publication FOR ALL TABLES publishes the record table, and a publisher's
# record must not reach a subscriber's record table.
#
# A record names a table by OID and a transaction by ID. Both mean something
# only in the cluster that wrote them: on a subscriber the OID is some other
# table's, or nobody's, and the transaction ID may not be assigned yet. A
# record applied there would refuse every read and write of the table that
# has the OID (the transaction ID "has not been assigned") until the
# subscriber's transaction counter passed it.
#
# Two things keep it out. The writer inserts its record without the
# information logical decoding needs (insertRecord, HEAP_INSERT_NO_LOGICAL),
# so the record of a REPACK (CONCURRENTLY) is never sent. And the record
# table's BEFORE INSERT trigger refuses whatever arrives all the same: the
# rows a new subscription's initial copy reads from the publisher's record
# table, and a row from any other source, which this test writes with
# test_insert_record.
#
# Setup: the extension on both nodes, and the tables created in such an order
# that the publisher's repacked table t and the subscriber's unrelated table
# victim have the same OID. The publisher's transaction counter is moved well
# ahead of the subscriber's before the REPACK.
#
# What must hold:
# - a REPACK (CONCURRENTLY) on the publisher sends no record and does not
#   disturb the subscription;
# - when a record is sent all the same, the subscriber's record table stays
#   empty and victim stays readable and writable: the record table's BEFORE
#   INSERT trigger refuses the row, in the apply worker as in any session;
# - the refusal is loud: the apply worker reports it and retries, and until
#   an operator acts the subscription applies nothing more, neither for the
#   record table nor for any other published table;
# - the documented recovery works (doc/installation.md, "Logical
#   Replication"): exclude the record table from the publication, refresh
#   the subscription, and replication continues; a later
#   REPACK (CONCURRENTLY) on the publisher no longer stops it;
# - the initial table synchronization refuses a publisher's existing
#   records in the same way, for that one table, and the same recovery ends
#   its retries;
# - a publisher's TRUNCATE of its record table does not empty a
#   subscriber's: the BEFORE TRUNCATE trigger refuses it in the apply
#   worker, and the same recovery releases the subscription.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib $FindBin::RealBin;
use RepackSafeTest;

my $publisher = preloadedNode('publisher', allows_streaming => 'logical');
$publisher->start;
# A short retry interval, so that the apply worker's second attempt does not
# take the default five seconds.
my $subscriber = preloadedNode('subscriber',
    conf => "wal_retrieve_retry_interval = 200ms\n");
$subscriber->start;

# The same statements up to the first table, so that publisher t and
# subscriber victim get the same OID.
$publisher->safe_psql('postgres', q{
    CREATE EXTENSION pg_repack_safe;
    CREATE TABLE t (id int PRIMARY KEY);
    CREATE TABLE other (id int PRIMARY KEY);
    CREATE TABLE progress (id int PRIMARY KEY);
    INSERT INTO t SELECT generate_series(1, 5);
    CREATE FUNCTION test_insert_record(record_table regclass, relid oid,
                                       rewrite_xid xid8)
    RETURNS void
    AS 'test_record'
    LANGUAGE C STRICT;
});
$subscriber->safe_psql('postgres', q{
    CREATE EXTENSION pg_repack_safe;
    CREATE TABLE victim (id int PRIMARY KEY);
    CREATE TABLE t (id int PRIMARY KEY);
    CREATE TABLE other (id int PRIMARY KEY);
    CREATE TABLE progress (id int PRIMARY KEY);
    INSERT INTO victim SELECT generate_series(1, 3);
});
my $tOid = $publisher->safe_psql('postgres', q{SELECT 't'::regclass::oid});
is($subscriber->safe_psql('postgres', q{SELECT 'victim'::regclass::oid}),
    $tOid, "publisher t and subscriber victim share OID $tOid");

$publisher->safe_psql('postgres', 'CREATE PUBLICATION pub FOR ALL TABLES');
is($publisher->safe_psql('postgres', q{
        SELECT count(*) FROM pg_publication_tables
        WHERE pubname = 'pub' AND tablename = 'pg_repack_safe_rewrites'}),
    '1', 'FOR ALL TABLES publishes the record table');

my $connstr = $publisher->connstr . ' dbname=postgres';
$subscriber->safe_psql('postgres',
    "CREATE SUBSCRIPTION sub CONNECTION '$connstr' PUBLICATION pub");
$subscriber->wait_for_subscription_sync($publisher, 'sub');
is($subscriber->safe_psql('postgres', 'SELECT count(*) FROM t'),
    '5', 'initial synchronization copied t');

# Put the publisher's transaction counter ahead of the subscriber's, so
# that the record's transaction ID is not assigned on the subscriber.
$publisher->safe_psql('postgres', "SELECT pg_current_xact_id();\n" x 3000);
cmp_ok(
    $publisher->safe_psql('postgres',
        'SELECT pg_current_xact_id()::text::bigint'),
    '>',
    $subscriber->safe_psql('postgres',
        'SELECT pg_current_xact_id()::text::bigint') + 2000,
    "the publisher's transaction counter is ahead of the subscriber's");

my $errorCount = q{(SELECT apply_error_count FROM pg_stat_subscription_stats
                    WHERE subname = 'sub')};

# The writer's record is not sent. A change committed after the REPACK
# arrives, so the subscription went past the REPACK's transaction, and it
# did so without an error and without storing anything.
$publisher->safe_psql('postgres', 'REPACK (CONCURRENTLY) t');
is($publisher->safe_psql('postgres',
        "SELECT count(*) FROM pg_repack_safe_rewrites WHERE relid = $tOid"),
    '1', 'the publisher recorded the REPACK');
$publisher->safe_psql('postgres', 'INSERT INTO progress VALUES (1)');
$publisher->wait_for_catchup('sub');
is($subscriber->safe_psql('postgres', 'SELECT count(*) FROM progress'),
    '1', "a REPACK (CONCURRENTLY) on the publisher does not stop the subscription");
is($subscriber->safe_psql('postgres',
        'SELECT count(*) FROM pg_repack_safe_rewrites'),
    '0', "the writer's record does not reach the subscriber");
is($subscriber->safe_psql('postgres', "SELECT $errorCount"),
    '0', 'the apply worker reported no error');

# A record that is sent all the same: one inserted the way the writer
# inserts except for that (test_insert_record, test/modules/test_record),
# as a record from any other source would be.
my $logOffset = -s $subscriber->logfile;
$publisher->safe_psql('postgres', qq{
    SELECT test_insert_record('pg_repack_safe_rewrites', $tOid,
                              pg_current_xact_id())});
is($publisher->safe_psql('postgres',
        "SELECT count(*) FROM pg_repack_safe_rewrites WHERE relid = $tOid"),
    '2', 'the publisher holds a record that logical decoding sees');
# A change to another published table, committed after that record.
$publisher->safe_psql('postgres', 'INSERT INTO other VALUES (1)');

# Wait until the subscriber has dealt with the record one way or the other:
# refused it at least twice (an error and a retry), or stored it.
$subscriber->poll_query_until('postgres', qq{
    SELECT $errorCount >= 2
        OR EXISTS (SELECT 1 FROM pg_repack_safe_rewrites)
}) or die 'the subscriber neither refused nor stored the record';

is($subscriber->safe_psql('postgres',
        'SELECT count(*) FROM pg_repack_safe_rewrites'),
    '0', "the subscriber's record table holds no record");
my ($ret, $stdout, $stderr) =
  $subscriber->psql('postgres', 'SELECT count(*) FROM victim');
is($stderr, '', 'the table with the same OID on the subscriber: no error');
is($stdout, '3', 'the table with the same OID on the subscriber: readable');
($ret, $stdout, $stderr) =
  $subscriber->psql('postgres', 'INSERT INTO victim VALUES (4)');
is($ret, 0, 'the table with the same OID on the subscriber: writable')
  or diag($stderr);

# The apply worker reports the refusal, with the table it was applying to.
my $log = slurp_file($subscriber->logfile, $logOffset);
like($log,
    qr/ERROR:  cannot insert into the pg_repack_safe record table/,
    'the apply worker reports the refused record');
like($log,
    qr/CONTEXT:  processing remote data for replication origin "[^"]+" during message type "INSERT" for replication target relation "public\.pg_repack_safe_rewrites"/,
    'the report names the record table as the replication target');

# The subscription is stopped at the refused transaction: the later change
# to another table has not arrived although the worker has retried.
is($subscriber->safe_psql('postgres', 'SELECT count(*) FROM other'),
    '0', 'the subscription applies nothing after the refused transaction');

# Recovery, step 1, on the publisher: stop publishing the record table.
$publisher->safe_psql('postgres', q{
    ALTER PUBLICATION pub SET ALL TABLES EXCEPT (TABLE pg_repack_safe_rewrites)
});
is($publisher->safe_psql('postgres', q{
        SELECT count(*) FROM pg_publication_tables
        WHERE pubname = 'pub' AND tablename = 'pg_repack_safe_rewrites'}),
    '0', 'the record table is excluded from the publication');

# The exclusion does not release the transaction already refused: the apply
# worker fails on it again. Two more failures are waited for, so that the
# second attempt began after the publication changed.
my $before = $subscriber->safe_psql('postgres', "SELECT $errorCount");
$subscriber->poll_query_until('postgres',
    "SELECT $errorCount >= $before + 2")
  or die 'the apply worker did not retry';
is($subscriber->safe_psql('postgres', 'SELECT count(*) FROM other'),
    '0', 'excluding the table does not release the refused transaction');

# Step 2, on the subscriber: refresh the subscription, which drops the
# record table from it. The apply worker then passes over that table's
# changes (worker.c:2683-2691, should_apply_changes_for_rel) and goes on.
$subscriber->safe_psql('postgres',
    'ALTER SUBSCRIPTION sub REFRESH PUBLICATION');
$publisher->wait_for_catchup('sub');
is($subscriber->safe_psql('postgres', 'SELECT count(*) FROM other'),
    '1', 'after the refresh, the later change arrives');
is($subscriber->safe_psql('postgres',
        'SELECT count(*) FROM pg_repack_safe_rewrites'),
    '0', "after the refresh, the subscriber's record table holds no record");

# With the record table excluded, a REPACK (CONCURRENTLY) on the publisher
# does not stop the subscription, and changes to the repacked table arrive.
$before = $subscriber->safe_psql('postgres', "SELECT $errorCount");
$publisher->safe_psql('postgres', q{
    REPACK (CONCURRENTLY) t;
    INSERT INTO t VALUES (6);
    INSERT INTO other VALUES (2);
});
$publisher->wait_for_catchup('sub');
is($subscriber->safe_psql('postgres', 'SELECT count(*) FROM t'),
    '6', 'record table excluded: changes to the repacked table arrive');
is($subscriber->safe_psql('postgres', 'SELECT count(*) FROM other'),
    '2', 'record table excluded: changes to other tables arrive');
is($subscriber->safe_psql('postgres', "SELECT $errorCount"),
    $before, 'record table excluded: the apply worker reports no error');
is($subscriber->safe_psql('postgres',
        'SELECT count(*) FROM pg_repack_safe_rewrites'),
    '0', "record table excluded: the subscriber's record table stays empty");
is($subscriber->safe_psql('postgres', 'SELECT count(*) FROM victim'),
    '4', 'the table with the same OID on the subscriber is still readable');

# The initial table synchronization. The publisher's record table now holds
# records, and a second publication includes it. A subscription to that one,
# from another database of the subscriber, copies the record table with
# COPY ... FROM (tablesync.c:1214-1217), which the trigger refuses as well.
$publisher->safe_psql('postgres',
    'CREATE PUBLICATION pub_sync FOR ALL TABLES');
$subscriber->safe_psql('postgres', 'CREATE DATABASE sync');
$subscriber->safe_psql('sync', q{
    CREATE EXTENSION pg_repack_safe;
    CREATE TABLE t (id int PRIMARY KEY);
    CREATE TABLE other (id int PRIMARY KEY);
    CREATE TABLE progress (id int PRIMARY KEY);
});
$logOffset = -s $subscriber->logfile;
$subscriber->safe_psql('sync',
    "CREATE SUBSCRIPTION sub_sync CONNECTION '$connstr' PUBLICATION pub_sync");

# The other tables are copied; the record table's copy fails and is retried.
my $syncErrors = q{(SELECT sync_table_error_count
                    FROM pg_stat_subscription_stats
                    WHERE subname = 'sub_sync')};
$subscriber->poll_query_until('sync', qq{
    SELECT $syncErrors >= 2
       AND (SELECT count(*) = 2 FROM pg_subscription_rel
            WHERE srsubstate = 'r'
              AND srrelid IN ('t'::regclass, 'other'::regclass))
}) or die 'the initial synchronization did not reach the expected state';
$log = slurp_file($subscriber->logfile, $logOffset);
like($log,
    qr/logical replication tablesync worker\[\d+\] ERROR:  cannot insert into the pg_repack_safe record table/,
    'the table synchronization worker reports the refused record');
like($log,
    qr/logical replication tablesync worker\[\d+\] CONTEXT:  COPY pg_repack_safe_rewrites, line 1/,
    "the report names the record table's COPY");
is($subscriber->safe_psql('sync',
        'SELECT count(*) FROM pg_repack_safe_rewrites'),
    '0', "initial synchronization: the subscriber's record table stays empty");
is($subscriber->safe_psql('sync', 'SELECT count(*) FROM t'),
    '6', 'initial synchronization: the other tables are copied');
is($subscriber->safe_psql('sync', q{
        SELECT srsubstate FROM pg_subscription_rel
        WHERE srrelid = 'pg_repack_safe_rewrites'::regclass}),
    'd', 'initial synchronization: the record table is not synchronized');
$publisher->safe_psql('postgres', 'INSERT INTO other VALUES (3)');
$publisher->wait_for_catchup('sub_sync');
is($subscriber->safe_psql('sync', 'SELECT count(*) FROM other'),
    '3', 'initial synchronization: changes to the other tables arrive');

# The same two recovery steps end the retries.
$publisher->safe_psql('postgres', q{
    ALTER PUBLICATION pub_sync
        SET ALL TABLES EXCEPT (TABLE pg_repack_safe_rewrites)
});
$subscriber->safe_psql('sync',
    'ALTER SUBSCRIPTION sub_sync REFRESH PUBLICATION');
$subscriber->wait_for_subscription_sync($publisher, 'sub_sync', 'sync');
is($subscriber->safe_psql('sync', q{
        SELECT count(*) FROM pg_subscription_rel
        WHERE srrelid = 'pg_repack_safe_rewrites'::regclass}),
    '0', 'after the refresh, the record table is not in the subscription');

# A replicated TRUNCATE. The apply worker truncates without the TRUNCATE
# command's checks (worker.c:3803 calls ExecuteTruncateGuts directly), so
# only the record table's BEFORE TRUNCATE trigger stands between a
# publisher's TRUNCATE of its record table and a subscriber's records.
# A third subscription, in another database of the subscriber, takes the
# record table without the initial copy (copy_data = false), so that the
# table is in the subscription although the publisher's holds records.
$publisher->safe_psql('postgres',
    'CREATE PUBLICATION pub_truncate FOR ALL TABLES');
$subscriber->safe_psql('postgres', 'CREATE DATABASE truncated');
$subscriber->safe_psql('truncated', q{
    CREATE EXTENSION pg_repack_safe;
    CREATE TABLE t (id int PRIMARY KEY);
    CREATE TABLE other (id int PRIMARY KEY);
    CREATE TABLE progress (id int PRIMARY KEY);
    CREATE TABLE local (id int PRIMARY KEY);
    INSERT INTO local SELECT generate_series(1, 3);
});
$subscriber->safe_psql('truncated', 'REPACK (CONCURRENTLY) local');
is($subscriber->safe_psql('truncated',
        'SELECT count(*) FROM pg_repack_safe_rewrites'),
    '1', 'replicated TRUNCATE: the subscriber has a record of its own');
$subscriber->safe_psql('truncated', qq{
    CREATE SUBSCRIPTION sub_truncate CONNECTION '$connstr'
        PUBLICATION pub_truncate WITH (copy_data = false)
});
$logOffset = -s $subscriber->logfile;
$publisher->safe_psql('postgres', q{
    SET pg_repack_safe.allow_unprotected_drop = on;
    TRUNCATE pg_repack_safe_rewrites;
});
$publisher->safe_psql('postgres', 'INSERT INTO other VALUES (4)');

my $truncateErrors = q{(SELECT apply_error_count
                        FROM pg_stat_subscription_stats
                        WHERE subname = 'sub_truncate')};
$subscriber->poll_query_until('truncated', qq{
    SELECT $truncateErrors >= 2
        OR NOT EXISTS (SELECT 1 FROM pg_repack_safe_rewrites)
}) or die 'the subscriber neither refused nor applied the TRUNCATE';
is($subscriber->safe_psql('truncated',
        'SELECT count(*) FROM pg_repack_safe_rewrites'),
    '1', "replicated TRUNCATE: the subscriber's record survives");
$log = slurp_file($subscriber->logfile, $logOffset);
like($log,
    qr/ERROR:  cannot truncate the pg_repack_safe record table while it holds records/,
    'replicated TRUNCATE: the apply worker reports the refusal');
like($log,
    qr/HINT:  On the publisher, exclude the record table from the publication/,
    'replicated TRUNCATE: the hint gives the recovery for a subscription');
like($log,
    qr/CONTEXT:  processing remote data for replication origin "[^"]+" during message type "TRUNCATE"/,
    'replicated TRUNCATE: the report names the TRUNCATE message');
is($subscriber->safe_psql('truncated', 'SELECT count(*) FROM other'),
    '0', 'replicated TRUNCATE: the subscription applies nothing after it');

# The same two recovery steps release the subscription.
$publisher->safe_psql('postgres', q{
    ALTER PUBLICATION pub_truncate
        SET ALL TABLES EXCEPT (TABLE pg_repack_safe_rewrites)
});
$subscriber->safe_psql('truncated',
    'ALTER SUBSCRIPTION sub_truncate REFRESH PUBLICATION WITH (copy_data = false)');
$publisher->wait_for_catchup('sub_truncate');
is($subscriber->safe_psql('truncated', 'SELECT count(*) FROM other'),
    '1', 'replicated TRUNCATE: after the refresh, the later change arrives');
is($subscriber->safe_psql('truncated',
        'SELECT count(*) FROM pg_repack_safe_rewrites'),
    '1', 'replicated TRUNCATE: after the refresh, the record is still there');

$subscriber->stop;
$publisher->stop;
done_testing();
