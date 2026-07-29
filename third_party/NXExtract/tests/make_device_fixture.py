#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Create a tiny, synthetic XAPK fixture for real-device integration tests."""

import argparse
import hashlib
import json
import os
import shutil
import struct
import zipfile
from pathlib import Path


def digest(data):
    return hashlib.sha256(data).hexdigest()


def fake_elf():
    data = bytearray(96)
    data[:4] = b"\x7fELF"
    data[4] = 2
    data[5] = 1
    data[6] = 1
    struct.pack_into("<H", data, 16, 3)
    struct.pack_into("<H", data, 18, 183)
    return bytes(data) + b"NXEXTRACT-DEVICE-TEST"


def manifest():
    return (
        b'<?xml version="1.0" encoding="utf-8"?>'
        b'<manifest package="org.nextos.nxextract.fixture"></manifest>'
    )


def make_zip(path, entries):
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in entries.items():
            archive.writestr(name, data)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output")
    args = parser.parse_args()
    output = Path(args.output).resolve()
    if output.exists():
        shutil.rmtree(output)
    game = output / "port"
    data_dir = game / "gamedata"
    data_dir.mkdir(parents=True)

    library = fake_elf()
    asset = b"synthetic-device-asset\n"
    obb = b"LPK\0" + b"synthetic-obb-data" * 16
    inner = output / "inner.tmp"
    make_zip(
        inner,
        {
            "AndroidManifest.xml": manifest(),
            "lib/arm64-v8a/libgame.so": library,
            "assets/data/content.bin": asset,
        },
    )
    source = data_dir / "nome-totalmente-livre.xapk"
    make_zip(
        source,
        {
            "splits/qualquer-coisa.apk": inner.read_bytes(),
            "Android/obb/org.nextos.nxextract.fixture/payload.obb": obb,
            "manifest.json": b"{}",
        },
    )
    inner.unlink()

    hook = game / "slow_hook.py"
    hook.write_text(
        """#!/usr/bin/env python3
import os
import pathlib
import sys
import time

stage = pathlib.Path(sys.argv[1])
for value in range(13):
    print("NXEXTRACT_PROGRESS %d 12 UI DEVICE TEST %d/12" % (value, value), flush=True)
    time.sleep(0.25)
(stage / "processed.ok").write_text("ok\\n", encoding="utf-8")
""",
        encoding="utf-8",
    )
    hook.chmod(0o755)

    recipe = {
        "schema": 1,
        "id": "device-fixture",
        "version": "1",
        "title": "UNIVERSAL EXTRACTOR TEST",
        "abi_order": ["arm64-v8a"],
        "input": {
            "search_dirs": ["gamedata", "."],
            "prefer_first_nonempty": True,
            "sniff_all_in_primary": True,
            "max_member_bytes": 8 * 1024 * 1024,
            "max_bundle_bytes": 16 * 1024 * 1024,
        },
        "extract": [
            {
                "id": "native",
                "source": {
                    "kind": "entry",
                    "patterns": ["lib/{abi}/libgame.so"],
                },
                "destination": "lib/{abi}/libgame.so",
                "validate": {
                    "size": len(library),
                    "sha256": digest(library),
                    "elf_machine": "arm64-v8a",
                },
            },
            {
                "id": "assets",
                "source": {
                    "kind": "entries",
                    "patterns": ["assets/**"],
                    "strip_prefix": "assets/",
                },
                "destination": "assets",
                "validate": {
                    "type": "tree",
                    "exact_files": 1,
                    "exact_bytes": len(asset),
                    "required_paths": ["data/content.bin"],
                },
            },
            {
                "id": "obb",
                "source": {
                    "kind": "entry",
                    "patterns": ["*.obb", "**/*.obb"],
                    "scopes": ["bundle"],
                },
                "destination": "data/game.obb",
                "validate": {
                    "size": len(obb),
                    "sha256": digest(obb),
                    "magic_ascii": "LPK\u0000",
                },
            },
        ],
        "hooks": [
            {
                "id": "synthetic-bake",
                "argv": ["python3", "{game_dir}/slow_hook.py", "{stage}"],
                "checkpoint": [
                    {
                        "path": "processed.ok",
                        "type": "file",
                        "size": 3,
                        "sha256": digest(b"ok\n"),
                    }
                ],
            }
        ],
        "validate": [
            {
                "path": "processed.ok",
                "type": "file",
                "size": 3,
                "sha256": digest(b"ok\n"),
            }
        ],
        "commit": [
            "lib/{abi}/libgame.so",
            "assets",
            "data/game.obb",
            "processed.ok",
        ],
        "marker": ".device-fixture.ok.json",
        "space": {"safety_bytes": 0},
        "log": "nxextract-device.log",
        "ui_success_seconds": 1,
        "ui_error_seconds": 0,
    }
    (game / "extractor.json").write_text(
        json.dumps(recipe, sort_keys=True, indent=2), encoding="utf-8"
    )
    print(game)


if __name__ == "__main__":
    main()
