"""Amplified targets (fitted value iteration): play games with a search agent, and at every turn start record the
state (side-0 view) with the agent's equilibrium search value V1 as the label.

    python -m frlgsim.amplify --teams ../ai/data/randbats_30k.tsv --agent "rmplus:iters=30,samples=16" --games 2000 --out ../ai/data/amp1
Writes <out>_F.npy (float16), <out>_I.npy (int16), <out>_v.npy (V1 in [-1,1]), <out>_y.npy (side-0 result), <out>_g.npy (game).
"""
import argparse
import sys
import time
import numpy as np
from concurrent.futures import ThreadPoolExecutor
from . import sim as S
from .teams import Teams
from .sim import Enc


def play_one(args):
    gi, seed, pa, pb, spec, max_turns, repeat = args
    rng = np.random.default_rng(seed)
    ags = [S.CAgent(spec, int(rng.integers(1 << 30))), S.CAgent(spec, int(rng.integers(1 << 30)))]
    ag2 = S.CAgent(spec, int(rng.integers(1 << 30)))   # an independent second draw of V1 (noise ceiling)
    s = S.Sim()
    if s.setup(pa, pb, seed, max_turns) != 0:
        return None
    ts = S.Sim()
    ts_turn = -1
    Fs, Is, Vs, V2s = [], [], [], []
    guard = 0
    while True:
        r = s.run()
        if r != S.RUN_REQUEST:
            break
        b, kind = s.request_battler, s.request_kind
        if kind == S.REQ_ACTION and b == 0:
            ts.copy_from(s)
            ts_turn = s.turn
            v = ags[0].search_value(ts)
            if v is not None:
                f, i = s.encode(0)
                Fs.append(f.astype(np.float16)); Is.append(i.astype(np.int16)); Vs.append(v)
                V2s.append(ag2.search_value(ts) if repeat else v)
        a = ags[b & 1].decide(s, ts if (kind == S.REQ_ACTION and ts_turn == s.turn) else None, b, kind)
        if s.answer(b, a) != 0:
            legal = s.legal(b, kind)
            if len(legal) == 0 or s.answer(b, legal[0]) != 0:
                return None
        guard += 1
        if guard > 5000:
            return None
    if r != S.RUN_FINISHED or not Fs:
        return None
    o = s.outcome
    y = 1.0 if o == S.OUTCOME_WON else 0.0 if o == S.OUTCOME_LOST else 0.5
    return np.stack(Fs), np.stack(Is), np.array(Vs, np.float32), np.full(len(Fs), y, np.float32), np.array(V2s, np.float32)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--teams", required=True)
    ap.add_argument("--agent", default="rmplus:iters=30,samples=16")
    ap.add_argument("--games", type=int, default=2000)
    ap.add_argument("--threads", type=int, default=10)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out", required=True)
    ap.add_argument("--repeat", action="store_true", help="also compute an independent second V1 per state (<out>_v2.npy)")
    args = ap.parse_args()
    teams = Teams(tsv_path=args.teams)
    rng = np.random.default_rng(args.seed)
    jobs = []
    for gi in range(args.games):
        a, b = teams.sample_pair(rng)
        jobs.append((gi, int(rng.integers(1 << 30)), teams.party(a), teams.party(b), args.agent, 300, args.repeat))
    t0 = time.time()
    Fs, Is, Vs, Ys, Gs, V2s = [], [], [], [], [], []
    with ThreadPoolExecutor(args.threads) as pool:
        for gi, res in enumerate(pool.map(play_one, jobs)):
            if res is None:
                continue
            f, i, v, y, v2 = res
            Fs.append(f); Is.append(i); Vs.append(v); Ys.append(y); Gs.append(np.full(len(f), gi, np.int32)); V2s.append(v2)
            if gi % 200 == 0:
                print(f"game {gi} states {sum(len(x) for x in Vs)} {time.time() - t0:.0f}s", file=sys.stderr)
    F, I, V, Y, G = np.concatenate(Fs), np.concatenate(Is), np.concatenate(Vs), np.concatenate(Ys), np.concatenate(Gs)
    print(f"{len(Fs)} games, {len(V)} states, {time.time() - t0:.0f}s; V mean {V.mean():.3f} std {V.std():.3f}, side-0 win {Y.mean():.3f}", file=sys.stderr)
    np.save(args.out + "_F.npy", F); np.save(args.out + "_I.npy", I); np.save(args.out + "_v.npy", V); np.save(args.out + "_y.npy", Y); np.save(args.out + "_g.npy", G)
    if args.repeat:
        V2 = np.concatenate(V2s)
        np.save(args.out + "_v2.npy", V2)
        print(f"noise ceiling: corr(V1, V1') = {np.corrcoef(V, V2)[0, 1]:.3f}", file=sys.stderr)


if __name__ == "__main__":
    main()
