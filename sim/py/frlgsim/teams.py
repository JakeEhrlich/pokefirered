"""Team corpus access: the combined TSV (ai/data/all_teams.tsv), party bytes, ratings and band pairing."""
import json
import os
import numpy as np
from . import sim as S

SIM_DIR = S.SIM_DIR


class Teams:
    def __init__(self, tsv_path=None, ratings_path=None, min_rating=None, band=150.0):
        tsv_path = tsv_path or os.path.join(SIM_DIR, "ai", "data", "all_teams.tsv")
        ratings_path = ratings_path or os.path.join(SIM_DIR, "ratings", "corpus_team_elo_500k_bt.json")
        rows = {}
        order = []
        with open(tsv_path) as f:
            for line in f:
                if not line.strip():
                    continue
                tid = line.split("\t", 1)[0]
                if tid not in rows:
                    rows[tid] = []
                    order.append(tid)
                rows[tid].append(line)
        self.rows = {k: "".join(v) for k, v in rows.items()}
        ratings = {x["name"]: float(x["rating"]) for x in json.load(open(ratings_path))} if os.path.exists(ratings_path) else {}
        self.ids = [t for t in order if (min_rating is None or ratings.get(t, -1e9) >= min_rating)]
        self.rating = np.array([ratings.get(t, np.nan) for t in self.ids], np.float64)
        self.band = band
        self._party = {}
        # for band pairing: ids sorted by rating
        rated = np.where(np.isfinite(self.rating))[0]
        self._rated_idx = rated[np.argsort(self.rating[rated])]
        self._rated_vals = self.rating[self._rated_idx]

    def __len__(self):
        return len(self.ids)

    def party(self, idx: int) -> bytes:
        p = self._party.get(idx)
        if p is None:
            p = self._party[idx] = S.party_from_tsv(self.rows[self.ids[idx]])
        return p

    def sample_pair(self, rng: np.random.Generator):
        """Two team indices whose ratings are within `band` of each other (falls back to any pair)."""
        a = int(rng.integers(len(self.ids)))
        ra = self.rating[a]
        if np.isfinite(ra) and len(self._rated_idx) > 1:
            lo = np.searchsorted(self._rated_vals, ra - self.band)
            hi = np.searchsorted(self._rated_vals, ra + self.band, side="right")
            if hi - lo > 1:
                for _ in range(8):
                    b = int(self._rated_idx[rng.integers(lo, hi)])
                    if b != a:
                        return a, b
        b = int(rng.integers(len(self.ids)))
        while b == a:
            b = int(rng.integers(len(self.ids)))
        return a, b
