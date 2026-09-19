// Game data tables the simulator needs, included straight from the repo so they stay in sync.
#include "global.h"
#include "battle.h"
#include "pokemon.h"
#include "item.h"
#include "sim_items.h"
#include "constants/abilities.h"
#include "constants/items.h"
#include "constants/moves.h"
#include "constants/species.h"
#include "constants/trainers.h"
#include "constants/battle_ai.h"
#include "constants/trainer_types.h"
#include "constants/pokemon.h"
#include "data/trainer_parties.h"
#include "data/trainers.h"
#include "data/text/species_names.h"
#include "data/text/trainer_class_names.h"

u8 ItemId_GetHoldEffect(u16 itemId)
{
    if (itemId >= ITEMS_COUNT)
        itemId = ITEM_NONE;
    return gSimItems[itemId].holdEffect;
}

u8 ItemId_GetHoldEffectParam(u16 itemId)
{
    if (itemId >= ITEMS_COUNT)
        itemId = ITEM_NONE;
    return gSimItems[itemId].holdEffectParam;
}

u8 ItemId_GetPocket(u16 itemId)
{
    if (itemId >= ITEMS_COUNT)
        itemId = ITEM_NONE;
    return gSimItems[itemId].pocket;
}

u8 ItemId_GetBattleUsage(u16 itemId)
{
    if (itemId >= ITEMS_COUNT)
        itemId = ITEM_NONE;
    return gSimItems[itemId].battleUsage;
}
