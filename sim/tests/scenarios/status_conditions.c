// Major and volatile status conditions: sleep, poison/toxic, burn, paralysis, freeze, confusion,
// infatuation, flinch, Safeguard/Mist, party cures, Synchronize/Shed Skin/Natural Cure, held-item cures.
#include "scenario.h"

// --- helpers ---------------------------------------------------------------------------------------
#define SPL4 MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH
#define MON_ST(sp, lv, m1, m2, m3, m4, st) { .species = sp, .level = lv, .moves = { m1, m2, m3, m4 }, .status = st }
#define MON_ST_AB(sp, lv, m1, m2, m3, m4, st, ab) { .species = sp, .level = lv, .moves = { m1, m2, m3, m4 }, .status = st, .abilityNum = ab }
#define MON_ST_ITEM(sp, lv, m1, m2, m3, m4, st, it) { .species = sp, .level = lv, .moves = { m1, m2, m3, m4 }, .status = st, .item = it }
#define MON_HP(sp, lv, m1, m2, m3, m4, h) { .species = sp, .level = lv, .moves = { m1, m2, m3, m4 }, .hp = h, .hpSet = 1 }
// Snorlax with Thick Fat (2nd ability) instead of Immunity: a bulky, slow, statusable Normal type.
#define SNORLAX_TF MON_AB(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1)
#define SNORLAX_TF_ST(st) MON_ST_AB(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, st, 1)
#define SNORLAX_TF_ITEM(it) { .species = SPECIES_SNORLAX, .level = 50, .moves = { SPL4 }, .abilityNum = 1, .item = it }

#define USED_MOVE_T(b, t) (LOG_INDEX_T(STRINGID_USEDMOVE, b, t) >= 0)
#define SLEEP_CTR(b) (STATUS1(b) & STATUS1_SLEEP)
#define TOXIC_CTR(b) ((STATUS1(b) & STATUS1_TOXIC_COUNTER) >> 8)
#define INFATUATED_WITH(b) (1u << (16 + (b)))

// Damage the engine would compute for `atk` hitting battler `defId` with `move` (before the 85-100% roll):
// CalculateBaseDamage (badges, burn, Guts, Thick Fat, ... included) x script multiplier x STAB.
static s32 ExpectedDmg(struct BattleSim *sim, struct BattlePokemon *atk, u8 atkId, u8 defId, u16 move, int mult, int stab)
{
    s32 d = CalculateBaseDamage(atk, &B(defId), move, 0, 0, 0, atkId, defId);
    d *= mult;
    if (stab)
        d = d * 15 / 10;
    return d;
}
static int InRoll(s32 dealt, s32 x) { return dealt >= x * 85 / 100 && dealt <= x; }

// --- seed predicates -------------------------------------------------------------------------------
static int WantWokeUpTurn1(struct BattleSim *sim) { return LOG_HAS_T(STRINGID_PKMNWOKEUP, 1); }
static int WantMissed(struct BattleSim *sim) { return LOG_HAS(STRINGID_ATTACKMISSED); }
static int WantPlayerFullHp(struct BattleSim *sim) { return HP(0) == MAXHP(0); }
static int WantEnemyPoisoned(struct BattleSim *sim) { return (STATUS1(1) & STATUS1_POISON) != 0; }
static int WantEnemyToxic(struct BattleSim *sim) { return (STATUS1(1) & STATUS1_TOXIC_POISON) != 0; }
static int WantPlayerPoisoned(struct BattleSim *sim) { return (STATUS1(0) & STATUS1_POISON) != 0; }
static int WantEnemyBurned(struct BattleSim *sim) { return (STATUS1(1) & STATUS1_BURN) != 0; }
static int WantPlayerBurned(struct BattleSim *sim) { return (STATUS1(0) & STATUS1_BURN) != 0; }
static int WantEnemyParalyzed(struct BattleSim *sim) { return (STATUS1(1) & STATUS1_PARALYSIS) != 0; }
static int WantPlayerParalyzed(struct BattleSim *sim) { return (STATUS1(0) & STATUS1_PARALYSIS) != 0; }
static int WantEnemyFrozen(struct BattleSim *sim) { return (STATUS1(1) & STATUS1_FREEZE) != 0; }
static int WantEnemyConfused(struct BattleSim *sim) { return (STATUS2(1) & STATUS2_CONFUSION) != 0; }
static int WantPlayerMovedNoCrit(struct BattleSim *sim) { return USED_MOVE_T(0, 0) && !LOG_HAS(STRINGID_CRITICALHIT) && !LOG_HAS(STRINGID_ATTACKMISSED); }
static int WantEnemyMovedTurn1(struct BattleSim *sim) { return USED_MOVE_T(1, 1); }
static int WantEnemyFullyParalyzed(struct BattleSim *sim) { return LOG_HAS_T(STRINGID_PKMNISPARALYZED, 0); }
static int WantEnemyStaysFrozen(struct BattleSim *sim) { return LOG_HAS_T(STRINGID_PKMNISFROZEN, 0); }
static int WantEnemyThaws(struct BattleSim *sim) { return LOG_HAS_T(STRINGID_PKMNWASDEFROSTED2, 0); }
static int WantConfusionEndsTurn1(struct BattleSim *sim) { return LOG_HAS_T(STRINGID_PKMNHEALEDCONFUSION, 1); }
static int WantSelfHit(struct BattleSim *sim) { return LOG_HAS_T(STRINGID_ITHURTCONFUSION, 0); }
static int WantEnemyAtkPlus2(struct BattleSim *sim) { return STAGE(1, STAT_ATK) == 8; }
static int WantAlreadyConfusedTurn1(struct BattleSim *sim) { return LOG_HAS_T(STRINGID_PKMNALREADYCONFUSED, 1); }
static int WantImmobilizedByLove(struct BattleSim *sim) { return LOG_HAS_T(STRINGID_PKMNIMMOBILIZEDBYLOVE, 0); }
static int WantInLoveButMoved(struct BattleSim *sim) { return LOG_HAS_T(STRINGID_PKMNINLOVE, 0) && !LOG_HAS_T(STRINGID_PKMNIMMOBILIZEDBYLOVE, 0); }
static int WantPlayerInfatuated(struct BattleSim *sim) { return (STATUS2(0) & STATUS2_INFATUATION) != 0; }
static int WantFlinched(struct BattleSim *sim) { return LOG_HAS(STRINGID_PKMNFLINCHED); }
static int WantEnemyCured(struct BattleSim *sim) { return STATUS1(1) == 0; }
static int WantEnemyStillPoisoned(struct BattleSim *sim) { return (STATUS1(1) & STATUS1_POISON) != 0; }

// --- SLEEP -----------------------------------------------------------------------------------------
static void CheckSporeSleepAndWake(struct BattleSim *sim)
{
    // Counter 2: fast asleep on turn 1 (2->1), wakes up on turn 2 (1->0) and moves that same turn.
    CHECK(LOG_HAS_T(STRINGID_PKMNFELLASLEEP, 0), "fell asleep message");
    CHECK(LOG_HAS_T(STRINGID_PKMNFASTASLEEP, 0), "fast asleep on turn 1");
    CHECK(!USED_MOVE_T(1, 0), "no move on turn 1");
    CHECK(LOG_HAS_T(STRINGID_PKMNWOKEUP, 1), "woke up on turn 2");
    CHECK(USED_MOVE_T(1, 1), "moves on the wake-up turn");
    CHECK(STATUS1(1) == 0, "status cleared (status %x)", STATUS1(1));
}

static void CheckSleepCounterThree(struct BattleSim *sim)
{
    // Preset counter 3: 3->2 (asleep), 2->1 (asleep), 1->0 (wakes and moves).
    CHECK(!USED_MOVE_T(1, 0) && !USED_MOVE_T(1, 1), "asleep for two turns");
    CHECK(LOG_COUNT(STRINGID_PKMNFASTASLEEP) == 2, "two fast-asleep messages (got %d)", LOG_COUNT(STRINGID_PKMNFASTASLEEP));
    CHECK(LOG_HAS_T(STRINGID_PKMNWOKEUP, 2) && USED_MOVE_T(1, 2), "wakes and moves on turn 3");
    CHECK(STATUS1(1) == 0, "status cleared");
}

static void CheckEarlyBird(struct BattleSim *sim)
{
    // Early Bird subtracts 2 per attempt: 3->1 (asleep), then 1<2 -> cleared: wakes on turn 2.
    CHECK(!USED_MOVE_T(1, 0) && LOG_HAS_T(STRINGID_PKMNFASTASLEEP, 0), "asleep turn 1");
    CHECK(LOG_HAS_T(STRINGID_PKMNWOKEUP, 1) && USED_MOVE_T(1, 1), "wakes on turn 2 (counter 3)");
    CHECK(STATUS1(1) == 0, "status cleared");
}

static void CheckInsomnia(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNSTAYEDAWAKEUSING), "stayed awake using Insomnia");
    CHECK(!(STATUS1(1) & STATUS1_SLEEP), "not asleep");
}

static void CheckVitalSpiritYawn(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNSXMADEITINEFFECTIVE), "ability made yawn ineffective");
    CHECK(!(STATUS3(1) & STATUS3_YAWN), "not drowsy");
}

static void CheckButItFailedNoStatusChange(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "but it failed");
}

static void CheckSporeVsParalyzed(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "sleep move fails on a statused target");
    CHECK(STATUS1(1) == STATUS1_PARALYSIS, "still only paralyzed (status %x)", STATUS1(1));
}

static void CheckSporeVsAsleep(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNALREADYASLEEP), "already asleep message");
    CHECK(SLEEP_CTR(1) == 4, "counter untouched by spore, decremented once by the target's attempt (got %d)", SLEEP_CTR(1));
}

static void CheckHypnosisMiss(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_ATTACKMISSED), "missed");
    CHECK(!(STATUS1(1) & STATUS1_SLEEP), "not asleep");
}

static void CheckRestHeal(struct BattleSim *sim)
{
    CHECK(HP(0) == MAXHP(0), "fully healed");
    CHECK(STATUS1(0) == STATUS1_SLEEP_TURN(3), "paralysis replaced by sleep counter 3 (status %x)", STATUS1(0));
    CHECK(LOG_HAS(STRINGID_PKMNSLEPTHEALTHY), "slept and became healthy (statused variant)");
    CHECK(LOG_HAS(STRINGID_PKMNREGAINEDHEALTH), "regained health");
}

static void CheckRestWakesThirdTurn(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNWENTTOSLEEP, 0), "went to sleep");
    CHECK(!USED_MOVE_T(0, 1) && !USED_MOVE_T(0, 2), "asleep on turns 2 and 3");
    CHECK(LOG_HAS_T(STRINGID_PKMNWOKEUP, 3) && USED_MOVE_T(0, 3), "wakes and moves on turn 4");
}

static void CheckRestFullHp(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNHPFULL), "hp full message");
    CHECK(STATUS1(0) == 0, "not asleep");
}

static void CheckSleepTalkRest(struct BattleSim *sim)
{
    // Sleep Talk can only pick Rest (MOVE_NONE slots are invalid); Rest while asleep -> "already asleep".
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_REST, 0), "sleep talk called rest");
    CHECK(LOG_HAS(STRINGID_PKMNALREADYASLEEP2), "rest: already asleep");
    CHECK(HP(0) == 10, "no heal (hp %d)", HP(0));
}

static void CheckSleepTalkSelection(struct BattleSim *sim)
{
    // Fly (two-turn) and Focus Punch are excluded: Pound is the only choice.
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_POUND, 0), "pound called");
    CHECK(!LOG_HAS_MOVE(STRINGID_USEDMOVE, MOVE_FLY) && !LOG_HAS_MOVE(STRINGID_USEDMOVE, MOVE_FOCUS_PUNCH), "fly/focus punch never called");
    CHECK(HP(1) < MAXHP(1), "pound hit");
    CHECK(PP(0, 0) == 9, "sleep talk lost 1 pp (got %d)", PP(0, 0));
    CHECK(PP(0, 3) == 35, "called move keeps its pp (got %d)", PP(0, 3));
}

static void CheckSleepTalkNoValid(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "failed with no usable moves");
    CHECK(HP(1) == MAXHP(1), "nothing was called");
}

static void CheckSnoreAsleep(struct BattleSim *sim)
{
    CHECK(LOG_HAS_MOVE(STRINGID_USEDMOVE, MOVE_SNORE), "snore used while asleep");
    CHECK(HP(1) < MAXHP(1), "snore dealt damage");
    CHECK(SLEEP_CTR(0) == 4, "sleep counter still ticks (got %d)", SLEEP_CTR(0));
}

static void CheckNightmareDamage(struct BattleSim *sim)
{
    u16 max = MAXHP(1);
    CHECK(STATUS2(1) & STATUS2_NIGHTMARE, "nightmare set");
    CHECK(LOG_HAS(STRINGID_PKMNFELLINTONIGHTMARE), "fell into a nightmare");
    CHECK(LOG_COUNT(STRINGID_PKMNLOCKEDINNIGHTMARE) == 2, "two nightmare ticks");
    CHECK(HP(1) == max - 2 * (max / 4), "1/4 per turn (hp %d/%d)", HP(1), max);
}

static void CheckNightmareEndsOnWake(struct BattleSim *sim)
{
    u16 max = MAXHP(1);
    CHECK(LOG_HAS_T(STRINGID_PKMNWOKEUP, 1), "woke up on turn 2");
    CHECK(!(STATUS2(1) & STATUS2_NIGHTMARE), "nightmare cleared on waking");
    CHECK(HP(1) == max - max / 4, "only one nightmare tick (hp %d/%d)", HP(1), max);
}

static void CheckNightmareAwakeFails(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "fails on an awake target");
    CHECK(!(STATUS2(1) & STATUS2_NIGHTMARE) && HP(1) == MAXHP(1), "no nightmare");
}

static void CheckDreamEater(struct BattleSim *sim)
{
    int dealt = MAXHP(1) - HP(1);
    int heal = dealt / 2 ? dealt / 2 : 1;
    int expect = 1 + heal > MAXHP(0) ? MAXHP(0) : 1 + heal;
    CHECK(dealt > 0, "dealt damage");
    CHECK(LOG_HAS(STRINGID_PKMNDREAMEATEN), "dream eaten message");
    CHECK(HP(0) == expect, "healed half the damage (hp %d, dealt %d)", HP(0), dealt);
}

static void CheckDreamEaterAwake(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNWASNTAFFECTED), "wasn't affected");
    CHECK(HP(1) == MAXHP(1), "no damage");
}

static void CheckYawnTiming(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNWASMADEDROWSY, 0), "made drowsy");
    CHECK(!LOG_HAS_T(STRINGID_PKMNFELLASLEEP, 0), "not asleep at the end of turn 1");
    CHECK(LOG_HAS_T(STRINGID_PKMNFELLASLEEP, 1), "fell asleep at the end of turn 2");
    CHECK(STATUS1(1) & STATUS1_SLEEP, "asleep");
    CHECK(!(STATUS3(1) & STATUS3_YAWN), "yawn timer gone");
}

static void CheckYawnTwiceFails(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 1), "second yawn fails");
    CHECK(STATUS1(1) & STATUS1_SLEEP, "asleep after turn 2");
}

static void CheckYawnBlockedByLaterStatus(struct BattleSim *sim)
{
    CHECK(STATUS1(1) == STATUS1_PARALYSIS, "paralyzed only, yawn did not put it to sleep (status %x)", STATUS1(1));
    CHECK(!LOG_HAS(STRINGID_PKMNFELLASLEEP), "no fell asleep message");
}

static void CheckYawnIgnoresLaterSafeguard(struct BattleSim *sim)
{
    CHECK(SIDE_STATUS(B_SIDE_OPPONENT) & SIDE_STATUS_SAFEGUARD, "safeguard up");
    CHECK(STATUS1(1) & STATUS1_SLEEP, "yawn sleep goes through a safeguard set afterwards");
}

static void CheckYawnBlockedBySafeguard(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNUSEDSAFEGUARD, 1), "safeguard message");
    CHECK(!(STATUS3(1) & STATUS3_YAWN), "not drowsy");
}

static void CheckUproarWakes(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNWOKEUPINUPROAR, 0), "woke up in the uproar");
    CHECK(STATUS1(1) == 0, "awake");
    CHECK(USED_MOVE_T(1, 0), "moved after waking");
}

static void CheckUproarSoundproof(struct BattleSim *sim)
{
    CHECK(!LOG_HAS(STRINGID_PKMNWOKEUPINUPROAR), "soundproof sleeper not woken");
    CHECK(SLEEP_CTR(1) == 4, "still asleep, counter ticked once (got %d)", SLEEP_CTR(1));
}

static void CheckUproarBlocksSpore(struct BattleSim *sim)
{
    CHECK(!(STATUS1(0) & STATUS1_SLEEP), "uproar user cannot be put to sleep");
    CHECK(LOG_HAS(STRINGID_PKMNCANTSLEEPINUPROAR2), "'can't sleep in an uproar' (target is the uproar user)");
}

static void CheckSoundproofUproarUserSleeps(struct BattleSim *sim)
{
    // UproarWakeUpCheck skips Soundproof battlers, even the one making the uproar: Spore works.
    CHECK(STATUS1(0) & STATUS1_SLEEP, "soundproof uproar user put to sleep by spore");
    CHECK(LOG_HAS(STRINGID_PKMNFELLASLEEP), "fell asleep");
}

static void CheckRestUnderUproar(struct BattleSim *sim)
{
    CHECK(!(STATUS1(0) & STATUS1_SLEEP), "rest could not put the user to sleep");
    CHECK(!LOG_HAS(STRINGID_PKMNREGAINEDHEALTH), "no heal");
    CHECK(LOG_HAS(STRINGID_UPROARKEPTPKMNAWAKE), "'the uproar kept it awake' (uproar user is not the sleeper)");
}

static void CheckChestoRest(struct BattleSim *sim)
{
    // Chesto Berry is checked at the end of Rest itself: healed, awake, and moving again next turn.
    CHECK(HP(0) == MAXHP(0), "healed");
    CHECK(LOG_HAS_T(STRINGID_PKMNSITEMWOKEIT, 0), "chesto berry woke it");
    CHECK(STATUS1(0) == 0 && B(0).item == ITEM_NONE, "awake, berry gone");
    CHECK(USED_MOVE_T(0, 1), "acts on the next turn");
}

static void CheckSleepTalkSnore(struct BattleSim *sim)
{
    // Snore called by Sleep Talk skips its own "fast asleep" message and hits.
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_SNORE, 0), "snore called");
    CHECK(LOG_COUNT(STRINGID_PKMNFASTASLEEP) == 1, "one fast-asleep message (got %d)", LOG_COUNT(STRINGID_PKMNFASTASLEEP));
    CHECK(HP(1) < MAXHP(1), "snore hit");
}

static int WantSubStandsNoCrit(struct BattleSim *sim) { return (STATUS2(1) & STATUS2_SUBSTITUTE) && !Sc_LogHas(sim, STRINGID_CRITICALHIT, SC_ANY_TURN); }
static void CheckSecondaryVsSubstitute(struct BattleSim *sim)
{
    CHECK(STATUS2(1) & STATUS2_SUBSTITUTE, "substitute still up");
    CHECK(!(STATUS1(1) & STATUS1_PARALYSIS) && !LOG_HAS(STRINGID_PKMNWASPARALYZED), "body slam never paralyzes through a substitute");
}

// --- POISON ----------------------------------------------------------------------------------------
static void CheckPoisonPowder(struct BattleSim *sim)
{
    CHECK(STATUS1(1) == STATUS1_POISON, "poisoned (status %x)", STATUS1(1));
    CHECK(LOG_HAS(STRINGID_PKMNWASPOISONED) && LOG_HAS(STRINGID_PKMNHURTBYPOISON), "messages");
    CHECK(HP(1) == MAXHP(1) - MAXHP(1) / 8, "1/8 poison damage (hp %d/%d)", HP(1), MAXHP(1));
}

static void CheckToxicFourTurns(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNBADLYPOISONED), "badly poisoned message");
    CHECK(TOXIC_CTR(1) == 4, "toxic counter 4 (got %d)", TOXIC_CTR(1));
    CHECK(HP(1) == MAXHP(1) - (MAXHP(1) / 16) * (1 + 2 + 3 + 4), "1+2+3+4 sixteenths (hp %d/%d)", HP(1), MAXHP(1));
}

static void CheckToxicAcrossSwitch(struct BattleSim *sim)
{
    // The end-of-turn counter increment (DoBattlerEndTurnEffects, ENDTURN_BAD_POISON) only touches
    // gBattleMons and is never written back to the party mon, so the counter restarts at 0 when the
    // mon is reloaded on switch-in: 1/16 (turn 1), out (turn 2), 1/16 (turn 3), 2/16 (turn 4).
    CHECK(B(1).species == SPECIES_RATTATA, "rattata back in");
    CHECK(STATUS1(1) & STATUS1_TOXIC_POISON, "still badly poisoned");
    CHECK(TOXIC_CTR(1) == 2, "toxic counter restarted after switching (got %d)", TOXIC_CTR(1));
    CHECK(HP(1) == MAXHP(1) - (MAXHP(1) / 16) * (1 + 1 + 2), "damage 1+1+2 sixteenths (hp %d/%d)", HP(1), MAXHP(1));
}

static void CheckDoesntAffect(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_ITDOESNTAFFECT), "it doesn't affect");
    CHECK(STATUS1(1) == 0, "no status");
}

static void CheckImmunityAbility(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNPREVENTSPOISONINGWITH), "immunity message");
    CHECK(STATUS1(1) == 0, "not poisoned");
}

static void CheckPoisonPoint(struct BattleSim *sim)
{
    CHECK(STATUS1(0) == STATUS1_POISON, "attacker poisoned by contact");
    CHECK(LOG_HAS(STRINGID_PKMNPOISONEDBY), "poisoned by ability message");
}

static void CheckAlreadyPoisoned(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNALREADYPOISONED), "already poisoned message");
    CHECK(STATUS1(1) == STATUS1_POISON, "toxic did not upgrade regular poison (status %x)", STATUS1(1));
}

static void CheckSludgeBomb(struct BattleSim *sim)
{
    CHECK(STATUS1(1) == STATUS1_POISON, "poisoned by the secondary effect");
    CHECK(LOG_HAS(STRINGID_PKMNWASPOISONED), "message");
}

static void CheckToxicMiss(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_ATTACKMISSED) && STATUS1(1) == 0, "toxic missed");
}

// --- BURN ------------------------------------------------------------------------------------------
static void CheckWillOWisp(struct BattleSim *sim)
{
    CHECK(STATUS1(1) == STATUS1_BURN, "burned");
    CHECK(LOG_HAS(STRINGID_PKMNWASBURNED) && LOG_HAS(STRINGID_PKMNHURTBYBURN), "messages");
    CHECK(HP(1) == MAXHP(1) - MAXHP(1) / 8, "1/8 burn damage (hp %d/%d)", HP(1), MAXHP(1));
}

static void CheckWaterVeil(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNSXPREVENTSBURNS), "water veil message");
    CHECK(STATUS1(1) == 0, "not burned");
}

static void CheckAlreadyBurned(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNALREADYHASBURN), "already burned message");
}

static void CheckBurnHalvesPhysical(struct BattleSim *sim)
{
    struct BattlePokemon unburned = B(0);
    s32 dealt = MAXHP(1) - HP(1);
    s32 burned = ExpectedDmg(sim, &B(0), 0, 1, MOVE_STRENGTH, 1, 0);
    s32 normal;
    unburned.status1 = 0;
    normal = ExpectedDmg(sim, &unburned, 0, 1, MOVE_STRENGTH, 1, 0);
    CHECK(STATUS1(0) & STATUS1_BURN, "attacker burned");
    CHECK(InRoll(dealt, burned), "damage in the burned range (dealt %d, expect %d..%d)", dealt, burned * 85 / 100, burned);
    CHECK(!InRoll(dealt, normal), "not in the unburned range (dealt %d, unburned %d..%d)", dealt, normal * 85 / 100, normal);
}

static void CheckBurnSpecialUnaffected(struct BattleSim *sim)
{
    struct BattlePokemon unburned = B(0);
    s32 dealt = MAXHP(1) - HP(1);
    s32 burned = ExpectedDmg(sim, &B(0), 0, 1, MOVE_THUNDER_PUNCH, 1, 0);
    s32 normal;
    unburned.status1 = 0;
    normal = ExpectedDmg(sim, &unburned, 0, 1, MOVE_THUNDER_PUNCH, 1, 0);
    CHECK(burned == normal, "special damage not halved by burn (%d vs %d)", burned, normal);
    CHECK(InRoll(dealt, burned), "damage in range (dealt %d, expect %d..%d)", dealt, burned * 85 / 100, burned);
}

static void CheckGuts(struct BattleSim *sim)
{
    struct BattlePokemon unburned = B(0);
    s32 dealt = MAXHP(1) - HP(1);
    s32 guts = ExpectedDmg(sim, &B(0), 0, 1, MOVE_STRENGTH, 1, 0);
    s32 plain;
    unburned.status1 = 0;
    plain = ExpectedDmg(sim, &unburned, 0, 1, MOVE_STRENGTH, 1, 0);
    CHECK(guts > plain, "guts boosts a burned attacker above its healthy damage (%d vs %d)", guts, plain);
    CHECK(InRoll(dealt, guts), "damage in the guts range (dealt %d, expect %d..%d)", dealt, guts * 85 / 100, guts);
}

static void CheckFlameBody(struct BattleSim *sim)
{
    CHECK(STATUS1(0) == STATUS1_BURN, "attacker burned by flame body");
    CHECK(LOG_HAS(STRINGID_PKMNBURNEDBY), "burned by ability message");
}

static void CheckFacadeParalyzed(struct BattleSim *sim)
{
    s32 dealt = MAXHP(1) - HP(1);
    s32 x = ExpectedDmg(sim, &B(0), 0, 1, MOVE_FACADE, 2, 1); // x2, STAB
    s32 single = ExpectedDmg(sim, &B(0), 0, 1, MOVE_FACADE, 1, 1);
    CHECK(InRoll(dealt, x), "facade doubled (dealt %d, expect %d..%d)", dealt, x * 85 / 100, x);
    CHECK(!InRoll(dealt, single), "not single damage");
}

static void CheckFacadeBurned(struct BattleSim *sim)
{
    // Gen 3 Facade: doubled, but the burn attack halving still applies.
    struct BattlePokemon unburned = B(0);
    s32 dealt = MAXHP(1) - HP(1);
    s32 x = ExpectedDmg(sim, &B(0), 0, 1, MOVE_FACADE, 2, 1);
    s32 noburn;
    unburned.status1 = 0;
    noburn = ExpectedDmg(sim, &unburned, 0, 1, MOVE_FACADE, 2, 1);
    CHECK(InRoll(dealt, x), "burned facade (dealt %d, expect %d..%d)", dealt, x * 85 / 100, x);
    CHECK(!InRoll(dealt, noburn), "burn halving not ignored (unburned x2 would be %d..%d)", noburn * 85 / 100, noburn);
}

static void CheckSmellingSalt(struct BattleSim *sim)
{
    s32 dealt = MAXHP(1) - HP(1);
    s32 x = ExpectedDmg(sim, &B(0), 0, 1, MOVE_SMELLING_SALT, 2, 0);
    CHECK(InRoll(dealt, x), "double damage on a paralyzed target (dealt %d, expect %d..%d)", dealt, x * 85 / 100, x);
    CHECK(STATUS1(1) == 0, "paralysis cured");
    CHECK(LOG_HAS(STRINGID_PKMNHEALEDPARALYSIS), "healed paralysis message");
}

static void CheckSmellingSaltNormal(struct BattleSim *sim)
{
    s32 dealt = MAXHP(1) - HP(1);
    s32 x = ExpectedDmg(sim, &B(0), 0, 1, MOVE_SMELLING_SALT, 1, 0);
    CHECK(InRoll(dealt, x), "normal damage on a healthy target (dealt %d, expect %d..%d)", dealt, x * 85 / 100, x);
    CHECK(!LOG_HAS(STRINGID_PKMNHEALEDPARALYSIS), "nothing to cure");
}

// --- PARALYSIS -------------------------------------------------------------------------------------
static void CheckParalyzed(struct BattleSim *sim)
{
    CHECK(STATUS1(1) == STATUS1_PARALYSIS, "paralyzed (status %x)", STATUS1(1));
    CHECK(LOG_HAS(STRINGID_PKMNWASPARALYZED), "message");
}

static void CheckLimber(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNPREVENTSPARALYSISWITH), "limber message");
    CHECK(STATUS1(1) == 0, "not paralyzed");
}

static void CheckSpeedQuartered(struct BattleSim *sim)
{
    CHECK(MOVED_BEFORE(1, 0, 0), "electrode first before paralysis");
    CHECK(STATUS1(1) & STATUS1_PARALYSIS, "paralyzed");
    CHECK(MOVED_BEFORE(0, 1, 1), "snorlax first once electrode's speed is quartered");
}

static void CheckFullParalysis(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNISPARALYZED, 0), "fully paralyzed message");
    CHECK(!USED_MOVE_T(1, 0), "did not move");
}

static void CheckStatic(struct BattleSim *sim)
{
    CHECK(STATUS1(0) == STATUS1_PARALYSIS, "attacker paralyzed by static");
    CHECK(LOG_HAS(STRINGID_PKMNWASPARALYZEDBY), "paralyzed by ability message");
}

static void CheckAlreadyParalyzed(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNISALREADYPARALYZED), "already paralyzed message");
}

static void CheckCheriMoveEnd(struct BattleSim *sim)
{
    // Berries are checked at the end of every move: the paralysis is cured before the turn ends.
    CHECK(LOG_HAS_T(STRINGID_PKMNWASPARALYZED, 0), "was paralyzed first");
    CHECK(LOG_HAS_T(STRINGID_PKMNSITEMCUREDPARALYSIS, 0), "cheri berry cured it");
    CHECK(STATUS1(1) == 0, "cured");
    CHECK(B(1).item == ITEM_NONE, "berry consumed");
}

static void CheckLumSleep(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNSITEMCUREDPROBLEM, 0), "lum berry cured");
    CHECK(STATUS1(1) == 0 && B(1).item == ITEM_NONE, "awake, berry gone");
    CHECK(USED_MOVE_T(1, 0), "moved normally after the cure");
}

static void CheckRawstPreset(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNSITEMHEALEDBURN), "rawst berry healed the burn");
    CHECK(STATUS1(1) == 0 && B(1).item == ITEM_NONE, "cured, berry gone");
    CHECK(HP(1) == MAXHP(1), "cured before any burn damage");
}

// --- FREEZE ----------------------------------------------------------------------------------------
static void CheckFrozen(struct BattleSim *sim)
{
    CHECK(STATUS1(1) == STATUS1_FREEZE, "frozen (status %x)", STATUS1(1));
    CHECK(LOG_HAS(STRINGID_PKMNWASFROZEN), "message");
}

static void CheckStaysFrozen(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNISFROZEN, 0), "is frozen solid");
    CHECK(!USED_MOVE_T(1, 0), "did not move");
    CHECK(STATUS1(1) == STATUS1_FREEZE, "still frozen");
}

static void CheckThaws(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNWASDEFROSTED2, 0), "thawed out");
    CHECK(USED_MOVE_T(1, 0), "moved on the thaw turn");
    CHECK(STATUS1(1) == 0, "no longer frozen");
}

static void CheckFireThawsTarget(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNWASDEFROSTED), "defrosted by the fire move");
    CHECK(STATUS1(1) == 0, "thawed and not burned (status %x)", STATUS1(1));
    CHECK(HP(1) < MAXHP(1), "took damage");
}

static void CheckFlameWheelThawsUser(struct BattleSim *sim)
{
    CHECK(STATUS1(0) == 0, "user thawed");
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_FLAME_WHEEL, 0), "flame wheel used");
    CHECK(LOG_HAS(STRINGID_PKMNWASDEFROSTEDBY) || LOG_HAS(STRINGID_PKMNWASDEFROSTED2), "thaw message");
    CHECK(HP(1) < MAXHP(1), "hit the target");
}

static void CheckNeverFrozen(struct BattleSim *sim)
{
    CHECK(!(STATUS1(1) & STATUS1_FREEZE) && !LOG_HAS(STRINGID_PKMNWASFROZEN), "never frozen");
}

static void CheckSporeVsFrozen(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "spore fails on a frozen target");
    CHECK(!(STATUS1(1) & STATUS1_SLEEP), "not asleep");
}

// --- CONFUSION -------------------------------------------------------------------------------------
static void CheckConfuseRayTwoTurns(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNWASCONFUSED, 0), "became confused");
    CHECK(LOG_HAS_T(STRINGID_PKMNISCONFUSED, 0), "is confused on turn 1");
    CHECK(LOG_HAS_T(STRINGID_PKMNHEALEDCONFUSION, 1), "snapped out on turn 2");
    CHECK(USED_MOVE_T(1, 1), "moves on the turn confusion ends");
    CHECK(!(STATUS2(1) & STATUS2_CONFUSION), "confusion cleared");
}

static void CheckConfusionSelfHit(struct BattleSim *sim)
{
    // Self-hit: 40-power typeless Pound against itself, no crit/STAB, then the 85-100% roll.
    s32 dealt = MAXHP(1) - HP(1);
    s32 x = CalculateBaseDamage(&B(1), &B(1), MOVE_POUND, 0, 40, 0, 1, 1);
    CHECK(LOG_HAS_T(STRINGID_ITHURTCONFUSION, 0), "hurt itself");
    CHECK(!USED_MOVE_T(1, 0), "did not move");
    CHECK(InRoll(dealt, x), "self-hit damage (dealt %d, expect %d..%d)", dealt, x * 85 / 100, x);
}

static void CheckOwnTempo(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNPREVENTSCONFUSIONWITH), "own tempo message");
    CHECK(!(STATUS2(1) & STATUS2_CONFUSION), "not confused");
}

static void CheckSwagger(struct BattleSim *sim)
{
    CHECK(STAGE(1, STAT_ATK) == 8, "+2 attack");
    CHECK(STATUS2(1) & STATUS2_CONFUSION, "confused");
    CHECK(LOG_HAS(STRINGID_PKMNWASCONFUSED), "message");
}

static void CheckSwaggerOwnTempo(struct BattleSim *sim)
{
    CHECK(STAGE(1, STAT_ATK) == 8, "attack still raised");
    CHECK(LOG_HAS(STRINGID_PKMNPREVENTSCONFUSIONWITH), "own tempo message");
    CHECK(!(STATUS2(1) & STATUS2_CONFUSION), "not confused");
}

static void CheckSwaggerSafeguard(struct BattleSim *sim)
{
    CHECK(STAGE(1, STAT_ATK) == 8, "attack raised through safeguard");
    CHECK(LOG_HAS(STRINGID_PKMNUSEDSAFEGUARD), "safeguard blocked the confusion");
    CHECK(!(STATUS2(1) & STATUS2_CONFUSION), "not confused");
}

static void CheckFlatter(struct BattleSim *sim)
{
    CHECK(STAGE(1, STAT_SPATK) == 7, "+1 sp. atk (stage %d)", STAGE(1, STAT_SPATK));
    CHECK(STATUS2(1) & STATUS2_CONFUSION, "confused");
}

static void CheckAlreadyConfused(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNALREADYCONFUSED, 1), "already confused message");
}

static void CheckPersim(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNWASCONFUSED, 0), "confused first");
    CHECK(LOG_HAS_T(STRINGID_PKMNSITEMSNAPPEDOUT, 0), "persim berry snapped it out");
    CHECK(!(STATUS2(1) & STATUS2_CONFUSION) && B(1).item == ITEM_NONE, "cured, berry gone");
}

static void CheckStatusVsSubstitute(struct BattleSim *sim)
{
    CHECK(STATUS2(1) & STATUS2_SUBSTITUTE, "substitute up");
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 0), "confuse ray fails");
    CHECK(LOG_HAS_T(STRINGID_ATTACKMISSED, 1), "swagger 'misses'");
    CHECK(STAGE(1, STAT_ATK) == 6, "swagger did not raise attack");
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 2), "will-o-wisp fails");
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 3), "yawn fails");
    CHECK(STATUS1(1) == 0 && !(STATUS2(1) & STATUS2_CONFUSION) && !(STATUS3(1) & STATUS3_YAWN), "no status through the substitute");
}

// --- INFATUATION -----------------------------------------------------------------------------------
static void CheckAttractImmobilized(struct BattleSim *sim)
{
    CHECK(STATUS2(1) & INFATUATED_WITH(0), "infatuated with battler 0");
    CHECK(LOG_HAS(STRINGID_PKMNFELLINLOVE), "fell in love");
    CHECK(LOG_HAS_T(STRINGID_PKMNINLOVE, 0) && LOG_HAS_T(STRINGID_PKMNIMMOBILIZEDBYLOVE, 0), "immobilized by love");
    CHECK(!USED_MOVE_T(1, 0), "did not move");
}

static void CheckAttractMoved(struct BattleSim *sim)
{
    CHECK(STATUS2(1) & STATUS2_INFATUATION, "infatuated");
    CHECK(LOG_HAS_T(STRINGID_PKMNINLOVE, 0) && USED_MOVE_T(1, 0), "in love but still moved");
}

static void CheckAttractFails(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "attract failed");
    CHECK(!(STATUS2(1) & STATUS2_INFATUATION), "not infatuated");
}

static void CheckOblivious(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNPREVENTSROMANCEWITH), "oblivious message");
    CHECK(!(STATUS2(1) & STATUS2_INFATUATION), "not infatuated");
}

static void CheckMentalHerb(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNFELLINLOVE, 0), "fell in love first");
    CHECK(LOG_HAS_T(STRINGID_PKMNSITEMCUREDPROBLEM, 0), "mental herb cured it");
    CHECK(!(STATUS2(1) & STATUS2_INFATUATION) && B(1).item == ITEM_NONE, "cured, herb gone");
}

static void CheckCuteCharm(struct BattleSim *sim)
{
    CHECK(STATUS2(0) & INFATUATED_WITH(1), "attacker infatuated with the target");
    CHECK(LOG_HAS(STRINGID_PKMNSXINFATUATEDY), "cute charm message");
}

static void CheckAttractTwice(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNFELLINLOVE, 0), "first attract worked");
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 1), "second attract fails (already infatuated)");
    CHECK(STATUS2(1) & STATUS2_INFATUATION, "infatuation does not wear off");
}

// --- FLINCH ----------------------------------------------------------------------------------------
static void CheckFlinch(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNFLINCHED), "flinched");
    CHECK(!USED_MOVE_T(1, 0), "target lost its move");
}

static void CheckNeverFlinch(struct BattleSim *sim)
{
    CHECK(!LOG_HAS(STRINGID_PKMNFLINCHED), "a slower attacker can never make the target flinch");
}

static void CheckInnerFocus(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNSXPREVENTSFLINCHING), "inner focus message");
    CHECK(USED_MOVE_T(1, 0), "target still moved");
}

// --- SAFEGUARD / MIST ------------------------------------------------------------------------------
static void CheckSafeguardFiveTurns(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNCOVEREDBYVEIL, 0), "safeguard set");
    CHECK(LOG_COUNT(STRINGID_PKMNUSEDSAFEGUARD) == 5, "blocked thunder wave on turns 1-5 (got %d)", LOG_COUNT(STRINGID_PKMNUSEDSAFEGUARD));
    CHECK(LOG_HAS_T(STRINGID_PKMNSAFEGUARDEXPIRED, 4), "expired at the end of turn 5");
    CHECK(LOG_HAS_T(STRINGID_PKMNWASPARALYZED, 5) && (STATUS1(1) & STATUS1_PARALYSIS), "paralyzed on turn 6");
}

static void CheckSafeguardTwice(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 1), "second safeguard fails");
    CHECK(sim->sideTimers[B_SIDE_OPPONENT].safeguardTimer == 3, "timer not refreshed (got %d)", sim->sideTimers[B_SIDE_OPPONENT].safeguardTimer);
}

static void CheckSafeguardNotVsStatic(struct BattleSim *sim)
{
    CHECK(SIDE_STATUS(B_SIDE_PLAYER) & SIDE_STATUS_SAFEGUARD, "safeguard still up");
    CHECK(STATUS1(0) & STATUS1_PARALYSIS, "static paralyzes through safeguard");
    CHECK(LOG_HAS(STRINGID_PKMNWASPARALYZEDBY), "message");
}

static void CheckSafeguardBlocksSecondary(struct BattleSim *sim)
{
    CHECK(!(STATUS1(0) & STATUS1_PARALYSIS) && !LOG_HAS(STRINGID_PKMNWASPARALYZED), "body slam never paralyzes under safeguard");
    CHECK(HP(0) < MAXHP(0), "body slam still hit");
}

static void CheckMist(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNSHROUDEDINMIST), "mist set");
    CHECK(LOG_HAS(STRINGID_PKMNPROTECTEDBYMIST), "growl blocked");
    CHECK(STAGE(0, STAT_ATK) == 6, "attack unchanged");
}

// --- HEAL BELL / AROMATHERAPY / REFRESH ------------------------------------------------------------
static void CheckHealBell(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_BELLCHIMED), "bell chimed");
    CHECK(STATUS1(0) == 0, "user cured");
    CHECK(PARTY_STATUS(B_SIDE_PLAYER, 1) == 0 && PARTY_STATUS(B_SIDE_PLAYER, 2) == 0, "party cured (%x %x)", (unsigned)PARTY_STATUS(B_SIDE_PLAYER, 1), (unsigned)PARTY_STATUS(B_SIDE_PLAYER, 2));
    CHECK(HP(0) == MAXHP(0), "cured before burn damage");
}

static void CheckHealBellSoundproof(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNSXBLOCKSY), "soundproof blocks the bell for the user");
    CHECK(STATUS1(0) == STATUS1_POISON, "soundproof user not cured (status %x)", STATUS1(0));
    CHECK(PARTY_STATUS(B_SIDE_PLAYER, 1) == STATUS1_PARALYSIS, "soundproof party member not cured");
    CHECK(PARTY_STATUS(B_SIDE_PLAYER, 2) == 0, "other party member cured");
}

static void CheckAromatherapy(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_SOOTHINGAROMA), "soothing aroma");
    CHECK(STATUS1(0) == 0 && PARTY_STATUS(B_SIDE_PLAYER, 1) == 0 && PARTY_STATUS(B_SIDE_PLAYER, 2) == 0, "everyone cured incl. soundproof");
}

static void CheckRefresh(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNSTATUSNORMAL), "status returned to normal");
    CHECK(STATUS1(0) == 0, "toxic and its counter cleared (status %x)", STATUS1(0));
    CHECK(HP(0) == MAXHP(0), "no poison damage after the cure");
}

// --- SYNCHRONIZE / SHED SKIN / NATURAL CURE / SWITCHING -------------------------------------------
static void CheckSyncParalysis(struct BattleSim *sim)
{
    CHECK(STATUS1(1) == STATUS1_PARALYSIS, "target paralyzed");
    CHECK(STATUS1(0) == STATUS1_PARALYSIS, "synchronize passed it back");
    CHECK(LOG_HAS(STRINGID_PKMNWASPARALYZED) && LOG_HAS(STRINGID_PKMNWASPARALYZEDBY), "move message, then the ability ('by') message");
}

static void CheckSyncToxic(struct BattleSim *sim)
{
    CHECK(STATUS1(1) & STATUS1_TOXIC_POISON, "target badly poisoned");
    CHECK(STATUS1(0) == STATUS1_POISON, "synchronize passes regular poison, not toxic (status %x)", STATUS1(0));
    CHECK(HP(0) == MAXHP(0) - MAXHP(0) / 8, "regular 1/8 poison damage on the user");
}

static void CheckSyncNotSleep(struct BattleSim *sim)
{
    CHECK(STATUS1(1) & STATUS1_SLEEP, "target asleep");
    CHECK(STATUS1(0) == 0, "sleep is not synchronized");
}

static void CheckSyncAttacker(struct BattleSim *sim)
{
    CHECK(STATUS1(0) == STATUS1_PARALYSIS, "synchronizer paralyzed by static");
    CHECK(STATUS1(1) == STATUS1_PARALYSIS, "static's owner paralyzed back");
}

static void CheckShedSkinCured(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNSXCUREDYPROBLEM), "shed skin message");
    CHECK(STATUS1(1) == 0 && HP(1) == MAXHP(1), "cured before poison damage (hp %d/%d)", HP(1), MAXHP(1));
}

static void CheckShedSkinNotCured(struct BattleSim *sim)
{
    CHECK(!LOG_HAS(STRINGID_PKMNSXCUREDYPROBLEM), "no cure this turn");
    CHECK(HP(1) == MAXHP(1) - MAXHP(1) / 8, "poison damage (hp %d/%d)", HP(1), MAXHP(1));
}

static void CheckNaturalCure(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_RATTATA, "switched");
    CHECK(PARTY_STATUS(B_SIDE_PLAYER, 0) == 0, "natural cure removed the paralysis on switch-out");
}

static void CheckStatusPersistsSwitch(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_RATTATA, "rattata back in");
    CHECK(STATUS1(0) & STATUS1_BURN, "still burned");
    CHECK(HP(0) == MAXHP(0) - MAXHP(0) / 8, "burn ticked only while active (hp %d/%d)", HP(0), MAXHP(0));
}

static void CheckWoWVsSleeping(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "will-o-wisp fails on a sleeping target");
    CHECK(!(STATUS1(1) & STATUS1_BURN), "not burned");
}

// --- scenarios -------------------------------------------------------------------------------------
static const struct Scenario sScenarios[] =
{
    // sleep
    { .name = "spore_sleep_and_wake",
      .player = { MON(SPECIES_PARASECT, 50, MOVE_SPORE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .wantSeed = WantWokeUpTurn1, .check = CheckSporeSleepAndWake },
    { .name = "sleep_counter_three_turns",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF_ST(STATUS1_SLEEP_TURN(3)) },
      .actions = { T(0, 0), T(0, 0), T(0, 0) }, .turns = 3, .check = CheckSleepCounterThree },
    { .name = "early_bird_halves_sleep",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_ST(SPECIES_KANGASKHAN, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, STATUS1_SLEEP_TURN(3)) },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .check = CheckEarlyBird },
    { .name = "insomnia_blocks_spore",
      .player = { MON(SPECIES_PARASECT, 50, MOVE_SPORE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_HYPNO, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckInsomnia },
    { .name = "vital_spirit_blocks_yawn",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_YAWN, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_PRIMEAPE, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckVitalSpiritYawn },
    { .name = "spore_vs_paralyzed_fails",
      .player = { MON(SPECIES_PARASECT, 50, MOVE_SPORE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF_ST(STATUS1_PARALYSIS) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSporeVsParalyzed },
    { .name = "spore_vs_asleep_already_asleep",
      .player = { MON(SPECIES_PARASECT, 50, MOVE_SPORE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF_ST(STATUS1_SLEEP_TURN(5)) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSporeVsAsleep },
    { .name = "hypnosis_miss",
      .player = { MON(SPECIES_GENGAR, 50, MOVE_HYPNOSIS, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantMissed, .check = CheckHypnosisMiss },
    { .name = "rest_heals_and_sets_sleep_3",
      .player = { { .species = SPECIES_SNORLAX, .level = 50, .moves = { MOVE_REST, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 10, .hpSet = 1, .status = STATUS1_PARALYSIS } },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantPlayerFullHp, .check = CheckRestHeal },
    { .name = "rest_wakes_on_third_turn",
      .player = { MON_HP(SPECIES_SNORLAX, 50, MOVE_REST, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 10) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0), T(1, 0), T(1, 0) }, .turns = 4, .check = CheckRestWakesThirdTurn },
    { .name = "rest_full_hp_fails",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_REST, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckRestFullHp },
    { .name = "sleep_talk_calls_rest_already_asleep",
      .player = { { .species = SPECIES_SNORLAX, .level = 50, .moves = { MOVE_SLEEP_TALK, MOVE_REST, MOVE_NONE, MOVE_NONE }, .hp = 10, .hpSet = 1, .status = STATUS1_SLEEP_TURN(5) } },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSleepTalkRest },
    { .name = "sleep_talk_skips_two_turn_and_focus_punch",
      .player = { MON_ST(SPECIES_SNORLAX, 50, MOVE_SLEEP_TALK, MOVE_FLY, MOVE_FOCUS_PUNCH, MOVE_POUND, STATUS1_SLEEP_TURN(5)) },
      .enemy = { MON(SPECIES_RHYDON, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSleepTalkSelection },
    { .name = "sleep_talk_no_valid_moves",
      .player = { MON_ST(SPECIES_SNORLAX, 50, MOVE_SLEEP_TALK, MOVE_UPROAR, MOVE_SOLAR_BEAM, MOVE_METRONOME, STATUS1_SLEEP_TURN(5)) },
      .enemy = { MON(SPECIES_RHYDON, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSleepTalkNoValid },
    { .name = "sleep_talk_awake_fails",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SLEEP_TALK, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RHYDON, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckButItFailedNoStatusChange },
    { .name = "snore_awake_fails",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SNORE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RHYDON, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckButItFailedNoStatusChange },
    { .name = "snore_while_asleep",
      .player = { MON_ST(SPECIES_SNORLAX, 50, MOVE_SNORE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, STATUS1_SLEEP_TURN(5)) },
      .enemy = { MON(SPECIES_RHYDON, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSnoreAsleep },
    { .name = "nightmare_quarter_per_turn",
      .player = { MON(SPECIES_GENGAR, 50, MOVE_NIGHTMARE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF_ST(STATUS1_SLEEP_TURN(5)) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckNightmareDamage },
    { .name = "nightmare_ends_on_wake",
      .player = { MON(SPECIES_GENGAR, 50, MOVE_NIGHTMARE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF_ST(STATUS1_SLEEP_TURN(2)) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckNightmareEndsOnWake },
    { .name = "nightmare_awake_fails",
      .player = { MON(SPECIES_GENGAR, 50, MOVE_NIGHTMARE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckNightmareAwakeFails },
    { .name = "dream_eater_heals_half",
      .player = { MON_HP(SPECIES_GENGAR, 50, MOVE_DREAM_EATER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .enemy = { SNORLAX_TF_ST(STATUS1_SLEEP_TURN(5)) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckDreamEater },
    { .name = "dream_eater_awake_no_effect",
      .player = { MON(SPECIES_GENGAR, 50, MOVE_DREAM_EATER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckDreamEaterAwake },
    { .name = "yawn_sleeps_end_of_next_turn",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_YAWN, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckYawnTiming },
    { .name = "yawn_twice_fails",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_YAWN, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .check = CheckYawnTwiceFails },
    { .name = "yawn_blocked_by_later_status",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_YAWN, MOVE_THUNDER_WAVE, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckYawnBlockedByLaterStatus },
    { .name = "yawn_ignores_safeguard_set_after",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_YAWN, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_AB(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SAFEGUARD, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .check = CheckYawnIgnoresLaterSafeguard },
    { .name = "yawn_blocked_by_safeguard",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_YAWN, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_AB(SPECIES_SNORLAX, 50, MOVE_SAFEGUARD, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .check = CheckYawnBlockedBySafeguard },
    { .name = "uproar_wakes_sleeper",
      .player = { MON(SPECIES_ELECTRODE, 50, MOVE_UPROAR, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF_ST(STATUS1_SLEEP_TURN(5)) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckUproarWakes },
    { .name = "uproar_soundproof_sleeper_stays_asleep",
      .player = { MON(SPECIES_ELECTRODE, 50, MOVE_UPROAR, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_ST(SPECIES_EXPLOUD, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, STATUS1_SLEEP_TURN(5)) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckUproarSoundproof },
    { .name = "uproar_prevents_spore",
      .player = { MON(SPECIES_PERSIAN, 50, MOVE_UPROAR, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_PARASECT, 50, MOVE_SPORE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckUproarBlocksSpore },
    { .name = "soundproof_uproar_user_can_be_slept",
      .player = { MON(SPECIES_ELECTRODE, 50, MOVE_UPROAR, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_PARASECT, 50, MOVE_SPORE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSoundproofUproarUserSleeps },
    { .name = "rest_fails_during_uproar",
      .player = { MON_HP(SPECIES_SNORLAX, 50, MOVE_REST, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 100) },
      .enemy = { MON(SPECIES_ELECTRODE, 50, MOVE_UPROAR, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckRestUnderUproar },
    { .name = "chesto_berry_rest",
      .player = { { .species = SPECIES_SNORLAX, .level = 50, .moves = { MOVE_REST, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 10, .hpSet = 1, .item = ITEM_CHESTO_BERRY } },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckChestoRest },
    { .name = "sleep_talk_calls_snore",
      .player = { MON_ST(SPECIES_SNORLAX, 50, MOVE_SLEEP_TALK, MOVE_SNORE, MOVE_NONE, MOVE_NONE, STATUS1_SLEEP_TURN(5)) },
      .enemy = { MON(SPECIES_RHYDON, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSleepTalkSnore },
    { .name = "secondary_effect_vs_substitute",
      .player = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_BODY_SLAM, MOVE_SPLASH, MOVE_SPLASH) }, // weak enough to leave the substitute standing
      .enemy = { MON(SPECIES_STEELIX, 50, MOVE_SUBSTITUTE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1), T(1, 1) }, .turns = 3, .wantSeed = WantSubStandsNoCrit, .check = CheckSecondaryVsSubstitute },

    // poison
    { .name = "poison_powder_eighth",
      .player = { MON(SPECIES_VILEPLUME, 50, MOVE_POISON_POWDER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantEnemyPoisoned, .check = CheckPoisonPowder },
    { .name = "toxic_counter_four_turns",
      .player = { MON(SPECIES_MUK, 50, MOVE_TOXIC, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF },
      .actions = { T(0, 0), T(1, 0), T(1, 0), T(1, 0) }, .turns = 4, .wantSeed = WantEnemyToxic, .check = CheckToxicFourTurns },
    { .name = "toxic_counter_across_switch",
      .player = { MON_AB(SPECIES_SNORLAX, 50, MOVE_TOXIC, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON(SPECIES_PIDGEY, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, SC_SWITCH(1)), T(1, SC_SWITCH(0)), T(1, 0) }, .turns = 4, .wantSeed = WantEnemyToxic, .check = CheckToxicAcrossSwitch },
    { .name = "toxic_vs_poison_type",
      .player = { MON_AB(SPECIES_SNORLAX, 50, MOVE_TOXIC, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .enemy = { MON(SPECIES_MUK, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckDoesntAffect },
    { .name = "poison_powder_vs_steel_type",
      .player = { MON(SPECIES_VILEPLUME, 50, MOVE_POISON_POWDER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_STEELIX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckDoesntAffect },
    { .name = "immunity_blocks_toxic",
      .player = { MON(SPECIES_MUK, 50, MOVE_TOXIC, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) }, // Immunity
      .actions = { T(0, 0) }, .turns = 1, .check = CheckImmunityAbility },
    { .name = "poison_point_on_contact",
      .player = { MON(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_NIDOKING, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantPlayerPoisoned, .check = CheckPoisonPoint },
    { .name = "toxic_vs_already_poisoned",
      .player = { MON(SPECIES_MUK, 50, MOVE_TOXIC, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF_ST(STATUS1_POISON) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckAlreadyPoisoned },
    { .name = "sludge_bomb_secondary_poison",
      .player = { MON(SPECIES_MUK, 50, MOVE_SLUDGE_BOMB, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantEnemyPoisoned, .check = CheckSludgeBomb },
    { .name = "toxic_miss",
      .player = { MON(SPECIES_MUK, 50, MOVE_TOXIC, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantMissed, .check = CheckToxicMiss },

    // burn
    { .name = "will_o_wisp_eighth",
      .player = { MON(SPECIES_MISDREAVUS, 50, MOVE_WILL_O_WISP, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantEnemyBurned, .check = CheckWillOWisp },
    { .name = "will_o_wisp_vs_fire_type",
      .player = { MON(SPECIES_MISDREAVUS, 50, MOVE_WILL_O_WISP, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_ARCANINE, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckDoesntAffect },
    { .name = "water_veil_blocks_burn",
      .player = { MON(SPECIES_MISDREAVUS, 50, MOVE_WILL_O_WISP, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_WAILORD, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckWaterVeil },
    { .name = "will_o_wisp_vs_already_burned",
      .player = { MON(SPECIES_MISDREAVUS, 50, MOVE_WILL_O_WISP, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF_ST(STATUS1_BURN) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckAlreadyBurned },
    { .name = "will_o_wisp_vs_sleeping",
      .player = { MON(SPECIES_MISDREAVUS, 50, MOVE_WILL_O_WISP, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF_ST(STATUS1_SLEEP_TURN(5)) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckWoWVsSleeping },
    { .name = "burn_halves_physical_damage",
      .player = { MON_ST(SPECIES_HITMONCHAN, 50, MOVE_STRENGTH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, STATUS1_BURN) },
      .enemy = { SNORLAX_TF },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantPlayerMovedNoCrit, .check = CheckBurnHalvesPhysical },
    { .name = "burn_leaves_special_damage",
      .player = { MON_ST(SPECIES_HITMONCHAN, 50, MOVE_THUNDER_PUNCH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, STATUS1_BURN) },
      .enemy = { SNORLAX_TF },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantPlayerMovedNoCrit, .check = CheckBurnSpecialUnaffected },
    { .name = "guts_ignores_burn_halving",
      .player = { MON_ST(SPECIES_MACHAMP, 50, MOVE_STRENGTH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, STATUS1_BURN) },
      .enemy = { SNORLAX_TF },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantPlayerMovedNoCrit, .check = CheckGuts },
    { .name = "flame_body_on_contact",
      .player = { MON(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_MAGMAR, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantPlayerBurned, .check = CheckFlameBody },
    { .name = "facade_doubles_when_paralyzed",
      .player = { MON_ST(SPECIES_DODRIO, 50, MOVE_FACADE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, STATUS1_PARALYSIS) },
      .enemy = { SNORLAX_TF },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantPlayerMovedNoCrit, .check = CheckFacadeParalyzed },
    { .name = "facade_burned_still_halved",
      .player = { MON_ST(SPECIES_DODRIO, 50, MOVE_FACADE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, STATUS1_BURN) },
      .enemy = { SNORLAX_TF },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantPlayerMovedNoCrit, .check = CheckFacadeBurned },
    { .name = "smelling_salt_doubles_and_cures",
      .player = { MON(SPECIES_HITMONCHAN, 50, MOVE_SMELLING_SALT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF_ST(STATUS1_PARALYSIS) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantPlayerMovedNoCrit, .check = CheckSmellingSalt },
    { .name = "smelling_salt_healthy_target",
      .player = { MON(SPECIES_HITMONCHAN, 50, MOVE_SMELLING_SALT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantPlayerMovedNoCrit, .check = CheckSmellingSaltNormal },

    // paralysis
    { .name = "thunder_wave_vs_electric_type",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_THUNDER_WAVE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_ELECTABUZZ, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckParalyzed },
    { .name = "thunder_wave_vs_ground_type",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_THUNDER_WAVE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RHYDON, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckDoesntAffect },
    { .name = "glare_vs_ghost_type",
      .player = { MON(SPECIES_ARBOK, 50, MOVE_GLARE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_GENGAR, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckDoesntAffect },
    { .name = "limber_blocks_thunder_wave",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_THUNDER_WAVE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_PERSIAN, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckLimber },
    { .name = "paralysis_quarters_speed",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_THUNDER_WAVE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_ELECTRODE, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .wantSeed = WantEnemyMovedTurn1, .check = CheckSpeedQuartered },
    { .name = "full_paralysis",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_ST(SPECIES_ELECTRODE, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, STATUS1_PARALYSIS) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantEnemyFullyParalyzed, .check = CheckFullParalysis },
    { .name = "body_slam_secondary_paralysis",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_BODY_SLAM, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RHYDON, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantEnemyParalyzed, .check = CheckParalyzed },
    { .name = "static_on_contact",
      .player = { MON(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_ELECTABUZZ, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantPlayerParalyzed, .check = CheckStatic },
    { .name = "thunder_wave_vs_already_paralyzed",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_THUNDER_WAVE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF_ST(STATUS1_PARALYSIS) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckAlreadyParalyzed },
    { .name = "thunder_wave_vs_sleeping",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_THUNDER_WAVE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF_ST(STATUS1_SLEEP_TURN(5)) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckButItFailedNoStatusChange },
    { .name = "cheri_berry_cures_at_move_end",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_THUNDER_WAVE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF_ITEM(ITEM_CHERI_BERRY) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckCheriMoveEnd },
    { .name = "lum_berry_cures_sleep",
      .player = { MON(SPECIES_PARASECT, 50, MOVE_SPORE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF_ITEM(ITEM_LUM_BERRY) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckLumSleep },
    { .name = "rawst_berry_cures_preset_burn",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { { .species = SPECIES_SNORLAX, .level = 50, .moves = { SPL4 }, .abilityNum = 1, .item = ITEM_RAWST_BERRY, .status = STATUS1_BURN } },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckRawstPreset },

    // freeze
    { .name = "ice_beam_freezes",
      .player = { MON(SPECIES_CLOYSTER, 50, MOVE_ICE_BEAM, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantEnemyFrozen, .check = CheckFrozen },
    { .name = "frozen_cannot_move",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF_ST(STATUS1_FREEZE) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantEnemyStaysFrozen, .check = CheckStaysFrozen },
    { .name = "frozen_thaws_and_moves",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF_ST(STATUS1_FREEZE) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantEnemyThaws, .check = CheckThaws },
    { .name = "fire_move_thaws_target",
      .player = { MON(SPECIES_MAGMAR, 50, MOVE_FLAMETHROWER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF_ST(STATUS1_FREEZE) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckFireThawsTarget },
    { .name = "flame_wheel_thaws_user",
      .player = { MON_ST(SPECIES_RAPIDASH, 50, MOVE_FLAME_WHEEL, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, STATUS1_FREEZE) },
      .enemy = { SNORLAX_TF },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckFlameWheelThawsUser },
    { .name = "ice_type_never_frozen",
      .player = { MON(SPECIES_CLOYSTER, 50, MOVE_ICE_BEAM, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_GLALIE, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0), T(0, 0), T(0, 0), T(0, 0) }, .turns = 5, .check = CheckNeverFrozen },
    { .name = "magma_armor_never_frozen",
      .player = { MON(SPECIES_CLOYSTER, 50, MOVE_POWDER_SNOW, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SLUGMA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0), T(0, 0), T(0, 0), T(0, 0), T(0, 0) }, .turns = 6, .check = CheckNeverFrozen },
    { .name = "spore_vs_frozen_fails",
      .player = { MON(SPECIES_PARASECT, 50, MOVE_SPORE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF_ST(STATUS1_FREEZE) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSporeVsFrozen },

    // confusion
    { .name = "confuse_ray_two_turns",
      .player = { MON(SPECIES_GENGAR, 50, MOVE_CONFUSE_RAY, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .wantSeed = WantConfusionEndsTurn1, .check = CheckConfuseRayTwoTurns },
    { .name = "confusion_self_hit_damage",
      .player = { MON(SPECIES_GENGAR, 50, MOVE_CONFUSE_RAY, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantSelfHit, .check = CheckConfusionSelfHit },
    { .name = "own_tempo_blocks_confuse_ray",
      .player = { MON(SPECIES_GENGAR, 50, MOVE_CONFUSE_RAY, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SPINDA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckOwnTempo },
    { .name = "swagger_raises_and_confuses",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SWAGGER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RHYDON, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantEnemyAtkPlus2, .check = CheckSwagger },
    { .name = "swagger_vs_own_tempo",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SWAGGER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SPINDA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantEnemyAtkPlus2, .check = CheckSwaggerOwnTempo },
    { .name = "swagger_vs_safeguard",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SWAGGER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RHYDON, 50, MOVE_SAFEGUARD, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantEnemyAtkPlus2, .check = CheckSwaggerSafeguard },
    { .name = "flatter_raises_and_confuses",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_FLATTER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RHYDON, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckFlatter },
    { .name = "confuse_ray_vs_already_confused",
      .player = { MON(SPECIES_GENGAR, 50, MOVE_CONFUSE_RAY, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .wantSeed = WantAlreadyConfusedTurn1, .check = CheckAlreadyConfused },
    { .name = "persim_berry_cures_confusion",
      .player = { MON(SPECIES_GENGAR, 50, MOVE_CONFUSE_RAY, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF_ITEM(ITEM_PERSIM_BERRY) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckPersim },
    { .name = "status_moves_vs_substitute",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_CONFUSE_RAY, MOVE_SWAGGER, MOVE_WILL_O_WISP, MOVE_YAWN) },
      .enemy = { MON(SPECIES_RHYDON, 50, MOVE_SUBSTITUTE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1), T(2, 1), T(3, 1) }, .turns = 4, .check = CheckStatusVsSubstitute },

    // infatuation
    { .name = "attract_immobilizes",
      .player = { MON(SPECIES_JYNX, 50, MOVE_ATTRACT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_NIDOKING, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantImmobilizedByLove, .check = CheckAttractImmobilized },
    { .name = "attract_in_love_still_moves",
      .player = { MON(SPECIES_JYNX, 50, MOVE_ATTRACT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_NIDOKING, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantInLoveButMoved, .check = CheckAttractMoved },
    { .name = "attract_same_gender_fails",
      .player = { MON(SPECIES_JYNX, 50, MOVE_ATTRACT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_MILTANK, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckAttractFails },
    { .name = "attract_genderless_fails",
      .player = { MON(SPECIES_JYNX, 50, MOVE_ATTRACT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_MAGNEMITE, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckAttractFails },
    { .name = "oblivious_blocks_attract",
      .player = { MON(SPECIES_NIDOKING, 50, MOVE_ATTRACT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_JYNX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckOblivious },
    { .name = "mental_herb_cures_attract",
      .player = { MON(SPECIES_JYNX, 50, MOVE_ATTRACT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_ITEM(SPECIES_NIDOKING, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_MENTAL_HERB) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckMentalHerb },
    { .name = "cute_charm_on_contact",
      .player = { MON(SPECIES_NIDOKING, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_CLEFABLE, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantPlayerInfatuated, .check = CheckCuteCharm },
    { .name = "attract_twice_fails",
      .player = { MON(SPECIES_JYNX, 50, MOVE_ATTRACT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_NIDOKING, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .check = CheckAttractTwice },

    // flinch
    { .name = "headbutt_flinch_when_faster",
      .player = { MON(SPECIES_PERSIAN, 50, MOVE_HEADBUTT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantFlinched, .check = CheckFlinch },
    { .name = "no_flinch_when_slower",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_HEADBUTT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_ELECTRODE, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0), T(0, 0), T(0, 0), T(0, 0), T(0, 0) }, .turns = 6, .check = CheckNeverFlinch },
    { .name = "inner_focus_blocks_fake_out",
      .player = { MON(SPECIES_PERSIAN, 50, MOVE_FAKE_OUT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_GLALIE, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckInnerFocus },
    { .name = "kings_rock_flinch",
      .player = { MON_ITEM(SPECIES_PERSIAN, 50, MOVE_SCRATCH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_KINGS_ROCK) },
      .enemy = { SNORLAX_TF },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantFlinched, .check = CheckFlinch },

    // safeguard / mist
    { .name = "safeguard_lasts_five_turns",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_THUNDER_WAVE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_ELECTRODE, 50, MOVE_SAFEGUARD, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 1), T(0, 1), T(0, 1), T(0, 1), T(0, 1) }, .turns = 6, .check = CheckSafeguardFiveTurns },
    { .name = "safeguard_twice_fails",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_ELECTRODE, 50, MOVE_SAFEGUARD, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .check = CheckSafeguardTwice },
    { .name = "safeguard_does_not_block_static",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SAFEGUARD, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_ELECTABUZZ, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0), T(1, 0), T(1, 0) }, .turns = 4, .wantSeed = WantPlayerParalyzed, .check = CheckSafeguardNotVsStatic },
    { .name = "safeguard_blocks_body_slam_paralysis",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SAFEGUARD, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_BODY_SLAM, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1), T(1, 1), T(1, 1), T(1, 1) }, .turns = 5, .check = CheckSafeguardBlocksSecondary },
    { .name = "mist_blocks_growl",
      .player = { MON(SPECIES_STARMIE, 50, MOVE_MIST, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_GROWL, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckMist },

    // heal bell / aromatherapy / refresh
    { .name = "heal_bell_cures_party",
      .player = { MON_ST(SPECIES_MILTANK, 50, MOVE_HEAL_BELL, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, STATUS1_BURN),
                  MON_ST(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, STATUS1_POISON),
                  MON_ST(SPECIES_PIDGEY, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, STATUS1_SLEEP_TURN(3)) },
      .enemy = { SNORLAX_TF },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckHealBell },
    { .name = "heal_bell_skips_soundproof",
      .player = { MON_ST(SPECIES_EXPLOUD, 50, MOVE_HEAL_BELL, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, STATUS1_POISON),
                  MON_ST(SPECIES_WHISMUR, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, STATUS1_PARALYSIS),
                  MON_ST(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, STATUS1_BURN) },
      .enemy = { SNORLAX_TF },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckHealBellSoundproof },
    { .name = "aromatherapy_cures_soundproof_too",
      .player = { MON_ST(SPECIES_EXPLOUD, 50, MOVE_AROMATHERAPY, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, STATUS1_POISON),
                  MON_ST(SPECIES_WHISMUR, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, STATUS1_PARALYSIS),
                  MON_ST(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, STATUS1_BURN) },
      .enemy = { SNORLAX_TF },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckAromatherapy },
    { .name = "refresh_cures_toxic",
      .player = { MON_ST(SPECIES_MILTANK, 50, MOVE_REFRESH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, STATUS1_TOXIC_POISON | STATUS1_TOXIC_TURN(3)) },
      .enemy = { SNORLAX_TF },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckRefresh },
    { .name = "refresh_without_status_fails",
      .player = { MON(SPECIES_MILTANK, 50, MOVE_REFRESH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckButItFailedNoStatusChange },

    // synchronize / shed skin / natural cure / switching
    { .name = "synchronize_paralysis",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_THUNDER_WAVE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_ESPEON, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSyncParalysis },
    { .name = "synchronize_toxic_becomes_poison",
      .player = { MON_AB(SPECIES_SNORLAX, 50, MOVE_TOXIC, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .enemy = { MON(SPECIES_ESPEON, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantEnemyToxic, .check = CheckSyncToxic },
    { .name = "synchronize_not_sleep",
      .player = { MON(SPECIES_PARASECT, 50, MOVE_SPORE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_ESPEON, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSyncNotSleep },
    { .name = "synchronize_attacker_vs_static",
      .player = { MON(SPECIES_ESPEON, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_ELECTABUZZ, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantPlayerParalyzed, .check = CheckSyncAttacker },
    { .name = "shed_skin_cures_before_poison_damage",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_ST(SPECIES_SEVIPER, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, STATUS1_POISON) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantEnemyCured, .check = CheckShedSkinCured },
    { .name = "shed_skin_no_cure_takes_damage",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_ST(SPECIES_SEVIPER, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, STATUS1_POISON) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantEnemyStillPoisoned, .check = CheckShedSkinNotCured },
    { .name = "natural_cure_on_switch",
      .player = { MON_ST_AB(SPECIES_STARMIE, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, STATUS1_PARALYSIS, 1), MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF },
      .actions = { T(SC_SWITCH(1), 0) }, .turns = 1, .check = CheckNaturalCure },
    { .name = "burn_persists_across_switch",
      .player = { MON_ST(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, STATUS1_BURN), MON(SPECIES_PIDGEY, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF },
      .actions = { T(SC_SWITCH(1), 0), T(SC_SWITCH(0), 0) }, .turns = 2, .check = CheckStatusPersistsSwitch },
};

SCENARIO_GROUP(status_conditions, sScenarios)
