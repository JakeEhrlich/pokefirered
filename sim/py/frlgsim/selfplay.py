"""Lockstep self-play (ai/DESIGN.md section 3).

Many games advance together. At every decision point the simulator plays out the joint actions (turn start) or
the single answer (mid-turn replacement) for `samples` engine seeds, every resulting state is encoded, all
states of all games go through the network in one batch, and the values are scattered back into each game's
payoff matrix, which RM+ solves. Actions are sampled from the average strategy.

Every decision point of a training game (both players networks) records, from side 0's view: the state, the
learner's value p of it, and the backup b = sum_ij sigma_i sigma_j p_ij under the strategies actually played.
The training loss is BCE(p, y) + BCE(p, b): outcome plus one-turn backup, both in [0, 1] (train.py).
"""
import time
import numpy as np
import torch
from concurrent.futures import ThreadPoolExecutor
from . import sim as S
from . import features as FT
from .sim import Enc

RM_ITERS = 100


class NetEvaluator:
    """Batched win-probability evaluation of encoder records, padded to size buckets (for torch.compile)."""

    def __init__(self, net, device, compile=True, buckets=(256, 512, 1024, 2048, 4096), name="net"):
        self.net = net
        self.device = device
        self.buckets = buckets          # batches larger than the last bucket are evaluated in chunks of that size
        self.fwd = torch.compile(net) if compile else net
        self.name = name
        self.calls = 0
        self.states = 0
        self.time = 0.0

    @torch.no_grad()
    def probs(self, F, I):
        """F [n, FLOATS] float32, I [n, INTS] int32 -> p [n] (my-view win probability); terminal records exact."""
        n = F.shape[0]
        if n == 0:
            return np.zeros(0, np.float32)
        t0 = time.time()
        E = Enc.load()
        chunk = self.buckets[-1]
        out = np.zeros(n, np.float32)
        for k in range(0, n, chunk):
            out[k:k + chunk] = self._probs(F[k:k + chunk], I[k:k + chunk])
        term = I[:, E.I_TERMINAL]
        out = np.where(term == 1, 1.0, np.where(term == 2, 0.0, np.where(term == 3, 0.5, out))).astype(np.float32)
        self.calls += 1
        self.states += n
        self.time += time.time() - t0
        return out

    def _probs(self, F, I):
        n = F.shape[0]
        size = next(b for b in self.buckets if b >= n)
        if size > n:
            F = np.concatenate([F, np.repeat(F[:1], size - n, 0)], 0)
            I = np.concatenate([I, np.repeat(I[:1], size - n, 0)], 0)
        rec = FT.split_records(F, I)
        rec.pop("terminal")
        b = FT.to_torch(rec, self.device)
        z = self.fwd(b)
        return torch.sigmoid(z).float().cpu().numpy()[:n]


def solve(M, iters=RM_ITERS):
    """Row player's payoff matrix in win probability -> (sigma_row, sigma_col) via RM+ on 2p-1."""
    n, m = M.shape
    if n == 1 and m == 1:
        return np.ones(1, np.float32), np.ones(1, np.float32)
    sr, sc = S.regret_matching(2.0 * M - 1.0, iters=iters, plus=True, alternating=True, linear_avg=True)
    sr = np.clip(sr, 0, None); sc = np.clip(sc, 0, None)
    sr = sr / sr.sum() if sr.sum() > 0 else np.ones(n, np.float32) / n
    sc = sc / sc.sum() if sc.sum() > 0 else np.ones(m, np.float32) / m
    return sr.astype(np.float32), sc.astype(np.float32)


class Player:
    """Who decides for one side: a network (evaluator) or a C agent."""

    def __init__(self, evaluator=None, cagent=None, samples=2, epsilon=0.0, name=""):
        self.ev = evaluator
        self.cagent = cagent
        self.samples = samples
        self.epsilon = epsilon
        self.name = name or (evaluator.name if evaluator is not None else cagent.name)

    @property
    def is_net(self):
        return self.ev is not None


class Game:
    def __init__(self, gid, party_a, party_b, seed, players, learner=None, max_turns=300, team_ids=None):
        self.id = gid
        self.sim = S.Sim()
        self.turn_start = S.Sim()
        self.ts_turn = -1
        self.players = players            # [side0, side1]
        self.learner = learner            # evaluator whose values define the training targets (None: no records)
        self.record = learner is not None and all(p.is_net for p in players)
        self.seed = seed
        self.rng = np.random.default_rng(seed)
        self.team_ids = team_ids
        self.done = False
        self.result = None                # side 0 result: 1 win, 0 loss, 0.5 draw; None on engine error
        self.err = None
        self.records = []                 # per decision point (side-0 view): dict(F, I, p, b)
        self.turn_matrix = None           # dict for the current turn start: turn, mine, theirs, sig (per side), M0, p0
        self.decisions = 0
        self.pending = None
        if self.sim.setup(party_a, party_b, seed, max_turns) != 0:
            self.done = True
            self.err = "setup"

    def advance(self):
        """Runs the engine to the next request or the end. Returns True while a decision is pending."""
        if self.done:
            return False
        r = self.sim.run()
        if r == S.RUN_REQUEST:
            b, kind = self.sim.request_battler, self.sim.request_kind
            if kind == S.REQ_ACTION and b == 0:
                self.turn_start.copy_from(self.sim)
                self.ts_turn = self.sim.turn
            self.pending = (b, kind)
            return True
        self.done = True
        self.pending = None
        if r == S.RUN_FINISHED:
            o = self.sim.outcome
            self.result = 1.0 if o == S.OUTCOME_WON else 0.0 if o == S.OUTCOME_LOST else 0.5
        else:
            self.err = r
        return False

    def _answer(self, b, kind, act):
        if self.sim.answer(b, act) != 0:
            legal = self.sim.legal(b, kind)
            if len(legal) == 0 or self.sim.answer(b, legal[0]) != 0:
                self.done = True
                self.err = "answer"
                return
        self.decisions += 1
        self.pending = None
        self.advance()

    def decide_c(self, b, kind):
        pl = self.players[b & 1]
        ts = self.turn_start if (kind == S.REQ_ACTION and self.ts_turn == self.sim.turn) else None
        act = pl.cagent.decide(self.sim, ts, b, kind)
        self._answer(b, kind, act)

    def has_turn_matrix(self):
        return self.turn_matrix is not None and self.turn_matrix["turn"] == self.sim.turn

    def decide_net_action(self, b):
        """Turn-start decision for a network player from the solved turn matrix."""
        side = b & 1
        pl = self.players[side]
        tm = self.turn_matrix
        acts = tm["mine"] if side == 0 else tm["theirs"]
        sig = tm["sig"][side]
        k = int(self.rng.integers(len(acts))) if (pl.epsilon > 0 and self.rng.random() < pl.epsilon) else int(self.rng.choice(len(acts), p=sig))
        if self.record and side == 0 and tm["p0"] is not None:
            s0 = self._played(tm["sig"][0], self.players[0].epsilon)
            s1 = self._played(tm["sig"][1], self.players[1].epsilon)
            self.records.append({"F": tm["F0"], "I": tm["I0"], "p": tm["p0"], "b": float(s0 @ tm["M0"] @ s1)})
        self._answer(b, S.REQ_ACTION, acts[k])

    def decide_net_switch(self, b, acts, sig, M0, p0, F0, I0):
        side = b & 1
        pl = self.players[side]
        k = int(self.rng.integers(len(acts))) if (pl.epsilon > 0 and self.rng.random() < pl.epsilon) else int(self.rng.choice(len(acts), p=sig))
        if self.record and p0 is not None:
            s = self._played(sig, pl.epsilon)
            self.records.append({"F": F0, "I": I0, "p": p0, "b": float(s @ M0)})
        self._answer(b, S.REQ_SWITCH, acts[k])

    @staticmethod
    def _played(sig, eps):
        return (1 - eps) * sig + eps / len(sig)


class Runner:
    """Advances a set of games in lockstep."""

    def __init__(self, threads=8):
        self.pool = ThreadPoolExecutor(threads)
        self.sim_time = 0.0
        self.steps = 0

    def step(self, games):
        """One decision for every game with a pending request (games must have been advance()d). Returns the count."""
        active = [g for g in games if not g.done and g.pending is not None]
        if not active:
            return 0
        jobs = []
        for g in active:
            b, kind = g.pending
            pl = g.players[b & 1]
            if not pl.is_net:
                g.decide_c(b, kind)
            elif kind == S.REQ_ACTION and g.has_turn_matrix():
                g.decide_net_action(b)
            elif kind == S.REQ_SWITCH and len(g.sim.legal(b, kind)) == 0:
                g._answer(b, kind, S.action(S.B_ACTION_SWITCH, party_slot=0))   # nothing legal: let the engine sort it out
            else:
                jobs.append((g, b, kind))
        if not jobs:
            return len(active)

        def simulate(job):
            g, b, kind = job
            samples = max(p.samples for p in g.players if p.is_net)
            seed = int(g.rng.integers(1 << 31))
            if kind == S.REQ_ACTION:
                ts = g.turn_start
                mine = ts.legal(0, S.REQ_ACTION)
                theirs = ts.legal(1, S.REQ_ACTION)
                F, I = ts.joint_encode(0, mine, theirs, samples, seed, 0)
            else:
                mine = g.sim.legal(b, kind)
                theirs = None
                F, I = g.sim.after_encode(b, mine, samples, seed, 0)
            F0, I0 = g.sim.encode(0)
            return (g, b, kind, mine, theirs, samples, F, I, F0, I0)

        t0 = time.time()
        results = list(self.pool.map(simulate, jobs))
        self.sim_time += time.time() - t0
        # one batch per evaluator that any of these games needs
        Fs, Is, spans = [], [], []
        off = 0
        for r in results:
            F, I, F0, I0 = r[6], r[7], r[8], r[9]
            Fs.append(F); Is.append(I); Fs.append(F0[None]); Is.append(I0[None])
            spans.append((off, off + len(F)))
            off += len(F) + 1
        Fall = np.concatenate(Fs, 0)
        Iall = np.concatenate(Is, 0)
        evs = {}
        for r in results:
            g = r[0]
            for pl in g.players:
                if pl.is_net:
                    evs[id(pl.ev)] = pl.ev
            if g.learner is not None:
                evs[id(g.learner)] = g.learner
        P = {k: ev.probs(Fall, Iall) for k, ev in evs.items()}
        for (g, b, kind, mine, theirs, samples, F, I, F0, I0), (a0, a1) in zip(results, spans):
            n = len(mine)
            m = len(theirs) if theirs is not None else 1
            side = b & 1

            def matrix(ev):
                return P[id(ev)][a0:a1].reshape(n, m, samples).mean(2)   # side-0 win probability

            learner = g.learner
            M0 = matrix(learner) if learner is not None else None
            p0 = float(P[id(learner)][a1]) if learner is not None else None
            if kind == S.REQ_ACTION:
                sig = {}
                for s_ in (0, 1):
                    pl = g.players[s_]
                    if pl.is_net:
                        sr, sc = solve(matrix(pl.ev))
                        sig[s_] = sr if s_ == 0 else sc
                    else:
                        sig[s_] = None
                g.turn_matrix = {"turn": g.sim.turn, "mine": mine, "theirs": theirs, "sig": sig, "M0": M0, "p0": p0, "F0": F0, "I0": I0}
                g.decide_net_action(b)
            else:
                Mside = matrix(g.players[side].ev)
                if side == 1:
                    Mside = 1.0 - Mside
                sr, _ = solve(Mside)
                g.decide_net_switch(b, mine, sr, M0[:, 0] if M0 is not None else None, p0, F0, I0)
        self.steps += 1
        return len(active)


def finish_records(g):
    """A finished game's records -> training rows (F float16, I int16, y, b): the side-0 result and the backup
    b = sum_ij sigma_i sigma_j p_ij at each decision point (both in [0, 1])."""
    if g.result is None or not g.records:
        return None
    y = g.result
    n = len(g.records)
    b = np.array([r["b"] for r in g.records], np.float32)
    F = np.stack([r["F"] for r in g.records]).astype(np.float16)
    I = np.stack([r["I"] for r in g.records]).astype(np.int16)
    return F, I, np.full(n, y, np.float32), b


def play_games(games, runner, max_steps=100000, on_progress=None):
    """Runs games to completion."""
    for g in games:
        g.advance()
    steps = 0
    while any(not g.done for g in games) and steps < max_steps:
        runner.step(games)
        steps += 1
        if on_progress and steps % 20 == 0:
            on_progress(steps, sum(g.done for g in games))
    return games
