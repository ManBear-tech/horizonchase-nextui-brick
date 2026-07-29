#!/usr/bin/env bash
# Public low-glibc AArch64 build used unchanged on ArkOS/R36S and NextOS.
set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export HC_R36S_OUTPUT=${HC_UNIVERSAL_OUTPUT:-horizonchase-universal}
exec "$SCRIPT_DIR/build_r36s.sh"
