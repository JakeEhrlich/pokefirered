# Team corpus

Seed datasets of teams for the battle simulator (format: `FORMAT.md`; tooling: `tools/teams.py`,
`build/teamcheck`). Each dataset is one JSONL file with its own write-up (`<name>.md`) describing where the
teams came from and how to regenerate them. The training mix is decided separately; these are the raw pools.

| dataset | teams | ok | encodable | what it is |
|---|---|---|---|---|
| `game_trainers` | 742 | 732 | 10 | every trainer party exactly as the game builds it (`tests/dump_trainer_teams.c`) |
| `smogon_adv` | 5647 | 1755 | 3892 | real ADV teams: Smogon sample-team/resource threads, tour dumps, builder exports, usage-stats synthetic (`tools/import_showdown.py`, raw pastes in `sources/`) |
| `showdown_randbats` | 1500 | 481 | 1019 | teams generated like Showdown's Gen 3 Random Battle (`tools/gen_randbats_teams.py`, data in `sources/`) |
| `random_valid` | 2500 | 2500 | 0 | uniformly random but fully legal mons (`tools/gen_random_teams.py --mode valid`) |
| `random_encodable` | 2500 | 10 | 2490 | uniformly random over everything the struct can hold, mostly illegal (`tools/gen_encodable_teams.py`) |
| `playthrough` | 405 | 405 | 0 | likely player teams at 15 story checkpoints x 3 starters x 7 styles (`tools/gen_playthrough_teams.py`) |
| `strategies` | 136 | 135 | 1 | hand-built archetypes (37, `tools/gen_strategy_teams.py`): stall, spikes, Baton Pass, boom, weather, Curselax, FEAR-style, doubles ... |
| `agent_picks_competitive` | 60 | 60 | 0 | an agent's own competitive builds (OU/UU/NU/Ubers/doubles) |
| `agent_picks_casual` | 60 | 60 | 0 | what a casual story player actually runs (three level bands) |
| `agent_picks_gimmick` | 60 | 60 | 0 | monotype, area-only, Ditto, Metronome, Magikarp, Smeargle, Shedinja, hax, doubles nonsense |

`ok` = legal in FireRed by the simulator's rules; `encodable` = the simulator can battle with it but some move,
item or ability is not obtainable in FireRed (Emerald tutors, event moves, bad pastes); each dataset tags those
teams `not_frlg_legal` where the author knew. No dataset contains `invalid` teams.

Re-check everything:
```
for f in teams/*.jsonl; do python3 tools/teams.py check $f --quiet | tail -1; done
```

## Team ratings

`sim/ratings/randbats_team_elo_100k_bt.json`: Bradley-Terry ratings of the 1500 randbats teams from 100000
games with `rmplus:iters=30` on both sides (about 133 games per team, standard error about 31 points, true
spread about 51 points). Produced with `build/arena --rate-teams` and `tools/elo_fit.py --players teams`.
Use these to draw evenly matched teams for evaluations.
