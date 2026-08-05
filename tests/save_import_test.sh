#!/usr/bin/env bash
# Contract for the Android profile import: what it takes, what it refuses, and
# the on-disk layout it has to keep agreeing with prefs_load() in jni_shim.c.
set -euo pipefail

PROJECT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/horizon-save-import-test.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT INT TERM

IMPORTER="$PROJECT_ROOT/tools/import_android_save.py"

fail() {
  printf 'save_import_test: %s\n' "$*" >&2
  exit 1
}

write_xml() {
  # write_xml <path> <full-game:true|false> [extra-settings]
  local path=$1 full=$2 extra=${3:-}
  python3 - "$path" "$full" "$extra" <<'PY'
import json
import sys
import xml.sax.saxutils as escaping

path, full, extra = sys.argv[1], sys.argv[2] == "true", sys.argv[3]
profile = {
    "UserProfileVersion": 2,
    "RevisionNumber": 42,
    "Cups": [{"id": 1}],
    "Races": [1, 2, 3],
    "NumberOfTokens": 900,
    "UnlockedFullGame": full,
    "UnlockedProducts": ["full_game"] if full else [],
}
body = [f'<string name="user_profile">{escaping.escape(json.dumps(profile))}</string>']
if extra:
    body.append('<int name="ScreenWidth" value="1080" />')
    body.append('<boolean name="sfx" value="true" />')
    body.append('<float name="gamma" value="1.5" />')
with open(path, "w", encoding="utf-8") as stream:
    stream.write('<?xml version="1.0" encoding="utf-8"?>\n<map>\n')
    stream.write("\n".join(body))
    stream.write("\n</map>\n")
PY
}

dump_keys() {
  python3 - "$PROJECT_ROOT" "$1" <<'PY'
import sys

sys.path.insert(0, f"{sys.argv[1]}/tools")
from import_android_save import read_prefs

print(" ".join(sorted(read_prefs(sys.argv[2]))))
PY
}

profile_field() {
  python3 - "$PROJECT_ROOT" "$1" "$2" <<'PY'
import json
import sys

sys.path.insert(0, f"{sys.argv[1]}/tools")
from import_android_save import read_prefs

profile = json.loads(read_prefs(sys.argv[2])["user_profile"].sval)
print(profile.get(sys.argv[3]))
PY
}

# --- the port's own settings survive an import -------------------------------
# A phone's SharedPreferences describe that phone. Importing them wholesale
# would overwrite the resolution and audio the port tuned for the handheld.
GAME="$TEST_ROOT/scope"
mkdir -p "$GAME/userdata"
write_xml "$TEST_ROOT/phone.xml" true with-settings
python3 - "$PROJECT_ROOT" "$GAME" <<'PY'
import os
import sys

sys.path.insert(0, f"{sys.argv[1]}/tools")
from import_android_save import Entry, write_prefs

port_width = Entry()
port_width.set_int(640)
write_prefs(os.path.join(sys.argv[2], "userdata", "shared_prefs.bin"),
            {"ScreenWidth": port_width})
PY
python3 "$IMPORTER" "$TEST_ROOT/phone.xml" -g "$GAME" >/dev/null
keys=$(dump_keys "$GAME/userdata/shared_prefs.bin")
[[ "$keys" == "ScreenWidth user_profile" ]] ||
  fail "default scope imported phone settings: $keys"
width=$(python3 - "$PROJECT_ROOT" "$GAME/userdata/shared_prefs.bin" <<'PY'
import sys

sys.path.insert(0, f"{sys.argv[1]}/tools")
from import_android_save import read_prefs

print(read_prefs(sys.argv[2])["ScreenWidth"].ival)
PY
)
[[ "$width" == "640" ]] || fail "port resolution was overwritten: $width"

# --- --all is the explicit escape hatch --------------------------------------
python3 "$IMPORTER" "$TEST_ROOT/phone.xml" -g "$GAME" --all >/dev/null
keys=$(dump_keys "$GAME/userdata/shared_prefs.bin")
[[ "$keys" == "ScreenWidth gamma sfx user_profile" ]] ||
  fail "--all did not import every key: $keys"
[[ -f "$GAME/userdata/shared_prefs.bin.bak" ]] ||
  fail "the previous save was not backed up"

# --- a profile without the purchase imports, but unlocks nothing -------------
# The port never invents an entitlement; it copies only what the player's own
# profile already proves.
DEMO="$TEST_ROOT/demo"
mkdir -p "$DEMO"
write_xml "$TEST_ROOT/demo.xml" false
python3 "$IMPORTER" "$TEST_ROOT/demo.xml" -g "$DEMO" >/dev/null 2>&1
[[ "$(profile_field "$DEMO/userdata/shared_prefs.bin" UnlockedFullGame)" == "False" ]] ||
  fail "a demo profile came out unlocked"

# --- anything that is not a Horizon Chase profile is refused -----------------
printf '<?xml version="1.0"?>\n<map><string name="other">x</string></map>\n' \
  > "$TEST_ROOT/alien.xml"
if python3 "$IMPORTER" "$TEST_ROOT/alien.xml" -g "$TEST_ROOT/alien" \
     >/dev/null 2>&1; then
  fail "a foreign SharedPreferences file was accepted"
fi
printf 'not json at all\n' > "$TEST_ROOT/garbage.json"
if python3 "$IMPORTER" "$TEST_ROOT/garbage.json" -g "$TEST_ROOT/alien" \
     >/dev/null 2>&1; then
  fail "a non-JSON source was accepted"
fi

# --- --auto: the launcher path ------------------------------------------------
AUTO="$TEST_ROOT/auto"
mkdir -p "$AUTO/gamedata"
write_xml "$AUTO/gamedata/com.aquiris.horizonchase.v2.playerprefs.xml" true
output=$(python3 "$IMPORTER" --auto -g "$AUTO" 2>&1)
[[ "$(profile_field "$AUTO/userdata/shared_prefs.bin" RevisionNumber)" == "42" ]] ||
  fail "--auto did not import the dropped profile"
[[ -f "$AUTO/userdata/save-imports/com.aquiris.horizonchase.v2.playerprefs.xml" ]] ||
  fail "--auto did not archive the source"
[[ -z "$(ls -A "$AUTO/gamedata")" ]] ||
  fail "--auto left the source in the drop folder"

# A second launch must be silent and must not re-apply a now-stale profile over
# progress made on the handheld.
output=$(python3 "$IMPORTER" --auto -g "$AUTO" 2>&1)
[[ -z "$output" ]] || fail "--auto was not quiet on a second run: $output"

# An empty or absent drop folder is the normal case and must stay silent.
output=$(python3 "$IMPORTER" --auto -g "$TEST_ROOT/nowhere" 2>&1)
[[ -z "$output" ]] || fail "--auto complained about a missing gamedata: $output"

# A stray unrelated file must never break the launch.
STRAY="$TEST_ROOT/stray"
mkdir -p "$STRAY/gamedata"
printf 'just a note\n' > "$STRAY/gamedata/notes.xml"
output=$(python3 "$IMPORTER" --auto -g "$STRAY" 2>&1) ||
  fail "--auto failed on a stray file"
[[ -z "$output" ]] || fail "--auto complained about a stray file: $output"

# --- DLC ownership is read from progress, never from a flag ------------------
# A profile that owns nothing already carries every DLC container, zeroed, and
# even carries "unlocked" flags. Only a race the player actually ran proves the
# campaign was owned.
python3 - "$PROJECT_ROOT" <<'PY'
import sys

sys.path.insert(0, f"{sys.argv[1]}/tools")
from import_android_save import dlc_evidence

empty_container = {
    "RaceDataList": [
        {"Score": 0, "TimesRaced": 0, "BestPosition": 99, "ItemId": "chapter1"},
    ],
}
demo = {
    "AyrtonCareerSaveData": dict(empty_container),
    "SummerSaveData": dict(empty_container),
    # A locked profile carries this as true; it is not proof of anything.
    "AyrtonChampionshipSaveData": {"IsEasyUnlocked": True, "RaceDataList": []},
}
if dlc_evidence(demo):
    raise SystemExit(f"a demo profile was read as owning DLC: {dlc_evidence(demo)}")

played = {
    "AyrtonCareerSaveData": {
        "RaceDataList": [
            {"Score": 15400, "TimesRaced": 3, "ItemId": "chapter1"},
            {"Score": 0, "TimesRaced": 0, "ItemId": "chapter2"},
        ],
    },
}
found = dlc_evidence(played)
if len(found) != 1 or next(iter(found.values())) != 1:
    raise SystemExit(f"played DLC races were not recognized: {found}")

# A campaign shipped in a later build must still be recognized by shape.
future = {"SomeNewThingSaveData": {"RaceDataList": [{"TimesRaced": 2}]}}
if not dlc_evidence(future):
    raise SystemExit("an unknown DLC container was ignored")

# Anything that is not a race container must be left alone.
noise = {"TelemetryData": {"SessionRunning": False}, "Cups": [{"TimesRaced": 9}]}
if dlc_evidence(noise):
    raise SystemExit(f"non-DLC data was read as a DLC: {dlc_evidence(noise)}")
PY

# --- limits prefs_load() enforces --------------------------------------------
# Breaking either one makes the loader drop every entry, silently losing the
# save, so the importer has to refuse to write such a file.
python3 - "$PROJECT_ROOT" <<'PY'
import sys

sys.path.insert(0, f"{sys.argv[1]}/tools")
from import_android_save import (MAX_PREFS, MAX_STRING_LEN, Entry, SaveError,
                                 write_prefs)

too_many = {}
for index in range(MAX_PREFS + 1):
    entry = Entry()
    entry.set_int(index)
    too_many[f"k{index}"] = entry
try:
    write_prefs("/dev/null", too_many)
except SaveError:
    pass
else:
    raise SystemExit("write_prefs accepted more entries than the loader takes")

huge = Entry()
huge.set_string("x" * (MAX_STRING_LEN + 1))
try:
    write_prefs("/dev/null", {"user_profile": huge})
except SaveError:
    pass
else:
    raise SystemExit("write_prefs accepted a string the loader would reject")
PY

# --- the on-disk record must keep matching jni_shim.c -------------------------
python3 - "$PROJECT_ROOT" <<'PY'
import struct
import sys

sys.path.insert(0, f"{sys.argv[1]}/tools")
from import_android_save import MAGIC, RECORD

# uint32 key_len, uint32 string_len, uint8 flags, int ival, float fval,
# int64 lval, int bval -- written field by field, so no padding.
if struct.calcsize(RECORD) != 4 + 4 + 1 + 4 + 4 + 8 + 4:
    raise SystemExit(f"record layout drifted: {struct.calcsize(RECORD)} bytes")
if MAGIC != b"HCPREF2\x00":
    raise SystemExit(f"magic drifted: {MAGIC!r}")
PY

# --- the bench unlock must be unable to reach a release ----------------------
grep -q 'dev_unlock' "$PROJECT_ROOT/package/universal/package-files.txt" &&
  fail "the bench unlock tool is listed in the release allowlist"
grep -q "name 'dev_\*.py'" "$PROJECT_ROOT/package/build-portmaster-package.sh" ||
  fail "the build no longer rejects dev_*.py in the staging tree"

printf 'save_import_test: OK\n'
