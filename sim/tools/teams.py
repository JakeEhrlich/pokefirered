#!/usr/bin/env python3
"""Team corpus tooling (format: sim/teams/FORMAT.md).

  teams.py check <file.jsonl> [--quiet]   resolve names, run build/teamcheck, print a verdict per team
  teams.py tsv   <file.jsonl>             print the flat numeric form (what build/teamcheck reads)
  teams.py stats <file.jsonl>             histograms
  teams.py names                          dump the name tables (species/moves/items/abilities/natures)
"""
import json, os, re, subprocess, sys, collections

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))          # sim/
REPO = os.path.dirname(ROOT)
STATS = ['hp', 'atk', 'def', 'spe', 'spa', 'spd']


def load_defines(path, prefix, exclude=()):
    out = {}
    rx = re.compile(r'^#define\s+(%s\w+)\s+(\d+)\s*$' % prefix)
    for line in open(path):
        m = rx.match(line)
        if m and not any(m.group(1).endswith(x) for x in exclude):  # MOVES_COUNT, not MOVE_COUNTER
            out[m.group(1)[len(prefix):]] = int(m.group(2))
    return out


def tables():
    t = {}
    t['species'] = load_defines(os.path.join(REPO, 'include/constants/species.h'), 'SPECIES_', ('_COUNT',))
    # ids 1..411 are encodable (SPECIES_EGG = 412); the UNOWN_B.. letter forms are aliases above that range
    t['species'] = {k: v for k, v in t['species'].items() if 0 < v < 412}
    t['moves'] = load_defines(os.path.join(REPO, 'include/constants/moves.h'), 'MOVE_', ('_COUNT',))
    t['items'] = load_defines(os.path.join(REPO, 'include/constants/items.h'), 'ITEM_', ('_COUNT',))
    t['abilities'] = load_defines(os.path.join(REPO, 'include/constants/abilities.h'), 'ABILITY_', ('_COUNT',))
    t['natures'] = load_defines(os.path.join(REPO, 'include/constants/pokemon.h'), 'NATURE_')
    return t


class Bad(Exception):
    pass


def resolve_mon(m, t, slot):
    if not isinstance(m, dict):
        raise Bad('mon %d is not an object' % slot)
    sp = str(m.get('species', '')).upper()
    if sp not in t['species'] or t['species'][sp] == 0:
        raise Bad('mon %d: unknown species %r' % (slot, m.get('species')))
    level = m.get('level')
    if not isinstance(level, int) or not 1 <= level <= 100:
        raise Bad('mon %d: bad level %r' % (slot, level))
    moves = [str(x).upper() for x in m.get('moves', []) if str(x).upper() != 'NONE']
    if not 1 <= len(moves) <= 4:
        raise Bad('mon %d: needs 1..4 moves' % slot)
    if len(set(moves)) != len(moves):
        raise Bad('mon %d: duplicate move' % slot)
    mids = []
    for mv in moves:
        if mv not in t['moves'] or t['moves'][mv] == 0:
            raise Bad('mon %d: unknown move %r' % (slot, mv))
        mids.append(t['moves'][mv])
    item = str(m.get('item', 'NONE')).upper()
    if item not in t['items']:
        raise Bad('mon %d: unknown item %r' % (slot, m.get('item')))
    ab = m.get('ability', 0)
    if isinstance(ab, int):
        if ab not in (0, 1):
            raise Bad('mon %d: ability slot must be 0 or 1' % slot)
        abnum, abname = ab, -1
    else:
        ab = str(ab).upper()
        if ab not in t['abilities'] or t['abilities'][ab] == 0:
            raise Bad('mon %d: unknown ability %r' % (slot, ab))
        abnum, abname = 255, t['abilities'][ab]      # teamcheck resolves the slot from the species
    nature = str(m.get('nature', 'HARDY')).upper()
    if nature not in t['natures']:
        raise Bad('mon %d: unknown nature %r' % (slot, nature))
    ivs = m.get('ivs', [31] * 6)
    evs = m.get('evs', [0] * 6)
    for name, arr, hi in (('ivs', ivs, 31), ('evs', evs, 255)):
        if not (isinstance(arr, list) and len(arr) == 6 and all(isinstance(x, int) and 0 <= x <= hi for x in arr)):
            raise Bad('mon %d: %s must be 6 ints in 0..%d' % (slot, name, hi))
    if sum(evs) > 510:
        raise Bad('mon %d: EV total %d > 510' % (slot, sum(evs)))
    hap = m.get('happiness', 70)
    if not isinstance(hap, int) or not 0 <= hap <= 255:
        raise Bad('mon %d: bad happiness' % slot)
    ot = m.get('ot', 'player')
    if ot not in ('player', 'outsider'):
        raise Bad('mon %d: ot must be player|outsider' % slot)
    nick = m.get('nickname')
    if nick is not None and not re.match(r'^[A-Za-z0-9 .\-]{1,10}$', str(nick)):
        raise Bad('mon %d: bad nickname' % slot)
    return [t['species'][sp], level] + (mids + [0] * 4)[:4] + [t['items'][item], abnum, abname, t['natures'][nature]] \
        + list(ivs) + list(evs) + [hap, 0 if ot == 'player' else 1, 1 if m.get('fateful') else 0]


def resolve_team(obj, t, seen):
    if not isinstance(obj, dict):
        raise Bad('team is not an object')
    tid = str(obj.get('id', ''))
    if not re.match(r'^[A-Za-z0-9_.:-]+$', tid):
        raise Bad('bad or missing id %r' % tid)
    if tid in seen:
        raise Bad('duplicate id %s' % tid)
    seen.add(tid)
    if not obj.get('source'):
        raise Bad('%s: missing source' % tid)
    fmt = obj.get('format', 'singles')
    if fmt not in ('singles', 'doubles'):
        raise Bad('%s: format must be singles|doubles' % tid)
    mons = obj.get('mons')
    if not isinstance(mons, list) or not 1 <= len(mons) <= 6:
        raise Bad('%s: needs 1..6 mons' % tid)
    if fmt == 'doubles' and len(mons) < 2:
        raise Bad('%s: doubles needs 2 mons' % tid)
    rows = []
    for i, m in enumerate(mons):
        rows.append([tid, i] + resolve_mon(m, t, i) + [1 if fmt == 'doubles' else 0])
    return tid, rows


def read_jsonl(path):
    with open(path) as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            try:
                yield n, json.loads(line)
            except json.JSONDecodeError as e:
                yield n, Bad('line %d: bad JSON: %s' % (n, e))


def to_tsv(path, t, errors):
    seen = set()
    out = []
    for n, obj in read_jsonl(path):
        if isinstance(obj, Bad):
            errors.append(str(obj)); continue
        try:
            tid, rows = resolve_team(obj, t, seen)
        except Bad as e:
            errors.append('line %d: %s' % (n, e)); continue
        for r in rows:
            out.append('\t'.join(str(x) for x in r))
    return out


def cmd_check(path, quiet):
    t = tables()
    errors = []
    rows = to_tsv(path, t, errors)
    exe = os.path.join(ROOT, 'build', 'teamcheck')
    if not os.path.exists(exe):
        print('build/teamcheck missing: run make in sim/'); return 2
    res = subprocess.run([exe], input='\n'.join(rows) + '\n', capture_output=True, text=True)
    counts = collections.Counter()
    for line in res.stdout.splitlines():
        parts = line.split(' ', 2)
        if parts[0] == 'TEAM' and len(parts) >= 3:
            verdict = parts[2].split(':')[0]
            counts[verdict] += 1
            if not quiet and verdict != 'ok':
                print(line)
        elif not quiet:
            print(line)
    for e in errors:
        counts['invalid'] += 1
        print('TEAM ? invalid: ' + e)
    total = sum(counts.values())
    print('%s: %d teams: %d ok, %d encodable, %d invalid' % (os.path.basename(path), total, counts['ok'], counts['encodable'], counts['invalid']))
    return 1 if counts['invalid'] else 0


def cmd_stats(path):
    t = tables()
    inv = {k: {v: n for n, v in d.items()} for k, d in t.items()}
    errors = []
    rows = [r.split('\t') for r in to_tsv(path, t, errors)]
    teams = collections.OrderedDict()
    for r in rows:
        teams.setdefault(r[0], []).append(r)
    sp = collections.Counter(inv['species'][int(r[2])] for r in rows)
    it = collections.Counter(inv['items'][int(r[8])] for r in rows)
    mv = collections.Counter(inv['moves'][int(m)] for r in rows for m in r[4:8] if int(m))
    lv = collections.Counter(int(r[3]) for r in rows)
    print('%d teams, %d mons, %d bad lines' % (len(teams), len(rows), len(errors)))
    print('team sizes:', dict(collections.Counter(len(v) for v in teams.values())))
    print('levels: min %d max %d, top %s' % (min(lv), max(lv), lv.most_common(5)))
    print('top species:', sp.most_common(15))
    print('top items:', it.most_common(10))
    print('top moves:', mv.most_common(15))
    print('distinct species %d / moves %d / items %d' % (len(sp), len(mv), len(it)))


def main():
    if len(sys.argv) < 2:
        print(__doc__); return 2
    cmd = sys.argv[1]
    if cmd == 'names':
        t = tables()
        for k, d in t.items():
            print(k, ' '.join(sorted(d, key=d.get)))
        return 0
    path = sys.argv[2]
    if cmd == 'check':
        return cmd_check(path, '--quiet' in sys.argv)
    if cmd == 'tsv':
        errors = []
        print('\n'.join(to_tsv(path, tables(), errors)))
        for e in errors:
            print('# ' + e, file=sys.stderr)
        return 1 if errors else 0
    if cmd == 'stats':
        cmd_stats(path); return 0
    print(__doc__); return 2


if __name__ == '__main__':
    sys.exit(main())
