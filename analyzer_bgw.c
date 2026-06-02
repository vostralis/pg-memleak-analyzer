#include "analyzer_bgw.h"

AnalyzerIPC *ipc_state = NULL;
shmem_request_hook_type prev_shmem_request_hook = NULL;
shmem_startup_hook_type prev_shmem_startup_hook = NULL;

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