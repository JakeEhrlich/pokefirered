// Value-network state encoder (include/sim_encode.h, ai/DESIGN.md section 1).
#include <math.h>
#include <string.h>
#include "global.h"
#include "battle.h"
#include "pokemon.h"
#include "item.h"
#include "pokedex.h"
#include "data.h"
#include "sim.h"
#include "sim_encode.h"
#include "sim_agent.h"
#include "sim_globals.h"
#include "constants/species.h"
#include "constants/moves.h"
#include "constants/abilities.h"
#include "constants/battle_move_effects.h"
#include "constants/items.h"

// ---- mon float features
enum
{
    MF_STAT = 0,          // 6: log(stat)/8: maxHP, atk, def, spe, spa, spd
    MF_LEVEL = 6,
    MF_WEIGHT = 7,
    MF_GENDER = 8,        // 3
    MF_HOLDPARAM = 11,
    MF_TYPE1 = 12,        // 18
    MF_TYPE2 = 30,        // 18
    MF_HPFRAC = 48,
    MF_LOGMAXHP = 49,
    MF_FAINTED = 50,
    MF_PSN = 51, MF_BRN = 52, MF_FRZ = 53, MF_PAR = 54, MF_TOX = 55, MF_TOXCTR = 56,
    MF_SLP = 57, MF_SLPELAPSED = 58, MF_SLPKNOWN = 59,
    MF_ACTIVE = 60,
    MF_ISEGG = 61,
    // active extras
    MF_STAGE = 62,        // 7: (stage-6)/6 atk def spe spa spd acc eva
    MF_S2 = 69,           // status2 flags, 20
    MF_S2_CONF = 69, MF_S2_CONFELAPSED = 70, MF_S2_UPROAR = 71, MF_S2_BIDE = 72, MF_S2_LOCKCONF = 73, MF_S2_MULTI = 74,
    MF_S2_WRAPPED = 75, MF_S2_INFAT = 76, MF_S2_FOCUS = 77, MF_S2_TRANSFORMED = 78, MF_S2_RECHARGE = 79, MF_S2_RAGE = 80,
    MF_S2_SUB = 81, MF_S2_SUBHP = 82, MF_S2_DBOND = 83, MF_S2_ESCPREV = 84, MF_S2_NIGHTMARE = 85, MF_S2_CURSED = 86,
    MF_S2_FORESIGHT = 87, MF_S2_DCURL = 88, MF_S2_TORMENT = 89,
    MF_S3 = 90,           // status3, 16
    MF_S3_LEECH = 90, MF_S3_LEECHMINE = 91, MF_S3_ALWAYSHIT = 92, MF_S3_PERISH = 93, MF_S3_PERISHT = 94, MF_S3_ONAIR = 95,
    MF_S3_UNDERGROUND = 96, MF_S3_UNDERWATER = 97, MF_S3_MINIMIZED = 98, MF_S3_CHARGED = 99, MF_S3_ROOTED = 100, MF_S3_YAWN = 101,
    MF_S3_YAWNT = 102, MF_S3_IMPRISON = 103, MF_S3_GRUDGE = 104, MF_S3_MUDSPORT = 105, MF_S3_WATERSPORT = 106,
    MF_D_DISABLE = 107, MF_D_ENCORE = 108, MF_D_PROTECT = 109, MF_D_STOCKPILE = 110, MF_D_TAUNT = 111, MF_D_ROLLOUT = 112,
    MF_D_FURYCUTTER = 113, MF_D_CHARGE = 114, MF_D_FIRSTTURN = 115, MF_D_TRUANT = 116, MF_D_RECHARGE = 117, MF_D_SUREHIT = 118,
    MF_LASTMOVE = 119, MF_WISH = 120, MF_FSIGHT = 121, MF_FSIGHTDMG = 122, MF_FSIGHTMINE = 123,
    MF_ISREQUEST = 124,   // this battler is the one being asked
    MF_COUNT = 125
};
// ---- move float features
enum
{
    VF_TYPE = 0,          // 18
    VF_POWER = 18, VF_ACC = 19, VF_PRIO = 20, VF_PPFRAC = 21, VF_NOPP = 22, VF_CHANCE = 23,
    VF_TARGET = 24,       // 7 target flags
    VF_FLAGS = 31,        // 6
    VF_PHYSICAL = 37, VF_STATUS = 38,
    VF_DISABLED = 39, VF_ENCORED = 40, VF_TAUNTED = 41, VF_LOCKED = 42, VF_LASTUSED = 43, VF_MIMICKED = 44, VF_HASEFFECT = 45,
    VF_COUNT = 46
};
enum
{
    SF_REFLECT = 0, SF_LSCREEN = 1, SF_MIST = 2, SF_SAFEGUARD = 3, SF_SPIKES = 4 /*4*/, SF_FOLLOWME = 8, SF_ALIVE = 9, SF_FAINTED = 10,
    SF_FSIGHT = 11, SF_COUNT = 12
};
enum
{
    FF_WEATHER = 0 /*5: none rain sun sand hail*/, FF_WDUR = 5, FF_WPERM = 6, FF_TURN = 7, FF_REQSWITCH = 8, FF_REQMINE = 9, FF_REQANY = 10, FF_COUNT = 11
};

_Static_assert(MF_COUNT <= SIMENC_MON_F, "mon features");
_Static_assert(VF_COUNT <= SIMENC_MOVE_F, "move features");
_Static_assert(SF_COUNT <= SIMENC_SIDE_F, "side features");
_Static_assert(FF_COUNT <= SIMENC_FIELD_F, "field features");
// ---- extra (engine-computed) per-side features, all relative to the opponent's current active battler
enum
{
    XF_ACT_EXPDMG = 0,      // best expected damage of the active, uncapped, /100
    XF_ACT_KOFRAC = 1,      // best expected damage capped, as a fraction of their current HP
    XF_ACT_CANKO = 2,       // best raw damage >= their HP
    XF_ACT_CAN2HKO = 3,
    XF_ACT_BESTMULT = 4,    // best type multiplier among damaging moves, /4
    XF_ACT_HAS_SE = 5,
    XF_ACT_HAS_DMG_PP = 6,  // a damaging move with PP left
    XF_ACT_DMG_PP = 7,      // PP on damaging moves /40
    XF_ACT_PRIORITY = 8,    // has a damaging move with priority > 0
    XF_ACT_STATUS_MOVES = 9,// status moves /4
    XF_ACT_FASTER = 10,     // effective speed > theirs
    XF_SPEED_LOGRATIO = 11, // log(mine/theirs) clipped to [-1, 1]
    XF_ACT_HP = 12,         // absolute HP /200
    XF_ACT_LEVEL = 13,
    XF_ACT_WEATHER_BOOST = 14,
    XF_ACT_WEATHER_NERF = 15,
    XF_ACT_FAINTED = 16,    // the active slot holds a fainted mon (replacement pending)
    XF_TEAM_EXPDMG = 17,    // sum over alive mons of best expected damage, uncapped, /400
    XF_TEAM_KOFRAC = 18,    // best over alive mons of the capped fraction
    XF_TEAM_HP = 19,        // sum of HP /600
    XF_TEAM_MAXHP = 20,     // sum of max HP /600
    XF_TEAM_FASTER = 21,    // mons faster than their active /6
    XF_TEAM_SE = 22,        // mons with a super-effective damaging move /6
    XF_TEAM_CANKO = 23,     // mons that can KO their active /6
    XF_TEAM_LEVEL = 24,     // mean level /100
    XF_BENCH_SAFE = 25,     // bench mons taking < 1/3 of max HP from their best move /6
    XF_BENCH_RESIST = 26,   // 1 - min over bench of (their best raw damage / max HP)
    XF_COUNT = 27
};
_Static_assert(XF_COUNT <= SIMENC_EXTRA_F, "extra features");

static void FillBattleMon(struct BattlePokemon *out, struct Pokemon *mon)
{
    int i;
    memset(out, 0, sizeof(*out));
    out->species = GetMonData(mon, MON_DATA_SPECIES);
    out->attack = GetMonData(mon, MON_DATA_ATK); out->defense = GetMonData(mon, MON_DATA_DEF); out->speed = GetMonData(mon, MON_DATA_SPEED);
    out->spAttack = GetMonData(mon, MON_DATA_SPATK); out->spDefense = GetMonData(mon, MON_DATA_SPDEF);
    for (i = 0; i < 4; i++) { out->moves[i] = GetMonData(mon, MON_DATA_MOVE1 + i); out->pp[i] = GetMonData(mon, MON_DATA_PP1 + i); }
    for (i = 0; i < NUM_BATTLE_STATS; i++) out->statStages[i] = 6;
    out->abilityNum = GetMonData(mon, MON_DATA_ABILITY_NUM);
    out->ability = out->species < NUM_SPECIES ? (gSpeciesInfo[out->species].abilities[out->abilityNum] ? gSpeciesInfo[out->species].abilities[out->abilityNum] : gSpeciesInfo[out->species].abilities[0]) : 0;
    out->type1 = out->species < NUM_SPECIES ? gSpeciesInfo[out->species].types[0] : 0;
    out->type2 = out->species < NUM_SPECIES ? gSpeciesInfo[out->species].types[1] : 0;
    out->hp = GetMonData(mon, MON_DATA_HP); out->maxHP = GetMonData(mon, MON_DATA_MAX_HP);
    out->level = GetMonData(mon, MON_DATA_LEVEL); out->item = GetMonData(mon, MON_DATA_HELD_ITEM);
    out->status1 = GetMonData(mon, MON_DATA_STATUS);
    out->personality = GetMonData(mon, MON_DATA_PERSONALITY);
}

static float EffectiveSpeed(struct BattlePokemon *m)
{
    float sp = (float)m->speed * gStatStageRatios[m->statStages[STAT_SPEED]][0] / gStatStageRatios[m->statStages[STAT_SPEED]][1];
    if (m->status1 & STATUS1_PARALYSIS) sp /= 4;
    return sp;
}

// Best damaging move of atk against def: expected (capped) and raw damage, multiplier, flags.
static void BestAttack(struct BattlePokemon *atk, struct BattlePokemon *def, u8 atkB, u8 defB, int usePP,
                       int *bestExp, int *bestRaw, int *bestMult, int *hasSE, int *hasDmgPP, int *dmgPP, int *prio, int *statusMoves)
{
    int j;
    *bestExp = *bestRaw = *bestMult = *hasSE = *hasDmgPP = *dmgPP = *prio = *statusMoves = 0;
    for (j = 0; j < 4; j++)
    {
        u16 mv = atk->moves[j];
        int raw, e, mult;
        if (mv == MOVE_NONE || mv >= MOVES_COUNT) continue;
        if (gBattleMoves[mv].power == 0) { (*statusMoves)++; continue; }
        if (usePP && atk->pp[j] == 0) continue;
        *dmgPP += atk->pp[j];
        *hasDmgPP = 1;
        if (gBattleMoves[mv].priority > 0) *prio = 1;
        e = Sim_EstimateDamageMons(atk, def, mv, atkB, defB, &raw);
        mult = Sim_TypeMultiplier(gBattleMoves[mv].type, def->type1, def->type2);
        if (mult >= 200) *hasSE = 1;
        if (mult > *bestMult) *bestMult = mult;
        if (e > *bestExp) *bestExp = e;
        if (raw > *bestRaw) *bestRaw = raw;
    }
}

static void EncodeExtra(struct BattleSim *sim, int side, int gameSide, float *x)
{
    u8 me = gameSide, opp = gameSide ^ 1;
    struct BattlePokemon *act = &gBattleMons[me], *def = &gBattleMons[opp];
    struct Pokemon *party = Sim_Party(sim, gameSide);
    struct BattlePokemon tmp, oppTmp;
    int bestExp, bestRaw, bestMult, hasSE, hasDmgPP, dmgPP, prio, statusMoves;
    int i, alive = 0, levelSum = 0, hpSum = 0, maxHpSum = 0, teamExp = 0, teamFaster = 0, teamSE = 0, teamKO = 0, benchSafe = 0, bench = 0;
    float teamKoFrac = 0.0f, benchResist = 1.0f, oppSpeed;
    int oppBestExp, oppBestRaw, d1, d2, d3, d4, d5, d6;
    memset(x, 0, sizeof(float) * SIMENC_EXTRA_F);
    if (def->hp == 0 || def->species == SPECIES_NONE) { oppTmp = *def; oppTmp.hp = oppTmp.maxHP ? oppTmp.maxHP : 1; def = &oppTmp; }
    oppSpeed = EffectiveSpeed(def);
    if (act->hp > 0 && act->species != SPECIES_NONE)
    {
        BestAttack(act, def, me, opp, 1, &bestExp, &bestRaw, &bestMult, &hasSE, &hasDmgPP, &dmgPP, &prio, &statusMoves);
        x[XF_ACT_EXPDMG] = bestRaw * (bestExp > 0 ? 1.0f : 0.0f) / 100.0f;
        x[XF_ACT_KOFRAC] = def->hp ? (float)bestExp / def->hp : 0.0f;
        x[XF_ACT_CANKO] = bestRaw >= def->hp;
        x[XF_ACT_CAN2HKO] = 2 * bestRaw >= def->hp;
        x[XF_ACT_BESTMULT] = bestMult / 400.0f;
        x[XF_ACT_HAS_SE] = hasSE; x[XF_ACT_HAS_DMG_PP] = hasDmgPP; x[XF_ACT_DMG_PP] = dmgPP / 40.0f;
        x[XF_ACT_PRIORITY] = prio; x[XF_ACT_STATUS_MOVES] = statusMoves / 4.0f;
        {
            float sp = EffectiveSpeed(act);
            x[XF_ACT_FASTER] = sp > oppSpeed;
            x[XF_SPEED_LOGRATIO] = (sp > 0 && oppSpeed > 0) ? logf(sp / oppSpeed) : 0.0f;
            if (x[XF_SPEED_LOGRATIO] > 1) x[XF_SPEED_LOGRATIO] = 1; if (x[XF_SPEED_LOGRATIO] < -1) x[XF_SPEED_LOGRATIO] = -1;
        }
        x[XF_ACT_HP] = act->hp / 200.0f;
        x[XF_ACT_LEVEL] = act->level / 100.0f;
        {
            u16 w = gBattleWeather;
            int j;
            for (j = 0; j < 4; j++)
            {
                u16 mv = act->moves[j];
                u8 t;
                if (mv == MOVE_NONE || mv >= MOVES_COUNT || gBattleMoves[mv].power == 0) continue;
                t = gBattleMoves[mv].type;
                if (((w & B_WEATHER_RAIN) && t == TYPE_WATER) || ((w & B_WEATHER_SUN) && t == TYPE_FIRE)) x[XF_ACT_WEATHER_BOOST] = 1;
                if (((w & B_WEATHER_RAIN) && t == TYPE_FIRE) || ((w & B_WEATHER_SUN) && t == TYPE_WATER)) x[XF_ACT_WEATHER_NERF] = 1;
            }
        }
    }
    else
        x[XF_ACT_FAINTED] = 1;
    // their best raw damage against my bench (for safe switch-ins)
    for (i = 0; i < PARTY_SIZE; i++)
    {
        struct Pokemon *mon = &party[i];
        struct BattlePokemon *m;
        u16 species = GetMonData(mon, MON_DATA_SPECIES_OR_EGG);
        int isActive = (i == gBattlerPartyIndexes[me]);
        if (species == SPECIES_NONE || species == SPECIES_EGG || species >= NUM_SPECIES) continue;
        if (GetMonData(mon, MON_DATA_HP) == 0) continue;
        if (isActive) m = act; else { FillBattleMon(&tmp, mon); m = &tmp; }
        alive++;
        levelSum += m->level; hpSum += m->hp; maxHpSum += m->maxHP;
        BestAttack(m, def, me, opp, 1, &bestExp, &bestRaw, &bestMult, &hasSE, &hasDmgPP, &dmgPP, &prio, &statusMoves);
        teamExp += bestRaw * (bestExp > 0);
        if (def->hp && (float)bestExp / def->hp > teamKoFrac) teamKoFrac = (float)bestExp / def->hp;
        if (EffectiveSpeed(m) > oppSpeed) teamFaster++;
        if (hasSE) teamSE++;
        if (bestRaw >= def->hp) teamKO++;
        if (!isActive)
        {
            float frac;
            bench++;
            BestAttack(def, m, opp, me, 0, &oppBestExp, &oppBestRaw, &d1, &d2, &d3, &d4, &d5, &d6);
            frac = m->maxHP ? (float)oppBestRaw / m->maxHP : 1.0f;
            if (frac < 1.0f / 3.0f) benchSafe++;
            if (frac < benchResist) benchResist = frac;
        }
    }
    x[XF_TEAM_EXPDMG] = teamExp / 400.0f;
    x[XF_TEAM_KOFRAC] = teamKoFrac > 1 ? 1 : teamKoFrac;
    x[XF_TEAM_HP] = hpSum / 600.0f; x[XF_TEAM_MAXHP] = maxHpSum / 600.0f;
    x[XF_TEAM_FASTER] = teamFaster / 6.0f; x[XF_TEAM_SE] = teamSE / 6.0f; x[XF_TEAM_CANKO] = teamKO / 6.0f;
    x[XF_TEAM_LEVEL] = alive ? levelSum / (100.0f * alive) : 0.0f;
    x[XF_BENCH_SAFE] = benchSafe / 6.0f;
    x[XF_BENCH_RESIST] = bench ? 1.0f - (benchResist > 1 ? 1 : benchResist) : 0.0f;
}


static float Log8(u32 v) { return v ? logf((float)v) / 8.0f : 0.0f; }

static void EncodeMoveToken(float *f, int32_t *outI, int tok, u16 move, u8 pp, u8 maxPP, u8 active, struct DisableStruct *d,
                            u32 status2, u16 locked, u16 lastMove, u8 slot)
{
    const struct BattleMove *bm;
    int t;
    memset(f, 0, sizeof(float) * SIMENC_MOVE_F);
    outI[SIMENC_I_MOVEID + tok - 12] = move;
    if (move == MOVE_NONE || move >= MOVES_COUNT) { outI[SIMENC_I_PRESENT + tok] = 0; return; }
    outI[SIMENC_I_PRESENT + tok] = 1;
    bm = &gBattleMoves[move];
    if (bm->type < 18) f[VF_TYPE + bm->type] = 1.0f;
    f[VF_POWER] = bm->power / 250.0f;
    f[VF_ACC] = bm->accuracy / 100.0f;
    f[VF_PRIO] = bm->priority / 7.0f;
    f[VF_PPFRAC] = maxPP ? (float)pp / maxPP : 0.0f;
    f[VF_NOPP] = pp == 0;
    f[VF_CHANCE] = bm->secondaryEffectChance / 100.0f;
    if (bm->target == 0) f[VF_TARGET] = 1.0f;
    for (t = 0; t < 7; t++) if (bm->target & (1 << t)) f[VF_TARGET + (t < 6 ? t + 1 : 6)] = 1.0f;
    for (t = 0; t < 6; t++) if (bm->flags & (1 << t)) f[VF_FLAGS + t] = 1.0f;
    f[VF_PHYSICAL] = bm->power > 0 && bm->type < TYPE_MYSTERY;
    f[VF_STATUS] = bm->power == 0;
    f[VF_HASEFFECT] = bm->effect != EFFECT_HIT;
    if (active)
    {
        f[VF_DISABLED] = d->disableTimer && d->disabledMove == move;
        f[VF_ENCORED] = d->encoreTimer && d->encoredMovePos == slot;
        f[VF_TAUNTED] = d->tauntTimer && bm->power == 0;
        f[VF_LOCKED] = (status2 & STATUS2_MULTIPLETURNS) && locked == move;
        f[VF_LASTUSED] = lastMove == move;
        f[VF_MIMICKED] = (d->mimickedMoves >> slot) & 1;
    }
}

int Sim_EncodeState(struct BattleSim *sim, int side, float *outF, int32_t *outI)
{
    int s, i, k, terminal = 0;
    float *monF = outF;
    float *moveF = outF + SIMENC_MONS * SIMENC_MON_F;
    float *sideF = moveF + SIMENC_MOVES * SIMENC_MOVE_F;
    float *fieldF = sideF + 2 * SIMENC_SIDE_F;
    float *extraF = fieldF + SIMENC_FIELD_F;
    Sim_Bind(sim);
    memset(outF, 0, sizeof(float) * SIMENC_FLOATS);
    memset(outI, 0, sizeof(int32_t) * SIMENC_INTS);

    if (sim->finished)
    {
        int won = gBattleOutcome == B_OUTCOME_WON, lost = gBattleOutcome == B_OUTCOME_LOST;
        if (side) { int t = won; won = lost; lost = t; }
        terminal = won ? 1 : lost ? 2 : 3;
    }
    outI[SIMENC_I_TERMINAL] = terminal;

    for (s = 0; s < 2; s++)
    {
        int gameSide = side ^ s;                       // s = 0: my side, 1: opponent
        u8 battler = gameSide;                         // singles: battler id == side
        struct Pokemon *party = Sim_Party(sim, gameSide);
        int activeSlot = gBattlerPartyIndexes[battler];
        struct BattlePokemon *bmon = &gBattleMons[battler];
        struct DisableStruct *d = &gDisableStructs[battler];
        float *sf = sideF + s * SIMENC_SIDE_F;
        int alive = 0, fainted = 0;

        for (i = 0; i < PARTY_SIZE; i++)
        {
            int m = s * PARTY_SIZE + i;
            float *f = monF + m * SIMENC_MON_F;
            struct Pokemon *mon = &party[i];
            u16 species = GetMonData(mon, MON_DATA_SPECIES_OR_EGG);
            int active = (i == activeSlot);
            u16 item, ability, moves[4];
            u8 pp[4], maxPP[4];
            u32 status1, status2 = 0, status3 = 0;
            u16 hp, maxHP;
            u8 type1, type2, level, gender;
            int j;

            if (species == SPECIES_NONE || species >= NUM_SPECIES || GetMonData(mon, MON_DATA_IS_EGG))
            {
                outI[SIMENC_I_PRESENT + m] = 0;
                outI[SIMENC_I_TOKCAT + m] = s ? SIMENC_CAT_OPP_BENCH_MON : SIMENC_CAT_MY_BENCH_MON;
                for (j = 0; j < 4; j++)
                {
                    int tok = 12 + m * 4 + j;
                    outI[SIMENC_I_PRESENT + tok] = 0;
                    outI[SIMENC_I_TOKCAT + tok] = s ? SIMENC_CAT_OPP_BENCH_MOVE : SIMENC_CAT_MY_BENCH_MOVE;
                }
                f[MF_ISEGG] = species == SPECIES_EGG;
                continue;
            }
            outI[SIMENC_I_PRESENT + m] = 1;
            outI[SIMENC_I_TOKCAT + m] = s ? (active ? SIMENC_CAT_OPP_ACTIVE_MON : SIMENC_CAT_OPP_BENCH_MON)
                                          : (active ? SIMENC_CAT_MY_ACTIVE_MON : SIMENC_CAT_MY_BENCH_MON);
            level = GetMonData(mon, MON_DATA_LEVEL);
            gender = GetMonGender(mon);
            if (active)
            {
                hp = bmon->hp; maxHP = bmon->maxHP; item = bmon->item; ability = bmon->ability;
                type1 = bmon->type1; type2 = bmon->type2; status1 = bmon->status1; status2 = bmon->status2; status3 = gStatuses3[battler];
                f[MF_STAT + 0] = Log8(bmon->maxHP); f[MF_STAT + 1] = Log8(bmon->attack); f[MF_STAT + 2] = Log8(bmon->defense);
                f[MF_STAT + 3] = Log8(bmon->speed); f[MF_STAT + 4] = Log8(bmon->spAttack); f[MF_STAT + 5] = Log8(bmon->spDefense);
                for (j = 0; j < 4; j++) { moves[j] = bmon->moves[j]; pp[j] = bmon->pp[j]; maxPP[j] = moves[j] ? CalculatePPWithBonus(moves[j], bmon->ppBonuses, j) : 0; }
                species = bmon->species < NUM_SPECIES ? bmon->species : species;
            }
            else
            {
                u8 abilityNum = GetMonData(mon, MON_DATA_ABILITY_NUM);
                hp = GetMonData(mon, MON_DATA_HP); maxHP = GetMonData(mon, MON_DATA_MAX_HP);
                item = GetMonData(mon, MON_DATA_HELD_ITEM);
                ability = gSpeciesInfo[species].abilities[abilityNum & 1] ? gSpeciesInfo[species].abilities[abilityNum & 1] : gSpeciesInfo[species].abilities[0];
                type1 = gSpeciesInfo[species].types[0]; type2 = gSpeciesInfo[species].types[1];
                status1 = GetMonData(mon, MON_DATA_STATUS);
                f[MF_STAT + 0] = Log8(maxHP); f[MF_STAT + 1] = Log8(GetMonData(mon, MON_DATA_ATK)); f[MF_STAT + 2] = Log8(GetMonData(mon, MON_DATA_DEF));
                f[MF_STAT + 3] = Log8(GetMonData(mon, MON_DATA_SPEED)); f[MF_STAT + 4] = Log8(GetMonData(mon, MON_DATA_SPATK)); f[MF_STAT + 5] = Log8(GetMonData(mon, MON_DATA_SPDEF));
                for (j = 0; j < 4; j++) { moves[j] = GetMonData(mon, MON_DATA_MOVE1 + j); pp[j] = GetMonData(mon, MON_DATA_PP1 + j); maxPP[j] = moves[j] ? CalculatePPWithBonus(moves[j], GetMonData(mon, MON_DATA_PP_BONUSES), j) : 0; }
            }
            if (hp) alive++; else fainted++;
            outI[SIMENC_I_ITEM + m] = item < ITEMS_COUNT ? item : 0;
            outI[SIMENC_I_ABILITY + m] = ability < ABILITIES_COUNT ? ability : 0;
            f[MF_LEVEL] = level / 100.0f;
            f[MF_WEIGHT] = Log8(GetPokedexHeightWeight(SpeciesToNationalPokedexNum(species), 1));
            f[MF_GENDER + (gender == MON_MALE ? 0 : gender == MON_FEMALE ? 1 : 2)] = 1.0f;
            f[MF_HOLDPARAM] = item < ITEMS_COUNT ? ItemId_GetHoldEffectParam(item) / 255.0f : 0.0f;
            if (type1 < 18) f[MF_TYPE1 + type1] = 1.0f;
            if (type2 < 18) f[MF_TYPE2 + type2] = 1.0f;
            f[MF_HPFRAC] = maxHP ? (float)hp / maxHP : 0.0f;
            f[MF_LOGMAXHP] = Log8(maxHP);
            f[MF_FAINTED] = hp == 0;
            f[MF_PSN] = (status1 & STATUS1_POISON) != 0; f[MF_BRN] = (status1 & STATUS1_BURN) != 0; f[MF_FRZ] = (status1 & STATUS1_FREEZE) != 0;
            f[MF_PAR] = (status1 & STATUS1_PARALYSIS) != 0; f[MF_TOX] = (status1 & STATUS1_TOXIC_POISON) != 0;
            f[MF_TOXCTR] = ((status1 & STATUS1_TOXIC_COUNTER) >> 8) / 16.0f;
            f[MF_SLP] = (status1 & STATUS1_SLEEP) != 0;
            if (status1 & STATUS1_SLEEP) { f[MF_SLPELAPSED] = sim->sleepElapsed[gameSide][i] / 5.0f; f[MF_SLPKNOWN] = sim->sleepKnown[gameSide][i]; }
            f[MF_ACTIVE] = active;
            if (active)
            {
                f[MF_STAGE + 0] = (bmon->statStages[STAT_ATK] - 6) / 6.0f; f[MF_STAGE + 1] = (bmon->statStages[STAT_DEF] - 6) / 6.0f;
                f[MF_STAGE + 2] = (bmon->statStages[STAT_SPEED] - 6) / 6.0f; f[MF_STAGE + 3] = (bmon->statStages[STAT_SPATK] - 6) / 6.0f;
                f[MF_STAGE + 4] = (bmon->statStages[STAT_SPDEF] - 6) / 6.0f; f[MF_STAGE + 5] = (bmon->statStages[STAT_ACC] - 6) / 6.0f;
                f[MF_STAGE + 6] = (bmon->statStages[STAT_EVASION] - 6) / 6.0f;
                f[MF_S2_CONF] = (status2 & STATUS2_CONFUSION) != 0;
                if (status2 & STATUS2_CONFUSION) f[MF_S2_CONFELAPSED] = sim->confusionElapsed[battler] / 5.0f;
                f[MF_S2_UPROAR] = (status2 & STATUS2_UPROAR) != 0; f[MF_S2_BIDE] = (status2 & STATUS2_BIDE) != 0;
                f[MF_S2_LOCKCONF] = (status2 & STATUS2_LOCK_CONFUSE) != 0; f[MF_S2_MULTI] = (status2 & STATUS2_MULTIPLETURNS) != 0;
                f[MF_S2_WRAPPED] = (status2 & STATUS2_WRAPPED) != 0; f[MF_S2_INFAT] = (status2 & STATUS2_INFATUATION) != 0;
                f[MF_S2_FOCUS] = (status2 & STATUS2_FOCUS_ENERGY) != 0; f[MF_S2_TRANSFORMED] = (status2 & STATUS2_TRANSFORMED) != 0;
                f[MF_S2_RECHARGE] = (status2 & STATUS2_RECHARGE) != 0; f[MF_S2_RAGE] = (status2 & STATUS2_RAGE) != 0;
                f[MF_S2_SUB] = (status2 & STATUS2_SUBSTITUTE) != 0; f[MF_S2_SUBHP] = maxHP ? (float)d->substituteHP / maxHP : 0.0f;
                f[MF_S2_DBOND] = (status2 & STATUS2_DESTINY_BOND) != 0; f[MF_S2_ESCPREV] = (status2 & STATUS2_ESCAPE_PREVENTION) != 0;
                f[MF_S2_NIGHTMARE] = (status2 & STATUS2_NIGHTMARE) != 0; f[MF_S2_CURSED] = (status2 & STATUS2_CURSED) != 0;
                f[MF_S2_FORESIGHT] = (status2 & STATUS2_FORESIGHT) != 0; f[MF_S2_DCURL] = (status2 & STATUS2_DEFENSE_CURL) != 0;
                f[MF_S2_TORMENT] = (status2 & STATUS2_TORMENT) != 0;
                f[MF_S3_LEECH] = (status3 & STATUS3_LEECHSEED) != 0;
                if (status3 & STATUS3_LEECHSEED) f[MF_S3_LEECHMINE] = ((status3 & STATUS3_LEECHSEED_BATTLER) & BIT_SIDE) == side; // seeder on the viewer's side
                f[MF_S3_ALWAYSHIT] = (status3 & STATUS3_ALWAYS_HITS) != 0;
                f[MF_S3_PERISH] = (status3 & STATUS3_PERISH_SONG) != 0; f[MF_S3_PERISHT] = d->perishSongTimer / 3.0f;
                f[MF_S3_ONAIR] = (status3 & STATUS3_ON_AIR) != 0; f[MF_S3_UNDERGROUND] = (status3 & STATUS3_UNDERGROUND) != 0;
                f[MF_S3_UNDERWATER] = (status3 & STATUS3_UNDERWATER) != 0; f[MF_S3_MINIMIZED] = (status3 & STATUS3_MINIMIZED) != 0;
                f[MF_S3_CHARGED] = (status3 & STATUS3_CHARGED_UP) != 0; f[MF_S3_ROOTED] = (status3 & STATUS3_ROOTED) != 0;
                f[MF_S3_YAWN] = (status3 & STATUS3_YAWN) != 0; f[MF_S3_YAWNT] = ((status3 & STATUS3_YAWN) >> 11) / 2.0f;
                f[MF_S3_IMPRISON] = (status3 & STATUS3_IMPRISONED_OTHERS) != 0; f[MF_S3_GRUDGE] = (status3 & STATUS3_GRUDGE) != 0;
                f[MF_S3_MUDSPORT] = (status3 & STATUS3_MUDSPORT) != 0; f[MF_S3_WATERSPORT] = (status3 & STATUS3_WATERSPORT) != 0;
                f[MF_D_DISABLE] = d->disableTimer != 0; f[MF_D_ENCORE] = d->encoreTimer != 0;
                f[MF_D_PROTECT] = d->protectUses > 4 ? 1.0f : d->protectUses / 4.0f;
                f[MF_D_STOCKPILE] = d->stockpileCounter / 3.0f; f[MF_D_TAUNT] = d->tauntTimer / 2.0f;
                f[MF_D_ROLLOUT] = d->rolloutTimer / 5.0f; f[MF_D_FURYCUTTER] = d->furyCutterCounter / 5.0f;
                f[MF_D_CHARGE] = d->chargeTimer / 2.0f; f[MF_D_FIRSTTURN] = d->isFirstTurn != 0;
                f[MF_D_TRUANT] = d->truantCounter; f[MF_D_RECHARGE] = d->rechargeTimer != 0;
                f[MF_D_SUREHIT] = d->battlerWithSureHit != 0;
                f[MF_LASTMOVE] = gLastMoves[battler] != 0 && gLastMoves[battler] != 0xFFFF;
                f[MF_WISH] = gWishFutureKnock.wishCounter[battler] / 2.0f;
                f[MF_FSIGHT] = gWishFutureKnock.futureSightCounter[battler] / 3.0f;
                if (gWishFutureKnock.futureSightCounter[battler])
                {
                    f[MF_FSIGHTDMG] = maxHP ? (float)gWishFutureKnock.futureSightDmg[battler] / maxHP : 0.0f;
                    f[MF_FSIGHTMINE] = (gWishFutureKnock.futureSightAttacker[battler] & BIT_SIDE) == side; // attacker on the viewer's side
                    sf[SF_FSIGHT] = 1.0f;
                }
                f[MF_ISREQUEST] = sim->requestKind != SIM_REQ_NONE && sim->requestBattler == battler;
            }
            for (j = 0; j < 4; j++)
            {
                int tok = 12 + m * 4 + j;
                outI[SIMENC_I_TOKCAT + tok] = s ? (active ? SIMENC_CAT_OPP_ACTIVE_MOVE : SIMENC_CAT_OPP_BENCH_MOVE)
                                                : (active ? SIMENC_CAT_MY_ACTIVE_MOVE : SIMENC_CAT_MY_BENCH_MOVE);
                EncodeMoveToken(moveF + (m * 4 + j) * SIMENC_MOVE_F, outI, tok, moves[j], pp[j], maxPP[j], active, d, status2,
                                gLockedMoves[battler], gLastMoves[battler], j);
            }
        }
        sf[SF_REFLECT] = gSideTimers[gameSide].reflectTimer / 5.0f; sf[SF_LSCREEN] = gSideTimers[gameSide].lightscreenTimer / 5.0f;
        sf[SF_MIST] = gSideTimers[gameSide].mistTimer / 5.0f; sf[SF_SAFEGUARD] = gSideTimers[gameSide].safeguardTimer / 5.0f;
        k = gSideTimers[gameSide].spikesAmount; if (k > 3) k = 3;
        sf[SF_SPIKES + k] = 1.0f;
        sf[SF_FOLLOWME] = gSideTimers[gameSide].followmeTimer != 0;
        sf[SF_ALIVE] = alive / 6.0f; sf[SF_FAINTED] = fainted / 6.0f;
        outI[SIMENC_I_TOKCAT + 60 + s] = s ? SIMENC_CAT_OPP_SIDE : SIMENC_CAT_MY_SIDE;
        outI[SIMENC_I_PRESENT + 60 + s] = 1;
        if (!sim->finished)
            EncodeExtra(sim, side, gameSide, extraF + s * SIMENC_EXTRA_F);
    }
    {
        u16 w = gBattleWeather;
        int idx = (w & B_WEATHER_RAIN) ? 1 : (w & B_WEATHER_SUN) ? 2 : (w & B_WEATHER_SANDSTORM) ? 3 : (w & B_WEATHER_HAIL) ? 4 : 0;
        fieldF[FF_WEATHER + idx] = 1.0f;
        if (idx) fieldF[FF_WDUR] = gWishFutureKnock.weatherDuration / 5.0f;
        fieldF[FF_WPERM] = (w & (B_WEATHER_RAIN_PERMANENT | B_WEATHER_SUN_PERMANENT | B_WEATHER_SANDSTORM_PERMANENT)) != 0;
        fieldF[FF_TURN] = sim->turnCount > 50 ? 1.0f : sim->turnCount / 50.0f;
        fieldF[FF_REQSWITCH] = sim->requestKind == SIM_REQ_SWITCH;
        fieldF[FF_REQMINE] = sim->requestKind != SIM_REQ_NONE && (sim->requestBattler & BIT_SIDE) == side;
        fieldF[FF_REQANY] = sim->requestKind != SIM_REQ_NONE;
        outI[SIMENC_I_TOKCAT + 62] = SIMENC_CAT_FIELD;
        outI[SIMENC_I_PRESENT + 62] = 1;
    }
    return terminal;
}
