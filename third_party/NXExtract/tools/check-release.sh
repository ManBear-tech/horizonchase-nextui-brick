#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$PROJECT_ROOT"

python3 -m py_compile \
  nxextract.py \
  tests/make_device_fixture.py \
  tests/test_nxextract.py
python3 -m unittest -v tests/test_nxextract.py
python3 -m json.tool examples/recipe-minimal.json >/dev/null
python3 nxextract.py recipe-check \
  --recipe examples/recipe-minimal.json \
  >/dev/null
project_version="$(tr -d '[:space:]' <VERSION)"
cli_version="$(python3 nxextract.py --version)"
test "$cli_version" = "NXExtract $project_version"

bash -n \
  run-extractor.sh \
  ui/build-ui.sh \
  ui/build-compat-container.sh \
  tools/check-glibc.sh \
  tools/check-release.sh

cc \
  -std=gnu11 \
  -D_GNU_SOURCE \
  -Wall \
  -Wextra \
  -Wformat=2 \
  -Wshadow \
  -Wstrict-prototypes \
  -fsyntax-only \
  ui/nxextract_ui.c

if [ -f ui/build/nxextract-ui ]; then
  tools/check-glibc.sh ui/build/nxextract-ui
fi

echo "NXExtract release checks passed"
