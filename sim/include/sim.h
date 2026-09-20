// Battle simulator state and public API.
//
// The simulator is a port of pokefirered's battle engine (battle_main.c, battle_util.c,
// battle_script_commands.c, the battle scripts bytecode, pokemon.c) with the UI/controller
// layer replaced by a synchronous controller that can either call a policy callback or
// suspend and hand a decision back to the caller.
//
// All engine globals live inside `struct BattleSim` (see gen/sim_state_fields.h) so a whole
// battle can be copied with memcpy for rollouts. The engine code accesses them through the
// macros in sim_globals.h via the thread-local pointer `gSim`.
#ifndef GUARD_SIM_H
#define GUARD_SIM_H

#include <stdio.h>
#include "global.h"
#include "battle.h"
#include "pokemon.h"
#include "battle_controllers.h"
#include "battle_message.h"

// Embedded (non-pointer) version of struct BattleResources.
struct SimBattleResources
{
    struct ResourceFlags flags;
    struct BattleScriptsStack battleScriptsStack;
    struct BattleCallbacksStack battleCallbackStack;
    struct StatsArray beforeLvlUp;
    struct AI_ThinkingStruct ai;
    struct BattleHistory battleHistory;
    struct BattleScriptsStack AI_ScriptsStack;
};

// What the engine is waiting on.
enum
{
    SIM_REQ_NONE,
    SIM_REQ_ACTION,   // start of turn: choose move/switch/item/run for requestBattler
    SIM_REQ_SWITCH,   // mid-turn: choose a party member to send out for requestBattler
};

// Result of Sim_Run().
enum
{
    SIM_RUN_REQUEST,  // a decision is pending; see sim->requestKind / requestBattler
    SIM_RUN_FINISHED, // battle over; see gBattleOutcome
    SIM_RUN_STUCK,    // engine made no progress (bug / unsupported path)
    SIM_RUN_ERROR,    // the engine hit an invariant trap or a policy kept answering illegally; see sim->error
};

struct SimAction
{
    u8 type;       // B_ACTION_USE_MOVE, B_ACTION_SWITCH, B_ACTION_USE_ITEM, B_ACTION_RUN
    u8 moveSlot;   // 0..3 (B_ACTION_USE_MOVE)
    u8 target;     // battler id (B_ACTION_USE_MOVE)
    u8 partySlot;  // 0..5 (B_ACTION_SWITCH / SIM_REQ_SWITCH answers)
    u16 item;      // item id (B_ACTION_USE_ITEM)
};

enum SimError
{
    SIM_ERR_NONE,
    SIM_ERR_TRAP,          // engine invariant violated (controller exec flags corrupted)
    SIM_ERR_BAD_POLICY,    // a policy callback returned illegal actions too many times in a row
    SIM_ERR_STUCK,         // step budget exhausted
};

struct BattleSim;
typedef void (*SimPolicyFunc)(struct BattleSim *sim, u8 battler, u8 requestKind, struct SimAction *out);

struct SimLogEntry
{
    u16 stringId;
    u8 battler;
    u8 multistring;
    u16 move;        // gCurrentMove at the time
    u16 hpTarget;    // gBattleMons[gBattlerTarget].hp
    s32 dmg;         // gBattleMoveDamage
    u16 turn;        // sim->turnCount at the time
};

#define SIM_LOG_MAX 400

struct BattleSim
{
#include "sim_state_fields.h"

    // --- simulator specific ---
    u8 requestKind;
    u8 requestBattler;
    u8 finished;
    u8 exp_enabled;
    u8 createTrainerParty;           // Sim_Start creates gTrainers[gTrainerBattleOpponent_A]'s party (set by Sim_LoadTrainerParty)
    struct SimAction answer[MAX_BATTLERS_COUNT];
    u8 answerValid[MAX_BATTLERS_COUNT];
    u8 answerKind[MAX_BATTLERS_COUNT];  // request kind the stored answer was produced for
    SimPolicyFunc policy[2];         // per side; NULL => suspend and ask the caller
    u32 badgeFlags;                  // bit i set => player has badge i+1 (stat boosts)
    u8 logEnabled;
    u8 logCount;
    struct SimLogEntry log[SIM_LOG_MAX];
    u32 frames;
    // RNG replay (cross-checking against the real game): when rngScript is set, Random() returns the
    // scripted values in order instead of advancing the LCG.
    const u16 *rngScript;
    u32 rngScriptLen;
    u32 rngScriptPos;
    u32 rngScriptUnderflow;          // calls made after the script ran out
    u8 rngXorshift;                  // 1 = xorshift32 on gRngValue instead of the game's LCG (better rollouts; also
                                     //     what the cross-check harness ROM uses for engine calls)
    u32 rngCalls;                    // Random() calls made so far
    u16 turnCount;                   // full turns elapsed (not saturating like gBattleResults)
    u16 maxTurns;                    // battle is declared a draw after this many turns (0 = no cap)
    u8 strictAnswers;                // 1: Sim_Answer rejects illegal actions (the game's re-prompt paths are never entered)
    u8 error;                        // enum SimError, set when Sim_Run returns SIM_RUN_ERROR
    u16 rejectedAnswers;             // illegal answers seen (Sim_Answer rejections + policy fallbacks)
    // Elapsed-turn counters for random-duration effects (what a player can observe; the engine stores the
    // remaining count). Reset when the effect is applied, incremented at each move attempt while it lasts.
    u8 sleepElapsed[2][PARTY_SIZE];  // per party slot (sleep persists across switches)
    u8 sleepKnown[2][PARTY_SIZE];    // 1: fixed-length sleep (Rest)
    u8 confusionElapsed[MAX_BATTLERS_COUNT];
};

extern _Thread_local struct BattleSim *gSim;

// --- lifecycle ---
void Sim_Bind(struct BattleSim *sim);                 // make `sim` the current state (sets gSim)
void Sim_Init(struct BattleSim *sim, u32 battleTypeFlags, u16 seed); // zero state, bind, set flags/seed
// Starts the battle. Returns 0, or -1 if a side has no usable mon (or a double battle would start with one),
// -2 if a party holds data the engine cannot encode (species/move/item ids out of range, level 0 or > 100,
//    a corrupted mon, or an empty slot before a filled one: parties are contiguous from slot 0),
// -3 if battleTypeFlags carry bits the simulator does not support (only TRAINER and DOUBLE are).
int Sim_Start(struct BattleSim *sim);                 // prepare the battle; returns -1 for an invalid setup
int Sim_Run(struct BattleSim *sim);                   // run until a request or the end of the battle
// Answers the pending request. Returns 0 if accepted, -1 if no such request is pending or (with
// sim->strictAnswers) the action is not legal in the current state; a rejected answer leaves the request
// pending so the caller can choose again. Legality is what Sim_LegalActions / Sim_LegalSwitches enumerate,
// plus B_ACTION_USE_ITEM with an item the game lets you use in battle (B_ACTION_RUN is never legal: only
// trainer battles are simulated). Without strictAnswers
// an illegal choice is handed to the engine, which re-prompts the way the game does (no PP, Taunt, ...);
// either way nothing a caller passes can crash the simulator (see tests/robust.c).
int Sim_Answer(struct BattleSim *sim, u8 battler, const struct SimAction *action);
int Sim_ValidateAction(struct BattleSim *sim, u8 battler, u8 requestKind, const struct SimAction *action); // 1 legal, 0 not
void Sim_SetPolicy(struct BattleSim *sim, u8 side, SimPolicyFunc policy);

// --- helpers for building parties ---
struct Pokemon *Sim_Party(struct BattleSim *sim, u8 side);
void Sim_MakeMon(struct Pokemon *mon, u16 species, u8 level, u8 nature, const u8 *ivs, const u8 *evs,
                 const u16 *moves, u16 item, u8 abilityNum);
// Same, with an explicit original-trainer id (0 = the player's own id; anything else is an "outsider"
// mon subject to the obedience rules) and the fateful-encounter bit (needed for Mew/Deoxys to obey).
int Sim_MakeMonEx(struct Pokemon *mon, u16 species, u8 level, u8 nature, const u8 *ivs, const u8 *evs,
                  const u16 *moves, u16 item, u8 abilityNum, u32 otId, u8 fatefulEncounter); // 0, or -1 (mon zeroed) for ids the engine cannot index
void Sim_MakeTrainerMon(struct Pokemon *mon, u16 species, u8 level, u8 fixedIV, const u16 *moves, u16 item, u16 trainerNum);
u8 Sim_LoadTrainerParty(struct BattleSim *sim, u16 trainerNum); // loads gTrainers[trainerNum] into the enemy party
void Sim_PrintLog(struct BattleSim *sim);                        // dump the message log (needs logEnabled)
void Sim_PrintBattlers(struct BattleSim *sim);

// --- legality ---
int Sim_LegalActions(struct BattleSim *sim, u8 battler, struct SimAction *out, int maxOut);
int Sim_LegalSwitches(struct BattleSim *sim, u8 battler, u8 *outSlots, int maxOut);

// --- move legality (FRLG learnsets, incl. pre-evolutions) ---
bool8 Sim_LearnsByLevelUp(u16 species, u16 move, u8 maxLevel);
bool8 Sim_LearnsByTMHM(u16 species, u16 move);
bool8 Sim_LearnsByTutor(u16 species, u16 move);
bool8 Sim_IsEggMove(u16 species, u16 move);
u16 Sim_PreEvolution(u16 species);
// --- team corpus (teams/FORMAT.md; flat TSV rows from tools/teams.py) ---
struct SimTeamMon
{
    char teamId[64];
    u8 slot;
    u16 species;
    u8 level;
    u16 moves[4];
    u16 item;
    u8 abilityNum;      // 0/1, or 255 = resolve from abilityName
    s16 abilityName;    // ABILITY_* when abilityNum == 255
    u8 nature;
    u16 ivs[6];
    u16 evs[6];
    u8 happiness;
    u8 outsider;        // OT is not the player (obedience rules apply)
    u8 fateful;
    u8 doubles;
};
int Sim_ParseTeamRow(const char *line, struct SimTeamMon *out);                 // 1 on success
int Sim_CheckTeamMon(const struct SimTeamMon *m, char *why, int whyLen);        // 2 legal, 1 encodable, 0 invalid
void Sim_BuildTeamMon(struct Pokemon *mon, const struct SimTeamMon *m);
int Sim_ReadTeamTsv(FILE *f, struct Pokemon *party, char *teamId, int teamIdLen, u8 *doubles); // mons read, 0 at EOF
bool8 Sim_CanLearnMove(u16 species, u16 move, u8 level);
int Sim_LearnableMoves(u16 species, u8 level, u16 *out, int maxOut);

// --- policies ---
// The game's own trainer AI (opponent side only; uses gTrainers[gTrainerBattleOpponent_A].aiFlags and items).
void Sim_VanillaAIPolicy(struct BattleSim *sim, u8 battler, u8 requestKind, struct SimAction *out);
void Sim_RandomPolicy(struct BattleSim *sim, u8 battler, u8 requestKind, struct SimAction *out);

// --- Random() call tracing (host only; for cross-checking) ---
#define SIM_RNG_TRACE_MAX 4096
struct SimRngTraceEntry { u16 value; const char *caller; };
extern struct SimRngTraceEntry gSimRngTrace[SIM_RNG_TRACE_MAX];
extern int gSimRngTraceCount;
extern int gSimRngTraceEnabled;

// --- internals used by the controller / engine ---
void *SimDecodePtr(u32 encoded);
void SimSetControllerToSim(void);
void SimLog(u16 stringId, u8 battler);

#endif // GUARD_SIM_H
