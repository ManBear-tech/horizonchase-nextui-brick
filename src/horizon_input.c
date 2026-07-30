#define _GNU_SOURCE
#include "horizon_input.h"

#include <SDL2/SDL.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <unistd.h>

/*
 * Horizon Chase 2.6.9 uses:
 *
 *   InputSourceManager.GetCurrentInputSource()
 *     -> GamepadInputSource
 *       -> NavActionSet.CheckActions()
 *
 * The Android Input System device cannot be populated by Linux evdev in the
 * so-loader environment.  We therefore intercept only the game's input-source
 * getters, keeping NavActionSet, menu listeners and vehicle control untouched.
 *
 * Logical layout is always Xbox:
 *   A confirm/accelerate, B cancel/nitro, X extra/nitro, Y camera,
 *   Back option, Start pause, LB/RB bumpers, LT/RT analog triggers.
 */

enum hc_button {
  HC_A,
  HC_B,
  HC_X,
  HC_Y,
  HC_LB,
  HC_RB,
  HC_BACK,
  HC_START,
  HC_L3,
  HC_R3,
  HC_UP,
  HC_DOWN,
  HC_LEFT,
  HC_RIGHT,
  HC_BUTTON_COUNT
};

enum hc_axis {
  HC_LX,
  HC_LY,
  HC_RX,
  HC_RY,
  HC_LT,
  HC_RT,
  HC_AXIS_COUNT
};

static uintptr_t g_base;
static int g_installed;
static int g_install_retry;
static int g_initialized;
static int g_open_retry;
static int g_exit_requested;
static SDL_GameController *g_controller;
static unsigned char g_button[HC_BUTTON_COUNT];
static unsigned char g_previous_button[HC_BUTTON_COUNT];
static float g_axis[HC_AXIS_COUNT];
static float g_previous_axis[HC_AXIS_COUNT];
static int g_virtual_button_frames[HC_BUTTON_COUNT];
static int g_virtual_axis_frames[HC_AXIS_COUNT];
static float g_virtual_axis_value[HC_AXIS_COUNT];

static const float HC_MENU_DEADZONE = 0.45f;
static const float HC_STICK_DEADZONE = 0.18f;
static const float HC_TRIGGER_THRESHOLD = 0.35f;

static float hc_clamp_axis(Sint16 value) {
  float result = value < 0 ? (float)value / 32768.0f
                           : (float)value / 32767.0f;
  if (result > 1.0f) result = 1.0f;
  if (result < -1.0f) result = -1.0f;
  return result;
}

static float hc_deadzone(float value) {
  return (value > -HC_STICK_DEADZONE && value < HC_STICK_DEADZONE)
             ? 0.0f
             : value;
}

static int hc_axis_negative(int axis) {
  return g_axis[axis] < -HC_MENU_DEADZONE;
}

static int hc_axis_positive(int axis) {
  return g_axis[axis] > HC_MENU_DEADZONE;
}

static int hc_axis_negative_down(int axis) {
  return hc_axis_negative(axis) &&
         g_previous_axis[axis] >= -HC_MENU_DEADZONE;
}

static int hc_axis_positive_down(int axis) {
  return hc_axis_positive(axis) &&
         g_previous_axis[axis] <= HC_MENU_DEADZONE;
}

static int hc_button_down(int button) {
  return g_button[button] && !g_previous_button[button];
}

static int hc_left_down(void) {
  return hc_button_down(HC_LEFT) || hc_axis_negative_down(HC_LX);
}

static int hc_right_down(void) {
  return hc_button_down(HC_RIGHT) || hc_axis_positive_down(HC_LX);
}

static int hc_up_down(void) {
  return hc_button_down(HC_UP) || hc_axis_negative_down(HC_LY);
}

static int hc_down_down(void) {
  return hc_button_down(HC_DOWN) || hc_axis_positive_down(HC_LY);
}

static int hc_up_held(void) {
  return g_button[HC_UP] || hc_axis_negative(HC_LY);
}

static void *hc_current_input_source(void *self, void *method_info) {
  (void)method_info;
  return self ? *(void **)((char *)self + 0x18) : NULL;
}

static int hc_was_left(void *self, void *method_info) {
  (void)self;
  (void)method_info;
  return hc_left_down() || hc_button_down(HC_LB);
}

static int hc_was_right(void *self, void *method_info) {
  (void)self;
  (void)method_info;
  return hc_right_down() || hc_button_down(HC_RB);
}

static int hc_was_up(void *self, void *method_info) {
  (void)self;
  (void)method_info;
  return hc_up_down();
}

static int hc_was_down(void *self, void *method_info) {
  (void)self;
  (void)method_info;
  return hc_down_down();
}

static int hc_was_confirm(void *self, void *method_info) {
  (void)self;
  (void)method_info;
  return hc_button_down(HC_A);
}

static int hc_was_cancel(void *self, void *method_info) {
  (void)self;
  (void)method_info;
  return hc_button_down(HC_B);
}

static int hc_was_option(void *self, void *method_info) {
  (void)self;
  (void)method_info;
  return hc_button_down(HC_BACK);
}

static int hc_was_extra(void *self, void *method_info) {
  (void)self;
  (void)method_info;
  return hc_button_down(HC_X);
}

static int hc_was_start(void *self, void *method_info) {
  (void)self;
  (void)method_info;
  return hc_button_down(HC_START);
}

static int hc_was_left_bumper(void *self, void *method_info) {
  (void)self;
  (void)method_info;
  return hc_button_down(HC_LB);
}

static int hc_was_right_bumper(void *self, void *method_info) {
  (void)self;
  (void)method_info;
  return hc_button_down(HC_RB);
}

static int hc_was_first_person(void *self, void *method_info) {
  (void)self;
  (void)method_info;
  return hc_button_down(HC_Y);
}

static int hc_is_accelerating(void *self, void *method_info) {
  (void)self;
  (void)method_info;
  return g_button[HC_A] || g_button[HC_RB] || hc_up_held() ||
         g_axis[HC_LT] > HC_TRIGGER_THRESHOLD ||
         g_axis[HC_RT] > HC_TRIGGER_THRESHOLD;
}

static int hc_was_nitro(void *self, void *method_info) {
  (void)self;
  (void)method_info;
  return hc_button_down(HC_X) || hc_button_down(HC_B) ||
         hc_button_down(HC_LB);
}

static float hc_horizontal(void *self, void *method_info) {
  (void)self;
  (void)method_info;
  if (g_button[HC_LEFT]) return -1.0f;
  if (g_button[HC_RIGHT]) return 1.0f;
  return hc_deadzone(g_axis[HC_LX]);
}

static float hc_vertical(void *self, void *method_info) {
  (void)self;
  (void)method_info;
  if (g_button[HC_UP]) return 1.0f;
  if (g_button[HC_DOWN]) return -1.0f;
  return -hc_deadzone(g_axis[HC_LY]);
}

static int hc_patch(unsigned long offset, void *replacement) {
  if (!g_base || !offset || !replacement) return 0;
  long page_size = sysconf(_SC_PAGESIZE);
  uintptr_t address = g_base + offset;
  uintptr_t page = address & ~((uintptr_t)page_size - 1);
  if (mprotect((void *)page, (size_t)page_size * 2,
               PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    fprintf(stderr, "[HCINPUT] mprotect 0x%lx: %s\n",
            offset, strerror(errno));
    return 0;
  }
  uint32_t *code = (uint32_t *)address;
  code[0] = 0x58000050u; /* ldr x16, [pc, #8] */
  code[1] = 0xD61F0200u; /* br x16 */
  *(uint64_t *)(code + 2) = (uint64_t)(uintptr_t)replacement;
  __builtin___clear_cache((char *)address, (char *)address + 16);
  mprotect((void *)page, (size_t)page_size * 2, PROT_READ | PROT_EXEC);
  return 1;
}

/*
 * UnityEngine.Application.Quit() only flags the player as quitting: on Android
 * the Java activity is what finishes the process afterwards.  This host has no
 * Java player loop, so the managed call returns, the game tears its own view
 * down and the process keeps running with audio and without input, which reads
 * as a freeze.  Route the quit into the same shutdown the SELECT+START hotkey
 * uses, so the render loop leaves through the normal focus/pause/save path.
 */
static void hc_application_quit(void) {
  if (g_exit_requested) return;
  g_exit_requested = 1;
  fprintf(stderr, "[HCINPUT] Application.Quit: saída solicitada pelo jogo\n");
}

static unsigned long hc_method(const char *class_name, const char *method_name,
                               unsigned long exact_rva) {
  /*
   * These RVAs belong to the supported Horizon Chase 2.6.9 IL2CPP image.
   * Runtime method lookup touches metadata that is not initialized yet at
   * JNI_OnLoad time, so patch the versioned entry points directly.
   */
  (void)class_name;
  (void)method_name;
  return exact_rva;
}

int hc_input_install(uintptr_t il2cpp_base) {
  if (g_installed) return 1;
  if (!il2cpp_base) return 0;
  g_base = il2cpp_base;

  struct hc_hook {
    const char *class_name;
    const char *method_name;
    unsigned long exact_rva;
    void *replacement;
  };
  static const struct hc_hook hooks[] = {
      {"InputSourceManager", "GetCurrentInputSource", 0x14255BC,
       (void *)hc_current_input_source},
      {"GamepadInputSource", "get_WasLeftButtonPressedThisFrame", 0x1424E5C,
       (void *)hc_was_left},
      {"GamepadInputSource", "get_WasRightButtonPressedThisFrame", 0x1424F28,
       (void *)hc_was_right},
      {"GamepadInputSource", "get_WasUpButtonPressedThisFrame", 0x1424FA0,
       (void *)hc_was_up},
      {"GamepadInputSource", "get_WasDownButtonPressedThisFrame", 0x1424FFC,
       (void *)hc_was_down},
      {"GamepadInputSource", "get_WasConfirmButtonPressedThisFrame", 0x1425058,
       (void *)hc_was_confirm},
      {"GamepadInputSource", "get_WasCancelButtonPressedThisFrame", 0x142507C,
       (void *)hc_was_cancel},
      {"GamepadInputSource", "get_WasOptionButtonPressedThisFrame", 0x14250A0,
       (void *)hc_was_option},
      {"GamepadInputSource", "get_WasExtraButtonPressedThisFrame", 0x14250C4,
       (void *)hc_was_extra},
      {"GamepadInputSource", "get_WasStartButtonPressedThisFrame", 0x14250E8,
       (void *)hc_was_start},
      {"GamepadInputSource", "get_WasLeftBumperButtonPressedThisFrame", 0x142510C,
       (void *)hc_was_left_bumper},
      {"GamepadInputSource", "get_WasRightBumperButtonPressedThisFrame", 0x1425130,
       (void *)hc_was_right_bumper},
      {"GamepadInputSource", "get_WasFirstPersonButtonPressedThisFrame", 0x1425154,
       (void *)hc_was_first_person},
      {"GamepadInputSource", "get_IsAccelerationButtonPressed", 0x1425178,
       (void *)hc_is_accelerating},
      {"GamepadInputSource", "get_WasNitroButtonPressedThisFrame", 0x1425244,
       (void *)hc_was_nitro},
      {"GamepadInputSource", "GetHorizontal", 0x14252AC,
       (void *)hc_horizontal},
      {"GamepadInputSource", "GetVertical", 0x1425370,
       (void *)hc_vertical},
      {"UnityEngine.Application", "Quit", 0x2778FD8,
       (void *)hc_application_quit},
      {"UnityEngine.Application", "Quit(int)", 0x2779014,
       (void *)hc_application_quit},
  };

  int patched = 0;
  for (unsigned i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++) {
    unsigned long offset = hc_method(hooks[i].class_name, hooks[i].method_name,
                                     hooks[i].exact_rva);
    if (hc_patch(offset, hooks[i].replacement)) patched++;
  }
  g_installed = patched == (int)(sizeof(hooks) / sizeof(hooks[0]));
  fprintf(stderr,
          "[HCINPUT] Xbox nativo: %d/%zu pontos do fluxo Horizon instalados\n",
          patched, sizeof(hooks) / sizeof(hooks[0]));
  return g_installed;
}

static void hc_clear_state(void) {
  memset(g_button, 0, sizeof(g_button));
  memset(g_axis, 0, sizeof(g_axis));
}

static void hc_add_linux_mapping(int index) {
  SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(index);
  char guid_text[64];
  SDL_JoystickGetGUIDString(guid, guid_text, sizeof(guid_text));
  const char *name = SDL_JoystickNameForIndex(index);
  if (!name) name = "Linux Gamepad";

  /*
   * This exact 0810:0001 adapter ships with the test NextOS handheld and has a
   * known-good mapping in the approved GTA ports.  Its distro mapping varies
   * between SDL builds, so normalize only this GUID.  An explicit user mapping
   * still wins.
   */
  if (strcmp(guid_text, "0300605b100800000100000010010000") == 0) {
    const char *override = getenv("HC_PAD_MAP");
    const char *mapping =
        override && *override
            ? override
            : "0300605b100800000100000010010000,USB Gamepad,"
              "a:b2,b:b1,x:b3,y:b0,"
              "leftshoulder:b4,rightshoulder:b5,"
              "lefttrigger:b6,righttrigger:b7,"
              "back:b8,start:b9,leftstick:b10,rightstick:b11,"
              "leftx:a0,lefty:a1,rightx:a3,righty:a2,"
              "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,"
              "platform:Linux,";
    int result = SDL_GameControllerAddMapping(mapping);
    fprintf(stderr,
            "[HCINPUT] perfil conhecido 0810:0001: %s%s\n",
            result >= 0 ? "OK" : SDL_GetError(),
            override && *override ? " (HC_PAD_MAP)" : "");
    return;
  }

  /* Preserve mappings supplied by PortMaster/NextOS and SDL's own database. */
  if (SDL_IsGameController(index)) return;

  SDL_Joystick *probe = SDL_JoystickOpen(index);
  int buttons = probe ? SDL_JoystickNumButtons(probe) : 0;
  int axes = probe ? SDL_JoystickNumAxes(probe) : 0;
  int hats = probe ? SDL_JoystickNumHats(probe) : 0;
  if (probe) SDL_JoystickClose(probe);
  if (buttons < 8 || axes < 2) return;

  char mapping[1024];
  snprintf(mapping, sizeof(mapping),
           "%s,Horizon Xbox Profile,"
           "a:b0,b:b1,x:b2,y:b3,"
           "leftshoulder:b4,rightshoulder:b5,"
           "lefttrigger:b6,righttrigger:b7,"
           "back:b8,start:b9,leftstick:b10,rightstick:b11,"
           "leftx:a0,lefty:a1,rightx:a2,righty:a3,"
           "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,"
           "platform:Linux,",
           guid_text);
  if (SDL_GameControllerAddMapping(mapping) >= 0) {
    fprintf(stderr,
            "[HCINPUT] perfil Xbox normalizado: js%d \"%s\" "
            "(guid=%s, %d btn, %d axes, %d hats)\n",
            index, name, guid_text, buttons, axes, hats);
  }
}

static void hc_load_mapping_file(const char *path) {
  if (!path || !*path || access(path, R_OK) != 0) return;
  int count = SDL_GameControllerAddMappingsFromFile(path);
  if (count >= 0)
    fprintf(stderr, "[HCINPUT] %d perfis carregados de %s\n", count, path);
}

static int hc_open_controller(void) {
  if (!g_initialized) {
    g_initialized = 1;
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER |
                          SDL_INIT_EVENTS) != 0) {
      fprintf(stderr, "[HCINPUT] SDL input falhou: %s\n", SDL_GetError());
      return 0;
    }
    SDL_GameControllerEventState(SDL_ENABLE);
    hc_load_mapping_file(getenv("SDL_GAMECONTROLLERCONFIG_FILE"));
    hc_load_mapping_file(
        "/storage/.config/SDL-GameControllerDB/gamecontrollerdb.txt");
  }

  int count = SDL_NumJoysticks();
  for (int i = 0; i < count; i++) {
    hc_add_linux_mapping(i);
    if (!SDL_IsGameController(i)) continue;
    g_controller = SDL_GameControllerOpen(i);
    if (!g_controller) continue;
    SDL_Joystick *joystick = SDL_GameControllerGetJoystick(g_controller);
    char guid_text[64];
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joystick), guid_text,
                              sizeof(guid_text));
    fprintf(stderr, "[HCINPUT] conectado como Xbox: %s (guid=%s)\n",
            SDL_GameControllerName(g_controller)
                ? SDL_GameControllerName(g_controller)
                : "controle",
            guid_text);
    char *mapping = SDL_GameControllerMapping(g_controller);
    if (mapping) {
      fprintf(stderr, "[HCINPUT] mapping ativo: %s\n", mapping);
      SDL_free(mapping);
    }
    return 1;
  }
  if (g_open_retry == 0)
    fprintf(stderr, "[HCINPUT] nenhum controle detectado; aguardando hot-plug\n");
  return 0;
}

static unsigned char hc_gc_button(SDL_GameControllerButton button) {
  return g_controller &&
         SDL_GameControllerGetButton(g_controller, button) != 0;
}

static void hc_read_controller(void) {
  if (!g_controller) return;
  if (!SDL_GameControllerGetAttached(g_controller)) {
    fprintf(stderr, "[HCINPUT] controle desconectado\n");
    SDL_GameControllerClose(g_controller);
    g_controller = NULL;
    return;
  }
  SDL_GameControllerUpdate();
  g_button[HC_A] = hc_gc_button(SDL_CONTROLLER_BUTTON_A);
  g_button[HC_B] = hc_gc_button(SDL_CONTROLLER_BUTTON_B);
  g_button[HC_X] = hc_gc_button(SDL_CONTROLLER_BUTTON_X);
  g_button[HC_Y] = hc_gc_button(SDL_CONTROLLER_BUTTON_Y);
  g_button[HC_LB] = hc_gc_button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
  g_button[HC_RB] = hc_gc_button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
  g_button[HC_BACK] = hc_gc_button(SDL_CONTROLLER_BUTTON_BACK);
  g_button[HC_START] = hc_gc_button(SDL_CONTROLLER_BUTTON_START);
  g_button[HC_L3] = hc_gc_button(SDL_CONTROLLER_BUTTON_LEFTSTICK);
  g_button[HC_R3] = hc_gc_button(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
  g_button[HC_UP] = hc_gc_button(SDL_CONTROLLER_BUTTON_DPAD_UP);
  g_button[HC_DOWN] = hc_gc_button(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
  g_button[HC_LEFT] = hc_gc_button(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
  g_button[HC_RIGHT] = hc_gc_button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
  g_axis[HC_LX] = hc_clamp_axis(
      SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_LEFTX));
  g_axis[HC_LY] = hc_clamp_axis(
      SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_LEFTY));
  g_axis[HC_RX] = hc_clamp_axis(
      SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_RIGHTX));
  g_axis[HC_RY] = hc_clamp_axis(
      SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_RIGHTY));
  g_axis[HC_LT] = hc_clamp_axis(
      SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT));
  g_axis[HC_RT] = hc_clamp_axis(
      SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT));
  if (g_axis[HC_LT] < 0.0f) g_axis[HC_LT] = 0.0f;
  if (g_axis[HC_RT] < 0.0f) g_axis[HC_RT] = 0.0f;
}

static int hc_token_button(const char *token) {
  static const char *names[HC_BUTTON_COUNT] = {
      "a",     "b",     "x",    "y",     "lb",   "rb",   "back",
      "start", "l3",    "r3",   "up",    "down", "left", "right"};
  for (int i = 0; i < HC_BUTTON_COUNT; i++)
    if (strcasecmp(token, names[i]) == 0 ||
        (i == HC_BACK && strcasecmp(token, "select") == 0))
      return i;
  return -1;
}

static int hc_virtual_duration(char *token, int fallback) {
  char *separator = strrchr(token, ':');
  if (!separator || !separator[1])
    return fallback;
  char *end = NULL;
  long value = strtol(separator + 1, &end, 10);
  if (!end || *end || value <= 0)
    return fallback;
  *separator = '\0';
  if (value > 36000)
    value = 36000;
  return (int)value;
}

static int hc_token_axis(const char *token, int *axis, float *value) {
  if (strcasecmp(token, "lt") == 0) {
    *axis = HC_LT;
    *value = 1.0f;
    return 1;
  }
  if (strcasecmp(token, "rt") == 0) {
    *axis = HC_RT;
    *value = 1.0f;
    return 1;
  }

  static const char *names[] = {"lx=", "ly=", "rx=", "ry="};
  for (int i = 0; i < 4; i++) {
    if (strncasecmp(token, names[i], 3) != 0)
      continue;
    char *end = NULL;
    float parsed = strtof(token + 3, &end);
    if (!end || *end)
      return 0;
    if (parsed < -1.0f) parsed = -1.0f;
    if (parsed > 1.0f) parsed = 1.0f;
    *axis = i;
    *value = parsed;
    return 1;
  }
  return 0;
}

static void hc_read_virtual_input(void) {
  if (!getenv("HC_INPUT_TEST")) return;
  const char *path = getenv("HC_PAD_FILE");
  if (!path || !*path) path = "/tmp/horizon-pad";
  FILE *file = fopen(path, "r");
  if (file) {
    int default_duration = 4;
    const char *duration_env = getenv("HC_PAD_PULSE_FRAMES");
    if (duration_env && atoi(duration_env) > 0)
      default_duration = atoi(duration_env);

    char token[48];
    int read_any = 0;
    while (fscanf(file, "%47s", token) == 1) {
      read_any = 1;
      int duration = hc_virtual_duration(token, default_duration);
      int button = hc_token_button(token);
      if (button >= 0) {
        g_virtual_button_frames[button] = duration;
      } else {
        int axis = 0;
        float value = 0.0f;
        if (hc_token_axis(token, &axis, &value)) {
          g_virtual_axis_frames[axis] = duration;
          g_virtual_axis_value[axis] = value;
        }
      }
      fprintf(stderr, "[HCINPUT] teste virtual: %s (%d frames)\n",
              token, duration);
    }
    fclose(file);
    if (read_any)
      unlink(path);
  }

  for (int i = 0; i < HC_BUTTON_COUNT; i++) {
    if (g_virtual_button_frames[i] > 0) {
      g_virtual_button_frames[i]--;
      g_button[i] = 1;
    }
  }
  for (int i = 0; i < HC_AXIS_COUNT; i++) {
    if (g_virtual_axis_frames[i] > 0) {
      g_virtual_axis_frames[i]--;
      g_axis[i] = g_virtual_axis_value[i];
    }
  }
}

void hc_input_poll(void) {
  if (!g_installed) {
    if (g_base && g_install_retry-- <= 0) {
      g_install_retry = 60;
      hc_input_install(g_base);
    }
    if (!g_installed) return;
  }
  memcpy(g_previous_button, g_button, sizeof(g_button));
  memcpy(g_previous_axis, g_axis, sizeof(g_axis));
  hc_clear_state();

  if (!g_controller) {
    if (g_open_retry <= 0) {
      hc_open_controller();
      g_open_retry = 120;
    } else {
      g_open_retry--;
    }
  }
  hc_read_controller();
  hc_read_virtual_input();
  if (!g_exit_requested && g_button[HC_BACK] && g_button[HC_START]) {
    g_exit_requested = 1;
    fprintf(stderr, "[HCINPUT] SELECT+START: saída solicitada\n");
  }
}

int hc_input_exit_requested(void) {
  return g_exit_requested;
}

void hc_input_shutdown(void) {
  if (g_controller) SDL_GameControllerClose(g_controller);
  g_controller = NULL;
}
