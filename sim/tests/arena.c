// Tournament runner: agents play each other on teams drawn from a pool, results feed ELO ratings.
//
//   arena --teams pool.tsv --agents random,greedy,rmplus:iters=100 --games 2000 [--threads 8] [--seed 1]
//         [--out games.jsonl] [--ratings ratings.json] [--maxturns 300] [--k 24] [--doubles]
//         [--record games.tsv]   (every game's seed, sides, result, team ids and accepted actions: replayable, see py/frlgsim)
//   arena --teams pool.tsv --rate-teams --agent rmplus:iters=100 --games 20000 ...   (teams are the players)
//   --pool  = the built-in agent pool (random ... rmplus:iters=1000)
//
// pool.tsv is the flat form from tools/teams.py tsv (one mon per line). Doubles teams are skipped unless
// --doubles (the joint-action agents evaluate single actions there).
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
static int sGames = 1000, sThreads = 4, sMaxTurns = 300, sAllowDoubles = 0, sRateTeams = 0;
static u32 sSeed = 1;
static double sK = 24.0;
static FILE *sOut, *sRecord;
#define REC_MAX 40000
static _Thread_local u8 sRecActs[REC_MAX][8];   // battler, kind, type, moveSlot, target, partySlot, item lo, item hi
static _Thread_local int sRecCount;
static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;
static int sNextGame;
static long sTotalTurns, sTotalDecisions;

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
    struct SimAgent agents[2];
    struct Team *teams[2];
    int result = -1, res, guard = 0;
    u32 flags = BATTLE_TYPE_TRAINER | ((teamA->doubles || teamB->doubles) ? BATTLE_TYPE_DOUBLE : 0);

    agents[sideA] = *tplA; agents[sideA ^ 1] = *tplB;
    agents[0].decisions = agents[0].matrixCells = agents[0].simulatedTurns = 0;
    agents[1].decisions = agents[1].matrixCells = agents[1].simulatedTurns = 0;
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
    if (agents[1].isGameAI)
        Sim_SetPolicy(sim, B_SIDE_OPPONENT, Sim_VanillaAIPolicy);
    if (agents[0].isGameAI) { free(sim); free(turnStart); return -1; } // the game's AI only plays the opponent side
    if (Sim_Start(sim) != 0) { free(sim); free(turnStart); return -1; }
    turnStart->turnCount = 0xFFFF;
    sRecCount = 0;
    for (;;)
    {
        res = Sim_Run(sim);
        if (res == SIM_RUN_FINISHED)
        {
            result = sim->battleOutcome == B_OUTCOME_WON ? (sideA == 0) : sim->battleOutcome == B_OUTCOME_LOST ? (sideA == 1) : 2;
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
            if (getenv("ARENA_TRACE"))
                fprintf(stderr, "T %d %d %d %u %u hp %d %d\n", sim->turnCount, b, kind, sim->rngValue, sim->rngCalls, sim->battleMons[0].hp, sim->battleMons[1].hp);
            (*decisions)++;
            ag->decide(ag, sim, (kind == SIM_REQ_ACTION && turnStart->turnCount == sim->turnCount) ? turnStart : NULL, b, kind, &act);
            if (Sim_Answer(sim, b, &act) != 0)
            {
                struct SimAction acts[32];
                u8 slots[PARTY_SIZE];
                int n;
                memset(&act, 0, sizeof(act));
                if (kind == SIM_REQ_SWITCH) { n = Sim_LegalSwitches(sim, b, slots, PARTY_SIZE); act.type = B_ACTION_SWITCH; act.partySlot = n ? slots[0] : 0; }
                else { n = Sim_LegalActions(sim, b, acts, 32); if (n) act = acts[0]; else { act.type = B_ACTION_USE_MOVE; act.target = 0xFF; } }
                if (Sim_Answer(sim, b, &act) != 0) { result = 2; break; }
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
    free(sim); free(turnStart);
    return result;
}

static void *Worker(void *arg)
{
    for (;;)
    {
        int g, ia, ib, ta, tb, sideA, r, turns;
        long decisions;
        u32 x, seed;
        pthread_mutex_lock(&sLock);
        g = sNextGame++;
        pthread_mutex_unlock(&sLock);
        if (g >= sGames) break;
        x = sSeed * 0x9E3779B9u + (u32)g * 0x85EBCA6Bu; x ^= x >> 15; x *= 0x2C1B3C6Du; x ^= x >> 12; x *= 0x297A2D39u; x ^= x >> 15;
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
        sideA = g & 1;
        if (sAgents[ia].isGameAI) sideA = 1;
        if (sAgents[ib].isGameAI) sideA = 0;
        if (sAgents[ia].isGameAI && sAgents[ib].isGameAI) continue;
        r = PlayGame(&sAgents[ia], &sAgents[ib], &sTeams[ta], &sTeams[tb], seed, sideA, &turns, &decisions);
        if (r < 0) continue;
        pthread_mutex_lock(&sLock);
        sAgents[ia].decisions += sStatsA.decisions; sAgents[ia].matrixCells += sStatsA.matrixCells; sAgents[ia].simulatedTurns += sStatsA.simulatedTurns;
        sAgents[ib].decisions += sStatsB.decisions; sAgents[ib].matrixCells += sStatsB.matrixCells; sAgents[ib].simulatedTurns += sStatsB.simulatedTurns;
        if (sRateTeams) Update(ta, tb, r == 1 ? 1.0 : r == 0 ? 0.0 : 0.5);
        else Update(ia, ib, r == 1 ? 1.0 : r == 0 ? 0.0 : 0.5);
        sTotalTurns += turns; sTotalDecisions += decisions;
        if (sOut)
            fprintf(sOut, "{\"game\":%d,\"a\":\"%s\",\"b\":\"%s\",\"teamA\":\"%s\",\"teamB\":\"%s\",\"sideA\":%d,\"result\":\"%s\",\"turns\":%d,\"seed\":%u}\n",
                    g, sRateTeams ? sTeams[ta].id : sAgents[ia].name, sRateTeams ? sTeams[tb].id : sAgents[ib].name,
                    sTeams[ta].id, sTeams[tb].id, sideA, r == 1 ? "A" : r == 0 ? "B" : "draw", turns, seed);
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
    fprintf(stderr, "arena: %d teams, %d agents, %d games, %d threads, seed %u\n", sTeamCount, sAgentCount, sGames, sThreads, sSeed);
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
    printf("%d games, avg %.1f turns, %.1f decisions per game, %.1fs cpu\n", sGames, (double)sTotalTurns / sGames, (double)sTotalDecisions / sGames,
           (double)(clock() - t0) / CLOCKS_PER_SEC);
    if (!sRateTeams)
        for (i = 0; i < sAgentCount; i++)
            if (!sAgents[i].isGameAI)
                printf("  %-40s decisions %u, matrix cells %u, simulated turns %u\n", sAgents[i].name, sAgents[i].decisions, sAgents[i].matrixCells, sAgents[i].simulatedTurns);
    return 0;
}
