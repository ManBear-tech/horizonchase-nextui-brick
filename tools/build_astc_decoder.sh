#!/bin/sh
# Build Arm astcenc 5.0.0 as the runtime decoder used by Horizon Chase.
set -eu

astc_source=${ASTCENC_SOURCE:-${1:-}}
if [ -z "$astc_source" ] || [ ! -f "$astc_source/astcenc.h" ]; then
  echo "Usage: ASTCENC_SOURCE=/path/to/astc-encoder/Source $0" >&2
  exit 2
fi

script_dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
port_dir=$(dirname "$script_dir")
if [ -n "${NEXTOS_TOOLCHAIN:-}" ]; then
  nextos_toolchain=$NEXTOS_TOOLCHAIN
else
  nextos_root=${NEXTOS_ROOT:-"$HOME/NextOS-Elite-Edition"}
  nextos_toolchain=$(
    find -H "$nextos_root" -maxdepth 2 -type d \
      -path '*/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-*/toolchain' \
      -print | sort -V | tail -1
  )
fi
compiler=$nextos_toolchain/bin/aarch64-libreelec-linux-gnu-g++
stripper=$nextos_toolchain/bin/aarch64-libreelec-linux-gnu-strip
symbol_reader=$nextos_toolchain/bin/aarch64-libreelec-linux-gnu-nm
version_reader=$nextos_toolchain/bin/aarch64-libreelec-linux-gnu-readelf
sysroot=$nextos_toolchain/aarch64-libreelec-linux-gnu/sysroot
output=$port_dir/libastcUtil.so

[ -n "$nextos_toolchain" ] ||
  { echo "NextOS toolchain not found" >&2; exit 1; }
[ -x "$compiler" ] || { echo "NextOS compiler not found: $compiler" >&2; exit 1; }
[ -d "$sysroot" ] || { echo "NextOS sysroot not found: $sysroot" >&2; exit 1; }
[ -x "$version_reader" ] ||
  { echo "NextOS readelf not found: $version_reader" >&2; exit 1; }

"$compiler" --sysroot="$sysroot" \
  -O3 -fPIC -shared -std=c++17 -march=armv8-a+simd \
  -DASTCENC_DECOMPRESS_ONLY=1 -fno-strict-aliasing \
  -fno-exceptions -fno-rtti -I "$astc_source" \
  -Wl,-soname,libastcUtil.so \
  -o "$output" "$astc_source"/astcenc_*.cpp \
  -lm -lpthread -lgcc_s
"$stripper" "$output"

symbols=$("$symbol_reader" -D "$output")
for required_symbol in \
  _Z19astcenc_config_init15astcenc_profilejjjfjP14astcenc_config \
  _Z21astcenc_context_allocPK14astcenc_configjPP15astcenc_context \
  _Z24astcenc_decompress_imageP15astcenc_contextPKhmP13astcenc_imagePK15astcenc_swizzlej \
  _Z24astcenc_decompress_resetP15astcenc_context; do
  printf '%s\n' "$symbols" | grep -F "$required_symbol" >/dev/null ||
    { echo "Incompatible astcenc API: missing $required_symbol" >&2; exit 1; }
done

echo "Built $output"
sysroot_glibc=$(
  "$version_reader" -V "$sysroot/usr/lib/libc.so.6" |
    grep -oE 'GLIBC_[0-9]+([.][0-9]+)*' |
    sort -Vu | tail -1
)
echo "Toolchain: $nextos_toolchain"
echo "Sysroot glibc: $sysroot_glibc"
file "$output"
sha256sum "$output"
