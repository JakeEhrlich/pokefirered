#include "global.h"
#include "event_data.h"
#include "item.h"
#include "constants/items.h"
#include "pokemon.h"
#include "game_rules.h"
#include "constants/flags.h"
#include "constants/pokemon.h"
#include "constants/trainers.h"
#include "constants/battle.h"

// Level cap by number of badges. Index 0 is "no badges".
static const u8 sLevelCaps[NUM_BADGES + 1] = {
    [0] = 10,
    [1] = 20,
    [2] = 30,
    [3] = 40,
    [4] = 50,
    [5] = 60,
    [6] = 70,
    [7] = 80,
    [8] = MAX_LEVEL,
};

static const u16 sBadgeFlags[NUM_BADGES] = {
    FLAG_BADGE01_GET,
    FLAG_BADGE02_GET,
    FLAG_BADGE03_GET,
    FLAG_BADGE04_GET,
    FLAG_BADGE05_GET,
    FLAG_BADGE06_GET,
    FLAG_BADGE07_GET,
    FLAG_BADGE08_GET,
};

u8 GetBadgeCount(void)
{
    u8 i, count = 0;

    for (i = 0; i < NUM_BADGES; i++)
    {
        if (FlagGet(sBadgeFlags[i]))
            count++;
    }
    return count;
}

u8 GetLevelCap(void)
{
    return sLevelCaps[GetBadgeCount()];
}

// Total experience at which a mon of this species hits the current level cap.
u32 GetLevelCapExp(u16 species)
{
    return gExperienceTables[gSpeciesInfo[species].growthRate][GetLevelCap()];
}

// Every trainer becomes battleable again. Called on white-out and after a
// Pokémon Center heal. Rival and gym-leader story scripts are gated by their
// own flags/vars, so those still only trigger where the story allows.
void ResetAllTrainerFlags(void)
{
    u16 flag;
    u32 i;

    for (flag = TRAINER_FLAGS_START; flag <= TRAINER_FLAGS_END; flag++)
        FlagClear(flag);
    for (i = 0; i < MAX_REMATCH_ENTRIES; i++)
        gSaveBlock1Ptr->trainerRematches[i] = 0;
}

// Consumable healing items are taken away on every Pokémon Center heal and
// replaced with a fixed kit. (Later: scale the kit by badges / vouchers.)
static const u16 sHealingItems[] = {
    ITEM_POTION, ITEM_SUPER_POTION, ITEM_HYPER_POTION, ITEM_MAX_POTION, ITEM_FULL_RESTORE,
    ITEM_FRESH_WATER, ITEM_SODA_POP, ITEM_LEMONADE, ITEM_MOOMOO_MILK,
    ITEM_ENERGY_POWDER, ITEM_ENERGY_ROOT, ITEM_HEAL_POWDER, ITEM_LAVA_COOKIE, ITEM_BERRY_JUICE,
    ITEM_ANTIDOTE, ITEM_BURN_HEAL, ITEM_ICE_HEAL, ITEM_AWAKENING, ITEM_PARALYZE_HEAL, ITEM_FULL_HEAL,
    ITEM_REVIVE, ITEM_MAX_REVIVE, ITEM_REVIVAL_HERB, ITEM_SACRED_ASH,
    ITEM_ETHER, ITEM_MAX_ETHER, ITEM_ELIXIR, ITEM_MAX_ELIXIR,
};

static const struct { u16 item; u16 count; } sPokeCenterHealingKit[] = {
    { ITEM_POTION, 3 },
};

u16 GetBagItemQuantity(u16 *ptr); // item.c, not exported in item.h

static u16 CountItemInBag(u16 itemId)
{
    struct BagPocket *pocket = &gBagPockets[ItemId_GetPocket(itemId) - 1];
    u16 i, total = 0;

    for (i = 0; i < pocket->capacity; i++)
    {
        if (pocket->itemSlots[i].itemId == itemId)
            total += GetBagItemQuantity(&pocket->itemSlots[i].quantity);
    }
    return total;
}

void ResetHealingItems(void)
{
    u32 i;
    u16 qty;

    for (i = 0; i < ARRAY_COUNT(sHealingItems); i++)
    {
        qty = CountItemInBag(sHealingItems[i]);
        if (qty)
            RemoveBagItem(sHealingItems[i], qty);
    }
    for (i = 0; i < ARRAY_COUNT(sPokeCenterHealingKit); i++)
        AddBagItem(sPokeCenterHealingKit[i].item, sPokeCenterHealingKit[i].count);

    // Saves from before the candy existed get one here.
    if (!CheckBagHasItem(ITEM_ENDLESS_CANDY, 1))
        AddBagItem(ITEM_ENDLESS_CANDY, 1);
}

// Trainer mons that start the battle with a status condition. Slot is the
// party index (0-based) in the trainer's party data.
static const struct { u16 trainer; u8 slot; u32 status; } sTrainerMonPreStatus[] = {
    { TRAINER_RIVAL_ROUTE22_EARLY_SQUIRTLE,   2, STATUS1_BURN }, // Guts Taillow
    { TRAINER_RIVAL_ROUTE22_EARLY_BULBASAUR,  2, STATUS1_BURN },
    { TRAINER_RIVAL_ROUTE22_EARLY_CHARMANDER, 2, STATUS1_BURN },
};

void ApplyTrainerMonPreStatus(u16 trainerNum, u8 slot, struct Pokemon *mon)
{
    u32 i;

    for (i = 0; i < ARRAY_COUNT(sTrainerMonPreStatus); i++)
    {
        if (sTrainerMonPreStatus[i].trainer == trainerNum && sTrainerMonPreStatus[i].slot == slot)
            SetMonData(mon, MON_DATA_STATUS, &sTrainerMonPreStatus[i].status);
    }
}
