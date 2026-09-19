# smogon_adv.jsonl: competitive Gen 3 (ADV) teams from Smogon

Real competitive ADV teams and sets, converted from Showdown export text with `tools/import_showdown.py`.
Rebuild with `python3 teams/sources/smogon_adv/build.py` (raw inputs live under `teams/sources/smogon_adv/`).

## Counts

`tools/teams.py check teams/smogon_adv.jsonl`: **5647 teams: 1736 ok, 3911 encodable, 0 invalid.**
Every team has 6 mons at level 100 (LC synthetic teams: level 5).

| kind | teams | ok | encodable | tag |
|---|---|---|---|---|
| curated: sample-team threads, resource threads, single-team pastes, historical archives | 535 | 156 | 379 | `sample-team`, `team-dump`, `historical` (no `builder-dump`) |
| whole-teambuilder exports posted by players (one paste = 10-1460 teams) | 4690 | 1475 | 3215 | `builder-dump` |
| synthetic: 6 individually posted sets packed by usage-stats teammate affinity | 202 | | | `synthetic_from_sets` (source `smogon`) |
| synthetic: most-common set per mon from Showdown usage stats, mon + its top teammates | 220 | | | `synthetic_from_sets`, `usage-stats` (source `usage_stats`) |

Synthetic teams together: 105 ok, 317 encodable.

| tier tag | total | curated | builder-dump | synthetic | ok | encodable |
|---|---|---|---|---|---|---|
| `ou` | 5347 | 437 | 4690 | 220 | 1654 | 3693 |
| `uu` | 74 | 36 | 0 | 38 | 22 | 52 |
| `nu` | 70 | 45 | 0 | 25 | 34 | 36 |
| `ubers` | 59 | 17 | 0 | 42 | 6 | 53 |
| `lc` | 14 | 0 | 0 | 14 | 0 | 14 |
| `ru` | 26 | 0 | 0 | 26 | 16 | 10 |
| `uubl` | 22 | 0 | 0 | 22 | 1 | 21 |
| `zu` | 25 | 0 | 0 | 25 | 3 | 22 |
| `doubles-ou` | 10 | 0 | 0 | 10 | 0 | 10 |

The ok/encodable split is what you would expect of ADV: Smogon's tier assumes Emerald/XD/Colosseum tutor and event
moves. The commonest reasons a team is only `encodable` (each is tagged `not_frlg_legal` with the checker's
reason in `notes`): Jirachi Fire Punch / Ice Punch / Dynamic Punch (Emerald tutor), Gengar Ice Punch / Fire Punch,
Snorlax Self-Destruct, Zapdos Baton Pass (XD), Blissey / Salamence Wish (event), Moltres Will-O-Wisp, Smeargle
sketched moves, Suicune / Zapdos Sleep Talk, Claydol Refresh, Deoxys Recover / Spikes / Extreme Speed. About 12
teams have abilities the species cannot have (bad pastes); they are kept and flagged the same way.

## Sources

All fetched with `curl` (Smogon's forums and pokepast.es serve plain HTML/raw text to a browser user agent); the
post-segmented thread text and every referenced paste are saved so the conversion is reproducible offline.

Forum threads (`sources/smogon_adv/threads/<name>.txt` = post text, `<name>.links.jsonl` = paste links per post):

| id prefix | thread | tags |
|---|---|---|
| `smogon_ou_sample_*` | [ADV OU Sample Teams](https://www.smogon.com/forums/threads/adv-ou-sample-teams.3687813/) (posts 1 and 2, maintained by the ADV mod team; teams by UD, M Dragon, BKC, ABR, Hclat, Kerts, eden, Star, vapicuno, SEA, ...) | `adv ou sample-team` |
| `smogon_ou_builder_tlc2019/2020/2020v/2021_*`, `smogon_ou_builder_ibidem2017_*` | thelinearcurve's 2019/2020/2021 and Ibidem's 2017 teambuilder dumps, linked from post 2 of the sample thread (pastebin `jyFh4AF3`, `tUytkX29`, `QbwUxw0P`, `6xCb1crp`, `i7HzJQmC`) | `adv ou team-dump builder-dump` |
| `smogon_ou_builder_l3w_*` | L3W's teambuilder (pastebin `iAKXBuaD`, from the set-sharing thread) | `adv ou team-dump builder-dump` |
| `smogon_ou_sharing_p<page>_*`, `smogon_ou_sharing_inline_*` | [ADV OU Set and Team Sharing](https://www.smogon.com/forums/threads/set-dump-team-dump-and-previously-general-metagame-discussion.3648620/) pages 1-10 (pokepastes and inline importables; includes SPL/tour team dumps by McMeghan, PDC, Jirachee, M Dragon, CALLOUS, giraffefromholland, pixie909, Zpanther ...) | `adv ou team-dump` (+ `builder-dump` when a single paste holds > 6 teams) |
| `smogon_ou_ages_*` | [ADV Teams Through The Ages](https://www.smogon.com/forums/threads/adv-teams-through-the-ages.3687203/) by M Dragon (Netbattle era to 2019) | `adv ou historical` |
| `smogon_uu_sample_*` | [ADV UU Sample Teams](https://www.smogon.com/forums/threads/adv-uu-sample-teams.3701817/) | `adv uu sample-team` |
| `smogon_uu_res_*` | [ADV UU Resources](https://www.smogon.com/forums/threads/adv-uu-resources.3733346/) (offense / balance team lists) | `adv uu sample-team` |
| `smogon_nu_sample_*` | [ADV NU Resources](https://www.smogon.com/forums/threads/adv-nu-resources.3730732/) sample teams | `adv nu sample-team` |
| `smogon_nu_disc_*` | [ADV NU Discussion](https://www.smogon.com/forums/threads/adv-nu-discussion.3761091/) team posts | `adv nu team-dump` |
| `smogon_ubers_sample_*` | [ADV Ubers Sample Teams](https://www.smogon.com/forums/threads/adv-ubers-sample-teams.3524399/) (inline importables) | `adv ubers sample-team` |
| `smogon_ubers_archive_*` | [Ubers Sample Teams (ORAS/BW/DPP/ADV)](https://www.smogon.com/forums/threads/sample-teams-oras-bw-dpp-adv-submissions-open.3580622/) ADV post (#6963709) | `adv ubers sample-team` |
| `smogon_ubers_hub_inline_*` | [Ubers Old Gens Hub](https://www.smogon.com/forums/threads/ubers-old-gens-hub.3714123/) ADV post (#9462710) | `adv ubers sample-team` |

Set compendia (single sets, packed into `smogon_<tier>_sets_*` synthetic teams):
thelinearcurve 2019 / 2020 set compendia (pastebin `haMgma5w`, `ubq1NEVR`), Jhonx's 2021 compilation (`qjNWD0bJ`),
the ADV UU set compendium (pokepaste `e54aa8461ca18693`), the ADV Ubers set compendium by SEA (`5fc1d96ac246ec5a`),
plus every 1-5 mon paste and inline set from the threads above.

Showdown usage statistics (`sources/smogon_adv/stats/`, `usage_gen3<tier>_<year>_*`):
`https://www.smogon.com/stats/<month>/moveset/<format>-1760.txt` for gen3ou (2024-12, 2018-12; 2016/2020/2022 also
saved), gen3uu (2025-06), gen3nu (2021-06), gen3ubers (2016-12), gen3lc (2024-03), gen3ru (2025-03), gen3uubl
(2025-03), gen3zu (2024-12) and gen3doublesou-1500 (2022-12). For each of the 30 most used mons: its most common
ability / item / spread / top-4 moves, plus the same for its 5 most common teammates.

Not usable: the [ADV OU Team Dump](https://www.smogon.com/forums/threads/adv-ou-team-dump.3584528/) and
[Ruins of Alph Sample Teams](https://www.smogon.com/forums/threads/ruins-of-alph-sample-teams.3650478/) threads
require a login. No ADV LC sample-team thread exists; LC comes only from usage stats.

## Conversion notes

* Stats are re-ordered from Showdown's HP/Atk/Def/SpA/SpD/Spe to the game's HP/Atk/Def/Spe/SpA/SpD.
* Hidden Power: the type is tagged `hp:<type>`; if the export's IVs already give that type under the gen-3
  formula they are kept, otherwise IVs are replaced by a 30/31 pattern giving the type at 70 power (about 130
  sets, mostly exports written with `IVs: 0 Atk` or with the wrong pattern; the replacement is noted).
  4 sets have a bare "Hidden Power" with no type: IVs left as given, noted.
* Happiness is 255 (Showdown's default) unless the export says otherwise or the set runs Frustration (0).
* Forms are folded onto the FireRed species: `Deoxys-Attack`/`-Defense`/`-Speed` -> `DEOXYS` with a
  `form:deoxys-<form>` tag (FireRed's Deoxys is the Attack form; the sim uses that regardless of the tag).
  Mew and Deoxys carry `fateful: true`.
* Teams under a `=== [format] name ===` header are kept only when the format starts with `gen3`
  (`gen3ou`, `gen3ou-box`, ...). Whole-builder dumps also held ~200 `gen9` teams and a few teams filed under
  invented folder names like `gen6asta edit`; those were dropped.
* Headerless pastes with more than 6 mons: prose-separated groups of 6 are teams; a single run of 6k mons is
  split into k teams (tag `split-by-6`, 2 teams); anything else is treated as a set compendium.
* Exact duplicate teams (same species, moves, item, nature and EVs) are dropped, first occurrence wins
  (~2600 duplicates: the sample thread links the same pastes twice, and thelinearcurve's yearly dumps overlap).
* Unresolvable pastes (68, all non-gen-3 content or prose mis-parsed as a set) are skipped; see
  `sources/smogon_adv/convert.log`.
* Archetype tags (`stall`, `offense`, `balance`, `spikes`, `rain`, `sun`, `baton-pass`, `cm-spam`, `sand`,
  `lead`) are keyword matches on the team name / link context and are only present where the thread stated one.

## Caveats

* OU dominates (5347 of 5647); 4690 of those are whole-teambuilder exports, which include in-progress or
  experimental teams next to tournament teams. Filter on the absence of `builder-dump` for the curated subset
  (535 teams), or on `sample-team` for the 126 mod-curated ones.
* Synthetic teams (`synthetic_from_sets`) are not teams anyone played; each mon's set is real.
* Nicknames from pastes are kept when they fit the 10-char ASCII rule; set compendia "nicknames" (set names)
  are dropped.
* Many sets use Emerald tutor / event moves, so most teams are `encodable` rather than `ok`; the simulator
  battles with them, but they are not obtainable in FireRed.
