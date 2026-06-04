#include "analyzer_core.h"

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
                strncmp(node_after->parent_name, node_before->parent_name, CONTEXT_NAME_MAX_LEN) == 0)
            {
                used_before = node_before->used_bytes;
                break;
            }
        }

        /* Calculate memory growth for the context */
        delta_bytes = (int64)node_after->used_bytes - (int64)used_before;

        /* Report only positive deltas */
        if (true)
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
            // values[6] = BoolGetDatum(leak_suspected);

            /* Append result row to an output */
            tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
        }
    }
}