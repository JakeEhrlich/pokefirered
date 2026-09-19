#!/bin/bash
# Build and run a single scenario group in isolation (ignores other groups' files, e.g. while they are
# being edited): tools/run_group.sh <group> [scenario-filter]
set -e
cd "$(dirname "$0")/.."
G="$1"; shift
OUT="build/group_$G"
mkdir -p "$OUT"
make BUILD="$OUT" "$OUT/libsim.a" > /dev/null
CFLAGS="-O2 -g -std=gnu11 -DFIRERED -DREVISION=0 -DENGLISH -DMODERN=1 -DSIM_BUILD=1 -I include -I gen -I tests -I ../include -I ../src -w"
cc $CFLAGS -c "tests/scenarios/$G.c" -o "$OUT/$G.o"
cc $CFLAGS -c tests/scenario.c -o "$OUT/scenario.o"
printf '#include "scenario.h"\nextern const struct ScenarioGroup gScenarioGroup_%s;\nconst struct ScenarioGroup *const gScenarioGroups[] = { &gScenarioGroup_%s };\nconst int gScenarioGroupCount = 1;\n' "$G" "$G" > "$OUT/list.c"
cc $CFLAGS -c "$OUT/list.c" -o "$OUT/list.o"
cc $CFLAGS tests/scenarios_main.c "$OUT/$G.o" "$OUT/scenario.o" "$OUT/list.o" "$OUT/libsim.a" -o "$OUT/scenarios"
exec "$OUT/scenarios" "$@"
