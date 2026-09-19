#!/usr/bin/env python3
"""Generate sim/teams/playthrough.jsonl: teams a human player is LIKELY to have at each point of a
FireRed playthrough (format: sim/teams/FORMAT.md).

  gen_playthrough_teams.py [--out sim/teams/playthrough.jsonl] [--md sim/teams/playthrough.md] [--seed N]

Everything about the game is read from the decomp or hand-encoded below so the corpus can be regenerated
or perturbed:

  repo data read at run time
    src/data/pokemon/level_up_learnsets.h + level_up_learnset_pointers.h   level-up moves (move timeline)
    src/data/pokemon/evolution.h                                            evolution method/level/stone
    src/data/pokemon/tmhm_learnsets.h                                       TM/HM compatibility
    src/data/pokemon/species_info.h                                         types, base stats (EV spread)
    src/data/battle_moves.h                                                 move power/type (move-forgetting)
  hand-encoded tables (CHECKPOINTS below), calibrated on
    src/data/trainers.h + trainer_parties.h   gym leader / rival / Elite Four levels
    src/data/wild_encounters.json             FireRed wild species + level ranges per map
    data/scripts/item_ball_scripts.inc, data/maps/*/scripts.inc   where each TM/HM is obtained

Checkpoints are cumulative: whatever was available at an earlier checkpoint stays available. Each mon is
built like the game builds it: caught form at a catch level, evolved by the game's method when the player
plausibly could (level; stone once the stone can be bought/found; trade only in `assumes_trade` teams),
level-up moves learned along the timeline with a "player forgets the weakest move" rule, then a few TMs the
player has found by then (single-use TMs are used once per team) and the HMs the party typically carries.
"""
import argparse, collections, json, os, random, re

HERE = os.path.dirname(os.path.abspath(__file__))
SIM = os.path.dirname(HERE)
REPO = os.path.dirname(SIM)

# ----------------------------------------------------------------------------------------------------
# repo data

def read(path):
    return open(os.path.join(REPO, path)).read()


def load_learnsets():
    src = read('src/data/pokemon/level_up_learnsets.h')
    sets = {}
    for m in re.finditer(r'static const u16 (\w+)\[\] = \{(.*?)\};', src, re.S):
        sets[m.group(1)] = [(int(l), mv) for l, mv in re.findall(r'LEVEL_UP_MOVE\(\s*(\d+),\s*MOVE_(\w+)\)', m.group(2))]
    ptr = read('src/data/pokemon/level_up_learnset_pointers.h')
    out = {}
    for sp, name in re.findall(r'\[SPECIES_(\w+)\] = (\w+)', ptr):
        out[sp] = sets[name]
    return out


def load_evolutions():
    src = read('src/data/pokemon/evolution.h')
    out = collections.defaultdict(list)
    for sp, body in species_chunks(src):
        for meth, param, target in re.findall(r'\{EVO_(\w+),\s*(\w+),\s*SPECIES_(\w+)\}', body):
            out[sp].append((meth, param, target))
    return out


def species_chunks(src):
    '''[(species, text up to the next [SPECIES_...] entry)] for the table-style data files.'''
    parts = re.split(r'\[SPECIES_(\w+)\]', src)
    return [(parts[i], parts[i + 1]) for i in range(1, len(parts) - 1, 2)]


def load_tmhm():
    src = read('src/data/pokemon/tmhm_learnsets.h')
    out = {}
    for sp, body in species_chunks(src):
        out[sp] = set(re.findall(r'TMHM\(((?:TM|HM)\d\d_\w+)\)', body))
    return out


def load_species_info():
    src = read('src/data/pokemon/species_info.h')
    out = {}
    for m in re.finditer(r'\[SPECIES_(\w+)\]\s*=\s*\n\s*\{(.*?)\n    \}', src, re.S):
        body = m.group(2)
        types = re.search(r'\.types = \{TYPE_(\w+), TYPE_(\w+)\}', body)
        if not types:
            continue
        st = {k: int(v) for k, v in re.findall(r'\.base(\w+) = (\d+)', body)}
        ab = re.search(r'\.abilities = \{ABILITY_(\w+), ABILITY_(\w+)\}', body)
        out[m.group(1)] = {'types': (types.group(1), types.group(2)), 'atk': st['Attack'], 'spa': st['SpAttack'],
                           'spe': st['Speed'], 'hp': st['HP'], 'two_abilities': bool(ab) and ab.group(2) != 'NONE'}
    return out


def load_moves():
    src = read('src/data/battle_moves.h')
    out = {}
    for m in re.finditer(r'\[MOVE_(\w+)\]\s*=\s*\{(.*?)\}', src, re.S):
        body = m.group(2)
        out[m.group(1)] = {'power': int(re.search(r'\.power = (\d+)', body).group(1)),
                           'type': re.search(r'\.type = TYPE_(\w+)', body).group(1),
                           'acc': int(re.search(r'\.accuracy = (\d+)', body).group(1))}
    return out


LEARN = load_learnsets()
EVOS = load_evolutions()
TMHM = load_tmhm()
INFO = load_species_info()
MOVES = load_moves()
PREEVO = {}
for sp, lst in EVOS.items():
    for _, _, tgt in lst:
        PREEVO[tgt] = sp


def family_root(sp):
    while sp in PREEVO:
        sp = PREEVO[sp]
    return sp


def tm_move(label):            # 'TM06_TOXIC' -> 'TOXIC'
    return label.split('_', 1)[1]

# ----------------------------------------------------------------------------------------------------
# hand-encoded playthrough knowledge
#
# Pool entries: (species as caught, catch level lo, catch level hi, popularity weight[, flags])
#   flags: 'gift' (fixed level), 'legend', 'fade' (weak early catch, players drop it later),
#          'trade' (in-game trade: OT is an outsider), 'fossil'
# Levels come from src/data/wild_encounters.json (FireRed tables); gift levels are the game's.

CHECKPOINTS = [
    dict(key='route22', label='Rival on Route 22 (before Viridian Forest)', ref=8, boss='Rival Squirtle L9',
         party=(1, 2), tms=[], hms=[], stones=[], tutors=[],
         pool=[('PIDGEY', 2, 5, 10), ('RATTATA', 2, 4, 5, 'fade'), ('CATERPIE', 4, 5, 4, 'fade'),
               ('WEEDLE', 4, 5, 3, 'fade'), ('MANKEY', 3, 5, 5), ('SPEAROW', 3, 5, 4)]),
    dict(key='brock', label='Pewter Gym (Brock)', ref=13, boss='Onix L14',
         party=(1, 3), tms=[], hms=[], stones=[], tutors=[],
         pool=[('PIKACHU', 3, 5, 8), ('METAPOD', 5, 5, 1, 'fade'), ('KAKUNA', 4, 6, 1, 'fade')]),
    dict(key='misty', label='Cerulean Gym (Misty)', ref=20, boss='Starmie L21',
         party=(2, 4), tms=['TM39_ROCK_TOMB', 'TM09_BULLET_SEED', 'TM46_THIEF', 'TM05_ROAR', 'TM45_ATTRACT',
                            'TM43_SECRET_POWER'],
         hms=[], stones=['MOON_STONE'], tutors=['MEGA_PUNCH', 'MEGA_KICK', 'SEISMIC_TOSS'],
         pool=[('NIDORAN_M', 6, 7, 8), ('NIDORAN_F', 6, 6, 6), ('JIGGLYPUFF', 3, 7, 4), ('ZUBAT', 7, 11, 3, 'fade'),
               ('GEODUDE', 7, 10, 7), ('PARAS', 5, 12, 3, 'fade'), ('CLEFAIRY', 8, 12, 4), ('EKANS', 6, 12, 4),
               ('ABRA', 8, 13, 7), ('ODDISH', 12, 14, 5), ('MAGIKARP', 5, 5, 4, 'gift')]),
    dict(key='ss_anne', label='Rival on the S.S. Anne', ref=21, boss='Rival Wartortle L20',
         party=(2, 4), tms=['TM28_DIG'], hms=[], stones=[], tutors=[],
         pool=[('MEOWTH', 10, 16, 5), ('DROWZEE', 11, 15, 4), ('DIGLETT', 15, 22, 6), ('DUGTRIO', 29, 31, 1),
               ('SANDSHREW', 8, 12, 0)]),        # Sandshrew is LeafGreen-only: weight 0, kept for perturbation
    dict(key='surge', label='Vermilion Gym (Lt. Surge)', ref=24, boss='Raichu L24',
         party=(3, 4), tms=['TM31_BRICK_BREAK', 'TM44_REST'], hms=['HM01_CUT'], stones=[], tutors=[],
         pool=[]),
    dict(key='erika', label='Celadon Gym (Erika)', ref=28, boss='Vileplume L29',
         party=(3, 5),
         tms=['TM34_SHOCK_WAVE', 'TM40_AERIAL_ACE', 'TM05_ROAR', 'TM15_HYPER_BEAM', 'TM28_DIG', 'TM31_BRICK_BREAK',
              'TM43_SECRET_POWER', 'TM45_ATTRACT', 'TM16_LIGHT_SCREEN', 'TM20_SAFEGUARD', 'TM33_REFLECT',
              'TM13_ICE_BEAM', 'TM23_IRON_TAIL', 'TM24_THUNDERBOLT', 'TM30_SHADOW_BALL', 'TM35_FLAMETHROWER'],
         hms=['HM05_FLASH', 'HM02_FLY'], stones=['FIRE_STONE', 'WATER_STONE', 'THUNDER_STONE', 'LEAF_STONE'],
         tutors=['ROCK_SLIDE', 'COUNTER'],
         pool=[('VOLTORB', 14, 17, 4), ('MACHOP', 16, 17, 6), ('ONIX', 13, 17, 4), ('GROWLITHE', 15, 20, 7),
               ('EEVEE', 25, 25, 8, 'gift'), ('FARFETCHD', 15, 22, 2, 'trade'), ('MR_MIME', 12, 20, 1, 'trade'),
               ('DRATINI', 18, 18, 1, 'gift'), ('SCYTHER', 25, 25, 1, 'gift'), ('PORYGON', 18, 18, 1, 'gift')]),
    dict(key='tower', label='Pokemon Tower (Lavender)', ref=29, boss='Rival Wartortle L25 / Marowak ghost',
         party=(3, 5), tms=['TM19_GIGA_DRAIN', 'TM12_TAUNT', 'TM21_FRUSTRATION', 'TM49_SNATCH'],
         hms=[], stones=[], tutors=[],
         pool=[('GASTLY', 13, 19, 6), ('CUBONE', 15, 19, 5), ('HAUNTER', 20, 25, 3)]),
    dict(key='koga', label='Fuchsia Gym (Koga)', ref=38, boss='Weezing L43',
         party=(4, 6), tms=['TM27_RETURN', 'TM48_SKILL_SWAP', 'TM18_RAIN_DANCE', 'TM11_SUNNY_DAY',
                            'TM32_DOUBLE_TEAM', 'TM47_STEEL_WING'],
         hms=['HM03_SURF', 'HM04_STRENGTH'], stones=[], tutors=[],
         pool=[('SNORLAX', 30, 30, 7, 'gift'), ('VENONAT', 24, 26, 3), ('GLOOM', 28, 30, 2), ('PIDGEOTTO', 29, 29, 2),
               ('DITTO', 23, 25, 1), ('DODUO', 18, 28, 6), ('FEAROW', 25, 29, 2), ('RATICATE', 23, 29, 1),
               ('RHYHORN', 25, 26, 6), ('NIDORINO', 30, 33, 2), ('NIDORINA', 30, 31, 2), ('EXEGGCUTE', 23, 27, 4),
               ('KANGASKHAN', 25, 28, 4), ('SCYTHER', 23, 28, 5), ('TAUROS', 25, 28, 3), ('CHANSEY', 23, 26, 2),
               ('PARASECT', 25, 30, 1), ('VENOMOTH', 32, 32, 1), ('DRATINI', 15, 25, 4), ('GOLDEEN', 5, 25, 2),
               ('POLIWAG', 5, 25, 4), ('SEAKING', 20, 30, 1), ('PSYDUCK', 15, 35, 3), ('KRABBY', 5, 15, 3),
               ('HORSEA', 5, 35, 3), ('GYARADOS', 15, 25, 4), ('SHELLDER', 15, 25, 2), ('POLIWHIRL', 20, 30, 2),
               ('LICKITUNG', 25, 35, 1, 'trade')]),
    dict(key='silph', label='Silph Co. (rival + Giovanni)', ref=39, boss='Rival Blastoise L40',
         party=(4, 6), tms=['TM06_TOXIC', 'TM29_PSYCHIC', 'TM01_FOCUS_PUNCH', 'TM08_BULK_UP', 'TM41_TORMENT'],
         hms=[], stones=[], tutors=['THUNDER_WAVE'],
         pool=[('LAPRAS', 25, 25, 8, 'gift'), ('HITMONLEE', 25, 25, 4, 'gift'), ('HITMONCHAN', 25, 25, 4, 'gift')]),
    dict(key='sabrina', label='Saffron Gym (Sabrina)', ref=41, boss='Alakazam L43',
         party=(4, 6), tms=[], hms=[], stones=[], tutors=['MIMIC'], pool=[]),
    dict(key='blaine', label='Cinnabar Gym (Blaine)', ref=44, boss='Arcanine L47',
         party=(5, 6), tms=['TM04_CALM_MIND', 'TM14_BLIZZARD', 'TM22_SOLAR_BEAM', 'TM17_PROTECT', 'TM25_THUNDER'],
         hms=[], stones=[], tutors=['METRONOME'],
         pool=[('TENTACOOL', 5, 40, 2), ('SEEL', 28, 34, 4), ('DEWGONG', 32, 36, 2), ('GOLBAT', 26, 30, 1),
               ('GOLDUCK', 32, 35, 2), ('SEADRA', 25, 35, 2), ('TANGELA', 17, 28, 3), ('KOFFING', 28, 30, 2),
               ('GRIMER', 28, 28, 2), ('WEEZING', 32, 34, 1), ('OMANYTE', 5, 5, 2, 'fossil'), ('KABUTO', 5, 5, 2, 'fossil'),
               ('AERODACTYL', 5, 5, 3, 'fossil'), ('ARTICUNO', 50, 50, 4, 'legend'), ('MAGNEMITE', 22, 25, 4),
               ('MAGNETON', 31, 34, 2), ('ELECTABUZZ', 32, 35, 4), ('ZAPDOS', 50, 50, 5, 'legend')]),
    dict(key='giovanni', label='Viridian Gym (Giovanni)', ref=47, boss='Rhyhorn L50',
         party=(5, 6), tms=['TM38_FIRE_BLAST'], hms=['HM06_ROCK_SMASH'], stones=[],
         tutors=['BODY_SLAM', 'SWORDS_DANCE'],
         pool=[('MOLTRES', 50, 50, 5, 'legend'), ('PONYTA', 30, 36, 4), ('RAPIDASH', 37, 42, 2), ('MACHOKE', 38, 42, 3),
               ('PERSIAN', 37, 40, 1), ('HYPNO', 37, 40, 2), ('VENOMOTH', 37, 40, 1)]),
    dict(key='victory_road', label='Victory Road (rival on Route 22, late)', ref=49, boss='Rival Blastoise L53',
         party=(5, 6), tms=['TM26_EARTHQUAKE', 'TM02_DRAGON_CLAW', 'TM07_HAIL', 'TM37_SANDSTORM', 'TM50_OVERHEAT'],
         hms=[], stones=[], tutors=['DOUBLE_EDGE'],
         pool=[('ONIX', 40, 48, 2), ('MACHOKE', 44, 48, 2), ('MAROWAK', 44, 48, 3), ('ARBOK', 44, 46, 2),
               ('GOLBAT', 44, 46, 2), ('PRIMEAPE', 42, 42, 3), ('FEAROW', 40, 44, 1)]),
    dict(key='elite4', label='Elite Four + Champion', ref=53, boss='Lance Dragonite L60 / Champion Blastoise L63',
         party=(6, 6), tms=[], hms=[], stones=[], tutors=[], pool=[]),
    dict(key='postgame', label='Post-game (Sevii Islands 4-7, Cerulean Cave)', ref=62, boss='Mewtwo L70',
         party=(6, 6), tms=['TM42_FACADE', 'TM36_SLUDGE_BOMB', 'TM10_HIDDEN_POWER'], hms=['HM07_WATERFALL'], stones=[],
         tutors=['EXPLOSION', 'SUBSTITUTE', 'DREAM_EATER', 'SOFT_BOILED'],
         pool=[('MEWTWO', 70, 70, 6, 'legend'), ('WOBBUFFET', 55, 61, 2), ('ELECTRODE', 58, 64, 2), ('MAGNETON', 49, 55, 1),
               ('KADABRA', 55, 67, 3), ('DITTO', 52, 67, 1), ('PRIMEAPE', 52, 61, 1), ('GOLDUCK', 40, 65, 2),
               ('GRAVELER', 40, 65, 2), ('LAPRAS', 30, 45, 2), ('DEWGONG', 49, 53, 1), ('SWINUB', 23, 31, 2),
               ('DELIBIRD', 30, 30, 1), ('HERACROSS', 15, 30, 4), ('LEDYBA', 9, 14, 1), ('SPINARAK', 9, 14, 1),
               ('NATU', 15, 20, 2), ('YANMA', 18, 18, 1), ('WOOPER', 15, 15, 2), ('MURKROW', 15, 22, 3),
               ('LARVITAR', 15, 20, 4), ('SKARMORY', 30, 30, 3), ('PHANPY', 10, 15, 2), ('DUNSPARCE', 5, 35, 1),
               ('HOPPIP', 6, 16, 1), ('SENTRET', 10, 15, 1), ('QWILFISH', 15, 25, 1), ('SLUGMA', 18, 36, 2),
               ('MAGCARGO', 25, 45, 1), ('TENTACRUEL', 35, 45, 1)]),
]
CP_INDEX = {c['key']: i for i, c in enumerate(CHECKPOINTS)}

# TMs the player can buy again (Celadon Dept. Store 2F/roof) or win again (Game Corner): reusable, but the
# Game Corner ones cost a lot of coins so they are rarer.
REUSABLE_TMS = {'TM05_ROAR': 0.3, 'TM15_HYPER_BEAM': 0.5, 'TM28_DIG': 1.0, 'TM31_BRICK_BREAK': 1.0,
                'TM43_SECRET_POWER': 0.4, 'TM45_ATTRACT': 0.2, 'TM16_LIGHT_SCREEN': 0.3, 'TM20_SAFEGUARD': 0.2,
                'TM33_REFLECT': 0.3, 'TM13_ICE_BEAM': 0.35, 'TM23_IRON_TAIL': 0.25, 'TM24_THUNDERBOLT': 0.45,
                'TM30_SHADOW_BALL': 0.3, 'TM35_FLAMETHROWER': 0.45}

# How often the party carries each HM once it is available (Surf is a real attack, Cut/Flash are chores).
HM_CARRY = {'HM01_CUT': 0.6, 'HM02_FLY': 0.6, 'HM03_SURF': 0.9, 'HM04_STRENGTH': 0.4, 'HM05_FLASH': 0.15,
            'HM06_ROCK_SMASH': 0.2, 'HM07_WATERFALL': 0.3}

# Held items a playthrough mon plausibly holds (checkpoint index it becomes available at, weight).
ITEMS = [('ORAN_BERRY', 2, 3), ('PECHA_BERRY', 2, 1), ('CHERI_BERRY', 2, 1), ('SITRUS_BERRY', 7, 2),
         ('LUM_BERRY', 11, 1), ('QUICK_CLAW', 7, 2), ('AMULET_COIN', 5, 3), ('EXP_SHARE', 7, 3),
         ('SOOTHE_BELL', 9, 1), ('LEFTOVERS', 7, 1), ('KINGS_ROCK', 8, 1), ('MACHO_BRACE', 8, 1),
         ('SILK_SCARF', 8, 1), ('CHARCOAL', 10, 1), ('MYSTIC_WATER', 10, 1), ('MIRACLE_SEED', 10, 1),
         ('MAGNET', 10, 1), ('BLACK_BELT', 8, 1), ('TWISTED_SPOON', 9, 1), ('SHELL_BELL', 14, 1),
         ('BRIGHT_POWDER', 14, 1), ('FOCUS_BAND', 12, 1), ('LUCKY_EGG', 12, 1)]
P_ITEM = 0.18

# Value of status moves to a casual player (damaging moves are scored from power/STAB).
STATUS_VALUE = collections.defaultdict(lambda: 20, {
    'SWORDS_DANCE': 55, 'LEECH_SEED': 50, 'SLEEP_POWDER': 60, 'STUN_SPORE': 45, 'THUNDER_WAVE': 55, 'HYPNOSIS': 50,
    'RECOVER': 55, 'REST': 40, 'SUBSTITUTE': 35, 'AGILITY': 40, 'CALM_MIND': 55, 'BULK_UP': 50, 'TOXIC': 45,
    'CONFUSE_RAY': 40, 'WILL_O_WISP': 35, 'AMNESIA': 40, 'BARRIER': 30, 'REFLECT': 35, 'LIGHT_SCREEN': 35,
    'HAZE': 15, 'MINIMIZE': 30, 'DOUBLE_TEAM': 35, 'PROTECT': 30, 'GROWTH': 35, 'SYNTHESIS': 45, 'MORNING_SUN': 45,
    'SOFT_BOILED': 55, 'MILK_DRINK': 50, 'BELLY_DRUM': 30, 'CURSE': 30, 'DRAGON_DANCE': 50, 'MEAN_LOOK': 10,
    'SPIDER_WEB': 10, 'LOCK_ON': 15, 'MIND_READER': 15, 'SCREECH': 20, 'ACID_ARMOR': 30, 'POISON_POWDER': 25,
    'POISON_GAS': 15, 'SING': 35, 'LOVELY_KISS': 45, 'SPORE': 60, 'GLARE': 45, 'ATTRACT': 25, 'FLASH': 8,
    'SAND_ATTACK': 12, 'SMOKESCREEN': 12, 'KINESIS': 10, 'MIMIC': 10, 'METRONOME': 25, 'TRANSFORM': 40, 'SPLASH': 0,
    'MIST': 12, 'SAFEGUARD': 20, 'ENCORE': 25, 'SPITE': 10, 'DISABLE': 15, 'TAUNT': 20, 'TORMENT': 15,
    'SWAGGER': 20, 'ROLE_PLAY': 10, 'SKILL_SWAP': 10, 'ENDURE': 15, 'DETECT': 25, 'MEDITATE': 20, 'HARDEN': 12,
    'WITHDRAW': 12, 'DEFENSE_CURL': 15, 'FOCUS_ENERGY': 15, 'LEER': 12, 'TAIL_WHIP': 12, 'GROWL': 12, 'CHARM': 25,
    'SWEET_SCENT': 10, 'STRING_SHOT': 10, 'HELPING_HAND': 5, 'FOLLOW_ME': 5, 'ROAR': 8, 'WHIRLWIND': 8,
    'TELEPORT': 5, 'SUPERSONIC': 12, 'SHARPEN': 15, 'TAIL_GLOW': 45, 'COSMIC_POWER': 30, 'IRON_DEFENSE': 30,
    'HOWL': 20, 'TICKLE': 15, 'SWEET_KISS': 25, 'FAKE_TEARS': 15, 'FLATTER': 10, 'RAIN_DANCE': 15, 'SUNNY_DAY': 15,
    'HAIL': 8, 'SANDSTORM': 8, 'CONVERSION': 10, 'CONVERSION_2': 10, 'SNATCH': 10, 'PSYCH_UP': 10, 'STOCKPILE': 15,
    'SWALLOW': 15, 'WISH': 30, 'YAWN': 30, 'REFRESH': 20, 'INGRAIN': 20, 'AROMATHERAPY': 20, 'HEAL_BELL': 20,
    'PERISH_SONG': 10, 'DESTINY_BOND': 15, 'GRUDGE': 5, 'IMPRISON': 5, 'MAGIC_COAT': 10, 'RECYCLE': 5,
    'MUD_SPORT': 5, 'WATER_SPORT': 5, 'ODOR_SLEUTH': 5, 'FORESIGHT': 5, 'MOONLIGHT': 45, 'SLACK_OFF': 50,
    'BATON_PASS': 10, 'PAIN_SPLIT': 25, 'REST': 40, 'BLOCK': 5, 'CAMOUFLAGE': 5, 'CHARGE': 10, 'NIGHTMARE': 15,
    'SCARY_FACE': 15, 'COTTON_SPORE': 25, 'SPIKES': 15, 'ASSIST': 10, 'GRASS_WHISTLE': 35, 'TEETER_DANCE': 20,
    'FAKE_OUT': 25, 'BULK_UP': 50, 'LIGHT_SCREEN': 35, 'DOUBLE_TEAM': 35, 'FLATTER': 10,
})
FIXED_VALUE = {'SEISMIC_TOSS': 48, 'NIGHT_SHADE': 42, 'DRAGON_RAGE': 45, 'SONIC_BOOM': 30, 'PSYWAVE': 25,
               'SUPER_FANG': 35, 'COUNTER': 30, 'MIRROR_COAT': 30, 'BIDE': 15, 'ENDEAVOR': 15, 'FLAIL': 20,
               'REVERSAL': 20, 'LOW_KICK': 30, 'RETURN': 85, 'FRUSTRATION': 20, 'HIDDEN_POWER': 55, 'MAGNITUDE': 60,
               'PRESENT': 20, 'SPIT_UP': 10, 'FISSURE': 25, 'HORN_DRILL': 25, 'GUILLOTINE': 25, 'SHEER_COLD': 25,
               'EXPLOSION': 22, 'SELF_DESTRUCT': 18, 'STRUGGLE': 0, 'FOCUS_PUNCH': 60, 'SOLAR_BEAM': 70,
               'SKY_ATTACK': 60, 'RAZOR_WIND': 40, 'SKULL_BASH': 55, 'HYPER_BEAM': 95, 'DREAM_EATER': 35,
               'TRIPLE_KICK': 40, 'ROLLOUT': 35, 'FURY_CUTTER': 30, 'SNORE': 10, 'SLEEP_TALK': 15,
               'ERUPTION': 70, 'WATER_SPOUT': 70, 'FACADE': 65, 'NATURE_POWER': 40, 'WEATHER_BALL': 45,
               'FALSE_SWIPE': 30, 'BEAT_UP': 20, 'UPROAR': 40, 'SECRET_POWER': 68}


PHYSICAL_TYPES = {'NORMAL', 'FIGHTING', 'FLYING', 'POISON', 'GROUND', 'ROCK', 'BUG', 'GHOST', 'STEEL'}


def split_factor(mv, sp):
    """Down-weight physical moves on special attackers and vice versa (players notice weak damage)."""
    if sp is None or MOVES[mv]['power'] <= 1:
        return 1.0
    info = INFO[sp]
    phys = MOVES[mv]['type'] in PHYSICAL_TYPES
    if phys and info['spa'] > info['atk'] * 1.3:
        return 0.6
    if not phys and info['atk'] > info['spa'] * 1.3:
        return 0.6
    return 1.0


def move_score(mv, types, known, rng=None, sp=None):
    """How much a casual player values `mv` on a mon with `types` already knowing `known`."""
    d = MOVES[mv]
    if mv in FIXED_VALUE:
        s = FIXED_VALUE[mv] * (1.3 if d['type'] in types and d['power'] else 1.0)
    elif d['power'] > 1:
        s = d['power'] * (1.5 if d['type'] in types else 1.0) * (0.75 + d['acc'] / 400.0)
        # redundant with a stronger move of the same type the mon already has
        for k in known:
            if k != mv and MOVES[k]['type'] == d['type'] and MOVES[k]['power'] > d['power']:
                s *= 0.45
                break
    elif d['power'] == 1:
        s = 40
    else:
        s = STATUS_VALUE[mv]
    s *= split_factor(mv, sp)
    if rng is not None:
        s *= rng.uniform(0.85, 1.15)
    return s


def levelup_moves_at(sp, level):
    """The moves a freshly generated `sp` of `level` knows (last 4 level-up moves <= level, game order)."""
    out = []
    for l, mv in LEARN[sp]:
        if l <= level and mv not in out:
            out.append(mv)
    return out[-4:]


def learn(moves, mv, sp, rng):
    types = INFO[sp]['types']
    if mv in moves:
        return
    if len(moves) < 4:
        moves.append(mv)
        return
    cand = moves + [mv]
    scores = [move_score(m, types, cand, rng, sp) for m in cand]
    drop = scores.index(min(scores))
    if drop == 4:
        return                                  # player declines the new move
    moves[drop] = mv


# ----------------------------------------------------------------------------------------------------
# building one mon

def evolve_plan(species, catch, level, cp_idx, rng, trade_ok, is_starter=False):
    """Return [(form, from_level)...]: the forms the mon passes through between `catch` and `level`."""
    stones = {}                      # stone -> earliest level the player plausibly has it (ref of that checkpoint - 6)
    for c in CHECKPOINTS[:cp_idx + 1]:
        for st in c['stones']:
            stones.setdefault(st, max(1, c['ref'] - 6))
    p_level = 1.0 if is_starter else 0.94
    forms = [(species, catch)]
    cur, at = species, catch
    while True:
        opts = EVOS.get(cur, [])
        if not opts:
            break
        choice = None
        if cur == 'EEVEE':
            r = rng.random()
            avail = [(t, stones[it.replace('ITEM_', '')]) for (m, it, t) in opts
                     if m == 'ITEM' and it.replace('ITEM_', '') in stones and level > stones[it.replace('ITEM_', '')]]
            if avail and r < 0.85:
                choice = ('ITEM',) + rng.choice(avail)
            elif r < 0.95 and level >= at + 12:
                choice = ('FRIEND', rng.choice(['ESPEON', 'UMBREON']))
        else:
            for meth, param, tgt in opts:
                if meth == 'LEVEL' and int(param) <= level and rng.random() < p_level:
                    choice = ('LEVEL', tgt, int(param)); break
                if meth == 'ITEM' and param.replace('ITEM_', '') in stones and level > stones[param.replace('ITEM_', '')] \
                        and rng.random() < (0.7 if level >= 26 else 0.35):
                    choice = ('ITEM', tgt, stones[param.replace('ITEM_', '')]); break
                if meth == 'TRADE' and trade_ok and rng.random() < 0.75:
                    choice = ('TRADE', tgt); break
                if meth == 'FRIENDSHIP' and level >= 40 and level - at >= 18 and rng.random() < 0.25:
                    choice = ('FRIEND', tgt); break
        if choice is None:
            break
        if choice[0] == 'LEVEL':
            lv = max(choice[2], at + 1)
        else:
            floor = max(at + 1, min(level, at + 3), choice[2] if len(choice) > 2 else 0)
            lv = rng.randint(floor, level) if level >= floor else level + 1
        if lv > level:
            break
        cur, at = choice[1], lv
        forms.append((cur, lv))
    return forms


def build_moves(forms, level, rng):
    """Simulate the level-up timeline: initial wild/gift moves, then learn each level's moves, forgetting
    the weakest move when full."""
    sp0, catch = forms[0]
    moves = levelup_moves_at(sp0, catch)
    fi = 0
    for lv in range(catch + 1, level + 1):
        for l, mv in LEARN[forms[fi][0]]:
            if l == lv:
                learn(moves, mv, forms[fi][0], rng)
        while fi + 1 < len(forms) and forms[fi + 1][1] == lv:
            fi += 1
            for l, mv in LEARN[forms[fi][0]]:
                if l == lv:
                    learn(moves, mv, forms[fi][0], rng)
    while fi + 1 < len(forms):      # evolutions at the catch level itself (rare)
        fi += 1
    return moves, forms[-1][0]


def teach(moves, mv, sp, protected, rng, force=False):
    types = INFO[sp]['types']
    if mv in moves:
        return False
    if len(moves) < 4:
        moves.append(mv); return True
    cand = [(move_score(m, types, moves, rng, sp), i) for i, m in enumerate(moves) if m not in protected]
    if not cand:
        return False
    low, idx = min(cand)
    if force or move_score(mv, types, moves, rng, sp) > low:
        moves[idx] = mv
        return True
    return False


def ev_spread(sp, level, rng):
    info = INFO[sp]
    total = rng.randint(0, min(150, level * 4))
    w = [1.0, 2.0 if info['atk'] >= info['spa'] else 0.4, 0.7, 1.2, 2.0 if info['spa'] > info['atk'] else 0.4, 0.6]
    w = [x * rng.uniform(0.5, 1.5) for x in w]
    s = sum(w)
    evs = [int(total * x / s) for x in w]
    return evs


def make_mon(entry, level, cp_idx, rng, ctx, is_starter=False, hm_slave=False):
    sp, lo, hi, weight = entry[:4]
    flags = entry[4:]
    catch = 5 if is_starter else rng.randint(lo, hi)
    level = max(level, catch)
    forms = evolve_plan(sp, catch, level, cp_idx, rng, ctx['trade_ok'], is_starter)
    moves, final = build_moves(forms, level, rng)
    protected = set()
    types = INFO[final]['types']
    # TMs: single-use ones once per team, reusable ones with their weight
    p_tm = 0.0 if hm_slave else ctx['p_tm'] * (1.35 if is_starter else 1.0)
    for _ in range(2):
        if rng.random() > p_tm:
            break
        cands = []
        for label in ctx['tms']:
            if label in ctx['used_tms'] and label not in REUSABLE_TMS:
                continue
            if label not in TMHM.get(final, ()):
                continue
            mv = tm_move(label)
            if mv in moves:
                continue
            sc = move_score(mv, types, moves, None, final) * REUSABLE_TMS.get(label, 1.0)
            if MOVES[mv]['power'] == 0 and mv not in FIXED_VALUE:
                sc *= 0.4
            cands.append((sc ** 2, label, mv))
        if not cands:
            break
        tot = sum(c[0] for c in cands)
        r = rng.uniform(0, tot)
        for c in cands:
            r -= c[0]
            if r <= 0:
                break
        if teach(moves, c[2], final, protected, rng):
            protected.add(c[2])
            ctx['used_tms'].add(c[1])
        p_tm *= 0.5
    # tutors (rare)
    if ctx['tutors'] and rng.random() < 0.08:
        mv = rng.choice(ctx['tutors'])
        if mv in TUTOR_OK.get(final, ()) and teach(moves, mv, final, protected, rng):
            protected.add(mv)
    ivs = [rng.randint(0, 31) for _ in range(6)]
    item = 'NONE'
    if rng.random() < ctx['p_item']:
        opts = [(w, it) for it, at, w in ITEMS if at <= cp_idx]
        if opts:
            tot = sum(w for w, _ in opts)
            r = rng.uniform(0, tot)
            for w, it in opts:
                r -= w
                if r <= 0:
                    item = it; break
    hap = rng.randint(150, 230) if is_starter else rng.randint(100, 200)
    mon = {'species': final, 'level': level, 'moves': moves, 'item': item,
           'ability': rng.choice([0, 1]) if INFO[final]['two_abilities'] else 0, 'nature': rng.choice(NATURES), 'ivs': ivs,
           'evs': ev_spread(final, level, rng), 'happiness': hap}
    if 'trade' in flags:
        mon['ot'] = 'outsider'
    mon['_protected'] = protected
    mon['_flags'] = flags
    return mon


NATURES = ['HARDY', 'LONELY', 'BRAVE', 'ADAMANT', 'NAUGHTY', 'BOLD', 'DOCILE', 'RELAXED', 'IMPISH', 'LAX', 'TIMID',
           'HASTY', 'SERIOUS', 'JOLLY', 'NAIVE', 'MODEST', 'MILD', 'QUIET', 'BASHFUL', 'RASH', 'CALM', 'GENTLE',
           'SASSY', 'CAREFUL', 'QUIRKY']


def load_tutor_ok():
    src = read('src/data/pokemon/tutor_learnsets.h')
    out = {}
    for sp, body in species_chunks(src):
        out[sp] = set(re.findall(r'TUTOR\(MOVE_(\w+)\)', body))
    return out


TUTOR_OK = load_tutor_ok()

# ----------------------------------------------------------------------------------------------------
# building a team

STARTERS = ['BULBASAUR', 'CHARMANDER', 'SQUIRTLE']

STYLES = {
    # key: (description, party size rule, starter level offset range, others level offset range, variants)
    'early_catches': ('starter plus the common catches of the routes so far, trained roughly evenly', 'mid', (-2, 2), (-5, -1), 2),
    'balanced':      ('type coverage: catches chosen to add new types to the party', 'max', (-1, 3), (-4, 0), 2),
    'monotype':      ('favourite-type party: starter plus mons sharing one type', 'max', (-1, 3), (-4, 0), 1),
    'speedrun':      ('small fast party: overleveled starter plus an HM carrier', 'small', (4, 9), (-12, -4), 1),
    'solo':          ('overleveled starter alone', 'one', (5, 11), (0, 0), 1),
    'underleveled':  ('full party that has spread experience thin', 'max', (-6, -3), (-9, -4), 1),
    'overleveled':   ('grinder: everything a few levels above the leader', 'mid', (4, 8), (0, 5), 1),
}


def cumulative(cp_idx, key):
    out = []
    for c in CHECKPOINTS[:cp_idx + 1]:
        out.extend(c[key])
    return out


def pool_at(cp_idx):
    out = []
    for i, c in enumerate(CHECKPOINTS[:cp_idx + 1]):
        for e in c['pool']:
            w = e[3]
            if 'fade' in e[4:] and cp_idx - i >= 2:
                w *= 0.3
            if 'legend' in e[4:] and cp_idx - i >= 1:
                w *= 1.3
            if w > 0:
                out.append((e, w, i))
    return out


def weighted_pick(items, rng):
    tot = sum(w for _, w in items)
    r = rng.uniform(0, tot)
    for it, w in items:
        r -= w
        if r <= 0:
            return it
    return items[-1][0]


def final_types(sp, level):
    """Rough final-form types for coverage/monotype selection (level evolutions only)."""
    cur = sp
    while True:
        nxt = [t for m, p, t in EVOS.get(cur, []) if m == 'LEVEL' and int(p) <= level]
        if not nxt:
            break
        cur = nxt[0]
    return set(INFO[cur]['types']) - {'NONE'}


def choose_party(cp_idx, style, size, ref, rng, mono_type=None):
    """Pick (entry, checkpoint index) for the non-starter slots."""
    pool = pool_at(cp_idx)
    chosen, roots, types = [], set(), set()
    if mono_type:
        pool = [(e, w, i) for e, w, i in pool if mono_type in final_types(e[0], ref) or mono_type in INFO[e[0]]['types']]
    tries = 0
    while len(chosen) < size and pool and tries < 200:
        tries += 1
        e, w, i = weighted_pick([((e, w, i), w) for e, w, i in pool], rng)
        root = family_root(e[0])
        if root in roots:
            continue
        if style == 'speedrun':
            hms = [h for h in cumulative(cp_idx, 'hms')]
            if hms and sum(h in TMHM.get(e[0], ()) for h in hms) < min(3, len(hms)):
                continue
        if style == 'balanced':
            ft = final_types(e[0], ref)
            if ft <= types and rng.random() < 0.7:
                continue
            types |= ft
        # legendaries are rare in a normal party
        if 'legend' in e[4:] and style not in ('balanced', 'overleveled') and rng.random() < 0.6:
            continue
        roots.add(root)
        chosen.append((e, i))
    chosen.sort(key=lambda x: x[1])         # party order ~ catch order
    return chosen


def make_team(cp_idx, starter, style, n, rng):
    cp = CHECKPOINTS[cp_idx]
    ref = cp['ref']
    desc, size_rule, s_off, o_off, _ = STYLES[style]
    lo, hi = cp['party']
    if size_rule == 'one':
        size = 1
    elif size_rule == 'small':
        size = 1 if cp_idx < CP_INDEX['surge'] else rng.randint(2, 3)
    elif size_rule == 'max':
        size = hi
    else:
        size = rng.randint(lo, hi)
    trade_ok = (style == 'balanced' and n == 2 and cp_idx >= CP_INDEX['misty'])
    ctx = {'trade_ok': trade_ok, 'tms': cumulative(cp_idx, 'tms'), 'used_tms': set(),
           'tutors': cumulative(cp_idx, 'tutors'), 'p_tm': 0.25 + 0.03 * cp_idx, 'p_item': P_ITEM if cp_idx >= 2 else 0.05}
    tags = [cp['key'], starter.lower(), style]
    mono_type = None
    if style == 'monotype':
        counts = collections.Counter()
        for e, w, i in pool_at(cp_idx):
            for t in final_types(e[0], ref):
                counts[t] += 1
        cands = [(t, c) for t, c in counts.items() if c >= 2 and t != 'NONE']
        mono_type = weighted_pick(cands, rng) if cands else None
        if mono_type:
            tags.append('mono_' + mono_type.lower())
    starter_level = max(5, min(100, ref + rng.randint(*s_off)))
    mons = [make_mon((starter, 5, 5, 0), starter_level, cp_idx, rng, ctx, is_starter=True)]
    party = choose_party(cp_idx, style, size - 1, ref, rng, mono_type) if size > 1 else []
    for e, i in party:
        if style == 'speedrun':
            lv = rng.randint(e[1], e[2] + 3)
        elif 'legend' in e[4:] or 'gift' in e[4:] and e[1] >= ref - 8:
            lv = e[1] + rng.randint(0, 4)
        else:
            lv = ref + rng.randint(*o_off)
        lv = max(2, min(100, lv))
        mons.append(make_mon(e, lv, cp_idx, rng, ctx, hm_slave=(style == 'speedrun')))
    # HMs the party carries
    if style == 'speedrun' and len(mons) > 1:
        slave = mons[-1]
        for h in cumulative(cp_idx, 'hms'):
            if h in TMHM.get(slave['species'], ()) and h != 'HM05_FLASH':
                    teach(slave['moves'], tm_move(h), slave['species'], slave['_protected'], rng, force=True)
                    slave['_protected'].add(tm_move(h))
    else:
        for h in cumulative(cp_idx, 'hms'):
            if rng.random() > HM_CARRY[h]:
                continue
            mv = tm_move(h)
            if any(mv in m['moves'] for m in mons):
                continue
            cands = [m for m in mons if h in TMHM.get(m['species'], ()) and sum(x in HMS_ALL for x in m['moves']) < 2]
            if not cands:
                continue
            if h == 'HM03_SURF':
                pref = [m for m in cands if 'WATER' in INFO[m['species']]['types']] or cands
            else:
                pref = [m for m in cands if m is not mons[0]] or cands
            m = rng.choice(pref)
            if teach(m['moves'], mv, m['species'], m['_protected'], rng, force=True):
                m['_protected'].add(mv)
    # tags
    if any('legend' in m['_flags'] for m in mons):
        tags.append('legendary')
    if any(m.get('ot') == 'outsider' for m in mons):
        tags.append('ingame_trade')
    if trade_ok and any(any(mm == 'TRADE' for mm, _, _ in EVOS.get(PREEVO.get(m['species'], ''), [])) for m in mons):
        tags.append('assumes_trade')
    if style in ('underleveled',) or (style not in ('solo', 'speedrun', 'overleveled') and starter_level < ref - 2):
        tags.append('below_ref')
    if style in ('solo', 'speedrun', 'overleveled') or starter_level > ref + 3:
        tags.append('above_ref')
    if mono_type is None and style == 'monotype':
        tags.append('mono_none')
    for m in mons:
        del m['_protected']; del m['_flags']
    return {'id': 'pt_%s_%s_%s_%d' % (cp['key'], starter.lower(), style, n), 'source': 'playthrough', 'tags': tags,
            'format': 'singles', 'level_cap': max(m['level'] for m in mons),
            'notes': '%s (%s): %s; reference level %d' % (cp['label'], cp['boss'], desc, ref), 'mons': mons}


HMS_ALL = {'CUT', 'FLY', 'SURF', 'STRENGTH', 'FLASH', 'ROCK_SMASH', 'WATERFALL', 'DIVE'}


def generate(seed):
    teams = []
    for ci, cp in enumerate(CHECKPOINTS):
        for starter in STARTERS:
            for style, spec in STYLES.items():
                for n in range(1, spec[4] + 1):
                    rng = random.Random('%d:%s:%s:%s:%d' % (seed, cp['key'], starter, style, n))
                    teams.append(make_team(ci, starter, style, n, rng))
    return teams


def write_md(path, teams):
    by_cp = collections.Counter(t['tags'][0] for t in teams)
    by_style = collections.Counter(t['tags'][2] for t in teams)
    by_starter = collections.Counter(t['tags'][1] for t in teams)
    nmons = sum(len(t['mons']) for t in teams)
    sizes = collections.Counter(len(t['mons']) for t in teams)
    species = collections.Counter(m['species'] for t in teams for m in t['mons'])
    items = collections.Counter(m['item'] for t in teams for m in t['mons'])
    moves = collections.Counter(mv for t in teams for m in t['mons'] for mv in m['moves'])
    levels = [m['level'] for t in teams for m in t['mons']]
    trade = sum('assumes_trade' in t['tags'] for t in teams)
    legend = sum('legendary' in t['tags'] for t in teams)
    L = []
    L.append('# playthrough.jsonl\n')
    L.append('Teams a human player is likely to have at each point of a FireRed playthrough. Generated by '
             '`sim/tools/gen_playthrough_teams.py` (run it again to regenerate; `--seed` perturbs). Intended verdict: '
             '100% `ok` (every mon fully legal in FireRed).\n')
    L.append('Source data: gym/rival/Elite Four levels from `src/data/trainers.h` + `src/data/trainer_parties.h`; '
             'wild species and levels from `src/data/wild_encounters.json` (FireRed tables); TM/HM locations from '
             '`data/scripts/item_ball_scripts.inc` and `data/maps/*/scripts.inc`; level-up learnsets '
             '(`src/data/pokemon/level_up_learnsets.h`), evolutions (`evolution.h`), TM/HM compatibility '
             '(`tmhm_learnsets.h`), tutors (`tutor_learnsets.h`), types/base stats (`species_info.h`) and move '
             'power/type (`src/data/battle_moves.h`) are read at generation time. Gift Pokemon, in-game trades, '
             'stone availability and the "what has the player reached by now" ordering are hand-encoded from '
             'FireRed knowledge in `CHECKPOINTS`.\n')
    L.append('## Checkpoints\n')
    L.append('Reference level = the level a typical player\'s starter is at (gym ace level minus a little for the '
             'late-game spikes); starters are placed at ref + style offset, catches a few levels lower. Everything '
             'is cumulative down the table.\n')
    L.append('| # | key | point | boss (ace) | ref level | party size | new catches / gifts (catch level) | new TMs | new HMs | stones | tutors |')
    L.append('|---|---|---|---|---|---|---|---|---|---|---|')
    for i, c in enumerate(CHECKPOINTS):
        pool = ', '.join('%s%s (%d%s)' % (e[0].title().replace('_', ' '), '*' if 'gift' in e[4:] or 'fossil' in e[4:] else ('!' if 'legend' in e[4:] else ('~' if 'trade' in e[4:] else '')),
                                          e[1], '-%d' % e[2] if e[2] != e[1] else '') for e in c['pool'] if e[3] > 0)
        L.append('| %d | `%s` | %s | %s | %d | %d-%d | %s | %s | %s | %s | %s |' % (
            i, c['key'], c['label'], c['boss'], c['ref'], c['party'][0], c['party'][1], pool or '-',
            ', '.join(c['tms']) or '-', ', '.join(c['hms']) or '-', ', '.join(c['stones']) or '-', ', '.join(c['tutors']) or '-'))
    L.append('\n`*` gift / revived fossil (fixed level), `!` legendary, `~` in-game trade (OT = outsider). Sandshrew '
             'is listed with weight 0 (LeafGreen-only) so a perturbation can switch versions.\n')
    L.append('TM notes: single-use TMs are taught to at most one mon per team; Celadon Dept. Store TMs (05, 15, 16, 20, '
             '28, 31, 33, 43, 45) and Game Corner TMs (13, 23, 24, 30, 35) are reusable but weighted by cost. HM carry '
             'rates once available: ' + ', '.join('%s %.0f%%' % (k, v * 100) for k, v in HM_CARRY.items()) + '.\n')
    L.append('## Styles\n')
    L.append('| style | meaning | party size | starter level | other levels | variants |')
    L.append('|---|---|---|---|---|---|')
    for k, (d, sz, so, oo, nv) in STYLES.items():
        L.append('| `%s` | %s | %s | ref %+d..%+d | ref %+d..%+d | %d |' % (k, d, {'one': '1', 'small': '1 early, 2-3 later', 'mid': 'random in checkpoint range', 'max': 'checkpoint max'}[sz], so[0], so[1], oo[0], oo[1], nv))
    L.append('\n`balanced` variant 2 assumes trading (trade evolutions allowed; tagged `assumes_trade` when one '
             'actually happened). `speedrun` puts the HMs on a low-level carrier caught for the purpose.\n')
    L.append('## Per-mon generation\n')
    L.append('- catch level from the encounter table (starters at 5, gifts at their fixed level); the mon is then '
             'evolved the way the game would: level evolutions at the evolution level (starters always, others 94%%), stone '
             'evolutions once the stone is obtainable and not below the level the player has when reaching it (Moon Stone '
             'after Mt. Moon, the four Celadon stones from Celadon), Eevee usually gets a '
             'stone, friendship evolutions rarely and only late, trade evolutions only in `assumes_trade` teams.\n'
             '- moves: the caught form\'s last four level-up moves, then every level-up move along the timeline with a '
             '"forget the weakest move" rule (power x STAB x accuracy, status moves hand-valued, moves redundant with a '
             'stronger same-type move discounted); then up to two TMs from the inventory so far, weighted by usefulness, '
             'only if better than the weakest current move; rarely a tutor move; then the party\'s HMs.\n'
             '- IVs uniform 0-31, natures uniform, EVs 0..min(150, 4 x level) spread toward HP/Speed and the better '
             'attacking stat, happiness 150-230 for the starter and 100-200 otherwise, held item NONE %.0f%% of the '
             'time (otherwise a Berry, Quick Claw, Amulet Coin, Exp. Share, Soothe Bell, type boosters ... once obtainable).\n' % ((1 - P_ITEM) * 100))
    L.append('## Counts\n')
    L.append('- %d teams, %d mons, party sizes %s' % (len(teams), nmons, dict(sorted(sizes.items()))))
    L.append('- per checkpoint: ' + ', '.join('%s %d' % (c['key'], by_cp[c['key']]) for c in CHECKPOINTS))
    L.append('- per starter: ' + ', '.join('%s %d' % (k, v) for k, v in sorted(by_starter.items())))
    L.append('- per style: ' + ', '.join('%s %d' % (k, by_style[k]) for k in STYLES))
    L.append('- teams with a trade evolution (`assumes_trade`): %d; with a legendary: %d; with an in-game trade mon: %d' % (
        trade, legend, sum('ingame_trade' in t['tags'] for t in teams)))
    L.append('- levels: min %d, max %d, mean %.1f' % (min(levels), max(levels), sum(levels) / len(levels)))
    L.append('- distinct species %d, moves %d, items %d' % (len(species), len(moves), len(items)))
    L.append('- top species: ' + ', '.join('%s %d' % kv for kv in species.most_common(20)))
    L.append('- top moves: ' + ', '.join('%s %d' % kv for kv in moves.most_common(20)))
    L.append('- items: ' + ', '.join('%s %d' % kv for kv in items.most_common(12)))
    L.append('')
    open(path, 'w').write('\n'.join(L))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(SIM, 'teams', 'playthrough.jsonl'))
    ap.add_argument('--md', default=os.path.join(SIM, 'teams', 'playthrough.md'))
    ap.add_argument('--seed', type=int, default=1)
    a = ap.parse_args()
    teams = generate(a.seed)
    with open(a.out, 'w') as f:
        for t in teams:
            f.write(json.dumps(t, separators=(',', ':')) + '\n')
    if a.md:
        write_md(a.md, teams)
    print('%d teams -> %s' % (len(teams), a.out))


if __name__ == '__main__':
    main()
