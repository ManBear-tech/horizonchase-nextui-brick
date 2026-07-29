# NXExtract architecture

NXExtract separates policy, extraction and presentation so a new game changes
data, not extractor code.

## Components

| Component | Responsibility |
|---|---|
| `nxextract.py` | discovery, planning, validation, staging and transactions |
| `extractor.json` | per-port paths, ABIs, validators, commit roots and hooks |
| `nxextract-ui` | optional SDL2 progress renderer |
| hook | optional game-specific bake that writes only to the stage |
| `run-extractor.sh` | generic foreground launcher |

The Python core uses only the standard library and supports Python 3.7 or
newer. The UI loads the firmware’s SDL2 dynamically and is built as an
AArch64 ELF requiring GLIBC 2.30 or older.

## Pipeline

```text
discover by content
  → classify APK / bundle / archive / loose file
  → parse Android package and split identity
  → evaluate recipe for each ABI
  → reject zero or multiple different payload plans
  → preflight only bytes still missing from the stage
  → copy with partial files, CRC and fsync
  → run resumable hooks in the stage
  → fully validate the staged result
  → publish commit roots with journal + backup + rename
  → write marker and remove temporary source cache
```

External filenames are metadata for logs only. They are never a game identity.

## Candidate identity

Direct split APKs are grouped only when their parsed
`AndroidManifest.xml` package matches. A bundle is expanded into a
same-filesystem cache, then its inner APKs pass through the same planner.

Each successful plan is fingerprinted from rule, destination, size and CRC.
Equivalent copies are harmless; two different fingerprints are an ambiguity
and stop the install.

## Transaction states

The live payload is untouched while extraction and baking run.

| Crash point | Next-run action |
|---|---|
| while copying | valid staged files resume; partial file is replaced |
| while running a hook | validated hook checkpoint resumes, otherwise hook reruns |
| before publication | stage remains; live payload is unchanged |
| after backing up a live root | journal restores the backup |
| after installing some roots | installed roots return to stage, backups return live |
| after marker publication | transaction cleanup completes; published data remains |

The journal is fsynced after every path-state transition. Commit roots may not
overlap.

## Progress protocol

The core atomically writes:

```text
STATE OVERALL 1000
MESSAGE
NXEXTRACT_V1 PHASE OVERALL PHASE_PROGRESS DONE_BYTES TOTAL_BYTES
DETAIL
```

Hooks report:

```text
NXEXTRACT_PROGRESS DONE TOTAL OPTIONAL DETAIL
```

The UI is deliberately non-critical. If SDL2 or a visible renderer is
unavailable, extraction continues headless with identical transaction
guarantees.

## Display negotiation

The UI first honors a valid backend inherited from the launcher, then lets
SDL2 auto-select. `dummy`, `offscreen` and non-display backends are rejected.
If automatic selection is invisible, NXExtract enumerates only backends
advertised by that SDL build and tries those compatible with the current
session:

- Wayland only when its socket exists;
- X11 only when `DISPLAY` exists;
- KMSDRM only when a DRM card exists;
- fbdev/Mali-class drivers only when a framebuffer exists.

Any environment adjustment exists only inside the UI child process.
