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
make test           # mechanics checks, ~2200 scenarios, 2000 fuzz battles, every trainer vs. the vanilla AI,
                    # 3000 robustness battles
make backtest       # replays the recorded ROM transcripts in corpus/ (3581 battles, ~5 s, no mGBA needed)
make robust         # robustness fuzz under ASan + UBSan: 20000 garbage battles in build_asan/ (~7 s once built)
build/fuzz 20000 1  # random full-pool battles (doubles included); ~5000 battles/s on an M-series Mac
```

The simulator has also been checked against the real game running in mGBA: with a harness build of the ROM
(`make firered_harness` at the repo root) both sides play the same battles with the same RNG and every
decision point compares the full battle state; see `harness/README.md`. Every scenario group and 1400 random
trainer battles match state for state; the transcripts are kept in `corpus/` so the check reruns offline.

### Robustness contract

Nothing a caller passes can crash or hang the simulator (`tests/robust.c` throws random encodable and
deliberately invalid parties, junk flags, garbage answers and garbage policies at it under the sanitizers):

- `Sim_MakeMonEx` returns -1 and zeroes the mon for ids the engine cannot index (species, move, item, level,
  nature). `Sim_Start` returns -1 (no usable mon, or a double battle with one), -2 (unencodable data, a
  corrupted mon, or a gap in the party: parties are contiguous from slot 0 like in the game), -3 (unsupported
  battle type bits).
- `Sim_Answer` returns -1 when no request is pending for that battler, or, with `sim->strictAnswers`, when the
  action is not one of `Sim_LegalActions` / `Sim_LegalSwitches` (or a usable item); the request stays pending.
  Without `strictAnswers` an illegal choice reaches the engine, which re-prompts exactly like the game
  (no PP, Taunt, Disable, ...); garbage fields are masked or fall back to the first legal action.
- Policy callbacks that answer illegally get the first legal action instead (`sim->rejectedAnswers` counts).
- Engine invariant traps and the step budget end the battle with `SIM_RUN_ERROR` / `SIM_RUN_STUCK` and set
  `sim->error`; there is no `abort()` left in the library.
- Real out-of-bounds reads of the game (Protect's 5th consecutive use, Low Kick on the unused species slots,
  `gActionsByTurnOrder[4]`, `gStatuses3[4]`) are reproduced explicitly from the ROM's neighbouring data
  rather than through struct layout, so the sanitizer build is clean and the results still match the ROM.

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

## Agents and the arena

`include/sim_agent.h` defines an agent (`decide(agent, sim, turnStart, battler, kind, out)`) and
`src/sim_agents.c` provides the built-in pool, each configurable from a spec string:

| spec | what it does |
|---|---|
| `game[:flags=smart|basic]` | the game's own trainer AI (opponent side only) |
| `random` | uniform over legal actions |
| `movebias[:moves=0.85]` | a legal move with p=0.85, else a legal switch |
| `greedy` | the move with the highest expected damage (`Sim_EstimateDamage`); switches only when forced, to the best type matchup |
| `epsgreedy[:eps=0.1]` | greedy, but a random legal action with probability eps |
| `expect` / `epsexpect` | one-turn lookahead: simulates every (my action, their action) pair from the turn-start state, values the result, maximises the mean over a uniform opponent |
| `rm[:iters=10,floor=P]` | vanilla regret matching on that payoff matrix, samples the average strategy |
| `rmplus[:iters=N,floor=P,alt=1,linavg=1]` | RM+ with alternating updates and linear averaging |

Common keys: `vf=basic|material` (value function; any agent that uses one takes it as configuration),
`samples=N` (RNG samples per simulated pair), `eps`, `floor` (minimum sampling probability), `seed`.
Value agents receive the state at the first decision of the turn, before either side committed, so the second
mover never sees the first mover's choice. `Sim_ValueBasic` scores HP fractions, status, stat stages, screens
and spikes as a zero-sum number in [-1, 1].

`build/arena` plays tournaments (threads, per-game seeds, alternating sides) on a team pool and rates the
players with ELO:

```
python3 tools/teams.py tsv teams/showdown_randbats.jsonl > /tmp/randbats.tsv
build/arena --teams /tmp/randbats.tsv --pool --games 4000 --threads 8 --ratings ratings.json --out games.jsonl
build/arena --teams /tmp/randbats.tsv --rate-teams --agent rmplus:iters=30 --games 20000 --ratings team_elo.json
```
The second form rates the teams themselves: one fixed agent plays both sides. ELO is the primary evaluation;
`--out` keeps every game (agents, teams, sides, result, seed). The arena prints sequential ELO; for the real
numbers fit order-independent Bradley-Terry ratings with standard errors from the log:
```
python3 tools/elo_fit.py games.jsonl --h2h --json ratings.json          # agents
python3 tools/elo_fit.py team_games.jsonl --players teams --min-games 20  # teams
```
Results so far live in `ratings/` (randbats pool: unfloored regret matching 1157-1170 +-7, expectimax 1088,
greedy 1029, the game's own AI 825, random 352).

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
