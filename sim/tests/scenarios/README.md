# Writing battle scenarios

A scenario is a fixed battle (both parties, every action of every battler, an RNG seed) plus checks on the
final simulator state. Scenarios run two ways:

- `make scenarios && build/scenarios [filter]` runs them in the simulator and evaluates the checks.
- `build/crosscheck --scenarios [filter]` plays each one through the real game in mGBA and compares the
  complete battle state at every decision point (needs the harness ROM; see `sim/harness/README.md`).

The simulator *is* the game's engine code (`sim/src/*.c` are copies of `src/battle_*.c`, `pokemon.c`, and
the battle scripts in `data/battle_scripts_1.s`), so a failing check almost always means the expectation is
wrong, not the engine. Read the code before deciding.

## Format

One file per group in `tests/scenarios/<group>.c`:

```c
#include "scenario.h"

static void CheckLeechSeed(struct BattleSim *sim)
{
    CHECK(STATUS3(1) & STATUS3_LEECHSEED, "seeded");
    CHECK(HP(1) == MAXHP(1) - MAXHP(1) / 8, "drained 1/8 (hp %d/%d)", HP(1), MAXHP(1));
}
static int WantLeechSeedHit(struct BattleSim *sim) { return (STATUS3(1) & STATUS3_LEECHSEED) != 0; }

static const struct Scenario sScenarios[] =
{
    { .name = "leech_seed_drain",
      .player = { MON(SPECIES_BULBASAUR, 50, MOVE_LEECH_SEED, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy  = { MON(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) },          // turn 0: player uses move slot 0, enemy uses move slot 0
      .turns = 1,                      // stop after 1 turn (0 = play until the battle ends)
      .wantSeed = WantLeechSeedHit,    // optional: search seeds until this predicate holds (chance outcomes)
      .check = CheckLeechSeed },
};

SCENARIO_GROUP(my_group, sScenarios)   // group name must be unique across files
```

- Battlers: 0 = player's mon, 1 = enemy's mon (doubles: 2 = player's second, 3 = enemy's second; set
  `.flags = BATTLE_TYPE_DOUBLE`, give both sides >= 2 mons, use `T4(a, b, c, d)` per turn, and `.targets`
  with battler id + 1 to pick a target).
- Actions: `SC_MOVE(n)`/`n` = move slot, `SC_SWITCH(n)` = switch to party slot n, `SC_DEFAULT` = slot 0.
  Faint replacements use the same table for that turn (`SC_SWITCH(n)`, else the first legal mon).
  If the game refuses the scripted choice (disabled/taunted/no PP/trapped), the runner falls back to the
  first legal action for that battler (so "the move could not be selected" shows up as another move used).
- `struct ScenarioMon` fields: `.species .level .moves .item .abilityNum (0/1) .nature .ivs/.ivsSet
  .evs .hp/.hpSet .status (STATUS1_*) .pp/.ppSet .otId .fateful`. IVs default to 31, EVs to 0, nature
  Hardy, level 50.
- Mons are the player's own (obedient, player OT) unless `.otId` is set (outsider: obedience rules apply;
  Mew/Deoxys additionally need `.fateful = 1`). The player has all 8 badges (attack/defense/speed/special
  +10% for the player's side, all levels obey) unless the scenario sets `.badgesSet = 1, .badges = mask`.
- Checks: `CHECK(cond, fmt, ...)`; state accessors `HP(b) MAXHP(b) STATUS1(b) STATUS2(b) STATUS3(b)
  STAGE(b, STAT_x) PP(b, slot) B(b).<field> PARTY_HP(side, i) WEATHER() SIDE_STATUS(side) OUTCOME()
  sim->disableStructs[b] sim->sideTimers[side] sim->wishFutureKnock`; message log helpers
  `LOG_HAS(id) LOG_HAS_T(id, turn) LOG_COUNT(id) LOG_HAS_MOVE(id, move) LOG_HAS_MOVE_T(id, move, turn)
  LOG_INDEX_T(id, battler, turn) MOVED_BEFORE(a, b, turn)` with ids from `constants/battle_string_ids.h`
  (turns are 0-based; `SC_LAST_TURN`, `SC_ANY_TURN`).
- Chance outcomes: do not assume a hit/crit/secondary effect; use `.wantSeed` to select a seed where the
  outcome you want to test occurred, or assert both branches.
- Watch the species data: the *first* ability is used unless `.abilityNum = 1` (Snorlax: Immunity/Thick
  Fat, Poliwrath: Water Absorb/Damp, ...), and types give immunities (Poison types can't be poisoned, ...).
- Keep scenarios short (`.turns`), and prefer bulky, slow or fast mons deliberately so the order of
  events is what you intend (check `MOVED_BEFORE`).

Build and run only your group while iterating: `tools/run_group.sh <group> [filter]` (compiles just that
file, so other groups' work-in-progress cannot break your build; `make` silently reuses a stale binary if
any scenario file fails to compile). `SCENARIO_VERBOSE=1` prints each scenario's message log and battlers.
