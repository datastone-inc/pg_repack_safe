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
 *   did it.
 * - ProcessUtility_hook: classifies the innermost utility statement. Only
 *   REPACK with the CONCURRENTLY option counts as an unsafe rewriter.
 * - ExecutorStart_hook: after the executor holds its relation locks, looks
 *   for unsafe-rewrite records.
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_class.h"
#include "commands/defrem.h"
#include "commands/extension.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "storage/bufmgr.h"
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
#define TRANSIENT_HEAP_PREFIX "pg_temp_"

void        _PG_init(void);

static object_access_hook_type prevObjectAccessHook = NULL;
static ProcessUtility_hook_type prevProcessUtilityHook = NULL;
static ExecutorStart_hook_type prevExecutorStartHook = NULL;

/* GUC: report every detected storage replacement of a table as a NOTICE. */
static bool traceStorageChanges = false;

/* True while the innermost utility statement is REPACK (CONCURRENTLY). */
static bool inUnsafeRewriter = false;

/* Backend-local cache of the record table's OID in the current database. */
static bool recordTableOidKnown = false;
static Oid  recordTableOid = InvalidOid;

static Oid  lookupRecordTable(bool forReader);

/**
 * @brief Read a relation's relfilenode as the current transaction left it
 * @param relid OID of the relation
 * @return Relfilenode from the pg_class row visible to SnapshotSelf, or
 *         InvalidRelFileNumber when no such row exists
 */
static RelFileNumber
selfVisibleRelfilenode(Oid relid)
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
    tuple = systable_getnext(scan);
    if (HeapTupleIsValid(tuple))
        result = ((Form_pg_class) GETSTRUCT(tuple))->relfilenode;
    systable_endscan(scan);
    table_close(classRel, AccessShareLock);

    return result;
}

/**
 * @brief Detect that the current command replaced a relation's storage
 * @param relid OID of the relation that was just altered
 *
 * Runs at OAT_POST_ALTER, before the command counter advances, so the
 * catalog cache and the relation cache still describe the old storage.
 */
static void
noteRelationAltered(Oid relid)
{
    HeapTuple   oldTuple;
    Form_pg_class oldForm;
    RelFileNumber oldNode;
    RelFileNumber newNode;
    char        relkind;
    NameData    relname;

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

    newNode = selfVisibleRelfilenode(relid);
    if (!RelFileNumberIsValid(newNode) || newNode == oldNode)
        return;

    /*
     * The storage was replaced. Report tables and materialized views only,
     * and not the transient heap on the other side of the swap, whose name
     * is not stable across runs.
     */
    if ((relkind == RELKIND_RELATION || relkind == RELKIND_MATVIEW) &&
        strncmp(NameStr(relname), TRANSIENT_HEAP_PREFIX,
                strlen(TRANSIENT_HEAP_PREFIX)) != 0)
    {
        Relation    rel;
        BlockNumber oldBlocks;

        /* The caller holds the lock under which it replaced the storage. */
        rel = relation_open(relid, NoLock);
        oldBlocks = RelationGetNumberOfBlocks(rel);
        relation_close(rel, NoLock);

        ereport(NOTICE,
                (errmsg("pg_repack_safe: storage of \"%s\" replaced, old storage has %u blocks, unsafe rewriter: %s",
                        NameStr(relname), oldBlocks,
                        inUnsafeRewriter ? "yes" : "no")));
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

    /* The detection's only consumer is the trace NOTICE. */
    if (traceStorageChanges && access == OAT_POST_ALTER &&
        classId == RelationRelationId && subId == 0)
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
    bool        savedInUnsafeRewriter = inUnsafeRewriter;
    bool        unsafeRewriter = isConcurrentRepack(pstmt->utilityStmt);

    /*
     * An unsafe rewrite must be recorded. If the extension exists but its
     * record table cannot be found, there is nowhere to record it, so the
     * lookup raises an error here, in binary-upgrade mode too. This runs
     * before the flag is set so that the error leaves the flag unchanged.
     */
    if (unsafeRewriter)
        (void) lookupRecordTable(false);

    inUnsafeRewriter = unsafeRewriter;

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
        inUnsafeRewriter = savedInUnsafeRewriter;
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
