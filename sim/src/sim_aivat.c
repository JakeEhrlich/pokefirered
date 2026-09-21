// AIVAT-style scoring referee: see include/sim_aivat.h.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "global.h"
#include "battle.h"
#include "pokemon.h"
#include "sim.h"
#include "sim_agent.h"
#include "sim_aivat.h"
#include "fast.h"

#define AV_MAX 32

#include <math.h>
static float Calib(const struct SimAivat *av, float v)
{
    if (av->calibK <= 0 || av->calibK == 1.0f) return v;
    if (v >= 0.999f) return 1.0f;
    if (v <= -0.999f) return -1.0f;
    return tanhf(av->calibK * atanhf(v));
}

static u32 AvRand(struct SimAivat *av) { u32 x = av->rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; av->rng = x; return x; }

// The baseline on a fast state. vf 1: the one-turn lookahead value of the (calibrated) heuristic, i.e. the RM+
// value of the joint matrix of K1-sample means; a stochastic baseline is fine (every use is an independent draw).
static float ValueFS(struct SimAivat *av, const fs_state *s)
{
    fs_action fl[2][AV_MAX];
    float M[AV_MAX * AV_MAX], sRow[AV_MAX], sCol[AV_MAX];
    int n[2], a, b, k, side;
    double v = 0.0;
    if (av->vf == 0 || s->request == FS_REQ_DONE) return Calib(av, fs_value_basic(s, 0));
    for (side = 0; side < 2; side++)
    {
        n[side] = fs_legal_actions(s, side, fl[side]);
        if (n[side] <= 0) { n[side] = 1; fl[side][0].type = FS_ACT_MOVE; fl[side][0].slot = 0; }
        if (n[side] > AV_MAX) n[side] = AV_MAX;
    }
    for (a = 0; a < n[0]; a++)
        for (b = 0; b < n[1]; b++)
        {
            double sum = 0.0;
            for (k = 0; k < av->K1; k++)
            {
                fs_state w = *s;
                fs_seed(&w, AvRand(av) | 1);
                fs_step(&w, fl[0][a], fl[1][b]);
                sum += Calib(av, fs_value_basic(&w, 0));
            }
            M[a * n[1] + b] = (float)(sum / av->K1);
            av->sims += av->K1;
        }
    Sim_RegretMatching(M, n[0], n[1], 30, 1, 1, 1, sRow, sCol);
    for (a = 0; a < n[0]; a++) for (b = 0; b < n[1]; b++) v += sRow[a] * M[a * n[1] + b] * sCol[b];
    return (float)v;
}

void Sim_AivatBegin(struct SimAivat *av, u32 seed, int K)
{
    memset(av, 0, sizeof(*av));
    av->K = K > 0 ? K : 8;
    av->rng = seed * 2654435761u ^ 0xA5A5A5A5u;
    if (av->rng == 0) av->rng = 1;
}

// One verbatim sample of the joint cell: answers the requested battler(s) and runs to the next decision point.
static float VerbatimSample(const struct BattleSim *state, const struct SimAction *a0, const struct SimAction *a1, u8 kind, u8 requester, u32 seed, struct BattleSim *work)
{
    int r;
    memcpy(work, state, sizeof(*work));
    work->policy[0] = work->policy[1] = NULL;
    work->strictAnswers = 0;
    work->logEnabled = 0;
    work->rngXorshift = 1;
    work->rngValue = seed | 1;
    if (kind == SIM_REQ_SWITCH)
    {
        const struct SimAction *act = (requester & BIT_SIDE) == 0 ? a0 : a1;
        if (Sim_Answer(work, requester, act) != 0) { work->finished = 1; work->battleOutcome = B_OUTCOME_DREW; }
        else { r = Sim_Run(work); if (r == SIM_RUN_STUCK || r == SIM_RUN_ERROR) { work->finished = 1; work->battleOutcome = B_OUTCOME_DREW; } }
    }
    else
    {
        u8 first = work->requestBattler, second = first ^ BIT_SIDE;
        if (Sim_Answer(work, first, first == 0 ? a0 : a1) != 0) { work->finished = 1; work->battleOutcome = B_OUTCOME_DREW; }
        else
        {
            r = Sim_Run(work);
            if (r == SIM_RUN_REQUEST && work->requestBattler == second && work->requestKind == SIM_REQ_ACTION)
            {
                if (Sim_Answer(work, second, second == 0 ? a0 : a1) != 0) { work->finished = 1; work->battleOutcome = B_OUTCOME_DREW; }
                else { r = Sim_Run(work); if (r == SIM_RUN_STUCK || r == SIM_RUN_ERROR) { work->finished = 1; work->battleOutcome = B_OUTCOME_DREW; } }
            }
            else if (r == SIM_RUN_STUCK || r == SIM_RUN_ERROR) { work->finished = 1; work->battleOutcome = B_OUTCOME_DREW; }
        }
    }
    return Sim_ValueBasic(work, B_SIDE_PLAYER, NULL);
}

static int FindAction(const struct SimAction *list, int n, const struct SimAction *a)
{
    int i;
    for (i = 0; i < n; i++)
    {
        if (list[i].type != a->type) continue;
        if (a->type == B_ACTION_SWITCH ? list[i].partySlot == a->partySlot : list[i].moveSlot == a->moveSlot) return i;
    }
    return -1;
}

void Sim_AivatNode(struct SimAivat *av, const struct SimAivatNode *nd, const struct BattleSim *next)
{
    struct SimAction legal[2][AV_MAX];
    fs_action flegal[2][AV_MAX];
    int n[2] = {0, 0}, idx[2] = {-1, -1}, useFast = 0, side, a, b, k, K = av->K;
    static _Thread_local struct BattleSim work;
    static _Thread_local float M[AV_MAX * AV_MAX];
    fs_state fs, fnext, wf;
    float vNext;
    double term, rowAvg[AV_MAX], W = 0.0;
    const float *pol[2];

    av->nodes++;
    Sim_Bind((struct BattleSim *)nd->state);   // Sim_LegalSwitches reads the bound globals
    // legal lists (canonical order) and the chosen indices
    if (nd->kind == SIM_REQ_SWITCH)
    {
        u8 slots[PARTY_SIZE];
        int ns = Sim_LegalSwitches(nd->state, nd->requester, slots, PARTY_SIZE), r = nd->requester & BIT_SIDE;
        for (k = 0; k < ns; k++) { memset(&legal[r][k], 0, sizeof(legal[r][k])); legal[r][k].type = B_ACTION_SWITCH; legal[r][k].partySlot = slots[k]; }
        n[r] = ns;
        n[r ^ 1] = 1; memset(&legal[r ^ 1][0], 0, sizeof(legal[0][0])); idx[r ^ 1] = 0;
        if (!nd->haveAct[r]) { av->nodesSkipped++; if (getenv("AIVAT_DEBUG")) fprintf(stderr, "skip: switch node without an action\n"); return; }
        idx[r] = FindAction(legal[r], ns, &nd->act[r]);
    }
    else
    {
        for (side = 0; side < 2; side++)
        {
            n[side] = Sim_LegalActions(nd->state, side, legal[side], AV_MAX);
            if (n[side] == 1) idx[side] = 0;                       // a forced action (recharge etc.), asked or not
            else if (nd->haveAct[side]) idx[side] = FindAction(legal[side], n[side], &nd->act[side]);
        }
    }
    if (n[0] <= 0 || n[1] <= 0 || idx[0] < 0 || idx[1] < 0 || n[0] > AV_MAX || n[1] > AV_MAX)
    {
        av->nodesSkipped++;
        if (getenv("AIVAT_DEBUG")) fprintf(stderr, "skip: kind %d n %d/%d idx %d/%d haveAct %d/%d turn %u\n", nd->kind, n[0], n[1], idx[0], idx[1], nd->haveAct[0], nd->haveAct[1], nd->state->turnCount);
        return;
    }

    // engine choice: the fast engine when both the node and the next state import and the legal lists line up
    if (!av->verbatimOnly && fs_import(&fs, nd->state) == 0 && fs_import(&fnext, next) == 0)
    {
        int ok = 1;
        for (side = 0; side < 2 && ok; side++)
        {
            int m = fs_legal_actions(&fs, side, flegal[side]);
            if (nd->kind == SIM_REQ_SWITCH && side != (nd->requester & BIT_SIDE)) { m = 1; flegal[side][0].type = FS_ACT_MOVE; flegal[side][0].slot = 0; }
            if (m != n[side]) ok = 0;
        }
        if (ok) useFast = 1;
    }
    if (useFast) { av->nodesFast++; vNext = fs_value_basic(&fnext, 0); if (av->nV < 512) av->vTrace[av->nV++] = vNext; vNext = ValueFS(av, &fnext); }
    else
    {
        memcpy(&work, next, sizeof(work));
        vNext = Sim_ValueBasic(&work, B_SIDE_PLAYER, NULL);
        if (av->nV < 512) av->vTrace[av->nV++] = vNext;
        vNext = Calib(av, vNext);
    }

    // the joint matrix of K-sample means
    for (a = 0; a < n[0]; a++)
        for (b = 0; b < n[1]; b++)
        {
            double sum = 0.0;
            for (k = 0; k < K; k++)
            {
                u32 seed = AvRand(av) | 1;
                if (useFast)
                {
                    wf = fs;
                    fs_seed(&wf, seed);
                    fs_step(&wf, flegal[0][a], flegal[1][b]);
                    sum += ValueFS(av, &wf);
                }
                else
                    sum += Calib(av, VerbatimSample(nd->state, &legal[0][a], &legal[1][b], nd->kind, nd->requester, seed, &work));
            }
            M[a * n[1] + b] = (float)(sum / K);
            av->sims += K;
        }

    // control variates (all from side 0's view; a = side 0's action, b = side 1's)
    term = vNext - M[idx[0] * n[1] + idx[1]];                      // chance
    pol[0] = nd->havePolicy[0] && nd->policyN[0] == n[0] ? nd->policy[0] : NULL;
    pol[1] = nd->havePolicy[1] && nd->policyN[1] == n[1] ? nd->policy[1] : NULL;
    if (n[0] == 1) pol[0] = NULL;   // nothing to correct
    if (n[1] == 1) pol[1] = NULL;
    if (pol[1])
    {
        // side 1's action given side 0's: M(a*, b*) - sum_b sigma_b M(a*, b)
        double e = 0.0;
        for (b = 0; b < n[1]; b++) e += pol[1][b] * M[idx[0] * n[1] + b];
        term += M[idx[0] * n[1] + idx[1]] - e;
        if (pol[0])
        {
            // side 0's action: sum_b sigma_b M(a*, b) - sum_a sum_b sigma_a sigma_b M(a, b)
            for (a = 0; a < n[0]; a++) { rowAvg[a] = 0.0; for (b = 0; b < n[1]; b++) rowAvg[a] += pol[1][b] * M[a * n[1] + b]; W += pol[0][a] * rowAvg[a]; }
            term += e - W;
        }
    }
    else if (pol[0])
    {
        // only side 0's distribution is known: M(a*, b*) - sum_a sigma_a M(a, b*)
        double e = 0.0;
        for (a = 0; a < n[0]; a++) e += pol[0][a] * M[a * n[1] + idx[1]];
        term += M[idx[0] * n[1] + idx[1]] - e;
    }
    av->correction += term;
    if (getenv("AIVAT_DEBUG")) fprintf(stderr, "node turn %u kind %d n %dx%d pol %d/%d vNext %+.3f M* %+.3f term %+.4f total %+.4f\n", nd->state->turnCount, nd->kind, n[0], n[1], pol[0] != NULL, pol[1] != NULL, vNext, M[idx[0] * n[1] + idx[1]], term, av->correction);
}

double Sim_AivatScore(const struct SimAivat *av, double z)
{
    return z - av->correction;
}
