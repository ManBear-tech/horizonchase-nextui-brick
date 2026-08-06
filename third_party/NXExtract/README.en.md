# NXExtract

[![CI](https://github.com/NextOs-Ports/NXExtract/actions/workflows/ci.yml/badge.svg)](https://github.com/NextOs-Ports/NXExtract/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/NextOs-Ports/NXExtract)](https://github.com/NextOs-Ports/NXExtract/releases/latest)
[![MIT License](https://img.shields.io/badge/license-MIT-2ea44f.svg)](LICENSE)

NXExtract is a content-driven, transactional first-run data installer and bake
framework for Android-to-Linux game ports.

[Português](README.md) · [MIT license](LICENSE) ·
[Architecture](docs/ARCHITECTURE.md) ·
[Recipe reference](docs/RECIPE.md) ·
[Device matrix](docs/DEVICE-COMPATIBILITY.md)

## Download

Get the source from the [latest release](https://github.com/NextOs-Ports/NXExtract/releases/latest).
The release also provides a prebuilt
[`nxextract-ui-aarch64`](https://github.com/NextOs-Ports/NXExtract/releases/latest/download/nxextract-ui-aarch64)
requiring only GLIBC 2.17. The Python engine, generic launcher and recipe
example are in that release's source tree.

To develop or adapt NXExtract:

```bash
git clone https://github.com/NextOs-Ports/NXExtract.git
cd NXExtract
./tools/check-release.sh
```

## Real device UI

<p align="center">
  <img src="docs/images/nxextract-mali-1280x720.png"
       alt="NXExtract using the Mali backend at 1280 by 720" width="49%">
  <img src="docs/images/nxextract-kmsdrm-640x480.png"
       alt="NXExtract using the KMSDRM backend at 640 by 480" width="49%">
</p>

<p align="center">
  <sub>Real captures of the same binary: Mali/fbdev at 1280×720 and
  KMSDRM at 640×480. The displayed payload is entirely synthetic.</sub>
</p>

## Support and contribute

NXExtract is free software under MIT. Code, new-device testing, synthetic
recipes and documentation are welcome:

- 🐛 **Bugs and proposals**: [GitHub Issues](https://github.com/NextOs-Ports/NXExtract/issues);
- 🔀 **Code**: [Pull Requests](https://github.com/NextOs-Ports/NXExtract/pulls) and
  the [contribution guide](CONTRIBUTING.md);
- 💬 **Community**: [NextOS Discord](https://discord.gg/DHfY62eDNN).

External filenames are never game identifiers. A user may rename an APK,
APKM, APKS or XAPK freely; NXExtract inspects its contents, Android package,
splits, ABI, internal paths and validators.

## Supported inputs

- merged APK;
- loose split APK set;
- APKM, APKS and XAPK containers;
- companion ZIP archives;
- OBBs and validated loose files;
- extensionless ZIP/APK input in the primary `gamedata` directory.

Each port supplies a small trusted `extractor.json`. Game-specific texture
conversion or other baking is an optional hook. Adding a new port does not
require another extractor implementation.

## Safety model

- ZIP traversal, absolute paths, symlinks, encrypted members, duplicates and
  case-folding destination collisions are rejected.
- Different Android packages are never merged into one split set.
- Different matching payloads are rejected as ambiguous.
- Files copy into a same-filesystem resumable stage.
- Hooks write into the stage, never live data.
- Full validation completes before publication.
- A journal, backups, rename and fsync provide crash recovery and rollback.
- The user’s legal source package is never deleted.
- A validated marker makes the second run fast and source-independent.

## Port integration

Ship these files together:

```text
extractor.json
nxextract.py
nxextract-ui
run-extractor.sh
```

Then call NXExtract in the foreground:

```bash
GAMEDIR="/storage/roms/ports/my-port"
cd "$GAMEDIR"

./run-extractor.sh || exit 1
exec ./my-loader "$GAMEDIR"
```

Do not stop the frontend, force a display driver, use `setsid`, or background
the extractor in a distributed launcher.

## Authoring and diagnostics

Start with `examples/recipe-minimal.json`:

```bash
python3 nxextract.py --version
python3 nxextract.py recipe-check --recipe extractor.json
python3 nxextract.py scan --game-dir .
python3 nxextract.py plan --recipe extractor.json --game-dir .
python3 nxextract.py install --recipe extractor.json --game-dir .
python3 nxextract.py verify --recipe extractor.json --game-dir .
```

See [the full recipe reference](docs/RECIPE.md) for source kinds, templates,
validators, commit roots and resumable bake hooks.

Set `elf_machine` to a literal Android ABI or to `{abi}` when one recipe must
validate the architecture currently being evaluated.

## Compatibility build

NXExtract’s Python core supports Python 3.7 or newer and uses only the standard
library. Every distributed AArch64 ELF in the kit must require GLIBC 2.30 or
older.

Build the UI:

```bash
./ui/build-ui.sh
```

The default build runs an AArch64 GCC toolchain in `debian:bullseye`, strips
the result and executes a hard compatibility gate:

```bash
./tools/check-glibc.sh ./ui/build/nxextract-ui
```

The current UI requires GLIBC 2.17 and dynamically loads the SDL2 supplied by
the target firmware.

## Tests

```bash
./tools/check-release.sh
```

The synthetic suite covers merged and renamed APKs, binary Android manifests,
loose splits, APKM/APKS/XAPK, OBB selection, ambiguity, traversal and collision
rejection, resume, hooks, crash rollback and transaction finalization.

## License

NXExtract is released under the [MIT License](LICENSE). Anyone may use, fork,
modify and redistribute it under those terms. Android packages and game data
are not part of NXExtract and remain subject to their respective owners’
licenses.

See [CONTRIBUTING.md](CONTRIBUTING.md), the
[device validation matrix](docs/DEVICE-COMPATIBILITY.md),
[SECURITY.md](SECURITY.md) and [CHANGELOG.md](CHANGELOG.md).
