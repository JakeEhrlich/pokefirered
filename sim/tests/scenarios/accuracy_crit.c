// Accuracy, evasion and critical hits (Cmd_accuracycheck / AccuracyCalcHelper / Cmd_tryKO / Cmd_critcalc /
// CalculateBaseDamage).  Engine facts these scenarios rely on:
//  - hit unless (Random()%100 + 1) > calc, so calc >= 100 never misses and calc == 0 never hits; the stage index is
//    attackerAcc + 6 - targetEvasion (clamped 0..12) through sAccuracyStageRatios (33% at -6 ... 300% at +6), then
//    Compound Eyes x1.3, Sand Veil in sandstorm x0.8 (needs WEATHER_HAS_EFFECT), Hustle x0.8 on physical types,
//    Bright Powder x0.9 / Lax Incense x0.95 (HOLD_EFFECT_EVASION_UP 10 / 5).  Foresight makes the index the raw
//    attacker accuracy stage (evasion ignored).
//  - AccuracyCalcHelper order: Lock-On sure hit -> on air / underground / underwater misses (unless the script set
//    HITMARKER_IGNORE_*) -> Thunder in rain / EFFECT_ALWAYS_HIT / EFFECT_VITAL_THROW hit.  So Swift misses a flying
//    target but a locked-on Pound does not.
//  - OHKO (NO_ACC_CALC_CHECK_LOCK_ON): sure hit skips the semi-invulnerable check (Protect is still applied earlier
//    by attackcanceler); otherwise semi-invulnerable => "but it failed".  tryKO: Sturdy first (even with Lock-On),
//    then chance = acc + (atkLevel - defLevel), hit if Random()%100+1 < chance and level >= target level;
//    Lock-On + level >= target = certain.
//  - crit stage = 2*FocusEnergy + highCritMove + ScopeLens + 2*(LuckyPunch & Chansey) + 2*(Stick & Farfetch'd),
//    capped at 4, chances {1/16,1/8,1/4,1/3,1/2}; Battle/Shell Armor block before the roll.  A crit doubles the
//    damage before type calc, ignores the attacker's *negative* and the defender's *positive* stages and Reflect /
//    Light Screen, but keeps the attacker's positive / defender's negative stages.
#include "scenario.h"

// ---- helpers -------------------------------------------------------------------------------------------------

// Replicates CalculateBaseDamage + Cmd_damagecalc for a plain hit (no STAB, no weather, no items/abilities that
// touch damage) and returns the [85%, 100%] range Cmd_adjustnormaldamage can produce.  Player-side battlers get
// the badge boosts (attack/defense/special x1.1).  typeMul10 is the type effectiveness in tenths (10 = neutral).
struct DmgRange { int lo, hi; };
static struct DmgRange ExpectDamage(struct BattleSim *sim, int atk, int def, int power, int special, int crit, int dmgMult, int screen, int typeMul10)
{
    static const int ratio[13][2] = { {10,40},{10,35},{10,30},{10,25},{10,20},{10,15},{10,10},{15,10},{20,10},{25,10},{30,10},{35,10},{40,10} };
    int attack = special ? B(atk).spAttack : B(atk).attack;
    int defense = special ? B(def).spDefense : B(def).defense;
    int as = B(atk).statStages[special ? STAT_SPATK : STAT_ATK];
    int ds = B(def).statStages[special ? STAT_SPDEF : STAT_DEF];
    int level = B(atk).level;
    int d, dh;
    struct DmgRange r;

    if (!(atk & 1)) attack = 110 * attack / 100;    // player's attacker: badge 1 / badge 7 boost
    if (!(def & 1)) defense = 110 * defense / 100;  // player's defender: badge 5 / badge 7 boost
    d = (crit && as <= DEFAULT_STAT_STAGE) ? attack : attack * ratio[as][0] / ratio[as][1];
    d = d * power;
    d *= (2 * level / 5 + 2);
    dh = (crit && ds >= DEFAULT_STAT_STAGE) ? defense : defense * ratio[ds][0] / ratio[ds][1];
    d = d / dh;
    d /= 50;
    if (screen && !crit) d /= 2;
    if (!special && d == 0) d = 1;
    d += 2;
    d = d * (crit ? 2 : 1) * dmgMult;
    d = d * typeMul10 / 10;
    if (d == 0) d = 1;
    r.lo = d * 85 / 100; if (r.lo == 0) r.lo = 1;
    r.hi = d;
    return r;
}
#define IN_RANGE(v, r) ((v) >= (r).lo && (v) <= (r).hi)

// The charge turn of Fly/Dig/Dive prints no "used move" string, so MOVED_BEFORE cannot see it: instead require the
// target's (battler 1) charge message to precede the attacker's (battler 0) move on `turn`.
static int TargetHidFirst(struct BattleSim *sim, int turn)
{
    int atk = Sc_LogIndex(sim, STRINGID_USEDMOVE, 0, 0, turn), hid = -1, i;
    const u16 charge[3] = { STRINGID_PKMNFLEWHIGH, STRINGID_PKMNDUGHOLE, STRINGID_PKMNHIDUNDERWATER };
    for (i = 0; i < 3; i++)
    {
        int idx = Sc_LogIndex(sim, charge[i], 1, 0, turn);
        if (idx >= 0 && (hid < 0 || idx < hid)) hid = idx;
    }
    return hid >= 0 && atk >= 0 && hid < atk;
}

// ---- accuracy / evasion stages ------------------------------------------------------------------------------

static void CheckSandAttack(struct BattleSim *sim)
{
    CHECK(STAGE(1, STAT_ACC) == 5, "accuracy -1 (stage %d)", STAGE(1, STAT_ACC));
    CHECK(LOG_HAS(STRINGID_DEFENDERSSTATFELL), "stat fell message");
    CHECK(STAGE(1, STAT_EVASION) == 6, "evasion untouched");
}

static void CheckSandAttackMin(struct BattleSim *sim)
{
    // 6 drops reach stage 0; the 7th prints "won't go lower".
    CHECK(STAGE(1, STAT_ACC) == 0, "accuracy at minimum (stage %d)", STAGE(1, STAT_ACC));
    CHECK(!LOG_HAS_T(STRINGID_STATSWONTDECREASE, 5), "6th drop still worked");
    CHECK(LOG_HAS_T(STRINGID_STATSWONTDECREASE, 6), "7th drop refused");
}

static void CheckSandAttackVsFlying(struct BattleSim *sim)
{
    // Stat-down moves never run typecalc: Ground-type Sand Attack lowers a Flying type's accuracy.
    CHECK(STAGE(1, STAT_ACC) == 5, "flying golbat's accuracy fell (stage %d)", STAGE(1, STAT_ACC));
    CHECK(!LOG_HAS(STRINGID_ITDOESNTAFFECT), "no immunity message");
}

static void CheckKeenEye(struct BattleSim *sim)
{
    CHECK(STAGE(1, STAT_ACC) == 6, "keen eye kept accuracy (stage %d)", STAGE(1, STAT_ACC));
    CHECK(LOG_HAS_T(STRINGID_PKMNSXPREVENTSYLOSS, 0), "keen eye message");
    CHECK(STAGE(1, STAT_DEF) == 5, "leer still lowers defense (stage %d)", STAGE(1, STAT_DEF));
}

static void CheckKeenEyeMudSlap(struct BattleSim *sim)
{
    CHECK(HP(1) < MAXHP(1), "mud-slap damage landed");
    CHECK(STAGE(1, STAT_ACC) == 6, "secondary accuracy drop blocked by keen eye (stage %d)", STAGE(1, STAT_ACC));
}

static void CheckMudSlap(struct BattleSim *sim)
{
    CHECK(HP(1) < MAXHP(1), "mud-slap damage landed");
    CHECK(STAGE(1, STAT_ACC) == 5, "100%% secondary accuracy drop (stage %d)", STAGE(1, STAT_ACC));
}

static void CheckDoubleTeamMax(struct BattleSim *sim)
{
    CHECK(STAGE(0, STAT_EVASION) == 12, "evasion maxed (stage %d)", STAGE(0, STAT_EVASION));
    // Self-targeted: gBattlerTarget == gActiveBattler selects B_MSG_DEFENDER_STAT_ROSE.
    CHECK(LOG_HAS_T(STRINGID_DEFENDERSSTATROSE, 0), "stat rose message");
    CHECK(LOG_HAS_T(STRINGID_STATSWONTINCREASE, 6), "7th double team refused");
}

static void CheckMinimize(struct BattleSim *sim)
{
    CHECK(STATUS3(0) & STATUS3_MINIMIZED, "minimized flag set");
    CHECK(STAGE(0, STAT_EVASION) == 7, "evasion +1 (stage %d)", STAGE(0, STAT_EVASION));
}

static void CheckEvasion6VsAerialAce(struct BattleSim *sim)
{
    CHECK(STAGE(0, STAT_EVASION) == 12, "evasion +6");
    CHECK(!LOG_HAS(STRINGID_ATTACKMISSED), "aerial ace never misses");
    CHECK(HP(0) < MAXHP(0), "aerial ace landed");
}

static void CheckAccMinus6VsAlwaysHit(struct BattleSim *sim)
{
    CHECK(STAGE(1, STAT_ACC) == 0, "accuracy -6 (stage %d)", STAGE(1, STAT_ACC));
    CHECK(!LOG_HAS_T(STRINGID_ATTACKMISSED, 6), "always-hit move ignored the accuracy stage");
    CHECK(HP(0) < MAXHP(0), "damage landed");
}

// Evasion +6 vs a 100-accuracy move: index 0 => 33%, so both outcomes must be possible.
static int WantMinimizeHit(struct BattleSim *sim) { return HP(0) < MAXHP(0); }
static int WantMinimizeMiss(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_ATTACKMISSED, 6); }
static void CheckMinimize6Hit(struct BattleSim *sim)
{
    CHECK(STAGE(0, STAT_EVASION) == 12, "evasion +6");
    CHECK(HP(0) < MAXHP(0), "pound can still hit at 33%%");
}
static void CheckMinimize6Miss(struct BattleSim *sim)
{
    CHECK(STAGE(0, STAT_EVASION) == 12, "evasion +6");
    CHECK(LOG_HAS_T(STRINGID_ATTACKMISSED, 6) && HP(0) == MAXHP(0), "pound missed");
}

// Foresight: the accuracy index is the attacker's accuracy stage alone (calc 100 => no miss possible).
static int WantForesightSet(struct BattleSim *sim) { return (STATUS2(1) & STATUS2_FORESIGHT) != 0; }
static void CheckForesightEvasion(struct BattleSim *sim)
{
    int t;
    CHECK(STAGE(1, STAT_EVASION) == 12, "target evasion +6 (stage %d)", STAGE(1, STAT_EVASION));
    CHECK(STATUS2(1) & STATUS2_FORESIGHT, "identified");
    for (t = 7; t <= 9; t++)
        CHECK(!LOG_HAS_T(STRINGID_ATTACKMISSED, t), "drill peck cannot miss an identified target (turn %d)", t);
    CHECK(HP(1) < MAXHP(1), "damage landed");
}

static void CheckForesightGhost(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNIDENTIFIED, 0), "identified message");
    CHECK(!LOG_HAS(STRINGID_ITDOESNTAFFECT), "normal and fighting hit the identified ghost");
    CHECK(LOG_INDEX_T(STRINGID_USEDMOVE, 0, 1) >= 0 && LOG_INDEX_T(STRINGID_USEDMOVE, 0, 2) >= 0, "both attacks used");
    CHECK(HP(1) < MAXHP(1), "gengar took damage");
}

static void CheckNormalVsGhost(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_ITDOESNTAFFECT), "no foresight: immune");
    CHECK(HP(1) == MAXHP(1), "no damage");
}

static void CheckForesightGhostVsNormal(struct BattleSim *sim)
{
    // The Foresight rows only cover Normal/Fighting vs Ghost; Ghost vs Normal stays immune.
    CHECK(STATUS2(1) & STATUS2_FORESIGHT, "identified");
    CHECK(LOG_HAS_T(STRINGID_ITDOESNTAFFECT, 1), "shadow punch still does not affect a normal type");
    CHECK(HP(1) == MAXHP(1), "no damage");
}

// ---- ability / item accuracy modifiers ----------------------------------------------------------------------

static void CheckNoMissAndDamage(struct BattleSim *sim)
{
    CHECK(!LOG_HAS(STRINGID_ATTACKMISSED), "no miss in any turn");
    CHECK(HP(1) < MAXHP(1), "damage landed");
}

static int WantAMiss(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_ATTACKMISSED, SC_ANY_TURN); }
static void CheckAMiss(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_ATTACKMISSED), "the move missed at least once");
}

static void CheckCompoundEyes(struct BattleSim *sim)
{
    CHECK(B(0).ability == ABILITY_COMPOUND_EYES, "butterfree has compound eyes");
    CHECK(!LOG_HAS(STRINGID_ATTACKMISSED), "80 * 1.3 = 104: hydro pump never misses");
    CHECK(HP(1) < MAXHP(1), "damage landed");
}

static void CheckHustleMiss(struct BattleSim *sim)
{
    CHECK(B(0).ability == ABILITY_HUSTLE, "togetic has hustle");
    CHECK(LOG_HAS(STRINGID_ATTACKMISSED), "100-accuracy physical move missed under hustle (80%%)");
}

static void CheckHustleSpecial(struct BattleSim *sim)
{
    CHECK(B(0).ability == ABILITY_HUSTLE, "togetic has hustle");
    CHECK(!LOG_HAS(STRINGID_ATTACKMISSED), "special moves are not affected by hustle");
    CHECK(HP(1) < MAXHP(1), "damage landed");
}

static void CheckSandVeilSandstorm(struct BattleSim *sim)
{
    CHECK(WEATHER() & B_WEATHER_SANDSTORM, "sandstorm up");
    CHECK(LOG_HAS(STRINGID_ATTACKMISSED), "sand veil made a 100-accuracy move miss (80%%)");
}

static void CheckSandVeilNoWeather(struct BattleSim *sim)
{
    CHECK(B(1).ability == ABILITY_SAND_VEIL, "sandslash has sand veil");
    CHECK(!(WEATHER() & B_WEATHER_SANDSTORM), "no sandstorm");
    CHECK(!LOG_HAS(STRINGID_ATTACKMISSED), "sand veil does nothing without sandstorm");
}

static void CheckSandVeilCloudNine(struct BattleSim *sim)
{
    CHECK(B(0).ability == ABILITY_CLOUD_NINE, "golduck has cloud nine");
    CHECK(WEATHER() & B_WEATHER_SANDSTORM, "sandstorm is set");
    CHECK(!LOG_HAS(STRINGID_ATTACKMISSED), "cloud nine negates sand veil's boost");
    CHECK(HP(1) < MAXHP(1), "damage landed");
}

// ---- Thunder --------------------------------------------------------------------------------------------------

static void CheckThunderRain(struct BattleSim *sim)
{
    CHECK(WEATHER() & B_WEATHER_RAIN, "rain up");
    CHECK(!LOG_HAS(STRINGID_ATTACKMISSED), "thunder never misses in rain");
    CHECK(HP(1) < MAXHP(1), "damage landed");
}

static void CheckThunderSunMiss(struct BattleSim *sim)
{
    CHECK(WEATHER() & B_WEATHER_SUN, "sun up");
    CHECK(LOG_HAS(STRINGID_ATTACKMISSED), "thunder missed in sun (50%%)");
}

static void CheckThunderRainCloudNine(struct BattleSim *sim)
{
    CHECK(WEATHER() & B_WEATHER_RAIN, "rain up");
    CHECK(B(1).ability == ABILITY_CLOUD_NINE, "golduck has cloud nine");
    CHECK(LOG_HAS(STRINGID_ATTACKMISSED), "no sure hit under cloud nine (70%%)");
}

static void CheckThunderVsFly(struct BattleSim *sim)
{
    CHECK(TargetHidFirst(sim, 1), "target went semi-invulnerable before the attack");
    CHECK(LOG_HAS_T(STRINGID_PKMNFLEWHIGH, 1), "pidgeot flew up first");
    CHECK(!LOG_HAS(STRINGID_ATTACKMISSED), "thunder ignores on-air");
    CHECK(HP(1) < MAXHP(1), "thunder hit the flying pidgeot");
}

static void CheckThunderVsDig(struct BattleSim *sim)
{
    // Thunder only sets HITMARKER_IGNORE_ON_AIR: the underground check still misses, even in rain, and before typecalc.
    CHECK(TargetHidFirst(sim, 1), "target went semi-invulnerable before the attack");
    CHECK(LOG_HAS_T(STRINGID_PKMNDUGHOLE, 1), "dugtrio dug first");
    CHECK(LOG_HAS_T(STRINGID_ATTACKMISSED, 1), "thunder missed the underground target");
    CHECK(!LOG_HAS(STRINGID_ITDOESNTAFFECT), "accuracy failure precedes the ground immunity");
    CHECK(HP(1) == MAXHP(1), "no damage");
}

// ---- semi-invulnerable targets -------------------------------------------------------------------------------

static void CheckGustVsFly(struct BattleSim *sim)
{
    struct DmgRange r = ExpectDamage(sim, 0, 1, 40, 0, 0, 2, 0, 10);
    int dealt = MAXHP(1) - HP(1);
    CHECK(TargetHidFirst(sim, 0), "target went semi-invulnerable before the attack");
    CHECK(LOG_HAS(STRINGID_PKMNFLEWHIGH), "target flew up");
    CHECK(!LOG_HAS(STRINGID_ATTACKMISSED), "gust hits a flying target");
    CHECK(!LOG_HAS(STRINGID_CRITICALHIT), "shell armor: no crit");
    CHECK(IN_RANGE(dealt, r), "gust double damage (dealt %d, expected %d..%d)", dealt, r.lo, r.hi);
}

static void CheckTwisterVsFly(struct BattleSim *sim)
{
    struct DmgRange r = ExpectDamage(sim, 0, 1, 40, 1, 0, 2, 0, 10);
    int dealt = MAXHP(1) - HP(1);
    CHECK(TargetHidFirst(sim, 0), "target went semi-invulnerable before the attack");
    CHECK(!LOG_HAS(STRINGID_ATTACKMISSED), "twister hits a flying target");
    CHECK(IN_RANGE(dealt, r), "twister double damage (dealt %d, expected %d..%d)", dealt, r.lo, r.hi);
}

static void CheckEarthquakeVsDig(struct BattleSim *sim)
{
    struct DmgRange r = ExpectDamage(sim, 0, 1, 100, 0, 0, 2, 0, 10);
    int dealt = MAXHP(1) - HP(1);
    CHECK(TargetHidFirst(sim, 0), "target went semi-invulnerable before the attack");
    CHECK(LOG_HAS(STRINGID_PKMNDUGHOLE), "target dug");
    CHECK(!LOG_HAS(STRINGID_ATTACKMISSED), "earthquake hits an underground target");
    CHECK(IN_RANGE(dealt, r), "earthquake double damage (dealt %d, expected %d..%d)", dealt, r.lo, r.hi);
}

static void CheckMagnitudeVsDig(struct BattleSim *sim)
{
    struct DmgRange r = ExpectDamage(sim, 0, 1, 10, 0, 0, 2, 0, 10);   // weakest magnitude, doubled
    int dealt = MAXHP(1) - HP(1);
    CHECK(TargetHidFirst(sim, 0), "target went semi-invulnerable before the attack");
    CHECK(LOG_HAS(STRINGID_MAGNITUDESTRENGTH), "magnitude strength message");
    CHECK(!LOG_HAS(STRINGID_ATTACKMISSED), "magnitude hits an underground target");
    CHECK(dealt >= r.lo, "at least the doubled magnitude-4 damage (dealt %d, min %d)", dealt, r.lo);
}

static void CheckSurfVsDive(struct BattleSim *sim)
{
    // Water vs Rock/Water: x2 then x0.5 cancel exactly, so the result is the doubled neutral damage.
    struct DmgRange r = ExpectDamage(sim, 0, 1, 95, 1, 0, 2, 0, 10);
    int dealt = MAXHP(1) - HP(1);
    CHECK(TargetHidFirst(sim, 0), "target went semi-invulnerable before the attack");
    CHECK(LOG_HAS(STRINGID_PKMNHIDUNDERWATER), "target dived");
    CHECK(!LOG_HAS(STRINGID_ATTACKMISSED), "surf hits an underwater target");
    CHECK(!LOG_HAS(STRINGID_CRITICALHIT), "battle armor: no crit");
    CHECK(IN_RANGE(dealt, r), "surf double damage (dealt %d, expected %d..%d)", dealt, r.lo, r.hi);
}

static int WantHitNoMiss(struct BattleSim *sim) { return HP(1) < MAXHP(1) && !Sc_LogHas(sim, STRINGID_ATTACKMISSED, SC_ANY_TURN); }
static void CheckWhirlpoolVsDive(struct BattleSim *sim)
{
    struct DmgRange r = ExpectDamage(sim, 0, 1, 15, 1, 0, 2, 0, 10);
    int dealt = MAXHP(1) - HP(1) - MAXHP(1) / 16;   // minus the end-of-turn trap damage
    CHECK(TargetHidFirst(sim, 0), "target went semi-invulnerable before the attack");
    CHECK(STATUS2(1) & STATUS2_WRAPPED, "trapped by whirlpool");
    CHECK(IN_RANGE(dealt, r), "whirlpool double damage (dealt %d, expected %d..%d)", dealt, r.lo, r.hi);
}

static void CheckSkyUppercutVsFly(struct BattleSim *sim)
{
    // Hits the flying target but with normal (super effective vs Ice) damage: no x2 multiplier.
    struct DmgRange r = ExpectDamage(sim, 0, 1, 85, 0, 0, 1, 0, 20);
    int dealt = MAXHP(1) - HP(1);
    CHECK(TargetHidFirst(sim, 0), "target went semi-invulnerable before the attack");
    CHECK(LOG_HAS(STRINGID_PKMNFLEWHIGH), "target flew up");
    CHECK(IN_RANGE(dealt, r), "sky uppercut normal damage (dealt %d, expected %d..%d)", dealt, r.lo, r.hi);
}

static void CheckMissedSemiInvulnerable(struct BattleSim *sim)
{
    CHECK(TargetHidFirst(sim, 0), "target went semi-invulnerable before the attack");
    CHECK(LOG_HAS_T(STRINGID_ATTACKMISSED, 0), "missed the semi-invulnerable target");
    CHECK(HP(1) == MAXHP(1), "no damage");
}

// ---- Lock-On / Mind Reader -----------------------------------------------------------------------------------

static void CheckLockOnFissure(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNTOOKAIM, 0), "took aim");
    CHECK(LOG_HAS_T(STRINGID_ONEHITKO, 1), "one-hit KO message");
    CHECK(HP(1) == 0, "target fainted");
}

static void CheckLockOnTimer1(struct BattleSim *sim)
{
    CHECK((STATUS3(1) & STATUS3_ALWAYS_HITS) == STATUS3_ALWAYS_HITS_TURN(1), "sure-hit timer 1 after the first end of turn (status3 %#x)", STATUS3(1));
    CHECK(sim->disableStructs[1].battlerWithSureHit == 0, "sure hit belongs to battler 0");
}

static void CheckLockOnTimer0(struct BattleSim *sim)
{
    CHECK((STATUS3(1) & STATUS3_ALWAYS_HITS) == 0, "sure hit expired after two ends of turn (status3 %#x)", STATUS3(1));
}

static void CheckLockOnExpiredVsFly(struct BattleSim *sim)
{
    CHECK(TargetHidFirst(sim, 2), "target went semi-invulnerable before the attack");
    CHECK(LOG_HAS_T(STRINGID_PKMNFLEWHIGH, 2), "target flew up on turn 3");
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 2), "fissure fails vs a flying target once lock-on is gone");
    CHECK(HP(1) == MAXHP(1), "no damage");
}

static void CheckMindReaderVsFly(struct BattleSim *sim)
{
    CHECK(TargetHidFirst(sim, 1), "target went semi-invulnerable before the attack");
    CHECK(LOG_HAS_T(STRINGID_PKMNTOOKAIM, 0), "took aim");
    CHECK(LOG_HAS_T(STRINGID_PKMNFLEWHIGH, 1), "pidgeot flew up first");
    CHECK(!LOG_HAS(STRINGID_ATTACKMISSED), "sure hit beats semi-invulnerability");
    CHECK(HP(1) < MAXHP(1), "pound hit the flying pidgeot");
}

static void CheckLockOnDynamicPunch(struct BattleSim *sim)
{
    CHECK(!LOG_HAS_T(STRINGID_ATTACKMISSED, 1), "50-accuracy move cannot miss after lock-on");
    CHECK(HP(1) < MAXHP(1), "damage landed");
    CHECK(STATUS2(1) & STATUS2_CONFUSION, "dynamic punch confused the target");
}

static void CheckLockOnFissureVsFly(struct BattleSim *sim)
{
    CHECK(TargetHidFirst(sim, 1), "target went semi-invulnerable before the attack");
    CHECK(LOG_HAS_T(STRINGID_PKMNFLEWHIGH, 1), "target flew up first");
    CHECK(LOG_HAS_T(STRINGID_ONEHITKO, 1), "locked-on fissure hits the flying target");
    CHECK(HP(1) == 0, "target fainted");
}

static void CheckLockOnFissureVsProtect(struct BattleSim *sim)
{
    // Cmd_attackcanceler flags a protected target as MOVE_RESULT_MISSED before accuracycheck runs, so the sure-hit
    // branch of NO_ACC_CALC_CHECK_LOCK_ON (which skips its own Protect check) never matters.
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_PROTECT, 1), "protect was used");
    CHECK(LOG_HAS_T(STRINGID_PKMNPROTECTEDITSELF, 1), "protect blocked the locked-on fissure");
    CHECK(!LOG_HAS(STRINGID_ONEHITKO) && HP(1) == MAXHP(1), "no KO");
}

static void CheckLockOnHigherLevel(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNUNAFFECTED, 1), "unaffected: target level is higher");
    CHECK(!LOG_HAS(STRINGID_ONEHITKO) && HP(1) == MAXHP(1), "no KO");
}

static void CheckLockOnSturdy(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNPROTECTEDBY, 1), "sturdy message");
    CHECK(HP(1) == MAXHP(1), "no damage");
}

static void CheckLockOnVsSubstitute(struct BattleSim *sim)
{
    CHECK(MOVED_BEFORE(1, 0, 0), "target moved first");
    CHECK(STATUS2(1) & STATUS2_SUBSTITUTE, "substitute up");
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 0), "lock-on fails against a substitute");
    CHECK((STATUS3(1) & STATUS3_ALWAYS_HITS) == 0, "no sure hit");
}

static void CheckLockOnOnlyForUser(struct BattleSim *sim)
{
    // Doubles: Snorlax (0) locked on to Cloyster (1); Slowbro (2) did not.  Cloyster flies on turn 2.
    CHECK(sim->disableStructs[1].battlerWithSureHit == 0, "sure hit belongs to snorlax");
    CHECK(LOG_INDEX_T(STRINGID_ATTACKMISSED, 2, 1) >= 0, "slowbro's pound missed the flying cloyster");
    CHECK(LOG_INDEX_T(STRINGID_ATTACKMISSED, 0, 1) < 0, "snorlax's pound did not miss");
    CHECK(HP(1) < MAXHP(1), "cloyster took damage");
}

// ---- OHKO -----------------------------------------------------------------------------------------------------

static int WantOhkoHit(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_ONEHITKO, SC_ANY_TURN); }
static int WantOhkoMiss(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_ATTACKMISSED, SC_ANY_TURN); }
static void CheckOhkoHit(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_ONEHITKO) && HP(1) == 0, "same level: fissure can hit");
}
static void CheckOhkoMiss(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_ATTACKMISSED), "same level: fissure can miss (\"missed\", not \"unaffected\")");
    CHECK(!LOG_HAS(STRINGID_PKMNUNAFFECTED) && HP(1) == MAXHP(1), "no damage");
}

static void CheckOhkoLevelGap(struct BattleSim *sim)
{
    // chance = 30 + 99 = 129 > any roll.
    CHECK(LOG_HAS_T(STRINGID_ONEHITKO, 0) && HP(1) == 0, "level 100 vs 1: always hits");
}

static void CheckOhkoIgnoresEvasion(struct BattleSim *sim)
{
    // (the faint resets the stat stages, so count the six "evasiveness rose" messages instead)
    CHECK(LOG_COUNT(STRINGID_DEFENDERSSTATROSE) == 6, "six minimizes (%d rose messages)", LOG_COUNT(STRINGID_DEFENDERSSTATROSE));
    CHECK(LOG_HAS_T(STRINGID_ONEHITKO, 6) && HP(1) == 0, "OHKO accuracy ignores evasion");
}

static void CheckOhkoVsFly(struct BattleSim *sim)
{
    CHECK(TargetHidFirst(sim, 0), "target went semi-invulnerable before the attack");
    CHECK(LOG_HAS_T(STRINGID_PKMNFLEWHIGH, 0), "target flew up first");
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 0), "OHKO vs semi-invulnerable: \"but it failed\"");
    CHECK(!LOG_HAS(STRINGID_ATTACKMISSED) && HP(1) == MAXHP(1), "no miss message, no damage");
}

static void CheckOhkoVsProtect(struct BattleSim *sim)
{
    CHECK(MOVED_BEFORE(1, 0, 0), "target moved first");
    CHECK(LOG_HAS_T(STRINGID_PKMNPROTECTEDITSELF, 0), "protect blocks a non-locked-on OHKO");
    CHECK(HP(1) == MAXHP(1), "no damage");
}

static void CheckOhkoSturdy(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNPROTECTEDBY, 0), "sturdy message");
    CHECK(HP(1) == MAXHP(1), "no damage");
}

static void CheckHornDrillVsGhost(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_ITDOESNTAFFECT, 0), "typecalc runs before tryKO");
    CHECK(HP(1) == MAXHP(1), "no damage");
}

// ---- critical hits --------------------------------------------------------------------------------------------

static void CheckFocusEnergy(struct BattleSim *sim)
{
    CHECK(STATUS2(0) & STATUS2_FOCUS_ENERGY, "focus energy flag");
    CHECK(LOG_HAS_T(STRINGID_PKMNGETTINGPUMPED, 0), "getting pumped");
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 1), "second focus energy fails");
}

static int WantCritT0(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_CRITICALHIT, 0); }
static int WantCritT1(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_CRITICALHIT, 1); }
static int WantCritT2(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_CRITICALHIT, 2); }
static int WantCritT3(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_CRITICALHIT, 3); }
static int WantNoCrit(struct BattleSim *sim) { return !Sc_LogHas(sim, STRINGID_CRITICALHIT, SC_ANY_TURN) && HP(1) < MAXHP(1); }

static void CheckCritDouble(struct BattleSim *sim)
{
    struct DmgRange crit = ExpectDamage(sim, 0, 1, 80, 0, 1, 1, 0, 10);
    struct DmgRange normal = ExpectDamage(sim, 0, 1, 80, 0, 0, 1, 0, 10);
    int dealt = MAXHP(1) - HP(1);
    int i, logged = -1;
    for (i = 0; i < sim->logCount; i++)
        if (sim->log[i].stringId == STRINGID_CRITICALHIT) logged = sim->log[i].dmg;
    CHECK(LOG_HAS(STRINGID_CRITICALHIT), "critical hit message");
    CHECK(IN_RANGE(dealt, crit), "crit damage doubled (dealt %d, expected %d..%d)", dealt, crit.lo, crit.hi);
    CHECK(!IN_RANGE(dealt, normal), "outside the non-crit range %d..%d", normal.lo, normal.hi);
    CHECK(logged == dealt, "logged damage matches hp loss (%d vs %d)", logged, dealt);
}

static void CheckNoCritNormal(struct BattleSim *sim)
{
    struct DmgRange normal = ExpectDamage(sim, 0, 1, 80, 0, 0, 1, 0, 10);
    int dealt = MAXHP(1) - HP(1);
    CHECK(IN_RANGE(dealt, normal), "non-crit damage (dealt %d, expected %d..%d)", dealt, normal.lo, normal.hi);
}

static void CheckCritIgnoresAtkDrop(struct BattleSim *sim)
{
    struct DmgRange crit = ExpectDamage(sim, 0, 1, 80, 0, 1, 1, 0, 10);
    int dealt = MAXHP(1) - HP(1);
    CHECK(STAGE(0, STAT_ATK) == 0, "attacker at -6 attack (stage %d)", STAGE(0, STAT_ATK));
    CHECK(LOG_HAS_T(STRINGID_CRITICALHIT, 3), "crit on turn 4");
    CHECK(IN_RANGE(dealt, crit), "crit ignores the attack drop (dealt %d, expected %d..%d)", dealt, crit.lo, crit.hi);
}

static void CheckCritIgnoresDefBoost(struct BattleSim *sim)
{
    struct DmgRange crit = ExpectDamage(sim, 0, 1, 80, 0, 1, 1, 0, 10);
    int dealt = MAXHP(1) - HP(1);
    CHECK(STAGE(1, STAT_DEF) == 12, "defender at +6 defense (stage %d)", STAGE(1, STAT_DEF));
    CHECK(LOG_HAS_T(STRINGID_CRITICALHIT, 3), "crit on turn 4");
    CHECK(IN_RANGE(dealt, crit), "crit ignores the defense boost (dealt %d, expected %d..%d)", dealt, crit.lo, crit.hi);
}

static void CheckCritKeepsAtkBoost(struct BattleSim *sim)
{
    struct DmgRange crit = ExpectDamage(sim, 0, 1, 80, 0, 1, 1, 0, 10);
    int dealt = MAXHP(1) - HP(1);
    CHECK(STAGE(0, STAT_ATK) == 8, "attacker at +2 attack");
    CHECK(LOG_HAS_T(STRINGID_CRITICALHIT, 1), "crit on turn 2");
    CHECK(IN_RANGE(dealt, crit), "crit keeps the attack boost (dealt %d, expected %d..%d)", dealt, crit.lo, crit.hi);
}

static void CheckCritKeepsDefDrop(struct BattleSim *sim)
{
    struct DmgRange crit = ExpectDamage(sim, 0, 1, 80, 0, 1, 1, 0, 10);
    int dealt = MAXHP(1) - HP(1);
    CHECK(STAGE(1, STAT_DEF) == 4, "defender at -2 defense (stage %d)", STAGE(1, STAT_DEF));
    CHECK(LOG_HAS_T(STRINGID_CRITICALHIT, 2), "crit on turn 3");
    CHECK(IN_RANGE(dealt, crit), "crit keeps the defense drop (dealt %d, expected %d..%d)", dealt, crit.lo, crit.hi);
}

static void CheckCritIgnoresReflect(struct BattleSim *sim)
{
    struct DmgRange crit = ExpectDamage(sim, 0, 1, 80, 0, 1, 1, 1, 10);
    int dealt = MAXHP(1) - HP(1);
    CHECK(SIDE_STATUS(B_SIDE_OPPONENT) & SIDE_STATUS_REFLECT, "reflect up");
    CHECK(LOG_HAS_T(STRINGID_CRITICALHIT, 1), "crit on turn 2");
    CHECK(IN_RANGE(dealt, crit), "crit ignores reflect (dealt %d, expected %d..%d)", dealt, crit.lo, crit.hi);
}

static void CheckReflectHalvesNonCrit(struct BattleSim *sim)
{
    struct DmgRange normal = ExpectDamage(sim, 0, 1, 80, 0, 0, 1, 1, 10);
    int dealt = MAXHP(1) - HP(1);
    CHECK(SIDE_STATUS(B_SIDE_OPPONENT) & SIDE_STATUS_REFLECT, "reflect up");
    CHECK(IN_RANGE(dealt, normal), "reflect halves a non-crit (dealt %d, expected %d..%d)", dealt, normal.lo, normal.hi);
}

static void CheckCritIgnoresLightScreen(struct BattleSim *sim)
{
    struct DmgRange crit = ExpectDamage(sim, 0, 1, 95, 1, 1, 1, 1, 10);
    int dealt = MAXHP(1) - HP(1);
    CHECK(SIDE_STATUS(B_SIDE_OPPONENT) & SIDE_STATUS_LIGHTSCREEN, "light screen up");
    CHECK(LOG_HAS_T(STRINGID_CRITICALHIT, 1), "crit on turn 2");
    CHECK(IN_RANGE(dealt, crit), "special crit ignores light screen (dealt %d, expected %d..%d)", dealt, crit.lo, crit.hi);
}

static void CheckHighCritStage(struct BattleSim *sim)
{
    // Stage 4 (1/2) for 16 attacks: expect plenty of crits (P(<4) ~ 1% for a random seed; the seed is fixed).
    CHECK(STATUS2(0) & STATUS2_FOCUS_ENERGY, "focus energy up");
    CHECK(!LOG_HAS(STRINGID_ATTACKMISSED), "slash never missed");
    CHECK(LOG_COUNT(STRINGID_CRITICALHIT) >= 4, "max crit stage: %d crits in 16 slashes", LOG_COUNT(STRINGID_CRITICALHIT));
    CHECK(HP(1) > 0, "target survived");
}

static void CheckArmorNoCrit(struct BattleSim *sim)
{
    CHECK(STATUS2(0) & STATUS2_FOCUS_ENERGY, "focus energy up");
    CHECK(HP(1) < MAXHP(1) && HP(1) > 0, "slashes landed, target alive");
    CHECK(LOG_COUNT(STRINGID_CRITICALHIT) == 0, "armor ability: %d crits", LOG_COUNT(STRINGID_CRITICALHIT));
}

static void CheckScopeLensCrit(struct BattleSim *sim)
{
    struct DmgRange crit = ExpectDamage(sim, 0, 1, 80, 0, 1, 1, 0, 10);
    int dealt = MAXHP(1) - HP(1);
    CHECK(LOG_HAS_T(STRINGID_CRITICALHIT, 0), "crit");
    CHECK(IN_RANGE(dealt, crit), "scope lens crit is still x2 (dealt %d, expected %d..%d)", dealt, crit.lo, crit.hi);
}

// ---- always-hit vs Protect -------------------------------------------------------------------------------------

static void CheckSwiftVsProtect(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNPROTECTEDITSELF, 0), "protect blocks swift");
    CHECK(HP(1) == MAXHP(1), "no damage");
}

// ---- scenarios ------------------------------------------------------------------------------------------------

#define T6(a, b) T(a, b), T(a, b), T(a, b), T(a, b), T(a, b), T(a, b)
#define T16(a, b) T(a, b), T(a, b), T(a, b), T(a, b), T(a, b), T(a, b), T(a, b), T(a, b), T(a, b), T(a, b), T(a, b), T(a, b), T(a, b), T(a, b), T(a, b), T(a, b)

static const struct Scenario sScenarios[] =
{
    // --- accuracy / evasion stages ---
    { .name = "sand_attack_lowers_accuracy",
      .player = { MON(SPECIES_SANDSLASH, 50, MOVE_SAND_ATTACK, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSandAttack },
    { .name = "sand_attack_min_stage",
      .player = { MON(SPECIES_SANDSLASH, 50, MOVE_SAND_ATTACK, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T6(0, 0), T(0, 0) }, .turns = 7, .check = CheckSandAttackMin },
    { .name = "sand_attack_vs_flying_type",
      .player = { MON(SPECIES_SANDSLASH, 50, MOVE_SAND_ATTACK, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_GOLBAT, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSandAttackVsFlying },
    { .name = "keen_eye_blocks_accuracy_drop",
      .player = { MON(SPECIES_SANDSLASH, 50, MOVE_SAND_ATTACK, MOVE_LEER, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_PIDGEOT, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckKeenEye },
    { .name = "keen_eye_blocks_mud_slap_secondary",
      .player = { MON(SPECIES_SANDSLASH, 50, MOVE_MUD_SLAP, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_HITMONCHAN, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckKeenEyeMudSlap },
    { .name = "mud_slap_secondary_accuracy_drop",
      .player = { MON(SPECIES_SANDSLASH, 50, MOVE_MUD_SLAP, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckMudSlap },
    { .name = "double_team_max_stage",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_DOUBLE_TEAM, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T6(0, 0), T(0, 0) }, .turns = 7, .check = CheckDoubleTeamMax },
    { .name = "minimize_flag_and_evasion",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_MINIMIZE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckMinimize },
    { .name = "evasion_plus6_vs_aerial_ace",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_DOUBLE_TEAM, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_PIDGEOT, 50, MOVE_SPLASH, MOVE_AERIAL_ACE, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T6(0, 0), T(1, 1) }, .turns = 7, .check = CheckEvasion6VsAerialAce },
    { .name = "accuracy_minus6_vs_swift",
      .player = { MON(SPECIES_SANDSLASH, 50, MOVE_SAND_ATTACK, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SWIFT, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T6(0, 0), T(1, 1) }, .turns = 7, .check = CheckAccMinus6VsAlwaysHit },
    { .name = "accuracy_minus6_vs_vital_throw",
      .player = { MON(SPECIES_SANDSLASH, 50, MOVE_SAND_ATTACK, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_MACHOP, 50, MOVE_SPLASH, MOVE_VITAL_THROW, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T6(0, 0), T(1, 1) }, .turns = 7, .check = CheckAccMinus6VsAlwaysHit },
    { .name = "minimize_x6_can_still_be_hit",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_MINIMIZE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T6(0, 0), T(1, 1) }, .turns = 7, .wantSeed = WantMinimizeHit, .check = CheckMinimize6Hit },
    { .name = "minimize_x6_can_dodge",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_MINIMIZE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T6(0, 0), T(1, 1) }, .turns = 7, .wantSeed = WantMinimizeMiss, .check = CheckMinimize6Miss },
    { .name = "foresight_ignores_evasion",
      .player = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_FORESIGHT, MOVE_DRILL_PECK, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_DOUBLE_TEAM, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T6(0, 0), T(1, 1), T(2, 1), T(2, 1), T(2, 1) }, .turns = 10, .wantSeed = WantForesightSet, .check = CheckForesightEvasion },
    { .name = "foresight_ghost_hit_by_normal_and_fighting",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_FORESIGHT, MOVE_POUND, MOVE_KARATE_CHOP, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_GENGAR, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0), T(2, 0) }, .turns = 3, .check = CheckForesightGhost },
    { .name = "normal_vs_ghost_without_foresight",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_GENGAR, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckNormalVsGhost },
    { .name = "foresight_does_not_help_ghost_vs_normal",
      .player = { MON(SPECIES_GENGAR, 50, MOVE_FORESIGHT, MOVE_SHADOW_PUNCH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckForesightGhostVsNormal },

    // --- ability / item modifiers ---
    { .name = "compound_eyes_80acc_never_misses",
      .player = { MON(SPECIES_BUTTERFREE, 50, MOVE_HYDRO_PUMP, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_BLISSEY, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0), T(0, 0), T(0, 0) }, .turns = 4, .check = CheckCompoundEyes },
    { .name = "hydro_pump_80acc_can_miss",
      .player = { MON(SPECIES_RATTATA, 50, MOVE_HYDRO_PUMP, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_BLISSEY, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0), T(0, 0), T(0, 0) }, .turns = 4, .wantSeed = WantAMiss, .check = CheckAMiss },
    { .name = "hustle_physical_can_miss",
      .player = { MON(SPECIES_TOGETIC, 50, MOVE_STRENGTH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_BLISSEY, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0), T(0, 0), T(0, 0) }, .turns = 4, .wantSeed = WantAMiss, .check = CheckHustleMiss },
    { .name = "hustle_special_unaffected",
      .player = { MON(SPECIES_TOGETIC, 50, MOVE_ICE_BEAM, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_BLISSEY, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0), T(0, 0), T(0, 0) }, .turns = 4, .check = CheckHustleSpecial },
    { .name = "bright_powder_can_dodge",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_DRILL_PECK, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_ITEM(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_BRIGHT_POWDER) },
      .actions = { T(0, 0), T(0, 0), T(0, 0) }, .turns = 3, .wantSeed = WantAMiss, .check = CheckAMiss },
    { .name = "lax_incense_can_dodge",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_DRILL_PECK, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_ITEM(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_LAX_INCENSE) },
      .actions = { T(0, 0), T(0, 0), T(0, 0) }, .turns = 3, .wantSeed = WantAMiss, .check = CheckAMiss },
    { .name = "bright_powder_vs_swift",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SWIFT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_ITEM(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_BRIGHT_POWDER) },
      .actions = { T(0, 0), T(0, 0), T(0, 0), T(0, 0), T(0, 0) }, .turns = 5, .check = CheckNoMissAndDamage },
    { .name = "sand_veil_in_sandstorm_can_dodge",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SANDSTORM, MOVE_DRILL_PECK, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SANDSLASH, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0), T(1, 0), T(1, 0) }, .turns = 4, .wantSeed = WantAMiss, .check = CheckSandVeilSandstorm },
    { .name = "sand_veil_without_sandstorm",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_DRILL_PECK, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SANDSLASH, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0), T(0, 0), T(0, 0) }, .turns = 4, .check = CheckSandVeilNoWeather },
    { .name = "sand_veil_negated_by_cloud_nine",
      .player = { MON_AB(SPECIES_GOLDUCK, 50, MOVE_SANDSTORM, MOVE_DRILL_PECK, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .enemy = { MON(SPECIES_SANDSLASH, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0), T(1, 0), T(1, 0) }, .turns = 4, .check = CheckSandVeilCloudNine },

    // --- Thunder ---
    { .name = "thunder_in_rain_never_misses",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_RAIN_DANCE, MOVE_THUNDER, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_BLISSEY, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0), T(1, 0), T(1, 0) }, .turns = 4, .check = CheckThunderRain },
    { .name = "thunder_in_sun_can_miss",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SUNNY_DAY, MOVE_THUNDER, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_BLISSEY, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0), T(1, 0), T(1, 0) }, .turns = 4, .wantSeed = WantAMiss, .check = CheckThunderSunMiss },
    { .name = "thunder_rain_cloud_nine_can_miss",
      .player = { MON(SPECIES_SNORLAX, 20, MOVE_RAIN_DANCE, MOVE_THUNDER, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_AB(SPECIES_GOLDUCK, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .actions = { T(0, 0), T(1, 0), T(1, 0), T(1, 0) }, .turns = 4, .wantSeed = WantAMiss, .check = CheckThunderRainCloudNine },
    { .name = "thunder_in_rain_hits_flying",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_RAIN_DANCE, MOVE_THUNDER, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_PIDGEOT, 50, MOVE_SPLASH, MOVE_FLY, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .check = CheckThunderVsFly },
    { .name = "thunder_in_rain_misses_underground",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_RAIN_DANCE, MOVE_THUNDER, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_DUGTRIO, 50, MOVE_SPLASH, MOVE_DIG, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .check = CheckThunderVsDig },

    // --- semi-invulnerable targets (Cloyster = Shell Armor, Kabutops[1] = Battle Armor: no crits) ---
    { .name = "gust_vs_fly_double_damage",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_GUST, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_CLOYSTER, 50, MOVE_FLY, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckGustVsFly },
    { .name = "twister_vs_fly_double_damage",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_TWISTER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_CLOYSTER, 50, MOVE_FLY, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckTwisterVsFly },
    { .name = "earthquake_vs_dig_double_damage",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_EARTHQUAKE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_CLOYSTER, 50, MOVE_DIG, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckEarthquakeVsDig },
    { .name = "magnitude_vs_dig_hits",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_MAGNITUDE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_CLOYSTER, 50, MOVE_DIG, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckMagnitudeVsDig },
    { .name = "surf_vs_dive_double_damage",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SURF, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_AB(SPECIES_KABUTOPS, 50, MOVE_DIVE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSurfVsDive },
    { .name = "whirlpool_vs_dive_double_damage",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_WHIRLPOOL, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_AB(SPECIES_KABUTOPS, 50, MOVE_DIVE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantHitNoMiss, .check = CheckWhirlpoolVsDive },
    { .name = "sky_uppercut_vs_fly_normal_damage",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SKY_UPPERCUT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_CLOYSTER, 50, MOVE_FLY, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantHitNoMiss, .check = CheckSkyUppercutVsFly },
    { .name = "earthquake_vs_fly_misses",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_EARTHQUAKE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_CLOYSTER, 50, MOVE_FLY, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckMissedSemiInvulnerable },
    { .name = "swift_vs_fly_misses",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SWIFT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_CLOYSTER, 50, MOVE_FLY, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckMissedSemiInvulnerable },
    { .name = "aerial_ace_vs_dig_misses",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_AERIAL_ACE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_CLOYSTER, 50, MOVE_DIG, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckMissedSemiInvulnerable },
    { .name = "pound_vs_dive_misses",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_CLOYSTER, 50, MOVE_DIVE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckMissedSemiInvulnerable },

    // --- Lock-On / Mind Reader ---
    { .name = "lock_on_then_fissure",
      .player = { MON(SPECIES_DUGTRIO, 50, MOVE_LOCK_ON, MOVE_FISSURE, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckLockOnFissure },
    { .name = "lock_on_timer_after_one_turn",
      .player = { MON(SPECIES_DUGTRIO, 50, MOVE_LOCK_ON, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckLockOnTimer1 },
    { .name = "lock_on_expires_after_two_turns",
      .player = { MON(SPECIES_DUGTRIO, 50, MOVE_LOCK_ON, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckLockOnTimer0 },
    { .name = "lock_on_expired_fissure_vs_fly_fails",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_LOCK_ON, MOVE_SPLASH, MOVE_FISSURE, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_CLOYSTER, 50, MOVE_SPLASH, MOVE_FLY, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0), T(2, 1) }, .turns = 3, .check = CheckLockOnExpiredVsFly },
    { .name = "mind_reader_hits_flying",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_MIND_READER, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_PIDGEOT, 50, MOVE_SPLASH, MOVE_FLY, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .check = CheckMindReaderVsFly },
    { .name = "lock_on_50acc_move_hits",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_LOCK_ON, MOVE_DYNAMIC_PUNCH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckLockOnDynamicPunch },
    { .name = "lock_on_fissure_hits_flying",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_LOCK_ON, MOVE_FISSURE, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_CLOYSTER, 50, MOVE_SPLASH, MOVE_FLY, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .check = CheckLockOnFissureVsFly },
    { .name = "lock_on_fissure_blocked_by_protect",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_LOCK_ON, MOVE_FISSURE, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_CLOYSTER, 50, MOVE_SPLASH, MOVE_PROTECT, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .check = CheckLockOnFissureVsProtect },
    { .name = "lock_on_fissure_vs_higher_level_fails",
      .player = { MON(SPECIES_DUGTRIO, 50, MOVE_LOCK_ON, MOVE_FISSURE, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 51, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckLockOnHigherLevel },
    { .name = "lock_on_fissure_vs_sturdy",
      .player = { MON(SPECIES_DUGTRIO, 50, MOVE_LOCK_ON, MOVE_FISSURE, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SUDOWOODO, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckLockOnSturdy },
    { .name = "lock_on_fails_vs_substitute",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_LOCK_ON, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_DUGTRIO, 50, MOVE_SUBSTITUTE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckLockOnVsSubstitute },
    { .name = "lock_on_sure_hit_only_for_its_user",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_LOCK_ON, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_SLOWBRO, 50, MOVE_SPLASH, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_CLOYSTER, 50, MOVE_SPLASH, MOVE_FLY, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_SHUCKLE, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T4(0, 0, 0, 0), T4(1, 1, 1, 0) },
      .targets = { { 2, 0, 0, 0 }, { 2, 0, 2, 0 } },
      .turns = 2, .check = CheckLockOnOnlyForUser },

    // --- OHKO ---
    { .name = "ohko_same_level_can_hit",
      .player = { MON(SPECIES_DUGTRIO, 50, MOVE_FISSURE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantOhkoHit, .check = CheckOhkoHit },
    { .name = "ohko_same_level_can_miss",
      .player = { MON(SPECIES_DUGTRIO, 50, MOVE_FISSURE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantOhkoMiss, .check = CheckOhkoMiss },
    { .name = "ohko_big_level_gap_always_hits",
      .player = { MON(SPECIES_DUGTRIO, 100, MOVE_FISSURE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 1, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckOhkoLevelGap },
    { .name = "ohko_ignores_evasion",
      .player = { MON(SPECIES_DUGTRIO, 100, MOVE_SPLASH, MOVE_FISSURE, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 1, MOVE_MINIMIZE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T6(0, 0), T(1, 1) }, .turns = 7, .check = CheckOhkoIgnoresEvasion },
    { .name = "ohko_vs_fly_fails",
      .player = { MON(SPECIES_SHUCKLE, 100, MOVE_FISSURE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_CLOYSTER, 50, MOVE_FLY, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckOhkoVsFly },
    { .name = "ohko_vs_protect_blocked",
      .player = { MON(SPECIES_SHUCKLE, 100, MOVE_FISSURE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_CLOYSTER, 50, MOVE_PROTECT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckOhkoVsProtect },
    { .name = "ohko_vs_sturdy",
      .player = { MON(SPECIES_DUGTRIO, 100, MOVE_FISSURE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SUDOWOODO, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckOhkoSturdy },
    { .name = "horn_drill_vs_ghost_no_effect",
      .player = { MON(SPECIES_SHUCKLE, 100, MOVE_HORN_DRILL, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_GENGAR, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckHornDrillVsGhost },

    // --- critical hits (Snorlax Drill Peck = physical, non-STAB, no secondary; Surf = special) ---
    { .name = "focus_energy_flag_and_refail",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_FOCUS_ENERGY, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .check = CheckFocusEnergy },
    { .name = "crit_doubles_damage",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_DRILL_PECK, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantCritT0, .check = CheckCritDouble },
    { .name = "no_crit_normal_damage",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_DRILL_PECK, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantNoCrit, .check = CheckNoCritNormal },
    { .name = "crit_ignores_attacker_attack_drop",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_DRILL_PECK, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_CHARM, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0), T(0, 0), T(1, 1) }, .turns = 4, .wantSeed = WantCritT3, .check = CheckCritIgnoresAtkDrop },
    { .name = "crit_ignores_defender_defense_boost",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_DRILL_PECK, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_IRON_DEFENSE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0), T(0, 0), T(1, 1) }, .turns = 4, .wantSeed = WantCritT3, .check = CheckCritIgnoresDefBoost },
    { .name = "crit_keeps_attacker_attack_boost",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SWORDS_DANCE, MOVE_DRILL_PECK, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .wantSeed = WantCritT1, .check = CheckCritKeepsAtkBoost },
    { .name = "crit_keeps_defender_defense_drop",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_TAIL_WHIP, MOVE_DRILL_PECK, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0), T(1, 0) }, .turns = 3, .wantSeed = WantCritT2, .check = CheckCritKeepsDefDrop },
    { .name = "crit_ignores_reflect",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_DRILL_PECK, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_REFLECT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .wantSeed = WantCritT1, .check = CheckCritIgnoresReflect },
    { .name = "reflect_halves_non_crit",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_DRILL_PECK, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_REFLECT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .wantSeed = WantNoCrit, .check = CheckReflectHalvesNonCrit },
    { .name = "crit_ignores_light_screen",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SURF, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_LIGHT_SCREEN, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .wantSeed = WantCritT1, .check = CheckCritIgnoresLightScreen },
    { .name = "stick_focus_energy_slash_max_stage",
      .player = { MON_ITEM(SPECIES_FARFETCHD, 30, MOVE_FOCUS_ENERGY, MOVE_SLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_STICK) },
      .enemy = { MON(SPECIES_SHUCKLE, 100, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T16(1, 0) }, .turns = 17, .check = CheckHighCritStage },
    { .name = "lucky_punch_chansey_max_stage",
      .player = { MON_ITEM(SPECIES_CHANSEY, 50, MOVE_FOCUS_ENERGY, MOVE_SLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_LUCKY_PUNCH) },
      .enemy = { MON(SPECIES_SHUCKLE, 100, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T16(1, 0) }, .turns = 17, .check = CheckHighCritStage },
    { .name = "scope_lens_focus_energy_slash_max_stage",
      .player = { MON_ITEM(SPECIES_SNORLAX, 20, MOVE_FOCUS_ENERGY, MOVE_SLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_SCOPE_LENS) },
      .enemy = { MON(SPECIES_SHUCKLE, 100, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T16(1, 0) }, .turns = 17, .check = CheckHighCritStage },
    { .name = "battle_armor_blocks_crits",
      .player = { MON_ITEM(SPECIES_FARFETCHD, 30, MOVE_FOCUS_ENERGY, MOVE_SLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_STICK) },
      .enemy = { MON_AB(SPECIES_KABUTOPS, 100, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .actions = { T(0, 0), T16(1, 0) }, .turns = 17, .check = CheckArmorNoCrit },
    { .name = "shell_armor_blocks_crits",
      .player = { MON_ITEM(SPECIES_FARFETCHD, 30, MOVE_FOCUS_ENERGY, MOVE_SLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_STICK) },
      .enemy = { MON(SPECIES_CLOYSTER, 100, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T16(1, 0) }, .turns = 17, .check = CheckArmorNoCrit },
    { .name = "scope_lens_crit_is_x2",
      .player = { MON_ITEM(SPECIES_SNORLAX, 50, MOVE_DRILL_PECK, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_SCOPE_LENS) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantCritT0, .check = CheckScopeLensCrit },

    // --- always-hit moves still respect Protect ---
    { .name = "swift_vs_protect",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SWIFT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_CLOYSTER, 50, MOVE_PROTECT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSwiftVsProtect },
};

SCENARIO_GROUP(accuracy_crit, sScenarios)
