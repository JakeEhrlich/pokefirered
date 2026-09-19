#include <stdio.h>
#include <string.h>
#include "global.h"
#include "battle.h"
#include "pokemon.h"
#include "sim.h"
#include "constants/species.h"
#include "constants/moves.h"
#include "constants/items.h"
#include "constants/pokemon.h"

static void PrintBattlers(struct BattleSim *sim)
{
    int i;
    for (i = 0; i < sim->battlersCount; i++)
        printf("  battler %d: species %d hp %d/%d status1 %x\n", i, sim->battleMons[i].species,
               sim->battleMons[i].hp, sim->battleMons[i].maxHP, sim->battleMons[i].status1);
}

int main(int argc, char **argv)
{
    static struct BattleSim sim;
    static const u16 movesA[4] = { MOVE_TACKLE, MOVE_GROWL, MOVE_VINE_WHIP, MOVE_LEECH_SEED };
    static const u16 movesB[4] = { MOVE_SCRATCH, MOVE_EMBER, MOVE_GROWL, MOVE_TAIL_WHIP };
    int res;
    int wins = 0, n = 0, i;
    u32 seed = 1;

    printf("state size %zu\n", sizeof(sim));
    for (n = 0; n < 200; n++)
    {
        Sim_Init(&sim, BATTLE_TYPE_TRAINER, seed + n);
        Sim_MakeMon(&sim.playerParty[0], SPECIES_BULBASAUR, 20, NATURE_HARDY, NULL, NULL, movesA, ITEM_NONE, 0);
        Sim_MakeMon(&sim.playerParty[1], SPECIES_SQUIRTLE, 20, NATURE_HARDY, NULL, NULL, movesA, ITEM_NONE, 0);
        Sim_MakeMon(&sim.enemyParty[0], SPECIES_CHARMANDER, 20, NATURE_HARDY, NULL, NULL, movesB, ITEM_NONE, 0);
        Sim_MakeMon(&sim.enemyParty[1], SPECIES_PIDGEY, 20, NATURE_HARDY, NULL, NULL, movesB, ITEM_NONE, 0);
        Sim_SetPolicy(&sim, B_SIDE_PLAYER, Sim_RandomPolicy);
        Sim_SetPolicy(&sim, B_SIDE_OPPONENT, Sim_RandomPolicy);
        sim.logEnabled = (n == 0);
        Sim_Start(&sim);
        res = Sim_Run(&sim);
        if (n == 0)
        {
            printf("result %d outcome %d turns %d frames %u\n", res, sim.battleOutcome, sim.battleResults.battleTurnCounter, sim.frames);
            PrintBattlers(&sim);
            for (i = 0; i < sim.logCount; i++)
                printf("  log: string %d battler %d ms %d\n", sim.log[i].stringId, sim.log[i].battler, sim.log[i].multistring);
        }
        if (res != SIM_RUN_FINISHED)
        {
            printf("battle %d did not finish: res %d\n", n, res);
            return 1;
        }
        if (sim.battleOutcome == B_OUTCOME_WON)
            wins++;
    }
    printf("player won %d / %d\n", wins, n);
    return 0;
}
