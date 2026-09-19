// Legality checker / helper for the team corpus (teams/FORMAT.md). Reads TSV rows on stdin
// (tools/teams.py tsv) and prints one verdict per team:  TEAM <id> ok | encodable: why | invalid: why
//   --moves SPECIES LEVEL      moves the species can legally know at that level
//   --abilities SPECIES        the species' abilities
//   --items                    holdable items
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "global.h"
#include "pokemon.h"
#include "sim.h"
#include "sim_names.h"
#include "sim_items.h"
#include "constants/species.h"
#include "constants/moves.h"
#include "constants/items.h"
#include "constants/global.h"

static int FindName(const char *const *names, int count, const char *name)
{
    int i;
    char *end;
    long v = strtol(name, &end, 10);
    if (*end == 0 && end != name) return (int)v;
    for (i = 0; i < count; i++)
        if (!strcasecmp(names[i], name)) return i;
    return -1;
}

int main(int argc, char **argv)
{
    static struct BattleSim sim;
    static char line[2048], why[512], teamWhy[2048], curTeam[64];
    int i, verdict = 2, nTeams = 0, nMons = 0;

    Sim_Init(&sim, 0, 1); // binds the engine tables
    if (argc >= 4 && !strcmp(argv[1], "--moves"))
    {
        static u16 out[MOVES_COUNT];
        int sp = FindName(gSimSpeciesNames, gSimSpeciesNames_Count, argv[2]), n;
        if (sp <= 0) { printf("unknown species %s\n", argv[2]); return 2; }
        n = Sim_LearnableMoves(sp, atoi(argv[3]), out, MOVES_COUNT);
        for (i = 0; i < n; i++) printf("%s\n", gSimMoveNames[out[i]]);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "--abilities"))
    {
        int sp = FindName(gSimSpeciesNames, gSimSpeciesNames_Count, argv[2]);
        if (sp <= 0) { printf("unknown species %s\n", argv[2]); return 2; }
        printf("%s\n", gSimAbilityNames[gSpeciesInfo[sp].abilities[0]]);
        if (gSpeciesInfo[sp].abilities[1]) printf("%s\n", gSimAbilityNames[gSpeciesInfo[sp].abilities[1]]);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "--items"))
    {
        for (i = 1; i < ITEMS_COUNT; i++)
            if (gSimItems[i].name && gSimItems[i].pocket != POCKET_KEY_ITEMS && gSimItems[i].name[0] != '?')
                printf("%s\n", gSimItemNames[i]);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "--species"))
    {
        for (i = 1; i < NUM_SPECIES; i++) printf("%s\n", gSimSpeciesNames[i]);
        return 0;
    }

    curTeam[0] = 0; teamWhy[0] = 0;
    for (;;)
    {
        struct SimTeamMon m;
        int have = fgets(line, sizeof(line), stdin) != NULL;
        if (!have || !Sim_ParseTeamRow(line, &m) || strcmp(m.teamId, curTeam) != 0)
        {
            if (curTeam[0])
            {
                printf("TEAM %s %s%s%s\n", curTeam, verdict == 2 ? "ok" : verdict == 1 ? "encodable" : "invalid", teamWhy[0] ? ": " : "", teamWhy);
                nTeams++;
            }
            if (!have) break;
            if (!Sim_ParseTeamRow(line, &m)) { printf("TEAM ? invalid: unparsable row\n"); curTeam[0] = 0; continue; }
            snprintf(curTeam, sizeof(curTeam), "%s", m.teamId);
            verdict = 2; teamWhy[0] = 0;
        }
        {
            int v = Sim_CheckTeamMon(&m, why, sizeof(why));
            nMons++;
            if (v < verdict) verdict = v;
            if (why[0] && strlen(teamWhy) + strlen(why) + 16 < sizeof(teamWhy))
            {
                if (teamWhy[0]) strcat(teamWhy, " | ");
                sprintf(teamWhy + strlen(teamWhy), "[%d] %s", m.slot, why);
            }
        }
    }
    fprintf(stderr, "teamcheck: %d teams, %d mons\n", nTeams, nMons);
    return 0;
}
