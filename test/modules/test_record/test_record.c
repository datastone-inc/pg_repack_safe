/*
 * Copyright (c) 2026 dataStone Inc.
 * SPDX-License-Identifier: MIT
 */

/**
 * @file test_record.c
 * @brief Test-only module: write an unsafe-rewrite record with chosen values
 *
 * The extension lets nothing but REPACK (CONCURRENTLY) insert into its
 * record table, and that writer always records the table it rewrote under
 * its own transaction ID. The reader check has cases no REPACK can set up
 * from a test: a record whose transaction aborted, one from another XID
 * epoch, one whose transaction ID is not assigned yet. test_insert_record
 * writes such a record much as the extension's writer does (insertRecord in
 * src/pg_repack_safe.c): simple_heap_insert, which fires no trigger, and a
 * relcache invalidation of the record table, which is what makes readers
 * reload their copy of the records. The writer also keeps its insert from
 * logical decoding; this one does not, so a publication that includes the
 * record table sends it (t/016_publication.pl relies on that).
 *
 * A test creates the function itself:
 *
 *     CREATE FUNCTION test_insert_record(regclass, oid, xid8) RETURNS void
 *     AS 'test_record' LANGUAGE C STRICT;
 *
 * Not part of the extension; installed only by make installcheck.
 */

#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "storage/lockdefs.h"
#include "utils/inval.h"
#include "utils/rel.h"
#include "utils/xid8.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_insert_record);

/**
 * @brief Insert one row (relid, rewrite_xid) into a record table
 * @param recordTable (regclass) The record table
 * @param relid (oid) The record's relid
 * @param rewriteXid (xid8) The record's rewrite_xid
 * @return void
 *
 * The row is formed from the two-column definition the extension creates,
 * so a table with any other definition is refused rather than written to.
 */
Datum
test_insert_record(PG_FUNCTION_ARGS)
{
    Oid         recordTable = PG_GETARG_OID(0);
    Oid         relid = PG_GETARG_OID(1);
    FullTransactionId rewriteXid = PG_GETARG_FULLTRANSACTIONID(2);
    Relation    rel;
    TupleDesc   desc;
    Datum       values[2];
    bool        nulls[2] = {false, false};
    HeapTuple   tuple;

    rel = table_open(recordTable, RowExclusiveLock);
    desc = RelationGetDescr(rel);
    if (rel->rd_rel->relkind != RELKIND_RELATION ||
        rel->rd_tableam != GetHeapamTableAmRoutine() ||
        desc->natts != 2 ||
        TupleDescAttr(desc, 0)->attisdropped ||
        TupleDescAttr(desc, 0)->atttypid != OIDOID ||
        TupleDescAttr(desc, 1)->attisdropped ||
        TupleDescAttr(desc, 1)->atttypid != XID8OID)
        ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                 errmsg("test_insert_record: \"%s\" is not a heap table with the columns (oid, xid8)",
                        RelationGetRelationName(rel))));

    values[0] = ObjectIdGetDatum(relid);
    values[1] = FullTransactionIdGetDatum(rewriteXid);
    tuple = heap_form_tuple(desc, values, nulls);
    simple_heap_insert(rel, tuple);
    heap_freetuple(tuple);

    CacheInvalidateRelcache(rel);
    table_close(rel, RowExclusiveLock);

    PG_RETURN_VOID();
}
