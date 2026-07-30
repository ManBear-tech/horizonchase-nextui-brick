# Changelog

All notable NXExtract changes are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## 1.1.2

- Report matched-but-rejected candidates in required-payload plan errors.
  When files match a payload's source pattern but every one fails validation
  (size, sha256, crc32 or ELF machine), the error now says so and names one of
  the rejected candidates instead of claiming the payload was not found.
- Add a synthetic regression test for the rejected-candidate diagnostic.

## 1.1.1

- Log the exact full-validation rejection for every attempted ABI when existing
  game data cannot be adopted. Validation remains strict; the additional
  diagnostic identifies the incomplete or mismatched path without requiring a
  source-package scan to fail first.
- Add a synthetic regression test for the existing-data rejection diagnostic.

## 1.1.0

- Licensed the standalone project under MIT.
- Made the UI compatibility build the default release path.
- Added an AArch64 release gate that rejects GLIBC requirements above 2.30.
- Added `elf_machine: "{abi}"` for ABI-neutral recipes and an ARMv7 fallback
  regression test.
- Added a public `nxextract --version` command and engine version in new
  installation markers.
- Added complete English documentation and standalone architecture, recipe,
  contribution, security and device-compatibility guides.
- Added sanitized real-device screenshots using only the synthetic fixture.
- Added public CI, issue forms, pull-request guidance, funding/community links
  and standalone release notes.
- Validated the Python 3.7 core, GLIBC 2.17 UI and KMSDRM flow on ArkOS.

## 1.0.0

- Initial content-driven APK/APKM/APKS/XAPK extractor.
- Loose split grouping by Android package and automatic ABI selection.
- Resumable staging, bake hooks, full validation and journaled publication.
- Crash recovery, rollback, fast markers and legal-source preservation.
- Dynamic SDL2 first-run UI for fbdev/Mali, KMSDRM and Wayland-class systems.
