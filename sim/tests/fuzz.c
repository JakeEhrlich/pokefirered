// Fuzz test: random parties from the full species/move/item pool, random legal actions, many battles.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "global.h"
#include "battle.h"
#include "pokemon.h"
#include "sim.h"
#include "sim_names.h"
#include "battle_scripts.h"
#include "constants/species.h"
#include "constants/moves.h"
#include "constants/items.h"
#include "constants/pokemon.h"

static u32 sRng = 12345;
static u32 R(void) { sRng = sRng * 1103515245 + 12345; return sRng >> 8; }

static u16 RandomSpecies(void)
{
    u16 s;
    do
        s = 1 + R() % (NUM_SPECIES - 1);
    while ((s > SPECIES_CELEBI && s < SPECIES_TREECKO) || s == SPECIES_EGG || s == SPECIES_NONE); // skip old unown gap
    return s;
}

static void RandomMon(struct Pokemon *mon)
{
    u16 moves[4];
    u8 ivs[6], evs[6];
    int i;
    for (i = 0; i < 4; i++)
        moves[i] = 1 + R() % (MOVES_COUNT - 1);
    for (i = 0; i < 6; i++) { ivs[i] = R() % 32; evs[i] = R() % 86; }
    Sim_MakeMon(mon, RandomSpecies(), 5 + R() % 96, R() % NUM_NATURES, ivs, evs, moves,
                (R() % 3 == 0) ? ITEM_NONE : 1 + R() % (ITEMS_COUNT - 1), R() % 2);
}

int main(int argc, char **argv)
{
    static struct BattleSim sim;
    int n = argc > 1 ? atoi(argv[1]) : 2000;
    int seed = argc > 2 ? atoi(argv[2]) : 1;
    int i, k, res;
    int outcomes[16] = {0};
    long totalTurns = 0;
    clock_t t0 = clock();

    sRng = seed;
    for (i = 0; i < n; i++)
    {
        u32 flags = BATTLE_TYPE_TRAINER | ((R() % 4 == 0) ? BATTLE_TYPE_DOUBLE : 0);
        int np = 1 + R() % 6, ne = 1 + R() % 6;
        if (flags & BATTLE_TYPE_DOUBLE) { if (np < 2) np = 2; if (ne < 2) ne = 2; }
        Sim_Init(&sim, flags, R());
        for (k = 0; k < np; k++) RandomMon(&sim.playerParty[k]);
        for (k = 0; k < ne; k++) RandomMon(&sim.enemyParty[k]);
        Sim_SetPolicy(&sim, B_SIDE_PLAYER, Sim_RandomPolicy);
        Sim_SetPolicy(&sim, B_SIDE_OPPONENT, Sim_RandomPolicy);
        sim.logEnabled = TRUE;
        Sim_Start(&sim);
        res = Sim_Run(&sim);
        if (res != SIM_RUN_FINISHED)
        {
            printf("battle %d (seed %d): res %d after %d turns, %u frames, flags %x\n", i, seed, res, sim.turnCount, sim.frames, flags);
            Sim_PrintBattlers(&sim);
            Sim_PrintLog(&sim);
            printf("  script offset %ld mainfunc %p execflags %x\n", (long)(sim.battlescriptCurrInstr - gBattleScriptBlob), (void *)sim.battleMainFunc, sim.battleControllerExecFlags);
            return 1;
        }
        outcomes[sim.battleOutcome & 0xF]++;
        totalTurns += sim.turnCount;
    }
    printf("%d battles ok in %.2fs; won %d lost %d drew %d other %d; avg turns %.1f\n", n,
           (double)(clock() - t0) / CLOCKS_PER_SEC, outcomes[B_OUTCOME_WON], outcomes[B_OUTCOME_LOST], outcomes[B_OUTCOME_DREW],
           n - outcomes[B_OUTCOME_WON] - outcomes[B_OUTCOME_LOST] - outcomes[B_OUTCOME_DREW], (double)totalTurns / n);
    return 0;
}
