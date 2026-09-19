#!/usr/bin/env python3
"""Import Pokemon Showdown export text into the team corpus format (sim/teams/FORMAT.md).

  import_showdown.py convert [opts] FILE...           Showdown export text -> JSONL on stdout / --out
  import_showdown.py convert --manifest M.jsonl       one manifest line per input file (see below)
  import_showdown.py forum-text THREAD.html --url U   XenForo (Smogon forums) HTML -> post-segmented text
                                                      (+ a listing of pokepast.es / pastebin links per post)

convert options:
  --source S         provenance tag (default "smogon")
  --tag T            tag to add to every team (repeatable)
  --id-prefix P      team ids are P_001, P_002 ... (default: the file's basename)
  --notes N          free text added to every team's notes
  --level L          level for mons that have no "Level:" line (default 100)
  --annotate         run build/teamcheck; teams that are only `encodable` get the tag "not_frlg_legal"
                     and the checker's reason appended to notes (teams that fail to encode are dropped)
  --out FILE         write JSONL there instead of stdout
  --split N          split a run of more than 6 consecutive mons into teams of N (default 6)
  --no-dedup         keep teams whose mons are identical to an earlier team
  --formats F        drop teams under a "=== [fmt] name ===" header unless fmt starts with F (e.g. gen3); repeatable.
                     Teams without a header are always kept

Manifest lines are JSON objects: {"file": path, "id": team id or prefix, "source": ..., "tags": [...],
"notes": "...", "level": 100, "format": "singles", "nicknames": true}. A file that yields several teams gets
ids id_1, id_2, ... ("nicknames": false drops nicknames, useful for set compendia whose "nicknames" are set names).

Conversion rules
  * Names are matched case-insensitively after stripping everything but letters and digits, so
    "ThunderPunch", "Thunder Punch", "Double-Edge", "Mr. Mime", "Farfetch'd", "Nidoran-F" and "King's Rock" all
    resolve. A few spelling aliases (Softboiled, Hi/High Jump Kick, Faint/Feint Attack, Smelling Salt(s),
    Vice/Vise Grip, Compoundeyes, Lightningrod ...) are handled explicitly.
  * Forms are folded onto the FireRed species (Deoxys-Attack -> DEOXYS, Castform-Sunny -> CASTFORM,
    Unown-X -> UNOWN) and recorded as a tag "form:deoxys-attack".
  * EVs/IVs are re-ordered from Showdown's HP/Atk/Def/SpA/SpD/Spe to the game's HP/Atk/Def/Spe/SpA/SpD.
  * "Hidden Power [Type]" becomes HIDDEN_POWER; if the export gives IVs that already yield that type under
    the gen-3 formula they are kept, otherwise IVs are chosen (30/31 only, so 70 power) to produce the type.
    Sets with a bare "Hidden Power" keep their IVs and get a note.
  * Happiness: "Happiness: N" if present, else 0 for Frustration users, else 255 (Showdown's default).
  * Mew and Deoxys get fateful=true (they only obey with the fateful-encounter flag).
  * "Trait:" (old export spelling of Ability:) is accepted; "Move A / Move B" slashed options keep the first move.
  * Teams containing a name that cannot be resolved (usually a non-gen-3 paste) are dropped with a warning on
    stderr.
"""
import argparse, html as htmlmod, json, os, re, subprocess, sys, collections

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import teams as teamsmod                                   # noqa: E402  (sim/tools/teams.py)

SIM = os.path.dirname(HERE)
HP_TYPES = ['FIGHTING', 'FLYING', 'POISON', 'GROUND', 'ROCK', 'BUG', 'GHOST', 'STEEL',
            'FIRE', 'WATER', 'GRASS', 'ELECTRIC', 'PSYCHIC', 'ICE', 'DRAGON', 'DARK']
SHOWDOWN_STATS = ['hp', 'atk', 'def', 'spa', 'spd', 'spe']      # export order
GAME_ORDER = ['hp', 'atk', 'def', 'spe', 'spa', 'spd']          # struct Pokemon order
FORMS = ('attack', 'defense', 'speed', 'normal', 'sunny', 'rainy', 'snowy')


def squash(s):
    return re.sub(r'[^a-z0-9]', '', s.lower())


ALIASES = {
    'moves': {'softboiled': 'SOFT_BOILED', 'highjumpkick': 'HI_JUMP_KICK', 'feintattack': 'FAINT_ATTACK',
              'smellingsalts': 'SMELLING_SALT', 'visegrip': 'VICE_GRIP', 'selfdestruct': 'SELF_DESTRUCT',
              'ancientpower': 'ANCIENT_POWER', 'extremespeed': 'EXTREME_SPEED', 'dragonbreath': 'DRAGON_BREATH',
              'solarbeam': 'SOLAR_BEAM', 'thunderpunch': 'THUNDER_PUNCH', 'dynamicpunch': 'DYNAMIC_PUNCH',
              'bubblebeam': 'BUBBLE_BEAM', 'thundershock': 'THUNDER_SHOCK', 'poisonpowder': 'POISON_POWDER',
              'sonicboom': 'SONIC_BOOM', 'featherdance': 'FEATHER_DANCE', 'grasswhistle': 'GRASS_WHISTLE',
              'doubleslap': 'DOUBLE_SLAP', 'faintattack': 'FAINT_ATTACK', 'hijumpkick': 'HI_JUMP_KICK'},
    'items': {'brightpowder': 'BRIGHT_POWDER', 'nevermeltice': 'NEVER_MELT_ICE', 'twistedspoon': 'TWISTED_SPOON',
              'silverpowder': 'SILVER_POWDER', 'blackglasses': 'BLACK_GLASSES', 'deepseatooth': 'DEEP_SEA_TOOTH',
              'deepseascale': 'DEEP_SEA_SCALE', 'upgrade': 'UP_GRADE', 'kingsrock': 'KINGS_ROCK',
              'nothing': 'NONE', 'noitem': 'NONE', '': 'NONE'},
    'abilities': {'compoundeyes': 'COMPOUND_EYES', 'lightningrod': 'LIGHTNING_ROD', 'airlock': 'AIR_LOCK',
                  'effectspore': 'EFFECT_SPORE', 'none': 'NONE'},
    'species': {'mrmime': 'MR_MIME', 'farfetchd': 'FARFETCHD', 'nidoranf': 'NIDORAN_F', 'nidoranm': 'NIDORAN_M',
                'hooh': 'HO_OH', 'porygon2': 'PORYGON2'},
    'natures': {},
}


class Tables:
    def __init__(self):
        self.t = teamsmod.tables()
        self.idx = {}
        for kind, d in self.t.items():
            m = {}
            for name, val in d.items():
                if val == 0 and kind != 'natures':
                    continue
                m.setdefault(squash(name), name)
            self.idx[kind] = m

    def lookup(self, kind, name):
        key = squash(name)
        if key in ALIASES[kind]:
            return ALIASES[kind][key]
        return self.idx[kind].get(key)


class Unresolved(Exception):
    pass


def hp_type_of(ivs):
    """ivs in game order. Returns (type name, power) under the gen-3 formula."""
    bits = [(ivs[i] & 1) for i in range(6)]
    tbits = [((ivs[i] >> 1) & 1) for i in range(6)]
    tsum = sum(b << i for i, b in enumerate(bits))
    psum = sum(b << i for i, b in enumerate(tbits))
    return HP_TYPES[tsum * 15 // 63], psum * 40 // 63 + 30


def ivs_for_hp_type(tname):
    """Pick 30/31 IVs (game order) giving Hidden Power <tname> at 70 power, maximizing the number of 31s."""
    want = HP_TYPES.index(tname.upper())
    best = None
    for mask in range(64):
        ivs = [31 if (mask >> i) & 1 else 30 for i in range(6)]
        t, p = hp_type_of(ivs)
        if t == HP_TYPES[want] and p == 70:
            score = (sum(1 for v in ivs if v == 31), ivs[0])      # prefer more 31s, then 31 HP
            if best is None or score > best[0]:
                best = (score, ivs)
    return best[1]


# ---------------------------------------------------------------- parsing Showdown export text

MON_HEAD = re.compile(r'^(?P<name>[^@]+?)(?:\s*@\s*(?P<item>.+?))?\s*$')
STAT_RX = re.compile(r'(\d+)\s*(HP|Atk|Def|SpA|SpD|Spe|Spd|Spc)\b', re.I)
TEAM_HEADER = re.compile(r'^===\s*(?:\[(?P<fmt>[^\]]*)\]\s*)?(?P<name>.*?)\s*===\s*$')
ATTR_RX = re.compile(r'^(Ability|Trait|EVs|IVs|Level|Shiny|Happiness|Nature|Gender|Tera Type):|^\w+ Nature$', re.I)


def parse_blocks(text):
    """Yield ('header', name, fmt), ('mon', dict, None) or ('prose', text, None) items in file order."""
    lines = [l.rstrip() for l in text.replace('\r', '').split('\n')]
    block = []

    def flush():
        if block:
            yield from parse_block(block)
            del block[:]

    for line in lines:
        s = line.strip()
        m = TEAM_HEADER.match(s)
        if m:
            yield from flush()
            yield ('header', m.group('name'), m.group('fmt'))
            continue
        if not s:
            yield from flush()
            continue
        block.append(s)
    yield from flush()


def parse_block(block):
    """A block is a group of non-empty lines. It is a mon if it has at least one '- move' line."""
    if not any(l.startswith('-') for l in block) or block[0].startswith('-'):
        yield ('prose', ' '.join(block), None)
        return
    # a team title pasted directly above the first mon line (no blank line) is prose: the mon head is the
    # last line before the first attribute / move line
    first_attr = next(i for i, l in enumerate(block) if l.startswith('-') or ATTR_RX.match(l))
    if first_attr == 0:
        yield ('prose', ' '.join(block), None)
        return
    if first_attr > 1:
        yield ('prose', ' '.join(block[:first_attr - 1]), None)
        block = block[first_attr - 1:]
    head = block[0]
    m = MON_HEAD.match(head)
    if not m or not m.group('name').strip():
        yield ('prose', ' '.join(block), None)
        return
    name = m.group('name').strip()
    item = (m.group('item') or '').strip()
    gender = None
    g = re.match(r'^(.*?)\s*\((M|F|N)\)$', name)
    if g:
        name, gender = g.group(1).strip(), g.group(2)
    nick = None
    n = re.match(r'^(.*?)\s*\(([^()]+)\)$', name)
    if n:
        nick, name = n.group(1).strip(), n.group(2).strip()
    mon = {'species_raw': name, 'nick': nick, 'gender': gender, 'item_raw': item, 'ability_raw': None,
           'nature_raw': None, 'evs': {}, 'ivs': {}, 'level': None, 'happiness': None, 'moves_raw': [], 'shiny': False}
    for l in block[1:]:
        if l.startswith('-'):
            mv = l[1:].strip()
            if ' / ' in mv:                                  # "Move A / Move B": slashed options, keep the first
                mon['slashed'] = True
                mv = mv.split(' / ')[0].strip()
            if mv:
                mon['moves_raw'].append(mv)
            continue
        low = l.lower()
        if low.startswith('ability:') or low.startswith('trait:'):        # "Trait:" is the pre-2013 export spelling
            mon['ability_raw'] = l.split(':', 1)[1].strip()
        elif low.startswith('evs:'):
            for v, st in STAT_RX.findall(l.split(':', 1)[1]):
                mon['evs'][norm_stat(st)] = int(v)
        elif low.startswith('ivs:'):
            for v, st in STAT_RX.findall(l.split(':', 1)[1]):
                mon['ivs'][norm_stat(st)] = int(v)
        elif low.endswith('nature') and len(l.split()) == 2:
            mon['nature_raw'] = l.split()[0]
        elif low.startswith('nature:'):
            mon['nature_raw'] = l.split(':', 1)[1].strip()
        elif low.startswith('level:'):
            try:
                mon['level'] = int(l.split(':', 1)[1].strip())
            except ValueError:
                pass
        elif low.startswith('happiness:'):
            try:
                mon['happiness'] = int(l.split(':', 1)[1].strip())
            except ValueError:
                pass
        elif low.startswith('shiny:'):
            mon['shiny'] = 'yes' in low
        # Gender:, Tera Type:, Dynamax Level:, Gigantamax: ... ignored
    yield ('mon', mon, None)


def norm_stat(s):
    s = s.lower()
    return {'spd': 'spd', 'spc': 'spa'}.get(s, s)


# ---------------------------------------------------------------- conversion

def convert_mon(raw, T, default_level, warnings):
    tags, notes = [], []
    sp_raw = raw['species_raw']
    form = None
    base = sp_raw
    if '-' in sp_raw:
        b, suf = sp_raw.rsplit('-', 1)
        if suf.lower() in FORMS or b.lower() in ('unown', 'castform', 'deoxys'):
            base, form = b, sp_raw.lower()
    species = T.lookup('species', base)
    if species is None:
        raise Unresolved('species %r' % sp_raw)
    if form:
        tags.append('form:' + form)
    moves, hp_type = [], None
    for mv in raw['moves_raw']:
        m = re.match(r'^hidden\s*power\s*[\[\(]?\s*([a-z]*)\s*[\]\)]?$', mv, re.I)
        if m:
            if 'HIDDEN_POWER' in moves:
                continue
            moves.append('HIDDEN_POWER')
            if m.group(1):
                if m.group(1).upper() not in HP_TYPES:
                    raise Unresolved('hidden power type %r' % mv)
                hp_type = m.group(1).upper()
            continue
        c = T.lookup('moves', mv)
        if c is None:
            raise Unresolved('move %r' % mv)
        if c not in moves:
            moves.append(c)
    if not moves:
        raise Unresolved('no moves for %s' % sp_raw)
    moves = moves[:4]
    item = T.lookup('items', raw['item_raw'])
    if item is None:
        raise Unresolved('item %r' % raw['item_raw'])
    ability = None
    if raw['ability_raw']:
        ability = T.lookup('abilities', raw['ability_raw'])
        if ability is None or ability == 'NONE':
            warnings.append('%s: ability %r unknown, using slot 0' % (sp_raw, raw['ability_raw']))
            ability = None
    nature = 'HARDY'
    if raw['nature_raw']:
        nature = T.lookup('natures', raw['nature_raw'])
        if nature is None:
            raise Unresolved('nature %r' % raw['nature_raw'])
    evs = [raw['evs'].get(s, 0) for s in GAME_ORDER]
    if sum(evs) > 510:
        # older exports occasionally exceed 510 (pre-2004 "252 all" habits); scale the largest ones down
        notes.append('EV total %d capped to 510' % sum(evs))
        while sum(evs) > 510:
            i = evs.index(max(evs)); evs[i] -= min(4, sum(evs) - 510) or 1
    ivs = [raw['ivs'].get(s, 31) for s in GAME_ORDER]
    ivs = [max(0, min(31, v)) for v in ivs]
    if hp_type:
        t, p = hp_type_of(ivs)
        if raw['ivs'] and t == hp_type:
            pass                                             # export's IVs already give the right type
        else:
            new = ivs_for_hp_type(hp_type)
            if raw['ivs']:
                notes.append('%s: export IVs gave HP %s %d, replaced with gen-3 IVs for HP %s 70' % (sp_raw, t, p, hp_type))
            ivs = new
        tags.append('hp:' + hp_type.lower())
    elif 'HIDDEN_POWER' in moves:
        notes.append('%s: Hidden Power type not stated, IVs left as given (HP %s %d)' % ((sp_raw,) + hp_type_of(ivs)))
    if raw.get('slashed'):
        notes.append('%s: slashed move options in the export, first option kept' % sp_raw)
    level = raw['level'] or default_level
    if raw['happiness'] is not None:
        hap = raw['happiness']
    elif 'FRUSTRATION' in moves:
        hap = 0
    else:
        hap = 255
    mon = {'species': species, 'level': level, 'moves': moves, 'item': item,
           'ability': ability if ability else 0, 'nature': nature, 'ivs': ivs, 'evs': evs, 'happiness': hap}
    if species in ('MEW', 'DEOXYS'):
        mon['fateful'] = True                                # event-only species obey only with the flag
    if raw['nick'] and re.match(r'^[A-Za-z0-9 .\-]{1,10}$', raw['nick']):
        mon['nickname'] = raw['nick']
    return mon, tags, notes


def team_key(mons):
    return json.dumps([[m['species'], sorted(m['moves']), m['item'], m['nature'], m['evs']] for m in mons], sort_keys=True)


def convert_text(text, T, default_level, split):
    """Returns [(team_name, [mon_raw...]), ...]: consecutive mon blocks form a team; a '=== name ===' header
    or any block of prose (a non-mon paragraph) ends the current team."""
    teams, cur, cur_name = [], [], None
    for kind, val, extra in parse_blocks(text):
        if kind == 'header' or kind == 'prose':
            if cur:
                teams.append((cur_name, cur))
            cur, cur_name = [], (val if kind == 'header' else None)
            if kind == 'header' and extra:
                cur_name = '[%s] %s' % (extra, val)
        else:
            cur.append(val)
            if len(cur) == split:
                teams.append((cur_name, cur)); cur = []
    if cur:
        teams.append((cur_name, cur))
    return teams


def convert_file(path, meta, T, args, out, seen, counters):
    text = open(path, encoding='utf-8', errors='replace').read()
    groups = convert_text(text, T, meta.get('level', args.level), args.split)
    idbase = meta.get('id') or re.sub(r'[^A-Za-z0-9_.:-]', '_', os.path.splitext(os.path.basename(path))[0])
    n_out = 0
    if args.formats:
        groups = [(n, r) for n, r in groups
                  if not (n and n.startswith('[')) or any(n[1:].lower().startswith(f) for f in args.formats)]
    for gi, (gname, raws) in enumerate(groups, 1):
        tid = idbase if len(groups) == 1 else '%s_%d' % (idbase, gi)
        mons, tags, notes, warnings = [], [], [], []
        try:
            for r in raws:
                m, mt, mn = convert_mon(r, T, meta.get('level', args.level), warnings)
                mons.append(m); tags += mt; notes += mn
        except Unresolved as e:
            print('skip %s (%s): unresolved %s' % (tid, os.path.basename(path), e), file=sys.stderr)
            counters['skipped_unresolved'] += 1
            continue
        if not mons:
            continue
        key = team_key(mons)
        if key in seen and not args.no_dedup:
            print('skip %s: duplicate of %s' % (tid, seen[key]), file=sys.stderr)
            counters['skipped_duplicate'] += 1
            continue
        seen[key] = tid
        note_parts = []
        if meta.get('notes'):
            note_parts.append(meta['notes'])
        if gname:
            note_parts.append('team name: ' + gname)
        note_parts += notes + warnings
        if meta.get('nicknames') is False:
            for m in mons:
                m.pop('nickname', None)
        team = {'id': tid, 'source': meta.get('source', args.source),
                'tags': list(dict.fromkeys(list(meta.get('tags', [])) + list(args.tag or []) + tags)),
                'format': meta.get('format', 'singles'), 'level_cap': max(m['level'] for m in mons),
                'notes': '; '.join(note_parts), 'mons': mons}
        if len(mons) < 6:
            team['tags'].append('partial')
        out.append(team)
        n_out += 1
    return n_out


def annotate(teams):
    """Run build/teamcheck; tag encodable teams not_frlg_legal, drop invalid ones."""
    T = teamsmod.tables()
    rows, errors, seen = [], [], set()
    byid = {t['id']: t for t in teams}
    for t in teams:
        try:
            _, r = teamsmod.resolve_team(t, T, seen)
            rows += ['\t'.join(str(x) for x in row) for row in r]
        except teamsmod.Bad as e:
            errors.append('%s: %s' % (t['id'], e))
    exe = os.path.join(SIM, 'build', 'teamcheck')
    res = subprocess.run([exe], input='\n'.join(rows) + '\n', capture_output=True, text=True)
    verdict = {}
    for line in res.stdout.splitlines():
        parts = line.split(' ', 2)
        if parts[0] == 'TEAM' and len(parts) >= 3:
            v, _, reason = parts[2].partition(':')
            verdict[parts[1]] = (v.strip(), reason.strip())
    kept = []
    for t in teams:
        v, reason = verdict.get(t['id'], ('invalid', 'not checked'))
        if v == 'ok':
            kept.append(t)
        elif v == 'encodable':
            t['tags'].append('not_frlg_legal')
            t['notes'] = (t['notes'] + '; ' if t.get('notes') else '') + 'not FRLG-legal: ' + reason
            kept.append(t)
        else:
            print('drop %s: %s %s' % (t['id'], v, reason), file=sys.stderr)
    for e in errors:
        print('drop ' + e, file=sys.stderr)
    return kept


def cmd_convert(args):
    T = Tables()
    out, seen = [], {}
    counters = collections.Counter()
    jobs = []
    if args.manifest:
        for line in open(args.manifest):
            line = line.strip()
            if line and not line.startswith('#'):
                m = json.loads(line)
                jobs.append((m['file'], m))
    for f in args.files:
        jobs.append((f, {'id': args.id_prefix} if args.id_prefix and len(args.files) == 1 else {}))
    if args.id_prefix and len(args.files) > 1:
        for i, (f, m) in enumerate(jobs[len(jobs) - len(args.files):], 1):
            m['id'] = '%s_%03d' % (args.id_prefix, i)
    for path, meta in jobs:
        if args.notes and not meta.get('notes'):
            meta['notes'] = args.notes
        counters['teams'] += convert_file(path, meta, T, args, out, seen, counters)
    if args.annotate:
        out = annotate(out)
    fh = open(args.out, 'w') if args.out else sys.stdout
    for t in out:
        fh.write(json.dumps(t, separators=(',', ':')) + '\n')
    if args.out:
        fh.close()
    print('%d teams written, %d skipped (unresolved), %d skipped (duplicate)' %
          (len(out), counters['skipped_unresolved'], counters['skipped_duplicate']), file=sys.stderr)
    return 0


# ---------------------------------------------------------------- XenForo thread HTML -> text

def html_to_text(fragment):
    s = re.sub(r'<script.*?</script>', '', fragment, flags=re.S)
    s = re.sub(r'<br\s*/?>[ \t]*\n?', '\n', s)
    s = re.sub(r'</(p|div|li|tr|h\d|blockquote)>', '\n', s)
    s = re.sub(r'<[^>]+>', '', s)
    s = htmlmod.unescape(s)
    s = s.replace('​', '').replace('\xa0', ' ')
    lines = [l.strip() for l in s.split('\n')]
    outl, blank = [], 0
    for l in lines:
        if not l:
            blank += 1
            if blank <= 1:
                outl.append('')
        else:
            blank = 0
            outl.append(l)
    return '\n'.join(outl).strip()


def cmd_forum_text(args):
    src = open(args.html, encoding='utf-8', errors='replace').read()
    posts = re.split(r'<article class="message message--post', src)[1:]
    out = []
    links = []
    for p in posts:
        pid = re.search(r'data-content="post-(\d+)"', p)
        author = re.search(r'data-author="([^"]*)"', p)
        pid = pid.group(1) if pid else '?'
        author = htmlmod.unescape(author.group(1)) if author else '?'
        i = p.find('class="bbWrapper"')
        body = p[p.find('>', i) + 1:] if i >= 0 else p
        j = body.find('<div class="js-selectToQuoteEnd">')
        if j > 0:
            body = body[:j]
        text = html_to_text(body)
        url = '%s#post-%s' % (args.url, pid) if args.url else 'post-' + pid
        out.append('### post %s author=%s url=%s\n%s\n' % (pid, author, url, text))
        for m in re.finditer(r'<a[^>]+href="(https?://(?:pokepast\.es/[0-9a-f]{16}|pastebin\.com/(?:raw/)?[A-Za-z0-9]{8}))[^"]*"[^>]*>(.*?)</a>', body, re.S):
            label = html_to_text(m.group(2)).replace('\n', ' ')[:80]
            # a little context before the link, to pick up team names written as plain text
            pre = body[max(0, m.start() - 400):m.start()]
            pre = html_to_text(pre[pre.find('>') + 1:]).replace('\n', ' | ')[-160:]
            post = body[m.end():m.end() + 1500]
            post = html_to_text(post[:post.rfind('<')]).replace('\n', ' | ')[:160]
            links.append({'thread': args.url, 'post': pid, 'author': author, 'url': m.group(1), 'label': label,
                          'context': pre, 'after': post})
    open(args.out, 'w').write('\n'.join(out))
    if args.links:
        with open(args.links, 'w') as f:
            for l in links:
                f.write(json.dumps(l) + '\n')
    print('%s: %d posts, %d paste links' % (args.html, len(posts), len(links)), file=sys.stderr)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd')
    c = sub.add_parser('convert')
    c.add_argument('files', nargs='*')
    c.add_argument('--manifest')
    c.add_argument('--source', default='smogon')
    c.add_argument('--tag', action='append')
    c.add_argument('--id-prefix')
    c.add_argument('--notes')
    c.add_argument('--level', type=int, default=100)
    c.add_argument('--annotate', action='store_true')
    c.add_argument('--out')
    c.add_argument('--split', type=int, default=6)
    c.add_argument('--no-dedup', action='store_true')
    c.add_argument('--formats', action='append', help='keep only teams whose "=== [fmt] name ===" header starts with this (e.g. gen3); repeatable')
    f = sub.add_parser('forum-text')
    f.add_argument('html')
    f.add_argument('--url', default='')
    f.add_argument('--out', required=True)
    f.add_argument('--links')
    args = ap.parse_args()
    if args.cmd == 'convert':
        return cmd_convert(args)
    if args.cmd == 'forum-text':
        return cmd_forum_text(args)
    ap.print_help(); return 2


if __name__ == '__main__':
    sys.exit(main())
