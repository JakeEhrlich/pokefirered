// Tournament runner: agents play each other on teams drawn from a pool, results feed ELO ratings.
//
//   arena --teams pool.tsv --agents random,greedy,rmplus:iters=100 --games 2000 [--threads 8] [--seed 1]
//         [--out games.jsonl] [--ratings ratings.json] [--maxturns 300] [--k 24] [--doubles]
//         [--record games.tsv]   (every game's seed, sides, result, team ids and accepted actions: replayable, see py/frlgsim)
//         [--paired]             (games 2k and 2k+1 share agents, teams and engine seed with the sides swapped)
//   arena --teams pool.tsv --rate-teams --agent rmplus:iters=100 --games 20000 ...   (teams are the players)
//   --pool  = the built-in agent pool (random ... rmplus:iters=1000)
//   --aivat K [--aivat-calib k] [--aivat-verbatim] [--aivat-lookahead K1]   AIVAT scoring (include/sim_aivat.h)
//   --cs-decide | --cs-eps E [--cs-alpha A] [--cs-topt T] [--cs-burnin N]  anytime-valid confidence sequence on the
//         agent-0-vs-agent-1 score stream (two agents only); the run stops when 0 is excluded / the half-width is <= E
//
// pool.tsv is the flat form from tools/teams.py tsv (one mon per line). Doubles teams are skipped unless
// --doubles (the joint-action agents evaluate single actions there).
//
// --paired: a paired design that cancels team imbalance. The pair index k = game / 2 drives every random draw
// (agents, teams, engine seed), so both games of a pair are the same matchup; game 2k puts A (and teamA) on
// side 0, game 2k+1 puts them on side 1. The two games only differ through the agents' decisions. The
// --out lines carry "pair":k. The game's own AI can only play side 1, so a pair involving it is not swapped.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include "global.h"
#include "battle.h"
#include "pokemon.h"
#include "sim.h"
#include "sim_agent.h"
#include "sim_aivat.h"
#include "constants/species.h"

#define MAX_AGENTS 64
#define MAX_TEAMS 20000

struct Team { char id[64]; u8 doubles; u8 n; struct Pokemon party[PARTY_SIZE]; };
struct Rated { char name[96]; double rating; int games, wins, losses, draws; };

static struct Team *sTeams;
static int sTeamCount;
static struct SimAgent sAgents[MAX_AGENTS];
static int sAgentCount;
static struct Rated sRated[MAX_TEAMS];
static int sRatedCount;
static int sGames = 1000, sThreads = 4, sMaxTurns = 300, sAllowDoubles = 0, sRateTeams = 0, sPaired = 0;
static u32 sSeed = 1;
static double sK = 24.0;
static FILE *sOut, *sRecord;
#define REC_MAX 40000
static _Thread_local u8 sRecActs[REC_MAX][8];   // battler, kind, type, moveSlot, target, partySlot, item lo, item hi
static _Thread_local int sRecCount;
static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;
static int sNextGame;
static long sTotalTurns, sTotalDecisions;
// --aivat K: AIVAT scoring (see include/sim_aivat.h). Per game the worker gets side 0's raw and AIVAT scores.
static int sAivatK = 0, sAivatVerbatim = 0;
static float sAivatCalib = 0;
static int sAivatVf = 0, sAivatK1 = 2;
static FILE *sAivatDump;
static _Thread_local double sGameRaw, sGameAivat;
static _Thread_local struct SimAivat sAv;
static long sAvNodes, sAvFast, sAvSkipped, sAvSims;
struct PairStat { long n; double raw, raw2, av, av2; };
static struct PairStat sPairStat[MAX_AGENTS][MAX_AGENTS];   // [ia][ib], scores from ia's view (ia < ib)
static double *sPairRawG, *sPairAvG; static u8 *sPairHaveG; static int *sPairIa, *sPairIb;   // --paired: per game, for pair-level SEs
// Anytime-valid monitoring (AV-AIVAT, Li/Chen/Huang 2026, eq. 5: the asymptotic confidence sequence of Waudby-Smith et al.):
// the per-game (per-pair when --paired) score stream of agent 0 vs agent 1 is monitored in game order; the run stops at
// the first prefix whose half-width is <= --cs-eps, or (with --cs-decide) whose interval excludes 0. Both the raw and
// the AIVAT stream are tracked; the primary one (AIVAT when --aivat) decides.
static double sCsEps = 0, sCsAlpha = 0.05, sCsRho = 0; static int sCsDecide = 0, sCsTopt = 200, sCsBurnin = 30;   // the CS is asymptotic: no stop before sCsBurnin units
static double *sStreamRaw, *sStreamAv; static u8 *sStreamHave;   // per game (or per pair): agent 0's score vs agent 1
static volatile int sStop; static int sCsStopRaw = -1, sCsStopAv = -1; static double sCsWidthRaw, sCsWidthAv, sCsMeanRaw, sCsMeanAv;
static double CsHalfWidth(double var, long t, double rho, double alpha)
{
    double r2 = rho * rho;
    return sqrt(var * 2 * (t * r2 + 1) / (t * t * r2) * log(sqrt(t * r2 + 1) / alpha));
}
// returns the stopping index (1-based unit count) on the complete prefix, or -1; fills the interval at the last complete unit
static int CsScan(const double *x, const u8 *have, int units, int stopAt, double *meanOut, double *widthOut)
{
    double sum = 0, sum2 = 0; long t; int stop = -1;
    *meanOut = 0; *widthOut = 0;
    for (t = 1; t <= units && have[t - 1]; t++)
    {
        double m, v, w;
        sum += x[t - 1]; sum2 += x[t - 1] * x[t - 1];
        m = sum / t; v = t > 1 ? (sum2 - t * m * m) / (t - 1) : 1.0;
        if (v < 1e-12) v = 1e-12;
        w = CsHalfWidth(v, t, sCsRho, sCsAlpha);
        *meanOut = m; *widthOut = w;
        if (stop < 0 && t >= sCsBurnin && ((sCsEps > 0 && w <= sCsEps) || (sCsDecide && fabs(m) > w))) { stop = (int)t; if (stopAt) break; }
    }
    return stop;
}

static const char *const sPool[] = {
    "game:flags=smart", "random", "movebias", "greedy", "epsgreedy:eps=0.1",
    "expect", "epsexpect:eps=0.1", "rm:iters=10", "rm:iters=10,floor=0.011",
    "rmplus:iters=10", "rmplus:iters=10,floor=0.011", "rmplus:iters=30", "rmplus:iters=100", "rmplus:iters=1000",
    "rmplus:iters=100,samples=16", "rmplus:iters=1000,samples=16", "rmsample:iters=100", "rmsample:iters=1000",
};

static int LoadTeams(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return -1; }
    sTeams = calloc(MAX_TEAMS, sizeof(*sTeams));
    for (;;)
    {
        struct Team *t = &sTeams[sTeamCount];
        int n = Sim_ReadTeamTsv(f, t->party, t->id, sizeof(t->id), &t->doubles);
        if (n == 0) break;
        t->n = n;
        if (t->doubles && !sAllowDoubles) continue;
        if (t->doubles && n < 2) continue;
        if (++sTeamCount >= MAX_TEAMS) break;
    }
    fclose(f);
    return sTeamCount;
}

static double Expected(double ra, double rb) { return 1.0 / (1.0 + pow(10.0, (rb - ra) / 400.0)); }

static void Update(int a, int b, double scoreA)
{
    double ea = Expected(sRated[a].rating, sRated[b].rating);
    sRated[a].rating += sK * (scoreA - ea);
    sRated[b].rating += sK * ((1.0 - scoreA) - (1.0 - ea));
    sRated[a].games++; sRated[b].games++;
    if (scoreA == 1.0) { sRated[a].wins++; sRated[b].losses++; }
    else if (scoreA == 0.0) { sRated[a].losses++; sRated[b].wins++; }
    else { sRated[a].draws++; sRated[b].draws++; }
}

// Plays one game: agent A on side `sideA`, agent B on the other. Returns 1 = A won, 0 = B won, 2 = draw, -1 error.
static _Thread_local struct SimAgent sStatsA, sStatsB; // per-game copies, statistics harvested by the worker

static int PlayGame(struct SimAgent *tplA, struct SimAgent *tplB, struct Team *teamA, struct Team *teamB, u32 seed, int sideA,
                    int *turns, long *decisions)
{
    struct BattleSim *sim = malloc(sizeof(*sim));
    struct BattleSim *turnStart = malloc(sizeof(*sim));
    struct BattleSim *nodeState = sAivatK ? malloc(sizeof(*sim)) : NULL;   // --aivat: the pending decision point
    struct SimAivatNode node;
    int nodePending = 0;
    struct SimAgent agents[2];
    struct Team *teams[2];
    int result = -1, res, guard = 0;
    u32 flags = BATTLE_TYPE_TRAINER | ((teamA->doubles || teamB->doubles) ? BATTLE_TYPE_DOUBLE : 0);

    agents[sideA] = *tplA; agents[sideA ^ 1] = *tplB;
    agents[0].decisions = agents[0].matrixCells = agents[0].simulatedTurns = 0; agents[0].decideSeconds = 0;
    agents[1].decisions = agents[1].matrixCells = agents[1].simulatedTurns = 0; agents[1].decideSeconds = 0;
    teams[sideA] = teamA; teams[sideA ^ 1] = teamB;
    agents[0].rng = seed * 2654435761u + 17; agents[1].rng = seed * 2246822519u + 29;
    *turns = 0; *decisions = 0;

    Sim_Init(sim, flags, 1);
    memcpy(sim->playerParty, teams[0]->party, sizeof(sim->playerParty));
    memcpy(sim->enemyParty, teams[1]->party, sizeof(sim->enemyParty));
    sim->createTrainerParty = FALSE;
    sim->trainerBattleOpponent_A = Sim_TrainerForAIFlags(agents[1].isGameAI ? agents[1].aiFlags : 7);
    if (sim->trainerBattleOpponent_A == 0) sim->trainerBattleOpponent_A = 1;
    sim->rngXorshift = 1;
    sim->rngValue = seed | 1;
    sim->rngCalls = 0;
    sim->strictAnswers = 1;
    sim->maxTurns = sMaxTurns;
    sim->badgeFlags = 0;   // no player-side badge stat boosts: both sides equal
    sim->exactFrames = 0;   // no extra menu frames: identical trajectories, less work
    if (agents[1].isGameAI)
        Sim_SetPolicy(sim, B_SIDE_OPPONENT, Sim_VanillaAIPolicy);
    if (agents[0].isGameAI) { free(sim); free(turnStart); return -1; } // the game's AI only plays the opponent side
    if (Sim_Start(sim) != 0) { free(sim); free(turnStart); return -1; }
    turnStart->turnCount = 0xFFFF;
    sRecCount = 0;
    if (sAivatK) { Sim_AivatBegin(&sAv, seed ^ 0x51ED270Bu, sAivatK); sAv.verbatimOnly = sAivatVerbatim; sAv.calibK = sAivatCalib; sAv.vf = sAivatVf; sAv.K1 = sAivatK1; memset(&node, 0, sizeof(node)); node.state = nodeState; }
    for (;;)
    {
        res = Sim_Run(sim);
        if (res == SIM_RUN_FINISHED)
        {
            result = sim->battleOutcome == B_OUTCOME_WON ? (sideA == 0) : sim->battleOutcome == B_OUTCOME_LOST ? (sideA == 1) : 2;
            if (sAivatK && nodePending) { Sim_AivatNode(&sAv, &node, sim); nodePending = 0; }
            break;
        }
        if (res != SIM_RUN_REQUEST) { result = 2; break; }
        if (++guard > 20000) { result = 2; break; }
        {
            u8 b = sim->requestBattler, kind = sim->requestKind, side = b & BIT_SIDE;
            struct SimAgent *ag = &agents[side];
            struct SimAction act;
            if (kind == SIM_REQ_ACTION && b == 0)
                memcpy(turnStart, sim, sizeof(*sim));
            if (sAivatK)
            {
                // a new decision point unless this is the second action request of the pending turn
                int sameTurn = nodePending && node.kind == SIM_REQ_ACTION && kind == SIM_REQ_ACTION && nodeState->turnCount == sim->turnCount;
                if (!sameTurn)
                {
                    if (nodePending) Sim_AivatNode(&sAv, &node, sim);
                    memcpy(nodeState, sim, sizeof(*sim));
                    memset(&node, 0, sizeof(node)); node.state = nodeState; node.kind = kind; node.requester = b;
                    nodePending = 1;
                }
            }
            ag->policyValid = 0;
            if (getenv("ARENA_TRACE"))
                fprintf(stderr, "T %d %d %d %u %u hp %d %d\n", sim->turnCount, b, kind, sim->rngValue, sim->rngCalls, sim->battleMons[0].hp, sim->battleMons[1].hp);
            (*decisions)++;
            {
                struct timespec t0, t1;
                clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t0);
                ag->decide(ag, sim, (kind == SIM_REQ_ACTION && turnStart->turnCount == sim->turnCount) ? turnStart : NULL, b, kind, &act);
                clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t1);
                ag->decideSeconds += (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
            }
            if (Sim_Answer(sim, b, &act) != 0)
            {
                struct SimAction acts[32];
                u8 slots[PARTY_SIZE];
                int n;
                memset(&act, 0, sizeof(act));
                if (kind == SIM_REQ_SWITCH) { n = Sim_LegalSwitches(sim, b, slots, PARTY_SIZE); act.type = B_ACTION_SWITCH; act.partySlot = n ? slots[0] : 0; }
                else { n = Sim_LegalActions(sim, b, acts, 32); if (n) act = acts[0]; else { act.type = B_ACTION_USE_MOVE; act.target = 0xFF; } }
                if (Sim_Answer(sim, b, &act) != 0) { result = 2; break; }
                ag->policyValid = 0;   // the fallback action was not drawn from the agent's distribution
            }
            if (sAivatK && nodePending)
            {
                node.act[side] = act; node.haveAct[side] = 1;
                if (ag->policyValid) { memcpy(node.policy[side], ag->policy, sizeof(node.policy[side])); node.policyN[side] = ag->policyN; node.havePolicy[side] = 1; }
            }
            if (sRecord && sRecCount < REC_MAX)
            {
                u8 *r = sRecActs[sRecCount++];
                r[0] = b; r[1] = kind; r[2] = act.type; r[3] = act.moveSlot; r[4] = act.target; r[5] = act.partySlot;
                r[6] = act.item & 0xFF; r[7] = act.item >> 8;
            }
        }
    }
    *turns = sim->turnCount;
    sStatsA = agents[sideA]; sStatsB = agents[sideA ^ 1];
    if (sAivatK)
    {
        double z = result == 2 ? 0.0 : (result == 1) == (sideA == 0) ? 1.0 : -1.0;   // side 0's raw score
        sGameRaw = z;
        sGameAivat = Sim_AivatScore(&sAv, z);
        if (sAivatDump)
        {
            int q;
            pthread_mutex_lock(&sLock);
            fprintf(sAivatDump, "GAME %.0f %.4f %d\n", z, sAv.correction, sAv.nV);
            for (q = 0; q < sAv.nV; q++) fprintf(sAivatDump, "%.4f %.0f\n", sAv.vTrace[q], z);
            pthread_mutex_unlock(&sLock);
        }
        free(nodeState);
    }
    free(sim); free(turnStart);
    return result;
}

static void *Worker(void *arg)
{
    for (;;)
    {
        int g, k, ia, ib, ta, tb, sideA, r, turns;
        long decisions;
        u32 x, seed;
        pthread_mutex_lock(&sLock);
        g = sNextGame++;
        pthread_mutex_unlock(&sLock);
        if (g >= sGames || sStop) break;
        k = sPaired ? g >> 1 : g;   // --paired: the pair index drives the draws, so games 2k and 2k+1 match
        x = sSeed * 0x9E3779B9u + (u32)k * 0x85EBCA6Bu; x ^= x >> 15; x *= 0x2C1B3C6Du; x ^= x >> 12; x *= 0x297A2D39u; x ^= x >> 15;
        seed = x;
        if (sRateTeams)
        {
            ia = ib = 0;
            ta = x % sTeamCount; tb = (x >> 8) % sTeamCount;
            if (tb == ta) tb = (ta + 1) % sTeamCount;
        }
        else
        {
            ia = x % sAgentCount; ib = (x >> 8) % sAgentCount;
            if (ib == ia) ib = (ia + 1) % sAgentCount;
            ta = (x >> 16) % sTeamCount; tb = ((x >> 24) ^ (x << 3)) % sTeamCount;
        }
        sideA = g & 1;   // --paired: 0 for game 2k, 1 for game 2k+1 (A and teamA swap sides within the pair)
        if (sAgents[ia].isGameAI) sideA = 1;
        if (sAgents[ib].isGameAI) sideA = 0;
        if (sAgents[ia].isGameAI && sAgents[ib].isGameAI) continue;
        r = PlayGame(&sAgents[ia], &sAgents[ib], &sTeams[ta], &sTeams[tb], seed, sideA, &turns, &decisions);
        if (r < 0) continue;
        pthread_mutex_lock(&sLock);
        sAgents[ia].decisions += sStatsA.decisions; sAgents[ia].matrixCells += sStatsA.matrixCells; sAgents[ia].simulatedTurns += sStatsA.simulatedTurns; sAgents[ia].decideSeconds += sStatsA.decideSeconds;
        sAgents[ib].decisions += sStatsB.decisions; sAgents[ib].matrixCells += sStatsB.matrixCells; sAgents[ib].simulatedTurns += sStatsB.simulatedTurns; sAgents[ib].decideSeconds += sStatsB.decideSeconds;
        if (sRateTeams) Update(ta, tb, r == 1 ? 1.0 : r == 0 ? 0.0 : 0.5);
        else Update(ia, ib, r == 1 ? 1.0 : r == 0 ? 0.0 : 0.5);
        if (sAivatK && !sRateTeams)
        {
            // scores from A's (agent ia's) view; stored under the unordered pair with the lower index first
            double raw = sideA == 0 ? sGameRaw : -sGameRaw, av = sideA == 0 ? sGameAivat : -sGameAivat;
            int lo = ia < ib ? ia : ib, hi = ia < ib ? ib : ia;
            struct PairStat *ps = &sPairStat[lo][hi];
            if (lo != ia) { raw = -raw; av = -av; }
            ps->n++; ps->raw += raw; ps->raw2 += raw * raw; ps->av += av; ps->av2 += av * av;
            sAvNodes += sAv.nodes; sAvFast += sAv.nodesFast; sAvSkipped += sAv.nodesSkipped; sAvSims += sAv.sims;
            if (sPaired) { sPairRawG[g] = raw; sPairAvG[g] = av; sPairHaveG[g] = 1; sPairIa[g] = lo; sPairIb[g] = hi; }
        }
        if (sStreamRaw && sAgentCount == 2)
        {
            // agent 0's score in this game; the unit is the game, or the pair mean when --paired
            double raw = (sideA == 0 ? sGameRaw : -sGameRaw) * (ia == 0 ? 1 : -1), av = sAivatK ? (sideA == 0 ? sGameAivat : -sGameAivat) * (ia == 0 ? 1 : -1) : raw;
            int u = sPaired ? g >> 1 : g, units = sPaired ? sGames / 2 : sGames;
            if (sPaired)
            {
                if (sStreamHave[u] == 0) { sStreamRaw[u] = raw; sStreamAv[u] = av; sStreamHave[u] = 1; }
                else { sStreamRaw[u] = (sStreamRaw[u] + raw) / 2; sStreamAv[u] = (sStreamAv[u] + av) / 2; sStreamHave[u] = 2; }
            }
            else { sStreamRaw[u] = raw; sStreamAv[u] = av; sStreamHave[u] = 2; }
            if ((sCsEps > 0 || sCsDecide) && !sStop)
            {
                u8 *complete = malloc(units); int k2, tr, ta2;
                for (k2 = 0; k2 < units; k2++) complete[k2] = sStreamHave[k2] == 2;
                tr = CsScan(sStreamRaw, complete, units, 1, &sCsMeanRaw, &sCsWidthRaw);
                ta2 = CsScan(sStreamAv, complete, units, 1, &sCsMeanAv, &sCsWidthAv);
                if (tr > 0 && sCsStopRaw < 0) sCsStopRaw = tr;
                if (ta2 > 0 && sCsStopAv < 0) sCsStopAv = ta2;
                if ((sAivatK ? ta2 : tr) > 0) sStop = 1;
                free(complete);
            }
        }
        sTotalTurns += turns; sTotalDecisions += decisions;
        if (sOut)
        {
            fprintf(sOut, "{\"game\":%d,", g);
            if (sPaired) fprintf(sOut, "\"pair\":%d,", k);
            fprintf(sOut, "\"a\":\"%s\",\"b\":\"%s\",\"teamA\":\"%s\",\"teamB\":\"%s\",\"sideA\":%d,\"result\":\"%s\",\"turns\":%d,\"seed\":%u}\n",
                    sRateTeams ? sTeams[ta].id : sAgents[ia].name, sRateTeams ? sTeams[tb].id : sAgents[ib].name,
                    sTeams[ta].id, sTeams[tb].id, sideA, r == 1 ? "A" : r == 0 ? "B" : "draw", turns, seed);
        }
        if (sRecord)
        {
            int k;
            fprintf(sRecord, "%u\t%d\t%d\t%d\t%s\t%s\t", seed, sideA, r, turns, sTeams[ta].id, sTeams[tb].id);
            for (k = 0; k < sRecCount; k++)
            {
                const u8 *a = sRecActs[k];
                fprintf(sRecord, "%s%d,%d,%d,%d,%d,%d,%d", k ? " " : "", a[0], a[1], a[2], a[3], a[4], a[5], a[6] | (a[7] << 8));
            }
            fputc('\n', sRecord);
        }
        if ((g + 1) % 200 == 0) { fprintf(stderr, "\r%d/%d games", g + 1, sGames); fflush(stderr); }
        pthread_mutex_unlock(&sLock);
    }
    return NULL;
}

static int CompareRated(const void *a, const void *b)
{
    const struct Rated *x = a, *y = b;
    return x->rating < y->rating ? 1 : x->rating > y->rating ? -1 : 0;
}

int main(int argc, char **argv)
{
    const char *teamsPath = NULL, *agentSpecs = NULL, *outPath = NULL, *ratingsPath = NULL, *recordPath = NULL, *singleAgent = "rmplus:iters=100";
    int i, usePool = 0;
    pthread_t threads[64];
    clock_t t0;

    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--teams") && i + 1 < argc) teamsPath = argv[++i];
        else if (!strcmp(argv[i], "--agents") && i + 1 < argc) agentSpecs = argv[++i];
        else if (!strcmp(argv[i], "--agent") && i + 1 < argc) singleAgent = argv[++i];
        else if (!strcmp(argv[i], "--pool")) usePool = 1;
        else if (!strcmp(argv[i], "--games") && i + 1 < argc) sGames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) sThreads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) sSeed = (u32)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) outPath = argv[++i];
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) recordPath = argv[++i];
        else if (!strcmp(argv[i], "--ratings") && i + 1 < argc) ratingsPath = argv[++i];
        else if (!strcmp(argv[i], "--maxturns") && i + 1 < argc) sMaxTurns = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--k") && i + 1 < argc) sK = atof(argv[++i]);
        else if (!strcmp(argv[i], "--doubles")) sAllowDoubles = 1;
        else if (!strcmp(argv[i], "--rate-teams")) sRateTeams = 1;
        else if (!strcmp(argv[i], "--paired")) sPaired = 1;
        else if (!strcmp(argv[i], "--aivat") && i + 1 < argc) sAivatK = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--aivat-verbatim")) sAivatVerbatim = 1;
        else if (!strcmp(argv[i], "--aivat-calib") && i + 1 < argc) sAivatCalib = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--aivat-lookahead") && i + 1 < argc) { sAivatVf = 1; sAivatK1 = atoi(argv[++i]); if (sAivatK1 < 1) sAivatK1 = 1; }
        else if (!strcmp(argv[i], "--cs-eps") && i + 1 < argc) sCsEps = atof(argv[++i]);
        else if (!strcmp(argv[i], "--cs-decide")) sCsDecide = 1;
        else if (!strcmp(argv[i], "--cs-alpha") && i + 1 < argc) sCsAlpha = atof(argv[++i]);
        else if (!strcmp(argv[i], "--cs-topt") && i + 1 < argc) sCsTopt = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cs-burnin") && i + 1 < argc) sCsBurnin = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--aivat-dump") && i + 1 < argc) sAivatDump = fopen(argv[++i], "w");
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (!teamsPath) { fprintf(stderr, "--teams <pool.tsv> is required\n"); return 2; }
    if (sThreads < 1) sThreads = 1;
    if (sThreads > 64) sThreads = 64;

    if (sRateTeams)
    {
        if (Sim_AgentFromSpec(&sAgents[0], singleAgent) != 0) { fprintf(stderr, "bad agent: %s\n", sAgents[0].name); return 2; }
        sAgentCount = 1;
        if (sAgents[0].isGameAI) { fprintf(stderr, "the game AI cannot play both sides\n"); return 2; }
    }
    else
    {
        char buf[4096];
        char *tok;
        if (usePool)
            for (i = 0; i < (int)(sizeof(sPool) / sizeof(sPool[0])); i++)
                if (Sim_AgentFromSpec(&sAgents[sAgentCount++], sPool[i]) != 0) { fprintf(stderr, "bad agent: %s\n", sAgents[sAgentCount - 1].name); return 2; }
        if (agentSpecs)
        {
            snprintf(buf, sizeof(buf), "%s", agentSpecs);
            for (tok = strtok(buf, " ;"); tok; tok = strtok(NULL, " ;"))
                if (Sim_AgentFromSpec(&sAgents[sAgentCount++], tok) != 0) { fprintf(stderr, "bad agent: %s\n", sAgents[sAgentCount - 1].name); return 2; }
        }
        if (sAgentCount < 2) { fprintf(stderr, "need at least two agents (--agents \"a b c\" or --pool)\n"); return 2; }
    }
    {
        static struct BattleSim boot;
        Sim_Init(&boot, 0, 1); // binds the engine globals for the loader (mon creation runs engine code)
    }
    if (LoadTeams(teamsPath) < 2) { fprintf(stderr, "need at least two usable teams in %s\n", teamsPath); return 2; }

    sRatedCount = sRateTeams ? sTeamCount : sAgentCount;
    for (i = 0; i < sRatedCount; i++)
    {
        snprintf(sRated[i].name, sizeof(sRated[i].name), "%s", sRateTeams ? sTeams[i].id : sAgents[i].name);
        sRated[i].rating = 1000.0;
    }
    if (outPath) sOut = fopen(outPath, "w");
    if (recordPath) sRecord = fopen(recordPath, "w");
    if (sPaired && (sGames & 1)) { sGames++; fprintf(stderr, "--paired: rounding --games up to %d\n", sGames); }
    if (sAgentCount == 2 && !sRateTeams)
    {
        int units = sPaired ? sGames / 2 : sGames;
        sStreamRaw = calloc(units + 1, sizeof(double)); sStreamAv = calloc(units + 1, sizeof(double)); sStreamHave = calloc(units + 1, 1);
        // rho for a target unit count (Waudby-Smith et al.): minimises the width around t_opt
        sCsRho = sqrt((-2 * log(sCsAlpha) + log(-2 * log(sCsAlpha) + 1)) / sCsTopt);
    }
    if (sAivatK && sPaired) { sPairRawG = calloc(sGames, sizeof(double)); sPairAvG = calloc(sGames, sizeof(double)); sPairHaveG = calloc(sGames, 1); sPairIa = calloc(sGames, sizeof(int)); sPairIb = calloc(sGames, sizeof(int)); }
    fprintf(stderr, "arena: %d teams, %d agents, %d games%s, %d threads, seed %u\n", sTeamCount, sAgentCount, sGames, sPaired ? " (paired)" : "", sThreads, sSeed);
    t0 = clock();
    for (i = 0; i < sThreads; i++) pthread_create(&threads[i], NULL, Worker, NULL);
    for (i = 0; i < sThreads; i++) pthread_join(threads[i], NULL);
    fprintf(stderr, "\n");
    if (sOut) fclose(sOut);
    if (sRecord) fclose(sRecord);

    {
        struct Rated *sorted = malloc(sizeof(struct Rated) * sRatedCount);
        FILE *rf = ratingsPath ? fopen(ratingsPath, "w") : NULL;
        memcpy(sorted, sRated, sizeof(struct Rated) * sRatedCount);
        qsort(sorted, sRatedCount, sizeof(struct Rated), CompareRated);
        if (rf) fprintf(rf, "[\n");
        for (i = 0; i < sRatedCount; i++)
        {
            if (i < 40 || !sRateTeams)
                printf("%7.1f  %5d games  %4d-%4d-%4d  %s\n", sorted[i].rating, sorted[i].games, sorted[i].wins, sorted[i].losses, sorted[i].draws, sorted[i].name);
            if (rf) fprintf(rf, "  {\"name\":\"%s\",\"rating\":%.1f,\"games\":%d,\"wins\":%d,\"losses\":%d,\"draws\":%d}%s\n",
                            sorted[i].name, sorted[i].rating, sorted[i].games, sorted[i].wins, sorted[i].losses, sorted[i].draws, i + 1 < sRatedCount ? "," : "");
        }
        if (rf) { fprintf(rf, "]\n"); fclose(rf); }
        free(sorted);
    }
    {
        long played = 0;
        for (i = 0; i < sRatedCount; i++) played += sRated[i].games;
        played /= 2;
        if (played < 1) played = 1;
        printf("%ld games played, avg %.1f turns, %.1f decisions per game, %.1fs cpu\n", played, (double)sTotalTurns / played, (double)sTotalDecisions / played,
               (double)(clock() - t0) / CLOCKS_PER_SEC);
    }
    if (sStreamRaw)
    {
        int units = sPaired ? sGames / 2 : sGames, k2, tr, ta2; double mr, wr, ma, wa;
        u8 *complete = malloc(units);
        for (k2 = 0; k2 < units; k2++) complete[k2] = sStreamHave[k2] == 2;
        tr = CsScan(sStreamRaw, complete, units, 0, &mr, &wr);
        ta2 = CsScan(sStreamAv, complete, units, 0, &ma, &wa);
        printf("confidence sequence (alpha %.3f, rho %.3f, unit = %s, %s): raw %+.4f +- %.4f; AIVAT %+.4f +- %.4f\n", sCsAlpha, sCsRho, sPaired ? "pair" : "game",
               sCsEps > 0 ? "stop when half-width <= eps" : sCsDecide ? "stop when 0 is excluded" : "no stopping rule", mr, wr, ma, wa);
        if (tr > 0 || ta2 > 0) printf("  stopping unit: raw %d, AIVAT %d (-1 = not reached)%s\n", tr, ta2, sStop ? "; the run stopped early" : "");
        free(complete);
    }
    if (sAivatK && !sRateTeams)
    {
        int a, b;
        printf("AIVAT (K=%d, %s): %ld decision points, %ld on the fast engine, %ld skipped, %ld referee sims\n", sAivatK, sAivatVerbatim ? "verbatim" : "fast engine when possible",
               sAvNodes, sAvFast, sAvSkipped, sAvSims);
        for (a = 0; a < sAgentCount; a++)
            for (b = a + 1; b < sAgentCount; b++)
            {
                struct PairStat *ps = &sPairStat[a][b];
                double mr, ma, vr, va, pr, pa, er, ea, dr, da;
                if (ps->n < 2) continue;
                mr = ps->raw / ps->n; ma = ps->av / ps->n;
                vr = (ps->raw2 - ps->n * mr * mr) / (ps->n - 1); va = (ps->av2 - ps->n * ma * ma) / (ps->n - 1);
                pr = (mr + 1) / 2; pa = (ma + 1) / 2;
                er = 400 * log10(pr / (1 - pr)); ea = 400 * log10(pa / (1 - pa));
                dr = 400 / log(10.0) / (pr * (1 - pr)) * sqrt(vr / ps->n) / 2; da = 400 / log(10.0) / (pa * (1 - pa)) * sqrt(va / ps->n) / 2;
                printf("  %s vs %s: %ld games\n    raw   score %+.4f +- %.4f  (ELO diff %+.0f +- %.0f)\n    AIVAT score %+.4f +- %.4f  (ELO diff %+.0f +- %.0f)   variance ratio %.1fx\n",
                       sAgents[a].name, sAgents[b].name, ps->n, mr, sqrt(vr / ps->n), er, dr, ma, sqrt(va / ps->n), ea, da, va > 0 ? vr / va : 0.0);
                if (sPaired)
                {
                    long np = 0; double sr = 0, sr2 = 0, sa = 0, sa2 = 0;
                    int g;
                    for (g = 0; g + 1 < sGames; g += 2)
                        if (sPairHaveG[g] && sPairHaveG[g + 1] && sPairIa[g] == a && sPairIb[g] == b)
                        {
                            double r = (sPairRawG[g] + sPairRawG[g + 1]) / 2, v = (sPairAvG[g] + sPairAvG[g + 1]) / 2;
                            np++; sr += r; sr2 += r * r; sa += v; sa2 += v * v;
                        }
                    if (np >= 2)
                    {
                        double pmr = sr / np, pma = sa / np, pvr = (sr2 - np * pmr * pmr) / (np - 1), pva = (sa2 - np * pma * pma) / (np - 1);
                        printf("    paired (%ld pairs): raw %+.4f +- %.4f, AIVAT %+.4f +- %.4f, variance ratio %.1fx\n", np, pmr, sqrt(pvr / np), pma, sqrt(pva / np), pva > 0 ? pvr / pva : 0.0);
                    }
                }
            }
    }
    if (!sRateTeams)
        for (i = 0; i < sAgentCount; i++)
            if (!sAgents[i].isGameAI)
                printf("  %-40s decisions %u, matrix cells %llu, simulated turns %llu, %.1f ms/decision, %.0fk sims/s, %.0f sims/decision\n", sAgents[i].name, sAgents[i].decisions, sAgents[i].matrixCells, sAgents[i].simulatedTurns,
                       sAgents[i].decisions ? 1e3 * sAgents[i].decideSeconds / sAgents[i].decisions : 0.0, sAgents[i].decideSeconds > 0 ? sAgents[i].simulatedTurns / sAgents[i].decideSeconds / 1e3 : 0.0, sAgents[i].decisions ? (double)sAgents[i].simulatedTurns / sAgents[i].decisions : 0.0);
    return 0;
}
