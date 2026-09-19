// Engine-level edge cases: turn order rules, battle outcome determination, obedience/badges,
// faint-replacement timing, PP exhaustion. Complements core_engine.c.
#include "scenario.h"


// --- turn order ---
static int WantPlayerFirst(struct BattleSim *sim) { return MOVED_BEFORE(0, 1, 0); }
static int WantEnemyFirst(struct BattleSim *sim) { return MOVED_BEFORE(1, 0, 0); }
static void CheckPlayerFirst(struct BattleSim *sim) { CHECK(MOVED_BEFORE(0, 1, 0), "player moved first"); }
static void CheckEnemyFirst(struct BattleSim *sim) { CHECK(MOVED_BEFORE(1, 0, 0), "enemy moved first"); }

static void CheckBadgeSpeedBoost(struct BattleSim *sim)
{
    // Identical Rattatas; the player's side has the speed badge (+10%), so it is strictly faster.
    CHECK(B(0).speed == B(1).speed, "base speed stats identical (%d vs %d)", B(0).speed, B(1).speed);
    CHECK(MOVED_BEFORE(0, 1, 0), "player's badge-boosted rattata moves first");
}

static void CheckPriorityBeatsQuickClaw(struct BattleSim *sim)
{
    // Quick Claw only breaks ties within the same priority bracket: Quick Attack still goes first.
    CHECK(MOVED_BEFORE(1, 0, 0), "quick attack (+1) before a quick-claw tackle");
}
static int WantQuickClaw(struct BattleSim *sim) { return MOVED_BEFORE(0, 1, 0); }
static void CheckQuickClawWorked(struct BattleSim *sim)
{
    CHECK(B(0).speed < B(1).speed, "slowpoke is slower");
    CHECK(MOVED_BEFORE(0, 1, 0), "quick claw let the slower mon move first");
}

static void CheckBothSwitchOrder(struct BattleSim *sim)
{
    // Switches are ordered by battler index, not speed: the player's switch resolves first even though
    // the enemy is faster.
    CHECK(LOG_INDEX_T(STRINGID_SWITCHINMON, 0, 0) >= 0 && LOG_INDEX_T(STRINGID_SWITCHINMON, 1, 0) >= 0, "both switched");
    CHECK(LOG_INDEX_T(STRINGID_SWITCHINMON, 0, 0) < LOG_INDEX_T(STRINGID_SWITCHINMON, 1, 0), "player switch before enemy switch");
}

static void CheckSwitchBeforePriorityMove(struct BattleSim *sim)
{
    CHECK(LOG_INDEX_T(STRINGID_SWITCHINMON, 0, 0) < LOG_INDEX_T(STRINGID_USEDMOVE, 1, 0), "switch resolves before quick attack");
}

static void CheckMinusPriorityLast(struct BattleSim *sim)
{
    // Roar (-6) goes after the slower mon's move.
    CHECK(MOVED_BEFORE(1, 0, 0), "slow rattata moves before roar");
}

static void CheckReplacementDoesNotAct(struct BattleSim *sim)
{
    // Enemy's first mon is KOed by the faster player; the replacement is sent out but does not move
    // this turn, and acts on the next one.
    CHECK(B(1).species == SPECIES_PIDGEY, "pidgey replaced the fainted rattata");
    CHECK(LOG_INDEX_T(STRINGID_USEDMOVE, 1, 0) < 0, "replacement did not act on the faint turn");
    CHECK(LOG_INDEX_T(STRINGID_USEDMOVE, 1, 1) >= 0, "replacement acted on the next turn");
}

// --- outcomes ---
static void CheckDrawRecoil(struct BattleSim *sim)
{
    // Double-Edge KOs the enemy's last mon and its recoil KOs the user (at 1 HP): both parties at 0.
    CHECK(OUTCOME() == B_OUTCOME_DREW, "draw when both last mons faint (outcome %d)", OUTCOME());
}
static int WantDoubleEdgeHit(struct BattleSim *sim) { return HP(1) == 0; }

static void CheckDrawExplosion(struct BattleSim *sim)
{
    CHECK(HP(0) == 0, "user fainted from explosion");
    CHECK(OUTCOME() == B_OUTCOME_DREW || OUTCOME() == B_OUTCOME_LOST, "explosion ending: outcome %d", OUTCOME());
    CHECK(OUTCOME() == (HP(1) == 0 ? B_OUTCOME_DREW : B_OUTCOME_LOST), "draw only if the target also fainted");
}

static void CheckLossOnRecoilSurvivor(struct BattleSim *sim)
{
    // Recoil KOs the user while the target survives: player loses.
    CHECK(HP(1) > 0 && HP(0) == 0, "user fainted, target survived");
    CHECK(OUTCOME() == B_OUTCOME_LOST, "lost (outcome %d)", OUTCOME());
}

static void CheckWinWithReserve(struct BattleSim *sim)
{
    // Player's active mon faints to recoil but a reserve exists: replacement prompt, no outcome yet.
    CHECK(OUTCOME() == B_OUTCOME_WON, "won after KO (outcome %d)", OUTCOME());
}

static void CheckDestinyBondDraw(struct BattleSim *sim)
{
    CHECK(HP(0) == 0 && HP(1) == 0, "both fainted");
    CHECK(OUTCOME() == B_OUTCOME_DREW, "destiny bond on last mons is a draw (outcome %d)", OUTCOME());
}

static void CheckPerishDraw(struct BattleSim *sim)
{
    CHECK(OUTCOME() == B_OUTCOME_DREW, "perish song taking both last mons is a draw (outcome %d)", OUTCOME());
}

// --- obedience ---
static int Disobeyed(struct BattleSim *sim)
{
    return LOG_HAS(STRINGID_PKMNBEGANTONAP) || LOG_HAS(STRINGID_PKMNLOAFING) || LOG_HAS(STRINGID_PKMNTURNEDAWAY)
        || LOG_HAS(STRINGID_PKMNWONTOBEY) || LOG_HAS(STRINGID_PKMNIGNOREDORDERS) || LOG_HAS(STRINGID_PKMNIGNORESASLEEP);
}
static int WantDisobey(struct BattleSim *sim) { return Disobeyed(sim); }
static void CheckDisobeyed(struct BattleSim *sim) { CHECK(Disobeyed(sim), "outsider mon above the obedience level disobeyed"); }
static void CheckObeyed(struct BattleSim *sim)
{
    CHECK(!Disobeyed(sim), "obeyed");
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_TACKLE, 0), "used tackle");
}
static void CheckOwnMonAlwaysObeys(struct BattleSim *sim)
{
    // Player-OT mons obey regardless of badges and level.
    CHECK(!Disobeyed(sim), "own mon obeyed with no badges");
}

// --- PP ---
static void CheckPressureDoublePP(struct BattleSim *sim)
{
    CHECK(PP(0, 0) == 35 - 2, "tackle lost 2 pp against pressure (pp %d)", PP(0, 0));
}
static void CheckStruggleAfterPressure(struct BattleSim *sim)
{
    // 1 PP tackle vs Pressure: 0 after turn 1, Struggle on turn 2.
    CHECK(PP(0, 0) == 0, "pp drained to 0");
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_STRUGGLE, 1), "struggled on turn 2");
}

// --- end-of-turn ordering ---
static void CheckLeechSeedThenPoison(struct BattleSim *sim)
{
    // Snorlax (enemy) seeded and poisoned: leech seed drains first (1/8), then poison (1/8).
    u16 max = MAXHP(1);
    CHECK(STATUS3(1) & STATUS3_LEECHSEED, "seeded");
    CHECK(STATUS1(1) & STATUS1_POISON, "poisoned");
    // turn 0: seed tick; turn 1: seed tick then poison tick
    CHECK(HP(1) == max - 3 * (max / 8), "three 1/8 ticks over two turns (hp %d/%d)", HP(1), max);
    CHECK(LOG_COUNT(STRINGID_PKMNSAPPEDBYLEECHSEED) == 2, "two leech messages");
    CHECK(LOG_INDEX_T(STRINGID_PKMNSAPPEDBYLEECHSEED, 1, 1) < LOG_INDEX_T(STRINGID_PKMNHURTBYPOISON, 1, 1), "leech seed ticks before poison");
}
static int WantSeedAndPoison(struct BattleSim *sim) { return (STATUS3(1) & STATUS3_LEECHSEED) && (STATUS1(1) & STATUS1_POISON); }

static void CheckIngrainBeforeLeechSeed(struct BattleSim *sim)
{
    // Turn 0: Leech Seed / Ingrain; end: no ingrain heal at full HP, seed -1/8. Turn 1: Seismic Toss 50;
    // end: ingrain +1/16 first, then seed -1/8.
    u16 max = MAXHP(1);
    CHECK(STATUS3(1) & STATUS3_ROOTED, "rooted");
    CHECK(HP(1) == max - max / 8 - 50 + max / 16 - max / 8, "seed, toss, ingrain, seed (hp %d/%d)", HP(1), max);
    CHECK(LOG_INDEX_T(STRINGID_PKMNABSORBEDNUTRIENTS, 1, 1) < LOG_INDEX_T(STRINGID_PKMNSAPPEDBYLEECHSEED, 1, 1), "ingrain heal before leech drain");
}

static const struct Scenario sScenarios[] =
{
    { .name = "speed_tie_player_first",
      .player = { MON(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) }, .enemy = { MON(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .badgesSet = 1, .badges = 0, .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantPlayerFirst, .check = CheckPlayerFirst },
    { .name = "speed_tie_enemy_first",
      .player = { MON(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) }, .enemy = { MON(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .badgesSet = 1, .badges = 0, .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantEnemyFirst, .check = CheckEnemyFirst },
    { .name = "badge_speed_boost",
      .player = { MON(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) }, .enemy = { MON(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckBadgeSpeedBoost },
    { .name = "priority_beats_quick_claw",
      .player = { MON_ITEM(SPECIES_SLOWPOKE, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_QUICK_CLAW) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_QUICK_ATTACK, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantEnemyFirst, .check = CheckPriorityBeatsQuickClaw },
    { .name = "quick_claw_activates",
      .player = { MON_ITEM(SPECIES_SLOWPOKE, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_QUICK_CLAW) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantQuickClaw, .check = CheckQuickClawWorked },
    { .name = "both_switch_index_order",
      .player = { MON(SPECIES_SLOWPOKE, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_PIDGEOT, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_PIDGEY, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(SC_SWITCH(1), SC_SWITCH(1)) }, .turns = 1, .check = CheckBothSwitchOrder },
    { .name = "switch_before_priority_move",
      .player = { MON(SPECIES_SLOWPOKE, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_PIDGEOT, 50, MOVE_QUICK_ATTACK, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(SC_SWITCH(1), 0) }, .turns = 1, .check = CheckSwitchBeforePriorityMove },
    { .name = "negative_priority_last",
      .player = { MON(SPECIES_ARCANINE, 50, MOVE_ROAR, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SLOWPOKE, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckMinusPriorityLast },
    { .name = "replacement_does_not_act",
      .player = { MON(SPECIES_MACHAMP, 100, MOVE_CROSS_CHOP, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 5, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_PIDGEY, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckReplacementDoesNotAct },
    { .name = "draw_double_edge_recoil",
      .player = { { .species = SPECIES_TAUROS, .level = 100, .moves = { MOVE_DOUBLE_EDGE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 1, .hpSet = 1 } },
      .enemy = { MON(SPECIES_RATTATA, 5, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .wantSeed = WantDoubleEdgeHit, .check = CheckDrawRecoil },
    { .name = "explosion_last_mons",
      .player = { MON(SPECIES_ELECTRODE, 100, MOVE_EXPLOSION, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 5, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .check = CheckDrawExplosion },
    { .name = "loss_recoil_target_survives",
      .player = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_DOUBLE_EDGE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 1, .hpSet = 1 } },
      .enemy = { MON(SPECIES_SNORLAX, 100, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .check = CheckLossOnRecoilSurvivor },
    { .name = "win_after_recoil_with_reserve",
      .player = { { .species = SPECIES_TAUROS, .level = 100, .moves = { MOVE_DOUBLE_EDGE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 1, .hpSet = 1 }, MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 5, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .wantSeed = WantDoubleEdgeHit, .check = CheckWinWithReserve },
    { .name = "destiny_bond_last_mons_draw",
      .player = { { .species = SPECIES_GENGAR, .level = 50, .moves = { MOVE_DESTINY_BOND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 1, .hpSet = 1 } },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_BITE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .check = CheckDestinyBondDraw },
    { .name = "perish_song_last_mons_draw",
      .player = { MON(SPECIES_MISDREAVUS, 50, MOVE_PERISH_SONG, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0), T(1, 0), T(1, 0) }, .check = CheckPerishDraw },
    { .name = "outsider_no_badges_disobeys",
      .player = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .otId = 0x1234 } },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .badgesSet = 1, .badges = 0, .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantDisobey, .check = CheckDisobeyed },
    { .name = "outsider_four_badges_l50_obeys",
      .player = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .otId = 0x1234 } },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .badgesSet = 1, .badges = 0x0F, .actions = { T(0, 0) }, .turns = 1, .check = CheckObeyed },
    { .name = "outsider_four_badges_l51_disobeys",
      .player = { { .species = SPECIES_RATTATA, .level = 51, .moves = { MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .otId = 0x1234 } },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .badgesSet = 1, .badges = 0x0F, .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantDisobey, .check = CheckDisobeyed },
    { .name = "outsider_eight_badges_obeys",
      .player = { { .species = SPECIES_RATTATA, .level = 100, .moves = { MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .otId = 0x1234 } },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckObeyed },
    { .name = "own_mon_no_badges_obeys",
      .player = { MON(SPECIES_RATTATA, 100, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .badgesSet = 1, .badges = 0, .actions = { T(0, 0) }, .turns = 1, .check = CheckOwnMonAlwaysObeys },
    { .name = "mew_without_fateful_disobeys",
      .player = { MON(SPECIES_MEW, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantDisobey, .check = CheckDisobeyed },
    { .name = "mew_fateful_obeys",
      .player = { { .species = SPECIES_MEW, .level = 50, .moves = { MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .fateful = 1 } },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckObeyed },
    { .name = "pressure_double_pp",
      .player = { MON(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_ZAPDOS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckPressureDoublePP },
    { .name = "struggle_after_pressure",
      .player = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_TACKLE, MOVE_NONE, MOVE_NONE, MOVE_NONE }, .pp = { 1, 0, 0, 0 }, .ppSet = 1 } },
      .enemy = { MON(SPECIES_ZAPDOS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .check = CheckStruggleAfterPressure },
    { .name = "leech_seed_then_poison",
      .player = { MON(SPECIES_BULBASAUR, 50, MOVE_LEECH_SEED, MOVE_POISON_POWDER, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_AB(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .wantSeed = WantSeedAndPoison, .check = CheckLeechSeedThenPoison },
    { .name = "ingrain_and_leech_seed",
      .player = { MON(SPECIES_BULBASAUR, 50, MOVE_LEECH_SEED, MOVE_SEISMIC_TOSS, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_AB(SPECIES_SNORLAX, 50, MOVE_INGRAIN, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .wantSeed = NULL, .check = CheckIngrainBeforeLeechSeed },
};

SCENARIO_GROUP(engine_edges, sScenarios)
