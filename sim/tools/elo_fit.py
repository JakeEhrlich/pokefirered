#!/usr/bin/env python3
"""Order-independent ratings from an arena game log (games.jsonl from build/arena --out).

  elo_fit.py games.jsonl [--players agents|teams] [--h2h] [--json out.json] [--top N] [--min-games N]

Fits Bradley-Terry strengths by maximum likelihood (draws count half for each side), scaled to ELO
(400 * log10), centred at 1000, with standard errors from the observed Fisher information. Sequential ELO
(what the arena prints) depends on game order and on K; this does not.
"""
import json, math, sys, collections


def load(path, players):
    games = []
    for line in open(path):
        line = line.strip()
        if not line:
            continue
        g = json.loads(line)
        if players == 'teams':
            a, b = g['teamA'], g['teamB']
        else:
            a, b = g['a'], g['b']
        s = 1.0 if g['result'] == 'A' else 0.0 if g['result'] == 'B' else 0.5
        games.append((a, b, s))
    return games


def fit(games, iters=200):
    names = sorted({a for a, _, _ in games} | {b for _, b, _ in games})
    idx = {n: i for i, n in enumerate(names)}
    n = len(names)
    wins = [0.0] * n
    played = collections.defaultdict(float)   # (i, j) -> games between i and j (i < j)
    for a, b, s in games:
        i, j = idx[a], idx[b]
        wins[i] += s
        wins[j] += 1.0 - s
        key = (min(i, j), max(i, j))
        played[key] += 1.0
    strength = [1.0] * n
    # MM algorithm (Hunter 2004) for Bradley-Terry
    for _ in range(iters):
        denom = [0.0] * n
        for (i, j), c in played.items():
            d = c / (strength[i] + strength[j])
            denom[i] += d
            denom[j] += d
        new = [wins[i] / denom[i] if denom[i] > 0 else strength[i] for i in range(n)]
        geo = math.exp(sum(math.log(max(x, 1e-12)) for x in new) / n)
        strength = [x / geo for x in new]
    elo = [1000.0 + 400.0 * math.log10(max(s, 1e-12)) for s in strength]
    # standard errors: diagonal of the inverse Fisher information (approximate, per player, in ELO points)
    se = []
    for i in range(n):
        info = 0.0
        for (a, b), c in played.items():
            if i in (a, b):
                p = strength[a] / (strength[a] + strength[b])
                info += c * p * (1.0 - p)
        se.append((400.0 / math.log(10)) / math.sqrt(info) if info > 0 else float('inf'))
    games_of = collections.Counter()
    for a, b, _ in games:
        games_of[a] += 1
        games_of[b] += 1
    return [(names[i], elo[i], se[i], wins[i], games_of[names[i]]) for i in range(n)]


def h2h(games):
    table = collections.defaultdict(lambda: [0.0, 0])
    for a, b, s in games:
        table[(a, b)][0] += s; table[(a, b)][1] += 1
        table[(b, a)][0] += 1.0 - s; table[(b, a)][1] += 1
    names = sorted({a for a, _, _ in games} | {b for _, b, _ in games})
    w = max(len(x) for x in names)
    print(' ' * (w + 2) + ' '.join('%5d' % i for i in range(len(names))))
    for i, a in enumerate(names):
        row = []
        for b in names:
            s, c = table[(a, b)]
            row.append('  -  ' if a == b else ('%4.0f%%' % (100 * s / c) if c else '   . '))
        print('%2d %-*s %s' % (i, w, a, ' '.join(row)))


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__); return 2
    path = args[0]
    players = 'teams' if '--players' in args and args[args.index('--players') + 1] == 'teams' else 'agents'
    top = int(args[args.index('--top') + 1]) if '--top' in args else 40
    min_games = int(args[args.index('--min-games') + 1]) if '--min-games' in args else 1
    games = load(path, players)
    rows = [r for r in fit(games) if r[4] >= min_games]
    rows.sort(key=lambda r: -r[1])
    print('%d games, %d players (Bradley-Terry MLE, ELO scale, centred at 1000)' % (len(games), len(rows)))
    for name, elo, se, w, g in rows[:top]:
        print('%7.1f +- %5.1f  %5d games  %6.1f wins  %s' % (elo, se, g, w, name))
    if '--json' in args:
        out = args[args.index('--json') + 1]
        json.dump([{'name': r[0], 'rating': round(r[1], 1), 'se': round(r[2], 1), 'wins': r[3], 'games': r[4]} for r in rows], open(out, 'w'), indent=1)
    if '--h2h' in args:
        h2h(games)
    return 0


if __name__ == '__main__':
    sys.exit(main())
