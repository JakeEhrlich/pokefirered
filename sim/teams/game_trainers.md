# game_trainers.jsonl

Every trainer party in FireRed exactly as the game builds it: 742 teams (trainer ids 1..742, all of
`include/constants/opponents.h` except `TRAINER_NONE`), 1754 mons. No trainer was skipped: every id,
including the unused Ruby/Sapphire dummy classes and unused numbered FRLG slots, has a real party.

## Method

`sim/tests/dump_trainer_teams.c` (build: `cd sim && make build/dump_trainer_teams`, run:
`./build/dump_trainer_teams .. > teams/game_trainers.jsonl`). For each id it calls
`Sim_Init(&sim, BATTLE_TYPE_TRAINER, 1); Sim_LoadTrainerParty(&sim, id);`, which runs the engine's own
`CreateNPCTrainerParty`, then reads the resulting `struct Pokemon`s back with `GetMonData`: species, level,
moves (`MOVE_NONE` pads dropped), held item, ability (`GetAbilityBySpecies(species, MON_DATA_ABILITY_NUM)`),
nature (`GetNature`), IVs/EVs (`MON_DATA_HP_IV + k`, game stat order HP/Atk/Def/Spe/SpA/SpD), friendship.
Nothing is approximated: the party is deterministic because the game derives the personality from the
trainer/species name hash (the seed only affects the hidden OT id), so nature and ability slot are the
ones the real game uses.

Trainer metadata comes from `gTrainers[id]` (class, `doubleBattle`, `items`, decoded trainer name) and from
the constants headers parsed at run time (`include/constants/opponents.h` for `TRAINER_*` names,
`include/constants/trainers.h` for class names). `rematch` tags come from the Vs. Seeker table
`sRematches[]` in `src/vs_seeker.c` (every trainer after the first in a row, ignoring `SKIP` and rows that
reuse the same party) plus `ELITE_FOUR_*_2` and `CHAMPION_REMATCH_*`; a Python re-parse of the same table
agrees exactly (130 Vs. Seeker parties + 7 = 137).

## Fields

- `id`: `trainer_<id>_<CONSTANT>` (e.g. `trainer_414_LEADER_BROCK`); `source`: `game_trainer`.
- `tags`: lower-cased trainer class (`leader`, `elite_four`, `champion`, `rival_early`/`rival_late`,
  `cooltrainer`, ...); plus `rival` (rival classes and champion), `rematch`, `doubles`, `unnamed`
  (empty trainer name: RS dummies and unused numbered FRLG slots) and `doubles_flag_1mon` (see oddities).
- `format`: `doubles` when `doubleBattle` is set and the party has 2+ mons (27 teams), else `singles`.
- `level_cap`: highest level in the party. `notes`: `trainer <id> <CONSTANT>; name: <NAME>; class: <CLASS>;
  items: FULL_RESTORE x4` (usable trainer items with counts, or `none`).
- Mons carry `item`, `ability`, `nature`, `ivs`, `evs` (always 0), `happiness`; `ot`/`nickname`/`fateful`
  are left at defaults.

## Validation

`python3 tools/teams.py check teams/game_trainers.jsonl`: **742 teams: 727 ok, 15 encodable, 0 invalid**.
`stats`: team sizes {1: 202, 2: 271, 3: 144, 4: 58, 5: 56, 6: 11}; levels 5..75; 164 distinct species,
273 moves, 8 held items (BLACK_BELT 66, SITRUS_BERRY 10, STARDUST 4, NUGGET 3, PERSIM 2, CHERI 1, CHESTO 1;
1667 mons hold nothing). Top species: EKANS 123 (the 72 RS dummy trainers all use a L5 Ekans), KOFFING 48,
RATICATE 44. Trainer items: 60 trainers have any (Full Restore / Hyper Potion / Super Potion / Full Heal);
the Elite Four and champion get FULL_RESTORE x2..x4.

The 15 `encodable` teams are the game's own data with moves the sim's FRLG learnset rules reject; kept
as-is: 171 SUPER_NERD_AIDAN (Koffing Sludge at L20, learned at 21), 392/401 COOLTRAINER_SAMUEL/WARREN
(Rhyhorn Take Down), 393/399 COOLTRAINER_GEORGE/BERKE (Cloyster Spike Cannon), 395 COOLTRAINER_PAUL
(Shellder Clamp), 398 COOLTRAINER_OWEN (Nidorino Scratch/Bite, Nidorina Horn Attack/Leer), 419
LEADER_BLAINE (Ponyta/Rapidash Bounce), 420 LEADER_SABRINA (Mr. Mime Baton Pass at L37), 599
COOLTRAINER_LEROY (Kangaskhan Reversal), and the five Painter Smeargles 526/562/563/604/703 (Sketched moves).

## Oddities

- Six unused RS dummy trainers (6 INTERVIEWER, 29 LEADER_TATE_LIZA, 32 SR_AND_JR, 53 RS_TWINS, 76
  OLD_COUPLE, 77 RS_SIS_AND_BRO) have `doubleBattle` set with a one-mon party; the format forbids a
  1-mon doubles team, so they are emitted as `singles` with tag `doubles_flag_1mon` and a note.
- 99 teams are `unnamed`: 72 RS dummy classes (ids 1..85) and 27 unused FRLG slots (BUG_CATCHER_2..8,
  SUPER_NERD_1..3, BURGLAR_1..4, CHANNELER_1..8, BIKER_1/2, LASS_2, CAMPER_2, GAMER_1).
- IVs are the game's flat per-trainer value (`iv * 31 / 255`): 1045 mons at 0, only the first champion
  fight and the Elite Four / champion rematches at 31. EVs are always 0. Friendship is the species' base
  value (70, 140 for the Clefairy/Chansey lines, 35 for Dratini/Houndour lines, Misdreavus, Tyranitar).
- All 66 BLACK_BELT holders are genuine (Black Belt, Crush Kin and Crush Girl parties in
  `src/data/trainer_parties.h`).
- Found while validating: `tools/teams.py` excluded any define containing `_COUNT`, which dropped
  `MOVE_COUNTER` and made 13 trainer teams falsely invalid; it now excludes only names ending in `_COUNT`.
