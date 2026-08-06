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
import urllib.parse
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
    return unescape_key(key) == PROFILE_KEY or key.startswith(PROFILE_KEY + "_")


def unescape_key(key):
    """The key as the profile system names it, whatever the spelling on disk."""
    return urllib.parse.unquote(key) if "%" in key else key


def profile_json(text):
    """Parse the profile string, escaped or not.

    Unity's PlayerPrefs v2 backend on the phone percent-escapes keys and
    values before they reach SharedPreferences, so a profile pulled off a real
    device arrives as `%7B%22UniqueUserID%22...` rather than as bare JSON.
    The unescape on the way back is tied to the `__UNITY_PLAYERPREFS_VERSION__`
    marker, which the import's scope filter drops on purpose -- and the port's
    own storage is the plain v1 form anyway: the game reads and writes
    `user_profile` as bare JSON there (the bench unlock always proved that
    in-game).  So this reads through either spelling, and everything the port
    writes is written PLAIN -- see profile_text().
    """
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        if "%" not in text:
            raise
    return json.loads(urllib.parse.unquote(text))


def profile_text(profile):
    """Serialize a profile the way the port's game stores it: bare JSON."""
    return json.dumps(profile, separators=(",", ":"))


# The profile also carries WHICH control scheme the player uses -- the phone's
# one.  `InputType` indexes Horizon's own EInputType (read out of the game's
# global-metadata):
#
#   0 Classic1  1 Classic2  2 AutoAccelerate1  3 AutoAccelerate2
#   4 AutoAccelerate3  5 Tilt1  6 Tilt2  7 Controller
#   8 AppleRemoteTilt2  9 AppleRemoteTilt1  10 AppleRemoteAutoAccelerate1
#   11 AndroidRemoteClassic1  12 AndroidRemoteAutoAccelerate1
#   13 AndroidRemoteAutoAccelerate2
#
# Everything below 7 steers by touching the screen or by tilting the phone --
# hardware no handheld running this port has.  The port's own profile, made on
# the R36S, sits on 12 (Android remote = physical pad).  Importing a phone
# profile used to drop the phone's value straight in, so a purchased profile
# landed on AutoAccelerate1 and the car stopped answering the pad DURING THE
# RACE while the menus kept working -- the menus come through the port's
# GamepadInputSource hooks, the car does not.
#
# So the scheme is device-local, exactly like resolution and audio routing:
# the import never takes it from the phone, and a profile already carrying an
# untouchable scheme is healed on launch.  Nothing else in the profile is
# rewritten -- progress and entitlements come across verbatim.
INPUT_TYPE_KEY = "InputType"
PAD_INPUT_TYPES = (7, 11, 12, 13)
PORT_INPUT_TYPE = 12
INPUT_TYPE_NAMES = {
    0: "Classic1 (toque)", 1: "Classic2 (toque)",
    2: "AutoAccelerate1 (toque)", 3: "AutoAccelerate2 (toque)",
    4: "AutoAccelerate3 (toque)", 5: "Tilt1 (inclinacao)",
    6: "Tilt2 (inclinacao)", 7: "Controller",
    8: "AppleRemoteTilt2", 9: "AppleRemoteTilt1",
    10: "AppleRemoteAutoAccelerate1", 11: "AndroidRemoteClassic1",
    12: "AndroidRemoteAutoAccelerate1", 13: "AndroidRemoteAutoAccelerate2",
}


def input_type_name(value):
    return INPUT_TYPE_NAMES.get(value, f"desconhecido ({value})")


def playable_input_type(profile):
    """The scheme in this profile, if it is one a pad can drive."""
    value = profile.get(INPUT_TYPE_KEY)
    return value if isinstance(value, int) and value in PAD_INPUT_TYPES else None


def heal_input_type(profile, keep=None, prefix="[save]"):
    """Force a pad-drivable control scheme. Returns True if it changed.

    `keep` is the scheme the port was already using (the local profile's), so
    a player who picked a different pad layout in the game's own options keeps
    it.  A profile with no InputType at all is left alone: the game picks the
    scheme itself for a fresh profile, which is how every install before the
    import feature ended up on a working one.
    """
    current = profile.get(INPUT_TYPE_KEY)
    if not isinstance(current, int) or current in PAD_INPUT_TYPES:
        return False
    wanted = keep if keep in PAD_INPUT_TYPES else PORT_INPUT_TYPE
    profile[INPUT_TYPE_KEY] = wanted
    print(
        f"{prefix} controle: perfil vinha em {input_type_name(current)}, "
        f"que so funciona com tela/sensor -- ajustado para "
        f"{input_type_name(wanted)}"
    )
    return True


def stored_profile(entries):
    """The profile already in the port's prefs, parsed, or None."""
    entry = find_profile_entry(entries)
    if entry is None or not entry.sval:
        return None
    try:
        profile = profile_json(entry.sval)
    except (json.JSONDecodeError, ValueError):
        return None
    return profile if isinstance(profile, dict) else None


def heal_stored_input_type(gamedir, prefix="[save]"):
    """Repair an install whose stored profile carries a touch/tilt scheme.

    The import consumes its source file, so an install already broken by an
    earlier import cannot be fixed by importing again -- the launcher runs
    this on every start instead.  It writes only when there is something to
    fix, and touches nothing but InputType.
    """
    dest = os.path.join(gamedir, "userdata", "shared_prefs.bin")
    try:
        entries = read_prefs(dest)
    except (SaveError, OSError):
        return False
    profile = stored_profile(entries)
    if profile is None or not heal_input_type(profile, prefix=prefix):
        return False
    healed = Entry()
    healed.set_string(profile_text(profile))
    entries[find_profile_key(entries)] = healed
    write_prefs(dest, entries)
    return True


def find_profile_key(entries):
    """The key the profile is stored under, whatever the spelling."""
    if PROFILE_KEY in entries:
        return PROFILE_KEY
    for key in entries:
        if unescape_key(key) == PROFILE_KEY:
            return key
    return PROFILE_KEY


def find_profile_entry(entries):
    """The profile entry, under whatever spelling the source used."""
    entry = entries.get(PROFILE_KEY)
    if entry is not None:
        return entry
    for key, candidate in entries.items():
        if unescape_key(key) == PROFILE_KEY:
            return candidate
    return None


def check_profile(entries, origin):
    """Refuse anything that is not a Horizon Chase profile."""
    entry = find_profile_entry(entries)
    if entry is None or not entry.sval:
        raise SaveError(
            f"{origin}: nao tem a chave '{PROFILE_KEY}' -- nao e um save do "
            "Horizon Chase (confira se puxou o playerprefs.xml certo)"
        )
    try:
        profile = profile_json(entry.sval)
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
    # The phone stores these percent-escaped (PlayerPrefs v2); the port's game
    # reads them plain (v1).  Land them in the form the game actually reads.
    for key, entry in selected.items():
        if not (in_profile_scope(key) and entry.sval and "%" in entry.sval):
            continue
        plain = Entry()
        if unescape_key(key) == PROFILE_KEY:
            plain.set_string(profile_text(profile_json(entry.sval)))
        else:
            plain.set_string(urllib.parse.unquote(entry.sval))
        selected[key] = plain
    selected = {unescape_key(key): entry for key, entry in selected.items()}
    userdata = os.path.join(gamedir, "userdata")
    os.makedirs(userdata, exist_ok=True)
    dest = os.path.join(userdata, "shared_prefs.bin")
    merged = read_prefs(dest)
    # The control scheme belongs to this handheld, not to the phone the
    # profile came from: keep whatever the port was already using and never
    # let a touch/tilt scheme ride in with the progress.
    local = stored_profile(merged)
    keep = playable_input_type(local) if local else None
    profile_key = find_profile_key(selected)
    incoming_entry = selected.get(profile_key)
    if incoming_entry is not None and incoming_entry.sval:
        try:
            profile = profile_json(incoming_entry.sval)
        except (json.JSONDecodeError, ValueError):
            profile = None
        if isinstance(profile, dict) and heal_input_type(profile, keep, prefix):
            fixed = Entry()
            fixed.set_string(profile_text(profile))
            selected[profile_key] = fixed
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
    if b"<map" not in head:
        return False
    # The key can arrive percent-escaped, like everything Unity's PlayerPrefs
    # v2 backend writes.  Sniff both spellings.
    escaped = urllib.parse.quote(PROFILE_KEY, safe="").encode()
    return PROFILE_KEY.encode() in head or escaped in head


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
    for source in auto_candidates(gamedir):
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
    # An install imported by an older version may still be sitting on the
    # phone's touch scheme with its source already consumed, so the scheme is
    # checked on every launch and not only when something is dropped.  A save
    # already on a pad scheme is not rewritten and says nothing.
    try:
        heal_stored_input_type(gamedir)
    except (SaveError, OSError) as exc:
        print(f"[save] ajuste de controle ignorado: {exc}", file=sys.stderr)
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
    parser.add_argument(
        "--heal-input", action="store_true",
        help="so conserta o esquema de controle do save ja instalado",
    )
    args = parser.parse_args()

    if args.heal_input:
        try:
            if not heal_stored_input_type(args.gamedir, prefix="[save]"):
                print("[save] esquema de controle ja esta ok")
        except (SaveError, OSError) as exc:
            print(f"erro: {exc}", file=sys.stderr)
            return 1
        return 0
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
