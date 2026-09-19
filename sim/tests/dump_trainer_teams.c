// Dump every in-game trainer party as one JSONL team (teams/FORMAT.md), exactly as the game builds it:
// Sim_LoadTrainerParty runs the engine's CreateNPCTrainerParty, so species/level/moves/item/IVs/nature/
// ability slot/friendship are read back from the real struct Pokemon with GetMonData.
//   usage: build/dump_trainer_teams [repo_root] > teams/game_trainers.jsonl     (default root: ..)
// Trainer constant names come from include/constants/opponents.h, class names from
// include/constants/trainers.h (parsed at run time), everything else from gTrainers[] and the party.
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
#include "constants/trainers.h"
#include "characters.h"

#define MAX_IDS 1024

static const char *const sNatureNames[NUM_NATURES] = {
    "HARDY", "LONELY", "BRAVE", "ADAMANT", "NAUGHTY", "BOLD", "DOCILE", "RELAXED", "IMPISH", "LAX",
    "TIMID", "HASTY", "SERIOUS", "JOLLY", "NAIVE", "MODEST", "MILD", "QUIET", "BASHFUL", "RASH",
    "CALM", "GENTLE", "SASSY", "CAREFUL", "QUIRKY",
};

static char *sTrainerNames[MAX_IDS];
static char *sClassNames[256];

// Reads "#define <prefix>NAME  <number>" lines; stores NAME (prefix stripped) at out[number].
static void LoadDefines(const char *path, const char *prefix, char **out, int max)
{
    static char line[512];
    FILE *f = fopen(path, "r");
    size_t plen = strlen(prefix);
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    while (fgets(line, sizeof(line), f))
    {
        char name[128];
        long v;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "#define ", 8) != 0) continue;
        p += 8;
        while (*p == ' ') p++;
        if (strncmp(p, prefix, plen) != 0) continue;
        if (sscanf(p + plen, "%127s %ld", name, &v) != 2) continue;
        if (v < 0 || v >= max || out[v]) continue;
        out[v] = strdup(name);
    }
    fclose(f);
}

// Decode a game-charset string (the subset used by trainer names) to ASCII.
static void DecodeName(const u8 *s, char *out, int max)
{
    int n = 0;
    while (*s != EOS && n < max - 1)
    {
        u8 c = *s++;
        if (c == 0x00) out[n++] = ' ';
        else if (c >= 0xA1 && c <= 0xAA) out[n++] = '0' + (c - 0xA1);
        else if (c >= 0xBB && c <= 0xD4) out[n++] = 'A' + (c - 0xBB);
        else if (c >= 0xD5 && c <= 0xEE) out[n++] = 'a' + (c - 0xD5);
        else if (c == 0xAB) out[n++] = '!';
        else if (c == 0xAC) out[n++] = '?';
        else if (c == 0xAD) out[n++] = '.';
        else if (c == 0xAE) out[n++] = '-';
        else if (c == 0xB8) out[n++] = ',';
        else if (c == 0xBA) out[n++] = '/';
        else if (c == 0xB3 || c == 0xB4) out[n++] = '\'';
        else out[n++] = '?';
    }
    out[n] = 0;
}

static void Lower(char *s) { for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s += 'a' - 'A'; }

int main(int argc, char **argv)
{
    static struct BattleSim sim;
    const char *root = argc > 1 ? argv[1] : "..";
    char path[1024];
    int t, i, k, nTeams = 0, nMons = 0, nAbilityFallback = 0;

    snprintf(path, sizeof(path), "%s/include/constants/opponents.h", root);
    LoadDefines(path, "TRAINER_", sTrainerNames, MAX_IDS);
    snprintf(path, sizeof(path), "%s/include/constants/trainers.h", root);
    LoadDefines(path, "TRAINER_CLASS_", sClassNames, 256);

    for (t = 1; t < NUM_TRAINERS; t++)
    {
        const struct Trainer *tr = &gTrainers[t];
        char tname[32], cls[64], clsTag[64];
        u8 n, levelCap = 0;
        int isRival, isRematch;

        Sim_Init(&sim, BATTLE_TYPE_TRAINER, 1);
        n = Sim_LoadTrainerParty(&sim, t);
        if (n == 0 || tr->partySize == 0 || GetMonData(&sim.enemyParty[0], MON_DATA_SPECIES) == SPECIES_NONE)
        {
            fprintf(stderr, "skip trainer %d (%s): no party\n", t, sTrainerNames[t] ? sTrainerNames[t] : "?");
            continue;
        }
        if (!sTrainerNames[t]) { fprintf(stderr, "trainer %d has no constant name\n", t); return 2; }

        DecodeName(tr->trainerName, tname, sizeof(tname));
        snprintf(cls, sizeof(cls), "%s", sClassNames[tr->trainerClass] ? sClassNames[tr->trainerClass] : "UNKNOWN");
        snprintf(clsTag, sizeof(clsTag), "%s", cls);
        Lower(clsTag);
        isRival = tr->trainerClass == TRAINER_CLASS_RIVAL_EARLY || tr->trainerClass == TRAINER_CLASS_RIVAL_LATE
               || tr->trainerClass == TRAINER_CLASS_CHAMPION;
        isRematch = strstr(sTrainerNames[t], "REMATCH") != NULL || strstr(sTrainerNames[t], "_2") != NULL
                 || strstr(sTrainerNames[t], "_3") != NULL || strstr(sTrainerNames[t], "_4") != NULL
                 || strstr(sTrainerNames[t], "_5") != NULL;

        for (i = 0; i < n; i++)
        {
            u8 lv = GetMonData(&sim.enemyParty[i], MON_DATA_LEVEL);
            if (lv > levelCap) levelCap = lv;
        }

        printf("{\"id\": \"trainer_%d_%s\", \"source\": \"game_trainer\", \"tags\": [\"%s\"", t, sTrainerNames[t], clsTag);
        if (isRival) printf(", \"rival\"");
        if (isRematch) printf(", \"rematch\"");
        if (tr->doubleBattle) printf(", \"doubles\"");
        printf("], \"format\": \"%s\", \"level_cap\": %d", tr->doubleBattle ? "doubles" : "singles", levelCap);

        // notes: trainer id, name, class, usable items
        printf(", \"notes\": \"trainer %d %s; name: %s; class: %s; items: ", t, sTrainerNames[t], tname, cls);
        {
            int printed = 0, done[MAX_TRAINER_ITEMS] = {0};
            for (i = 0; i < MAX_TRAINER_ITEMS; i++)
            {
                int cnt = 0;
                if (tr->items[i] == ITEM_NONE || done[i]) continue;
                for (k = i; k < MAX_TRAINER_ITEMS; k++)
                    if (tr->items[k] == tr->items[i]) { cnt++; done[k] = 1; }
                printf("%s%s x%d", printed ? ", " : "", gSimItemNames[tr->items[i]], cnt);
                printed++;
            }
            if (!printed) printf("none");
        }
        printf("\", \"mons\": [");

        for (i = 0; i < n; i++)
        {
            struct Pokemon *mon = &sim.enemyParty[i];
            u16 species = GetMonData(mon, MON_DATA_SPECIES);
            u16 item = GetMonData(mon, MON_DATA_HELD_ITEM);
            u8 abNum = GetMonData(mon, MON_DATA_ABILITY_NUM);
            u8 ability = GetAbilityBySpecies(species, abNum);
            int nm = 0;

            if (ability == ABILITY_NONE) { ability = gSpeciesInfo[species].abilities[0]; nAbilityFallback++; }

            printf("%s{\"species\": \"%s\", \"level\": %d, \"moves\": [", i ? ", " : "",
                   gSimSpeciesNames[species], GetMonData(mon, MON_DATA_LEVEL));
            for (k = 0; k < MAX_MON_MOVES; k++)
            {
                u16 mv = GetMonData(mon, MON_DATA_MOVE1 + k);
                if (mv == MOVE_NONE) continue;
                printf("%s\"%s\"", nm ? ", " : "", gSimMoveNames[mv]);
                nm++;
            }
            printf("], \"item\": \"%s\", \"ability\": \"%s\", \"nature\": \"%s\", \"ivs\": [",
                   gSimItemNames[item], gSimAbilityNames[ability], sNatureNames[GetNature(mon)]);
            for (k = 0; k < NUM_STATS; k++)
                printf("%s%d", k ? ", " : "", GetMonData(mon, MON_DATA_HP_IV + k));
            printf("], \"evs\": [");
            for (k = 0; k < NUM_STATS; k++)
                printf("%s%d", k ? ", " : "", GetMonData(mon, MON_DATA_HP_EV + k));
            printf("], \"happiness\": %d}", GetMonData(mon, MON_DATA_FRIENDSHIP));
            nMons++;
        }
        printf("]}\n");
        nTeams++;
    }
    fprintf(stderr, "dump_trainer_teams: %d teams, %d mons, %d mons with ability slot 1 but no second ability (used slot 0)\n",
            nTeams, nMons, nAbilityFallback);
    return 0;
}
