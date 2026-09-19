// Sweep every in-game trainer with the vanilla AI against a random player party (exercises the AI code paths).
#include <stdio.h>
#include <stdlib.h>
#include "global.h"
#include "battle.h"
#include "pokemon.h"
#include "sim.h"
#include "constants/species.h"
#include "constants/moves.h"
#include "constants/items.h"
#include "constants/pokemon.h"
#include "constants/opponents.h"

static u32 sRng = 99;
static u32 R(void) { sRng = sRng * 1103515245 + 12345; return sRng >> 8; }

int main(int argc, char **argv)
{
    static struct BattleSim sim;
    int per = argc > 1 ? atoi(argv[1]) : 2;
    int t, i, k, played = 0, wins = 0;
    for (t = 1; t < 743; t++)
    {
        for (i = 0; i < per; i++)
        {
            int res;
            Sim_Init(&sim, BATTLE_TYPE_TRAINER, t * 7 + i);
            Sim_LoadTrainerParty(&sim, t);
            if (sim.enemyPartyCount == 0 && GetMonData(&sim.enemyParty[0], MON_DATA_SPECIES) == SPECIES_NONE)
                break;
            {
                int np = 1 + R() % 6;
                for (k = 0; k < np; k++)
                {
                    u16 moves[4];
                    int m;
                    for (m = 0; m < 4; m++) moves[m] = 1 + R() % (MOVES_COUNT - 1);
                    Sim_MakeMon(&sim.playerParty[k], 1 + R() % 151, 10 + R() % 60, R() % NUM_NATURES, NULL, NULL, moves, ITEM_NONE, R() % 2);
                }
                if ((sim.battleTypeFlags & BATTLE_TYPE_DOUBLE) && np < 2)
                    Sim_MakeMon(&sim.playerParty[1], SPECIES_RATTATA, 20, 0, NULL, NULL, NULL, ITEM_NONE, 0);
            }
            Sim_SetPolicy(&sim, B_SIDE_PLAYER, Sim_RandomPolicy);
            Sim_SetPolicy(&sim, B_SIDE_OPPONENT, Sim_VanillaAIPolicy);
            if (Sim_Start(&sim) != 0)
                break; // placeholder trainer with no party
            res = Sim_Run(&sim);
            if (res != SIM_RUN_FINISHED)
            {
                printf("trainer %d battle %d: res %d\n", t, i, res);
                return 1;
            }
            played++;
            if (sim.battleOutcome == B_OUTCOME_WON) wins++;
        }
    }
    printf("all trainers: %d battles ok, player won %d\n", played, wins);
    return 0;
}
