#!/bin/bash
# Cross-check every scenario group against the ROM, one group at a time, logging to /tmp/cc_<group>.log.
#   harness/crosscheck_all.sh [group ...]    (default: all groups except species_sweep)
cd "$(dirname "$0")/.."
SC_GROUPS="$*"
if [ -z "$SC_GROUPS" ]; then
  SC_GROUPS=$(grep -ho "SCENARIO_GROUP([a-z_0-9]*" tests/scenarios/*.c | sed 's/SCENARIO_GROUP(//' | grep -v species_sweep)
fi
for g in $SC_GROUPS; do
  echo "=== $g $(date +%H:%M)"
  timeout 20000 ./build/crosscheck --scenarios "$g" > "/tmp/cc_$g.log" 2>&1
  echo "exit=$?" >> "/tmp/cc_$g.log"
  tail -1 "/tmp/cc_$g.log"
  grep -c "  ok:" "/tmp/cc_$g.log"
done
