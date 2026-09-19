# FRLG battle simulator

A standalone, host-buildable copy of pokefirered's battle engine, intended for rollouts, search and
AI experiments (and, eventually, an AI that runs on the GBA itself).

It is **not a re-implementation**: the engine sources (`battle_main.c`, `battle_util.c`,
`battle_script_commands.c`, `pokemon.c`, the controller emitters and the trainer AI) are copied nearly
verbatim, the battle/AI scripts are assembled from `data/battle_scripts_*.s` / `data/battle_ai_scripts.s`
into bytecode that is verified byte-for-byte against the retail ROM, and every data table (species,
moves, items, learnsets, trainers, type chart) is included straight from the repo. Only the UI layer is
replaced.

## Build & test

```
cd sim
make                # builds build/libsim.a and the test programs (needs clang, python3, tools/preproc)
make test           # mechanics scenarios, 2000 fuzz battles, every in-game trainer vs. the vanilla AI
build/fuzz 20000 1  # random full-pool battles (doubles included); ~5000 battles/s on an M-series Mac
```

`make OPT="-O1 -g -fsanitize=address"` builds with ASan (the fuzz suite is clean under it).

The simulator has also been checked against the real game running in mGBA: with a harness build of the ROM
(`make firered_harness` at the repo root) both sides play the same battles with the same RNG and every
decision point compares the full battle state; see `harness/README.md`. Dozens of random trainer battles
match turn for turn.

Regenerating the assembled scripts with ROM verification:

```
BSASM_VERIFY_ROM=../pokefirered.gba BSASM_VERIFY_MAP=../pokefirered.map \
python3 tools/bsasm.py gen/battle_scripts.c include/battle_scripts.h \
    ../data/battle_scripts_1.s ../data/battle_scripts_2.s ../data/battle_ai_scripts.s
```

## Using it

```c
#include "sim.h"

static struct BattleSim sim;                       // ~8 KB, plain data: memcpy it for rollouts
Sim_Init(&sim, BATTLE_TYPE_TRAINER, seed);         // flags: BATTLE_TYPE_TRAINER, | BATTLE_TYPE_DOUBLE
Sim_MakeMon(&sim.playerParty[0], SPECIES_SNORLAX, 50, NATURE_ADAMANT, ivs, evs, moves, ITEM_LEFTOVERS, 0);
Sim_LoadTrainerParty(&sim, TRAINER_LEADER_BROCK);  // or fill sim.enemyParty yourself
Sim_SetPolicy(&sim, B_SIDE_OPPONENT, Sim_VanillaAIPolicy); // the game's own trainer AI (opponent side)
Sim_Start(&sim);

for (;;) {
    int r = Sim_Run(&sim);                         // runs until someone must decide, or the battle ends
    if (r != SIM_RUN_REQUEST) break;               // gBattleOutcome / sim.battleOutcome tells who won
    struct SimAction acts[32];
    int n = Sim_LegalActions(&sim, sim.requestBattler, acts, 32);   // moves (+targets in doubles), switches
    // sim.requestKind: SIM_REQ_ACTION (turn start) or SIM_REQ_SWITCH (faint replacement etc.)
    Sim_Answer(&sim, sim.requestBattler, &acts[pick]);
}
```

A side can instead be driven by a callback (`Sim_SetPolicy`), e.g. `Sim_RandomPolicy` for rollouts.
The full engine state is visible in the struct (`sim.battleMons[]`, `statuses3`, `sideStatuses`,
`disableStructs`, parties, RNG, ...), so evaluation functions can read anything the game knows.

Other helpers: `Sim_CanLearnMove` / `Sim_LearnableMoves` (level-up, TM/HM, tutor, egg moves, pre-evolutions),
`Sim_PrintBattlers`, `Sim_PrintLog` (message log by battle string id), name tables in `sim_names.h`.

## What is and isn't simulated

Simulated exactly as in the game: damage, accuracy, criticals, all 354 moves and move effects, abilities,
held items (incl. berries), status, weather, doubles targeting and spread moves, switching, trapping,
Struggle, trainer items via the AI, obedience (OT id + badges), badge stat boosts (`sim.badgeFlags`).

Deliberately not simulated: experience/level-ups (`getexp` runs but grants nothing), link battles,
Safari/tutorial/Pokedude battles, the "SHIFT" battle-style switch prompt (the sim uses SET), catching.
The player's bag is not modelled: a `B_ACTION_USE_ITEM` on the player side applies the item to
`partySlot` immediately at selection time, which is what the party menu does in the game.
The vanilla AI (`Sim_VanillaAIPolicy`) is only valid on the opponent side, like in the game.

Turn cap: `sim.maxTurns` (default 500) declares a draw.

RNG: by default the sim uses the game's LCG; set `sim.rngXorshift = 1` (after `Sim_Init`) to use xorshift32
instead, which is better for rollouts. Nothing in the engine depends on which one is used.

## Layout

- `include/sim.h` — state struct + API. `include/sim_globals.h` remaps the game's globals into it.
- `src/` — copied engine files (see `git diff --no-index ../src/<file> src/<file>` for the edits, all marked
  "simulator"), plus `sim_controller.c` (synchronous controller), `sim_api.c`, `sim_legal.c`, `stubs.c`.
- `tools/bsasm.py` (script assembler + ROM verifier), `gen_state.py` (state struct/macros), `gen_items.py`,
  `gen_names.py`, `extract_tables.py`, `gen_tmhm.py` — all outputs land in `gen/`.
- `tests/` — `mechanics.c` (scenario checks), `fuzz.c`, `trainer.c`, `alltrainers.c`, `simtest.c`.

## Notes for the GBA target

Everything in `src/` is C99 with no heap or float use and no libc dependency in the engine itself; the
state struct is 8 KB. Script pointers inside the blob are encoded as `0x08000000 | offset` and C-symbol
references as `0x02000000 | id << 12 | offset` (see `SimDecodePtr`), so the blob is position independent.
