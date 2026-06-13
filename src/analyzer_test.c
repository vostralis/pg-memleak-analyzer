#include "postgres.h"
#include "fmgr.h"
#include "utils/memutils.h"

PG_FUNCTION_INFO_V1(trigger_context_leak);
Datum
trigger_context_leak(PG_FUNCTION_ARGS)
{
    MemoryContextAlloc(TopMemoryContext, 1000);
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(trigger_multiple_contexts_leak);
Datum
trigger_multiple_contexts_leak(PG_FUNCTION_ARGS)
{
    for (int i = 0; i < 3; ++i)
    {
        MemoryContext ctx = AllocSetContextCreate(TopMemoryContext, "TestContext", ALLOCSET_DEFAULT_SIZES);
        MemoryContextAlloc(ctx, 1024);
    }

    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(trigger_nested_contexts_leak);
Datum trigger_nested_contexts_leak(PG_FUNCTION_ARGS)
{
    MemoryContext lvl1 = AllocSetContextCreate(TopMemoryContext, "Level1Context", ALLOCSET_DEFAULT_SIZES);
    MemoryContext lvl2 = AllocSetContextCreate(lvl1, "Level2Context", ALLOCSET_DEFAULT_SIZES);
    MemoryContext lvl3 = AllocSetContextCreate(lvl2, "Level3Context", ALLOCSET_DEFAULT_SIZES);
    
    MemoryContextAlloc(lvl3, 1024);
    
    PG_RETURN_VOID();
}