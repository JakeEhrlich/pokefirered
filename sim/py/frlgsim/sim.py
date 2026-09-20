"""ctypes binding of the FRLG battle simulator (sim/build/libfrlgsim.dylib, include/sim_capi.h).

A `Sim` owns one opaque state blob. Actions are 6-byte arrays: type, moveSlot, target, partySlot, item lo, item hi.
ctypes releases the GIL around every call, so games can be run from a thread pool.
"""
import ctypes as C
import os
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SIM_DIR = os.path.abspath(os.path.join(HERE, "..", ".."))
LIB_PATH = os.environ.get("FRLGSIM_LIB", os.path.join(SIM_DIR, "build", "libfrlgsim.dylib"))

B_ACTION_USE_MOVE, B_ACTION_USE_ITEM, B_ACTION_SWITCH, B_ACTION_RUN = 0, 1, 2, 3
REQ_NONE, REQ_ACTION, REQ_SWITCH = 0, 1, 2
RUN_REQUEST, RUN_FINISHED, RUN_STUCK, RUN_ERROR = 0, 1, 2, 3
OUTCOME_WON, OUTCOME_LOST, OUTCOME_DREW = 1, 2, 3   # gBattleOutcome low bits (player side perspective)

_lib = None


def lib():
    global _lib
    if _lib is None:
        L = C.CDLL(LIB_PATH)
        f32p, i32p, u8p, vp = C.POINTER(C.c_float), C.POINTER(C.c_int32), C.POINTER(C.c_uint8), C.c_void_p
        sigs = {
            "sim_state_size": (C.c_size_t, []),
            "sim_setup_game": (C.c_int, [vp, vp, vp, C.c_uint32, C.c_int]),
            "sim_party_from_tsv": (C.c_int, [C.c_char_p, vp]),
            "sim_run": (C.c_int, [vp]),
            "sim_request_battler": (C.c_int, [vp]),
            "sim_request_kind": (C.c_int, [vp]),
            "sim_answer": (C.c_int, [vp, C.c_int, u8p]),
            "sim_legal_actions": (C.c_int, [vp, C.c_int, u8p, C.c_int]),
            "sim_legal_switches": (C.c_int, [vp, C.c_int, u8p, C.c_int]),
            "sim_finished": (C.c_int, [vp]),
            "sim_outcome": (C.c_int, [vp]),
            "sim_turn": (C.c_int, [vp]),
            "sim_error": (C.c_int, [vp]),
            "sim_enc_sizes": (None, [i32p]),
            "sim_encode": (C.c_int, [vp, C.c_int, f32p, i32p]),
            "sim_joint_encode": (C.c_int, [vp, C.c_int, u8p, C.c_int, u8p, C.c_int, C.c_int, C.c_uint32, C.c_int, f32p, i32p]),
            "sim_after_encode": (C.c_int, [vp, C.c_int, u8p, C.c_int, C.c_int, C.c_uint32, C.c_int, f32p, i32p]),
            "sim_regret_matching": (None, [f32p, C.c_int, C.c_int, C.c_int, C.c_int, C.c_int, C.c_int, f32p, f32p]),
            "sim_agent_new": (vp, [C.c_char_p, C.c_uint32]),
            "sim_agent_free": (None, [vp]),
            "sim_agent_name": (C.c_char_p, [vp]),
            "sim_agent_decide": (C.c_int, [vp, vp, vp, C.c_int, C.c_int, u8p]),
            "sim_value_basic": (C.c_float, [vp, C.c_int]),
            "sim_species_name": (C.c_char_p, [C.c_int]),
            "sim_move_name": (C.c_char_p, [C.c_int]),
            "sim_item_name": (C.c_char_p, [C.c_int]),
            "sim_ability_name": (C.c_char_p, [C.c_int]),
            "sim_get_party_mon": (C.c_int, [vp, C.c_int, C.c_int, i32p]),
            "sim_get_battler": (C.c_int, [vp, C.c_int, i32p]),
        }
        for name, (res, args) in sigs.items():
            fn = getattr(L, name)
            fn.restype, fn.argtypes = res, args
        _lib = L
    return _lib


class Enc:
    """Encoding layout constants (include/sim_encode.h)."""
    _loaded = False

    @classmethod
    def load(cls):
        if cls._loaded:
            return cls
        out = np.zeros(16, np.int32)
        lib().sim_enc_sizes(out.ctypes.data_as(C.POINTER(C.c_int32)))
        (cls.FLOATS, cls.INTS, cls.MONS, cls.MOVES, cls.TOKENS, cls.MON_F, cls.MOVE_F, cls.SIDE_F, cls.FIELD_F, cls.CATS, cls.EXTRA_F) = [int(x) for x in out[:11]]
        cls.I_ITEM, cls.I_ABILITY, cls.I_MOVEID, cls.I_TOKCAT, cls.I_PRESENT, cls.I_TERMINAL = 0, 12, 24, 72, 135, 198
        cls.OFF_MOVE = cls.MONS * cls.MON_F
        cls.OFF_SIDE = cls.OFF_MOVE + cls.MOVES * cls.MOVE_F
        cls.OFF_FIELD = cls.OFF_SIDE + 2 * cls.SIDE_F
        cls.OFF_EXTRA = cls.OFF_FIELD + cls.FIELD_F
        cls._loaded = True
        return cls


STATE_SIZE = None


def state_size():
    global STATE_SIZE
    if STATE_SIZE is None:
        STATE_SIZE = lib().sim_state_size()
    return STATE_SIZE


def party_from_tsv(rows: str) -> bytes:
    buf = C.create_string_buffer(600)
    n = lib().sim_party_from_tsv(rows.encode(), buf)
    if n <= 0:
        raise ValueError("no mons parsed from rows")
    return buf.raw


def action(type_, move_slot=0, target=0, party_slot=0, item=0):
    return np.array([type_, move_slot, target, party_slot, item & 0xFF, item >> 8], np.uint8)


def _u8p(a):
    return a.ctypes.data_as(C.POINTER(C.c_uint8))


class Sim:
    def __init__(self):
        self.buf = C.create_string_buffer(state_size())
        self.ptr = C.cast(self.buf, C.c_void_p)

    def setup(self, party_a: bytes, party_b: bytes, seed: int, max_turns: int = 300) -> int:
        return lib().sim_setup_game(self.ptr, party_a, party_b, seed & 0xFFFFFFFF, max_turns)

    def copy_from(self, other: "Sim"):
        C.memmove(self.buf, other.buf, state_size())

    def clone(self) -> "Sim":
        s = Sim()
        s.copy_from(self)
        return s

    def run(self) -> int:
        return lib().sim_run(self.ptr)

    @property
    def request_battler(self):
        return lib().sim_request_battler(self.ptr)

    @property
    def request_kind(self):
        return lib().sim_request_kind(self.ptr)

    @property
    def finished(self):
        return lib().sim_finished(self.ptr) != 0

    @property
    def outcome(self):
        return lib().sim_outcome(self.ptr) & 7

    @property
    def turn(self):
        return lib().sim_turn(self.ptr)

    @property
    def error(self):
        return lib().sim_error(self.ptr)

    def answer(self, battler: int, act: np.ndarray) -> int:
        act = np.ascontiguousarray(act, np.uint8)
        return lib().sim_answer(self.ptr, battler, _u8p(act))

    def legal_actions(self, battler: int) -> np.ndarray:
        out = np.zeros((32, 6), np.uint8)
        n = lib().sim_legal_actions(self.ptr, battler, _u8p(out), 32)
        return out[:n].copy()

    def legal_switches(self, battler: int) -> np.ndarray:
        out = np.zeros(6, np.uint8)
        n = lib().sim_legal_switches(self.ptr, battler, _u8p(out), 6)
        return out[:n].copy()

    def legal(self, battler: int, kind: int) -> np.ndarray:
        """Legal answers for a request of `kind` as 6-byte actions."""
        if kind == REQ_SWITCH:
            slots = self.legal_switches(battler)
            acts = np.zeros((len(slots), 6), np.uint8)
            acts[:, 0] = B_ACTION_SWITCH
            acts[:, 3] = slots
            return acts
        return self.legal_actions(battler)

    def encode(self, side: int, out_f=None, out_i=None):
        E = Enc.load()
        if out_f is None:
            out_f = np.zeros(E.FLOATS, np.float32)
        if out_i is None:
            out_i = np.zeros(E.INTS, np.int32)
        lib().sim_encode(self.ptr, side, out_f.ctypes.data_as(C.POINTER(C.c_float)), out_i.ctypes.data_as(C.POINTER(C.c_int32)))
        return out_f, out_i

    def joint_encode(self, me: int, mine: np.ndarray, theirs: np.ndarray, samples: int, seed: int, side: int):
        """From this turn-start state: every (mine[i], theirs[j]) pair x samples, encoded from `side`. Returns (F, I) with
        shapes [n*m*samples, FLOATS] / [n*m*samples, INTS]; index (i*m + j)*samples + s."""
        E = Enc.load()
        n, m = len(mine), len(theirs)
        k = n * m * samples
        F = np.zeros((k, E.FLOATS), np.float32)
        I = np.zeros((k, E.INTS), np.int32)
        mine = np.ascontiguousarray(mine, np.uint8)
        theirs = np.ascontiguousarray(theirs, np.uint8)
        lib().sim_joint_encode(self.ptr, me, _u8p(mine), n, _u8p(theirs), m, samples, seed & 0xFFFFFFFF, side,
                               F.ctypes.data_as(C.POINTER(C.c_float)), I.ctypes.data_as(C.POINTER(C.c_int32)))
        return F, I

    def after_encode(self, me: int, acts: np.ndarray, samples: int, seed: int, side: int):
        E = Enc.load()
        n = len(acts)
        k = n * samples
        F = np.zeros((k, E.FLOATS), np.float32)
        I = np.zeros((k, E.INTS), np.int32)
        acts = np.ascontiguousarray(acts, np.uint8)
        lib().sim_after_encode(self.ptr, me, _u8p(acts), n, samples, seed & 0xFFFFFFFF, side,
                               F.ctypes.data_as(C.POINTER(C.c_float)), I.ctypes.data_as(C.POINTER(C.c_int32)))
        return F, I

    def value_basic(self, side: int) -> float:
        return lib().sim_value_basic(self.ptr, side)

    def party_mon(self, side, slot):
        out = np.zeros(32, np.int32)
        lib().sim_get_party_mon(self.ptr, side, slot, out.ctypes.data_as(C.POINTER(C.c_int32)))
        return out

    def battler(self, b):
        out = np.zeros(64, np.int32)
        lib().sim_get_battler(self.ptr, b, out.ctypes.data_as(C.POINTER(C.c_int32)))
        return out


def regret_matching(M: np.ndarray, iters=100, plus=True, alternating=True, linear_avg=True):
    """RM / RM+ on the row player's payoff matrix M [n, m]. Returns (sigma_row, sigma_col)."""
    M = np.ascontiguousarray(M, np.float32)
    n, m = M.shape
    sr = np.zeros(n, np.float32)
    sc = np.zeros(m, np.float32)
    lib().sim_regret_matching(M.ctypes.data_as(C.POINTER(C.c_float)), n, m, iters, int(plus), int(alternating), int(linear_avg),
                              sr.ctypes.data_as(C.POINTER(C.c_float)), sc.ctypes.data_as(C.POINTER(C.c_float)))
    return sr, sc


class CAgent:
    """One of the built-in C agents (sim_agent.h specs)."""

    def __init__(self, spec: str, seed: int = 1):
        self.h = lib().sim_agent_new(spec.encode(), seed & 0xFFFFFFFF)
        if not self.h:
            raise ValueError(f"bad agent spec {spec!r}")
        self.spec = spec

    @property
    def name(self):
        return lib().sim_agent_name(self.h).decode()

    def decide(self, sim: Sim, turn_start, battler: int, kind: int) -> np.ndarray:
        out = np.zeros(6, np.uint8)
        ts = turn_start.ptr if turn_start is not None else None
        r = lib().sim_agent_decide(self.h, sim.ptr, ts, battler, kind, _u8p(out))
        if r != 0:
            raise RuntimeError("agent cannot play this side")
        return out

    def __del__(self):
        try:
            lib().sim_agent_free(self.h)
        except Exception:
            pass


def names(kind, i):
    fn = {"species": lib().sim_species_name, "move": lib().sim_move_name, "item": lib().sim_item_name, "ability": lib().sim_ability_name}[kind]
    return fn(int(i)).decode()
