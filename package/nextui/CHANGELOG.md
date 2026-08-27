# Changelog

## 1.2.2-brick.1

- Adds a TrimUI Brick / NextUI package layout rooted at
  `/mnt/SDCARD/Roms/Ports (PORTS)/.ports/horizonchase`.
- Keeps EGL contexts under SDL ownership on NextUI's misleadingly named
  `mali` backend, which actually drives the Brick's PowerVR Rogue GE8300.
- Normalizes default-backbuffer alpha immediately before SDL presents the
  frame, fixing the road disappearing during live gameplay.
- Defaults to a 60 Hz Unity lifecycle cadence without modifying game
  `timeScale`; `HC_FRAME_LIMIT=30` remains available as a fallback.
- Pins manual input scheme 11. The bottom Xbox-layout face button accelerates
  and D-pad Down brakes; no temporary save-editing block is required.
- Does not enable `HC_GLSTATE_TRACE`; that switch is diagnostic-only.

Based on Horizon Chase NextOS port 1.2.2. See the Git history and `NOTICE.md`
for upstream authorship, third-party notices, and license details.
