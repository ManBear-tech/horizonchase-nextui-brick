/* ===== Controle REAL p/ Cuphead (Mali-450) via USB Gamepad (joydev js0) =====
 *
 * O jogo lê input por Rewired.Player.GetButton/GetButtonDown/GetAxis(string actionName).
 * SEM controle reconhecido, o Rewired retorna false/0 pra tudo. Em vez de fazer o Rewired
 * reconhecer o controle (depende de templates/maps), SUBSTITUÍMOS esses métodos: lemos o
 * nome da ação (il2cpp String) e respondemos com o estado do /dev/input/js0.
 *
 * Gated por CUP_GAMEPAD=1 (no main.c). CUP_GPLOG=1 loga eventos do js0 + nomes de ação
 * pedidos (1ª vez) p/ calibrar. Overrides de botão: CUP_GP_<ACAO>=<num> (ex CUP_GP_JUMP=0).
 */
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <linux/joystick.h>

extern void hook_arm64(uintptr_t addr, uintptr_t fn);


#define GP_NBTN  32
#define GP_NAXIS 16

static int     gp_fd = -1;
static unsigned char gp_btn[GP_NBTN];        /* estado atual (este frame) */
static unsigned char gp_btn_prev[GP_NBTN];   /* estado no frame anterior  */
static int16_t gp_axis[GP_NAXIS];
static int16_t gp_axis_rest[GP_NAXIS];       /* valor de REPOUSO (calibração) */
static int     gp_calib = 0, gp_pollcnt = 0; /* eixos viciados (ex: 0 travado em 32767) */
static int     gp_log = 0;
/* ===== INPUT VIRTUAL (CUP_GPVIRT): lê /tmp/gpcmd p/ dirigir o jogo SEM o pad físico.
   Escreve um token no arquivo (up/down/left/right/accept/cancel/jump/shoot/dash/super/
   switch/pause/lock) -> pulso virtual por ~6 frames. Permite testar/calibrar sozinho. */
static int gp_virt = 0, gv_frames = 0, gv_btn = -1, gv_h = 0, gv_v = 0, gv_force = 0;
static int gp_map_lookup(const char *cmd);  /* fwd */
static int gv_btn_held = -1;   /* botão físico atualmente forçado pelo overlay virtual */
static void gp_virt_poll(void) {
  if (gv_force > 0) gv_force--;
  if (gv_frames > 0) gv_frames--;
  else {
    /* SOLTA o botão virtual ao expirar: sem isso gp_btn[j] fica 1 p/ sempre
       (joydev nunca manda button-up de um botão virtual) -> gp_btn_prev trava
       em 1 -> nenhum edge GetButtonDown novo dispara -> menu não navega. */
    if (gv_btn_held >= 0 && gv_btn_held < GP_NBTN) gp_btn[gv_btn_held] = 0;
    gv_btn_held = -1; gv_btn = -1; gv_h = 0; gv_v = 0;
  }
  FILE *f = fopen("/tmp/gpcmd", "r");
  if (!f) return;
  char cmd[32] = {0};
  int got = (fscanf(f, "%31s", cmd) == 1 && cmd[0]);
  fclose(f);
  if (got) {
    f = fopen("/tmp/gpcmd", "w"); if (f) fclose(f);   /* consome */
    gv_btn = -1; gv_h = 0; gv_v = 0; gv_frames = 6;
    /* DIREÇÕES no menu: hold CURTO (2 frames). O menu repete o movimento enquanto
     * o eixo fica passado do limiar; com 6 frames (~0.4s) ele movia 2x por comando
     * (impossível parar em item ímpar). 2 frames = 1 passo exato. CUP_GPV_DIRF override. */
    int dirf = getenv("CUP_GPV_DIRF") ? atoi(getenv("CUP_GPV_DIRF")) : 2;
    if (!strcasecmp(cmd, "up")) { gv_v = -32767; gv_frames = dirf; }
    else if (!strcasecmp(cmd, "down")) { gv_v = 32767; gv_frames = dirf; }
    else if (!strcasecmp(cmd, "left")) { gv_h = -32767; gv_frames = dirf; }
    else if (!strcasecmp(cmd, "right")) { gv_h = 32767; gv_frames = dirf; }
    else if (!strcasecmp(cmd, "any") || !strcasecmp(cmd, "go")) gv_force = 10;  /* força TODO botão true */
    else gv_btn = gp_map_lookup(cmd);
    fprintf(stderr, "[GPV] cmd=%s -> btn=%d h=%d v=%d\n", cmd, gv_btn, gv_h, gv_v); fflush(stderr);
  }
}
/* valor do eixo RELATIVO ao repouso (corrige eixo travado/gatilho) */
static int axval(int ax) {
  if (ax < 0 || ax >= GP_NAXIS) return 0;
  int v = (int)gp_axis[ax] - (gp_calib ? (int)gp_axis_rest[ax] : 0);
  if (v > 32767) v = 32767; if (v < -32767) v = -32767;
  return v;
}

/* ---- mapa ação -> botão do gamepad (defaults p/ USB pad genérico; env override) ---- */
enum { B_JUMP, B_SHOOT, B_DASH, B_LOCK, B_SUPER, B_SWITCH, B_PAUSE,
       B_ACCEPT, B_CANCEL, B_PARRY, B_COUNT };
static int gp_map[B_COUNT] = {
  /*JUMP */ 0, /*SHOOT*/ 2, /*DASH */ 5, /*LOCK */ 4, /*SUPER*/ 1,
  /*SWITCH*/3, /*PAUSE*/ 9, /*ACCEPT*/0, /*CANCEL*/1, /*PARRY*/ 0,
};
static const char *gp_envname[B_COUNT] = {
  "CUP_GP_JUMP","CUP_GP_SHOOT","CUP_GP_DASH","CUP_GP_LOCK","CUP_GP_SUPER",
  "CUP_GP_SWITCH","CUP_GP_PAUSE","CUP_GP_ACCEPT","CUP_GP_CANCEL","CUP_GP_PARRY",
};

/* eixos: stick/dpad principal. Override: CUP_GP_AXH / CUP_GP_AXV (default 0/1). */
static int gp_axh = 0, gp_axv = 1;
static int gp_axdead = 8000;            /* zona morta */
static int gp_axthresh = 16000;         /* limiar p/ Up/Down/Left/Right digital */
static int gp_invv = 0;                 /* inverter vertical (Unity up=+1; js up=neg) */

/* ---- leitura do js0 ---- */
int gp_open(const char *path) {
  gp_fd = open(path, O_RDONLY | O_NONBLOCK);
  return gp_fd;
}

void gp_poll(void) {
  if (gp_fd < 0) return;
  struct js_event e;
  while (read(gp_fd, &e, sizeof(e)) == (int)sizeof(e)) {
    int t = e.type & ~JS_EVENT_INIT;
    if (t == JS_EVENT_BUTTON && e.number < GP_NBTN) {
      gp_btn[e.number] = e.value ? 1 : 0;
      if (gp_log) { fprintf(stderr, "[GP] btn %d = %d\n", e.number, e.value); fflush(stderr); }
    } else if (t == JS_EVENT_AXIS && e.number < GP_NAXIS) {
      gp_axis[e.number] = e.value;
      /* anti-lixo de INIT: o joydev reporta -32767 em todos os eixos ANTES do 1º
         report real do pad -> calibração envenenada -> eixo lê +32767 parado
         (menu anda sozinho). Se o repouso calibrado é extremo e o eixo reporta 0
         EXATO (centro real), re-zera o repouso. (gatilho real nunca senta em 0.) */
      if (gp_calib && e.value == 0 &&
          (gp_axis_rest[e.number] == 32767 || gp_axis_rest[e.number] == -32767)) {
        gp_axis_rest[e.number] = 0;
        if (gp_log) { fprintf(stderr, "[GP] RECAL ax%d repouso extremo -> 0\n", e.number); fflush(stderr); }
      }
      if (gp_log && (e.value > 20000 || e.value < -20000)) {
        fprintf(stderr, "[GP] axis %d = %d\n", e.number, e.value); fflush(stderr);
      }
    }
  }
  if (gp_virt) {
    gp_virt_poll();
    if (gv_btn >= 0 && gv_btn < GP_NBTN) { gp_btn[gv_btn] = 1; gv_btn_held = gv_btn; }  /* overlay botão virtual */
  }
  /* calibração: após ~40 polls (init+settle), snapshot do repouso de cada eixo */
  if (!gp_calib && ++gp_pollcnt >= 40) {
    memcpy(gp_axis_rest, gp_axis, sizeof gp_axis);
    gp_calib = 1;
    if (gp_log) {
      fprintf(stderr, "[GP] CALIBRADO repouso:");
      for (int i = 0; i < 8; i++) fprintf(stderr, " ax%d=%d", i, gp_axis_rest[i]);
      fprintf(stderr, "\n"); fflush(stderr);
    }
  }
}

/* ---- eixo-como-botão (menu): GetButtonDown("MenuVertical") = eixo cruzou limiar ---- */
static int gp_axbtn_thresh = 12000;
static int vpos_prev, vneg_prev, hpos_prev, hneg_prev;
static int logical_h(void); static int logical_v(void);  /* fwd */
static int axis_cur(int kind) { return (kind == 2) ? logical_v() : logical_h(); }

/* fim do frame: snapshot p/ edge-detect do GetButtonDown/Up + axis-as-button */
static int dirp_prev[5];               /* 1=Up 2=Down 3=Left 4=Right (frame anterior) */
static int dir_held(int d);            /* fwd */
void gp_frame_end(void) {
  memcpy(gp_btn_prev, gp_btn, sizeof(gp_btn));
  int h = logical_h(), v = logical_v();
  hpos_prev = h > gp_axbtn_thresh;  hneg_prev = h < -gp_axbtn_thresh;
  vpos_prev = v > gp_axbtn_thresh;  vneg_prev = v < -gp_axbtn_thresh;
  for (int d = 1; d <= 4; d++) dirp_prev[d] = dir_held(d);
}

/* ---- il2cpp String -> ascii (layout: +0x10 len int32, +0x14 chars utf16) ---- */
static void str_ascii(void *s, char *out, int cap) {
  out[0] = 0;
  if (!s) return;
  int32_t len = *(int32_t *)((char *)s + 0x10);
  if (len < 0 || len > 64) len = (len < 0) ? 0 : 64;
  uint16_t *c = (uint16_t *)((char *)s + 0x14);
  int n = 0;
  for (int i = 0; i < len && n < cap - 1; i++) {
    uint16_t ch = c[i];
    out[n++] = (ch < 128) ? (char)ch : '?';
  }
  out[n] = 0;
}

/* nome de ação -> índice de botão (B_*) ou -1 se não é botão simples */
static int name_btn(const char *n) {
  if (!strcasecmp(n, "Jump"))                  return B_JUMP;
  if (!strcasecmp(n, "Shoot"))                 return B_SHOOT;
  if (!strcasecmp(n, "Dash"))                  return B_DASH;
  if (!strcasecmp(n, "Lock") || !strcasecmp(n, "Aim")) return B_LOCK;
  if (!strcasecmp(n, "Super") || !strcasecmp(n, "EX")) return B_SUPER;
  if (!strcasecmp(n, "Switch") || !strcasecmp(n, "SwitchWeapon") ||
      !strcasecmp(n, "SwitchTailDirection"))   return B_SWITCH;
  if (!strcasecmp(n, "Pause"))                 return B_PAUSE;
  if (!strcasecmp(n, "Accept") || !strcasecmp(n, "Submit") || !strcasecmp(n, "UISubmit"))
                                               return B_ACCEPT;
  if (!strcasecmp(n, "Cancel") || !strcasecmp(n, "UICancel")) return B_CANCEL;
  if (!strcasecmp(n, "Parry"))                 return B_PARRY;
  return -1;
}
/* comando virtual -> índice de botão FÍSICO do js0 (via mapa de ação) */
static int gp_map_lookup(const char *cmd) {
  int b = name_btn(cmd);
  return (b >= 0) ? gp_map[b] : -1;
}
/* eixo lógico: 1=horizontal 2=vertical 0=não-eixo. Cobre Menu/UI/Move/cru. */
static int name_axis(const char *n) {
  if (!strcasecmp(n,"Horizontal")||!strcasecmp(n,"MoveHorizontal")||
      !strcasecmp(n,"MenuHorizontal")||!strcasecmp(n,"UIHorizontal")) return 1;
  if (!strcasecmp(n,"Vertical")||!strcasecmp(n,"MoveVertical")||
      !strcasecmp(n,"MenuVertical")||!strcasecmp(n,"UIVertical")) return 2;
  return 0;
}
/* eixo do menu/jogo combinando ANALÓGICO + DPAD (pega o de maior magnitude) */
static int gp_axh2 = 4, gp_axv2 = 5;   /* dpad (override CUP_GP_AXH2/AXV2) */
static int combo_axis(int prim, int sec) {
  int a = axval(prim), b = axval(sec);
  return (abs(b) > abs(a)) ? b : a;
}
/* eixo lógico horizontal/vertical: override virtual tem prioridade */
static int logical_h(void) { return gv_h ? gv_h : combo_axis(gp_axh, gp_axh2); }
static int logical_v(void) { return gv_v ? gv_v : combo_axis(gp_axv, gp_axv2); }

/* direções digitais (Up/Down/Left/Right) a partir do eixo */
static int name_dir(const char *n) {
  /* retorna: 1=Up 2=Down 3=Left 4=Right 0=não-direção */
  if (!strcasecmp(n, "Up"))    return 1;
  if (!strcasecmp(n, "Down"))  return 2;
  if (!strcasecmp(n, "Left"))  return 3;
  if (!strcasecmp(n, "Right")) return 4;
  if (!strcasecmp(n, "MenuUp"))    return 1;
  if (!strcasecmp(n, "MenuDown"))  return 2;
  if (!strcasecmp(n, "MenuLeft"))  return 3;
  if (!strcasecmp(n, "MenuRight")) return 4;
  return 0;
}

static int dir_held(int d) {
  int h = logical_h(), v = logical_v();
  switch (d) {
    case 3: return h < -gp_axthresh;  /* Left  */
    case 4: return h >  gp_axthresh;  /* Right */
    case 1: return gp_invv ? (v > gp_axthresh) : (v < -gp_axthresh);  /* Up   */
    case 2: return gp_invv ? (v < -gp_axthresh) : (v > gp_axthresh);  /* Down */
  }
  return 0;
}

/* loga nome de ação 1× p/ calibrar */
static void log_action(const char *kind, const char *n) {
  static char seen[64][24]; static int sn = 0;
  if (!gp_log) return;
  for (int i = 0; i < sn; i++) if (!strcmp(seen[i], n)) return;
  if (sn < 64) { strncpy(seen[sn], n, 23); seen[sn][23] = 0; sn++; }
  fprintf(stderr, "[GP] action %s(\"%s\")\n", kind, n); fflush(stderr);
}

/* ---- lógica core por NOME de ação (compartilhada pelos hooks string e int) ---- */
static int core_GetButton(const char *nm) {
  if (gv_force) return 1;
  int b = name_btn(nm); if (b >= 0) return gp_btn[gp_map[b]] ? 1 : 0;
  int k = name_axis(nm); if (k) return (axis_cur(k) > gp_axbtn_thresh) ? 1 : 0;  /* eixo+ segurado */
  int d = name_dir(nm); if (d)  return dir_held(d) ? 1 : 0;
  return 0;
}
static int core_GetButtonDown(const char *nm) {
  if (gv_force) { if (gp_log) { fprintf(stderr, "[GP] >>> FORCE GetButtonDown(\"%s\")=1\n", nm); fflush(stderr); } return 1; }
  int b = name_btn(nm), r = 0;
  if (b >= 0) { int j = gp_map[b]; r = (gp_btn[j] && !gp_btn_prev[j]) ? 1 : 0; }
  else { int k = name_axis(nm);
    /* axis-as-button: se input é VIRTUAL (gv_v/gv_h), retorna SEGURADO (o menu faz seu
       próprio edge); se físico, edge real p/ não scrollar sem parar. */
    if (k == 2) r = gv_v ? (gv_v > gp_axbtn_thresh) : (logical_v() > gp_axbtn_thresh && !vpos_prev);
    else if (k == 1) r = gv_h ? (gv_h > gp_axbtn_thresh) : (logical_h() > gp_axbtn_thresh && !hpos_prev);
    /* direção digital (MenuUp/Down/...): EDGE, senão o menu rola 1 item POR FRAME */
    else { int d = name_dir(nm); if (d) r = (dir_held(d) && !dirp_prev[d]) ? 1 : 0; } }
  if (gp_log && r) { fprintf(stderr, "[GP] >>> GetButtonDown(\"%s\")=1\n", nm); fflush(stderr); }
  return r;
}
static int core_GetButtonUp(const char *nm) {
  int b = name_btn(nm);
  if (b >= 0) { int j = gp_map[b]; return (!gp_btn[j] && gp_btn_prev[j]) ? 1 : 0; }
  return 0;
}
static int core_GetNegativeButton(const char *nm) {
  if (gv_force) return 1;
  int k = name_axis(nm); if (k) return (axis_cur(k) < -gp_axbtn_thresh) ? 1 : 0;
  return 0;
}
static int core_GetNegativeButtonDown(const char *nm) {
  if (gv_force) return 1;
  int k = name_axis(nm), r = 0;
  if (k == 2) r = gv_v ? (gv_v < -gp_axbtn_thresh) : (logical_v() < -gp_axbtn_thresh && !vneg_prev);
  else if (k == 1) r = gv_h ? (gv_h < -gp_axbtn_thresh) : (logical_h() < -gp_axbtn_thresh && !hneg_prev);
  if (gp_log && r) { fprintf(stderr, "[GP] >>> GetNegativeButtonDown(\"%s\")=1\n", nm); fflush(stderr); }
  return r;
}
static float core_GetAxis(const char *nm) {
  int kind = name_axis(nm), inv = 0, v = 0;
  if (kind == 1) v = logical_h();                       /* horizontal: stick0 + dpad4 + virt */
  else if (kind == 2) { v = logical_v(); inv = !gp_invv; }  /* vertical: stick1 + dpad5 + virt */
  else return 0.0f;
  if (v > -gp_axdead && v < gp_axdead) return 0.0f;
  float f = v / 32767.0f; if (f > 1) f = 1; if (f < -1) f = -1;
  f = inv ? -f : f;   /* Unity: up=+1 */
  if (gp_log) { fprintf(stderr, "[GP] >>> GetAxis(\"%s\")=%.2f (v=%d)\n", nm, f, v); fflush(stderr); }
  return f;
}

/* ---- gating por PLAYER (anti Player-2-fantasma) ----
 * Os hooks ignoravam o `self` (objeto Rewired.Player) e devolviam o MESMO input
 * p/ TODOS os players. Resultado: quando o jogo pergunta o input do Player 2,
 * recebia o nosso -> P2 "entra" sozinho e joga com o mesmo controle -> e a
 * PlayerManager.Update() estoura NullReferenceException (P2 meio-inicializado) ao
 * sair da casa -> tela trava. Rewired.Player.get_id = ldr w0,[self,#0x1C] (P1=0,
 * P2=1...). Bloqueia (input neutro) p/ qualquer Player com id>=1. CUP_GP_P2 libera. */
static int g_allow_p2 = -1;
static int player_blocked(void *self) {
  if (g_allow_p2 < 0) g_allow_p2 = getenv("CUP_GP_P2") ? 1 : 0;
  if (g_allow_p2 || !self) return 0;
  int id = *(int *)((char *)self + 0x1C);
  return (id >= 1) ? 1 : 0;   /* id 0 = P1, id<0 = System/menu -> liberados */
}
/* ---- hooks string-overload (Rewired.Player.*(string actionName)) ---- */
int gp_GetButton(void *self, void *name) {
  if (player_blocked(self)) return 0;
  char nm[32]; str_ascii(name, nm, sizeof nm); log_action("GetButton", nm);
  return core_GetButton(nm);
}
int gp_GetButtonDown(void *self, void *name) {
  if (player_blocked(self)) return 0;
  char nm[32]; str_ascii(name, nm, sizeof nm); log_action("GetButtonDown", nm);
  return core_GetButtonDown(nm);
}
int gp_GetButtonUp(void *self, void *name) {
  if (player_blocked(self)) return 0;
  char nm[32]; str_ascii(name, nm, sizeof nm);
  return core_GetButtonUp(nm);
}
int gp_GetNegativeButton(void *self, void *name) {
  if (player_blocked(self)) return 0;
  char nm[32]; str_ascii(name, nm, sizeof nm);
  return core_GetNegativeButton(nm);
}
int gp_GetNegativeButtonDown(void *self, void *name) {
  if (player_blocked(self)) return 0;
  char nm[32]; str_ascii(name, nm, sizeof nm);
  return core_GetNegativeButtonDown(nm);
}
float gp_GetAxis(void *self, void *name) {
  if (player_blocked(self)) return 0.0f;
  char nm[32]; str_ascii(name, nm, sizeof nm); log_action("GetAxis", nm);
  return core_GetAxis(nm);
}

/* ---- hooks int-overload (Rewired.Player.*(int actionId)) ----
 * 🔑 O MENU usa ESTE caminho: SlotSelectScreen.GetButtonDown(CupheadButton) ->
 * AnyPlayerInput.GetButtonDown(CupheadButton) 0xCBC1A4 -> Player.GetButtonDown(int)
 * 0x11A54B0 — o enum CupheadButton É o actionId do Rewired (Accept=13 etc). Os
 * hooks de string nunca eram consultados pelo menu (por isso ele não navegava). */
static const char *aid_name(int id) {
  switch (id) {
    case 0:  return "MoveHorizontal";
    case 1:  return "MoveVertical";
    case 2:  return "Jump";
    case 3:  return "Shoot";
    case 4:  return "Super";
    case 5:  return "SwitchWeapon";
    case 6:  return "Lock";
    case 7:  return "Dash";
    case 8:  return "Pause";
    case 13: return "Accept";
    case 14: return "Cancel";
    case 16: return "MenuUp";
    case 18: return "MenuLeft";
    case 19: return "MenuDown";
    case 20: return "MenuRight";
    case 22: return "MenuHorizontal";
    case 23: return "MenuVertical";
  }
  return "?";  /* NextPage(11)/PreviousPage(12)/EquipMenu(15)/Swap(26): sem mapa ainda */
}
static void log_action_i(const char *kind, int id, const char *nm) {
  static unsigned seen = 0;  /* bitmask por id (0..31), loga 1ª vez de cada */
  if (!gp_log || id < 0 || id > 31 || (seen & (1u << id))) return;
  seen |= 1u << id;
  fprintf(stderr, "[GP] action %s(id=%d \"%s\")\n", kind, id, nm); fflush(stderr);
}
int gp_GetButton_i(void *self, int id) {
  if (player_blocked(self)) return 0;
  const char *nm = aid_name(id); log_action_i("GetButton", id, nm);
  return core_GetButton(nm);
}
int gp_GetButtonDown_i(void *self, int id) {
  if (player_blocked(self)) return 0;
  const char *nm = aid_name(id); log_action_i("GetButtonDown", id, nm);
  return core_GetButtonDown(nm);
}
int gp_GetButtonUp_i(void *self, int id) {
  if (player_blocked(self)) return 0;
  return core_GetButtonUp(aid_name(id));
}
int gp_GetNegativeButton_i(void *self, int id) {
  if (player_blocked(self)) return 0;
  return core_GetNegativeButton(aid_name(id));
}
int gp_GetNegativeButtonDown_i(void *self, int id) {
  if (player_blocked(self)) return 0;
  const char *nm = aid_name(id); log_action_i("GetNegButtonDown", id, nm);
  return core_GetNegativeButtonDown(nm);
}
float gp_GetAxis_i(void *self, int id) {
  if (player_blocked(self)) return 0.0f;
  const char *nm = aid_name(id); log_action_i("GetAxis", id, nm);
  return core_GetAxis(nm);
}

/* "qualquer botão" — p/ o disclaimer "press any button to skip" e menus.
   GetAnyButton = algum botão segurado; GetAnyButtonDown = edge (algum botão novo). */
int gp_GetAnyButton(void *self) {
  (void)self;
  if (gv_force) return 1;
  for (int i = 0; i < GP_NBTN; i++) if (gp_btn[i]) return 1;
  return 0;
}
int gp_GetAnyButtonDown(void *self) {
  (void)self;
  if (gv_force) return 1;
  for (int i = 0; i < GP_NBTN; i++) if (gp_btn[i] && !gp_btn_prev[i]) {
    if (gp_log) { fprintf(stderr, "[GP] >>> GetAnyButtonDown=1 (btn%d)\n", i); fflush(stderr); }
    return 1;
  }
  return 0;
}
/* Versões de Rewired.Player.GetAnyButton/Down (self = Player): GATEADAS por player.
 * O JOIN do Player 2 é detectado quando o Player 2 (não-juntado) vê "qualquer botão"
 * -> player_blocked(self) corta isso (id>=1) -> P2 nunca junta -> sem NullRef em
 * PlayerManager.Update. NÃO usar no AnyPlayerInput global (0xCC2854): lá self NÃO é
 * um Player (ler [self+0x1C] seria lixo) — esse fica no gp_GetAnyButton(Down) cru. */
int gp_GetAnyButton_player(void *self) {
  if (player_blocked(self)) return 0;
  return gp_GetAnyButton(self);
}
int gp_GetAnyButtonDown_player(void *self) {
  if (player_blocked(self)) return 0;
  return gp_GetAnyButtonDown(self);
}

/* CUP_GPJOIN: o menu do Cuphead ignora input se o Rewired acha que não há controle
   atribuído ao player (player não "joined"). Forçamos: isPlaying=true + joystickCount=1
   (player E global). Assim o menu processa a navegação/seleção dos meus hooks. */
int gp_isPlaying(void *self) { (void)self; return 1; }
int gp_joystickCount(void *self) { (void)self; return 1; }
static int g_gpjoin = 0;

void gp_install_hooks(uintptr_t base) {
  if (g_gpjoin) {
    hook_arm64(base + 0x11a51bc, (uintptr_t)gp_isPlaying);     /* Player.get_isPlaying -> true */
    /* ⚠️ joystickCount=1 CRASHA (SIGILL): o jogo acessa Joysticks[0] que não existe.
       Só com CUP_GPJOIN_JC (arriscado). isPlaying sozinho é seguro. */
    if (getenv("CUP_GPJOIN_JC")) {
      hook_arm64(base + 0x11ab958, (uintptr_t)gp_joystickCount);
      hook_arm64(base + 0xedfc10,  (uintptr_t)gp_joystickCount);
    }
    fprintf(stderr, "[GPJOIN] isPlaying=true%s\n", getenv("CUP_GPJOIN_JC") ? " + joystickCount=1" : "");
  }
  /* string-overloads do Rewired.Player (RVAs do dump) */
  hook_arm64(base + 0x11a5378, (uintptr_t)gp_GetButton);     /* GetButton(string)     */
  hook_arm64(base + 0x11a5448, (uintptr_t)gp_GetButtonDown); /* GetButtonDown(string) */
  hook_arm64(base + 0x11a5518, (uintptr_t)gp_GetButtonUp);   /* GetButtonUp(string)   */
  hook_arm64(base + 0x11a7f90, (uintptr_t)gp_GetAxis);       /* GetAxis(string)       */
  hook_arm64(base + 0x11a807c, (uintptr_t)gp_GetAxis);       /* GetAxisRaw(string)    */
  /* "any button" — disclaimer skip + Rewired.Player.GetAnyButton/Down */
  hook_arm64(base + 0xCC2854,  (uintptr_t)gp_GetAnyButtonDown); /* AnyPlayerInput (GLOBAL, NÃO gatear) */
  hook_arm64(base + 0x11a66f8, (uintptr_t)gp_GetAnyButton_player);     /* Rewired.Player.GetAnyButton (gateado)     */
  hook_arm64(base + 0x11a672c, (uintptr_t)gp_GetAnyButtonDown_player); /* Rewired.Player.GetAnyButtonDown (gateado) */
  hook_arm64(base + 0x11a6968, (uintptr_t)gp_GetNegativeButton);     /* GetNegativeButton(string)     */
  hook_arm64(base + 0x11a6a38, (uintptr_t)gp_GetNegativeButtonDown); /* GetNegativeButtonDown(string) */
  /* int-overloads — o caminho REAL do menu (CupheadButton -> actionId) */
  hook_arm64(base + 0x11a53e0, (uintptr_t)gp_GetButton_i);             /* GetButton(int)             */
  hook_arm64(base + 0x11a54b0, (uintptr_t)gp_GetButtonDown_i);         /* GetButtonDown(int)         */
  hook_arm64(base + 0x11a5580, (uintptr_t)gp_GetButtonUp_i);           /* GetButtonUp(int)           */
  hook_arm64(base + 0x11a69d0, (uintptr_t)gp_GetNegativeButton_i);     /* GetNegativeButton(int)     */
  hook_arm64(base + 0x11a6aa0, (uintptr_t)gp_GetNegativeButtonDown_i); /* GetNegativeButtonDown(int) */
  hook_arm64(base + 0x11a8014, (uintptr_t)gp_GetAxis_i);               /* GetAxis(int)               */
  hook_arm64(base + 0x11a80e4, (uintptr_t)gp_GetAxis_i);               /* GetAxisRaw(int)            */
}

void gp_init(uintptr_t il2cpp_base) {
  gp_log = getenv("CUP_GPLOG") ? 1 : 0;
  gp_virt = getenv("CUP_GPVIRT") ? 1 : 0;
  g_gpjoin = getenv("CUP_GPJOIN") ? 1 : 0;
  if (getenv("CUP_GP_AXH")) gp_axh = atoi(getenv("CUP_GP_AXH"));
  if (getenv("CUP_GP_AXV")) gp_axv = atoi(getenv("CUP_GP_AXV"));
  if (getenv("CUP_GP_AXH2")) gp_axh2 = atoi(getenv("CUP_GP_AXH2"));
  if (getenv("CUP_GP_AXV2")) gp_axv2 = atoi(getenv("CUP_GP_AXV2"));
  if (getenv("CUP_GP_INVV")) gp_invv = atoi(getenv("CUP_GP_INVV"));
  for (int i = 0; i < B_COUNT; i++)
    if (getenv(gp_envname[i])) gp_map[i] = atoi(getenv(gp_envname[i]));
  const char *dev = getenv("CUP_GP_DEV") ? getenv("CUP_GP_DEV") : "/dev/input/js0";
  int fd = gp_open(dev);
  fprintf(stderr, "[GAMEPAD] js=%s fd=%d log=%d (hooks Rewired.Player string overloads)\n",
          dev, fd, gp_log);
  gp_install_hooks(il2cpp_base);
}
