#!/bin/bash
# Shard the scenario groups across several mGBA bridges (ports 8899, 8900, ...) and run them in parallel.
# CROSSCHECK_RECORD_DIR=<dir> also records a transcript per group (<dir>/scenarios_<group>.trace).
#   harness/crosscheck_parallel.sh <nports> [group ...]
# Each bridge must already be listening (one mGBA window per port with harness_gen.lua loaded; see
# harness/launch_many.sh). Logs: /tmp/cc_<group>.log, summary lines in /tmp/cc_parallel.log.
cd "$(dirname "$0")/.."
N="$1"; shift
SC_GROUPS="$*"
if [ -z "$SC_GROUPS" ]; then
  SC_GROUPS=$(grep -ho "SCENARIO_GROUP([a-z_0-9]*" tests/scenarios/*.c | sed 's/SCENARIO_GROUP(//' | grep -v species_sweep)
fi
i=0
for g in $SC_GROUPS; do
  port=$((8899 + i % N))
  eval "SHARD_$((i % N))=\"\$SHARD_$((i % N)) $g\""
  i=$((i + 1))
done
: > /tmp/cc_parallel.log
for s in $(seq 0 $((N - 1))); do
  eval "groups=\$SHARD_$s"
  port=$((8899 + s))
  ( for g in $groups; do
      echo "=== $g on port $port $(date +%H:%M)" >> /tmp/cc_parallel.log
      rec=""; [ -n "$CROSSCHECK_RECORD_DIR" ] && rec="$CROSSCHECK_RECORD_DIR/scenarios_$g.trace" && : > "$rec"
      CROSSCHECK_RECORD="$rec" CROSSCHECK_PORT=$port timeout 20000 ./build/crosscheck --scenarios "$g" > "/tmp/cc_$g.log" 2>&1
      echo "exit=$?" >> "/tmp/cc_$g.log"
      echo "$g: $(tail -2 /tmp/cc_$g.log | head -1)" >> /tmp/cc_parallel.log
    done ) &
done
wait
echo "all shards done" >> /tmp/cc_parallel.log
