#include <string.h>
#include "scenario.h"
#include "util.h"

int gScenarioFailures, gScenarioChecks;
const char *gScenarioCurrent = "";

int Sc_LastTurn(struct BattleSim *sim)
{
    return sim->logCount ? sim->log[sim->logCount - 1].turn : 0;
}
static int TurnMatches(struct BattleSim *sim, int entryTurn, int turn)
{
    if (turn == SC_ANY_TURN) return 1;
    if (turn == SC_LAST_TURN) return entryTurn == Sc_LastTurn(sim);
    return entryTurn == turn;
}
int Sc_LogHas(struct BattleSim *sim, u16 stringId, int turn) { return Sc_LogCount(sim, stringId, turn) > 0; }
int Sc_LogCount(struct BattleSim *sim, u16 stringId, int turn)
{
    int i, n = 0;
    for (i = 0; i < sim->logCount; i++)
        if (sim->log[i].stringId == stringId && TurnMatches(sim, sim->log[i].turn, turn))
            n++;
    return n;
}
int Sc_LogHasMove(struct BattleSim *sim, u16 stringId, u16 move, int turn)
{
    int i;
    for (i = 0; i < sim->logCount; i++)
        if (sim->log[i].stringId == stringId && sim->log[i].move == move && TurnMatches(sim, sim->log[i].turn, turn))
            return 1;
    return 0;
}
int Sc_LogIndex(struct BattleSim *sim, u16 stringId, u8 battler, int nth, int turn)
{
    int i;
    for (i = 0; i < sim->logCount; i++)
        if (sim->log[i].stringId == stringId && sim->log[i].battler == battler && TurnMatches(sim, sim->log[i].turn, turn) && nth-- == 0)
            return i;
    return -1;
}

static void MakeMon(struct Pokemon *mon, const struct ScenarioMon *m)
{
    static const u8 max[6] = { 31, 31, 31, 31, 31, 31 };
    int i;
    Sim_MakeMonEx(mon, m->species, m->level ? m->level : 50, m->nature, m->ivsSet ? m->ivs : max, m->evs, m->moves, m->item, m->abilityNum, m->otId, m->fateful);
    if (m->hpSet)
    {
        u16 hp = m->hp;
        SetMonData(mon, MON_DATA_HP, &hp);
    }
    if (m->status)
    {
        u32 st = m->status;
        SetMonData(mon, MON_DATA_STATUS, &st);
    }
    if (m->ppSet)
        for (i = 0; i < 4; i++)
        {
            u8 pp = m->pp[i];
            SetMonData(mon, MON_DATA_PP1 + i, &pp);
        }
}

void Scenario_Setup(struct BattleSim *sim, const struct Scenario *sc, u16 seed)
{
    int i;
    Sim_Init(sim, BATTLE_TYPE_TRAINER | sc->flags, 1);
    sim->logEnabled = TRUE;
    sim->maxTurns = SC_MAX_TURNS + 2;
    for (i = 0; i < 6 && sc->player[i].species; i++)
        MakeMon(&sim->playerParty[i], &sc->player[i]);
    for (i = 0; i < 6 && sc->enemy[i].species; i++)
        MakeMon(&sim->enemyParty[i], &sc->enemy[i]);
    sim->trainerBattleOpponent_A = SC_TRAINER_ID;
    if (sc->badgesSet)
        sim->badgeFlags = sc->badges;
    sim->createTrainerParty = FALSE;
    sim->rngXorshift = 1;
    sim->rngValue = seed ? seed : 0x1234;
    sim->rngCalls = 0;
}

void Scenario_Decide(struct ScenarioRun *run, struct BattleSim *sim, u8 battler, u8 kind, struct SimAction *out)
{
    const struct Scenario *sc = run->sc;
    int turn = sim->turnCount < SC_MAX_TURNS ? sim->turnCount : SC_MAX_TURNS - 1;
    u8 a = sc->actions[turn][battler];
    u8 slots[6];
    int n;

    memset(out, 0, sizeof(*out));
    if (kind == SIM_REQ_SWITCH)
    {
        n = Sim_LegalSwitches(sim, battler, slots, 6);
        out->type = B_ACTION_SWITCH;
        if (a >= 10 && a < 16)
            out->partySlot = a - 10;
        else
            out->partySlot = n ? slots[0] : 0;
        return;
    }
    if (run->asked[turn][battler]++ > 0)
    {
        // The game rejected the scripted choice (disabled move, trapped, ...): take the first legal action.
        struct SimAction acts[32];
        n = Sim_LegalActions(sim, battler, acts, 32);
        if (n)
        {
            *out = acts[0];
            return;
        }
    }
    if (a >= 10 && a < 16)
    {
        out->type = B_ACTION_SWITCH;
        out->partySlot = a - 10;
        return;
    }
    out->type = B_ACTION_USE_MOVE;
    out->moveSlot = (a == SC_DEFAULT) ? 0 : (a & 3);
    out->target = sc->targets[turn][battler] ? sc->targets[turn][battler] - 1 : 0xFF;
}

int Scenario_ShouldStop(struct BattleSim *sim, const struct Scenario *sc)
{
    int limit = sc->turns ? sc->turns : SC_MAX_TURNS;
    return sim->turnCount >= limit;
}

int Scenario_RunSim(struct BattleSim *sim, const struct Scenario *sc, u16 seed)
{
    struct ScenarioRun run = { .sc = sc };
    int res;

    Scenario_Setup(sim, sc, seed);
    if (Sim_Start(sim) != 0)
        return -1;
    for (;;)
    {
        struct SimAction act;
        res = Sim_Run(sim);
        if (res != SIM_RUN_REQUEST)
            return res;
        if (Scenario_ShouldStop(sim, sc))
            return res;
        Scenario_Decide(&run, sim, sim->requestBattler, sim->requestKind, &act);
        Sim_Answer(sim, sim->requestBattler, &act);
        if (sim->requestKind == SIM_REQ_NONE && sim->turnCount > 0)
            ; // keep the log across a turn: it is cleared by the runner at turn boundaries below
    }
}

u16 Scenario_ResolveSeed(struct BattleSim *sim, const struct Scenario *sc)
{
    u16 seed = sc->seed ? sc->seed : 0x1234;
    int tries;
    if (!sc->wantSeed)
        return seed;
    for (tries = 0; tries < 2000; tries++)
    {
        Scenario_RunSim(sim, sc, seed);
        if (sc->wantSeed(sim))
            return seed;
        seed = seed * 3 + 1;
        if (seed == 0) seed = 1;
    }
    printf("  WARNING [%s]: no seed satisfied wantSeed; using %#x\n", sc->name, seed);
    return seed;
}
