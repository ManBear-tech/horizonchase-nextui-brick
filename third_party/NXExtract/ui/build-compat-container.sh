#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

CC=aarch64-linux-gnu-gcc
STRIP=aarch64-linux-gnu-strip
OUTPUT=/src/build/nxextract-ui

if ! command -v "$CC" >/dev/null 2>&1; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq \
    gcc-aarch64-linux-gnu \
    binutils-aarch64-linux-gnu \
    >/dev/null
fi

mkdir -p /src/build

"$CC" \
  -std=gnu11 \
  -D_GNU_SOURCE \
  -O2 \
  -fPIE \
  -pie \
  -Wall \
  -Wextra \
  -Wformat=2 \
  -Wshadow \
  -Wstrict-prototypes \
  -Wl,--as-needed \
  -Wl,-z,relro,-z,now \
  -o "$OUTPUT" \
  /src/nxextract_ui.c \
  -ldl

"$STRIP" --strip-unneeded "$OUTPUT"
chmod 755 "$OUTPUT"

case "${NXEXTRACT_HOST_UID:-}:${NXEXTRACT_HOST_GID:-}" in
  *[!0-9:]*|:|*:|*:*:*) ;;
  *) chown "$NXEXTRACT_HOST_UID:$NXEXTRACT_HOST_GID" "$OUTPUT" ;;
esac
