// Runs every scenario in the simulator and evaluates its checks.  scenarios [name-substring]
#include <string.h>
#include <stdlib.h>
#include "scenario.h"

int main(int argc, char **argv)
{
    static struct BattleSim sim;
    const char *filter = argc > 1 ? argv[1] : NULL;
    int g, i, ran = 0;
    for (g = 0; g < gScenarioGroupCount; g++)
    {
        const struct ScenarioGroup *grp = gScenarioGroups[g];
        for (i = 0; i < grp->count; i++)
        {
            const struct Scenario *sc = &grp->scenarios[i];
            u16 seed;
            int res;
            if (filter && !strstr(sc->name, filter) && !strstr(grp->name, filter))
                continue;
            gScenarioCurrent = sc->name;
            seed = Scenario_ResolveSeed(&sim, sc);
            res = Scenario_RunSim(&sim, sc, seed);
            if (res == -1) { CHECK(0, "invalid setup"); continue; }
            if (res == SIM_RUN_STUCK) { CHECK(0, "simulator stuck"); continue; }
            if (sc->check)
                sc->check(&sim);
            if (getenv("SCENARIO_VERBOSE"))
            {
                printf("--- %s/%s (seed %#x, %d turns)\n", grp->name, sc->name, seed, sim.turnCount);
                Sim_PrintLog(&sim);
                Sim_PrintBattlers(&sim);
            }
            ran++;
        }
    }
    printf("scenarios: %d run, %d checks, %d failures\n", ran, gScenarioChecks, gScenarioFailures);
    return gScenarioFailures != 0;
}
