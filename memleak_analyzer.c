#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"

#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/tuplestore.h"

PG_MODULE_MAGIC;

/* Constants */
#define CONTEXT_NAME_MAX_LEN     64
#define SNAPSHOT_MAX_NODES       512
#define TOP_CONTEXT_PARENT_LABEL "-"


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
static void traverse_memory_contexts(MemoryContext context, const char *parent_name, int level, MemorySnapshot* snapshot);
static void compute_contexts_diff(ReturnSetInfo *rsinfo);


/* SQL binds */
Datum analyze_query(PG_FUNCTION_ARGS);


static void
analyzer_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
    Assert(queryDesc != NULL);

    if (backend_profiling_active)
    {
        traverse_memory_contexts(TopMemoryContext, TOP_CONTEXT_PARENT_LABEL, 0, &backend_snapshot_before);
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
        traverse_memory_contexts(TopMemoryContext, TOP_CONTEXT_PARENT_LABEL, 0, &backend_snapshot_after);
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
    char          *query  = text_to_cstring(PG_GETARG_TEXT_PP(0)); /* Extract target query */
    ReturnSetInfo *rsinfo = (ReturnSetInfo *)fcinfo->resultinfo;

    InitMaterializedSRF(fcinfo, 0);
    BeginInternalSubTransaction(NULL);

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
        RollbackAndReleaseCurrentSubTransaction();
    }
    PG_CATCH();
    {
        SPI_finish();
        backend_profiling_active = false;
        RollbackAndReleaseCurrentSubTransaction();
        PG_RE_THROW();
    }
    PG_END_TRY();

    compute_contexts_diff(rsinfo);

    PG_RETURN_NULL();
}

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

static void
compute_contexts_diff(ReturnSetInfo *rsinfo)
{
    for (int i = 0; i < backend_snapshot_after.node_count; i++)
    {
        ContextNode *node_after = &backend_snapshot_after.nodes[i];
        uint64 used_before = 0;
        int64 delta_bytes = 0;

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

        delta_bytes = (int64)node_after->used_bytes - (int64)used_before;

        if (delta_bytes > 0)
        {
            Datum values[7];
            bool  nulls[7] = { false };

            bool leak_suspected = (node_after->level <= 1) || 
                                  (strstr(node_after->name, "Cache") != NULL);

            values[0] = CStringGetTextDatum(node_after->name);
            values[1] = CStringGetTextDatum(node_after->parent_name);
            values[2] = Int32GetDatum(node_after->level);
            values[3] = UInt64GetDatum(used_before);
            values[4] = UInt64GetDatum(node_after->used_bytes);
            values[5] = Int64GetDatum(delta_bytes);
            values[6] = BoolGetDatum(leak_suspected);

            tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
        }
    }
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