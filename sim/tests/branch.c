// Sibling-pair data for a ranking loss (ai/DESIGN.md): one game per pair, split at one uniformly chosen turn.
//
//   branch --teams pool.tsv --agent "rmplus:iters=30,samples=16" --pairs 100000 [--threads 12] [--seed 1]
//          [--random-both 0.5] --out pairs.bin
//
// For every pair: two random teams play with the agent on both sides; the turn-start states are snapshotted;
// one turn t is chosen uniformly; from that state two distinct joint-action cells are simulated with fresh
// engine seeds (cell A is the agent's own joint choice unless --random-both hits, cell B is uniform over the
// other cells) up to the next decision; each child state is encoded (side-0 view) and then played out with the
// agent to the end. Record (binary, little endian):
//   int32 meta[16]: pair, turn, turns, iA, jA, iB, jB, outcomeA, outcomeB (1 side-0 won, 0 lost, 2 draw, -1 error),
//                   randomBoth, nMine, nTheirs, decisionsA, decisionsB, 0, 0
//   float16 F_A[SIMENC_FLOATS], int16 I_A[SIMENC_INTS], float16 F_B[...], int16 I_B[...]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include "global.h"
#include "battle.h"
#include "pokemon.h"
#include "sim.h"
#include "sim_agent.h"
#include "sim_encode.h"
#include "constants/species.h"

#define MAX_TEAMS 40000
#define MAX_TURNS 300
#define MAX_ACTIONS 32

struct Team { char id[64]; u8 doubles; u8 n; struct Pokemon party[PARTY_SIZE]; };
static struct Team *sTeams;
static int sTeamCount;
static struct SimAgent sAgent;
static int sPairs = 1000, sThreads = 8, sMaxTurns = 300;
static u32 sSeed = 1;
static double sRandomBoth = 0.5;
static FILE *sOut;
static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;
static int sNext, sWritten, sDiscordant, sErrors;
static long sDecisions;

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
        if (t->doubles) continue;
        if (++sTeamCount >= MAX_TEAMS) break;
    }
    fclose(f);
    return sTeamCount;
}

static u32 Next(u32 *x) { *x ^= *x << 13; *x ^= *x >> 17; *x ^= *x << 5; return *x; }

// Plays `sim` to the end with the agent on both sides (snapshots turn starts into `snaps` when non-NULL).
// Returns the side-0 outcome (1/0/2) or -1 on an engine error. *decisions counts decisions.
static int PlayOut(struct BattleSim *sim, struct SimAgent *agents, struct BattleSim *snaps, int *nSnaps, int *decisions)
{
    static _Thread_local struct BattleSim turnStart;
    int guard = 0;
    turnStart.turnCount = 0xFFFF;
    for (;;)
    {
        int res = Sim_Run(sim);
        u8 b, kind;
        struct SimAction act;
        if (res == SIM_RUN_FINISHED)
            return sim->battleOutcome == B_OUTCOME_WON ? 1 : sim->battleOutcome == B_OUTCOME_LOST ? 0 : 2;
        if (res != SIM_RUN_REQUEST || ++guard > 20000) return -1;
        b = sim->requestBattler; kind = sim->requestKind;
        if (kind == SIM_REQ_ACTION && b == 0)
        {
            memcpy(&turnStart, sim, sizeof(*sim));
            if (snaps && *nSnaps < MAX_TURNS) memcpy(&snaps[(*nSnaps)++], sim, sizeof(*sim));
        }
        (*decisions)++;
        agents[b & 1].decide(&agents[b & 1], sim, (kind == SIM_REQ_ACTION && turnStart.turnCount == sim->turnCount) ? &turnStart : NULL, b, kind, &act);
        if (Sim_Answer(sim, b, &act) != 0)
        {
            struct SimAction acts[MAX_ACTIONS];
            u8 slots[PARTY_SIZE];
            int n;
            memset(&act, 0, sizeof(act));
            if (kind == SIM_REQ_SWITCH) { n = Sim_LegalSwitches(sim, b, slots, PARTY_SIZE); act.type = B_ACTION_SWITCH; act.partySlot = n ? slots[0] : 0; }
            else { n = Sim_LegalActions(sim, b, acts, MAX_ACTIONS); if (n) act = acts[0]; else { act.type = B_ACTION_USE_MOVE; act.target = 0xFF; } }
            if (Sim_Answer(sim, b, &act) != 0) return -1;
        }
    }
}

// Applies joint cell (i, j) from the turn-start state with a fresh engine seed and runs to the next decision.
static int ApplyCell(struct BattleSim *sim, const struct BattleSim *ts, const struct SimAction *mine, const struct SimAction *theirs, u32 seed)
{
    u8 first, second;
    int r;
    memcpy(sim, ts, sizeof(*sim));
    sim->rngXorshift = 1;
    sim->rngValue = seed | 1;
    first = sim->requestBattler;
    second = first ^ BIT_SIDE;
    if (Sim_Answer(sim, first, first == 0 ? mine : theirs) != 0) return -1;
    r = Sim_Run(sim);
    if (r == SIM_RUN_REQUEST && sim->requestBattler == second && sim->requestKind == SIM_REQ_ACTION)
    {
        if (Sim_Answer(sim, second, second == 0 ? mine : theirs) != 0) return -1;
        r = Sim_Run(sim);
    }
    if (r == SIM_RUN_STUCK || r == SIM_RUN_ERROR) return -1;
    return 0;
}

static void WriteHalf(float *F, int32_t *I, _Float16 *fh, int16_t *ih)
{
    int k;
    for (k = 0; k < SIMENC_FLOATS; k++) fh[k] = (_Float16)F[k];
    for (k = 0; k < SIMENC_INTS; k++) ih[k] = (int16_t)I[k];
}

static void *Worker(void *arg)
{
    struct BattleSim *sim = malloc(sizeof(*sim)), *snaps = malloc(sizeof(*sim) * MAX_TURNS), *childA = malloc(sizeof(*sim)), *childB = malloc(sizeof(*sim));
    float *FA = malloc(sizeof(float) * SIMENC_FLOATS), *FB = malloc(sizeof(float) * SIMENC_FLOATS);
    int32_t *IA = malloc(sizeof(int32_t) * SIMENC_INTS), *IB = malloc(sizeof(int32_t) * SIMENC_INTS);
    _Float16 *fhA = malloc(sizeof(_Float16) * SIMENC_FLOATS), *fhB = malloc(sizeof(_Float16) * SIMENC_FLOATS);
    int16_t *ihA = malloc(sizeof(int16_t) * SIMENC_INTS), *ihB = malloc(sizeof(int16_t) * SIMENC_INTS);
    struct SimAgent agents[2];
    for (;;)
    {
        int p, ta, tb, nSnaps = 0, decisions = 0, dA = 0, dB = 0, t, i, j, nMine, nTheirs, cellA, cellB, iA, jA, iB, jB, outA, outB, randomBoth;
        u32 x, seed;
        struct SimAction mine[MAX_ACTIONS], theirs[MAX_ACTIONS], chosen0, chosen1;
        int32_t meta[16];
        pthread_mutex_lock(&sLock);
        p = sNext++;
        pthread_mutex_unlock(&sLock);
        if (p >= sPairs) break;
        x = sSeed * 0x9E3779B9u + (u32)p * 0x85EBCA6Bu; x ^= x >> 15; x *= 0x2C1B3C6Du; x ^= x >> 12; x *= 0x297A2D39u; x ^= x >> 15; x |= 1;
        seed = x;
        ta = Next(&x) % sTeamCount; tb = Next(&x) % sTeamCount;
        if (tb == ta) tb = (ta + 1) % sTeamCount;
        agents[0] = sAgent; agents[1] = sAgent;
        agents[0].rng = Next(&x) | 1; agents[1].rng = Next(&x) | 1;
        // the prefix game, snapshotting every turn start
        Sim_Init(sim, BATTLE_TYPE_TRAINER, 1);
        memcpy(sim->playerParty, sTeams[ta].party, sizeof(sim->playerParty));
        memcpy(sim->enemyParty, sTeams[tb].party, sizeof(sim->enemyParty));
        sim->createTrainerParty = FALSE;
        sim->trainerBattleOpponent_A = Sim_TrainerForAIFlags(7);
        if (sim->trainerBattleOpponent_A == 0) sim->trainerBattleOpponent_A = 1;
        sim->rngXorshift = 1; sim->rngValue = seed | 1; sim->strictAnswers = 1; sim->maxTurns = sMaxTurns;
        sim->badgeFlags = 0;   // no player-side badge stat boosts: both sides equal
        if (Sim_Start(sim) != 0) { pthread_mutex_lock(&sLock); sErrors++; pthread_mutex_unlock(&sLock); continue; }
        if (PlayOut(sim, agents, snaps, &nSnaps, &decisions) < 0 || nSnaps == 0) { pthread_mutex_lock(&sLock); sErrors++; pthread_mutex_unlock(&sLock); continue; }
        // the split
        t = Next(&x) % nSnaps;
        nMine = Sim_LegalActions(&snaps[t], 0, mine, MAX_ACTIONS);
        nTheirs = Sim_LegalActions(&snaps[t], 1, theirs, MAX_ACTIONS);
        if (nMine * nTheirs < 2) { pthread_mutex_lock(&sLock); sErrors++; pthread_mutex_unlock(&sLock); continue; }
        randomBoth = (Next(&x) % 1000) < sRandomBoth * 1000;
        if (randomBoth)
            cellA = Next(&x) % (nMine * nTheirs);
        else
        {
            // the agent's own joint choice at this turn start (a sample from its strategies)
            agents[0].decide(&agents[0], &snaps[t], &snaps[t], 0, SIM_REQ_ACTION, &chosen0);
            agents[1].decide(&agents[1], &snaps[t], &snaps[t], 1, SIM_REQ_ACTION, &chosen1);
            iA = jA = 0;
            for (i = 0; i < nMine; i++) if (mine[i].type == chosen0.type && mine[i].moveSlot == chosen0.moveSlot && mine[i].partySlot == chosen0.partySlot && mine[i].item == chosen0.item) iA = i;
            for (j = 0; j < nTheirs; j++) if (theirs[j].type == chosen1.type && theirs[j].moveSlot == chosen1.moveSlot && theirs[j].partySlot == chosen1.partySlot && theirs[j].item == chosen1.item) jA = j;
            cellA = iA * nTheirs + jA;
        }
        cellB = Next(&x) % (nMine * nTheirs - 1);
        if (cellB >= cellA) cellB++;
        iA = cellA / nTheirs; jA = cellA % nTheirs; iB = cellB / nTheirs; jB = cellB % nTheirs;
        if (ApplyCell(childA, &snaps[t], &mine[iA], &theirs[jA], Next(&x)) < 0 || ApplyCell(childB, &snaps[t], &mine[iB], &theirs[jB], Next(&x)) < 0)
        { pthread_mutex_lock(&sLock); sErrors++; pthread_mutex_unlock(&sLock); continue; }
        Sim_EncodeState(childA, 0, FA, IA);
        Sim_EncodeState(childB, 0, FB, IB);
        agents[0].rng = Next(&x) | 1; agents[1].rng = Next(&x) | 1;
        outA = childA->finished ? (childA->battleOutcome == B_OUTCOME_WON ? 1 : childA->battleOutcome == B_OUTCOME_LOST ? 0 : 2) : PlayOut(childA, agents, NULL, NULL, &dA);
        agents[0].rng = Next(&x) | 1; agents[1].rng = Next(&x) | 1;
        outB = childB->finished ? (childB->battleOutcome == B_OUTCOME_WON ? 1 : childB->battleOutcome == B_OUTCOME_LOST ? 0 : 2) : PlayOut(childB, agents, NULL, NULL, &dB);
        WriteHalf(FA, IA, fhA, ihA); WriteHalf(FB, IB, fhB, ihB);
        memset(meta, 0, sizeof(meta));
        meta[0] = p; meta[1] = t; meta[2] = nSnaps; meta[3] = iA; meta[4] = jA; meta[5] = iB; meta[6] = jB; meta[7] = outA; meta[8] = outB;
        meta[9] = randomBoth; meta[10] = nMine; meta[11] = nTheirs; meta[12] = dA; meta[13] = dB;
        pthread_mutex_lock(&sLock);
        fwrite(meta, sizeof(meta), 1, sOut);
        fwrite(fhA, sizeof(_Float16), SIMENC_FLOATS, sOut); fwrite(ihA, sizeof(int16_t), SIMENC_INTS, sOut);
        fwrite(fhB, sizeof(_Float16), SIMENC_FLOATS, sOut); fwrite(ihB, sizeof(int16_t), SIMENC_INTS, sOut);
        sWritten++;
        sDecisions += decisions + dA + dB;
        if (outA >= 0 && outB >= 0 && outA != outB && outA != 2 && outB != 2) sDiscordant++;
        if (sWritten % 500 == 0) { fprintf(stderr, "\r%d/%d pairs, %d discordant, %d errors", sWritten, sPairs, sDiscordant, sErrors); fflush(stderr); }
        pthread_mutex_unlock(&sLock);
    }
    free(sim); free(snaps); free(childA); free(childB); free(FA); free(FB); free(IA); free(IB); free(fhA); free(fhB); free(ihA); free(ihB);
    return NULL;
}

int main(int argc, char **argv)
{
    const char *teamsPath = NULL, *spec = "rmplus:iters=30,samples=16", *outPath = NULL;
    pthread_t threads[64];
    int i;
    clock_t t0;
    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--teams") && i + 1 < argc) teamsPath = argv[++i];
        else if (!strcmp(argv[i], "--agent") && i + 1 < argc) spec = argv[++i];
        else if (!strcmp(argv[i], "--pairs") && i + 1 < argc) sPairs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) sThreads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) sSeed = (u32)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--random-both") && i + 1 < argc) sRandomBoth = atof(argv[++i]);
        else if (!strcmp(argv[i], "--maxturns") && i + 1 < argc) sMaxTurns = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) outPath = argv[++i];
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (!teamsPath || !outPath) { fprintf(stderr, "--teams and --out are required\n"); return 2; }
    if (sThreads < 1) sThreads = 1;
    if (sThreads > 64) sThreads = 64;
    if (Sim_AgentFromSpec(&sAgent, spec) != 0 || sAgent.isGameAI) { fprintf(stderr, "bad agent: %s\n", sAgent.name); return 2; }
    {
        static struct BattleSim boot;
        Sim_Init(&boot, 0, 1);
    }
    if (LoadTeams(teamsPath) < 2) { fprintf(stderr, "need teams\n"); return 2; }
    sOut = fopen(outPath, "wb");
    if (!sOut) { fprintf(stderr, "cannot write %s\n", outPath); return 2; }
    fprintf(stderr, "branch: %d teams, agent %s, %d pairs, %d threads, seed %u, floats %d ints %d\n", sTeamCount, sAgent.name, sPairs, sThreads, sSeed, SIMENC_FLOATS, SIMENC_INTS);
    t0 = clock();
    for (i = 0; i < sThreads; i++) pthread_create(&threads[i], NULL, Worker, NULL);
    for (i = 0; i < sThreads; i++) pthread_join(threads[i], NULL);
    fclose(sOut);
    fprintf(stderr, "\n%d pairs written, %d discordant, %d errors, %.1f decisions per pair, %.0fs cpu\n", sWritten, sDiscordant, sErrors,
            (double)sDecisions / (sWritten ? sWritten : 1), (double)(clock() - t0) / CLOCKS_PER_SEC);
    return 0;
}
