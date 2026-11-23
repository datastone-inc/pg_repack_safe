/*
 * Copyright (c) 2026 dataStone Inc.
 * SPDX-License-Identifier: MIT
 */

/**
 * @file custom_scan_provider.c
 * @brief Test-only module: a custom scan provider, for the reader check
 *
 * While custom_scan_provider.enabled is on, every scan of a plain table is
 * planned as a CustomScan that returns the table's rows as a sequential
 * scan does. specs/unread_relations.spec loads it to put a CustomScan in a
 * plan and checks that pg_repack_safe's reader check then covers the
 * range-table relations the executor did not open. Not part of the
 * extension; installed only by make installcheck.
 */

#include "postgres.h"

#include "access/relscan.h"
#include "access/tableam.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "nodes/extensible.h"
#include "nodes/pathnodes.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"
#include "utils/guc.h"
#include "utils/rel.h"

PG_MODULE_MAGIC;

void _PG_init(void);

#define PROVIDER_NAME "custom_scan_provider"

/* Execution state: the node and its sequential scan, begun on first use. */
typedef struct ProviderScanState
{
  CustomScanState css;
  TableScanDesc scan;
} ProviderScanState;

static set_rel_pathlist_hook_type prevSetRelPathlistHook = NULL;

/* GUC: plan every scan of a plain table as this module's CustomScan. */
static bool providerEnabled = false;

static Plan *planProviderPath(PlannerInfo *root, RelOptInfo *rel,
                              CustomPath *bestPath, List *tlist, List *clauses,
                              List *customPlans);
static Node *createProviderScanState(CustomScan *cscan);
static void beginProviderScan(CustomScanState *node, EState *estate,
                              int eflags);
static TupleTableSlot *execProviderScan(CustomScanState *node);
static void endProviderScan(CustomScanState *node);
static void rescanProviderScan(CustomScanState *node);

static const CustomPathMethods providerPathMethods = {
    .CustomName = PROVIDER_NAME,
    .PlanCustomPath = planProviderPath,
};

static const CustomScanMethods providerScanMethods = {
    .CustomName = PROVIDER_NAME,
    .CreateCustomScanState = createProviderScanState,
};

static const CustomExecMethods providerExecMethods = {
    .CustomName = PROVIDER_NAME,
    .BeginCustomScan = beginProviderScan,
    .ExecCustomScan = execProviderScan,
    .EndCustomScan = endProviderScan,
    .ReScanCustomScan = rescanProviderScan,
};

/**
 * @brief set_rel_pathlist_hook: offer a CustomPath for a plain table
 * @param root Planner state
 * @param rel Relation being planned
 * @param rti Range-table index of the relation
 * @param rte Range-table entry of the relation
 *
 * The path costs nothing, so the planner picks it over the paths it built
 * itself. A relation with lateral references is left alone: its scan would
 * need parameters this provider does not handle.
 */
static void
providerSetRelPathlist(PlannerInfo *root, RelOptInfo *rel, Index rti,
                       RangeTblEntry *rte)
{
  CustomPath *path;

  if (prevSetRelPathlistHook != NULL)
    prevSetRelPathlistHook(root, rel, rti, rte);

  if (!providerEnabled || rte->rtekind != RTE_RELATION ||
      rte->relkind != RELKIND_RELATION || rte->inh ||
      rel->reloptkind != RELOPT_BASEREL || !bms_is_empty(rel->lateral_relids))
    return;

  path = makeNode(CustomPath);
  path->path.pathtype = T_CustomScan;
  path->path.parent = rel;
  path->path.pathtarget = rel->reltarget;
  path->path.rows = rel->rows;
  path->methods = &providerPathMethods;
  add_path(rel, &path->path);
}

/**
 * @brief PlanCustomPath: build the CustomScan for a path of this provider
 * @param root Planner state
 * @param rel Relation the path scans
 * @param bestPath The chosen path
 * @param tlist Target list the plan node must produce
 * @param clauses Restriction clauses the plan node must apply
 * @param customPlans Child plans; none for this provider
 * @return The CustomScan plan node
 */
static Plan *
planProviderPath(PlannerInfo *root, RelOptInfo *rel, CustomPath *bestPath,
                 List *tlist, List *clauses, List *customPlans)
{
  CustomScan *cscan = makeNode(CustomScan);

  cscan->scan.plan.targetlist = tlist;
  cscan->scan.plan.qual = extract_actual_clauses(clauses, false);
  cscan->scan.scanrelid = rel->relid;
  cscan->methods = &providerScanMethods;

  return &cscan->scan.plan;
}

/**
 * @brief CreateCustomScanState: allocate the execution state
 * @param cscan Plan node
 * @return The new state, with its tag, methods and slot type set
 *
 * The scan slot holds heap tuples; the provider is offered for plain
 * tables only, and the tests' tables use the heap access method.
 */
static Node *
createProviderScanState(CustomScan *cscan)
{
  ProviderScanState *state = palloc0_object(ProviderScanState);

  NodeSetTag(state, T_CustomScanState);
  state->css.methods = &providerExecMethods;
  state->css.slotOps = &TTSOpsBufferHeapTuple;

  return (Node *)state;
}

/**
 * @brief BeginCustomScan: nothing to set up
 * @param node Execution state
 * @param estate Executor state
 * @param eflags Executor flags
 *
 * ExecInitCustomScan has opened the scan relation; the table scan begins
 * with the first tuple request, so EXPLAIN without ANALYZE starts none.
 */
static void
beginProviderScan(CustomScanState *node, EState *estate, int eflags)
{
}

/**
 * @brief Scan access method: fetch the next tuple of the table
 * @param node Scan state
 * @return The scan slot holding the tuple, or NULL at the end of the table
 */
static TupleTableSlot *
nextProviderTuple(ScanState *node)
{
  ProviderScanState *state = (ProviderScanState *)node;
  TupleTableSlot *slot = node->ss_ScanTupleSlot;

  if (state->scan == NULL)
    state->scan = table_beginscan(node->ss_currentRelation,
                                  node->ps.state->es_snapshot, 0, NULL, 0);

  if (table_scan_getnextslot(state->scan, ForwardScanDirection, slot))
    return slot;
  return NULL;
}

/**
 * @brief Scan recheck method: a sequential scan has no quals to recheck
 * @param node Scan state
 * @param slot Tuple to recheck
 * @return True
 */
static bool
recheckProviderTuple(ScanState *node, TupleTableSlot *slot)
{
  return true;
}

/**
 * @brief ExecCustomScan: return the next tuple that passes the quals
 * @param node Execution state
 * @return The projected tuple, or an empty slot at the end of the scan
 */
static TupleTableSlot *
execProviderScan(CustomScanState *node)
{
  return ExecScan(&node->ss, nextProviderTuple, recheckProviderTuple);
}

/**
 * @brief EndCustomScan: end the table scan
 * @param node Execution state
 */
static void
endProviderScan(CustomScanState *node)
{
  ProviderScanState *state = (ProviderScanState *)node;

  if (state->scan != NULL)
    table_endscan(state->scan);
}

/**
 * @brief ReScanCustomScan: restart the table scan
 * @param node Execution state
 */
static void
rescanProviderScan(CustomScanState *node)
{
  ProviderScanState *state = (ProviderScanState *)node;

  if (state->scan != NULL)
    table_rescan(state->scan, NULL);
}

/**
 * @brief Module load: define the GUC and install the planner hook
 */
void
_PG_init(void)
{
  DefineCustomBoolVariable("custom_scan_provider.enabled",
                           "Plans every scan of a plain table as this module's CustomScan.",
                           NULL, &providerEnabled, false, PGC_USERSET, 0, NULL,
                           NULL, NULL);
  MarkGUCPrefixReserved("custom_scan_provider");

  RegisterCustomScanMethods(&providerScanMethods);

  prevSetRelPathlistHook = set_rel_pathlist_hook;
  set_rel_pathlist_hook = providerSetRelPathlist;
}
