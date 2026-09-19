// Move legality: which moves a species can know in FRLG (level-up, TM/HM, tutor, egg moves, pre-evolutions).
#include "global.h"
#include "pokemon.h"
#include "sim.h"
#include "constants/items.h"
#include "constants/moves.h"
#include "constants/species.h"
#include "constants/pokemon.h"
#include "constants/party_menu.h"
#include "data/pokemon/tutor_learnsets.h"
#include "data/pokemon/egg_moves.h"
#include "tmhm_moves.h"

u32 SpeciesCanLearnTMHM(u16 species, u8 tm);
extern const struct Evolution gEvolutionTable[NUM_SPECIES][EVOS_PER_MON];

bool8 Sim_LearnsByLevelUp(u16 species, u16 move, u8 maxLevel)
{
    const u16 *learnset = gLevelUpLearnsets[species];
    int i;

    for (i = 0; learnset[i] != LEVEL_UP_END; i++)
    {
        u8 lvl = learnset[i] >> 9;
        u16 m = learnset[i] & 0x1FF;
        if (m == move && lvl <= maxLevel)
            return TRUE;
    }
    return FALSE;
}

bool8 Sim_LearnsByTMHM(u16 species, u16 move)
{
    int i;
    for (i = 0; i < NUM_TECHNICAL_MACHINES + NUM_HIDDEN_MACHINES; i++)
        if (sTMHMMoves[i] == move && SpeciesCanLearnTMHM(species, i))
            return TRUE;
    return FALSE;
}

bool8 Sim_LearnsByTutor(u16 species, u16 move)
{
    int i;
    for (i = 0; i < TUTOR_MOVE_COUNT; i++)
        if (sTutorMoves[i] == move && (sTutorLearnsets[species] & (1 << i)))
            return TRUE;
    return FALSE;
}

bool8 Sim_IsEggMove(u16 species, u16 move)
{
    int i = 0;
    while (i < (int)ARRAY_COUNT(gEggMoves))
    {
        if (gEggMoves[i] == species + EGG_MOVES_SPECIES_OFFSET)
        {
            i++;
            while (i < (int)ARRAY_COUNT(gEggMoves) && gEggMoves[i] < EGG_MOVES_SPECIES_OFFSET)
            {
                if (gEggMoves[i] == move)
                    return TRUE;
                i++;
            }
            return FALSE;
        }
        i++;
    }
    return FALSE;
}

u16 Sim_PreEvolution(u16 species)
{
    int s, e;
    for (s = 1; s < NUM_SPECIES; s++)
        for (e = 0; e < EVOS_PER_MON; e++)
            if (gEvolutionTable[s][e].targetSpecies == species && gEvolutionTable[s][e].method != 0)
                return s;
    return SPECIES_NONE;
}

// True if `species` at `level` can legitimately know `move` (own learnsets or any pre-evolution's).
bool8 Sim_CanLearnMove(u16 species, u16 move, u8 level)
{
    int depth;
    u16 s = species;

    if (move == MOVE_NONE || move >= MOVES_COUNT || species == SPECIES_NONE || species >= NUM_SPECIES)
        return FALSE;
    // Smeargle sketches any move it sees (Sketch itself comes by level-up, Struggle cannot be sketched).
    if (species == SPECIES_SMEARGLE && move != MOVE_STRUGGLE)
        return TRUE;
    for (depth = 0; depth < 4 && s != SPECIES_NONE; depth++)
    {
        if (Sim_LearnsByLevelUp(s, move, level) || Sim_LearnsByTMHM(s, move) || Sim_LearnsByTutor(s, move) || Sim_IsEggMove(s, move))
            return TRUE;
        // FireRed-only: the Cape Brink tutors teach the starters' ultimate moves; Pichu hatches with Volt
        // Tackle when a parent holds a Light Ball (so Pikachu/Raichu inherit it through this walk).
        if ((s == SPECIES_VENUSAUR && move == MOVE_FRENZY_PLANT) || (s == SPECIES_CHARIZARD && move == MOVE_BLAST_BURN)
         || (s == SPECIES_BLASTOISE && move == MOVE_HYDRO_CANNON) || (s == SPECIES_PICHU && move == MOVE_VOLT_TACKLE))
            return TRUE;
        s = Sim_PreEvolution(s);
    }
    return FALSE;
}

// Fills `out` with every move the species can know at `level`; returns the count.
int Sim_LearnableMoves(u16 species, u8 level, u16 *out, int maxOut)
{
    int n = 0;
    u16 move;
    for (move = 1; move < MOVES_COUNT && n < maxOut; move++)
        if (Sim_CanLearnMove(species, move, level))
            out[n++] = move;
    return n;
}
