// Differential test of the fast engine (fast/) against the verbatim engine: sample real positions from games,
// apply the same joint action in both engines many times, and compare the outcome distributions.
//
//   fastdiff --teams pool.tsv [--states 300] [--samples 2000] [--seed 1] [--verbose]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "global.h"
#include "battle.h"
#include "pokemon.h"
#include "sim.h"
#include "sim_agent.h"
#include "sim_capi.h"
#include "fast.h"
int fs_effect_supported(u8 effect); int fs_ability_supported(u8 ability);
#include "constants/species.h"
#include "sim_globals.h"

#define MAX_TEAMS 40000
struct Team { char id[64]; u8 doubles; u8 n; struct Pokemon party[PARTY_SIZE]; };
static struct Team *sTeams; static int sTeamCount;

static int LoadTeams(const char *path)
{
    FILE *f = fopen(path, "r"); if (!f) return -1;
    sTeams = calloc(MAX_TEAMS, sizeof(*sTeams));
    for (;;) { struct Team *t = &sTeams[sTeamCount]; int n = Sim_ReadTeamTsv(f, t->party, t->id, sizeof(t->id), &t->doubles); if (n == 0) break; t->n = n; if (!t->doubles && ++sTeamCount >= MAX_TEAMS) break; }
    fclose(f); return sTeamCount;
}

// outcome summary: the observable state after the step, from a fast state (verbatim results are imported first)
struct Summary
{
    u64 h;
    u8 req, mask, outcome, weather, weatherTurns, phase;
    struct { u8 present, mon; u16 hp, st; s8 stg[8]; u32 vol; u8 sub, taunt, encore, disable, perish, yawn, wrap, conf;
             u8 reflect, ls, safeguard, mist, spikes, wish, fsight; u16 php[6], pst[6]; } sd[2];
};
static u64 Mix(u64 h, u64 v) { h ^= v; h *= 0x9E3779B97F4A7C15ull; return h ^ (h >> 29); }
static void Summarize(const fs_state *s, struct Summary *o)
{
    int sd, i; u64 h = 0x1234567ull;
    memset(o, 0, sizeof(*o));
    o->req = s->request; o->mask = s->switchMask; o->outcome = s->outcome; o->weather = s->weather; o->weatherTurns = s->weatherTurns;
    for (sd = 0; sd < 2; sd++)
    {
        const fs_battler *a = &s->side[sd].act; const fs_side *side = &s->side[sd];
        o->sd[sd].present = a->present; o->sd[sd].mon = a->present ? a->monIdx : 0xFF; o->sd[sd].hp = a->present ? a->hp : 0;
        o->sd[sd].st = a->present ? ((a->status1 & ~FS_S1_SLEEP) | ((a->status1 & FS_S1_SLEEP) ? 1 : 0)) : 0;   // the sleep count is hidden information
        if (a->present) memcpy(o->sd[sd].stg, a->stages, 8); else memset(o->sd[sd].stg, 6, 8);
        o->sd[sd].vol = a->present ? (a->vol & ~(FS_V_MOVED_THIS_TURN | FS_V_PROTECTED | FS_V_ENDURED | FS_V_FLINCH | FS_V_FLASH_FIRE)) : 0;
        o->sd[sd].sub = a->present ? a->substituteHP : 0; o->sd[sd].taunt = a->present ? a->tauntTimer : 0; o->sd[sd].encore = a->present ? a->encoreTimer : 0;
        o->sd[sd].disable = a->present ? a->disableTimer : 0; o->sd[sd].perish = a->present ? a->perishTimer : 0; o->sd[sd].yawn = a->present ? a->yawnTimer : 0;
        o->sd[sd].wrap = a->present ? (a->wrapTurns != 0) : 0; o->sd[sd].conf = a->present ? ((a->vol & FS_V_CONFUSED) != 0) : 0;
        o->sd[sd].reflect = side->reflect; o->sd[sd].ls = side->lightscreen; o->sd[sd].safeguard = side->safeguard; o->sd[sd].mist = side->mist; o->sd[sd].spikes = side->spikes;
        o->sd[sd].wish = side->wishTurns; o->sd[sd].fsight = side->futureSightTurns;
        for (i = 0; i < 6; i++) { o->sd[sd].php[i] = side->party[i].hp; o->sd[sd].pst[i] = (side->party[i].status1 & ~FS_S1_SLEEP) | ((side->party[i].status1 & FS_S1_SLEEP) ? 1 : 0); }
    }
    {
        const u8 *p = (const u8 *)o + sizeof(o->h);
        for (i = 0; i < (int)(sizeof(*o) - sizeof(o->h)); i++) h = Mix(h, p[i]);
    }
    o->h = h;
}

// prints the fields that differ between two summaries
static void PrintDiff(const struct Summary *a, const struct Summary *b)
{
    int sd, i;
#define D(name, x) if ((a->x) != (b->x)) printf(" %s %d/%d", name, (int)(a->x), (int)(b->x))
    D("req", req); D("mask", mask); D("outcome", outcome); D("weather", weather); D("wturns", weatherTurns);
    for (sd = 0; sd < 2; sd++)
    {
        char nm[32];
#define DS(name, x) if ((a->sd[sd].x) != (b->sd[sd].x)) printf(" P%d.%s %d/%d", sd, name, (int)(a->sd[sd].x), (int)(b->sd[sd].x))
        DS("present", present); DS("mon", mon); DS("hp", hp); DS("st", st); DS("vol", vol); DS("sub", sub); DS("taunt", taunt); DS("encore", encore); DS("disable", disable);
        DS("perish", perish); DS("yawn", yawn); DS("wrap", wrap); DS("conf", conf); DS("reflect", reflect); DS("ls", ls); DS("safeguard", safeguard); DS("mist", mist); DS("spikes", spikes); DS("wish", wish); DS("fsight", fsight);
        for (i = 0; i < 8; i++) { snprintf(nm, sizeof(nm), "stg%d", i); if (a->sd[sd].stg[i] != b->sd[sd].stg[i]) printf(" P%d.%s %d/%d", sd, nm, a->sd[sd].stg[i] - 6, b->sd[sd].stg[i] - 6); }
        for (i = 0; i < 6; i++) { if (a->sd[sd].php[i] != b->sd[sd].php[i]) printf(" P%d.party%dhp %d/%d", sd, i, a->sd[sd].php[i], b->sd[sd].php[i]); if (a->sd[sd].pst[i] != b->sd[sd].pst[i]) printf(" P%d.party%dst %x/%x", sd, i, a->sd[sd].pst[i], b->sd[sd].pst[i]); }
    }
#undef D
#undef DS
}

struct Bucket { u64 h; int n[2]; struct Summary ex; };
#define MAX_BUCKETS 4096
static struct Bucket sB[MAX_BUCKETS]; static int sNB;
static void Add(int which, const struct Summary *sm)
{
    int i; for (i = 0; i < sNB; i++) if (sB[i].h == sm->h) { sB[i].n[which]++; return; }
    if (sNB < MAX_BUCKETS) { sB[sNB].h = sm->h; sB[sNB].n[0] = sB[sNB].n[1] = 0; sB[sNB].n[which] = 1; sB[sNB].ex = *sm; sNB++; }
}

static int sFocusLog;
static u32 resultImportFail;   // FS_UNSUP_* mask of verbatim result states the fast import could not represent
static void RunVerbatim(const struct BattleSim *ts, const struct SimAction *a0, const struct SimAction *a1, u32 seed, struct BattleSim *out)
{
    memcpy(out, ts, sizeof(*out)); out->policy[0] = out->policy[1] = NULL; out->strictAnswers = 0; out->logEnabled = sFocusLog; out->logCount = 0; out->rngXorshift = 1; out->rngValue = seed | 1;
    if (ts->requestKind == SIM_REQ_SWITCH)
    {
        u8 b = ts->requestBattler; int r;
        if (Sim_Answer(out, b, b == 0 ? a0 : a1) != 0) { out->finished = 1; out->battleOutcome = B_OUTCOME_DREW; return; }
        r = Sim_Run(out); if (r == SIM_RUN_STUCK || r == SIM_RUN_ERROR) { out->finished = 1; out->battleOutcome = B_OUTCOME_DREW; }
        return;
    }
    {
        int r;
        if (Sim_Answer(out, 0, a0) != 0) { out->finished = 1; out->battleOutcome = B_OUTCOME_DREW; return; }
        r = Sim_Run(out);
        if (r == SIM_RUN_REQUEST && out->requestBattler == 1 && out->requestKind == SIM_REQ_ACTION)
        { if (Sim_Answer(out, 1, a1) != 0) { out->finished = 1; out->battleOutcome = B_OUTCOME_DREW; return; } r = Sim_Run(out); }
        if (r == SIM_RUN_STUCK || r == SIM_RUN_ERROR) { out->finished = 1; out->battleOutcome = B_OUTCOME_DREW; }
    }
}

static void PrintMon(const fs_battler *a) { printf("%s L%d hp %d/%d st %x stg", sim_species_name(a->species), a->level, a->hp, a->maxHP, a->status1); for (int i = 1; i < 6; i++) printf(" %d", a->stages[i] - 6); }

int main(int argc, char **argv)
{
    const char *teamsPath = NULL; int nStates = 300, nSamples = 2000, verbose = 0, i, nFocus = 0, focusGame[16], focusTurn[16]; u32 seed = 1;
    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--teams") && i + 1 < argc) teamsPath = argv[++i];
        else if (!strcmp(argv[i], "--states") && i + 1 < argc) nStates = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--samples") && i + 1 < argc) nSamples = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (u32)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--verbose")) verbose = 1;
        else if (!strcmp(argv[i], "--focus") && i + 1 < argc) { if (nFocus < 16 && sscanf(argv[++i], "%d:%d", &focusGame[nFocus], &focusTurn[nFocus]) == 2) nFocus++; }
    }
    if (!teamsPath) { fprintf(stderr, "--teams required\n"); return 2; }
    { static struct BattleSim boot; Sim_Init(&boot, 0, 1); }
    if (LoadTeams(teamsPath) < 2) return 2;
    {
        struct BattleSim *sim = malloc(sizeof(*sim)), *ts = malloc(sizeof(*sim)), *out = malloc(sizeof(*sim));
        struct SimAgent ag; Sim_AgentFromSpec(&ag, "rmplus:iters=10"); 
        u32 x = seed * 2654435761u + 12345; int tested = 0, unsup = 0, mismatched = 0, legalMismatch = 0, importFail = 0, g = 0;
        long unsupReasons[8] = {0}; long unsupEffect[256] = {0}, unsupAbility[128] = {0};
        while (tested < nStates)
        {
            int ta, tb, guard = 0;
            x ^= x << 13; x ^= x >> 17; x ^= x << 5; ta = x % sTeamCount; x ^= x << 13; x ^= x >> 17; x ^= x << 5; tb = x % sTeamCount; if (tb == ta) tb = (ta + 1) % sTeamCount;
            Sim_Init(sim, BATTLE_TYPE_TRAINER, 1);
            memcpy(sim->playerParty, sTeams[ta].party, sizeof(sim->playerParty)); memcpy(sim->enemyParty, sTeams[tb].party, sizeof(sim->enemyParty));
            sim->createTrainerParty = FALSE; sim->trainerBattleOpponent_A = Sim_TrainerForAIFlags(7); if (!sim->trainerBattleOpponent_A) sim->trainerBattleOpponent_A = 1;
            sim->rngXorshift = 1; sim->rngValue = (x | 1); sim->strictAnswers = 1; sim->maxTurns = 300; sim->badgeFlags = 0; sim->exactFrames = 0;
            ag.rng = x | 1; g++;
            if (Sim_Start(sim) != 0) continue;
            for (;;)
            {
                int r = Sim_Run(sim); u8 b, kind; struct SimAction act;
                if (r != SIM_RUN_REQUEST || ++guard > 2000) break;
                b = sim->requestBattler; kind = sim->requestKind;
                if (kind == SIM_REQ_ACTION && b == 0) memcpy(ts, sim, sizeof(*ts));
                // test this decision point with probability 1/6 (turn starts and forced switches)
                if ((kind == SIM_REQ_ACTION && b == 0) || kind == SIM_REQ_SWITCH)
                {
                    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
                    if (x % 6 == 0 && tested < nStates)
                    {
                        fs_state fs; struct SimAction va0, va1; fs_action fa0 = {0, 0}, fa1 = {0, 0};
                        struct SimAction legal0[32], legal1[32]; int n0 = 0, n1 = 0; u8 slots[6];
                        const struct BattleSim *src = kind == SIM_REQ_ACTION ? ts : sim;
                        if (kind == SIM_REQ_SWITCH && verbose)
                        {
                            extern void BattleTurnPassed(void);
                            Sim_Bind(sim);
                            printf("switch request game %d turn %d battler %d: mainFunc %s, curAction %d, order [%d:%d %d:%d], chosenMoves %d/%d, fainted hp %d/%d\n", g, sim->turnCount, b,
                                   gBattleMainFunc == BattleTurnPassed ? "BattleTurnPassed" : "other", gCurrentTurnActionNumber, gBattlerByTurnOrder[0], gActionsByTurnOrder[0], gBattlerByTurnOrder[1], gActionsByTurnOrder[1],
                                   gChosenMoveByBattler[0], gChosenMoveByBattler[1], gBattleMons[0].hp, gBattleMons[1].hp);
                        }
                        if (fs_import(&fs, src) != 0)
                        {
                            if (fs.unsupported & FS_UNSUP_STATE) importFail++; else unsup++;
                            for (int k = 0; k < 8; k++) if (fs.unsupported & (1u << k)) unsupReasons[k]++;
                            for (int sd = 0; sd < 2; sd++) for (int mi = 0; mi < 6; mi++) { const fs_mon *m = &fs.side[sd].party[mi]; if (!m->species) continue;
                                for (int k = 0; k < 4; k++) if (m->moves[k] && !fs_effect_supported(gBattleMoves[m->moves[k]].effect)) unsupEffect[gBattleMoves[m->moves[k]].effect]++;
                                if (!fs_ability_supported(m->ability)) unsupAbility[m->ability]++; }
                            goto decide;
                        }
                        // legal actions comparison + random joint action
                        if (kind == SIM_REQ_ACTION)
                        {
                            fs_action fl0[32], fl1[32]; int m0 = fs_legal_actions(&fs, 0, fl0), m1 = fs_legal_actions(&fs, 1, fl1);
                            n0 = Sim_LegalActions((struct BattleSim *)ts, 0, legal0, 32); n1 = Sim_LegalActions((struct BattleSim *)ts, 1, legal1, 32);
                            if (m0 != n0 || m1 != n1)
                            {
                                legalMismatch++;
                                printf("LEGAL MISMATCH game %d turn %d: fast %d/%d verbatim %d/%d  P0 %s (%s/%s/%s/%s pp %d/%d/%d/%d taunt %d enc %d dis %d vol %x) P1 %s (vol %x abil %d)\n", g, sim->turnCount, m0, m1, n0, n1,
                                       sim_species_name(fs.side[0].act.species), fs_move_name(fs.side[0].act.moves[0]), fs_move_name(fs.side[0].act.moves[1]), fs_move_name(fs.side[0].act.moves[2]), fs_move_name(fs.side[0].act.moves[3]),
                                       fs.side[0].act.pp[0], fs.side[0].act.pp[1], fs.side[0].act.pp[2], fs.side[0].act.pp[3], fs.side[0].act.tauntTimer, fs.side[0].act.encoreTimer, fs.side[0].act.disableTimer, fs.side[0].act.vol,
                                       sim_species_name(fs.side[1].act.species), fs.side[1].act.vol, fs.side[1].act.ability);
                                for (int sd = 0; sd < 2; sd++)
                                {
                                    Sim_Bind((struct BattleSim *)ts);
                                    printf("   verbatim P%d battle: status2 %x s3 %x pp %d/%d/%d/%d disabled %d(t%d) encored %d(pos %d t%d) taunt %d limits %x moves %d/%d/%d/%d\n", sd, gBattleMons[sd].status2, gStatuses3[sd],
                                           gBattleMons[sd].pp[0], gBattleMons[sd].pp[1], gBattleMons[sd].pp[2], gBattleMons[sd].pp[3], gDisableStructs[sd].disabledMove, gDisableStructs[sd].disableTimer,
                                           gDisableStructs[sd].encoredMove, gDisableStructs[sd].encoredMovePos, gDisableStructs[sd].encoreTimer, gDisableStructs[sd].tauntTimer, CheckMoveLimitations(sd, 0, MOVE_LIMITATIONS_ALL),
                                           gBattleMons[sd].moves[0], gBattleMons[sd].moves[1], gBattleMons[sd].moves[2], gBattleMons[sd].moves[3]);
                                }
                                for (int sd = 0; sd < 2; sd++)
                                {
                                    int nn = sd ? n1 : n0; struct SimAction *ll = sd ? legal1 : legal0; int mm = sd ? m1 : m0; fs_action *ff = sd ? fl1 : fl0;
                                    printf("   verbatim P%d:", sd); for (int k = 0; k < nn; k++) printf(" %s%d", ll[k].type == B_ACTION_SWITCH ? "sw" : "mv", ll[k].type == B_ACTION_SWITCH ? ll[k].partySlot : ll[k].moveSlot);
                                    printf("   fast P%d:", sd); for (int k = 0; k < mm; k++) printf(" %s%d", ff[k].type == FS_ACT_SWITCH ? "sw" : "mv", ff[k].slot);
                                    printf("   party hp:"); for (int k = 0; k < 6; k++) printf(" %d", fs.side[sd].party[k].hp); printf("  active idx %d\n", fs.side[sd].act.monIdx);
                                }
                                goto decide;
                            }
                            x ^= x << 13; x ^= x >> 17; x ^= x << 5; { int i0 = x % n0; x ^= x << 13; x ^= x >> 17; x ^= x << 5; int i1 = x % n1; va0 = legal0[i0]; va1 = legal1[i1]; fa0 = fl0[i0]; fa1 = fl1[i1]; }
                            // map: verbatim moveSlot / partySlot vs fast slot (both enumerate moves in slot order then switches in party order)
                        }
                        else
                        {
                            int ns = Sim_LegalSwitches(sim, b, slots, 6), k; fs_action fl[32]; int m = fs_legal_actions(&fs, b & 1, fl);
                            if (ns == 0 || m != ns) { legalMismatch++; goto decide; }
                            x ^= x << 13; x ^= x >> 17; x ^= x << 5; k = x % ns;
                            memset(&va0, 0, sizeof(va0)); va0.type = B_ACTION_SWITCH; va0.partySlot = slots[k]; va1 = va0;
                            fa0 = fl[k]; fa1 = fl[k];
                        }
                        // distributions
                        sNB = 0;
                        resultImportFail = 0;
                        {
                            int focus = 0; for (int fi = 0; fi < nFocus; fi++) if (g == focusGame[fi] && sim->turnCount == focusTurn[fi]) focus = 1;
                            if (focus)
                            {
                                printf("FOCUS game %d turn %d kind %d battler %d: actions verbatim P0 %s%d P1 %s%d, fast P0 %d:%d P1 %d:%d, fast phase %d order", g, sim->turnCount, kind, b,
                                       va0.type == B_ACTION_SWITCH ? "sw" : "mv", va0.type == B_ACTION_SWITCH ? va0.partySlot : va0.moveSlot, va1.type == B_ACTION_SWITCH ? "sw" : "mv", va1.type == B_ACTION_SWITCH ? va1.partySlot : va1.moveSlot,
                                       fa0.type, fa0.slot, fa1.type, fa1.slot, fs.phase);
                                for (int k = 0; k < fs.orderN; k++) printf(" %d", fs.order[k]); printf(" pos %d pending %d:%d %d:%d\n", fs.orderPos, fs.pending[0].type, fs.pending[0].slot, fs.pending[1].type, fs.pending[1].slot);
                                for (int sd = 0; sd < 2; sd++) { printf("   P%d party:", sd); for (int mi = 0; mi < 6; mi++) printf(" %d:%s(%d/%d %s)", mi, fs.side[sd].party[mi].species ? sim_species_name(fs.side[sd].party[mi].species) : "-", fs.side[sd].party[mi].hp, fs.side[sd].party[mi].maxHP, sim_item_name(fs.side[sd].party[mi].item)); printf("  active %d present %d vol %x wrap %d\n", fs.side[sd].act.monIdx, fs.side[sd].act.present, fs.side[sd].act.vol, fs.side[sd].act.wrapTurns); }
                                Sim_Bind((struct BattleSim *)src);
                                printf("   verbatim: curAction %d order [%d:%d %d:%d] partyIdx %d/%d absent %x status2 %x/%x\n", gCurrentTurnActionNumber, gBattlerByTurnOrder[0], gActionsByTurnOrder[0], gBattlerByTurnOrder[1], gActionsByTurnOrder[1],
                                       gBattlerPartyIndexes[0], gBattlerPartyIndexes[1], gAbsentBattlerFlags, gBattleMons[0].status2, gBattleMons[1].status2);
                            }
                            for (int k = 0; k < nSamples; k++)
                            {
                                struct Summary sm; fs_state r2; x ^= x << 13; x ^= x >> 17; x ^= x << 5;
                                sFocusLog = focus && k < 3;
                                RunVerbatim(src, &va0, &va1, x, out);
                                fs_state imp; if (fs_import(&imp, out) != 0) resultImportFail |= imp.unsupported; Summarize(&imp, &sm); Add(0, &sm);
                                if (sFocusLog) { printf("   verbatim sample %d: req %d battler %d kind %d partyIdx %d/%d hp %d/%d\n", k, out->finished ? 0 : 1, out->requestBattler, out->requestKind, gBattlerPartyIndexes[0], gBattlerPartyIndexes[1], gBattleMons[0].hp, gBattleMons[1].hp); Sim_PrintLog(out); }
                                r2 = fs; fs_seed(&r2, x ^ 0xABCDEF); fs_step(&r2, fa0, fa1); Summarize(&r2, &sm); Add(1, &sm);
                                if (sFocusLog) { struct Summary vs; Summarize(&imp, &vs); printf("   fast sample %d: req %d mask %d phase %d; diff vs verbatim:", k, r2.request, r2.switchMask, r2.phase); PrintDiff(&vs, &sm); printf("\n"); }
                            }
                            sFocusLog = 0;
                        }
                        {
                            // two-sample chi-square over the outcome buckets (equal sample sizes): X2 = sum (n1-n2)^2/(n1+n2), df = k-1;
                            // flagged when X2 exceeds df + 4*sqrt(2 df) + 12 (roughly p < 1e-4), and always when a bucket is seen >= 25 times in one engine only
                            double tv = 0, x2 = 0; int df = sNB - 1, exclusive = 0;
                            for (int k = 0; k < sNB; k++)
                            {
                                double d = fabs((double)sB[k].n[0] - sB[k].n[1]) / nSamples; tv += 0.5 * d;
                                x2 += (double)(sB[k].n[0] - sB[k].n[1]) * (sB[k].n[0] - sB[k].n[1]) / (sB[k].n[0] + sB[k].n[1]);
                                if ((sB[k].n[0] == 0 && sB[k].n[1] >= 25) || (sB[k].n[1] == 0 && sB[k].n[0] >= 25)) exclusive = 1;
                            }
                            tested++;
                            if (x2 > df + 4 * sqrt(2.0 * (df > 0 ? df : 1)) + 12 || exclusive)
                            {
                                mismatched++;
                                printf("MISMATCH game %d turn %d (%s) TV %.3f X2 %.0f/df %d buckets %d: ", g, sim->turnCount, kind == SIM_REQ_ACTION ? "turn" : "switch", tv, x2, df, sNB);
                                if (kind == SIM_REQ_ACTION) printf("P0 %s %s vs P1 %s %s\n", va0.type == B_ACTION_SWITCH ? "switch" : "move", va0.type == B_ACTION_SWITCH ? sim_species_name(fs.side[0].party[va0.partySlot].species) : fs_move_name(fs.side[0].act.moves[va0.moveSlot]), va1.type == B_ACTION_SWITCH ? "switch" : "move", va1.type == B_ACTION_SWITCH ? sim_species_name(fs.side[1].party[va1.partySlot].species) : fs_move_name(fs.side[1].act.moves[va1.moveSlot]));
                                else printf("switch to slot %d\n", va0.partySlot);
                                if (resultImportFail) printf("   (some verbatim result states could not be imported: unsupported mask %x)\n", resultImportFail);
                                printf("   P0: "); PrintMon(&fs.side[0].act); printf("  moves %s/%s/%s/%s\n", fs_move_name(fs.side[0].act.moves[0]), fs_move_name(fs.side[0].act.moves[1]), fs_move_name(fs.side[0].act.moves[2]), fs_move_name(fs.side[0].act.moves[3]));
                                printf("   P1: "); PrintMon(&fs.side[1].act); printf("  moves %s/%s/%s/%s\n", fs_move_name(fs.side[1].act.moves[0]), fs_move_name(fs.side[1].act.moves[1]), fs_move_name(fs.side[1].act.moves[2]), fs_move_name(fs.side[1].act.moves[3]));
                                // the most common verbatim outcome vs the most common fast outcome, and the field differences
                                {
                                    int bv = -1, bf = -1;
                                    for (int k = 0; k < sNB; k++) { if (bv < 0 || sB[k].n[0] > sB[bv].n[0]) bv = k; if (bf < 0 || sB[k].n[1] > sB[bf].n[1]) bf = k; }
                                    printf("   most common: verbatim %d/%d (bucket also seen %d times in fast); fast %d/%d (seen %d in verbatim)\n", sB[bv].n[0], nSamples, sB[bv].n[1], sB[bf].n[1], nSamples, sB[bf].n[0]);
                                    if (bv != bf) { printf("   differing fields (verbatim/fast):"); PrintDiff(&sB[bv].ex, &sB[bf].ex); printf("\n"); }
                                }
                                // HP histograms of both actives, per engine (sorted by hp)
                                for (int sd = 0; sd < 2; sd++)
                                {
                                    int hist[2][600], anyDiff = 0, k2;
                                    memset(hist, 0, sizeof(hist));
                                    for (int k = 0; k < sNB; k++) { int hp = sB[k].ex.sd[sd].present ? sB[k].ex.sd[sd].hp : 0; if (hp > 599) hp = 599; hist[0][hp] += sB[k].n[0]; hist[1][hp] += sB[k].n[1]; }
                                    for (k2 = 0; k2 < 600; k2++) if (hist[0][k2] != hist[1][k2]) anyDiff = 1;
                                    if (!anyDiff) continue;
                                    printf("   P%d hp (verbatim|fast):", sd);
                                    for (k2 = 599; k2 >= 0; k2--) if (hist[0][k2] || hist[1][k2]) printf(" %d:%d|%d", k2, hist[0][k2], hist[1][k2]);
                                    printf("\n");
                                }
                                for (int k = 0; k < sNB && k < 0; k++)
                                {
                                    const struct Summary *e = &sB[k].ex;
                                    printf("   %5d vs %5d  hp %d/%d mon %d/%d st %x/%x req %d mask %d out %d pty %d/%d\n", sB[k].n[0], sB[k].n[1], e->sd[0].hp, e->sd[1].hp, e->sd[0].mon, e->sd[1].mon, e->sd[0].st, e->sd[1].st, e->req, e->mask, e->outcome,
                                           e->sd[0].php[0] + e->sd[0].php[1] + e->sd[0].php[2] + e->sd[0].php[3] + e->sd[0].php[4] + e->sd[0].php[5], e->sd[1].php[0] + e->sd[1].php[1] + e->sd[1].php[2] + e->sd[1].php[3] + e->sd[1].php[4] + e->sd[1].php[5]);
                                }
                            }
                            else if (verbose) printf("ok game %d turn %d TV %.3f buckets %d\n", g, sim->turnCount, tv, sNB);
                        }
                    }
                }
            decide:
                ag.decide(&ag, sim, (kind == SIM_REQ_ACTION && ts->turnCount == sim->turnCount) ? ts : NULL, b, kind, &act);
                if (Sim_Answer(sim, b, &act) != 0) { struct SimAction acts[32]; u8 sl[6]; int n; memset(&act, 0, sizeof(act)); if (kind == SIM_REQ_SWITCH) { n = Sim_LegalSwitches(sim, b, sl, 6); act.type = B_ACTION_SWITCH; act.partySlot = n ? sl[0] : 0; } else { n = Sim_LegalActions(sim, b, acts, 32); if (n) act = acts[0]; } if (Sim_Answer(sim, b, &act) != 0) break; }
            }
        }
        printf("fastdiff: %d positions tested, %d mismatched (%.1f%%), %d legal-action mismatches, %d unsupported positions skipped, %d import failures\n", tested, mismatched, 100.0 * mismatched / (tested ? tested : 1), legalMismatch, unsup, importFail);
        printf("unsupported reasons: move effect %ld, ability %ld, item %ld, volatile %ld, doubles %ld, state %ld\n", unsupReasons[0], unsupReasons[1], unsupReasons[2], unsupReasons[3], unsupReasons[4], unsupReasons[5]);
        printf("unsupported move effects (effect id: positions):"); for (int k = 0; k < 256; k++) if (unsupEffect[k]) printf(" %d:%ld", k, unsupEffect[k]); printf("\n");
        printf("unsupported abilities (id: positions):"); for (int k = 0; k < 128; k++) if (unsupAbility[k]) printf(" %d:%ld", k, unsupAbility[k]); printf("\n");
    }
    return 0;
}
