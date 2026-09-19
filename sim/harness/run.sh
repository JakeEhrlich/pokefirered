#!/bin/bash
# Relaunch mGBA with the harness ROM, wait for the Lua bridge to listen, then run the cross-check.
#   harness/run.sh <battles> [trainerId]
# Loading the script inside mGBA is automated when this terminal has Accessibility access
# (System Settings > Privacy & Security > Accessibility); otherwise load it by hand when prompted.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ROM="$ROOT/pokefirered_modern_harness.gba"
SCRIPT="$ROOT/sim/harness/harness_gen.lua"
PORT=8899

if lsof -nP -iTCP:$PORT -sTCP:LISTEN >/dev/null 2>&1 && [ "${RELAUNCH:-0}" != 1 ]; then
  echo "bridge already listening; reusing the running mGBA (RELAUNCH=1 to force a restart)"
  exec "$ROOT/sim/build/crosscheck" "${1:-5}" "${2:-0}"
fi

pkill -x mGBA 2>/dev/null || true
sleep 1
open -a mGBA --args -C fpsTarget=1000 "$ROM"
sleep 2

# Real UI-element access is needed (a plain process query succeeds without Accessibility).
if osascript -e 'tell application "System Events" to tell process "mGBA" to get name of menu bar 1' >/dev/null 2>&1; then
  osascript <<APPLESCRIPT >/dev/null 2>&1 || true
tell application "mGBA" to activate
delay 1
tell application "System Events"
  tell process "mGBA"
    click menu item "Scripting…" of menu "Tools" of menu bar 1
    delay 1
    keystroke "o" using {command down}
    delay 1
    keystroke "g" using {command down, shift down}
    delay 0.5
    keystroke "$SCRIPT"
    keystroke return
    delay 0.5
    keystroke return
  end tell
end tell
APPLESCRIPT
else
  echo "No Accessibility access: in mGBA use Tools > Scripting..., then File > Load script... and pick"
  echo "  $SCRIPT"
fi

echo -n "waiting for the harness bridge on port $PORT "
for i in $(seq 1 600); do
  if lsof -nP -iTCP:$PORT -sTCP:LISTEN >/dev/null 2>&1; then echo; echo "bridge is listening"; break; fi
  sleep 1; echo -n .
done
lsof -nP -iTCP:$PORT -sTCP:LISTEN >/dev/null 2>&1 || { echo; echo "bridge never appeared"; exit 1; }
exec "$ROOT/sim/build/crosscheck" "${1:-5}" "${2:-0}"
