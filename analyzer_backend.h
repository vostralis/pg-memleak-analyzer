#ifndef ANALYZER_BACKEND_H
#define ANALYZER_BACKEND_H

#include "analyzer_core.h"
#include "executor/spi.h"

#define TOP_CONTEXT_PARENT_LABEL "-"
#define WARMUP_RUNS               2

/* Execute profiled query inside rollbackable subtransaction */
extern bool backend_rollback_mode; 

/* Saved hook values in case of unload */
extern ExecutorStart_hook_type prev_ExecutorStart;
extern ExecutorEnd_hook_type   prev_ExecutorEnd;

/* Forward declarations */
extern void analyzer_ExecutorStart(QueryDesc *queryDesc, int eflags);
extern void analyzer_ExecutorEnd(QueryDesc *queryDesc);

/* SQL bind */
Datum analyze_query(PG_FUNCTION_ARGS);

#endif // ANALYZER_BACKEND_H