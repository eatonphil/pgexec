#include "postgres.h"
#include "fmgr.h"

#include "utils/rel.h"
#include "executor/executor.h"
#include "catalog/pg_operator.h"
#include "utils/syscache.h"
#include "utils/lsyscache.h"

PG_MODULE_MAGIC;

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

  Assert(additional >= 0);

  if (buf->offset + additional < buf->len) {
    return;
  }

  newsize = (buf->offset + additional) * 2;
  new = (char*)malloc(sizeof(char) * newsize);
  Assert(new != NULL);
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
  Assert(chars >= 0); // TODO: error handling.

  // Resize to fit result.
  buffer_resize_to_fit_additional(buf, chars);

  // Actually do the printf into buf.
  va_end(arglist);
  va_start(arglist, fmt);
  chars = vsprintf(buf->mem + buf->offset, fmt, arglist);
  Assert(chars >= 0); // TODO: error handling.
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

  // Offset should stay the same. This is a fake NULL.
  Assert(buf->offset == prev_offset);

  return buf->mem;
}

static void buffer_free(PGExec_Buffer* buf) {
  free(buf->mem);
}

static ExecutorRun_hook_type prev_executor_run_hook = NULL;

static void buffer_print_expr(PGExec_Buffer*, PlannedStmt*, Node*);
static void buffer_print_list(PGExec_Buffer*, PlannedStmt*, List*, char*);

static void buffer_print_opexpr(PGExec_Buffer* buf, PlannedStmt* stmt, OpExpr* op) {
  HeapTuple opertup = SearchSysCache1(OPEROID, ObjectIdGetDatum(op->opno));

  buffer_print_expr(buf, stmt, lfirst(list_nth_cell(op->args, 0)));

  if (HeapTupleIsValid(opertup)) {
    Form_pg_operator operator = (Form_pg_operator)GETSTRUCT(opertup);
    buffer_appendf(buf, " %s ", NameStr(operator->oprname));
    ReleaseSysCache(opertup);
  } else {
    buffer_appendf(buf, "[Unknown operation: %d]", op->opno);
  }

  // TODO: Support single operand operations.
  buffer_print_expr(buf, stmt, lfirst(list_nth_cell(op->args, 1)));
}

static void buffer_print_const(PGExec_Buffer* buf, Const* cnst) {
  switch (cnst->consttype) {
  case INT4OID:
    int32 val = DatumGetInt32(cnst->constvalue);
    buffer_appendf(buf, "%d", val);
    break;

  default:
    buffer_appendf(buf, "[Unknown consttype oid: %d]", cnst->consttype);
  }
}

static void buffer_print_var(PGExec_Buffer* buf, PlannedStmt* stmt, Var* var) {
  char* name = NULL;
  RangeTblEntry* rte = list_nth(stmt->rtable, var->varno-1);
  if (rte->rtekind != RTE_RELATION) {
    elog(LOG, "[Unsupported relation type for var: %d].", rte->rtekind);
    return;
  }

  name = get_attname(rte->relid, var->varattno, false);
  buffer_appendz(buf, name);
  pfree(name);
}

static void buffer_print_expr(PGExec_Buffer* buf, PlannedStmt* stmt, Node* expr) {
  switch (nodeTag(expr)) {
  case T_Const:
    buffer_print_const(buf, (Const*)expr);
    break;

  case T_Var:
    buffer_print_var(buf, stmt, (Var*)expr);
    break;

  case T_TargetEntry:
    buffer_print_expr(buf, stmt, (Node*)((TargetEntry*)expr)->expr);
    break;

  case T_OpExpr:
    buffer_print_opexpr(buf, stmt, (OpExpr*)expr);
    break;

  default:
    buffer_appendf(buf, "[Unknown: %d]", nodeTag(expr));
  }
}

static void buffer_print_list(PGExec_Buffer* buf, PlannedStmt* stmt, List* list, char* sep) {
  ListCell* cell = NULL;
  bool first = true;

  foreach(cell, list) {
    if (!first) {
      buffer_appendz(buf, sep);
    }

    first = false;
    buffer_print_expr(buf, stmt, (Node*)lfirst(cell));
  }
}

static void buffer_print_where(PGExec_Buffer* buf, QueryDesc* queryDesc, Plan* plan) {
  if (plan->qual == NULL) {
    return;
  }

  buffer_appendz(buf, " WHERE ");
  buffer_print_list(buf, queryDesc->plannedstmt, plan->qual, " AND ");
}

static void buffer_print_select_columns(PGExec_Buffer* buf, QueryDesc* queryDesc, Plan* plan) {
  if (plan->targetlist == NULL) {
    return;
  }

  buffer_print_list(buf, queryDesc->plannedstmt, plan->targetlist, ", ");
}

static void print_plan(QueryDesc* queryDesc) {
  SeqScan* scan = NULL;
  RangeTblEntry* rte = NULL;
  Relation relation = {};
  char* tablename = NULL;
  Plan* plan = queryDesc->plannedstmt->planTree;
  PGExec_Buffer buf = {};
  
  if (plan->type != T_SeqScan) {
    elog(LOG, "[pgexec] Unsupported plan type.");
    return;
  }

  scan = (SeqScan*)plan;
  rte = list_nth(queryDesc->plannedstmt->rtable, scan->scan.scanrelid-1);
  if (rte->rtekind != RTE_RELATION) {
    elog(LOG, "[pgexec] Unsupported FROM type: %d.", rte->rtekind);
    return;
  }

  buffer_init(&buf);

  relation = RelationIdGetRelation(rte->relid);
  tablename = NameStr(relation->rd_rel->relname);

  buffer_appendz(&buf, "SELECT ");
  buffer_print_select_columns(&buf, queryDesc, plan);
  buffer_appendf(&buf, " FROM %s", tablename);
  buffer_print_where(&buf, queryDesc, plan);

  elog(LOG, "[pgexec] %s", buffer_cstring(&buf));

  RelationClose(relation);
  buffer_free(&buf);
}

static void pgexec_run_hook(
  QueryDesc* queryDesc,
  ScanDirection direction,
  uint64 count,
  bool execute_once
) {
  print_plan(queryDesc);
  return prev_executor_run_hook(queryDesc, direction, count, execute_once);
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
