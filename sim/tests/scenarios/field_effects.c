// Weather and field/side effects: Rain Dance/Sunny Day/Sandstorm/Hail, Drizzle/Drought/Sand Stream,
// weather chip damage and multipliers, Thunder/Solar Beam/Synthesis/Weather Ball in weather, Castform,
// Cloud Nine/Air Lock, Swift Swim/Chlorophyll/Rain Dish, Reflect/Light Screen/Brick Break, Spikes/Rapid
// Spin, Mist, Safeguard, Mud/Water Sport, Secret Power/Nature Power/Camouflage on grass, end-of-turn order.
//
// Gen 3 rules exercised here (sim/src/battle_util.c, battle_script_commands.c, pokemon.c):
//  - weather moves set X_TEMPORARY with duration 5, counted down in the field end-turn slot of the same
//    turn: the weather ends at the end of the 5th turn.  A move fails only against the same weather kind;
//    any other weather (even a permanent ability weather) is replaced outright.
//  - sandstorm/hail deal maxHP/16 each end of turn while they continue; on the turn they subside there is
//    no damage.  Rock/Ground/Steel + Sand Veil + underground/underwater ignore sand, Ice ignores hail.
//  - field end-turn effects (screens, safeguard, mist, weather) run before battler effects (Rain Dish,
//    Leftovers, Leech Seed, poison ...).
//  - Cloud Nine/Air Lock leave the weather flags alone (they keep counting down and printing) but every
//    WEATHER_HAS_EFFECT check fails: no chip, no multipliers, Thunder/Solar Beam/Synthesis/Weather Ball/
//    Castform/Swift Swim/Chlorophyll/Rain Dish behave as in clear weather.
#include "scenario.h"

// ---------------------------------------------------------------------------------------------------
// Damage range helper: replicates CalculateBaseDamage + typecalc for a non-crit hit and returns the
// [85%, 100%] random range.  The player's side has all badges (+10% atk/def/spatk/spdef).
// ---------------------------------------------------------------------------------------------------
enum { WM_NONE, WM_BOOST, WM_HALVE };
struct Dmg
{
    u8 power, special, stab;
    u8 tnum, tden;       // type effectiveness as a fraction
    u8 screen, dscreen;  // Reflect/Light Screen active; dscreen = doubles with 2 alive defenders
    u8 weather;          // WM_*
    u8 solar;            // Solar Beam halving (rain/sand/hail)
    u8 mult;             // crit or dmgMultiplier (0 = 1)
};
static void DmgRange(struct BattleSim *sim, int a, int d, const struct Dmg *s, int *lo, int *hi)
{
    const struct BattlePokemon *atk = &sim->battleMons[a], *def = &sim->battleMons[d];
    u32 atkStat = s->special ? atk->spAttack : atk->attack;
    u32 defStat = s->special ? def->spDefense : def->defense;
    int atkStage = atk->statStages[s->special ? STAT_SPATK : STAT_ATK];
    int defStage = def->statStages[s->special ? STAT_SPDEF : STAT_DEF];
    s32 dmg, dh;

    if (!(a & 1)) atkStat = (110 * atkStat) / 100;
    if (!(d & 1)) defStat = (110 * defStat) / 100;
    dmg = atkStat * gStatStageRatios[atkStage][0] / gStatStageRatios[atkStage][1];
    dmg = dmg * s->power;
    dmg *= (2 * atk->level / 5 + 2);
    dh = defStat * gStatStageRatios[defStage][0] / gStatStageRatios[defStage][1];
    dmg = dmg / dh;
    dmg /= 50;
    if (s->screen)
        dmg = s->dscreen ? 2 * (dmg / 3) : dmg / 2;
    if (!s->special && dmg == 0)
        dmg = 1;
    if (s->weather == WM_BOOST) dmg = (15 * dmg) / 10;
    else if (s->weather == WM_HALVE) dmg /= 2;
    if (s->solar) dmg /= 2;
    dmg += 2;
    dmg *= s->mult ? s->mult : 1;
    if (s->stab) dmg = dmg * 15 / 10;
    dmg = dmg * s->tnum / s->tden;
    if (dmg == 0) dmg = 1;
    *hi = dmg;
    *lo = dmg * 85 / 100;
    if (*lo == 0) *lo = 1;
}
#define CHECK_DMG_RANGE(a, d, dealt, spec, what) do { int lo_, hi_; DmgRange(sim, a, d, &(spec), &lo_, &hi_); \
    CHECK((dealt) >= lo_ && (dealt) <= hi_, what ": dealt %d, expected %d..%d", (int)(dealt), lo_, hi_); } while (0)
#define CHECK_DMG_BELOW(a, d, dealt, spec, what) do { int lo_, hi_; DmgRange(sim, a, d, &(spec), &lo_, &hi_); \
    CHECK((dealt) < lo_, what ": dealt %d, should be below %d", (int)(dealt), lo_); } while (0)
#define CHECK_DMG_ABOVE(a, d, dealt, spec, what) do { int lo_, hi_; DmgRange(sim, a, d, &(spec), &lo_, &hi_); \
    CHECK((dealt) > hi_, what ": dealt %d, should be above %d", (int)(dealt), hi_); } while (0)
static int WantNoCrit(struct BattleSim *sim) { return !Sc_LogHas(sim, STRINGID_CRITICALHIT, SC_ANY_TURN); }
#define PARTY_MAXHP(side, i) ((int)GetMonData(&(side == B_SIDE_PLAYER ? sim->playerParty : sim->enemyParty)[i], MON_DATA_MAX_HP))
#define SPL MOVE_SPLASH
#define CHIP(b) (MAXHP(b) / 16)
#define TYPES(b, t) (B(b).type1 == (t) && B(b).type2 == (t))

// ================================================= weather moves ===================================

static void CheckRainDance5(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_STARTEDTORAIN, 0), "rain started on turn 1");
    CHECK(LOG_COUNT(STRINGID_RAINCONTINUES) == 4, "rain continues at the end of turns 1-4 (got %d)", LOG_COUNT(STRINGID_RAINCONTINUES));
    CHECK(LOG_HAS_T(STRINGID_RAINSTOPPED, 4), "rain stopped at the end of turn 5");
    CHECK(WEATHER() == 0, "no weather left (flags %#x)", WEATHER());
    CHECK(sim->wishFutureKnock.weatherDuration == 0, "duration 0");
}

static void CheckSunnyDay5(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_SUNLIGHTGOTBRIGHT, 0), "sun started");
    CHECK(LOG_COUNT(STRINGID_SUNLIGHTSTRONG) == 4, "sun continues 4 times (got %d)", LOG_COUNT(STRINGID_SUNLIGHTSTRONG));
    CHECK(LOG_HAS_T(STRINGID_SUNLIGHTFADED, 4), "sun faded at the end of turn 5");
    CHECK(WEATHER() == 0, "no weather left (flags %#x)", WEATHER());
}

static void CheckSandstorm5(struct BattleSim *sim)
{
    // Golem (Rock/Ground) is immune; Snorlax takes 1/16 at the end of turns 1-4 but not on the turn the
    // storm subsides.
    CHECK(LOG_HAS_T(STRINGID_SANDSTORMBREWED, 0), "sandstorm brewed");
    CHECK(LOG_COUNT(STRINGID_SANDSTORMRAGES) == 4, "sandstorm rages 4 times (got %d)", LOG_COUNT(STRINGID_SANDSTORMRAGES));
    CHECK(LOG_HAS_T(STRINGID_SANDSTORMSUBSIDED, 4), "subsided at the end of turn 5");
    CHECK(!LOG_HAS_T(STRINGID_PKMNBUFFETEDBYSANDSTORM, 4), "no chip on the turn it subsides");
    CHECK(LOG_COUNT(STRINGID_PKMNBUFFETEDBYSANDSTORM) == 4, "4 chip messages (got %d)", LOG_COUNT(STRINGID_PKMNBUFFETEDBYSANDSTORM));
    CHECK(HP(1) == MAXHP(1) - 4 * CHIP(1), "snorlax took 4 x 1/16 (hp %d/%d)", HP(1), MAXHP(1));
    CHECK(HP(0) == MAXHP(0), "golem untouched");
    CHECK(WEATHER() == 0, "no weather left (flags %#x)", WEATHER());
}

static void CheckHail5(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_STARTEDHAIL, 0), "hail started");
    CHECK(LOG_COUNT(STRINGID_HAILCONTINUES) == 4, "hail continues 4 times (got %d)", LOG_COUNT(STRINGID_HAILCONTINUES));
    CHECK(LOG_HAS_T(STRINGID_HAILSTOPPED, 4), "hail stopped at the end of turn 5");
    CHECK(LOG_COUNT(STRINGID_PKMNPELTEDBYHAIL) == 4, "4 pelted messages (got %d)", LOG_COUNT(STRINGID_PKMNPELTEDBYHAIL));
    CHECK(HP(1) == MAXHP(1) - 4 * CHIP(1), "snorlax took 4 x 1/16 (hp %d/%d)", HP(1), MAXHP(1));
    CHECK(HP(0) == MAXHP(0), "glalie (ice) untouched");
    CHECK(WEATHER() == 0, "no weather left (flags %#x)", WEATHER());
}

static void CheckWeatherReplaces(struct BattleSim *sim)
{
    // rain -> sun -> sand -> hail, each replacing the previous one and resetting the 5-turn timer.
    CHECK(LOG_HAS_T(STRINGID_STARTEDTORAIN, 0) && LOG_HAS_T(STRINGID_SUNLIGHTGOTBRIGHT, 1)
       && LOG_HAS_T(STRINGID_SANDSTORMBREWED, 2) && LOG_HAS_T(STRINGID_STARTEDHAIL, 3), "each weather started");
    CHECK(!LOG_HAS(STRINGID_BUTITFAILED), "none failed");
    CHECK(LOG_COUNT(STRINGID_RAINCONTINUES) == 1 && LOG_COUNT(STRINGID_SUNLIGHTSTRONG) == 1
       && LOG_COUNT(STRINGID_SANDSTORMRAGES) == 1 && LOG_COUNT(STRINGID_HAILCONTINUES) == 1, "each weather continued exactly once");
    CHECK(WEATHER() == B_WEATHER_HAIL_TEMPORARY, "hail only (flags %#x)", WEATHER());
    CHECK(sim->wishFutureKnock.weatherDuration == 4, "timer reset by hail (got %d)", sim->wishFutureKnock.weatherDuration);
    CHECK(HP(0) == MAXHP(0) - CHIP(0), "lapras: sand chip on turn 3, immune to hail (hp %d/%d)", HP(0), MAXHP(0));
    CHECK(HP(1) == MAXHP(1) - 2 * CHIP(1), "snorlax: sand + hail chip (hp %d/%d)", HP(1), MAXHP(1));
}

static void CheckSameWeatherFails(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 1), "second rain dance failed");
    CHECK(WEATHER() == B_WEATHER_RAIN_TEMPORARY, "temporary rain only (flags %#x)", WEATHER());
    CHECK(sim->wishFutureKnock.weatherDuration == 3, "timer not reset (got %d)", sim->wishFutureKnock.weatherDuration);
}

static void CheckSandImmuneTypes(struct BattleSim *sim)
{
    // Skarmory (Steel/Flying) and Marowak (pure Ground, Rock Head: no Sand Veil) take no chip.
    CHECK(HP(0) == MAXHP(0) && HP(1) == MAXHP(1), "steel and ground types take no chip (hp %d/%d, %d/%d)", HP(0), MAXHP(0), HP(1), MAXHP(1));
    CHECK(!LOG_HAS(STRINGID_PKMNBUFFETEDBYSANDSTORM), "no chip message");
    CHECK(LOG_HAS_T(STRINGID_SANDSTORMRAGES, 0), "storm still rages");
}

static void CheckSandVeil(struct BattleSim *sim)
{
    CHECK(HP(0) == MAXHP(0), "sand veil user (grass/dark) untouched");
    CHECK(HP(1) == MAXHP(1) - CHIP(1), "rattata took 1/16 (hp %d/%d)", HP(1), MAXHP(1));
}

static void CheckHailVsSandImmunity(struct BattleSim *sim)
{
    // Jynx (Ice) is hit by sand on turn 1 but not by hail on turn 2; Golem (Rock/Ground) the reverse.
    CHECK(HP(0) == MAXHP(0) - CHIP(0), "jynx: one sand chip (hp %d/%d)", HP(0), MAXHP(0));
    CHECK(HP(1) == MAXHP(1) - CHIP(1), "golem: one hail chip (hp %d/%d)", HP(1), MAXHP(1));
    CHECK(LOG_HAS_T(STRINGID_PKMNBUFFETEDBYSANDSTORM, 0) && LOG_HAS_T(STRINGID_PKMNPELTEDBYHAIL, 1), "messages");
    CHECK(WEATHER() == B_WEATHER_HAIL_TEMPORARY, "hail replaced sand (flags %#x)", WEATHER());
}

// ================================================= ability weather ===============================

static void CheckDrizzle(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNMADEITRAIN, 0), "drizzle message");
    CHECK(WEATHER() == (B_WEATHER_RAIN_PERMANENT | B_WEATHER_RAIN_TEMPORARY), "permanent+temporary rain (flags %#x)", WEATHER());
    CHECK(LOG_COUNT(STRINGID_RAINCONTINUES) == 6, "rain continues every turn (got %d)", LOG_COUNT(STRINGID_RAINCONTINUES));
    CHECK(!LOG_HAS(STRINGID_RAINSTOPPED), "never stops");
}

static void CheckRainDanceVsDrizzle(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 0), "rain dance fails in drizzle rain");
    CHECK(WEATHER() == (B_WEATHER_RAIN_PERMANENT | B_WEATHER_RAIN_TEMPORARY), "rain untouched (flags %#x)", WEATHER());
}

static void CheckSunnyDayVsDrizzle(struct BattleSim *sim)
{
    // Sunny Day overwrites the permanent rain; when the sun fades nothing brings the rain back.
    CHECK(LOG_HAS_T(STRINGID_SUNLIGHTGOTBRIGHT, 0), "sun replaced drizzle rain");
    CHECK(!LOG_HAS_T(STRINGID_RAINCONTINUES, 0), "no rain message once replaced");
    CHECK(LOG_HAS_T(STRINGID_SUNLIGHTFADED, 4), "sun faded on turn 5");
    CHECK(WEATHER() == 0, "clear afterwards (flags %#x)", WEATHER());
}

static void CheckSandStream(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNSXWHIPPEDUPSANDSTORM, 0), "sand stream message");
    CHECK(WEATHER() == B_WEATHER_SANDSTORM, "permanent sandstorm (flags %#x)", WEATHER());
    CHECK(LOG_COUNT(STRINGID_SANDSTORMRAGES) == 3, "rages every turn (got %d)", LOG_COUNT(STRINGID_SANDSTORMRAGES));
    CHECK(HP(0) == MAXHP(0) - 3 * CHIP(0), "snorlax 3 chips (hp %d/%d)", HP(0), MAXHP(0));
    CHECK(HP(1) == MAXHP(1), "tyranitar (rock) untouched");
}

static void CheckRainReplacesSandStream(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_RAINCONTINUES, 0) && !LOG_HAS_T(STRINGID_SANDSTORMRAGES, 0), "rain replaced the sandstorm on turn 1");
    CHECK(!LOG_HAS(STRINGID_PKMNBUFFETEDBYSANDSTORM), "no sand chip at all");
    CHECK(HP(0) == MAXHP(0), "lapras untouched");
    CHECK(LOG_HAS_T(STRINGID_RAINSTOPPED, 4), "rain stopped on turn 5");
    CHECK(WEATHER() == 0, "sandstorm does not return (flags %#x)", WEATHER());
}

static void CheckSandStreamSwitchIn(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_RAINCONTINUES, 0), "rain on turn 1");
    CHECK(LOG_HAS_T(STRINGID_PKMNSXWHIPPEDUPSANDSTORM, 1), "sand stream on switch-in");
    CHECK(WEATHER() == B_WEATHER_SANDSTORM, "sandstorm replaced rain (flags %#x)", WEATHER());
    CHECK(LOG_HAS_T(STRINGID_SANDSTORMRAGES, 1) && HP(0) == MAXHP(0) - CHIP(0), "sand chip at the end of turn 2 (hp %d/%d)", HP(0), MAXHP(0));
}

static void CheckDroughtSolarBeam(struct BattleSim *sim)
{
    struct Dmg s = { .power = 120, .special = 1, .stab = 1, .tnum = 2, .tden = 1 };
    CHECK(LOG_HAS_T(STRINGID_PKMNSXINTENSIFIEDSUN, 0), "drought message");
    CHECK(WEATHER() == B_WEATHER_SUN, "permanent sun (flags %#x)", WEATHER());
    CHECK(!LOG_HAS(STRINGID_PKMNTOOKSUNLIGHT), "no charge turn in sun");
    CHECK_DMG_RANGE(0, 1, MAXHP(1) - HP(1), s, "solar beam fired at full power");
}

// ================================================= multipliers ====================================

static void CheckRainBoostsWater(struct BattleSim *sim)
{
    struct Dmg s = { .power = 95, .special = 1, .stab = 1, .tnum = 1, .tden = 1, .weather = WM_BOOST };
    struct Dmg plain = s; plain.weather = WM_NONE;
    CHECK_DMG_RANGE(0, 1, MAXHP(1) - HP(1), s, "surf x1.5 in rain");
    CHECK_DMG_ABOVE(0, 1, MAXHP(1) - HP(1), plain, "more than clear-weather surf");
}

static void CheckRainHalvesFire(struct BattleSim *sim)
{
    struct Dmg s = { .power = 95, .special = 1, .stab = 1, .tnum = 1, .tden = 2, .weather = WM_HALVE };
    struct Dmg plain = s; plain.weather = WM_NONE;
    CHECK_DMG_RANGE(0, 1, MAXHP(1) - HP(1), s, "flamethrower halved in rain (and resisted)");
    CHECK_DMG_BELOW(0, 1, MAXHP(1) - HP(1), plain, "less than clear-weather flamethrower");
}

static void CheckSunBoostsFire(struct BattleSim *sim)
{
    struct Dmg s = { .power = 95, .special = 1, .stab = 1, .tnum = 1, .tden = 1, .weather = WM_BOOST };
    struct Dmg plain = s; plain.weather = WM_NONE;
    CHECK_DMG_RANGE(0, 1, MAXHP(1) - HP(1), s, "flamethrower x1.5 in sun");
    CHECK_DMG_ABOVE(0, 1, MAXHP(1) - HP(1), plain, "more than clear-weather flamethrower");
}

static void CheckSunHalvesWater(struct BattleSim *sim)
{
    struct Dmg s = { .power = 95, .special = 1, .stab = 1, .tnum = 2, .tden = 1, .weather = WM_HALVE };
    struct Dmg plain = s; plain.weather = WM_NONE;
    CHECK_DMG_RANGE(0, 1, MAXHP(1) - HP(1), s, "surf halved in sun (super effective on groudon)");
    CHECK_DMG_BELOW(0, 1, MAXHP(1) - HP(1), plain, "less than clear-weather surf");
}

// ================================================= thunder ========================================

static void CheckThunderRain(struct BattleSim *sim)
{
    // 10 Thunders (70% accuracy, all of its PP) under Drizzle: none may miss.
    CHECK(LOG_COUNT(STRINGID_ATTACKMISSED) == 0, "no misses in rain (got %d)", LOG_COUNT(STRINGID_ATTACKMISSED));
    CHECK(LOG_COUNT(STRINGID_USEDMOVE) >= 10, "thunder used every turn");
    CHECK(PP(0, 0) == 0, "all 10 pp used (got %d)", PP(0, 0));
    CHECK(HP(1) < MAXHP(1), "kyogre damaged");
}

static int WantAMiss(struct BattleSim *sim) { return Sc_LogCount(sim, STRINGID_ATTACKMISSED, SC_ANY_TURN) >= 1; }
static void CheckThunderSun(struct BattleSim *sim)
{
    // Thunder's accuracy drops to 50 in sun; with this seed at least one of three misses.
    CHECK(WEATHER() & B_WEATHER_SUN, "sun up");
    CHECK(LOG_COUNT(STRINGID_ATTACKMISSED) >= 1, "thunder can miss in sun");
}

static void CheckThunderRainCloudNine(struct BattleSim *sim)
{
    // Cloud Nine: rain flags stay but Thunder's sure-hit is gone.
    CHECK(WEATHER() == (B_WEATHER_RAIN_PERMANENT | B_WEATHER_RAIN_TEMPORARY), "rain flags kept (flags %#x)", WEATHER());
    CHECK(LOG_COUNT(STRINGID_RAINCONTINUES) == 6, "rain still announced each turn");
    CHECK(LOG_COUNT(STRINGID_ATTACKMISSED) >= 1, "thunder missed despite rain");
}

// ================================================= solar beam =====================================

static void CheckSolarBeamRain(struct BattleSim *sim)
{
    struct Dmg s = { .power = 120, .special = 1, .stab = 1, .tnum = 2, .tden = 1, .solar = 1 };
    struct Dmg full = s; full.solar = 0;
    CHECK(LOG_HAS_T(STRINGID_PKMNTOOKSUNLIGHT, 0), "charged on turn 1");
    CHECK_DMG_RANGE(0, 1, MAXHP(1) - HP(1), s, "solar beam halved in rain");
    CHECK_DMG_BELOW(0, 1, MAXHP(1) - HP(1), full, "less than full solar beam");
}

static void CheckSolarBeamSand(struct BattleSim *sim)
{
    struct Dmg s = { .power = 120, .special = 1, .stab = 1, .tnum = 2, .tden = 1, .solar = 1 };
    CHECK(LOG_HAS_T(STRINGID_PKMNTOOKSUNLIGHT, 0), "charged on turn 1");
    CHECK(HP(0) == MAXHP(0) - 2 * CHIP(0), "venusaur took 2 sand chips (hp %d/%d)", HP(0), MAXHP(0));
    CHECK_DMG_RANGE(0, 1, MAXHP(1) - HP(1), s, "solar beam halved in sandstorm");
}

static void CheckSolarBeamHail(struct BattleSim *sim)
{
    struct Dmg s = { .power = 120, .special = 1, .stab = 1, .tnum = 1, .tden = 1, .solar = 1 };
    int dealt = MAXHP(1) - 3 * CHIP(1) - HP(1);
    CHECK(LOG_HAS_T(STRINGID_PKMNTOOKSUNLIGHT, 1), "charged on turn 2");
    CHECK(HP(0) == MAXHP(0) - 3 * CHIP(0), "venusaur 3 hail chips (hp %d/%d)", HP(0), MAXHP(0));
    CHECK_DMG_RANGE(0, 1, dealt, s, "solar beam halved in hail");
}

static void CheckSolarBeamAirLock(struct BattleSim *sim)
{
    // Sun is up but Rayquaza's Air Lock makes Solar Beam charge; it then fires unhalved (x0.25 typing).
    struct Dmg s = { .power = 120, .special = 1, .stab = 1, .tnum = 1, .tden = 4 };
    CHECK(WEATHER() == B_WEATHER_SUN_TEMPORARY, "sun flags kept (flags %#x)", WEATHER());
    CHECK(LOG_HAS_T(STRINGID_PKMNTOOKSUNLIGHT, 1), "charged despite sun");
    CHECK(!LOG_HAS_MOVE_T(STRINGID_PKMNTOOKSUNLIGHT, MOVE_SOLAR_BEAM, 2), "fired on turn 3");
    CHECK_DMG_RANGE(0, 1, MAXHP(1) - HP(1), s, "single unhalved hit");
}

// ================================================= sunlight heals =================================

static void CheckSynthesisClear(struct BattleSim *sim)
{
    CHECK(HP(0) == 1 + MAXHP(0) / 2, "clear weather: +1/2 (hp %d/%d)", HP(0), MAXHP(0));
    CHECK(LOG_HAS(STRINGID_PKMNREGAINEDHEALTH), "heal message");
}
static void CheckSynthesisSun(struct BattleSim *sim)
{
    CHECK(HP(0) == 1 + 20 * MAXHP(0) / 30, "sun: +20/30 (hp %d/%d)", HP(0), MAXHP(0));
}
static void CheckSynthesisRain(struct BattleSim *sim)
{
    CHECK(HP(0) == 1 + MAXHP(0) / 4, "rain: +1/4 (hp %d/%d)", HP(0), MAXHP(0));
}
static void CheckMoonlightSand(struct BattleSim *sim)
{
    CHECK(HP(0) == 1 + MAXHP(0) / 4 - CHIP(0), "sand: +1/4 then 1/16 chip (hp %d/%d)", HP(0), MAXHP(0));
}
static void CheckMorningSunHail(struct BattleSim *sim)
{
    CHECK(HP(0) == 40 - CHIP(0) + MAXHP(0) / 4 - CHIP(0), "hail: chip, +1/4, chip (hp %d/%d)", HP(0), MAXHP(0));
}
static void CheckSynthesisCloudNine(struct BattleSim *sim)
{
    CHECK(WEATHER() == B_WEATHER_SUN_TEMPORARY, "sun flags kept");
    CHECK(HP(0) == 1 + MAXHP(0) / 2, "cloud nine: sun ignored, +1/2 (hp %d/%d)", HP(0), MAXHP(0));
}
static void CheckSynthesisFull(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNHPFULL), "hp full message");
    CHECK(PP(0, 0) == 4, "pp still deducted (got %d)", PP(0, 0));
}

// ================================================= weather ball ===================================

static void CheckWeatherBallRain(struct BattleSim *sim)
{
    // Water-type, power x2, rain boost, STAB (Lapras), resisted by Kyogre.
    struct Dmg s = { .power = 50, .special = 1, .stab = 1, .tnum = 1, .tden = 2, .weather = WM_BOOST, .mult = 2 };
    CHECK_DMG_RANGE(0, 1, MAXHP(1) - HP(1), s, "weather ball water in rain");
}
static void CheckWeatherBallSun(struct BattleSim *sim)
{
    struct Dmg s = { .power = 50, .special = 1, .stab = 0, .tnum = 1, .tden = 1, .weather = WM_BOOST, .mult = 2 };
    struct Dmg normal = { .power = 50, .special = 0, .stab = 0, .tnum = 1, .tden = 1, .mult = 2 };   // Normal is physical
    CHECK_DMG_RANGE(0, 1, MAXHP(1) - HP(1), s, "weather ball fire in sun");
    CHECK_DMG_ABOVE(0, 1, MAXHP(1) - HP(1), normal, "more than a normal-type x2 hit");
}
static void CheckWeatherBallSand(struct BattleSim *sim)
{
    // Rock (and Normal) are physical types in Gen 3.
    struct Dmg s = { .power = 50, .special = 0, .stab = 0, .tnum = 1, .tden = 1, .mult = 2 };
    struct Dmg normal = { .power = 50, .special = 0, .stab = 0, .tnum = 1, .tden = 2, .mult = 2 };
    CHECK_DMG_RANGE(0, 1, MAXHP(1) - HP(1), s, "weather ball rock in sandstorm (neutral on tyranitar)");
    CHECK_DMG_ABOVE(0, 1, MAXHP(1) - HP(1), normal, "more than a normal-type hit resisted by rock");
}
static void CheckWeatherBallHail(struct BattleSim *sim)
{
    struct Dmg s = { .power = 50, .special = 1, .stab = 1, .tnum = 1, .tden = 1, .mult = 2 };
    int dealt = MAXHP(1) - 2 * CHIP(1) - HP(1);
    CHECK(HP(0) == MAXHP(0), "lapras (ice) not pelted");
    CHECK_DMG_RANGE(0, 1, dealt, s, "weather ball ice in hail (STAB for lapras)");
}
static void CheckWeatherBallClear(struct BattleSim *sim)
{
    // No weather: setweatherballtype does nothing, so it is a plain 50-power physical Normal hit.
    struct Dmg s = { .power = 50, .special = 0, .stab = 0, .tnum = 1, .tden = 1 };
    struct Dmg doubled = s; doubled.mult = 2;
    CHECK(WEATHER() == 0, "clear");
    CHECK_DMG_RANGE(0, 1, MAXHP(1) - HP(1), s, "plain weather ball");
    CHECK_DMG_BELOW(0, 1, MAXHP(1) - HP(1), doubled, "not doubled");
}
static void CheckWeatherBallCloudNine(struct BattleSim *sim)
{
    // Stays Normal-type (physical), power 50, no multiplier, no STAB for Kyogre.
    struct Dmg s = { .power = 50, .special = 0, .stab = 0, .tnum = 1, .tden = 1 };
    CHECK(WEATHER() & B_WEATHER_RAIN, "drizzle rain flags present");
    CHECK_DMG_RANGE(0, 1, MAXHP(1) - HP(1), s, "plain 50 power normal weather ball under cloud nine");
}

// ================================================= castform =======================================

static void CheckCastformFire(struct BattleSim *sim) { CHECK(TYPES(0, TYPE_FIRE), "sun form (types %d/%d)", B(0).type1, B(0).type2); }
static void CheckCastformWater(struct BattleSim *sim) { CHECK(TYPES(0, TYPE_WATER), "rain form (types %d/%d)", B(0).type1, B(0).type2); }
static void CheckCastformIce(struct BattleSim *sim)
{
    CHECK(TYPES(0, TYPE_ICE), "hail form (types %d/%d)", B(0).type1, B(0).type2);
    CHECK(HP(0) == MAXHP(0), "ice form immune to hail");
    CHECK(HP(1) == MAXHP(1) - CHIP(1), "snorlax pelted");
}
static void CheckCastformSand(struct BattleSim *sim)
{
    CHECK(TYPES(0, TYPE_NORMAL), "back to normal in sandstorm (types %d/%d)", B(0).type1, B(0).type2);
    CHECK(HP(0) == MAXHP(0) - CHIP(0), "normal castform takes sand chip");
}
static void CheckCastformReverts(struct BattleSim *sim)
{
    CHECK(WEATHER() == 0, "sun gone");
    CHECK(TYPES(0, TYPE_NORMAL), "normal again after the sun fades (types %d/%d)", B(0).type1, B(0).type2);
}
static void CheckCastformCloudNine(struct BattleSim *sim)
{
    CHECK(WEATHER() == B_WEATHER_SUN_TEMPORARY, "sun set");
    CHECK(TYPES(0, TYPE_NORMAL), "no form change under cloud nine (types %d/%d)", B(0).type1, B(0).type2);
}
static void CheckCastformSwitchIn(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_CASTFORM, "castform in");
    CHECK(TYPES(0, TYPE_WATER), "water form on switch-in to drizzle rain (types %d/%d)", B(0).type1, B(0).type2);
}

// ================================================= cloud nine / speed abilities ==================

static void CheckCloudNineSand(struct BattleSim *sim)
{
    CHECK(WEATHER() == B_WEATHER_SANDSTORM, "sandstorm flags kept");
    CHECK(LOG_COUNT(STRINGID_SANDSTORMRAGES) == 2, "storm still announced");
    CHECK(!LOG_HAS(STRINGID_PKMNBUFFETEDBYSANDSTORM), "no chip messages");
    CHECK(HP(0) == MAXHP(0) && HP(1) == MAXHP(1), "nobody chipped");
}

static void CheckAirLockRain(struct BattleSim *sim)
{
    struct Dmg s = { .power = 95, .special = 1, .stab = 0, .tnum = 1, .tden = 1 };
    struct Dmg boosted = s; boosted.weather = WM_BOOST;
    CHECK(WEATHER() == B_WEATHER_RAIN_TEMPORARY, "rain set");
    CHECK_DMG_RANGE(0, 1, MAXHP(1) - HP(1), s, "surf unboosted under air lock");
    CHECK_DMG_BELOW(0, 1, MAXHP(1) - HP(1), boosted, "below the rain-boosted range");
}

static void CheckSwiftSwimOff(struct BattleSim *sim)
{
    CHECK(MOVED_BEFORE(1, 0, 0), "golduck faster on turn 1");
    CHECK(MOVED_BEFORE(1, 0, 1), "golduck still faster in rain (cloud nine)");
}
static void CheckSwiftSwim(struct BattleSim *sim)
{
    CHECK(MOVED_BEFORE(1, 0, 0), "raichu faster before rain");
    CHECK(MOVED_BEFORE(0, 1, 1), "ludicolo doubled speed in rain");
}
static void CheckChlorophyll(struct BattleSim *sim)
{
    CHECK(MOVED_BEFORE(1, 0, 0), "raichu faster before sun");
    CHECK(MOVED_BEFORE(0, 1, 1), "exeggutor doubled speed in sun");
}
static void CheckChlorophyllRain(struct BattleSim *sim)
{
    CHECK(MOVED_BEFORE(1, 0, 0) && MOVED_BEFORE(1, 0, 1), "chlorophyll does nothing in rain");
}

static void CheckRainDish(struct BattleSim *sim)
{
    CHECK(HP(0) == 50 + 2 * CHIP(0), "rain dish +1/16 per turn (hp %d/%d)", HP(0), MAXHP(0));
    CHECK(LOG_COUNT(STRINGID_PKMNSXRESTOREDHPALITTLE2) == 2, "two rain dish messages");
}
static void CheckRainDishCloudNine(struct BattleSim *sim)
{
    CHECK(WEATHER() == B_WEATHER_RAIN_TEMPORARY, "rain set");
    CHECK(HP(0) == 50, "no rain dish under cloud nine (hp %d)", HP(0));
    CHECK(!LOG_HAS(STRINGID_PKMNSXRESTOREDHPALITTLE2), "no message");
}
static void CheckRainDishNotWhenRainStops(struct BattleSim *sim)
{
    // Field effects (weather countdown) run before battler effects (Rain Dish): on the 5th turn the rain
    // is already gone when Rain Dish is checked, so only 4 heals.
    CHECK(LOG_HAS_T(STRINGID_RAINSTOPPED, 4), "rain stopped on turn 5");
    CHECK(LOG_COUNT(STRINGID_PKMNSXRESTOREDHPALITTLE2) == 4, "4 rain dish heals (got %d)", LOG_COUNT(STRINGID_PKMNSXRESTOREDHPALITTLE2));
    CHECK(!LOG_HAS_T(STRINGID_PKMNSXRESTOREDHPALITTLE2, 4), "no heal on the turn the rain stops");
    CHECK(HP(0) == 50 + 4 * CHIP(0), "hp 50 + 4/16 (hp %d/%d)", HP(0), MAXHP(0));
}
static void CheckRainDishBeforeLeftovers(struct BattleSim *sim)
{
    int dish = LOG_INDEX_T(STRINGID_PKMNSXRESTOREDHPALITTLE2, 0, 0);
    int left = LOG_INDEX_T(STRINGID_PKMNSITEMRESTOREDHPALITTLE, 0, 0);
    CHECK(HP(0) == MAXHP(0), "healed to full (hp %d/%d)", HP(0), MAXHP(0));
    CHECK(dish >= 0 && left >= 0 && dish < left, "ability heal before item heal (%d, %d)", dish, left);
}

// ================================================= screens ========================================

static void CheckReflectPhysical(struct BattleSim *sim)
{
    struct Dmg s = { .power = 80, .special = 0, .stab = 1, .tnum = 1, .tden = 1, .screen = 1 };
    struct Dmg plain = s; plain.screen = 0;
    CHECK(LOG_HAS_T(STRINGID_PKMNRAISEDDEF, 0), "reflect message");
    CHECK_DMG_RANGE(1, 0, MAXHP(0) - HP(0), s, "strength halved by reflect");
    CHECK_DMG_BELOW(1, 0, MAXHP(0) - HP(0), plain, "below unscreened");
}
static void CheckLightScreenSpecial(struct BattleSim *sim)
{
    struct Dmg s = { .power = 95, .special = 1, .stab = 0, .tnum = 2, .tden = 1, .screen = 1 };
    struct Dmg plain = s; plain.screen = 0;
    CHECK(LOG_HAS_T(STRINGID_PKMNRAISEDSPDEF, 0), "light screen message");
    CHECK_DMG_RANGE(1, 0, MAXHP(0) - HP(0), s, "thunderbolt halved by light screen");
    CHECK_DMG_BELOW(1, 0, MAXHP(0) - HP(0), plain, "below unscreened");
}
static void CheckLightScreenNotPhysical(struct BattleSim *sim)
{
    struct Dmg s = { .power = 80, .special = 0, .stab = 1, .tnum = 1, .tden = 1 };
    CHECK(SIDE_STATUS(B_SIDE_PLAYER) & SIDE_STATUS_LIGHTSCREEN, "light screen up");
    CHECK_DMG_RANGE(1, 0, MAXHP(0) - HP(0), s, "strength unaffected by light screen");
}
static void CheckReflectNotSpecial(struct BattleSim *sim)
{
    struct Dmg s = { .power = 95, .special = 1, .stab = 0, .tnum = 2, .tden = 1 };
    CHECK(SIDE_STATUS(B_SIDE_PLAYER) & SIDE_STATUS_REFLECT, "reflect up");
    CHECK_DMG_RANGE(1, 0, MAXHP(0) - HP(0), s, "thunderbolt unaffected by reflect");
}
static void CheckReflectExpires(struct BattleSim *sim)
{
    CHECK(!LOG_HAS_T(STRINGID_PKMNSXWOREOFF, 3), "still up after 4 turns");
    CHECK(LOG_HAS_T(STRINGID_PKMNSXWOREOFF, 4), "wore off at the end of turn 5");
    CHECK(!(SIDE_STATUS(B_SIDE_PLAYER) & SIDE_STATUS_REFLECT), "flag cleared");
    CHECK(sim->sideTimers[B_SIDE_PLAYER].reflectTimer == 0, "timer 0");
}
static void CheckReflectTwice(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 1), "second reflect fails");
    CHECK(sim->sideTimers[B_SIDE_PLAYER].reflectTimer == 3, "timer not reset (got %d)", sim->sideTimers[B_SIDE_PLAYER].reflectTimer);
}
static void CheckBrickBreak(struct BattleSim *sim)
{
    struct Dmg s = { .power = 75, .special = 0, .stab = 1, .tnum = 1, .tden = 2 };
    CHECK(LOG_HAS_T(STRINGID_THEWALLSHATTERED, 2), "wall shattered");
    CHECK(!(SIDE_STATUS(B_SIDE_PLAYER) & (SIDE_STATUS_REFLECT | SIDE_STATUS_LIGHTSCREEN)), "both screens gone");
    CHECK(sim->sideTimers[B_SIDE_PLAYER].reflectTimer == 0 && sim->sideTimers[B_SIDE_PLAYER].lightscreenTimer == 0, "timers 0");
    CHECK_DMG_RANGE(1, 0, MAXHP(0) - HP(0), s, "brick break did unscreened damage");
}
static void CheckBrickBreakNoScreen(struct BattleSim *sim)
{
    struct Dmg s = { .power = 75, .special = 0, .stab = 1, .tnum = 1, .tden = 2 };
    CHECK(!LOG_HAS(STRINGID_THEWALLSHATTERED), "no wall message");
    CHECK_DMG_RANGE(1, 0, MAXHP(0) - HP(0), s, "normal brick break damage");
}
static void CheckBrickBreakGhost(struct BattleSim *sim)
{
    // removelightscreenreflect runs before typecalc: the screens shatter even though Fighting cannot
    // touch a Ghost, and the hit itself does nothing.
    CHECK(LOG_HAS_T(STRINGID_THEWALLSHATTERED, 1), "wall shattered on a ghost");
    CHECK(LOG_HAS_T(STRINGID_ITDOESNTAFFECT, 1), "doesn't affect gengar");
    CHECK(HP(0) == MAXHP(0), "no damage (hp %d/%d)", HP(0), MAXHP(0));
    CHECK(!(SIDE_STATUS(B_SIDE_PLAYER) & (SIDE_STATUS_REFLECT | SIDE_STATUS_LIGHTSCREEN)), "screens gone");
    CHECK(sim->sideTimers[B_SIDE_PLAYER].reflectTimer == 0, "timer 0");
}
static void CheckReflectOwnSideOnly(struct BattleSim *sim)
{
    struct Dmg s = { .power = 80, .special = 0, .stab = 0, .tnum = 1, .tden = 1 };
    struct Dmg screened = s; screened.screen = 1;
    CHECK(SIDE_STATUS(B_SIDE_PLAYER) & SIDE_STATUS_REFLECT, "our reflect up");
    CHECK_DMG_RANGE(0, 1, MAXHP(1) - HP(1), s, "our strength unaffected by our own reflect");
    CHECK_DMG_ABOVE(0, 1, MAXHP(1) - HP(1), screened, "above the halved range");
}
static void CheckScreenSurvivesFaint(struct BattleSim *sim)
{
    struct Dmg s = { .power = 80, .special = 0, .stab = 0, .tnum = 1, .tden = 1, .screen = 1 };
    CHECK(B(0).species == SPECIES_SLOWBRO && PARTY_HP(B_SIDE_PLAYER, 0) == 0, "rattata fainted, slowbro in");
    CHECK(SIDE_STATUS(B_SIDE_PLAYER) & SIDE_STATUS_REFLECT, "reflect still up after the setter fainted");
    CHECK(sim->sideTimers[B_SIDE_PLAYER].reflectTimer == 2, "timer kept counting (got %d)", sim->sideTimers[B_SIDE_PLAYER].reflectTimer);
    CHECK_DMG_RANGE(1, 0, MAXHP(0) - HP(0), s, "strength on slowbro halved");
}
static void CheckReflectDoubles(struct BattleSim *sim)
{
    struct Dmg s = { .power = 80, .special = 0, .stab = 0, .tnum = 1, .tden = 1, .screen = 1, .dscreen = 1 };
    struct Dmg half = s; half.dscreen = 0;
    CHECK(LOG_HAS_T(STRINGID_PKMNRAISEDDEFALITTLE, 0), "doubles reflect message");
    CHECK_DMG_RANGE(1, 0, MAXHP(0) - HP(0), s, "reflect 2/3 in doubles");
    CHECK_DMG_ABOVE(1, 0, MAXHP(0) - HP(0), half, "more than singles halving");
    CHECK(HP(2) == MAXHP(2), "partner untouched");
}
static void CheckLightScreenDoubles(struct BattleSim *sim)
{
    struct Dmg s = { .power = 95, .special = 1, .stab = 1, .tnum = 2, .tden = 1, .screen = 1, .dscreen = 1 };
    struct Dmg half = s; half.dscreen = 0;
    CHECK(LOG_HAS_T(STRINGID_PKMNRAISEDSPDEFALITTLE, 0), "doubles light screen message");
    CHECK_DMG_RANGE(1, 0, MAXHP(0) - HP(0), s, "light screen 2/3 in doubles");
    CHECK_DMG_ABOVE(1, 0, MAXHP(0) - HP(0), half, "more than singles halving");
    CHECK(HP(2) == MAXHP(2), "partner untouched");
}
static void CheckReflectDoublesPartnerFainted(struct BattleSim *sim)
{
    // CountAliveMonsInBattle(BATTLE_ALIVE_DEF_SIDE) == 1 after the partner fainted: back to halving.
    struct Dmg s = { .power = 80, .special = 0, .stab = 0, .tnum = 1, .tden = 1, .screen = 1 };
    struct Dmg twoThirds = s; twoThirds.dscreen = 1;
    CHECK(PARTY_HP(B_SIDE_PLAYER, 1) == 0, "rattata partner fainted on turn 1");
    CHECK(SIDE_STATUS(B_SIDE_PLAYER) & SIDE_STATUS_REFLECT, "reflect up");
    CHECK_DMG_RANGE(1, 0, MAXHP(0) - HP(0), s, "reflect halves with one defender left");
    CHECK_DMG_BELOW(1, 0, MAXHP(0) - HP(0), twoThirds, "below the 2/3 range");
}
static int WantCritTurn1(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_CRITICALHIT, 1); }
static void CheckCritIgnoresReflect(struct BattleSim *sim)
{
    struct Dmg s = { .power = 70, .special = 0, .stab = 1, .tnum = 1, .tden = 1, .mult = 2 };
    CHECK(LOG_HAS_T(STRINGID_CRITICALHIT, 1), "crit landed");
    CHECK(SIDE_STATUS(B_SIDE_PLAYER) & SIDE_STATUS_REFLECT, "reflect up");
    CHECK_DMG_RANGE(1, 0, MAXHP(0) - HP(0), s, "crit slash ignores reflect");
}

// ================================================= spikes =========================================

static void CheckSpikes1(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_SPIKESSCATTERED, 0), "spikes scattered");
    CHECK(sim->sideTimers[B_SIDE_OPPONENT].spikesAmount == 1, "one layer");
    CHECK(B(1).species == SPECIES_SNORLAX && HP(1) == MAXHP(1) - MAXHP(1) / 8, "1/8 on switch-in (hp %d/%d)", HP(1), MAXHP(1));
    CHECK(LOG_HAS_T(STRINGID_PKMNHURTBYSPIKES, 1), "hurt by spikes message");
}
static void CheckSpikes2(struct BattleSim *sim)
{
    CHECK(sim->sideTimers[B_SIDE_OPPONENT].spikesAmount == 2, "two layers");
    CHECK(HP(1) == MAXHP(1) - MAXHP(1) / 6, "1/6 on switch-in (hp %d/%d)", HP(1), MAXHP(1));
}
static void CheckSpikes3(struct BattleSim *sim)
{
    CHECK(sim->sideTimers[B_SIDE_OPPONENT].spikesAmount == 3, "three layers");
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 3), "fourth layer fails");
    CHECK(PP(0, 0) == 16, "pp deducted for the failed spikes too (got %d)", PP(0, 0));
    CHECK(HP(1) == MAXHP(1) - MAXHP(1) / 4, "1/4 on switch-in (hp %d/%d)", HP(1), MAXHP(1));
}
static void CheckSpikesImmune(struct BattleSim *sim)
{
    CHECK(PARTY_HP(B_SIDE_OPPONENT, 1) == PARTY_MAXHP(B_SIDE_OPPONENT, 1), "skarmory (flying) took nothing");
    CHECK(PARTY_HP(B_SIDE_OPPONENT, 2) == PARTY_MAXHP(B_SIDE_OPPONENT, 2), "gengar (levitate) took nothing");
    CHECK(B(1).species == SPECIES_RATTATA && HP(1) == MAXHP(1) - MAXHP(1) / 8, "rattata took 1/8 when it came back (hp %d/%d)", HP(1), MAXHP(1));
    CHECK(LOG_COUNT(STRINGID_PKMNHURTBYSPIKES) == 1, "one spikes hit (got %d)", LOG_COUNT(STRINGID_PKMNHURTBYSPIKES));
}
static int WantEnemyLeadFainted(struct BattleSim *sim) { return PARTY_HP(B_SIDE_OPPONENT, 0) == 0; }
static void CheckSpikesReplacement(struct BattleSim *sim)
{
    CHECK(B(1).species == SPECIES_SNORLAX, "replacement sent out");
    CHECK(HP(1) == MAXHP(1) - MAXHP(1) / 8, "replacement after a KO takes spikes (hp %d/%d)", HP(1), MAXHP(1));
}
static void CheckRapidSpinSpikes(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNBLEWAWAYSPIKES, 1), "blew away spikes");
    CHECK(!(SIDE_STATUS(B_SIDE_PLAYER) & SIDE_STATUS_SPIKES) && sim->sideTimers[B_SIDE_PLAYER].spikesAmount == 0, "spikes gone");
}
static int WantShedSeedTurn3(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_PKMNSHEDLEECHSEED, 2); }
static void CheckRapidSpinSeedFirst(struct BattleSim *sim)
{
    // rapidspinfree pushes its own cursor, so after each "freed" script it runs again: one Rapid Spin
    // clears leech seed and then the spikes, in that order.
    int seed = LOG_INDEX_T(STRINGID_PKMNSHEDLEECHSEED, 0, 2), spikes = LOG_INDEX_T(STRINGID_PKMNBLEWAWAYSPIKES, 0, 2);
    CHECK(seed >= 0 && spikes >= 0 && seed < spikes, "one spin: leech seed then spikes (%d, %d)", seed, spikes);
    CHECK(!(STATUS3(0) & STATUS3_LEECHSEED) && sim->sideTimers[B_SIDE_PLAYER].spikesAmount == 0
       && !(SIDE_STATUS(B_SIDE_PLAYER) & SIDE_STATUS_SPIKES), "both cleared");
}
static int WantGotFreeTurn3(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_PKMNGOTFREE, 2); }
static void CheckRapidSpinWrapFirst(struct BattleSim *sim)
{
    int wrap = LOG_INDEX_T(STRINGID_PKMNGOTFREE, 0, 2), spikes = LOG_INDEX_T(STRINGID_PKMNBLEWAWAYSPIKES, 0, 2);
    CHECK(wrap >= 0 && spikes >= 0 && wrap < spikes, "one spin: wrap then spikes (%d, %d)", wrap, spikes);
    CHECK(!(STATUS2(0) & STATUS2_WRAPPED), "not wrapped");
    CHECK(!(SIDE_STATUS(B_SIDE_PLAYER) & SIDE_STATUS_SPIKES) && sim->sideTimers[B_SIDE_PLAYER].spikesAmount == 0, "spikes gone");
}

// ================================================= mist ===========================================

static void CheckMistGrowl(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNSHROUDEDINMIST, 0), "mist message");
    CHECK(LOG_HAS_T(STRINGID_PKMNPROTECTEDBYMIST, 1), "protected by mist");
    CHECK(STAGE(0, STAT_ATK) == DEFAULT_STAT_STAGE, "attack not lowered");
}
static void CheckMistIntimidate(struct BattleSim *sim)
{
    CHECK(B(1).species == SPECIES_ARCANINE, "arcanine in");
    CHECK(LOG_HAS_T(STRINGID_PKMNPROTECTEDBYMIST, 1), "intimidate blocked by mist");
    CHECK(STAGE(0, STAT_ATK) == DEFAULT_STAT_STAGE, "attack not lowered");
}
static void CheckMistSuperpower(struct BattleSim *sim)
{
    CHECK(STAGE(0, STAT_ATK) == DEFAULT_STAT_STAGE - 1 && STAGE(0, STAT_DEF) == DEFAULT_STAT_STAGE - 1,
          "own superpower drop goes through mist (atk %d def %d)", STAGE(0, STAT_ATK), STAGE(0, STAT_DEF));
    CHECK(!LOG_HAS(STRINGID_PKMNPROTECTEDBYMIST), "no mist message");
}
static void CheckMistNotFoe(struct BattleSim *sim)
{
    CHECK(STAGE(1, STAT_ATK) == DEFAULT_STAT_STAGE - 1, "foe's attack lowered despite our mist");
}
static void CheckMistExpiry(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 1), "second mist fails");
    CHECK(LOG_HAS_T(STRINGID_PKMNPROTECTEDBYMIST, 4) && STAGE(0, STAT_ATK) == DEFAULT_STAT_STAGE, "still protects on turn 5");
    CHECK(LOG_HAS_T(STRINGID_PKMNSXWOREOFF, 4), "wore off at the end of turn 5");
    CHECK(sim->sideTimers[B_SIDE_PLAYER].mistTimer == 0 && !(SIDE_STATUS(B_SIDE_PLAYER) & SIDE_STATUS_MIST), "cleared");
}

// ================================================= safeguard ======================================

static int WantSafeguard3(struct BattleSim *sim) { return Sc_LogCount(sim, STRINGID_PKMNUSEDSAFEGUARD, SC_ANY_TURN) == 3; }
static void CheckSafeguardStatusMoves(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNCOVEREDBYVEIL, 0), "safeguard message");
    CHECK(LOG_COUNT(STRINGID_PKMNUSEDSAFEGUARD) == 3, "thunder wave, toxic, swagger's confusion blocked");
    CHECK(STATUS1(0) == 0, "no status");
    CHECK(!(STATUS2(0) & STATUS2_CONFUSION), "not confused");
    CHECK(STAGE(0, STAT_ATK) == DEFAULT_STAT_STAGE + 2, "swagger's attack boost still applied (stage %d)", STAGE(0, STAT_ATK));
}
static int WantHitNoMiss(struct BattleSim *sim) { return Sc_LogCount(sim, STRINGID_ATTACKMISSED, SC_ANY_TURN) == 0 && HP(0) < MAXHP(0); }
static void CheckSafeguardZapCannon(struct BattleSim *sim)
{
    CHECK(HP(0) < MAXHP(0), "zap cannon hit");
    CHECK(!(STATUS1(0) & STATUS1_PARALYSIS), "100%% paralysis blocked by safeguard");
}
static void CheckSafeguardDynamicPunch(struct BattleSim *sim)
{
    CHECK(HP(0) < MAXHP(0), "dynamic punch hit");
    CHECK(!(STATUS2(0) & STATUS2_CONFUSION), "100%% confusion blocked by safeguard");
}
static void CheckSafeguardRest(struct BattleSim *sim)
{
    CHECK(STATUS1(0) & STATUS1_SLEEP, "rest works under own safeguard");
    CHECK(HP(0) == MAXHP(0), "healed");
}
static void CheckSafeguardExpiry(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 1), "second safeguard fails");
    CHECK(LOG_HAS_T(STRINGID_PKMNUSEDSAFEGUARD, 4) && STATUS1(0) == 0, "still blocks on turn 5");
    CHECK(LOG_HAS_T(STRINGID_PKMNSAFEGUARDEXPIRED, 4), "expired at the end of turn 5");
    CHECK(!(SIDE_STATUS(B_SIDE_PLAYER) & SIDE_STATUS_SAFEGUARD) && sim->sideTimers[B_SIDE_PLAYER].safeguardTimer == 0, "cleared");
}
static void CheckSafeguardYawn(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNUSEDSAFEGUARD, 1), "yawn blocked");
    CHECK(!(STATUS3(0) & STATUS3_YAWN), "not drowsy");
}

// ================================================= sports =========================================

static void CheckMudSport(struct BattleSim *sim)
{
    struct Dmg s = { .power = 95 / 2, .special = 1, .stab = 1, .tnum = 1, .tden = 2 };
    struct Dmg full = s; full.power = 95;
    CHECK(LOG_HAS_T(STRINGID_ELECTRICITYWEAKENED, 0), "electricity weakened");
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 1), "second mud sport fails");
    CHECK(STATUS3(0) & STATUS3_MUDSPORT, "flag on the user");
    CHECK_DMG_RANGE(1, 0, MAXHP(0) - HP(0), s, "thunderbolt at half power");
    CHECK_DMG_BELOW(1, 0, MAXHP(0) - HP(0), full, "below full power");
}
static int WantNoBurn(struct BattleSim *sim) { return !(STATUS1(0) & STATUS1_BURN) && HP(0) < MAXHP(0) && WantNoCrit(sim); }
static void CheckWaterSport(struct BattleSim *sim)
{
    struct Dmg s = { .power = 95 / 2, .special = 1, .stab = 1, .tnum = 1, .tden = 2 };
    struct Dmg full = s; full.power = 95;
    CHECK(LOG_HAS_T(STRINGID_FIREWEAKENED, 0), "fire weakened");
    CHECK_DMG_RANGE(1, 0, MAXHP(0) - HP(0), s, "flamethrower at half power");
    CHECK_DMG_BELOW(1, 0, MAXHP(0) - HP(0), full, "below full power");
}
static void CheckWaterSportSwitch(struct BattleSim *sim)
{
    struct Dmg full = { .power = 95, .special = 1, .stab = 1, .tnum = 1, .tden = 1 };
    CHECK(B(0).species == SPECIES_SNORLAX, "snorlax in");
    CHECK(!(STATUS3(0) & STATUS3_WATERSPORT), "sport flag left with marill");
    CHECK_DMG_RANGE(1, 0, MAXHP(0) - HP(0), full, "full power flamethrower on the switch-in");
}

// ================================================= terrain (grass) ================================

static int WantTargetPoisoned(struct BattleSim *sim) { return (STATUS1(1) & STATUS1_POISON) != 0; }
static void CheckSecretPower(struct BattleSim *sim)
{
    CHECK(STATUS1(1) & STATUS1_POISON, "secret power on grass poisons");
    CHECK(LOG_HAS(STRINGID_PKMNWASPOISONED), "poison message");
}
static int WantTargetParalyzed(struct BattleSim *sim) { return (STATUS1(1) & STATUS1_PARALYSIS) != 0; }
static void CheckNaturePower(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_NATUREPOWERTURNEDINTO, 0), "nature power turned into ...");
    CHECK(LOG_HAS_MOVE(STRINGID_USEDMOVE, MOVE_NATURE_POWER), "announced as nature power");
    CHECK(STATUS1(1) & STATUS1_PARALYSIS, "... stun spore on grass");
    CHECK(HP(1) == MAXHP(1), "no damage");
}
static void CheckCamouflage(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNCHANGEDTYPE, 0), "type changed");
    CHECK(TYPES(0, TYPE_GRASS), "grass on grass terrain (types %d/%d)", B(0).type1, B(0).type2);
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 1), "fails when already grass");
}

// ================================================= end-of-turn ordering ===========================

static void CheckSandBeforeLeftovers(struct BattleSim *sim)
{
    int chip = LOG_INDEX_T(STRINGID_PKMNBUFFETEDBYSANDSTORM, 1, 0);
    int item = LOG_INDEX_T(STRINGID_PKMNSITEMRESTOREDHPALITTLE, 1, 0);
    CHECK(chip >= 0 && item >= 0 && chip < item, "sand chip before leftovers (%d, %d)", chip, item);
    CHECK(HP(1) == MAXHP(1), "leftovers healed the chip back to full (hp %d/%d)", HP(1), MAXHP(1));
}
static void CheckSandBeforePoison(struct BattleSim *sim)
{
    int chip = LOG_INDEX_T(STRINGID_PKMNBUFFETEDBYSANDSTORM, 1, 0);
    int psn = LOG_INDEX_T(STRINGID_PKMNHURTBYPOISON, 1, 0);
    CHECK(chip >= 0 && psn >= 0 && chip < psn, "sand chip before poison (%d, %d)", chip, psn);
    CHECK(HP(1) == MAXHP(1) - CHIP(1) - MAXHP(1) / 8, "1/16 + 1/8 (hp %d/%d)", HP(1), MAXHP(1));
}
static void CheckSandSpeedOrder(struct BattleSim *sim)
{
    int fast = LOG_INDEX_T(STRINGID_PKMNBUFFETEDBYSANDSTORM, 1, 0);
    int slow = LOG_INDEX_T(STRINGID_PKMNBUFFETEDBYSANDSTORM, 0, 0);
    CHECK(fast >= 0 && slow >= 0 && fast < slow, "faster rattata chipped before snorlax (%d, %d)", fast, slow);
}
static void CheckSandKO(struct BattleSim *sim)
{
    CHECK(PARTY_HP(B_SIDE_OPPONENT, 0) == 0, "rattata fainted to the sandstorm");
    CHECK(B(1).species == SPECIES_SNORLAX && HP(1) == MAXHP(1), "replacement sent out untouched");
    CHECK(OUTCOME() == 0, "battle goes on");
}
static void CheckSandChipMinimum(struct BattleSim *sim)
{
    // maxHP / 16 rounds to 0 for a level-1 Rattata; the chip is then 1.
    CHECK(MAXHP(1) < 16, "rattata has fewer than 16 hp (%d)", MAXHP(1));
    CHECK(LOG_HAS_T(STRINGID_PKMNBUFFETEDBYSANDSTORM, 0), "buffeted");
    CHECK(HP(1) == MAXHP(1) - 1, "took exactly 1 (hp %d/%d)", HP(1), MAXHP(1));
}
static void CheckDiveDodgesSand(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNHIDUNDERWATER, 0), "dove on turn 1");
    CHECK(LOG_INDEX_T(STRINGID_PKMNBUFFETEDBYSANDSTORM, 0, 0) < 0, "no chip while underwater");
    CHECK(LOG_INDEX_T(STRINGID_PKMNBUFFETEDBYSANDSTORM, 0, 1) >= 0, "chipped after surfacing");
    CHECK(HP(0) == MAXHP(0) - CHIP(0), "one chip only (hp %d/%d)", HP(0), MAXHP(0));
}
static void CheckDigDodgesSand(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNDUGHOLE, 0), "dug in on turn 1");
    CHECK(LOG_INDEX_T(STRINGID_PKMNBUFFETEDBYSANDSTORM, 0, 0) < 0, "no chip while underground");
    CHECK(LOG_INDEX_T(STRINGID_PKMNBUFFETEDBYSANDSTORM, 0, 1) >= 0, "chipped after surfacing");
    CHECK(HP(0) == MAXHP(0) - CHIP(0), "one chip only (hp %d/%d)", HP(0), MAXHP(0));
}
static void CheckScreenBeforeWeatherEnd(struct BattleSim *sim)
{
    int screen = LOG_INDEX_T(STRINGID_PKMNSXWOREOFF, 0, 4);
    int rain = LOG_INDEX_T(STRINGID_RAINSTOPPED, 0, 4);
    CHECK(screen >= 0 && rain >= 0 && screen < rain, "reflect wears off before the rain stops (%d, %d)", screen, rain);
    CHECK(WEATHER() == 0 && !(SIDE_STATUS(B_SIDE_PLAYER) & SIDE_STATUS_REFLECT), "both gone");
}

// ==================================================================================================

#define SNORLAX_SPLASH MON(SPECIES_SNORLAX, 50, SPL, SPL, SPL, SPL)
#define RATTATA_SPLASH MON(SPECIES_RATTATA, 50, SPL, SPL, SPL, SPL)
#define KYOGRE_SPLASH MON(SPECIES_KYOGRE, 50, SPL, SPL, SPL, SPL)
#define GROUDON_SPLASH MON(SPECIES_GROUDON, 50, SPL, SPL, SPL, SPL)
#define TYRANITAR_SPLASH MON(SPECIES_TYRANITAR, 50, SPL, SPL, SPL, SPL)
#define GOLDUCK_CLOUD_NINE MON_AB(SPECIES_GOLDUCK, 50, SPL, SPL, SPL, SPL, 1)
#define RAICHU_SPLASH MON(SPECIES_RAICHU, 50, SPL, SPL, SPL, SPL)

static const struct Scenario sScenarios[] =
{
    // ---- weather moves: 5 turns, replacement, failure, immunities ----
    { .name = "rain_dance_5_turns",
      .player = { MON(SPECIES_LAPRAS, 50, MOVE_RAIN_DANCE, SPL, SPL, SPL) }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0), T(1, 0), T(1, 0), T(1, 0), T(1, 0) }, .turns = 5, .check = CheckRainDance5 },
    { .name = "sunny_day_5_turns",
      .player = { MON(SPECIES_ARCANINE, 50, MOVE_SUNNY_DAY, SPL, SPL, SPL) }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0), T(1, 0), T(1, 0), T(1, 0), T(1, 0) }, .turns = 5, .check = CheckSunnyDay5 },
    { .name = "sandstorm_5_turns_chip",
      .player = { MON(SPECIES_GOLEM, 50, MOVE_SANDSTORM, SPL, SPL, SPL) }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0), T(1, 0), T(1, 0), T(1, 0), T(1, 0) }, .turns = 5, .check = CheckSandstorm5 },
    { .name = "hail_5_turns_chip",
      .player = { MON(SPECIES_GLALIE, 50, MOVE_HAIL, SPL, SPL, SPL) }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0), T(1, 0), T(1, 0), T(1, 0), T(1, 0) }, .turns = 5, .check = CheckHail5 },
    { .name = "weather_replaces_weather",
      .player = { MON(SPECIES_LAPRAS, 50, MOVE_RAIN_DANCE, MOVE_SUNNY_DAY, MOVE_SANDSTORM, MOVE_HAIL) }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0), T(1, 0), T(2, 0), T(3, 0) }, .turns = 4, .check = CheckWeatherReplaces },
    { .name = "same_weather_fails",
      .player = { MON(SPECIES_LAPRAS, 50, MOVE_RAIN_DANCE, SPL, SPL, SPL) }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .check = CheckSameWeatherFails },
    { .name = "sandstorm_steel_ground_immune",
      .player = { MON(SPECIES_SKARMORY, 50, MOVE_SANDSTORM, SPL, SPL, SPL) }, .enemy = { MON(SPECIES_MAROWAK, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSandImmuneTypes },
    { .name = "sandstorm_sand_veil_immune",
      .player = { MON(SPECIES_CACTURNE, 50, MOVE_SANDSTORM, SPL, SPL, SPL) }, .enemy = { RATTATA_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSandVeil },
    { .name = "hail_ice_immune_sand_rock_immune",
      .player = { MON(SPECIES_JYNX, 50, MOVE_SANDSTORM, MOVE_HAIL, SPL, SPL) }, .enemy = { MON(SPECIES_GOLEM, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckHailVsSandImmunity },

    // ---- ability weather ----
    { .name = "drizzle_permanent",
      .player = { SNORLAX_SPLASH }, .enemy = { KYOGRE_SPLASH },
      .actions = { T(0, 0), T(0, 0), T(0, 0), T(0, 0), T(0, 0), T(0, 0) }, .turns = 6, .check = CheckDrizzle },
    { .name = "rain_dance_vs_drizzle_fails",
      .player = { MON(SPECIES_LAPRAS, 50, MOVE_RAIN_DANCE, SPL, SPL, SPL) }, .enemy = { KYOGRE_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckRainDanceVsDrizzle },
    { .name = "sunny_day_replaces_drizzle",
      .player = { MON(SPECIES_LAPRAS, 50, MOVE_SUNNY_DAY, SPL, SPL, SPL) }, .enemy = { KYOGRE_SPLASH },
      .actions = { T(0, 0), T(1, 0), T(1, 0), T(1, 0), T(1, 0) }, .turns = 5, .check = CheckSunnyDayVsDrizzle },
    { .name = "sand_stream_permanent_chip",
      .player = { SNORLAX_SPLASH }, .enemy = { TYRANITAR_SPLASH },
      .actions = { T(0, 0), T(0, 0), T(0, 0) }, .turns = 3, .check = CheckSandStream },
    { .name = "rain_dance_replaces_sand_stream",
      .player = { MON(SPECIES_LAPRAS, 50, MOVE_RAIN_DANCE, SPL, SPL, SPL) }, .enemy = { TYRANITAR_SPLASH },
      .actions = { T(0, 0), T(1, 0), T(1, 0), T(1, 0), T(1, 0) }, .turns = 5, .check = CheckRainReplacesSandStream },
    { .name = "sand_stream_switch_in_replaces_rain",
      .player = { MON(SPECIES_LAPRAS, 50, MOVE_RAIN_DANCE, SPL, SPL, SPL) }, .enemy = { RATTATA_SPLASH, TYRANITAR_SPLASH },
      .actions = { T(0, 0), T(1, SC_SWITCH(1)) }, .turns = 2, .check = CheckSandStreamSwitchIn },
    { .name = "drought_solar_beam_no_charge",
      .player = { MON(SPECIES_VENUSAUR, 50, MOVE_SOLAR_BEAM, SPL, SPL, SPL) }, .enemy = { GROUDON_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantNoCrit, .check = CheckDroughtSolarBeam },

    // ---- rain/sun multipliers ----
    { .name = "rain_boosts_water",
      .player = { MON(SPECIES_LAPRAS, 50, MOVE_RAIN_DANCE, MOVE_SURF, SPL, SPL) }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .wantSeed = WantNoCrit, .check = CheckRainBoostsWater },
    { .name = "rain_halves_fire",
      .player = { MON(SPECIES_CHARIZARD, 50, MOVE_FLAMETHROWER, SPL, SPL, SPL) }, .enemy = { KYOGRE_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantNoCrit, .check = CheckRainHalvesFire },
    { .name = "sun_boosts_fire",
      .player = { MON(SPECIES_CHARIZARD, 50, MOVE_FLAMETHROWER, SPL, SPL, SPL) }, .enemy = { GROUDON_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantNoCrit, .check = CheckSunBoostsFire },
    { .name = "sun_halves_water",
      .player = { MON(SPECIES_LAPRAS, 50, MOVE_SURF, SPL, SPL, SPL) }, .enemy = { GROUDON_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantNoCrit, .check = CheckSunHalvesWater },

    // ---- thunder ----
    { .name = "thunder_never_misses_in_rain",
      .player = { MON(SPECIES_PIKACHU, 10, MOVE_THUNDER, SPL, SPL, SPL) }, .enemy = { KYOGRE_SPLASH },
      .actions = { T(0, 0), T(0, 0), T(0, 0), T(0, 0), T(0, 0), T(0, 0), T(0, 0), T(0, 0), T(0, 0), T(0, 0) }, .turns = 10, .check = CheckThunderRain },
    { .name = "thunder_can_miss_in_sun",
      .player = { MON(SPECIES_RAICHU, 50, MOVE_SUNNY_DAY, MOVE_THUNDER, SPL, SPL) }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0), T(1, 0), T(1, 0), T(1, 0) }, .turns = 4, .wantSeed = WantAMiss, .check = CheckThunderSun },
    { .name = "thunder_rain_cloud_nine_can_miss",
      .player = { MON(SPECIES_KYOGRE, 5, MOVE_THUNDER, SPL, SPL, SPL) }, .enemy = { GOLDUCK_CLOUD_NINE },
      .actions = { T(0, 0), T(0, 0), T(0, 0), T(0, 0), T(0, 0), T(0, 0) }, .turns = 6, .wantSeed = WantAMiss, .check = CheckThunderRainCloudNine },

    // ---- solar beam ----
    { .name = "solar_beam_halved_in_rain",
      .player = { MON(SPECIES_VENUSAUR, 50, MOVE_SOLAR_BEAM, SPL, SPL, SPL) }, .enemy = { KYOGRE_SPLASH },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .wantSeed = WantNoCrit, .check = CheckSolarBeamRain },
    { .name = "solar_beam_halved_in_sandstorm",
      .player = { MON(SPECIES_VENUSAUR, 50, MOVE_SOLAR_BEAM, SPL, SPL, SPL) }, .enemy = { TYRANITAR_SPLASH },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .wantSeed = WantNoCrit, .check = CheckSolarBeamSand },
    { .name = "solar_beam_halved_in_hail",
      .player = { MON(SPECIES_VENUSAUR, 50, MOVE_HAIL, MOVE_SOLAR_BEAM, SPL, SPL) }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0), T(1, 0), T(1, 0) }, .turns = 3, .wantSeed = WantNoCrit, .check = CheckSolarBeamHail },
    { .name = "solar_beam_charges_in_sun_under_air_lock",
      .player = { MON(SPECIES_VENUSAUR, 50, MOVE_SUNNY_DAY, MOVE_SOLAR_BEAM, SPL, SPL) }, .enemy = { MON(SPECIES_RAYQUAZA, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, 0), T(1, 0) }, .turns = 3, .wantSeed = WantNoCrit, .check = CheckSolarBeamAirLock },

    // ---- synthesis / morning sun / moonlight ----
    { .name = "synthesis_clear_half",
      .player = { { .species = SPECIES_VENUSAUR, .level = 50, .moves = { MOVE_SYNTHESIS, SPL, SPL, SPL }, .hp = 1, .hpSet = 1 } }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSynthesisClear },
    { .name = "synthesis_sun_two_thirds",
      .player = { { .species = SPECIES_VENUSAUR, .level = 50, .moves = { MOVE_SUNNY_DAY, MOVE_SYNTHESIS, SPL, SPL }, .hp = 1, .hpSet = 1 } }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckSynthesisSun },
    { .name = "synthesis_rain_quarter",
      .player = { { .species = SPECIES_VENUSAUR, .level = 50, .moves = { MOVE_SYNTHESIS, SPL, SPL, SPL }, .hp = 1, .hpSet = 1 } }, .enemy = { KYOGRE_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSynthesisRain },
    { .name = "moonlight_sandstorm_quarter",
      .player = { { .species = SPECIES_UMBREON, .level = 50, .moves = { MOVE_MOONLIGHT, SPL, SPL, SPL }, .hp = 1, .hpSet = 1 } }, .enemy = { TYRANITAR_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckMoonlightSand },
    { .name = "morning_sun_hail_quarter",
      .player = { { .species = SPECIES_CLEFABLE, .level = 50, .moves = { MOVE_HAIL, MOVE_MORNING_SUN, SPL, SPL }, .hp = 40, .hpSet = 1 } }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckMorningSunHail },
    { .name = "synthesis_sun_cloud_nine_half",
      .player = { { .species = SPECIES_VENUSAUR, .level = 50, .moves = { MOVE_SUNNY_DAY, MOVE_SYNTHESIS, SPL, SPL }, .hp = 1, .hpSet = 1 } }, .enemy = { GOLDUCK_CLOUD_NINE },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckSynthesisCloudNine },
    { .name = "synthesis_full_hp_fails",
      .player = { MON(SPECIES_VENUSAUR, 50, MOVE_SYNTHESIS, SPL, SPL, SPL) }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSynthesisFull },

    // ---- weather ball ----
    { .name = "weather_ball_rain_water",
      .player = { MON(SPECIES_LAPRAS, 50, MOVE_WEATHER_BALL, SPL, SPL, SPL) }, .enemy = { KYOGRE_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantNoCrit, .check = CheckWeatherBallRain },
    { .name = "weather_ball_sun_fire",
      .player = { MON(SPECIES_LAPRAS, 50, MOVE_WEATHER_BALL, SPL, SPL, SPL) }, .enemy = { GROUDON_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantNoCrit, .check = CheckWeatherBallSun },
    { .name = "weather_ball_sandstorm_rock",
      .player = { MON(SPECIES_LAPRAS, 50, MOVE_WEATHER_BALL, SPL, SPL, SPL) }, .enemy = { TYRANITAR_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantNoCrit, .check = CheckWeatherBallSand },
    { .name = "weather_ball_hail_ice",
      .player = { MON(SPECIES_LAPRAS, 50, MOVE_HAIL, MOVE_WEATHER_BALL, SPL, SPL) }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .wantSeed = WantNoCrit, .check = CheckWeatherBallHail },
    { .name = "weather_ball_clear_normal",
      .player = { MON(SPECIES_LAPRAS, 50, MOVE_WEATHER_BALL, SPL, SPL, SPL) }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantNoCrit, .check = CheckWeatherBallClear },
    { .name = "weather_ball_cloud_nine_normal",
      .player = { MON(SPECIES_KYOGRE, 50, MOVE_WEATHER_BALL, SPL, SPL, SPL) }, .enemy = { GOLDUCK_CLOUD_NINE },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantNoCrit, .check = CheckWeatherBallCloudNine },

    // ---- castform / forecast ----
    { .name = "castform_sun_fire",
      .player = { MON(SPECIES_CASTFORM, 50, MOVE_SUNNY_DAY, SPL, SPL, SPL) }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckCastformFire },
    { .name = "castform_rain_water",
      .player = { MON(SPECIES_CASTFORM, 50, MOVE_RAIN_DANCE, SPL, SPL, SPL) }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckCastformWater },
    { .name = "castform_hail_ice_no_chip",
      .player = { MON(SPECIES_CASTFORM, 50, MOVE_HAIL, SPL, SPL, SPL) }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckCastformIce },
    { .name = "castform_sandstorm_normal",
      .player = { MON(SPECIES_CASTFORM, 50, MOVE_RAIN_DANCE, MOVE_SANDSTORM, SPL, SPL) }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckCastformSand },
    { .name = "castform_reverts_when_sun_ends",
      .player = { MON(SPECIES_CASTFORM, 50, MOVE_SUNNY_DAY, SPL, SPL, SPL) }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0), T(1, 0), T(1, 0), T(1, 0), T(1, 0) }, .turns = 5, .check = CheckCastformReverts },
    { .name = "castform_cloud_nine_stays_normal",
      .player = { MON(SPECIES_CASTFORM, 50, MOVE_SUNNY_DAY, SPL, SPL, SPL) }, .enemy = { GOLDUCK_CLOUD_NINE },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckCastformCloudNine },
    { .name = "castform_switch_in_to_drizzle",
      .player = { RATTATA_SPLASH, MON(SPECIES_CASTFORM, 50, SPL, SPL, SPL, SPL) }, .enemy = { KYOGRE_SPLASH },
      .actions = { T(SC_SWITCH(1), 0) }, .turns = 1, .check = CheckCastformSwitchIn },

    // ---- cloud nine / air lock / swift swim / chlorophyll / rain dish ----
    { .name = "cloud_nine_no_sandstorm_chip",
      .player = { GOLDUCK_CLOUD_NINE }, .enemy = { TYRANITAR_SPLASH },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .check = CheckCloudNineSand },
    { .name = "air_lock_no_rain_boost",
      .player = { MON(SPECIES_RAYQUAZA, 50, MOVE_RAIN_DANCE, MOVE_SURF, SPL, SPL) }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .wantSeed = WantNoCrit, .check = CheckAirLockRain },
    { .name = "cloud_nine_disables_swift_swim",
      .player = { MON(SPECIES_LUDICOLO, 50, MOVE_RAIN_DANCE, SPL, SPL, SPL) }, .enemy = { GOLDUCK_CLOUD_NINE },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckSwiftSwimOff },
    { .name = "swift_swim_turn_order",
      .player = { MON(SPECIES_LUDICOLO, 50, MOVE_RAIN_DANCE, SPL, SPL, SPL) }, .enemy = { RAICHU_SPLASH },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckSwiftSwim },
    { .name = "chlorophyll_turn_order",
      .player = { MON(SPECIES_EXEGGUTOR, 50, MOVE_SUNNY_DAY, SPL, SPL, SPL) }, .enemy = { RAICHU_SPLASH },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckChlorophyll },
    { .name = "chlorophyll_not_in_rain",
      .player = { MON(SPECIES_EXEGGUTOR, 50, MOVE_RAIN_DANCE, SPL, SPL, SPL) }, .enemy = { RAICHU_SPLASH },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckChlorophyllRain },
    { .name = "rain_dish_heals",
      .player = { { .species = SPECIES_LUDICOLO, .level = 50, .moves = { SPL, SPL, SPL, SPL }, .abilityNum = 1, .hp = 50, .hpSet = 1 } }, .enemy = { KYOGRE_SPLASH },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .check = CheckRainDish },
    { .name = "rain_dish_cloud_nine",
      .player = { { .species = SPECIES_LUDICOLO, .level = 50, .moves = { MOVE_RAIN_DANCE, SPL, SPL, SPL }, .abilityNum = 1, .hp = 50, .hpSet = 1 } }, .enemy = { GOLDUCK_CLOUD_NINE },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckRainDishCloudNine },
    { .name = "rain_dish_not_on_the_turn_rain_stops",
      .player = { { .species = SPECIES_LUDICOLO, .level = 50, .moves = { MOVE_RAIN_DANCE, SPL, SPL, SPL }, .abilityNum = 1, .hp = 50, .hpSet = 1 } }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0), T(1, 0), T(1, 0), T(1, 0), T(1, 0) }, .turns = 5, .check = CheckRainDishNotWhenRainStops },
    { .name = "rain_dish_before_leftovers",
      .player = { { .species = SPECIES_LUDICOLO, .level = 50, .moves = { SPL, SPL, SPL, SPL }, .abilityNum = 1, .item = ITEM_LEFTOVERS, .hp = 143, .hpSet = 1 } }, .enemy = { KYOGRE_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckRainDishBeforeLeftovers },

    // ---- reflect / light screen / brick break ----
    { .name = "reflect_halves_physical",
      .player = { MON(SPECIES_SLOWBRO, 50, MOVE_REFLECT, SPL, SPL, SPL) }, .enemy = { MON(SPECIES_RATTATA, 50, MOVE_STRENGTH, SPL, SPL, SPL) },
      .actions = { T(0, 1), T(1, 0) }, .turns = 2, .wantSeed = WantNoCrit, .check = CheckReflectPhysical },
    { .name = "light_screen_halves_special",
      .player = { MON(SPECIES_SLOWBRO, 50, MOVE_LIGHT_SCREEN, SPL, SPL, SPL) }, .enemy = { MON(SPECIES_RATTATA, 50, MOVE_THUNDERBOLT, SPL, SPL, SPL) },
      .actions = { T(0, 1), T(1, 0) }, .turns = 2, .wantSeed = WantNoCrit, .check = CheckLightScreenSpecial },
    { .name = "light_screen_ignores_physical",
      .player = { MON(SPECIES_SLOWBRO, 50, MOVE_LIGHT_SCREEN, SPL, SPL, SPL) }, .enemy = { MON(SPECIES_RATTATA, 50, MOVE_STRENGTH, SPL, SPL, SPL) },
      .actions = { T(0, 1), T(1, 0) }, .turns = 2, .wantSeed = WantNoCrit, .check = CheckLightScreenNotPhysical },
    { .name = "reflect_ignores_special",
      .player = { MON(SPECIES_SLOWBRO, 50, MOVE_REFLECT, SPL, SPL, SPL) }, .enemy = { MON(SPECIES_RATTATA, 50, MOVE_THUNDERBOLT, SPL, SPL, SPL) },
      .actions = { T(0, 1), T(1, 0) }, .turns = 2, .wantSeed = WantNoCrit, .check = CheckReflectNotSpecial },
    { .name = "reflect_expires_after_5_turns",
      .player = { MON(SPECIES_SLOWBRO, 50, MOVE_REFLECT, SPL, SPL, SPL) }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0), T(1, 0), T(1, 0), T(1, 0), T(1, 0) }, .turns = 5, .check = CheckReflectExpires },
    { .name = "reflect_twice_fails",
      .player = { MON(SPECIES_SLOWBRO, 50, MOVE_REFLECT, SPL, SPL, SPL) }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .check = CheckReflectTwice },
    { .name = "brick_break_shatters_screens",
      .player = { MON(SPECIES_SLOWBRO, 50, MOVE_REFLECT, MOVE_LIGHT_SCREEN, SPL, SPL) }, .enemy = { MON(SPECIES_MACHAMP, 50, MOVE_BRICK_BREAK, SPL, SPL, SPL) },
      .actions = { T(0, 1), T(1, 1), T(2, 0) }, .turns = 3, .wantSeed = WantNoCrit, .check = CheckBrickBreak },
    { .name = "brick_break_without_screens",
      .player = { MON(SPECIES_SLOWBRO, 50, SPL, SPL, SPL, SPL) }, .enemy = { MON(SPECIES_MACHAMP, 50, MOVE_BRICK_BREAK, SPL, SPL, SPL) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantNoCrit, .check = CheckBrickBreakNoScreen },
    { .name = "brick_break_shatters_screens_even_on_ghost",
      .player = { MON(SPECIES_GENGAR, 50, MOVE_REFLECT, SPL, SPL, SPL) }, .enemy = { MON(SPECIES_MACHAMP, 50, MOVE_BRICK_BREAK, SPL, SPL, SPL) },
      .actions = { T(0, 1), T(1, 0) }, .turns = 2, .check = CheckBrickBreakGhost },
    { .name = "reflect_does_not_weaken_own_attacks",
      .player = { MON(SPECIES_MACHAMP, 50, MOVE_REFLECT, MOVE_STRENGTH, SPL, SPL) }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .wantSeed = WantNoCrit, .check = CheckReflectOwnSideOnly },
    { .name = "reflect_survives_setter_fainting",
      .player = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_REFLECT, SPL, SPL, SPL }, .hp = 20, .hpSet = 1 }, MON(SPECIES_SLOWBRO, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_MACHAMP, 50, MOVE_KARATE_CHOP, MOVE_STRENGTH, SPL, SPL) },
      .actions = { T(0, 2), T(1, 0), T(0, 1) }, .turns = 3, .wantSeed = WantNoCrit, .check = CheckScreenSurvivesFaint },
    { .name = "reflect_doubles_two_thirds",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { MON(SPECIES_SLOWBRO, 50, MOVE_REFLECT, SPL, SPL, SPL), MON(SPECIES_SLOWBRO, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_MACHAMP, 50, MOVE_STRENGTH, SPL, SPL, SPL), SNORLAX_SPLASH },
      .actions = { T4(0, 1, 0, 0), T4(1, 0, 0, 0) }, .targets = { { 0, 0, 0, 0 }, { 0, 1, 0, 0 } }, .turns = 2, .wantSeed = WantNoCrit, .check = CheckReflectDoubles },
    { .name = "light_screen_doubles_two_thirds",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { MON(SPECIES_SLOWBRO, 50, MOVE_LIGHT_SCREEN, SPL, SPL, SPL), MON(SPECIES_SLOWBRO, 50, SPL, SPL, SPL, SPL) },
      .enemy = { MON(SPECIES_RAICHU, 50, MOVE_THUNDERBOLT, SPL, SPL, SPL), SNORLAX_SPLASH },
      .actions = { T4(0, 1, 0, 0), T4(1, 0, 0, 0) }, .targets = { { 0, 0, 0, 0 }, { 0, 1, 0, 0 } }, .turns = 2, .wantSeed = WantNoCrit, .check = CheckLightScreenDoubles },
    { .name = "reflect_doubles_halves_once_partner_fainted",
      .flags = BATTLE_TYPE_DOUBLE,
      .player = { MON(SPECIES_SLOWBRO, 50, MOVE_REFLECT, SPL, SPL, SPL), { .species = SPECIES_RATTATA, .level = 50, .moves = { SPL, SPL, SPL, SPL }, .hp = 1, .hpSet = 1 } },
      .enemy = { MON(SPECIES_MACHAMP, 50, MOVE_STRENGTH, MOVE_KARATE_CHOP, SPL, SPL), SNORLAX_SPLASH },
      .actions = { T4(0, 1, 0, 0), T4(1, 0, 0, 0) }, .targets = { { 0, 3, 0, 0 }, { 0, 1, 0, 0 } }, .turns = 2, .wantSeed = WantNoCrit, .check = CheckReflectDoublesPartnerFainted },
    { .name = "crit_ignores_reflect",
      .player = { MON(SPECIES_SLOWBRO, 50, MOVE_REFLECT, SPL, SPL, SPL) }, .enemy = { MON(SPECIES_PERSIAN, 50, MOVE_SLASH, SPL, SPL, SPL) },
      .actions = { T(0, 1), T(1, 0) }, .turns = 2, .wantSeed = WantCritTurn1, .check = CheckCritIgnoresReflect },

    // ---- spikes / rapid spin ----
    { .name = "spikes_one_layer_eighth",
      .player = { MON(SPECIES_FORRETRESS, 50, MOVE_SPIKES, SPL, SPL, SPL) }, .enemy = { RATTATA_SPLASH, SNORLAX_SPLASH },
      .actions = { T(0, 0), T(1, SC_SWITCH(1)) }, .turns = 2, .check = CheckSpikes1 },
    { .name = "spikes_two_layers_sixth",
      .player = { MON(SPECIES_FORRETRESS, 50, MOVE_SPIKES, SPL, SPL, SPL) }, .enemy = { RATTATA_SPLASH, SNORLAX_SPLASH },
      .actions = { T(0, 0), T(0, 0), T(1, SC_SWITCH(1)) }, .turns = 3, .check = CheckSpikes2 },
    { .name = "spikes_three_layers_quarter_fourth_fails",
      .player = { MON(SPECIES_FORRETRESS, 50, MOVE_SPIKES, SPL, SPL, SPL) }, .enemy = { RATTATA_SPLASH, SNORLAX_SPLASH },
      .actions = { T(0, 0), T(0, 0), T(0, 0), T(0, 0), T(1, SC_SWITCH(1)) }, .turns = 5, .check = CheckSpikes3 },
    { .name = "spikes_flying_levitate_immune",
      .player = { MON(SPECIES_FORRETRESS, 50, MOVE_SPIKES, SPL, SPL, SPL) },
      .enemy = { RATTATA_SPLASH, MON(SPECIES_SKARMORY, 50, SPL, SPL, SPL, SPL), MON(SPECIES_GENGAR, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, SC_SWITCH(1)), T(1, SC_SWITCH(2)), T(1, SC_SWITCH(0)) }, .turns = 4, .check = CheckSpikesImmune },
    { .name = "spikes_hit_faint_replacement",
      .player = { MON(SPECIES_FORRETRESS, 50, MOVE_SPIKES, MOVE_TACKLE, SPL, SPL) },
      .enemy = { { .species = SPECIES_RATTATA, .level = 50, .moves = { SPL, SPL, SPL, SPL }, .hp = 1, .hpSet = 1 }, SNORLAX_SPLASH },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .wantSeed = WantEnemyLeadFainted, .check = CheckSpikesReplacement },
    { .name = "rapid_spin_clears_spikes",
      .player = { MON(SPECIES_STARMIE, 50, MOVE_RAPID_SPIN, SPL, SPL, SPL) }, .enemy = { MON(SPECIES_FORRETRESS, 50, MOVE_SPIKES, SPL, SPL, SPL) },
      .actions = { T(1, 0), T(0, 1) }, .turns = 2, .check = CheckRapidSpinSpikes },
    { .name = "rapid_spin_clears_leech_seed_and_spikes",
      .player = { MON(SPECIES_STARMIE, 50, MOVE_RAPID_SPIN, SPL, SPL, SPL) }, .enemy = { MON(SPECIES_FORRETRESS, 50, MOVE_SPIKES, MOVE_LEECH_SEED, SPL, SPL) },
      .actions = { T(1, 0), T(1, 1), T(0, 2) }, .turns = 3, .wantSeed = WantShedSeedTurn3, .check = CheckRapidSpinSeedFirst },
    { .name = "rapid_spin_clears_wrap_and_spikes",
      .player = { MON(SPECIES_STARMIE, 50, MOVE_RAPID_SPIN, SPL, SPL, SPL) }, .enemy = { MON(SPECIES_ARBOK, 50, MOVE_SPIKES, MOVE_WRAP, SPL, SPL) },
      .actions = { T(1, 0), T(1, 1), T(0, 2) }, .turns = 3, .wantSeed = WantGotFreeTurn3, .check = CheckRapidSpinWrapFirst },

    // ---- mist ----
    { .name = "mist_blocks_growl",
      .player = { MON(SPECIES_SLOWBRO, 50, MOVE_MIST, SPL, SPL, SPL) }, .enemy = { MON(SPECIES_RATTATA, 50, MOVE_GROWL, SPL, SPL, SPL) },
      .actions = { T(0, 1), T(1, 0) }, .turns = 2, .check = CheckMistGrowl },
    { .name = "mist_blocks_intimidate",
      .player = { MON(SPECIES_SLOWBRO, 50, MOVE_MIST, SPL, SPL, SPL) }, .enemy = { RATTATA_SPLASH, MON(SPECIES_ARCANINE, 50, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, SC_SWITCH(1)) }, .turns = 2, .check = CheckMistIntimidate },
    { .name = "mist_does_not_block_own_superpower",
      .player = { MON(SPECIES_MACHAMP, 50, MOVE_MIST, MOVE_SUPERPOWER, SPL, SPL) }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckMistSuperpower },
    { .name = "mist_does_not_protect_foe",
      .player = { MON(SPECIES_SLOWBRO, 50, MOVE_MIST, MOVE_GROWL, SPL, SPL) }, .enemy = { RATTATA_SPLASH },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckMistNotFoe },
    { .name = "mist_fails_twice_protects_turn_5_then_expires",
      .player = { MON(SPECIES_SLOWBRO, 50, MOVE_MIST, SPL, SPL, SPL) }, .enemy = { MON(SPECIES_RATTATA, 50, MOVE_GROWL, SPL, SPL, SPL) },
      .actions = { T(0, 1), T(0, 1), T(1, 1), T(1, 1), T(1, 0) }, .turns = 5, .check = CheckMistExpiry },

    // ---- safeguard ----
    { .name = "safeguard_blocks_status_moves",
      .player = { MON(SPECIES_SLOWBRO, 50, MOVE_SAFEGUARD, SPL, SPL, SPL) }, .enemy = { MON(SPECIES_RAICHU, 50, MOVE_THUNDER_WAVE, MOVE_TOXIC, MOVE_SWAGGER, SPL) },
      .actions = { T(0, 3), T(1, 0), T(1, 1), T(1, 2) }, .turns = 4, .wantSeed = WantSafeguard3, .check = CheckSafeguardStatusMoves },
    { .name = "safeguard_blocks_secondary_paralysis",
      .player = { MON(SPECIES_SLOWBRO, 50, MOVE_SAFEGUARD, SPL, SPL, SPL) }, .enemy = { MON(SPECIES_MAGNETON, 50, MOVE_ZAP_CANNON, SPL, SPL, SPL) },
      .actions = { T(0, 1), T(1, 0) }, .turns = 2, .wantSeed = WantHitNoMiss, .check = CheckSafeguardZapCannon },
    { .name = "safeguard_blocks_secondary_confusion",
      .player = { MON(SPECIES_SLOWBRO, 50, MOVE_SAFEGUARD, SPL, SPL, SPL) }, .enemy = { MON(SPECIES_MACHAMP, 50, MOVE_DYNAMIC_PUNCH, SPL, SPL, SPL) },
      .actions = { T(0, 1), T(1, 0) }, .turns = 2, .wantSeed = WantHitNoMiss, .check = CheckSafeguardDynamicPunch },
    { .name = "safeguard_allows_rest",
      .player = { { .species = SPECIES_SLOWBRO, .level = 50, .moves = { MOVE_SAFEGUARD, MOVE_REST, SPL, SPL }, .hp = 30, .hpSet = 1 } }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckSafeguardRest },
    { .name = "safeguard_fails_twice_blocks_turn_5_then_expires",
      .player = { MON(SPECIES_SLOWBRO, 50, MOVE_SAFEGUARD, SPL, SPL, SPL) }, .enemy = { MON(SPECIES_RAICHU, 50, MOVE_THUNDER_WAVE, SPL, SPL, SPL) },
      .actions = { T(0, 1), T(0, 1), T(1, 1), T(1, 1), T(1, 0) }, .turns = 5, .check = CheckSafeguardExpiry },
    { .name = "safeguard_blocks_yawn",
      .player = { MON(SPECIES_SLOWBRO, 50, MOVE_SAFEGUARD, SPL, SPL, SPL) }, .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_YAWN, SPL, SPL, SPL) },
      .actions = { T(0, 1), T(1, 0) }, .turns = 2, .check = CheckSafeguardYawn },

    // ---- mud sport / water sport ----
    { .name = "mud_sport_halves_electric",
      .player = { MON(SPECIES_PIKACHU, 50, MOVE_MUD_SPORT, SPL, SPL, SPL) }, .enemy = { MON(SPECIES_RAICHU, 50, MOVE_THUNDERBOLT, SPL, SPL, SPL) },
      .actions = { T(0, 1), T(0, 0) }, .turns = 2, .wantSeed = WantNoCrit, .check = CheckMudSport },
    { .name = "water_sport_halves_fire",
      .player = { MON_AB(SPECIES_MARILL, 50, MOVE_WATER_SPORT, SPL, SPL, SPL, 1) }, .enemy = { MON(SPECIES_ARCANINE, 50, MOVE_FLAMETHROWER, SPL, SPL, SPL) },
      .actions = { T(0, 1), T(1, 0) }, .turns = 2, .wantSeed = WantNoBurn, .check = CheckWaterSport },
    { .name = "water_sport_ends_when_user_switches",
      .player = { MON_AB(SPECIES_MARILL, 50, MOVE_WATER_SPORT, SPL, SPL, SPL, 1), SNORLAX_SPLASH }, .enemy = { MON(SPECIES_ARCANINE, 50, MOVE_FLAMETHROWER, SPL, SPL, SPL) },
      .actions = { T(0, 1), T(SC_SWITCH(1), 0) }, .turns = 2, .wantSeed = WantNoBurn, .check = CheckWaterSportSwitch },

    // ---- terrain: grass ----
    { .name = "secret_power_grass_poisons",
      .player = { MON(SPECIES_CLEFABLE, 50, MOVE_SECRET_POWER, SPL, SPL, SPL) }, .enemy = { RATTATA_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantTargetPoisoned, .check = CheckSecretPower },
    { .name = "nature_power_grass_stun_spore",
      .player = { MON(SPECIES_EXEGGUTOR, 50, MOVE_NATURE_POWER, SPL, SPL, SPL) }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantTargetParalyzed, .check = CheckNaturePower },
    { .name = "camouflage_grass_type",
      .player = { MON(SPECIES_KECLEON, 50, MOVE_CAMOUFLAGE, SPL, SPL, SPL) }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .check = CheckCamouflage },

    // ---- end-of-turn ordering ----
    { .name = "sandstorm_before_leftovers",
      .player = { MON(SPECIES_GOLEM, 50, MOVE_SANDSTORM, SPL, SPL, SPL) }, .enemy = { MON_ITEM(SPECIES_SNORLAX, 50, SPL, SPL, SPL, SPL, ITEM_LEFTOVERS) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSandBeforeLeftovers },
    { .name = "sandstorm_before_poison",
      .player = { MON(SPECIES_GOLEM, 50, MOVE_SANDSTORM, SPL, SPL, SPL) },
      .enemy = { { .species = SPECIES_SNORLAX, .level = 50, .moves = { SPL, SPL, SPL, SPL }, .abilityNum = 1, .status = STATUS1_POISON } },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSandBeforePoison },
    { .name = "sandstorm_chip_in_speed_order",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SANDSTORM, SPL, SPL, SPL) }, .enemy = { RATTATA_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSandSpeedOrder },
    { .name = "sandstorm_ko_brings_replacement",
      .player = { MON(SPECIES_GOLEM, 50, MOVE_SANDSTORM, SPL, SPL, SPL) },
      .enemy = { { .species = SPECIES_RATTATA, .level = 50, .moves = { SPL, SPL, SPL, SPL }, .hp = 1, .hpSet = 1 }, SNORLAX_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSandKO },
    { .name = "sandstorm_chip_minimum_one",
      .player = { MON(SPECIES_GOLEM, 50, MOVE_SANDSTORM, SPL, SPL, SPL) }, .enemy = { MON(SPECIES_RATTATA, 1, SPL, SPL, SPL, SPL) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSandChipMinimum },
    { .name = "dive_dodges_sandstorm_chip",
      .player = { MON(SPECIES_LAPRAS, 50, MOVE_DIVE, SPL, SPL, SPL) }, .enemy = { TYRANITAR_SPLASH },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .check = CheckDiveDodgesSand },
    { .name = "dig_dodges_sandstorm_chip",
      .player = { MON(SPECIES_RATTATA, 50, MOVE_DIG, SPL, SPL, SPL) }, .enemy = { TYRANITAR_SPLASH },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .check = CheckDigDodgesSand },
    { .name = "reflect_wears_off_before_rain_stops",
      .player = { MON(SPECIES_SLOWBRO, 50, MOVE_REFLECT, SPL, SPL, SPL) }, .enemy = { MON(SPECIES_LAPRAS, 50, MOVE_RAIN_DANCE, SPL, SPL, SPL) },
      .actions = { T(0, 0), T(1, 1), T(1, 1), T(1, 1), T(1, 1) }, .turns = 5, .check = CheckScreenBeforeWeatherEnd },
};

SCENARIO_GROUP(field_effects, sScenarios)
