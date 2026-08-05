HORIZON CHASE 2.6.9 — DADOS DO USUARIO / USER-SUPPLIED DATA

PT-BR
Coloque aqui uma copia legal da versao Android arm64 2.6.9:

- APK completo;
- todos os APKs split da mesma instalacao;
- bundle APKS, APKM ou XAPK completo.

O nome do arquivo nao importa. Na primeira abertura, o NXExtract identifica o
conteudo, valida a versao e prepara os dados. O arquivo original nao e apagado.
Versoes trial, incompletas, corrompidas ou diferentes sao recusadas sem alterar
uma instalacao que ja funciona.

SEU PERFIL DO ANDROID (opcional)

O Horizon Chase e free-to-play: o APK traz todas as pistas, mas a campanha
alem da demo e liberada por um direito guardado no perfil do jogador, nao
pelos arquivos. Quem concede esse direito e a compra na Google Play, com a
qual este port offline nao conversa. Por isso uma instalacao nova comeca na
demo, igual a um celular que nunca comprou.

Se voce tem o jogo completo no Android, traga o seu perfil: copie tambem para
esta pasta o arquivo

    com.aquiris.horizonchase.v2.playerprefs.xml

que fica em /data/data/com.aquiris.horizonchase/shared_prefs/ no aparelho
(precisa de root ou de um app de backup que leia dados privados).

A proxima abertura importa sozinha, avisa o que o perfil carrega e move o
arquivo para userdata/save-imports/. So o perfil entra: os ajustes do celular
sao ignorados para nao passar por cima dos do portatil. O port nao inventa
direito nenhum - libera apenas o que o seu proprio perfil ja comprova.

ENGLISH
Place a legally owned arm64 Android 2.6.9 copy here:

- one complete APK;
- every split APK from the same installation;
- one complete APKS, APKM or XAPK bundle.

The filename does not matter. On first launch NXExtract identifies the content,
validates the version and prepares the data. The original source is never
deleted. Trial, incomplete, damaged or different versions are rejected without
changing an existing working installation.

YOUR ANDROID PROFILE (optional)

Horizon Chase is free-to-play: the APK ships every track, but everything past
the demo is released by an entitlement stored in the player's profile, not by
the files on disk. That entitlement comes from the Google Play purchase, which
this offline port has no bridge to. A fresh install therefore starts on the
demo, exactly like a phone that never bought it.

If you own the full game on Android, bring your own profile: also copy into
this folder the file

    com.aquiris.horizonchase.v2.playerprefs.xml

found at /data/data/com.aquiris.horizonchase/shared_prefs/ on the device
(requires root or a backup tool that can read app-private data).

The next launch imports it on its own, reports what it carries and moves the
file to userdata/save-imports/. Only the profile is taken; the phone's own
settings are ignored so they cannot override the handheld's. The port never
invents an entitlement - it releases only what your own profile already
proves.
