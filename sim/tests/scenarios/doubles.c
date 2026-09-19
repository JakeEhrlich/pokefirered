// Double battles: turn order among four battlers, explicit / redirected targeting, spread moves,
// partner-support moves, Lightning Rod / Follow Me redirection, faint replacements, side-wide effects.
//
// Battler ids: 0 = player left, 1 = opponent left, 2 = player right, 3 = opponent right.
// Damage expectations replicate CalculateBaseDamage (pokemon.c) + Cmd_damagecalc/typecalc/adjustnormaldamage:
// L50, 31 IVs, 0 EVs, Hardy, player side with all badges (+10% atk/def/spatk/spdef and speed).
#include "scenario.h"

#define SPLASH3 MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH
#define SPLASH4 MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH
#define M(sp, m1) MON(sp, 50, m1, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH)
#define M2(sp, m1, m2) MON(sp, 50, m1, m2, MOVE_SPLASH, MOVE_SPLASH)
#define IDLE(sp) MON(sp, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH)
#define HP1(sp, m1) { .species = sp, .level = 50, .moves = { m1, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 1, .hpSet = 1 }
#define TG(b) ((b) + 1)   // .targets entries are battler id + 1
#define LOST(b) ((int)MAXHP(b) - (int)HP(b))

// ---- expected damage ranges (random roll 85..100%) ----
struct DmgSpec
{
    int atk, def;      // battler ids
    int power;
    int special;       // move type is special
    int stab;
    int typeMul;       // 10 = neutral, 20 = 2x, 5 = 0.5x (0 = neutral)
    int spread;        // MOVE_TARGET_BOTH with two alive targets: damage halved
    int reflect;       // 1 = halved (one defender alive), 2 = two thirds (two alive)
    int halfDef;       // Explosion / Self-Destruct
    int mult;          // sDMG_MULTIPLIER (Dive/Dig bonus, Pursuit on switch); 0 = 1
    int helpingHand;
    int crit;
    int atkStat, defStat;  // raw stat overrides (battler was replaced since the hit); 0 = read from B()
};

static void DmgRange(struct BattleSim *sim, const struct DmgSpec *s, int *lo, int *hi)
{
    int atkStat = s->atkStat ? s->atkStat : (s->special ? B(s->atk).spAttack : B(s->atk).attack);
    int defStat = s->defStat ? s->defStat : (s->special ? B(s->def).spDefense : B(s->def).defense);
    int level = B(s->atk).level;
    int dmg;

    if ((s->atk & 1) == B_SIDE_PLAYER) atkStat = atkStat * 110 / 100;
    if ((s->def & 1) == B_SIDE_PLAYER) defStat = defStat * 110 / 100;
    if (s->halfDef) defStat /= 2;
    dmg = atkStat * s->power * (2 * level / 5 + 2);
    dmg = dmg / defStat;
    dmg /= 50;
    if (s->reflect == 2) dmg = 2 * (dmg / 3);
    else if (s->reflect == 1) dmg /= 2;
    if (s->spread) dmg /= 2;
    if (dmg == 0) dmg = 1;
    dmg += 2;
    dmg = dmg * (s->crit ? 2 : 1) * (s->mult ? s->mult : 1);
    if (s->helpingHand) dmg = dmg * 15 / 10;
    if (s->stab) dmg = dmg * 15 / 10;
    dmg = dmg * (s->typeMul ? s->typeMul : 10) / 10;
    *lo = dmg * 85 / 100;
    if (*lo == 0) *lo = 1;
    *hi = dmg;
}

static int InRange(struct BattleSim *sim, int lost, const struct DmgSpec *s, int hits)
{
    int lo, hi;
    DmgRange(sim, s, &lo, &hi);
    return lost >= lo * hits && lost <= hi * hits;
}
#define CHECK_LOST(b, hits, ...) do { struct DmgSpec s_ = { __VA_ARGS__ }; int lo_, hi_; DmgRange(sim, &s_, &lo_, &hi_); \
    CHECK(InRange(sim, LOST(b), &s_, hits), "battler %d lost %d, expected %d..%d (x%d)", b, LOST(b), lo_, hi_, hits); } while (0)

static int UsedMoveCount(struct BattleSim *sim, u8 battler, int turn)
{
    int n = 0;
    while (Sc_LogIndex(sim, STRINGID_USEDMOVE, battler, n, turn) >= 0)
        n++;
    return n;
}

static int WantNoCrit(struct BattleSim *sim) { return !LOG_HAS(STRINGID_CRITICALHIT); }

// ---------------------------------------------------------------- turn order

static void CheckSpeedOrder(struct BattleSim *sim)
{
    // Jolteon (150*1.1) > Electrode 160 > Golem 65 > Snorlax (50*1.1)
    CHECK(MOVED_BEFORE(0, 1, 0), "player jolteon before electrode");
    CHECK(MOVED_BEFORE(1, 3, 0), "electrode before golem");
    CHECK(MOVED_BEFORE(3, 2, 0), "golem before snorlax");
}

static void CheckPriorityOrder(struct BattleSim *sim)
{
    // Helping Hand (+5) < Quick Attack (+1) < Splash (0, fastest) < Counter (-5) < Roar (-6)
    CHECK(MOVED_BEFORE(2, 0, 0), "helping hand first");
    CHECK(MOVED_BEFORE(0, 1, 0), "quick attack before splash");
    CHECK(MOVED_BEFORE(1, 3, 0), "splash before counter");
    CHECK(LOG_INDEX_T(STRINGID_USEDMOVE, 3, 0) >= 0 && LOG_INDEX_T(STRINGID_USEDMOVE, 1, 0) < LOG_INDEX_T(STRINGID_USEDMOVE, 3, 0), "counter before roar");
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 0), "roar fails (fewer than 3 healthy mons) but was still used last");
}

static int WantQuickClawFirst(struct BattleSim *sim)
{
    return MOVED_BEFORE(2, 0, 0) && MOVED_BEFORE(2, 1, 0) && MOVED_BEFORE(2, 3, 0);
}
static void CheckQuickClaw(struct BattleSim *sim)
{
    CHECK(MOVED_BEFORE(2, 0, 0) && MOVED_BEFORE(2, 1, 0) && MOVED_BEFORE(2, 3, 0), "quick claw snorlax moved before all three");
    CHECK(MOVED_BEFORE(0, 1, 0) && MOVED_BEFORE(1, 3, 0), "others still in speed order");
}

static void CheckSwitchesFirst(struct BattleSim *sim)
{
    int s0 = LOG_INDEX_T(STRINGID_SWITCHINMON, 0, 0), s1 = LOG_INDEX_T(STRINGID_SWITCHINMON, 1, 0);
    int m2 = LOG_INDEX_T(STRINGID_USEDMOVE, 2, 0), m3 = LOG_INDEX_T(STRINGID_USEDMOVE, 3, 0);
    CHECK(s0 >= 0 && s1 >= 0 && m2 >= 0 && m3 >= 0, "all events logged (%d %d %d %d)", s0, s1, m2, m3);
    CHECK(s0 < s1, "switches in battler order (player left first)");
    CHECK(s1 < m3 && s1 < m2, "both switches before any move (even the fast electrode's)");
    CHECK(m3 < m2, "then moves by speed");
    CHECK(B(0).species == SPECIES_GOLEM && B(1).species == SPECIES_GOLEM, "switched in");
}

// ---------------------------------------------------------------- explicit targets

static void CheckExplicitRightFoe(struct BattleSim *sim)
{
    CHECK(HP(3) == MAXHP(3) - 100, "both seismic tosses hit the right foe (hp %d/%d)", HP(3), MAXHP(3));
    CHECK(HP(1) == MAXHP(1), "left foe untouched");
    CHECK(HP(2) == MAXHP(2), "partner untouched");
    {
        struct SimAction acts[32];
        int n = Sim_LegalActions(sim, 0, acts, 32);
        CHECK(n == 6, "toss x3 targets (foe, foe, partner) + splash x3 = 6 legal actions (%d)", n);
    }
}

static void CheckDefaultTargetOpposite(struct BattleSim *sim)
{
    CHECK(HP(1) == MAXHP(1) - 50, "left attacker defaults to the left foe");
    CHECK(HP(3) == MAXHP(3) - 50, "right attacker defaults to the right foe");
}

static void CheckTargetPartner(struct BattleSim *sim)
{
    CHECK(HP(2) == MAXHP(2) - 50, "partner took seismic toss (hp %d/%d)", HP(2), MAXHP(2));
    CHECK(HP(1) == MAXHP(1) && HP(3) == MAXHP(3), "foes untouched");
}

static void CheckFaintedTargetRedirect(struct BattleSim *sim)
{
    CHECK(sim->absentBattlerFlags & 2, "left foe absent (no replacement)");
    CHECK(HP(3) == MAXHP(3) - 50, "toss aimed at the fainted foe hit the other foe (hp %d/%d)", HP(3), MAXHP(3));
    CHECK(LOG_COUNT(STRINGID_TARGETFAINTED) == 1, "one faint");
    CHECK(OUTCOME() == 0, "battle continues");
    {
        // Sim_LegalActions: seismic toss can target partner (0) and the remaining foe (3) only; splash x3; no switches
        struct SimAction acts[32];
        int i, n = Sim_LegalActions(sim, 2, acts, 32), tosses = 0, absentTarget = 0;
        for (i = 0; i < n; i++)
            if (acts[i].type == B_ACTION_USE_MOVE && acts[i].moveSlot == 0)
            {
                tosses++;
                if (acts[i].target == 1 || acts[i].target == 2) absentTarget++;
            }
        CHECK(n == 5 && tosses == 2 && absentTarget == 0, "legal actions %d (tosses %d, bad targets %d)", n, tosses, absentTarget);
    }
}

static void CheckReplacedTargetHit(struct BattleSim *sim)
{
    CHECK(B(1).species == SPECIES_GOLEM, "golem replaced the fainted rattata mid-turn");
    CHECK(HP(1) == MAXHP(1) - 50, "the replacement took the toss aimed at that slot (hp %d/%d)", HP(1), MAXHP(1));
    CHECK(UsedMoveCount(sim, 1, 0) == 0, "neither the fainted mon nor its replacement acted");
    CHECK(HP(3) == MAXHP(3), "right foe untouched");
}

static void CheckFaintedPartnerTargetRedirect(struct BattleSim *sim)
{
    CHECK(sim->absentBattlerFlags & 4, "partner absent");
    CHECK(HP(1) == MAXHP(1) - 50, "toss aimed at the fainted partner went to the foe across (hp %d/%d)", HP(1), MAXHP(1));
    CHECK(HP(3) == MAXHP(3), "other foe untouched");
}

static int WantThrashHitsRight(struct BattleSim *sim) { return HP(3) < MAXHP(3) && !LOG_HAS(STRINGID_CRITICALHIT); }
static void CheckThrashRandomTarget(struct BattleSim *sim)
{
    CHECK(HP(3) < MAXHP(3) && HP(1) == MAXHP(1), "thrash picked the right foe this seed");
    CHECK_LOST(3, 1, .atk = 0, .def = 3, .power = 90, .stab = 1);
}

static void CheckThrashAbsentFoe(struct BattleSim *sim)
{
    CHECK(sim->absentBattlerFlags & 8, "right foe absent");
    CHECK(UsedMoveCount(sim, 0, 0) == 1 && UsedMoveCount(sim, 0, 1) == 1, "thrash used on both turns");
    CHECK_LOST(1, 2, .atk = 0, .def = 1, .power = 90, .stab = 1);
}

// ---------------------------------------------------------------- spread moves

static void CheckSurfBothHalved(struct BattleSim *sim)
{
    CHECK(UsedMoveCount(sim, 0, 0) == 1, "one attack string for both targets");
    CHECK_LOST(1, 1, .atk = 0, .def = 1, .power = 95, .special = 1, .stab = 1, .spread = 1);
    CHECK_LOST(3, 1, .atk = 0, .def = 3, .power = 95, .special = 1, .stab = 1, .spread = 1);
    CHECK(HP(2) == MAXHP(2), "partner not hit by surf");
}

static void CheckSurfSingleFoeFull(struct BattleSim *sim)
{
    CHECK(sim->absentBattlerFlags & 8, "right foe absent before surf");
    CHECK_LOST(1, 1, .atk = 0, .def = 1, .power = 95, .special = 1, .stab = 1, .spread = 0);
}

static void CheckSurfSecondTargetStillHalved(struct BattleSim *sim)
{
    CHECK(HP(1) == 0 && LOG_COUNT(STRINGID_TARGETFAINTED) == 1, "first target fainted");
    // gAbsentBattlerFlags is only set after the move, so the partner is still "alive" for the halving.
    CHECK_LOST(3, 1, .atk = 0, .def = 3, .power = 95, .special = 1, .stab = 1, .spread = 1);
}

static int WantOneAvoided(struct BattleSim *sim) { return LOG_COUNT(STRINGID_PKMNAVOIDEDATTACK) == 1; }
static void CheckRockSlideAccuracyPerTarget(struct BattleSim *sim)
{
    CHECK(LOG_COUNT(STRINGID_PKMNAVOIDEDATTACK) == 1, "exactly one target avoided the attack");
    CHECK((HP(1) == MAXHP(1)) != (HP(3) == MAXHP(3)), "one foe hit, the other untouched (%d/%d, %d/%d)", HP(1), MAXHP(1), HP(3), MAXHP(3));
}

static int WantOneCrit(struct BattleSim *sim) { return LOG_COUNT(STRINGID_CRITICALHIT) == 1; }
static void CheckSurfCritPerTarget(struct BattleSim *sim)
{
    struct DmgSpec n1 = { .atk = 0, .def = 1, .power = 95, .special = 1, .stab = 1, .spread = 1 };
    struct DmgSpec c1 = n1; c1.crit = 1;
    struct DmgSpec n3 = n1; n3.def = 3;
    struct DmgSpec c3 = c1; c3.def = 3;
    CHECK(LOG_COUNT(STRINGID_CRITICALHIT) == 1, "one crit");
    CHECK((InRange(sim, LOST(1), &c1, 1) && InRange(sim, LOST(3), &n3, 1))
       || (InRange(sim, LOST(1), &n1, 1) && InRange(sim, LOST(3), &c3, 1)), "crit rolled for one target only (lost %d, %d)", LOST(1), LOST(3));
}

static void CheckEarthquakeFull(struct BattleSim *sim)
{
    CHECK(UsedMoveCount(sim, 0, 0) == 1, "one attack string");
    // FOES_AND_ALLY moves are not halved (only MOVE_TARGET_BOTH is)
    CHECK_LOST(1, 1, .atk = 0, .def = 1, .power = 100, .stab = 1);
    CHECK_LOST(3, 1, .atk = 0, .def = 3, .power = 100, .stab = 1);
    CHECK_LOST(2, 1, .atk = 0, .def = 2, .power = 100, .stab = 1);
}

static void CheckEarthquakeLevitatePartner(struct BattleSim *sim)
{
    CHECK(HP(2) == MAXHP(2), "levitating partner unhurt");
    CHECK(LOG_HAS(STRINGID_PKMNMAKESGROUNDMISS), "levitate message");
    CHECK(HP(1) < MAXHP(1) && HP(3) < MAXHP(3), "foes hit");
}

static void CheckEarthquakeDugPartner(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNDUGHOLE, 0), "partner dug on turn 1");
    CHECK_LOST(2, 1, .atk = 0, .def = 2, .power = 100, .stab = 1, .mult = 2);
    CHECK_LOST(1, 1, .atk = 0, .def = 1, .power = 100, .stab = 1);
}

static void CheckEarthquakeAbsentFoe(struct BattleSim *sim)
{
    CHECK(sim->absentBattlerFlags & 8, "right foe absent");
    CHECK(HP(1) < MAXHP(1) && HP(2) < MAXHP(2), "left foe and partner hit");
    CHECK(UsedMoveCount(sim, 0, 0) == 1, "one attack string");
}

static void CheckEarthquakeAbsentPartner(struct BattleSim *sim)
{
    CHECK(sim->absentBattlerFlags & 4, "partner absent");
    CHECK(HP(1) < MAXHP(1) && HP(3) < MAXHP(3), "both foes hit");
}

static void CheckExplosionAll(struct BattleSim *sim)
{
    CHECK(HP(0) == 0 || B(0).species == SPECIES_JOLTEON, "electrode fainted");
    CHECK(LOG_HAS(STRINGID_ATTACKERFAINTED), "attacker faint message");
    CHECK(B(0).species == SPECIES_JOLTEON, "replacement sent out mid-turn");
    // battler 0 is already the replacement: take the electrode's attack from the party
    int atk = GetMonData(&sim->playerParty[0], MON_DATA_ATK);
    CHECK_LOST(1, 1, .atk = 0, .def = 1, .power = 250, .halfDef = 1, .atkStat = atk);
    CHECK_LOST(3, 1, .atk = 0, .def = 3, .power = 250, .halfDef = 1, .atkStat = atk);
    CHECK_LOST(2, 1, .atk = 0, .def = 2, .power = 250, .halfDef = 1, .atkStat = atk);
}

static void CheckExplosionDamp(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNPREVENTSUSAGE), "damp message");
    CHECK(HP(0) == MAXHP(0), "user did not faint");
    CHECK(HP(1) == MAXHP(1) && HP(2) == MAXHP(2) && HP(3) == MAXHP(3), "nobody hurt");
    CHECK(PP(0, 0) == 4, "pp still deducted (5 -> %d)", PP(0, 0));
}

static void CheckExplosionAbsentFoe(struct BattleSim *sim)
{
    CHECK(sim->absentBattlerFlags & 2, "left foe absent");
    CHECK(HP(0) == 0, "voltorb fainted");
    CHECK(HP(2) < MAXHP(2) && HP(3) < MAXHP(3), "partner and remaining foe hit");
    CHECK(LOG_COUNT(STRINGID_TARGETFAINTED) == 1, "only the thunderbolt KO");
}

// ---------------------------------------------------------------- partner support

static void CheckHelpingHand(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNREADYTOHELP), "helping hand message");
    CHECK(MOVED_BEFORE(0, 2, 0), "helping hand (+5) before the partner's strength");
    CHECK_LOST(1, 1, .atk = 2, .def = 1, .power = 80, .helpingHand = 1);
}

static void CheckHelpingHandPartnerAbsent(struct BattleSim *sim)
{
    CHECK(sim->absentBattlerFlags & 4, "partner absent");
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 1), "helping hand fails with no partner");
    CHECK(!LOG_HAS(STRINGID_PKMNREADYTOHELP), "no help message");
}

static void CheckHelpingHandBoth(struct BattleSim *sim)
{
    CHECK(MOVED_BEFORE(2, 0, 0), "faster alakazam first");
    CHECK(LOG_COUNT(STRINGID_PKMNREADYTOHELP) == 1, "only the first helping hand works");
    CHECK(LOG_COUNT(STRINGID_BUTITFAILED) == 1, "second fails (its user is already helped)");
}

static void CheckFollowMe(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNCENTERATTENTION), "follow me message");
    CHECK(HP(0) == MAXHP(0), "left mon not hit");
    CHECK(HP(2) == MAXHP(2) - 100, "both tosses redirected to the follow me user (hp %d/%d)", HP(2), MAXHP(2));
}

static void CheckFollowMeExpires(struct BattleSim *sim)
{
    CHECK(HP(2) == MAXHP(2) - 100, "turn 1 redirected");
    CHECK(HP(0) == MAXHP(0) - 100, "turn 2 no longer redirected");
}

static void CheckFollowMeUserFainted(struct BattleSim *sim)
{
    CHECK(HP(2) == 0 || (sim->absentBattlerFlags & 4), "follow me user KO'd by quick attack");
    CHECK(HP(0) == MAXHP(0) - 50, "later toss went to its original target");
}

static int WantThrashHitsLeft(struct BattleSim *sim) { return HP(0) < MAXHP(0); }
static void CheckFollowMeIgnoredByRandomTarget(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNCENTERATTENTION), "follow me used");
    CHECK(HP(0) < MAXHP(0) && HP(2) == MAXHP(2), "thrash (random target) is not pulled by follow me");
}

static void CheckFollowMeSpread(struct BattleSim *sim)
{
    CHECK(HP(0) < MAXHP(0) && HP(2) < MAXHP(2), "surf still hits both");
}

static void CheckLightningRodThunderbolt(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNSXTOOKATTACK), "took attack message");
    CHECK(HP(1) == MAXHP(1), "intended target untouched");
    CHECK_LOST(3, 1, .atk = 0, .def = 3, .power = 95, .special = 1, .stab = 1, .typeMul = 5);
}

static void CheckLightningRodThunderWave(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNSXTOOKATTACK), "status move also redirected");
    CHECK(STATUS1(3) & STATUS1_PARALYSIS, "rod holder paralyzed");
    CHECK(STATUS1(1) == 0, "intended target not paralyzed");
}

static void CheckLightningRodDirect(struct BattleSim *sim)
{
    CHECK(!LOG_HAS(STRINGID_PKMNSXTOOKATTACK), "no redirect message when the rod is the target");
    CHECK(HP(3) < MAXHP(3) && HP(1) == MAXHP(1), "rod holder hit");
}

static void CheckLightningRodOwnSide(struct BattleSim *sim)
{
    CHECK(!LOG_HAS(STRINGID_PKMNSXTOOKATTACK), "own partner's rod does not pull");
    CHECK(HP(1) < MAXHP(1) && HP(2) == MAXHP(2), "foe hit, partner not");
}

// ---------------------------------------------------------------- side-wide effects

static void CheckReflectTwoThirds(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNRAISEDDEFALITTLE), "doubles reflect message");
    CHECK(SIDE_STATUS(B_SIDE_PLAYER) & SIDE_STATUS_REFLECT, "reflect up");
    CHECK_LOST(0, 1, .atk = 1, .def = 0, .power = 80, .reflect = 2);
}

static void CheckReflectHalfAlone(struct BattleSim *sim)
{
    CHECK(sim->absentBattlerFlags & 4, "partner absent");
    CHECK(LOG_HAS_T(STRINGID_PKMNRAISEDDEF, 1), "singles-style reflect message");
    CHECK_LOST(0, 1, .atk = 1, .def = 0, .power = 80, .reflect = 1);
}

static void CheckSafeguardPartner(struct BattleSim *sim)
{
    CHECK(SIDE_STATUS(B_SIDE_PLAYER) & SIDE_STATUS_SAFEGUARD, "safeguard up");
    CHECK(LOG_HAS(STRINGID_PKMNCOVEREDBYVEIL), "veil message");
    CHECK(STATUS1(2) == 0, "partner protected from thunder wave");
    CHECK(LOG_HAS(STRINGID_PKMNUSEDSAFEGUARD), "safeguard protected message");
}

static void CheckHealBell(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_BELLCHIMED), "bell chimed");
    CHECK(STATUS1(0) == 0, "user cured");
    CHECK(STATUS1(2) == 0, "partner cured");
    CHECK(PARTY_STATUS(B_SIDE_PLAYER, 2) == 0, "reserve cured");
}

static void CheckHealBellSoundproofPartner(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_BELLCHIMED), "bell chimed");
    CHECK(LOG_HAS(STRINGID_PKMNSXBLOCKSY2), "partner's soundproof blocks");
    CHECK(STATUS1(2) & STATUS1_PARALYSIS, "soundproof partner still paralyzed");
    CHECK(PARTY_STATUS(B_SIDE_PLAYER, 2) == 0, "reserve cured");
}

static void CheckAromatherapy(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_SOOTHINGAROMA), "aroma message");
    CHECK(STATUS1(2) == 0, "aromatherapy ignores the partner's soundproof");
    CHECK(PARTY_STATUS(B_SIDE_PLAYER, 2) == 0, "reserve cured");
}

static void CheckPerishSong(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_FAINTINTHREE), "perish song message");
    CHECK((STATUS3(0) & STATUS3_PERISH_SONG) && (STATUS3(1) & STATUS3_PERISH_SONG) && (STATUS3(2) & STATUS3_PERISH_SONG), "three battlers affected");
    CHECK(!(STATUS3(3) & STATUS3_PERISH_SONG), "soundproof electrode unaffected");
    CHECK(sim->disableStructs[0].perishSongTimer == 2 && sim->disableStructs[1].perishSongTimer == 2 && sim->disableStructs[2].perishSongTimer == 2, "counters 3 -> 2 after the first end of turn");
    CHECK(LOG_COUNT(STRINGID_PKMNPERISHCOUNTFELL) == 3, "three count-down messages");
    // end-of-turn effects run in speed order: misdreavus (115) > player snorlax (55) > enemy snorlax (50)
    CHECK(LOG_INDEX_T(STRINGID_PKMNPERISHCOUNTFELL, 0, 0) < LOG_INDEX_T(STRINGID_PKMNPERISHCOUNTFELL, 2, 0)
       && LOG_INDEX_T(STRINGID_PKMNPERISHCOUNTFELL, 2, 0) < LOG_INDEX_T(STRINGID_PKMNPERISHCOUNTFELL, 1, 0), "counters fall in speed order");
}

static void CheckPerishSongDraw(struct BattleSim *sim)
{
    CHECK(HP(0) == 0 && HP(1) == 0 && HP(2) == 0 && HP(3) == 0, "all four fainted");
    CHECK(OUTCOME() == B_OUTCOME_DREW, "outcome %d (expected draw)", OUTCOME());
    CHECK(Sc_LastTurn(sim) == 3, "everyone fainted at the end of turn 4 (last turn %d)", Sc_LastTurn(sim));
    // sEndTurnFuncsTable[B_OUTCOME_DREW] == HandleEndTurn_BattleLost: a draw is presented as a loss
    CHECK(LOG_HAS(STRINGID_PLAYERLOSTAGAINSTENEMYTRAINER), "draw shows the trainer-loss message");
}

static void CheckUproarWakes(struct BattleSim *sim)
{
    CHECK(!(STATUS1(1) & STATUS1_SLEEP), "left foe woke up");
    CHECK(!(STATUS1(2) & STATUS1_SLEEP), "partner woke up");
    CHECK(STATUS1(3) & STATUS1_SLEEP, "soundproof electrode keeps sleeping");
    CHECK(LOG_COUNT(STRINGID_PKMNWOKEUPINUPROAR) == 2, "two woke up (%d)", LOG_COUNT(STRINGID_PKMNWOKEUPINUPROAR));
}

static void CheckIntimidateBothFoes(struct BattleSim *sim)
{
    CHECK(STAGE(1, STAT_ATK) == 5 && STAGE(3, STAT_ATK) == 5, "gyarados lowered both foes");
    CHECK(STAGE(0, STAT_ATK) == 5 && STAGE(2, STAT_ATK) == 5, "arcanine lowered both player mons");
    CHECK(LOG_COUNT(STRINGID_PKMNCUTSATTACKWITH) == 4, "four intimidate messages (%d)", LOG_COUNT(STRINGID_PKMNCUTSATTACKWITH));
}

static void CheckIntimidateBlocked(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_GYARADOS, "gyarados switched in");
    CHECK(STATUS2(1) & STATUS2_SUBSTITUTE, "foe behind substitute");
    CHECK(STAGE(1, STAT_ATK) == 6, "substitute blocks intimidate");
    CHECK(STAGE(3, STAT_ATK) == 6, "clear body blocks intimidate");
    CHECK(LOG_HAS_T(STRINGID_PREVENTEDFROMWORKING, 1), "clear body message");
    CHECK(!LOG_HAS_T(STRINGID_PKMNCUTSATTACKWITH, 1), "nobody was lowered");
}

static void CheckSkillSwapPartner(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNSWAPPEDABILITIES), "swapped");
    CHECK(B(0).ability == ABILITY_GUTS, "alakazam now has guts (%d)", B(0).ability);
    CHECK(B(2).ability == ABILITY_SYNCHRONIZE, "machamp now has synchronize (%d)", B(2).ability);
}

static void CheckRolePlayPartner(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNCOPIEDFOE), "copied");
    CHECK(B(0).ability == ABILITY_GUTS && B(2).ability == ABILITY_GUTS, "both guts");
}

static void CheckCounterLastAttacker(struct BattleSim *sim)
{
    CHECK(MOVED_BEFORE(3, 1, 0), "hitmonlee hits before machamp");
    CHECK(HP(0) == MAXHP(0) - 100, "snorlax took two tosses");
    CHECK(HP(1) == MAXHP(1) - 100, "counter hit the last physical attacker for double (hp %d/%d)", HP(1), MAXHP(1));
    CHECK(HP(3) == MAXHP(3), "earlier attacker untouched");
}

static void CheckCounterPartnerFails(struct BattleSim *sim)
{
    CHECK(HP(0) == MAXHP(0) - 50, "hit by partner");
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "counter fails against a same-side hit");
    CHECK(HP(1) == MAXHP(1) && HP(3) == MAXHP(3) && HP(2) == MAXHP(2), "nobody countered");
}

static void CheckCounterFollowMe(struct BattleSim *sim)
{
    CHECK(HP(0) == MAXHP(0) - 50, "hit by machamp");
    CHECK(HP(1) == MAXHP(1) - 100, "counter pulled by follow me (hp %d/%d)", HP(1), MAXHP(1));
    CHECK(HP(3) == MAXHP(3), "actual attacker untouched");
}

static int WantMirrorCoatUsed(struct BattleSim *sim) { return LOG_HAS_MOVE(STRINGID_USEDMOVE, MOVE_MIRROR_COAT) && HP(0) < MAXHP(0) && !LOG_HAS(STRINGID_BUTITFAILED); }
static void CheckMirrorCoat(struct BattleSim *sim)
{
    CHECK(HP(0) < MAXHP(0), "slowbro was hit");
    CHECK(LOST(3) == 2 * LOST(0), "mirror coat returned double (%d vs %d)", LOST(3), LOST(0));
    CHECK(HP(1) == MAXHP(1), "other foe untouched");
}

// ---------------------------------------------------------------- switching / faints

static void CheckPursuitSwitching(struct BattleSim *sim)
{
    int max0 = GetMonData(&sim->enemyParty[0], MON_DATA_MAX_HP);
    struct DmgSpec s = { .atk = 0, .def = 1, .power = 40, .special = 1, .mult = 2, .defStat = GetMonData(&sim->enemyParty[0], MON_DATA_SPDEF) };
    int lo, hi;
    DmgRange(sim, &s, &lo, &hi);
    CHECK(B(1).species == SPECIES_GOLEM && HP(1) == MAXHP(1), "replacement came in unhurt");
    CHECK(max0 - PARTY_HP(B_SIDE_OPPONENT, 0) >= lo && max0 - PARTY_HP(B_SIDE_OPPONENT, 0) <= hi, "switching snorlax took doubled pursuit before leaving (lost %d, expected %d..%d)", max0 - PARTY_HP(B_SIDE_OPPONENT, 0), lo, hi);
    CHECK_LOST(3, 1, .atk = 2, .def = 3, .power = 40, .special = 1);
    CHECK(UsedMoveCount(sim, 0, 0) == 1 && UsedMoveCount(sim, 2, 0) == 1, "each pursuit used once");
}

static void CheckPursuitBothPartners(struct BattleSim *sim)
{
    int max0 = GetMonData(&sim->enemyParty[0], MON_DATA_MAX_HP);
    struct DmgSpec s = { .atk = 0, .def = 1, .power = 40, .special = 1, .mult = 2, .defStat = GetMonData(&sim->enemyParty[0], MON_DATA_SPDEF) };
    int lo, hi;
    DmgRange(sim, &s, &lo, &hi);
    CHECK(B(1).species == SPECIES_GOLEM && HP(1) == MAXHP(1), "replacement unhurt");
    CHECK(max0 - PARTY_HP(B_SIDE_OPPONENT, 0) >= 2 * lo && max0 - PARTY_HP(B_SIDE_OPPONENT, 0) <= 2 * hi, "both partners' pursuits hit the switching mon (lost %d, expected %d..%d)", max0 - PARTY_HP(B_SIDE_OPPONENT, 0), 2 * lo, 2 * hi);
    CHECK(HP(3) == MAXHP(3), "right foe untouched");
    CHECK(LOG_INDEX_T(STRINGID_USEDMOVE, 2, 0) < LOG_INDEX_T(STRINGID_USEDMOVE, 0, 0), "right partner's pursuit resolves first");
}

static int WantSuperFangHit(struct BattleSim *sim) { return HP(1) != MAXHP(1); }
static void CheckEndeavorSuperFang(struct BattleSim *sim)
{
    CHECK(HP(3) == 10, "endeavor brought the right foe to the user's hp (%d)", HP(3));
    CHECK(HP(1) == MAXHP(1) - MAXHP(1) / 2, "super fang halved the left foe (%d/%d)", HP(1), MAXHP(1));
}

static void CheckDoubleFaintReplacements(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_JOLTEON, "player left replaced by slot 2");
    CHECK(B(1).species == SPECIES_GOLEM, "opponent left replaced by slot 2");
    CHECK(B(3).species == SPECIES_SNORLAX, "opponent right replaced by slot 3");
    CHECK(HP(1) == MAXHP(1) - 50, "later toss hit the left replacement (hp %d/%d)", HP(1), MAXHP(1));
    CHECK(HP(3) == MAXHP(3) && HP(0) == MAXHP(0), "other replacements untouched");
    CHECK(UsedMoveCount(sim, 1, 0) == 0 && UsedMoveCount(sim, 3, 0) == 0, "fainted foes / replacements did not act");
    CHECK(LOG_COUNT(STRINGID_TARGETFAINTED) == 2 && LOG_COUNT(STRINGID_ATTACKERFAINTED) == 1, "three faints");
}

static void CheckBatonPass(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_SNORLAX, "baton passed to snorlax");
    CHECK(STAGE(0, STAT_ATK) == 8, "swords dance boost passed");
    CHECK(HP(0) == MAXHP(0) - 50, "snorlax took the toss aimed at that slot after the pass");
    CHECK(PARTY_HP(B_SIDE_PLAYER, 0) == GetMonData(&sim->playerParty[0], MON_DATA_MAX_HP) - 50, "scyther kept turn-1 damage only");
}

static int WantRoarPidgey(struct BattleSim *sim) { return B(1).species == SPECIES_PIDGEY; }
static void CheckRoarRandom(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNWASDRAGGEDOUT), "dragged out");
    CHECK(B(1).species == SPECIES_PIDGEY, "random reserve (slot 3) chosen this seed");
    CHECK(B(3).species == SPECIES_SNORLAX, "partner untouched");
}

static void CheckRoarFailsTwoMons(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "roar fails with fewer than 3 healthy mons");
    CHECK(B(1).species == SPECIES_RATTATA, "no switch");
}

static void CheckRoarThreeMons(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNWASDRAGGEDOUT), "dragged out");
    CHECK(B(1).species == SPECIES_GOLEM, "only reserve forced in");
}

static void CheckSpikesEachSwitchIn(struct BattleSim *sim)
{
    CHECK(SIDE_STATUS(B_SIDE_OPPONENT) & SIDE_STATUS_SPIKES, "spikes on the opponent side");
    CHECK(B(1).species == SPECIES_GOLEM && B(3).species == SPECIES_SNORLAX, "both switched");
    CHECK(LOG_COUNT(STRINGID_PKMNHURTBYSPIKES) == 2, "two spikes hits (%d)", LOG_COUNT(STRINGID_PKMNHURTBYSPIKES));
    CHECK(HP(1) == MAXHP(1) - MAXHP(1) / 8, "golem 1/8 (%d/%d)", HP(1), MAXHP(1));
    CHECK(HP(3) == MAXHP(3) - MAXHP(3) / 8, "snorlax 1/8 (%d/%d)", HP(3), MAXHP(3));
}

static void CheckFlyDodges(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNFLEWHIGH), "pidgeot flew up first");
    CHECK(LOG_HAS(STRINGID_ATTACKMISSED) && HP(1) == MAXHP(1), "toss at the flying foe missed");
    CHECK(HP(3) == MAXHP(3) - 50, "partner's toss on the other foe hit");
}

static void CheckSurfHitsDiver(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNHIDUNDERWATER), "persian dove");
    CHECK_LOST(1, 1, .atk = 0, .def = 1, .power = 95, .special = 1, .stab = 1, .spread = 1);
    CHECK_LOST(3, 1, .atk = 0, .def = 3, .power = 95, .special = 1, .stab = 1, .spread = 1, .mult = 2);
}

static void CheckSubstituteSpread(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_SUBSTITUTEDAMAGED), "substitute took the hit");
    CHECK(HP(1) == MAXHP(1) - MAXHP(1) / 4, "alakazam only paid for the substitute");
    CHECK(HP(3) < MAXHP(3), "other foe hit directly");
}

static void CheckGhostCurseRight(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNLAIDCURSE), "curse laid");
    CHECK(STATUS2(3) & STATUS2_CURSED, "explicit target cursed");
    CHECK(!(STATUS2(1) & STATUS2_CURSED) && !(STATUS2(2) & STATUS2_CURSED), "nobody else cursed");
    CHECK(HP(0) == MAXHP(0) - MAXHP(0) / 2, "gengar paid half hp");
    CHECK(HP(3) == MAXHP(3) - MAXHP(3) / 4, "cursed foe lost 1/4 at end of turn (%d/%d)", HP(3), MAXHP(3));
}

static void CheckGhostCursePartner(struct BattleSim *sim)
{
    CHECK(STATUS2(2) & STATUS2_CURSED, "partner cursed");
    CHECK(HP(2) == MAXHP(2) - MAXHP(2) / 4, "partner lost 1/4");
    CHECK(HP(0) == MAXHP(0) - MAXHP(0) / 2, "gengar paid half hp");
}

static void CheckGrowlBothFoes(struct BattleSim *sim)
{
    CHECK(STAGE(1, STAT_ATK) == 5, "left foe lowered");
    CHECK(STAGE(3, STAT_ATK) == 6, "clear body foe not lowered");
    CHECK(LOG_HAS(STRINGID_PKMNPREVENTSSTATLOSSWITH), "clear body message");
    CHECK(STAGE(2, STAT_ATK) == 6, "partner untouched");
    CHECK(UsedMoveCount(sim, 0, 0) == 1, "one attack string");
}

static void CheckProtectOneFoe(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNPROTECTEDITSELF), "protected message");
    CHECK(HP(1) == MAXHP(1), "protecting foe unhurt");
    CHECK_LOST(3, 1, .atk = 0, .def = 3, .power = 95, .special = 1, .stab = 1, .spread = 1);
}

static int WantStruggleRight(struct BattleSim *sim) { return HP(3) < MAXHP(3); }
static void CheckStruggleRandom(struct BattleSim *sim)
{
    CHECK(LOG_HAS_MOVE(STRINGID_USEDMOVE, MOVE_STRUGGLE), "struggled");
    CHECK(HP(3) < MAXHP(3) && HP(1) == MAXHP(1), "struggle picked the right foe this seed");
    CHECK(HP(0) < MAXHP(0), "recoil");
}

static void CheckAbsentBattlerNoPartnerTarget(struct BattleSim *sim)
{
    // Player's partner is gone; the foe's move that targeted slot 2 hits the foe-facing redirect target.
    CHECK(sim->absentBattlerFlags & 4, "partner absent");
    CHECK(HP(0) == MAXHP(0) - 50, "foe's toss aimed at the empty slot went to the flank partner (hp %d/%d)", HP(0), MAXHP(0));
}

static int WantTraceRockHead(struct BattleSim *sim) { return B(0).ability == ABILITY_ROCK_HEAD; }
static void CheckTraceRandomFoe(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNTRACED), "trace message");
    CHECK(B(0).ability == ABILITY_ROCK_HEAD, "traced the right foe's ability this seed (%d)", B(0).ability);
}

static void CheckCurseNonGhostIgnoresTarget(struct BattleSim *sim)
{
    CHECK(STAGE(0, STAT_ATK) == 7 && STAGE(0, STAT_DEF) == 7 && STAGE(0, STAT_SPEED) == 5, "stat curse on the user");
    CHECK(!(STATUS2(3) & STATUS2_CURSED) && !(STATUS2(1) & STATUS2_CURSED), "the chosen target is ignored");
    CHECK(HP(0) == MAXHP(0), "no hp cost");
}

static void CheckMistProtectsBoth(struct BattleSim *sim)
{
    CHECK(SIDE_STATUS(B_SIDE_PLAYER) & SIDE_STATUS_MIST, "mist up");
    CHECK(STAGE(0, STAT_ATK) == 6 && STAGE(2, STAT_ATK) == 6, "growl blocked on both");
    CHECK(LOG_COUNT(STRINGID_PKMNPROTECTEDBYMIST) == 2, "one mist message per target (%d)", LOG_COUNT(STRINGID_PKMNPROTECTEDBYMIST));
}

static void CheckExplosionAbsentPartner(struct BattleSim *sim)
{
    CHECK(sim->absentBattlerFlags & 4, "partner absent");
    CHECK(HP(0) == 0, "exploded");
    CHECK(HP(1) < MAXHP(1) && HP(3) < MAXHP(3), "both foes hit");
    CHECK(OUTCOME() == B_OUTCOME_LOST, "player side is out of mons (outcome %d)", OUTCOME());
}

static const struct Scenario sScenarios[] =
{
    // ---- turn order
    { .name = "doubles_speed_order",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { IDLE(SPECIES_JOLTEON), IDLE(SPECIES_SNORLAX) },
      .enemy  = { IDLE(SPECIES_ELECTRODE), IDLE(SPECIES_GOLEM) },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .check = CheckSpeedOrder },
    { .name = "doubles_priority_order",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_SNORLAX, MOVE_QUICK_ATTACK), M(SPECIES_MACHAMP, MOVE_HELPING_HAND) },
      .enemy  = { M(SPECIES_ELECTRODE, MOVE_COUNTER), M(SPECIES_GOLEM, MOVE_ROAR) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(TG(1), 0, 0, TG(0)) }, .turns = 1, .check = CheckPriorityOrder },
    { .name = "doubles_quick_claw",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { IDLE(SPECIES_JOLTEON), MON_ITEM(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_QUICK_CLAW) },
      .enemy  = { IDLE(SPECIES_ELECTRODE), IDLE(SPECIES_GOLEM) },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .wantSeed = WantQuickClawFirst, .check = CheckQuickClaw },
    { .name = "doubles_switches_before_moves",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_MACHAMP), IDLE(SPECIES_GOLEM) },
      .enemy  = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_ELECTRODE), IDLE(SPECIES_GOLEM) },
      .actions = { T4(SC_SWITCH(2), SC_SWITCH(2), 0, 0) }, .turns = 1, .check = CheckSwitchesFirst },

    // ---- explicit targets
    { .name = "doubles_explicit_right_foe",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_MACHAMP, MOVE_SEISMIC_TOSS), M(SPECIES_MACHAMP, MOVE_SEISMIC_TOSS) },
      .enemy  = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(TG(3), 0, TG(3), 0) }, .turns = 1, .check = CheckExplicitRightFoe },
    { .name = "doubles_default_target_opposite",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_MACHAMP, MOVE_SEISMIC_TOSS), M(SPECIES_MACHAMP, MOVE_SEISMIC_TOSS) },
      .enemy  = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .check = CheckDefaultTargetOpposite },
    { .name = "doubles_target_partner",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_MACHAMP, MOVE_SEISMIC_TOSS), IDLE(SPECIES_SNORLAX) },
      .enemy  = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(TG(2), 0, 0, 0) }, .turns = 1, .check = CheckTargetPartner },
    { .name = "doubles_fainted_target_redirects_to_flank",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_JOLTEON, MOVE_THUNDERBOLT), M(SPECIES_MACHAMP, MOVE_SEISMIC_TOSS) },
      .enemy  = { HP1(SPECIES_RATTATA, MOVE_SPLASH), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(TG(1), 0, TG(1), 0) }, .turns = 1, .check = CheckFaintedTargetRedirect },
    { .name = "doubles_replacement_takes_pending_hit",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_JOLTEON, MOVE_THUNDERBOLT), M(SPECIES_MACHAMP, MOVE_SEISMIC_TOSS) },
      .enemy  = { HP1(SPECIES_RATTATA, MOVE_SPLASH), IDLE(SPECIES_SNORLAX), IDLE(SPECIES_GOLEM) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(TG(1), 0, TG(1), 0) }, .turns = 1, .check = CheckReplacedTargetHit },
    { .name = "doubles_fainted_partner_target_redirects_to_foe",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_MACHAMP, MOVE_SEISMIC_TOSS), HP1(SPECIES_RATTATA, MOVE_SPLASH) },
      .enemy  = { M(SPECIES_ELECTRODE, MOVE_THUNDERBOLT), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(TG(2), TG(2), 0, 0) }, .turns = 1, .check = CheckFaintedPartnerTargetRedirect },
    { .name = "doubles_foe_targets_absent_partner",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { IDLE(SPECIES_SNORLAX), HP1(SPECIES_RATTATA, MOVE_SPLASH) },
      .enemy  = { M(SPECIES_ELECTRODE, MOVE_THUNDERBOLT), M(SPECIES_MACHAMP, MOVE_SEISMIC_TOSS) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(0, TG(2), 0, TG(2)) }, .turns = 1, .check = CheckAbsentBattlerNoPartnerTarget },
    { .name = "doubles_thrash_random_target",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_TAUROS, MOVE_THRASH), IDLE(SPECIES_SNORLAX) },
      .enemy  = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .wantSeed = WantThrashHitsRight, .check = CheckThrashRandomTarget },
    { .name = "doubles_thrash_absent_foe_always_other",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_TAUROS, MOVE_THRASH), M(SPECIES_JOLTEON, MOVE_THUNDERBOLT) },
      .enemy  = { IDLE(SPECIES_SNORLAX), HP1(SPECIES_RATTATA, MOVE_SPLASH) },
      .actions = { T4(0, 0, 0, 0), T4(0, 0, 1, 0) }, .targets = { T4(0, 0, TG(3), 0) }, .turns = 2, .wantSeed = WantNoCrit, .check = CheckThrashAbsentFoe },

    // ---- spread moves
    { .name = "doubles_surf_both_halved",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_LAPRAS, MOVE_SURF), IDLE(SPECIES_MACHAMP) },
      .enemy  = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .wantSeed = WantNoCrit, .check = CheckSurfBothHalved },
    { .name = "doubles_surf_single_foe_full",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_LAPRAS, MOVE_SURF), M(SPECIES_JOLTEON, MOVE_THUNDERBOLT) },
      .enemy  = { IDLE(SPECIES_SNORLAX), HP1(SPECIES_RATTATA, MOVE_SPLASH) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(0, 0, TG(3), 0) }, .turns = 1, .wantSeed = WantNoCrit, .check = CheckSurfSingleFoeFull },
    { .name = "doubles_surf_second_target_halved_after_first_ko",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_LAPRAS, MOVE_SURF), IDLE(SPECIES_MACHAMP) },
      .enemy  = { HP1(SPECIES_RATTATA, MOVE_SPLASH), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .wantSeed = WantNoCrit, .check = CheckSurfSecondTargetStillHalved },
    { .name = "doubles_rock_slide_accuracy_per_target",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_GOLEM, MOVE_ROCK_SLIDE), IDLE(SPECIES_MACHAMP) },
      .enemy  = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .wantSeed = WantOneAvoided, .check = CheckRockSlideAccuracyPerTarget },
    { .name = "doubles_surf_crit_per_target",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_LAPRAS, MOVE_SURF), IDLE(SPECIES_MACHAMP) },
      .enemy  = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .wantSeed = WantOneCrit, .check = CheckSurfCritPerTarget },
    { .name = "doubles_earthquake_not_halved",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_DUGTRIO, MOVE_EARTHQUAKE), IDLE(SPECIES_SNORLAX) },
      .enemy  = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .wantSeed = WantNoCrit, .check = CheckEarthquakeFull },
    { .name = "doubles_earthquake_levitate_partner",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_DUGTRIO, MOVE_EARTHQUAKE), IDLE(SPECIES_GENGAR) },
      .enemy  = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .check = CheckEarthquakeLevitatePartner },
    { .name = "doubles_earthquake_hits_digging_partner_double",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M2(SPECIES_DUGTRIO, MOVE_EARTHQUAKE, MOVE_SPLASH), M(SPECIES_SNORLAX, MOVE_DIG) },
      .enemy  = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(1, 0, 0, 0), T4(0, 0, 0, 0) }, .targets = { T4(0, 0, TG(3), 0), T4(0, 0, TG(3), 0) }, .turns = 2, .wantSeed = WantNoCrit, .check = CheckEarthquakeDugPartner },
    { .name = "doubles_earthquake_absent_foe",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_SNORLAX, MOVE_EARTHQUAKE), M(SPECIES_JOLTEON, MOVE_THUNDERBOLT) },
      .enemy  = { IDLE(SPECIES_SNORLAX), HP1(SPECIES_RATTATA, MOVE_SPLASH) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(0, 0, TG(3), 0) }, .turns = 1, .check = CheckEarthquakeAbsentFoe },
    { .name = "doubles_earthquake_absent_partner",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_SNORLAX, MOVE_EARTHQUAKE), HP1(SPECIES_RATTATA, MOVE_SPLASH) },
      .enemy  = { M(SPECIES_ELECTRODE, MOVE_THUNDERBOLT), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(0, TG(2), 0, 0) }, .turns = 1, .check = CheckEarthquakeAbsentPartner },
    { .name = "doubles_explosion_hits_all_three",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_ELECTRODE, MOVE_EXPLOSION), IDLE(SPECIES_SNORLAX), IDLE(SPECIES_JOLTEON) },
      .enemy  = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .wantSeed = WantNoCrit, .check = CheckExplosionAll },
    { .name = "doubles_explosion_damp_partner",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_ELECTRODE, MOVE_EXPLOSION), IDLE(SPECIES_PSYDUCK) },
      .enemy  = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .check = CheckExplosionDamp },
    { .name = "doubles_explosion_damp_foe",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_ELECTRODE, MOVE_EXPLOSION), IDLE(SPECIES_SNORLAX) },
      .enemy  = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_GOLDUCK) },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .check = CheckExplosionDamp },
    { .name = "doubles_explosion_skips_absent_foe",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_VOLTORB, MOVE_EXPLOSION), M(SPECIES_ELECTRODE, MOVE_THUNDERBOLT) },
      .enemy  = { HP1(SPECIES_RATTATA, MOVE_SPLASH), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(0, 0, TG(1), 0) }, .turns = 1, .check = CheckExplosionAbsentFoe },

    // ---- partner support
    { .name = "doubles_helping_hand_boost",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_CLEFABLE, MOVE_HELPING_HAND), M(SPECIES_MACHAMP, MOVE_STRENGTH) },
      .enemy  = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(0, 0, TG(1), 0) }, .turns = 1, .wantSeed = WantNoCrit, .check = CheckHelpingHand },
    { .name = "doubles_helping_hand_no_partner",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M2(SPECIES_CLEFABLE, MOVE_HELPING_HAND, MOVE_SPLASH), HP1(SPECIES_RATTATA, MOVE_SPLASH) },
      .enemy  = { M(SPECIES_ELECTRODE, MOVE_THUNDERBOLT), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(1, 0, 0, 0), T4(0, 1, 0, 0) }, .targets = { T4(0, TG(2), 0, 0) }, .turns = 2, .check = CheckHelpingHandPartnerAbsent },
    { .name = "doubles_helping_hand_both_partners",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_CLEFABLE, MOVE_HELPING_HAND), M(SPECIES_ALAKAZAM, MOVE_HELPING_HAND) },
      .enemy  = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .check = CheckHelpingHandBoth },
    { .name = "doubles_follow_me_redirects",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { IDLE(SPECIES_SNORLAX), M(SPECIES_BLISSEY, MOVE_FOLLOW_ME) },
      .enemy  = { M(SPECIES_MACHAMP, MOVE_SEISMIC_TOSS), M(SPECIES_MACHAMP, MOVE_SEISMIC_TOSS) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(0, TG(0), 0, TG(0)) }, .turns = 1, .check = CheckFollowMe },
    { .name = "doubles_follow_me_lasts_one_turn",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { IDLE(SPECIES_SNORLAX), M2(SPECIES_BLISSEY, MOVE_FOLLOW_ME, MOVE_SPLASH) },
      .enemy  = { M(SPECIES_MACHAMP, MOVE_SEISMIC_TOSS), M(SPECIES_MACHAMP, MOVE_SEISMIC_TOSS) },
      .actions = { T4(0, 0, 0, 0), T4(0, 0, 1, 0) }, .targets = { T4(0, TG(0), 0, TG(0)), T4(0, TG(0), 0, TG(0)) }, .turns = 2, .check = CheckFollowMeExpires },
    { .name = "doubles_follow_me_user_fainted",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { IDLE(SPECIES_SNORLAX), HP1(SPECIES_BLISSEY, MOVE_FOLLOW_ME) },
      .enemy  = { M(SPECIES_ELECTRODE, MOVE_QUICK_ATTACK), M(SPECIES_MACHAMP, MOVE_SEISMIC_TOSS) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(0, TG(2), 0, TG(0)) }, .turns = 1, .check = CheckFollowMeUserFainted },
    { .name = "doubles_follow_me_ignored_by_thrash",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { IDLE(SPECIES_SNORLAX), M(SPECIES_BLISSEY, MOVE_FOLLOW_ME) },
      .enemy  = { M(SPECIES_TAUROS, MOVE_THRASH), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .wantSeed = WantThrashHitsLeft, .check = CheckFollowMeIgnoredByRandomTarget },
    { .name = "doubles_follow_me_ignored_by_spread",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { IDLE(SPECIES_SNORLAX), M(SPECIES_BLISSEY, MOVE_FOLLOW_ME) },
      .enemy  = { M(SPECIES_LAPRAS, MOVE_SURF), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .check = CheckFollowMeSpread },
    { .name = "doubles_lightning_rod_thunderbolt",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_JOLTEON, MOVE_THUNDERBOLT), IDLE(SPECIES_SNORLAX) },
      .enemy  = { IDLE(SPECIES_SNORLAX), MON_AB(SPECIES_MANECTRIC, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(TG(1), 0, 0, 0) }, .turns = 1, .wantSeed = WantNoCrit, .check = CheckLightningRodThunderbolt },
    { .name = "doubles_lightning_rod_thunder_wave",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_JOLTEON, MOVE_THUNDER_WAVE), IDLE(SPECIES_SNORLAX) },
      .enemy  = { IDLE(SPECIES_SNORLAX), MON_AB(SPECIES_MANECTRIC, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(TG(1), 0, 0, 0) }, .turns = 1, .check = CheckLightningRodThunderWave },
    { .name = "doubles_lightning_rod_targeted_directly",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_JOLTEON, MOVE_THUNDERBOLT), IDLE(SPECIES_SNORLAX) },
      .enemy  = { IDLE(SPECIES_SNORLAX), MON_AB(SPECIES_MANECTRIC, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(TG(3), 0, 0, 0) }, .turns = 1, .check = CheckLightningRodDirect },
    { .name = "doubles_lightning_rod_partner_does_not_pull",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_JOLTEON, MOVE_THUNDERBOLT), MON_AB(SPECIES_MANECTRIC, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .enemy  = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(TG(1), 0, 0, 0) }, .turns = 1, .check = CheckLightningRodOwnSide },

    // ---- side-wide effects
    { .name = "doubles_reflect_two_thirds",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_ALAKAZAM, MOVE_REFLECT), IDLE(SPECIES_SNORLAX) },
      .enemy  = { M(SPECIES_MACHAMP, MOVE_STRENGTH), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(0, TG(0), 0, 0) }, .turns = 1, .wantSeed = WantNoCrit, .check = CheckReflectTwoThirds },
    { .name = "doubles_reflect_half_when_alone",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M2(SPECIES_ALAKAZAM, MOVE_REFLECT, MOVE_SPLASH), HP1(SPECIES_RATTATA, MOVE_SPLASH) },
      .enemy  = { M2(SPECIES_MACHAMP, MOVE_STRENGTH, MOVE_SPLASH), M(SPECIES_ELECTRODE, MOVE_THUNDERBOLT) },
      .actions = { T4(1, 1, 0, 0), T4(0, 0, 0, 1) }, .targets = { T4(0, 0, 0, TG(2)), T4(0, TG(0), 0, 0) }, .turns = 2, .wantSeed = WantNoCrit, .check = CheckReflectHalfAlone },
    { .name = "doubles_safeguard_covers_partner",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_ALAKAZAM, MOVE_SAFEGUARD), IDLE(SPECIES_MACHAMP) },
      .enemy  = { M(SPECIES_SNORLAX, MOVE_THUNDER_WAVE), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(0, TG(2), 0, 0) }, .turns = 1, .check = CheckSafeguardPartner },
    { .name = "doubles_heal_bell_partner_and_party",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { { .species = SPECIES_BLISSEY, .level = 50, .moves = { MOVE_HEAL_BELL, SPLASH3 }, .status = STATUS1_BURN },
                  { .species = SPECIES_MACHAMP, .level = 50, .moves = { SPLASH4 }, .status = STATUS1_PARALYSIS },
                  { .species = SPECIES_RATTATA, .level = 50, .moves = { SPLASH4 }, .status = STATUS1_POISON } },
      .enemy  = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .check = CheckHealBell },
    { .name = "doubles_heal_bell_soundproof_partner",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_BLISSEY, MOVE_HEAL_BELL),
                  { .species = SPECIES_ELECTRODE, .level = 50, .moves = { SPLASH4 }, .status = STATUS1_PARALYSIS },
                  { .species = SPECIES_RATTATA, .level = 50, .moves = { SPLASH4 }, .status = STATUS1_POISON } },
      .enemy  = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .check = CheckHealBellSoundproofPartner },
    { .name = "doubles_aromatherapy_ignores_soundproof",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_BLISSEY, MOVE_AROMATHERAPY),
                  { .species = SPECIES_ELECTRODE, .level = 50, .moves = { SPLASH4 }, .status = STATUS1_PARALYSIS },
                  { .species = SPECIES_RATTATA, .level = 50, .moves = { SPLASH4 }, .status = STATUS1_POISON } },
      .enemy  = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .check = CheckAromatherapy },
    { .name = "doubles_perish_song_all_but_soundproof",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_MISDREAVUS, MOVE_PERISH_SONG), IDLE(SPECIES_SNORLAX) },
      .enemy  = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_ELECTRODE) },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .check = CheckPerishSong },
    { .name = "doubles_perish_song_draw",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_MISDREAVUS, MOVE_PERISH_SONG), IDLE(SPECIES_SNORLAX) },
      .enemy  = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_GOLEM) },
      .actions = { T4(0, 0, 0, 0), T4(1, 0, 0, 0), T4(1, 0, 0, 0), T4(1, 0, 0, 0) }, .turns = 0, .check = CheckPerishSongDraw },
    { .name = "doubles_uproar_wakes_everyone",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_MACHAMP, MOVE_UPROAR), { .species = SPECIES_SNORLAX, .level = 50, .moves = { SPLASH4 }, .status = STATUS1_SLEEP_TURN(4) } },
      .enemy  = { { .species = SPECIES_SNORLAX, .level = 50, .moves = { SPLASH4 }, .status = STATUS1_SLEEP_TURN(4) },
                  { .species = SPECIES_ELECTRODE, .level = 50, .moves = { SPLASH4 }, .status = STATUS1_SLEEP_TURN(4) } },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .check = CheckUproarWakes },
    { .name = "doubles_intimidate_both_foes",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { IDLE(SPECIES_GYARADOS), IDLE(SPECIES_SNORLAX) },
      .enemy  = { IDLE(SPECIES_ARCANINE), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .check = CheckIntimidateBothFoes },
    { .name = "doubles_intimidate_blocked_sub_and_clear_body",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_SNORLAX), IDLE(SPECIES_GYARADOS) },
      .enemy  = { M(SPECIES_ALAKAZAM, MOVE_SUBSTITUTE), IDLE(SPECIES_TENTACRUEL) },
      .actions = { T4(0, 0, 0, 0), T4(SC_SWITCH(2), 1, 0, 0) }, .turns = 2, .check = CheckIntimidateBlocked },
    { .name = "doubles_skill_swap_partner",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_ALAKAZAM, MOVE_SKILL_SWAP), IDLE(SPECIES_MACHAMP) },
      .enemy  = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(TG(2), 0, 0, 0) }, .turns = 1, .check = CheckSkillSwapPartner },
    { .name = "doubles_role_play_partner",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_ALAKAZAM, MOVE_ROLE_PLAY), IDLE(SPECIES_MACHAMP) },
      .enemy  = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(TG(2), 0, 0, 0) }, .turns = 1, .check = CheckRolePlayPartner },
    { .name = "doubles_counter_last_physical_attacker",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_SNORLAX, MOVE_COUNTER), IDLE(SPECIES_SNORLAX) },
      .enemy  = { M(SPECIES_MACHAMP, MOVE_SEISMIC_TOSS), M(SPECIES_HITMONLEE, MOVE_SEISMIC_TOSS) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(0, TG(0), 0, TG(0)) }, .turns = 1, .check = CheckCounterLastAttacker },
    { .name = "doubles_counter_fails_vs_partner_hit",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_SNORLAX, MOVE_COUNTER), M(SPECIES_MACHAMP, MOVE_SEISMIC_TOSS) },
      .enemy  = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(0, 0, TG(0), 0) }, .turns = 1, .check = CheckCounterPartnerFails },
    { .name = "doubles_counter_pulled_by_follow_me",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_SNORLAX, MOVE_COUNTER), IDLE(SPECIES_SNORLAX) },
      .enemy  = { M(SPECIES_BLISSEY, MOVE_FOLLOW_ME), M(SPECIES_MACHAMP, MOVE_SEISMIC_TOSS) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(0, 0, 0, TG(0)) }, .turns = 1, .check = CheckCounterFollowMe },
    { .name = "doubles_mirror_coat_last_special_attacker",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_SLOWBRO, MOVE_MIRROR_COAT), IDLE(SPECIES_SNORLAX) },
      .enemy  = { IDLE(SPECIES_SNORLAX), M(SPECIES_BLISSEY, MOVE_PSYCHIC) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(0, 0, 0, TG(0)) }, .turns = 1, .wantSeed = WantMirrorCoatUsed, .check = CheckMirrorCoat },

    // ---- switching / faints
    { .name = "doubles_pursuit_on_switching_foe",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_PERSIAN, MOVE_PURSUIT), M(SPECIES_PERSIAN, MOVE_PURSUIT) },
      .enemy  = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_SNORLAX), IDLE(SPECIES_GOLEM) },
      .actions = { T4(0, SC_SWITCH(2), 0, 0) }, .targets = { T4(TG(1), 0, TG(3), 0) }, .turns = 1, .wantSeed = WantNoCrit, .check = CheckPursuitSwitching },
    { .name = "doubles_pursuit_both_partners_on_switcher",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_PERSIAN, MOVE_PURSUIT), M(SPECIES_PERSIAN, MOVE_PURSUIT) },
      .enemy  = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_SNORLAX), IDLE(SPECIES_GOLEM) },
      .actions = { T4(0, SC_SWITCH(2), 0, 0) }, .targets = { T4(TG(1), 0, TG(1), 0) }, .turns = 1, .wantSeed = WantNoCrit, .check = CheckPursuitBothPartners },
    { .name = "doubles_endeavor_and_super_fang_targets",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_ENDEAVOR, SPLASH3 }, .hp = 10, .hpSet = 1 }, M(SPECIES_RATICATE, MOVE_SUPER_FANG) },
      .enemy  = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(TG(3), 0, TG(1), 0) }, .turns = 1, .wantSeed = WantSuperFangHit, .check = CheckEndeavorSuperFang },
    { .name = "doubles_double_faint_replacements",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_ELECTRODE, MOVE_EXPLOSION), M(SPECIES_MACHAMP, MOVE_SEISMIC_TOSS), IDLE(SPECIES_JOLTEON) },
      .enemy  = { MON(SPECIES_RATTATA, 5, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_RATTATA, 5, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), IDLE(SPECIES_GOLEM), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(0, 0, TG(1), 0) }, .turns = 1, .check = CheckDoubleFaintReplacements },
    { .name = "doubles_baton_pass",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M2(SPECIES_SCYTHER, MOVE_SWORDS_DANCE, MOVE_BATON_PASS), IDLE(SPECIES_CLEFABLE), IDLE(SPECIES_SNORLAX) },
      .enemy  = { M(SPECIES_MACHAMP, MOVE_SEISMIC_TOSS), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0), T4(1, 0, 0, 0) }, .targets = { T4(0, TG(0), 0, 0), T4(0, TG(0), 0, 0) }, .turns = 2, .check = CheckBatonPass },
    { .name = "doubles_roar_random_reserve",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_SNORLAX, MOVE_ROAR), IDLE(SPECIES_SNORLAX) },
      .enemy  = { IDLE(SPECIES_RATTATA), IDLE(SPECIES_SNORLAX), IDLE(SPECIES_GOLEM), IDLE(SPECIES_PIDGEY) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(TG(1), 0, 0, 0) }, .turns = 1, .wantSeed = WantRoarPidgey, .check = CheckRoarRandom },
    { .name = "doubles_roar_fails_with_two_mons",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_SNORLAX, MOVE_ROAR), IDLE(SPECIES_SNORLAX) },
      .enemy  = { IDLE(SPECIES_RATTATA), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(TG(1), 0, 0, 0) }, .turns = 1, .check = CheckRoarFailsTwoMons },
    { .name = "doubles_roar_three_mons_forced_reserve",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_SNORLAX, MOVE_ROAR), IDLE(SPECIES_SNORLAX) },
      .enemy  = { IDLE(SPECIES_RATTATA), IDLE(SPECIES_SNORLAX), IDLE(SPECIES_GOLEM) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(TG(1), 0, 0, 0) }, .turns = 1, .check = CheckRoarThreeMons },
    { .name = "doubles_spikes_each_switch_in",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_FORRETRESS, MOVE_SPIKES), IDLE(SPECIES_SNORLAX) },
      .enemy  = { IDLE(SPECIES_RATTATA), IDLE(SPECIES_RATTATA), IDLE(SPECIES_GOLEM), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0), T4(1, SC_SWITCH(2), 0, SC_SWITCH(3)) }, .turns = 2, .check = CheckSpikesEachSwitchIn },
    { .name = "doubles_fly_dodges_targeted_move",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_MACHAMP, MOVE_SEISMIC_TOSS), M(SPECIES_MACHAMP, MOVE_SEISMIC_TOSS) },
      .enemy  = { M(SPECIES_PIDGEOT, MOVE_FLY), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(TG(1), TG(0), TG(3), 0) }, .turns = 1, .check = CheckFlyDodges },
    { .name = "doubles_surf_hits_diving_foe_double",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_LAPRAS, MOVE_SURF), IDLE(SPECIES_SNORLAX) },
      .enemy  = { IDLE(SPECIES_SNORLAX), M(SPECIES_PERSIAN, MOVE_DIVE) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(0, 0, 0, TG(0)) }, .turns = 1, .wantSeed = WantNoCrit, .check = CheckSurfHitsDiver },
    { .name = "doubles_substitute_blocks_spread_hit",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_LAPRAS, MOVE_SURF), IDLE(SPECIES_SNORLAX) },
      .enemy  = { M(SPECIES_ALAKAZAM, MOVE_SUBSTITUTE), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .check = CheckSubstituteSpread },
    { .name = "doubles_ghost_curse_explicit_target",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_GENGAR, MOVE_CURSE), IDLE(SPECIES_SNORLAX) },
      .enemy  = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(TG(3), 0, 0, 0) }, .turns = 1, .check = CheckGhostCurseRight },
    { .name = "doubles_ghost_curse_partner",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_GENGAR, MOVE_CURSE), IDLE(SPECIES_SNORLAX) },
      .enemy  = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(TG(2), 0, 0, 0) }, .turns = 1, .check = CheckGhostCursePartner },
    { .name = "doubles_growl_both_foes_clear_body",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_SNORLAX, MOVE_GROWL), IDLE(SPECIES_SNORLAX) },
      .enemy  = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_TENTACRUEL) },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .check = CheckGrowlBothFoes },
    { .name = "doubles_protect_one_foe_vs_spread",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_LAPRAS, MOVE_SURF), IDLE(SPECIES_SNORLAX) },
      .enemy  = { M(SPECIES_ALAKAZAM, MOVE_PROTECT), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .wantSeed = WantNoCrit, .check = CheckProtectOneFoe },
    { .name = "doubles_struggle_random_foe",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { { .species = SPECIES_MACHAMP, .level = 50, .moves = { MOVE_TACKLE, MOVE_NONE, MOVE_NONE, MOVE_NONE }, .pp = { 0, 0, 0, 0 }, .ppSet = 1 }, IDLE(SPECIES_SNORLAX) },
      .enemy  = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .wantSeed = WantStruggleRight, .check = CheckStruggleRandom },
    { .name = "doubles_trace_random_foe",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { MON_AB(SPECIES_GARDEVOIR, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1), IDLE(SPECIES_SNORLAX) },
      .enemy  = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_GOLEM) },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .wantSeed = WantTraceRockHead, .check = CheckTraceRandomFoe },
    { .name = "doubles_curse_non_ghost_ignores_target",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_SNORLAX, MOVE_CURSE), IDLE(SPECIES_SNORLAX) },
      .enemy  = { IDLE(SPECIES_SNORLAX), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(TG(3), 0, 0, 0) }, .turns = 1, .check = CheckCurseNonGhostIgnoresTarget },
    { .name = "doubles_mist_protects_partner",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_ALAKAZAM, MOVE_MIST), IDLE(SPECIES_SNORLAX) },
      .enemy  = { M(SPECIES_SNORLAX, MOVE_GROWL), IDLE(SPECIES_SNORLAX) },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .check = CheckMistProtectsBoth },
    { .name = "doubles_explosion_absent_partner_ends_battle",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M(SPECIES_SNORLAX, MOVE_EXPLOSION), HP1(SPECIES_RATTATA, MOVE_SPLASH) },
      // the second foe must survive the Explosion, otherwise both sides are out and the result is a draw
      .enemy  = { M(SPECIES_ELECTRODE, MOVE_THUNDERBOLT), { .species = SPECIES_STEELIX, .level = 100, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH } } },
      .actions = { T4(0, 0, 0, 0) }, .targets = { T4(0, TG(2), 0, 0) }, .turns = 0, .check = CheckExplosionAbsentPartner },
};

SCENARIO_GROUP(doubles, sScenarios)
