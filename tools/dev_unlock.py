#!/usr/bin/env python3
"""BANCADA INTERNA -- NAO VAI PARA RELEASE NENHUMA.

Marca o direito do jogo completo no `shared_prefs.bin` de um install de teste,
para conferir a fiacao do desbloqueio (menu, campanha, pistas) sem depender de
ter um perfil comprado em maos na hora do teste.

Isto NAO faz parte do port. O caminho publico e apenas
`import_android_save.py`, que copia o direito que o perfil do proprio jogador
ja carrega e nunca inventa nenhum.

Duas travas impedem este arquivo de vazar para um pacote:

  1. ele nao esta em `package/universal/package-files.txt`, e o build compara a
     arvore montada com essa lista e aborta na divergencia;
  2. `build-portmaster-package.sh` recusa explicitamente qualquer `dev_*.py`
     que apareca na arvore de staging.

Uso:
    python3 tools/dev_unlock.py -g /caminho/do/install-de-teste
    python3 tools/dev_unlock.py -g /caminho/do/install-de-teste --revert
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from import_android_save import (  # noqa: E402
    PROFILE_KEY,
    Entry,
    SaveError,
    read_prefs,
    write_prefs,
)

BENCH_PRODUCT = "bench.unlock"
# Guarda o valor real antes de mexer, para que --revert devolva o perfil ao que
# ele era -- inclusive quando a bancada e aplicada sobre um perfil comprado.
BENCH_SAVED = "_benchPreviousFullGame"
MINIMAL_PROFILE = {
    "UserProfileVersion": 1,
    "RevisionNumber": 1,
    "Cups": [],
    "Races": [],
    "NumberOfTokens": 0,
}


def load_profile(entries):
    entry = entries.get(PROFILE_KEY)
    if entry is None or not entry.sval:
        return dict(MINIMAL_PROFILE), False
    try:
        profile = json.loads(entry.sval)
    except json.JSONDecodeError as exc:
        raise SaveError(f"'{PROFILE_KEY}' existente nao e JSON valido ({exc})")
    if not isinstance(profile, dict):
        raise SaveError(f"'{PROFILE_KEY}' existente nao e um objeto JSON")
    return profile, True


def store_profile(entries, profile):
    entry = Entry()
    entry.set_string(json.dumps(profile, separators=(",", ":")))
    entries[PROFILE_KEY] = entry


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-g", "--gamedir", default=".",
                        help="install de teste (default: pasta atual)")
    parser.add_argument("--revert", action="store_true",
                        help="remove a marca de bancada e volta ao estado real")
    args = parser.parse_args()

    print("*** BANCADA INTERNA: nao distribuir este estado ***", file=sys.stderr)

    userdata = os.path.join(args.gamedir, "userdata")
    dest = os.path.join(userdata, "shared_prefs.bin")
    try:
        entries = read_prefs(dest)
        profile, existed = load_profile(entries)
        products = [str(p) for p in (profile.get("UnlockedProducts") or []) if p]

        if args.revert:
            profile["UnlockedFullGame"] = bool(profile.pop(BENCH_SAVED, False))
            profile["UnlockedProducts"] = [p for p in products
                                           if p != BENCH_PRODUCT]
            action = "revertida"
        else:
            if BENCH_SAVED not in profile:
                profile[BENCH_SAVED] = bool(profile.get("UnlockedFullGame"))
            profile["UnlockedFullGame"] = True
            if BENCH_PRODUCT not in products:
                products.append(BENCH_PRODUCT)
            profile["UnlockedProducts"] = products
            action = "aplicada"

        store_profile(entries, profile)
        os.makedirs(userdata, exist_ok=True)
        write_prefs(dest, entries)
    except SaveError as exc:
        print(f"erro: {exc}", file=sys.stderr)
        return 1
    except OSError as exc:
        print(f"erro de E/S: {exc}", file=sys.stderr)
        return 1

    origem = "perfil existente" if existed else "perfil minimo novo"
    print(f"[bench] marca {action} sobre {origem} em {dest}")
    print(f"[bench] jogo completo: {profile['UnlockedFullGame']}")
    print(f"[bench] produtos: {profile['UnlockedProducts'] or '(nenhum)'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
