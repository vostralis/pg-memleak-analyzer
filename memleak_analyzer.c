#include "postgres.h"
#include "fmgr.h"

#include "executor/executor.h"

PG_MODULE_MAGIC;

typedef struct ContextNode {
    char   name[64];
    int    level;
    uint64 total_bytes;
    uint64 free_bytes;
} ContextNode;

#define SNAPSHOT_MAX_NODES 256

typedef struct MemorySnapshot {
    ContextNode nodes[SNAPSHOT_MAX_NODES];
    int         node_count;
} MemorySnapshot;

/* Snapshots for capturing the state of memory before and after a client request is executed */
static MemorySnapshot snapshot_before;
static MemorySnapshot snapshot_after;

/* Saved hook values in case of unload */
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorEnd_hook_type   prev_ExecutorEnd   = NULL;

/* Forward declarations */
void _PG_init(void);
void _PG_fini(void);
static void analyzer_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void analyzer_ExecutorEnd(QueryDesc *queryDesc);
static void reset_snapshot(MemorySnapshot *snapshot);

static void
analyzer_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
    Assert(queryDesc != NULL);

    if (prev_ExecutorStart)
        prev_ExecutorStart(queryDesc, eflags);
    else
        standard_ExecutorStart(queryDesc, eflags);
}

static void
analyzer_ExecutorEnd(QueryDesc *queryDesc)
{
    Assert(queryDesc != NULL);

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

        memset(node->name, 0, sizeof(node->name));
        node->level = 0;
        node->total_bytes = 0;
        node->free_bytes = 0;
    }

    snapshot->node_count = 0;
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