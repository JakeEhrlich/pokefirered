// Stat stages: self/target stat moves, caps and messages, secondary-effect drops, self-drops, Belly Drum,
// Curse, Swagger/Flatter, multi-stat moves, Psych Up/Haze/Mist, blocking abilities, Intimidate, Substitute,
// White Herb, Baton Pass, switch reset, and the effect of stages on damage / speed / accuracy / crits.
//
// Stage values: DEFAULT_STAT_STAGE = 6, so +2 = 8, -2 = 4, +6 = 12, -6 = 0.
// Level 50, 31 IVs, 0 EVs, Hardy: stat = base + 20, HP = base + 75. The player's mon gets the badge
// boosts (+10% attack/defense/speed/special) in the damage/speed formulas, the enemy does not.
#include "scenario.h"

#define S_ATK STAGE(0, STAT_ATK)
#define E_ATK STAGE(1, STAT_ATK)

// --- damage helpers: replicate CalculateBaseDamage + typecalc for one physical hit ---
static int PlayerSide(int b) { return GetBattlerSide(b) == B_SIDE_PLAYER; }

static int gIgnoreAbility;   // helper switch: compute the "as if no Guts/Hustle" value for contrast checks
static int AtkStat(struct BattleSim *sim, int b)
{
    int a = B(b).attack;
    if (PlayerSide(b))
        a = (110 * a) / 100;
    if (gIgnoreAbility)
        return a;
    if (B(b).ability == ABILITY_HUSTLE)
        a = (150 * a) / 100;
    if (B(b).ability == ABILITY_GUTS && B(b).status1)
        a = (150 * a) / 100;
    return a;
}

static int DefStat(struct BattleSim *sim, int b)
{
    int d = B(b).defense;
    if (PlayerSide(b))
        d = (110 * d) / 100;
    return d;
}

// Expected damage (before the 85-100% random roll) of a physical move `power` from atkB to defB.
// stabX10 = 15 or 10, effX10 = type multiplier * 10 (5, 10, 20).
static int PhysDamage(struct BattleSim *sim, int atkB, int defB, int power, int crit, int stabX10, int effX10)
{
    int a = AtkStat(sim, atkB), d = DefStat(sim, defB);
    int sa = B(atkB).statStages[STAT_ATK], sd = B(defB).statStages[STAT_DEF];
    int dmg, helper;

    if (crit && sa <= DEFAULT_STAT_STAGE)
        dmg = a;
    else
        dmg = a * gStatStageRatios[sa][0] / gStatStageRatios[sa][1];
    dmg = dmg * power;
    dmg *= (2 * B(atkB).level / 5 + 2);
    if (crit && sd >= DEFAULT_STAT_STAGE)
        helper = d;
    else
        helper = d * gStatStageRatios[sd][0] / gStatStageRatios[sd][1];
    dmg = dmg / helper;
    dmg /= 50;
    if ((B(atkB).status1 & STATUS1_BURN) && (gIgnoreAbility || B(atkB).ability != ABILITY_GUTS))
        dmg /= 2;
    if (dmg == 0)
        dmg = 1;
    dmg += 2;
    dmg *= crit ? 2 : 1;
    dmg = dmg * stabX10 / 10;
    dmg = dmg * effX10 / 10;
    if (dmg == 0)
        dmg = 1;
    return dmg;
}

static int InRoll(int dealt, int expected)
{
    int lo = expected * 85 / 100;
    if (lo == 0) lo = 1;
    return dealt >= lo && dealt <= expected;
}

// ---------------------------------------------------------------- self stat ups
static void CheckSwordsDance(struct BattleSim *sim)
{
    CHECK(S_ATK == 8, "swords dance +2 (stage %d)", S_ATK);
    // Self-targeting moves set gBattlerTarget to the user, so the "defender" string table entry is used.
    CHECK(LOG_HAS(STRINGID_DEFENDERSSTATROSE) && !LOG_HAS(STRINGID_ATTACKERSSTATROSE), "stat rose message (defender variant)");
    CHECK(STAGE(1, STAT_ATK) == 6, "target untouched");
}

static void CheckSelfUpCap(struct BattleSim *sim)
{
    // SD x3 reaches +6; the 4th prints "won't go higher" (pp is still used).
    CHECK(S_ATK == 12, "capped at +6 (stage %d)", S_ATK);
    CHECK(LOG_COUNT(STRINGID_DEFENDERSSTATROSE) == 3, "three rises (got %d)", LOG_COUNT(STRINGID_DEFENDERSSTATROSE));
    CHECK(LOG_HAS_T(STRINGID_STATSWONTINCREASE, 3), "won't go higher on turn 4");
    CHECK(!LOG_HAS_T(STRINGID_STATSWONTINCREASE, 2), "no cap message on turn 3");
    CHECK(PP(0, 0) == 26, "pp used on the failed try (pp %d)", PP(0, 0));
}

static void CheckOneStageMoves(struct BattleSim *sim)
{
    CHECK(STAGE(0, STAT_DEF) == 7, "harden +1 def (%d)", STAGE(0, STAT_DEF));
    CHECK(STAGE(0, STAT_SPATK) == 7, "growth +1 spatk (%d)", STAGE(0, STAT_SPATK));
    CHECK(STAGE(0, STAT_ATK) == 7, "meditate +1 atk (%d)", STAGE(0, STAT_ATK));
    CHECK(STAGE(0, STAT_EVASION) == 7, "double team +1 evasion (%d)", STAGE(0, STAT_EVASION));
    CHECK(STAGE(0, STAT_SPEED) == 6 && STAGE(0, STAT_SPDEF) == 6 && STAGE(0, STAT_ACC) == 6, "other stats untouched");
}

static void CheckTwoStageMoves(struct BattleSim *sim)
{
    CHECK(STAGE(0, STAT_SPEED) == 8, "agility +2 (%d)", STAGE(0, STAT_SPEED));
    CHECK(STAGE(0, STAT_SPDEF) == 8, "amnesia +2 (%d)", STAGE(0, STAT_SPDEF));
    CHECK(STAGE(0, STAT_DEF) == 10, "barrier + iron defense = +4 (%d)", STAGE(0, STAT_DEF));
}

static void CheckCapFromPlus5(struct BattleSim *sim)
{
    // +4, +5, then SD (+2) is clamped to +6 with a normal "rose" message.
    CHECK(S_ATK == 12, "clamped at +6 (stage %d)", S_ATK);
    CHECK(LOG_COUNT(STRINGID_DEFENDERSSTATROSE) == 4, "four rise messages (got %d)", LOG_COUNT(STRINGID_DEFENDERSSTATROSE));
    CHECK(!LOG_HAS(STRINGID_STATSWONTINCREASE), "no cap message");
}

static void CheckMinimize(struct BattleSim *sim)
{
    CHECK(STAGE(0, STAT_EVASION) == 7, "minimize +1 evasion (%d)", STAGE(0, STAT_EVASION));
    CHECK(STATUS3(0) & STATUS3_MINIMIZED, "minimized flag");
}

// ---------------------------------------------------------------- target stat downs
static void CheckGrowl(struct BattleSim *sim)
{
    CHECK(E_ATK == 5, "growl -1 (stage %d)", E_ATK);
    CHECK(LOG_HAS(STRINGID_DEFENDERSSTATFELL), "defender's stat fell message");
}

static void CheckCharm(struct BattleSim *sim)
{
    CHECK(E_ATK == 4, "charm -2 (stage %d)", E_ATK);
}

static void CheckTargetDownCap(struct BattleSim *sim)
{
    CHECK(E_ATK == 0, "floored at -6 (stage %d)", E_ATK);
    CHECK(LOG_COUNT(STRINGID_DEFENDERSSTATFELL) == 3, "three falls (got %d)", LOG_COUNT(STRINGID_DEFENDERSSTATFELL));
    CHECK(LOG_HAS_T(STRINGID_STATSWONTDECREASE, 3), "won't go lower on turn 4");
    CHECK(!LOG_HAS_T(STRINGID_STATSWONTDECREASE, 2), "no floor message on turn 3");
}

static void CheckAccEvaTargetMoves(struct BattleSim *sim)
{
    CHECK(STAGE(1, STAT_ACC) == 5, "sand-attack -1 accuracy (%d)", STAGE(1, STAT_ACC));
    CHECK(STAGE(1, STAT_EVASION) == 5, "sweet scent -1 evasion (%d)", STAGE(1, STAT_EVASION));
}

static int WantDown2AllHit(struct BattleSim *sim)
{
    return STAGE(1, STAT_DEF) == 4 && STAGE(1, STAT_SPEED) == 4 && STAGE(1, STAT_SPDEF) == 4;
}
static void CheckDown2Moves(struct BattleSim *sim)
{
    CHECK(STAGE(1, STAT_DEF) == 4, "screech -2 def (%d)", STAGE(1, STAT_DEF));
    CHECK(STAGE(1, STAT_SPEED) == 4, "scary face -2 speed (%d)", STAGE(1, STAT_SPEED));
    CHECK(STAGE(1, STAT_SPDEF) == 4, "fake tears -2 spdef (%d)", STAGE(1, STAT_SPDEF));
    CHECK(LOG_COUNT(STRINGID_DEFENDERSSTATFELL) == 3, "three fell messages");
}

static void CheckStatDownVsSubstitute(struct BattleSim *sim)
{
    CHECK(STATUS2(1) & STATUS2_SUBSTITUTE, "substitute up");
    CHECK(E_ATK == 6, "growl blocked by substitute (stage %d)", E_ATK);
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 1), "but it failed");
    CHECK(PP(0, 0) == 39, "pp still deducted (pp %d)", PP(0, 0));
}

static int WantMoveMissed(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_ATTACKMISSED, SC_ANY_TURN); }
static void CheckStatDownMiss(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_ATTACKMISSED), "flash missed");
    CHECK(STAGE(1, STAT_ACC) == 6, "no drop on a miss (stage %d)", STAGE(1, STAT_ACC));
}

static void CheckStatDownVsProtect(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNPROTECTEDITSELF), "protected itself");
    CHECK(E_ATK == 6, "growl blocked by protect (stage %d)", E_ATK);
}

// ---------------------------------------------------------------- secondary effects
static int WantSpDefDrop(struct BattleSim *sim) { return STAGE(1, STAT_SPDEF) == 5 && HP(1) > 0; }
static void CheckSecondarySpDefDrop(struct BattleSim *sim)
{
    CHECK(STAGE(1, STAT_SPDEF) == 5, "spdef -1 (stage %d)", STAGE(1, STAT_SPDEF));
    CHECK(LOG_HAS(STRINGID_DEFENDERSSTATFELL), "defender's stat fell message");
    CHECK(HP(1) < MAXHP(1), "damage dealt");
}

static int WantAtkDrop(struct BattleSim *sim) { return STAGE(1, STAT_ATK) == 5 && HP(1) > 0; }
static void CheckAuroraBeam(struct BattleSim *sim)
{
    CHECK(E_ATK == 5, "aurora beam -1 atk (stage %d)", E_ATK);
    CHECK(LOG_HAS(STRINGID_DEFENDERSSTATFELL), "fell message");
}

static int WantIcyWindDrop(struct BattleSim *sim) { return STAGE(1, STAT_SPEED) == 5 && HP(1) > 0; }
static void CheckIcyWindOrder(struct BattleSim *sim)
{
    // Rattata (92) outspeeds Lapras (80 * 1.1 = 88) on turn 1; after -1 speed (61) it moves last.
    CHECK(STAGE(1, STAT_SPEED) == 5, "icy wind -1 speed (stage %d)", STAGE(1, STAT_SPEED));
    CHECK(MOVED_BEFORE(1, 0, 0), "rattata first on turn 1");
    CHECK(MOVED_BEFORE(0, 1, 1), "lapras first on turn 2 after the speed drop");
}

static int WantHitNoMiss(struct BattleSim *sim) { return !Sc_LogHas(sim, STRINGID_ATTACKMISSED, SC_ANY_TURN) && HP(1) > 0; }
static void CheckSecondaryVsSubstitute(struct BattleSim *sim)
{
    CHECK(STATUS2(1) & STATUS2_SUBSTITUTE, "substitute still up");
    CHECK(sim->disableStructs[1].substituteHP < MAXHP(1) / 4, "substitute took the hit (sub hp %d)", sim->disableStructs[1].substituteHP);
    CHECK(STAGE(1, STAT_SPEED) == 6, "no speed drop behind a substitute (stage %d)", STAGE(1, STAT_SPEED));
    CHECK(!LOG_HAS(STRINGID_DEFENDERSSTATFELL), "no fell message");
}

static int WantAllStatsUp(struct BattleSim *sim) { return STAGE(0, STAT_ATK) == 7; }
static void CheckAncientPower(struct BattleSim *sim)
{
    CHECK(STAGE(0, STAT_ATK) == 7 && STAGE(0, STAT_DEF) == 7 && STAGE(0, STAT_SPEED) == 7
          && STAGE(0, STAT_SPATK) == 7 && STAGE(0, STAT_SPDEF) == 7, "all five stats +1 (%d %d %d %d %d)",
          STAGE(0, STAT_ATK), STAGE(0, STAT_DEF), STAGE(0, STAT_SPEED), STAGE(0, STAT_SPATK), STAGE(0, STAT_SPDEF));
    CHECK(STAGE(0, STAT_ACC) == 6 && STAGE(0, STAT_EVASION) == 6, "accuracy/evasion untouched");
    CHECK(LOG_COUNT(STRINGID_ATTACKERSSTATROSE) == 5, "five rose messages (got %d)", LOG_COUNT(STRINGID_ATTACKERSSTATROSE));
}

static int WantSilverWindDef(struct BattleSim *sim) { return STAGE(0, STAT_DEF) == 7; }
static void CheckSilverWindPartial(struct BattleSim *sim)
{
    // Attack already +6: it prints "won't go higher" and the other four still rise.
    CHECK(S_ATK == 12, "attack stays +6");
    CHECK(STAGE(0, STAT_DEF) == 7 && STAGE(0, STAT_SPEED) == 7 && STAGE(0, STAT_SPATK) == 7 && STAGE(0, STAT_SPDEF) == 7, "other four +1");
    CHECK(LOG_HAS_T(STRINGID_STATSWONTINCREASE, 3), "won't go higher for attack");
    CHECK(Sc_LogCount(sim, STRINGID_ATTACKERSSTATROSE, 3) == 4, "four rose messages on turn 4 (got %d)", Sc_LogCount(sim, STRINGID_ATTACKERSSTATROSE, 3));
}

static void CheckSuperpower(struct BattleSim *sim)
{
    CHECK(HP(1) > 0 && HP(1) < MAXHP(1), "hit and target survived (hp %d/%d)", HP(1), MAXHP(1));
    CHECK(S_ATK == 5 && STAGE(0, STAT_DEF) == 5, "superpower -1 atk/-1 def (%d %d)", S_ATK, STAGE(0, STAT_DEF));
    CHECK(LOG_COUNT(STRINGID_ATTACKERSSTATFELL) == 2, "two attacker-fell messages (got %d)", LOG_COUNT(STRINGID_ATTACKERSSTATFELL));
}

static void CheckSuperpowerFaintsTarget(struct BattleSim *sim)
{
    CHECK(PARTY_HP(B_SIDE_OPPONENT, 0) == 0, "target fainted");
    CHECK(S_ATK == 5 && STAGE(0, STAT_DEF) == 5, "self-drop still applied (%d %d)", S_ATK, STAGE(0, STAT_DEF));
}

static void CheckSuperpowerVsGhost(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_ITDOESNTAFFECT), "doesn't affect ghost");
    CHECK(S_ATK == 6 && STAGE(0, STAT_DEF) == 6, "no self-drop when the move has no effect (%d %d)", S_ATK, STAGE(0, STAT_DEF));
}

static int WantHit(struct BattleSim *sim) { return HP(1) < MAXHP(1) && HP(1) > 0; }
static void CheckOverheatHit(struct BattleSim *sim)
{
    CHECK(STAGE(0, STAT_SPATK) == 4, "overheat -2 spatk (stage %d)", STAGE(0, STAT_SPATK));
    CHECK(LOG_HAS(STRINGID_ATTACKERSSTATFELL), "attacker's stat fell");
}

static void CheckOverheatMiss(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_ATTACKMISSED), "missed");
    CHECK(STAGE(0, STAT_SPATK) == 6, "no self-drop on a miss (stage %d)", STAGE(0, STAT_SPATK));
}

static void CheckSelfDropIgnoresMistClearBody(struct BattleSim *sim)
{
    CHECK(sim->sideTimers[B_SIDE_PLAYER].mistTimer > 0, "mist active");
    CHECK(B(0).ability == ABILITY_CLEAR_BODY, "clear body user");
    CHECK(S_ATK == 5 && STAGE(0, STAT_DEF) == 5, "certain self-drop goes through mist and clear body (%d %d)", S_ATK, STAGE(0, STAT_DEF));
    CHECK(!LOG_HAS(STRINGID_PKMNPROTECTEDBYMIST) && !LOG_HAS(STRINGID_PKMNPREVENTSSTATLOSSWITH), "no protection messages");
}

static void CheckShieldDust(struct BattleSim *sim)
{
    CHECK(HP(1) < MAXHP(1) && HP(1) > 0, "hit");
    CHECK(STAGE(1, STAT_SPEED) == 6, "shield dust blocks the secondary speed drop (stage %d)", STAGE(1, STAT_SPEED));
    CHECK(!LOG_HAS(STRINGID_DEFENDERSSTATFELL), "no message");
}

// ---------------------------------------------------------------- belly drum
static void CheckBellyDrumOdd(struct BattleSim *sim)
{
    // Snorlax: 235 max HP, cost 235/2 = 117.
    CHECK(MAXHP(0) == 235, "snorlax max hp 235 (got %d)", MAXHP(0));
    CHECK(S_ATK == 12, "attack maxed");
    CHECK(HP(0) == 118, "paid floor(max/2) (hp %d)", HP(0));
    CHECK(LOG_HAS(STRINGID_PKMNCUTHPMAXEDATTACK), "belly drum message");
}

static void CheckBellyDrumAtHalfFails(struct BattleSim *sim)
{
    CHECK(HP(0) == 117, "hp unchanged at exactly half (hp %d)", HP(0));
    CHECK(S_ATK == 6, "no boost");
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "but it failed");
}

static void CheckBellyDrumJustAbove(struct BattleSim *sim)
{
    CHECK(HP(0) == 1, "left with 1 hp (hp %d)", HP(0));
    CHECK(S_ATK == 12, "attack maxed");
}

static void CheckBellyDrumMaxed(struct BattleSim *sim)
{
    CHECK(S_ATK == 12, "attack already maxed");
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 3), "belly drum fails at +6");
    CHECK(HP(0) == MAXHP(0), "no hp paid");
}

// ---------------------------------------------------------------- curse
static void CheckCurseNonGhost(struct BattleSim *sim)
{
    CHECK(S_ATK == 7 && STAGE(0, STAT_DEF) == 7, "+1 atk/+1 def (%d %d)", S_ATK, STAGE(0, STAT_DEF));
    CHECK(STAGE(0, STAT_SPEED) == 5, "-1 speed (%d)", STAGE(0, STAT_SPEED));
    CHECK(HP(0) == MAXHP(0), "no hp cost");
    CHECK(!(STATUS2(1) & STATUS2_CURSED), "target not cursed");
    // gBattlerTarget is set to the attacker for the non-ghost branch, so the "defender" strings are used.
    CHECK(LOG_HAS(STRINGID_DEFENDERSSTATFELL) && LOG_HAS(STRINGID_DEFENDERSSTATROSE), "self stat messages via defender strings");
}

static void CheckCurseGhost(struct BattleSim *sim)
{
    CHECK(STATUS2(1) & STATUS2_CURSED, "target cursed");
    CHECK(HP(0) == MAXHP(0) - MAXHP(0) / 2, "user paid half max hp (hp %d/%d)", HP(0), MAXHP(0));
    CHECK(HP(1) == MAXHP(1) - MAXHP(1) / 4, "target lost 1/4 at end of turn (hp %d/%d)", HP(1), MAXHP(1));
    CHECK(LOG_HAS(STRINGID_PKMNLAIDCURSE), "laid a curse message");
    CHECK(S_ATK == 6 && STAGE(0, STAT_SPEED) == 6, "no stat changes for ghost curse");
}

static void CheckCurseGhostVsSubstitute(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 1), "curse fails on a substitute");
    CHECK(!(STATUS2(1) & STATUS2_CURSED), "not cursed");
    CHECK(HP(0) == MAXHP(0), "no hp paid");
}

static void CheckCurseGhostFaintsUser(struct BattleSim *sim)
{
    CHECK(PARTY_HP(B_SIDE_PLAYER, 0) == 0, "gengar fainted from the curse cost");
    CHECK(STATUS2(1) & STATUS2_CURSED, "target still cursed");
    CHECK(B(0).species == SPECIES_RATTATA, "replacement sent out");
}

static void CheckCurseSpeedIgnoresMist(struct BattleSim *sim)
{
    CHECK(sim->sideTimers[B_SIDE_PLAYER].mistTimer > 0, "mist active");
    CHECK(STAGE(0, STAT_SPEED) == 5, "curse speed drop ignores mist and clear body (%d)", STAGE(0, STAT_SPEED));
    CHECK(S_ATK == 7 && STAGE(0, STAT_DEF) == 7, "boosts applied");
    CHECK(!LOG_HAS(STRINGID_PKMNPROTECTEDBYMIST) && !LOG_HAS(STRINGID_PKMNPREVENTSSTATLOSSWITH), "no protection messages");
}

static void CheckCurseAllMaxed(struct BattleSim *sim)
{
    CHECK(S_ATK == 12 && STAGE(0, STAT_DEF) == 12 && STAGE(0, STAT_SPEED) == 0, "after six curses (%d %d %d)", S_ATK, STAGE(0, STAT_DEF), STAGE(0, STAT_SPEED));
    CHECK(!LOG_HAS_T(STRINGID_BUTITFAILED, 5), "sixth curse still works");
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 6), "seventh curse fails with everything capped");
}

// ---------------------------------------------------------------- swagger / flatter
static int WantSwaggerHit(struct BattleSim *sim) { return STAGE(1, STAT_ATK) == 8; }
static void CheckSwagger(struct BattleSim *sim)
{
    CHECK(E_ATK == 8, "swagger +2 atk (%d)", E_ATK);
    CHECK(STATUS2(1) & STATUS2_CONFUSION, "confused");
    CHECK(LOG_HAS(STRINGID_DEFENDERSSTATROSE), "defender's stat rose");
    CHECK(LOG_HAS(STRINGID_PKMNWASCONFUSED), "was confused message");
}

static void CheckSwaggerOwnTempo(struct BattleSim *sim)
{
    CHECK(E_ATK == 8, "attack still raised (%d)", E_ATK);
    CHECK(!(STATUS2(1) & STATUS2_CONFUSION), "own tempo prevents confusion");
    CHECK(LOG_HAS(STRINGID_PKMNPREVENTSCONFUSIONWITH), "own tempo message");
}

static int WantSwaggerTwiceStillConfused(struct BattleSim *sim)
{
    return STAGE(1, STAT_ATK) == 10 && (STATUS2(1) & STATUS2_CONFUSION) && Sc_LogCount(sim, STRINGID_PKMNWASCONFUSED, SC_ANY_TURN) == 1;
}
static void CheckSwaggerAlreadyConfused(struct BattleSim *sim)
{
    CHECK(E_ATK == 10, "second swagger still raises attack (%d)", E_ATK);
    CHECK(STATUS2(1) & STATUS2_CONFUSION, "still confused");
    CHECK(LOG_COUNT(STRINGID_PKMNWASCONFUSED) == 1, "confused only once");
    CHECK(!LOG_HAS(STRINGID_PKMNALREADYCONFUSED), "no 'already confused' message (silent)");
}

static int WantButItFailed(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_BUTITFAILED, SC_ANY_TURN); }
static void CheckSwaggerConfusedAndMaxed(struct BattleSim *sim)
{
    CHECK(E_ATK == 12, "attack maxed (%d)", E_ATK);
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "swagger fails when confused and attack maxed");
}

static void CheckSwaggerVsSubstitute(struct BattleSim *sim)
{
    CHECK(STATUS2(1) & STATUS2_SUBSTITUTE, "substitute up");
    CHECK(LOG_HAS_T(STRINGID_ATTACKMISSED, 1), "reported as a miss");
    CHECK(E_ATK == 6 && !(STATUS2(1) & STATUS2_CONFUSION), "no boost, no confusion");
}

static void CheckFlatter(struct BattleSim *sim)
{
    CHECK(STAGE(1, STAT_SPATK) == 7, "flatter +1 spatk (%d)", STAGE(1, STAT_SPATK));
    CHECK(STATUS2(1) & STATUS2_CONFUSION, "confused");
}

// ---------------------------------------------------------------- multi-stat moves
static void CheckTickle(struct BattleSim *sim)
{
    CHECK(E_ATK == 5 && STAGE(1, STAT_DEF) == 5, "tickle -1 atk/-1 def (%d %d)", E_ATK, STAGE(1, STAT_DEF));
    CHECK(LOG_COUNT(STRINGID_DEFENDERSSTATFELL) == 2, "two fell messages");
}

static void CheckTicklePartialMin(struct BattleSim *sim)
{
    CHECK(E_ATK == 0, "attack stays -6");
    CHECK(STAGE(1, STAT_DEF) == 5, "defense still drops (%d)", STAGE(1, STAT_DEF));
    CHECK(!LOG_HAS_T(STRINGID_STATSWONTDECREASE, 3), "attack floor is silent for tickle");
}

static void CheckTickleVsClearBody(struct BattleSim *sim)
{
    CHECK(E_ATK == 6 && STAGE(1, STAT_DEF) == 6, "both blocked");
    CHECK(LOG_COUNT(STRINGID_PKMNPREVENTSSTATLOSSWITH) == 1, "clear body message printed once (got %d)", LOG_COUNT(STRINGID_PKMNPREVENTSSTATLOSSWITH));
}

static void CheckTickleVsSubstitute(struct BattleSim *sim)
{
    // Tickle's script has no substitute check: it lowers the stats through the substitute.
    CHECK(STATUS2(1) & STATUS2_SUBSTITUTE, "substitute up");
    CHECK(E_ATK == 5 && STAGE(1, STAT_DEF) == 5, "tickle goes through substitute (%d %d)", E_ATK, STAGE(1, STAT_DEF));
}

static void CheckMultiStatUps(struct BattleSim *sim)
{
    CHECK(S_ATK == 8, "bulk up + dragon dance attack (%d)", S_ATK);
    CHECK(STAGE(0, STAT_DEF) == 8, "bulk up + cosmic power defense (%d)", STAGE(0, STAT_DEF));
    CHECK(STAGE(0, STAT_SPEED) == 7, "dragon dance speed (%d)", STAGE(0, STAT_SPEED));
    CHECK(STAGE(0, STAT_SPATK) == 7, "calm mind spatk (%d)", STAGE(0, STAT_SPATK));
    CHECK(STAGE(0, STAT_SPDEF) == 8, "calm mind + cosmic power spdef (%d)", STAGE(0, STAT_SPDEF));
    CHECK(LOG_COUNT(STRINGID_DEFENDERSSTATROSE) == 8, "eight rose messages (got %d)", LOG_COUNT(STRINGID_DEFENDERSSTATROSE));
}

static void CheckBulkUpPartial(struct BattleSim *sim)
{
    CHECK(S_ATK == 12, "attack stays +6");
    CHECK(STAGE(0, STAT_DEF) == 7, "defense still +1 (%d)", STAGE(0, STAT_DEF));
    CHECK(!LOG_HAS_T(STRINGID_STATSWONTINCREASE, 3), "capped attack is silent for bulk up");
    CHECK(!LOG_HAS(STRINGID_STATSWONTINCREASE2), "no double-cap message");
}

static void CheckBulkUpBothMaxed(struct BattleSim *sim)
{
    CHECK(S_ATK == 12 && STAGE(0, STAT_DEF) == 12, "both maxed");
    CHECK(LOG_HAS_T(STRINGID_STATSWONTINCREASE2, 6), "stats won't go any higher");
}

static void CheckDragonDanceOrder(struct BattleSim *sim)
{
    // Tauros (130) beats Gyarados (101 * 1.1 = 111) on turn 1; +1 speed (101 * 1.5 * 1.1 = 166) flips it.
    // Both leads have Intimidate: Gyarados starts at -1 attack, Dragon Dance brings it back to 0.
    CHECK(STAGE(0, STAT_SPEED) == 7 && S_ATK == 6 && E_ATK == 5, "dragon dance stages after mutual intimidate (%d %d %d)", STAGE(0, STAT_SPEED), S_ATK, E_ATK);
    CHECK(MOVED_BEFORE(1, 0, 0), "tauros first on turn 1");
    CHECK(MOVED_BEFORE(0, 1, 1), "gyarados first on turn 2");
}

// ---------------------------------------------------------------- psych up / haze / mist
static void CheckPsychUp(struct BattleSim *sim)
{
    CHECK(E_ATK == 8, "enemy +2 atk");
    CHECK(S_ATK == 8, "copied +2 atk (%d)", S_ATK);
    CHECK(STAGE(0, STAT_DEF) == 6, "own +1 def overwritten by the copy (%d)", STAGE(0, STAT_DEF));
    CHECK(LOG_HAS(STRINGID_PKMNCOPIEDSTATCHANGES), "copied message");
}

static void CheckPsychUpVsSubstitute(struct BattleSim *sim)
{
    CHECK(STATUS2(1) & STATUS2_SUBSTITUTE, "substitute up");
    CHECK(S_ATK == 8, "psych up works through a substitute (%d)", S_ATK);
}

static void CheckHaze(struct BattleSim *sim)
{
    CHECK(S_ATK == 6 && STAGE(1, STAT_SPEED) == 6, "both sides reset (%d %d)", S_ATK, STAGE(1, STAT_SPEED));
    CHECK(LOG_HAS(STRINGID_STATCHANGESGONE), "stat changes eliminated");
}

static void CheckHazeThroughProtect(struct BattleSim *sim)
{
    // Haze has no accuracy check, so Protect never gets to say "protected itself".
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_PROTECT, 1) && MOVED_BEFORE(1, 0, 1), "enemy protected first");
    CHECK(!LOG_HAS_T(STRINGID_PKMNPROTECTEDITSELF, 1), "no protect message against haze");
    CHECK(E_ATK == 6, "haze resets a protected foe (%d)", E_ATK);
    CHECK(LOG_HAS_T(STRINGID_STATCHANGESGONE, 1), "haze message");
}

static void CheckMistDuration(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNSHROUDEDINMIST, 0), "mist set");
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 1), "second mist fails");
    CHECK(LOG_COUNT(STRINGID_PKMNPROTECTEDBYMIST) == 4, "growl blocked on turns 2-5 (got %d)", LOG_COUNT(STRINGID_PKMNPROTECTEDBYMIST));
    CHECK(!LOG_HAS_T(STRINGID_PKMNPROTECTEDBYMIST, 5), "mist gone on turn 6");
    CHECK(S_ATK == 5, "growl works after mist expires (%d)", S_ATK);
    CHECK(sim->sideTimers[B_SIDE_PLAYER].mistTimer == 0 && !(SIDE_STATUS(B_SIDE_PLAYER) & SIDE_STATUS_MIST), "mist timer/status cleared");
}

static void CheckMistBlocksSecondary(struct BattleSim *sim)
{
    CHECK(HP(0) < MAXHP(0), "icy wind hit");
    CHECK(STAGE(0, STAT_SPEED) == 6, "secondary drop blocked (%d)", STAGE(0, STAT_SPEED));
    CHECK(!LOG_HAS(STRINGID_PKMNPROTECTEDBYMIST), "silent (no mist message for secondary effects)");
}
static int WantPlayerHit(struct BattleSim *sim) { return HP(0) < MAXHP(0); }

static void CheckMistBlocksIntimidate(struct BattleSim *sim)
{
    CHECK(B(1).species == SPECIES_GYARADOS, "gyarados switched in");
    CHECK(S_ATK == 6, "intimidate blocked by mist (%d)", S_ATK);
    CHECK(LOG_HAS_T(STRINGID_PKMNPROTECTEDBYMIST, 1), "mist message");
}

// ---------------------------------------------------------------- abilities
static void CheckClearBodyGrowl(struct BattleSim *sim)
{
    CHECK(E_ATK == 6, "clear body blocks growl");
    CHECK(LOG_HAS(STRINGID_PKMNPREVENTSSTATLOSSWITH), "prevents stat loss message");
}

static void CheckWhiteSmokeCharm(struct BattleSim *sim)
{
    CHECK(E_ATK == 6, "white smoke blocks charm");
    CHECK(LOG_HAS(STRINGID_PKMNPREVENTSSTATLOSSWITH), "prevents stat loss message");
}

static void CheckHyperCutter(struct BattleSim *sim)
{
    CHECK(E_ATK == 6, "hyper cutter blocks growl");
    CHECK(LOG_HAS_T(STRINGID_PKMNSXPREVENTSYLOSS, 0), "specific stat loss message");
    CHECK(STAGE(1, STAT_DEF) == 5, "tail whip still works (%d)", STAGE(1, STAT_DEF));
}

static void CheckKeenEye(struct BattleSim *sim)
{
    CHECK(STAGE(1, STAT_ACC) == 6, "keen eye blocks sand-attack");
    CHECK(LOG_HAS_T(STRINGID_PKMNSXPREVENTSYLOSS, 0), "specific stat loss message");
    CHECK(E_ATK == 5, "growl still works (%d)", E_ATK);
}

static void CheckClearBodySecondary(struct BattleSim *sim)
{
    CHECK(HP(1) < MAXHP(1), "hit");
    CHECK(STAGE(1, STAT_SPEED) == 6, "clear body blocks the secondary drop");
    CHECK(!LOG_HAS(STRINGID_PKMNPREVENTSSTATLOSSWITH), "silent for secondary effects");
}

static void CheckIntimidate(struct BattleSim *sim)
{
    CHECK(E_ATK == 5, "intimidate -1 atk (%d)", E_ATK);
    CHECK(LOG_HAS(STRINGID_PKMNCUTSATTACKWITH), "cuts attack message");
    CHECK(S_ATK == 6, "user unaffected");
}

static void CheckIntimidateBlockedByAbility(struct BattleSim *sim)
{
    CHECK(E_ATK == 6, "intimidate blocked (%d)", E_ATK);
    CHECK(LOG_HAS(STRINGID_PREVENTEDFROMWORKING), "prevented from working");
    CHECK(!LOG_HAS(STRINGID_PKMNCUTSATTACKWITH), "no cut message");
}

static void CheckIntimidateVsKeenEye(struct BattleSim *sim)
{
    CHECK(E_ATK == 5, "keen eye does not stop intimidate (%d)", E_ATK);
}

static void CheckIntimidateVsSubstitute(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_GYARADOS, "gyarados in");
    CHECK(STATUS2(1) & STATUS2_SUBSTITUTE, "substitute up");
    CHECK(E_ATK == 6, "substitute blocks intimidate (%d)", E_ATK);
    CHECK(!LOG_HAS(STRINGID_PKMNCUTSATTACKWITH) && !LOG_HAS(STRINGID_PREVENTEDFROMWORKING), "silent");
}

static void CheckIntimidateBothLeads(struct BattleSim *sim)
{
    CHECK(S_ATK == 5 && E_ATK == 5, "both leads intimidated (%d %d)", S_ATK, E_ATK);
    CHECK(LOG_COUNT(STRINGID_PKMNCUTSATTACKWITH) == 2, "two messages");
}

// ---------------------------------------------------------------- white herb
static void CheckWhiteHerbGrowl(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_DEFENDERSSTATFELL), "growl landed first");
    CHECK(S_ATK == 6, "white herb restored attack (%d)", S_ATK);
    CHECK(B(0).item == ITEM_NONE, "white herb consumed");
    CHECK(LOG_HAS(STRINGID_PKMNSITEMRESTOREDSTATUS), "item restored status message");
}

static void CheckWhiteHerbSuperpower(struct BattleSim *sim)
{
    CHECK(S_ATK == 6 && STAGE(0, STAT_DEF) == 6, "self-drops restored (%d %d)", S_ATK, STAGE(0, STAT_DEF));
    CHECK(B(0).item == ITEM_NONE, "white herb consumed");
}

static void CheckWhiteHerbIntimidate(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNCUTSATTACKWITH), "intimidate fired");
    CHECK(S_ATK == 6, "white herb restored attack at switch-in (%d)", S_ATK);
    CHECK(B(0).item == ITEM_NONE, "white herb consumed");
}

// ---------------------------------------------------------------- baton pass / switch
static void CheckBatonPassStages(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_RATTATA, "rattata received the pass");
    CHECK(STAGE(0, STAT_DEF) == 7 && STAGE(0, STAT_SPEED) == 8, "stages carried (%d %d)", STAGE(0, STAT_DEF), STAGE(0, STAT_SPEED));
    CHECK(!(STATUS2(0) & STATUS2_DEFENSE_CURL), "defense curl flag not carried");
}

static void CheckSwitchResets(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_SNORLAX, "snorlax back in");
    CHECK(S_ATK == 6, "stages reset by switching (%d)", S_ATK);
}

// ---------------------------------------------------------------- damage / crits / speed / accuracy
static int WantNoCrit(struct BattleSim *sim) { return !Sc_LogHas(sim, STRINGID_CRITICALHIT, SC_ANY_TURN) && HP(1) > 0 && HP(1) < MAXHP(1); }
static int WantNoCritNoMiss(struct BattleSim *sim) { return WantNoCrit(sim) && !Sc_LogHas(sim, STRINGID_ATTACKMISSED, SC_ANY_TURN); }

static void CheckAttackPlus2Damage(struct BattleSim *sim)
{
    int dealt = MAXHP(1) - HP(1), exp = PhysDamage(sim, 0, 1, 40, 0, 15, 10);
    CHECK(S_ATK == 8, "attack +2");
    CHECK(InRoll(dealt, exp), "pound at +2 attack: dealt %d, expected %d..%d", dealt, exp * 85 / 100, exp);
}

static int WantDefDrop2NoCrit(struct BattleSim *sim) { return STAGE(1, STAT_DEF) == 4 && WantNoCrit(sim); }
static void CheckDefenseMinus2Damage(struct BattleSim *sim)
{
    int dealt = MAXHP(1) - HP(1), exp = PhysDamage(sim, 0, 1, 40, 0, 15, 10);
    CHECK(STAGE(1, STAT_DEF) == 4, "defense -2");
    CHECK(InRoll(dealt, exp), "pound at -2 defense: dealt %d, expected %d..%d", dealt, exp * 85 / 100, exp);
}

static int WantCritTurn1(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_CRITICALHIT, 1) && HP(1) > 0; }
static int WantCritTurn2(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_CRITICALHIT, 2) && HP(1) > 0; }
static void CheckCritIgnoresNegativeAttack(struct BattleSim *sim)
{
    // Slash (Normal) on Steelix: STAB 1.5, resisted 0.5; the -2 attack stage is ignored on a crit.
    int dealt = MAXHP(1) - HP(1), exp = PhysDamage(sim, 0, 1, 70, 1, 15, 5);
    int withStage = PhysDamage(sim, 0, 1, 70, 0, 15, 5) * 2;   // what a crit would do if the -2 stage counted
    CHECK(S_ATK == 4, "charmed to -2");
    CHECK(InRoll(dealt, exp), "crit ignores -2 attack: dealt %d, expected %d..%d (with stage: <=%d)", dealt, exp * 85 / 100, exp, withStage);
    CHECK(dealt > withStage, "more than a stage-respecting crit could do (%d > %d)", dealt, withStage);
}

static void CheckCritIgnoresPositiveDefense(struct BattleSim *sim)
{
    int dealt = MAXHP(1) - HP(1), exp = PhysDamage(sim, 0, 1, 70, 1, 15, 5);
    int withStage = PhysDamage(sim, 0, 1, 70, 0, 15, 5) * 2;
    CHECK(STAGE(1, STAT_DEF) == 8, "iron defense +2");
    CHECK(InRoll(dealt, exp), "crit ignores +2 defense: dealt %d, expected %d..%d", dealt, exp * 85 / 100, exp);
    CHECK(dealt > withStage, "more than a stage-respecting crit could do (%d > %d)", dealt, withStage);
}

static void CheckCritKeepsPositiveAttack(struct BattleSim *sim)
{
    int dealt = MAXHP(1) - HP(1), exp = PhysDamage(sim, 0, 1, 70, 1, 15, 5);
    int s = B(0).statStages[STAT_ATK], ignored;
    B(0).statStages[STAT_ATK] = 6; ignored = PhysDamage(sim, 0, 1, 70, 1, 15, 5); B(0).statStages[STAT_ATK] = s;
    CHECK(S_ATK == 8, "swords dance +2");
    CHECK(InRoll(dealt, exp), "crit keeps +2 attack: dealt %d, expected %d..%d", dealt, exp * 85 / 100, exp);
    CHECK(dealt > ignored, "more than a crit at +0 attack (%d > %d)", dealt, ignored);
}

static void CheckGutsWithStages(struct BattleSim *sim)
{
    int dealt = MAXHP(1) - HP(1), exp = PhysDamage(sim, 0, 1, 40, 0, 10, 5), noGuts;
    gIgnoreAbility = 1; noGuts = PhysDamage(sim, 0, 1, 40, 0, 10, 5); gIgnoreAbility = 0;
    CHECK(STATUS1(0) & STATUS1_BURN, "burned");
    CHECK(S_ATK == 7, "bulk up +1");
    CHECK(InRoll(dealt, exp), "guts x1.5 then +1 stage, no burn halving: dealt %d, expected %d..%d", dealt, exp * 85 / 100, exp);
    CHECK(dealt > noGuts, "more than a burned non-guts attacker (%d > %d)", dealt, noGuts);
}

static void CheckHustleWithStages(struct BattleSim *sim)
{
    int dealt = MAXHP(1) - HP(1), exp = PhysDamage(sim, 0, 1, 40, 0, 15, 5), noHustle;
    gIgnoreAbility = 1; noHustle = PhysDamage(sim, 0, 1, 40, 0, 15, 5); gIgnoreAbility = 0;
    CHECK(B(0).ability == ABILITY_HUSTLE, "hustle");
    CHECK(S_ATK == 7, "meditate +1");
    CHECK(InRoll(dealt, exp), "hustle x1.5 then +1 stage: dealt %d, expected %d..%d", dealt, exp * 85 / 100, exp);
    CHECK(dealt > noHustle * 85 / 100 && exp > noHustle, "hustle value distinguishable (%d vs no-hustle %d)", dealt, noHustle);
}

static void CheckAgilityOrder(struct BattleSim *sim)
{
    // Snorlax 50 * 1.1 = 55 < Rattata 92; at +2: 100 * 1.1 = 110 > 92.
    CHECK(STAGE(0, STAT_SPEED) == 8, "agility +2");
    CHECK(MOVED_BEFORE(1, 0, 0), "rattata first on turn 1");
    CHECK(MOVED_BEFORE(0, 1, 1), "snorlax first on turn 2");
}

static int WantMissTurn6(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_ATTACKMISSED, 6); }
static void CheckEvasionPlus6(struct BattleSim *sim)
{
    CHECK(STAGE(0, STAT_EVASION) == 12, "evasion +6");
    CHECK(LOG_HAS_T(STRINGID_ATTACKMISSED, 6), "pound (100%%) can miss at +6 evasion (33%%)");
}

static void CheckAccuracyMinus6(struct BattleSim *sim)
{
    CHECK(STAGE(1, STAT_ACC) == 0, "accuracy -6");
    CHECK(LOG_HAS_T(STRINGID_ATTACKMISSED, 6), "pound can miss at -6 accuracy");
}

static int WantForesightLanded(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_PKMNIDENTIFIED, 6); }
static void CheckForesightIgnoresEvasion(struct BattleSim *sim)
{
    CHECK(STAGE(0, STAT_EVASION) == 12, "evasion +6");
    CHECK(STATUS2(0) & STATUS2_FORESIGHT, "identified");
    CHECK(!LOG_HAS_T(STRINGID_ATTACKMISSED, 7), "pound cannot miss once foresight ignores evasion");
    CHECK(HP(0) < MAXHP(0), "pound hit");
}

static void CheckGrowlDoubles(struct BattleSim *sim)
{
    CHECK(STAGE(1, STAT_ATK) == 5 && STAGE(3, STAT_ATK) == 5, "growl lowers both foes (%d %d)", STAGE(1, STAT_ATK), STAGE(3, STAT_ATK));
    CHECK(STAGE(2, STAT_ATK) == 6, "partner untouched");
}

static int WantDefUp(struct BattleSim *sim) { return STAGE(0, STAT_DEF) == 7; }
static void CheckSteelWingDefUp(struct BattleSim *sim)
{
    CHECK(STAGE(0, STAT_DEF) == 7, "steel wing +1 def");
    CHECK(LOG_HAS(STRINGID_ATTACKERSSTATROSE), "attacker's stat rose");
}

#define RATTATA_SPLASH MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH)
#define CHANSEY_SPLASH MON(SPECIES_CHANSEY, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH)
#define STEELIX60_SPLASH MON(SPECIES_STEELIX, 60, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH)

static const struct Scenario sScenarios[] =
{
    // --- self stat ups
    { .name = "swords_dance_plus2",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SWORDS_DANCE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { RATTATA_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSwordsDance },
    { .name = "self_up_cap_message",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SWORDS_DANCE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { RATTATA_SPLASH },
      .actions = { T(0, 0), T(0, 0), T(0, 0), T(0, 0) }, .turns = 4, .check = CheckSelfUpCap },
    { .name = "one_stage_self_moves",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_HARDEN, MOVE_GROWTH, MOVE_MEDITATE, MOVE_DOUBLE_TEAM) },
      .enemy = { RATTATA_SPLASH },
      .actions = { T(0, 0), T(1, 0), T(2, 0), T(3, 0) }, .turns = 4, .check = CheckOneStageMoves },
    { .name = "two_stage_self_moves",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_AGILITY, MOVE_AMNESIA, MOVE_BARRIER, MOVE_IRON_DEFENSE) },
      .enemy = { RATTATA_SPLASH },
      .actions = { T(0, 0), T(1, 0), T(2, 0), T(3, 0) }, .turns = 4, .check = CheckTwoStageMoves },
    { .name = "self_up_clamped_from_plus5",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SWORDS_DANCE, MOVE_MEDITATE, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { RATTATA_SPLASH },
      .actions = { T(0, 0), T(0, 0), T(1, 0), T(0, 0) }, .turns = 4, .check = CheckCapFromPlus5 },
    { .name = "minimize_evasion",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_MINIMIZE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { RATTATA_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckMinimize },

    // --- target stat downs
    { .name = "growl_minus1",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_GROWL, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { RATTATA_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckGrowl },
    { .name = "charm_minus2",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_CHARM, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { RATTATA_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckCharm },
    { .name = "target_down_cap_message",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_CHARM, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { RATTATA_SPLASH },
      .actions = { T(0, 0), T(0, 0), T(0, 0), T(0, 0) }, .turns = 4, .check = CheckTargetDownCap },
    { .name = "accuracy_evasion_target_moves",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SAND_ATTACK, MOVE_SWEET_SCENT, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { RATTATA_SPLASH },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckAccEvaTargetMoves },
    { .name = "two_stage_target_moves",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SCREECH, MOVE_SCARY_FACE, MOVE_FAKE_TEARS, MOVE_SPLASH) },
      .enemy = { RATTATA_SPLASH },
      .actions = { T(0, 0), T(1, 0), T(2, 0) }, .turns = 3, .wantSeed = WantDown2AllHit, .check = CheckDown2Moves },
    { .name = "stat_down_vs_substitute",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_GROWL, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SUBSTITUTE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(1, 0), T(0, 1) }, .turns = 2, .check = CheckStatDownVsSubstitute },
    { .name = "stat_down_can_miss",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_FLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { RATTATA_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantMoveMissed, .check = CheckStatDownMiss },
    { .name = "stat_down_vs_protect",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_GROWL, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_PROTECT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckStatDownVsProtect },

    // --- secondary effects
    { .name = "psychic_spdef_drop",
      .player = { MON(SPECIES_ALAKAZAM, 50, MOVE_PSYCHIC, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantSpDefDrop, .check = CheckSecondarySpDefDrop },
    { .name = "crunch_spdef_drop",
      .player = { MON(SPECIES_UMBREON, 50, MOVE_CRUNCH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantSpDefDrop, .check = CheckSecondarySpDefDrop },
    { .name = "aurora_beam_atk_drop",
      .player = { MON(SPECIES_LAPRAS, 50, MOVE_AURORA_BEAM, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantAtkDrop, .check = CheckAuroraBeam },
    { .name = "icy_wind_speed_drop_order",
      .player = { MON(SPECIES_LAPRAS, 50, MOVE_ICY_WIND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { RATTATA_SPLASH },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .wantSeed = WantIcyWindDrop, .check = CheckIcyWindOrder },
    { .name = "secondary_drop_vs_substitute",
      .player = { MON(SPECIES_LAPRAS, 50, MOVE_ICY_WIND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SUBSTITUTE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(1, 0), T(0, 1) }, .turns = 2, .wantSeed = WantHitNoMiss, .check = CheckSecondaryVsSubstitute },
    { .name = "ancient_power_all_stats_up",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_ANCIENT_POWER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { CHANSEY_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantAllStatsUp, .check = CheckAncientPower },
    { .name = "silver_wind_with_attack_maxed",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SWORDS_DANCE, MOVE_SILVER_WIND, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { CHANSEY_SPLASH },
      .actions = { T(0, 0), T(0, 0), T(0, 0), T(1, 0) }, .turns = 4, .wantSeed = WantSilverWindDef, .check = CheckSilverWindPartial },
    { .name = "superpower_self_drop",
      .player = { MON(SPECIES_MACHOP, 50, MOVE_SUPERPOWER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { STEELIX60_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSuperpower },
    { .name = "superpower_target_faints_still_drops",
      .player = { MON(SPECIES_MACHAMP, 50, MOVE_SUPERPOWER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { RATTATA_SPLASH, RATTATA_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSuperpowerFaintsTarget },
    { .name = "superpower_no_effect_no_drop",
      .player = { MON(SPECIES_MACHAMP, 50, MOVE_SUPERPOWER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_GENGAR, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSuperpowerVsGhost },
    { .name = "overheat_self_drop",
      .player = { MON(SPECIES_ARCANINE, 50, MOVE_OVERHEAT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_BLISSEY, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantHit, .check = CheckOverheatHit },
    { .name = "overheat_miss_no_drop",
      .player = { MON(SPECIES_ARCANINE, 50, MOVE_OVERHEAT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_BLISSEY, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantMoveMissed, .check = CheckOverheatMiss },
    { .name = "self_drop_ignores_mist_and_clear_body",
      .player = { MON(SPECIES_METANG, 50, MOVE_MIST, MOVE_SUPERPOWER, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { STEELIX60_SPLASH },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckSelfDropIgnoresMistClearBody },
    { .name = "shield_dust_blocks_secondary_drop",
      .player = { MON(SPECIES_LAPRAS, 50, MOVE_ICY_WIND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_VENOMOTH, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantHit, .check = CheckShieldDust },

    // --- belly drum
    { .name = "belly_drum_odd_max_hp",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_BELLY_DRUM, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { RATTATA_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckBellyDrumOdd },
    { .name = "belly_drum_at_half_fails",
      .player = { { .species = SPECIES_SNORLAX, .level = 50, .moves = { MOVE_BELLY_DRUM, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 117, .hpSet = 1 } },
      .enemy = { RATTATA_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckBellyDrumAtHalfFails },
    { .name = "belly_drum_just_above_half",
      .player = { { .species = SPECIES_SNORLAX, .level = 50, .moves = { MOVE_BELLY_DRUM, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 118, .hpSet = 1 } },
      .enemy = { RATTATA_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckBellyDrumJustAbove },
    { .name = "belly_drum_already_maxed",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SWORDS_DANCE, MOVE_BELLY_DRUM, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { RATTATA_SPLASH },
      .actions = { T(0, 0), T(0, 0), T(0, 0), T(1, 0) }, .turns = 4, .check = CheckBellyDrumMaxed },

    // --- curse
    { .name = "curse_non_ghost",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_CURSE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { RATTATA_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckCurseNonGhost },
    { .name = "curse_ghost",
      .player = { MON(SPECIES_GENGAR, 50, MOVE_CURSE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckCurseGhost },
    { .name = "curse_ghost_vs_substitute",
      .player = { MON(SPECIES_GENGAR, 50, MOVE_CURSE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SUBSTITUTE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(1, 0), T(0, 1) }, .turns = 2, .check = CheckCurseGhostVsSubstitute },
    { .name = "curse_ghost_user_faints",
      .player = { { .species = SPECIES_GENGAR, .level = 50, .moves = { MOVE_CURSE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 10, .hpSet = 1 }, RATTATA_SPLASH },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckCurseGhostFaintsUser },
    { .name = "curse_speed_drop_ignores_mist_clear_body",
      .player = { MON(SPECIES_METANG, 50, MOVE_MIST, MOVE_CURSE, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { RATTATA_SPLASH },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckCurseSpeedIgnoresMist },
    { .name = "curse_fails_when_all_capped",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_CURSE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { RATTATA_SPLASH },
      .actions = { T(0, 0), T(0, 0), T(0, 0), T(0, 0), T(0, 0), T(0, 0), T(0, 0) }, .turns = 7, .check = CheckCurseAllMaxed },

    // --- swagger / flatter
    { .name = "swagger_basic",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SWAGGER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { RATTATA_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantSwaggerHit, .check = CheckSwagger },
    { .name = "swagger_vs_own_tempo",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SWAGGER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_AB(SPECIES_SLOWBRO, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantSwaggerHit, .check = CheckSwaggerOwnTempo },
    { .name = "swagger_already_confused",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SWAGGER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { RATTATA_SPLASH },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .wantSeed = WantSwaggerTwiceStillConfused, .check = CheckSwaggerAlreadyConfused },
    { .name = "swagger_confused_and_maxed_fails",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SWAGGER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { RATTATA_SPLASH },
      .actions = { T(0, 0), T(0, 0), T(0, 0), T(0, 0) }, .turns = 4, .wantSeed = WantButItFailed, .check = CheckSwaggerConfusedAndMaxed },
    { .name = "swagger_vs_substitute",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SWAGGER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SUBSTITUTE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(1, 0), T(0, 1) }, .turns = 2, .check = CheckSwaggerVsSubstitute },
    { .name = "flatter_basic",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_FLATTER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { RATTATA_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckFlatter },

    // --- multi-stat moves
    { .name = "tickle",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_TICKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { RATTATA_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckTickle },
    { .name = "tickle_attack_already_min",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_CHARM, MOVE_TICKLE, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { RATTATA_SPLASH },
      .actions = { T(0, 0), T(0, 0), T(0, 0), T(1, 0) }, .turns = 4, .check = CheckTicklePartialMin },
    { .name = "tickle_vs_clear_body",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_TICKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_METAGROSS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckTickleVsClearBody },
    { .name = "tickle_through_substitute",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_TICKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SUBSTITUTE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(1, 0), T(0, 1) }, .turns = 2, .check = CheckTickleVsSubstitute },
    { .name = "bulk_up_calm_mind_dragon_dance_cosmic_power",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_BULK_UP, MOVE_CALM_MIND, MOVE_DRAGON_DANCE, MOVE_COSMIC_POWER) },
      .enemy = { RATTATA_SPLASH },
      .actions = { T(0, 0), T(1, 0), T(2, 0), T(3, 0) }, .turns = 4, .check = CheckMultiStatUps },
    { .name = "bulk_up_attack_already_maxed",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SWORDS_DANCE, MOVE_BULK_UP, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { RATTATA_SPLASH },
      .actions = { T(0, 0), T(0, 0), T(0, 0), T(1, 0) }, .turns = 4, .check = CheckBulkUpPartial },
    { .name = "bulk_up_both_maxed_fails",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SWORDS_DANCE, MOVE_IRON_DEFENSE, MOVE_BULK_UP, MOVE_SPLASH) },
      .enemy = { RATTATA_SPLASH },
      .actions = { T(0, 0), T(0, 0), T(0, 0), T(1, 0), T(1, 0), T(1, 0), T(2, 0) }, .turns = 7, .check = CheckBulkUpBothMaxed },
    { .name = "dragon_dance_speed_order",
      .player = { MON(SPECIES_GYARADOS, 50, MOVE_DRAGON_DANCE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_TAUROS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckDragonDanceOrder },

    // --- psych up / haze / mist
    { .name = "psych_up_copies_all_stages",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_HARDEN, MOVE_PSYCH_UP, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SWORDS_DANCE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .check = CheckPsychUp },
    { .name = "psych_up_through_substitute",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_PSYCH_UP, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SUBSTITUTE, MOVE_SWORDS_DANCE, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(1, 0), T(1, 1), T(0, 2) }, .turns = 3, .check = CheckPsychUpVsSubstitute },
    { .name = "haze_resets_both_sides",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SWORDS_DANCE, MOVE_HAZE, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_AGILITY, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .check = CheckHaze },
    { .name = "haze_through_protect",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_HAZE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SWORDS_DANCE, MOVE_PROTECT, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(1, 0), T(0, 1) }, .turns = 2, .check = CheckHazeThroughProtect },
    { .name = "mist_five_turns_and_refail",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_MIST, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_GROWL, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 1), T(0, 0), T(1, 0), T(1, 0), T(1, 0), T(1, 0) }, .turns = 6, .check = CheckMistDuration },
    { .name = "mist_blocks_secondary_drop_silently",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_MIST, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_LAPRAS, 50, MOVE_ICY_WIND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 1), T(1, 0) }, .turns = 2, .wantSeed = WantPlayerHit, .check = CheckMistBlocksSecondary },
    { .name = "mist_blocks_intimidate",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_MIST, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { RATTATA_SPLASH, MON(SPECIES_GYARADOS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, SC_SWITCH(1)) }, .turns = 2, .check = CheckMistBlocksIntimidate },

    // --- blocking abilities
    { .name = "clear_body_blocks_growl",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_GROWL, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_METAGROSS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckClearBodyGrowl },
    { .name = "white_smoke_blocks_charm",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_CHARM, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_TORKOAL, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckWhiteSmokeCharm },
    { .name = "hyper_cutter_only_attack",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_GROWL, MOVE_TAIL_WHIP, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_PINSIR, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckHyperCutter },
    { .name = "keen_eye_only_accuracy",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SAND_ATTACK, MOVE_GROWL, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_PIDGEOT, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckKeenEye },
    { .name = "clear_body_blocks_secondary_silently",
      .player = { MON(SPECIES_LAPRAS, 50, MOVE_ICY_WIND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_METAGROSS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantHit, .check = CheckClearBodySecondary },
    { .name = "intimidate_on_lead",
      .player = { MON(SPECIES_GYARADOS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { RATTATA_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckIntimidate },
    { .name = "intimidate_vs_clear_body",
      .player = { MON(SPECIES_GYARADOS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_METAGROSS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckIntimidateBlockedByAbility },
    { .name = "intimidate_vs_hyper_cutter",
      .player = { MON(SPECIES_GYARADOS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_PINSIR, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckIntimidateBlockedByAbility },
    { .name = "intimidate_vs_keen_eye_works",
      .player = { MON(SPECIES_GYARADOS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_PIDGEOT, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckIntimidateVsKeenEye },
    { .name = "intimidate_vs_substitute",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_GYARADOS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SUBSTITUTE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(SC_SWITCH(1), 1) }, .turns = 2, .check = CheckIntimidateVsSubstitute },
    { .name = "intimidate_both_leads",
      .player = { MON(SPECIES_GYARADOS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_TAUROS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckIntimidateBothLeads },

    // --- white herb
    { .name = "white_herb_restores_growl",
      .player = { MON_ITEM(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_WHITE_HERB) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_GROWL, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckWhiteHerbGrowl },
    { .name = "white_herb_after_superpower",
      .player = { MON_ITEM(SPECIES_MACHOP, 50, MOVE_SUPERPOWER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_WHITE_HERB) },
      .enemy = { STEELIX60_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckWhiteHerbSuperpower },
    { .name = "white_herb_vs_intimidate",
      .player = { MON_ITEM(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_WHITE_HERB) },
      .enemy = { MON(SPECIES_GYARADOS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckWhiteHerbIntimidate },

    // --- baton pass / switching
    { .name = "baton_pass_carries_stages_not_defense_curl",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_DEFENSE_CURL, MOVE_AGILITY, MOVE_BATON_PASS, MOVE_SPLASH), RATTATA_SPLASH },
      .enemy = { CHANSEY_SPLASH },
      .actions = { T(0, 0), T(1, 0), T(2, 0) }, .turns = 3, .check = CheckBatonPassStages },
    { .name = "switch_resets_stages",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SWORDS_DANCE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), RATTATA_SPLASH },
      .enemy = { CHANSEY_SPLASH },
      .actions = { T(0, 0), T(SC_SWITCH(1), 0), T(SC_SWITCH(0), 0) }, .turns = 3, .check = CheckSwitchResets },

    // --- stages in the damage / speed / accuracy formulas
    { .name = "attack_plus2_damage",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SWORDS_DANCE, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { CHANSEY_SPLASH },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .wantSeed = WantNoCrit, .check = CheckAttackPlus2Damage },
    { .name = "defense_minus2_damage",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SCREECH, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { CHANSEY_SPLASH },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .wantSeed = WantDefDrop2NoCrit, .check = CheckDefenseMinus2Damage },
    { .name = "crit_ignores_negative_attack_stage",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_FOCUS_ENERGY, MOVE_SLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_STEELIX, 50, MOVE_CHARM, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .wantSeed = WantCritTurn1, .check = CheckCritIgnoresNegativeAttack },
    { .name = "crit_ignores_positive_defense_stage",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_FOCUS_ENERGY, MOVE_SLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_STEELIX, 50, MOVE_IRON_DEFENSE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .wantSeed = WantCritTurn1, .check = CheckCritIgnoresPositiveDefense },
    { .name = "crit_keeps_positive_attack_stage",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SWORDS_DANCE, MOVE_FOCUS_ENERGY, MOVE_SLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_STEELIX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0), T(2, 0) }, .turns = 3, .wantSeed = WantCritTurn2, .check = CheckCritKeepsPositiveAttack },
    { .name = "guts_burn_with_stage",
      .player = { { .species = SPECIES_MACHAMP, .level = 50, .moves = { MOVE_BULK_UP, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH }, .status = STATUS1_BURN } },
      .enemy = { MON(SPECIES_STEELIX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .wantSeed = WantNoCrit, .check = CheckGutsWithStages },
    { .name = "hustle_with_stage",
      .player = { MON(SPECIES_TOGETIC, 50, MOVE_MEDITATE, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_STEELIX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .wantSeed = WantNoCritNoMiss, .check = CheckHustleWithStages },
    { .name = "agility_speed_order",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_AGILITY, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { RATTATA_SPLASH },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckAgilityOrder },
    { .name = "evasion_plus6_can_miss",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_DOUBLE_TEAM, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 1), T(0, 1), T(0, 1), T(0, 1), T(0, 1), T(0, 1), T(1, 0) }, .turns = 7, .wantSeed = WantMissTurn6, .check = CheckEvasionPlus6 },
    { .name = "accuracy_minus6_can_miss",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SAND_ATTACK, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 1), T(0, 1), T(0, 1), T(0, 1), T(0, 1), T(0, 1), T(1, 0) }, .turns = 7, .wantSeed = WantMissTurn6, .check = CheckAccuracyMinus6 },
    { .name = "foresight_ignores_evasion",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_DOUBLE_TEAM, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_FORESIGHT, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 2), T(0, 2), T(0, 2), T(0, 2), T(0, 2), T(0, 2), T(1, 0), T(1, 1) }, .turns = 8, .wantSeed = WantForesightLanded, .check = CheckForesightIgnoresEvasion },
    { .name = "growl_doubles_hits_both_foes",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_GROWL, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), RATTATA_SPLASH },
      .enemy = { RATTATA_SPLASH, RATTATA_SPLASH },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .check = CheckGrowlDoubles },
    { .name = "steel_wing_secondary_def_up",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_STEEL_WING, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { CHANSEY_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantDefUp, .check = CheckSteelWingDefUp },
};

SCENARIO_GROUP(stat_stages, sScenarios)
