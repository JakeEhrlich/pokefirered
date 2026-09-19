#!/usr/bin/env python3
"""Random team generator for the team corpus (format: sim/teams/FORMAT.md).

  gen_random_teams.py --mode valid --count N --seed S [--out FILE] [--prefix rv]

Modes are registered in MODES; `valid` produces mons that pass every FireRed rule (verdict `ok`
from `tools/teams.py check`). Other modes (e.g. `encodable`) can be added by registering another
mon generator in MODES without touching the team/level/IV/EV plumbing.

Legality data comes from build/teamcheck (--species, --items, --abilities, --moves); results are
cached per process so a (species, level) pair costs one subprocess call.
"""
import argparse, json, os, random, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))          # sim/
TEAMCHECK = os.path.join(ROOT, 'build', 'teamcheck')

NATURES = ['HARDY', 'LONELY', 'BRAVE', 'ADAMANT', 'NAUGHTY', 'BOLD', 'DOCILE', 'RELAXED', 'IMPISH', 'LAX',
           'TIMID', 'HASTY', 'SERIOUS', 'JOLLY', 'NAIVE', 'MODEST', 'MILD', 'QUIET', 'BASHFUL', 'RASH',
           'CALM', 'GENTLE', 'SASSY', 'CAREFUL', 'QUIRKY']
FATEFUL_SPECIES = {'MEW', 'DEOXYS'}

# ---- distributions (documented in teams/random_valid.md) --------------------------------------
LEVEL_BUCKETS = [(0.20, 1, 20), (0.30, 21, 50), (0.30, 51, 80), (0.20, 81, 100)]   # (weight, lo, hi)
MOVE_COUNT_WEIGHTS = {4: 0.80, 3: 0.10, 2: 0.06, 1: 0.04}                          # capped by the legal list
P_ITEM_NONE = 0.40
P_EVS_ZERO, P_EVS_RANDOM = 0.50, 0.25                                              # remainder: competitive
P_HAPPINESS_RANDOM = 0.30
DEFAULT_HAPPINESS = 70
P_TEAM_SIX = 0.70


# ---- teamcheck-backed legality tables -----------------------------------------------------------
class Tables:
    def __init__(self, exe=TEAMCHECK):
        if not os.path.exists(exe):
            sys.exit('%s missing: build the sim tools first' % exe)
        self.exe = exe
        self._moves = {}
        self._abilities = {}
        self.species = [s for s in self._run('--species') if s != '?']       # 386 real species
        self.items = self._run('--items')                                     # holdable items

    def _run(self, *args):
        out = subprocess.run([self.exe] + list(args), capture_output=True, text=True, check=True).stdout
        return [l for l in out.splitlines() if l.strip()]

    def moves(self, species, level):
        key = (species, level)
        if key not in self._moves:
            self._moves[key] = self._run('--moves', species, str(level))
        return self._moves[key]

    def abilities(self, species):
        if species not in self._abilities:
            self._abilities[species] = self._run('--abilities', species)
        return self._abilities[species]


# ---- samplers ----------------------------------------------------------------------------------
def weighted_choice(rng, pairs):
    """pairs: iterable of (weight, value)."""
    pairs = list(pairs)
    r = rng.random() * sum(w for w, _ in pairs)
    for w, v in pairs:
        r -= w
        if r < 0:
            return v
    return pairs[-1][1]


def sample_level(rng):
    lo, hi = weighted_choice(rng, ((w, (lo, hi)) for w, lo, hi in LEVEL_BUCKETS))
    return rng.randint(lo, hi)


def sample_ivs(rng):
    return [rng.randint(0, 31) for _ in range(6)]


def sample_evs_random(rng):
    """A uniformly random total in 0..510 split at random cut points; reject parts over 255."""
    while True:
        total = rng.randint(0, 510)
        cuts = sorted(rng.randint(0, total) for _ in range(5))
        parts = [b - a for a, b in zip([0] + cuts, cuts + [total])]
        if max(parts) <= 255:
            return parts


def sample_evs_competitive(rng):
    stats = rng.sample(range(6), 3)
    evs = [0] * 6
    evs[stats[0]] = evs[stats[1]] = 252
    evs[stats[2]] = rng.choice((4, 6))
    return evs


def sample_evs(rng):
    r = rng.random()
    if r < P_EVS_ZERO:
        return [0] * 6
    if r < P_EVS_ZERO + P_EVS_RANDOM:
        return sample_evs_random(rng)
    return sample_evs_competitive(rng)


def sample_moves(rng, legal):
    want = weighted_choice(rng, ((w, n) for n, w in MOVE_COUNT_WEIGHTS.items()))
    return rng.sample(legal, min(want, len(legal)))


def sample_happiness(rng):
    return rng.randint(0, 255) if rng.random() < P_HAPPINESS_RANDOM else DEFAULT_HAPPINESS


def sample_item(rng, tables):
    return 'NONE' if rng.random() < P_ITEM_NONE else rng.choice(tables.items)


# ---- mon generators (one per --mode) ------------------------------------------------------------
def gen_mon_valid(rng, tables):
    species = rng.choice(tables.species)
    level = sample_level(rng)
    legal = tables.moves(species, level)
    if not legal:                                   # cannot happen in FRLG, but stay safe
        return gen_mon_valid(rng, tables)
    mon = {
        'species': species,
        'level': level,
        'moves': sample_moves(rng, legal),
        'item': sample_item(rng, tables),
        'ability': rng.choice(tables.abilities(species)),
        'nature': rng.choice(NATURES),
        'ivs': sample_ivs(rng),
        'evs': sample_evs(rng),
        'happiness': sample_happiness(rng),
    }
    if species in FATEFUL_SPECIES:
        mon['fateful'] = True
    return mon


MODES = {
    'valid': {'gen_mon': gen_mon_valid, 'source': 'random_valid', 'tags': ['random', 'valid'], 'prefix': 'rv'},
    # 'encodable': to be added (mons that fit struct Pokemon but may break FireRed rules)
}


# ---- teams -------------------------------------------------------------------------------------
def sample_team_size(rng):
    return 6 if rng.random() < P_TEAM_SIX else rng.randint(1, 5)


def gen_team(rng, tables, mode, idx, prefix):
    spec = MODES[mode]
    mons = [spec['gen_mon'](rng, tables) for _ in range(sample_team_size(rng))]
    return {
        'id': '%s_%d' % (prefix, idx),
        'source': spec['source'],
        'tags': list(spec['tags']),
        'format': 'singles',
        'level_cap': max(m['level'] for m in mons),
        'mons': mons,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--mode', choices=sorted(MODES), default='valid')
    ap.add_argument('--count', type=int, default=2500)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--out', default=None, help='output .jsonl (default: stdout)')
    ap.add_argument('--prefix', default=None, help='id prefix (default per mode)')
    args = ap.parse_args()

    rng = random.Random(args.seed)
    tables = Tables()
    prefix = args.prefix or MODES[args.mode]['prefix']
    out = open(args.out, 'w') if args.out else sys.stdout
    try:
        for i in range(1, args.count + 1):
            out.write(json.dumps(gen_team(rng, tables, args.mode, i, prefix), separators=(',', ':')) + '\n')
    finally:
        if args.out:
            out.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
