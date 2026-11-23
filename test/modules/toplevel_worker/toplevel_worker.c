/*
 * Copyright (c) 2026 dataStone Inc.
 * SPDX-License-Identifier: MIT
 */

/**
 * @file toplevel_worker.c
 * @brief Test-only module: run a statement at top level in a background
 *        worker
 *
 * Schedulers such as pg_cron (with cron.use_background_workers) run SQL in
 * a background worker, not in a client backend. A background worker never
 * loads session_preload_libraries or local_preload_libraries: only
 * PostgresMain asks InitPostgres for them (postinit.c, InitPostgres,
 * INIT_PG_LOAD_SESSION_LIBS). t/009_not_preloaded.pl uses this module to
 * show what a REPACK (CONCURRENTLY) run that way does.
 *
 * REPACK (CONCURRENTLY) refuses to run inside a function or a transaction
 * block (repack.c, ExecRepack, PreventInTransactionBlock), so SPI, which
 * worker_spi uses, cannot run it. The worker runs the statement the way
 * postgres.c's exec_simple_query does: parse, analyze, plan, and run an
 * unnamed portal with isTopLevel = true outside a transaction block.
 *
 * A test creates the function itself:
 *
 *     CREATE FUNCTION test_run_in_worker(text) RETURNS text
 *     AS 'toplevel_worker' LANGUAGE C STRICT;
 *
 * It runs the statement in the current database as the current user, waits
 * for the worker to exit, and returns NULL if the statement completed, or
 * the error message if it failed.
 *
 * Not part of the extension; installed only by make installcheck.
 */

#include "postgres.h"

#include <limits.h>

#include "access/xact.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "parser/analyze.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "storage/dsm.h"
#include "storage/ipc.h"
#include "tcop/dest.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/backend_status.h"
#include "utils/memutils.h"
#include "utils/portal.h"
#include "utils/snapmgr.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_run_in_worker);

PGDLLEXPORT void toplevelWorkerMain(Datum mainArg);

#define TOPLEVEL_WORKER_MAX_ERROR 1024

/*
 * Shared between the launching backend and the worker. The launcher fills
 * in everything but completed and errorMessage, which only the worker
 * writes; the launcher reads them after the worker has exited.
 */
typedef struct ToplevelWorkerShared
{
  Oid databaseId;
  Oid userId;
  bool completed;
  char errorMessage[TOPLEVEL_WORKER_MAX_ERROR];
  char sql[FLEXIBLE_ARRAY_MEMBER];
} ToplevelWorkerShared;

/**
 * @brief Run a query string at top level, as exec_simple_query does
 * @param sql The statements to run
 * @param context Memory context that outlives the statements' transactions
 *
 * Plans are built in context, not in a transaction's context, because a
 * statement such as REPACK (CONCURRENTLY) commits and starts transactions
 * while its portal runs.
 */
static void
runToplevel(const char *sql, MemoryContext context)
{
  List *parsetreeList;
  ListCell *cell;
  MemoryContext oldContext;

  StartTransactionCommand();
  oldContext = MemoryContextSwitchTo(context);
  parsetreeList = pg_parse_query(sql);
  MemoryContextSwitchTo(oldContext);

  foreach(cell, parsetreeList)
  {
    RawStmt *parsetree = lfirst_node(RawStmt, cell);
    bool snapshotSet = false;
    List *querytreeList;
    List *plantreeList;
    Portal portal;
    DestReceiver *receiver;
    QueryCompletion qc;

    if (cell != list_head(parsetreeList))
      StartTransactionCommand();

    if (analyze_requires_snapshot(parsetree))
    {
      PushActiveSnapshot(GetTransactionSnapshot());
      snapshotSet = true;
    }

    oldContext = MemoryContextSwitchTo(context);
    querytreeList =
        pg_analyze_and_rewrite_fixedparams(parsetree, sql, NULL, 0, NULL);
    plantreeList =
        pg_plan_queries(querytreeList, sql, CURSOR_OPT_PARALLEL_OK, NULL);
    MemoryContextSwitchTo(oldContext);

    if (snapshotSet)
      PopActiveSnapshot();

    portal = CreatePortal("", true, true);
    portal->visible = false;
    PortalDefineQuery(portal, NULL, sql, CreateCommandTag(parsetree->stmt),
                      plantreeList, NULL);
    PortalStart(portal, NULL, 0, InvalidSnapshot);

    receiver = CreateDestReceiver(DestNone);
    InitializeQueryCompletion(&qc);
    (void)PortalRun(portal, FETCH_ALL, true, receiver, receiver, &qc);
    receiver->rDestroy(receiver);
    PortalDrop(portal, false);

    CommitTransactionCommand();
  }
}

/**
 * @brief Background worker entry point
 * @param mainArg The handle of the dynamic shared memory segment
 */
void
toplevelWorkerMain(Datum mainArg)
{
  dsm_segment *segment;
  ToplevelWorkerShared *shared;
  MemoryContext statementContext;

  pqsignal(SIGTERM, die);
  BackgroundWorkerUnblockSignals();

  segment = dsm_attach(DatumGetUInt32(mainArg));
  if (segment == NULL)
    ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("toplevel_worker: could not attach to the shared memory segment")));
  dsm_pin_mapping(segment);
  shared = (ToplevelWorkerShared *)dsm_segment_address(segment);

  BackgroundWorkerInitializeConnectionByOid(shared->databaseId, shared->userId,
                                            0);

  statementContext =
      AllocSetContextCreate(TopMemoryContext, "toplevel_worker statement",
                            ALLOCSET_DEFAULT_SIZES);

  debug_query_string = shared->sql;
  pgstat_report_activity(STATE_RUNNING, shared->sql);

  PG_TRY();
  {
    runToplevel(shared->sql, statementContext);
  }
  PG_CATCH();
  {
    ErrorData *edata;

    MemoryContextSwitchTo(TopMemoryContext);
    edata = CopyErrorData();
    strlcpy(shared->errorMessage, edata->message, TOPLEVEL_WORKER_MAX_ERROR);
    PG_RE_THROW();
  }
  PG_END_TRY();

  pgstat_report_activity(STATE_IDLE, NULL);
  shared->completed = true;
  proc_exit(0);
}

/**
 * @brief SQL function: run a statement at top level in a background worker
 * @param sql (text) The statement to run
 * @return NULL if it completed, else the error message (text)
 */
Datum
test_run_in_worker(PG_FUNCTION_ARGS)
{
  char *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
  Size sqlLength = strlen(sql) + 1;
  dsm_segment *segment;
  ToplevelWorkerShared *shared;
  BackgroundWorker worker;
  BackgroundWorkerHandle *handle;
  BgwHandleStatus status;
  pid_t pid;
  text *result = NULL;

  segment = dsm_create(offsetof(ToplevelWorkerShared, sql) + sqlLength, 0);
  shared = (ToplevelWorkerShared *)dsm_segment_address(segment);
  shared->databaseId = MyDatabaseId;
  shared->userId = GetUserId();
  shared->completed = false;
  shared->errorMessage[0] = '\0';
  memcpy(shared->sql, sql, sqlLength);

  memset(&worker, 0, sizeof(worker));
  worker.bgw_flags =
      BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
  worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
  worker.bgw_restart_time = BGW_NEVER_RESTART;
  strlcpy(worker.bgw_library_name, "toplevel_worker", BGW_MAXLEN);
  strlcpy(worker.bgw_function_name, "toplevelWorkerMain", BGW_MAXLEN);
  strlcpy(worker.bgw_name, "toplevel_worker", BGW_MAXLEN);
  strlcpy(worker.bgw_type, "toplevel_worker", BGW_MAXLEN);
  worker.bgw_main_arg = UInt32GetDatum(dsm_segment_handle(segment));
  worker.bgw_notify_pid = MyProcPid;

  if (!RegisterDynamicBackgroundWorker(&worker, &handle))
    ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
             errmsg("toplevel_worker: could not register the background worker"),
             errhint("Raise max_worker_processes.")));

  status = WaitForBackgroundWorkerStartup(handle, &pid);
  if (status != BGWH_STARTED && status != BGWH_STOPPED)
    ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
             errmsg("toplevel_worker: the background worker did not start")));
  (void)WaitForBackgroundWorkerShutdown(handle);

  if (!shared->completed)
    result =
        cstring_to_text(shared->errorMessage[0] != '\0'
                            ? shared->errorMessage
                            : "toplevel_worker: the worker exited without completing");
  dsm_detach(segment);

  if (result == NULL)
    PG_RETURN_NULL();
  PG_RETURN_TEXT_P(result);
}
