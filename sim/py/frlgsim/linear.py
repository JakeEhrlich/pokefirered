"""Stage 1: the C heuristic (Sim_ValueBasic) as a linear model over per-side features read from encoder records.

phi(side) has FEATURES entries; V = w . (phi(me) - phi(opp)); p = sigmoid(scale * V). `hand_weights()` reproduces the
heuristic (up to its clamps).  `fit()` learns w and the scale by logistic regression on outcome-labelled states.

    python -m frlgsim.linear fit ../ai/data/warm ../ai/runs/linear1.npz
"""
import sys
import numpy as np
import torch
from .sim import Enc
from . import features as FT

# mon feature indices (src/sim_encode.c)
MF_HPFRAC, MF_FAINTED = 48, 50
MF_PSN, MF_BRN, MF_FRZ, MF_PAR, MF_TOX, MF_SLP = 51, 52, 53, 54, 55, 57
MF_ACTIVE = 60
MF_STAGE = 62             # atk def spe spa spd acc eva, (stage-6)/6
MF_CONF, MF_WRAPPED, MF_INFAT, MF_FOCUS, MF_SUB, MF_NIGHTMARE, MF_CURSED = 69, 75, 76, 77, 81, 85, 86
MF_LEECH, MF_PERISH, MF_PERISHT, MF_ROOTED = 90, 93, 94, 100
SF_REFLECT, SF_LSCREEN, SF_MIST, SF_SAFEGUARD, SF_SPIKES = 0, 1, 2, 3, 4

BASE_NAMES = ["alive", "hp", "hp*sleep", "hp*freeze", "hp*toxic", "hp*burn", "hp*para", "hp*poison",
              "stage_atk", "stage_def", "stage_spe", "stage_spa", "stage_spd", "stage_acc", "stage_eva",
              "confused", "infatuated", "substitute", "cursed", "nightmare", "wrapped", "focus_energy",
              "leech_seed", "perish(3-t)", "rooted",
              "reflect", "light_screen", "safeguard", "mist", "spikes"]
# engine-computed block (src/sim_encode.c EncodeExtra), stage 2
EXTRA_NAMES = ["act_expdmg", "act_kofrac", "act_canko", "act_can2hko", "act_bestmult", "act_has_se", "act_has_dmg_pp", "act_dmg_pp",
               "act_priority", "act_status_moves", "act_faster", "speed_logratio", "act_hp", "act_level", "weather_boost", "weather_nerf",
               "act_fainted", "team_expdmg", "team_kofrac", "team_hp", "team_maxhp", "team_faster", "team_se", "team_canko", "team_level",
               "bench_safe", "bench_resist"]
EXTRA_MASK = np.array([0.0 if n in ("weather_boost", "weather_nerf") else 1.0 for n in EXTRA_NAMES], np.float32)
# derived exchange features (from the extra block of both sides): turns-to-KO each way and who wins the exchange
XCHG_NAMES = ["ttk_me", "ttk_them", "xchg_win", "xchg_lose", "xchg_win_faster", "xchg_lose_faster"]
NAMES = BASE_NAMES + EXTRA_NAMES + XCHG_NAMES
BASE_FEATURES = len(BASE_NAMES)
FEATURES = len(NAMES)


def hand_weights():
    w = np.array([0.15, 1.0, -0.30, -0.40, -0.25, -0.20, -0.15, -0.10,
                  0.03, 0.03, 0.03, 0.03, 0.03, 0.0, 0.0,
                  -0.08, -0.08, 0.10, -0.12, -0.06, -0.03, 0.02,
                  -0.08, -0.10, 0.03,
                  0.05, 0.05, 0.03, 0.01, -0.05], np.float32)
    return np.concatenate([w, np.zeros(FEATURES - BASE_FEATURES, np.float32)])


def side_phi(mon, side, present):
    """mon [n,6,MON_F], side [n,SIDE_F], present [n,6] -> phi [n, FEATURES] for one side."""
    alive = present * (1 - mon[..., MF_FAINTED])                     # [n,6]
    hp = mon[..., MF_HPFRAC] * alive
    act = mon[..., MF_ACTIVE] * alive                                 # active and alive
    cols = [alive.sum(1), hp.sum(1)]
    for idx in (MF_SLP, MF_FRZ, MF_TOX, MF_BRN, MF_PAR, MF_PSN):
        cols.append((hp * mon[..., idx]).sum(1))
    for k in range(7):
        cols.append((act * mon[..., MF_STAGE + k] * 6.0).sum(1))
    for idx in (MF_CONF, MF_INFAT, MF_SUB, MF_CURSED, MF_NIGHTMARE, MF_WRAPPED, MF_FOCUS, MF_LEECH):
        cols.append((act * mon[..., idx]).sum(1))
    cols.append((act * mon[..., MF_PERISH] * (3.0 - mon[..., MF_PERISHT] * 3.0)).sum(1))
    cols.append((act * mon[..., MF_ROOTED]).sum(1))
    for idx in (SF_REFLECT, SF_LSCREEN, SF_SAFEGUARD, SF_MIST):
        cols.append((side[:, idx] > 0).astype(np.float32))
    cols.append((side[:, SF_SPIKES:SF_SPIKES + 4] * np.arange(4, dtype=np.float32)).sum(1))
    return np.stack(cols, 1).astype(np.float32)


def delta_phi(F, I):
    """Encoder records (my view) -> phi(me) - phi(opp), [n, FEATURES]."""
    rec = FT.split_records(np.asarray(F, np.float32), np.asarray(I, np.int32))
    mon, side, present = rec["mon"], rec["side"], rec["present"][:, :12].astype(np.float32)
    base = side_phi(mon[:, :6], side[:, 0], present[:, :6]) - side_phi(mon[:, 6:], side[:, 1], present[:, 6:])
    exA, exB = rec["extra"][:, 0, :len(EXTRA_NAMES)], rec["extra"][:, 1, :len(EXTRA_NAMES)]
    ex = (exA - exB) * EXTRA_MASK
    return np.concatenate([base, ex, exchange_features(exA, exB)], 1).astype(np.float32)


def exchange_features(exA, exB):
    """Turns-to-KO between the actives from raw best damage (act_expdmg is raw/100) and HP (act_hp is hp/200); the
    exchange winner is whoever KOs first, speed breaking ties. Antisymmetric by construction (side A's view)."""
    iD, iH, iF = EXTRA_NAMES.index("act_expdmg"), EXTRA_NAMES.index("act_hp"), EXTRA_NAMES.index("act_faster")
    dmgA, dmgB = exA[:, iD] * 100.0, exB[:, iD] * 100.0
    hpA, hpB = exA[:, iH] * 200.0, exB[:, iH] * 200.0
    with np.errstate(divide="ignore", invalid="ignore"):
        ttkA = np.where(dmgA > 0, np.ceil(hpB / np.maximum(dmgA, 1e-6)), 9.0)   # turns for A to KO B
        ttkB = np.where(dmgB > 0, np.ceil(hpA / np.maximum(dmgB, 1e-6)), 9.0)
    ttkA, ttkB = np.minimum(ttkA, 9.0), np.minimum(ttkB, 9.0)
    fasterA = exA[:, iF] > 0
    winA = (ttkA < ttkB) | ((ttkA == ttkB) & fasterA & (ttkA < 9))
    winB = (ttkB < ttkA) | ((ttkB == ttkA) & ~fasterA & (ttkB < 9))
    return np.stack([ttkA / 6.0, ttkB / 6.0, winA.astype(np.float32), winB.astype(np.float32),
                     (winA & fasterA).astype(np.float32), (winB & ~fasterA).astype(np.float32)], 1).astype(np.float32)


class LinearEvaluator:
    """Same interface as selfplay.NetEvaluator.probs. p = sigmoid(scale * w.dphi); the heuristic uses V = clip(w.dphi/7)."""

    def __init__(self, w, scale=None, name="linear", heuristic_clip=False, std=None):
        self.w = np.asarray(w, np.float32)
        self.std = None if std is None else np.asarray(std, np.float32)   # standardized model: w applies to dphi / std
        self.scale = scale
        self.heuristic_clip = heuristic_clip
        self.name = name
        self.calls = self.states = 0
        self.time = 0.0

    def probs(self, F, I):
        E = Enc.load()
        x = delta_phi(F, I)
        if self.std is not None:
            x = x / self.std
        d = x @ self.w
        if self.heuristic_clip:
            v = np.clip(d / 7.0, -1, 1)
            p = (v + 1) / 2
        else:
            p = 1 / (1 + np.exp(-self.scale * d))
        term = np.asarray(I)[:, E.I_TERMINAL]
        p = np.where(term == 1, 1.0, np.where(term == 2, 0.0, np.where(term == 3, 0.5, p))).astype(np.float32)
        self.calls += 1
        self.states += len(p)
        return p


def fit(F, I, Y, epochs=30, lr=0.02, l2=1e-3, min_frac=0.005, batch=8192, log=print, Fh=None, Ih=None, Yh=None, G=None):
    """Standardized logistic regression: x = dphi / std (dphi is antisymmetric so it has mean 0 by construction),
    features nonzero in fewer than min_frac of the states are dropped (std set to inf), L2 on the standardized weights.
    Returns (w, std): the evaluator computes sigmoid((dphi / std) . w)."""
    D = delta_phi(F, I)
    nz = (D != 0).mean(0)
    std = D.std(0).astype(np.float32)
    keep = (nz >= min_frac) & (std > 0)
    std = np.where(keep, std, np.inf).astype(np.float32)
    if log:
        log("dropped: " + ", ".join(f"{NAMES[i]} ({nz[i] * 100:.2f}%)" for i in range(FEATURES) if not keep[i]))
    X = torch.from_numpy(D / std)
    y = torch.from_numpy(np.asarray(Y, np.float32))
    n = len(y)
    w = torch.zeros(FEATURES, requires_grad=True)
    params = [w]
    if G is not None:   # fixed effects: one free offset per game (dropped at play time)
        g = torch.from_numpy(np.asarray(G, np.int64))
        c = torch.zeros(int(g.max()) + 1, requires_grad=True)
        params.append(c)
    opt = torch.optim.Adam(params, lr=lr)
    for ep in range(epochs):
        perm = torch.randperm(n)
        tot = 0.0
        for k in range(0, n, batch):
            idx = perm[k:k + batch]
            z = X[idx] @ w
            if G is not None:
                z = z + c[g[idx]]
            loss = torch.nn.functional.binary_cross_entropy_with_logits(z, y[idx]) + l2 * (w * w).sum()
            opt.zero_grad(); loss.backward(); opt.step()
            tot += float(loss) * len(idx)
        if log and (ep % 10 == 0 or ep == epochs - 1):
            msg = f"epoch {ep} train {tot / n:.4f}"
            if Fh is not None:
                msg += f" heldout {loss_of(w.detach().numpy(), Fh, Ih, Yh, std=std):.4f}"
            log(msg)
    return w.detach().numpy(), std


def loss_of(w, F, I, Y, scale=1.0, std=None):
    x = delta_phi(F, I)
    if std is not None:
        x = x / std
    z = x @ w * scale
    y = np.asarray(Y, np.float64)
    return float(np.mean(np.logaddexp(0, z) - y * z))


# ---------------------------------------------------------------------------------------------------------
# Sibling pairs (tests/branch.c): pairwise ranking loss

def load_pairs(path):
    """Returns dict with FA, IA, FB, IB (float16/int16 arrays) and meta [n,16] from a branch.c output file."""
    E = Enc.load()
    rec = np.dtype([("meta", "<i4", 16), ("FA", "<f2", E.FLOATS), ("IA", "<i2", E.INTS), ("FB", "<f2", E.FLOATS), ("IB", "<i2", E.INTS)])
    d = np.fromfile(path, dtype=rec)
    return {"meta": d["meta"], "FA": d["FA"], "IA": d["IA"], "FB": d["FB"], "IB": d["IB"]}


def fit_pairs(P, epochs=200, lr=0.02, l2=1e-3, min_frac=0.005, outcome_weight=0.0, log=print, heldout_frac=0.1, seed=0):
    """Pairwise loss on discordant sibling pairs: CE(sigmoid(z_A - z_B), 1[A's branch won]), z = (dphi / std) . w,
    plus outcome_weight * BCE(z, outcome) on both children. Features standardized on the pair set (std of dphi over
    all children), rare ones dropped. Returns (w, std, heldout_pair_loss, heldout_pair_acc)."""
    m = P["meta"]
    outA, outB = m[:, 7], m[:, 8]
    disc = (outA >= 0) & (outB >= 0) & (outA != outB) & (outA < 2) & (outB < 2)
    DA = delta_phi(P["FA"], P["IA"])
    DB = delta_phi(P["FB"], P["IB"])
    Dall = np.concatenate([DA, DB], 0)
    nz = (Dall != 0).mean(0)
    std = Dall.std(0).astype(np.float32)
    keep = (nz >= min_frac) & (std > 0)
    std = np.where(keep, std, np.inf).astype(np.float32)
    XA = torch.from_numpy(DA / std)
    XB = torch.from_numpy(DB / std)
    label = torch.from_numpy((outA == 1).astype(np.float32))       # A better
    idx = np.where(disc)[0]
    rng = np.random.default_rng(seed)
    rng.shuffle(idx)
    nh = int(len(idx) * heldout_frac)
    ho, tr = torch.from_numpy(idx[:nh]), torch.from_numpy(idx[nh:])
    yA = torch.from_numpy(np.where(outA == 2, 0.5, outA).astype(np.float32))
    yB = torch.from_numpy(np.where(outB == 2, 0.5, outB).astype(np.float32))
    valid = torch.from_numpy(((outA >= 0) & (outB >= 0)))
    w = torch.zeros(FEATURES, requires_grad=True)
    opt = torch.optim.Adam([w], lr=lr)
    bce = torch.nn.functional.binary_cross_entropy_with_logits

    def pair_loss(ids):
        return bce(XA[ids] @ w - XB[ids] @ w, label[ids])

    for ep in range(epochs):
        loss = pair_loss(tr) + l2 * (w * w).sum()
        if outcome_weight > 0:
            loss = loss + outcome_weight * 0.5 * (bce((XA @ w)[valid], yA[valid]) + bce((XB @ w)[valid], yB[valid]))
        opt.zero_grad(); loss.backward(); opt.step()
        if log and (ep % 50 == 0 or ep == epochs - 1):
            with torch.no_grad():
                hl = float(pair_loss(ho)); acc = float(((XA[ho] @ w - XB[ho] @ w > 0).float() == label[ho]).float().mean())
            log(f"epoch {ep} train {float(loss):.4f} heldout pair loss {hl:.4f} acc {acc:.3f}")
    with torch.no_grad():
        hl = float(pair_loss(ho)); acc = float(((XA[ho] @ w - XB[ho] @ w > 0).float() == label[ho]).float().mean())
    return w.detach().numpy(), std, hl, acc


def pair_metrics(w, std, P, ids=None):
    """Held-out style metrics of any (w, std) on a pair set: pair loss and ranking accuracy on discordant pairs."""
    m = P["meta"]
    outA, outB = m[:, 7], m[:, 8]
    disc = (outA >= 0) & (outB >= 0) & (outA != outB) & (outA < 2) & (outB < 2)
    if ids is not None:
        mask = np.zeros(len(m), bool); mask[ids] = True; disc &= mask
    dz = (delta_phi(P["FA"][disc], P["IA"][disc]) / std) @ w - (delta_phi(P["FB"][disc], P["IB"][disc]) / std) @ w
    lab = (outA[disc] == 1).astype(np.float64)
    loss = float(np.mean(np.logaddexp(0, dz) - lab * dz))
    acc = float(np.mean((dz > 0) == (lab == 1)))
    return loss, acc, int(disc.sum())


# ---------------------------------------------------------------------------------------------------------
# Amplified targets (frlgsim.amplify): regress the linear model onto the search value V1

def fit_amplified(F, I, V, epochs=300, lr=0.05, l2=1e-3, min_frac=0.005, log=print, heldout_frac=0.1, G=None, seed=0):
    """z = (dphi / std) . w fitted so that sigmoid(z) matches the search value p = (V + 1) / 2 (soft-target BCE).
    Held out by game when G is given. Returns (w, std, heldout_loss, heldout_corr)."""
    D = delta_phi(F, I)
    nz = (D != 0).mean(0)
    std = D.std(0).astype(np.float32)
    keep = (nz >= min_frac) & (std > 0)
    std = np.where(keep, std, np.inf).astype(np.float32)
    X = torch.from_numpy(D / std)
    p = torch.from_numpy(((np.asarray(V, np.float32) + 1) / 2).astype(np.float32))
    n = len(p)
    rng = np.random.default_rng(seed)
    if G is not None:
        games = np.unique(G); rng.shuffle(games)
        hold = np.isin(G, games[:int(len(games) * heldout_frac)])
    else:
        hold = rng.random(n) < heldout_frac
    tr, ho = torch.from_numpy(np.where(~hold)[0]), torch.from_numpy(np.where(hold)[0])
    w = torch.zeros(FEATURES, requires_grad=True)
    opt = torch.optim.Adam([w], lr=lr)
    bce = torch.nn.functional.binary_cross_entropy_with_logits
    for ep in range(epochs):
        loss = bce(X[tr] @ w, p[tr]) + l2 * (w * w).sum()
        opt.zero_grad(); loss.backward(); opt.step()
        if log and (ep % 100 == 0 or ep == epochs - 1):
            with torch.no_grad():
                log(f"epoch {ep} train {float(loss):.4f} heldout {float(bce(X[ho] @ w, p[ho])):.4f}")
    with torch.no_grad():
        z = X[ho] @ w
        hl = float(bce(z, p[ho]))
        corr = float(np.corrcoef(torch.sigmoid(z).numpy(), p[ho].numpy())[0, 1])
    return w.detach().numpy(), std, hl, corr


# ---------------------------------------------------------------------------------------------------------
# Stage 3: a small MLP on the same per-side feature differences (still antisymmetric: f(dphi) - f(-dphi))

class MLPValue(torch.nn.Module):
    def __init__(self, n_in, hidden=64):
        super().__init__()
        self.net = torch.nn.Sequential(torch.nn.Linear(n_in, hidden), torch.nn.GELU(), torch.nn.Linear(hidden, hidden), torch.nn.GELU(), torch.nn.Linear(hidden, 1))

    def forward(self, x):
        return (self.net(x) - self.net(-x)).squeeze(-1) * 0.5


def fit_amplified_mlp(F, I, V, hidden=64, epochs=60, lr=2e-3, wd=1e-4, min_frac=0.005, batch=2048, log=print, heldout_frac=0.1, G=None, seed=0):
    D = delta_phi(F, I)
    nz = (D != 0).mean(0)
    std = D.std(0).astype(np.float32)
    keep = (nz >= min_frac) & (std > 0)
    std = np.where(keep, std, np.inf).astype(np.float32)
    X = torch.from_numpy(D / std)
    p = torch.from_numpy(((np.asarray(V, np.float32) + 1) / 2).astype(np.float32))
    n = len(p)
    rng = np.random.default_rng(seed)
    if G is not None:
        games = np.unique(G); rng.shuffle(games)
        hold = np.isin(G, games[:int(len(games) * heldout_frac)])
    else:
        hold = rng.random(n) < heldout_frac
    tr, ho = np.where(~hold)[0], torch.from_numpy(np.where(hold)[0])
    torch.manual_seed(seed)
    net = MLPValue(FEATURES, hidden)
    opt = torch.optim.AdamW(net.parameters(), lr=lr, weight_decay=wd)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, epochs)
    bce = torch.nn.functional.binary_cross_entropy_with_logits
    for ep in range(epochs):
        perm = rng.permutation(tr)
        for k in range(0, len(perm), batch):
            idx = torch.from_numpy(perm[k:k + batch])
            loss = bce(net(X[idx]), p[idx])
            opt.zero_grad(); loss.backward(); opt.step()
        sched.step()
        if log and (ep % 20 == 0 or ep == epochs - 1):
            with torch.no_grad():
                log(f"epoch {ep} heldout {float(bce(net(X[ho]), p[ho])):.4f}")
    with torch.no_grad():
        z = net(X[ho])
        hl = float(bce(z, p[ho]))
        corr = float(np.corrcoef(torch.sigmoid(z).numpy(), p[ho].numpy())[0, 1])
    return net, std, hl, corr


class MLPEvaluator:
    """selfplay.NetEvaluator interface for MLPValue on delta-phi features (CPU)."""

    def __init__(self, net, std, name="mlp"):
        self.net = net.eval()
        self.std = np.asarray(std, np.float32)
        self.name = name
        self.calls = self.states = 0
        self.time = 0.0

    @torch.no_grad()
    def probs(self, F, I):
        E = Enc.load()
        x = torch.from_numpy(delta_phi(F, I) / self.std)
        p = torch.sigmoid(self.net(x)).numpy()
        term = np.asarray(I)[:, E.I_TERMINAL]
        p = np.where(term == 1, 1.0, np.where(term == 2, 0.0, np.where(term == 3, 0.5, p))).astype(np.float32)
        self.calls += 1; self.states += len(p)
        return p


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "fitampmlp":
        # python -m frlgsim.linear fitampmlp ../ai/data/amp1 out.pt [--hidden 64]
        prefix, out = sys.argv[2], sys.argv[3]
        hidden = int(sys.argv[sys.argv.index("--hidden") + 1]) if "--hidden" in sys.argv else 64
        F = np.array(np.load(prefix + "_F.npy", mmap_mode="r")); I = np.array(np.load(prefix + "_I.npy", mmap_mode="r")); V = np.load(prefix + "_v.npy"); G = np.load(prefix + "_g.npy")
        net, std, hl, corr = fit_amplified_mlp(F, I, V, hidden=hidden, G=G)
        print(f"mlp hidden {hidden}: held-out soft-BCE {hl:.4f} corr(p, target) {corr:.3f}")
        torch.save({"state": net.state_dict(), "std": std, "hidden": hidden, "n_in": FEATURES}, out)
        sys.exit(0)
    if cmd == "fitamp":
        # python -m frlgsim.linear fitamp ../ai/data/amp1 out.npz
        prefix, out = sys.argv[2], sys.argv[3]
        F = np.load(prefix + "_F.npy", mmap_mode="r"); I = np.load(prefix + "_I.npy", mmap_mode="r"); V = np.load(prefix + "_v.npy"); G = np.load(prefix + "_g.npy")
        F, I = np.array(F), np.array(I)
        # the hand heuristic vs the same targets (its own raw value, for reference): correlation of clip(w.dphi/7) with V
        d = delta_phi(F, I) @ hand_weights()
        print(f"{len(V)} states; hand heuristic value vs V1: corr {np.corrcoef(np.clip(d / 7, -1, 1), V)[0, 1]:.3f}")
        best = None
        for l2 in (1e-4, 1e-3, 1e-2):
            w, std, hl, corr = fit_amplified(F, I, V, l2=l2, log=None, G=G)
            print(f"l2 {l2:g}: held-out soft-BCE {hl:.4f} corr(p, target) {corr:.3f}")
            if best is None or hl < best[0]:
                best = (hl, l2, w, std, corr)
        hl, l2, w, std, corr = best
        print(f"chosen l2 {l2:g}: held-out {hl:.4f} corr {corr:.3f}")
        for nme, b, sd in zip(NAMES, w, std):
            if b != 0: print(f"  {nme:16s} w {b:+.3f}  (per raw unit {b / sd:+.3f})")
        np.savez(out, w=w, std=std)
        sys.exit(0)
    if cmd == "fitpairs":
        # python -m frlgsim.linear fitpairs pairs.bin out.npz [--outcome 0.1]
        path, out = sys.argv[2], sys.argv[3]
        ow = float(sys.argv[sys.argv.index("--outcome") + 1]) if "--outcome" in sys.argv else 0.0
        P = load_pairs(path)
        hw = hand_weights()
        # the hand heuristic's ranking quality on the same pairs (raw units: std = 1, scale irrelevant to accuracy)
        hl, hacc, nd = pair_metrics(hw * 1.2, np.ones(FEATURES, np.float32), P)
        print(f"{len(P['meta'])} pairs, {nd} discordant; hand weights: pair loss {hl:.4f} acc {hacc:.3f}")
        best = None
        for l2 in (1e-4, 1e-3, 1e-2):
            w, std, hlo, acc = fit_pairs(P, l2=l2, outcome_weight=ow, log=None)
            print(f"l2 {l2:g}: held-out pair loss {hlo:.4f} acc {acc:.3f}")
            if best is None or hlo < best[0]:
                best = (hlo, l2, w, std, acc)
        hlo, l2, w, std, acc = best
        print(f"chosen l2 {l2:g}: held-out pair loss {hlo:.4f} acc {acc:.3f}")
        for nme, b, sd in zip(NAMES, w, std):
            if b != 0: print(f"  {nme:16s} w {b:+.3f}  (per raw unit {b / sd:+.3f})")
        np.savez(out, w=w, std=std)
        sys.exit(0)
    if cmd == "fit":
        prefix, out = sys.argv[2], sys.argv[3]
        Fw = np.load(prefix + "_F.npy", mmap_mode="r"); Iw = np.load(prefix + "_I.npy", mmap_mode="r"); Yw = np.load(prefix + "_y.npy")
        fixed = "--fixed" in sys.argv
        Gw = np.load(prefix + "_g.npy") if fixed else None
        nh = 20000
        Ftr, Itr, Ytr = np.array(Fw[:-nh]), np.array(Iw[:-nh]), Yw[:-nh]
        Gtr = Gw[:-nh] if fixed else None
        Fh, Ih, Yh = np.array(Fw[-nh:]), np.array(Iw[-nh:]), Yw[-nh:]
        hw = hand_weights()
        best = min(((loss_of(hw, Fh, Ih, Yh, s), s) for s in np.linspace(0.2, 6, 30)), key=lambda t: t[0])
        print(f"hand weights: held-out loss {best[0]:.4f} at scale {best[1]:.2f}")
        results = []
        for l2 in ((1e-3,) if fixed else (1e-4, 1e-3, 1e-2, 3e-2)):   # with offsets the between-game held-out loss cannot select l2
            w, std = fit(Ftr, Itr, Ytr, l2=l2, log=None, G=Gtr, epochs=60 if fixed else 30)
            hl = loss_of(w, Fh, Ih, Yh, std=std)   # without offsets: a between-game loss, not the fit's objective
            results.append((hl, l2, w, std))
            print(f"l2 {l2:g}: held-out loss {hl:.4f}  max|w| {np.abs(w).max():.3f}")
        hl, l2, w, std = min(results, key=lambda t: t[0])
        print(f"chosen l2 {l2:g}, held-out {hl:.4f}")
        fit(Ftr[:1], Itr[:1], Ytr[:1], epochs=0, log=print)   # prints the dropped features
        for nme, b, sd in zip(NAMES, w, std):
            print(f"  {nme:14s} w {b:+.3f}  (per raw unit {b / sd if np.isfinite(sd) else 0:+.3f})")
        np.savez(out, w=w, std=std, hand_scale=best[1])






