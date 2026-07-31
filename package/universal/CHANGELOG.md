# Changelog

## 1.0.3

- ROCKNIX's inherited `SDL_AUDIODRIVER=pulseaudio` no longer enters a server
  backend that can block before returning an error. When there is no explicit
  Horizon audio override and SDL provides ALSA, the FMOD AudioTrack bridge
  selects ALSA before opening the device.
- Other firmware selections remain automatic. `HC_AUDIO_DRIVER` still has
  priority, while `HC_AUDIO_KEEP_INHERITED_PULSE=1` is an engineering escape
  hatch for testing an inherited PulseAudio route.
- Audio diagnostics now identify initialization, enumerated outputs, requested
  and obtained PCM formats, queue prefill and the first non-zero FMOD PCM peak.
- Owner-data setup no longer depends on `run-extractor.sh` retaining its Unix
  execute bit after a manual ZIP installation. The launcher invokes NXExtract
  explicitly through Bash and reports the marker and missing runtime payloads.
- A mandatory post-extraction data gate refuses to start the loader unless the
  Unity libraries, metadata, Android asset tree and generated asset pack are
  present. An incomplete install now produces an actionable launcher error
  instead of ending later at `so_load libunity FALHOU`.
- Short-lived `/proc/<pid>/cmdline` races are silenced during stale-instance
  detection without hiding a real matching Horizon process.
- The SDL-owned KMS/Wayland path now selects a real RGBA8888 EGLConfig instead
  of accepting the first RGBX8888 match. Unity 2022.3.33f1 requires exact
  8/8/8/8 channel sizes; Mesa/Panfrost returned 8/8/8/0 first and Unity
  intentionally stopped with `Unable to find a configuration matching minimum
  spec`.
- SDL video initialization is independent from audio initialization. A missing
  PulseAudio compatibility socket can no longer make the successful Wayland
  video startup look like a failed combined SDL initialization.
- Physically validated on an RG-DS running ROCKNIX with Wayland/Panfrost:
  first-run XAPK extraction completed, Unity reached the menu, ALSA delivered
  non-zero FMOD PCM with clear audio, the native controller mapping worked and
  `Select + Start` returned cleanly to the frontend.

## 1.0.2

- The in-game **QUIT GAME** dialog now really closes the port. Confirming it
  used to leave the process running with the music still playing and no input
  responding, because `Application.Quit()` only flags the Unity player as
  quitting and expects the Android Java activity to finish the process, which
  does not exist in this host. The loader now routes both `Application.Quit`
  overloads into the same shutdown the `Select + Start` hotkey uses, so the
  game saves, stops audio and returns to the frontend.

## 1.0.1

- Recipe `2.6.9-2`: accept both known Google Play builds of Horizon Chase
  2.6.9. Play ships two builds of the same version (versionCode 2054272382
  and 2054272383) whose `libunity.so` differ by a few bytes; the recipe now
  accepts either SHA-256. Setup no longer fails with
  `required payload libunity was not found` on split bundles taken from the
  second build.
- Recipe `2.6.9-2`: `assets/hr.txt` is no longer required. The file only
  exists in one modified repack of the game, is never read by the loader and
  broke setup with every original Google Play package.
- NXExtract 1.1.2: when files match a payload pattern but fail the
  size/sha256 validation, the error now says the input is probably a
  different build instead of claiming the payload was not found.

- First public BYO-data release.
- One AArch64 loader for Mali/fbdev, KMSDRM and Wayland-class backends.
- Physical validation on Mali-450, Mali-G31 and Mali-G310.
- Automatic GLES, texture-memory and SDL audio negotiation.
- Native controller support, persistent saves and `Select + Start` exit.
- Transactional first-run setup through NXExtract 1.1.1.
- Content-addressed migration for the private pre-release Swappy/asset-pack
  data layout.
