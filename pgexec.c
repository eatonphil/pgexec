#include "postgres.h"
#include "fmgr.h"

#include "utils/rel.h"
#include "nodes/nodes.h"
#include "executor/executor.h"

PG_MODULE_MAGIC;

static ExecutorRun_hook_type prev_executor_run_hook = NULL;

static void pgexec_run_hook(
  QueryDesc* queryDesc,
  ScanDirection direction,
  uint64 count,
  bool execute_once
) {
  DestReceiver* dest = queryDesc->dest;
  TupleTableSlot* slot = NULL;
  size_t i = 0;

  bool fallback = true;
  Relation relation = NULL;
  char* table = NULL;

  if (queryDesc->operation == CMD_SELECT) {
    if (nodeTag(queryDesc->planstate) == T_SeqScanState) {
      relation = ((SeqScanState*)queryDesc->planstate)->ss.ss_currentRelation;
      table = NameStr(relation->rd_rel->relname);
      if (strcmp(table, "x") == 0) {
	fallback = false;
      }
    } else {
      //elog(LOG, "UNKNOWN PLAN TYPE: %d\n", nodeTag(queryDesc->planstate));
    }
  }

  if (fallback) {
    //elog(LOG, "FALLING BACK!\n");
    Assert(prev_executor_run_hook != NULL);
    return prev_executor_run_hook(queryDesc, direction, count, execute_once);
  } else {
    //elog(LOG, "NOT FALLING BACK, TABLE IS: %s", table);
  }
  
  dest->rStartup(dest, queryDesc->operation, queryDesc->tupDesc);

  slot = MakeTupleTableSlot(queryDesc->tupDesc, &TTSOpsVirtual);
  slot->tts_nvalid = 1;
  slot->tts_values = (Datum*)malloc(sizeof(Datum) * slot->tts_nvalid);
  slot->tts_isnull = (bool*)malloc(sizeof(bool) * slot->tts_nvalid);
  for (i = 0; i < 2; i++) {
    ExecClearTuple(slot);
    slot->tts_values[0] = Int32GetDatum(i);
    slot->tts_isnull[0] = false;
    ExecStoreVirtualTuple(slot);

    if (nodeTag(queryDesc->planstate->plan) == T_SeqScan) {
      ExprContext* econtext = GetPerTupleExprContext(queryDesc->estate);
      Assert(econtext != NULL);
      econtext->ecxt_scantuple = slot;
      if (!ExecQual(queryDesc->planstate->qual, econtext)) {
	continue;
      }
    }
    
    Assert(dest->receiveSlot(slot, dest) == (i != 2));
  }

  ExecDropSingleTupleTableSlot(slot);
 
  dest->rShutdown(dest);
}

void _PG_init(void) {
  prev_executor_run_hook = ExecutorRun_hook;
  if (prev_executor_run_hook == NULL) {
    prev_executor_run_hook = standard_ExecutorRun;
  }
  ExecutorRun_hook = pgexec_run_hook;
}

void _PG_fini(void) {
  ExecutorRun_hook = prev_executor_run_hook;
}
