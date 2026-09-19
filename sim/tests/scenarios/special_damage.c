// Damage formula and special damage mechanics: badge boosts, items/abilities in CalculateBaseDamage, burn,
// screens, weather, crits, type chart (STAB, 4x/0.25x, immunities, Foresight, Wonder Guard, typeless Struggle
// and Future Sight), fixed-damage moves, HP/friendship/weight/IV based power, Stockpile, Beat Up, multi-hit,
// recoil, drain, Pain Split, Endeavor, OHKO, Endure/Focus Band/False Swipe and a few doubles-only modifiers.
#include "scenario.h"

// ---- expected-damage helpers (mirror CalculateBaseDamage / typecalc / ApplyRandomDmgMultiplier) ----
static const u8 sRatio[13][2] = { {10,40},{10,35},{10,30},{10,25},{10,20},{10,15},{10,10},{15,10},{20,10},{25,10},{30,10},{35,10},{40,10} };
static int Mod(int stat, int stage) { return stat * sRatio[stage][0] / sRatio[stage][1]; }
static int Badge(int stat) { return 110 * stat / 100; }   // player's side badge boost (attack/defense/special)
static int Core(int atk, int def, int power, int level) { s32 d = atk * power; d *= (2 * level / 5 + 2); d /= def; d /= 50; return d; }
static int Phys(int atk, int def, int power, int level) { int d = Core(atk, def, power, level); if (d == 0) d = 1; return d + 2; }
static int PhysHalf(int atk, int def, int power, int level) { int d = Core(atk, def, power, level) / 2; if (d == 0) d = 1; return d + 2; } // burn, Reflect, spread
static int Spec(int atk, int def, int power, int level) { return Core(atk, def, power, level) + 2; }
static int SpecScaled(int atk, int def, int power, int level, int num, int den) { int d = Core(atk, def, power, level); d = num * d / den; return d + 2; } // screen/weather/flash fire
static int Stab(int d) { return d * 15 / 10; }
static int Mul(int d, int m) { d = d * m / 10; if (d == 0 && m) d = 1; return d; }
static int Roll(int d, int pct) { if (d == 0) return 0; d = d * pct / 100; return d ? d : 1; }
static int InRoll(int dmg, int full) { return dmg >= Roll(full, 85) && dmg <= full; }
static int Dealt(struct BattleSim *sim, int b) { return MAXHP(b) - HP(b); }
static int FlailPower(int hp, int max) { int f = hp * 48 / max; if (f == 0 && hp > 0) f = 1; return f <= 1 ? 200 : f <= 4 ? 150 : f <= 9 ? 100 : f <= 16 ? 80 : f <= 32 ? 40 : 20; }
#define NO_CRIT (!LOG_HAS(STRINGID_CRITICALHIT))
#define PATK(b) Badge(B(b).attack)
#define PSPA(b) Badge(B(b).spAttack)
#define PDEF(b) Badge(B(b).defense)
#define PSPD(b) Badge(B(b).spDefense)
#define LV(b) (B(b).level)
// One EMPTYSTRING3 per landed hit of a multi-hit move; BattleScript_FaintTarget/FaintAttacker print one too, so faints are subtracted.
#define HITS() (LOG_COUNT(STRINGID_EMPTYSTRING3) - LOG_COUNT(STRINGID_TARGETFAINTED) - LOG_COUNT(STRINGID_ATTACKERFAINTED))
static int PoisonChip(struct BattleSim *sim, int b) { int c = MAXHP(b) / 8; return c ? c : 1; }   // end-of-turn poison damage, paid after the move

// EXACT(name, target, full): pins the 100% damage roll with wantSeed and checks the damage exactly.
#define EXACT(name, target, full) \
    static int Full_##name(struct BattleSim *sim) { (void)sim; return (full); } \
    static int Want_##name(struct BattleSim *sim) { return NO_CRIT && Dealt(sim, target) == Full_##name(sim); } \
    static void Check_##name(struct BattleSim *sim) { int e = Full_##name(sim); CHECK(NO_CRIT, "no crit"); CHECK(Dealt(sim, target) == e, "expected %d damage, dealt %d (hp %d/%d)", e, Dealt(sim, target), HP(target), MAXHP(target)); }
// EXACT_CRIT: the same, but the hit must be a critical hit.
#define EXACT_CRIT(name, target, full) \
    static int Full_##name(struct BattleSim *sim) { (void)sim; return (full); } \
    static int Want_##name(struct BattleSim *sim) { return LOG_HAS(STRINGID_CRITICALHIT) && Dealt(sim, target) == Full_##name(sim); } \
    static void Check_##name(struct BattleSim *sim) { int e = Full_##name(sim); CHECK(LOG_HAS(STRINGID_CRITICALHIT), "critical hit"); CHECK(Dealt(sim, target) == e, "expected %d crit damage, dealt %d (hp %d/%d)", e, Dealt(sim, target), HP(target), MAXHP(target)); }

#define M1(sp, lv, m) MON(sp, lv, m, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH)
#define M2(sp, lv, m1, m2) MON(sp, lv, m1, m2, MOVE_SPLASH, MOVE_SPLASH)
#define SPLASHER(sp, lv) M1(sp, lv, MOVE_SPLASH)
#define MI1(sp, lv, m, it) MON_ITEM(sp, lv, m, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, it)
#define MA1(sp, lv, m, ab) MON_AB(sp, lv, m, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ab)

// ================= base formula =================
// Machop Strength (80, no STAB) vs Snorlax: ((atk*80*22)/def)/50 + 2 with the attack badge boost.
EXACT(phys, 1, Phys(PATK(0), B(1).defense, 80, LV(0)))
// Squirtle Ice Beam (95, special, no STAB) vs Snorlax with the special badge boost.
EXACT(spec, 1, Spec(PSPA(0), B(1).spDefense, 95, LV(0)))
// Enemy attacker gets no badge boost; the player's defender gets the defense badge boost.
EXACT(enemy_phys, 0, Phys(B(1).attack, PDEF(0), 80, LV(1)))
EXACT(enemy_spec, 0, Spec(B(1).spAttack, PSPD(0), 95, LV(1)))
static void CheckRollRange(struct BattleSim *sim)
{
    int full = Phys(PATK(0), B(1).defense, 80, LV(0)), d = Dealt(sim, 1);
    if (LOG_HAS(STRINGID_CRITICALHIT)) full *= 2;
    CHECK(InRoll(d, full), "damage %d outside [%d,%d]", d, Roll(full, 85), full);
}
EXACT(stab, 1, Stab(Phys(PATK(0), B(1).defense, 40, LV(0))))
EXACT(burn_phys, 1, Stab(PhysHalf(PATK(0), B(1).defense, 40, LV(0))))
EXACT(burn_spec, 1, Stab(Spec(PSPA(0), B(1).spDefense, 40, LV(0))))
EXACT(guts, 1, Phys(150 * PATK(0) / 100, B(1).defense, 80, LV(0)))
EXACT(reflect, 1, PhysHalf(PATK(0), B(1).defense, 80, LV(0)))
EXACT(light_screen, 1, SpecScaled(PSPA(0), B(1).spDefense, 95, LV(0), 1, 2))
EXACT_CRIT(crit_reflect, 1, 2 * Phys(PATK(0), B(1).defense, 80, LV(0)))
EXACT_CRIT(crit_double, 1, Stab(2 * Phys(PATK(0), B(1).defense, 40, LV(0))))
EXACT_CRIT(crit_atk_drop, 1, 2 * Phys(PATK(0), B(1).defense, 80, LV(0)))
static void CheckCritAtkDrop(struct BattleSim *sim) { Check_crit_atk_drop(sim); CHECK(STAGE(0, STAT_ATK) == 4, "attack at -2 (stage %d)", STAGE(0, STAT_ATK)); }
EXACT_CRIT(crit_def_boost, 1, 2 * Phys(PATK(0), B(1).defense, 80, LV(0)))
static void CheckCritDefBoost(struct BattleSim *sim) { Check_crit_def_boost(sim); CHECK(STAGE(1, STAT_DEF) == 7, "defense at +1 (stage %d)", STAGE(1, STAT_DEF)); }
EXACT_CRIT(crit_atk_boost, 1, 2 * Phys(Mod(PATK(0), 8), B(1).defense, 80, LV(0)))
EXACT(atk_plus2, 1, Phys(Mod(PATK(0), 8), B(1).defense, 80, LV(0)))
static int Full_def_minus2(struct BattleSim *sim) { return Stab(Phys(PATK(0), Mod(B(1).defense, 4), 40, LV(0))); }
static int Want_def_minus2(struct BattleSim *sim) { return STAGE(1, STAT_DEF) == 4 && NO_CRIT && Dealt(sim, 1) == Full_def_minus2(sim); }
static void Check_def_minus2(struct BattleSim *sim)
{
    CHECK(STAGE(1, STAT_DEF) == 4, "screech landed (stage %d)", STAGE(1, STAT_DEF));
    CHECK(Dealt(sim, 1) == Full_def_minus2(sim), "expected %d, dealt %d", Full_def_minus2(sim), Dealt(sim, 1));
}
EXACT(huge_power, 1, Phys(Badge(2 * B(0).attack), B(1).defense, 80, LV(0)))
EXACT(choice_band, 1, Phys(150 * PATK(0) / 100, B(1).defense, 80, LV(0)))
EXACT(silk_scarf, 1, Stab(Phys(PATK(0) * 110 / 100, B(1).defense, 40, LV(0))))
EXACT(charcoal, 1, Stab(Spec(PSPA(0) * 110 / 100, B(1).spDefense, 40, LV(0))))
EXACT(thick_fat, 1, Stab(Spec(PSPA(0) / 2, B(1).spDefense, 40, LV(0))))
EXACT(hustle, 1, Stab(Phys(150 * PATK(0) / 100, B(1).defense, 80, LV(0))))
EXACT(thick_club, 1, Stab(Phys(2 * PATK(0), B(1).defense, 100, LV(0))))
EXACT(light_ball, 1, Stab(Spec(2 * PSPA(0), B(1).spDefense, 95, LV(0))))
EXACT(metal_powder, 1, Stab(Phys(PATK(0), 2 * B(1).defense, 40, LV(0))))
EXACT(marvel_scale, 1, Phys(PATK(0), 150 * B(1).defense / 100, 80, LV(0)))
// Explosion halves the target's defense (before stat stages); the user drops to 0 HP first.
EXACT(explosion, 1, Phys(PATK(0), B(1).defense / 2, 250, LV(0)))
static void CheckExplosion(struct BattleSim *sim) { Check_explosion(sim); CHECK(HP(0) == 0, "user fainted"); }
static void CheckSelfDestructGhost(struct BattleSim *sim)
{
    CHECK(HP(0) == 0, "user still faints against a ghost (hp %d)", HP(0));
    CHECK(HP(1) == MAXHP(1), "ghost untouched");
    CHECK(LOG_HAS(STRINGID_ITDOESNTAFFECT), "doesn't affect message");
}
// Caterpie L2 Tackle vs Steelix L100: physical minimum 1 -> 3 after +2, Steel resists -> 1, roll keeps 1.
static int WantHit1(struct BattleSim *sim) { return Dealt(sim, 1) > 0; }
static void CheckMinDamage(struct BattleSim *sim) { CHECK(Dealt(sim, 1) == 1, "minimum damage 1 (dealt %d)", Dealt(sim, 1)); }
EXACT(rain_water, 1, Stab(SpecScaled(PSPA(0), B(1).spDefense, 40, LV(0), 15, 10)))
EXACT(rain_fire, 1, Stab(SpecScaled(PSPA(0), B(1).spDefense, 40, LV(0), 1, 2)))
EXACT(sun_fire, 1, Stab(SpecScaled(PSPA(0), B(1).spDefense, 40, LV(0), 15, 10)))
static void CheckRainWater(struct BattleSim *sim) { CHECK(WEATHER() & B_WEATHER_RAIN, "raining"); Check_rain_water(sim); }
static void CheckSunFire(struct BattleSim *sim) { CHECK(WEATHER() & B_WEATHER_SUN, "sunny"); Check_sun_fire(sim); }
// Overgrow at hp <= maxHP/3: Vine Whip 35 -> 52.
EXACT(overgrow, 1, Stab(Spec(PSPA(0), B(1).spDefense, 52, LV(0))))
// Flash Fire: Growlithe absorbs Ponyta's Ember, then its own Ember is 1.5x (Fire vs Fire is 0.5x).
static int Full_flash_fire(struct BattleSim *sim) { return Mul(Stab(SpecScaled(PSPA(0), B(1).spDefense, 40, LV(0), 15, 10)), 5); }
static int Want_flash_fire(struct BattleSim *sim) { return NO_CRIT && HP(0) == MAXHP(0) && Dealt(sim, 1) == Full_flash_fire(sim); }
static void Check_flash_fire(struct BattleSim *sim)
{
    CHECK(HP(0) == MAXHP(0), "ember absorbed by flash fire");
    CHECK(Dealt(sim, 1) == Full_flash_fire(sim), "expected %d, dealt %d", Full_flash_fire(sim), Dealt(sim, 1));
}

// ================= type chart =================
EXACT(se2, 1, Mul(Stab(Spec(PSPA(0), B(1).spDefense, 40, LV(0))), 20))
static void CheckSe2(struct BattleSim *sim) { Check_se2(sim); CHECK(LOG_HAS(STRINGID_SUPEREFFECTIVE), "super effective message"); }
EXACT(nve, 1, Mul(Stab(Phys(PATK(0), B(1).defense, 40, LV(0))), 5))
static void CheckNve(struct BattleSim *sim) { Check_nve(sim); CHECK(LOG_HAS(STRINGID_NOTVERYEFFECTIVE), "not very effective message"); }
EXACT(se4, 1, Mul(Mul(Stab(Spec(PSPA(0), B(1).spDefense, 95, LV(0))), 20), 20))
EXACT(nve4, 1, Mul(Mul(Stab(Phys(PATK(0), B(1).defense, 75, LV(0))), 5), 5))
static void CheckImmune(struct BattleSim *sim)
{
    CHECK(HP(1) == MAXHP(1), "no damage (hp %d/%d)", HP(1), MAXHP(1));
    CHECK(LOG_HAS(STRINGID_ITDOESNTAFFECT), "doesn't affect message");
}
static void CheckLevitate(struct BattleSim *sim)
{
    CHECK(HP(1) == MAXHP(1), "no damage");
    CHECK(LOG_HAS(STRINGID_PKMNMAKESGROUNDMISS), "levitate message");
}
// Foresight: the Normal->Ghost immunity rows follow the FORESIGHT row and are skipped; Ghost/Poison takes neutral Normal damage.
EXACT(foresight, 1, Stab(Phys(PATK(0), B(1).defense, 40, LV(0))))
static void CheckForesight(struct BattleSim *sim) { CHECK(STATUS2(1) & STATUS2_FORESIGHT, "foresighted"); Check_foresight(sim); }
static void CheckForesightSeismicToss(struct BattleSim *sim)
{
    CHECK(STATUS2(1) & STATUS2_FORESIGHT, "foresighted");
    CHECK(Dealt(sim, 1) == 50, "seismic toss hits the ghost for its level (dealt %d)", Dealt(sim, 1));
}
static void CheckWonderGuardSe(struct BattleSim *sim) { CHECK(HP(1) == 0, "super effective ember gets through wonder guard"); }
static void CheckWonderGuardBlocked(struct BattleSim *sim)
{
    CHECK(HP(1) == MAXHP(1), "blocked by wonder guard");
    CHECK(LOG_HAS(STRINGID_AVOIDEDDAMAGE), "avoided damage message");
}
// Struggle skips typecalc entirely: no STAB, no type chart, hits Ghost (and Wonder Guard); recoil is 1/4 of the damage dealt.
static void CheckStruggleWonderGuard(struct BattleSim *sim)
{
    CHECK(LOG_HAS_MOVE(STRINGID_USEDMOVE, MOVE_STRUGGLE), "used struggle");
    CHECK(HP(1) == 0, "struggle KOs shedinja through wonder guard");
    CHECK(HP(0) == MAXHP(0) - 1, "recoil 1/4 of 1 -> minimum 1 (hp %d/%d)", HP(0), MAXHP(0));
}
EXACT(struggle_ghost, 1, Phys(PATK(0), B(1).defense, 50, LV(0)))
static void CheckStruggleGhost(struct BattleSim *sim)
{
    int recoil = Dealt(sim, 1) / 4;
    if (recoil == 0) recoil = 1;
    Check_struggle_ghost(sim);
    CHECK(HP(0) == MAXHP(0) - recoil, "recoil %d (hp %d/%d)", recoil, HP(0), MAXHP(0));
    CHECK(LOG_HAS(STRINGID_PKMNHITWITHRECOIL), "recoil message");
}
// Future Sight: base damage stored when set (no crit, no STAB, no type chart), only the random roll at the end of turn 3.
static int WantFutureSightHit(struct BattleSim *sim) { return Dealt(sim, 1) > 0; }
static void CheckFutureSight(struct BattleSim *sim)
{
    int full = Spec(PSPA(0), B(1).spDefense, 80, LV(0));
    CHECK(LOG_HAS_T(STRINGID_PKMNTOOKATTACK, 2), "future sight lands at the end of the 3rd turn");
    CHECK(InRoll(Dealt(sim, 1), full), "typeless, un-STABed damage %d in [%d,%d] against a Dark type", Dealt(sim, 1), Roll(full, 85), full);
    CHECK(HP(1) == MAXHP(1) - Dealt(sim, 1), "hp consistent");
}

// ================= fixed damage =================
static void CheckSeismicToss(struct BattleSim *sim) { CHECK(Dealt(sim, 1) == 37, "seismic toss = level 37 (dealt %d)", Dealt(sim, 1)); }
static void CheckNightShade(struct BattleSim *sim) { CHECK(Dealt(sim, 1) == 43, "night shade = level 43 (dealt %d)", Dealt(sim, 1)); }
static void CheckDragonRageSteel(struct BattleSim *sim)
{
    CHECK(Dealt(sim, 1) == 40, "dragon rage = 40 even against steel (dealt %d)", Dealt(sim, 1));
    CHECK(!LOG_HAS(STRINGID_NOTVERYEFFECTIVE), "effectiveness flags cleared for fixed damage");
}
static int WantDealt20(struct BattleSim *sim) { return Dealt(sim, 1) == 20; }
static void CheckSonicBoom(struct BattleSim *sim) { CHECK(Dealt(sim, 1) == 20, "sonic boom = 20 (dealt %d)", Dealt(sim, 1)); }
static int WantSuperFang(struct BattleSim *sim) { return Dealt(sim, 1) == MAXHP(1) / 2; }
static void CheckSuperFang(struct BattleSim *sim) { CHECK(Dealt(sim, 1) == MAXHP(1) / 2, "half of %d (dealt %d)", MAXHP(1), Dealt(sim, 1)); }
static int WantKo(struct BattleSim *sim) { return HP(1) == 0; }
static void CheckSuperFang1Hp(struct BattleSim *sim) { CHECK(HP(1) == 0, "super fang on 1 hp does the minimum 1 and KOs"); }
static void CheckPsywave(struct BattleSim *sim)
{
    int r, d = Dealt(sim, 1), ok = 0;
    for (r = 0; r <= 10; r++)
        if (d == LV(0) * (r * 10 + 50) / 100)
            ok = 1;
    CHECK(d > 0 && ok, "psywave damage %d is level*(50..150)/100 for level %d", d, LV(0));
}
static void CheckEndeavor(struct BattleSim *sim) { CHECK(HP(1) == 10, "target brought down to the user's 10 hp (hp %d)", HP(1)); CHECK(HP(0) == 10, "user unchanged"); }
static void CheckEndeavorFails(struct BattleSim *sim) { CHECK(LOG_HAS(STRINGID_BUTITFAILED), "fails when the target has less hp"); CHECK(HP(1) == MAXHP(1), "no damage"); }

// ================= variable power =================
static int FlailFull(struct BattleSim *sim) { return Stab(Phys(PATK(0), B(1).defense, FlailPower(HP(0), MAXHP(0)), LV(0))); }
static int WantFlail(struct BattleSim *sim) { return NO_CRIT && Dealt(sim, 1) == FlailFull(sim); }
static void CheckFlail1Hp(struct BattleSim *sim)
{
    CHECK(FlailPower(HP(0), MAXHP(0)) == 200, "1 hp -> power 200");
    CHECK(Dealt(sim, 1) == FlailFull(sim), "expected %d, dealt %d", FlailFull(sim), Dealt(sim, 1));
}
static void CheckFlailFull(struct BattleSim *sim)
{
    CHECK(FlailPower(HP(0), MAXHP(0)) == 20, "full hp -> power 20");
    CHECK(Dealt(sim, 1) == FlailFull(sim), "expected %d, dealt %d", FlailFull(sim), Dealt(sim, 1));
}
static int ReversalFull(struct BattleSim *sim) { return Mul(Stab(Phys(PATK(0), B(1).defense, FlailPower(HP(0), MAXHP(0)), LV(0))), 20); }
static int WantReversal(struct BattleSim *sim) { return NO_CRIT && Dealt(sim, 1) == ReversalFull(sim); }
static void CheckReversalMid(struct BattleSim *sim)
{
    CHECK(HP(0) * 48 / MAXHP(0) > 4 && HP(0) * 48 / MAXHP(0) <= 9, "24/%d hp lies in the 100-power bracket", MAXHP(0));
    CHECK(FlailPower(HP(0), MAXHP(0)) == 100, "power 100");
    CHECK(Dealt(sim, 1) == ReversalFull(sim), "expected %d, dealt %d", ReversalFull(sim), Dealt(sim, 1));
}
EXACT(return70, 1, Stab(Phys(PATK(0), B(1).defense, 10 * B(0).friendship / 25, LV(0))))
static void CheckReturn70(struct BattleSim *sim) { CHECK(B(0).friendship == 70, "base friendship 70 (got %d)", B(0).friendship); Check_return70(sim); }
EXACT(frustration70, 1, Stab(Phys(PATK(0), B(1).defense, 10 * (255 - B(0).friendship) / 25, LV(0))))
static void CheckFrustration70(struct BattleSim *sim) { CHECK(10 * (255 - B(0).friendship) / 25 == 74, "power 74"); Check_frustration70(sim); }
EXACT(return140, 1, Stab(Phys(PATK(0), B(1).defense, 56, LV(0))))
static void CheckReturn140(struct BattleSim *sim) { CHECK(B(0).friendship == 140, "clefable base friendship 140 (got %d)", B(0).friendship); Check_return140(sim); }
static int WantPresentHeal(struct BattleSim *sim) { return HP(1) == 100 + MAXHP(1) / 4; }
static void CheckPresentHeal(struct BattleSim *sim)
{
    CHECK(HP(1) == 100 + MAXHP(1) / 4, "present healed 1/4 of max hp (hp %d/%d)", HP(1), MAXHP(1));
    CHECK(LOG_HAS(STRINGID_PKMNREGAINEDHEALTH), "regained health message");
}
EXACT(present120, 1, Stab(Phys(PATK(0), B(1).defense, 120, LV(0))))
static int WantMagnitude(struct BattleSim *sim) { return NO_CRIT && Dealt(sim, 1) > 0; }
static void CheckMagnitude(struct BattleSim *sim)
{
    static const int powers[] = { 10, 30, 50, 70, 90, 110, 150 };
    int i, ok = 0, d = Dealt(sim, 1);
    for (i = 0; i < 7; i++)
        if (InRoll(d, Stab(Phys(PATK(0), B(1).defense, powers[i], LV(0)))))
            ok = 1;
    CHECK(LOG_HAS(STRINGID_MAGNITUDESTRENGTH), "magnitude strength message");
    CHECK(ok, "damage %d matches one of the magnitude powers", d);
}
EXACT(low_kick_heavy, 1, Mul(Stab(Phys(PATK(0), B(1).defense, 120, LV(0))), 20))   // Snorlax 460.0 kg -> 120
EXACT(low_kick_light, 1, Mul(Stab(Phys(PATK(0), B(1).defense, 20, LV(0))), 20))    // Rattata 3.5 kg -> 20
EXACT(low_kick_mid, 1, Mul(Mul(Stab(Phys(PATK(0), B(1).defense, 80, LV(0))), 5), 5)) // Golbat 55.0 kg -> 80 (Poison/Flying resist)
// Hidden Power: all IVs 31 -> Dark 70 (special); all IVs 30 -> Fighting 70 (physical); all IVs 0 -> Fighting 30.
EXACT(hp_dark, 1, Mul(Spec(PSPA(0), B(1).spDefense, 70, LV(0)), 20))
EXACT(hp_fighting70, 1, Mul(Phys(PATK(0), B(1).defense, 70, LV(0)), 20))
EXACT(hp_fighting30, 1, Phys(PATK(0), B(1).defense, 30, LV(0)))
EXACT(eruption_full, 1, Stab(Spec(PSPA(0), B(1).spDefense, 150, LV(0))))
// scaledamagebyhealthratio: power = hp*150/maxHP (min 1); at hp <= maxHP/3 Typhlosion's Blaze then multiplies that dynamic power by 1.5 in CalculateBaseDamage.
static int EruptionLowPower(struct BattleSim *sim) { int p = HP(0) * 150 / MAXHP(0); if (p == 0) p = 1; if (HP(0) <= MAXHP(0) / 3) p = 150 * p / 100; return p; }
static int EruptionLowFull(struct BattleSim *sim) { return Stab(Spec(PSPA(0), B(1).spDefense, EruptionLowPower(sim), LV(0))); }
static int WantEruptionLow(struct BattleSim *sim) { return NO_CRIT && Dealt(sim, 1) == EruptionLowFull(sim); }
static void CheckEruptionLow(struct BattleSim *sim)
{
    CHECK(HP(0) == 20 && HP(0) <= MAXHP(0) / 3, "user at 20 hp, inside blaze range (max %d)", MAXHP(0));
    CHECK(Dealt(sim, 1) == EruptionLowFull(sim), "power %d (blaze on %d): expected %d, dealt %d", EruptionLowPower(sim), 20 * 150 / MAXHP(0), EruptionLowFull(sim), Dealt(sim, 1));
}
// Spit Up: base damage * stockpile count, no random roll (deterministic), typecalc still applies (no STAB for Ivysaur).
static void CheckSpitUp2(struct BattleSim *sim)
{
    int e = 2 * Phys(PATK(0), B(1).defense, 100, LV(0));
    CHECK(LOG_COUNT(STRINGID_PKMNSTOCKPILED) == 2, "stockpiled twice");
    CHECK(Dealt(sim, 1) == e, "spit up x2 = %d exactly, dealt %d", e, Dealt(sim, 1));
    CHECK(sim->disableStructs[0].stockpileCounter == 0, "stockpile counter reset");
}
static void CheckStockpileCap(struct BattleSim *sim)
{
    CHECK(sim->disableStructs[0].stockpileCounter == 3, "capped at 3 (got %d)", sim->disableStructs[0].stockpileCounter);
    CHECK(LOG_HAS(STRINGID_PKMNCANTSTOCKPILE), "can't stockpile any more");
}
static void CheckSwallow2(struct BattleSim *sim)
{
    CHECK(HP(0) == 1 + MAXHP(0) / 2, "swallow with 2 stockpiles heals half (hp %d/%d)", HP(0), MAXHP(0));
    CHECK(sim->disableStructs[0].stockpileCounter == 0, "counter reset");
}
static void CheckSpitUpFails(struct BattleSim *sim) { CHECK(LOG_HAS(STRINGID_FAILEDTOSPITUP), "spit up without stockpile fails"); CHECK(HP(1) == MAXHP(1), "no damage"); }
// Beat Up: each healthy party member hits with (baseAtk*10*(lvl*2/5+2)/baseDef)/50+2, random roll, no type chart.
static int BeatUpFull(struct BattleSim *sim, int i)
{
    struct Pokemon *mon = &sim->playerParty[i];
    int d = gSpeciesInfo[GetMonData(mon, MON_DATA_SPECIES)].baseAttack * 10;
    d *= (GetMonData(mon, MON_DATA_LEVEL) * 2 / 5 + 2);
    d /= gSpeciesInfo[B(1).species].baseDefense;
    return d / 50 + 2;
}
static int WantBeatUp(struct BattleSim *sim) { return NO_CRIT && Dealt(sim, 1) > 0; }
static void CheckBeatUp(struct BattleSim *sim)
{
    int lo = Roll(BeatUpFull(sim, 0), 85) + Roll(BeatUpFull(sim, 1), 85), hi = BeatUpFull(sim, 0) + BeatUpFull(sim, 1);
    CHECK(LOG_COUNT(STRINGID_PKMNATTACK) == 2, "two attackers (the paralyzed one is skipped), got %d", LOG_COUNT(STRINGID_PKMNATTACK));
    CHECK(Dealt(sim, 1) >= lo && Dealt(sim, 1) <= hi, "total %d in [%d,%d]", Dealt(sim, 1), lo, hi);
}
static int WantTripleKick(struct BattleSim *sim) { return NO_CRIT && HITS() == 3; }
static void CheckTripleKick(struct BattleSim *sim)
{
    int p, lo = 0, hi = 0;
    for (p = 10; p <= 30; p += 10)
    {
        int f = Mul(Stab(Phys(PATK(0), B(1).defense, p, LV(0))), 20);
        lo += Roll(f, 85);
        hi += f;
    }
    CHECK(HITS() == 3, "three hits");
    CHECK(LOG_HAS(STRINGID_HITXTIMES), "hit x times message");
    CHECK(Dealt(sim, 1) >= lo && Dealt(sim, 1) <= hi, "10+20+30 power: total %d in [%d,%d]", Dealt(sim, 1), lo, hi);
}
static int WantHits5(struct BattleSim *sim) { return NO_CRIT && HITS() == 5; }
static int WantHits2(struct BattleSim *sim) { return NO_CRIT && HITS() == 2; }
static void CheckMultiHit(struct BattleSim *sim, int hits, int full)
{
    CHECK(HITS() == hits, "%d hits (got %d)", hits, HITS());
    CHECK(LOG_HAS(STRINGID_HITXTIMES), "hit x times message");
    CHECK(Dealt(sim, 1) >= hits * Roll(full, 85) && Dealt(sim, 1) <= hits * full, "total %d in [%d,%d]", Dealt(sim, 1), hits * Roll(full, 85), hits * full);
}
static void CheckFuryAttack5(struct BattleSim *sim) { CheckMultiHit(sim, 5, Stab(Phys(PATK(0), B(1).defense, 15, LV(0)))); }
static void CheckFuryAttack2(struct BattleSim *sim) { CheckMultiHit(sim, 2, Stab(Phys(PATK(0), B(1).defense, 15, LV(0)))); }
static int WantHit1Ko(struct BattleSim *sim) { return HP(1) == 0; }
static void CheckMultiHitStopsOnFaint(struct BattleSim *sim)
{
    CHECK(HP(1) == 0, "target fainted");
    CHECK(HITS() == 1, "stopped after the first hit (hits %d)", HITS());
    CHECK(LOG_HAS(STRINGID_HITXTIMES), "hit 1 time message");
}
static void CheckBonemerang(struct BattleSim *sim) { CheckMultiHit(sim, 2, Stab(Phys(PATK(0), B(1).defense, 50, LV(0)))); }
static int WantTwineedle(struct BattleSim *sim) { return NO_CRIT && HITS() == 2 && (STATUS1(1) & STATUS1_POISON); }
static void CheckTwineedle(struct BattleSim *sim)
{
    int full = Stab(Phys(PATK(0), B(1).defense, 25, LV(0))), d = Dealt(sim, 1) - PoisonChip(sim, 1);
    CHECK(STATUS1(1) & STATUS1_POISON, "poisoned by twineedle");
    CHECK(HITS() == 2, "2 hits (got %d)", HITS());
    CHECK(LOG_HAS(STRINGID_HITXTIMES), "hit x times message");
    CHECK(d >= 2 * Roll(full, 85) && d <= 2 * full, "two hits %d (after the %d poison tick) in [%d,%d]", d, PoisonChip(sim, 1), 2 * Roll(full, 85), 2 * full);
}
EXACT(revenge, 1, Mul(Stab(2 * Phys(PATK(0), B(1).defense, 60, LV(0))), 20))
static void CheckRevenge(struct BattleSim *sim) { CHECK(MOVED_BEFORE(1, 0, 0), "revenge (-4) goes last"); Check_revenge(sim); }
EXACT(facade, 1, Stab(2 * PhysHalf(PATK(0), B(1).defense, 70, LV(0))))
EXACT(smelling_salt, 1, Stab(2 * Phys(PATK(0), B(1).defense, 60, LV(0))))
static void CheckSmellingSalt(struct BattleSim *sim) { Check_smelling_salt(sim); CHECK(!(STATUS1(1) & STATUS1_PARALYSIS), "paralysis cured"); }
EXACT(rollout_curl, 1, Phys(PATK(0), B(1).defense, 60, LV(0)))  // Rollout 30 (Rock: no STAB for Marowak), first hit, x2 from Defense Curl

// ================= HP sharing / draining =================
static void CheckPainSplit(struct BattleSim *sim)
{
    int avg = (10 + MAXHP(1)) / 2;
    CHECK(HP(0) == avg && HP(1) == avg, "both at (10+%d)/2 = %d (got %d and %d)", MAXHP(1), avg, HP(0), HP(1));
    CHECK(LOG_HAS(STRINGID_SHAREDPAIN), "shared pain message");
}
static void CheckPainSplitSub(struct BattleSim *sim)
{
    CHECK(STATUS2(1) & STATUS2_SUBSTITUTE, "substitute up");
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "pain split fails against a substitute");
    CHECK(HP(0) == 10, "user hp unchanged");
}
static int WantOddDrain(struct BattleSim *sim) { return NO_CRIT && (Dealt(sim, 1) & 1); }
static void CheckDrainOdd(struct BattleSim *sim)
{
    int d = Dealt(sim, 1);
    CHECK(d & 1, "odd damage %d", d);
    CHECK(HP(0) == 50 + d / 2, "healed floor(%d/2) = %d (hp %d)", d, d / 2, HP(0));
    CHECK(LOG_HAS(STRINGID_PKMNENERGYDRAINED), "energy drained message");
}
static void CheckDrainKo(struct BattleSim *sim)
{
    CHECK(HP(1) == 0, "target fainted");
    CHECK(HP(0) == 50 + 5 / 2, "heal is based on the 5 hp actually dealt (hp %d)", HP(0));
}
static void CheckLiquidOoze(struct BattleSim *sim)
{
    int loss = Dealt(sim, 1) / 2;
    if (loss == 0) loss = 1;
    CHECK(Dealt(sim, 1) > 0, "hit");
    CHECK(HP(0) == 50 - loss, "liquid ooze: lost %d instead of healing (hp %d)", loss, HP(0));
    CHECK(LOG_HAS(STRINGID_ITSUCKEDLIQUIDOOZE), "liquid ooze message");
}
static void CheckDrainVsSub(struct BattleSim *sim)
{
    int subMax = MAXHP(1) / 4, dealt = subMax - sim->disableStructs[1].substituteHP;
    CHECK(STATUS2(1) & STATUS2_SUBSTITUTE, "substitute survived");
    CHECK(HP(1) == MAXHP(1) - subMax, "only the substitute cost was paid (hp %d/%d)", HP(1), MAXHP(1));
    CHECK(dealt > 0 && HP(0) == 50 + dealt / 2, "healed half of the %d dealt to the substitute (hp %d)", dealt, HP(0));
}
static void CheckDreamEaterAwake(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNWASNTAFFECTED), "wasn't affected");
    CHECK(HP(1) == MAXHP(1), "no damage");
}
static int Full_dream_eater(struct BattleSim *sim) { return Spec(PSPA(0), B(1).spDefense, 100, LV(0)); }
static int Want_dream_eater(struct BattleSim *sim) { return NO_CRIT && Dealt(sim, 1) == Full_dream_eater(sim); }
static void Check_dream_eater(struct BattleSim *sim)
{
    int e = Full_dream_eater(sim);
    CHECK(Dealt(sim, 1) == e, "expected %d, dealt %d", e, Dealt(sim, 1));
    CHECK(HP(0) == 50 + e / 2, "healed half (hp %d)", HP(0));
    CHECK(LOG_HAS(STRINGID_PKMNDREAMEATEN), "dream eaten message");
}

// ================= recoil =================
static void CheckDoubleEdge(struct BattleSim *sim)
{
    int recoil = Dealt(sim, 1) / 3;
    if (recoil == 0) recoil = 1;
    CHECK(Dealt(sim, 1) > 0, "hit");
    CHECK(HP(0) == MAXHP(0) - recoil, "recoil 1/3 = %d (hp %d/%d)", recoil, HP(0), MAXHP(0));
    CHECK(LOG_HAS(STRINGID_PKMNHITWITHRECOIL), "recoil message");
}
static void CheckTakeDown(struct BattleSim *sim)
{
    int recoil = Dealt(sim, 1) / 4;
    if (recoil == 0) recoil = 1;
    CHECK(Dealt(sim, 1) > 0, "hit");
    CHECK(HP(0) == MAXHP(0) - recoil, "recoil 1/4 = %d (hp %d/%d)", recoil, HP(0), MAXHP(0));
}
static void CheckRockHead(struct BattleSim *sim)
{
    CHECK(Dealt(sim, 1) > 0, "hit");
    CHECK(HP(0) == MAXHP(0), "rock head: no recoil");
    CHECK(!LOG_HAS(STRINGID_PKMNHITWITHRECOIL), "no recoil message");
}
static void CheckRecoilOnKo(struct BattleSim *sim)
{
    CHECK(HP(1) == 0, "target KOed");
    CHECK(HP(0) == MAXHP(0) - 1, "recoil from the 4 hp dealt = 1 (hp %d/%d)", HP(0), MAXHP(0));
}
static int WantCrash(struct BattleSim *sim) { return LOG_HAS(STRINGID_PKMNCRASHED); }
static void CheckJumpKickMiss(struct BattleSim *sim)
{
    int full = Mul(Stab(Phys(PATK(0), B(1).defense, 85, LV(0))), 20), loss = MAXHP(0) - HP(0);
    CHECK(HP(1) == MAXHP(1), "missed");
    CHECK(loss >= Roll(full, 85) / 2 && loss <= full / 2, "crash damage %d is half of the would-be damage [%d,%d]", loss, Roll(full, 85) / 2, full / 2);
    CHECK(loss <= MAXHP(1) / 2, "capped at half the target's max hp");
}
// Against a Ghost the crash branch is never reached: a real accuracy miss runs CheckWonderGuardAndLevitate, which sets
// DOESNT_AFFECT_FOE, so BattleScript_MoveMissedDoDamage prints "doesn't affect" and leaves before PKMNCRASHED; a hit
// fails in typecalc the same way. Both branches leave the same state and log, so this holds for every seed.
static void CheckJumpKickGhost(struct BattleSim *sim)
{
    CHECK(!LOG_HAS(STRINGID_PKMNCRASHED), "no crash message against a ghost");
    CHECK(LOG_HAS(STRINGID_ITDOESNTAFFECT), "doesn't affect message");
    CHECK(HP(0) == MAXHP(0), "no crash damage (hp %d/%d)", HP(0), MAXHP(0));
    CHECK(HP(1) == MAXHP(1), "ghost untouched");
}
// Protect sends accuracycheck straight to the crash branch (no wonder-guard/immunity check): PKMNCRASHED prints, then
// typecalc inside that branch sets DOESNT_AFFECT_FOE for the Ghost and datahpupdate skips the crash damage.
static void CheckJumpKickProtectGhost(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNCRASHED), "crash message printed");
    CHECK(HP(0) == MAXHP(0), "no crash damage against a protecting ghost (hp %d/%d)", HP(0), MAXHP(0));
    CHECK(HP(1) == MAXHP(1), "ghost untouched");
}
// The same branch against a non-Ghost: crash damage is half the would-be damage (random roll applied), capped at half the target's max HP.
static void CheckJumpKickProtect(struct BattleSim *sim)
{
    int full = Mul(Stab(Phys(PATK(0), B(1).defense, 85, LV(0))), 20), loss = MAXHP(0) - HP(0);
    CHECK(LOG_HAS(STRINGID_PKMNCRASHED), "crash message printed");
    CHECK(HP(1) == MAXHP(1), "protected");
    CHECK(loss >= Roll(full, 85) / 2 && loss <= full / 2, "crash damage %d in [%d,%d]", loss, Roll(full, 85) / 2, full / 2);
    CHECK(loss <= MAXHP(1) / 2, "capped at half the target's max hp");
}

// ================= OHKO / survival =================
static void CheckOhko(struct BattleSim *sim) { CHECK(HP(1) == 0, "one-hit KO"); CHECK(LOG_HAS(STRINGID_ONEHITKO), "OHKO message"); }
static void CheckOhkoLevelBonus(struct BattleSim *sim) { CHECK(HP(1) == 0 && LOG_HAS(STRINGID_ONEHITKO), "30 + (100-5) > 100: always hits"); }
static void CheckSturdy(struct BattleSim *sim)
{
    CHECK(HP(1) == MAXHP(1), "sturdy blocks fissure");
    CHECK(LOG_HAS(STRINGID_PKMNPROTECTEDBY), "protected by sturdy message");
}
static int WantHp1(struct BattleSim *sim) { return HP(1) == 1; }
static void CheckOhkoEndure(struct BattleSim *sim) { CHECK(HP(1) == 1, "endured the OHKO at 1 hp"); CHECK(LOG_HAS(STRINGID_PKMNENDUREDHIT), "endured message"); }
static void CheckFalseSwipe(struct BattleSim *sim) { CHECK(HP(1) == 1, "false swipe leaves 1 hp (hp %d)", HP(1)); }
static void CheckEndure(struct BattleSim *sim) { CHECK(HP(1) == 1, "endured at 1 hp (hp %d)", HP(1)); CHECK(LOG_HAS(STRINGID_PKMNENDUREDHIT), "endured message"); }
static void CheckFocusBand(struct BattleSim *sim) { CHECK(HP(1) == 1, "hung on at 1 hp"); CHECK(LOG_HAS(STRINGID_PKMNHUNGONWITHX), "hung on message"); }

// ================= other effects =================
static int WantSpun(struct BattleSim *sim) { return LOG_HAS(STRINGID_PKMNSHEDLEECHSEED) && LOG_HAS(STRINGID_PKMNBLEWAWAYSPIKES) && NO_CRIT; }
static void CheckRapidSpin(struct BattleSim *sim)
{
    int full = Mul(Phys(PATK(0), B(1).defense, 20, LV(0)), 5);
    CHECK(sim->sideTimers[B_SIDE_PLAYER].spikesAmount == 0, "spikes cleared");
    CHECK(!(STATUS3(0) & STATUS3_LEECHSEED), "leech seed cleared");
    CHECK(InRoll(Dealt(sim, 1), full), "rapid spin damage %d in [%d,%d]", Dealt(sim, 1), Roll(full, 85), full);
}
static void CheckSkyUppercut(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNFLEWHIGH), "target flew up first");
    CHECK(Dealt(sim, 1) > 0, "sky uppercut hits the flying target");
}
EXACT(brick_break, 1, Mul(Stab(Phys(PATK(0), B(1).defense, 75, LV(0))), 5))
static void CheckBrickBreak(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_THEWALLSHATTERED), "wall shattered");
    CHECK(!(SIDE_STATUS(B_SIDE_OPPONENT) & SIDE_STATUS_REFLECT), "reflect gone");
    Check_brick_break(sim);   // full (unhalved) damage
}
EXACT(knock_off, 1, Stab(Spec(PSPA(0), B(1).spDefense, 20, LV(0))))   // Dark is a special type in gen 3; Sneasel gets STAB
static void CheckKnockOff(struct BattleSim *sim)
{
    Check_knock_off(sim);
    CHECK(B(1).item == ITEM_NONE, "item knocked off");
    CHECK(LOG_HAS(STRINGID_PKMNKNOCKEDOFF), "knocked off message");
    CHECK(sim->wishFutureKnock.knockedOffMons[B_SIDE_OPPONENT] & 1, "knocked-off flag for party slot 0");
}
static void CheckThawHit(struct BattleSim *sim)
{
    CHECK(!(STATUS1(0) & STATUS1_FREEZE), "flame wheel thawed the user");
    CHECK(Dealt(sim, 1) > 0, "and hit");
    CHECK(LOG_HAS(STRINGID_PKMNWASDEFROSTED) || LOG_HAS(STRINGID_PKMNWASDEFROSTED2) || LOG_HAS(STRINGID_PKMNWASDEFROSTEDBY), "defrost message");
}
static int WantPoisoned(struct BattleSim *sim) { return NO_CRIT && (STATUS1(1) & STATUS1_POISON) != 0; }
static void CheckSecretPower(struct BattleSim *sim)
{
    int full = Stab(Phys(PATK(0), B(1).defense, 70, LV(0))), d = Dealt(sim, 1) - PoisonChip(sim, 1);
    CHECK(STATUS1(1) & STATUS1_POISON, "grass terrain: secret power poisons");
    CHECK(InRoll(d, full), "damage %d (after the %d poison tick) in [%d,%d]", d, PoisonChip(sim, 1), Roll(full, 85), full);
}
EXACT(charge, 1, Stab(2 * Spec(PSPA(0), B(1).spDefense, 95, LV(0))))
static void CheckCharge(struct BattleSim *sim) { CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_CHARGE, 0), "charged"); Check_charge(sim); }
EXACT(mud_sport, 1, Stab(Spec(PSPA(0), B(1).spDefense, 47, LV(0))))

// ================= doubles =================
static int WantBothHit(struct BattleSim *sim) { return NO_CRIT && Dealt(sim, 1) > 0 && Dealt(sim, 3) > 0; }
static void CheckRockSlideDoubles(struct BattleSim *sim)
{
    int b;
    for (b = 1; b <= 3; b += 2)
    {
        int full = Stab(PhysHalf(PATK(0), B(b).defense, 75, LV(0)));
        CHECK(InRoll(Dealt(sim, b), full), "battler %d: halved spread damage %d in [%d,%d]", b, Dealt(sim, b), Roll(full, 85), full);
    }
}
static void CheckEarthquakeDoubles(struct BattleSim *sim)
{
    int b;
    for (b = 1; b <= 3; b += 2)
    {
        int full = Stab(Phys(PATK(0), B(b).defense, 100, LV(0)));
        CHECK(InRoll(Dealt(sim, b), full), "battler %d: earthquake (FOES_AND_ALLY) is not halved: %d in [%d,%d]", b, Dealt(sim, b), Roll(full, 85), full);
    }
    CHECK(HP(2) == MAXHP(2), "flying partner immune");
}
EXACT(helping_hand, 1, Phys(PATK(0), B(1).defense, 80, LV(0)) * 15 / 10)

static const struct Scenario sScenarios[] =
{
    // ---- base formula ----
    { .name = "formula_physical_badge",
      .player = { M1(SPECIES_MACHOP, 50, MOVE_STRENGTH) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_phys, .check = Check_phys },
    { .name = "formula_special_badge",
      .player = { M1(SPECIES_SQUIRTLE, 50, MOVE_ICE_BEAM) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_spec, .check = Check_spec },
    { .name = "formula_enemy_physical_no_badge",
      .player = { SPLASHER(SPECIES_SNORLAX, 50) }, .enemy = { M1(SPECIES_MACHOP, 50, MOVE_STRENGTH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_enemy_phys, .check = Check_enemy_phys },
    { .name = "formula_enemy_special_no_badge",
      .player = { SPLASHER(SPECIES_SNORLAX, 50) }, .enemy = { M1(SPECIES_SQUIRTLE, 50, MOVE_ICE_BEAM) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_enemy_spec, .check = Check_enemy_spec },
    { .name = "random_roll_85_to_100",
      .player = { M1(SPECIES_MACHOP, 50, MOVE_STRENGTH) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .seed = 777, .check = CheckRollRange },
    { .name = "stab_1_5x",
      .player = { M1(SPECIES_RATTATA, 50, MOVE_SCRATCH) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_stab, .check = Check_stab },
    { .name = "burn_halves_physical",
      .player = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_SCRATCH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .status = STATUS1_BURN } },
      .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_burn_phys, .check = Check_burn_phys },
    { .name = "burn_does_not_halve_special",
      .player = { { .species = SPECIES_CHARMANDER, .level = 50, .moves = { MOVE_EMBER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .status = STATUS1_BURN } },
      .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_burn_spec, .check = Check_burn_spec },
    { .name = "guts_burn_1_5x_no_halving",
      .player = { { .species = SPECIES_MACHOP, .level = 50, .moves = { MOVE_STRENGTH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .status = STATUS1_BURN } },
      .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_guts, .check = Check_guts },
    { .name = "reflect_halves_physical",
      .player = { M1(SPECIES_MACHOP, 50, MOVE_STRENGTH) }, .enemy = { M1(SPECIES_SNORLAX, 50, MOVE_REFLECT) },
      .actions = { T(1, 0), T(0, 1) }, .turns = 2, .wantSeed = Want_reflect, .check = Check_reflect },
    { .name = "light_screen_halves_special",
      .player = { M1(SPECIES_SQUIRTLE, 50, MOVE_ICE_BEAM) }, .enemy = { M1(SPECIES_SNORLAX, 50, MOVE_LIGHT_SCREEN) },
      .actions = { T(1, 0), T(0, 1) }, .turns = 2, .wantSeed = Want_light_screen, .check = Check_light_screen },
    { .name = "crit_ignores_reflect",
      .player = { M1(SPECIES_MACHOP, 50, MOVE_STRENGTH) }, .enemy = { M1(SPECIES_SNORLAX, 50, MOVE_REFLECT) },
      .actions = { T(1, 0), T(0, 1) }, .turns = 2, .wantSeed = Want_crit_reflect, .check = Check_crit_reflect },
    { .name = "crit_doubles_damage",
      .player = { M1(SPECIES_RATTATA, 50, MOVE_SCRATCH) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_crit_double, .check = Check_crit_double },
    { .name = "crit_ignores_attack_drop",
      .player = { M1(SPECIES_MACHOP, 50, MOVE_STRENGTH) }, .enemy = { M1(SPECIES_SNORLAX, 50, MOVE_CHARM) },
      .actions = { T(1, 0), T(0, 1) }, .turns = 2, .wantSeed = Want_crit_atk_drop, .check = CheckCritAtkDrop },
    { .name = "crit_ignores_defense_boost",
      .player = { M1(SPECIES_MACHOP, 50, MOVE_STRENGTH) }, .enemy = { M1(SPECIES_SNORLAX, 50, MOVE_HARDEN) },
      .actions = { T(1, 0), T(0, 1) }, .turns = 2, .wantSeed = Want_crit_def_boost, .check = CheckCritDefBoost },
    { .name = "crit_keeps_attack_boost",
      .player = { M2(SPECIES_MACHOP, 50, MOVE_STRENGTH, MOVE_SWORDS_DANCE) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(1, 0), T(0, 0) }, .turns = 2, .wantSeed = Want_crit_atk_boost, .check = Check_crit_atk_boost },
    { .name = "attack_plus_2_doubles",
      .player = { M2(SPECIES_MACHOP, 50, MOVE_STRENGTH, MOVE_SWORDS_DANCE) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(1, 0), T(0, 0) }, .turns = 2, .wantSeed = Want_atk_plus2, .check = Check_atk_plus2 },
    { .name = "defense_minus_2_screech",
      .player = { M2(SPECIES_RATTATA, 50, MOVE_SCRATCH, MOVE_SCREECH) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(1, 0), T(0, 0) }, .turns = 2, .wantSeed = Want_def_minus2, .check = Check_def_minus2 },
    { .name = "huge_power_doubles_attack",
      .player = { MA1(SPECIES_AZUMARILL, 50, MOVE_STRENGTH, 1) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_huge_power, .check = Check_huge_power },
    { .name = "choice_band_1_5x",
      .player = { MI1(SPECIES_MACHOP, 50, MOVE_STRENGTH, ITEM_CHOICE_BAND) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_choice_band, .check = Check_choice_band },
    { .name = "silk_scarf_normal_10pct",
      .player = { MI1(SPECIES_RATTATA, 50, MOVE_SCRATCH, ITEM_SILK_SCARF) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_silk_scarf, .check = Check_silk_scarf },
    { .name = "charcoal_fire_10pct",
      .player = { MI1(SPECIES_CHARMANDER, 50, MOVE_EMBER, ITEM_CHARCOAL) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_charcoal, .check = Check_charcoal },
    { .name = "thick_fat_halves_fire_spatk",
      .player = { M1(SPECIES_CHARMANDER, 50, MOVE_EMBER) }, .enemy = { MA1(SPECIES_SNORLAX, 50, MOVE_SPLASH, 1) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_thick_fat, .check = Check_thick_fat },
    { .name = "hustle_1_5x_attack",
      .player = { M1(SPECIES_TOGETIC, 50, MOVE_STRENGTH) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_hustle, .check = Check_hustle },
    { .name = "thick_club_marowak",
      .player = { MI1(SPECIES_MAROWAK, 50, MOVE_EARTHQUAKE, ITEM_THICK_CLUB) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_thick_club, .check = Check_thick_club },
    { .name = "light_ball_pikachu",
      .player = { MI1(SPECIES_PIKACHU, 50, MOVE_THUNDERBOLT, ITEM_LIGHT_BALL) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_light_ball, .check = Check_light_ball },
    { .name = "metal_powder_ditto",
      .player = { M1(SPECIES_RATTATA, 50, MOVE_SCRATCH) }, .enemy = { MI1(SPECIES_DITTO, 50, MOVE_SPLASH, ITEM_METAL_POWDER) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_metal_powder, .check = Check_metal_powder },
    { .name = "marvel_scale_statused",
      .player = { M1(SPECIES_MACHOP, 50, MOVE_STRENGTH) },
      .enemy = { { .species = SPECIES_MILOTIC, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .status = STATUS1_PARALYSIS } },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_marvel_scale, .check = Check_marvel_scale },
    { .name = "explosion_halves_defense",
      .player = { M1(SPECIES_ELECTRODE, 30, MOVE_EXPLOSION) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 100) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_explosion, .check = CheckExplosion },
    { .name = "self_destruct_vs_ghost_still_faints",
      .player = { M1(SPECIES_ELECTRODE, 50, MOVE_SELF_DESTRUCT) }, .enemy = { SPLASHER(SPECIES_GASTLY, 50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSelfDestructGhost },
    { .name = "minimum_damage_1",
      .player = { M1(SPECIES_CATERPIE, 2, MOVE_TACKLE) }, .enemy = { SPLASHER(SPECIES_STEELIX, 100) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantHit1, .check = CheckMinDamage },
    { .name = "rain_boosts_water",
      .player = { M2(SPECIES_SQUIRTLE, 50, MOVE_WATER_GUN, MOVE_RAIN_DANCE) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(1, 0), T(0, 0) }, .turns = 2, .wantSeed = Want_rain_water, .check = CheckRainWater },
    { .name = "rain_halves_fire",
      .player = { M2(SPECIES_CHARMANDER, 50, MOVE_EMBER, MOVE_RAIN_DANCE) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(1, 0), T(0, 0) }, .turns = 2, .wantSeed = Want_rain_fire, .check = Check_rain_fire },
    { .name = "sun_boosts_fire",
      .player = { M2(SPECIES_CHARMANDER, 50, MOVE_EMBER, MOVE_SUNNY_DAY) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(1, 0), T(0, 0) }, .turns = 2, .wantSeed = Want_sun_fire, .check = CheckSunFire },
    { .name = "overgrow_at_third_hp",
      .player = { { .species = SPECIES_BULBASAUR, .level = 50, .moves = { MOVE_VINE_WHIP, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 1, .hpSet = 1 } },
      .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_overgrow, .check = Check_overgrow },
    { .name = "flash_fire_boost",
      .player = { MA1(SPECIES_GROWLITHE, 50, MOVE_EMBER, 1) }, .enemy = { M1(SPECIES_PONYTA, 50, MOVE_EMBER) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_flash_fire, .check = Check_flash_fire },

    // ---- type chart ----
    { .name = "super_effective_2x",
      .player = { M1(SPECIES_SQUIRTLE, 50, MOVE_WATER_GUN) }, .enemy = { SPLASHER(SPECIES_CHARMANDER, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_se2, .check = CheckSe2 },
    { .name = "not_very_effective_half",
      .player = { M1(SPECIES_RATTATA, 50, MOVE_SCRATCH) }, .enemy = { SPLASHER(SPECIES_STEELIX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_nve, .check = CheckNve },
    { .name = "dual_type_4x",
      .player = { M1(SPECIES_LAPRAS, 50, MOVE_ICE_BEAM) }, .enemy = { SPLASHER(SPECIES_DRAGONITE, 100) },   // L100: 4x would KO a L50 Dragonite and cap the damage
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_se4, .check = Check_se4 },
    { .name = "dual_type_quarter",
      .player = { M1(SPECIES_MACHAMP, 50, MOVE_BRICK_BREAK) }, .enemy = { SPLASHER(SPECIES_ZUBAT, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_nve4, .check = Check_nve4 },
    { .name = "immune_ground_vs_flying",
      .player = { M1(SPECIES_DUGTRIO, 50, MOVE_EARTHQUAKE) }, .enemy = { SPLASHER(SPECIES_PIDGEY, 50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckImmune },
    { .name = "immune_electric_vs_ground",
      .player = { M1(SPECIES_PIKACHU, 50, MOVE_THUNDERBOLT) }, .enemy = { SPLASHER(SPECIES_DIGLETT, 50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckImmune },
    { .name = "immune_psychic_vs_dark",
      .player = { M1(SPECIES_ALAKAZAM, 50, MOVE_PSYCHIC) }, .enemy = { SPLASHER(SPECIES_UMBREON, 50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckImmune },
    { .name = "immune_poison_vs_steel",
      .player = { M1(SPECIES_GENGAR, 50, MOVE_SLUDGE_BOMB) }, .enemy = { SPLASHER(SPECIES_STEELIX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckImmune },
    { .name = "levitate_ground_immunity",
      .player = { M1(SPECIES_DUGTRIO, 50, MOVE_EARTHQUAKE) }, .enemy = { SPLASHER(SPECIES_GENGAR, 50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckLevitate },
    { .name = "foresight_normal_hits_ghost",
      .player = { M2(SPECIES_RATTATA, 50, MOVE_SCRATCH, MOVE_FORESIGHT) }, .enemy = { SPLASHER(SPECIES_GASTLY, 50) },
      .actions = { T(1, 0), T(0, 0) }, .turns = 2, .wantSeed = Want_foresight, .check = CheckForesight },
    { .name = "foresight_seismic_toss_hits_ghost",
      .player = { M2(SPECIES_MACHOP, 50, MOVE_SEISMIC_TOSS, MOVE_FORESIGHT) }, .enemy = { SPLASHER(SPECIES_GASTLY, 50) },
      .actions = { T(1, 0), T(0, 0) }, .turns = 2, .check = CheckForesightSeismicToss },
    { .name = "wonder_guard_super_effective_hits",
      .player = { M1(SPECIES_CHARMANDER, 50, MOVE_EMBER) }, .enemy = { SPLASHER(SPECIES_SHEDINJA, 50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckWonderGuardSe },
    { .name = "wonder_guard_blocks_not_very_effective",
      .player = { M1(SPECIES_GENGAR, 50, MOVE_SLUDGE_BOMB) }, .enemy = { SPLASHER(SPECIES_SHEDINJA, 50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckWonderGuardBlocked },
    { .name = "wonder_guard_blocks_dragon_rage",
      .player = { M1(SPECIES_GYARADOS, 50, MOVE_DRAGON_RAGE) }, .enemy = { SPLASHER(SPECIES_SHEDINJA, 50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckWonderGuardBlocked },
    { .name = "struggle_bypasses_wonder_guard",
      .player = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_TACKLE, MOVE_NONE, MOVE_NONE, MOVE_NONE }, .pp = { 0, 0, 0, 0 }, .ppSet = 1 } },
      .enemy = { SPLASHER(SPECIES_SHEDINJA, 50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckStruggleWonderGuard },
    { .name = "struggle_typeless_hits_ghost_recoil_quarter",
      .player = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_TACKLE, MOVE_NONE, MOVE_NONE, MOVE_NONE }, .pp = { 0, 0, 0, 0 }, .ppSet = 1 } },
      .enemy = { SPLASHER(SPECIES_GASTLY, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_struggle_ghost, .check = CheckStruggleGhost },
    { .name = "future_sight_typeless_vs_dark",
      .player = { M1(SPECIES_ALAKAZAM, 50, MOVE_FUTURE_SIGHT) }, .enemy = { SPLASHER(SPECIES_UMBREON, 50) },
      .actions = { T(0, 0), T(1, 0), T(1, 0) }, .turns = 3, .wantSeed = WantFutureSightHit, .check = CheckFutureSight },

    // ---- fixed damage ----
    { .name = "seismic_toss_level",
      .player = { M1(SPECIES_MACHOP, 37, MOVE_SEISMIC_TOSS) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSeismicToss },
    { .name = "seismic_toss_vs_ghost_immune",
      .player = { M1(SPECIES_MACHOP, 50, MOVE_SEISMIC_TOSS) }, .enemy = { SPLASHER(SPECIES_GASTLY, 50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckImmune },
    { .name = "night_shade_level",
      .player = { M1(SPECIES_GASTLY, 43, MOVE_NIGHT_SHADE) }, .enemy = { SPLASHER(SPECIES_MACHOP, 50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckNightShade },
    { .name = "night_shade_vs_normal_immune",
      .player = { M1(SPECIES_GASTLY, 50, MOVE_NIGHT_SHADE) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckImmune },
    { .name = "dragon_rage_40_ignores_resistance",
      .player = { M1(SPECIES_GYARADOS, 50, MOVE_DRAGON_RAGE) }, .enemy = { SPLASHER(SPECIES_STEELIX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckDragonRageSteel },
    { .name = "sonic_boom_20",
      .player = { M1(SPECIES_MAGNETON, 50, MOVE_SONIC_BOOM) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantDealt20, .check = CheckSonicBoom },
    { .name = "sonic_boom_vs_ghost_immune",
      .player = { M1(SPECIES_MAGNETON, 50, MOVE_SONIC_BOOM) }, .enemy = { SPLASHER(SPECIES_GASTLY, 50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckImmune },
    { .name = "super_fang_half",
      .player = { M1(SPECIES_RATICATE, 50, MOVE_SUPER_FANG) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantSuperFang, .check = CheckSuperFang },
    { .name = "super_fang_on_1hp",
      .player = { M1(SPECIES_RATICATE, 50, MOVE_SUPER_FANG) },
      .enemy = { { .species = SPECIES_SNORLAX, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 1, .hpSet = 1 } },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantKo, .check = CheckSuperFang1Hp },
    { .name = "psywave_level_range",
      .player = { M1(SPECIES_ALAKAZAM, 51, MOVE_PSYWAVE) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantHit1, .check = CheckPsywave },
    { .name = "endeavor_matches_user_hp",
      .player = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_ENDEAVOR, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 10, .hpSet = 1 } },
      .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckEndeavor },
    { .name = "endeavor_fails_vs_lower_hp",
      .player = { M1(SPECIES_SNORLAX, 50, MOVE_ENDEAVOR) }, .enemy = { SPLASHER(SPECIES_RATTATA, 50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckEndeavorFails },
    { .name = "endeavor_vs_ghost_immune",
      .player = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_ENDEAVOR, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 10, .hpSet = 1 } },
      .enemy = { SPLASHER(SPECIES_GASTLY, 50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckImmune },

    // ---- variable power ----
    { .name = "flail_1hp_power_200",
      .player = { { .species = SPECIES_RATICATE, .level = 50, .moves = { MOVE_FLAIL, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 1, .hpSet = 1 } },
      .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantFlail, .check = CheckFlail1Hp },
    { .name = "flail_full_hp_power_20",
      .player = { M1(SPECIES_RATICATE, 50, MOVE_FLAIL) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantFlail, .check = CheckFlailFull },
    { .name = "reversal_bracket_100",
      .player = { { .species = SPECIES_HITMONLEE, .level = 50, .moves = { MOVE_REVERSAL, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 24, .hpSet = 1 } },
      .enemy = { SPLASHER(SPECIES_SNORLAX, 100) },   // L100: the full roll exceeds a L50 Snorlax's 235 hp
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantReversal, .check = CheckReversalMid },
    { .name = "return_friendship_70_power_28",
      .player = { M1(SPECIES_RATICATE, 50, MOVE_RETURN) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_return70, .check = CheckReturn70 },
    { .name = "frustration_friendship_70_power_74",
      .player = { M1(SPECIES_RATICATE, 50, MOVE_FRUSTRATION) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_frustration70, .check = CheckFrustration70 },
    { .name = "return_friendship_140_power_56",
      .player = { M1(SPECIES_CLEFABLE, 50, MOVE_RETURN) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_return140, .check = CheckReturn140 },
    { .name = "present_heals_quarter",
      .player = { M1(SPECIES_RATTATA, 50, MOVE_PRESENT) },
      .enemy = { { .species = SPECIES_SNORLAX, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 100, .hpSet = 1 } },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantPresentHeal, .check = CheckPresentHeal },
    { .name = "present_power_120",
      .player = { M1(SPECIES_RATTATA, 50, MOVE_PRESENT) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_present120, .check = Check_present120 },
    { .name = "magnitude_power_table",
      .player = { M1(SPECIES_DUGTRIO, 50, MOVE_MAGNITUDE) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantMagnitude, .check = CheckMagnitude },
    { .name = "low_kick_heavy_120",
      .player = { M1(SPECIES_HITMONLEE, 50, MOVE_LOW_KICK) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 100) },   // L100: the full roll exceeds a L50 Snorlax's 235 hp
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_low_kick_heavy, .check = Check_low_kick_heavy },
    { .name = "low_kick_light_20",
      .player = { M1(SPECIES_HITMONLEE, 50, MOVE_LOW_KICK) }, .enemy = { SPLASHER(SPECIES_RATTATA, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_low_kick_light, .check = Check_low_kick_light },
    { .name = "low_kick_mid_80",
      .player = { M1(SPECIES_HITMONLEE, 50, MOVE_LOW_KICK) }, .enemy = { SPLASHER(SPECIES_GOLBAT, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_low_kick_mid, .check = Check_low_kick_mid },
    { .name = "hidden_power_iv31_dark_70",
      .player = { M1(SPECIES_GENGAR, 50, MOVE_HIDDEN_POWER) }, .enemy = { SPLASHER(SPECIES_ALAKAZAM, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_hp_dark, .check = Check_hp_dark },
    { .name = "hidden_power_iv30_fighting_70",
      .player = { { .species = SPECIES_GENGAR, .level = 50, .moves = { MOVE_HIDDEN_POWER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .ivs = { 30, 30, 30, 30, 30, 30 }, .ivsSet = 1 } },
      .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_hp_fighting70, .check = Check_hp_fighting70 },
    { .name = "hidden_power_iv0_fighting_30",
      .player = { { .species = SPECIES_SNORLAX, .level = 50, .moves = { MOVE_HIDDEN_POWER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .ivs = { 0, 0, 0, 0, 0, 0 }, .ivsSet = 1 } },
      .enemy = { SPLASHER(SPECIES_MACHOP, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_hp_fighting30, .check = Check_hp_fighting30 },
    { .name = "eruption_full_hp_150",
      .player = { M1(SPECIES_TYPHLOSION, 50, MOVE_ERUPTION) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_eruption_full, .check = Check_eruption_full },
    { .name = "eruption_scales_with_hp",
      .player = { { .species = SPECIES_TYPHLOSION, .level = 50, .moves = { MOVE_ERUPTION, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 20, .hpSet = 1 } },
      .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantEruptionLow, .check = CheckEruptionLow },
    { .name = "spit_up_two_stockpiles_no_roll",
      .player = { M2(SPECIES_IVYSAUR, 50, MOVE_SPIT_UP, MOVE_STOCKPILE) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(1, 0), T(1, 0), T(0, 0) }, .turns = 3, .check = CheckSpitUp2 },
    { .name = "stockpile_caps_at_3",
      .player = { M1(SPECIES_IVYSAUR, 50, MOVE_STOCKPILE) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0), T(0, 0), T(0, 0), T(0, 0) }, .turns = 4, .check = CheckStockpileCap },
    { .name = "swallow_two_stockpiles_heals_half",
      .player = { { .species = SPECIES_IVYSAUR, .level = 50, .moves = { MOVE_SWALLOW, MOVE_STOCKPILE, MOVE_SPLASH, MOVE_SPLASH }, .hp = 1, .hpSet = 1 } },
      .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(1, 0), T(1, 0), T(0, 0) }, .turns = 3, .check = CheckSwallow2 },
    { .name = "spit_up_without_stockpile_fails",
      .player = { M1(SPECIES_IVYSAUR, 50, MOVE_SPIT_UP) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSpitUpFails },
    { .name = "beat_up_skips_statused_party",
      .player = { M1(SPECIES_SNEASEL, 50, MOVE_BEAT_UP), SPLASHER(SPECIES_RATTATA, 50),
                  { .species = SPECIES_MACHOP, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .status = STATUS1_PARALYSIS } },
      .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantBeatUp, .check = CheckBeatUp },
    { .name = "triple_kick_10_20_30",
      .player = { M1(SPECIES_HITMONTOP, 50, MOVE_TRIPLE_KICK) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantTripleKick, .check = CheckTripleKick },
    { .name = "multi_hit_five_times",
      .player = { M1(SPECIES_PIDGEOT, 50, MOVE_FURY_ATTACK) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantHits5, .check = CheckFuryAttack5 },
    { .name = "multi_hit_two_times",
      .player = { M1(SPECIES_PIDGEOT, 50, MOVE_FURY_ATTACK) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantHits2, .check = CheckFuryAttack2 },
    { .name = "multi_hit_stops_on_faint",
      .player = { M1(SPECIES_PIDGEOT, 50, MOVE_FURY_ATTACK) },
      .enemy = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 1, .hpSet = 1 } },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantHit1Ko, .check = CheckMultiHitStopsOnFaint },
    { .name = "double_hit_bonemerang",
      .player = { M1(SPECIES_MAROWAK, 50, MOVE_BONEMERANG) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantHits2, .check = CheckBonemerang },
    { .name = "twineedle_two_hits_poison",
      .player = { M1(SPECIES_BEEDRILL, 50, MOVE_TWINEEDLE) }, .enemy = { MA1(SPECIES_SNORLAX, 50, MOVE_SPLASH, 1) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantTwineedle, .check = CheckTwineedle },
    { .name = "revenge_doubles_after_being_hit",
      .player = { M1(SPECIES_MACHOP, 50, MOVE_REVENGE) }, .enemy = { M1(SPECIES_SNORLAX, 50, MOVE_TACKLE) },   // bulky enough to survive the doubled hit
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_revenge, .check = CheckRevenge },
    { .name = "facade_burned_doubles_after_burn_halving",
      .player = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_FACADE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .status = STATUS1_BURN } },
      .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_facade, .check = Check_facade },
    { .name = "smelling_salt_paralyzed_double_and_cure",
      .player = { M1(SPECIES_RATTATA, 50, MOVE_SMELLING_SALT) },
      .enemy = { { .species = SPECIES_SNORLAX, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .status = STATUS1_PARALYSIS } },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_smelling_salt, .check = CheckSmellingSalt },
    { .name = "rollout_defense_curl_first_hit_60",
      .player = { M2(SPECIES_MAROWAK, 50, MOVE_ROLLOUT, MOVE_DEFENSE_CURL) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(1, 0), T(0, 0) }, .turns = 2, .wantSeed = Want_rollout_curl, .check = Check_rollout_curl },

    // ---- HP sharing / draining ----
    { .name = "pain_split_average",
      .player = { { .species = SPECIES_GENGAR, .level = 50, .moves = { MOVE_PAIN_SPLIT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 10, .hpSet = 1 } },
      .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckPainSplit },
    { .name = "pain_split_vs_substitute_fails",
      .player = { { .species = SPECIES_GENGAR, .level = 50, .moves = { MOVE_PAIN_SPLIT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 10, .hpSet = 1 } },
      .enemy = { M1(SPECIES_ELECTRODE, 50, MOVE_SUBSTITUTE) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckPainSplitSub },
    { .name = "mega_drain_odd_damage_rounds_down",
      .player = { { .species = SPECIES_IVYSAUR, .level = 50, .moves = { MOVE_MEGA_DRAIN, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 50, .hpSet = 1 } },
      .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantOddDrain, .check = CheckDrainOdd },
    { .name = "giga_drain_on_ko_heals_from_hp_dealt",
      .player = { { .species = SPECIES_IVYSAUR, .level = 50, .moves = { MOVE_GIGA_DRAIN, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 50, .hpSet = 1 } },
      .enemy = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 5, .hpSet = 1 } },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckDrainKo },
    { .name = "liquid_ooze_hurts_drainer",
      .player = { { .species = SPECIES_IVYSAUR, .level = 50, .moves = { MOVE_MEGA_DRAIN, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 50, .hpSet = 1 } },
      .enemy = { SPLASHER(SPECIES_GULPIN, 50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckLiquidOoze },
    { .name = "drain_vs_substitute_heals_from_sub_damage",
      .player = { { .species = SPECIES_IVYSAUR, .level = 50, .moves = { MOVE_MEGA_DRAIN, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 50, .hpSet = 1 } },
      .enemy = { M1(SPECIES_ELECTRODE, 50, MOVE_SUBSTITUTE) },
      .actions = { T(1, 0), T(0, 1) }, .turns = 2, .check = CheckDrainVsSub },
    { .name = "dream_eater_needs_sleep",
      .player = { M1(SPECIES_GENGAR, 50, MOVE_DREAM_EATER) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckDreamEaterAwake },
    { .name = "dream_eater_heals_half",
      .player = { { .species = SPECIES_GENGAR, .level = 50, .moves = { MOVE_DREAM_EATER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 50, .hpSet = 1 } },
      .enemy = { { .species = SPECIES_SNORLAX, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .status = STATUS1_SLEEP_TURN(3) } },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_dream_eater, .check = Check_dream_eater },

    // ---- recoil ----
    { .name = "double_edge_recoil_third",
      .player = { M1(SPECIES_TAUROS, 50, MOVE_DOUBLE_EDGE) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckDoubleEdge },
    { .name = "take_down_recoil_quarter",
      .player = { M1(SPECIES_TAUROS, 50, MOVE_TAKE_DOWN) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantHit1, .check = CheckTakeDown },
    { .name = "rock_head_no_recoil",
      .player = { M1(SPECIES_GOLEM, 50, MOVE_DOUBLE_EDGE) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckRockHead },
    { .name = "recoil_on_ko_uses_hp_dealt",
      .player = { M1(SPECIES_TAUROS, 50, MOVE_DOUBLE_EDGE) },
      .enemy = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 4, .hpSet = 1 } },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckRecoilOnKo },
    { .name = "hi_jump_kick_miss_crash_half",
      .player = { M1(SPECIES_HITMONLEE, 50, MOVE_HI_JUMP_KICK) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantCrash, .check = CheckJumpKickMiss },
    { .name = "hi_jump_kick_vs_ghost_never_crashes",
      .player = { M1(SPECIES_HITMONLEE, 50, MOVE_HI_JUMP_KICK) }, .enemy = { SPLASHER(SPECIES_GASTLY, 50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckJumpKickGhost },
    { .name = "hi_jump_kick_vs_protecting_ghost_no_crash_damage",
      .player = { M1(SPECIES_HITMONLEE, 50, MOVE_HI_JUMP_KICK) }, .enemy = { M1(SPECIES_GASTLY, 50, MOVE_PROTECT) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckJumpKickProtectGhost },
    { .name = "hi_jump_kick_vs_protect_crash_half",
      .player = { M1(SPECIES_HITMONLEE, 50, MOVE_HI_JUMP_KICK) }, .enemy = { M1(SPECIES_SNORLAX, 50, MOVE_PROTECT) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckJumpKickProtect },

    // ---- OHKO / survival ----
    { .name = "ohko_same_level_30pct",
      .player = { M1(SPECIES_DUGTRIO, 50, MOVE_FISSURE) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantKo, .check = CheckOhko },
    { .name = "ohko_level_bonus_always_hits",
      .player = { M1(SPECIES_DUGTRIO, 100, MOVE_FISSURE) }, .enemy = { SPLASHER(SPECIES_RATTATA, 5) },
      .actions = { T(0, 0) }, .turns = 1, .seed = 99, .check = CheckOhkoLevelBonus },
    { .name = "ohko_sturdy",
      .player = { M1(SPECIES_DUGTRIO, 50, MOVE_FISSURE) }, .enemy = { MA1(SPECIES_ONIX, 50, MOVE_SPLASH, 1) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSturdy },
    { .name = "ohko_vs_flying_immune",
      .player = { M1(SPECIES_DUGTRIO, 50, MOVE_FISSURE) }, .enemy = { SPLASHER(SPECIES_PIDGEY, 50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckImmune },
    { .name = "ohko_endured_at_1hp",
      .player = { M1(SPECIES_DUGTRIO, 50, MOVE_FISSURE) }, .enemy = { M1(SPECIES_RATTATA, 50, MOVE_ENDURE) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantHp1, .check = CheckOhkoEndure },
    { .name = "false_swipe_leaves_1hp",
      .player = { M1(SPECIES_SCYTHER, 50, MOVE_FALSE_SWIPE) },
      .enemy = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 5, .hpSet = 1 } },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckFalseSwipe },
    { .name = "endure_survives_at_1hp",
      .player = { M1(SPECIES_MACHAMP, 50, MOVE_STRENGTH) },
      .enemy = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_ENDURE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .hp = 5, .hpSet = 1 } },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckEndure },
    { .name = "focus_band_hangs_on",
      .player = { M1(SPECIES_MACHAMP, 50, MOVE_STRENGTH) },
      .enemy = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .item = ITEM_FOCUS_BAND, .hp = 5, .hpSet = 1 } },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantHp1, .check = CheckFocusBand },

    // ---- other effects ----
    { .name = "rapid_spin_clears_spikes_and_seed",
      .player = { M2(SPECIES_FORRETRESS, 50, MOVE_RAPID_SPIN, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_FORRETRESS, 50, MOVE_SPIKES, MOVE_LEECH_SEED, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(1, 0), T(1, 1), T(0, 2) }, .turns = 3, .wantSeed = WantSpun, .check = CheckRapidSpin },
    { .name = "sky_uppercut_hits_flying_target",
      .player = { M1(SPECIES_MACHAMP, 50, MOVE_SKY_UPPERCUT) }, .enemy = { M1(SPECIES_PIDGEOT, 50, MOVE_FLY) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantHit1, .check = CheckSkyUppercut },
    { .name = "brick_break_shatters_reflect_full_damage",
      .player = { M1(SPECIES_MACHAMP, 50, MOVE_BRICK_BREAK) }, .enemy = { M1(SPECIES_ALAKAZAM, 50, MOVE_REFLECT) },
      .actions = { T(1, 0), T(0, 1) }, .turns = 2, .wantSeed = Want_brick_break, .check = CheckBrickBreak },   // one Brick Break only, after Reflect is up
    { .name = "knock_off_removes_item",
      .player = { M1(SPECIES_SNEASEL, 50, MOVE_KNOCK_OFF) }, .enemy = { MI1(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_LEFTOVERS) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = Want_knock_off, .check = CheckKnockOff },
    { .name = "flame_wheel_thaws_user",
      .player = { { .species = SPECIES_PONYTA, .level = 50, .moves = { MOVE_FLAME_WHEEL, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .status = STATUS1_FREEZE } },
      .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckThawHit },
    { .name = "secret_power_grass_poisons",
      .player = { M1(SPECIES_RATTATA, 50, MOVE_SECRET_POWER) }, .enemy = { MA1(SPECIES_SNORLAX, 50, MOVE_SPLASH, 1) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantPoisoned, .check = CheckSecretPower },
    { .name = "charge_doubles_electric",
      .player = { M2(SPECIES_PIKACHU, 50, MOVE_THUNDERBOLT, MOVE_CHARGE) }, .enemy = { SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T(1, 0), T(0, 0) }, .turns = 2, .wantSeed = Want_charge, .check = CheckCharge },
    { .name = "mud_sport_halves_electric_power",
      .player = { M1(SPECIES_PIKACHU, 50, MOVE_THUNDERBOLT) }, .enemy = { M1(SPECIES_SNORLAX, 50, MOVE_MUD_SPORT) },
      .actions = { T(1, 0), T(0, 1) }, .turns = 2, .wantSeed = Want_mud_sport, .check = Check_mud_sport },

    // ---- doubles ----
    { .name = "doubles_rock_slide_halved",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M1(SPECIES_GOLEM, 50, MOVE_ROCK_SLIDE), SPLASHER(SPECIES_PIDGEY, 50) },
      .enemy = { SPLASHER(SPECIES_SNORLAX, 50), SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .wantSeed = WantBothHit, .check = CheckRockSlideDoubles },
    { .name = "doubles_earthquake_not_halved",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M1(SPECIES_GOLEM, 50, MOVE_EARTHQUAKE), SPLASHER(SPECIES_PIDGEY, 50) },
      .enemy = { SPLASHER(SPECIES_SNORLAX, 50), SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T4(0, 0, 0, 0) }, .turns = 1, .wantSeed = WantBothHit, .check = CheckEarthquakeDoubles },
    { .name = "doubles_helping_hand_1_5x",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { M1(SPECIES_MACHOP, 50, MOVE_STRENGTH), M1(SPECIES_CLEFABLE, 50, MOVE_HELPING_HAND) },
      .enemy = { SPLASHER(SPECIES_SNORLAX, 50), SPLASHER(SPECIES_SNORLAX, 50) },
      .actions = { T4(0, 0, 0, 0) }, .targets = { { 2, 0, 0, 0 } }, .turns = 1, .wantSeed = Want_helping_hand, .check = Check_helping_hand },
};

SCENARIO_GROUP(special_damage, sScenarios)
