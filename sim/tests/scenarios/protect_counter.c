// Protection, counters and move restriction: Protect/Detect/Endure, Counter/Mirror Coat, Bide,
// Destiny Bond, Grudge, Substitute, Magic Coat, Snatch, Follow Me, Helping Hand, Taunt, Torment,
// Disable, Encore, Imprison, Spite, Pressure, Struggle and Focus Punch.
#include "scenario.h"

#define SPLASH4 MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH
#define DS(b) (sim->disableStructs[b])

// hp of the target recorded when `battler` printed its "used move" string on `turn` (-1 if absent)
static int HpTargetAtUsedMove(struct BattleSim *sim, u8 battler, int turn)
{
    int i = Sc_LogIndex(sim, STRINGID_USEDMOVE, battler, 0, turn);
    return i < 0 ? -1 : sim->log[i].hpTarget;
}

// ---------------------------------------------------------------- Protect / Detect

static void CheckProtectBlocksPound(struct BattleSim *sim)
{
    CHECK(MOVED_BEFORE(0, 1, 0), "protect (+3) went before pound");
    CHECK(HP(0) == MAXHP(0), "no damage taken");
    CHECK(LOG_HAS_T(STRINGID_PKMNPROTECTEDITSELF2, 0), "'protected itself' on use");
    CHECK(LOG_HAS_T(STRINGID_PKMNPROTECTEDITSELF, 0), "'protected itself' on the blocked attack");
    CHECK(DS(0).protectUses == 1, "protectUses 1 (got %d)", DS(0).protectUses);
    CHECK(PP(0, 0) == 9, "protect pp 9 (got %d)", PP(0, 0));
    CHECK(PP(1, 0) == 34, "blocked pound still costs pp (got %d)", PP(1, 0));
}

static int WantProtectThreeTimes(struct BattleSim *sim) { return Sc_LogCount(sim, STRINGID_PKMNPROTECTEDITSELF2, SC_ANY_TURN) == 3; }
static void CheckProtectChainThree(struct BattleSim *sim)
{
    // 1/2 then 1/4: with this seed all three succeed and the counter climbs to 3.
    CHECK(DS(0).protectUses == 3, "protectUses 3 (got %d)", DS(0).protectUses);
    CHECK(HP(0) == MAXHP(0), "never hit");
    CHECK(!LOG_HAS(STRINGID_BUTITFAILED), "no failure");
}

static int WantProtectFailsTurn1(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_BUTITFAILED, 1); }
static void CheckProtectFailResets(struct BattleSim *sim)
{
    // Turn 1 failed (1/2 chance); the failure resets the counter, so turn 2 protect is guaranteed.
    CHECK(!LOG_HAS_T(STRINGID_PKMNPROTECTEDITSELF2, 1), "no protect message on the failed turn");
    CHECK(LOG_HAS_T(STRINGID_PKMNPROTECTEDITSELF, 2), "turn 3 protect blocked pound");
    CHECK(LOG_HAS_T(STRINGID_PKMNPROTECTEDITSELF2, 2), "turn 3 protect succeeded");
    CHECK(DS(0).protectUses == 1, "counter restarted at 1 (got %d)", DS(0).protectUses);
}

static void CheckProtectResetByOtherMove(struct BattleSim *sim)
{
    // Protect, Splash, Protect: the intervening move resets the counter, so the second protect cannot fail.
    CHECK(LOG_HAS_T(STRINGID_PKMNPROTECTEDITSELF2, 2), "turn 3 protect succeeded");
    CHECK(LOG_HAS_T(STRINGID_PKMNPROTECTEDITSELF, 2), "turn 3 pound blocked");
    CHECK(DS(0).protectUses == 1, "protectUses 1 (got %d)", DS(0).protectUses);
}

static int WantDetectFails(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_BUTITFAILED, 1); }
static void CheckDetectSharesCounter(struct BattleSim *sim)
{
    // Detect after Protect uses the shared counter (1/2), so it can fail.
    CHECK(LOG_HAS_T(STRINGID_PKMNPROTECTEDITSELF2, 0), "protect worked on turn 1");
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_DETECT, 1), "detect used on turn 2");
    CHECK(DS(0).protectUses == 0, "counter reset by the failure (got %d)", DS(0).protectUses);
    CHECK(HP(0) < MAXHP(0), "pound landed");
}

static void CheckProtectFailsWhenLast(struct BattleSim *sim)
{
    // The foe switched (switches resolve before moves), so the protect user acted last: Protect fails.
    CHECK(B(1).species == SPECIES_PIDGEY, "foe switched to pidgey");
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 0), "protect failed");
    CHECK(!LOG_HAS(STRINGID_PKMNPROTECTEDITSELF2), "no protect message");
    CHECK(DS(0).protectUses == 0, "protectUses 0 (got %d)", DS(0).protectUses);
    CHECK(PP(0, 0) == 9, "pp still deducted (got %d)", PP(0, 0));
}

static void CheckProtectNotVsFutureSight(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNPROTECTEDITSELF2, 2), "protect succeeded on turn 3");
    CHECK(HP(0) < MAXHP(0), "future sight hit through protect at the end of turn 3");
}

static void CheckProtectBlocksRoar(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_SNORLAX, "not dragged out");
    CHECK(LOG_HAS_T(STRINGID_PKMNPROTECTEDITSELF, 0), "roar blocked by protect");
    CHECK(PP(1, 0) == 19, "roar pp deducted (got %d)", PP(1, 0));
}

static void CheckProtectBlocksFlyLanding(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNFLEWHIGH, 0), "flew up on turn 1");
    CHECK(LOG_HAS_T(STRINGID_PKMNPROTECTEDITSELF, 1), "fly's landing blocked");
    CHECK(HP(0) == MAXHP(0), "no damage");
    CHECK(!(STATUS3(1) & STATUS3_ON_AIR), "no longer in the air");
    CHECK(!(STATUS2(1) & STATUS2_MULTIPLETURNS), "fly cancelled");
}

static void CheckProtectNotVsGhostCurse(struct BattleSim *sim)
{
    // Curse has no FLAG_PROTECT_AFFECTED in FRLG, so a Ghost's Curse goes through Protect.
    CHECK(LOG_HAS_T(STRINGID_PKMNPROTECTEDITSELF2, 0), "protect succeeded");
    CHECK(STATUS2(0) & STATUS2_CURSED, "cursed through protect");
    CHECK(HP(0) == MAXHP(0) - MAXHP(0) / 4, "curse damage 1/4 at end of turn (hp %d/%d)", HP(0), MAXHP(0));
    CHECK(HP(1) == MAXHP(1) - MAXHP(1) / 2, "gengar paid half its hp (hp %d/%d)", HP(1), MAXHP(1));
}

static void CheckProtectNotVsPerishSong(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNPROTECTEDITSELF2, 0), "protect succeeded");
    CHECK(STATUS3(0) & STATUS3_PERISH_SONG, "perish song applied through protect");
    CHECK(DS(0).perishSongTimer == 2, "perish counter 3 -> 2 at end of turn (got %d)", DS(0).perishSongTimer);
}

static void CheckProtectBlocksBideRelease(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNSTORINGENERGY, 1), "storing energy on turn 2");
    CHECK(LOG_HAS_T(STRINGID_PKMNUNLEASHEDENERGY, 2), "unleashed on turn 3");
    CHECK(LOG_HAS_T(STRINGID_PKMNPROTECTEDITSELF, 2), "bide's release blocked by protect");
    CHECK(HP(0) == MAXHP(0), "no damage");
}

// ---------------------------------------------------------------- Endure / Focus Band

static void CheckEndureSurvives(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNBRACEDITSELF), "braced itself");
    CHECK(LOG_HAS(STRINGID_PKMNENDUREDHIT), "endured the hit");
    CHECK(HP(0) == 1, "left at 1 hp (got %d)", HP(0));
}

static int WantProtectAfterEndureFails(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_BUTITFAILED, 1); }
static void CheckEndureProtectChain(struct BattleSim *sim)
{
    // Endure then Protect: the counter is shared, so the Protect runs at 1/2 and failed with this seed.
    CHECK(LOG_HAS_T(STRINGID_PKMNBRACEDITSELF, 0), "endure worked");
    CHECK(DS(0).protectUses == 0, "counter reset (got %d)", DS(0).protectUses);
    CHECK(HP(0) < MAXHP(0), "pound landed after the failed protect");
}

static void CheckEndureBeatsFocusBand(struct BattleSim *sim)
{
    CHECK(HP(0) == 1, "left at 1 hp (got %d)", HP(0));
    CHECK(LOG_HAS(STRINGID_PKMNENDUREDHIT), "endure message takes precedence");
    CHECK(!LOG_HAS(STRINGID_PKMNHUNGONWITHX), "no focus band message");
}

static int WantFocusBand(struct BattleSim *sim) { return HP(0) == 1; }
static void CheckFocusBandHangsOn(struct BattleSim *sim)
{
    CHECK(HP(0) == 1, "hung on at 1 hp");
    CHECK(LOG_HAS(STRINGID_PKMNHUNGONWITHX), "focus band message");
}

static void CheckEndureNotVsPoison(struct BattleSim *sim)
{
    // Endure only guards against damaging moves; the poisoned Rattata fainted at the end of the turn
    // and its replacement was sent out, so look at the party slot rather than battler 0.
    CHECK(LOG_HAS(STRINGID_PKMNBRACEDITSELF), "braced itself");
    CHECK(LOG_HAS_T(STRINGID_PKMNHURTBYPOISON, 0), "hurt by poison");
    CHECK(PARTY_HP(B_SIDE_PLAYER, 0) == 0, "poison at the end of the turn still fainted it (party hp %d)", PARTY_HP(B_SIDE_PLAYER, 0));
    CHECK(B(0).species == SPECIES_PIDGEY, "replacement sent out");
}

// ---------------------------------------------------------------- Counter / Mirror Coat

static int WantNoCrit(struct BattleSim *sim) { return !Sc_LogHas(sim, STRINGID_CRITICALHIT, SC_ANY_TURN); }

static void CheckCounterDouble(struct BattleSim *sim)
{
    int dealt = MAXHP(0) - HP(0);
    CHECK(MOVED_BEFORE(1, 0, 0), "counter (-5) went last");
    CHECK(dealt > 0, "took physical damage");
    CHECK(HP(1) > 0, "the foe survived the doubled damage (so the exact amount is observable)");
    CHECK(HP(1) == MAXHP(1) - 2 * dealt, "counter returned exactly double (took %d, foe hp %d/%d)", dealt, HP(1), MAXHP(1));
}

static void CheckCounterFailsVsSpecial(struct BattleSim *sim)
{
    CHECK(HP(0) < MAXHP(0), "surf landed");
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "counter failed against a special hit");
    CHECK(HP(1) == MAXHP(1), "no damage returned");
    CHECK(PP(0, 0) == 19, "counter pp deducted on failure (got %d)", PP(0, 0));
}

static void CheckMirrorCoatDouble(struct BattleSim *sim)
{
    int dealt = MAXHP(0) - HP(0);
    CHECK(dealt > 0, "took special damage");
    CHECK(HP(1) == MAXHP(1) - 2 * dealt, "mirror coat returned double (took %d, foe hp %d/%d)", dealt, HP(1), MAXHP(1));
}

static void CheckMirrorCoatFailsVsPhysical(struct BattleSim *sim)
{
    CHECK(HP(0) < MAXHP(0), "pound landed");
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "mirror coat failed against a physical hit");
    CHECK(HP(1) == MAXHP(1), "no damage returned");
}

static void CheckCounterVsGhost(struct BattleSim *sim)
{
    CHECK(HP(0) < MAXHP(0), "pound landed");
    CHECK(LOG_HAS(STRINGID_ITDOESNTAFFECT), "fighting-type counter does not affect a ghost");
    CHECK(HP(1) == MAXHP(1), "no damage");
}

static void CheckMirrorCoatVsDark(struct BattleSim *sim)
{
    CHECK(HP(0) < MAXHP(0), "surf landed");
    CHECK(LOG_HAS(STRINGID_ITDOESNTAFFECT), "psychic-type mirror coat does not affect a dark type");
    CHECK(HP(1) == MAXHP(1), "no damage");
}

static void CheckCounterAfterSubHit(struct BattleSim *sim)
{
    // Damage absorbed by a substitute is not recorded as physical damage: Counter has nothing to return.
    CHECK(HP(0) == MAXHP(0) - MAXHP(0) / 4, "only the substitute cost was paid (hp %d/%d)", HP(0), MAXHP(0));
    CHECK(LOG_HAS_T(STRINGID_SUBSTITUTEDAMAGED, 1), "substitute took the pound");
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 1), "counter failed");
    CHECK(HP(1) == MAXHP(1), "no damage returned");
}

static void CheckCounterPreviousTurn(struct BattleSim *sim)
{
    CHECK(HP(0) < MAXHP(0), "pound landed on turn 1");
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 1), "counter on the next turn fails (damage is per turn)");
    CHECK(HP(1) == MAXHP(1), "no damage returned");
}

static void CheckCounterPartnerHit(struct BattleSim *sim)
{
    CHECK(HP(0) < MAXHP(0), "partner's pound landed");
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "counter fails when the last physical attacker is an ally");
    CHECK(HP(1) == MAXHP(1) && HP(3) == MAXHP(3), "foes untouched");
}

static void CheckCounterFollowMe(struct BattleSim *sim)
{
    int dealt = MAXHP(0) - HP(0);
    CHECK(dealt > 0, "rattata's pound landed");
    CHECK(HP(1) == MAXHP(1), "the attacker was not hit");
    CHECK(HP(3) == MAXHP(3) - 2 * dealt, "counter redirected to the follow me user (hp %d/%d, took %d)", HP(3), MAXHP(3), dealt);
}

// ---------------------------------------------------------------- Bide

static void CheckBideDouble(struct BattleSim *sim)
{
    // Turn 1 Bide set (Splash did nothing), turn 2 stores, turn 3 unleashes twice the damage taken.
    int taken = MAXHP(1) - HP(1);
    CHECK(taken > 0, "bide user took damage");
    CHECK(LOG_HAS_T(STRINGID_PKMNSTORINGENERGY, 1), "storing energy on turn 2");
    CHECK(LOG_HAS_T(STRINGID_PKMNUNLEASHEDENERGY, 2), "unleashed on turn 3");
    CHECK(HP(0) == MAXHP(0) - 2 * taken, "bide dealt double (%d taken, hp %d/%d)", taken, HP(0), MAXHP(0));
    CHECK(!(STATUS2(1) & (STATUS2_BIDE | STATUS2_MULTIPLETURNS)), "bide over");
}

static void CheckBideNoDamage(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNUNLEASHEDENERGY, 2), "unleashed on turn 3");
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 2), "but it failed (no damage stored)");
    CHECK(HP(0) == MAXHP(0), "no damage");
}

static void CheckBideIgnoresResistance(struct BattleSim *sim)
{
    int taken = MAXHP(1) - HP(1);
    CHECK(taken > 0, "bide user took damage");
    CHECK(HP(0) == MAXHP(0) - 2 * taken, "steel type still took the full double (%d taken, hp %d/%d)", taken, HP(0), MAXHP(0));
    CHECK(!LOG_HAS_T(STRINGID_NOTVERYEFFECTIVE, 2), "no effectiveness message");
}

static void CheckBideVsGhost(struct BattleSim *sim)
{
    CHECK(HP(1) < MAXHP(1), "bide user took damage");
    CHECK(LOG_HAS_T(STRINGID_PKMNUNLEASHEDENERGY, 2), "unleashed on turn 3");
    CHECK(LOG_HAS_T(STRINGID_ITDOESNTAFFECT, 2), "normal-type bide does not affect a ghost");
    CHECK(HP(0) == MAXHP(0), "no damage");
}

static void CheckBideSleep(struct BattleSim *sim)
{
    // Falling asleep cancels multi-turn moves (SetMoveEffect: STATUS1_SLEEP -> CancelMultiTurnMoves): Bide
    // was started this turn and would otherwise still hold its 2-turn counter after the turn.
    CHECK(MOVED_BEFORE(0, 1, 0), "bide set before spore");
    CHECK(LOG_HAS_T(STRINGID_PKMNFELLASLEEP, 0), "fell asleep on turn 1");
    CHECK(STATUS1(0) & STATUS1_SLEEP, "asleep");
    CHECK(!(STATUS2(0) & STATUS2_BIDE), "bide counter cleared (status2 %x)", STATUS2(0));
    CHECK(!(STATUS2(0) & STATUS2_MULTIPLETURNS), "no longer locked into bide");
}

static int WantFlinchTurn1(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_PKMNFLINCHED, 1); }
static void CheckBideFlinch(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNFLINCHED, 1), "flinched on turn 2");
    CHECK(!(STATUS2(1) & STATUS2_BIDE), "flinch cancelled bide");
    CHECK(!(STATUS2(1) & STATUS2_MULTIPLETURNS), "no longer locked");
}

// ---------------------------------------------------------------- Destiny Bond / Grudge

static void CheckDestinyBondSameTurn(struct BattleSim *sim)
{
    // Both fainted and were replaced, so check the party slots.
    CHECK(PARTY_HP(B_SIDE_PLAYER, 0) == 0, "gengar fainted");
    CHECK(PARTY_HP(B_SIDE_OPPONENT, 0) == 0, "the attacker was taken down");
    CHECK(B(0).species == SPECIES_RATTATA && B(1).species == SPECIES_PIDGEY, "both sides sent replacements");
    CHECK(LOG_HAS_T(STRINGID_PKMNTOOKFOE, 0), "took foe message");
}

static void CheckDestinyBondPersists(struct BattleSim *sim)
{
    // Set on turn 1; on turn 2 the faster foe KOs the user before it moves: the bond is still active.
    CHECK(PARTY_HP(B_SIDE_PLAYER, 0) == 0, "user fainted on turn 2");
    CHECK(PARTY_HP(B_SIDE_OPPONENT, 0) == 0, "foe taken down");
    CHECK(LOG_HAS_T(STRINGID_PKMNTOOKFOE, 1), "took foe on turn 2");
}

static void CheckDestinyBondExpires(struct BattleSim *sim)
{
    // The user moved (Splash) before being KO'd: the bond was cleared at the start of its move.
    CHECK(MOVED_BEFORE(0, 1, 1), "user moved before being hit");
    CHECK(PARTY_HP(B_SIDE_PLAYER, 0) == 0, "user fainted");
    CHECK(HP(1) == MAXHP(1), "foe unharmed");
    CHECK(!LOG_HAS(STRINGID_PKMNTOOKFOE), "no destiny bond");
}

static void CheckDestinyBondNotPoison(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNHURTBYPOISON, 0), "hurt by poison");
    CHECK(PARTY_HP(B_SIDE_PLAYER, 0) == 0, "fainted to poison");
    CHECK(HP(1) == MAXHP(1), "poison does not trigger destiny bond");
    CHECK(!LOG_HAS(STRINGID_PKMNTOOKFOE), "no message");
}

static void CheckDestinyBondNotPartner(struct BattleSim *sim)
{
    CHECK(HP(0) == 0 || B(0).species != SPECIES_SABLEYE, "sableye was KO'd by the partner's earthquake");
    CHECK(HP(2) == MAXHP(2), "partner unaffected");
    CHECK(!LOG_HAS(STRINGID_PKMNTOOKFOE), "destiny bond ignores allies");
}

static void CheckGrudge(struct BattleSim *sim)
{
    CHECK(HP(0) == 0 || B(0).species != SPECIES_RATTATA, "grudge user fainted");
    CHECK(LOG_HAS_T(STRINGID_PKMNLOSTPPGRUDGE, 1), "grudge message");
    CHECK(PP(1, 0) == 0, "pound pp reduced to 0 (got %d)", PP(1, 0));
    CHECK(PP(1, 1) == 39, "other move untouched (got %d)", PP(1, 1));
}

static void CheckGrudgeNotStruggle(struct BattleSim *sim)
{
    CHECK(LOG_HAS_MOVE(STRINGID_USEDMOVE, MOVE_STRUGGLE), "foe struggled");
    CHECK(LOG_HAS(STRINGID_PKMNWANTSGRUDGE), "grudge was set");
    CHECK(HP(0) == 0 || B(0).species != SPECIES_RATTATA, "grudge user fainted");
    CHECK(!LOG_HAS(STRINGID_PKMNLOSTPPGRUDGE), "struggle is exempt from grudge");
}

static void CheckGrudgeTwice(struct BattleSim *sim)
{
    // The grudge flag is cleared when the user starts its next move, so a second Grudge never "fails".
    CHECK(LOG_COUNT(STRINGID_PKMNWANTSGRUDGE) == 2, "grudge succeeded twice (got %d)", LOG_COUNT(STRINGID_PKMNWANTSGRUDGE));
    CHECK(!LOG_HAS(STRINGID_BUTITFAILED), "no failure");
    CHECK(STATUS3(0) & STATUS3_GRUDGE, "grudge active");
}

// ---------------------------------------------------------------- Substitute

static void CheckSubstituteCost(struct BattleSim *sim)
{
    CHECK(MAXHP(0) == 235, "snorlax max hp 235 (got %d)", MAXHP(0));
    CHECK(STATUS2(0) & STATUS2_SUBSTITUTE, "substitute up");
    CHECK(HP(0) == MAXHP(0) - MAXHP(0) / 4, "paid 1/4 (hp %d)", HP(0));
    CHECK(DS(0).substituteHP == MAXHP(0) / 4, "substitute hp 1/4 (got %d)", DS(0).substituteHP);
    CHECK(LOG_HAS(STRINGID_PKMNMADESUBSTITUTE), "made a substitute");
}

static void CheckSubstituteTooWeak(struct BattleSim *sim)
{
    // hp == maxHP/4 exactly is too weak (the check is hp <= maxHP/4).
    CHECK(LOG_HAS(STRINGID_TOOWEAKFORSUBSTITUTE), "too weak message");
    CHECK(!(STATUS2(0) & STATUS2_SUBSTITUTE), "no substitute");
    CHECK(HP(0) == 58, "hp untouched (got %d)", HP(0));
    CHECK(PP(0, 0) == 9, "pp still deducted (got %d)", PP(0, 0));
}

static void CheckSubstituteJustEnough(struct BattleSim *sim)
{
    CHECK(STATUS2(0) & STATUS2_SUBSTITUTE, "substitute up with hp = 1/4 + 1");
    CHECK(HP(0) == 1, "left at 1 hp (got %d)", HP(0));
}

static void CheckSubstituteAlreadyUp(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNHASSUBSTITUTE, 1), "already has a substitute");
    CHECK(HP(0) == MAXHP(0) - MAXHP(0) / 4, "paid only once (hp %d/%d)", HP(0), MAXHP(0));
    CHECK(PP(0, 0) == 8, "pp deducted both times (got %d)", PP(0, 0));
}

static void CheckSubstituteAbsorbs(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_SUBSTITUTEDAMAGED, 1), "substitute took damage");
    CHECK(STATUS2(0) & STATUS2_SUBSTITUTE, "still up");
    CHECK(DS(0).substituteHP > 0 && DS(0).substituteHP < MAXHP(0) / 4, "substitute hp reduced (got %d)", DS(0).substituteHP);
    CHECK(HP(0) == MAXHP(0) - MAXHP(0) / 4, "user untouched (hp %d/%d)", HP(0), MAXHP(0));
}

static void CheckSubstituteBreaks(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNSUBSTITUTEFADED, 1), "substitute faded");
    CHECK(!(STATUS2(0) & STATUS2_SUBSTITUTE), "substitute gone");
    CHECK(DS(0).substituteHP == 0, "substitute hp 0 (got %d)", DS(0).substituteHP);
    CHECK(HP(0) == MAXHP(0) - MAXHP(0) / 4, "no damage carries over (hp %d/%d)", HP(0), MAXHP(0));
}

static void CheckSubstituteBlocksStatDrops(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 1), "growl failed against the substitute");
    CHECK(PP(1, 1) == 39, "growl pp still deducted (got %d)", PP(1, 1));
    CHECK(STAGE(0, STAT_ATK) == 6, "attack unchanged (stage %d)", STAGE(0, STAT_ATK));
    CHECK(LOG_HAS_T(STRINGID_SUBSTITUTEDAMAGED, 2) || LOG_HAS_T(STRINGID_PKMNSUBSTITUTEFADED, 2), "mud-slap hit the substitute");
    CHECK(STAGE(0, STAT_ACC) == 6, "mud-slap's accuracy drop blocked (stage %d)", STAGE(0, STAT_ACC));
}

static void CheckSubstituteBlocksIntimidate(struct BattleSim *sim)
{
    CHECK(B(1).species == SPECIES_GYARADOS, "gyarados switched in");
    CHECK(STAGE(0, STAT_ATK) == 6, "intimidate blocked by the substitute (stage %d)", STAGE(0, STAT_ATK));
    CHECK(!LOG_HAS(STRINGID_PKMNCUTSATTACKWITH), "no intimidate message");
}

static void CheckSubstituteNotVsAttract(struct BattleSim *sim)
{
    CHECK(STATUS2(0) & STATUS2_SUBSTITUTE, "substitute up");
    CHECK(LOG_HAS_T(STRINGID_PKMNFELLINLOVE, 1), "attract goes through the substitute");
    CHECK(STATUS2(0) & STATUS2_INFATUATION, "infatuated");
}

static int WantConfusionSelfHitBehindSub(struct BattleSim *sim)
{
    return (STATUS2(0) & STATUS2_SUBSTITUTE) && Sc_LogHas(sim, STRINGID_ITHURTCONFUSION, 1);
}
static void CheckSubstituteConfusionSelfHit(struct BattleSim *sim)
{
    CHECK(DS(0).substituteHP == MAXHP(0) / 4, "substitute untouched by the self-hit (got %d)", DS(0).substituteHP);
    CHECK(HP(0) < MAXHP(0) - MAXHP(0) / 4, "the user itself took the confusion damage (hp %d/%d)", HP(0), MAXHP(0));
}

static void CheckSubstituteDrain(struct BattleSim *sim)
{
    int toSub = MAXHP(0) / 4 - DS(0).substituteHP;
    CHECK(toSub > 0 && (STATUS2(0) & STATUS2_SUBSTITUTE), "giga drain hit the substitute (dealt %d)", toSub);
    CHECK(HP(0) == MAXHP(0) - MAXHP(0) / 4, "user untouched");
    CHECK(HP(1) == 50 + toSub / 2, "drained half of the damage dealt to the substitute (hp %d, dealt %d)", HP(1), toSub);
    CHECK(LOG_HAS(STRINGID_PKMNENERGYDRAINED), "drain message");
}

static int WantSubFaded(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_PKMNSUBSTITUTEFADED, SC_ANY_TURN); }
static void CheckSubstituteRecoil(struct BattleSim *sim)
{
    // Double-Edge's recoil is 1/3 (MOVE_EFFECT_RECOIL_33) of the damage actually dealt (gHpDealt), which is
    // capped by the substitute's hp.
    CHECK(LOG_HAS(STRINGID_PKMNHITWITHRECOIL), "recoil message");
    CHECK(HP(1) == MAXHP(1) - (MAXHP(0) / 4) / 3, "recoil from the substitute's hp (hp %d/%d)", HP(1), MAXHP(1));
    CHECK(HP(0) == MAXHP(0) - MAXHP(0) / 4, "user untouched");
}

static void CheckSubstitutePainSplit(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 1), "pain split fails against a substitute");
    CHECK(HP(0) == MAXHP(0) - MAXHP(0) / 4, "user hp unchanged");
    CHECK(HP(1) == 50, "foe hp unchanged (got %d)", HP(1));
}

static void CheckSubstituteThief(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_SUBSTITUTEDAMAGED, 1) || LOG_HAS_T(STRINGID_PKMNSUBSTITUTEFADED, 1), "thief hit the substitute");
    CHECK(B(0).item == ITEM_LEFTOVERS, "item not stolen through the substitute");
    CHECK(B(1).item == ITEM_NONE, "thief got nothing");
}

static void CheckSubstituteBatonPass(struct BattleSim *sim)
{
    int scytherMax = GetMonData(&sim->playerParty[0], MON_DATA_MAX_HP);
    CHECK(B(0).species == SPECIES_RATTATA, "rattata received the pass");
    CHECK(STATUS2(0) & STATUS2_SUBSTITUTE, "substitute passed");
    CHECK(DS(0).substituteHP == scytherMax / 4, "substitute keeps scyther's 1/4 hp (got %d, want %d)", DS(0).substituteHP, scytherMax / 4);
}

static void CheckSubstituteSoundMove(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_SUBSTITUTEDAMAGED, 1) || LOG_HAS_T(STRINGID_PKMNSUBSTITUTEFADED, 1), "hyper voice hits the substitute in gen 3");
    CHECK(HP(0) == MAXHP(0) - MAXHP(0) / 4, "user untouched");
}

// ---------------------------------------------------------------- Magic Coat / Snatch

static int WantLeechSeedBounced(struct BattleSim *sim) { return (STATUS3(1) & STATUS3_LEECHSEED) != 0; }
static void CheckMagicCoatLeechSeed(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNMOVEBOUNCED), "bounced");
    CHECK(STATUS3(1) & STATUS3_LEECHSEED, "seeded the user");
    CHECK(!(STATUS3(0) & STATUS3_LEECHSEED), "magic coat user not seeded");
    CHECK(PP(1, 0) == 9, "leech seed pp paid by its user (got %d)", PP(1, 0));
    CHECK(PP(0, 0) == 14, "magic coat pp 14 (got %d)", PP(0, 0));
}

static void CheckMagicCoatGrowl(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNMOVEBOUNCED), "growl bounced");
    CHECK(STAGE(1, STAT_ATK) == 5, "growl user lost attack (stage %d)", STAGE(1, STAT_ATK));
    CHECK(STAGE(0, STAT_ATK) == 6, "magic coat user untouched (stage %d)", STAGE(0, STAT_ATK));
}

static void CheckMagicCoatNotTaunt(struct BattleSim *sim)
{
    CHECK(!LOG_HAS(STRINGID_PKMNMOVEBOUNCED), "taunt is not magic coat affected");
    CHECK(LOG_HAS(STRINGID_PKMNFELLFORTAUNT), "taunt landed");
    CHECK(DS(0).tauntTimer == 1, "taunt timer 2 -> 1 at end of turn (got %d)", DS(0).tauntTimer);
}

static void CheckMagicCoatFailsWhenLast(struct BattleSim *sim)
{
    CHECK(B(1).species == SPECIES_PIDGEY, "foe switched");
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "magic coat fails when the user moves last");
    CHECK(!LOG_HAS(STRINGID_PKMNSHROUDEDITSELF), "no shroud message");
    CHECK(PP(0, 0) == 14, "pp deducted (got %d)", PP(0, 0));
}

static int WantToxicBounced(struct BattleSim *sim) { return (STATUS1(1) & STATUS1_TOXIC_POISON) != 0; }
static void CheckMagicCoatPressure(struct BattleSim *sim)
{
    // Bouncing a Pressure user's move costs the magic coat user one extra PP.
    CHECK(STATUS1(1) & STATUS1_TOXIC_POISON, "toxic bounced onto dusclops");
    CHECK(PP(0, 0) == 13, "magic coat pp 15 - 1 - 1 (got %d)", PP(0, 0));
    CHECK(PP(1, 0) == 9, "toxic pp 9: alakazam has no pressure (got %d)", PP(1, 0));
}

static int WantSecondWaveLands(struct BattleSim *sim) { return (STATUS1(0) & STATUS1_PARALYSIS) != 0; }
static void CheckMagicCoatOneBounce(struct BattleSim *sim)
{
    CHECK(LOG_COUNT(STRINGID_PKMNMOVEBOUNCED) == 1, "only one bounce per turn (got %d)", LOG_COUNT(STRINGID_PKMNMOVEBOUNCED));
    CHECK((STATUS1(1) | STATUS1(3)) & STATUS1_PARALYSIS, "one foe paralyzed by its own thunder wave");
    CHECK(STATUS1(0) & STATUS1_PARALYSIS, "the second thunder wave landed");
}

static void CheckSnatchSwordsDance(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNSNATCHEDMOVE), "snatched");
    CHECK(STAGE(0, STAT_ATK) == 8, "snatcher got +2 attack (stage %d)", STAGE(0, STAT_ATK));
    CHECK(STAGE(1, STAT_ATK) == 6, "original user got nothing (stage %d)", STAGE(1, STAT_ATK));
    CHECK(PP(1, 0) == 29, "swords dance pp paid by its user (got %d)", PP(1, 0));
    CHECK(PP(0, 0) == 9, "snatch pp 9 (got %d)", PP(0, 0));
}

static void CheckSnatchRecover(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNSNATCHEDMOVE), "snatched");
    CHECK(HP(0) == 20 + MAXHP(0) / 2, "snatcher healed half its max hp (hp %d/%d)", HP(0), MAXHP(0));
    CHECK(HP(1) == MAXHP(1), "foe unchanged");
}

static void CheckSnatchNotProtect(struct BattleSim *sim)
{
    // Doubles: in singles Protect (+3) always moves last after Snatch (+4) and fails for that reason alone.
    CHECK(MOVED_BEFORE(0, 1, 0), "snatch went first");
    CHECK(LOG_HAS(STRINGID_PKMNWAITSFORTARGET), "snatch set up");
    CHECK(!LOG_HAS(STRINGID_PKMNSNATCHEDMOVE), "protect is not snatchable");
    CHECK(!LOG_HAS(STRINGID_BUTITFAILED), "protect did not fail");
    CHECK(LOG_HAS(STRINGID_PKMNPROTECTEDITSELF2), "foe protected itself");
    CHECK(DS(1).protectUses == 1 && DS(0).protectUses == 0, "protect counted for the foe, not the snatcher (%d/%d)", DS(1).protectUses, DS(0).protectUses);
}

static void CheckSnatchPartner(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNSNATCHEDMOVE), "snatched the ally's move");
    CHECK(STAGE(0, STAT_ATK) == 8, "snatcher +2 (stage %d)", STAGE(0, STAT_ATK));
    CHECK(STAGE(2, STAT_ATK) == 6, "ally got nothing (stage %d)", STAGE(2, STAT_ATK));
}

static void CheckSnatchSubstituteFails(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNSNATCHEDMOVE), "snatched substitute");
    CHECK(LOG_HAS(STRINGID_TOOWEAKFORSUBSTITUTE), "snatcher too weak to make one");
    CHECK(!(STATUS2(0) & STATUS2_SUBSTITUTE) && !(STATUS2(1) & STATUS2_SUBSTITUTE), "nobody has a substitute");
    CHECK(HP(0) == 30 && HP(1) == MAXHP(1), "no hp paid");
    CHECK(PP(1, 0) == 9, "substitute pp paid by its user (got %d)", PP(1, 0));
}

static void CheckSnatchPressure(struct BattleSim *sim)
{
    CHECK(STAGE(0, STAT_ATK) == 8, "snatched swords dance");
    CHECK(PP(0, 0) == 8, "snatching from a pressure user costs an extra pp (got %d)", PP(0, 0));
}

// ---------------------------------------------------------------- Follow Me / Helping Hand

static void CheckFollowMe(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNCENTERATTENTION, 0), "follow me used");
    CHECK(HP(0) < MAXHP(0), "follow me user took the hits");
    // partner was only hit on turn 2 (the pounds aimed at it on turn 1 were redirected)
    CHECK(HpTargetAtUsedMove(sim, 1, 1) == MAXHP(2) || HpTargetAtUsedMove(sim, 3, 1) == MAXHP(2), "partner was still at full hp entering turn 2");
    CHECK(HP(2) < MAXHP(2), "follow me expired: partner hit on turn 2");
}

static void CheckHelpingHand(struct BattleSim *sim)
{
    // d0 = damage with helping hand (turn 1), d1 = without (turn 2): 1.5x makes d0 > d1 for any damage roll.
    int hpEnteringTurn1 = HpTargetAtUsedMove(sim, 2, 1);
    int d0 = MAXHP(1) - hpEnteringTurn1;
    int d1 = hpEnteringTurn1 - HP(1);
    CHECK(LOG_HAS_T(STRINGID_PKMNREADYTOHELP, 0), "ready to help");
    CHECK(hpEnteringTurn1 > 0 && d0 > 0 && d1 > 0, "both pounds landed (d0 %d d1 %d)", d0, d1);
    CHECK(d0 > d1, "helping hand pound stronger (%d vs %d)", d0, d1);
    CHECK(d0 <= d1 * 15 / 10 * 100 / 85 + 2, "boost is 1.5x (%d vs %d)", d0, d1);
}

static void CheckHelpingHandSingles(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "helping hand fails in singles");
    CHECK(!LOG_HAS(STRINGID_PKMNREADYTOHELP), "no message");
}

static void CheckHelpingHandNoPartner(struct BattleSim *sim)
{
    CHECK(!LOG_HAS_T(STRINGID_PKMNREADYTOHELP, 1), "no helping hand with the partner gone");
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 1), "failed on turn 2");
}

static void CheckHelpingHandBoth(struct BattleSim *sim)
{
    CHECK(LOG_COUNT(STRINGID_PKMNREADYTOHELP) == 1, "only the first helping hand works (got %d)", LOG_COUNT(STRINGID_PKMNREADYTOHELP));
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "second one failed");
}

// ---------------------------------------------------------------- Taunt / Torment

static void CheckTaunt(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNFELLFORTAUNT, 0), "taunted");
    CHECK(LOG_HAS_T(STRINGID_PKMNCANTUSEMOVETAUNT, 0), "growl chosen before the taunt is cancelled");
    CHECK(!LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_GROWL, 1), "growl not selectable on turn 2");
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_POUND, 1), "fell back to pound");
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_GROWL, 2), "growl usable again on turn 3");
    CHECK(STAGE(0, STAT_ATK) == 5, "exactly one growl landed (stage %d)", STAGE(0, STAT_ATK));
    CHECK(DS(1).tauntTimer == 0, "taunt over");
}

static void CheckTauntStruggle(struct BattleSim *sim)
{
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_STRUGGLE, 1), "only status moves: struggle");
    CHECK(HP(0) < MAXHP(0), "struggle hit");
}

static void CheckTauntAgain(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNFELLFORTAUNT, 0), "first taunt worked");
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 1), "second taunt fails while the first is active");
    CHECK(!LOG_HAS_T(STRINGID_PKMNFELLFORTAUNT, 1), "no second taunt message");
}

static void CheckTauntAllowsCounter(struct BattleSim *sim)
{
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_COUNTER, 1), "counter (power 1) is selectable under taunt");
    CHECK(!LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_GROWL, 1), "growl not selectable");
}

static void CheckTorment(struct BattleSim *sim)
{
    CHECK(STATUS2(1) & STATUS2_TORMENT, "tormented");
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_POUND, 0), "pound on turn 1");
    CHECK(!LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_POUND, 1) && LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_GROWL, 1), "turn 2: pound refused, growl used");
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_POUND, 2), "turn 3: pound allowed again");
}

static void CheckTormentStruggle(struct BattleSim *sim)
{
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_POUND, 0), "pound on turn 1");
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_STRUGGLE, 1), "single-move mon must struggle");
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_POUND, 2), "pound again on turn 3");
}

static void CheckTormentAgain(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNSUBJECTEDTOTORMENT, 0), "tormented");
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 1), "second torment fails");
}

// ---------------------------------------------------------------- Disable

static int WantDisabledTurn0(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_PKMNMOVEWASDISABLED, 0); }
static void CheckDisable(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNMOVEWASDISABLED, 0), "pound disabled");
    CHECK(DS(1).disableTimerStartValue >= 2 && DS(1).disableTimerStartValue <= 5, "timer 2-5 (got %d)", DS(1).disableTimerStartValue);
    CHECK(!LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_POUND, 1) && LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_GROWL, 1), "pound not selectable, growl used");
    CHECK(DS(1).disabledMove == MOVE_POUND || LOG_HAS_T(STRINGID_PKMNMOVEDISABLEDNOMORE, 1), "still disabled unless the timer ran out");
}

static int WantDisabledTurn1(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_PKMNMOVEWASDISABLED, 1); }
static void CheckDisableSameTurn(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNMOVEISDISABLED, 1), "the already-chosen pound is cancelled");
    CHECK(LOG_INDEX_T(STRINGID_USEDMOVE, 1, 1) < 0, "no 'used pound' on turn 2");
}

static void CheckDisableNoLastMove(struct BattleSim *sim)
{
    CHECK(MOVED_BEFORE(0, 1, 0), "disable went first");
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 0), "nothing to disable yet");
    CHECK(DS(1).disabledMove == MOVE_NONE, "no move disabled");
}

static int WantDisableHits(struct BattleSim *sim) { return !Sc_LogHas(sim, STRINGID_ATTACKMISSED, 0); }
static void CheckDisableVsStruggle(struct BattleSim *sim)
{
    // Disable is 55% accurate (accuracycheck runs before disablelastusedattack), so the seed selects a hit.
    CHECK(!LOG_HAS_T(STRINGID_ATTACKMISSED, 0), "disable hit");
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_STRUGGLE, 0), "foe struggled");
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 0), "struggle cannot be disabled");
    CHECK(DS(1).disabledMove == MOVE_NONE, "no move disabled");
}

static void CheckDisableZeroPp(struct BattleSim *sim)
{
    CHECK(!LOG_HAS_T(STRINGID_ATTACKMISSED, 0), "disable hit");
    CHECK(PP(1, 0) == 0, "pound out of pp");
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 0), "a move with 0 pp cannot be disabled");
    CHECK(DS(1).disabledMove == MOVE_NONE, "no move disabled");
}

static int WantDisableTimer2(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_PKMNMOVEWASDISABLED, 0) && DS(1).disableTimerStartValue == 2; }
static void CheckDisableExpiry(struct BattleSim *sim)
{
    // Timer 2: ticks at the end of the turn it was set and the next: free again on turn 3.
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_GROWL, 1), "growl on turn 2");
    CHECK(LOG_HAS_T(STRINGID_PKMNMOVEDISABLEDNOMORE, 1), "disabled no more at the end of turn 2");
    CHECK(DS(1).disabledMove == MOVE_NONE && DS(1).disableTimer == 0, "cleared");
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_POUND, 2), "pound usable on turn 3");
}

// ---------------------------------------------------------------- Encore

static void CheckEncoreSameTurn(struct BattleSim *sim)
{
    // The foe chose Pound, but Encore (resolved first) forces its last move Growl on the same turn.
    CHECK(LOG_HAS_T(STRINGID_PKMNGOTENCORE, 1), "encored");
    CHECK(DS(1).encoredMove == MOVE_GROWL, "encored into growl");
    CHECK(DS(1).encoreTimerStartValue >= 3 && DS(1).encoreTimerStartValue <= 6, "timer 3-6 (got %d)", DS(1).encoreTimerStartValue);
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_GROWL, 1) && !LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_POUND, 1), "growl replaced pound");
    CHECK(STAGE(0, STAT_ATK) == 4, "two growls (stage %d)", STAGE(0, STAT_ATK));
}

static void CheckEncorePpOut(struct BattleSim *sim)
{
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_GROWL, 1), "forced growl on turn 2");
    CHECK(PP(1, 0) == 0, "growl out of pp");
    CHECK(LOG_HAS_T(STRINGID_PKMNENCOREENDED, 1), "encore ended when the pp ran out");
    CHECK(DS(1).encoredMove == MOVE_NONE && DS(1).encoreTimer == 0, "cleared");
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_POUND, 2), "free to use pound on turn 3");
}

static void CheckEncoreNoLastMove(struct BattleSim *sim)
{
    CHECK(MOVED_BEFORE(0, 1, 0), "encore went first");
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 0), "nothing to encore");
    CHECK(DS(1).encoredMove == MOVE_NONE, "not encored");
}

static void CheckEncoreVsStruggle(struct BattleSim *sim)
{
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_STRUGGLE, 0), "foe struggled");
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 0), "struggle cannot be encored");
}

static void CheckEncoreVsMirrorMove(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_MIRRORMOVEFAILED), "mirror move failed (nothing to copy)");
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 0), "mirror move cannot be encored");
    CHECK(DS(1).encoredMove == MOVE_NONE, "not encored");
}

static int WantEncoreTimer3(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_PKMNGOTENCORE, 0) && DS(1).encoreTimerStartValue == 3; }
static void CheckEncoreExpiry(struct BattleSim *sim)
{
    // Timer 3 ticks at the end of turns 1, 2, 3: forced on turns 2 and 3, free on turn 4.
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_GROWL, 1) && LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_GROWL, 2), "forced growl on turns 2 and 3");
    CHECK(LOG_HAS_T(STRINGID_PKMNENCOREENDED, 2), "encore ended at the end of turn 3");
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_POUND, 3), "pound on turn 4");
    CHECK(STAGE(0, STAT_ATK) == 3, "three growls (stage %d)", STAGE(0, STAT_ATK));
}

// ---------------------------------------------------------------- Imprison

static void CheckImprison(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNSEALEDOPPONENTMOVE, 0), "sealed");
    CHECK(STATUS3(0) & STATUS3_IMPRISONED_OTHERS, "imprison status");
    CHECK(LOG_HAS_T(STRINGID_PKMNCANTUSEMOVESEALED, 0), "foe's pound cancelled on the same turn");
    CHECK(HP(0) == MAXHP(0), "never hit");
    CHECK(!LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_POUND, 1) && LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_GROWL, 1), "pound not selectable, growl used");
}

static void CheckImprisonNoShared(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 0), "fails without a shared move");
    CHECK(!(STATUS3(0) & STATUS3_IMPRISONED_OTHERS), "no status");
}

static void CheckImprisonStruggle(struct BattleSim *sim)
{
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_STRUGGLE, 1), "all moves sealed: struggle");
}

static void CheckImprisonSwitch(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_SNORLAX, "switched");
    CHECK(!(STATUS3(0) & STATUS3_IMPRISONED_OTHERS), "imprison gone with the user");
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_GROWL, 1), "still sealed at turn 2 selection");
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_POUND, 2), "pound usable on turn 3");
}

static void CheckImprisonPressure(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNSEALEDOPPONENTMOVE), "sealed");
    CHECK(PP(0, 0) == 8, "imprison costs 2 pp against pressure (got %d)", PP(0, 0));
}

// ---------------------------------------------------------------- Spite

static void CheckSpite(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNREDUCEDPP), "pp reduced");
    CHECK(PP(1, 0) >= 29 && PP(1, 0) <= 32, "pound 34 - (2..5) (got %d)", PP(1, 0));
}

static void CheckSpiteNoLastMove(struct BattleSim *sim)
{
    CHECK(MOVED_BEFORE(0, 1, 0), "spite went first");
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "nothing to spite");
    CHECK(PP(1, 0) == 34, "pound pp only spent by its use (got %d)", PP(1, 0));
}

static void CheckSpiteOnePp(struct BattleSim *sim)
{
    CHECK(PP(1, 0) == 1, "pound at 1 pp (got %d)", PP(1, 0));
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "spite fails when the move has only 1 pp");
}

static void CheckSpiteToZero(struct BattleSim *sim)
{
    // Pound at 3 pp: 2 left after the use, and Spite's random 2..5 is capped at the remaining pp, so the
    // move always ends at 0 and the foe has to Struggle next turn.
    CHECK(LOG_HAS_T(STRINGID_PKMNREDUCEDPP, 0), "spite worked");
    CHECK(PP(1, 0) == 0, "spite capped at the remaining pp (got %d)", PP(1, 0));
    CHECK(LOG_HAS_T(STRINGID_PKMNHASNOMOVESLEFT, 1), "no moves left message");
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_STRUGGLE, 1), "struggle next turn");
}

// ---------------------------------------------------------------- Pressure / Struggle

static void CheckPressureDoublePp(struct BattleSim *sim)
{
    CHECK(HP(1) < MAXHP(1), "bite hit");
    CHECK(PP(0, 0) == 23, "bite costs 2 pp against pressure (got %d)", PP(0, 0));
}

static void CheckPressureSelfTarget(struct BattleSim *sim)
{
    CHECK(PP(0, 0) == 29, "swords dance costs 1 (got %d)", PP(0, 0));
    CHECK(PP(0, 1) == 9, "protect costs 1 (got %d)", PP(0, 1));
}

static void CheckPressureSpread(struct BattleSim *sim)
{
    CHECK(PP(0, 0) == 7, "earthquake costs 1 + 2 pressure foes (got %d)", PP(0, 0));
    CHECK(PP(0, 1) == 12, "surf costs 1 + 2 pressure foes (got %d)", PP(0, 1));
}

static void CheckPressureCounterFail(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "counter failed");
    CHECK(PP(0, 0) == 19, "a failed counter ignores pressure (got %d)", PP(0, 0));
}

static void CheckPressurePerishSong(struct BattleSim *sim)
{
    CHECK(STATUS3(1) & STATUS3_PERISH_SONG, "perish song applied");
    CHECK(PP(0, 0) == 3, "perish song costs 2 against pressure (got %d)", PP(0, 0));
}

static void CheckStruggleRecoil(struct BattleSim *sim)
{
    int dealt = MAXHP(1) - HP(1);
    int recoil = dealt / 4 ? dealt / 4 : 1;
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_STRUGGLE, 0), "no pp: struggle");
    CHECK(dealt > 0, "struggle hit");
    CHECK(HP(0) == MAXHP(0) - recoil, "recoil 1/4 of damage dealt (dealt %d, hp %d/%d)", dealt, HP(0), MAXHP(0));
    CHECK(PP(0, 0) == 0, "no pp used");
}

static void CheckStruggleVsGhost(struct BattleSim *sim)
{
    // Struggle skips the type calculation entirely (Cmd_typecalc returns for MOVE_STRUGGLE): it hits a Ghost
    // for neutral damage and the usual 1/4 recoil applies.
    int dealt = MAXHP(1) - HP(1);
    int recoil = dealt / 4 ? dealt / 4 : 1;
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_STRUGGLE, 0), "struggled");
    CHECK(!LOG_HAS(STRINGID_ITDOESNTAFFECT), "struggle ignores ghost immunity");
    CHECK(!LOG_HAS(STRINGID_NOTVERYEFFECTIVE) && !LOG_HAS(STRINGID_SUPEREFFECTIVE), "no effectiveness message");
    CHECK(dealt > 0, "the ghost took damage (hp %d/%d)", HP(1), MAXHP(1));
    CHECK(HP(0) == MAXHP(0) - recoil, "recoil 1/4 of damage dealt (dealt %d, hp %d/%d)", dealt, HP(0), MAXHP(0));
}

// ---------------------------------------------------------------- Focus Punch

static int WantFocusPunchLands(struct BattleSim *sim) { return HP(1) < MAXHP(1); }
static void CheckFocusPunchStatus(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNTIGHTENINGFOCUS), "tightening focus");
    CHECK(STATUS1(0) & STATUS1_PARALYSIS, "paralyzed by thunder wave");
    CHECK(!LOG_HAS(STRINGID_PKMNLOSTFOCUS), "a status move does not break focus");
    CHECK(HP(1) < MAXHP(1), "focus punch landed");
}

static void CheckFocusPunchLost(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNTIGHTENINGFOCUS), "tightening focus");
    CHECK(LOG_HAS(STRINGID_PKMNLOSTFOCUS), "lost focus");
    CHECK(HP(1) == MAXHP(1), "no punch");
    CHECK(PP(0, 0) == 19, "pp still deducted (got %d)", PP(0, 0));
}

static void CheckFocusPunchBehindSub(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNTIGHTENINGFOCUS, 1), "tightening focus");
    CHECK(LOG_HAS_T(STRINGID_SUBSTITUTEDAMAGED, 1), "the substitute took the pound");
    CHECK(!LOG_HAS(STRINGID_PKMNLOSTFOCUS), "damage to the substitute does not break focus");
    CHECK(HP(1) < MAXHP(1), "focus punch landed");
}

static void CheckFocusPunchVsProtect(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNTIGHTENINGFOCUS), "tightening focus");
    CHECK(LOG_HAS(STRINGID_PKMNPROTECTEDITSELF), "blocked by protect");
    CHECK(HP(1) == MAXHP(1), "no damage");
    CHECK(PP(0, 0) == 19, "pp deducted (got %d)", PP(0, 0));
}

static void CheckFocusPunchAsleep(struct BattleSim *sim)
{
    CHECK(!LOG_HAS(STRINGID_PKMNTIGHTENINGFOCUS), "no focus message while asleep");
    CHECK(LOG_HAS(STRINGID_PKMNFASTASLEEP), "fast asleep");
    CHECK(HP(1) == MAXHP(1), "no punch");
}

// ================================================================ scenarios

#define ENEMY_SNORLAX_SPLASH MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH)
#define ENEMY_RATTATA_POUND  MON(SPECIES_RATTATA, 50, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH)

static const struct Scenario sScenarios[] =
{
    // --- Protect / Detect ---
    { .name = "protect_blocks_pound",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_PROTECT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { ENEMY_RATTATA_POUND },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckProtectBlocksPound },
    { .name = "protect_chain_three_successes",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_PROTECT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { ENEMY_RATTATA_POUND },
      .actions = { T(0, 0), T(0, 0), T(0, 0) }, .turns = 3, .wantSeed = WantProtectThreeTimes, .check = CheckProtectChainThree },
    { .name = "protect_failure_resets_counter",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_PROTECT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { ENEMY_RATTATA_POUND },
      .actions = { T(0, 0), T(0, 0), T(0, 0) }, .turns = 3, .wantSeed = WantProtectFailsTurn1, .check = CheckProtectFailResets },
    { .name = "protect_counter_reset_by_other_move",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_PROTECT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { ENEMY_RATTATA_POUND },
      .actions = { T(0, 0), T(1, 0), T(0, 0) }, .turns = 3, .check = CheckProtectResetByOtherMove },
    { .name = "detect_shares_protect_counter",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_PROTECT, MOVE_DETECT, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { ENEMY_RATTATA_POUND },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .wantSeed = WantDetectFails, .check = CheckDetectSharesCounter },
    { .name = "protect_fails_when_moving_last",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_PROTECT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { ENEMY_RATTATA_POUND, MON(SPECIES_PIDGEY, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, SC_SWITCH(1)) }, .turns = 1, .check = CheckProtectFailsWhenLast },
    { .name = "protect_not_vs_future_sight",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_PROTECT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_ALAKAZAM, 50, MOVE_FUTURE_SIGHT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(1, 0), T(1, 1), T(0, 1) }, .turns = 3, .check = CheckProtectNotVsFutureSight },
    { .name = "protect_blocks_roar",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_PROTECT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_ROAR, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckProtectBlocksRoar },
    { .name = "protect_blocks_fly_landing",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_PROTECT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_PIDGEOT, 50, MOVE_FLY, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(1, 0), T(0, 0) }, .turns = 2, .check = CheckProtectBlocksFlyLanding },
    { .name = "protect_not_vs_ghost_curse",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_PROTECT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_GENGAR, 50, MOVE_CURSE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckProtectNotVsGhostCurse },
    { .name = "protect_not_vs_perish_song",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_PROTECT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_MISDREAVUS, 50, MOVE_PERISH_SONG, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckProtectNotVsPerishSong },
    { .name = "protect_blocks_bide_release",
      .player = { MON(SPECIES_CHANSEY, 50, MOVE_POUND, MOVE_PROTECT, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_BIDE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0), T(1, 0) }, .turns = 3, .check = CheckProtectBlocksBideRelease },

    // --- Endure / Focus Band ---
    { .name = "endure_survives_at_one_hp",
      .player = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_ENDURE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 30, .hpSet = 1 }, MON(SPECIES_PIDGEY, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_MACHAMP, 50, MOVE_KARATE_CHOP, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckEndureSurvives },
    { .name = "endure_then_protect_shares_chain",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_ENDURE, MOVE_PROTECT, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .wantSeed = WantProtectAfterEndureFails, .check = CheckEndureProtectChain },
    { .name = "endure_beats_focus_band",
      .player = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_ENDURE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .item = ITEM_FOCUS_BAND, .hp = 30, .hpSet = 1 }, MON(SPECIES_PIDGEY, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_MACHAMP, 50, MOVE_KARATE_CHOP, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckEndureBeatsFocusBand },
    { .name = "focus_band_hangs_on",
      .player = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .item = ITEM_FOCUS_BAND, .hp = 30, .hpSet = 1 }, MON(SPECIES_PIDGEY, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_MACHAMP, 50, MOVE_KARATE_CHOP, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantFocusBand, .check = CheckFocusBandHangsOn },
    { .name = "endure_not_vs_poison",
      .player = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_ENDURE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 5, .hpSet = 1, .status = STATUS1_POISON }, MON(SPECIES_PIDGEY, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { ENEMY_SNORLAX_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckEndureNotVsPoison },

    // --- Counter / Mirror Coat ---
    { .name = "counter_returns_double_physical",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_COUNTER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantNoCrit, .check = CheckCounterDouble },
    { .name = "counter_fails_vs_special",
      .player = { MON(SPECIES_CHANSEY, 50, MOVE_COUNTER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SURF, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckCounterFailsVsSpecial },
    { .name = "mirror_coat_returns_double_special",
      .player = { MON(SPECIES_CHANSEY, 50, MOVE_MIRROR_COAT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SURF, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantNoCrit, .check = CheckMirrorCoatDouble },
    { .name = "mirror_coat_fails_vs_physical",
      .player = { MON(SPECIES_CHANSEY, 50, MOVE_MIRROR_COAT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckMirrorCoatFailsVsPhysical },
    { .name = "counter_vs_ghost_no_effect",
      .player = { MON(SPECIES_CHANSEY, 50, MOVE_COUNTER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_GENGAR, 50, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckCounterVsGhost },
    { .name = "mirror_coat_vs_dark_no_effect",
      .player = { MON(SPECIES_CHANSEY, 50, MOVE_MIRROR_COAT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_UMBREON, 50, MOVE_SURF, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckMirrorCoatVsDark },
    { .name = "counter_after_substitute_hit_fails",
      .player = { MON(SPECIES_CHANSEY, 50, MOVE_SUBSTITUTE, MOVE_COUNTER, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { ENEMY_RATTATA_POUND },
      .actions = { T(0, 1), T(1, 0) }, .turns = 2, .check = CheckCounterAfterSubHit },
    { .name = "counter_previous_turn_damage_fails",
      .player = { MON(SPECIES_CHANSEY, 50, MOVE_COUNTER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { ENEMY_RATTATA_POUND },
      .actions = { T(1, 0), T(0, 1) }, .turns = 2, .check = CheckCounterPreviousTurn },
    { .name = "counter_partner_hit_fails_doubles",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { MON(SPECIES_CHANSEY, 50, MOVE_COUNTER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_RATTATA, 50, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { ENEMY_SNORLAX_SPLASH, ENEMY_SNORLAX_SPLASH },
      .actions = { T4(0, 0, 0, 0) }, .targets = { { 0, 0, 1, 0 } }, .turns = 1, .check = CheckCounterPartnerHit },
    { .name = "counter_redirected_by_follow_me",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { MON(SPECIES_CHANSEY, 50, MOVE_COUNTER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { ENEMY_RATTATA_POUND, MON(SPECIES_CLEFABLE, 50, MOVE_FOLLOW_ME, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { { 0, 1, 0, 0 } }, .turns = 1, .wantSeed = WantNoCrit, .check = CheckCounterFollowMe },

    // --- Bide ---
    { .name = "bide_unleashes_double",
      .player = { MON(SPECIES_CHANSEY, 50, MOVE_SPLASH, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_BIDE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0), T(1, 0) }, .turns = 3, .check = CheckBideDouble },
    { .name = "bide_no_damage_fails",
      .player = { MON(SPECIES_CHANSEY, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_BIDE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0), T(0, 0) }, .turns = 3, .check = CheckBideNoDamage },
    { .name = "bide_ignores_resistance",
      .player = { MON(SPECIES_STEELIX, 50, MOVE_SPLASH, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_BIDE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0), T(1, 0) }, .turns = 3, .wantSeed = WantNoCrit, .check = CheckBideIgnoresResistance },
    { .name = "bide_vs_ghost_no_effect",
      .player = { MON(SPECIES_GENGAR, 50, MOVE_SPLASH, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_BIDE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0), T(1, 0) }, .turns = 3, .check = CheckBideVsGhost },
    { .name = "bide_cancelled_by_falling_asleep",
      .player = { MON(SPECIES_CHANSEY, 50, MOVE_BIDE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPORE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckBideSleep },
    { .name = "bide_cancelled_by_flinch",
      .player = { MON(SPECIES_PERSIAN, 50, MOVE_SPLASH, MOVE_BITE, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_CHANSEY, 50, MOVE_BIDE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .wantSeed = WantFlinchTurn1, .check = CheckBideFlinch },

    // --- Destiny Bond / Grudge ---
    { .name = "destiny_bond_same_turn",
      .player = { { .species = SPECIES_GENGAR, .level = 50, .moves = { MOVE_DESTINY_BOND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 1, .hpSet = 1 }, MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_BITE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_PIDGEY, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckDestinyBondSameTurn },
    { .name = "destiny_bond_persists_until_user_moves",
      .player = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_DESTINY_BOND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 1, .hpSet = 1 }, MON(SPECIES_PIDGEY, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_PERSIAN, 50, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_PIDGEY, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 1), T(1, 0) }, .turns = 2, .check = CheckDestinyBondPersists },
    { .name = "destiny_bond_expires_when_user_moves",
      .player = { { .species = SPECIES_GENGAR, .level = 50, .moves = { MOVE_DESTINY_BOND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 1, .hpSet = 1 }, MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_BITE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 1), T(1, 0) }, .turns = 2, .check = CheckDestinyBondExpires },
    { .name = "destiny_bond_not_by_poison",
      .player = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_DESTINY_BOND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 5, .hpSet = 1, .status = STATUS1_POISON }, MON(SPECIES_PIDGEY, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { ENEMY_SNORLAX_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckDestinyBondNotPoison },
    { .name = "destiny_bond_ignores_partner",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { { .species = SPECIES_SABLEYE, .level = 50, .moves = { MOVE_DESTINY_BOND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 1, .hpSet = 1 },
                  MON(SPECIES_SNORLAX, 50, MOVE_EARTHQUAKE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { ENEMY_SNORLAX_SPLASH, ENEMY_SNORLAX_SPLASH },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .check = CheckDestinyBondNotPartner },
    { .name = "grudge_zeroes_pp",
      .player = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_GRUDGE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 1, .hpSet = 1 }, MON(SPECIES_PIDGEY, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_PERSIAN, 50, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 1), T(1, 0) }, .turns = 2, .check = CheckGrudge },
    { .name = "grudge_not_vs_struggle",
      .player = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_GRUDGE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 1, .hpSet = 1 }, MON(SPECIES_PIDGEY, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { { .species = SPECIES_SNORLAX, .level = 50, .moves = { MOVE_POUND, MOVE_NONE, MOVE_NONE, MOVE_NONE }, .pp = { 0, 0, 0, 0 }, .ppSet = 1 } },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckGrudgeNotStruggle },
    { .name = "grudge_twice_never_fails",
      .player = { MON(SPECIES_RATTATA, 50, MOVE_GRUDGE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { ENEMY_SNORLAX_SPLASH },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .check = CheckGrudgeTwice },

    // --- Substitute ---
    { .name = "substitute_cost_quarter",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SUBSTITUTE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { ENEMY_SNORLAX_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSubstituteCost },
    { .name = "substitute_fails_at_quarter_hp",
      .player = { { .species = SPECIES_SNORLAX, .level = 50, .moves = { MOVE_SUBSTITUTE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 58, .hpSet = 1 } },
      .enemy = { ENEMY_SNORLAX_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSubstituteTooWeak },
    { .name = "substitute_at_quarter_plus_one",
      .player = { { .species = SPECIES_SNORLAX, .level = 50, .moves = { MOVE_SUBSTITUTE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 59, .hpSet = 1 } },
      .enemy = { ENEMY_SNORLAX_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSubstituteJustEnough },
    { .name = "substitute_already_up",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SUBSTITUTE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { ENEMY_SNORLAX_SPLASH },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .check = CheckSubstituteAlreadyUp },
    { .name = "substitute_absorbs_hit",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SUBSTITUTE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .check = CheckSubstituteAbsorbs },
    { .name = "substitute_breaks_without_bleed",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SUBSTITUTE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_MACHAMP, 50, MOVE_SPLASH, MOVE_KARATE_CHOP, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .check = CheckSubstituteBreaks },
    { .name = "substitute_blocks_stat_drops",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SUBSTITUTE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_GROWL, MOVE_MUD_SLAP, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1), T(1, 2) }, .turns = 3, .check = CheckSubstituteBlocksStatDrops },
    { .name = "substitute_blocks_intimidate",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SUBSTITUTE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_GYARADOS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, SC_SWITCH(1)) }, .turns = 2, .check = CheckSubstituteBlocksIntimidate },
    { .name = "substitute_not_vs_attract",
      .player = { MON(SPECIES_TAUROS, 50, MOVE_SUBSTITUTE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_CHANSEY, 50, MOVE_SPLASH, MOVE_ATTRACT, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .check = CheckSubstituteNotVsAttract },
    { .name = "substitute_confusion_self_hit_bypasses",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SUBSTITUTE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_GENGAR, 50, MOVE_CONFUSE_RAY, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .wantSeed = WantConfusionSelfHitBehindSub, .check = CheckSubstituteConfusionSelfHit },
    { .name = "substitute_drain_heals_off_sub",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SUBSTITUTE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { { .species = SPECIES_VENUSAUR, .level = 50, .moves = { MOVE_SPLASH, MOVE_GIGA_DRAIN, MOVE_SPLASH, MOVE_SPLASH }, .hp = 50, .hpSet = 1 } },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .check = CheckSubstituteDrain },
    { .name = "substitute_recoil_from_sub_damage",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SUBSTITUTE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_DOUBLE_EDGE, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .wantSeed = WantSubFaded, .check = CheckSubstituteRecoil },
    { .name = "substitute_pain_split_fails",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SUBSTITUTE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { { .species = SPECIES_MISDREAVUS, .level = 50, .moves = { MOVE_SPLASH, MOVE_PAIN_SPLIT, MOVE_SPLASH, MOVE_SPLASH }, .hp = 50, .hpSet = 1 } },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .check = CheckSubstitutePainSplit },
    { .name = "substitute_blocks_thief",
      .player = { MON_ITEM(SPECIES_SNORLAX, 50, MOVE_SUBSTITUTE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_LEFTOVERS) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_THIEF, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .check = CheckSubstituteThief },
    { .name = "substitute_baton_pass_keeps_hp",
      .player = { MON(SPECIES_SCYTHER, 50, MOVE_SUBSTITUTE, MOVE_BATON_PASS, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { ENEMY_SNORLAX_SPLASH },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckSubstituteBatonPass },
    { .name = "substitute_hit_by_sound_move",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SUBSTITUTE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_HYPER_VOICE, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .check = CheckSubstituteSoundMove },

    // --- Magic Coat / Snatch ---
    { .name = "magic_coat_bounces_leech_seed",
      .player = { MON(SPECIES_ALAKAZAM, 50, MOVE_MAGIC_COAT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_LEECH_SEED, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantLeechSeedBounced, .check = CheckMagicCoatLeechSeed },
    { .name = "magic_coat_bounces_growl",
      .player = { MON(SPECIES_ALAKAZAM, 50, MOVE_MAGIC_COAT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_GROWL, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckMagicCoatGrowl },
    { .name = "magic_coat_not_vs_taunt",
      .player = { MON(SPECIES_ALAKAZAM, 50, MOVE_MAGIC_COAT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_TAUNT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckMagicCoatNotTaunt },
    { .name = "magic_coat_fails_when_moving_last",
      .player = { MON(SPECIES_ALAKAZAM, 50, MOVE_MAGIC_COAT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_PIDGEY, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, SC_SWITCH(1)) }, .turns = 1, .check = CheckMagicCoatFailsWhenLast },
    { .name = "magic_coat_bounce_pressure_pp",
      .player = { MON(SPECIES_ALAKAZAM, 50, MOVE_MAGIC_COAT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_DUSCLOPS, 50, MOVE_TOXIC, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantToxicBounced, .check = CheckMagicCoatPressure },
    { .name = "magic_coat_one_bounce_per_turn",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { MON(SPECIES_ALAKAZAM, 50, MOVE_MAGIC_COAT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_THUNDER_WAVE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_RATTATA, 50, MOVE_THUNDER_WAVE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { { 0, 1, 0, 1 } }, .turns = 1, .wantSeed = WantSecondWaveLands, .check = CheckMagicCoatOneBounce },
    { .name = "snatch_steals_swords_dance",
      .player = { MON(SPECIES_SNEASEL, 50, MOVE_SNATCH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SWORDS_DANCE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSnatchSwordsDance },
    { .name = "snatch_steals_recover",
      .player = { { .species = SPECIES_SNEASEL, .level = 50, .moves = { MOVE_SNATCH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 20, .hpSet = 1 } },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_RECOVER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSnatchRecover },
    { .name = "snatch_not_vs_protect",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { MON(SPECIES_SNEASEL, 50, MOVE_SNATCH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_CHANSEY, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_PROTECT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_PIDGEY, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .check = CheckSnatchNotProtect },
    { .name = "snatch_steals_partner_move",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { MON(SPECIES_SNEASEL, 50, MOVE_SNATCH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_RATTATA, 50, MOVE_SWORDS_DANCE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { ENEMY_SNORLAX_SPLASH, ENEMY_SNORLAX_SPLASH },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .check = CheckSnatchPartner },
    { .name = "snatch_substitute_fails_for_snatcher",
      .player = { { .species = SPECIES_SNEASEL, .level = 50, .moves = { MOVE_SNATCH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 30, .hpSet = 1 } },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SUBSTITUTE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSnatchSubstituteFails },
    { .name = "snatch_pressure_pp",
      .player = { MON(SPECIES_SNEASEL, 50, MOVE_SNATCH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_DUSCLOPS, 50, MOVE_SWORDS_DANCE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSnatchPressure },

    // --- Follow Me / Helping Hand ---
    { .name = "follow_me_redirects_for_one_turn",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { MON(SPECIES_CLEFABLE, 50, MOVE_FOLLOW_ME, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { ENEMY_RATTATA_POUND, ENEMY_RATTATA_POUND },
      .actions = { T4(0, 0, 0, 0), T4(1, 0, 0, 0) }, .targets = { { 0, 3, 0, 3 }, { 0, 3, 0, 3 } }, .turns = 2, .check = CheckFollowMe },
    { .name = "helping_hand_boosts_partner",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { MON(SPECIES_PLUSLE, 50, MOVE_HELPING_HAND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_SNORLAX, 50, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { ENEMY_SNORLAX_SPLASH, ENEMY_SNORLAX_SPLASH },
      .actions = { T4(0, 0, 0, 0), T4(1, 0, 0, 0) }, .targets = { { 0, 0, 2, 0 }, { 0, 0, 2, 0 } }, .turns = 2, .wantSeed = WantNoCrit, .check = CheckHelpingHand },
    { .name = "helping_hand_fails_in_singles",
      .player = { MON(SPECIES_PLUSLE, 50, MOVE_HELPING_HAND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { ENEMY_SNORLAX_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckHelpingHandSingles },
    { .name = "helping_hand_fails_without_partner",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { MON(SPECIES_PLUSLE, 50, MOVE_SPLASH, MOVE_HELPING_HAND, MOVE_SPLASH, MOVE_SPLASH), { .species = SPECIES_RATTATA, .level = 50, .moves = { SPLASH4 }, .hp = 1, .hpSet = 1 } },
      .enemy = { MON(SPECIES_PERSIAN, 50, MOVE_QUICK_ATTACK, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), ENEMY_SNORLAX_SPLASH },
      .actions = { T4(0, 0, 0, 0), T4(1, 1, 0, 0) }, .targets = { { 0, 3, 0, 0 } }, .turns = 2, .check = CheckHelpingHandNoPartner },
    { .name = "helping_hand_second_fails",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { MON(SPECIES_PLUSLE, 50, MOVE_HELPING_HAND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_MINUN, 50, MOVE_HELPING_HAND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { ENEMY_SNORLAX_SPLASH, ENEMY_SNORLAX_SPLASH },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .check = CheckHelpingHandBoth },

    // --- Taunt / Torment ---
    { .name = "taunt_two_turns",
      .player = { MON(SPECIES_PERSIAN, 50, MOVE_TAUNT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_GROWL, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0), T(1, 0) }, .turns = 3, .check = CheckTaunt },
    { .name = "taunt_forces_struggle",
      .player = { MON(SPECIES_PERSIAN, 50, MOVE_TAUNT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_GROWL, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckTauntStruggle },
    { .name = "taunt_already_taunted_fails",
      .player = { MON(SPECIES_PERSIAN, 50, MOVE_TAUNT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .check = CheckTauntAgain },
    { .name = "taunt_allows_counter",
      .player = { MON(SPECIES_PERSIAN, 50, MOVE_TAUNT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_GROWL, MOVE_COUNTER, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckTauntAllowsCounter },
    { .name = "torment_blocks_consecutive_use",
      .player = { MON(SPECIES_PERSIAN, 50, MOVE_TORMENT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_POUND, MOVE_GROWL, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0), T(1, 0) }, .turns = 3, .check = CheckTorment },
    { .name = "torment_single_move_struggles",
      .player = { MON(SPECIES_PERSIAN, 50, MOVE_TORMENT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_POUND, MOVE_NONE, MOVE_NONE, MOVE_NONE) },
      .actions = { T(0, 0), T(1, 0), T(1, 0) }, .turns = 3, .check = CheckTormentStruggle },
    { .name = "torment_already_tormented_fails",
      .player = { MON(SPECIES_PERSIAN, 50, MOVE_TORMENT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_POUND, MOVE_GROWL, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 1) }, .turns = 2, .check = CheckTormentAgain },

    // --- Disable ---
    { .name = "disable_blocks_last_move",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_DISABLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_POUND, MOVE_GROWL, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .wantSeed = WantDisabledTurn0, .check = CheckDisable },
    { .name = "disable_cancels_chosen_move_same_turn",
      .player = { MON(SPECIES_PERSIAN, 50, MOVE_SPLASH, MOVE_DISABLE, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_POUND, MOVE_GROWL, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .wantSeed = WantDisabledTurn1, .check = CheckDisableSameTurn },
    { .name = "disable_fails_before_target_moves",
      .player = { MON(SPECIES_PERSIAN, 50, MOVE_DISABLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_POUND, MOVE_GROWL, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckDisableNoLastMove },
    { .name = "disable_fails_vs_struggle",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_DISABLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_POUND, MOVE_NONE, MOVE_NONE, MOVE_NONE }, .pp = { 0, 0, 0, 0 }, .ppSet = 1 } },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantDisableHits, .check = CheckDisableVsStruggle },
    { .name = "disable_fails_on_zero_pp_move",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_DISABLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_POUND, MOVE_GROWL, MOVE_NONE, MOVE_NONE }, .pp = { 1, 40, 0, 0 }, .ppSet = 1 } },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantDisableHits, .check = CheckDisableZeroPp },
    { .name = "disable_timer_expiry",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_DISABLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_POUND, MOVE_GROWL, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0), T(1, 0) }, .turns = 3, .wantSeed = WantDisableTimer2, .check = CheckDisableExpiry },

    // --- Encore ---
    { .name = "encore_replaces_choice_same_turn",
      .player = { MON(SPECIES_CLEFABLE, 50, MOVE_SPLASH, MOVE_ENCORE, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_GROWL, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .check = CheckEncoreSameTurn },
    { .name = "encore_ends_when_pp_runs_out",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_ENCORE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_GROWL, MOVE_POUND, MOVE_NONE, MOVE_NONE }, .pp = { 2, 35, 0, 0 }, .ppSet = 1 } },
      .actions = { T(0, 0), T(1, 1), T(1, 1) }, .turns = 3, .check = CheckEncorePpOut },
    { .name = "encore_fails_before_target_moves",
      .player = { MON(SPECIES_PERSIAN, 50, MOVE_ENCORE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_GROWL, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckEncoreNoLastMove },
    { .name = "encore_fails_vs_struggle",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_ENCORE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_POUND, MOVE_NONE, MOVE_NONE, MOVE_NONE }, .pp = { 0, 0, 0, 0 }, .ppSet = 1 } },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckEncoreVsStruggle },
    { .name = "encore_fails_vs_mirror_move",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_ENCORE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_MIRROR_MOVE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckEncoreVsMirrorMove },
    { .name = "encore_timer_expiry",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_ENCORE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_GROWL, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1), T(1, 1), T(1, 1) }, .turns = 4, .wantSeed = WantEncoreTimer3, .check = CheckEncoreExpiry },

    // --- Imprison ---
    { .name = "imprison_seals_shared_moves",
      .player = { MON(SPECIES_ALAKAZAM, 50, MOVE_IMPRISON, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_POUND, MOVE_GROWL, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(2, 0) }, .turns = 2, .check = CheckImprison },
    { .name = "imprison_fails_without_shared_move",
      .player = { MON(SPECIES_ALAKAZAM, 50, MOVE_IMPRISON, MOVE_PSYCHIC, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_POUND, MOVE_GROWL, MOVE_TACKLE, MOVE_BITE) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckImprisonNoShared },
    { .name = "imprison_all_sealed_struggles",
      .player = { MON(SPECIES_ALAKAZAM, 50, MOVE_IMPRISON, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_POUND, MOVE_NONE, MOVE_NONE, MOVE_NONE) },
      .actions = { T(0, 0), T(2, 0) }, .turns = 2, .check = CheckImprisonStruggle },
    { .name = "imprison_ends_on_switch",
      .player = { MON(SPECIES_ALAKAZAM, 50, MOVE_IMPRISON, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_POUND, MOVE_GROWL, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(SC_SWITCH(1), 0), T(0, 0) }, .turns = 3, .check = CheckImprisonSwitch },
    { .name = "imprison_pressure_pp",
      .player = { MON(SPECIES_ALAKAZAM, 50, MOVE_IMPRISON, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_DUSCLOPS, 50, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 1) }, .turns = 1, .check = CheckImprisonPressure },

    // --- Spite ---
    { .name = "spite_reduces_pp",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SPITE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { ENEMY_RATTATA_POUND },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSpite },
    { .name = "spite_fails_before_target_moves",
      .player = { MON(SPECIES_PERSIAN, 50, MOVE_SPITE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSpiteNoLastMove },
    { .name = "spite_fails_at_one_pp",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SPITE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_POUND, MOVE_GROWL, MOVE_NONE, MOVE_NONE }, .pp = { 2, 40, 0, 0 }, .ppSet = 1 } },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSpiteOnePp },
    { .name = "spite_caps_at_remaining_pp",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SPITE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_POUND, MOVE_NONE, MOVE_NONE, MOVE_NONE }, .pp = { 3, 0, 0, 0 }, .ppSet = 1 } },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckSpiteToZero },

    // --- Pressure / Struggle ---
    { .name = "pressure_doubles_pp_cost",
      .player = { MON(SPECIES_RATTATA, 50, MOVE_BITE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_DUSCLOPS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckPressureDoublePp },
    { .name = "pressure_not_on_self_targeting_moves",
      .player = { MON(SPECIES_RATTATA, 50, MOVE_SWORDS_DANCE, MOVE_PROTECT, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_DUSCLOPS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckPressureSelfTarget },
    { .name = "pressure_spread_moves_doubles",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { MON(SPECIES_DUGTRIO, 50, MOVE_EARTHQUAKE, MOVE_SURF, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_DUSCLOPS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_DUSCLOPS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T4(0, 0, 0, 0), T4(1, 0, 0, 0) }, .turns = 2, .check = CheckPressureSpread },
    { .name = "pressure_failed_counter_costs_one",
      .player = { MON(SPECIES_CHANSEY, 50, MOVE_COUNTER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_DUSCLOPS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckPressureCounterFail },
    { .name = "pressure_perish_song",
      .player = { MON(SPECIES_MISDREAVUS, 50, MOVE_PERISH_SONG, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_DUSCLOPS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckPressurePerishSong },
    { .name = "struggle_when_out_of_pp",
      .player = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_POUND, MOVE_NONE, MOVE_NONE, MOVE_NONE }, .pp = { 0, 0, 0, 0 }, .ppSet = 1 } },
      .enemy = { ENEMY_SNORLAX_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckStruggleRecoil },
    { .name = "struggle_ignores_ghost_immunity",
      .player = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_POUND, MOVE_NONE, MOVE_NONE, MOVE_NONE }, .pp = { 0, 0, 0, 0 }, .ppSet = 1 } },
      .enemy = { MON(SPECIES_GENGAR, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckStruggleVsGhost },

    // --- Focus Punch ---
    { .name = "focus_punch_keeps_focus_vs_status",
      .player = { MON(SPECIES_MACHAMP, 50, MOVE_FOCUS_PUNCH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_ALAKAZAM, 50, MOVE_THUNDER_WAVE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantFocusPunchLands, .check = CheckFocusPunchStatus },
    { .name = "focus_punch_lost_focus_costs_pp",
      .player = { MON(SPECIES_MACHAMP, 50, MOVE_FOCUS_PUNCH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { ENEMY_RATTATA_POUND },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckFocusPunchLost },
    { .name = "focus_punch_keeps_focus_behind_substitute",
      .player = { MON(SPECIES_MACHAMP, 50, MOVE_FOCUS_PUNCH, MOVE_SUBSTITUTE, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(1, 0), T(0, 1) }, .turns = 2, .check = CheckFocusPunchBehindSub },
    { .name = "focus_punch_vs_protect",
      .player = { MON(SPECIES_MACHAMP, 50, MOVE_FOCUS_PUNCH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_PROTECT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckFocusPunchVsProtect },
    { .name = "focus_punch_no_focus_while_asleep",
      .player = { { .species = SPECIES_MACHAMP, .level = 50, .moves = { MOVE_FOCUS_PUNCH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .status = STATUS1_SLEEP_TURN(3) } },
      .enemy = { ENEMY_SNORLAX_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckFocusPunchAsleep },
};

SCENARIO_GROUP(protect_counter, sScenarios)
