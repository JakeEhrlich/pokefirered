// Combat, damage and status abilities: stat/power modifiers in CalculateBaseDamage (Huge Power, Guts,
// Marvel Scale, Thick Fat, Hustle, Flash Fire, Overgrow & co.), absorbing abilities (Volt/Water Absorb,
// Flash Fire), contact abilities (Static, Poison Point, Flame Body, Effect Spore, Cute Charm, Rough Skin),
// Color Change, secondary-effect abilities (Shield Dust, Serene Grace, Inner Focus, Own Tempo), status
// immunities and their move-end cures, Synchronize, Early Bird, Shed Skin, Soundproof, Sticky Hold,
// Liquid Ooze, Rock Head, Battle/Shell Armor, Sand Veil, Sturdy, Wonder Guard, Truant, Swift Swim.
//
// Damage expectations replicate CalculateBaseDamage (sim/src/pokemon.c) with the actual battle stats, and
// accept the whole 85%-100% random roll. Seeds are chosen so that no critical hit or miss disturbs them.
#include "scenario.h"

// --- damage helpers (mirror CalculateBaseDamage / typecalc / adjustnormaldamage) ---
static int BaseDmg(int atk, int def, int level, int power, int burnHalf, int flashFire)
{
    int d = atk * power;
    d *= (2 * level / 5 + 2);
    d = d / def;
    d /= 50;
    if (burnHalf)
        d /= 2;
    if (flashFire)
        d = (15 * d) / 10;
    if (d == 0)
        d = 1;
    return d + 2;
}
static int Final(int base, int stab, int typeMul10)
{
    if (stab)
        base = base * 15 / 10;
    if (typeMul10 != 10)
    {
        base = base * typeMul10 / 10;
        if (base == 0)
            base = 1;
    }
    return base;
}
// the random roll is dmg * (100 - Random() % 16) / 100
static int InRoll(int actual, int expect)
{
    int lo = expect * 85 / 100;
    if (lo == 0) lo = 1;
    return actual >= lo && actual <= expect;
}
#define BADGE(x) ((110 * (x)) / 100)           // the player's side has all badges (+10%)
#define DEALT(b) (MAXHP(b) - HP(b))
#define DMG_CHECK(actual, expect, what) CHECK(InRoll(actual, expect), what ": dealt %d, expected %d..%d", actual, (expect) * 85 / 100, expect)

static int WantCleanHit(struct BattleSim *sim)
{
    return !Sc_LogHas(sim, STRINGID_CRITICALHIT, SC_ANY_TURN) && !Sc_LogHas(sim, STRINGID_ATTACKMISSED, SC_ANY_TURN)
        && !Sc_LogHas(sim, STRINGID_PKMNISPARALYZED, SC_ANY_TURN);
}
static int WantNoCrit(struct BattleSim *sim) { return !Sc_LogHas(sim, STRINGID_CRITICALHIT, SC_ANY_TURN); }
static int WantHit(struct BattleSim *sim) { return !Sc_LogHas(sim, STRINGID_ATTACKMISSED, SC_ANY_TURN) && !Sc_LogHas(sim, STRINGID_BUTITFAILED, SC_ANY_TURN); }
static int WantMiss(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_ATTACKMISSED, SC_ANY_TURN); }
static int WantPlayerParalyzed(struct BattleSim *sim) { return (STATUS1(0) & STATUS1_PARALYSIS) != 0; }
static int WantPlayerPoisoned(struct BattleSim *sim) { return (STATUS1(0) & STATUS1_POISON) != 0; }
static int WantPlayerBurned(struct BattleSim *sim) { return (STATUS1(0) & STATUS1_BURN) != 0; }
static int WantPlayerToxic(struct BattleSim *sim) { return (STATUS1(0) & STATUS1_TOXIC_POISON) != 0; }
static int WantPlayerAnyStatus(struct BattleSim *sim) { return (STATUS1(0) & STATUS1_ANY) != 0; }
static int WantPlayerInfatuated(struct BattleSim *sim) { return (STATUS2(0) & STATUS2_INFATUATION) != 0; }
static int WantEnemyNoStatus(struct BattleSim *sim) { return STATUS1(1) == 0; }
static int WantBadlyPoisonedMsg(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_PKMNBADLYPOISONED, SC_ANY_TURN); }
static int WantEnemySeeded(struct BattleSim *sim) { return (STATUS3(1) & STATUS3_LEECHSEED) != 0; }
static int WantFlashFireMsg(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_PKMNRAISEDFIREPOWERWITH, SC_ANY_TURN) && WantNoCrit(sim); }
// ... and the boosted Flamethrower did not burn the switched-in target (a burn tick would add to the measured damage)
static int WantFlashFireMsgNoBurn(struct BattleSim *sim) { return WantFlashFireMsg(sim) && STATUS1(1) == 0; }

// ---------------------------------------------------------------- attack/defense/power modifiers
static void CheckHugePower(struct BattleSim *sim)
{
    // Huge Power doubles attack before the badge boost: attack = ((atk * 2) * 110) / 100.
    int atk = BADGE(B(0).attack * 2);
    int exp = Final(BaseDmg(atk, B(1).defense, 50, 40, 0, 0), 0, 10);
    int noBoost = Final(BaseDmg(BADGE(B(0).attack), B(1).defense, 50, 40, 0, 0), 0, 10);
    DMG_CHECK(DEALT(1), exp, "huge power pound");
    CHECK(DEALT(1) > noBoost, "more than the unboosted maximum (%d > %d)", DEALT(1), noBoost);
}
static void CheckHugePowerChoiceBand(struct BattleSim *sim)
{
    // doubling, then badge, then Choice Band (150%)
    int atk = (BADGE(B(0).attack * 2) * 150) / 100;
    int exp = Final(BaseDmg(atk, B(1).defense, 50, 40, 0, 0), 0, 10);
    DMG_CHECK(DEALT(1), exp, "huge power + choice band pound");
}
static void CheckPurePower(struct BattleSim *sim)
{
    int atk = BADGE(B(0).attack * 2);
    int exp = Final(BaseDmg(atk, B(1).defense, 50, 40, 0, 0), 0, 10);
    CHECK(B(0).ability == ABILITY_PURE_POWER, "medicham has pure power");
    DMG_CHECK(DEALT(1), exp, "pure power pound");
}
static void CheckGutsBurn(struct BattleSim *sim)
{
    // Guts: attack * 150% and the burn halving is skipped.
    int atk = (BADGE(B(0).attack) * 150) / 100;
    int exp = Final(BaseDmg(atk, B(1).defense, 50, 40, 0, 0), 0, 10);
    CHECK(STATUS1(0) & STATUS1_BURN, "still burned");
    DMG_CHECK(DEALT(1), exp, "guts + burn pound");
}
static void CheckNoGutsBurn(struct BattleSim *sim)
{
    // same mon without Guts: no boost, damage halved by the burn (before the +2)
    int exp = Final(BaseDmg(BADGE(B(0).attack), B(1).defense, 50, 40, 1, 0), 0, 10);
    CHECK(B(0).ability == ABILITY_SWARM, "swarm heracross");
    DMG_CHECK(DEALT(1), exp, "burned pound without guts");
}
static void CheckGutsParalysis(struct BattleSim *sim)
{
    // any status1 counts for Guts
    int atk = (BADGE(B(0).attack) * 150) / 100;
    int exp = Final(BaseDmg(atk, B(1).defense, 50, 40, 0, 0), 0, 10);
    DMG_CHECK(DEALT(1), exp, "guts + paralysis pound");
}
static void CheckMarvelScalePhysical(struct BattleSim *sim)
{
    // enemy Snorlax's Pound (STAB) into a paralyzed Milotic: defense = badge(def) * 150%
    int def = (BADGE(B(0).defense) * 150) / 100;
    int exp = Final(BaseDmg(B(1).attack, def, 50, 40, 0, 0), 1, 10);
    int noBoost = Final(BaseDmg(B(1).attack, BADGE(B(0).defense), 50, 40, 0, 0), 1, 10);
    DMG_CHECK(DEALT(0), exp, "pound into marvel scale");
    CHECK(DEALT(0) < noBoost * 85 / 100, "less than the unboosted minimum (%d < %d)", DEALT(0), noBoost * 85 / 100);
}
static void CheckMarvelScaleSpecial(struct BattleSim *sim)
{
    // special defense is not touched by Marvel Scale
    int exp = Final(BaseDmg(B(1).spAttack, BADGE(B(0).spDefense), 50, 90, 0, 0), 1, 10);
    DMG_CHECK(DEALT(0), exp, "psychic into marvel scale");
}
static void CheckThickFatFire(struct BattleSim *sim)
{
    // Thick Fat halves the attacker's Sp. Atk (after the badge boost) for Fire and Ice moves.
    int spa = BADGE(B(0).spAttack) / 2;
    int exp = Final(BaseDmg(spa, B(1).spDefense, 50, 95, 0, 0), 1, 10);
    CHECK(B(1).ability == ABILITY_THICK_FAT, "snorlax has thick fat");
    DMG_CHECK(DEALT(1), exp, "flamethrower into thick fat");
}
static void CheckThickFatIce(struct BattleSim *sim)
{
    int spa = BADGE(B(0).spAttack) / 2;
    int exp = Final(BaseDmg(spa, B(1).spDefense, 50, 95, 0, 0), 1, 10);
    DMG_CHECK(DEALT(1), exp, "ice beam into thick fat");
}
static void CheckThickFatWater(struct BattleSim *sim)
{
    int exp = Final(BaseDmg(BADGE(B(0).spAttack), B(1).spDefense, 50, 95, 0, 0), 1, 10);
    DMG_CHECK(DEALT(1), exp, "surf into thick fat (unaffected)");
}
static void CheckHustleDamage(struct BattleSim *sim)
{
    int atk = (BADGE(B(0).attack) * 150) / 100;
    int exp = Final(BaseDmg(atk, B(1).defense, 50, 40, 0, 0), 1, 10);
    CHECK(B(0).ability == ABILITY_HUSTLE, "togetic has hustle");
    DMG_CHECK(DEALT(1), exp, "hustle pound");
}
static void CheckHustleCanMiss(struct BattleSim *sim)
{
    // Pound has 100% accuracy; a miss can only come from Hustle's 80% on physical moves.
    CHECK(LOG_HAS(STRINGID_ATTACKMISSED), "a 100%% physical move missed under hustle");
}
static void CheckHustleSpecialNeverMisses(struct BattleSim *sim)
{
    CHECK(LOG_COUNT(STRINGID_ATTACKMISSED) == 0, "water gun never misses under hustle (misses %d)", LOG_COUNT(STRINGID_ATTACKMISSED));
    CHECK(LOG_COUNT(STRINGID_USEDMOVE) >= 16, "both used a move every turn");
}
static void CheckCompoundEyes(struct BattleSim *sim)
{
    // Rock Slide 90% * 1.3 = 117%: the roll (1..100) can never exceed it.
    CHECK(LOG_COUNT(STRINGID_ATTACKMISSED) == 0, "rock slide never missed with compound eyes (misses %d)", LOG_COUNT(STRINGID_ATTACKMISSED));
    CHECK(Sc_LogIndex(sim, STRINGID_USEDMOVE, 0, 0, 7) >= 0, "attacked on the last turn too");
}
static void CheckOvergrowAt(struct BattleSim *sim)
{
    // hp == maxHP/3 triggers Overgrow: power 35 -> 52
    int exp = Final(BaseDmg(BADGE(B(0).spAttack), B(1).spDefense, 50, 150 * 35 / 100, 0, 0), 1, 10);
    CHECK(MAXHP(0) == 155 && HP(0) == 51, "venusaur at exactly 1/3 (hp %d/%d)", HP(0), MAXHP(0));
    DMG_CHECK(DEALT(1), exp, "overgrow vine whip");
}
static void CheckOvergrowAbove(struct BattleSim *sim)
{
    int exp = Final(BaseDmg(BADGE(B(0).spAttack), B(1).spDefense, 50, 35, 0, 0), 1, 10);
    CHECK(MAXHP(0) == 155 && HP(0) == 52, "venusaur one hp above 1/3 (hp %d/%d)", HP(0), MAXHP(0));
    DMG_CHECK(DEALT(1), exp, "vine whip without overgrow");
}
static void CheckBlaze(struct BattleSim *sim)
{
    int exp = Final(BaseDmg(BADGE(B(0).spAttack), B(1).spDefense, 50, 150 * 95 / 100, 0, 0), 1, 10);
    CHECK(MAXHP(0) == 153 && HP(0) == 51, "charizard at 1/3 (hp %d/%d)", HP(0), MAXHP(0));
    DMG_CHECK(DEALT(1), exp, "blaze flamethrower");
}
static void CheckTorrent(struct BattleSim *sim)
{
    int exp = Final(BaseDmg(BADGE(B(0).spAttack), B(1).spDefense, 50, 150 * 95 / 100, 0, 0), 1, 10);
    CHECK(MAXHP(0) == 154 && HP(0) == 51, "blastoise at 1/3 (hp %d/%d)", HP(0), MAXHP(0));
    DMG_CHECK(DEALT(1), exp, "torrent surf");
}
static void CheckSwarm(struct BattleSim *sim)
{
    // Bug is physical: Swarm boosts the power, attack goes through the physical formula.
    int exp = Final(BaseDmg(BADGE(B(0).attack), B(1).defense, 50, 150 * 120 / 100, 0, 0), 1, 10);
    CHECK(MAXHP(0) == 155 && HP(0) == 51, "heracross at 1/3 (hp %d/%d)", HP(0), MAXHP(0));
    DMG_CHECK(DEALT(1), exp, "swarm megahorn");
}

// ---------------------------------------------------------------- absorbing abilities
static void CheckFlashFireBoost(struct BattleSim *sim)
{
    // turn 1: Ember absorbed (flag set, "raised fire power"); turn 2: Flamethrower gets 1.5x, second Ember
    // prints the "made it ineffective" variant and still does nothing.
    int exp = Final(BaseDmg(BADGE(B(0).spAttack), B(1).spDefense, 50, 95, 0, 1), 1, 5);
    CHECK(HP(0) == MAXHP(0), "ember did no damage to ninetales");
    CHECK(LOG_HAS_T(STRINGID_PKMNRAISEDFIREPOWERWITH, 0), "flash fire boost message on turn 1");
    CHECK(sim->sBattleResourcesStorage.flags.flags[0] & RESOURCE_FLAG_FLASH_FIRE, "flash fire flag set");
    CHECK(LOG_HAS_T(STRINGID_PKMNSXMADEYINEFFECTIVE, 1), "second ember: 'made it ineffective'");
    DMG_CHECK(DEALT(1), exp, "flash-fire boosted flamethrower (resisted)");
}
static void CheckFlashFireViaTraceWillOWisp(struct BattleSim *sim)
{
    // Trace copies Flash Fire; Will-O-Wisp is a Fire move, so it activates Flash Fire instead of burning
    // (the absorbing check has no power requirement). Turn 2 the enemy switches to Snorlax (Immunity, so
    // no Thick Fat halving), which takes the boosted Flamethrower (the flag survives on the holder).
    int exp = Final(BaseDmg(BADGE(B(0).spAttack), B(1).spDefense, 50, 95, 0, 1), 0, 10);
    CHECK(B(0).ability == ABILITY_FLASH_FIRE, "gardevoir traced flash fire (ability %d)", B(0).ability);
    CHECK(!(STATUS1(0) & STATUS1_BURN), "not burned");
    CHECK(LOG_HAS_T(STRINGID_PKMNRAISEDFIREPOWERWITH, 0), "will-o-wisp raised fire power");
    CHECK(sim->sBattleResourcesStorage.flags.flags[0] & RESOURCE_FLAG_FLASH_FIRE, "flash fire flag set");
    CHECK(B(1).species == SPECIES_SNORLAX && B(1).ability == ABILITY_IMMUNITY, "immunity snorlax switched in (ability %d)", B(1).ability);
    CHECK(STATUS1(1) == 0, "snorlax not burned by flamethrower (seed precondition, status %#x)", STATUS1(1));
    DMG_CHECK(DEALT(1), exp, "boosted flamethrower into snorlax");
}
static void CheckFlashFireVsThickFat(struct BattleSim *sim)
{
    // Both modifiers at once: Thick Fat halves the (badge-boosted) Sp. Atk before the formula, the Flash
    // Fire 1.5x is applied to the computed damage afterwards (before the +2).
    int spa = BADGE(B(0).spAttack) / 2;
    int exp = Final(BaseDmg(spa, B(1).spDefense, 50, 95, 0, 1), 1, 10);
    int noFlashFire = Final(BaseDmg(spa, B(1).spDefense, 50, 95, 0, 0), 1, 10);
    CHECK(B(1).ability == ABILITY_THICK_FAT, "snorlax has thick fat (ability %d)", B(1).ability);
    CHECK(sim->sBattleResourcesStorage.flags.flags[0] & RESOURCE_FLAG_FLASH_FIRE, "flash fire flag set");
    CHECK(STATUS1(1) == 0, "snorlax not burned by flamethrower (seed precondition, status %#x)", STATUS1(1));
    DMG_CHECK(DEALT(1), exp, "flash-fire boosted flamethrower into thick fat");
    CHECK(DEALT(1) > noFlashFire, "more than the unboosted maximum (%d > %d)", DEALT(1), noFlashFire);
}
static void CheckFlashFireFrozen(struct BattleSim *sim)
{
    // a frozen Flash Fire holder does not absorb: the Fire move hits and thaws it
    CHECK(HP(0) < MAXHP(0), "ember hit the frozen vulpix");
    CHECK(!(STATUS1(0) & STATUS1_FREEZE), "thawed");
    CHECK(LOG_HAS(STRINGID_PKMNWASDEFROSTED), "defrosted by the fire move");
    CHECK(!(sim->sBattleResourcesStorage.flags.flags[0] & RESOURCE_FLAG_FLASH_FIRE), "flash fire not activated");
    CHECK(!LOG_HAS(STRINGID_PKMNRAISEDFIREPOWERWITH), "no flash fire message");
}
static void CheckVoltAbsorbHeal(struct BattleSim *sim)
{
    CHECK(HP(0) == 1 + MAXHP(0) / 4, "healed 1/4 (hp %d, max %d)", HP(0), MAXHP(0));
    CHECK(LOG_HAS(STRINGID_PKMNRESTOREDHPUSING), "restored hp message");
}
static void CheckVoltAbsorbFull(struct BattleSim *sim)
{
    CHECK(HP(0) == MAXHP(0), "no damage");
    CHECK(LOG_HAS(STRINGID_PKMNSXMADEYUSELESS), "'made it useless' at full hp");
    CHECK(!LOG_HAS(STRINGID_PKMNRESTOREDHPUSING), "no heal message");
}
static void CheckVoltAbsorbThunderWave(struct BattleSim *sim)
{
    // power 0: not absorbed
    CHECK(STATUS1(0) & STATUS1_PARALYSIS, "thunder wave paralyzed the volt absorb holder");
    CHECK(!LOG_HAS(STRINGID_PKMNSXMADEYUSELESS) && !LOG_HAS(STRINGID_PKMNRESTOREDHPUSING), "no absorb message");
}
static void CheckWaterAbsorb(struct BattleSim *sim)
{
    CHECK(HP(0) == 1 + MAXHP(0) / 4, "healed 1/4 (hp %d, max %d)", HP(0), MAXHP(0));
    CHECK(LOG_HAS(STRINGID_PKMNRESTOREDHPUSING), "restored hp message");
}
static void CheckLightningRodSingles(struct BattleSim *sim)
{
    CHECK(HP(1) < MAXHP(1), "thunderbolt hit the lightning rod holder normally");
    CHECK(!LOG_HAS(STRINGID_PKMNSXTOOKATTACK), "no redirection message in singles");
}
static void CheckLevitate(struct BattleSim *sim)
{
    CHECK(HP(1) == MAXHP(1), "earthquake did nothing");
    CHECK(LOG_HAS(STRINGID_PKMNMAKESGROUNDMISS), "levitate message");
}

// ---------------------------------------------------------------- contact abilities
static void CheckStatic(struct BattleSim *sim)
{
    CHECK(STATUS1(0) & STATUS1_PARALYSIS, "attacker paralyzed by static");
    CHECK(LOG_HAS(STRINGID_PKMNWASPARALYZEDBY), "'paralyzed by' message");
}
static void CheckStaticNonContact(struct BattleSim *sim)
{
    CHECK(STATUS1(0) == 0, "water gun never triggers static (status %#x)", STATUS1(0));
    CHECK(LOG_COUNT(STRINGID_USEDMOVE) >= 12, "six turns played");
}
static void CheckStaticAlreadyStatused(struct BattleSim *sim)
{
    CHECK(STATUS1(0) == STATUS1_BURN, "burn stays, no paralysis added (status %#x)", STATUS1(0));
    CHECK(!LOG_HAS(STRINGID_PKMNWASPARALYZEDBY), "no static message");
}
static void CheckStaticVsLimber(struct BattleSim *sim)
{
    // Limber blocks the ability status silently (not primary/certain => no prevention message)
    CHECK(STATUS1(0) == 0, "limber attacker never paralyzed");
    CHECK(!LOG_HAS(STRINGID_PKMNSXPREVENTSYSZ) && !LOG_HAS(STRINGID_PKMNPREVENTSPARALYSISWITH), "silent");
}
static void CheckPoisonPoint(struct BattleSim *sim)
{
    CHECK(STATUS1(0) & STATUS1_POISON, "attacker poisoned by poison point");
    CHECK(LOG_HAS(STRINGID_PKMNPOISONEDBY), "'poisoned by' message");
}
static void CheckPoisonPointVsPoisonType(struct BattleSim *sim)
{
    CHECK(STATUS1(0) == 0, "poison-type attacker never poisoned");
    CHECK(!LOG_HAS(STRINGID_PKMNSXHADNOEFFECTONY) && !LOG_HAS(STRINGID_PKMNPOISONEDBY), "silent (secondary effect path)");
}
static void CheckPoisonPointVsImmunity(struct BattleSim *sim)
{
    CHECK(STATUS1(0) == 0, "immunity attacker never poisoned");
    CHECK(!LOG_HAS(STRINGID_PKMNSXPREVENTSYSZ) && !LOG_HAS(STRINGID_PKMNPREVENTSPOISONINGWITH), "silent");
}
static void CheckFlameBody(struct BattleSim *sim)
{
    CHECK(STATUS1(0) & STATUS1_BURN, "attacker burned by flame body");
    CHECK(LOG_HAS(STRINGID_PKMNBURNEDBY), "'burned by' message");
}
static void CheckFlameBodyVsFireType(struct BattleSim *sim)
{
    CHECK(STATUS1(0) == 0, "fire-type attacker never burned");
    CHECK(!LOG_HAS(STRINGID_PKMNBURNEDBY), "no burn message");
}
static void CheckEffectSpore(struct BattleSim *sim)
{
    u32 st = STATUS1(0);
    CHECK((st & STATUS1_SLEEP) || (st & STATUS1_POISON) || (st & STATUS1_PARALYSIS), "effect spore gave sleep/poison/paralysis (status %#x)", st);
    CHECK(!(st & STATUS1_BURN) && !(st & STATUS1_TOXIC_POISON) && !(st & STATUS1_FREEZE), "never burn/toxic/freeze");
    CHECK(LOG_HAS(STRINGID_PKMNMADESLEEP) || LOG_HAS(STRINGID_PKMNPOISONEDBY) || LOG_HAS(STRINGID_PKMNWASPARALYZEDBY), "ability status message");
}
static void CheckCuteCharm(struct BattleSim *sim)
{
    u8 gAtk = GetMonGender(&sim->playerParty[0]), gDef = GetMonGender(&sim->enemyParty[0]);
    CHECK(gAtk != gDef && gAtk != MON_GENDERLESS && gDef != MON_GENDERLESS, "opposite genders (%d vs %d)", gAtk, gDef);
    CHECK(STATUS2(0) & STATUS2_INFATUATION, "attacker infatuated");
    CHECK((STATUS2(0) & STATUS2_INFATUATION) == (1u << (16 + 1)), "infatuated with battler 1");
    CHECK(LOG_HAS(STRINGID_PKMNSXINFATUATEDY), "cute charm message");
}
static void CheckCuteCharmVsOblivious(struct BattleSim *sim)
{
    u8 gAtk = GetMonGender(&sim->playerParty[0]), gDef = GetMonGender(&sim->enemyParty[0]);
    CHECK(gAtk != gDef, "opposite genders (%d vs %d), so only oblivious prevents it", gAtk, gDef);
    CHECK(!(STATUS2(0) & STATUS2_INFATUATION), "oblivious attacker never infatuated");
}
static void CheckCuteCharmVsGenderless(struct BattleSim *sim)
{
    CHECK(GetMonGender(&sim->playerParty[0]) == MON_GENDERLESS, "magneton is genderless");
    CHECK(!(STATUS2(0) & STATUS2_INFATUATION), "genderless attacker never infatuated");
}
static void CheckRoughSkin(struct BattleSim *sim)
{
    CHECK(HP(0) == MAXHP(0) - MAXHP(0) / 16, "attacker lost 1/16 (hp %d/%d)", HP(0), MAXHP(0));
    CHECK(LOG_HAS(STRINGID_PKMNHURTSWITH), "rough skin message");
}
static void CheckRoughSkinNonContact(struct BattleSim *sim)
{
    CHECK(HP(0) == MAXHP(0), "no rough skin damage from water gun");
    CHECK(HP(1) < MAXHP(1), "water gun hit");
}

// ---------------------------------------------------------------- Color Change
static void CheckColorChange(struct BattleSim *sim)
{
    // Growl (power 0) does not change the type; Water Gun does; the second Water Gun is then resisted.
    CHECK(!LOG_HAS_T(STRINGID_PKMNCHANGEDTYPEWITH, 0), "growl did not change the type");
    CHECK(LOG_HAS_T(STRINGID_PKMNCHANGEDTYPEWITH, 1), "water gun changed the type");
    CHECK(B(1).type1 == TYPE_WATER && B(1).type2 == TYPE_WATER, "kecleon is now water/water (%d/%d)", B(1).type1, B(1).type2);
    CHECK(LOG_HAS_T(STRINGID_NOTVERYEFFECTIVE, 2), "third turn: water gun resisted");
    CHECK(!LOG_HAS_T(STRINGID_PKMNCHANGEDTYPEWITH, 2), "no change when already of that type");
}
static void CheckColorChangeSeismicToss(struct BattleSim *sim)
{
    CHECK(B(1).type1 == TYPE_FIGHTING && B(1).type2 == TYPE_FIGHTING, "fixed-damage move still changes the type (%d/%d)", B(1).type1, B(1).type2);
}
static void CheckColorChangeStruggle(struct BattleSim *sim)
{
    CHECK(LOG_HAS_MOVE(STRINGID_USEDMOVE, MOVE_STRUGGLE), "struggled");
    CHECK(HP(1) < MAXHP(1), "struggle hit");
    CHECK(B(1).type1 == TYPE_NORMAL && B(1).type2 == TYPE_NORMAL, "struggle never changes the type (%d/%d)", B(1).type1, B(1).type2);
    CHECK(!LOG_HAS(STRINGID_PKMNCHANGEDTYPEWITH), "no message");
}

// ---------------------------------------------------------------- secondary effect abilities
static void CheckShieldDustParalysis(struct BattleSim *sim)
{
    CHECK(STATUS1(1) == 0, "shield dust blocked every thunder shock paralysis");
    CHECK(HP(1) < MAXHP(1), "damage still dealt");
}
static void CheckShieldDustStatDrop(struct BattleSim *sim)
{
    // Stat-lowering secondary effects pass the "<= 9" filter in SetMoveEffect, but ChangeStatBuffs
    // refuses them for Shield Dust (flags == 0), even a 100% one like Rock Tomb's; silently.
    CHECK(HP(1) < MAXHP(1), "rock tomb hit");
    CHECK(STAGE(1, STAT_SPEED) == DEFAULT_STAT_STAGE, "speed not lowered (stage %d)", STAGE(1, STAT_SPEED));
    CHECK(!LOG_HAS(STRINGID_DEFENDERSSTATFELL) && !LOG_HAS(STRINGID_PKMNSXPREVENTSYLOSS), "no stat message at all");
}
static void CheckShieldDustNotThief(struct BattleSim *sim)
{
    // MOVE_EFFECT_STEAL_ITEM (31) is above the filter: the item is stolen
    CHECK(B(0).item == ITEM_LEFTOVERS && B(1).item == ITEM_NONE, "thief stole through shield dust (items %d, %d)", B(0).item, B(1).item);
    CHECK(LOG_HAS(STRINGID_PKMNSTOLEITEM), "stole message");
}
static void CheckShieldDustNotStatic(struct BattleSim *sim)
{
    CHECK(STATUS1(0) & STATUS1_PARALYSIS, "static still paralyzes a shield dust attacker");
}
static void CheckShieldDustKingsRock(struct BattleSim *sim)
{
    int t;
    // King's Rock goes through SetMoveEffect(FALSE, 0) as effect 8 with no ability marker: blocked.
    for (t = 0; t < 6; t++)
        CHECK(LOG_INDEX_T(STRINGID_USEDMOVE, 1, t) >= 0, "dustox moved on turn %d", t + 1);
    CHECK(!LOG_HAS(STRINGID_PKMNFLINCHED), "never flinched");
    CHECK(MOVED_BEFORE(0, 1, 0), "persian moved first");
}
static void CheckSereneGraceSacredFire(struct BattleSim *sim)
{
    // 50% * 2 = 100%: SetMoveEffect is called with MOVE_EFFECT_CERTAIN, the burn always lands.
    CHECK(STATUS1(1) & STATUS1_BURN, "sacred fire always burns with serene grace");
}
static void CheckInnerFocusFakeOut(struct BattleSim *sim)
{
    CHECK(MOVED_BEFORE(0, 1, 0), "fake out went first");
    CHECK(HP(1) < MAXHP(1), "fake out hit");
    CHECK(LOG_HAS(STRINGID_PKMNSXPREVENTSFLINCHING), "inner focus message (certain effect)");
    CHECK(!LOG_HAS(STRINGID_PKMNFLINCHED), "golbat did not flinch");
}
static void CheckOwnTempoConfuseRay(struct BattleSim *sim)
{
    CHECK(!(STATUS2(1) & STATUS2_CONFUSION), "not confused");
    CHECK(LOG_HAS(STRINGID_PKMNPREVENTSCONFUSIONWITH), "own tempo message");
}
static void CheckOwnTempoSwagger(struct BattleSim *sim)
{
    CHECK(STAGE(1, STAT_ATK) == DEFAULT_STAT_STAGE + 2, "swagger still raised attack (stage %d)", STAGE(1, STAT_ATK));
    CHECK(!(STATUS2(1) & STATUS2_CONFUSION), "own tempo blocked the confusion");
    CHECK(LOG_HAS(STRINGID_PKMNPREVENTSCONFUSIONWITH), "swagger's script checks own tempo explicitly and prints the message");
}

// ---------------------------------------------------------------- status immunities
static void CheckImmunityToxic(struct BattleSim *sim)
{
    CHECK(STATUS1(1) == 0, "zangoose not poisoned");
    CHECK(LOG_HAS(STRINGID_PKMNPREVENTSPOISONINGWITH), "immunity message");
}
static void CheckImmunitySecondarySilent(struct BattleSim *sim)
{
    CHECK(STATUS1(1) == 0, "poison sting never poisoned zangoose");
    CHECK(!LOG_HAS(STRINGID_PKMNPREVENTSPOISONINGWITH) && !LOG_HAS(STRINGID_PKMNSXPREVENTSYSZ), "no message for a secondary effect");
}
static void CheckLimberThunderWave(struct BattleSim *sim)
{
    CHECK(STATUS1(1) == 0, "persian not paralyzed");
    CHECK(LOG_HAS(STRINGID_PKMNPREVENTSPARALYSISWITH), "limber message");
}
static void CheckInsomnia(struct BattleSim *sim)
{
    CHECK(STATUS1(1) == 0, "hypno stays awake");
    CHECK(LOG_HAS(STRINGID_PKMNSTAYEDAWAKEUSING), "insomnia message");
}
static void CheckVitalSpirit(struct BattleSim *sim)
{
    CHECK(STATUS1(1) == 0, "primeape stays awake");
    CHECK(LOG_HAS(STRINGID_PKMNSTAYEDAWAKEUSING), "vital spirit message");
}
static void CheckWaterVeil(struct BattleSim *sim)
{
    CHECK(STATUS1(1) == 0, "wailmer not burned");
    CHECK(LOG_HAS(STRINGID_PKMNSXPREVENTSBURNS), "water veil message");
}
static void CheckMagmaArmorCure(struct BattleSim *sim)
{
    // a frozen Magma Armor holder is cured by the move-end immunity check
    CHECK(STATUS1(1) == 0, "camerupt thawed by magma armor (status %#x)", STATUS1(1));
    CHECK(LOG_HAS(STRINGID_PKMNSXCUREDITSYPROBLEM), "cured message");
}
static void CheckSkillSwapInsomnia(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNSWAPPEDABILITIES), "abilities swapped");
    CHECK(B(0).ability == ABILITY_THICK_FAT && B(1).ability == ABILITY_INSOMNIA, "hypno<->snorlax (%d, %d)", B(0).ability, B(1).ability);
    CHECK(STATUS1(1) == 0, "snorlax woke up from the gained insomnia (status %#x)", STATUS1(1));
    CHECK(LOG_HAS(STRINGID_PKMNSXCUREDITSYPROBLEM), "cured message");
}
static void CheckSkillSwapWonderGuard(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "skill swap failed against wonder guard");
    CHECK(B(0).ability == ABILITY_INSOMNIA && B(1).ability == ABILITY_WONDER_GUARD, "abilities unchanged");
}
static void CheckRolePlayWonderGuard(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "role play failed against wonder guard");
    CHECK(B(0).ability == ABILITY_INSOMNIA, "ability unchanged");
}

// ---------------------------------------------------------------- Synchronize
static void CheckSynchronizeToxic(struct BattleSim *sim)
{
    CHECK(STATUS1(0) & STATUS1_TOXIC_POISON, "espeon badly poisoned");
    CHECK((STATUS1(1) & STATUS1_POISON) && !(STATUS1(1) & STATUS1_TOXIC_POISON), "attacker gets regular poison (status %#x)", STATUS1(1));
    CHECK(LOG_HAS(STRINGID_PKMNPOISONEDBY), "'poisoned by' (ability) message");
}
static void CheckSynchronizeParalysis(struct BattleSim *sim)
{
    CHECK((STATUS1(0) & STATUS1_PARALYSIS) && (STATUS1(1) & STATUS1_PARALYSIS), "both paralyzed (%#x, %#x)", STATUS1(0), STATUS1(1));
    CHECK(LOG_HAS(STRINGID_PKMNWASPARALYZEDBY), "ability message");
}
static void CheckSynchronizeSleep(struct BattleSim *sim)
{
    CHECK(STATUS1(0) & STATUS1_SLEEP, "espeon asleep");
    CHECK(STATUS1(1) == 0, "sleep is not synchronized");
}
static void CheckSynchronizeVsPoisonType(struct BattleSim *sim)
{
    CHECK(STATUS1(0) & STATUS1_TOXIC_POISON, "espeon badly poisoned");
    CHECK(STATUS1(1) == 0, "muk (poison type) not poisoned back");
    CHECK(LOG_HAS(STRINGID_PKMNSXHADNOEFFECTONY), "'had no effect' message (primary ability status vs poison type)");
}
static void CheckSynchronizeSecondary(struct BattleSim *sim)
{
    CHECK((STATUS1(0) & STATUS1_PARALYSIS) && (STATUS1(1) & STATUS1_PARALYSIS), "secondary paralysis mirrored (%#x, %#x)", STATUS1(0), STATUS1(1));
}

// ---------------------------------------------------------------- sleep / end of turn
static void CheckEarlyBird(struct BattleSim *sim)
{
    // sleep counter 3: -2 per turn => asleep on turn 1, wakes on turn 2
    CHECK(LOG_HAS_T(STRINGID_PKMNFASTASLEEP, 0), "asleep on turn 1");
    CHECK(LOG_HAS_T(STRINGID_PKMNWOKEUP, 1), "woke up on turn 2");
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_POUND, 1), "attacked on turn 2");
}
static void CheckNormalSleep(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNFASTASLEEP, 0) && LOG_HAS_T(STRINGID_PKMNFASTASLEEP, 1), "asleep on turns 1 and 2");
    CHECK(LOG_HAS_T(STRINGID_PKMNWOKEUP, 2), "woke up on turn 3");
}
static void CheckShedSkin(struct BattleSim *sim)
{
    CHECK(STATUS1(1) == 0, "shed skin cured the poison");
    CHECK(LOG_HAS(STRINGID_PKMNSXCUREDYPROBLEM), "shed skin message");
}

// ---------------------------------------------------------------- Soundproof
static void CheckSoundproofGrowl(struct BattleSim *sim)
{
    CHECK(STAGE(1, STAT_ATK) == DEFAULT_STAT_STAGE, "growl blocked");
    CHECK(LOG_HAS(STRINGID_PKMNSXBLOCKSY), "soundproof message");
}
static void CheckSoundproofHyperVoice(struct BattleSim *sim)
{
    CHECK(HP(1) == MAXHP(1), "hyper voice blocked");
    CHECK(LOG_HAS(STRINGID_PKMNSXBLOCKSY), "soundproof message");
}
static void CheckSoundproofPerishSong(struct BattleSim *sim)
{
    CHECK(STATUS3(0) & STATUS3_PERISH_SONG, "user got the perish count");
    CHECK(!(STATUS3(1) & STATUS3_PERISH_SONG), "soundproof exploud unaffected");
    CHECK(!LOG_HAS(STRINGID_BUTITFAILED), "did not fail (the user is affected)");
}
static void CheckSoundproofHealBell(struct BattleSim *sim)
{
    CHECK(STATUS1(0) & STATUS1_BURN, "soundproof user not healed by its own heal bell");
    CHECK(PARTY_STATUS(B_SIDE_PLAYER, 1) == 0, "party rattata cured");
    CHECK(LOG_HAS(STRINGID_BELLCHIMED) && LOG_HAS(STRINGID_PKMNSXBLOCKSY), "bell + blocks messages");
}

// ---------------------------------------------------------------- items / drain / recoil
static void CheckStickyHoldThief(struct BattleSim *sim)
{
    CHECK(B(1).item == ITEM_LEFTOVERS && B(0).item == ITEM_NONE, "thief could not steal (items %d, %d)", B(0).item, B(1).item);
    CHECK(LOG_HAS(STRINGID_PKMNSXMADEYINEFFECTIVE), "sticky hold message");
}
static void CheckStickyHoldTrick(struct BattleSim *sim)
{
    CHECK(B(0).item == ITEM_LEFTOVERS && B(1).item == ITEM_ORAN_BERRY, "trick could not swap (items %d, %d)", B(0).item, B(1).item);
    CHECK(LOG_HAS(STRINGID_PKMNSXMADEYINEFFECTIVE), "sticky hold message");
}
static void CheckLiquidOozeDrain(struct BattleSim *sim)
{
    int dealt = DEALT(1), loss = dealt / 2;
    if (loss == 0) loss = 1;
    CHECK(dealt > 0, "giga drain hit");
    CHECK(HP(0) == MAXHP(0) - loss, "drainer lost half the damage (hp %d/%d, dealt %d)", HP(0), MAXHP(0), dealt);
    CHECK(LOG_HAS(STRINGID_ITSUCKEDLIQUIDOOZE), "liquid ooze message");
}
static void CheckLiquidOozeLeechSeed(struct BattleSim *sim)
{
    CHECK(STATUS3(1) & STATUS3_LEECHSEED, "seeded");
    CHECK(HP(1) == MAXHP(1) - MAXHP(1) / 8, "seeded mon lost 1/8 (hp %d/%d)", HP(1), MAXHP(1));
    CHECK(HP(0) == MAXHP(0) - MAXHP(1) / 8, "seeder damaged by the same amount instead of healed (hp %d/%d)", HP(0), MAXHP(0));
}
static void CheckRockHead(struct BattleSim *sim)
{
    CHECK(HP(1) < MAXHP(1), "double-edge hit");
    CHECK(HP(0) == MAXHP(0), "no recoil with rock head");
    CHECK(!LOG_HAS(STRINGID_PKMNHITWITHRECOIL), "no recoil message");
}
static void CheckRockHeadStruggle(struct BattleSim *sim)
{
    CHECK(LOG_HAS_MOVE(STRINGID_USEDMOVE, MOVE_STRUGGLE), "struggled");
    CHECK(HP(0) < MAXHP(0), "struggle recoil still applies with rock head");
    CHECK(LOG_HAS(STRINGID_PKMNHITWITHRECOIL), "recoil message");
}

// ---------------------------------------------------------------- crits / accuracy / OHKO / Wonder Guard
static void CheckShellArmor(struct BattleSim *sim)
{
    CHECK(STATUS2(0) & STATUS2_FOCUS_ENERGY, "focus energy up");
    CHECK(LOG_COUNT(STRINGID_CRITICALHIT) == 0, "no critical hit against shell armor (%d)", LOG_COUNT(STRINGID_CRITICALHIT));
    CHECK(LOG_COUNT(STRINGID_USEDMOVE) >= 12, "six turns played");
}
static void CheckBattleArmor(struct BattleSim *sim)
{
    CHECK(STATUS2(0) & STATUS2_FOCUS_ENERGY, "focus energy up");
    CHECK(LOG_COUNT(STRINGID_CRITICALHIT) == 0, "no critical hit against battle armor (%d)", LOG_COUNT(STRINGID_CRITICALHIT));
}
static void CheckSandVeil(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_SANDSTORMBREWED, 0), "sandstorm started");
    CHECK(LOG_HAS(STRINGID_ATTACKMISSED), "a 100%% move missed against sand veil in a sandstorm");
}
static void CheckSturdy(struct BattleSim *sim)
{
    CHECK(HP(1) == MAXHP(1), "fissure did nothing");
    CHECK(LOG_HAS(STRINGID_PKMNPROTECTEDBY), "sturdy message");
}
static void CheckWonderGuardSuperEffective(struct BattleSim *sim)
{
    CHECK(HP(1) == 0, "ember (super effective) got through wonder guard");
}
static void CheckWonderGuardStatusMove(struct BattleSim *sim)
{
    // Toxic (power 0) is not filtered by Wonder Guard; the 1-HP Shedinja then faints from the poison tick.
    CHECK(LOG_HAS(STRINGID_PKMNBADLYPOISONED), "shedinja badly poisoned");
    CHECK(LOG_HAS(STRINGID_PKMNHURTBYPOISON) && HP(1) == 0, "fainted from poison (hp %d)", HP(1));
    CHECK(OUTCOME() == B_OUTCOME_WON, "battle won (outcome %d)", OUTCOME());
}
static void CheckWonderGuardStruggle(struct BattleSim *sim)
{
    // typecalc returns early for Struggle, so Wonder Guard is never consulted
    CHECK(LOG_HAS_MOVE(STRINGID_USEDMOVE, MOVE_STRUGGLE), "struggled");
    CHECK(HP(1) == 0, "struggle knocked out shedinja");
}
static void CheckWonderGuardNightShade(struct BattleSim *sim)
{
    // Ghost vs Bug/Ghost is super effective in typecalc, so the fixed damage lands
    CHECK(HP(1) == 0, "night shade knocked out shedinja");
}

// ---------------------------------------------------------------- Truant / Swift Swim
static void CheckTruantFocusPunch(struct BattleSim *sim)
{
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_POUND, 0), "attacked on turn 1");
    CHECK(LOG_HAS_T(STRINGID_PKMNLOAFING, 1), "loafed on turn 2");
    CHECK(LOG_INDEX_T(STRINGID_USEDMOVE, 0, 1) < 0, "no move used on the loaf turn");
    CHECK(!LOG_HAS_T(STRINGID_PKMNTIGHTENINGFOCUS, 1), "no focus punch set-up message on the loaf turn");
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_POUND, 2), "attacked again on turn 3");
    CHECK(sim->disableStructs[0].truantCounter == 1, "counter toggled at the end of turn 3");
}
static void CheckSwiftSwim(struct BattleSim *sim)
{
    CHECK(MOVED_BEFORE(1, 0, 0), "jolteon faster without rain");
    CHECK(WEATHER() & B_WEATHER_RAIN, "rain up");
    CHECK(MOVED_BEFORE(0, 1, 1), "kingdra faster in rain (swift swim)");
}

#define SNORLAX_TF(lv) MON_AB(SPECIES_SNORLAX, lv, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1)      // Thick Fat (no Immunity)
#define SNORLAX_IM(lv) MON(SPECIES_SNORLAX, lv, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH)             // Immunity
#define T6(a, b) T(a, b), T(a, b), T(a, b), T(a, b), T(a, b), T(a, b)

static const struct Scenario sScenarios[] =
{
    // --- attack / defense / power modifiers ---
    { .name = "huge_power_doubles_attack",
      .player = { MON_AB(SPECIES_AZUMARILL, 50, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .enemy = { SNORLAX_TF(50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantCleanHit, .check = CheckHugePower },
    { .name = "huge_power_choice_band",
      .player = { { .species = SPECIES_AZUMARILL, .level = 50, .moves = { MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .abilityNum = 1, .item = ITEM_CHOICE_BAND } },
      .enemy = { SNORLAX_TF(50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantCleanHit, .check = CheckHugePowerChoiceBand },
    { .name = "pure_power_medicham",
      .player = { MON(SPECIES_MEDICHAM, 50, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF(50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantCleanHit, .check = CheckPurePower },
    { .name = "guts_burn_no_halving",
      .player = { { .species = SPECIES_HERACROSS, .level = 50, .moves = { MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .abilityNum = 1, .status = STATUS1_BURN } },
      .enemy = { SNORLAX_TF(50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantCleanHit, .check = CheckGutsBurn },
    { .name = "burn_halves_without_guts",
      .player = { { .species = SPECIES_HERACROSS, .level = 50, .moves = { MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .abilityNum = 0, .status = STATUS1_BURN } },
      .enemy = { SNORLAX_TF(50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantCleanHit, .check = CheckNoGutsBurn },
    { .name = "guts_paralysis",
      .player = { { .species = SPECIES_HERACROSS, .level = 50, .moves = { MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .abilityNum = 1, .status = STATUS1_PARALYSIS } },
      .enemy = { SNORLAX_TF(50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantCleanHit, .check = CheckGutsParalysis },
    { .name = "marvel_scale_physical",
      .player = { { .species = SPECIES_MILOTIC, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .status = STATUS1_PARALYSIS } },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantCleanHit, .check = CheckMarvelScalePhysical },
    { .name = "marvel_scale_special_unaffected",
      .player = { { .species = SPECIES_MILOTIC, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .status = STATUS1_PARALYSIS } },
      .enemy = { MON(SPECIES_ALAKAZAM, 50, MOVE_PSYCHIC, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantCleanHit, .check = CheckMarvelScaleSpecial },
    { .name = "thick_fat_fire",
      .player = { MON_AB(SPECIES_ARCANINE, 50, MOVE_FLAMETHROWER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .enemy = { SNORLAX_TF(50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantCleanHit, .check = CheckThickFatFire },
    { .name = "thick_fat_ice",
      .player = { MON(SPECIES_DEWGONG, 50, MOVE_ICE_BEAM, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF(50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantCleanHit, .check = CheckThickFatIce },
    { .name = "thick_fat_water_unaffected",
      .player = { MON(SPECIES_DEWGONG, 50, MOVE_SURF, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF(50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantCleanHit, .check = CheckThickFatWater },
    { .name = "hustle_damage",
      .player = { MON(SPECIES_TOGETIC, 50, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF(50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantCleanHit, .check = CheckHustleDamage },
    { .name = "hustle_physical_can_miss",
      .player = { MON(SPECIES_TOGETIC, 50, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_IM(100) },
      .actions = { T6(0, 0), T(0, 0), T(0, 0) }, .turns = 8, .wantSeed = WantMiss, .check = CheckHustleCanMiss },
    { .name = "hustle_special_never_misses",
      .player = { MON(SPECIES_TOGETIC, 50, MOVE_WATER_GUN, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_IM(100) },
      .actions = { T6(0, 0), T(0, 0), T(0, 0) }, .turns = 8, .check = CheckHustleSpecialNeverMisses },
    { .name = "compound_eyes_rock_slide_never_misses",
      .player = { MON(SPECIES_BUTTERFREE, 50, MOVE_ROCK_SLIDE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_IM(100) },
      .actions = { T6(0, 0), T(0, 0), T(0, 0) }, .turns = 8, .check = CheckCompoundEyes },
    { .name = "overgrow_at_one_third",
      .player = { { .species = SPECIES_VENUSAUR, .level = 50, .moves = { MOVE_VINE_WHIP, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 51, .hpSet = 1 } },
      .enemy = { SNORLAX_TF(50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantCleanHit, .check = CheckOvergrowAt },
    { .name = "overgrow_above_one_third",
      .player = { { .species = SPECIES_VENUSAUR, .level = 50, .moves = { MOVE_VINE_WHIP, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 52, .hpSet = 1 } },
      .enemy = { SNORLAX_TF(50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantCleanHit, .check = CheckOvergrowAbove },
    { .name = "blaze_at_one_third",
      .player = { { .species = SPECIES_CHARIZARD, .level = 50, .moves = { MOVE_FLAMETHROWER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 51, .hpSet = 1 } },
      .enemy = { SNORLAX_IM(50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantCleanHit, .check = CheckBlaze },
    { .name = "torrent_at_one_third",
      .player = { { .species = SPECIES_BLASTOISE, .level = 50, .moves = { MOVE_SURF, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 51, .hpSet = 1 } },
      .enemy = { SNORLAX_IM(50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantCleanHit, .check = CheckTorrent },
    { .name = "swarm_at_one_third",
      .player = { { .species = SPECIES_HERACROSS, .level = 50, .moves = { MOVE_MEGAHORN, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 51, .hpSet = 1 } },
      .enemy = { SNORLAX_IM(50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantCleanHit, .check = CheckSwarm },

    // --- absorbing abilities ---
    { .name = "flash_fire_absorb_then_boost",
      .player = { MON(SPECIES_NINETALES, 50, MOVE_FLAMETHROWER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RAPIDASH, 50, MOVE_EMBER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(1, 0), T(0, 0) }, .turns = 2, .wantSeed = WantNoCrit, .check = CheckFlashFireBoost },
    { .name = "flash_fire_via_trace_vs_will_o_wisp",
      .player = { MON_AB(SPECIES_GARDEVOIR, 50, MOVE_FLAMETHROWER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },   // Trace
      .enemy = { MON(SPECIES_NINETALES, 50, MOVE_WILL_O_WISP, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), SNORLAX_IM(50) },
      .actions = { T(1, 0), T(0, SC_SWITCH(1)) }, .turns = 2, .wantSeed = WantFlashFireMsgNoBurn, .check = CheckFlashFireViaTraceWillOWisp },
    { .name = "flash_fire_boost_vs_thick_fat",
      .player = { MON(SPECIES_NINETALES, 50, MOVE_FLAMETHROWER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RAPIDASH, 50, MOVE_EMBER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), SNORLAX_TF(50) },
      .actions = { T(1, 0), T(0, SC_SWITCH(1)) }, .turns = 2, .wantSeed = WantFlashFireMsgNoBurn, .check = CheckFlashFireVsThickFat },
    { .name = "flash_fire_frozen_holder_is_hit",
      .player = { { .species = SPECIES_VULPIX, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .status = STATUS1_FREEZE } },
      .enemy = { MON(SPECIES_RAPIDASH, 50, MOVE_EMBER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckFlashFireFrozen },
    { .name = "volt_absorb_heals_quarter",
      .player = { { .species = SPECIES_JOLTEON, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 1, .hpSet = 1 } },
      .enemy = { MON(SPECIES_RAICHU, 50, MOVE_THUNDERBOLT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckVoltAbsorbHeal },
    { .name = "volt_absorb_full_hp_useless",
      .player = { MON(SPECIES_JOLTEON, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RAICHU, 50, MOVE_THUNDERBOLT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckVoltAbsorbFull },
    { .name = "volt_absorb_vs_thunder_wave",
      .player = { MON(SPECIES_JOLTEON, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RAICHU, 50, MOVE_THUNDER_WAVE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckVoltAbsorbThunderWave },
    { .name = "water_absorb_heals_quarter",
      .player = { { .species = SPECIES_VAPOREON, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 1, .hpSet = 1 } },
      .enemy = { MON(SPECIES_STARMIE, 50, MOVE_SURF, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckWaterAbsorb },
    { .name = "lightning_rod_no_effect_in_singles",
      .player = { MON(SPECIES_RAICHU, 50, MOVE_THUNDERBOLT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_AB(SPECIES_MANECTRIC, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantHit, .check = CheckLightningRodSingles },
    { .name = "levitate_vs_earthquake",
      .player = { MON(SPECIES_DUGTRIO, 50, MOVE_EARTHQUAKE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_GENGAR, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckLevitate },

    // --- contact abilities ---
    { .name = "static_paralyzes_contact_attacker",
      .player = { MON_AB(SPECIES_SNORLAX, 50, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .enemy = { MON(SPECIES_RAICHU, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantPlayerParalyzed, .check = CheckStatic },
    { .name = "static_ignores_non_contact",
      .player = { MON_AB(SPECIES_SNORLAX, 50, MOVE_WATER_GUN, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .enemy = { MON(SPECIES_RAICHU, 100, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T6(0, 0) }, .turns = 6, .check = CheckStaticNonContact },
    { .name = "static_vs_already_statused",
      .player = { { .species = SPECIES_SNORLAX, .level = 50, .moves = { MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .abilityNum = 1, .status = STATUS1_BURN } },
      .enemy = { MON(SPECIES_RAICHU, 100, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T6(0, 0) }, .turns = 6, .check = CheckStaticAlreadyStatused },
    { .name = "static_vs_limber",
      .player = { MON(SPECIES_PERSIAN, 20, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RAICHU, 100, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T6(0, 0) }, .turns = 6, .check = CheckStaticVsLimber },
    { .name = "poison_point_poisons_attacker",
      .player = { MON_AB(SPECIES_SNORLAX, 50, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .enemy = { MON(SPECIES_NIDORINO, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantPlayerPoisoned, .check = CheckPoisonPoint },
    { .name = "poison_point_vs_poison_type",
      .player = { MON(SPECIES_ARBOK, 50, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_NIDORINO, 100, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T6(0, 0) }, .turns = 6, .check = CheckPoisonPointVsPoisonType },
    { .name = "poison_point_vs_immunity",
      .player = { MON(SPECIES_ZANGOOSE, 50, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_NIDORINO, 100, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T6(0, 0) }, .turns = 6, .check = CheckPoisonPointVsImmunity },
    { .name = "flame_body_burns_attacker",
      .player = { MON_AB(SPECIES_SNORLAX, 50, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .enemy = { MON(SPECIES_MAGMAR, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantPlayerBurned, .check = CheckFlameBody },
    { .name = "flame_body_vs_fire_type",
      .player = { MON(SPECIES_RAPIDASH, 50, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_MAGMAR, 100, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T6(0, 0) }, .turns = 6, .check = CheckFlameBodyVsFireType },
    { .name = "effect_spore",
      .player = { MON_AB(SPECIES_SNORLAX, 50, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .enemy = { MON(SPECIES_PARASECT, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantPlayerAnyStatus, .check = CheckEffectSpore },
    { .name = "cute_charm_infatuates",
      .player = { MON(SPECIES_HITMONCHAN, 50, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },     // always male
      .enemy = { MON(SPECIES_WIGGLYTUFF, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantPlayerInfatuated, .check = CheckCuteCharm },
    { .name = "cute_charm_vs_oblivious",
      .player = { MON(SPECIES_SLOWBRO, 50, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },        // Oblivious (male with this personality)
      .enemy = { MON(SPECIES_WIGGLYTUFF, 100, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T6(0, 0) }, .turns = 6, .check = CheckCuteCharmVsOblivious },
    { .name = "cute_charm_vs_genderless",
      .player = { MON(SPECIES_MAGNETON, 50, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_WIGGLYTUFF, 100, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T6(0, 0) }, .turns = 6, .check = CheckCuteCharmVsGenderless },
    { .name = "rough_skin_contact",
      .player = { MON_AB(SPECIES_SNORLAX, 50, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .enemy = { MON(SPECIES_SHARPEDO, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckRoughSkin },
    { .name = "rough_skin_non_contact",
      .player = { MON_AB(SPECIES_SNORLAX, 50, MOVE_WATER_GUN, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .enemy = { MON(SPECIES_SHARPEDO, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckRoughSkinNonContact },

    // --- Color Change ---
    { .name = "color_change",
      .player = { MON(SPECIES_SQUIRTLE, 50, MOVE_GROWL, MOVE_WATER_GUN, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_KECLEON, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0), T(1, 0) }, .turns = 3, .check = CheckColorChange },
    { .name = "color_change_fixed_damage",
      .player = { MON(SPECIES_MACHOP, 50, MOVE_SEISMIC_TOSS, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_KECLEON, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckColorChangeSeismicToss },
    { .name = "color_change_not_struggle",
      .player = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_WATER_GUN, MOVE_NONE, MOVE_NONE, MOVE_NONE }, .pp = { 0, 0, 0, 0 }, .ppSet = 1 } },
      .enemy = { MON(SPECIES_KECLEON, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckColorChangeStruggle },

    // --- secondary effect abilities ---
    { .name = "shield_dust_blocks_paralysis",
      .player = { MON(SPECIES_PIKACHU, 30, MOVE_THUNDER_SHOCK, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_VENOMOTH, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T6(0, 0) }, .turns = 6, .check = CheckShieldDustParalysis },
    { .name = "shield_dust_blocks_stat_drops",
      .player = { MON_AB(SPECIES_SNORLAX, 50, MOVE_ROCK_TOMB, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .enemy = { MON(SPECIES_VENOMOTH, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantHit, .check = CheckShieldDustStatDrop },
    { .name = "shield_dust_not_vs_thief",
      .player = { MON(SPECIES_PERSIAN, 50, MOVE_THIEF, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_ITEM(SPECIES_VENOMOTH, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_LEFTOVERS) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckShieldDustNotThief },
    { .name = "shield_dust_not_vs_static",
      .player = { MON(SPECIES_VENOMOTH, 50, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RAICHU, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantPlayerParalyzed, .check = CheckShieldDustNotStatic },
    { .name = "shield_dust_blocks_kings_rock",
      .player = { MON_ITEM(SPECIES_PERSIAN, 35, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_KINGS_ROCK) },
      .enemy = { MON(SPECIES_DUSTOX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T6(0, 0) }, .turns = 6, .check = CheckShieldDustKingsRock },
    { .name = "serene_grace_sacred_fire_always_burns",
      .player = { MON(SPECIES_DUNSPARCE, 50, MOVE_SACRED_FIRE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_TF(50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantHit, .check = CheckSereneGraceSacredFire },
    { .name = "inner_focus_vs_fake_out",
      .player = { MON(SPECIES_PERSIAN, 50, MOVE_FAKE_OUT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_GOLBAT, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckInnerFocusFakeOut },
    { .name = "own_tempo_vs_confuse_ray",
      .player = { MON(SPECIES_GENGAR, 50, MOVE_CONFUSE_RAY, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SPINDA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckOwnTempoConfuseRay },
    { .name = "own_tempo_vs_swagger",
      .player = { MON(SPECIES_GENGAR, 50, MOVE_SWAGGER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SPINDA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantHit, .check = CheckOwnTempoSwagger },

    // --- status immunities ---
    { .name = "immunity_vs_toxic",
      .player = { MON(SPECIES_BULBASAUR, 50, MOVE_TOXIC, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_ZANGOOSE, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckImmunityToxic },
    { .name = "immunity_vs_secondary_poison_silent",
      .player = { MON(SPECIES_NIDOKING, 50, MOVE_POISON_STING, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_ZANGOOSE, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T6(0, 0) }, .turns = 6, .check = CheckImmunitySecondarySilent },
    { .name = "limber_vs_thunder_wave",
      .player = { MON(SPECIES_PIKACHU, 50, MOVE_THUNDER_WAVE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_PERSIAN, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckLimberThunderWave },
    { .name = "insomnia_vs_spore",
      .player = { MON(SPECIES_PARASECT, 50, MOVE_SPORE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_HYPNO, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckInsomnia },
    { .name = "vital_spirit_vs_spore",
      .player = { MON(SPECIES_PARASECT, 50, MOVE_SPORE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_PRIMEAPE, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckVitalSpirit },
    { .name = "water_veil_vs_will_o_wisp",
      .player = { MON(SPECIES_NINETALES, 50, MOVE_WILL_O_WISP, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_WAILMER, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckWaterVeil },
    { .name = "magma_armor_cures_freeze_at_move_end",
      .player = { MON(SPECIES_PERSIAN, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { { .species = SPECIES_CAMERUPT, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .status = STATUS1_FREEZE } },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckMagmaArmorCure },
    { .name = "skill_swap_insomnia_wakes_target",
      .player = { MON(SPECIES_HYPNO, 50, MOVE_SKILL_SWAP, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { { .species = SPECIES_SNORLAX, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .abilityNum = 1, .status = STATUS1_SLEEP_TURN(4) } },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSkillSwapInsomnia },
    { .name = "skill_swap_vs_wonder_guard_fails",
      .player = { MON(SPECIES_HYPNO, 50, MOVE_SKILL_SWAP, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SHEDINJA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSkillSwapWonderGuard },
    { .name = "role_play_vs_wonder_guard_fails",
      .player = { MON(SPECIES_HYPNO, 50, MOVE_ROLE_PLAY, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SHEDINJA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckRolePlayWonderGuard },

    // --- Synchronize ---
    { .name = "synchronize_toxic_gives_regular_poison",
      .player = { MON(SPECIES_ESPEON, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_AB(SPECIES_SNORLAX, 50, MOVE_TOXIC, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantPlayerToxic, .check = CheckSynchronizeToxic },
    { .name = "synchronize_thunder_wave",
      .player = { MON(SPECIES_ESPEON, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RAICHU, 50, MOVE_THUNDER_WAVE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSynchronizeParalysis },
    { .name = "synchronize_not_sleep",
      .player = { MON(SPECIES_ESPEON, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_PARASECT, 50, MOVE_SPORE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSynchronizeSleep },
    { .name = "synchronize_vs_poison_type_attacker",
      .player = { MON(SPECIES_ESPEON, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_MUK, 50, MOVE_TOXIC, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantPlayerToxic, .check = CheckSynchronizeVsPoisonType },
    { .name = "synchronize_secondary_paralysis",
      .player = { MON(SPECIES_ESPEON, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RAICHU, 50, MOVE_THUNDERBOLT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantPlayerParalyzed, .check = CheckSynchronizeSecondary },

    // --- sleep / end of turn ---
    { .name = "early_bird_halves_sleep",
      .player = { { .species = SPECIES_KANGASKHAN, .level = 50, .moves = { MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .status = STATUS1_SLEEP_TURN(3) } },
      .enemy = { SNORLAX_IM(100) },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .check = CheckEarlyBird },
    { .name = "normal_sleep_counter",
      .player = { { .species = SPECIES_TAUROS, .level = 50, .moves = { MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .status = STATUS1_SLEEP_TURN(3) } },
      .enemy = { SNORLAX_IM(100) },
      .actions = { T(0, 0), T(0, 0), T(0, 0) }, .turns = 3, .check = CheckNormalSleep },
    { .name = "shed_skin_cures",
      .player = { MON(SPECIES_PERSIAN, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { { .species = SPECIES_ARBOK, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .abilityNum = 1, .status = STATUS1_POISON } },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantEnemyNoStatus, .check = CheckShedSkin },

    // --- Soundproof ---
    { .name = "soundproof_vs_growl",
      .player = { MON(SPECIES_RATTATA, 50, MOVE_GROWL, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_WHISMUR, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSoundproofGrowl },
    { .name = "soundproof_vs_hyper_voice",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_HYPER_VOICE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_WHISMUR, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSoundproofHyperVoice },
    { .name = "soundproof_vs_perish_song",
      .player = { MON(SPECIES_MISDREAVUS, 50, MOVE_PERISH_SONG, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_EXPLOUD, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSoundproofPerishSong },
    { .name = "soundproof_user_heal_bell",
      .player = { { .species = SPECIES_WHISMUR, .level = 50, .moves = { MOVE_HEAL_BELL, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .status = STATUS1_BURN },
                  { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .status = STATUS1_POISON } },
      .enemy = { SNORLAX_IM(50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSoundproofHealBell },

    // --- items / drain / recoil ---
    { .name = "sticky_hold_vs_thief",
      .player = { MON(SPECIES_PERSIAN, 50, MOVE_THIEF, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { { .species = SPECIES_MUK, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .abilityNum = 1, .item = ITEM_LEFTOVERS } },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckStickyHoldThief },
    { .name = "sticky_hold_vs_trick",
      .player = { MON_ITEM(SPECIES_ALAKAZAM, 50, MOVE_TRICK, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_LEFTOVERS) },
      .enemy = { { .species = SPECIES_MUK, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .abilityNum = 1, .item = ITEM_ORAN_BERRY } },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckStickyHoldTrick },
    { .name = "liquid_ooze_vs_giga_drain",
      .player = { MON(SPECIES_VENUSAUR, 50, MOVE_GIGA_DRAIN, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SWALOT, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckLiquidOozeDrain },
    { .name = "liquid_ooze_vs_leech_seed",
      .player = { MON(SPECIES_VENUSAUR, 50, MOVE_LEECH_SEED, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SWALOT, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantEnemySeeded, .check = CheckLiquidOozeLeechSeed },
    { .name = "rock_head_no_recoil",
      .player = { MON(SPECIES_AERODACTYL, 50, MOVE_DOUBLE_EDGE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_IM(100) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckRockHead },
    { .name = "rock_head_struggle_recoils",
      .player = { { .species = SPECIES_AERODACTYL, .level = 50, .moves = { MOVE_DOUBLE_EDGE, MOVE_NONE, MOVE_NONE, MOVE_NONE }, .pp = { 0, 0, 0, 0 }, .ppSet = 1 } },
      .enemy = { SNORLAX_IM(100) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckRockHeadStruggle },

    // --- crits / accuracy / OHKO / Wonder Guard ---
    { .name = "shell_armor_blocks_crits",
      .player = { MON(SPECIES_PERSIAN, 30, MOVE_FOCUS_ENERGY, MOVE_SLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_CLOYSTER, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0), T(1, 0), T(1, 0), T(1, 0), T(1, 0) }, .turns = 6, .check = CheckShellArmor },
    { .name = "battle_armor_blocks_crits",
      .player = { MON(SPECIES_PERSIAN, 30, MOVE_FOCUS_ENERGY, MOVE_SLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_AB(SPECIES_KABUTOPS, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .actions = { T(0, 0), T(1, 0), T(1, 0), T(1, 0), T(1, 0), T(1, 0) }, .turns = 6, .check = CheckBattleArmor },
    { .name = "sand_veil_in_sandstorm",
      .player = { MON_AB(SPECIES_SNORLAX, 50, MOVE_POUND, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .enemy = { MON(SPECIES_SANDSLASH, 100, MOVE_SANDSTORM, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 1), T(0, 1), T(0, 1), T(0, 1), T(0, 1), T(0, 1), T(0, 1) }, .turns = 8, .wantSeed = WantMiss, .check = CheckSandVeil },
    { .name = "sturdy_vs_ohko",
      .player = { MON(SPECIES_DUGTRIO, 60, MOVE_FISSURE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_AB(SPECIES_GOLEM, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, 1) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSturdy },
    { .name = "wonder_guard_super_effective_hits",
      .player = { MON(SPECIES_CHARMANDER, 50, MOVE_EMBER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SHEDINJA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckWonderGuardSuperEffective },
    { .name = "wonder_guard_status_move_works",
      .player = { MON(SPECIES_BULBASAUR, 50, MOVE_TOXIC, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SHEDINJA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantBadlyPoisonedMsg, .check = CheckWonderGuardStatusMove },
    { .name = "wonder_guard_struggle_hits",
      .player = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_TACKLE, MOVE_NONE, MOVE_NONE, MOVE_NONE }, .pp = { 0, 0, 0, 0 }, .ppSet = 1 } },
      .enemy = { MON(SPECIES_SHEDINJA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckWonderGuardStruggle },
    { .name = "wonder_guard_night_shade_hits",
      .player = { MON(SPECIES_GENGAR, 50, MOVE_NIGHT_SHADE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SHEDINJA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckWonderGuardNightShade },

    // --- Truant / Swift Swim ---
    { .name = "truant_loafs_every_other_turn",
      .player = { MON(SPECIES_SLAKING, 50, MOVE_POUND, MOVE_FOCUS_PUNCH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { SNORLAX_IM(100) },
      .actions = { T(0, 0), T(1, 0), T(0, 0) }, .turns = 3, .check = CheckTruantFocusPunch },
    { .name = "swift_swim",
      .player = { MON(SPECIES_KINGDRA, 50, MOVE_RAIN_DANCE, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_JOLTEON, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckSwiftSwim },
};

SCENARIO_GROUP(abilities_combat, sScenarios)
