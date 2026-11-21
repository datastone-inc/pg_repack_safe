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
 *   records and any change to when its triggers fire, and, while the table
 *   holds records, its drop or truncation and the drop of its triggers or
 *   of the extension.
 * - ProcessUtility_hook: classifies the innermost utility statement. Only
 *   REPACK with the CONCURRENTLY option counts as an unsafe rewriter, and it
 *   is refused unless the record table exists and is locked first. COPY
 *   relation TO, which reads outside the executor, gets the reader check
 *   here.
 * - ExecutorStart_hook: after the executor holds its relation locks, looks
 *   for unsafe-rewrite records.
 *
 * Besides the hooks:
 *
 * - a custom WAL resource manager, whose record at the swap of an unsafe
 *   rewrite stops replay on a standby that does not have this library;
 * - the record table's two trigger functions:
 *   pg_repack_safe_records_changed invalidates readers' record caches, and
 *   pg_repack_safe_records_protected refuses UPDATE and DELETE of records.
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
#include "catalog/pg_class.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "catalog/storage.h"
#include "commands/defrem.h"
#include "commands/extension.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
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
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/xid8.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(pg_repack_safe_records_changed);
PG_FUNCTION_INFO_V1(pg_repack_safe_records_protected);

#define EXTENSION_NAME "pg_repack_safe"
#define RECORD_TABLE_NAME "pg_repack_safe_rewrites"
#define CHANGED_TRIGGER_FUNCTION "pg_repack_safe_records_changed"
#define PROTECTED_TRIGGER_FUNCTION "pg_repack_safe_records_protected"

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
 * RepackStmt; gram.y, RepackStmt). They may rewrite the record table. Never
 * true for an unsafe rewriter. processUtilityHook says which statements set
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
 * by the writer and by the table's statement trigger) and by the syscache
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
 * @param name The trigger function's name, or NULL for either of them
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
        : (strcmp(funcName, CHANGED_TRIGGER_FUNCTION) != 0 &&
           strcmp(funcName, PROTECTED_TRIGGER_FUNCTION) != 0))
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
 * @param timing TRIGGER_TYPE_BEFORE or TRIGGER_TYPE_AFTER
 * @param events TRIGGER_TYPE_* event bits, all of which the trigger must
 *        fire on
 * @return True if a statement-level trigger with those properties exists
 *
 * TRIGGER_TYPE_MATCHES compares the level, the timing and the given event
 * bits exactly (pg_trigger.h), so a trigger firing on only some of the
 * events does not match; one firing on more does.
 */
static bool
hasRecordTrigger(Relation rel, const char *functionName, int16 timing,
                 int16 events)
{
    TriggerDesc *trigdesc = rel->trigdesc;

    if (trigdesc == NULL)
        return false;
    for (int i = 0; i < trigdesc->numtriggers; i++)
    {
        Trigger    *trigger = &trigdesc->triggers[i];

        if (trigger->tgenabled == TRIGGER_FIRES_ALWAYS &&
            TRIGGER_TYPE_MATCHES(trigger->tgtype, TRIGGER_TYPE_STATEMENT,
                                 timing, events) &&
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
        (!hasRecordTrigger(rel, CHANGED_TRIGGER_FUNCTION, TRIGGER_TYPE_AFTER,
                           TRIGGER_TYPE_INSERT | TRIGGER_TYPE_UPDATE |
                           TRIGGER_TYPE_DELETE | TRIGGER_TYPE_TRUNCATE) ||
         !hasRecordTrigger(rel, PROTECTED_TRIGGER_FUNCTION,
                           TRIGGER_TYPE_BEFORE,
                           TRIGGER_TYPE_UPDATE | TRIGGER_TYPE_DELETE)))
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("pg_repack_safe cannot use its record table"),
                 errdetail("The table \"%s\" does not have both of its triggers, enabled ALWAYS.",
                           RECORD_TABLE_NAME),
                 errhint("Drop and re-create the extension.")));

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
                 errhint("Drop and re-create the extension.")));
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
 * @brief List a partitioned table's partitions so that the list cannot grow
 * @param relid A partitioned table, on which the caller holds a lock
 * @param attachLocked Output: the partitioned tables locked here with
 *        ShareUpdateExclusiveLock, appended to the list; the caller releases
 *        those locks once the statement has scanned the partitions
 * @return relid and every relation below it, each locked with
 *         AccessShareLock
 *
 * Invariant: until the locks in attachLocked are released, listing the
 * partitions again returns no relation that is not in the returned list.
 * A relation joins the tree only as a partition of one of its partitioned
 * tables, through CREATE TABLE ... PARTITION OF or ALTER TABLE ... ATTACH
 * PARTITION, and both take at least ShareUpdateExclusiveLock on that parent
 * (tablecmds.c, DefineRelation and AlterTableGetLockLevel), which conflicts
 * with itself. So every partitioned table in the tree is locked in that mode
 * and the tree is listed again until a listing finds each of them locked
 * before it began.
 */
static List *
listStablePartitions(Oid relid, List **attachLocked)
{
    LockRelationOid(relid, ShareUpdateExclusiveLock);
    *attachLocked = lappend_oid(*attachLocked, relid);

    for (;;)
    {
        List       *relids;
        bool        lockedMore = false;

        CHECK_FOR_INTERRUPTS();

        relids = find_all_inheritors(relid, AccessShareLock, NULL);
        foreach_oid(child, relids)
        {
            if (get_rel_relkind(child) != RELKIND_PARTITIONED_TABLE ||
                list_member_oid(*attachLocked, child))
                continue;
            LockRelationOid(child, ShareUpdateExclusiveLock);
            *attachLocked = lappend_oid(*attachLocked, child);
            lockedMore = true;
        }
        if (!lockedMore)
            return relids;
        list_free(relids);
    }
}

/**
 * @brief Reader check for COPY relation TO
 * @param pstmt Planned statement wrapping a CopyStmt
 * @param readOnlyTree Whether pstmt must not be modified
 * @param attachLocked Output: see listStablePartitions
 * @return The statement to run: pstmt, or a copy naming the checked
 *         relation with its schema
 *
 * COPY relation TO scans the relation, or every partition of a partitioned
 * table, with the active snapshot, outside the executor (copy.c, DoCopy;
 * copyto.c, CopyRelationTo). COPY (query) TO, and COPY of a relation with
 * row-level security, run a query and are checked in ExecutorStart.
 *
 * The relation and its partitions are locked here with the AccessShareLock
 * DoCopy takes, then checked against the active snapshot. Under READ
 * COMMITTED that snapshot was taken before the lock, so a REPACK that
 * committed while this COPY waited for the lock is caught too. The returned
 * statement names the relation with its schema, and the lock keeps that name
 * from being dropped or renamed, so DoCopy opens the relation checked here.
 * For a partitioned table, DoCopy lists the partitions again and scans that
 * list (copyto.c, BeginCopyTo); listStablePartitions keeps it equal to the
 * list checked here.
 */
static PlannedStmt *
checkCopyTo(PlannedStmt *pstmt, bool readOnlyTree, List **attachLocked)
{
    CopyStmt   *stmt = (CopyStmt *) pstmt->utilityStmt;
    Oid         relid;
    List       *relids;
    Snapshot    snapshot;

    relid = RangeVarGetRelid(stmt->relation, AccessShareLock, false);
    if (get_rel_relkind(relid) == RELKIND_PARTITIONED_TABLE)
        relids = listStablePartitions(relid, attachLocked);
    else
        relids = list_make1_oid(relid);

    if (recordCacheHasRecords())
    {
        snapshot = ActiveSnapshotSet() ? GetActiveSnapshot() : InvalidSnapshot;
        foreach_oid(child, relids)
            checkRelationRecords(child, snapshot);
    }

    if (readOnlyTree)
        pstmt = copyObject(pstmt);
    stmt = (CopyStmt *) pstmt->utilityStmt;
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
 * @brief Find and lock the record table for an unsafe rewriter
 * @return OID of the record table, on which this transaction then holds
 *         RowExclusiveLock until it ends
 *
 * Refuses the rewrite when the library was not loaded through
 * shared_preload_libraries, and when there is nowhere to record it: the
 * extension is not created in this database, or its record table cannot be
 * found or has another definition. The lock keeps the table from being
 * dropped, renamed or moved to another schema (each needs
 * AccessExclusiveLock) before the swap writes the record. Taking it processes pending invalidation messages, so
 * the OID is accepted only if the cache still holds it once the lock is held.
 */
static Oid
lockRecordTableForWriter(void)
{
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

    for (;;)
    {
        Oid         tableOid;
        Relation    rel;

        CHECK_FOR_INTERRUPTS();

        tableOid = lookupRecordTable(RECORD_TABLE_FOR_WRITER);

        if (!OidIsValid(tableOid))
            ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("REPACK (CONCURRENTLY) requires the extension \"%s\" in this database",
                            EXTENSION_NAME),
                     errdetail("The extension records each REPACK (CONCURRENTLY) so that readers with older snapshots get an error instead of wrong results."),
                     errhint("Run CREATE EXTENSION %s.", EXTENSION_NAME)));

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
                 ? errdetail("Process %d, whose transaction started at %s, would get a serialization failure if it read the table after the REPACK. Checked at the swap.",
                             status->st_procpid,
                             timestamptz_to_str(status->st_xact_start_timestamp))
                 : errdetail("Process %d, whose transaction started at %s, would get a serialization failure if it read the table after the REPACK. Checked before the REPACK started.",
                             status->st_procpid,
                             timestamptz_to_str(status->st_xact_start_timestamp)),
                 errhint("Wait for that transaction to end, or set pg_repack_safe.precheck_snapshot_age to -1 to skip this check.")));
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
    simple_heap_insert(rel, tuple);
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
        precheckOldSnapshots(relid, true);
        logUnsafeRewrite(relid);
        insertRecord(relid);
        if (OidIsValid(newToastRelid))
            insertRecord(newToastRelid);
    }
}

/* ----------------------------------------------------------------
 * Record table protection
 *
 * Refusals of the statements that would remove records or the triggers
 * that guard them.
 * ----------------------------------------------------------------
 */

/* How a statement would remove the record table's protection. */
typedef enum RemovalKind
{
    REMOVAL_DROP,
    REMOVAL_TRUNCATE
} RemovalKind;

/**
 * @brief Refuse to remove protection while the record table holds records
 * @param kind Whether the object is being dropped or truncated
 * @param objectDescription The object being removed, for the message
 *
 * Records are never deleted, and any row version in the table may be the
 * record an older snapshot needs, on this server or on a standby, so any
 * block counts as holding records. A superuser can override with
 * pg_repack_safe.allow_unprotected_drop.
 *
 * Each message is a complete literal, with nothing but the object's
 * description substituted, so that each can be translated as a whole.
 */
static void
refuseUnprotectedRemoval(RemovalKind kind, const char *objectDescription)
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

    if (kind == REMOVAL_TRUNCATE)
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("cannot truncate the pg_repack_safe record table while it holds records"),
                 errdetail("The statement would truncate %s. A snapshot taken before a recorded REPACK (CONCURRENTLY), on this server or on any standby, would then read the rewritten table as empty.",
                           objectDescription),
                 errhint("First make sure no snapshot older than the newest record remains on this server or on any standby (see the README, \"Removing the extension\"); then a superuser can set pg_repack_safe.allow_unprotected_drop.")));
    else
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("cannot drop the pg_repack_safe extension, its record table or its triggers while the record table holds records"),
                 errdetail("The statement would drop %s. A snapshot taken before a recorded REPACK (CONCURRENTLY), on this server or on any standby, would then read the rewritten table as empty.",
                           objectDescription),
                 errhint("First make sure no snapshot older than the newest record remains on this server or on any standby (see the README, \"Removing the extension\"); then a superuser can set pg_repack_safe.allow_unprotected_drop.")));
}

/**
 * @brief Tell whether a trigger is one of the record table's own triggers
 * @param tgrelid The trigger's table
 * @param tgfoid The trigger's function
 * @return True if the table is the record table and the function is one of
 *         its two trigger functions
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
        refuseUnprotectedRemoval(REMOVAL_DROP,
                                 getObjectDescription(&address, false));
    }
}

/**
 * @brief OAT_POST_ALTER of the record table: refuse an unsafe rewrite
 * @param relid The record table
 *
 * A replacement of the table's storage, detected as in noteRelationAltered,
 * is allowed only under VACUUM (FULL), CLUSTER or REPACK without CONCURRENTLY
 * (inMvccSafeRewriter): they copy every row version a snapshot may need with
 * its original xmin and xmax, and the records' values unchanged. Refused:
 * REPACK (CONCURRENTLY), which re-inserts every row under its own XID, and
 * ALTER TABLE, whose rewrites also do that and can change the values (ALTER
 * COLUMN ... USING); ALTER TABLE ... SET TABLESPACE is refused with them.
 */
static void
protectRecordTableFromRewrite(Oid relid)
{
    HeapTuple   oldTuple;
    RelFileNumber oldNode;

    oldTuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
    if (!HeapTupleIsValid(oldTuple))
        return;
    oldNode = ((Form_pg_class) GETSTRUCT(oldTuple))->relfilenode;
    ReleaseSysCache(oldTuple);

    /*
     * When no self-visible row is found, the comparison fails and the
     * command is refused: the verdict is unknown. noteRelationAltered
     * returns in that case instead, and says why it can.
     */
    if (!inMvccSafeRewriter &&
        selfVisibleRelfilenode(relid, NULL) != oldNode)
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("cannot rewrite the pg_repack_safe record table with this command"),
                 errdetail("Only VACUUM FULL, CLUSTER and REPACK without CONCURRENTLY keep every record unchanged."),
                 errhint("Use VACUUM FULL or REPACK.")));
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
             errdetail("The record table's triggers keep readers' caches current and its records unchanged, and must fire ALWAYS.")));
}

/* ----------------------------------------------------------------
 * Trigger functions of the record table
 * ----------------------------------------------------------------
 */

/**
 * @brief Trigger function: refuse UPDATE and DELETE of records
 * @return Does not return
 *
 * BEFORE UPDATE OR DELETE, statement level, enabled ALWAYS, so it covers
 * every route to such a change (MERGE, a writable CTE, a function) and
 * session_replication_role.
 *
 * TODO(dsharpe 2026-11-21): allow deleting records that no snapshot can
 * need, by the rule in doc/research/fix-design.md (step 6).
 */
Datum
pg_repack_safe_records_protected(PG_FUNCTION_ARGS)
{
    if (!CALLED_AS_TRIGGER(fcinfo))
        ereport(ERROR,
                (errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
                 errmsg("pg_repack_safe_records_protected() must be called as a trigger")));

    ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("cannot update or delete pg_repack_safe records"),
             errdetail("A snapshot taken before a recorded REPACK (CONCURRENTLY), on this server or on a standby, relies on the record.")));

    PG_RETURN_NULL();
}

/**
 * @brief Trigger function: invalidate readers' record caches
 * @return NULL (statement-level AFTER trigger)
 *
 * Fires after any INSERT, UPDATE, DELETE or TRUNCATE of the record table
 * (the trigger is ENABLE ALWAYS, so session_replication_role does not skip
 * it), so changes made by hand reach readers' caches like the writer's own.
 */
Datum
pg_repack_safe_records_changed(PG_FUNCTION_ARGS)
{
    TriggerData *trigdata = (TriggerData *) fcinfo->context;

    if (!CALLED_AS_TRIGGER(fcinfo))
        ereport(ERROR,
                (errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
                 errmsg("pg_repack_safe_records_changed() must be called as a trigger")));

    CacheInvalidateRelcache(trigdata->tg_relation);

    return PointerGetDatum(NULL);
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
 */
static void
objectAccessHook(ObjectAccessType access, Oid classId, Oid objectId,
                 int subId, void *arg)
{
    if (prevObjectAccessHook != NULL)
        prevObjectAccessHook(access, classId, objectId, subId, arg);

    switch (access)
    {
        case OAT_DROP:
            protectRecordsFromDrop(classId, objectId, subId);
            break;
        case OAT_TRUNCATE:
            if (isRecordTable(objectId))
                refuseUnprotectedRemoval(REMOVAL_TRUNCATE,
                                         "table " RECORD_TABLE_NAME);
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
    List       *attachLocked = NIL;

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
        pstmt = checkCopyTo(pstmt, readOnlyTree, &attachLocked);
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

    /*
     * The COPY has scanned its partitions, so partitions may be attached
     * again. After an error, the transaction's abort releases these locks.
     */
    foreach_oid(parent, attachLocked)
        UnlockRelationOid(parent, ShareUpdateExclusiveLock);
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
