#include "analyzer_backend.h"
#include "analyzer_bgw.h"

#include "utils/guc.h"

PG_MODULE_MAGIC;

/* Forward declarations */
void _PG_init(void);
void _PG_fini(void);

void
_PG_init(void)
{
    if (!process_shared_preload_libraries_in_progress)
    {
        ereport(
            ERROR,
            errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
            errmsg("pg_memleak_analyzer must be loaded via shared_preload_libraries")
        );
    }

    /* Install new and save previous executor hooks */
    prev_shmem_request_hook = shmem_request_hook;
    shmem_request_hook = analyzer_shmem_request;

    prev_shmem_startup_hook = shmem_startup_hook;
    shmem_startup_hook = analyzer_shmem_startup;
    
    prev_ExecutorStart = ExecutorStart_hook;
    ExecutorStart_hook = analyzer_ExecutorStart;

    prev_ExecutorEnd = ExecutorEnd_hook;
    ExecutorEnd_hook = analyzer_ExecutorEnd;

    /*
     * Register GUC variable that controls
     * transactional behavior during query profiling
     */
    DefineCustomBoolVariable(
        "memleak_analyzer.rollback_mode",
        "Rollback analyzed query after execution",
        NULL,
        &backend_rollback_mode,
        true,
        PGC_USERSET,
        0,
        NULL, NULL, NULL
    );

    DefineCustomIntVariable(
        "memleak_analyzer.max_context_level_displayed",
        "Maximum depth level of memory contexts to display (-1 for all)",
        NULL,
        &max_context_level_displayed,
        -1,
        -1,
        100,
        PGC_USERSET,
        0,
        NULL, NULL, NULL
    );

    bgw_snapshot_signal_reason = RegisterCustomProcSignalHandler(bgw_snapshot_signal_handler);
}

void
_PG_fini(void)
{
    /* Restore saved hooks on unload */
    shmem_request_hook = prev_shmem_request_hook;
    shmem_startup_hook = prev_shmem_startup_hook;
    ExecutorStart_hook = prev_ExecutorStart;
    ExecutorEnd_hook   = prev_ExecutorEnd;
}