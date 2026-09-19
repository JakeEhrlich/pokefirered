# agent_picks_gimmick

60 hand-written teams from the point of view of a player who loves gimmicks, themes and weird-but-real ideas: monotype teams (all 17 types, plus a level-50 one), single-area teams, Ditto/Transform, Metronome-only, Wobbuffet/Wynaut, Splash-only Magikarp, item theft, Smeargle, Shedinja, Unown, Ubers, babies, confusion/infatuation hax, doubles nonsense, sleep spam, weather, Perish trap, Salac/Flail, Skill Swap, Curse and more. Source `agent`, tags `persona:gimmick` + one theme tag. Written by an agent from FRLG learnset knowledge and validated with `tools/teams.py check`; intended verdicts: all `ok` except the teams tagged `not_frlg_legal` (which are `encodable`: the gimmick needs something the checker cannot model or FireRed cannot do; the notes say why). Stat order in the file is HP/Atk/Def/Spe/SpA/SpD.

| id | format | lv | theme | mons | gimmick |
|---|---|---|---|---|---|
| apg_1_mono_normal | singles | 100 | monotype:normal | SNORLAX TAUROS BLISSEY KANGASKHAN DODRIO PORYGON2 | Six Normal-types leaning on Kanto bulk: Snorlax, Blissey and Tauros carry, Porygon2 and Kangaskhan support. |
| apg_2_mono_fire | singles | 100 | monotype:fire | CHARIZARD ARCANINE NINETALES RAPIDASH MAGMAR HOUNDOOM | All-Fire team: every FRLG Fire-type worth using, Sunny Day support from Ninetales. |
| apg_3_mono_water | singles | 100 | monotype:water | STARMIE VAPOREON KINGDRA LAPRAS GYARADOS CLOYSTER | All-Water team with Rain Dance support for Kingdra and a Curse Lapras. |
| apg_4_mono_grass | singles | 100 | monotype:grass | VENUSAUR EXEGGUTOR VILEPLUME VICTREEBEL JUMPLUFF TANGELA | All-Grass team: sleep powders everywhere and Sunny Day Chlorophyll sweepers. |
| apg_5_mono_electric | singles | 100 | monotype:electric | JOLTEON RAICHU MAGNETON ELECTRODE LANTURN ELECTABUZZ | All-Electric team; Magneton and Lanturn cover each other, everyone paralyzes. |
| apg_6_mono_ice | singles | 100 | monotype:ice | LAPRAS CLOYSTER DEWGONG JYNX PILOSWINE SNEASEL | All-Ice team with Hail from Walrein-less Kanto: Lapras, Cloyster, Dewgong, Jynx, Piloswine, Sneasel. |
| apg_7_mono_fighting | singles | 100 | monotype:fighting | MACHAMP HITMONLEE HITMONCHAN HITMONTOP HERACROSS PRIMEAPE | All-Fighting team: five Kanto brawlers and Heracross, Hitmontop with Intimidate. |
| apg_8_mono_poison | singles | 100 | monotype:poison | GENGAR WEEZING MUK CROBAT NIDOKING TENTACRUEL | All-Poison team: Gengar, Weezing and Muk are real, the rest is for the theme. |
| apg_9_mono_ground | singles | 100 | monotype:ground | DUGTRIO GOLEM RHYDON MAROWAK NIDOQUEEN QUAGSIRE | All-Ground team: Earthquake six times over, Quagsire and Nidoqueen bring the coverage. |
| apg_10_mono_flying | singles | 100 | monotype:flying | ZAPDOS AERODACTYL SKARMORY CROBAT DODRIO PIDGEOT | All-Flying team: Zapdos and Aerodactyl lead, Skarmory Spikes, Crobat, Dodrio, Pidgeot. |
| apg_11_mono_psychic | singles | 100 | monotype:psychic | ALAKAZAM STARMIE HYPNO SLOWBRO EXEGGUTOR XATU | All-Psychic team: Alakazam and Starmie go fast, Slowbro and Hypno stall. |
| apg_12_mono_bug | singles | 100 | monotype:bug | SCIZOR HERACROSS FORRETRESS PINSIR VENOMOTH BUTTERFREE | All-Bug team: Scizor, Heracross and Forretress carry the weaker Kanto bugs. |
| apg_13_mono_rock | singles | 100 | monotype:rock | TYRANITAR AERODACTYL GOLEM OMASTAR KABUTOPS RHYDON | All-Rock team: Tyranitar summons sand, every one of them has Rock Slide. |
| apg_14_mono_ghost | singles | 100 | monotype:ghost | GENGAR MISDREAVUS DUSCLOPS BANETTE HAUNTER SHEDINJA | All-Ghost team: every Gen 3 Ghost line that FireRed can hold, Shedinja included. |
| apg_15_mono_dragon | singles | 100 | monotype:dragon | DRAGONITE KINGDRA SALAMENCE FLYGON ALTARIA DRAGONAIR | All-Dragon team: Dragonite, Kingdra, Salamence, Flygon, Altaria and a Dragonair for purity. |
| apg_16_mono_dark | singles | 100 | monotype:dark | TYRANITAR HOUNDOOM UMBREON SNEASEL MURKROW ABSOL | All-Dark team: Tyranitar and Houndoom hit, Umbreon walls, Sneasel and Murkrow annoy. |
| apg_17_mono_steel | singles | 100 | monotype:steel | SKARMORY MAGNETON FORRETRESS SCIZOR STEELIX METAGROSS | All-Steel team: Skarmory and Forretress stack Spikes while Metagross and Scizor clean. |
| apg_18_mono_water_lv50 | singles | 50 | monotype:water | STARMIE VAPOREON GYARADOS BLASTOISE POLIWRATH KINGLER | Level-50 all-Water team for the Battle Tower style cap. |
| apg_19_route1 | singles | 100 | area:route_1 | PIDGEOT RATICATE PIDGEOTTO RATTATA PIDGEY RATICATE | Only Pokemon that appear on Route 1: Pidgey and Rattata lines, three each. |
| apg_20_viridian_forest | singles | 100 | area:viridian_forest | BUTTERFREE BEEDRILL PIKACHU METAPOD KAKUNA CATERPIE | Only Viridian Forest Pokemon: Caterpie and Weedle lines plus the forest Pikachu. |
| apg_21_mt_moon | singles | 100 | area:mt_moon | CLEFABLE GOLEM PARASECT CROBAT OMASTAR KABUTOPS | Only Mt. Moon Pokemon: Clefairy, Zubat, Geodude, Paras lines and the two fossils. |
| apg_22_safari_zone | singles | 100 | area:safari_zone | KANGASKHAN SCYTHER PINSIR CHANSEY TAUROS DRAGONITE | Only Safari Zone catches: Kangaskhan, Scyther, Pinsir, Chansey, Tauros and a Dratini grown into Dragonite. |
| apg_23_power_plant | singles | 100 | area:power_plant | ZAPDOS ELECTABUZZ MAGNETON ELECTRODE RAICHU VOLTORB | Only Power Plant Pokemon: Electric-types from the abandoned plant, capped by Zapdos. |
| apg_24_cerulean_cave | singles | 100 | area:cerulean_cave | MEWTWO GOLBAT KADABRA PARASECT DITTO WOBBUFFET | Only Cerulean Cave residents: Mewtwo and the strong wild things that live around it. |
| apg_25_six_ditto | singles | 100 | gimmick:ditto | DITTO DITTO DITTO DITTO DITTO DITTO | Six Ditto, six different held items; the only move on the team is Transform. |
| apg_26_transform_mew | singles | 100 | gimmick:transform | MEW DITTO MEW DITTO MEW DITTO | Three Ditto and three fateful Mew: Mew learns Transform at level 10, so the team copies whatever it sees. |
| apg_27_metronome_only | singles | 100 | gimmick:metronome | CLEFABLE TOGETIC SNORLAX CLEFAIRY BLISSEY MEW | Metronome and nothing else on six mons; Serene Grace Togetic makes the random flinches worse. |
| apg_28_metronome_plus | singles | 100 | gimmick:metronome | CLEFABLE TOGETIC SNORLAX CHANSEY CLEFAIRY TOGEPI | Metronome users that keep one real move each to survive long enough to roll a good one. |
| apg_29_wobb_wynaut | singles | 100 | gimmick:wobbuffet | WOBBUFFET WYNAUT WOBBUFFET WYNAUT WOBBUFFET WYNAUT | Three Wobbuffet and three Wynaut: Shadow Tag traps you, Counter and Mirror Coat do the rest. |
| apg_30_six_magikarp_splash | singles | 100 | gimmick:magikarp | MAGIKARP MAGIKARP MAGIKARP MAGIKARP MAGIKARP MAGIKARP | Six level-100 Magikarp that only know Splash: perfectly legal, perfectly hopeless. |
| apg_31_magikarp_flail | singles | 100 | gimmick:magikarp | MAGIKARP MAGIKARP MAGIKARP FEEBAS MAGIKARP WYNAUT | Magikarp that actually attack: Flail off a Salac Berry, with a Feebas and a Wynaut for company. |
| apg_32_level5_magikarp | singles | 5 | gimmick:magikarp | MAGIKARP MAGIKARP MAGIKARP MAGIKARP MAGIKARP MAGIKARP | Six freshly-bought level-5 Magikarp from the Mt. Moon salesman, Splash only. |
| apg_33_trick_theft | singles | 100 | gimmick:trick | ALAKAZAM MR_MIME KECLEON KADABRA GENGAR SNORLAX | Trick team: Choice Bands and Macho Braces get handed to the opponent by Alakazam, Mr. Mime and Kecleon. |
| apg_34_knock_off_thief | singles | 100 | gimmick:item_theft | SNEASEL PERSIAN BANETTE MURKROW KECLEON SABLEYE | Item-removal team: Knock Off, Thief and Covet on everything, most of them holding nothing so Thief actually steals. |
| apg_35_smeargle_sketch_only | singles | 100 | gimmick:smeargle | SMEARGLE SMEARGLE SMEARGLE SMEARGLE SMEARGLE SMEARGLE | Six Smeargle that only know Sketch: they have to copy the opponent mid-battle to do anything at all. |
| apg_36_smeargle_sketched | singles | 100 | gimmick:smeargle, not_frlg_legal | SMEARGLE SMEARGLE SMEARGLE SMEARGLE SMEARGLE SMEARGLE | Six Smeargle with pre-Sketched movesets (Spore, Belly Drum, Baton Pass, Spikes, Sheer Cold...); real in FRLG but the checker only credits Sketch itself, so it reads as encodable. |
| apg_37_shedinja_wonder_guard | singles | 100 | gimmick:shedinja | SHEDINJA CLAYDOL BLISSEY SWAMPERT UMBREON SKARMORY | Shedinja and Wonder Guard: Rapid Spin support, cleric Blissey, and walls that beat the six types that hurt it. |
| apg_38_six_shedinja | singles | 100 | gimmick:shedinja | SHEDINJA SHEDINJA SHEDINJA SHEDINJA SHEDINJA SHEDINJA | Six Shedinja: one HP each, Wonder Guard on all of them, and hopefully no Sandstorm. |
| apg_39_six_unown | singles | 100 | gimmick:unown | UNOWN UNOWN UNOWN UNOWN UNOWN UNOWN | Six Unown, all Hidden Power only, with IVs tuned to six different Hidden Power types. |
| apg_40_ubers_all_legendary | singles | 100 | gimmick:ubers | MEWTWO LUGIA HO_OH KYOGRE GROUDON RAYQUAZA | All-legendary Ubers team: Mewtwo, Lugia, Ho-Oh, Kyogre, Groudon, Rayquaza. |
| apg_41_kanto_legends | singles | 100 | gimmick:ubers | ARTICUNO ZAPDOS MOLTRES MEWTWO MEW DEOXYS | Every legendary FireRed itself hands out: the three birds, Mewtwo, event Mew and Deoxys. |
| apg_42_babies_lv5 | singles | 5 | gimmick:babies | PICHU CLEFFA IGGLYBUFF TOGEPI SMOOCHUM ELEKID | Six freshly hatched babies at level 5: Pichu, Cleffa, Igglybuff, Togepi, Smoochum, Elekid. |
| apg_43_babies_lv100 | singles | 100 | gimmick:babies | PICHU CLEFFA IGGLYBUFF TOGEPI SMOOCHUM ELEKID | Unevolved babies raised to level 100 with all their egg moves and TMs. |
| apg_44_tyrogue_trio | singles | 100 | gimmick:babies | TYROGUE HITMONLEE HITMONCHAN HITMONTOP MAGBY ELEKID | Tyrogue and its three Hitmons plus Magby and Elekid: the punch-and-kick nursery. |
| apg_45_attract_confuse_hax | singles | 100 | gimmick:hax | PERSIAN JYNX GENGAR UMBREON CLEFABLE CROBAT | Attract, Swagger and Confuse Ray on everything: win by making the opponent hit itself. |
| apg_46_paraflinch_hax | singles | 100 | gimmick:hax | TOGETIC JIRACHI DUNSPARCE BLISSEY SNORLAX GLIGAR | Paraflinch: Thunder Wave then Serene Grace Body Slam / Headbutt / Bite, plus Quick Claw and Bright Powder. |
| apg_47_evasion_hax | singles | 100 | gimmick:hax | CLEFABLE CACTURNE TYRANITAR SANDSLASH GLIGAR DUGTRIO | Double Team, Minimize and Sand Veil plus Bright Powder: an evasion team no honest player enjoys facing. |
| apg_48_fake_out_boom_doubles | doubles | 100 | gimmick:doubles_explosion | PERSIAN ELECTRODE GOLEM WEEZING GENGAR KANGASKHAN | Doubles: Fake Out to buy a turn, then Explosion or Self-Destruct on everything that has it. |
| apg_49_eq_levitate_doubles | doubles | 100 | gimmick:doubles_earthquake | DUGTRIO GENGAR SWAMPERT CLAYDOL METAGROSS FLYGON | Doubles: everyone spams Earthquake while the partner is Levitate, Flying or a Skarmory. |
| apg_50_follow_me_doubles | doubles | 100 | gimmick:doubles_support | CLEFABLE TOGETIC PLUSLE MINUN MACHAMP SALAMENCE | Doubles: Follow Me Clefable and Togetic redirect while Plusle and Minun Helping Hand the sweepers. |
| apg_51_skill_swap_slaking_doubles | doubles | 100 | gimmick:skill_swap | SLAKING ALAKAZAM GARDEVOIR EXEGGUTOR SHEDINJA DUSCLOPS | Doubles: Skill Swap partners hand their ability to Slaking to get rid of Truant, and swap Wonder Guard onto Shedinja... or away from it. |
| apg_52_spore_spam | singles | 100 | gimmick:sleep | BRELOOM PARASECT VILEPLUME VENUSAUR EXEGGUTOR BUTTERFREE | Sleep spam with 100% accurate Spore where possible, Sleep Powder elsewhere. |
| apg_53_hypnosis_dream_eater | singles | 100 | gimmick:sleep | HYPNO GENGAR HAUNTER NINETALES POLIWRATH DROWZEE | Hypnosis into Dream Eater or Nightmare on everything that can do it. |
| apg_54_sunny_day_chlorophyll | singles | 100 | gimmick:weather_sun | NINETALES EXEGGUTOR CHARIZARD VICTREEBEL JUMPLUFF VILEPLUME | Sunny Day team: Ninetales and Charizard set it, Chlorophyll Grass-types abuse Solar Beam. |
| apg_55_rain_dance_swift_swim | singles | 100 | gimmick:weather_rain | KINGDRA LUDICOLO OMASTAR ZAPDOS LANTURN GOLDUCK | Rain Dance team: Swift Swim Kingdra, Ludicolo and Omastar sweep under rain with Thunder support. |
| apg_56_sandstorm_sand_veil | singles | 100 | gimmick:weather_sand | TYRANITAR DUGTRIO CACTURNE SANDSLASH SKARMORY CLAYDOL | Sandstorm team: Tyranitar sets it, Sand Veil Dugtrio and Cacturne dodge, Rock and Steel-types ignore it. |
| apg_57_hail_blizzard | singles | 100 | gimmick:weather_hail | WALREIN GLALIE REGICE CLOYSTER JYNX LAPRAS | Hail team: Walrein, Glalie and Regice keep it snowing and spam perfectly accurate Blizzard. |
| apg_58_perish_trap | singles | 100 | gimmick:perish_song | MISDREAVUS GENGAR UMBREON POLITOED ALTARIA WOBBUFFET | Perish Song plus Mean Look or Shadow Tag: every mon traps and counts the opponent down. |
| apg_59_endeavor_flail_salac | singles | 100 | gimmick:salac_flail | SWELLOW HERACROSS HITMONLEE RATICATE DODRIO LINOONE | Substitute down to a Salac Berry, then Endeavor, Flail or Reversal at full power. |
| apg_60_cursed_ghosts | singles | 100 | gimmick:curse | GENGAR DUSCLOPS BANETTE HAUNTER DUSKULL GASTLY | Ghost-type Curse on every mon (Cursed Body does not exist in Gen 3, so the move has to do): halve your HP to bleed theirs. |
