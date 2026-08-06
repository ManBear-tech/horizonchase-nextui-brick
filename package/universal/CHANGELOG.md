# Changelog

## 1.2.2

- Conserta o controle que parava de funcionar DENTRO DA CORRIDA depois de
  importar o perfil do Android (menus continuavam respondendo). O perfil
  guarda também o esquema de controle do celular, no campo `InputType`, que
  indexa o `EInputType` do próprio jogo: tudo abaixo de 7 dirige por toque na
  tela ou inclinando o aparelho. A importação levava esse valor junto com o
  progresso, então um perfil comprado caía em `AutoAccelerate1` (toque) e o
  carro deixava de responder ao controle — os menus seguiam funcionando
  porque passam pelos hooks de `GamepadInputSource` do port, e o carro não.
- O esquema de controle agora é do aparelho, igual à resolução e ao áudio: a
  importação mantém o que o port já usava (o R36S fica em
  `AndroidRemoteAutoAccelerate1`) e nunca traz o do celular. Progresso,
  compra e DLC continuam vindo intactos.
- Quem já importou numa versão anterior não consegue consertar reimportando —
  o arquivo de origem é consumido no primeiro uso. Por isso o launcher
  confere o esquema em toda abertura e conserta o save instalado uma vez,
  sem tocar em mais nada. Save já num esquema de controle não é reescrito e
  não imprime nada.

## 1.2.1

- Aceita o perfil do Android no formato que o aparelho realmente grava. O
  backend PlayerPrefs v2 da Unity no celular (o que carimba
  `__UNITY_PLAYERPREFS_VERSION__`) escapa chaves e valores em porcentagem
  antes de chegarem ao SharedPreferences, então o `user_profile` de um
  celular de verdade vem como `%7B%22UniqueUserID%22...` e não como JSON cru.
  O importador só aceitava JSON cru e recusava o perfil legítimo com
  "`user_profile` nao e JSON valido" — o jogador largava o XML em `gamedata/`
  e nada acontecia.
- No port, porém, o jogo lê e grava o `user_profile` na forma crua (v1) — o
  desescape do celular está preso ao marcador de versão do PlayerPrefs, que o
  filtro de escopo descarta de propósito. Então a importação decodifica na
  entrada e grava JSON puro, validado num aparelho de verdade com perfil
  comprado: guardar os bytes escapados fazia o jogo abrir num perfil zerado.
- A chave `user_profile` também é reconhecida escapada, tanto na varredura do
  `gamedata/` quanto na importação.

## 1.2.0

- Fixes the black/flickering picture seen on Amlogic-ng boxes (S922X,
  Mali-G52 with the fbdev blob at 1080p) while keeping every proven device
  byte-for-byte on its 1.0.4 behavior:
  - The opaque-backbuffer alpha clear now also runs when the GLES3 driver
    presents with a framebuffer object still bound. The Amlogic OSD blends
    fb0 by pixel alpha, and skipping that clear let frames with alpha zero
    scan out as black. On Utgard this branch never executes.
  - The loader now probes the two fb0 pages after boot. If the panel pan
    alternates while one page never receives a frame, the rendered page is
    mirrored onto the dead one after every present (the proven two-page
    fbdev recipe). On healthy devices the probe observes both pages carrying
    content and shuts itself off seconds in; KMSDRM devices skip all of it.
  - `HC_FB_MIRROR=0` disables the probe, `HC_FB_MIRROR=1` forces the mirror
    for field diagnosis; both paths report what they saw in the log.

## 1.1.0-beta.1 / 1.1.0-beta.2 (folded into 1.2.0)

- A player who owns Horizon Chase on Android can bring their own profile over.
  The game is free-to-play and releases everything past the demo through an
  entitlement stored in the player's profile rather than in the files on disk;
  that entitlement is granted by the Google Play purchase flow, which an
  offline port has no bridge to, so a fresh install started on the demo even
  for an owner.
- Dropping `com.aquiris.horizonchase.v2.playerprefs.xml` into `gamedata/` is
  now enough. The launcher validates it, reports what it carries, merges it
  into `userdata/shared_prefs.bin` and moves the source to
  `userdata/save-imports/`. The source is consumed once, so a stale phone
  profile cannot overwrite progress made on the handheld later.
- Nothing is synthesized. The port releases only what the player's own profile
  already proves; a profile without the purchase imports its progress, stays
  on the demo and says so.
- DLC campaigns are reported the same way. Their content ships inside the APK,
  so a profile that carries them runs them offline. Ownership is read from the
  campaign's own progress rather than from a flag: a profile that owns nothing
  already carries every DLC container, zeroed, and even carries
  `IsEasyUnlocked`, so only a race the player actually ran is proof. Campaigns
  added in a later build are recognized by shape instead of being ignored.
- Only the profile is imported. The rest of a phone's SharedPreferences
  describes that phone — resolution, quality, audio routing — and previously
  would have overridden the settings tuned for the handheld.
- The importer refuses to write a preferences file that would break the
  loader's own limits. Exceeding them makes the loader drop every entry, which
  would have looked like a lost save rather than a rejected import.
- A failed or skipped import never blocks the game from starting.

## 1.0.4

- A fully installed game is no longer reported as a failed install. NXExtract
  1.1.3 drops its scratch source cache only after every source archive is
  closed, and a removal the filesystem still refuses is logged instead of
  aborting the run. On FUSE-backed shares — exFAT as Knulli and Batocera use it
  for `/userdata`, plus NFS and SMB — a file unlinked while still open leaves a
  hidden placeholder behind, so `source-cache/bundle-*` answered
  `[Errno 39] Directory not empty` seconds after the payload had been committed
  and validated. The launcher then stopped with
  `owner-data setup failed status=1` even though the extraction had finished
  correctly.
- The post-extraction data gate is unchanged and still refuses to start the
  loader with an incomplete payload.

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
