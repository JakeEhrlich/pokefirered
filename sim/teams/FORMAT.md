# Team corpus format

A *dataset* is one `.jsonl` file in `sim/teams/`: one JSON object per line, one **team** per object.
Datasets are independent ("seed" corpora); the training mix is decided later. Every dataset ships with a
`README` section in `sim/teams/README.md` (or a `<name>.md` next to it) saying where the teams came from and
how they were generated, so they can be regenerated or perturbed later.

## Team object

```json
{"id": "adv_ou_sample_01",
 "source": "smogon",
 "tags": ["adv", "ou", "sample-team"],
 "format": "singles",
 "level_cap": 100,
 "notes": "free text, optional",
 "mons": [ {mon}, {mon}, ... ]}
```

| field | required | meaning |
|---|---|---|
| `id` | yes | unique within the dataset (`^[A-Za-z0-9_.:-]+$`) |
| `source` | yes | short provenance tag: `game_trainer`, `smogon`, `showdown_randbats`, `random_valid`, `random_encodable`, `playthrough`, `strategy`, `agent` ... |
| `tags` | no | list of strings, free-form |
| `format` | no | `singles` (default) or `doubles` (a doubles team needs at least 2 mons) |
| `level_cap` | no | the level the team is meant to be played at; informational |
| `notes` | no | free text |
| `mons` | yes | 1 to 6 mon objects, in party order |

## Mon object

```json
{"species": "SNORLAX", "level": 100, "moves": ["BODY_SLAM", "CURSE", "REST", "SHADOW_BALL"],
 "item": "LEFTOVERS", "ability": "THICK_FAT", "nature": "CAREFUL",
 "ivs": [31, 31, 31, 31, 31, 31], "evs": [188, 0, 68, 0, 0, 252],
 "happiness": 255, "nickname": null, "ot": "player", "fateful": false}
```

| field | required | meaning |
|---|---|---|
| `species` | yes | `SPECIES_` constant without the prefix (`BULBASAUR` ... `DEOXYS`, `UNOWN`) |
| `level` | yes | 1..100 |
| `moves` | yes | 1..4 `MOVE_` constants without the prefix; no duplicates; `NONE` pads are dropped |
| `item` | no | `ITEM_` constant without the prefix; default `NONE` |
| `ability` | no | `ABILITY_` constant name, or the slot number 0/1; default 0 (first ability). A named ability must be one of the species' two abilities |
| `nature` | no | `NATURE_` constant name; default `HARDY` |
| `ivs` | no | 6 ints 0..31 in game stat order: HP, Atk, Def, Spe, SpA, SpD; default all 31 |
| `evs` | no | 6 ints 0..255 in the same order, sum <= 510; default all 0 |
| `happiness` | no | 0..255; default 70 (matters for Return/Frustration) |
| `nickname` | no | up to 10 chars, ASCII letters/digits/space; default the species name |
| `ot` | no | `player` (default: obeys) or `outsider` (subject to the badge obedience rules) |
| `fateful` | no | fateful-encounter flag (Mew/Deoxys only obey with it); default false |

Stat order is the game's (`STAT_HP, STAT_ATK, STAT_DEF, STAT_SPEED, STAT_SPATK, STAT_SPDEF`), **not**
Showdown's (which is HP/Atk/Def/SpA/SpD/Spe) - convert when importing.

## Legality levels

`tools/teams.py check <file.jsonl>` resolves the names and runs `build/teamcheck`, which uses the simulator's
own rules. Each team gets one verdict:

- `ok`: every mon is legal in FireRed: species exists, level in range, every move learnable by that species
  at that level (level-up, TM/HM, FRLG tutors, egg moves, pre-evolutions; same rule as `Sim_CanLearnMove`),
  item is a real holdable item, ability is one of the species' abilities, IV/EV bounds hold.
- `encodable`: fits in `struct Pokemon` (species 1..411, moves 1..354, item 0..374, level 1..100, IV/EV
  bounds) but breaks a FireRed rule (illegal move, wrong ability, Emerald-only tutor, ...). The simulator
  can still battle with it.
- `invalid`: cannot be encoded at all (unknown name, bad level, 7 mons, ...). Not accepted in any dataset.

Datasets say in their README which verdicts they intend to contain (e.g. `random_encodable` is expected to be
mostly `encodable`; `game_trainers` must be 100% `ok`).

## Tooling

```
tools/teams.py check   teams/<name>.jsonl        # verdict per team + summary; exit 1 if any invalid
tools/teams.py tsv     teams/<name>.jsonl > x.tsv # the flat form build/teamcheck and the sim loader read
tools/teams.py stats   teams/<name>.jsonl        # counts, species/item/move histograms
build/teamcheck --moves SPECIES LEVEL            # list the moves a species can legally know at a level
build/teamcheck --abilities SPECIES              # the species' two abilities
build/teamcheck --items                          # holdable items
```

The flat TSV form (one mon per line) is: `team_id slot species level m1 m2 m3 m4 item abilityNum nature
iv0..iv5 ev0..ev5 happiness ot fateful format` with numeric ids; `sim/src/sim_teams.c` loads it into a party
with `Sim_MakeMonEx`.
