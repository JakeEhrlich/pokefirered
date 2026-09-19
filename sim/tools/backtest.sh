#!/bin/bash
# Replay every recorded ROM transcript in corpus/ (or the dirs/files given) against the simulator alone.
# Exit status is non-zero if any battle diverges. Takes seconds; no mGBA needed.
cd "$(dirname "$0")/.."
files=("$@"); [ ${#files[@]} -eq 0 ] && files=(corpus/*.trace corpus/*.trace.gz)
fail=0; total=0
for f in "${files[@]}"; do
  [ -e "$f" ] || continue
  case "$f" in
    *.gz) out=$(gunzip -c "$f" | ./build/crosscheck --replay /dev/stdin 2>&1 | tail -1) ;;
    *)    out=$(./build/crosscheck --replay "$f" 2>&1 | tail -1) ;;
  esac
  echo "$out"
  n=$(echo "$out" | sed -n 's/.*: \([0-9]*\) battles.*/\1/p'); m=$(echo "$out" | sed -n 's/.*, \([0-9]*\) with mismatches.*/\1/p')
  total=$((total + ${n:-0})); fail=$((fail + ${m:-0}))
done
echo "backtest: $total battles, $fail with mismatches"
[ "$fail" -eq 0 ]
