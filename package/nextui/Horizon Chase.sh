#!/bin/bash
# TrimUI Brick / NextUI entry point for Horizon Chase.
set -u

# NextUI's Ports collection is mounted here on the TrimUI Brick. Advanced
# users may override either path before invoking this script by hand.
NEXTUI_PORTS_ROOT=${HC_NEXTUI_PORTS_ROOT:-"/mnt/SDCARD/Roms/Ports (PORTS)"}
GAMEDIR=${HC_GAMEDIR:-"$NEXTUI_PORTS_ROOT/.ports/horizonchase"}

if [ ! -d "$GAMEDIR" ]; then
  printf 'Horizon Chase: game directory is missing: %s\n' "$GAMEDIR" >&2
  exit 1
fi

export HC_GAMEDIR="$GAMEDIR"
export HC_CONTROLFOLDER="${HC_CONTROLFOLDER:-/mnt/SDCARD/Emus/tg5040/PORTS.pak/PortMaster}"

# NextUI calls its SDL backend "mali", but the Brick actually presents through
# a PowerVR Rogue GE8300. SDL must retain ownership of the EGL contexts.
export HC_PURE_SDL_CONTEXTS=1

# This raises Unity's Android lifecycle cadence; it does not alter timeScale or
# double the simulation speed. Set HC_FRAME_LIMIT=30 before launch to fall back.
export HC_FRAME_LIMIT=${HC_FRAME_LIMIT:-60}

# Manual Android-remote mode. The Brick loader maps its forward action to the
# bottom Xbox-layout face button while D-pad Down remains brake.
export HC_FORCE_INPUT_TYPE=${HC_FORCE_INPUT_TYPE:-11}

# These old experiments disable the road-alpha correction if inherited.
unset HC_NO_OPAQUE_BACKBUFFER
unset HC_GLES_MAJOR

# Trace output was useful while calibrating the road fix but is not needed in
# normal play and would add periodic diagnostic logging.
unset HC_GLSTATE_TRACE

exec bash "$GAMEDIR/launcher-common.sh"
