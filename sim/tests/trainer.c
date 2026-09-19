// Play a fixed player team against a real in-game trainer using the game's own AI.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "global.h"
#include "battle.h"
#include "pokemon.h"
#include "sim.h"
#include "sim_names.h"
#include "constants/species.h"
#include "constants/moves.h"
#include "constants/items.h"
#include "constants/pokemon.h"
#include "constants/opponents.h"

int main(int argc, char **argv)
{
    static struct BattleSim sim;
    u16 trainer = argc > 1 ? atoi(argv[1]) : TRAINER_LEADER_BROCK;
    int n = argc > 2 ? atoi(argv[2]) : 200;
    int i, wins = 0, res;
    static const u16 movesA[4] = { MOVE_WATER_GUN, MOVE_TACKLE, MOVE_BUBBLE, MOVE_WITHDRAW };
    static const u16 movesB[4] = { MOVE_VINE_WHIP, MOVE_TACKLE, MOVE_LEECH_SEED, MOVE_GROWL };
    static const u16 movesC[4] = { MOVE_GUST, MOVE_TACKLE, MOVE_SAND_ATTACK, MOVE_QUICK_ATTACK };

    for (i = 0; i < n; i++)
    {
        Sim_Init(&sim, BATTLE_TYPE_TRAINER, 1000 + i);
        Sim_MakeMon(&sim.playerParty[0], SPECIES_SQUIRTLE, 14, NATURE_MODEST, NULL, NULL, movesA, ITEM_NONE, 0);
        Sim_MakeMon(&sim.playerParty[1], SPECIES_BULBASAUR, 13, NATURE_HARDY, NULL, NULL, movesB, ITEM_NONE, 0);
        Sim_MakeMon(&sim.playerParty[2], SPECIES_PIDGEY, 12, NATURE_JOLLY, NULL, NULL, movesC, ITEM_NONE, 0);
        Sim_LoadTrainerParty(&sim, trainer);
        Sim_SetPolicy(&sim, B_SIDE_PLAYER, Sim_RandomPolicy);
        Sim_SetPolicy(&sim, B_SIDE_OPPONENT, Sim_VanillaAIPolicy);
        sim.logEnabled = (i == 0);
        if (Sim_Start(&sim) != 0)
        {
            printf("invalid setup\n");
            return 1;
        }
        res = Sim_Run(&sim);
        if (i == 0)
        {
            int k;
            printf("trainer %d party:", trainer);
            for (k = 0; k < sim.enemyPartyCount; k++)
                printf(" %s L%d", gSimSpeciesNames[GetMonData(&sim.enemyParty[k], MON_DATA_SPECIES)], GetMonData(&sim.enemyParty[k], MON_DATA_LEVEL));
            printf("\nfirst battle: res %d outcome %d turns %d\n", res, sim.battleOutcome, sim.turnCount);
            Sim_PrintBattlers(&sim);
            Sim_PrintLog(&sim);
        }
        if (res != SIM_RUN_FINISHED)
        {
            printf("battle %d did not finish (res %d)\n", i, res);
            return 1;
        }
        if (sim.battleOutcome == B_OUTCOME_WON)
            wins++;
    }
    printf("random player vs trainer %d (vanilla AI): won %d / %d\n", trainer, wins, n);
    return 0;
}
