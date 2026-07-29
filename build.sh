#!/bin/bash
# Build de desenvolvimento/validação no sysroot corrente do NextOS Amlogic-old.
# Para a variante portátil R36S com GLIBC <= 2.30, use build_r36s.sh.
set -euo pipefail

export TMPDIR=${TMPDIR:-/tmp}
NEXTOS_ROOT=${NEXTOS_ROOT:-"$HOME/NextOS-Elite-Edition"}
TC=${NEXTOS_TOOLCHAIN:-$(
  find -H "$NEXTOS_ROOT" -maxdepth 2 -type d \
    -path '*/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-*/toolchain' \
    -print | sort -V | tail -1
)}
[ -n "$TC" ] || { echo "toolchain NextOS atual não encontrado em $NEXTOS_ROOT" >&2; exit 1; }

CC=$TC/bin/aarch64-libreelec-linux-gnu-gcc
READELF=$TC/bin/aarch64-libreelec-linux-gnu-readelf
SR=$TC/aarch64-libreelec-linux-gnu/sysroot
LIBC=$SR/usr/lib/libc.so.6
cd "$(dirname "$0")"

[ -x "$CC" ] || { echo "compilador não encontrado: $CC" >&2; exit 1; }
[ -x "$READELF" ] || { echo "readelf não encontrado: $READELF" >&2; exit 1; }
[ -s "$LIBC" ] || { echo "libc do sysroot não encontrada: $LIBC" >&2; exit 1; }

SYSROOT_GLIBC=$(
  "$READELF" -V "$LIBC" |
    grep -oE 'GLIBC_[0-9]+([.][0-9]+)*' |
    sort -Vu | tail -1
)
mapfile -t SRCS < <(find src -maxdepth 1 -type f -name '*.c' -print | sort)
"$CC" --sysroot="$SR" -I src -I "$SR/usr/include" \
  -O2 -fPIC -fno-omit-frame-pointer -rdynamic \
  -o horizonchase "${SRCS[@]}" \
  -lSDL2 -ldl -lm -lpthread -lgcc_s

echo "NEXTOS BUILD OK -> $(file horizonchase | cut -d, -f1-3)"
echo "toolchain: $TC"
echo "glibc do sysroot: $SYSROOT_GLIBC"
sha256sum horizonchase
