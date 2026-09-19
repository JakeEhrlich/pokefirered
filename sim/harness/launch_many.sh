#!/bin/bash
# Open N additional mGBA windows with the harness ROM (each needs harness_gen.lua loaded by hand).
N="${1:-2}"
ROM="$(cd "$(dirname "$0")/../.." && pwd)/pokefirered_modern_harness.gba"
for i in $(seq 1 "$N"); do
  open -n -a mGBA --args -C fpsTarget=1000 "$ROM"
  sleep 2
done
