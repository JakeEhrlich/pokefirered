// Samples turn-start positions from randbats games and writes tempo features with V0 and the one-turn lookahead
// value V1 (RM+ 30 over a K-sample joint matrix of V0 on the fast engine), side 0's view, as CSV.
//   vfdata --teams pool.tsv [--states N] [--samples K] [--seed S] > out.csv
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "global.h"
#include "battle.h"
#include "pokemon.h"
#include "sim.h"
#include "sim_agent.h"
#include "fast.h"
#include <time.h>
static double sPlyT; static long sPlyN;
static double Now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
#define MAX_TEAMS 40000
struct Team { char id[64]; u8 doubles; u8 n; struct Pokemon party[PARTY_SIZE]; };
static struct Team *sTeams; static int sTeamCount;
static u32 sX;
static u32 Rnd(void) { sX ^= sX << 13; sX ^= sX >> 17; sX ^= sX << 5; return sX; }

static float Lookahead(const fs_state *s, int K)
{
    fs_action fl[2][32]; float M[32 * 32], sRow[32], sCol[32]; int n[2], a, b, k, side; double v = 0;
    if (s->request == FS_REQ_DONE) return fs_value_basic(s, 0);
    for (side = 0; side < 2; side++) { n[side] = fs_legal_actions(s, side, fl[side]); if (n[side] <= 0) { n[side] = 1; fl[side][0].type = FS_ACT_MOVE; fl[side][0].slot = 0; } }
    for (a = 0; a < n[0]; a++) for (b = 0; b < n[1]; b++)
    {
        double sum = 0;
        for (k = 0; k < K; k++) { fs_state w = *s; fs_seed(&w, Rnd() | 1); fs_step(&w, fl[0][a], fl[1][b]); sum += fs_value_basic(&w, 0); }
        M[a * n[1] + b] = (float)(sum / K);
    }
    Sim_RegretMatching(M, n[0], n[1], 30, 1, 1, 1, sRow, sCol);
    for (a = 0; a < n[0]; a++) for (b = 0; b < n[1]; b++) v += sRow[a] * M[a * n[1] + b] * sCol[b];
    return (float)v;
}

int main(int argc, char **argv)
{
    const char *teamsPath = NULL; int nStates = 20000, K = 16, i, done = 0, g = 0; u32 seed = 1;
    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--teams") && i + 1 < argc) teamsPath = argv[++i];
        else if (!strcmp(argv[i], "--states") && i + 1 < argc) nStates = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--samples") && i + 1 < argc) K = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (u32)strtoul(argv[++i], NULL, 0);
    }
    if (!teamsPath) return 2;
    { static struct BattleSim boot; Sim_Init(&boot, 0, 1); }
    {
        FILE *f = fopen(teamsPath, "r"); if (!f) return 2;
        sTeams = calloc(MAX_TEAMS, sizeof(*sTeams));
        for (;;) { struct Team *t = &sTeams[sTeamCount]; int n = Sim_ReadTeamTsv(f, t->party, t->id, sizeof(t->id), &t->doubles); if (n == 0) break; t->n = n; if (!t->doubles && ++sTeamCount >= MAX_TEAMS) break; }
        fclose(f);
    }
    sX = seed * 2654435761u + 12345;
    printf("turn");
    for (i = 0; i < FS_TEMPO_NF; i++) printf(",f%d", i);
    printf(",v0,v1,ply1\n");
    {
        struct BattleSim *sim = malloc(sizeof(*sim));
        struct SimAgent ag; Sim_AgentFromSpec(&ag, "rmplus:iters=10");
        while (done < nStates)
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
                if (kind == SIM_REQ_ACTION && b == 0 && Rnd() % 4 == 0 && done < nStates)
                {
                    fs_state fs; float f[FS_TEMPO_NF];
                    if (fs_import(&fs, sim) == 0)
                    {
                        float v1 = Lookahead(&fs, K);
                        fs_tempo_features_sym(&fs, 0, f);
                        printf("%u", sim->turnCount);
                        for (i = 0; i < FS_TEMPO_NF; i++) printf(",%.5f", f[i]);
                        { double t0 = Now(); float p1 = fs_value_ply1(&fs, 0); sPlyT += Now() - t0; sPlyN++; printf(",%.5f,%.5f,%.5f\n", fs_value_basic(&fs, 0), v1, p1); }
                        done++;
                    }
                }
                ag.decide(&ag, sim, (kind == SIM_REQ_ACTION && b == 0) ? sim : NULL, b, kind, &act);
                if (Sim_Answer(sim, b, &act) != 0) break;
            }
        }
        fprintf(stderr, "vfdata: %d states from %d games; ply1 %.2f us each\n", done, g, sPlyN ? 1e6 * sPlyT / sPlyN : 0);
    }
    return 0;
}
