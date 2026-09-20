# Value network for the FRLG battle simulator: architecture proposal

Goal: a fully general PyTorch network `V(state, side) -> [-1, 1]` (zero-sum, from the perspective of
`side`) that can replace `Sim_ValueBasic` inside the regret-matching agents, be trained by self-play later,
and be measured with the existing ELO arena. Scope: singles only (one active battler per side); doubles
is explicitly out of scope for this model. Nothing assumes a fixed roster: every input is an entity token.

## 1. What the network sees

Everything is read from `struct BattleSim` by a C exporter (`Sim_EncodeState(sim, side, record)`), which
writes one binary record per state: categorical ids as `u16`, scalars as `float`, plus a mask. Python
turns records into tokens; the network never sees raw engine fields. The two sides are labelled A and B
only so that tokens can be grouped; the network is built so that swapping the labels negates the value
exactly (section 2), so one forward pass serves both players.

Full information for now (the simulator has it, the game's own AI uses it, and the ratings compare
against agents that use it). Hidden-information variants are a masking change on the exporter, not an
architecture change.

### Tokens: 12 Pokémon, 48 moves, 2 sides, 1 field (63 per stream)

No species identity anywhere. A Pokémon is described only by its attributes, so a new mon with novel
stats/types/ability/moves is just a new combination of known parts: the vocabularies of types (18),
abilities (78), moves (355) and items (375) are the game's and fixed; everything species-specific is
numeric. Every categorical is a full-width (d = 256) embedding added into its token; scalars go through
the token's input MLP. Move and item effects are not embedded separately: the game's move -> effect and
item -> effect maps are fixed, so `E_move[id]` and `E_item[id]` carry them.

```
mon token  = MLP_mon(scalars) + E_item[item] + E_ability[ability] + E_category
move token = MLP_move(E_move[id], move attributes, per-move state) + E_category      one per (mon, slot), 4 per mon
```

Moves are tokens of their own so attention can compare a specific move with a specific defender directly
(this Earthquake against that Levitate token) instead of through a pooled mon vector. A move token is tied
to its owner by the typed relations in section 2, not by position.

Pokémon token scalars:

| feature | encoding |
|---|---|
| current stats (6) | log(stat)/8 (level, nature, EVs/IVs, Transform baked in; stages applied for the active mon). Base stats are not given: every mechanic reads the current stats |
| level | /100. Not redundant with stats: the damage formula has its own (2 x level / 5 + 2) factor, and Seismic Toss / Night Shade deal level damage |
| weight | log(weight)/8 (Low Kick) |
| gender | one-hot 3 (Attract, Cute Charm) |
| held item | E_item[id] plus the item's hold-effect parameter/255 as a scalar (Leftovers 10%, Sitrus 30 HP, Salac at 25% HP, Focus Band 10%, Quick Claw 20%, ...) |
| ability | E_ability[id] |
| types (2) | one-hot 18 each (current battle types for the active mon: Conversion, Camouflage, Transform change them) |
| HP | hp/maxHP, log(maxHP)/8, hp == 0 |
| status1 | poison, burn, freeze, paralysis, toxic + toxic counter/16, asleep + turns asleep so far (see below) |
| role flags | is-active, is-fainted, is-egg/empty (masked out). No side flag: side membership is expressed by the category embeddings |

Move token scalars: attributes from the move table (type one-hot 18, power/250, accuracy/100, priority/7,
target class one-hot, flags: contact, Protect-affected, Magic Coat, Snatch, Mirror Move, King's Rock;
physical/special by type) and state (PP/maxPP, PP == 0, disabled, encored, taunted-usable, choice-locked,
imprisoned, is-last-used, is-last-landed). A missing move slot is masked out.

Happiness is fixed at 255 for every mon (Return at full power, Frustration useless) and is not a feature.

### Hidden timers are exported as elapsed, not remaining

The engine stores the *remaining* duration of its random-length effects: sleep (2-5 turns), confusion
(2-5), Disable (2-5), Encore (2-6), Wrap/Bind (2-5), Thrash/Petal Dance (2-3), Uproar (2-5). A player
cannot see those, only how long the effect has lasted. The exporter therefore emits, for each of them,
"active" plus "turns elapsed since it started", from small counters the simulator keeps next to the engine
(set when the effect is applied, incremented at turn end, cleared on expiry or switch; they live in the
state blob, so rollouts carry them). Fixed-length effects (Taunt 2, screens/weather/Safeguard/Mist 5, Wish 2,
Future Sight 3, Rollout 5, Perish Song's visible countdown) are public and exported as remaining turns.
Caveat: the regret-matching agents' rollouts still simulate with the true counters, so the *search* uses
information the value function does not; acceptable against the full-information heuristics, and for
human play the later fix is to resample hidden timers inside the rollouts.

### Active-battler extras (appended to the active Pokémon token; zeros on the bench)

- stat stages: 7 values as (stage-6)/6, plus one-hot per stage for the four combat stats
- status2: confusion (+elapsed), infatuation, focus energy, transformed, recharge, rage, bide (+elapsed,
  +stored damage/maxHP), lock-on, multiple turns (+elapsed), uproar (+elapsed), wrapped (+elapsed), curse,
  foresight, defense curl, torment, escape prevention, nightmare, substitute (+substitute HP/maxHP)
- status3: leech seed (+seeder on my side / the other side), always-hits, perish song (+remaining, public),
  on-air, underground, underwater, minimized, charged, rooted, yawn (+remaining, fixed), imprisoned, grudge,
  mud sport, water sport
- DisableStruct: disabled move slot (one-hot 4) + elapsed, encored move slot + elapsed, protect uses,
  stockpile count/3, taunt remaining/2, rollout counter/5, fury cutter counter/5, charge, isFirstTurn,
  truant counter, recharge, battler with sure-hit (is-me)
- last move succeeded, used a move last turn (which move: flags on the move tokens)
- wish (remaining/2, incoming HP/maxHP), future sight incoming (remaining/3, damage/maxHP, attacker on my side / other side)

Move attributes and the item/ability/effect tables are read from the simulator's own data tables by the
exporter, so a custom species registered in the simulator (section 6) flows through automatically.

### Side tokens (2: side A, side B)

reflect timer/5, light screen timer/5, mist timer/5, safeguard timer/5, spikes layers (one-hot 4), follow
me active, HP on switch-out/maxHP of the active mon, number of usable mons/6, number fainted/6, a doom
desire / future sight pending against this side.

### Field token (1)

weather (one-hot: none, rain, sun, sand, hail) + duration/5 + permanent flag, turn count/50 (clipped),
the request kind (turn start vs forced replacement). Nothing in the field token refers to a side.

Total: 63 tokens per stream; about 60 scalar features per mon token and about 45 per move token, plus the
summed embeddings, everything in [-1, 1] or [0, 1].

## 2. Architecture

Two twin residual streams, one per perspective, exchanging information layer by layer, then an
antisymmetric head:

```
tokens_A = label(A: self, B: other, field)      tokens_B = label(B: self, A: other, field)      (63 tokens each)
r_A^0 = in(tokens_A)                             r_B^0 = in(tokens_B)
for n in 1..N:   r_A^n = F_n(r_A^{n-1}, r_B^{n-1})        r_B^n = F_n(r_B^{n-1}, r_A^{n-1})
h_A = m(pool(P r_A^N))                           h_B = m(pool(P r_B^N))          (d = 512 throughout)
z(A) = w . ( g(h_A, h_B) - g(h_B, h_A) )         the logit; antisymmetric: z(B) = -z(A)
p(A) = sigmoid(z(A))   = P(A wins)               V(A) = 2 p(A) - 1 is what the agents put in the payoff matrix
```

- `in`: per-token-type MLP (raw -> 512 -> 256) + role embedding (token type, self/other, active/bench, slot).
- `F_n`: pre-LN transformer layer. Queries come from the own stream's 63 tokens; keys/values from the own
  stream's 63 tokens and the twin stream's 63 tokens; every head sees the union of the 126 keys; attention
  is bidirectional (nothing is causal: the output is pooled); the FFN acts on the own stream. Weights of
  F_n are shared by the two streams within a layer.
- Positions are categories and typed relations, never slots. From a stream's point of view every token has
  a category: mon tokens by side (self/other) x activity (active/bench), move tokens the same as their owner
  plus "move", side tokens by side, the field token none. A learned factored embedding, summed
  (e_type + e_side + e_activity), is the token's input role embedding. Bench order and move order carry no
  information, so the model is exactly permutation-invariant over both (no augmentation).
- Every (query, key) pair has a relation type, computed from the query's perspective:
  identity; owns (mon -> one of its moves); not-owned, same side / other side (mon -> a move of another
  mon); owner (move -> its mon); not-owner, same side / other side (move -> a mon that is not its owner);
  sibling (move -> another move of the same mon); self-mon / other-mon (mon -> another mon); self-move /
  other-move (move -> a move of another mon); self-side / other-side; field. 14 types, doubled by own/twin
  stream: 28 relations. A borrowed twin token is re-labelled through the self/other swap, identically in
  both streams, which keeps the symmetry proof intact.
- The relation enters attention as a dense linear transform of the query's structural half. Per layer and
  head, with `r_ij` the relation of the ordered pair (i, j) (a constant table `REL : [N, N]`, N = 126 keys):

  ```
  q = x_i . W_q                      W_q : [d_model, d_head]
  k = x_j . W_k                      W_k : [d_model, d_head]
  g = I + (D[r_ij] * (1 - I))        D   : [R, d_head/2, d_head/2]   (off-diagonal part learned, diagonal pinned to 1)
  q' = q;  q'[d_head/2:] = q[d_head/2:] . g
  logit(x_i, x_j) = ( q' . k  +  q . W_e[r_ij]  +  W_b[r_ij] ) / sqrt(d_head)      W_e : [R, d_head], W_b : [R]
  ```

  The first half of every head compares as plain attention regardless of relation; the second half is
  translated into relation r's key space before the dot product, so the Q/K projections can put features
  there that should only be compared under particular relations (a move's type against a defender's types
  under other-side relations, PP against its owner). Pinning the diagonal to 1 by construction makes every
  relation start as plain attention and keeps it anchored there. Cost: 28 matrices of 32 x 32 applied to
  every query, a few percent of the FFN. Values are plain `x_j . W_v`; softmax over all 126 keys.
  Ablations: a diagonal-only `g` (cheaper, weaker), and a fixed token layout with rotary embeddings and no
  relation table at all (unary category transforms only; needs a block stride of at least 9 so that owner
  and sibling offsets do not collide, and permutation augmentation instead of structural invariance).
- `P`: linear 512 -> 512 on every token of r^N; `pool`: mean over the 63 tokens concatenated with max over
  them (1024); `m`: MLP 1024 -> 1024 -> 512 on just the pooled vector. That is h(A,B) = h_A.
- `g`: MLP 1024 -> 1024 -> 512 (GELU), no reduction to a scalar; `w`: a single linear 512 -> 1.

Symmetry is structural. Swapping A and B swaps r_A^0 and r_B^0; F_n is applied identically with swapped
arguments at every layer, so by induction the final streams swap, h_A and h_B swap, and the two g terms
trade places: V(B) = -V(A) for every input and parameter setting, whatever h computes (h_A and h_B need not
be related at all). The win head is a monotone squash of V. A unit test asserts V(s,A) == -V(s,B) to float precision. The two streams compute genuinely
different things (different labels, different pooled side), so borrowing the twin's keys adds capacity
rather than repeating work. An independent-passes variant (no borrowed keys) is kept as an ablation.

Not for now: a single-pass equivariant variant (side membership only as a relative attention bias) as a
possible inference optimization later.

Both permutation invariances are structural: bench order through the categories, move order through the
summed per-move vectors.

### Hyperparameters (first run)

| | |
|---|---|
| d_model / heads / head dim | 512 / 8 / 64 (32 content + 32 structural per head) |
| layers N | 4 |
| FFN | 2048, GELU |
| norm | pre-LN, final LN before P |
| dropout | 0.0 (self-play data is unlimited); 0.1 only for a small supervised warm start |
| embeddings | move 16, effect 16, ability 16, item 16, hold effect 8; type/target/flags as one-hots |
| parameters | ~15M (encoder 12.6M, embeddings ~0.4M, input MLPs ~1.5M, head ~1M); relation tables are tiny |
| optimizer | AdamW, lr 3e-4, betas (0.9, 0.95), weight decay 0.01, 1k warmup steps, cosine to 3e-5 |
| batch | 1024 states (both orderings of every state are one sample: the model needs both streams anyway) |
| grad clip | 1.0 |
| precision | bf16 autocast on MPS/CUDA, fp32 master weights |
| loss | BCE(p, t), t = outcome minus lambda x sum of Bellman-residual control variates; no symmetry term |
| augmentation | none: bench order (categories) and move order (summed per-move vectors) are structurally irrelevant |
| compute per state | 2 streams x 63 tokens x 4 layers at d = 512: ~0.45 GFLOP; a 1,300-state batch ~0.6 TFLOP, ~50-100 ms on an M-series GPU. Arena evaluations of the network agent take hours, not minutes, unless several games are batched through the network together (the agent server can) |

## 3. Self-play training

The network is a win-probability function used by a one-turn equilibrium planner; nothing is differentiated
through the planner.

**Acting.** At a decision, the C library simulates every (my action, their action) pair from the turn-start
state for k RNG samples (k = 2..4 in self-play, 16 in evaluation). Many games run in lockstep: their
n_i x m_i x k states (different counts per game: legal actions differ) are flattened into one batch with an
index table, evaluated in one forward pass, and scattered back into each game's matrix M_ij = 2 p_ij - 1.
RM+ (alternating, linear averaging, 100 iterations) solves M; the action is *sampled* from the average
strategy (never the argmax), with a small epsilon of random legal actions early in training.

**Targets.** Write `p` for the EWMA network's win probability (a lagging copy of the weights, see below).
At every decision t of a game the play-time solve gives the backup `b(s_t) = sum_ij sigma_i sigma_j p(s'_ij)`
under the mixed strategies actually played by both sides, and the game then reaches some next decision state
s_{t+1}. The residual `c_t = p(s_{t+1}) - b(s_t)` has conditional mean exactly zero given s_t (the matrix
samples estimate E[p(s_{t+1}) | s_t] without bias and are independent of the continuation drawn), so with
y in {0, 1/2, 1} the final result,

```
t(s_t) = y - lambda * sum_{u >= t} c_u
```

is an unbiased estimate of the true win probability of s_t for every lambda, and at lambda = 1 it telescopes
to `b(s_t) + sum_{u > t} [b(s_u) - p(s_u)]`: the low-variance backup plus the network's own Bellman residuals
along the rest of the game, which vanish as the network becomes self-consistent. This is the control-variate
(doubly robust) estimator: y alone is unbiased and noisy, b alone is low-variance and biased, y - sum c is
both. lambda is the control-variate coefficient: chosen per epoch as Cov(y, sum c) / Var(sum c) on the
buffer (near 0 for a random network, rising to 1), since with a poor network the residuals only add noise.

```
loss = BCE(p(s), t(s))       gradient in the logit is p - t, linear in t, so the target may leave [0, 1]
```

Terminal states are exact. Both perspectives of a state are one sample (the head is antisymmetric). The
residuals cost nothing extra: b is computed at play time, each decision stores c_t, and the targets are
formed when the game ends.

**Data.** A replay buffer of the last ~500k decision states, shuffled (states within a game are highly
correlated and share an outcome). Teams are sampled from the whole corpus (all ten datasets, later the
mutated/crossed-over set), paired within a rating band (<= 150 points apart by the corpus ratings) so the
agents, not the teams, decide the game; mirror matches (both sides the same team) are an extra option.

**Opponents.** A league: the opponent of a self-play game is sampled from the current EWMA network and the
saved checkpoints (recent ones weighted higher), never only the current self, to avoid self-referential
strategies and cycling.

**Warm start.** Before self-play, ~300k games between the heuristic agents (`rmplus:iters=100,samples=16`
and the rest of the pool) generate outcome-labelled states; the network is trained on those first (held-out
log-loss as the gate), so the first self-play network already plays at the heuristic level.

**Hidden timers as hazards.** Sleep and the other random-duration effects are re-implemented in the
simulator (behind a flag, off for the ROM cross-check) as per-attempt wake-up hazards: the state stores
turns elapsed only, and each attempt draws u in [0, 1] and ends the effect when u < hazard(elapsed), the
hazard being the game's duration distribution inverted (sleep 2..5 uniform gives 0, 1/4, 1/3, 1/2, 1 on
attempts 1..5). Same marginal distribution, but the state is Markov in observable quantities and the k
samples of a matrix cell resample the wake-up independently instead of all sharing the hidden remaining count.

**Checkpoints and evaluation.** An EWMA (Polyak average) of the weights is maintained throughout; it is the
evaluated model, the bootstrap target network and the candidate at every save point. Saves happen every
3.3% of the run (about 30 in total), storing both EWMA and raw weights (so a different averaging window can be
reconstructed later); the best so far by evaluation is kept as "current best". Evaluation: a 2,000-game arena
run at k = 16 against `rmplus:iters=100,samples=16` and against previous checkpoints, Bradley-Terry with
standard errors, on band-paired corpus teams; plus a fixed held-out set of states for log-loss and
calibration. A checkpoint is saved as a result when it beats the heuristic outside the +-15 band.

## 4. How it plugs into the simulator

The network stays in PyTorch; the simulator is exposed to Python, and agents talk to the C side over a
small network protocol. No model-format export is needed.

- **Simulator as a shared library.** `libfrlgsim` with a flat C API (`include/sim_capi.h`), called from
  Python through ctypes (`py/frlgsim`). A battle state is an opaque 14 KB blob the caller owns; the API
  covers setup (teams, trainer, RNG, policies), play (run / request / answer / legal actions), batched
  rollouts (`sim_joint_states`: every (my action, their action) pair from a turn-start state for k RNG
  seeds, returning the resulting *states* so Python evaluates them with the network in one batch and runs
  RM+ in numpy), the exporter's accessors (party mon, battler, side, field, move/item/species tables) and
  the hand-tuned value functions for comparison.
- **Agent server protocol.** Newline-delimited text with base64 payloads, trivially implementable in any
  language:
  ```
  client -> server:  DECIDE <battler> <kind>\n STATE <base64 state>\n TURNSTART <base64 state | ->\n END\n
  server -> client:  ACT <type> <moveSlot> <target> <partySlot> <item>\n
  ```
  `kind` is 1 (turn action) or 2 (forced replacement); the turn-start state is the snapshot before either
  side committed (what the joint-action agents need) or `-` when there is none.
- **`net:host=127.0.0.1,port=8950`** is an agent spec like any other: `play --agent net:...` and the arena
  connect per game, so a Python agent plays humans through the mGBA harness and gets an ELO in the same
  tournaments as the C agents.

Training data comes from the same pieces: Python drives self-play through the library, records each
decision's state blob, side and final result, and the exporter's accessors turn blobs into tokens.

## 5. Sizes and a first budget

| item | value |
|---|---|
| parameters | ~3.5M (embeddings ~0.5M) |
| inputs per token | ~330 floats + 7 categorical ids |
| tokens per state | 15 (12 Pokémon, 2 sides, 1 field) |
| batch of 1,300 states | ~4 GFLOP, ~5 ms GPU / ~30 ms CPU |
| training set to start | 2M states (about 30k self-play games at ~70 decisions), a few hundred MB |
| first success criterion | a Python RM+ agent (16 samples, 100 iterations) with the network as its value rates above the C `rmplus:iters=100,samples=16` in a 4,000-game arena run, outside the ±15 error band |

## 6. Custom Pokémon in the simulator

The game's tables (`gSpeciesInfo`, learnsets, dex height/weight) are `const` ROM data. To battle with
novel mons the simulator gets a mutable species table: `Sim_RegisterSpecies(id, base stats, types,
abilities, weight, height, gender ratio, ...)` writes into a copy that the engine reads (a one-line change
at the table's definition), using the 25 unused OLD_UNOWN slots (252..276) first and, if more are needed,
ids above 411 with the tables widened. Learnset legality is irrelevant for custom mons (they are built
directly, not checked), the exporter reads the same table, and the network sees only attributes, so
nothing else changes. Engine code with hard-coded species checks (Transform on Ditto, Castform/Deoxys
form handling, Shedinja's 1 HP is a base stat) keeps applying to the original ids only, which is the
intended behaviour.

## 7. Open questions for the review

1. (settled) Both teams are fully known, as if from the guide; the only hidden state is the random-duration timers (sleep above all), exported as elapsed turns so the state stays Markov.
2. (settled) Value only; no policy head.
3. (settled) d = 512, 4 layers to start; depth is the remaining knob.
4. (settled) Singles only.
