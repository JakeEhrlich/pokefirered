// Tempo features and the "tempo" value function: a static approximation of the one-turn lookahead value V1 from
// quantities the lookahead is sensitive to (who KOs whom next turn, how fast, and what the bench can do), on the
// fast state. Weights in fs_value_tempo were fitted by tools/fit_tempo.py against V1 labels (tests/vfdata.c).
#include <math.h>
#include <string.h>
#include "global.h"
#include "battle.h"
#include "pokemon.h"
#include "fast.h"
#include "fast_internal.h"
#include "constants/moves.h"
#include "constants/abilities.h"
#include "constants/hold_effects.h"
#include "constants/battle_move_effects.h"
#include "sim.h"
#include "sim_agent.h"

// A mon as an attacker / defender for the estimates: either the battle copy (active) or a party entry (bench)
struct VMon { u16 hp, maxHP, atk, def, spa, spd, spe, level, status1, item; u8 type1, type2, ability, hpType, hpPower, friendship; const u16 *moves; s8 stages[8]; int active; };

static void FillActive(const fs_state *s, int side, struct VMon *m)
{
    const fs_battler *a = &s->side[side].act;
    const fs_mon *p = &s->side[side].party[a->monIdx];
    memset(m, 0, sizeof(*m));
    m->hp = a->hp; m->maxHP = a->maxHP; m->atk = a->atk; m->def = a->def; m->spa = a->spa; m->spd = a->spd; m->spe = a->spe;
    m->level = a->level; m->status1 = a->status1; m->item = a->item; m->type1 = a->type1; m->type2 = a->type2; m->ability = a->ability;
    m->hpType = a->hpTypeCache; m->hpPower = p->hpPower; m->friendship = p->friendship; m->moves = a->moves;
    memcpy(m->stages, a->stages, 8); m->active = 1;
}

static void FillBench(const fs_state *s, int side, int idx, struct VMon *m)
{
    const fs_mon *p = &s->side[side].party[idx];
    int i;
    memset(m, 0, sizeof(*m));
    m->hp = p->hp; m->maxHP = p->maxHP; m->atk = p->atk; m->def = p->def; m->spa = p->spa; m->spd = p->spd; m->spe = p->spe;
    m->level = p->level; m->status1 = p->status1; m->item = p->item; m->type1 = p->type1; m->type2 = p->type2; m->ability = p->ability;
    m->hpType = p->hpType; m->hpPower = p->hpPower; m->friendship = p->friendship; m->moves = p->moves;
    for (i = 0; i < 8; i++) m->stages[i] = 6;
}

static float StatusPenalty7(u16 st)
{
    if (st & FS_S1_SLEEP) return 0.30f;
    if (st & FS_S1_FRZ) return 0.40f;
    if (st & FS_S1_TOX) return 0.25f;
    if (st & FS_S1_BRN) return 0.20f;
    if (st & FS_S1_PAR) return 0.15f;
    if (st & FS_S1_PSN) return 0.10f;
    return 0.0f;
}

static u32 StatMod(u32 stat, s8 stage) { return stat * fs_stat_ratio[stage][0] / fs_stat_ratio[stage][1]; }

static u32 Speed(const fs_state *s, const struct VMon *m)
{
    u32 sp = StatMod(m->spe, m->stages[3]);
    if (m->ability == ABILITY_SWIFT_SWIM && s->weather == 1) sp *= 2;
    if (m->ability == ABILITY_CHLOROPHYLL && s->weather == 2) sp *= 2;
    if (fs_hold_effect(m->item) == HOLD_EFFECT_MACHO_BRACE) sp /= 2;
    if (m->status1 & FS_S1_PAR) sp /= 4;
    return sp;
}

// Expected damage (mean roll, no crit) of `move` by at against df, times its accuracy: the per-turn expectation.
// Returns 0 for non-damaging moves. *hitProb gets the accuracy, *maxDmg the top roll (for KO probabilities).
static float MoveDamage(const fs_state *s, const struct VMon *at, const struct VMon *df, int atSide, u16 move, float *hitProb, float *maxDmg)
{
    const struct BattleMove *bm;
    u16 power; u8 type;
    u32 attack, defense, dmg;
    int physical, mult;
    *hitProb = 0; *maxDmg = 0;
    if (move == 0) return 0;
    bm = fs_move(move);
    power = bm->power; type = bm->type;
    if (bm->effect == EFFECT_HIDDEN_POWER) { power = at->hpPower; type = at->hpType; }
    else if (bm->effect == EFFECT_RETURN) power = 10 * at->friendship / 25;
    else if (bm->effect == EFFECT_FRUSTRATION) power = 10 * (255 - at->friendship) / 25;
    else if (bm->effect == EFFECT_FLAIL) power = 40;
    else if (bm->effect == EFFECT_ERUPTION) power = 150 * at->hp / (at->maxHP ? at->maxHP : 1);
    else if (bm->effect == EFFECT_OHKO) { *hitProb = 0.3f; *maxDmg = df->hp; return 0.3f * df->hp; }
    else if (bm->effect == EFFECT_LEVEL_DAMAGE) { *hitProb = 1; *maxDmg = at->level; return at->level; }
    else if (bm->effect == EFFECT_DRAGON_RAGE) { *hitProb = 1; *maxDmg = 40; return 40; }
    else if (bm->effect == EFFECT_SONICBOOM) { *hitProb = 0.9f; *maxDmg = 20; return 18; }
    else if (bm->effect == EFFECT_SUPER_FANG) { *hitProb = 0.9f; *maxDmg = df->hp / 2; return 0.9f * (df->hp / 2); }
    if (power == 0) return 0;
    if (bm->effect == EFFECT_EXPLOSION || bm->effect == EFFECT_COUNTER || bm->effect == EFFECT_MIRROR_COAT || bm->effect == EFFECT_BIDE || bm->effect == EFFECT_FUTURE_SIGHT
     || bm->effect == EFFECT_SOLAR_BEAM || bm->effect == EFFECT_SKULL_BASH || bm->effect == EFFECT_RAZOR_WIND || bm->effect == EFFECT_SKY_ATTACK || bm->effect == EFFECT_SEMI_INVULNERABLE)
        return 0;   // not usable as steady damage in the race
    mult = fs_type_mult(type, df->type1, df->type2, 0);
    if (mult == 0) return 0;
    if (df->ability == ABILITY_LEVITATE && type == TYPE_GROUND) return 0;
    if ((df->ability == ABILITY_VOLT_ABSORB && type == TYPE_ELECTRIC) || (df->ability == ABILITY_WATER_ABSORB && type == TYPE_WATER) || (df->ability == ABILITY_FLASH_FIRE && type == TYPE_FIRE)) return 0;
    if (df->ability == ABILITY_WONDER_GUARD && mult <= 10) return 0;
    physical = type < TYPE_MYSTERY;
    if (physical) { attack = StatMod(at->atk, at->stages[1]); defense = StatMod(df->def, df->stages[2]); }
    else { attack = StatMod(at->spa, at->stages[4]); defense = StatMod(df->spd, df->stages[5]); }
    if (fs_hold_effect(at->item) == HOLD_EFFECT_CHOICE_BAND && physical) attack = attack * 3 / 2;
    if (physical && (at->status1 & FS_S1_BRN) && at->ability != ABILITY_GUTS) attack /= 2;
    if (at->ability == ABILITY_HUSTLE && physical) attack = attack * 3 / 2;
    if (df->ability == ABILITY_THICK_FAT && (type == TYPE_FIRE || type == TYPE_ICE)) attack /= 2;
    if (physical ? s->side[atSide ^ 1].reflect : s->side[atSide ^ 1].lightscreen) defense *= 2;
    if (s->weather == 1 && type == TYPE_FIRE) power /= 2;
    if (s->weather == 1 && type == TYPE_WATER) power = power * 3 / 2;
    if (s->weather == 2 && type == TYPE_WATER) power /= 2;
    if (s->weather == 2 && type == TYPE_FIRE) power = power * 3 / 2;
    if (defense == 0) defense = 1;
    dmg = ((2 * at->level / 5 + 2) * power * attack / defense) / 50 + 2;
    if (at->type1 == type || at->type2 == type) dmg = dmg * 3 / 2;
    dmg = dmg * mult / 10;
    *hitProb = bm->accuracy ? bm->accuracy / 100.0f : 1.0f;
    if (at->ability == ABILITY_HUSTLE && physical && bm->accuracy) *hitProb *= 0.8f;
    *maxDmg = (float)dmg;
    return *hitProb * dmg * 0.925f;
}

// Best-move numbers of at against df: expected damage per turn and the probability that one use KOs (roll x hit).
static void BestAttack(const fs_state *s, const struct VMon *at, const struct VMon *df, int atSide, float *expDmg, float *pKO)
{
    int i;
    *expDmg = 0; *pKO = 0;
    for (i = 0; i < 4; i++)
    {
        float hit, mx, e = MoveDamage(s, at, df, atSide, at->moves[i], &hit, &mx), p = 0;
        if (e > *expDmg) *expDmg = e;
        if (mx > 0 && df->hp > 0)
        {
            // rolls are uniform over 85..100% of mx: P(roll >= hp) = fraction of the 16 rolls at or above hp
            float need = (float)df->hp / mx;   // fraction of the max roll needed
            if (need <= 0.85f) p = 1.0f;
            else if (need > 1.0f) p = 0.0f;
            else p = (1.0f - need) / 0.15f;
            p *= hit;
        }
        if (p > *pKO) *pKO = p;
    }
}

static float Clamp(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

// Expected turns for `at` to KO `df` with its best move (capped), given the expected damage per turn.
static float Ttk(float expDmg, float hp) { return expDmg <= 0.5f ? 12.0f : Clamp(hp / expDmg, 0.0f, 12.0f); }

// Race score in [-1, 1] for `me` against `them`: positive when I win the 1v1 exchange, using turns-to-KO and speed.
static float Race(float ttkMe, float ttkThem, int meFaster)
{
    float d = ttkThem - ttkMe + (meFaster > 0 ? 0.6f : meFaster < 0 ? -0.6f : 0.0f);
    return tanhf(d);
}

int fs_tempo_features(const fs_state *s, int side, float *f)
{
    struct VMon me, them, b;
    int opp = side ^ 1, i, n = 0, meFaster, incMe, incThem;
    float dMe, kMe, dThem, kThem, ttkMe, ttkThem, race, benchBest = -1.0f, benchAvg = 0.0f, theirBenchBest = -1.0f, benchCount = 0, theirCount = 0;
    float benchSafe = 0, theirSafe = 0;
    u32 spMe, spThem;
    memset(f, 0, sizeof(float) * FS_TEMPO_NF);
    f[0] = fs_value_basic(s, side);
    if (s->request == FS_REQ_DONE) return FS_TEMPO_NF;
    if (!s->side[side].act.present || !s->side[opp].act.present || s->side[side].act.hp == 0 || s->side[opp].act.hp == 0)
        return FS_TEMPO_NF;   // a replacement is pending: only the material term
    FillActive(s, side, &me); FillActive(s, opp, &them);
    spMe = Speed(s, &me); spThem = Speed(s, &them);
    meFaster = spMe > spThem ? 1 : spMe < spThem ? -1 : 0;
    BestAttack(s, &me, &them, side, &dMe, &kMe);
    BestAttack(s, &them, &me, opp, &dThem, &kThem);
    ttkMe = Ttk(dMe, them.hp); ttkThem = Ttk(dThem, me.hp);
    incMe = (me.status1 & (FS_S1_SLEEP | FS_S1_FRZ)) != 0 || (s->side[side].act.vol & FS_V_RECHARGE);
    incThem = (them.status1 & (FS_S1_SLEEP | FS_S1_FRZ)) != 0 || (s->side[opp].act.vol & FS_V_RECHARGE);
    race = Race(ttkMe, ttkThem, meFaster);
    f[1] = race;
    f[2] = kMe; f[3] = kThem;
    f[4] = (float)meFaster;
    f[5] = kMe * (meFaster >= 0);                        // I can KO first
    f[6] = kThem * (meFaster <= 0);                      // they can KO first
    f[7] = Clamp(dMe / (them.maxHP ? them.maxHP : 1), 0, 1) - Clamp(dThem / (me.maxHP ? me.maxHP : 1), 0, 1);   // fraction of max HP per turn
    f[8] = Clamp((ttkThem - ttkMe) / 4.0f, -1, 1);
    f[9] = (float)(incThem - incMe);
    f[10] = (ttkMe <= 1.0f ? 1.0f : 0.0f) - (ttkThem <= 1.0f ? 1.0f : 0.0f);   // one-shot flags
    f[11] = ((ttkMe <= 2.0f && meFaster > 0) ? 1.0f : 0.0f) - ((ttkThem <= 2.0f && meFaster < 0) ? 1.0f : 0.0f);   // 2HKO with the speed
    // my bench against their active, their bench against my active
    for (i = 0; i < FS_PARTY; i++)
    {
        const fs_mon *p = &s->side[side].party[i];
        float d1, k1, d2, k2, r;
        u32 sp;
        if (!p->species || p->hp == 0 || i == s->side[side].act.monIdx) continue;
        FillBench(s, side, i, &b);
        sp = Speed(s, &b);
        BestAttack(s, &b, &them, side, &d1, &k1); BestAttack(s, &them, &b, opp, &d2, &k2);
        r = Race(Ttk(d1, them.hp), Ttk(d2, b.hp), sp > spThem ? 1 : sp < spThem ? -1 : 0);
        if (r > benchBest) benchBest = r;
        benchAvg += r; benchCount++;
        if (Ttk(d2, b.hp) >= 3.0f) benchSafe++;   // a switch-in that survives two hits
    }
    for (i = 0; i < FS_PARTY; i++)
    {
        const fs_mon *p = &s->side[opp].party[i];
        float d1, k1, d2, k2, r;
        u32 sp;
        if (!p->species || p->hp == 0 || i == s->side[opp].act.monIdx) continue;
        FillBench(s, opp, i, &b);
        sp = Speed(s, &b);
        BestAttack(s, &b, &me, opp, &d1, &k1); BestAttack(s, &me, &b, side, &d2, &k2);
        r = Race(Ttk(d1, me.hp), Ttk(d2, b.hp), sp > spMe ? 1 : sp < spMe ? -1 : 0);
        if (r > theirBenchBest) theirBenchBest = r;
        theirCount++;
        if (Ttk(d2, b.hp) >= 3.0f) theirSafe++;
    }
    f[12] = benchCount ? benchBest : 0.0f;
    f[13] = theirCount ? theirBenchBest : 0.0f;
    f[14] = benchCount ? benchAvg / benchCount : 0.0f;
    f[15] = (benchCount ? benchSafe / benchCount : 0.0f) - (theirCount ? theirSafe / theirCount : 0.0f);
    f[16] = (float)me.hp / (me.maxHP ? me.maxHP : 1) - (float)them.hp / (them.maxHP ? them.maxHP : 1);
    f[17] = race * f[16];
    f[18] = f[2] - f[3];
    n = FS_TEMPO_NF;
    return n;
}

int fs_tempo_features_sym(const fs_state *s, int side, float *f)
{
    float g[FS_TEMPO_NF];
    int i;
    fs_tempo_features(s, side, f);
    fs_tempo_features(s, side ^ 1, g);
    for (i = 0; i < FS_TEMPO_NF; i++) f[i] = 0.5f * (f[i] - g[i]);
    return FS_TEMPO_NF;
}

// fitted weights (tools/fit_tempo.py) on the symmetric features; f[0] is V0
static const float sTempoW[FS_TEMPO_NF] = {1.06033f, 0.01481f, 0.00316f, -0.00316f, -0.00413f, 0.04621f, -0.04621f, 0.00729f, -0.01156f, 0.00290f, 0.00328f, 0.00089f, 0.00607f, -0.00607f, -0.02861f, 0.00893f, -0.01312f, 0.00000f, 0.00628f};

float fs_value_tempo(const fs_state *s, int side)
{
    float f[FS_TEMPO_NF], v = 0.0f;
    int i;
    if (s->request == FS_REQ_DONE) return fs_value_basic(s, side);
    fs_tempo_features_sym(s, side, f);
    for (i = 0; i < FS_TEMPO_NF; i++) v += sTempoW[i] * f[i];
    return v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
}

// ---------------------------------------------------------------------------------------------------------
// Analytic one-ply value: the joint matrix of this turn's legal actions with each cell's outcome estimated in
// closed form (expected damage, KO probability, primary / secondary status, stat stages, healing, Explosion,
// switch-ins taking the hit) as a change of the material heuristic, solved with RM+ (30 iterations). No
// simulator calls: ~5 us. This is what the simulated lookahead V1 computes with samples.

#define P1_MAX 10
struct P1Act { fs_action a; int isSwitch; int slot; };   // slot: move slot or party slot

// the defender's heuristic contribution per HP fraction and for a KO (SideScore terms / 7)
static float HpWeight(const struct VMon *m) { return (1.0f - StatusPenalty7(m->status1)) / 7.0f; }
static float KoLoss(const struct VMon *m) { return ((float)m->hp / (m->maxHP ? m->maxHP : 1) * (1.0f - StatusPenalty7(m->status1)) + 0.15f) / 7.0f; }

static u16 PrimaryStatus(u8 effect, u16 move)
{
    switch (effect)
    {
    case EFFECT_SLEEP: return FS_S1_SLEEP;
    case EFFECT_POISON: return FS_S1_PSN;
    case EFFECT_TOXIC: return FS_S1_TOX;
    case EFFECT_PARALYZE: return FS_S1_PAR;
    case EFFECT_WILL_O_WISP: return FS_S1_BRN;
    }
    return 0;
}
static u16 SecondaryStatus(u8 effect)
{
    switch (effect)
    {
    case EFFECT_POISON_HIT: case EFFECT_POISON_TAIL: case EFFECT_TWINEEDLE: return FS_S1_PSN;
    case EFFECT_POISON_FANG: return FS_S1_TOX;
    case EFFECT_BURN_HIT: case EFFECT_BLAZE_KICK: case EFFECT_THAW_HIT: return FS_S1_BRN;
    case EFFECT_FREEZE_HIT: return FS_S1_FRZ;
    case EFFECT_PARALYZE_HIT: case EFFECT_THUNDER: return FS_S1_PAR;
    }
    return 0;
}
static int StatusPossible(const fs_state *s, int defSide, const struct VMon *df, u16 st)
{
    if (df->status1 & FS_S1_ANY) return 0;
    if (s->side[defSide].safeguard) return 0;
    if ((st & (FS_S1_PSN | FS_S1_TOX)) && (df->type1 == TYPE_POISON || df->type2 == TYPE_POISON || df->type1 == TYPE_STEEL || df->type2 == TYPE_STEEL || df->ability == ABILITY_IMMUNITY)) return 0;
    if ((st & FS_S1_BRN) && (df->type1 == TYPE_FIRE || df->type2 == TYPE_FIRE || df->ability == ABILITY_WATER_VEIL)) return 0;
    if ((st & FS_S1_FRZ) && (df->type1 == TYPE_ICE || df->type2 == TYPE_ICE || df->ability == ABILITY_MAGMA_ARMOR)) return 0;
    if ((st & FS_S1_PAR) && df->ability == ABILITY_LIMBER) return 0;
    if ((st & FS_S1_SLEEP) && (df->ability == ABILITY_INSOMNIA || df->ability == ABILITY_VITAL_SPIRIT)) return 0;
    return 1;
}
// stage change of a self-boosting / target-lowering effect: sum of stage deltas (x0.03/7 in the heuristic)
static int SelfStages(u8 effect)
{
    switch (effect)
    {
    case EFFECT_ATTACK_UP: case EFFECT_DEFENSE_UP: case EFFECT_SPEED_UP: case EFFECT_SPECIAL_ATTACK_UP: case EFFECT_SPECIAL_DEFENSE_UP: return 1;
    case EFFECT_ATTACK_UP_2: case EFFECT_DEFENSE_UP_2: case EFFECT_SPEED_UP_2: case EFFECT_SPECIAL_ATTACK_UP_2: case EFFECT_SPECIAL_DEFENSE_UP_2: return 2;
    case EFFECT_CALM_MIND: case EFFECT_DRAGON_DANCE: case EFFECT_BULK_UP: case EFFECT_COSMIC_POWER: return 2;
    case EFFECT_CURSE: return 1;   // +1 atk +1 def -1 spe for non-ghosts
    }
    return 0;
}
static int TargetStages(u8 effect)
{
    switch (effect)
    {
    case EFFECT_ATTACK_DOWN: case EFFECT_DEFENSE_DOWN: case EFFECT_SPEED_DOWN: case EFFECT_SPECIAL_ATTACK_DOWN: case EFFECT_SPECIAL_DEFENSE_DOWN: return 1;
    case EFFECT_ATTACK_DOWN_2: case EFFECT_DEFENSE_DOWN_2: case EFFECT_SPEED_DOWN_2: case EFFECT_SPECIAL_ATTACK_DOWN_2: case EFFECT_SPECIAL_DEFENSE_DOWN_2: return 2;
    }
    return 0;
}

// Expected heuristic change (side 0's view: +good for side 0) when `at` (side atSide) uses `move` on `df`, plus
// the probability that df is KO'd by it. dmgPen: the material weight of df's HP; koOut: P(KO).
// lastMon: df is its side's last standing mon, so a KO ends the game (value +-1 instead of a material loss)
static float MoveOutcome(const fs_state *s, int atSide, const struct VMon *at, const struct VMon *df, u16 move, float *koOut, int lastMon, float v0)
{
    const struct BattleMove *bm = fs_move(move);
    float hit, mx, e, pKO = 0, delta = 0, sign = atSide == 0 ? 1.0f : -1.0f;
    int defSide = atSide ^ 1;
    *koOut = 0;
    if (move == 0) return 0;
    e = MoveDamage(s, at, df, atSide, move, &hit, &mx);
    if (bm->effect == EFFECT_EXPLOSION)
    {
        // the user faints; the target takes a hit with halved defence (~2x)
        float hit2, mx2, e2 = 2.0f * MoveDamage(s, at, df, atSide, move, &hit2, &mx2);
        float need = mx2 > 0 ? (float)df->hp / (2.0f * mx2) : 2.0f;
        pKO = need <= 0.85f ? 1.0f : need > 1.0f ? 0.0f : (1.0f - need) / 0.15f;
        pKO *= hit2;
        delta = sign * (-KoLoss(at)) + pKO * (lastMon ? sign * 1.0f - v0 : sign * KoLoss(df)) + (1 - pKO) * sign * Clamp(e2 / (df->maxHP ? df->maxHP : 1), 0, 1) * HpWeight(df);
        *koOut = pKO;
        return delta;
    }
    if (mx > 0 && df->hp > 0)
    {
        float need = (float)df->hp / mx;
        pKO = need <= 0.85f ? 1.0f : need > 1.0f ? 0.0f : (1.0f - need) / 0.15f;
        pKO *= hit;
        delta += pKO * (lastMon ? sign * 1.0f - v0 : sign * KoLoss(df)) + (1 - pKO) * sign * Clamp(e / (df->maxHP ? df->maxHP : 1), 0, 1) * HpWeight(df);
        if (bm->effect == EFFECT_RECOIL || bm->effect == EFFECT_DOUBLE_EDGE)
            if (at->ability != ABILITY_ROCK_HEAD) delta -= sign * Clamp(e / 3.0f / (at->maxHP ? at->maxHP : 1), 0, 1) * HpWeight(at);
        if (bm->effect == EFFECT_ABSORB || bm->effect == EFFECT_DREAM_EATER)
            delta += sign * Clamp(e / 2.0f / (at->maxHP ? at->maxHP : 1), 0, 1) * HpWeight(at) * ((float)(at->maxHP - at->hp) / (at->maxHP ? at->maxHP : 1) > 0 ? 1 : 0);
        {
            u16 st = SecondaryStatus(bm->effect);
            if (st && StatusPossible(s, defSide, df, st))
                delta += sign * (bm->secondaryEffectChance ? bm->secondaryEffectChance : 100) / 100.0f * hit * (1 - pKO) * StatusPenalty7(st) * (float)df->hp / (df->maxHP ? df->maxHP : 1) / 7.0f;
        }
        {
            int ts = 0;
            switch (bm->effect) { case EFFECT_ATTACK_DOWN_HIT: case EFFECT_DEFENSE_DOWN_HIT: case EFFECT_SPEED_DOWN_HIT: case EFFECT_SPECIAL_ATTACK_DOWN_HIT: case EFFECT_SPECIAL_DEFENSE_DOWN_HIT: ts = 1; }
            if (ts && df->active) delta += sign * (bm->secondaryEffectChance ? bm->secondaryEffectChance : 100) / 100.0f * hit * (1 - pKO) * 0.03f / 7.0f;
            if (bm->effect == EFFECT_OVERHEAT) delta -= sign * 2 * 0.03f / 7.0f;
            if (bm->effect == EFFECT_SUPERPOWER) delta -= sign * 2 * 0.03f / 7.0f;
            if (bm->effect == EFFECT_ATTACK_UP_HIT || bm->effect == EFFECT_DEFENSE_UP_HIT) delta += sign * hit * (bm->secondaryEffectChance ? bm->secondaryEffectChance : 100) / 100.0f * 0.03f / 7.0f;
        }
        *koOut = pKO;
        return delta;
    }
    // non-damaging
    {
        u16 st = PrimaryStatus(bm->effect, move);
        float acc = bm->accuracy ? bm->accuracy / 100.0f : 1.0f;
        int ss = SelfStages(bm->effect), ts = TargetStages(bm->effect);
        if (st && StatusPossible(s, defSide, df, st)) return sign * acc * StatusPenalty7(st) * (float)df->hp / (df->maxHP ? df->maxHP : 1) / 7.0f;
        if (ss) { int room = 12 - at->stages[1]; if (room < 0) room = 0; return sign * (ss < room ? ss : room) * 0.03f / 7.0f; }
        if (ts && df->active) return sign * acc * (df->ability == ABILITY_CLEAR_BODY || df->ability == ABILITY_WHITE_SMOKE ? 0 : ts) * 0.03f / 7.0f;
        if (bm->effect == EFFECT_RESTORE_HP || bm->effect == EFFECT_SOFTBOILED || bm->effect == EFFECT_MORNING_SUN || bm->effect == EFFECT_SYNTHESIS || bm->effect == EFFECT_MOONLIGHT)
        {
            float missing = (float)(at->maxHP - at->hp) / (at->maxHP ? at->maxHP : 1), heal = missing < 0.5f ? missing : 0.5f;
            return sign * heal * HpWeight(at);
        }
        if (bm->effect == EFFECT_REST)
        {
            float missing = (float)(at->maxHP - at->hp) / (at->maxHP ? at->maxHP : 1);
            return sign * (missing * (1.0f / 7.0f) - StatusPenalty7(FS_S1_SLEEP) / 7.0f + (at->status1 ? StatusPenalty7(at->status1) / 7.0f : 0));
        }
        if (bm->effect == EFFECT_SUBSTITUTE && at->hp > at->maxHP / 4) return sign * (0.10f / 7.0f - 0.25f * HpWeight(at));
        if (bm->effect == EFFECT_REFLECT || bm->effect == EFFECT_LIGHT_SCREEN) return sign * 0.05f / 7.0f;
        if (bm->effect == EFFECT_SPIKES) return sign * 0.05f / 7.0f;
        if (bm->effect == EFFECT_LEECH_SEED && !(df->type1 == TYPE_GRASS || df->type2 == TYPE_GRASS)) return sign * acc * 0.08f / 7.0f;
        if (bm->effect == EFFECT_CONFUSE) return sign * acc * 0.08f / 7.0f;
    }
    return 0;
}

float fs_value_ply1(const fs_state *s, int side)
{
    struct P1Act acts[2][P1_MAX];
    struct VMon act[2], bench[2][FS_PARTY];
    fs_action fl[P1_MAX + 8];
    float M[P1_MAX * P1_MAX], sRow[P1_MAX], sCol[P1_MAX], v0, v = 0.0f;
    int n[2], p, i, a, b, alive[2];
    u32 sp[2];
    if (s->request != FS_REQ_TURN) return fs_value_basic(s, side);
    alive[0] = fs_alive_count(s, 0); alive[1] = fs_alive_count(s, 1);
    for (p = 0; p < 2; p++)
    {
        int m = fs_legal_actions(s, p, fl), k;
        if (m <= 0) return fs_value_basic(s, side);
        if (m > P1_MAX) m = P1_MAX;
        n[p] = m;
        for (k = 0; k < m; k++) { acts[p][k].a = fl[k]; acts[p][k].isSwitch = fl[k].type == FS_ACT_SWITCH; acts[p][k].slot = fl[k].slot; }
        FillActive(s, p, &act[p]);
        sp[p] = Speed(s, &act[p]);
        for (i = 0; i < FS_PARTY; i++) FillBench(s, p, i, &bench[p][i]);
    }
    v0 = fs_value_basic(s, 0);
    for (a = 0; a < n[0]; a++)
        for (b = 0; b < n[1]; b++)
        {
            const struct P1Act *A = &acts[0][a], *B = &acts[1][b];
            float cell = v0;
            if (A->isSwitch && B->isSwitch) { /* nothing happens */ }
            else if (A->isSwitch || B->isSwitch)
            {
                // the switch resolves first; the other side's move hits the incoming mon
                int mover = A->isSwitch ? 1 : 0, swSide = mover ^ 1;
                const struct P1Act *S = A->isSwitch ? A : B, *Mv = A->isSwitch ? B : A;
                const struct VMon *target = &bench[swSide][S->slot];
                u16 move = Mv->slot == 4 ? MOVE_STRUGGLE : act[mover].moves[Mv->slot];
                float ko;
                cell += MoveOutcome(s, mover, &act[mover], target, move, &ko, alive[swSide] <= 1, v0);
            }
            else
            {
                u16 mv0 = A->slot == 4 ? MOVE_STRUGGLE : act[0].moves[A->slot], mv1 = B->slot == 4 ? MOVE_STRUGGLE : act[1].moves[B->slot];
                s8 pr0 = fs_move(mv0)->priority, pr1 = fs_move(mv1)->priority;
                float pFirst0 = pr0 > pr1 ? 1.0f : pr0 < pr1 ? 0.0f : sp[0] > sp[1] ? 1.0f : sp[0] < sp[1] ? 0.0f : 0.5f;
                float ko0, ko1, d0 = MoveOutcome(s, 0, &act[0], &act[1], mv0, &ko0, alive[1] <= 1, v0), d1 = MoveOutcome(s, 1, &act[1], &act[0], mv1, &ko1, alive[0] <= 1, v0);
                int prot0 = fs_move(mv0)->effect == EFFECT_PROTECT, prot1 = fs_move(mv1)->effect == EFFECT_PROTECT;
                if (prot0) d1 = 0;
                if (prot1) d0 = 0;
                // first mover acts; if it KOs, the second never moves
                cell += pFirst0 * (d0 + (1 - ko0) * d1) + (1 - pFirst0) * (d1 + (1 - ko1) * d0);
            }
            M[a * n[1] + b] = cell;
        }
    Sim_RegretMatching(M, n[0], n[1], 30, 1, 1, 1, sRow, sCol);
    for (a = 0; a < n[0]; a++) for (b = 0; b < n[1]; b++) v += sRow[a] * M[a * n[1] + b] * sCol[b];
    if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
    return side == 0 ? v : -v;
}
