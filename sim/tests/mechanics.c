// Scenario tests for individual battle mechanics, driven through the suspend/answer API.
#include <stdio.h>
#include <string.h>
#include "global.h"
#include "battle.h"
#include "pokemon.h"
#include "sim.h"
#include "sim_names.h"
#include "constants/species.h"
#include "constants/moves.h"
#include "constants/items.h"
#include "constants/opponents.h"
#include "constants/battle_string_ids.h"
#include "constants/pokemon.h"
#include "constants/abilities.h"
#include "constants/battle_string_ids.h"

static int sFailures, sChecks;
#define CHECK(cond, ...) do { sChecks++; if (!(cond)) { sFailures++; printf("  FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static struct BattleSim sim;

static void Mon(struct Pokemon *mon, u16 species, u8 level, u16 m1, u16 m2, u16 m3, u16 m4, u16 item)
{
    u16 moves[4] = { m1, m2, m3, m4 };
    Sim_MakeMon(mon, species, level, NATURE_HARDY, NULL, NULL, moves, item, 0);
}

static void Setup(u32 flags, u16 seed)
{
    Sim_Init(&sim, flags, seed);
    sim.logEnabled = TRUE;
}

// Answers the pending request with the given move slots (slot >= 10 means switch to party slot-10).
static void AnswerPending(int slotA, int slotB)
{
    struct SimAction a = {0};
    int slot = GetBattlerSide(sim.requestBattler) == B_SIDE_PLAYER ? slotA : slotB;
    if (sim.requestKind == SIM_REQ_SWITCH)
    {
        u8 slots[6];
        int n = Sim_LegalSwitches(&sim, sim.requestBattler, slots, 6);
        a.type = B_ACTION_SWITCH;
        a.partySlot = n ? slots[0] : 0;
    }
    else if (slot >= 10)
    {
        a.type = B_ACTION_SWITCH;
        a.partySlot = slot - 10;
    }
    else
    {
        a.type = B_ACTION_USE_MOVE;
        a.moveSlot = slot;
        a.target = 0xFF;
    }
    Sim_Answer(&sim, sim.requestBattler, &a);
}

static int LogHas(u16 stringId)
{
    int i;
    for (i = 0; i < sim.logCount; i++)
        if (sim.log[i].stringId == stringId)
            return 1;
    return 0;
}

static int LogIndex(u16 stringId, u8 battler, int nth)
{
    int i;
    for (i = 0; i < sim.logCount; i++)
        if (sim.log[i].stringId == stringId && sim.log[i].battler == battler && nth-- == 0)
            return i;
    return -1;
}

static void Start(void)
{
    CHECK(Sim_Start(&sim) == 0, "start");
    CHECK(Sim_Run(&sim) == SIM_RUN_REQUEST, "initial request");
    sim.logCount = 0;
}

// Plays exactly one full turn (a request must be pending on entry) and stops at the next decision point.
static void Turn(int slotA, int slotB)
{
    int startTurn = sim.turnCount;
    int res = SIM_RUN_REQUEST;
    sim.logCount = 0;
    while (res == SIM_RUN_REQUEST && sim.turnCount == startTurn && !sim.finished)
    {
        AnswerPending(slotA, slotB);
        res = Sim_Run(&sim);
    }
}

static void TestLeechSeedAndDrain(void)
{
    int seed, hits = 0;
    for (seed = 1; seed <= 20 && hits < 3; seed++)
    {
        Setup(BATTLE_TYPE_TRAINER, seed);
        Mon(&sim.playerParty[0], SPECIES_BULBASAUR, 30, MOVE_LEECH_SEED, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_NONE);
        Mon(&sim.enemyParty[0], SPECIES_RATTATA, 30, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_NONE);
        Start();
        Turn(0, 0);
        if (LogHas(STRINGID_PKMNSEEDED))
        {
            u16 max = sim.battleMons[1].maxHP;
            hits++;
            CHECK(sim.battleMons[1].hp == max - max / 8, "leech seed drained %d of %d", max - sim.battleMons[1].hp, max);
            CHECK(sim.battleMons[0].hp == sim.battleMons[0].maxHP, "seeder stays at full HP");
            CHECK(sim.statuses3[1] & STATUS3_LEECHSEED, "status3 leech seed set");
        }
    }
    CHECK(hits > 0, "leech seed never hit in 20 seeds");
}

static void TestParalysisSpeedAndThunderWave(void)
{
    Setup(BATTLE_TYPE_TRAINER, 3);
    Mon(&sim.playerParty[0], SPECIES_BULBASAUR, 30, MOVE_THUNDER_WAVE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_NONE);
    Mon(&sim.enemyParty[0], SPECIES_RATTATA, 30, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_NONE);
    Start();
    CHECK(sim.battleMons[1].speed > sim.battleMons[0].speed, "rattata faster");
    Turn(0, 0);
    CHECK(LogIndex(STRINGID_USEDMOVE, 1, 0) < LogIndex(STRINGID_USEDMOVE, 0, 0), "rattata moved first on turn 1");
    CHECK(sim.battleMons[1].status1 & STATUS1_PARALYSIS, "rattata paralyzed");
    Turn(1, 0);
    CHECK(LogIndex(STRINGID_USEDMOVE, 0, 0) >= 0, "bulbasaur moved");
    CHECK(LogIndex(STRINGID_USEDMOVE, 1, 0) < 0 || LogIndex(STRINGID_USEDMOVE, 0, 0) < LogIndex(STRINGID_USEDMOVE, 1, 0),
          "paralyzed rattata (speed/4) moves after bulbasaur");
}

static void TestProtect(void)
{
    Setup(BATTLE_TYPE_TRAINER, 4);
    Mon(&sim.playerParty[0], SPECIES_BULBASAUR, 30, MOVE_PROTECT, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_NONE);
    Mon(&sim.enemyParty[0], SPECIES_RATTATA, 30, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_NONE);
    Start();
    Turn(0, 0);
    CHECK(LogHas(STRINGID_PKMNPROTECTEDITSELF2) || LogHas(STRINGID_PKMNPROTECTEDITSELF), "protect message");
    CHECK(sim.battleMons[0].hp == sim.battleMons[0].maxHP, "no damage through protect");
}

static void TestSubstitute(void)
{
    Setup(BATTLE_TYPE_TRAINER, 5);
    Mon(&sim.playerParty[0], SPECIES_BULBASAUR, 30, MOVE_SUBSTITUTE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_NONE);
    Mon(&sim.enemyParty[0], SPECIES_RATTATA, 30, MOVE_SPLASH, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, ITEM_NONE);
    Start();
    Turn(0, 0);
    {
        u16 max = sim.battleMons[0].maxHP;
        CHECK(sim.battleMons[0].hp == max - max / 4, "substitute costs 1/4 (hp %d/%d)", sim.battleMons[0].hp, max);
        CHECK(sim.battleMons[0].status2 & STATUS2_SUBSTITUTE, "substitute status");
        CHECK(sim.disableStructs[0].substituteHP == max / 4, "substitute hp");
    }
    Turn(1, 1);
    {
        u16 max = sim.battleMons[0].maxHP;
        CHECK(sim.battleMons[0].hp == max - max / 4, "tackle absorbed by substitute");
    }
}

static void TestLeftoversAndSeismicToss(void)
{
    Setup(BATTLE_TYPE_TRAINER, 6);
    Mon(&sim.playerParty[0], SPECIES_SNORLAX, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_LEFTOVERS);
    Mon(&sim.enemyParty[0], SPECIES_MACHOP, 50, MOVE_SEISMIC_TOSS, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_NONE);
    Start();
    Turn(0, 0);
    {
        u16 max = sim.battleMons[0].maxHP;
        CHECK(sim.battleMons[0].hp == max - 50 + max / 16, "seismic toss 50 then leftovers %d: hp %d/%d", max / 16, sim.battleMons[0].hp, max);
    }
}

static void TestSandstormChip(void)
{
    Setup(BATTLE_TYPE_TRAINER, 7);
    Mon(&sim.playerParty[0], SPECIES_GEODUDE, 30, MOVE_SANDSTORM, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_NONE);
    Mon(&sim.enemyParty[0], SPECIES_RATTATA, 30, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_NONE);
    Start();
    Turn(0, 0);
    CHECK(sim.battleWeather & B_WEATHER_SANDSTORM, "sandstorm active");
    CHECK(sim.battleMons[0].hp == sim.battleMons[0].maxHP, "rock type immune to sandstorm");
    CHECK(sim.battleMons[1].hp == sim.battleMons[1].maxHP - sim.battleMons[1].maxHP / 16, "rattata takes 1/16");
    Turn(1, 0); Turn(1, 0); Turn(1, 0); Turn(1, 0);
    CHECK(!(sim.battleWeather & B_WEATHER_SANDSTORM), "sandstorm ends after 5 turns");
}

static void TestIntimidate(void)
{
    Setup(BATTLE_TYPE_TRAINER, 8);
    Mon(&sim.playerParty[0], SPECIES_RATTATA, 30, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_NONE);
    Mon(&sim.playerParty[1], SPECIES_GYARADOS, 30, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_NONE);
    Mon(&sim.enemyParty[0], SPECIES_RATTATA, 30, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_NONE);
    Start();
    CHECK(sim.battleMons[1].statStages[STAT_ATK] == 6, "attack stage neutral");
    Turn(11, 0);
    CHECK(sim.battleMons[0].species == SPECIES_GYARADOS, "switched to gyarados");
    CHECK(sim.battleMons[0].ability == ABILITY_INTIMIDATE, "gyarados has intimidate");
    CHECK(sim.battleMons[1].statStages[STAT_ATK] == 5, "intimidate lowered attack (stage %d)", sim.battleMons[1].statStages[STAT_ATK]);
}

static void TestTypeImmunityAndPriority(void)
{
    Setup(BATTLE_TYPE_TRAINER, 9);
    Mon(&sim.playerParty[0], SPECIES_SANDSHREW, 30, MOVE_EARTHQUAKE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_NONE);
    Mon(&sim.enemyParty[0], SPECIES_PIDGEY, 30, MOVE_QUICK_ATTACK, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_NONE);
    Start();
    Turn(0, 0);
    CHECK(sim.battleMons[1].hp == sim.battleMons[1].maxHP, "earthquake does not affect flying");
    CHECK(LogHas(STRINGID_ITDOESNTAFFECT), "doesn't affect message");
    CHECK(sim.battleMons[0].hp < sim.battleMons[0].maxHP, "quick attack landed");
    CHECK(LogIndex(STRINGID_USEDMOVE, 1, 0) < LogIndex(STRINGID_USEDMOVE, 0, 0), "quick attack (+1) goes first despite pidgey speed? (pidgey %d vs sandshrew %d)", sim.battleMons[1].speed, sim.battleMons[0].speed);
}

static void TestStruggle(void)
{
    u8 zero = 0;
    int i;
    Setup(BATTLE_TYPE_TRAINER, 10);
    Mon(&sim.playerParty[0], SPECIES_RATTATA, 30, MOVE_TACKLE, MOVE_NONE, MOVE_NONE, MOVE_NONE, ITEM_NONE);
    Mon(&sim.enemyParty[0], SPECIES_GEODUDE, 30, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_NONE);
    for (i = 0; i < 4; i++)
        SetMonData(&sim.playerParty[0], MON_DATA_PP1 + i, &zero);
    Start();
    {
        struct SimAction acts[32];
        int n = Sim_LegalActions(&sim, 0, acts, 32);
        CHECK(n == 1 && acts[0].type == B_ACTION_USE_MOVE, "only struggle legal (%d actions)", n);
    }
    Turn(0, 0);
    CHECK(sim.battleMons[0].hp < sim.battleMons[0].maxHP, "struggle recoil");
    CHECK(sim.battleMons[1].hp < sim.battleMons[1].maxHP, "struggle hit");
}

static void TestDoublesEarthquake(void)
{
    Setup(BATTLE_TYPE_TRAINER | BATTLE_TYPE_DOUBLE, 12);
    Mon(&sim.playerParty[0], SPECIES_DUGTRIO, 40, MOVE_EARTHQUAKE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_NONE);
    Mon(&sim.playerParty[1], SPECIES_RATTATA, 40, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_NONE);
    Mon(&sim.enemyParty[0], SPECIES_SNORLAX, 40, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_NONE);
    Mon(&sim.enemyParty[1], SPECIES_SNORLAX, 40, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_NONE);
    Start();
    CHECK(sim.battlersCount == 4, "four battlers");
    Turn(0, 0);
    CHECK(sim.battleMons[1].hp < sim.battleMons[1].maxHP, "eq hit foe 1");
    CHECK(sim.battleMons[3].hp < sim.battleMons[3].maxHP, "eq hit foe 2");
    CHECK(sim.battleMons[2].hp < sim.battleMons[2].maxHP, "eq hit partner");
    CHECK(sim.battleMons[0].hp == sim.battleMons[0].maxHP, "user untouched");
}

static void TestDamageFormulaRange(void)
{
    // Rattata (Tackle, STAB) vs Bulbasaur: damage must lie in [85%,100%] of the Gen 3 base damage, over many seeds.
    int seed, minD = 9999, maxD = 0;
    int expectedMax = 0;
    for (seed = 1; seed <= 40; seed++)
    {
        int dmg;
        Setup(BATTLE_TYPE_TRAINER, 100 + seed);
        Mon(&sim.playerParty[0], SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_NONE);
        Mon(&sim.enemyParty[0], SPECIES_BULBASAUR, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_NONE);
        sim.badgeFlags = 0;
        Start();
        if (!expectedMax)
        {
            int atk = sim.battleMons[0].attack, def = sim.battleMons[1].defense;
            int base = ((2 * 50 / 5 + 2) * 35 * atk / def) / 50 + 2; // Tackle power 35 in FRLG
            expectedMax = base * 15 / 10; // STAB
        }
        Turn(0, 0);
        if (LogHas(STRINGID_CRITICALHIT) || LogHas(STRINGID_ATTACKMISSED))
            continue;
        dmg = sim.battleMons[1].maxHP - sim.battleMons[1].hp;
        if (dmg < minD) minD = dmg;
        if (dmg > maxD) maxD = dmg;
    }
    CHECK(maxD <= expectedMax && maxD >= expectedMax - 1, "max damage %d vs formula %d", maxD, expectedMax);
    CHECK(minD >= expectedMax * 85 / 100 - 1, "min damage %d vs 85%% of %d", minD, expectedMax);
}

static void TestChoiceBandLock(void)
{
    Setup(BATTLE_TYPE_TRAINER, 13);
    Mon(&sim.playerParty[0], SPECIES_RATTATA, 30, MOVE_TACKLE, MOVE_QUICK_ATTACK, MOVE_SPLASH, MOVE_SPLASH, ITEM_CHOICE_BAND);
    Mon(&sim.enemyParty[0], SPECIES_SNORLAX, 30, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_NONE);
    Start();
    Turn(1, 0);
    {
        struct SimAction acts[32];
        int n = Sim_LegalActions(&sim, 0, acts, 32);
        CHECK(n == 1 && acts[0].moveSlot == 1, "choice band locks into slot 1 (%d legal)", n);
    }
}

u16 GetPokedexHeightWeight(u16 dexNum, u8 data);
u16 SpeciesToNationalPokedexNum(u16 species);

static void TestLowKickWeight(void)
{
    CHECK(GetPokedexHeightWeight(SpeciesToNationalPokedexNum(SPECIES_HITMONLEE), 1) == 498, "hitmonlee weighs 49.8 kg");
    CHECK(GetPokedexHeightWeight(SpeciesToNationalPokedexNum(SPECIES_SNORLAX), 1) == 4600, "snorlax weighs 460 kg");
    CHECK(GetPokedexHeightWeight(SpeciesToNationalPokedexNum(SPECIES_PIDGEY), 1) == 18, "pidgey weighs 1.8 kg");
}

// The trainer AI's item use goes through the opponent controller's CONTROLLER_OPENBAG handler, which
// must return the item ShouldUseItem() picked (OpponentHandleChooseItem) rather than asking a policy.
// Regression: the sim used to burn a Random() call here and use no item at all.
static void TestTrainerAIUsesItem(void)
{
    u16 hpAfterHit, hpAfterHeal;
    Setup(BATTLE_TYPE_TRAINER, 7);
    Mon(&sim.playerParty[0], SPECIES_CHANSEY, 60, MOVE_SEISMIC_TOSS, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_NONE);
    Sim_LoadTrainerParty(&sim, TRAINER_COOLTRAINER_SAMUEL); // SANDSLASH L37 first, carries one SUPER POTION
    Sim_SetPolicy(&sim, B_SIDE_OPPONENT, Sim_VanillaAIPolicy);
    Start();
    Turn(0, 0); // Seismic Toss: 60 damage, more than a Super Potion heals -> AI wants the item next turn
    hpAfterHit = sim.battleMons[1].hp;
    CHECK(hpAfterHit > 0 && hpAfterHit < sim.battleMons[1].maxHP, "sandslash damaged to %d/%d", hpAfterHit, sim.battleMons[1].maxHP);
    Turn(1, 0); // player splashes; the AI should use its Super Potion (+50 HP)
    hpAfterHeal = sim.battleMons[1].hp;
    CHECK(LogHas(STRINGID_TRAINER1USEDITEM), "trainer used an item");
    CHECK(hpAfterHeal == hpAfterHit + 50 || hpAfterHeal == sim.battleMons[1].maxHP, "super potion healed 50 (%d -> %d)", hpAfterHit, hpAfterHeal);
    CHECK(sim.sBattleStructStorage.chosenItem[0] == ITEM_SUPER_POTION, "chosenItem recorded (%d)", sim.sBattleStructStorage.chosenItem[0]);
    CHECK(sim.sBattleResourcesStorage.battleHistory.trainerItems[0] == ITEM_NONE, "item consumed from the trainer's list");
}

static void TestLegality(void)
{
    CHECK(Sim_CanLearnMove(SPECIES_VENUSAUR, MOVE_LEECH_SEED, 100), "venusaur leech seed via pre-evo level-up");
    CHECK(Sim_CanLearnMove(SPECIES_BULBASAUR, MOVE_SYNTHESIS, 39), "bulbasaur synthesis at 39");
    CHECK(!Sim_CanLearnMove(SPECIES_BULBASAUR, MOVE_SYNTHESIS, 38), "bulbasaur no synthesis at 38");
    CHECK(Sim_CanLearnMove(SPECIES_BULBASAUR, MOVE_SOLAR_BEAM, 5), "bulbasaur solar beam via TM22");
    CHECK(Sim_CanLearnMove(SPECIES_CHARIZARD, MOVE_FLY, 36), "charizard fly (HM)");
    CHECK(Sim_CanLearnMove(SPECIES_BULBASAUR, MOVE_LIGHT_SCREEN, 5), "bulbasaur light screen (egg move)");
    CHECK(Sim_CanLearnMove(SPECIES_SNORLAX, MOVE_SEISMIC_TOSS, 5), "snorlax seismic toss (tutor)");
    CHECK(!Sim_CanLearnMove(SPECIES_MAGIKARP, MOVE_SURF, 100), "magikarp cannot learn surf");
}

int main(void)
{
    printf("  %s\n", "TestLeechSeedAndDrain"); fflush(stdout); TestLeechSeedAndDrain();
    printf("  %s\n", "TestParalysisSpeedAndThunderWave"); fflush(stdout); TestParalysisSpeedAndThunderWave();
    printf("  %s\n", "TestProtect"); fflush(stdout); TestProtect();
    printf("  %s\n", "TestSubstitute"); fflush(stdout); TestSubstitute();
    printf("  %s\n", "TestLeftoversAndSeismicToss"); fflush(stdout); TestLeftoversAndSeismicToss();
    printf("  %s\n", "TestSandstormChip"); fflush(stdout); TestSandstormChip();
    printf("  %s\n", "TestIntimidate"); fflush(stdout); TestIntimidate();
    printf("  %s\n", "TestTypeImmunityAndPriority"); fflush(stdout); TestTypeImmunityAndPriority();
    printf("  %s\n", "TestStruggle"); fflush(stdout); TestStruggle();
    printf("  %s\n", "TestDoublesEarthquake"); fflush(stdout); TestDoublesEarthquake();
    printf("  %s\n", "TestDamageFormulaRange"); fflush(stdout); TestDamageFormulaRange();
    printf("  %s\n", "TestChoiceBandLock"); fflush(stdout); TestChoiceBandLock();
    printf("  %s\n", "TestLowKickWeight"); fflush(stdout); TestLowKickWeight();
    printf("  %s\n", "TestTrainerAIUsesItem"); fflush(stdout); TestTrainerAIUsesItem();
    printf("  %s\n", "TestLegality"); fflush(stdout); TestLegality();
    printf("mechanics: %d checks, %d failures\n", sChecks, sFailures);
    return sFailures != 0;
}
