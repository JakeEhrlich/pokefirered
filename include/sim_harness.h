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
    /*0x1CA*/ u16 pad;
};

extern struct SimHarness gSimHarness;

void SimHarness_CB2_Boot(void);
bool8 SimHarness_EngineRandom(u32 caller, u16 *value);
void SimHarness_HandleChooseAction(void);
void SimHarness_HandleChooseMove(void);
void SimHarness_HandleChooseItem(void);
void SimHarness_HandleChoosePokemon(void);
void SimHarness_HandleExpUpdate(void);

#endif // GUARD_SIM_HARNESS_H
