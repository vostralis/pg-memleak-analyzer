#include "analyzer_backend.h"

static bool backend_profiling_active = false;
/* Snapshots for capturing the state of memory before and after a client query is executed */
static MemorySnapshot backend_snapshot_before = { .node_count = 0 };
static MemorySnapshot backend_snapshot_after = { .node_count = 0 };

bool analyzer_rollback_mode = true;
bool analyzer_enable_warmup = true;

ExecutorStart_hook_type prev_ExecutorStart = NULL;
ExecutorEnd_hook_type prev_ExecutorEnd = NULL;

static void execute_query_times(const char *query, int count);

PG_FUNCTION_INFO_V1(analyze_query);

/*
 * analyze_query
 *
 * Entry point for SQL-level memory profiling. Executes the
 * target query while profiling hooks are enabled, captures
 * memory snapshots before and after execution, and returns
 * the memory differences as a set-returning table.
 *
 * Returns:
 * A materialized result set containing memory statistics.
 */
Datum
analyze_query(PG_FUNCTION_ARGS)
{
    char          *query  = text_to_cstring(PG_GETARG_TEXT_PP(0)); /* Extract target query */
    ReturnSetInfo *rsinfo = (ReturnSetInfo *)fcinfo->resultinfo;

    /* Initialize materialized SRF infrastructure */
    InitMaterializedSRF(fcinfo, 0);

    /* Reset memory snapshots before a query execution */
    reset_snapshot(&backend_snapshot_before);
    reset_snapshot(&backend_snapshot_after); 

    /* Create an isolated transactional scope for query execution */
    BeginInternalSubTransaction(NULL); 

    PG_TRY();
    {
        SPI_connect();
        
        /* Run target query WARMUP_RUNS times to populate internal caches */
        if (analyzer_enable_warmup)
            execute_query_times(query, WARMUP_RUNS);

        /* Enable profiling flag */
        backend_profiling_active = true;

        /* Execute target query */
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

        /* Disable profiling */
        backend_profiling_active = false;

        /*
         * Finalize subtransaction:
         * Rollback changes if rollback mode is enabled,
         * otherwise commit the subtransaction.
         */
        if (analyzer_rollback_mode)
            RollbackAndReleaseCurrentSubTransaction();
        else
            ReleaseCurrentSubTransaction();
    }
    PG_CATCH();
    {
        /* Ensure SPI state is cleaned up on failure */
        SPI_finish();
        /* Disable profiling */
        backend_profiling_active = false;
        /* Rollback subtransaction on failure */
        RollbackAndReleaseCurrentSubTransaction();

        PG_RE_THROW();
    }
    PG_END_TRY();

    /* Compute memory differences and materialize results */
    compute_contexts_diff(rsinfo, &backend_snapshot_before, &backend_snapshot_after);

    PG_RETURN_NULL();
}

/*
 * analyzer_ExecutorStart
 *
 * ExecutorStart hook used to capture the initial memory state
 * before query execution begins. When profiling is active,
 * a snapshot of the current MemoryContext tree is captured.
 */
extern void
analyzer_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
    Assert(queryDesc != NULL);

    if (backend_profiling_active)
    {
        /* Capture a snapshot before the query execution */
        traverse_memory_contexts(
            TopMemoryContext, TOP_CONTEXT_PARENT_LABEL, 0,
            analyzer_max_context_level, analyzer_merge_contexts, &backend_snapshot_before
        );
    }

    if (prev_ExecutorStart)
        prev_ExecutorStart(queryDesc, eflags);
    else
        standard_ExecutorStart(queryDesc, eflags);
}

/*
 * analyzer_ExecutorEnd
 *
 * ExecutorEnd hook used to capture the memory state after query
 * execution is finished. This snapshot is later compared
 * against the initial snapshot to identify memory growth patterns.
 */
extern void
analyzer_ExecutorEnd(QueryDesc *queryDesc)
{
    Assert(queryDesc != NULL);

    if (backend_profiling_active)
    {
        traverse_memory_contexts(
            TopMemoryContext, TOP_CONTEXT_PARENT_LABEL, 0, 
            analyzer_max_context_level, analyzer_merge_contexts, &backend_snapshot_after
        );
    }

    if (prev_ExecutorEnd)
        prev_ExecutorEnd(queryDesc);
    else
        standard_ExecutorEnd(queryDesc);
}

static void
execute_query_times(const char *query, int count)
{
    for (int i = 0; i < count; ++i)
    {
        BeginInternalSubTransaction(NULL);
        PG_TRY();
        {
            if (SPI_execute(query, false, 0) < 0)
            {
                ereport(
                    ERROR,
                    errcode(ERRCODE_SYNTAX_ERROR),
                    errmsg("Failed to execute query: %s", query)
                );
            }
        }
        PG_CATCH();
        {
            RollbackAndReleaseCurrentSubTransaction();
            PG_RE_THROW();
        }
        PG_END_TRY();
        
        RollbackAndReleaseCurrentSubTransaction();
    }
}