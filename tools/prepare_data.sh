#!/bin/sh
# Prepare legally owned Horizon Chase 2.6.9 Android files for the NextOS port.
set -eu

usage() {
  cat <<'EOF'
Usage:
  prepare_data.sh BASE_APK UNITY_DATA_ASSET_PACK_APK OUTPUT_DIR

The script never downloads or ships game data. BASE_APK and the asset-pack APK
must come from the user's own legitimate arm64-v8a installation.
EOF
}

[ "$#" -eq 3 ] || { usage >&2; exit 2; }
base_apk=$1
asset_pack_apk=$2
output_dir=${3%/}

[ -f "$base_apk" ] || { echo "Base APK not found: $base_apk" >&2; exit 1; }
[ -f "$asset_pack_apk" ] ||
  { echo "Unity data asset-pack APK not found: $asset_pack_apk" >&2; exit 1; }
[ -n "$output_dir" ] && [ "$output_dir" != "/" ] ||
  { echo "Refusing unsafe output directory: $output_dir" >&2; exit 1; }
command -v unzip >/dev/null 2>&1 ||
  { echo "Missing required host tool: unzip" >&2; exit 1; }

for member in \
  lib/arm64-v8a/libunity.so \
  lib/arm64-v8a/libil2cpp.so \
  lib/arm64-v8a/libmain.so \
  assets/bin/Data/data.unity3d; do
  unzip -Z1 "$base_apk" | grep -Fx "$member" >/dev/null ||
    { echo "BASE_APK is missing $member" >&2; exit 1; }
done

pack_member=assets/bin/Data/datapack.unity3d
unzip -Z1 "$asset_pack_apk" | grep -Fx "$pack_member" >/dev/null ||
  { echo "Asset-pack APK is missing $pack_member" >&2; exit 1; }

work_dir=$(mktemp -d "${TMPDIR:-/tmp}/horizon-data.XXXXXX")
trap 'rm -rf "$work_dir"' EXIT HUP INT TERM

mkdir -p "$work_dir/root"
unzip -q "$base_apk" 'assets/*' -d "$work_dir/base"
cp -a "$work_dir/base/assets/." "$work_dir/root/"
unzip -q -j "$base_apk" \
  lib/arm64-v8a/libunity.so \
  lib/arm64-v8a/libil2cpp.so \
  lib/arm64-v8a/libmain.so \
  -d "$work_dir/root"
cp "$asset_pack_apk" "$work_dir/root/UnityDataAssetPack.apk"

mkdir -p "$output_dir"
cp -a "$work_dir/root/." "$output_dir/"

for required in \
  "$output_dir/libunity.so" \
  "$output_dir/libil2cpp.so" \
  "$output_dir/libmain.so" \
  "$output_dir/bin/Data/data.unity3d" \
  "$output_dir/UnityDataAssetPack.apk"; do
  [ -s "$required" ] || { echo "Prepared file is empty: $required" >&2; exit 1; }
done

echo "Horizon Chase data prepared in: $output_dir"
echo "Copy the loader, libastcUtil.so and run.sh beside these files."
