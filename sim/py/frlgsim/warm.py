"""Warm-start data: replays arena-recorded games (arena --record) and encodes a sample of their decision states.

Output (ai/data/warm_*.npy): F [n, FLOATS] float16 (side-0 view), I [n, INTS] int16, y [n] float32 (side-0 result), g [n] game index.

    python -m frlgsim.warm ai/data/warm_games.tsv ai/data/warm --max-states 600000 --keep 0.3
"""
import argparse
import os
import sys
import time
import numpy as np
from concurrent.futures import ThreadPoolExecutor
from . import sim as S
from .teams import Teams
from .sim import Enc


def replay_game(line, teams, keep, rng_seed):
    seed, sideA, r, turns, ida, idb, acts = line.rstrip("\n").split("\t")
    if ida not in teams.rows or idb not in teams.rows:
        return None
    pa, pb = S.party_from_tsv(teams.rows[ida]), S.party_from_tsv(teams.rows[idb])
    if int(sideA) == 1:
        pa, pb = pb, pa
    s = S.Sim()
    if s.setup(pa, pb, int(seed), 300) != 0:
        return None
    acts = [list(map(int, a.split(","))) for a in acts.split(" ")] if acts else []
    rng = np.random.default_rng(rng_seed)
    E = Enc.load()
    Fs, Is = [], []
    k = 0
    while True:
        res = s.run()
        if res != S.RUN_REQUEST:
            break
        b, kind = s.request_battler, s.request_kind
        if k >= len(acts) or (b, kind) != (acts[k][0], acts[k][1]):
            return None
        a = acts[k]
        k += 1
        if rng.random() < keep and (kind == S.REQ_SWITCH or b == 0):
            f, i = s.encode(0)
            Fs.append(f.astype(np.float16))
            Is.append(i.astype(np.int16))
        if s.answer(b, S.action(a[2], a[3], a[4], a[5], a[6])) != 0:
            return None
    if not s.finished:
        return None
    o = s.outcome
    y = 1.0 if o == S.OUTCOME_WON else 0.0 if o == S.OUTCOME_LOST else 0.5
    if not Fs:
        return None
    return np.stack(Fs), np.stack(Is), np.full(len(Fs), y, np.float32)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("games")
    ap.add_argument("out_prefix")
    ap.add_argument("--max-states", type=int, default=600000)
    ap.add_argument("--keep", type=float, default=0.3)
    ap.add_argument("--threads", type=int, default=8)
    args = ap.parse_args()
    teams = Teams()
    lines = [l for l in open(args.games) if l.strip()]
    print(f"{len(lines)} games", file=sys.stderr)
    E = Enc.load()
    F = np.zeros((args.max_states, E.FLOATS), np.float16)
    I = np.zeros((args.max_states, E.INTS), np.int16)
    Y = np.zeros(args.max_states, np.float32)
    G = np.zeros(args.max_states, np.int32)
    n = 0
    t0 = time.time()
    games_used = 0
    with ThreadPoolExecutor(args.threads) as pool:
        for gi, res in enumerate(pool.map(lambda a: replay_game(a[1], teams, args.keep, a[0]), enumerate(lines), chunksize=64)):
            if res is None:
                continue
            f, i, y = res
            m = min(len(f), args.max_states - n)
            F[n:n + m] = f[:m]; I[n:n + m] = i[:m]; Y[n:n + m] = y[:m]; G[n:n + m] = games_used
            n += m
            games_used += 1
            if gi % 5000 == 0:
                print(f"game {gi} states {n} {time.time() - t0:.0f}s", file=sys.stderr)
            if n >= args.max_states:
                break
    print(f"{games_used} games, {n} states, {time.time() - t0:.0f}s", file=sys.stderr)
    np.save(args.out_prefix + "_F.npy", F[:n])
    np.save(args.out_prefix + "_I.npy", I[:n])
    np.save(args.out_prefix + "_y.npy", Y[:n])
    np.save(args.out_prefix + "_g.npy", G[:n])


if __name__ == "__main__":
    main()
