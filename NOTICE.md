# Horizon Chase — NextOS loader notices

The compatibility loader in this directory is part of
`nextos_ports_android`, Copyright 2026 felc18-blip, and is distributed under
GNU GPL version 3. The repository root contains the complete license and
corresponding source; release packages also preserve it as
`licenses/GPL-3.0.txt`.

The reusable Android compatibility-loader framework includes work derived
from:

- `syberia_arm64` and `lswtcs_arm64` by mtojek, Apache License 2.0;
- the `max_arm64` lineage by Jaakko Lukkari, fgsfds and Andy Nguyen;
- interoperability work from initdream's Crazy Taxi loader;
- `cod-boz-port` by Producdevity, MIT License; the complete notice is in
  `licenses/Producdevity-MIT.txt`.

`src/hashmap.h` is `sheredom/hashmap.h`, released into the public domain under
the Unlicense notice preserved in that file and in `licenses/Unlicense.txt`.

`libastcUtil.so` is a separately linked build of Arm `astcenc`. Copyright Arm
Limited and contributors, licensed under Apache License 2.0. The complete
Apache 2.0 text is in `licenses/Apache-2.0.txt`. The public source is available
from <https://github.com/ARM-software/astc-encoder>.

SDL2, EGL, GLES and the standard C libraries are supplied by the target
firmware and are not bundled as part of this loader.

Horizon Chase, its Unity and IL2CPP native libraries, scenes, textures,
artwork, music, sound effects and other game data are proprietary works of
their respective rightsholders. They are separate from this compatibility
loader, are not covered by its licenses, and are not distributed in the
public source tree. Users must supply files from their own legitimate Android
installation.

This is an independent interoperability project. It is not affiliated with
or endorsed by Aquiris, Epic Games, Unity Technologies or the other
rightsholders.
