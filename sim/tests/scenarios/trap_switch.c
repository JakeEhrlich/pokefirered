// Trapping and switching: Wrap-family moves, Mean Look, Ingrain, trapping abilities, Roar/Whirlwind,
// Baton Pass, Pursuit, Perish Song, Teleport, what a switch resets/keeps, switch-in effect order,
// Future Sight / Wish / Leech Seed on replacements, faint replacement timing.
//
// Engine facts these scenarios rely on (sim/src/battle_*.c, data/battle_scripts_1.s):
// - MOVE_EFFECT_WRAP sets STATUS2_WRAPPED_TURN((Random() & 3) + 3), i.e. a counter of 3..6. At the end of
//   each turn the counter is decremented first; if still non-zero the target takes maxHP/16 (PKMNHURTBY),
//   otherwise it is freed (PKMNFREEDFROM). So there are 2..5 damage ticks and the mon is free on the turn
//   after the last tick. SwitchInClearSetData/FaintClearSetData clear STATUS2_WRAPPED on every mon wrapped
//   by the leaving battler. Secondary effects never apply through a Substitute (SetMoveEffect), and
//   Cmd_setsubstitute clears STATUS2_WRAPPED on the user: making a Substitute frees a wrapped mon silently.
// - Mean Look/Block/Spider Web: STATUS2_ESCAPE_PREVENTION + battlerPreventingEscape; cleared when that
//   battler switches (except via Baton Pass!) or faints. Fails vs Substitute / already trapped. Wrap, by
//   contrast, is cleared on the target even when the wrapper leaves by Baton Pass (that loop in
//   SwitchInClearSetData is unconditional), as is infatuation with the leaving battler.
// - Ingrain: STATUS3_ROOTED, maxHP/16 heal at ENDTURN_INGRAIN (before Leech Seed and status damage),
//   blocks the switch action and Roar; carried by Baton Pass.
// - Trapping abilities only matter at action selection (HandleTurnActionSelectionState): Shadow Tag,
//   Arena Trap (not Flying / Levitate), Magnet Pull (Steel only). They never block Baton Pass or the
//   replacement of a fainted mon, and never bind the holder itself.
// - Roar/Whirlwind (priority -6): Suction Cups / Ingrain / Soundproof block it; in a trainer battle it
//   fails with < 2 healthy mons (< 3 in doubles); the target's level check (TryDoForceSwitchOut) applies to
//   trainer battles too; the dragged-in mon is a random other healthy party member and it goes through
//   the normal switch-in effects (Spikes etc.); Natural Cure triggers for the roared-out mon.
// - Baton Pass keeps stat stages, STATUS2 confusion/focus energy/substitute/escape prevention/curse,
//   STATUS3 leech seed/lock-on/perish song/ingrain/sports, substituteHP, perishSongTimer,
//   battlerPreventingEscape; jumpifcantswitch ignores every escape prevention for it.
// - Pursuit on a switch (BattleScript_ActionSwitch): only when the pursuer chose Pursuit targeting the
//   switcher and is not asleep/frozen; sDMG_MULTIPLIER = 2, hits before the switch, then the pursuer's
//   action is finished. Baton Pass and faint replacements are not "switch" actions.
// - A KOed mon is replaced right after the action that KOed it (HandleAction_TryFinish ->
//   HandleFaintedMonActions), before the remaining actions and the end-of-turn effects; end-of-turn KOs
//   (poison, Perish Song, Future Sight) are replaced within the same BattleTurnPassed (it re-enters and
//   reruns HandleFaintedMonActions after every script).
// - The log entry for a replacement's SWITCHINMON carries gBattlerAttacker (not the replaced battler);
//   its hpTarget is the replacement's hp (gBattlerTarget = the fainted battler), which identifies the side.
#include "scenario.h"

#define OPP B_SIDE_OPPONENT
#define PLR B_SIDE_PLAYER
#define S4(m1, m2, m3, m4) m1, m2, m3, m4
#define SPL MOVE_SPLASH

// index of the first log entry with `id` on `turn` (any battler), -1 if none
static int FirstLog(struct BattleSim *sim, u16 id, int turn)
{
    int i;
    for (i = 0; i < sim->logCount; i++)
        if (sim->log[i].stringId == id && (turn == SC_ANY_TURN || sim->log[i].turn == turn))
            return i;
    return -1;
}

// ---------------------------------------------------------------- Wrap family

static int WantWrapMin(struct BattleSim *sim)
{
    // counter 3: ticks at the end of turns 0 and 1, freed at the end of turn 2
    return Sc_LogHas(sim, STRINGID_PKMNWRAPPEDBY, 0) && Sc_LogCount(sim, STRINGID_PKMNHURTBY, SC_ANY_TURN) == 2
        && Sc_LogHas(sim, STRINGID_PKMNFREEDFROM, 2);
}
static void CheckWrapMin(struct BattleSim *sim)
{
    int w = FirstLog(sim, STRINGID_PKMNWRAPPEDBY, 0);
    int i, ticks = 0;
    u16 max = (u16)GetMonData(&sim->enemyParty[0], MON_DATA_MAX_HP);
    CHECK(w >= 0, "wrap message logged");
    for (i = 0; i < sim->logCount; i++)
        if (sim->log[i].stringId == STRINGID_PKMNHURTBY)
        {
            ticks++;
            CHECK(sim->log[i].dmg == max / 16, "wrap tick is maxHP/16 (dmg %d, max %d)", sim->log[i].dmg, max);
        }
    CHECK(ticks == 2, "two damage ticks");
    // turns 1 and 2: the switch was refused, Rattata fell back to Splash and stayed in
    CHECK(LOG_INDEX_T(STRINGID_SWITCHINMON, 1, 1) < 0 && LOG_INDEX_T(STRINGID_SWITCHINMON, 1, 2) < 0, "no switch while wrapped");
    CHECK(LOG_INDEX_T(STRINGID_USEDMOVE, 1, 1) >= 0 && LOG_INDEX_T(STRINGID_USEDMOVE, 1, 2) >= 0, "fell back to a move while wrapped");
    // turn 3: free, the switch goes through
    CHECK(B(1).species == SPECIES_PIDGEY, "switched once freed (got %s)", gSimSpeciesNames[B(1).species]);
    CHECK(LOG_INDEX_T(STRINGID_SWITCHINMON, 1, 3) >= 0, "switch-in on turn 3");
    if (w >= 0)
        CHECK(PARTY_HP(OPP, 0) == sim->log[w].hpTarget - 2 * (max / 16), "hp after wrap hit minus two ticks (party hp %d, after hit %d)", PARTY_HP(OPP, 0), sim->log[w].hpTarget);
}

static int WantWrapMax(struct BattleSim *sim)
{
    // counter 6: five ticks (turns 0-4), freed at the end of turn 5
    return Sc_LogHas(sim, STRINGID_PKMNWRAPPEDBY, 0) && Sc_LogCount(sim, STRINGID_PKMNHURTBY, SC_ANY_TURN) == 5
        && Sc_LogHas(sim, STRINGID_PKMNFREEDFROM, 5);
}
static void CheckWrapMax(struct BattleSim *sim)
{
    int t;
    for (t = 1; t <= 5; t++)
        CHECK(LOG_INDEX_T(STRINGID_SWITCHINMON, 1, t) < 0, "no switch on turn %d", t);
    CHECK(B(1).species == SPECIES_PIDGEY && LOG_INDEX_T(STRINGID_SWITCHINMON, 1, 6) >= 0, "switched on turn 6 after 5 ticks");
    CHECK(PARTY_HP(OPP, 0) > 0, "rattata survived the trap");
}

static int WantWrapTwice(struct BattleSim *sim)
{
    return !Sc_LogHas(sim, STRINGID_ATTACKMISSED, SC_ANY_TURN) && Sc_LogHas(sim, STRINGID_PKMNFREEDFROM, 2);
}
static void CheckWrapTwice(struct BattleSim *sim)
{
    // The second Wrap hit does not restart or extend the trap (MOVE_EFFECT_WRAP skipped when already wrapped).
    CHECK(LOG_COUNT(STRINGID_PKMNWRAPPEDBY) == 1, "wrap applied only once (%d)", LOG_COUNT(STRINGID_PKMNWRAPPEDBY));
    CHECK(LOG_COUNT(STRINGID_PKMNHURTBY) == 2, "still only two ticks");
    CHECK(!(STATUS2(1) & STATUS2_WRAPPED), "freed at the end of turn 2");
}

static int WantWrapHitT0(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_PKMNWRAPPEDBY, 0); }

static void CheckWrapperSwitchFrees(struct BattleSim *sim)
{
    CHECK(LOG_COUNT(STRINGID_PKMNHURTBY) == 1, "only the turn-0 tick (got %d)", LOG_COUNT(STRINGID_PKMNHURTBY));
    CHECK(!LOG_HAS(STRINGID_PKMNFREEDFROM), "no 'freed' message: the flag is cleared silently");
    CHECK(B(0).species == SPECIES_SANDSHREW, "arbok left");
    CHECK(B(1).species == SPECIES_PIDGEY, "rattata could switch on turn 2 once arbok left");
}

static void CheckWrapperFaintFrees(struct BattleSim *sim)
{
    CHECK(!(STATUS2(0) & STATUS2_WRAPPED), "wrap cleared when the wrapper fainted");
    CHECK(LOG_COUNT(STRINGID_PKMNHURTBY) == 0, "no end-of-turn tick");
    CHECK(B(1).species == SPECIES_PIDGEY, "arbok replaced at the end of the turn");
}

static void CheckWrapperBatonPassFrees(struct BattleSim *sim)
{
    // Unlike Mean Look, the wrap flag on the target is cleared even when the wrapper leaves by Baton Pass.
    CHECK(B(0).species == SPECIES_SANDSHREW, "arbok passed to sandshrew");
    CHECK(LOG_COUNT(STRINGID_PKMNHURTBY) == 1 && Sc_LogCount(sim, STRINGID_PKMNHURTBY, 1) == 0, "only the turn-0 tick (got %d)", LOG_COUNT(STRINGID_PKMNHURTBY));
    CHECK(!LOG_HAS(STRINGID_PKMNFREEDFROM), "cleared silently");
    CHECK(LOG_INDEX_T(STRINGID_SWITCHINMON, 1, 1) < 0 && LOG_INDEX_T(STRINGID_USEDMOVE, 1, 1) >= 0, "still trapped at turn-1 selection (before the pass)");
    CHECK(B(1).species == SPECIES_PIDGEY && LOG_INDEX_T(STRINGID_SWITCHINMON, 1, 2) >= 0, "free to switch on turn 2");
}

static void CheckSubstituteFreesWrap(struct BattleSim *sim)
{
    // Cmd_setsubstitute clears STATUS2_WRAPPED on the user: no tick that turn, no 'freed' message, and the
    // switch is allowed next turn.
    CHECK(LOG_HAS_T(STRINGID_PKMNWRAPPEDBY, 0) && LOG_HAS_T(STRINGID_PKMNMADESUBSTITUTE, 0), "wrapped, then made a substitute");
    CHECK(MOVED_BEFORE(0, 1, 0), "arbok wrapped before snorlax moved");
    CHECK(LOG_COUNT(STRINGID_PKMNHURTBY) == 0, "no wrap tick after the substitute (got %d)", LOG_COUNT(STRINGID_PKMNHURTBY));
    CHECK(!LOG_HAS(STRINGID_PKMNFREEDFROM), "no 'freed' message");
    CHECK(B(1).species == SPECIES_PIDGEY && LOG_INDEX_T(STRINGID_SWITCHINMON, 1, 1) >= 0, "could switch on turn 1");
}

static void CheckWrapGhost(struct BattleSim *sim)
{
    CHECK(HP(1) == MAXHP(1), "normal-type wrap does not hurt gastly");
    CHECK(!(STATUS2(1) & STATUS2_WRAPPED), "and does not trap it");
    CHECK(LOG_HAS(STRINGID_ITDOESNTAFFECT), "doesn't affect message");
}

static int WantVortex(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_PKMNTRAPPEDINVORTEX, 0); }
static void CheckFireSpinGhost(struct BattleSim *sim)
{
    CHECK(HP(1) < MAXHP(1), "fire spin hurts gastly");
    CHECK(B(1).species == SPECIES_GASTLY, "gastly could not switch on turn 1");
    CHECK(LOG_INDEX_T(STRINGID_USEDMOVE, 1, 1) >= 0, "gastly used a move instead");
}

static int WantNoMiss(struct BattleSim *sim) { return !Sc_LogHas(sim, STRINGID_ATTACKMISSED, SC_ANY_TURN); }
static void CheckWrapSubstitute(struct BattleSim *sim)
{
    CHECK(STATUS2(1) & STATUS2_SUBSTITUTE, "substitute still up");
    CHECK(!(STATUS2(1) & STATUS2_WRAPPED), "wrap effect does not go through a substitute");
    CHECK(!LOG_HAS(STRINGID_PKMNWRAPPEDBY) && LOG_COUNT(STRINGID_PKMNHURTBY) == 0, "no wrap message or tick");
}

static void CheckBindMsg(struct BattleSim *sim) { CHECK(LOG_HAS(STRINGID_PKMNSQUEEZEDBYBIND) && (STATUS2(1) & STATUS2_WRAPPED), "bind message and trap"); }
static void CheckClampMsg(struct BattleSim *sim) { CHECK(LOG_HAS(STRINGID_PKMNCLAMPED) && (STATUS2(1) & STATUS2_WRAPPED), "clamp message and trap"); }
static void CheckSandTombMsg(struct BattleSim *sim) { CHECK(LOG_HAS(STRINGID_PKMNTRAPPEDBYSANDTOMB) && (STATUS2(1) & STATUS2_WRAPPED), "sand tomb message and trap"); }
static int WantBind(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_PKMNSQUEEZEDBYBIND, SC_ANY_TURN); }
static int WantClamp(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_PKMNCLAMPED, SC_ANY_TURN); }
static int WantSandTomb(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_PKMNTRAPPEDBYSANDTOMB, SC_ANY_TURN); }

static void CheckRapidSpinWrap(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNGOTFREE, 1), "got free message on turn 1");
    CHECK(LOG_COUNT(STRINGID_PKMNHURTBY) == 1, "no tick after spinning free");
    CHECK(B(0).species == SPECIES_RATTATA, "could switch on turn 2");
}

static int WantWrapAndSeed(struct BattleSim *sim)
{
    return Sc_LogHas(sim, STRINGID_PKMNWRAPPEDBY, 0) && Sc_LogHas(sim, STRINGID_PKMNSEEDED, 1);
}
static void CheckRapidSpinWrapAndSeed(struct BattleSim *sim)
{
    // rapidspinfree looks like an if/else chain, but each freeing script returns to the pushed cursor
    // (rapidspinfree itself), so a single Rapid Spin clears wrap, then leech seed, then spikes.
    CHECK(LOG_HAS_T(STRINGID_PKMNGOTFREE, 2) && LOG_HAS_T(STRINGID_PKMNSHEDLEECHSEED, 2), "one spin freed both wrap and leech seed");
    CHECK(!(STATUS2(0) & STATUS2_WRAPPED) && !(STATUS3(0) & STATUS3_LEECHSEED), "free of both");
    CHECK(Sc_LogCount(sim, STRINGID_PKMNHURTBY, 2) == 0 && Sc_LogCount(sim, STRINGID_PKMNSAPPEDBYLEECHSEED, 2) == 0, "no tick or drain at the end of turn 2");
}

static void CheckRapidSpinSpikes(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNBLEWAWAYSPIKES, 1), "blew away spikes");
    CHECK(sim->sideTimers[PLR].spikesAmount == 0 && !(SIDE_STATUS(PLR) & SIDE_STATUS_SPIKES), "spikes gone");
    CHECK(B(0).species == SPECIES_RATTATA && HP(0) == MAXHP(0), "replacement took no spikes damage");
}

// ---------------------------------------------------------------- Mean Look family

static void CheckMeanLook(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_TARGETCANTESCAPENOW, 0), "can't escape message");
    CHECK(STATUS2(1) & STATUS2_ESCAPE_PREVENTION, "escape prevention set");
    CHECK(sim->disableStructs[1].battlerPreventingEscape == 0, "prevented by battler 0");
    CHECK(B(1).species == SPECIES_RATTATA && LOG_INDEX_T(STRINGID_USEDMOVE, 1, 1) >= 0, "switch refused, used a move");
}

static void CheckMeanLookSubstitute(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 1), "mean look failed vs substitute");
    CHECK(!(STATUS2(1) & STATUS2_ESCAPE_PREVENTION), "not trapped");
}

static void CheckMeanLookTwice(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_TARGETCANTESCAPENOW, 0) && LOG_HAS_T(STRINGID_BUTITFAILED, 1), "second mean look fails");
}

static void CheckMeanLookUserSwitch(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_RATTATA, "gengar left on turn 1");
    CHECK(B(1).species == SPECIES_PIDGEY, "target could switch on turn 2");
}

static void CheckMeanLookUserFaint(struct BattleSim *sim)
{
    CHECK(PARTY_HP(OPP, 0) == 0, "umbreon fainted");
    CHECK(B(0).species == SPECIES_RATTATA, "machamp could switch on turn 1");
    CHECK(B(1).species == SPECIES_PIDGEY, "umbreon replaced");
}

static void CheckMeanLookUserBatonPass(struct BattleSim *sim)
{
    // SwitchInClearSetData only clears escape prevention caused by the leaving battler when the switch is
    // NOT a Baton Pass: the trap survives the user passing.
    CHECK(B(0).species == SPECIES_RATTATA, "umbreon passed");
    CHECK(STATUS2(1) & STATUS2_ESCAPE_PREVENTION, "target still trapped after the user baton passed");
    CHECK(B(1).species == SPECIES_RATTATA && LOG_INDEX_T(STRINGID_USEDMOVE, 1, 2) >= 0, "switch refused on turn 2");
}

static void CheckMeanLookTargetBatonPass(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_RATTATA, "scyther passed to rattata");
    CHECK(STATUS2(0) & STATUS2_ESCAPE_PREVENTION, "escape prevention carried by baton pass");
    CHECK(sim->disableStructs[0].battlerPreventingEscape == 1, "battlerPreventingEscape carried");
    CHECK(LOG_INDEX_T(STRINGID_SWITCHINMON, 0, 2) < 0 && LOG_INDEX_T(STRINGID_USEDMOVE, 0, 2) >= 0, "replacement cannot switch");
}

static void CheckRoarIgnoresMeanLook(struct BattleSim *sim)
{
    CHECK(B(1).species == SPECIES_PIDGEY, "roar dragged the mean look user out");
    CHECK(!(STATUS2(0) & STATUS2_ESCAPE_PREVENTION), "escape prevention gone with its user");
    CHECK(B(0).species == SPECIES_RATTATA, "could switch on turn 2");
}

static void CheckSpiderWeb(struct BattleSim *sim)
{
    CHECK(STATUS2(1) & STATUS2_ESCAPE_PREVENTION, "spider web traps");
    CHECK(B(1).species == SPECIES_RATTATA, "no switch");
}

// ---------------------------------------------------------------- Ingrain

static void CheckIngrain(struct BattleSim *sim)
{
    CHECK(STATUS3(0) & STATUS3_ROOTED, "rooted");
    CHECK(LOG_COUNT(STRINGID_PKMNABSORBEDNUTRIENTS) == 3, "healed at the end of every turn (%d)", LOG_COUNT(STRINGID_PKMNABSORBEDNUTRIENTS));
    CHECK(HP(0) == 40 + 3 * (MAXHP(0) / 16), "1/16 per turn (hp %d/%d)", HP(0), MAXHP(0));
    CHECK(B(0).species == SPECIES_BULBASAUR, "switch refused while rooted");
    CHECK(LOG_INDEX_T(STRINGID_USEDMOVE, 0, 1) >= 0 && LOG_INDEX_T(STRINGID_USEDMOVE, 0, 2) >= 0, "fell back to moves");
}

static void CheckIngrainTwice(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNPLANTEDROOTS, 0) && LOG_HAS_T(STRINGID_BUTITFAILED, 1), "second ingrain fails");
}

static void CheckRoarVsIngrain(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNANCHOREDITSELF), "anchored message");
    CHECK(B(1).species == SPECIES_BULBASAUR, "not dragged out");
}

static void CheckIngrainBeforePoison(struct BattleSim *sim)
{
    // hp starts at maxHP/8: healed by 1/16 first, then loses 1/8 -> maxHP/16 left (poison first would KO).
    CHECK(MAXHP(0) / 8 == 15, "setup: maxHP/8 is 15 (max %d)", MAXHP(0));
    CHECK(HP(0) == MAXHP(0) / 16, "ingrain heal happens before poison damage (hp %d)", HP(0));
}

static void CheckBatonPassIngrain(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_RATTATA, "passed");
    CHECK(STATUS3(0) & STATUS3_ROOTED, "rooted status carried");
    CHECK(HP(0) == 20 + 2 * (MAXHP(0) / 16), "replacement heals 1/16 on turns 1 and 2 (hp %d/%d)", HP(0), MAXHP(0));
    CHECK(LOG_INDEX_T(STRINGID_SWITCHINMON, 0, 2) < 0 && LOG_INDEX_T(STRINGID_USEDMOVE, 0, 2) >= 0, "replacement cannot switch");
}

// ---------------------------------------------------------------- trapping abilities

static void CheckShadowTag(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_RATTATA && LOG_INDEX_T(STRINGID_SWITCHINMON, 0, 0) >= 0, "the shadow tag holder itself switched freely");
    CHECK(LOG_INDEX_T(STRINGID_SWITCHINMON, 1, 0) < 0 && LOG_INDEX_T(STRINGID_USEDMOVE, 1, 0) >= 0, "foe could not switch while wobbuffet was in");
    CHECK(B(1).species == SPECIES_PIDGEY && LOG_INDEX_T(STRINGID_SWITCHINMON, 1, 1) >= 0, "foe switched once wobbuffet was gone");
}

static void CheckShadowTagMutual(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_WOBBUFFET && B(1).species == SPECIES_WOBBUFFET, "neither could switch");
    CHECK(!LOG_HAS(STRINGID_SWITCHINMON), "no switch at all");
}

static void CheckSwitchedToRattata(struct BattleSim *sim) { CHECK(B(0).species == SPECIES_RATTATA, "switch allowed (got %s)", gSimSpeciesNames[B(0).species]); }
static void CheckStuckRattata(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_RATTATA && !LOG_HAS(STRINGID_SWITCHINMON), "switch refused");
    CHECK(LOG_INDEX_T(STRINGID_USEDMOVE, 0, 0) >= 0, "used a move instead");
}
static void CheckStuckSkarmory(struct BattleSim *sim) { CHECK(B(0).species == SPECIES_SKARMORY && !LOG_HAS(STRINGID_SWITCHINMON), "steel type trapped by magnet pull"); }
static void CheckSwitchedToPidgey(struct BattleSim *sim) { CHECK(B(0).species == SPECIES_PIDGEY, "non-steel free (got %s)", gSimSpeciesNames[B(0).species]); }

static void CheckBatonPassUnderShadowTag(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_RATTATA && LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_BATON_PASS, 0), "baton pass works under shadow tag");
}

static void CheckFaintReplacementUnderShadowTag(struct BattleSim *sim)
{
    CHECK(PARTY_HP(PLR, 0) == 0, "rattata fainted");
    CHECK(B(0).species == SPECIES_PIDGEY, "replacement sent out despite shadow tag");
}

// ---------------------------------------------------------------- Roar / Whirlwind

static int WantRoarSandshrew(struct BattleSim *sim) { return B(1).species == SPECIES_SANDSHREW; }
static int WantRoarPidgey(struct BattleSim *sim) { return B(1).species == SPECIES_PIDGEY; }
static void CheckRoarDrag(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNWASDRAGGEDOUT, 0), "dragged out message");
    CHECK(MOVED_BEFORE(1, 0, 0), "roar (-6) goes after the foe's move");
    CHECK(STAGE(1, STAT_ATK) == DEFAULT_STAT_STAGE, "dragged-in mon has neutral stages (rattata's swords dance gone)");
    CHECK(PARTY_HP(OPP, 0) == (int)GetMonData(&sim->enemyParty[0], MON_DATA_MAX_HP), "roared-out rattata untouched");
    CHECK(STATUS2(1) == 0 && STATUS3(1) == 0, "clean volatiles on the dragged-in mon");
}

static void CheckWhirlwindLast(struct BattleSim *sim)
{
    CHECK(MOVED_BEFORE(1, 0, 0), "slow rattata still moves before whirlwind");
    CHECK(B(1).species == SPECIES_PIDGEY, "whirlwind dragged pidgey in");
}

static void CheckRoarFailsLastMon(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "roar fails against a lone mon");
    CHECK(B(1).species == SPECIES_RATTATA, "still in");
}

static void CheckRoarSuctionCups(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNANCHORSITSELFWITH), "suction cups message");
    CHECK(B(1).species == SPECIES_OCTILLERY, "not dragged out");
}

static void CheckRoarSoundproof(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNSXBLOCKSY), "soundproof blocks roar");
    CHECK(B(1).species == SPECIES_ELECTRODE, "not dragged out");
}

static int WantRoarLevelFail(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_BUTITFAILED, SC_ANY_TURN); }
static void CheckRoarLevelFail(struct BattleSim *sim)
{
    CHECK(B(1).species == SPECIES_RATTATA, "lower-level roar failed the level check");
}
static void CheckRoarLevelPass(struct BattleSim *sim)
{
    CHECK(!LOG_HAS(STRINGID_BUTITFAILED) && LOG_HAS(STRINGID_PKMNWASDRAGGEDOUT), "level check passed");
}

static void CheckRoarThroughSubstitute(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNWASDRAGGEDOUT, 1), "roar is not blocked by a substitute");
    CHECK(B(1).species == SPECIES_PIDGEY, "pidgey dragged in");
}

static void CheckRoarNaturalCure(struct BattleSim *sim)
{
    CHECK(B(1).species == SPECIES_PIDGEY, "chansey roared out");
    CHECK(PARTY_STATUS(OPP, 0) == 0, "natural cure healed the roared-out chansey (status %#x)", (unsigned)PARTY_STATUS(OPP, 0));
}

static void CheckRoarIntoSpikes(struct BattleSim *sim)
{
    CHECK(B(1).species == SPECIES_SANDSHREW, "sandshrew dragged in");
    CHECK(LOG_HAS_T(STRINGID_PKMNHURTBYSPIKES, 1), "dragged-in mon takes spikes");
    CHECK(HP(1) == MAXHP(1) - MAXHP(1) / 8, "one layer: 1/8 (hp %d/%d)", HP(1), MAXHP(1));
}

static void CheckRoarDoublesTwo(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "doubles roar needs 3 healthy mons on the target's side");
    CHECK(B(1).species == SPECIES_RATTATA && B(3).species == SPECIES_PIDGEY, "nobody moved");
}

static void CheckRoarDoublesThree(struct BattleSim *sim)
{
    CHECK(B(1).species == SPECIES_SANDSHREW, "the only benched mon was dragged in (got %s)", gSimSpeciesNames[B(1).species]);
    CHECK(B(3).species == SPECIES_PIDGEY, "partner untouched");
}

// ---------------------------------------------------------------- Baton Pass

static void CheckBatonPassAlone(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "baton pass fails with nobody to pass to");
    CHECK(PP(0, 0) == 39, "pp still spent (pp %d)", PP(0, 0));
}

static void CheckBatonPassEscapesWrap(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_RATTATA, "baton pass allowed while wrapped");
    CHECK(!(STATUS2(0) & STATUS2_WRAPPED), "wrap is not passed");
    CHECK(LOG_COUNT(STRINGID_PKMNHURTBY) == 1, "no tick on turn 1");
}

static int WantSeededT0(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_PKMNSEEDED, 0); }
static void CheckBatonPassLeechSeed(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_RATTATA, "passed");
    CHECK(STATUS3(0) & STATUS3_LEECHSEED, "leech seed carried");
    CHECK((STATUS3(0) & STATUS3_LEECHSEED_BATTLER) == 1, "drains to battler 1");
    CHECK(HP(0) == MAXHP(0) - MAXHP(0) / 8, "replacement drained 1/8 at the end of turn 1 (hp %d/%d)", HP(0), MAXHP(0));
}

static void CheckBatonPassPerish(struct BattleSim *sim)
{
    CHECK(LOG_COUNT(STRINGID_PKMNPERISHCOUNTFELL) == 8, "two counts per turn for four turns (%d)", LOG_COUNT(STRINGID_PKMNPERISHCOUNTFELL));
    CHECK(PARTY_HP(PLR, 1) == 0, "the replacement fainted from the passed perish count");
    CHECK(PARTY_HP(PLR, 0) > 0, "the original singer is fine in the party");
    CHECK(PARTY_HP(OPP, 0) == 0 && B(1).species == SPECIES_PIDGEY, "foe perished at the same time and was replaced");
}

static int WantPassedToRattata(struct BattleSim *sim) { return B(0).species == SPECIES_RATTATA; }
static void CheckBatonPassConfusion(struct BattleSim *sim)
{
    CHECK(STATUS2(0) & STATUS2_CONFUSION, "confusion carried to rattata");
}

static void CheckBatonPassYawn(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_RATTATA, "passed");
    CHECK(!(STATUS3(0) & STATUS3_YAWN), "yawn is not passed");
    CHECK(!(STATUS1(0) & STATUS1_SLEEP), "replacement did not fall asleep");
}

static void CheckBatonPassCurse(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_RATTATA && (STATUS2(0) & STATUS2_CURSED), "curse carried");
    CHECK(HP(0) == MAXHP(0) - MAXHP(0) / 4, "replacement loses 1/4 (hp %d/%d)", HP(0), MAXHP(0));
}

static void CheckBatonPassStagesSubFocus(struct BattleSim *sim)
{
    u16 scytherMax = (u16)GetMonData(&sim->playerParty[0], MON_DATA_MAX_HP);
    CHECK(B(0).species == SPECIES_RATTATA, "passed");
    CHECK(STAGE(0, STAT_ATK) == DEFAULT_STAT_STAGE + 2, "+2 attack carried (stage %d)", STAGE(0, STAT_ATK));
    CHECK(STATUS2(0) & STATUS2_SUBSTITUTE, "substitute carried");
    CHECK(sim->disableStructs[0].substituteHP == scytherMax / 4, "substitute hp is the passer's maxHP/4 (%d, scyther max %d)", sim->disableStructs[0].substituteHP, scytherMax);
    CHECK(STATUS2(0) & STATUS2_FOCUS_ENERGY, "focus energy carried");
    CHECK(HP(0) == MAXHP(0), "the replacement's own hp is untouched");
    CHECK(PARTY_HP(PLR, 0) == scytherMax - scytherMax / 4, "scyther paid for the substitute (party hp %d)", PARTY_HP(PLR, 0));
}

static void CheckLockOnExpires(struct BattleSim *sim)
{
    // Lock-On sets a 2-turn timer on the target; it is decremented at the end of turns 0 and 1.
    CHECK(!(STATUS3(1) & STATUS3_ALWAYS_HITS), "lock-on gone after two end-of-turns");
}
static void CheckLockOnRenewedByBatonPass(struct BattleSim *sim)
{
    // Baton Pass by the Lock-On user resets the foe's timer to 2 (then one decrement at the end of turn 1).
    CHECK(B(0).species == SPECIES_RATTATA, "passed");
    CHECK((STATUS3(1) & STATUS3_ALWAYS_HITS) == STATUS3_ALWAYS_HITS_TURN(1), "lock-on renewed for the replacement (status3 %#x)", (unsigned)STATUS3(1));
    CHECK(sim->disableStructs[1].battlerWithSureHit == 0, "sure hit from battler 0");
}

static void CheckBatonPassSpikes(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_RATTATA && LOG_HAS_T(STRINGID_PKMNHURTBYSPIKES, 1), "passed-in mon takes spikes");
    CHECK(HP(0) == MAXHP(0) - MAXHP(0) / 8, "1/8 (hp %d/%d)", HP(0), MAXHP(0));
}

static void CheckBatonPassNaturalCure(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_RATTATA, "passed");
    CHECK(PARTY_STATUS(PLR, 0) == 0, "natural cure healed the passer (status %#x)", (unsigned)PARTY_STATUS(PLR, 0));
}

static void CheckBatonPassWhiteHerb(struct BattleSim *sim)
{
    int spikes = FirstLog(sim, STRINGID_PKMNHURTBYSPIKES, 2);
    int herb = FirstLog(sim, STRINGID_PKMNSITEMRESTOREDSTATUS, 2);
    CHECK(B(0).species == SPECIES_RATTATA, "passed");
    CHECK(spikes >= 0 && herb >= 0, "spikes (%d) and white herb (%d) both on turn 2", spikes, herb);
    CHECK(spikes < herb, "spikes damage before the switch-in item effect");
    CHECK(STAGE(0, STAT_ATK) == DEFAULT_STAT_STAGE, "white herb restored the passed -1 attack (stage %d)", STAGE(0, STAT_ATK));
    CHECK(B(0).item == ITEM_NONE, "white herb consumed");
}

// ---------------------------------------------------------------- Pursuit

static int WantNoCrit(struct BattleSim *sim) { return !Sc_LogHas(sim, STRINGID_CRITICALHIT, SC_ANY_TURN); }
static void CheckPursuitSwitchKO(struct BattleSim *sim)
{
    // Pursuit is Dark, i.e. special in Gen 3: Rattata L50 (sp.atk 45) vs Bulbasaur L50 (sp.def 93 with the
    // badge boost): base 10 -> 8..10 normally, but the switch doubles it to 17..20, which KOs at 14 HP.
    CHECK(PARTY_HP(PLR, 0) == 0, "doubled pursuit KOed the switching bulbasaur (party hp %d)", PARTY_HP(PLR, 0));
    CHECK(B(0).species == SPECIES_SQUIRTLE && HP(0) == MAXHP(0), "the chosen replacement still came in untouched");
    CHECK(LOG_INDEX_T(STRINGID_USEDMOVE, 1, 0) >= 0 && LOG_INDEX_T(STRINGID_USEDMOVE, 1, 0) < LOG_INDEX_T(STRINGID_SWITCHINMON, 0, 0), "pursuit hit before the switch-in");
    CHECK(Sc_LogIndex(sim, STRINGID_USEDMOVE, 1, 1, 0) < 0, "pursuer did not act a second time");
    CHECK(PP(1, 0) == 19, "pursuit pp spent once (pp %d)", PP(1, 0));
    CHECK(OUTCOME() == 0, "battle goes on");
}

static void CheckPursuitNormal(struct BattleSim *sim)
{
    CHECK(HP(0) >= 25 - 10 && HP(0) <= 25 - 8, "undoubled pursuit: 8..10 damage (hp %d)", HP(0));
}

static void CheckPursuitBatonPass(struct BattleSim *sim)
{
    // Scyther passes first; the slower pursuer then hits the incoming Squirtle (sp.def 92) for the normal
    // 8..10 - a Baton Pass is not a switch action, so no doubling.
    CHECK(B(0).species == SPECIES_SQUIRTLE, "passed to squirtle");
    CHECK(PARTY_HP(PLR, 0) == (int)GetMonData(&sim->playerParty[0], MON_DATA_MAX_HP), "scyther untouched");
    CHECK(HP(0) >= 25 - 10 && HP(0) <= 25 - 8, "normal pursuit damage on the replacement (hp %d)", HP(0));
    CHECK(LOG_INDEX_T(STRINGID_USEDMOVE, 0, 0) >= 0 && LOG_INDEX_T(STRINGID_USEDMOVE, 0, 0) < LOG_INDEX_T(STRINGID_USEDMOVE, 1, 0), "pursuit came after the pass");
}

static void CheckPursuitAsleep(struct BattleSim *sim)
{
    CHECK(PARTY_HP(PLR, 0) == (int)GetMonData(&sim->playerParty[0], MON_DATA_MAX_HP), "sleeping pursuer gets no switch hit");
    CHECK(B(0).species == SPECIES_SQUIRTLE && HP(0) == MAXHP(0), "switch went through cleanly");
    CHECK(LOG_HAS(STRINGID_PKMNFASTASLEEP), "pursuer slept through its turn");
}

static void CheckPursuitFrozen(struct BattleSim *sim)
{
    CHECK(PARTY_HP(PLR, 0) == (int)GetMonData(&sim->playerParty[0], MON_DATA_MAX_HP), "frozen pursuer gets no switch hit");
    CHECK(B(0).species == SPECIES_SQUIRTLE && HP(0) == MAXHP(0), "switch went through cleanly");
    CHECK(LOG_HAS(STRINGID_PKMNISFROZEN), "pursuer stayed frozen");
}

static void CheckPursuitDoubles(struct BattleSim *sim)
{
    // BattleScript_ActionSwitch uses setmultihit 2 in doubles: jumpifnopursuitswitchdmg checks the right foe
    // (counter 2) and then the left foe (counter 1), so both Pursuits land on the switcher, right foe first.
    int p3 = LOG_INDEX_T(STRINGID_USEDMOVE, 3, 0), p1 = LOG_INDEX_T(STRINGID_USEDMOVE, 1, 0), sw = LOG_INDEX_T(STRINGID_SWITCHINMON, 0, 0);
    CHECK(p3 >= 0 && p1 >= 0 && sw >= 0 && p3 < p1 && p1 < sw, "right foe's pursuit (%d), left foe's (%d), then the switch-in (%d)", p3, p1, sw);
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_PURSUIT, 0) && Sc_LogCount(sim, STRINGID_USEDMOVE, 0) == 3, "two pursuits and snorlax's move (%d moves)", Sc_LogCount(sim, STRINGID_USEDMOVE, 0));
    CHECK(Sc_LogIndex(sim, STRINGID_USEDMOVE, 1, 1, 0) < 0 && Sc_LogIndex(sim, STRINGID_USEDMOVE, 3, 1, 0) < 0, "neither pursuer acted again");
    CHECK(PARTY_HP(PLR, 0) > 0 && PARTY_HP(PLR, 0) <= 100 - 2 * 17, "bulbasaur took two doubled hits (party hp %d)", PARTY_HP(PLR, 0));
    CHECK(B(0).species == SPECIES_SQUIRTLE && HP(0) == MAXHP(0), "squirtle came in untouched");
    CHECK(PP(1, 0) == 19 && PP(3, 0) == 19, "each pursuit pp spent once (%d %d)", PP(1, 0), PP(3, 0));
}

// ---------------------------------------------------------------- Perish Song

static void CheckPerishTiming(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_FAINTINTHREE, 0), "faint in three message");
    CHECK(LOG_COUNT(STRINGID_PKMNPERISHCOUNTFELL) == 6, "count fell twice per turn for 3 turns (%d)", LOG_COUNT(STRINGID_PKMNPERISHCOUNTFELL));
    CHECK(HP(0) > 0 && HP(1) > 0, "both alive after three end-of-turns");
    CHECK(sim->disableStructs[0].perishSongTimer == 0 && sim->disableStructs[1].perishSongTimer == 0, "timers at 0 (%d %d)", sim->disableStructs[0].perishSongTimer, sim->disableStructs[1].perishSongTimer);
    CHECK((STATUS3(0) & STATUS3_PERISH_SONG) && (STATUS3(1) & STATUS3_PERISH_SONG), "still under the song");
}

static void CheckPerishDraw(struct BattleSim *sim)
{
    CHECK(HP(0) == 0 && HP(1) == 0, "both perished");
    CHECK(OUTCOME() == B_OUTCOME_DREW, "both sides out at once: draw (outcome %d)", OUTCOME());
    CHECK(LOG_COUNT(STRINGID_PKMNPERISHCOUNTFELL) == 8, "final count messages (%d)", LOG_COUNT(STRINGID_PKMNPERISHCOUNTFELL));
}

static void CheckPerishSoundproofUser(struct BattleSim *sim)
{
    CHECK(!(STATUS3(0) & STATUS3_PERISH_SONG), "soundproof singer unaffected");
    CHECK(STATUS3(1) & STATUS3_PERISH_SONG, "foe affected");
    CHECK(!LOG_HAS(STRINGID_BUTITFAILED), "song did not fail");
}

static void CheckPerishAllSoundproof(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "fails when nobody can be affected");
    CHECK(!(STATUS3(0) & STATUS3_PERISH_SONG) && !(STATUS3(1) & STATUS3_PERISH_SONG), "nobody affected");
}

static void CheckPerishSwitchClears(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_PIDGEY && !(STATUS3(0) & STATUS3_PERISH_SONG), "replacement is free of the song");
    CHECK(STATUS3(1) & STATUS3_PERISH_SONG, "singer still counting down");
    CHECK(Sc_LogCount(sim, STRINGID_PKMNPERISHCOUNTFELL, 1) == 1, "only one count message on turn 1");
}

static void CheckPerishReplacements(struct BattleSim *sim)
{
    // Both perish at the end of turn 3; BattleTurnPassed re-enters after each perish script and reruns
    // HandleFaintedMonActions, so both replacements come in during that same end-of-turn.
    CHECK(PARTY_HP(PLR, 0) == 0 && PARTY_HP(OPP, 0) == 0, "both singers fainted");
    CHECK(Sc_LogCount(sim, STRINGID_SWITCHINMON, 3) == 2, "both replacements sent out on the perish turn (%d)", Sc_LogCount(sim, STRINGID_SWITCHINMON, 3));
    CHECK(Sc_LogCount(sim, STRINGID_PKMNPERISHCOUNTFELL, 3) == 2, "both final counts");
    CHECK(B(0).species == SPECIES_RATTATA && B(1).species == SPECIES_PIDGEY, "replacements in");
    CHECK(!(STATUS3(0) & STATUS3_PERISH_SONG) && !(STATUS3(1) & STATUS3_PERISH_SONG), "replacements are not under the song");
    CHECK(OUTCOME() == 0, "battle continues");
}

// ---------------------------------------------------------------- Teleport

static void CheckTeleport(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "teleport fails in a trainer battle");
    CHECK(OUTCOME() == 0 && sim->turnCount == 1, "battle continues");
}

// ---------------------------------------------------------------- what a switch resets / keeps

static void CheckSwitchStagesBurn(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_SNORLAX, "back in");
    CHECK(STAGE(0, STAT_ATK) == DEFAULT_STAT_STAGE, "stages reset by switching (stage %d)", STAGE(0, STAT_ATK));
    CHECK(STATUS1(0) & STATUS1_BURN, "burn kept");
    CHECK(LOG_COUNT(STRINGID_PKMNHURTBYBURN) == 2, "burn ticked on the two turns it was in (%d)", LOG_COUNT(STRINGID_PKMNHURTBYBURN));
}

static void CheckSwitchSleepCounter(struct BattleSim *sim)
{
    // The attack canceller writes the decremented counter back to the party mon, so it survives a switch.
    CHECK(LOG_HAS_T(STRINGID_PKMNFASTASLEEP, 0), "slept through turn 0");
    CHECK((STATUS1(0) & STATUS1_SLEEP) == STATUS1_SLEEP_TURN(3), "sleep counter 4 -> 3 kept across the switch (got %d)", (int)(STATUS1(0) & STATUS1_SLEEP));
}

static void CheckSwitchToxicCounter(struct BattleSim *sim)
{
    // The toxic counter is only incremented in gBattleMons and never written back, so the party mon still
    // has counter 0 and the first tick after re-entry is 1/16 again.
    u16 max = MAXHP(0);
    CHECK(((STATUS1(0) & STATUS1_TOXIC_COUNTER) >> 8) == 1, "toxic counter restarted at 1 (got %d)", (STATUS1(0) & STATUS1_TOXIC_COUNTER) >> 8);
    CHECK(HP(0) == max - (max / 16) * (1 + 2) - max / 16, "1/16 + 2/16 before, 1/16 after (hp %d/%d)", HP(0), max);
}

static void CheckSwitchClearsVolatiles(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_SNORLAX, "back in");
    CHECK(!(STATUS2(0) & STATUS2_CONFUSION), "confusion cleared");
    CHECK(!(STATUS3(0) & STATUS3_YAWN) && !(STATUS1(0) & STATUS1_SLEEP), "yawn cleared, never fell asleep");
}

static void CheckSwitchClearsEncore(struct BattleSim *sim)
{
    CHECK(sim->disableStructs[1].encoredMove == MOVE_NONE, "encore cleared by the switch");
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_TACKLE, 3), "free to use tackle on turn 3");
}

static void CheckNaturalCureSwitch(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_RATTATA, "switched");
    CHECK(PARTY_STATUS(PLR, 0) == 0, "burn cured on switch-out (status %#x)", (unsigned)PARTY_STATUS(PLR, 0));
}

static void CheckSwitchClearsInfatuation(struct BattleSim *sim)
{
    // SwitchInClearSetData clears STATUS2_INFATUATED_WITH(leaving battler) on every battler; infatuation has
    // no timer, so without the switch nidoking would still be in love.
    CHECK(LOG_HAS_T(STRINGID_PKMNFELLINLOVE, 0), "attract landed on turn 0");
    CHECK(B(0).species == SPECIES_RATTATA, "nidoqueen switched out");
    CHECK(!(STATUS2(1) & STATUS2_INFATUATION), "infatuation cleared when its cause left (status2 %#x)", (unsigned)STATUS2(1));
}

static void CheckSpikesBeforeIntimidate(struct BattleSim *sim)
{
    int spikes = FirstLog(sim, STRINGID_PKMNHURTBYSPIKES, 1);
    int intim = FirstLog(sim, STRINGID_PKMNCUTSATTACKWITH, 1);
    CHECK(B(0).species == SPECIES_ARCANINE, "arcanine in");
    CHECK(HP(0) == MAXHP(0) - MAXHP(0) / 8, "took spikes (hp %d/%d)", HP(0), MAXHP(0));
    CHECK(spikes >= 0 && intim >= 0 && spikes < intim, "spikes (%d) resolve before the switch-in ability (%d)", spikes, intim);
    CHECK(STAGE(1, STAT_ATK) == DEFAULT_STAT_STAGE - 1, "intimidate landed");
}

// ---------------------------------------------------------------- faint replacement timing

static void CheckFaintReplacementMidTurn(struct BattleSim *sim)
{
    int sw = FirstLog(sim, STRINGID_SWITCHINMON, 1);
    int sand = FirstLog(sim, STRINGID_SANDSTORMRAGES, 1);
    CHECK(PARTY_HP(PLR, 0) == 0, "rattata KOed on turn 1");
    CHECK(LOG_INDEX_T(STRINGID_USEDMOVE, 0, 1) < 0, "the KOed mon never got to act");
    CHECK(sw >= 0 && sand >= 0 && sw < sand, "replacement (%d) came in before the end-of-turn sandstorm (%d)", sw, sand);
    CHECK(B(0).species == SPECIES_PIDGEY && HP(0) == MAXHP(0) - MAXHP(0) / 16, "so the replacement took the sandstorm chip (hp %d/%d)", HP(0), MAXHP(0));
    CHECK(sim->turnCount == 2, "two turns elapsed");
}

static void CheckDoublesFaintOthersAct(struct BattleSim *sim)
{
    int sw = FirstLog(sim, STRINGID_SWITCHINMON, 0);
    CHECK(PARTY_HP(PLR, 0) == 0 && B(0).species == SPECIES_PIDGEY, "rattata KOed and replaced by pidgey");
    CHECK(sw >= 0 && sw < LOG_INDEX_T(STRINGID_USEDMOVE, 2, 0) && sw < LOG_INDEX_T(STRINGID_USEDMOVE, 3, 0), "the replacement came in before the slower battlers acted");
    CHECK(LOG_INDEX_T(STRINGID_USEDMOVE, 2, 0) >= 0 && LOG_INDEX_T(STRINGID_USEDMOVE, 3, 0) >= 0, "both slow battlers still acted");
    CHECK(LOG_INDEX_T(STRINGID_USEDMOVE, 0, 0) < 0, "neither the KOed mon nor its replacement acted");
}

static void CheckExplosionBothReplaced(struct BattleSim *sim)
{
    int first = FirstLog(sim, STRINGID_SWITCHINMON, 0);
    CHECK(PARTY_HP(PLR, 0) == 0 && PARTY_HP(OPP, 0) == 0, "both fainted");
    CHECK(B(0).species == SPECIES_RATTATA && B(1).species == SPECIES_PIDGEY, "both replaced");
    CHECK(Sc_LogCount(sim, STRINGID_SWITCHINMON, 0) == 2, "two switch-ins");
    // HandleFaintedMonActions walks battlers in id order: the player's replacement (Rattata, 105 hp) is
    // sent out before the foe's (Pidgey, 115 hp).
    CHECK(first >= 0 && sim->log[first].hpTarget == MAXHP(0) && first + 1 < sim->logCount && sim->log[first + 1].hpTarget == MAXHP(1),
          "player's replacement first (hp %d then %d)", first >= 0 ? sim->log[first].hpTarget : -1, first >= 0 ? sim->log[first + 1].hpTarget : -1);
    CHECK(OUTCOME() == 0, "battle continues");
}

// ---------------------------------------------------------------- Leech Seed / Future Sight / Wish on replacements

static void CheckLeechSeedHealsReplacement(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_RATTATA, "seeder switched out");
    CHECK(HP(0) == 20 + MAXHP(1) / 8, "replacement in the seeder's slot receives the drain (hp %d, drain %d)", HP(0), MAXHP(1) / 8);
    CHECK(HP(1) == MAXHP(1) - 2 * (MAXHP(1) / 8), "foe drained on both turns (hp %d/%d)", HP(1), MAXHP(1));
}

static int WantSeededAny(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_PKMNSEEDED, SC_ANY_TURN); }
static void CheckLeechSeedSeederFainted(struct BattleSim *sim)
{
    // The seeder is KOed and replaced mid-turn, so at the end of the turn the seeder's slot holds the
    // replacement, which receives the drain.
    CHECK(PARTY_HP(PLR, 0) == 0 && B(0).species == SPECIES_RATTATA, "seeder fainted and was replaced");
    CHECK(FirstLog(sim, STRINGID_SWITCHINMON, 0) < FirstLog(sim, STRINGID_PKMNSAPPEDBYLEECHSEED, 0), "replacement before the drain");
    CHECK(HP(1) == MAXHP(1) - MAXHP(1) / 8, "foe drained (hp %d/%d)", HP(1), MAXHP(1));
    CHECK(HP(0) == 20 + MAXHP(1) / 8, "replacement healed by the drain (hp %d)", HP(0));
}

static void CheckSeededSwitchClears(struct BattleSim *sim)
{
    CHECK(B(1).species == SPECIES_RATTATA, "rattata back in");
    CHECK(!(STATUS3(1) & STATUS3_LEECHSEED), "leech seed cleared by switching");
    CHECK(LOG_COUNT(STRINGID_PKMNSAPPEDBYLEECHSEED) == 1, "only the turn-0 drain (%d)", LOG_COUNT(STRINGID_PKMNSAPPEDBYLEECHSEED));
}

static void CheckFutureSightNotYet(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNFORESAWATTACK, 0), "foresaw message");
    CHECK(HP(1) == MAXHP(1) && !LOG_HAS(STRINGID_PKMNTOOKATTACK), "nothing after two end-of-turns");
    CHECK(sim->wishFutureKnock.futureSightCounter[1] == 1, "counter at 1 (%d)", sim->wishFutureKnock.futureSightCounter[1]);
}

static int WantFutureHit(struct BattleSim *sim) { return HP(1) < MAXHP(1); }
static void CheckFutureSightLands(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNTOOKATTACK, 2), "lands at the end of the third turn");
    CHECK(sim->wishFutureKnock.futureSightCounter[1] == 0, "counter cleared");
}

static void CheckFutureSightDark(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNTOOKATTACK, 2) && HP(1) < MAXHP(1), "typeless: hits a dark type");
    CHECK(!LOG_HAS(STRINGID_ITDOESNTAFFECT), "no immunity message");
}

static void CheckFutureSightHitsReplacement(struct BattleSim *sim)
{
    // The attack is stored per target position; whoever occupies the slot when the counter runs out takes it.
    CHECK(B(1).species == SPECIES_PIDGEY, "pidgey came in on turn 1");
    CHECK(LOG_HAS_T(STRINGID_PKMNTOOKATTACK, 2) && HP(1) < MAXHP(1), "the replacement took the attack (hp %d/%d)", HP(1), MAXHP(1));
    CHECK(PARTY_HP(OPP, 0) == (int)GetMonData(&sim->enemyParty[0], MON_DATA_MAX_HP), "the original target is untouched");
}

static void CheckFutureSightPending(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNFORESAWATTACK, 0) && LOG_HAS_T(STRINGID_BUTITFAILED, 1), "second future sight on the same slot fails");
}

static void CheckFutureSightUserSwitched(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_RATTATA, "alakazam left");
    CHECK(LOG_HAS_T(STRINGID_PKMNTOOKATTACK, 2), "attack still lands after the user switched out");
}

static void CheckWishReplacementExact(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_RATTATA && LOG_HAS_T(STRINGID_PKMNWISHCAMETRUE, 1), "wish came true for the replacement");
    CHECK(HP(0) == 20 + MAXHP(0) / 2, "half of the replacement's max hp (hp %d/%d)", HP(0), MAXHP(0));
}

static void CheckWishPending(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 1), "second wish fails while one is pending");
    CHECK(LOG_HAS_T(STRINGID_PKMNWISHCAMETRUE, 1), "first wish still came true");
}

static void CheckWishBeforePoison(struct BattleSim *sim)
{
    u16 max = MAXHP(0);
    // hp: 31 -> (t0 poison) 10 -> (t1 wish +85, then poison -21) 74. Poison first on turn 1 would KO.
    CHECK(max / 8 == 21 && max / 16 == 10, "setup: clefable max hp 170 (got %d)", max);
    CHECK(HP(0) == max / 16 + max / 2 - max / 8, "wish (field effects) heals before poison (battler effects) (hp %d)", HP(0));
}

static void CheckWishFullHp(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNWISHCAMETRUE, 1) && LOG_HAS_T(STRINGID_PKMNHPFULL, 1), "wish at full hp prints the full-hp message");
}

static const struct Scenario sScenarios[] =
{
    // ---- Wrap family
    { .name = "wrap_min_duration_two_ticks",
      .player = { MON(SPECIES_ARBOK, 50, MOVE_WRAP, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL), MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, SC_SWITCH(1)), T(1, SC_SWITCH(1)), T(1, SC_SWITCH(1)) }, .turns = 4,
      .wantSeed = WantWrapMin, .check = CheckWrapMin },
    { .name = "wrap_max_duration_five_ticks",
      .player = { MON(SPECIES_ARBOK, 50, MOVE_WRAP, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL), MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, SC_SWITCH(1)), T(1, SC_SWITCH(1)), T(1, SC_SWITCH(1)), T(1, SC_SWITCH(1)), T(1, SC_SWITCH(1)), T(1, SC_SWITCH(1)) }, .turns = 7,
      .wantSeed = WantWrapMax, .check = CheckWrapMax },
    { .name = "wrap_twice_does_not_extend",
      .player = { MON(SPECIES_ARBOK, 50, MOVE_WRAP, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(0, 0), T(1, 0) }, .turns = 3, .wantSeed = WantWrapTwice, .check = CheckWrapTwice },
    { .name = "wrapper_switch_frees_target",
      .player = { MON(SPECIES_ARBOK, 50, MOVE_WRAP, SPL, SPL, SPL), MON(SPECIES_SANDSHREW, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL), MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(SC_SWITCH(1), 0), T(0, SC_SWITCH(1)) }, .turns = 3, .wantSeed = WantWrapHitT0, .check = CheckWrapperSwitchFrees },
    { .name = "wrapper_faint_frees_target",
      .player = { MON(SPECIES_MACHAMP, 50, MOVE_KARATE_CHOP, SPL, SPL, SPL) },
      .enemy = { { .species = SPECIES_ARBOK, .level = 50, .moves = { MOVE_WRAP, SPL, SPL, SPL }, .hp = 1, .hpSet = 1 }, MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantWrapHitT0, .check = CheckWrapperFaintFrees },
    { .name = "wrapper_baton_pass_frees_target",
      .player = { MON(SPECIES_ARBOK, 50, MOVE_WRAP, MOVE_BATON_PASS, SPL, SPL), MON(SPECIES_SANDSHREW, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL), MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, SC_SWITCH(1)), T(0, SC_SWITCH(1)) }, .turns = 3, .wantSeed = WantWrapHitT0, .check = CheckWrapperBatonPassFrees },
    { .name = "substitute_frees_user_from_wrap",
      .player = { MON(SPECIES_ARBOK, 50, MOVE_WRAP, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SUBSTITUTE, SPL, SPL, SPL), MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, SC_SWITCH(1)) }, .turns = 2, .wantSeed = WantWrapHitT0, .check = CheckSubstituteFreesWrap },
    { .name = "wrap_no_effect_on_ghost",
      .player = { MON(SPECIES_ARBOK, 50, MOVE_WRAP, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_GASTLY, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckWrapGhost },
    { .name = "fire_spin_traps_ghost",
      .player = { MON(SPECIES_NINETALES, 50, MOVE_FIRE_SPIN, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_GASTLY, 50, SPL, SPL, SPL, SPL), MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, SC_SWITCH(1)) }, .turns = 2, .wantSeed = WantVortex, .check = CheckFireSpinGhost },
    { .name = "wrap_blocked_by_substitute",
      .player = { MON(SPECIES_ARBOK, 50, MOVE_WRAP, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SUBSTITUTE, SPL, SPL, SPL) },
      .actions = { T(1, 0), T(0, 1) }, .turns = 2, .wantSeed = WantNoMiss, .check = CheckWrapSubstitute },
    { .name = "bind_message",
      .player = { MON(SPECIES_ONIX, 50, MOVE_BIND, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_SNORLAX, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantBind, .check = CheckBindMsg },
    { .name = "clamp_message",
      .player = { MON(SPECIES_CLOYSTER, 50, MOVE_CLAMP, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_SNORLAX, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantClamp, .check = CheckClampMsg },
    { .name = "sand_tomb_message",
      .player = { MON(SPECIES_FLYGON, 50, MOVE_SAND_TOMB, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_SNORLAX, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantSandTomb, .check = CheckSandTombMsg },
    { .name = "rapid_spin_frees_wrap",
      .player = { MON(SPECIES_STARMIE, 50, SPL, MOVE_RAPID_SPIN, SPL, SPL), MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_ARBOK, 50, MOVE_WRAP, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, 1), T(SC_SWITCH(1), 1) }, .turns = 3, .wantSeed = WantWrapHitT0, .check = CheckRapidSpinWrap },
    { .name = "rapid_spin_frees_wrap_and_leech_seed",
      .player = { MON(SPECIES_STARMIE, 50, SPL, MOVE_RAPID_SPIN, SPL, SPL) },
      .enemy = { MON(SPECIES_ARBOK, 50, MOVE_WRAP, MOVE_LEECH_SEED, SPL, SPL) },
      .actions = { T(0, 0), T(0, 1), T(1, 2) }, .turns = 3, .wantSeed = WantWrapAndSeed, .check = CheckRapidSpinWrapAndSeed },
    { .name = "rapid_spin_clears_spikes",
      .player = { MON(SPECIES_STARMIE, 50, SPL, MOVE_RAPID_SPIN, SPL, SPL), MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_FORRETRESS, 50, MOVE_SPIKES, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, 1), T(SC_SWITCH(1), 1) }, .turns = 3, .check = CheckRapidSpinSpikes },

    // ---- Mean Look family
    { .name = "mean_look_traps",
      .player = { MON(SPECIES_GENGAR, 50, MOVE_MEAN_LOOK, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL), MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, SC_SWITCH(1)) }, .turns = 2, .check = CheckMeanLook },
    { .name = "mean_look_fails_vs_substitute",
      .player = { MON(SPECIES_GENGAR, 50, MOVE_MEAN_LOOK, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SUBSTITUTE, SPL, SPL, SPL) },
      .actions = { T(1, 0), T(0, 1) }, .turns = 2, .check = CheckMeanLookSubstitute },
    { .name = "mean_look_twice_fails",
      .player = { MON(SPECIES_GENGAR, 50, MOVE_MEAN_LOOK, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .check = CheckMeanLookTwice },
    { .name = "mean_look_user_switch_frees",
      .player = { MON(SPECIES_GENGAR, 50, MOVE_MEAN_LOOK, SPL, SPL, SPL), MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL), MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(SC_SWITCH(1), 0), T(0, SC_SWITCH(1)) }, .turns = 3, .check = CheckMeanLookUserSwitch },
    { .name = "mean_look_user_faint_frees",
      .player = { MON(SPECIES_MACHAMP, 50, MOVE_KARATE_CHOP, SPL, SPL, SPL), MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .enemy = { { .species = SPECIES_UMBREON, .level = 50, .moves = { MOVE_MEAN_LOOK, SPL, SPL, SPL }, .hp = 1, .hpSet = 1 }, MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(SC_SWITCH(1), 0) }, .turns = 2, .check = CheckMeanLookUserFaint },
    { .name = "mean_look_user_baton_pass_keeps_trap",
      .player = { MON(SPECIES_UMBREON, 50, MOVE_MEAN_LOOK, MOVE_BATON_PASS, SPL, SPL), MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL), MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, 0), T(0, SC_SWITCH(1)) }, .turns = 3, .check = CheckMeanLookUserBatonPass },
    { .name = "mean_look_target_baton_pass_stays_trapped",
      .player = { MON(SPECIES_SCYTHER, 50, MOVE_BATON_PASS, SPL, SPL, SPL), MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_UMBREON, 50, MOVE_MEAN_LOOK, SPL, SPL, SPL) },
      .actions = { T(1, 0), T(0, 1), T(SC_SWITCH(0), 1) }, .turns = 3, .check = CheckMeanLookTargetBatonPass },
    { .name = "roar_ignores_mean_look",
      .player = { MON(SPECIES_ARCANINE, 50, MOVE_ROAR, SPL, SPL, SPL), MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_UMBREON, 50, MOVE_MEAN_LOOK, SPL, SPL, SPL), MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(1, 0), T(0, 1), T(SC_SWITCH(1), 0) }, .turns = 3, .check = CheckRoarIgnoresMeanLook },
    { .name = "spider_web_traps",
      .player = { MON(SPECIES_ARIADOS, 50, MOVE_SPIDER_WEB, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL), MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, SC_SWITCH(1)) }, .turns = 2, .check = CheckSpiderWeb },

    // ---- Ingrain
    { .name = "ingrain_heals_and_traps",
      .player = { { .species = SPECIES_BULBASAUR, .level = 50, .moves = { MOVE_INGRAIN, SPL, SPL, SPL }, .hp = 40, .hpSet = 1 }, MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_SNORLAX, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(SC_SWITCH(1), 0), T(SC_SWITCH(1), 0) }, .turns = 3, .check = CheckIngrain },
    { .name = "ingrain_twice_fails",
      .player = { MON(SPECIES_BULBASAUR, 50, MOVE_INGRAIN, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_SNORLAX, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .check = CheckIngrainTwice },
    { .name = "roar_fails_vs_ingrain",
      .player = { MON(SPECIES_ARCANINE, 50, MOVE_ROAR, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_BULBASAUR, 50, MOVE_INGRAIN, SPL, SPL, SPL), MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckRoarVsIngrain },
    { .name = "ingrain_heal_before_poison_damage",
      .player = { { .species = SPECIES_BULBASAUR, .level = 50, .moves = { MOVE_INGRAIN, SPL, SPL, SPL }, .hp = 15, .hpSet = 1, .status = STATUS1_POISON } },
      .enemy = { MON(SPECIES_SNORLAX, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckIngrainBeforePoison },
    { .name = "baton_pass_carries_ingrain",
      .player = { MON(SPECIES_BULBASAUR, 50, MOVE_INGRAIN, MOVE_BATON_PASS, SPL, SPL), { .species = SPECIES_RATTATA, .level = 50, .moves = { SPL, SPL, SPL, SPL }, .hp = 20, .hpSet = 1 } },
      .enemy = { MON(SPECIES_SNORLAX, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, 0), T(SC_SWITCH(0), 0) }, .turns = 3, .check = CheckBatonPassIngrain },

    // ---- trapping abilities
    { .name = "shadow_tag_traps_foe_not_holder",
      .player = { MON(SPECIES_WOBBUFFET, 50, SPL, SPL, SPL, SPL), MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL), MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(SC_SWITCH(1), SC_SWITCH(1)), T(0, SC_SWITCH(1)) }, .turns = 2, .check = CheckShadowTag },
    { .name = "shadow_tag_mutual",
      .player = { MON(SPECIES_WOBBUFFET, 50, SPL, SPL, SPL, SPL), MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_WOBBUFFET, 50, SPL, SPL, SPL, SPL), MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(SC_SWITCH(1), SC_SWITCH(1)) }, .turns = 1, .check = CheckShadowTagMutual },
    { .name = "arena_trap_flying_exempt",
      .player = { MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL), MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON_AB(SPECIES_DUGTRIO, 50, SPL, SPL, SPL, SPL, 1) },
      .actions = { T(SC_SWITCH(1), 0) }, .turns = 1, .check = CheckSwitchedToRattata },
    { .name = "arena_trap_levitate_exempt",
      .player = { MON(SPECIES_GENGAR, 50, SPL, SPL, SPL, SPL), MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON_AB(SPECIES_DUGTRIO, 50, SPL, SPL, SPL, SPL, 1) },
      .actions = { T(SC_SWITCH(1), 0) }, .turns = 1, .check = CheckSwitchedToRattata },
    { .name = "arena_trap_traps_grounded",
      .player = { MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL), MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON_AB(SPECIES_DUGTRIO, 50, SPL, SPL, SPL, SPL, 1) },
      .actions = { T(SC_SWITCH(1), 0) }, .turns = 1, .check = CheckStuckRattata },
    { .name = "magnet_pull_traps_steel",
      .player = { MON(SPECIES_SKARMORY, 50, SPL, SPL, SPL, SPL), MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_MAGNETON, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(SC_SWITCH(1), 0) }, .turns = 1, .check = CheckStuckSkarmory },
    { .name = "magnet_pull_ignores_non_steel",
      .player = { MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL), MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_MAGNETON, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(SC_SWITCH(1), 0) }, .turns = 1, .check = CheckSwitchedToPidgey },
    { .name = "baton_pass_under_shadow_tag",
      .player = { MON(SPECIES_SCYTHER, 50, MOVE_BATON_PASS, SPL, SPL, SPL), MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_WOBBUFFET, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckBatonPassUnderShadowTag },
    { .name = "faint_replacement_under_shadow_tag",
      .player = { { .species = SPECIES_RATTATA, .level = 50, .moves = { SPL, SPL, SPL, SPL }, .hp = 1, .hpSet = 1 }, MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_WOBBUFFET, 50, MOVE_TACKLE, SPL, SPL, SPL) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckFaintReplacementUnderShadowTag },

    // ---- Roar / Whirlwind
    { .name = "roar_drags_random_slot2",
      .player = { MON(SPECIES_ARCANINE, 50, MOVE_ROAR, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SWORDS_DANCE, SPL, SPL, SPL), MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL), MON(SPECIES_SANDSHREW, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantRoarSandshrew, .check = CheckRoarDrag },
    { .name = "roar_drags_random_slot1",
      .player = { MON(SPECIES_ARCANINE, 50, MOVE_ROAR, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SWORDS_DANCE, SPL, SPL, SPL), MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL), MON(SPECIES_SANDSHREW, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantRoarPidgey, .check = CheckRoarDrag },
    { .name = "whirlwind_goes_last",
      .player = { MON(SPECIES_PIDGEOT, 50, MOVE_WHIRLWIND, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_TACKLE, SPL, SPL, SPL), MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckWhirlwindLast },
    { .name = "roar_fails_vs_last_mon",
      .player = { MON(SPECIES_ARCANINE, 50, MOVE_ROAR, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckRoarFailsLastMon },
    { .name = "roar_fails_vs_suction_cups",
      .player = { MON(SPECIES_ARCANINE, 50, MOVE_ROAR, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_OCTILLERY, 50, SPL, SPL, SPL, SPL), MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckRoarSuctionCups },
    { .name = "roar_blocked_by_soundproof",
      .player = { MON(SPECIES_ARCANINE, 50, MOVE_ROAR, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_ELECTRODE, 50, SPL, SPL, SPL, SPL), MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckRoarSoundproof },
    { .name = "roar_level_check_fails",
      .player = { MON(SPECIES_GROWLITHE, 10, MOVE_ROAR, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_RATTATA, 100, SPL, SPL, SPL, SPL), MON(SPECIES_PIDGEY, 100, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantRoarLevelFail, .check = CheckRoarLevelFail },
    { .name = "roar_level_check_passes",
      .player = { MON(SPECIES_GROWLITHE, 10, MOVE_ROAR, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_RATTATA, 100, SPL, SPL, SPL, SPL), MON(SPECIES_PIDGEY, 100, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantRoarPidgey, .check = CheckRoarLevelPass },
    { .name = "roar_through_substitute",
      .player = { MON(SPECIES_ARCANINE, 50, MOVE_ROAR, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SUBSTITUTE, SPL, SPL, SPL), MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(1, 0), T(0, 1) }, .turns = 2, .check = CheckRoarThroughSubstitute },
    { .name = "roar_triggers_natural_cure",
      .player = { MON(SPECIES_ARCANINE, 50, MOVE_ROAR, SPL, SPL, SPL) },
      .enemy = { { .species = SPECIES_CHANSEY, .level = 50, .moves = { SPL, SPL, SPL, SPL }, .status = STATUS1_BURN }, MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckRoarNaturalCure },
    { .name = "roar_into_spikes",
      .player = { MON(SPECIES_FORRETRESS, 50, MOVE_SPIKES, MOVE_ROAR, SPL, SPL) },
      .enemy = { MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL), MON(SPECIES_SANDSHREW, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckRoarIntoSpikes },
    { .name = "roar_doubles_fails_with_two_mons",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { MON(SPECIES_ARCANINE, 50, MOVE_ROAR, SPL, SPL, SPL), MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL), MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { { 2, 0, 0, 0 } }, .turns = 1, .check = CheckRoarDoublesTwo },
    { .name = "roar_doubles_drags_benched_mon",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { MON(SPECIES_ARCANINE, 50, MOVE_ROAR, SPL, SPL, SPL), MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL), MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL), MON(SPECIES_SANDSHREW, 50, SPL, SPL, SPL, SPL) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { { 2, 0, 0, 0 } }, .turns = 1, .check = CheckRoarDoublesThree },

    // ---- Baton Pass
    { .name = "baton_pass_alone_fails",
      .player = { MON(SPECIES_SCYTHER, 50, MOVE_BATON_PASS, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_SNORLAX, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckBatonPassAlone },
    { .name = "baton_pass_escapes_wrap",
      .player = { MON(SPECIES_SCYTHER, 50, SPL, MOVE_BATON_PASS, SPL, SPL), MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_ARBOK, 50, MOVE_WRAP, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .wantSeed = WantWrapHitT0, .check = CheckBatonPassEscapesWrap },
    { .name = "baton_pass_carries_leech_seed",
      .player = { MON(SPECIES_SCYTHER, 50, SPL, MOVE_BATON_PASS, SPL, SPL), MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_BULBASAUR, 50, MOVE_LEECH_SEED, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .wantSeed = WantSeededT0, .check = CheckBatonPassLeechSeed },
    { .name = "baton_pass_carries_perish_count",
      .player = { MON(SPECIES_MISDREAVUS, 50, MOVE_PERISH_SONG, MOVE_BATON_PASS, SPL, SPL), MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL), MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, 0), T(0, 0), T(0, 0) }, .turns = 4, .check = CheckBatonPassPerish },
    { .name = "baton_pass_carries_confusion",
      .player = { MON(SPECIES_SCYTHER, 50, SPL, MOVE_BATON_PASS, SPL, SPL), MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_GENGAR, 50, MOVE_CONFUSE_RAY, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .wantSeed = WantPassedToRattata, .check = CheckBatonPassConfusion },
    { .name = "baton_pass_drops_yawn",
      .player = { MON(SPECIES_SCYTHER, 50, SPL, MOVE_BATON_PASS, SPL, SPL), MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_YAWN, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .check = CheckBatonPassYawn },
    { .name = "baton_pass_carries_curse",
      .player = { MON(SPECIES_SCYTHER, 50, SPL, MOVE_BATON_PASS, SPL, SPL), MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_GENGAR, 50, MOVE_CURSE, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .check = CheckBatonPassCurse },
    { .name = "baton_pass_carries_stages_substitute_focus_energy",
      .player = { MON(SPECIES_SCYTHER, 50, MOVE_SWORDS_DANCE, MOVE_SUBSTITUTE, MOVE_FOCUS_ENERGY, MOVE_BATON_PASS), MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_SNORLAX, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, 0), T(2, 0), T(3, 0) }, .turns = 4, .check = CheckBatonPassStagesSubFocus },
    { .name = "lock_on_expires_after_two_turns",
      .player = { MON(SPECIES_HITMONLEE, 50, MOVE_LOCK_ON, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_SNORLAX, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckLockOnExpires },
    { .name = "lock_on_renewed_by_baton_pass",
      .player = { MON(SPECIES_HITMONLEE, 50, MOVE_LOCK_ON, MOVE_BATON_PASS, SPL, SPL), MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_SNORLAX, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckLockOnRenewedByBatonPass },
    { .name = "baton_pass_into_spikes",
      .player = { MON(SPECIES_SCYTHER, 50, SPL, MOVE_BATON_PASS, SPL, SPL), MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_FORRETRESS, 50, MOVE_SPIKES, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .check = CheckBatonPassSpikes },
    { .name = "baton_pass_triggers_natural_cure",
      .player = { { .species = SPECIES_CHANSEY, .level = 50, .moves = { MOVE_BATON_PASS, SPL, SPL, SPL }, .status = STATUS1_BURN }, MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_SNORLAX, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckBatonPassNaturalCure },
    { .name = "baton_pass_spikes_then_white_herb",
      .player = { MON(SPECIES_SCYTHER, 50, SPL, MOVE_BATON_PASS, SPL, SPL), MON_ITEM(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL, ITEM_WHITE_HERB) },
      .enemy = { MON(SPECIES_FORRETRESS, 50, MOVE_SPIKES, MOVE_GROWL, SPL, SPL) },
      .actions = { T(0, 0), T(0, 1), T(1, 2) }, .turns = 3, .check = CheckBatonPassWhiteHerb },

    // ---- Pursuit
    { .name = "pursuit_on_switch_doubled_and_first",
      .player = { { .species = SPECIES_BULBASAUR, .level = 50, .moves = { SPL, SPL, SPL, SPL }, .hp = 14, .hpSet = 1 }, MON(SPECIES_SQUIRTLE, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_PURSUIT, SPL, SPL, SPL) },
      .actions = { T(SC_SWITCH(1), 0) }, .turns = 1, .check = CheckPursuitSwitchKO },
    { .name = "pursuit_without_switch_normal_damage",
      .player = { { .species = SPECIES_BULBASAUR, .level = 50, .moves = { SPL, SPL, SPL, SPL }, .hp = 25, .hpSet = 1 }, MON(SPECIES_SQUIRTLE, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_PURSUIT, SPL, SPL, SPL) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantNoCrit, .check = CheckPursuitNormal },
    { .name = "pursuit_not_doubled_on_baton_pass",
      .player = { MON(SPECIES_SCYTHER, 50, MOVE_BATON_PASS, SPL, SPL, SPL), { .species = SPECIES_SQUIRTLE, .level = 50, .moves = { SPL, SPL, SPL, SPL }, .hp = 25, .hpSet = 1 } },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_PURSUIT, SPL, SPL, SPL) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantNoCrit, .check = CheckPursuitBatonPass },
    { .name = "pursuit_user_asleep_no_switch_hit",
      .player = { MON(SPECIES_BULBASAUR, 50, SPL, SPL, SPL, SPL), MON(SPECIES_SQUIRTLE, 50, SPL, SPL, SPL, SPL) },
      .enemy = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_PURSUIT, SPL, SPL, SPL }, .status = STATUS1_SLEEP_TURN(3) } },
      .actions = { T(SC_SWITCH(1), 0) }, .turns = 1, .check = CheckPursuitAsleep },

    { .name = "pursuit_user_frozen_no_switch_hit",
      .player = { MON(SPECIES_BULBASAUR, 50, SPL, SPL, SPL, SPL), MON(SPECIES_SQUIRTLE, 50, SPL, SPL, SPL, SPL) },
      .enemy = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_PURSUIT, SPL, SPL, SPL }, .status = STATUS1_FREEZE } },
      .actions = { T(SC_SWITCH(1), 0) }, .turns = 1, .check = CheckPursuitFrozen },
    { .name = "pursuit_doubles_both_foes_hit_switcher",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { { .species = SPECIES_BULBASAUR, .level = 50, .moves = { SPL, SPL, SPL, SPL }, .hp = 100, .hpSet = 1 }, MON(SPECIES_SNORLAX, 50, SPL, SPL, SPL, SPL), MON(SPECIES_SQUIRTLE, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_PURSUIT, SPL, SPL, SPL), MON(SPECIES_PIDGEY, 50, MOVE_PURSUIT, SPL, SPL, SPL) },
      .actions = { T4(SC_SWITCH(2), 0, 0, 0) }, .targets = { { 0, 1, 0, 1 } }, .turns = 1, .wantSeed = WantNoCrit, .check = CheckPursuitDoubles },

    // ---- Perish Song
    { .name = "perish_song_timing",
      .player = { MON(SPECIES_MISDREAVUS, 50, MOVE_PERISH_SONG, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, 0), T(1, 0) }, .turns = 3, .check = CheckPerishTiming },
    { .name = "perish_song_both_faint_draw",
      .player = { MON(SPECIES_MISDREAVUS, 50, MOVE_PERISH_SONG, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, 0), T(1, 0), T(1, 0), T(1, 0) }, .turns = 0, .check = CheckPerishDraw },
    { .name = "perish_song_soundproof_user_unaffected",
      .player = { MON(SPECIES_ELECTRODE, 50, MOVE_PERISH_SONG, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckPerishSoundproofUser },
    { .name = "perish_song_fails_all_soundproof",
      .player = { MON(SPECIES_ELECTRODE, 50, MOVE_PERISH_SONG, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_ELECTRODE, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckPerishAllSoundproof },
    { .name = "perish_song_cleared_by_switch",
      .player = { MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL), MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_MISDREAVUS, 50, MOVE_PERISH_SONG, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(SC_SWITCH(1), 1) }, .turns = 2, .check = CheckPerishSwitchClears },
    { .name = "perish_song_faints_replaced_same_turn",
      .player = { MON(SPECIES_MISDREAVUS, 50, MOVE_PERISH_SONG, SPL, SPL, SPL), MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL), MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, 0), T(1, 0), T(1, 0) }, .turns = 4, .check = CheckPerishReplacements },

    // ---- Teleport
    { .name = "teleport_fails_in_trainer_battle",
      .player = { MON(SPECIES_ABRA, 50, MOVE_TELEPORT, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckTeleport },

    // ---- what a switch resets / keeps
    { .name = "switch_resets_stages_keeps_burn",
      .player = { { .species = SPECIES_SNORLAX, .level = 50, .moves = { MOVE_SWORDS_DANCE, SPL, SPL, SPL }, .abilityNum = 1, .status = STATUS1_BURN }, MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_SNORLAX, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(SC_SWITCH(1), 0), T(SC_SWITCH(0), 0) }, .turns = 3, .check = CheckSwitchStagesBurn },
    { .name = "switch_keeps_sleep_counter",
      .player = { { .species = SPECIES_SNORLAX, .level = 50, .moves = { SPL, SPL, SPL, SPL }, .abilityNum = 1, .status = STATUS1_SLEEP_TURN(4) }, MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_SNORLAX, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(SC_SWITCH(1), 0), T(SC_SWITCH(0), 0) }, .turns = 3, .check = CheckSwitchSleepCounter },
    { .name = "switch_resets_toxic_counter",
      .player = { { .species = SPECIES_SNORLAX, .level = 50, .moves = { SPL, SPL, SPL, SPL }, .abilityNum = 1, .status = STATUS1_TOXIC_POISON }, MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_SNORLAX, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(0, 0), T(SC_SWITCH(1), 0), T(SC_SWITCH(0), 0) }, .turns = 4, .check = CheckSwitchToxicCounter },
    { .name = "switch_clears_confusion_and_yawn",
      .player = { MON_AB(SPECIES_SNORLAX, 50, SPL, SPL, SPL, SPL, 1), MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_GENGAR, 50, MOVE_CONFUSE_RAY, MOVE_YAWN, SPL, SPL) },
      .actions = { T(0, 0), T(0, 1), T(SC_SWITCH(1), 2), T(SC_SWITCH(0), 2) }, .turns = 4, .check = CheckSwitchClearsVolatiles },
    { .name = "switch_clears_encore",
      .player = { MON(SPECIES_CLEFABLE, 50, MOVE_ENCORE, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_GROWL, MOVE_TACKLE, SPL, SPL), MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, SC_SWITCH(1)), T(1, SC_SWITCH(0)), T(1, 1) }, .turns = 4, .check = CheckSwitchClearsEncore },
    { .name = "natural_cure_on_switch",
      .player = { { .species = SPECIES_CHANSEY, .level = 50, .moves = { SPL, SPL, SPL, SPL }, .status = STATUS1_BURN }, MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_SNORLAX, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(SC_SWITCH(1), 0) }, .turns = 1, .check = CheckNaturalCureSwitch },
    { .name = "switch_clears_foes_infatuation",
      .player = { MON(SPECIES_NIDOQUEEN, 50, MOVE_ATTRACT, SPL, SPL, SPL), MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_NIDOKING, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(SC_SWITCH(1), 0) }, .turns = 2, .check = CheckSwitchClearsInfatuation },
    { .name = "switch_in_spikes_before_intimidate",
      .player = { MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL), MON(SPECIES_ARCANINE, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_FORRETRESS, 50, MOVE_SPIKES, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(SC_SWITCH(1), 1) }, .turns = 2, .check = CheckSpikesBeforeIntimidate },

    // ---- faint replacement timing
    { .name = "faint_replacement_mid_turn_before_end_effects",
      .player = { { .species = SPECIES_RATTATA, .level = 50, .moves = { SPL, SPL, SPL, SPL }, .hp = 20, .hpSet = 1 }, MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_PERSIAN, 50, MOVE_SANDSTORM, MOVE_SLASH, SPL, SPL) },
      .actions = { T(0, 0), T(0, 1) }, .turns = 2, .check = CheckFaintReplacementMidTurn },
    { .name = "doubles_faint_replaced_mid_turn_others_still_act",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { { .species = SPECIES_RATTATA, .level = 50, .moves = { SPL, SPL, SPL, SPL }, .hp = 1, .hpSet = 1 }, MON(SPECIES_SNORLAX, 50, SPL, SPL, SPL, SPL), MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_PERSIAN, 50, MOVE_SLASH, SPL, SPL, SPL), MON(SPECIES_SNORLAX, 50, SPL, SPL, SPL, SPL) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { { 0, 1, 0, 0 } }, .turns = 1, .check = CheckDoublesFaintOthersAct },
    { .name = "explosion_both_replaced_player_first",
      .player = { MON(SPECIES_ELECTRODE, 50, MOVE_EXPLOSION, SPL, SPL, SPL), MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .enemy = { { .species = SPECIES_RATTATA, .level = 50, .moves = { SPL, SPL, SPL, SPL }, .hp = 1, .hpSet = 1 }, MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckExplosionBothReplaced },

    // ---- Leech Seed / Future Sight / Wish on replacements
    { .name = "leech_seed_heals_seeder_replacement",
      .player = { MON(SPECIES_BULBASAUR, 50, MOVE_LEECH_SEED, SPL, SPL, SPL), { .species = SPECIES_RATTATA, .level = 50, .moves = { SPL, SPL, SPL, SPL }, .hp = 20, .hpSet = 1 } },
      .enemy = { MON_AB(SPECIES_SNORLAX, 50, SPL, SPL, SPL, SPL, 1) },
      .actions = { T(0, 0), T(SC_SWITCH(1), 0) }, .turns = 2, .wantSeed = WantSeededT0, .check = CheckLeechSeedHealsReplacement },
    { .name = "leech_seed_drain_to_koed_seeders_replacement",
      .player = { { .species = SPECIES_BULBASAUR, .level = 50, .moves = { MOVE_LEECH_SEED, SPL, SPL, SPL }, .hp = 1, .hpSet = 1 }, { .species = SPECIES_RATTATA, .level = 50, .moves = { SPL, SPL, SPL, SPL }, .hp = 20, .hpSet = 1 } },
      .enemy = { MON_AB(SPECIES_SNORLAX, 50, MOVE_TACKLE, SPL, SPL, SPL, 1) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantSeededAny, .check = CheckLeechSeedSeederFainted },
    { .name = "seeded_mon_switch_clears_seed",
      .player = { MON(SPECIES_BULBASAUR, 50, MOVE_LEECH_SEED, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL), MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, SC_SWITCH(1)), T(1, SC_SWITCH(0)) }, .turns = 3, .wantSeed = WantSeededT0, .check = CheckSeededSwitchClears },
    { .name = "future_sight_not_after_two_turns",
      .player = { MON(SPECIES_ALAKAZAM, 50, MOVE_FUTURE_SIGHT, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_SNORLAX, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckFutureSightNotYet },
    { .name = "future_sight_lands_third_turn",
      .player = { MON(SPECIES_ALAKAZAM, 50, MOVE_FUTURE_SIGHT, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_SNORLAX, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, 0), T(1, 0) }, .turns = 3, .wantSeed = WantFutureHit, .check = CheckFutureSightLands },
    { .name = "future_sight_typeless_hits_dark",
      .player = { MON(SPECIES_ALAKAZAM, 50, MOVE_FUTURE_SIGHT, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_UMBREON, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, 0), T(1, 0) }, .turns = 3, .wantSeed = WantFutureHit, .check = CheckFutureSightDark },
    { .name = "future_sight_hits_target_replacement",
      .player = { MON(SPECIES_ALAKAZAM, 50, MOVE_FUTURE_SIGHT, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL), MON(SPECIES_PIDGEY, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, SC_SWITCH(1)), T(1, 0) }, .turns = 3, .wantSeed = WantFutureHit, .check = CheckFutureSightHitsReplacement },
    { .name = "future_sight_fails_while_pending",
      .player = { MON(SPECIES_ALAKAZAM, 50, MOVE_FUTURE_SIGHT, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_SNORLAX, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .check = CheckFutureSightPending },
    { .name = "future_sight_lands_after_user_switched",
      .player = { MON(SPECIES_ALAKAZAM, 50, MOVE_FUTURE_SIGHT, SPL, SPL, SPL), MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_SNORLAX, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(SC_SWITCH(1), 0), T(0, 0) }, .turns = 3, .wantSeed = WantFutureHit, .check = CheckFutureSightUserSwitched },
    { .name = "wish_heals_replacement_half_of_its_max",
      .player = { MON(SPECIES_CLEFABLE, 50, MOVE_WISH, SPL, SPL, SPL), { .species = SPECIES_RATTATA, .level = 50, .moves = { SPL, SPL, SPL, SPL }, .hp = 20, .hpSet = 1 } },
      .enemy = { MON(SPECIES_SNORLAX, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(SC_SWITCH(1), 0) }, .turns = 2, .check = CheckWishReplacementExact },
    { .name = "wish_fails_while_pending",
      .player = { { .species = SPECIES_CLEFABLE, .level = 50, .moves = { MOVE_WISH, SPL, SPL, SPL }, .hp = 50, .hpSet = 1 } },
      .enemy = { MON(SPECIES_SNORLAX, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .check = CheckWishPending },
    { .name = "wish_heals_before_poison_damage",
      .player = { { .species = SPECIES_CLEFABLE, .level = 50, .moves = { MOVE_WISH, SPL, SPL, SPL }, .hp = 31, .hpSet = 1, .status = STATUS1_POISON } },
      .enemy = { MON(SPECIES_SNORLAX, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckWishBeforePoison },
    { .name = "wish_at_full_hp",
      .player = { MON(SPECIES_CLEFABLE, 50, MOVE_WISH, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_SNORLAX, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckWishFullHp },
};

SCENARIO_GROUP(trap_switch, sScenarios)
