"""Self-play training loop (ai/DESIGN.md section 3).

    python -m frlgsim.train --out ../ai/runs/r1 --hours 7 --warm ../ai/data/warm --warm-epochs 2

Phase 1 (warm start): fit the network to outcome-labelled states from heuristic games.
Phase 2 (self-play): a pool of lockstep games between the EWMA network and itself / league checkpoints; every
decision point enters a replay buffer with its outcome y and residual sum S; training minimises
BCE(p, y - lambda S) with lambda = Cov(y, S) / Var(S) re-estimated on the buffer; the EWMA weights act,
bootstrap and get saved; every 1/saves of the time budget a checkpoint is saved, evaluated against a C
heuristic (RM+ 100 x 4 samples) and on held-out warm states, and added to the league.
"""
import argparse
import json
import os
import sys
import time
import copy
import numpy as np
import torch
import torch.nn.functional as Fn
from . import sim as S
from . import features as FT
from . import selfplay as SP
from .model import ValueNet, count_params
from .teams import Teams
from .sim import Enc


def bce_logits(z, t):
    """-t log p - (1-t) log(1-p) for any real target t (gradient p - t)."""
    return (Fn.softplus(z) - t * z).mean()


class Buffer:
    def __init__(self, cap):
        E = Enc.load()
        self.cap = cap
        self.F = np.zeros((cap, E.FLOATS), np.float16)
        self.I = np.zeros((cap, E.INTS), np.int16)
        self.Y = np.zeros(cap, np.float32)
        self.S = np.zeros(cap, np.float32)
        self.n = 0
        self.pos = 0
        self.added = 0

    def add(self, F, I, Y, Ssum):
        m = len(F)
        for k in range(0, m, self.cap):
            f, i, y, s = F[k:k + self.cap], I[k:k + self.cap], Y[k:k + self.cap], Ssum[k:k + self.cap]
            mm = len(f)
            end = self.pos + mm
            if end <= self.cap:
                self.F[self.pos:end] = f; self.I[self.pos:end] = i; self.Y[self.pos:end] = y; self.S[self.pos:end] = s
            else:
                a = self.cap - self.pos
                self.F[self.pos:] = f[:a]; self.I[self.pos:] = i[:a]; self.Y[self.pos:] = y[:a]; self.S[self.pos:] = s[:a]
                self.F[:mm - a] = f[a:]; self.I[:mm - a] = i[a:]; self.Y[:mm - a] = y[a:]; self.S[:mm - a] = s[a:]
            self.pos = end % self.cap
            self.n = min(self.cap, self.n + mm)
            self.added += mm

    def sample(self, rng, batch):
        idx = rng.integers(self.n, size=batch)
        return self.F[idx], self.I[idx], self.Y[idx], self.S[idx]

    def lam(self):
        n = self.n
        if n < 1000:
            return 0.0
        y, s = self.Y[:n].astype(np.float64), np.clip(self.S[:n].astype(np.float64), -3.0, 3.0)
        v = s.var()
        if v < 1e-12:
            return 0.0
        return float(np.clip(((y - y.mean()) * (s - s.mean())).mean() / v, 0.0, 1.0))


def make_batch(F, I, device):
    rec = FT.split_records(F.astype(np.float32), I.astype(np.int32))
    rec.pop("terminal")
    return FT.to_torch(rec, device)


def train_step(net, opt, ema, ema_decay, F, I, T, device, clip=1.0):
    net.train()
    b = make_batch(F, I, device)
    t = torch.from_numpy(T.astype(np.float32)).to(device)
    z = net(b)
    loss = bce_logits(z, t)
    stats = {"z_abs": float(z.detach().abs().mean()), "t_abs": float(t.abs().mean()), "t_max": float(t.abs().max())}
    opt.zero_grad(set_to_none=True)
    loss.backward()
    torch.nn.utils.clip_grad_norm_(net.parameters(), clip)
    opt.step()
    with torch.no_grad():
        for pe, p in zip(ema.parameters(), net.parameters()):
            pe.mul_(ema_decay).add_(p.detach(), alpha=1 - ema_decay)
    net.eval()
    return float(loss.detach()), stats


@torch.no_grad()
def heldout_loss(net, F, I, Y, device, batch=2048):
    net.eval()
    tot, n = 0.0, 0
    for k in range(0, len(F), batch):
        b = make_batch(F[k:k + batch], I[k:k + batch], device)
        z = net(b)
        t = torch.from_numpy(Y[k:k + batch]).to(device)
        tot += float(bce_logits(z, t)) * len(z)
        n += len(z)
    return tot / max(n, 1)


def eval_vs_c(ev, spec, teams, rng, games=32, samples=4, threads=8, max_turns=300, log=None):
    """Win rate of the network (samples per cell) against a C agent, sides alternating. Returns (score, wins, draws, losses)."""
    gs = []
    cagents = [S.CAgent(spec, int(rng.integers(1 << 30))) for _ in range(games)]
    for i in range(games):
        a, b = teams.sample_pair(rng)
        seed = int(rng.integers(1 << 30))
        if i % 2 == 0:
            players = [SP.Player(ev, samples=samples), SP.Player(cagent=cagents[i])]
        else:
            players = [SP.Player(cagent=cagents[i]), SP.Player(ev, samples=samples)]
        gs.append(SP.Game(i, teams.party(a), teams.party(b), seed, players, learner=None, max_turns=max_turns))
    runner = SP.Runner(threads)
    SP.play_games(gs, runner)
    w = d = l = 0
    for i, g in enumerate(gs):
        if g.result is None:
            continue
        r = g.result if i % 2 == 0 else 1.0 - g.result
        if r == 1.0: w += 1
        elif r == 0.0: l += 1
        else: d += 1
    n = max(w + d + l, 1)
    return (w + 0.5 * d) / n, w, d, l


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--hours", type=float, default=7.0)
    ap.add_argument("--warm", default=None, help="prefix of warm_{F,I,y}.npy")
    ap.add_argument("--warm-epochs", type=float, default=2.0)
    ap.add_argument("--warm-buffer", type=int, default=150000, help="warm states seeded into the replay buffer")
    ap.add_argument("--games", type=int, default=96, help="games in flight")
    ap.add_argument("--buffer", type=int, default=400000)
    ap.add_argument("--batch", type=int, default=1024)
    ap.add_argument("--lr", type=float, default=3e-4)
    ap.add_argument("--wd", type=float, default=0.01)
    ap.add_argument("--ema", type=float, default=0.999)
    ap.add_argument("--samples", type=int, default=2)
    ap.add_argument("--eps", type=float, default=0.03)
    ap.add_argument("--league-frac", type=float, default=0.3)
    ap.add_argument("--league-size", type=int, default=4)
    ap.add_argument("--train-per-step", type=int, default=1)
    ap.add_argument("--clip-s", type=float, default=3.0, help="clip of the residual sum S in the target")
    ap.add_argument("--saves", type=int, default=30)
    ap.add_argument("--eval-games", type=int, default=32)
    ap.add_argument("--eval-spec", default="rmplus:iters=100,samples=4")
    ap.add_argument("--min-rating", type=float, default=None)
    ap.add_argument("--band", type=float, default=150.0)
    ap.add_argument("--threads", type=int, default=8)
    ap.add_argument("--device", default="mps")
    ap.add_argument("--d", type=int, default=128)
    ap.add_argument("--layers", type=int, default=3)
    ap.add_argument("--heads", type=int, default=4)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--init", default=None, help="checkpoint to start from (skips the warm phase)")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    logf = open(os.path.join(args.out, "log.jsonl"), "a")

    def log(**kw):
        kw["t"] = time.time()
        kw["wall"] = time.strftime("%H:%M:%S")
        logf.write(json.dumps(kw) + "\n"); logf.flush()
        print(json.dumps(kw), file=sys.stderr)

    torch.manual_seed(args.seed)
    rng = np.random.default_rng(args.seed)
    dev = args.device
    net = ValueNet(d=args.d, heads=args.heads, layers=args.layers).to(dev)
    ema = copy.deepcopy(net).to(dev)
    for p in ema.parameters():
        p.requires_grad_(False)
    opt = torch.optim.AdamW(net.parameters(), lr=args.lr, weight_decay=args.wd, betas=(0.9, 0.98))
    log(event="start", params=count_params(net), args=vars(args))
    teams = Teams(min_rating=args.min_rating, band=args.band)
    log(event="teams", n=len(teams))

    # ---- warm data
    Fw = Iw = Yw = None
    if args.warm:
        Fw = np.load(args.warm + "_F.npy", mmap_mode="r")
        Iw = np.load(args.warm + "_I.npy", mmap_mode="r")
        Yw = np.load(args.warm + "_y.npy")
        nh = min(20000, len(Yw) // 20)
        Fh, Ih, Yh = np.array(Fw[-nh:]), np.array(Iw[-nh:]), Yw[-nh:]
        Fw, Iw, Yw = Fw[:-nh], Iw[:-nh], Yw[:-nh]
        log(event="warm_data", train=len(Yw), heldout=nh)

    step = 0
    if args.init:
        ck = torch.load(args.init, map_location=dev)
        net.load_state_dict(ck["net"]); ema.load_state_dict(ck["ema"])
        log(event="init", path=args.init)
    elif args.warm and args.warm_epochs > 0:
        nw = len(Yw)
        total = int(args.warm_epochs * nw / args.batch)
        sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=args.lr, total_steps=max(total, 1), pct_start=0.1)
        t0 = time.time()
        for k in range(total):
            idx = np.sort(rng.integers(nw, size=args.batch))
            loss, _ = train_step(net, opt, ema, args.ema if k > 200 else 0.0, Fw[idx], Iw[idx], Yw[idx], dev)
            sched.step()
            step += 1
            if k % 200 == 0 or k == total - 1:
                hl = heldout_loss(ema, Fh, Ih, Yh, dev)
                log(event="warm", step=k, of=total, loss=loss, heldout_ema=hl, elapsed=time.time() - t0)
        torch.save({"net": net.state_dict(), "ema": ema.state_dict(), "step": step}, os.path.join(args.out, "warm.pt"))
        for g in opt.param_groups:
            g["lr"] = args.lr

    # ---- self-play
    buf = Buffer(args.buffer)
    if args.warm and args.warm_buffer > 0:
        idx = np.sort(rng.choice(len(Yw), size=min(args.warm_buffer, len(Yw)), replace=False))
        buf.add(np.array(Fw[idx]), np.array(Iw[idx]), Yw[idx], np.zeros(len(idx), np.float32))
        log(event="buffer_seeded", n=buf.n)

    ev = SP.NetEvaluator(ema, dev, compile=True, name="ema")
    league = []          # list of (path, evaluator)
    runner = SP.Runner(args.threads)
    t_start = time.time()
    budget = args.hours * 3600
    save_every = budget / args.saves
    next_save = t_start + save_every
    best = (-1.0, None)
    lam = buf.lam()
    games = []
    gid = 0
    stats = {"games": 0, "states": 0, "draws": 0, "errors": 0, "league_games": 0, "loss": 0.0, "loss_n": 0, "z_abs": 0.0, "t_abs": 0.0, "t_max": 0.0}

    def new_game():
        nonlocal gid
        a, b = teams.sample_pair(rng)
        seed = int(rng.integers(1 << 30))
        opp = ev
        if league and rng.random() < args.league_frac:
            opp = league[int(rng.integers(len(league)))][1]
            stats["league_games"] += 1
        players = [SP.Player(ev, samples=args.samples, epsilon=args.eps), SP.Player(opp, samples=args.samples, epsilon=args.eps)]
        if opp is not ev and rng.random() < 0.5:
            players = players[::-1]
        g = SP.Game(gid, teams.party(a), teams.party(b), seed, players, learner=ev)
        gid += 1
        g.advance()
        return g

    games = [new_game() for _ in range(args.games)]
    last_log = time.time()
    while time.time() - t_start < budget:
        runner.step(games)
        for k, g in enumerate(games):
            if g.done:
                if g.result is None:
                    stats["errors"] += 1
                else:
                    stats["games"] += 1
                    stats["draws"] += g.result == 0.5
                    rows = SP.finish_records(g)
                    if rows is not None:
                        buf.add(*rows)
                        stats["states"] += len(rows[0])
                games[k] = new_game()
        for _ in range(args.train_per_step):
            if buf.n >= args.batch:
                F, I, Y, Ss = buf.sample(rng, args.batch)
                loss, ts_ = train_step(net, opt, ema, args.ema, F, I, Y - lam * np.clip(Ss, -args.clip_s, args.clip_s), dev)
                stats["loss"] += loss; stats["loss_n"] += 1
                stats["z_abs"] += ts_["z_abs"]; stats["t_abs"] += ts_["t_abs"]; stats["t_max"] = max(stats["t_max"], ts_["t_max"])
                step += 1
        if runner.steps % 200 == 0:
            lam = buf.lam()
        if time.time() - last_log > 120:
            ln = max(stats["loss_n"], 1)
            nb = buf.n
            log(event="progress", elapsed=time.time() - t_start, step=step, runner_steps=runner.steps, lam=lam, buffer=nb,
                loss=stats["loss"] / ln, z_abs=stats["z_abs"] / ln, t_abs=stats["t_abs"] / ln, t_max=stats["t_max"],
                s_abs=float(np.abs(buf.S[:nb]).mean()) if nb else 0.0, s_max=float(np.abs(buf.S[:nb]).max()) if nb else 0.0,
                net_states=ev.states, net_time=ev.time, sim_time=runner.sim_time,
                **{k: v for k, v in stats.items() if k not in ("loss", "loss_n", "z_abs", "t_abs", "t_max")})
            stats["loss"] = 0.0; stats["loss_n"] = 0; stats["z_abs"] = 0.0; stats["t_abs"] = 0.0; stats["t_max"] = 0.0
            last_log = time.time()
        if time.time() >= next_save or time.time() - t_start >= budget:
            next_save += save_every
            k = len([f for f in os.listdir(args.out) if f.startswith("ckpt_")])
            path = os.path.join(args.out, f"ckpt_{k:03d}.pt")
            torch.save({"net": net.state_dict(), "ema": ema.state_dict(), "step": step, "args": vars(args)}, path)
            hl = heldout_loss(ema, Fh, Ih, Yh, dev) if args.warm else None
            score, w, d, l = eval_vs_c(ev, args.eval_spec, teams, rng, games=args.eval_games, samples=4, threads=args.threads)
            log(event="checkpoint", path=path, step=step, heldout_ema=hl, eval_score=score, eval_wdl=[w, d, l], lam=lam, games=stats["games"], elapsed=time.time() - t_start)
            if score > best[0]:
                best = (score, path)
                torch.save({"net": net.state_dict(), "ema": ema.state_dict(), "step": step, "args": vars(args), "eval_score": score}, os.path.join(args.out, "best.pt"))
            lnet = ValueNet(d=args.d, heads=args.heads, layers=args.layers).to(dev)
            lnet.load_state_dict(ema.state_dict())
            lnet.eval()
            for p in lnet.parameters():
                p.requires_grad_(False)
            league.append((path, SP.NetEvaluator(lnet, dev, compile=True, name=os.path.basename(path))))
            if len(league) > args.league_size:
                league.pop(0)
    torch.save({"net": net.state_dict(), "ema": ema.state_dict(), "step": step, "args": vars(args)}, os.path.join(args.out, "final.pt"))
    log(event="done", step=step, best=best[1], best_score=best[0], games=stats["games"])


if __name__ == "__main__":
    main()
