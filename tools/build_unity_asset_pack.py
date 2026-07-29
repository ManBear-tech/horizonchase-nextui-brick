#!/usr/bin/env python3
"""Build Horizon Chase's install-time Unity asset-pack APK deterministically."""

import os
import shutil
import stat
import sys
import zipfile


MEMBER = "assets/bin/Data/datapack.unity3d"
CHUNK = 1024 * 1024


def fail(message):
    raise SystemExit("asset-pack error: %s" % message)


def inside(root, path):
    try:
        return os.path.commonpath((root, path)) == root
    except ValueError:
        return False


def main():
    if len(sys.argv) != 3:
        fail("usage: build_unity_asset_pack.py DATAPACK OUTPUT_APK")

    source = os.path.realpath(sys.argv[1])
    output = os.path.abspath(sys.argv[2])
    stage_env = os.environ.get("NXEXTRACT_STAGE")
    if stage_env:
        stage = os.path.realpath(stage_env)
        if not inside(stage, source) or not inside(stage, output):
            fail("input and output must stay inside NXEXTRACT_STAGE")

    try:
        source_stat = os.stat(source)
    except OSError as error:
        fail("cannot read datapack: %s" % error)
    if not stat.S_ISREG(source_stat.st_mode) or source_stat.st_size <= 0:
        fail("datapack is not a non-empty regular file")

    parent = os.path.dirname(output)
    os.makedirs(parent, exist_ok=True)
    temporary = output + ".partial"
    try:
        os.unlink(temporary)
    except FileNotFoundError:
        pass

    info = zipfile.ZipInfo(MEMBER, date_time=(1980, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_STORED
    info.create_system = 3
    info.external_attr = (stat.S_IFREG | 0o644) << 16
    info.file_size = source_stat.st_size

    print("NXEXTRACT_PROGRESS 0 1 CRIANDO UNITY ASSET PACK")
    try:
        with open(source, "rb") as reader:
            with zipfile.ZipFile(
                temporary, "w", compression=zipfile.ZIP_STORED,
                allowZip64=True
            ) as archive:
                with archive.open(info, "w") as writer:
                    shutil.copyfileobj(reader, writer, length=CHUNK)
        with open(temporary, "rb") as stream:
            os.fsync(stream.fileno())
        os.replace(temporary, output)
        directory_fd = os.open(parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    except (OSError, zipfile.BadZipFile) as error:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        fail("cannot create asset-pack APK: %s" % error)

    with zipfile.ZipFile(output) as archive:
        names = archive.namelist()
        if names != [MEMBER] or archive.getinfo(MEMBER).file_size != source_stat.st_size:
            fail("generated APK failed its member validation")
        if archive.testzip() is not None:
            fail("generated APK failed its CRC validation")
    print("NXEXTRACT_PROGRESS 1 1 UNITY ASSET PACK PRONTO")


if __name__ == "__main__":
    main()
