#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/horizon-nextui-launcher.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT INT TERM

fail() {
  printf 'nextui_launcher_test: %s\n' "$*" >&2
  exit 1
}

GAME="$TEST_ROOT/.ports/horizonchase"
mkdir -p "$GAME"
cat > "$GAME/launcher-common.sh" <<'COMMON'
#!/bin/bash
{
  printf 'gamedir=%s\n' "${HC_GAMEDIR:-}"
  printf 'control=%s\n' "${HC_CONTROLFOLDER:-}"
  printf 'sdl=%s\n' "${HC_PURE_SDL_CONTEXTS:-}"
  printf 'fps=%s\n' "${HC_FRAME_LIMIT:-}"
  printf 'input=%s\n' "${HC_FORCE_INPUT_TYPE:-}"
  printf 'opaque=%s\n' "${HC_NO_OPAQUE_BACKBUFFER-unset}"
  printf 'gles=%s\n' "${HC_GLES_MAJOR-unset}"
  printf 'trace=%s\n' "${HC_GLSTATE_TRACE-unset}"
} > "$HC_GAMEDIR/launcher.env"
COMMON
chmod 0755 "$GAME/launcher-common.sh"

env \
  HC_NEXTUI_PORTS_ROOT="$TEST_ROOT" \
  HC_CONTROLFOLDER="$TEST_ROOT/PortMaster" \
  HC_NO_OPAQUE_BACKBUFFER=1 \
  HC_GLES_MAJOR=2 \
  HC_GLSTATE_TRACE=1 \
  bash "$PROJECT_ROOT/package/nextui/Horizon Chase.sh"

ENV_LOG="$GAME/launcher.env"
grep -Fqx "gamedir=$GAME" "$ENV_LOG" || fail "wrong game directory"
grep -Fqx "control=$TEST_ROOT/PortMaster" "$ENV_LOG" || fail "wrong control path"
grep -Fqx 'sdl=1' "$ENV_LOG" || fail "SDL context override missing"
grep -Fqx 'fps=60' "$ENV_LOG" || fail "60 FPS default missing"
grep -Fqx 'input=11' "$ENV_LOG" || fail "manual input default missing"
grep -Fqx 'opaque=unset' "$ENV_LOG" || fail "opaque-disable experiment survived"
grep -Fqx 'gles=unset' "$ENV_LOG" || fail "GLES experiment survived"
grep -Fqx 'trace=unset' "$ENV_LOG" || fail "GL-state trace survived"

printf 'nextui_launcher_test: OK\n'
