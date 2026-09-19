// Battle-simulator cross-check harness (built with SIM_HARNESS=1, see sim/harness/README.md).
// The ROM boots straight into a trainer battle configured through an EWRAM mailbox that an mGBA Lua
// script fills in; the player's decisions come from the same mailbox and every Random() call is logged.
#ifndef GUARD_SIM_HARNESS_H
#define GUARD_SIM_HARNESS_H

#include "global.h"

#define SIM_HARNESS_MAGIC     0x53494D48 // 'SIMH'
#define SIM_RNG_LOG_SIZE      256

#define SIM_HARNESS_STATE_IDLE      0
#define SIM_HARNESS_STATE_IN_BATTLE 1
#define SIM_HARNESS_STATE_FINISHED  2

#define SIM_HARNESS_REQ_NONE   0
#define SIM_HARNESS_REQ_ACTION 1
#define SIM_HARNESS_REQ_SWITCH 2

#define SIM_HARNESS_MAX_RANGES 16

struct SimHarness
{
    /*0x00*/ u32 magic;
    /*0x04*/ u32 go;              // Lua sets to 1 after filling in the fields below (and gPlayerParty)
    /*0x08*/ u32 battleTypeFlags;
    /*0x0C*/ u16 trainerId;
    /*0x0E*/ u16 seed;            // seed for the game's own LCG (non-engine callers)
    /*0x10*/ u8 terrain;
    /*0x11*/ u8 badges;
    /*0x12*/ u8 battleStyle;      // 1 = SET
    /*0x13*/ u8 sceneOff;
    /*0x14*/ u32 state;           // SIM_HARNESS_STATE_*
    /*0x18*/ u32 outcome;         // gBattleOutcome once finished
    /*0x1C*/ u32 battleCount;
    /*0x20*/ u32 request;         // SIM_HARNESS_REQ_*
    /*0x24*/ u32 requestBattler;
    /*0x28*/ u32 requestSeq;
    /*0x2C*/ u32 answerSeq;       // Lua sets equal to requestSeq once the answer fields are written
    /*0x30*/ u8 ansType;
    /*0x31*/ u8 ansMoveSlot;
    /*0x32*/ u8 ansTarget;
    /*0x33*/ u8 ansPartySlot;
    /*0x34*/ u16 ansItem;
    /*0x36*/ u16 caseId;          // party-screen case id of the current switch request
    // Battle-engine RNG: Random() calls made from code inside engineRanges draw from a xorshift32 stream
    // (identical to the simulator's) instead of the game's LCG, so both sides see the same rolls.
    /*0x38*/ u32 engineRngState;  // current xorshift state (Lua seeds it; 0 = disabled)
    /*0x3C*/ u32 engineRngCalls;  // number of engine calls so far
    /*0x40*/ u32 otherRngCalls;   // number of non-engine calls so far
    /*0x44*/ u32 rangeCount;
    /*0x48*/ u32 engineRanges[SIM_HARNESS_MAX_RANGES][2]; // [start, end) ROM addresses
    /*0xC8*/ u32 callerRing[64];  // caller addresses of the most recent engine calls (index = engineRngCalls % 64)
    /*0x1C8*/ u8 scriptOpponent;  // 1 = the opponent's decisions also come from the mailbox (scenario tests)
    /*0x1C9*/ u8 useEnemyParty;   // 1 = gEnemyParty was written by Lua; do not create the trainer's party
    /*0x1CA*/ u8 mainLoopBusy;  // 1 while the main loop iteration runs; 0 while waiting for VBlank (Lua snapshots only then)
    /*0x1CB*/ u8 pad;
};

extern struct SimHarness gSimHarness;

// Consistent snapshot of the state the cross-check compares, copied by SimHarness_EndOfMainLoop() at the end
// of every main-loop iteration. A GBA frame ends on a cycle budget, so the emulator's frame callback can fire
// in the middle of an iteration (e.g. during a long AI evaluation); the bridge therefore reads this copy,
// and only while `busy` is 0, instead of the live variables.
struct SimHarnessShadow
{
    /*0x000*/ u32 busy;            // 1 while the copy is in progress
    /*0x004*/ u32 iters;           // completed main-loop iterations
    /*0x008*/ u32 engineRngCalls;
    /*0x00C*/ u32 otherRngCalls;
    /*0x010*/ u32 callerRing[64];
    /*0x110*/ u8 battleMons[88 * 4];
    /*0x270*/ u8 statuses3[16];
    /*0x280*/ u8 sideStatuses[4];
    /*0x284*/ u8 sideTimers[24];
    /*0x29C*/ u8 disableStructs[28 * 4];
    /*0x30C*/ u8 weather[2];
    /*0x30E*/ u8 pad0[2];
    /*0x310*/ u8 wishFutureKnock[44];
    /*0x33C*/ u8 playerParty[600];
    /*0x594*/ u8 enemyParty[600];
    /*0x7EC*/ u8 battlerPartyIndexes[8];
    /*0x7F4*/ u8 absentBattlerFlags;
    /*0x7F5*/ u8 battleOutcome;
    /*0x7F6*/ u8 pad1[2];
    /*0x7F8*/ u8 battleCommunication[8];
    /*0x800*/ u32 controllerExecFlags;
    /*0x804*/ u32 request;         // mailbox request/requestBattler/requestSeq/state as seen at the end of the iteration
    /*0x808*/ u32 requestBattler;
    /*0x80C*/ u32 requestSeq;
    /*0x810*/ u32 state;
};
extern struct SimHarnessShadow gSimHarnessShadow;
void SimHarness_EndOfMainLoop(void);

void SimHarness_CB2_Boot(void);
bool8 SimHarness_EngineRandom(u32 caller, u16 *value);
void SimHarness_HandleChooseAction(void);
void SimHarness_HandleChooseMove(void);
void SimHarness_HandleChooseItem(void);
void SimHarness_HandleChoosePokemon(void);
void SimHarness_HandleExpUpdate(void);

#endif // GUARD_SIM_HARNESS_H
