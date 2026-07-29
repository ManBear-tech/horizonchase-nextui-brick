# Device compatibility

This is a living compatibility matrix. “Verified” means the listed release
completed install, full verify and marker-based second run with a synthetic
renamed package. A release column prevents an older result from being
mistaken for a test of newer code.

“Maintenance revalidated” means the preceding verified release remains the
full synthetic baseline and the maintenance release completed its changed
path, full output verification and marker path on the listed real device.

No private IP address, hostname, credential or raw log belongs in this file.

| Family / OS | Arch | glibc | Python | SDL video | UI backend | Visual check | Release / status |
|---|---|---:|---:|---|---|---|---|
| NextOS Amlogic-old / Mali-450 | AArch64 | 2.43 | 3.14.6 | `mali`, `offscreen` | `mali` | 1280×720 capture inspected | 1.1.1 maintenance revalidated |
| NextOS X5M / Mali-G310 Valhall | AArch64 | 2.43 | 3.14 | KMSDRM/Wayland-class | `KMSDRM` | 1920×1080 backend opened | 1.1.1 maintenance revalidated |
| R36S / ArkOS | AArch64 | 2.30 | 3.7.5 | `KMSDRM`, `offscreen` | `KMSDRM` | 640×480 capture inspected | 1.1.1 maintenance revalidated |

The release UI is built on Debian Bullseye with an automatic gate of
GLIBC 2.30 or older. The current binary requires GLIBC 2.17.

## Verified records

### R36S / ArkOS — release 1.1.0

- The complete Python suite passed under the device's Python 3.7.5.
- A randomly named XAPK containing an arbitrarily named inner APK was
  discovered by content and installed for `arm64-v8a`.
- The synthetic bake, full verification and marker fast path passed.
- The input SHA-256 remained unchanged and no stage, source cache, journal,
  backup or extractor process remained after completion.
- SDL advertised `KMSDRM` and `offscreen`; NXExtract selected visible
  `KMSDRM`. A real [640×480 scanout capture](images/nxextract-kmsdrm-640x480.png)
  confirmed the responsive header, phase bars, detail and overall progress.
- During a laboratory visual test, the frontend released DRM master first.
  This is not a launcher requirement and service control is never distributed.

### NextOS Amlogic-old / Mali-450 — release 1.1.0

- Install, full verification, source preservation and marker fast path passed.
- The firmware SDL advertised `mali` and `offscreen`; NXExtract selected the
  visible `mali` backend.
- A real [1280×720 framebuffer capture](images/nxextract-mali-1280x720.png)
  confirmed the responsive header, phase bars, detail and overall progress.

### NextOS X5M / Mali-G310 Valhall — release 1.0.0

- Install, full verification, source preservation and marker fast path passed.
- NXExtract selected visible `KMSDRM`.

### Release 1.1.1 maintenance revalidation

- The new per-ABI adoption-rejection diagnostic passed its synthetic regression
  test in the complete 22-test release suite.
- On R36S/ArkOS and NextOS Mali-450, 1.1.1 strictly adopted validated existing
  data, wrote a new marker, completed a full verify and accepted the subsequent
  marker path.
- On NextOS Mali-G310, 1.1.1 accepted the existing marker and completed a full
  verify.
- All three foreground runs exited cleanly, left no extractor or game process
  and restored the frontend state used before testing.
- The SDL UI binary is byte-identical to 1.1.0; no rendering code changed in
  this maintenance release.

## Validation contract

For each new family:

1. record architecture, glibc, Python and advertised SDL drivers;
2. use a synthetic APK/bundle whose external and inner filenames are arbitrary;
3. run first install with the UI visible;
4. run `verify`;
5. run install again and confirm marker fast-path;
6. confirm the original source hash did not change;
7. confirm no transaction, UI or hook process remains;
8. restore the frontend state used before the test.

## Known display behavior

- Mali/fbdev firmware generally auto-selects `mali`.
- Direct DRM firmware generally auto-selects `KMSDRM` once the frontend has
  released DRM master.
- Wayland is attempted only when a real Wayland socket exists.
- An `offscreen` or `dummy` driver is never accepted as visible success.
- A UI failure is non-fatal; extraction continues headless.

Service control is a laboratory-only test step. It must never be copied into a
distributed port launcher.

## Report template

```text
Family / OS:
Architecture:
glibc:
Python:
SDL drivers:
Selected backend:
Resolution visibly checked:
Install:
Full verify:
Second run:
Source hash preserved:
Required workaround:
```
