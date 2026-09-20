// Fast engine: move execution (cancellers, the damage template, status/stat helpers, effect dispatch),
// switch-in abilities, end-of-turn items, and the support tables.
#include <stdio.h>
#include "fast.h"
#include "fast_internal.h"
#include "global.h"
#include "battle.h"
#include "pokemon.h"
#include "constants/species.h"
#include "constants/moves.h"
#include "constants/abilities.h"
#include "constants/items.h"
#include "constants/hold_effects.h"
#include "constants/battle_move_effects.h"

#define OPP(sd_) ((sd_) ^ 1)
#define ACT(st_, sd_) (&(st_)->side[sd_].act)
#ifndef NUM_BATTLE_MOVE_EFFECTS
#define NUM_BATTLE_MOVE_EFFECTS 214
#endif

// ---------------------------------------------------------------------------------------------------------
// support tables

static const u8 sSupportedEffects[NUM_BATTLE_MOVE_EFFECTS] = {
    [EFFECT_HIT] = 1, [EFFECT_SLEEP] = 1, [EFFECT_POISON_HIT] = 1, [EFFECT_ABSORB] = 1, [EFFECT_BURN_HIT] = 1, [EFFECT_FREEZE_HIT] = 1,
    [EFFECT_PARALYZE_HIT] = 1, [EFFECT_EXPLOSION] = 1, [EFFECT_DREAM_EATER] = 1,
    [EFFECT_ATTACK_UP] = 1, [EFFECT_DEFENSE_UP] = 1, [EFFECT_SPEED_UP] = 1, [EFFECT_SPECIAL_ATTACK_UP] = 1, [EFFECT_SPECIAL_DEFENSE_UP] = 1,
    [EFFECT_ACCURACY_UP] = 1, [EFFECT_EVASION_UP] = 1, [EFFECT_ALWAYS_HIT] = 1,
    [EFFECT_ATTACK_DOWN] = 1, [EFFECT_DEFENSE_DOWN] = 1, [EFFECT_SPEED_DOWN] = 1, [EFFECT_SPECIAL_ATTACK_DOWN] = 1, [EFFECT_SPECIAL_DEFENSE_DOWN] = 1,
    [EFFECT_ACCURACY_DOWN] = 1, [EFFECT_EVASION_DOWN] = 1, [EFFECT_HAZE] = 1, [EFFECT_ROAR] = 1, [EFFECT_MULTI_HIT] = 1, [EFFECT_FLINCH_HIT] = 1,
    [EFFECT_RESTORE_HP] = 1, [EFFECT_TOXIC] = 1, [EFFECT_PAY_DAY] = 1, [EFFECT_LIGHT_SCREEN] = 1, [EFFECT_TRI_ATTACK] = 1, [EFFECT_REST] = 1,
    [EFFECT_OHKO] = 1, [EFFECT_SUPER_FANG] = 1, [EFFECT_DRAGON_RAGE] = 1, [EFFECT_TRAP] = 1, [EFFECT_HIGH_CRITICAL] = 1, [EFFECT_DOUBLE_HIT] = 1,
    [EFFECT_RECOIL_IF_MISS] = 1, [EFFECT_MIST] = 1, [EFFECT_FOCUS_ENERGY] = 1, [EFFECT_RECOIL] = 1, [EFFECT_CONFUSE] = 1,
    [EFFECT_ATTACK_UP_2] = 1, [EFFECT_DEFENSE_UP_2] = 1, [EFFECT_SPEED_UP_2] = 1, [EFFECT_SPECIAL_ATTACK_UP_2] = 1, [EFFECT_SPECIAL_DEFENSE_UP_2] = 1,
    [EFFECT_ACCURACY_UP_2] = 1, [EFFECT_EVASION_UP_2] = 1,
    [EFFECT_ATTACK_DOWN_2] = 1, [EFFECT_DEFENSE_DOWN_2] = 1, [EFFECT_SPEED_DOWN_2] = 1, [EFFECT_SPECIAL_ATTACK_DOWN_2] = 1, [EFFECT_SPECIAL_DEFENSE_DOWN_2] = 1,
    [EFFECT_ACCURACY_DOWN_2] = 1, [EFFECT_EVASION_DOWN_2] = 1, [EFFECT_REFLECT] = 1, [EFFECT_POISON] = 1, [EFFECT_PARALYZE] = 1,
    [EFFECT_ATTACK_DOWN_HIT] = 1, [EFFECT_DEFENSE_DOWN_HIT] = 1, [EFFECT_SPEED_DOWN_HIT] = 1, [EFFECT_SPECIAL_ATTACK_DOWN_HIT] = 1,
    [EFFECT_SPECIAL_DEFENSE_DOWN_HIT] = 1, [EFFECT_ACCURACY_DOWN_HIT] = 1, [EFFECT_EVASION_DOWN_HIT] = 1, [EFFECT_CONFUSE_HIT] = 1,
    [EFFECT_TWINEEDLE] = 1, [EFFECT_VITAL_THROW] = 1, [EFFECT_SUBSTITUTE] = 1, [EFFECT_RECHARGE] = 1, [EFFECT_LEECH_SEED] = 1, [EFFECT_SPLASH] = 1,
    [EFFECT_DISABLE] = 1, [EFFECT_LEVEL_DAMAGE] = 1, [EFFECT_PSYWAVE] = 1, [EFFECT_COUNTER] = 1, [EFFECT_ENCORE] = 1, [EFFECT_PAIN_SPLIT] = 1,
    [EFFECT_DESTINY_BOND] = 1, [EFFECT_FLAIL] = 1, [EFFECT_FALSE_SWIPE] = 1, [EFFECT_HEAL_BELL] = 1, [EFFECT_QUICK_ATTACK] = 1, [EFFECT_TRIPLE_KICK] = 1,
    [EFFECT_THIEF] = 1, [EFFECT_MEAN_LOOK] = 1, [EFFECT_NIGHTMARE] = 1, [EFFECT_MINIMIZE] = 1, [EFFECT_CURSE] = 1, [EFFECT_PROTECT] = 1, [EFFECT_SPIKES] = 1,
    [EFFECT_FORESIGHT] = 1, [EFFECT_PERISH_SONG] = 1, [EFFECT_SANDSTORM] = 1, [EFFECT_ENDURE] = 1, [EFFECT_SWAGGER] = 1, [EFFECT_ATTRACT] = 1,
    [EFFECT_RETURN] = 1, [EFFECT_FRUSTRATION] = 1, [EFFECT_SAFEGUARD] = 1, [EFFECT_THAW_HIT] = 1, [EFFECT_MAGNITUDE] = 1, [EFFECT_BATON_PASS] = 1,
    [EFFECT_RAPID_SPIN] = 1, [EFFECT_SONICBOOM] = 1, [EFFECT_MORNING_SUN] = 1, [EFFECT_SYNTHESIS] = 1, [EFFECT_MOONLIGHT] = 1, [EFFECT_HIDDEN_POWER] = 1,
    [EFFECT_RAIN_DANCE] = 1, [EFFECT_SUNNY_DAY] = 1, [EFFECT_DEFENSE_UP_HIT] = 1, [EFFECT_ATTACK_UP_HIT] = 1, [EFFECT_ALL_STATS_UP_HIT] = 1,
    [EFFECT_BELLY_DRUM] = 1, [EFFECT_PSYCH_UP] = 1, [EFFECT_MIRROR_COAT] = 1, [EFFECT_TWISTER] = 1, [EFFECT_EARTHQUAKE] = 1, [EFFECT_FUTURE_SIGHT] = 1,
    [EFFECT_GUST] = 1, [EFFECT_FLINCH_MINIMIZE_HIT] = 1, [EFFECT_THUNDER] = 1, [EFFECT_TELEPORT] = 1, [EFFECT_DEFENSE_CURL] = 1, [EFFECT_SOFTBOILED] = 1,
    [EFFECT_FAKE_OUT] = 1, [EFFECT_HAIL] = 1, [EFFECT_TORMENT] = 1, [EFFECT_FLATTER] = 1, [EFFECT_WILL_O_WISP] = 1, [EFFECT_MEMENTO] = 1, [EFFECT_FACADE] = 1,
    [EFFECT_FOCUS_PUNCH] = 1, [EFFECT_SMELLINGSALT] = 1, [EFFECT_TAUNT] = 1, [EFFECT_WISH] = 1, [EFFECT_INGRAIN] = 1, [EFFECT_SUPERPOWER] = 1,
    [EFFECT_REVENGE] = 1, [EFFECT_BRICK_BREAK] = 1, [EFFECT_YAWN] = 1, [EFFECT_KNOCK_OFF] = 1, [EFFECT_ENDEAVOR] = 1, [EFFECT_ERUPTION] = 1,
    [EFFECT_REFRESH] = 1, [EFFECT_LOW_KICK] = 1, [EFFECT_DOUBLE_EDGE] = 1, [EFFECT_TEETER_DANCE] = 1, [EFFECT_BLAZE_KICK] = 1, [EFFECT_MUD_SPORT] = 1,
    [EFFECT_POISON_FANG] = 1, [EFFECT_WEATHER_BALL] = 1, [EFFECT_OVERHEAT] = 1, [EFFECT_TICKLE] = 1, [EFFECT_COSMIC_POWER] = 1, [EFFECT_SKY_UPPERCUT] = 1,
    [EFFECT_BULK_UP] = 1, [EFFECT_POISON_TAIL] = 1, [EFFECT_WATER_SPORT] = 1, [EFFECT_CALM_MIND] = 1, [EFFECT_DRAGON_DANCE] = 1,
};

int fs_effect_supported(u8 effect) { return effect < NUM_BATTLE_MOVE_EFFECTS && (sSupportedEffects[effect] || fs_ext_effect_supported(effect)); }

int fs_ability_supported(u8 ab)
{
    switch (ab)
    {
    case ABILITY_TRACE: case ABILITY_CLOUD_NINE: case ABILITY_AIR_LOCK: case ABILITY_FORECAST: case ABILITY_COLOR_CHANGE:
        return fs_ext_ability_supported(ab);
    default:
        return 1;
    }
}

int fs_item_supported(u16 item)
{
    u8 he = fs_hold_effect(item);
    switch (he)
    {
    case HOLD_EFFECT_CONFUSE_SPICY: case HOLD_EFFECT_CONFUSE_DRY: case HOLD_EFFECT_CONFUSE_SWEET: case HOLD_EFFECT_CONFUSE_BITTER: case HOLD_EFFECT_CONFUSE_SOUR:
        return 0;
    default:
        return item != ITEM_ENIGMA_BERRY;
    }
}

// ---------------------------------------------------------------------------------------------------------
// helpers

static int IsType(const fs_battler *a, u8 t) { return a->type1 == t || a->type2 == t; }

static int IsSoundMove(u16 move)
{
    switch (move)
    {
    case MOVE_GROWL: case MOVE_ROAR: case MOVE_SING: case MOVE_SUPERSONIC: case MOVE_SCREECH: case MOVE_SNORE: case MOVE_UPROAR:
    case MOVE_METAL_SOUND: case MOVE_GRASS_WHISTLE: case MOVE_HYPER_VOICE: case MOVE_PERISH_SONG: case MOVE_HEAL_BELL:
        return 1;
    }
    return 0;
}

static void Damage(fs_state *s, int side, s32 dmg)
{
    fs_battler *a = ACT(s, side);
    if (!a->present) return;
    if (dmg >= a->hp) { a->hp = 0; fs_faint(s, side); }
    else a->hp -= dmg;
}

static void Heal(fs_state *s, int side, s32 amount)
{
    fs_battler *a = ACT(s, side);
    if (!a->present || a->hp == 0) return;
    if (amount <= 0) amount = 1;
    a->hp = (a->hp + amount > a->maxHP) ? a->maxHP : a->hp + amount;
}

// stat stage change; byOpp: caused by the opponent's move (Mist / Clear Body / substitute apply). Returns 1 if changed.
static int ChangeStage(fs_state *s, int side, int stat, int delta, int byOpp, int ignoreSub)
{
    fs_battler *a = ACT(s, side);
    s8 v;
    if (!a->present) return 0;
    if (byOpp)
    {
        if (delta < 0)
        {
            if ((a->vol & FS_V_SUBSTITUTE) && !ignoreSub) return 0;
            if (s->side[side].mist) return 0;
            if (a->ability == ABILITY_CLEAR_BODY || a->ability == ABILITY_WHITE_SMOKE) return 0;
            if (a->ability == ABILITY_KEEN_EYE && stat == STAT_ACC_) return 0;
            if (a->ability == ABILITY_HYPER_CUTTER && stat == STAT_ATK_) return 0;
        }
    }
    v = a->stages[stat] + delta;
    if (delta > 0 && a->stages[stat] >= 12) return 0;
    if (delta < 0 && a->stages[stat] <= 0) return 0;
    if (v > 12) v = 12;
    if (v < 0) v = 0;
    a->stages[stat] = v;
    return 1;
}

// non-volatile status. byOpp: inflicted by the other side's move (Safeguard, substitute apply). Returns 1 if inflicted.
static int TryStatus(fs_state *s, int side, u16 status, int byOpp, int attackerSide, int isSecondary)
{
    fs_battler *a = ACT(s, side);
    if (!a->present || a->hp == 0) return 0;
    if (a->status1) return 0;
    if (byOpp && (a->vol & FS_V_SUBSTITUTE)) return 0;
    if (byOpp && s->side[side].safeguard) return 0;
    if (isSecondary && a->ability == ABILITY_SHIELD_DUST) return 0;
    switch (status)
    {
    case FS_S1_PSN: case FS_S1_TOX:
        if (IsType(a, TYPE_POISON) || IsType(a, TYPE_STEEL)) return 0;
        if (a->ability == ABILITY_IMMUNITY) return 0;
        break;
    case FS_S1_BRN:
        if (IsType(a, TYPE_FIRE)) return 0;
        if (a->ability == ABILITY_WATER_VEIL) return 0;
        break;
    case FS_S1_PAR:
        if (a->ability == ABILITY_LIMBER) return 0;
        break;
    case FS_S1_FRZ:
        if (IsType(a, TYPE_ICE)) return 0;
        if (a->ability == ABILITY_MAGMA_ARMOR) return 0;
        if (fs_weather_active(s) && s->weather == FS_WEATHER_SUN) return 0;
        break;
    case FS_S1_SLEEP:
        if (a->ability == ABILITY_INSOMNIA || a->ability == ABILITY_VITAL_SPIRIT) return 0;
        break;
    }
    if (status == FS_S1_SLEEP) a->status1 = 2 + fs_roll(s, 4);
    else a->status1 = status;
    if (status == FS_S1_TOX) a->status1 = FS_S1_TOX;
    // Synchronize: poison / burn / paralysis bounce to the attacker
    if (byOpp && a->ability == ABILITY_SYNCHRONIZE && (status == FS_S1_PSN || status == FS_S1_TOX || status == FS_S1_BRN || status == FS_S1_PAR))
        TryStatus(s, attackerSide, status == FS_S1_TOX ? FS_S1_PSN : status, 0, side, 0);
    return 1;
}

static int TryConfuse(fs_state *s, int side, int byOpp, int isSecondary)
{
    fs_battler *a = ACT(s, side);
    if (!a->present || a->hp == 0) return 0;
    if (a->vol & FS_V_CONFUSED) return 0;
    if (byOpp && (a->vol & FS_V_SUBSTITUTE)) return 0;
    if (byOpp && s->side[side].safeguard) return 0;
    if (a->ability == ABILITY_OWN_TEMPO) return 0;
    if (isSecondary && a->ability == ABILITY_SHIELD_DUST) return 0;
    a->vol |= FS_V_CONFUSED;
    a->confusionTurns = 2 + fs_roll(s, 4);
    return 1;
}

static void ForceSwitchRandom(fs_state *s, int side)
{
    // Roar / Whirlwind: a random other alive party member
    fs_side *sd = &s->side[side];
    int cand[FS_PARTY], n = 0, i;
    for (i = 0; i < FS_PARTY; i++)
        if (sd->party[i].species && sd->party[i].hp > 0 && i != sd->act.monIdx) cand[n++] = i;
    if (n == 0) return;
    fs_switch_in(s, side, cand[fs_roll(s, n)]);
}

// ---------------------------------------------------------------------------------------------------------
// the attack

struct Hit { s32 dmg; int hit; int crit; int mult; };

// Deals the damage of one hit of `move` with `power` and `type`. Handles accuracy (unless noAcc), immunities,
// crit, STAB, type, roll, Substitute / Endure / Focus Band / False Swipe, and applies it. Returns the hit record.
static struct Hit AttackHit(fs_state *s, int side, u16 move, u16 power, u8 type, int noAcc, int falseSwipe, int noCrit)
{
    struct Hit h = {0, 0, 0, 10};
    int opp = OPP(side);
    fs_battler *a = ACT(s, side), *t = ACT(s, opp);
    const struct BattleMove *bm = fs_move(move);
    s32 dmg;
    if (!t->present) return h;
    if ((t->vol & FS_V_PROTECTED) && (bm->flags & FLAG_PROTECT_AFFECTED)) return h;
    if (!noAcc && !fs_accuracy_check(s, side, opp, move, type)) return h;
    if (t->ability == ABILITY_LEVITATE && type == TYPE_GROUND) return h;
    if (t->ability == ABILITY_SOUNDPROOF && IsSoundMove(move)) return h;
    h.mult = fs_type_mult(type, t->type1, t->type2, (t->vol & FS_V_FORESIGHT) != 0);
    if (h.mult == 0 && move != MOVE_STRUGGLE) return h;
    if (t->ability == ABILITY_WONDER_GUARD && h.mult <= 10) return h;
    if (t->ability == ABILITY_VOLT_ABSORB && type == TYPE_ELECTRIC) { Heal(s, opp, t->maxHP / 4); h.hit = 1; h.dmg = 0; return h; }
    if (t->ability == ABILITY_WATER_ABSORB && type == TYPE_WATER) { Heal(s, opp, t->maxHP / 4); h.hit = 1; h.dmg = 0; return h; }
    if (t->ability == ABILITY_FLASH_FIRE && type == TYPE_FIRE) { t->vol |= FS_V_FLASH_FIRE; h.hit = 1; h.dmg = 0; return h; }
    h.crit = noCrit ? 0 : fs_crit_check(s, side, opp, move);
    dmg = fs_base_damage(s, side, opp, move, power, type, h.crit);
    dmg *= h.crit ? 2 : 1;
    if ((a->vol & FS_V_CHARGED) && type == TYPE_ELECTRIC) dmg *= 2;
    if (move != MOVE_STRUGGLE)
    {
        if (IsType(a, type)) dmg = dmg * 15 / 10;
        dmg = fs_apply_type(dmg, type, t->type1, t->type2, (t->vol & FS_V_FORESIGHT) != 0, NULL);
    }
    dmg = fs_random_roll(s, dmg);
    if (dmg == 0) dmg = 1;
    // Focus Band / Endure / False Swipe
    {
        int focus = fs_hold_effect(t->item) == HOLD_EFFECT_FOCUS_BAND && fs_chance(s, fs_hold_param(t->item), 100);
        if (!(t->vol & FS_V_SUBSTITUTE) && (falseSwipe || (t->vol & FS_V_ENDURED) || focus) && t->hp <= dmg)
            dmg = t->hp - 1;
    }
    h.hit = 1;
    if (t->vol & FS_V_SUBSTITUTE)
    {
        if (dmg >= t->substituteHP) { t->substituteHP = 0; t->vol &= ~FS_V_SUBSTITUTE; }
        else t->substituteHP -= dmg;
        h.dmg = dmg;
        return h;
    }
    if (dmg > t->hp) dmg = t->hp;
    h.dmg = dmg;
    t->lastLandedMove = move; t->lastHitByType = type;
    if (t->vol & FS_V_DESTINY_BOND) { /* handled by the caller after faint */ }
    Damage(s, opp, dmg);
    return h;
}

// after a hit: contact abilities, King's Rock, Destiny Bond, Shell Bell, Rough Skin
static void AfterHit(fs_state *s, int side, u16 move, struct Hit *h, int contact)
{
    int opp = OPP(side);
    fs_battler *a = ACT(s, side), *t = ACT(s, opp);
    if (!h->hit || h->dmg == 0) return;
    if (contact && a->present)
    {
        switch (t->ability)
        {
        case ABILITY_STATIC: if (fs_chance(s, 3, 10)) TryStatus(s, side, FS_S1_PAR, 0, opp, 0); break;
        case ABILITY_POISON_POINT: if (fs_chance(s, 3, 10)) TryStatus(s, side, FS_S1_PSN, 0, opp, 0); break;
        case ABILITY_FLAME_BODY: if (fs_chance(s, 3, 10)) TryStatus(s, side, FS_S1_BRN, 0, opp, 0); break;
        case ABILITY_EFFECT_SPORE:
            if (fs_chance(s, 1, 10))
            {
                u32 r = fs_roll(s, 3);
                TryStatus(s, side, r == 0 ? FS_S1_PSN : r == 1 ? FS_S1_PAR : FS_S1_SLEEP, 0, opp, 0);
            }
            break;
        case ABILITY_ROUGH_SKIN: Damage(s, side, a->maxHP / 16 ? a->maxHP / 16 : 1); break;
        case ABILITY_CUTE_CHARM:
            if (fs_chance(s, 3, 10) && !(a->vol & FS_V_INFATUATED) && a->gender != t->gender && a->gender != 0xFF && t->gender != 0xFF && a->ability != ABILITY_OBLIVIOUS)
                a->vol |= FS_V_INFATUATED;
            break;
        }
    }
    if (a->present && fs_hold_effect(a->item) == HOLD_EFFECT_SHELL_BELL && h->dmg > 0) Heal(s, side, h->dmg / 8 ? h->dmg / 8 : 1);
    if (t->present && !(t->vol & FS_V_SUBSTITUTE) && fs_hold_effect(a->item) == HOLD_EFFECT_FLINCH && (fs_move(move)->flags & FLAG_KINGS_ROCK_AFFECTED) && fs_chance(s, fs_hold_param(a->item), 100))
        t->vol |= FS_V_FLINCH;
    if (!t->present && (t->vol & FS_V_DESTINY_BOND) == 0) {}
}

// secondary effect roll (Serene Grace doubles; substitute and Shield Dust block effects on the target)
static int SecondaryRoll(fs_state *s, int side, u16 move)
{
    u32 chance = fs_move(move)->secondaryEffectChance;
    if (ACT(s, side)->ability == ABILITY_SERENE_GRACE) chance *= 2;
    if (chance == 0) return 0;
    if (chance >= 100) return 1;
    return fs_roll(s, 100) < chance;
}

static void RecoilFromDamage(fs_state *s, int side, s32 dealt, int frac)
{
    fs_battler *a = ACT(s, side);
    s32 r = dealt / frac;
    if (a->ability == ABILITY_ROCK_HEAD || !a->present) return;
    if (r == 0) r = 1;
    Damage(s, side, r);
}

static void Drain(fs_state *s, int side, s32 dealt)
{
    fs_battler *t = ACT(s, OPP(side));
    s32 h = dealt / 2;
    if (h == 0) h = 1;
    if (t->ability == ABILITY_LIQUID_OOZE) Damage(s, side, h); else Heal(s, side, h);
}

static void Weather(fs_state *s, u8 w, u8 turns) { s->weather = w; s->weatherTurns = turns; }

// ---------------------------------------------------------------------------------------------------------
// cancellers: returns 0 if the move is cancelled this turn

static int Cancellers(fs_state *s, int side, u16 move)
{
    fs_battler *a = ACT(s, side);
    const struct BattleMove *bm = fs_move(move);
    a->vol &= ~FS_V_DESTINY_BOND;
    if (a->status1 & FS_S1_SLEEP)
    {
        u8 toSub = a->ability == ABILITY_EARLY_BIRD ? 2 : 1, cnt = a->status1 & FS_S1_SLEEP;
        if (cnt < toSub) a->status1 &= ~FS_S1_SLEEP; else a->status1 -= toSub;
        if (a->status1 & FS_S1_SLEEP) return 0;   // Snore / Sleep Talk are unsupported
        a->vol &= ~FS_V_NIGHTMARE;
    }
    if (a->status1 & FS_S1_FRZ)
    {
        if (fs_chance(s, 1, 5)) a->status1 &= ~FS_S1_FRZ;
        else if (bm->effect != EFFECT_THAW_HIT) return 0;
    }
    if (a->ability == ABILITY_TRUANT && a->truantCounter) return 0;
    if (a->vol & FS_V_RECHARGE) { a->vol &= ~FS_V_RECHARGE; a->rechargeTimer = 0; return 0; }
    if (a->vol & FS_V_FLINCH) { a->vol &= ~FS_V_FLINCH; return 0; }
    if (a->disableTimer && move == a->moves[a->disabledPos]) return 0;
    if (a->tauntTimer && bm->power == 0) return 0;
    if (a->vol & FS_V_CONFUSED)
    {
        a->confusionTurns--;
        if (a->confusionTurns)
        {
            if (fs_chance(s, 1, 2))
            {
                // hits itself: 40-power typeless physical hit, no crit, no STAB, no type
                s32 dmg = fs_base_damage(s, side, side, MOVE_POUND, 40, TYPE_NORMAL, 0);
                dmg = fs_random_roll(s, dmg);
                if (a->vol & FS_V_SUBSTITUTE) { if (dmg >= a->substituteHP) { a->substituteHP = 0; a->vol &= ~FS_V_SUBSTITUTE; } else a->substituteHP -= dmg; }
                else Damage(s, side, dmg);
                return 0;
            }
        }
        else a->vol &= ~FS_V_CONFUSED;
    }
    if ((a->status1 & FS_S1_PAR) && fs_chance(s, 1, 4)) return 0;
    if ((a->vol & FS_V_INFATUATED) && fs_chance(s, 1, 2)) return 0;
    if ((a->status1 & FS_S1_FRZ) && bm->effect == EFFECT_THAW_HIT) a->status1 &= ~FS_S1_FRZ;
    return 1;
}

// ---------------------------------------------------------------------------------------------------------
// the move

static u16 MovePower(fs_state *s, int side, u16 move, u8 *type)
{
    fs_battler *a = ACT(s, side), *t = ACT(s, OPP(side));
    const struct BattleMove *bm = fs_move(move);
    u16 power = bm->power;
    *type = bm->type;
    switch (bm->effect)
    {
    case EFFECT_HIDDEN_POWER: *type = a->hpTypeCache; power = s->side[side].party[a->monIdx].hpPower; break;
    case EFFECT_RETURN: power = 10 * a->level / 25; { u8 f = s->side[side].party[a->monIdx].friendship; power = 10 * f / 25; } if (power == 0) power = 1; break;
    case EFFECT_FRUSTRATION: { u8 f = s->side[side].party[a->monIdx].friendship; power = 10 * (255 - f) / 25; if (power == 0) power = 1; } break;
    case EFFECT_FACADE: if (a->status1 & (FS_S1_PSN | FS_S1_TOX | FS_S1_BRN | FS_S1_PAR)) power *= 2; break;
    case EFFECT_FLAIL:
    {
        u32 r = a->hp * 48 / a->maxHP;
        power = r < 2 ? 200 : r < 5 ? 150 : r < 10 ? 100 : r < 17 ? 80 : r < 33 ? 40 : 20;
        break;
    }
    case EFFECT_ERUPTION: power = bm->power * a->hp / a->maxHP; if (power == 0) power = 1; break;
    case EFFECT_LOW_KICK:
    {
        u32 w = GetPokedexHeightWeight(SpeciesToNationalPokedexNum(t->species), 1);
        static const u16 table[] = {100, 20, 250, 40, 500, 60, 1000, 80, 2000, 100, 0xFFFF, 120};
        int i;
        power = 120;
        for (i = 0; i < 12; i += 2) if (w < table[i]) { power = table[i + 1]; break; }
        break;
    }
    case EFFECT_MAGNITUDE:
    {
        u32 r = fs_roll(s, 100);
        power = r < 5 ? 10 : r < 15 ? 30 : r < 35 ? 50 : r < 65 ? 70 : r < 85 ? 90 : r < 95 ? 110 : 150;
        break;
    }
    case EFFECT_WEATHER_BALL:
        if (s->weather && fs_weather_active(s))
        {
            power = 100;
            *type = s->weather == FS_WEATHER_RAIN ? TYPE_WATER : s->weather == FS_WEATHER_SUN ? TYPE_FIRE : s->weather == FS_WEATHER_SAND ? TYPE_ROCK : TYPE_ICE;
        }
        break;
    case EFFECT_REVENGE: if (a->lastLandedMove && (t->vol & FS_V_MOVED_THIS_TURN) && a->bideDmg) power *= 2; break;
    case EFFECT_SMELLINGSALT: if (t->status1 & FS_S1_PAR) power *= 2; break;
    case EFFECT_EARTHQUAKE: if (t->vol & FS_V_UNDERGROUND) power *= 2; break;
    case EFFECT_GUST: case EFFECT_TWISTER: if (t->vol & FS_V_ON_AIR) power *= 2; break;
    case EFFECT_FLINCH_MINIMIZE_HIT: if (t->vol & FS_V_MINIMIZED) power *= 2; break;
    }
    return power;
}

static void StatUpMove(fs_state *s, int side, int stat, int delta) { ChangeStage(s, side, stat, delta, 0, 1); }

static int StatDownMove(fs_state *s, int side, u16 move, int stat, int delta)
{
    int opp = OPP(side);
    fs_battler *t = ACT(s, opp);
    if (!t->present) return 0;
    if ((t->vol & FS_V_PROTECTED) && (fs_move(move)->flags & FLAG_PROTECT_AFFECTED)) return 0;
    if (t->vol & FS_V_SUBSTITUTE) return 0;
    if (!fs_accuracy_check(s, side, opp, move, fs_move(move)->type)) return 0;
    return ChangeStage(s, opp, stat, delta, 1, 0);
}

static int StatusMove(fs_state *s, int side, u16 move, u16 status)
{
    int opp = OPP(side);
    fs_battler *t = ACT(s, opp);
    const struct BattleMove *bm = fs_move(move);
    if (!t->present) return 0;
    if ((t->vol & FS_V_PROTECTED) && (bm->flags & FLAG_PROTECT_AFFECTED)) return 0;
    if (t->vol & FS_V_SUBSTITUTE) return 0;
    if (t->status1) return 0;
    if (status == FS_S1_PAR && move == MOVE_THUNDER_WAVE && fs_type_mult(TYPE_ELECTRIC, t->type1, t->type2, 0) == 0) return 0;
    if (t->ability == ABILITY_SOUNDPROOF && IsSoundMove(move)) return 0;
    if (status == FS_S1_TOX && IsType(ACT(s, side), TYPE_POISON)) { if (!fs_accuracy_check(s, side, opp, move, bm->type)) return 0; }
    else if (!fs_accuracy_check(s, side, opp, move, bm->type)) return 0;
    return TryStatus(s, opp, status, 1, side, 0);
}

static void SecondaryStatus(fs_state *s, int side, u16 move, u16 status, struct Hit *h)
{
    if (!h->hit || !ACT(s, OPP(side))->present) return;
    if (ACT(s, OPP(side))->vol & FS_V_SUBSTITUTE) return;
    if (SecondaryRoll(s, side, move)) TryStatus(s, OPP(side), status, 1, side, 1);
}

void fs_use_move(fs_state *s, int side, int slot)
{
    int opp = OPP(side);
    fs_battler *a = ACT(s, side), *t = ACT(s, opp);
    u16 move = slot == 4 ? MOVE_STRUGGLE : a->moves[slot];
    const struct BattleMove *bm = fs_move(move);
    u8 type;
    u16 power;
    struct Hit h = {0, 0, 0, 10};
    int contact = (bm->flags & FLAG_MAKES_CONTACT) != 0;
    if (move == MOVE_NONE) return;
    if (!Cancellers(s, side, move)) { a->vol |= FS_V_MOVED_THIS_TURN; a->chosenMove = 0; return; }
    a->vol |= FS_V_MOVED_THIS_TURN;
    a->lastMove = move;
    if (fs_hold_effect(a->item) == HOLD_EFFECT_CHOICE_BAND && move != MOVE_STRUGGLE && !a->choicedMove && bm->effect != EFFECT_BATON_PASS) a->choicedMove = move;
    if (slot < 4 && a->pp[slot])
    {
        u8 dec = 1;
        if (t->present && t->ability == ABILITY_PRESSURE) dec++;
        a->pp[slot] = a->pp[slot] > dec ? a->pp[slot] - dec : 0;
    }
    if (a->protectUses && bm->effect != EFFECT_PROTECT && bm->effect != EFFECT_ENDURE) a->protectUses = 0;
    power = MovePower(s, side, move, &type);
    if (!fs_effect_supported(bm->effect)) { s->unsupported |= FS_UNSUP_MOVE_EFFECT; return; }
    switch (bm->effect)
    {
    // ---- plain and secondary-effect hits
    case EFFECT_HIT: case EFFECT_PAY_DAY: case EFFECT_HIGH_CRITICAL: case EFFECT_QUICK_ATTACK: case EFFECT_VITAL_THROW: case EFFECT_HIDDEN_POWER:
    case EFFECT_RETURN: case EFFECT_FRUSTRATION: case EFFECT_FACADE: case EFFECT_FLAIL: case EFFECT_ERUPTION: case EFFECT_LOW_KICK: case EFFECT_MAGNITUDE:
    case EFFECT_WEATHER_BALL: case EFFECT_REVENGE: case EFFECT_EARTHQUAKE: case EFFECT_GUST: case EFFECT_TWISTER: case EFFECT_SKY_UPPERCUT:
        h = AttackHit(s, side, move, power, type, 0, 0, 0);
        AfterHit(s, side, move, &h, contact);
        if (bm->effect == EFFECT_TWISTER && h.hit && h.dmg && SecondaryRoll(s, side, move) && t->present && !(t->vol & FS_V_SUBSTITUTE) && t->ability != ABILITY_INNER_FOCUS) t->vol |= FS_V_FLINCH;
        break;
    case EFFECT_ALWAYS_HIT: h = AttackHit(s, side, move, power, type, 1, 0, 0); AfterHit(s, side, move, &h, contact); break;
    case EFFECT_FALSE_SWIPE: h = AttackHit(s, side, move, power, type, 0, 1, 0); AfterHit(s, side, move, &h, contact); break;
    case EFFECT_POISON_HIT: case EFFECT_POISON_TAIL: h = AttackHit(s, side, move, power, type, 0, 0, 0); AfterHit(s, side, move, &h, contact); SecondaryStatus(s, side, move, FS_S1_PSN, &h); break;
    case EFFECT_POISON_FANG: h = AttackHit(s, side, move, power, type, 0, 0, 0); AfterHit(s, side, move, &h, contact); SecondaryStatus(s, side, move, FS_S1_TOX, &h); break;
    case EFFECT_BURN_HIT: case EFFECT_BLAZE_KICK: h = AttackHit(s, side, move, power, type, 0, 0, 0); AfterHit(s, side, move, &h, contact); SecondaryStatus(s, side, move, FS_S1_BRN, &h); break;
    case EFFECT_THAW_HIT: h = AttackHit(s, side, move, power, type, 0, 0, 0); AfterHit(s, side, move, &h, contact); SecondaryStatus(s, side, move, FS_S1_BRN, &h); break;
    case EFFECT_FREEZE_HIT: h = AttackHit(s, side, move, power, type, 0, 0, 0); AfterHit(s, side, move, &h, contact); SecondaryStatus(s, side, move, FS_S1_FRZ, &h); break;
    case EFFECT_PARALYZE_HIT: case EFFECT_THUNDER: h = AttackHit(s, side, move, power, type, 0, 0, 0); AfterHit(s, side, move, &h, contact); SecondaryStatus(s, side, move, FS_S1_PAR, &h); break;
    case EFFECT_TRI_ATTACK:
        h = AttackHit(s, side, move, power, type, 0, 0, 0); AfterHit(s, side, move, &h, contact);
        if (h.hit && t->present && !(t->vol & FS_V_SUBSTITUTE) && SecondaryRoll(s, side, move))
        {
            u32 r = fs_roll(s, 3);
            TryStatus(s, opp, r == 0 ? FS_S1_PAR : r == 1 ? FS_S1_BRN : FS_S1_FRZ, 1, side, 1);
        }
        break;
    case EFFECT_FLINCH_HIT: case EFFECT_FLINCH_MINIMIZE_HIT: case EFFECT_FAKE_OUT:
        if (bm->effect == EFFECT_FAKE_OUT && a->isFirstTurn == 0) break;
        h = AttackHit(s, side, move, power, type, 0, 0, 0); AfterHit(s, side, move, &h, contact);
        if (h.hit && h.dmg && t->present && !(t->vol & FS_V_SUBSTITUTE) && t->ability != ABILITY_INNER_FOCUS && t->ability != ABILITY_SHIELD_DUST && SecondaryRoll(s, side, move)) t->vol |= FS_V_FLINCH;
        break;
    case EFFECT_CONFUSE_HIT:
        h = AttackHit(s, side, move, power, type, 0, 0, 0); AfterHit(s, side, move, &h, contact);
        if (h.hit && h.dmg && SecondaryRoll(s, side, move)) TryConfuse(s, opp, 1, 1);
        break;
    case EFFECT_ATTACK_DOWN_HIT: case EFFECT_DEFENSE_DOWN_HIT: case EFFECT_SPEED_DOWN_HIT: case EFFECT_SPECIAL_ATTACK_DOWN_HIT:
    case EFFECT_SPECIAL_DEFENSE_DOWN_HIT: case EFFECT_ACCURACY_DOWN_HIT: case EFFECT_EVASION_DOWN_HIT:
    {
        int stat = STAT_ATK_ + (bm->effect - EFFECT_ATTACK_DOWN_HIT);
        h = AttackHit(s, side, move, power, type, 0, 0, 0); AfterHit(s, side, move, &h, contact);
        if (h.hit && h.dmg && t->present && t->ability != ABILITY_SHIELD_DUST && SecondaryRoll(s, side, move)) ChangeStage(s, opp, stat, -1, 1, 0);
        break;
    }
    case EFFECT_DEFENSE_UP_HIT: case EFFECT_ATTACK_UP_HIT:
        h = AttackHit(s, side, move, power, type, 0, 0, 0); AfterHit(s, side, move, &h, contact);
        if (h.hit && SecondaryRoll(s, side, move)) ChangeStage(s, side, bm->effect == EFFECT_DEFENSE_UP_HIT ? STAT_DEF_ : STAT_ATK_, 1, 0, 1);
        break;
    case EFFECT_ALL_STATS_UP_HIT:
        h = AttackHit(s, side, move, power, type, 0, 0, 0); AfterHit(s, side, move, &h, contact);
        if (h.hit && SecondaryRoll(s, side, move)) { int st; for (st = STAT_ATK_; st <= STAT_SPDEF_; st++) ChangeStage(s, side, st, 1, 0, 1); }
        break;
    case EFFECT_RECOIL: case EFFECT_DOUBLE_EDGE:
        h = AttackHit(s, side, move, power, type, 0, 0, 0); AfterHit(s, side, move, &h, contact);
        if (h.hit && h.dmg) RecoilFromDamage(s, side, h.dmg, bm->effect == EFFECT_DOUBLE_EDGE ? 3 : 4);
        break;
    case EFFECT_RECOIL_IF_MISS:
        h = AttackHit(s, side, move, power, type, 0, 0, 0); AfterHit(s, side, move, &h, contact);
        if (!h.hit || h.mult == 0) { s32 d = fs_base_damage(s, side, opp, move, power, type, 0) / 2; if (d < 1) d = 1; if (t->present) Damage(s, side, d); }
        break;
    case EFFECT_ABSORB: case EFFECT_DREAM_EATER:
        if (bm->effect == EFFECT_DREAM_EATER && !(t->status1 & FS_S1_SLEEP)) break;
        h = AttackHit(s, side, move, power, type, 0, 0, 0); AfterHit(s, side, move, &h, contact);
        if (h.hit && h.dmg) Drain(s, side, h.dmg);
        break;
    case EFFECT_EXPLOSION:
        if (t->present && t->ability == ABILITY_DAMP) break;
        a->hp = 0;
        h = AttackHit(s, side, move, power, type, 0, 0, 0); AfterHit(s, side, move, &h, 0);
        fs_faint(s, side);
        break;
    case EFFECT_MULTI_HIT: case EFFECT_DOUBLE_HIT: case EFFECT_TWINEEDLE: case EFFECT_TRIPLE_KICK:
    {
        int hits, i;
        if (bm->effect == EFFECT_MULTI_HIT) { u32 r = fs_roll(s, 4); hits = r < 2 ? r + 2 : (fs_roll(s, 4) < 2 ? 2 : 3); if (r >= 2) hits = 2 + fs_roll(s, 4); if (hits > 5) hits = 5; }
        else if (bm->effect == EFFECT_TRIPLE_KICK) hits = 3;
        else hits = 2;
        // the game: MULTI_HIT rolls Random()%4: 0,1 -> 2,3 hits; else Random()%4 again + 2 (2..5)
        if (bm->effect == EFFECT_MULTI_HIT) { u32 r = fs_roll(s, 4); hits = (r < 2) ? r + 2 : (int)fs_roll(s, 4) + 2; }
        for (i = 0; i < hits && t->present; i++)
        {
            u16 p = bm->effect == EFFECT_TRIPLE_KICK ? power * (i + 1) : power;
            struct Hit hh = AttackHit(s, side, move, p, type, i > 0, 0, 0);
            if (!hh.hit) break;
            AfterHit(s, side, move, &hh, contact);
            if (bm->effect == EFFECT_TWINEEDLE && hh.dmg && t->present && !(t->vol & FS_V_SUBSTITUTE) && SecondaryRoll(s, side, move)) TryStatus(s, opp, FS_S1_PSN, 1, side, 1);
            h = hh;
        }
        break;
    }
    case EFFECT_RECHARGE:
        h = AttackHit(s, side, move, power, type, 0, 0, 0); AfterHit(s, side, move, &h, contact);
        if (h.hit) { a->vol |= FS_V_RECHARGE; a->rechargeTimer = 1; }
        break;
    case EFFECT_SUPERPOWER:
        h = AttackHit(s, side, move, power, type, 0, 0, 0); AfterHit(s, side, move, &h, contact);
        if (h.hit) { ChangeStage(s, side, STAT_ATK_, -1, 0, 1); ChangeStage(s, side, STAT_DEF_, -1, 0, 1); }
        break;
    case EFFECT_OVERHEAT:
        h = AttackHit(s, side, move, power, type, 0, 0, 0); AfterHit(s, side, move, &h, contact);
        if (h.hit) ChangeStage(s, side, STAT_SPATK_, -2, 0, 1);
        break;
    case EFFECT_BRICK_BREAK:
        if (t->present) { s->side[opp].reflect = 0; s->side[opp].lightscreen = 0; }
        h = AttackHit(s, side, move, power, type, 0, 0, 0); AfterHit(s, side, move, &h, contact);
        break;
    case EFFECT_KNOCK_OFF:
        h = AttackHit(s, side, move, power, type, 0, 0, 0); AfterHit(s, side, move, &h, contact);
        if (h.hit && h.dmg && t->present && t->item && t->ability != ABILITY_STICKY_HOLD) { t->item = 0; t->choicedMove = 0; s->side[opp].party[t->monIdx].item = 0; }
        break;
    case EFFECT_THIEF:
        h = AttackHit(s, side, move, power, type, 0, 0, 0); AfterHit(s, side, move, &h, contact);
        if (h.hit && h.dmg && t->present && t->item && !a->item && t->ability != ABILITY_STICKY_HOLD) { a->item = t->item; t->item = 0; t->choicedMove = 0; a->choicedMove = 0; s->side[opp].party[t->monIdx].item = 0; s->side[side].party[a->monIdx].item = a->item; }
        break;
    case EFFECT_RAPID_SPIN:
        h = AttackHit(s, side, move, power, type, 0, 0, 0); AfterHit(s, side, move, &h, contact);
        if (h.hit) { a->vol &= ~FS_V_LEECH_SEED; a->wrapTurns = 0; s->side[side].spikes = 0; }
        break;
    case EFFECT_TRAP:
        h = AttackHit(s, side, move, power, type, 0, 0, 0); AfterHit(s, side, move, &h, contact);
        if (h.hit && h.dmg && t->present && !t->wrapTurns) { t->wrapTurns = 2 + fs_roll(s, 4); t->wrapMove = move; }
        break;
    case EFFECT_FOCUS_PUNCH:
        if (a->bideDmg) break;   // hit this turn before moving (bideDmg doubles as "took damage this turn")
        h = AttackHit(s, side, move, power, type, 0, 0, 0); AfterHit(s, side, move, &h, contact);
        break;
    // ---- fixed damage
    case EFFECT_LEVEL_DAMAGE: case EFFECT_PSYWAVE: case EFFECT_DRAGON_RAGE: case EFFECT_SONICBOOM: case EFFECT_SUPER_FANG: case EFFECT_ENDEAVOR: case EFFECT_OHKO:
    {
        s32 dmg;
        if (!t->present) break;
        if ((t->vol & FS_V_PROTECTED) && (bm->flags & FLAG_PROTECT_AFFECTED)) break;
        if (bm->effect == EFFECT_OHKO)
        {
            u32 acc;
            if (t->ability == ABILITY_STURDY || a->level < t->level) break;
            if (fs_type_mult(type, t->type1, t->type2, 0) == 0) break;
            acc = bm->accuracy + a->level - t->level;
            if (!(fs_roll(s, 100) + 1 <= acc)) break;
            dmg = t->hp;
        }
        else
        {
            if (!fs_accuracy_check(s, side, opp, move, type)) break;
            if (fs_type_mult(type, t->type1, t->type2, (t->vol & FS_V_FORESIGHT) != 0) == 0) break;
            switch (bm->effect)
            {
            case EFFECT_LEVEL_DAMAGE: dmg = a->level; break;
            case EFFECT_PSYWAVE: dmg = a->level * (fs_roll(s, 11) + 5) / 10; break;
            case EFFECT_DRAGON_RAGE: dmg = 40; break;
            case EFFECT_SONICBOOM: dmg = 20; break;
            case EFFECT_SUPER_FANG: dmg = t->hp / 2; break;
            default: dmg = t->hp > a->hp ? t->hp - a->hp : 0; break;
            }
            if (dmg == 0 && bm->effect != EFFECT_ENDEAVOR) dmg = 1;
            if (dmg == 0) break;
        }
        if (t->vol & FS_V_SUBSTITUTE) { if (dmg >= t->substituteHP) { t->substituteHP = 0; t->vol &= ~FS_V_SUBSTITUTE; } else t->substituteHP -= dmg; break; }
        if (dmg > t->hp) dmg = t->hp;
        t->lastLandedMove = move; t->lastHitByType = type;
        Damage(s, opp, dmg);
        h.hit = 1; h.dmg = dmg;
        AfterHit(s, side, move, &h, contact);
        break;
    }
    case EFFECT_COUNTER: case EFFECT_MIRROR_COAT:
    {
        int phys = bm->effect == EFFECT_COUNTER;
        if (!t->present || !a->bideDmg) break;
        if (!(phys ? (a->lastHitByType < TYPE_MYSTERY) : (a->lastHitByType > TYPE_MYSTERY))) break;
        if (!fs_accuracy_check(s, side, opp, move, type)) break;
        {
            s32 dmg = a->bideDmg * 2;
            if (t->vol & FS_V_SUBSTITUTE) { if (dmg >= t->substituteHP) { t->substituteHP = 0; t->vol &= ~FS_V_SUBSTITUTE; } else t->substituteHP -= dmg; break; }
            if (dmg > t->hp) dmg = t->hp;
            Damage(s, opp, dmg);
        }
        break;
    }
    // ---- stat moves
    case EFFECT_ATTACK_UP: case EFFECT_DEFENSE_UP: case EFFECT_SPEED_UP: case EFFECT_SPECIAL_ATTACK_UP: case EFFECT_SPECIAL_DEFENSE_UP: case EFFECT_ACCURACY_UP: case EFFECT_EVASION_UP:
        StatUpMove(s, side, STAT_ATK_ + (bm->effect - EFFECT_ATTACK_UP), 1); break;
    case EFFECT_ATTACK_UP_2: case EFFECT_DEFENSE_UP_2: case EFFECT_SPEED_UP_2: case EFFECT_SPECIAL_ATTACK_UP_2: case EFFECT_SPECIAL_DEFENSE_UP_2: case EFFECT_ACCURACY_UP_2: case EFFECT_EVASION_UP_2:
        StatUpMove(s, side, STAT_ATK_ + (bm->effect - EFFECT_ATTACK_UP_2), 2); break;
    case EFFECT_ATTACK_DOWN: case EFFECT_DEFENSE_DOWN: case EFFECT_SPEED_DOWN: case EFFECT_SPECIAL_ATTACK_DOWN: case EFFECT_SPECIAL_DEFENSE_DOWN: case EFFECT_ACCURACY_DOWN: case EFFECT_EVASION_DOWN:
        StatDownMove(s, side, move, STAT_ATK_ + (bm->effect - EFFECT_ATTACK_DOWN), -1); break;
    case EFFECT_ATTACK_DOWN_2: case EFFECT_DEFENSE_DOWN_2: case EFFECT_SPEED_DOWN_2: case EFFECT_SPECIAL_ATTACK_DOWN_2: case EFFECT_SPECIAL_DEFENSE_DOWN_2: case EFFECT_ACCURACY_DOWN_2: case EFFECT_EVASION_DOWN_2:
        StatDownMove(s, side, move, STAT_ATK_ + (bm->effect - EFFECT_ATTACK_DOWN_2), -2); break;
    case EFFECT_TICKLE: if (StatDownMove(s, side, move, STAT_ATK_, -1)) ChangeStage(s, opp, STAT_DEF_, -1, 1, 0); break;
    case EFFECT_COSMIC_POWER: StatUpMove(s, side, STAT_DEF_, 1); StatUpMove(s, side, STAT_SPDEF_, 1); break;
    case EFFECT_BULK_UP: StatUpMove(s, side, STAT_ATK_, 1); StatUpMove(s, side, STAT_DEF_, 1); break;
    case EFFECT_CALM_MIND: StatUpMove(s, side, STAT_SPATK_, 1); StatUpMove(s, side, STAT_SPDEF_, 1); break;
    case EFFECT_DRAGON_DANCE: StatUpMove(s, side, STAT_ATK_, 1); StatUpMove(s, side, STAT_SPEED_, 1); break;
    case EFFECT_DEFENSE_CURL: StatUpMove(s, side, STAT_DEF_, 1); a->vol |= FS_V_DEFENSE_CURL; break;
    case EFFECT_MINIMIZE: StatUpMove(s, side, STAT_EVASION_, 1); a->vol |= FS_V_MINIMIZED; break;
    case EFFECT_BELLY_DRUM:
        if (a->hp > a->maxHP / 2 && a->stages[STAT_ATK_] < 12) { Damage(s, side, a->maxHP / 2); a->stages[STAT_ATK_] = 12; }
        break;
    case EFFECT_CURSE:
        if (IsType(a, TYPE_GHOST))
        {
            if (t->present && !(t->vol & FS_V_CURSED) && !((t->vol & FS_V_PROTECTED)) && !(t->vol & FS_V_SUBSTITUTE)) { t->vol |= FS_V_CURSED; Damage(s, side, a->maxHP / 2); }
        }
        else { ChangeStage(s, side, STAT_SPEED_, -1, 0, 1); ChangeStage(s, side, STAT_ATK_, 1, 0, 1); ChangeStage(s, side, STAT_DEF_, 1, 0, 1); }
        break;
    case EFFECT_SWAGGER: case EFFECT_FLATTER:
        if (!t->present || (t->vol & FS_V_SUBSTITUTE) || ((t->vol & FS_V_PROTECTED))) break;
        if (!fs_accuracy_check(s, side, opp, move, type)) break;
        ChangeStage(s, opp, bm->effect == EFFECT_SWAGGER ? STAT_ATK_ : STAT_SPATK_, bm->effect == EFFECT_SWAGGER ? 2 : 1, 1, 1);
        TryConfuse(s, opp, 1, 0);
        break;
    case EFFECT_PSYCH_UP: if (t->present) memcpy(a->stages, t->stages, sizeof(a->stages)); break;
    case EFFECT_HAZE: { int st; for (st = 0; st < FS_STAGES; st++) { a->stages[st] = 6; t->stages[st] = 6; } break; }
    case EFFECT_FOCUS_ENERGY: a->vol |= FS_V_FOCUS_ENERGY; break;
    // ---- status moves
    case EFFECT_SLEEP: StatusMove(s, side, move, FS_S1_SLEEP); break;
    case EFFECT_POISON: StatusMove(s, side, move, FS_S1_PSN); break;
    case EFFECT_TOXIC: StatusMove(s, side, move, FS_S1_TOX); break;
    case EFFECT_PARALYZE: StatusMove(s, side, move, FS_S1_PAR); break;
    case EFFECT_WILL_O_WISP: StatusMove(s, side, move, FS_S1_BRN); break;
    case EFFECT_CONFUSE: case EFFECT_TEETER_DANCE:
        if (!t->present || (t->vol & FS_V_SUBSTITUTE) || ((t->vol & FS_V_PROTECTED) && (bm->flags & FLAG_PROTECT_AFFECTED))) break;
        if (t->ability == ABILITY_SOUNDPROOF && IsSoundMove(move)) break;
        if (!fs_accuracy_check(s, side, opp, move, type)) break;
        TryConfuse(s, opp, 1, 0);
        break;
    case EFFECT_ATTRACT:
        if (!t->present || (t->vol & FS_V_INFATUATED) || a->gender == t->gender || a->gender == 0xFF || t->gender == 0xFF || t->ability == ABILITY_OBLIVIOUS) break;
        if ((t->vol & FS_V_PROTECTED) || !fs_accuracy_check(s, side, opp, move, type)) break;
        t->vol |= FS_V_INFATUATED;
        break;
    case EFFECT_YAWN:
        if (!t->present || (t->vol & FS_V_SUBSTITUTE) || (t->vol & FS_V_PROTECTED) || t->status1 || t->yawnTimer) break;
        if (t->ability == ABILITY_INSOMNIA || t->ability == ABILITY_VITAL_SPIRIT || s->side[opp].safeguard) break;
        if (!fs_accuracy_check(s, side, opp, move, type)) break;
        t->yawnTimer = 2;
        break;
    case EFFECT_LEECH_SEED:
        if (!t->present || (t->vol & FS_V_SUBSTITUTE) || (t->vol & FS_V_PROTECTED) || (t->vol & FS_V_LEECH_SEED) || IsType(t, TYPE_GRASS)) break;
        if (!fs_accuracy_check(s, side, opp, move, type)) break;
        t->vol |= FS_V_LEECH_SEED;
        break;
    case EFFECT_NIGHTMARE:
        if (!t->present || !(t->status1 & FS_S1_SLEEP) || (t->vol & FS_V_NIGHTMARE) || (t->vol & FS_V_SUBSTITUTE) || (t->vol & FS_V_PROTECTED)) break;
        t->vol |= FS_V_NIGHTMARE;
        break;
    case EFFECT_MEAN_LOOK:
        if (!t->present || (t->vol & FS_V_ESCAPE_PREV) || (t->vol & FS_V_PROTECTED) || (t->vol & FS_V_SUBSTITUTE)) break;
        if (!fs_accuracy_check(s, side, opp, move, type)) break;
        t->vol |= FS_V_ESCAPE_PREV;
        break;
    case EFFECT_FORESIGHT:
        if (!t->present || (t->vol & FS_V_PROTECTED) || (t->vol & FS_V_SUBSTITUTE)) break;
        if (!fs_accuracy_check(s, side, opp, move, type)) break;
        t->vol |= FS_V_FORESIGHT;
        break;
    case EFFECT_TORMENT:
        if (!t->present || (t->vol & FS_V_TORMENT) || (t->vol & FS_V_PROTECTED)) break;
        if (!fs_accuracy_check(s, side, opp, move, type)) break;
        t->vol |= FS_V_TORMENT;
        break;
    case EFFECT_TAUNT:
        if (!t->present || t->tauntTimer || (t->vol & FS_V_PROTECTED)) break;
        if (!fs_accuracy_check(s, side, opp, move, type)) break;
        t->tauntTimer = 2;
        break;
    case EFFECT_DISABLE:
    {
        int i;
        if (!t->present || t->disableTimer || !t->lastMove || (t->vol & FS_V_PROTECTED)) break;
        for (i = 0; i < 4; i++) if (t->moves[i] == t->lastMove && t->pp[i]) break;
        if (i == 4) break;
        if (!fs_accuracy_check(s, side, opp, move, type)) break;
        t->disableTimer = 2 + fs_roll(s, 4);
        t->disabledPos = i;
        break;
    }
    case EFFECT_ENCORE:
    {
        int i;
        if (!t->present || t->encoreTimer || !t->lastMove || (t->vol & FS_V_PROTECTED)) break;
        if (t->lastMove == MOVE_STRUGGLE || t->lastMove == MOVE_ENCORE || t->lastMove == MOVE_MIRROR_MOVE) break;
        for (i = 0; i < 4; i++) if (t->moves[i] == t->lastMove && t->pp[i]) break;
        if (i == 4) break;
        if (!fs_accuracy_check(s, side, opp, move, type)) break;
        t->encoreTimer = 2 + fs_roll(s, 5);
        t->encoredPos = i;
        break;
    }
    case EFFECT_PERISH_SONG:
        if (!a->perishTimer && a->ability != ABILITY_SOUNDPROOF) a->perishTimer = 4;
        if (t->present && !t->perishTimer && t->ability != ABILITY_SOUNDPROOF) t->perishTimer = 4;
        break;
    case EFFECT_ROAR:
        if (!t->present || (t->vol & FS_V_PROTECTED) || t->ability == ABILITY_SUCTION_CUPS || (t->vol & FS_V_ROOTED)) break;
        if (fs_alive_count(s, opp) < 2) break;
        if (t->ability == ABILITY_SOUNDPROOF && IsSoundMove(move)) break;
        if (!fs_accuracy_check(s, side, opp, move, type)) break;
        ForceSwitchRandom(s, opp);
        break;
    // ---- self / field
    case EFFECT_RESTORE_HP: case EFFECT_SOFTBOILED: Heal(s, side, a->maxHP / 2); break;
    case EFFECT_MORNING_SUN: case EFFECT_SYNTHESIS: case EFFECT_MOONLIGHT:
    {
        s32 amt = a->maxHP / 2;
        if (fs_weather_active(s) && s->weather == FS_WEATHER_SUN) amt = 20 * a->maxHP / 30;
        else if (fs_weather_active(s) && s->weather) amt = a->maxHP / 4;
        Heal(s, side, amt);
        break;
    }
    case EFFECT_REST:
        if (a->hp == a->maxHP || a->ability == ABILITY_INSOMNIA || a->ability == ABILITY_VITAL_SPIRIT) break;
        a->status1 = 3; a->hp = a->maxHP; a->vol &= ~FS_V_NIGHTMARE;
        break;
    case EFFECT_REFRESH: a->status1 &= ~(FS_S1_PSN | FS_S1_TOX | FS_S1_BRN | FS_S1_PAR | FS_S1_TOXCTR); break;
    case EFFECT_HEAL_BELL:
    {
        int i;
        fs_side *sd = &s->side[side];
        for (i = 0; i < FS_PARTY; i++) if (sd->party[i].species && !(i == a->monIdx)) sd->party[i].status1 = 0;
        if (a->ability != ABILITY_SOUNDPROOF) a->status1 = 0;
        break;
    }
    case EFFECT_PROTECT: case EFFECT_ENDURE:
    {
        static const u16 odds[] = {0xFFFF, 0x7FFF, 0x3FFF, 0x1FFF, 0x0FFF, 0x07FF, 0x03FF, 0x01FF, 0x00FF, 0x007F, 0x003F, 0x001F, 0x000F, 0x0007, 0x0003, 0x0001, 0x0000};
        int idx = a->protectUses > 16 ? 16 : a->protectUses;
        if (t->present && (t->vol & FS_V_MOVED_THIS_TURN) == 0 && fs_who_strikes_first_ignoring_moves(s) == side) {}
        // fails if the user moves last this turn
        if (t->present && (t->vol & FS_V_MOVED_THIS_TURN)) { a->protectUses = 0; break; }
        if ((fs_rand(s) & 0xFFFF) > odds[idx]) { a->protectUses = 0; break; }
        a->vol |= bm->effect == EFFECT_PROTECT ? FS_V_PROTECTED : FS_V_ENDURED;
        a->protectUses++;
        break;
    }
    case EFFECT_SUBSTITUTE:
    {
        s32 cost = a->maxHP / 4;
        if (cost == 0) cost = 1;
        if ((a->vol & FS_V_SUBSTITUTE) || a->hp <= cost) break;
        a->hp -= cost;
        a->vol |= FS_V_SUBSTITUTE;
        a->substituteHP = cost;
        break;
    }
    case EFFECT_LIGHT_SCREEN: if (!s->side[side].lightscreen) s->side[side].lightscreen = 5; break;
    case EFFECT_REFLECT: if (!s->side[side].reflect) s->side[side].reflect = 5; break;
    case EFFECT_MIST: if (!s->side[side].mist) s->side[side].mist = 5; break;
    case EFFECT_SAFEGUARD: if (!s->side[side].safeguard) s->side[side].safeguard = 5; break;
    case EFFECT_SPIKES: if (s->side[opp].spikes < 3) s->side[opp].spikes++; break;
    case EFFECT_RAIN_DANCE: if (s->weather != FS_WEATHER_RAIN || s->weatherTurns != 0xFF) Weather(s, FS_WEATHER_RAIN, 5); break;
    case EFFECT_SUNNY_DAY: if (s->weather != FS_WEATHER_SUN || s->weatherTurns != 0xFF) Weather(s, FS_WEATHER_SUN, 5); break;
    case EFFECT_SANDSTORM: if (s->weather != FS_WEATHER_SAND || s->weatherTurns != 0xFF) Weather(s, FS_WEATHER_SAND, 5); break;
    case EFFECT_HAIL: if (s->weather != FS_WEATHER_HAIL) Weather(s, FS_WEATHER_HAIL, 5); break;
    case EFFECT_MUD_SPORT: a->vol |= FS_V_MUD_SPORT; break;
    case EFFECT_WATER_SPORT: a->vol |= FS_V_WATER_SPORT; break;
    case EFFECT_INGRAIN: a->vol |= FS_V_ROOTED; break;
    case EFFECT_WISH: if (!s->side[side].wishTurns) { s->side[side].wishTurns = 2; s->side[side].wishMon = a->monIdx; } break;
    case EFFECT_DESTINY_BOND: a->vol |= FS_V_DESTINY_BOND; break;
    case EFFECT_SPLASH: case EFFECT_TELEPORT: break;
    case EFFECT_MEMENTO:
        if (!t->present || (t->vol & FS_V_PROTECTED) || (t->vol & FS_V_SUBSTITUTE)) break;
        if (!fs_accuracy_check(s, side, opp, move, type)) break;
        ChangeStage(s, opp, STAT_ATK_, -2, 1, 0); ChangeStage(s, opp, STAT_SPATK_, -2, 1, 0);
        a->hp = 0; fs_faint(s, side);
        break;
    case EFFECT_PAIN_SPLIT:
    {
        s32 tot;
        if (!t->present || (t->vol & FS_V_SUBSTITUTE) || (t->vol & FS_V_PROTECTED)) break;
        tot = (a->hp + t->hp) / 2;
        a->hp = tot > a->maxHP ? a->maxHP : tot;
        t->hp = tot > t->maxHP ? t->maxHP : tot;
        break;
    }
    case EFFECT_FUTURE_SIGHT:
        if (s->side[opp].futureSightTurns) break;
        s->side[opp].futureSightTurns = 3;
        s->side[opp].futureSightMove = move;
        s->side[opp].futureSightFromSide = side;
        {
            // damage computed now against the current target, no crit, with the roll
            s32 dmg = t->present ? fs_base_damage(s, side, opp, move, power, type, 0) : 0;
            if (t->present) { if (IsType(a, type)) dmg = dmg * 15 / 10; dmg = dmg * fs_type_mult(type, t->type1, t->type2, 0) / 10; dmg = fs_random_roll(s, dmg); }
            s->side[opp].futureSightDmg = dmg;
        }
        break;
    case EFFECT_BATON_PASS:
    {
        // switch to a random other alive party member, keeping stages and volatile flags
        fs_side *sd = &s->side[side];
        int cand[FS_PARTY], n = 0, i;
        s8 stages[FS_STAGES]; u32 vol; u8 subHP, confusionTurns, perish;
        for (i = 0; i < FS_PARTY; i++) if (sd->party[i].species && sd->party[i].hp > 0 && i != a->monIdx) cand[n++] = i;
        if (n == 0) break;
        memcpy(stages, a->stages, sizeof(stages)); vol = a->vol; subHP = a->substituteHP; confusionTurns = a->confusionTurns; perish = a->perishTimer;
        fs_switch_in(s, side, cand[fs_roll(s, n)]);
        a = ACT(s, side);
        memcpy(a->stages, stages, sizeof(stages));
        a->vol |= vol & (FS_V_CONFUSED | FS_V_FOCUS_ENERGY | FS_V_SUBSTITUTE | FS_V_LEECH_SEED | FS_V_ROOTED | FS_V_CURSED | FS_V_ESCAPE_PREV | FS_V_MUD_SPORT | FS_V_WATER_SPORT);
        a->substituteHP = subHP; a->confusionTurns = confusionTurns; a->perishTimer = perish;
        break;
    }
    default:
        if (!fs_ext_use_move(s, side, slot, move, power, type, &h)) s->unsupported |= FS_UNSUP_MOVE_EFFECT;
        break;
    }
    // Struggle recoil
    if (move == MOVE_STRUGGLE && h.hit && h.dmg && a->present) Damage(s, side, h.dmg / 4 ? h.dmg / 4 : 1);
    // damage taken this turn (for Counter / Mirror Coat / Focus Punch / Revenge): recorded on the target
    if (h.hit && h.dmg && ACT(s, opp)->present) { ACT(s, opp)->bideDmg = h.dmg; }
    // Destiny Bond
    if (h.hit && h.dmg && !t->present && (t->vol & FS_V_DESTINY_BOND) && a->present) { a->hp = 0; fs_faint(s, side); }
    // berries that react to HP
    fs_end_turn_items(s, side); fs_end_turn_items(s, opp);
}

// ---------------------------------------------------------------------------------------------------------
// helpers for the extensions

int fs_change_stage(fs_state *s, int side, int stat, int delta, int byOpp, int ignoreSub) { return ChangeStage(s, side, stat, delta, byOpp, ignoreSub); }
int fs_try_status(fs_state *s, int side, u16 status, int byOpp, int attackerSide, int isSecondary) { return TryStatus(s, side, status, byOpp, attackerSide, isSecondary); }
int fs_try_confuse(fs_state *s, int side, int byOpp, int isSecondary) { return TryConfuse(s, side, byOpp, isSecondary); }
void fs_damage(fs_state *s, int side, s32 dmg) { Damage(s, side, dmg); }
void fs_heal(fs_state *s, int side, s32 amount) { Heal(s, side, amount); }
s32 fs_attack(fs_state *s, int side, u16 move, u16 power, u8 type, int noAcc, int falseSwipe, int noCrit)
{
    struct Hit h = AttackHit(s, side, move, power, type, noAcc, falseSwipe, noCrit);
    AfterHit(s, side, move, &h, (fs_move(move)->flags & FLAG_MAKES_CONTACT) != 0);
    if (h.hit && h.dmg && ACT(s, OPP(side))->present) ACT(s, OPP(side))->bideDmg = h.dmg;
    return h.hit ? h.dmg : 0;
}

// ---------------------------------------------------------------------------------------------------------
// switch-in abilities

void fs_switch_in_abilities(fs_state *s, int side)
{
    fs_battler *a = ACT(s, side), *t = ACT(s, OPP(side));
    switch (a->ability)
    {
    case ABILITY_INTIMIDATE:
        if (t->present && t->ability != ABILITY_CLEAR_BODY && t->ability != ABILITY_WHITE_SMOKE && t->ability != ABILITY_HYPER_CUTTER)
            ChangeStage(s, OPP(side), STAT_ATK_, -1, 0, 1);
        break;
    case ABILITY_DRIZZLE: if (!(s->weather == FS_WEATHER_RAIN && s->weatherTurns == 0xFF)) Weather(s, FS_WEATHER_RAIN, 0xFF); break;
    case ABILITY_DROUGHT: if (!(s->weather == FS_WEATHER_SUN && s->weatherTurns == 0xFF)) Weather(s, FS_WEATHER_SUN, 0xFF); break;
    case ABILITY_SAND_STREAM: if (!(s->weather == FS_WEATHER_SAND && s->weatherTurns == 0xFF)) Weather(s, FS_WEATHER_SAND, 0xFF); break;
    default: fs_ext_switch_in_ability(s, side); break;
    }
}

// ---------------------------------------------------------------------------------------------------------
// items that trigger on HP / status (checked after moves and at end of turn) and Leftovers

void fs_end_turn_items(fs_state *s, int side)
{
    fs_battler *a = ACT(s, side);
    u8 he, param;
    if (!a->present || a->hp == 0) return;
    he = fs_hold_effect(a->item); param = fs_hold_param(a->item);
    switch (he)
    {
    case HOLD_EFFECT_RESTORE_HP: if (a->hp <= a->maxHP / 2) { Heal(s, side, param); a->item = 0; } break;
    case HOLD_EFFECT_CURE_PAR: if (a->status1 & FS_S1_PAR) { a->status1 &= ~FS_S1_PAR; a->item = 0; } break;
    case HOLD_EFFECT_CURE_SLP: if (a->status1 & FS_S1_SLEEP) { a->status1 &= ~FS_S1_SLEEP; a->vol &= ~FS_V_NIGHTMARE; a->item = 0; } break;
    case HOLD_EFFECT_CURE_PSN: if (a->status1 & (FS_S1_PSN | FS_S1_TOX)) { a->status1 &= ~(FS_S1_PSN | FS_S1_TOX | FS_S1_TOXCTR); a->item = 0; } break;
    case HOLD_EFFECT_CURE_BRN: if (a->status1 & FS_S1_BRN) { a->status1 &= ~FS_S1_BRN; a->item = 0; } break;
    case HOLD_EFFECT_CURE_FRZ: if (a->status1 & FS_S1_FRZ) { a->status1 &= ~FS_S1_FRZ; a->item = 0; } break;
    case HOLD_EFFECT_CURE_CONFUSION: if (a->vol & FS_V_CONFUSED) { a->vol &= ~FS_V_CONFUSED; a->item = 0; } break;
    case HOLD_EFFECT_CURE_STATUS: if (a->status1 || (a->vol & FS_V_CONFUSED)) { a->status1 = 0; a->vol &= ~(FS_V_CONFUSED | FS_V_NIGHTMARE); a->item = 0; } break;
    case HOLD_EFFECT_CURE_ATTRACT: if (a->vol & FS_V_INFATUATED) { a->vol &= ~FS_V_INFATUATED; a->item = 0; } break;
    case HOLD_EFFECT_ATTACK_UP: case HOLD_EFFECT_DEFENSE_UP: case HOLD_EFFECT_SPEED_UP: case HOLD_EFFECT_SP_ATTACK_UP: case HOLD_EFFECT_SP_DEFENSE_UP:
        if (a->hp <= a->maxHP / param) { int st = STAT_ATK_ + (he - HOLD_EFFECT_ATTACK_UP); if (a->stages[st] < 12) { a->stages[st]++; a->item = 0; } }
        break;
    case HOLD_EFFECT_CRITICAL_UP: if (a->hp <= a->maxHP / param && !(a->vol & FS_V_FOCUS_ENERGY)) { a->vol |= FS_V_FOCUS_ENERGY; a->item = 0; } break;
    case HOLD_EFFECT_RANDOM_STAT_UP:
        if (a->hp <= a->maxHP / param)
        {
            int st, cand[5], n = 0;
            for (st = STAT_ATK_; st <= STAT_SPDEF_; st++) if (a->stages[st] < 12) cand[n++] = st;
            if (n) { a->stages[cand[fs_roll(s, n)]] += 2; if (a->stages[cand[0]] > 12) {} a->item = 0; }
        }
        break;
    case HOLD_EFFECT_RESTORE_STATS:
    {
        int st, any = 0;
        for (st = 0; st < FS_STAGES; st++) if (a->stages[st] < 6) { a->stages[st] = 6; any = 1; }
        if (any) a->item = 0;
        break;
    }
    case HOLD_EFFECT_RESTORE_PP:
    {
        int i;
        for (i = 0; i < 4; i++) if (a->moves[i] && a->pp[i] == 0) { u8 mx = s->side[side].party[a->monIdx].maxPP[i]; a->pp[i] = param > mx ? mx : param; a->item = 0; break; }
        break;
    }
    }
    if (a->item == 0) s->side[side].party[a->monIdx].item = 0;
}
