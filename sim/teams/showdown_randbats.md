# showdown_randbats

1500 teams generated the way Pokémon Showdown's **[Gen 3] Random Battle** builds a side, converted to
FireRed constants. `source: "showdown_randbats"`, ids `randbats_1` .. `randbats_1500`, tags
`["gen3", "randbats"]` (+ `deoxys-<forme>` when the team carries a Deoxys).

Regenerate: `python3 sim/tools/gen_randbats_teams.py [--n 1500] [--seed 3] [--hp-ivs] [-o ...]`

## Source

| file | origin |
|---|---|
| `sources/showdown_randbats/sets.json` | `data/random-battles/gen3/sets.json` |
| `sources/showdown_randbats/teams.ts` | `data/random-battles/gen3/teams.ts` (`RandomGen3Teams`) |
| `sources/showdown_randbats/gen4_teams.ts` | `data/random-battles/gen4/teams.ts` (parent class) |
| `sources/showdown_randbats/gen9_teams.ts` | `data/random-battles/gen9/teams.ts` (base class: `queryMoves`, `addMove`, `getPokemonPool`, ...) |

Repo `smogon/pokemon-showdown`, files last changed in commit `0490edb43b60fdaf2fad7db6010f1379f225e8e7`
("Randomized format updates (#12308)", 2026-09-13); fetched from `master` at
`2ddfa0476f8207e12e204b1c69f7c7683b17633c` (2026-09-17).
URL: https://github.com/smogon/pokemon-showdown/tree/0490edb43b60fdaf2fad7db6010f1379f225e8e7/data/random-battles/gen3

Hidden Power IV spreads come from `data/typechart.ts` (`HPivs`), verified against the game's own
Hidden Power formula (each spread gives the intended type at 70 BP).

## What the generator reproduces

`sim/tools/gen_randbats_teams.py` is a method-by-method port of `RandomGen3Teams` and the inherited
gen4/gen9 helpers it actually calls:

- **Species pool**: the 220 entries of `sets.json` (217 species; Deoxys has 4 formes and is therefore
  drawn twice as often, exactly as `getPokemonPool` weights base species with 4+ formes).
- **Team assembly** (`randomTeam`): species clause, at most 2 mons sharing a type, at most 3 weak
  / 1 doubly-weak to any type (using Showdown's `getEffectiveness` semantics: immunities count as
  neutral, abilities ignored), one level-100 mon, Tyranitar vs Shedinja/Flail/Reversal-user
  incompatibility, the lead slot (Dugtrio/Wobbuffet never lead; the lead is generated with
  `isLead`), and the `teamDetails` flags (rain/sun/sand, status cure, spikes, rapid spin) that steer
  later sets.
- **Sets** (`randomSet` / `randomMoveset` / `cullMovePool`): one of the species' sets is drawn, then
  the role-driven enforcement order (Seismic Toss/Spikes/Spore, Substitute+Baton Pass, preferred
  types, per-type STAB checkers, fallback STAB, recovery for bulky roles, Staller's
  Protect/Toxic/Wish, setup, Berry Sweeper Flail/Reversal + Endure/Substitute, at least one attack,
  coverage), the incompatible-move culls, move pairs, hidden-power deduplication, team-based culls.
- **Ability** (`getAbility`): Yanma hard-code, Chlorophyll/Swift Swim only under the team's
  weather, Rock Head only with recoil, else random among the set's abilities.
- **Item** (`getItem`): the full priority chain (Soul Dew, Silk Scarf Linoone, Thick Club, Light
  Ball, Unown, White Herb Deoxys, Choice Band for Trick / 4 physical / 3 physical + Baton Pass or
  mixed Wallbreaker, Lum for DD / Belly Drum / Shedinja, Petaya, pinch berries for Berry Sweepers
  and Swords Dance sets with the speed tiers, Stick, Leftovers).
- **Level**: `level` from `sets.json` (66..100), 80 if absent.
- **EVs/IVs**: 85 in every stat, IVs 31; the HP-EV trimming for Substitute + Flail/Reversal (HP not
  divisible by 4), Substitute + pinch berry (HP divisible by 4) and Belly Drum (odd HP); Atk EV 0
  and Atk IV 0 on sets with no physical move (and no Transform).
- **Nature**: Showdown sets none (neutral); written as `HARDY`. **Happiness** 255 (Showdown's
  default, matters for Return).
- Moves are shuffled at the end as in Showdown.

Move/species data (types, base power, accuracy, priority, fixed-damage / recoil effects, base
stats, type chart) come from the game's tables in this repo rather than Showdown's dex; Gen 3
categories are derived from the move type. Deoxys forme stats are hard-coded from Showdown.

## Simplifications

- **PRNG**: Python's `random.Random(seed)` instead of Showdown's PRNG. Same decision structure and
  distribution, but seeds do not reproduce Showdown's streams.
- **Hidden Power**: written as `HIDDEN_POWER` with all-31 IVs; the intended type is in the mon's
  tags (`hp:ghost`). In the game this plays as HP Dark 70. `--hp-ivs` writes Showdown's gen-3 IV
  spreads instead (then the type is right in-game; Atk IV becomes `spread - 28` as in Showdown).
  Because Atk IV 0 would change the HP type, the all-31 mode keeps Atk IV 31 on HP sets (Atk EV is
  still 0).
- **Deoxys formes**: FireRed has one Deoxys (`DEOXYS`, Attack-forme stats in-game). The forme drawn
  by the generator (`normal`/`attack`/`defense`/`speed`) only affects the set choice, level, item
  and the HP-EV math, and is recorded in the mon and team tags.
- **Per-battle state**: `battleHasWobbuffet`/`battleHasDitto` (one Wobbuffet per *battle*) are
  reset per team, since each team here is an independent side.
- Dropped: monotype branch, shiny roll, gender, `forceofthefallenmod`, nickname.
- Mew and Deoxys get `fateful: true` so they obey in the simulator.
- Mon-level `tags` (`role:...`, `forme:...`, `hp:...`) are extra fields the checker ignores.

## Validation

`python3 sim/tools/teams.py check sim/teams/showdown_randbats.jsonl` (seed 3):

```
showdown_randbats.jsonl: 1500 teams: 465 ok, 1035 encodable, 0 invalid
```

All `encodable` verdicts are moves FireRed cannot teach (1572 flagged mon-moves on 1572 distinct
mons = 17.5% of the 9000 mons; 69% of teams contain at least one). No item or ability issues. The
offending moves are Emerald tutors, XD/Colosseum or event moves that Showdown allows in Gen 3:

| move | mons | typical carriers |
|---|---|---|
| Sleep Talk (Emerald tutor) | 527 | Registeel, Arbok, Articuno, Lanturn, Kyogre, ... |
| Fire/Thunder/Ice Punch (Emerald tutor) | 296 | Grumpig, Typhlosion, Gardevoir, Jirachi, Gengar, ... |
| Morning Sun (XD) | 137 | Tangela, Moltres, Scizor, Butterfree |
| Wish (event) | 119 | Lickitung, Hypno, Kangaskhan |
| Heal Bell (XD/egg) | 93 | Lapras, Altaria |
| Surf (event) | 81 | Pikachu, Raichu |
| Endure (Emerald tutor) | 53 | |
| Sacred Fire | 39 | Ho-Oh (learns it at L77, randbats level is 70) |
| Self-Destruct / Explosion (Emerald tutor) | 44 | Qwilfish, ... |
| Extreme Speed, Spikes, Recover (Deoxys), Baton Pass (Shedinja), Leech Seed, Volt Tackle, Encore, Spore, Will-O-Wisp, Agility | 183 | |

These sets are kept as-is (the simulator battles with them); a FireRed-legal subset is the 465 `ok`
teams.

## Stats

`python3 sim/tools/teams.py stats sim/teams/showdown_randbats.jsonl`:

```
1500 teams, 9000 mons, 0 bad lines
team sizes: {6: 1500}
levels: min 66 max 100, top [(82, 865), (84, 712), (85, 656), (87, 609), (83, 549)]
top species: [('DEOXYS', 87), ('TANGELA', 58), ('AZUMARILL', 56), ('RAICHU', 54), ('CHIMECHO', 54), ('SUDOWOODO', 54), ('MACHAMP', 53), ('PIKACHU', 53), ('SANDSLASH', 53), ('MEWTWO', 53), ('ELECTRODE', 53), ('GOLEM', 52), ('UMBREON', 51), ('GOREBYSS', 51), ('SKARMORY', 51)]
top items: [('LEFTOVERS', 6570), ('CHOICE_BAND', 1091), ('SALAC_BERRY', 417), ('PETAYA_BERRY', 362), ('LIECHI_BERRY', 184), ('LUM_BERRY', 134), ('SOUL_DEW', 69), ('LIGHT_BALL', 53), ('WHITE_HERB', 42), ('THICK_CLUB', 37)]
top moves: [('HIDDEN_POWER', 4243), ('TOXIC', 2620), ('EARTHQUAKE', 2247), ('ICE_BEAM', 1394), ('SUBSTITUTE', 1291), ('ROCK_SLIDE', 1226), ('SURF', 1152), ('THUNDERBOLT', 1005), ('PROTECT', 1004), ('PSYCHIC', 962), ('SHADOW_BALL', 930), ('CALM_MIND', 809), ('REST', 801), ('DOUBLE_EDGE', 797), ('RETURN', 776)]
distinct species 217 / moves 113 / items 13
```

4243 of 9000 mons (47%) carry a Hidden Power.
