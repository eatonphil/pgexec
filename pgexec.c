#include "postgres.h"
#include "fmgr.h"

#include "catalog/pg_operator.h"
#include "executor/executor.h"
#include "nodes/nodes.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

PG_MODULE_MAGIC;

static ExecutorRun_hook_type prev_executor_run_hook = NULL;

typedef struct {
  char* mem;
  size_t len;
  size_t offset;
} PGExec_Buffer;

static void buffer_init(PGExec_Buffer* buf) {
  buf->offset = 0;
  buf->len = 8;
  buf->mem = (char*)malloc(sizeof(char) * buf->len);
}

static void buffer_resize_to_fit_additional(PGExec_Buffer* buf, size_t additional) {
  char* new = {};
  size_t newsize = 0;

  if (buf->offset + additional < buf->len) {
    return;
  }

  newsize = (buf->offset + additional) * 2;
  new = (char*)malloc(sizeof(char) * newsize);
  memcpy(new, buf->mem, buf->len * sizeof(char));
  free(buf->mem);
  buf->len = newsize;
  buf->mem = new;
}

static void buffer_append(PGExec_Buffer*, char*, size_t);

static void buffer_appendz(PGExec_Buffer* buf, char* c) {
  buffer_append(buf, c, strlen(c));
}

static void buffer_append(PGExec_Buffer* buf, char* c, size_t chars) {
  buffer_resize_to_fit_additional(buf, chars);
  memcpy(buf->mem + buf->offset, c, chars);
  buf->offset += chars;
}

static void buffer_appendf(
  PGExec_Buffer *,
  const char* restrict,
  ...
) __attribute__ ((format (gnu_printf, 2, 3)));

static void buffer_appendf(PGExec_Buffer *buf, const char* restrict fmt, ...) {
  // First figure out how long the result will be.
  size_t chars = 0;
  va_list arglist;
  va_start(arglist, fmt);
  chars = vsnprintf(0, 0, fmt, arglist);

  // Resize to fit result.
  buffer_resize_to_fit_additional(buf, chars);

  // Actually do the printf into buf.
  va_end(arglist);
  va_start(arglist, fmt);
  vsprintf(buf->mem + buf->offset, fmt, arglist);
  buf->offset += chars;
  va_end(arglist);
}

static char* buffer_cstring(PGExec_Buffer* buf) {
  char zero = 0;
  const size_t prev_offset = buf->offset;

  if (buf->offset == buf->len) {
    buffer_append(buf, &zero, 1);
    buf->offset--;
  } else {
    buf->mem[buf->offset] = 0;
  }

  // Offset should stay the same-> This is a fake NULL->
  Assert(buf->offset == prev_offset);

  return buf->mem;
}

static void buffer_free(PGExec_Buffer* buf) {
  free(buf->mem);
}

static void buffer_print_list(PGExec_Buffer*, List*, char*, EState*);
static void buffer_print_expr(PGExec_Buffer*, Expr*, EState*);

static void buffer_print_op(PGExec_Buffer* buf, OpExpr* op, EState* estate) {
  HeapTuple opertup = SearchSysCache1(OPEROID, ObjectIdGetDatum(op->opno));

  buffer_print_expr(buf, lfirst(list_nth_cell(op->args, 0)), estate);

  if (HeapTupleIsValid(opertup)) {
    Form_pg_operator operator = (Form_pg_operator)GETSTRUCT(opertup);
    buffer_appendf(buf, " %s ", NameStr(operator->oprname));
    ReleaseSysCache(opertup);
  } else {
    buffer_appendf(buf, "[Unknown operation: %d]", op->opno);
  }

  // TODO: Support single operand operations.
  buffer_print_expr(buf, lfirst(list_nth_cell(op->args, 1)), estate);
}

static void buffer_print_var(PGExec_Buffer* buf, Var* var, EState *estate) {
  RangeTblEntry* rte = list_nth(estate->es_range_table, var->varno - 1);

  char* name;
  if (var->varattno > 0) {
    name = get_attname(rte->relid, var->varattno, false);
    buffer_appendz(buf, name);
    pfree(name);
  } else {
    buffer_appendz(buf, "[Unsupported varrtno type]");
  }
}

static void buffer_print_const(PGExec_Buffer* buf, Const* cnst, EState *estate) {
  switch (cnst->consttype) {
  case INT4OID:
    int32 val = DatumGetInt32(cnst->constvalue);
    buffer_appendf(buf, "%d", val);
    break;

  default:
    buffer_appendf(buf, "[Unknown consttype oid: %d]", cnst->consttype);
  }
}

static void buffer_print_expr(PGExec_Buffer* buf, Expr* expr, EState* estate) {
  if (expr == NULL) {
    // TODO: Should print out here?
    return;
  }

  switch (nodeTag(expr)) {
  case T_Var:
    buffer_print_var(buf, (Var*)expr, estate);
    break;

  case T_Const:
    buffer_print_const(buf, (Const*)expr, estate);
    break;

  case T_TargetEntry:
    buffer_print_expr(buf, ((TargetEntry*)expr)->expr, estate);
    break;

  case T_OpExpr:
    buffer_print_op(buf, (OpExpr*)expr, estate);
    break;

  default:
    buffer_appendf(buf, "[unclear: %d]", nodeTag(expr));
  }
}

static void buffer_print_list(
  PGExec_Buffer* buf,
  List* list,
  char* sep,
  EState* estate
) {
  ListCell *cell = NULL;
  bool first = true;
  foreach(cell, list) {
    if (!first) {
      buffer_appendz(buf, sep);
    }
    first = false;
    buffer_print_expr(buf, lfirst(cell), estate);
  }
}

static void print_selectplan(Plan* plan, EState* estate) {
  RangeTblEntry* rte = list_nth(
    estate->es_range_table,
    ((SeqScan*)plan)->scan.scanrelid - 1);
  Relation relation = RelationIdGetRelation(rte->relid);
  char* table = NameStr(relation->rd_rel->relname);

  PGExec_Buffer buf = {};
  buffer_init(&buf);

  buffer_appendz(&buf, "SELECT ");
  buffer_print_list(&buf, plan->targetlist, ", ", estate);
  buffer_appendf(&buf, " FROM %s", table);
  if (plan->qual != NULL) {
    buffer_appendz(&buf, " WHERE ");
    buffer_print_list(&buf, plan->qual, " AND ", estate);
  }

  elog(LOG, "QUERY: { %s }", buffer_cstring(&buf));

  buffer_free(&buf);
  RelationClose(relation);
}

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
  ExprContext* econtext = NULL;

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

  print_selectplan(queryDesc->planstate->plan, queryDesc->estate);

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

    econtext = GetPerTupleExprContext(queryDesc->estate);
    Assert(econtext != NULL);
    econtext->ecxt_scantuple = slot;
    if (!ExecQual(queryDesc->planstate->qual, econtext)) {
      continue;
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
