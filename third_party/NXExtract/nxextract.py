#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""NXExtract universal, transactional Android data extractor.

The external filename is deliberately never used to identify a game. Inputs are
classified by their contents and a small per-port JSON recipe selects and validates
the required payload.
"""

import argparse
import binascii
import contextlib
import errno
import fcntl
import fnmatch
import hashlib
import json
import os
import platform
import re
import shutil
import signal
import stat
import struct
import subprocess
import sys
import time
import uuid
import zipfile
from pathlib import Path, PurePosixPath


NXEXTRACT_VERSION = "1.1.3"
FORMAT_VERSION = 1
CHUNK_SIZE = 1024 * 1024
DEFAULT_SAFETY_BYTES = 128 * 1024 * 1024
DEFAULT_EXTENSIONS = (
    ".apk",
    ".apkm",
    ".apks",
    ".xapk",
    ".zip",
    ".obb",
)
APK_EXTENSIONS = (".apk",)
BUNDLE_EXTENSIONS = (".apkm", ".apks", ".xapk")
PHASES = (
    "PREPARING",
    "SCANNING FILES",
    "VALIDATING PACKAGES",
    "SELECTING DATA",
    "EXTRACTING DATA",
    "PROCESSING DATA",
    "VALIDATING DATA",
    "INSTALLING DATA",
    "READY",
)
ELF_MACHINES = {
    "arm": 40,
    "armeabi": 40,
    "armeabi-v7a": 40,
    "aarch64": 183,
    "arm64": 183,
    "arm64-v8a": 183,
    "x86": 3,
    "x86_64": 62,
}


class NXError(Exception):
    """Expected, user-facing setup failure."""


class RecipeError(NXError):
    pass


class SourceError(NXError):
    pass


class PlanError(NXError):
    pass


class ValidationError(NXError):
    pass


def _json_no_duplicates(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise RecipeError("duplicate JSON key: %s" % key)
        result[key] = value
    return result


def load_json(path):
    try:
        with open(path, "r", encoding="utf-8") as stream:
            return json.load(stream, object_pairs_hook=_json_no_duplicates)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise RecipeError("cannot read JSON %s: %s" % (path, error))


def canonical_json(value):
    return json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")


def sha256_bytes(value):
    return hashlib.sha256(value).hexdigest()


def is_regular_file(path):
    try:
        return stat.S_ISREG(os.lstat(path).st_mode)
    except OSError:
        return False


def validate_relative_path(value, label="path", allow_dot=False):
    if not isinstance(value, str):
        raise RecipeError("%s must be a string" % label)
    if "\x00" in value or "\\" in value or any(ord(char) < 32 for char in value):
        raise RecipeError("unsafe %s: %r" % (label, value))
    if value == "." and allow_dot:
        return value
    if not value or value.startswith("/"):
        raise RecipeError("unsafe %s: %r" % (label, value))
    parts = value.split("/")
    if any(part in ("", ".", "..") for part in parts):
        raise RecipeError("unsafe %s: %r" % (label, value))
    return "/".join(parts)


def safe_zip_name(name, directory=False):
    if not isinstance(name, str):
        raise SourceError("ZIP entry name is not text")
    if "\x00" in name or "\\" in name or any(ord(char) < 32 for char in name):
        raise SourceError("unsafe ZIP entry name: %r" % name)
    if name.startswith("/"):
        raise SourceError("unsafe absolute ZIP entry: %r" % name)
    clean = name[:-1] if directory and name.endswith("/") else name
    if not clean:
        return ""
    parts = clean.split("/")
    if any(part in ("", ".", "..") or ":" in part for part in parts):
        raise SourceError("unsafe ZIP entry path: %r" % name)
    return "/".join(parts)


def safe_join(root, relative, label="destination"):
    relative = validate_relative_path(relative, label)
    root = os.path.realpath(root)
    destination = os.path.abspath(os.path.join(root, *relative.split("/")))
    try:
        inside = os.path.commonpath((root, destination)) == root
    except ValueError:
        inside = False
    if not inside:
        raise NXError("%s escapes its root: %s" % (label, relative))
    return destination


def ensure_no_symlink_parents(root, relative):
    root = os.path.realpath(root)
    current = root
    parts = validate_relative_path(relative).split("/")[:-1]
    for part in parts:
        current = os.path.join(current, part)
        try:
            mode = os.lstat(current).st_mode
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(mode) or not stat.S_ISDIR(mode):
            raise NXError("unsafe non-directory parent: %s" % current)


def remove_path(path):
    try:
        mode = os.lstat(path).st_mode
    except FileNotFoundError:
        return
    if stat.S_ISDIR(mode) and not stat.S_ISLNK(mode):
        shutil.rmtree(path)
    else:
        os.unlink(path)


def discard_path(path, logger=None, label=None):
    """Drop a scratch path without ever failing the run.

    FUSE-backed shares (exFAT on Knulli/Batocera, NFS, SMB) replace a file that
    is unlinked while still open with a hidden placeholder, so the parent
    directory can answer ENOTEMPTY even after every real entry is gone. Scratch
    space that survives one extra run is harmless; a committed payload reported
    as a failure is not.
    """
    try:
        remove_path(path)
        return True
    except OSError as error:
        failure = error
    shutil.rmtree(path, ignore_errors=True)
    if not os.path.lexists(path):
        return True
    if logger is not None:
        logger.log(
            "warning: kept %s for the next run (%s)" % (label or path, failure)
        )
    return False


def fsync_directory(path):
    try:
        descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    except OSError:
        return
    try:
        os.fsync(descriptor)
    except OSError:
        pass
    finally:
        os.close(descriptor)


def atomic_write(path, data, mode="w"):
    parent = os.path.dirname(path) or "."
    os.makedirs(parent, exist_ok=True)
    temporary = "%s.tmp.%d.%s" % (path, os.getpid(), uuid.uuid4().hex[:8])
    binary = "b" in mode
    try:
        kwargs = {} if binary else {"encoding": "utf-8", "newline": "\n"}
        with open(temporary, mode, **kwargs) as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        fsync_directory(parent)
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


def atomic_write_json(path, value):
    atomic_write(
        path,
        json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + "\n",
    )


def file_size(path):
    return os.stat(path, follow_symlinks=False).st_size


def file_crc32(path):
    value = 0
    with open(path, "rb") as stream:
        while True:
            block = stream.read(CHUNK_SIZE)
            if not block:
                return value & 0xFFFFFFFF
            value = binascii.crc32(block, value)


def file_sha256(path):
    value = hashlib.sha256()
    with open(path, "rb") as stream:
        while True:
            block = stream.read(CHUNK_SIZE)
            if not block:
                return value.hexdigest()
            value.update(block)


def human_bytes(value):
    value = int(value)
    units = ("B", "KiB", "MiB", "GiB", "TiB")
    amount = float(value)
    for unit in units:
        if amount < 1024.0 or unit == units[-1]:
            if unit == "B":
                return "%d %s" % (int(amount), unit)
            return "%.1f %s" % (amount, unit)
        amount /= 1024.0
    return "%d B" % value


def normalize_hash_list(value, name):
    if value is None:
        return ()
    values = value if isinstance(value, list) else [value]
    result = []
    for item in values:
        if not isinstance(item, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", item):
            raise RecipeError("%s must contain SHA-256 hex strings" % name)
        result.append(item.lower())
    return tuple(result)


def normalize_crc_list(value, name):
    if value is None:
        return ()
    values = value if isinstance(value, list) else [value]
    result = []
    for item in values:
        if isinstance(item, int):
            number = item
        elif isinstance(item, str) and re.fullmatch(r"[0-9a-fA-F]{1,8}", item):
            number = int(item, 16)
        else:
            raise RecipeError("%s must contain CRC32 values" % name)
        if number < 0 or number > 0xFFFFFFFF:
            raise RecipeError("%s CRC32 is out of range" % name)
        result.append(number)
    return tuple(result)


def parse_magic(spec):
    if "magic_hex" in spec and "magic_ascii" in spec:
        raise RecipeError("validation cannot contain both magic_hex and magic_ascii")
    if "magic_hex" in spec:
        value = spec["magic_hex"]
        if not isinstance(value, str) or len(value) % 2 or not re.fullmatch(
            r"[0-9a-fA-F]*", value
        ):
            raise RecipeError("magic_hex must be an even-length hexadecimal string")
        return bytes.fromhex(value)
    if "magic_ascii" in spec:
        value = spec["magic_ascii"]
        if not isinstance(value, str):
            raise RecipeError("magic_ascii must be a string")
        try:
            return value.encode("ascii")
        except UnicodeEncodeError:
            raise RecipeError("magic_ascii must contain only ASCII")
    return None


class Recipe:
    def __init__(self, path):
        self.path = os.path.realpath(path)
        self.root = os.path.dirname(self.path)
        self.data = load_json(self.path)
        self._validate()
        self.digest = sha256_bytes(canonical_json(self.data))

    def _validate(self):
        data = self.data
        if not isinstance(data, dict):
            raise RecipeError("recipe root must be a JSON object")
        if data.get("schema") != FORMAT_VERSION:
            raise RecipeError(
                "unsupported recipe schema %r (expected %d)"
                % (data.get("schema"), FORMAT_VERSION)
            )
        identifier = data.get("id")
        if not isinstance(identifier, str) or not re.fullmatch(
            r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}", identifier
        ):
            raise RecipeError("recipe id must be 1-64 safe characters")
        version = data.get("version")
        if not isinstance(version, (str, int)) or not str(version):
            raise RecipeError("recipe version is required")
        title = data.get("title", identifier)
        if not isinstance(title, str) or not title.strip():
            raise RecipeError("recipe title must be text")
        input_config = data.get("input", {})
        if not isinstance(input_config, dict):
            raise RecipeError("input must be an object")
        space = data.get("space", {})
        if not isinstance(space, dict):
            raise RecipeError("space must be an object")
        safety = space.get("safety_bytes", DEFAULT_SAFETY_BYTES)
        if (
            not isinstance(safety, int)
            or isinstance(safety, bool)
            or safety < 0
        ):
            raise RecipeError("space.safety_bytes must be a non-negative integer")
        log = data.get("log", "nxextract.log")
        validate_relative_path(log, "log path")
        for delay_name in ("ui_success_seconds", "ui_error_seconds"):
            delay = data.get(delay_name, 1 if delay_name == "ui_success_seconds" else 5)
            if (
                not isinstance(delay, (int, float))
                or isinstance(delay, bool)
                or delay < 0
                or delay > 300
            ):
                raise RecipeError("%s must be between 0 and 300" % delay_name)
        rules = data.get("extract")
        if not isinstance(rules, list) or not rules:
            raise RecipeError("recipe extract must be a non-empty list")
        seen = set()
        for index, rule in enumerate(rules):
            self._validate_rule(rule, index, seen)
        commit = data.get("commit")
        if not isinstance(commit, list) or not commit:
            raise RecipeError("recipe commit must be a non-empty list")
        normalized = []
        for index, item in enumerate(commit):
            if not isinstance(item, str):
                raise RecipeError("commit[%d] must be a string" % index)
            normalized.append(item)
        for left_index, left in enumerate(normalized):
            for right in normalized[left_index + 1 :]:
                left_plain = left.replace("{abi}", "ABI")
                right_plain = right.replace("{abi}", "ABI")
                if left_plain == right_plain:
                    raise RecipeError("duplicate commit path: %s" % left)
                if left_plain.startswith(right_plain + "/") or right_plain.startswith(
                    left_plain + "/"
                ):
                    raise RecipeError(
                        "overlapping commit paths are not allowed: %s / %s"
                        % (left, right)
                    )
        marker = data.get("marker", ".nxextract-%s.json" % identifier)
        validate_relative_path(marker, "marker")
        hooks = data.get("hooks", [])
        if not isinstance(hooks, list):
            raise RecipeError("hooks must be a list")
        hook_ids = set()
        for index, hook in enumerate(hooks):
            if not isinstance(hook, dict):
                raise RecipeError("hooks[%d] must be an object" % index)
            hook_id = hook.get("id")
            if not isinstance(hook_id, str) or not re.fullmatch(
                r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}", hook_id
            ):
                raise RecipeError("hooks[%d].id is invalid" % index)
            if hook_id in hook_ids:
                raise RecipeError("duplicate hook id: %s" % hook_id)
            hook_ids.add(hook_id)
            argv = hook.get("argv")
            if not isinstance(argv, list) or not argv or not all(
                isinstance(item, str) and item for item in argv
            ):
                raise RecipeError("hook %s argv must be a non-empty string list" % hook_id)
            checks = hook.get("checkpoint", [])
            if not isinstance(checks, list):
                raise RecipeError("hook %s checkpoint must be a list" % hook_id)
            for check in checks:
                self._validate_output_check(check, "hook %s checkpoint" % hook_id)
        checks = data.get("validate", [])
        if not isinstance(checks, list):
            raise RecipeError("validate must be a list")
        for check in checks:
            self._validate_output_check(check, "validate")

    def _validate_rule(self, rule, index, seen):
        if not isinstance(rule, dict):
            raise RecipeError("extract[%d] must be an object" % index)
        rule_id = rule.get("id")
        if not isinstance(rule_id, str) or not re.fullmatch(
            r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}", rule_id
        ):
            raise RecipeError("extract[%d].id is invalid" % index)
        if rule_id in seen:
            raise RecipeError("duplicate extract id: %s" % rule_id)
        seen.add(rule_id)
        source = rule.get("source")
        if not isinstance(source, dict):
            raise RecipeError("extract %s source must be an object" % rule_id)
        kind = source.get("kind")
        if kind not in ("entry", "entries", "file", "entry_or_file"):
            raise RecipeError("extract %s has unsupported source kind" % rule_id)
        patterns = source.get("patterns", ["*"])
        if not isinstance(patterns, list) or not patterns or not all(
            isinstance(item, str) and item for item in patterns
        ):
            raise RecipeError("extract %s patterns must be a non-empty string list" % rule_id)
        destination = rule.get("destination")
        if not isinstance(destination, str) or not destination:
            raise RecipeError("extract %s destination is required" % rule_id)
        if kind in ("entry", "file", "entry_or_file"):
            validate_relative_path(
                destination.replace("{abi}", "ABI").replace("{basename}", "FILE"),
                "extract %s destination" % rule_id,
            )
        else:
            validate_relative_path(
                destination.replace("{abi}", "ABI"),
                "extract %s destination" % rule_id,
            )
        strip_prefix = source.get("strip_prefix")
        if strip_prefix is not None and not isinstance(strip_prefix, str):
            raise RecipeError("extract %s strip_prefix must be text" % rule_id)
        validation = rule.get("validate", {})
        if not isinstance(validation, dict):
            raise RecipeError("extract %s validate must be an object" % rule_id)
        self._validate_validation(validation, "extract %s" % rule_id)

    def _validate_validation(self, spec, label):
        for key in (
            "size",
            "min_size",
            "max_size",
            "exact_files",
            "min_files",
            "max_files",
            "exact_bytes",
            "min_bytes",
            "max_bytes",
            "magic_offset",
        ):
            if key in spec and (
                not isinstance(spec[key], int) or isinstance(spec[key], bool) or spec[key] < 0
            ):
                raise RecipeError("%s %s must be a non-negative integer" % (label, key))
        normalize_hash_list(spec.get("sha256"), "%s sha256" % label)
        normalize_crc_list(spec.get("crc32"), "%s crc32" % label)
        parse_magic(spec)
        if "elf_machine" in spec:
            machine = str(spec["elf_machine"]).lower()
            if machine != "{abi}" and machine not in ELF_MACHINES:
                raise RecipeError("%s elf_machine is unsupported" % label)
        required = spec.get("required_paths", [])
        if not isinstance(required, list):
            raise RecipeError("%s required_paths must be a list" % label)
        for item in required:
            validate_relative_path(item, "%s required path" % label)

    def _validate_output_check(self, check, label):
        if not isinstance(check, dict) or not isinstance(check.get("path"), str):
            raise RecipeError("%s item must contain a path" % label)
        validate_relative_path(
            check["path"].replace("{abi}", "ABI"), "%s path" % label
        )
        self._validate_validation(check, label)

    @property
    def identifier(self):
        return self.data["id"]

    @property
    def title(self):
        return self.data.get("title", self.identifier)

    @property
    def version(self):
        return str(self.data["version"])

    @property
    def marker(self):
        return self.data.get("marker", ".nxextract-%s.json" % self.identifier)

    @property
    def input_config(self):
        value = self.data.get("input", {})
        if not isinstance(value, dict):
            raise RecipeError("input must be an object")
        return value

    def abi_order(self):
        values = self.data.get("abi_order")
        if values is None:
            machine = platform.machine().lower()
            if machine in ("aarch64", "arm64"):
                return ["arm64-v8a", "armeabi-v7a"]
            if machine.startswith("arm"):
                return ["armeabi-v7a", "armeabi"]
            if machine in ("x86_64", "amd64"):
                return ["x86_64", "x86"]
            return ["arm64-v8a", "armeabi-v7a", "x86_64", "x86"]
        if not isinstance(values, list) or not values or not all(
            isinstance(value, str) and value for value in values
        ):
            raise RecipeError("abi_order must be a non-empty string list")
        return values


class Progress:
    def __init__(self, path=None, logger=None):
        self.path = os.path.realpath(path) if path else None
        self.logger = logger
        self.state = 1
        self.phase = 0
        self.overall = 0
        self.phase_progress = 0
        self.done_bytes = 0
        self.total_bytes = 0
        self.message = "PREPARING"
        self.detail = ""
        self.last_write = 0.0
        self.last_tuple = None

    def update(
        self,
        phase=None,
        overall=None,
        phase_progress=None,
        done_bytes=None,
        total_bytes=None,
        message=None,
        detail=None,
        state=None,
        force=False,
    ):
        if phase is not None:
            self.phase = max(0, min(8, int(phase)))
        if overall is not None:
            self.overall = max(0, min(1000, int(overall)))
        if phase_progress is not None:
            self.phase_progress = max(0, min(1000, int(phase_progress)))
        if done_bytes is not None:
            self.done_bytes = max(0, int(done_bytes))
        if total_bytes is not None:
            self.total_bytes = max(0, int(total_bytes))
        if message is not None:
            self.message = " ".join(str(message).replace("\r", " ").replace("\n", " ").split())
        if detail is not None:
            self.detail = " ".join(str(detail).replace("\r", " ").replace("\n", " ").split())
        if state is not None:
            self.state = int(state)
        current = (
            self.state,
            self.phase,
            self.overall,
            self.phase_progress,
            self.done_bytes,
            self.total_bytes,
            self.message,
            self.detail,
        )
        now = time.monotonic()
        if not force and current == self.last_tuple:
            return
        if not force and now - self.last_write < 0.08:
            return
        self.last_tuple = current
        self.last_write = now
        if self.path:
            payload = (
                "%d %d 1000\n"
                "%s\n"
                "NXEXTRACT_V1 %d %d %d %d %d\n"
                "%s\n"
            ) % (
                self.state,
                self.overall,
                self.message or PHASES[self.phase],
                self.phase,
                self.overall,
                self.phase_progress,
                self.done_bytes,
                self.total_bytes,
                self.detail,
            )
            try:
                atomic_write(self.path, payload)
            except OSError:
                self.path = None

    def fail(self, message):
        self.update(state=2, message=message, force=True)

    def done(self, message="GAME DATA READY"):
        self.update(
            phase=8,
            overall=1000,
            phase_progress=1000,
            state=3,
            message=message,
            force=True,
        )


class Logger:
    def __init__(self, path=None, verbose=True):
        self.path = path
        self.verbose = verbose
        self.stream = None
        if path:
            os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
            self.stream = open(path, "a", encoding="utf-8", buffering=1)

    def log(self, message):
        line = "[%s] %s" % (time.strftime("%Y-%m-%d %H:%M:%S"), message)
        if self.verbose:
            print(line, flush=True)
        if self.stream:
            self.stream.write(line + "\n")

    def close(self):
        if self.stream:
            self.stream.close()
            self.stream = None


def _decode_length8(data, offset):
    first = data[offset]
    offset += 1
    if first & 0x80:
        second = data[offset]
        offset += 1
        return ((first & 0x7F) << 8) | second, offset
    return first, offset


def _decode_length16(data, offset):
    first = struct.unpack_from("<H", data, offset)[0]
    offset += 2
    if first & 0x8000:
        second = struct.unpack_from("<H", data, offset)[0]
        offset += 2
        return ((first & 0x7FFF) << 16) | second, offset
    return first, offset


class AndroidStringPool:
    UTF8_FLAG = 0x100

    def __init__(self, chunk):
        if len(chunk) < 28:
            raise ValueError("truncated Android string pool")
        chunk_type, header_size, chunk_size = struct.unpack_from("<HHI", chunk, 0)
        if chunk_type != 0x0001 or chunk_size > len(chunk) or header_size < 28:
            raise ValueError("invalid Android string pool")
        count, _styles, flags, strings_start, _styles_start = struct.unpack_from(
            "<IIIII", chunk, 8
        )
        if count > 1_000_000 or header_size + count * 4 > chunk_size:
            raise ValueError("unreasonable Android string pool")
        offsets = struct.unpack_from("<%dI" % count, chunk, header_size) if count else ()
        self.strings = []
        utf8 = bool(flags & self.UTF8_FLAG)
        for item in offsets:
            position = strings_start + item
            if position >= chunk_size:
                raise ValueError("Android string offset out of range")
            if utf8:
                _utf16_length, position = _decode_length8(chunk, position)
                byte_length, position = _decode_length8(chunk, position)
                end = position + byte_length
                if end > chunk_size:
                    raise ValueError("truncated Android UTF-8 string")
                text = chunk[position:end].decode("utf-8", "strict")
            else:
                char_length, position = _decode_length16(chunk, position)
                end = position + char_length * 2
                if end > chunk_size:
                    raise ValueError("truncated Android UTF-16 string")
                text = chunk[position:end].decode("utf-16le", "strict")
            self.strings.append(text)

    def get(self, index):
        if index == 0xFFFFFFFF:
            return None
        if index < 0 or index >= len(self.strings):
            raise ValueError("Android string index out of range")
        return self.strings[index]


def parse_android_manifest(data):
    """Return (package, split) from binary AXML or plain XML."""
    stripped = data.lstrip()
    if stripped.startswith(b"<"):
        import xml.etree.ElementTree as element_tree

        root = element_tree.fromstring(data)
        package_name = root.attrib.get("package")
        split = root.attrib.get("split", "")
        if not package_name:
            raise ValueError("plain Android manifest has no package")
        return package_name, split

    if len(data) < 8:
        raise ValueError("truncated Android manifest")
    xml_type, xml_header, xml_size = struct.unpack_from("<HHI", data, 0)
    if xml_type != 0x0003 or xml_header < 8 or xml_size > len(data):
        raise ValueError("not an Android binary XML document")
    pool = None
    offset = xml_header
    while offset + 8 <= xml_size:
        chunk_type, header_size, chunk_size = struct.unpack_from("<HHI", data, offset)
        if header_size < 8 or chunk_size < header_size or offset + chunk_size > xml_size:
            raise ValueError("invalid Android XML chunk")
        chunk = data[offset : offset + chunk_size]
        if chunk_type == 0x0001:
            pool = AndroidStringPool(chunk)
        elif chunk_type == 0x0102 and pool is not None:
            if header_size < 16 or len(chunk) < 36:
                raise ValueError("truncated Android start element")
            name_index = struct.unpack_from("<I", chunk, 20)[0]
            name = pool.get(name_index)
            if name != "manifest":
                offset += chunk_size
                continue
            attribute_start, attribute_size, attribute_count = struct.unpack_from(
                "<HHH", chunk, 24
            )
            if attribute_size < 20 or attribute_count > 4096:
                raise ValueError("invalid Android manifest attributes")
            base = 16 + attribute_start
            package_name = None
            split = ""
            for index in range(attribute_count):
                position = base + index * attribute_size
                if position + 20 > len(chunk):
                    raise ValueError("truncated Android manifest attribute")
                _namespace, attribute_name, raw_value = struct.unpack_from(
                    "<III", chunk, position
                )
                value_type = chunk[position + 15]
                value_data = struct.unpack_from("<I", chunk, position + 16)[0]
                key = pool.get(attribute_name)
                if raw_value != 0xFFFFFFFF:
                    value = pool.get(raw_value)
                elif value_type == 0x03:
                    value = pool.get(value_data)
                else:
                    value = str(value_data)
                if key == "package":
                    package_name = value
                elif key == "split":
                    split = value or ""
            if not package_name:
                raise ValueError("Android manifest has no package")
            return package_name, split
        offset += chunk_size
    raise ValueError("Android manifest root was not found")


class Archive:
    def __init__(self, path, kind, parent=None, label=None):
        self.path = os.path.realpath(path)
        self.kind = kind
        self.parent = os.path.realpath(parent) if parent else self.path
        self.label = label or os.path.basename(self.parent)
        self.zip = None
        self.members = {}
        self.package = None
        self.split = ""
        self._open()

    def _open(self):
        if not is_regular_file(self.path):
            raise SourceError("archive is missing, linked or not regular: %s" % self.path)
        try:
            self.zip = zipfile.ZipFile(self.path, "r")
            exact = set()
            for info in self.zip.infolist():
                name = safe_zip_name(info.filename, info.is_dir())
                if not name or info.is_dir():
                    continue
                if name in exact:
                    raise SourceError(
                        "duplicate ZIP entry in %s: %s" % (self.label, name)
                    )
                exact.add(name)
                self.members[name] = info
            manifest = self.members.get("AndroidManifest.xml")
            if manifest is not None:
                try:
                    with self.zip.open(manifest, "r") as stream:
                        data = stream.read(4 * 1024 * 1024 + 1)
                    if len(data) > 4 * 1024 * 1024:
                        raise ValueError("Android manifest is unreasonably large")
                    self.package, self.split = parse_android_manifest(data)
                except (OSError, RuntimeError, ValueError, zipfile.BadZipFile) as error:
                    raise SourceError(
                        "cannot identify Android manifest in %s: %s"
                        % (self.label, error)
                    )
        except Exception:
            self.close()
            raise

    def open_member(self, info):
        if info.flag_bits & 1:
            raise SourceError(
                "encrypted ZIP member is unsupported: %s in %s"
                % (info.filename, self.label)
            )
        mode = (info.external_attr >> 16) & 0xFFFF
        if stat.S_ISLNK(mode):
            raise SourceError(
                "symbolic-link ZIP member is unsupported: %s in %s"
                % (info.filename, self.label)
            )
        return self.zip.open(info, "r")

    def close(self):
        if self.zip is not None:
            try:
                self.zip.close()
            except Exception:
                pass
            self.zip = None


class LooseFile:
    def __init__(self, path):
        self.path = os.path.realpath(path)
        self.label = os.path.basename(path)
        if not is_regular_file(self.path):
            raise SourceError("input is missing, linked or not regular: %s" % path)


class CandidateGroup:
    def __init__(self, label, archives, loose, package=None, source_kind="loose"):
        self.label = label
        self.archives = archives
        self.loose = loose
        self.package = package
        self.source_kind = source_kind

    def description(self):
        if self.package:
            return "%s (package %s)" % (self.label, self.package)
        return self.label


class Discovery:
    def __init__(self):
        self.apks = []
        self.bundles = []
        self.generic_archives = []
        self.loose = []
        self.skipped = []

    def all_paths(self):
        return self.apks + self.bundles + self.generic_archives + self.loose


def zip_classification(path):
    try:
        with zipfile.ZipFile(path, "r") as archive:
            regular = [info for info in archive.infolist() if not info.is_dir()]
            names = {info.filename for info in regular}
            if "AndroidManifest.xml" in names:
                return "apk"
            inner_apks = [
                info
                for info in regular
                if PurePosixPath(info.filename).suffix.lower() == ".apk"
            ]
            if inner_apks:
                return "bundle"
            return "archive"
    except (OSError, zipfile.BadZipFile, RuntimeError, NotImplementedError):
        return None


def discover_inputs(recipe, game_dir, explicit_inputs, logger):
    config = recipe.input_config
    extensions = config.get("extensions", list(DEFAULT_EXTENSIONS))
    if not isinstance(extensions, list) or not all(
        isinstance(item, str) and item.startswith(".") for item in extensions
    ):
        raise RecipeError("input.extensions must be a list of .extensions")
    extensions = {item.lower() for item in extensions}
    for rule in recipe.data["extract"]:
        source = rule["source"]
        for item in source.get("file_extensions", []):
            if not isinstance(item, str) or not item.startswith("."):
                raise RecipeError("file_extensions values must begin with a dot")
            extensions.add(item.lower())
    maximum = int(config.get("max_files", 128))
    if maximum < 1 or maximum > 4096:
        raise RecipeError("input.max_files must be between 1 and 4096")
    paths = []
    if explicit_inputs:
        for value in explicit_inputs:
            candidate = os.path.realpath(value)
            if not is_regular_file(candidate):
                raise SourceError("explicit input is not a regular file: %s" % value)
            paths.append(candidate)
    else:
        search_dirs = config.get("search_dirs", ["gamedata", "."])
        if not isinstance(search_dirs, list) or not search_dirs:
            raise RecipeError("input.search_dirs must be a non-empty list")
        prefer_first = bool(config.get("prefer_first_nonempty", True))
        sniff_primary = bool(config.get("sniff_all_in_primary", True))
        for directory_index, relative in enumerate(search_dirs):
            if relative == ".":
                directory = game_dir
            else:
                validate_relative_path(relative, "input search directory")
                directory = safe_join(game_dir, relative, "input search directory")
            if not os.path.isdir(directory) or os.path.islink(directory):
                continue
            found_here = []
            for name in sorted(os.listdir(directory), key=lambda item: item.casefold()):
                candidate = os.path.join(directory, name)
                if not is_regular_file(candidate):
                    continue
                suffix = Path(name).suffix.lower()
                if suffix in extensions or (directory_index == 0 and sniff_primary):
                    found_here.append(os.path.realpath(candidate))
            if found_here:
                paths.extend(found_here)
                if prefer_first:
                    break
    unique = []
    seen = set()
    for path in paths:
        if path in seen:
            continue
        seen.add(path)
        unique.append(path)
    if len(unique) > maximum:
        raise SourceError(
            "too many candidate input files (%d; maximum %d)" % (len(unique), maximum)
        )

    result = Discovery()
    for path in unique:
        classification = zip_classification(path) if zipfile.is_zipfile(path) else None
        if classification == "apk":
            result.apks.append(path)
        elif classification == "bundle":
            result.bundles.append(path)
        elif classification == "archive":
            result.generic_archives.append(path)
        else:
            suffix = Path(path).suffix.lower()
            if suffix in extensions:
                result.loose.append(path)
            else:
                result.skipped.append(path)
    logger.log(
        "content scan: %d APK, %d bundle, %d companion archive, %d loose file"
        % (
            len(result.apks),
            len(result.bundles),
            len(result.generic_archives),
            len(result.loose),
        )
    )
    return result


def _bundle_cache_token(path):
    info = os.stat(path, follow_symlinks=False)
    identity = "%s\0%d\0%d" % (
        os.path.realpath(path),
        info.st_size,
        getattr(info, "st_mtime_ns", int(info.st_mtime * 1_000_000_000)),
    )
    return sha256_bytes(identity.encode("utf-8"))[:20]


def _copy_zip_member_resume(archive, info, destination, max_member_bytes):
    if info.file_size <= 0 or info.file_size > max_member_bytes:
        raise SourceError(
            "inner APK has unsafe size %d: %s" % (info.file_size, info.filename)
        )
    if is_regular_file(destination):
        try:
            if file_size(destination) == info.file_size and file_crc32(destination) == info.CRC:
                return
        except OSError:
            pass
    os.makedirs(os.path.dirname(destination), exist_ok=True)
    temporary = destination + ".part"
    try:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        value = 0
        written = 0
        if info.flag_bits & 1:
            raise SourceError("encrypted inner APK is unsupported: %s" % info.filename)
        with archive.open(info, "r") as source, open(temporary, "xb") as output:
            while True:
                block = source.read(CHUNK_SIZE)
                if not block:
                    break
                output.write(block)
                value = binascii.crc32(block, value)
                written += len(block)
            output.flush()
            os.fsync(output.fileno())
        if written != info.file_size or (value & 0xFFFFFFFF) != info.CRC:
            raise SourceError("inner APK failed size/CRC validation: %s" % info.filename)
        os.replace(temporary, destination)
        fsync_directory(os.path.dirname(destination))
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


def _check_free_space(path, required, label):
    available = shutil.disk_usage(path).free
    if available < required:
        raise SourceError(
            "not enough free space for %s: need %s, have %s"
            % (label, human_bytes(required), human_bytes(available))
        )


def build_candidate_groups(recipe, discovery, workspace, logger, progress):
    config = recipe.input_config
    max_bundle_apks = int(config.get("max_bundle_apks", 128))
    max_member_bytes = int(config.get("max_member_bytes", 8 * 1024**3))
    max_bundle_bytes = int(config.get("max_bundle_bytes", 16 * 1024**3))
    if max_bundle_apks < 1 or max_bundle_apks > 4096:
        raise RecipeError("input.max_bundle_apks is out of range")
    if max_member_bytes < 1 or max_bundle_bytes < 1:
        raise RecipeError("input bundle byte limits must be positive")
    safety = int(recipe.data.get("space", {}).get("safety_bytes", DEFAULT_SAFETY_BYTES))
    cache_root = os.path.join(workspace, "source-cache")
    os.makedirs(cache_root, exist_ok=True)
    loose = [LooseFile(path) for path in discovery.loose]
    generic = [
        Archive(path, "bundle", label=os.path.basename(path))
        for path in discovery.generic_archives
    ]
    groups = []
    opened = list(generic)

    for bundle_index, path in enumerate(discovery.bundles):
        progress.update(
            phase=2,
            overall=80,
            phase_progress=0,
            message="PREPARING APK BUNDLE",
            detail=os.path.basename(path),
            force=True,
        )
        outer = Archive(path, "bundle", label=os.path.basename(path))
        opened.append(outer)
        members = [
            info
            for name, info in outer.members.items()
            if PurePosixPath(name).suffix.lower() == ".apk"
        ]
        if not members:
            raise SourceError("bundle contains no APK members: %s" % outer.label)
        if len(members) > max_bundle_apks:
            raise SourceError(
                "bundle contains too many APK members (%d): %s"
                % (len(members), outer.label)
            )
        expanded_bytes = sum(info.file_size for info in members)
        if expanded_bytes > max_bundle_bytes:
            raise SourceError(
                "bundle APK payload exceeds safety limit: %s" % outer.label
            )
        bundle_cache = os.path.join(cache_root, "bundle-" + _bundle_cache_token(path))
        os.makedirs(bundle_cache, exist_ok=True)
        cached_bytes = 0
        for name in os.listdir(bundle_cache):
            candidate = os.path.join(bundle_cache, name)
            if is_regular_file(candidate):
                cached_bytes += file_size(candidate)
        missing = max(0, expanded_bytes - cached_bytes)
        _check_free_space(workspace, missing + safety, "APK bundle expansion")
        inner_archives = []
        for member_index, info in enumerate(members):
            token = sha256_bytes(info.filename.encode("utf-8"))[:12]
            destination = os.path.join(
                bundle_cache, "%03d-%s.apk" % (member_index, token)
            )
            _copy_zip_member_resume(outer.zip, info, destination, max_member_bytes)
            inner = Archive(
                destination,
                "apk",
                parent=path,
                label="%s:%s" % (outer.label, info.filename),
            )
            if not inner.package:
                raise SourceError("inner APK has no Android package: %s" % inner.label)
            inner_archives.append(inner)
            opened.append(inner)
        packages = sorted({item.package for item in inner_archives if item.package})
        package_name = packages[0] if len(packages) == 1 else None
        if len(packages) > 1:
            raise SourceError(
                "bundle mixes multiple Android packages: %s" % ", ".join(packages)
            )
        groups.append(
            CandidateGroup(
                outer.label,
                inner_archives + [outer] + generic,
                loose,
                package_name,
                "bundle",
            )
        )
        logger.log(
            "bundle %s: %d APK(s), package=%s"
            % (outer.label, len(inner_archives), package_name or "unknown")
        )

    direct_by_package = {}
    unknown_direct = []
    for path in discovery.apks:
        archive = Archive(path, "apk", label=os.path.basename(path))
        opened.append(archive)
        if archive.package:
            direct_by_package.setdefault(archive.package, []).append(archive)
        else:
            unknown_direct.append(archive)
    for package_name, archives in sorted(direct_by_package.items()):
        label = "loose APK set (%s)" % package_name
        groups.append(
            CandidateGroup(
                label,
                archives + generic,
                loose,
                package_name,
                "apk-set",
            )
        )
        logger.log("APK set %s: %d split(s)" % (package_name, len(archives)))
    if unknown_direct:
        groups.append(
            CandidateGroup(
                "unidentified loose APK set",
                unknown_direct + generic,
                loose,
                None,
                "apk-set",
            )
        )
    if not groups and (generic or loose):
        groups.append(
            CandidateGroup("companion data", generic, loose, None, "companion")
        )
    if not groups:
        for archive in opened:
            archive.close()
        raise SourceError(
            "no APK, APKM, APKS, XAPK, companion archive or loose payload was found"
        )
    return groups, opened


class SourceItem:
    def __init__(self, rule_id, destination, archive=None, info=None, loose=None):
        self.rule_id = rule_id
        self.destination = destination
        self.archive = archive
        self.info = info
        self.loose = loose
        if info is not None:
            self.size = info.file_size
            self.crc = info.CRC
            self.source_name = info.filename
            self.source_label = archive.label
        else:
            self.size = file_size(loose.path)
            self.crc = None
            self.source_name = loose.label
            self.source_label = loose.label

    def identity(self):
        return (
            self.rule_id,
            self.destination.casefold(),
            self.size,
            self.crc,
            self.source_name,
        )


class Plan:
    def __init__(self, group, abi, items, commit_paths):
        self.group = group
        self.abi = abi
        self.items = items
        self.commit_paths = commit_paths
        fingerprint = [
            (item.rule_id, item.destination, item.size, item.crc)
            for item in sorted(items, key=lambda value: value.destination.casefold())
        ]
        self.fingerprint = sha256_bytes(canonical_json(fingerprint))

    @property
    def total_bytes(self):
        return sum(item.size for item in self.items)


def template_value(value, abi, basename=None):
    mapping = {
        "abi": abi,
        "basename": basename or "",
    }
    try:
        return value.format(**mapping)
    except (KeyError, ValueError) as error:
        raise RecipeError("invalid template %r: %s" % (value, error))


def member_matches(name, pattern, case_sensitive=True):
    if case_sensitive:
        return fnmatch.fnmatchcase(name, pattern)
    return fnmatch.fnmatchcase(name.casefold(), pattern.casefold())


def _read_magic_from_member(archive, info, offset, length):
    with archive.open_member(info) as stream:
        if offset:
            remaining = offset
            while remaining:
                block = stream.read(min(CHUNK_SIZE, remaining))
                if not block:
                    return b""
                remaining -= len(block)
        return stream.read(length)


def _sha256_member(archive, info):
    digest = hashlib.sha256()
    with archive.open_member(info) as stream:
        while True:
            block = stream.read(CHUNK_SIZE)
            if not block:
                return digest.hexdigest()
            digest.update(block)


def _elf_machine_from_header(header):
    if len(header) < 20 or header[:4] != b"\x7fELF":
        return None
    if header[5] == 1:
        return struct.unpack_from("<H", header, 18)[0]
    if header[5] == 2:
        return struct.unpack_from(">H", header, 18)[0]
    return None


def _expected_elf_machine(spec, abi):
    machine = str(spec["elf_machine"]).lower()
    if machine == "{abi}":
        if not abi:
            raise RecipeError("elf_machine {abi} requires a resolved ABI")
        machine = str(abi).lower()
    try:
        return ELF_MACHINES[machine]
    except KeyError:
        raise RecipeError("elf_machine is unsupported for ABI %s" % machine)


def validate_member_candidate(archive, info, spec, abi=None):
    if not _size_valid(info.file_size, spec):
        return False
    crc_values = normalize_crc_list(spec.get("crc32"), "crc32")
    if crc_values and info.CRC not in crc_values:
        return False
    magic = parse_magic(spec)
    offset = int(spec.get("magic_offset", 0))
    if magic is not None:
        try:
            if _read_magic_from_member(archive, info, offset, len(magic)) != magic:
                return False
        except (OSError, RuntimeError, zipfile.BadZipFile):
            return False
    if "elf_machine" in spec:
        try:
            header = _read_magic_from_member(archive, info, 0, 64)
        except (OSError, RuntimeError, zipfile.BadZipFile):
            return False
        expected = _expected_elf_machine(spec, abi)
        if _elf_machine_from_header(header) != expected:
            return False
    hashes = normalize_hash_list(spec.get("sha256"), "sha256")
    if hashes:
        try:
            if _sha256_member(archive, info) not in hashes:
                return False
        except (OSError, RuntimeError, zipfile.BadZipFile):
            return False
    return True


def validate_loose_candidate(loose, spec, abi=None):
    try:
        size = file_size(loose.path)
    except OSError:
        return False
    if not _size_valid(size, spec):
        return False
    magic = parse_magic(spec)
    offset = int(spec.get("magic_offset", 0))
    if magic is not None:
        try:
            with open(loose.path, "rb") as stream:
                stream.seek(offset)
                if stream.read(len(magic)) != magic:
                    return False
        except OSError:
            return False
    if "elf_machine" in spec:
        try:
            with open(loose.path, "rb") as stream:
                header = stream.read(64)
        except OSError:
            return False
        expected = _expected_elf_machine(spec, abi)
        if _elf_machine_from_header(header) != expected:
            return False
    crc_values = normalize_crc_list(spec.get("crc32"), "crc32")
    if crc_values and file_crc32(loose.path) not in crc_values:
        return False
    hashes = normalize_hash_list(spec.get("sha256"), "sha256")
    if hashes and file_sha256(loose.path) not in hashes:
        return False
    return True


def _size_valid(size, spec):
    if "size" in spec and size != spec["size"]:
        return False
    if "min_size" in spec and size < spec["min_size"]:
        return False
    if "max_size" in spec and size > spec["max_size"]:
        return False
    return True


def validate_summary(count, total, spec):
    if "exact_files" in spec and count != spec["exact_files"]:
        return False
    if "min_files" in spec and count < spec["min_files"]:
        return False
    if "max_files" in spec and count > spec["max_files"]:
        return False
    if "exact_bytes" in spec and total != spec["exact_bytes"]:
        return False
    if "min_bytes" in spec and total < spec["min_bytes"]:
        return False
    if "max_bytes" in spec and total > spec["max_bytes"]:
        return False
    return True


def _source_scopes(source):
    scopes = source.get("scopes", ["apk", "bundle"])
    if not isinstance(scopes, list) or not all(
        item in ("apk", "bundle") for item in scopes
    ):
        raise RecipeError("source scopes must contain apk and/or bundle")
    return scopes


def _candidate_members(group, source, abi):
    patterns = [template_value(value, abi) for value in source.get("patterns", ["*"])]
    scopes = _source_scopes(source)
    case_sensitive = bool(source.get("case_sensitive", True))
    by_pattern = []
    for pattern in patterns:
        matches = []
        for archive in group.archives:
            if archive.kind not in scopes:
                continue
            for name, info in archive.members.items():
                if member_matches(name, pattern, case_sensitive):
                    matches.append((archive, info, name))
        by_pattern.append((pattern, matches))
    return by_pattern


def _candidate_loose(group, source, abi):
    patterns = [template_value(value, abi) for value in source.get("patterns", ["*"])]
    case_sensitive = bool(source.get("case_sensitive", False))
    extensions = source.get("file_extensions", [])
    extensions = {item.lower() for item in extensions}
    matches = []
    for loose in group.loose:
        if extensions and Path(loose.path).suffix.lower() not in extensions:
            continue
        name = loose.label
        if any(member_matches(name, pattern, case_sensitive) for pattern in patterns):
            matches.append(loose)
    return matches


def _choose_one(rule, group, abi):
    source = rule["source"]
    spec = rule.get("validate", {})
    candidates = []
    rejected = 0
    rejected_example = None
    if source["kind"] in ("entry", "entry_or_file"):
        for _pattern, matches in _candidate_members(group, source, abi):
            valid = [
                (archive, info, name)
                for archive, info, name in matches
                if validate_member_candidate(archive, info, spec, abi)
            ]
            invalid = [item for item in matches if item not in valid]
            rejected += len(invalid)
            if invalid and rejected_example is None:
                archive, _info, name = invalid[0]
                rejected_example = "%s:%s" % (archive.label, name)
            if valid:
                candidates.extend(valid)
                break
    loose_candidates = []
    if source["kind"] in ("file", "entry_or_file"):
        loose_all = _candidate_loose(group, source, abi)
        loose_candidates = [
            loose
            for loose in loose_all
            if validate_loose_candidate(loose, spec, abi)
        ]
        loose_invalid = [item for item in loose_all if item not in loose_candidates]
        rejected += len(loose_invalid)
        if loose_invalid and rejected_example is None:
            rejected_example = loose_invalid[0].label
    identities = {}
    for archive, info, name in candidates:
        key = (info.file_size, info.CRC)
        identities.setdefault(key, []).append((archive, info, name))
    for loose in loose_candidates:
        key = (file_size(loose.path), file_crc32(loose.path))
        identities.setdefault(key, []).append(loose)
    if not identities:
        if rule.get("required", True):
            if rejected:
                raise PlanError(
                    "required payload %s was not found: %d candidate(s) matched "
                    "the source pattern but failed validation "
                    "(size/sha256/crc32/ELF), e.g. %s; the input is probably a "
                    "different build of the game"
                    % (rule["id"], rejected, rejected_example)
                )
            raise PlanError("required payload %s was not found" % rule["id"])
        return None
    if len(identities) > 1:
        raise PlanError(
            "payload %s is ambiguous (%d different matching files)"
            % (rule["id"], len(identities))
        )
    selected = next(iter(identities.values()))[0]
    if isinstance(selected, LooseFile):
        basename = selected.label
        destination = template_value(rule["destination"], abi, basename)
        validate_relative_path(destination, "destination")
        return SourceItem(rule["id"], destination, loose=selected)
    archive, info, name = selected
    basename = PurePosixPath(name).name
    destination = template_value(rule["destination"], abi, basename)
    validate_relative_path(destination, "destination")
    return SourceItem(rule["id"], destination, archive=archive, info=info)


def _choose_many(rule, group, abi):
    source = rule["source"]
    matches = []
    seen_source = set()
    for _pattern, pattern_matches in _candidate_members(group, source, abi):
        for archive, info, name in pattern_matches:
            key = (archive.path, name)
            if key in seen_source:
                continue
            seen_source.add(key)
            matches.append((archive, info, name))
    if not matches:
        if rule.get("required", True):
            raise PlanError("required payload tree %s was not found" % rule["id"])
        return []
    strip_prefix = source.get("strip_prefix", "")
    strip_prefix = template_value(strip_prefix, abi) if strip_prefix else ""
    if strip_prefix and not strip_prefix.endswith("/"):
        strip_prefix += "/"
    flatten = bool(source.get("flatten", False))
    destination_root = template_value(rule["destination"], abi)
    validate_relative_path(destination_root, "destination")
    destinations = {}
    for archive, info, name in matches:
        if strip_prefix:
            if not name.startswith(strip_prefix):
                continue
            relative = name[len(strip_prefix) :]
        else:
            relative = PurePosixPath(name).name if flatten else name
        if flatten:
            relative = PurePosixPath(relative).name
        if not relative:
            continue
        safe_zip_name(relative)
        destination = "%s/%s" % (destination_root.rstrip("/"), relative)
        validate_relative_path(destination, "destination")
        key = destination.casefold()
        previous = destinations.get(key)
        if previous is not None:
            if (
                previous.destination != destination
                or previous.size != info.file_size
                or previous.crc != info.CRC
            ):
                raise PlanError(
                    "payload %s has a conflicting destination: %s"
                    % (rule["id"], destination)
                )
            continue
        destinations[key] = SourceItem(
            rule["id"], destination, archive=archive, info=info
        )
    items = sorted(destinations.values(), key=lambda item: item.destination.casefold())
    if not validate_summary(
        len(items), sum(item.size for item in items), rule.get("validate", {})
    ):
        raise PlanError("payload tree %s failed count/size validation" % rule["id"])
    return items


def _expand_commit_paths(recipe, abi):
    paths = []
    for value in recipe.data["commit"]:
        path = template_value(value, abi)
        validate_relative_path(path, "commit path")
        paths.append(path)
    for index, left in enumerate(paths):
        for right in paths[index + 1 :]:
            if left == right or left.startswith(right + "/") or right.startswith(left + "/"):
                raise RecipeError("expanded commit paths overlap: %s / %s" % (left, right))
    return paths


def _under_any_commit(destination, commit_paths):
    return any(
        destination == root or destination.startswith(root.rstrip("/") + "/")
        for root in commit_paths
    )


def build_plan_for(recipe, group, abi):
    commit_paths = _expand_commit_paths(recipe, abi)
    items = []
    destinations = {}
    for rule in recipe.data["extract"]:
        kind = rule["source"]["kind"]
        selected = _choose_many(rule, group, abi) if kind == "entries" else _choose_one(
            rule, group, abi
        )
        selected_items = selected if isinstance(selected, list) else ([selected] if selected else [])
        for item in selected_items:
            if not _under_any_commit(item.destination, commit_paths):
                raise RecipeError(
                    "destination %s is outside recipe commit roots" % item.destination
                )
            key = item.destination.casefold()
            previous = destinations.get(key)
            if previous is not None:
                if previous.identity() != item.identity():
                    raise PlanError("two rules write conflicting path %s" % item.destination)
                continue
            destinations[key] = item
            items.append(item)
    if not items:
        raise PlanError("recipe selected no payload")
    return Plan(group, abi, items, commit_paths)


def resolve_plan(recipe, groups, abi_override, logger, progress):
    abis = [abi_override] if abi_override else recipe.abi_order()
    successes = []
    failures = []
    progress.update(
        phase=3,
        overall=170,
        phase_progress=0,
        message="SELECTING GAME DATA BY CONTENT",
        force=True,
    )
    attempts = max(1, len(groups) * len(abis))
    attempt = 0
    for group in groups:
        for abi in abis:
            attempt += 1
            progress.update(
                phase_progress=attempt * 1000 // attempts,
                detail="%s | ABI %s" % (group.description(), abi),
            )
            try:
                plan = build_plan_for(recipe, group, abi)
            except (PlanError, ValidationError) as error:
                failures.append("%s / %s: %s" % (group.description(), abi, error))
                continue
            successes.append(plan)
    if not successes:
        detail = "; ".join(failures[:8])
        raise PlanError("no input set matches this recipe%s" % (": " + detail if detail else ""))
    by_fingerprint = {}
    for plan in successes:
        by_fingerprint.setdefault(plan.fingerprint, []).append(plan)
    if len(by_fingerprint) > 1:
        descriptions = [
            "%s / ABI %s" % (plan.group.description(), plan.abi)
            for plan in successes[:8]
        ]
        raise PlanError(
            "multiple different payload sets match; keep one version or pass --input: %s"
            % "; ".join(descriptions)
        )
    equivalent = next(iter(by_fingerprint.values()))
    abi_rank = {abi: index for index, abi in enumerate(abis)}
    source_rank = {"apk-set": 0, "bundle": 1, "companion": 2}
    equivalent.sort(
        key=lambda plan: (
            abi_rank.get(plan.abi, 999),
            source_rank.get(plan.group.source_kind, 9),
            plan.group.description(),
        )
    )
    selected = equivalent[0]
    if len(equivalent) > 1:
        logger.log(
            "found %d equivalent sources; selected %s"
            % (len(equivalent), selected.group.description())
        )
    logger.log(
        "selected %s, ABI %s, %d files, %s"
        % (
            selected.group.description(),
            selected.abi,
            len(selected.items),
            human_bytes(selected.total_bytes),
        )
    )
    return selected


def _validation_paths_for_rule(recipe, rule, abi, plan=None, marker=None):
    kind = rule["source"]["kind"]
    if kind == "entries":
        return [template_value(rule["destination"], abi)]
    if plan is not None:
        return [
            item.destination for item in plan.items if item.rule_id == rule["id"]
        ]
    if marker is not None:
        return [
            item["destination"]
            for item in marker.get("items", [])
            if item.get("rule") == rule["id"]
            and isinstance(item.get("destination"), str)
        ]
    if "{basename}" in rule["destination"]:
        return []
    return [template_value(rule["destination"], abi)]


def _tree_stats(path, full):
    count = 0
    total = 0
    fingerprint = hashlib.sha256() if full else None
    required_files = []
    for current, directories, files in os.walk(path, topdown=True, followlinks=False):
        safe_directories = []
        for name in sorted(directories):
            child = os.path.join(current, name)
            if os.path.islink(child):
                raise ValidationError("tree contains symbolic link: %s" % child)
            safe_directories.append(name)
        directories[:] = safe_directories
        for name in sorted(files):
            child = os.path.join(current, name)
            if not is_regular_file(child):
                raise ValidationError("tree contains non-regular file: %s" % child)
            if name.endswith((".nxpart", ".part")):
                raise ValidationError("tree contains an incomplete file: %s" % child)
            relative = os.path.relpath(child, path).replace(os.sep, "/")
            size = file_size(child)
            count += 1
            total += size
            required_files.append(relative)
            if fingerprint is not None:
                encoded = relative.encode("utf-8")
                fingerprint.update(struct.pack("<I", len(encoded)))
                fingerprint.update(encoded)
                fingerprint.update(struct.pack("<QI", size, file_crc32(child)))
    return count, total, fingerprint.hexdigest() if fingerprint else None, required_files


def validate_output_path(path, spec, full=True, label=None, abi=None):
    label = label or path
    expected_type = spec.get("type")
    if os.path.islink(path):
        raise ValidationError("%s is a symbolic link" % label)
    if os.path.isdir(path):
        if expected_type not in (None, "tree", "directory"):
            raise ValidationError("%s is a directory, expected %s" % (label, expected_type))
        count, total, fingerprint, relative_files = _tree_stats(path, full)
        if not validate_summary(count, total, spec):
            raise ValidationError(
                "%s tree count/size mismatch (%d files, %d bytes)"
                % (label, count, total)
            )
        required = spec.get("required_paths", [])
        relative_set = set(relative_files)
        for relative in required:
            candidate = os.path.join(path, *relative.split("/"))
            if (
                relative not in relative_set
                and not os.path.isdir(candidate)
            ):
                raise ValidationError("%s is missing required path %s" % (label, relative))
        expected_fingerprint = spec.get("tree_fingerprint")
        if expected_fingerprint is not None:
            if not isinstance(expected_fingerprint, str) or not re.fullmatch(
                r"[0-9a-fA-F]{64}", expected_fingerprint
            ):
                raise RecipeError("tree_fingerprint must be SHA-256 hex")
            if full and fingerprint != expected_fingerprint.lower():
                raise ValidationError("%s tree fingerprint mismatch" % label)
        return
    if not is_regular_file(path):
        raise ValidationError("%s is missing or not a regular file" % label)
    if expected_type in ("tree", "directory"):
        raise ValidationError("%s is a file, expected directory" % label)
    size = file_size(path)
    if not _size_valid(size, spec):
        raise ValidationError("%s has unexpected size %d" % (label, size))
    magic = parse_magic(spec)
    if magic is not None:
        with open(path, "rb") as stream:
            stream.seek(int(spec.get("magic_offset", 0)))
            actual = stream.read(len(magic))
        if actual != magic:
            raise ValidationError("%s has unexpected magic %s" % (label, actual.hex()))
    if "elf_machine" in spec:
        with open(path, "rb") as stream:
            header = stream.read(64)
        expected = _expected_elf_machine(spec, abi)
        actual = _elf_machine_from_header(header)
        if actual != expected:
            raise ValidationError(
                "%s has ELF machine %r, expected %d" % (label, actual, expected)
            )
    if full:
        crc_values = normalize_crc_list(spec.get("crc32"), "crc32")
        if crc_values:
            actual_crc = file_crc32(path)
            if actual_crc not in crc_values:
                raise ValidationError("%s CRC32 mismatch" % label)
        hashes = normalize_hash_list(spec.get("sha256"), "sha256")
        if hashes:
            actual_hash = file_sha256(path)
            if actual_hash not in hashes:
                raise ValidationError("%s SHA-256 mismatch" % label)


def validate_recipe_outputs(root, recipe, abi, plan=None, marker=None, full=True):
    checked = set()
    for rule in recipe.data["extract"]:
        paths = _validation_paths_for_rule(recipe, rule, abi, plan, marker)
        if not paths:
            if rule.get("required", True):
                raise ValidationError(
                    "cannot derive installed path for required payload %s" % rule["id"]
                )
            continue
        validation = rule.get("validate", {})
        for relative in paths:
            validate_relative_path(relative, "validation path")
            path = safe_join(root, relative, "validation path")
            if not os.path.exists(path) and not rule.get("required", True):
                continue
            validate_output_path(
                path,
                validation,
                full,
                "%s (%s)" % (rule["id"], relative),
                abi=abi,
            )
            checked.add(relative)
    for index, check in enumerate(recipe.data.get("validate", [])):
        relative = template_value(check["path"], abi)
        validate_relative_path(relative, "validation path")
        validate_output_path(
            safe_join(root, relative, "validation path"),
            check,
            full,
            "validation[%d] (%s)" % (index, relative),
            abi=abi,
        )
        checked.add(relative)
    commit_paths = (
        plan.commit_paths
        if plan is not None
        else marker.get("commit", [])
        if marker is not None
        else _expand_commit_paths(recipe, abi)
    )
    for relative in commit_paths:
        validate_relative_path(relative, "commit path")
        path = safe_join(root, relative, "commit path")
        if not os.path.exists(path) and not os.path.islink(path):
            raise ValidationError("commit payload is missing: %s" % relative)
    return checked


def _resume_item_valid(path, item, validation, abi):
    if not is_regular_file(path):
        return False
    try:
        if file_size(path) != item.size:
            return False
        if item.crc is not None:
            return file_crc32(path) == item.crc
        return validate_loose_candidate(item.loose, validation, abi) and (
            file_sha256(path) == file_sha256(item.loose.path)
        )
    except OSError:
        return False


def _copy_item(item, destination, progress, base_done, total_bytes, logger):
    os.makedirs(os.path.dirname(destination), exist_ok=True)
    temporary = destination + ".nxpart"
    try:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        written = 0
        crc = 0
        source_context = (
            item.archive.open_member(item.info)
            if item.info is not None
            else open(item.loose.path, "rb")
        )
        with source_context as source, open(temporary, "xb") as output:
            while True:
                block = source.read(CHUNK_SIZE)
                if not block:
                    break
                output.write(block)
                written += len(block)
                crc = binascii.crc32(block, crc)
                done = base_done + written
                phase_value = done * 1000 // max(total_bytes, 1)
                progress.update(
                    phase=4,
                    overall=220 + phase_value * 430 // 1000,
                    phase_progress=phase_value,
                    done_bytes=done,
                    total_bytes=total_bytes,
                    message="EXTRACTING GAME DATA",
                    detail="%s | %s / %s"
                    % (
                        item.source_name,
                        human_bytes(done),
                        human_bytes(total_bytes),
                    ),
                )
            output.flush()
            os.fsync(output.fileno())
        if written != item.size:
            raise SourceError(
                "short extraction for %s (%d/%d bytes)"
                % (item.source_name, written, item.size)
            )
        if item.crc is not None and (crc & 0xFFFFFFFF) != item.crc:
            raise SourceError("CRC mismatch while extracting %s" % item.source_name)
        os.replace(temporary, destination)
        fsync_directory(os.path.dirname(destination))
        logger.log(
            "extracted %s -> %s (%s)"
            % (item.source_name, item.destination, human_bytes(item.size))
        )
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


def preflight_payload_space(recipe, plan, stage, logger):
    missing = 0
    rules = {rule["id"]: rule for rule in recipe.data["extract"]}
    for item in plan.items:
        destination = safe_join(stage, item.destination, "stage destination")
        if not _resume_item_valid(
            destination,
            item,
            rules[item.rule_id].get("validate", {}),
            plan.abi,
        ):
            missing += item.size
    safety = int(recipe.data.get("space", {}).get("safety_bytes", DEFAULT_SAFETY_BYTES))
    if safety < 0:
        raise RecipeError("space.safety_bytes must not be negative")
    available = shutil.disk_usage(os.path.dirname(stage)).free
    required = missing + safety
    logger.log(
        "storage preflight: missing=%s safety=%s available=%s"
        % (human_bytes(missing), human_bytes(safety), human_bytes(available))
    )
    if available < required:
        raise SourceError(
            "not enough free space: need %s, have %s"
            % (human_bytes(required), human_bytes(available))
        )


def extract_plan(recipe, plan, stage, progress, logger):
    if os.path.lexists(stage):
        if os.path.islink(stage) or not os.path.isdir(stage):
            raise NXError("stage is linked or not a directory: %s" % stage)
    else:
        os.makedirs(stage)
    rules = {rule["id"]: rule for rule in recipe.data["extract"]}
    resumed = 0
    for item in plan.items:
        destination = safe_join(stage, item.destination, "stage destination")
        ensure_no_symlink_parents(stage, item.destination)
        validation = rules[item.rule_id].get("validate", {})
        if _resume_item_valid(destination, item, validation, plan.abi):
            resumed += item.size
    done = resumed
    total = plan.total_bytes
    progress.update(
        phase=4,
        overall=220 + done * 430 // max(total, 1),
        phase_progress=done * 1000 // max(total, 1),
        done_bytes=done,
        total_bytes=total,
        message="EXTRACTING GAME DATA",
        detail="resuming %s of %s" % (human_bytes(done), human_bytes(total)),
        force=True,
    )
    if resumed:
        logger.log("resuming %s of already validated staged data" % human_bytes(resumed))
    for item in plan.items:
        destination = safe_join(stage, item.destination, "stage destination")
        validation = rules[item.rule_id].get("validate", {})
        if _resume_item_valid(destination, item, validation, plan.abi):
            continue
        _copy_item(item, destination, progress, done, total, logger)
        mode = rules[item.rule_id].get("mode")
        if mode is not None:
            try:
                numeric_mode = int(str(mode), 8)
            except ValueError:
                raise RecipeError("extract %s mode must be octal" % item.rule_id)
            if numeric_mode < 0 or numeric_mode > 0o777:
                raise RecipeError("extract %s mode is out of range" % item.rule_id)
            os.chmod(destination, numeric_mode)
        done += item.size
    progress.update(
        phase=4,
        overall=650,
        phase_progress=1000,
        done_bytes=total,
        total_bytes=total,
        message="GAME DATA EXTRACTED",
        force=True,
    )


def _format_hook_value(value, mapping):
    try:
        return value.format(**mapping)
    except (KeyError, ValueError) as error:
        raise RecipeError("invalid hook template %r: %s" % (value, error))


def _checkpoint_valid(stage, checks, abi):
    if not checks:
        return False
    try:
        for check in checks:
            relative = template_value(check["path"], abi)
            validate_relative_path(relative, "hook checkpoint")
            validate_output_path(
                safe_join(stage, relative, "hook checkpoint"),
                check,
                full=True,
                label="hook checkpoint %s" % relative,
                abi=abi,
            )
        return True
    except (OSError, ValidationError):
        return False


def run_hooks(recipe, plan, game_dir, stage, workspace, progress, logger):
    hooks = recipe.data.get("hooks", [])
    if not hooks:
        return
    mapping = {
        "game_dir": game_dir,
        "stage": stage,
        "workspace": workspace,
        "recipe_dir": recipe.root,
        "abi": plan.abi,
    }
    hook_root = os.path.join(workspace, "hooks")
    os.makedirs(hook_root, exist_ok=True)
    for index, hook in enumerate(hooks):
        checkpoint = hook.get("checkpoint", [])
        marker = os.path.join(hook_root, hook["id"] + ".json")
        if is_regular_file(marker) and _checkpoint_valid(
            stage, checkpoint, plan.abi
        ):
            try:
                state = load_json(marker)
            except RecipeError:
                state = {}
            if state.get("recipe_digest") == recipe.digest:
                logger.log("hook %s resumed from validated checkpoint" % hook["id"])
                continue
        phase_progress = index * 1000 // max(len(hooks), 1)
        progress.update(
            phase=5,
            overall=650 + index * 100 // max(len(hooks), 1),
            phase_progress=phase_progress,
            message="PROCESSING GAME DATA",
            detail=hook["id"],
            force=True,
        )
        argv = [_format_hook_value(value, mapping) for value in hook["argv"]]
        cwd_value = hook.get("cwd", "{game_dir}")
        cwd = _format_hook_value(cwd_value, mapping)
        if not os.path.isabs(cwd):
            cwd = os.path.join(game_dir, cwd)
        cwd = os.path.realpath(cwd)
        try:
            if os.path.commonpath((game_dir, cwd)) != game_dir:
                raise RecipeError("hook %s cwd escapes game directory" % hook["id"])
        except ValueError:
            raise RecipeError("hook %s cwd is invalid" % hook["id"])
        environment = os.environ.copy()
        environment.update(
            {
                "NXEXTRACT_GAME_DIR": game_dir,
                "NXEXTRACT_STAGE": stage,
                "NXEXTRACT_WORKSPACE": workspace,
                "NXEXTRACT_ABI": plan.abi,
                "NXEXTRACT_PROGRESS_FILE": progress.path or "",
            }
        )
        extra_environment = hook.get("env", {})
        if not isinstance(extra_environment, dict) or not all(
            isinstance(key, str) and isinstance(value, str)
            for key, value in extra_environment.items()
        ):
            raise RecipeError("hook %s env must be a string object" % hook["id"])
        for key, value in extra_environment.items():
            if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", key):
                raise RecipeError("hook %s has unsafe environment name" % hook["id"])
            environment[key] = _format_hook_value(value, mapping)
        logger.log("running hook %s: %s" % (hook["id"], " ".join(argv)))
        try:
            process = subprocess.Popen(
                argv,
                cwd=cwd,
                env=environment,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
            )
        except OSError as error:
            raise NXError("cannot start hook %s: %s" % (hook["id"], error))
        try:
            for line in process.stdout:
                line = line.rstrip("\r\n")
                logger.log("[%s] %s" % (hook["id"], line))
                match = re.match(r"^NXEXTRACT_PROGRESS\s+(\d+)\s+(\d+)(?:\s+(.*))?$", line)
                if match:
                    done = int(match.group(1))
                    total = max(1, int(match.group(2)))
                    value = min(1000, done * 1000 // total)
                    progress.update(
                        phase_progress=value,
                        detail=match.group(3) or hook["id"],
                    )
            status = process.wait()
        except BaseException:
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            raise
        if status != 0:
            raise NXError("hook %s failed with status %d" % (hook["id"], status))
        if checkpoint and not _checkpoint_valid(stage, checkpoint, plan.abi):
            raise ValidationError("hook %s checkpoint did not validate" % hook["id"])
        atomic_write_json(
            marker,
            {
                "format": FORMAT_VERSION,
                "hook": hook["id"],
                "recipe_digest": recipe.digest,
                "completed": int(time.time()),
            },
        )
    progress.update(
        phase=5,
        overall=750,
        phase_progress=1000,
        message="GAME DATA PROCESSED",
        force=True,
    )


class WorkspaceLock:
    def __init__(self, workspace):
        self.workspace = workspace
        self.path = os.path.join(workspace, "install.lock")
        self.stream = None

    def __enter__(self):
        os.makedirs(self.workspace, exist_ok=True)
        if os.path.islink(self.workspace):
            raise NXError("workspace must not be a symbolic link")
        self.stream = open(self.path, "a+", encoding="utf-8")
        try:
            fcntl.flock(self.stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            if error.errno in (errno.EACCES, errno.EAGAIN):
                raise NXError("another extraction is already active")
            raise
        self.stream.seek(0)
        self.stream.truncate()
        self.stream.write("%d\n" % os.getpid())
        self.stream.flush()
        os.fsync(self.stream.fileno())
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        if self.stream is not None:
            try:
                fcntl.flock(self.stream.fileno(), fcntl.LOCK_UN)
            finally:
                self.stream.close()
                self.stream = None


def _load_marker(path):
    if not is_regular_file(path):
        return None
    try:
        value = load_json(path)
    except RecipeError:
        return None
    return value if isinstance(value, dict) else None


def _journal_path(workspace):
    return os.path.join(workspace, "transaction.json")


def _backup_root(workspace):
    return os.path.join(workspace, "backup")


def _stage_root(workspace):
    return os.path.join(workspace, "stage")


def _ensure_real_directory(path, label):
    try:
        os.mkdir(path)
    except FileExistsError:
        pass
    try:
        mode = os.lstat(path).st_mode
    except OSError as error:
        raise NXError("%s is unavailable: %s" % (label, error))
    if stat.S_ISLNK(mode) or not stat.S_ISDIR(mode):
        raise NXError("%s must be a real directory: %s" % (label, path))


def prepare_workspace(game_dir, identifier):
    parent = os.path.join(game_dir, ".nxextract")
    _ensure_real_directory(parent, "extractor workspace root")
    workspace = os.path.join(parent, identifier)
    _ensure_real_directory(workspace, "extractor workspace")
    return workspace


def _journal_write(workspace, journal):
    atomic_write_json(_journal_path(workspace), journal)


def _finalize_published_transaction(workspace, logger):
    remove_path(_backup_root(workspace))
    remove_path(_stage_root(workspace))
    discard_path(
        os.path.join(workspace, "source-cache"), logger, label="source cache"
    )
    try:
        os.unlink(_journal_path(workspace))
    except FileNotFoundError:
        pass
    fsync_directory(workspace)
    logger.log("finished cleanup of a previously published transaction")


def rollback_transaction(game_dir, workspace, journal, logger):
    stage = _stage_root(workspace)
    backup = _backup_root(workspace)
    os.makedirs(stage, exist_ok=True)
    logger.log("rolling back interrupted payload transaction")
    for item in reversed(journal.get("paths", [])):
        relative = item.get("path")
        validate_relative_path(relative, "transaction path")
        destination = safe_join(game_dir, relative, "transaction destination")
        staged = safe_join(stage, relative, "transaction stage")
        previous = safe_join(backup, relative, "transaction backup")
        if item.get("installed"):
            if os.path.lexists(destination):
                if not os.path.lexists(staged):
                    os.makedirs(os.path.dirname(staged), exist_ok=True)
                    os.rename(destination, staged)
                else:
                    remove_path(destination)
        if item.get("backed_up") and os.path.lexists(previous):
            if os.path.lexists(destination):
                remove_path(destination)
            os.makedirs(os.path.dirname(destination), exist_ok=True)
            os.rename(previous, destination)
    fsync_directory(game_dir)
    remove_path(backup)
    try:
        os.unlink(_journal_path(workspace))
    except FileNotFoundError:
        pass
    fsync_directory(workspace)
    logger.log("payload transaction rolled back; staged work was preserved")


def recover_transaction(game_dir, workspace, marker_path, logger):
    path = _journal_path(workspace)
    if not is_regular_file(path):
        return
    journal = load_json(path)
    if not isinstance(journal, dict) or journal.get("format") != FORMAT_VERSION:
        raise NXError("unsafe or unsupported transaction journal was preserved at %s" % path)
    marker = _load_marker(marker_path)
    transaction_id = journal.get("transaction_id")
    if journal.get("published") or (
        marker is not None and marker.get("transaction_id") == transaction_id
    ):
        _finalize_published_transaction(workspace, logger)
        return
    rollback_transaction(game_dir, workspace, journal, logger)


def _write_install_marker(marker_path, recipe, plan, transaction_id):
    marker = {
        "format": FORMAT_VERSION,
        "nxextract_version": NXEXTRACT_VERSION,
        "recipe_id": recipe.identifier,
        "recipe_version": recipe.version,
        "recipe_digest": recipe.digest,
        "abi": plan.abi,
        "plan_fingerprint": plan.fingerprint,
        "transaction_id": transaction_id,
        "completed": int(time.time()),
        "commit": list(plan.commit_paths),
        "items": [
            {"rule": item.rule_id, "destination": item.destination, "size": item.size}
            for item in plan.items
        ],
    }
    atomic_write_json(marker_path, marker)
    return marker


def commit_stage(recipe, plan, game_dir, workspace, marker_path, progress, logger):
    stage = _stage_root(workspace)
    backup = _backup_root(workspace)
    remove_path(backup)
    os.makedirs(backup, exist_ok=True)
    transaction_id = uuid.uuid4().hex
    journal = {
        "format": FORMAT_VERSION,
        "transaction_id": transaction_id,
        "recipe_digest": recipe.digest,
        "abi": plan.abi,
        "published": False,
        "paths": [
            {"path": relative, "backed_up": False, "installed": False}
            for relative in plan.commit_paths
        ],
    }
    for relative in plan.commit_paths:
        staged = safe_join(stage, relative, "commit stage")
        if not os.path.lexists(staged):
            raise ValidationError("staged commit path is missing: %s" % relative)
        ensure_no_symlink_parents(stage, relative)
        ensure_no_symlink_parents(game_dir, relative)
    _journal_write(workspace, journal)
    progress.update(
        phase=7,
        overall=900,
        phase_progress=0,
        message="INSTALLING VALIDATED GAME DATA",
        force=True,
    )
    try:
        for index, item in enumerate(journal["paths"]):
            relative = item["path"]
            destination = safe_join(game_dir, relative, "commit destination")
            previous = safe_join(backup, relative, "commit backup")
            if os.path.lexists(destination):
                os.makedirs(os.path.dirname(previous), exist_ok=True)
                os.rename(destination, previous)
                item["backed_up"] = True
                _journal_write(workspace, journal)
            staged = safe_join(stage, relative, "commit stage")
            os.makedirs(os.path.dirname(destination), exist_ok=True)
            os.rename(staged, destination)
            item["installed"] = True
            _journal_write(workspace, journal)
            progress.update(
                phase_progress=(index + 1) * 700 // len(journal["paths"]),
                overall=900 + (index + 1) * 60 // len(journal["paths"]),
                detail=relative,
            )
        fsync_directory(game_dir)
        progress.update(
            phase=6,
            overall=970,
            phase_progress=900,
            message="VERIFYING INSTALLED GAME DATA",
            force=True,
        )
        validate_recipe_outputs(game_dir, recipe, plan.abi, plan=plan, full=True)
        _write_install_marker(marker_path, recipe, plan, transaction_id)
        journal["published"] = True
        _journal_write(workspace, journal)
        fsync_directory(game_dir)
    except BaseException:
        rollback_transaction(game_dir, workspace, journal, logger)
        raise
    remove_path(backup)
    remove_path(stage)
    try:
        os.unlink(_journal_path(workspace))
    except FileNotFoundError:
        pass
    fsync_directory(workspace)
    logger.log("validated payload committed transactionally")


class UISession:
    def __init__(self, ui_option, script_dir, workspace, progress_path, recipe, logger):
        self.ui_option = ui_option
        self.script_dir = script_dir
        self.workspace = workspace
        self.progress_path = progress_path
        self.recipe = recipe
        self.logger = logger
        self.process = None
        self.stop_path = os.path.join(workspace, "ui.stop")
        self.log_stream = None

    def _find_binary(self):
        if self.ui_option in (None, "none", "off", "0"):
            return None
        if self.ui_option != "auto":
            candidate = os.path.realpath(self.ui_option)
            return candidate if os.access(candidate, os.X_OK) else None
        candidates = (
            os.path.join(self.script_dir, "nxextract-ui"),
            os.path.join(self.script_dir, "ui", "build", "nxextract-ui"),
            os.path.join(self.script_dir, "build", "nxextract-ui"),
        )
        for candidate in candidates:
            if os.access(candidate, os.X_OK) and is_regular_file(candidate):
                return candidate
        return None

    def start(self):
        binary = self._find_binary()
        if not binary:
            return
        try:
            os.unlink(self.stop_path)
        except FileNotFoundError:
            pass
        self.log_stream = open(
            os.path.join(self.workspace, "ui.log"), "a", encoding="utf-8", buffering=1
        )
        try:
            self.process = subprocess.Popen(
                [
                    binary,
                    self.progress_path,
                    self.stop_path,
                    self.recipe.title,
                    self.recipe.version,
                ],
                stdin=subprocess.DEVNULL,
                stdout=self.log_stream,
                stderr=subprocess.STDOUT,
                cwd=self.workspace,
            )
            self.logger.log("setup UI started with %s" % binary)
        except OSError as error:
            self.logger.log("setup UI unavailable (%s); continuing headless" % error)
            self.log_stream.close()
            self.log_stream = None

    def stop(self, delay=0):
        if self.process is None:
            return
        if delay > 0:
            time.sleep(delay)
        atomic_write(self.stop_path, "")
        try:
            self.process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            try:
                self.process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        if self.log_stream:
            self.log_stream.close()
        self.process = None
        self.log_stream = None


def marker_matches_recipe(marker, recipe):
    return (
        isinstance(marker, dict)
        and marker.get("format") == FORMAT_VERSION
        and marker.get("recipe_id") == recipe.identifier
        and marker.get("recipe_digest") == recipe.digest
        and isinstance(marker.get("abi"), str)
    )


def marker_fast_valid(marker_path, recipe, game_dir, logger):
    marker = _load_marker(marker_path)
    if not marker_matches_recipe(marker, recipe):
        return None
    try:
        validate_recipe_outputs(
            game_dir,
            recipe,
            marker["abi"],
            marker=marker,
            full=False,
        )
    except (OSError, NXError) as error:
        logger.log("existing marker rejected: %s" % error)
        return None
    return marker


def try_adopt_existing(recipe, game_dir, marker_path, logger, progress, abi_override):
    abis = [abi_override] if abi_override else recipe.abi_order()
    for abi in abis:
        try:
            validate_recipe_outputs(game_dir, recipe, abi, full=True)
        except (OSError, NXError) as error:
            logger.log(
                "existing data not adoptable for ABI %s: %s" % (abi, error)
            )
            continue
        pseudo_items = []
        for rule in recipe.data["extract"]:
            for relative in _validation_paths_for_rule(recipe, rule, abi):
                path = safe_join(game_dir, relative, "adopted payload")
                if is_regular_file(path):
                    pseudo = type("AdoptedItem", (), {})()
                    pseudo.rule_id = rule["id"]
                    pseudo.destination = relative
                    pseudo.size = file_size(path)
                    pseudo.crc = None
                    pseudo_items.append(pseudo)
        pseudo_group = CandidateGroup("validated existing data", [], [], None, "existing")
        plan = Plan(pseudo_group, abi, pseudo_items, _expand_commit_paths(recipe, abi))
        _write_install_marker(marker_path, recipe, plan, uuid.uuid4().hex)
        logger.log("adopted fully validated existing data without requiring an APK")
        progress.done("EXISTING GAME DATA VALIDATED")
        return True
    return False


def _prepare_stage_state(recipe, plan, workspace, logger):
    state_path = os.path.join(workspace, "state.json")
    stage = _stage_root(workspace)
    state = _load_marker(state_path)
    expected = {
        "format": FORMAT_VERSION,
        "recipe_digest": recipe.digest,
        "plan_fingerprint": plan.fingerprint,
        "abi": plan.abi,
    }
    if state is not None and any(state.get(key) != value for key, value in expected.items()):
        logger.log("discarding staged data made for a different recipe or payload")
        remove_path(stage)
        remove_path(os.path.join(workspace, "hooks"))
    os.makedirs(stage, exist_ok=True)
    atomic_write_json(state_path, expected)
    return stage


def install_command(args):
    game_dir = os.path.realpath(args.game_dir)
    if not os.path.isdir(game_dir) or os.path.islink(game_dir):
        raise NXError("game directory is missing, linked or not a directory")
    recipe = Recipe(args.recipe)
    workspace = prepare_workspace(game_dir, recipe.identifier)
    log_path = safe_join(
        game_dir, recipe.data.get("log", "nxextract.log"), "log path"
    )
    ensure_no_symlink_parents(
        game_dir, recipe.data.get("log", "nxextract.log")
    )
    if os.path.islink(log_path):
        raise NXError("log path must not be a symbolic link")
    logger = Logger(log_path, verbose=not args.quiet)
    progress_path = (
        os.path.realpath(args.progress_file)
        if args.progress_file
        else os.path.join(workspace, "progress.txt")
    )
    progress = Progress(progress_path, logger)
    marker_path = safe_join(game_dir, recipe.marker, "marker")
    ui = UISession(
        args.ui,
        os.path.dirname(os.path.realpath(__file__)),
        workspace,
        progress_path,
        recipe,
        logger,
    )
    archives = []
    try:
        with WorkspaceLock(workspace):
            logger.log(
                "=== NXExtract %s format=%s recipe=%s version=%s ==="
                % (
                    NXEXTRACT_VERSION,
                    FORMAT_VERSION,
                    recipe.identifier,
                    recipe.version,
                )
            )
            recover_transaction(game_dir, workspace, marker_path, logger)
            marker = marker_fast_valid(marker_path, recipe, game_dir, logger)
            if marker is not None:
                progress.done("GAME DATA ALREADY READY")
                logger.log("fast validation marker accepted; no source scan needed")
                return 0
            ui.start()
            progress.update(
                phase=0,
                overall=0,
                phase_progress=0,
                message="PREPARING GAME DATA",
                force=True,
            )
            if try_adopt_existing(
                recipe,
                game_dir,
                marker_path,
                logger,
                progress,
                args.abi,
            ):
                ui.stop(delay=float(recipe.data.get("ui_success_seconds", 1)))
                return 0
            progress.update(
                phase=1,
                overall=20,
                phase_progress=0,
                message="SCANNING APK AND BUNDLE CONTENTS",
                force=True,
            )
            discovery = discover_inputs(recipe, game_dir, args.input, logger)
            groups, archives = build_candidate_groups(
                recipe, discovery, workspace, logger, progress
            )
            plan = resolve_plan(recipe, groups, args.abi, logger, progress)
            stage = _prepare_stage_state(recipe, plan, workspace, logger)
            preflight_payload_space(recipe, plan, stage, logger)
            extract_plan(recipe, plan, stage, progress, logger)
            run_hooks(
                recipe, plan, game_dir, stage, workspace, progress, logger
            )
            progress.update(
                phase=6,
                overall=780,
                phase_progress=0,
                message="VALIDATING EXTRACTED GAME DATA",
                force=True,
            )
            validate_recipe_outputs(stage, recipe, plan.abi, plan=plan, full=True)
            progress.update(
                phase=6,
                overall=890,
                phase_progress=1000,
                message="EXTRACTED GAME DATA VALIDATED",
                force=True,
            )
            commit_stage(
                recipe,
                plan,
                game_dir,
                workspace,
                marker_path,
                progress,
                logger,
            )
            for archive in archives:
                archive.close()
            discard_path(
                os.path.join(workspace, "source-cache"),
                logger,
                label="source cache",
            )
            progress.done()
            logger.log("=== installation complete ===")
            ui.stop(delay=float(recipe.data.get("ui_success_seconds", 1)))
            return 0
    except (NXError, OSError, zipfile.BadZipFile, RuntimeError, NotImplementedError) as error:
        logger.log("ERROR: %s" % error)
        progress.fail(str(error).upper())
        if ui.process is not None:
            ui.stop(delay=float(recipe.data.get("ui_error_seconds", 5)))
        return 1
    finally:
        for archive in archives:
            archive.close()
        ui.stop()
        logger.close()


def plan_command(args):
    game_dir = os.path.realpath(args.game_dir)
    if not os.path.isdir(game_dir) or os.path.islink(game_dir):
        raise NXError("game directory is missing, linked or not a directory")
    recipe = Recipe(args.recipe)
    workspace = prepare_workspace(game_dir, recipe.identifier)
    logger = Logger(None, verbose=not args.quiet)
    progress = Progress(None, logger)
    archives = []
    try:
        with WorkspaceLock(workspace):
            discovery = discover_inputs(recipe, game_dir, args.input, logger)
            groups, archives = build_candidate_groups(
                recipe, discovery, workspace, logger, progress
            )
            plan = resolve_plan(recipe, groups, args.abi, logger, progress)
            output = {
                "recipe": recipe.identifier,
                "recipe_version": recipe.version,
                "group": plan.group.description(),
                "abi": plan.abi,
                "total_bytes": plan.total_bytes,
                "commit": plan.commit_paths,
                "items": [
                    {
                        "rule": item.rule_id,
                        "source_archive": item.source_label,
                        "source_entry": item.source_name,
                        "destination": item.destination,
                        "size": item.size,
                        "crc32": "%08x" % item.crc if item.crc is not None else None,
                    }
                    for item in plan.items
                ],
            }
            print(json.dumps(output, ensure_ascii=False, indent=2, sort_keys=True))
            return 0
    finally:
        for archive in archives:
            archive.close()
        logger.close()


class ScanRecipe:
    def __init__(self):
        self.data = {"extract": []}

    @property
    def input_config(self):
        return {
            "search_dirs": ["gamedata", "."],
            "prefer_first_nonempty": True,
            "sniff_all_in_primary": True,
            "extensions": list(DEFAULT_EXTENSIONS),
        }


def scan_command(args):
    game_dir = os.path.realpath(args.game_dir)
    logger = Logger(None, verbose=False)
    recipe = Recipe(args.recipe) if args.recipe else ScanRecipe()
    discovery = discover_inputs(recipe, game_dir, args.input, logger)
    records = []
    for kind, values in (
        ("apk", discovery.apks),
        ("bundle", discovery.bundles),
        ("archive", discovery.generic_archives),
        ("loose", discovery.loose),
    ):
        for path in values:
            record = {
                "path": path,
                "filename": os.path.basename(path),
                "kind": kind,
                "size": file_size(path),
            }
            if kind == "apk":
                archive = Archive(path, "apk")
                try:
                    record.update(
                        {
                            "package": archive.package,
                            "split": archive.split,
                            "entries": len(archive.members),
                            "abis": sorted(
                                {
                                    name.split("/")[1]
                                    for name in archive.members
                                    if name.startswith("lib/") and name.count("/") >= 2
                                }
                            ),
                        }
                    )
                finally:
                    archive.close()
            elif kind == "bundle":
                with zipfile.ZipFile(path, "r") as archive:
                    members = [
                        info
                        for info in archive.infolist()
                        if not info.is_dir()
                        and PurePosixPath(info.filename).suffix.lower() == ".apk"
                    ]
                    record["inner_apks"] = len(members)
                    record["inner_apk_bytes"] = sum(info.file_size for info in members)
            records.append(record)
    print(json.dumps({"inputs": records}, ensure_ascii=False, indent=2, sort_keys=True))
    return 0


def verify_command(args):
    game_dir = os.path.realpath(args.game_dir)
    recipe = Recipe(args.recipe)
    marker_path = safe_join(game_dir, recipe.marker, "marker")
    marker = _load_marker(marker_path)
    if not marker_matches_recipe(marker, recipe):
        raise ValidationError("matching installation marker was not found")
    validate_recipe_outputs(
        game_dir, recipe, marker["abi"], marker=marker, full=True
    )
    print(
        "OK: %s version %s, ABI %s"
        % (recipe.title, recipe.version, marker["abi"])
    )
    return 0


def recipe_check_command(args):
    recipe = Recipe(args.recipe)
    print(
        "OK: recipe=%s version=%s digest=%s"
        % (recipe.identifier, recipe.version, recipe.digest)
    )
    return 0


def progress_command(args):
    progress = Progress(args.file)
    progress.update(
        phase=args.phase,
        overall=args.overall,
        phase_progress=args.phase_progress,
        done_bytes=args.done_bytes,
        total_bytes=args.total_bytes,
        message=args.message,
        detail=args.detail,
        state=args.state,
        force=True,
    )
    return 0


def build_parser():
    parser = argparse.ArgumentParser(
        prog="nxextract",
        description="Content-driven universal APK/APKM/APKS/XAPK extractor",
    )
    parser.add_argument(
        "--version",
        action="version",
        version="NXExtract %s" % NXEXTRACT_VERSION,
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    def add_common(subparser, recipe_required=True):
        subparser.add_argument(
            "--recipe", required=recipe_required, help="per-port JSON extraction recipe"
        )
        subparser.add_argument("--game-dir", required=True, help="port data directory")
        subparser.add_argument(
            "--input",
            action="append",
            default=[],
            help="explicit input path; repeat for a loose split set",
        )
        subparser.add_argument("--abi", help="override the recipe ABI selection")
        subparser.add_argument("--quiet", action="store_true")

    install = subparsers.add_parser("install", help="extract, validate and commit data")
    add_common(install)
    install.add_argument(
        "--ui",
        default="auto",
        help="auto, none, or a path to nxextract-ui (default: auto)",
    )
    install.add_argument("--progress-file", help="override progress protocol path")
    install.set_defaults(handler=install_command)

    plan = subparsers.add_parser("plan", help="resolve sources without extracting payload")
    add_common(plan)
    plan.set_defaults(handler=plan_command)

    scan = subparsers.add_parser("scan", help="classify candidate files by contents")
    scan.add_argument("--game-dir", required=True)
    scan.add_argument("--recipe")
    scan.add_argument("--input", action="append", default=[])
    scan.set_defaults(handler=scan_command)

    verify = subparsers.add_parser("verify", help="fully verify an installed payload")
    verify.add_argument("--recipe", required=True)
    verify.add_argument("--game-dir", required=True)
    verify.set_defaults(handler=verify_command)

    recipe_check = subparsers.add_parser("recipe-check", help="validate a recipe")
    recipe_check.add_argument("--recipe", required=True)
    recipe_check.set_defaults(handler=recipe_check_command)

    progress = subparsers.add_parser(
        "progress", help="write one NXEXTRACT_V1 progress update for a hook"
    )
    progress.add_argument("--file", required=True)
    progress.add_argument("--state", type=int, default=1)
    progress.add_argument("--phase", type=int, default=5)
    progress.add_argument("--overall", type=int, default=650)
    progress.add_argument("--phase-progress", type=int, default=0)
    progress.add_argument("--done-bytes", type=int, default=0)
    progress.add_argument("--total-bytes", type=int, default=0)
    progress.add_argument("--message", default="PROCESSING GAME DATA")
    progress.add_argument("--detail", default="")
    progress.set_defaults(handler=progress_command)
    return parser


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return int(args.handler(args) or 0)
    except (NXError, OSError, zipfile.BadZipFile, RuntimeError, NotImplementedError) as error:
        print("nxextract: ERROR: %s" % error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
