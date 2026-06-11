#ifndef ANALYZER_BGW_H
#define ANALYZER_BGW_H

#include "analyzer_core.h"

#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "storage/procsignal.h"

/* Shared memory state used for communication between backend processes and background workers */
typedef struct AnalyzerIPC
{
    slock_t mutex;           /* Mutex */
    pid_t target_pid;        /* Target BGW PID for snapshot request */
    bool dump_ready;         /* Indicates that snapshot is taken */
    int max_context_level;   /* Snapshot depth limit */
    bool merge_contexts;     /* Whether matching contexts should be merged */
    MemorySnapshot snapshot; /* Last captured memory snapshot */
} AnalyzerIPC;

/* Pointer to shared AnalyzerIPC state in shared memory that must be initialized in shmem_startup_hook */
extern AnalyzerIPC *ipc_state;

/* Previous shared memory hooks */
extern shmem_request_hook_type prev_shmem_request_hook;
extern shmem_startup_hook_type prev_shmem_startup_hook;
/* 
 * Custom process signal used to trigger background worker snapshot 
 * Assigned dynamically during extension initialization using
 * RegisterCustomProcSignalHandler().
 */
extern ProcSignalReason bgw_snapshot_signal_reason;

extern void analyzer_shmem_startup(void);
extern void analyzer_shmem_request(void);
extern void bgw_snapshot_signal_handler(void);

Datum get_bgw_memory_snapshot(PG_FUNCTION_ARGS);
Datum analyze_bgw(PG_FUNCTION_ARGS);

#endif // ANALYZER_BGW_H