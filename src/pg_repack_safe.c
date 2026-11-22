/*
 * Copyright (c) 2026 dataStone Inc.
 * SPDX-License-Identifier: MIT
 */

/**
 * @file pg_repack_safe.c
 * @brief Server hooks that detect unsafe table rewrites and guard readers
 * @author Dave Sharpe
 * @date 2026-11-15
 *
 * Three hooks, each chained to the previous value:
 *
 * - object_access_hook: detects, by meaning, that a relation's storage was
 *   replaced. Invariant: at OAT_POST_ALTER the catalog cache still holds the
 *   relation's pg_class row as it was before the current command, while a
 *   SnapshotSelf scan returns the row the command just wrote: the swap
 *   updates the row and fires the hook without advancing the command counter
 *   in between (repack.c, swap_relation_files), and the catalog cache does
 *   not see a command's changes before that. A differing
 *   relfilenode therefore means the storage was replaced, whichever command
 *   did it. Under an unsafe rewriter, the replacement of a pre-existing
 *   table's non-empty storage is recorded (the writer). The same hook
 *   protects the record table: it refuses a rewrite that would not keep the
 *   records, any change to when its triggers fire, any index on it and any
 *   change to its columns, and, while the table holds records, its drop or
 *   truncation and the drop of its triggers or of the extension.
 * - ProcessUtility_hook: classifies the innermost utility statement. Only
 *   REPACK with the CONCURRENTLY option counts as an unsafe rewriter, and it
 *   is refused unless the record table exists and is locked first. COPY
 *   relation TO, which reads outside the executor, gets the reader check
 *   here; for a partitioned table it is turned into COPY (query) TO, which
 *   the executor hook checks.
 * - ExecutorStart_hook: after the executor holds its relation locks, looks
 *   for unsafe-rewrite records.
 *
 * Besides the hooks:
 *
 * - a custom WAL resource manager, whose record at the swap of an unsafe
 *   rewrite stops replay on a standby that does not have this library;
 * - the record table's two trigger functions:
 *   pg_repack_safe_records_protected refuses UPDATE and DELETE of records
 *   and TRUNCATE of a record table that holds any, and
 *   pg_repack_safe_records_writer_only refuses every INSERT, so that only
 *   the writer in this file, which fires no trigger, adds records.
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "access/xloginsert.h"
#include "access/xlogreader.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_class.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_index.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "catalog/storage.h"
#include "commands/copy.h"
#include "commands/defrem.h"
#include "commands/extension.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "nodes/value.h"
#include "replication/logicalworker.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "utils/backend_status.h"
#include "utils/timestamp.h"
#include "tcop/utility.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/rls.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/xid8.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(pg_repack_safe_records_protected);
PG_FUNCTION_INFO_V1(pg_repack_safe_records_writer_only);
PG_FUNCTION_INFO_V1(pg_repack_safe_prune_records);

#define EXTENSION_NAME "pg_repack_safe"
#define RECORD_TABLE_NAME "pg_repack_safe_rewrites"
#define PROTECTED_TRIGGER_FUNCTION "pg_repack_safe_records_protected"
#define WRITER_ONLY_TRIGGER_FUNCTION "pg_repack_safe_records_writer_only"

/*
 * Custom WAL resource manager. The ID is the one reserved for pg_repack_safe
 * on https://wiki.postgresql.org/wiki/CustomWALResourceManagers. It is part
 * of the WAL format: a server replaying this WAL finds the resource manager
 * by this number, so it never changes.
 */
#define PG_REPACK_SAFE_RMGR_ID 149
#define PG_REPACK_SAFE_RMGR_NAME "pg_repack_safe"
#define XLOG_PG_REPACK_SAFE_UNSAFE_REWRITE 0x00

/* WAL record written at the swap of an unsafe rewrite. */
typedef struct xl_pg_repack_safe_rewrite
{
    Oid         dbOid;          /* database of the rewritten table */
    Oid         relid;          /* the rewritten table */
} xl_pg_repack_safe_rewrite;

void        _PG_init(void);

static object_access_hook_type prevObjectAccessHook = NULL;
static ProcessUtility_hook_type prevProcessUtilityHook = NULL;
static ExecutorStart_hook_type prevExecutorStartHook = NULL;

/*
 * True if this library was loaded through shared_preload_libraries, so that
 * every backend of the server has the reader check from its first query.
 */
static bool loadedAtServerStart = false;

/* GUC: report every detected storage replacement of a table as a NOTICE. */
static bool traceStorageChanges = false;

/*
 * GUC: let DROP and TRUNCATE remove the record table, its triggers or the
 * extension while the record table holds records.
 */
static bool allowUnprotectedDrop = false;

/*
 * GUC: refuse REPACK (CONCURRENTLY) while a session holds a snapshot of at
 * least this age, in milliseconds, that could later read the table (see
 * precheckOldSnapshots). -1 turns the check off; 0 counts any age.
 */
static int  precheckSnapshotAge = -1;

/*
 * While the innermost utility statement is REPACK (CONCURRENTLY): the record
 * table, on which this transaction holds RowExclusiveLock. InvalidOid
 * otherwise, so a valid OID is what marks the statement as an unsafe rewriter
 * (inUnsafeRewriter).
 */
static Oid  rewriterRecordTable = InvalidOid;

/*
 * True while the innermost utility statement is one whose rewrites keep
 * every row version a snapshot may need, with its original xmin and xmax:
 * VACUUM (FULL) and REPACK or CLUSTER without CONCURRENTLY (both parse to
 * RepackStmt; gram.y, RepackStmt). They may rewrite the record table while
 * it holds no deleted record (protectRecordTableFromRewrite). Never true
 * for an unsafe rewriter. processUtilityHook says which statements set
 * it.
 */
static bool inMvccSafeRewriter = false;

/**
 * @brief Tell whether the innermost utility statement is an unsafe rewriter
 * @return True while REPACK (CONCURRENTLY) runs with the record table locked
 */
static inline bool
inUnsafeRewriter(void)
{
    return OidIsValid(rewriterRecordTable);
}

/*
 * Backend-local cache of the record table's OID in the current database.
 * recordTableNameHash is the RELNAMENSP hash of the record table's name in
 * the extension's schema as of the last lookup that found the extension
 * (once recordTableNameHashSet), so that only changes to that pg_class entry
 * clear the cache. A lookup that finds no extension leaves the hash as it
 * was; creating the extension changes pg_extension, which clears the cache
 * whatever the hash.
 */
static bool recordTableOidKnown = false;
static Oid  recordTableOid = InvalidOid;
static bool recordTableNameHashSet = false;
static uint32 recordTableNameHash = 0;

/*
 * Backend-local copy of the record table: every (relid, rewrite_xid) pair,
 * deleted row versions included, keyed by relid. recordCacheState is set to
 * RECORD_CACHE_INVALID by any relcache invalidation of the record table (sent
 * by the writer; no SQL statement changes a record) and by the syscache
 * callback that forgets the cached record table OID
 * (recordTableCacheCallback); the next reader check then reloads it. Only
 * RECORD_CACHE_CURRENT allows the cache to be used.
 *
 * recordCacheTable is the record table the cache was loaded from. The cached
 * OID is also forgotten without touching recordCacheState
 * (lockRecordTableForWriter's retry, lookupRecordTable's missing-table
 * error), so recordCacheHasRecords compares recordCacheTable with the OID it
 * has just looked up on every use: a cache loaded from another table is
 * never used.
 */
typedef enum RecordCacheState
{
    RECORD_CACHE_INVALID,       /* must be loaded before use */
    RECORD_CACHE_LOADING,       /* a load is running, or an error cut one
                                 * short */
    RECORD_CACHE_CURRENT        /* loaded, and no invalidation arrived since
                                 * the load began */
} RecordCacheState;

typedef struct RecordCacheEntry
{
    Oid         relid;          /* hash key */
    int         nxids;
    int         maxxids;
    FullTransactionId *xids;
} RecordCacheEntry;

static MemoryContext recordCacheContext = NULL;
static HTAB *recordCache = NULL;
static RecordCacheState recordCacheState = RECORD_CACHE_INVALID;
static Oid  recordCacheTable = InvalidOid;

/* What a caller needs the record table for (see exemptInBinaryUpgrade). */
typedef enum RecordTableUse
{
    RECORD_TABLE_FOR_READER,    /* the reader check */
    RECORD_TABLE_FOR_WRITER     /* REPACK (CONCURRENTLY) */
} RecordTableUse;

/* ----------------------------------------------------------------
 * Record table: lookup and definition
 *
 * Finding the record table, with and without the backend-local OID cache,
 * and checking that it has the definition the reader and the writer rely
 * on.
 * ----------------------------------------------------------------
 */

/**
 * @brief Find the schema the extension is installed in
 * @return The schema's OID, or InvalidOid if the extension is not created in
 *         this database
 */
static Oid
extensionSchema(void)
{
    Oid         extensionOid = get_extension_oid(EXTENSION_NAME, true);

    if (!OidIsValid(extensionOid))
        return InvalidOid;
    return get_extension_schema(extensionOid);
}

/**
 * @brief Find the record table without the OID cache and without errors
 * @return The record table's OID, or InvalidOid if the extension or the
 *         table is missing
 *
 * For DDL-time checks, which must not raise lookupRecordTable's error for a
 * missing table on every DDL statement in a damaged database.
 */
static Oid
findRecordTableNoError(void)
{
    Oid         schemaOid = extensionSchema();

    if (!OidIsValid(schemaOid))
        return InvalidOid;
    return get_relname_relid(RECORD_TABLE_NAME, schemaOid);
}

/**
 * @brief Tell whether a relation is the record table
 * @param relid Relation OID
 * @return True if it is the table named like the record table in the
 *         extension's schema, which is where every check looks for it
 */
static bool
isRecordTable(Oid relid)
{
    char       *name = get_rel_name(relid);

    if (name == NULL || strcmp(name, RECORD_TABLE_NAME) != 0)
        return false;
    return relid == findRecordTableNoError();
}

/**
 * @brief Tell whether a function is one of the record table's trigger
 *        functions
 * @param funcOid Function OID
 * @param name The trigger function's name, or NULL for any of them
 * @return True if it has that name and lives in the extension's schema
 */
static bool
isRecordTriggerFunction(Oid funcOid, const char *name)
{
    char       *funcName = get_func_name(funcOid);
    Oid         schemaOid;

    if (funcName == NULL)
        return false;
    if (name != NULL
        ? strcmp(funcName, name) != 0
        : (strcmp(funcName, PROTECTED_TRIGGER_FUNCTION) != 0 &&
           strcmp(funcName, WRITER_ONLY_TRIGGER_FUNCTION) != 0))
        return false;
    schemaOid = extensionSchema();
    return OidIsValid(schemaOid) && get_func_namespace(funcOid) == schemaOid;
}

/**
 * @brief Syscache callback: forget the cached record table OID
 * @param arg Unused
 * @param cacheid Cache that was invalidated
 * @param hashvalue Hash of the invalidated entry, or 0 for all
 *
 * Any pg_extension change clears the cache. A pg_class change clears it
 * only if it is a reset (hash 0) or touches an entry with the record table's
 * name in the extension's schema: an update sends invalidations for both the
 * old and the new row, so a rename, schema move or drop of the record table
 * matches, and so does a new table taking that name. Before the first lookup
 * has set the hash, every pg_class change clears it.
 */
static void
recordTableCacheCallback(Datum arg, int cacheid, uint32 hashvalue)
{
    (void) arg;

    if (cacheid == RELNAMENSP && hashvalue != 0 && recordTableNameHashSet &&
        hashvalue != recordTableNameHash)
        return;

    recordTableOidKnown = false;
    recordCacheState = RECORD_CACHE_INVALID;
}

/**
 * @brief Tell whether a record table check is skipped in binary-upgrade mode
 * @param use What the caller needs the record table for
 * @return True for the reader check while the server runs in binary-upgrade
 *         mode
 *
 * The one exemption from "a record table that cannot be used is refused",
 * decided by the developer (fix-design.md section 6b). In binary-upgrade
 * mode pg_dump creates the extension empty (pg_dump.c,
 * binary_upgrade_create_empty_extension), adds the record table in a later
 * statement and its triggers after that, and runs queries in between
 * (t/003_pg_upgrade.pl fails without the exemption). So the reader check
 * passes an extension without its record table, and a record table without
 * its triggers. Only superusers can connect in that mode (postinit.c).
 * REPACK (CONCURRENTLY) is never exempt, so no unsafe rewrite can happen in
 * either state.
 */
static bool
exemptInBinaryUpgrade(RecordTableUse use)
{
    return use == RECORD_TABLE_FOR_READER && IsBinaryUpgrade;
}

/**
 * @brief Find the unsafe-rewrite record table in the current database
 * @param use What the caller needs the table for; decides the
 *        binary-upgrade exemption from the missing-table error
 * @return OID of the record table, or InvalidOid when the extension is not
 *         created in this database, or under that exemption
 *
 * Invariant: the returned OID comes from lookups during which no pg_class
 * invalidation arrived. The catalog lookups can process invalidation
 * messages, so the cache is marked valid before they run; a callback that
 * fires during them clears the mark and the lookup is repeated.
 *
 * An extension without its record table is catalog state that neither the
 * reader check nor an unsafe rewriter can work with, so it raises an error.
 */
static Oid
lookupRecordTable(RecordTableUse use)
{
    while (!recordTableOidKnown)
    {
        Oid         extensionOid;
        Oid         tableOid = InvalidOid;

        CHECK_FOR_INTERRUPTS();

        recordTableOidKnown = true;
        extensionOid = get_extension_oid(EXTENSION_NAME, true);
        if (OidIsValid(extensionOid))
        {
            Oid         schemaOid = get_extension_schema(extensionOid);

            /*
             * Set before the pg_class lookup, so that an invalidation the
             * lookup processes is matched against this schema. One processed
             * earlier was matched against the previous lookup's schema; if
             * the schema changed since, the pg_extension change cleared the
             * cache anyway.
             */
            recordTableNameHash =
                GetSysCacheHashValue2(RELNAMENSP,
                                      PointerGetDatum(RECORD_TABLE_NAME),
                                      ObjectIdGetDatum(schemaOid));
            recordTableNameHashSet = true;
            tableOid = get_relname_relid(RECORD_TABLE_NAME, schemaOid);
        }
        if (!recordTableOidKnown)
            continue;

        if (OidIsValid(extensionOid) && !OidIsValid(tableOid))
        {
            /* Not cached: every query looks again until the state is fixed. */
            recordTableOidKnown = false;

            /* pg_upgrade has created the extension but not yet the table. */
            if (exemptInBinaryUpgrade(use))
                return InvalidOid;

            ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("pg_repack_safe cannot find its record table"),
                     errdetail("The extension \"%s\" exists in this database, but the table \"%s\" is not in its schema.",
                               EXTENSION_NAME, RECORD_TABLE_NAME),
                     errhint("Restore the table's name and schema, or drop and re-create the extension.")));
        }
        recordTableOid = tableOid;
    }

    return recordTableOid;
}

/**
 * @brief Tell whether the record table has one of its triggers, firing ALWAYS
 * @param rel The record table
 * @param functionName The trigger function the trigger must call
 * @param level TRIGGER_TYPE_ROW or TRIGGER_TYPE_STATEMENT
 * @param timing TRIGGER_TYPE_BEFORE or TRIGGER_TYPE_AFTER
 * @param events TRIGGER_TYPE_* event bits, all of which the trigger must
 *        fire on
 * @return True if a trigger with those properties exists
 *
 * TRIGGER_TYPE_MATCHES compares the level, the timing and the given event
 * bits exactly (pg_trigger.h), so a trigger firing on only some of the
 * events does not match; one firing on more does.
 */
static bool
hasRecordTrigger(Relation rel, const char *functionName, int16 level,
                 int16 timing, int16 events)
{
    TriggerDesc *trigdesc = rel->trigdesc;

    if (trigdesc == NULL)
        return false;
    for (int i = 0; i < trigdesc->numtriggers; i++)
    {
        Trigger    *trigger = &trigdesc->triggers[i];

        if (trigger->tgenabled == TRIGGER_FIRES_ALWAYS &&
            TRIGGER_TYPE_MATCHES(trigger->tgtype, level, timing, events) &&
            isRecordTriggerFunction(trigger->tgfoid, functionName))
            return true;
    }
    return false;
}

/**
 * @brief Check that the record table has the definition the writer and the
 *        reader expect
 * @param rel The record table, locked by the caller
 * @param use What the caller needs the table for; decides the
 *        binary-upgrade exemption from the trigger check
 *
 * The writer forms its rows from this definition and the reader scans the
 * table with heap semantics, so any other definition is refused: two columns
 * (oid, xid8), the heap access method, and permanent storage, which survives
 * a crash and reaches standbys through WAL.
 */
static void
checkRecordTableDefinition(Relation rel, RecordTableUse use)
{
    TupleDesc   desc = RelationGetDescr(rel);

    /*
     * The triggers are checked for here as well as protected by the object
     * access hook, so that a backend without the library, which no hook can
     * stop, cannot leave the table unprotected without every read and
     * REPACK (CONCURRENTLY) refusing. Skipped while pg_upgrade has created
     * the table but not yet its triggers.
     */
    if (!exemptInBinaryUpgrade(use) &&
        (!hasRecordTrigger(rel, PROTECTED_TRIGGER_FUNCTION,
                           TRIGGER_TYPE_STATEMENT, TRIGGER_TYPE_BEFORE,
                           TRIGGER_TYPE_UPDATE | TRIGGER_TYPE_DELETE |
                           TRIGGER_TYPE_TRUNCATE) ||
         !hasRecordTrigger(rel, WRITER_ONLY_TRIGGER_FUNCTION,
                           TRIGGER_TYPE_ROW, TRIGGER_TYPE_BEFORE,
                           TRIGGER_TYPE_INSERT)))
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("pg_repack_safe cannot use its record table"),
                 errdetail("The table \"%s\" does not have both of its triggers, enabled ALWAYS.",
                           RECORD_TABLE_NAME),
                 errhint("Drop and re-create the extension. That discards its records, so do it when no transaction older than the last REPACK (CONCURRENTLY) remains; while the record table holds records the drop needs pg_repack_safe.allow_unprotected_drop.")));

    if (rel->rd_rel->relkind != RELKIND_RELATION ||
        rel->rd_tableam != GetHeapamTableAmRoutine() ||
        rel->rd_rel->relpersistence != RELPERSISTENCE_PERMANENT ||
        desc->natts != 2 ||
        TupleDescAttr(desc, 0)->attisdropped ||
        TupleDescAttr(desc, 0)->atttypid != OIDOID ||
        TupleDescAttr(desc, 1)->attisdropped ||
        TupleDescAttr(desc, 1)->atttypid != XID8OID)
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("pg_repack_safe cannot use its record table"),
                 errdetail("The table \"%s\" does not have the definition the extension created.",
                           RECORD_TABLE_NAME),
                 errhint("Drop and re-create the extension. That discards its records, so do it when no transaction older than the last REPACK (CONCURRENTLY) remains; while the record table holds records the drop needs pg_repack_safe.allow_unprotected_drop.")));
}

/* ----------------------------------------------------------------
 * Reader
 *
 * The backend-local copy of the records, and the check of each relation a
 * query or COPY reads against the snapshot it reads with.
 * ----------------------------------------------------------------
 */

/**
 * @brief Relcache callback: forget the record cache when the table changes
 * @param arg Unused
 * @param relid Invalidated relation, or InvalidOid for all relations
 */
static void
recordCacheRelcacheCallback(Datum arg, Oid relid)
{
    (void) arg;

    if (!OidIsValid(relid) || relid == recordCacheTable)
        recordCacheState = RECORD_CACHE_INVALID;
}

/**
 * @brief Tell whether a record's rewrite is invisible to a snapshot
 * @param fxid The record's rewrite_xid, a full transaction ID
 * @param next The next full transaction ID, read after the snapshot was
 *        taken and after the record cache was made current
 * @param snapshot The reader's MVCC snapshot
 * @return True if the rewrite committed but the snapshot does not see it
 *
 * A record at least 2^31 transactions older than the next transaction ID
 * precedes every live snapshot's xmin: transaction IDs are not assigned
 * past xidStopLimit, below oldest datfrozenxid + 2^31 (varsup.c,
 * SetTransactionIdLimit), and datfrozenxid never passes a live snapshot's
 * xmin. Such a record is visible to the snapshot or aborted and is passed
 * without its 32-bit form, which may by then name a newer transaction. Any
 * other record's 32-bit form is unambiguous. A record at or beyond the next
 * transaction ID cannot come from the writer and is refused.
 *
 * next may be older than the true next transaction ID by the time it is
 * used; both tests allow for that. The writer's transaction ID was assigned
 * before its record could be loaded into the cache, so it precedes a next
 * read after that load. And the snapshot was taken before next was read, so
 * its xmin does not exceed next: a record less than 2^31 transactions older
 * than next is less than that much older than the snapshot's xmin, which is
 * what the 32-bit comparison needs.
 *
 * The snapshot test comes before the commit-log lookup: an XID it does not
 * report as running is either visible to it or aborted, and in both cases the
 * snapshot sees the rewritten table as stock MVCC would. The current
 * transaction's own rewrite is visible to it.
 */
static bool
rewriteHiddenFromSnapshot(FullTransactionId fxid, FullTransactionId next,
                          Snapshot snapshot)
{
    TransactionId xid;

    if (!FullTransactionIdPrecedes(fxid, next))
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("pg_repack_safe found an unsafe-rewrite record with a transaction ID that has not been assigned"),
                 errdetail("Record transaction ID %" PRIu64 ", next transaction ID %" PRIu64 ".",
                           U64FromFullTransactionId(fxid),
                           U64FromFullTransactionId(next))));

    if (U64FromFullTransactionId(next) - U64FromFullTransactionId(fxid) >=
        (uint64) (MaxTransactionId >> 1))
        return false;

    xid = XidFromFullTransactionId(fxid);
    if (!XidInMVCCSnapshot(xid, snapshot))
        return false;
    if (TransactionIdIsCurrentTransactionId(xid))
        return false;
    return TransactionIdDidCommit(xid);
}

/**
 * @brief Load the record cache from the record table
 * @param tableOid The record table found by lookupRecordTable
 * @return False if there is no record table to load from, in which case
 *         there is nothing to check: the extension was dropped meanwhile, or
 *         the binary-upgrade exemption applies (exemptInBinaryUpgrade). An
 *         extension left without its record table raises an error instead
 *         (lookupRecordTable).
 *
 * Invariant: when this returns true, the cache holds every row version of
 * the record table as of a scan that no invalidation of the table followed.
 * The state is set to RECORD_CACHE_LOADING before the table is locked and
 * scanned; taking the lock processes pending invalidations, and a callback
 * that fires then or later sets it to RECORD_CACHE_INVALID, so the load is
 * repeated.
 */
static bool
loadRecordCache(Oid tableOid)
{
    for (;;)
    {
        Relation    rel;
        HASHCTL     ctl;
        TableScanDesc scan;
        HeapTuple   tuple;
        TupleDesc   desc;

        CHECK_FOR_INTERRUPTS();

        recordCacheState = RECORD_CACHE_LOADING;
        recordCacheTable = tableOid;
        rel = try_table_open(tableOid, AccessShareLock);
        if (rel == NULL || !recordTableOidKnown ||
            recordTableOid != tableOid)
        {
            if (rel != NULL)
                table_close(rel, AccessShareLock);
            recordCacheState = RECORD_CACHE_INVALID;
            recordTableOidKnown = false;
            tableOid = lookupRecordTable(RECORD_TABLE_FOR_READER);
            if (!OidIsValid(tableOid))
                return false;
            continue;
        }
        checkRecordTableDefinition(rel, RECORD_TABLE_FOR_READER);

        if (recordCacheContext == NULL)
            recordCacheContext =
                AllocSetContextCreate(CacheMemoryContext,
                                      "pg_repack_safe record cache",
                                      ALLOCSET_SMALL_SIZES);
        else
            MemoryContextReset(recordCacheContext);

        ctl.keysize = sizeof(Oid);
        ctl.entrysize = sizeof(RecordCacheEntry);
        ctl.hcxt = recordCacheContext;
        recordCache = hash_create("pg_repack_safe records", 16, &ctl,
                                  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

        desc = RelationGetDescr(rel);
        scan = table_beginscan(rel, SnapshotAny, 0, NULL, SO_NONE);
        while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
        {
            Oid         relid;
            FullTransactionId fxid;
            bool        relidNull;
            bool        xidNull;
            RecordCacheEntry *entry;
            bool        found;

            relid = DatumGetObjectId(heap_getattr(tuple, 1, desc,
                                                  &relidNull));
            fxid = DatumGetFullTransactionId(heap_getattr(tuple, 2, desc,
                                                          &xidNull));
            if (relidNull || xidNull)
                continue;

            entry = hash_search(recordCache, &relid, HASH_ENTER, &found);
            if (!found)
            {
                entry->nxids = 0;
                entry->maxxids = 4;
                entry->xids = MemoryContextAlloc(recordCacheContext,
                                                 entry->maxxids *
                                                 sizeof(FullTransactionId));
            }
            else if (entry->nxids == entry->maxxids)
            {
                entry->maxxids *= 2;
                entry->xids = repalloc(entry->xids,
                                       entry->maxxids *
                                       sizeof(FullTransactionId));
            }
            entry->xids[entry->nxids++] = fxid;
        }
        table_endscan(scan);
        table_close(rel, AccessShareLock);

        if (recordCacheState == RECORD_CACHE_LOADING)
        {
            recordCacheState = RECORD_CACHE_CURRENT;
            return true;
        }
    }
}

/**
 * @brief Make the record cache current and tell whether it holds records
 * @return False if the extension is not created in this database or the
 *         record table holds no record, in which case nothing can be refused
 */
static bool
recordCacheHasRecords(void)
{
    Oid         tableOid = lookupRecordTable(RECORD_TABLE_FOR_READER);

    if (!OidIsValid(tableOid))
        return false;
    if ((recordCacheState != RECORD_CACHE_CURRENT ||
         recordCacheTable != tableOid) &&
        !loadRecordCache(tableOid))
        return false;
    return hash_get_num_entries(recordCache) > 0;
}

/**
 * @brief Refuse a read of one relation whose snapshot cannot see a rewrite
 * @param relid Relation about to be read; the caller holds a lock on it
 * @param snapshot Snapshot the read will use
 *
 * Invariant: raises an error if the relation has a record whose rewrite
 * committed and is not visible to the snapshot. The caller has made the
 * record cache current (recordCacheHasRecords) after taking its lock. A
 * rewriter holds AccessExclusiveLock from the swap, where the record is
 * written, to its commit (repack.c: rebuild_relation_finish_concurrent takes
 * it before swapping, and cluster_rel's header says it is kept until the
 * transaction ends), so a lock taken after that commit sees the record
 * committed. The cache holds every row version of the record table (it is
 * loaded with SnapshotAny), and the rewriter's invalidation clears it before
 * such a lock is granted (insertRecord). Each record is judged against the
 * snapshot every time; the cache holds no verdicts.
 *
 * A matching record that cannot be judged, because the read has no MVCC
 * snapshot, is refused.
 */
static void
checkRelationRecords(Oid relid, Snapshot snapshot)
{
    RecordCacheEntry *entry;
    FullTransactionId next;

    entry = hash_search(recordCache, &relid, HASH_FIND, NULL);
    if (entry == NULL)
        return;

    if (snapshot == InvalidSnapshot || !IsMVCCSnapshot(snapshot))
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("pg_repack_safe cannot check table \"%s\" for this query",
                        get_rel_name(relid)),
                 errdetail("The table has an unsafe-rewrite record and the query does not run with an MVCC snapshot.")));

    /*
     * Read once for all of the relation's records, here and not earlier:
     * rewriteHiddenFromSnapshot needs it read after the cache was made
     * current.
     */
    next = ReadNextFullTransactionId();
    for (int i = 0; i < entry->nxids; i++)
    {
        if (rewriteHiddenFromSnapshot(entry->xids[i], next, snapshot))
            ereport(ERROR,
                    (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
                     errmsg("cannot read table \"%s\" with this snapshot",
                            get_rel_name(relid)),
                     errdetail("REPACK (CONCURRENTLY) rewrote the table after the snapshot was taken, and row visibility for older snapshots was not preserved."),
                     errhint("Retry the transaction.")));
    }
}

/**
 * @brief Reader check for a query: every relation in its range table
 * @param queryDesc Query being started; its relation locks are held
 */
static void
checkUnsafeRewriteRecords(QueryDesc *queryDesc)
{
    if (!recordCacheHasRecords())
        return;

    foreach_node(RangeTblEntry, rte, queryDesc->plannedstmt->rtable)
    {
        if (rte->rtekind == RTE_RELATION)
            checkRelationRecords(rte->relid, queryDesc->snapshot);
    }
}

/**
 * @brief Turn COPY partitioned_table TO into COPY (SELECT ...) TO
 * @param stmt The statement, which the caller may modify
 * @param relid The partitioned table, on which the caller holds
 *        AccessShareLock
 *
 * Invariant: the rewritten statement writes what the relation form writes,
 * and reads only relations that are in a plan's range table, which the
 * reader check in ExecutorStart covers.
 *
 * The relation form lists the partitions when DoCopy starts and scans that
 * list outside the executor (copyto.c, BeginCopyTo and DoCopyTo). A check
 * made here would use a list taken earlier, and a table repacked after the
 * reader's snapshot and attached between the two listings would be scanned
 * unchecked (specs/copy_attach_race.spec). The query form has one listing,
 * the planner's, and the executor scans exactly the relations it locked.
 *
 * The select list is the column list the relation form uses: CopyGetAttnums
 * gives the columns named in the statement, or every column that is neither
 * dropped nor generated, and raises the relation form's errors for a
 * generated, repeated or unknown column (copy.c). The table is named with
 * its schema and the caller's lock keeps that name from being dropped or
 * renamed, so the query reads the relation examined here. Privileges are
 * checked by the executor for the same columns.
 *
 * One difference from the relation form: a partition that is a foreign
 * table is read, where BeginCopyTo raises an error.
 */
static void
copyPartitionedAsQuery(CopyStmt *stmt, Oid relid)
{
    Relation    rel = table_open(relid, NoLock);
    TupleDesc   desc = RelationGetDescr(rel);
    List       *attnums = CopyGetAttnums(desc, rel, stmt->attlist);
    SelectStmt *select = makeNode(SelectStmt);
    RangeVar   *from;

    foreach_int(attnum, attnums)
    {
        ColumnRef  *column = makeNode(ColumnRef);
        ResTarget  *target = makeNode(ResTarget);

        column->fields =
            list_make1(makeString(pstrdup(NameStr(TupleDescAttr(desc, attnum - 1)->attname))));
        column->location = -1;
        target->val = (Node *) column;
        target->location = -1;
        select->targetList = lappend(select->targetList, target);
    }

    from = makeRangeVar(get_namespace_name(RelationGetNamespace(rel)),
                        pstrdup(RelationGetRelationName(rel)), -1);
    from->inh = true;
    select->fromClause = list_make1(from);
    table_close(rel, NoLock);

    stmt->relation = NULL;
    stmt->attlist = NIL;
    stmt->query = (Node *) select;
}

/**
 * @brief Reader check for COPY relation TO
 * @param pstmt Planned statement wrapping a CopyStmt
 * @param readOnlyTree Whether pstmt must not be modified
 * @return The statement to run: a copy of pstmt if it was read-only,
 *         changed as described below
 *
 * COPY relation TO scans the relation with the active snapshot, outside the
 * executor (copy.c, DoCopy; copyto.c, DoCopyTo). COPY (query) TO runs a
 * query and is checked in ExecutorStart.
 *
 * The relation is locked here with the AccessShareLock DoCopy takes. Then:
 * - A partitioned table without row-level security is turned into
 *   COPY (query) TO (copyPartitionedAsQuery), whether or not this database
 *   has records: a record written after this point must be seen too, and
 *   only the executor's check runs late enough.
 * - A partitioned table with row-level security is left as it is: DoCopy
 *   turns it into a query itself, or raises an error when row_security is
 *   off (copy.c, check_enable_rls). Both calls of check_enable_rls run in
 *   this statement, for the same user and settings, so they agree.
 * - Any other relation is checked here against the active snapshot. Under
 *   READ COMMITTED that snapshot was taken before the lock, so a REPACK that
 *   committed while this COPY waited for the lock is caught too. The
 *   statement is made to name the relation with its schema, and the lock
 *   keeps that name from being dropped or renamed, so DoCopy opens the
 *   relation checked here.
 */
static PlannedStmt *
checkCopyTo(PlannedStmt *pstmt, bool readOnlyTree)
{
    CopyStmt   *stmt = (CopyStmt *) pstmt->utilityStmt;
    Oid         relid;

    relid = RangeVarGetRelid(stmt->relation, AccessShareLock, false);

    if (readOnlyTree)
        pstmt = copyObject(pstmt);
    stmt = (CopyStmt *) pstmt->utilityStmt;

    if (get_rel_relkind(relid) == RELKIND_PARTITIONED_TABLE)
    {
        if (check_enable_rls(relid, InvalidOid, true) != RLS_ENABLED)
            copyPartitionedAsQuery(stmt, relid);
        return pstmt;
    }

    if (recordCacheHasRecords())
        checkRelationRecords(relid,
                             ActiveSnapshotSet()
                             ? GetActiveSnapshot() : InvalidSnapshot);

    stmt->relation->schemaname = get_namespace_name(get_rel_namespace(relid));
    stmt->relation->relname = get_rel_name(relid);
    stmt->relation->catalogname = NULL;

    return pstmt;
}

/* ----------------------------------------------------------------
 * Writer
 *
 * What REPACK (CONCURRENTLY) does before it runs (lock the record table,
 * the optional pre-check) and at its swap (the WAL record, the records).
 * ----------------------------------------------------------------
 */

/**
 * @brief Find the record table and lock it for a change to its records
 * @return OID of the record table, on which this transaction then holds
 *         RowExclusiveLock until it ends, or InvalidOid when the extension
 *         is not created in this database
 *
 * The lock keeps the table from being dropped, renamed or moved to another
 * schema (each needs AccessExclusiveLock) while the caller writes or deletes
 * records. Taking it processes pending invalidation messages, so the OID is
 * accepted only if the cache still holds it once the lock is held; the
 * table's definition is checked under the lock. A record table that cannot
 * be found, or has another definition, raises an error (lookupRecordTable,
 * checkRecordTableDefinition).
 */
static Oid
lockRecordTableForChange(void)
{
    for (;;)
    {
        Oid         tableOid;
        Relation    rel;

        CHECK_FOR_INTERRUPTS();

        tableOid = lookupRecordTable(RECORD_TABLE_FOR_WRITER);
        if (!OidIsValid(tableOid))
            return InvalidOid;

        LockRelationOid(tableOid, RowExclusiveLock);
        if (recordTableOidKnown && recordTableOid == tableOid)
        {
            rel = try_table_open(tableOid, NoLock);
            if (rel != NULL)
            {
                checkRecordTableDefinition(rel, RECORD_TABLE_FOR_WRITER);
                table_close(rel, NoLock);
                return tableOid;
            }
        }
        UnlockRelationOid(tableOid, RowExclusiveLock);
        recordTableOidKnown = false;
    }
}

/**
 * @brief Find and lock the record table for an unsafe rewriter
 * @return OID of the record table, locked as by lockRecordTableForChange
 *
 * Refuses the rewrite when the library was not loaded through
 * shared_preload_libraries, and when there is nowhere to record it: the
 * extension is not created in this database, or its record table cannot be
 * found or has another definition. The lock is held from here to the end of
 * the transaction, so the table is still there when the swap writes the
 * record.
 */
static Oid
lockRecordTableForWriter(void)
{
    Oid         tableOid;

    /*
     * A record protects readers only in backends that have the reader
     * check, and only shared_preload_libraries puts it in every backend.
     */
    if (!loadedAtServerStart)
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("REPACK (CONCURRENTLY) requires pg_repack_safe in shared_preload_libraries"),
                 errdetail("The library was loaded in this session only, so other sessions may not check their reads against the rewrite."),
                 errhint("Add pg_repack_safe to shared_preload_libraries and restart the server.")));

    tableOid = lockRecordTableForChange();
    if (!OidIsValid(tableOid))
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("REPACK (CONCURRENTLY) requires the extension \"%s\" in this database",
                        EXTENSION_NAME),
                 errdetail("The extension records each REPACK (CONCURRENTLY) so that readers with older snapshots get an error instead of wrong results."),
                 errhint("Run CREATE EXTENSION %s.", EXTENSION_NAME)));

    return tableOid;
}

/**
 * @brief List the processes holding a lock on a relation
 * @param relid Relation in the current database
 * @return PIDs (int List) of the holders and of their lock group leaders
 */
static List *
pidsLockingRelation(Oid relid)
{
    LockData   *lockData = GetLockStatusData();
    List       *pids = NIL;

    for (int i = 0; i < lockData->nelements; i++)
    {
        LockInstanceData *instance = &lockData->locks[i];

        if (instance->holdMask == 0 ||
            instance->locktag.locktag_type != LOCKTAG_RELATION ||
            instance->locktag.locktag_field1 != MyDatabaseId ||
            instance->locktag.locktag_field2 != relid)
            continue;
        pids = list_append_unique_int(pids, instance->pid);
        pids = list_append_unique_int(pids, instance->leaderPid);
    }
    return pids;
}

/**
 * @brief Advisory pre-check: refuse REPACK (CONCURRENTLY) while a session
 *        could later read the table with an older snapshot
 * @param relid The table, or InvalidOid if it could not be resolved
 * @param atSwap True at the swap, where this backend holds
 *        AccessExclusiveLock on the table
 *
 * Best-effort and not part of the guarantee: the reader check refuses such a
 * read either way. This spares long transactions the serialization failure
 * by refusing the REPACK instead. A client session in this database counts
 * if all of these hold:
 * - it holds a snapshot (backend_xmin is set);
 * - the snapshot outlives a statement: the session is idle in a
 *   transaction, or its transaction started before its current statement.
 *   A single statement locks every relation of its plan when it starts, so
 *   if it holds no lock on the table it does not read it (except through a
 *   function that locks lazily). A READ COMMITTED transaction between
 *   statements holds no snapshot. A READ COMMITTED statement after the
 *   first of its transaction counts although its next statement takes a
 *   new snapshot: the isolation level is not visible here, so the check
 *   errs towards refusing;
 * - it holds no lock on the table: REPACK waits for such a session at the
 *   swap, so it cannot be hurt. At the swap no other session holds one;
 * - its transaction started at least pg_repack_safe.precheck_snapshot_age
 *   ago. A snapshot is never older than its transaction.
 * Not seen: sessions on standbys, background workers, and snapshots taken
 * after the check.
 *
 * The error does not identify the session: pg_stat_activity hides another
 * role's process ID and transaction start from users without
 * pg_read_all_stats, and any table owner can set
 * pg_repack_safe.precheck_snapshot_age.
 */
static void
precheckOldSnapshots(Oid relid, bool atSwap)
{
    TimestampTz cutoff;
    List       *lockingPids = NIL;
    int         nbackends;

    if (precheckSnapshotAge < 0)
        return;

    cutoff = TimestampTzPlusMilliseconds(GetCurrentTimestamp(),
                                         -precheckSnapshotAge);
    if (!atSwap && OidIsValid(relid))
        lockingPids = pidsLockingRelation(relid);

    pgstat_clear_backend_activity_snapshot();
    nbackends = pgstat_fetch_stat_numbackends();
    for (int i = 1; i <= nbackends; i++)
    {
        LocalPgBackendStatus *local = pgstat_get_local_beentry_by_index(i);
        PgBackendStatus *status = &local->backendStatus;
        bool        spansStatements;

        if (status->st_procpid <= 0 || status->st_procpid == MyProcPid ||
            status->st_backendType != B_BACKEND ||
            status->st_databaseid != MyDatabaseId ||
            !TransactionIdIsValid(local->backend_xmin) ||
            status->st_xact_start_timestamp == 0)
            continue;

        spansStatements =
            status->st_state == STATE_IDLEINTRANSACTION ||
            status->st_state == STATE_IDLEINTRANSACTION_ABORTED ||
            status->st_xact_start_timestamp <
            status->st_activity_start_timestamp;
        if (!spansStatements ||
            status->st_xact_start_timestamp > cutoff ||
            list_member_int(lockingPids, status->st_procpid))
            continue;

        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_IN_USE),
                 errmsg("REPACK (CONCURRENTLY) refused: another session holds a snapshot that has not read table \"%s\"",
                        OidIsValid(relid) ? get_rel_name(relid) : "?"),
                 atSwap
                 ? errdetail("A session in this database would get a serialization failure if it read the table after the REPACK. Checked at the swap.")
                 : errdetail("A session in this database would get a serialization failure if it read the table after the REPACK. Checked before the REPACK started."),
                 errhint("Wait for that session's transaction to end (pg_stat_activity shows the sessions with a backend_xmin and their xact_start), or set pg_repack_safe.precheck_snapshot_age to -1 to skip this check.")));
    }
    list_free(lockingPids);
}

/**
 * @brief Write the WAL record that marks an unsafe rewrite
 * @param relid The rewritten table
 *
 * Written at the swap, so it precedes the rewriter's commit record. A
 * standby without this library cannot check reads; replaying this record
 * there fails (rmgr.c, RmgrNotFound), so its replay stops before the commit
 * instead of serving the rewritten table to old snapshots. Known gap (open
 * question 17): a standby whose replay starts at a checkpoint between this
 * record and the commit never sees it.
 */
static void
logUnsafeRewrite(Oid relid)
{
    xl_pg_repack_safe_rewrite xlrec;

    xlrec.dbOid = MyDatabaseId;
    xlrec.relid = relid;
    XLogBeginInsert();
    XLogRegisterData(&xlrec, sizeof(xlrec));
    (void) XLogInsert(PG_REPACK_SAFE_RMGR_ID,
                      XLOG_PG_REPACK_SAFE_UNSAFE_REWRITE);
}

/**
 * @brief Redo routine: nothing to apply
 * @param record The record being replayed
 *
 * The records reach standbys as rows of the record table. Replaying this
 * record matters only where the library is missing, where it fails.
 */
static void
pgRepackSafeRedo(XLogReaderState *record)
{
    uint8       info = XLogRecGetInfo(record) & XLR_RMGR_INFO_MASK;

    if (info != XLOG_PG_REPACK_SAFE_UNSAFE_REWRITE)
        elog(PANIC, "pg_repack_safe redo: unknown op code %u", info);
}

/**
 * @brief Describe a record for pg_waldump and redo error context
 * @param buf Output buffer
 * @param record The record
 */
static void
pgRepackSafeDesc(StringInfo buf, XLogReaderState *record)
{
    uint8       info = XLogRecGetInfo(record) & XLR_RMGR_INFO_MASK;

    if (info == XLOG_PG_REPACK_SAFE_UNSAFE_REWRITE)
    {
        xl_pg_repack_safe_rewrite *xlrec =
            (xl_pg_repack_safe_rewrite *) XLogRecGetData(record);

        appendStringInfo(buf, "db %u rel %u", xlrec->dbOid, xlrec->relid);
    }
}

/**
 * @brief Name a record type for pg_waldump
 * @param info Record info bits
 * @return The record type's name, or NULL if unknown
 */
static const char *
pgRepackSafeIdentify(uint8 info)
{
    if ((info & XLR_RMGR_INFO_MASK) == XLOG_PG_REPACK_SAFE_UNSAFE_REWRITE)
        return "UNSAFE_REWRITE";
    return NULL;
}

static const RmgrData pgRepackSafeRmgr = {
    .rm_name = PG_REPACK_SAFE_RMGR_NAME,
    .rm_redo = pgRepackSafeRedo,
    .rm_desc = pgRepackSafeDesc,
    .rm_identify = pgRepackSafeIdentify,
};

/**
 * @brief Write one unsafe-rewrite record
 * @param relid Relation that old snapshots must not read
 *
 * The record carries the current transaction's full (64-bit) ID, whose low
 * 32 bits are the ID the rewrite stamped on every row it copied. The record
 * table (rewriterRecordTable) was locked with RowExclusiveLock when the
 * statement started, so it cannot have been dropped, renamed or moved since,
 * provided this is still that transaction. REPACK (CONCURRENTLY, ANALYZE)
 * commits after the swap and runs ANALYZE in a new transaction (repack.c,
 * process_single_relation), so the lock is checked rather than assumed.
 *
 * The insert fires no trigger and is not decoded for logical replication:
 * with HEAP_INSERT_NO_LOGICAL its WAL record is not marked as carrying a
 * tuple for decoding, and decoding passes over an insert without that mark
 * (heapam.c, heap_insert; decode.c, DecodeInsert). A publication that
 * includes the record table therefore does not send it; the record's OID
 * and transaction ID mean nothing in another cluster. Physical replication
 * and recovery replay the insert like any other.
 */
static void
insertRecord(Oid relid)
{
    Relation    rel;
    Datum       values[2];
    bool        nulls[2] = {false, false};
    HeapTuple   tuple;

    if (!CheckRelationOidLockedByMe(rewriterRecordTable, RowExclusiveLock,
                                    false))
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("pg_repack_safe cannot record an unsafe rewrite outside the transaction that locked its record table")));

    rel = table_open(rewriterRecordTable, NoLock);
    values[0] = ObjectIdGetDatum(relid);
    values[1] = FullTransactionIdGetDatum(GetCurrentFullTransactionId());
    tuple = heap_form_tuple(RelationGetDescr(rel), values, nulls);
    heap_insert(rel, tuple, GetCurrentCommandId(true),
                HEAP_INSERT_NO_LOGICAL, NULL);
    heap_freetuple(tuple);

    /*
     * Readers' record caches must learn of the record. The invalidation is
     * sent when this transaction commits, before it releases its locks
     * (xact.c, CommitTransaction), so a reader that locks the rewritten table
     * after the commit processes it while taking that lock (lmgr.c,
     * LockRelationOid), before its check runs.
     */
    CacheInvalidateRelcache(rel);
    table_close(rel, NoLock);
}

/**
 * @brief Delete every committed record older than the current transaction
 * @param rel The record table, locked by the caller with RowExclusiveLock
 * @return Number of records deleted
 *
 * Deleting marks a row version; it removes nothing. The reader loads every
 * row version (loadRecordCache scans with SnapshotAny), so a deleted record
 * protects exactly as before until VACUUM or page pruning removes it.
 *
 * Invariant: a record this function deletes is not removed while a snapshot
 * that does not see the record's rewrite is alive, on this server or on a
 * standby. Call the record's transaction R, the current one D, and the
 * snapshot S. The function deletes only where R precedes D, compared as
 * full transaction IDs, and where the row was not inserted by D.
 * - S does not see R, so S's xmin does not follow R (snapmgr.c,
 *   XidInMVCCSnapshot), and R precedes D.
 * - On this server a row deleted by D is removed only once D precedes the
 *   oldest xmin of the database's backends (heapam_visibility.c,
 *   HeapTupleSatisfiesVacuumHorizon; pruneheap.c applies the same test on
 *   access), which S's backend holds at or below S's xmin.
 * - On a standby the removal is replayed with a conflict horizon of at
 *   least D, because the row's xmax is not its xmin (heapam.c,
 *   HeapTupleHeaderAdvanceConflictHorizon), and replay first ends every
 *   backend of the database whose xmin does not follow that horizon
 *   (heapam_xlog.c, standby.c: ResolveRecoveryConflictWithSnapshot). A row
 *   deleted by its inserter has no such horizon, which is why the row's
 *   inserter is never the deleter (t/002_standby_record_removal.pl shows
 *   both cases on a plain table).
 *
 * The rewrites that drop a deleted row version without a conflict horizon
 * (VACUUM FULL, CLUSTER, REPACK) are refused while the table holds one
 * (protectRecordTableFromRewrite).
 *
 * The scan uses a new snapshot, so every row it returns was inserted by a
 * committed transaction and is not yet deleted by one. The delete does not
 * wait: a row another transaction is deleting, or has deleted since the
 * snapshot, is left to that transaction, and any result but success means
 * the row is left alone. The delete is not decoded for logical replication:
 * a subscriber's records are its own.
 */
static uint64
deleteOlderRecords(Relation rel)
{
    FullTransactionId ownFxid = GetCurrentFullTransactionId();
    TupleDesc   desc = RelationGetDescr(rel);
    Snapshot    snapshot = RegisterSnapshot(GetLatestSnapshot());
    TableScanDesc scan;
    HeapTuple   tuple;
    uint64      deleted = 0;

    scan = table_beginscan(rel, snapshot, 0, NULL, SO_NONE);
    while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
    {
        FullTransactionId fxid;
        bool        xidNull;
        TM_FailureData tmfd;

        CHECK_FOR_INTERRUPTS();

        fxid = DatumGetFullTransactionId(heap_getattr(tuple, 2, desc,
                                                      &xidNull));
        if (xidNull || !FullTransactionIdPrecedes(fxid, ownFxid))
            continue;
        if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetRawXmin(tuple->t_data)))
            continue;

        if (heap_delete(rel, &tuple->t_self, GetCurrentCommandId(true),
                        TABLE_DELETE_NO_LOGICAL, InvalidSnapshot,
                        false, &tmfd) == TM_Ok)
            deleted++;
    }
    table_endscan(scan);
    UnregisterSnapshot(snapshot);

    return deleted;
}

/**
 * @brief Tell whether the record table holds a deleted row version
 * @param rel The record table, locked by the caller
 * @return True if any row version has a deleting transaction, committed or
 *         not, or one that aborted and has not yet been marked so
 *
 * A row version whose deleter aborted counts until VACUUM or a visibility
 * check marks it: the answer errs towards "holds one".
 */
static bool
holdsDeletedRecord(Relation rel)
{
    TableScanDesc scan;
    HeapTuple   tuple;
    bool        found = false;

    scan = table_beginscan(rel, SnapshotAny, 0, NULL, SO_NONE);
    while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
    {
        uint16      infomask = tuple->t_data->t_infomask;

        if (!(infomask & HEAP_XMAX_INVALID) &&
            !HEAP_XMAX_IS_LOCKED_ONLY(infomask))
        {
            found = true;
            break;
        }
    }
    table_endscan(scan);

    return found;
}

/**
 * @brief Read a relation's storage as the current transaction left it
 * @param relid OID of the relation
 * @param toastRelid Output, unless NULL: reltoastrelid from the same
 *        pg_class row, or InvalidOid when no such row exists
 * @return Relfilenode from the pg_class row visible to SnapshotSelf, or
 *         InvalidRelFileNumber when no such row exists
 *
 * Invariant: if the current transaction wrote the relation's pg_class row,
 * the row it wrote is returned. SnapshotSelf sees a row version whose xmin
 * is the current transaction and which has no xmax, whatever the command
 * counter (heapam_visibility.c, HeapTupleSatisfiesSelf), and no other
 * transaction can update or delete a row version that has not committed.
 *
 * Otherwise the scan can find no row although one exists: SnapshotSelf is
 * not a consistent snapshot, so when another transaction's update of the row
 * (such as a GRANT, which takes no lock on the relation) commits during the
 * scan, the scan can reach the new version before that commit and the old
 * one after it, and see neither.
 */
static RelFileNumber
selfVisibleRelfilenode(Oid relid, Oid *toastRelid)
{
    Relation    classRel;
    ScanKeyData key;
    SysScanDesc scan;
    HeapTuple   tuple;
    RelFileNumber result = InvalidRelFileNumber;

    classRel = table_open(RelationRelationId, AccessShareLock);
    ScanKeyInit(&key, Anum_pg_class_oid, BTEqualStrategyNumber, F_OIDEQ,
                ObjectIdGetDatum(relid));
    scan = systable_beginscan(classRel, ClassOidIndexId, true, SnapshotSelf,
                              1, &key);
    if (toastRelid != NULL)
        *toastRelid = InvalidOid;
    tuple = systable_getnext(scan);
    if (HeapTupleIsValid(tuple))
    {
        result = ((Form_pg_class) GETSTRUCT(tuple))->relfilenode;
        if (toastRelid != NULL)
            *toastRelid = ((Form_pg_class) GETSTRUCT(tuple))->reltoastrelid;
    }
    systable_endscan(scan);
    table_close(classRel, AccessShareLock);

    return result;
}

/**
 * @brief Tell whether a relation storage was created in the current transaction
 * @param locator Storage to look for
 * @return True if the storage is scheduled for deletion should the current
 *         transaction abort
 *
 * Only RelationCreateStorage schedules a deletion at abort (storage.c);
 * storage that existed before the transaction is only ever scheduled for
 * deletion at commit (RelationDropStorage). smgrGetPendingDeletes leaves out
 * temporary relations and entries of enclosing transaction levels, so such
 * storage is reported as not created here, and the caller then treats it as
 * pre-existing: the error is towards recording too much.
 */
static bool
storageCreatedInTransaction(RelFileLocator locator)
{
    RelFileLocator *created;
    int         ncreated;
    bool        found = false;

    ncreated = smgrGetPendingDeletes(false, &created);
    for (int i = 0; i < ncreated; i++)
    {
        if (RelFileLocatorEquals(created[i], locator))
        {
            found = true;
            break;
        }
    }
    if (created != NULL)
        pfree(created);

    return found;
}

/**
 * @brief Detect that the current command replaced a relation's storage
 * @param relid OID of the relation that was just altered
 *
 * Runs at OAT_POST_ALTER, before the command counter advances, so the
 * catalog cache and the relation cache still describe the old storage. The
 * relation cache's storage locator is checked against the catalog cache
 * rather than assumed; a mismatch leaves the old storage unknown and is
 * refused.
 *
 * Only the replacement of a table's or materialized view's storage that
 * existed before the current transaction counts. Storage created in this
 * transaction (such as the copy a REPACK writes into its transient heap,
 * which the swap hands back to the transient heap, whatever its name) holds
 * no row that a snapshot from before this transaction could have seen.
 *
 * Under an unsafe rewriter, a counted replacement of non-empty storage is
 * recorded, together with the TOAST relation the table has after the
 * replacement: REPACK (CONCURRENTLY) swaps TOAST by relation, so that TOAST
 * relation was also written by this transaction. An old storage with zero
 * blocks never held a row, so every snapshot sees the table empty either
 * way and nothing is recorded.
 *
 * Having written its records, the rewrite deletes every older one
 * (deleteOlderRecords), which is what keeps the record table from growing
 * with every REPACK (CONCURRENTLY).
 */
static void
noteRelationAltered(Oid relid)
{
    HeapTuple   oldTuple;
    Form_pg_class oldForm;
    RelFileNumber oldNode;
    RelFileNumber newNode;
    Oid         newToastRelid;
    char        relkind;
    NameData    relname;
    Relation    rel;
    bool        oldCreatedHere;
    BlockNumber oldBlocks = 0;

    oldTuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
    if (!HeapTupleIsValid(oldTuple))
        return;
    oldForm = (Form_pg_class) GETSTRUCT(oldTuple);
    oldNode = oldForm->relfilenode;
    relkind = oldForm->relkind;
    relname = oldForm->relname;
    ReleaseSysCache(oldTuple);

    /* Mapped relations keep relfilenode 0; REPACK rejects them concurrently. */
    if (!RelFileNumberIsValid(oldNode))
        return;

    if (relkind != RELKIND_RELATION && relkind != RELKIND_MATVIEW)
        return;

    /*
     * No self-visible row means the current command did not write the
     * relation's pg_class row (selfVisibleRelfilenode), so it did not
     * replace the storage and there is nothing to record. A command that
     * replaced it wrote the row before this hook ran and without advancing
     * the command counter (repack.c, swap_relation_files).
     * protectRecordTableFromRewrite treats the same case as unknown and
     * refuses, which costs only a spurious error.
     */
    newNode = selfVisibleRelfilenode(relid, &newToastRelid);
    if (!RelFileNumberIsValid(newNode) || newNode == oldNode)
        return;

    /* The caller holds the lock under which it replaced the storage. */
    rel = relation_open(relid, NoLock);
    if (rel->rd_locator.relNumber != oldNode)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("pg_repack_safe cannot determine the previous storage of \"%s\"",
                        NameStr(relname)),
                 errdetail("The relation cache no longer describes the storage being replaced.")));
    oldCreatedHere = storageCreatedInTransaction(rel->rd_locator);
    if (!oldCreatedHere)
        oldBlocks = RelationGetNumberOfBlocks(rel);
    relation_close(rel, NoLock);

    if (oldCreatedHere)
        return;

    if (traceStorageChanges)
        ereport(NOTICE,
                (inUnsafeRewriter()
                 ? errmsg("pg_repack_safe: storage of \"%s\" replaced, old storage has %u blocks, unsafe rewriter: yes",
                          NameStr(relname), oldBlocks)
                 : errmsg("pg_repack_safe: storage of \"%s\" replaced, old storage has %u blocks, unsafe rewriter: no",
                          NameStr(relname), oldBlocks)));

    if (inUnsafeRewriter() && oldBlocks > 0)
    {
        Relation    recordRel;

        precheckOldSnapshots(relid, true);
        logUnsafeRewrite(relid);
        insertRecord(relid);
        if (OidIsValid(newToastRelid))
            insertRecord(newToastRelid);

        /*
         * Older records are deleted here and nowhere else in a rewrite, so
         * that a transaction which deletes records has also written one and
         * holds the lock insertRecord has just checked.
         */
        recordRel = table_open(rewriterRecordTable, NoLock);
        (void) deleteOlderRecords(recordRel);
        table_close(recordRel, NoLock);
    }
}

/* ----------------------------------------------------------------
 * Record table protection
 *
 * Refusals of the statements that would remove records or the triggers
 * that guard them, give the table an index its writer does not maintain, or
 * change its columns. The refusals of UPDATE, DELETE and INSERT are the
 * table's trigger functions, further down.
 * ----------------------------------------------------------------
 */

/**
 * @brief Refuse a DROP that removes protection while the record table holds
 *        records
 * @param objectDescription The object being dropped, for the message
 *
 * Any row version in the table, deleted or not, may be the record an older
 * snapshot needs, on this server or on a standby, so any block counts as
 * holding records. VACUUM returns the table to zero blocks once every
 * record is deleted and no snapshot needs one (deleteOlderRecords). A superuser can override with
 * pg_repack_safe.allow_unprotected_drop. TRUNCATE is refused by the same
 * rule in pg_repack_safe_records_protected.
 */
static void
refuseUnprotectedDrop(const char *objectDescription)
{
    Oid         tableOid;
    Relation    rel;
    BlockNumber blocks;

    if (allowUnprotectedDrop)
        return;
    tableOid = findRecordTableNoError();
    if (!OidIsValid(tableOid))
        return;
    rel = try_table_open(tableOid, AccessShareLock);
    if (rel == NULL)
        return;
    blocks = RelationGetNumberOfBlocks(rel);
    table_close(rel, AccessShareLock);
    if (blocks == 0)
        return;

    ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("cannot drop the pg_repack_safe extension, its record table or its triggers while the record table holds records"),
             errdetail("The statement would drop %s. A snapshot taken before a recorded REPACK (CONCURRENTLY), on this server or on any standby, would then read the rewritten table as empty.",
                       objectDescription),
             errhint("Delete the records with pg_repack_safe_prune_records() and run VACUUM on the record table until it is empty (see the README, \"Removing the extension\"). A superuser can set pg_repack_safe.allow_unprotected_drop instead.")));
}

/**
 * @brief Tell whether a trigger is one of the record table's own triggers
 * @param tgrelid The trigger's table
 * @param tgfoid The trigger's function
 * @return True if the table is the record table and the function is one of
 *         its trigger functions
 *
 * The trigger's name is not part of its identity, so renaming one changes
 * nothing.
 */
static bool
isRecordTrigger(Oid tgrelid, Oid tgfoid)
{
    return isRecordTable(tgrelid) && isRecordTriggerFunction(tgfoid, NULL);
}

/**
 * @brief Read a pg_trigger row as the current command left it
 * @param triggerOid Trigger OID
 * @param tgrelid Output: the trigger's table
 * @param tgfoid Output: the trigger's function
 * @param tgenabled Output: when the trigger fires
 * @return False if no such row is visible
 */
static bool
readTrigger(Oid triggerOid, Oid *tgrelid, Oid *tgfoid, char *tgenabled)
{
    Relation    triggerRel;
    ScanKeyData key;
    SysScanDesc scan;
    HeapTuple   tuple;
    bool        found = false;

    triggerRel = table_open(TriggerRelationId, AccessShareLock);
    ScanKeyInit(&key, Anum_pg_trigger_oid, BTEqualStrategyNumber, F_OIDEQ,
                ObjectIdGetDatum(triggerOid));
    scan = systable_beginscan(triggerRel, TriggerOidIndexId, true,
                              SnapshotSelf, 1, &key);
    tuple = systable_getnext(scan);
    if (HeapTupleIsValid(tuple))
    {
        Form_pg_trigger form = (Form_pg_trigger) GETSTRUCT(tuple);

        *tgrelid = form->tgrelid;
        *tgfoid = form->tgfoid;
        *tgenabled = form->tgenabled;
        found = true;
    }
    systable_endscan(scan);
    table_close(triggerRel, AccessShareLock);

    return found;
}

/**
 * @brief OAT_DROP: refuse to drop the record table, its triggers or the
 *        extension while the record table holds records
 * @param classId Catalog of the object being dropped
 * @param objectId Its OID
 * @param subId Column number, or 0 for the whole object
 *
 * Caught by meaning, at the drop of each object, whatever statement drops it:
 * DROP TABLE, DROP EXTENSION (also after the table was detached from it),
 * DROP TRIGGER, DROP FUNCTION ... CASCADE, DROP SCHEMA ... CASCADE.
 */
static void
protectRecordsFromDrop(Oid classId, Oid objectId, int subId)
{
    bool        protectedObject = false;

    if (classId == RelationRelationId && subId == 0)
        protectedObject = isRecordTable(objectId);
    else if (classId == ExtensionRelationId)
        protectedObject = objectId == get_extension_oid(EXTENSION_NAME, true);
    else if (classId == TriggerRelationId)
    {
        Oid         tgrelid;
        Oid         tgfoid;
        char        tgenabled;

        protectedObject = readTrigger(objectId, &tgrelid, &tgfoid,
                                      &tgenabled) &&
            isRecordTrigger(tgrelid, tgfoid);
    }

    if (protectedObject)
    {
        ObjectAddress address;

        ObjectAddressSubSet(address, classId, objectId, subId);
        refuseUnprotectedDrop(getObjectDescription(&address, false));
    }
}

/**
 * @brief OAT_POST_ALTER of the record table: refuse an unsafe rewrite
 * @param relid The record table
 *
 * A replacement of the table's storage, detected as in noteRelationAltered,
 * is allowed only under VACUUM (FULL), CLUSTER or REPACK without CONCURRENTLY
 * (inMvccSafeRewriter), and only while the old storage holds no deleted row
 * version.
 *
 * Those three copy every row version a snapshot on this server may need,
 * with its original xmin and xmax and the records' values unchanged, and
 * drop the versions no snapshot here needs (heapam_handler.c,
 * heapam_relation_copy_for_cluster). The swap is replayed on a standby
 * without a conflict horizon, so a deleted record dropped this way would be
 * gone for a standby snapshot that still needs it. Plain VACUUM removes the
 * same versions with the horizon (deleteOlderRecords), after which nothing
 * the rewrite drops was ever a record a snapshot needs. The rewriter holds
 * AccessExclusiveLock on the table, so no record is deleted between the
 * check and the swap.
 *
 * Refused whatever the table holds: REPACK (CONCURRENTLY), which re-inserts
 * every row under its own XID, and ALTER TABLE, whose rewrites (such as SET
 * UNLOGGED) also do that; ALTER TABLE ... SET TABLESPACE is refused with
 * them. A rewriting ALTER COLUMN does not get this far
 * (protectRecordTableColumns).
 */
static void
protectRecordTableFromRewrite(Oid relid)
{
    HeapTuple   oldTuple;
    RelFileNumber oldNode;
    Relation    rel;
    bool        deleted;

    oldTuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
    if (!HeapTupleIsValid(oldTuple))
        return;
    oldNode = ((Form_pg_class) GETSTRUCT(oldTuple))->relfilenode;
    ReleaseSysCache(oldTuple);

    /*
     * When no self-visible row is found, the comparison fails and the
     * command is treated as a rewrite: the verdict is unknown.
     * noteRelationAltered returns in that case instead, and says why it can.
     */
    if (selfVisibleRelfilenode(relid, NULL) == oldNode)
        return;

    if (!inMvccSafeRewriter)
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("cannot rewrite the pg_repack_safe record table with this command"),
                 errdetail("Only VACUUM FULL, CLUSTER and REPACK without CONCURRENTLY keep every record unchanged."),
                 errhint("Use VACUUM FULL or REPACK.")));

    /*
     * The relation cache still describes the old storage, as in
     * noteRelationAltered; the caller holds the lock it rewrote under.
     */
    rel = relation_open(relid, NoLock);
    if (rel->rd_locator.relNumber != oldNode)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("pg_repack_safe cannot determine the previous storage of its record table"),
                 errdetail("The relation cache no longer describes the storage being replaced.")));
    deleted = holdsDeletedRecord(rel);
    relation_close(rel, NoLock);

    if (deleted)
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("cannot rewrite the pg_repack_safe record table while it holds deleted records"),
                 errdetail("The rewrite would drop a deleted record without ending the snapshots on standbys that still need it."),
                 errhint("Run VACUUM on the record table first, then repeat the command. VACUUM keeps a deleted record for as long as a snapshot on this server needs it.")));
}

/**
 * @brief OAT_POST_ALTER of a trigger: keep the record table's triggers
 *        firing ALWAYS
 * @param triggerOid The altered trigger
 *
 * ALTER TABLE ... DISABLE TRIGGER, ENABLE TRIGGER and ENABLE REPLICA
 * TRIGGER, in every spelling (a trigger name, ALL, USER), each fire this for
 * every trigger they change (trigger.c, EnableDisableTrigger). Renaming a
 * trigger also fires it and is allowed: the checks identify the triggers by
 * their function.
 */
static void
protectRecordTrigger(Oid triggerOid)
{
    Oid         tgrelid;
    Oid         tgfoid;
    char        tgenabled;
    ObjectAddress address;

    if (!readTrigger(triggerOid, &tgrelid, &tgfoid, &tgenabled))
        return;
    if (tgenabled == TRIGGER_FIRES_ALWAYS ||
        !isRecordTrigger(tgrelid, tgfoid))
        return;

    ObjectAddressSet(address, TriggerRelationId, triggerOid);
    ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("cannot change when %s fires",
                    getObjectDescription(&address, false)),
             errdetail("The record table's triggers refuse every change to the records but the extension's own, and must fire ALWAYS.")));
}

/**
 * @brief Find the table of an index as the current command left it
 * @param relid OID of a relation
 * @return indrelid from the relation's pg_index row visible to SnapshotSelf,
 *         or InvalidOid when no such row exists
 *
 * Invariant: if the current transaction inserted the relation's pg_index
 * row, that row is found, whatever the command counter, and no other
 * transaction can have changed it (selfVisibleRelfilenode gives the
 * argument). A relation that is not an index has no pg_index row.
 */
static Oid
selfVisibleIndexTable(Oid relid)
{
    Relation    indexRel;
    ScanKeyData key;
    SysScanDesc scan;
    HeapTuple   tuple;
    Oid         result = InvalidOid;

    indexRel = table_open(IndexRelationId, AccessShareLock);
    ScanKeyInit(&key, Anum_pg_index_indexrelid, BTEqualStrategyNumber,
                F_OIDEQ, ObjectIdGetDatum(relid));
    scan = systable_beginscan(indexRel, IndexRelidIndexId, true,
                              SnapshotSelf, 1, &key);
    tuple = systable_getnext(scan);
    if (HeapTupleIsValid(tuple))
        result = ((Form_pg_index) GETSTRUCT(tuple))->indrelid;
    systable_endscan(scan);
    table_close(indexRel, AccessShareLock);

    return result;
}

/**
 * @brief OAT_POST_CREATE of a relation: refuse an index on the record table
 * @param relid The relation just created
 *
 * Invariant: the record table has no index. The writer inserts records with
 * heap_insert (insertRecord), which maintains no index, so an index
 * would miss every later record and SQL that uses it would return stale
 * answers. No setting overrides the refusal.
 *
 * Caught by meaning, at the creation of the index, whatever statement
 * creates it: CREATE INDEX with or without CONCURRENTLY, a PRIMARY KEY,
 * UNIQUE or EXCLUDE constraint, REINDEX CONCURRENTLY. Only index_create
 * (index.c) inserts pg_index rows, through UpdateIndexRelation, whose
 * insertion is the only one into that catalog, and it fires this hook for
 * the new relation with subId 0. It does so before advancing the command
 * counter, so the catalog cache does not yet hold the pg_index row and the
 * row is read with SnapshotSelf.
 *
 * The hook also fires for every new table, view and sequence
 * (heap.c, heap_create_with_catalog); those have no pg_index row and pass. The transient heap of
 * a VACUUM FULL, CLUSTER or REPACK of the record table is such a table, and
 * since the record table has no index and no TOAST table, those commands
 * create no index for it.
 */
static void
protectRecordTableFromIndex(Oid relid)
{
    Oid         tableOid = findRecordTableNoError();

    if (!OidIsValid(tableOid) || selfVisibleIndexTable(relid) != tableOid)
        return;

    ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("cannot create an index on the pg_repack_safe record table"),
             errdetail("Records are inserted without maintaining indexes, so an index would miss every later record and a query using it would return stale results.")));
}

/**
 * @brief Refuse to add, drop or alter a column of the record table
 * @param access OAT_POST_CREATE, OAT_DROP or OAT_POST_ALTER
 * @param classId Catalog of the object the hook reports
 * @param objectId Its OID
 * @param subId Column number, or 0 for the whole object
 *
 * Invariant: the record table's columns are the two the extension created.
 * The writer forms its rows from that definition and the reader refuses
 * every query in the database when it finds any other
 * (checkRecordTableDefinition), a state that dropping the column again does
 * not undo, since a dropped column still counts. So the change is refused
 * where it is made, and no setting overrides the refusal:
 * pg_repack_safe.allow_unprotected_drop is for giving up the records, which
 * a column change does not do.
 *
 * Caught by meaning: PostgreSQL reports a column as (pg_class, the table's
 * OID, the column number). In tablecmds.c:
 * - ADD COLUMN: OAT_POST_CREATE (ATExecAddColumn);
 * - DROP COLUMN: OAT_DROP. ATExecDropColumn hands the column's address to
 *   performMultipleDeletions, and every deleted object is reported with its
 *   sub-ID (dependency.c, deleteOneObject);
 * - OAT_POST_ALTER: RENAME COLUMN (renameatt_internal), ALTER COLUMN TYPE
 *   (ATExecAlterColumnType, in every case, also when the new type is binary
 *   coercible and nothing is rewritten), DROP NOT NULL (ATExecDropNotNull),
 *   a SET NOT NULL that adds a constraint (ATExecSetNotNull; the record
 *   table's columns have one, so it changes nothing and is not reported),
 *   ADD, SET and DROP IDENTITY (ATExecAddIdentity, ATExecSetIdentity,
 *   ATExecDropIdentity), SET and DROP EXPRESSION (ATExecSetExpression,
 *   ATExecDropExpression), SET STATISTICS (ATExecSetStatistics), SET and
 *   RESET of attribute options (ATExecSetOptions), SET STORAGE
 *   (ATExecSetStorage) and SET COMPRESSION (ATExecSetCompression).
 * A new column default is reported differently, as OAT_POST_CREATE of
 * (pg_attrdef, the table's OID, the column number), by StoreAttrDefault
 * (pg_attrdef.c), which every route to a default goes through; that is
 * refused as well. DROP DEFAULT then never finds a default to drop.
 *
 * Dropping the whole table, or the extension, reports the table with sub-ID
 * 0 and no column: a column is deleted as an object of its own only when it
 * is the original target or depends on another object being dropped
 * (dependency.c, findDependentObjects, skips a table's own columns when
 * collecting what depends on the table). The record table's columns depend on nothing: their
 * types are pinned, they have no collation, and this function refuses
 * whatever could add a dependency (protection.sql checks pg_depend).
 *
 * Not reported with a column number, and so not refused: COMMENT, SECURITY
 * LABEL and GRANT on a column, and table constraints. None of them changes
 * the column list or what the writer and the reader do.
 */
static void
protectRecordTableColumns(ObjectAccessType access, Oid classId, Oid objectId,
                          int subId)
{
    if (subId == 0)
        return;
    if (classId != RelationRelationId &&
        !(classId == AttrDefaultRelationId && access == OAT_POST_CREATE))
        return;
    if (!isRecordTable(objectId))
        return;

    ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("cannot change the columns of the pg_repack_safe record table"),
             errdetail("The extension reads and writes records as the two columns it created. With any other column list, every query in this database would be refused.")));
}

/* ----------------------------------------------------------------
 * Trigger functions of the record table
 * ----------------------------------------------------------------
 */

/**
 * @brief Trigger function: refuse UPDATE and DELETE of records, and
 *        TRUNCATE of a record table that holds any
 * @return NULL for a TRUNCATE that is allowed; otherwise does not return
 *
 * BEFORE UPDATE OR DELETE OR TRUNCATE, statement level, enabled ALWAYS, so
 * it covers every route to such a change (MERGE, a writable CTE, a
 * function) and session_replication_role.
 *
 * Invariant for TRUNCATE: while the record table has this trigger, firing
 * ALWAYS, no caller of ExecuteTruncateGuts empties a record table that has
 * any block, unless pg_repack_safe.allow_unprotected_drop is set. Any block
 * counts, as for DROP (refuseUnprotectedDrop). ExecuteTruncateGuts fires
 * the BEFORE TRUNCATE triggers of every table it is to truncate, those
 * reached by CASCADE included, before it truncates any, and it has two
 * callers: the TRUNCATE command (tablecmds.c, ExecuteTruncate) and the
 * logical replication apply worker (worker.c, apply_handle_truncate). The
 * apply worker does not pass truncate_check_rel, the one place that reports
 * OAT_TRUNCATE, which is why this is a trigger and not a case of the object
 * access hook.
 *
 * In an apply worker the refusal stops the subscription as a refused record
 * does (pg_repack_safe_records_writer_only), and the hint gives the same
 * recovery.
 *
 * The library's own deletions (deleteOlderRecords) call the heap directly
 * and fire no trigger.
 */
Datum
pg_repack_safe_records_protected(PG_FUNCTION_ARGS)
{
    TriggerData *trigdata = (TriggerData *) fcinfo->context;

    if (!CALLED_AS_TRIGGER(fcinfo))
        ereport(ERROR,
                (errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
                 errmsg("pg_repack_safe_records_protected() must be called as a trigger")));

    if (TRIGGER_FIRED_BY_TRUNCATE(trigdata->tg_event))
    {
        if (allowUnprotectedDrop ||
            RelationGetNumberOfBlocks(trigdata->tg_relation) == 0)
            return PointerGetDatum(NULL);

        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("cannot truncate the pg_repack_safe record table while it holds records"),
                 errdetail("A snapshot taken before a recorded REPACK (CONCURRENTLY), on this server or on any standby, would then read the rewritten table as empty."),
                 IsLogicalWorker()
                 ? errhint("On the publisher, exclude the record table from the publication (ALTER PUBLICATION ... SET ALL TABLES EXCEPT (TABLE %s)); then, on the subscriber, run ALTER SUBSCRIPTION ... REFRESH PUBLICATION. See \"Logical Replication\" in doc/installation.md.",
                           RECORD_TABLE_NAME)
                 : errhint("Delete the records with pg_repack_safe_prune_records() and run VACUUM on the record table until it is empty (see the README, \"Removing the extension\"). A superuser can set pg_repack_safe.allow_unprotected_drop instead.")));
    }

    ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("cannot update or delete pg_repack_safe records"),
             errdetail("A snapshot taken before a recorded REPACK (CONCURRENTLY), on this server or on a standby, relies on the record.")));

    PG_RETURN_NULL();
}

/**
 * @brief Trigger function: refuse every INSERT into the record table
 * @return Does not return
 *
 * Invariant: while the record table has this trigger, firing ALWAYS, no
 * row enters it through the executor, COPY or logical replication, which
 * leaves insertRecord, in this database, for the REPACK (CONCURRENTLY) the
 * record describes. Without the trigger every read and REPACK
 * (CONCURRENTLY) is refused (checkRecordTableDefinition). A record names a
 * table by OID and a transaction by ID, which mean something only in the
 * cluster that wrote them. A row from anywhere else names whichever table
 * has that OID here, and the reader check then refuses reads of it, or
 * every statement on it while the transaction ID is not yet assigned
 * (rewriteHiddenFromSnapshot); nobody could delete the row either
 * (pg_repack_safe_records_protected).
 *
 * BEFORE INSERT, row level, enabled ALWAYS. Every row passes
 * ExecBRInsertTriggers first: INSERT and MERGE (nodeModifyTable.c,
 * ExecInsert, which MERGE's insert action calls from ExecMergeNotMatched),
 * COPY FROM (copyfrom.c, CopyFrom), and logical replication, both the apply
 * worker (execReplication.c, ExecSimpleRelationInsert) and the initial
 * table synchronization, which is a CopyFrom (tablesync.c, copy_table). Row
 * level, because the apply worker fires no statement-level insert trigger
 * (neither worker.c nor execReplication.c calls ExecBSInsertTriggers).
 * ALWAYS, because the apply worker runs with session_replication_role =
 * replica (worker.c, InitializeLogRepWorker), under which a trigger enabled
 * any other way does not fire (trigger.c, TriggerEnabled).
 *
 * insertRecord is not refused: heap_insert fires no trigger, and its record
 * is not sent to subscribers either, so what reaches this trigger through
 * logical replication is the initial copy of a publisher's record table or
 * a row from some other source. Nor are
 * the rewrites that copy the table's rows as they are (VACUUM FULL, CLUSTER,
 * REPACK), pg_upgrade, which copies files, or a standby's replay.
 *
 * The apply worker treats the error like any other: it reports it, exits,
 * and applies the same transaction again when it restarts, so the
 * subscription applies nothing more, for any table, until an operator
 * excludes the record table from the publication and refreshes the
 * subscription, after which the worker passes over the table's changes
 * (worker.c, should_apply_changes_for_rel in apply_handle_insert;
 * t/016_publication.pl, doc/installation.md "Logical
 * Replication"). Raising is the developer's decision: returning NULL here
 * would skip the row and keep the subscription running, but would discard
 * rows without telling anyone.
 */
Datum
pg_repack_safe_records_writer_only(PG_FUNCTION_ARGS)
{
    if (!CALLED_AS_TRIGGER(fcinfo))
        ereport(ERROR,
                (errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
                 errmsg("pg_repack_safe_records_writer_only() must be called as a trigger")));

    ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("cannot insert into the pg_repack_safe record table"),
             errdetail("Only REPACK (CONCURRENTLY) writes records. A record names a table by OID and a transaction by ID, which mean something only in the database cluster that wrote it."),
             errhint("If logical replication delivered this row: on the publisher, exclude the record table from the publication (ALTER PUBLICATION ... SET ALL TABLES EXCEPT (TABLE %s)); then, on the subscriber, run ALTER SUBSCRIPTION ... REFRESH PUBLICATION. See \"Logical Replication\" in doc/installation.md.",
                     RECORD_TABLE_NAME)));

    PG_RETURN_NULL();
}

/**
 * @brief SQL function: delete every record older than the calling
 *        transaction
 * @return Number of records deleted
 *
 * What each REPACK (CONCURRENTLY) does after writing its own record, for a
 * database that has stopped repacking: the last records would otherwise
 * never be deleted, and DROP EXTENSION needs the record table empty. The
 * rule and its argument are deleteOlderRecords'. The deleted records stay
 * in the table until VACUUM removes them.
 */
Datum
pg_repack_safe_prune_records(PG_FUNCTION_ARGS)
{
    Oid         tableOid = lockRecordTableForChange();
    Relation    rel;
    uint64      deleted;

    if (!OidIsValid(tableOid))
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("pg_repack_safe cannot find its record table")));

    rel = table_open(tableOid, NoLock);
    deleted = deleteOlderRecords(rel);
    table_close(rel, NoLock);

    PG_RETURN_INT64((int64) deleted);
}

/* ----------------------------------------------------------------
 * Hooks
 * ----------------------------------------------------------------
 */

/**
 * @brief object_access_hook: detect storage replacements and protect the
 *        record table
 * @param access Kind of access being reported
 * @param classId Catalog the object belongs to
 * @param objectId OID of the object
 * @param subId Column number, or 0 for the whole object
 * @param arg Access-type-specific argument
 *
 * A pg_class alteration is first examined for a storage replacement
 * (noteRelationAltered), then, if it is the record table's, refused unless
 * the rewrite keeps its records (protectRecordTableFromRewrite).
 *
 * A column is reported with its number as subId, by OAT_POST_CREATE,
 * OAT_DROP or OAT_POST_ALTER; protectRecordTableColumns sees all three and
 * refuses the record table's. A new relation is reported by OAT_POST_CREATE
 * with subId 0, and is refused if it is an index on the record table
 * (protectRecordTableFromIndex).
 */
static void
objectAccessHook(ObjectAccessType access, Oid classId, Oid objectId,
                 int subId, void *arg)
{
    if (prevObjectAccessHook != NULL)
        prevObjectAccessHook(access, classId, objectId, subId, arg);

    switch (access)
    {
        case OAT_POST_CREATE:
            if (classId == RelationRelationId && subId == 0)
                protectRecordTableFromIndex(objectId);
            else
                protectRecordTableColumns(access, classId, objectId, subId);
            break;
        case OAT_DROP:
            protectRecordTableColumns(access, classId, objectId, subId);
            protectRecordsFromDrop(classId, objectId, subId);
            break;
        case OAT_POST_ALTER:
            if (classId == RelationRelationId && subId == 0)
            {
                /*
                 * The detection's consumers are the writer and the trace
                 * NOTICE.
                 */
                if (inUnsafeRewriter() || traceStorageChanges)
                    noteRelationAltered(objectId);
                if (isRecordTable(objectId))
                    protectRecordTableFromRewrite(objectId);
            }
            else if (classId == TriggerRelationId)
                protectRecordTrigger(objectId);
            else
                protectRecordTableColumns(access, classId, objectId, subId);
            break;
        default:
            break;
    }
}

/**
 * @brief Tell whether a utility statement is REPACK with CONCURRENTLY
 * @param parsetree Utility statement node
 * @return True for REPACK (CONCURRENTLY) in any option spelling
 *
 * Mirrors the option loop in ExecRepack (repack.c), the only place that
 * sets CLUOPT_CONCURRENT.
 *
 * A CONCURRENTLY value that is not a Boolean raises an error here, through
 * the same defGetBoolean call ExecRepack makes. The statement fails either
 * way; when it has a second fault, this error is reported instead of the one
 * ExecRepack would have reached first.
 */
static bool
isConcurrentRepack(Node *parsetree)
{
    RepackStmt *stmt;
    bool        concurrently = false;

    if (!IsA(parsetree, RepackStmt))
        return false;

    stmt = (RepackStmt *) parsetree;
    foreach_node(DefElem, opt, stmt->params)
    {
        if (strcmp(opt->defname, "concurrently") == 0)
            concurrently = defGetBoolean(opt);
    }

    return concurrently;
}

/**
 * @brief Run the rest of the ProcessUtility chain under a classification
 * @param recordTable rewriterRecordTable for the statement
 * @param mvccSafeRewriter inMvccSafeRewriter for the statement
 * @param pstmt Planned statement wrapping the utility statement
 * @param queryString Source text
 * @param readOnlyTree Whether the tree must not be modified
 * @param context Where the statement came from
 * @param params Bound parameters
 * @param queryEnv Query environment
 * @param dest Result receiver
 * @param qc Completion tag output
 *
 * Sets the two classification variables for the statement and restores
 * the enclosing statement's values however it ends. A function of its own
 * so that nothing it uses after the setjmp in PG_TRY is modified before
 * it (the caller replaces pstmt and readOnlyTree for COPY TO).
 */
static void
runUtilityAs(Oid recordTable, bool mvccSafeRewriter, PlannedStmt *pstmt,
             const char *queryString, bool readOnlyTree,
             ProcessUtilityContext context, ParamListInfo params,
             QueryEnvironment *queryEnv, DestReceiver *dest,
             QueryCompletion *qc)
{
    Oid         savedRewriterRecordTable = rewriterRecordTable;
    bool        savedInMvccSafeRewriter = inMvccSafeRewriter;

    Assert(!(OidIsValid(recordTable) && mvccSafeRewriter));

    rewriterRecordTable = recordTable;
    inMvccSafeRewriter = mvccSafeRewriter;

    PG_TRY();
    {
        if (prevProcessUtilityHook != NULL)
            prevProcessUtilityHook(pstmt, queryString, readOnlyTree, context,
                                   params, queryEnv, dest, qc);
        else
            standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
                                    params, queryEnv, dest, qc);
    }
    PG_FINALLY();
    {
        rewriterRecordTable = savedRewriterRecordTable;
        inMvccSafeRewriter = savedInMvccSafeRewriter;
    }
    PG_END_TRY();
}

/**
 * @brief ProcessUtility_hook: classify the innermost utility statement
 * @param pstmt Planned statement wrapping the utility statement
 * @param queryString Source text
 * @param readOnlyTree Whether the tree must not be modified
 * @param context Where the statement came from
 * @param params Bound parameters
 * @param queryEnv Query environment
 * @param dest Result receiver
 * @param qc Completion tag output
 */
static void
processUtilityHook(PlannedStmt *pstmt, const char *queryString,
                   bool readOnlyTree, ProcessUtilityContext context,
                   ParamListInfo params, QueryEnvironment *queryEnv,
                   DestReceiver *dest, QueryCompletion *qc)
{
    Oid         recordTable = InvalidOid;
    bool        unsafeRewriter = isConcurrentRepack(pstmt->utilityStmt);
    bool        mvccSafeRewriter;

    /*
     * An unsafe rewrite must be recorded, so it runs only once the record
     * table is found and locked. This runs before rewriterRecordTable is set
     * so that an error leaves it unchanged.
     *
     * The refusals here (preload, record table, pre-check, COPY check) run
     * before the previous ProcessUtility hook, so a refused statement never
     * reaches the modules loaded before this one: it must not run at all.
     */
    if (unsafeRewriter)
    {
        RepackStmt *repack = (RepackStmt *) pstmt->utilityStmt;

        recordTable = lockRecordTableForWriter();
        precheckOldSnapshots(repack->relation != NULL &&
                             repack->relation->relation != NULL
                             ? RangeVarGetRelid(repack->relation->relation,
                                                NoLock, true)
                             : InvalidOid,
                             false);
    }

    /* COPY relation TO reads outside the executor; check it here. */
    if (IsA(pstmt->utilityStmt, CopyStmt) &&
        !((CopyStmt *) pstmt->utilityStmt)->is_from &&
        ((CopyStmt *) pstmt->utilityStmt)->relation != NULL)
    {
        pstmt = checkCopyTo(pstmt, readOnlyTree);
        /* The statement is now checkCopyTo's copy, or was modifiable. */
        readOnlyTree = false;
    }

    /*
     * Every VacuumStmt counts, not only VACUUM (FULL): plain VACUUM and
     * ANALYZE parse to the same node and never replace a table's storage
     * (vacuum.c, vacuum_rel calls cluster_rel only under VACOPT_FULL), so
     * the classification has nothing to allow for them.
     */
    mvccSafeRewriter = !unsafeRewriter &&
        (IsA(pstmt->utilityStmt, VacuumStmt) ||
         IsA(pstmt->utilityStmt, RepackStmt));

    runUtilityAs(recordTable, mvccSafeRewriter, pstmt, queryString,
                 readOnlyTree, context, params, queryEnv, dest, qc);
}

/**
 * @brief ExecutorStart_hook: run the reader check once locks are held
 * @param queryDesc Query being started
 * @param eflags Executor flags
 */
static void
executorStartHook(QueryDesc *queryDesc, int eflags)
{
    /* Call through first: the executor takes its relation locks here. */
    if (prevExecutorStartHook != NULL)
        prevExecutorStartHook(queryDesc, eflags);
    else
        standard_ExecutorStart(queryDesc, eflags);

    /* EXPLAIN without ANALYZE reads no rows. */
    if ((eflags & EXEC_FLAG_EXPLAIN_ONLY) == 0)
        checkUnsafeRewriteRecords(queryDesc);
}

/* ----------------------------------------------------------------
 * Module load
 * ----------------------------------------------------------------
 */

/**
 * @brief Module load: define GUCs and install the hooks
 */
void
_PG_init(void)
{
    loadedAtServerStart = process_shared_preload_libraries_in_progress;

    /*
     * Registration is possible only while shared_preload_libraries is being
     * processed (rmgr.c), which is also the only way an unsafe rewrite is
     * allowed to run (lockRecordTableForWriter).
     */
    if (loadedAtServerStart)
        RegisterCustomRmgr(PG_REPACK_SAFE_RMGR_ID, &pgRepackSafeRmgr);

    DefineCustomBoolVariable("pg_repack_safe.trace_storage_changes",
                             "Reports each detected replacement of a table's storage as a NOTICE.",
                             NULL,
                             &traceStorageChanges,
                             false,
                             PGC_SUSET,
                             0,
                             NULL, NULL, NULL);
    DefineCustomBoolVariable("pg_repack_safe.allow_unprotected_drop",
                             "Lets DROP and TRUNCATE remove the record table, its triggers or the extension while records exist.",
                             "Snapshots taken before a recorded REPACK (CONCURRENTLY) may then read the rewritten tables as empty.",
                             &allowUnprotectedDrop,
                             false,
                             PGC_SUSET,
                             0,
                             NULL, NULL, NULL);
    DefineCustomIntVariable("pg_repack_safe.precheck_snapshot_age",
                            "Refuses REPACK (CONCURRENTLY) while a session holds a snapshot this old that has not read the table.",
                            "-1 turns the check off; 0 counts a snapshot of any age. Best-effort: the reader check protects such sessions either way.",
                            &precheckSnapshotAge,
                            -1,
                            -1,
                            INT_MAX,
                            PGC_USERSET,
                            GUC_UNIT_MS,
                            NULL, NULL, NULL);
    MarkGUCPrefixReserved("pg_repack_safe");

    /*
     * The cached record table OID is derived from pg_extension (existence
     * and schema of the extension) and pg_class (the table by name), so a
     * change to the relevant entries clears it (recordTableCacheCallback).
     */
    CacheRegisterSyscacheCallback(RELNAMENSP, recordTableCacheCallback,
                                  (Datum) 0);
    CacheRegisterSyscacheCallback(EXTENSIONOID, recordTableCacheCallback,
                                  (Datum) 0);
    CacheRegisterRelcacheCallback(recordCacheRelcacheCallback, (Datum) 0);

    prevObjectAccessHook = object_access_hook;
    object_access_hook = objectAccessHook;
    prevProcessUtilityHook = ProcessUtility_hook;
    ProcessUtility_hook = processUtilityHook;
    prevExecutorStartHook = ExecutorStart_hook;
    ExecutorStart_hook = executorStartHook;
}
