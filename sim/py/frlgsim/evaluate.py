"""Evaluate a checkpoint against C agents and place it on the pool's ELO scale.

    python -m frlgsim.evaluate ../ai/runs/r1/warm.pt --games 64 --samples 4 \
        --opponents "rmplus:iters=100,samples=4;rm:iters=10;greedy"

Each opponent's pool rating comes from ratings/agents_combined_bt.json (Bradley-Terry over the 18-agent pool on
randbats); the implied rating from a win rate p against an opponent rated R is R + 400 log10(p / (1 - p)).
"""
import argparse
import json
import os
import sys
import time
import numpy as np
import torch
from . import sim as S
from . import selfplay as SP
from .model import ValueNet
from .teams import Teams
from .train import eval_vs_c

POOL_RATINGS = os.path.join(S.SIM_DIR, "ratings", "agents_combined_bt.json")


def load_net(path, device):
    ck = torch.load(path, map_location=device)
    a = ck.get("args", {})
    net = ValueNet(d=a.get("d", 128), heads=a.get("heads", 4), layers=a.get("layers", 3)).to(device)
    net.load_state_dict(ck["ema"])
    net.eval()
    for p in net.parameters():
        p.requires_grad_(False)
    return net, ck


def pool_rating(spec):
    if not os.path.exists(POOL_RATINGS):
        return None
    data = json.load(open(POOL_RATINGS))
    rows = data if isinstance(data, list) else data.get("agents", data)
    for r in rows:
        if r.get("name") == spec or r.get("spec") == spec:
            return float(r["rating"])
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ckpt", help="a .pt checkpoint, a linear .npz, or 'hand' (the C heuristic weights through the Python search)")
    ap.add_argument("--games", type=int, default=64)
    ap.add_argument("--samples", type=int, default=4)
    ap.add_argument("--opponents", default="rmplus:iters=100,samples=4;rm:iters=10;greedy")
    ap.add_argument("--min-rating", type=float, default=800.0)
    ap.add_argument("--threads", type=int, default=8)
    ap.add_argument("--device", default="mps")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--out", default=None, help="append a JSON line here")
    args = ap.parse_args()
    if args.ckpt == "hand":
        from .linear import LinearEvaluator, hand_weights
        ev, ck = LinearEvaluator(hand_weights(), heuristic_clip=True, name="hand"), {}
    elif args.ckpt.endswith(".npz"):
        from .linear import LinearEvaluator
        z = np.load(args.ckpt)
        ev, ck = LinearEvaluator(z["w"], scale=1.0, name=os.path.basename(args.ckpt), std=z["std"] if "std" in z.files else None), {}
    else:
        net, ck = load_net(args.ckpt, args.device)
        ev = SP.NetEvaluator(net, args.device, compile=True, name=os.path.basename(args.ckpt))
    teams = Teams(min_rating=args.min_rating)
    rng = np.random.default_rng(args.seed)
    results = {}
    for spec in [s for s in args.opponents.split(";") if s]:
        t0 = time.time()
        score, w, d, l = eval_vs_c(ev, spec, teams, rng, games=args.games, samples=args.samples, threads=args.threads)
        n = w + d + l
        se = float(np.sqrt(max(score * (1 - score), 0.01) / max(n, 1)))
        R = pool_rating(spec)
        p = min(max(score, 0.02), 0.98)
        implied = (R + 400 * np.log10(p / (1 - p))) if R is not None else None
        results[spec] = {"score": score, "wdl": [w, d, l], "se": se, "opp_rating": R, "implied_rating": implied, "secs": time.time() - t0}
        print(f"{spec:36s} score {score:.3f} ±{se:.3f}  W-D-L {w}-{d}-{l}  opp {R}  implied {implied and round(implied)}  {time.time() - t0:.0f}s", file=sys.stderr)
    imp = [r["implied_rating"] for r in results.values() if r["implied_rating"] is not None]
    summary = {"ckpt": args.ckpt, "step": ck.get("step"), "games": args.games, "samples": args.samples, "results": results,
               "implied_mean": float(np.mean(imp)) if imp else None}
    print(json.dumps(summary))
    if args.out:
        with open(args.out, "a") as f:
            f.write(json.dumps(summary) + "\n")


if __name__ == "__main__":
    main()
