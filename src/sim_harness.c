#if SIM_HARNESS
#include "global.h"
#include <string.h>
#include "gflib.h"
#include "battle.h"
#include "battle_anim.h"
#include "battle_controllers.h"
#include "battle_main.h"
#include "event_data.h"
#include "item_menu.h"
#include "party_menu.h"
#include "load_save.h"
#include "main.h"
#include "new_game.h"
#include "pokemon.h"
#include "random.h"
#include "sim_harness.h"
#include "util.h"
#include "constants/flags.h"
#include "constants/items.h"
#include "constants/party_menu.h"

EWRAM_DATA struct SimHarness gSimHarness = {0};

EWRAM_DATA struct SimHarnessShadow gSimHarnessShadow = {0};

void SimHarness_EndOfMainLoop(void)
{
    struct SimHarnessShadow *sh = &gSimHarnessShadow;
    sh->busy = 1;
    sh->engineRngCalls = gSimHarness.engineRngCalls;
    sh->otherRngCalls = gSimHarness.otherRngCalls;
    memcpy(sh->callerRing, gSimHarness.callerRing, sizeof(sh->callerRing));
    memcpy(sh->battleMons, gBattleMons, sizeof(sh->battleMons));
    memcpy(sh->statuses3, gStatuses3, sizeof(sh->statuses3));
    memcpy(sh->sideStatuses, gSideStatuses, sizeof(sh->sideStatuses));
    memcpy(sh->sideTimers, gSideTimers, sizeof(sh->sideTimers));
    memcpy(sh->disableStructs, gDisableStructs, sizeof(sh->disableStructs));
    memcpy(sh->weather, &gBattleWeather, sizeof(sh->weather));
    memcpy(sh->wishFutureKnock, &gWishFutureKnock, sizeof(sh->wishFutureKnock));
    memcpy(sh->playerParty, gPlayerParty, sizeof(sh->playerParty));
    memcpy(sh->enemyParty, gEnemyParty, sizeof(sh->enemyParty));
    memcpy(sh->battlerPartyIndexes, gBattlerPartyIndexes, sizeof(sh->battlerPartyIndexes));
    sh->absentBattlerFlags = gAbsentBattlerFlags;
    sh->battleOutcome = gBattleOutcome;
    memcpy(sh->battleCommunication, gBattleCommunication, sizeof(sh->battleCommunication));
    sh->controllerExecFlags = gBattleControllerExecFlags;
    sh->request = gSimHarness.request;
    sh->requestBattler = gSimHarness.requestBattler;
    sh->requestSeq = gSimHarness.requestSeq;
    sh->state = gSimHarness.state;
    sh->iters++;
    sh->busy = 0;
}


// Mirrors sim/src/sim_controller.c: an answer is kept per battler until a request of the same kind consumes it.
struct HarnessAnswer
{
    u8 valid;
    u8 kind;
    u8 type;
    u8 moveSlot;
    u8 target;
    u8 partySlot;
    u16 item;
};
static EWRAM_DATA struct HarnessAnswer sAnswer[MAX_BATTLERS_COUNT] = {0};

extern void PlayerBufferExecCompleted_Harness(void);
extern void OpponentBufferExecCompleted(void);

static void Complete(void)
{
    if (GetBattlerSide(gActiveBattler) == B_SIDE_PLAYER)
        PlayerBufferExecCompleted_Harness();
    else
        OpponentBufferExecCompleted();
}

bool8 SimHarness_EngineRandom(u32 caller, u16 *value)
{
    u32 i, x;

    if (gSimHarness.engineRngState == 0)
        return FALSE;
    caller &= ~1u;
    for (i = 0; i < gSimHarness.rangeCount && i < SIM_HARNESS_MAX_RANGES; i++)
        if (caller >= gSimHarness.engineRanges[i][0] && caller < gSimHarness.engineRanges[i][1])
            break;
    if (i == gSimHarness.rangeCount || i >= SIM_HARNESS_MAX_RANGES)
    {
        gSimHarness.otherRngCalls++;
        return FALSE;
    }
    x = gSimHarness.engineRngState;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    gSimHarness.engineRngState = x;
    gSimHarness.callerRing[gSimHarness.engineRngCalls % 64] = caller;
    gSimHarness.engineRngCalls++;
    *value = x >> 16;
    return TRUE;
}

static void StartBattle(void)
{
    s32 i;
    static const u8 sName[] = { 0xBB, 0xC3, 0xC7, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }; // same bytes as the simulator's stub

    Sav2_ClearSetDefault();
    ClearSav1();
    for (i = 0; i < PLAYER_NAME_LENGTH + 1; i++)
        gSaveBlock2Ptr->playerName[i] = sName[i];
    {
        // Rival-class trainers print gSaveBlock1Ptr->rivalName; ClearSav1() leaves it without an EOS,
        // so the intro text would run off into garbage. "BLUE" in the game's charmap.
        static const u8 sRival[] = { 0xBC, 0xC6, 0xCF, 0xBF, 0xFF, 0xFF, 0xFF, 0xFF };
        for (i = 0; i < PLAYER_NAME_LENGTH + 1; i++)
            gSaveBlock1Ptr->rivalName[i] = sRival[i];
    }
    gSaveBlock2Ptr->playerTrainerId[0] = 0;
    gSaveBlock2Ptr->playerTrainerId[1] = 0;
    gSaveBlock2Ptr->playerTrainerId[2] = 0;
    gSaveBlock2Ptr->playerTrainerId[3] = 0;
    gSaveBlock2Ptr->optionsBattleStyle = gSimHarness.battleStyle;
    gSaveBlock2Ptr->optionsBattleSceneOff = gSimHarness.sceneOff;
    gSaveBlock2Ptr->optionsTextSpeed = OPTIONS_TEXT_SPEED_FAST;
    gSimHarness.engineRngCalls = 0;
    gSimHarness.otherRngCalls = 0;
    for (i = 0; i < 8; i++)
        if ((gSimHarness.badges >> i) & 1)
            FlagSet(FLAG_BADGE01_GET + i);
    for (i = 0; i < MAX_BATTLERS_COUNT; i++)
        sAnswer[i].valid = FALSE;
    gSimHarness.request = SIM_HARNESS_REQ_NONE;
    SeedRng(gSimHarness.seed);
    gTrainerBattleOpponent_A = gSimHarness.trainerId;
    gBattleTypeFlags = gSimHarness.battleTypeFlags;
    gMain.savedCallback = SimHarness_CB2_Boot;
    gSimHarness.state = SIM_HARNESS_STATE_IN_BATTLE;
    SetMainCallback2(CB2_InitBattle);
}

void SimHarness_CB2_Boot(void)
{
    gSimHarness.magic = SIM_HARNESS_MAGIC;
    switch (gSimHarness.state)
    {
    case SIM_HARNESS_STATE_IN_BATTLE:
        // The battle handed control back to us.
        gSimHarness.outcome = gBattleOutcome;
        gSimHarness.battleCount++;
        gSimHarness.state = SIM_HARNESS_STATE_FINISHED;
        break;
    case SIM_HARNESS_STATE_IDLE:
    case SIM_HARNESS_STATE_FINISHED:
        if (gSimHarness.go)
        {
            gSimHarness.go = 0;
            StartBattle();
        }
        break;
    }
}

// ---- player controller replacement ----

static bool8 ObtainAnswer(u8 kind)
{
    struct HarnessAnswer *a = &sAnswer[gActiveBattler];

    if (a->valid && a->kind == kind)
        return TRUE;
    a->valid = FALSE;
    if (gSimHarness.request == SIM_HARNESS_REQ_NONE)
    {
        gSimHarness.request = kind;
        gSimHarness.requestBattler = gActiveBattler;
        gSimHarness.requestSeq++;
        return FALSE;
    }
    if (gSimHarness.request == kind && gSimHarness.requestBattler == gActiveBattler && gSimHarness.answerSeq == gSimHarness.requestSeq)
    {
        a->type = gSimHarness.ansType;
        a->moveSlot = gSimHarness.ansMoveSlot;
        a->target = gSimHarness.ansTarget;
        a->partySlot = gSimHarness.ansPartySlot;
        a->item = gSimHarness.ansItem;
        a->valid = TRUE;
        a->kind = kind;
        gSimHarness.request = SIM_HARNESS_REQ_NONE;
        return TRUE;
    }
    return FALSE;
}

static void WaitChooseAction(void)
{
    struct HarnessAnswer *a = &sAnswer[gActiveBattler];

    if (!ObtainAnswer(SIM_HARNESS_REQ_ACTION))
        return;
    if (a->type == B_ACTION_USE_ITEM && a->item == ITEM_NONE)
        a->type = B_ACTION_USE_MOVE;
    BtlController_EmitTwoReturnValues(BUFFER_B, a->type, 0);
    Complete();
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

static void WaitChooseMove(void)
{
    struct HarnessAnswer *a = &sAnswer[gActiveBattler];
    struct ChooseMoveStruct *moveInfo = (struct ChooseMoveStruct *)(&gBattleBufferA[gActiveBattler][4]);
    u8 target;

    if (!ObtainAnswer(SIM_HARNESS_REQ_ACTION))
        return;
    a->valid = FALSE;
    if (a->type != B_ACTION_USE_MOVE)
    {
        BtlController_EmitTwoReturnValues(BUFFER_B, 10, 0xFFFF);
        PlayerBufferExecCompleted_Harness();
        return;
    }
    target = a->target;
    if (target >= gBattlersCount || (gAbsentBattlerFlags & gBitTable[target]) || !(gBattleTypeFlags & BATTLE_TYPE_DOUBLE))
        target = DefaultMoveTarget(moveInfo->moves[a->moveSlot & 3]);
    BtlController_EmitTwoReturnValues(BUFFER_B, 10, (a->moveSlot & 3) | (target << 8));
    Complete();
}

static void WaitChooseItem(void)
{
    struct HarnessAnswer *a = &sAnswer[gActiveBattler];
    u16 item;

    if (!ObtainAnswer(SIM_HARNESS_REQ_ACTION))
        return;
    a->valid = FALSE;
    item = (a->type == B_ACTION_USE_ITEM) ? a->item : ITEM_NONE;
    if (item != ITEM_NONE && item > ITEM_PREMIER_BALL && GetBattlerSide(gActiveBattler) == B_SIDE_PLAYER)
    {
        u8 slot = a->partySlot < PARTY_SIZE ? a->partySlot : gBattlerPartyIndexes[gActiveBattler];
        PokemonUseItemEffects(&gPlayerParty[slot], item, slot, a->moveSlot, FALSE);
    }
    gSpecialVar_ItemId = item;
    BtlController_EmitOneReturnValue(BUFFER_B, item);
    Complete();
}

static void WaitChoosePokemon(void)
{
    struct HarnessAnswer *a = &sAnswer[gActiveBattler];
    u8 caseId = gBattleBufferA[gActiveBattler][1] & 0xF;
    u8 chosen = PARTY_SIZE;
    s32 i;

    for (i = 0; i < 3; ++i)
        gBattlePartyCurrentOrder[i] = gBattleBufferA[gActiveBattler][4 + i];
    gSimHarness.caseId = caseId;
    switch (caseId)
    {
    case PARTY_ACTION_CANT_SWITCH:
    case PARTY_ACTION_ABILITY_PREVENTS:
        a->valid = FALSE;
        chosen = PARTY_SIZE;
        break;
    case PARTY_ACTION_CHOOSE_MON:
        if (a->valid && a->type == B_ACTION_SWITCH)
        {
            chosen = a->partySlot;
            a->valid = FALSE;
            break;
        }
        // fallthrough
    default:
        if (!ObtainAnswer(SIM_HARNESS_REQ_SWITCH))
            return;
        chosen = a->partySlot;
        a->valid = FALSE;
        break;
    }
    if (chosen != PARTY_SIZE)
        *(gBattleStruct->monToSwitchIntoId + gActiveBattler) = chosen;
    BtlController_EmitChosenMonReturnValue(BUFFER_B, chosen, gBattlePartyCurrentOrder);
    Complete();
}

void SimHarness_HandleChooseAction(void)
{
    gBattlerControllerFuncs[gActiveBattler] = WaitChooseAction;
}

void SimHarness_HandleChooseMove(void)
{
    gBattlerControllerFuncs[gActiveBattler] = WaitChooseMove;
}

void SimHarness_HandleChooseItem(void)
{
    gBattlerControllerFuncs[gActiveBattler] = WaitChooseItem;
}

void SimHarness_HandleChoosePokemon(void)
{
    gBattlerControllerFuncs[gActiveBattler] = WaitChoosePokemon;
}

void SimHarness_HandleExpUpdate(void)
{
    // The simulator does not grant experience; reply "no level up" like its controller does.
    BtlController_EmitTwoReturnValues(BUFFER_B, 0, 0);
    Complete();
}
#endif // SIM_HARNESS
