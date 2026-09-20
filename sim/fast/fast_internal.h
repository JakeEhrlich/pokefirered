// Internal declarations shared by fast.c and fast_effects.c.
#ifndef FAST_INTERNAL_H
#define FAST_INTERNAL_H

#include "fast.h"

enum { FS_WEATHER_NONE = 0, FS_WEATHER_RAIN, FS_WEATHER_SUN, FS_WEATHER_SAND, FS_WEATHER_HAIL };
enum { STAT_HP_ = 0, STAT_ATK_ = 1, STAT_DEF_ = 2, STAT_SPEED_ = 3, STAT_SPATK_ = 4, STAT_SPDEF_ = 5, STAT_ACC_ = 6, STAT_EVASION_ = 7 };

struct BattleMove;
extern const u8 fs_stat_ratio[13][2];

// fast.c
const struct BattleMove *fs_move(u16 move);
u8 fs_hold_effect(u16 item);
u8 fs_hold_param(u16 item);
int fs_type_mult(u8 moveType, u8 t1, u8 t2, int foresight);
s32 fs_apply_type(s32 dmg, u8 moveType, u8 t1, u8 t2, int foresight, int *mult);
u32 fs_stat_mod(u32 stat, s8 stage);
u32 fs_effective_speed(fs_state *s, int side);
s32 fs_base_damage(fs_state *s, int atkSide, int defSide, u16 move, u16 power, u8 type, int crit);
int fs_weather_active(const fs_state *s);
int fs_accuracy_check(fs_state *s, int atkSide, int defSide, u16 move, u8 type);
int fs_crit_check(fs_state *s, int atkSide, int defSide, u16 move);
s32 fs_random_roll(fs_state *s, s32 dmg);
void fs_sync_to_party(fs_state *s, int side);
void fs_switch_in(fs_state *s, int side, int idx);
void fs_baton_pass(fs_state *s, int side, int idx);
void fs_faint(fs_state *s, int side);
int fs_alive_count(const fs_state *s, int side);
int fs_can_switch(const fs_state *s, int side);
int fs_who_strikes_first(fs_state *s, int b1, int b2);
int fs_who_strikes_first_ignoring_moves(fs_state *s);
void fs_end_turn(fs_state *s);

// fast_effects.c
void fs_use_move(fs_state *s, int side, int slot);          // slot 4 = Struggle
void fs_switch_in_abilities(fs_state *s, int side);
void fs_fire_pending_intimidate(fs_state *s);
void fs_end_turn_items(fs_state *s, int side);
void fs_move_end_items(fs_state *s, int side);   // status cures / White Herb only (ITEMEFFECT_MOVE_END)
int fs_effect_supported(u8 effect);
int fs_ability_supported(u8 ability);
int fs_item_supported(u16 item);

// fast_ext.c: extensions (two-turn moves, Sleep Talk, Trace, ...). The core calls these hooks for anything it
// does not handle itself; they return 1 when they handled it.
struct fs_hit { s32 dmg; int hit; int crit; int mult; int dbond; int hadSub; };
int fs_ext_effect_supported(u8 effect);
int fs_ext_ability_supported(u8 ability);
int fs_ext_use_move(fs_state *s, int side, int slot, u16 move, u16 power, u8 type, void *hit);
void fs_ext_switch_in_ability(fs_state *s, int side);   // called for every switch-in, after the core's own switch-in abilities
void fs_ext_field_update(fs_state *s);                   // after every action, replacement and end of turn: Trace retries, Forecast forms
void fs_ext_before_switch(fs_state *s, int side);        // right before a chosen (not forced) switch action runs: Pursuit
void fs_ext_on_damage(fs_state *s, int side, u16 move, const struct fs_hit *h);   // after each damaging hit by `side` (Color Change)
void fs_ext_end_turn_item(fs_state *s, int side);        // end-of-turn-only item effects (the confusion berries)
// helpers exported by fast_effects.c for extensions
int fs_change_stage(fs_state *s, int side, int stat, int delta, int byOpp, int ignoreSub);
int fs_try_status(fs_state *s, int side, u16 status, int byOpp, int attackerSide, int isSecondary);
int fs_try_confuse(fs_state *s, int side, int byOpp, int isSecondary);
void fs_damage(fs_state *s, int side, s32 dmg);
void fs_heal(fs_state *s, int side, s32 amount);
// one hit of a damaging move with all the core's rules (accuracy unless noAcc, immunities, crit, STAB, type, roll,
// Substitute / Endure / Focus Band, contact abilities, King's Rock). Returns damage dealt (0 = no hit / no damage).
s32 fs_attack(fs_state *s, int side, u16 move, u16 power, u8 type, int noAcc, int falseSwipe, int noCrit);
// the same with a damage multiplier applied with the crit multiplier (before STAB / type) and optionally no random roll
s32 fs_attack_ex(fs_state *s, int side, u16 move, u16 power, u8 type, int noAcc, int falseSwipe, int noCrit, int dmgMult, int noRoll);

#endif
