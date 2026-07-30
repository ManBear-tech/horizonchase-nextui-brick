# Changelog

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
