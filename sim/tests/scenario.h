// Declarative battle scenarios: fixed parties, scripted actions for every battler, and checks on the
// resulting simulator state. The same scenario runs (a) in the simulator alone (build/scenarios) and
// (b) through the real game in mGBA (build/crosscheck --scenarios), where every decision point compares
// the full battle state of both.
#ifndef GUARD_SCENARIO_H
#define GUARD_SCENARIO_H

#include <stdio.h>
#include "global.h"
#include "battle.h"
#include "pokemon.h"
#include "sim.h"
#include "sim_names.h"
#include "constants/species.h"
#include "constants/moves.h"
#include "constants/items.h"
#include "constants/abilities.h"
#include "constants/pokemon.h"
#include "constants/battle_string_ids.h"

#define SC_MAX_TURNS 40

struct ScenarioMon
{
    u16 species;
    u8 level;
    u16 moves[4];
    u16 item;
    u8 abilityNum;   // 0 = first ability, 1 = second
    u8 nature;
    u8 ivs[6];       // all 31 if ivsSet == 0
    u8 evs[6];
    u8 ivsSet;       // 1 = use ivs[] (else 31s)
    u16 hp;          // starting HP; 0 = full (use hpSet to start at 0 HP)
    u8 hpSet;        // 1 = use hp
    u32 status;      // starting status1 (STATUS1_*), e.g. STATUS1_SLEEP_TURN(3)
    u8 pp[4];        // starting PP; 0 = default (use ppSet)
    u8 ppSet;
    u32 otId;        // 0 = the player's own mon; otherwise an outsider (obedience rules apply)
    u8 fateful;      // modern fateful encounter bit (Mew/Deoxys obey only with it)
};

// Actions per turn and battler: 0-3 = move slot, SC_SWITCH(n) = switch to party slot n,
// SC_DEFAULT = move slot 0. For SIM_REQ_SWITCH (faint replacement) the same table is consulted
// for that turn: SC_SWITCH(n) picks n, anything else picks the first legal slot.
#define SC_DEFAULT 0xFF
#define SC_SWITCH(n) (10 + (n))
#define SC_MOVE(n) (n)

struct Scenario
{
    const char *name;
    u32 flags;                       // extra battle type flags (BATTLE_TYPE_DOUBLE); TRAINER is always set
    struct ScenarioMon player[6];
    struct ScenarioMon enemy[6];
    u8 actions[SC_MAX_TURNS][4];     // [turn][battler]
    u8 targets[SC_MAX_TURNS][4];     // [turn][battler] battler id + 1 (0 = default target); doubles only
    u8 turns;                        // stop after this many turns (0 = play to the end, at most SC_MAX_TURNS)
    u16 seed;                        // engine RNG seed (0 = 0x1234)
    u8 badgesSet;                    // 1 = use `badges` instead of all eight
    u8 badges;                       // bit i = badge i+1 (stat boosts, obedience level caps)
    int (*wantSeed)(struct BattleSim *sim);   // optional: try seeds until the sim run satisfies this
    void (*check)(struct BattleSim *sim);     // assertions on the final simulator state
};

struct ScenarioGroup
{
    const char *name;
    const struct Scenario *scenarios;
    int count;
};

#define SCENARIO_GROUP(groupName, array) \
    const struct ScenarioGroup gScenarioGroup_##groupName = { #groupName, array, sizeof(array) / sizeof((array)[0]) };

extern const struct ScenarioGroup *const gScenarioGroups[];
extern const int gScenarioGroupCount;

// --- helpers for writing scenarios ---
#define MON(sp, lv, m1, m2, m3, m4) { .species = sp, .level = lv, .moves = { m1, m2, m3, m4 } }
#define MON_ITEM(sp, lv, m1, m2, m3, m4, it) { .species = sp, .level = lv, .moves = { m1, m2, m3, m4 }, .item = it }
#define MON_AB(sp, lv, m1, m2, m3, m4, ab) { .species = sp, .level = lv, .moves = { m1, m2, m3, m4 }, .abilityNum = ab }
#define T(a, b) { a, b }                    // singles: actions of battler 0 (player) and 1 (enemy) for one turn
#define T4(a, b, c, d) { a, b, c, d }       // doubles

// --- helpers for checks ---
extern int gScenarioFailures;
extern int gScenarioChecks;
extern const char *gScenarioCurrent;
#define CHECK(cond, ...) do { gScenarioChecks++; if (!(cond)) { gScenarioFailures++; printf("  FAIL [%s] %s:%d: ", gScenarioCurrent, __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define B(n) (sim->battleMons[n])
#define HP(n) (sim->battleMons[n].hp)
#define MAXHP(n) (sim->battleMons[n].maxHP)
#define STATUS1(n) (sim->battleMons[n].status1)
#define STATUS2(n) (sim->battleMons[n].status2)
#define STATUS3(n) (sim->statuses3[n])
#define STAGE(n, stat) (sim->battleMons[n].statStages[stat])
#define PP(n, slot) (sim->battleMons[n].pp[slot])
#define PARTY_HP(side, i) ((int)GetMonData(&(side == B_SIDE_PLAYER ? sim->playerParty : sim->enemyParty)[i], MON_DATA_HP))
#define PARTY_STATUS(side, i) (GetMonData(&(side == B_SIDE_PLAYER ? sim->playerParty : sim->enemyParty)[i], MON_DATA_STATUS))
#define WEATHER() (sim->battleWeather)
#define SIDE_STATUS(side) (sim->sideStatuses[side])
#define OUTCOME() (sim->battleOutcome)
// The log covers the whole scenario; `turn` selects one turn (0-based) or SC_ANY_TURN / SC_LAST_TURN.
#define SC_ANY_TURN  -1
#define SC_LAST_TURN -2
int Sc_LogHas(struct BattleSim *sim, u16 stringId, int turn);
int Sc_LogCount(struct BattleSim *sim, u16 stringId, int turn);
int Sc_LogHasMove(struct BattleSim *sim, u16 stringId, u16 move, int turn);   // message with gCurrentMove == move
int Sc_LogIndex(struct BattleSim *sim, u16 stringId, u8 battler, int nth, int turn); // -1 if absent
int Sc_LastTurn(struct BattleSim *sim);
#define LOG_HAS(id) Sc_LogHas(sim, id, SC_ANY_TURN)
#define LOG_HAS_T(id, turn) Sc_LogHas(sim, id, turn)
#define LOG_COUNT(id) Sc_LogCount(sim, id, SC_ANY_TURN)
#define LOG_HAS_MOVE(id, move) Sc_LogHasMove(sim, id, move, SC_ANY_TURN)
#define LOG_HAS_MOVE_T(id, move, turn) Sc_LogHasMove(sim, id, move, turn)
#define LOG_INDEX_T(id, battler, turn) Sc_LogIndex(sim, id, battler, 0, turn)
// "battler a acted before battler b" during `turn` (both must have a USEDMOVE entry)
#define MOVED_BEFORE(a, b, turn) (LOG_INDEX_T(STRINGID_USEDMOVE, a, turn) >= 0 && LOG_INDEX_T(STRINGID_USEDMOVE, b, turn) >= 0 && LOG_INDEX_T(STRINGID_USEDMOVE, a, turn) < LOG_INDEX_T(STRINGID_USEDMOVE, b, turn))

// --- runner API (scenario.c) ---
struct ScenarioRun
{
    const struct Scenario *sc;
    u8 asked[SC_MAX_TURNS + 1][4];   // how many times a battler was asked in a turn (illegal choice => re-ask)
};
void Scenario_Setup(struct BattleSim *sim, const struct Scenario *sc, u16 seed);          // parties + flags + RNG
void Scenario_Decide(struct ScenarioRun *run, struct BattleSim *sim, u8 battler, u8 kind, struct SimAction *out);
int Scenario_RunSim(struct BattleSim *sim, const struct Scenario *sc, u16 seed);          // returns Sim_Run result
u16 Scenario_ResolveSeed(struct BattleSim *sim, const struct Scenario *sc);               // honours wantSeed
int Scenario_ShouldStop(struct BattleSim *sim, const struct Scenario *sc);                // turn limit reached
#define SC_TRAINER_ID 1  // a plain trainer (no league friendship bonus); its team is never used

#endif
