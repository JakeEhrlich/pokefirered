// Play random battles yourself in mGBA against a simulator agent (or the game's own AI).
//
//   play --agent rmplus:iters=1000,samples=16 [--teams pool.tsv] [--you TEAMID --foe TEAMID] [--port 8899] [--seed N] [--games N] [--doubles]
//        [--verbose] [--anims]   (move animations are off by default; text speed is always fast)
//
// The harness ROM runs in human mode: you use the real menus; every choice you commit is reported to this
// program, which mirrors the battle in the simulator (same parties, same engine RNG) and asks the agent for the
// opponent's decisions. The mirror is checked against the ROM at every opponent decision. With
// --agent game:flags=smart the ROM's trainer AI plays and this program only mirrors.
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
#include "sim_agent.h"
#include "sim_names.h"
#include "constants/species.h"
#include "constants/moves.h"
#include "constants/items.h"

static int sSock = -1;
static char sLine[8192];
static char sRxBuf[65536];
static int sRxLen;

static int ReadLine(void)
{
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
            return 1;
        }
        {
            int r = recv(sSock, sRxBuf + sRxLen, sizeof(sRxBuf) - sRxLen, 0);
            if (r <= 0) return 0;
            sRxLen += r;
        }
    }
}

static void SendLine(const char *s) { send(sSock, s, strlen(s), 0); send(sSock, "\n", 1, 0); }

static int HexToBytes(const char *hex, u8 *out, int max)
{
    int n = 0;
    while (hex[0] && hex[1] && n < max) { unsigned v; sscanf(hex, "%2x", &v); out[n++] = v; hex += 2; }
    return n;
}

struct Team { char id[64]; u8 doubles; u8 n; struct Pokemon party[PARTY_SIZE]; };
static struct Team *sTeams;
static int sTeamCount;
static u32 sRng = 1;
static u32 R(void) { sRng ^= sRng << 13; sRng ^= sRng >> 17; sRng ^= sRng << 5; return sRng; }

static void PrintTeam(const char *label, struct Team *t)
{
    int i, m;
    printf("%s (%s):\n", label, t->id);
    for (i = 0; i < t->n; i++)
    {
        struct Pokemon *mon = &t->party[i];
        printf("  %-11s L%-3d %-14s", gSimSpeciesNames[GetMonData(mon, MON_DATA_SPECIES)], GetMonData(mon, MON_DATA_LEVEL),
               gSimItemNames[GetMonData(mon, MON_DATA_HELD_ITEM)]);
        for (m = 0; m < 4; m++)
        {
            u16 mv = GetMonData(mon, MON_DATA_MOVE1 + m);
            if (mv) printf(" %s", gSimMoveNames[mv]);
        }
        printf("\n");
    }
}

// Reads a REQ/DONE block; returns 1 for REQ, 2 for DONE, 3 for PLAYER, 0 on error. Fills the snapshot fields.
static u8 sMons[88 * 4], sPParty[600], sEParty[600];
static int sReqKind, sReqBattler, sOutcome;
static int sPlayer[6];

static int ReadEvent(void)
{
    int type = 0;
    for (;;)
    {
        if (!ReadLine()) return 0;
        if (!strncmp(sLine, "REQ ", 4)) { type = 1; sscanf(sLine + 4, "%d %d", &sReqKind, &sReqBattler); }
        else if (!strncmp(sLine, "DONE ", 5)) { type = 2; sscanf(sLine + 5, "%d", &sOutcome); }
        else if (!strncmp(sLine, "PLAYER ", 7)) { sscanf(sLine + 7, "%d %d %d %d %d %d", &sPlayer[0], &sPlayer[1], &sPlayer[2], &sPlayer[3], &sPlayer[4], &sPlayer[5]); return 3; }
        else if (!strncmp(sLine, "MONS ", 5)) HexToBytes(sLine + 5, sMons, sizeof(sMons));
        else if (!strncmp(sLine, "PPARTY ", 7)) HexToBytes(sLine + 7, sPParty, sizeof(sPParty));
        else if (!strncmp(sLine, "EPARTY ", 7)) HexToBytes(sLine + 7, sEParty, sizeof(sEParty));
        else if (!strcmp(sLine, "END")) return type;
    }
}

// Loose mirror check: HP and status of every battler and party mon.
static void CheckMirror(struct BattleSim *sim, int decision)
{
    int i, bad = 0;
    for (i = 0; i < sim->battlersCount; i++)
    {
        u16 hp = sMons[88 * i + 40] | (sMons[88 * i + 41] << 8);
        u16 species = sMons[88 * i] | (sMons[88 * i + 1] << 8);
        if (hp != sim->battleMons[i].hp || species != sim->battleMons[i].species)
        {
            printf("  [mirror] battler %d differs: rom %s hp %d, sim %s hp %d\n", i, gSimSpeciesNames[species], hp,
                   gSimSpeciesNames[sim->battleMons[i].species], sim->battleMons[i].hp);
            bad = 1;
        }
    }
    for (i = 0; i < PARTY_SIZE; i++)
    {
        struct Pokemon *rp = (struct Pokemon *)(sPParty + i * 100), *ep = (struct Pokemon *)(sEParty + i * 100);
        if (GetMonData(&sim->playerParty[i], MON_DATA_SPECIES) && rp->hp != GetMonData(&sim->playerParty[i], MON_DATA_HP)) { printf("  [mirror] your mon %d hp: rom %d sim %d\n", i, rp->hp, GetMonData(&sim->playerParty[i], MON_DATA_HP)); bad = 1; }
        if (GetMonData(&sim->enemyParty[i], MON_DATA_SPECIES) && ep->hp != GetMonData(&sim->enemyParty[i], MON_DATA_HP)) { printf("  [mirror] foe mon %d hp: rom %d sim %d\n", i, ep->hp, GetMonData(&sim->enemyParty[i], MON_DATA_HP)); bad = 1; }
    }
    if (bad) printf("  [mirror] diverged at opponent decision %d; the agent now plays on an inexact state\n", decision);
}

static void ActionName(struct BattleSim *sim, u8 battler, const struct SimAction *a, char *buf, int len)
{
    if (a->type == B_ACTION_USE_MOVE) snprintf(buf, len, "%s", gSimMoveNames[sim->battleMons[battler].moves[a->moveSlot & 3]]);
    else if (a->type == B_ACTION_SWITCH) snprintf(buf, len, "switch to %s", gSimSpeciesNames[GetMonData(&Sim_Party(sim, battler & 1)[a->partySlot], MON_DATA_SPECIES)]);
    else snprintf(buf, len, "action %d", a->type);
}

int main(int argc, char **argv)
{
    static struct BattleSim sim, turnStart;
    struct SimAgent agent;
    const char *spec = "rmplus:iters=1000,samples=16", *teamsPath = "/tmp/randbats.tsv";
    const char *youId = NULL, *foeId = NULL;   // --you / --foe: pick teams by id instead of at random
    int port = getenv("CROSSCHECK_PORT") ? atoi(getenv("CROSSCHECK_PORT")) : 8899;
    int games = 1000000, g, i, allowDoubles = 0, verbose = 0, anims = 0;
    struct sockaddr_in addr = { .sin_family = AF_INET };
    static char msg[4096];

    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--agent") && i + 1 < argc) spec = argv[++i];
        else if (!strcmp(argv[i], "--you") && i + 1 < argc) youId = argv[++i];
        else if (!strcmp(argv[i], "--foe") && i + 1 < argc) foeId = argv[++i];
        else if (!strcmp(argv[i], "--teams") && i + 1 < argc) teamsPath = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) sRng = (u32)strtoul(argv[++i], NULL, 0) | 1;
        else if (!strcmp(argv[i], "--games") && i + 1 < argc) games = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--doubles")) allowDoubles = 1;
        else if (!strcmp(argv[i], "--verbose")) verbose = 1;
        else if (!strcmp(argv[i], "--anims")) anims = 1;
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (Sim_AgentFromSpec(&agent, spec) != 0) { fprintf(stderr, "bad agent: %s\n", agent.name); return 2; }
    Sim_Init(&sim, 0, 1);
    {
        FILE *f = fopen(teamsPath, "r");
        if (!f) { fprintf(stderr, "cannot open %s (python3 tools/teams.py tsv teams/showdown_randbats.jsonl > %s)\n", teamsPath, teamsPath); return 2; }
        sTeams = calloc(20000, sizeof(*sTeams));
        for (;;)
        {
            struct Team *t = &sTeams[sTeamCount];
            int n = Sim_ReadTeamTsv(f, t->party, t->id, sizeof(t->id), &t->doubles);
            if (n == 0) break;
            t->n = n;
            if (t->doubles && !allowDoubles) continue;
            sTeamCount++;
        }
        fclose(f);
    }
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    sSock = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(sSock, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        printf("cannot connect to mGBA on port %d: open pokefirered_modern_harness.gba in mGBA and load sim/harness/harness_gen.lua (Tools > Scripting)\n", port);
        return 2;
    }
    printf("opponent: %s   (%d teams in the pool)\n", agent.name, sTeamCount);

    for (g = 0; g < games; g++)
    {
        struct Team *you = &sTeams[R() % sTeamCount], *foe = &sTeams[R() % sTeamCount];
        if (youId || foeId)
        {
            int k;
            for (k = 0; k < sTeamCount; k++)
            {
                if (youId && !strcmp(sTeams[k].id, youId)) you = &sTeams[k];
                if (foeId && !strcmp(sTeams[k].id, foeId)) foe = &sTeams[k];
            }
        }
        u32 seed = R() | 1;
        u32 flags = BATTLE_TYPE_TRAINER | ((you->doubles || foe->doubles) ? BATTLE_TYPE_DOUBLE : 0);
        struct SimAction human = {0}, oppPending = {0};
        int humanStage = 0, haveOpp = 0, decisions = 0, ev, res;
        char *p;

        printf("\n=== game %d (seed %x) ===\n", g + 1, seed);
        PrintTeam("YOU", you);
        PrintTeam("FOE", foe);

        Sim_Init(&sim, flags, 1);
        sim.badgeFlags = 0;   // no badge boosts for the human side either: same rules as the arena
        memcpy(sim.playerParty, you->party, sizeof(sim.playerParty));
        memcpy(sim.enemyParty, foe->party, sizeof(sim.enemyParty));
        sim.createTrainerParty = FALSE;
        sim.trainerBattleOpponent_A = Sim_TrainerForAIFlags(agent.isGameAI ? agent.aiFlags : 7);
        if (sim.trainerBattleOpponent_A == 0) sim.trainerBattleOpponent_A = 1;
        sim.rngXorshift = 1;
        sim.rngValue = seed;
        sim.rngCalls = 0;
        sim.strictAnswers = 0;
        sim.maxTurns = 0;
        if (agent.isGameAI) Sim_SetPolicy(&sim, B_SIDE_OPPONENT, Sim_VanillaAIPolicy);
        agent.rng = seed * 2654435761u + 7;

        // fields: flags trainer 1 terrain badges battleStyle(1 = SET, what the simulator models) sceneOff(0 = animations) seed
        p = msg + sprintf(msg, "START %u %u %u %u %u %u %u %u ", flags, sim.trainerBattleOpponent_A, 1, 0, sim.badgeFlags & 0xFF, 1, anims ? 0 : 1, seed);
        for (i = 0; i < 600; i++) p += sprintf(p, "%02x", ((u8 *)sim.playerParty)[i]);
        p += sprintf(p, " %d ", agent.isGameAI ? 0 : 1);
        for (i = 0; i < 600; i++) p += sprintf(p, "%02x", ((u8 *)sim.enemyParty)[i]);
        sprintf(p, " 1");
        if (Sim_Start(&sim) != 0) { printf("  (invalid setup, skipping)\n"); continue; }
        SendLine(msg);
        res = Sim_Run(&sim);   // your first decision is now pending in the mirror

        for (;;)
        {
            ev = ReadEvent();
            if (ev == 0) { printf("connection lost\n"); return 3; }
            if (ev == 2)
            {
                printf("=== %s (%d turns) ===\n", sOutcome == B_OUTCOME_WON ? "YOU WIN" : sOutcome == B_OUTCOME_LOST ? "YOU LOSE" : "DRAW", sim.turnCount);
                break;
            }
            if (ev == 1)
            {
                // the opponent's decision (never in game-AI mode)
                struct SimAction act;
                char name[64];
                decisions++;
                CheckMirror(&sim, decisions);
                if (sReqKind == SIM_REQ_ACTION)
                {
                    memcpy(&turnStart, &sim, sizeof(sim));
                    agent.decide(&agent, &sim, (sim.requestKind == SIM_REQ_ACTION && sim.requestBattler == 0) ? &turnStart : NULL, sReqBattler, SIM_REQ_ACTION, &act);
                    oppPending = act; haveOpp = 1;
                    if (res == SIM_RUN_REQUEST && sim.requestKind == SIM_REQ_ACTION && sim.requestBattler == sReqBattler)
                    {
                        Sim_Answer(&sim, sReqBattler, &act); res = Sim_Run(&sim); haveOpp = 0;
                    }
                }
                else
                {
                    agent.decide(&agent, &sim, NULL, sReqBattler, SIM_REQ_SWITCH, &act);
                    if (res == SIM_RUN_REQUEST && sim.requestKind == SIM_REQ_SWITCH && sim.requestBattler == sReqBattler)
                    {
                        Sim_Answer(&sim, sReqBattler, &act); res = Sim_Run(&sim);
                    }
                    else { oppPending = act; haveOpp = 1; }
                }
                // Never trust the mirror blindly: check the choice against the ROM's own party snapshot.
                if (act.type == B_ACTION_SWITCH)
                {
                    struct Pokemon *ep = (struct Pokemon *)(sEParty + act.partySlot * 100);
                    u16 cur = sMons[88 * sReqBattler + 0] | (sMons[88 * sReqBattler + 1] << 8);
                    if (act.partySlot >= PARTY_SIZE || ep->hp == 0 || GetMonData(ep, MON_DATA_SPECIES) == SPECIES_NONE || GetMonData(ep, MON_DATA_SPECIES) == cur)
                    {
                        int k, found = -1;
                        for (k = 0; k < PARTY_SIZE; k++)
                        {
                            struct Pokemon *m = (struct Pokemon *)(sEParty + k * 100);
                            if (m->hp && GetMonData(m, MON_DATA_SPECIES) && GetMonData(m, MON_DATA_SPECIES) != cur) { found = k; break; }
                        }
                        printf("  [mirror] the agent chose party slot %d which the ROM says is unusable; using slot %d instead\n", act.partySlot, found);
                        if (found >= 0) act.partySlot = found;
                        else { act.type = B_ACTION_USE_MOVE; act.moveSlot = 0; act.target = 0xFF; }
                    }
                }
                if (verbose)
                    printf("  [req] kind %d battler %d | mirror: res %d request kind %d battler %d turn %d\n", sReqKind, sReqBattler, res, sim.requestKind, sim.requestBattler, sim.turnCount);
                ActionName(&sim, sReqBattler, &act, name, sizeof(name));
                printf("  foe decides: %s\n", name);
                sprintf(msg, "ACT %d %d %d %d %d", act.type, act.moveSlot, act.target, act.partySlot, act.item);
                SendLine(msg);
                continue;
            }
            // ev == 3: a report of your own choice
            {
                int kind = sPlayer[0], complete = 0;
                if (verbose)
                    printf("  [report] kind %d type %d slot %d target %d item %d partyIdx %d | mirror: res %d request kind %d battler %d turn %d stage %d haveOpp %d\n",
                           sPlayer[0], sPlayer[1], sPlayer[2], sPlayer[3], sPlayer[4], sPlayer[5], res, sim.requestKind, sim.requestBattler, sim.turnCount, humanStage, haveOpp);
                if (kind == 1)
                {
                    human.type = sPlayer[1]; human.target = 0xFF; humanStage = 1;
                    if (human.type == B_ACTION_USE_MOVE)
                    {
                        // locked or move-less mons skip the move menu in the game
                        Sim_Bind(&sim);
                        if (sim.battleMons[0].status2 & (STATUS2_MULTIPLETURNS | STATUS2_RECHARGE)) { human.moveSlot = 0; complete = 1; }
                        else if (sim.disableStructs[0].encoredMove != MOVE_NONE) { human.moveSlot = sim.disableStructs[0].encoredMovePos; complete = 1; }
                        else
                        {
                            struct SimAction acts[32];
                            int n = Sim_LegalActions(&sim, 0, acts, 32), moves = 0, k;
                            for (k = 0; k < n; k++) if (acts[k].type == B_ACTION_USE_MOVE) moves++;
                            if (moves == 1 && acts[0].moveSlot == 0 && CheckMoveLimitations(0, 0, 0xFF) == 0xF) { human.moveSlot = 0; complete = 1; } // Struggle
                        }
                    }
                    else if (human.type == B_ACTION_RUN) complete = 1;
                }
                else if (kind == 2 && humanStage == 1)
                {
                    if (sPlayer[2] == 0xFF && sPlayer[3] == 0xFF) { humanStage = 0; continue; } // backed out of the move menu
                    human.moveSlot = sPlayer[2]; human.target = sPlayer[3]; complete = 1;
                }
                else if (kind == 3)
                {
                    if (sPlayer[2] >= PARTY_SIZE) { humanStage = 0; continue; }   // cancelled the party screen
                    human.type = B_ACTION_SWITCH; human.partySlot = sPlayer[2]; complete = 1;
                }
                else if (kind == 4)
                {
                    if (sPlayer[4] == 0) { humanStage = 0; continue; }           // closed the bag
                    human.type = B_ACTION_USE_ITEM; human.item = sPlayer[4]; human.partySlot = sPlayer[5] < PARTY_SIZE ? sPlayer[5] : 0xFF; complete = 1;
                }
                if (!complete) continue;
                humanStage = 0;
                if (res == SIM_RUN_REQUEST && sim.requestBattler == 0)
                {
                    if (Sim_Answer(&sim, 0, &human) != 0) printf("  [mirror] your choice was not accepted by the simulator\n");
                    res = Sim_Run(&sim);
                    // the foe already committed (the ROM asked it while you were in a menu): feed its stored answer
                    if (res == SIM_RUN_REQUEST && sim.requestBattler != 0 && haveOpp
                     && (sim.requestKind == SIM_REQ_ACTION || oppPending.type == B_ACTION_SWITCH))
                    {
                        Sim_Answer(&sim, sim.requestBattler, &oppPending); haveOpp = 0; res = Sim_Run(&sim);
                    }
                }
                else
                    printf("  [mirror] unexpected report (kind %d) while the simulator waits for battler %d\n", kind, sim.requestBattler);
            }
        }
        printf("(next game starts in a moment; Ctrl-C to stop)\n");
        sleep(4);
    }
    return 0;
}
