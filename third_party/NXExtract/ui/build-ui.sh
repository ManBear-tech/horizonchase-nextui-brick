#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
PROJECT_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd -P)"
CONTAINER_ENGINE="${NXEXTRACT_CONTAINER_ENGINE:-docker}"
BUILD_IMAGE="${NXEXTRACT_BUILD_IMAGE:-debian:bullseye}"
OUTPUT="$SCRIPT_DIR/build/nxextract-ui"

command -v "$CONTAINER_ENGINE" >/dev/null 2>&1 || {
  echo "build: container engine not found: $CONTAINER_ENGINE" >&2
  exit 1
}

mkdir -p "$SCRIPT_DIR/build"

"$CONTAINER_ENGINE" run --rm \
  -e NXEXTRACT_HOST_UID="$(id -u)" \
  -e NXEXTRACT_HOST_GID="$(id -g)" \
  -v "$SCRIPT_DIR:/src" \
  -w /src \
  "$BUILD_IMAGE" \
  bash /src/build-compat-container.sh

"$PROJECT_ROOT/tools/check-glibc.sh" "$OUTPUT"

echo "BUILD OK: $OUTPUT"
file "$OUTPUT"
