// Two-turn, charging, semi-invulnerable, recharge and locking moves: Solar Beam, Razor Wind, Skull Bash,
// Sky Attack, Fly/Dig/Dive, Hyper Beam & co, Bide, Rollout/Ice Ball, Fury Cutter, Rage, Thrash & co, Uproar,
// Focus Punch, Fake Out, plus Endure/Protect/Truant interactions.
//
// Engine rules exercised (sim/src/battle_util.c, battle_script_commands.c, data/battle_scripts_1.s):
// - Charge turn: BattleScriptFirstChargingTurn deducts PP and sets STATUS2_MULTIPLETURNS + gLockedMoves; the
//   second turn uses HITMARKER_NO_PPDEDUCT. Protect only blocks a two-turn move once MULTIPLETURNS is set.
// - AtkCanceller: flinch/disable/taunt/truant/recharge call CancelMultiTurnMoves; full paralysis does it
//   from the script (cancelmultiturnmoves). Falling asleep cancels immediately too (SetMoveEffect's sleep case
//   and Yawn's ENDTURN_YAWN both call CancelMultiTurnMoves), so a Fly user lands the moment it falls asleep;
//   the extra "asleep + MULTIPLETURNS" cleanup in HandleEndTurn_ContinueBattle only matters for a locking
//   move called by Sleep Talk.
// - Uproar's sleep block is skipped when the *target* has Soundproof (UproarWakeUpCheck/SetMoveEffect), so an
//   Exploud can be put to sleep during its own uproar.
// - Recharge is a move effect applied after the hit (miss/protect/immunity -> no recharge, KO -> recharge).
//   CANCELLER_ASLEEP/TRUANT come before CANCELLER_RECHARGE, and rechargeTimer expires on its own.
// - Bide: damage stored from setbide on, 2x, type immunity applies but SE/NVE flags are cleared.
// - Rollout/Fury Cutter: base power doubles per hit (Defense Curl doubles Rollout again); a miss, Protect or
//   any CancelMultiTurnMoves resets. Fury Cutter's counter only resets on miss/cancel/switch (not on using
//   another move).
// - Thrash: 2-3 turns then confusion at the end of the last turn unless WasUnableToUseMove() that turn.
// - Uproar: 2-5 turns, wakes non-Soundproof sleepers, blocks sleep (Rest included), blocked by Soundproof.
#include "scenario.h"

// ---------------------------------------------------------------- helpers ----------------------------------------
static int UsedBy(struct BattleSim *sim, u8 battler, u16 move, int turn)
{
    int i;
    for (i = 0; i < sim->logCount; i++)
        if (sim->log[i].stringId == STRINGID_USEDMOVE && sim->log[i].battler == battler && sim->log[i].move == move
            && (turn == SC_ANY_TURN || sim->log[i].turn == turn))
            return 1;
    return 0;
}
#define LOG_HAS_B(id, b, turn) (Sc_LogIndex(sim, id, b, 0, turn) >= 0)

// Damage dealt by `attacker`'s `move` on `turn`: target HP when the attack string was printed minus the HP at the
// attacker's next use of the same move (or the target's final HP). Only valid while nothing else changes the
// target's HP in between.
static int DmgOnTurn(struct BattleSim *sim, u8 attacker, u16 move, u8 target, int turn)
{
    int i, before = -1;
    for (i = 0; i < sim->logCount; i++)
    {
        if (sim->log[i].stringId != STRINGID_USEDMOVE || sim->log[i].battler != attacker || sim->log[i].move != move)
            continue;
        if (before < 0)
        {
            if (sim->log[i].turn == turn)
                before = sim->log[i].hpTarget;
        }
        else if (sim->log[i].turn > turn)
        {
            return before - sim->log[i].hpTarget;
        }
    }
    return before < 0 ? -1 : before - HP(target);
}

// CalculateBaseDamage() for a non-critical hit at neutral-or-current stat stages (player side gets the badge
// boosts), i.e. the value before crit/multiplier/STAB/type/random. `preHalve` = weather halving (Solar Beam).
static const s8 sStageRatios[][2] = { {10,40},{10,35},{10,30},{10,25},{10,20},{10,15},{10,10},{15,10},{20,10},{25,10},{30,10},{35,10},{40,10} };
static int BaseDmg(struct BattleSim *sim, u8 atk, u8 def, int power, int special, int preHalve)
{
    int a = special ? B(atk).spAttack : B(atk).attack;
    int d = special ? B(def).spDefense : B(def).defense;
    int dmg;
    if ((atk & 1) == B_SIDE_PLAYER) a = a * 110 / 100;
    if ((def & 1) == B_SIDE_PLAYER) d = d * 110 / 100;
    a = a * sStageRatios[B(atk).statStages[special ? STAT_SPATK : STAT_ATK]][0] / sStageRatios[B(atk).statStages[special ? STAT_SPATK : STAT_ATK]][1];
    d = d * sStageRatios[B(def).statStages[special ? STAT_SPDEF : STAT_DEF]][0] / sStageRatios[B(def).statStages[special ? STAT_SPDEF : STAT_DEF]][1];
    dmg = a * power;
    dmg *= (2 * B(atk).level / 5 + 2);
    dmg /= d;
    dmg /= 50;
    if (dmg == 0) dmg = 1;
    if (preHalve) dmg /= 2;
    return dmg + 2;
}
// Cmd_damagecalc multiplier, then typecalc (STAB x1.5, then type multiplier /10).
static int Final(int base, int mul, int stab, int type10)
{
    int d = base * mul;
    if (stab) d = d * 15 / 10;
    d = d * type10 / 10;
    if (d == 0 && type10) d = 1;
    return d;
}
// adjustnormaldamage: x(100 - rand%16)/100
static int InRoll(int x, int d) { int lo = d * 85 / 100; if (lo == 0) lo = 1; return x >= lo && x <= d; }
#define CHECK_ROLL(x, d, what) CHECK(InRoll(x, d), what " (dmg %d, expected %d..%d)", (x), (d) * 85 / 100, (d))

static int NoCrit(struct BattleSim *sim) { return !LOG_HAS(STRINGID_CRITICALHIT); }
static int NoCritNoMiss(struct BattleSim *sim) { return !LOG_HAS(STRINGID_CRITICALHIT) && !LOG_HAS(STRINGID_ATTACKMISSED); }
static int NoMiss(struct BattleSim *sim) { return !LOG_HAS(STRINGID_ATTACKMISSED); }

#define M0(sp, lv) MON(sp, lv, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH)
#define M1(sp, lv, m1) MON(sp, lv, m1, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH)
#define SNORLAX_SPLASH M0(SPECIES_SNORLAX, 50)
#define STEELIX_SPLASH M0(SPECIES_STEELIX, 50)

// ---------------------------------------------------------------- Solar Beam --------------------------------------
static void CheckSolarBeamCharge(struct BattleSim *sim)
{
    int i = Sc_LogIndex(sim, STRINGID_USEDMOVE, 0, 0, 1);
    CHECK(LOG_HAS_T(STRINGID_PKMNTOOKSUNLIGHT, 0), "took in sunlight on turn 1");
    CHECK(i >= 0 && sim->log[i].hpTarget == MAXHP(1), "no damage before the turn-2 attack");
    CHECK(UsedBy(sim, 0, MOVE_SOLAR_BEAM, 1), "fired on turn 2");
    CHECK(PP(0, 0) == 9, "pp deducted once (pp %d)", PP(0, 0));
    CHECK(!(STATUS2(0) & STATUS2_MULTIPLETURNS), "lock cleared after firing");
    CHECK_ROLL(MAXHP(1) - HP(1), Final(BaseDmg(sim, 0, 1, 120, 1, 0), 1, 1, 10), "solar beam damage");
}
static void CheckSolarBeamSun(struct BattleSim *sim)
{
    CHECK(WEATHER() & B_WEATHER_SUN, "sun");
    CHECK(!LOG_HAS(STRINGID_PKMNTOOKSUNLIGHT), "no charge turn under sun");
    CHECK(UsedBy(sim, 0, MOVE_SOLAR_BEAM, 1) && HP(1) < MAXHP(1), "fired in one turn");
    CHECK(PP(0, 1) == 9, "pp deducted (pp %d)", PP(0, 1));
    CHECK(!(STATUS2(0) & STATUS2_MULTIPLETURNS), "no lock left");
    CHECK_ROLL(MAXHP(1) - HP(1), Final(BaseDmg(sim, 0, 1, 120, 1, 0), 1, 1, 10), "sun does not boost grass");
}
static void CheckSolarBeamRain(struct BattleSim *sim)
{
    CHECK(WEATHER() & B_WEATHER_RAIN, "rain");
    CHECK(LOG_HAS_T(STRINGID_PKMNTOOKSUNLIGHT, 0), "charged (rain set after the charge)");
    CHECK_ROLL(MAXHP(1) - HP(1), Final(BaseDmg(sim, 0, 1, 120, 1, 1), 1, 1, 10), "solar beam halved by rain");
}
static void CheckSolarBeamSandstorm(struct BattleSim *sim)
{
    CHECK(WEATHER() & B_WEATHER_SANDSTORM, "sandstorm");
    CHECK(LOG_HAS_T(STRINGID_PKMNTOOKSUNLIGHT, 0), "charged (sandstorm set after the charge)");
    CHECK(!LOG_HAS_B(STRINGID_PKMNBUFFETEDBYSANDSTORM, 1, SC_ANY_TURN) && LOG_HAS_B(STRINGID_PKMNBUFFETEDBYSANDSTORM, 0, SC_ANY_TURN), "sandstorm chips venusaur, not steelix");
    CHECK_ROLL(MAXHP(1) - HP(1), Final(BaseDmg(sim, 0, 1, 120, 1, 1), 1, 1, 10), "solar beam halved by sandstorm");
}
static void CheckSolarBeamHail(struct BattleSim *sim)
{
    CHECK(WEATHER() & B_WEATHER_HAIL, "hail");
    CHECK(LOG_HAS_T(STRINGID_PKMNTOOKSUNLIGHT, 0), "charged (hail set after the charge)");
    CHECK(LOG_HAS_B(STRINGID_PKMNPELTEDBYHAIL, 0, SC_ANY_TURN) && !LOG_HAS_B(STRINGID_PKMNPELTEDBYHAIL, 1, SC_ANY_TURN), "hail chips venusaur, not glalie");
    CHECK_ROLL(MAXHP(1) - HP(1), Final(BaseDmg(sim, 0, 1, 120, 1, 1), 1, 1, 10), "solar beam halved by hail");
}
static void CheckSolarBeamCloudNine(struct BattleSim *sim)
{
    CHECK(WEATHER() & B_WEATHER_SUN, "sun is up");
    CHECK(LOG_HAS_T(STRINGID_PKMNTOOKSUNLIGHT, 1), "cloud nine: still has to charge");
    CHECK(HP(1) == MAXHP(1), "no damage yet");
    CHECK(STATUS2(0) & STATUS2_MULTIPLETURNS, "locked into solar beam");
    CHECK(sim->lockedMoves[0] == MOVE_SOLAR_BEAM, "locked move recorded");
}
static void CheckTwoTurnProtectSecondTurn(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNPROTECTEDITSELF, 1), "protected on the attack turn");
    CHECK(HP(1) == MAXHP(1), "no damage");
    CHECK(!(STATUS2(0) & STATUS2_MULTIPLETURNS), "lock cancelled by protect");
    CHECK(UsedBy(sim, 0, MOVE_SPLASH, 2), "free to choose on turn 3");
    CHECK(PP(0, 0) == 9, "pp deducted once (pp %d)", PP(0, 0));
}
static void CheckTwoTurnProtectChargeTurn(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNTOOKSUNLIGHT, 0), "charge not blocked by protect");
    CHECK(!LOG_HAS(STRINGID_PKMNPROTECTEDITSELF), "no protect message");
    CHECK(HP(1) < MAXHP(1), "fired on turn 2");
}
static int WantChargeThenFlinch(struct BattleSim *sim)
{
    return Sc_LogHas(sim, STRINGID_PKMNTOOKSUNLIGHT, 0) && Sc_LogHas(sim, STRINGID_PKMNFLINCHED, 1) && UsedBy(sim, 0, MOVE_SPLASH, 2);
}
static void CheckTwoTurnFlinch(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNFLINCHED, 1), "flinched on the attack turn");
    CHECK(HP(1) == MAXHP(1), "never fired");
    CHECK(!(STATUS2(0) & STATUS2_MULTIPLETURNS), "lock cancelled");
    CHECK(UsedBy(sim, 0, MOVE_SPLASH, 2), "free choice afterwards");
}
static int WantChargeThenParalysis(struct BattleSim *sim)
{
    return Sc_LogHas(sim, STRINGID_PKMNTOOKSUNLIGHT, 0) && Sc_LogHas(sim, STRINGID_PKMNISPARALYZED, 1) && UsedBy(sim, 0, MOVE_SPLASH, 2);
}
static void CheckTwoTurnParalysis(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNISPARALYZED, 1), "fully paralyzed on the attack turn");
    CHECK(HP(1) == MAXHP(1), "never fired");
    CHECK(!(STATUS2(0) & STATUS2_MULTIPLETURNS), "full paralysis cancels the lock (script cancelmultiturnmoves)");
    CHECK(UsedBy(sim, 0, MOVE_SPLASH, 2), "free choice afterwards");
}
static void CheckTwoTurnRoared(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNTOOKSUNLIGHT, 0), "charged");
    CHECK(B(0).species == SPECIES_RATTATA, "dragged out mid-charge");
    CHECK(HP(1) == MAXHP(1), "beam never fired");
    CHECK(!(STATUS2(0) & STATUS2_MULTIPLETURNS), "replacement is not locked");
    CHECK(UsedBy(sim, 0, MOVE_SPLASH, 1), "replacement chose its own move");
}
static void CheckTwoTurnTargetSwitched(struct BattleSim *sim)
{
    CHECK(B(1).species == SPECIES_STEELIX, "steelix switched in");
    CHECK(HP(1) < MAXHP(1), "beam hit the replacement");
    CHECK(PARTY_HP(B_SIDE_OPPONENT, 0) == (int)GetMonData(&sim->enemyParty[0], MON_DATA_MAX_HP), "original target untouched");
}

// ---------------------------------------------------------------- Razor Wind / Skull Bash / Sky Attack ------------
static void CheckRazorWind(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNWHIPPEDWHIRLWIND, 0), "whipped up a whirlwind");
    CHECK(UsedBy(sim, 0, MOVE_RAZOR_WIND, 1), "hit on turn 2");
    CHECK(PP(0, 0) == 9, "pp deducted once (pp %d)", PP(0, 0));
    CHECK_ROLL(MAXHP(1) - HP(1), Final(BaseDmg(sim, 0, 1, 80, 0, 0), 1, 1, 10), "razor wind damage");
}
static void CheckSkullBash(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNLOWEREDHEAD, 0), "lowered its head");
    CHECK(STAGE(0, STAT_DEF) == 7, "+1 defense on the charge turn (stage %d)", STAGE(0, STAT_DEF));
    CHECK(LOG_HAS_T(STRINGID_ATTACKERSSTATROSE, 0) || LOG_HAS_T(STRINGID_DEFENDERSSTATROSE, 0), "stat rose message");
    CHECK(UsedBy(sim, 0, MOVE_SKULL_BASH, 1) && HP(1) < MAXHP(1), "hit on turn 2");
    CHECK(PP(0, 0) == 14, "pp deducted once (pp %d)", PP(0, 0));
    CHECK_ROLL(MAXHP(1) - HP(1), Final(BaseDmg(sim, 0, 1, 100, 0, 0), 1, 0, 10), "skull bash damage");
}
static int WantFlinchT1(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_PKMNFLINCHED, 1); }
static void CheckSkyAttackFlinch(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNISGLOWING, 0), "is glowing");
    CHECK(LOG_HAS_T(STRINGID_PKMNFLINCHED, 1), "target flinched from the turn-2 hit");
    CHECK(!LOG_HAS_B(STRINGID_USEDMOVE, 1, 1), "target did not move on turn 2");
    CHECK(PP(0, 0) == 4, "pp deducted once (pp %d)", PP(0, 0));
}
static int WantMissT1(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_ATTACKMISSED, 1); }
static void CheckSkyAttackMiss(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_ATTACKMISSED, 1), "missed on the attack turn");
    CHECK(HP(1) == MAXHP(1), "no damage");
    CHECK(!(STATUS2(0) & STATUS2_MULTIPLETURNS), "lock cleared even on a miss");
    CHECK(PP(0, 0) == 4, "no pp for the second turn (pp %d)", PP(0, 0));
    CHECK(UsedBy(sim, 0, MOVE_SPLASH, 2), "free choice afterwards");
}

// ---------------------------------------------------------------- semi-invulnerable -------------------------------
static void CheckFlyGust(struct BattleSim *sim)
{
    int d0 = DmgOnTurn(sim, 1, MOVE_GUST, 0, 0), d1 = DmgOnTurn(sim, 1, MOVE_GUST, 0, 1);
    int base = BaseDmg(sim, 1, 0, 40, 0, 0);
    CHECK(LOG_HAS_T(STRINGID_PKMNFLEWHIGH, 0), "flew up");
    CHECK(!LOG_HAS_T(STRINGID_ATTACKMISSED, 0), "gust hits the airborne mon");
    CHECK_ROLL(d0, Final(base, 2, 1, 10), "gust double damage vs fly");
    CHECK_ROLL(d1, Final(base, 1, 1, 10), "gust normal damage once landed");
    CHECK(HP(1) < MAXHP(1), "fly landed on turn 2");
}
static void CheckFlyTwister(struct BattleSim *sim)
{
    int d0 = DmgOnTurn(sim, 1, MOVE_TWISTER, 0, 0), d1 = DmgOnTurn(sim, 1, MOVE_TWISTER, 0, 1);
    int base = BaseDmg(sim, 1, 0, 40, 1, 0);
    CHECK(!LOG_HAS_T(STRINGID_ATTACKMISSED, 0), "twister hits the airborne mon");
    CHECK_ROLL(d0, Final(base, 2, 1, 10), "twister double damage vs fly");
    CHECK_ROLL(d1, Final(base, 1, 1, 10), "twister normal damage once landed");
    CHECK(!LOG_HAS(STRINGID_PKMNFLINCHED), "flinch cannot apply to a mon that already moved");
}
static int WantPlayerDamaged(struct BattleSim *sim) { return HP(0) < MAXHP(0); }
static void CheckFlyThunder(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNFLEWHIGH, 0), "flew up");
    CHECK(!LOG_HAS(STRINGID_ATTACKMISSED) && HP(0) < MAXHP(0), "thunder hits the airborne mon");
    CHECK(STATUS3(0) & STATUS3_ON_AIR, "still airborne at the end of the charge turn");
}
static void CheckLockOnFly(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNTOOKAIM, 0), "took aim");
    CHECK(LOG_HAS_T(STRINGID_PKMNFLEWHIGH, 1), "flew up");
    CHECK(!LOG_HAS_T(STRINGID_ATTACKMISSED, 1) && HP(0) < MAXHP(0), "lock-on hit the airborne mon");
    CHECK(!(STATUS3(0) & STATUS3_ALWAYS_HITS), "lock-on expired after 2 turns");
}
static void CheckMindReaderDig(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNDUGHOLE, 1), "dug a hole");
    CHECK(!LOG_HAS_T(STRINGID_ATTACKMISSED, 1) && HP(0) < MAXHP(0), "mind reader hit the underground mon");
}
static void CheckDigEarthquake(struct BattleSim *sim)
{
    int d0 = DmgOnTurn(sim, 1, MOVE_EARTHQUAKE, 0, 0), d1 = DmgOnTurn(sim, 1, MOVE_EARTHQUAKE, 0, 1);
    int base = BaseDmg(sim, 1, 0, 100, 0, 0);
    CHECK(LOG_HAS_T(STRINGID_PKMNDUGHOLE, 0), "dug a hole");
    CHECK(!LOG_HAS_T(STRINGID_ATTACKMISSED, 0), "earthquake hits underground");
    CHECK_ROLL(d0, Final(base, 2, 1, 10), "earthquake double damage vs dig");
    CHECK_ROLL(d1, Final(base, 1, 1, 10), "earthquake normal damage once surfaced");
    CHECK(HP(1) < MAXHP(1), "dig hit on turn 2");
}
static void CheckDigMagnitude(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNDUGHOLE, 0), "dug a hole");
    CHECK(LOG_HAS_T(STRINGID_MAGNITUDESTRENGTH, 0), "magnitude announced");
    CHECK(!LOG_HAS(STRINGID_ATTACKMISSED) && HP(0) < MAXHP(0), "magnitude hits underground");
}
static void CheckDiveSurf(struct BattleSim *sim)
{
    int d0 = DmgOnTurn(sim, 1, MOVE_SURF, 0, 0), d1 = DmgOnTurn(sim, 1, MOVE_SURF, 0, 1);
    int base = BaseDmg(sim, 1, 0, 95, 1, 0);
    CHECK(LOG_HAS_T(STRINGID_PKMNHIDUNDERWATER, 0), "hid underwater");
    CHECK(!LOG_HAS_T(STRINGID_ATTACKMISSED, 0), "surf hits underwater");
    CHECK_ROLL(d0, Final(base, 2, 1, 10), "surf double damage vs dive");
    CHECK_ROLL(d1, Final(base, 1, 1, 10), "surf normal damage once surfaced");
    CHECK(HP(1) < MAXHP(1), "dive hit on turn 2");
}
static void CheckDiveWhirlpool(struct BattleSim *sim)
{
    int dmg = MAXHP(0) - HP(0) - MAXHP(0) / 16;   // minus the end-of-turn trap damage
    CHECK(STATUS2(0) & STATUS2_WRAPPED, "trapped by whirlpool while diving");
    CHECK_ROLL(dmg, Final(BaseDmg(sim, 1, 0, 15, 1, 0), 2, 1, 10), "whirlpool double damage vs dive");
}
static void CheckDigDodgesHypnosis(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNDUGHOLE, 0), "dug a hole");
    CHECK(LOG_HAS(STRINGID_ATTACKMISSED) || LOG_HAS(STRINGID_BUTITFAILED), "hypnosis missed the underground mon");
    CHECK(!(STATUS1(0) & STATUS1_SLEEP), "not asleep");
}
static int WantFlyThenParalysis(struct BattleSim *sim)
{
    return Sc_LogHas(sim, STRINGID_PKMNFLEWHIGH, 0) && Sc_LogHas(sim, STRINGID_PKMNISPARALYZED, 1);
}
static void CheckFlyParalysis(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNISPARALYZED, 1), "fully paralyzed on the attack turn");
    CHECK(!(STATUS3(0) & STATUS3_ON_AIR), "landed without attacking");
    CHECK(!(STATUS2(0) & STATUS2_MULTIPLETURNS), "lock cancelled");
    CHECK(HP(1) == MAXHP(1), "no damage");
}
static void CheckFlyYawnSleep(struct BattleSim *sim)
{
    // Yawn (turn 1) puts the mon to sleep at the end of turn 2, after it flew up. ENDTURN_YAWN calls
    // CancelMultiTurnMoves, so it lands (and loses the lock) right there: the turn-3 tackle hits a sleeping,
    // grounded Pidgeot and Fly never fires.
    CHECK(LOG_HAS_T(STRINGID_PKMNFLEWHIGH, 1), "flew up on turn 2");
    CHECK(LOG_HAS_T(STRINGID_ATTACKMISSED, 1), "tackle missed on turn 2");
    CHECK(LOG_HAS_T(STRINGID_PKMNFELLASLEEP, 1), "fell asleep at the end of turn 2");
    CHECK(LOG_HAS_T(STRINGID_PKMNFASTASLEEP, 2), "asleep on turn 3");
    CHECK(!LOG_HAS_T(STRINGID_ATTACKMISSED, 2) && HP(0) < MAXHP(0), "landed when it fell asleep: tackle hits on turn 3");
    CHECK(!(STATUS3(0) & STATUS3_ON_AIR) && !(STATUS2(0) & STATUS2_MULTIPLETURNS), "no lock left");
    CHECK(!UsedBy(sim, 0, MOVE_FLY, 2) && HP(1) == MAXHP(1), "fly never attacked");
}
// Sleep Talk calling Thrash is the one way to end a turn asleep and locked: HandleEndTurn_ContinueBattle
// cancels the lock (battle_main.c) before the end-turn effects, without confusion.
static int WantSleepTalkThrash(struct BattleSim *sim) { return DmgOnTurn(sim, 0, MOVE_SLEEP_TALK, 1, 0) > 0; }
static void CheckSleepTalkThrash(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNFASTASLEEP, 0) && UsedBy(sim, 0, MOVE_SLEEP_TALK, 0), "sleep talk used while asleep");
    CHECK(UsedBy(sim, 0, MOVE_THRASH, 0) && DmgOnTurn(sim, 0, MOVE_SLEEP_TALK, 1, 0) > 0, "thrash was called and hit");
    CHECK(STATUS1(0) & STATUS1_SLEEP, "still asleep");
    CHECK(!(STATUS2(0) & (STATUS2_LOCK_CONFUSE | STATUS2_MULTIPLETURNS)), "lock cancelled at the end of the sleeping turn");
    CHECK(!(STATUS2(0) & STATUS2_CONFUSION) && !LOG_HAS(STRINGID_PKMNFATIGUECONFUSION), "no fatigue confusion");
    CHECK(UsedBy(sim, 0, MOVE_SLEEP_TALK, 1), "sleep talk chosen again on turn 2 (not locked into thrash)");
    CHECK(PP(0, 0) == 8, "sleep talk pp deducted twice (pp %d)", PP(0, 0));
}
static void CheckDigSubstituteEarthquake(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNDUGHOLE, 1), "dug a hole behind the substitute");
    CHECK(!LOG_HAS_T(STRINGID_ATTACKMISSED, 1), "earthquake hits");
    CHECK(HP(0) == MAXHP(0) - MAXHP(0) / 4, "substitute absorbed the doubled earthquake (hp %d/%d)", HP(0), MAXHP(0));
    CHECK(!(STATUS2(0) & STATUS2_SUBSTITUTE), "substitute broke");
    CHECK(STATUS3(0) & STATUS3_UNDERGROUND, "still underground");
}

// ---------------------------------------------------------------- recharge ----------------------------------------
static int WantHitT0(struct BattleSim *sim) { return !Sc_LogHas(sim, STRINGID_ATTACKMISSED, 0) && !Sc_LogHas(sim, STRINGID_CRITICALHIT, SC_ANY_TURN); }
static void CheckHyperBeamRecharge(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNMUSTRECHARGE, 1), "must recharge on turn 2");
    CHECK(!UsedBy(sim, 0, MOVE_HYPER_BEAM, 1), "no attack on the recharge turn");
    CHECK(!(STATUS2(0) & STATUS2_RECHARGE), "recharge flag cleared");
    CHECK(sim->disableStructs[0].rechargeTimer == 0, "recharge timer cleared");
    CHECK(sim->lockedMoves[0] == MOVE_HYPER_BEAM, "locked move recorded");
    CHECK(UsedBy(sim, 0, MOVE_SPLASH, 2), "free choice on turn 3");
    CHECK(PP(0, 0) == 4, "pp deducted once (pp %d)", PP(0, 0));
    CHECK_ROLL(MAXHP(1) - HP(1), Final(BaseDmg(sim, 0, 1, 150, 0, 0), 1, 1, 5), "hyper beam damage (resisted)");
}
static int WantMissThenHit(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_ATTACKMISSED, 0) && !Sc_LogHas(sim, STRINGID_ATTACKMISSED, 1); }
static void CheckHyperBeamMiss(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_ATTACKMISSED, 0), "missed");
    CHECK(!LOG_HAS(STRINGID_PKMNMUSTRECHARGE), "no recharge after a miss");
    CHECK(UsedBy(sim, 0, MOVE_HYPER_BEAM, 1) && HP(1) < MAXHP(1), "attacked again on turn 2 and hit");
    CHECK((STATUS2(0) & STATUS2_RECHARGE) && sim->disableStructs[0].rechargeTimer == 1, "the turn-2 hit set the recharge (timer %d)", sim->disableStructs[0].rechargeTimer);
    CHECK(PP(0, 0) == 3, "pp deducted twice (pp %d)", PP(0, 0));
}
static void CheckHyperBeamProtect(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNPROTECTEDITSELF, 0), "protected");
    CHECK(!LOG_HAS(STRINGID_PKMNMUSTRECHARGE), "no recharge when blocked by protect");
    CHECK(UsedBy(sim, 0, MOVE_HYPER_BEAM, 1), "attacked again on turn 2");
}
static int WantFirstEnemyFainted(struct BattleSim *sim) { return PARTY_HP(B_SIDE_OPPONENT, 0) == 0; }
static void CheckHyperBeamKo(struct BattleSim *sim)
{
    CHECK(PARTY_HP(B_SIDE_OPPONENT, 0) == 0, "target fainted");
    CHECK(B(1).species == SPECIES_STEELIX, "replacement sent out");
    CHECK(LOG_HAS_T(STRINGID_PKMNMUSTRECHARGE, 1), "still has to recharge after a KO");
    CHECK(HP(1) == MAXHP(1), "replacement untouched");
}
static void CheckHyperBeamImmune(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_ITDOESNTAFFECT, 0), "ghost immune");
    CHECK(!LOG_HAS(STRINGID_PKMNMUSTRECHARGE) && !(STATUS2(0) & STATUS2_RECHARGE), "no recharge when the move has no effect");
    CHECK(UsedBy(sim, 0, MOVE_HYPER_BEAM, 1), "attacked again on turn 2");
    CHECK(PP(0, 0) == 3, "pp deducted both times (pp %d)", PP(0, 0));
}
static int WantHyperBeamHitT1(struct BattleSim *sim) { return !Sc_LogHas(sim, STRINGID_ATTACKMISSED, 1); }
static void CheckRechargeSleep(struct BattleSim *sim)
{
    // Yawn's sleep lands at the end of the Hyper Beam turn; CANCELLER_ASLEEP runs before CANCELLER_RECHARGE and
    // the recharge timer expires on its own, so the sleep "absorbs" the recharge turn.
    CHECK(HP(1) < MAXHP(1), "hyper beam hit");
    CHECK(STATUS1(0) & STATUS1_SLEEP, "asleep");
    CHECK(LOG_HAS_T(STRINGID_PKMNFASTASLEEP, 2), "fast asleep on the would-be recharge turn");
    CHECK(!LOG_HAS(STRINGID_PKMNMUSTRECHARGE), "no recharge message");
    CHECK(!(STATUS2(0) & STATUS2_RECHARGE), "recharge flag expired with its timer");
}
static void CheckHydroCannon(struct BattleSim *sim)
{
    CHECK(HP(1) < MAXHP(1), "hydro cannon hit");
    CHECK(LOG_HAS_T(STRINGID_PKMNMUSTRECHARGE, 1), "must recharge");
    CHECK(sim->lockedMoves[0] == MOVE_HYDRO_CANNON, "locked move recorded");
}

// ---------------------------------------------------------------- Bide ------------------------------------------
static void CheckBide(struct BattleSim *sim)
{
    CHECK(!LOG_HAS_T(STRINGID_PKMNSTORINGENERGY, 0) && LOG_HAS_T(STRINGID_PKMNSTORINGENERGY, 1), "storing energy on turn 2");
    CHECK(LOG_HAS_T(STRINGID_PKMNUNLEASHEDENERGY, 2), "unleashed on turn 3");
    CHECK(HP(0) == MAXHP(0) - 150, "took three seismic tosses (hp %d/%d)", HP(0), MAXHP(0));
    CHECK(HP(1) == MAXHP(1) - 200, "bide returned double the damage taken after it started (hp %d/%d)", HP(1), MAXHP(1));
    CHECK(PP(0, 0) == 9, "pp deducted once (pp %d)", PP(0, 0));
    CHECK(!(STATUS2(0) & (STATUS2_BIDE | STATUS2_MULTIPLETURNS)), "bide flags cleared");
}
static void CheckBideIgnoresResistance(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNUNLEASHEDENERGY, 2), "unleashed");
    CHECK(HP(1) == MAXHP(1) - 100, "steel type does not resist bide (hp %d/%d)", HP(1), MAXHP(1));
}
static void CheckBideNoDamage(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNUNLEASHEDENERGY, 2) && LOG_HAS_T(STRINGID_BUTITFAILED, 2), "unleashed but failed");
    CHECK(HP(1) == MAXHP(1), "no damage");
}
static void CheckBideGhost(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNUNLEASHEDENERGY, 2), "unleashed");
    CHECK(LOG_HAS_T(STRINGID_ITDOESNTAFFECT, 2), "ghost is immune to bide");
    CHECK(HP(1) == MAXHP(1), "no damage");
}
static void CheckBideProtect(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNPROTECTEDITSELF, 2), "protect blocks the release");
    CHECK(HP(1) == MAXHP(1), "no damage");
    CHECK(!(STATUS2(0) & (STATUS2_BIDE | STATUS2_MULTIPLETURNS)), "bide over");
}
static void CheckBideSwitch(struct BattleSim *sim)
{
    CHECK(B(1).species == SPECIES_STEELIX, "steelix switched in");
    CHECK(HP(1) == MAXHP(1) - 100, "bide hits the replacement with the stored damage (hp %d/%d)", HP(1), MAXHP(1));
    CHECK(PARTY_HP(B_SIDE_OPPONENT, 0) == (int)GetMonData(&sim->enemyParty[0], MON_DATA_MAX_HP), "original target untouched");
}

// ---------------------------------------------------------------- Rollout / Ice Ball ------------------------------
static void CheckRolloutFive(struct BattleSim *sim)
{
    int k;
    for (k = 0; k < 5; k++)
    {
        int d = DmgOnTurn(sim, 0, MOVE_ROLLOUT, 1, k);
        CHECK(InRoll(d, Final(BaseDmg(sim, 0, 1, 30 << k, 0, 0), 1, 1, 10)), "rollout hit %d power %d (dmg %d, expected %d..%d)",
              k + 1, 30 << k, d, Final(BaseDmg(sim, 0, 1, 30 << k, 0, 0), 1, 1, 10) * 85 / 100, Final(BaseDmg(sim, 0, 1, 30 << k, 0, 0), 1, 1, 10));
    }
    CHECK(!(STATUS2(0) & STATUS2_MULTIPLETURNS) && sim->disableStructs[0].rolloutTimer == 0, "sequence over after 5 hits");
    CHECK(UsedBy(sim, 0, MOVE_SPLASH, 5), "free choice on turn 6");
    CHECK(PP(0, 0) == 19, "pp deducted once for the sequence (pp %d)", PP(0, 0));
}
static void CheckRolloutDefenseCurl(struct BattleSim *sim)
{
    int d1 = DmgOnTurn(sim, 0, MOVE_ROLLOUT, 1, 1), d2 = DmgOnTurn(sim, 0, MOVE_ROLLOUT, 1, 2);
    CHECK(STATUS2(0) & STATUS2_DEFENSE_CURL, "defense curl flag");
    CHECK_ROLL(d1, Final(BaseDmg(sim, 0, 1, 60, 0, 0), 1, 1, 10), "first rollout hit doubled by defense curl");
    CHECK_ROLL(d2, Final(BaseDmg(sim, 0, 1, 120, 0, 0), 1, 1, 10), "second hit doubled again");
}
static int WantHitThenMiss(struct BattleSim *sim) { return !Sc_LogHas(sim, STRINGID_ATTACKMISSED, 0) && Sc_LogHas(sim, STRINGID_ATTACKMISSED, 1); }
static void CheckRolloutMiss(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_ATTACKMISSED, 1), "missed mid-sequence");
    CHECK(!(STATUS2(0) & STATUS2_MULTIPLETURNS) && sim->disableStructs[0].rolloutTimer == 0, "sequence ended by the miss");
    CHECK(UsedBy(sim, 0, MOVE_SPLASH, 2), "free choice afterwards");
    CHECK(PP(0, 0) == 19, "no pp for the missed continuation (pp %d)", PP(0, 0));
}
static void CheckRolloutProtect(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNPROTECTEDITSELF, 1), "protected");
    CHECK(!(STATUS2(0) & STATUS2_MULTIPLETURNS) && sim->disableStructs[0].rolloutTimer == 0, "sequence ended by protect");
    CHECK(PP(0, 0) == 18, "protect cancels the lock before ppreduce: pp deducted again (pp %d)", PP(0, 0));
    CHECK(UsedBy(sim, 0, MOVE_SPLASH, 2), "free choice afterwards");
}
static void CheckRolloutFaint(struct BattleSim *sim)
{
    CHECK(PARTY_HP(B_SIDE_OPPONENT, 0) == 0 && PARTY_HP(B_SIDE_OPPONENT, 1) == 0, "both rattata fainted");
    CHECK(UsedBy(sim, 0, MOVE_ROLLOUT, 1) && UsedBy(sim, 0, MOVE_ROLLOUT, 2), "rollout continued on the replacements");
    CHECK(B(1).species == SPECIES_STEELIX && HP(1) < MAXHP(1), "third hit landed on steelix");
    CHECK((STATUS2(0) & STATUS2_MULTIPLETURNS) && sim->disableStructs[0].rolloutTimer == 2, "sequence still running (timer %d)", sim->disableStructs[0].rolloutTimer);
}
static void CheckIceBall(struct BattleSim *sim)
{
    CHECK(STATUS2(0) & STATUS2_MULTIPLETURNS, "locked");
    CHECK(sim->lockedMoves[0] == MOVE_ICE_BALL, "locked into ice ball");
    CHECK(sim->disableStructs[0].rolloutTimer == 4, "4 hits to go (timer %d)", sim->disableStructs[0].rolloutTimer);
    CHECK(PP(0, 0) == 19, "pp %d", PP(0, 0));
}

// ---------------------------------------------------------------- Fury Cutter -------------------------------------
static void CheckFuryCutterGrowth(struct BattleSim *sim)
{
    static const int powers[6] = { 10, 20, 40, 80, 160, 160 };
    int k;
    for (k = 0; k < 6; k++)
    {
        int d = DmgOnTurn(sim, 0, MOVE_FURY_CUTTER, 1, k), e = Final(BaseDmg(sim, 0, 1, powers[k], 0, 0), 1, 1, 10);
        CHECK(InRoll(d, e), "fury cutter hit %d power %d (dmg %d, expected %d..%d)", k + 1, powers[k], d, e * 85 / 100, e);
    }
    CHECK(sim->disableStructs[0].furyCutterCounter == 5, "counter capped at 5 (%d)", sim->disableStructs[0].furyCutterCounter);
    CHECK(PP(0, 0) == 14, "pp %d", PP(0, 0));
}
static int WantFuryCutterMissT1(struct BattleSim *sim)
{
    return !Sc_LogHas(sim, STRINGID_ATTACKMISSED, 0) && Sc_LogHas(sim, STRINGID_ATTACKMISSED, 1) && !Sc_LogHas(sim, STRINGID_ATTACKMISSED, 2)
        && !Sc_LogHas(sim, STRINGID_CRITICALHIT, SC_ANY_TURN);
}
static void CheckFuryCutterMissResets(struct BattleSim *sim)
{
    CHECK_ROLL(DmgOnTurn(sim, 0, MOVE_FURY_CUTTER, 1, 2), Final(BaseDmg(sim, 0, 1, 10, 0, 0), 1, 1, 10), "back to base power after a miss");
    CHECK(sim->disableStructs[0].furyCutterCounter == 1, "counter restarted (%d)", sim->disableStructs[0].furyCutterCounter);
}
static void CheckFuryCutterOtherMove(struct BattleSim *sim)
{
    // FRLG never resets the counter for using another move: only miss, CancelMultiTurnMoves and switching do.
    CHECK(UsedBy(sim, 0, MOVE_SPLASH, 1), "used another move in between");
    CHECK_ROLL(DmgOnTurn(sim, 0, MOVE_FURY_CUTTER, 1, 2), Final(BaseDmg(sim, 0, 1, 20, 0, 0), 1, 1, 10), "counter kept across another move");
    CHECK(sim->disableStructs[0].furyCutterCounter == 2, "counter 2 (%d)", sim->disableStructs[0].furyCutterCounter);
}
static void CheckFuryCutterSwitchResets(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_SCYTHER, "scyther back in");
    CHECK_ROLL(DmgOnTurn(sim, 0, MOVE_FURY_CUTTER, 1, 3), Final(BaseDmg(sim, 0, 1, 10, 0, 0), 1, 1, 10), "base power after switching");
    CHECK(sim->disableStructs[0].furyCutterCounter == 1, "counter restarted (%d)", sim->disableStructs[0].furyCutterCounter);
}
static void CheckFuryCutterProtectResets(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNPROTECTEDITSELF, 1), "protected");
    CHECK_ROLL(DmgOnTurn(sim, 0, MOVE_FURY_CUTTER, 1, 2), Final(BaseDmg(sim, 0, 1, 10, 0, 0), 1, 1, 10), "base power after protect");
    CHECK(sim->disableStructs[0].furyCutterCounter == 1, "counter restarted (%d)", sim->disableStructs[0].furyCutterCounter);
}

// ---------------------------------------------------------------- Rage --------------------------------------------
static void CheckRageBuilds(struct BattleSim *sim)
{
    // Faster Persian tackles before Rage on turn 1 (no boost), after Rage was set on turn 2 (+1), and on turn 3
    // the rage status was cleared at turn start (TryClearRageStatuses) because Splash was chosen.
    CHECK(!LOG_HAS_T(STRINGID_PKMNRAGEBUILDING, 0), "no build before rage was used");
    CHECK(LOG_HAS_T(STRINGID_PKMNRAGEBUILDING, 1), "rage built when hit on turn 2");
    CHECK(!LOG_HAS_T(STRINGID_PKMNRAGEBUILDING, 2), "no build once another move is chosen");
    CHECK(STAGE(0, STAT_ATK) == 7, "+1 attack (stage %d)", STAGE(0, STAT_ATK));
    CHECK(!(STATUS2(0) & STATUS2_RAGE), "rage status cleared");
}
static int WantNoMissT1(struct BattleSim *sim) { return !Sc_LogHas(sim, STRINGID_ATTACKMISSED, 1); }
static void CheckRageMissClears(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_ATTACKMISSED, 0), "rage missed the airborne target");
    CHECK(!LOG_HAS(STRINGID_PKMNRAGEBUILDING), "fly's hit did not build rage: the miss cleared the status");
    CHECK(STAGE(0, STAT_ATK) == 6, "attack unchanged (stage %d)", STAGE(0, STAT_ATK));
    CHECK(STATUS2(0) & STATUS2_RAGE, "rage set again by the turn-2 hit");
}
static void CheckRageStatusMove(struct BattleSim *sim)
{
    CHECK(!LOG_HAS(STRINGID_PKMNRAGEBUILDING), "growl does not build rage");
    CHECK(STAGE(0, STAT_ATK) == 4, "two growls (stage %d)", STAGE(0, STAT_ATK));
}
static void CheckRageSubstitute(struct BattleSim *sim)
{
    CHECK(STATUS2(0) & STATUS2_SUBSTITUTE, "substitute up");
    CHECK(UsedBy(sim, 1, MOVE_TACKLE, 2), "tackled while raging");
    CHECK(!LOG_HAS(STRINGID_PKMNRAGEBUILDING) && STAGE(0, STAT_ATK) == 6, "hits on the substitute do not build rage");
}

// ---------------------------------------------------------------- Thrash / Outrage / Petal Dance -----------------
static int WantFatigueT1(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_PKMNFATIGUECONFUSION, 1); }
static int WantFatigueT2(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_PKMNFATIGUECONFUSION, 2); }
static void CheckThrashTwoTurns(struct BattleSim *sim)
{
    CHECK(UsedBy(sim, 0, MOVE_THRASH, 0) && UsedBy(sim, 0, MOVE_THRASH, 1), "thrashed for two turns");
    CHECK(STATUS2(0) & STATUS2_CONFUSION, "confused");
    CHECK(!(STATUS2(0) & (STATUS2_LOCK_CONFUSE | STATUS2_MULTIPLETURNS)), "lock over");
    CHECK(!UsedBy(sim, 0, MOVE_THRASH, 2) && LOG_HAS_T(STRINGID_PKMNISCONFUSED, 2), "free (confused) choice on turn 3");
    CHECK(PP(0, 0) == 19, "pp deducted once (pp %d)", PP(0, 0));
}
static void CheckThrashThreeTurns(struct BattleSim *sim)
{
    CHECK(UsedBy(sim, 0, MOVE_THRASH, 2), "thrashed for three turns");
    CHECK(!LOG_HAS_T(STRINGID_PKMNFATIGUECONFUSION, 1) && LOG_HAS_T(STRINGID_PKMNFATIGUECONFUSION, 2), "fatigue after the third turn");
    CHECK(STATUS2(0) & STATUS2_CONFUSION, "confused");
    CHECK(PP(0, 0) == 19, "pp deducted once (pp %d)", PP(0, 0));
}
static void CheckThrashProtect(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNPROTECTEDITSELF, 1), "protected");
    CHECK(!(STATUS2(0) & (STATUS2_LOCK_CONFUSE | STATUS2_MULTIPLETURNS | STATUS2_CONFUSION)), "lock ended without confusion");
    CHECK(!LOG_HAS(STRINGID_PKMNFATIGUECONFUSION), "no fatigue message");
    CHECK(PP(0, 0) == 18, "pp deducted again on the protected turn (pp %d)", PP(0, 0));
    CHECK(UsedBy(sim, 0, MOVE_SPLASH, 2), "free choice afterwards");
}
static int WantThrashThenParalysis(struct BattleSim *sim) { return UsedBy(sim, 0, MOVE_THRASH, 0) && Sc_LogHas(sim, STRINGID_PKMNISPARALYZED, 1); }
static void CheckThrashParalysis(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNISPARALYZED, 1), "fully paralyzed mid-thrash");
    CHECK(!(STATUS2(0) & (STATUS2_LOCK_CONFUSE | STATUS2_MULTIPLETURNS | STATUS2_CONFUSION)), "lock ended without confusion");
    CHECK(!LOG_HAS(STRINGID_PKMNFATIGUECONFUSION), "no fatigue message");
}
static int WantAsleepT1(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_PKMNFASTASLEEP, 1); }
static void CheckThrashSleep(struct BattleSim *sim)
{
    CHECK(UsedBy(sim, 0, MOVE_THRASH, 0), "thrash started");
    CHECK(LOG_HAS_T(STRINGID_PKMNFASTASLEEP, 1), "asleep on turn 2");
    CHECK(!(STATUS2(0) & (STATUS2_LOCK_CONFUSE | STATUS2_MULTIPLETURNS | STATUS2_CONFUSION)), "falling asleep cancelled the lock (SetMoveEffect), no confusion");
    CHECK(!LOG_HAS(STRINGID_PKMNFATIGUECONFUSION), "no fatigue message");
}
static void CheckThrashImmune(struct BattleSim *sim)
{
    CHECK(B(1).species == SPECIES_GENGAR && LOG_HAS_T(STRINGID_ITDOESNTAFFECT, 1), "thrash had no effect on the ghost");
    CHECK(!(STATUS2(0) & (STATUS2_LOCK_CONFUSE | STATUS2_MULTIPLETURNS | STATUS2_CONFUSION)), "no-effect turn ends the lock without confusion");
    CHECK(!LOG_HAS(STRINGID_PKMNFATIGUECONFUSION), "no fatigue message");
}
static void CheckThrashFaint(struct BattleSim *sim)
{
    CHECK(PARTY_HP(B_SIDE_OPPONENT, 0) == 0 && PARTY_HP(B_SIDE_OPPONENT, 1) == 0, "both rattata fainted");
    CHECK(UsedBy(sim, 0, MOVE_THRASH, 1), "thrash continued on the replacement");
    CHECK(B(1).species == SPECIES_STEELIX, "steelix out");
}
static void CheckOutrage(struct BattleSim *sim)
{
    CHECK(UsedBy(sim, 0, MOVE_OUTRAGE, 0) && UsedBy(sim, 0, MOVE_OUTRAGE, 1), "outrage locked");
    CHECK(LOG_HAS(STRINGID_PKMNFATIGUECONFUSION) && (STATUS2(0) & STATUS2_CONFUSION), "confused after 2-3 turns");
}
static void CheckPetalDance(struct BattleSim *sim)
{
    CHECK(UsedBy(sim, 0, MOVE_PETAL_DANCE, 0) && UsedBy(sim, 0, MOVE_PETAL_DANCE, 1), "petal dance locked");
    CHECK(LOG_HAS(STRINGID_PKMNFATIGUECONFUSION) && (STATUS2(0) & STATUS2_CONFUSION), "confused after 2-3 turns");
}
static void CheckEndureThrash(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNENDUREDHIT, 0), "endured");
    CHECK(HP(1) == 1, "left at 1 hp (hp %d)", HP(1));
    CHECK(STATUS2(0) & STATUS2_LOCK_CONFUSE, "thrash keeps going");
}

// ---------------------------------------------------------------- Uproar ------------------------------------------
static int WantCalmT1(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_PKMNCALMEDDOWN, 1); }
static void CheckUproarTwoTurns(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNCAUSEDUPROAR, 0), "caused an uproar");
    CHECK(LOG_HAS_T(STRINGID_PKMNMAKINGUPROAR, 0), "still making an uproar at the end of turn 1");
    CHECK(UsedBy(sim, 0, MOVE_UPROAR, 1), "uproar continued on turn 2");
    CHECK(LOG_HAS_T(STRINGID_PKMNCALMEDDOWN, 1), "calmed down at the end of turn 2");
    CHECK(!(STATUS2(0) & (STATUS2_UPROAR | STATUS2_MULTIPLETURNS)), "uproar over");
    CHECK(UsedBy(sim, 0, MOVE_SPLASH, 2), "free choice on turn 3");
    CHECK(PP(0, 0) == 9, "pp deducted once (pp %d)", PP(0, 0));
}
static void CheckUproarWakesEndTurn(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNFASTASLEEP, 0), "faster sleeper was still asleep when it moved");
    CHECK(LOG_HAS_T(STRINGID_PKMNWOKEUPINUPROAR, 0), "woke up in the uproar at the end of the turn");
    CHECK(!(STATUS1(1) & STATUS1_SLEEP), "awake");
}
static void CheckUproarWakesOnAction(struct BattleSim *sim)
{
    CHECK(!LOG_HAS(STRINGID_PKMNFASTASLEEP), "never reported as fast asleep");
    CHECK(LOG_HAS_T(STRINGID_PKMNWOKEUPINUPROAR, 0), "woke up in the uproar when trying to move");
    CHECK(UsedBy(sim, 1, MOVE_SPLASH, 0), "moved right after waking");
    CHECK(!(STATUS1(1) & STATUS1_SLEEP), "awake");
}
static void CheckUproarSoundproofUserSlept(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNCAUSEDUPROAR, 0) && LOG_HAS_T(STRINGID_PKMNMAKINGUPROAR, 0), "exploud's uproar is running");
    CHECK(UsedBy(sim, 1, MOVE_HYPNOSIS, 1) && LOG_HAS_T(STRINGID_PKMNFELLASLEEP, 1), "soundproof exploud is put to sleep during its own uproar");
    CHECK(STATUS1(0) & STATUS1_SLEEP, "asleep");
    CHECK(!(STATUS2(0) & (STATUS2_UPROAR | STATUS2_MULTIPLETURNS)), "the sleep cancelled the uproar");
    CHECK(!LOG_HAS_T(STRINGID_PKMNCALMEDDOWN, 1) && !LOG_HAS_T(STRINGID_PKMNMAKINGUPROAR, 1), "no uproar end-turn message once cancelled");
}
static int WantHypnosisAttempted(struct BattleSim *sim) { return UsedBy(sim, 1, MOVE_HYPNOSIS, 1); }
static void CheckUproarBlocksSleep(struct BattleSim *sim)
{
    CHECK(UsedBy(sim, 1, MOVE_HYPNOSIS, 1), "hypnosis attempted during the uproar");
    CHECK(LOG_HAS_T(STRINGID_PKMNCANTSLEEPINUPROAR2, 1), "can't sleep in an uproar");
    CHECK(!(STATUS1(0) & STATUS1_SLEEP), "not asleep");
}
static void CheckUproarBlocksRest(struct BattleSim *sim)
{
    CHECK(UsedBy(sim, 1, MOVE_REST, 1), "rest attempted");
    CHECK(LOG_HAS_T(STRINGID_UPROARKEPTPKMNAWAKE, 1), "uproar kept it awake");
    CHECK(!(STATUS1(1) & STATUS1_SLEEP) && HP(1) < MAXHP(1), "rest failed");
}
static void CheckUproarSoundproof(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNSXBLOCKSY, 0), "soundproof blocks uproar");
    CHECK(HP(1) == MAXHP(1), "no damage");
    CHECK(!(STATUS2(0) & (STATUS2_UPROAR | STATUS2_MULTIPLETURNS)), "no uproar started");
    CHECK(PP(0, 0) == 9, "pp still deducted (pp %d)", PP(0, 0));
}
static int WantUproarThenFlinch(struct BattleSim *sim)
{
    return Sc_LogHas(sim, STRINGID_PKMNCAUSEDUPROAR, 0) && Sc_LogHas(sim, STRINGID_PKMNFLINCHED, 1) && UsedBy(sim, 0, MOVE_SPLASH, 2);
}
static void CheckUproarFlinch(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNFLINCHED, 1), "flinched mid-uproar");
    CHECK(!(STATUS2(0) & (STATUS2_UPROAR | STATUS2_MULTIPLETURNS)), "uproar cancelled");
    CHECK(!LOG_HAS_T(STRINGID_PKMNCALMEDDOWN, 1) && !LOG_HAS_T(STRINGID_PKMNMAKINGUPROAR, 1), "no uproar end-turn message after the cancel");
    CHECK(UsedBy(sim, 0, MOVE_SPLASH, 2), "free choice afterwards");
}

// ---------------------------------------------------------------- Focus Punch -------------------------------------
static void CheckFocusPunchHits(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNTIGHTENINGFOCUS, 0), "tightening focus at turn start");
    CHECK(!LOG_HAS(STRINGID_PKMNLOSTFOCUS) && HP(1) < MAXHP(1), "status move does not break focus");
    CHECK(MOVED_BEFORE(1, 0, 0), "focus punch goes last");
    CHECK(PP(0, 0) == 19, "pp %d", PP(0, 0));
}
static void CheckFocusPunchLost(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNTIGHTENINGFOCUS, 0) && LOG_HAS_T(STRINGID_PKMNLOSTFOCUS, 0), "lost focus");
    CHECK(HP(1) == MAXHP(1), "no damage");
    CHECK(PP(0, 0) == 19, "pp still deducted (pp %d)", PP(0, 0));
}
static void CheckFocusPunchSubstitute(struct BattleSim *sim)
{
    CHECK(STATUS2(0) & STATUS2_SUBSTITUTE, "substitute up");
    CHECK(UsedBy(sim, 1, MOVE_TACKLE, 1), "tackled the substitute");
    CHECK(!LOG_HAS(STRINGID_PKMNLOSTFOCUS) && HP(1) < MAXHP(1), "damage to the substitute does not break focus");
}
static void CheckFocusPunchAsleep(struct BattleSim *sim)
{
    CHECK(!LOG_HAS(STRINGID_PKMNTIGHTENINGFOCUS), "no focus message while asleep");
    CHECK(LOG_HAS_T(STRINGID_PKMNFASTASLEEP, 0) && HP(1) == MAXHP(1), "slept through");
}
static void CheckFocusPunchProtect(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNTIGHTENINGFOCUS, 0), "focus set up");
    CHECK(LOG_HAS_T(STRINGID_PKMNPROTECTEDITSELF, 0) && HP(1) == MAXHP(1), "protected");
    CHECK(PP(0, 0) == 19, "pp %d", PP(0, 0));
}

// ---------------------------------------------------------------- Fake Out ----------------------------------------
static void CheckFakeOut(struct BattleSim *sim)
{
    CHECK(MOVED_BEFORE(0, 1, 1) || !LOG_HAS_B(STRINGID_USEDMOVE, 1, 0), "fake out has +1 priority");
    CHECK(LOG_HAS_T(STRINGID_PKMNFLINCHED, 0) && !LOG_HAS_B(STRINGID_USEDMOVE, 1, 0), "target flinched on turn 1");
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 1), "fails on turn 2");
    CHECK(UsedBy(sim, 1, MOVE_TACKLE, 1), "target moved on turn 2");
    CHECK(PP(0, 0) == 8, "pp deducted both times (pp %d)", PP(0, 0));
}
static void CheckFakeOutAfterSwitch(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_PERSIAN, "persian in");
    CHECK(LOG_HAS_T(STRINGID_PKMNFLINCHED, 1), "works on the first turn after switching in");
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 2), "fails the turn after");
}
static void CheckFakeOutInnerFocus(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNSXPREVENTSFLINCHING, 0), "inner focus prevents the flinch");
    CHECK(HP(1) < MAXHP(1), "damage still dealt");
    CHECK(UsedBy(sim, 1, MOVE_TACKLE, 0), "target still moved");
}
static void CheckFakeOutSubstitute(struct BattleSim *sim)
{
    CHECK(STATUS2(1) & STATUS2_SUBSTITUTE, "substitute still up");
    CHECK(UsedBy(sim, 0, MOVE_FAKE_OUT, 2), "fake out used on the first turn after switching in");
    CHECK(!LOG_HAS(STRINGID_PKMNFLINCHED) && UsedBy(sim, 1, MOVE_TACKLE, 2), "substitute blocks the flinch");
}

// ---------------------------------------------------------------- Truant ------------------------------------------
static void CheckTruantFly(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNFLEWHIGH, 0), "flew up on turn 1");
    CHECK(LOG_HAS_T(STRINGID_PKMNLOAFING, 1), "loafing on turn 2");
    CHECK(HP(1) == MAXHP(1), "fly never landed");
    CHECK(LOG_HAS_T(STRINGID_PKMNFLEWHIGH, 2), "started over on turn 3");
    CHECK(PP(0, 0) == 13, "pp deducted for each charge (pp %d)", PP(0, 0));
}
static void CheckTruantHyperBeam(struct BattleSim *sim)
{
    CHECK(UsedBy(sim, 0, MOVE_HYPER_BEAM, 0), "hyper beam on turn 1");
    CHECK(LOG_HAS_T(STRINGID_PKMNLOAFING, 1) && !LOG_HAS(STRINGID_PKMNMUSTRECHARGE), "truant check comes before the recharge check");
    CHECK(UsedBy(sim, 0, MOVE_HYPER_BEAM, 2) && DmgOnTurn(sim, 0, MOVE_HYPER_BEAM, 1, 2) > 0, "attacks again on turn 3: the first recharge expired with its timer");
    CHECK((STATUS2(0) & STATUS2_RECHARGE) && sim->disableStructs[0].rechargeTimer == 1, "turn-3 beam set a fresh recharge (timer %d)", sim->disableStructs[0].rechargeTimer);
}
static void CheckTruantThrash(struct BattleSim *sim)
{
    CHECK(UsedBy(sim, 0, MOVE_THRASH, 0) && LOG_HAS_T(STRINGID_PKMNLOAFING, 1), "thrash then loaf");
    CHECK(!LOG_HAS(STRINGID_PKMNFATIGUECONFUSION) && !(STATUS2(0) & STATUS2_CONFUSION), "loafing cancels the thrash without confusion");
    CHECK(UsedBy(sim, 0, MOVE_THRASH, 2), "new thrash on turn 3");
    CHECK(PP(0, 0) == 18, "pp for each start (pp %d)", PP(0, 0));
}
static void CheckTruantRollout(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNLOAFING, 1), "loafing on turn 2");
    CHECK(UsedBy(sim, 0, MOVE_ROLLOUT, 2) && sim->disableStructs[0].rolloutTimer == 4, "rollout restarted from the first hit (timer %d)", sim->disableStructs[0].rolloutTimer);
    CHECK(PP(0, 0) == 18, "pp for each start (pp %d)", PP(0, 0));
}
static void CheckTruantFocusPunch(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNTIGHTENINGFOCUS, 0) && HP(1) < MAXHP(1), "focus punch on turn 1");
    CHECK(!LOG_HAS_T(STRINGID_PKMNTIGHTENINGFOCUS, 1) && LOG_HAS_T(STRINGID_PKMNLOAFING, 1), "no focus message on the loafing turn");
}

// ---------------------------------------------------------------- scenarios ---------------------------------------
static const struct Scenario sScenarios[] =
{
    // --- Solar Beam ---
    { .name = "solar_beam_charge_pp_damage",
      .player = { M1(SPECIES_VENUSAUR, 50, MOVE_SOLAR_BEAM) }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .wantSeed = NoCrit, .check = CheckSolarBeamCharge },
    { .name = "solar_beam_sun_no_charge",
      .player = { MON(SPECIES_VENUSAUR, 50, MOVE_SUNNY_DAY, MOVE_SOLAR_BEAM, MOVE_SPLASH, MOVE_SPLASH) }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .wantSeed = NoCrit, .check = CheckSolarBeamSun },
    { .name = "solar_beam_rain_halves",
      .player = { M1(SPECIES_VENUSAUR, 50, MOVE_SOLAR_BEAM) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_RAIN_DANCE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 1) }, .turns = 2, .wantSeed = NoCrit, .check = CheckSolarBeamRain },
    { .name = "solar_beam_sandstorm_halves",
      .player = { M1(SPECIES_VENUSAUR, 50, MOVE_SOLAR_BEAM) },
      .enemy = { MON(SPECIES_STEELIX, 50, MOVE_SANDSTORM, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },   // steel/ground: neutral to grass, no chip
      .actions = { T(0, 0), T(0, 1) }, .turns = 2, .wantSeed = NoCrit, .check = CheckSolarBeamSandstorm },
    { .name = "solar_beam_hail_halves",
      .player = { M1(SPECIES_VENUSAUR, 50, MOVE_SOLAR_BEAM) },
      .enemy = { MON(SPECIES_GLALIE, 50, MOVE_HAIL, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },   // pure ice: neutral to grass, no hail chip
      .actions = { T(0, 0), T(0, 1) }, .turns = 2, .wantSeed = NoCrit, .check = CheckSolarBeamHail },
    { .name = "solar_beam_sun_cloud_nine_charges",
      .player = { MON(SPECIES_VENUSAUR, 50, MOVE_SUNNY_DAY, MOVE_SOLAR_BEAM, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { { .species = SPECIES_GOLDUCK, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .abilityNum = 1 } },   // Cloud Nine
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckSolarBeamCloudNine },
    { .name = "two_turn_protect_on_attack_turn",
      .player = { MON(SPECIES_VENUSAUR, 50, MOVE_SOLAR_BEAM, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_PROTECT, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 1), T(1, 0) }, .turns = 3, .check = CheckTwoTurnProtectSecondTurn },
    { .name = "two_turn_protect_on_charge_turn_ignored",
      .player = { M1(SPECIES_VENUSAUR, 50, MOVE_SOLAR_BEAM) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_PROTECT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 1) }, .turns = 2, .check = CheckTwoTurnProtectChargeTurn },
    { .name = "two_turn_flinch_cancels",
      .player = { MON(SPECIES_EXEGGUTOR, 50, MOVE_SOLAR_BEAM, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { M1(SPECIES_PERSIAN, 50, MOVE_BITE) },
      .actions = { T(0, 0), T(0, 0), T(1, 0) }, .turns = 3, .wantSeed = WantChargeThenFlinch, .check = CheckTwoTurnFlinch },
    { .name = "two_turn_full_paralysis_cancels",
      .player = { { .species = SPECIES_EXEGGUTOR, .level = 50, .moves = { MOVE_SOLAR_BEAM, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .status = STATUS1_PARALYSIS } },
      .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0), T(0, 0), T(1, 0) }, .turns = 3, .wantSeed = WantChargeThenParalysis, .check = CheckTwoTurnParalysis },
    { .name = "two_turn_roared_out_mid_charge",
      .player = { M1(SPECIES_EXEGGUTOR, 50, MOVE_SOLAR_BEAM), M0(SPECIES_RATTATA, 50) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_ROAR, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 1) }, .turns = 2, .check = CheckTwoTurnRoared },
    { .name = "two_turn_target_switches",
      .player = { M1(SPECIES_VENUSAUR, 50, MOVE_SOLAR_BEAM) },
      .enemy = { SNORLAX_SPLASH, STEELIX_SPLASH },
      .actions = { T(0, 0), T(0, SC_SWITCH(1)) }, .turns = 2, .check = CheckTwoTurnTargetSwitched },
    // --- Razor Wind / Skull Bash / Sky Attack ---
    { .name = "razor_wind_two_turns",
      .player = { M1(SPECIES_PIDGEOT, 50, MOVE_RAZOR_WIND) }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .wantSeed = NoCrit, .check = CheckRazorWind },
    { .name = "skull_bash_defense_on_charge",
      .player = { M1(SPECIES_BLASTOISE, 50, MOVE_SKULL_BASH) }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .wantSeed = NoCrit, .check = CheckSkullBash },
    { .name = "sky_attack_flinch",
      .player = { M1(SPECIES_DODRIO, 50, MOVE_SKY_ATTACK) },
      .enemy = { M1(SPECIES_SNORLAX, 50, MOVE_TACKLE) },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .wantSeed = WantFlinchT1, .check = CheckSkyAttackFlinch },
    { .name = "sky_attack_miss_ends_lock",
      .player = { MON(SPECIES_DODRIO, 50, MOVE_SKY_ATTACK, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0), T(0, 0), T(1, 0) }, .turns = 3, .wantSeed = WantMissT1, .check = CheckSkyAttackMiss },
    // --- semi-invulnerable ---
    { .name = "fly_gust_double_damage",
      .player = { M1(SPECIES_PIDGEOT, 50, MOVE_FLY) },
      .enemy = { M1(SPECIES_PIDGEOT, 50, MOVE_GUST) },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .wantSeed = NoCritNoMiss, .check = CheckFlyGust },
    { .name = "fly_twister_double_damage",
      .player = { M1(SPECIES_PIDGEOT, 50, MOVE_FLY) },
      .enemy = { M1(SPECIES_DRAGONAIR, 50, MOVE_TWISTER) },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .wantSeed = NoCritNoMiss, .check = CheckFlyTwister },
    { .name = "fly_thunder_hits",
      .player = { M1(SPECIES_PIDGEOT, 50, MOVE_FLY) },
      .enemy = { M1(SPECIES_AMPHAROS, 20, MOVE_THUNDER) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantPlayerDamaged, .check = CheckFlyThunder },
    { .name = "fly_lock_on_hits",
      .player = { MON(SPECIES_PIDGEOT, 50, MOVE_SPLASH, MOVE_FLY, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_LOCK_ON, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .check = CheckLockOnFly },
    { .name = "dig_mind_reader_hits",
      .player = { MON(SPECIES_SANDSLASH, 50, MOVE_SPLASH, MOVE_DIG, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_MIND_READER, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .check = CheckMindReaderDig },
    { .name = "dig_earthquake_double_damage",
      .player = { M1(SPECIES_SANDSLASH, 50, MOVE_DIG) },
      .enemy = { M1(SPECIES_DONPHAN, 30, MOVE_EARTHQUAKE) },   // neutral to Dig, bulky enough for a doubled EQ not to matter
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .wantSeed = NoCritNoMiss, .check = CheckDigEarthquake },
    { .name = "dig_magnitude_hits",
      .player = { M1(SPECIES_SANDSLASH, 50, MOVE_DIG) },
      .enemy = { M1(SPECIES_RHYDON, 30, MOVE_MAGNITUDE) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckDigMagnitude },
    { .name = "dive_surf_double_damage",
      .player = { M1(SPECIES_QUAGSIRE, 50, MOVE_DIVE) },
      .enemy = { M1(SPECIES_SLOWPOKE, 30, MOVE_SURF) },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .wantSeed = NoCritNoMiss, .check = CheckDiveSurf },
    { .name = "dive_whirlpool_double_damage",
      .player = { M1(SPECIES_QUAGSIRE, 50, MOVE_DIVE) },
      .enemy = { M1(SPECIES_SLOWPOKE, 30, MOVE_WHIRLPOOL) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = NoCritNoMiss, .check = CheckDiveWhirlpool },
    { .name = "dig_dodges_hypnosis",
      .player = { M1(SPECIES_SANDSLASH, 50, MOVE_DIG) },
      .enemy = { M1(SPECIES_HYPNO, 50, MOVE_HYPNOSIS) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckDigDodgesHypnosis },
    { .name = "fly_full_paralysis_lands",
      .player = { { .species = SPECIES_PIDGEOT, .level = 50, .moves = { MOVE_FLY, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .status = STATUS1_PARALYSIS } },
      .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .wantSeed = WantFlyThenParalysis, .check = CheckFlyParalysis },
    { .name = "fly_yawn_sleep_lands_at_end_of_turn",
      .player = { MON(SPECIES_PIDGEOT, 50, MOVE_SPLASH, MOVE_FLY, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_HYPNO, 50, MOVE_YAWN, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1), T(1, 1) }, .turns = 3, .check = CheckFlyYawnSleep },
    { .name = "sleep_talk_thrash_lock_cancelled_asleep",
      .player = { { .species = SPECIES_TAUROS, .level = 50, .moves = { MOVE_SLEEP_TALK, MOVE_THRASH, MOVE_SPLASH, MOVE_SPLASH }, .status = STATUS1_SLEEP_TURN(5) } },
      .enemy = { STEELIX_SPLASH },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .wantSeed = WantSleepTalkThrash, .check = CheckSleepTalkThrash },
    { .name = "dig_substitute_takes_earthquake",
      .player = { MON(SPECIES_SANDSLASH, 50, MOVE_SUBSTITUTE, MOVE_DIG, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RHYDON, 40, MOVE_SPLASH, MOVE_EARTHQUAKE, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .check = CheckDigSubstituteEarthquake },
    // --- recharge ---
    { .name = "hyper_beam_recharge_turn",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_HYPER_BEAM, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) }, .enemy = { STEELIX_SPLASH },
      .actions = { T(0, 0), T(0, 0), T(1, 0) }, .turns = 3, .wantSeed = WantHitT0, .check = CheckHyperBeamRecharge },
    { .name = "hyper_beam_miss_no_recharge",
      .player = { M1(SPECIES_SNORLAX, 50, MOVE_HYPER_BEAM) }, .enemy = { STEELIX_SPLASH },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .wantSeed = WantMissThenHit, .check = CheckHyperBeamMiss },
    { .name = "hyper_beam_protect_no_recharge",
      .player = { M1(SPECIES_SNORLAX, 50, MOVE_HYPER_BEAM) },
      .enemy = { MON(SPECIES_STEELIX, 50, MOVE_PROTECT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 1) }, .turns = 2, .check = CheckHyperBeamProtect },
    { .name = "hyper_beam_ko_still_recharges",
      .player = { M1(SPECIES_SNORLAX, 50, MOVE_HYPER_BEAM) },
      .enemy = { M0(SPECIES_RATTATA, 5), STEELIX_SPLASH },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .wantSeed = WantFirstEnemyFainted, .check = CheckHyperBeamKo },
    { .name = "hyper_beam_immune_no_recharge",
      .player = { M1(SPECIES_SNORLAX, 50, MOVE_HYPER_BEAM) }, .enemy = { M0(SPECIES_GENGAR, 50) },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .check = CheckHyperBeamImmune },
    { .name = "recharge_absorbed_by_sleep",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_HYPER_BEAM, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_HYPNO, 50, MOVE_YAWN, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1), T(1, 1) }, .turns = 3, .wantSeed = WantHyperBeamHitT1, .check = CheckRechargeSleep },
    { .name = "hydro_cannon_recharge",
      .player = { M1(SPECIES_BLASTOISE, 50, MOVE_HYDRO_CANNON) }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .wantSeed = WantHitT0, .check = CheckHydroCannon },
    // --- Bide ---
    { .name = "bide_double_damage",
      .player = { M1(SPECIES_SLOWPOKE, 50, MOVE_BIDE) },
      .enemy = { M1(SPECIES_BLISSEY, 50, MOVE_SEISMIC_TOSS) },
      .actions = { T(0, 0), T(0, 0), T(0, 0) }, .turns = 3, .check = CheckBide },
    { .name = "bide_ignores_type_resistance",
      .player = { M1(SPECIES_SLOWPOKE, 50, MOVE_BIDE) },
      .enemy = { MON(SPECIES_STEELIX, 50, MOVE_SPLASH, MOVE_SEISMIC_TOSS, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 1), T(0, 0) }, .turns = 3, .check = CheckBideIgnoresResistance },
    { .name = "bide_no_damage_fails",
      .player = { M1(SPECIES_SLOWPOKE, 50, MOVE_BIDE) }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0), T(0, 0), T(0, 0) }, .turns = 3, .check = CheckBideNoDamage },
    { .name = "bide_ghost_immune",
      .player = { M1(SPECIES_SLOWPOKE, 50, MOVE_BIDE) },
      .enemy = { M1(SPECIES_GENGAR, 50, MOVE_NIGHT_SHADE) },
      .actions = { T(0, 0), T(0, 0), T(0, 0) }, .turns = 3, .check = CheckBideGhost },
    { .name = "bide_protect_blocks_release",
      .player = { M1(SPECIES_SLOWPOKE, 50, MOVE_BIDE) },
      .enemy = { MON(SPECIES_STEELIX, 50, MOVE_SPLASH, MOVE_SEISMIC_TOSS, MOVE_PROTECT, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 1), T(0, 2) }, .turns = 3, .check = CheckBideProtect },
    { .name = "bide_hits_replacement",
      .player = { M1(SPECIES_SLOWPOKE, 50, MOVE_BIDE) },
      .enemy = { MON(SPECIES_BLISSEY, 50, MOVE_SPLASH, MOVE_SEISMIC_TOSS, MOVE_SPLASH, MOVE_SPLASH), STEELIX_SPLASH },
      .actions = { T(0, 0), T(0, 1), T(0, SC_SWITCH(1)) }, .turns = 3, .check = CheckBideSwitch },
    // --- Rollout / Ice Ball ---
    { .name = "rollout_five_hits_doubling",
      .player = { MON(SPECIES_GOLEM, 30, MOVE_ROLLOUT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) }, .enemy = { M0(SPECIES_SNORLAX, 70) },
      .actions = { T(0, 0), T(0, 0), T(0, 0), T(0, 0), T(0, 0), T(1, 0) }, .turns = 6, .wantSeed = NoCritNoMiss, .check = CheckRolloutFive },
    { .name = "rollout_defense_curl_doubles",
      .player = { MON(SPECIES_GOLEM, 30, MOVE_DEFENSE_CURL, MOVE_ROLLOUT, MOVE_SPLASH, MOVE_SPLASH) }, .enemy = { M0(SPECIES_SNORLAX, 70) },
      .actions = { T(0, 0), T(1, 0), T(1, 0) }, .turns = 3, .wantSeed = NoCritNoMiss, .check = CheckRolloutDefenseCurl },
    { .name = "rollout_miss_ends_sequence",
      .player = { MON(SPECIES_GOLEM, 30, MOVE_ROLLOUT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) }, .enemy = { M0(SPECIES_SNORLAX, 70) },
      .actions = { T(0, 0), T(0, 0), T(1, 0) }, .turns = 3, .wantSeed = WantHitThenMiss, .check = CheckRolloutMiss },
    { .name = "rollout_protect_ends_sequence_costs_pp",
      .player = { MON(SPECIES_GOLEM, 30, MOVE_ROLLOUT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 70, MOVE_SPLASH, MOVE_PROTECT, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 1), T(1, 0) }, .turns = 3, .wantSeed = NoMiss, .check = CheckRolloutProtect },
    { .name = "rollout_continues_after_ko",
      .player = { M1(SPECIES_GOLEM, 50, MOVE_ROLLOUT) },
      .enemy = { M0(SPECIES_RATTATA, 5), M0(SPECIES_RATTATA, 5), STEELIX_SPLASH },
      .actions = { T(0, 0), T(0, 0), T(0, 0) }, .turns = 3, .wantSeed = NoMiss, .check = CheckRolloutFaint },
    { .name = "ice_ball_locks",
      .player = { M1(SPECIES_CLOYSTER, 50, MOVE_ICE_BALL) }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = NoMiss, .check = CheckIceBall },
    // --- Fury Cutter ---
    { .name = "fury_cutter_growth_caps_at_5",
      .player = { M1(SPECIES_SCYTHER, 30, MOVE_FURY_CUTTER) }, .enemy = { M0(SPECIES_SNORLAX, 60) },
      .actions = { T(0, 0), T(0, 0), T(0, 0), T(0, 0), T(0, 0), T(0, 0) }, .turns = 6, .wantSeed = NoCritNoMiss, .check = CheckFuryCutterGrowth },
    { .name = "fury_cutter_miss_resets",
      .player = { M1(SPECIES_SCYTHER, 30, MOVE_FURY_CUTTER) }, .enemy = { M0(SPECIES_SNORLAX, 60) },
      .actions = { T(0, 0), T(0, 0), T(0, 0) }, .turns = 3, .wantSeed = WantFuryCutterMissT1, .check = CheckFuryCutterMissResets },
    { .name = "fury_cutter_other_move_keeps_counter",
      .player = { MON(SPECIES_SCYTHER, 30, MOVE_FURY_CUTTER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) }, .enemy = { M0(SPECIES_SNORLAX, 60) },
      .actions = { T(0, 0), T(1, 0), T(0, 0) }, .turns = 3, .wantSeed = NoCritNoMiss, .check = CheckFuryCutterOtherMove },
    { .name = "fury_cutter_switch_resets",
      .player = { M1(SPECIES_SCYTHER, 30, MOVE_FURY_CUTTER), M0(SPECIES_RATTATA, 50) }, .enemy = { M0(SPECIES_SNORLAX, 60) },
      .actions = { T(0, 0), T(SC_SWITCH(1), 0), T(SC_SWITCH(0), 0), T(0, 0) }, .turns = 4, .wantSeed = NoCritNoMiss, .check = CheckFuryCutterSwitchResets },
    { .name = "fury_cutter_protect_resets",
      .player = { M1(SPECIES_SCYTHER, 30, MOVE_FURY_CUTTER) },
      .enemy = { MON(SPECIES_SNORLAX, 60, MOVE_SPLASH, MOVE_PROTECT, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 1), T(0, 0) }, .turns = 3, .wantSeed = NoCritNoMiss, .check = CheckFuryCutterProtectResets },
    // --- Rage ---
    { .name = "rage_builds_when_hit",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_RAGE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { M1(SPECIES_PERSIAN, 50, MOVE_TACKLE) },
      .actions = { T(0, 0), T(0, 0), T(1, 0) }, .turns = 3, .check = CheckRageBuilds },
    { .name = "rage_miss_clears_status",
      .player = { M1(SPECIES_SNORLAX, 50, MOVE_RAGE) },
      .enemy = { M1(SPECIES_PIDGEOT, 50, MOVE_FLY) },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .wantSeed = WantNoMissT1, .check = CheckRageMissClears },
    { .name = "rage_not_built_by_status_moves",
      .player = { M1(SPECIES_SNORLAX, 50, MOVE_RAGE) },
      .enemy = { M1(SPECIES_PERSIAN, 50, MOVE_GROWL) },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .check = CheckRageStatusMove },
    { .name = "rage_not_built_behind_substitute",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SUBSTITUTE, MOVE_RAGE, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_PERSIAN, 50, MOVE_SPLASH, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1), T(1, 1) }, .turns = 3, .check = CheckRageSubstitute },
    // --- Thrash & co ---
    { .name = "thrash_two_turns_then_confusion",
      .player = { MON(SPECIES_TAUROS, 50, MOVE_THRASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) }, .enemy = { STEELIX_SPLASH },
      .actions = { T(0, 0), T(0, 0), T(1, 0) }, .turns = 3, .wantSeed = WantFatigueT1, .check = CheckThrashTwoTurns },
    { .name = "thrash_three_turns_then_confusion",
      .player = { MON(SPECIES_TAUROS, 50, MOVE_THRASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) }, .enemy = { STEELIX_SPLASH },
      .actions = { T(0, 0), T(0, 0), T(1, 0) }, .turns = 3, .wantSeed = WantFatigueT2, .check = CheckThrashThreeTurns },
    { .name = "thrash_protect_ends_no_confusion",
      .player = { MON(SPECIES_TAUROS, 50, MOVE_THRASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_STEELIX, 50, MOVE_SPLASH, MOVE_PROTECT, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 1), T(1, 0) }, .turns = 3, .check = CheckThrashProtect },
    { .name = "thrash_full_paralysis_ends_no_confusion",
      .player = { { .species = SPECIES_TAUROS, .level = 50, .moves = { MOVE_THRASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .status = STATUS1_PARALYSIS } },
      .enemy = { STEELIX_SPLASH },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .wantSeed = WantThrashThenParalysis, .check = CheckThrashParalysis },
    { .name = "thrash_sleep_ends_no_confusion",
      .player = { M1(SPECIES_TAUROS, 50, MOVE_THRASH) },
      .enemy = { M1(SPECIES_HYPNO, 50, MOVE_HYPNOSIS) },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .wantSeed = WantAsleepT1, .check = CheckThrashSleep },
    { .name = "thrash_immune_target_ends_no_confusion",
      .player = { M1(SPECIES_TAUROS, 50, MOVE_THRASH) },
      .enemy = { SNORLAX_SPLASH, M0(SPECIES_GENGAR, 50) },
      .actions = { T(0, 0), T(0, SC_SWITCH(1)) }, .turns = 2, .check = CheckThrashImmune },
    { .name = "thrash_continues_after_ko",
      .player = { M1(SPECIES_TAUROS, 50, MOVE_THRASH) },
      .enemy = { M0(SPECIES_RATTATA, 5), M0(SPECIES_RATTATA, 5), STEELIX_SPLASH },
      .actions = { T(0, 0), T(0, 0), T(0, 0) }, .turns = 3, .check = CheckThrashFaint },
    { .name = "outrage_lock_confusion",
      .player = { M1(SPECIES_DRAGONITE, 50, MOVE_OUTRAGE) }, .enemy = { STEELIX_SPLASH },
      .actions = { T(0, 0), T(0, 0), T(0, 0) }, .turns = 3, .check = CheckOutrage },
    { .name = "petal_dance_lock_confusion",
      .player = { M1(SPECIES_VILEPLUME, 50, MOVE_PETAL_DANCE) }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0), T(0, 0), T(0, 0) }, .turns = 3, .wantSeed = NoCrit, .check = CheckPetalDance },
    { .name = "endure_survives_thrash",
      .player = { M1(SPECIES_TAUROS, 50, MOVE_THRASH) },
      .enemy = { M1(SPECIES_RATTATA, 5, MOVE_ENDURE) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckEndureThrash },
    // --- Uproar ---
    { .name = "uproar_two_turns",
      .player = { MON(SPECIES_EXPLOUD, 50, MOVE_UPROAR, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) }, .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0), T(0, 0), T(1, 0) }, .turns = 3, .wantSeed = WantCalmT1, .check = CheckUproarTwoTurns },
    { .name = "uproar_wakes_sleeper_at_end_of_turn",
      .player = { M1(SPECIES_EXPLOUD, 50, MOVE_UPROAR) },
      .enemy = { { .species = SPECIES_PERSIAN, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .status = STATUS1_SLEEP_TURN(5) } },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckUproarWakesEndTurn },
    { .name = "uproar_wakes_sleeper_when_it_moves",
      .player = { M1(SPECIES_EXPLOUD, 50, MOVE_UPROAR) },
      .enemy = { { .species = SPECIES_SNORLAX, .level = 50, .moves = { MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .status = STATUS1_SLEEP_TURN(5) } },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckUproarWakesOnAction },
    { .name = "uproar_soundproof_user_can_be_put_to_sleep",   // Exploud's own Soundproof skips the uproar sleep check
      .player = { M1(SPECIES_EXPLOUD, 50, MOVE_UPROAR) },
      .enemy = { MON(SPECIES_HYPNO, 50, MOVE_SPLASH, MOVE_HYPNOSIS, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 1) }, .turns = 2, .wantSeed = WantHypnosisAttempted, .check = CheckUproarSoundproofUserSlept },
    { .name = "uproar_blocks_hypnosis",
      .player = { M1(SPECIES_PERSIAN, 50, MOVE_UPROAR) },   // Limber: no Soundproof on the uproar user
      .enemy = { MON(SPECIES_HYPNO, 50, MOVE_SPLASH, MOVE_HYPNOSIS, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 1) }, .turns = 2, .wantSeed = WantHypnosisAttempted, .check = CheckUproarBlocksSleep },
    { .name = "uproar_blocks_rest",
      .player = { M1(SPECIES_EXPLOUD, 50, MOVE_UPROAR) },
      .enemy = { { .species = SPECIES_SNORLAX, .level = 50, .moves = { MOVE_SPLASH, MOVE_REST, MOVE_SPLASH, MOVE_SPLASH }, .hp = 200, .hpSet = 1 } },
      .actions = { T(0, 0), T(0, 1) }, .turns = 2, .wantSeed = NoCrit, .check = CheckUproarBlocksRest },
    { .name = "uproar_soundproof",
      .player = { M1(SPECIES_EXPLOUD, 50, MOVE_UPROAR) },
      .enemy = { M0(SPECIES_VOLTORB, 50) },   // Soundproof
      .actions = { T(0, 0) }, .turns = 1, .check = CheckUproarSoundproof },
    { .name = "uproar_flinch_ends",
      .player = { MON(SPECIES_EXPLOUD, 50, MOVE_UPROAR, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { M1(SPECIES_PERSIAN, 50, MOVE_BITE) },
      .actions = { T(0, 0), T(0, 0), T(1, 0) }, .turns = 3, .wantSeed = WantUproarThenFlinch, .check = CheckUproarFlinch },
    // --- Focus Punch ---
    { .name = "focus_punch_status_move_keeps_focus",
      .player = { M1(SPECIES_MACHAMP, 50, MOVE_FOCUS_PUNCH) },
      .enemy = { M1(SPECIES_SNORLAX, 50, MOVE_GROWL) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckFocusPunchHits },
    { .name = "focus_punch_lost_focus_costs_pp",
      .player = { M1(SPECIES_MACHAMP, 50, MOVE_FOCUS_PUNCH) },
      .enemy = { M1(SPECIES_RATTATA, 20, MOVE_TACKLE) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckFocusPunchLost },
    { .name = "focus_punch_substitute_keeps_focus",
      .player = { MON(SPECIES_MACHAMP, 50, MOVE_SUBSTITUTE, MOVE_FOCUS_PUNCH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 20, MOVE_SPLASH, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .check = CheckFocusPunchSubstitute },
    { .name = "focus_punch_asleep_no_setup",
      .player = { { .species = SPECIES_MACHAMP, .level = 50, .moves = { MOVE_FOCUS_PUNCH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .status = STATUS1_SLEEP_TURN(3) } },
      .enemy = { SNORLAX_SPLASH },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckFocusPunchAsleep },
    { .name = "focus_punch_protected",
      .player = { M1(SPECIES_MACHAMP, 50, MOVE_FOCUS_PUNCH) },
      .enemy = { M1(SPECIES_SNORLAX, 50, MOVE_PROTECT) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckFocusPunchProtect },
    // --- Fake Out ---
    { .name = "fake_out_flinch_then_fails",
      .player = { M1(SPECIES_PERSIAN, 50, MOVE_FAKE_OUT) },
      .enemy = { M1(SPECIES_SNORLAX, 50, MOVE_TACKLE) },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .check = CheckFakeOut },
    { .name = "fake_out_after_switch_in",
      .player = { M0(SPECIES_RATTATA, 50), M1(SPECIES_PERSIAN, 50, MOVE_FAKE_OUT) },
      .enemy = { M1(SPECIES_SNORLAX, 50, MOVE_TACKLE) },
      .actions = { T(SC_SWITCH(1), 0), T(0, 0), T(0, 0) }, .turns = 3, .check = CheckFakeOutAfterSwitch },
    { .name = "fake_out_inner_focus",
      .player = { M1(SPECIES_PERSIAN, 50, MOVE_FAKE_OUT) },
      .enemy = { M1(SPECIES_ZUBAT, 50, MOVE_TACKLE) },   // Inner Focus
      .actions = { T(0, 0) }, .turns = 1, .check = CheckFakeOutInnerFocus },
    { .name = "fake_out_substitute_no_flinch",
      .player = { M0(SPECIES_RATTATA, 50), M1(SPECIES_PERSIAN, 50, MOVE_FAKE_OUT) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SUBSTITUTE, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(SC_SWITCH(1), 1), T(0, 1) }, .turns = 3, .wantSeed = NoCrit, .check = CheckFakeOutSubstitute },
    // --- Truant ---
    { .name = "truant_fly",
      .player = { M1(SPECIES_SLAKING, 50, MOVE_FLY) }, .enemy = { STEELIX_SPLASH },
      .actions = { T(0, 0), T(0, 0), T(0, 0) }, .turns = 3, .check = CheckTruantFly },
    { .name = "truant_hyper_beam",
      .player = { M1(SPECIES_SLAKING, 50, MOVE_HYPER_BEAM) }, .enemy = { STEELIX_SPLASH },
      .actions = { T(0, 0), T(0, 0), T(0, 0) }, .turns = 3, .wantSeed = NoMiss, .check = CheckTruantHyperBeam },
    { .name = "truant_thrash",
      .player = { M1(SPECIES_SLAKING, 50, MOVE_THRASH) }, .enemy = { STEELIX_SPLASH },
      .actions = { T(0, 0), T(0, 0), T(0, 0) }, .turns = 3, .check = CheckTruantThrash },
    { .name = "truant_rollout",
      .player = { M1(SPECIES_SLAKING, 50, MOVE_ROLLOUT) }, .enemy = { STEELIX_SPLASH },
      .actions = { T(0, 0), T(0, 0), T(0, 0) }, .turns = 3, .wantSeed = NoMiss, .check = CheckTruantRollout },
    { .name = "truant_focus_punch",
      .player = { M1(SPECIES_SLAKING, 50, MOVE_FOCUS_PUNCH) }, .enemy = { STEELIX_SPLASH },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .check = CheckTruantFocusPunch },
};

SCENARIO_GROUP(moves_twoturn, sScenarios)
