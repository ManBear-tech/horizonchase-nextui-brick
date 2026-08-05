#!/usr/bin/env python3
"""Carry a Horizon Chase profile from Android into the port.

Horizon Chase is free-to-play: the APK ships every track, but everything past
the demo is released by an entitlement stored in the player's profile, not by
the files on disk.  That entitlement is granted by the Google Play purchase
flow, which this offline port has no bridge to, so a fresh install starts on
the demo exactly like a phone that never bought the game.

The whole profile -- progress and the entitlements already attached to it --
lives in a single Unity PlayerPrefs entry, `user_profile`.  On Android that
sits in SharedPreferences; the port stores the same key/value pairs in
`<gamedir>/userdata/shared_prefs.bin`.  This converts one into the other, so a
player carries their own purchase across without the port ever contacting a
store.

Nothing is ever synthesized: the port unlocks only what the player's own
profile already proves.  A profile without the purchase imports its progress
and stays on the demo.

Input is either the SharedPreferences XML pulled from the device
(`com.aquiris.horizonchase.v2.playerprefs.xml`) or a bare `user_profile` JSON.

With `--auto` the drop folder is scanned and any profile found there is
imported once and then moved aside.  That is how the launcher runs it, so on a
handheld the player only has to drop the file next to the APK.
"""

import argparse
import json
import os
import struct
import sys
import xml.etree.ElementTree as ET

MAGIC = b"HCPREF2\x00"
PROFILE_KEY = "user_profile"

# key_len, string_len, flags, ival, fval, lval, bval -- byte for byte the field
# order prefs_save_locked() writes in src/jni_shim.c, with no padding.
RECORD = "<IIBifqi"

# Bit set stored per entry; mirrors `flags` in src/jni_shim.c.
HAS_S, HAS_I, HAS_F, HAS_L, HAS_B = 1, 2, 4, 8, 16

# Limits prefs_load() enforces in src/jni_shim.c.  A file that breaks any of
# them is not partially loaded -- the loader drops every entry and the player
# silently loses the save.  Refuse to write one instead.
MAX_PREFS = 4096
MAX_KEY_LEN = 65535
MAX_STRING_LEN = 16 * 1024 * 1024 - 1

# Where --auto looks, and where a consumed source is kept afterwards.
DROP_DIR = "gamedata"
ARCHIVE_DIR = "userdata/save-imports"


class SaveError(Exception):
    """A source that cannot be imported. Fatal by hand, reported under --auto."""


class Entry:
    def __init__(self):
        self.sval = None
        self.ival = 0
        self.fval = 0.0
        self.lval = 0
        self.bval = 0
        self.flags = 0

    def set_string(self, value):
        self.sval = value
        self.flags |= HAS_S

    def set_int(self, value):
        self.ival = value
        self.flags |= HAS_I

    def set_float(self, value):
        self.fval = value
        self.flags |= HAS_F

    def set_long(self, value):
        self.lval = value
        self.flags |= HAS_L

    def set_bool(self, value):
        self.bval = 1 if value else 0
        self.flags |= HAS_B


def read_prefs(path):
    """Parse an existing shared_prefs.bin. Missing/!HCPREF2 file -> empty."""
    entries = {}
    try:
        with open(path, "rb") as handle:
            blob = handle.read()
    except FileNotFoundError:
        return entries
    if len(blob) < 12 or blob[:8] != MAGIC:
        raise SaveError(f"{path}: nao e um shared_prefs.bin do port (magic)")
    (count,) = struct.unpack_from("<I", blob, 8)
    off = 12
    for _ in range(count):
        key_len, str_len, flags, ival, fval, lval, bval = struct.unpack_from(
            RECORD, blob, off
        )
        off += struct.calcsize(RECORD)
        key = blob[off:off + key_len].decode("utf-8", "replace")
        off += key_len
        entry = Entry()
        entry.flags, entry.ival, entry.fval = flags, ival, fval
        entry.lval, entry.bval = lval, bval
        if str_len:
            entry.sval = blob[off:off + str_len].decode("utf-8", "replace")
            off += str_len
        entries[key] = entry
    return entries


def write_prefs(path, entries):
    if len(entries) > MAX_PREFS:
        raise SaveError(
            f"{len(entries)} chaves excedem o limite de {MAX_PREFS} do loader; "
            "o jogo recusaria o arquivo inteiro (use o escopo padrao, sem --all)"
        )
    out = bytearray(MAGIC)
    out += struct.pack("<I", len(entries))
    for key, entry in entries.items():
        key_bytes = key.encode("utf-8")
        str_bytes = entry.sval.encode("utf-8") if entry.sval else b""
        if not key_bytes or len(key_bytes) > MAX_KEY_LEN:
            raise SaveError(f"chave de tamanho invalido: {len(key_bytes)} bytes")
        if len(str_bytes) > MAX_STRING_LEN:
            raise SaveError(
                f"valor de '{key}' tem {len(str_bytes)} bytes e passa do "
                f"limite de {MAX_STRING_LEN} do loader"
            )
        out += struct.pack(
            RECORD,
            len(key_bytes), len(str_bytes), entry.flags,
            entry.ival, entry.fval, entry.lval, entry.bval,
        )
        out += key_bytes + str_bytes
    tmp = path + ".tmp"
    with open(tmp, "wb") as handle:
        handle.write(out)
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(tmp, path)


def entries_from_xml(path):
    """Read an Android SharedPreferences XML into our entry map."""
    try:
        root = ET.parse(path).getroot()
    except ET.ParseError as exc:
        raise SaveError(f"{path}: XML invalido ({exc})")
    entries = {}
    for node in root:
        name = node.get("name")
        if not name:
            continue
        entry = entries.setdefault(name, Entry())
        try:
            if node.tag == "string":
                entry.set_string(node.text or "")
            elif node.tag == "int":
                entry.set_int(int(node.get("value", "0")))
            elif node.tag == "float":
                entry.set_float(float(node.get("value", "0")))
            elif node.tag == "long":
                entry.set_long(int(node.get("value", "0")))
            elif node.tag == "boolean":
                entry.set_bool(node.get("value", "false") == "true")
        except ValueError as exc:
            raise SaveError(f"{path}: valor invalido em '{name}' ({exc})")
    return entries


def entries_from_json(path):
    with open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    entry = Entry()
    entry.set_string(text)
    return {PROFILE_KEY: entry}


def read_source(path):
    if path.lower().endswith(".xml"):
        return entries_from_xml(path)
    return entries_from_json(path)


def in_profile_scope(key):
    """Keys the profile system owns.

    Everything else in a phone's SharedPreferences describes that phone --
    resolution, quality, audio routing -- and would overwrite the settings the
    port tuned for the handheld.
    """
    return key == PROFILE_KEY or key.startswith(PROFILE_KEY + "_")


def check_profile(entries, origin):
    """Refuse anything that is not a Horizon Chase profile."""
    entry = entries.get(PROFILE_KEY)
    if entry is None or not entry.sval:
        raise SaveError(
            f"{origin}: nao tem a chave '{PROFILE_KEY}' -- nao e um save do "
            "Horizon Chase (confira se puxou o playerprefs.xml certo)"
        )
    try:
        profile = json.loads(entry.sval)
    except json.JSONDecodeError as exc:
        raise SaveError(f"{origin}: '{PROFILE_KEY}' nao e JSON valido ({exc})")
    if not isinstance(profile, dict):
        raise SaveError(f"{origin}: '{PROFILE_KEY}' nao e um objeto JSON")
    known = ("UserProfileVersion", "RevisionNumber", "Cups", "Races")
    present = [field for field in known if field in profile]
    if len(present) < 2:
        raise SaveError(
            f"{origin}: '{PROFILE_KEY}' nao parece um UserProfile do Horizon "
            f"Chase (nenhum campo conhecido entre {', '.join(known)})"
        )
    return profile


# Each DLC campaign keeps its own progress container in the profile.  A demo
# profile ALREADY carries these containers, fully zeroed, and even carries
# "unlocked" flags such as AyrtonChampionshipSaveData.IsEasyUnlocked -- so
# their presence proves nothing.  Progress does: a race with TimesRaced or a
# score can only exist if the player owned that campaign when they ran it.
DLC_LABELS = {
    "AyrtonCareerSaveData": "Senna Forever (carreira)",
    "AyrtonChampionshipSaveData": "Senna Forever (campeonato)",
    "SummerSaveData": "Summer Vibes",
    "TurboCarsDLCSaveData": "Turbo Cars",
}


def dlc_evidence(profile):
    """DLC campaigns the profile itself proves were played.

    Discovered by shape, not by a hardcoded list, so a campaign added in a
    later build is still recognized instead of silently ignored.
    """
    found = {}
    for key, value in sorted(profile.items()):
        if not key.endswith("SaveData") or not isinstance(value, dict):
            continue
        races = value.get("RaceDataList")
        if not isinstance(races, list):
            continue
        played = 0
        for race in races:
            if not isinstance(race, dict):
                continue
            if (race.get("TimesRaced") or 0) > 0 or (race.get("Score") or 0) > 0:
                played += 1
        if played:
            found[DLC_LABELS.get(key, key)] = played
    return found


def entitlements(profile):
    """What the profile itself proves the player owns. Never invented here."""
    full = bool(profile.get("UnlockedFullGame"))
    products = [str(p) for p in (profile.get("UnlockedProducts") or []) if p]
    if not full:
        # Tolerate a build that spells the flag differently rather than
        # reporting "demo" for a profile that really carries the purchase.
        for key, value in profile.items():
            if "fullgame" in key.lower().replace("_", "") and value:
                full = True
                break
    return full, products


def describe(profile, prefix="[import]"):
    full, products = entitlements(profile)
    cups = profile.get("Cups") or []
    races = profile.get("Races") or []
    print(f"{prefix} revisao ..... {profile.get('RevisionNumber')}")
    print(f"{prefix} copas ....... {len(cups)}")
    print(f"{prefix} corridas .... {len(races)}")
    print(f"{prefix} tokens ...... {profile.get('NumberOfTokens', 0)}")
    print(f"{prefix} jogo completo {'SIM' if full else 'NAO'}")
    print(f"{prefix} produtos .... {', '.join(products) if products else '(nenhum)'}")
    dlcs = dlc_evidence(profile)
    if dlcs:
        for label, played in dlcs.items():
            print(f"{prefix} DLC .......... {label} ({played} corrida(s) no perfil)")
    else:
        print(f"{prefix} DLC .......... nenhum com progresso neste perfil")
    if not full and not products:
        print(
            f"{prefix} aviso: este perfil nao carrega a compra do jogo "
            "completo. O progresso entra, mas as corridas alem da demo "
            "continuam bloqueadas.",
            file=sys.stderr,
        )
    return full or bool(products)


def apply_import(gamedir, incoming, take_all, prefix="[import]"):
    """Merge the selected keys into the port's prefs. Returns keys written."""
    selected = {
        key: entry for key, entry in incoming.items()
        if take_all or in_profile_scope(key)
    }
    if not selected:
        raise SaveError("nada a importar depois do filtro de escopo")
    userdata = os.path.join(gamedir, "userdata")
    os.makedirs(userdata, exist_ok=True)
    dest = os.path.join(userdata, "shared_prefs.bin")
    merged = read_prefs(dest)
    if merged:
        backup = dest + ".bak"
        with open(dest, "rb") as source, open(backup, "wb") as target:
            target.write(source.read())
        print(f"{prefix} save anterior preservado em {backup}")
    merged.update(selected)
    write_prefs(dest, merged)
    skipped = len(incoming) - len(selected)
    if skipped:
        print(
            f"{prefix} {skipped} ajustes do aparelho Android ignorados "
            "(o port mantem os proprios)"
        )
    print(f"{prefix} {len(selected)} chave(s) gravada(s) em {dest}")
    return len(selected)


def archive_source(gamedir, source, prefix="[save]"):
    """Consume the drop file so the stale phone profile cannot come back.

    Re-importing on every launch would overwrite progress made on the handheld
    with whatever the phone had, so the source is moved aside after one use.
    """
    archive = os.path.join(gamedir, ARCHIVE_DIR)
    os.makedirs(archive, exist_ok=True)
    target = os.path.join(archive, os.path.basename(source))
    serial = 1
    while os.path.exists(target):
        root, ext = os.path.splitext(os.path.basename(source))
        target = os.path.join(archive, f"{root}.{serial}{ext}")
        serial += 1
    try:
        os.replace(source, target)
    except OSError:
        # gamedata and userdata may sit on different mounts.
        with open(source, "rb") as handle, open(target, "wb") as copy:
            copy.write(handle.read())
        os.unlink(source)
    print(f"{prefix} origem movida para {target}")


def looks_like_prefs(path):
    """Cheap sniff so a stray file in gamedata is not reported every launch."""
    try:
        with open(path, "rb") as handle:
            head = handle.read(4096)
    except OSError:
        return False
    if path.lower().endswith(".json"):
        return b'"' + PROFILE_KEY.encode() + b'"' in head or head.lstrip()[:1] == b"{"
    return b"<map" in head and PROFILE_KEY.encode() in head


def auto_candidates(gamedir):
    drop = os.path.join(gamedir, DROP_DIR)
    try:
        names = sorted(os.listdir(drop))
    except OSError:
        return []
    found = []
    for name in names:
        lowered = name.lower()
        if not lowered.endswith((".xml", ".json")):
            continue
        path = os.path.join(drop, name)
        if os.path.isfile(path) and looks_like_prefs(path):
            found.append(path)
    return found


def run_auto(gamedir, take_all):
    """Launcher path: import whatever the player dropped, never break boot."""
    candidates = auto_candidates(gamedir)
    if not candidates:
        return 0
    for source in candidates:
        name = os.path.basename(source)
        try:
            incoming = read_source(source)
            profile = check_profile(incoming, name)
            print(f"[save] perfil Android encontrado em {DROP_DIR}/{name}")
            describe(profile, prefix="[save]")
            apply_import(gamedir, incoming, take_all, prefix="[save]")
            archive_source(gamedir, source)
            print("[save] perfil importado; o progresso vale offline")
        except SaveError as exc:
            print(f"[save] ignorado: {exc}", file=sys.stderr)
        except OSError as exc:
            print(f"[save] falha de E/S em {name}: {exc}", file=sys.stderr)
    return 0


def main():
    parser = argparse.ArgumentParser(
        description="Importa um perfil do Horizon Chase do Android para o port."
    )
    parser.add_argument(
        "source", nargs="?",
        help="playerprefs.xml do aparelho Android, ou um user_profile.json",
    )
    parser.add_argument(
        "-g", "--gamedir", default=".",
        help="pasta do port (default: pasta atual)",
    )
    parser.add_argument(
        "-n", "--dry-run", action="store_true",
        help="so inspeciona a origem, nao escreve nada",
    )
    parser.add_argument(
        "--all", dest="take_all", action="store_true",
        help="importa tambem os ajustes do aparelho (sobrescreve os do port)",
    )
    parser.add_argument(
        "--auto", action="store_true",
        help=f"varre {DROP_DIR}/ e importa o que achar (usado pelo launcher)",
    )
    args = parser.parse_args()

    if args.auto:
        if args.source:
            parser.error("--auto nao aceita uma origem explicita")
        return run_auto(args.gamedir, args.take_all)
    if not args.source:
        parser.error("informe a origem, ou use --auto")

    try:
        incoming = read_source(args.source)
        profile = check_profile(incoming, args.source)
        describe(profile)
        if args.dry_run:
            return 0
        apply_import(args.gamedir, incoming, args.take_all)
    except SaveError as exc:
        print(f"erro: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
