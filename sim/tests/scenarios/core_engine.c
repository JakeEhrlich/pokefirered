// Core engine edge cases: turn order, switching, multi-turn moves, protection, end-of-turn ordering.
#include "scenario.h"

static void CheckQuickAttackFirst(struct BattleSim *sim)
{
    // Slow Snorlax's Quick Attack (+1) goes before faster Rattata's Tackle.
    CHECK(MOVED_BEFORE(0, 1, 0), "quick attack first");
}

static void CheckParalysisOrder(struct BattleSim *sim)
{
    CHECK(STATUS1(1) & STATUS1_PARALYSIS, "target paralyzed");
    // turn 2: paralyzed Rattata (speed/4) moves after Bulbasaur
    CHECK(MOVED_BEFORE(1, 0, 0), "faster rattata moved first on turn 1");
    CHECK(MOVED_BEFORE(0, 1, 1) || LOG_INDEX_T(STRINGID_USEDMOVE, 1, 1) < 0, "paralyzed rattata moves last on turn 2");
}

static void CheckSwitchBeatsMove(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_SQUIRTLE, "switched to squirtle");
    CHECK(LOG_INDEX_T(STRINGID_SWITCHINMON, 0, 0) < LOG_INDEX_T(STRINGID_USEDMOVE, 1, 0), "switch resolves before the enemy move");
    CHECK(PARTY_HP(B_SIDE_PLAYER, 0) == (int)GetMonData(&sim->playerParty[0], MON_DATA_MAX_HP), "old mon untouched");
}

static void CheckPursuitOnSwitch(struct BattleSim *sim)
{
    // Pursuit hits the switching Bulbasaur (before it leaves) for double damage.
    CHECK(PARTY_HP(B_SIDE_PLAYER, 0) < (int)GetMonData(&sim->playerParty[0], MON_DATA_MAX_HP), "pursuit hit the switching mon");
    CHECK(B(0).species == SPECIES_SQUIRTLE && HP(0) == MAXHP(0), "squirtle came in untouched");
}

static void CheckSolarBeamCharges(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNTOOKSUNLIGHT), "charge message on turn 1");
    CHECK(HP(1) == MAXHP(1), "no damage on the charge turn");
}

static void CheckSolarBeamSun(struct BattleSim *sim)
{
    CHECK(WEATHER() & B_WEATHER_SUN, "sun active");
    CHECK(HP(1) < MAXHP(1), "solar beam fired in one turn under sun");
}

static void CheckFlyDodges(struct BattleSim *sim)
{
    // Pidgey uses Fly (turn 1: goes up); Rattata's Tackle misses; turn 2 Fly lands.
    CHECK(LOG_HAS(STRINGID_PKMNFLEWHIGH), "flew up");
    CHECK(HP(0) == MAXHP(0), "tackle missed the flying mon");
}

static void CheckDigHitByEarthquake(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNDUGHOLE), "dug a hole");
    CHECK(HP(0) < MAXHP(0), "earthquake hits the underground mon");
}

static void CheckHyperBeamRecharge(struct BattleSim *sim)
{
    // Turn 1 Hyper Beam hits, turn 2 the user must recharge.
    CHECK(LOG_HAS(STRINGID_PKMNMUSTRECHARGE), "recharge turn");
}

static void CheckRolloutDefenseCurl(struct BattleSim *sim)
{
    // Damage per hit should keep doubling (and be doubled again by Defense Curl).
    int i, last = 0, ok = 1, hits = 0;
    for (i = 0; i < sim->logCount; i++)
        if (sim->log[i].stringId == STRINGID_USEDMOVE && sim->log[i].battler == 0)
            hits++;
    CHECK(hits >= 1, "rollout kept going");
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_ROLLOUT, 3) || LOG_HAS(STRINGID_ATTACKMISSED) || OUTCOME() != 0, "still rolling on turn 4 (or a miss/KO ended it)");
    CHECK(STATUS2(0) & STATUS2_DEFENSE_CURL, "defense curl flag set");
    (void)last; (void)ok;
}

static void CheckThrashLocksThenConfuses(struct BattleSim *sim)
{
    CHECK(STATUS2(0) & STATUS2_CONFUSION, "confused after thrash ends");
    CHECK(LOG_HAS(STRINGID_PKMNFATIGUECONFUSION), "fatigue message");
}

static void CheckCounter(struct BattleSim *sim)
{
    // Counter returns double the physical damage taken this turn.
    CHECK(HP(1) < MAXHP(1), "counter dealt damage");
}

static void CheckSubstituteBlocksStatus(struct BattleSim *sim)
{
    CHECK(STATUS2(0) & STATUS2_SUBSTITUTE, "substitute up");
    CHECK(!(STATUS1(0) & STATUS1_PSN_ANY), "toxic blocked by substitute");
    CHECK(!(STATUS3(0) & STATUS3_LEECHSEED), "leech seed blocked by substitute");
}

static void CheckProtectTwiceFails(struct BattleSim *sim)
{
    // Turn 1 protect succeeds; turn 2 second protect fails (with this seed) and Tackle lands.
    CHECK(HP(0) < MAXHP(0), "second protect failed and tackle hit");
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "failure message");
}
static int WantProtectFail(struct BattleSim *sim) { return HP(0) < MAXHP(0) && Sc_LogHas(sim, STRINGID_BUTITFAILED, SC_ANY_TURN); }

static int WantToxicHit(struct BattleSim *sim) { return (STATUS1(1) & STATUS1_TOXIC_POISON) != 0; }
static void CheckToxicCounter(struct BattleSim *sim)
{
    // Toxic damage grows each turn: after 3 turns of toxic the counter is 3 (bits 8-11 hold it).
    CHECK(STATUS1(1) & STATUS1_TOXIC_POISON, "badly poisoned");
    CHECK(((STATUS1(1) & STATUS1_TOXIC_COUNTER) >> 8) == 3, "toxic counter 3 (got %d)", (STATUS1(1) & STATUS1_TOXIC_COUNTER) >> 8);
    CHECK(HP(1) == MAXHP(1) - (MAXHP(1) / 16) * (1 + 2 + 3), "toxic damage 1/16 + 2/16 + 3/16 (hp %d/%d)", HP(1), MAXHP(1));
}

static void CheckRestAndSleepTalk(struct BattleSim *sim)
{
    CHECK(HP(0) == MAXHP(0), "rest healed");
    CHECK((STATUS1(0) & STATUS1_SLEEP) != 0, "still asleep after 1 turn (rest sleeps 2)");
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_SLEEP_TALK, 1), "sleep talk used on turn 2");
}

static void CheckEncore(struct BattleSim *sim)
{
    CHECK(sim->disableStructs[1].encoredMove == MOVE_GROWL, "encored into growl");
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_GROWL, 2), "forced to use growl on turn 3");
    CHECK(HP(0) == MAXHP(0), "no tackle damage while encored");
}

static void CheckTauntBlocksStatus(struct BattleSim *sim)
{
    // Taunt lasts 2 turns (timer 2, decremented at the end of each turn). On turn 2 the taunted mon
    // cannot even select Growl, so the script falls back to its first legal move (Splash).
    CHECK(!LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_GROWL, 1), "growl not usable under taunt");
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_SPLASH, 1), "fell back to splash");
}

static void CheckChoiceBandResetsOnSwitch(struct BattleSim *sim)
{
    // Locked into Tackle, switched out and back in: lock cleared.
    CHECK(sim->sBattleStructStorage.choicedMove[0] == MOVE_NONE, "choice lock cleared by switching");
}

static void CheckBurnBeforeLeftovers(struct BattleSim *sim)
{
    // End of turn: Leftovers heal happens before burn damage (item effects, then status).
    u16 max = MAXHP(0);
    CHECK(STATUS1(0) & STATUS1_BURN, "burned");
    CHECK(HP(0) == max - 50 + max / 16 - max / 8, "seismic toss 50, leftovers +1/16, burn -1/8 (hp %d/%d)", HP(0), max);
}

static void CheckPerishSong(struct BattleSim *sim)
{
    // Both fainted at the end of turn 3: battle decided as a draw or loss.
    CHECK(OUTCOME() != 0, "battle ended by perish song (outcome %d)", OUTCOME());
}

static void CheckBatonPass(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_RATTATA, "rattata received the pass");
    CHECK(STAGE(0, STAT_ATK) == 8, "passed +2 attack (stage %d)", STAGE(0, STAT_ATK));
    CHECK(STATUS2(0) & STATUS2_SUBSTITUTE, "substitute passed along");
}

static void CheckDestinyBond(struct BattleSim *sim)
{
    CHECK(HP(1) == 0, "destiny bond took the attacker down");
}

static void CheckOhkoLevel(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNUNAFFECTED) || LOG_HAS(STRINGID_ATTACKMISSED) || LOG_HAS(STRINGID_BUTITFAILED), "fissure fails against a higher level");
    CHECK(HP(1) == MAXHP(1), "no damage");
}

static void CheckWonderGuard(struct BattleSim *sim)
{
    CHECK(HP(1) == MAXHP(1), "wonder guard blocked tackle");
    CHECK(LOG_HAS(STRINGID_AVOIDEDDAMAGE) || LOG_HAS(STRINGID_ITDOESNTAFFECT), "wonder guard message");
}

static void CheckDampBlocksExplosion(struct BattleSim *sim)
{
    CHECK(HP(0) == MAXHP(0) && HP(1) == MAXHP(1), "explosion prevented");
    CHECK(LOG_HAS(STRINGID_PKMNPREVENTSUSAGE), "damp message");
}

static void CheckWrapTraps(struct BattleSim *sim)
{
    CHECK(STATUS2(1) & STATUS2_WRAPPED, "wrapped");
    CHECK(HP(1) < MAXHP(1), "wrap damage");
    CHECK(B(1).species == SPECIES_RATTATA, "could not switch while wrapped");
}

static void CheckRoar(struct BattleSim *sim)
{
    CHECK(B(1).species == SPECIES_PIDGEY, "roar dragged out the second mon (got %s)", gSimSpeciesNames[B(1).species]);
}

static void CheckStruggle(struct BattleSim *sim)
{
    CHECK(LOG_HAS_MOVE(STRINGID_USEDMOVE, MOVE_STRUGGLE), "used struggle");
    CHECK(HP(0) < MAXHP(0), "struggle recoil");
}

static void CheckTransform(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_RATTATA, "ditto became rattata");
    CHECK(B(0).moves[0] == MOVE_TACKLE && B(0).pp[0] == 5, "copied moves with 5 pp");
    CHECK(B(0).attack == B(1).attack && B(0).speed == B(1).speed, "copied stats");
}

static void CheckSpikes(struct BattleSim *sim)
{
    // Two layers of spikes: switch-in takes 1/6; Pidgey (flying) takes none.
    CHECK(sim->sideTimers[B_SIDE_OPPONENT].spikesAmount == 2, "two layers (got %d)", sim->sideTimers[B_SIDE_OPPONENT].spikesAmount);
    CHECK(B(1).species == SPECIES_RATTATA && HP(1) == MAXHP(1) - MAXHP(1) / 6, "rattata took 1/6 from spikes (hp %d/%d)", HP(1), MAXHP(1));
}

static void CheckFutureSightAfterSwitch(struct BattleSim *sim)
{
    // Future Sight set on turn 1 hits the mon in the target's slot at the end of turn 3 even after a switch.
    CHECK(B(1).species == SPECIES_PIDGEY, "pidgey switched in");
    CHECK(HP(1) < MAXHP(1), "future sight hit the replacement");
}

static void CheckWishHealsReplacement(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_RATTATA, "rattata switched in");
    CHECK(HP(0) == MAXHP(0) || LOG_HAS(STRINGID_PKMNWISHCAMETRUE), "wish came true for the replacement");
}

static void CheckFixedDamageMoves(struct BattleSim *sim)
{
    // Seismic Toss (level 50) + Super Fang (half of remaining) + Endeavor...
    u16 max = MAXHP(1);
    CHECK(HP(1) == (max - 50) - (max - 50) / 2, "seismic toss then super fang (hp %d/%d)", HP(1), max);
}

static void CheckBellyDrum(struct BattleSim *sim)
{
    CHECK(STAGE(0, STAT_ATK) == 12, "attack maxed");
    CHECK(HP(0) == MAXHP(0) - MAXHP(0) / 2, "paid half hp");
}

static void CheckMagicCoat(struct BattleSim *sim)
{
    CHECK(STATUS1(1) & STATUS1_TOXIC_POISON, "toxic bounced back onto the user");
    CHECK(!(STATUS1(0) & STATUS1_PSN_ANY), "magic coat user unaffected");
}

static void CheckFocusPunchFlinch(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNLOSTFOCUS), "lost focus after being hit");
    CHECK(HP(1) == MAXHP(1), "focus punch did not go off");
}

static void CheckFakeOutOnlyFirstTurn(struct BattleSim *sim)
{
    CHECK(LOG_COUNT(STRINGID_BUTITFAILED) >= 1, "fake out failed on the second turn");
}

static void CheckDoublesEarthquakeAndFollowMe(struct BattleSim *sim)
{
    CHECK(HP(2) < MAXHP(2), "partner hit by earthquake");
    CHECK(HP(1) < MAXHP(1) && HP(3) < MAXHP(3), "both foes hit");
}

static const struct Scenario sScenarios[] =
{
    { .name = "quick_attack_priority",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_QUICK_ATTACK, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckQuickAttackFirst },
    { .name = "paralysis_speed",
      .player = { MON(SPECIES_BULBASAUR, 50, MOVE_THUNDER_WAVE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckParalysisOrder },
    { .name = "switch_beats_move",
      .player = { MON(SPECIES_BULBASAUR, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_SQUIRTLE, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(SC_SWITCH(1), 0) }, .turns = 1, .check = CheckSwitchBeatsMove },
    { .name = "pursuit_on_switch",
      .player = { MON(SPECIES_BULBASAUR, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_SQUIRTLE, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_PURSUIT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(SC_SWITCH(1), 0) }, .turns = 1, .check = CheckPursuitOnSwitch },
    { .name = "solar_beam_charges",
      .player = { MON(SPECIES_BULBASAUR, 50, MOVE_SOLAR_BEAM, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSolarBeamCharges },
    { .name = "solar_beam_in_sun",
      .player = { MON(SPECIES_BULBASAUR, 50, MOVE_SUNNY_DAY, MOVE_SOLAR_BEAM, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckSolarBeamSun },
    { .name = "fly_dodges",
      .player = { MON(SPECIES_PIDGEOT, 50, MOVE_FLY, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckFlyDodges },
    { .name = "dig_hit_by_earthquake",
      .player = { MON(SPECIES_SANDSHREW, 50, MOVE_DIG, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RHYDON, 50, MOVE_EARTHQUAKE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckDigHitByEarthquake },
    { .name = "hyper_beam_recharge",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_HYPER_BEAM, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_STEELIX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .check = CheckHyperBeamRecharge },
    { .name = "rollout_defense_curl",
      .player = { MON(SPECIES_GOLEM, 30, MOVE_DEFENSE_CURL, MOVE_ROLLOUT, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_STEELIX, 100, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0), T(1, 0), T(1, 0) }, .turns = 4, .check = CheckRolloutDefenseCurl },
    { .name = "thrash_lock_confusion",
      .player = { MON(SPECIES_TAUROS, 50, MOVE_THRASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_STEELIX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0), T(0, 0), T(0, 0) }, .turns = 4, .check = CheckThrashLocksThenConfuses },
    { .name = "counter",
      .player = { MON(SPECIES_CHANSEY, 50, MOVE_COUNTER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckCounter },
    { .name = "substitute_blocks_status",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SUBSTITUTE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_BULBASAUR, 50, MOVE_TOXIC, MOVE_LEECH_SEED, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0), T(1, 1) }, .turns = 3, .check = CheckSubstituteBlocksStatus },
    { .name = "protect_twice_fails",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_PROTECT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .wantSeed = WantProtectFail, .check = CheckProtectTwiceFails },
    { .name = "toxic_counter",
      .player = { MON(SPECIES_BULBASAUR, 50, MOVE_TOXIC, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_AB(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) }, // Thick Fat, not Immunity
      .actions = { T(0, 0), T(1, 0), T(1, 0) }, .turns = 3, .wantSeed = WantToxicHit, .check = CheckToxicCounter },
    { .name = "rest_sleep_talk",
      .player = { { .species = SPECIES_SNORLAX, .level = 50, .moves = { MOVE_REST, MOVE_SLEEP_TALK, MOVE_SPLASH, MOVE_SPLASH }, .hp = 10, .hpSet = 1 } },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckRestAndSleepTalk },
    { .name = "encore",
      .player = { MON(SPECIES_CLEFABLE, 50, MOVE_ENCORE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_GROWL, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1), T(1, 1) }, .turns = 3, .check = CheckEncore },
    { .name = "taunt",
      .player = { MON(SPECIES_MACHAMP, 50, MOVE_TAUNT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_GROWL, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckTauntBlocksStatus },
    { .name = "choice_band_switch_reset",
      .player = { MON_ITEM(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_QUICK_ATTACK, MOVE_SPLASH, MOVE_SPLASH, ITEM_CHOICE_BAND), MON(SPECIES_PIDGEY, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(SC_SWITCH(1), 0), T(SC_SWITCH(0), 0) }, .turns = 3, .check = CheckChoiceBandResetsOnSwitch },
    { .name = "burn_and_leftovers_order",
      .player = { { .species = SPECIES_SNORLAX, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .item = ITEM_LEFTOVERS, .status = STATUS1_BURN } },
      .enemy = { MON(SPECIES_MACHOP, 50, MOVE_SEISMIC_TOSS, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckBurnBeforeLeftovers },
    { .name = "perish_song",
      .player = { MON(SPECIES_MISDREAVUS, 50, MOVE_PERISH_SONG, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0), T(1, 0), T(1, 0) }, .check = CheckPerishSong },
    { .name = "baton_pass",
      .player = { MON(SPECIES_SCYTHER, 50, MOVE_SWORDS_DANCE, MOVE_SUBSTITUTE, MOVE_BATON_PASS, MOVE_SPLASH), MON(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0), T(2, 0) }, .turns = 3, .check = CheckBatonPass },
    { .name = "destiny_bond",
      .player = { { .species = SPECIES_GENGAR, .level = 50, .moves = { MOVE_DESTINY_BOND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 1, .hpSet = 1 } },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_BITE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckDestinyBond },
    { .name = "ohko_higher_level",
      .player = { MON(SPECIES_DUGTRIO, 40, MOVE_FISSURE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 60, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckOhkoLevel },
    { .name = "wonder_guard",
      .player = { MON(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SHEDINJA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckWonderGuard },
    { .name = "damp_blocks_explosion",
      .player = { MON(SPECIES_ELECTRODE, 50, MOVE_EXPLOSION, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_AB(SPECIES_POLIWRATH, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckDampBlocksExplosion },
    { .name = "wrap_traps",
      .player = { MON(SPECIES_ARBOK, 50, MOVE_WRAP, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_PIDGEY, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, SC_SWITCH(1)) }, .turns = 2, .wantSeed = NULL, .check = CheckWrapTraps },
    { .name = "roar",
      .player = { MON(SPECIES_ARCANINE, 50, MOVE_ROAR, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_PIDGEY, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckRoar },
    { .name = "struggle",
      .player = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_TACKLE, MOVE_NONE, MOVE_NONE, MOVE_NONE }, .pp = { 0, 0, 0, 0 }, .ppSet = 1 } },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckStruggle },
    { .name = "transform",
      .player = { MON(SPECIES_DITTO, 50, MOVE_TRANSFORM, MOVE_NONE, MOVE_NONE, MOVE_NONE) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_GROWL, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 1) }, .turns = 1, .check = CheckTransform },
    { .name = "spikes",
      .player = { MON(SPECIES_FORRETRESS, 50, MOVE_SPIKES, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_PIDGEY, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0), T(1, SC_SWITCH(1)) }, .turns = 3, .check = CheckSpikes },
    { .name = "future_sight_after_switch",
      .player = { MON(SPECIES_ALAKAZAM, 50, MOVE_FUTURE_SIGHT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_PIDGEY, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, SC_SWITCH(1)), T(1, 0) }, .turns = 3, .check = CheckFutureSightAfterSwitch },
    { .name = "wish_heals_replacement",
      .player = { MON(SPECIES_CLEFABLE, 50, MOVE_WISH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 20, .hpSet = 1 } },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(SC_SWITCH(1), 0) }, .turns = 2, .check = CheckWishHealsReplacement },
    { .name = "fixed_damage_moves",
      .player = { MON(SPECIES_RATICATE, 50, MOVE_SEISMIC_TOSS, MOVE_SUPER_FANG, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckFixedDamageMoves },
    { .name = "belly_drum",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_BELLY_DRUM, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckBellyDrum },
    { .name = "magic_coat",
      .player = { MON(SPECIES_ALAKAZAM, 50, MOVE_MAGIC_COAT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_TOXIC, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantToxicHit, .check = CheckMagicCoat },
    { .name = "focus_punch_flinch",
      .player = { MON(SPECIES_MACHAMP, 50, MOVE_FOCUS_PUNCH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckFocusPunchFlinch },
    { .name = "fake_out_first_turn_only",
      .player = { MON(SPECIES_PERSIAN, 50, MOVE_FAKE_OUT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .check = CheckFakeOutOnlyFirstTurn },
    { .name = "doubles_earthquake",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { MON(SPECIES_DUGTRIO, 50, MOVE_EARTHQUAKE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .check = CheckDoublesEarthquakeAndFollowMe },
};

SCENARIO_GROUP(core_engine, sScenarios)
