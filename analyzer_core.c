#include "analyzer_core.h"

int analyzer_max_context_level = -1;
bool analyzer_merge_contexts = false;
bool analyzer_show_positive_deltas = false;

static inline void init_context_node(ContextNode *node, 
                                     const char *name, const char *parent_name, 
                                     int level, uint64 used_bytes, uintptr_t address);

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
extern void
traverse_memory_contexts(MemoryContext context,
                         const char *parent_name,
                         int level,
                         int max_level,
                         bool merge_contexts,
                         MemorySnapshot *snapshot)
{
    MemoryContext         child;
    MemoryContextCounters stats;
    bool context_limit_reached = (snapshot->node_count >= SNAPSHOT_MAX_NODES);
    uint64 used_bytes;
    
    /* Base case */
    if (context == NULL)
        return;

    if (max_level >= 0 && level > max_level)
        return;

    /* If contexts shouldn't be merged and the context limis was reached, 
     * then we can't keep traversing the context tree
     */
    if (!merge_contexts && context_limit_reached)
    {
        /* Emit a warning once the context limit is reached */
        if (!snapshot->limit_warning_emitted)
        {
            ereport(
                WARNING,
                errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
                errmsg("SNAPSHOT_MAX_NODES (%d) reached, snapshot truncated", SNAPSHOT_MAX_NODES)
            );
            
            snapshot->limit_warning_emitted = true;
        }

        return;
    }

    MemSet(&stats, 0, sizeof(stats));
    context->methods->stats(context, NULL, NULL, &stats, false);
    used_bytes = (uint64)(stats.totalspace - stats.freespace);

    if (merge_contexts)
    {
        bool is_merged = false;

        for (int i = 0; i < snapshot->node_count; ++i)
        {
            ContextNode *existing_node = &snapshot->nodes[i];

            /* If such context exists, merge it */
            if (existing_node->level == level &&
                strncmp(existing_node->name, context->name, CONTEXT_NAME_MAX_LEN) == 0 &&
                strncmp(existing_node->parent_name, parent_name, CONTEXT_NAME_MAX_LEN) == 0)
            {
                existing_node->used_bytes += used_bytes;
                is_merged = true;
                break;
            }
        }

        /* If it doesn't and we didn't reach the context limit, push back a new one */
        if (!is_merged && !context_limit_reached)
        {
            init_context_node(
                &snapshot->nodes[snapshot->node_count++], 
                context->name, parent_name, level, used_bytes, (uintptr_t)NULL
            );
        }
    }
    else
    {   
        init_context_node(
            &snapshot->nodes[snapshot->node_count++], 
            context->name, parent_name, level, used_bytes, (uintptr_t)context
        );
    }

    for (child = context->firstchild; child != NULL; child = child->nextchild)
    {
        traverse_memory_contexts(child, context->name, level + 1, max_level, merge_contexts, snapshot);
    }
}

/*
 * reset_snapshot
 *
 * Resets a memory snapshot by clearing all of its fields.
 *
 * Parameters:
 * snapshot - pointer to the snapshot structure to reset.
 */
extern void
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
    snapshot->limit_warning_emitted = false;
}

/*
 * compute_contexts_diff
 *
 * Compares memory snapshots to identify memory deltas.
 * Contexts with increased memory usage are appended into the SQL result set.
 *
 * Parameters:
 * rsinfo          - ReturnSetInfo structure used to materialize results into a tuplestore.
 * snapshot_before - snapshot that was taken before the target query execution.
 * snapshot_after  - snapshot that was taken after the target query execution.
 */
extern void
compute_contexts_diff(ReturnSetInfo *rsinfo, 
                      MemorySnapshot *snapshot_before, 
                      MemorySnapshot *snapshot_after)
{
    /* Compare post-execution snapshot against baseline */
    for (int i = 0; i < snapshot_after->node_count; i++)
    {
        ContextNode *node_after = &snapshot_after->nodes[i];
        uint64 used_before = 0;
        int64 delta_bytes = 0;

        /* Find matching context in the baseline snapshot */
        for (int j = 0; j < snapshot_before->node_count; j++)
        {
            ContextNode *node_before = &snapshot_before->nodes[j];
            if (strncmp(node_after->name, node_before->name, CONTEXT_NAME_MAX_LEN) == 0 &&
                strncmp(node_after->parent_name, node_before->parent_name, CONTEXT_NAME_MAX_LEN) == 0 &&
                node_before->address == node_after->address)
            {
                used_before = node_before->used_bytes;
                break;
            }
        }

        /* Calculate memory growth for the context */
        delta_bytes = (int64)node_after->used_bytes - (int64)used_before;

        if (analyzer_show_positive_deltas && delta_bytes == 0)
            continue;
        
        Datum values[6];
        bool nulls[6] = { false };

        values[0] = CStringGetTextDatum(node_after->name);        /* context_name */
        values[1] = CStringGetTextDatum(node_after->parent_name); /* parent_name */
        values[2] = Int32GetDatum(node_after->level);             /* level */
        values[3] = UInt64GetDatum(used_before);                  /* allocated_before */
        values[4] = UInt64GetDatum(node_after->used_bytes);       /* allocated_after */
        values[5] = Int64GetDatum(delta_bytes);                   /* delta_bytes */

        /* Append result row to an output */
        tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
    }
}

static inline void
init_context_node(ContextNode *node, 
                  const char *name, const char *parent_name, 
                  int level, uint64 used_bytes, uintptr_t address)
{
    strlcpy(node->name, name, CONTEXT_NAME_MAX_LEN);
    strlcpy(node->parent_name, parent_name, CONTEXT_NAME_MAX_LEN);
    node->level = level;
    node->used_bytes = used_bytes;
    node->address = address;
}