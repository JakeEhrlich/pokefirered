#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include "global.h"
#include "battle.h"
#include "battle_controllers.h"
#include "battle_main.h"
#include "battle_setup.h"
#include "battle_ai_script_commands.h"
#include "battle_util.h"
#include "main.h"
#include "util.h"
#include "item.h"
#include "pokemon.h"
#include "random.h"
#include "constants/abilities.h"
#include "constants/items.h"
#include "constants/moves.h"
#include "constants/species.h"
#include "sim_names.h"
#include "sim_items.h"
#include "battle_scripts.h"
#include "sim_globals.h"

static void SimDebugTrap(const char *where)
{
    if (getenv("SIM_TRAP_VERBOSE"))
    {
        Dl_info info;
        dladdr((void *)gBattleMainFunc, &info);
        fprintf(stderr, "TRAP %s: mainfunc=%s script=%ld active=%d flags=%x typeflags=%x attacker=%d target=%d\n", where,
                info.dli_sname ? info.dli_sname : "?", (long)(gBattlescriptCurrInstr - gBattleScriptBlob), gActiveBattler,
                gBattleControllerExecFlags, gBattleTypeFlags, gBattlerAttacker, gBattlerTarget);
    }
    // Not fatal: the battle is abandoned and Sim_Run reports SIM_RUN_ERROR.
    gSim->error = SIM_ERR_TRAP;
    gSim->finished = TRUE;
    gBattleOutcome = B_OUTCOME_DREW;
}

void HandleTurnActionSelectionState(void);
u8 CreateNPCTrainerParty(struct Pokemon *party, u16 trainerNum);

_Thread_local struct BattleSim *gSim;

void Sim_Bind(struct BattleSim *sim)
{
    gSim = sim;
}

void Sim_Init(struct BattleSim *sim, u32 battleTypeFlags, u16 seed)
{
    memset(sim, 0, sizeof(*sim));
    Sim_Bind(sim);
    gBattleTypeFlags = battleTypeFlags;
    SeedRng(seed);
    sim->badgeFlags = 0xFF;
    sim->maxTurns = 500;
}

u8 Sim_LoadTrainerParty(struct BattleSim *sim, u16 trainerNum)
{
    Sim_Bind(sim);
    gTrainerBattleOpponent_A = trainerNum;
    gBattleTypeFlags |= BATTLE_TYPE_TRAINER;
    if (gTrainers[trainerNum].doubleBattle)
        gBattleTypeFlags |= BATTLE_TYPE_DOUBLE;
    sim->createTrainerParty = TRUE; // Sim_Start re-creates it at the same point the game does
    return CreateNPCTrainerParty(gEnemyParty, trainerNum);
}

void Sim_SetPolicy(struct BattleSim *sim, u8 side, SimPolicyFunc policy)
{
    sim->policy[side] = policy;
}

struct Pokemon *Sim_Party(struct BattleSim *sim, u8 side)
{
    return side == B_SIDE_PLAYER ? sim->playerParty : sim->enemyParty;
}

void SimLog(u16 stringId, u8 battler)
{
    if (!gSim->logEnabled || gSim->logCount >= SIM_LOG_MAX)
        return;
    gSim->log[gSim->logCount].stringId = stringId;
    gSim->log[gSim->logCount].battler = battler;
    gSim->log[gSim->logCount].multistring = gBattleCommunication[MULTISTRING_CHOOSER];
    gSim->log[gSim->logCount].move = gCurrentMove;
    gSim->log[gSim->logCount].hpTarget = gBattlerTarget < MAX_BATTLERS_COUNT ? gBattleMons[gBattlerTarget].hp : 0;
    gSim->log[gSim->logCount].dmg = gBattleMoveDamage;
    gSim->log[gSim->logCount].turn = gSim->turnCount;
    gSim->logCount++;
}

static void CountParties(void)
{
    s32 i;
    gPlayerPartyCount = 0;
    gEnemyPartyCount = 0;
    for (i = 0; i < PARTY_SIZE; i++)
        if (GetMonData(&gPlayerParty[i], MON_DATA_SPECIES) != SPECIES_NONE)
            gPlayerPartyCount = i + 1;
    for (i = 0; i < PARTY_SIZE; i++)
        if (GetMonData(&gEnemyParty[i], MON_DATA_SPECIES) != SPECIES_NONE)
            gEnemyPartyCount = i + 1;
}

// Mirrors the non-link path of CB2_InitBattleInternal / CB2_HandleStartBattle.
static int CountUsableMons(struct Pokemon *party)
{
    int i, n = 0;
    for (i = 0; i < PARTY_SIZE; i++)
        if (GetMonData(&party[i], MON_DATA_SPECIES_OR_EGG) != SPECIES_NONE
         && GetMonData(&party[i], MON_DATA_SPECIES_OR_EGG) != SPECIES_EGG
         && GetMonData(&party[i], MON_DATA_HP) != 0)
            n++;
    return n;
}

// Every mon must hold ids the engine's tables can index; anything else would read out of bounds.
static bool8 PartyEncodable(struct Pokemon *party)
{
    s32 i, m;
    bool8 gap = FALSE;
    for (i = 0; i < PARTY_SIZE; i++)
    {
        u16 species = GetMonData(&party[i], MON_DATA_SPECIES);
        u8 level;
        if (species == SPECIES_NONE)
        {
            gap = TRUE;
            continue;
        }
        if (gap)
            return FALSE; // parties are contiguous from slot 0 in the game; the engine indexes by position
        if (GetMonData(&party[i], MON_DATA_SANITY_IS_BAD_EGG))
            return FALSE; // a corrupted mon (checksum mismatch) reads as species 412 and the game itself misbehaves
        if (GetMonData(&party[i], MON_DATA_IS_EGG))
            continue; // eggs never battle; the engine skips them
        if (species >= SPECIES_EGG) // 1..411 are species; the UNOWN_B.. aliases are never stored
            return FALSE;
        level = GetMonData(&party[i], MON_DATA_LEVEL);
        if (level < 1 || level > MAX_LEVEL)
            return FALSE;
        if (GetMonData(&party[i], MON_DATA_HELD_ITEM) >= ITEMS_COUNT)
            return FALSE;
        for (m = 0; m < MAX_MON_MOVES; m++)
            if (GetMonData(&party[i], MON_DATA_MOVE1 + m) >= MOVES_COUNT)
                return FALSE;
        if (GetMonData(&party[i], MON_DATA_HP) > GetMonData(&party[i], MON_DATA_MAX_HP))
            return FALSE;
    }
    return TRUE;
}

int Sim_Start(struct BattleSim *sim)
{
    s32 i;

    Sim_Bind(sim);
    if (gBattleTypeFlags & ~(BATTLE_TYPE_TRAINER | BATTLE_TYPE_DOUBLE | BATTLE_TYPE_IS_MASTER))
        return -3;
    if (!PartyEncodable(gPlayerParty) || !PartyEncodable(gEnemyParty))
        return -2;
    CountParties();
    if (CountUsableMons(gPlayerParty) == 0 || CountUsableMons(gEnemyParty) == 0)
        return -1;
    if ((gBattleTypeFlags & BATTLE_TYPE_DOUBLE) && (CountUsableMons(gPlayerParty) < 2 || CountUsableMons(gEnemyParty) < 2))
        return -1; // the game never starts a double battle with a single usable mon on a side
    gBattleTerrain = BATTLE_TERRAIN_GRASS;
    SetUpBattleVars();
    if (sim->createTrainerParty) // same point as CB2_InitBattleInternal, so RNG is consumed in the game's order
        CreateNPCTrainerParty(gEnemyParty, gTrainerBattleOpponent_A);
    SetWildMonHeldItem();
    gMain.inBattle = TRUE;
    for (i = 0; i < PARTY_SIZE; i++)
        AdjustFriendship(&gPlayerParty[i], FRIENDSHIP_EVENT_LEAGUE_BATTLE);
    gBattleTypeFlags |= BATTLE_TYPE_IS_MASTER;
    SetAllPlayersBerryData();
    InitBattleControllers();
    for (i = 0; i < MAX_BATTLERS_COUNT; i++)
    {
        gActiveBattler = i;
        SimSetControllerToSim();
    }
    gActiveBattler = 0;
    gBattleCommunication[MULTIUSE_STATE] = 0;
    return 0;
}

int Sim_Run(struct BattleSim *sim)
{
    u32 budget = 1000000;

    Sim_Bind(sim);
    sim->requestKind = SIM_REQ_NONE;
    if (sim->error)
        return SIM_RUN_ERROR;
    while (!sim->finished)
    {
        gBattleMainFunc();
        if (gBattleControllerExecFlags & 0xF0000000) { SimDebugTrap("main"); return SIM_RUN_ERROR; }
        for (gActiveBattler = 0; gActiveBattler < gBattlersCount; gActiveBattler++)
        {
            gBattlerControllerFuncs[gActiveBattler]();
            if (gBattleControllerExecFlags & 0xF0000000) { SimDebugTrap("controller"); return SIM_RUN_ERROR; }
            if (sim->error) return SIM_RUN_ERROR;
        }
        if (sim->error) return SIM_RUN_ERROR;
        sim->frames++;
        if (sim->requestKind != SIM_REQ_NONE)
        {
            // The game keeps running while the player sits in the menu: other battlers' controllers (the AI)
            // finish their decisions meanwhile. Step frames until nothing changes any more.
            static _Thread_local struct BattleSim before;
            u32 extra;
            for (extra = 0; extra < 100000; extra++)
            {
                u32 f = sim->frames;
                memcpy(&before, sim, sizeof(before));
                gBattleMainFunc();
                for (gActiveBattler = 0; gActiveBattler < gBattlersCount; gActiveBattler++)
                    gBattlerControllerFuncs[gActiveBattler]();
                sim->frames = f;
                if (memcmp(&before, sim, sizeof(before)) == 0)
                    break;
                sim->frames = f + 1;
            }
            return SIM_RUN_REQUEST;
        }
        if (sim->maxTurns != 0 && sim->turnCount >= sim->maxTurns && gBattleMainFunc == HandleTurnActionSelectionState)
        {
            gBattleOutcome = B_OUTCOME_DREW;
            sim->finished = TRUE;
        }
        if (--budget == 0)
        {
            sim->error = SIM_ERR_STUCK;
            sim->finished = TRUE;
            gBattleOutcome = B_OUTCOME_DREW;
            return SIM_RUN_STUCK;
        }
    }
    return sim->error ? SIM_RUN_ERROR : SIM_RUN_FINISHED;
}

int Sim_ValidateAction(struct BattleSim *sim, u8 battler, u8 requestKind, const struct SimAction *a)
{
    struct SimAction acts[64];
    u8 slots[PARTY_SIZE];
    int n, i;

    Sim_Bind(sim);
    if (battler >= gBattlersCount)
        return 0;
    if (requestKind == SIM_REQ_SWITCH)
    {
        if (a->type != B_ACTION_SWITCH)
            return 0;
        n = Sim_LegalSwitches(sim, battler, slots, PARTY_SIZE);
        for (i = 0; i < n; i++)
            if (slots[i] == a->partySlot)
                return 1;
        return 0;
    }
    if (requestKind != SIM_REQ_ACTION)
        return 0;
    if (a->type == B_ACTION_RUN)
        return 0; // only trainer battles are simulated and running from a trainer always fails
    if (a->type == B_ACTION_USE_ITEM)
        return a->item != ITEM_NONE && a->item < ITEMS_COUNT && gSimItems[a->item].name != NULL && gSimItems[a->item].battleUsage != 0
            && (a->partySlot < PARTY_SIZE || a->partySlot == 0xFF);
    n = Sim_LegalActions(sim, battler, acts, 64);
    for (i = 0; i < n; i++)
    {
        if (acts[i].type != a->type)
            continue;
        if (a->type == B_ACTION_USE_MOVE)
        {
            if (acts[i].moveSlot != (a->moveSlot & 3) && !(gBattleMons[battler].status2 & (STATUS2_MULTIPLETURNS | STATUS2_RECHARGE)))
                continue;
            if (acts[i].target != 0xFF && acts[i].target != a->target && a->target != 0xFF)
                continue;
            return 1;
        }
        if (a->type == B_ACTION_SWITCH && acts[i].partySlot == a->partySlot)
            return 1;
    }
    return 0;
}

int Sim_Answer(struct BattleSim *sim, u8 battler, const struct SimAction *action)
{
    if (sim->requestKind == SIM_REQ_NONE || sim->requestBattler != battler)
        return -1;
    if (sim->strictAnswers && !Sim_ValidateAction(sim, battler, sim->requestKind, action))
    {
        sim->rejectedAnswers++;
        return -1;
    }
    sim->answer[battler] = *action;
    sim->answerValid[battler] = TRUE;
    sim->answerKind[battler] = sim->requestKind;
    sim->requestKind = SIM_REQ_NONE;
    return 0;
}

// ---- legality ----

int Sim_LegalSwitches(struct BattleSim *sim, u8 battler, u8 *outSlots, int maxOut)
{
    struct Pokemon *party = Sim_Party(sim, GetBattlerSide(battler));
    u8 partner = BATTLE_PARTNER(battler);
    int n = 0;
    s32 i;

    for (i = 0; i < PARTY_SIZE && n < maxOut; i++)
    {
        if (GetMonData(&party[i], MON_DATA_SPECIES_OR_EGG) == SPECIES_NONE
         || GetMonData(&party[i], MON_DATA_SPECIES_OR_EGG) == SPECIES_EGG
         || GetMonData(&party[i], MON_DATA_HP) == 0)
            continue;
        if (i == gBattlerPartyIndexes[battler])
            continue;
        if ((gBattleTypeFlags & BATTLE_TYPE_DOUBLE) && i == gBattlerPartyIndexes[partner])
            continue;
        if ((gBattleTypeFlags & BATTLE_TYPE_DOUBLE) && i == *(gBattleStruct->monToSwitchIntoId + partner))
            continue;
        outSlots[n++] = i;
    }
    return n;
}

static bool8 IsTrapped(u8 battler)
{
    s32 i;

    if (gBattleMons[battler].status2 & (STATUS2_WRAPPED | STATUS2_ESCAPE_PREVENTION))
        return TRUE;
    if (gStatuses3[battler] & STATUS3_ROOTED)
        return TRUE;
    if (ABILITY_ON_OPPOSING_FIELD(battler, ABILITY_SHADOW_TAG))
        return TRUE;
    if (ABILITY_ON_OPPOSING_FIELD(battler, ABILITY_ARENA_TRAP)
     && !IS_BATTLER_OF_TYPE(battler, TYPE_FLYING)
     && gBattleMons[battler].ability != ABILITY_LEVITATE)
        return TRUE;
    i = AbilityBattleEffects(ABILITYEFFECT_CHECK_FIELD_EXCEPT_BATTLER, battler, ABILITY_MAGNET_PULL, 0, 0);
    if (i != 0 && IS_BATTLER_OF_TYPE(battler, TYPE_STEEL))
        return TRUE;
    return FALSE;
}

int Sim_LegalActions(struct BattleSim *sim, u8 battler, struct SimAction *out, int maxOut)
{
    int n = 0;
    s32 i, t;
    u8 unusable;
    u8 slots[PARTY_SIZE];
    int nSwitch;

    Sim_Bind(sim);
    // Locked into a move (Thrash, Rollout, recharge, ...): the engine ignores the choice.
    if (gBattleMons[battler].status2 & (STATUS2_MULTIPLETURNS | STATUS2_RECHARGE))
    {
        if (n < maxOut)
        {
            out[n].type = B_ACTION_USE_MOVE; out[n].moveSlot = 0; out[n].target = 0xFF; n++;
        }
        return n;
    }
    unusable = CheckMoveLimitations(battler, 0, MOVE_LIMITATIONS_ALL);
    if (gDisableStructs[battler].encoredMove != MOVE_NONE)
        unusable = (u8)~gBitTable[gDisableStructs[battler].encoredMovePos];
    if (unusable == 0xF)
    {
        // Struggle
        if (n < maxOut)
        {
            out[n].type = B_ACTION_USE_MOVE; out[n].moveSlot = 0; out[n].target = 0xFF; n++;
        }
    }
    else
    {
        for (i = 0; i < MAX_MON_MOVES; i++)
        {
            u16 move = gBattleMons[battler].moves[i];
            if (unusable & gBitTable[i])
                continue;
            if (!(gBattleTypeFlags & BATTLE_TYPE_DOUBLE)
             || (gBattleMoves[move].target & (MOVE_TARGET_USER | MOVE_TARGET_BOTH | MOVE_TARGET_FOES_AND_ALLY | MOVE_TARGET_OPPONENTS_FIELD | MOVE_TARGET_RANDOM | MOVE_TARGET_DEPENDS)))
            {
                if (n < maxOut)
                {
                    out[n].type = B_ACTION_USE_MOVE; out[n].moveSlot = i; out[n].target = 0xFF; n++;
                }
            }
            else
            {
                for (t = 0; t < gBattlersCount; t++)
                {
                    if (t == battler && !(gBattleMoves[move].target & MOVE_TARGET_USER_OR_SELECTED))
                        continue;
                    if (gAbsentBattlerFlags & gBitTable[t])
                        continue;
                    if (n < maxOut)
                    {
                        out[n].type = B_ACTION_USE_MOVE; out[n].moveSlot = i; out[n].target = t; n++;
                    }
                }
            }
        }
    }
    if (!IsTrapped(battler))
    {
        nSwitch = Sim_LegalSwitches(sim, battler, slots, PARTY_SIZE);
        for (i = 0; i < nSwitch && n < maxOut; i++)
        {
            out[n].type = B_ACTION_SWITCH; out[n].partySlot = slots[i]; n++;
        }
    }
    return n;
}

void Sim_RandomPolicy(struct BattleSim *sim, u8 battler, u8 requestKind, struct SimAction *out)
{
    struct SimAction acts[32];
    u8 slots[PARTY_SIZE];
    int n;

    memset(out, 0, sizeof(*out));
    if (requestKind == SIM_REQ_SWITCH)
    {
        n = Sim_LegalSwitches(sim, battler, slots, PARTY_SIZE);
        out->type = B_ACTION_SWITCH;
        out->partySlot = n ? slots[Random() % n] : 0;
        return;
    }
    n = Sim_LegalActions(sim, battler, acts, 32);
    if (n == 0)
    {
        out->type = B_ACTION_USE_MOVE; out->moveSlot = 0; out->target = 0xFF;
        return;
    }
    *out = acts[Random() % n];
}

// ---- party helpers ----

int Sim_MakeMonEx(struct Pokemon *mon, u16 species, u8 level, u8 nature, const u8 *ivs, const u8 *evs,
                  const u16 *moves, u16 item, u8 abilityNum, u32 otId, u8 fatefulEncounter)
{
    // Reject anything the engine's tables cannot index: a mon is never half-built from bad ids.
    memset(mon, 0, sizeof(*mon));
    if (species == SPECIES_NONE || species >= SPECIES_EGG || level < 1 || level > MAX_LEVEL || nature >= NUM_NATURES
     || item >= ITEMS_COUNT || abilityNum > 1)
        return -1;
    if (moves)
    {
        int k;
        for (k = 0; k < MAX_MON_MOVES; k++)
            if (moves[k] >= MOVES_COUNT)
                return -1;
    }
    s32 i;
    u32 personality;
    u8 iv31 = 31;

    // personality: choose one giving the requested nature and ability bit
    do
        personality = Random32();
    while (personality % NUM_NATURES != nature || (personality & 1) != (abilityNum & 1));
    if (otId == 0)
        CreateMon(mon, species, level, 0, TRUE, personality, OT_ID_PLAYER_ID, 0); // player OT so the obedience rule treats it as the player's own mon
    else
        CreateMon(mon, species, level, 0, TRUE, personality, OT_ID_PRESET, otId);
    for (i = 0; i < NUM_STATS; i++)
    {
        u8 iv = ivs ? ivs[i] : iv31;
        u8 ev = evs ? evs[i] : 0;
        SetMonData(mon, MON_DATA_HP_IV + i, &iv);
        SetMonData(mon, MON_DATA_HP_EV + i, &ev);
    }
    if (moves)
    {
        for (i = 0; i < MAX_MON_MOVES; i++)
        {
            u16 move = moves[i];
            u8 pp = gBattleMoves[move].pp;
            SetMonData(mon, MON_DATA_MOVE1 + i, &move);
            SetMonData(mon, MON_DATA_PP1 + i, &pp);
        }
    }
    SetMonData(mon, MON_DATA_HELD_ITEM, &item);
    if (fatefulEncounter)
    {
        u8 one = 1;
        SetMonData(mon, MON_DATA_MODERN_FATEFUL_ENCOUNTER, &one);
    }
    CalculateMonStats(mon);
    return 0;
}

void Sim_MakeMon(struct Pokemon *mon, u16 species, u8 level, u8 nature, const u8 *ivs, const u8 *evs,
                 const u16 *moves, u16 item, u8 abilityNum)
{
    Sim_MakeMonEx(mon, species, level, nature, ivs, evs, moves, item, abilityNum, 0, 0);
}

void Sim_PrintBattlers(struct BattleSim *sim)
{
    int i;
    for (i = 0; i < sim->battlersCount; i++)
    {
        struct BattlePokemon *m = &sim->battleMons[i];
        printf("  [%d] %-10s L%-3d hp %3d/%-3d st1 %05x st2 %08x st3 %05x ability %s item %s\n", i,
               gSimSpeciesNames[m->species], m->level, m->hp, m->maxHP, m->status1, m->status2, sim->statuses3[i],
               gSimAbilityNames[m->ability], gSimItemNames[m->item]);
    }
}

void Sim_PrintLog(struct BattleSim *sim)
{
    int i;
    for (i = 0; i < sim->logCount; i++)
        printf("  t%-2d %-3d %-28s (battler %d, ms %d) move %-14s dmg %-5d targetHp %d\n", sim->log[i].turn, sim->log[i].stringId,
               sim->log[i].stringId < gSimStringIdNames_Count ? gSimStringIdNames[sim->log[i].stringId] : "?",
               sim->log[i].battler, sim->log[i].multistring, gSimMoveNames[sim->log[i].move], sim->log[i].dmg, sim->log[i].hpTarget);
}
