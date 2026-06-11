#include "analyzer_bgw.h"

AnalyzerIPC *ipc_state = NULL;

shmem_request_hook_type prev_shmem_request_hook = NULL;
shmem_startup_hook_type prev_shmem_startup_hook = NULL;

ProcSignalReason bgw_snapshot_signal_reason = INVALID_PROCSIGNAL;

static void fetch_bgw_snapshot(pid_t target_pid, MemorySnapshot *local_snapshot);

extern void
analyzer_shmem_startup(void)
{
    bool found;

    if (prev_shmem_startup_hook)
        prev_shmem_startup_hook();

    LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
    ipc_state = (AnalyzerIPC *)ShmemInitStruct("MemleakAnalyzerIPC", sizeof(AnalyzerIPC), &found);
    LWLockRelease(AddinShmemInitLock);

    if (!found)
    {
        SpinLockInit(&ipc_state->mutex);
        ipc_state->target_pid = 0;
        ipc_state->dump_ready = false;
        reset_snapshot(&ipc_state->snapshot);
    }
}

extern void
analyzer_shmem_request(void)
{
    if (prev_shmem_request_hook)
        prev_shmem_request_hook();

    RequestAddinShmemSpace(MAXALIGN(sizeof(AnalyzerIPC)));
}

extern void
bgw_snapshot_signal_handler(void)
{
    MemorySnapshot local_snapshot = { .node_count = 0 };
    bool is_target = false;
    int local_max_context_level = -1;
    bool local_merge_contexts = false;

    SpinLockAcquire(&ipc_state->mutex);
    is_target = (ipc_state->target_pid == MyProcPid);
    local_max_context_level = ipc_state->max_context_level;
    local_merge_contexts = ipc_state->merge_contexts;
    SpinLockRelease(&ipc_state->mutex);
    
    if (is_target)
    {
        reset_snapshot(&local_snapshot);

        traverse_memory_contexts(
            TopMemoryContext, TOP_CONTEXT_PARENT_LABEL, 0, 
            local_max_context_level, local_merge_contexts, &local_snapshot
        );
        
        SpinLockAcquire(&ipc_state->mutex);
        memcpy(&ipc_state->snapshot, &local_snapshot, sizeof(MemorySnapshot));
        ipc_state->dump_ready = true;
        SpinLockRelease(&ipc_state->mutex);
    }
    
}

PG_FUNCTION_INFO_V1(get_bgw_memory_snapshot);

Datum get_bgw_memory_snapshot(PG_FUNCTION_ARGS)
{
    pid_t target_pid =  PG_GETARG_INT32(0);

    ReturnSetInfo  *rsinfo   = (ReturnSetInfo *)fcinfo->resultinfo;
    MemorySnapshot *snapshot = (MemorySnapshot *)palloc0(sizeof(MemorySnapshot));

    InitMaterializedSRF(fcinfo, 0);

    fetch_bgw_snapshot(target_pid, snapshot);

    for (int i = 0; i < snapshot->node_count; ++i)
    {
        ContextNode *node = &snapshot->nodes[i];

        Datum values[4];
        bool nulls[4] = { false };

        values[0] = CStringGetTextDatum(node->name);
        values[1] = CStringGetTextDatum(node->parent_name);
        values[2] = Int32GetDatum(node->level);
        values[3] = UInt64GetDatum((uint64)node->used_bytes);

        tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
    }

    PG_RETURN_NULL();
}

static void
fetch_bgw_snapshot(pid_t target_pid, MemorySnapshot *local_snapshot)
{
    bool ready = false;

    SpinLockAcquire(&ipc_state->mutex);
    ipc_state->target_pid        = target_pid;
    ipc_state->dump_ready        = false;
    ipc_state->max_context_level = analyzer_max_context_level;
    ipc_state->merge_contexts    = analyzer_merge_contexts;
    SpinLockRelease(&ipc_state->mutex);

    if (SendProcSignal(target_pid, bgw_snapshot_signal_reason, INVALID_PROC_NUMBER) < 0)
    {
        ereport(
            ERROR,
            errcode(ERRCODE_SYSTEM_ERROR),
            errmsg("Failed to route signal to PID %d", target_pid)
        );
    }

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

        if (ipc_state->dump_ready)
        {
            ready = ipc_state->dump_ready;
            memcpy(local_snapshot, &ipc_state->snapshot, sizeof(MemorySnapshot));
            ipc_state->target_pid = 0;
        }
        
        SpinLockRelease(&ipc_state->mutex);
    }
}

PG_FUNCTION_INFO_V1(analyze_bgw);

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

    fetch_bgw_snapshot(target_pid, snapshot_before);

    WaitLatch(
        MyLatch,
        WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
        observation_interval * 1000L,
        PG_WAIT_EXTENSION
    );

    ResetLatch(MyLatch);
    CHECK_FOR_INTERRUPTS();

    fetch_bgw_snapshot(target_pid, snapshot_after);

    compute_contexts_diff(rsinfo, snapshot_before, snapshot_after);

    pfree(snapshot_before);
    pfree(snapshot_after);
    
    PG_RETURN_NULL();
}