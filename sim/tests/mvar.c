// Variance of the search across its own randomness: samples turn-start states, runs the mctsf search R times
// per state with different agent seeds, and reports the spread of the root matrix, value and strategy.
//   mvar --teams pool.tsv [--agent "mctsf:nodes=400,kids=8"] [--states 100] [--runs 1000] [--threads 8] [--seed S]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include "global.h"
#include "battle.h"
#include "pokemon.h"
#include "sim.h"
#include "sim_agent.h"
#include "fast.h"
#define MAX_TEAMS 40000
#define MAX_STATES 512
struct Team { char id[64]; u8 doubles; u8 n; struct Pokemon party[PARTY_SIZE]; };
static struct Team *sTeams; static int sTeamCount;
static fs_state sStates[MAX_STATES]; static int sNStates, sRuns = 1000, sThreads = 8; static const char *sSpec = "mctsf:nodes=400,kids=8";
static u32 sX;
static u32 Rnd(void) { sX ^= sX << 13; sX ^= sX >> 17; sX ^= sX << 5; return sX; }
struct Res { int n, m; double cellMean, cellStd, cellStdMax, valMean, valStd, rowTV, colTV, argmaxAgree, strategyEntropy; };
static struct Res sRes[MAX_STATES];
static int sNext; static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;

static void *Worker(void *arg)
{
    struct SimAgent ag;
    float *M = malloc(sizeof(float) * 1024), sRow[32], sCol[32], val;
    double *sum = malloc(sizeof(double) * 1024), *sum2 = malloc(sizeof(double) * 1024), *rowSum = malloc(sizeof(double) * 32), *colSum = malloc(sizeof(double) * 32);
    float *rows = malloc(sizeof(float) * 32 * sRuns), *cols = malloc(sizeof(float) * 32 * sRuns);
    int *argm = malloc(sizeof(int) * sRuns);
    Sim_AgentFromSpec(&ag, sSpec);
    for (;;)
    {
        int si, r, n = 0, m = 0, a, b, ok = 0;
        double vs = 0, vs2 = 0;
        pthread_mutex_lock(&sLock); si = sNext++; pthread_mutex_unlock(&sLock);
        if (si >= sNStates) break;
        memset(sum, 0, sizeof(double) * 1024); memset(sum2, 0, sizeof(double) * 1024); memset(rowSum, 0, sizeof(double) * 32); memset(colSum, 0, sizeof(double) * 32);
        for (r = 0; r < sRuns; r++)
        {
            ag.rng = (u32)(si * 1000003u + r * 2654435761u + 7u) | 1;
            if (Sim_MctsfSearchRoot(&ag, &sStates[si], M, &n, &m, sRow, sCol, &val) != 0) break;
            ok++;
            for (a = 0; a < n * m; a++) { sum[a] += M[a]; sum2[a] += (double)M[a] * M[a]; }
            vs += val; vs2 += (double)val * val;
            for (a = 0; a < n; a++) { rowSum[a] += sRow[a]; rows[r * 32 + a] = sRow[a]; }
            for (b = 0; b < m; b++) { colSum[b] += sCol[b]; cols[r * 32 + b] = sCol[b]; }
            { int best = 0; for (a = 1; a < n; a++) if (sRow[a] > sRow[best]) best = a; argm[r] = best; }
        }
        {
            struct Res *R = &sRes[si];
            double cs = 0, cmax = 0, cm = 0, tv = 0, tvc = 0, ent = 0; int cnt = 0, agree = 0, mode = 0, modeN = 0;
            memset(R, 0, sizeof(*R));
            if (!ok) { R->n = -1; continue; }
            R->n = n; R->m = m;
            for (a = 0; a < n * m; a++) { double mu = sum[a] / ok, var = sum2[a] / ok - mu * mu; if (var < 0) var = 0; cs += sqrt(var); cm += fabs(mu); if (sqrt(var) > cmax) cmax = sqrt(var); cnt++; }
            R->cellStd = cs / cnt; R->cellMean = cm / cnt; R->cellStdMax = cmax;
            R->valMean = vs / ok; { double v = vs2 / ok - R->valMean * R->valMean; R->valStd = sqrt(v < 0 ? 0 : v); }
            // mean total-variation distance of each run's strategy from the mean strategy; entropy of the mean strategy
            for (r = 0; r < ok; r++) { double d = 0, dc = 0; for (a = 0; a < n; a++) d += fabs(rows[r * 32 + a] - rowSum[a] / ok); for (b = 0; b < m; b++) dc += fabs(cols[r * 32 + b] - colSum[b] / ok); tv += d / 2; tvc += dc / 2; }
            R->rowTV = tv / ok; R->colTV = tvc / ok;
            for (a = 0; a < n; a++) { double p = rowSum[a] / ok; if (p > 1e-9) ent -= p * log(p); }
            R->strategyEntropy = ent;
            for (a = 0; a < n; a++) { int c = 0; for (r = 0; r < ok; r++) if (argm[r] == a) c++; if (c > modeN) { modeN = c; mode = a; } }
            agree = modeN; R->argmaxAgree = (double)agree / ok;
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    const char *teamsPath = NULL; int nStates = 100, i, g = 0; u32 seed = 1;
    pthread_t th[64];
    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--teams") && i + 1 < argc) teamsPath = argv[++i];
        else if (!strcmp(argv[i], "--agent") && i + 1 < argc) sSpec = argv[++i];
        else if (!strcmp(argv[i], "--states") && i + 1 < argc) nStates = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--runs") && i + 1 < argc) sRuns = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) sThreads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (u32)strtoul(argv[++i], NULL, 0);
    }
    if (!teamsPath) return 2;
    if (nStates > MAX_STATES) nStates = MAX_STATES;
    { static struct BattleSim boot; Sim_Init(&boot, 0, 1); }
    { FILE *f = fopen(teamsPath, "r"); if (!f) return 2; sTeams = calloc(MAX_TEAMS, sizeof(*sTeams));
      for (;;) { struct Team *t = &sTeams[sTeamCount]; int n = Sim_ReadTeamTsv(f, t->party, t->id, sizeof(t->id), &t->doubles); if (n == 0) break; t->n = n; if (!t->doubles && ++sTeamCount >= MAX_TEAMS) break; } fclose(f); }
    sX = seed * 2654435761u + 12345;
    // sample turn-start states from rmplus:10 self-play, 1 in 6 turn starts
    {
        struct BattleSim *sim = malloc(sizeof(*sim)); struct SimAgent ag; Sim_AgentFromSpec(&ag, "rmplus:iters=10");
        while (sNStates < nStates)
        {
            int ta = Rnd() % sTeamCount, tb = Rnd() % sTeamCount, guard = 0; if (tb == ta) tb = (ta + 1) % sTeamCount;
            Sim_Init(sim, BATTLE_TYPE_TRAINER, 1);
            memcpy(sim->playerParty, sTeams[ta].party, sizeof(sim->playerParty)); memcpy(sim->enemyParty, sTeams[tb].party, sizeof(sim->enemyParty));
            sim->createTrainerParty = FALSE; sim->trainerBattleOpponent_A = Sim_TrainerForAIFlags(7); if (!sim->trainerBattleOpponent_A) sim->trainerBattleOpponent_A = 1;
            sim->rngXorshift = 1; sim->rngValue = Rnd() | 1; sim->strictAnswers = 1; sim->maxTurns = 300; sim->badgeFlags = 0; sim->exactFrames = 0;
            ag.rng = Rnd() | 1; g++;
            if (Sim_Start(sim) != 0) continue;
            for (;;)
            {
                int r = Sim_Run(sim); u8 b, kind; struct SimAction act;
                if (r != SIM_RUN_REQUEST || ++guard > 2000) break;
                b = sim->requestBattler; kind = sim->requestKind;
                if (kind == SIM_REQ_ACTION && b == 0 && Rnd() % 6 == 0 && sNStates < nStates && fs_import(&sStates[sNStates], sim) == 0) sNStates++;
                ag.decide(&ag, sim, (kind == SIM_REQ_ACTION && b == 0) ? sim : NULL, b, kind, &act);
                if (Sim_Answer(sim, b, &act) != 0) break;
            }
        }
    }
    fprintf(stderr, "mvar: %d states from %d games, %d runs each of %s on %d threads\n", sNStates, g, sRuns, sSpec, sThreads);
    for (i = 0; i < sThreads; i++) pthread_create(&th[i], NULL, Worker, NULL);
    for (i = 0; i < sThreads; i++) pthread_join(th[i], NULL);
    {
        double cs = 0, cm = 0, cmax = 0, vsd = 0, tv = 0, tvc = 0, agree = 0, ent = 0; int k = 0;
        printf("state  n x m  |mean cell|  cell std  max cell std  value mean  value std  rowTV  colTV  argmax agree  H(row)\n");
        for (i = 0; i < sNStates; i++)
        {
            struct Res *R = &sRes[i];
            if (R->n < 0) continue;
            printf("%5d  %dx%d  %9.3f  %8.4f  %12.4f  %10.3f  %9.4f  %5.3f  %5.3f  %11.2f  %5.2f\n", i, R->n, R->m, R->cellMean, R->cellStd, R->cellStdMax, R->valMean, R->valStd, R->rowTV, R->colTV, R->argmaxAgree, R->strategyEntropy);
            cs += R->cellStd; cm += R->cellMean; cmax += R->cellStdMax; vsd += R->valStd; tv += R->rowTV; tvc += R->colTV; agree += R->argmaxAgree; ent += R->strategyEntropy; k++;
        }
        printf("\nsummary over %d states, %d runs each (%s):\n  mean |cell| %.3f, mean cell std %.4f (max-cell std %.4f), root value std %.4f, strategy TV from its mean: row %.3f col %.3f, argmax agreement %.2f, mean row entropy %.2f nats\n",
               k, sRuns, sSpec, cm / k, cs / k, cmax / k, vsd / k, tv / k, tvc / k, agree / k, ent / k);
    }
    return 0;
}
