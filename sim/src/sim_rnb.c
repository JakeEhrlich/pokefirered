// The "Run and Bun" (1.07) trainer AI as an arena agent ("rnb"), kept to what exists in Gen 3 (FireRed).
//
// Source: "AI Document for RnB (1.07)" by Croven. It is a move-scoring AI: every legal move gets a score
// (0 here = the game's default of 100), with the listed random components; the highest score wins with a
// uniform tie-break; plus a hard-switch rule. Rules for moves / abilities / items / field effects that do
// not exist in Gen 3 (Sucker Punch, terrains, Trick Room, hazards other than Spikes, Z / Mega, Fell Stinger,
// Relic Song, Meteor Beam, Coaching, King's Shield, Final Gambit, Fling, doubles-only rules, ...) are left
// out; every Gen 3 move effect is mapped to the closest listed rule, and effects the doc does not cover get
// the defaults: +6 for a non-damaging move, the "All damaging moves" rules for a damaging one. The doc's
// "useless move" filter is implemented for the intuitive cases (USELESS below).
//
// Damage: the AI "rolls a random damage roll for all of its attacking moves": each AI move gets its own
// roll (0..15 %) from the agent's RNG; the threat checks (player KOs / 2HKOs / 3HKOs the AI) use the player's
// best move at the maximum roll. Damage comes from the fast engine's base-damage formula on the imported
// state (STAB and type applied here), or from Sim_EstimateDamageMons when the state cannot be imported.
// Speed: effective speeds (stages, paralysis, Swift Swim / Chlorophyll, Macho Brace); ties count as the AI
// being faster ("AI sees speed ties as them being faster"). Nothing here touches the engine's RNG.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "global.h"
#include "battle.h"
#include "battle_main.h"
#include "battle_util.h"
#include "pokemon.h"
#include "pokedex.h"
#include "item.h"
#include "sim.h"
#include "sim_agent.h"
#include "sim_globals.h"
#include "util.h"
#include "constants/moves.h"
#include "constants/species.h"
#include "constants/pokemon.h"
#include "constants/abilities.h"
#include "constants/items.h"
#include "constants/hold_effects.h"
#include "constants/battle_move_effects.h"
#include "fast.h"
#include "fast_internal.h"

#define RNB_MAX_ACTIONS 32
#define USELESS (-10)   // the "bad move" filter: the move would fail or do nothing (score <= -5: "ineffective")
#define NEVER   (-20)   // the doc's "never used (-20)"

// A battler or benched party mon as the AI reasons about it.
struct RnbMon
{
    u16 species, hp, maxHP, atk, def, spe, spa, spd;
    u16 moves[4];
    u8 pp[4];
    u16 item;
    u8 ability, level, type1, type2, gender, friendship, hpType, hpPower;
    u32 status1, status2, status3;
    s8 stages[NUM_BATTLE_STATS];
    u8 active;            // 1: in play as `battler`; 0: benched (party slot `slot`), neutral stages, no volatiles
    u8 battler, slot;
    u8 isFirstTurn, protectUses, stockpile, tauntTimer, encoreTimer, disableTimer, truantCounter, flashFire;
    u16 lastMove;
};

struct Rnb
{
    struct SimAgent *ag;
    struct BattleSim *sim;      // the state reasoned about (turn start when available), bound as gSim
    int haveFast;
    fs_state fs;
    u8 aiB, plB, aiSide, plSide, doubles;
    struct RnbMon ai, pl;
    u16 weather;
    int weatherActive;
    // derived once per decision
    u32 aiSpe, plSpe;
    int aiFaster;               // ties count as faster
    int plBest;                 // max-roll damage of the player's best move against the AI mon
    int plCanKO, plCan2HKO, plCan3HKO;
    int plHasPhys, plHasSpec, plHasStatus, plHasSound, plHasDamaging;
    int aiAlive, plAlive;
    int dmg[4], kill[4], mult[4], isHighest[4];
    int anyKill;
    int highBonus;              // +6 (80%) / +8 (20%) for the highest damaging move(s), rolled once per decision
};

// ---------------------------------------------------------------------------------------------------------
// small helpers

static int Pct(struct Rnb *c, int p) { return (int)(Sim_AgentRandom(c->ag) % 100) < p; }
static u8 HoldEffect(u16 item) { return item < ITEMS_COUNT ? ItemId_GetHoldEffect(item) : 0; }
static u8 HoldParam(u16 item) { return item < ITEMS_COUNT ? ItemId_GetHoldEffectParam(item) : 0; }
static int IsType(const struct RnbMon *m, u8 t) { return m->type1 == t || m->type2 == t; }
static int IsPhysicalType(u8 type) { return type < TYPE_MYSTERY; }
static int IsDamagingMove(u16 move) { return move != MOVE_NONE && move < MOVES_COUNT && gBattleMoves[move].power > 0; }
static int HasMove(const struct RnbMon *m, u16 move) { int i; for (i = 0; i < 4; i++) if (m->moves[i] == move) return 1; return 0; }
static int HasEffect(const struct RnbMon *m, u8 eff) { int i; for (i = 0; i < 4; i++) if (m->moves[i] && gBattleMoves[m->moves[i]].effect == eff) return 1; return 0; }

static int IsSoundMove(u16 move)
{
    // the engine's Soundproof table plus Perish Song (a sound move that the doc's Substitute rule cares about)
    switch (move)
    {
    case MOVE_GROWL: case MOVE_ROAR: case MOVE_SING: case MOVE_SUPERSONIC: case MOVE_SCREECH: case MOVE_SNORE:
    case MOVE_UPROAR: case MOVE_METAL_SOUND: case MOVE_GRASS_WHISTLE: case MOVE_HYPER_VOICE: case MOVE_PERISH_SONG:
        return 1;
    }
    return 0;
}

static int IsHighCritEffect(u8 eff)   // Cmd_critcalc's high crit ratio effects
{
    return eff == EFFECT_HIGH_CRITICAL || eff == EFFECT_SKY_ATTACK || eff == EFFECT_BLAZE_KICK || eff == EFFECT_POISON_TAIL;
}

static int IsFlinchEffect(u8 eff)     // "a move that flinches"
{
    return eff == EFFECT_FLINCH_HIT || eff == EFFECT_FLINCH_MINIMIZE_HIT || eff == EFFECT_TWISTER || eff == EFFECT_FAKE_OUT || eff == EFFECT_SNORE;
}

static int HasFlinchMove(const struct RnbMon *m) { int i; for (i = 0; i < 4; i++) if (m->moves[i] && IsFlinchEffect(gBattleMoves[m->moves[i]].effect)) return 1; return 0; }
static int IsSetupHigh(const struct RnbMon *m, int stat) { return m->stages[stat] >= MAX_STAT_STAGE; }
static int AnyStageChanged(const struct RnbMon *m) { int i; for (i = 1; i < NUM_BATTLE_STATS; i++) if (m->stages[i] != DEFAULT_STAT_STAGE) return 1; return 0; }
static int AnyStageUp(const struct RnbMon *m) { int i; for (i = 1; i < NUM_BATTLE_STATS; i++) if (m->stages[i] > DEFAULT_STAT_STAGE) return 1; return 0; }

// ---------------------------------------------------------------------------------------------------------
// loading the mons

static void HiddenPowerFromIVs(const u8 iv[6], u8 *type, u8 *power)
{
    u8 typeBits = (iv[0] & 1) | ((iv[1] & 1) << 1) | ((iv[2] & 1) << 2) | ((iv[3] & 1) << 3) | ((iv[4] & 1) << 4) | ((iv[5] & 1) << 5);
    u8 powerBits = ((iv[0] & 2) >> 1) | (iv[1] & 2) | ((iv[2] & 2) << 1) | ((iv[3] & 2) << 2) | ((iv[4] & 2) << 3) | ((iv[5] & 2) << 4);
    *type = (15 * typeBits) / 63 + 1;
    if (*type >= TYPE_MYSTERY) (*type)++;
    *power = (40 * powerBits) / 63 + 30;
}

static void LoadActive(struct Rnb *c, struct RnbMon *m, u8 battler)
{
    const struct BattlePokemon *b = &gBattleMons[battler];
    const struct DisableStruct *d = &gDisableStructs[battler];
    u8 iv[6] = { b->hpIV, b->attackIV, b->defenseIV, b->speedIV, b->spAttackIV, b->spDefenseIV };
    int i;
    memset(m, 0, sizeof(*m));
    m->active = 1; m->battler = battler; m->slot = gBattlerPartyIndexes[battler];
    m->species = b->species; m->hp = b->hp; m->maxHP = b->maxHP;
    m->atk = b->attack; m->def = b->defense; m->spe = b->speed; m->spa = b->spAttack; m->spd = b->spDefense;
    for (i = 0; i < 4; i++) { m->moves[i] = b->moves[i]; m->pp[i] = b->pp[i]; }
    m->item = b->item; m->ability = b->ability; m->level = b->level; m->type1 = b->type1; m->type2 = b->type2;
    m->friendship = b->friendship;
    m->gender = GetMonGender(&Sim_Party(c->sim, GetBattlerSide(battler))[m->slot]);
    m->status1 = b->status1; m->status2 = b->status2; m->status3 = gStatuses3[battler];
    for (i = 0; i < NUM_BATTLE_STATS; i++) m->stages[i] = b->statStages[i];
    HiddenPowerFromIVs(iv, &m->hpType, &m->hpPower);
    m->isFirstTurn = d->isFirstTurn; m->protectUses = d->protectUses; m->stockpile = d->stockpileCounter;
    m->tauntTimer = d->tauntTimer; m->encoreTimer = d->encoreTimer; m->disableTimer = d->disableTimer; m->truantCounter = d->truantCounter;
    m->flashFire = (gBattleResources->flags.flags[battler] & RESOURCE_FLAG_FLASH_FIRE) != 0;
    m->lastMove = (gLastMoves[battler] == 0xFFFF) ? MOVE_NONE : gLastMoves[battler];
    if (gAbsentBattlerFlags & gBitTable[battler]) m->hp = 0;
}

static void LoadParty(struct Rnb *c, struct RnbMon *m, u8 side, u8 slot)
{
    struct Pokemon *mon = &Sim_Party(c->sim, side)[slot];
    u16 species = GetMonData(mon, MON_DATA_SPECIES_OR_EGG);
    u8 iv[6];
    int i;
    memset(m, 0, sizeof(*m));
    m->slot = slot;
    if (species == SPECIES_NONE || species == SPECIES_EGG || species >= NUM_SPECIES) return;
    m->species = species;
    m->level = GetMonData(mon, MON_DATA_LEVEL);
    m->hp = GetMonData(mon, MON_DATA_HP); m->maxHP = GetMonData(mon, MON_DATA_MAX_HP);
    m->atk = GetMonData(mon, MON_DATA_ATK); m->def = GetMonData(mon, MON_DATA_DEF); m->spe = GetMonData(mon, MON_DATA_SPEED);
    m->spa = GetMonData(mon, MON_DATA_SPATK); m->spd = GetMonData(mon, MON_DATA_SPDEF);
    for (i = 0; i < 4; i++) { m->moves[i] = GetMonData(mon, MON_DATA_MOVE1 + i); m->pp[i] = GetMonData(mon, MON_DATA_PP1 + i); }
    m->item = (gWishFutureKnock.knockedOffMons[side] & gBitTable[slot]) ? ITEM_NONE : GetMonData(mon, MON_DATA_HELD_ITEM);
    {
        u8 an = GetMonData(mon, MON_DATA_ABILITY_NUM);
        m->ability = gSpeciesInfo[species].abilities[an & 1] ? gSpeciesInfo[species].abilities[an & 1] : gSpeciesInfo[species].abilities[0];
    }
    m->type1 = gSpeciesInfo[species].types[0]; m->type2 = gSpeciesInfo[species].types[1];
    m->gender = GetMonGender(mon);
    m->friendship = GetMonData(mon, MON_DATA_FRIENDSHIP);
    m->status1 = GetMonData(mon, MON_DATA_STATUS);
    for (i = 0; i < NUM_BATTLE_STATS; i++) m->stages[i] = DEFAULT_STAT_STAGE;
    iv[0] = GetMonData(mon, MON_DATA_HP_IV); iv[1] = GetMonData(mon, MON_DATA_ATK_IV); iv[2] = GetMonData(mon, MON_DATA_DEF_IV);
    iv[3] = GetMonData(mon, MON_DATA_SPEED_IV); iv[4] = GetMonData(mon, MON_DATA_SPATK_IV); iv[5] = GetMonData(mon, MON_DATA_SPDEF_IV);
    HiddenPowerFromIVs(iv, &m->hpType, &m->hpPower);
    m->isFirstTurn = 2;
}

static int AliveCount(struct Rnb *c, u8 side)
{
    struct Pokemon *party = Sim_Party(c->sim, side);
    int i, n = 0;
    for (i = 0; i < PARTY_SIZE; i++)
    {
        u16 sp = GetMonData(&party[i], MON_DATA_SPECIES_OR_EGG);
        if (sp != SPECIES_NONE && sp != SPECIES_EGG && GetMonData(&party[i], MON_DATA_HP) > 0) n++;
    }
    return n;
}

// ---------------------------------------------------------------------------------------------------------
// speed and damage

// Effective speed the way GetWhoStrikesFirst computes it, minus the Quick Claw roll.
static u32 Speed(const struct Rnb *c, const struct RnbMon *m)
{
    u32 sp = m->spe;
    u8 he = HoldEffect(m->item);
    if (c->weatherActive && (c->weather & B_WEATHER_RAIN) && m->ability == ABILITY_SWIFT_SWIM) sp *= 2;
    if (c->weatherActive && (c->weather & B_WEATHER_SUN) && m->ability == ABILITY_CHLOROPHYLL) sp *= 2;
    sp = sp * gStatStageRatios[m->stages[STAT_SPEED]][0] / gStatStageRatios[m->stages[STAT_SPEED]][1];
    if (he == HOLD_EFFECT_MACHO_BRACE) sp /= 2;
    if (m->status1 & STATUS1_PARALYSIS) sp /= 4;
    return sp;
}

static void FillFsBattler(fs_battler *a, const struct RnbMon *m)
{
    int i;
    memset(a, 0, sizeof(*a));
    a->present = m->hp > 0;
    a->monIdx = m->slot;
    a->species = m->species; a->hp = m->hp; a->maxHP = m->maxHP; a->atk = m->atk; a->def = m->def; a->spe = m->spe; a->spa = m->spa; a->spd = m->spd;
    for (i = 0; i < 4; i++) { a->moves[i] = m->moves[i]; a->pp[i] = m->pp[i]; }
    a->item = m->item; a->ability = m->ability; a->level = m->level; a->type1 = m->type1; a->type2 = m->type2; a->gender = m->gender;
    a->status1 = m->status1 & 0xFFF;
    for (i = 0; i < FS_STAGES; i++) a->stages[i] = m->stages[i];
    if (m->status3 & STATUS3_MUDSPORT) a->vol |= FS_V_MUD_SPORT;
    if (m->status3 & STATUS3_WATERSPORT) a->vol |= FS_V_WATER_SPORT;
    if (m->status2 & STATUS2_FORESIGHT) a->vol |= FS_V_FORESIGHT;
    if (m->flashFire) a->vol |= FS_V_FLASH_FIRE;
    a->hpTypeCache = m->hpType; a->hpPowerCache = m->hpPower;
}

static void FillBattleMon(struct BattlePokemon *b, const struct RnbMon *m)
{
    int i;
    memset(b, 0, sizeof(*b));
    b->species = m->species; b->attack = m->atk; b->defense = m->def; b->speed = m->spe; b->spAttack = m->spa; b->spDefense = m->spd;
    for (i = 0; i < 4; i++) { b->moves[i] = m->moves[i]; b->pp[i] = m->pp[i]; }
    for (i = 0; i < NUM_BATTLE_STATS; i++) b->statStages[i] = m->stages[i];
    b->ability = m->ability; b->type1 = m->type1; b->type2 = m->type2;
    b->hp = m->hp; b->level = m->level; b->friendship = m->friendship; b->maxHP = m->maxHP; b->item = m->item;
    b->status1 = m->status1; b->status2 = m->status2;
}

// Power and type of a variable move, as the fast engine resolves them (Magnitude / Present at their typical value).
static u16 MovePowerType(const struct Rnb *c, const struct RnbMon *atk, const struct RnbMon *def, u16 move, u8 *type)
{
    const struct BattleMove *bm = &gBattleMoves[move];
    u16 power = bm->power;
    *type = bm->type;
    switch (bm->effect)
    {
    case EFFECT_HIDDEN_POWER: *type = atk->hpType; power = atk->hpPower; break;
    case EFFECT_RETURN: power = 10 * atk->friendship / 25; if (power == 0) power = 1; break;
    case EFFECT_FRUSTRATION: power = 10 * (255 - atk->friendship) / 25; if (power == 0) power = 1; break;
    case EFFECT_FLAIL:
    {
        u32 r = atk->maxHP ? atk->hp * 48 / atk->maxHP : 48;
        power = r < 2 ? 200 : r < 5 ? 150 : r < 10 ? 100 : r < 17 ? 80 : r < 33 ? 40 : 20;
        break;
    }
    case EFFECT_ERUPTION: power = atk->maxHP ? bm->power * atk->hp / atk->maxHP : 1; if (power == 0) power = 1; break;
    case EFFECT_LOW_KICK:
    {
        u32 w = GetPokedexHeightWeight(SpeciesToNationalPokedexNum(def->species), 1);
        static const u16 table[] = {100, 20, 250, 40, 500, 60, 1000, 80, 2000, 100, 0xFFFF, 120};
        int i;
        power = 120;
        for (i = 0; i < 12; i += 2) if (w < table[i]) { power = table[i + 1]; break; }
        break;
    }
    case EFFECT_MAGNITUDE: power = 70; break;
    case EFFECT_PRESENT: power = 40; break;
    case EFFECT_SPIT_UP: power = 100 * atk->stockpile; break;
    case EFFECT_WEATHER_BALL:
        if (c->weatherActive)
        {
            power = 100;
            *type = (c->weather & B_WEATHER_RAIN) ? TYPE_WATER : (c->weather & B_WEATHER_SUN) ? TYPE_FIRE : (c->weather & B_WEATHER_SANDSTORM) ? TYPE_ROCK : TYPE_ICE;
        }
        break;
    case EFFECT_FACADE: if (atk->status1 & (STATUS1_POISON | STATUS1_TOXIC_POISON | STATUS1_BURN | STATUS1_PARALYSIS)) power *= 2; break;
    case EFFECT_SMELLINGSALT: if ((def->status1 & STATUS1_PARALYSIS) && !(def->status2 & STATUS2_SUBSTITUTE)) power *= 2; break;
    }
    return power;
}

// Damage of `move` from atk to def at roll r (0 = the maximum roll, 15 = the minimum), before accuracy.
// *multOut: type effectiveness x10, 0 for any immunity (type chart, Levitate, absorbing abilities, Wonder Guard).
static int Damage(struct Rnb *c, const struct RnbMon *atk, const struct RnbMon *def, u16 move, int r, int *multOut)
{
    const struct BattleMove *bm;
    u8 type, eff;
    u16 power;
    int mult, fixed = -1;
    s32 dmg;
    if (multOut) *multOut = 10;
    if (!IsDamagingMove(move) || def->hp == 0) return 0;
    bm = &gBattleMoves[move];
    eff = bm->effect;
    power = MovePowerType(c, atk, def, move, &type);
    mult = fs_type_mult(type, def->type1, def->type2, (def->status2 & STATUS2_FORESIGHT) != 0);
    if (type == TYPE_GROUND && def->ability == ABILITY_LEVITATE) mult = 0;
    if (type == TYPE_ELECTRIC && def->ability == ABILITY_VOLT_ABSORB) mult = 0;
    if (type == TYPE_WATER && def->ability == ABILITY_WATER_ABSORB) mult = 0;
    if (type == TYPE_FIRE && def->ability == ABILITY_FLASH_FIRE) mult = 0;
    if (def->ability == ABILITY_SOUNDPROOF && IsSoundMove(move)) mult = 0;
    if (def->ability == ABILITY_WONDER_GUARD && mult <= 10) mult = 0;
    if (multOut) *multOut = mult;
    if (mult == 0) return 0;
    // fixed-damage and unknowable-damage effects
    switch (eff)
    {
    case EFFECT_LEVEL_DAMAGE: case EFFECT_PSYWAVE: fixed = atk->level; break;
    case EFFECT_DRAGON_RAGE: fixed = 40; break;
    case EFFECT_SONICBOOM: fixed = 20; break;
    case EFFECT_SUPER_FANG: fixed = def->hp / 2; if (fixed == 0) fixed = 1; break;
    case EFFECT_ENDEAVOR: fixed = def->hp > atk->hp ? def->hp - atk->hp : 0; break;
    case EFFECT_OHKO: case EFFECT_COUNTER: case EFFECT_MIRROR_COAT: case EFFECT_BIDE: case EFFECT_BEAT_UP: fixed = 0; break;
    }
    if (fixed >= 0) return fixed;
    if (power == 0) return 0;
    if (c->haveFast)
    {
        fs_state tmp;
        // benched mons are always the AI's party (switch candidates); active ones are already in the imported state
        int as = (atk == &c->pl) ? c->plSide : c->aiSide;
        int ds = (def == &c->pl) ? c->plSide : c->aiSide;
        memcpy(&tmp, &c->fs, sizeof(tmp));
        if (!atk->active) FillFsBattler(&tmp.side[as].act, atk);
        if (!def->active) FillFsBattler(&tmp.side[ds].act, def);
        dmg = fs_base_damage(&tmp, as, ds, move, power, type, 0);
        if (type == atk->type1 || type == atk->type2) dmg = dmg * 15 / 10;
        dmg = dmg * mult / 10;
        if (dmg == 0) dmg = 1;
        if (eff == EFFECT_MULTI_HIT) dmg *= 3;
        if (eff == EFFECT_DOUBLE_HIT || eff == EFFECT_TWINEEDLE) dmg *= 2;
        if (eff == EFFECT_TRIPLE_KICK) dmg *= 6;   // 10 + 20 + 30
    }
    else
    {
        // fallback: the verbatim engine's estimate (average roll x0.92, STAB and type included); undo the average
        struct BattlePokemon a, d;
        int raw = 0;
        FillBattleMon(&a, atk); FillBattleMon(&d, def);
        Sim_EstimateDamageMons(&a, &d, move, atk->active ? atk->battler : c->aiB, def->active ? def->battler : c->plB, &raw);
        dmg = raw * 100 / 92;
        if (dmg == 0) dmg = 1;
    }
    dmg = dmg * (100 - r) / 100;
    return dmg < 1 ? 1 : dmg;
}

// Maximum-roll damage of the best usable move of `atk` against `def` (the player's threat against an AI mon).
static int BestDamage(struct Rnb *c, const struct RnbMon *atk, const struct RnbMon *def)
{
    int i, best = 0;
    if (atk->hp == 0) return 0;
    for (i = 0; i < 4; i++)
    {
        int d;
        if (atk->moves[i] == MOVE_NONE || atk->pp[i] == 0) continue;
        if (gBattleMoves[atk->moves[i]].effect == EFFECT_EXPLOSION) continue;
        d = Damage(c, atk, def, atk->moves[i], 0, NULL);
        if (d > best) best = d;
    }
    return best;
}

// ---------------------------------------------------------------------------------------------------------
// setup

static u8 OpponentOf(u8 battler)
{
    u8 o = battler ^ BIT_SIDE;
    if ((gAbsentBattlerFlags & gBitTable[o]) && (gBattleTypeFlags & BATTLE_TYPE_DOUBLE)) o ^= BIT_FLANK;
    return o;
}

static void Setup(struct Rnb *c, struct SimAgent *ag, struct BattleSim *st, u8 battler)
{
    int i, b;
    memset(c, 0, sizeof(*c));
    c->ag = ag; c->sim = st;
    c->haveFast = fs_import(&c->fs, st) == 0;
    Sim_Bind(st);
    c->doubles = (gBattleTypeFlags & BATTLE_TYPE_DOUBLE) != 0;
    c->aiB = battler; c->plB = OpponentOf(battler);
    c->aiSide = GetBattlerSide(c->aiB); c->plSide = GetBattlerSide(c->plB);
    c->weather = gBattleWeather & B_WEATHER_ANY;
    c->weatherActive = c->weather != 0;
    for (b = 0; b < gBattlersCount; b++)
        if (!(gAbsentBattlerFlags & gBitTable[b]) && gBattleMons[b].hp && (gBattleMons[b].ability == ABILITY_CLOUD_NINE || gBattleMons[b].ability == ABILITY_AIR_LOCK))
            c->weatherActive = 0;
    LoadActive(c, &c->ai, c->aiB);
    LoadActive(c, &c->pl, c->plB);
    c->aiAlive = AliveCount(c, c->aiSide);
    c->plAlive = AliveCount(c, c->plSide);
    c->aiSpe = Speed(c, &c->ai); c->plSpe = Speed(c, &c->pl);
    c->aiFaster = c->aiSpe >= c->plSpe;
    // the player's moves and threat
    for (i = 0; i < 4; i++)
    {
        u16 m = c->pl.moves[i];
        if (m == MOVE_NONE) continue;
        if (IsDamagingMove(m))
        {
            c->plHasDamaging = 1;
            if (IsPhysicalType(gBattleMoves[m].type)) c->plHasPhys = 1; else c->plHasSpec = 1;
        }
        else c->plHasStatus = 1;
        if (IsSoundMove(m)) c->plHasSound = 1;
    }
    c->plBest = BestDamage(c, &c->pl, &c->ai);
    c->plCanKO = c->ai.hp && c->plBest >= c->ai.hp;
    c->plCan2HKO = c->ai.hp && c->plBest * 2 >= c->ai.hp;
    c->plCan3HKO = c->ai.hp && c->plBest * 3 >= c->ai.hp;
    // the AI's damage rolls ("AI will roll a random damage roll for all of its attacking moves")
    {
        int maxDmg = 0;
        for (i = 0; i < 4; i++)
        {
            u16 m = c->ai.moves[i];
            u8 eff = m ? gBattleMoves[m].effect : 0;
            int r = (int)(Sim_AgentRandom(ag) % 16);
            c->dmg[i] = Damage(c, &c->ai, &c->pl, m, r, &c->mult[i]);
            c->kill[i] = c->pl.hp && c->dmg[i] >= c->pl.hp;
            // never considered the "highest damaging move": Explosion, Rollout, trapping moves, Future Sight
            if (m && IsDamagingMove(m) && eff != EFFECT_EXPLOSION && eff != EFFECT_ROLLOUT && eff != EFFECT_TRAP && eff != EFFECT_FUTURE_SIGHT)
            {
                if (c->dmg[i] > maxDmg) maxDmg = c->dmg[i];
            }
            else
                c->kill[i] = c->kill[i] && eff != EFFECT_EXPLOSION && eff != EFFECT_ROLLOUT;   // those two get no kill check at all
            if (c->kill[i]) c->anyKill = 1;
        }
        for (i = 0; i < 4; i++)
        {
            u16 m = c->ai.moves[i];
            u8 eff = m ? gBattleMoves[m].effect : 0;
            int rolled = m && IsDamagingMove(m) && eff != EFFECT_EXPLOSION && eff != EFFECT_ROLLOUT && eff != EFFECT_TRAP && eff != EFFECT_FUTURE_SIGHT;
            // "If multiple moves kill, then they are all considered the highest damaging move"
            c->isHighest[i] = rolled && c->dmg[i] > 0 && (c->dmg[i] == maxDmg || c->kill[i]);
        }
    }
    c->highBonus = Pct(c, 20) ? 8 : 6;
}

// ---------------------------------------------------------------------------------------------------------
// status conditions the AI may try to inflict

static int SideHasSafeguard(u8 side) { return gSideTimers[side].safeguardTimer != 0; }

static int CanSleep(struct Rnb *c, const struct RnbMon *t)
{
    int b;
    if (t->status1 & STATUS1_ANY) return 0;
    if (t->ability == ABILITY_INSOMNIA || t->ability == ABILITY_VITAL_SPIRIT) return 0;
    if (SideHasSafeguard(c->plSide) || (t->status2 & STATUS2_SUBSTITUTE)) return 0;
    for (b = 0; b < gBattlersCount; b++) if (gBattleMons[b].status2 & STATUS2_UPROAR) return 0;
    return 1;
}
static int CanPoison(struct Rnb *c, const struct RnbMon *t)
{
    if (t->status1 & STATUS1_ANY) return 0;
    if (IsType(t, TYPE_POISON) || IsType(t, TYPE_STEEL) || t->ability == ABILITY_IMMUNITY) return 0;
    if (SideHasSafeguard(c->plSide) || (t->status2 & STATUS2_SUBSTITUTE)) return 0;
    return 1;
}
static int CanParalyze(struct Rnb *c, const struct RnbMon *t, u16 move)
{
    if (t->status1 & STATUS1_ANY) return 0;
    if (t->ability == ABILITY_LIMBER) return 0;
    if (move == MOVE_THUNDER_WAVE && fs_type_mult(TYPE_ELECTRIC, t->type1, t->type2, 0) == 0) return 0;
    if (SideHasSafeguard(c->plSide) || (t->status2 & STATUS2_SUBSTITUTE)) return 0;
    return 1;
}
static int CanBurn(struct Rnb *c, const struct RnbMon *t)
{
    if (t->status1 & STATUS1_ANY) return 0;
    if (IsType(t, TYPE_FIRE) || t->ability == ABILITY_WATER_VEIL) return 0;
    if (SideHasSafeguard(c->plSide) || (t->status2 & STATUS2_SUBSTITUTE)) return 0;
    return 1;
}
static int CanConfuse(struct Rnb *c, const struct RnbMon *t)
{
    if (t->status2 & STATUS2_CONFUSION) return 0;
    if (t->ability == ABILITY_OWN_TEMPO) return 0;
    if (SideHasSafeguard(c->plSide) || (t->status2 & STATUS2_SUBSTITUTE)) return 0;
    return 1;
}

// "player mon is incapacitated (frozen with no thawing move, asleep, recharging, loafing around due to Truant)"
static int PlayerIncapacitated(struct Rnb *c)
{
    const struct RnbMon *p = &c->pl;
    if (p->status1 & STATUS1_SLEEP) return 1;
    if ((p->status1 & STATUS1_FREEZE) && !HasEffect(p, EFFECT_THAW_HIT)) return 1;
    if (p->status2 & STATUS2_RECHARGE) return 1;
    if (p->ability == ABILITY_TRUANT && p->truantCounter) return 1;
    return 0;
}

// Protect: "AI is inflicted with any of the following: Poison, Burn, Cursed, Infatuated, Perish Songed, Leech Seeded, Yawned"
static int Afflicted(const struct RnbMon *m)
{
    return (m->status1 & (STATUS1_POISON | STATUS1_TOXIC_POISON | STATUS1_BURN)) || (m->status2 & (STATUS2_CURSED | STATUS2_INFATUATION))
        || (m->status3 & (STATUS3_PERISH_SONG | STATUS3_LEECHSEED | STATUS3_YAWN));
}

// End-of-turn damage the AI mon takes if it just sits there ("AI will not protect if they die to secondary damage afterwards").
static int ResidualDamage(struct Rnb *c)
{
    const struct RnbMon *m = &c->ai;
    int d = 0, mx = m->maxHP;
    if (c->weatherActive && (c->weather & B_WEATHER_SANDSTORM) && !IsType(m, TYPE_ROCK) && !IsType(m, TYPE_GROUND) && !IsType(m, TYPE_STEEL) && m->ability != ABILITY_SAND_VEIL) d += mx / 16;
    if (c->weatherActive && (c->weather & B_WEATHER_HAIL) && !IsType(m, TYPE_ICE)) d += mx / 16;
    if (m->status1 & STATUS1_POISON) d += mx / 8;
    if (m->status1 & STATUS1_TOXIC_POISON) d += (mx / 16) * (((m->status1 & STATUS1_TOXIC_COUNTER) >> 8) + 1);
    if (m->status1 & STATUS1_BURN) d += mx / 8;
    if (m->status3 & STATUS3_LEECHSEED) d += mx / 8;
    if ((m->status2 & STATUS2_NIGHTMARE) && (m->status1 & STATUS1_SLEEP)) d += mx / 4;
    if (m->status2 & STATUS2_CURSED) d += mx / 4;
    if (HoldEffect(m->item) == HOLD_EFFECT_LEFTOVERS) d -= mx / 16;
    return d;
}

// "Should AI Recover function" (EXTRA DETAILS). pct: the move's recovery percentage.
static int ShouldRecover(struct Rnb *c, int pct)
{
    const struct RnbMon *m = &c->ai;
    int heal = (int)m->maxHP * pct / 100;
    if (heal > (int)(m->maxHP - m->hp)) heal = m->maxHP - m->hp;
    if (m->status1 & STATUS1_TOXIC_POISON) return 0;
    if (c->plBest >= heal) return 0;
    if (c->aiFaster)
    {
        if (c->plCanKO) return c->plBest < (int)m->hp + heal;
        if (m->hp * 100 < m->maxHP * 40) return 1;
        if (m->hp * 100 < m->maxHP * 66) return Pct(c, 50);
        return 0;
    }
    if (m->hp * 2 < m->maxHP) return 1;
    if (m->hp * 100 < m->maxHP * 70) return Pct(c, 75);
    return 0;
}

// "Thunder Wave, Stun Spore, Glare, Nuzzle, Zap Cannon": +8 / +7 then -1 (50%), returned relative to the +6 base.
static int ParalyzeBonus(struct Rnb *c)
{
    int good = (c->plSpe > c->aiSpe && c->plSpe / 4 <= c->aiSpe)          // faster now, slower after paralysis
            || HasFlinchMove(&c->ai)
            || (c->pl.status2 & (STATUS2_INFATUATION | STATUS2_CONFUSION));
    int b = good ? 2 : 1;
    if (Pct(c, 50)) b -= 1;
    return b;
}

// "Offensive Setup" bonuses relative to the +6 base.
static int OffensiveSetup(struct Rnb *c)
{
    int b = 0;
    if (PlayerIncapacitated(c)) b += 3;
    if (!c->aiFaster && c->plCan2HKO) b -= 5;
    return b;
}

// "Defensive Setup" bonuses relative to the +6 base.
static int DefensiveSetup(struct Rnb *c, int boostsBoth)
{
    int b = 0;
    if (!c->aiFaster && c->plCan2HKO) b -= 5;
    if (Pct(c, 95))
    {
        if (PlayerIncapacitated(c)) b += 2;
        if (boostsBoth && (c->ai.stages[STAT_DEF] < DEFAULT_STAT_STAGE + 2 || c->ai.stages[STAT_SPDEF] < DEFAULT_STAT_STAGE + 2)) b += 2;
    }
    return b;
}

// Pure stat-lowering status moves: useless when the drop cannot happen or does nothing.
static int StatDownUseless(struct Rnb *c, int stat)
{
    const struct RnbMon *t = &c->pl;
    if (t->status2 & STATUS2_SUBSTITUTE) return 1;
    if (gSideTimers[c->plSide].mistTimer) return 1;
    if (t->ability == ABILITY_CLEAR_BODY || t->ability == ABILITY_WHITE_SMOKE) return 1;
    if (stat == STAT_ATK && t->ability == ABILITY_HYPER_CUTTER) return 1;
    if (stat == STAT_ACC && t->ability == ABILITY_KEEN_EYE) return 1;
    if (t->stages[stat] <= DEFAULT_STAT_STAGE - 2) return 1;
    if (stat == STAT_ATK && !c->plHasPhys) return 1;
    if (stat == STAT_SPATK && !c->plHasSpec) return 1;
    return 0;
}

// ---------------------------------------------------------------------------------------------------------
// the score of one move

static int ScoreMove(struct Rnb *c, int slot)
{
    const struct RnbMon *ai = &c->ai, *pl = &c->pl;
    u16 move = ai->moves[slot];
    const struct BattleMove *bm;
    u8 eff;
    int S = 0, damaging, kill, mult;

    if (move == MOVE_NONE || move >= MOVES_COUNT) return NEVER;
    bm = &gBattleMoves[move];
    eff = bm->effect;
    damaging = IsDamagingMove(move);
    kill = c->kill[slot]; mult = c->mult[slot];

    // Asleep for at least another turn: only Sleep Talk and Snore can do anything.
    if ((ai->status1 & STATUS1_SLEEP) && (ai->status1 & STATUS1_SLEEP) >= 2 && eff != EFFECT_SLEEP_TALK && eff != EFFECT_SNORE) return USELESS;

    if (damaging)
    {
        // "All damaging moves"
        if (mult == 0) return USELESS;                                     // the target is immune
        if (c->isHighest[slot]) S += c->highBonus;                          // +6 (80%) / +8 (20%)
        if (kill)
        {
            // "If AI mon is faster, or the move has priority and AI is slower: +6; if AI mon is slower: +3"
            if (c->aiFaster || bm->priority > 0) S += 6; else S += 3;
        }
        // "If a damaging move has a high crit chance and is Super Effective on the target: +1 (50%)"
        if (IsHighCritEffect(eff) && mult > 10 && Pct(c, 50)) S += 1;
        // "Damaging priority moves: if AI is dead to player mon and slower, all attacking moves with priority get +11"
        if (c->plCanKO && !c->aiFaster && bm->priority > 0) S += 11;
    }
    else
        S += 6;   // "the default score for non-attacking moves is +6"

    switch (eff)
    {
    // ---- damaging moves with their own AI
    case EFFECT_TRAP:              // "Damaging Trapping moves": +6 (80%) / +8 (20%)
        S += Pct(c, 20) ? 8 : 6;
        break;
    case EFFECT_SPEED_DOWN_HIT:    // "Damaging speed reduction moves" (guaranteed effect: Icy Wind, Rock Tomb, Mud Shot)
        if (bm->secondaryEffectChance == 100 && !c->isHighest[slot])
            S += (!c->aiFaster && pl->ability != ABILITY_CLEAR_BODY && pl->ability != ABILITY_WHITE_SMOKE) ? 6 : 5;
        break;
    case EFFECT_FUTURE_SIGHT:      // "Future Sight"
        if (gSideStatuses[c->plSide] & SIDE_STATUS_FUTUREATTACK) return USELESS;
        S += (c->aiFaster && c->plCanKO) ? 8 : 6;
        break;
    case EFFECT_PURSUIT:           // "Pursuit"
        if (kill) S += 10;
        else if (pl->hp * 5 < pl->maxHP) S += 10;
        else if (pl->hp * 5 < pl->maxHP * 2 && Pct(c, 50)) S += 8;
        if (c->aiFaster) S += 3;
        break;
    case EFFECT_ROLLOUT:           // "Rollout: always +7"
        S += 7;
        break;
    case EFFECT_EXPLOSION:         // "Explosion, Self Destruct"
        if (c->aiAlive == 1 && c->plAlive > 1) return NEVER;
        if (ai->hp * 10 < ai->maxHP) S += 10;
        else if (ai->hp * 3 < ai->maxHP) S += Pct(c, 70) ? 8 : 0;
        else if (ai->hp * 3 < ai->maxHP * 2) S += Pct(c, 50) ? 7 : 0;
        else S += Pct(c, 5) ? 7 : 0;
        if (c->aiAlive == 1 && c->plAlive == 1) S -= 1;
        break;
    case EFFECT_FAKE_OUT:          // "Fake Out"
        if (!ai->isFirstTurn) return USELESS;
        if (pl->ability != ABILITY_SHIELD_DUST && pl->ability != ABILITY_INNER_FOCUS) S += 9;
        break;
    case EFFECT_PARALYZE_HIT:      // Zap Cannon (guaranteed) shares the paralysis bonus
        if (bm->secondaryEffectChance == 100 && CanParalyze(c, pl, move)) S += ParalyzeBonus(c);
        break;
    case EFFECT_DREAM_EATER:
        if (!(pl->status1 & STATUS1_SLEEP) || (pl->status2 & STATUS2_SUBSTITUTE)) return USELESS;
        break;
    case EFFECT_SNORE:
        if (!(ai->status1 & STATUS1_SLEEP)) return USELESS;
        break;
    case EFFECT_COUNTER: case EFFECT_MIRROR_COAT:   // "Counter / Mirror Coat": +6 base (not scored as damage: unknown)
    {
        int phys = eff == EFFECT_COUNTER;
        int has = phys ? c->plHasPhys : c->plHasSpec, only = has && !(phys ? c->plHasSpec : c->plHasPhys);
        if (!has) return USELESS;
        S += 6;
        if (c->plCanKO) S -= 20;
        else if (only && Pct(c, 80)) S += 2;
        if (c->aiFaster && Pct(c, 25)) S -= 1;
        if (c->plHasStatus && Pct(c, 25)) S -= 1;
        break;
    }
    case EFFECT_BIDE: case EFFECT_OHKO: case EFFECT_BEAT_UP:
        break;   // damage unknown to the AI: scored as a damaging move that is never the highest

    // ---- status: sleep / poison / paralysis / burn / confusion / others
    case EFFECT_SLEEP: case EFFECT_YAWN:    // "Yawn, Dark Void, and all other non-damaging sleep moves"
        if (!CanSleep(c, pl) || (eff == EFFECT_YAWN && (pl->status3 & STATUS3_YAWN))) return USELESS;
        if (!c->anyKill && Pct(c, 25))
        {
            S += 1;
            if ((HasMove(ai, MOVE_DREAM_EATER) || HasMove(ai, MOVE_NIGHTMARE)) && !HasMove(pl, MOVE_SNORE) && !HasMove(pl, MOVE_SLEEP_TALK)) S += 1;
        }
        break;
    case EFFECT_POISON: case EFFECT_TOXIC:  // "Poisoning Moves": the +2 branch needs Hex / Venoshock / Venom Drench / Merciless (none in Gen 3)
        if (!CanPoison(c, pl)) return USELESS;
        break;
    case EFFECT_PARALYZE:                   // "Thunder Wave, Stun Spore, Glare"
        if (!CanParalyze(c, pl, move)) return USELESS;
        S += ParalyzeBonus(c);
        break;
    case EFFECT_WILL_O_WISP:                // "Will-o-Wisp"
        if (!CanBurn(c, pl)) return USELESS;
        if (Pct(c, 37) && c->plHasPhys) S += 1;
        break;
    case EFFECT_CONFUSE: case EFFECT_SWAGGER: case EFFECT_FLATTER: case EFFECT_TEETER_DANCE:
        if (!CanConfuse(c, pl)) return USELESS;
        break;
    case EFFECT_LEECH_SEED:
        if (IsType(pl, TYPE_GRASS) || (pl->status3 & STATUS3_LEECHSEED) || (pl->status2 & STATUS2_SUBSTITUTE)) return USELESS;
        break;
    case EFFECT_ATTRACT:
        if (pl->ability == ABILITY_OBLIVIOUS || (pl->status2 & STATUS2_INFATUATION) || ai->gender == MON_GENDERLESS || pl->gender == MON_GENDERLESS || ai->gender == pl->gender) return USELESS;
        break;
    case EFFECT_NIGHTMARE:
        if (!(pl->status1 & STATUS1_SLEEP) || (pl->status2 & (STATUS2_NIGHTMARE | STATUS2_SUBSTITUTE))) return USELESS;
        break;

    // ---- setup ("General setup": never when the player can KO the AI)
    case EFFECT_ATTACK_UP: case EFFECT_ATTACK_UP_2:   // "Offensive Setup" (Swords Dance, Howl, Sharpen, Meditate)
        if (IsSetupHigh(ai, STAT_ATK)) return USELESS;
        if (c->plCanKO) return NEVER;
        S += OffensiveSetup(c);
        break;
    case EFFECT_DRAGON_DANCE:
        if (IsSetupHigh(ai, STAT_ATK) && IsSetupHigh(ai, STAT_SPEED)) return USELESS;
        if (c->plCanKO) return NEVER;
        S += OffensiveSetup(c);
        break;
    case EFFECT_DEFENSE_UP: case EFFECT_DEFENSE_UP_2: case EFFECT_DEFENSE_CURL:   // "Defensive Setup" (Harden, Withdraw, Acid Armor, Barrier, Iron Defense)
        if (IsSetupHigh(ai, STAT_DEF)) return USELESS;
        if (c->plCanKO) return NEVER;
        S += DefensiveSetup(c, 0);
        break;
    case EFFECT_SPECIAL_DEFENSE_UP_2:       // Amnesia
        if (IsSetupHigh(ai, STAT_SPDEF)) return USELESS;
        if (c->plCanKO) return NEVER;
        S += DefensiveSetup(c, 0);
        break;
    case EFFECT_COSMIC_POWER:
        if (IsSetupHigh(ai, STAT_DEF) && IsSetupHigh(ai, STAT_SPDEF)) return USELESS;
        if (c->plCanKO) return NEVER;
        S += DefensiveSetup(c, 1);
        break;
    case EFFECT_STOCKPILE:
        if (ai->stockpile >= 3) return USELESS;
        if (c->plCanKO) return NEVER;
        S += DefensiveSetup(c, 0);
        break;
    case EFFECT_BULK_UP:                    // "Coil, Bulk Up": defensive vs a physical-only player, offensive otherwise
        if (IsSetupHigh(ai, STAT_ATK) && IsSetupHigh(ai, STAT_DEF)) return USELESS;
        if (c->plCanKO) return NEVER;
        S += (c->plHasPhys && !c->plHasSpec) ? DefensiveSetup(c, 0) : OffensiveSetup(c);
        break;
    case EFFECT_CALM_MIND:                  // "Calm Mind, Quiver Dance": defensive vs a special-only player, offensive otherwise
        if (IsSetupHigh(ai, STAT_SPATK) && IsSetupHigh(ai, STAT_SPDEF)) return USELESS;
        if (c->plCanKO) return NEVER;
        S += (c->plHasSpec && !c->plHasPhys) ? DefensiveSetup(c, 0) : OffensiveSetup(c);
        break;
    case EFFECT_CURSE:
        if (IsType(ai, TYPE_GHOST))
        {
            if ((pl->status2 & (STATUS2_CURSED | STATUS2_SUBSTITUTE)) || ai->hp * 2 <= ai->maxHP) return USELESS;
            break;
        }
        if (IsSetupHigh(ai, STAT_ATK) && IsSetupHigh(ai, STAT_DEF)) return USELESS;
        if (c->plCanKO) return NEVER;
        S += (c->plHasPhys && !c->plHasSpec) ? DefensiveSetup(c, 0) : OffensiveSetup(c);
        break;
    case EFFECT_SPEED_UP: case EFFECT_SPEED_UP_2:   // "Agility, Rock Polish": +7 when slower, never otherwise
        if (IsSetupHigh(ai, STAT_SPEED)) return USELESS;
        if (c->aiFaster) return NEVER;
        S = 7;
        break;
    case EFFECT_SPECIAL_ATTACK_UP: case EFFECT_SPECIAL_ATTACK_UP_2:   // "Tail Glow, Nasty Plot" (Growth is in the setup list too)
        if (IsSetupHigh(ai, STAT_SPATK)) return USELESS;
        if (c->plCanKO) return NEVER;
        if (PlayerIncapacitated(c)) S += 3;
        else if (!c->plCan3HKO) { S += 1; if (c->aiFaster) S += 1; }
        if (!c->aiFaster && c->plCan2HKO) S -= 5;
        if (ai->stages[STAT_SPATK] >= DEFAULT_STAT_STAGE + 2) S -= 1;
        break;
    case EFFECT_BELLY_DRUM:                 // "Belly Drum" (Sitrus Berry taken into account)
    {
        int hpAfter;
        if (ai->hp <= ai->maxHP / 2 || IsSetupHigh(ai, STAT_ATK)) return USELESS;
        hpAfter = ai->hp - ai->maxHP / 2;
        if (HoldEffect(ai->item) == HOLD_EFFECT_RESTORE_HP && hpAfter * 2 <= ai->maxHP) { hpAfter += HoldParam(ai->item); if (hpAfter > ai->maxHP) hpAfter = ai->maxHP; }
        if (PlayerIncapacitated(c)) S = 9;
        else if (c->plBest < hpAfter) S = 8;
        else S = 4;
        break;
    }
    case EFFECT_FOCUS_ENERGY:               // "Focus Energy"
        if (pl->ability == ABILITY_SHELL_ARMOR || pl->ability == ABILITY_BATTLE_ARMOR || (ai->status2 & STATUS2_FOCUS_ENERGY)) return USELESS;
        if (HoldEffect(ai->item) == HOLD_EFFECT_SCOPE_LENS || HasEffect(ai, EFFECT_HIGH_CRITICAL) || HasEffect(ai, EFFECT_SKY_ATTACK)
         || HasEffect(ai, EFFECT_BLAZE_KICK) || HasEffect(ai, EFFECT_POISON_TAIL)) S += 1;
        break;
    case EFFECT_EVASION_UP: case EFFECT_EVASION_UP_2: case EFFECT_MINIMIZE:
        if (IsSetupHigh(ai, STAT_EVASION)) return USELESS;
        break;
    case EFFECT_ACCURACY_UP: case EFFECT_ACCURACY_UP_2:
        if (IsSetupHigh(ai, STAT_ACC)) return USELESS;
        break;

    // ---- protection
    case EFFECT_PROTECT:                    // "Protect"
        if (ResidualDamage(c) >= (int)ai->hp) return NEVER;
        if (ai->protectUses >= 2) return NEVER;
        if (ai->protectUses == 1 && Pct(c, 50)) return NEVER;
        if (Afflicted(ai)) S -= 2;
        if (Afflicted(pl)) S += 1;
        if (ai->isFirstTurn && !c->doubles) S -= 1;
        break;
    case EFFECT_ENDURE:                     // not in the doc: Protect's consecutive-use rule, useless at 1 HP
        if (ai->hp <= 1) return USELESS;
        if (ai->protectUses >= 2) return NEVER;
        if (ai->protectUses == 1 && Pct(c, 50)) return NEVER;
        break;
    case EFFECT_SUBSTITUTE:                 // "Substitute"
        if ((ai->status2 & STATUS2_SUBSTITUTE) || ai->hp <= ai->maxHP / 4) return USELESS;
        if (ai->hp * 2 <= ai->maxHP) return NEVER;
        if (pl->status1 & STATUS1_SLEEP) S += 2;
        if ((pl->status3 & STATUS3_LEECHSEED) && c->aiFaster) S += 2;
        if (Pct(c, 50)) S -= 1;
        if (c->plHasSound) S -= 8;
        break;

    // ---- field
    case EFFECT_LIGHT_SCREEN:               // "Light Screen / Reflect" (no Light Clay in Gen 3)
        if (gSideStatuses[c->aiSide] & SIDE_STATUS_LIGHTSCREEN) return USELESS;
        if (c->plHasSpec && Pct(c, 50)) S += 1;
        break;
    case EFFECT_REFLECT:
        if (gSideStatuses[c->aiSide] & SIDE_STATUS_REFLECT) return USELESS;
        if (c->plHasPhys && Pct(c, 50)) S += 1;
        break;
    case EFFECT_SAFEGUARD:
        if (gSideStatuses[c->aiSide] & SIDE_STATUS_SAFEGUARD) return USELESS;
        break;
    case EFFECT_MIST:
        if (gSideStatuses[c->aiSide] & SIDE_STATUS_MIST) return USELESS;
        break;
    case EFFECT_SPIKES:                     // "Spikes"
    {
        int layers = gSideTimers[c->plSide].spikesAmount;
        if (layers >= 3) return USELESS;
        S = ai->isFirstTurn ? (Pct(c, 75) ? 9 : 8) : (Pct(c, 75) ? 7 : 6);
        if (layers > 0) S -= 1;
        break;
    }
    case EFFECT_RAIN_DANCE: if (gBattleWeather & B_WEATHER_RAIN) return USELESS; break;
    case EFFECT_SUNNY_DAY:  if (gBattleWeather & B_WEATHER_SUN) return USELESS; break;
    case EFFECT_SANDSTORM:  if (gBattleWeather & B_WEATHER_SANDSTORM) return USELESS; break;
    case EFFECT_HAIL:       if (gBattleWeather & B_WEATHER_HAIL) return USELESS; break;
    case EFFECT_HAZE:
        if (!AnyStageChanged(ai) && !AnyStageChanged(pl)) return USELESS;
        break;
    case EFFECT_MUD_SPORT:   if (ai->status3 & STATUS3_MUDSPORT) return USELESS; break;
    case EFFECT_WATER_SPORT: if (ai->status3 & STATUS3_WATERSPORT) return USELESS; break;

    // ---- switching / self-sacrifice
    case EFFECT_BATON_PASS:                 // "Baton Pass"
    {
        u8 slots[PARTY_SIZE];
        if (Sim_LegalSwitches(c->sim, c->aiB, slots, PARTY_SIZE) == 0) return NEVER;
        S = ((ai->status2 & STATUS2_SUBSTITUTE) || AnyStageUp(ai)) ? 14 : 0;
        break;
    }
    case EFFECT_MEMENTO:                    // "Memento"
        if (c->aiAlive == 1) return NEVER;
        if ((pl->status2 & STATUS2_SUBSTITUTE) || (pl->stages[STAT_ATK] == MIN_STAT_STAGE && pl->stages[STAT_SPATK] == MIN_STAT_STAGE)) return USELESS;
        if (ai->hp * 10 < ai->maxHP) S = 16;
        else if (ai->hp * 3 < ai->maxHP) S = Pct(c, 70) ? 14 : 6;
        else if (ai->hp * 3 < ai->maxHP * 2) S = Pct(c, 50) ? 13 : 6;
        else S = Pct(c, 5) ? 13 : 6;
        break;
    case EFFECT_ROAR:
        if (c->plAlive <= 1 || pl->ability == ABILITY_SUCTION_CUPS || (pl->status3 & STATUS3_ROOTED)) return USELESS;
        break;
    case EFFECT_TELEPORT: case EFFECT_SPLASH:
        return USELESS;

    // ---- recovery
    case EFFECT_RESTORE_HP: case EFFECT_SOFTBOILED: case EFFECT_WISH: case EFFECT_SWALLOW:   // "Recovery Moves"
        if (eff == EFFECT_SWALLOW && ai->stockpile == 0) return USELESS;
        if (eff == EFFECT_WISH && gWishFutureKnock.wishCounter[c->aiB]) return USELESS;
        if (ai->hp >= ai->maxHP) return NEVER;
        S = ShouldRecover(c, eff == EFFECT_SWALLOW ? (ai->stockpile == 1 ? 25 : ai->stockpile == 2 ? 50 : 100) : 50) ? 7 : 5;
        if (ai->hp * 100 >= ai->maxHP * 85) S -= 6;
        break;
    case EFFECT_MORNING_SUN: case EFFECT_SYNTHESIS: case EFFECT_MOONLIGHT:   // "Sun-based recovery moves"
        if (ai->hp >= ai->maxHP) return NEVER;
        if (c->weatherActive && (c->weather & B_WEATHER_SUN) && ShouldRecover(c, 67)) S = 7;
        else S = ShouldRecover(c, 50) ? 7 : 5;
        if (ai->hp * 100 >= ai->maxHP * 85) S -= 6;
        break;
    case EFFECT_REST:                       // "Rest"
        if (ai->hp >= ai->maxHP || (ai->status1 & STATUS1_SLEEP) || ai->ability == ABILITY_INSOMNIA || ai->ability == ABILITY_VITAL_SPIRIT) return NEVER;
        if (ShouldRecover(c, 100))
        {
            u8 he = HoldEffect(ai->item);
            S = (he == HOLD_EFFECT_CURE_SLP || he == HOLD_EFFECT_CURE_STATUS || HasMove(ai, MOVE_SLEEP_TALK) || HasMove(ai, MOVE_SNORE)
              || ai->ability == ABILITY_SHED_SKIN || ai->ability == ABILITY_EARLY_BIRD) ? 8 : 7;
        }
        else S = 5;
        if (ai->hp * 100 >= ai->maxHP * 85) S -= 6;
        break;
    case EFFECT_INGRAIN:
        if (ai->status3 & STATUS3_ROOTED) return USELESS;
        break;
    case EFFECT_HEAL_BELL:
    {
        struct Pokemon *party = Sim_Party(c->sim, c->aiSide);
        int i, any = 0;
        for (i = 0; i < PARTY_SIZE; i++)
            if (GetMonData(&party[i], MON_DATA_SPECIES_OR_EGG) != SPECIES_NONE && GetMonData(&party[i], MON_DATA_HP) && GetMonData(&party[i], MON_DATA_STATUS)) any = 1;
        if (!any && !(ai->status1 & STATUS1_ANY)) return USELESS;
        break;
    }
    case EFFECT_REFRESH:
        if (!(ai->status1 & (STATUS1_POISON | STATUS1_TOXIC_POISON | STATUS1_BURN | STATUS1_PARALYSIS))) return USELESS;
        break;

    // ---- disruption
    case EFFECT_TAUNT:                      // "Taunt": no Trick Room / Defog / Aurora Veil in Gen 3, so +5
        if (pl->tauntTimer) return USELESS;
        S = 5;
        break;
    case EFFECT_ENCORE:                     // "Encore"
        if (pl->encoreTimer || pl->lastMove == MOVE_NONE || pl->isFirstTurn) return USELESS;
        if (c->aiFaster) { if (!IsDamagingMove(pl->lastMove)) S += 1; }   // "encouraged": mostly non-damaging moves
        else if (Pct(c, 50)) S -= 1;
        break;
    case EFFECT_DISABLE:
        if (pl->disableTimer || pl->lastMove == MOVE_NONE) return USELESS;
        break;
    case EFFECT_TORMENT:
        if (pl->status2 & STATUS2_TORMENT) return USELESS;
        break;
    case EFFECT_SPITE: case EFFECT_MIRROR_MOVE: case EFFECT_MIMIC: case EFFECT_SKETCH:
        if (pl->lastMove == MOVE_NONE) return USELESS;
        break;
    case EFFECT_IMPRISON:                   // "Imprison"
    {
        int i, common = 0;
        if (ai->status3 & STATUS3_IMPRISONED_OTHERS) return USELESS;
        for (i = 0; i < 4; i++) if (ai->moves[i] && HasMove(pl, ai->moves[i])) common = 1;
        if (!common) return NEVER;
        S = 9;
        break;
    }
    case EFFECT_TRICK:                      // "Trick, Switcheroo": none of the listed items exist in Gen 3, so +5
        if (pl->ability == ABILITY_STICKY_HOLD || (ai->item == ITEM_NONE && pl->item == ITEM_NONE) || (pl->status2 & STATUS2_SUBSTITUTE)) return USELESS;
        S = 5;
        break;
    case EFFECT_DESTINY_BOND:               // "Destiny Bond"
        if (c->aiFaster) { if (c->plCanKO && Pct(c, 81)) S += 1; }
        else if (Pct(c, 50)) S -= 1;
        break;
    case EFFECT_PERISH_SONG:
        if ((ai->status3 & STATUS3_PERISH_SONG) || pl->ability == ABILITY_SOUNDPROOF) return USELESS;
        break;
    case EFFECT_MEAN_LOOK:
        if ((pl->status2 & STATUS2_ESCAPE_PREVENTION) || (pl->status2 & STATUS2_SUBSTITUTE)) return USELESS;
        break;
    case EFFECT_LOCK_ON:
        if ((pl->status2 & STATUS2_SUBSTITUTE) || ((ai->status3 & STATUS3_ALWAYS_HITS) && gDisableStructs[c->aiB].battlerWithSureHit == c->plB)) return USELESS;
        break;
    case EFFECT_FORESIGHT:
        if (pl->status2 & STATUS2_FORESIGHT) return USELESS;
        break;
    case EFFECT_PSYCH_UP:
        if (!AnyStageUp(pl)) return USELESS;
        break;
    case EFFECT_TRANSFORM:
        if ((ai->status2 & STATUS2_TRANSFORMED) || (pl->status2 & STATUS2_TRANSFORMED)) return USELESS;
        break;
    case EFFECT_RECYCLE:
        if (ai->item != ITEM_NONE || gBattleStruct->usedHeldItems[c->aiB] == ITEM_NONE) return USELESS;
        break;
    case EFFECT_CHARGE:
    {
        int i, electric = 0;
        for (i = 0; i < 4; i++) if (IsDamagingMove(ai->moves[i]) && gBattleMoves[ai->moves[i]].type == TYPE_ELECTRIC) electric = 1;
        if ((ai->status3 & STATUS3_CHARGED_UP) || !electric) return USELESS;
        break;
    }
    case EFFECT_SLEEP_TALK:
        if (!(ai->status1 & STATUS1_SLEEP)) return USELESS;
        break;
    case EFFECT_SPIT_UP:
        if (ai->stockpile == 0) return USELESS;
        break;
    case EFFECT_HELPING_HAND: case EFFECT_FOLLOW_ME:   // doubles only: +6 there
        if (!c->doubles) return USELESS;
        break;
    case EFFECT_ROLE_PLAY:                  // "Role Play": needs a partner with Huge Power / Pure Power in the doc; never in singles
        if (!c->doubles) return NEVER;
        break;

    // ---- stat-lowering status moves
    case EFFECT_ATTACK_DOWN: case EFFECT_ATTACK_DOWN_2:   if (StatDownUseless(c, STAT_ATK)) return USELESS; break;
    case EFFECT_DEFENSE_DOWN: case EFFECT_DEFENSE_DOWN_2: if (StatDownUseless(c, STAT_DEF)) return USELESS; break;
    case EFFECT_SPEED_DOWN: case EFFECT_SPEED_DOWN_2:     if (StatDownUseless(c, STAT_SPEED)) return USELESS; break;
    case EFFECT_SPECIAL_ATTACK_DOWN: case EFFECT_SPECIAL_ATTACK_DOWN_2: if (StatDownUseless(c, STAT_SPATK)) return USELESS; break;
    case EFFECT_SPECIAL_DEFENSE_DOWN: case EFFECT_SPECIAL_DEFENSE_DOWN_2: if (StatDownUseless(c, STAT_SPDEF)) return USELESS; break;
    case EFFECT_ACCURACY_DOWN: case EFFECT_ACCURACY_DOWN_2: if (StatDownUseless(c, STAT_ACC)) return USELESS; break;
    case EFFECT_EVASION_DOWN: case EFFECT_EVASION_DOWN_2:   if (StatDownUseless(c, STAT_EVASION)) return USELESS; break;
    case EFFECT_TICKLE:
        if (StatDownUseless(c, STAT_ATK) && StatDownUseless(c, STAT_DEF)) return USELESS;
        break;
    }
    return S;
}

// ---------------------------------------------------------------------------------------------------------
// switching

// Post-KO switch-in choice (the doc does not describe it): the fastest party mon that is not OHKO'd by the
// player's best move, else the one that takes the least damage; ties go to the one hitting the player hardest.
// `allowed` (optional) restricts the candidates (the hard-switch rule's second condition).
static u8 PickSwitchIn(struct Rnb *c, const u8 *slots, int n, const u8 *allowed)
{
    int i, bestI = 0;
    long long bestKey = -1;
    for (i = 0; i < n; i++)
    {
        struct RnbMon m;
        int taken, given, ohko, tier;
        long long key;
        if (allowed && !allowed[i]) continue;
        LoadParty(c, &m, c->aiSide, slots[i]);
        if (m.species == SPECIES_NONE || m.hp == 0) continue;
        taken = BestDamage(c, &c->pl, &m);
        given = BestDamage(c, &m, &c->pl);
        ohko = taken >= (int)m.hp;
        if (!ohko)
        {
            tier = 2;
            key = (long long)Speed(c, &m) * 100000LL + (given > 99999 ? 99999 : given);
        }
        else
        {
            tier = 1;
            key = (99999LL - (long long)taken * 1000 / (m.hp ? m.hp : 1)) * 100000LL + (given > 99999 ? 99999 : given);
            if (key < 0) key = 0;
        }
        key += (long long)tier * 100000000000LL;
        if (key > bestKey) { bestKey = key; bestI = i; }
    }
    return n ? slots[bestI] : 0;
}

// The "Switch AI" second condition with its documented bug: once a faster mon was seen, every later mon is
// treated as faster too. Fills allowed[] and returns the number of passing candidates.
static int SwitchCandidates(struct Rnb *c, const u8 *slots, int n, u8 *allowed)
{
    int i, sawFaster = 0, count = 0;
    for (i = 0; i < n; i++)
    {
        struct RnbMon m;
        int taken;
        allowed[i] = 0;
        LoadParty(c, &m, c->aiSide, slots[i]);
        if (m.species == SPECIES_NONE || m.hp == 0) continue;
        if (Speed(c, &m) >= c->plSpe) sawFaster = 1;
        taken = BestDamage(c, &c->pl, &m);
        if (sawFaster) allowed[i] = taken < (int)m.hp;          // faster and not OHKO'd
        else allowed[i] = taken * 2 < (int)m.hp;                 // slower and not 2HKO'd
        count += allowed[i];
    }
    return count;
}

// ---------------------------------------------------------------------------------------------------------
// the agent

void Sim_DecideRnB(struct SimAgent *ag, struct BattleSim *sim, const struct BattleSim *turnStart, u8 battler, u8 kind, struct SimAction *out)
{
    static _Thread_local struct Rnb c;
    struct BattleSim *st = (kind == SIM_REQ_ACTION && turnStart) ? (struct BattleSim *)turnStart : sim;
    struct SimAction acts[RNB_MAX_ACTIONS];
    int score[RNB_MAX_ACTIONS];
    int n, i, best = -1000, nBest = 0, nMoves = 0, nSwitch = 0;
    u8 switchSlots[PARTY_SIZE];

    ag->policyValid = 0;
    ag->decisions++;
    memset(out, 0, sizeof(*out));
    Setup(&c, ag, st, battler);

    if (kind == SIM_REQ_SWITCH)
    {
        int ns = Sim_LegalSwitches(sim, battler, switchSlots, PARTY_SIZE);
        Sim_Bind(st);
        out->type = B_ACTION_SWITCH;
        out->partySlot = ns ? PickSwitchIn(&c, switchSlots, ns, NULL) : 0;
        return;
    }

    n = Sim_LegalActions(st, battler, acts, RNB_MAX_ACTIONS);
    if (n == 0) { out->type = B_ACTION_USE_MOVE; out->moveSlot = 0; out->target = 0xFF; return; }
    // locked into a move (Thrash, Rollout, recharge, ...): the engine ignores the choice
    if (gBattleMons[battler].status2 & (STATUS2_MULTIPLETURNS | STATUS2_RECHARGE)) { *out = acts[0]; return; }

    for (i = 0; i < n; i++)
    {
        if (acts[i].type == B_ACTION_USE_MOVE)
        {
            score[i] = ScoreMove(&c, acts[i].moveSlot);
            // doubles: never aim a selectable-target move at our own side unless the move is meant for it
            if (c.doubles && acts[i].target != 0xFF && GetBattlerSide(acts[i].target) == c.aiSide
             && !(gBattleMoves[gBattleMons[battler].moves[acts[i].moveSlot]].target & (MOVE_TARGET_USER | MOVE_TARGET_USER_OR_SELECTED)))
                score[i] -= 30;
            nMoves++;
            if (score[i] > best) { best = score[i]; nBest = 1; }
            else if (score[i] == best) nBest++;
        }
        else
        {
            score[i] = -1000;
            if (nSwitch < PARTY_SIZE) switchSlots[nSwitch++] = acts[i].partySlot;
        }
    }
    if (nMoves == 0)
    {
        out->type = B_ACTION_SWITCH;
        out->partySlot = PickSwitchIn(&c, switchSlots, nSwitch, NULL);
        return;
    }

    // "Switch AI" (singles): only ineffective moves (score <= -5), a viable party mon, AI mon at 50% or more, then 50%.
    if (!c.doubles && best <= -5 && nSwitch > 0 && c.ai.hp * 2 >= c.ai.maxHP)
    {
        u8 allowed[PARTY_SIZE];
        if (SwitchCandidates(&c, switchSlots, nSwitch, allowed) > 0 && Pct(&c, 50))
        {
            out->type = B_ACTION_SWITCH;
            out->partySlot = PickSwitchIn(&c, switchSlots, nSwitch, allowed);
            return;
        }
    }

    // the highest score, uniform tie-break
    {
        int pick = (int)(Sim_AgentRandom(ag) % (u32)nBest), k = 0;
        for (i = 0; i < n; i++)
        {
            if (acts[i].type != B_ACTION_USE_MOVE || score[i] != best) continue;
            if (k++ == pick) { *out = acts[i]; break; }
        }
    }
    if (getenv("SIM_RNB_TRACE"))
    {
        fprintf(stderr, "rnb turn %u battler %d (species %u vs %u, spe %u/%u%s, player's best %d vs hp %d%s):", st->turnCount, battler,
                c.ai.species, c.pl.species, c.aiSpe, c.plSpe, c.aiFaster ? " faster" : " slower", c.plBest, c.ai.hp, c.haveFast ? "" : ", fallback damage");
        for (i = 0; i < n; i++)
            if (acts[i].type == B_ACTION_USE_MOVE)
                fprintf(stderr, " %s=%d(dmg %d%s)", fs_move_name(gBattleMons[battler].moves[acts[i].moveSlot]), score[i], c.dmg[acts[i].moveSlot], c.kill[acts[i].moveSlot] ? " KO" : "");
        fprintf(stderr, " -> %s\n", out->type == B_ACTION_SWITCH ? "switch" : fs_move_name(gBattleMons[battler].moves[out->moveSlot]));
    }
}
