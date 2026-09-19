#!/usr/bin/env python3
"""Generate sim/teams/strategies.jsonl: hand-built teams that embody known Gen 3 strategies.

The teams are held here as data (see TEAMS at the bottom) so they can be perturbed or extended later.

  python3 sim/tools/gen_strategy_teams.py            # writes sim/teams/strategies.jsonl
  python3 sim/tools/gen_strategy_teams.py --check    # ...and runs teams.py check on it

Conventions: stat order in EV strings is game order via names (hp atk def spe spa spd);
happiness is forced to 255 when a mon carries RETURN. Ids are strat_<archetype>_<n>.
Deliberately illegal teams carry the tag "not_frlg_legal" and say why in `notes`.
"""
import json, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))          # sim/
OUT = os.path.join(ROOT, 'teams', 'strategies.jsonl')
STAT_IDX = {'hp': 0, 'atk': 1, 'def': 2, 'spe': 3, 'spa': 4, 'spd': 5}


def evs(spec):
    """'hp252 def252 spe4' -> [252, 0, 252, 4, 0, 0] (game order)."""
    out = [0] * 6
    for tok in spec.split():
        name = tok.rstrip('0123456789')
        out[STAT_IDX[name]] = int(tok[len(name):])
    assert sum(out) <= 510, spec
    return out


def M(species, moves, item='LEFTOVERS', nat='HARDY', ev='', ab=None, level=100, ivs=None, fateful=False, nick=None):
    mv = moves.split()
    m = {'species': species, 'level': level, 'moves': mv, 'item': item, 'nature': nat, 'evs': evs(ev)}
    if ab is not None:
        m['ability'] = ab
    if ivs is not None:
        m['ivs'] = ivs
    if 'RETURN' in mv:
        m['happiness'] = 255
    if fateful:
        m['fateful'] = True
    if nick:
        m['nickname'] = nick
    return m


TEAMS = []
_counts = {}


def T(arch, mons, notes, tags=(), level=100, fmt='singles', tier=None):
    n = _counts.get(arch, 0) + 1
    _counts[arch] = n
    stage = 'midgame' if level < 100 else 'endgame'
    tg = [arch] + ([tier] if tier else []) + [stage] + list(tags)
    if fmt == 'doubles':
        tg.append('doubles')
    TEAMS.append({'id': 'strat_%s_%d' % (arch, n), 'source': 'strategy', 'tags': tg, 'format': fmt,
                  'level_cap': level, 'notes': notes, 'mons': mons})


# ---------------------------------------------------------------------------------------------------
# Reusable competitive sets (level 100)
# ---------------------------------------------------------------------------------------------------
SKARM_SPIKES = lambda: M('SKARMORY', 'SPIKES WHIRLWIND DRILL_PECK PROTECT', nat='IMPISH', ev='hp252 def252 spe4', ab='KEEN_EYE')
SKARM_ROAR = lambda: M('SKARMORY', 'SPIKES ROAR DRILL_PECK REST', nat='IMPISH', ev='hp252 def200 spd56', ab='STURDY')
SKARM_TOXIC = lambda: M('SKARMORY', 'SPIKES WHIRLWIND TOXIC REST', nat='IMPISH', ev='hp252 def252 spd4', ab='KEEN_EYE')
BLISS_TOSS = lambda: M('BLISSEY', 'SOFT_BOILED SEISMIC_TOSS TOXIC AROMATHERAPY', nat='BOLD', ev='hp252 def252 spd4', ab='NATURAL_CURE')
BLISS_BELL = lambda: M('BLISSEY', 'SOFT_BOILED SEISMIC_TOSS ICE_BEAM HEAL_BELL', nat='BOLD', ev='hp180 def252 spd76', ab='NATURAL_CURE')
BLISS_CM = lambda: M('BLISSEY', 'CALM_MIND SOFT_BOILED ICE_BEAM THUNDERBOLT', nat='BOLD', ev='hp252 def252 spd4', ab='NATURAL_CURE')
BLISS_WALL = lambda: M('BLISSEY', 'SOFT_BOILED TOXIC PROTECT SEISMIC_TOSS', nat='BOLD', ev='hp252 def252 spd4', ab='NATURAL_CURE')
GENGAR_SPINBLOCK = lambda: M('GENGAR', 'THUNDERBOLT GIGA_DRAIN WILL_O_WISP TAUNT', nat='TIMID', ev='hp88 spe252 spa168', ab='LEVITATE')
GENGAR_ATK = lambda: M('GENGAR', 'THUNDERBOLT GIGA_DRAIN HIDDEN_POWER EXPLOSION', nat='HASTY', ev='atk40 spe252 spa216', ab='LEVITATE')
GENGAR_HYPNO = lambda: M('GENGAR', 'HYPNOSIS THUNDERBOLT GIGA_DRAIN EXPLOSION', nat='TIMID', ev='hp6 spe252 spa252', ab='LEVITATE')
DUSCLOPS_WALL = lambda: M('DUSCLOPS', 'WILL_O_WISP SEISMIC_TOSS PAIN_SPLIT PROTECT', nat='CAREFUL', ev='hp252 def100 spd156', ab='PRESSURE')
DUSCLOPS_REST = lambda: M('DUSCLOPS', 'WILL_O_WISP NIGHT_SHADE REST TOXIC', nat='CAREFUL', ev='hp252 def92 spd164', ab='PRESSURE')
MILOTIC_STALL = lambda: M('MILOTIC', 'SURF RECOVER TOXIC ICE_BEAM', nat='BOLD', ev='hp252 def252 spa4', ab='MARVEL_SCALE')
MILOTIC_MC = lambda: M('MILOTIC', 'SURF RECOVER MIRROR_COAT HYPNOSIS', nat='BOLD', ev='hp252 def252 spa4', ab='MARVEL_SCALE')
CLAYDOL_SPIN = lambda: M('CLAYDOL', 'RAPID_SPIN EARTHQUAKE PSYCHIC EXPLOSION', nat='RELAXED', ev='hp252 def92 spd164', ab='LEVITATE')
CLAYDOL_SCREENS = lambda: M('CLAYDOL', 'REFLECT LIGHT_SCREEN RAPID_SPIN EXPLOSION', nat='BOLD', ev='hp252 def252 spd4', ab='LEVITATE')
FORRY_SPIKES = lambda: M('FORRETRESS', 'SPIKES RAPID_SPIN EXPLOSION EARTHQUAKE', nat='RELAXED', ev='hp252 atk92 def164', ab='STURDY')
FORRY_HP = lambda: M('FORRETRESS', 'SPIKES RAPID_SPIN HIDDEN_POWER EXPLOSION', nat='RELAXED', ev='hp252 atk92 def164', ab='STURDY')
CLOYSTER_SPIKES = lambda: M('CLOYSTER', 'SPIKES SURF RAPID_SPIN EXPLOSION', nat='RELAXED', ev='hp252 def40 spd216', ab='SHELL_ARMOR')
SWAMP_CURSE = lambda: M('SWAMPERT', 'CURSE EARTHQUAKE REST HYDRO_PUMP', nat='RELAXED', ev='hp252 def140 spd116', ab='TORRENT')
SWAMP_ROAR = lambda: M('SWAMPERT', 'EARTHQUAKE SURF ROAR PROTECT', nat='RELAXED', ev='hp252 def216 spd40', ab='TORRENT')
SWAMP_MIXED = lambda: M('SWAMPERT', 'EARTHQUAKE ICE_BEAM SURF PROTECT', nat='RELAXED', ev='hp252 def216 spd40', ab='TORRENT')
SUICUNE_CM = lambda: M('SUICUNE', 'CALM_MIND SURF REST ROAR', nat='BOLD', ev='hp252 def220 spe36', ab='PRESSURE')
SUICUNE_CROC = lambda: M('SUICUNE', 'CALM_MIND SURF ICE_BEAM REST', nat='BOLD', ev='hp252 def128 spe128', ab='PRESSURE', item='CHESTO_BERRY')
SUICUNE_ROAR = lambda: M('SUICUNE', 'SURF ROAR REST TOXIC', nat='BOLD', ev='hp252 def252 spe4', ab='PRESSURE')
TTAR_DD = lambda: M('TYRANITAR', 'DRAGON_DANCE ROCK_SLIDE EARTHQUAKE TAUNT', nat='ADAMANT', ev='hp16 atk252 spe240', ab='SAND_STREAM')
TTAR_DD_HP = lambda: M('TYRANITAR', 'DRAGON_DANCE ROCK_SLIDE EARTHQUAKE HIDDEN_POWER', nat='ADAMANT', ev='hp16 atk252 spe240', ab='SAND_STREAM', item='LUM_BERRY')
TTAR_CB = lambda: M('TYRANITAR', 'ROCK_SLIDE EARTHQUAKE FOCUS_PUNCH HIDDEN_POWER', nat='ADAMANT', ev='hp72 atk252 spe184', ab='SAND_STREAM', item='CHOICE_BAND')
TTAR_TAUNT = lambda: M('TYRANITAR', 'TAUNT ROCK_SLIDE EARTHQUAKE FIRE_BLAST', nat='ADAMANT', ev='hp56 atk252 spe200', ab='SAND_STREAM')
TTAR_TYRANIBOAH = lambda: M('TYRANITAR', 'SUBSTITUTE FOCUS_PUNCH CRUNCH THUNDERBOLT', nat='QUIET', ev='hp252 spa252 spd4', ab='SAND_STREAM')
TTAR_PURSUIT = lambda: M('TYRANITAR', 'PURSUIT ROCK_SLIDE EARTHQUAKE TAUNT', nat='ADAMANT', ev='hp252 atk252 spe4', ab='SAND_STREAM')
MENCE_DD = lambda: M('SALAMENCE', 'DRAGON_DANCE EARTHQUAKE ROCK_SLIDE HIDDEN_POWER', nat='ADAMANT', ev='hp6 atk252 spe252', ab='INTIMIDATE', item='LUM_BERRY')
MENCE_DD_AA = lambda: M('SALAMENCE', 'DRAGON_DANCE EARTHQUAKE ROCK_SLIDE AERIAL_ACE', nat='ADAMANT', ev='hp6 atk252 spe252', ab='INTIMIDATE', item='LEFTOVERS')
MENCE_CB = lambda: M('SALAMENCE', 'HIDDEN_POWER EARTHQUAKE ROCK_SLIDE BRICK_BREAK', nat='ADAMANT', ev='hp6 atk252 spe252', ab='INTIMIDATE', item='CHOICE_BAND')
MENCE_MIX = lambda: M('SALAMENCE', 'DRAGON_CLAW FIRE_BLAST EARTHQUAKE ROCK_SLIDE', nat='NAUGHTY', ev='atk252 spe252 spa6', ab='INTIMIDATE')
META_STD = lambda: M('METAGROSS', 'METEOR_MASH EARTHQUAKE ROCK_SLIDE EXPLOSION', nat='ADAMANT', ev='hp152 atk252 spe104', ab='CLEAR_BODY')
META_AGI = lambda: M('METAGROSS', 'AGILITY METEOR_MASH EARTHQUAKE EXPLOSION', nat='ADAMANT', ev='hp72 atk252 spe184', ab='CLEAR_BODY', item='LUM_BERRY')
META_CB = lambda: M('METAGROSS', 'METEOR_MASH EARTHQUAKE ROCK_SLIDE EXPLOSION', nat='ADAMANT', ev='hp152 atk252 spe104', ab='CLEAR_BODY', item='CHOICE_BAND')
META_SCREENS = lambda: M('METAGROSS', 'REFLECT LIGHT_SCREEN METEOR_MASH EXPLOSION', nat='ADAMANT', ev='hp252 atk252 spe4', ab='CLEAR_BODY')
ZAPDOS_STD = lambda: M('ZAPDOS', 'THUNDERBOLT HIDDEN_POWER DRILL_PECK REST', nat='MODEST', ev='hp252 spe64 spa192', ab='PRESSURE')
ZAPDOS_SUB = lambda: M('ZAPDOS', 'SUBSTITUTE THUNDERBOLT HIDDEN_POWER TOXIC', nat='TIMID', ev='hp252 spe252 spa4', ab='PRESSURE')
ZAPDOS_PP = lambda: M('ZAPDOS', 'SUBSTITUTE PROTECT TOXIC THUNDERBOLT', nat='TIMID', ev='hp252 spe252 spa4', ab='PRESSURE')
ZAPDOS_RAIN = lambda: M('ZAPDOS', 'RAIN_DANCE THUNDER HIDDEN_POWER DRILL_PECK', nat='MODEST', ev='hp6 spe252 spa252', ab='PRESSURE', item='LEFTOVERS')
ZAPDOS_AGI = lambda: M('ZAPDOS', 'AGILITY THUNDERBOLT HIDDEN_POWER DRILL_PECK', nat='MODEST', ev='hp6 spe252 spa252', ab='PRESSURE')
CELEBI_CM = lambda: M('CELEBI', 'CALM_MIND PSYCHIC GIGA_DRAIN RECOVER', nat='BOLD', ev='hp252 def76 spe180', ab='NATURAL_CURE')
CELEBI_BP = lambda: M('CELEBI', 'CALM_MIND BATON_PASS PSYCHIC RECOVER', nat='BOLD', ev='hp252 def76 spe180', ab='NATURAL_CURE')
CELEBI_SEED = lambda: M('CELEBI', 'LEECH_SEED RECOVER PSYCHIC HEAL_BELL', nat='BOLD', ev='hp252 def128 spe128', ab='NATURAL_CURE')
CELEBI_PERISH = lambda: M('CELEBI', 'PERISH_SONG RECOVER PSYCHIC LEECH_SEED', nat='BOLD', ev='hp252 def128 spe128', ab='NATURAL_CURE')
JIRACHI_CM = lambda: M('JIRACHI', 'CALM_MIND PSYCHIC THUNDERBOLT WISH', nat='TIMID', ev='hp252 spe252 spa4', ab='SERENE_GRACE')
JIRACHI_WISH = lambda: M('JIRACHI', 'WISH PROTECT BODY_SLAM PSYCHIC', nat='CAREFUL', ev='hp252 def80 spd176', ab='SERENE_GRACE')
JIRACHI_SCREENS = lambda: M('JIRACHI', 'REFLECT LIGHT_SCREEN WISH PSYCHIC', nat='TIMID', ev='hp252 spe252 spa4', ab='SERENE_GRACE')
JIRACHI_PARA = lambda: M('JIRACHI', 'BODY_SLAM WISH PROTECT PSYCHIC', nat='CAREFUL', ev='hp252 def80 spd176', ab='SERENE_GRACE')
SNORLAX_CURSE = lambda: M('SNORLAX', 'CURSE BODY_SLAM SHADOW_BALL REST', nat='CAREFUL', ev='hp188 def68 spd252', ab='THICK_FAT')
SNORLAX_CURSE_EQ = lambda: M('SNORLAX', 'CURSE RETURN EARTHQUAKE REST', nat='CAREFUL', ev='hp144 atk112 spd252', ab='IMMUNITY')
SNORLAX_TALK = lambda: M('SNORLAX', 'REST SLEEP_TALK BODY_SLAM CURSE', nat='CAREFUL', ev='hp188 def68 spd252', ab='THICK_FAT')
SNORLAX_TALK_ATK = lambda: M('SNORLAX', 'REST SLEEP_TALK BODY_SLAM SHADOW_BALL', nat='ADAMANT', ev='hp144 atk112 spd252', ab='THICK_FAT')
SNORLAX_BD = lambda: M('SNORLAX', 'BELLY_DRUM RETURN SHADOW_BALL EARTHQUAKE', nat='ADAMANT', ev='hp112 atk252 def144', ab='THICK_FAT', item='LEFTOVERS')
SNORLAX_BD_REST = lambda: M('SNORLAX', 'BELLY_DRUM REST BODY_SLAM EARTHQUAKE', nat='ADAMANT', ev='hp112 atk252 def144', ab='THICK_FAT', item='CHESTO_BERRY')
SNORLAX_CB = lambda: M('SNORLAX', 'RETURN EARTHQUAKE SHADOW_BALL FOCUS_PUNCH', nat='ADAMANT', ev='hp144 atk252 spd112', ab='THICK_FAT', item='CHOICE_BAND')
SNORLAX_SUBPUNCH = lambda: M('SNORLAX', 'SUBSTITUTE FOCUS_PUNCH BODY_SLAM SHADOW_BALL', nat='ADAMANT', ev='hp144 atk252 spd112', ab='THICK_FAT')
STARMIE_SPIN = lambda: M('STARMIE', 'RAPID_SPIN SURF THUNDERBOLT RECOVER', nat='TIMID', ev='hp6 spe252 spa252', ab='NATURAL_CURE')
STARMIE_ATK = lambda: M('STARMIE', 'SURF ICE_BEAM THUNDERBOLT RECOVER', nat='TIMID', ev='hp6 spe252 spa252', ab='NATURAL_CURE')
MAGNETON_TRAP = lambda: M('MAGNETON', 'THUNDERBOLT HIDDEN_POWER TOXIC THUNDER_WAVE', nat='MODEST', ev='hp252 spe4 spa252', ab='MAGNET_PULL', item='LEFTOVERS')
MAGNETON_RAIN = lambda: M('MAGNETON', 'RAIN_DANCE THUNDER HIDDEN_POWER PROTECT', nat='MODEST', ev='hp252 spe4 spa252', ab='MAGNET_PULL')
AERO_LEAD = lambda: M('AERODACTYL', 'ROCK_SLIDE EARTHQUAKE DOUBLE_EDGE TAUNT', nat='JOLLY', ev='hp6 atk252 spe252', ab='ROCK_HEAD')
AERO_CB = lambda: M('AERODACTYL', 'ROCK_SLIDE EARTHQUAKE DOUBLE_EDGE HIDDEN_POWER', nat='JOLLY', ev='hp6 atk252 spe252', ab='ROCK_HEAD', item='CHOICE_BAND')
HERA_SD = lambda: M('HERACROSS', 'SWORDS_DANCE MEGAHORN ROCK_SLIDE BRICK_BREAK', nat='ADAMANT', ev='hp6 atk252 spe252', ab='GUTS', item='SALAC_BERRY')
HERA_CB = lambda: M('HERACROSS', 'MEGAHORN BRICK_BREAK ROCK_SLIDE FACADE', nat='ADAMANT', ev='hp6 atk252 spe252', ab='GUTS', item='CHOICE_BAND')
HERA_SUBPUNCH = lambda: M('HERACROSS', 'SUBSTITUTE FOCUS_PUNCH MEGAHORN ROCK_SLIDE', nat='ADAMANT', ev='hp6 atk252 spe252', ab='GUTS', item='LEFTOVERS')
HERA_ENDURE = lambda: M('HERACROSS', 'ENDURE REVERSAL MEGAHORN ROCK_SLIDE', nat='ADAMANT', ev='hp6 atk252 spe252', ab='SWARM', item='SALAC_BERRY')
BRELOOM_SUBPUNCH = lambda: M('BRELOOM', 'SPORE SUBSTITUTE FOCUS_PUNCH SKY_UPPERCUT', nat='ADAMANT', ev='hp6 atk252 spe252', ab='EFFECT_SPORE')
BRELOOM_SEED = lambda: M('BRELOOM', 'SPORE LEECH_SEED SUBSTITUTE FOCUS_PUNCH', nat='JOLLY', ev='hp6 atk252 spe252', ab='EFFECT_SPORE')
MACHAMP_SUBPUNCH = lambda: M('MACHAMP', 'SUBSTITUTE FOCUS_PUNCH CROSS_CHOP ROCK_SLIDE', nat='ADAMANT', ev='hp252 atk252 spe4', ab='GUTS')
MACHAMP_CB = lambda: M('MACHAMP', 'CROSS_CHOP ROCK_SLIDE EARTHQUAKE HIDDEN_POWER', nat='ADAMANT', ev='hp252 atk252 spe4', ab='GUTS', item='CHOICE_BAND')
GYARA_DD = lambda: M('GYARADOS', 'DRAGON_DANCE HIDDEN_POWER EARTHQUAKE DOUBLE_EDGE', nat='ADAMANT', ev='hp6 atk252 spe252', ab='INTIMIDATE')
GYARA_DD_TAUNT = lambda: M('GYARADOS', 'DRAGON_DANCE HIDDEN_POWER EARTHQUAKE TAUNT', nat='ADAMANT', ev='hp6 atk252 spe252', ab='INTIMIDATE', item='LUM_BERRY')
KINGDRA_RAIN = lambda: M('KINGDRA', 'RAIN_DANCE SURF ICE_BEAM HIDDEN_POWER', nat='MODEST', ev='hp6 spe252 spa252', ab='SWIFT_SWIM', item='LUM_BERRY')
KINGDRA_SUB = lambda: M('KINGDRA', 'SUBSTITUTE SURF ICE_BEAM HIDDEN_POWER', nat='MODEST', ev='hp6 spe252 spa252', ab='SWIFT_SWIM', item='PETAYA_BERRY')
LUDI_RAIN = lambda: M('LUDICOLO', 'RAIN_DANCE SURF ICE_BEAM GIGA_DRAIN', nat='MODEST', ev='hp6 spe252 spa252', ab='SWIFT_SWIM')
LUDI_SEED = lambda: M('LUDICOLO', 'LEECH_SEED SURF ICE_BEAM TOXIC', nat='CALM', ev='hp252 def92 spd164', ab='RAIN_DISH')
OMASTAR_RAIN = lambda: M('OMASTAR', 'RAIN_DANCE SURF ICE_BEAM HIDDEN_POWER', nat='MODEST', ev='hp6 spe252 spa252', ab='SWIFT_SWIM')
OMASTAR_SPIKES = lambda: M('OMASTAR', 'SPIKES SURF ICE_BEAM PROTECT', nat='BOLD', ev='hp252 def252 spa4', ab='SHELL_ARMOR')
KABUTOPS_RAIN = lambda: M('KABUTOPS', 'RAIN_DANCE SWORDS_DANCE SURF ROCK_SLIDE', nat='ADAMANT', ev='hp6 atk252 spe252', ab='SWIFT_SWIM', item='LUM_BERRY')
KABUTOPS_SD = lambda: M('KABUTOPS', 'SWORDS_DANCE ROCK_SLIDE BRICK_BREAK RAPID_SPIN', nat='ADAMANT', ev='hp6 atk252 spe252', ab='BATTLE_ARMOR', item='LUM_BERRY')
GOREBYSS_RAIN = lambda: M('GOREBYSS', 'RAIN_DANCE SURF ICE_BEAM HIDDEN_POWER', nat='MODEST', ev='hp6 spe252 spa252', ab='SWIFT_SWIM', item='LUM_BERRY')
QWILFISH_RAIN = lambda: M('QWILFISH', 'RAIN_DANCE SPIKES SURF DESTINY_BOND', nat='ADAMANT', ev='hp6 atk252 spe252', ab='SWIFT_SWIM')
EXEGG_SUN = lambda: M('EXEGGUTOR', 'SUNNY_DAY SOLAR_BEAM PSYCHIC HIDDEN_POWER', nat='MODEST', ev='hp6 spe252 spa252', ab='CHLOROPHYLL')
EXEGG_SUN_SLEEP = lambda: M('EXEGGUTOR', 'SLEEP_POWDER SOLAR_BEAM PSYCHIC EXPLOSION', nat='MILD', ev='atk6 spe252 spa252', ab='CHLOROPHYLL')
VICTREEBEL_SUN = lambda: M('VICTREEBEL', 'SUNNY_DAY SOLAR_BEAM SLUDGE_BOMB SLEEP_POWDER', nat='MODEST', ev='hp6 spe252 spa252', ab='CHLOROPHYLL')
VICTREEBEL_SD = lambda: M('VICTREEBEL', 'SWORDS_DANCE SLUDGE_BOMB HIDDEN_POWER SLEEP_POWDER', nat='ADAMANT', ev='hp6 atk252 spe252', ab='CHLOROPHYLL')
JUMPLUFF_SUN = lambda: M('JUMPLUFF', 'SUNNY_DAY SLEEP_POWDER LEECH_SEED SOLAR_BEAM', nat='TIMID', ev='hp252 spe252 spa4', ab='CHLOROPHYLL')
JUMPLUFF_SEED = lambda: M('JUMPLUFF', 'LEECH_SEED SLEEP_POWDER SUBSTITUTE HIDDEN_POWER', nat='TIMID', ev='hp252 spe252 spa4', ab='CHLOROPHYLL')
TANGELA_SUN = lambda: M('TANGELA', 'SUNNY_DAY SOLAR_BEAM SLEEP_POWDER HIDDEN_POWER', nat='MODEST', ev='hp6 spe252 spa252', ab='CHLOROPHYLL')
SHIFTRY_SUN = lambda: M('SHIFTRY', 'SUNNY_DAY SOLAR_BEAM EXPLOSION BRICK_BREAK', nat='LONELY', ev='atk252 spe252 spa6', ab='CHLOROPHYLL')
CHARIZARD_SUN = lambda: M('CHARIZARD', 'SUNNY_DAY FIRE_BLAST HIDDEN_POWER DRAGON_CLAW', nat='MODEST', ev='hp6 spe252 spa252', ab='BLAZE', item='LEFTOVERS')
CHARIZARD_BD = lambda: M('CHARIZARD', 'BELLY_DRUM EARTHQUAKE ROCK_SLIDE AERIAL_ACE', nat='JOLLY', ev='hp6 atk252 spe252', ab='BLAZE', item='SALAC_BERRY')
CHARIZARD_SUB = lambda: M('CHARIZARD', 'SUBSTITUTE FIRE_BLAST HIDDEN_POWER DRAGON_CLAW', nat='MODEST', ev='hp6 spe252 spa252', ab='BLAZE', item='PETAYA_BERRY')
HOUNDOOM_SUN = lambda: M('HOUNDOOM', 'SUNNY_DAY FIRE_BLAST CRUNCH HIDDEN_POWER', nat='TIMID', ev='hp6 spe252 spa252', ab='FLASH_FIRE', item='LEFTOVERS')
NINETALES_SUN = lambda: M('NINETALES', 'SUNNY_DAY FLAMETHROWER HIDDEN_POWER WILL_O_WISP', nat='TIMID', ev='hp6 spe252 spa252', ab='FLASH_FIRE')
MOLTRES_SUN = lambda: M('MOLTRES', 'SUNNY_DAY FIRE_BLAST HIDDEN_POWER AGILITY', nat='MODEST', ev='hp6 spe252 spa252', ab='PRESSURE')
MOLTRES_PP = lambda: M('MOLTRES', 'SUBSTITUTE PROTECT TOXIC FLAMETHROWER', nat='TIMID', ev='hp252 spe252 spa4', ab='PRESSURE')
CACTURNE_SAND = lambda: M('CACTURNE', 'SUBSTITUTE LEECH_SEED FOCUS_PUNCH NEEDLE_ARM', nat='ADAMANT', ev='hp6 atk252 spe252', ab='SAND_VEIL')
CACTURNE_SPIKES = lambda: M('CACTURNE', 'SPIKES LEECH_SEED DESTINY_BOND NEEDLE_ARM', nat='JOLLY', ev='hp252 atk6 spe252', ab='SAND_VEIL', item='LEFTOVERS')
SANDSLASH_SAND = lambda: M('SANDSLASH', 'SWORDS_DANCE EARTHQUAKE ROCK_SLIDE RAPID_SPIN', nat='ADAMANT', ev='hp252 atk252 spe4', ab='SAND_VEIL')
DUGTRIO_TRAP = lambda: M('DUGTRIO', 'EARTHQUAKE ROCK_SLIDE AERIAL_ACE HIDDEN_POWER', nat='JOLLY', ev='hp6 atk252 spe252', ab='ARENA_TRAP', item='CHOICE_BAND')
DUGTRIO_SAND = lambda: M('DUGTRIO', 'SUBSTITUTE EARTHQUAKE ROCK_SLIDE AERIAL_ACE', nat='JOLLY', ev='hp6 atk252 spe252', ab='SAND_VEIL')
GLIGAR_SAND = lambda: M('GLIGAR', 'SWORDS_DANCE EARTHQUAKE HIDDEN_POWER PROTECT', nat='ADAMANT', ev='hp6 atk252 spe252', ab='SAND_VEIL')
GLIGAR_TOXIC = lambda: M('GLIGAR', 'TOXIC PROTECT EARTHQUAKE HIDDEN_POWER', nat='IMPISH', ev='hp252 def252 spe4', ab='SAND_VEIL')
REGICE_STD = lambda: M('REGICE', 'ICE_BEAM THUNDERBOLT REST EXPLOSION', nat='MODEST', ev='hp252 spa252 spd4', ab='CLEAR_BODY')
REGICE_TWAVE = lambda: M('REGICE', 'ICE_BEAM THUNDER_WAVE REST TOXIC', nat='CALM', ev='hp252 def92 spd164', ab='CLEAR_BODY')
REGIROCK_CURSE = lambda: M('REGIROCK', 'CURSE ROCK_SLIDE EARTHQUAKE EXPLOSION', nat='ADAMANT', ev='hp252 atk252 spd4', ab='CLEAR_BODY')
REGISTEEL_CURSE = lambda: M('REGISTEEL', 'CURSE REST SEISMIC_TOSS TOXIC', nat='CAREFUL', ev='hp252 def92 spd164', ab='CLEAR_BODY')
REGISTEEL_WALL = lambda: M('REGISTEEL', 'SEISMIC_TOSS TOXIC REST PROTECT', nat='CAREFUL', ev='hp252 def92 spd164', ab='CLEAR_BODY')
UMBREON_WISH = lambda: M('UMBREON', 'WISH PROTECT TOXIC TAUNT', nat='CALM', ev='hp252 def92 spd164', ab='SYNCHRONIZE')
UMBREON_BP = lambda: M('UMBREON', 'MEAN_LOOK BATON_PASS WISH TOXIC', nat='CALM', ev='hp252 def92 spd164', ab='SYNCHRONIZE')
UMBREON_CURSE = lambda: M('UMBREON', 'CURSE REST BODY_SLAM TOXIC', nat='CAREFUL', ev='hp252 def92 spd164', ab='SYNCHRONIZE')
VAPOREON_WISH = lambda: M('VAPOREON', 'WISH PROTECT SURF ICE_BEAM', nat='BOLD', ev='hp188 def252 spa68', ab='WATER_ABSORB')
VAPOREON_BP = lambda: M('VAPOREON', 'SUBSTITUTE ACID_ARMOR BATON_PASS SURF', nat='BOLD', ev='hp188 def252 spe68', ab='WATER_ABSORB')
VAPOREON_WISHBP = lambda: M('VAPOREON', 'WISH BATON_PASS SUBSTITUTE SURF', nat='BOLD', ev='hp188 def252 spe68', ab='WATER_ABSORB')
JOLTEON_BP = lambda: M('JOLTEON', 'AGILITY SUBSTITUTE BATON_PASS THUNDERBOLT', nat='TIMID', ev='hp6 spe252 spa252', ab='VOLT_ABSORB')
JOLTEON_SUB = lambda: M('JOLTEON', 'SUBSTITUTE THUNDERBOLT HIDDEN_POWER BATON_PASS', nat='TIMID', ev='hp6 spe252 spa252', ab='VOLT_ABSORB')
JOLTEON_WISH = lambda: M('JOLTEON', 'WISH THUNDERBOLT HIDDEN_POWER BATON_PASS', nat='TIMID', ev='hp6 spe252 spa252', ab='VOLT_ABSORB')
JOLTEON_TWAVE = lambda: M('JOLTEON', 'THUNDER_WAVE THUNDERBOLT HIDDEN_POWER SUBSTITUTE', nat='TIMID', ev='hp6 spe252 spa252', ab='VOLT_ABSORB')
ESPEON_BP = lambda: M('ESPEON', 'CALM_MIND SUBSTITUTE BATON_PASS PSYCHIC', nat='TIMID', ev='hp6 spe252 spa252', ab='SYNCHRONIZE')
ESPEON_SCREENS = lambda: M('ESPEON', 'REFLECT LIGHT_SCREEN PSYCHIC MORNING_SUN', nat='TIMID', ev='hp252 spe252 spa4', ab='SYNCHRONIZE')
NINJASK_BP = lambda: M('NINJASK', 'SWORDS_DANCE SUBSTITUTE BATON_PASS PROTECT', nat='JOLLY', ev='hp252 atk6 spe252', ab='SPEED_BOOST')
NINJASK_BP_AA = lambda: M('NINJASK', 'SWORDS_DANCE BATON_PASS PROTECT AERIAL_ACE', nat='JOLLY', ev='hp252 atk6 spe252', ab='SPEED_BOOST')
SCIZOR_BP = lambda: M('SCIZOR', 'SWORDS_DANCE AGILITY BATON_PASS HIDDEN_POWER', nat='ADAMANT', ev='hp252 atk252 spe6', ab='SWARM')
SCIZOR_SD = lambda: M('SCIZOR', 'SWORDS_DANCE HIDDEN_POWER SILVER_WIND AGILITY', nat='ADAMANT', ev='hp6 atk252 spe252', ab='SWARM', item='LIECHI_BERRY')
MIME_BP = lambda: M('MR_MIME', 'CALM_MIND SUBSTITUTE BATON_PASS PSYCHIC', nat='TIMID', ev='hp252 spe252 spa6', ab='SOUNDPROOF')
MEDICHAM_BP = lambda: M('MEDICHAM', 'CALM_MIND BATON_PASS PSYCHIC RECOVER', nat='TIMID', ev='hp252 spe252 spa6', ab='PURE_POWER')
MEDICHAM_CB = lambda: M('MEDICHAM', 'HI_JUMP_KICK SHADOW_BALL ROCK_SLIDE FAKE_OUT', nat='ADAMANT', ev='hp6 atk252 spe252', ab='PURE_POWER', item='CHOICE_BAND')
ARIADOS_WEB = lambda: M('ARIADOS', 'SPIDER_WEB BATON_PASS AGILITY SLUDGE_BOMB', nat='ADAMANT', ev='hp252 atk252 spe6', ab='INSOMNIA')
SMEARGLE_BP = lambda: M('SMEARGLE', 'SPORE BELLY_DRUM BATON_PASS SUBSTITUTE', nat='JOLLY', ev='hp252 def6 spe252', ab='OWN_TEMPO', item='SALAC_BERRY')
SMEARGLE_SPIKES = lambda: M('SMEARGLE', 'SPORE SPIKES EXPLOSION AGILITY', nat='JOLLY', ev='hp252 def6 spe252', ab='OWN_TEMPO', item='SALAC_BERRY')
MAROWAK_SD = lambda: M('MAROWAK', 'SWORDS_DANCE BONEMERANG ROCK_SLIDE DOUBLE_EDGE', nat='ADAMANT', ev='hp252 atk252 spe6', ab='ROCK_HEAD', item='THICK_CLUB')
MAROWAK_PERISH = lambda: M('MAROWAK', 'PERISH_SONG BONEMERANG ROCK_SLIDE PROTECT', nat='ADAMANT', ev='hp252 atk252 spe6', ab='ROCK_HEAD', item='THICK_CLUB')
ABSOL_SD = lambda: M('ABSOL', 'SWORDS_DANCE SHADOW_BALL HIDDEN_POWER BATON_PASS', nat='ADAMANT', ev='hp6 atk252 spe252', ab='PRESSURE', item='LUM_BERRY')
ABSOL_MC = lambda: M('ABSOL', 'MAGIC_COAT SHADOW_BALL HIDDEN_POWER TAUNT', nat='JOLLY', ev='hp6 atk252 spe252', ab='PRESSURE', item='LUM_BERRY')
SCEPTILE_SD = lambda: M('SCEPTILE', 'SWORDS_DANCE LEAF_BLADE EARTHQUAKE HIDDEN_POWER', nat='JOLLY', ev='hp6 atk252 spe252', ab='OVERGROW', item='SALAC_BERRY')
SCEPTILE_SUB = lambda: M('SCEPTILE', 'SUBSTITUTE LEECH_SEED LEAF_BLADE HIDDEN_POWER', nat='TIMID', ev='hp6 spe252 spa252', ab='OVERGROW', item='PETAYA_BERRY')
BLAZIKEN_SD = lambda: M('BLAZIKEN', 'SWORDS_DANCE SKY_UPPERCUT ROCK_SLIDE FIRE_BLAST', nat='NAUGHTY', ev='atk252 spe252 spa6', ab='BLAZE', item='SALAC_BERRY')
BLAZIKEN_ENDURE = lambda: M('BLAZIKEN', 'ENDURE REVERSAL FLAMETHROWER ROCK_SLIDE', nat='NAUGHTY', ev='atk252 spe252 spa6', ab='BLAZE', item='SALAC_BERRY')
BLAZIKEN_SUBPUNCH = lambda: M('BLAZIKEN', 'SUBSTITUTE FOCUS_PUNCH FIRE_BLAST HIDDEN_POWER', nat='NAUGHTY', ev='atk252 spe252 spa6', ab='BLAZE', item='LEFTOVERS')
DODRIO_CB = lambda: M('DODRIO', 'DRILL_PECK RETURN QUICK_ATTACK HIDDEN_POWER', nat='JOLLY', ev='hp6 atk252 spe252', ab='EARLY_BIRD', item='CHOICE_BAND')
SLAKING_CB = lambda: M('SLAKING', 'RETURN EARTHQUAKE SHADOW_BALL FOCUS_PUNCH', nat='ADAMANT', ev='hp6 atk252 spe252', ab='TRUANT', item='CHOICE_BAND')
URSARING_CB = lambda: M('URSARING', 'RETURN EARTHQUAKE FOCUS_PUNCH HIDDEN_POWER', nat='ADAMANT', ev='hp6 atk252 spe252', ab='GUTS', item='CHOICE_BAND')
URSARING_TALK = lambda: M('URSARING', 'REST SLEEP_TALK RETURN EARTHQUAKE', nat='ADAMANT', ev='hp252 atk252 spe6', ab='GUTS')
MILTANK_TALK = lambda: M('MILTANK', 'REST SLEEP_TALK BODY_SLAM CURSE', nat='CAREFUL', ev='hp252 def6 spd252', ab='THICK_FAT')
MILTANK_HEAL = lambda: M('MILTANK', 'MILK_DRINK HEAL_BELL BODY_SLAM CURSE', nat='CAREFUL', ev='hp252 def6 spd252', ab='THICK_FAT')
LAPRAS_TALK = lambda: M('LAPRAS', 'REST SLEEP_TALK SURF ICE_BEAM', nat='CALM', ev='hp252 def92 spd164', ab='WATER_ABSORB')
LAPRAS_PERISH = lambda: M('LAPRAS', 'PERISH_SONG SURF ICE_BEAM REST', nat='CALM', ev='hp252 def92 spd164', ab='WATER_ABSORB')
SLOWBRO_CM = lambda: M('SLOWBRO', 'CALM_MIND SURF PSYCHIC REST', nat='BOLD', ev='hp252 def252 spd6', ab='OWN_TEMPO')
SLOWBRO_TALK = lambda: M('SLOWBRO', 'REST SLEEP_TALK SURF PSYCHIC', nat='BOLD', ev='hp252 def252 spd6', ab='OWN_TEMPO')
SLOWBRO_CURSE = lambda: M('SLOWBRO', 'CURSE REST SLEEP_TALK EARTHQUAKE', nat='CAREFUL', ev='hp252 def6 spd252', ab='OWN_TEMPO')
WOBB_STD = lambda: M('WOBBUFFET', 'COUNTER MIRROR_COAT ENCORE SAFEGUARD', nat='CALM', ev='hp28 def228 spd252', ab='SHADOW_TAG')
WOBB_DBOND = lambda: M('WOBBUFFET', 'COUNTER MIRROR_COAT ENCORE DESTINY_BOND', nat='CALM', ev='hp28 def228 spd252', ab='SHADOW_TAG')
WYNAUT_STD = lambda: M('WYNAUT', 'COUNTER MIRROR_COAT ENCORE DESTINY_BOND', nat='CALM', ev='hp28 def228 spd252', ab='SHADOW_TAG')
WEEZING_WALL = lambda: M('WEEZING', 'WILL_O_WISP SLUDGE_BOMB PAIN_SPLIT HAZE', nat='BOLD', ev='hp252 def252 spa6', ab='LEVITATE')
WEEZING_BOOM = lambda: M('WEEZING', 'WILL_O_WISP SLUDGE_BOMB FIRE_BLAST EXPLOSION', nat='BOLD', ev='hp252 def252 spa6', ab='LEVITATE')
WEEZING_DBOND = lambda: M('WEEZING', 'WILL_O_WISP SLUDGE_BOMB DESTINY_BOND HAZE', nat='BOLD', ev='hp252 def252 spa6', ab='LEVITATE')
GOLEM_BOOM = lambda: M('GOLEM', 'EARTHQUAKE ROCK_SLIDE EXPLOSION COUNTER', nat='ADAMANT', ev='hp252 atk252 def6', ab='STURDY')
ELECTRODE_BOOM = lambda: M('ELECTRODE', 'THUNDERBOLT HIDDEN_POWER TAUNT EXPLOSION', nat='HASTY', ev='atk6 spe252 spa252', ab='SOUNDPROOF')
SOLROCK_BOOM = lambda: M('SOLROCK', 'ROCK_SLIDE EARTHQUAKE REFLECT EXPLOSION', nat='ADAMANT', ev='hp252 atk252 def6', ab='LEVITATE')
LUNATONE_BOOM = lambda: M('LUNATONE', 'CALM_MIND PSYCHIC ICE_BEAM EXPLOSION', nat='MODEST', ev='hp252 spa252 spd6', ab='LEVITATE')
GLALIE_SPIKES = lambda: M('GLALIE', 'SPIKES ICE_BEAM EARTHQUAKE EXPLOSION', nat='NAUGHTY', ev='hp6 atk252 spe252', ab='INNER_FOCUS')
CAMERUPT_BOOM = lambda: M('CAMERUPT', 'EARTHQUAKE OVERHEAT ROCK_SLIDE EXPLOSION', nat='BRAVE', ev='hp252 atk252 spa6', ab='MAGMA_ARMOR')
STEELIX_CURSE = lambda: M('RHYDON', 'CURSE EARTHQUAKE ROCK_SLIDE REST', nat='CAREFUL', ev='hp252 atk6 spd252', ab='ROCK_HEAD')
STEELIX_ROAR = lambda: M('STEELIX', 'EARTHQUAKE ROCK_SLIDE ROAR EXPLOSION', nat='IMPISH', ev='hp252 atk6 spd252', ab='STURDY')
MUK_CURSE = lambda: M('MUK', 'CURSE SLUDGE_BOMB REST HIDDEN_POWER', nat='CAREFUL', ev='hp252 atk6 spd252', ab='STICKY_HOLD')
MUK_BOOM = lambda: M('MUK', 'SLUDGE_BOMB FIRE_BLAST HIDDEN_POWER EXPLOSION', nat='ADAMANT', ev='hp252 atk252 spd6', ab='STICKY_HOLD')
ALAKAZAM_CM = lambda: M('ALAKAZAM', 'CALM_MIND PSYCHIC FIRE_PUNCH RECOVER', nat='TIMID', ev='hp6 spe252 spa252', ab='SYNCHRONIZE')
ALAKAZAM_TRICK = lambda: M('ALAKAZAM', 'TRICK PSYCHIC FIRE_PUNCH ICE_PUNCH', nat='TIMID', ev='hp6 spe252 spa252', ab='SYNCHRONIZE', item='CHOICE_BAND')
ALAKAZAM_ENCORE = lambda: M('ALAKAZAM', 'ENCORE PSYCHIC FIRE_PUNCH RECOVER', nat='TIMID', ev='hp6 spe252 spa252', ab='SYNCHRONIZE')
ALAKAZAM_COUNTER = lambda: M('ALAKAZAM', 'COUNTER PSYCHIC FIRE_PUNCH RECOVER', nat='TIMID', ev='hp6 spe252 spa252', ab='INNER_FOCUS')
ALAKAZAM_SCREENS = lambda: M('ALAKAZAM', 'REFLECT LIGHT_SCREEN PSYCHIC RECOVER', nat='TIMID', ev='hp252 spe252 spa6', ab='SYNCHRONIZE')
ALAKAZAM_KNOCK = lambda: M('ALAKAZAM', 'KNOCK_OFF PSYCHIC FIRE_PUNCH RECOVER', nat='TIMID', ev='hp6 spe252 spa252', ab='SYNCHRONIZE')
GARDEVOIR_CM = lambda: M('GARDEVOIR', 'CALM_MIND PSYCHIC THUNDERBOLT WILL_O_WISP', nat='TIMID', ev='hp6 spe252 spa252', ab='TRACE')
GARDEVOIR_SWAP = lambda: M('GARDEVOIR', 'SKILL_SWAP PSYCHIC THUNDERBOLT PROTECT', nat='TIMID', ev='hp6 spe252 spa252', ab='TRACE')
GARDEVOIR_DBOND = lambda: M('GARDEVOIR', 'DESTINY_BOND PSYCHIC WILL_O_WISP MEAN_LOOK', nat='TIMID', ev='hp6 spe252 spa252', ab='SYNCHRONIZE')
RAIKOU_CM = lambda: M('RAIKOU', 'CALM_MIND THUNDERBOLT HIDDEN_POWER REST', nat='TIMID', ev='hp6 spe252 spa252', ab='PRESSURE')
RAIKOU_SUB = lambda: M('RAIKOU', 'SUBSTITUTE CALM_MIND THUNDERBOLT HIDDEN_POWER', nat='TIMID', ev='hp6 spe252 spa252', ab='PRESSURE', item='LEFTOVERS')
LATIOS_CM = lambda: M('LATIOS', 'CALM_MIND DRAGON_CLAW THUNDERBOLT RECOVER', nat='TIMID', ev='hp6 spe252 spa252', ab='LEVITATE', item='SOUL_DEW')
LATIAS_CM = lambda: M('LATIAS', 'CALM_MIND DRAGON_CLAW REFRESH RECOVER', nat='TIMID', ev='hp252 spe252 spa6', ab='LEVITATE', item='SOUL_DEW')
LATIAS_WISH = lambda: M('LATIAS', 'WISH DRAGON_CLAW TOXIC PROTECT', nat='TIMID', ev='hp252 spe252 spd6', ab='LEVITATE', item='LEFTOVERS')
MEWTWO_CM = lambda: M('MEWTWO', 'CALM_MIND PSYCHIC ICE_BEAM RECOVER', nat='TIMID', ev='hp6 spe252 spa252', ab='PRESSURE')
MEWTWO_BOLT = lambda: M('MEWTWO', 'CALM_MIND PSYCHIC THUNDERBOLT FLAMETHROWER', nat='TIMID', ev='hp6 spe252 spa252', ab='PRESSURE')
MEWTWO_TAUNT = lambda: M('MEWTWO', 'TAUNT CALM_MIND PSYCHIC RECOVER', nat='TIMID', ev='hp6 spe252 spa252', ab='PRESSURE')
LUGIA_WALL = lambda: M('LUGIA', 'AEROBLAST TOXIC RECOVER WHIRLWIND', nat='BOLD', ev='hp252 def252 spe6', ab='PRESSURE')
LUGIA_CM = lambda: M('LUGIA', 'CALM_MIND AEROBLAST ICE_BEAM RECOVER', nat='TIMID', ev='hp252 spe252 spa6', ab='PRESSURE')
LUGIA_PP = lambda: M('LUGIA', 'TOXIC RECOVER PROTECT SUBSTITUTE', nat='BOLD', ev='hp252 def252 spe6', ab='PRESSURE')
HOOH_WALL = lambda: M('HO_OH', 'SACRED_FIRE RECOVER TOXIC WHIRLWIND', nat='CALM', ev='hp252 def6 spd252', ab='PRESSURE')
HOOH_CM = lambda: M('HO_OH', 'CALM_MIND SACRED_FIRE THUNDERBOLT RECOVER', nat='MODEST', ev='hp252 spa252 spd6', ab='PRESSURE')
KYOGRE_CM = lambda: M('KYOGRE', 'CALM_MIND SURF ICE_BEAM THUNDER', nat='MODEST', ev='hp252 spa252 spd6', ab='DRIZZLE')
KYOGRE_REST = lambda: M('KYOGRE', 'SURF ICE_BEAM THUNDER REST', nat='MODEST', ev='hp252 spa252 spd6', ab='DRIZZLE', item='CHESTO_BERRY')
KYOGRE_SPOUT = lambda: M('KYOGRE', 'WATER_SPOUT SURF THUNDER ICE_BEAM', nat='MODEST', ev='hp6 spe252 spa252', ab='DRIZZLE')
GROUDON_SD = lambda: M('GROUDON', 'SWORDS_DANCE EARTHQUAKE ROCK_SLIDE OVERHEAT', nat='ADAMANT', ev='hp252 atk252 spe6', ab='DROUGHT')
GROUDON_BULK = lambda: M('GROUDON', 'BULK_UP EARTHQUAKE ROCK_SLIDE REST', nat='ADAMANT', ev='hp252 atk252 spd6', ab='DROUGHT')
GROUDON_ERUPT = lambda: M('GROUDON', 'ERUPTION EARTHQUAKE ROCK_SLIDE SOLAR_BEAM', nat='NAUGHTY', ev='atk252 spe252 spa6', ab='DROUGHT')
RAYQUAZA_DD = lambda: M('RAYQUAZA', 'DRAGON_DANCE EARTHQUAKE ROCK_SLIDE EXTREME_SPEED', nat='ADAMANT', ev='hp6 atk252 spe252', ab='AIR_LOCK', item='LUM_BERRY')
RAYQUAZA_MIX = lambda: M('RAYQUAZA', 'EXTREME_SPEED EARTHQUAKE OVERHEAT ROCK_SLIDE', nat='LONELY', ev='atk252 spe252 spa6', ab='AIR_LOCK', item='CHOICE_BAND')
DEOXYS_ATK = lambda: M('DEOXYS', 'PSYCHO_BOOST SUPERPOWER SHADOW_BALL ICE_BEAM', nat='NAIVE', ev='atk6 spe252 spa252', ab='PRESSURE', fateful=True)
DEOXYS_LEAD = lambda: M('DEOXYS', 'TAUNT COUNTER PSYCHO_BOOST SUPERPOWER', nat='NAIVE', ev='atk6 spe252 spa252', ab='PRESSURE', fateful=True)
MEW_SD = lambda: M('MEW', 'SWORDS_DANCE EARTHQUAKE ROCK_SLIDE EXPLOSION', nat='JOLLY', ev='hp6 atk252 spe252', ab='SYNCHRONIZE', fateful=True)
MEW_BOOM = lambda: M('MEW', 'PSYCHIC THUNDERBOLT SOFT_BOILED EXPLOSION', nat='MILD', ev='atk6 spe252 spa252', ab='SYNCHRONIZE', fateful=True)

# ---------------------------------------------------------------------------------------------------
# Endgame singles teams (level 100)
# ---------------------------------------------------------------------------------------------------
# full stall -----------------------------------------------------------------------------------------
T('full_stall', [SKARM_SPIKES(), BLISS_TOSS(), GENGAR_SPINBLOCK(), DUSCLOPS_WALL(), MILOTIC_STALL(), CLAYDOL_SPIN()],
  'Lead Skarmory, stack Spikes and Whirlwind attackers around; Gengar blocks Rapid Spin, Blissey and Dusclops absorb hits and spread Toxic/burn. Win by residual damage; never leave a wall in on its counter.', tier='ou')
T('full_stall', [SKARM_TOXIC(), BLISS_WALL(), WEEZING_WALL(), DUSCLOPS_REST(), SUICUNE_ROAR(), FORRY_HP()],
  'Lead Forretress for Spikes; Weezing and Skarmory wall physical, Blissey special; Suicune Roars through boosts. Win condition: Toxic plus Spikes attrition while the team out-heals everything.', tier='ou')
T('full_stall', [SKARM_ROAR(), BLISS_BELL(), MILOTIC_MC(), CLAYDOL_SCREENS(), UMBREON_WISH(), GENGAR_SPINBLOCK()],
  'Lead Skarmory and Spike early; Umbreon Wish-Protect and Taunt shut down opposing stall; Milotic Mirror Coats special attackers. Win by PP and Toxic attrition with Blissey Heal Bell keeping walls healthy.', tier='ou')
T('full_stall', [REGISTEEL_WALL(), BLISS_TOSS(), DUSCLOPS_WALL(), MILOTIC_STALL(), SWAMP_ROAR(), SKARM_SPIKES()],
  'Lead Swampert, phaze early to spread Spikes damage; Registeel and Blissey split the special hits, Dusclops burns physical attackers. Win condition: opponent runs out of healthy attackers before the walls run out of PP.', tier='ou')

# spikes stacking + phazing --------------------------------------------------------------------------
T('spikes_phaze', [SKARM_SPIKES(), SWAMP_ROAR(), GENGAR_SPINBLOCK(), TTAR_DD(), BLISS_BELL(), SUICUNE_ROAR()],
  'Lead Skarmory, get three layers of Spikes, then Whirlwind/Roar with Skarmory, Swampert and Suicune to rack up entry damage. Gengar blocks Rapid Spin; Tyranitar cleans once everything is chipped.', tier='ou')
T('spikes_phaze', [FORRY_SPIKES(), SUICUNE_ROAR(), GENGAR_HYPNO(), ZAPDOS_STD(), SNORLAX_CURSE(), MENCE_DD()],
  'Lead Forretress and lay Spikes; Suicune Roars to spread Spikes damage and Gengar keeps them on the field. Salamence Dragon Dances late to sweep a chipped team.', tier='ou')
T('spikes_phaze', [CLOYSTER_SPIKES(), SKARM_ROAR(), STEELIX_ROAR(), GENGAR_SPINBLOCK(), BLISS_TOSS(), AERO_LEAD()],
  'Lead Cloyster for Spikes, then rotate three phazers (Skarmory, Steelix, Cloyster Explosion as last resort). Aerodactyl Taunts opposing spinners and revenge kills; win on entry damage.', tier='ou')
T('spikes_phaze', [SMEARGLE_SPIKES(), SKARM_ROAR(), SWAMP_ROAR(), GENGAR_SPINBLOCK(), MILOTIC_STALL(), TTAR_DD()],
  'Smeargle leads: Spore the lead, then Spikes, then Explode. Two Roar phazers cycle the opponent through Spikes; Tyranitar sweeps at the end. Smeargle moves are Sketched (the checker accepts any move on Smeargle).', tier='ou', tags=['smeargle_sketch'])

# skarmbliss ------------------------------------------------------------------------------------------
T('skarmbliss', [SKARM_SPIKES(), BLISS_TOSS(), STARMIE_SPIN(), TTAR_DD(), MENCE_CB(), SWAMP_MIXED()],
  'Classic Skarmbliss core: Skarmory takes physical hits and Spikes, Blissey takes special hits and heals. Lead Tyranitar to set sand; win when the two walls have worn everything down for a Salamence finish.', tier='ou')
T('skarmbliss', [SKARM_TOXIC(), BLISS_CM(), MAGNETON_TRAP(), CELEBI_CM(), AERO_LEAD(), SWAMP_CURSE()],
  'Skarmory + Blissey wall both sides; Magneton traps enemy Skarmory so Blissey can Calm Mind up. Lead Aerodactyl to Taunt; win when Calm Mind Blissey or Celebi sweeps late.', tier='ou')
T('skarmbliss', [SKARM_ROAR(), BLISS_BELL(), CLAYDOL_SPIN(), JIRACHI_CM(), GYARA_DD(), DUGTRIO_TRAP()],
  'Skarmbliss defensive core with Dugtrio to trap and kill the Tyranitar/Magneton that break it. Lead Jirachi; Gyarados sweeps once Zapdos and Blissey are worn down.', tier='ou')

# curselax ---------------------------------------------------------------------------------------------
T('curselax', [SNORLAX_CURSE(), SKARM_SPIKES(), MAGNETON_TRAP(), SUICUNE_CM(), CLAYDOL_SPIN(), TTAR_PURSUIT()],
  'Snorlax is the win condition: Curse up on special attackers, Rest off damage, Body Slam/Shadow Ball everything. Magneton removes Skarmory and Tyranitar Pursuits the Gengar that would Will-O-Wisp it.', tier='ou')
T('curselax', [SNORLAX_CURSE_EQ(), DUGTRIO_TRAP(), ZAPDOS_STD(), SWAMP_MIXED(), GENGAR_ATK(), FORRY_SPIKES()],
  'Lead Forretress for Spikes; Dugtrio traps opposing Snorlax/Blissey. Snorlax Curses when the fighting types are gone and sweeps with Return/Earthquake, Resting behind Zapdos support.', tier='ou')
T('curselax', [SNORLAX_CURSE(), SNORLAX_CB(), SKARM_ROAR(), BLISS_TOSS(), MAGNETON_TRAP(), MENCE_DD()],
  'Two Snorlax: the Choice Band one breaks walls early, the Curse one wins late. Skarmory/Blissey back it; Magneton kills Skarmory first so Curselax can set up freely.', tier='ou')

# curse-rest tanks --------------------------------------------------------------------------------------
T('curse_rest_tank', [SWAMP_CURSE(), REGISTEEL_CURSE(), MILTANK_TALK(), CELEBI_SEED(), MAGNETON_TRAP(), MOLTRES_SUN()],
  'Slow Curse+Rest tanks: Swampert and Registeel Curse up on physical attackers and Rest. Celebi Leech Seeds and Heal Bells; win by boosting to +6 on something that cannot break you and Resting through.', tier='ou')
T('curse_rest_tank', [STEELIX_CURSE(), SLOWBRO_CURSE(), MUK_CURSE(), BLISS_BELL(), CLAYDOL_SPIN(), ZAPDOS_STD()],
  'Lead Zapdos; Steelix, Slowbro and Muk each Curse+Rest on the things they wall. Blissey Heal Bells sleep; win by an unbreakable +6 tank once special threats are gone.', tier='ou')
T('curse_rest_tank', [REGIROCK_CURSE(), UMBREON_CURSE(), SNORLAX_TALK(), SKARM_SPIKES(), SUICUNE_CROC(), DUGTRIO_TRAP()],
  'Three Curse tanks (Regirock, Umbreon, Snorlax) with Rest/Sleep Talk; Skarmory Spikes; Dugtrio traps the Machamp/Heracross that would break them. Boost only when their fighting type is gone.', tier='ou')

# baton pass chains ----------------------------------------------------------------------------------------
T('baton_pass', [NINJASK_BP(), VAPOREON_BP(), JOLTEON_BP(), CELEBI_BP(), MIME_BP(), META_STD()],
  'Full Baton Pass chain. Ninjask leads: Protect for Speed Boost, Swords Dance, Substitute, pass. Vaporeon passes Acid Armor, Celebi/Mr. Mime pass Calm Mind, Metagross receives everything and sweeps.', tier='ou')
T('baton_pass', [NINJASK_BP_AA(), SCIZOR_BP(), HERA_SD(), UMBREON_BP(), ZAPDOS_AGI(), SWAMP_MIXED()],
  'Ninjask leads and passes Speed Boosts plus Swords Dance to Heracross or Scizor; Umbreon Mean Look + Baton Pass traps a wall for the receiver. Win when Heracross or Scizor sweeps at +2/+2.', tier='ou')
T('baton_pass', [JOLTEON_SUB(), VAPOREON_WISHBP(), ESPEON_BP(), RAIKOU_SUB(), GENGAR_ATK(), TTAR_DD()],
  'Eeveelution pass: Jolteon leads, Substitutes, passes the Sub; Vaporeon passes Wish+Sub; Espeon passes Calm Mind into Raikou. Win by a Raikou or Tyranitar sweep behind a passed Substitute.', tier='ou')
T('baton_pass', [SMEARGLE_BP(), MEDICHAM_BP(), SNORLAX_BD(), ZAPDOS_STD(), SWAMP_CURSE(), META_AGI()],
  'Smeargle leads: Spore, Belly Drum, Substitute, Baton Pass to Snorlax or Metagross. Medicham passes Calm Mind as backup. Smeargle moves are Sketched (the checker accepts any move on Smeargle).', tier='ou', tags=['smeargle_sketch'])
T('baton_pass', [CELEBI_BP(), JIRACHI_CM(), ABSOL_SD(), ARIADOS_WEB(), SUICUNE_CM(), MAGNETON_TRAP()],
  'Ariados Spider Webs a wall then Baton Passes the trap to a setup sweeper; Celebi passes Calm Mind to Jirachi. Win by trapping the one thing that walls your sweeper and boosting on it.', tier='ou')
T('baton_pass', [SCIZOR_BP(), NINJASK_BP(), SNORLAX_CURSE(), MENCE_DD_AA(), STARMIE_SPIN(), TTAR_DD()],
  'Ninjask/Scizor both pass Swords Dance and Agility; the receivers are Salamence, Tyranitar and Snorlax. Lead Ninjask, pass on the first switch; keep Starmie to spin Spikes for the chain.', tier='ou')

# substitute + focus punch -------------------------------------------------------------------------------
T('sub_punch', [BRELOOM_SUBPUNCH(), TTAR_TYRANIBOAH(), SNORLAX_SUBPUNCH(), HERA_SUBPUNCH(), GENGAR_SPINBLOCK(), SKARM_SPIKES()],
  'Substitute + Focus Punch everywhere. Breloom leads and Spores, then Sub-Punches; Tyranitar (Tyraniboah) Subs on Blissey and Focus Punches it. Win by punching through the special walls behind a Sub.', tier='ou')
T('sub_punch', [MACHAMP_SUBPUNCH(), BLAZIKEN_SUBPUNCH(), CACTURNE_SAND(), TTAR_DD(), ZAPDOS_SUB(), SWAMP_MIXED()],
  'Tyranitar sets sand so Cacturne Subs behind Sand Veil and Leech Seeds while Focus Punching. Machamp/Blaziken Sub on predicted switches and Punch; win by breaking Blissey/Snorlax for a Tyranitar sweep.', tier='ou')
T('sub_punch', [SNORLAX_SUBPUNCH(), SLAKING_CB(), BRELOOM_SEED(), GENGAR_HYPNO(), MAGNETON_TRAP(), SUICUNE_CM()],
  'Gengar leads and Hypnotizes; Breloom Sub-Seeds the sleeper; Snorlax Sub-Punches the things that want to switch into it. Magneton removes Skarmory so Focus Punch users are not walled.', tier='ou')

# perish song trapping -----------------------------------------------------------------------------------------
T('perish_trap', [M('GENGAR', 'MEAN_LOOK PERISH_SONG PROTECT SHADOW_BALL', nat='TIMID', ev='hp252 spe252 spd6', ab='LEVITATE'),
                  M('MISDREAVUS', 'MEAN_LOOK PERISH_SONG PROTECT PAIN_SPLIT', nat='CALM', ev='hp252 def128 spd128', ab='LEVITATE'),
                  SKARM_SPIKES(), BLISS_TOSS(), SWAMP_ROAR(), TTAR_PURSUIT()],
  'Perish trapping: Gengar/Misdreavus Mean Look a wall, Perish Song, then Protect and stall three turns before switching out on the last turn. Kill one Pokemon per trap; Skarmory/Blissey hold the line in between.', tier='ou')
T('perish_trap', [UMBREON_BP(), LAPRAS_PERISH(), CELEBI_PERISH(), MAROWAK_PERISH(), MAGNETON_TRAP(), GENGAR_SPINBLOCK()],
  'Umbreon Mean Looks and Baton Passes the trap to Lapras or Celebi, who Perish Song and stall it out with Rest/Recover. Win by picking off walls one at a time; Marowak Perish Songs as a last resort.', tier='ou')
T('perish_trap', [M('MISDREAVUS', 'MEAN_LOOK PERISH_SONG PROTECT DESTINY_BOND', nat='TIMID', ev='hp252 spe252 spd6', ab='LEVITATE'),
                  M('ARIADOS', 'SPIDER_WEB BATON_PASS TOXIC PROTECT', nat='IMPISH', ev='hp252 def252 spe6', ab='INSOMNIA'),
                  M('POLITOED', 'PERISH_SONG SURF ICE_BEAM PROTECT', nat='BOLD', ev='hp252 def252 spa6', ab='WATER_ABSORB'),
                  WOBB_STD(), BLISS_TOSS(), SKARM_SPIKES()],
  'Three trappers: Misdreavus Mean Look, Ariados Spider Web -> Baton Pass to Politoed Perish Song, and Wobbuffet Shadow Tag. Trap a wall, Perish Song, Protect, switch on the last turn.', tier='ou')
T('perish_trap', [M('GENGAR', 'MEAN_LOOK PERISH_SONG PROTECT SUBSTITUTE', nat='TIMID', ev='hp252 spe252 spd6', ab='LEVITATE'),
                  M('AZUMARILL', 'PERISH_SONG SURF RETURN PROTECT', nat='ADAMANT', ev='hp252 atk252 def6', ab='HUGE_POWER'),
                  WOBB_DBOND(), SNORLAX_CURSE(), CLAYDOL_SPIN(), ZAPDOS_STD()],
  'Gengar leads with Substitute then Mean Look + Perish Song; Wobbuffet Shadow Tags and Encores a setup move so Azumarill can Perish Song safely. Win by removing the opponent walls to let Curselax through.', tier='ou')

# wobbuffet traps ---------------------------------------------------------------------------------------------
T('wobbuffet_trap', [WOBB_STD(), TTAR_DD(), MENCE_DD(), SKARM_SPIKES(), BLISS_BELL(), MAGNETON_TRAP()],
  'Wobbuffet traps whatever attacks it and kills it with Counter/Mirror Coat, Encore-locking anything that sets up. Once the checks to Tyranitar and Salamence are gone, Dragon Dance and sweep.', tier='ou')
T('wobbuffet_trap', [WOBB_DBOND(), JOLTEON_BP(), SNORLAX_BD(), GENGAR_HYPNO(), SWAMP_MIXED(), AERO_LEAD()],
  'Wobbuffet Encores a resisted or setup move, then Jolteon/Snorlax get a free switch to boost. Destiny Bond takes one more with it. Win with a Belly Drum Snorlax sweep after Wobbuffet has cleared its checks.', tier='ou')
T('wobbuffet_trap', [WYNAUT_STD(), WOBB_STD(), CELEBI_BP(), META_AGI(), SUICUNE_CM(), DUGTRIO_TRAP()],
  'Double Shadow Tag (Wynaut, Wobbuffet) plus Dugtrio Arena Trap: nothing gets to switch out. Encore setup moves, Counter/Mirror Coat attackers, and pass Celebi Calm Minds into Metagross or Suicune.', tier='ou')

# boom teams ------------------------------------------------------------------------------------------------------
T('boom', [GENGAR_ATK(), META_STD(), CLAYDOL_SPIN(), FORRY_SPIKES(), GOLEM_BOOM(), SNORLAX_CB()],
  'Boom team: every Pokemon except Snorlax carries Explosion. Trade one-for-one from the lead on: Forretress Spikes then Explodes, Gengar/Metagross/Claydol/Golem Explode on their checks, Choice Band Snorlax cleans.', tier='ou')
T('boom', [ELECTRODE_BOOM(), WEEZING_BOOM(), EXEGG_SUN_SLEEP(), REGICE_STD(), SOLROCK_BOOM(), MENCE_DD()],
  'Electrode leads, Taunts and Explodes on the lead; Exeggutor Sleep Powders then Explodes, Weezing burns then Explodes, Regice/Solrock trade too. Salamence Dragon Dances on the wreckage.', tier='ou')
T('boom', [CLOYSTER_SPIKES(), GLALIE_SPIKES(), CAMERUPT_BOOM(), MUK_BOOM(), LUNATONE_BOOM(), TTAR_DD()],
  'Two Spikers who Explode after laying layers (Cloyster, Glalie); Camerupt/Muk/Lunatone each take a wall down with Explosion. Tyranitar sweeps a 5-on-1 once the trades are done.', tier='ou')

# sun / rain / sand ------------------------------------------------------------------------------------------------
T('sun', [CHARIZARD_SUN(), EXEGG_SUN(), VICTREEBEL_SUN(), JUMPLUFF_SUN(), MOLTRES_SUN(), DUGTRIO_TRAP()],
  'Sunny Day OU: Charizard leads and sets sun, Exeggutor/Victreebel/Jumpluff outspeed with Chlorophyll and fire off one-turn Solar Beams; Fire moves are boosted. Dugtrio traps Tyranitar, which resets the weather.', tier='ou')
T('sun', [HOUNDOOM_SUN(), TANGELA_SUN(), SHIFTRY_SUN(), NINETALES_SUN(), VICTREEBEL_SD(), CLAYDOL_SPIN()],
  'Houndoom leads and sets sun (Flash Fire absorbs Fire moves); Tangela and Shiftry sweep under Chlorophyll, Victreebel Swords Dances. Win before the eight sun turns run out; reset with Ninetales.', tier='ou')
T('sun', [NINETALES_SUN(), EXEGG_SUN_SLEEP(), CHARIZARD_SUN(), JUMPLUFF_SEED(), MOLTRES_SUN(), MAGNETON_TRAP()],
  'Ninetales sets sun and Will-O-Wisps; Exeggutor Sleep Powders and Solar Beams; Charizard and Moltres get 1.5x Fire Blasts. Magneton kills Skarmory, the main answer to the Chlorophyll sweepers.', tier='ou')
T('rain', [ZAPDOS_RAIN(), KINGDRA_RAIN(), LUDI_RAIN(), KABUTOPS_RAIN(), OMASTAR_RAIN(), MAGNETON_RAIN()],
  'Rain Dance OU: Zapdos leads and sets rain for 100 percent Thunder; Kingdra, Ludicolo, Omastar and Kabutops double their Speed with Swift Swim and sweep with boosted Surfs. Reset rain from any member.', tier='ou')
T('rain', [KINGDRA_SUB(), GOREBYSS_RAIN(), QWILFISH_RAIN(), LUDI_SEED(), ZAPDOS_RAIN(), META_STD()],
  'Qwilfish leads: Rain Dance, Spikes, Self-Destruct. Gorebyss and Kingdra sweep under rain; Ludicolo Rain Dish heals with Leech Seed. Metagross covers the Zapdos/Raikou that stop the swimmers.', tier='ou')
T('rain', [OMASTAR_RAIN(), KABUTOPS_RAIN(), KINGDRA_RAIN(), MAGNETON_RAIN(), SUICUNE_CM(), DUGTRIO_TRAP()],
  'Magneton leads and sets rain for Thunder; the three Swift Swimmers then take turns sweeping. Dugtrio removes Blissey/Tyranitar (sand cancels rain). Keep one rain setter alive at all times.', tier='ou')
T('sand', [TTAR_DD(), CACTURNE_SAND(), SANDSLASH_SAND(), DUGTRIO_SAND(), SKARM_SPIKES(), CLAYDOL_SPIN()],
  'Tyranitar leads and its Sand Stream chips everything not Rock/Ground/Steel; Cacturne, Sandslash and Dugtrio dodge behind Sand Veil and Substitute. Win on sand + Spikes attrition and a late Tyranitar sweep.', tier='ou')
T('sand', [TTAR_PURSUIT(), GLIGAR_TOXIC(), REGIROCK_CURSE(), STEELIX_CURSE(), BLISS_WALL(), CACTURNE_SPIKES()],
  'Sandstorm stall: Tyranitar summons sand, then Gligar/Regirock/Steelix (all sand-immune) Toxic and Protect stall while sand ticks. Cacturne lays Spikes; Blissey heals. Win by residual damage, not attacks.', tier='ou')
T('sand', [TTAR_CB(), GLIGAR_SAND(), DUGTRIO_TRAP(), META_AGI(), AERO_LEAD(), MILOTIC_STALL()],
  'Aerodactyl leads and Taunts; Tyranitar brings sand which stacks with Choice Band hits and Gligar Swords Dances behind Sand Veil misses. Metagross and Dugtrio are sand-immune cleaners.', tier='ou')

# ubers weather ------------------------------------------------------------------------------------------------------
T('ubers_weather', [KYOGRE_CM(), KINGDRA_RAIN(), LUDI_RAIN(), LATIOS_CM(), RAYQUAZA_DD(), META_STD()],
  'Kyogre leads and its Drizzle is permanent rain: Calm Mind Surf hits everything, Kingdra and Ludicolo double Speed with Swift Swim. Rayquaza is the backup sweeper. Beat Groudon by keeping Kyogre alive to re-set rain.', tier='ubers')
T('ubers_weather', [GROUDON_SD(), EXEGG_SUN(), VICTREEBEL_SD(), HOOH_CM(), MEWTWO_BOLT(), LATIAS_CM()],
  'Groudon leads and its Drought is permanent sun: Swords Dance Earthquake plus Overheat, Exeggutor/Victreebel sweep with Chlorophyll, Ho-Oh Sacred Fire hits 1.5x. Keep Groudon alive to re-set sun over Kyogre.', tier='ubers')
T('ubers_weather', [KYOGRE_SPOUT(), GROUDON_ERUPT(), RAYQUAZA_MIX(), LUGIA_WALL(), DEOXYS_ATK(), JIRACHI_WISH()],
  'Weather war: whichever of Kyogre/Groudon comes in last owns the weather; Water Spout under rain or Eruption under sun at full HP is the win condition. Rayquaza Air Lock ignores weather to revenge either.', tier='ubers')

# belly drum -------------------------------------------------------------------------------------------------------
T('belly_drum', [SNORLAX_BD(), MAGNETON_TRAP(), DUGTRIO_TRAP(), GENGAR_HYPNO(), CLAYDOL_SPIN(), SUICUNE_CM()],
  'Belly Drum Snorlax is the win condition: Magneton removes Skarmory, Dugtrio removes Tyranitar/Blissey, Gengar sleeps a wall, then Snorlax Drums on a special attacker and sweeps at +6.', tier='ou')
T('belly_drum', [CHARIZARD_BD(), M('LINOONE', 'BELLY_DRUM RETURN SHADOW_BALL SUBSTITUTE', nat='ADAMANT', ev='hp6 atk252 spe252', ab='PICKUP', item='SALAC_BERRY'),
                  SNORLAX_BD_REST(), MAGNETON_TRAP(), DUGTRIO_TRAP(), ZAPDOS_SUB()],
  'Three Belly Drummers: Charizard drums to Salac range and sweeps with EQ/Rock Slide/Aerial Ace, Linoone drums behind a Substitute, Snorlax drums with Chesto-Rest. Remove Skarmory/Tyranitar first with Magneton/Dugtrio.', tier='ou')
T('belly_drum', [M('AZUMARILL', 'BELLY_DRUM RETURN HIDDEN_POWER SUBSTITUTE', nat='ADAMANT', ev='hp252 atk252 spe6', ab='HUGE_POWER', item='SALAC_BERRY'),
                  M('POLIWRATH', 'BELLY_DRUM HYDRO_PUMP BRICK_BREAK HYPNOSIS', nat='ADAMANT', ev='hp252 atk252 spe6', ab='WATER_ABSORB', item='SALAC_BERRY'),
                  M('CLEFABLE', 'BELLY_DRUM RETURN SHADOW_BALL SOFT_BOILED', nat='ADAMANT', ev='hp252 atk252 spe6', ab='CUTE_CHARM'),
                  JOLTEON_BP(), GENGAR_HYPNO(), SKARM_SPIKES()],
  'Jolteon passes Substitute so Azumarill (Huge Power) or Poliwrath can Belly Drum safely; Poliwrath Hypnosis buys the turn otherwise. Win with a +6 Huge Power Return sweep from behind a Sub.', tier='uu')

# dragon dance ---------------------------------------------------------------------------------------------------------
T('dragon_dance', [MENCE_DD(), TTAR_DD_HP(), GYARA_DD(), MAGNETON_TRAP(), SWAMP_MIXED(), CLAYDOL_SPIN()],
  'Three Dragon Dancers (Salamence, Tyranitar, Gyarados) that share checks: Magneton removes Skarmory, Swampert beats the enemy Tyranitar. Lead Tyranitar; whichever dancer gets a free turn sweeps.', tier='ou')
T('dragon_dance', [M('KINGDRA', 'DRAGON_DANCE HIDDEN_POWER DOUBLE_EDGE SUBSTITUTE', nat='ADAMANT', ev='hp6 atk252 spe252', ab='SWIFT_SWIM'),
                  M('ALTARIA', 'DRAGON_DANCE EARTHQUAKE HIDDEN_POWER REST', nat='ADAMANT', ev='hp252 atk252 spe6', ab='NATURAL_CURE'),
                  M('DRAGONITE', 'DRAGON_DANCE HIDDEN_POWER EARTHQUAKE DOUBLE_EDGE', nat='ADAMANT', ev='hp6 atk252 spe252', ab='INNER_FOCUS', item='LUM_BERRY'),
                  GYARA_DD_TAUNT(), MAGNETON_TRAP(), BLISS_TOSS()],
  'Dragon Dance Dragons: Dragonite, Kingdra and Altaria each want one free turn. Gyarados Taunts phazers first; Magneton kills Skarmory; Blissey absorbs the Ice Beams aimed at the dragons.', tier='uu')
T('dragon_dance', [TTAR_DD(), MENCE_DD_AA(), SKARM_SPIKES(), GENGAR_SPINBLOCK(), BLISS_BELL(), DUGTRIO_TRAP()],
  'Spikes plus two Dragon Dancers: Skarmory lays Spikes, Gengar keeps them, and once the Roar users are worn down Tyranitar or Salamence set up and sweep. Dugtrio traps the Swampert that walls Tyranitar.', tier='ou')

# swords dance / calm mind sweepers -------------------------------------------------------------------------------
T('swords_dance', [HERA_SD(), SCIZOR_SD(), MAROWAK_SD(), MAGNETON_TRAP(), GENGAR_HYPNO(), SUICUNE_CM()],
  'Swords Dance offense: Heracross, Scizor and Thick Club Marowak each need one free turn (Gengar Hypnosis provides it). Magneton removes Skarmory. Win by a +2 sweep once Salamence/Weezing are gone.', tier='ou')
T('swords_dance', [KABUTOPS_SD(), SCEPTILE_SD(), BLAZIKEN_SD(), ABSOL_SD(), DUGTRIO_TRAP(), ZAPDOS_STD()],
  'Salac Berry Swords Dancers: Sceptile and Blaziken set up to +2 and pop Salac at low HP; Absol passes Swords Dance to Kabutops. Dugtrio removes the Tyranitar and Blissey that stall them.', tier='uu')
T('swords_dance', [MAROWAK_SD(), M('SANDSLASH', 'SWORDS_DANCE EARTHQUAKE ROCK_SLIDE HIDDEN_POWER', nat='ADAMANT', ev='hp252 atk252 spe6', ab='SAND_VEIL'),
                  M('KINGLER', 'SWORDS_DANCE CRABHAMMER HIDDEN_POWER DOUBLE_EDGE', nat='ADAMANT', ev='hp6 atk252 spe252', ab='HYPER_CUTTER', item='SALAC_BERRY'),
                  M('GLIGAR', 'SWORDS_DANCE EARTHQUAKE AERIAL_ACE HIDDEN_POWER', nat='ADAMANT', ev='hp6 atk252 spe252', ab='SAND_VEIL', item='LUM_BERRY'),
                  MAGNETON_TRAP(), ALAKAZAM_ENCORE()],
  'Alakazam Encores a support move so a Swords Dancer can switch in and boost; Marowak, Sandslash, Kingler, Gligar each break a different wall at +2. Magneton traps Skarmory, the universal answer.', tier='uu')
T('calm_mind', [SUICUNE_CM(), JIRACHI_CM(), CELEBI_CM(), TTAR_DD(), SKARM_SPIKES(), DUGTRIO_TRAP()],
  'Three Calm Mind users that beat each other checks: Suicune (Rest/Roar) wins long games, Jirachi and Celebi win once Blissey is trapped by Dugtrio. Tyranitar Pursuits the Gengar and Skarmory Spikes.', tier='ou')
T('calm_mind', [RAIKOU_CM(), ALAKAZAM_CM(), GARDEVOIR_CM(), SNORLAX_CURSE(), SWAMP_ROAR(), MAGNETON_TRAP()],
  'Calm Mind sweepers with Rest (Raikou) or Recover (Alakazam): boost on Skarmory/Forretress after Magneton has removed the enemy Magneton, then sweep. Snorlax checks the special attackers aimed at them.', tier='ou')
T('calm_mind', [SLOWBRO_CM(), BLISS_CM(), SUICUNE_CROC(), JIRACHI_CM(), CLAYDOL_SPIN(), DUGTRIO_TRAP()],
  'Bulky Calm Mind wall-breakers: Slowbro and Blissey boost on physical/special attackers respectively, Suicune with Chesto Rest is the closer. Dugtrio traps the opposing Blissey that PP-stalls them.', tier='ou')

# choice band --------------------------------------------------------------------------------------------------------
T('choice_band', [MENCE_CB(), TTAR_CB(), HERA_CB(), META_CB(), SKARM_SPIKES(), CLAYDOL_SPIN()],
  'Choice Band offense: Salamence, Tyranitar, Heracross and Metagross each 2HKO the walls that the others cannot. Lead Skarmory for Spikes; predict the switch, click the move that hits it, switch out, repeat.', tier='ou')
T('choice_band', [AERO_CB(), MEDICHAM_CB(), DODRIO_CB(), SLAKING_CB(), MAGNETON_TRAP(), BLISS_BELL()],
  'Fast Choice Banders (Aerodactyl, Medicham, Dodrio) revenge kill and pressure; Slaking Return breaks walls in one hit per switch. Magneton removes Skarmory; Blissey gives switches.', tier='ou')
T('choice_band', [SNORLAX_CB(), URSARING_CB(), MACHAMP_CB(), SWAMP_MIXED(), GENGAR_SPINBLOCK(), FORRY_SPIKES()],
  'Slow, hard Choice Banders: Snorlax, Ursaring and Machamp trade hits; Spikes from Forretress turn 2HKOs into OHKOs. Lead Forretress; win by making every switch-in cost a third of its HP.', tier='ou')

# endure / substitute + pinch berries + reversal/flail/endeavor; FEAR ------------------------------------------------
T('endure_berry', [HERA_ENDURE(), BLAZIKEN_ENDURE(), M('KANGASKHAN', 'ENDURE REVERSAL EARTHQUAKE SHADOW_BALL', nat='JOLLY', ev='hp6 atk252 spe252', ab='EARLY_BIRD', item='SALAC_BERRY'),
                   M('DONPHAN', 'ENDURE FLAIL EARTHQUAKE ROCK_SLIDE', nat='ADAMANT', ev='hp6 atk252 spe252', ab='STURDY', item='SALAC_BERRY'),
                   GENGAR_HYPNO(), MAGNETON_TRAP()],
  'Endure to 1 HP, Salac Berry activates for +1 Speed, then Reversal/Flail at 200 base power. Heracross leads; Gengar sleeps the priority users (Extreme Speed/Quick Attack) that beat this strategy. Sand and Spikes kill it, so keep Tyranitar out.', tier='ou')
T('endure_berry', [M('SWELLOW', 'SUBSTITUTE ENDEAVOR QUICK_ATTACK AERIAL_ACE', nat='JOLLY', ev='hp6 atk252 spe252', ab='GUTS', item='LIECHI_BERRY'),
                   CHARIZARD_SUB(), KINGDRA_SUB(), SCEPTILE_SUB(), M('RAIKOU', 'SUBSTITUTE CALM_MIND THUNDERBOLT HIDDEN_POWER', nat='TIMID', ev='hp6 spe252 spa252', ab='PRESSURE', item='PETAYA_BERRY'), DUGTRIO_TRAP()],
  'Substitute three times to reach 25 percent HP, the pinch berry fires (+1 SpA Petaya, +1 Atk Liechi) and the last Sub sweeps. Swellow Endeavors from behind a Sub to bring walls to its own HP, then Quick Attacks.', tier='ou')
T('endure_berry', [M('HITMONLEE', 'ENDURE REVERSAL ROCK_SLIDE HIDDEN_POWER', nat='ADAMANT', ev='hp6 atk252 spe252', ab='LIMBER', item='SALAC_BERRY'),
                   M('HARIYAMA', 'ENDURE REVERSAL ROCK_SLIDE FAKE_OUT', nat='ADAMANT', ev='hp6 atk252 spe252', ab='GUTS', item='SALAC_BERRY'),
                   M('VIGOROTH', 'ENDURE REVERSAL ROCK_SLIDE SHADOW_BALL', nat='JOLLY', ev='hp6 atk252 spe252', ab='VITAL_SPIRIT', item='SALAC_BERRY'),
                   ZAPDOS_STD(), CLAYDOL_SPIN(), GENGAR_HYPNO()],
  'Fighting-type Endure/Reversal sweepers; Claydol spins away Spikes and Gengar sleeps the Salamence/Gengar that resist Reversal. Endure on a hit that would kill, Salac boosts Speed, Reversal from 1 HP.', tier='uu')
T('endure_berry', [M('KABUTOPS', 'ENDURE FLAIL ROCK_SLIDE SWORDS_DANCE', nat='ADAMANT', ev='hp6 atk252 spe252', ab='BATTLE_ARMOR', item='SALAC_BERRY'),
                   M('FLAREON', 'ENDURE FLAIL SHADOW_BALL FIRE_BLAST', nat='ADAMANT', ev='hp6 atk252 spe252', ab='FLASH_FIRE', item='SALAC_BERRY'),
                   M('TORKOAL', 'ENDURE FLAIL OVERHEAT EXPLOSION', nat='ADAMANT', ev='hp6 atk252 spe252', ab='WHITE_SMOKE', item='SALAC_BERRY'),
                   SKARM_SPIKES(), BLISS_TOSS(), MAGNETON_TRAP()],
  'Flail variants: Kabutops Swords Dances first then Endures, Flareon uses Flash Fire switches. Torkoal is the oddball: it is one of the few FRLG Pokemon that learns both Endure and Flail, and Explodes when the trick fails. Skarmory/Blissey hold the fort.', tier='uu')
T('fear', [M('EEVEE', 'ENDURE FLAIL TACKLE TAIL_WHIP', nat='JOLLY', ev='', ab='RUN_AWAY', item='SALAC_BERRY', level=5),
           M('SCYTHER', 'ENDURE REVERSAL QUICK_ATTACK LEER', nat='JOLLY', ev='', ab='SWARM', item='SALAC_BERRY', level=5),
           M('RATTATA', 'ENDEAVOR QUICK_ATTACK SUBSTITUTE SUPER_FANG', nat='JOLLY', ev='hp6 atk252 spe252', ab='GUTS', item='FOCUS_BAND', level=41),
           M('SWELLOW', 'ENDEAVOR QUICK_ATTACK SUBSTITUTE AERIAL_ACE', nat='JOLLY', ev='hp6 atk252 spe252', ab='GUTS', item='LIECHI_BERRY', level=100),
           M('ARON', 'ENDEAVOR HARDEN ROCK_TOMB PROTECT', nat='IMPISH', ev='', ab='STURDY', item='FOCUS_BAND', level=5),
           M('DODUO', 'ENDEAVOR QUICK_ATTACK FLAIL PECK', nat='JOLLY', ev='', ab='EARLY_BIRD', item='FOCUS_BAND', level=5)],
  'FEAR (F.E.A.R.) as far as FireRed allows: no species learns Endure AND Endeavor in FRLG and Focus Sash does not exist, so the legal pieces are split. Eevee/Scyther L5 Endure+Flail/Reversal with Salac; Rattata L41 (its Endeavor level) and Aron/Doduo L5 Endeavor (egg move) with Focus Band, which only sometimes saves them. Play: survive one hit, Endeavor to equalise HP, Quick Attack to finish.', tier='none', tags=['gimmick', 'low_level'])
T('fear', [M('RATTATA', 'ENDURE ENDEAVOR QUICK_ATTACK TAIL_WHIP', nat='JOLLY', ev='', ab='GUTS', item='FOCUS_BAND', level=1),
           M('RATTATA', 'ENDURE ENDEAVOR QUICK_ATTACK TAIL_WHIP', nat='JOLLY', ev='', ab='GUTS', item='FOCUS_BAND', level=2),
           M('RATICATE', 'ENDURE ENDEAVOR QUICK_ATTACK SUPER_FANG', nat='JOLLY', ev='', ab='GUTS', item='FOCUS_BAND', level=5),
           M('SWELLOW', 'ENDURE ENDEAVOR QUICK_ATTACK AERIAL_ACE', nat='JOLLY', ev='', ab='GUTS', item='FOCUS_BAND', level=5),
           M('PHANPY', 'ENDURE ENDEAVOR EARTHQUAKE PROTECT', nat='JOLLY', ev='', ab='PICKUP', item='FOCUS_BAND', level=5),
           M('SNORLAX', 'CURSE BODY_SLAM SHADOW_BALL REST', nat='CAREFUL', ev='hp188 def68 spd252', ab='THICK_FAT')],
  'FEAR as commonly described (Level 1 Rattata: Endure, Endeavor, Quick Attack). NOT FireRed-legal: Rattata only learns Endeavor at L41 and never Endure in FRLG, Swellow/Phanpy never learn Endeavor+Endure together, and level 1 is below the L5 hatch level; Focus Band stands in for Focus Sash. Play: Endure the first hit, Endeavor to 1 HP, Quick Attack.', tier='none', tags=['gimmick', 'low_level', 'not_frlg_legal'])
T('fear', [M('SWELLOW', 'ENDEAVOR QUICK_ATTACK FACADE SUBSTITUTE', nat='JOLLY', ev='hp6 atk252 spe252', ab='GUTS', item='LIECHI_BERRY', level=30),
           M('TAILLOW', 'ENDEAVOR QUICK_ATTACK AERIAL_ACE DOUBLE_TEAM', nat='JOLLY', ev='', ab='GUTS', item='FOCUS_BAND', level=26),
           M('RATTATA', 'ENDEAVOR QUICK_ATTACK SUBSTITUTE HYPER_FANG', nat='JOLLY', ev='hp6 atk252 spe252', ab='GUTS', item='FOCUS_BAND', level=41),
           M('EEVEE', 'ENDURE FLAIL QUICK_ATTACK SHADOW_BALL', nat='JOLLY', ev='hp6 atk252 spe252', ab='RUN_AWAY', item='SALAC_BERRY', level=30)],
  'Lowest-level legal Endeavor users in FRLG (Taillow/Swellow from L26, Rattata from L41) with Focus Band or a Substitute to survive the first hit, plus an Endure+Flail Eevee. Play: get low, Endeavor, finish with Quick Attack.', tier='none', tags=['gimmick', 'low_level'], level=41)

# dual screens + setup ------------------------------------------------------------------------------------------------
T('dual_screens', [ALAKAZAM_SCREENS(), TTAR_DD(), SNORLAX_CURSE(), META_AGI(), SUICUNE_CROC(), MAGNETON_TRAP()],
  'Alakazam leads with Reflect + Light Screen, then the setup sweepers (Dragon Dance Tyranitar, Agility Metagross, Curselax) boost behind the screens. Magneton removes Skarmory; win within the five screen turns.', tier='ou')
T('dual_screens', [JIRACHI_SCREENS(), CELEBI_CM(), SNORLAX_BD(), GYARA_DD(), CLAYDOL_SCREENS(), DUGTRIO_TRAP()],
  'Two screen setters (Jirachi, Claydol) so the sweepers always get screens: Belly Drum Snorlax behind Reflect survives anything, Gyarados and Celebi boost behind them. Dugtrio traps the Blissey that stalls Celebi.', tier='ou')
T('dual_screens', [ESPEON_SCREENS(), META_SCREENS(), MENCE_DD(), HERA_SD(), RAIKOU_CM(), SWAMP_CURSE()],
  'Espeon leads with screens then Morning Sun stalls; Metagross re-sets screens later. Salamence, Heracross and Raikou each get one setup turn behind screens and sweep.', tier='ou')

# wish passing ----------------------------------------------------------------------------------------------------------
T('wish_pass', [VAPOREON_WISH(), JIRACHI_WISH(), UMBREON_WISH(), SKARM_SPIKES(), TTAR_DD(), META_STD()],
  'Wish support: Vaporeon, Jirachi and Umbreon Wish then switch to the Pokemon that needs healing (Tyranitar, Metagross, Skarmory). Win by keeping the sweepers healthy until the opponent walls are gone.', tier='ou')
T('wish_pass', [M('CLEFABLE', 'WISH PROTECT SEISMIC_TOSS THUNDER_WAVE', nat='BOLD', ev='hp252 def252 spd6', ab='CUTE_CHARM'),
                M('TOGETIC', 'WISH PROTECT TOXIC SEISMIC_TOSS', nat='CALM', ev='hp252 def92 spd164', ab='SERENE_GRACE'),
                M('XATU', 'WISH PROTECT PSYCHIC REFLECT', nat='CALM', ev='hp252 def92 spd164', ab='SYNCHRONIZE'),
                JOLTEON_WISH(), SNORLAX_CURSE(), MACHAMP_CB()],
  'Clefable, Togetic, Xatu and Jolteon all Wish; Jolteon Baton Passes the Wish into a sweeper. Curselax and Choice Band Machamp are the receivers; win by wearing the opponent down while the receivers never die.', tier='uu')
T('wish_pass', [VAPOREON_WISHBP(), JIRACHI_PARA(), LATIAS_WISH(), SNORLAX_CURSE_EQ(), SWAMP_CURSE(), BLISS_TOSS()],
  'Vaporeon Wish + Baton Pass gives Snorlax or Swampert a full heal mid-setup; Jirachi Body Slam paralyses, Latias Wish/Toxic stalls. Win with a Curse tank kept healthy by three Wish users.', tier='ubers')

# toxic + protect stall ------------------------------------------------------------------------------------------------------
T('toxic_protect', [ZAPDOS_PP(), MILOTIC_STALL(), GLIGAR_TOXIC(), UMBREON_WISH(), BLISS_WALL(), SKARM_TOXIC()],
  'Toxic + Protect stall: Zapdos leads, Toxics, Substitutes/Protects while poison ticks; Gligar, Umbreon and Blissey do the same. Skarmory Spikes plus Whirlwind spreads Toxic across the team. Win by poison damage alone.', tier='ou')
T('toxic_protect', [M('TENTACRUEL', 'TOXIC PROTECT SURF RAPID_SPIN', nat='CALM', ev='hp252 def6 spd252', ab='LIQUID_OOZE'),
                    M('WEEZING', 'TOXIC PROTECT SLUDGE_BOMB PAIN_SPLIT', nat='BOLD', ev='hp252 def252 spa6', ab='LEVITATE'),
                    M('SUICUNE', 'TOXIC PROTECT SURF REST', nat='BOLD', ev='hp252 def252 spe6', ab='PRESSURE'),
                    DUSCLOPS_WALL(), BLISS_WALL(), CELEBI_SEED()],
  'Every wall carries Toxic or Protect; Dusclops and Suicune have Pressure so Protect burns 2 PP per turn. Celebi Heal Bells the walls own status. Win by Toxic attrition; Tentacruel spins Spikes away.', tier='ou')
T('toxic_protect', [GLIGAR_TOXIC(), M('LUDICOLO', 'TOXIC PROTECT LEECH_SEED SURF', nat='CALM', ev='hp252 def92 spd164', ab='RAIN_DISH'),
                    M('QUAGSIRE', 'TOXIC PROTECT EARTHQUAKE REST', nat='RELAXED', ev='hp252 def252 spd6', ab='WATER_ABSORB'),
                    M('FORRETRESS', 'TOXIC PROTECT SPIKES RAPID_SPIN', nat='RELAXED', ev='hp252 def252 spd6', ab='STURDY'),
                    MILOTIC_STALL(), BLISS_BELL()],
  'UU-style Toxic stall: Forretress lays Spikes, Ludicolo Leech Seeds, everything Protects on the poison turns. Rest users (Quagsire, Blissey Heal Bell) keep the walls alive; win when the whole enemy team is poisoned.', tier='uu')

# pressure pp stall ------------------------------------------------------------------------------------------------------------------
T('pp_stall', [ZAPDOS_PP(), DUSCLOPS_WALL(), MOLTRES_PP(), M('ARTICUNO', 'SUBSTITUTE PROTECT TOXIC ICE_BEAM', nat='TIMID', ev='hp252 spe252 spa6', ab='PRESSURE'),
               BLISS_WALL(), SUICUNE_ROAR()],
  'Pressure PP stall: every Pressure Pokemon Substitutes or Protects so each attack costs the opponent 2 PP. Zapdos leads with Sub/Protect/Toxic; win when the opponent is down to Struggle.', tier='ou')
T('pp_stall', [DUSCLOPS_REST(), M('DUSCLOPS', 'PROTECT REST PAIN_SPLIT SEISMIC_TOSS', nat='CAREFUL', ev='hp252 def100 spd156', ab='PRESSURE'),
               ZAPDOS_PP(), M('ABSOL', 'PROTECT SUBSTITUTE TOXIC SHADOW_BALL', nat='JOLLY', ev='hp252 spe252 atk6', ab='PRESSURE'),
               SKARM_TOXIC(), BLISS_TOSS()],
  'Double Dusclops Pressure stall: Protect every other turn, Rest off damage, Pain Split when low. Zapdos and Absol Substitute against Roar. Win by draining the opponent moves to zero PP; Skarmory Spikes speed it up.', tier='ou')
T('pp_stall', [LUGIA_PP(), M('MEWTWO', 'SUBSTITUTE PROTECT TOXIC RECOVER', nat='TIMID', ev='hp252 spe252 spd6', ab='PRESSURE'),
               HOOH_WALL(), M('RAIKOU', 'SUBSTITUTE PROTECT TOXIC THUNDERBOLT', nat='TIMID', ev='hp252 spe252 spa6', ab='PRESSURE'),
               M('ENTEI', 'SUBSTITUTE PROTECT TOXIC FLAMETHROWER', nat='TIMID', ev='hp252 spe252 spa6', ab='PRESSURE'),
               DUSCLOPS_WALL()],
  'Ubers Pressure stall: Lugia and Mewtwo Toxic, Sub, Protect and Recover; Ho-Oh Whirlwinds boosters. Every attack against a Pressure user costs 2 PP; win by PP exhaustion.', tier='ubers')

# rest + sleep talk ------------------------------------------------------------------------------------------------------------------
T('rest_talk', [SNORLAX_TALK(), LAPRAS_TALK(), URSARING_TALK(), MILTANK_TALK(), SLOWBRO_TALK(), SKARM_SPIKES()],
  'Rest + Sleep Talk on every tank: Rest at half HP, Sleep Talk picks an attack or Curse while asleep. Snorlax and Miltank Curse up; Ursaring Guts is boosted while asleep. Skarmory Spikes; win by out-lasting.', tier='ou')
T('rest_talk', [SNORLAX_TALK_ATK(), M('SUICUNE', 'CALM_MIND SURF REST ROAR', nat='BOLD', ev='hp252 def220 spe36', ab='PRESSURE'),
                M('WAILORD', 'REST SLEEP_TALK SURF ICE_BEAM', nat='CALM', ev='hp252 def92 spd164', ab='WATER_VEIL'),
                M('EXPLOUD', 'REST SLEEP_TALK RETURN OVERHEAT', nat='ADAMANT', ev='hp252 atk252 spe6', ab='SOUNDPROOF'),
                UMBREON_CURSE(), CLAYDOL_SPIN()],
  'Rest-Talk attackers (Snorlax, Wailord, Exploud) with Curse Umbreon: the sleep turns are never wasted. Suicune Rests without Sleep Talk and relies on Roar. Win by never being worn down.', tier='ou')
T('rest_talk', [SLOWBRO_CURSE(), MILTANK_TALK(), M('RELICANTH', 'REST SLEEP_TALK ROCK_SLIDE EARTHQUAKE', nat='IMPISH', ev='hp252 def252 spd6', ab='ROCK_HEAD'),
                M('LICKITUNG', 'REST SLEEP_TALK BODY_SLAM CURSE', nat='CAREFUL', ev='hp252 def6 spd252', ab='OWN_TEMPO'),
                BLISS_BELL(), MAGNETON_TRAP()],
  'Curse + Rest + Sleep Talk on Slowbro, Miltank and Lickitung; Relicanth Rest-Talks with Rock Head recoil-free hits. Blissey Heal Bells if a Curser gets Toxic-ed before Resting. Win with one +6 tank.', tier='uu')

# trick / knock off support ------------------------------------------------------------------------------------------------------------
T('trick_knockoff', [ALAKAZAM_TRICK(), M('KECLEON', 'TRICK THUNDER_WAVE RETURN SHADOW_BALL', nat='ADAMANT', ev='hp252 atk252 spd6', ab='COLOR_CHANGE', item='CHOICE_BAND'),
                     M('SABLEYE', 'KNOCK_OFF SHADOW_BALL RECOVER TOXIC', nat='IMPISH', ev='hp252 def252 spd6', ab='KEEN_EYE'),
                     SNORLAX_CURSE(), TTAR_DD(), SKARM_SPIKES()],
  'Alakazam and Kecleon Trick a Choice Band onto walls (Blissey/Skarmory) to lock them into one move; Sableye Knocks Off Leftovers (Kecleon has no Knock Off in FRLG, so it paralyses instead). Curselax and Tyranitar then set up on the crippled walls.', tier='ou')
T('trick_knockoff', [M('GRUMPIG', 'TRICK PSYCHIC MAGIC_COAT TAUNT', nat='TIMID', ev='hp6 spe252 spa252', ab='THICK_FAT', item='CHOICE_BAND'),
                     M('KABUTOPS', 'KNOCK_OFF ROCK_SLIDE BRICK_BREAK RAPID_SPIN', nat='ADAMANT', ev='hp6 atk252 spe252', ab='BATTLE_ARMOR'),
                     M('HARIYAMA', 'KNOCK_OFF CROSS_CHOP ROCK_SLIDE FAKE_OUT', nat='ADAMANT', ev='hp252 atk252 spe6', ab='THICK_FAT'),
                     ALAKAZAM_KNOCK(), SUICUNE_CM(), MAGNETON_TRAP()],
  'Grumpig Tricks a Choice Band onto a support Pokemon, Kabutops/Hariyama/Alakazam Knock Off Leftovers and Choice Bands from everything. Suicune Calm Minds once the enemy stall is item-less.', tier='uu')
T('trick_knockoff', [M('MR_MIME', 'TRICK PSYCHIC ENCORE BATON_PASS', nat='TIMID', ev='hp252 spe252 spa6', ab='SOUNDPROOF', item='CHOICE_BAND'),
                     M('FURRET', 'TRICK RETURN SHADOW_BALL QUICK_ATTACK', nat='JOLLY', ev='hp6 atk252 spe252', ab='RUN_AWAY', item='CHOICE_BAND'),
                     M('CRAWDAUNT', 'KNOCK_OFF CRABHAMMER BRICK_BREAK SWORDS_DANCE', nat='ADAMANT', ev='hp6 atk252 spe252', ab='SHELL_ARMOR'),
                     M('BANETTE', 'KNOCK_OFF SHADOW_BALL WILL_O_WISP DESTINY_BOND', nat='ADAMANT', ev='hp6 atk252 spe252', ab='INSOMNIA'),
                     SNORLAX_BD(), CLAYDOL_SPIN()],
  'Mr. Mime Tricks the Choice Band then Encores; Furret Tricks as a second lead. Crawdaunt and Banette Knock Off items. Belly Drum Snorlax cleans once walls are locked or item-less.', tier='uu')

# ingrain / leech seed stall --------------------------------------------------------------------------------------------------------------
T('ingrain_seed', [M('CRADILY', 'INGRAIN TOXIC PROTECT ROCK_SLIDE', nat='CAREFUL', ev='hp252 def6 spd252', ab='SUCTION_CUPS'),
                   M('VENUSAUR', 'LEECH_SEED SLEEP_POWDER SLUDGE_BOMB HIDDEN_POWER', nat='TIMID', ev='hp252 spe252 spa6', ab='OVERGROW'),
                   CELEBI_SEED(), JUMPLUFF_SEED(), M('TANGELA', 'INGRAIN LEECH_SEED STUN_SPORE GIGA_DRAIN', nat='BOLD', ev='hp252 def252 spd6', ab='CHLOROPHYLL'),
                   SKARM_SPIKES()],
  'Ingrain (Cradily, Tangela) roots the wall so it cannot be Roared while healing; Leech Seed from Venusaur/Celebi/Jumpluff drains. Suction Cups also blocks phazing. Win by Leech Seed + Toxic attrition.', tier='uu')
T('ingrain_seed', [CELEBI_SEED(), M('VILEPLUME', 'INGRAIN MOONLIGHT SLEEP_POWDER SLUDGE_BOMB', nat='BOLD', ev='hp252 def252 spa6', ab='CHLOROPHYLL'),
                   M('EXEGGUTOR', 'INGRAIN LEECH_SEED PSYCHIC SLEEP_POWDER', nat='BOLD', ev='hp252 def252 spa6', ab='CHLOROPHYLL'),
                   BRELOOM_SEED(), SUICUNE_ROAR(), BLISS_TOSS()],
  'Sleep + Leech Seed: put the switch-in to sleep, Seed it, and rotate walls while it drains. Vileplume and Exeggutor Ingrain to become permanent healers; Breloom Sub-Seeds. Win by attrition.', tier='ou')
T('ingrain_seed', [SCEPTILE_SUB(), M('LUDICOLO', 'LEECH_SEED SUBSTITUTE SURF TOXIC', nat='CALM', ev='hp252 def92 spd164', ab='RAIN_DISH'),
                   M('ROSELIA', 'INGRAIN LEECH_SEED SPIKES GIGA_DRAIN', nat='BOLD', ev='hp252 def252 spd6', ab='NATURAL_CURE'),
                   CACTURNE_SAND(), JUMPLUFF_SEED(), TTAR_PURSUIT()],
  'Sub-Seed offense: Sceptile, Ludicolo, Jumpluff and Cacturne Leech Seed then Substitute so the drain covers the Sub cost. Roselia Ingrains and lays Spikes. Win when the opponent cannot break Subs faster than Seed heals.', tier='uu')

# counter / mirror coat / destiny bond suicide leads ------------------------------------------------------------------------------------
T('suicide_lead', [WOBB_DBOND(), M('SWAMPERT', 'COUNTER MIRROR_COAT EARTHQUAKE SURF', nat='RELAXED', ev='hp252 def216 spd40', ab='TORRENT'),
                   GENGAR_HYPNO(), M('BANETTE', 'DESTINY_BOND SHADOW_BALL WILL_O_WISP TAUNT', nat='JOLLY', ev='hp6 atk252 spe252', ab='INSOMNIA'),
                   TTAR_DD(), MENCE_DD()],
  'Wobbuffet leads to take one Pokemon with Counter/Mirror Coat and another with Destiny Bond; Swampert and Banette do the same. The trades leave the opponent 4-on-3 against Dragon Dance sweepers.', tier='ou')
T('suicide_lead', [DEOXYS_LEAD(), M('WEEZING', 'DESTINY_BOND WILL_O_WISP SLUDGE_BOMB EXPLOSION', nat='BOLD', ev='hp252 def252 spa6', ab='LEVITATE'),
                   M('BLASTOISE', 'MIRROR_COAT COUNTER SURF RAPID_SPIN', nat='BOLD', ev='hp252 def252 spd6', ab='TORRENT'),
                   ALAKAZAM_COUNTER(), MEWTWO_TAUNT(), META_STD()],
  'Deoxys leads: Taunt stops Spikes, Counter kills the physical lead, Psycho Boost the rest. Weezing Destiny Bonds, Blastoise Counters and Mirror Coats. Mewtwo cleans a thinned team.', tier='ubers')
T('suicide_lead', [M('CACTURNE', 'DESTINY_BOND COUNTER SPIKES NEEDLE_ARM', nat='JOLLY', ev='hp252 atk6 spe252', ab='SAND_VEIL', item='LEFTOVERS'),
                   M('MILOTIC', 'MIRROR_COAT SURF RECOVER HYPNOSIS', nat='BOLD', ev='hp252 def252 spa4', ab='MARVEL_SCALE'),
                   M('CHANSEY', 'COUNTER SOFT_BOILED SEISMIC_TOSS TOXIC', nat='BOLD', ev='hp252 def252 spd6', ab='NATURAL_CURE'),
                   M('DUSKULL', 'DESTINY_BOND WILL_O_WISP NIGHT_SHADE PAIN_SPLIT', nat='BOLD', ev='hp252 def252 spd6', ab='LEVITATE'),
                   HERA_CB(), SNORLAX_CURSE()],
  'Cacturne leads: Spikes, then Counter or Destiny Bond to trade. Milotic Mirror Coats special attackers, Chansey Counters physical ones. Two suicides plus Spikes open the way for Choice Band Heracross.', tier='uu')

# taunt / magic coat anti-stall leads --------------------------------------------------------------------------------------------------
T('taunt_lead', [TTAR_TAUNT(), GENGAR_SPINBLOCK(), AERO_LEAD(), ABSOL_MC(), SNORLAX_BD(), MAGNETON_TRAP()],
  'Anti-stall: Tyranitar leads and Taunts Skarmory before it can Spike, Aerodactyl Taunts Blissey, Absol Magic Coats Toxic/Spikes/Will-O-Wisp back. Belly Drum Snorlax wins once stall cannot status it.', tier='ou')
T('taunt_lead', [M('GRUMPIG', 'MAGIC_COAT PSYCHIC TAUNT SUBSTITUTE', nat='TIMID', ev='hp252 spe252 spa6', ab='THICK_FAT'),
                 M('GIRAFARIG', 'MAGIC_COAT PSYCHIC THUNDERBOLT BATON_PASS', nat='TIMID', ev='hp6 spe252 spa252', ab='EARLY_BIRD'),
                 M('KECLEON', 'MAGIC_COAT RETURN SHADOW_BALL TRICK', nat='ADAMANT', ev='hp252 atk252 spd6', ab='COLOR_CHANGE', item='CHOICE_BAND'),
                 GYARA_DD_TAUNT(), MENCE_DD(), META_STD()],
  'Magic Coat leads reflect Toxic, Spore, Leech Seed and Spikes back at the user; Gyarados Taunts phazers. Once the opponent status is bounced, the Dragon Dancers set up on the disabled walls.', tier='uu')
T('taunt_lead', [MEWTWO_TAUNT(), GENGAR_SPINBLOCK(), M('CROBAT', 'TAUNT SLUDGE_BOMB AERIAL_ACE HIDDEN_POWER', nat='JOLLY', ev='hp6 atk252 spe252', ab='INNER_FOCUS'),
                 TTAR_TAUNT(), SNORLAX_CURSE(), SWAMP_MIXED()],
  'Fast Taunt from Mewtwo, Gengar and Crobat stops Spikes, Toxic and Recover; then Calm Mind Mewtwo or Curselax wins the resulting attack war. Swampert covers Aerodactyl and Tyranitar.', tier='ubers')

# sleep / paralysis status spreading ----------------------------------------------------------------------------------------------------
T('status_spread', [BRELOOM_SEED(), GENGAR_HYPNO(), JIRACHI_PARA(), JOLTEON_TWAVE(), M('PARASECT', 'SPORE AROMATHERAPY GIGA_DRAIN SLUDGE_BOMB', nat='CAREFUL', ev='hp252 def6 spd252', ab='EFFECT_SPORE'), TTAR_DD()],
  'Status spam: Breloom/Parasect Spore, Gengar Hypnosis, Jirachi Serene Grace Body Slam (60 percent paralysis), Jolteon Thunder Wave. Sleep Clause is not a game rule; sleep as many as possible then Tyranitar sweeps the crippled team.', tier='ou')
T('status_spread', [M('VENUSAUR', 'SLEEP_POWDER LEECH_SEED SLUDGE_BOMB SYNTHESIS', nat='BOLD', ev='hp252 def252 spe6', ab='OVERGROW'),
                    M('HYPNO', 'HYPNOSIS THUNDER_WAVE PSYCHIC CALM_MIND', nat='CALM', ev='hp252 def6 spd252', ab='INSOMNIA'),
                    M('STARMIE', 'THUNDER_WAVE SURF RECOVER RAPID_SPIN', nat='TIMID', ev='hp6 spe252 spa252', ab='NATURAL_CURE'),
                    M('DUNSPARCE', 'BODY_SLAM HEADBUTT THUNDER_WAVE ROCK_SLIDE', nat='ADAMANT', ev='hp252 atk252 def6', ab='SERENE_GRACE'),
                    SNORLAX_CURSE(), MACHAMP_CB()],
  'Paralyse everything with Thunder Wave and Serene Grace Body Slam, then Dunsparce flinches paralysed targets with Headbutt/Rock Slide. Venusaur sleeps the walls. Slow Curselax and Machamp outspeed a paralysed team.', tier='uu')

# ---------------------------------------------------------------------------------------------------
# Doubles archetypes (level 100)
# ---------------------------------------------------------------------------------------------------
D = dict(fmt='doubles')
T('dbl_boom_protect', [GENGAR_ATK(), M('METAGROSS', 'PROTECT METEOR_MASH EARTHQUAKE EXPLOSION', nat='ADAMANT', ev='hp152 atk252 spe104', ab='CLEAR_BODY'),
                       M('SNORLAX', 'PROTECT RETURN SHADOW_BALL EARTHQUAKE', nat='ADAMANT', ev='hp144 atk252 spd112', ab='THICK_FAT'),
                       M('CLAYDOL', 'PROTECT EXPLOSION EARTHQUAKE PSYCHIC', nat='RELAXED', ev='hp252 def92 spd164', ab='LEVITATE')],
  'Doubles Explosion + Protect: one partner Protects while the other Explodes to hit both opponents. Lead Gengar (Levitate, immune to partner Earthquake) + Metagross; Claydol and Snorlax repeat the trick.', **D)
T('dbl_boom_protect', [M('ELECTRODE', 'PROTECT EXPLOSION THUNDERBOLT TAUNT', nat='HASTY', ev='atk6 spe252 spa252', ab='SOUNDPROOF'),
                       M('GOLEM', 'PROTECT EXPLOSION EARTHQUAKE ROCK_SLIDE', nat='ADAMANT', ev='hp252 atk252 def6', ab='STURDY'),
                       M('WEEZING', 'PROTECT EXPLOSION SLUDGE_BOMB WILL_O_WISP', nat='BOLD', ev='hp252 def252 spa6', ab='LEVITATE'),
                       M('REGICE', 'PROTECT EXPLOSION ICE_BEAM THUNDERBOLT', nat='MODEST', ev='hp252 spa252 spd6', ab='CLEAR_BODY'),
                       M('SALAMENCE', 'PROTECT DRAGON_DANCE EARTHQUAKE ROCK_SLIDE', nat='ADAMANT', ev='hp6 atk252 spe252', ab='INTIMIDATE'),
                       M('GARDEVOIR', 'PROTECT PSYCHIC THUNDERBOLT SKILL_SWAP', nat='TIMID', ev='hp6 spe252 spa252', ab='TRACE')],
  'Electrode leads: fastest Explosion in the game while Golem Protects, then Golem Explodes while Weezing Protects. After two double-KOs Salamence Dragon Dances behind Gardevoir Protect.', **D)
T('dbl_helping_hand', [M('JIRACHI', 'HELPING_HAND PSYCHIC THUNDERBOLT PROTECT', nat='TIMID', ev='hp252 spe252 spa6', ab='SERENE_GRACE'),
                       M('KINGDRA', 'SURF ICE_BEAM HIDDEN_POWER PROTECT', nat='MODEST', ev='hp6 spe252 spa252', ab='SWIFT_SWIM'),
                       M('LATIAS', 'HELPING_HAND DRAGON_CLAW THUNDERBOLT RECOVER', nat='TIMID', ev='hp252 spe252 spa6', ab='LEVITATE'),
                       M('METAGROSS', 'METEOR_MASH EARTHQUAKE ROCK_SLIDE PROTECT', nat='ADAMANT', ev='hp152 atk252 spe104', ab='CLEAR_BODY')],
  'Helping Hand doubles: Jirachi or Latias Helping Hands (+50 percent) the partner Surf/Earthquake/Rock Slide spread move each turn. Lead Jirachi + Kingdra; Protect on the turn the opponent targets the attacker.', **D)
T('dbl_helping_hand', [M('PLUSLE', 'HELPING_HAND THUNDERBOLT THUNDER_WAVE PROTECT', nat='TIMID', ev='hp252 spe252 spa6', ab='PLUS'),
                       M('MINUN', 'HELPING_HAND THUNDERBOLT ENCORE PROTECT', nat='TIMID', ev='hp252 spe252 spa6', ab='MINUS'),
                       M('KYOGRE', 'WATER_SPOUT SURF ICE_BEAM PROTECT', nat='MODEST', ev='hp6 spe252 spa252', ab='DRIZZLE'),
                       M('HARIYAMA', 'HELPING_HAND FAKE_OUT CROSS_CHOP ROCK_SLIDE', nat='ADAMANT', ev='hp252 atk252 spe6', ab='THICK_FAT')],
  'Plusle/Minun Helping Hand a Kyogre Water Spout in rain (both get Plus/Minus boosts next to each other). Hariyama Fake Outs the faster threat on turn 1 so Kyogre fires at full HP.', tier='ubers', **D)
T('dbl_follow_me', [M('CLEFABLE', 'FOLLOW_ME SOFT_BOILED THUNDER_WAVE SEISMIC_TOSS', nat='BOLD', ev='hp252 def252 spd6', ab='CUTE_CHARM'),
                    M('SALAMENCE', 'DRAGON_DANCE EARTHQUAKE ROCK_SLIDE AERIAL_ACE', nat='ADAMANT', ev='hp6 atk252 spe252', ab='INTIMIDATE'),
                    M('FURRET', 'FOLLOW_ME RETURN SHADOW_BALL QUICK_ATTACK', nat='JOLLY', ev='hp252 atk6 spe252', ab='KEEN_EYE'),
                    M('SNORLAX', 'BELLY_DRUM RETURN EARTHQUAKE SHADOW_BALL', nat='ADAMANT', ev='hp112 atk252 def144', ab='THICK_FAT')],
  'Follow Me doubles: Clefable (or Furret) draws every attack while the partner sets up. Lead Clefable + Salamence: Follow Me, Dragon Dance, then Salamence sweeps; Furret + Belly Drum Snorlax repeats it.', **D)
T('dbl_follow_me', [M('TOGETIC', 'FOLLOW_ME WISH PROTECT SEISMIC_TOSS', nat='CALM', ev='hp252 def92 spd164', ab='SERENE_GRACE'),
                    M('TYRANITAR', 'DRAGON_DANCE ROCK_SLIDE EARTHQUAKE PROTECT', nat='ADAMANT', ev='hp16 atk252 spe240', ab='SAND_STREAM'),
                    M('CLEFABLE', 'FOLLOW_ME ENCORE SOFT_BOILED SHADOW_BALL', nat='BOLD', ev='hp252 def252 spd6', ab='CUTE_CHARM'),
                    M('GENGAR', 'THUNDERBOLT GIGA_DRAIN HYPNOSIS PROTECT', nat='TIMID', ev='hp6 spe252 spa252', ab='LEVITATE'),
                    M('SUICUNE', 'CALM_MIND SURF ICE_BEAM PROTECT', nat='BOLD', ev='hp252 def252 spe6', ab='PRESSURE'),
                    M('METAGROSS', 'METEOR_MASH EARTHQUAKE ROCK_SLIDE PROTECT', nat='ADAMANT', ev='hp152 atk252 spe104', ab='CLEAR_BODY')],
  'Togetic/Clefable Follow Me redirects attacks while Tyranitar or Suicune boost; Gengar (Levitate) is the Earthquake-immune partner for Tyranitar. Win by a boosted spread-move sweep.', **D)
T('dbl_skill_swap', [M('SLAKING', 'RETURN EARTHQUAKE SHADOW_BALL PROTECT', nat='ADAMANT', ev='hp6 atk252 spe252', ab='TRUANT'),
                     GARDEVOIR_SWAP(), M('ALAKAZAM', 'SKILL_SWAP PSYCHIC FIRE_PUNCH PROTECT', nat='TIMID', ev='hp6 spe252 spa252', ab='SYNCHRONIZE'),
                     M('METAGROSS', 'METEOR_MASH EARTHQUAKE EXPLOSION PROTECT', nat='ADAMANT', ev='hp152 atk252 spe104', ab='CLEAR_BODY')],
  'Skill Swap Slaking: Gardevoir/Alakazam Skill Swaps Truant off Slaking on turn 1 (Slaking Protects), then Slaking attacks every turn with 160 base Attack. Swap Truant onto an opponent when possible.', **D)
T('dbl_skill_swap', [M('SLAKING', 'RETURN EARTHQUAKE SHADOW_BALL FOCUS_PUNCH', nat='ADAMANT', ev='hp6 atk252 spe252', ab='TRUANT'),
                     M('CLAYDOL', 'SKILL_SWAP EARTHQUAKE PSYCHIC PROTECT', nat='RELAXED', ev='hp252 def92 spd164', ab='LEVITATE'),
                     M('DUSCLOPS', 'SKILL_SWAP WILL_O_WISP SEISMIC_TOSS PROTECT', nat='CAREFUL', ev='hp252 def100 spd156', ab='PRESSURE'),
                     M('SHEDINJA', 'SHADOW_BALL PROTECT TOXIC SILVER_WIND', nat='ADAMANT', ev='atk252 spe252', ab='WONDER_GUARD'),
                     M('SLOWKING', 'SKILL_SWAP SURF PSYCHIC PROTECT', nat='BOLD', ev='hp252 def252 spd6', ab='OWN_TEMPO'),
                     M('SALAMENCE', 'DRAGON_DANCE EARTHQUAKE ROCK_SLIDE PROTECT', nat='ADAMANT', ev='hp6 atk252 spe252', ab='INTIMIDATE')],
  'Claydol Skill Swaps Truant off Slaking and gains Truant itself (it Levitates over Slaking Earthquake first); Dusclops/Slowking can swap Levitate or Pressure onto partners. Slaking then spams Return.', **D)
T('dbl_eq_levitate', [M('SWAMPERT', 'EARTHQUAKE SURF ICE_BEAM PROTECT', nat='RELAXED', ev='hp252 def216 spd40', ab='TORRENT'),
                      M('GENGAR', 'THUNDERBOLT GIGA_DRAIN PSYCHIC PROTECT', nat='TIMID', ev='hp6 spe252 spa252', ab='LEVITATE'),
                      M('TYRANITAR', 'EARTHQUAKE ROCK_SLIDE DRAGON_DANCE PROTECT', nat='ADAMANT', ev='hp16 atk252 spe240', ab='SAND_STREAM'),
                      M('FLYGON', 'EARTHQUAKE ROCK_SLIDE DRAGON_CLAW PROTECT', nat='ADAMANT', ev='hp6 atk252 spe252', ab='LEVITATE'),
                      M('ZAPDOS', 'THUNDERBOLT DRILL_PECK HIDDEN_POWER PROTECT', nat='MODEST', ev='hp6 spe252 spa252', ab='PRESSURE'),
                      M('CLAYDOL', 'EARTHQUAKE PSYCHIC ICE_BEAM EXPLOSION', nat='RELAXED', ev='hp252 def92 spd164', ab='LEVITATE')],
  'Earthquake spam next to a Levitate/Flying partner: Swampert or Tyranitar Earthquakes every turn while Gengar, Flygon, Zapdos or Claydol are immune and attack too. Lead Swampert + Gengar.', **D)
T('dbl_eq_levitate', [M('DUGTRIO', 'EARTHQUAKE ROCK_SLIDE AERIAL_ACE PROTECT', nat='JOLLY', ev='hp6 atk252 spe252', ab='ARENA_TRAP'),
                      M('AERODACTYL', 'ROCK_SLIDE EARTHQUAKE DOUBLE_EDGE PROTECT', nat='JOLLY', ev='hp6 atk252 spe252', ab='ROCK_HEAD'),
                      M('MAROWAK', 'EARTHQUAKE BONEMERANG ROCK_SLIDE PROTECT', nat='ADAMANT', ev='hp252 atk252 spe6', ab='ROCK_HEAD', item='THICK_CLUB'),
                      M('WEEZING', 'SLUDGE_BOMB WILL_O_WISP FIRE_BLAST PROTECT', nat='BOLD', ev='hp252 def252 spa6', ab='LEVITATE')],
  'Fast Earthquake/Rock Slide spam: Dugtrio + Aerodactyl lead (Aerodactyl is immune to Earthquake and flinches with Rock Slide), Marowak Thick Club Earthquake with Weezing Levitating beside it.', **D)
T('dbl_spread_weather', [M('KYOGRE', 'SURF ICE_BEAM THUNDER PROTECT', nat='MODEST', ev='hp252 spa252 spd6', ab='DRIZZLE'),
                         M('LUDICOLO', 'SURF ICE_BEAM GIGA_DRAIN PROTECT', nat='MODEST', ev='hp6 spe252 spa252', ab='SWIFT_SWIM'),
                         M('LANTURN', 'SURF THUNDER ICE_BEAM PROTECT', nat='MODEST', ev='hp252 spa252 spd6', ab='VOLT_ABSORB'),
                         M('LAPRAS', 'SURF ICE_BEAM THUNDER PROTECT', nat='MODEST', ev='hp252 spa252 spd6', ab='WATER_ABSORB')],
  'Rain Surf spam: Kyogre Drizzle makes Surf 1.5x and Thunder never miss; Ludicolo doubles Speed and Lapras/Lanturn absorb the partner Surf with Water Absorb (or resist it). Lead Kyogre + Ludicolo and Surf every turn.', tier='ubers', **D)
T('dbl_spread_weather', [M('GROUDON', 'EARTHQUAKE ROCK_SLIDE OVERHEAT PROTECT', nat='ADAMANT', ev='hp252 atk252 spe6', ab='DROUGHT'),
                         M('TORKOAL', 'ERUPTION HEAT_WAVE EXPLOSION PROTECT', nat='QUIET', ev='hp252 spa252 spd6', ab='WHITE_SMOKE'),
                         M('CHARIZARD', 'HEAT_WAVE FIRE_BLAST DRAGON_CLAW PROTECT', nat='MODEST', ev='hp6 spe252 spa252', ab='BLAZE'),
                         M('EXEGGUTOR', 'SOLAR_BEAM PSYCHIC SLEEP_POWDER PROTECT', nat='MODEST', ev='hp6 spe252 spa252', ab='CHLOROPHYLL')],
  'Sun spread: Groudon Drought powers Torkoal Eruption and Charizard Heat Wave (both hit both foes) and makes Exeggutor Solar Beam instant. Charizard/Exeggutor are immune to or resist Groudon Earthquake.', tier='ubers', **D)
T('dbl_spread_weather', [M('TYRANITAR', 'ROCK_SLIDE EARTHQUAKE CRUNCH PROTECT', nat='ADAMANT', ev='hp252 atk252 spe6', ab='SAND_STREAM'),
                         M('AERODACTYL', 'ROCK_SLIDE EARTHQUAKE AERIAL_ACE PROTECT', nat='JOLLY', ev='hp6 atk252 spe252', ab='ROCK_HEAD'),
                         M('REGIROCK', 'ROCK_SLIDE EARTHQUAKE EXPLOSION PROTECT', nat='ADAMANT', ev='hp252 atk252 spd6', ab='CLEAR_BODY'),
                         M('CACTURNE', 'NEEDLE_ARM LEECH_SEED DESTINY_BOND PROTECT', nat='JOLLY', ev='hp252 atk6 spe252', ab='SAND_VEIL')],
  'Sandstorm doubles: Tyranitar sets sand which chips both opponents; the team is all sand-immune Rock types plus Sand Veil Cacturne. Rock Slide from two attackers flinches both foes each turn.', **D)

# ---------------------------------------------------------------------------------------------------
# Playthrough-level ("midgame") versions: level 30-50, Kanto-obtainable Pokemon, modest EVs
# ---------------------------------------------------------------------------------------------------
MID = 'hp80 atk80 def40 spe80'
MIDS = 'hp80 def40 spe80 spa80'
T('curselax', [M('SNORLAX', 'CURSE BODY_SLAM SHADOW_BALL REST', nat='CAREFUL', ev='hp100 atk60 spd100', ab='THICK_FAT', item='CHESTO_BERRY', level=40),
               M('KADABRA', 'PSYCHIC REFLECT THUNDER_WAVE RECOVER', nat='TIMID', ev=MIDS, ab='SYNCHRONIZE', level=38, item='TWISTED_SPOON'),
               M('ARCANINE', 'FLAMETHROWER AERIAL_ACE ROAR CRUNCH', nat='ADAMANT', ev=MID, ab='INTIMIDATE', level=40, item='CHARCOAL'),
               M('GYARADOS', 'SURF ICE_BEAM RETURN THUNDER_WAVE', nat='ADAMANT', ev=MID, ab='INTIMIDATE', level=40, item='MYSTIC_WATER'),
               M('DUGTRIO', 'EARTHQUAKE ROCK_SLIDE AERIAL_ACE SLASH', nat='JOLLY', ev=MID, ab='ARENA_TRAP', level=40, item='SOFT_SAND')],
  'Playthrough Curselax: Snorlax (Route 12/16, L30) with egg-move Curse. Lead Kadabra to Reflect and paralyse, then Snorlax Curses and sweeps, Chesto Berry for one Rest. Dugtrio traps Fighting types.', level=40)
T('belly_drum', [M('CHARIZARD', 'BELLY_DRUM EARTHQUAKE ROCK_SLIDE AERIAL_ACE', nat='JOLLY', ev=MID, ab='BLAZE', item='SALAC_BERRY', level=42),
                 M('SNORLAX', 'BELLY_DRUM RETURN EARTHQUAKE REST', nat='ADAMANT', ev='hp100 atk100 def60', ab='THICK_FAT', item='CHESTO_BERRY', level=42),
                 M('HYPNO', 'HYPNOSIS PSYCHIC REFLECT LIGHT_SCREEN', nat='CALM', ev=MIDS, ab='INSOMNIA', level=40),
                 M('POLIWRATH', 'BELLY_DRUM HYPNOSIS BRICK_BREAK SURF', nat='ADAMANT', ev=MID, ab='WATER_ABSORB', item='LEFTOVERS', level=42),
                 M('MAGNETON', 'THUNDERBOLT THUNDER_WAVE HIDDEN_POWER SUPERSONIC', nat='MODEST', ev=MIDS, ab='MAGNET_PULL', item='MAGNET', level=40)],
  'Playthrough Belly Drum: Hypno leads, sleeps the lead and sets screens; Charizard (egg-move Belly Drum) or Snorlax drums behind them and sweeps. Poliwrath Hypnosis-Drums as backup.', level=42)
T('dragon_dance', [M('GYARADOS', 'DRAGON_DANCE HIDDEN_POWER EARTHQUAKE RETURN', nat='ADAMANT', ev=MID, ab='INTIMIDATE', item='LEFTOVERS', level=50),
                   M('CHARIZARD', 'DRAGON_DANCE AERIAL_ACE EARTHQUAKE FLAMETHROWER', nat='ADAMANT', ev=MID, ab='BLAZE', item='LUM_BERRY', level=50),
                   M('DRAGONAIR', 'DRAGON_DANCE RETURN THUNDERBOLT THUNDER_WAVE', nat='ADAMANT', ev=MID, ab='SHED_SKIN', item='LEFTOVERS', level=50),
                   M('MAGNETON', 'THUNDERBOLT THUNDER_WAVE HIDDEN_POWER PROTECT', nat='MODEST', ev=MIDS, ab='MAGNET_PULL', item='MAGNET', level=48),
                   M('GOLEM', 'EARTHQUAKE ROCK_SLIDE EXPLOSION COUNTER', nat='ADAMANT', ev=MID, ab='STURDY', item='HARD_STONE', level=48),
                   M('HYPNO', 'HYPNOSIS PSYCHIC THUNDER_WAVE REFLECT', nat='CALM', ev=MIDS, ab='INSOMNIA', level=48)],
  'Playthrough Dragon Dance: Gyarados learns DD at L50, Charizard/Dragonair via egg moves. Hypno leads and sleeps; a dancer sets up once and sweeps. Magneton kills the Steel types that wall Gyarados.', level=50)
T('swords_dance', [M('SCYTHER', 'SWORDS_DANCE AERIAL_ACE SLASH AGILITY', nat='JOLLY', ev=MID, ab='SWARM', item='SILVER_POWDER', level=40),
                   M('PINSIR', 'SWORDS_DANCE BRICK_BREAK RETURN ROCK_TOMB', nat='ADAMANT', ev=MID, ab='HYPER_CUTTER', item='LEFTOVERS', level=40),
                   M('SANDSLASH', 'SWORDS_DANCE EARTHQUAKE ROCK_SLIDE AERIAL_ACE', nat='ADAMANT', ev=MID, ab='SAND_VEIL', item='SOFT_SAND', level=40),
                   M('KADABRA', 'PSYCHIC THUNDER_WAVE REFLECT RECOVER', nat='TIMID', ev=MIDS, ab='SYNCHRONIZE', item='TWISTED_SPOON', level=40),
                   M('VENUSAUR', 'SWORDS_DANCE SLUDGE_BOMB SLEEP_POWDER EARTHQUAKE', nat='ADAMANT', ev=MID, ab='OVERGROW', item='LEFTOVERS', level=40)],
  'Playthrough Swords Dance: Scyther/Pinsir/Sandslash boost once (Swords Dance is an FRLG tutor move) and sweep; Kadabra paralyses first to give the setup turn. Venusaur sleeps then dances.', level=40)
T('sub_punch', [M('MACHOKE', 'SUBSTITUTE FOCUS_PUNCH ROCK_SLIDE CROSS_CHOP', nat='ADAMANT', ev=MID, ab='GUTS', item='LEFTOVERS', level=40),
                M('PRIMEAPE', 'SUBSTITUTE FOCUS_PUNCH ROCK_SLIDE BULK_UP', nat='JOLLY', ev=MID, ab='VITAL_SPIRIT', item='LEFTOVERS', level=40),
                M('HAUNTER', 'HYPNOSIS THUNDERBOLT SHADOW_BALL SUBSTITUTE', nat='TIMID', ev=MIDS, ab='LEVITATE', item='SPELL_TAG', level=40),
                M('SNORLAX', 'SUBSTITUTE FOCUS_PUNCH BODY_SLAM SHADOW_BALL', nat='ADAMANT', ev='hp100 atk100 def60', ab='THICK_FAT', item='LEFTOVERS', level=40),
                M('STARMIE', 'SURF THUNDERBOLT RECOVER THUNDER_WAVE', nat='TIMID', ev=MIDS, ab='NATURAL_CURE', item='MYSTIC_WATER', level=40)],
  'Playthrough Substitute + Focus Punch (TM01 from Silph Co.): Haunter leads and sleeps something, then Machoke/Primeape/Snorlax Substitute on the switch and Focus Punch from behind it.', level=40)
T('sun', [M('EXEGGUTOR', 'SUNNY_DAY SOLAR_BEAM PSYCHIC SLEEP_POWDER', nat='MODEST', ev=MIDS, ab='CHLOROPHYLL', item='MIRACLE_SEED', level=40),
          M('VICTREEBEL', 'SUNNY_DAY SOLAR_BEAM SLUDGE_BOMB SLEEP_POWDER', nat='MODEST', ev=MIDS, ab='CHLOROPHYLL', item='LEFTOVERS', level=40),
          M('CHARIZARD', 'SUNNY_DAY FLAMETHROWER AERIAL_ACE DRAGON_CLAW', nat='MODEST', ev=MIDS, ab='BLAZE', item='CHARCOAL', level=40),
          M('NINETALES', 'SUNNY_DAY FLAMETHROWER WILL_O_WISP CONFUSE_RAY', nat='TIMID', ev=MIDS, ab='FLASH_FIRE', item='LEFTOVERS', level=40),
          M('TANGELA', 'SUNNY_DAY SOLAR_BEAM STUN_SPORE SLEEP_POWDER', nat='MODEST', ev=MIDS, ab='CHLOROPHYLL', item='LEFTOVERS', level=38)],
  'Playthrough sun: Charizard or Ninetales leads and sets Sunny Day (TM11); Exeggutor, Victreebel and Tangela then outspeed everything with Chlorophyll and fire instant Solar Beams (TM22).', level=40)
T('rain', [M('KABUTOPS', 'RAIN_DANCE SURF ROCK_SLIDE SWORDS_DANCE', nat='ADAMANT', ev=MID, ab='SWIFT_SWIM', item='MYSTIC_WATER', level=41),
           M('SEAKING', 'RAIN_DANCE SURF ICE_BEAM RETURN', nat='NAUGHTY', ev=MID, ab='SWIFT_SWIM', item='LEFTOVERS', level=41),
           M('MAGNETON', 'RAIN_DANCE THUNDER THUNDER_WAVE HIDDEN_POWER', nat='MODEST', ev=MIDS, ab='MAGNET_PULL', item='MAGNET', level=40),
           M('OMASTAR', 'RAIN_DANCE SURF ICE_BEAM PROTECT', nat='MODEST', ev=MIDS, ab='SWIFT_SWIM', item='LEFTOVERS', level=40),
           M('STARMIE', 'RAIN_DANCE SURF THUNDER RECOVER', nat='TIMID', ev=MIDS, ab='NATURAL_CURE', item='LEFTOVERS', level=40)],
  'Playthrough rain: Magneton leads, Rain Dance (TM18) then 100 percent Thunder; Kabutops, Seaking and Omastar double their Speed with Swift Swim and Surf everything (Seadra has Poison Point in Gen 3, not Swift Swim). Re-set rain with Starmie.', level=41)
T('boom', [M('ELECTRODE', 'THUNDERBOLT EXPLOSION THUNDER_WAVE LIGHT_SCREEN', nat='HASTY', ev=MIDS, ab='SOUNDPROOF', item='MAGNET', level=40),
           M('GOLEM', 'EARTHQUAKE ROCK_SLIDE EXPLOSION COUNTER', nat='ADAMANT', ev=MID, ab='STURDY', item='HARD_STONE', level=40),
           M('WEEZING', 'SLUDGE_BOMB EXPLOSION WILL_O_WISP FIRE_BLAST', nat='BOLD', ev='hp100 def100 spa60', ab='LEVITATE', item='LEFTOVERS', level=40),
           M('EXEGGUTOR', 'SLEEP_POWDER PSYCHIC EXPLOSION SOLAR_BEAM', nat='MILD', ev=MIDS, ab='CHLOROPHYLL', item='LEFTOVERS', level=40),
           M('HAUNTER', 'HYPNOSIS SHADOW_BALL THUNDERBOLT EXPLOSION', nat='HASTY', ev=MIDS, ab='LEVITATE', item='SPELL_TAG', level=40),
           M('SNORLAX', 'BODY_SLAM EARTHQUAKE SHADOW_BALL REST', nat='ADAMANT', ev='hp100 atk100 def60', ab='THICK_FAT', item='LEFTOVERS', level=40)],
  'Playthrough boom: Electrode leads (fastest Explosion), Golem/Weezing/Exeggutor/Haunter each blow up on the Pokemon that walls them. Snorlax cleans the remains.', level=40)
T('perish_trap', [M('HAUNTER', 'MEAN_LOOK PERISH_SONG PROTECT SHADOW_BALL', nat='TIMID', ev='hp100 spe100 spd60', ab='LEVITATE', item='LEFTOVERS', level=48),
                  M('GOLBAT', 'MEAN_LOOK TOXIC CONFUSE_RAY AERIAL_ACE', nat='JOLLY', ev=MID, ab='INNER_FOCUS', item='LEFTOVERS', level=45),
                  M('WIGGLYTUFF', 'PERISH_SONG PROTECT BODY_SLAM THUNDER_WAVE', nat='BOLD', ev='hp100 def100 spd60', ab='CUTE_CHARM', item='LEFTOVERS', level=45),
                  M('DEWGONG', 'PERISH_SONG PROTECT SURF ICE_BEAM', nat='CALM', ev='hp100 def60 spd100', ab='THICK_FAT', item='LEFTOVERS', level=45),
                  M('CHANSEY', 'SOFT_BOILED SEISMIC_TOSS THUNDER_WAVE TOXIC', nat='BOLD', ev='hp100 def160', ab='NATURAL_CURE', item='LEFTOVERS', level=45)],
  'Playthrough Perish trapping: Haunter (Mean Look at L48, egg-move Perish Song) traps one Pokemon, Perish Songs, Protects until the count hits 0, then switches out. Golbat Mean Look + Toxic stalls the same way; Chansey holds the line.', level=48)
T('suicide_lead', [M('WARTORTLE', 'MIRROR_COAT COUNTER SURF PROTECT', nat='BOLD', ev='hp100 def80 spd80', ab='TORRENT', item='LEFTOVERS', level=35),
                   M('CHANSEY', 'COUNTER SOFT_BOILED SEISMIC_TOSS TOXIC', nat='BOLD', ev='hp100 def160', ab='NATURAL_CURE', item='LEFTOVERS', level=35),
                   M('MACHOKE', 'COUNTER BRICK_BREAK ROCK_TOMB BULK_UP', nat='ADAMANT', ev=MID, ab='GUTS', item='LEFTOVERS', level=35),
                   M('TENTACRUEL', 'MIRROR_COAT SURF TOXIC SLUDGE_BOMB', nat='CALM', ev='hp100 def40 spd120', ab='CLEAR_BODY', item='LEFTOVERS', level=35),
                   M('KADABRA', 'PSYCHIC THUNDER_WAVE RECOVER REFLECT', nat='TIMID', ev=MIDS, ab='SYNCHRONIZE', item='TWISTED_SPOON', level=35)],
  'Playthrough Counter/Mirror Coat: Wartortle leads and reflects the first big hit for double damage (Counter for physical, Mirror Coat for special; both are FRLG tutor/level moves). Chansey and Machoke Counter, Tentacruel Mirror Coats.', level=35)
T('toxic_protect', [M('TENTACRUEL', 'TOXIC PROTECT SURF SLUDGE_BOMB', nat='CALM', ev='hp100 def40 spd120', ab='LIQUID_OOZE', item='LEFTOVERS', level=40),
                    M('CHANSEY', 'TOXIC PROTECT SOFT_BOILED SEISMIC_TOSS', nat='BOLD', ev='hp100 def160', ab='NATURAL_CURE', item='LEFTOVERS', level=40),
                    M('HYPNO', 'TOXIC PROTECT PSYCHIC REST', nat='CALM', ev='hp100 def60 spd100', ab='INSOMNIA', item='LEFTOVERS', level=40),
                    M('WEEZING', 'TOXIC PROTECT SLUDGE_BOMB WILL_O_WISP', nat='BOLD', ev='hp100 def100 spa60', ab='LEVITATE', item='LEFTOVERS', level=40),
                    M('CLOYSTER', 'TOXIC PROTECT SURF SPIKES', nat='RELAXED', ev='hp100 def60 spd100', ab='SHELL_ARMOR', item='LEFTOVERS', level=40)],
  'Playthrough Toxic stall: Toxic (TM06) and Protect (TM17) on every wall; Cloyster lays Spikes (L36). Poison the switch-in, Protect on the damage turns, switch to the wall that resists its attack. Win on poison damage.', level=40)
T('dual_screens', [M('KADABRA', 'REFLECT LIGHT_SCREEN PSYCHIC THUNDER_WAVE', nat='TIMID', ev=MIDS, ab='SYNCHRONIZE', item='LEFTOVERS', level=38),
                   M('MACHOKE', 'BULK_UP BRICK_BREAK ROCK_TOMB EARTHQUAKE', nat='ADAMANT', ev=MID, ab='GUTS', item='LEFTOVERS', level=38),
                   M('SNORLAX', 'CURSE BODY_SLAM EARTHQUAKE REST', nat='CAREFUL', ev='hp100 atk60 spd100', ab='THICK_FAT', item='CHESTO_BERRY', level=38),
                   M('HYPNO', 'REFLECT LIGHT_SCREEN HYPNOSIS PSYCHIC', nat='CALM', ev=MIDS, ab='INSOMNIA', item='LEFTOVERS', level=38),
                   M('GYARADOS', 'RETURN EARTHQUAKE SURF THUNDER_WAVE', nat='ADAMANT', ev=MID, ab='INTIMIDATE', item='LEFTOVERS', level=38)],
  'Playthrough dual screens: Kadabra leads with Reflect (TM33) + Light Screen (TM16), then Machoke Bulk Ups or Snorlax Curses behind the screens and sweeps. Hypno re-sets screens later.', level=38)
T('spikes_phaze', [M('CLOYSTER', 'SPIKES SURF ICE_BEAM EXPLOSION', nat='RELAXED', ev='hp100 def60 spd100', ab='SHELL_ARMOR', item='LEFTOVERS', level=40),
                   M('ARCANINE', 'ROAR FLAMETHROWER AERIAL_ACE CRUNCH', nat='ADAMANT', ev=MID, ab='INTIMIDATE', item='CHARCOAL', level=40),
                   M('GOLEM', 'ROAR EARTHQUAKE ROCK_SLIDE EXPLOSION', nat='ADAMANT', ev=MID, ab='STURDY', item='LEFTOVERS', level=40),
                   M('HAUNTER', 'SHADOW_BALL THUNDERBOLT HYPNOSIS TAUNT', nat='TIMID', ev=MIDS, ab='LEVITATE', item='SPELL_TAG', level=40),
                   M('PIDGEOT', 'WHIRLWIND AERIAL_ACE RETURN STEEL_WING', nat='JOLLY', ev=MID, ab='KEEN_EYE', item='SHARP_BEAK', level=40),
                   M('CHANSEY', 'SOFT_BOILED SEISMIC_TOSS TOXIC THUNDER_WAVE', nat='BOLD', ev='hp100 def160', ab='NATURAL_CURE', item='LEFTOVERS', level=40)],
  'Playthrough Spikes + phazing: Cloyster (Spikes at L36) leads and lays layers; Arcanine/Golem Roar and Pidgeot Whirlwinds to make the opponent switch through Spikes. Haunter blocks Rapid Spin.', level=40)
T('baton_pass', [M('SCYTHER', 'SWORDS_DANCE AGILITY BATON_PASS AERIAL_ACE', nat='JOLLY', ev=MID, ab='SWARM', item='LEFTOVERS', level=40),
                 M('VENOMOTH', 'BATON_PASS SLEEP_POWDER SUBSTITUTE PSYCHIC', nat='TIMID', ev=MIDS, ab='SHIELD_DUST', item='LEFTOVERS', level=40),
                 M('MR_MIME', 'CALM_MIND PSYCHIC THUNDERBOLT SUBSTITUTE', nat='TIMID', ev=MIDS, ab='SOUNDPROOF', item='LEFTOVERS', level=40),
                 M('SNORLAX', 'BODY_SLAM EARTHQUAKE SHADOW_BALL REST', nat='ADAMANT', ev='hp100 atk100 def60', ab='THICK_FAT', item='LEFTOVERS', level=40),
                 M('PRIMEAPE', 'CROSS_CHOP ROCK_SLIDE BULK_UP EARTHQUAKE', nat='JOLLY', ev=MID, ab='VITAL_SPIRIT', item='LEFTOVERS', level=40),
                 M('STARMIE', 'SURF THUNDERBOLT PSYCHIC RECOVER', nat='TIMID', ev=MIDS, ab='NATURAL_CURE', item='LEFTOVERS', level=40)],
  'Playthrough Baton Pass: Scyther (egg-move Baton Pass) leads, Swords Dance + Agility, passes to Snorlax or Primeape; Venomoth sleeps then passes a Substitute. Win with a +2/+2 Snorlax Body Slam sweep.', level=40)
T('rest_talk', [M('SNORLAX', 'REST SLEEP_TALK BODY_SLAM CURSE', nat='CAREFUL', ev='hp100 atk60 spd100', ab='THICK_FAT', item='LEFTOVERS', level=40),
                M('LAPRAS', 'REST SLEEP_TALK SURF ICE_BEAM', nat='CALM', ev='hp100 def60 spd100', ab='WATER_ABSORB', item='LEFTOVERS', level=40),
                M('SLOWBRO', 'REST SLEEP_TALK SURF PSYCHIC', nat='BOLD', ev='hp100 def100 spd60', ab='OWN_TEMPO', item='LEFTOVERS', level=40),
                M('LICKITUNG', 'REST SLEEP_TALK BODY_SLAM EARTHQUAKE', nat='CAREFUL', ev='hp100 def60 spd100', ab='OWN_TEMPO', item='LEFTOVERS', level=40),
                M('MAGNETON', 'THUNDERBOLT THUNDER_WAVE HIDDEN_POWER REST', nat='MODEST', ev=MIDS, ab='MAGNET_PULL', item='MAGNET', level=40)],
  'Playthrough Rest + Sleep Talk (Sleep Talk is a level-up move for Snorlax L37, Lapras, Slowbro, Lickitung): Rest at half HP, keep attacking through Sleep Talk. Snorlax Curses; win by never running out of HP.', level=40)
T('endure_berry', [M('KABUTOPS', 'ENDURE FLAIL ROCK_SLIDE SURF', nat='ADAMANT', ev=MID, ab='BATTLE_ARMOR', item='SALAC_BERRY', level=40),
                   M('FLAREON', 'ENDURE FLAIL FLAMETHROWER SHADOW_BALL', nat='ADAMANT', ev=MID, ab='FLASH_FIRE', item='SALAC_BERRY', level=40),
                   M('HITMONLEE', 'ENDURE BRICK_BREAK ROCK_SLIDE FACADE', nat='ADAMANT', ev=MID, ab='LIMBER', item='SALAC_BERRY', level=41),
                   M('HAUNTER', 'HYPNOSIS SHADOW_BALL THUNDERBOLT GIGA_DRAIN', nat='TIMID', ev=MIDS, ab='LEVITATE', item='SPELL_TAG', level=40),
                   M('JOLTEON', 'ENDURE FLAIL THUNDERBOLT HIDDEN_POWER', nat='JOLLY', ev=MID, ab='VOLT_ABSORB', item='SALAC_BERRY', level=40)],
  'Playthrough Endure + Salac + Flail: Kabutops (Endure at L37), Flareon and Jolteon (egg-move Endure/Flail) Endure a killing hit, Salac gives +1 Speed, Flail at 1 HP does 200 base power. Haunter sleeps the priority users first.', level=41)
T('wish_pass', [M('VAPOREON', 'WISH PROTECT SURF ICE_BEAM', nat='BOLD', ev='hp100 def100 spa60', ab='WATER_ABSORB', item='LEFTOVERS', level=40),
                M('CLEFABLE', 'WISH PROTECT SEISMIC_TOSS THUNDER_WAVE', nat='BOLD', ev='hp100 def100 spd60', ab='CUTE_CHARM', item='LEFTOVERS', level=40),
                M('JOLTEON', 'WISH THUNDERBOLT BATON_PASS HIDDEN_POWER', nat='TIMID', ev=MIDS, ab='VOLT_ABSORB', item='MAGNET', level=40),
                M('SNORLAX', 'CURSE BODY_SLAM EARTHQUAKE SHADOW_BALL', nat='CAREFUL', ev='hp100 atk60 spd100', ab='THICK_FAT', item='LEFTOVERS', level=40),
                M('MACHOKE', 'BRICK_BREAK ROCK_TOMB BULK_UP EARTHQUAKE', nat='ADAMANT', ev=MID, ab='GUTS', item='LEFTOVERS', level=40)],
  'Playthrough Wish passing: Vaporeon/Clefable/Jolteon (egg-move Wish) Wish and switch to the Pokemon that needs healing; Jolteon can Baton Pass the Wish directly. Curselax without Rest is the receiver.', level=40)
T('calm_mind', [M('ALAKAZAM', 'CALM_MIND PSYCHIC FIRE_PUNCH RECOVER', nat='TIMID', ev=MIDS, ab='SYNCHRONIZE', item='TWISTED_SPOON', level=45),
                M('HYPNO', 'CALM_MIND PSYCHIC HYPNOSIS REST', nat='CALM', ev=MIDS, ab='INSOMNIA', item='LEFTOVERS', level=45),
                M('SLOWBRO', 'CALM_MIND SURF PSYCHIC REST', nat='BOLD', ev='hp100 def100 spd60', ab='OWN_TEMPO', item='LEFTOVERS', level=45),
                M('CLEFABLE', 'CALM_MIND ICE_BEAM THUNDERBOLT SOFT_BOILED', nat='BOLD', ev='hp100 def100 spa60', ab='CUTE_CHARM', item='LEFTOVERS', level=45),
                M('DUGTRIO', 'EARTHQUAKE ROCK_SLIDE AERIAL_ACE SLASH', nat='JOLLY', ev=MID, ab='ARENA_TRAP', item='SOFT_SAND', level=45)],
  'Playthrough Calm Mind (TM04 from Saffron Gym): Alakazam boosts and Recovers, Hypno sleeps then boosts, Slowbro/Clefable boost and Rest/Soft-Boiled. Dugtrio traps the Dark/Steel types that wall Psychics.', level=45)
T('choice_band', [M('TAUROS', 'RETURN EARTHQUAKE IRON_TAIL HIDDEN_POWER', nat='JOLLY', ev=MID, ab='INTIMIDATE', item='CHOICE_BAND', level=45),
                  M('DODRIO', 'DRILL_PECK RETURN QUICK_ATTACK STEEL_WING', nat='JOLLY', ev=MID, ab='EARLY_BIRD', item='CHOICE_BAND', level=45),
                  M('MACHAMP', 'CROSS_CHOP ROCK_SLIDE EARTHQUAKE FACADE', nat='ADAMANT', ev=MID, ab='GUTS', item='CHOICE_BAND', level=45),
                  M('SNORLAX', 'RETURN EARTHQUAKE SHADOW_BALL FOCUS_PUNCH', nat='ADAMANT', ev='hp100 atk100 def60', ab='THICK_FAT', item='CHOICE_BAND', level=45),
                  M('KADABRA', 'PSYCHIC THUNDER_WAVE RECOVER REFLECT', nat='TIMID', ev=MIDS, ab='SYNCHRONIZE', item='TWISTED_SPOON', level=45),
                  M('STARMIE', 'SURF THUNDERBOLT PSYCHIC RECOVER', nat='TIMID', ev=MIDS, ab='NATURAL_CURE', item='LEFTOVERS', level=45)],
  'Playthrough Choice Band (Trainer Tower prize): Tauros/Dodrio/Machamp/Snorlax hit one move at 1.5x and switch out. Kadabra paralyses fast threats; the Banders then pick off everything with the right move.', level=45)
T('taunt_lead', [M('HAUNTER', 'TAUNT HYPNOSIS SHADOW_BALL THUNDERBOLT', nat='TIMID', ev=MIDS, ab='LEVITATE', item='SPELL_TAG', level=40),
                 M('PERSIAN', 'TAUNT HYPNOSIS RETURN SHADOW_BALL', nat='JOLLY', ev=MID, ab='LIMBER', item='SILK_SCARF', level=40),
                 M('PRIMEAPE', 'TAUNT CROSS_CHOP ROCK_SLIDE BULK_UP', nat='JOLLY', ev=MID, ab='VITAL_SPIRIT', item='LEFTOVERS', level=40),
                 M('SNORLAX', 'CURSE BODY_SLAM EARTHQUAKE REST', nat='CAREFUL', ev='hp100 atk60 spd100', ab='THICK_FAT', item='CHESTO_BERRY', level=40),
                 M('GYARADOS', 'TAUNT SURF RETURN EARTHQUAKE', nat='ADAMANT', ev=MID, ab='INTIMIDATE', item='LEFTOVERS', level=40)],
  'Playthrough anti-stall: Haunter/Persian/Primeape Taunt (TM12) the trainer walls so they cannot Rest, Toxic or set screens; Curselax then wins the resulting attack war.', level=40)
T('pp_stall', [M('ZAPDOS', 'SUBSTITUTE PROTECT TOXIC THUNDERBOLT', nat='TIMID', ev=MIDS, ab='PRESSURE', item='LEFTOVERS', level=50),
               M('ARTICUNO', 'SUBSTITUTE PROTECT TOXIC ICE_BEAM', nat='TIMID', ev=MIDS, ab='PRESSURE', item='LEFTOVERS', level=50),
               M('MOLTRES', 'SUBSTITUTE PROTECT TOXIC FLAMETHROWER', nat='TIMID', ev=MIDS, ab='PRESSURE', item='LEFTOVERS', level=50),
               M('CHANSEY', 'SOFT_BOILED SEISMIC_TOSS TOXIC PROTECT', nat='BOLD', ev='hp100 def160', ab='NATURAL_CURE', item='LEFTOVERS', level=50),
               M('KADABRA', 'PSYCHIC RECOVER REFLECT THUNDER_WAVE', nat='TIMID', ev=MIDS, ab='SYNCHRONIZE', item='TWISTED_SPOON', level=50)],
  'Playthrough Pressure stall with the three legendary birds (caught at L50): Substitute/Protect so every attack costs 2 PP, Toxic for damage. Chansey holds while the birds recover Leftovers.', level=50)
T('full_stall', [M('CHANSEY', 'SOFT_BOILED SEISMIC_TOSS TOXIC THUNDER_WAVE', nat='BOLD', ev='hp100 def160', ab='NATURAL_CURE', item='LEFTOVERS', level=45),
                 M('CLOYSTER', 'SPIKES SURF TOXIC EXPLOSION', nat='RELAXED', ev='hp100 def60 spd100', ab='SHELL_ARMOR', item='LEFTOVERS', level=45),
                 M('WEEZING', 'WILL_O_WISP SLUDGE_BOMB PAIN_SPLIT HAZE', nat='BOLD', ev='hp100 def100 spa60', ab='LEVITATE', item='LEFTOVERS', level=45),
                 M('HAUNTER', 'WILL_O_WISP SHADOW_BALL THUNDERBOLT TAUNT', nat='TIMID', ev=MIDS, ab='LEVITATE', item='SPELL_TAG', level=45),
                 M('SLOWBRO', 'SURF PSYCHIC REST SLEEP_TALK', nat='BOLD', ev='hp100 def100 spd60', ab='OWN_TEMPO', item='LEFTOVERS', level=45),
                 M('ARCANINE', 'ROAR FLAMETHROWER AERIAL_ACE REST', nat='IMPISH', ev='hp100 def100 spe60', ab='INTIMIDATE', item='LEFTOVERS', level=45)],
  'Playthrough full stall: Cloyster leads with Spikes, Chansey/Weezing/Slowbro wall, Haunter blocks Rapid Spin and burns, Arcanine Roars through Spikes. Win by Toxic, burn and Spikes damage.', level=45)
T('ingrain_seed', [M('VENUSAUR', 'LEECH_SEED SLEEP_POWDER SLUDGE_BOMB SYNTHESIS', nat='BOLD', ev='hp100 def100 spe60', ab='OVERGROW', item='LEFTOVERS', level=40),
                   M('TANGELA', 'INGRAIN LEECH_SEED STUN_SPORE GIGA_DRAIN', nat='BOLD', ev='hp100 def100 spa60', ab='CHLOROPHYLL', item='LEFTOVERS', level=40),
                   M('EXEGGUTOR', 'INGRAIN LEECH_SEED PSYCHIC SLEEP_POWDER', nat='BOLD', ev='hp100 def100 spa60', ab='CHLOROPHYLL', item='LEFTOVERS', level=40),
                   M('VILEPLUME', 'INGRAIN SLEEP_POWDER SLUDGE_BOMB MOONLIGHT', nat='BOLD', ev='hp100 def100 spa60', ab='CHLOROPHYLL', item='LEFTOVERS', level=40),
                   M('CHANSEY', 'SOFT_BOILED SEISMIC_TOSS TOXIC THUNDER_WAVE', nat='BOLD', ev='hp100 def160', ab='NATURAL_CURE', item='LEFTOVERS', level=40)],
  'Playthrough Ingrain/Leech Seed: sleep the target, Leech Seed it, Ingrain (egg move) so the Grass wall heals every turn and cannot be forced out. Rotate to Chansey when a Fire type comes in.', level=40)
T('wobbuffet_trap', [M('WOBBUFFET', 'COUNTER MIRROR_COAT ENCORE SAFEGUARD', nat='CALM', ev='hp60 def100 spd100', ab='SHADOW_TAG', item='LEFTOVERS', level=40),
                     M('SNORLAX', 'CURSE BODY_SLAM EARTHQUAKE REST', nat='CAREFUL', ev='hp100 atk60 spd100', ab='THICK_FAT', item='CHESTO_BERRY', level=40),
                     M('KADABRA', 'PSYCHIC THUNDER_WAVE RECOVER REFLECT', nat='TIMID', ev=MIDS, ab='SYNCHRONIZE', item='TWISTED_SPOON', level=40),
                     M('ARCANINE', 'FLAMETHROWER AERIAL_ACE ROAR CRUNCH', nat='ADAMANT', ev=MID, ab='INTIMIDATE', item='CHARCOAL', level=40),
                     M('GYARADOS', 'SURF RETURN EARTHQUAKE THUNDER_WAVE', nat='ADAMANT', ev=MID, ab='INTIMIDATE', item='LEFTOVERS', level=40)],
  'Playthrough Wobbuffet (Sevii Islands, post-Elite Four but still FireRed): leads, Shadow Tag keeps the foe in, Counter/Mirror Coat kill the attacker, Encore locks a setup move so Snorlax can Curse for free.', level=40, tags=['sevii'])


# ---------------------------------------------------------------------------------------------------
def main():
    with open(OUT, 'w') as f:
        for t in TEAMS:
            f.write(json.dumps(t, separators=(',', ':')) + '\n')
    print('wrote %d teams to %s' % (len(TEAMS), OUT))
    print('archetypes:', ', '.join('%s=%d' % kv for kv in sorted(_counts.items())))
    if '--check' in sys.argv:
        sys.exit(subprocess.call([sys.executable, os.path.join(ROOT, 'tools', 'teams.py'), 'check', OUT]))


if __name__ == '__main__':
    main()
