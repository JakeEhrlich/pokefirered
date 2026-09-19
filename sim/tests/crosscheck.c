// Cross-check the simulator against the real game running in mGBA (see harness/README.md).
//
// Talks to harness/harness_gen.lua over TCP. For each battle it sends the player party and settings; the
// harness ROM gives its battle-engine code a xorshift RNG stream seeded identically to the simulator's, so
// both play the same battle as long as they make the same decisions. At every decision the ROM makes, the
// driver runs the simulator to the same point, diffs the full battle state, and sends the same action to
// both. The opponent is the game's own trainer AI on both sides.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "global.h"
#include "battle.h"
#include "pokemon.h"
#include "sim.h"
#include "sim_names.h"
#include "harness_addrs.h"
#include "constants/species.h"
#include "constants/moves.h"
#include "constants/items.h"
#include "constants/pokemon.h"
#include "constants/opponents.h"
#include "scenario.h"
#undef CHECK

static int sSock;
static char sLine[8192];
static char sRxBuf[65536];
static int sRxLen;
static u32 sRomEngineCalls, sRomOtherCalls;
static u8 sCallerRing[256];

static u32 sHostRng = 777;
static u32 R(void) { sHostRng = sHostRng * 1103515245 + 12345; return sHostRng >> 8; }

// Transcript recording / replay. CROSSCHECK_RECORD=<file> appends everything exchanged with the ROM
// (START/ACT sent, REQ/DONE + snapshots received) plus a "# label" / "CFG" header per battle, so the
// same battles can later be re-verified against the simulator without mGBA (--replay <file>).
static FILE *sRecord;
static FILE *sReplay;
static int sPushback;

static int ReadLine(void)
{
    if (sReplay)
    {
        size_t n;
        if (sPushback) { sPushback = 0; return 1; }
        if (!fgets(sLine, sizeof(sLine), sReplay))
            return 0;
        n = strlen(sLine);
        while (n && (sLine[n - 1] == '\n' || sLine[n - 1] == '\r')) sLine[--n] = 0;
        return 1;
    }
    for (;;)
    {
        char *nl = memchr(sRxBuf, '\n', sRxLen);
        if (nl)
        {
            int n = nl - sRxBuf;
            if (n >= (int)sizeof(sLine)) n = sizeof(sLine) - 1;
            memcpy(sLine, sRxBuf, n);
            sLine[n] = 0;
            sRxLen -= (nl - sRxBuf) + 1;
            memmove(sRxBuf, nl + 1, sRxLen);
            if (sRecord) fprintf(sRecord, "%s\n", sLine);
            return 1;
        }
        {
            int r = recv(sSock, sRxBuf + sRxLen, sizeof(sRxBuf) - sRxLen, 0);
            if (r <= 0)
                return 0;
            sRxLen += r;
        }
    }
}

static void SendLine(const char *s)
{
    if (sRecord) fprintf(sRecord, "%s\n", s);
    send(sSock, s, strlen(s), 0);
    send(sSock, "\n", 1, 0);
}

static int HexToBytes(const char *hex, u8 *out, int max)
{
    int n = 0;
    while (hex[0] && hex[1] && n < max)
    {
        unsigned v;
        sscanf(hex, "%2x", &v);
        out[n++] = v;
        hex += 2;
    }
    return n;
}

struct Snapshot
{
    u8 mons[88 * 4];
    u8 st3[16];
    u8 side[28];
    u8 dis[28 * 4];
    u8 weather[2];
    u8 wish[44];
    u8 pparty[600];
    u8 eparty[600];
    u8 misc[10];
    int kind, battler, seq, outcome;
};

static const struct { int off; const char *name; } sMonFields[] = {
    {0,"species"},{2,"attack"},{4,"defense"},{6,"speed"},{8,"spAttack"},{10,"spDefense"},{12,"moves"},{20,"ivs/abilityNum"},
    {24,"statStages"},{32,"ability"},{33,"type1"},{34,"type2"},{35,"unknown"},{36,"pp"},{40,"hp"},{42,"level"},{43,"friendship"},
    {44,"maxHP"},{46,"item"},{48,"nickname"},{59,"ppBonuses"},{60,"otName"},{68,"experience"},{72,"personality"},{76,"status1"},
    {80,"status2"},{84,"otId"},{88,NULL}};

static const char *MonFieldName(int off)
{
    int i;
    for (i = 0; sMonFields[i + 1].name; i++)
        if (off >= sMonFields[i].off && off < sMonFields[i + 1].off)
            return sMonFields[i].name;
    return "?";
}

static int sMismatches;

static int DiffBytes(const char *what, const u8 *rom, const u8 *simb, int len, int perElem)
{
    int i;
    for (i = 0; i < len; i++)
    {
        if (rom[i] != simb[i])
        {
            int e = perElem ? i / perElem : 0, o = perElem ? i % perElem : i;
            printf("    MISMATCH %s[%d] byte %d (%s): rom %02x sim %02x\n", what, e, o,
                   strcmp(what, "battleMons") == 0 ? MonFieldName(o) : "-", rom[i], simb[i]);
            sMismatches++;
            return 1;
        }
    }
    return 0;
}

static void DiffParty(struct Pokemon *rom, struct Pokemon *simp)
{
    static const struct { int field; const char *name; } fields[] = {
        {MON_DATA_SPECIES,"species"},{MON_DATA_HELD_ITEM,"item"},{MON_DATA_EXP,"exp"},{MON_DATA_FRIENDSHIP,"friendship"},
        {MON_DATA_HP,"hp"},{MON_DATA_MAX_HP,"maxHP"},{MON_DATA_STATUS,"status"},{MON_DATA_LEVEL,"level"},{MON_DATA_OT_ID,"otId"},
        {MON_DATA_PERSONALITY,"personality"},{MON_DATA_HP_EV,"hpEV"},{MON_DATA_ATK_EV,"atkEV"},{MON_DATA_MET_LOCATION,"metLocation"},
        {MON_DATA_MET_LEVEL,"metLevel"},{MON_DATA_POKEBALL,"pokeball"},{MON_DATA_PP1,"pp1"},{MON_DATA_PP2,"pp2"},{MON_DATA_PP3,"pp3"},
        {MON_DATA_PP4,"pp4"},{MON_DATA_MOVE1,"move1"},{MON_DATA_PP_BONUSES,"ppBonuses"},{MON_DATA_LANGUAGE,"language"},
        {MON_DATA_CHECKSUM,"checksum"},{MON_DATA_MARKINGS,"markings"},{MON_DATA_POKERUS,"pokerus"},{MON_DATA_OT_GENDER,"otGender"},
        {MON_DATA_ATK,"atk"},{MON_DATA_DEF,"def"},{MON_DATA_SPEED,"speed"},{MON_DATA_SPATK,"spatk"},{MON_DATA_SPDEF,"spdef"},{-1,NULL}};
    int m, f;
    for (m = 0; m < PARTY_SIZE; m++)
        for (f = 0; fields[f].name; f++)
        {
            u32 a = GetMonData(&rom[m], fields[f].field), b = GetMonData(&simp[m], fields[f].field);
            if (a != b)
                printf("      mon %d %s: rom %u sim %u\n", m, fields[f].name, a, b);
        }
}

// The game copies names only up to the terminator, leaving stack garbage behind it; ignore those bytes.
static void MaskPartyNames(u8 *rom, const u8 *simb)
{
    int m, k, eos;
    for (m = 0; m < PARTY_SIZE; m++)
    {
        u8 *r = rom + m * 100;
        const u8 *q = simb + m * 100;
        for (k = 8, eos = 0; k < 18; k++) { if (eos) r[k] = q[k]; else if (r[k] == 0xFF) eos = 1; }
        for (k = 20, eos = 0; k < 27; k++) { if (eos) r[k] = q[k]; else if (r[k] == 0xFF) eos = 1; }
    }
}

static int CompareSnapshot(struct BattleSim *sim, struct Snapshot *s)
{
    int before = sMismatches;
    u8 misc[10];
    int i;

    for (i = 0; i < sim->battlersCount; i++)
    {
        u8 rom[88];
        memcpy(rom, s->mons + 88 * i, 88);
        rom[35] = ((u8 *)&sim->battleMons[i])[35]; // BattlePokemon.unknown is never initialized by the game
        {
            int k, eos = 0;
            for (k = 48; k < 59; k++) { if (eos) rom[k] = ((u8 *)&sim->battleMons[i])[k]; else if (rom[k] == 0xFF) eos = 1; }
            for (k = 60, eos = 0; k < 68; k++) { if (eos) rom[k] = ((u8 *)&sim->battleMons[i])[k]; else if (rom[k] == 0xFF) eos = 1; }
        }
        if (DiffBytes("battleMons", rom, (u8 *)&sim->battleMons[i], 88, 88))
            printf("      (battler %d)\n", i);
    }
    DiffBytes("statuses3", s->st3, (u8 *)sim->statuses3, 4 * sim->battlersCount, 4);
    DiffBytes("sideStatuses", s->side, (u8 *)sim->sideStatuses, 4, 2);
    DiffBytes("sideTimers", s->side + 4, (u8 *)sim->sideTimers, 24, 12);
    DiffBytes("disableStructs", s->dis, (u8 *)sim->disableStructs, 28 * sim->battlersCount, 28);
    DiffBytes("weather", s->weather, (u8 *)&sim->battleWeather, 2, 0);
    DiffBytes("wishFutureKnock", s->wish, (u8 *)&sim->wishFutureKnock, sizeof(struct WishFutureKnock), 0);
    MaskPartyNames(s->pparty, (u8 *)sim->playerParty);
    MaskPartyNames(s->eparty, (u8 *)sim->enemyParty);
    if (DiffBytes("playerParty", s->pparty, (u8 *)sim->playerParty, 600, 100))
        DiffParty((struct Pokemon *)s->pparty, sim->playerParty);
    if (DiffBytes("enemyParty", s->eparty, (u8 *)sim->enemyParty, 600, 100))
        DiffParty((struct Pokemon *)s->eparty, sim->enemyParty);
    for (i = 0; i < 4; i++) { misc[i * 2] = sim->battlerPartyIndexes[i]; misc[i * 2 + 1] = sim->battlerPartyIndexes[i] >> 8; }
    misc[8] = sim->absentBattlerFlags;
    misc[9] = sim->battleOutcome;
    DiffBytes("partyIndexes", s->misc, misc, 2 * sim->battlersCount, 2);
    DiffBytes("absent/outcome", s->misc + 8, misc + 8, 2, 0);
    return sMismatches - before;
}

// Reads REQ/DONE + snapshot lines. Returns 1 for REQ, 2 for DONE, 0 on error.
static int ReadEvent(struct Snapshot *s)
{
    int type = 0;
    for (;;)
    {
        if (!ReadLine())
            return 0;
        if (!strncmp(sLine, "REQ ", 4)) { type = 1; sscanf(sLine + 4, "%d %d %d", &s->kind, &s->battler, &s->seq); }
        else if (!strncmp(sLine, "DONE ", 5)) { type = 2; sscanf(sLine + 5, "%d", &s->outcome); }
        else if (!strncmp(sLine, "RNGCALLS ", 9)) sscanf(sLine + 9, "%u %u", &sRomEngineCalls, &sRomOtherCalls);
        else if (!strncmp(sLine, "CALLERS ", 8)) HexToBytes(sLine + 8, sCallerRing, sizeof(sCallerRing));
        else if (!strncmp(sLine, "MONS ", 5)) HexToBytes(sLine + 5, s->mons, sizeof(s->mons));
        else if (!strncmp(sLine, "ST3 ", 4)) HexToBytes(sLine + 4, s->st3, sizeof(s->st3));
        else if (!strncmp(sLine, "SIDE ", 5)) HexToBytes(sLine + 5, s->side, sizeof(s->side));
        else if (!strncmp(sLine, "DIS ", 4)) HexToBytes(sLine + 4, s->dis, sizeof(s->dis));
        else if (!strncmp(sLine, "WEATHER ", 8)) HexToBytes(sLine + 8, s->weather, sizeof(s->weather));
        else if (!strncmp(sLine, "WISH ", 5)) HexToBytes(sLine + 5, s->wish, sizeof(s->wish));
        else if (!strncmp(sLine, "PPARTY ", 7)) HexToBytes(sLine + 7, s->pparty, sizeof(s->pparty));
        else if (!strncmp(sLine, "EPARTY ", 7)) HexToBytes(sLine + 7, s->eparty, sizeof(s->eparty));
        else if (!strncmp(sLine, "MISC ", 5)) HexToBytes(sLine + 5, s->misc, sizeof(s->misc));
        else if (!strcmp(sLine, "END")) return type;
        else if (!strcmp(sLine, "STARTED") || !strcmp(sLine, "READY") || !strcmp(sLine, "PONG")) {}
        else printf("  (unexpected line: %.60s)\n", sLine);
    }
}

static void MakePlayerParty(struct BattleSim *sim, int n)
{
    int k;
    for (k = 0; k < n; k++)
    {
        u16 species, moves[4], pool[MOVES_COUNT];
        u8 level, ivs[6], evs[6];
        int np, m;
        do
            species = 1 + R() % 151;
        while (species == SPECIES_NONE);
        level = 20 + R() % 50;
        np = Sim_LearnableMoves(species, level, pool, MOVES_COUNT);
        for (m = 0; m < 4; m++)
            moves[m] = np ? pool[R() % np] : MOVE_TACKLE;
        for (m = 0; m < 6; m++) { ivs[m] = R() % 32; evs[m] = R() % 64; }
        Sim_MakeMon(&sim->playerParty[k], species, level, R() % NUM_NATURES, ivs, evs, moves, (R() % 3 == 0) ? ITEM_LEFTOVERS : ITEM_NONE, R() % 2);
    }
}

static void ChooseAction(struct BattleSim *sim, u8 battler, u8 kind, struct SimAction *out)
{
    struct SimAction acts[32];
    u8 slots[6];
    int n;
    memset(out, 0, sizeof(*out));
    if (kind == SIM_REQ_SWITCH)
    {
        n = Sim_LegalSwitches(sim, battler, slots, 6);
        out->type = B_ACTION_SWITCH;
        out->partySlot = n ? slots[R() % n] : 0;
        return;
    }
    n = Sim_LegalActions(sim, battler, acts, 32);
    if (n == 0) { out->type = B_ACTION_USE_MOVE; out->target = 0xFF; return; }
    *out = acts[R() % n];
}

static const char *Symbolize(u32 addr)
{
    unsigned i;
    const char *best = "?";
    for (i = 0; i < sizeof(sHarnessSymbols) / sizeof(sHarnessSymbols[0]); i++)
        if (sHarnessSymbols[i].addr <= addr)
            best = sHarnessSymbols[i].name;
    return best;
}

static void DumpRngTraces(u32 romFrom, int simFrom)
{
    u32 i, n = sRomEngineCalls - romFrom;
    printf("    engine Random() calls since the previous decision (rom caller | sim value caller):\n");
    if (n > 64) { printf("      (rom: only the last 64 of %u are known)\n", n); romFrom = sRomEngineCalls - 64; n = 64; }
    for (i = 0; i < n || simFrom + (int)i < gSimRngTraceCount; i++)
    {
        if (i < n)
        {
            u32 idx = (romFrom + i) % 64;
            u32 a = sCallerRing[idx * 4] | (sCallerRing[idx * 4 + 1] << 8) | (sCallerRing[idx * 4 + 2] << 16) | ((u32)sCallerRing[idx * 4 + 3] << 24);
            printf("      %-36s", Symbolize(a));
        }
        else
            printf("      %-36s", "-");
        if (simFrom + (int)i < gSimRngTraceCount)
            printf(" | %04x %s", gSimRngTrace[simFrom + i].value, gSimRngTrace[simFrom + i].caller);
        printf("\n");
    }
}

typedef void (*DecideFn)(void *ctx, struct BattleSim *sim, u8 battler, u8 kind, struct SimAction *out);
typedef int (*StopFn)(void *ctx, struct BattleSim *sim);

// Plays one battle on both sides. The caller has already prepared `sim` (parties, flags, RNG mode/seed,
// policies) but not called Sim_Start. Returns 1 if the battle matched throughout, 0 otherwise.
static int PlayBattle(struct BattleSim *sim, const char *label, int scriptOpponent, DecideFn decide, StopFn stop, void *ctx)
{
    static char msg[4096];
    char *p;
    struct Snapshot snap;
    int i, ev, decisions = 0, battleFailed = 0, simFrom = 0, stopped = 0;
    u32 romFrom = 0;
    u32 flags = sim->battleTypeFlags;
    u32 seed = sim->rngValue;

    // Build the START message from the pre-battle state (Sim_Start bumps friendship for league battles,
    // exactly as the ROM will), then start the simulator: a setup it rejects (e.g. an unused double-battle
    // trainer with a single mon, like TRAINER_INTERVIEWER) must not be sent to the ROM at all.
    if (sRecord) fprintf(sRecord, "# %s\nCFG %u %u %u\n", label, sim->badgeFlags, scriptOpponent, sim->createTrainerParty);
    p = msg + sprintf(msg, "START %u %u %u %u %u %u %u %u ", flags, sim->trainerBattleOpponent_A, 1, 0, sim->badgeFlags & 0xFF, 1, 1, seed);
    for (i = 0; i < 600; i++)
        p += sprintf(p, "%02x", ((u8 *)sim->playerParty)[i]);
    if (scriptOpponent)
    {
        p += sprintf(p, " 1 ");
        for (i = 0; i < 600; i++)
            p += sprintf(p, "%02x", ((u8 *)sim->enemyParty)[i]);
    }
    gSimRngTraceEnabled = 1;
    gSimRngTraceCount = 0;
    if (Sim_Start(sim) != 0)
    {
        printf("%s: flags %x, seed %x\n  skipped: invalid setup for the simulator\n", label, flags, seed);
        if (sRecord) fprintf(sRecord, "# skipped\n");
        return 1;
    }
    SendLine(msg);
    printf("%s: flags %x, seed %x\n", label, flags, seed);
    ev = ReadEvent(&snap);
    for (;;)
    {
        int r;
        struct SimAction act;

        if (ev == 0) { printf("  connection lost\n"); exit(3); }
        r = Sim_Run(sim);
        if (ev == 1)
        {
            decisions++;
            if (r != SIM_RUN_REQUEST || sim->requestKind != snap.kind || sim->requestBattler != snap.battler)
            {
                printf("  DESYNC at decision %d: rom asks kind %d battler %d, sim %s kind %d battler %d\n", decisions,
                       snap.kind, snap.battler, r == SIM_RUN_REQUEST ? "asks" : "finished/stuck", sim->requestKind, sim->requestBattler);
                battleFailed = 1;
            }
        }
        else if (r != SIM_RUN_FINISHED)
        {
            printf("  DESYNC: rom finished (outcome %d) but sim %s\n", snap.outcome, r == SIM_RUN_REQUEST ? "asks for a decision" : "is stuck");
            battleFailed = 1;
        }
        if (sim->rngCalls != sRomEngineCalls)
        {
            printf("  RNG CALL COUNT differs at %s %d: rom engine calls %u, sim %u\n", ev == 1 ? "decision" : "end",
                   decisions, sRomEngineCalls, sim->rngCalls);
            battleFailed = 1;
        }
        if (CompareSnapshot(sim, &snap))
            battleFailed = 1;
        if (battleFailed)
        {
            DumpRngTraces(romFrom, simFrom);
            printf("  turn %d, sim log:\n", sim->turnCount);
            Sim_PrintLog(sim);
            Sim_PrintBattlers(sim);
            break;
        }
        if (ev == 2)
        {
            printf("  ok: %d decisions, %d turns, outcome %d, engine rng calls %u (rom made %u other calls)\n",
                   decisions, sim->turnCount, snap.outcome, sRomEngineCalls, sRomOtherCalls);
            break;
        }
        if (stop && stop(ctx, sim))
        {
            printf("  ok: stopped after %d decisions, %d turns (scenario turn limit); rom left mid-battle\n", decisions, sim->turnCount);
            stopped = 1;
            break;
        }
        decide(ctx, sim, snap.battler, snap.kind, &act);
        Sim_Answer(sim, snap.battler, &act);
        sprintf(msg, "ACT %d %d %d %d %d", act.type, act.moveSlot, act.target, act.partySlot, act.item);
        SendLine(msg);
        sim->logCount = 0;
        simFrom = gSimRngTraceCount;
        romFrom = sRomEngineCalls;
        ev = ReadEvent(&snap);
    }
    // A failed or stopped battle is simply abandoned: the bridge restores the idle save state before the next START.
    return !battleFailed;
}

static void RandomDecide(void *ctx, struct BattleSim *sim, u8 battler, u8 kind, struct SimAction *out)
{
    ChooseAction(sim, battler, kind, out);
}

static void ScenarioDecide(void *ctx, struct BattleSim *sim, u8 battler, u8 kind, struct SimAction *out)
{
    Scenario_Decide((struct ScenarioRun *)ctx, sim, battler, kind, out);
}

static int ScenarioStop(void *ctx, struct BattleSim *sim)
{
    return Scenario_ShouldStop(sim, ((struct ScenarioRun *)ctx)->sc);
}

static int RunScenarios(const char *filter)
{
    static struct BattleSim sim;
    int g, i, ran = 0, failed = 0;
    for (g = 0; g < gScenarioGroupCount; g++)
    {
        const struct ScenarioGroup *grp = gScenarioGroups[g];
        for (i = 0; i < grp->count; i++)
        {
            const struct Scenario *sc = &grp->scenarios[i];
            struct ScenarioRun run = { .sc = sc };
            char label[256];
            u16 seed;
            if (filter && !strstr(sc->name, filter) && !strstr(grp->name, filter))
                continue;
            seed = Scenario_ResolveSeed(&sim, sc);
            Scenario_Setup(&sim, sc, seed);
            snprintf(label, sizeof(label), "scenario %s/%s", grp->name, sc->name);
            ran++;
            if (!PlayBattle(&sim, label, 1, ScenarioDecide, ScenarioStop, &run))
                failed++;
        }
    }
    printf("crosscheck scenarios: %d run, %d with mismatches\n", ran, failed);
    return failed != 0;
}

// Re-verifies recorded transcripts against the simulator alone.
static int Replay(const char *path, const char *filter)
{
    static struct BattleSim sim;
    static char label[256];
    struct Snapshot snap;
    u32 cfgBadges = 0, cfgScript = 0, cfgCreate = 0;
    int ran = 0, failed = 0, skipping = 1;
    int decisions = 0, simFrom = 0, battleFailed = 0, started = 0;
    u32 romFrom = 0;

    sReplay = fopen(path, "r");
    if (!sReplay) { printf("cannot open %s\n", path); return 2; }
    while (ReadLine())
    {
        if (!strncmp(sLine, "# ", 2))
        {
            if (started && !battleFailed)
                printf("  ok: %d decisions, %d turns (transcript ends: scenario stop)\n", decisions, sim.turnCount);
            started = 0;
            snprintf(label, sizeof(label), "%s", sLine + 2);
            skipping = filter && !strstr(label, filter);
            continue;
        }
        if (skipping) continue;
        if (!strncmp(sLine, "CFG ", 4)) { sscanf(sLine + 4, "%u %u %u", &cfgBadges, &cfgScript, &cfgCreate); continue; }
        if (!strncmp(sLine, "START ", 6))
        {
            u32 flags, trainer, a, b, badges8, c, d, seed;
            char *q;
            int n = 0;
            sscanf(sLine + 6, "%u %u %u %u %u %u %u %u %n", &flags, &trainer, &a, &b, &badges8, &c, &d, &seed, &n);
            q = sLine + 6 + n;
            Sim_Init(&sim, flags, 1);
            sim.logEnabled = TRUE;
            HexToBytes(q, (u8 *)sim.playerParty, 600);
            q += 1200;
            if (cfgScript)
            {
                while (*q == ' ') q++;
                if (*q == '1') { q++; while (*q == ' ') q++; HexToBytes(q, (u8 *)sim.enemyParty, 600); }
            }
            else
                Sim_SetPolicy(&sim, B_SIDE_OPPONENT, Sim_VanillaAIPolicy);
            sim.trainerBattleOpponent_A = trainer;
            sim.badgeFlags = cfgBadges;
            sim.createTrainerParty = cfgCreate;
            sim.rngXorshift = 1;
            sim.rngValue = seed;
            sim.rngCalls = 0;
            gSimRngTraceEnabled = 1;
            gSimRngTraceCount = 0;
            printf("%s: flags %x, seed %x\n", label, flags, seed);
            ran++;
            decisions = 0; simFrom = 0; romFrom = 0; battleFailed = 0; started = 1;
            if (Sim_Start(&sim) != 0) { printf("  invalid setup\n"); failed++; battleFailed = 1; }
            continue;
        }
        if (!started || battleFailed) continue;
        if (!strncmp(sLine, "ACT ", 4))
        {
            struct SimAction act = {0};
            int t, m, tg, ps, it;
            sscanf(sLine + 4, "%d %d %d %d %d", &t, &m, &tg, &ps, &it);
            act.type = t; act.moveSlot = m; act.target = tg; act.partySlot = ps; act.item = it;
            Sim_Answer(&sim, sim.requestBattler, &act);
            sim.logCount = 0;
            simFrom = gSimRngTraceCount;
            romFrom = sRomEngineCalls;
            continue;
        }
        if (!strncmp(sLine, "REQ ", 4) || !strncmp(sLine, "DONE ", 5))
        {
            int ev, r;
            sPushback = 1;
            ev = ReadEvent(&snap);
            r = Sim_Run(&sim);
            if (ev == 1)
            {
                decisions++;
                if (r != SIM_RUN_REQUEST || sim.requestKind != snap.kind || sim.requestBattler != snap.battler)
                {
                    printf("  DESYNC at decision %d: rom asks kind %d battler %d, sim %s kind %d battler %d\n", decisions,
                           snap.kind, snap.battler, r == SIM_RUN_REQUEST ? "asks" : "finished/stuck", sim.requestKind, sim.requestBattler);
                    battleFailed = 1;
                }
            }
            else if (r != SIM_RUN_FINISHED)
            {
                printf("  DESYNC: rom finished (outcome %d) but sim %s\n", snap.outcome, r == SIM_RUN_REQUEST ? "asks for a decision" : "is stuck");
                battleFailed = 1;
            }
            if (sim.rngCalls != sRomEngineCalls)
            {
                printf("  RNG CALL COUNT differs at %s %d: rom engine calls %u, sim %u\n", ev == 1 ? "decision" : "end", decisions, sRomEngineCalls, sim.rngCalls);
                battleFailed = 1;
            }
            if (CompareSnapshot(&sim, &snap))
                battleFailed = 1;
            if (battleFailed)
            {
                DumpRngTraces(romFrom, simFrom);
                printf("  turn %d, sim log:\n", sim.turnCount);
                Sim_PrintLog(&sim);
                Sim_PrintBattlers(&sim);
                failed++;
            }
            else if (ev == 2)
            {
                printf("  ok: %d decisions, %d turns, outcome %d, engine rng calls %u\n", decisions, sim.turnCount, snap.outcome, sRomEngineCalls);
                started = 0;
            }
            continue;
        }
        // STARTED/READY and anything else between events is ignored.
    }
    if (started && !battleFailed)
        printf("  ok: %d decisions, %d turns (transcript ends: scenario stop)\n", decisions, sim.turnCount);
    printf("replay %s: %d battles, %d with mismatches\n", path, ran, failed);
    return failed != 0;
}

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "--replay"))
    {
        setvbuf(stdout, NULL, _IOLBF, 0);
        return Replay(argv[2], argc > 3 ? argv[3] : NULL);
    }
    static struct BattleSim sim;
    int nBattles = argc > 1 ? atoi(argv[1]) : 5;
    int trainerArg = argc > 2 ? atoi(argv[2]) : 0;
    int port = getenv("CROSSCHECK_PORT") ? atoi(getenv("CROSSCHECK_PORT")) : 8899;
    u32 seedBase = getenv("CROSSCHECK_SEED") ? (u32)strtoul(getenv("CROSSCHECK_SEED"), NULL, 0) : 0;
    int fromBattle = getenv("CROSSCHECK_FROM") ? atoi(getenv("CROSSCHECK_FROM")) : 0;
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(port) };
    int b, failed = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    sSock = socket(AF_INET, SOCK_STREAM, 0);
    if (getenv("CROSSCHECK_RECORD") && *getenv("CROSSCHECK_RECORD"))
    {
        sRecord = fopen(getenv("CROSSCHECK_RECORD"), "a");
        if (!sRecord) { printf("cannot open %s for recording\n", getenv("CROSSCHECK_RECORD")); return 2; }
        setvbuf(sRecord, NULL, _IOFBF, 1 << 16);
    }
    if (getenv("CROSSCHECK_DRYRUN"))
        sSock = -1; // print the battles of a seeded batch without touching the ROM
    else if (connect(sSock, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        printf("cannot connect to mGBA harness on port %d (load harness/harness_gen.lua in Tools > Scripting first)\n", port);
        return 2;
    }
    if (argc > 1 && !strcmp(argv[1], "--scenarios"))
        return RunScenarios(argc > 2 ? argv[2] : NULL);

    for (b = 0; b < nBattles; b++)
    {
        char label[64];
        u16 trainer;
        // Every battle of a seeded batch draws from its own host RNG stream so that CROSSCHECK_FROM and
        // CROSSCHECK_DRYRUN reproduce battle b exactly, regardless of the decisions made in earlier battles.
        sHostRng = 777u ^ (seedBase * 2654435761u) ^ ((u32)b * 0x9E3779B9u);
        trainer = trainerArg ? trainerArg : 1 + R() % 742;
        u32 flags = BATTLE_TYPE_TRAINER;
        u32 rngSeed = 0x1000 + b * 7919 + seedBase * 104729;

        // Skip placeholder trainers.
        Sim_Init(&sim, flags, 1);
        if (Sim_LoadTrainerParty(&sim, trainer) == 0 && GetMonData(&sim.enemyParty[0], MON_DATA_SPECIES) == SPECIES_NONE)
        { b--; continue; }
        flags = sim.battleTypeFlags;

        Sim_Init(&sim, flags, 1);
        sim.logEnabled = TRUE;
        MakePlayerParty(&sim, (flags & BATTLE_TYPE_DOUBLE) ? 2 + R() % 5 : 1 + R() % 6);
        Sim_LoadTrainerParty(&sim, trainer); // preview copy; Sim_Start re-creates the party at the game's point
        Sim_SetPolicy(&sim, B_SIDE_OPPONENT, Sim_VanillaAIPolicy);
        sim.rngXorshift = 1;
        sim.rngValue = rngSeed;
        sim.rngCalls = 0;
        snprintf(label, sizeof(label), "battle %d: trainer %u", b, trainer);
        if (b < fromBattle)
            continue;
        if (sSock < 0)
        {
            int k;
            printf("%s: flags %x, seed %x\n", label, flags, rngSeed);
            for (k = 0; k < PARTY_SIZE && GetMonData(&sim.playerParty[k], MON_DATA_SPECIES); k++)
                printf("  player[%d] %s L%d item %s moves %s %s %s %s\n", k, gSimSpeciesNames[GetMonData(&sim.playerParty[k], MON_DATA_SPECIES)],
                       GetMonData(&sim.playerParty[k], MON_DATA_LEVEL), gSimItemNames[GetMonData(&sim.playerParty[k], MON_DATA_HELD_ITEM)],
                       gSimMoveNames[GetMonData(&sim.playerParty[k], MON_DATA_MOVE1)], gSimMoveNames[GetMonData(&sim.playerParty[k], MON_DATA_MOVE2)],
                       gSimMoveNames[GetMonData(&sim.playerParty[k], MON_DATA_MOVE3)], gSimMoveNames[GetMonData(&sim.playerParty[k], MON_DATA_MOVE4)]);
            for (k = 0; k < PARTY_SIZE && GetMonData(&sim.enemyParty[k], MON_DATA_SPECIES); k++)
                printf("  enemy[%d] %s L%d item %s\n", k, gSimSpeciesNames[GetMonData(&sim.enemyParty[k], MON_DATA_SPECIES)],
                       GetMonData(&sim.enemyParty[k], MON_DATA_LEVEL), gSimItemNames[GetMonData(&sim.enemyParty[k], MON_DATA_HELD_ITEM)]);
            continue;
        }
        if (!PlayBattle(&sim, label, 0, RandomDecide, NULL, NULL))
            failed++;
    }
    printf("crosscheck: %d battles, %d with mismatches\n", nBattles, failed);
    return failed != 0;
}
