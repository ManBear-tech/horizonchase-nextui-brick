#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

export LC_ALL=C
MAX_MAJOR=2
MAX_MINOR=30

if [ "$#" -eq 0 ]; then
  echo "usage: check-glibc.sh AARCH64_ELF [...]" >&2
  exit 2
fi

if command -v aarch64-linux-gnu-readelf >/dev/null 2>&1; then
  READELF=aarch64-linux-gnu-readelf
else
  READELF=readelf
fi

failed=0
for binary in "$@"; do
  if [ ! -f "$binary" ]; then
    echo "GLIBC GATE FAILED: missing file: $binary" >&2
    failed=1
    continue
  fi
  if ! header="$("$READELF" -h "$binary" 2>/dev/null)"; then
    echo "GLIBC GATE FAILED: not an ELF file: $binary" >&2
    failed=1
    continue
  fi
  if ! grep -qE 'Class:[[:space:]]+ELF64' <<<"$header" ||
     ! grep -qE 'Machine:[[:space:]]+AArch64' <<<"$header"; then
    echo "GLIBC GATE FAILED: expected ELF64 AArch64: $binary" >&2
    failed=1
    continue
  fi

  versions="$(
    "$READELF" --version-info "$binary" 2>/dev/null |
      grep -oE 'GLIBC_[0-9]+\.[0-9]+' |
      sed 's/GLIBC_//' |
      sort -Vu || true
  )"
  maximum="$(printf '%s\n' "$versions" | sed '/^$/d' | tail -n 1)"
  if [ -z "$maximum" ]; then
    echo "GLIBC GATE OK: $binary has no dynamic GLIBC requirement"
    continue
  fi

  major="${maximum%%.*}"
  minor="${maximum#*.}"
  minor="${minor%%.*}"
  if [ "$major" -gt "$MAX_MAJOR" ] ||
     { [ "$major" -eq "$MAX_MAJOR" ] && [ "$minor" -gt "$MAX_MINOR" ]; }; then
    echo "GLIBC GATE FAILED: $binary needs GLIBC_$maximum (> 2.30)" >&2
    failed=1
  else
    echo "GLIBC GATE OK: $binary needs GLIBC_$maximum (<= 2.30)"
  fi
done

exit "$failed"
