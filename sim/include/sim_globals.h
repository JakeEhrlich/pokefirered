// Remaps the game's battle globals onto the current simulator state. Must be the LAST include
// in each ported engine source file (after all game headers, which declare the globals extern).
#ifndef GUARD_SIM_GLOBALS_H
#define GUARD_SIM_GLOBALS_H

#include "sim.h"
#include "sim_globals_gen.h"

#define gBattleStruct (&gSim->sBattleStructStorage)
#define gBattleResources (&gSim->sBattleResourcesStorage)

// The one-line battler helpers of src/sim_support.c, inlined: on the host every call costs a thread-local
// lookup of gSim. Same argument conversion and result as the functions (which stay defined for other callers).
#define GetBattlerSide(battler) ((u8)GET_BATTLER_SIDE2((u8)(battler)))
#define GetBattlerPosition(battler) ((u8)GET_BATTLER_POSITION((u8)(battler)))

// Battle script pointers are stored encoded in the bytecode blob (see tools/bsasm.py).
#undef T1_READ_PTR
#undef T2_READ_PTR
#define T1_READ_PTR(ptr) ((u8 *)SimDecodePtr(T1_READ_32(ptr)))
#define T2_READ_PTR(ptr) ((void *)SimDecodePtr(T2_READ_32(ptr)))

#endif // GUARD_SIM_GLOBALS_H

// Same values as the game's macro, without the signed-int shift (undefined behaviour on the host).
#undef Random32
#define Random32() ((u32)Random() | ((u32)Random() << 16))
