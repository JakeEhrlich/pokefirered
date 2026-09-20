// Built-in agents, value functions and the one-turn joint-action machinery (see include/sim_agent.h).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
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

// Copies a battle state for a rollout, skipping the message log (unused when logging is off).
static void SimCloneState(struct BattleSim *dst, const struct BattleSim *src)
{
    memcpy(dst, src, offsetof(struct BattleSim, log));
    memcpy(&dst->frames, &src->frames, sizeof(*src) - offsetof(struct BattleSim, frames));
    dst->logEnabled = 0;
    dst->logCount = 0;
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
        u16 species = sim->trustParty ? sim->partySpeciesOrEgg[side][i] : GetMonData(&party[i], MON_DATA_SPECIES_OR_EGG);
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

// Expected damage of `move` from atk to def (accuracy-weighted, average roll, no crit, capped at def->hp);
// *rawOut gets the uncapped average-roll damage. The battler ids only feed the engine's per-battler lookups
// (badge boosts, Flash Fire, enigma berries) and may be approximate for benched mons.
int Sim_EstimateDamageMons(struct BattlePokemon *atk, struct BattlePokemon *def, u16 move, u8 atkBattler, u8 defBattler, int *rawOut)
{
    s32 dmg;
    int mult, acc;
    if (rawOut) *rawOut = 0;
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
        dmg = CalculateBaseDamage(atk, def, move, gSideStatuses[GetBattlerSide(defBattler)], 0, 0, atkBattler, defBattler);
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
    if (rawOut) *rawOut = dmg;
    if (dmg > def->hp) dmg = def->hp;
    return dmg * acc / 100;
}

int Sim_TypeMultiplier(u8 moveType, u8 defType1, u8 defType2) { return TypeMultiplier(moveType, defType1, defType2); }

int Sim_EstimateDamage(struct BattleSim *sim, u8 attacker, u8 defender, u8 moveSlot)
{
    Sim_Bind(sim);
    return Sim_EstimateDamageMons(&gBattleMons[attacker], &gBattleMons[defender], gBattleMons[attacker].moves[moveSlot & 3], attacker, defender, NULL);
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

    SimCloneState(&clone, turnStart);
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
    SimCloneState(&clone, sim);
    clone.policy[0] = clone.policy[1] = NULL;
    clone.strictAnswers = 0;
    clone.logEnabled = 0;
    clone.rngXorshift = 1;
    clone.rngValue = seed | 1;
    ag->simulatedTurns++;
    if (clone.requestKind != SIM_REQ_NONE && clone.requestBattler != me)
    {
        // someone else is being asked first (e.g. both sides need a replacement): let them answer randomly
        struct SimAction other;
        RandomLegal(ag, &clone, clone.requestBattler, clone.requestKind, &other);
        Sim_Answer(&clone, clone.requestBattler, &other);
        if (Sim_Run(&clone) != SIM_RUN_REQUEST || clone.requestBattler != me)
            return ag->value(&clone, GetBattlerSide(me), ag->valueCtx);
    }
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

// Records the exact distribution a final SampleWithFloor(sigma, n, floor) draws from (ag->policy).
static void RecordPolicy(struct SimAgent *ag, const float *sigma, int n, float floor)
{
    float s = 0.0f;
    int i;
    if (n > SIM_AGENT_MAX_ACTIONS) { ag->policyValid = 0; return; }
    for (i = 0; i < n; i++) { ag->policy[i] = sigma[i] < floor ? floor : sigma[i]; s += ag->policy[i]; }
    if (s <= 0) { ag->policyValid = 0; return; }
    for (i = 0; i < n; i++) ag->policy[i] /= s;
    ag->policyN = n;
    ag->policyValid = 1;
}

// ---------------------------------------------------------------------------------------------------------
// Agents

static void DecideRandom(struct SimAgent *ag, struct BattleSim *sim, const struct BattleSim *ts, u8 battler, u8 kind, struct SimAction *out)
{
    struct SimAction acts[MAX_ACTIONS];
    u8 slots[PARTY_SIZE];
    int n, i;
    ag->decisions++;
    RandomLegal(ag, sim, battler, kind, out);
    n = kind == SIM_REQ_SWITCH ? Sim_LegalSwitches(sim, battler, slots, PARTY_SIZE) : Sim_LegalActions(sim, battler, acts, MAX_ACTIONS);
    if (n > 0 && n <= SIM_AGENT_MAX_ACTIONS) { for (i = 0; i < n; i++) ag->policy[i] = 1.0f / n; ag->policyN = n; ag->policyValid = 1; }
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

// Equilibrium value for side 0 of the one-turn matrix from a turn-start state (battler 0's action request), using
// the agent's value function and samples; RM+ with the agent's iteration settings. Returns 0 and writes *value,
// or -1 if the state is not a turn start. *nCells gets n*m.
int Sim_SearchValue(struct SimAgent *ag, const struct BattleSim *turnStart, float *value, int *nCells)
{
    struct SimAction mine[MAX_ACTIONS], theirs[MAX_ACTIONS];
    static _Thread_local float M[MAX_ACTIONS * MAX_ACTIONS];
    float sRow[MAX_ACTIONS], sCol[MAX_ACTIONS];
    int n, m, i, j;
    float v = 0.0f;
    struct BattleSim *ts = (struct BattleSim *)turnStart;
    if (ts->requestKind != SIM_REQ_ACTION || ts->requestBattler != 0)
        return -1;
    if (!BuildMatrix(ag, ts, ts, 0, SIM_REQ_ACTION, mine, &n, theirs, &m, M))
        return -1;
    Sim_RegretMatching(M, n, m, ag->iterations, ag->plus, ag->alternating, ag->linearAvg, sRow, sCol);
    for (i = 0; i < n; i++)
        for (j = 0; j < m; j++)
            v += sRow[i] * M[i * m + j] * sCol[j];
    *value = v;
    if (nCells) *nCells = n * m;
    return 0;
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
    if (ag->epsilon <= 0) RecordPolicy(ag, sRow, n, ag->floor);
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
    if (ag->epsilon <= 0) RecordPolicy(ag, avg, n, ag->floor);
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


// ---------------------------------------------------------------------------------------------------------
// MCTS for the simultaneous-move, stochastic game.
//   Nodes are decision states: a turn start (both players choose; n x m matrix) or a forced replacement (one
//   side chooses among its switches, the other has a single no-op; n x 1 or 1 x m). Every node keeps an
//   empirical payoff matrix Q[a][b] and RM+ regrets that persist across visits (warm-started solves).
//   Selection samples both players' RM+ strategies (epsilon-uniform), optionally on an optimistic matrix
//   (bonus=c adds c*sqrt(ln(N+1)/(n_ab+1)) for the row player and subtracts it for the column player).
//   Chance: each cell stores up to `kids` outcomes (the engine seed that produced it, its leaf value, visits,
//   and its node once built). Below the cap a visit rolls a new seed, builds the outcome's node and stops
//   (expansion); at the cap it picks a stored outcome uniformly and descends. No hashing.
//   Expansion modes: current (one outcome per cell at creation), eager (`samples` per cell), lazy (none;
//   unvisited cells carry the node's own heuristic value).
//   A cell's value is the visit-weighted mean of its outcomes' values (a built outcome contributes its node's
//   equilibrium value, otherwise its leaf value). Node value = sigma0' Q sigma1. The root decision samples the
//   root strategy (a forced replacement of the agent's own is searched the same way).

#define MCTS_MAX_NODES 4096
#define MCTS_MAX_KIDS 8
#define MCTS_MAX_DEPTH 48
enum { MCTS_EXPAND_CURRENT, MCTS_EXPAND_EAGER, MCTS_EXPAND_LAZY };

struct McKid { u32 seed; float leaf; int visits, node; };

struct McCell
{
    struct McKid kid[MCTS_MAX_KIDS];
    int nKids;
};

struct McNode
{
    struct BattleSim state;
    struct SimAction mine[MAX_ACTIONS], theirs[MAX_ACTIONS];
    int n, m;
    u8 kind;                      // SIM_REQ_ACTION or SIM_REQ_SWITCH
    u8 requester;                 // switch nodes: the battler choosing
    int visits;
    float value, prior;
    u8 terminal;
    struct McCell cells[MAX_ACTIONS * MAX_ACTIONS];
    // persistent RM+ state
    float R1[MAX_ACTIONS], R2[MAX_ACTIONS], S1[MAX_ACTIONS], S2[MAX_ACTIONS], s1[MAX_ACTIONS], s2[MAX_ACTIONS];
    int rmT;
    float sRow[MAX_ACTIONS], sCol[MAX_ACTIONS];   // average strategies
};

struct McTree
{
    struct McNode *nodes;
    int count;
    int itExpanded, itDescended, depthSum, depthMax, nodesByDepth[MCTS_MAX_DEPTH];
    double tExpandSim, tDescentSim, tSolve, tCopy;
    long nExpandSim, nDescentSim, nSolve;
};

#include <time.h>
static double McNow(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static _Thread_local int sMcTiming;
static _Thread_local struct McTree sMcTree;

// Applies the joint cell (a, b) of `node` with `seed` into `out` and runs to the next decision of any kind or
// the end. Returns the leaf value for side 0.
static float McStep(struct SimAgent *ag, const struct McNode *node, int a, int b, u32 seed, struct BattleSim *out)
{
    int r;
    SimCloneState(out, &node->state);
    out->policy[0] = out->policy[1] = NULL;
    out->strictAnswers = 0;
    out->rngXorshift = 1;
    out->rngValue = seed | 1;
    ag->simulatedTurns++;
    if (node->kind == SIM_REQ_SWITCH)
    {
        const struct SimAction *act = (node->requester & BIT_SIDE) == 0 ? &node->mine[a] : &node->theirs[b];
        if (Sim_Answer(out, node->requester, act) != 0) { out->finished = 1; out->battleOutcome = B_OUTCOME_DREW; }
        else { r = Sim_Run(out); if (r == SIM_RUN_STUCK || r == SIM_RUN_ERROR) { out->finished = 1; out->battleOutcome = B_OUTCOME_DREW; } }
    }
    else
    {
        u8 first = out->requestBattler, second = first ^ BIT_SIDE;
        if (Sim_Answer(out, first, first == 0 ? &node->mine[a] : &node->theirs[b]) != 0) { out->finished = 1; out->battleOutcome = B_OUTCOME_DREW; }
        else
        {
            r = Sim_Run(out);
            if (r == SIM_RUN_REQUEST && out->requestBattler == second && out->requestKind == SIM_REQ_ACTION)
            {
                if (Sim_Answer(out, second, second == 0 ? &node->mine[a] : &node->theirs[b]) != 0) { out->finished = 1; out->battleOutcome = B_OUTCOME_DREW; }
                else { r = Sim_Run(out); if (r == SIM_RUN_STUCK || r == SIM_RUN_ERROR) { out->finished = 1; out->battleOutcome = B_OUTCOME_DREW; } }
            }
            else if (r == SIM_RUN_STUCK || r == SIM_RUN_ERROR) { out->finished = 1; out->battleOutcome = B_OUTCOME_DREW; }
        }
    }
    return ag->value(out, B_SIDE_PLAYER, ag->valueCtx);
}

static float McSimTimed(struct SimAgent *ag, const struct McNode *node, int a, int b, u32 seed, struct BattleSim *out, double *acc, long *cnt)
{
    double t0 = sMcTiming ? McNow() : 0;
    float v = McStep(ag, node, a, b, seed, out);
    if (sMcTiming) { *acc += McNow() - t0; (*cnt)++; }
    return v;
}

static int McNewNode(struct SimAgent *ag, const struct BattleSim *state)
{
    struct McTree *t = &sMcTree;
    struct McNode *nd;
    int a, b, s, idx;
    static _Thread_local struct BattleSim child;
    if (t->count >= MCTS_MAX_NODES) return -1;
    idx = t->count++;
    nd = &t->nodes[idx];
    {
        double t0 = sMcTiming ? McNow() : 0;
        SimCloneState(&nd->state, state);
        if (sMcTiming) t->tCopy += McNow() - t0;
    }
    nd->visits = 0;
    nd->rmT = 0;
    nd->prior = nd->value = ag->value(&nd->state, B_SIDE_PLAYER, ag->valueCtx);
    nd->terminal = state->finished || state->requestKind == SIM_REQ_NONE;
    nd->n = nd->m = 0;
    if (nd->terminal) return idx;
    nd->kind = state->requestKind;
    nd->requester = state->requestBattler;
    if (nd->kind == SIM_REQ_SWITCH)
    {
        u8 slots[PARTY_SIZE];
        int ns = Sim_LegalSwitches(&nd->state, nd->requester, slots, PARTY_SIZE), k;
        struct SimAction *acts = (nd->requester & BIT_SIDE) == 0 ? nd->mine : nd->theirs;
        for (k = 0; k < ns; k++) { memset(&acts[k], 0, sizeof(acts[k])); acts[k].type = B_ACTION_SWITCH; acts[k].partySlot = slots[k]; }
        if ((nd->requester & BIT_SIDE) == 0) { nd->n = ns; nd->m = 1; } else { nd->n = 1; nd->m = ns; }
    }
    else
    {
        nd->n = Sim_LegalActions(&nd->state, 0, nd->mine, MAX_ACTIONS);
        nd->m = Sim_LegalActions(&nd->state, 1, nd->theirs, MAX_ACTIONS);
    }
    if (nd->n == 0 || nd->m == 0) { nd->terminal = 1; return idx; }
    memset(nd->cells, 0, sizeof(nd->cells[0]) * nd->n * nd->m);
    memset(nd->R1, 0, sizeof(nd->R1)); memset(nd->R2, 0, sizeof(nd->R2)); memset(nd->S1, 0, sizeof(nd->S1)); memset(nd->S2, 0, sizeof(nd->S2));
    for (a = 0; a < nd->n; a++) nd->s1[a] = 1.0f / nd->n;
    for (b = 0; b < nd->m; b++) nd->s2[b] = 1.0f / nd->m;
    if (ag->mctsExpand != MCTS_EXPAND_LAZY)
    {
        int per = ag->mctsExpand == MCTS_EXPAND_EAGER ? ag->samples : 1;
        if (per > ag->mctsKids) per = ag->mctsKids;
        for (a = 0; a < nd->n; a++)
            for (b = 0; b < nd->m; b++)
            {
                struct McCell *c = &nd->cells[a * nd->m + b];
                for (s = 0; s < per; s++)
                {
                    struct McKid *kd = &c->kid[c->nKids++];
                    kd->seed = Sim_AgentRandom(ag);
                    kd->leaf = McSimTimed(ag, nd, a, b, kd->seed, &child, &t->tExpandSim, &t->nExpandSim);
                    kd->visits = 1; kd->node = -1;
                    ag->matrixCells++;
                }
            }
    }
    return idx;
}

static float McCellValue(const struct McTree *t, const struct McNode *nd, const struct McCell *c, int *visits)
{
    float sum = 0.0f;
    int n = 0, k;
    for (k = 0; k < c->nKids; k++)
    {
        const struct McKid *kd = &c->kid[k];
        sum += (kd->node >= 0 ? t->nodes[kd->node].value : kd->leaf) * kd->visits;
        n += kd->visits;
    }
    *visits = n;
    return n ? sum / n : nd->prior;
}

// Runs `iters` more RM+ iterations (alternating, linear averaging) on the node's persistent regrets, using the
// optimistic matrix Mrow for the row player and Mcol for the column player, and refreshes the average strategies.
static void McRegretIterate(struct McNode *nd, const float *Mrow, const float *Mcol, int iters)
{
    int n = nd->n, m = nd->m, t, i, j;
    float u1[MAX_ACTIONS], u2[MAX_ACTIONS];
    for (t = 0; t < iters; t++)
    {
        float w = (float)(++nd->rmT), U1 = 0, U2 = 0;
        for (i = 0; i < n; i++)
        {
            u1[i] = 0;
            for (j = 0; j < m; j++) u1[i] += nd->s2[j] * Mrow[i * m + j];
            U1 += nd->s1[i] * u1[i];
        }
        for (i = 0; i < n; i++) { nd->R1[i] += u1[i] - U1; if (nd->R1[i] < 0) nd->R1[i] = 0; }
        Normalize(nd->R1, n, nd->s1);
        for (j = 0; j < m; j++)
        {
            u2[j] = 0;
            for (i = 0; i < n; i++) u2[j] += nd->s1[i] * -Mcol[i * m + j];
            U2 += nd->s2[j] * u2[j];
        }
        for (j = 0; j < m; j++) { nd->R2[j] += u2[j] - U2; if (nd->R2[j] < 0) nd->R2[j] = 0; }
        Normalize(nd->R2, m, nd->s2);
        for (i = 0; i < n; i++) nd->S1[i] += w * nd->s1[i];
        for (j = 0; j < m; j++) nd->S2[j] += w * nd->s2[j];
    }
    Normalize(nd->S1, n, nd->sRow);
    Normalize(nd->S2, m, nd->sCol);
}

static void McSolve(struct SimAgent *ag, struct McNode *nd)
{
    static _Thread_local float M[MAX_ACTIONS * MAX_ACTIONS], Mr[MAX_ACTIONS * MAX_ACTIONS], Mc[MAX_ACTIONS * MAX_ACTIONS];
    int a, b;
    float v = 0.0f, logN = logf((float)nd->visits + 1.0f);
    double t0 = sMcTiming ? McNow() : 0;
    sMcTree.nSolve++;
    for (a = 0; a < nd->n; a++)
        for (b = 0; b < nd->m; b++)
        {
            int vis;
            float q = McCellValue(&sMcTree, nd, &nd->cells[a * nd->m + b], &vis), bonus = 0.0f;
            if (ag->mctsBonus > 0) bonus = ag->mctsBonus * sqrtf(logN / (vis + 1.0f));
            M[a * nd->m + b] = q;
            Mr[a * nd->m + b] = q + bonus;
            Mc[a * nd->m + b] = q - bonus;
        }
    McRegretIterate(nd, Mr, Mc, ag->iterations);
    for (a = 0; a < nd->n; a++)
        for (b = 0; b < nd->m; b++)
            v += nd->sRow[a] * M[a * nd->m + b] * nd->sCol[b];
    nd->value = v;
    if (sMcTiming) sMcTree.tSolve += McNow() - t0;
}

static int McSample(struct SimAgent *ag, const float *sigma, int n, float eps)
{
    float r = Frand(ag), acc = 0.0f;
    int i;
    if (n == 1) return 0;
    if (eps > 0 && Frand(ag) < eps) return (int)(Sim_AgentRandom(ag) % n);
    for (i = 0; i < n; i++) { acc += sigma[i]; if (r < acc) return i; }
    return n - 1;
}

// One iteration: descend from the root, expand one node, refresh the values on the path.
static void McIterate(struct SimAgent *ag)
{
    struct McTree *t = &sMcTree;
    int path[MCTS_MAX_DEPTH], depth = 0, idx = 0, k;
    static _Thread_local struct BattleSim child;
    for (;;)
    {
        struct McNode *nd = &t->nodes[idx];
        struct McCell *c;
        struct McKid *kd;
        int a, b, ni;
        if (nd->terminal || depth >= MCTS_MAX_DEPTH - 1) break;
        if (nd->visits == 0) McSolve(ag, nd);
        a = McSample(ag, nd->sRow, nd->n, ag->epsilon);
        b = McSample(ag, nd->sCol, nd->m, ag->epsilon);
        path[depth++] = idx;
        c = &nd->cells[a * nd->m + b];
        if (c->nKids < ag->mctsKids)
        {
            kd = &c->kid[c->nKids++];                                   // a new outcome: roll the dice, build its node, stop
            kd->seed = Sim_AgentRandom(ag);
            kd->leaf = McSimTimed(ag, nd, a, b, kd->seed, &child, &t->tExpandSim, &t->nExpandSim);
            kd->visits = 1; kd->node = -1;
        }
        else
        {
            kd = &c->kid[Sim_AgentRandom(ag) % (u32)c->nKids];         // at the cap: one of the stored outcomes, uniformly
            kd->visits++;
            if (kd->node >= 0) { idx = kd->node; t->itDescended++; continue; }
            McSimTimed(ag, nd, a, b, kd->seed, &child, &t->tDescentSim, &t->nDescentSim);   // rebuild its state
        }
        ni = McNewNode(ag, &child);
        if (ni < 0) break;
        kd->node = ni;
        if (!t->nodes[ni].terminal) McSolve(ag, &t->nodes[ni]);
        t->itExpanded++;
        if (depth < MCTS_MAX_DEPTH) t->nodesByDepth[depth]++;
        break;
    }
    t->depthSum += depth;
    if (depth > t->depthMax) t->depthMax = depth;
    for (k = depth - 1; k >= 0; k--)
    {
        struct McNode *nd = &t->nodes[path[k]];
        nd->visits++;
        McSolve(ag, nd);
    }
}

static void DecideMcts(struct SimAgent *ag, struct BattleSim *sim, const struct BattleSim *ts, u8 battler, u8 kind, struct SimAction *out)
{
    struct McTree *t = &sMcTree;
    struct McNode *root;
    const struct BattleSim *rootState;
    int it, side = battler & BIT_SIDE, maxIt = ag->mctsNodes > 0 ? ag->mctsNodes * 20 : ag->mctsIters;
    ag->decisions++;
    if (kind == SIM_REQ_ACTION && (ts == NULL || (ts->battleTypeFlags & BATTLE_TYPE_DOUBLE) || ts->turnCount != sim->turnCount))
    { DecideBySingleEval(ag, sim, battler, kind, out); return; }
    rootState = kind == SIM_REQ_ACTION ? ts : sim;    // a forced replacement is searched from the current state
    if (t->nodes == NULL) t->nodes = malloc(sizeof(struct McNode) * MCTS_MAX_NODES);
    t->count = 0;
    t->itExpanded = t->itDescended = t->depthSum = t->depthMax = 0;
    memset(t->nodesByDepth, 0, sizeof(t->nodesByDepth));
    sMcTiming = getenv("SIM_MCTS_TRACE") != NULL;
    t->tExpandSim = t->tDescentSim = t->tSolve = t->tCopy = 0; t->nExpandSim = t->nDescentSim = t->nSolve = 0;
    if (McNewNode(ag, rootState) < 0 || t->nodes[0].terminal) { RandomLegal(ag, sim, battler, kind, out); return; }
    root = &t->nodes[0];
    McSolve(ag, root);
    for (it = 0; it < maxIt; it++)
    {
        if (ag->mctsNodes > 0 && t->count >= ag->mctsNodes) break;
        McIterate(ag);
        if (t->count >= MCTS_MAX_NODES) break;
    }
    McSolve(ag, root);
    if (sMcTiming)
    {
        int d;
        double tot = t->tExpandSim + t->tDescentSim + t->tSolve + t->tCopy;
        fprintf(stderr, "mcts turn %u (%s): %d iters, nodes %d, descended %d, mean path depth %.2f, max %d, nodes by depth:",
                sim->turnCount, kind == SIM_REQ_ACTION ? "turn" : "switch", it, t->count, t->itDescended, it ? (double)t->depthSum / it : 0.0, t->depthMax);
        for (d = 0; d < 8; d++) fprintf(stderr, " %d", t->nodesByDepth[d]);
        fprintf(stderr, "  root %dx%d value %.3f\n", root->n, root->m, root->value);
        fprintf(stderr, "  time %.1f ms: expansion sims %.0f%% (%ld, %.1f us each), descent sims %.0f%% (%ld), RM+ solves %.0f%% (%ld, %.1f us each), node copies %.0f%%\n",
                tot * 1e3, 100 * t->tExpandSim / tot, t->nExpandSim, t->nExpandSim ? 1e6 * t->tExpandSim / t->nExpandSim : 0,
                100 * t->tDescentSim / tot, t->nDescentSim, 100 * t->tSolve / tot, t->nSolve, t->nSolve ? 1e6 * t->tSolve / t->nSolve : 0, 100 * t->tCopy / tot);
    }
    if (root->kind == SIM_REQ_SWITCH)
    {
        if ((root->requester & BIT_SIDE) == 0) { RecordPolicy(ag, root->sRow, root->n, ag->floor); *out = root->mine[SampleWithFloor(ag, root->sRow, root->n, ag->floor)]; }
        else { RecordPolicy(ag, root->sCol, root->m, ag->floor); *out = root->theirs[SampleWithFloor(ag, root->sCol, root->m, ag->floor)]; }
        return;
    }
    if (side == 0) { RecordPolicy(ag, root->sRow, root->n, ag->floor); *out = root->mine[SampleWithFloor(ag, root->sRow, root->n, ag->floor)]; }
    else { RecordPolicy(ag, root->sCol, root->m, ag->floor); *out = root->theirs[SampleWithFloor(ag, root->sCol, root->m, ag->floor)]; }
}


// ---------------------------------------------------------------------------------------------------------
// MCTS on the fast engine (mctsf): the same search as mcts (RM+ at every node, seed-stored outcomes per cell,
// heuristic leaves) with fs_state nodes stepped by fs_step. Positions the fast engine cannot represent
// (fs_import fails: an unsupported move effect / ability / item / volatile anywhere in either party) fall back
// to the verbatim mcts for that decision.

#include "fast.h"
#define MF_MAX_ACTIONS 10          // singles: 4 moves (or Struggle) + 5 switches
#define MF_MAX_NODES 4096

struct MfNode
{
    fs_state state;
    fs_action mine[MF_MAX_ACTIONS], theirs[MF_MAX_ACTIONS];
    int n, m;
    u8 kind;                      // FS_REQ_TURN or FS_REQ_SWITCH
    u8 requester;                 // switch nodes: the side choosing
    int visits;
    float value, prior;
    u8 terminal;
    struct McCell cells[MF_MAX_ACTIONS * MF_MAX_ACTIONS];
    float R1[MF_MAX_ACTIONS], R2[MF_MAX_ACTIONS], S1[MF_MAX_ACTIONS], S2[MF_MAX_ACTIONS], s1[MF_MAX_ACTIONS], s2[MF_MAX_ACTIONS];
    int rmT;
    float sRow[MF_MAX_ACTIONS], sCol[MF_MAX_ACTIONS];
};

struct MfTree
{
    struct MfNode *nodes;
    int count;
    int itExpanded, itDescended, depthSum, depthMax;
    long nSims, nUnsupported, nSolve;
    u32 fallbacks;
    double tSim, tSolve;
};
static _Thread_local struct MfTree sMfTree;

static float MfStep(struct SimAgent *ag, const struct MfNode *node, int a, int b, u32 seed, fs_state *out)
{
    float v;
    double t0 = sMcTiming ? McNow() : 0;
    *out = node->state;
    fs_seed(out, seed | 1);
    ag->simulatedTurns++;
    sMfTree.nSims++;
    if (node->kind == FS_REQ_SWITCH)
    {
        fs_action none = {FS_ACT_MOVE, 0};
        if (node->requester == 0) fs_step(out, node->mine[a], none);
        else fs_step(out, none, node->theirs[b]);
    }
    else
        fs_step(out, node->mine[a], node->theirs[b]);
    if (out->unsupported) sMfTree.nUnsupported++;
    v = fs_value_basic(out, 0);
    if (sMcTiming) sMfTree.tSim += McNow() - t0;
    return v;
}

static int MfNewNode(struct SimAgent *ag, const fs_state *state)
{
    struct MfTree *t = &sMfTree;
    struct MfNode *nd;
    int a, b, s, idx;
    static _Thread_local fs_state child;
    if (t->count >= MF_MAX_NODES) return -1;
    idx = t->count++;
    nd = &t->nodes[idx];
    nd->state = *state;
    nd->visits = 0;
    nd->rmT = 0;
    nd->prior = nd->value = fs_value_basic(&nd->state, 0);
    nd->terminal = state->request == FS_REQ_DONE;
    nd->n = nd->m = 0;
    if (nd->terminal) return idx;
    nd->kind = state->request;
    if (nd->kind == FS_REQ_SWITCH)
    {
        nd->requester = (state->switchMask & 1) ? 0 : 1;   // fs_step replaces the player side first
        if (nd->requester == 0) { nd->n = fs_legal_actions(state, 0, nd->mine); nd->m = 1; }
        else { nd->n = 1; nd->m = fs_legal_actions(state, 1, nd->theirs); }
    }
    else
    {
        nd->n = fs_legal_actions(state, 0, nd->mine);
        nd->m = fs_legal_actions(state, 1, nd->theirs);
    }
    if (nd->n > MF_MAX_ACTIONS) nd->n = MF_MAX_ACTIONS;
    if (nd->m > MF_MAX_ACTIONS) nd->m = MF_MAX_ACTIONS;
    if (nd->n == 0 || nd->m == 0) { nd->terminal = 1; return idx; }
    memset(nd->cells, 0, sizeof(nd->cells[0]) * nd->n * nd->m);
    memset(nd->R1, 0, sizeof(nd->R1)); memset(nd->R2, 0, sizeof(nd->R2)); memset(nd->S1, 0, sizeof(nd->S1)); memset(nd->S2, 0, sizeof(nd->S2));
    for (a = 0; a < nd->n; a++) nd->s1[a] = 1.0f / nd->n;
    for (b = 0; b < nd->m; b++) nd->s2[b] = 1.0f / nd->m;
    if (ag->mctsExpand != MCTS_EXPAND_LAZY)
    {
        int per = ag->mctsExpand == MCTS_EXPAND_EAGER ? ag->samples : 1;
        if (per > ag->mctsKids) per = ag->mctsKids;
        for (a = 0; a < nd->n; a++)
            for (b = 0; b < nd->m; b++)
            {
                struct McCell *c = &nd->cells[a * nd->m + b];
                for (s = 0; s < per; s++)
                {
                    struct McKid *kd = &c->kid[c->nKids++];
                    kd->seed = Sim_AgentRandom(ag);
                    kd->leaf = MfStep(ag, nd, a, b, kd->seed, &child);
                    kd->visits = 1; kd->node = -1;
                    ag->matrixCells++;
                }
            }
    }
    return idx;
}

static float MfCellValue(const struct MfTree *t, const struct MfNode *nd, const struct McCell *c, int *visits)
{
    float sum = 0.0f;
    int n = 0, k;
    for (k = 0; k < c->nKids; k++)
    {
        const struct McKid *kd = &c->kid[k];
        sum += (kd->node >= 0 ? t->nodes[kd->node].value : kd->leaf) * kd->visits;
        n += kd->visits;
    }
    *visits = n;
    return n ? sum / n : nd->prior;
}

static void MfRegretIterate(struct MfNode *nd, const float *Mrow, const float *Mcol, int iters)
{
    int n = nd->n, m = nd->m, t, i, j;
    float u1[MF_MAX_ACTIONS], u2[MF_MAX_ACTIONS];
    for (t = 0; t < iters; t++)
    {
        float w = (float)(++nd->rmT), U1 = 0, U2 = 0;
        for (i = 0; i < n; i++)
        {
            u1[i] = 0;
            for (j = 0; j < m; j++) u1[i] += nd->s2[j] * Mrow[i * m + j];
            U1 += nd->s1[i] * u1[i];
        }
        for (i = 0; i < n; i++) { nd->R1[i] += u1[i] - U1; if (nd->R1[i] < 0) nd->R1[i] = 0; }
        Normalize(nd->R1, n, nd->s1);
        for (j = 0; j < m; j++)
        {
            u2[j] = 0;
            for (i = 0; i < n; i++) u2[j] += nd->s1[i] * -Mcol[i * m + j];
            U2 += nd->s2[j] * u2[j];
        }
        for (j = 0; j < m; j++) { nd->R2[j] += u2[j] - U2; if (nd->R2[j] < 0) nd->R2[j] = 0; }
        Normalize(nd->R2, m, nd->s2);
        for (i = 0; i < n; i++) nd->S1[i] += w * nd->s1[i];
        for (j = 0; j < m; j++) nd->S2[j] += w * nd->s2[j];
    }
    Normalize(nd->S1, n, nd->sRow);
    Normalize(nd->S2, m, nd->sCol);
}

static void MfSolve(struct SimAgent *ag, struct MfNode *nd)
{
    float M[MF_MAX_ACTIONS * MF_MAX_ACTIONS], Mr[MF_MAX_ACTIONS * MF_MAX_ACTIONS], Mc[MF_MAX_ACTIONS * MF_MAX_ACTIONS];
    int a, b;
    float v = 0.0f, logN = logf((float)nd->visits + 1.0f);
    double t0 = sMcTiming ? McNow() : 0;
    sMfTree.nSolve++;
    for (a = 0; a < nd->n; a++)
        for (b = 0; b < nd->m; b++)
        {
            int vis;
            float q = MfCellValue(&sMfTree, nd, &nd->cells[a * nd->m + b], &vis), bonus = 0.0f;
            if (ag->mctsBonus > 0) bonus = ag->mctsBonus * sqrtf(logN / (vis + 1.0f));
            M[a * nd->m + b] = q;
            Mr[a * nd->m + b] = q + bonus;
            Mc[a * nd->m + b] = q - bonus;
        }
    MfRegretIterate(nd, Mr, Mc, ag->iterations);
    for (a = 0; a < nd->n; a++)
        for (b = 0; b < nd->m; b++)
            v += nd->sRow[a] * M[a * nd->m + b] * nd->sCol[b];
    nd->value = v;
    if (sMcTiming) sMfTree.tSolve += McNow() - t0;
}

static void MfIterate(struct SimAgent *ag)
{
    struct MfTree *t = &sMfTree;
    int path[MCTS_MAX_DEPTH], depth = 0, idx = 0, k;
    static _Thread_local fs_state child;
    for (;;)
    {
        struct MfNode *nd = &t->nodes[idx];
        struct McCell *c;
        struct McKid *kd;
        int a, b, ni;
        if (nd->terminal || depth >= MCTS_MAX_DEPTH - 1) break;
        if (nd->visits == 0) MfSolve(ag, nd);
        a = McSample(ag, nd->sRow, nd->n, ag->epsilon);
        b = McSample(ag, nd->sCol, nd->m, ag->epsilon);
        path[depth++] = idx;
        c = &nd->cells[a * nd->m + b];
        if (c->nKids < ag->mctsKids)
        {
            kd = &c->kid[c->nKids++];
            kd->seed = Sim_AgentRandom(ag);
            kd->leaf = MfStep(ag, nd, a, b, kd->seed, &child);
            kd->visits = 1; kd->node = -1;
        }
        else
        {
            kd = &c->kid[Sim_AgentRandom(ag) % (u32)c->nKids];
            kd->visits++;
            if (kd->node >= 0) { idx = kd->node; t->itDescended++; continue; }
            MfStep(ag, nd, a, b, kd->seed, &child);
        }
        ni = MfNewNode(ag, &child);
        if (ni < 0) break;
        kd->node = ni;
        if (!t->nodes[ni].terminal) MfSolve(ag, &t->nodes[ni]);
        t->itExpanded++;
        break;
    }
    t->depthSum += depth;
    if (depth > t->depthMax) t->depthMax = depth;
    for (k = depth - 1; k >= 0; k--)
    {
        struct MfNode *nd = &t->nodes[path[k]];
        nd->visits++;
        MfSolve(ag, nd);
    }
}

// Maps the chosen fast action back to the verbatim action list of the root (both enumerate moves in slot order
// then switches in party order, so the lists line up index by index when their sizes agree).
static void DecideMctsFast(struct SimAgent *ag, struct BattleSim *sim, const struct BattleSim *ts, u8 battler, u8 kind, struct SimAction *out)
{
    struct MfTree *t = &sMfTree;
    struct MfNode *root;
    const struct BattleSim *rootState;
    fs_state fs;
    struct SimAction legal[MAX_ACTIONS];
    int it, side = battler & BIT_SIDE, maxIt = ag->mctsNodes > 0 ? ag->mctsNodes * 20 : ag->mctsIters, nLegal, pick;
    const float *sigma;
    if (kind == SIM_REQ_ACTION && (ts == NULL || (ts->battleTypeFlags & BATTLE_TYPE_DOUBLE) || ts->turnCount != sim->turnCount))
    { DecideMcts(ag, sim, ts, battler, kind, out); return; }
    rootState = kind == SIM_REQ_ACTION ? ts : sim;
    if (fs_import(&fs, rootState) != 0) { t->fallbacks++; if (getenv("SIM_MCTS_TRACE")) fprintf(stderr, "mctsf fallback: import unsupported mask %x\n", fs.unsupported); DecideMcts(ag, sim, ts, battler, kind, out); return; }
    // the verbatim legal list of the deciding battler, for the action mapping
    if (kind == SIM_REQ_SWITCH)
    {
        u8 slots[PARTY_SIZE];
        int ns = Sim_LegalSwitches(sim, battler, slots, PARTY_SIZE), k;
        for (k = 0; k < ns; k++) { memset(&legal[k], 0, sizeof(legal[k])); legal[k].type = B_ACTION_SWITCH; legal[k].partySlot = slots[k]; }
        nLegal = ns;
    }
    else
        nLegal = Sim_LegalActions((struct BattleSim *)ts, battler, legal, MAX_ACTIONS);
    {
        fs_action fl[MF_MAX_ACTIONS + 8];
        int nf = fs_legal_actions(&fs, side, fl);
        if (nf != nLegal || nf > MF_MAX_ACTIONS || nLegal == 0) { t->fallbacks++; if (getenv("SIM_MCTS_TRACE")) fprintf(stderr, "mctsf fallback: legal actions fast %d verbatim %d\n", nf, nLegal); DecideMcts(ag, sim, ts, battler, kind, out); return; }
    }
    ag->decisions++;
    if (t->nodes == NULL) t->nodes = malloc(sizeof(struct MfNode) * MF_MAX_NODES);
    t->count = 0;
    t->itExpanded = t->itDescended = t->depthSum = t->depthMax = 0;
    t->nSims = t->nUnsupported = t->nSolve = 0; t->tSim = t->tSolve = 0;
    sMcTiming = getenv("SIM_MCTS_TRACE") != NULL;
    if (MfNewNode(ag, &fs) < 0 || t->nodes[0].terminal) { RandomLegal(ag, sim, battler, kind, out); return; }
    root = &t->nodes[0];
    MfSolve(ag, root);
    for (it = 0; it < maxIt; it++)
    {
        if (ag->mctsNodes > 0 && t->count >= ag->mctsNodes) break;
        MfIterate(ag);
        if (t->count >= MF_MAX_NODES) break;
    }
    MfSolve(ag, root);
    if (sMcTiming)
        fprintf(stderr, "mctsf turn %u (%s): %d iters, nodes %d, descended %d, mean depth %.2f, max %d, sims %ld (unsupported steps %ld), root %dx%d value %.3f; sims %.1f ms (%.2f us each), RM+ solves %.1f ms (%ld, %.1f us each), fallbacks so far %u\n",
                sim->turnCount, kind == SIM_REQ_ACTION ? "turn" : "switch", it, t->count, t->itDescended, it ? (double)t->depthSum / it : 0.0, t->depthMax,
                t->nSims, t->nUnsupported, root->n, root->m, root->value, t->tSim * 1e3, t->nSims ? 1e6 * t->tSim / t->nSims : 0, t->tSolve * 1e3, t->nSolve, t->nSolve ? 1e6 * t->tSolve / t->nSolve : 0, t->fallbacks);
    if (root->kind == FS_REQ_SWITCH) sigma = root->requester == 0 ? root->sRow : root->sCol;
    else sigma = side == 0 ? root->sRow : root->sCol;
    RecordPolicy(ag, sigma, nLegal, ag->floor);
    pick = SampleWithFloor(ag, sigma, nLegal, ag->floor);
    *out = legal[pick];
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
    if (!strcmp(name, "mcts") || !strcmp(name, "mctsf"))
    {
        // mcts:nodes=<budget in tree nodes> (or iters=<tree iterations>), expand=current|eager|lazy, samples=<eager: outcomes per cell>,
        //      kids=<outcomes stored per cell (4)>, rm=<RM+ iterations per solve (30)>, eps=<uniform exploration (0.1)>, bonus=<optimism c (0)>
        // mctsf: the same search on the fast engine (fast/), falling back to mcts on unsupported positions
        ag->decide = !strcmp(name, "mctsf") ? DecideMctsFast : DecideMcts;
        ag->mctsIters = ag->iterations > 0 ? ag->iterations : 400;
        ag->mctsNodes = 0;
        if (OptValue(spec, "nodes", val, sizeof(val))) ag->mctsNodes = atoi(val);
        ag->iterations = 30;
        if (OptValue(spec, "rm", val, sizeof(val))) ag->iterations = atoi(val) > 0 ? atoi(val) : 30;
        if (ag->epsilon == 0 && !OptValue(spec, "eps", val, sizeof(val))) ag->epsilon = 0.1f;
        ag->mctsExpand = MCTS_EXPAND_CURRENT;
        if (OptValue(spec, "expand", val, sizeof(val)))
            ag->mctsExpand = !strcmp(val, "eager") ? MCTS_EXPAND_EAGER : !strcmp(val, "lazy") ? MCTS_EXPAND_LAZY : MCTS_EXPAND_CURRENT;
        if (ag->mctsExpand == MCTS_EXPAND_EAGER && ag->samples < 1) ag->samples = 4;
        ag->mctsKids = 4;
        if (OptValue(spec, "kids", val, sizeof(val))) { ag->mctsKids = atoi(val); if (ag->mctsKids < 1) ag->mctsKids = 1; if (ag->mctsKids > MCTS_MAX_KIDS) ag->mctsKids = MCTS_MAX_KIDS; }
        ag->mctsBonus = 0.0f;
        if (OptValue(spec, "bonus", val, sizeof(val))) ag->mctsBonus = (float)atof(val);
        ag->plus = 1; ag->alternating = 1; ag->linearAvg = 1;
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
