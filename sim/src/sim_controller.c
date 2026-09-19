// Synchronous battle controller for the simulator. Replaces the player/opponent controllers:
// mon-data transfers happen immediately, UI commands are no-ops, and decisions either come from a
// per-side policy callback or suspend the simulation (Sim_Run returns SIM_RUN_REQUEST).
#include <stdio.h>
#include <stdlib.h>
#include "global.h"
#include "battle.h"
#include "battle_controllers.h"
#include "battle_message.h"
#include "battle_interface.h"
#include "battle_ai_script_commands.h"
#include "battle_ai_switch_items.h"
#include "item.h"
#include "pokemon.h"
#include "random.h"
#include "string_util.h"
#include "util.h"
#include "constants/battle_anim.h"
#include "constants/items.h"
#include "constants/moves.h"
#include "constants/party_menu.h"
#include "constants/songs.h"
#include "sim_globals.h"

static void SimBufferRunCommand(void);
static void SimBufferExecCompleted(void);
static u32 GetSimMonData(struct Pokemon *party, u8 monId, u8 *dst);
static void SetSimMonData(struct Pokemon *party, u8 monId);

static struct Pokemon *PartyOf(u8 battler)
{
    return GetBattlerSide(battler) == B_SIDE_PLAYER ? gPlayerParty : gEnemyParty;
}

void SimSetControllerToSim(void)
{
    gBattlerControllerFuncs[gActiveBattler] = SimBufferRunCommand;
}

static void SimBufferExecCompleted(void)
{
    gBattlerControllerFuncs[gActiveBattler] = SimBufferRunCommand;
    gBattleControllerExecFlags &= ~gBitTable[gActiveBattler];
}

static void SimHandleGetMonData(void)
{
    u8 monData[sizeof(struct Pokemon) * 2 + 56];
    u32 size = 0;
    u8 monToCheck;
    s32 i;
    struct Pokemon *party = PartyOf(gActiveBattler);

    if (!gBattleBufferA[gActiveBattler][2])
    {
        size += GetSimMonData(party, gBattlerPartyIndexes[gActiveBattler], monData);
    }
    else
    {
        monToCheck = gBattleBufferA[gActiveBattler][2];
        for (i = 0; i < PARTY_SIZE; ++i)
        {
            if (monToCheck & 1)
                size += GetSimMonData(party, i, monData + size);
            monToCheck >>= 1;
        }
    }
    BtlController_EmitDataTransfer(BUFFER_B, size, monData);
    SimBufferExecCompleted();
}

static void SimHandleSetMonData(void)
{
    u8 monToCheck;
    u8 i;
    struct Pokemon *party = PartyOf(gActiveBattler);

    if (gBattleBufferA[gActiveBattler][2] == 0)
    {
        SetSimMonData(party, gBattlerPartyIndexes[gActiveBattler]);
    }
    else
    {
        monToCheck = gBattleBufferA[gActiveBattler][2];
        for (i = 0; i < PARTY_SIZE; ++i)
        {
            if (monToCheck & 1)
                SetSimMonData(party, i);
            monToCheck >>= 1;
        }
    }
    SimBufferExecCompleted();
}

static void SimHandleGetRawMonData(void)
{
    struct BattlePokemon battleMon;
    u8 *src = (u8 *)&PartyOf(gActiveBattler)[gBattlerPartyIndexes[gActiveBattler]] + gBattleBufferA[gActiveBattler][1];
    u8 *dst = (u8 *)&battleMon + gBattleBufferA[gActiveBattler][1];
    u8 i;

    for (i = 0; i < gBattleBufferA[gActiveBattler][2]; ++i)
        dst[i] = src[i];
    BtlController_EmitDataTransfer(BUFFER_B, gBattleBufferA[gActiveBattler][2], dst);
    SimBufferExecCompleted();
}

static void SimHandleSetRawMonData(void)
{
    u8 *dst = (u8 *)&PartyOf(gActiveBattler)[gBattlerPartyIndexes[gActiveBattler]] + gBattleBufferA[gActiveBattler][1];
    u8 i;

    for (i = 0; i < gBattleBufferA[gActiveBattler][2]; ++i)
        dst[i] = gBattleBufferA[gActiveBattler][3 + i];
    SimBufferExecCompleted();
}

static void SimHandleSwitchInAnim(void)
{
    *(gBattleStruct->monToSwitchIntoId + gActiveBattler) = PARTY_SIZE;
    gBattlerPartyIndexes[gActiveBattler] = gBattleBufferA[gActiveBattler][1];
    SimBufferExecCompleted();
}

// ---- decisions ----

static bool8 ObtainAnswer(u8 kind)
{
    u8 side = GetBattlerSide(gActiveBattler);

    // A stale answer from an earlier request (e.g. a move choice the engine never consumed because the
    // battler was encored / locked) must not be reused for a different kind of request.
    if (gSim->answerValid[gActiveBattler] && gSim->answerKind[gActiveBattler] == kind)
        return TRUE;
    gSim->answerValid[gActiveBattler] = FALSE;
    if (gSim->policy[side] != NULL)
    {
        gSim->policy[side](gSim, gActiveBattler, kind, &gSim->answer[gActiveBattler]);
        gSim->answerValid[gActiveBattler] = TRUE;
        gSim->answerKind[gActiveBattler] = kind;
        return TRUE;
    }
    // One outstanding request at a time (like the harness mailbox): a second battler waits its turn.
    if (gSim->requestKind != SIM_REQ_NONE && gSim->requestBattler != gActiveBattler)
        return FALSE;
    gSim->requestKind = kind;
    gSim->requestBattler = gActiveBattler;
    return FALSE;
}

static bool8 UsesVanillaAI(void)
{
    return gSim->policy[GetBattlerSide(gActiveBattler)] == Sim_VanillaAIPolicy;
}

static void SimHandleChooseAction(void)
{
    struct SimAction *a = &gSim->answer[gActiveBattler];

    if (UsesVanillaAI())
    {
        AI_TrySwitchOrUseItem(); // emits the chosen action itself (OpponentHandleChooseAction)
        SimBufferExecCompleted();
        return;
    }
    if (!ObtainAnswer(SIM_REQ_ACTION))
        return;
    if (a->type == B_ACTION_USE_ITEM && a->item == ITEM_NONE)
        a->type = B_ACTION_USE_MOVE;
    BtlController_EmitTwoReturnValues(BUFFER_B, a->type, 0);
    SimBufferExecCompleted();
}

static u8 DefaultMoveTarget(u16 move)
{
    u8 target;

    if (gBattleMoves[move].target & (MOVE_TARGET_USER | MOVE_TARGET_USER_OR_SELECTED))
        return gActiveBattler;
    target = BATTLE_OPPOSITE(gActiveBattler);
    if (gAbsentBattlerFlags & gBitTable[target])
        target = BATTLE_PARTNER(target);
    return target;
}

// Ported from OpponentHandleChooseMove (trainer branch).
static void VanillaAIChooseMove(void)
{
    u8 chosenMoveId;
    struct ChooseMoveStruct *moveInfo = (struct ChooseMoveStruct *)(&gBattleBufferA[gActiveBattler][4]);

    BattleAI_SetupAIData();
    chosenMoveId = BattleAI_ChooseMoveOrAction();
    switch (chosenMoveId)
    {
    case AI_CHOICE_WATCH:
        BtlController_EmitTwoReturnValues(1, B_ACTION_SAFARI_WATCH_CAREFULLY, 0);
        break;
    case AI_CHOICE_FLEE:
        BtlController_EmitTwoReturnValues(1, B_ACTION_RUN, 0);
        break;
    default:
        if (gBattleMoves[moveInfo->moves[chosenMoveId]].target & (MOVE_TARGET_USER_OR_SELECTED | MOVE_TARGET_USER))
            gBattlerTarget = gActiveBattler;
        if (gBattleMoves[moveInfo->moves[chosenMoveId]].target & MOVE_TARGET_BOTH)
        {
            gBattlerTarget = GetBattlerAtPosition(B_POSITION_PLAYER_LEFT);
            if (gAbsentBattlerFlags & gBitTable[gBattlerTarget])
                gBattlerTarget = GetBattlerAtPosition(B_POSITION_PLAYER_RIGHT);
        }
        BtlController_EmitTwoReturnValues(1, 10, (chosenMoveId) | (gBattlerTarget << 8));
        break;
    }
    SimBufferExecCompleted();
}

// Ported from OpponentHandleChoosePokemon.
static void VanillaAIChoosePokemon(void)
{
    s32 chosenMonId;

    if (*(gBattleStruct->AI_monToSwitchIntoId + (GetBattlerPosition(gActiveBattler) >> 1)) == PARTY_SIZE)
    {
        chosenMonId = GetMostSuitableMonToSwitchInto();
        if (chosenMonId == PARTY_SIZE)
        {
            s32 battler1, battler2;

            if (!(gBattleTypeFlags & BATTLE_TYPE_DOUBLE))
            {
                battler2 = battler1 = GetBattlerAtPosition(B_POSITION_OPPONENT_LEFT);
            }
            else
            {
                battler1 = GetBattlerAtPosition(B_POSITION_OPPONENT_LEFT);
                battler2 = GetBattlerAtPosition(B_POSITION_OPPONENT_RIGHT);
            }
            for (chosenMonId = 0; chosenMonId < PARTY_SIZE; ++chosenMonId)
                if (GetMonData(&gEnemyParty[chosenMonId], MON_DATA_HP) != 0
                 && chosenMonId != gBattlerPartyIndexes[battler1]
                 && chosenMonId != gBattlerPartyIndexes[battler2])
                    break;
        }
    }
    else
    {
        chosenMonId = *(gBattleStruct->AI_monToSwitchIntoId + (GetBattlerPosition(gActiveBattler) >> 1));
        *(gBattleStruct->AI_monToSwitchIntoId + (GetBattlerPosition(gActiveBattler) >> 1)) = PARTY_SIZE;
    }
    *(gBattleStruct->monToSwitchIntoId + gActiveBattler) = chosenMonId;
    BtlController_EmitChosenMonReturnValue(1, chosenMonId, NULL);
    SimBufferExecCompleted();
}

void Sim_VanillaAIPolicy(struct BattleSim *sim, u8 battler, u8 requestKind, struct SimAction *out)
{
    // Sentinel: the controller dispatches to the game's AI directly (see UsesVanillaAI()).
    Sim_RandomPolicy(sim, battler, requestKind, out);
}

static void SimHandleChooseMove(void)
{
    struct SimAction *a = &gSim->answer[gActiveBattler];
    struct ChooseMoveStruct *moveInfo = (struct ChooseMoveStruct *)(&gBattleBufferA[gActiveBattler][4]);
    u8 target;

    if (UsesVanillaAI())
    {
        VanillaAIChooseMove();
        return;
    }
    if (!ObtainAnswer(SIM_REQ_ACTION))
        return;
    gSim->answerValid[gActiveBattler] = FALSE;
    if (a->type != B_ACTION_USE_MOVE)
    {
        // The policy changed its mind (e.g. re-asked after an illegal move); go back to the action menu.
        BtlController_EmitTwoReturnValues(BUFFER_B, 10, 0xFFFF);
        SimBufferExecCompleted();
        return;
    }
    target = a->target;
    if (target >= gBattlersCount || (gAbsentBattlerFlags & gBitTable[target]) || !(gBattleTypeFlags & BATTLE_TYPE_DOUBLE))
        target = DefaultMoveTarget(moveInfo->moves[a->moveSlot & 3]);
    BtlController_EmitTwoReturnValues(BUFFER_B, 10, (a->moveSlot & 3) | (target << 8));
    SimBufferExecCompleted();
}

static void SimHandleChooseItem(void)
{
    struct SimAction *a = &gSim->answer[gActiveBattler];
    u16 item;

    if (UsesVanillaAI())
    {
        // OpponentHandleChooseItem: the AI already picked the item in ShouldUseItem().
        BtlController_EmitOneReturnValue(BUFFER_B, *(gBattleStruct->chosenItem + (gActiveBattler / 2) * 2));
        SimBufferExecCompleted();
        return;
    }
    if (!ObtainAnswer(SIM_REQ_ACTION))
        return;
    gSim->answerValid[gActiveBattler] = FALSE;
    item = (a->type == B_ACTION_USE_ITEM) ? a->item : ITEM_NONE;
    if (item != ITEM_NONE && GetBattlerSide(gActiveBattler) == B_SIDE_PLAYER)
    {
        // The bag menu applies medicine immediately at selection time; mirror that.
        u8 slot = a->partySlot < PARTY_SIZE ? a->partySlot : gBattlerPartyIndexes[gActiveBattler];
        if (item > ITEM_PREMIER_BALL)
            PokemonUseItemEffects(&PartyOf(gActiveBattler)[slot], item, slot, a->moveSlot, FALSE);
    }
    gSpecialVar_ItemId = item;
    BtlController_EmitOneReturnValue(BUFFER_B, item);
    SimBufferExecCompleted();
}

static void SimHandleChoosePokemon(void)
{
    struct SimAction *a = &gSim->answer[gActiveBattler];
    u8 caseId = gBattleBufferA[gActiveBattler][1] & 0xF;
    u8 chosen = PARTY_SIZE;
    s32 i;

    for (i = 0; i < 3; ++i)
        gBattlePartyCurrentOrder[i] = gBattleBufferA[gActiveBattler][4 + i];

    if (UsesVanillaAI())
    {
        VanillaAIChoosePokemon();
        return;
    }
    switch (caseId)
    {
    case PARTY_ACTION_CANT_SWITCH:
    case PARTY_ACTION_ABILITY_PREVENTS:
        // Player would see a message and return to the action menu.
        gSim->answerValid[gActiveBattler] = FALSE;
        chosen = PARTY_SIZE;
        break;
    case PARTY_ACTION_CHOOSE_MON:
        if (gSim->answerValid[gActiveBattler] && a->type == B_ACTION_SWITCH)
        {
            chosen = a->partySlot;
            gSim->answerValid[gActiveBattler] = FALSE;
            break;
        }
        // fallthrough: an unexpected mid-turn request
    default:
        if (!ObtainAnswer(SIM_REQ_SWITCH))
            return;
        chosen = a->partySlot;
        gSim->answerValid[gActiveBattler] = FALSE;
        break;
    }
    if (chosen != PARTY_SIZE)
    {
        struct Pokemon *party = PartyOf(gActiveBattler);
        if (chosen > PARTY_SIZE || GetMonData(&party[chosen], MON_DATA_HP) == 0 || GetMonData(&party[chosen], MON_DATA_SPECIES) == SPECIES_NONE)
        {
            fprintf(stderr, "SIM: policy chose invalid party slot %d for battler %d (caseId %d, answerValid %d type %d)\n",
                    chosen, gActiveBattler, caseId, gSim->answerValid[gActiveBattler], a->type);
            for (i = 0; i < PARTY_SIZE; i++)
                fprintf(stderr, "  party[%d]: species %d hp %d/%d\n", (int)i, GetMonData(&party[i], MON_DATA_SPECIES), GetMonData(&party[i], MON_DATA_HP), GetMonData(&party[i], MON_DATA_MAX_HP));
            fprintf(stderr, "  absent %x outcome %d partyIndexes %d %d %d %d double %d turn %d\n", gAbsentBattlerFlags, gBattleOutcome,
                    gBattlerPartyIndexes[0], gBattlerPartyIndexes[1], gBattlerPartyIndexes[2], gBattlerPartyIndexes[3], (gBattleTypeFlags & BATTLE_TYPE_DOUBLE) != 0, gSim->turnCount);
            Sim_PrintLog(gSim);
            abort();
        }
        *(gBattleStruct->monToSwitchIntoId + gActiveBattler) = chosen;
    }
    BtlController_EmitChosenMonReturnValue(BUFFER_B, chosen, gBattlePartyCurrentOrder);
    SimBufferExecCompleted();
}

static void SimHandlePrintString(void)
{
    u16 stringId = gBattleBufferA[gActiveBattler][2] | (gBattleBufferA[gActiveBattler][3] << 8);
    SimLog(stringId, gActiveBattler);
    SimBufferExecCompleted();
}

static void SimHandleExpUpdate(void)
{
    // Experience is not simulated (would require level-up / move-learning UI flow).
    BtlController_EmitTwoReturnValues(BUFFER_B, 0, 0);
    SimBufferExecCompleted();
}

static void SimHandleNop(void)
{
    SimBufferExecCompleted();
}

static void SimBufferRunCommand(void)
{
    if (!(gBattleControllerExecFlags & gBitTable[gActiveBattler]))
        return;
    switch (gBattleBufferA[gActiveBattler][0])
    {
    case CONTROLLER_GETMONDATA:     SimHandleGetMonData(); break;
    case CONTROLLER_GETRAWMONDATA:  SimHandleGetRawMonData(); break;
    case CONTROLLER_SETMONDATA:     SimHandleSetMonData(); break;
    case CONTROLLER_SETRAWMONDATA:  SimHandleSetRawMonData(); break;
    case CONTROLLER_SWITCHINANIM:   SimHandleSwitchInAnim(); break;
    case CONTROLLER_PRINTSTRING:
    case CONTROLLER_PRINTSTRINGPLAYERONLY: SimHandlePrintString(); break;
    case CONTROLLER_CHOOSEACTION:   SimHandleChooseAction(); break;
    case CONTROLLER_CHOOSEMOVE:     SimHandleChooseMove(); break;
    case CONTROLLER_OPENBAG:        SimHandleChooseItem(); break;
    case CONTROLLER_CHOOSEPOKEMON:  SimHandleChoosePokemon(); break;
    case CONTROLLER_EXPUPDATE:      SimHandleExpUpdate(); break;
    default:                        SimHandleNop(); break;
    }
}

// ---- mon data transfer (ported from battle_controller_opponent.c, party generalized) ----

static u32 GetSimMonData(struct Pokemon *party, u8 monId, u8 *dst)
{
    struct BattlePokemon battleMon;
    struct MovePpInfo moveData;
    u8 nickname[20];
    u8 *src;
    s16 data16;
    u32 data32;
    s32 size = 0;

    switch (gBattleBufferA[gActiveBattler][1])
    {
    case REQUEST_ALL_BATTLE:
        battleMon.species = GetMonData(&party[monId], MON_DATA_SPECIES);
        battleMon.item = GetMonData(&party[monId], MON_DATA_HELD_ITEM);
        for (size = 0; size < MAX_MON_MOVES; ++size)
        {
            battleMon.moves[size] = GetMonData(&party[monId], MON_DATA_MOVE1 + size);
            battleMon.pp[size] = GetMonData(&party[monId], MON_DATA_PP1 + size);
        }
        battleMon.ppBonuses = GetMonData(&party[monId], MON_DATA_PP_BONUSES);
        battleMon.friendship = GetMonData(&party[monId], MON_DATA_FRIENDSHIP);
        battleMon.experience = GetMonData(&party[monId], MON_DATA_EXP);
        battleMon.hpIV = GetMonData(&party[monId], MON_DATA_HP_IV);
        battleMon.attackIV = GetMonData(&party[monId], MON_DATA_ATK_IV);
        battleMon.defenseIV = GetMonData(&party[monId], MON_DATA_DEF_IV);
        battleMon.speedIV = GetMonData(&party[monId], MON_DATA_SPEED_IV);
        battleMon.spAttackIV = GetMonData(&party[monId], MON_DATA_SPATK_IV);
        battleMon.spDefenseIV = GetMonData(&party[monId], MON_DATA_SPDEF_IV);
        battleMon.personality = GetMonData(&party[monId], MON_DATA_PERSONALITY);
        battleMon.status1 = GetMonData(&party[monId], MON_DATA_STATUS);
        battleMon.level = GetMonData(&party[monId], MON_DATA_LEVEL);
        battleMon.hp = GetMonData(&party[monId], MON_DATA_HP);
        battleMon.maxHP = GetMonData(&party[monId], MON_DATA_MAX_HP);
        battleMon.attack = GetMonData(&party[monId], MON_DATA_ATK);
        battleMon.defense = GetMonData(&party[monId], MON_DATA_DEF);
        battleMon.speed = GetMonData(&party[monId], MON_DATA_SPEED);
        battleMon.spAttack = GetMonData(&party[monId], MON_DATA_SPATK);
        battleMon.spDefense = GetMonData(&party[monId], MON_DATA_SPDEF);
        battleMon.isEgg = GetMonData(&party[monId], MON_DATA_IS_EGG);
        battleMon.abilityNum = GetMonData(&party[monId], MON_DATA_ABILITY_NUM);
        battleMon.otId = GetMonData(&party[monId], MON_DATA_OT_ID);
        GetMonData(&party[monId], MON_DATA_NICKNAME, nickname);
        StringCopy_Nickname(battleMon.nickname, nickname);
        GetMonData(&party[monId], MON_DATA_OT_NAME, battleMon.otName);
        src = (u8 *)&battleMon;
        for (size = 0; size < sizeof(battleMon); ++size)
            dst[size] = src[size];
        break;
    case REQUEST_SPECIES_BATTLE:
        data16 = GetMonData(&party[monId], MON_DATA_SPECIES);
        dst[0] = data16;
        dst[1] = data16 >> 8;
        size = 2;
        break;
    case REQUEST_HELDITEM_BATTLE:
        data16 = GetMonData(&party[monId], MON_DATA_HELD_ITEM);
        dst[0] = data16;
        dst[1] = data16 >> 8;
        size = 2;
        break;
    case REQUEST_MOVES_PP_BATTLE:
        for (size = 0; size < MAX_MON_MOVES; ++size)
        {
            moveData.moves[size] = GetMonData(&party[monId], MON_DATA_MOVE1 + size);
            moveData.pp[size] = GetMonData(&party[monId], MON_DATA_PP1 + size);
        }
        moveData.ppBonuses = GetMonData(&party[monId], MON_DATA_PP_BONUSES);
        src = (u8 *)(&moveData);
        for (size = 0; size < sizeof(moveData); ++size)
            dst[size] = src[size];
        break;
    case REQUEST_MOVE1_BATTLE:
    case REQUEST_MOVE2_BATTLE:
    case REQUEST_MOVE3_BATTLE:
    case REQUEST_MOVE4_BATTLE:
        data16 = GetMonData(&party[monId], MON_DATA_MOVE1 + gBattleBufferA[gActiveBattler][1] - REQUEST_MOVE1_BATTLE);
        dst[0] = data16;
        dst[1] = data16 >> 8;
        size = 2;
        break;
    case REQUEST_PP_DATA_BATTLE:
        for (size = 0; size < MAX_MON_MOVES; ++size)
            dst[size] = GetMonData(&party[monId], MON_DATA_PP1 + size);
        dst[size] = GetMonData(&party[monId], MON_DATA_PP_BONUSES);
        ++size;
        break;
    case REQUEST_PPMOVE1_BATTLE:
    case REQUEST_PPMOVE2_BATTLE:
    case REQUEST_PPMOVE3_BATTLE:
    case REQUEST_PPMOVE4_BATTLE:
        dst[0] = GetMonData(&party[monId], MON_DATA_PP1 + gBattleBufferA[gActiveBattler][1] - REQUEST_PPMOVE1_BATTLE);
        size = 1;
        break;
    case REQUEST_OTID_BATTLE:
        data32 = GetMonData(&party[monId], MON_DATA_OT_ID);
        dst[0] = (data32 & 0x000000FF);
        dst[1] = (data32 & 0x0000FF00) >> 8;
        dst[2] = (data32 & 0x00FF0000) >> 16;
        size = 3;
        break;
    case REQUEST_EXP_BATTLE:
        data32 = GetMonData(&party[monId], MON_DATA_EXP);
        dst[0] = (data32 & 0x000000FF);
        dst[1] = (data32 & 0x0000FF00) >> 8;
        dst[2] = (data32 & 0x00FF0000) >> 16;
        size = 3;
        break;
    case REQUEST_HP_EV_BATTLE:
        dst[0] = GetMonData(&party[monId], MON_DATA_HP_EV);
        size = 1;
        break;
    case REQUEST_ATK_EV_BATTLE:
        dst[0] = GetMonData(&party[monId], MON_DATA_ATK_EV);
        size = 1;
        break;
    case REQUEST_DEF_EV_BATTLE:
        dst[0] = GetMonData(&party[monId], MON_DATA_DEF_EV);
        size = 1;
        break;
    case REQUEST_SPEED_EV_BATTLE:
        dst[0] = GetMonData(&party[monId], MON_DATA_SPEED_EV);
        size = 1;
        break;
    case REQUEST_SPATK_EV_BATTLE:
        dst[0] = GetMonData(&party[monId], MON_DATA_SPATK_EV);
        size = 1;
        break;
    case REQUEST_SPDEF_EV_BATTLE:
        dst[0] = GetMonData(&party[monId], MON_DATA_SPDEF_EV);
        size = 1;
        break;
    case REQUEST_FRIENDSHIP_BATTLE:
        dst[0] = GetMonData(&party[monId], MON_DATA_FRIENDSHIP);
        size = 1;
        break;
    case REQUEST_POKERUS_BATTLE:
        dst[0] = GetMonData(&party[monId], MON_DATA_POKERUS);
        size = 1;
        break;
    case REQUEST_MET_LOCATION_BATTLE:
        dst[0] = GetMonData(&party[monId], MON_DATA_MET_LOCATION);
        size = 1;
        break;
    case REQUEST_MET_LEVEL_BATTLE:
        dst[0] = GetMonData(&party[monId], MON_DATA_MET_LEVEL);
        size = 1;
        break;
    case REQUEST_MET_GAME_BATTLE:
        dst[0] = GetMonData(&party[monId], MON_DATA_MET_GAME);
        size = 1;
        break;
    case REQUEST_POKEBALL_BATTLE:
        dst[0] = GetMonData(&party[monId], MON_DATA_POKEBALL);
        size = 1;
        break;
    case REQUEST_ALL_IVS_BATTLE:
        dst[0] = GetMonData(&party[monId], MON_DATA_HP_IV);
        dst[1] = GetMonData(&party[monId], MON_DATA_ATK_IV);
        dst[2] = GetMonData(&party[monId], MON_DATA_DEF_IV);
        dst[3] = GetMonData(&party[monId], MON_DATA_SPEED_IV);
        dst[4] = GetMonData(&party[monId], MON_DATA_SPATK_IV);
        dst[5] = GetMonData(&party[monId], MON_DATA_SPDEF_IV);
        size = 6;
        break;
    case REQUEST_HP_IV_BATTLE:
        dst[0] = GetMonData(&party[monId], MON_DATA_HP_IV);
        size = 1;
        break;
    case REQUEST_ATK_IV_BATTLE:
        dst[0] = GetMonData(&party[monId], MON_DATA_ATK_IV);
        size = 1;
        break;
    case REQUEST_DEF_IV_BATTLE:
        dst[0] = GetMonData(&party[monId], MON_DATA_DEF_IV);
        size = 1;
        break;
    case REQUEST_SPEED_IV_BATTLE:
        dst[0] = GetMonData(&party[monId], MON_DATA_SPEED_IV);
        size = 1;
        break;
    case REQUEST_SPATK_IV_BATTLE:
        dst[0] = GetMonData(&party[monId], MON_DATA_SPATK_IV);
        size = 1;
        break;
    case REQUEST_SPDEF_IV_BATTLE:
        dst[0] = GetMonData(&party[monId], MON_DATA_SPDEF_IV);
        size = 1;
        break;
    case REQUEST_PERSONALITY_BATTLE:
        data32 = GetMonData(&party[monId], MON_DATA_PERSONALITY);
        dst[0] = (data32 & 0x000000FF);
        dst[1] = (data32 & 0x0000FF00) >> 8;
        dst[2] = (data32 & 0x00FF0000) >> 16;
        dst[3] = (data32 & 0xFF000000) >> 24;
        size = 4;
        break;
    case REQUEST_CHECKSUM_BATTLE:
        data16 = GetMonData(&party[monId], MON_DATA_CHECKSUM);
        dst[0] = data16;
        dst[1] = data16 >> 8;
        size = 2;
        break;
    case REQUEST_STATUS_BATTLE:
        data32 = GetMonData(&party[monId], MON_DATA_STATUS);
        dst[0] = (data32 & 0x000000FF);
        dst[1] = (data32 & 0x0000FF00) >> 8;
        dst[2] = (data32 & 0x00FF0000) >> 16;
        dst[3] = (data32 & 0xFF000000) >> 24;
        size = 4;
        break;
    case REQUEST_LEVEL_BATTLE:
        dst[0] = GetMonData(&party[monId], MON_DATA_LEVEL);
        size = 1;
        break;
    case REQUEST_HP_BATTLE:
        data16 = GetMonData(&party[monId], MON_DATA_HP);
        dst[0] = data16;
        dst[1] = data16 >> 8;
        size = 2;
        break;
    case REQUEST_MAX_HP_BATTLE:
        data16 = GetMonData(&party[monId], MON_DATA_MAX_HP);
        dst[0] = data16;
        dst[1] = data16 >> 8;
        size = 2;
        break;
    case REQUEST_ATK_BATTLE:
        data16 = GetMonData(&party[monId], MON_DATA_ATK);
        dst[0] = data16;
        dst[1] = data16 >> 8;
        size = 2;
        break;
    case REQUEST_DEF_BATTLE:
        data16 = GetMonData(&party[monId], MON_DATA_DEF);
        dst[0] = data16;
        dst[1] = data16 >> 8;
        size = 2;
        break;
    case REQUEST_SPEED_BATTLE:
        data16 = GetMonData(&party[monId], MON_DATA_SPEED);
        dst[0] = data16;
        dst[1] = data16 >> 8;
        size = 2;
        break;
    case REQUEST_SPATK_BATTLE:
        data16 = GetMonData(&party[monId], MON_DATA_SPATK);
        dst[0] = data16;
        dst[1] = data16 >> 8;
        size = 2;
        break;
    case REQUEST_SPDEF_BATTLE:
        data16 = GetMonData(&party[monId], MON_DATA_SPDEF);
        dst[0] = data16;
        dst[1] = data16 >> 8;
        size = 2;
        break;
    case REQUEST_COOL_BATTLE:
        dst[0] = GetMonData(&party[monId], MON_DATA_COOL);
        size = 1;
        break;
    case REQUEST_BEAUTY_BATTLE:
        dst[0] = GetMonData(&party[monId], MON_DATA_BEAUTY);
        size = 1;
        break;
    case REQUEST_CUTE_BATTLE:
        dst[0] = GetMonData(&party[monId], MON_DATA_CUTE);
        size = 1;
        break;
    case REQUEST_SMART_BATTLE:
        dst[0] = GetMonData(&party[monId], MON_DATA_SMART);
        size = 1;
        break;
    case REQUEST_TOUGH_BATTLE:
        dst[0] = GetMonData(&party[monId], MON_DATA_TOUGH);
        size = 1;
        break;
    case REQUEST_SHEEN_BATTLE:
        dst[0] = GetMonData(&party[monId], MON_DATA_SHEEN);
        size = 1;
        break;
    case REQUEST_COOL_RIBBON_BATTLE:
        dst[0] = GetMonData(&party[monId], MON_DATA_COOL_RIBBON);
        size = 1;
        break;
    case REQUEST_BEAUTY_RIBBON_BATTLE:
        dst[0] = GetMonData(&party[monId], MON_DATA_BEAUTY_RIBBON);
        size = 1;
        break;
    case REQUEST_CUTE_RIBBON_BATTLE:
        dst[0] = GetMonData(&party[monId], MON_DATA_CUTE_RIBBON);
        size = 1;
        break;
    case REQUEST_SMART_RIBBON_BATTLE:
        dst[0] = GetMonData(&party[monId], MON_DATA_SMART_RIBBON);
        size = 1;
        break;
    case REQUEST_TOUGH_RIBBON_BATTLE:
        dst[0] = GetMonData(&party[monId], MON_DATA_TOUGH_RIBBON);
        size = 1;
        break;
    }
    return size;
}


static void SetSimMonData(struct Pokemon *party, u8 monId)
{
    struct BattlePokemon *battlePokemon = (struct BattlePokemon *)&gBattleBufferA[gActiveBattler][3];
    struct MovePpInfo *moveData = (struct MovePpInfo *)&gBattleBufferA[gActiveBattler][3];
    s32 i;

    switch (gBattleBufferA[gActiveBattler][1])
    {
    case REQUEST_ALL_BATTLE:
        {
            u8 iv;

            SetMonData(&party[monId], MON_DATA_SPECIES, &battlePokemon->species);
            SetMonData(&party[monId], MON_DATA_HELD_ITEM, &battlePokemon->item);
            for (i = 0; i < MAX_MON_MOVES; ++i)
            {
                SetMonData(&party[monId], MON_DATA_MOVE1 + i, &battlePokemon->moves[i]);
                SetMonData(&party[monId], MON_DATA_PP1 + i, &battlePokemon->pp[i]);
            }
            SetMonData(&party[monId], MON_DATA_PP_BONUSES, &battlePokemon->ppBonuses);
            SetMonData(&party[monId], MON_DATA_FRIENDSHIP, &battlePokemon->friendship);
            SetMonData(&party[monId], MON_DATA_EXP, &battlePokemon->experience);
            iv = battlePokemon->hpIV;
            SetMonData(&party[monId], MON_DATA_HP_IV, &iv);
            iv = battlePokemon->attackIV;
            SetMonData(&party[monId], MON_DATA_ATK_IV, &iv);
            iv = battlePokemon->defenseIV;
            SetMonData(&party[monId], MON_DATA_DEF_IV, &iv);
            iv = battlePokemon->speedIV;
            SetMonData(&party[monId], MON_DATA_SPEED_IV, &iv);
            iv = battlePokemon->spAttackIV;
            SetMonData(&party[monId], MON_DATA_SPATK_IV, &iv);
            iv = battlePokemon->spDefenseIV;
            SetMonData(&party[monId], MON_DATA_SPDEF_IV, &iv);
            SetMonData(&party[monId], MON_DATA_PERSONALITY, &battlePokemon->personality);
            SetMonData(&party[monId], MON_DATA_STATUS, &battlePokemon->status1);
            SetMonData(&party[monId], MON_DATA_LEVEL, &battlePokemon->level);
            SetMonData(&party[monId], MON_DATA_HP, &battlePokemon->hp);
            SetMonData(&party[monId], MON_DATA_MAX_HP, &battlePokemon->maxHP);
            SetMonData(&party[monId], MON_DATA_ATK, &battlePokemon->attack);
            SetMonData(&party[monId], MON_DATA_DEF, &battlePokemon->defense);
            SetMonData(&party[monId], MON_DATA_SPEED, &battlePokemon->speed);
            SetMonData(&party[monId], MON_DATA_SPATK, &battlePokemon->spAttack);
            SetMonData(&party[monId], MON_DATA_SPDEF, &battlePokemon->spDefense);
        }
        break;
    case REQUEST_SPECIES_BATTLE:
        SetMonData(&party[monId], MON_DATA_SPECIES, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_HELDITEM_BATTLE:
        SetMonData(&party[monId], MON_DATA_HELD_ITEM, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_MOVES_PP_BATTLE:
        for (i = 0; i < MAX_MON_MOVES; ++i)
        {
            SetMonData(&party[monId], MON_DATA_MOVE1 + i, &moveData->moves[i]);
            SetMonData(&party[monId], MON_DATA_PP1 + i, &moveData->pp[i]);
        }
        SetMonData(&party[monId], MON_DATA_PP_BONUSES, &moveData->ppBonuses);
        break;
    case REQUEST_MOVE1_BATTLE:
    case REQUEST_MOVE2_BATTLE:
    case REQUEST_MOVE3_BATTLE:
    case REQUEST_MOVE4_BATTLE:
        SetMonData(&party[monId], MON_DATA_MOVE1 + gBattleBufferA[gActiveBattler][1] - REQUEST_MOVE1_BATTLE, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_PP_DATA_BATTLE:
        SetMonData(&party[monId], MON_DATA_PP1, &gBattleBufferA[gActiveBattler][3]);
        SetMonData(&party[monId], MON_DATA_PP2, &gBattleBufferA[gActiveBattler][4]);
        SetMonData(&party[monId], MON_DATA_PP3, &gBattleBufferA[gActiveBattler][5]);
        SetMonData(&party[monId], MON_DATA_PP4, &gBattleBufferA[gActiveBattler][6]);
        SetMonData(&party[monId], MON_DATA_PP_BONUSES, &gBattleBufferA[gActiveBattler][7]);
        break;
    case REQUEST_PPMOVE1_BATTLE:
    case REQUEST_PPMOVE2_BATTLE:
    case REQUEST_PPMOVE3_BATTLE:
    case REQUEST_PPMOVE4_BATTLE:
        SetMonData(&party[monId], MON_DATA_PP1 + gBattleBufferA[gActiveBattler][1] - REQUEST_PPMOVE1_BATTLE, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_OTID_BATTLE:
        SetMonData(&party[monId], MON_DATA_OT_ID, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_EXP_BATTLE:
        SetMonData(&party[monId], MON_DATA_EXP, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_HP_EV_BATTLE:
        SetMonData(&party[monId], MON_DATA_HP_EV, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_ATK_EV_BATTLE:
        SetMonData(&party[monId], MON_DATA_ATK_EV, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_DEF_EV_BATTLE:
        SetMonData(&party[monId], MON_DATA_DEF_EV, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_SPEED_EV_BATTLE:
        SetMonData(&party[monId], MON_DATA_SPEED_EV, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_SPATK_EV_BATTLE:
        SetMonData(&party[monId], MON_DATA_SPATK_EV, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_SPDEF_EV_BATTLE:
        SetMonData(&party[monId], MON_DATA_SPDEF_EV, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_FRIENDSHIP_BATTLE:
        SetMonData(&party[monId], MON_DATA_FRIENDSHIP, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_POKERUS_BATTLE:
        SetMonData(&party[monId], MON_DATA_POKERUS, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_MET_LOCATION_BATTLE:
        SetMonData(&party[monId], MON_DATA_MET_LOCATION, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_MET_LEVEL_BATTLE:
        SetMonData(&party[monId], MON_DATA_MET_LEVEL, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_MET_GAME_BATTLE:
        SetMonData(&party[monId], MON_DATA_MET_GAME, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_POKEBALL_BATTLE:
        SetMonData(&party[monId], MON_DATA_POKEBALL, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_ALL_IVS_BATTLE:
        SetMonData(&party[monId], MON_DATA_HP_IV, &gBattleBufferA[gActiveBattler][3]);
        SetMonData(&party[monId], MON_DATA_ATK_IV, &gBattleBufferA[gActiveBattler][4]);
        SetMonData(&party[monId], MON_DATA_DEF_IV, &gBattleBufferA[gActiveBattler][5]);
        SetMonData(&party[monId], MON_DATA_SPEED_IV, &gBattleBufferA[gActiveBattler][6]);
        SetMonData(&party[monId], MON_DATA_SPATK_IV, &gBattleBufferA[gActiveBattler][7]);
        SetMonData(&party[monId], MON_DATA_SPDEF_IV, &gBattleBufferA[gActiveBattler][8]);
        break;
    case REQUEST_HP_IV_BATTLE:
        SetMonData(&party[monId], MON_DATA_HP_IV, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_ATK_IV_BATTLE:
        SetMonData(&party[monId], MON_DATA_ATK_IV, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_DEF_IV_BATTLE:
        SetMonData(&party[monId], MON_DATA_DEF_IV, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_SPEED_IV_BATTLE:
        SetMonData(&party[monId], MON_DATA_SPEED_IV, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_SPATK_IV_BATTLE:
        SetMonData(&party[monId], MON_DATA_SPATK_IV, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_SPDEF_IV_BATTLE:
        SetMonData(&party[monId], MON_DATA_SPDEF_IV, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_PERSONALITY_BATTLE:
        SetMonData(&party[monId], MON_DATA_PERSONALITY, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_CHECKSUM_BATTLE:
        SetMonData(&party[monId], MON_DATA_CHECKSUM, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_STATUS_BATTLE:
        SetMonData(&party[monId], MON_DATA_STATUS, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_LEVEL_BATTLE:
        SetMonData(&party[monId], MON_DATA_LEVEL, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_HP_BATTLE:
        SetMonData(&party[monId], MON_DATA_HP, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_MAX_HP_BATTLE:
        SetMonData(&party[monId], MON_DATA_MAX_HP, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_ATK_BATTLE:
        SetMonData(&party[monId], MON_DATA_ATK, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_DEF_BATTLE:
        SetMonData(&party[monId], MON_DATA_DEF, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_SPEED_BATTLE:
        SetMonData(&party[monId], MON_DATA_SPEED, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_SPATK_BATTLE:
        SetMonData(&party[monId], MON_DATA_SPATK, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_SPDEF_BATTLE:
        SetMonData(&party[monId], MON_DATA_SPDEF, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_COOL_BATTLE:
        SetMonData(&party[monId], MON_DATA_COOL, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_BEAUTY_BATTLE:
        SetMonData(&party[monId], MON_DATA_BEAUTY, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_CUTE_BATTLE:
        SetMonData(&party[monId], MON_DATA_CUTE, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_SMART_BATTLE:
        SetMonData(&party[monId], MON_DATA_SMART, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_TOUGH_BATTLE:
        SetMonData(&party[monId], MON_DATA_TOUGH, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_SHEEN_BATTLE:
        SetMonData(&party[monId], MON_DATA_SHEEN, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_COOL_RIBBON_BATTLE:
        SetMonData(&party[monId], MON_DATA_COOL_RIBBON, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_BEAUTY_RIBBON_BATTLE:
        SetMonData(&party[monId], MON_DATA_BEAUTY_RIBBON, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_CUTE_RIBBON_BATTLE:
        SetMonData(&party[monId], MON_DATA_CUTE_RIBBON, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_SMART_RIBBON_BATTLE:
        SetMonData(&party[monId], MON_DATA_SMART_RIBBON, &gBattleBufferA[gActiveBattler][3]);
        break;
    case REQUEST_TOUGH_RIBBON_BATTLE:
        SetMonData(&party[monId], MON_DATA_TOUGH_RIBBON, &gBattleBufferA[gActiveBattler][3]);
        break;
    }
}
