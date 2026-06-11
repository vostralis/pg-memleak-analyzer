#include "analyzer_bgw.h"

AnalyzerIPC *ipc_state = NULL;

shmem_request_hook_type prev_shmem_request_hook = NULL;
shmem_startup_hook_type prev_shmem_startup_hook = NULL;

ProcSignalReason bgw_snapshot_signal_reason = INVALID_PROCSIGNAL; /* Custom reason */

static void fetch_bgw_snapshot(pid_t target_pid, MemorySnapshot *local_snapshot);

/*
 * analyzer_shmem_startup
 *
 * Initializes shared memory segment for analyzer subsystem.
 * Called during PostgreSQL shared memory startup phase.
 */
extern void
analyzer_shmem_startup(void)
{
    bool found;

    if (prev_shmem_startup_hook)
        prev_shmem_startup_hook();

    /* Initialize AnalyzerIPC structure in shared memory */
    LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
    ipc_state = (AnalyzerIPC *)ShmemInitStruct("MemleakAnalyzerIPC", sizeof(AnalyzerIPC), &found);
    LWLockRelease(AddinShmemInitLock);

    /* If initialization was succeesful, reset structure's fields */
    if (!found)
    {
        SpinLockInit(&ipc_state->mutex);
        ipc_state->target_pid = 0;
        ipc_state->dump_ready = false;
        reset_snapshot(&ipc_state->snapshot);
    }
}

/*
 * analyzer_shmem_request
 *
 * Requests required amount of shared memory for AnalyzerIPC state.
 * Called during PostgreSQL shared memory sizing phase.
 */
extern void
analyzer_shmem_request(void)
{
    if (prev_shmem_request_hook)
        prev_shmem_request_hook();

    RequestAddinShmemSpace(MAXALIGN(sizeof(AnalyzerIPC)));
}

/*
 * bgw_snapshot_signal_handler
 *
 * Signal handler executed in background worker context when a snapshot
 * request is received via custom ProcSignal.
 *
 * Collects memory context tree snapshot and stores it in shared memory.
 */
extern void
bgw_snapshot_signal_handler(void)
{
    MemorySnapshot *local_snapshot = (MemorySnapshot *)palloc0(sizeof(MemorySnapshot));
    bool is_target = false;
    int local_max_context_level = -1;
    bool local_merge_contexts = false;

    /* Fetch query parameters from shared memory */
    SpinLockAcquire(&ipc_state->mutex);
    is_target = (ipc_state->target_pid == MyProcPid);
    local_max_context_level = ipc_state->max_context_level;
    local_merge_contexts = ipc_state->merge_contexts;
    SpinLockRelease(&ipc_state->mutex);
    
    /* Take a memory snapshot if the pid of current proccess matches the target pid */
    if (is_target)
    {
        reset_snapshot(local_snapshot);

        traverse_memory_contexts(
            TopMemoryContext, TOP_CONTEXT_PARENT_LABEL, TOP_CONTEXT_LEVEL, 
            local_max_context_level, local_merge_contexts, local_snapshot
        );
        
        /* Copy snapshot to the shared memory */
        SpinLockAcquire(&ipc_state->mutex);
        memcpy(&ipc_state->snapshot, local_snapshot, sizeof(MemorySnapshot));
        ipc_state->dump_ready = true;
        SpinLockRelease(&ipc_state->mutex);
    }
    
    pfree(local_snapshot);
}

PG_FUNCTION_INFO_V1(get_bgw_memory_snapshot);

/*
 * get_bgw_memory_snapshot
 *
 * SRF function that retrieves a single snapshot of a background worker memory context tree
 * that is obtained via request to the target process.
 */
Datum get_bgw_memory_snapshot(PG_FUNCTION_ARGS)
{
    pid_t target_pid =  PG_GETARG_INT32(0);

    ReturnSetInfo  *rsinfo   = (ReturnSetInfo *)fcinfo->resultinfo;
    MemorySnapshot *snapshot = (MemorySnapshot *)palloc0(sizeof(MemorySnapshot));

    InitMaterializedSRF(fcinfo, 0);

    /* Capture a single snapshot */
    fetch_bgw_snapshot(target_pid, snapshot);

    for (int i = 0; i < snapshot->node_count; ++i)
    {
        ContextNode *node = &snapshot->nodes[i];

        Datum values[4];
        bool nulls[4] = { false };

        values[0] = CStringGetTextDatum(node->name);        /* context_name */
        values[1] = CStringGetTextDatum(node->parent_name); /* parent_name */
        values[2] = Int32GetDatum(node->level);             /* level */
        values[3] = UInt64GetDatum(node->used_bytes);       /* allocated */

        /* Append result row to an output */
        tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
    }

    pfree(snapshot);

    PG_RETURN_NULL();
}

/*
 * fetch_bgw_snapshot
 *
 * Requests a memory context snapshot from a background worker process.
 *
 * Sends custom signal to target PID and waits until snapshot is
 * captured and stored in shared memory.
 */
static void
fetch_bgw_snapshot(pid_t target_pid, MemorySnapshot *local_snapshot)
{
    bool ready = false;

    /* Write query parameters to the shared memory */
    SpinLockAcquire(&ipc_state->mutex);
    ipc_state->target_pid        = target_pid;
    ipc_state->dump_ready        = false;
    ipc_state->max_context_level = analyzer_max_context_level;
    ipc_state->merge_contexts    = analyzer_merge_contexts;
    SpinLockRelease(&ipc_state->mutex);

    /* Notify target process that a snapshot is requested */
    if (SendProcSignal(target_pid, bgw_snapshot_signal_reason, INVALID_PROC_NUMBER) < 0)
    {
        ereport(
            ERROR,
            errcode(ERRCODE_SYSTEM_ERROR),
            errmsg("Failed to route signal to PID %d", target_pid)
        );
    }

    /* Wait loop for snapshot completion */
    while (!ready)
    {
        CHECK_FOR_INTERRUPTS();

        WaitLatch(
            MyLatch,
            WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
            1000L,
            PG_WAIT_EXTENSION
        );
        ResetLatch(MyLatch);

        SpinLockAcquire(&ipc_state->mutex);

        /* Snapshot is considered ready when dump_ready flag is set */
        if (ipc_state->dump_ready)
        {
            ready = ipc_state->dump_ready;
            /* At this point we copy the shared snapshot into the local memory */
            memcpy(local_snapshot, &ipc_state->snapshot, sizeof(MemorySnapshot));
            ipc_state->target_pid = 0;
        }
        
        SpinLockRelease(&ipc_state->mutex);
    }
}

PG_FUNCTION_INFO_V1(analyze_bgw);

/*
 * analyze_bgw
 *
 * SRF function that performs time-window based memory profiling of a background worker.
 *
 * Captures two memory snapshots separated by a configurable interval
 * and computes differences between memory contexts.
 */
Datum analyze_bgw(PG_FUNCTION_ARGS)
{
    pid_t target_pid           = PG_GETARG_INT32(0);
    int32 observation_interval = PG_GETARG_INT32(1);

    ReturnSetInfo *rsinfo = (ReturnSetInfo *)fcinfo->resultinfo;

    MemorySnapshot *snapshot_before;
    MemorySnapshot *snapshot_after;

    if (observation_interval <= 0)
    {
        ereport(
            ERROR,
            errcode(ERRCODE_RAISE_EXCEPTION),
            errmsg("Observation interval must be greater than zero")
        );
    }

    InitMaterializedSRF(fcinfo, 0);

    snapshot_before = (MemorySnapshot *)palloc0(sizeof(MemorySnapshot));
    snapshot_after  = (MemorySnapshot *)palloc0(sizeof(MemorySnapshot));

    /* Capture first snapshot */
    fetch_bgw_snapshot(target_pid, snapshot_before);

    /* Wait for observation_interval seconds */
    WaitLatch(
        MyLatch,
        WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
        observation_interval * 1000L,
        PG_WAIT_EXTENSION
    );

    ResetLatch(MyLatch);
    CHECK_FOR_INTERRUPTS();

    /* Capture second snapshot */
    fetch_bgw_snapshot(target_pid, snapshot_after);
    /* Compute memory differences and materialize results */
    compute_contexts_diff(rsinfo, snapshot_before, snapshot_after);

    pfree(snapshot_before);
    pfree(snapshot_after);
    
    PG_RETURN_NULL();
}