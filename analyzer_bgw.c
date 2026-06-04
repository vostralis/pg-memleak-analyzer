#include "analyzer_bgw.h"

AnalyzerIPC *ipc_state = NULL;

shmem_request_hook_type prev_shmem_request_hook = NULL;
shmem_startup_hook_type prev_shmem_startup_hook = NULL;

ProcSignalReason bgw_snapshot_signal_reason = INVALID_PROCSIGNAL;


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
    
    reset_snapshot(&local_snapshot);

    SpinLockAcquire(&ipc_state->mutex);
    is_target = (ipc_state->target_pid == MyProcPid);
    SpinLockRelease(&ipc_state->mutex);
    
    if (is_target)
    {
        traverse_memory_contexts(TopMemoryContext, TOP_CONTEXT_PARENT_LABEL, 0, &local_snapshot);
        
        SpinLockAcquire(&ipc_state->mutex);
        memcpy(&ipc_state->snapshot, &local_snapshot, sizeof(MemorySnapshot));
        ipc_state->dump_ready = true;
        SpinLockRelease(&ipc_state->mutex);
    }
    
}

PG_FUNCTION_INFO_V1(get_bgw_memory_snapshot);

Datum get_bgw_memory_snapshot(PG_FUNCTION_ARGS)
{
    pid_t target_pid = PG_GETARG_INT32(0);
    ReturnSetInfo *rsinfo = (ReturnSetInfo *)fcinfo->resultinfo;
    MemorySnapshot snapshot;
    bool ready = false;

    InitMaterializedSRF(fcinfo, 0);

    SpinLockAcquire(&ipc_state->mutex);
    ipc_state->target_pid = target_pid;
    ipc_state->dump_ready = false;
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
        pg_usleep(10000);
        
        SpinLockAcquire(&ipc_state->mutex);

        if (ipc_state->dump_ready)
        {
            ready = ipc_state->dump_ready;
            memcpy(&snapshot, &ipc_state->snapshot, sizeof(MemorySnapshot));
            ipc_state->target_pid = 0;
        }
        
        SpinLockRelease(&ipc_state->mutex);
    }

    for (int i = 0; i < snapshot.node_count; ++i)
    {
        ContextNode *node = &snapshot.nodes[i];
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