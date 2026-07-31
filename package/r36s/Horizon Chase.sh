#!/bin/bash
# Universal PortMaster entry point for Horizon Chase.

XDG_DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"

if [ -d /opt/system/Tools/PortMaster ]; then
  controlfolder=/opt/system/Tools/PortMaster
elif [ -d /opt/tools/PortMaster ]; then
  controlfolder=/opt/tools/PortMaster
elif [ -d "$XDG_DATA_HOME/PortMaster" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
elif [ -d /roms/ports/PortMaster ]; then
  controlfolder=/roms/ports/PortMaster
else
  controlfolder=/storage/.config/PortMaster
fi

[ -f "$controlfolder/control.txt" ] &&
  source "$controlfolder/control.txt"
case "${CFW_NAME:-}" in
  ''|*[!A-Za-z0-9._-]*) ;;
  *) [ -f "$controlfolder/mod_${CFW_NAME}.txt" ] &&
       source "$controlfolder/mod_${CFW_NAME}.txt" ;;
esac
declare -F get_controls >/dev/null 2>&1 && get_controls
: "${ESUDO:=}"
: "${CUR_TTY:=/dev/tty0}"

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" 2>/dev/null && pwd -P) || exit 1
if [ -n "${directory:-}" ]; then
  GAMEDIR="/${directory#/}/ports/horizonchase"
else
  GAMEDIR="$SCRIPT_DIR/horizonchase"
fi
GAMEDIR=$(cd -- "$GAMEDIR" 2>/dev/null && pwd -P) || {
  echo "Horizon Chase: diretório ausente: $GAMEDIR" > "$CUR_TTY" 2>/dev/null
  exit 1
}

export HC_GAMEDIR="$GAMEDIR"
# The loader selects SDL-owned contexts for KMSDRM/Wayland and raw EGL only for
# the legacy SDL "mali" fbdev backend. No device name or video driver is forced.
export LD_LIBRARY_PATH="/usr/local/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu:/lib/aarch64-linux-gnu:/usr/lib:/lib:$controlfolder/libs:$controlfolder/libs.aarch64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
[ -n "${sdl_controllerconfig:-}" ] &&
  export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

cd "$GAMEDIR" || exit 1
exec > "$GAMEDIR/debug.log" 2>&1
hc_port_version=unknown
if [ -r "$GAMEDIR/version.txt" ]; then
  hc_port_version=$(tr -d '\r\n' < "$GAMEDIR/version.txt")
  [ -n "$hc_port_version" ] || hc_port_version=unknown
fi
hc_runner_mode=missing
if [ -f "$GAMEDIR/run-extractor.sh" ]; then
  hc_runner_mode=readable
  [ -x "$GAMEDIR/run-extractor.sh" ] && hc_runner_mode=executable
fi
printf '=== Horizon Chase port %s ===\n' "$hc_port_version"
printf '[launcher] package runner=%s recipe=%s\n' \
  "$hc_runner_mode" \
  "$([ -r "$GAMEDIR/extractor.json" ] && printf readable || printf missing)"
for executable in horizonchase run.sh run-extractor.sh nxextract-ui; do
  [ -f "$GAMEDIR/$executable" ] || continue
  ${ESUDO:-} chmod +x "$GAMEDIR/$executable" 2>/dev/null ||
    chmod +x "$GAMEDIR/$executable" 2>/dev/null || true
done
${ESUDO:-} chmod 666 "$CUR_TTY" /dev/uinput 2>/dev/null || true

hc_runtime_missing() {
  local missing= relative
  for relative in \
    libunity.so \
    libil2cpp.so \
    libmain.so \
    bin/Data/Managed/Metadata/global-metadata.dat \
    UnityServicesProjectConfiguration.json \
    UnityDataAssetPack.apk; do
    [ -s "$GAMEDIR/$relative" ] || missing="$missing $relative"
  done
  [ -d "$GAMEDIR/Android" ] || missing="$missing Android/"
  printf '%s' "${missing# }"
}

hc_marker_state=absent
[ -f "$GAMEDIR/.nxextract-horizonchase.json" ] &&
  hc_marker_state=present
hc_missing_before=$(hc_runtime_missing)
printf '[launcher] data gate begin marker=%s missing=%s\n' \
  "$hc_marker_state" "${hc_missing_before:-none}"

# Private pre-release installs used a boot.config-only Swappy workaround and
# created the asset-pack ZIP with host timestamps.  The loader now disables
# Swappy at the correct point in Unity's native lifecycle, while NXExtract
# validates a deterministic asset pack.  Normalize only those two known,
# content-addressed legacy artifacts so an existing 2.6.9 installation can be
# adopted without asking the owner to copy the APK again.
if [ ! -f "$GAMEDIR/.nxextract-horizonchase.json" ] &&
   command -v sha256sum >/dev/null 2>&1; then
  legacy_boot="$GAMEDIR/bin/Data/boot.config"
  if [ -f "$legacy_boot" ]; then
    boot_hash=$(sha256sum "$legacy_boot" 2>/dev/null | awk '{print $1}')
    if [ "$boot_hash" = b236a5eec7a2858cdcbea02bb19ea930b0b219b9eab143f97d0f45e8005c56f6 ]; then
      backup_dir="$GAMEDIR/userdata/migration-backups"
      mkdir -p "$backup_dir"
      cp -p "$legacy_boot" "$backup_dir/boot.config.swappy" 2>/dev/null || true
      boot_tmp="$legacy_boot.nxextract.$$"
      awk '$0 != "swappy.disable=1"' "$legacy_boot" > "$boot_tmp"
      normalized_hash=$(sha256sum "$boot_tmp" 2>/dev/null | awk '{print $1}')
      if [ "$normalized_hash" = 3100dbbe5b5a1a290035bab1e82406dcabc0c34f55f31feb184d78727081fc0f ]; then
        chmod --reference="$legacy_boot" "$boot_tmp" 2>/dev/null || true
        mv -f "$boot_tmp" "$legacy_boot"
        echo "Horizon Chase: legacy Swappy config normalized"
      else
        rm -f "$boot_tmp"
      fi
    fi
  fi

  # One earlier R36S recipe left an original boot.config backup inside
  # bin/Data.  That extra file makes the exact Unity tree intentionally fail
  # validation. Move it out only when its full content hash is known.
  legacy_boot_copy="$GAMEDIR/bin/Data/boot.config.pre-swappy-disable"
  if [ -f "$legacy_boot_copy" ]; then
    legacy_copy_hash=$(sha256sum "$legacy_boot_copy" 2>/dev/null | awk '{print $1}')
    if [ "$legacy_copy_hash" = 3100dbbe5b5a1a290035bab1e82406dcabc0c34f55f31feb184d78727081fc0f ]; then
      backup_dir="$GAMEDIR/userdata/migration-backups"
      mkdir -p "$backup_dir"
      if [ ! -e "$backup_dir/boot.config.pre-swappy-disable" ]; then
        mv "$legacy_boot_copy" "$backup_dir/boot.config.pre-swappy-disable"
      elif cmp -s "$legacy_boot_copy" "$backup_dir/boot.config.pre-swappy-disable"; then
        rm -f "$legacy_boot_copy"
      fi
    fi
  fi

  datapack="$GAMEDIR/bin/Data/datapack.unity3d"
  assetpack="$GAMEDIR/UnityDataAssetPack.apk"
  if [ -f "$datapack" ] &&
     [ -f "$GAMEDIR/tools/build_unity_asset_pack.py" ]; then
    datapack_hash=$(sha256sum "$datapack" 2>/dev/null | awk '{print $1}')
    assetpack_hash=$(sha256sum "$assetpack" 2>/dev/null | awk '{print $1}')
    if [ "$datapack_hash" = 5092b3c9c507c476b852bfb68325d63fe3a4c2ca62fad64fed39b6dc193e4dc5 ] &&
       [ "$assetpack_hash" != 8b4f33d1809c60f515b0c2d667ed05236ee0c48cec0259734ce381f257192483 ]; then
      python3 "$GAMEDIR/tools/build_unity_asset_pack.py" \
        "$datapack" "$assetpack" ||
        echo "Horizon Chase: legacy asset-pack normalization failed"
    fi
  fi
fi

if [ ! -f "$GAMEDIR/run-extractor.sh" ]; then
  printf '[launcher] ERROR: data setup runner is missing: run-extractor.sh\n'
  exit 72
fi
if [ ! -r "$GAMEDIR/extractor.json" ]; then
  printf '[launcher] ERROR: data setup recipe is missing or unreadable: extractor.json\n'
  exit 72
fi
if ! command -v python3 >/dev/null 2>&1; then
  printf '[launcher] ERROR: python3 is unavailable; NXExtract cannot run\n'
  exit 72
fi

# Invoke through Bash deliberately. Some manual ZIP installers preserve the
# file but discard its Unix execute bit; that must never skip owner-data setup.
printf '[launcher] starting NXExtract data validation (execute bit independent)\n'
bash "$GAMEDIR/run-extractor.sh" || {
  status=$?
  printf '[launcher] ERROR: owner-data setup failed status=%d\n' "$status"
  printf '\033c' >> "$CUR_TTY" 2>/dev/null || true
  command -v pm_finish >/dev/null 2>&1 && pm_finish
  exit "$status"
}
printf '[launcher] NXExtract data validation complete\n'

hc_missing_after=$(hc_runtime_missing)
if [ -n "$hc_missing_after" ]; then
  hc_marker_state=absent
  [ -f "$GAMEDIR/.nxextract-horizonchase.json" ] &&
    hc_marker_state=present
  printf '[launcher] ERROR: runtime data gate failed marker=%s missing=%s\n' \
    "$hc_marker_state" "$hc_missing_after"
  printf '[launcher] refusing to start the loader with an incomplete payload\n'
  exit 73
fi
printf '[launcher] runtime data gate OK\n'

# Always enter through run.sh.  Handing the raw ELF to a platform helper would
# skip the adaptive memory, texture, audio and controller environment.
"$GAMEDIR/run.sh"
status=$?

${ESUDO:-} chmod 666 "$CUR_TTY" 2>/dev/null || true
printf '\033c' >> "$CUR_TTY" 2>/dev/null || true
command -v pm_finish >/dev/null 2>&1 && pm_finish
exit "$status"
