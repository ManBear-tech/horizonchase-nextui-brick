#!/usr/bin/env bash
# Build the public BYO-data package for TrimUI Brick running NextUI.
set -euo pipefail

export LC_ALL=C
export TZ=UTC

fail() {
  printf 'NextUI package error: %s\n' "$*" >&2
  exit 1
}

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
PORT_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd -P)
NEXTUI_DIR="$SCRIPT_DIR/nextui"
ALLOWLIST="$NEXTUI_DIR/package-files.txt"
BINARY=${HC_NEXTUI_BINARY:-"$PORT_DIR/horizonchase-universal"}
EXPECTED_LOADER_SHA256=${HC_EXPECTED_LOADER_SHA256:-6c243cd9b5b8a5a4fc67a3461b6b9527d57f8af40ea97029a2ab49af7ba45e79}
SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1785283200}
OUTPUT=${1:-"$SCRIPT_DIR/dist/horizonchase-nextui-brick.zip"}

for tool in bash cmp comm cp find grep install mkdir mktemp mv python3 \
            readelf rm sed sha256sum sort touch unzip zip; do
  command -v "$tool" >/dev/null 2>&1 || fail "missing host tool: $tool"
done

[[ -f "$ALLOWLIST" ]] || fail "missing allowlist: $ALLOWLIST"
[[ -f "$BINARY" ]] || fail "missing Brick loader: $BINARY"
actual_loader_sha=$(sha256sum "$BINARY" | awk '{print $1}')
[[ "$actual_loader_sha" == "$EXPECTED_LOADER_SHA256" ]] ||
  fail "loader SHA-256 is $actual_loader_sha; expected tested Brick build $EXPECTED_LOADER_SHA256"

SORTED=$(mktemp "${TMPDIR:-/tmp}/horizon-nextui-allowlist.XXXXXX")
sort -u "$ALLOWLIST" > "$SORTED"
cmp -s "$ALLOWLIST" "$SORTED" ||
  fail "package-files.txt must be sorted and unique"

TMP_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/horizon-nextui.XXXXXX")
BASE_ZIP="$TMP_ROOT/base.zip"
STAGE="$TMP_ROOT/stage"
TMP_ZIP="$TMP_ROOT/horizonchase-nextui-brick.zip"
cleanup() {
  rm -rf -- "$TMP_ROOT"
  rm -f -- "$SORTED"
}
trap cleanup EXIT INT TERM

# Reuse the upstream audited staging path first. It validates NXExtract,
# architecture/glibc/TLS requirements, strips the private bench block, and
# rejects proprietary payloads and development artifacts.
HC_PORTMASTER_BINARY="$BINARY" \
  "$SCRIPT_DIR/build-portmaster-package.sh" "$BASE_ZIP" >/dev/null

mkdir -p -- "$STAGE/.ports"
unzip -q "$BASE_ZIP" -d "$TMP_ROOT/base"
mv -- "$TMP_ROOT/base/horizonchase" "$STAGE/.ports/horizonchase"
install -m 0755 -- "$NEXTUI_DIR/Horizon Chase.sh" "$STAGE/Horizon Chase.sh"
install -m 0755 -- "$SCRIPT_DIR/r36s/Horizon Chase.sh" \
  "$STAGE/.ports/horizonchase/launcher-common.sh"
install -m 0644 -- "$NEXTUI_DIR/CHANGELOG.md" \
  "$STAGE/.ports/horizonchase/CHANGELOG.md"
install -m 0644 -- "$NEXTUI_DIR/version.txt" \
  "$STAGE/.ports/horizonchase/version.txt"
install -m 0644 -- "$NEXTUI_DIR/port.json" \
  "$STAGE/.ports/horizonchase/port.json"
install -m 0644 -- "$NEXTUI_DIR/gameinfo.xml" \
  "$STAGE/.ports/horizonchase/gameinfo.xml"

# Rebuild the manifest after moving the runtime into NextUI's hidden .ports
# directory and adding the common launcher used by the tiny menu entry point.
(
  cd -- "$STAGE"
  while IFS= read -r relative; do
    case "$relative" in
      .ports/horizonchase/PACKAGE-MANIFEST.sha256) continue ;;
    esac
    sha256sum -- "$relative"
  done < "$ALLOWLIST"
) > "$STAGE/.ports/horizonchase/PACKAGE-MANIFEST.sha256"

ACTUAL="$TMP_ROOT/actual.txt"
find "$STAGE" -type f -printf '%P\n' | sort > "$ACTUAL"
cmp -s "$ALLOWLIST" "$ACTUAL" || {
  comm -3 "$ALLOWLIST" "$ACTUAL" >&2
  fail "staged files differ from package-files.txt"
}

bash -n "$STAGE/Horizon Chase.sh"
bash -n "$STAGE/.ports/horizonchase/launcher-common.sh"
sh -n "$STAGE/.ports/horizonchase/run.sh"
grep -Fq '/mnt/SDCARD/Roms/Ports (PORTS)' "$STAGE/Horizon Chase.sh" ||
  fail "NextUI launcher lost the documented Ports root"
grep -Fq '.ports/horizonchase' "$STAGE/Horizon Chase.sh" ||
  fail "NextUI launcher lost the hidden runtime path"
grep -Fq 'HC_PURE_SDL_CONTEXTS=1' "$STAGE/Horizon Chase.sh" ||
  fail "PowerVR SDL-context override is missing"
grep -Fq 'HC_FRAME_LIMIT:-60' "$STAGE/Horizon Chase.sh" ||
  fail "60 FPS default is missing"
grep -Fq 'HC_FORCE_INPUT_TYPE:-11' "$STAGE/Horizon Chase.sh" ||
  fail "manual Brick control scheme is missing"
if grep -qE 'export[[:space:]]+HC_GLSTATE_TRACE=' "$STAGE/Horizon Chase.sh"; then
  fail "diagnostic GL-state tracing must not ship enabled"
fi

python3 - "$STAGE/.ports/horizonchase/port.json" \
             "$STAGE/.ports/horizonchase/gameinfo.xml" <<'PY'
import json
import sys
import xml.etree.ElementTree as ET

with open(sys.argv[1], encoding="utf-8") as stream:
    metadata = json.load(stream)
if metadata.get("name") != "horizonchase-nextui-brick.zip":
    raise SystemExit("unexpected NextUI package name")
if metadata.get("items") != ["Horizon Chase.sh", ".ports/horizonchase"]:
    raise SystemExit("NextUI package items do not match the archive layout")

game = ET.parse(sys.argv[2]).getroot().find("game")
if game is None or game.findtext("path") != "./Horizon Chase.sh":
    raise SystemExit("NextUI gameinfo launcher path is invalid")
if game.findtext("image") != "./.ports/horizonchase/cover.png":
    raise SystemExit("NextUI gameinfo cover path is invalid")
PY

if find "$STAGE" \( \
    -name '*.apk' -o -name '*.apks' -o -name '*.apkm' -o \
    -name '*.xapk' -o -name '*.obb' -o -name '*.dex' -o \
    -name 'libunity.so' -o -name 'libil2cpp.so' -o \
    -name 'libmain.so' -o -name 'global-metadata.dat' -o \
    -name 'sharedassets*' -o -name '*.unity3d' \
  \) -print -quit | grep . >/dev/null; then
  fail "proprietary game data entered the public staging tree"
fi
if find "$STAGE" \( \
    -name '*.log' -o -name '*.raw' -o -name '*.ppm' -o \
    -name 'userdata' -o -name '__pycache__' -o -name '*.pyc' \
  \) -print -quit | grep . >/dev/null; then
  fail "development or personal data entered the public staging tree"
fi

find "$STAGE" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +
(
  cd -- "$STAGE"
  zip -X -9 -q "$TMP_ZIP" -@ < "$ALLOWLIST"
)
unzip -tq "$TMP_ZIP" >/dev/null
unzip -Z1 "$TMP_ZIP" > "$TMP_ROOT/archive.txt"
cmp -s "$ALLOWLIST" "$TMP_ROOT/archive.txt" ||
  fail "ZIP entries or ordering differ from package-files.txt"

VERIFY="$TMP_ROOT/verify"
mkdir -p -- "$VERIFY"
unzip -q "$TMP_ZIP" -d "$VERIFY"
(
  cd -- "$VERIFY"
  sha256sum -c .ports/horizonchase/PACKAGE-MANIFEST.sha256 >/dev/null
)

mkdir -p -- "$(dirname -- "$OUTPUT")"
OUTPUT_DIR=$(cd -- "$(dirname -- "$OUTPUT")" && pwd -P)
OUTPUT="$OUTPUT_DIR/$(basename -- "$OUTPUT")"
install -m 0644 -- "$TMP_ZIP" "$OUTPUT"
(
  cd -- "$OUTPUT_DIR"
  sha256sum "$(basename -- "$OUTPUT")" > "$(basename -- "$OUTPUT").sha256"
)

printf 'OK: %s\n' "$OUTPUT"
printf 'Loader: %s  %s\n' "$actual_loader_sha" "$(basename -- "$BINARY")"
printf 'Package: '
sha256sum "$OUTPUT"
