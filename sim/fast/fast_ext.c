// Fast engine extensions: everything the core (fast_effects.c) does not implement is added here behind the
// hooks declared in fast_internal.h, so several people can extend coverage without touching the core.
//
// Implemented here (reference: the verbatim port in ../src, functions named in the comments):
//   abilities: Trace, Cloud Nine / Air Lock (via fs_weather_active in fast.c), Forecast, Color Change
//   moves: Transform, Pursuit, Trick, Imprison, Grudge, Spite, Mimic, Role Play, Skill Swap, Recycle,
//          Stockpile / Spit Up / Swallow, Present, Beat Up, Charge, Camouflage, Conversion, Conversion 2,
//          Lock-On / Mind Reader, Secret Power
//   items: the confusion berries (Figy / Wiki / Mago / Aguav / Iapapa)
#include "fast.h"
#include "fast_internal.h"
#include "global.h"
#include "battle.h"
#include "battle_main.h"
#include "pokemon.h"
#include "data.h"
#include "constants/species.h"
#include "constants/moves.h"
#include "constants/abilities.h"
#include "constants/items.h"
#include "constants/hold_effects.h"
#include "constants/battle_move_effects.h"

#define OPP(sd_) ((sd_) ^ 1)
#define ACT(st_, sd_) (&(st_)->side[sd_].act)

static int IsType(const fs_battler *a, u8 t) { return a->type1 == t || a->type2 == t; }
static void SetType(fs_battler *a, u8 t) { a->type1 = t; a->type2 = t; }
static int Protected(const fs_battler *t, u16 move) { return (t->vol & FS_V_PROTECTED) && (fs_move(move)->flags & FLAG_PROTECT_AFFECTED); }

// ---------------------------------------------------------------------------------------------------------
// support tables

int fs_ext_effect_supported(u8 effect)
{
#ifdef FS_EXT_DISABLE
    return 0;
#endif
    switch (effect)
    {
    case EFFECT_TRANSFORM: case EFFECT_PURSUIT: case EFFECT_TRICK: case EFFECT_IMPRISON: case EFFECT_GRUDGE: case EFFECT_SPITE:
    case EFFECT_MIMIC: case EFFECT_ROLE_PLAY: case EFFECT_SKILL_SWAP: case EFFECT_RECYCLE: case EFFECT_STOCKPILE: case EFFECT_SPIT_UP:
    case EFFECT_SWALLOW: case EFFECT_PRESENT: case EFFECT_BEAT_UP: case EFFECT_CHARGE: case EFFECT_CAMOUFLAGE: case EFFECT_CONVERSION:
    case EFFECT_CONVERSION_2: case EFFECT_LOCK_ON: case EFFECT_SECRET_POWER:
        return 1;
    }
    return 0;
}

int fs_ext_ability_supported(u8 ability)
{
#ifdef FS_EXT_DISABLE
    return 0;
#endif
    switch (ability)
    {
    case ABILITY_TRACE: case ABILITY_CLOUD_NINE: case ABILITY_AIR_LOCK: case ABILITY_FORECAST: case ABILITY_COLOR_CHANGE:
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------------------
// abilities

// ABILITYEFFECT_TRACE (battle_util.c): a Trace mon that has not traced yet copies the opposing battler's ability
// as soon as that battler is present with an ability; otherwise it keeps trying after every action.
static void TryTrace(fs_state *s, int side)
{
    fs_battler *a = ACT(s, side), *t = ACT(s, OPP(side));
    if (!a->present || a->ability != ABILITY_TRACE || !(a->vol & FS_V_TRACE_ARMED)) return;
    if (t->present && t->hp && t->ability)
    {
        a->ability = t->ability;
        a->vol &= ~FS_V_TRACE_ARMED;
    }
}

// CastformDataTypeChange (battle_util.c)
static void UpdateCastform(fs_state *s, int side)
{
    fs_battler *a = ACT(s, side);
    if (!a->present || a->species != SPECIES_CASTFORM || a->ability != ABILITY_FORECAST || a->hp == 0) return;
    if (!fs_weather_active(s))
    {
        if (!IsType(a, TYPE_NORMAL)) SetType(a, TYPE_NORMAL);
        return;
    }
    if (s->weather != FS_WEATHER_RAIN && s->weather != FS_WEATHER_SUN && s->weather != FS_WEATHER_HAIL && !IsType(a, TYPE_NORMAL)) SetType(a, TYPE_NORMAL);
    if (s->weather == FS_WEATHER_SUN && !IsType(a, TYPE_FIRE)) SetType(a, TYPE_FIRE);
    if (s->weather == FS_WEATHER_RAIN && !IsType(a, TYPE_WATER)) SetType(a, TYPE_WATER);
    if (s->weather == FS_WEATHER_HAIL && !IsType(a, TYPE_ICE)) SetType(a, TYPE_ICE);
}

// HandleFaintedMonActions case 6 / TryDoEventsBeforeFirstTurn: Trace, then the Castform forms (ABILITYEFFECT_FORECAST)
void fs_ext_field_update(fs_state *s)
{
    int side;
    for (side = 0; side < 2; side++) TryTrace(s, side);
    for (side = 0; side < 2; side++) UpdateCastform(s, side);
}

// ABILITYEFFECT_ON_SWITCHIN for the incoming mon: Trace arms (STATUS3_TRACE); Forecast and Cloud Nine / Air Lock
// re-evaluate the Castform forms; then the same field update that follows every action.
void fs_ext_switch_in_ability(fs_state *s, int side)
{
    fs_battler *a = ACT(s, side);
    if (a->ability == ABILITY_TRACE) a->vol |= FS_V_TRACE_ARMED;
    fs_ext_field_update(s);
}

// ABILITYEFFECT_ON_DAMAGE, Color Change: the target takes the type of a damaging move that hit its HP
void fs_ext_on_damage(fs_state *s, int side, u16 move, const struct fs_hit *h)
{
    fs_battler *t = ACT(s, OPP(side));
    u8 type;
    if (!t->present || t->hp == 0 || t->ability != ABILITY_COLOR_CHANGE) return;
    if (!h->hit || h->dmg == 0 || h->hadSub || move == MOVE_STRUGGLE || fs_move(move)->power == 0) return;
    type = t->lastHitByType;   // the type of this hit (set by the core right before the damage)
    if (IsType(t, type)) return;
    SetType(t, type);
}

// ---------------------------------------------------------------------------------------------------------
// items: the confusion berries (ItemBattleEffects ITEMEFFECT_NORMAL, end of turn only: TRY_EAT_CONFUSE_BERRY)

// sPokeblockFlavorCompatibilityTable (pokemon.c): [nature][flavor], flavors spicy, dry, sweet, bitter, sour
static const s8 sFlavorTable[25 * 5] = {
     0, 0, 0, 0, 0,   1, 0, 0, 0,-1,   1, 0,-1, 0, 0,   1,-1, 0, 0, 0,   1, 0, 0,-1, 0,
    -1, 0, 0, 0, 1,   0, 0, 0, 0, 0,   0, 0,-1, 0, 1,   0,-1, 0, 0, 1,   0, 0, 0,-1, 1,
    -1, 0, 1, 0, 0,   0, 0, 1, 0,-1,   0, 0, 0, 0, 0,   0,-1, 1, 0, 0,   0, 0, 1,-1, 0,
    -1, 1, 0, 0, 0,   0, 1, 0, 0,-1,   0, 1,-1, 0, 0,   0, 0, 0, 0, 0,   0, 1, 0,-1, 0,
    -1, 0, 0, 1, 0,   0, 0, 0, 1,-1,   0, 0,-1, 1, 0,   0,-1, 0, 1, 0,   0, 0, 0, 0, 0,
};

void fs_ext_end_turn_item(fs_state *s, int side)
{
    fs_battler *a = ACT(s, side);
    fs_side *sd = &s->side[side];
    u8 he, param, flavor, nature;
    s32 heal;
    u16 item;
    if (!a->present || a->hp == 0) return;
    he = fs_hold_effect(a->item);
    if (he < HOLD_EFFECT_CONFUSE_SPICY || he > HOLD_EFFECT_CONFUSE_SOUR) return;
    if (a->hp > a->maxHP / 2) return;
    param = fs_hold_param(a->item);
    heal = a->maxHP / (param ? param : 8);
    if (heal == 0) heal = 1;
    fs_heal(s, side, heal);
    flavor = he - HOLD_EFFECT_CONFUSE_SPICY;
    nature = sd->party[a->monIdx].nature;
    item = a->item;
    a->item = 0; sd->party[a->monIdx].item = 0; sd->usedItem = item;
    // BattleScript_BerryConfuseHealEnd2: a primary confusion on the user (no Safeguard / Substitute check)
    if (sFlavorTable[nature * 5 + flavor] < 0) fs_try_confuse(s, side, 0, 0);
}

// ---------------------------------------------------------------------------------------------------------
// Pursuit on a switching target (BattleScript_ActionSwitch: jumpifnopursuitswitchdmg + BattleScript_PursuitDmgOnSwitchOut)
// No attackcanceler and no accuracy check; PP is used; the damage multiplier is 2; the pursuer's action is spent.

void fs_ext_before_switch(fs_state *s, int side)
{
    int opp = OPP(side);
    fs_battler *a = ACT(s, side), *p = ACT(s, opp);
    const struct BattleMove *bm = fs_move(MOVE_PURSUIT);
    int slot = s->pending[opp].slot;
    if (!a->present || a->hp == 0 || !p->present || p->hp == 0) return;
    if (s->pending[opp].type != FS_ACT_MOVE || p->chosenMove != MOVE_PURSUIT) return;
    if (p->status1 & (FS_S1_SLEEP | FS_S1_FRZ)) return;
    if (p->ability == ABILITY_TRUANT && p->truantCounter) return;
    if (slot < 4 && p->pp[slot])
    {
        u8 dec = 1 + (a->ability == ABILITY_PRESSURE);
        p->pp[slot] = p->pp[slot] > dec ? p->pp[slot] - dec : 0;
    }
    fs_attack_ex(s, opp, MOVE_PURSUIT, bm->power, bm->type, 1, 0, 0, 2, 0);   // Destiny Bond is applied inside (trysetdestinybondtohappen)
    p->vol |= FS_V_MOVED_THIS_TURN;
    s->pending[opp].type = FS_ACT_NONE;
}

// ---------------------------------------------------------------------------------------------------------
// moves

static int IsMail(u16 item) { return item >= ITEM_ORANGE_MAIL && item <= ITEM_RETRO_MAIL; }

static void FillHit(struct fs_hit *h, s32 dmg) { h->hit = dmg > 0; h->dmg = dmg; }

int fs_ext_use_move(fs_state *s, int side, int slot, u16 move, u16 power, u8 type, void *hitp)
{
    int opp = OPP(side);
    fs_battler *a = ACT(s, side), *t = ACT(s, opp);
    fs_side *sd = &s->side[side], *od = &s->side[opp];
    const struct BattleMove *bm = fs_move(move);
    struct fs_hit *hit = (struct fs_hit *)hitp;
    int i, k;
    switch (bm->effect)
    {
    case EFFECT_PURSUIT:   // BattleScript_EffectHit when the target did not switch (the switch case runs in fs_ext_before_switch)
        FillHit(hit, fs_attack(s, side, move, power, type, 0, 0, 0));
        return 1;

    case EFFECT_TRANSFORM:   // Cmd_transformdataexecution
        a->lastMove = 0; a->choicedMove = 0;   // gChosenMove = MOVE_UNAVAILABLE: no last move, no Choice lock
        if (!t->present || Protected(t, move)) return 1;
        if ((t->vol & FS_V_TRANSFORMED) || (t->vol & (FS_V_ON_AIR | FS_V_UNDERGROUND | FS_V_UNDERWATER))) return 1;
        // everything before `pp` in struct BattlePokemon: species, stats, moves, IVs, stat stages, ability, types
        a->species = t->species; a->atk = t->atk; a->def = t->def; a->spe = t->spe; a->spa = t->spa; a->spd = t->spd;
        for (i = 0; i < 4; i++)
        {
            u8 pp = fs_move(t->moves[i])->pp;
            a->moves[i] = t->moves[i];
            a->pp[i] = pp < 5 ? pp : 5;
        }
        memcpy(a->stages, t->stages, sizeof(a->stages));
        a->ability = t->ability; a->type1 = t->type1; a->type2 = t->type2;
        a->hpTypeCache = t->hpTypeCache; a->hpPowerCache = t->hpPowerCache;
        a->vol |= FS_V_TRANSFORMED;
        a->disableTimer = 0; a->disabledPos = 0; a->mimicked = 0;
        return 1;

    case EFFECT_TRICK:   // BattleScript_EffectTrick / Cmd_tryswapitems
        if (!t->present || Protected(t, move) || (t->vol & FS_V_SUBSTITUTE)) return 1;
        if (!fs_accuracy_check(s, side, opp, move, type)) return 1;
        if (side == FS_SIDE_OPP) return 1;   // in a regular trainer battle the opponent cannot swap items with the player
        if ((sd->knockedOff & (1 << a->monIdx)) || (od->knockedOff & (1 << t->monIdx))) return 1;
        if (a->item == ITEM_NONE && t->item == ITEM_NONE) return 1;
        if (a->item == ITEM_ENIGMA_BERRY || t->item == ITEM_ENIGMA_BERRY || IsMail(a->item) || IsMail(t->item)) return 1;
        if (t->ability == ABILITY_STICKY_HOLD) return 1;
        {
            u16 old = a->item;
            a->item = t->item; t->item = old;
            a->choicedMove = 0; t->choicedMove = 0;
            sd->party[a->monIdx].item = a->item; od->party[t->monIdx].item = t->item;
        }
        return 1;

    case EFFECT_IMPRISON:   // Cmd_tryimprison: fails if the user already imprisons or shares no move with the foe
        if (a->vol & FS_V_IMPRISON) return 1;
        for (i = 0; i < 4; i++)
        {
            if (!a->moves[i]) continue;
            for (k = 0; k < 4; k++) if (t->moves[k] == a->moves[i]) break;
            if (k < 4) break;
        }
        if (i < 4) a->vol |= FS_V_IMPRISON;
        return 1;

    case EFFECT_GRUDGE:   // Cmd_trysetgrudge (cleared at the start of the user's next move, like Destiny Bond)
        if (!(a->vol & FS_V_GRUDGE)) a->vol |= FS_V_GRUDGE;
        return 1;

    case EFFECT_SPITE:   // Cmd_tryspiteppreduce: the target's last move loses 2..5 PP (only if it has more than 1)
        if (!t->present || Protected(t, move)) return 1;
        if (!fs_accuracy_check(s, side, opp, move, type)) return 1;
        if (!t->lastMove) return 1;
        for (i = 0; i < 4; i++) if (t->moves[i] == t->lastMove) break;
        if (i == 4 || t->pp[i] <= 1) return 1;
        {
            u8 dec = 2 + fs_roll(s, 4);
            if (t->pp[i] < dec) dec = t->pp[i];
            t->pp[i] -= dec;
        }
        return 1;

    case EFFECT_MIMIC:   // Cmd_mimicattackcopy: the slot becomes the target's last move with 5 PP until the user leaves
        a->lastMove = 0; a->choicedMove = 0;   // gChosenMove = MOVE_UNAVAILABLE
        if (!t->present || (t->vol & FS_V_SUBSTITUTE) || (t->vol & (FS_V_ON_AIR | FS_V_UNDERGROUND | FS_V_UNDERWATER)) || Protected(t, move)) return 1;
        if (a->vol & FS_V_TRANSFORMED) return 1;
        if (!t->lastMove || t->lastMove == MOVE_METRONOME || t->lastMove == MOVE_STRUGGLE || t->lastMove == MOVE_SKETCH || t->lastMove == MOVE_MIMIC) return 1;
        for (i = 0; i < 4; i++) if (a->moves[i] == t->lastMove) return 1;
        if (slot >= 4) return 1;
        {
            u8 pp = fs_move(t->lastMove)->pp;
            a->moves[slot] = t->lastMove;
            a->pp[slot] = pp < 5 ? pp : 5;
            a->mimicked |= 1 << slot;
        }
        return 1;

    case EFFECT_ROLE_PLAY:   // Cmd_trycopyability (accuracycheck NO_ACC_CALC_CHECK_LOCK_ON: only Protect / semi-invulnerability stop it)
        if (!t->present || (t->vol & (FS_V_ON_AIR | FS_V_UNDERGROUND | FS_V_UNDERWATER)) || Protected(t, move)) return 1;
        if (t->ability == ABILITY_NONE || t->ability == ABILITY_WONDER_GUARD) return 1;
        a->ability = t->ability;
        return 1;

    case EFFECT_SKILL_SWAP:   // Cmd_tryswapabilities
        if (!t->present || (t->vol & (FS_V_ON_AIR | FS_V_UNDERGROUND | FS_V_UNDERWATER)) || Protected(t, move)) return 1;
        if ((a->ability == ABILITY_NONE && t->ability == ABILITY_NONE) || a->ability == ABILITY_WONDER_GUARD || t->ability == ABILITY_WONDER_GUARD) return 1;
        { u8 ab = a->ability; a->ability = t->ability; t->ability = ab; }
        return 1;

    case EFFECT_RECYCLE:   // Cmd_tryrecycleitem: the item last consumed in this battler slot comes back
        if (sd->usedItem && a->item == ITEM_NONE)
        {
            a->item = sd->usedItem; sd->usedItem = 0;
            sd->party[a->monIdx].item = a->item;
        }
        return 1;

    case EFFECT_STOCKPILE:   // Cmd_stockpile
        if (a->stockpile < 3) a->stockpile++;
        return 1;

    case EFFECT_SPIT_UP:   // BattleScript_EffectSpitUp: base damage x count, STAB and type, no crit, no random roll
        if (!t->present) return 1;
        if (Protected(t, move)) { a->stockpile = 0; return 1; }   // BattleScript_SpitUpFailProtect still spends the stockpile
        if (!fs_accuracy_check(s, side, opp, move, type)) return 1;
        if (a->stockpile == 0) return 1;
        {
            int count = a->stockpile;
            a->stockpile = 0;
            FillHit(hit, fs_attack_ex(s, side, move, power, type, 1, 0, 1, count, 1));
        }
        return 1;

    case EFFECT_SWALLOW:   // Cmd_stockpiletohpheal: 1/4, 1/2 or all of max HP
        if (a->stockpile == 0) return 1;
        if (a->hp == a->maxHP) { a->stockpile = 0; return 1; }
        {
            s32 amt = a->maxHP / (1 << (3 - a->stockpile));
            if (amt == 0) amt = 1;
            a->stockpile = 0;
            fs_heal(s, side, amt);
        }
        return 1;

    case EFFECT_PRESENT:   // Cmd_presentdamagecalculation: 40 / 80 / 120 power or a heal of 1/4 max HP
        if (!t->present || Protected(t, move)) return 1;
        if (!fs_accuracy_check(s, side, opp, move, type)) return 1;
        {
            u32 r = fs_roll(s, 256);
            if (r < 204)
            {
                FillHit(hit, fs_attack_ex(s, side, move, r < 102 ? 40 : r < 178 ? 80 : 120, type, 1, 0, 0, 1, 0));
            }
            else if (t->hp != t->maxHP)
            {
                s32 amt = t->maxHP / 4;
                if (amt == 0) amt = 1;
                fs_heal(s, opp, amt);   // ignores the Substitute and type immunity
            }
        }
        return 1;

    case EFFECT_BEAT_UP:   // Cmd_trydobeatup: one hit per healthy party member, from base stats, no STAB / type chart
    {
        int any = 0;
        s32 last = 0;
        if (!t->present || Protected(t, move)) return 1;
        if (!fs_accuracy_check(s, side, opp, move, type)) return 1;
        for (i = 0; i < FS_PARTY && t->present; i++)
        {
            const fs_mon *m = &sd->party[i];
            u16 hp = (i == a->monIdx) ? a->hp : m->hp, st = (i == a->monIdx) ? a->status1 : m->status1;
            s32 dmg;
            int crit, focus;
            struct fs_hit hh = {0, 1, 0, 10, 0, 0};
            if (!m->species || hp == 0 || st) continue;
            any = 1;
            dmg = gSpeciesInfo[m->species].baseAttack;
            dmg *= bm->power;
            dmg *= (m->level * 2 / 5 + 2);
            dmg /= gSpeciesInfo[t->species].baseDefense;
            dmg = dmg / 50 + 2;
            crit = fs_crit_check(s, side, opp, move);
            if (crit) dmg *= 2;
            dmg = fs_random_roll(s, dmg);
            if (dmg == 0) dmg = 1;
            // adjustnormaldamage: Focus Band / Endure, then the Substitute
            focus = fs_hold_effect(t->item) == HOLD_EFFECT_FOCUS_BAND && fs_chance(s, fs_hold_param(t->item), 100);
            if (!(t->vol & FS_V_SUBSTITUTE) && ((t->vol & FS_V_ENDURED) || focus) && t->hp <= dmg) dmg = t->hp - 1;
            if (t->vol & FS_V_SUBSTITUTE)
            {
                hh.hadSub = 1;
                if (dmg >= t->substituteHP) { dmg = t->substituteHP; t->substituteHP = 0; t->vol &= ~FS_V_SUBSTITUTE; } else t->substituteHP -= dmg;
            }
            else
            {
                if (dmg > t->hp) dmg = t->hp;
                t->lastLandedMove = move; t->lastHitByType = type;
                hh.dbond = (t->vol & FS_V_DESTINY_BOND) != 0;
                fs_damage(s, opp, dmg);
                if (t->present) t->bideDmg = dmg;
                else if (hh.dbond && a->present) { a->hp = 0; fs_faint(s, side); }
            }
            hh.dmg = dmg; hh.crit = crit;
            fs_ext_on_damage(s, side, move, &hh);
            // MOVEEND_KINGSROCK_SHELLBELL runs for each hit
            if (t->present && !hh.hadSub && fs_hold_effect(a->item) == HOLD_EFFECT_FLINCH && (bm->flags & FLAG_KINGS_ROCK_AFFECTED) && fs_chance(s, fs_hold_param(a->item), 100))
                t->vol |= FS_V_FLINCH;
            if (a->present && t->present && fs_hold_effect(a->item) == HOLD_EFFECT_SHELL_BELL && dmg > 0) fs_heal(s, side, dmg / 8 ? dmg / 8 : 1);
            last = dmg;
        }
        if (any) FillHit(hit, last);
        return 1;
    }

    case EFFECT_CHARGE:   // Cmd_setcharge
        a->vol |= FS_V_CHARGED;
        a->chargeTimer = 2;
        return 1;

    case EFFECT_CAMOUFLAGE:   // Cmd_settypetoterrain: the simulator always battles on BATTLE_TERRAIN_GRASS (sim_api.c)
        if (!IsType(a, TYPE_GRASS)) SetType(a, TYPE_GRASS);
        return 1;

    case EFFECT_CONVERSION:   // Cmd_tryconversiontypechange: a random one of the user's move types it does not have yet
    {
        u8 types[4];
        int n = 0, valid = 0;
        for (valid = 0; valid < 4 && a->moves[valid]; valid++)
        {
            u8 mt = fs_move(a->moves[valid])->type;
            if (mt == TYPE_MYSTERY) mt = IsType(a, TYPE_GHOST) ? TYPE_GHOST : TYPE_NORMAL;
            if (!IsType(a, mt)) types[n++] = mt;
        }
        if (n) SetType(a, types[fs_roll(s, n)]);   // rejection sampling over the valid moves: uniform over the qualifying ones
        return 1;
    }

    case EFFECT_CONVERSION_2:   // Cmd_settypetorandomresistance: a random type that resists the last move that hit the user
    {
        u8 types[64];
        int n = 0;
        if (!a->lastLandedMove) return 1;
        for (i = 0; gTypeEffectiveness[i] != TYPE_ENDTABLE; i += 3)
        {
            if (gTypeEffectiveness[i] == TYPE_FORESIGHT) continue;
            if (gTypeEffectiveness[i] == a->lastHitByType && gTypeEffectiveness[i + 2] <= TYPE_MUL_NOT_EFFECTIVE && !IsType(a, gTypeEffectiveness[i + 1]) && n < 64)
                types[n++] = gTypeEffectiveness[i + 1];
        }
        if (n) SetType(a, types[fs_roll(s, n)]);   // 1000 random table probes: uniform over the qualifying entries
        return 1;
    }

    case EFFECT_LOCK_ON:   // Cmd_setalwayshitflag: the user's moves cannot miss the target next turn
        if (!t->present || Protected(t, move) || (t->vol & FS_V_SUBSTITUTE)) return 1;
        if (!fs_accuracy_check(s, side, opp, move, type)) return 1;
        t->lockOn = 2;
        return 1;

    case EFFECT_SECRET_POWER:   // Cmd_getsecretpowereffect on BATTLE_TERRAIN_GRASS: a hit with a chance to poison
    {
        s32 dmg = fs_attack(s, side, move, power, type, 0, 0, 0);
        FillHit(hit, dmg);
        if (dmg && t->present && !(t->vol & FS_V_SUBSTITUTE))
        {
            u32 chance = bm->secondaryEffectChance * (a->ability == ABILITY_SERENE_GRACE ? 2 : 1);
            if (fs_roll(s, 100) < chance) fs_try_status(s, opp, FS_S1_PSN, 1, side, 1);
        }
        return 1;
    }
    }
    return 0;
}
