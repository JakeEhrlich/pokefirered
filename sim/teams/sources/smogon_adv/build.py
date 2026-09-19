#!/usr/bin/env python3
"""Rebuild sim/teams/smogon_adv.jsonl from the raw sources in this directory.

    python3 sim/teams/sources/smogon_adv/build.py

Inputs (all fetched with curl, see smogon_adv.md for the URLs):
  threads/<name>.txt            post-segmented text of a Smogon thread (tools/import_showdown.py forum-text)
  threads/<name>.links.jsonl    the pokepast.es / pastebin links found in each post
  pokepaste/<id>.txt            https://pokepast.es/<id>/raw
  pastebin/<id>.txt             https://pastebin.com/raw/<id>
  stats/<month>_<format>.txt    https://www.smogon.com/stats/<month>/moveset/<format>.txt

Outputs: manifest.jsonl, sets/manifest.jsonl, inline/*.txt, stats/*.synthetic.txt, convert.log (intermediate,
regenerated every run) and ../../smogon_adv.jsonl.
"""
import collections, json, os, random, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SIM = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(SIM, 'tools'))
import import_showdown as I                              # noqa: E402

OUT = os.path.join(SIM, 'teams', 'smogon_adv.jsonl')
T = I.Tables()

# thread key -> (tier tags, kind tag, id prefix, source, where the team name sits relative to a paste link, post filter)
THREADS = [
    ('ou_sample_teams',            ['adv', 'ou'],    'sample-team', 'smogon_ou_sample',  'smogon', 'after',  None),
    ('uu_sample_teams',            ['adv', 'uu'],    'sample-team', 'smogon_uu_sample',  'smogon', 'after',  None),
    ('uu_resources',               ['adv', 'uu'],    'sample-team', 'smogon_uu_res',     'smogon', 'before', None),
    ('nu_resources',               ['adv', 'nu'],    'sample-team', 'smogon_nu_sample',  'smogon', 'before', None),
    ('nu_discussion',              ['adv', 'nu'],    'team-dump',   'smogon_nu_disc',    'smogon', 'before', None),
    ('ubers_old_gens_hub',         ['adv', 'ubers'], 'sample-team', 'smogon_ubers_hub',  'smogon', 'before', {'9462710'}),
    ('ou_teams_through_the_ages',  ['adv', 'ou'],    'historical',  'smogon_ou_ages',    'smogon', 'before', None),
] + [('ou_set_and_team_sharing_p%d' % p, ['adv', 'ou'], 'team-dump', 'smogon_ou_sharing_p%d' % p, 'smogon', 'before', None)
     for p in range(1, 11)]

# inline importables (thread text, post ids to take, tags, id prefix)
INLINE = [
    ('ubers_sample_teams',         None,          ['adv', 'ubers', 'sample-team'], 'smogon_ubers_sample'),
    ('ubers_sample_teams_multigen', {'6963709'},  ['adv', 'ubers', 'sample-team'], 'smogon_ubers_archive'),
    ('ubers_old_gens_hub',         {'9462710'},   ['adv', 'ubers', 'sample-team'], 'smogon_ubers_hub_inline'),
] + [('ou_set_and_team_sharing_p%d' % p, None, ['adv', 'ou', 'team-dump'], 'smogon_ou_sharing_inline_p%d' % p)
     for p in range(1, 11)]

# pastebin team builders / compendia linked from the OU sample thread (id -> (kind, tags, prefix, notes))
PASTEBINS = {
    'jyFh4AF3': ('teams', ['adv', 'ou', 'team-dump'], 'smogon_ou_builder_tlc2019',
                 'thelinearcurve 2019 teambuilder dump https://pastebin.com/raw/jyFh4AF3 (linked from ADV OU Sample Teams post https://www.smogon.com/forums/threads/adv-ou-sample-teams.3687813/#post-8918758)'),
    'tUytkX29': ('teams', ['adv', 'ou', 'team-dump'], 'smogon_ou_builder_tlc2020',
                 'thelinearcurve 2020 teambuilder dump (all) https://pastebin.com/raw/tUytkX29 (linked from https://www.smogon.com/forums/threads/adv-ou-sample-teams.3687813/#post-8918758)'),
    'QbwUxw0P': ('teams', ['adv', 'ou', 'team-dump'], 'smogon_ou_builder_tlc2020v',
                 'thelinearcurve 2020 teambuilder dump (more viable / user-friendly) https://pastebin.com/raw/QbwUxw0P (linked from https://www.smogon.com/forums/threads/adv-ou-sample-teams.3687813/#post-8918758)'),
    '6xCb1crp': ('teams', ['adv', 'ou', 'team-dump'], 'smogon_ou_builder_tlc2021',
                 'thelinearcurve 2021 teambuilder dump https://pastebin.com/raw/6xCb1crp (linked from https://www.smogon.com/forums/threads/adv-ou-sample-teams.3687813/#post-8918758)'),
    'i7HzJQmC': ('teams', ['adv', 'ou', 'team-dump'], 'smogon_ou_builder_ibidem2017',
                 'Ibidem 2017 teambuilder dump https://pastebin.com/i7HzJQmC (linked from https://www.smogon.com/forums/threads/adv-ou-sample-teams.3687813/#post-8918758)'),
    'iAKXBuaD': ('teams', ['adv', 'ou', 'team-dump'], 'smogon_ou_builder_l3w',
                 'L3W teambuilder dump https://pastebin.com/iAKXBuaD (linked from ADV OU Set and Team Sharing https://www.smogon.com/forums/threads/set-dump-team-dump-and-previously-general-metagame-discussion.3648620/page-5#post-9357776)'),
    'haMgma5w': ('sets', ['adv', 'ou'], 'tlc2019sets',
                 'thelinearcurve 2019 set compendium https://pastebin.com/raw/haMgma5w'),
    'ubq1NEVR': ('sets', ['adv', 'ou'], 'tlc2020sets',
                 'thelinearcurve 2020 set compendium https://pastebin.com/raw/ubq1NEVR'),
    'qjNWD0bJ': ('sets', ['adv', 'ou'], 'jhonx2021sets',
                 "Jhonx's 2021 set compilation https://pastebin.com/qjNWD0bJ"),
}
# pokepaste set compendia (not teams)
PASTE_SETS = {
    '5fc1d96ac246ec5a': (['adv', 'ubers'], 'ubers_sets',
                         'ADV Ubers set compendium (largely by SEA) https://pokepast.es/5fc1d96ac246ec5a, from Ubers Old Gens Hub https://www.smogon.com/forums/threads/ubers-old-gens-hub.3714123/#post-9462710'),
}

# usage stats files: (file, tier tags, id prefix, level, format, teammate source for set packing)
STATS = [
    ('2024-12_gen3ou-1760.txt',        ['adv', 'ou'],    'usage_gen3ou_2024',     100, 'singles'),
    ('2018-12_gen3ou-1760.txt',        ['adv', 'ou'],    'usage_gen3ou_2018',     100, 'singles'),
    ('2025-06_gen3uu-1760.txt',        ['adv', 'uu'],    'usage_gen3uu_2025',     100, 'singles'),
    ('2021-06_gen3nu-1760.txt',        ['adv', 'nu'],    'usage_gen3nu_2021',     100, 'singles'),
    ('2016-12_gen3ubers-1760.txt',     ['adv', 'ubers'], 'usage_gen3ubers_2016',  100, 'singles'),
    ('2024-03_gen3lc-1760.txt',        ['adv', 'lc'],    'usage_gen3lc_2024',       5, 'singles'),
    ('2025-03_gen3ru-1760.txt',        ['adv', 'ru'],    'usage_gen3ru_2025',     100, 'singles'),
    ('2025-03_gen3uubl-1760.txt',      ['adv', 'uubl'],  'usage_gen3uubl_2025',   100, 'singles'),
    ('2024-12_gen3zu-1760.txt',        ['adv', 'zu'],    'usage_gen3zu_2024',     100, 'singles'),
    ('2022-12_gen3doublesou-1500.txt', ['adv', 'doubles-ou'], 'usage_gen3dou_2022', 100, 'doubles'),
]
TEAMMATE_STATS = {'ou': '2024-12_gen3ou-1760.txt', 'ubers': '2016-12_gen3ubers-1760.txt'}

ARCHETYPES = [('stall', r'\bstall'), ('offense', r'\boffen|\bHO\b|hyper'), ('balance', r'\bbalance'),
              ('spikes', r'spike|\bTSS\b'), ('rain', r'\brain'), ('sun', r'\bsun\b'), ('baton-pass', r'\bpass\b|baton'),
              ('cm-spam', r'\bcm spam|calm mind spam'), ('sand', r'\bsand\b'), ('lead', r'\blead')]


def archetype_tags(text):
    return [t for t, rx in ARCHETYPES if re.search(rx, text, re.I)]


# ---------------------------------------------------------------- manifest for paste links

def paste_file(url):
    m = re.search(r'pokepast\.es/([0-9a-f]{16})', url)
    if m:
        return os.path.join(HERE, 'pokepaste', m.group(1) + '.txt'), 'pokepaste', m.group(1)
    m = re.search(r'pastebin\.com/(?:raw/)?([A-Za-z0-9]{8})', url)
    if m:
        return os.path.join(HERE, 'pastebin', m.group(1) + '.txt'), 'pastebin', m.group(1)
    return None, None, None


def paste_shape(path):
    """'team' (<= 6 mons, has === headers, or prose-separated groups that are mostly 6 mons), 'split6' (one
    headerless run of 6k mons: assume a team dump) or 'sets' (anything else with > 6 mons: a set compendium)."""
    text = open(path, encoding='utf-8', errors='replace').read()
    items = list(I.parse_blocks(text))
    n = sum(1 for x in items if x[0] == 'mon')
    if n <= 6 or any(x[0] == 'header' for x in items):
        return 'team'
    sizes = [len(g[1]) for g in I.convert_text(text, T, 100, 10 ** 9)]
    if len(sizes) == 1:
        return 'split6' if n % 6 == 0 else 'sets'
    if sum(1 for z in sizes if z == 6) >= 0.5 * len(sizes):
        return 'team'
    return 'sets'


def build_manifest():
    manifest, seen_urls = [], set()
    counters = collections.Counter()
    set_files = []                                           # (file, tags, notes, nicknames)
    for key, tier, kind, prefix, source, name_side, posts in THREADS:
        path = os.path.join(HERE, 'threads', key + '.links.jsonl')
        if not os.path.exists(path):
            continue
        for line in open(path):
            d = json.loads(line)
            if posts and d['post'] not in posts:
                continue
            f, ptype, pid = paste_file(d['url'])
            if not f or not os.path.exists(f) or os.path.getsize(f) == 0:
                continue
            if pid in seen_urls:
                continue
            seen_urls.add(pid)
            near = (d['after'] if name_side == 'after' else d['context']).strip(' |')
            near = re.sub(r'\s*\|\s*', ' | ', near)
            if ptype == 'pastebin':
                if pid not in PASTEBINS:
                    continue                                 # DPP guides / mechanics pastes
                pkind, ptags, pprefix, pnotes = PASTEBINS[pid]
                if pkind == 'sets':
                    set_files.append((f, ptags, pnotes, False))
                    continue
                manifest.append({'file': f, 'id': pprefix, 'source': 'smogon', 'tags': ptags,
                                 'notes': pnotes})
                continue
            notes = '%s (post by %s, %s#post-%s); near: %s' % (d['url'], d['author'], d['thread'], d['post'], near[:140])
            if pid in PASTE_SETS:
                ptags, _, pnotes = PASTE_SETS[pid]
                set_files.append((f, ptags, pnotes, False))
                continue
            shape = paste_shape(f)
            extra = []
            if shape == 'sets':
                set_files.append((f, tier, notes, False))
                continue
            if shape == 'split6':
                extra.append('split-by-6')
            counters[prefix] += 1
            manifest.append({'file': f, 'id': '%s_%03d' % (prefix, counters[prefix]), 'source': source,
                             'tags': tier + [kind] + extra + archetype_tags(near + ' ' + d['label']), 'notes': notes})
    return manifest, set_files


# ---------------------------------------------------------------- inline importables

def build_inline():
    entries = []
    os.makedirs(os.path.join(HERE, 'inline'), exist_ok=True)
    for key, posts, tags, prefix in INLINE:
        path = os.path.join(HERE, 'threads', key + '.txt')
        if not os.path.exists(path):
            continue
        txt = open(path).read()
        for post in txt.split('### post ')[1:]:
            head, _, body = post.partition('\n')
            pid = head.split()[0]
            author = re.search(r'author=(.*?) url=', head).group(1)
            url = head.split('url=')[1].strip()
            if posts and pid not in posts:
                continue
            if not I.convert_text(body, T, 100, 6):
                continue
            f = os.path.join(HERE, 'inline', '%s_%s.txt' % (key, pid))
            open(f, 'w').write(body)
            entries.append({'file': f, 'id': '%s_%s' % (prefix, pid), 'source': 'smogon', 'tags': tags,
                            'notes': 'inline importable, post by %s, %s' % (author, url)})
    return entries


# ---------------------------------------------------------------- usage stats

def parse_moveset_file(path):
    """Returns ordered dict name -> {'abilities': [(name, pct)], 'items':..., 'spreads':..., 'moves':..., 'teammates':...}
    The file is a sequence of boxes separated by +----+ rules; each mon has 8 boxes: name, counts, Abilities,
    Items, Spreads, Moves, Teammates, Checks and Counters."""
    mons = collections.OrderedDict()
    boxes, box = [], []
    for line in open(path, encoding='utf-8', errors='replace'):
        line = line.strip()
        if line.startswith('+--'):
            if box:
                boxes.append(box); box = []
        elif line.startswith('|'):
            b = line.strip('|').strip()
            if b:
                box.append(b)
    cur = None
    for box in boxes:
        if len(box) == 1 and '%' not in box[0] and not box[0].startswith(('Raw count', 'Avg. weight', 'Viability')) \
                and box[0] not in ('Abilities', 'Items', 'Spreads', 'Moves', 'Teammates', 'Checks and Counters'):
            cur = box[0]
            mons[cur] = collections.defaultdict(list)
            continue
        if cur is None:
            continue
        section = box[0].lower()
        if section in ('abilities', 'items', 'spreads', 'moves', 'teammates'):
            for body in box[1:]:
                m = re.match(r'^(.*?)\s+([\d.]+)%', body)
                if m:
                    mons[cur][section].append((m.group(1).strip(), float(m.group(2))))
    return mons


def top_set(name, d):
    """Build a Showdown export block for the most common set of a mon, or None."""
    moves = [m for m, _ in d['moves'] if m != 'Other'][:4]
    if not moves:
        return None
    spread = next((s for s, _ in d['spreads'] if s != 'Other'), None)
    nature, evs = 'Hardy', [0] * 6
    if spread:
        nature, ev = spread.split(':')
        evs = [int(x) for x in ev.split('/')]
    item = next((s for s, _ in d['items'] if s != 'Other'), 'Nothing')
    ability = next((s for s, _ in d['abilities'] if s != 'Other'), None)
    lines = ['%s @ %s' % (name, item)]
    if ability:
        lines.append('Ability: ' + ability)
    lines.append('EVs: ' + ' / '.join('%d %s' % (v, s) for v, s in zip(evs, ['HP', 'Atk', 'Def', 'SpA', 'SpD', 'Spe']) if v))
    lines.append('%s Nature' % nature)
    lines += ['- ' + m for m in moves]
    return '\n'.join(lines)


def teammate_table(path):
    tm = {}
    for name, d in parse_moveset_file(path).items():
        tm[I.squash(name)] = {I.squash(n): p for n, p in d['teammates']}
    return tm


def build_usage_synthetic():
    entries = []
    for fname, tier, prefix, level, fmt in STATS:
        path = os.path.join(HERE, 'stats', fname)
        if not os.path.exists(path):
            continue
        mons = parse_moveset_file(path)
        sets = {name: top_set(name, d) for name, d in mons.items()}
        names = [n for n in mons if sets[n]]
        seen, teams = set(), []
        for seed in names[:30]:
            team = [seed]
            cands = sorted(((p, n) for n, p in mons[seed]['teammates'] if n in sets and n != seed), reverse=True)
            for _, n in cands:
                if len(team) == 6:
                    break
                if n not in team:
                    team.append(n)
            for n in names:                                  # pad from the usage list if teammates ran short
                if len(team) == 6:
                    break
                if n not in team:
                    team.append(n)
            key = tuple(sorted(team))
            if key in seen or len(team) < 6:
                continue
            seen.add(key)
            teams.append((seed, team))
        month, fmtname = fname.split('_', 1)
        fmtname = fmtname.split('-')[0]
        url = 'https://www.smogon.com/stats/%s/moveset/%s' % (month, fname.split('_', 1)[1])
        out = os.path.join(HERE, 'stats', fname.replace('.txt', '.synthetic.txt'))
        with open(out, 'w') as f:
            for seed, team in teams:
                f.write('=== [%s] %s + top teammates ===\n\n' % (fmtname, seed))
                for n in team:
                    f.write(sets[n] + '\n\n')
        entries.append({'file': out, 'id': prefix, 'source': 'usage_stats', 'level': level, 'format': fmt,
                        'tags': tier + ['synthetic_from_sets', 'usage-stats'],
                        'notes': 'synthetic: each mon is its most common set from Showdown usage stats %s; team = a top-30 mon '
                                 'plus its most common teammates' % url})
    return entries


# ---------------------------------------------------------------- packing single sets into synthetic teams

def run_convert(manifest_path, out_path, extra=()):
    cmd = [sys.executable, os.path.join(SIM, 'tools', 'import_showdown.py'), 'convert', '--manifest', manifest_path,
           '--out', out_path, '--formats', 'gen3'] + list(extra)
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.stderr


def pack_sets(pool, tm, prefix, tags, note, rng):
    """pool: list of (mon, provenance). Greedy: seed with the next unused set, add the unused set whose species
    co-occurs most with the members so far (usage-stats teammate %), species distinct."""
    used = [False] * len(pool)
    teams = []
    order = list(range(len(pool)))
    rng.shuffle(order)
    for i in order:
        if used[i]:
            continue
        team, prov = [i], [pool[i][1]]
        used[i] = True
        while len(team) < 6:
            best, best_s = None, -1.0
            species = {pool[j][0]['species'] for j in team}
            for j in order:
                if used[j] or pool[j][0]['species'] in species:
                    continue
                s = sum(tm.get(I.squash(pool[k][0]['species']), {}).get(I.squash(pool[j][0]['species']), 0.0) for k in team)
                if s > best_s:
                    best, best_s = j, s
            if best is None:
                break
            used[best] = True
            team.append(best); prov.append(pool[best][1])
        if len(team) < 6:
            break
        mons = [dict(pool[j][0]) for j in team]
        teams.append({'id': '%s_%03d' % (prefix, len(teams) + 1), 'source': 'smogon', 'tags': list(tags),
                      'format': 'singles', 'level_cap': 100,
                      'notes': note + '; sets from: ' + '; '.join(dict.fromkeys(prov)), 'mons': mons})
    return teams


def main():
    rng = random.Random(3)
    manifest, set_files = build_manifest()
    manifest += build_inline()
    manifest += build_usage_synthetic()
    mpath = os.path.join(HERE, 'manifest.jsonl')
    with open(mpath, 'w') as f:
        for m in manifest:
            f.write(json.dumps(m) + '\n')
    tmp = tempfile.mkdtemp(prefix='smogon_adv_')
    raw = os.path.join(tmp, 'converted_raw.jsonl')
    log = run_convert(mpath, raw)
    open(os.path.join(HERE, 'convert.log'), 'w').write(log)
    teams = [json.loads(l) for l in open(raw)]

    # pastes that yield many teams are whole teambuilder exports, not curated teams: tag them
    manifest_ids = {m['id'] for m in manifest if '/inline/' not in m['file']}     # inline posts are curated

    def base_id(tid):
        return tid if tid in manifest_ids else re.sub(r'_\d+$', '', tid)
    per_file = collections.Counter(base_id(t['id']) for t in teams)
    for t in teams:
        if base_id(t['id']) in manifest_ids and per_file[base_id(t['id'])] > 6 and t['source'] != 'usage_stats' \
                and 'builder-dump' not in t['tags']:
            t['tags'].append('builder-dump')
    # full teams vs. sets: anything with fewer than 6 mons feeds the set pool of its tier
    full, pools = [], collections.defaultdict(list)
    for t in teams:
        if len(t['mons']) == 6:
            full.append(t)
        else:
            tier = next((x for x in t['tags'] if x in ('ou', 'uu', 'nu', 'ubers')), 'ou')
            for m in t['mons']:
                m.pop('nickname', None)
                pools[tier].append((m, t['notes'].split(';')[0]))
    # set compendia
    os.makedirs(os.path.join(HERE, 'sets'), exist_ok=True)
    smpath = os.path.join(HERE, 'sets', 'manifest.jsonl')
    with open(smpath, 'w') as f:
        for i, (path, tags, notes, nick) in enumerate(set_files):
            f.write(json.dumps({'file': path, 'id': 'sets_%d' % i, 'tags': tags, 'notes': notes, 'nicknames': nick}) + '\n')
    sraw = os.path.join(tmp, 'sets_raw.jsonl')
    log += run_convert(smpath, sraw, ['--split', '1', '--no-dedup'])
    for l in open(sraw):
        t = json.loads(l)
        tier = next((x for x in t['tags'] if x in ('ou', 'uu', 'nu', 'ubers')), 'ou')
        for m in t['mons']:
            pools[tier].append((m, t['notes'].split(';')[0]))
    synthetic = []
    for tier, pool in pools.items():
        # dedup identical sets
        uniq, keys = [], set()
        for m, prov in pool:
            k = json.dumps([m['species'], sorted(m['moves']), m['item'], m['nature'], m['evs']])
            if k not in keys:
                keys.add(k); uniq.append((m, prov))
        tm = teammate_table(os.path.join(HERE, 'stats', TEAMMATE_STATS.get(tier, TEAMMATE_STATS['ou'])))
        packed = pack_sets(uniq, tm, 'smogon_%s_sets' % tier, ['adv', tier, 'synthetic_from_sets'],
                           'synthetic: 6 individually posted %s sets packed by usage-stats teammate affinity' % tier.upper(), rng)
        synthetic += packed
        print('%s: %d sets (%d unique) -> %d synthetic teams' % (tier, len(pool), len(uniq), len(packed)), file=sys.stderr)

    # order: real teams (sample -> dumps -> inline), then synthetic from sets, then usage-stats synthetic
    real = [t for t in full if t['source'] != 'usage_stats']
    usage = [t for t in full if t['source'] == 'usage_stats']
    allteams = I.annotate(real + synthetic + usage)
    with open(OUT, 'w') as f:
        for t in allteams:
            f.write(json.dumps(t, separators=(',', ':')) + '\n')
    print('wrote %d teams to %s (%d real, %d set-packed, %d usage-stats)' %
          (len(allteams), OUT, len(real), len(synthetic), len(usage)), file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
