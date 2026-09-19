#include <dlfcn.h>
#include "global.h"
#include "random.h"
#include "sim_globals.h"

static u16 RandomInner(void);

struct SimRngTraceEntry gSimRngTrace[SIM_RNG_TRACE_MAX];
int gSimRngTraceCount;
int gSimRngTraceEnabled;

static void Trace(u16 v, void *caller)
{
    if (gSimRngTraceCount < SIM_RNG_TRACE_MAX)
    {
        Dl_info info;
        gSimRngTrace[gSimRngTraceCount].value = v;
        gSimRngTrace[gSimRngTraceCount].caller = (dladdr(caller, &info) && info.dli_sname) ? info.dli_sname : "?";
        gSimRngTraceCount++;
    }
}

u16 Random(void)
{
    u16 v = RandomInner();
    if (gSimRngTraceEnabled)
        Trace(v, __builtin_return_address(0));
    return v;
}

static u16 RandomInner(void)
{
    gSim->rngCalls++;
    if (gSim->rngScript != NULL)
    {
        u16 v;
        if (gSim->rngScriptPos < gSim->rngScriptLen)
            v = gSim->rngScript[gSim->rngScriptPos++];
        else
        {
            gSim->rngScriptUnderflow++;
            gRngValue = ISO_RANDOMIZE1(gRngValue);
            v = gRngValue >> 16;
        }
        gRngValue = (u32)v << 16;
        return v;
    }
    if (gSim->rngXorshift)
    {
        u32 x = gRngValue ? gRngValue : 0x9E3779B9;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        gRngValue = x;
        return x >> 16;
    }
    gRngValue = ISO_RANDOMIZE1(gRngValue);
    return gRngValue >> 16;
}

void SeedRng(u16 seed)
{
    gRngValue = seed;
}
