#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
GAME_DIR="${NXEXTRACT_GAME_DIR:-$SCRIPT_DIR}"
RECIPE="${NXEXTRACT_RECIPE:-$GAME_DIR/extractor.json}"
PYTHON_BIN="${NXEXTRACT_PYTHON:-python3}"

exec "$PYTHON_BIN" "$SCRIPT_DIR/nxextract.py" install \
  --recipe "$RECIPE" \
  --game-dir "$GAME_DIR" \
  "$@"
