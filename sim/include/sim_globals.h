// Remaps the game's battle globals onto the current simulator state. Must be the LAST include
// in each ported engine source file (after all game headers, which declare the globals extern).
#ifndef GUARD_SIM_GLOBALS_H
#define GUARD_SIM_GLOBALS_H

#include "sim.h"
#include "sim_globals_gen.h"

#define gBattleStruct (&gSim->sBattleStructStorage)
#define gBattleResources (&gSim->sBattleResourcesStorage)

// Battle script pointers are stored encoded in the bytecode blob (see tools/bsasm.py).
#undef T1_READ_PTR
#undef T2_READ_PTR
#define T1_READ_PTR(ptr) ((u8 *)SimDecodePtr(T1_READ_32(ptr)))
#define T2_READ_PTR(ptr) ((void *)SimDecodePtr(T2_READ_32(ptr)))

#endif // GUARD_SIM_GLOBALS_H
