# Recipe reference

Every port ships one trusted JSON recipe. The recipe describes valid content;
it never names the user’s external APK file.

## Root fields

| Field | Required | Meaning |
|---|---:|---|
| `schema` | yes | recipe schema, currently `1` |
| `id` | yes | stable safe identifier used for workspace names |
| `version` | yes | bump when accepted data or bake output changes |
| `title` | no | text displayed by the UI |
| `abi_order` | no | preferred Android ABIs |
| `input` | no | discovery directories and safety limits |
| `extract` | yes | payload-selection rules |
| `hooks` | no | bake commands executed without a shell |
| `validate` | no | additional final-output checks |
| `commit` | yes | non-overlapping roots published together |
| `marker` | no | completion marker path |
| `space` | no | free-space safety reserve |
| `log` | no | relative log path |

`id` and `title` identify the port and UI. Neither is an expected package
filename.

## Input discovery

Common `input` fields:

| Field | Default | Meaning |
|---|---|---|
| `search_dirs` | `["gamedata", "."]` | relative directories searched in order |
| `prefer_first_nonempty` | `true` | stop after the first directory with candidates |
| `sniff_all_in_primary` | `true` | inspect extensionless files in the first directory |
| `extensions` | APK/bundle/ZIP/OBB set | additional candidate extensions |
| `max_files` | `128` | candidate-file limit |
| `max_bundle_apks` | `128` | inner-APK limit |
| `max_member_bytes` | 8 GiB | maximum expanded inner APK |
| `max_bundle_bytes` | 16 GiB | maximum total inner APK bytes |

An explicit repeated `--input` bypasses directory discovery but not content
classification or validation.

## Extraction rules

Each `extract` item has a unique `id`, a `source`, a `destination` and optional
`validate`, `required` and `mode`.

Source kinds:

| `kind` | Selects |
|---|---|
| `entry` | one member inside an APK or bundle archive |
| `entries` | a member tree |
| `file` | one loose file |
| `entry_or_file` | either archive member or loose file |

Useful source fields:

- `patterns`: ordered glob patterns; `{abi}` is expanded;
- `scopes`: `apk`, `bundle` or both;
- `strip_prefix`: prefix removed from a selected tree;
- `flatten`: discard source directories;
- `file_extensions`: candidate loose-file extensions;
- `case_sensitive`: matching policy.

Single-file destinations support `{abi}` and `{basename}`. Tree destinations
support `{abi}`.

## Validators

File validators:

- exact `size`, `min_size`, `max_size`;
- `crc32` or `sha256`, each accepting one value or a list;
- `magic_ascii`, `magic_hex`, `magic_offset`;
- `elf_machine`: `arm64-v8a`, `armeabi-v7a`, `armeabi`, `x86_64`, `x86` or
  `{abi}`. The template resolves to the ABI currently being evaluated, so one
  recipe can strongly validate both ARM64 and ARMv7 payloads.

Tree validators:

- `exact_files`, `min_files`, `max_files`;
- `exact_bytes`, `min_bytes`, `max_bytes`;
- `required_paths`;
- `tree_fingerprint`.

Use SHA-256 for critical libraries, OBBs and bake sentinels. Count-only
validation is appropriate only when version drift is harmless.

## Hooks

A hook is an argv array, not shell text:

```json
{
  "id": "texture-bake",
  "argv": [
    "{game_dir}/tools/texture-bake",
    "--source",
    "{stage}/assets"
  ],
  "cwd": "{game_dir}",
  "checkpoint": [
    {
      "path": "assets/.texture-bake-ok",
      "type": "file",
      "sha256": "REAL_SHA256"
    }
  ]
}
```

Templates:

- `{game_dir}`;
- `{stage}`;
- `{workspace}`;
- `{recipe_dir}`;
- `{abi}`.

The hook also receives `NXEXTRACT_GAME_DIR`, `NXEXTRACT_STAGE`,
`NXEXTRACT_WORKSPACE`, `NXEXTRACT_ABI` and `NXEXTRACT_PROGRESS_FILE`.

Every compiled AArch64 hook shipped with a public port must pass:

```bash
./tools/check-glibc.sh path/to/hook
```

## Versioning

The marker contains a digest of the whole recipe. Any recipe edit invalidates
the fast path. Bump `version` when users or maintainers need that change to be
visible in UI and support reports.
