// Built-in agents, value functions and the one-turn joint-action machinery (see include/sim_agent.h).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "global.h"
#include "battle.h"
#include "battle_main.h"
#include "battle_util.h"
#include "pokemon.h"
#include "data.h"
#include "sim.h"
#include "sim_agent.h"
#include "sim_globals.h"
#include "util.h"
#include "constants/battle_ai.h"
#include "constants/moves.h"
#include "constants/species.h"
#include "constants/pokemon.h"
#include "constants/battle_move_effects.h"

#define MAX_ACTIONS 32

u32 Sim_AgentRandom(struct SimAgent *ag)
{
    u32 x = ag->rng ? ag->rng : 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    ag->rng = x;
    return x;
}

static float Frand(struct SimAgent *ag) { return (Sim_AgentRandom(ag) >> 8) * (1.0f / 16777216.0f); }

// ---------------------------------------------------------------------------------------------------------
// Value functions

static float StatusPenalty(u32 status1)
{
    if (status1 & STATUS1_SLEEP) return 0.25f;
    if (status1 & STATUS1_FREEZE) return 0.40f;
    if (status1 & STATUS1_TOXIC_POISON) return 0.25f;
    if (status1 & STATUS1_BURN) return 0.20f;
    if (status1 & STATUS1_PARALYSIS) return 0.15f;
    if (status1 & STATUS1_POISON) return 0.10f;
    return 0.0f;
}

static float SideScore(struct BattleSim *sim, u8 side, int material)
{
    struct Pokemon *party = Sim_Party(sim, side);
    float score = 0.0f;
    int i, b;

    for (i = 0; i < PARTY_SIZE; i++)
    {
        u16 species = GetMonData(&party[i], MON_DATA_SPECIES_OR_EGG);
        u16 hp, maxHp;
        float f;
        if (species == SPECIES_NONE || species == SPECIES_EGG)
            continue;
        hp = GetMonData(&party[i], MON_DATA_HP);
        maxHp = GetMonData(&party[i], MON_DATA_MAX_HP);
        if (hp == 0 || maxHp == 0)
            continue;
        f = (float)hp / (float)maxHp;
        if (material)
            score += f;
        else
            score += f * (1.0f - StatusPenalty(GetMonData(&party[i], MON_DATA_STATUS))) + 0.15f;
    }
    if (material)
        return score;
    for (b = 0; b < gBattlersCount; b++)
    {
        struct BattlePokemon *mon = &gBattleMons[b];
        float m = 0.0f;
        int s;
        if (GetBattlerSide(b) != side || (gAbsentBattlerFlags & gBitTable[b]) || mon->hp == 0)
            continue;
        for (s = STAT_ATK; s <= STAT_SPDEF; s++)
            m += ((int)mon->statStages[s] - 6) * 0.03f;
        if (mon->status2 & STATUS2_CONFUSION) m -= 0.08f;
        if (mon->status2 & STATUS2_INFATUATION) m -= 0.08f;
        if (mon->status2 & STATUS2_SUBSTITUTE) m += 0.10f;
        if (mon->status2 & STATUS2_CURSED) m -= 0.12f;
        if (mon->status2 & STATUS2_NIGHTMARE) m -= 0.06f;
        if (mon->status2 & STATUS2_WRAPPED) m -= 0.03f;
        if (mon->status2 & STATUS2_FOCUS_ENERGY) m += 0.02f;
        if (gStatuses3[b] & STATUS3_LEECHSEED) m -= 0.08f;
        if (gStatuses3[b] & STATUS3_PERISH_SONG) m -= 0.10f * (3 - gDisableStructs[b].perishSongTimer);
        if (gStatuses3[b] & STATUS3_ROOTED) m += 0.03f;
        if (m > 0.6f) m = 0.6f;
        if (m < -0.6f) m = -0.6f;
        score += m;
    }
    if (gSideTimers[side].reflectTimer) score += 0.05f;
    if (gSideTimers[side].lightscreenTimer) score += 0.05f;
    if (gSideTimers[side].safeguardTimer) score += 0.03f;
    if (gSideTimers[side].mistTimer) score += 0.01f;
    score -= 0.05f * gSideTimers[side].spikesAmount;
    return score;
}

static float Terminal(struct BattleSim *sim, u8 side, int *isTerminal)
{
    *isTerminal = 0;
    if (!sim->finished)
        return 0.0f;
    *isTerminal = 1;
    if (gBattleOutcome == B_OUTCOME_WON) return side == B_SIDE_PLAYER ? 1.0f : -1.0f;
    if (gBattleOutcome == B_OUTCOME_LOST) return side == B_SIDE_PLAYER ? -1.0f : 1.0f;
    return 0.0f;
}

float Sim_ValueBasic(struct BattleSim *sim, u8 side, void *ctx)
{
    int t;
    float v;
    Sim_Bind(sim);
    v = Terminal(sim, side, &t);
    if (t) return v;
    v = (SideScore(sim, side, 0) - SideScore(sim, side ^ 1, 0)) / 7.0f;
    return v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
}

float Sim_ValueMaterial(struct BattleSim *sim, u8 side, void *ctx)
{
    int t;
    float v;
    Sim_Bind(sim);
    v = Terminal(sim, side, &t);
    if (t) return v;
    v = (SideScore(sim, side, 1) - SideScore(sim, side ^ 1, 1)) / 6.0f;
    return v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
}

// ---------------------------------------------------------------------------------------------------------
// Damage / matchup heuristics

static int TypeMultiplier(u8 moveType, u8 defType1, u8 defType2)
{
    int mult = 100, i;
    for (i = 0; gTypeEffectiveness[i] != TYPE_ENDTABLE; i += 3)
    {
        if (gTypeEffectiveness[i] == TYPE_FORESIGHT)
            continue;
        if (gTypeEffectiveness[i] == moveType)
        {
            if (gTypeEffectiveness[i + 1] == defType1)
                mult = mult * gTypeEffectiveness[i + 2] / 10;
            if (gTypeEffectiveness[i + 1] == defType2 && defType2 != defType1)
                mult = mult * gTypeEffectiveness[i + 2] / 10;
        }
    }
    return mult;
}

int Sim_EstimateDamage(struct BattleSim *sim, u8 attacker, u8 defender, u8 moveSlot)
{
    u16 move;
    s32 dmg;
    int mult, acc;
    struct BattlePokemon *atk, *def;

    Sim_Bind(sim);
    atk = &gBattleMons[attacker];
    def = &gBattleMons[defender];
    move = atk->moves[moveSlot & 3];
    if (move == MOVE_NONE || move >= MOVES_COUNT)
        return 0;
    switch (gBattleMoves[move].effect)
    {
    case EFFECT_LEVEL_DAMAGE: dmg = atk->level; break;
    case EFFECT_PSYWAVE: dmg = atk->level; break;
    case EFFECT_DRAGON_RAGE: dmg = 40; break;
    case EFFECT_SONICBOOM: dmg = 20; break;
    case EFFECT_SUPER_FANG: dmg = def->hp / 2; break;
    case EFFECT_ENDEAVOR: dmg = def->hp > atk->hp ? def->hp - atk->hp : 0; break;
    case EFFECT_OHKO: dmg = 0; break;
    default:
        if (gBattleMoves[move].power == 0)
            return 0;
        dmg = CalculateBaseDamage(atk, def, move, gSideStatuses[GetBattlerSide(defender)], 0, 0, attacker, defender);
        if (atk->type1 == gBattleMoves[move].type || atk->type2 == gBattleMoves[move].type)
            dmg = dmg * 15 / 10;
        mult = TypeMultiplier(gBattleMoves[move].type, def->type1, def->type2);
        dmg = dmg * mult / 100;
        dmg = dmg * 92 / 100;
        if (gBattleMoves[move].effect == EFFECT_MULTI_HIT) dmg *= 3;
        if (gBattleMoves[move].effect == EFFECT_DOUBLE_HIT) dmg *= 2;
        if (gBattleMoves[move].effect == EFFECT_TRIPLE_KICK) dmg *= 3;
        break;
    }
    acc = gBattleMoves[move].accuracy;
    if (acc == 0) acc = 100;
    if (gBattleMoves[move].effect == EFFECT_OHKO) acc = 30;
    if (dmg > def->hp) dmg = def->hp;
    return dmg * acc / 100;
}

int Sim_TypeMatchupScore(struct BattleSim *sim, u16 species, u8 oppBattler)
{
    u8 t1, t2, o1, o2;
    int off, def, a, b;
    Sim_Bind(sim);
    if (species == SPECIES_NONE || species >= NUM_SPECIES)
        return 0;
    t1 = gSpeciesInfo[species].types[0];
    t2 = gSpeciesInfo[species].types[1];
    o1 = gBattleMons[oppBattler].type1;
    o2 = gBattleMons[oppBattler].type2;
    a = TypeMultiplier(t1, o1, o2);
    b = TypeMultiplier(t2, o1, o2);
    off = a > b ? a : b;
    a = TypeMultiplier(o1, t1, t2);
    b = TypeMultiplier(o2, t1, t2);
    def = a > b ? a : b;
    return (off - def) / 10;
}

u16 Sim_TrainerForAIFlags(u32 aiFlags)
{
    u16 t;
    for (t = 1; t < 743; t++)
    {
        const struct Trainer *tr = &gTrainers[t];
        if (tr->aiFlags == aiFlags && tr->items[0] == 0 && tr->items[1] == 0 && tr->items[2] == 0 && tr->items[3] == 0
         && tr->partySize > 0 && !tr->doubleBattle)
            return t;
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------------------
// Simulation helpers

static void RandomLegal(struct SimAgent *ag, struct BattleSim *sim, u8 battler, u8 kind, struct SimAction *out)
{
    struct SimAction acts[MAX_ACTIONS];
    u8 slots[PARTY_SIZE];
    int n;
    memset(out, 0, sizeof(*out));
    if (kind == SIM_REQ_SWITCH)
    {
        n = Sim_LegalSwitches(sim, battler, slots, PARTY_SIZE);
        out->type = B_ACTION_SWITCH;
        out->partySlot = n ? slots[Sim_AgentRandom(ag) % n] : 0;
        return;
    }
    n = Sim_LegalActions(sim, battler, acts, MAX_ACTIONS);
    if (n == 0) { out->type = B_ACTION_USE_MOVE; out->moveSlot = 0; out->target = 0xFF; return; }
    *out = acts[Sim_AgentRandom(ag) % n];
}

// Plays out pending mid-turn requests randomly until the next action request or the end.
static void RunToNextTurn(struct SimAgent *ag, struct BattleSim *sim)
{
    int guard = 0;
    for (;;)
    {
        int r = Sim_Run(sim);
        if (r != SIM_RUN_REQUEST)
            return;
        if (sim->requestKind == SIM_REQ_ACTION || ++guard > 64)
            return;
        {
            struct SimAction a;
            RandomLegal(ag, sim, sim->requestBattler, sim->requestKind, &a);
            if (Sim_Answer(sim, sim->requestBattler, &a) != 0)
                return;
        }
    }
}

float Sim_SimulateJoint(struct SimAgent *ag, const struct BattleSim *turnStart, u8 me, const struct SimAction *mine,
                        const struct SimAction *theirs, u32 seed)
{
    static _Thread_local struct BattleSim clone;
    u8 first, second;
    const struct SimAction *aFirst, *aSecond;
    int r;

    memcpy(&clone, turnStart, sizeof(clone));
    clone.policy[0] = clone.policy[1] = NULL;
    clone.strictAnswers = 0;
    clone.logEnabled = 0;
    clone.rngXorshift = 1;
    clone.rngValue = seed | 1;
    first = clone.requestBattler;                // whoever the engine asked first at the turn start
    second = first ^ BIT_SIDE;
    aFirst = (first == me) ? mine : theirs;
    aSecond = (first == me) ? theirs : mine;
    ag->simulatedTurns++;
    if (Sim_Answer(&clone, first, aFirst) != 0)
        return -1.0f;
    r = Sim_Run(&clone);
    if (r == SIM_RUN_REQUEST && clone.requestBattler == second && clone.requestKind == SIM_REQ_ACTION)
    {
        if (Sim_Answer(&clone, second, aSecond) != 0)
            return -1.0f;
        RunToNextTurn(ag, &clone);
    }
    else if (r == SIM_RUN_REQUEST)
    {
        RunToNextTurn(ag, &clone);
    }
    return ag->value(&clone, GetBattlerSide(me), ag->valueCtx);
}

float Sim_SimulateAfter(struct SimAgent *ag, const struct BattleSim *sim, u8 me, const struct SimAction *act, u32 seed)
{
    static _Thread_local struct BattleSim clone;
    memcpy(&clone, sim, sizeof(clone));
    clone.policy[0] = clone.policy[1] = NULL;
    clone.strictAnswers = 0;
    clone.logEnabled = 0;
    clone.rngXorshift = 1;
    clone.rngValue = seed | 1;
    ag->simulatedTurns++;
    if (Sim_Answer(&clone, me, act) != 0)
        return -1.0f;
    RunToNextTurn(ag, &clone);
    return ag->value(&clone, GetBattlerSide(me), ag->valueCtx);
}

// ---------------------------------------------------------------------------------------------------------
// Regret matching

static void Normalize(const float *pos, int n, float *out)
{
    float s = 0.0f;
    int i;
    for (i = 0; i < n; i++) s += pos[i] > 0 ? pos[i] : 0;
    for (i = 0; i < n; i++) out[i] = s > 0 ? (pos[i] > 0 ? pos[i] / s : 0.0f) : 1.0f / n;
}

void Sim_RegretMatching(const float *M, int n, int m, int iters, int plus, int alternating, int linearAvg,
                        float *sigmaRow, float *sigmaCol)
{
    float R1[MAX_ACTIONS] = {0}, R2[MAX_ACTIONS] = {0}, S1[MAX_ACTIONS] = {0}, S2[MAX_ACTIONS] = {0};
    float s1[MAX_ACTIONS], s2[MAX_ACTIONS], s1old[MAX_ACTIONS], u1[MAX_ACTIONS], u2[MAX_ACTIONS];
    int t, i, j;

    Normalize(R1, n, s1);
    Normalize(R2, m, s2);
    for (t = 1; t <= iters; t++)
    {
        float w = linearAvg ? (float)t : 1.0f, U1 = 0, U2 = 0;
        memcpy(s1old, s1, sizeof(float) * n);
        for (i = 0; i < n; i++)
        {
            u1[i] = 0;
            for (j = 0; j < m; j++) u1[i] += s2[j] * M[i * m + j];
            U1 += s1[i] * u1[i];
        }
        for (i = 0; i < n; i++)
        {
            R1[i] += u1[i] - U1;
            if (plus && R1[i] < 0) R1[i] = 0;
        }
        Normalize(R1, n, s1);
        {
            const float *sRow = alternating ? s1 : s1old;
            for (j = 0; j < m; j++)
            {
                u2[j] = 0;
                for (i = 0; i < n; i++) u2[j] += sRow[i] * -M[i * m + j];
                U2 += s2[j] * u2[j];
            }
        }
        for (j = 0; j < m; j++)
        {
            R2[j] += u2[j] - U2;
            if (plus && R2[j] < 0) R2[j] = 0;
        }
        Normalize(R2, m, s2);
        for (i = 0; i < n; i++) S1[i] += w * s1[i];
        for (j = 0; j < m; j++) S2[j] += w * s2[j];
    }
    Normalize(S1, n, sigmaRow);
    Normalize(S2, m, sigmaCol);
}

static int SampleWithFloor(struct SimAgent *ag, const float *sigma, int n, float floor)
{
    float p[MAX_ACTIONS], s = 0.0f, r;
    int i;
    for (i = 0; i < n; i++) { p[i] = sigma[i] < floor ? floor : sigma[i]; s += p[i]; }
    r = Frand(ag) * s;
    for (i = 0; i < n; i++) { r -= p[i]; if (r <= 0) return i; }
    return n - 1;
}

// ---------------------------------------------------------------------------------------------------------
// Agents

static void DecideRandom(struct SimAgent *ag, struct BattleSim *sim, const struct BattleSim *ts, u8 battler, u8 kind, struct SimAction *out)
{
    ag->decisions++;
    RandomLegal(ag, sim, battler, kind, out);
}

static void DecideMoveBias(struct SimAgent *ag, struct BattleSim *sim, const struct BattleSim *ts, u8 battler, u8 kind, struct SimAction *out)
{
    struct SimAction acts[MAX_ACTIONS], moves[MAX_ACTIONS], sw[MAX_ACTIONS];
    int n, i, nm = 0, ns = 0;
    ag->decisions++;
    if (kind == SIM_REQ_SWITCH) { RandomLegal(ag, sim, battler, kind, out); return; }
    n = Sim_LegalActions(sim, battler, acts, MAX_ACTIONS);
    for (i = 0; i < n; i++) { if (acts[i].type == B_ACTION_USE_MOVE) moves[nm++] = acts[i]; else sw[ns++] = acts[i]; }
    if (nm && (ns == 0 || Frand(ag) < ag->epsilon)) *out = moves[Sim_AgentRandom(ag) % nm];
    else if (ns) *out = sw[Sim_AgentRandom(ag) % ns];
    else RandomLegal(ag, sim, battler, kind, out);
}

static u8 OpponentOf(struct BattleSim *sim, u8 battler)
{
    u8 o = battler ^ BIT_SIDE;
    Sim_Bind(sim);
    if ((gAbsentBattlerFlags & gBitTable[o]) && (gBattleTypeFlags & BATTLE_TYPE_DOUBLE))
        o ^= BIT_FLANK;
    return o;
}

static void DecideGreedy(struct SimAgent *ag, struct BattleSim *sim, const struct BattleSim *ts, u8 battler, u8 kind, struct SimAction *out)
{
    struct SimAction acts[MAX_ACTIONS];
    int n, i, best = -1, bestScore = -1;
    u8 opp = OpponentOf(sim, battler);
    ag->decisions++;
    if (kind == SIM_REQ_SWITCH)
    {
        u8 slots[PARTY_SIZE];
        struct Pokemon *party = Sim_Party(sim, GetBattlerSide(battler));
        int ns = Sim_LegalSwitches(sim, battler, slots, PARTY_SIZE);
        memset(out, 0, sizeof(*out));
        out->type = B_ACTION_SWITCH;
        out->partySlot = ns ? slots[0] : 0;
        for (i = 0; i < ns; i++)
        {
            int sc = Sim_TypeMatchupScore(sim, GetMonData(&party[slots[i]], MON_DATA_SPECIES), opp) * 100
                   + GetMonData(&party[slots[i]], MON_DATA_HP) * 100 / (GetMonData(&party[slots[i]], MON_DATA_MAX_HP) + 1);
            if (sc > bestScore) { bestScore = sc; out->partySlot = slots[i]; }
        }
        return;
    }
    n = Sim_LegalActions(sim, battler, acts, MAX_ACTIONS);
    for (i = 0; i < n; i++)
    {
        int sc;
        if (acts[i].type != B_ACTION_USE_MOVE) continue;
        sc = Sim_EstimateDamage(sim, battler, acts[i].target == 0xFF ? opp : acts[i].target, acts[i].moveSlot) * 4 + (int)(Sim_AgentRandom(ag) % 3);
        if (sc > bestScore) { bestScore = sc; best = i; }
    }
    if (best < 0) { RandomLegal(ag, sim, battler, kind, out); return; }
    *out = acts[best];
}

static void DecideEpsGreedy(struct SimAgent *ag, struct BattleSim *sim, const struct BattleSim *ts, u8 battler, u8 kind, struct SimAction *out)
{
    if (Frand(ag) < ag->epsilon) { ag->decisions++; RandomLegal(ag, sim, battler, kind, out); return; }
    DecideGreedy(ag, sim, ts, battler, kind, out);
}

// Fills the payoff matrix for `me` from the turn-start state. Returns 0 if a joint matrix is not possible
// (no turn-start state, doubles, mid-turn request) in which case the caller evaluates single actions.
static int BuildMatrix(struct SimAgent *ag, struct BattleSim *sim, const struct BattleSim *ts, u8 me, u8 kind,
                       struct SimAction *mine, int *nMine, struct SimAction *theirs, int *nTheirs, float *M)
{
    int i, j, s;
    u8 opp;
    u32 base = Sim_AgentRandom(ag);
    if (ts == NULL || kind != SIM_REQ_ACTION || (ts->battleTypeFlags & BATTLE_TYPE_DOUBLE) || ts->turnCount != sim->turnCount)
        return 0;
    opp = me ^ BIT_SIDE;
    *nMine = Sim_LegalActions((struct BattleSim *)ts, me, mine, MAX_ACTIONS);
    *nTheirs = Sim_LegalActions((struct BattleSim *)ts, opp, theirs, MAX_ACTIONS);
    if (*nMine == 0 || *nTheirs == 0)
        return 0;
    for (i = 0; i < *nMine; i++)
        for (j = 0; j < *nTheirs; j++)
        {
            float v = 0.0f;
            for (s = 0; s < ag->samples; s++)
                v += Sim_SimulateJoint(ag, ts, me, &mine[i], &theirs[j], base + 7919u * s + 104729u * (i * *nTheirs + j));
            M[i * *nTheirs + j] = v / ag->samples;
            ag->matrixCells++;
        }
    return 1;
}

static void DecideBySingleEval(struct SimAgent *ag, struct BattleSim *sim, u8 battler, u8 kind, struct SimAction *out)
{
    struct SimAction acts[MAX_ACTIONS];
    u8 slots[PARTY_SIZE];
    int n = 0, i, s, best = 0;
    float bestV = -2.0f;
    u32 base = Sim_AgentRandom(ag);
    if (kind == SIM_REQ_SWITCH)
    {
        int ns = Sim_LegalSwitches(sim, battler, slots, PARTY_SIZE);
        for (i = 0; i < ns; i++) { memset(&acts[n], 0, sizeof(acts[n])); acts[n].type = B_ACTION_SWITCH; acts[n].partySlot = slots[i]; n++; }
    }
    else
        n = Sim_LegalActions(sim, battler, acts, MAX_ACTIONS);
    if (n == 0) { RandomLegal(ag, sim, battler, kind, out); return; }
    for (i = 0; i < n; i++)
    {
        float v = 0.0f;
        for (s = 0; s < ag->samples; s++)
            v += Sim_SimulateAfter(ag, sim, battler, &acts[i], base + 7919u * s + 131u * i);
        v = v / ag->samples + Frand(ag) * 1e-4f;
        if (v > bestV) { bestV = v; best = i; }
    }
    *out = acts[best];
}

static void DecideExpect(struct SimAgent *ag, struct BattleSim *sim, const struct BattleSim *ts, u8 battler, u8 kind, struct SimAction *out)
{
    struct SimAction mine[MAX_ACTIONS], theirs[MAX_ACTIONS];
    static _Thread_local float M[MAX_ACTIONS * MAX_ACTIONS];
    int n, m, i, j, best = 0;
    float bestV = -2.0f;
    ag->decisions++;
    if (ag->epsilon > 0 && Frand(ag) < ag->epsilon) { RandomLegal(ag, sim, battler, kind, out); return; }
    if (!BuildMatrix(ag, sim, ts, battler, kind, mine, &n, theirs, &m, M)) { DecideBySingleEval(ag, sim, battler, kind, out); return; }
    for (i = 0; i < n; i++)
    {
        float v = 0.0f;
        for (j = 0; j < m; j++) v += M[i * m + j];
        v = v / m + Frand(ag) * 1e-4f;
        if (v > bestV) { bestV = v; best = i; }
    }
    *out = mine[best];
}

static void DecideRegret(struct SimAgent *ag, struct BattleSim *sim, const struct BattleSim *ts, u8 battler, u8 kind, struct SimAction *out)
{
    struct SimAction mine[MAX_ACTIONS], theirs[MAX_ACTIONS];
    static _Thread_local float M[MAX_ACTIONS * MAX_ACTIONS];
    float sRow[MAX_ACTIONS], sCol[MAX_ACTIONS];
    int n, m;
    ag->decisions++;
    if (ag->epsilon > 0 && Frand(ag) < ag->epsilon) { RandomLegal(ag, sim, battler, kind, out); return; }
    if (!BuildMatrix(ag, sim, ts, battler, kind, mine, &n, theirs, &m, M)) { DecideBySingleEval(ag, sim, battler, kind, out); return; }
    Sim_RegretMatching(M, n, m, ag->iterations, ag->plus, ag->alternating, ag->linearAvg, sRow, sCol);
    *out = mine[SampleWithFloor(ag, sRow, n, ag->floor)];
}

// RM+ without a precomputed matrix (external-sampling MC-CFR on the one-shot game): every iteration samples
// the opponent's action from their current strategy and simulates each of my actions against it with a fresh
// engine RNG seed (and then the same for the opponent against my updated strategy). Utilities are unbiased
// samples; the regret sums average the noise out over iterations instead of baking it into a matrix.
static void DecideRegretSampled(struct SimAgent *ag, struct BattleSim *sim, const struct BattleSim *ts, u8 battler, u8 kind, struct SimAction *out)
{
    struct SimAction mine[MAX_ACTIONS], theirs[MAX_ACTIONS];
    float R1[MAX_ACTIONS] = {0}, R2[MAX_ACTIONS] = {0}, S1[MAX_ACTIONS] = {0}, S2[MAX_ACTIONS] = {0};
    float s1[MAX_ACTIONS], s2[MAX_ACTIONS], u1[MAX_ACTIONS], u2[MAX_ACTIONS], avg[MAX_ACTIONS];
    int n, m, t, i, j;
    u8 opp;

    ag->decisions++;
    if (ag->epsilon > 0 && Frand(ag) < ag->epsilon) { RandomLegal(ag, sim, battler, kind, out); return; }
    if (ts == NULL || kind != SIM_REQ_ACTION || (ts->battleTypeFlags & BATTLE_TYPE_DOUBLE) || ts->turnCount != sim->turnCount)
    {
        DecideBySingleEval(ag, sim, battler, kind, out);
        return;
    }
    opp = battler ^ BIT_SIDE;
    n = Sim_LegalActions((struct BattleSim *)ts, battler, mine, MAX_ACTIONS);
    m = Sim_LegalActions((struct BattleSim *)ts, opp, theirs, MAX_ACTIONS);
    if (n == 0 || m == 0) { DecideBySingleEval(ag, sim, battler, kind, out); return; }
    Normalize(R1, n, s1);
    Normalize(R2, m, s2);
    for (t = 1; t <= ag->iterations; t++)
    {
        float w = ag->linearAvg ? (float)t : 1.0f, U1 = 0, U2 = 0;
        int js = SampleWithFloor(ag, s2, m, 0.0f), is;
        u32 seed = Sim_AgentRandom(ag);
        for (i = 0; i < n; i++)
        {
            u1[i] = Sim_SimulateJoint(ag, ts, battler, &mine[i], &theirs[js], seed + 7919u * i);
            U1 += s1[i] * u1[i];
        }
        for (i = 0; i < n; i++) { R1[i] += u1[i] - U1; if (ag->plus && R1[i] < 0) R1[i] = 0; }
        Normalize(R1, n, s1);
        is = SampleWithFloor(ag, s1, n, 0.0f);
        seed = Sim_AgentRandom(ag);
        for (j = 0; j < m; j++)
        {
            u2[j] = -Sim_SimulateJoint(ag, ts, battler, &mine[is], &theirs[j], seed + 7919u * j);
            U2 += s2[j] * u2[j];
        }
        for (j = 0; j < m; j++) { R2[j] += u2[j] - U2; if (ag->plus && R2[j] < 0) R2[j] = 0; }
        Normalize(R2, m, s2);
        for (i = 0; i < n; i++) S1[i] += w * s1[i];
        for (j = 0; j < m; j++) S2[j] += w * s2[j];
        ag->matrixCells += n + m;
    }
    Normalize(S1, n, avg);
    *out = mine[SampleWithFloor(ag, avg, n, ag->floor)];
}

// ---------------------------------------------------------------------------------------------------------
// Spec parsing

static const char *OptValue(const char *spec, const char *key, char *buf, int len)
{
    const char *p = strchr(spec, ':');
    size_t kl = strlen(key);
    if (!p) return NULL;
    p++;
    while (*p)
    {
        const char *e = strchr(p, ',');
        size_t seg = e ? (size_t)(e - p) : strlen(p);
        if (seg > kl && !strncmp(p, key, kl) && p[kl] == '=')
        {
            size_t vl = seg - kl - 1;
            if (vl >= (size_t)len) vl = len - 1;
            memcpy(buf, p + kl + 1, vl); buf[vl] = 0;
            return buf;
        }
        if (!e) break;
        p = e + 1;
    }
    return NULL;
}

int Sim_AgentFromSpec(struct SimAgent *ag, const char *spec)
{
    char name[64], val[64];
    const char *colon = strchr(spec, ':');
    size_t nl = colon ? (size_t)(colon - spec) : strlen(spec);

    memset(ag, 0, sizeof(*ag));
    if (nl >= sizeof(name)) nl = sizeof(name) - 1;
    memcpy(name, spec, nl); name[nl] = 0;
    snprintf(ag->spec, sizeof(ag->spec), "%s", spec);
    snprintf(ag->name, sizeof(ag->name), "%s", spec);
    ag->value = Sim_ValueBasic;
    ag->samples = 1;
    ag->rng = 0x1234567u;
    if (OptValue(spec, "vf", val, sizeof(val)))
    {
        if (!strcmp(val, "material")) ag->value = Sim_ValueMaterial;
        else if (!strcmp(val, "basic")) ag->value = Sim_ValueBasic;
        else { snprintf(ag->name, sizeof(ag->name), "unknown value function %s", val); return -1; }
    }
    if (OptValue(spec, "samples", val, sizeof(val))) ag->samples = atoi(val) > 0 ? atoi(val) : 1;
    if (OptValue(spec, "iters", val, sizeof(val))) ag->iterations = atoi(val);
    if (OptValue(spec, "eps", val, sizeof(val))) ag->epsilon = (float)atof(val);
    if (OptValue(spec, "floor", val, sizeof(val))) ag->floor = (float)atof(val);
    if (OptValue(spec, "seed", val, sizeof(val))) ag->rng = (u32)strtoul(val, NULL, 0);

    if (!strcmp(name, "game"))
    {
        ag->isGameAI = 1;
        ag->aiFlags = AI_SCRIPT_CHECK_BAD_MOVE | AI_SCRIPT_TRY_TO_FAINT | AI_SCRIPT_CHECK_VIABILITY;
        if (OptValue(spec, "flags", val, sizeof(val)))
        {
            if (!strcmp(val, "basic")) ag->aiFlags = AI_SCRIPT_CHECK_BAD_MOVE;
            else if (!strcmp(val, "smart")) ag->aiFlags = AI_SCRIPT_CHECK_BAD_MOVE | AI_SCRIPT_TRY_TO_FAINT | AI_SCRIPT_CHECK_VIABILITY;
            else ag->aiFlags = (u32)strtoul(val, NULL, 0);
        }
        if (Sim_TrainerForAIFlags(ag->aiFlags) == 0) { snprintf(ag->name, sizeof(ag->name), "no item-less trainer has AI flags %x", ag->aiFlags); return -1; }
        return 0;
    }
    if (!strcmp(name, "random")) { ag->decide = DecideRandom; return 0; }
    if (!strcmp(name, "movebias")) { ag->decide = DecideMoveBias; ag->epsilon = 0.85f; if (OptValue(spec, "moves", val, sizeof(val))) ag->epsilon = (float)atof(val); return 0; }
    if (!strcmp(name, "greedy")) { ag->decide = DecideGreedy; return 0; }
    if (!strcmp(name, "epsgreedy")) { ag->decide = DecideEpsGreedy; if (ag->epsilon == 0) ag->epsilon = 0.1f; return 0; }
    if (!strcmp(name, "expect")) { ag->decide = DecideExpect; return 0; }
    if (!strcmp(name, "epsexpect")) { ag->decide = DecideExpect; if (ag->epsilon == 0) ag->epsilon = 0.1f; return 0; }
    if (!strcmp(name, "rm"))
    {
        ag->decide = DecideRegret;
        if (ag->iterations == 0) ag->iterations = 10;
        ag->plus = 0; ag->alternating = 0; ag->linearAvg = 0;
        return 0;
    }
    if (!strcmp(name, "rmsample"))
    {
        ag->decide = DecideRegretSampled;
        if (ag->iterations == 0) ag->iterations = 100;
        ag->plus = 1; ag->alternating = 1; ag->linearAvg = 1;
        if (ag->samples == 1) ag->samples = 16; // used by the single-action fallback (mid-turn switches)
        return 0;
    }
    if (!strcmp(name, "rmplus"))
    {
        ag->decide = DecideRegret;
        if (ag->iterations == 0) ag->iterations = 10;
        ag->plus = 1; ag->alternating = 1; ag->linearAvg = 1;
        if (OptValue(spec, "alt", val, sizeof(val))) ag->alternating = atoi(val) != 0;
        if (OptValue(spec, "linavg", val, sizeof(val))) ag->linearAvg = atoi(val) != 0;
        return 0;
    }
    snprintf(ag->name, sizeof(ag->name), "unknown agent %s", name);
    return -1;
}
