#ifndef ANALYZER_CORE_H
#define ANALYZER_CORE_H

#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"

#include "utils/builtins.h"
#include "utils/tuplestore.h"

#define CONTEXT_NAME_MAX_LEN 64
#define SNAPSHOT_MAX_NODES   512

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

/* Forward declarations */
extern void traverse_memory_contexts(MemoryContext context, const char *parent_name, int level, MemorySnapshot* snapshot);
extern void reset_snapshot(MemorySnapshot *snapshot);
extern void compute_contexts_diff(ReturnSetInfo *rsinfo, MemorySnapshot *snapshot_before, MemorySnapshot *snapshot_after);

#endif // ANALYZER_CORE_H