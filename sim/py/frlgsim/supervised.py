"""Supervised fit of the token-level network to amplified targets (frlgsim.amplify): the yardstick is held-out
correlation with V1 (ceiling: 0.999, the hand heuristic: 0.92, linear on 55 features: 0.926, MLP: 0.940).

    python -m frlgsim.supervised --data ../ai/data/amp1 --out ../ai/runs/sup1 --epochs 8 [--d 128 --layers 3 --heads 4]
"""
import argparse
import json
import os
import sys
import time
import numpy as np
import torch
from . import features as FT
from .model import ValueNet, count_params
from .train import bce_logits, make_batch


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--epochs", type=int, default=8)
    ap.add_argument("--batch", type=int, default=512)
    ap.add_argument("--lr", type=float, default=3e-4)
    ap.add_argument("--wd", type=float, default=0.01)
    ap.add_argument("--d", type=int, default=128)
    ap.add_argument("--layers", type=int, default=3)
    ap.add_argument("--heads", type=int, default=4)
    ap.add_argument("--heldout-frac", type=float, default=0.1)
    ap.add_argument("--device", default="mps")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--init", default=None)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    torch.manual_seed(args.seed)
    rng = np.random.default_rng(args.seed)
    F = np.load(args.data + "_F.npy", mmap_mode="r")
    I = np.load(args.data + "_I.npy", mmap_mode="r")
    V = np.load(args.data + "_v.npy")
    G = np.load(args.data + "_g.npy")
    P = ((V + 1) / 2).astype(np.float32)
    games = np.unique(G)
    rng.shuffle(games)
    hold = np.isin(G, games[:int(len(games) * args.heldout_frac)])
    tr, ho = np.where(~hold)[0], np.where(hold)[0]
    print(f"{len(V)} states, train {len(tr)} heldout {len(ho)} (by game)", file=sys.stderr)
    dev = args.device
    net = ValueNet(d=args.d, heads=args.heads, layers=args.layers).to(dev)
    if args.init:
        net.load_state_dict(torch.load(args.init, map_location=dev)["ema"])
    print(f"params {count_params(net)}", file=sys.stderr)
    opt = torch.optim.AdamW(net.parameters(), lr=args.lr, weight_decay=args.wd, betas=(0.9, 0.98))
    steps = args.epochs * (len(tr) // args.batch)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=args.lr, total_steps=max(steps, 1), pct_start=0.1)
    Fh, Ih, Ph = np.array(F[ho]), np.array(I[ho]), P[ho]

    @torch.no_grad()
    def evaluate():
        net.eval()
        zs = []
        for k in range(0, len(ho), 2048):
            zs.append(net(make_batch(Fh[k:k + 2048], Ih[k:k + 2048], dev)).float().cpu())
        z = torch.cat(zs)
        p = torch.sigmoid(z).numpy()
        loss = float(bce_logits(z, torch.from_numpy(Ph)))
        corr = float(np.corrcoef(p, Ph)[0, 1])
        net.train()
        return loss, corr

    best = (-1.0, None)
    log = open(os.path.join(args.out, "log.jsonl"), "a")
    t0 = time.time()
    step = 0
    for ep in range(args.epochs):
        perm = rng.permutation(tr)
        tot, n = 0.0, 0
        for k in range(0, len(perm) - args.batch + 1, args.batch):
            idx = np.sort(perm[k:k + args.batch])
            b = make_batch(np.array(F[idx]), np.array(I[idx]), dev)
            t = torch.from_numpy(P[idx]).to(dev)
            loss = bce_logits(net(b), t)
            opt.zero_grad(set_to_none=True)
            loss.backward()
            torch.nn.utils.clip_grad_norm_(net.parameters(), 1.0)
            opt.step(); sched.step()
            tot += float(loss.detach()); n += 1; step += 1
        hl, corr = evaluate()
        rec = {"epoch": ep, "step": step, "train": tot / max(n, 1), "heldout": hl, "corr": corr, "elapsed": time.time() - t0}
        print(json.dumps(rec), file=sys.stderr); log.write(json.dumps(rec) + "\n"); log.flush()
        if corr > best[0]:
            best = (corr, ep)
            torch.save({"net": net.state_dict(), "ema": net.state_dict(), "step": step, "args": vars(args), "corr": corr}, os.path.join(args.out, "best.pt"))
    print(f"best held-out corr {best[0]:.4f} at epoch {best[1]}", file=sys.stderr)


if __name__ == "__main__":
    main()
