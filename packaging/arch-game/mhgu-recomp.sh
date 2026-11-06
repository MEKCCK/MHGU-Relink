#!/bin/sh
# Runs the recompiled game. Pass your game folder (exefs/ + romfs/, 1.4.0 merged) or set MHGU_GAME_DIR.
# Caches, logs and saves live in $XDG_DATA_HOME/MHGURecomp (the game writes them in its working directory).
: "${XDG_DATA_HOME:=$HOME/.local/share}"
STATE="$XDG_DATA_HOME/MHGURecomp"
mkdir -p "$STATE"
if [ -e /usr/lib/mhgu-recomp/pipelines.list ] && [ ! -e "$STATE/pipelines.list" ]; then
  ln -sf /usr/lib/mhgu-recomp/pipelines.list "$STATE/pipelines.list"
fi
[ -e "$STATE/mhgurecomp.ini" ] || cp -f /usr/lib/mhgu-recomp/mhgurecomp.ini "$STATE/mhgurecomp.ini"
# A folder argument, MHGU_GAME_DIR, or the usual places (a desktop launch passes no argument; without a
# folder the game looks for data/ next to its own binary and stops with "game files were not found").
GAME_DIR="${1:-${MHGU_GAME_DIR:-}}"
if [ -z "$GAME_DIR" ]; then
  for d in "$HOME/game/MHGU-1.4" "$HOME/game/MHGU" "$HOME/MHGU-1.4" "$HOME/Games/MHGU-1.4" /opt/MHGU-1.4; do
    if [ -d "$d/exefs" ] && [ -d "$d/romfs" ]; then GAME_DIR="$d"; break; fi
  done
fi
if [ -z "$GAME_DIR" ]; then
  echo "mhgu-recomp: no game folder given and none of the usual places has exefs/ and romfs/." >&2
  echo "  Usage: mhgu-recomp /path/to/MHGU-1.4      (or set MHGU_GAME_DIR)" >&2
  echo "  It needs your own dump of the game, with the 1.4.0 update merged into romfs/." >&2
  exit 1
fi
if [ -z "${1:-}" ]; then set -- "$GAME_DIR"; fi
if [ -n "$GAME_DIR" ] && [ -z "${MHGU_GAME_LIB:-}" ]; then
  for lib in "$GAME_DIR/libmhgugame.so" "$GAME_DIR/mhgugame.so"; do
    if [ -e "$lib" ]; then export MHGU_GAME_LIB="$lib"; break; fi
  done
  if [ -z "${MHGU_GAME_LIB:-}" ]; then
    echo "mhgu-recomp: no game library in $GAME_DIR" >&2
    echo "  The translated game code (libmhgugame.so) is built from your own dump and is not" >&2
    echo "  part of this package: run the recompiler for your dump, or put your build there," >&2
    echo "  or set MHGU_GAME_LIB to it." >&2
    exit 1
  fi
fi
cd "$STATE" || exit 1
exec /usr/lib/mhgu-recomp/mhgurecomp "$@"
