#include "global.h"
#include "random.h"
#if SIM_HARNESS
#include "sim_harness.h"
#endif

// The number 1103515245 comes from the example implementation
// of rand and srand in the ISO C standard.

COMMON_DATA u32 gRngValue = 0;

u16 Random(void)
{
#if SIM_HARNESS
    u16 v;
    if (SimHarness_EngineRandom((u32)__builtin_return_address(0), &v))
        return v;
#endif
    gRngValue = ISO_RANDOMIZE1(gRngValue);
    return gRngValue >> 16;
}

void SeedRng(u16 seed)
{
    gRngValue = seed;
}
