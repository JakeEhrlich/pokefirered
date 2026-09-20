// Team corpus support: parse the flat TSV form produced by tools/teams.py (format in teams/FORMAT.md),
// judge FireRed legality with the simulator's own rules, and build the mon with Sim_MakeMonEx.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "global.h"
#include "pokemon.h"
#include "sim.h"
#include "sim_items.h"
#include "sim_names.h"
#include <stdarg.h>
#include "constants/species.h"
#include "constants/moves.h"
#include "constants/items.h"
#include "constants/global.h"
#include "constants/pokemon.h"

int Sim_ParseTeamRow(const char *line, struct SimTeamMon *out)
{
    int f[40], n = 0, i;
    const char *p = line;
    char *end;

    memset(out, 0, sizeof(*out));
    // team id
    end = strchr(line, '\t');
    if (!end) return 0;
    i = end - line;
    if (i >= (int)sizeof(out->teamId)) i = sizeof(out->teamId) - 1;
    memcpy(out->teamId, line, i);
    out->teamId[i] = 0;
    p = end + 1;
    while (*p && n < 40)
    {
        f[n++] = (int)strtol(p, &end, 10);
        if (end == p) return 0;
        p = end;
        while (*p == '\t' || *p == ' ') p++;
        if (*p == '\n' || *p == '\r') break;
    }
    if (n < 27) return 0;
    out->slot = f[0];
    out->species = f[1];
    out->level = f[2];
    for (i = 0; i < 4; i++) out->moves[i] = f[3 + i];
    out->item = f[7];
    out->abilityNum = f[8];
    out->abilityName = f[9];
    out->nature = f[10];
    for (i = 0; i < 6; i++) { out->ivs[i] = f[11 + i]; out->evs[i] = f[17 + i]; }
    out->happiness = f[23];
    out->outsider = f[24];
    out->fateful = f[25];
    out->doubles = f[26];
    return 1;
}

static void Reason(char *why, int len, const char *fmt, ...)
{
    va_list ap;
    int used = strlen(why);
    if (used >= len - 2) return;
    if (used) { why[used++] = ';'; why[used++] = ' '; why[used] = 0; }
    va_start(ap, fmt);
    vsnprintf(why + used, len - used, fmt, ap);
    va_end(ap);
}

// Returns 2 = legal in FireRed, 1 = encodable only, 0 = cannot be encoded. Fills `why` (may be "").
int Sim_CheckTeamMon(const struct SimTeamMon *m, char *why, int whyLen)
{
    int verdict = 2, i, total = 0, nMoves = 0;
    why[0] = 0;

    if (m->species == 0 || m->species >= NUM_SPECIES) { Reason(why, whyLen, "species %d not encodable", m->species); return 0; }
    if (m->level < 1 || m->level > 100) { Reason(why, whyLen, "level %d", m->level); return 0; }
    if (m->item >= ITEMS_COUNT) { Reason(why, whyLen, "item %d not encodable", m->item); return 0; }
    if (m->nature >= NUM_NATURES) { Reason(why, whyLen, "nature %d", m->nature); return 0; }
    for (i = 0; i < 6; i++)
    {
        if (m->ivs[i] > 31) { Reason(why, whyLen, "iv %d = %d", i, m->ivs[i]); return 0; }
        if (m->evs[i] > 255) { Reason(why, whyLen, "ev %d = %d", i, m->evs[i]); return 0; }
        total += m->evs[i];
    }
    if (total > 510) { Reason(why, whyLen, "ev total %d", total); return 0; }
    for (i = 0; i < 4; i++)
    {
        if (m->moves[i] >= MOVES_COUNT) { Reason(why, whyLen, "move %d not encodable", m->moves[i]); return 0; }
        if (m->moves[i]) nMoves++;
    }
    if (nMoves == 0) { Reason(why, whyLen, "no moves"); return 0; }
    if (m->abilityNum != 0 && m->abilityNum != 1 && m->abilityNum != 255) { Reason(why, whyLen, "ability slot %d", m->abilityNum); return 0; }

    // FireRed rules
    for (i = 0; i < 4; i++)
        if (m->moves[i] && !Sim_CanLearnMove(m->species, m->moves[i], m->level))
        {
            Reason(why, whyLen, "%s cannot know %s at L%d", gSimSpeciesNames[m->species], gSimMoveNames[m->moves[i]], m->level);
            verdict = 1;
        }
    if (m->item && (!gSimItems[m->item].name || gSimItems[m->item].pocket == POCKET_KEY_ITEMS || gSimItems[m->item].name[0] == '?'))
    {
        Reason(why, whyLen, "item %s not holdable", gSimItemNames[m->item]);
        verdict = 1;
    }
    if (m->abilityNum == 255)
    {
        if (gSpeciesInfo[m->species].abilities[0] != m->abilityName && gSpeciesInfo[m->species].abilities[1] != m->abilityName)
        {
            Reason(why, whyLen, "%s does not have ability %s", gSimSpeciesNames[m->species], gSimAbilityNames[m->abilityName]);
            verdict = 1;
        }
    }
    else if (m->abilityNum == 1 && gSpeciesInfo[m->species].abilities[1] == 0)
    {
        Reason(why, whyLen, "%s has no second ability", gSimSpeciesNames[m->species]);
        verdict = 1;
    }
    if ((m->species == SPECIES_MEW || m->species == SPECIES_DEOXYS) && !m->fateful)
    {
        Reason(why, whyLen, "%s disobeys without the fateful flag", gSimSpeciesNames[m->species]);
        verdict = 1;
    }
    return verdict;
}

// Resolves the ability slot (a named ability that the species lacks falls back to slot 0).
static u8 AbilitySlot(const struct SimTeamMon *m)
{
    if (m->abilityNum == 255)
        return gSpeciesInfo[m->species].abilities[1] == m->abilityName ? 1 : 0;
    if (m->abilityNum == 1 && gSpeciesInfo[m->species].abilities[1] == 0)
        return 0;
    return m->abilityNum;
}

void Sim_BuildTeamMon(struct Pokemon *mon, const struct SimTeamMon *m)
{
    u8 ivs[6], evs[6], hap = m->happiness;
    u16 moves[4];
    int i;
    for (i = 0; i < 6; i++) { ivs[i] = m->ivs[i]; evs[i] = m->evs[i]; }
    for (i = 0; i < 4; i++) moves[i] = m->moves[i];
    {
        // The personality (gender, Unown letter, Spinda spots) is drawn from the bound sim's RNG; seed it from the
        // row so the same team always builds the same mons, whatever was loaded before (replayable game records).
        u32 savedRng = gSim->rngValue;
        u8 savedXs = gSim->rngXorshift;
        u32 h = 2166136261u;
        h = (h ^ m->species) * 16777619u; h = (h ^ m->slot) * 16777619u; h = (h ^ m->level) * 16777619u;
        h = (h ^ m->nature) * 16777619u; h = (h ^ m->item) * 16777619u;
        for (i = 0; i < 4; i++) h = (h ^ moves[i]) * 16777619u;
        for (i = 0; i < 6; i++) h = (h ^ ivs[i]) * 16777619u;
        gSim->rngXorshift = 1;
        gSim->rngValue = h | 1;
        Sim_MakeMonEx(mon, m->species, m->level, m->nature, ivs, evs, moves, m->item, AbilitySlot(m),
                      m->outsider ? 0x12345 : 0, m->fateful);
        gSim->rngValue = savedRng;
        gSim->rngXorshift = savedXs;
    }
    SetMonData(mon, MON_DATA_FRIENDSHIP, &hap);
}

// Loads the next team from a TSV stream into `party` (up to 6 mons). Returns the number of mons, 0 at EOF.
// `teamId` receives the id; `doubles` the format flag.
int Sim_ReadTeamTsv(FILE *f, struct Pokemon *party, char *teamId, int teamIdLen, u8 *doubles)
{
    static char line[1024];
    static int havePending;
    static struct SimTeamMon pending;
    struct SimTeamMon m;
    int n = 0;

    memset(party, 0, sizeof(struct Pokemon) * PARTY_SIZE);
    teamId[0] = 0;
    for (;;)
    {
        if (havePending) { m = pending; havePending = 0; }
        else
        {
            if (!fgets(line, sizeof(line), f)) break;
            if (!Sim_ParseTeamRow(line, &m)) continue;
        }
        if (n == 0) { snprintf(teamId, teamIdLen, "%s", m.teamId); *doubles = m.doubles; }
        else if (strcmp(teamId, m.teamId) != 0) { pending = m; havePending = 1; break; }
        if (n < PARTY_SIZE)
            Sim_BuildTeamMon(&party[n++], &m);
    }
    return n;
}
