#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import uuid
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
NXEXTRACT = ROOT / "nxextract.py"


def load_module():
    spec = importlib.util.spec_from_file_location("nxextract_under_test", NXEXTRACT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


NX = load_module()


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def fake_elf(machine=183):
    data = bytearray(96)
    data[:4] = b"\x7fELF"
    data[4] = 1 if machine == 40 else 2
    data[5] = 1
    data[6] = 1
    struct.pack_into("<H", data, 16, 3)
    struct.pack_into("<H", data, 18, machine)
    return bytes(data) + b"NX-TEST-LIBRARY"


def plain_manifest(package, split=""):
    split_attribute = ' split="%s"' % split if split else ""
    return (
        '<?xml version="1.0" encoding="utf-8"?>'
        '<manifest package="%s"%s></manifest>' % (package, split_attribute)
    ).encode("utf-8")


def _utf8_string(value):
    encoded = value.encode("utf-8")
    assert len(value) < 128 and len(encoded) < 128
    return bytes((len(value), len(encoded))) + encoded + b"\0"


def binary_manifest(package, split=""):
    strings = ["manifest", "package", "split", package, split]
    payload = b""
    offsets = []
    for value in strings:
        offsets.append(len(payload))
        payload += _utf8_string(value)
    header_size = 28
    strings_start = header_size + len(strings) * 4
    pool_size = strings_start + len(payload)
    pool = struct.pack(
        "<HHIIIIII",
        0x0001,
        header_size,
        pool_size,
        len(strings),
        0,
        0x100,
        strings_start,
        0,
    )
    pool += struct.pack("<%dI" % len(offsets), *offsets) + payload

    attributes = []
    for name_index, value_index in ((1, 3), (2, 4)):
        attributes.append(
            struct.pack(
                "<IIIHBBI",
                0xFFFFFFFF,
                name_index,
                value_index,
                8,
                0,
                0x03,
                value_index,
            )
        )
    node_size = 16 + 20 + sum(len(value) for value in attributes)
    node = struct.pack("<HHIII", 0x0102, 16, node_size, 1, 0xFFFFFFFF)
    node += struct.pack(
        "<IIHHHHHH", 0xFFFFFFFF, 0, 20, 20, len(attributes), 0, 0, 0
    )
    node += b"".join(attributes)
    total = 8 + len(pool) + len(node)
    return struct.pack("<HHI", 0x0003, 8, total) + pool + node


def make_zip(path, entries):
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in entries.items():
            archive.writestr(name, data)


def zip_bytes(entries):
    with tempfile.NamedTemporaryFile(suffix=".zip") as stream:
        make_zip(Path(stream.name), entries)
        stream.seek(0)
        return stream.read()


def base_recipe(lib_data, assets, extra_rules=None, extra_commit=None, hooks=None):
    asset_bytes = sum(len(value) for value in assets.values())
    rules = [
        {
            "id": "native",
            "source": {
                "kind": "entry",
                "patterns": ["lib/{abi}/libgame.so"],
            },
            "destination": "lib/{abi}/libgame.so",
            "validate": {
                "type": "file",
                "size": len(lib_data),
                "sha256": sha256(lib_data),
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
                "exact_files": len(assets),
                "exact_bytes": asset_bytes,
                "required_paths": sorted(assets),
            },
        },
    ]
    if extra_rules:
        rules.extend(extra_rules)
    commit = ["lib/{abi}/libgame.so", "assets"]
    if extra_commit:
        commit.extend(extra_commit)
    return {
        "schema": 1,
        "id": "synthetic-port",
        "version": "test-1",
        "title": "SYNTHETIC PORT",
        "abi_order": ["arm64-v8a"],
        "input": {
            "search_dirs": ["gamedata", "."],
            "prefer_first_nonempty": True,
            "sniff_all_in_primary": True,
            "max_files": 64,
            "max_bundle_apks": 32,
            "max_member_bytes": 32 * 1024 * 1024,
            "max_bundle_bytes": 64 * 1024 * 1024,
        },
        "extract": rules,
        "validate": [
            {
                "path": "lib/{abi}/libgame.so",
                "type": "file",
                "sha256": sha256(lib_data),
                "elf_machine": "arm64-v8a",
            }
        ],
        "commit": commit,
        "hooks": hooks or [],
        "marker": ".synthetic-data.json",
        "space": {"safety_bytes": 0},
        "log": "test-extract.log",
        "ui_success_seconds": 0,
        "ui_error_seconds": 0,
    }


class NXExtractCase(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="nxextract-test-")
        self.game = Path(self.temporary.name) / "port"
        self.data = self.game / "gamedata"
        self.data.mkdir(parents=True)
        self.recipe_path = self.game / "extractor.json"
        self.lib = fake_elf()
        self.assets = {
            "readme.dat": b"asset-one",
            "levels/level01.bin": b"level-data" * 7,
        }

    def tearDown(self):
        self.temporary.cleanup()

    def write_recipe(self, recipe=None):
        if recipe is None:
            recipe = base_recipe(self.lib, self.assets)
        self.recipe_path.write_text(
            json.dumps(recipe, sort_keys=True, indent=2), encoding="utf-8"
        )
        return recipe

    def run_cli(self, command="install", inputs=None, expect=0, extra=None):
        argv = [
            sys.executable,
            str(NXEXTRACT),
            command,
            "--recipe",
            str(self.recipe_path),
            "--game-dir",
            str(self.game),
        ]
        if command in ("install", "plan"):
            argv.append("--quiet")
        if command == "install":
            argv += ["--ui", "none"]
        for value in inputs or []:
            argv += ["--input", str(value)]
        argv += extra or []
        result = subprocess.run(
            argv,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=30,
        )
        if result.returncode != expect:
            self.fail(
                "command returned %d, expected %d\nstdout:\n%s\nstderr:\n%s"
                % (result.returncode, expect, result.stdout, result.stderr)
            )
        return result

    def assert_payload(self):
        self.assertEqual(
            (self.game / "lib/arm64-v8a/libgame.so").read_bytes(), self.lib
        )
        for relative, data in self.assets.items():
            self.assertEqual((self.game / "assets" / relative).read_bytes(), data)

    def merged_entries(self, manifest=None):
        entries = {
            "AndroidManifest.xml": manifest or plain_manifest("org.nextos.synthetic"),
            "lib/arm64-v8a/libgame.so": self.lib,
        }
        entries.update({"assets/" + key: value for key, value in self.assets.items()})
        return entries

    def test_manifest_parser_handles_binary_axml(self):
        package, split = NX.parse_android_manifest(
            binary_manifest("org.nextos.binary", "config.arm64_v8a")
        )
        self.assertEqual(package, "org.nextos.binary")
        self.assertEqual(split, "config.arm64_v8a")

    def test_renamed_merged_apk_is_selected_by_content(self):
        self.write_recipe()
        source = self.data / ("renamed-" + uuid.uuid4().hex)
        make_zip(source, self.merged_entries(binary_manifest("org.nextos.synthetic")))
        self.run_cli()
        self.assert_payload()
        self.assertTrue(source.exists(), "the legal source must be preserved")
        self.run_cli(command="verify")

    def test_templated_elf_machine_selects_and_validates_armv7(self):
        self.lib = fake_elf(machine=40)
        recipe = base_recipe(self.lib, self.assets)
        recipe["abi_order"] = ["arm64-v8a", "armeabi-v7a"]
        recipe["extract"][0]["validate"]["elf_machine"] = "{abi}"
        recipe["validate"][0]["elf_machine"] = "{abi}"
        self.write_recipe(recipe)
        entries = {
            "AndroidManifest.xml": plain_manifest("org.nextos.synthetic"),
            "lib/armeabi-v7a/libgame.so": self.lib,
        }
        entries.update({"assets/" + key: value for key, value in self.assets.items()})
        source = self.data / ("abi-neutral-" + uuid.uuid4().hex + ".apk")
        make_zip(source, entries)

        self.run_cli()
        self.assertEqual(
            (self.game / "lib/armeabi-v7a/libgame.so").read_bytes(), self.lib
        )
        self.run_cli(command="verify")

    def test_loose_splits_are_grouped_by_manifest_package(self):
        self.write_recipe()
        base = self.data / ("one-" + uuid.uuid4().hex + ".apk")
        abi = self.data / ("two-" + uuid.uuid4().hex + ".apk")
        asset_entries = {
            "AndroidManifest.xml": binary_manifest("org.nextos.synthetic"),
        }
        asset_entries.update(
            {"assets/" + key: value for key, value in self.assets.items()}
        )
        make_zip(base, asset_entries)
        make_zip(
            abi,
            {
                "AndroidManifest.xml": binary_manifest(
                    "org.nextos.synthetic", "config.arm64_v8a"
                ),
                "lib/arm64-v8a/libgame.so": self.lib,
            },
        )
        self.run_cli()
        self.assert_payload()
        self.assertTrue(base.exists())
        self.assertTrue(abi.exists())

    def _bundle_case(self, extension):
        self.write_recipe()
        base_bytes = zip_bytes(
            {
                "AndroidManifest.xml": plain_manifest("org.nextos.synthetic"),
                **{"assets/" + key: value for key, value in self.assets.items()},
            }
        )
        split_bytes = zip_bytes(
            {
                "AndroidManifest.xml": plain_manifest(
                    "org.nextos.synthetic", "config.arm64_v8a"
                ),
                "lib/arm64-v8a/libgame.so": self.lib,
            }
        )
        bundle = self.data / ("completely-random-" + uuid.uuid4().hex + extension)
        make_zip(
            bundle,
            {
                "unknown/base-random.apk": base_bytes,
                "splits/no-fixed-name.apk": split_bytes,
                "metadata/info.json": b"{}",
            },
        )
        self.run_cli()
        self.assert_payload()
        self.assertTrue(bundle.exists())

    def test_apkm_bundle(self):
        self._bundle_case(".apkm")

    def test_apks_bundle(self):
        self._bundle_case(".apks")

    def test_xapk_bundle_with_direct_obb(self):
        obb = b"LPK\0" + b"xapk-obb-payload" * 11
        obb_rule = {
            "id": "obb",
            "source": {
                "kind": "entry",
                "patterns": ["*.obb", "**/*.obb"],
                "scopes": ["bundle"],
            },
            "destination": "data/game.obb",
            "validate": {
                "type": "file",
                "size": len(obb),
                "sha256": sha256(obb),
                "magic_ascii": "LPK\u0000",
            },
        }
        recipe = base_recipe(
            self.lib,
            self.assets,
            extra_rules=[obb_rule],
            extra_commit=["data/game.obb"],
        )
        self.write_recipe(recipe)
        merged = zip_bytes(self.merged_entries())
        bundle = self.data / ("export-" + uuid.uuid4().hex + ".xapk")
        make_zip(
            bundle,
            {
                "install/" + uuid.uuid4().hex + ".apk": merged,
                "Android/obb/org.nextos.synthetic/main.payload.obb": obb,
                "manifest.json": b"{}",
            },
        )
        self.run_cli()
        self.assert_payload()
        self.assertEqual((self.game / "data/game.obb").read_bytes(), obb)

    def test_loose_obb_is_chosen_by_hash_not_filename(self):
        obb = b"LPK\0" + os.urandom(128)
        obb_rule = {
            "id": "obb",
            "source": {
                "kind": "entry_or_file",
                "patterns": ["*"],
                "file_extensions": [".obb"],
            },
            "destination": "data/game.obb",
            "validate": {
                "size": len(obb),
                "sha256": sha256(obb),
                "magic_ascii": "LPK\u0000",
            },
        }
        self.write_recipe(
            base_recipe(
                self.lib,
                self.assets,
                extra_rules=[obb_rule],
                extra_commit=["data/game.obb"],
            )
        )
        make_zip(self.data / "base.apk", self.merged_entries())
        loose = self.data / (uuid.uuid4().hex + ".obb")
        loose.write_bytes(obb)
        self.run_cli()
        self.assertEqual((self.game / "data/game.obb").read_bytes(), obb)
        self.assertTrue(loose.exists())

    def test_second_run_uses_marker_without_source(self):
        self.write_recipe()
        source = self.data / "first.apk"
        make_zip(source, self.merged_entries())
        self.run_cli()
        parked = Path(self.temporary.name) / "parked-source"
        source.rename(parked)
        self.run_cli()
        self.assert_payload()

    def test_existing_data_rejection_logs_validation_reason(self):
        self.write_recipe()
        installed = self.game / "lib/arm64-v8a/libgame.so"
        installed.parent.mkdir(parents=True)
        installed.write_bytes(self.lib + b"-changed")
        self.run_cli(expect=1)
        log = (self.game / "test-extract.log").read_text(encoding="utf-8")
        self.assertIn(
            "existing data not adoptable for ABI arm64-v8a:",
            log,
        )
        self.assertIn(
            "native (lib/arm64-v8a/libgame.so) has unexpected size",
            log,
        )

    def test_rejected_candidates_are_reported_as_different_build(self):
        self.write_recipe()
        entries = self.merged_entries()
        entries["lib/arm64-v8a/libgame.so"] = self.lib + b"-other-build"
        make_zip(self.data / "other-build.apk", entries)
        self.run_cli(expect=1)
        log = (self.game / "test-extract.log").read_text(encoding="utf-8")
        self.assertIn(
            "required payload native was not found: 1 candidate(s) matched "
            "the source pattern but failed validation",
            log,
        )
        self.assertIn("probably a different build of the game", log)
        self.assertIn("lib/arm64-v8a/libgame.so", log)

    def test_valid_staged_file_is_resumed_without_rewrite(self):
        self.write_recipe()
        source = self.data / "resume.apk"
        make_zip(source, self.merged_entries())
        staged = (
            self.game
            / ".nxextract/synthetic-port/stage/lib/arm64-v8a/libgame.so"
        )
        staged.parent.mkdir(parents=True)
        staged.write_bytes(self.lib)
        old_time = time.time() - 3600
        os.utime(staged, (old_time, old_time))
        self.run_cli()
        self.assert_payload()
        log = (self.game / "test-extract.log").read_text(encoding="utf-8")
        self.assertIn("resuming", log)

    def test_failed_hook_preserves_previous_live_payload_and_source(self):
        recipe = base_recipe(
            self.lib,
            self.assets,
            hooks=[{"id": "intentional-failure", "argv": ["/bin/false"]}],
        )
        self.write_recipe(recipe)
        old = self.game / "assets/readme.dat"
        old.parent.mkdir(parents=True)
        old.write_bytes(b"old-live-data")
        source = self.data / "hook.apk"
        make_zip(source, self.merged_entries())
        self.run_cli(expect=1)
        self.assertEqual(old.read_bytes(), b"old-live-data")
        self.assertFalse((self.game / "lib/arm64-v8a/libgame.so").exists())
        self.assertTrue(source.exists())
        self.assertTrue(
            (
                self.game
                / ".nxextract/synthetic-port/stage/lib/arm64-v8a/libgame.so"
            ).exists()
        )

    def test_hook_checkpoint_expands_selected_abi(self):
        recipe = base_recipe(
            self.lib,
            self.assets,
            hooks=[
                {
                    "id": "abi-checkpoint",
                    "argv": ["/bin/true"],
                    "checkpoint": [
                        {
                            "path": "lib/{abi}/libgame.so",
                            "type": "file",
                            "sha256": sha256(self.lib),
                            "elf_machine": "arm64-v8a",
                        }
                    ],
                }
            ],
        )
        self.write_recipe(recipe)
        make_zip(self.data / "checkpoint.apk", self.merged_entries())
        self.run_cli()
        self.assert_payload()

    def test_recovery_rolls_back_an_interrupted_publish(self):
        workspace = self.game / ".nxextract/synthetic-port"
        live = self.game / "assets/readme.dat"
        backup = workspace / "backup/assets/readme.dat"
        cache = workspace / "source-cache/bundle/base.apk"
        live.parent.mkdir(parents=True)
        backup.parent.mkdir(parents=True)
        cache.parent.mkdir(parents=True)
        live.write_bytes(b"new-unpublished-data")
        backup.write_bytes(b"old-live-data")
        cache.write_bytes(b"cached-source")
        NX.atomic_write_json(
            workspace / "transaction.json",
            {
                "format": NX.FORMAT_VERSION,
                "transaction_id": "interrupted",
                "published": False,
                "paths": [
                    {
                        "path": "assets/readme.dat",
                        "backed_up": True,
                        "installed": True,
                    }
                ],
            },
        )

        logger = NX.Logger(None, verbose=False)
        NX.recover_transaction(
            str(self.game),
            str(workspace),
            str(self.game / ".synthetic-data.json"),
            logger,
        )

        self.assertEqual(live.read_bytes(), b"old-live-data")
        self.assertEqual(
            (workspace / "stage/assets/readme.dat").read_bytes(),
            b"new-unpublished-data",
        )
        self.assertEqual(cache.read_bytes(), b"cached-source")
        self.assertFalse((workspace / "backup").exists())
        self.assertFalse((workspace / "transaction.json").exists())

    def test_recovery_finishes_a_marker_published_transaction(self):
        workspace = self.game / ".nxextract/synthetic-port"
        marker = self.game / ".synthetic-data.json"
        for relative in ("stage/payload", "backup/payload", "source-cache/base.apk"):
            path = workspace / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"temporary")
        transaction_id = "marker-was-already-published"
        NX.atomic_write_json(
            workspace / "transaction.json",
            {
                "format": NX.FORMAT_VERSION,
                "transaction_id": transaction_id,
                "published": False,
                "paths": [],
            },
        )
        NX.atomic_write_json(marker, {"transaction_id": transaction_id})

        logger = NX.Logger(None, verbose=False)
        NX.recover_transaction(
            str(self.game), str(workspace), str(marker), logger
        )

        self.assertFalse((workspace / "stage").exists())
        self.assertFalse((workspace / "backup").exists())
        self.assertFalse((workspace / "source-cache").exists())
        self.assertFalse((workspace / "transaction.json").exists())
        self.assertTrue(marker.exists())

    def test_zip_slip_is_rejected_before_writing_payload(self):
        self.write_recipe()
        entries = self.merged_entries()
        entries["assets/../../escaped"] = b"bad"
        make_zip(self.data / "unsafe.apk", entries)
        self.run_cli(expect=1)
        self.assertFalse((self.game / "escaped").exists())
        self.assertFalse((Path(self.temporary.name) / "escaped").exists())

    def test_casefold_destination_collision_is_rejected(self):
        collision_assets = {
            "Foo.bin": b"first",
            "foo.bin": b"second",
        }
        self.assets = collision_assets
        self.write_recipe(base_recipe(self.lib, collision_assets))
        make_zip(self.data / "collision.apk", self.merged_entries())
        self.run_cli(expect=1)
        self.assertFalse((self.game / "assets").exists())

    def test_workspace_symlink_is_rejected(self):
        self.write_recipe()
        outside = Path(self.temporary.name) / "outside-workspace"
        outside.mkdir()
        (self.game / ".nxextract").symlink_to(outside, target_is_directory=True)
        make_zip(self.data / "payload.apk", self.merged_entries())
        self.run_cli(expect=1)
        self.assertEqual(list(outside.iterdir()), [])
        self.assertFalse((self.game / "assets").exists())

    def test_recipe_rejects_log_path_escape(self):
        recipe = base_recipe(self.lib, self.assets)
        recipe["log"] = "../escaped.log"
        self.write_recipe(recipe)
        make_zip(self.data / "payload.apk", self.merged_entries())
        self.run_cli(expect=1)
        self.assertFalse((Path(self.temporary.name) / "escaped.log").exists())

    def test_different_packages_are_never_merged(self):
        self.write_recipe()
        base_entries = {
            "AndroidManifest.xml": plain_manifest("org.nextos.one"),
            **{"assets/" + key: value for key, value in self.assets.items()},
        }
        make_zip(self.data / "one.apk", base_entries)
        make_zip(
            self.data / "two.apk",
            {
                "AndroidManifest.xml": plain_manifest(
                    "org.nextos.two", "config.arm64_v8a"
                ),
                "lib/arm64-v8a/libgame.so": self.lib,
            },
        )
        self.run_cli(expect=1)
        self.assertFalse((self.game / "assets").exists())

    def test_two_different_matching_bundles_are_ambiguous(self):
        recipe = base_recipe(self.lib, self.assets)
        recipe["extract"][0]["validate"].pop("sha256")
        recipe["extract"][0]["validate"].pop("size")
        recipe["validate"] = []
        self.write_recipe(recipe)
        for number in (1, 2):
            changed_lib = self.lib + bytes((number,))
            inner = zip_bytes(
                {
                    "AndroidManifest.xml": plain_manifest(
                        "org.nextos.synthetic%d" % number
                    ),
                    "lib/arm64-v8a/libgame.so": changed_lib,
                    **{
                        "assets/" + key: value
                        for key, value in self.assets.items()
                    },
                }
            )
            make_zip(
                self.data / ("bundle%d.apkm" % number),
                {"payload%d.apk" % number: inner},
            )
        self.run_cli(expect=1)

    def test_progress_protocol_is_atomic_and_parseable(self):
        target = Path(self.temporary.name) / "progress.txt"
        result = subprocess.run(
            [
                sys.executable,
                str(NXEXTRACT),
                "progress",
                "--file",
                str(target),
                "--phase",
                "5",
                "--overall",
                "700",
                "--phase-progress",
                "321",
                "--done-bytes",
                "123",
                "--total-bytes",
                "456",
                "--message",
                "BAKING TEXTURES",
                "--detail",
                "texture 7 / 20",
            ],
            check=False,
        )
        self.assertEqual(result.returncode, 0)
        lines = target.read_text(encoding="utf-8").splitlines()
        self.assertEqual(lines[0], "1 700 1000")
        self.assertEqual(lines[1], "BAKING TEXTURES")
        self.assertEqual(lines[2], "NXEXTRACT_V1 5 700 321 123 456")
        self.assertEqual(lines[3], "texture 7 / 20")


if __name__ == "__main__":
    unittest.main(verbosity=2)
