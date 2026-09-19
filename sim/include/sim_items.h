#ifndef GUARD_SIM_ITEMS_H
#define GUARD_SIM_ITEMS_H
#include "global.h"
struct SimItem
{
    u8 holdEffect;
    u8 holdEffectParam;
    u8 pocket;
    u8 battleUsage;
    u16 price;
    const char *name;
};
extern const struct SimItem gSimItems[];
#endif
