#!/usr/bin/env python3
"""Fits the tempo value function (fast/fast_value.c) to one-turn lookahead labels from tests/vfdata.c.

  fit_tempo.py train.csv test.csv        prints correlations and the C weight array
"""
import sys
import numpy as np

def load(path):
    d = np.loadtxt(path, delimiter=",", skiprows=1)
    turn, f, v0, v1 = d[:, 0], d[:, 1:-2], d[:, -2], d[:, -1]
    return turn, f, v0, v1

def expand(f):
    # antisymmetric nonlinear terms: signed squares of the main features (products of two antisymmetric
    # features would be symmetric and vanish, so only odd transforms are useful)
    extra = [f[:, i] * np.abs(f[:, i]) for i in (0, 1, 7, 8, 12, 13, 16)]
    return np.column_stack([f] + extra)

def ridge(X, y, lam):
    A = X.T @ X + lam * np.eye(X.shape[1]); A[0, 0] -= lam   # never shrink V0's weight
    return np.linalg.solve(A, X.T @ y)

tr_turn, tr_f, tr_v0, tr_v1 = load(sys.argv[1])
te_turn, te_f, te_v0, te_v1 = load(sys.argv[2])
print(f"train {len(tr_v1)} states, test {len(te_v1)}")
print(f"baseline corr(v0, v1): train {np.corrcoef(tr_v0, tr_v1)[0,1]:.4f} test {np.corrcoef(te_v0, te_v1)[0,1]:.4f}; rmse {np.sqrt(np.mean((te_v1-te_v0)**2)):.4f}")
print("feature std:", np.round(tr_f.std(axis=0), 3))
for name, Xtr, Xte in (("linear", tr_f, te_f), ("with signed squares", expand(tr_f), expand(te_f))):
    for lam in (1e-3, 1e-1, 1.0, 10.0):
        w = ridge(Xtr, tr_v1, lam)
        p = Xte @ w
        c = np.corrcoef(p, te_v1)[0, 1]
        rmse = np.sqrt(np.mean((p - te_v1) ** 2))
        print(f"{name:20s} lam {lam:6.3f}: test corr {c:.4f} rmse {rmse:.4f} | corr with v0 alone {np.corrcoef(te_v0, te_v1)[0,1]:.4f}")
w = ridge(tr_f, tr_v1, 1.0)
p = te_f @ w
print("\nlinear weights (lam 1):")
print("static const float sTempoW[FS_TEMPO_NF] = {" + ", ".join(f"{x:.5f}f" for x in w) + "};")
print(f"test corr {np.corrcoef(p, te_v1)[0,1]:.4f}")
# by game phase
for lo, hi in ((0, 5), (5, 15), (15, 40), (40, 400)):
    m = (te_turn >= lo) & (te_turn < hi)
    if m.sum() > 50:
        print(f"turns {lo:3d}-{hi:3d}: n {m.sum():5d} corr v0 {np.corrcoef(te_v0[m], te_v1[m])[0,1]:.3f} fitted {np.corrcoef(p[m], te_v1[m])[0,1]:.3f}")
