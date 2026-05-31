#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"

#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/tuplestore.h"

PG_MODULE_MAGIC;

/* Constants */
#define CONTEXT_NAME_MAX_LEN     64
#define SNAPSHOT_MAX_NODES       512
#define TOP_CONTEXT_PARENT_LABEL "-"


/* Representation of a single MemoryContext node */
typedef struct ContextNode {
    char   name[CONTEXT_NAME_MAX_LEN];
    char   parent_name[CONTEXT_NAME_MAX_LEN];
    int    level;
    uint64 used_bytes;
} ContextNode;

/* Fixed-size snapshot of a MemoryContext tree */
typedef struct MemorySnapshot {
    ContextNode nodes[SNAPSHOT_MAX_NODES];
    int         node_count;
} MemorySnapshot;


/* Execute profiled query inside rollbackable subtransaction */
static bool backend_rollback_mode = true; 

static bool backend_profiling_active = false;
/* Snapshots for capturing the state of memory before and after a client query is executed */
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
static void traverse_memory_contexts(MemoryContext context, const char *parent_name, int level, MemorySnapshot* snapshot);
static void compute_contexts_diff(ReturnSetInfo *rsinfo);


/* SQL binds */
Datum analyze_query(PG_FUNCTION_ARGS);


/*
 * analyzer_ExecutorStart
 *
 * ExecutorStart hook used to capture the initial memory state
 * before query execution begins. When profiling is active,
 * a snapshot of the current MemoryContext tree is captured.
 */
static void
analyzer_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
    Assert(queryDesc != NULL);

    if (backend_profiling_active)
    {
        /* Capture a snapshot before the query execution */
        traverse_memory_contexts(TopMemoryContext, TOP_CONTEXT_PARENT_LABEL, 0, &backend_snapshot_before);
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
static void
analyzer_ExecutorEnd(QueryDesc *queryDesc)
{
    Assert(queryDesc != NULL);

    if (backend_profiling_active)
    {
        traverse_memory_contexts(TopMemoryContext, TOP_CONTEXT_PARENT_LABEL, 0, &backend_snapshot_after);
    }

    if (prev_ExecutorEnd)
        prev_ExecutorEnd(queryDesc);
    else
        standard_ExecutorEnd(queryDesc);
}

/*
 * reset_snapshot
 *
 * Resets a memory snapshot by clearing all of its fields.
 *
 * Parameters:
 * snapshot - pointer to the snapshot structure to reset.
 */
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
    /* Create an isolated transactional scope for query execution */
    BeginInternalSubTransaction(NULL); 

    PG_TRY();
    {
        /* Enable profiling flag */
        backend_profiling_active = true;
        /* Reset memory snapshots before a query execution */
        reset_snapshot(&backend_snapshot_before);
        reset_snapshot(&backend_snapshot_after);

        SPI_connect();
        
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

        /* Compute memory differences and materialize results */
        compute_contexts_diff(rsinfo);
        /* Disable profiling */
        backend_profiling_active = false;

        /*
         * Finalize subtransaction:
         * Rollback changes if rollback mode is enabled,
         * otherwise commit the subtransaction.
         */
        if (backend_rollback_mode)
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

    PG_RETURN_NULL();
}

/*
 * traverse_memory_contexts
 *
 * Recursively traverses the MemoryContext tree serializes
 * context data into a snapshot representation for subsequent
 * comparison.
 *
 * Parameters:
 * context   - current MemoryContext being traversed.
 * snapshot  - target snapshot receiving collected nodes.
 * level     - current depth level in the tree.
 * parent    - name of the parent context.
 */
static void
traverse_memory_contexts(MemoryContext context,
                         const char *parent_name,
                         int level,
                         MemorySnapshot *snapshot)
{
    MemoryContext         child;
    ContextNode          *node;
    MemoryContextCounters stats;

    /* Base case */
    if (context == NULL || snapshot->node_count >= SNAPSHOT_MAX_NODES)
        return;

    MemSet(&stats, 0, sizeof(stats));
    context->methods->stats(context, NULL, NULL, &stats, false);
    
    node = &snapshot->nodes[snapshot->node_count++];

    /* Fill current node's fields */
    strlcpy(node->name, context->name, CONTEXT_NAME_MAX_LEN - 1);
    strlcpy(node->parent_name, parent_name, CONTEXT_NAME_MAX_LEN - 1);
    node->level = level;
    node->used_bytes = (uint64)(stats.totalspace - stats.freespace);

    for (child = context->firstchild; child != NULL; child = child->nextchild)
    {
        traverse_memory_contexts(child, context->name, level + 1, snapshot);
    }
}

/*
 * compute_contexts_diff
 *
 * Compares memory snapshots to identify memory deltas.
 * Contexts with increased memory usage are appended into the SQL result set.
 *
 * Parameters:
 * rsinfo - ReturnSetInfo structure used to materialize results into a tuplestore.
 */
static void
compute_contexts_diff(ReturnSetInfo *rsinfo)
{
    /* Compare post-execution snapshot against baseline */
    for (int i = 0; i < backend_snapshot_after.node_count; i++)
    {
        ContextNode *node_after = &backend_snapshot_after.nodes[i];
        uint64 used_before = 0;
        int64 delta_bytes = 0;

        /* Find matching context in the baseline snapshot */
        for (int j = 0; j < backend_snapshot_before.node_count; j++)
        {
            ContextNode *node_before = &backend_snapshot_before.nodes[j];
            if (strncmp(node_after->name, node_before->name, CONTEXT_NAME_MAX_LEN) == 0 &&
                strncmp(node_after->parent_name, node_before->parent_name, CONTEXT_NAME_MAX_LEN) == 0)
            {
                used_before = node_before->used_bytes;
                break;
            }
        }

        /* Calculate memory growth for the context */
        delta_bytes = (int64)node_after->used_bytes - (int64)used_before;

        /* Report only positive deltas */
        if (delta_bytes > 0)
        {
            Datum values[7];
            bool  nulls[7] = { false };

            /* Temporarily */
            bool leak_suspected = (node_after->level <= 1) || 
                                  (strstr(node_after->name, "Cache") != NULL);

            values[0] = CStringGetTextDatum(node_after->name);
            values[1] = CStringGetTextDatum(node_after->parent_name);
            values[2] = Int32GetDatum(node_after->level);
            values[3] = UInt64GetDatum(used_before);
            values[4] = UInt64GetDatum(node_after->used_bytes);
            values[5] = Int64GetDatum(delta_bytes);
            values[6] = BoolGetDatum(leak_suspected);

            /* Append result row to an output */
            tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
        }
    }
}

void
_PG_init(void)
{
    /*
     * Register GUC variable that controls
     * transactional behavior during query profiling
     */
    DefineCustomBoolVariable(
        "memleak_analyzer.rollback_mode",
        "Rollback analyzed query after execution",
        NULL,
        &backend_rollback_mode,
        true,
        PGC_USERSET,
        0,
        NULL, NULL, NULL
    );

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