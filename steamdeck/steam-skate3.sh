#!/usr/bin/env bash
# Steam Deck launcher for Skate 3 Recomp.
#
# Put this file next to the skate3 executable (together with skate3.toml from
# this folder), then add it to Steam as a non-Steam game. Works in Game Mode
# and Desktop Mode. Extra arguments are passed to the game, for example
# "%command% --fullscreen=false" in Steam's launch options.
set -uo pipefail
DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
GAME="$DIR/skate3"
if [ ! -x "$GAME" ]; then
  echo "steam-skate3: no skate3 executable next to this script ($DIR)" >&2
  exit 1
fi

# Steam starts non-Steam games with its own runtime's LD_LIBRARY_PATH. The game
# uses the system's GTK, Vulkan and udev and finds librexruntime.so next to
# itself, so use the system libraries only.
unset LD_LIBRARY_PATH

# One copy at a time. A copy left running (for example across a sleep/resume
# where Steam lost track of it) shares the controller and settings with a new
# one: both react to every button press, both restart on a settings change, and
# they fight over the screen. SIGKILL because the game ignores SIGTERM.
for pid in $(pgrep -x skate3); do
  if [ "$(readlink -f /proc/"$pid"/exe 2>/dev/null)" = "$GAME" ]; then
    echo "steam-skate3: stopping a copy of the game that was left running (pid $pid)" >&2
    kill -9 "$pid" 2>/dev/null
  fi
done
for _ in $(seq 1 30); do
  pgrep -x skate3 > /dev/null || break
  sleep 0.1
done

cd "$DIR" || exit 1
# Desktop Mode suspends on idle even while a game is running, and the game's
# GPU context does not survive the resume (frozen picture, then a crash).
# Block idle sleep while the game runs; harmless in Game Mode.
if systemd-inhibit --what=idle --mode=block --why=probe true 2> /dev/null; then
  exec systemd-inhibit --what=idle --mode=block --why="Skate 3 is running" "$GAME" "$@"
fi
exec "$GAME" "$@"
