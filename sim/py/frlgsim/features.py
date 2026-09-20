"""Turning encoder records (sim_encode.h) into model tensors: batch assembly, the perspective flip and the
fixed token structure (owners, same-owner table)."""
import numpy as np
import torch
from .sim import Enc

# mon float indices used by the flip (src/sim_encode.c)
MF_S3_LEECH, MF_S3_LEECHMINE = 90, 91
MF_FSIGHT, MF_FSIGHTMINE = 121, 123
FF_REQMINE, FF_REQANY = 9, 10

CAT_FLIP = [2, 3, 0, 1, 6, 7, 4, 5, 9, 8, 10]


def token_owner():
    """Owner id per token: mon tokens own themselves, move tokens their mon, side/field tokens are unique."""
    E = Enc.load()
    own = np.zeros(E.TOKENS, np.int64)
    own[:12] = np.arange(12)
    own[12:60] = np.arange(48) // 4
    own[60:] = 100 + np.arange(3)
    return own


def split_records(F: np.ndarray, I: np.ndarray):
    """F [B, FLOATS] float32, I [B, INTS] int32 -> dict of numpy arrays."""
    E = Enc.load()
    B = F.shape[0]
    return {
        "mon": F[:, :E.OFF_MOVE].reshape(B, E.MONS, E.MON_F),
        "move": F[:, E.OFF_MOVE:E.OFF_SIDE].reshape(B, E.MOVES, E.MOVE_F),
        "side": F[:, E.OFF_SIDE:E.OFF_FIELD].reshape(B, 2, E.SIDE_F),
        "field": F[:, E.OFF_FIELD:E.OFF_FIELD + E.FIELD_F],
        "extra": F[:, E.OFF_EXTRA:E.OFF_EXTRA + 2 * E.EXTRA_F].reshape(B, 2, E.EXTRA_F),
        "item": I[:, E.I_ITEM:E.I_ITEM + 12],
        "ability": I[:, E.I_ABILITY:E.I_ABILITY + 12],
        "move_id": I[:, E.I_MOVEID:E.I_MOVEID + 48],
        "cat": I[:, E.I_TOKCAT:E.I_TOKCAT + 63],
        "present": I[:, E.I_PRESENT:E.I_PRESENT + 63],
        "terminal": I[:, E.I_TERMINAL],
    }


def to_torch(rec, device):
    out = {}
    for k, v in rec.items():
        t = torch.from_numpy(np.ascontiguousarray(v))
        if t.dtype == torch.int32:
            t = t.long()
        if t.dtype == torch.float16:
            t = t.float()
        out[k] = t.to(device, non_blocking=True)
    return out


def flip(b):
    """The same states seen from the other side (torch tensors, any leading batch shape)."""
    mon = torch.cat([b["mon"][:, 6:], b["mon"][:, :6]], 1).clone()
    leech = mon[..., MF_S3_LEECH]
    mon[..., MF_S3_LEECHMINE] = leech - mon[..., MF_S3_LEECHMINE]
    fs = (mon[..., MF_FSIGHT] > 0).float()
    mon[..., MF_FSIGHTMINE] = fs - mon[..., MF_FSIGHTMINE]
    move = torch.cat([b["move"][:, 24:], b["move"][:, :24]], 1)
    side = torch.cat([b["side"][:, 1:], b["side"][:, :1]], 1)
    field = b["field"].clone()
    field[:, FF_REQMINE] = field[:, FF_REQANY] - field[:, FF_REQMINE]
    catmap = torch.tensor(CAT_FLIP, device=b["cat"].device)
    cat = catmap[b["cat"]]
    cat = torch.cat([cat[:, 6:12], cat[:, :6], cat[:, 36:60], cat[:, 12:36], cat[:, 61:62], cat[:, 60:61], cat[:, 62:63]], 1)
    pr = b["present"]
    present = torch.cat([pr[:, 6:12], pr[:, :6], pr[:, 36:60], pr[:, 12:36], pr[:, 61:62], pr[:, 60:61], pr[:, 62:63]], 1)
    out = {
        "mon": mon, "move": move, "side": side, "field": field,
        "item": torch.cat([b["item"][:, 6:], b["item"][:, :6]], 1),
        "ability": torch.cat([b["ability"][:, 6:], b["ability"][:, :6]], 1),
        "move_id": torch.cat([b["move_id"][:, 24:], b["move_id"][:, :24]], 1),
        "cat": cat, "present": present,
    }
    if "terminal" in b:
        t = b["terminal"]
        out["terminal"] = torch.where(t == 1, 2, torch.where(t == 2, 1, t))
    return out


def position_cats():
    """Per token position (my view): the bench-variant and active-variant category ids."""
    bench = np.zeros(63, np.int64)
    active = np.zeros(63, np.int64)
    bench[0:6], active[0:6] = 1, 0
    bench[6:12], active[6:12] = 3, 2
    bench[12:36], active[12:36] = 5, 4
    bench[36:60], active[36:60] = 7, 6
    bench[60] = active[60] = 8
    bench[61] = active[61] = 9
    bench[62] = active[62] = 10
    return bench, active
