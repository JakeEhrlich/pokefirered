#!/usr/bin/env python3
"""Generate FireRed teams the way Pokemon Showdown's [Gen 3] Random Battle does.

  gen_randbats_teams.py [--n 1500] [--seed 3] [--hp-ivs] [-o sim/teams/showdown_randbats.jsonl]

This is a Python port of data/random-battles/gen3/teams.ts (RandomGen3Teams) plus the parts of
gen4/teams.ts and gen9/teams.ts it inherits (queryMoves, addMove, incompatibleMoves, getPokemonPool,
getLevel, sample helpers).  The set data is sets.json from the same directory.  Raw copies of the
sources live in sim/teams/sources/showdown_randbats/ (commit recorded in SOURCE_COMMIT).

Species base stats / types, move type / power / accuracy / priority / effect and the type chart are
read from the game's own tables (src/data/pokemon/species_info.h, src/data/battle_moves.h,
src/battle_main.c) instead of Showdown's dex; for Gen 3 they agree (Deoxys formes are hard-coded).

Deliberate simplifications (see sim/teams/showdown_randbats.md):
  * Python's random.Random replaces Showdown's PRNG: same distribution, not the same streams.
  * Hidden Power stays MOVE_HIDDEN_POWER with IVs 31 (the intended type goes to tags) unless
    --hp-ivs is given, in which case Showdown's gen-3 HP IV spreads are written (they produce the
    intended type at 70 BP under the game's formula).
  * battleHasWobbuffet / battleHasDitto are per-team (each team is an independent battle side).
  * The unused monotype branch, shiny roll, gender, nickname and the 'forceofthefallenmod' rule
    are dropped.
"""
import argparse, json, os, re, random, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))          # sim/
REPO = os.path.dirname(ROOT)
SRC_DIR = os.path.join(ROOT, 'teams', 'sources', 'showdown_randbats')
SOURCE_COMMIT = '0490edb43b60fdaf2fad7db6010f1379f225e8e7'
SOURCE_URL = 'https://github.com/smogon/pokemon-showdown/tree/%s/data/random-battles/gen3' % SOURCE_COMMIT

# ----------------------------------------------------------------------------------------------
# constants copied from gen3/teams.ts (and the inherited lists it uses)
# ----------------------------------------------------------------------------------------------
RECOVERY_MOVES = ['milkdrink', 'moonlight', 'morningsun', 'recover', 'slackoff', 'softboiled', 'synthesis']
SETUP = ['acidarmor', 'agility', 'bellydrum', 'bulkup', 'calmmind', 'curse', 'dragondance', 'growth', 'howl',
         'irondefense', 'meditate', 'raindance', 'sunnyday', 'swordsdance', 'tailglow']
NO_STAB = ['eruption', 'explosion', 'fakeout', 'focuspunch', 'futuresight', 'icywind', 'knockoff', 'machpunch',
           'pursuit', 'quickattack', 'rapidspin', 'selfdestruct', 'skyattack', 'waterspout']
MOVE_PAIRS = [['sleeptalk', 'rest'], ['protect', 'wish'], ['leechseed', 'substitute'],
              ['focuspunch', 'substitute'], ['batonpass', 'spiderweb']]
NO_LEAD_POKEMON = ['dugtrio', 'wobbuffet']
# RandomGen4Teams.PRIORITY_POKEMON (gen3 inherits it from the gen4 constructor)
PRIORITY_POKEMON = ['cacturne', 'dusknoir', 'honchkrow', 'mamoswine', 'scizor', 'shedinja', 'shiftry']
MAX_TEAM_SIZE = 6
MAX_MOVE_COUNT = 4

TYPE_NAMES = ['Normal', 'Fighting', 'Flying', 'Poison', 'Ground', 'Rock', 'Bug', 'Ghost', 'Steel', 'Mystery',
              'Fire', 'Water', 'Grass', 'Electric', 'Psychic', 'Ice', 'Dragon', 'Dark']
GEN3_TYPES = [t for t in TYPE_NAMES if t != 'Mystery']          # dex.types.names() for gen 3
PHYSICAL_TYPES = {'Normal', 'Fighting', 'Flying', 'Poison', 'Ground', 'Rock', 'Bug', 'Ghost', 'Steel'}

# Showdown data/typechart.ts HPivs (gen 3+ spreads; everything not listed is 31)
HP_IVS = {
    'bug': {'atk': 30, 'def': 30, 'spd': 30}, 'dark': {}, 'dragon': {'atk': 30}, 'electric': {'spa': 30},
    'fighting': {'def': 30, 'spa': 30, 'spd': 30, 'spe': 30}, 'fire': {'atk': 30, 'spa': 30, 'spe': 30},
    'flying': {'hp': 30, 'atk': 30, 'def': 30, 'spa': 30, 'spd': 30}, 'ghost': {'def': 30, 'spd': 30},
    'grass': {'atk': 30, 'spa': 30}, 'ground': {'spa': 30, 'spd': 30}, 'ice': {'atk': 30, 'def': 30},
    'poison': {'def': 30, 'spa': 30, 'spd': 30}, 'psychic': {'atk': 30, 'spe': 30},
    'rock': {'def': 30, 'spd': 30, 'spe': 30}, 'steel': {'spd': 30}, 'water': {'atk': 30, 'def': 30, 'spa': 30},
}
HP_TYPE_ORDER = ['Fighting', 'Flying', 'Poison', 'Ground', 'Rock', 'Bug', 'Ghost', 'Steel', 'Fire', 'Water',
                 'Grass', 'Electric', 'Psychic', 'Ice', 'Dragon', 'Dark']

# Showdown id -> game constant where lowercase-strip-underscores does not match
MOVE_ID_OVERRIDES = {'highjumpkick': 'HI_JUMP_KICK', 'visegrip': 'VICE_GRIP', 'feintattack': 'FAINT_ATTACK',
                     'smellingsalts': 'SMELLING_SALT'}
# Moves Showdown treats as fixed damage (move.damage / damageCallback): they count as damaging moves
# but have no base power for STAB / coverage purposes.  Derived from the game's effect ids below.
FIXED_DAMAGE_EFFECTS = {'EFFECT_LEVEL_DAMAGE', 'EFFECT_PSYWAVE', 'EFFECT_SUPER_FANG', 'EFFECT_DRAGON_RAGE',
                        'EFFECT_SONICBOOM', 'EFFECT_COUNTER', 'EFFECT_MIRROR_COAT', 'EFFECT_ENDEAVOR'}
# game power > 0 but Showdown basePower 0 and no callback
NO_POWER_EFFECTS = {'EFFECT_BIDE', 'EFFECT_OHKO'}
RECOIL_EFFECTS = {'EFFECT_RECOIL', 'EFFECT_DOUBLE_EDGE', 'EFFECT_RECOIL_IF_MISS'}
DEOXYS_FORMES = {   # hp atk def spe spa spd (game order); Showdown ids
    'deoxys': (50, 150, 50, 150, 150, 50),
    'deoxysattack': (50, 180, 20, 150, 180, 20),
    'deoxysdefense': (50, 70, 160, 90, 70, 160),
    'deoxysspeed': (50, 95, 90, 180, 95, 90),
}
STAT_KEYS = ['hp', 'atk', 'def', 'spe', 'spa', 'spd']       # game order


# ----------------------------------------------------------------------------------------------
# game data
# ----------------------------------------------------------------------------------------------
def load_moves():
    """id -> dict(const, type, bp, accuracy, priority, category, fixed, recoil)"""
    src = open(os.path.join(REPO, 'src/data/battle_moves.h')).read()
    out = {}
    for m in re.finditer(r'\[MOVE_(\w+)\]\s*=\s*\{(.*?)\n\s*\},', src, re.S):
        const, body = m.group(1), m.group(2)
        f = lambda k: re.search(r'\.%s\s*=\s*(-?\w+)' % k, body).group(1)
        effect = f('effect')
        power = int(f('power'))
        mid = const.lower().replace('_', '')
        for k, v in MOVE_ID_OVERRIDES.items():
            if v == const:
                mid = k
        fixed = effect in FIXED_DAMAGE_EFFECTS
        if fixed or effect in NO_POWER_EFFECTS:
            power = 0
        typ = f('type')[5:].capitalize()            # TYPE_FIRE -> Fire
        out[mid] = dict(const=const, type=typ, bp=power, accuracy=int(f('accuracy')), priority=int(f('priority')),
                        category=('Status' if power == 0 and not fixed else ('Physical' if typ in PHYSICAL_TYPES else 'Special')),
                        fixed=fixed, recoil=effect in RECOIL_EFFECTS)
    for t in HP_IVS:
        out['hiddenpower' + t] = dict(const='HIDDEN_POWER', type=t.capitalize(), bp=70, accuracy=100, priority=0,
                                      category='Physical' if t.capitalize() in PHYSICAL_TYPES else 'Special',
                                      fixed=False, recoil=False, hptype=t.capitalize())
    return out


def load_species():
    """id -> dict(const, types, stats[6 game order], abilities[const names])"""
    src = open(os.path.join(REPO, 'src/data/pokemon/species_info.h')).read()
    out = {}
    for m in re.finditer(r'\[SPECIES_(\w+)\]\s*=\s*\{(.*?)\n\s*\},', src, re.S):
        const, body = m.group(1), m.group(2)
        if const.startswith('OLD_UNOWN') or const == 'NONE':
            continue
        g = lambda k: int(re.search(r'\.%s\s*=\s*(\d+)' % k, body).group(1))
        types = re.search(r'\.types\s*=\s*\{TYPE_(\w+),\s*TYPE_(\w+)\}', body).groups()
        types = [t.capitalize() for t in types]
        if types[1] == types[0]:
            types = types[:1]
        ab = re.search(r'\.abilities\s*=\s*\{ABILITY_(\w+),\s*ABILITY_(\w+)\}', body).groups()
        out[const.lower().replace('_', '')] = dict(
            const=const, types=types, abilities=[a for a in ab if a != 'NONE'],
            stats=[g('baseHP'), g('baseAttack'), g('baseDefense'), g('baseSpeed'), g('baseSpAttack'), g('baseSpDefense')])
    for forme, stats in DEOXYS_FORMES.items():
        out[forme] = dict(out['deoxys'], stats=list(stats))
    return out


def load_type_chart():
    """(attacker, defender) -> +1 super effective / -1 not very / 0 (neutral or immune), like Showdown's
    dex.getEffectiveness which ignores immunities."""
    src = open(os.path.join(REPO, 'src/battle_main.c')).read()
    body = src[src.index('gTypeEffectiveness['):]
    body = body[:body.index('};')]
    chart = {}
    for a, d, mul in re.findall(r'TYPE_(\w+),\s*TYPE_(\w+),\s*TYPE_MUL_(\w+)', body):
        if a in ('FORESIGHT_TARGET', 'ENDTABLE'):
            continue
        chart[(a.capitalize(), d.capitalize())] = {'SUPER_EFFECTIVE': 1, 'NOT_EFFECTIVE': -1}.get(mul, 0)
    return chart


MOVES = load_moves()
SPECIES = load_species()
CHART = load_type_chart()
SETS = json.load(open(os.path.join(SRC_DIR, 'sets.json')))


def effectiveness(attack_type, species):
    return sum(CHART.get((attack_type, t), 0) for t in species['types'])


def hp_type_and_power(ivs):
    """The game's Hidden Power formula on IVs in game order (HP, Atk, Def, Spe, SpA, SpD)."""
    tbits = sum(((iv & 1) << i) for i, iv in enumerate(ivs))
    pbits = sum((((iv >> 1) & 1) << i) for i, iv in enumerate(ivs))
    return HP_TYPE_ORDER[tbits * 15 // 63], pbits * 40 // 63 + 30


# ----------------------------------------------------------------------------------------------
# the generator
# ----------------------------------------------------------------------------------------------
class MoveCounter(dict):
    def __init__(self):
        super().__init__()
        self.damaging_moves = []        # move ids, insertion order (Showdown: Set<Move>)
        self.base_power_moves = []

    def get(self, k, default=0):
        return super().get(k, default)

    def add(self, k):
        self[k] = self.get(k) + 1


class RandomGen3Teams:
    def __init__(self, rng, hp_ivs=False):
        self.rng = rng
        self.hp_ivs = hp_ivs
        self.no_stab = NO_STAB
        self.battle_has_wobbuffet = False
        self.battle_has_ditto = False
        self.status_moves = [m for m, d in MOVES.items() if d['category'] == 'Status']
        self.move_enforcement_checkers = {
            'Bug': lambda mp, mv, ab, ty, c, sp: not c.get('Bug') and sp['id'] in ('armaldo', 'heracross', 'parasect'),
            'Dark': lambda mp, mv, ab, ty, c, sp: not c.get('Dark'),
            'Electric': lambda mp, mv, ab, ty, c, sp: not c.get('Electric'),
            'Fighting': lambda mp, mv, ab, ty, c, sp: not c.get('Fighting'),
            'Fire': lambda mp, mv, ab, ty, c, sp: not c.get('Fire'),
            'Flying': lambda mp, mv, ab, ty, c, sp: not c.get('Flying') and sp['id'] != 'crobat',
            'Ghost': lambda mp, mv, ab, ty, c, sp: not c.get('Ghost'),
            'Ground': lambda mp, mv, ab, ty, c, sp: not c.get('Ground'),
            'Ice': lambda mp, mv, ab, ty, c, sp: not c.get('Ice'),
            'Normal': lambda mp, mv, ab, ty, c, sp: not c.get('Normal'),
            'Poison': lambda mp, mv, ab, ty, c, sp: not c.get('Poison'),
            'Psychic': lambda mp, mv, ab, ty, c, sp: not c.get('Psychic') and sp['stats'][4] >= 100,
            'Rock': lambda mp, mv, ab, ty, c, sp: not c.get('Rock'),
            'Steel': lambda mp, mv, ab, ty, c, sp: not c.get('Steel') and sp['id'] != 'forretress',
            'Water': lambda mp, mv, ab, ty, c, sp: not c.get('Water'),
        }

    # --- PRNG helpers (gen9/teams.ts) ---
    def random_chance(self, num, den):
        return self.rng.randrange(den) < num

    def sample(self, items):
        return items[self.rng.randrange(len(items))]

    def fast_pop(self, lst, index):
        if index < 0 or index >= len(lst):
            raise IndexError('Index %d out of bounds' % index)
        el = lst[index]
        lst[index] = lst[-1]
        lst.pop()
        return el

    def sample_no_replace(self, lst):
        if not lst:
            return None
        return self.fast_pop(lst, self.rng.randrange(len(lst)))

    # --- moves ---
    @staticmethod
    def move_type(moveid):
        return MOVES[moveid]['type']

    def query_moves(self, moves, species, preferred_type, abilities):
        counter = MoveCounter()
        types = set(species['types'])
        if not moves:
            return counter
        categories = {'Physical': 0, 'Special': 0, 'Status': 0}
        for moveid in moves:
            mv = MOVES[moveid]
            move_type = mv['type']
            if mv['fixed']:
                counter.add('damage')
                counter.damaging_moves.append(moveid)
            else:
                categories[mv['category']] += 1
            if mv['recoil']:
                counter.add('recoil')
            if mv['bp']:
                counter.base_power_moves.append(moveid)
                if moveid not in self.no_stab or (species['id'] in PRIORITY_POKEMON and mv['priority'] > 0):
                    counter.add(move_type)
                    if move_type in types:
                        counter.add('stab')
                    counter.damaging_moves.append(moveid)
                if mv['priority'] > 0:
                    counter.add('priority')
            if mv['accuracy'] and mv['accuracy'] < 90:
                counter.add('inaccurate')
            if moveid in RECOVERY_MOVES:
                counter.add('recovery')
            if moveid in SETUP:
                counter.add('setup')
        counter['Physical'] = categories['Physical']
        counter['Special'] = categories['Special']
        counter['Status'] = categories['Status']
        return counter

    def incompatible_moves(self, moves, move_pool, moves_a, moves_b):
        arr_a = moves_a if isinstance(moves_a, list) else [moves_a]
        arr_b = moves_b if isinstance(moves_b, list) else [moves_b]
        if len(moves) + len(move_pool) <= MAX_MOVE_COUNT:
            return
        for moveid1 in list(moves):
            if moveid1 in arr_b:
                for moveid2 in arr_a:
                    if moveid1 != moveid2 and moveid2 in move_pool:
                        self.fast_pop(move_pool, move_pool.index(moveid2))
                        if len(moves) + len(move_pool) <= MAX_MOVE_COUNT:
                            return
            if moveid1 in arr_a:
                for moveid2 in arr_b:
                    if moveid1 != moveid2 and moveid2 in move_pool:
                        self.fast_pop(move_pool, move_pool.index(moveid2))
                        if len(moves) + len(move_pool) <= MAX_MOVE_COUNT:
                            return

    def cull_move_pool(self, types, moves, abilities, counter, move_pool, team_details, species, is_lead,
                       preferred_type, role):
        has_hidden_power = any(m.startswith('hiddenpower') for m in moves)
        if has_hidden_power:
            while True:
                hp = [m for m in move_pool if m.startswith('hiddenpower')]
                if not hp:
                    break
                self.fast_pop(move_pool, move_pool.index(hp[0]))

        if len(moves) + len(move_pool) <= MAX_MOVE_COUNT:
            return
        if len(moves) == MAX_MOVE_COUNT - 2:
            unpaired = list(move_pool)
            for a, b in MOVE_PAIRS:
                if a in move_pool and b in move_pool:
                    self.fast_pop(unpaired, unpaired.index(a))
                    self.fast_pop(unpaired, unpaired.index(b))
            if len(unpaired) == 1:
                self.fast_pop(move_pool, move_pool.index(unpaired[0]))
        if len(moves) == MAX_MOVE_COUNT - 1:
            for a, b in MOVE_PAIRS:
                if a in move_pool and b in move_pool:
                    self.fast_pop(move_pool, move_pool.index(a))
                    self.fast_pop(move_pool, move_pool.index(b))

        if team_details.get('rapidSpin'):
            if 'rapidspin' in move_pool:
                self.fast_pop(move_pool, move_pool.index('rapidspin'))
            if len(moves) + len(move_pool) <= MAX_MOVE_COUNT:
                return
        if team_details.get('spikes', 0) >= 2:
            if 'spikes' in move_pool:
                self.fast_pop(move_pool, move_pool.index('spikes'))
            if len(moves) + len(move_pool) <= MAX_MOVE_COUNT:
                return
        if team_details.get('statusCure'):
            for m in ('aromatherapy', 'healbell'):
                if m in move_pool:
                    self.fast_pop(move_pool, move_pool.index(m))
            if len(moves) + len(move_pool) <= MAX_MOVE_COUNT:
                return

        bad_with_setup = ['knockoff', 'rapidspin', 'toxic']
        incompatible_pairs = [
            [self.status_moves, 'trick'],
            [SETUP, bad_with_setup],
            ['rest', ['protect', 'substitute']],
            [['selfdestruct', 'explosion'], ['destinybond', 'painsplit', 'rest']],
            ['surf', 'hydropump'],
            [['bodyslam', 'return'], ['bodyslam', 'doubleedge']],
            ['fireblast', 'flamethrower'],
            ['bulkup', 'overheat'],
            ['endure', 'substitute'],
        ]
        for a, b in incompatible_pairs:
            self.incompatible_moves(moves, move_pool, a, b)
        status_inflicting = ['stunspore', 'thunderwave', 'toxic', 'willowisp', 'yawn']
        if role != 'Staller':
            self.incompatible_moves(moves, move_pool, status_inflicting, status_inflicting)

    def add_move(self, move, moves, types, abilities, team_details, species, is_lead, move_pool, preferred_type, role):
        moves[move] = True            # dict keeps insertion order like a JS Set
        self.fast_pop(move_pool, move_pool.index(move))
        counter = self.query_moves(moves, species, preferred_type, abilities)
        self.cull_move_pool(types, moves, abilities, counter, move_pool, team_details, species, is_lead,
                            preferred_type, role)
        return counter

    def stab_candidates(self, move_pool, want_types):
        return [m for m in move_pool if m not in self.no_stab and MOVES[m]['bp'] and MOVES[m]['type'] in want_types]

    def random_moveset(self, types, abilities, team_details, species, is_lead, move_pool, preferred_type, role):
        preferred_types = preferred_type.split(',') if preferred_type else []
        moves = {}
        counter = self.query_moves(moves, species, preferred_type, abilities)
        self.cull_move_pool(types, moves, abilities, counter, move_pool, team_details, species, is_lead,
                            preferred_type, role)
        args = lambda: (moves, types, abilities, team_details, species, is_lead, move_pool, preferred_type, role)

        if len(move_pool) <= MAX_MOVE_COUNT or species['id'] == 'unown':
            while move_pool:
                counter = self.add_move(self.sample(move_pool), *args())
            return moves

        def run_checker(name):
            f = self.move_enforcement_checkers.get(name)
            return bool(f and f(move_pool, moves, abilities, types, counter, species))

        for moveid in ('seismictoss', 'spikes', 'spore'):
            if moveid in move_pool:
                counter = self.add_move(moveid, *args())

        if 'Setup' not in role:
            if 'batonpass' in move_pool and 'substitute' in move_pool:
                counter = self.add_move('substitute', *args())

        for t in preferred_types:
            if not counter.get(t):
                stab = self.stab_candidates(move_pool, {t})
                if stab:
                    counter = self.add_move(self.sample(stab), *args())

        for t in types:
            stab = self.stab_candidates(move_pool, {t})
            while run_checker(t):
                if not stab:
                    break
                counter = self.add_move(self.sample_no_replace(stab), *args())

        if not counter.get('stab'):
            stab = self.stab_candidates(move_pool, set(types))
            if stab:
                counter = self.add_move(self.sample(stab), *args())

        if role in ('Bulky Support', 'Bulky Attacker', 'Bulky Setup', 'Staller'):
            rec = [m for m in move_pool if m in RECOVERY_MOVES]
            if rec:
                counter = self.add_move(self.sample(rec), *args())

        if role == 'Staller':
            for m in ('protect', 'toxic', 'wish'):
                if m in move_pool:
                    counter = self.add_move(m, *args())

        if 'Setup' in role or role == 'Berry Sweeper':
            setup = [m for m in move_pool if m in SETUP]
            if setup:
                counter = self.add_move(self.sample(setup), *args())

        if role == 'Berry Sweeper':
            for m in ('flail', 'reversal'):
                if m in move_pool:
                    counter = self.add_move(m, *args())
            hp_control = [m for m in move_pool if m in ('endure', 'substitute')]
            if hp_control:
                counter = self.add_move(self.sample(hp_control), *args())

        if not counter.damaging_moves:
            attacking = [m for m in move_pool if m not in self.no_stab and MOVES[m]['category'] != 'Status']
            if attacking:
                counter = self.add_move(self.sample(attacking), *args())

        if role in ('Fast Attacker', 'Setup Sweeper', 'Bulky Attacker', 'Wallbreaker', 'Berry Sweeper'):
            if len(counter.damaging_moves) == 1:
                current = MOVES[counter.damaging_moves[0]]['type']
                coverage = [m for m in move_pool if m not in self.no_stab and MOVES[m]['bp'] and MOVES[m]['type'] != current]
                if coverage:
                    counter = self.add_move(self.sample(coverage), *args())

        while len(moves) < MAX_MOVE_COUNT and move_pool:
            moveid = self.sample(move_pool)
            counter = self.add_move(moveid, *args())
            for a, b in MOVE_PAIRS:
                if moveid == a and b in move_pool:
                    counter = self.add_move(b, *args())
                if moveid == b and a in move_pool:
                    counter = self.add_move(a, *args())
        return moves

    # --- ability / item ---
    def should_cull_ability(self, ability, counter, team_details):
        if ability == 'Chlorophyll':
            return not team_details.get('sun')
        if ability == 'Rock Head':
            return not counter.get('recoil')
        if ability == 'Swift Swim':
            return not team_details.get('rain')
        return False

    def get_ability(self, moves, abilities, counter, team_details, species):
        if len(abilities) <= 1:
            return abilities[0]
        if species['id'] == 'yanma':
            return 'Compound Eyes' if counter.get('inaccurate') else 'Speed Boost'
        allowed = [a for a in abilities if not self.should_cull_ability(a, counter, team_details)]
        if allowed:
            return self.sample(allowed)
        weather = [a for a in abilities if a in ('Chlorophyll', 'Swift Swim')]
        if weather:
            return self.sample(weather)
        return self.sample(abilities)

    def get_item(self, ability, moves, counter, species, role):
        sid = species['id']
        spe = species['stats'][3]
        if sid in ('latias', 'latios'):
            return 'Soul Dew'
        if sid == 'linoone' and role == 'Setup Sweeper':
            return 'Silk Scarf'
        if sid == 'marowak':
            return 'Thick Club'
        if sid == 'pikachu':
            return 'Light Ball'
        if sid == 'unown':
            return 'Choice Band' if counter.get('Physical') else 'Twisted Spoon'
        if sid in ('deoxys', 'deoxysattack'):
            return 'White Herb'
        if 'trick' in moves:
            return 'Choice Band'
        if counter.get('Physical') >= 4:
            return 'Choice Band'
        if counter.get('Physical') >= 3 and ('batonpass' in moves or (role == 'Wallbreaker' and counter.get('Special'))):
            return 'Choice Band'
        if sid == 'shedinja':
            return 'Lum Berry'
        if 'dragondance' in moves and ability != 'Natural Cure' and 'healbell' not in moves and 'substitute' not in moves:
            return 'Lum Berry'
        if 'bellydrum' in moves:
            return 'Salac Berry' if 'substitute' in moves else 'Lum Berry'
        if 'raindance' in moves and counter.get('Special') >= 3:
            return 'Petaya Berry'
        if role == 'Berry Sweeper':
            if 'endure' in moves:
                return 'Salac Berry'
            if 'flail' in moves or 'reversal' in moves:
                return 'Liechi Berry' if spe >= 90 else 'Salac Berry'
            if 'substitute' in moves and counter.get('Physical') >= 3:
                return 'Liechi Berry'
            if 'substitute' in moves and counter.get('Special') >= 3:
                return 'Petaya Berry'
        if sid == 'farfetchd':
            return 'Stick'
        salac_reqs = 60 <= spe <= 100 and not counter.get('priority')
        if 'swordsdance' in moves and 'substitute' in moves and counter.get('Status') == 2:
            if salac_reqs:
                return 'Salac Berry'
            if spe > 100 and counter.get('Physical') >= 2:
                return 'Liechi Berry'
        if 'swordsdance' in moves and counter.get('Status') == 1:
            if salac_reqs:
                return 'Salac Berry'
            if spe > 100:
                return 'Liechi Berry' if (counter.get('Physical') >= 3 and self.random_chance(1, 2)) else 'Lum Berry'
        return 'Leftovers'

    @staticmethod
    def get_level(sid):
        return SETS[sid].get('level') or 80

    # --- set ---
    def random_set(self, sid, team_details, is_lead=False):
        species = dict(SPECIES[sid], id=sid)
        s = self.sample(SETS[sid]['sets'])
        role = s['role']
        move_pool = list(s['movepool'])
        preferred_type = ','.join(s['preferredTypes']) if s.get('preferredTypes') else ''
        evs = {'hp': 85, 'atk': 85, 'def': 85, 'spa': 85, 'spd': 85, 'spe': 85}
        ivs = {'hp': 31, 'atk': 31, 'def': 31, 'spa': 31, 'spd': 31, 'spe': 31}
        types = list(species['types'])
        abilities = s['abilities']

        moves = self.random_moveset(types, abilities, team_details, species, is_lead, move_pool, preferred_type, role)
        counter = self.query_moves(moves, species, preferred_type, abilities)
        ability = self.get_ability(moves, abilities, counter, team_details, species)
        item = self.get_item(ability, moves, counter, species, role)
        level = self.get_level(sid)

        hp_type = None
        for m in moves:
            if m.startswith('hiddenpower'):
                hp_type = m[11:]
        if hp_type and self.hp_ivs:
            ivs.update(HP_IVS[hp_type])

        base_hp = species['stats'][0]
        calc_hp = lambda: ((2 * base_hp + ivs['hp'] + evs['hp'] // 4 + 100) * level) // 100 + 10
        pinch = item in ('Salac Berry', 'Petaya Berry', 'Liechi Berry')
        while evs['hp'] > 1:
            hp = calc_hp()
            if 'substitute' in moves and ('flail' in moves or 'reversal' in moves):
                if hp % 4 > 0:
                    break
            elif 'substitute' in moves and pinch:
                if hp % 4 == 0:
                    break
            elif 'bellydrum' in moves:
                if hp % 2 > 0:
                    break
            else:
                break
            evs['hp'] -= 4

        if not counter.get('Physical') and 'transform' not in moves:
            evs['atk'] = 0
            ivs['atk'] = ((ivs['atk'] or 31) - 28) if hp_type else 0
            if hp_type and not self.hp_ivs:
                ivs['atk'] = 31       # keep the all-31 spread the dataset promises

        hp = calc_hp()
        if 'substitute' in moves and any(m in moves for m in ('endeavor', 'flail', 'reversal')):
            if hp % 4 == 0:
                evs['hp'] -= 4
        elif 'substitute' in moves and pinch:
            while hp % 4 > 0:
                evs['hp'] -= 4
                hp = calc_hp()

        shuffled = list(moves)
        self.rng.shuffle(shuffled)
        return dict(species=sid, level=level, moves=shuffled, ability=ability, item=item, evs=evs, ivs=ivs,
                    role=role, hp_type=hp_type, base_species='deoxys' if sid.startswith('deoxys') else sid)

    # --- team ---
    def compatible(self, sid, pokemon):
        reversal_users = ['raticate', 'primeape', 'hitmonlee', 'furret', 'yanma', 'heracross', 'blaziken', 'medicham']
        flail_users = ['dodrio', 'farfetchd']
        pairs = [(['shedinja'], ['tyranitar']), (flail_users + reversal_users, ['tyranitar'])]
        have = [p['species'] for p in pokemon]
        for a, b in pairs:
            if sid in b and any(m in a for m in have):
                return False
            if sid in a and any(m in b for m in have):
                return False
        return True

    def random_team(self):
        self.battle_has_wobbuffet = False
        self.battle_has_ditto = False
        pokemon = []
        base_formes, type_count, type_weak, type_double_weak = {}, {}, {}, {}
        team_details = {}
        num_max_level = 0

        # getPokemonPool: group formes by base species, weight = min(ceil(n/3), 3)
        pool = {}
        for sid in SETS:
            base = 'deoxys' if sid.startswith('deoxys') else sid
            pool.setdefault(base, []).append(sid)
        base_pool = []
        for base, formes in pool.items():
            base_pool += [base] * min(-(-len(formes) // 3), 3)

        leads_remaining = 1
        while base_pool and len(pokemon) < MAX_TEAM_SIZE:
            base = self.sample_no_replace(base_pool)
            sid = self.sample(pool[base])
            species = SPECIES[sid]
            if base_formes.get(base):
                continue
            if sid == 'wobbuffet' and self.battle_has_wobbuffet:
                continue
            types = species['types']

            limit_factor = 1
            if any(type_count.get(t, 0) >= 2 * limit_factor for t in types):
                continue
            skip = False
            for t in GEN3_TYPES:
                eff = effectiveness(t, species)
                if eff > 0:
                    type_weak.setdefault(t, 0)
                    if type_weak[t] >= 3 * limit_factor:
                        skip = True
                        break
                if eff > 1:
                    type_double_weak.setdefault(t, 0)
                    if type_double_weak[t] >= limit_factor:
                        skip = True
                        break
            if skip:
                continue
            if self.get_level(sid) == 100 and num_max_level >= limit_factor:
                continue
            if not self.compatible(sid, pokemon):
                continue

            if leads_remaining:
                if sid in NO_LEAD_POKEMON:
                    if len(pokemon) + leads_remaining == MAX_TEAM_SIZE:
                        continue
                    s = self.random_set(sid, team_details, False)
                    pokemon.append(s)
                else:
                    s = self.random_set(sid, team_details, True)
                    pokemon.insert(0, s)
                    leads_remaining -= 1
            else:
                s = self.random_set(sid, team_details, False)
                pokemon.append(s)

            if len(pokemon) == MAX_TEAM_SIZE:
                break

            base_formes[base] = 1
            for t in types:
                type_count[t] = type_count.get(t, 0) + 1
            for t in GEN3_TYPES:
                eff = effectiveness(t, species)
                if eff > 0:
                    type_weak[t] = type_weak.get(t, 0) + 1
                if eff > 1:
                    type_double_weak[t] = type_double_weak.get(t, 0) + 1
            if s['level'] == 100:
                num_max_level += 1
            if s['ability'] == 'Drizzle' or 'raindance' in s['moves']:
                team_details['rain'] = 1
            if s['ability'] == 'Drought' or 'sunnyday' in s['moves']:
                team_details['sun'] = 1
            if s['ability'] == 'Sand Stream':
                team_details['sand'] = 1
            if 'aromatherapy' in s['moves'] or 'healbell' in s['moves']:
                team_details['statusCure'] = 1
            if 'spikes' in s['moves']:
                team_details['spikes'] = 1
            if 'rapidspin' in s['moves']:
                team_details['rapidSpin'] = 1
            if sid == 'wobbuffet':
                self.battle_has_wobbuffet = True
            if sid == 'ditto':
                self.battle_has_ditto = True

        if len(pokemon) < MAX_TEAM_SIZE:
            raise RuntimeError('could not build a team')
        return pokemon


# ----------------------------------------------------------------------------------------------
# conversion to the corpus format
# ----------------------------------------------------------------------------------------------
def const_name(s):
    return re.sub(r'[^A-Z0-9]+', '_', s.upper()).strip('_')


def to_mon(s):
    sp = SPECIES[s['species']]
    tags = ['role:' + s['role'].lower().replace(' ', '_')]
    if s['species'].startswith('deoxys'):
        tags.append('forme:' + (s['species'][6:] or 'normal'))
    if s['hp_type']:
        tags.append('hp:' + s['hp_type'])
    ability = const_name(s['ability'])
    if ability not in sp['abilities']:
        tags.append('ability_mismatch')
    mon = {
        'species': sp['const'], 'level': s['level'],
        'moves': [MOVES[m]['const'] for m in s['moves']],
        'item': const_name(s['item']), 'ability': ability, 'nature': 'HARDY',
        'ivs': [s['ivs'][k] for k in STAT_KEYS], 'evs': [s['evs'][k] for k in STAT_KEYS],
        'happiness': 255,
    }
    if sp['const'] in ('MEW', 'DEOXYS'):
        mon['fateful'] = True
    if tags:
        mon['tags'] = tags
    return mon


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--n', type=int, default=1500)
    ap.add_argument('--seed', type=int, default=3)
    ap.add_argument('--hp-ivs', action='store_true', help='write gen-3 Hidden Power IV spreads instead of all-31')
    ap.add_argument('-o', '--out', default=os.path.join(ROOT, 'teams', 'showdown_randbats.jsonl'))
    a = ap.parse_args()

    # sanity: every set move / ability / species resolves
    for sid, d in SETS.items():
        assert sid in SPECIES, sid
        for s in d['sets']:
            for m in s['movepool']:
                assert m in MOVES, (sid, m)
    for t, spread in HP_IVS.items():
        ivs = [spread.get(k, 31) for k in STAT_KEYS]
        assert hp_type_and_power(ivs) == (t.capitalize(), 70), (t, hp_type_and_power(ivs))

    gen = RandomGen3Teams(random.Random(a.seed), hp_ivs=a.hp_ivs)
    note = ('Generated by sim/tools/gen_randbats_teams.py (seed %d), a port of Pokemon Showdown [Gen 3] Random '
            'Battle: %s (sets.json + teams.ts @ %s)' % (a.seed, SOURCE_URL, SOURCE_COMMIT[:12]))
    with open(a.out, 'w') as f:
        for i in range(1, a.n + 1):
            team = gen.random_team()
            tags = ['gen3', 'randbats']
            for p in team:
                if p['species'].startswith('deoxys'):
                    tags.append('deoxys-' + (p['species'][6:] or 'normal'))
            obj = {'id': 'randbats_%d' % i, 'source': 'showdown_randbats', 'tags': tags, 'format': 'singles',
                   'level_cap': 100, 'notes': note, 'mons': [to_mon(p) for p in team]}
            f.write(json.dumps(obj, separators=(',', ':')) + '\n')
    print('wrote %d teams to %s' % (a.n, a.out))


if __name__ == '__main__':
    main()
