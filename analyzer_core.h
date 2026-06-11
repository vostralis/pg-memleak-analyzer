#ifndef ANALYZER_CORE_H
#define ANALYZER_CORE_H

#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"

#include "utils/builtins.h"
#include "utils/tuplestore.h"

#define CONTEXT_NAME_MAX_LEN     64
#define SNAPSHOT_MAX_NODES       512
#define TOP_CONTEXT_PARENT_LABEL "-"
#define TOP_CONTEXT_LEVEL        0

/* Representation of a single MemoryContext node */
typedef struct ContextNode
{
    char      name[CONTEXT_NAME_MAX_LEN];        /* Memory context name */
    char      parent_name[CONTEXT_NAME_MAX_LEN]; /* Parent context name */
    int       level;                             /* Context depth in the tree */
    uint64    used_bytes;                        /* Memory currently used */
    uintptr_t address;                           /* Memory context address used for identity tracking */
} ContextNode;

/* Fixed-size snapshot of a MemoryContext tree */
typedef struct MemorySnapshot
{
    ContextNode nodes[SNAPSHOT_MAX_NODES]; /* Collected context nodes */
    int         node_count;                /* Number of collected nodes */
    bool        limit_warning_emitted;     /* Flag to prevent duplicate truncation warning */
} MemorySnapshot;

/* Maximum memory context tree depth included in snapshots */
extern int  analyzer_max_context_level;
/* Merge contexts with identical name and parent into a single node */
extern bool analyzer_merge_contexts;
/* Show only positive memory deltas in the output */
extern bool analyzer_show_positive_deltas;

extern void traverse_memory_contexts(MemoryContext context, 
                                     const char *parent_name, int level, 
                                     int max_level, bool merge_contexts,
                                     MemorySnapshot* snapshot);
extern void reset_snapshot(MemorySnapshot *snapshot);
extern void compute_contexts_diff(ReturnSetInfo *rsinfo, 
                                  MemorySnapshot *snapshot_before,
                                  MemorySnapshot *snapshot_after);

#endif // ANALYZER_CORE_H