// Fast engine extensions: everything the core (fast_effects.c) does not implement is added here behind the
// hooks declared in fast_internal.h, so several people can extend coverage without touching the core.
#include "fast.h"
#include "fast_internal.h"
#include "global.h"
#include "battle.h"
#include "pokemon.h"
#include "constants/moves.h"
#include "constants/abilities.h"
#include "constants/battle_move_effects.h"

int fs_ext_effect_supported(u8 effect) { (void)effect; return 0; }
int fs_ext_ability_supported(u8 ability) { (void)ability; return 0; }
int fs_ext_use_move(fs_state *s, int side, int slot, u16 move, u16 power, u8 type, void *hit) { (void)s; (void)side; (void)slot; (void)move; (void)power; (void)type; (void)hit; return 0; }
void fs_ext_switch_in_ability(fs_state *s, int side) { (void)s; (void)side; }
