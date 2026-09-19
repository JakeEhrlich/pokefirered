// Small helpers that live in other game files (battle_anim_mons.c, party_menu.c, util.c, ...).
#include "global.h"
#include "battle.h"
#include "pokemon.h"
#include "util.h"
#include "constants/flags.h"
#include "constants/weather.h"
#include "sim_globals.h"

const u32 gBitTable[] =
{
    1 << 0, 1 << 1, 1 << 2, 1 << 3, 1 << 4, 1 << 5, 1 << 6, 1 << 7,
    1 << 8, 1 << 9, 1 << 10, 1 << 11, 1 << 12, 1 << 13, 1 << 14, 1 << 15,
    1 << 16, 1 << 17, 1 << 18, 1 << 19, 1 << 20, 1 << 21, 1 << 22, 1 << 23,
    1 << 24, 1 << 25, 1 << 26, 1 << 27, 1 << 28, 1 << 29, 1 << 30, 1 << 31,
};

int CountTrailingZeroBits(u32 value)
{
    u8 i;
    for (i = 0; i < 32; i++)
        if ((value >> i) & 1)
            return i;
    return 0;
}

u16 Sqrt(u32 num)
{
    u32 res = 0, bit = 1u << 30;
    while (bit > num)
        bit >>= 2;
    while (bit)
    {
        if (num >= res + bit)
        {
            num -= res + bit;
            res = (res >> 1) + bit;
        }
        else
            res >>= 1;
        bit >>= 2;
    }
    return res;
}

// --- battle_anim_mons.c ---
u8 GetBattlerSide(u8 battlerId)
{
    return GET_BATTLER_SIDE2(battlerId);
}

u8 GetBattlerPosition(u8 battlerId)
{
    return GET_BATTLER_POSITION(battlerId);
}

u8 GetBattlerAtPosition(u8 position)
{
    u8 i;
    for (i = 0; i < gBattlersCount; ++i)
        if (gBattlerPositions[i] == position)
            break;
    return i;
}

// --- party_menu.c ---
bool8 IsMultiBattle(void)
{
    if (gBattleTypeFlags & BATTLE_TYPE_MULTI && gBattleTypeFlags & BATTLE_TYPE_DOUBLE && gBattleTypeFlags & BATTLE_TYPE_TRAINER && gBattleTypeFlags & BATTLE_TYPE_LINK)
        return TRUE;
    else
        return FALSE;
}

static bool8 IsDoubleBattle(void)
{
    return (gBattleTypeFlags & BATTLE_TYPE_DOUBLE) != 0;
}

static void BufferBattlePartyOrderBySide(u8 *partyBattleOrder, u8 flankId, u8 battlerId)
{
    u8 partyIndexes[PARTY_SIZE];
    s32 i, j;
    u8 leftBattler;
    u8 rightBattler;

    if (GetBattlerSide(battlerId) == B_SIDE_PLAYER)
    {
        leftBattler = GetBattlerAtPosition(B_POSITION_PLAYER_LEFT);
        rightBattler = GetBattlerAtPosition(B_POSITION_PLAYER_RIGHT);
    }
    else
    {
        leftBattler = GetBattlerAtPosition(B_POSITION_OPPONENT_LEFT);
        rightBattler = GetBattlerAtPosition(B_POSITION_OPPONENT_RIGHT);
    }
    if (IsMultiBattle() == TRUE)
    {
        if (flankId != 0)
        {
            partyBattleOrder[0] = 0 | (3 << 4);
            partyBattleOrder[1] = 5 | (4 << 4);
            partyBattleOrder[2] = 2 | (1 << 4);
        }
        else
        {
            partyBattleOrder[0] = 3 | (0 << 4);
            partyBattleOrder[1] = 2 | (1 << 4);
            partyBattleOrder[2] = 5 | (4 << 4);
        }
        return;
    }
    else if (IsDoubleBattle() == FALSE)
    {
        j = 1;
        partyIndexes[0] = gBattlerPartyIndexes[leftBattler];
        for (i = 0; i < PARTY_SIZE; ++i)
        {
            if (i != partyIndexes[0])
            {
                partyIndexes[j] = i;
                ++j;
            }
        }
    }
    else
    {
        j = 2;
        partyIndexes[0] = gBattlerPartyIndexes[leftBattler];
        partyIndexes[1] = gBattlerPartyIndexes[rightBattler];
        for (i = 0; i < PARTY_SIZE; ++i)
        {
            if (i != partyIndexes[0] && i != partyIndexes[1])
            {
                partyIndexes[j] = i;
                ++j;
            }
        }
    }
    for (i = 0; i < 3; ++i)
        partyBattleOrder[i] = (partyIndexes[0 + (i * 2)] << 4) | partyIndexes[1 + (i * 2)];
}

void BufferBattlePartyCurrentOrderBySide(u8 battlerId, u8 flankId)
{
    BufferBattlePartyOrderBySide(gBattleStruct->battlerPartyOrders[battlerId], flankId, battlerId);
}

void SwitchPartyOrderLinkMulti(u8 battlerId, u8 slot, u8 slot2)
{
    // link multi battles are not simulated
}

static u8 GetPartyIdFromBattleSlot(u8 slot)
{
    u8 modResult = slot & 1;
    u8 retVal;

    slot /= 2;
    if (modResult != 0)
        retVal = gBattlePartyCurrentOrder[slot] & 0xF;
    else
        retVal = gBattlePartyCurrentOrder[slot] >> 4;
    return retVal;
}

static void SetPartyIdAtBattleSlot(u8 slot, u8 setVal)
{
    bool32 modResult = slot & 1;

    slot /= 2;
    if (modResult != 0)
        gBattlePartyCurrentOrder[slot] = (gBattlePartyCurrentOrder[slot] & 0xF0) | setVal;
    else
        gBattlePartyCurrentOrder[slot] = (gBattlePartyCurrentOrder[slot] & 0xF) | (setVal << 4);
}

void SwitchPartyMonSlots(u8 slot, u8 slot2)
{
    u8 partyId = GetPartyIdFromBattleSlot(slot);

    SetPartyIdAtBattleSlot(slot, GetPartyIdFromBattleSlot(slot2));
    SetPartyIdAtBattleSlot(slot2, partyId);
}

u8 GetPartyIdFromBattlePartyId(u8 battlePartyId)
{
    u8 i, j;

    for (j = i = 0; i < (s32)NELEMS(gBattlePartyCurrentOrder); ++j, ++i)
    {
        if ((gBattlePartyCurrentOrder[i] >> 4) != battlePartyId)
        {
            ++j;
            if ((gBattlePartyCurrentOrder[i] & 0xF) == battlePartyId)
                return j;
        }
        else
        {
            return j;
        }
    }
    return 0;
}

// --- battle_util2.c ---
void AdjustFriendshipOnBattleFaint(u8 battlerId)
{
    u8 opposingBattlerId;

    if (gBattleTypeFlags & BATTLE_TYPE_DOUBLE)
    {
        u8 opposingBattlerId2;

        opposingBattlerId = GetBattlerAtPosition(B_POSITION_OPPONENT_LEFT);
        opposingBattlerId2 = GetBattlerAtPosition(B_POSITION_OPPONENT_RIGHT);
        if (gBattleMons[opposingBattlerId2].level > gBattleMons[opposingBattlerId].level)
            opposingBattlerId = opposingBattlerId2;
    }
    else
    {
        opposingBattlerId = GetBattlerAtPosition(B_POSITION_OPPONENT_LEFT);
    }
    if (gBattleMons[opposingBattlerId].level > gBattleMons[battlerId].level)
    {
        if (gBattleMons[opposingBattlerId].level - gBattleMons[battlerId].level > 29)
            AdjustFriendship(&gPlayerParty[gBattlerPartyIndexes[battlerId]], FRIENDSHIP_EVENT_FAINT_LARGE);
        else
            AdjustFriendship(&gPlayerParty[gBattlerPartyIndexes[battlerId]], FRIENDSHIP_EVENT_FAINT_SMALL);
    }
    else
    {
        AdjustFriendship(&gPlayerParty[gBattlerPartyIndexes[battlerId]], FRIENDSHIP_EVENT_FAINT_SMALL);
    }
}

// --- event_data.c: only badge flags matter to battles (stat boosts) ---
bool8 FlagGet(u16 id)
{
    if (id >= FLAG_BADGE01_GET && id <= FLAG_BADGE08_GET)
        return (gSim->badgeFlags >> (id - FLAG_BADGE01_GET)) & 1;
    return FALSE;
}

bool8 FlagSet(u16 id) { return FALSE; }
bool8 FlagClear(u16 id) { return FALSE; }
u16 VarGet(u16 id) { return 0; }
bool8 VarSet(u16 id, u16 value) { return FALSE; }

// --- field_weather.c / overworld.c ---
u8 GetCurrentWeather(void) { return WEATHER_NONE; }
u8 GetCurrentMapType(void) { return 0; }
u8 GetCurrentRegionMapSectionId(void) { return 196; } // what the game returns with no map loaded (map 0.0); affects met location / friendship
