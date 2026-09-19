#!/usr/bin/env python3
"""Coverage of species / moves / abilities / items.

  coverage.py teams  [teams/*.jsonl]      what the team corpus contains (per mon)
  coverage.py corpus [corpus/*.trace(.gz)] what the recorded ROM transcripts exercised (battle mons at every
                                          snapshot: species, ability, held item, known moves, and moves USED =
                                          a PP drop of that move between consecutive snapshots of a battler)
Prints the missing ids and the least covered ones for each table.
"""
import collections, glob, gzip, json, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import teams as T

NUM_SPECIES = 412   # 1..411 encodable
# FireRed's internal ids: 1..251 (Kanto/Johto), 252..276 unused OLD_UNOWN_* placeholder slots, 277..411 (Hoenn)
REAL_SPECIES = [i for i in range(1, NUM_SPECIES) if not 252 <= i <= 276]
STRUGGLE = 165      # never sits in a move slot


def tables():
    t = T.tables()
    inv = {k: {v: n for n, v in d.items()} for k, d in t.items()}
    return t, inv


def holdable_items(t):
    exe = os.path.join(ROOT, 'build', 'teamcheck')
    out = subprocess.run([exe, '--items'], capture_output=True, text=True).stdout.split()
    return [t['items'][x] for x in out if x in t['items']]


def species_abilities():
    """species id -> set of ability ids, from the game's species info."""
    src = open(os.path.join(os.path.dirname(ROOT), 'src/data/pokemon/species_info.h')).read()
    abil = T.load_defines(os.path.join(os.path.dirname(ROOT), 'include/constants/abilities.h'), 'ABILITY_', ('_COUNT',))
    sp = T.load_defines(os.path.join(os.path.dirname(ROOT), 'include/constants/species.h'), 'SPECIES_', ('_COUNT',))
    out = {}
    for m in re.finditer(r'\[SPECIES_(\w+)\]\s*=\s*\{(.*?)\n\s*\},', src, re.S):
        name, body = m.group(1), m.group(2)
        a = re.search(r'\.abilities\s*=\s*\{\s*ABILITY_(\w+)\s*,\s*ABILITY_(\w+)\s*\}', body)
        if a and name in sp:
            out[sp[name]] = {abil.get(a.group(1), 0), abil.get(a.group(2), 0)} - {0}
    return out


def report(name, counter, universe, inv, lowest=15, extra=None):
    missing = sorted(u for u in universe if counter.get(u, 0) == 0)
    print('== %s: %d/%d covered, %d missing' % (name, len(universe) - len(missing), len(universe), len(missing)))
    if missing:
        print('   missing:', ' '.join(inv[u] for u in missing))
    low = sorted(((counter.get(u, 0), u) for u in universe if counter.get(u, 0) > 0))[:lowest]
    print('   least covered:', ', '.join('%s=%d' % (inv[u], c) for c, u in low))
    if extra:
        extra(counter)


def cmd_teams(files):
    t, inv = tables()
    rows = []
    errors = []
    for f in files:
        rows += [r.split('\t') for r in T.to_tsv(f, t, errors)]
    sp = collections.Counter(int(r[2]) for r in rows)
    mv = collections.Counter(int(m) for r in rows for m in r[4:8] if int(m))
    it = collections.Counter(int(r[8]) for r in rows if int(r[8]))
    # ability: slot -> resolve with the species table; named -> id
    spab = species_abilities()
    ab = collections.Counter()
    for r in rows:
        s, slot, aname = int(r[2]), int(r[9]), int(r[10])
        abils = sorted(spab.get(s, set()))
        if slot == 255:
            ab[aname] += 1
        elif abils:
            ab[abils[min(slot, len(abils) - 1)] if slot < len(abils) else abils[0]] += 1
    print('%d mons from %d files' % (len(rows), len(files)))
    report('species (386 real)', sp, REAL_SPECIES, inv['species'])
    report('moves', mv, [m for m in range(1, len(inv['moves'])) if m != STRUGGLE], inv['moves'])
    report('abilities', ab, range(1, len(inv['abilities'])), inv['abilities'])
    report('held items (holdable)', it, holdable_items(t), inv['items'])


def read_trace(path):
    op = gzip.open if path.endswith('.gz') else open
    with op(path, 'rt') as f:
        for line in f:
            yield line.rstrip('\n')


def cmd_corpus(files):
    t, inv = tables()
    sp, ab, it, known, used = (collections.Counter() for _ in range(5))
    battles = 0
    prev = None   # per battler: (species, moves, pp) at the previous snapshot
    for path in files:
        for line in read_trace(path):
            if line.startswith('START '):
                battles += 1
                prev = {}
            elif line.startswith('MONS '):
                raw = bytes.fromhex(line[5:])
                for b in range(4):
                    m = raw[b * 88:(b + 1) * 88]
                    species = m[0] | (m[1] << 8)
                    if species == 0 or species >= NUM_SPECIES:
                        continue
                    moves = [m[12 + 2 * i] | (m[13 + 2 * i] << 8) for i in range(4)]
                    pp = list(m[36:40])
                    ability = m[32]
                    item = m[44] | (m[45] << 8)
                    sp[species] += 1
                    ab[ability] += 1
                    if item:
                        it[item] += 1
                    for mv in moves:
                        if mv:
                            known[mv] += 1
                    p = prev.get(b)
                    if p and p[0] == species:
                        for i in range(4):
                            if moves[i] and p[1][i] == moves[i] and pp[i] < p[2][i]:
                                used[moves[i]] += 1
                    prev[b] = (species, moves, pp)
    print('%d battles, %d battler-snapshots from %d files' % (battles, sum(sp.values()), len(files)))
    report('species on the field (386 real)', sp, REAL_SPECIES, inv['species'])
    report('abilities on the field', ab, range(1, len(inv['abilities'])), inv['abilities'])
    report('held items on the field (holdable)', it, holdable_items(t), inv['items'])
    report('moves known by a battler', known, [m for m in range(1, len(inv['moves'])) if m != STRUGGLE], inv['moves'])
    report('moves USED (PP dropped between snapshots)', used, [m for m in range(1, len(inv['moves'])) if m != STRUGGLE], inv['moves'], lowest=25)


def main():
    if len(sys.argv) < 2:
        print(__doc__); return 2
    cmd = sys.argv[1]
    files = sys.argv[2:]
    if cmd == 'teams':
        files = files or sorted(glob.glob(os.path.join(ROOT, 'teams', '*.jsonl')))
        cmd_teams(files)
    elif cmd == 'corpus':
        files = files or sorted(glob.glob(os.path.join(ROOT, 'corpus', '*.trace*')))
        cmd_corpus(files)
    else:
        print(__doc__); return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
