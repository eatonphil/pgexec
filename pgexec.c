#include "postgres.h"
#include "fmgr.h"

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

  if (queryDesc->operation != CMD_SELECT) {
    return;
  }
  
  dest->rStartup(dest, queryDesc->operation, queryDesc->tupDesc);
    
  slot = MakeTupleTableSlot(queryDesc->tupDesc, &TTSOpsVirtual);
  slot->tts_nvalid = 1;
  slot->tts_values = (Datum*)malloc(sizeof(Datum) * slot->tts_nvalid);
  slot->tts_isnull = (bool*)malloc(sizeof(bool) * slot->tts_nvalid);
  for (i = 0; i < 10; i++) {
    ExecClearTuple(slot);
    slot->tts_values[0] = Int32GetDatum(598);
    slot->tts_isnull[0] = false;
    ExecStoreVirtualTuple(slot);
    Assert(dest->receiveSlot(slot, dest) == (i != 10));
  }

  ExecDropSingleTupleTableSlot(slot);
 
  dest->rShutdown(dest);
}

void _PG_init(void) {
  prev_executor_run_hook = ExecutorRun_hook;
  ExecutorRun_hook = pgexec_run_hook;
}

void _PG_fini(void) {
  ExecutorRun_hook = prev_executor_run_hook;
}
