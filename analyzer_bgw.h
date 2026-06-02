#ifndef ANALYZER_BGW_H
#define ANALYZER_BGW_H

#include "analyzer_core.h"

#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/spin.h"

typedef struct AnalyzerIPC {
    slock_t mutex;
    pid_t target_pid;
    bool dump_ready;
    MemorySnapshot snapshot;
} AnalyzerIPC;

extern AnalyzerIPC *ipc_state;

extern shmem_request_hook_type prev_shmem_request_hook;
extern shmem_startup_hook_type prev_shmem_startup_hook;

extern void analyzer_shmem_startup(void);
extern void analyzer_shmem_request(void);

#endif // ANALYZER_BGW_H