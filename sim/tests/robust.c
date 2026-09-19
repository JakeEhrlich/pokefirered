// Robustness fuzz: throw arbitrary input at the simulator and require that it never crashes, never hangs and
// always ends every battle with a defined result. Build with sanitizers for the real thing (`make robust`).
//   - parties: random encodable fields (any species id incl. the unused OLD_UNOWN slots and eggs, any move id,
//     any item id incl. key items, level 1..100, random IVs/EVs/HP incl. 0 HP, empty slots in the middle),
//     sometimes deliberately out-of-range values that Sim_Start must reject cleanly
//   - flags: trainer/double mostly, sometimes junk bits (must be rejected)
//   - decisions: garbage SimAction bytes most of the time (strict mode: retried until accepted; lenient mode:
//     handed to the engine as-is), sometimes legal ones; policies that return garbage on the other side
//   - direct API abuse: answering the wrong battler, answering with no request pending
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dlfcn.h>
#include "global.h"
#include "battle.h"
#include "pokemon.h"
#include "sim.h"
#include "constants/species.h"
#include "constants/moves.h"
#include "constants/items.h"
#include "constants/pokemon.h"

static u32 sRng = 777;
static u32 R(void) { sRng ^= sRng << 13; sRng ^= sRng >> 17; sRng ^= sRng << 5; return sRng; }

static void GarbageMon(struct Pokemon *mon, int allowInvalid)
{
    u16 moves[4];
    u8 ivs[6], evs[6];
    u16 species, item;
    u8 level;
    int i;
    for (i = 0; i < 4; i++)
        moves[i] = (R() % 5 == 0) ? 0 : R() % MOVES_COUNT;
    if (allowInvalid && R() % 8 == 0) moves[R() % 4] = MOVES_COUNT + R() % 1000;
    for (i = 0; i < 6; i++) { ivs[i] = R() % 32; evs[i] = R() % 256; }
    species = 1 + R() % (SPECIES_EGG - 1);              // 1..411, unused slots included
    if (allowInvalid && R() % 8 == 0) species = SPECIES_EGG + R() % 200;
    level = 1 + R() % 100;
    if (allowInvalid && R() % 8 == 0) level = (R() & 1) ? 0 : 101 + R() % 100;
    item = R() % ITEMS_COUNT;                            // key items, TMs, balls, unnamed slots: all "encodable"
    if (allowInvalid && R() % 8 == 0) item = ITEMS_COUNT + R() % 1000;
    Sim_MakeMonEx(mon, species, level ? level : 1, R() % NUM_NATURES, ivs, evs, moves, item, R() % 2, (R() % 3) ? 0 : 0x1234, R() & 1);
    if (level == 0 || level > 100) SetMonData(mon, MON_DATA_LEVEL, &level);
    if (allowInvalid && item >= ITEMS_COUNT) SetMonData(mon, MON_DATA_HELD_ITEM, &item);
    if (allowInvalid && species >= SPECIES_EGG) SetMonData(mon, MON_DATA_SPECIES, &species);
    if (allowInvalid) for (i = 0; i < 4; i++) if (moves[i] >= MOVES_COUNT) SetMonData(mon, MON_DATA_MOVE1 + i, &moves[i]);
    if (R() % 6 == 0) { u16 hp = 0; SetMonData(mon, MON_DATA_HP, &hp); }              // fainted
    else if (R() % 4 == 0) { u16 hp = R() % (GetMonData(mon, MON_DATA_MAX_HP) + 1); SetMonData(mon, MON_DATA_HP, &hp); }
    if (R() % 5 == 0) { u32 st = (R() % 8 == 0) ? R() : (1u << (R() % 8)); SetMonData(mon, MON_DATA_STATUS, &st); }
    if (R() % 10 == 0) { u8 f = R() % 256; SetMonData(mon, MON_DATA_FRIENDSHIP, &f); }
}

static void GarbagePolicy(struct BattleSim *sim, u8 battler, u8 kind, struct SimAction *out)
{
    u8 *b = (u8 *)out;
    int i;
    for (i = 0; i < (int)sizeof(*out); i++) b[i] = R() % 256;
    if (R() % 3 == 0) Sim_RandomPolicy(sim, battler, kind, out);
}

int main(int argc, char **argv)
{
    static struct BattleSim sim;
    int n = argc > 1 ? atoi(argv[1]) : 3000;
    int seed = argc > 2 ? atoi(argv[2]) : 1;
    int i, k;
    int started = 0, rejectedStart[4] = {0}, finished = 0, errors = 0, stuck = 0, requests = 0, rejectedAnswers = 0;
    int garbageDecisions = 0, strictBattles = 0, errorKinds[4] = {0};
    clock_t t0 = clock();

    sRng = seed ? seed : 1;
    for (i = 0; i < n; i++)
    {
        u32 flags = BATTLE_TYPE_TRAINER | ((R() % 3 == 0) ? BATTLE_TYPE_DOUBLE : 0);
        int allowInvalid = (R() % 5 == 0);
        int strict = R() & 1;
        int res, steps = 0;
        if (R() % 12 == 0) flags = R();                 // junk flags: must be rejected by Sim_Start
        Sim_Init(&sim, flags, R());
        sim.logEnabled = R() & 1;
        sim.maxTurns = 1 + R() % 300;
        sim.strictAnswers = strict;
        sim.badgeFlags = R() % 256;
        {
            // contiguous parties of 1..6 (the game never has gaps); 1 in 10 parties gets a gap, which Sim_Start must reject
            int np = 1 + R() % 6, ne = 1 + R() % 6;
            for (k = 0; k < np; k++) GarbageMon(&sim.playerParty[k], allowInvalid);
            for (k = 0; k < ne; k++) GarbageMon(&sim.enemyParty[k], allowInvalid);
            if (R() % 10 == 0) memset(&sim.playerParty[R() % np], 0, sizeof(struct Pokemon));
            if (R() % 10 == 0) memset(&sim.enemyParty[R() % ne], 0, sizeof(struct Pokemon));
        }
        if (R() % 3 == 0) Sim_SetPolicy(&sim, B_SIDE_OPPONENT, (R() & 1) ? GarbagePolicy : Sim_RandomPolicy);
        if (R() % 5 == 0) Sim_SetPolicy(&sim, B_SIDE_PLAYER, (R() & 1) ? GarbagePolicy : Sim_RandomPolicy);
        sim.rngXorshift = R() & 1;
        sim.rngValue = R() | 1;
        res = Sim_Start(&sim);
        if (res != 0)
        {
            rejectedStart[-res & 3]++;
            continue;
        }
        started++;
        strictBattles += strict;
        if (getenv("ROBUST_TRACE")) { printf("battle %d start (flags %x strict %d)\n", i, sim.battleTypeFlags, strict); fflush(stdout); }
        if (getenv("ROBUST_ONLY") && atoi(getenv("ROBUST_ONLY")) == i) { sim.logEnabled = TRUE; sim.logCount = 0; }
        // API abuse before any request
        { struct SimAction junk = {0}; Sim_Answer(&sim, R() % 4, &junk); }
        for (;;)
        {
            res = Sim_Run(&sim);
            if (getenv("ROBUST_BADEGG"))
            {
                for (k = 0; k < PARTY_SIZE * 2; k++)
                {
                    struct Pokemon *m = k < PARTY_SIZE ? &sim.playerParty[k] : &sim.enemyParty[k - PARTY_SIZE];
                    if (GetMonData(m, MON_DATA_SPECIES) && GetMonData(m, MON_DATA_SANITY_IS_BAD_EGG))
                    {
                        printf("battle %d: party mon %d became a bad egg at turn %d (run result %d, request kind %d battler %d)\n", i, k, sim.turnCount, res, sim.requestKind, sim.requestBattler);
                        Sim_PrintBattlers(&sim);
                        Sim_PrintLog(&sim);
                        return 1;
                    }
                }
            }
            if (++steps > 20000) { printf("battle %d: no termination after %d runs\n", i, steps); return 1; }
            if (getenv("ROBUST_ONLY") && atoi(getenv("ROBUST_ONLY")) == i && res != SIM_RUN_REQUEST)
            {
                printf("battle %d ended with %d, turn %d\n", i, res, sim.turnCount);
                Sim_PrintBattlers(&sim);
                Sim_PrintLog(&sim);
            }
            if (res == SIM_RUN_FINISHED) { finished++; break; }
            if (res == SIM_RUN_ERROR)
            {
                errors++;
                errorKinds[sim.error & 3]++;
                if (getenv("ROBUST_VERBOSE")) { printf("battle %d ERROR kind %d turn %d\n", i, sim.error, sim.turnCount); Sim_PrintBattlers(&sim); Sim_PrintLog(&sim); }
                break;
            }
            if (res == SIM_RUN_STUCK)
            {
                stuck++;
                if (getenv("ROBUST_VERBOSE"))
                {
                    Dl_info info;
                    dladdr((void *)sim.battleMainFunc, &info);
                    printf("battle %d STUCK: turn %d frames %u strict %d double %d requests so far %d mainfunc %s comm %d %d %d %d exec %x answers v%d/k%d/t%d v%d/k%d/t%d v%d/k%d/t%d v%d/k%d/t%d absent %x\n",
                           i, sim.turnCount, sim.frames, strict, (sim.battleTypeFlags & BATTLE_TYPE_DOUBLE) != 0, requests,
                           info.dli_sname ? info.dli_sname : "?", sim.battleCommunication[0], sim.battleCommunication[1], sim.battleCommunication[2], sim.battleCommunication[3],
                           sim.battleControllerExecFlags, sim.answerValid[0], sim.answerKind[0], sim.answer[0].type, sim.answerValid[1], sim.answerKind[1], sim.answer[1].type,
                           sim.answerValid[2], sim.answerKind[2], sim.answer[2].type, sim.answerValid[3], sim.answerKind[3], sim.answer[3].type, sim.absentBattlerFlags);
                    Sim_PrintBattlers(&sim);
                    Sim_PrintLog(&sim);
                }
                break;
            }
            if (res != SIM_RUN_REQUEST) { printf("battle %d: unknown result %d\n", i, res); return 1; }
            requests++;
            {
                struct SimAction act;
                int tries;
                for (tries = 0; tries < 64; tries++)
                {
                    int garbage = (R() % 4 != 0) && tries < 40;
                    u8 battler = sim.requestBattler;
                    if (R() % 16 == 0) battler = R() % 4;              // wrong battler now and then
                    if (garbage)
                    {
                        u8 *b = (u8 *)&act;
                        for (k = 0; k < (int)sizeof(act); k++) b[k] = R() % 256;
                        garbageDecisions++;
                    }
                    else
                        Sim_RandomPolicy(&sim, sim.requestBattler, sim.requestKind, &act);
                    if (Sim_Answer(&sim, battler, &act) == 0)
                        break;
                    rejectedAnswers++;
                }
                if (tries == 64)
                {
                    // even a legal answer was refused: that would be a bug
                    Sim_RandomPolicy(&sim, sim.requestBattler, sim.requestKind, &act);
                    if (Sim_Answer(&sim, sim.requestBattler, &act) != 0)
                    {
                        printf("battle %d: legal answer rejected (kind %d battler %d)\n", i, sim.requestKind, sim.requestBattler);
                        return 1;
                    }
                }
            }
        }
    }
    printf("robust: %d battles attempted, %d started (rejected by Sim_Start: %d no-usable/doubles, %d unencodable, %d bad flags),\n"
           "        %d finished, %d errors (trap %d, bad policy %d, stuck-budget %d), %d stuck, %d strict; %d requests, %d garbage decisions, %d rejected answers; %.2fs\n",
           n, started, rejectedStart[1], rejectedStart[2], rejectedStart[3], finished, errors, errorKinds[1], errorKinds[2], errorKinds[3], stuck, strictBattles,
           requests, garbageDecisions, rejectedAnswers, (double)(clock() - t0) / CLOCKS_PER_SEC);
    return 0;
}
