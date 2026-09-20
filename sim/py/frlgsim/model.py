"""The value network (ai/DESIGN.md section 2): entity tokens, relation-aware attention, antisymmetric head.

forward(batch) -> logits z [B] from the batch's own ("my") perspective; p = sigmoid(z) = P(I win).
The other perspective is built by features.flip and both run through the same encoder, so z(flip) == -z exactly.
"""
import math
import torch
import torch.nn as nn
import torch.nn.functional as Fn
from .sim import Enc
from . import features as FT

N_MOVES, N_ITEMS, N_ABILITIES = 360, 380, 80
N_CATS = 11


class RelAttention(nn.Module):
    """Multi-head attention whose structural half of q/k is transformed per token category (query side: the
    query's category; key side: the key's category and whether it shares the query's owner), plus a bias per
    (cat_i, cat_j, same-owner) relation."""

    def __init__(self, d, heads):
        super().__init__()
        self.d, self.h, self.dh = d, heads, d // heads
        self.hs = self.dh // 2
        self.qkv = nn.Linear(d, 3 * d)
        self.out = nn.Linear(d, d)
        hs = self.hs
        self.A = nn.Parameter(torch.zeros(heads, N_CATS, hs, hs))        # query transform (off-diagonal), diag pinned 1
        self.Bk = nn.Parameter(torch.zeros(heads, N_CATS, 2, hs, hs))    # key transform by (cat_j, same owner)
        self.bias = nn.Parameter(torch.zeros(heads, N_CATS * N_CATS * 2))
        self.register_buffer("eye", torch.eye(hs), persistent=False)
        bench, active = FT.position_cats()
        self.register_buffer("benchcat", torch.from_numpy(bench), persistent=False)
        self.register_buffer("activecat", torch.from_numpy(active), persistent=False)

    def forward(self, x, cat, same, keymask):
        # x [B,N,d]; cat [B,N] long; same [N,N] float; keymask [B,N] bool
        B, N, _ = x.shape
        q, k, v = self.qkv(x).view(B, N, 3, self.h, self.dh).unbind(2)
        q, k, v = q.transpose(1, 2), k.transpose(1, 2), v.transpose(1, 2)     # [B,H,N,dh]
        hs = self.hs
        qc, qs = q[..., :hs], q[..., hs:]
        kc, ks = k[..., :hs], k[..., hs:]
        A = self.eye + self.A * (1 - self.eye)                                  # [H,C,hs,hs]
        Bk = self.eye + self.Bk * (1 - self.eye)                                # [H,C,2,hs,hs]
        # Each token position has two possible categories (bench/active variant, or one for side/field), so the
        # per-category transforms are two batched matmuls per side, selected by which variant the token is in.
        act = (cat == self.activecat[None]).to(x.dtype)[:, None, :, None]         # [B,1,N,1]
        A_b, A_a = A[:, self.benchcat], A[:, self.activecat]                          # [H,N,hs,hs]
        qs2 = torch.einsum("bhns,hnst->bhnt", qs, A_b) * (1 - act) + torch.einsum("bhns,hnst->bhnt", qs, A_a) * act
        ks_same = torch.einsum("bhns,hnst->bhnt", ks, Bk[:, self.benchcat, 1]) * (1 - act) + torch.einsum("bhns,hnst->bhnt", ks, Bk[:, self.activecat, 1]) * act
        ks_diff = torch.einsum("bhns,hnst->bhnt", ks, Bk[:, self.benchcat, 0]) * (1 - act) + torch.einsum("bhns,hnst->bhnt", ks, Bk[:, self.activecat, 0]) * act
        logit = torch.matmul(qc, kc.transpose(-1, -2))
        s_same = torch.matmul(qs2, ks_same.transpose(-1, -2))
        s_diff = torch.matmul(qs2, ks_diff.transpose(-1, -2))
        logit = logit + s_same * same + s_diff * (1 - same)
        rel = (cat[:, :, None] * N_CATS + cat[:, None, :]) * 2 + same.long()    # [B,N,N]
        logit = logit + self.bias[:, rel].permute(1, 0, 2, 3)                   # [B,H,N,N]
        logit = logit / math.sqrt(self.dh)
        logit = logit.masked_fill(~keymask[:, None, None, :], -1e4)
        att = torch.softmax(logit, -1)
        y = torch.matmul(att, v).transpose(1, 2).reshape(B, N, self.d)
        return self.out(y)


class Block(nn.Module):
    def __init__(self, d, heads, ffn_mult=4):
        super().__init__()
        self.ln1 = nn.LayerNorm(d)
        self.att = RelAttention(d, heads)
        self.ln2 = nn.LayerNorm(d)
        self.ffn = nn.Sequential(nn.Linear(d, ffn_mult * d), nn.GELU(), nn.Linear(ffn_mult * d, d))

    def forward(self, x, cat, same, keymask):
        x = x + self.att(self.ln1(x), cat, same, keymask)
        x = x + self.ffn(self.ln2(x))
        return x


class ValueNet(nn.Module):
    def __init__(self, d=128, heads=4, layers=3, ffn_mult=4):
        super().__init__()
        E = Enc.load()
        self.d = d
        self.mon_in = nn.Sequential(nn.Linear(E.MON_F, d), nn.GELU(), nn.Linear(d, d))
        self.move_in = nn.Sequential(nn.Linear(E.MOVE_F, d), nn.GELU(), nn.Linear(d, d))
        self.side_in = nn.Linear(E.SIDE_F, d)
        self.field_in = nn.Linear(E.FIELD_F, d)
        self.e_item = nn.Embedding(N_ITEMS, d)
        self.e_ability = nn.Embedding(N_ABILITIES, d)
        self.e_move = nn.Embedding(N_MOVES, d)
        self.e_cat = nn.Embedding(N_CATS, d)
        self.ln_in = nn.LayerNorm(d)
        self.blocks = nn.ModuleList([Block(d, heads, ffn_mult) for _ in range(layers)])
        self.ln_out = nn.LayerNorm(d)
        self.pool_mlp = nn.Sequential(nn.Linear(2 * d, 2 * d), nn.GELU(), nn.Linear(2 * d, d))
        self.g = nn.Sequential(nn.Linear(2 * d, 2 * d), nn.GELU(), nn.Linear(2 * d, d))
        self.w = nn.Linear(d, 1, bias=False)
        own = torch.from_numpy(FT.token_owner())
        self.register_buffer("same", (own[:, None] == own[None, :]).float(), persistent=False)

    def encode(self, b):
        """b: my-view batch dict -> pooled vector h [B, d]."""
        x_mon = self.mon_in(b["mon"]) + self.e_item(b["item"]) + self.e_ability(b["ability"])
        x_move = self.move_in(b["move"]) + self.e_move(b["move_id"])
        x_side = self.side_in(b["side"])
        x_field = self.field_in(b["field"])[:, None, :]
        x = torch.cat([x_mon, x_move, x_side, x_field], 1) + self.e_cat(b["cat"])
        x = self.ln_in(x)
        present = b["present"] > 0
        for blk in self.blocks:
            x = blk(x, b["cat"], self.same, present)
        x = self.ln_out(x)
        m = present[..., None].to(x.dtype)
        mean = (x * m).sum(1) / m.sum(1).clamp(min=1)
        mx = x.masked_fill(~present[..., None], -1e4).max(1).values
        return self.pool_mlp(torch.cat([mean, mx], -1))

    def forward(self, b):
        B = b["mon"].shape[0]
        fb = FT.flip(b)
        both = {k: torch.cat([b[k], fb[k]], 0) for k in ("mon", "move", "side", "field", "item", "ability", "move_id", "cat", "present")}
        h = self.encode(both)
        h_me, h_op = h[:B], h[B:]
        z = self.w(self.g(torch.cat([h_me, h_op], -1)) - self.g(torch.cat([h_op, h_me], -1)))
        return z.squeeze(-1)


def count_params(m):
    return sum(p.numel() for p in m.parameters())
