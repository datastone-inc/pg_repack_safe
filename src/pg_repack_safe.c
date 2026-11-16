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
 *   SnapshotSelf scan returns the row the command just wrote. A differing
 *   relfilenode therefore means the storage was replaced, whichever command
 *   did it. Under an unsafe rewriter, the replacement of a pre-existing
 *   table's non-empty storage is recorded (the writer).
 * - ProcessUtility_hook: classifies the innermost utility statement. Only
 *   REPACK with the CONCURRENTLY option counts as an unsafe rewriter, and it
 *   is refused unless the record table exists and is locked first.
 * - ExecutorStart_hook: after the executor holds its relation locks, looks
 *   for unsafe-rewrite records.
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "catalog/storage.h"
#include "commands/defrem.h"
#include "commands/extension.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "tcop/utility.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

PG_MODULE_MAGIC;

#define EXTENSION_NAME "pg_repack_safe"
#define RECORD_TABLE_NAME "pg_repack_safe_rewrites"

void        _PG_init(void);

static object_access_hook_type prevObjectAccessHook = NULL;
static ProcessUtility_hook_type prevProcessUtilityHook = NULL;
static ExecutorStart_hook_type prevExecutorStartHook = NULL;

/* GUC: report every detected storage replacement of a table as a NOTICE. */
static bool traceStorageChanges = false;

/*
 * While the innermost utility statement is REPACK (CONCURRENTLY): the record
 * table, on which this transaction holds RowExclusiveLock. InvalidOid
 * otherwise.
 */
static Oid  rewriterRecordTable = InvalidOid;

/* Backend-local cache of the record table's OID in the current database. */
static bool recordTableOidKnown = false;
static Oid  recordTableOid = InvalidOid;

static Oid  lookupRecordTable(bool forReader);
static Oid  lockRecordTableForWriter(void);

/**
 * @brief Read a relation's storage as the current transaction left it
 * @param relid OID of the relation
 * @param toastRelid Output: reltoastrelid from the same pg_class row, or
 *        InvalidOid when no such row exists
 * @return Relfilenode from the pg_class row visible to SnapshotSelf, or
 *         InvalidRelFileNumber when no such row exists
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
    *toastRelid = InvalidOid;
    tuple = systable_getnext(scan);
    if (HeapTupleIsValid(tuple))
    {
        result = ((Form_pg_class) GETSTRUCT(tuple))->relfilenode;
        *toastRelid = ((Form_pg_class) GETSTRUCT(tuple))->reltoastrelid;
    }
    systable_endscan(scan);
    table_close(classRel, AccessShareLock);

    return result;
}

/**
 * @brief Write one unsafe-rewrite record
 * @param relid Relation that old snapshots must not read
 *
 * The record carries the current transaction's ID, which is also the ID the
 * rewrite stamped on every row it copied. The record table
 * (rewriterRecordTable) was locked with RowExclusiveLock when the statement
 * started, so it cannot have been dropped, renamed or moved since, provided
 * this is still that transaction. REPACK (CONCURRENTLY, ANALYZE) commits
 * after the swap and runs ANALYZE in a new transaction (repack.c,
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
    values[1] = TransactionIdGetDatum(GetCurrentTransactionId());
    tuple = heap_form_tuple(RelationGetDescr(rel), values, nulls);
    simple_heap_insert(rel, tuple);
    heap_freetuple(tuple);
    table_close(rel, NoLock);
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

    newNode = selfVisibleRelfilenode(relid, &newToastRelid);
    if (!RelFileNumberIsValid(newNode) || newNode == oldNode)
        return;

    if (relkind != RELKIND_RELATION && relkind != RELKIND_MATVIEW)
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
                (errmsg("pg_repack_safe: storage of \"%s\" replaced, old storage has %u blocks, unsafe rewriter: %s",
                        NameStr(relname), oldBlocks,
                        OidIsValid(rewriterRecordTable) ? "yes" : "no")));

    if (OidIsValid(rewriterRecordTable) && oldBlocks > 0)
    {
        insertRecord(relid);
        if (OidIsValid(newToastRelid))
            insertRecord(newToastRelid);
    }
}

/**
 * @brief object_access_hook: watch pg_class alterations for storage swaps
 * @param access Kind of access being reported
 * @param classId Catalog the object belongs to
 * @param objectId OID of the object
 * @param subId Column number, or 0 for the whole object
 * @param arg Access-type-specific argument
 */
static void
objectAccessHook(ObjectAccessType access, Oid classId, Oid objectId,
                 int subId, void *arg)
{
    if (prevObjectAccessHook != NULL)
        prevObjectAccessHook(access, classId, objectId, subId, arg);

    /* The detection's consumers are the writer and the trace NOTICE. */
    if ((OidIsValid(rewriterRecordTable) || traceStorageChanges) &&
        access == OAT_POST_ALTER && classId == RelationRelationId &&
        subId == 0)
        noteRelationAltered(objectId);
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
    Oid         savedRewriterRecordTable = rewriterRecordTable;
    Oid         recordTable = InvalidOid;

    /*
     * An unsafe rewrite must be recorded, so it runs only once the record
     * table is found and locked. This runs before rewriterRecordTable is set
     * so that an error leaves it unchanged.
     */
    if (isConcurrentRepack(pstmt->utilityStmt))
        recordTable = lockRecordTableForWriter();

    rewriterRecordTable = recordTable;

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
    }
    PG_END_TRY();
}

/**
 * @brief Syscache callback: forget the cached record table OID
 * @param arg Unused
 * @param cacheid Cache that was invalidated
 * @param hashvalue Hash of the invalidated entry, or 0 for all
 */
static void
recordTableCacheCallback(Datum arg, int cacheid, uint32 hashvalue)
{
    (void) arg;
    (void) cacheid;
    (void) hashvalue;

    recordTableOidKnown = false;
}

/**
 * @brief Find the unsafe-rewrite record table in the current database
 * @param forReader True when called for the reader check, which alone is
 *        exempt from the missing-table error in binary-upgrade mode
 * @return OID of the record table, or InvalidOid when the extension is not
 *         created in this database
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
lookupRecordTable(bool forReader)
{
    while (!recordTableOidKnown)
    {
        Oid         extensionOid;
        Oid         tableOid = InvalidOid;

        CHECK_FOR_INTERRUPTS();

        recordTableOidKnown = true;
        extensionOid = get_extension_oid(EXTENSION_NAME, true);
        if (OidIsValid(extensionOid))
            tableOid = get_relname_relid(RECORD_TABLE_NAME,
                                         get_extension_schema(extensionOid));
        if (!recordTableOidKnown)
            continue;

        if (OidIsValid(extensionOid) && !OidIsValid(tableOid))
        {
            /* Not cached: every query looks again until the state is fixed. */
            recordTableOidKnown = false;

            /*
             * Exemption, decided by the developer (fix-design.md §6b): in
             * binary-upgrade mode pg_dump creates the extension empty
             * (pg_dump.c, binary_upgrade_create_empty_extension) and adds the
             * record table in a later statement, so this state is expected
             * and the queries in between must pass. Only superusers can
             * connect in that mode (postinit.c). REPACK (CONCURRENTLY) is
             * not exempt, so no unsafe rewrite can happen in this state.
             */
            if (forReader && IsBinaryUpgrade)
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
 * @brief Check that the record table has the definition the writer expects
 * @param rel The record table, locked by the caller
 *
 * The writer forms its rows from this definition and the reader scans the
 * table with heap semantics, so any other definition is refused: two columns
 * (oid, xid), the heap access method, and permanent storage, which survives
 * a crash and reaches standbys through WAL.
 */
static void
checkRecordTableDefinition(Relation rel)
{
    TupleDesc   desc = RelationGetDescr(rel);

    if (rel->rd_rel->relkind != RELKIND_RELATION ||
        rel->rd_tableam != GetHeapamTableAmRoutine() ||
        rel->rd_rel->relpersistence != RELPERSISTENCE_PERMANENT ||
        desc->natts != 2 ||
        TupleDescAttr(desc, 0)->attisdropped ||
        TupleDescAttr(desc, 0)->atttypid != OIDOID ||
        TupleDescAttr(desc, 1)->attisdropped ||
        TupleDescAttr(desc, 1)->atttypid != XIDOID)
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("pg_repack_safe cannot use its record table"),
                 errdetail("The table \"%s\" does not have the definition the extension created.",
                           RECORD_TABLE_NAME),
                 errhint("Drop and re-create the extension.")));
}

/**
 * @brief Find and lock the record table for an unsafe rewriter
 * @return OID of the record table, on which this transaction then holds
 *         RowExclusiveLock until it ends
 *
 * Refuses the rewrite when there is nowhere to record it: the extension is
 * not created in this database, or its record table cannot be found or has
 * another definition. The lock keeps the table from being dropped, renamed or
 * moved to another schema (each needs AccessExclusiveLock) before the swap
 * writes the record. Taking it processes pending invalidation messages, so
 * the OID is accepted only if the cache still holds it once the lock is held.
 */
static Oid
lockRecordTableForWriter(void)
{
    for (;;)
    {
        Oid         tableOid = lookupRecordTable(false);
        Relation    rel;

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
                checkRecordTableDefinition(rel);
                table_close(rel, NoLock);
                return tableOid;
            }
        }
        UnlockRelationOid(tableOid, RowExclusiveLock);
        recordTableOidKnown = false;
    }
}

/**
 * @brief Reader check: look for unsafe-rewrite records
 *
 * Invariant: a record table with zero blocks holds no record, live or
 * deleted, so no relation in this database has a pending unsafe rewrite.
 * Reading the records is not implemented; a non-empty record table cannot be
 * given a verdict, so the query is refused.
 */
static void
checkUnsafeRewriteRecords(void)
{
    Relation    rel;
    BlockNumber blocks;

    /*
     * Taking the lock processes pending invalidation messages. Accept the
     * relation only if the cached OID is still valid once the lock is held;
     * otherwise look the table up again.
     */
    for (;;)
    {
        Oid         tableOid = lookupRecordTable(true);

        if (!OidIsValid(tableOid))
            return;

        rel = try_table_open(tableOid, AccessShareLock);
        if (rel != NULL && recordTableOidKnown)
            break;
        if (rel != NULL)
            table_close(rel, AccessShareLock);
        recordTableOidKnown = false;
    }
    blocks = RelationGetNumberOfBlocks(rel);
    table_close(rel, AccessShareLock);

    if (blocks == 0)
        return;

    /* TODO(dsharpe 2026-11-15): scan the records against the snapshot. */
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("pg_repack_safe cannot check unsafe-rewrite records yet"),
             errdetail("The record table \"%s\" is not empty.",
                       RECORD_TABLE_NAME)));
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

    checkUnsafeRewriteRecords();
}

/**
 * @brief Module load: define GUCs and install the hooks
 */
void
_PG_init(void)
{
    DefineCustomBoolVariable("pg_repack_safe.trace_storage_changes",
                             "Reports each detected replacement of a table's storage as a NOTICE.",
                             NULL,
                             &traceStorageChanges,
                             false,
                             PGC_SUSET,
                             0,
                             NULL, NULL, NULL);
    MarkGUCPrefixReserved("pg_repack_safe");

    /*
     * The cached record table OID is derived from pg_extension (existence
     * and schema of the extension) and pg_class (the table by name), so a
     * change to either catalog clears it.
     */
    CacheRegisterSyscacheCallback(RELNAMENSP, recordTableCacheCallback,
                                  (Datum) 0);
    CacheRegisterSyscacheCallback(EXTENSIONOID, recordTableCacheCallback,
                                  (Datum) 0);

    prevObjectAccessHook = object_access_hook;
    object_access_hook = objectAccessHook;
    prevProcessUtilityHook = ProcessUtility_hook;
    ProcessUtility_hook = processUtilityHook;
    prevExecutorStartHook = ExecutorStart_hook;
    ExecutorStart_hook = executorStartHook;
}
