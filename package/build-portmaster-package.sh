#!/usr/bin/env bash
# Build the public, universal, BYO-data PortMaster release.
set -euo pipefail

export LC_ALL=C
export TZ=UTC

fail() {
  printf 'package error: %s\n' "$*" >&2
  exit 1
}

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
PORT_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd -P)
REPO_ROOT=$(cd -- "$PORT_DIR/../.." && pwd -P)
STATIC_DIR="$SCRIPT_DIR/universal"
PORTMASTER_DIR="$SCRIPT_DIR/r36s"
if [[ -n "${HC_NXEXTRACT_DIR:-}" ]]; then
  NXEXTRACT_DIR=$HC_NXEXTRACT_DIR
elif [[ -d "$PORT_DIR/third_party/NXExtract" ]]; then
  NXEXTRACT_DIR="$PORT_DIR/third_party/NXExtract"
else
  NXEXTRACT_DIR="$REPO_ROOT/suportando_outros_devices/extrator-universal"
fi
ALLOWLIST="$STATIC_DIR/package-files.txt"
if [[ -n "${HC_PORTMASTER_BINARY:-}" ]]; then
  BINARY=$HC_PORTMASTER_BINARY
elif [[ -f "$PORT_DIR/horizonchase-universal" ]]; then
  BINARY="$PORT_DIR/horizonchase-universal"
else
  BINARY="$PORT_DIR/horizonchase-r36s"
fi
UI_BINARY=${HC_NXEXTRACT_UI:-"$NXEXTRACT_DIR/ui/build/nxextract-ui"}
COVER=${HC_COVER:-"$STATIC_DIR/media/cover.png"}
SCREENSHOT=${HC_SCREENSHOT:-"$STATIC_DIR/media/screenshot.png"}
SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1785283200}
OUTPUT=${1:-"$SCRIPT_DIR/dist/horizonchase.zip"}

for tool in awk bash cmp comm dirname find grep install mkdir mktemp \
            python3 readelf rm sed sha256sum sort touch unzip zip; do
  command -v "$tool" >/dev/null 2>&1 || fail "missing host tool: $tool"
done

case "$SOURCE_DATE_EPOCH" in
  ''|*[!0-9]*) fail "SOURCE_DATE_EPOCH must be a Unix timestamp" ;;
esac
(( SOURCE_DATE_EPOCH >= 315532800 )) ||
  fail "SOURCE_DATE_EPOCH predates ZIP timestamps"
(( SOURCE_DATE_EPOCH <= 4354819198 )) ||
  fail "SOURCE_DATE_EPOCH exceeds ZIP timestamps"
(( SOURCE_DATE_EPOCH % 2 == 0 )) ||
  fail "SOURCE_DATE_EPOCH must use ZIP's two-second granularity"

[[ -f "$ALLOWLIST" ]] || fail "missing allowlist: $ALLOWLIST"
EXPECTED_SORT=$(mktemp "${TMPDIR:-/tmp}/horizon-allowlist.XXXXXX")
sort -u "$ALLOWLIST" > "$EXPECTED_SORT"
cmp -s "$ALLOWLIST" "$EXPECTED_SORT" ||
  fail "package-files.txt must be sorted and unique"

while IFS= read -r relative; do
  [[ -n "$relative" ]] || fail "blank allowlist entry"
  case "$relative" in
    /*|../*|*/../*|*/..|*/./*|./*)
      fail "unsafe allowlist path: $relative"
      ;;
  esac
done < "$ALLOWLIST"

TMP_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/horizon-portmaster.XXXXXX")
STAGE="$TMP_ROOT/stage"
TMP_ZIP="$TMP_ROOT/horizonchase.zip"
cleanup() {
  rm -rf -- "$TMP_ROOT"
  rm -f -- "$EXPECTED_SORT"
}
trap cleanup EXIT INT TERM
mkdir -p -- "$STAGE"

put() {
  local mode=$1 source=$2 destination=$3
  [[ -f "$source" ]] || fail "missing package source: $source"
  install -D -m "$mode" -- "$source" "$STAGE/$destination"
}

put 0755 "$PORTMASTER_DIR/Horizon Chase.sh" \
  "Horizon Chase.sh"
put 0755 "$BINARY" \
  "horizonchase/horizonchase"
put 0755 "$PORT_DIR/run.sh" \
  "horizonchase/run.sh"
put 0644 "$PORT_DIR/libastcUtil.so" \
  "horizonchase/libastcUtil.so"
put 0755 "$NXEXTRACT_DIR/run-extractor.sh" \
  "horizonchase/run-extractor.sh"
put 0644 "$NXEXTRACT_DIR/nxextract.py" \
  "horizonchase/nxextract.py"
put 0755 "$UI_BINARY" \
  "horizonchase/nxextract-ui"
put 0755 "$PORT_DIR/tools/build_unity_asset_pack.py" \
  "horizonchase/tools/build_unity_asset_pack.py"
put 0755 "$PORT_DIR/tools/import_android_save.py" \
  "horizonchase/tools/import_android_save.py"
# The bench unlock lives in the working tree's run.sh so the port can be
# maintained and tested on a device. It is stripped here rather than by hand:
# forgetting once would ship it. The delimiters are literal and must keep
# matching the ones in run.sh.
sed -i '/^# >>> HC_BENCH_BLOCK/,/^# <<< HC_BENCH_BLOCK/d' \
  "$STAGE/horizonchase/run.sh"
if grep -qE 'HC_BENCH_BLOCK|HC_UNLOCK|HC_BENCH_PY|bench\.unlock|bench-unlock' \
    "$STAGE/horizonchase/run.sh"; then
  fail "the bench unlock survived the strip in the staged run.sh"
fi
if ! grep -qE '^# >>> HC_BENCH_BLOCK' "$PORT_DIR/run.sh"; then
  printf 'package note: no bench block found in run.sh; nothing to strip\n' >&2
fi

put 0644 "$STATIC_DIR/extractor.json" \
  "horizonchase/extractor.json"
put 0644 "$STATIC_DIR/gamedata/README.txt" \
  "horizonchase/gamedata/README.txt"
put 0644 "$STATIC_DIR/CHANGELOG.md" \
  "horizonchase/CHANGELOG.md"
put 0644 "$STATIC_DIR/version.txt" \
  "horizonchase/version.txt"
put 0644 "$PORT_DIR/README.md" \
  "horizonchase/README.md"
put 0644 "$PORT_DIR/R36S-MIGRATION.md" \
  "horizonchase/R36S-MIGRATION.md"
put 0644 "$PORT_DIR/NOTICE.md" \
  "horizonchase/NOTICE.md"
put 0644 "$PORTMASTER_DIR/port.json" \
  "horizonchase/port.json"
put 0644 "$PORTMASTER_DIR/gameinfo.xml" \
  "horizonchase/gameinfo.xml"
put 0644 "$COVER" \
  "horizonchase/cover.png"
put 0644 "$SCREENSHOT" \
  "horizonchase/screenshot.png"
put 0644 "$PORT_DIR/licenses/Apache-2.0.txt" \
  "horizonchase/licenses/Apache-2.0.txt"
put 0644 "$PORT_DIR/licenses/GPL-3.0.txt" \
  "horizonchase/licenses/GPL-3.0.txt"
put 0644 "$PORT_DIR/licenses/Producdevity-MIT.txt" \
  "horizonchase/licenses/Producdevity-MIT.txt"
put 0644 "$PORT_DIR/licenses/Unlicense.txt" \
  "horizonchase/licenses/Unlicense.txt"
put 0644 "$NXEXTRACT_DIR/LICENSE" \
  "horizonchase/licenses/NXExtract-MIT.txt"

check_aarch64_glibc() {
  local file=$1 maximum=$2 machine newest selected
  machine=$(readelf -h "$file" |
    sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')
  [[ "$machine" == "AArch64" ]] ||
    fail "$file is not AArch64 (found: $machine)"
  newest=$(readelf --version-info "$file" 2>/dev/null |
    grep -oE 'GLIBC_[0-9]+([.][0-9]+)*' |
    sed 's/^GLIBC_//' | sort -Vu | tail -1)
  [[ -n "$newest" ]] || return 0
  selected=$(printf '%s\n%s\n' "$maximum" "$newest" | sort -V | tail -1)
  [[ "$selected" == "$maximum" ]] ||
    fail "$file requires GLIBC_$newest (maximum: GLIBC_$maximum)"
}

check_aarch64_glibc "$STAGE/horizonchase/horizonchase" 2.30
check_aarch64_glibc "$STAGE/horizonchase/libastcUtil.so" 2.30
check_aarch64_glibc "$STAGE/horizonchase/nxextract-ui" 2.30

TLS_FILESZ=$(readelf -lW "$STAGE/horizonchase/horizonchase" |
  awk '$1 == "TLS" { value = $5 } END { print value }')
PAD_LAYOUT=$(readelf -sW "$STAGE/horizonchase/horizonchase" |
  awk '$4 == "TLS" && $8 == "g_bionic_guard_pad" {
    value = $2 ":" $3
  } END { print value }')
[[ "$PAD_LAYOUT" == "0000000000000000:256" ]] ||
  fail "Bionic TLS guard pad layout changed: $PAD_LAYOUT"
[[ "$TLS_FILESZ" == "0x000100" ]] ||
  fail "unexpected TLS template size: $TLS_FILESZ"

bash -n "$STAGE/Horizon Chase.sh"
sh -n "$STAGE/horizonchase/run.sh"
bash -n "$STAGE/horizonchase/run-extractor.sh"
python3 - "$STAGE/horizonchase/nxextract.py" \
             "$STAGE/horizonchase/tools/build_unity_asset_pack.py" \
             "$STAGE/horizonchase/tools/import_android_save.py" <<'PY'
import sys

for path in sys.argv[1:]:
    with open(path, "rb") as stream:
        compile(stream.read(), path, "exec")
PY
python3 "$STAGE/horizonchase/nxextract.py" recipe-check \
  --recipe "$STAGE/horizonchase/extractor.json" >/dev/null

if grep -En '^[[:space:]]*(export[[:space:]]+)?SDL_(VIDEO|AUDIO)DRIVER=' \
    "$STAGE/Horizon Chase.sh" "$STAGE/horizonchase/run.sh"; then
  fail "launcher must not force an SDL video or audio backend"
fi
if grep -En \
    '(^|[[:space:]])(setsid|nohup|systemctl[[:space:]]+(stop|mask))([[:space:]]|$)' \
    "$STAGE/Horizon Chase.sh" "$STAGE/horizonchase/run.sh"; then
  fail "launcher contains a forbidden lifecycle command"
fi

python3 - "$STAGE/horizonchase/port.json" \
             "$STAGE/horizonchase/gameinfo.xml" <<'PY'
import json
import sys
import xml.etree.ElementTree as ET

with open(sys.argv[1], encoding="utf-8") as stream:
    metadata = json.load(stream)
if metadata.get("version") != 4:
    raise SystemExit("port.json schema must be version 4")
if metadata.get("name") != "horizonchase.zip":
    raise SystemExit("port.json stable name must be horizonchase.zip")
if metadata.get("items") != ["Horizon Chase.sh", "horizonchase"]:
    raise SystemExit("port.json items do not match the archive layout")
if metadata.get("attr", {}).get("arch") != ["aarch64"]:
    raise SystemExit("port.json must declare AArch64")
images = {
    "screenshot": "screenshot.png",
    "covers": ["cover.png"],
    "thumbnail": "cover.png",
    "video": None,
}
if metadata.get("attr", {}).get("image") != images:
    raise SystemExit("port.json image metadata is incomplete")

game = ET.parse(sys.argv[2]).getroot().find("game")
if game is None or game.findtext("path") != "./Horizon Chase.sh":
    raise SystemExit("gameinfo.xml launcher path is invalid")
if game.findtext("image") != "./horizonchase/cover.png":
    raise SystemExit("gameinfo.xml cover path is invalid")
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
    -name 'debug.log' -o -name 'userdata' -o -name 'artifacts' -o \
    -name 'HANDOFF.md' -o -name '__pycache__' -o -name '*.pyc' -o \
    -name 'dev_*.py' -o -name 'save-imports' \
  \) -print -quit | grep . >/dev/null; then
  fail "development artifact entered the public staging tree"
fi
if grep -IRnE '192[.]168[.]|/home/|/mnt/ARQUIVOS|root@' "$STAGE" \
    --include='*.sh' --include='*.py' --include='*.md' \
    --include='*.txt' --include='*.json' --include='*.xml'; then
  fail "release text contains a test address or personal path"
fi

(
  cd -- "$STAGE"
  while IFS= read -r relative; do
    case "$relative" in
      "Horizon Chase.sh"|horizonchase/port.json|\
      horizonchase/PACKAGE-MANIFEST.sha256)
        continue
        ;;
    esac
    sha256sum -- "$relative"
  done < "$ALLOWLIST"
) > "$STAGE/horizonchase/PACKAGE-MANIFEST.sha256"

ACTUAL="$TMP_ROOT/actual.txt"
find "$STAGE" -type f -printf '%P\n' | sort > "$ACTUAL"
cmp -s "$ALLOWLIST" "$ACTUAL" || {
  comm -3 "$ALLOWLIST" "$ACTUAL" >&2
  fail "staged files differ from package-files.txt"
}

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
  sha256sum -c horizonchase/PACKAGE-MANIFEST.sha256 >/dev/null
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
printf 'Loader: '
sha256sum "$BINARY"
printf 'Package: '
sha256sum "$OUTPUT"
