#!/usr/bin/env python3
"""Generate sim/teams/random_encodable.jsonl: teams of uniformly random mons that fit in
`struct Pokemon` (verdict `encodable`) but usually break FireRed's legality rules.

  gen_encodable_teams.py [--count N] [--seed S] [--out FILE]

Every field is sampled independently and uniformly over the range the checker
(tools/teams.py check -> build/teamcheck) accepts as encodable:

  species    uniform over the species names in `teams.py names` minus NONE and EGG
             (ids 1..411 except 252 = OLD_UNOWN_B, which the name table does not expose)
  level      uniform 1..100
  moves      k uniform in 1..4, then k distinct moves uniform over ids 1..354 (no NONE)
  item       uniform over all 375 item names, ids 0..374 (NONE, balls, key items, TMs/HMs,
             the unnamed 0xx/1xx slots ... anything in the item table)
  ability    slot number, uniform over {0, 1}
  nature     uniform over the 25 natures
  ivs        6 x uniform 0..31
  evs        6 x uniform 0..255; if the total exceeds 510 each stat is rescaled as
             ev_i = floor(ev_i * 510 / total), which keeps every stat in 0..255 and the
             total <= 510 (totals <= 510 are left untouched, so ~all mons are rescaled)
  happiness  uniform 0..255
  ot         "player" with probability 0.9, else "outsider"
  fateful    true with probability 0.5
  team size  uniform 1..6
  format     "doubles" with probability 0.1 when the team has >= 2 mons, else "singles"

Ids are re_<n> (n from 1), source "random_encodable", tags ["random", "encodable"].
The RNG is a seeded random.Random, so the same --count/--seed reproduces the file.
"""
import argparse, json, os, random, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import teams  # noqa: E402  (sim/tools/teams.py: the checker's name tables)

ROOT = teams.ROOT  # sim/


def name_lists():
    t = teams.tables()
    by_id = lambda d: [k for k, v in sorted(d.items(), key=lambda kv: kv[1])]
    species = [k for k in by_id(t['species']) if k not in ('NONE', 'EGG')]
    moves = [k for k in by_id(t['moves']) if k != 'NONE']
    items = by_id(t['items'])                       # includes NONE
    natures = by_id(t['natures'])
    return species, moves, items, natures


def rescale_evs(evs):
    total = sum(evs)
    if total <= 510:
        return evs
    return [e * 510 // total for e in evs]


def gen_mon(rng, species, moves, items, natures):
    k = rng.randint(1, 4)
    return {
        'species': rng.choice(species),
        'level': rng.randint(1, 100),
        'moves': rng.sample(moves, k),
        'item': rng.choice(items),
        'ability': rng.randint(0, 1),
        'nature': rng.choice(natures),
        'ivs': [rng.randint(0, 31) for _ in range(6)],
        'evs': rescale_evs([rng.randint(0, 255) for _ in range(6)]),
        'happiness': rng.randint(0, 255),
        'nickname': None,
        'ot': 'player' if rng.random() < 0.9 else 'outsider',
        'fateful': rng.random() < 0.5,
    }


def gen_team(rng, n, tables):
    size = rng.randint(1, 6)
    mons = [gen_mon(rng, *tables) for _ in range(size)]
    fmt = 'doubles' if size >= 2 and rng.random() < 0.1 else 'singles'
    return {'id': 're_%d' % n, 'source': 'random_encodable', 'tags': ['random', 'encodable'],
            'format': fmt, 'mons': mons}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--count', type=int, default=2500)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--out', default=os.path.join(ROOT, 'teams', 'random_encodable.jsonl'))
    a = ap.parse_args()
    rng = random.Random(a.seed)
    tables = name_lists()
    with open(a.out, 'w') as f:
        for n in range(1, a.count + 1):
            f.write(json.dumps(gen_team(rng, n, tables), separators=(',', ':')) + '\n')
    print('%s: %d teams (seed %d)' % (a.out, a.count, a.seed))


if __name__ == '__main__':
    main()
