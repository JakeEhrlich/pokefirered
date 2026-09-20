#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "global.h"
#include "battle.h"
#include "pokemon.h"
#include "sim.h"
#include "sim_agent.h"
#include "fast.h"
struct Team { char id[64]; u8 doubles; u8 n; struct Pokemon party[PARTY_SIZE]; };
static struct Team sTeams[4000]; static int sTeamCount;
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "r"); { static struct BattleSim boot; Sim_Init(&boot, 0, 1); }
    for (;;) { struct Team *t = &sTeams[sTeamCount]; int n = Sim_ReadTeamTsv(f, t->party, t->id, sizeof(t->id), &t->doubles); if (n == 0) break; t->n = n; if (!t->doubles && ++sTeamCount >= 4000) break; }
    fclose(f);
    struct BattleSim *sim = malloc(sizeof(*sim)); fs_state fs, work; u32 x = 12345; long turns = 0, fsTurns = 0, unsup = 0; double tv = 0, tf = 0;
    for (int g = 0; g < 200; g++)
    {
        int ta = (x = x * 1103515245 + 12345) >> 16 & 4095, tb = (x = x * 1103515245 + 12345) >> 16 & 4095; ta %= sTeamCount; tb %= sTeamCount; if (tb == ta) tb = (ta + 1) % sTeamCount;
        Sim_Init(sim, BATTLE_TYPE_TRAINER, 1); memcpy(sim->playerParty, sTeams[ta].party, 600); memcpy(sim->enemyParty, sTeams[tb].party, 600);
        sim->createTrainerParty = FALSE; sim->trainerBattleOpponent_A = Sim_TrainerForAIFlags(7); sim->rngXorshift = 1; sim->rngValue = x | 1; sim->strictAnswers = 0; sim->maxTurns = 300; sim->badgeFlags = 0; sim->exactFrames = 0;
        if (Sim_Start(sim) != 0) continue;
        if (Sim_Run(sim) != SIM_RUN_REQUEST) continue;
        if (fs_import(&fs, sim) != 0) { unsup++; continue; }
        // verbatim: random legal joint actions from this turn start, many samples of one turn
        struct SimAction m0[32], m1[32]; int n0 = Sim_LegalActions(sim, 0, m0, 32), n1 = Sim_LegalActions(sim, 1, m1, 32);
        static struct BattleSim clone;
        double t0 = now();
        for (int k = 0; k < 500; k++)
        {
            memcpy(&clone, sim, sizeof(clone)); clone.rngValue = (x = x * 1103515245 + 12345) | 1;
            Sim_Answer(&clone, 0, &m0[k % n0]); int r = Sim_Run(&clone);
            if (r == SIM_RUN_REQUEST && clone.requestBattler == 1 && clone.requestKind == SIM_REQ_ACTION) { Sim_Answer(&clone, 1, &m1[k % n1]); Sim_Run(&clone); }
            turns++;
        }
        tv += now() - t0;
        fs_action f0[32], f1[32]; int q0 = fs_legal_actions(&fs, 0, f0), q1 = fs_legal_actions(&fs, 1, f1);
        t0 = now();
        for (int k = 0; k < 500; k++) { work = fs; fs_seed(&work, (x = x * 1103515245 + 12345) | 1); fs_step(&work, f0[k % q0], f1[k % q1]); fsTurns++; }
        tf += now() - t0;
    }
    printf("verbatim: %.2f us/turn (%ld turns)   fast: %.3f us/turn (%ld turns)   speedup %.1fx   state %zu bytes vs %zu   unsupported positions %ld\n",
           1e6 * tv / turns, turns, 1e6 * tf / fsTurns, fsTurns, (tv / turns) / (tf / fsTurns), sizeof(fs_state), sizeof(struct BattleSim), unsup);
    return 0;
}
