#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/horizon-launcher-test.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT INT TERM

fail() {
  printf 'launcher_data_gate_test: %s\n' "$*" >&2
  exit 1
}

make_fixture() {
  local name=$1
  local root="$TEST_ROOT/$name"
  local ports="$root/roms/ports"
  local game="$ports/horizonchase"

  mkdir -p "$game" "$root/xdg/PortMaster" "$root/fakebin"
  : > "$root/xdg/PortMaster/control.txt"
  cp "$PROJECT_ROOT/package/r36s/Horizon Chase.sh" \
    "$ports/Horizon Chase.sh"
  printf 'loader placeholder\n' > "$game/horizonchase"
  printf '{}\n' > "$game/extractor.json"
  printf '1.0.3\n' > "$game/version.txt"
  cat > "$game/run.sh" <<'RUN'
#!/bin/sh
printf '[test] run.sh invoked\n'
: > "$HC_GAMEDIR/run.invoked"
RUN
  chmod 0755 "$ports/Horizon Chase.sh" "$game/run.sh"

  # Emulate an installer/filesystem that does not retain executable modes.
  # run.sh is already executable, while run-extractor.sh remains mode 0644.
  cat > "$root/fakebin/chmod" <<'CHMOD'
#!/bin/sh
exit 1
CHMOD
  chmod 0755 "$root/fakebin/chmod"
  printf '%s\n' "$root"
}

write_successful_extractor() {
  local game=$1
  cat > "$game/run-extractor.sh" <<'EXTRACTOR'
#!/usr/bin/env bash
set -euo pipefail
printf '[test] non-executable extractor invoked through Bash\n'
mkdir -p \
  "$HC_GAMEDIR/bin/Data/Managed/Metadata" \
  "$HC_GAMEDIR/Android"
for relative in \
  libunity.so \
  libil2cpp.so \
  libmain.so \
  bin/Data/Managed/Metadata/global-metadata.dat \
  UnityServicesProjectConfiguration.json \
  UnityDataAssetPack.apk; do
  printf 'validated payload\n' > "$HC_GAMEDIR/$relative"
done
EXTRACTOR
  chmod 0644 "$game/run-extractor.sh"
}

run_launcher() {
  local root=$1
  local ports="$root/roms/ports"
  env \
    HOME="$root/home" \
    XDG_DATA_HOME="$root/xdg" \
    CUR_TTY=/dev/null \
    directory="$root/roms" \
    PATH="$root/fakebin:$PATH" \
    bash "$ports/Horizon Chase.sh"
}

success_root=$(make_fixture success)
success_game="$success_root/roms/ports/horizonchase"
write_successful_extractor "$success_game"
run_launcher "$success_root"
[ -f "$success_game/run.invoked" ] ||
  fail "run.sh was not called after successful validation"
[ ! -x "$success_game/run-extractor.sh" ] ||
  fail "test did not preserve the non-executable extractor condition"
grep -Fq '[test] non-executable extractor invoked through Bash' \
  "$success_game/debug.log" ||
  fail "non-executable extractor was not invoked"
grep -Fq '[launcher] package runner=readable recipe=readable' \
  "$success_game/debug.log" ||
  fail "pre-chmod package diagnostics were not logged"
grep -Fq '[launcher] runtime data gate OK' "$success_game/debug.log" ||
  fail "successful runtime gate was not logged"

missing_root=$(make_fixture missing-runner)
missing_game="$missing_root/roms/ports/horizonchase"
set +e
run_launcher "$missing_root"
missing_status=$?
set -e
[ "$missing_status" -eq 72 ] ||
  fail "missing extractor returned $missing_status instead of 72"
[ ! -e "$missing_game/run.invoked" ] ||
  fail "loader path ran without the extractor"
grep -Fq 'ERROR: data setup runner is missing' "$missing_game/debug.log" ||
  fail "missing extractor diagnostic was not logged"

incomplete_root=$(make_fixture incomplete-payload)
incomplete_game="$incomplete_root/roms/ports/horizonchase"
cat > "$incomplete_game/run-extractor.sh" <<'EXTRACTOR'
#!/usr/bin/env bash
printf '[test] extractor returned success without a payload\n'
exit 0
EXTRACTOR
chmod 0644 "$incomplete_game/run-extractor.sh"
set +e
run_launcher "$incomplete_root"
incomplete_status=$?
set -e
[ "$incomplete_status" -eq 73 ] ||
  fail "incomplete payload returned $incomplete_status instead of 73"
[ ! -e "$incomplete_game/run.invoked" ] ||
  fail "loader path ran with an incomplete payload"
grep -Fq 'ERROR: runtime data gate failed' "$incomplete_game/debug.log" ||
  fail "incomplete payload diagnostic was not logged"
grep -Fq 'libunity.so' "$incomplete_game/debug.log" ||
  fail "missing libunity.so was not identified"

printf 'launcher_data_gate_test: OK\n'
