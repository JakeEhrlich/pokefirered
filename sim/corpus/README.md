# Recorded ROM transcripts

Each `*.trace` (or `*.trace.gz`) is a transcript recorded by `build/crosscheck` with `CROSSCHECK_RECORD` while
playing against the real game in mGBA: per battle a `# label`, `CFG`, the `START` line (flags, trainer, seed,
both parties), every `ACT` the driver sent and every `REQ`/`DONE` block with the ROM's state snapshot.
`make backtest` (tools/backtest.sh) replays all of them against the simulator alone and diffs; it takes seconds.
Files: `scenarios_<group>.trace` = the declarative scenario groups (tests/scenarios), `random_seed<n>.trace` =
60 random trainer battles per seed (CROSSCHECK_SEED=n, random player parties, the game's own AI for the trainer).
The corpus was recorded on 2026-09-19 with the harness ROM built from this tree.
