# Contributing to NXExtract

Contributions are welcome under the project’s MIT license.

## Start here

Use [GitHub Issues](https://github.com/NextOs-Ports/NXExtract/issues) for a
reproducible bug or a proposal. For code:

1. fork `NextOs-Ports/NXExtract`;
2. create a focused branch;
3. add a synthetic regression test;
4. run the checks below;
5. open a [pull request](https://github.com/NextOs-Ports/NXExtract/pulls).

Device results that do not need a code change can use the repository's
**Device compatibility report** issue form.

## Ground rules

- Never commit APKs, APKMs, APKS files, XAPKs, OBBs, game libraries, game
  assets, credentials, private IP addresses or raw device logs.
- Use synthetic fixtures in tests.
- Never identify an input by its external filename. Match package structure,
  internal paths, ABI and validated content.
- A hook writes only inside `NXEXTRACT_STAGE`; it must not mutate live game
  data before NXExtract commits the transaction.
- Every distributed AArch64 ELF must pass
  `tools/check-glibc.sh` and require GLIBC 2.30 or older.
- Launchers remain foreground processes and must not stop, mask or restart the
  frontend.

## Before sending a change

```bash
./ui/build-ui.sh
./tools/check-release.sh
./tools/check-glibc.sh ./ui/build/nxextract-ui
```

Add or update a synthetic test for every parser, planner, validation,
transaction or recovery change.

For a release, update `VERSION`, `NXEXTRACT_VERSION` in `nxextract.py` and the
changelog together. `tools/check-release.sh` rejects a mismatch.

## Reporting a new device

Update `docs/DEVICE-COMPATIBILITY.md` with:

- device family and operating system, without a private IP or hostname;
- architecture, glibc and Python versions;
- SDL video drivers advertised by that firmware;
- backend actually selected by NXExtract;
- resolution and whether the UI was visibly confirmed;
- install, full verify and second-run results;
- any required fix, including the smallest reproducible synthetic fixture.

Mark untested assumptions as “expected”, never “verified”.

For general discussion and community testing, use the
[NextOS Discord](https://discord.gg/DHfY62eDNN). Security vulnerabilities must
follow [SECURITY.md](SECURITY.md), not a public issue.
