// Switch-in, field and turn-based abilities: Intimidate, Trace, weather abilities, Forecast, Cloud Nine /
// Air Lock, Pressure, Speed Boost, Truant, Rain Dish, Shed Skin, Natural Cure, Levitate, Sturdy, Wonder
// Guard, Damp, Insomnia / Vital Spirit, Soundproof, Oblivious, Own Tempo, Keen Eye / Hyper Cutter / Clear
// Body / White Smoke, Suction Cups, Inner Focus, status immunities, Swift Swim / Chlorophyll.
//
// Gen 3 quirks reproduced here (see sim/src/battle_main.c TryDoEventsBeforeFirstTurn, battle_util.c
// AbilityBattleEffects / HandleFaintedMonActions):
//  - Intimidate and Trace of a mon switched in mid-turn resolve right after the switch action
//    (HandleAction_TryFinish -> HandleFaintedMonActions case 6 runs after every action), so they are
//    already in effect when the foe attacks; the same pass handles faint replacements mid-turn.
//  - When both leads have Intimidate, battler 0 (the player's) fires first regardless of speed
//    (ABILITYEFFECT_INTIMIDATE1 scans battlers by index).
//  - Weather abilities of both leads fire fastest-first, so the slower lead's weather wins.
//  - A weather move replaces permanent weather outright (gBattleWeather is assigned, not OR'ed).
//  - Trace copies Wonder Guard; Skill Swap / Role Play cannot.
//  - Levitate is only checked in typecalc, so Sand Attack (no typecalc) still lands on Levitate mons.
//  - Pressure: Perish Song / Imprison lose extra PP per Pressure mon; Magic Coat bouncing a Pressure mon's
//    move costs the coat user an extra Magic Coat PP; Spikes that fail at 3 layers cost only 1 PP.
//  - A Truant mon switched in (or sent in to replace a mon KO'd by a move) acts on its first turn, because
//    the end-of-turn ability pass toggles its counter; one sent in after an end-of-turn faint (poison)
//    comes in after that pass and loafs on its first turn.
//    The counter also toggles on turns the mon sleeps through, and the wake-up script returns into the
//    move, so a Truant mon can loaf on the very turn it wakes up.
#include "scenario.h"

// First log entry with this string in `turn` (any battler), or -1.
static int LogIndexAny(struct BattleSim *sim, u16 id, int turn)
{
    int i;
    for (i = 0; i < sim->logCount; i++)
        if (sim->log[i].stringId == id && (turn == SC_ANY_TURN || sim->log[i].turn == turn))
            return i;
    return -1;
}

// hpTarget (gBattleMons[gBattlerTarget].hp at print time) of the nth log entry with this string, or -1.
static int LogHpTargetNth(struct BattleSim *sim, u16 id, int nth)
{
    int i;
    for (i = 0; i < sim->logCount; i++)
        if (sim->log[i].stringId == id && nth-- == 0)
            return sim->log[i].hpTarget;
    return -1;
}

// ---------------------------------------------------------------- Intimidate

static void CheckIntimidateStart(struct BattleSim *sim)
{
    CHECK(STAGE(0, STAT_ATK) == 5, "player attack cut to -1 (stage %d)", STAGE(0, STAT_ATK));
    CHECK(STAGE(1, STAT_ATK) == 6, "gyarados itself untouched");
    CHECK(LOG_HAS_T(STRINGID_PKMNCUTSATTACKWITH, 0), "intimidate message at battle start");
}

static void CheckIntimidateBothLeads(struct BattleSim *sim)
{
    // Enemy Gyarados (level 60) is faster, but INTIMIDATE1 scans battlers by index: the player's fires first.
    CHECK(STAGE(0, STAT_ATK) == 5 && STAGE(1, STAT_ATK) == 5, "both leads at -1 (%d/%d)", STAGE(0, STAT_ATK), STAGE(1, STAT_ATK));
    CHECK(LOG_COUNT(STRINGID_PKMNCUTSATTACKWITH) == 2, "two activations");
    CHECK(MAXHP(0) != MAXHP(1), "distinguishable max hp");
    CHECK(LogHpTargetNth(sim, STRINGID_PKMNCUTSATTACKWITH, 0) == MAXHP(1), "player's intimidate (targeting the enemy) fired first");
    CHECK(LogHpTargetNth(sim, STRINGID_PKMNCUTSATTACKWITH, 1) == MAXHP(0), "enemy's intimidate fired second");
}

static void CheckIntimidateSwitchInImmediate(struct BattleSim *sim)
{
    int intim = LogIndexAny(sim, STRINGID_PKMNCUTSATTACKWITH, 0);
    int foeMove = LOG_INDEX_T(STRINGID_USEDMOVE, 1, 0);
    CHECK(B(0).species == SPECIES_GYARADOS, "gyarados in");
    CHECK(STAGE(1, STAT_ATK) == 5, "foe attack -1 (stage %d)", STAGE(1, STAT_ATK));
    CHECK(intim >= 0 && foeMove >= 0 && intim < foeMove, "intimidate resolved right after the switch, before the foe's move (idx %d vs %d)", intim, foeMove);
}

static void CheckIntimidateBlocked(struct BattleSim *sim)
{
    CHECK(STAGE(0, STAT_ATK) == 6, "attack not lowered (stage %d)", STAGE(0, STAT_ATK));
    CHECK(LOG_HAS(STRINGID_PREVENTEDFROMWORKING), "'prevented from working' message");
    CHECK(!LOG_HAS(STRINGID_PKMNCUTSATTACKWITH), "no cut-attack message");
}

static void CheckIntimidateVsSubstitute(struct BattleSim *sim)
{
    CHECK(STATUS2(1) & STATUS2_SUBSTITUTE, "substitute up");
    CHECK(STAGE(1, STAT_ATK) == 6, "attack unchanged behind substitute (stage %d)", STAGE(1, STAT_ATK));
    CHECK(!LOG_HAS(STRINGID_PKMNCUTSATTACKWITH) && !LOG_HAS(STRINGID_PREVENTEDFROMWORKING), "silent failure");
}

static void CheckIntimidateDoubles(struct BattleSim *sim)
{
    CHECK(STAGE(1, STAT_ATK) == 5, "snorlax -1 (stage %d)", STAGE(1, STAT_ATK));
    CHECK(STAGE(3, STAT_ATK) == 6, "metang (clear body) unaffected (stage %d)", STAGE(3, STAT_ATK));
    CHECK(STAGE(2, STAT_ATK) == 6, "partner unaffected");
    CHECK(LOG_COUNT(STRINGID_PKMNCUTSATTACKWITH) == 1 && LOG_HAS(STRINGID_PREVENTEDFROMWORKING), "one cut, one blocked");
}

static void CheckIntimidateReentry(struct BattleSim *sim)
{
    CHECK(STAGE(1, STAT_ATK) == 4, "two entries stack to -2 (stage %d)", STAGE(1, STAT_ATK));
    CHECK(LOG_COUNT(STRINGID_PKMNCUTSATTACKWITH) == 2, "two activations");
}

static void CheckIntimidateVsMist(struct BattleSim *sim)
{
    CHECK(STAGE(1, STAT_ATK) == 6, "mist blocked intimidate (stage %d)", STAGE(1, STAT_ATK));
    CHECK(LOG_HAS(STRINGID_PKMNPROTECTEDBYMIST), "mist message");
}

static void CheckIntimidateFaintReplacement(struct BattleSim *sim)
{
    CHECK(B(1).species == SPECIES_GYARADOS, "gyarados replaced the fainted rattata");
    CHECK(STAGE(0, STAT_ATK) == 5, "intimidate fired on the end-of-turn replacement (stage %d)", STAGE(0, STAT_ATK));
    CHECK(LOG_HAS_T(STRINGID_PKMNCUTSATTACKWITH, 0), "still turn 0");
}

// ---------------------------------------------------------------- Trace

static void CheckTraceIntimidate(struct BattleSim *sim)
{
    CHECK(B(0).ability == ABILITY_INTIMIDATE, "traced intimidate (ability %d)", B(0).ability);
    CHECK(STAGE(0, STAT_ATK) == 5, "arcanine's intimidate hit gardevoir first");
    CHECK(STAGE(1, STAT_ATK) == 6, "the traced intimidate does not activate");
    CHECK(LOG_HAS(STRINGID_PKMNTRACED), "trace message");
    CHECK(LogIndexAny(sim, STRINGID_PKMNCUTSATTACKWITH, 0) < LogIndexAny(sim, STRINGID_PKMNTRACED, 0), "intimidate resolves before trace");
}

static void CheckTraceWonderGuard(struct BattleSim *sim)
{
    CHECK(B(0).ability == ABILITY_WONDER_GUARD, "trace copied wonder guard (ability %d)", B(0).ability);
    CHECK(HP(0) == MAXHP(0), "tackle did no damage");
    CHECK(LOG_HAS(STRINGID_AVOIDEDDAMAGE), "wonder guard message");
}

static void CheckTraceSwitchIn(struct BattleSim *sim)
{
    int traced = LogIndexAny(sim, STRINGID_PKMNTRACED, 0);
    int foeMove = LOG_INDEX_T(STRINGID_USEDMOVE, 1, 0);
    CHECK(B(0).ability == ABILITY_INTIMIDATE, "porygon traced intimidate");
    CHECK(traced >= 0 && foeMove >= 0 && traced < foeMove, "trace resolved right after the switch, before the foe's move (idx %d vs %d)", traced, foeMove);
}

static int WantTraceImmunity(struct BattleSim *sim) { return B(0).ability == ABILITY_IMMUNITY; }
static void CheckTraceDoubles(struct BattleSim *sim)
{
    CHECK(B(0).ability == ABILITY_IMMUNITY, "traced the randomly chosen foe (snorlax)");
    CHECK(B(1).ability == ABILITY_INTIMIDATE && B(3).ability == ABILITY_IMMUNITY, "foes keep their abilities");
}

static void CheckTraceVsTrace(struct BattleSim *sim)
{
    CHECK(B(0).ability == ABILITY_TRACE && B(1).ability == ABILITY_TRACE, "both still trace");
    CHECK(LOG_COUNT(STRINGID_PKMNTRACED) == 2, "both traced each other's trace");
}

static void CheckTraceForecast(struct BattleSim *sim)
{
    CHECK(B(0).ability == ABILITY_FORECAST, "traced forecast");
    CHECK(WEATHER() & B_WEATHER_RAIN, "rain up");
    CHECK(B(1).type1 == TYPE_WATER, "castform became water");
    CHECK(B(0).type1 == TYPE_PSYCHIC && B(0).type2 == TYPE_PSYCHIC, "gardevoir is not castform: no type change");
}

// ---------------------------------------------------------------- Weather abilities

static void CheckDrizzlePermanent(struct BattleSim *sim)
{
    CHECK(WEATHER() == (B_WEATHER_RAIN_TEMPORARY | B_WEATHER_RAIN_PERMANENT), "permanent rain flags (weather %#x)", WEATHER());
    CHECK(LOG_HAS_T(STRINGID_PKMNMADEITRAIN, 0), "drizzle message");
    CHECK(LOG_COUNT(STRINGID_RAINCONTINUES) == 6, "rain continues every turn (%d)", LOG_COUNT(STRINGID_RAINCONTINUES));
    CHECK(!LOG_HAS(STRINGID_RAINSTOPPED), "never stops");
}

static void CheckDroughtOverridesDrizzle(struct BattleSim *sim)
{
    CHECK(WEATHER() == B_WEATHER_SUN, "slower groudon's sun wins (weather %#x)", WEATHER());
    CHECK(LogIndexAny(sim, STRINGID_PKMNMADEITRAIN, 0) < LogIndexAny(sim, STRINGID_PKMNSXINTENSIFIEDSUN, 0), "rain first, then sun");
}

static void CheckDrizzleOverridesDrought(struct BattleSim *sim)
{
    CHECK(WEATHER() == (B_WEATHER_RAIN_TEMPORARY | B_WEATHER_RAIN_PERMANENT), "slower kyogre's rain wins (weather %#x)", WEATHER());
    CHECK(LogIndexAny(sim, STRINGID_PKMNSXINTENSIFIEDSUN, 0) < LogIndexAny(sim, STRINGID_PKMNMADEITRAIN, 0), "sun first, then rain");
}

static void CheckSandStreamOverridesRain(struct BattleSim *sim)
{
    CHECK(WEATHER() == B_WEATHER_SANDSTORM, "permanent sandstorm replaced rain dance (weather %#x)", WEATHER());
    CHECK(LOG_HAS_T(STRINGID_PKMNSXWHIPPEDUPSANDSTORM, 1), "sand stream message on switch-in");
    CHECK(HP(0) == MAXHP(0) - 3 * (MAXHP(0) / 16), "rattata buffeted 3 times (hp %d/%d)", HP(0), MAXHP(0));
    CHECK(HP(1) == MAXHP(1), "rock type tyranitar immune");
}

static void CheckSunnyDayReplacesDrizzle(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_SUNLIGHTGOTBRIGHT, 0), "sunny day succeeded under drizzle rain");
    CHECK(LOG_HAS_T(STRINGID_SUNLIGHTFADED, 4), "sun faded after 5 turns");
    CHECK(WEATHER() == 0, "no weather at all afterwards: the permanent rain was overwritten (weather %#x)", WEATHER());
}

static void CheckRainDanceFailsUnderDrizzle(struct BattleSim *sim)
{
    CHECK(LOG_HAS_MOVE(STRINGID_BUTITFAILED, MOVE_RAIN_DANCE), "rain dance failed");
    CHECK(WEATHER() == (B_WEATHER_RAIN_TEMPORARY | B_WEATHER_RAIN_PERMANENT), "drizzle rain intact");
    CHECK(PP(0, 0) == 4, "pp still spent");
}

static void CheckDrizzleReentry(struct BattleSim *sim)
{
    CHECK(LOG_COUNT(STRINGID_PKMNMADEITRAIN) == 2, "drizzle fired again over temporary rain (%d)", LOG_COUNT(STRINGID_PKMNMADEITRAIN));
    CHECK(WEATHER() == (B_WEATHER_RAIN_TEMPORARY | B_WEATHER_RAIN_PERMANENT), "rain is permanent again (weather %#x)", WEATHER());
}

static void CheckDrizzleNoReactivation(struct BattleSim *sim)
{
    CHECK(B(1).species == SPECIES_KYOGRE, "second kyogre in");
    CHECK(LOG_COUNT(STRINGID_PKMNMADEITRAIN) == 1, "no second activation while permanent rain is up (%d)", LOG_COUNT(STRINGID_PKMNMADEITRAIN));
}

// ---------------------------------------------------------------- Forecast / Castform

static void CheckCastformRain(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_CASTFORM, "species unchanged (form is cosmetic)");
    CHECK(B(0).type1 == TYPE_WATER && B(0).type2 == TYPE_WATER, "castform became water (types %d/%d)", B(0).type1, B(0).type2);
    CHECK(LOG_HAS_T(STRINGID_PKMNTRANSFORMED, 0), "transformed message");
}

static void CheckCastformSun(struct BattleSim *sim)
{
    CHECK(B(0).type1 == TYPE_FIRE && B(0).type2 == TYPE_FIRE, "castform is fire under its own sunny day");
    CHECK(LOG_COUNT(STRINGID_PKMNTRANSFORMED) == 1, "one change");
}

static void CheckCastformSunFades(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_SUNLIGHTFADED, 4), "sun faded at the end of turn 5");
    CHECK(WEATHER() == 0, "no weather");
    CHECK(B(0).type1 == TYPE_NORMAL && B(0).type2 == TYPE_NORMAL, "back to normal (types %d/%d)", B(0).type1, B(0).type2);
    CHECK(LOG_COUNT(STRINGID_PKMNTRANSFORMED) == 2 && LOG_HAS_T(STRINGID_PKMNTRANSFORMED, 4), "reverted in the same end-of-turn");
}

static void CheckCastformSandstorm(struct BattleSim *sim)
{
    CHECK(B(0).type1 == TYPE_NORMAL, "no sandstorm form");
    CHECK(!LOG_HAS(STRINGID_PKMNTRANSFORMED), "no transform message");
    CHECK(HP(0) == MAXHP(0) - 2 * (MAXHP(0) / 16), "normal castform takes sandstorm damage (hp %d/%d)", HP(0), MAXHP(0));
}

static void CheckCastformHail(struct BattleSim *sim)
{
    CHECK(B(0).type1 == TYPE_ICE && B(0).type2 == TYPE_ICE, "ice form under hail");
    CHECK(HP(0) == MAXHP(0), "ice type takes no hail damage");
    CHECK(HP(1) == MAXHP(1) - 2 * (MAXHP(1) / 16), "snorlax pelted twice (hp %d/%d)", HP(1), MAXHP(1));
}

static void CheckCloudNineRevertsCastform(struct BattleSim *sim)
{
    CHECK(B(1).species == SPECIES_GOLDUCK && B(1).ability == ABILITY_CLOUD_NINE, "golduck with cloud nine in");
    CHECK(WEATHER() & B_WEATHER_RAIN, "rain still technically up");
    CHECK(B(0).type1 == TYPE_NORMAL, "castform reverted on cloud nine's entry (type %d)", B(0).type1);
    CHECK(LOG_COUNT(STRINGID_PKMNTRANSFORMED) == 2, "water, then back to normal");
}

static void CheckCloudNineBlocksCastform(struct BattleSim *sim)
{
    CHECK(WEATHER() & B_WEATHER_RAIN, "rain set");
    CHECK(B(0).type1 == TYPE_NORMAL && !LOG_HAS(STRINGID_PKMNTRANSFORMED), "no form change while cloud nine is out");
}

static void CheckCastformSwitchInRain(struct BattleSim *sim)
{
    // ABILITYEFFECT_ON_SWITCHIN case ABILITY_FORECAST: the form is set as part of the switch-in effects.
    int transformed = LogIndexAny(sim, STRINGID_PKMNTRANSFORMED, 0);
    int foeMove = LOG_INDEX_T(STRINGID_USEDMOVE, 1, 0);
    CHECK(B(0).species == SPECIES_CASTFORM, "castform in");
    CHECK(B(0).type1 == TYPE_WATER && B(0).type2 == TYPE_WATER, "water form right after switching into rain (types %d/%d)", B(0).type1, B(0).type2);
    CHECK(LOG_COUNT(STRINGID_PKMNTRANSFORMED) == 1, "one transform message (%d)", LOG_COUNT(STRINGID_PKMNTRANSFORMED));
    CHECK(transformed >= 0 && foeMove >= 0 && transformed < foeMove, "form changed on the switch, before the foe's move (idx %d vs %d)", transformed, foeMove);
}

// ---------------------------------------------------------------- Cloud Nine / Air Lock

static void CheckCloudNineSandstorm(struct BattleSim *sim)
{
    CHECK(WEATHER() == B_WEATHER_SANDSTORM, "sandstorm set by sand stream");
    CHECK(HP(0) == MAXHP(0), "golduck takes no sandstorm damage under cloud nine (hp %d/%d)", HP(0), MAXHP(0));
    CHECK(!LOG_HAS(STRINGID_PKMNBUFFETEDBYSANDSTORM), "no buffet message");
}

static void CheckAirLockSolarBeam(struct BattleSim *sim)
{
    CHECK(WEATHER() == B_WEATHER_SUN, "drought sun set");
    CHECK(LOG_HAS(STRINGID_PKMNTOOKSUNLIGHT), "solar beam had to charge under air lock");
    CHECK(HP(1) == MAXHP(1), "no damage on the charge turn");
}

static void CheckSwiftSwimSpeed(struct BattleSim *sim)
{
    CHECK(MOVED_BEFORE(1, 0, 0), "psyduck faster without rain");
    CHECK(MOVED_BEFORE(0, 1, 1), "omanyte doubled by swift swim in rain");
}

static void CheckCloudNineSwiftSwim(struct BattleSim *sim)
{
    CHECK(WEATHER() & B_WEATHER_RAIN, "rain up");
    CHECK(MOVED_BEFORE(1, 0, 1), "no swift swim boost under cloud nine");
}

static void CheckChlorophyllSpeed(struct BattleSim *sim)
{
    CHECK(MOVED_BEFORE(1, 0, 0), "psyduck faster without sun");
    CHECK(MOVED_BEFORE(0, 1, 1), "oddish doubled by chlorophyll in sun");
}

// ---------------------------------------------------------------- Pressure

static void CheckPressureSingles(struct BattleSim *sim)
{
    CHECK(PP(0, 0) == 33, "tackle vs pressure: 2 pp (got %d)", PP(0, 0));
    CHECK(PP(0, 1) == 29, "swords dance (self-target): 1 pp (got %d)", PP(0, 1));
    CHECK(PP(0, 2) == 38, "growl (target both): counts the pressure foe (got %d)", PP(0, 2));
}

static void CheckPressureDoubles(struct BattleSim *sim)
{
    CHECK(PP(0, 0) == 6, "earthquake: 1 + partner + 2 foes with pressure (got %d)", PP(0, 0));
    CHECK(PP(0, 1) == 37, "growl: 1 + 2 pressure foes (got %d)", PP(0, 1));
}

static void CheckPressurePerishSong(struct BattleSim *sim)
{
    CHECK(PP(0, 0) == 1, "perish song: 1 + every other pressure mon (3) (got %d)", PP(0, 0));
    CHECK(STATUS3(0) & STATUS3_PERISH_SONG, "song took effect");
}

static void CheckPressureImprison(struct BattleSim *sim)
{
    CHECK(STATUS3(0) & STATUS3_IMPRISONED_OTHERS, "imprison succeeded");
    CHECK(PP(0, 0) == 8, "imprison: 1 + 1 per pressure foe (got %d)", PP(0, 0));
}

static void CheckPressureSpikes(struct BattleSim *sim)
{
    CHECK(sim->sideTimers[B_SIDE_OPPONENT].spikesAmount == 3, "three layers");
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 3), "4th spikes failed");
    CHECK(PP(0, 0) == 13, "3 x 2 pp, failed one costs 1 (got %d)", PP(0, 0));
}

static void CheckPressureMagicCoat(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNMOVEBOUNCED), "toxic bounced");
    CHECK(PP(0, 0) == 13, "magic coat lost an extra pp to the bounced pressure mon (got %d)", PP(0, 0));
}

// ---------------------------------------------------------------- Speed Boost / Truant

static void CheckSpeedBoostCap(struct BattleSim *sim)
{
    CHECK(STAGE(0, STAT_SPEED) == 12, "speed maxed (stage %d)", STAGE(0, STAT_SPEED));
    CHECK(LOG_COUNT(STRINGID_PKMNRAISEDSPEED) == 2, "boosted on turns 1 and 2 only (%d)", LOG_COUNT(STRINGID_PKMNRAISEDSPEED));
    CHECK(!LOG_HAS_T(STRINGID_PKMNRAISEDSPEED, 2), "no boost at +6");
}

static void CheckSpeedBoostSwitchIn(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_NINJASK, "ninjask in");
    CHECK(!LOG_HAS_T(STRINGID_PKMNRAISEDSPEED, 0), "no boost on the switch-in turn");
    CHECK(LOG_HAS_T(STRINGID_PKMNRAISEDSPEED, 1), "boost on the next turn");
    CHECK(STAGE(0, STAT_SPEED) == 7, "speed +1 (stage %d)", STAGE(0, STAT_SPEED));
}

static void CheckTruantAlternates(struct BattleSim *sim)
{
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_TACKLE, 0), "acts on turn 1");
    CHECK(LOG_HAS_T(STRINGID_PKMNLOAFING, 1) && LOG_INDEX_T(STRINGID_USEDMOVE, 0, 1) < 0, "loafs on turn 2");
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_TACKLE, 2), "acts on turn 3");
    CHECK(LOG_HAS_T(STRINGID_PKMNLOAFING, 3), "loafs on turn 4");
    CHECK(PP(0, 0) == 33, "no pp spent while loafing (got %d)", PP(0, 0));
}

static void CheckTruantCancelsThrash(struct BattleSim *sim)
{
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_THRASH, 0), "thrash started");
    CHECK(LOG_HAS_T(STRINGID_PKMNLOAFING, 1), "loafed during the rampage");
    CHECK(!(STATUS2(0) & STATUS2_CONFUSION), "rampage cancelled: no fatigue confusion");
    CHECK(!LOG_HAS(STRINGID_PKMNFATIGUECONFUSION), "no fatigue message");
}

static void CheckTruantSwitchIn(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_SLAKING, "slaking back in");
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_TACKLE, 3) && !LOG_HAS_T(STRINGID_PKMNLOAFING, 3), "acts on its first turn after a mid-turn switch-in");
    CHECK(LOG_HAS_T(STRINGID_PKMNLOAFING, 4), "loafs the turn after");
}

static void CheckTruantReplacementAfterKO(struct BattleSim *sim)
{
    // Replacement sent in mid-turn (right after the KO): the end-of-turn ability pass toggles Truant.
    CHECK(B(0).species == SPECIES_SLAKING && PARTY_HP(B_SIDE_PLAYER, 0) == 0, "slaking replaced the fainted rattata");
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_TACKLE, 1) && !LOG_HAS_T(STRINGID_PKMNLOAFING, 1), "acts on its first turn");
    CHECK(LOG_HAS_T(STRINGID_PKMNLOAFING, 2), "loafs on the turn after");
}

static void CheckTruantAsleep(struct BattleSim *sim)
{
    // The end-of-turn ability pass toggles the counter even on turns the mon slept through, and the
    // waking-up script returns into the move (BattleScriptPushCursor), so CANCELLER_TRUANT still runs.
    CHECK(LOG_HAS_T(STRINGID_PKMNFASTASLEEP, 0) && LOG_INDEX_T(STRINGID_USEDMOVE, 0, 0) < 0, "slept through turn 1");
    CHECK(LOG_HAS_T(STRINGID_PKMNWOKEUP, 1), "woke up on turn 2");
    CHECK(LOG_HAS_T(STRINGID_PKMNLOAFING, 1) && LOG_INDEX_T(STRINGID_USEDMOVE, 0, 1) < 0, "loafed right after waking (counter toggled while asleep)");
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_TACKLE, 2), "acts on turn 3");
    CHECK(STATUS1(0) == 0, "awake");
}

static void CheckTruantReplacementAfterPoisonFaint(struct BattleSim *sim)
{
    // Rattata faints from poison at the end of turn 1, after the ability pass: Slaking's counter stays set.
    CHECK(B(0).species == SPECIES_SLAKING && PARTY_HP(B_SIDE_PLAYER, 0) == 0, "slaking replaced the poisoned rattata");
    CHECK(LOG_HAS_T(STRINGID_PKMNLOAFING, 1) && LOG_INDEX_T(STRINGID_USEDMOVE, 0, 1) < 0, "loafs on its first turn");
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_TACKLE, 2), "acts on the turn after");
}

// ---------------------------------------------------------------- Rain Dish / Shed Skin / Natural Cure

static void CheckRainDish(struct BattleSim *sim)
{
    CHECK(HP(0) == 100 + MAXHP(0) / 16, "healed 1/16 in rain (hp %d, max %d)", HP(0), MAXHP(0));
    CHECK(LOG_HAS(STRINGID_PKMNSXRESTOREDHPALITTLE2), "rain dish message");
}

static void CheckRainDishCloudNine(struct BattleSim *sim)
{
    CHECK(WEATHER() & B_WEATHER_RAIN, "rain set");
    CHECK(HP(0) == 100 && !LOG_HAS(STRINGID_PKMNSXRESTOREDHPALITTLE2), "no heal under cloud nine (hp %d)", HP(0));
}

static int WantShedSkinCured(struct BattleSim *sim) { return STATUS1(0) == 0; }
static int WantShedSkinNotCured(struct BattleSim *sim) { return STATUS1(0) != 0; }
static void CheckShedSkinCured(struct BattleSim *sim)
{
    CHECK(STATUS1(0) == 0, "poison shed");
    CHECK(LOG_HAS(STRINGID_PKMNSXCUREDYPROBLEM), "shed skin message");
    CHECK(HP(0) == MAXHP(0), "shed skin runs before poison damage");
}
static void CheckShedSkinNotCured(struct BattleSim *sim)
{
    CHECK(STATUS1(0) & STATUS1_POISON, "still poisoned this turn");
    CHECK(!LOG_HAS(STRINGID_PKMNSXCUREDYPROBLEM), "no message");
    CHECK(HP(0) == MAXHP(0) - MAXHP(0) / 8, "took poison damage");
}

static void CheckNaturalCureSwitch(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_RATTATA, "switched");
    CHECK(PARTY_STATUS(B_SIDE_PLAYER, 0) == 0, "chansey's burn cured on switch-out");
}

static void CheckNaturalCureRoar(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_RATTATA, "roared out");
    CHECK(PARTY_STATUS(B_SIDE_PLAYER, 0) == 0, "chansey's burn cured when forced out");
}

// ---------------------------------------------------------------- Levitate

static void CheckLevitateGround(struct BattleSim *sim)
{
    CHECK(HP(0) == MAXHP(0), "no damage from earthquake / fissure / magnitude");
    CHECK(LOG_COUNT(STRINGID_PKMNMAKESGROUNDMISS) == 3, "levitate message each time (%d)", LOG_COUNT(STRINGID_PKMNMAKESGROUNDMISS));
}

static void CheckLevitateSandAttack(struct BattleSim *sim)
{
    CHECK(STAGE(0, STAT_ACC) == 5, "sand attack (no typecalc) still lands on levitate (stage %d)", STAGE(0, STAT_ACC));
    CHECK(!LOG_HAS(STRINGID_PKMNMAKESGROUNDMISS), "no levitate message");
}

static void CheckLevitateSpikes(struct BattleSim *sim)
{
    CHECK(SIDE_STATUS(B_SIDE_PLAYER) & SIDE_STATUS_SPIKES, "spikes on player side");
    CHECK(B(0).species == SPECIES_GENGAR && HP(0) == MAXHP(0), "gengar took no spikes damage");
    CHECK(!LOG_HAS(STRINGID_PKMNHURTBYSPIKES), "no spikes message");
}

static void CheckLevitateArenaTrap(struct BattleSim *sim)
{
    CHECK(B(1).ability == ABILITY_ARENA_TRAP, "dugtrio has arena trap");
    CHECK(LOG_HAS_T(STRINGID_SWITCHINMON, 0), "gengar (levitate) switched out on turn 1");
    CHECK(B(0).species == SPECIES_RATTATA, "rattata is trapped (still out)");
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_SPLASH, 1), "rattata's switch was refused and it fell back to a move");
}

static void CheckLevitateRoar(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_RATTATA, "levitate does not stop roar");
}

// ---------------------------------------------------------------- Sturdy

static void CheckSturdy(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNPROTECTEDBY, 0), "sturdy blocked horn drill");
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_HORN_ATTACK, 1), "horn attack used on turn 2");
    CHECK(HP(1) == 0, "sturdy does not save a 1 hp golem from a normal hit");
}

// ---------------------------------------------------------------- Wonder Guard

static void CheckWonderGuardTypes(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_AVOIDEDDAMAGE, 0), "scratch avoided");
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_EMBER, 1), "ember used");
    CHECK(HP(1) == 0, "super effective ember hit");
}

static int WantShedinjaDown(struct BattleSim *sim) { return HP(1) == 0; }
static void CheckWonderGuardLeechSeed(struct BattleSim *sim)
{
    CHECK(HP(1) == 0, "leech seed drain (min 1) took shedinja down");
}

static void CheckWonderGuardSandstorm(struct BattleSim *sim)
{
    CHECK(HP(1) == 0, "sandstorm chip took shedinja down");
    CHECK(LOG_HAS(STRINGID_PKMNBUFFETEDBYSANDSTORM), "buffet message");
}

static void CheckWonderGuardStruggle(struct BattleSim *sim)
{
    CHECK(LOG_HAS_MOVE(STRINGID_USEDMOVE, MOVE_STRUGGLE), "struggled");
    CHECK(HP(1) == 0, "struggle skips typecalc and hits through wonder guard");
}

static int WantShedinjaToxic(struct BattleSim *sim) { return LOG_HAS(STRINGID_PKMNBADLYPOISONED); }
static void CheckWonderGuardStatusMove(struct BattleSim *sim)
{
    // Wonder Guard is only consulted in typecalc for moves with power: Toxic lands, and its end-of-turn
    // damage (min 1) takes the 1 hp shedinja down.
    CHECK(LOG_HAS(STRINGID_PKMNBADLYPOISONED), "toxic landed on shedinja");
    CHECK(!LOG_HAS(STRINGID_AVOIDEDDAMAGE), "no wonder guard message");
    CHECK(LOG_HAS(STRINGID_PKMNHURTBYPOISON) && HP(1) == 0, "poison damage took it down (hp %d)", HP(1));
}

static void CheckWonderGuardAbilityMoves(struct BattleSim *sim)
{
    CHECK(LOG_HAS_MOVE_T(STRINGID_BUTITFAILED, MOVE_SKILL_SWAP, 0), "skill swap fails");
    CHECK(LOG_HAS_MOVE_T(STRINGID_BUTITFAILED, MOVE_ROLE_PLAY, 1), "role play fails");
    CHECK(B(0).ability == ABILITY_SYNCHRONIZE && B(1).ability == ABILITY_WONDER_GUARD, "abilities unchanged");
}

// ---------------------------------------------------------------- Damp

static void CheckDampSelf(struct BattleSim *sim)
{
    CHECK(HP(0) == MAXHP(0) && HP(1) == MAXHP(1), "the damp user's own explosion fails");
    CHECK(LOG_HAS(STRINGID_PKMNPREVENTSUSAGE), "damp message");
    CHECK(PP(0, 0) == 4, "pp spent");
}

static void CheckDampFoe(struct BattleSim *sim)
{
    // tryexplosion scans every battler for Damp, so the target's ability stops the foe's explosion.
    CHECK(HP(0) == MAXHP(0) && HP(1) == MAXHP(1), "electrode's explosion stopped by poliwrath's damp (hp %d/%d)", HP(1), MAXHP(1));
    CHECK(LOG_HAS(STRINGID_PKMNPREVENTSUSAGE), "damp message");
    CHECK(PP(1, 0) == 4, "pp spent (got %d)", PP(1, 0));
    CHECK(OUTCOME() == 0, "battle continues");
}

static void CheckDampPartner(struct BattleSim *sim)
{
    CHECK(HP(0) == MAXHP(0) && HP(1) == MAXHP(1) && HP(2) == MAXHP(2) && HP(3) == MAXHP(3), "partner's damp stops the explosion");
    CHECK(LOG_HAS(STRINGID_PKMNPREVENTSUSAGE), "damp message");
}

// ---------------------------------------------------------------- Insomnia / Vital Spirit

static void CheckInsomnia(struct BattleSim *sim)
{
    CHECK(STATUS1(0) == 0, "never slept");
    CHECK(!(STATUS3(0) & STATUS3_YAWN), "yawn not applied");
    CHECK(LOG_HAS(STRINGID_PKMNSXMADEITINEFFECTIVE), "yawn: 'made it ineffective'");
    CHECK(HP(0) == 50, "rest failed: no heal (hp %d)", HP(0));
    CHECK(LOG_COUNT(STRINGID_PKMNSTAYEDAWAKEUSING) == 2, "rest and sing: 'stayed awake using' (%d)", LOG_COUNT(STRINGID_PKMNSTAYEDAWAKEUSING));
}

static void CheckVitalSpiritCure(struct BattleSim *sim)
{
    CHECK(STATUS1(0) == 0, "pre-set sleep cured by vital spirit at move end");
    CHECK(LOG_HAS(STRINGID_PKMNSXCUREDITSYPROBLEM), "cured message");
}

// ---------------------------------------------------------------- Soundproof

static void CheckSoundproof(struct BattleSim *sim)
{
    CHECK(STAGE(1, STAT_ATK) == 6, "growl blocked");
    CHECK(LOG_HAS_T(STRINGID_PKMNSXBLOCKSY, 0) && LOG_HAS_T(STRINGID_PKMNSXBLOCKSY, 1), "blocks message for growl and hyper voice");
    CHECK(PP(0, 0) == 39 && PP(0, 1) == 9, "pp still deducted for blocked sound moves (%d/%d)", PP(0, 0), PP(0, 1));
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_POUND, 2) && HP(1) < MAXHP(1), "non-sound move lands");
}

static void CheckSoundproofPerishSong(struct BattleSim *sim)
{
    CHECK(STATUS3(0) & STATUS3_PERISH_SONG, "user affected");
    CHECK(!(STATUS3(1) & STATUS3_PERISH_SONG), "soundproof foe not affected");
    CHECK(LOG_HAS(STRINGID_PKMNSXBLOCKSY2), "blocks message");
}

static void CheckSoundproofUserPerishSong(struct BattleSim *sim)
{
    CHECK(!(STATUS3(0) & STATUS3_PERISH_SONG), "soundproof user unaffected by its own song");
    CHECK(STATUS3(1) & STATUS3_PERISH_SONG, "foe affected");
}

// ---------------------------------------------------------------- Oblivious / Own Tempo

static void CheckOblivious(struct BattleSim *sim)
{
    CHECK(!(STATUS2(1) & STATUS2_INFATUATION), "not infatuated");
    CHECK(LOG_HAS(STRINGID_PKMNPREVENTSROMANCEWITH), "oblivious message");
}

static int WantSwaggerHit(struct BattleSim *sim) { return STAGE(0, STAT_ATK) == 8; }
static void CheckOwnTempo(struct BattleSim *sim)
{
    CHECK(STAGE(0, STAT_ATK) == 8, "swagger still raised attack");
    CHECK(!(STATUS2(0) & STATUS2_CONFUSION), "no confusion");
    CHECK(LOG_COUNT(STRINGID_PKMNPREVENTSCONFUSIONWITH) == 2, "own tempo message for swagger and confuse ray (%d)", LOG_COUNT(STRINGID_PKMNPREVENTSCONFUSIONWITH));
}

static void CheckOwnTempoThrash(struct BattleSim *sim)
{
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_THRASH, 0), "thrashed");
    CHECK(!(STATUS2(0) & STATUS2_CONFUSION), "no fatigue confusion with own tempo");
}

// ---------------------------------------------------------------- Keen Eye / Hyper Cutter / Clear Body / White Smoke

static void CheckKeenEye(struct BattleSim *sim)
{
    CHECK(STAGE(0, STAT_ACC) == 6, "accuracy intact (stage %d)", STAGE(0, STAT_ACC));
    CHECK(LOG_HAS_T(STRINGID_PKMNSXPREVENTSYLOSS, 0), "sand attack: keen eye message");
    CHECK(HP(0) < MAXHP(0), "mud-slap still did damage");
    CHECK(LOG_COUNT(STRINGID_PKMNSXPREVENTSYLOSS) == 1, "secondary effect blocked silently");
}

static void CheckHyperCutter(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNSXPREVENTSYLOSS), "growl blocked by hyper cutter");
    CHECK(STAGE(0, STAT_ATK) == 5 && STAGE(0, STAT_DEF) == 5, "superpower's own drop still applies (%d/%d)", STAGE(0, STAT_ATK), STAGE(0, STAT_DEF));
}

static void CheckClearBody(struct BattleSim *sim)
{
    CHECK(LOG_COUNT(STRINGID_PKMNPREVENTSSTATLOSSWITH) == 3, "growl, tail whip, sand attack all blocked (%d)", LOG_COUNT(STRINGID_PKMNPREVENTSSTATLOSSWITH));
    CHECK(STAGE(0, STAT_ACC) == 6, "accuracy intact");
    CHECK(STAGE(0, STAT_ATK) == 5 && STAGE(0, STAT_DEF) == 5, "self-inflicted superpower drop applies (%d/%d)", STAGE(0, STAT_ATK), STAGE(0, STAT_DEF));
}

static void CheckWhiteSmoke(struct BattleSim *sim)
{
    CHECK(STAGE(0, STAT_ATK) == 6, "growl blocked");
    CHECK(LOG_HAS(STRINGID_PKMNPREVENTSSTATLOSSWITH), "white smoke message");
}

static void CheckMementoVsClearBody(struct BattleSim *sim)
{
    CHECK(PARTY_HP(B_SIDE_PLAYER, 0) == 0, "memento user fainted anyway");
    CHECK(STAGE(1, STAT_ATK) == 6 && STAGE(1, STAT_SPATK) == 6, "no drops on clear body");
    CHECK(LOG_COUNT(STRINGID_PKMNPREVENTSSTATLOSSWITH) == 1, "message printed once (statLowered flag)");
}

static void CheckTickleVsHyperCutter(struct BattleSim *sim)
{
    CHECK(STAGE(0, STAT_ATK) == 6, "attack protected");
    CHECK(STAGE(0, STAT_DEF) == 5, "defense still lowered");
    CHECK(LOG_HAS(STRINGID_PKMNSXPREVENTSYLOSS), "hyper cutter message");
}

// ---------------------------------------------------------------- Suction Cups / Inner Focus

static void CheckSuctionCups(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_OCTILLERY, "not roared out");
    CHECK(LOG_HAS(STRINGID_PKMNANCHORSITSELFWITH), "suction cups message");
}

static void CheckInnerFocus(struct BattleSim *sim)
{
    CHECK(MOVED_BEFORE(1, 0, 0), "fake out went first");
    CHECK(HP(0) < MAXHP(0), "fake out damage dealt");
    CHECK(LOG_HAS(STRINGID_PKMNSXPREVENTSFLINCHING), "inner focus message (certain flinch)");
    CHECK(!LOG_HAS(STRINGID_PKMNFLINCHED), "did not flinch");
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_BITE, 0), "golbat still attacked");
}

// ---------------------------------------------------------------- Status immunities

static void CheckMagmaArmorCure(struct BattleSim *sim)
{
    CHECK(STATUS1(0) == 0, "pre-set freeze cured by magma armor");
    CHECK(LOG_HAS(STRINGID_PKMNSXCUREDITSYPROBLEM), "cured message");
}

static void CheckWaterVeil(struct BattleSim *sim)
{
    CHECK(STATUS1(0) == 0, "not burned");
    CHECK(LOG_HAS(STRINGID_PKMNSXPREVENTSBURNS), "water veil message");
}

static void CheckLimber(struct BattleSim *sim)
{
    // Static's paralysis is a non-primary, non-certain effect: Limber blocks it silently, so only the
    // Thunder Wave message can be asserted.
    CHECK(STATUS1(0) == 0, "persian never paralyzed");
    CHECK(LOG_HAS(STRINGID_PKMNPREVENTSPARALYSISWITH), "thunder wave: limber message");
    CHECK(!LOG_HAS(STRINGID_PKMNSXPREVENTSYSZ), "no ability-status message for static");
}

static int WantToxicLanded(struct BattleSim *sim) { return (STATUS1(1) & STATUS1_TOXIC_POISON) != 0; }
static void CheckImmunity(struct BattleSim *sim)
{
    CHECK(STATUS1(1) & STATUS1_TOXIC_POISON, "espeon badly poisoned");
    CHECK(STATUS1(0) == 0, "zangoose never poisoned");
    CHECK(LOG_HAS_T(STRINGID_PKMNSXPREVENTSYSZ, 0), "synchronize blocked by immunity");
    CHECK(LOG_HAS_T(STRINGID_PKMNPREVENTSPOISONINGWITH, 1), "toxic blocked by immunity");
}

static void CheckNoEffectAbilities(struct BattleSim *sim)
{
    CHECK(B(0).ability == ABILITY_STENCH && B(1).ability == ABILITY_ILLUMINATE, "abilities present");
    CHECK(HP(0) < MAXHP(0) && HP(1) < MAXHP(1), "both attacks landed normally");
    CHECK(!LOG_HAS(STRINGID_PKMNFLINCHED), "stench has no battle effect");
}

static const struct Scenario sScenarios[] =
{
    // ---- Intimidate
    { .name = "intimidate_battle_start",
      .player = { MON(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_GYARADOS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckIntimidateStart },
    { .name = "intimidate_both_leads_index_order",
      .player = { MON(SPECIES_GYARADOS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_GYARADOS, 60, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckIntimidateBothLeads },
    { .name = "intimidate_switch_in_resolves_immediately",
      .player = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_GYARADOS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(SC_SWITCH(1), 0) }, .turns = 1, .check = CheckIntimidateSwitchInImmediate },
    { .name = "intimidate_blocked_clear_body",
      .player = { MON(SPECIES_METANG, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_GYARADOS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckIntimidateBlocked },
    { .name = "intimidate_blocked_hyper_cutter",
      .player = { MON(SPECIES_PINSIR, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_GYARADOS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckIntimidateBlocked },
    { .name = "intimidate_blocked_white_smoke",
      .player = { MON(SPECIES_TORKOAL, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_GYARADOS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckIntimidateBlocked },
    { .name = "intimidate_vs_substitute",
      .player = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_GYARADOS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SUBSTITUTE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(SC_SWITCH(1), 1) }, .turns = 2, .check = CheckIntimidateVsSubstitute },
    { .name = "intimidate_doubles_both_foes",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { MON(SPECIES_GYARADOS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_METANG, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .check = CheckIntimidateDoubles },
    { .name = "intimidate_reentry_stacks",
      .player = { MON(SPECIES_GYARADOS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(SC_SWITCH(1), 0), T(SC_SWITCH(0), 0) }, .turns = 2, .check = CheckIntimidateReentry },
    { .name = "intimidate_vs_mist",
      .player = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_GYARADOS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_MIST, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(SC_SWITCH(1), 1) }, .turns = 2, .check = CheckIntimidateVsMist },
    { .name = "intimidate_on_faint_replacement",
      .player = { MON(SPECIES_MACHOP, 50, MOVE_KARATE_CHOP, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 1, .hpSet = 1 },
                 MON(SPECIES_GYARADOS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckIntimidateFaintReplacement },

    // ---- Trace
    { .name = "trace_copies_intimidate_no_activation",
      .player = { MON_AB(SPECIES_GARDEVOIR, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .enemy = { MON(SPECIES_ARCANINE, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckTraceIntimidate },
    { .name = "trace_copies_wonder_guard",
      .player = { MON_AB(SPECIES_GARDEVOIR, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .enemy = { MON(SPECIES_SHEDINJA, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckTraceWonderGuard },
    { .name = "trace_switch_in_resolves_immediately",
      .player = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_PORYGON, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_ARCANINE, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(SC_SWITCH(1), 0) }, .turns = 1, .check = CheckTraceSwitchIn },
    { .name = "trace_doubles_random_foe",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { MON(SPECIES_PORYGON, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_ARCANINE, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .wantSeed = WantTraceImmunity, .check = CheckTraceDoubles },
    { .name = "trace_vs_trace",
      .player = { MON(SPECIES_PORYGON, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_PORYGON2, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckTraceVsTrace },
    { .name = "trace_forecast_no_form_change",
      .player = { MON_AB(SPECIES_GARDEVOIR, 50, MOVE_RAIN_DANCE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .enemy = { MON(SPECIES_CASTFORM, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckTraceForecast },

    // ---- Weather abilities
    { .name = "drizzle_permanent_rain",
      .player = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_KYOGRE, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0), T(0, 0), T(0, 0), T(0, 0), T(0, 0) }, .turns = 6, .check = CheckDrizzlePermanent },
    { .name = "drought_overrides_faster_drizzle",
      .player = { MON(SPECIES_KYOGRE, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_GROUDON, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckDroughtOverridesDrizzle },
    { .name = "drizzle_overrides_faster_drought",
      .player = { MON(SPECIES_GROUDON, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_KYOGRE, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckDrizzleOverridesDrought },
    { .name = "sand_stream_overrides_rain_dance",
      .player = { MON(SPECIES_RATTATA, 50, MOVE_RAIN_DANCE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_TYRANITAR, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, SC_SWITCH(1)), T(1, 0), T(1, 0) }, .turns = 4, .check = CheckSandStreamOverridesRain },
    { .name = "sunny_day_replaces_permanent_rain",
      .player = { MON(SPECIES_RATTATA, 50, MOVE_SUNNY_DAY, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_KYOGRE, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0), T(1, 0), T(1, 0), T(1, 0), T(1, 0) }, .turns = 6, .check = CheckSunnyDayReplacesDrizzle },
    { .name = "rain_dance_fails_under_drizzle",
      .player = { MON(SPECIES_RATTATA, 50, MOVE_RAIN_DANCE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_KYOGRE, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckRainDanceFailsUnderDrizzle },
    { .name = "drizzle_reactivates_over_temporary_rain",
      .player = { MON(SPECIES_RATTATA, 50, MOVE_SUNNY_DAY, MOVE_RAIN_DANCE, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_KYOGRE, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, SC_SWITCH(1)), T(2, SC_SWITCH(0)) }, .turns = 3, .check = CheckDrizzleReentry },
    { .name = "drizzle_no_reactivation_when_permanent",
      .player = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_KYOGRE, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_KYOGRE, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, SC_SWITCH(1)) }, .turns = 1, .check = CheckDrizzleNoReactivation },

    // ---- Forecast / Castform
    { .name = "castform_rain_from_drizzle",
      .player = { MON(SPECIES_CASTFORM, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_KYOGRE, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckCastformRain },
    { .name = "castform_sun_form",
      .player = { MON(SPECIES_CASTFORM, 50, MOVE_SUNNY_DAY, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckCastformSun },
    { .name = "castform_reverts_when_sun_fades",
      .player = { MON(SPECIES_CASTFORM, 50, MOVE_SUNNY_DAY, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0), T(1, 0), T(1, 0), T(1, 0) }, .turns = 5, .check = CheckCastformSunFades },
    { .name = "castform_sandstorm_stays_normal",
      .player = { MON(SPECIES_CASTFORM, 50, MOVE_SANDSTORM, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckCastformSandstorm },
    { .name = "castform_hail_ice_form",
      .player = { MON(SPECIES_CASTFORM, 50, MOVE_HAIL, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckCastformHail },
    { .name = "cloud_nine_entry_reverts_castform",
      .player = { MON(SPECIES_CASTFORM, 50, MOVE_RAIN_DANCE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON_AB(SPECIES_GOLDUCK, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .actions = { T(0, 0), T(1, SC_SWITCH(1)) }, .turns = 2, .check = CheckCloudNineRevertsCastform },
    { .name = "cloud_nine_blocks_castform_change",
      .player = { MON(SPECIES_CASTFORM, 50, MOVE_RAIN_DANCE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_AB(SPECIES_GOLDUCK, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckCloudNineBlocksCastform },
    { .name = "castform_switch_in_under_rain",
      .player = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_CASTFORM, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_KYOGRE, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(SC_SWITCH(1), 0) }, .turns = 1, .check = CheckCastformSwitchInRain },

    // ---- Cloud Nine / Air Lock / weather speed
    { .name = "cloud_nine_negates_sandstorm_damage",
      .player = { MON_AB(SPECIES_GOLDUCK, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .enemy = { MON(SPECIES_TYRANITAR, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .check = CheckCloudNineSandstorm },
    { .name = "air_lock_solar_beam_charges",
      .player = { MON(SPECIES_RAYQUAZA, 50, MOVE_SOLAR_BEAM, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_GROUDON, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckAirLockSolarBeam },
    { .name = "swift_swim_rain_speed",
      .player = { MON(SPECIES_OMANYTE, 50, MOVE_RAIN_DANCE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_PSYDUCK, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckSwiftSwimSpeed },
    { .name = "cloud_nine_negates_swift_swim",
      .player = { MON(SPECIES_OMANYTE, 50, MOVE_RAIN_DANCE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_AB(SPECIES_GOLDUCK, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckCloudNineSwiftSwim },
    { .name = "chlorophyll_sun_speed",
      .player = { MON(SPECIES_ODDISH, 50, MOVE_SUNNY_DAY, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_PSYDUCK, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckChlorophyllSpeed },

    // ---- Pressure
    { .name = "pressure_pp_singles",
      .player = { MON(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_SWORDS_DANCE, MOVE_GROWL, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_ARTICUNO, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0), T(2, 0) }, .turns = 3, .check = CheckPressureSingles },
    { .name = "pressure_pp_doubles_spread",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { MON(SPECIES_DUGTRIO, 50, MOVE_EARTHQUAKE, MOVE_GROWL, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_ARTICUNO, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_ARTICUNO, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_ZAPDOS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T4(0, 0, 0, 0), T4(1, 0, 0, 0) }, .turns = 2, .check = CheckPressureDoubles },
    { .name = "pressure_perish_song_counts_all",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { MON(SPECIES_MISDREAVUS, 50, MOVE_PERISH_SONG, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_ARTICUNO, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_ARTICUNO, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_ZAPDOS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .check = CheckPressurePerishSong },
    { .name = "pressure_imprison_extra_pp",
      .player = { MON(SPECIES_ALAKAZAM, 50, MOVE_IMPRISON, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_ARTICUNO, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 1) }, .turns = 1, .check = CheckPressureImprison },
    { .name = "pressure_spikes_failure_costs_one",
      .player = { MON(SPECIES_FORRETRESS, 50, MOVE_SPIKES, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_ARTICUNO, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0), T(0, 0), T(0, 0) }, .turns = 4, .check = CheckPressureSpikes },
    { .name = "pressure_magic_coat_bounce_quirk",
      .player = { MON(SPECIES_ALAKAZAM, 50, MOVE_MAGIC_COAT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_ARTICUNO, 50, MOVE_TOXIC, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckPressureMagicCoat },

    // ---- Speed Boost / Truant
    { .name = "speed_boost_each_turn_capped",
      .player = { MON(SPECIES_NINJASK, 50, MOVE_AGILITY, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0), T(1, 0) }, .turns = 3, .check = CheckSpeedBoostCap },
    { .name = "speed_boost_not_on_switch_in_turn",
      .player = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_NINJASK, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(SC_SWITCH(1), 0), T(0, 0) }, .turns = 2, .check = CheckSpeedBoostSwitchIn },
    { .name = "truant_alternates",
      .player = { MON(SPECIES_SLAKING, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_STEELIX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0), T(0, 0), T(0, 0) }, .turns = 4, .check = CheckTruantAlternates },
    { .name = "truant_cancels_thrash",
      .player = { MON(SPECIES_SLAKING, 50, MOVE_THRASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_STEELIX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0), T(0, 0) }, .turns = 3, .check = CheckTruantCancelsThrash },
    { .name = "truant_switch_in_acts_first_turn",
      .player = { MON(SPECIES_SLAKING, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_STEELIX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(SC_SWITCH(1), 0), T(SC_SWITCH(0), 0), T(0, 0), T(0, 0) }, .turns = 5, .check = CheckTruantSwitchIn },
    { .name = "truant_replacement_after_ko_acts_first",
      .player = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 1, .hpSet = 1 },
                  MON(SPECIES_SLAKING, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_STEELIX, 50, MOVE_HEADBUTT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 1), T(0, 1) }, .turns = 3, .check = CheckTruantReplacementAfterKO },
    { .name = "truant_counter_advances_while_asleep",
      .player = { { .species = SPECIES_SLAKING, .level = 50, .moves = { MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .status = STATUS1_SLEEP_TURN(2) } },
      .enemy = { MON(SPECIES_STEELIX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0), T(0, 0) }, .turns = 3, .check = CheckTruantAsleep },
    { .name = "truant_replacement_after_poison_faint_loafs",
      .player = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 1, .hpSet = 1, .status = STATUS1_POISON },
                  MON(SPECIES_SLAKING, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_STEELIX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0), T(0, 0) }, .turns = 3, .check = CheckTruantReplacementAfterPoisonFaint },

    // ---- Rain Dish / Shed Skin / Natural Cure
    { .name = "rain_dish_heals_in_rain",
      .player = { { .species = SPECIES_LUDICOLO, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .abilityNum = 1, .hp = 100, .hpSet = 1 } },
      .enemy = { MON(SPECIES_KYOGRE, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckRainDish },
    { .name = "rain_dish_no_heal_under_cloud_nine",
      .player = { { .species = SPECIES_LUDICOLO, .level = 50, .moves = { MOVE_RAIN_DANCE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .abilityNum = 1, .hp = 100, .hpSet = 1 } },
      .enemy = { MON_AB(SPECIES_GOLDUCK, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckRainDishCloudNine },
    { .name = "shed_skin_cures",
      .player = { { .species = SPECIES_ARBOK, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .abilityNum = 1, .status = STATUS1_POISON } },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantShedSkinCured, .check = CheckShedSkinCured },
    { .name = "shed_skin_may_not_trigger",
      .player = { { .species = SPECIES_ARBOK, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .abilityNum = 1, .status = STATUS1_POISON } },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantShedSkinNotCured, .check = CheckShedSkinNotCured },
    { .name = "natural_cure_on_switch",
      .player = { { .species = SPECIES_CHANSEY, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .status = STATUS1_BURN },
                  MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(SC_SWITCH(1), 0) }, .turns = 1, .check = CheckNaturalCureSwitch },
    { .name = "natural_cure_on_roar",
      .player = { { .species = SPECIES_CHANSEY, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .status = STATUS1_BURN },
                  MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_ARCANINE, 50, MOVE_ROAR, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckNaturalCureRoar },

    // ---- Levitate
    { .name = "levitate_immune_to_ground_moves",
      .player = { MON(SPECIES_GENGAR, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_DUGTRIO, 50, MOVE_EARTHQUAKE, MOVE_FISSURE, MOVE_MAGNITUDE, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 1), T(0, 2) }, .turns = 3, .check = CheckLevitateGround },
    { .name = "levitate_not_immune_to_sand_attack",
      .player = { MON(SPECIES_GENGAR, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_DUGTRIO, 50, MOVE_SAND_ATTACK, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckLevitateSandAttack },
    { .name = "levitate_ignores_spikes",
      .player = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_GENGAR, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_FORRETRESS, 50, MOVE_SPIKES, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(SC_SWITCH(1), 1) }, .turns = 2, .check = CheckLevitateSpikes },
    { .name = "levitate_escapes_arena_trap",
      .player = { MON(SPECIES_GENGAR, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_AB(SPECIES_DUGTRIO, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .actions = { T(SC_SWITCH(1), 0), T(SC_SWITCH(0), 0) }, .turns = 2, .check = CheckLevitateArenaTrap },
    { .name = "levitate_does_not_stop_roar",
      .player = { MON(SPECIES_GENGAR, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_ARCANINE, 50, MOVE_ROAR, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckLevitateRoar },

    // ---- Sturdy
    { .name = "sturdy_blocks_ohko_only",
      .player = { MON(SPECIES_RHYDON, 50, MOVE_HORN_DRILL, MOVE_HORN_ATTACK, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { { .species = SPECIES_GOLEM, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .abilityNum = 1, .hp = 1, .hpSet = 1 } },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckSturdy },

    // ---- Wonder Guard
    { .name = "wonder_guard_only_super_effective",
      .player = { MON(SPECIES_CHARMANDER, 50, MOVE_SCRATCH, MOVE_EMBER, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SHEDINJA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckWonderGuardTypes },
    { .name = "wonder_guard_leech_seed_drain",
      .player = { MON(SPECIES_BULBASAUR, 50, MOVE_LEECH_SEED, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SHEDINJA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantShedinjaDown, .check = CheckWonderGuardLeechSeed },
    { .name = "wonder_guard_sandstorm_chip",
      .player = { MON(SPECIES_TYRANITAR, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SHEDINJA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckWonderGuardSandstorm },
    { .name = "wonder_guard_struggle_hits",
      .player = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_TACKLE, MOVE_NONE, MOVE_NONE, MOVE_NONE }, .pp = { 0, 0, 0, 0 }, .ppSet = 1 } },
      .enemy = { MON(SPECIES_SHEDINJA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckWonderGuardStruggle },
    { .name = "wonder_guard_status_moves_pass",
      .player = { MON(SPECIES_ARBOK, 50, MOVE_TOXIC, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SHEDINJA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantShedinjaToxic, .check = CheckWonderGuardStatusMove },
    { .name = "wonder_guard_skill_swap_role_play_fail",
      .player = { MON(SPECIES_ALAKAZAM, 50, MOVE_SKILL_SWAP, MOVE_ROLE_PLAY, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SHEDINJA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckWonderGuardAbilityMoves },

    // ---- Damp
    { .name = "damp_user_own_explosion_fails",
      .player = { MON_AB(SPECIES_POLIWRATH, 50, MOVE_EXPLOSION, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckDampSelf },
    { .name = "damp_foe_blocks_explosion",
      .player = { MON_AB(SPECIES_POLIWRATH, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .enemy = { MON(SPECIES_ELECTRODE, 50, MOVE_EXPLOSION, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckDampFoe },
    { .name = "damp_partner_blocks_explosion",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { MON(SPECIES_ELECTRODE, 50, MOVE_EXPLOSION, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON_AB(SPECIES_POLIWRATH, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .check = CheckDampPartner },

    // ---- Insomnia / Vital Spirit
    { .name = "insomnia_vs_yawn_rest_sing",
      .player = { { .species = SPECIES_HYPNO, .level = 50, .moves = { MOVE_REST, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 50, .hpSet = 1 } },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_YAWN, MOVE_SING, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .check = CheckInsomnia },
    { .name = "vital_spirit_cures_preset_sleep",
      .player = { { .species = SPECIES_PRIMEAPE, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .status = STATUS1_SLEEP_TURN(3) } },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckVitalSpiritCure },

    // ---- Soundproof
    { .name = "soundproof_blocks_sound_moves",
      .player = { MON(SPECIES_JIGGLYPUFF, 50, MOVE_GROWL, MOVE_HYPER_VOICE, MOVE_POUND, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_VOLTORB, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0), T(2, 0) }, .turns = 3, .check = CheckSoundproof },
    { .name = "soundproof_vs_perish_song",
      .player = { MON(SPECIES_MISDREAVUS, 50, MOVE_PERISH_SONG, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_VOLTORB, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSoundproofPerishSong },
    { .name = "soundproof_user_perish_song",
      .player = { MON(SPECIES_VOLTORB, 50, MOVE_PERISH_SONG, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSoundproofUserPerishSong },

    // ---- Oblivious / Own Tempo
    { .name = "oblivious_blocks_attract",
      .player = { MON(SPECIES_JYNX, 50, MOVE_ATTRACT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SLOWBRO, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckOblivious },
    { .name = "own_tempo_vs_swagger_and_confuse_ray",
      .player = { MON(SPECIES_SPINDA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_GENGAR, 50, MOVE_SWAGGER, MOVE_CONFUSE_RAY, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 1) }, .turns = 2, .wantSeed = WantSwaggerHit, .check = CheckOwnTempo },
    { .name = "own_tempo_no_thrash_fatigue",
      .player = { MON(SPECIES_SPINDA, 50, MOVE_THRASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_STEELIX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0), T(0, 0), T(0, 0) }, .turns = 4, .check = CheckOwnTempoThrash },

    // ---- Keen Eye / Hyper Cutter / Clear Body / White Smoke
    { .name = "keen_eye_blocks_accuracy_drops",
      .player = { MON(SPECIES_HITMONCHAN, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_DUGTRIO, 50, MOVE_SAND_ATTACK, MOVE_MUD_SLAP, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 1) }, .turns = 2, .check = CheckKeenEye },
    { .name = "hyper_cutter_blocks_foe_not_self",
      .player = { MON(SPECIES_PINSIR, 50, MOVE_SPLASH, MOVE_SUPERPOWER, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_MEOWTH, 50, MOVE_GROWL, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .check = CheckHyperCutter },
    { .name = "clear_body_blocks_foe_drops_not_self",
      .player = { MON(SPECIES_METAGROSS, 50, MOVE_SPLASH, MOVE_SUPERPOWER, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_MEOWTH, 50, MOVE_GROWL, MOVE_TAIL_WHIP, MOVE_SAND_ATTACK, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 1), T(1, 2) }, .turns = 3, .check = CheckClearBody },
    { .name = "white_smoke_blocks_growl",
      .player = { MON(SPECIES_TORKOAL, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_MEOWTH, 50, MOVE_GROWL, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckWhiteSmoke },
    { .name = "memento_vs_clear_body",
      .player = { MON(SPECIES_MISDREAVUS, 50, MOVE_MEMENTO, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_METANG, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckMementoVsClearBody },
    { .name = "tickle_vs_hyper_cutter",
      .player = { MON(SPECIES_PINSIR, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_AIPOM, 50, MOVE_TICKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckTickleVsHyperCutter },

    // ---- Suction Cups / Inner Focus
    { .name = "suction_cups_blocks_roar",
      .player = { MON(SPECIES_OCTILLERY, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_ARCANINE, 50, MOVE_ROAR, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSuctionCups },
    { .name = "inner_focus_blocks_fake_out",
      .player = { MON(SPECIES_GOLBAT, 50, MOVE_BITE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_PERSIAN, 50, MOVE_FAKE_OUT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckInnerFocus },

    // ---- Status immunities
    { .name = "magma_armor_cures_preset_freeze",
      .player = { { .species = SPECIES_SLUGMA, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .status = STATUS1_FREEZE } },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckMagmaArmorCure },
    { .name = "water_veil_blocks_will_o_wisp",
      .player = { MON(SPECIES_WAILMER, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_VULPIX, 50, MOVE_WILL_O_WISP, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckWaterVeil },
    { .name = "limber_blocks_thunder_wave",
      .player = { MON(SPECIES_PERSIAN, 50, MOVE_SCRATCH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_PIKACHU, 50, MOVE_THUNDER_WAVE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .check = CheckLimber },
    { .name = "immunity_blocks_toxic_and_synchronize",
      .player = { MON(SPECIES_ZANGOOSE, 50, MOVE_TOXIC, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_ESPEON, 50, MOVE_SPLASH, MOVE_TOXIC, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .wantSeed = WantToxicLanded, .check = CheckImmunity },
    { .name = "stench_illuminate_no_effect",
      .player = { MON(SPECIES_GRIMER, 50, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_STARYU, 50, MOVE_WATER_GUN, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckNoEffectAbilities },
};

SCENARIO_GROUP(abilities_field, sScenarios)
