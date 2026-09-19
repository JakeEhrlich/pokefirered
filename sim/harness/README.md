# Cross-checking the simulator against the real game

`make firered_harness` (from the repo root, with `DEVKITARM=/opt/devkitpro/devkitARM`) builds
`pokefirered_modern_harness.gba`: the retail game plus a small harness (`src/sim_harness.c`, guarded by
`SIM_HARNESS`) that

- boots straight into a trainer battle described by an EWRAM mailbox (`gSimHarness`),
- takes the player's decisions from that mailbox instead of the menus,
- gives the battle engine its own xorshift32 RNG stream (only callers inside the engine objects; frame and
  animation `Random()` calls keep using the game's LCG) seeded from the mailbox, so the simulator, running the
  same xorshift from the same seed, sees exactly the same rolls, and
- returns to the mailbox loop when the battle ends.

`harness_gen.lua` (generated from `harness.lua` plus the addresses and engine ranges from the linker map)
runs inside mGBA and bridges the mailbox to a TCP socket; it also restores an idle save state before each
battle and taps A so text never waits. `build/crosscheck` on the host connects to it, drives both the ROM and
the simulator with the same parties, the same trainer, the same decisions and the same RNG seed, and diffs
the full battle state (battle mons, statuses, side timers, disable structs, weather, wish/future sight, both
parties, party indexes, outcome) plus the engine RNG call count at every decision point and at the end.
On a mismatch it prints the first differing field and the engine `Random()` call sites of both sides since
the previous decision.

## Running it

1. Build the ROM and the tables (repo root):
   ```
   DEVKITARM=/opt/devkitpro/devkitARM make firered_harness -j8
   python3 sim/tools/gen_harness_addrs.py pokefirered_modern_harness.map sim/harness/harness.lua sim/harness/harness_gen.lua sim/gen/harness_addrs.h
   cd sim && make
   ```
2. Open `pokefirered_modern_harness.gba` in mGBA, then Tools → Scripting… → File → Load script… and pick
   `sim/harness/harness_gen.lua`. The scripting console should say `battle-sim harness listening on port 8899`.
   Turn on fast-forward (Emulation → Fast forward, or hold Tab) to speed things up; battles run with
   animations off.
3. Run the host driver: `sim/build/crosscheck <battles> [trainerId]` (trainer 0 = random trainers).

`harness/run.sh <battles> [trainerId]` does steps 2-3 (relaunching mGBA, loading the script through
System Events if the terminal has Accessibility access, waiting for the bridge) in one go.

Each battle prints `ok` with the number of decisions, or the first mismatching field with the
simulator's message log.

## Scenarios, several windows at once, replays

- `build/crosscheck --scenarios [substring]` plays the declarative scenarios (`tests/scenarios/*.c`) through
  the ROM and the simulator side by side; the filter matches group or scenario names.
- Every window is one bridge. `harness/launch_many.sh N` opens N extra mGBA windows on the harness ROM (each
  still needs `harness_gen.lua` loaded by hand; the script binds the first free port from 8899 up).
  `CROSSCHECK_PORT=<port>` points a driver at a given window; `harness/crosscheck_parallel.sh N group...`
  shards scenario groups round-robin over ports 8899..8899+N-1 (logs in `/tmp/cc_<group>.log`, summary in
  `/tmp/cc_parallel.log`).
- Random trainer batches are reproducible: `CROSSCHECK_SEED=<n>` picks the trainer/party/RNG sequence and
  `CROSSCHECK_FROM=<k>` skips straight to battle k of that sequence (`CROSSCHECK_SEED=2 CROSSCHECK_FROM=15
  build/crosscheck 16` replays exactly battle 15 of seed 2).

## Timing details worth knowing

- The bridge does not snapshot the instant a decision request appears: the other battlers' controllers (the
  AI, and in doubles the partner's AI several frames later) may still be finishing. It waits until the request
  has been pending for 4 frames and the engine state (battle mons, parties, disable structs, battle
  communication, controller exec flags, engine RNG count) has been unchanged for 3 frames, which mirrors the
  simulator stepping to quiescence before it reports a request.
- In the harness ROM, selection-time messages ("has no moves left!", "can't escape") complete immediately
  instead of waiting for A, again to match the simulator's decision order in doubles.
- Do not run `make firered_harness` while mGBA windows have the ROM open: mGBA maps the file, so the rewrite
  corrupts the running games (white or glitched screens). Quit the windows, rebuild, regenerate
  `harness_gen.lua`, delete `/tmp/simharness_idle_*.ss`, relaunch, reload the script.
- Killing `build/crosscheck` mid-battle leaves that window mid-battle; the next battle restores the idle save
  state, so this is harmless. Killing mGBA itself, of course, is not.

## Recording a corpus and replaying it without mGBA

`CROSSCHECK_RECORD=<file>` makes the driver append a transcript of every battle it plays: a `# label` and
`CFG` header, the `START` line (flags, trainer, seed, both parties), every `ACT` it sent and every
`REQ`/`DONE` block with the ROM's snapshot as received. `build/crosscheck --replay <file> [filter]` then
replays those battles in the simulator alone and diffs against the recorded ROM state with the same
comparison, in seconds. `harness/crosscheck_parallel.sh` records one file per group when
`CROSSCHECK_RECORD_DIR` is set. The corpus from the full run lives in `sim/corpus/` (large; keep it
gzipped if it goes into git). Use it to backtest simulator changes before touching the ROM again:
```
for f in corpus/*.trace; do build/crosscheck --replay $f | tail -1; done
```
