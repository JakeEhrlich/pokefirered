// Fast engine core: import, legality, damage arithmetic, turn order, the turn loop, switching, end of turn.
// Every formula follows the verbatim engine (src/pokemon.c CalculateBaseDamage, src/battle_script_commands.c
// accuracy/crit/typecalc/adjustnormaldamage, src/battle_main.c GetWhoStrikesFirst, src/battle_util.c
// AtkCanceller_UnableToUseMove and the end-turn effects). Randomness: fs_chance / fs_roll only.
#include <stdio.h>
#include <stdlib.h>
#include "fast.h"
#include "fast_internal.h"
// verbatim engine data tables and the import source
#include "global.h"
#include "battle.h"
#include "pokemon.h"
#include "item.h"
#include "data.h"
#include "sim.h"
#include "sim_globals.h"
#include "constants/species.h"
#include "constants/moves.h"
#include "constants/abilities.h"
#include "constants/items.h"
#include "constants/hold_effects.h"
#include "constants/battle_move_effects.h"

const u8 fs_stat_ratio[13][2] = {{10,40},{10,35},{10,30},{10,25},{10,20},{10,15},{10,10},{15,10},{20,10},{25,10},{30,10},{35,10},{40,10}};
static const u16 sAccNum[13] = {33, 36, 43, 50, 60, 75, 1, 133, 166, 2, 233, 133 * 2, 3};   // sAccuracyStageRatios (+5 = 266/100, +6 = 3/1)
static const u16 sAccDen[13] = {100, 100, 100, 100, 100, 100, 1, 100, 100, 1, 100, 100, 1};
static const u16 sCritChance[5] = {16, 8, 4, 3, 2};

const char *fs_move_name(u16 move) { extern const char *sim_move_name(int); return sim_move_name(move); }

// ---------------------------------------------------------------------------------------------------------
// data access

const struct BattleMove *fs_move(u16 move) { return &gBattleMoves[move < MOVES_COUNT ? move : 0]; }
u8 fs_hold_effect(u16 item) { return item < ITEMS_COUNT ? ItemId_GetHoldEffect(item) : 0; }
u8 fs_hold_param(u16 item) { return item < ITEMS_COUNT ? ItemId_GetHoldEffectParam(item) : 0; }

// Applies the type chart to `dmg` the way Cmd_typecalc does: each matching table entry in order, truncating, with a
// minimum of 1 after a non-zero factor. Returns the damage; *mult gets the combined multiplier x10 (0 = immune).
s32 fs_apply_type(s32 dmg, u8 moveType, u8 t1, u8 t2, int foresight, int *mult)
{
    int i, m = 10;
    for (i = 0; gTypeEffectiveness[i] != TYPE_ENDTABLE; i += 3)
    {
        if (gTypeEffectiveness[i] == TYPE_FORESIGHT) { if (foresight) break; continue; }
        if (gTypeEffectiveness[i] != moveType) continue;
        if (gTypeEffectiveness[i + 1] == t1) { u8 f = gTypeEffectiveness[i + 2]; m = m * f / 10; dmg = dmg * f / 10; if (dmg == 0 && f != 0) dmg = 1; }
        if (gTypeEffectiveness[i + 1] == t2 && t2 != t1) { u8 f = gTypeEffectiveness[i + 2]; m = m * f / 10; dmg = dmg * f / 10; if (dmg == 0 && f != 0) dmg = 1; }
    }
    if (mult) *mult = m;
    return dmg;
}

// type effectiveness x10 of moveType against a defender (Foresight handled by the caller), Levitate not included
int fs_type_mult(u8 moveType, u8 t1, u8 t2, int foresight)
{
    int mult = 10, i;
    for (i = 0; gTypeEffectiveness[i] != TYPE_ENDTABLE; i += 3)
    {
        if (gTypeEffectiveness[i] == TYPE_FORESIGHT) { if (foresight) break; continue; }
        if (gTypeEffectiveness[i] != moveType) continue;
        if (gTypeEffectiveness[i + 1] == t1) mult = mult * gTypeEffectiveness[i + 2] / 10;
        if (gTypeEffectiveness[i + 1] == t2 && t2 != t1) mult = mult * gTypeEffectiveness[i + 2] / 10;
    }
    return mult;
}

// ---------------------------------------------------------------------------------------------------------
// import from the verbatim engine

static void ImportMon(fs_mon *m, struct Pokemon *mon)
{
    int i;
    u16 species = GetMonData(mon, MON_DATA_SPECIES_OR_EGG);
    memset(m, 0, sizeof(*m));
    if (species == SPECIES_NONE || species == SPECIES_EGG || species >= NUM_SPECIES) return;
    m->species = species;
    m->level = GetMonData(mon, MON_DATA_LEVEL);
    m->hp = GetMonData(mon, MON_DATA_HP); m->maxHP = GetMonData(mon, MON_DATA_MAX_HP);
    m->atk = GetMonData(mon, MON_DATA_ATK); m->def = GetMonData(mon, MON_DATA_DEF); m->spe = GetMonData(mon, MON_DATA_SPEED);
    m->spa = GetMonData(mon, MON_DATA_SPATK); m->spd = GetMonData(mon, MON_DATA_SPDEF);
    for (i = 0; i < 4; i++)
    {
        m->moves[i] = GetMonData(mon, MON_DATA_MOVE1 + i);
        m->pp[i] = GetMonData(mon, MON_DATA_PP1 + i);
        m->maxPP[i] = m->moves[i] ? CalculatePPWithBonus(m->moves[i], GetMonData(mon, MON_DATA_PP_BONUSES), i) : 0;
    }
    m->item = GetMonData(mon, MON_DATA_HELD_ITEM);
    {
        u8 an = GetMonData(mon, MON_DATA_ABILITY_NUM);
        m->ability = gSpeciesInfo[species].abilities[an & 1] ? gSpeciesInfo[species].abilities[an & 1] : gSpeciesInfo[species].abilities[0];
    }
    m->type1 = gSpeciesInfo[species].types[0]; m->type2 = gSpeciesInfo[species].types[1];
    m->gender = GetMonGender(mon);
    m->status1 = GetMonData(mon, MON_DATA_STATUS) & 0xFFF;
    m->friendship = GetMonData(mon, MON_DATA_FRIENDSHIP);
    {
        // Hidden Power (the game: type from the IV low bits, power from bit 1)
        u8 iv[6] = { GetMonData(mon, MON_DATA_HP_IV), GetMonData(mon, MON_DATA_ATK_IV), GetMonData(mon, MON_DATA_DEF_IV),
                     GetMonData(mon, MON_DATA_SPEED_IV), GetMonData(mon, MON_DATA_SPATK_IV), GetMonData(mon, MON_DATA_SPDEF_IV) };
        u8 typeBits = (iv[0] & 1) | ((iv[1] & 1) << 1) | ((iv[2] & 1) << 2) | ((iv[3] & 1) << 3) | ((iv[4] & 1) << 4) | ((iv[5] & 1) << 5);
        u8 powerBits = ((iv[0] & 2) >> 1) | (iv[1] & 2) | ((iv[2] & 2) << 1) | ((iv[3] & 2) << 2) | ((iv[4] & 2) << 3) | ((iv[5] & 2) << 4);
        m->hpType = (15 * typeBits) / 63 + 1;
        if (m->hpType >= TYPE_MYSTERY) m->hpType++;
        m->hpPower = (40 * powerBits) / 63 + 30;
    }
}

int fs_import(fs_state *out, const struct BattleSim *simc)
{
    struct BattleSim *sim = (struct BattleSim *)simc;
    int s, i;
    memset(out, 0, sizeof(*out));
    Sim_Bind(sim);
    if (gBattleTypeFlags & BATTLE_TYPE_DOUBLE) { out->unsupported |= FS_UNSUP_DOUBLES; return -1; }
    if (sim->requestKind == SIM_REQ_NONE && !sim->finished) { out->unsupported |= FS_UNSUP_STATE; return -1; }
    // battler 0 is not asked while it recharges (the game picks USE_MOVE itself), so the turn's first request
    // can be battler 1's; that is still a turn start
    if (sim->requestKind == SIM_REQ_ACTION && sim->requestBattler != 0 && !(gBattleMons[0].status2 & STATUS2_RECHARGE)) { out->unsupported |= FS_UNSUP_STATE; return -1; }
    out->maxTurns = sim->maxTurns;
    out->turn = sim->turnCount;
    {
        u16 w = gBattleWeather;
        out->weather = (w & B_WEATHER_RAIN) ? FS_WEATHER_RAIN : (w & B_WEATHER_SUN) ? FS_WEATHER_SUN : (w & B_WEATHER_SANDSTORM) ? FS_WEATHER_SAND : (w & B_WEATHER_HAIL) ? FS_WEATHER_HAIL : 0;
        out->weatherTurns = (w & (B_WEATHER_RAIN_PERMANENT | B_WEATHER_SUN_PERMANENT | B_WEATHER_SANDSTORM_PERMANENT)) ? 0xFF : gWishFutureKnock.weatherDuration;
        if (out->weather && out->weatherTurns == 0) out->weatherTurns = 0xFF;
    }
    for (s = 0; s < 2; s++)
    {
        fs_side *sd = &out->side[s];
        struct Pokemon *party = Sim_Party(sim, s);
        struct BattlePokemon *bm = &gBattleMons[s];
        struct DisableStruct *d = &gDisableStructs[s];
        fs_battler *a = &sd->act;
        for (i = 0; i < FS_PARTY; i++) ImportMon(&sd->party[i], &party[i]);
        sd->reflect = gSideTimers[s].reflectTimer; sd->lightscreen = gSideTimers[s].lightscreenTimer;
        sd->mist = gSideTimers[s].mistTimer; sd->safeguard = gSideTimers[s].safeguardTimer; sd->spikes = gSideTimers[s].spikesAmount;
        sd->wishTurns = gWishFutureKnock.wishCounter[s]; sd->wishMon = gWishFutureKnock.wishMonId[s];
        sd->knockedOff = gWishFutureKnock.knockedOffMons[s];
        sd->futureSightTurns = gWishFutureKnock.futureSightCounter[s]; sd->futureSightDmg = gWishFutureKnock.futureSightDmg[s];
        sd->futureSightMove = gWishFutureKnock.futureSightMove[s]; sd->futureSightFromSide = gWishFutureKnock.futureSightAttacker[s] & 1;
        memset(a, 0, sizeof(*a));
        a->monIdx = gBattlerPartyIndexes[s];
        a->present = !(gAbsentBattlerFlags & (1 << s)) && bm->hp > 0;
        a->species = bm->species; a->hp = bm->hp; a->maxHP = bm->maxHP; a->atk = bm->attack; a->def = bm->defense; a->spe = bm->speed;
        a->spa = bm->spAttack; a->spd = bm->spDefense;
        for (i = 0; i < 4; i++) { a->moves[i] = bm->moves[i]; a->pp[i] = bm->pp[i]; }
        a->item = bm->item; a->ability = bm->ability; a->level = bm->level; a->type1 = bm->type1; a->type2 = bm->type2;
        a->gender = sd->party[a->monIdx].gender;
        a->status1 = bm->status1 & 0xFFF;
        for (i = 0; i < FS_STAGES; i++) a->stages[i] = bm->statStages[i];
        {
            u32 s2 = bm->status2, s3 = gStatuses3[s];
            if (s2 & STATUS2_CONFUSION) { a->vol |= FS_V_CONFUSED; a->confusionTurns = s2 & STATUS2_CONFUSION; }
            if (s2 & STATUS2_FLINCHED) a->vol |= FS_V_FLINCH;
            if (s2 & STATUS2_FOCUS_ENERGY) a->vol |= FS_V_FOCUS_ENERGY;
            if (s2 & STATUS2_SUBSTITUTE) { a->vol |= FS_V_SUBSTITUTE; a->substituteHP = d->substituteHP; }
            if (s2 & STATUS2_RECHARGE) { a->vol |= FS_V_RECHARGE; a->rechargeTimer = d->rechargeTimer; }
            if (s2 & STATUS2_DESTINY_BOND) a->vol |= FS_V_DESTINY_BOND;
            if (s2 & STATUS2_ESCAPE_PREVENTION) a->vol |= FS_V_ESCAPE_PREV;
            if (s2 & STATUS2_NIGHTMARE) a->vol |= FS_V_NIGHTMARE;
            if (s2 & STATUS2_CURSED) a->vol |= FS_V_CURSED;
            if (s2 & STATUS2_FORESIGHT) a->vol |= FS_V_FORESIGHT;
            if (s2 & STATUS2_DEFENSE_CURL) a->vol |= FS_V_DEFENSE_CURL;
            if (s2 & STATUS2_TORMENT) a->vol |= FS_V_TORMENT;
            if (s2 & STATUS2_INFATUATION) a->vol |= FS_V_INFATUATED;
            if (s2 & STATUS2_WRAPPED)
            {
                a->wrapTurns = (s2 & STATUS2_WRAPPED) >> 13;
                a->wrapMove = gBattleStruct->wrappedMove[s * 2] | (gBattleStruct->wrappedMove[s * 2 + 1] << 8);
            }
            if (s2 & (STATUS2_UPROAR | STATUS2_BIDE | STATUS2_LOCK_CONFUSE | STATUS2_MULTIPLETURNS | STATUS2_TRANSFORMED | STATUS2_RAGE))
                out->unsupported |= FS_UNSUP_VOLATILE;
            if (s3 & STATUS3_LEECHSEED) a->vol |= FS_V_LEECH_SEED;
            if (s3 & STATUS3_INTIMIDATE_POKES) a->vol |= FS_V_INTIMIDATE_PENDING;
            if (s3 & STATUS3_ROOTED) a->vol |= FS_V_ROOTED;
            if (s3 & STATUS3_CHARGED_UP) a->vol |= FS_V_CHARGED;
            if (s3 & STATUS3_MINIMIZED) a->vol |= FS_V_MINIMIZED;
            if (s3 & STATUS3_MUDSPORT) a->vol |= FS_V_MUD_SPORT;
            if (s3 & STATUS3_WATERSPORT) a->vol |= FS_V_WATER_SPORT;
            if (s3 & STATUS3_PERISH_SONG) a->perishTimer = d->perishSongTimer + 1;   // stored as remaining+1 so 0 = inactive
            if (s3 & STATUS3_YAWN) a->yawnTimer = (s3 & STATUS3_YAWN) >> 11;
            if (s3 & (STATUS3_ALWAYS_HITS | STATUS3_ON_AIR | STATUS3_UNDERGROUND | STATUS3_UNDERWATER | STATUS3_IMPRISONED_OTHERS | STATUS3_GRUDGE | STATUS3_TRACE))
                out->unsupported |= FS_UNSUP_VOLATILE;
            if (gBattleResources->flags.flags[s] & RESOURCE_FLAG_FLASH_FIRE) a->vol |= FS_V_FLASH_FIRE;
        }
        a->disableTimer = d->disableTimer; a->encoreTimer = d->encoreTimer; a->tauntTimer = d->tauntTimer;
        a->stockpile = d->stockpileCounter; a->protectUses = d->protectUses; a->truantCounter = d->truantCounter; a->isFirstTurn = d->isFirstTurn;
        a->furyCutter = d->furyCutterCounter; a->rolloutTimer = d->rolloutTimer; a->chargeTimer = d->chargeTimer;
        a->encoredPos = d->encoredMovePos;
        for (i = 0; i < 4; i++) if (d->disabledMove && a->moves[i] == d->disabledMove) a->disabledPos = i;
        if (d->rolloutTimer || d->furyCutterCounter || d->stockpileCounter || d->mimickedMoves) out->unsupported |= FS_UNSUP_VOLATILE;
        a->hpTypeCache = sd->party[a->monIdx].hpType;
        a->lastMove = (gLastMoves[s] == 0xFFFF) ? 0 : gLastMoves[s];
        a->choicedMove = (gBattleStruct->choicedMove[s] == 0xFFFF) ? 0 : gBattleStruct->choicedMove[s];
        a->lastLandedMove = gLastLandedMoves[s] == 0xFFFF ? 0 : gLastLandedMoves[s];
        a->lastHitByType = gLastHitByType[s];
    }
    if (sim->finished)
    {
        out->request = FS_REQ_DONE;
        out->outcome = gBattleOutcome == B_OUTCOME_WON ? FS_OUTCOME_P0_WON : gBattleOutcome == B_OUTCOME_LOST ? FS_OUTCOME_P1_WON : FS_OUTCOME_DRAW;
    }
    else if (sim->requestKind == SIM_REQ_SWITCH)
    {
        out->request = FS_REQ_SWITCH;
        out->switchMask = 0;
        for (s = 0; s < 2; s++) if (!out->side[s].act.present) out->switchMask |= 1 << s;
        if (!(out->switchMask & (1 << (sim->requestBattler & 1))))
        {
            // the requesting battler is still in: a Baton Pass switch
            out->switchMask |= 1 << (sim->requestBattler & 1);
            out->batonMask |= 1 << (sim->requestBattler & 1);
        }
        if (gCurrentTurnActionNumber >= gBattlersCount)
            out->phase = 2;   // the end-of-turn pass: the turn is over
        else
        {
            // mid-turn: the faint happened during action gCurrentTurnActionNumber; later actions still run
            int k;
            out->phase = 1;
            out->orderN = 0;
            for (k = gCurrentTurnActionNumber + 1; k < gBattlersCount && k < 2; k++)
            {
                u8 b = gBattlerByTurnOrder[k] & 1, act = gActionsByTurnOrder[k];
                fs_action *pa = &out->pending[b];
                if (act == B_ACTION_USE_MOVE) { pa->type = FS_ACT_MOVE; pa->slot = gBattleStruct->chosenMovePositions[gBattlerByTurnOrder[k]]; out->side[b].act.chosenMove = gChosenMoveByBattler[gBattlerByTurnOrder[k]]; }
                else if (act == B_ACTION_SWITCH) { pa->type = FS_ACT_SWITCH; pa->slot = gBattleStruct->monToSwitchIntoId[gBattlerByTurnOrder[k]]; }
                else continue;
                out->order[out->orderN++] = b;
            }
            out->orderPos = 0;
        }
    }
    else
        out->request = FS_REQ_TURN;
    out->unsupported |= fs_unsupported(out);
    return out->unsupported ? -1 : 0;
}

u32 fs_unsupported(const fs_state *s)
{
    u32 u = s->unsupported & ~FS_UNSUP_MOVE_EFFECT & ~FS_UNSUP_ABILITY & ~FS_UNSUP_ITEM;
    int sd, i, k;
    for (sd = 0; sd < 2; sd++)
        for (i = 0; i < FS_PARTY; i++)
        {
            const fs_mon *m = &s->side[sd].party[i];
            if (!m->species) continue;
            for (k = 0; k < 4; k++) if (m->moves[k] && !fs_effect_supported(fs_move(m->moves[k])->effect)) u |= FS_UNSUP_MOVE_EFFECT;
            if (!fs_ability_supported(m->ability)) u |= FS_UNSUP_ABILITY;
            if (!fs_item_supported(m->item)) u |= FS_UNSUP_ITEM;
        }
    return u;
}

void fs_seed(fs_state *s, u32 seed) { s->rng = seed ? seed : 1; }

// ---------------------------------------------------------------------------------------------------------
// legality (mirrors Sim_LegalActions / Sim_LegalSwitches for singles)

static int MoveUsable(const fs_state *s, int side, int slot)
{
    const fs_battler *a = &s->side[side].act;
    u16 move = a->moves[slot];
    if (move == 0 || a->pp[slot] == 0) return 0;
    if (a->disableTimer && slot == a->disabledPos) return 0;
    if (a->tauntTimer && fs_move(move)->power == 0) return 0;
    if ((a->vol & FS_V_TORMENT) && move == a->lastMove) return 0;
    if (a->encoreTimer && slot != a->encoredPos) return 0;
    if (a->choicedMove && fs_hold_effect(a->item) == HOLD_EFFECT_CHOICE_BAND && move != a->choicedMove) return 0;
    return 1;
}

int fs_can_switch(const fs_state *s, int side)
{
    const fs_battler *a = &s->side[side].act, *o = &s->side[side ^ 1].act;
    if (!a->present) return 1;
    if (a->vol & (FS_V_ESCAPE_PREV | FS_V_ROOTED)) return 0;
    if (a->wrapTurns) return 0;
    if (o->present && o->ability == ABILITY_SHADOW_TAG) return 0;
    if (o->present && o->ability == ABILITY_ARENA_TRAP && !(a->type1 == TYPE_FLYING || a->type2 == TYPE_FLYING || a->ability == ABILITY_LEVITATE)) return 0;
    if (o->present && o->ability == ABILITY_MAGNET_PULL && (a->type1 == TYPE_STEEL || a->type2 == TYPE_STEEL)) return 0;
    return 1;
}

int fs_legal_actions(const fs_state *s, int side, fs_action *out)
{
    int n = 0, i, any = 0;
    const fs_side *sd = &s->side[side];
    if (s->request == FS_REQ_DONE) return 0;
    if (s->request == FS_REQ_TURN)
    {
        // recharging: the game does not ask at all (USE_MOVE, then "must recharge"); one placeholder move action
        if (sd->act.vol & FS_V_RECHARGE) { out[0].type = FS_ACT_MOVE; out[0].slot = 0; return 1; }
        for (i = 0; i < 4; i++) if (MoveUsable(s, side, i)) { out[n].type = FS_ACT_MOVE; out[n].slot = i; n++; any = 1; }
        if (!any) { out[n].type = FS_ACT_MOVE; out[n].slot = 4; n++; }   // Struggle
        if (!fs_can_switch(s, side)) return n;
    }
    else if (!(s->switchMask & (1 << side)))
        return 0;
    for (i = 0; i < FS_PARTY; i++)
        if (sd->party[i].species && sd->party[i].hp > 0 && i != sd->act.monIdx) { out[n].type = FS_ACT_SWITCH; out[n].slot = i; n++; }
    return n;
}

// ---------------------------------------------------------------------------------------------------------
// stats and damage

u32 fs_stat_mod(u32 stat, s8 stage) { return stat * fs_stat_ratio[stage][0] / fs_stat_ratio[stage][1]; }

u32 fs_effective_speed(fs_state *s, int side)
{
    const fs_battler *a = &s->side[side].act;
    u32 sp = a->spe;
    u8 he = fs_hold_effect(a->item);
    if (s->weather == FS_WEATHER_RAIN && a->ability == ABILITY_SWIFT_SWIM) sp *= 2;
    if (s->weather == FS_WEATHER_SUN && a->ability == ABILITY_CHLOROPHYLL) sp *= 2;
    sp = sp * fs_stat_ratio[a->stages[STAT_SPEED]][0] / fs_stat_ratio[a->stages[STAT_SPEED]][1];
    if (he == HOLD_EFFECT_MACHO_BRACE) sp /= 2;
    if (a->status1 & FS_S1_PAR) sp /= 4;
    return sp;
}

// CalculateBaseDamage (singles, no badge boosts), crit ignores stage penalties the same way; returns damage + 2 before STAB/type
s32 fs_base_damage(fs_state *s, int atkSide, int defSide, u16 move, u16 power, u8 type, int crit)
{
    fs_battler *at = &s->side[atkSide].act, *df = &s->side[defSide].act;
    fs_side *dside = &s->side[defSide];
    u32 attack = at->atk, defense = df->def, spAttack = at->spa, spDefense = df->spd;
    u8 ahe = fs_hold_effect(at->item), aparam = fs_hold_param(at->item), dhe = fs_hold_effect(df->item);
    s32 damage = 0, helper;
    const struct BattleMove *bm = fs_move(move);
    u16 movePower = power;
    if (at->ability == ABILITY_HUGE_POWER || at->ability == ABILITY_PURE_POWER) attack *= 2;
    // type-boost items
    {
        static const u8 boost[][2] = {{HOLD_EFFECT_BUG_POWER, TYPE_BUG}, {HOLD_EFFECT_STEEL_POWER, TYPE_STEEL}, {HOLD_EFFECT_GROUND_POWER, TYPE_GROUND},
            {HOLD_EFFECT_ROCK_POWER, TYPE_ROCK}, {HOLD_EFFECT_GRASS_POWER, TYPE_GRASS}, {HOLD_EFFECT_DARK_POWER, TYPE_DARK}, {HOLD_EFFECT_FIGHTING_POWER, TYPE_FIGHTING},
            {HOLD_EFFECT_ELECTRIC_POWER, TYPE_ELECTRIC}, {HOLD_EFFECT_WATER_POWER, TYPE_WATER}, {HOLD_EFFECT_FLYING_POWER, TYPE_FLYING}, {HOLD_EFFECT_POISON_POWER, TYPE_POISON},
            {HOLD_EFFECT_ICE_POWER, TYPE_ICE}, {HOLD_EFFECT_GHOST_POWER, TYPE_GHOST}, {HOLD_EFFECT_PSYCHIC_POWER, TYPE_PSYCHIC}, {HOLD_EFFECT_FIRE_POWER, TYPE_FIRE},
            {HOLD_EFFECT_DRAGON_POWER, TYPE_DRAGON}, {HOLD_EFFECT_NORMAL_POWER, TYPE_NORMAL}};
        int i;
        for (i = 0; i < (int)(sizeof(boost) / sizeof(boost[0])); i++)
            if (ahe == boost[i][0] && type == boost[i][1])
            {
                if (type < TYPE_MYSTERY) attack = attack * (aparam + 100) / 100; else spAttack = spAttack * (aparam + 100) / 100;
                break;
            }
    }
    if (ahe == HOLD_EFFECT_CHOICE_BAND) attack = 150 * attack / 100;
    if (ahe == HOLD_EFFECT_SOUL_DEW && (at->species == SPECIES_LATIAS || at->species == SPECIES_LATIOS)) spAttack = 150 * spAttack / 100;
    if (dhe == HOLD_EFFECT_SOUL_DEW && (df->species == SPECIES_LATIAS || df->species == SPECIES_LATIOS)) spDefense = 150 * spDefense / 100;
    if (ahe == HOLD_EFFECT_DEEP_SEA_TOOTH && at->species == SPECIES_CLAMPERL) spAttack *= 2;
    if (dhe == HOLD_EFFECT_DEEP_SEA_SCALE && df->species == SPECIES_CLAMPERL) spDefense *= 2;
    if (ahe == HOLD_EFFECT_LIGHT_BALL && at->species == SPECIES_PIKACHU) spAttack *= 2;
    if (dhe == HOLD_EFFECT_METAL_POWDER && df->species == SPECIES_DITTO) defense *= 2;
    if (ahe == HOLD_EFFECT_THICK_CLUB && (at->species == SPECIES_CUBONE || at->species == SPECIES_MAROWAK)) attack *= 2;
    if (df->ability == ABILITY_THICK_FAT && (type == TYPE_FIRE || type == TYPE_ICE)) spAttack /= 2;
    if (at->ability == ABILITY_HUSTLE) attack = 150 * attack / 100;
    if (at->ability == ABILITY_PLUS && df->ability == ABILITY_MINUS) spAttack = 150 * spAttack / 100;
    if (at->ability == ABILITY_MINUS && df->ability == ABILITY_PLUS) spAttack = 150 * spAttack / 100;
    if (at->ability == ABILITY_GUTS && at->status1) attack = 150 * attack / 100;
    if (df->ability == ABILITY_MARVEL_SCALE && df->status1) defense = 150 * defense / 100;
    if (type == TYPE_ELECTRIC && ((at->vol | df->vol) & FS_V_MUD_SPORT)) movePower /= 2;
    if (type == TYPE_FIRE && ((at->vol | df->vol) & FS_V_WATER_SPORT)) movePower /= 2;
    if (type == TYPE_GRASS && at->ability == ABILITY_OVERGROW && at->hp <= at->maxHP / 3) movePower = 150 * movePower / 100;
    if (type == TYPE_FIRE && at->ability == ABILITY_BLAZE && at->hp <= at->maxHP / 3) movePower = 150 * movePower / 100;
    if (type == TYPE_WATER && at->ability == ABILITY_TORRENT && at->hp <= at->maxHP / 3) movePower = 150 * movePower / 100;
    if (type == TYPE_BUG && at->ability == ABILITY_SWARM && at->hp <= at->maxHP / 3) movePower = 150 * movePower / 100;
    if (bm->effect == EFFECT_EXPLOSION) defense /= 2;
    if (type < TYPE_MYSTERY)
    {
        if (crit && at->stages[STAT_ATK] <= 6) damage = attack; else damage = fs_stat_mod(attack, at->stages[STAT_ATK]);
        damage = damage * movePower;
        damage *= (2 * at->level / 5 + 2);
        if (crit && df->stages[STAT_DEF] >= 6) helper = defense; else helper = fs_stat_mod(defense, df->stages[STAT_DEF]);
        damage = damage / helper;
        damage /= 50;
        if ((at->status1 & FS_S1_BRN) && at->ability != ABILITY_GUTS) damage /= 2;
        if (dside->reflect && !crit) damage /= 2;
        if (damage == 0) damage = 1;
    }
    if (type == TYPE_MYSTERY) damage = 0;
    if (type > TYPE_MYSTERY)
    {
        if (crit && at->stages[STAT_SPATK] <= 6) damage = spAttack; else damage = fs_stat_mod(spAttack, at->stages[STAT_SPATK]);
        damage = damage * movePower;
        damage *= (2 * at->level / 5 + 2);
        if (crit && df->stages[STAT_SPDEF] >= 6) helper = spDefense; else helper = fs_stat_mod(spDefense, df->stages[STAT_SPDEF]);
        damage = damage / helper;
        damage /= 50;
        if (dside->lightscreen && !crit) damage /= 2;
        if (fs_weather_active(s))
        {
            if (s->weather == FS_WEATHER_RAIN) { if (type == TYPE_FIRE) damage /= 2; else if (type == TYPE_WATER) damage = 15 * damage / 10; }
            if ((s->weather == FS_WEATHER_RAIN || s->weather == FS_WEATHER_SAND || s->weather == FS_WEATHER_HAIL) && move == MOVE_SOLAR_BEAM) damage /= 2;
            if (s->weather == FS_WEATHER_SUN) { if (type == TYPE_FIRE) damage = 15 * damage / 10; else if (type == TYPE_WATER) damage /= 2; }
        }
        if ((at->vol & FS_V_FLASH_FIRE) && type == TYPE_FIRE) damage = 15 * damage / 10;
    }
    return damage + 2;
}

int fs_weather_active(const fs_state *s)
{
    // Cloud Nine / Air Lock negate weather; both are unsupported abilities for now, so weather is always active
    return s->weather != 0;
}

// accuracy check: 1 = hit
int fs_accuracy_check(fs_state *s, int atkSide, int defSide, u16 move, u8 type)
{
    fs_battler *at = &s->side[atkSide].act, *df = &s->side[defSide].act;
    const struct BattleMove *bm = fs_move(move);
    int buff;
    u32 calc;
    u8 moveAcc = bm->accuracy;
    u8 dhe = fs_hold_effect(df->item);
    if (moveAcc == 0) return 1;
    if (fs_weather_active(s) && s->weather == FS_WEATHER_RAIN && bm->effect == EFFECT_THUNDER) return 1;   // Thunder never misses in rain
    if (df->vol & FS_V_FORESIGHT) buff = at->stages[STAT_ACC];
    else buff = at->stages[STAT_ACC] + 6 - df->stages[STAT_EVASION];
    if (buff < 0) buff = 0;
    if (buff > 12) buff = 12;
    if (fs_weather_active(s) && s->weather == FS_WEATHER_SUN && bm->effect == EFFECT_THUNDER) moveAcc = 50;
    calc = sAccNum[buff] * moveAcc / sAccDen[buff];
    if (at->ability == ABILITY_COMPOUND_EYES) calc = calc * 130 / 100;
    if (fs_weather_active(s) && df->ability == ABILITY_SAND_VEIL && s->weather == FS_WEATHER_SAND) calc = calc * 80 / 100;
    if (at->ability == ABILITY_HUSTLE && type < TYPE_MYSTERY) calc = calc * 80 / 100;
    if (dhe == HOLD_EFFECT_EVASION_UP) calc = calc * (100 - fs_hold_param(df->item)) / 100;
    return (fs_roll(s, 100) + 1) <= calc;
}

int fs_crit_check(fs_state *s, int atkSide, int defSide, u16 move)
{
    fs_battler *at = &s->side[atkSide].act, *df = &s->side[defSide].act;
    const struct BattleMove *bm = fs_move(move);
    u8 he = fs_hold_effect(at->item);
    u32 c = 2 * ((at->vol & FS_V_FOCUS_ENERGY) != 0)
          + (bm->effect == EFFECT_HIGH_CRITICAL) + (bm->effect == EFFECT_SKY_ATTACK) + (bm->effect == EFFECT_BLAZE_KICK) + (bm->effect == EFFECT_POISON_TAIL)
          + (he == HOLD_EFFECT_SCOPE_LENS)
          + 2 * (he == HOLD_EFFECT_LUCKY_PUNCH && at->species == SPECIES_CHANSEY)
          + 2 * (he == HOLD_EFFECT_STICK && at->species == SPECIES_FARFETCHD);
    if (c > 4) c = 4;
    if (df->ability == ABILITY_BATTLE_ARMOR || df->ability == ABILITY_SHELL_ARMOR) return 0;
    return fs_roll(s, sCritChance[c]) == 0;
}

// the random 85..100% multiplier
s32 fs_random_roll(fs_state *s, s32 dmg)
{
    u32 r = fs_roll(s, 16);
    dmg = dmg * (100 - (s32)r) / 100;
    return dmg;
}

// ---------------------------------------------------------------------------------------------------------
// party <-> active sync and switching

void fs_sync_to_party(fs_state *s, int side)
{
    fs_side *sd = &s->side[side];
    fs_battler *a = &sd->act;
    fs_mon *m = &sd->party[a->monIdx];
    int i;
    if (!m->species || !a->present) return;
    m->hp = a->hp; m->status1 = a->status1 & ~FS_S1_TOXCTR;   // the party keeps the toxic flag only; the counter restarts on switch-in
    for (i = 0; i < 4; i++) if (!(a->vol & FS_V_TRANSFORMED)) m->pp[i] = a->pp[i];
    m->item = a->item;
}

static void ResetVolatile(fs_battler *a)
{
    int i;
    for (i = 0; i < FS_STAGES; i++) a->stages[i] = 6;
    a->vol = 0;
    a->confusionTurns = a->uproarTurns = a->bideTurns = a->lockTurns = a->wrapTurns = 0;
    a->disableTimer = a->encoreTimer = a->tauntTimer = a->perishTimer = a->rolloutTimer = a->chargeTimer = a->furyCutter = a->stockpile = 0;
    a->substituteHP = a->protectUses = a->truantCounter = a->rechargeTimer = a->yawnTimer = 0;
    a->disabledPos = a->encoredPos = 0;
    a->lockedMove = a->lastMove = a->lastLandedMove = a->lastHitByType = a->chosenMove = a->bideDmg = a->wrapMove = 0;
    a->choicedMove = 0;
}

// Brings party slot `idx` in on `side` (the previous active, if any, is synced out). Switch-in effects follow.
void fs_switch_in(fs_state *s, int side, int idx)
{
    fs_side *sd = &s->side[side];
    fs_battler *a = &sd->act;
    fs_mon *m;
    int i;
    if (a->present)
    {
        if (a->ability == ABILITY_NATURAL_CURE) a->status1 = 0;
        fs_sync_to_party(s, side);
        s->side[side ^ 1].act.wrapTurns = 0;   // the wrapper leaving frees its target
        s->side[side ^ 1].act.vol &= ~(FS_V_ESCAPE_PREV | FS_V_INFATUATED);   // Mean Look / Attract by the one leaving end too
    }
    m = &sd->party[idx];
    ResetVolatile(a);
    a->monIdx = idx;
    a->present = 1;
    a->species = m->species; a->hp = m->hp; a->maxHP = m->maxHP; a->atk = m->atk; a->def = m->def; a->spe = m->spe; a->spa = m->spa; a->spd = m->spd;
    for (i = 0; i < 4; i++) { a->moves[i] = m->moves[i]; a->pp[i] = m->pp[i]; }
    a->item = (sd->knockedOff & (1 << idx)) ? 0 : m->item; a->ability = m->ability; a->level = m->level; a->type1 = m->type1; a->type2 = m->type2; a->gender = m->gender;
    a->status1 = m->status1;
    a->isFirstTurn = 2;
    a->hpTypeCache = m->hpType;
    // spikes
    if (sd->spikes && !(a->type1 == TYPE_FLYING || a->type2 == TYPE_FLYING) && a->ability != ABILITY_LEVITATE)
    {
        s32 dmg = a->maxHP / ((5 - sd->spikes) * 2);   // 1/8, 1/6, 1/4 as the game computes it
        if (dmg == 0) dmg = 1;
        if (dmg >= a->hp) dmg = a->hp;
        a->hp -= dmg;
    }
    if (a->hp == 0) { fs_faint(s, side); return; }
    fs_switch_in_abilities(s, side);
}

// Baton Pass: switch keeping stat stages and the passable volatile state
void fs_baton_pass(fs_state *s, int side, int idx)
{
    fs_battler *a = &s->side[side].act;
    s8 stages[FS_STAGES]; u32 vol; u8 subHP, confusionTurns, perish;
    memcpy(stages, a->stages, sizeof(stages)); vol = a->vol; subHP = a->substituteHP; confusionTurns = a->confusionTurns; perish = a->perishTimer;
    fs_switch_in(s, side, idx);
    if (!a->present) return;
    memcpy(a->stages, stages, sizeof(stages));
    a->vol |= vol & (FS_V_CONFUSED | FS_V_FOCUS_ENERGY | FS_V_SUBSTITUTE | FS_V_LEECH_SEED | FS_V_ROOTED | FS_V_CURSED | FS_V_ESCAPE_PREV | FS_V_MUD_SPORT | FS_V_WATER_SPORT);
    a->substituteHP = subHP; a->confusionTurns = confusionTurns; a->perishTimer = perish;
}

void fs_faint(fs_state *s, int side)
{
    fs_battler *a = &s->side[side].act;
    a->hp = 0;
    s->side[side ^ 1].act.wrapTurns = 0;   // FaintClearSetData frees the fainted mon's wrap target
    s->side[side ^ 1].act.vol &= ~(FS_V_ESCAPE_PREV | FS_V_INFATUATED);
    a->status1 = 0;
    fs_sync_to_party(s, side);
    a->present = 0;
    a->vol = 0;
}

int fs_alive_count(const fs_state *s, int side)
{
    int i, n = 0;
    for (i = 0; i < FS_PARTY; i++) if (s->side[side].party[i].species && s->side[side].party[i].hp > 0) n++;
    return n;
}

// ---------------------------------------------------------------------------------------------------------
// the turn

static int CheckOutcome(fs_state *s)
{
    int a0 = fs_alive_count(s, 0), a1 = fs_alive_count(s, 1);
    if (a0 == 0 && a1 == 0) { s->outcome = FS_OUTCOME_DRAW; s->request = FS_REQ_DONE; return 1; }
    if (a1 == 0) { s->outcome = FS_OUTCOME_P0_WON; s->request = FS_REQ_DONE; return 1; }
    if (a0 == 0) { s->outcome = FS_OUTCOME_P1_WON; s->request = FS_REQ_DONE; return 1; }
    return 0;
}

// Decides what comes next. phase 1: the action phase just ended (replacements come before the end-of-turn
// effects, as in the game); phase 2: the end-of-turn effects ran (replacements for their faints, then a new turn).
static void FinishTurn(fs_state *s)
{
    fs_end_turn(s);
    fs_sync_to_party(s, 0); fs_sync_to_party(s, 1);
    s->turn++;
    s->phase = 2;
    if (s->maxTurns && s->turn >= s->maxTurns && !CheckOutcome(s)) { s->outcome = FS_OUTCOME_DRAW; s->request = FS_REQ_DONE; }
}

static void ClearTurnFlags(fs_state *s)
{
    int side;
    for (side = 0; side < 2; side++)
    {
        fs_battler *a = &s->side[side].act;
        a->vol &= ~(FS_V_PROTECTED | FS_V_ENDURED | FS_V_MOVED_THIS_TURN | FS_V_FLINCH);
        a->chosenMove = 0;
        if (!a->bideTurns) a->bideDmg = 0;   // damage taken this turn (Counter / Mirror Coat / Focus Punch / Revenge) starts fresh
    }
}

// Runs the rest of the turn from the current phase: remaining actions (a KO requests a replacement at once),
// then the end-of-turn effects, then the faint pass for those, then the next turn start.
static void Resume(fs_state *s)
{
    int side;
    if (s->phase == 1)
    {
        while (s->orderPos < s->orderN)
        {
            side = s->order[s->orderPos++];
            if (s->pending[side].type == FS_ACT_NONE)
                continue;   // cancelled: the mon that chose it was replaced
            if (s->pending[side].type == FS_ACT_SWITCH)
                fs_switch_in(s, side, s->pending[side].slot);   // chosen legally at the turn start; a trapper arriving first does not stop it
            else
            {
                fs_battler *a = &s->side[side].act;
                if (a->present && a->hp) fs_use_move(s, side, s->pending[side].slot);
            }
            fs_sync_to_party(s, 0); fs_sync_to_party(s, 1);
            if (CheckOutcome(s)) return;
            {
                int mask = s->batonMask;
                for (side = 0; side < 2; side++) if (!s->side[side].act.present) mask |= 1 << side;
                if (mask) { s->request = FS_REQ_SWITCH; s->switchMask = mask; return; }
            }
        }
        FinishTurn(s);
        if (s->request == FS_REQ_DONE) return;
    }
    // phase 2
    if (CheckOutcome(s)) return;
    {
        int mask = 0;
        for (side = 0; side < 2; side++) if (!s->side[side].act.present) mask |= 1 << side;
        if (mask) { s->request = FS_REQ_SWITCH; s->switchMask = mask; return; }
    }
    s->phase = 0;
    s->request = FS_REQ_TURN;
    s->switchMask = 0;
}

int fs_step(fs_state *s, fs_action a0, fs_action a1)
{
    fs_action acts[2] = {a0, a1};
    int side, n = 0;
    if (s->request == FS_REQ_DONE) return FS_REQ_DONE;
    if (s->request == FS_REQ_SWITCH)
    {
        // one replacement per step, player side first (the verbatim engine asks one side at a time)
        for (side = 0; side < 2; side++)
            if (s->switchMask & (1 << side))
            {
                if (acts[side].type == FS_ACT_SWITCH)
                {
                    if (s->batonMask & (1 << side)) fs_baton_pass(s, side, acts[side].slot);
                    else fs_switch_in(s, side, acts[side].slot);
                }
                if (s->batonMask & (1 << side))
                {
                    // Baton Pass: only the passer's own action is done (switchineffects); the rest of the turn goes on
                    s->batonMask &= ~(1 << side);
                }
                else if (s->phase == 1)
                {
                    // a fainted mon's replacement cancels every remaining action of the turn in singles
                    // (BattleScript_HandleFaintedMon: cancelallactions)
                    s->orderPos = s->orderN;
                }
                break;
            }
        fs_sync_to_party(s, 0); fs_sync_to_party(s, 1);
        {
            int mask = s->batonMask;
            for (side = 0; side < 2; side++) if (!s->side[side].act.present) mask |= 1 << side;
            if (mask && !CheckOutcome(s)) { s->request = FS_REQ_SWITCH; s->switchMask = mask; return s->request; }
        }
        fs_fire_pending_intimidate(s);   // HandleFaintedMonActions case 6 (ABILITYEFFECT_INTIMIDATE1) after the replacements
        s->switchMask = 0;
        Resume(s);
        return s->request;
    }
    // ---- a new turn
    ClearTurnFlags(s);
    for (side = 0; side < 2; side++)
    {
        fs_battler *a = &s->side[side].act;
        s->pending[side] = acts[side];
        if (acts[side].type == FS_ACT_MOVE) a->chosenMove = acts[side].slot == 4 ? MOVE_STRUGGLE : a->moves[acts[side].slot];
    }
    // switches first, player side first; then moves by priority and speed
    for (side = 0; side < 2; side++) if (acts[side].type == FS_ACT_SWITCH) s->order[n++] = side;
    if (n < 2)
    {
        int m0 = (acts[0].type == FS_ACT_MOVE), m1 = (acts[1].type == FS_ACT_MOVE);
        if (m0 && m1) { int first = fs_who_strikes_first(s, 0, 1); s->order[n++] = first; s->order[n++] = first ^ 1; }
        else if (m0) s->order[n++] = 0;
        else if (m1) s->order[n++] = 1;
    }
    s->orderN = n;
    s->orderPos = 0;
    s->phase = 1;
    Resume(s);
    return s->request;
}

// GetWhoStrikesFirst: returns the side that moves first
int fs_who_strikes_first(fs_state *s, int b1, int b2)
{
    u32 sp1 = fs_effective_speed(s, b1), sp2 = fs_effective_speed(s, b2);
    fs_battler *a1 = &s->side[b1].act, *a2 = &s->side[b2].act;
    s8 p1 = fs_move(a1->chosenMove)->priority, p2 = fs_move(a2->chosenMove)->priority;
    if (fs_hold_effect(a1->item) == HOLD_EFFECT_QUICK_CLAW && fs_chance(s, fs_hold_param(a1->item), 100)) sp1 = 0xFFFFFFFFu;
    if (fs_hold_effect(a2->item) == HOLD_EFFECT_QUICK_CLAW && fs_chance(s, fs_hold_param(a2->item), 100)) sp2 = 0xFFFFFFFFu;
    if (p1 != p2) return p1 > p2 ? b1 : b2;
    if (sp1 == sp2) return fs_chance(s, 1, 2) ? b2 : b1;
    return sp1 > sp2 ? b1 : b2;
}

// ---------------------------------------------------------------------------------------------------------
// end of turn (field then battlers, in the game's order)

static void EndTurnDamage(fs_state *s, int side, s32 dmg)
{
    fs_battler *a = &s->side[side].act;
    if (!a->present || a->hp == 0) return;
    if (dmg <= 0) dmg = 1;
    if (dmg >= a->hp) { a->hp = 0; fs_faint(s, side); }
    else a->hp -= dmg;
}

static void Heal(fs_state *s, int side, s32 amount)
{
    fs_battler *a = &s->side[side].act;
    if (!a->present || a->hp == 0 || a->hp == a->maxHP) return;
    if (amount <= 0) amount = 1;
    a->hp = (a->hp + amount > a->maxHP) ? a->maxHP : a->hp + amount;
}

static int BattleOver(const fs_state *s) { return fs_alive_count(s, 0) == 0 || fs_alive_count(s, 1) == 0; }
#define ET_NEXT() { if (BattleOver(s)) return; if (!a->present) continue; }

void fs_end_turn(fs_state *s)
{
    int side, first = fs_who_strikes_first_ignoring_moves(s), b;
    // field
    for (side = 0; side < 2; side++)
    {
        fs_side *sd = &s->side[side];
        if (sd->reflect && --sd->reflect == 0) {}
        if (sd->lightscreen && --sd->lightscreen == 0) {}
        if (sd->mist && --sd->mist == 0) {}
        if (sd->safeguard && --sd->safeguard == 0) {}
    }
    for (side = 0; side < 2; side++)
    {
        fs_side *sd = &s->side[side];
        if (sd->wishTurns && --sd->wishTurns == 0)
        {
            if (sd->act.present && sd->act.hp) Heal(s, side, sd->act.maxHP / 2);   // heals whoever is in the slot now (wishMonId only names it)
        }
    }
    if (s->weather && s->weatherTurns != 0xFF)
    {
        if (--s->weatherTurns == 0) s->weather = 0;
    }
    if (s->weather == FS_WEATHER_SAND || s->weather == FS_WEATHER_HAIL)
    {
        for (b = 0; b < 2; b++)
        {
            side = (b == 0) ? first : first ^ 1;
            fs_battler *a = &s->side[side].act;
            if (!a->present) continue;
            if (s->weather == FS_WEATHER_SAND && (a->type1 == TYPE_ROCK || a->type2 == TYPE_ROCK || a->type1 == TYPE_GROUND || a->type2 == TYPE_GROUND || a->type1 == TYPE_STEEL || a->type2 == TYPE_STEEL || a->ability == ABILITY_SAND_VEIL)) continue;
            if (s->weather == FS_WEATHER_HAIL && (a->type1 == TYPE_ICE || a->type2 == TYPE_ICE)) continue;
            if (a->vol & (FS_V_UNDERGROUND | FS_V_UNDERWATER)) continue;
            EndTurnDamage(s, side, a->maxHP / 16);
            if (BattleOver(s)) return;
        }
    }
    // battlers, faster first; once a faint decides the battle the remaining effects never run
    for (b = 0; b < 2; b++)
    {
        side = (b == 0) ? first : first ^ 1;
        fs_battler *a = &s->side[side].act;
        int other = side ^ 1;
        if (BattleOver(s)) return;
        if (!a->present) continue;
        if (a->vol & FS_V_ROOTED) Heal(s, side, a->maxHP / 16);
        // abilities
        if (a->ability == ABILITY_RAIN_DISH && s->weather == FS_WEATHER_RAIN) Heal(s, side, a->maxHP / 16);
        if (a->ability == ABILITY_SHED_SKIN && a->status1 && fs_chance(s, 1, 3)) a->status1 = 0;
        if (a->ability == ABILITY_SPEED_BOOST && a->stages[STAT_SPEED] < 12 && a->isFirstTurn != 2) a->stages[STAT_SPEED]++;
        if (a->ability == ABILITY_TRUANT) a->truantCounter ^= 1;
        // items
        if (fs_hold_effect(a->item) == HOLD_EFFECT_LEFTOVERS) Heal(s, side, a->maxHP / 16);
        fs_end_turn_items(s, side);
        ET_NEXT();
        if ((a->vol & FS_V_LEECH_SEED) && s->side[other].act.present && a->hp)
        {
            s32 dmg = a->maxHP / 8;
            if (dmg == 0) dmg = 1;
            if (dmg > a->hp) dmg = a->hp;
            EndTurnDamage(s, side, dmg);
            if (a->ability == ABILITY_LIQUID_OOZE) EndTurnDamage(s, other, dmg); else Heal(s, other, dmg);
        }
        ET_NEXT();
        if ((a->status1 & FS_S1_PSN) && a->hp) EndTurnDamage(s, side, a->maxHP / 8);
        ET_NEXT();
        if ((a->status1 & FS_S1_TOX) && a->hp)
        {
            u16 ctr = (a->status1 & FS_S1_TOXCTR) >> 8;
            if (ctr < 15) a->status1 += 0x100;
            EndTurnDamage(s, side, (a->maxHP / 16) * ((a->status1 & FS_S1_TOXCTR) >> 8));
        }
        ET_NEXT();
        if ((a->status1 & FS_S1_BRN) && a->hp) EndTurnDamage(s, side, a->maxHP / 8);
        ET_NEXT();
        if ((a->vol & FS_V_NIGHTMARE) && a->hp)
        {
            if (a->status1 & FS_S1_SLEEP) EndTurnDamage(s, side, a->maxHP / 4); else a->vol &= ~FS_V_NIGHTMARE;
        }
        ET_NEXT();
        if ((a->vol & FS_V_CURSED) && a->hp) EndTurnDamage(s, side, a->maxHP / 4);
        ET_NEXT();
        if (a->wrapTurns)
        {
            if (--a->wrapTurns) EndTurnDamage(s, side, a->maxHP / 16);
        }
        ET_NEXT();
        if (a->disableTimer && --a->disableTimer == 0) {}
        if (a->encoreTimer && --a->encoreTimer == 0) {}
        if (a->encoreTimer && a->pp[a->encoredPos] == 0) a->encoreTimer = 0;
        if (a->chargeTimer && --a->chargeTimer == 0) a->vol &= ~FS_V_CHARGED;
        if (a->tauntTimer) a->tauntTimer--;
        if (a->yawnTimer && --a->yawnTimer == 0)
        {
            if (!a->status1 && a->ability != ABILITY_VITAL_SPIRIT && a->ability != ABILITY_INSOMNIA && !s->side[side].safeguard)
                a->status1 |= 2 + fs_roll(s, 4);   // 2-5 turns
        }
        if (a->perishTimer)
        {
            if (--a->perishTimer == 0) { a->hp = 0; fs_faint(s, side); }   // game: 3,2,1,0 then faints (stored +1 here)
        }
    }
    // future sight lands
    for (side = 0; side < 2; side++)
    {
        fs_side *sd = &s->side[side];
        if (sd->futureSightTurns && --sd->futureSightTurns == 0)
        {
            if (sd->act.present) EndTurnDamage(s, side, sd->futureSightDmg);
            if (BattleOver(s)) return;
        }
    }
    for (side = 0; side < 2; side++)
    {
        fs_battler *a = &s->side[side].act;
        if (a->isFirstTurn) a->isFirstTurn--;
    }
}

int fs_who_strikes_first_ignoring_moves(fs_state *s)
{
    u32 sp1 = fs_effective_speed(s, 0), sp2 = fs_effective_speed(s, 1);
    if (sp1 == sp2) return fs_chance(s, 1, 2) ? 1 : 0;
    return sp1 > sp2 ? 0 : 1;
}

// ---------------------------------------------------------------------------------------------------------
// the heuristic (same numbers as Sim_ValueBasic)

static float StatusPenalty(u16 st)
{
    if (st & FS_S1_SLEEP) return 0.30f;
    if (st & FS_S1_FRZ) return 0.40f;
    if (st & FS_S1_TOX) return 0.25f;
    if (st & FS_S1_BRN) return 0.20f;
    if (st & FS_S1_PAR) return 0.15f;
    if (st & FS_S1_PSN) return 0.10f;
    return 0.0f;
}

static float SideScore(const fs_state *s, int side)
{
    const fs_side *sd = &s->side[side];
    float score = 0.0f, m = 0.0f;
    int i;
    for (i = 0; i < FS_PARTY; i++)
    {
        const fs_mon *mon = &sd->party[i];
        u16 hp = mon->hp, st = mon->status1;
        if (!mon->species || mon->maxHP == 0) continue;
        if (sd->act.present && sd->act.monIdx == i) { hp = sd->act.hp; st = sd->act.status1; }
        if (hp == 0) continue;
        score += (float)hp / mon->maxHP * (1.0f - StatusPenalty(st)) + 0.15f;
    }
    if (sd->act.present && sd->act.hp)
    {
        const fs_battler *a = &sd->act;
        for (i = STAT_ATK; i <= STAT_SPDEF; i++) m += (a->stages[i] - 6) * 0.03f;
        if (a->vol & FS_V_CONFUSED) m -= 0.08f;
        if (a->vol & FS_V_INFATUATED) m -= 0.08f;
        if (a->vol & FS_V_SUBSTITUTE) m += 0.10f;
        if (a->vol & FS_V_CURSED) m -= 0.12f;
        if (a->vol & FS_V_NIGHTMARE) m -= 0.06f;
        if (a->wrapTurns) m -= 0.03f;
        if (a->vol & FS_V_FOCUS_ENERGY) m += 0.02f;
        if (a->vol & FS_V_LEECH_SEED) m -= 0.08f;
        if (a->perishTimer) m -= 0.10f * (3 - (a->perishTimer - 1));
        if (a->vol & FS_V_ROOTED) m += 0.03f;
        if (m > 0.6f) m = 0.6f;
        if (m < -0.6f) m = -0.6f;
        score += m;
    }
    if (sd->reflect) score += 0.05f;
    if (sd->lightscreen) score += 0.05f;
    if (sd->safeguard) score += 0.03f;
    if (sd->mist) score += 0.01f;
    score -= 0.05f * sd->spikes;
    return score;
}

float fs_value_basic(const fs_state *s, int side)
{
    float v;
    if (s->request == FS_REQ_DONE)
    {
        if (s->outcome == FS_OUTCOME_P0_WON) return side == 0 ? 1.0f : -1.0f;
        if (s->outcome == FS_OUTCOME_P1_WON) return side == 0 ? -1.0f : 1.0f;
        return 0.0f;
    }
    v = (SideScore(s, side) - SideScore(s, side ^ 1)) / 7.0f;
    return v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
}
