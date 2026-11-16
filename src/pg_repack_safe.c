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
 *   is refused unless the record table exists and is locked first. COPY
 *   relation TO, which reads outside the executor, gets the reader check
 *   here.
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
#include "access/transam.h"
#include "access/xact.h"
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
 * While the innermost utility statement is REPACK (CONCURRENTLY): the record
 * table, on which this transaction holds RowExclusiveLock. InvalidOid
 * otherwise.
 */
static Oid  rewriterRecordTable = InvalidOid;

/*
 * True while the innermost utility statement is one whose rewrites keep
 * every row version a snapshot may need, with its original xmin and xmax:
 * VACUUM (FULL) and REPACK or CLUSTER without CONCURRENTLY (both parse to
 * RepackStmt). They may rewrite the record table.
 */
static bool inMvccSafeRewriter = false;

/*
 * Backend-local cache of the record table's OID in the current database.
 * recordTableNameHash is the RELNAMENSP hash of the record table's name in
 * the extension's schema as of the last lookup (once recordTableNameHashSet),
 * so that only changes to that pg_class entry clear the cache.
 */
static bool recordTableOidKnown = false;
static Oid  recordTableOid = InvalidOid;
static bool recordTableNameHashSet = false;
static uint32 recordTableNameHash = 0;

/*
 * Backend-local copy of the record table: every (relid, rewrite_xid) pair,
 * deleted row versions included, keyed by relid. recordCacheValid is cleared
 * by any relcache invalidation of the record table (sent by the writer and by
 * the table's statement trigger) and by any change to the cached record
 * table OID; the next reader check then reloads it. recordCacheComplete is
 * set only once a load has scanned the whole table, so a load cut short by
 * an error is never used. recordCacheTable is the record table it was loaded
 * from.
 */
typedef struct RecordCacheEntry
{
    Oid         relid;          /* hash key */
    int         nxids;
    int         maxxids;
    FullTransactionId *xids;
} RecordCacheEntry;

static MemoryContext recordCacheContext = NULL;
static HTAB *recordCache = NULL;
static bool recordCacheValid = false;
static bool recordCacheComplete = false;
static Oid  recordCacheTable = InvalidOid;

static Oid  lookupRecordTable(bool forReader);
static Oid  lockRecordTableForWriter(void);
static PlannedStmt *checkCopyTo(PlannedStmt *pstmt, bool readOnlyTree);

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
 * @brief Find the record table without the OID cache and without errors
 * @param schemaOid Output: the extension's schema, or InvalidOid
 * @return The record table's OID, or InvalidOid if the extension or the
 *         table is missing
 *
 * For DDL-time checks, which must not raise lookupRecordTable's error for a
 * missing table on every DDL statement in a damaged database.
 */
static Oid
findRecordTable(Oid *schemaOid)
{
    Oid         extensionOid = get_extension_oid(EXTENSION_NAME, true);
    Oid         schema;

    *schemaOid = InvalidOid;
    if (!OidIsValid(extensionOid))
        return InvalidOid;
    schema = get_extension_schema(extensionOid);
    *schemaOid = schema;
    return get_relname_relid(RECORD_TABLE_NAME, schema);
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
    Oid         schemaOid;

    if (name == NULL || strcmp(name, RECORD_TABLE_NAME) != 0)
        return false;
    return relid == findRecordTable(&schemaOid);
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
    if (name != NULL ? strcmp(funcName, name) != 0 :
        (strcmp(funcName, CHANGED_TRIGGER_FUNCTION) != 0 &&
         strcmp(funcName, PROTECTED_TRIGGER_FUNCTION) != 0))
        return false;
    (void) findRecordTable(&schemaOid);
    return OidIsValid(schemaOid) && get_func_namespace(funcOid) == schemaOid;
}

/**
 * @brief Refuse to remove protection while the record table holds records
 * @param verb "drop" or "truncate"
 * @param objectDescription The object being removed, for the message
 *
 * Records are never deleted, and any row version in the table may be the
 * record an older snapshot needs, on this server or on a standby, so any
 * block counts as holding records. A superuser can override with
 * pg_repack_safe.allow_unprotected_drop.
 */
static void
refuseUnprotectedRemoval(const char *verb, const char *objectDescription)
{
    Oid         schemaOid;
    Oid         tableOid;
    Relation    rel;
    BlockNumber blocks;

    if (allowUnprotectedDrop)
        return;
    tableOid = findRecordTable(&schemaOid);
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
             strcmp(verb, "truncate") == 0 ?
             errmsg("cannot truncate the pg_repack_safe record table while it holds records") :
             errmsg("cannot drop the pg_repack_safe extension, its record table or its triggers while the record table holds records"),
             errdetail("The statement would %s %s. A snapshot taken before a recorded REPACK (CONCURRENTLY), on this server or on any standby, would then read the rewritten table as empty.",
                       verb, objectDescription),
             errhint("First make sure no snapshot older than the newest record remains on this server or on any standby (see the README, \"Removing the extension\"); then a superuser can set pg_repack_safe.allow_unprotected_drop.")));
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
            isRecordTable(tgrelid) && isRecordTriggerFunction(tgfoid, NULL);
    }

    if (protectedObject)
    {
        ObjectAddress address;

        ObjectAddressSubSet(address, classId, objectId, subId);
        refuseUnprotectedRemoval("drop",
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
    Oid         newToastRelid;

    oldTuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
    if (!HeapTupleIsValid(oldTuple))
        return;
    oldNode = ((Form_pg_class) GETSTRUCT(oldTuple))->relfilenode;
    ReleaseSysCache(oldTuple);

    if (!inMvccSafeRewriter &&
        selfVisibleRelfilenode(relid, &newToastRelid) != oldNode)
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
    if (tgenabled == TRIGGER_FIRES_ALWAYS || !isRecordTable(tgrelid) ||
        !isRecordTriggerFunction(tgfoid, NULL))
        return;

    ObjectAddressSet(address, TriggerRelationId, triggerOid);
    ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("cannot change when %s fires",
                    getObjectDescription(&address, false)),
             errdetail("The record table's triggers keep readers' caches current and its records unchanged, and must fire ALWAYS.")));
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

    switch (access)
    {
        case OAT_DROP:
            protectRecordsFromDrop(classId, objectId, subId);
            break;
        case OAT_TRUNCATE:
            if (isRecordTable(objectId))
                refuseUnprotectedRemoval("truncate", "table " RECORD_TABLE_NAME);
            break;
        case OAT_POST_ALTER:
            if (classId == RelationRelationId && subId == 0 &&
                isRecordTable(objectId))
                protectRecordTableFromRewrite(objectId);
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
    bool        savedInMvccSafeRewriter = inMvccSafeRewriter;
    Oid         recordTable = InvalidOid;
    bool        unsafeRewriter = isConcurrentRepack(pstmt->utilityStmt);

    /*
     * An unsafe rewrite must be recorded, so it runs only once the record
     * table is found and locked. This runs before rewriterRecordTable is set
     * so that an error leaves it unchanged.
     */
    if (unsafeRewriter)
        recordTable = lockRecordTableForWriter();

    /* COPY relation TO reads outside the executor; check it here. */
    if (IsA(pstmt->utilityStmt, CopyStmt) &&
        !((CopyStmt *) pstmt->utilityStmt)->is_from &&
        ((CopyStmt *) pstmt->utilityStmt)->relation != NULL)
    {
        pstmt = checkCopyTo(pstmt, readOnlyTree);
        readOnlyTree = false;
    }

    rewriterRecordTable = recordTable;
    inMvccSafeRewriter = !unsafeRewriter &&
        (IsA(pstmt->utilityStmt, VacuumStmt) ||
         IsA(pstmt->utilityStmt, RepackStmt));

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
    recordCacheValid = false;
}

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
        recordCacheValid = false;
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
 * @param forReader True when called for the reader check
 *
 * The writer forms its rows from this definition and the reader scans the
 * table with heap semantics, so any other definition is refused: two columns
 * (oid, xid8), the heap access method, and permanent storage, which survives
 * a crash and reaches standbys through WAL.
 */
static bool isRecordTriggerFunction(Oid funcOid, const char *name);

/**
 * @brief Tell whether the record table has one of its triggers, firing ALWAYS
 * @param rel The record table
 * @param functionName The trigger function the trigger must call
 * @param before True for a BEFORE trigger, false for AFTER
 * @param events TRIGGER_TYPE_* event bits the trigger must fire on
 * @return True if a statement-level trigger with those properties exists
 */
static bool
hasRecordTrigger(Relation rel, const char *functionName, bool before,
                 int16 events)
{
    TriggerDesc *trigdesc = rel->trigdesc;

    if (trigdesc == NULL)
        return false;
    for (int i = 0; i < trigdesc->numtriggers; i++)
    {
        Trigger    *trigger = &trigdesc->triggers[i];

        if (trigger->tgenabled == TRIGGER_FIRES_ALWAYS &&
            !TRIGGER_FOR_ROW(trigger->tgtype) &&
            (TRIGGER_FOR_BEFORE(trigger->tgtype) != 0) == before &&
            (trigger->tgtype & events) == events &&
            isRecordTriggerFunction(trigger->tgfoid, functionName))
            return true;
    }
    return false;
}

static void
checkRecordTableDefinition(Relation rel, bool forReader)
{
    TupleDesc   desc = RelationGetDescr(rel);

    /*
     * The triggers are checked for here as well as protected by the object
     * access hook, so that a backend without the library, which no hook can
     * stop, cannot leave the table unprotected without every read and
     * REPACK (CONCURRENTLY) refusing. Exemption, extending the developer's
     * binary-upgrade decision (fix-design.md section 6b): pg_restore creates
     * the triggers after the table and runs queries in between
     * (t/003_pg_upgrade.pl fails without it), so readers in binary-upgrade
     * mode skip this part. REPACK (CONCURRENTLY) is not exempt.
     */
    if (!(forReader && IsBinaryUpgrade) &&
        (!hasRecordTrigger(rel, CHANGED_TRIGGER_FUNCTION, false,
                           TRIGGER_TYPE_INSERT | TRIGGER_TYPE_UPDATE |
                           TRIGGER_TYPE_DELETE | TRIGGER_TYPE_TRUNCATE) ||
         !hasRecordTrigger(rel, PROTECTED_TRIGGER_FUNCTION, true,
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

/**
 * @brief Find and lock the record table for an unsafe rewriter
 * @return OID of the record table, on which this transaction then holds
 *         RowExclusiveLock until it ends
 *
 * Refuses the rewrite when the library was not loaded through
 * shared_preload_libraries, and when there is nowhere to record it: the
 * extension is not created in this database, or its record table cannot be
 * found or has another definition. The lock keeps the table from being dropped, renamed or
 * moved to another schema (each needs AccessExclusiveLock) before the swap
 * writes the record. Taking it processes pending invalidation messages, so
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
                checkRecordTableDefinition(rel, false);
                table_close(rel, NoLock);
                return tableOid;
            }
        }
        UnlockRelationOid(tableOid, RowExclusiveLock);
        recordTableOidKnown = false;
    }
}

/**
 * @brief Tell whether a record's rewrite is invisible to a snapshot
 * @param fxid The record's rewrite_xid, a full transaction ID
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
 * The snapshot test comes before the commit-log lookup: an XID it does not
 * report as running is either visible to it or aborted, and in both cases the
 * snapshot sees the rewritten table as stock MVCC would. The current
 * transaction's own rewrite is visible to it.
 */
static bool
rewriteHiddenFromSnapshot(FullTransactionId fxid, Snapshot snapshot)
{
    FullTransactionId next = ReadNextFullTransactionId();
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
 * @return False if the extension or its record table went away meanwhile,
 *         in which case there is nothing to check
 *
 * Invariant: when this returns true, the cache holds every row version of
 * the record table as of a scan that no invalidation of the table followed.
 * The cache is marked valid before the table is locked and scanned; taking
 * the lock processes pending invalidations, and a callback that fires then
 * or later clears the mark, so the load is repeated.
 */
static bool
loadRecordCache(Oid tableOid)
{
    for (;;)
    {
        Relation    rel;
        HASHCTL     ctl;

        CHECK_FOR_INTERRUPTS();

        recordCacheComplete = false;
        recordCacheValid = true;
        recordCacheTable = tableOid;
        rel = try_table_open(tableOid, AccessShareLock);
        if (rel == NULL || !recordTableOidKnown ||
            recordTableOid != tableOid)
        {
            if (rel != NULL)
                table_close(rel, AccessShareLock);
            recordCacheValid = false;
            recordTableOidKnown = false;
            tableOid = lookupRecordTable(true);
            if (!OidIsValid(tableOid))
                return false;
            continue;
        }
        checkRecordTableDefinition(rel, true);

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

        if (RelationGetNumberOfBlocks(rel) > 0)
        {
            TableScanDesc scan;
            HeapTuple   tuple;
            TupleDesc   desc = RelationGetDescr(rel);

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
        }
        table_close(rel, AccessShareLock);

        if (recordCacheValid)
        {
            recordCacheComplete = true;
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
    Oid         tableOid = lookupRecordTable(true);

    if (!OidIsValid(tableOid))
        return false;
    if ((!recordCacheValid || !recordCacheComplete ||
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
 * written, to its commit, so a lock taken after that commit sees the record
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

    entry = hash_search(recordCache, &relid, HASH_FIND, NULL);
    if (entry == NULL)
        return;

    if (snapshot == InvalidSnapshot || !IsMVCCSnapshot(snapshot))
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("pg_repack_safe cannot check table \"%s\" for this query",
                        get_rel_name(relid)),
                 errdetail("The table has an unsafe-rewrite record and the query does not run with an MVCC snapshot.")));

    for (int i = 0; i < entry->nxids; i++)
    {
        if (rewriteHiddenFromSnapshot(entry->xids[i], snapshot))
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
 * @brief Reader check for COPY relation TO
 * @param pstmt Planned statement wrapping a CopyStmt
 * @param readOnlyTree Whether pstmt must not be modified
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
 */
static PlannedStmt *
checkCopyTo(PlannedStmt *pstmt, bool readOnlyTree)
{
    CopyStmt   *stmt = (CopyStmt *) pstmt->utilityStmt;
    Oid         relid;
    List       *relids;
    Snapshot    snapshot;

    relid = RangeVarGetRelid(stmt->relation, AccessShareLock, false);
    if (get_rel_relkind(relid) == RELKIND_PARTITIONED_TABLE)
        relids = find_all_inheritors(relid, AccessShareLock, NULL);
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

/**
 * @brief Trigger function: refuse UPDATE and DELETE of records
 * @return Does not return
 *
 * BEFORE UPDATE OR DELETE, statement level, enabled ALWAYS, so it covers
 * every route to such a change (MERGE, a writable CTE, a function) and
 * session_replication_role. Deleting records safely needs the rule in the
 * design notes (step 6), which nothing implements yet.
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

/**
 * @brief Module load: define GUCs and install the hooks
 */
void
_PG_init(void)
{
    loadedAtServerStart = process_shared_preload_libraries_in_progress;

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
