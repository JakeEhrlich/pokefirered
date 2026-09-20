// Flat C API (include/sim_capi.h). Thin wrappers over the sim API plus state accessors and batched rollouts.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "global.h"
#include "battle.h"
#include "pokemon.h"
#include "data.h"
#include "sim.h"
#include "sim_agent.h"
#include "sim_names.h"
#include "sim_items.h"
#include "sim_capi.h"
#include "sim_encode.h"
#include "sim_globals.h"
#include "util.h"
#include "constants/species.h"
#include "constants/moves.h"
#include "constants/items.h"

#define S(p) ((struct BattleSim *)(p))
#define CS(p) ((struct BattleSim *)(p)) // accessors bind the sim; the struct is not written

size_t sim_state_size(void) { return sizeof(struct BattleSim); }
size_t sim_pokemon_size(void) { return sizeof(struct Pokemon); }

void sim_init(void *sim, uint32_t flags, uint16_t seed) { Sim_Init(S(sim), flags, seed); }

int sim_set_team_tsv(void *sim, int side, const char *rows)
{
    struct Pokemon *party = Sim_Party(S(sim), side);
    const char *p = rows;
    int n = 0;
    Sim_Bind(S(sim));
    memset(party, 0, sizeof(struct Pokemon) * PARTY_SIZE);
    while (*p && n < PARTY_SIZE)
    {
        struct SimTeamMon m;
        const char *e = strchr(p, '\n');
        char line[1024];
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len); line[len] = 0;
        if (Sim_ParseTeamRow(line, &m))
            Sim_BuildTeamMon(&party[n++], &m);
        if (!e) break;
        p = e + 1;
    }
    return n;
}

int sim_set_party_bytes(void *sim, int side, const void *party600) { memcpy(Sim_Party(S(sim), side), party600, 600); return 0; }
void sim_get_party_bytes(const void *sim, int side, void *out600) { memcpy(out600, Sim_Party(CS(sim), side), 600); }
int sim_load_trainer(void *sim, uint16_t trainerId) { return Sim_LoadTrainerParty(S(sim), trainerId); }
void sim_set_trainer_id(void *sim, uint16_t trainerId) { S(sim)->trainerBattleOpponent_A = trainerId; }
uint16_t sim_trainer_for_ai_flags(uint32_t aiFlags) { return Sim_TrainerForAIFlags(aiFlags); }
void sim_set_rng(void *sim, uint32_t seed) { S(sim)->rngXorshift = 1; S(sim)->rngValue = seed | 1; S(sim)->rngCalls = 0; }
void sim_set_policy(void *sim, int side, int policy)
{
    Sim_SetPolicy(S(sim), side, policy == 1 ? Sim_RandomPolicy : policy == 2 ? Sim_VanillaAIPolicy : NULL);
}
void sim_set_strict(void *sim, int strict) { S(sim)->strictAnswers = strict != 0; }
void sim_set_max_turns(void *sim, int maxTurns) { S(sim)->maxTurns = maxTurns; }
int sim_start(void *sim) { return Sim_Start(S(sim)); }

int sim_run(void *sim)
{
    int r = Sim_Run(S(sim));
    return r == SIM_RUN_REQUEST ? 0 : r == SIM_RUN_FINISHED ? 1 : r == SIM_RUN_STUCK ? 2 : 3;
}
int sim_request_battler(const void *sim) { return CS(sim)->requestBattler; }
int sim_request_kind(const void *sim) { return CS(sim)->requestKind; }

static void ToAction(const uint8_t a[6], struct SimAction *out)
{
    memset(out, 0, sizeof(*out));
    out->type = a[0]; out->moveSlot = a[1]; out->target = a[2]; out->partySlot = a[3]; out->item = a[4] | (a[5] << 8);
}
static void FromAction(const struct SimAction *a, uint8_t out[6])
{
    out[0] = a->type; out[1] = a->moveSlot; out[2] = a->target; out[3] = a->partySlot; out[4] = a->item & 0xFF; out[5] = a->item >> 8;
}

int sim_answer(void *sim, int battler, const uint8_t action[6])
{
    Sim_Bind(S(sim));
    struct SimAction a;
    ToAction(action, &a);
    return Sim_Answer(S(sim), battler, &a);
}

int sim_legal_actions(const void *sim, int battler, uint8_t *out, int maxActions)
{
    struct SimAction acts[64];
    int n, i;
    Sim_Bind(CS(sim));
    n = Sim_LegalActions(CS(sim), battler, acts, maxActions < 64 ? maxActions : 64);
    for (i = 0; i < n; i++) FromAction(&acts[i], out + 6 * i);
    return n;
}
int sim_legal_switches(const void *sim, int battler, uint8_t *outSlots, int maxOut) { Sim_Bind(CS(sim)); return Sim_LegalSwitches(CS(sim), battler, outSlots, maxOut); }
int sim_finished(const void *sim) { return CS(sim)->finished; }
int sim_outcome(const void *sim) { return CS(sim)->battleOutcome; }
int sim_turn(const void *sim) { return CS(sim)->turnCount; }
uint32_t sim_rng_value(const void *sim) { return CS(sim)->rngValue; }
uint32_t sim_rng_calls(const void *sim) { return CS(sim)->rngCalls; }
int sim_error(const void *sim) { return CS(sim)->error; }

// ---- game setup
int sim_setup_game(void *sim, const void *partyA600, const void *partyB600, uint32_t seed, int maxTurns)
{
    struct BattleSim *b = S(sim);
    Sim_Init(b, BATTLE_TYPE_TRAINER, 1);
    memcpy(b->playerParty, partyA600, sizeof(b->playerParty));
    memcpy(b->enemyParty, partyB600, sizeof(b->enemyParty));
    b->createTrainerParty = FALSE;
    b->trainerBattleOpponent_A = Sim_TrainerForAIFlags(7);
    if (b->trainerBattleOpponent_A == 0) b->trainerBattleOpponent_A = 1;
    b->rngXorshift = 1;
    b->rngValue = seed | 1;
    b->rngCalls = 0;
    b->strictAnswers = 1;
    b->maxTurns = maxTurns;
    return Sim_Start(b);
}

int sim_party_from_tsv(const char *rows, void *out600)
{
    static _Thread_local struct BattleSim scratch;
    int n;
    Sim_Init(&scratch, 0, 1);
    n = sim_set_team_tsv(&scratch, 0, rows);
    memcpy(out600, scratch.playerParty, 600);
    return n;
}

// ---- encoding and rollouts
void sim_enc_sizes(int32_t out[16])
{
    memset(out, 0, sizeof(int32_t) * 16);
    out[0] = SIMENC_FLOATS; out[1] = SIMENC_INTS; out[2] = SIMENC_MONS; out[3] = SIMENC_MOVES; out[4] = SIMENC_TOKENS;
    out[5] = SIMENC_MON_F; out[6] = SIMENC_MOVE_F; out[7] = SIMENC_SIDE_F; out[8] = SIMENC_FIELD_F; out[9] = SIMENC_CAT_COUNT;
}

int sim_encode(void *sim, int side, float *outF, int32_t *outI) { return Sim_EncodeState(S(sim), side, outF, outI); }

static void PrepClone(struct BattleSim *clone, const struct BattleSim *src, uint32_t seed)
{
    memcpy(clone, src, sizeof(*clone));
    clone->policy[0] = clone->policy[1] = NULL;
    clone->strictAnswers = 0;
    clone->logEnabled = 0;
    clone->rngXorshift = 1;
    clone->rngValue = seed | 1;
}

// Runs to the next request of any kind or the end. A stuck/errored engine is reported as a draw.
static void RunToDecision(struct BattleSim *clone)
{
    int r = Sim_Run(clone);
    if (r == SIM_RUN_STUCK || r == SIM_RUN_ERROR) { clone->finished = 1; clone->battleOutcome = B_OUTCOME_DREW; }
}

int sim_joint_encode(const void *turnStart, int me, const uint8_t *mine, int nMine, const uint8_t *theirs, int nTheirs,
                     int samples, uint32_t seed, int side, float *outF, int32_t *outI)
{
    static _Thread_local struct BattleSim clone;
    const struct BattleSim *ts = CS(turnStart);
    int i, j, s, k = 0;
    for (i = 0; i < nMine; i++)
        for (j = 0; j < nTheirs; j++)
            for (s = 0; s < samples; s++, k++)
            {
                struct SimAction a, b;
                const struct SimAction *aFirst, *aSecond;
                u8 first, second;
                float *f = outF + (size_t)k * SIMENC_FLOATS;
                int32_t *ii = outI + (size_t)k * SIMENC_INTS;
                ToAction(mine + 6 * i, &a);
                ToAction(theirs + 6 * j, &b);
                PrepClone(&clone, ts, seed + 7919u * s + 104729u * (i * nTheirs + j));
                first = clone.requestBattler;
                second = first ^ BIT_SIDE;
                aFirst = (first == me) ? &a : &b;
                aSecond = (first == me) ? &b : &a;
                if (Sim_Answer(&clone, first, aFirst) != 0) { clone.finished = 1; clone.battleOutcome = B_OUTCOME_DREW; }
                else
                {
                    int r = Sim_Run(&clone);
                    if (r == SIM_RUN_REQUEST && clone.requestBattler == second && clone.requestKind == SIM_REQ_ACTION)
                    {
                        if (Sim_Answer(&clone, second, aSecond) != 0) { clone.finished = 1; clone.battleOutcome = B_OUTCOME_DREW; }
                        else RunToDecision(&clone);
                    }
                    else if (r == SIM_RUN_STUCK || r == SIM_RUN_ERROR) { clone.finished = 1; clone.battleOutcome = B_OUTCOME_DREW; }
                }
                Sim_EncodeState(&clone, side, f, ii);
            }
    return k;
}

int sim_after_encode(const void *sim, int me, const uint8_t *acts, int n, int samples, uint32_t seed, int side, float *outF, int32_t *outI)
{
    static _Thread_local struct BattleSim clone;
    int i, s, k = 0;
    for (i = 0; i < n; i++)
        for (s = 0; s < samples; s++, k++)
        {
            struct SimAction a;
            float *f = outF + (size_t)k * SIMENC_FLOATS;
            int32_t *ii = outI + (size_t)k * SIMENC_INTS;
            ToAction(acts + 6 * i, &a);
            PrepClone(&clone, CS(sim), seed + 7919u * s + 131u * i);
            if (clone.requestKind != SIM_REQ_NONE && clone.requestBattler != me)
            {
                struct SimAction other;
                Sim_RandomPolicy(&clone, clone.requestBattler, clone.requestKind, &other);
                Sim_Answer(&clone, clone.requestBattler, &other);
                RunToDecision(&clone);
            }
            if (!clone.finished && clone.requestKind != SIM_REQ_NONE && clone.requestBattler == me)
            {
                if (Sim_Answer(&clone, me, &a) != 0) { clone.finished = 1; clone.battleOutcome = B_OUTCOME_DREW; }
                else RunToDecision(&clone);
            }
            Sim_EncodeState(&clone, side, f, ii);
        }
    return k;
}

void sim_regret_matching(const float *M, int n, int m, int iters, int plus, int alternating, int linearAvg, float *sigmaRow, float *sigmaCol)
{
    Sim_RegretMatching(M, n, m, iters, plus, alternating, linearAvg, sigmaRow, sigmaCol);
}

// ---- agents
void *sim_agent_new(const char *spec, uint32_t seed)
{
    struct SimAgent *ag = calloc(1, sizeof(*ag));
    if (Sim_AgentFromSpec(ag, spec) != 0) { free(ag); return NULL; }
    ag->rng = seed * 2654435761u + 17;
    return ag;
}
void sim_agent_free(void *agent) { free(agent); }
const char *sim_agent_name(const void *agent) { return ((const struct SimAgent *)agent)->name; }

int sim_agent_decide(void *agent, void *sim, const void *turnStart, int battler, int kind, uint8_t out[6])
{
    struct SimAgent *ag = agent;
    struct SimAction act;
    memset(&act, 0, sizeof(act));
    if (ag->isGameAI)
    {
        if ((battler & BIT_SIDE) != B_SIDE_OPPONENT) return -1;
        Sim_Bind(S(sim));
        Sim_VanillaAIPolicy(S(sim), battler, kind, &act);
    }
    else
        ag->decide(ag, S(sim), (const struct BattleSim *)turnStart, battler, kind, &act);
    FromAction(&act, out);
    return 0;
}

float sim_value_basic(const void *sim, int side) { return Sim_ValueBasic(CS(sim), side, NULL); }
float sim_value_material(const void *sim, int side) { return Sim_ValueMaterial(CS(sim), side, NULL); }

// ---- accessors
int sim_get_party_mon(const void *sim, int side, int slot, int32_t out[32])
{
    struct Pokemon *mon;
    int i;
    if (slot < 0 || slot >= PARTY_SIZE) return -1;
    Sim_Bind(CS(sim));
    mon = &Sim_Party(CS(sim), side)[slot];
    memset(out, 0, sizeof(int32_t) * 32);
    out[0] = GetMonData(mon, MON_DATA_SPECIES);
    if (out[0] == 0) return 0;
    out[1] = GetMonData(mon, MON_DATA_LEVEL);
    out[2] = GetMonData(mon, MON_DATA_HP);
    out[3] = GetMonData(mon, MON_DATA_MAX_HP);
    out[4] = GetMonData(mon, MON_DATA_STATUS);
    out[5] = GetMonData(mon, MON_DATA_HELD_ITEM);
    out[6] = GetMonData(mon, MON_DATA_ABILITY_NUM);
    out[7] = gSpeciesInfo[out[0] < NUM_SPECIES ? out[0] : 0].abilities[out[6] & 1] ? gSpeciesInfo[out[0] < NUM_SPECIES ? out[0] : 0].abilities[out[6] & 1] : gSpeciesInfo[out[0] < NUM_SPECIES ? out[0] : 0].abilities[0];
    for (i = 0; i < 4; i++) { out[8 + i] = GetMonData(mon, MON_DATA_MOVE1 + i); out[12 + i] = GetMonData(mon, MON_DATA_PP1 + i); }
    for (i = 0; i < 4; i++) out[16 + i] = out[8 + i] ? CalculatePPWithBonus(out[8 + i], GetMonData(mon, MON_DATA_PP_BONUSES), i) : 0;
    out[20] = out[3];
    out[21] = GetMonData(mon, MON_DATA_ATK);
    out[22] = GetMonData(mon, MON_DATA_DEF);
    out[23] = GetMonData(mon, MON_DATA_SPEED);
    out[24] = GetMonData(mon, MON_DATA_SPATK);
    out[25] = GetMonData(mon, MON_DATA_SPDEF);
    out[26] = out[0] < NUM_SPECIES ? gSpeciesInfo[out[0]].types[0] : 0;
    out[27] = out[0] < NUM_SPECIES ? gSpeciesInfo[out[0]].types[1] : 0;
    out[28] = GetMonData(mon, MON_DATA_FRIENDSHIP);
    out[29] = GetMonData(mon, MON_DATA_IS_EGG);
    out[30] = GetMonGender(mon);
    out[31] = GetMonData(mon, MON_DATA_PERSONALITY) & 0xFF;
    return 1;
}

int sim_get_battler(const void *sim, int battler, int32_t out[64])
{
    struct BattlePokemon *m;
    struct DisableStruct *d;
    int i;
    Sim_Bind(CS(sim));
    memset(out, 0, sizeof(int32_t) * 64);
    if (battler < 0 || battler >= gBattlersCount) return -1;
    m = &gBattleMons[battler];
    d = &gDisableStructs[battler];
    out[0] = m->species; out[1] = m->hp; out[2] = m->maxHP; out[3] = m->level; out[4] = m->item; out[5] = m->ability;
    out[6] = m->type1; out[7] = m->type2;
    for (i = 0; i < 4; i++) { out[8 + i] = m->moves[i]; out[12 + i] = m->pp[i]; }
    out[16] = m->maxHP; out[17] = m->attack; out[18] = m->defense; out[19] = m->speed; out[20] = m->spAttack; out[21] = m->spDefense;
    for (i = 0; i < 8; i++) out[22 + i] = m->statStages[i];
    out[30] = m->status1; out[31] = m->status2; out[32] = gStatuses3[battler];
    out[33] = d->disabledMove; out[34] = d->disableTimer; out[35] = d->encoredMove; out[36] = d->encoredMovePos; out[37] = d->encoreTimer;
    out[38] = d->protectUses; out[39] = d->stockpileCounter; out[40] = d->substituteHP; out[41] = d->perishSongTimer; out[42] = d->rolloutTimer;
    out[43] = d->chargeTimer; out[44] = d->tauntTimer; out[45] = d->furyCutterCounter; out[46] = d->isFirstTurn; out[47] = d->truantCounter;
    out[48] = d->rechargeTimer; out[49] = d->battlerWithSureHit;
    out[50] = gLastMoves[battler]; out[51] = gLastLandedMoves[battler]; out[52] = gLastResultingMoves[battler];
    out[53] = gBattlerPartyIndexes[battler]; out[54] = (gAbsentBattlerFlags >> battler) & 1;
    out[55] = gWishFutureKnock.wishCounter[battler]; out[56] = gWishFutureKnock.futureSightCounter[battler];
    out[57] = gWishFutureKnock.futureSightDmg[battler]; out[58] = gWishFutureKnock.futureSightAttacker[battler];
    return 1;
}

int sim_get_side(const void *sim, int side, int32_t out[16])
{
    struct Pokemon *party;
    int i;
    Sim_Bind(CS(sim));
    memset(out, 0, sizeof(int32_t) * 16);
    out[0] = gSideTimers[side].reflectTimer; out[1] = gSideTimers[side].lightscreenTimer; out[2] = gSideTimers[side].mistTimer;
    out[3] = gSideTimers[side].safeguardTimer; out[4] = gSideTimers[side].followmeTimer; out[5] = gSideTimers[side].spikesAmount;
    out[6] = gBattleStruct->hpOnSwitchout[side];
    party = Sim_Party(CS(sim), side);
    for (i = 0; i < PARTY_SIZE; i++)
    {
        u16 sp = GetMonData(&party[i], MON_DATA_SPECIES_OR_EGG);
        if (sp == SPECIES_NONE || sp == SPECIES_EGG) continue;
        if (GetMonData(&party[i], MON_DATA_HP)) out[7]++; else out[8]++;
    }
    return 1;
}

int sim_get_field(const void *sim, int32_t out[16])
{
    Sim_Bind(CS(sim));
    memset(out, 0, sizeof(int32_t) * 16);
    out[0] = gBattleWeather; out[1] = gWishFutureKnock.weatherDuration; out[2] = CS(sim)->turnCount; out[3] = gBattlersCount;
    out[4] = gBattleTypeFlags; out[5] = CS(sim)->requestKind; out[6] = CS(sim)->requestBattler;
    return 1;
}
int sim_battlers_count(const void *sim) { return CS(sim)->battlersCount; }

const char *sim_species_name(int id) { return (id >= 0 && id < gSimSpeciesNames_Count) ? gSimSpeciesNames[id] : "?"; }
const char *sim_move_name(int id) { return (id >= 0 && id < gSimMoveNames_Count) ? gSimMoveNames[id] : "?"; }
const char *sim_item_name(int id) { return (id >= 0 && id < gSimItemNames_Count) ? gSimItemNames[id] : "?"; }
const char *sim_ability_name(int id) { return (id >= 0 && id < gSimAbilityNames_Count) ? gSimAbilityNames[id] : "?"; }

int sim_get_move_data(int move, int32_t out[16])
{
    memset(out, 0, sizeof(int32_t) * 16);
    if (move < 0 || move >= MOVES_COUNT) return -1;
    out[0] = gBattleMoves[move].effect; out[1] = gBattleMoves[move].power; out[2] = gBattleMoves[move].type; out[3] = gBattleMoves[move].accuracy;
    out[4] = gBattleMoves[move].pp; out[5] = gBattleMoves[move].secondaryEffectChance; out[6] = gBattleMoves[move].target;
    out[7] = gBattleMoves[move].priority; out[8] = gBattleMoves[move].flags;
    return 1;
}

int sim_get_species_data(int species, int32_t out[16])
{
    memset(out, 0, sizeof(int32_t) * 16);
    if (species <= 0 || species >= NUM_SPECIES) return -1;
    out[0] = gSpeciesInfo[species].baseHP; out[1] = gSpeciesInfo[species].baseAttack; out[2] = gSpeciesInfo[species].baseDefense;
    out[3] = gSpeciesInfo[species].baseSpeed; out[4] = gSpeciesInfo[species].baseSpAttack; out[5] = gSpeciesInfo[species].baseSpDefense;
    out[6] = gSpeciesInfo[species].types[0]; out[7] = gSpeciesInfo[species].types[1];
    out[8] = gSpeciesInfo[species].abilities[0]; out[9] = gSpeciesInfo[species].abilities[1];
    out[10] = gSpeciesInfo[species].genderRatio;
    {
        extern u16 GetPokedexHeightWeight(u16 dexNum, u8 data);
        u16 dex = SpeciesToNationalPokedexNum(species);
        out[11] = GetPokedexHeightWeight(dex, 1);
        out[12] = GetPokedexHeightWeight(dex, 0);
    }
    return 1;
}

int sim_get_item_data(int item, int32_t out[8])
{
    memset(out, 0, sizeof(int32_t) * 8);
    if (item < 0 || item >= ITEMS_COUNT || !gSimItems[item].name) return -1;
    out[0] = gSimItems[item].holdEffect; out[1] = gSimItems[item].holdEffectParam; out[2] = gSimItems[item].pocket; out[3] = gSimItems[item].battleUsage;
    return 1;
}
