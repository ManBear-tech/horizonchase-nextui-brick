# Horizon Chase — migração do Unity Mali-450/fbdev para R36S/Mali-G31/KMSDRM

Documento técnico da adaptação do primeiro port Unity deste projeto que saiu
do alvo NextOS Mali-450 para o R36S/ArkOS. O jogo é o Horizon Chase 2.6.9,
Unity 2022.3.33f1, IL2CPP arm64.

Este relatório registra tanto as correções aprovadas quanto as experiências
rejeitadas. Ele não substitui o fluxo Android do jogo: toda a adaptação
intercepta o player nativo na mesma ordem em que a Activity original o
executaria.

## 1. Base que já funcionava no Mali-450

O port original validado no NextOS usa:

- Mali-450 Utgard, GLES2 e EGL/fbdev;
- `libunity.so` e `libil2cpp.so` carregadas por um so-loader aarch64;
- `init_array` completo e `JNI_OnLoad` das duas bibliotecas;
- ciclo de Activity/Surface, HandlerThread e Choreographer reproduzido;
- AssetManager e o split `UnityDataAssetPack.apk` expostos pelo JNI;
- FMOD/AudioTrack/OpenSL encaminhado para SDL;
- controle físico injetado na fronteira nativa `GamepadInputSource` do jogo;
- SharedPreferences tipado e atômico para persistência;
- saída `Select + Start` com focus-loss, pause, flush do save e retorno ao
  frontend.

O Mali-450 não oferece ASTC nem ETC2. Nesse alvo, o port preserva ETC1 nativo,
inclusive a combinação ETC1 RGB + Alpha8 usada em dupla camada pelo Horizon,
e decodifica por software somente os uploads comprimidos que o GLES2 não
aceita. O compositor Amlogic também exige alpha 1 no backbuffer final; por isso
o caminho fbdev faz um clear somente do canal alpha antes do swap.

Nada desse fluxo foi pulado para chegar ao R36S.

## 2. Diferenças reais do R36S testado

O aparelho usado na validação expõe:

| Componente | R36S/ArkOS |
|---|---|
| Tela | DRM/KMS, DSI, 640×480 a 60 Hz |
| GPU | Mali-G31 |
| API | OpenGL ES 3.2, driver r13p0 |
| Vídeo SDL | backend KMSDRM |
| RAM física exposta | cerca de 640 MiB |
| Swap usado durante os testes | 512 MiB |

Ao contrário do Mali-450, a Mali-G31 aceita ASTC, ETC1 e ETC2 diretamente.
Portanto o perfil R36S mantém esses formatos comprimidos na GPU e aplica o
teto de 512 somente a texturas cruas ou decodificadas que realmente excedam a
resolução útil do painel.

## 3. Sintoma inicial

O Unity completava o boot, montava os asset bundles, tocava música e efeitos e
continuava executando o ciclo de cenas, mas o painel não mostrava imagem.

Esse sintoma continha três falhas de apresentação independentes. Corrigir só
uma delas fazia o log avançar, mas ainda deixava a tela preta.

## 4. Falha 1 — Swappy sem SurfaceFlinger

O Unity 2022.3.33f1 inclui o Android Game SDK Swappy. No Android normal ele usa
as extensões de timestamp de frame, incluindo funções da família
`EGL_ANDROID_get_frame_timestamps`.

O EGL/GBM do ArkOS não fornece essa interface Android. A construção interna do
backend Swappy retornava nula, mas a opção `androidUseSwappy` continuava
ligada. O wrapper de apresentação então não chamava nem o Swappy nem o
`eglSwapBuffers` comum.

Tentativas por propriedade Android e `swappy.disable` no `boot.config` não
resolveram, porque o carregador falhava antes de consultar a propriedade.

### Correção aprovada

Depois de `nativeRecreateGfxState`, `surfaceChanged`, resume e focus, o host
marca o estado interno de “Swappy forçadamente desativado” dessa versão exata
do Unity. Assim o próprio wrapper do engine seleciona seu fallback EGL
normal.

Os offsets são específicos de `libunity.so` 2022.3.33f1 e não devem ser
reutilizados em outro Unity sem nova análise:

- predicado `androidUseSwappy`: `libunity + 0x9b6da4`;
- seletor de swap: `libunity + 0x9b6fa0`;
- estado force-disabled: `libunity + 0x1850db4`.

## 5. Falha 2 — identidade incorreta do EGLSurface

O shim criava o surface Android falso com `strdup("window")`, mas
`eglGetCurrentSurface` devolvia o endereço de outro literal `"window"`.
O conteúdo das strings era igual; os ponteiros, não.

O Unity compara a identidade do handle antes de apresentar. Como os endereços
eram diferentes, tratava o surface como não-current e pulava
`eglSwapBuffers` em todos os frames.

### Correção aprovada

Cada `_egl_context` passou a guardar os handles `draw` e `read` exatos
recebidos em `eglMakeCurrent`. `eglGetCurrentSurface(EGL_DRAW/EGL_READ)`
devolve esses mesmos handles, preservando a semântica EGL e a comparação de
identidade feita pelo Unity.

Depois dessa correção, o seletor de apresentação e
`SDL_GL_SwapWindow` passaram a ser chamados em todos os frames.

## 6. Falha 3 — contexto EGL cru e buffer KMS vazio

Mesmo com swaps reais, o painel ainda ficava preto. Uma captura feita na
thread de apresentação encontrou:

- 640×480 pixels;
- zero pixels RGB visíveis;
- backbuffer completamente preto.

Isso provou que não era textura ausente nem defeito do painel: o KMS estava
apresentando uma window que não recebeu os draws.

A comparação foi feita somente com ports comprovadamente funcionais no
R36S:

- Bully cria a window/contexto pelo SDL e usa `SDL_GL_SwapWindow` no KMSDRM;
- Sonic 4 mantém `SDL_GL_MakeCurrent` e o present sob propriedade do SDL.

No Horizon, duas threads do Unity alternavam o mesmo contexto usando
`eglMakeCurrent` cru, enquanto o SDL era chamado apenas no swap. Nesse estado
`SDL_GL_GetCurrentContext` era nulo durante a apresentação. O contexto e a
surface usados pelos draws não pertenciam ao mesmo caminho que fazia o
page-flip.

### Correção aprovada

O `egl_shim` agora resolve ownership em runtime:

- backend SDL `mali` (case-insensitive) mantém o EGL cru exigido pelo blob
  fbdev do Mali-450;
- KMSDRM, Wayland e backends equivalentes mantêm o contexto integralmente sob
  ownership do SDL;
- `HC_PURE_SDL_CONTEXTS=0|1` e `HC_RAW_EGL_CONTEXTS=1` ficam somente como
  escapes explícitos de engenharia.

No caminho KMS automático:

- bind e unbind usam `SDL_GL_MakeCurrent`;
- o SDL continua dono do contexto criado para sua window;
- o mesmo backend KMSDRM executa a renderização e o page-flip;
- nenhum nome de device ou driver de vídeo precisa ser fixado no launcher.

A primeira captura após a mudança mostrou o logo Aquiris. Uma captura
posterior mostrou o título completo, pista, cidade e cenário. A imagem e os
carros também foram confirmados no painel físico.

## 7. Experiência rejeitada — descarte manual de VBOs

Foi testado um coletor de buffers dinâmicos que tentava identificar VBOs
transientes do Unity e apagá-los depois de uma fence da GPU. A intenção era
conter RAM/VRAM em sessões longas.

O título e o cenário continuaram visíveis, mas o carro desapareceu. Isso
provou que o Unity reutiliza pelo menos parte dos nomes classificados como
transientes. A fence confirmava que a GPU havia terminado o uso daquele
frame, mas não transferia ao host a propriedade do objeto.

Resultado: a otimização foi rejeitada e não é habilitada pelo runtime nem pelo
launcher R36S. A correção correta preserva a propriedade dos buffers pelo
engine. O teste sem descarte restaurou imediatamente os carros.

## 8. RAM e cadência do lifecycle

O Android chama `nativeRender` pela cadência do Choreographer. Um loop host
sem pacing conseguia submeter centenas de ciclos por segundo, desperdiçando
CPU/GPU e aumentando a pressão de buffers no driver.

O launcher comum define `HC_FRAME_LIMIT=30` como padrão em todos os backends.
O valor continua sobrescrevível para engenharia.

O host usa `clock_nanosleep` com deadline absoluto para reproduzir a cadência
de 30 FPS sem acumular drift. Essa limitação atua no lifecycle; não pula
nenhuma etapa do jogo.

Também foram aplicadas estas reduções seguras:

- `MALLOC_ARENA_MAX=2`, evitando uma arena glibc retida para cada worker curto;
- liberação das imagens ELF temporárias completas de `libunity.so` e
  `libil2cpp.so` depois de relocação, hooks e resolução;
- ASTC/ETC1/ETC2 nativos na Mali-G31, sem expansão RGBA desnecessária;
- teto automático de 512 para texturas cruas grandes no perfil KMS de pouca
  RAM;
- logs e capturas pesadas compilados fora do build de release.

A swap de 512 MiB foi autorizada e usada para dar margem aos testes, mas não
substitui as políticas acima. O objetivo é não depender de thrashing para
manter a corrida jogável.

## 9. Build universal compatível com ArkOS e NextOS

`build_universal.sh` chama a receita histórica `build_r36s.sh` para gerar o
executável AArch64 público de glibc baixa. O mesmo arquivo foi executado sem
alteração no R36S/Mali-G31, NextOS/Mali-450 e NextOS/Mali-G310. O SDL é ligado
somente pelo SONAME resolvido no aparelho.

Cada build:

1. rejeita símbolos acima de `GLIBC_2.30`;
2. confere que `g_bionic_guard_pad` continua no offset TLS zero e tem 256
   bytes;
3. confere o template TLS de `0x100` bytes;
4. produz PIE aarch64 com o interpretador normal do sistema.

O build aprovado durante esta migração exige no máximo `GLIBC_2.27`.

O teto baixo de glibc é uma variante pública multi-firmware explicitamente
autorizada para esta adaptação. O build interno `build.sh` continua usando o
toolchain/sysroot atual do próprio NextOS.

## 10. Perfil final por plataforma

| Item | NextOS Mali-450 | R36S Mali-G31 | NextOS Mali-G310 |
|---|---|---|---|
| Display | EGL/fbdev | SDL2/KMSDRM | SDL2/KMSDRM |
| Contexto | EGL real do blob Mali | ownership integral do SDL | ownership integral do SDL |
| Resolução validada | 1280×720 | 640×480 | 1920×1080 |
| GLES | 2.0 | 3.x | 3.2 |
| ASTC/ETC2 | decoder de fallback | upload nativo | upload nativo |
| ETC1 + Alpha8 | preservado | preservado | preservado |
| Alpha final | clear A-only para OSD | scanout KMS XRGB | scanout KMS XRGB |
| Limite bruto padrão | 768 | 512 | sem teto em RAM alta |
| Lifecycle | fluxo Android nativo, 30 FPS | mesmo fluxo, 30 FPS | mesmo fluxo, 30 FPS |
| Binário público | universal, GLIBC_2.27 | o mesmo SHA | o mesmo SHA |

## 11. Evidências de validação

| Teste | Resultado |
|---|---|
| SDL criou KMSDRM 640×480 e contexto Mali-G31 GLES3 | aprovado |
| Unity/IL2CPP `init_array` + `JNI_OnLoad` completos | aprovado |
| Asset bundles e música carregados | aprovado |
| Swappy desligado e fallback EGL chamado | aprovado |
| Identidade exata de `EGLSurface` | aprovado |
| Captura anterior ao SDL puro | 0 pixels visíveis, tela preta |
| Captura SDL puro no splash | logo Aquiris visível |
| Captura SDL puro tardia | título/cenário completos |
| Carro com descarte manual de VBO | reprovado, carro ausente |
| Carro sem descarte manual de VBO | aprovado no painel físico |
| Corrida completa, música/efeitos e controle no R36S | aprovado |
| Save persistente e saída `Select + Start` no R36S | aprovado |
| Mesmo binário no Mali-450/fbdev | aprovado |
| APK 2.6.9 preparado transacionalmente pelo NXExtract no X5 | aprovado |
| KMSDRM/GLES3.2, HUD e gameplay no Mali-G310 | aprovado |
| Cadência durante corrida no X5 | 30,0 FPS |
| Memória durante corrida 1920×1080 no X5 | ~503 MiB PSS, swap zero |
| Build release sem diagnósticos | aprovado |
| Auditoria de glibc/TLS | GLIBC_2.27, TLS aprovado |

Gameplay, save/reload e saída pelo launcher permanecem no checklist de toda
nova revisão, mesmo depois destas validações.

## 12. Arquivos responsáveis

- `src/egl_shim.c`: ownership SDL/EGL, handles draw/read exatos e swap KMS;
- `src/main.c`: lifecycle Android, bloqueio do Swappy, pacing, GL/texturas e
  liberação das imagens ELF temporárias;
- `src/so_util.c`: liberação segura da imagem temporária de cada módulo;
- `run.sh`: autodetecção de resolução/memória, pacing, áudio e trava de
  instância;
- `package/r36s/Horizon Chase.sh`: integração PortMaster, NXExtract e migração
  content-addressed de instalações privadas antigas;
- `build_universal.sh` / `build_r36s.sh`: build público e auditorias;
- `package/universal/extractor.json`: receita exata do APK 2.6.9;
- `tools/build_unity_asset_pack.py`: asset-pack determinístico;
- `package/build-portmaster-package.sh`: pacote público BYO-data reproduzível.

## 13. Regras para mudanças futuras

1. Não copiar offsets para outro `libunity.so`.
2. Não reativar o descarte manual de VBOs; ele remove carros.
3. Não forçar driver SDL nem caminho raw EGL em KMS; manter a autodetecção.
4. Não achatar ETC1 + Alpha8; são duas camadas intencionais.
5. Não aplicar clamp global de wrap/sampler.
6. Não usar log volumoso no aparelho de pouca RAM.
7. Antes de lançar, confirmar que não existe outra instância, inclusive
   executável marcado como `(deleted)`.
8. Validar sempre splash, título, carro, corrida, áudio, controle, save/reload
   e `Select + Start`.
