#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"

#include "executor/executor.h"
#include "executor/spi.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;

/* Constants */
#define CONTEXT_NAME_MAX_LEN 64
#define SNAPSHOT_MAX_NODES   512

typedef struct ContextNode {
    char   name[CONTEXT_NAME_MAX_LEN];
    char   parent_name[CONTEXT_NAME_MAX_LEN];
    int    level;
    uint64 used_bytes;
} ContextNode;

typedef struct MemorySnapshot {
    ContextNode nodes[SNAPSHOT_MAX_NODES];
    int         node_count;
} MemorySnapshot;

static bool backend_profiling_active = false;
/* Snapshots for capturing the state of memory before and after a client request is executed */
static MemorySnapshot backend_snapshot_before;
static MemorySnapshot backend_snapshot_after;

/* Saved hook values in case of unload */
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorEnd_hook_type   prev_ExecutorEnd   = NULL;

/* Forward declarations */
void _PG_init(void);
void _PG_fini(void);
static void analyzer_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void analyzer_ExecutorEnd(QueryDesc *queryDesc);
static void reset_snapshot(MemorySnapshot *snapshot);

/* SQL binds */
Datum analyze_query(PG_FUNCTION_ARGS);

static void
analyzer_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
    Assert(queryDesc != NULL);

    if (backend_profiling_active)
    {
        elog(LOG, "Profiling is active (start)");

        /* Do something */
    }

    if (prev_ExecutorStart)
        prev_ExecutorStart(queryDesc, eflags);
    else
        standard_ExecutorStart(queryDesc, eflags);
}

static void
analyzer_ExecutorEnd(QueryDesc *queryDesc)
{
    Assert(queryDesc != NULL);

    if (backend_profiling_active)
    {
        elog(LOG, "Profiling is active (end)");

        /* Do something */
    }

    if (prev_ExecutorEnd)
        prev_ExecutorEnd(queryDesc);
    else
        standard_ExecutorEnd(queryDesc);
}

static void
reset_snapshot(MemorySnapshot *snapshot)
{
    Assert(snapshot->node_count <= SNAPSHOT_MAX_NODES);

    for (size_t i = 0; i < snapshot->node_count; ++i) {
        ContextNode *node = &snapshot->nodes[i];

        memset(node->name, 0, CONTEXT_NAME_MAX_LEN);
        memset(node->parent_name, 0, CONTEXT_NAME_MAX_LEN);
        node->level = 0;
        node->used_bytes = 0;
    }

    snapshot->node_count = 0;
}

PG_FUNCTION_INFO_V1(analyze_query);

Datum
analyze_query(PG_FUNCTION_ARGS)
{
    char *query = text_to_cstring(PG_GETARG_TEXT_PP(0)); /* Extract target query */
    
    InitMaterializedSRF(fcinfo, 0);

    PG_TRY();
    {
        backend_profiling_active = true; /* Enable profiling flag */
        /* Reset memory snapshots before a query execution */
        reset_snapshot(&backend_snapshot_before);
        reset_snapshot(&backend_snapshot_after);

        SPI_connect();
        
        if (SPI_execute(query, false, 0) < 0)
        {
            /* Query execution failure */
            ereport(
                ERROR,
                errcode(ERRCODE_SYNTAX_ERROR),
                errmsg("Failed to execute query: %s", query)
            );
        }
        
        SPI_finish();

        backend_profiling_active = false;
    }
    PG_CATCH();
    {
        SPI_finish();
        backend_profiling_active = false;
        
        PG_RE_THROW();
    }
    PG_END_TRY();

    PG_RETURN_NULL();
}

void
_PG_init(void)
{
    /* Install new and save previous executor hooks */
    prev_ExecutorStart = ExecutorStart_hook;
    ExecutorStart_hook = analyzer_ExecutorStart;

    prev_ExecutorEnd = ExecutorEnd_hook;
    ExecutorEnd_hook = analyzer_ExecutorEnd;
}

void
_PG_fini(void)
{
    /* Restore saved hooks on unload */
    ExecutorStart_hook = prev_ExecutorStart;
    ExecutorEnd_hook   = prev_ExecutorEnd;
}