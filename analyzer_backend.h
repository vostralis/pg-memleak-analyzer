#ifndef ANALYZER_BACKEND_H
#define ANALYZER_BACKEND_H

#include "analyzer_core.h"
#include "executor/spi.h"

#define WARMUP_RUNS 2

/* Controls whether executed queries are wrapped in an internal subtransaction and rolled back after execution */
extern bool analyzer_rollback_mode;

/* Enables warmup phase before actual profiling execution for population of internal caches */
extern bool analyzer_enable_warmup;

/* Previous Executor memory hooks */
extern ExecutorStart_hook_type prev_ExecutorStart;
extern ExecutorEnd_hook_type   prev_ExecutorEnd;

extern void analyzer_ExecutorStart(QueryDesc *queryDesc, int eflags);
extern void analyzer_ExecutorEnd(QueryDesc *queryDesc);

Datum analyze_query(PG_FUNCTION_ARGS);

#endif // ANALYZER_BACKEND_H