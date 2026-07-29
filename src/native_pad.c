/* ===== NATPAD: controle Xbox NATIVO do Terraria via InControl device-attach =====
 *
 * Filosofia (rewrite 2026-07-01): NADA de simular mouse/teclado/navegação por regiões.
 * O Terraria mobile já tem o suporte completo de controle Xbox (InControl + perfis
 * Android + ControllerDeviceManager + glifos). Ele só não funciona no so-loader porque
 * Input.GetJoystickNames() do Unity retorna vazio => InControl nunca ATTACHA um device
 * => AnyControllerConnected/HasController = false => toda a UI ignora o controle.
 *
 * Fazemos o jogo VER um controle de verdade, no TOPO da cadeia:
 *   1. Input.GetJoystickNames() -> ["<TER_NPNAME>"] (default "Microsoft X-Box 360 pad")
 *      O UnityInputDeviceManager do InControl detecta (1x/s), casa o profile e attacha.
 *   2. UnityInputDevice.ReadRawButtonState(i)/ReadRawAnalogValue(i) -> estado do
 *      SDL_GameController (layout Xbox normalizado pelo SDL).
 *   3. UnityInputDevice.get_IsSupportedOnThisPlatform -> true (mata regex de plataforma).
 *   4. Fallback TER_NPATTACH=1: se o UnityInputDeviceManager não existir no build,
 *      criamos um UnityInputDevice via il2cpp e chamamos InputManager.AttachDevice.
 *
 * Daí em diante menu/options/bolsa/gameplay/glifos são 100% código do próprio jogo.
 *
 * Diagnóstico:
 *   TER_NATLOG=1  loga attach, chamadas, índices raw consultados (o profile só consulta
 *                 o que mapeou => o histograma REVELA o mapeamento do profile) e o estado
 *                 do ControllerActionManager (HasController/_controllerActive).
 *   TER_NATCAL=1  autocalibração: força UM índice raw por vez (90 frames cada) e loga
 *                 quais _KeyState/AxisValue do ControllerDevice do Terraria acendem.
 *   /tmp/tergp    input virtual (tokens up/down/left/right/a/b/x/y/start/select/l1/r1/
 *                 lt/rt/l3/r3) p/ dirigir o jogo por ssh sem pad físico.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <SDL2/SDL.h>

extern uintptr_t ter_il2cpp_base(void);
static uintptr_t g_il2cpp_base;   /* espelho local: main.c mantém o dele static */
/* helpers do main.c (agora não-static) */
extern unsigned long ter_method_off(const char *ns, const char *cn, const char *mn, int argc);
extern int ter_install_hook4(unsigned long off, void *fn, void **orig_out);
extern void *ter_static_obj(const char *ns, const char *cn, const char *fn);
extern int ter_vkbd_blocking(void);   /* teclado na tela aberto: jogo não deve ver input */

/* ---- il2cpp API (exports em RVA fixo, confirmados no readelf do libil2cpp.so) ---- */
#define IL2(fn, rva) (*(fn)(uintptr_t)(g_il2cpp_base + (rva)))
typedef void *(*f_dom_get)(void);
typedef const void **(*f_dom_asms)(void *, size_t *);
typedef void *(*f_asm_img)(const void *);
typedef void *(*f_cls_from_name)(void *, const char *, const char *);
typedef void *(*f_cls_method)(void *, const char *, int);
typedef void *(*f_string_new)(const char *);
typedef void *(*f_array_new)(void *, size_t);
typedef unsigned (*f_gch_new)(void *, int);
typedef void *(*f_obj_new)(void *);
typedef void *(*f_rt_invoke)(void *, void *, void **, void **);
#define NP_dom_get       ((f_dom_get)(g_il2cpp_base + 0x73c860))
#define NP_dom_asms      ((f_dom_asms)(g_il2cpp_base + 0x73c86c))
#define NP_asm_img       ((f_asm_img)(g_il2cpp_base + 0x73c22c))
#define NP_cls_from_name ((f_cls_from_name)(g_il2cpp_base + 0x73c264))
#define NP_cls_method    ((f_cls_method)(g_il2cpp_base + 0x73c28c))
#define NP_string_new    ((f_string_new)(g_il2cpp_base + 0x73cc98))
#define NP_array_new     ((f_array_new)(g_il2cpp_base + 0x73c214))
#define NP_gch_new       ((f_gch_new)(g_il2cpp_base + 0x73cac8))
#define NP_obj_new       ((f_obj_new)(g_il2cpp_base + 0x73cc34))
#define NP_rt_invoke     ((f_rt_invoke)(g_il2cpp_base + 0x73cc7c))

static int np_log;

/* ================= estado lógico do pad (layout Xbox, via SDL) ================= */
/* botões: 0=A 1=B 2=X 3=Y 4=LB 5=RB 6=BACK 7=START 8=L3 9=R3 10=DU 11=DD 12=DL 13=DR */
enum { NPB_A, NPB_B, NPB_X, NPB_Y, NPB_LB, NPB_RB, NPB_BACK, NPB_START,
       NPB_L3, NPB_R3, NPB_DU, NPB_DD, NPB_DL, NPB_DR, NPB_COUNT };
/* eixos: 0=LX 1=LY 2=RX 3=RY 4=LT 5=RT (SDL: -1..1, Y down=+1; LT/RT 0..1) */
enum { NPA_LX, NPA_LY, NPA_RX, NPA_RY, NPA_LT, NPA_RT, NPA_COUNT };
static unsigned char g_npb[NPB_COUNT];
static float g_npa[NPA_COUNT];

static SDL_GameController *g_np_gc;   /* pad SDL aberto (lido direto pelo exit-hotkey) */
static void np_poll_sdl(void) {
  SDL_GameController *gc = g_np_gc; static int tried;
  if (!tried) {
    tried = 1;
    SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS);
    /* Twin USB PS2 Adapter (vendor 0810): o mapa embutido do SDL erra o eixo Y do stick
       direito; a entrada da comunidade usa rightx:a3/righty:a2. Override por GUID. */
    const char *um = getenv("TER_GP_MAP");
    SDL_GameControllerAddMapping(um && *um ? um :
      "0300605b100800000100000010010000,USB Gamepad,platform:Linux,"
      "a:b1,b:b2,x:b0,y:b3,leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,righttrigger:b7,"
      "back:b8,start:b9,leftstick:b10,rightstick:b11,leftx:a0,lefty:a1,rightx:a3,righty:a2,"
      "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,");
    int n = SDL_NumJoysticks();
    for (int i = 0; i < n; i++) {
      if (!SDL_IsGameController(i)) continue;
      gc = SDL_GameControllerOpen(i);
      g_np_gc = gc;
      if (gc) { fprintf(stderr, "[NATPAD] SDL pad js%d: %s\n", i,
                        SDL_GameControllerName(gc) ? SDL_GameControllerName(gc) : "?"); fsync(2); break; }
    }
    if (!gc) { fprintf(stderr, "[NATPAD] nenhum SDL GameController (NumJoysticks=%d)\n", n); fsync(2); }
  }
  if (!gc) return;
  SDL_GameControllerUpdate();
  int swap = getenv("TER_SWAPAB") ? 1 : 0;
  g_npb[swap?NPB_B:NPB_A] = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A);
  g_npb[swap?NPB_A:NPB_B] = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_B);
  g_npb[NPB_X]    = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_X);
  g_npb[NPB_Y]    = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_Y);
  g_npb[NPB_LB]   = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
  g_npb[NPB_RB]   = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
  g_npb[NPB_BACK] = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK);
  g_npb[NPB_START]= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START);
  g_npb[NPB_L3]   = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSTICK);
  g_npb[NPB_R3]   = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSTICK);
  g_npb[NPB_DU]   = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP);
  g_npb[NPB_DD]   = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
  g_npb[NPB_DL]   = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
  g_npb[NPB_DR]   = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
  g_npa[NPA_LX] = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
  g_npa[NPA_LY] = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f;
  g_npa[NPA_RX] = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.0f;
  g_npa[NPA_RY] = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0f;
  short lt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
  short rt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
  g_npa[NPA_LT] = lt > 0 ? lt / 32767.0f : 0.0f;
  g_npa[NPA_RT] = rt > 0 ? rt / 32767.0f : 0.0f;
}

/* input virtual p/ teste autônomo por ssh: 1 token em /tmp/tergp -> pulso de N frames */
static void np_poll_virtual(void) {
  static int vb[NPB_COUNT]; static int va[NPA_COUNT];
  if (!getenv("TER_GPVIRT")) return;
  FILE *f = fopen("/tmp/tergp", "r");
  if (f) {
    char tok[24] = {0}; int got = (fscanf(f, "%23s", tok) == 1 && tok[0]); fclose(f);
    if (got) {
      f = fopen("/tmp/tergp", "w"); if (f) fclose(f);
      int dur = getenv("TER_GPVDUR") ? atoi(getenv("TER_GPVDUR")) : 4;
      static const char *bn[NPB_COUNT] = {"a","b","x","y","l1","r1","select","start",
                                          "l3","r3","up","down","left","right"};
      static const char *an[NPA_COUNT] = {"lx+","ly+","rx+","ry+","lt","rt"};
      int hit = 0;
      for (int i = 0; i < NPB_COUNT; i++) if (!strcasecmp(tok, bn[i])) { vb[i] = dur; hit = 1; }
      for (int i = 0; i < NPA_COUNT; i++) if (!strcasecmp(tok, an[i])) { va[i] = dur; hit = 1; }
      fprintf(stderr, "[NATPAD] virt \"%s\" hit=%d x%d\n", tok, hit, dur); fsync(2);
    }
  }
  for (int i = 0; i < NPB_COUNT; i++) if (vb[i] > 0) { vb[i]--; g_npb[i] = 1; }
  for (int i = 0; i < NPA_COUNT; i++) if (va[i] > 0) { va[i]--; g_npa[i] = 1.0f; }
}

/* ============ mapa raw-index (Unity Android) -> estado lógico ============
 * Convenção Unity/Android (padrão dos perfis *AndroidUnityProfile do InControl):
 *   buttons: 0=A 1=B 2=X 3=Y 4=LB 5=RB 8=L3 9=R3 10=Start 11=Back/Select
 *   analogs: 0=LX 1=LY 2=RX 3=RY 4=dpadX(hat) 5=dpadY(hat) 6=LT 7=RT
 * Se o profile do jogo consultar diferente, TER_NATLOG mostra os índices consultados e
 * TER_NATCAL deriva o mapa; override por TER_NPB<i>=<NPB_*> / TER_NPA<i>=<NPA_*|-dpx|-dpy>.
 */
static signed char np_rawbtn[20] = { NPB_A, NPB_B, NPB_X, NPB_Y, NPB_LB, NPB_RB, -1, -1,
                                     NPB_L3, NPB_R3, NPB_START, NPB_BACK, -1,-1,-1,-1,-1,-1,-1,-1 };
/* eixos: código especial 100=dpadX 101=dpadY (hat digital do SDL como eixo) */
static signed char np_rawax[20]  = { NPA_LX, NPA_LY, NPA_RX, NPA_RY, 100, 101, NPA_LT, NPA_RT,
                                     -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };
static int np_invy = 0;   /* TER_NPINVY: inverte sinal dos eixos Y (LY/RY) se o profile esperar up=+1 */

static void np_maps_env(void) {
  char nm[16];
  for (int i = 0; i < 20; i++) {
    snprintf(nm, sizeof nm, "TER_NPB%d", i);
    if (getenv(nm)) np_rawbtn[i] = atoi(getenv(nm));
    snprintf(nm, sizeof nm, "TER_NPA%d", i);
    if (getenv(nm)) np_rawax[i] = atoi(getenv(nm));
  }
  np_invy = getenv("TER_NPINVY") ? atoi(getenv("TER_NPINVY")) : 0;
}

/* ---- calibração: força 1 índice raw por vez; ler [NATCAL] + [NATDIAG] no log ---- */
static int np_cal_kind, np_cal_idx = -1;   /* kind 0=button 1=analog */
static void np_cal_step(void) {
  if (!getenv("TER_NATCAL")) { np_cal_idx = -1; return; }
  static int fc; int f = fc++;
  int per = 90, total = 20 * 2;
  int slot = (f / per) % total;
  np_cal_kind = slot >= 20; np_cal_idx = slot % 20;
  if (f % per == 0) { fprintf(stderr, "[NATCAL] forcando raw %s %d\n",
                              np_cal_kind ? "analog" : "button", np_cal_idx); fsync(2); }
}

/* SELECT+START segurados ~0.75s = fecha o jogo (padrão dos ports NextOS).
   TER_NPEXIT=0 desliga; TER_NPEXITF muda o nº de frames. _exit direto: o processo
   morre sem teardown GL (mesmo efeito do kill -9 que o device já tolera). */
/* Hotkey universal de SAIR (SELECT+START) — COPIADO do Bully/Sonic: le o
 * SDL_GameController DIRETO (nao o estado cacheado) e mata NA HORA. Chamado de
 * todo frame (np_frame) E de dentro do ReadRawButtonState (o jogo chama muitas
 * vezes por frame) -> garante deteccao mesmo se o swap-hook falhar num frame.
 * _exit/SIGKILL imediato evita o deadlock do blob Mali ao liberar o contexto GL. */
void np_check_exit_hotkey(void) {
  if (getenv("TER_NPEXIT") && !atoi(getenv("TER_NPEXIT"))) return;
  if (g_np_gc &&
      SDL_GameControllerGetButton(g_np_gc, SDL_CONTROLLER_BUTTON_BACK) &&
      SDL_GameControllerGetButton(g_np_gc, SDL_CONTROLLER_BUTTON_START)) {
    fprintf(stderr, "[NATPAD] SELECT+START -> saindo do jogo\n"); fsync(2);
    kill(getpid(), SIGKILL);
    _exit(0);
  }
}

/* estado p/ consumidores externos (vkbd do main.c): botão segurado + edge deste frame */
static unsigned char g_npb_prev[NPB_COUNT];
int np_btn(int b)      { return (b >= 0 && b < NPB_COUNT) ? g_npb[b] : 0; }
int np_btn_down(int b) { return (b >= 0 && b < NPB_COUNT) ? (g_npb[b] && !g_npb_prev[b]) : 0; }

/* histograma de consultas (revela o mapeamento do profile) */
static unsigned np_qbtn[20], np_qax[20];
static unsigned long np_jn_calls, np_rb_calls, np_ra_calls;

/* ================= corpos substituídos (chamados pelo InControl) ================= */
static int np_ReadRawButtonState(void *self, int index, void *mi) {
  (void)self; (void)mi; np_rb_calls++;
  np_check_exit_hotkey();
  if (index >= 0 && index < 20) np_qbtn[index]++;
  if (ter_vkbd_blocking()) return 0;   /* digitando no teclado: jogo não vê botões */
  if (np_cal_idx >= 0) return (!np_cal_kind && index == np_cal_idx) ? 1 : 0;
  if (index < 0 || index >= 20 || np_rawbtn[index] < 0) return 0;
  return g_npb[(int)np_rawbtn[index]] ? 1 : 0;
}
static float np_ReadRawAnalogValue(void *self, int index, void *mi) {
  (void)self; (void)mi; np_ra_calls++;
  if (index >= 0 && index < 20) np_qax[index]++;
  if (ter_vkbd_blocking()) return 0.0f;
  if (np_cal_idx >= 0) return (np_cal_kind && index == np_cal_idx) ? 1.0f : 0.0f;
  if (index < 0 || index >= 20) return 0.0f;
  int m = np_rawax[index];
  if (m == 100) return (float)(g_npb[NPB_DR] - g_npb[NPB_DL]);          /* dpadX: right=+1 */
  if (m == 101) return (float)(g_npb[NPB_DD] - g_npb[NPB_DU]);          /* dpadY: down=+1 (Android HAT_Y) */
  if (m < 0 || m >= NPA_COUNT) return 0.0f;
  float v = g_npa[m];
  if (np_invy && (m == NPA_LY || m == NPA_RY)) v = -v;
  return v;
}
static int np_IsSupported(void *self, void *mi) { (void)self; (void)mi; return 1; }

/* Input.GetJoystickNames() -> string[1] fixo (cacheado + pinado contra o GC) */
static void *g_np_names_arr;
static void *np_string_class(void) {
  void *dom = NP_dom_get(); if (!dom) return NULL;
  size_t na = 0; const void **as = NP_dom_asms(dom, &na); if (!as) return NULL;
  for (size_t i = 0; i < na; i++) {
    void *img = NP_asm_img(as[i]); if (!img) continue;
    void *c = NP_cls_from_name(img, "System", "String"); if (c) return c;
  }
  return NULL;
}
static void *np_GetJoystickNames(void *mi) {
  (void)mi; np_jn_calls++;
  if (!g_np_names_arr) {
    void *sc = np_string_class(); if (!sc) return NULL;
    const char *nm = getenv("TER_NPNAME") ? getenv("TER_NPNAME") : "Microsoft X-Box 360 pad";
    void *arr = NP_array_new(sc, 1); if (!arr) return NULL;
    void *s = NP_string_new(nm); if (!s) return NULL;
    ((void **)((char *)arr + 0x20))[0] = s;   /* data começa em +0x20 (bounds NULL, len em +0x18) */
    NP_gch_new(arr, 1);                       /* pina (nunca coletar/mover) */
    g_np_names_arr = arr;
    fprintf(stderr, "[NATPAD] GetJoystickNames -> [\"%s\"]\n", nm); fsync(2);
  }
  return g_np_names_arr;
}

/* ================= instalação (lazy, do swap-hook) ================= */
static int np_body_replace(unsigned long off, void *fn) {
  if (!off) return 0;
  long pgsz = sysconf(_SC_PAGESIZE);
  uint32_t *c = (uint32_t *)(g_il2cpp_base + off);
  void *pa = (void *)((uintptr_t)c & ~((uintptr_t)pgsz - 1));
  mprotect(pa, pgsz * 2, PROT_READ | PROT_WRITE | PROT_EXEC);
  c[0] = 0x58000050u;                 /* ldr x16,[pc+8] */
  c[1] = 0xD61F0200u;                 /* br x16 */
  *(uint64_t *)(c + 2) = (uint64_t)(uintptr_t)fn;
  mprotect(pa, pgsz * 2, PROT_READ | PROT_EXEC);
  __builtin___clear_cache((char *)pa, (char *)pa + 16);
  return 1;
}
static unsigned long np_off_or(const char *ns, const char *cn, const char *mn, int argc,
                               unsigned long fallback) {
  unsigned long o = ter_method_off(ns, cn, mn, argc);
  if (!o) o = fallback;
  return o;
}
static int np_installed;
static void np_install(void) {
  if (np_installed || !g_il2cpp_base) return;
  static int tries; if (tries++ > 900) { np_installed = 1; return; }
  /* resolve por NOME (robusto a versão); fallback = RVAs do dump 1.4.4.9.x */
  unsigned long jn = np_off_or("UnityEngine", "Input", "GetJoystickNames", 0, 0x26AE7C4);
  unsigned long rb = np_off_or("InControl", "UnityInputDevice", "ReadRawButtonState", 1, 0x2052908);
  unsigned long ra = np_off_or("InControl", "UnityInputDevice", "ReadRawAnalogValue", 1, 0x20529A0);
  unsigned long sp = np_off_or("InControl", "UnityInputDevice", "get_IsSupportedOnThisPlatform", 0, 0x2052A38);
  if (!jn || !rb || !ra) return;   /* runtime ainda não subiu as classes; tenta no próximo frame */
  np_maps_env();
  np_body_replace(jn, (void *)np_GetJoystickNames);
  np_body_replace(rb, (void *)np_ReadRawButtonState);
  np_body_replace(ra, (void *)np_ReadRawAnalogValue);
  if (sp) np_body_replace(sp, (void *)np_IsSupported);
  np_installed = 1;
  fprintf(stderr, "[NATPAD] instalado: GetJoystickNames@0x%lx ReadRawButton@0x%lx "
                  "ReadRawAnalog@0x%lx IsSupported@0x%lx\n", jn, rb, ra, sp); fsync(2);
}

/* ---- fallback TER_NPATTACH: attach manual (UnityInputDevice + InputManager.AttachDevice) ---- */
static void *np_find_class(const char *ns, const char *cn) {
  void *dom = NP_dom_get(); if (!dom) return NULL;
  size_t na = 0; const void **as = NP_dom_asms(dom, &na); if (!as) return NULL;
  for (size_t i = 0; i < na; i++) {
    void *img = NP_asm_img(as[i]); if (!img) continue;
    void *c = NP_cls_from_name(img, ns, cn); if (c) return c;
  }
  return NULL;
}
static void np_manual_attach(void) {
  static int done; if (done) return;
  if (!getenv("TER_NPATTACH")) { done = 1; return; }
  static int frames; if (frames++ < 300) return;   /* espera o InputManager.Setup do jogo */
  void *cls_uid = np_find_class("InControl", "UnityInputDevice");
  void *cls_im  = np_find_class("InControl", "InputManager");
  if (!cls_uid || !cls_im) return;
  void *m_isset = NP_cls_method(cls_im, "get_IsSetup", 0);
  if (m_isset) {
    void *exc = NULL; void *r = NP_rt_invoke(m_isset, NULL, NULL, &exc);
    int ok = (exc == NULL && r && *(unsigned char *)((char *)r + 0x10));
    if (!ok) { if (np_log && frames % 300 == 0) { fprintf(stderr, "[NATPAD] InputManager ainda nao IsSetup\n"); fsync(2); } return; }
  }
  void *m_ctor = NP_cls_method(cls_uid, ".ctor", 2);
  void *m_att  = NP_cls_method(cls_im, "AttachDevice", 1);
  if (!m_ctor || !m_att) { done = 1; fprintf(stderr, "[NATPAD] NPATTACH: ctor/AttachDevice nao achados\n"); fsync(2); return; }
  void *dev = NP_obj_new(cls_uid); if (!dev) return;
  const char *nm = getenv("TER_NPNAME") ? getenv("TER_NPNAME") : "Microsoft X-Box 360 pad";
  int jid = 1; void *s = NP_string_new(nm);
  void *args1[2] = { &jid, s }; void *exc = NULL;
  NP_rt_invoke(m_ctor, dev, args1, &exc);
  if (exc) { done = 1; fprintf(stderr, "[NATPAD] NPATTACH: ctor lançou exceção %p\n", exc); fsync(2); return; }
  void *args2[1] = { dev }; exc = NULL;
  NP_rt_invoke(m_att, NULL, args2, &exc);
  NP_gch_new(dev, 0);
  done = 1;
  fprintf(stderr, "[NATPAD] NPATTACH: UnityInputDevice(\"%s\") attachado exc=%p\n", nm, exc); fsync(2);
}

/* ================= diagnóstico ================= */
static void np_diag(void) {
  if (!np_log) return;
  static int fc; if ((fc++ % 120) != 0) return;
  /* estado do ControllerActionManager do Terraria */
  void *cam = ter_static_obj("Controller", "ControllerActionManager", "Instance");
  int act = -1; void *ctrl = NULL;
  if (cam) { act = *(unsigned char *)((char *)cam + 0x30); ctrl = *(void **)((char *)cam + 0x28); }
  fprintf(stderr, "[NATDIAG] jn=%lu rb=%lu ra=%lu | CAM=%p _controllerActive=%d _controller=%p\n",
          np_jn_calls, np_rb_calls, np_ra_calls, cam, act, ctrl);
  if (ctrl) {   /* _KeyState bool[] @0x60 / AxisValue float[] @0x28 (dados em +0x20) */
    unsigned char *ks = NULL; float *av = NULL;
    void *ksa = *(void **)((char *)ctrl + 0x60), *ava = *(void **)((char *)ctrl + 0x28);
    if (ksa) ks = (unsigned char *)ksa + 0x20;
    if (ava) av = (float *)((char *)ava + 0x20);
    if (ks && av) {
      char line[256]; int p = 0;
      p += snprintf(line + p, sizeof line - p, "[NATDIAG] KeyState:");
      for (int i = 0; i < 13; i++) p += snprintf(line + p, sizeof line - p, "%d", ks[i] ? 1 : 0);
      p += snprintf(line + p, sizeof line - p, " Axis:");
      for (int i = 0; i < 8; i++) p += snprintf(line + p, sizeof line - p, " %.1f", av[i]);
      fprintf(stderr, "%s\n", line);
    }
  }
  if (np_rb_calls + np_ra_calls > 0) {   /* histograma = mapeamento consultado pelo profile */
    char l1[160], l2[160]; int p1 = 0, p2 = 0;
    p1 += snprintf(l1 + p1, sizeof l1 - p1, "[NATDIAG] qbtn:");
    p2 += snprintf(l2 + p2, sizeof l2 - p2, "[NATDIAG] qax :");
    for (int i = 0; i < 16; i++) {
      p1 += snprintf(l1 + p1, sizeof l1 - p1, " %d:%u", i, np_qbtn[i]);
      p2 += snprintf(l2 + p2, sizeof l2 - p2, " %d:%u", i, np_qax[i]);
    }
    fprintf(stderr, "%s\n%s\n", l1, l2);
  }
  fsync(2);
}

/* ================= entrada por frame (chamada do swap-hook do main.c) ================= */
void np_frame(void) {
  if (!getenv("TER_NATPAD")) return;
  g_il2cpp_base = ter_il2cpp_base();
  if (!g_il2cpp_base) return;
  np_log = getenv("TER_NATLOG") ? 1 : 0;
  np_install();
  if (!np_installed) return;
  memcpy(g_npb_prev, g_npb, sizeof g_npb_prev);
  np_poll_sdl();
  np_poll_virtual();
  np_check_exit_hotkey();
  np_cal_step();
  np_manual_attach();
  np_diag();
}
