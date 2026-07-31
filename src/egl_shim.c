#ifndef PORT_WINDOW_TITLE
#define PORT_WINDOW_TITLE "nextos_port"
#endif
/*
 * egl_shim.c -- EGL wrapper backed by SDL2 (adaptive OpenGL ES)
 *
 * Each fake EGL context gets a real SDL GL context. We keep a bootstrap
 * context around as the share root so all contexts can share resources.
 */

#include <SDL2/SDL.h>
#include <GLES2/gl2.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <dlfcn.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "egl_config_contract.h"
#include "egl_shim.h"
#include "util.h"

static int g_screen_w = 1280;
static int g_screen_h = 720;
#define SCREEN_WIDTH g_screen_w
#define SCREEN_HEIGHT g_screen_h

/* The fake Android EGL surface is backed by SDL, but Unity also enumerates EGL
 * configs and creates shared worker contexts. Capture SDL's real EGL objects
 * so those queries and the upload-worker pbuffer use the device's own config. */
static void *(*r_eglGetCurrentDisplay)(void);
static unsigned (*r_eglChooseConfig)(void *, const int *, void **, int, int *);
static unsigned (*r_eglGetConfigAttrib)(void *, void *, int, int *);
static void *(*r_eglGetCurrentSurface)(int);
static void *(*r_eglCreatePbufferSurface)(void *, void *, const int *);
static unsigned (*r_eglMakeCurrent)(void *, void *, void *, void *);
static void *(*r_eglCreateContext)(void *, void *, void *, const int *);
/* dono da WINDOW é por-THREAD (a thread de render do Unity); as demais threads usam
 * um contexto real próprio (compartilhado com o share-root) num pbuffer. */
static pthread_t g_owner_thread;
static int g_have_owner = 0;
static pthread_mutex_t g_owner_mtx2 = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local void *tls_real_ctx = NULL;   /* contexto real próprio da thread (pbuffer) */
static _Thread_local int tls_is_window = 0;       /* esta thread renderiza na window? */
static void *g_real_dpy = NULL;   /* EGLDisplay real do SDL */
static void *g_real_cfg = NULL;   /* real EGLConfig matching the chosen GLES */
static void *g_win_surf = NULL;   /* EGLSurface REAL da window do SDL */
static void *g_pbuf = NULL;       /* pbuffer REAL p/ contextos worker (uploads) */
/* Dono da surface da window: o 1º contexto que pede MakeCurrent(window) fica com a
 * window; os demais (worker do Unity) recebem o pbuffer -> sem EGL_BAD_ACCESS
 * (2 contextos NÃO podem compartilhar a mesma surface). Compartilham recursos, então
 * o upload do worker vale pro render. */
static SDL_GLContext g_window_owner = NULL;

typedef struct {
  SDL_GLContext sdl_context;
  EGLBoolean is_pbuffer;
  EGLSurface current_draw;
  EGLSurface current_read;
  int id;
} _egl_context;

static SDL_Window *egl_window = NULL;
static SDL_GLContext egl_share_root = NULL;
static pthread_mutex_t egl_context_create_mutex = PTHREAD_MUTEX_INITIALIZER;
static int frame_count = 0;
static int next_context_id = 1;
/* Actual GLES version shared by the bootstrap and Unity contexts. Native
 * ASTC/ETC2 benefits from ES3 on R36S; ES2 remains the fallback. */
static int g_es_major = 0;
static int g_es_minor = 0;
static int g_red_size = HC_EGL_RGBA_CHANNEL_BITS;
static int g_green_size = HC_EGL_RGBA_CHANNEL_BITS;
static int g_blue_size = HC_EGL_RGBA_CHANNEL_BITS;
static int g_alpha_size = HC_EGL_RGBA_CHANNEL_BITS;
static int g_depth_size = 24;
static int g_stencil_size = 8;
static char g_fallback_config_token;

static _Thread_local _egl_context *current_context = NULL;
static _Thread_local _egl_context *last_context = NULL;
static _Thread_local int has_real_gl = 0;

SDL_Window *egl_shim_get_window(void) { return egl_window; }

void egl_shim_get_size(int *width, int *height) {
  if (width) *width = g_screen_w;
  if (height) *height = g_screen_h;
}

static const char *hc_env(const char *name) {
  const char *value = getenv(name);
  return value && *value ? value : NULL;
}

static int hc_env_on(const char *name) {
  const char *value = hc_env(name);
  return value && strcmp(value, "0") != 0 &&
         strcasecmp(value, "false") != 0 &&
         strcasecmp(value, "no") != 0 &&
         strcasecmp(value, "off") != 0;
}

/*
 * Bully and Sonic 4 prove the backend split used here:
 *
 *   mali/fbdev          -> raw EGL ownership and swap
 *   KMSDRM/Wayland/etc. -> SDL owns bind, unbind and page-flip
 *
 * Moving Unity's context between threads through raw eglMakeCurrent while SDL
 * owns KMS presents an untouched black buffer. Conversely, the old Mali/fbdev
 * stack needs its real EGL objects. Select by the backend that SDL actually
 * opened, not by a device name. The envs remain diagnostic escape hatches.
 */
static int hc_pure_sdl_contexts(void) {
  static int enabled = -1;
  if (enabled >= 0)
    return enabled;

  if (hc_env("HC_PURE_SDL_CONTEXTS")) {
    enabled = hc_env_on("HC_PURE_SDL_CONTEXTS");
  } else if (hc_env_on("HC_RAW_EGL_CONTEXTS")) {
    enabled = 0;
  } else {
    const char *driver = SDL_GetCurrentVideoDriver();
    if (!driver)
      return 0; /* SDL video is not initialized yet; do not cache a guess. */
    enabled = strcasecmp(driver, "mali") != 0;
  }

  {
    const char *driver = SDL_GetCurrentVideoDriver();
    fprintf(stderr,
            "egl_shim: backend '%s' -> contextos %s%s\n",
            driver ? driver : "?",
            enabled ? "SDL puro" : "EGL cru",
            hc_env("HC_PURE_SDL_CONTEXTS") || hc_env_on("HC_RAW_EGL_CONTEXTS")
                ? " (override)" : " (auto)");
  }
  return enabled;
}

static int hc_positive_int(const char *name) {
  const char *value = hc_env(name);
  if (!value) return 0;
  char *end = NULL;
  long parsed = strtol(value, &end, 10);
  return end != value && parsed > 0 && parsed < 32768 ? (int)parsed : 0;
}

static int hc_read_size(const char *path, int *width, int *height) {
  FILE *file = fopen(path, "r");
  if (!file) return 0;
  char text[128] = {0};
  int ok = fgets(text, sizeof text, file) != NULL;
  fclose(file);
  if (!ok) return 0;
  int w = 0, h = 0;
  if (sscanf(text, "%d,%d", &w, &h) != 2 &&
      sscanf(text, "%dx%d", &w, &h) != 2 &&
      sscanf(text, "%*[^0-9]%dx%d", &w, &h) != 2)
    return 0;
  if (w <= 0 || h <= 0 || w >= 32768 || h >= 32768) return 0;
  *width = w;
  *height = h;
  return 1;
}

static void hc_detect_screen_size(void) {
  int width = hc_positive_int("TER_SCREEN_W");
  int height = hc_positive_int("TER_SCREEN_H");
  const char *source = NULL;
  if (!width) width = hc_positive_int("TER_SCREEN_WIDTH");
  if (!height) height = hc_positive_int("TER_SCREEN_HEIGHT");
  if (width > 0 && height > 0) source = "launcher";

  if (!source) {
    SDL_DisplayMode mode;
    if (SDL_GetDesktopDisplayMode(0, &mode) == 0 &&
        mode.w > 0 && mode.h > 0) {
      width = mode.w;
      height = mode.h;
      source = "SDL desktop";
    }
  }

  if (!source) {
    DIR *directory = opendir("/sys/class/drm");
    struct dirent *entry;
    while (directory && (entry = readdir(directory)) != NULL && !source) {
      if (strncmp(entry->d_name, "card", 4) != 0 ||
          !strchr(entry->d_name, '-'))
        continue;
      char path[256], state[32] = {0};
      snprintf(path, sizeof path, "/sys/class/drm/%s/status",
               entry->d_name);
      FILE *file = fopen(path, "r");
      if (!file) continue;
      fgets(state, sizeof state, file);
      fclose(file);
      if (strncmp(state, "connected", 9) != 0) continue;
      snprintf(path, sizeof path, "/sys/class/drm/%s/modes",
               entry->d_name);
      if (hc_read_size(path, &width, &height)) source = "DRM connector";
    }
    if (directory) closedir(directory);
  }

  if (!source &&
      (hc_read_size("/sys/class/graphics/fb0/mode", &width, &height) ||
       hc_read_size("/sys/class/graphics/fb0/modes", &width, &height)))
    source = "fb0 mode";

  if (!source &&
      hc_read_size("/sys/class/graphics/fb0/virtual_size", &width, &height)) {
    if (height > width && (height % 2) == 0) {
      int half = height / 2;
      if (half <= width && half * 2 >= width) height = half;
    }
    source = "fb0 virtual";
  }

  if (source) {
    g_screen_w = width;
    g_screen_h = height;
  }
  fprintf(stderr, "egl_shim: resolução %dx%d (fonte: %s)\n",
          g_screen_w, g_screen_h, source ? source : "fallback");
}

static int ctx_is_gles(void) {
  const unsigned char *(*get_string)(unsigned int) =
      (const unsigned char *(*)(unsigned int))
          SDL_GL_GetProcAddress("glGetString");
  if (!get_string) return 1;
  const char *version = (const char *)get_string(0x1F02 /* GL_VERSION */);
  return !version || strstr(version, "OpenGL ES") != NULL;
}

static void egl_set_ctx_attrs(int major, int minor, int depth, int stencil) {
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  /*
   * This Unity build asks for format 8 (RGBA8888) and then performs an exact
   * R/G/B/A comparison. RGBX8888 happens to be Mesa/Panfrost's first match
   * when alpha is omitted, so advertising that real config makes Unity abort
   * with "Unable to find a configuration matching minimum spec".
   */
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, HC_EGL_RGBA_CHANNEL_BITS);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, depth);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, stencil);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
}

void egl_shim_create_window(void) {
  if (!hc_env_on("HC_NO_FORCE_GLES")) {
    SDL_SetHint("SDL_OPENGL_ES_DRIVER", "1");
    SDL_SetHint("SDL_VIDEO_X11_FORCE_EGL", "1");
  }
  if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "egl_shim: SDL video init falhou: %s\n", SDL_GetError());
    return;
  }
  hc_detect_screen_size();

  int versions[2] = {3, 2};
  int version_count = 2;
  const char *forced = hc_env("HC_GLES_MAJOR");
  if (!forced) forced = hc_env("CUP_GLES_MAJOR");
  if (forced && (forced[0] == '2' || forced[0] == '3')) {
    versions[0] = forced[0] - '0';
    version_count = 1;
  }
  static const struct {
    int depth;
    int stencil;
  } formats[] = {{24, 8}, {16, 0}, {0, 0}};

  Uint32 fullscreen = hc_env_on("HC_EXCLUSIVE_FULLSCREEN")
                          ? SDL_WINDOW_FULLSCREEN
                          : SDL_WINDOW_FULLSCREEN_DESKTOP;
  for (size_t f = 0;
       f < sizeof formats / sizeof formats[0] && !egl_share_root; f++) {
    egl_set_ctx_attrs(versions[0], 0, formats[f].depth,
                      formats[f].stencil);
    egl_window = SDL_CreateWindow(
        PORT_WINDOW_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SCREEN_WIDTH, SCREEN_HEIGHT, SDL_WINDOW_OPENGL | fullscreen);
    if (!egl_window) {
      fprintf(stderr,
              "egl_shim: janela depth%d/stencil%d falhou: %s\n",
              formats[f].depth, formats[f].stencil, SDL_GetError());
      continue;
    }

    for (int v = 0; v < version_count && !egl_share_root; v++) {
      egl_set_ctx_attrs(versions[v], 0, formats[f].depth,
                        formats[f].stencil);
      egl_share_root = SDL_GL_CreateContext(egl_window);
      if (egl_share_root && !hc_env_on("HC_ALLOW_DESKTOP_GL") &&
          !ctx_is_gles()) {
        fprintf(stderr,
                "egl_shim: ES%d devolveu OpenGL desktop; rejeitado\n",
                versions[v]);
        SDL_GL_DeleteContext(egl_share_root);
        egl_share_root = NULL;
      }
      if (egl_share_root) {
        g_es_major = versions[v];
        g_es_minor = 0;
        g_depth_size = formats[f].depth;
        g_stencil_size = formats[f].stencil;
        SDL_GL_GetAttribute(SDL_GL_RED_SIZE, &g_red_size);
        SDL_GL_GetAttribute(SDL_GL_GREEN_SIZE, &g_green_size);
        SDL_GL_GetAttribute(SDL_GL_BLUE_SIZE, &g_blue_size);
        SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE, &g_alpha_size);
        SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &g_depth_size);
        SDL_GL_GetAttribute(SDL_GL_STENCIL_SIZE, &g_stencil_size);
      } else {
        fprintf(stderr,
                "egl_shim: contexto ES%d depth%d/stencil%d falhou: %s\n",
                versions[v], formats[f].depth, formats[f].stencil,
                SDL_GetError());
      }
    }
    if (!egl_share_root) {
      SDL_DestroyWindow(egl_window);
      egl_window = NULL;
    }
  }
  if (!egl_window || !egl_share_root) {
    fprintf(stderr, "egl_shim: nenhuma combinação EGL/GLES criou vídeo\n");
    return;
  }

  fprintf(stderr,
          "egl_shim: contexto ES%d.%d rgba=%d/%d/%d/%d "
          "depth=%d stencil=%d criado em %dx%d\n",
          g_es_major, g_es_minor,
          g_red_size, g_green_size, g_blue_size, g_alpha_size,
          g_depth_size, g_stencil_size,
          SCREEN_WIDTH, SCREEN_HEIGHT);
  {
    const unsigned char *(*get_string)(unsigned int) =
        (const unsigned char *(*)(unsigned int))
            SDL_GL_GetProcAddress("glGetString");
    if (get_string) {
      fprintf(stderr, "egl_shim: GL_VENDOR=%s\n",
              get_string(0x1F00) ? (const char *)get_string(0x1F00) : "?");
      fprintf(stderr, "egl_shim: GL_RENDERER=%s\n",
              get_string(0x1F01) ? (const char *)get_string(0x1F01) : "?");
      fprintf(stderr, "egl_shim: GL_VERSION=%s\n",
              get_string(0x1F02) ? (const char *)get_string(0x1F02) : "?");
      fprintf(stderr, "egl_shim: GL_GLSL=%s\n",
              get_string(0x8B8C) ? (const char *)get_string(0x8B8C) : "?");
    }
  }

  /* captura o EGLDisplay REAL do SDL (contexto está current agora) + escolhe uma
     EGLConfig compatível p/ as queries e contextos worker do Unity. */
  r_eglGetCurrentDisplay = (void *(*)(void))dlsym(RTLD_DEFAULT, "eglGetCurrentDisplay");
  r_eglChooseConfig = (unsigned (*)(void *, const int *, void **, int, int *))dlsym(RTLD_DEFAULT, "eglChooseConfig");
  r_eglGetConfigAttrib = (unsigned (*)(void *, void *, int, int *))dlsym(RTLD_DEFAULT, "eglGetConfigAttrib");
  r_eglGetCurrentSurface = (void *(*)(int))dlsym(RTLD_DEFAULT, "eglGetCurrentSurface");
  r_eglCreatePbufferSurface = (void *(*)(void *, void *, const int *))dlsym(RTLD_DEFAULT, "eglCreatePbufferSurface");
  r_eglMakeCurrent = (unsigned (*)(void *, void *, void *, void *))dlsym(RTLD_DEFAULT, "eglMakeCurrent");
  r_eglCreateContext = (void *(*)(void *, void *, void *, const int *))dlsym(RTLD_DEFAULT, "eglCreateContext");
  if (r_eglGetCurrentDisplay) g_real_dpy = r_eglGetCurrentDisplay();
  if (r_eglGetCurrentSurface) g_win_surf = r_eglGetCurrentSurface(0x3059 /*EGL_DRAW*/);
  if (g_real_dpy && r_eglChooseConfig && r_eglGetConfigAttrib) {
    int attrs[HC_EGL_WINDOW_CONFIG_ATTR_CAPACITY];
    hc_egl_build_window_config_attributes(
        g_es_major, g_depth_size, g_stencil_size, attrs,
        sizeof attrs / sizeof attrs[0]);
    void *cfgs[64];
    int n = 0;
    if (r_eglChooseConfig(g_real_dpy, attrs, cfgs, 64, &n) && n > 0) {
      hc_egl_config_properties required = {
          .red = HC_EGL_RGBA_CHANNEL_BITS,
          .green = HC_EGL_RGBA_CHANNEL_BITS,
          .blue = HC_EGL_RGBA_CHANNEL_BITS,
          .alpha = HC_EGL_RGBA_CHANNEL_BITS,
          .depth = g_depth_size,
          .stencil = g_stencil_size,
          .samples = 0,
          .renderable = g_es_major >= 3
                            ? HC_EGL_OPENGL_ES3_BIT_KHR
                            : HC_EGL_OPENGL_ES2_BIT,
          .surfaces = HC_EGL_WINDOW_BIT | HC_EGL_PBUFFER_BIT,
          .native_visual_type = 0
      };
      hc_egl_config_properties selected = {0};
      int checked = n < 64 ? n : 64;
      for (int i = 0; i < checked; i++) {
        hc_egl_config_properties candidate = {0};
#define HC_QUERY_CONFIG(field, attribute)                                  \
        do {                                                               \
          r_eglGetConfigAttrib(                                            \
              g_real_dpy, cfgs[i], (attribute), &candidate.field);         \
        } while (0)
        HC_QUERY_CONFIG(red, HC_EGL_RED_SIZE);
        HC_QUERY_CONFIG(green, HC_EGL_GREEN_SIZE);
        HC_QUERY_CONFIG(blue, HC_EGL_BLUE_SIZE);
        HC_QUERY_CONFIG(alpha, HC_EGL_ALPHA_SIZE);
        HC_QUERY_CONFIG(depth, HC_EGL_DEPTH_SIZE);
        HC_QUERY_CONFIG(stencil, HC_EGL_STENCIL_SIZE);
        HC_QUERY_CONFIG(samples, HC_EGL_SAMPLES);
        HC_QUERY_CONFIG(renderable, HC_EGL_RENDERABLE_TYPE);
        HC_QUERY_CONFIG(surfaces, HC_EGL_SURFACE_TYPE);
        HC_QUERY_CONFIG(native_visual_type, HC_EGL_NATIVE_VISUAL_TYPE);
#undef HC_QUERY_CONFIG
        if (hc_egl_config_meets_unity(&candidate, &required)) {
          g_real_cfg = cfgs[i];
          selected = candidate;
          break;
        }
      }
      if (g_real_cfg) {
        fprintf(stderr,
                "egl_shim: EGLConfig Unity selecionada "
                "rgba=%d/%d/%d/%d depth=%d stencil=%d "
                "samples=%d renderable=0x%x candidates=%d\n",
                selected.red, selected.green, selected.blue, selected.alpha,
                selected.depth, selected.stencil, selected.samples,
                selected.renderable, n);
      /* pbuffer real p/ os contextos worker do Unity (uploads compartilhados) */
        if (r_eglCreatePbufferSurface) {
          static const int pb[] = {
              0x3057, 16, 0x3056, 16, HC_EGL_NONE
          }; /* EGL_WIDTH, EGL_HEIGHT, EGL_NONE */
          g_pbuf = r_eglCreatePbufferSurface(
              g_real_dpy, g_real_cfg, pb);
          debugPrintf("egl_shim: pbuffer worker real = %p\n", g_pbuf);
        }
      } else {
        fprintf(stderr,
                "egl_shim: nenhuma das %d EGLConfigs reais satisfaz "
                "RGBA8888; usando contrato lógico SDL\n",
                n);
      }
    } else {
      fprintf(stderr,
              "egl_shim: seleção EGLConfig RGBA8888 falhou (n=%d); "
              "usando contrato lógico SDL\n",
              n);
    }
  } else {
    debugPrintf("egl_shim: sem EGLDisplay real (dpy=%p) — usa attribs hardcoded\n", g_real_dpy);
  }

  {
    int drawable_w = 0, drawable_h = 0;
    for (int i = 0; i < 30; i++) {
      SDL_Event event;
      while (SDL_PollEvent(&event)) {}
      SDL_GL_GetDrawableSize(egl_window, &drawable_w, &drawable_h);
      if (drawable_w > 0 && drawable_h > 0 &&
          drawable_w == g_screen_w && drawable_h == g_screen_h)
        break;
      usleep(10000);
    }
    if (drawable_w <= 0 || drawable_h <= 0)
      SDL_GetWindowSize(egl_window, &drawable_w, &drawable_h);
    if (drawable_w > 0 && drawable_h > 0) {
      if (drawable_w != g_screen_w || drawable_h != g_screen_h)
        fprintf(stderr,
                "egl_shim: drawable real %dx%d (pedido %dx%d)\n",
                drawable_w, drawable_h, g_screen_w, g_screen_h);
      g_screen_w = drawable_w;
      g_screen_h = drawable_h;
      char width[16], height[16];
      snprintf(width, sizeof width, "%d", g_screen_w);
      snprintf(height, sizeof height, "%d", g_screen_h);
      setenv("TER_SCREEN_W", width, 1);
      setenv("TER_SCREEN_H", height, 1);
    }
  }

  SDL_GL_MakeCurrent(egl_window, NULL);
  debugPrintf("egl_shim: Context released, ready for game\n");
}

/* --- Mutex hooks (called from imports.c pthread wrappers) --- */

void egl_shim_on_mutex_post_lock(void *mutex_id) {
  (void)mutex_id;
}

void egl_shim_on_mutex_pre_unlock(void *mutex_id) {
  (void)mutex_id;
}

int egl_shim_ensure_current(void) {
  if (has_real_gl)
    return 1;
  _egl_context *ctx = current_context ? current_context : last_context;
  if (!egl_window || !ctx || !ctx->sdl_context)
    return 0;

  int ret;
  if (!hc_pure_sdl_contexts() &&
      g_real_dpy && r_eglMakeCurrent && g_win_surf) {
    int is_main = (syscall(SYS_gettid) == getpid()) && !getenv("CUP_MAINGL");
    int am_owner = !is_main && g_have_owner && pthread_equal(g_owner_thread, pthread_self());
    void *use_ctx = (void *)ctx->sdl_context, *surf;
    if (am_owner) { surf = g_win_surf; tls_is_window = 1; }
    else {
      if (!tls_real_ctx && r_eglCreateContext) {
        int ca[] = { 0x3098, g_es_major, 0x3038 };
        tls_real_ctx = r_eglCreateContext(g_real_dpy, g_real_cfg, egl_share_root, ca);
      }
      if (tls_real_ctx) use_ctx = tls_real_ctx;
      surf = g_pbuf ? g_pbuf : g_win_surf; tls_is_window = 0;
    }
    ret = r_eglMakeCurrent(g_real_dpy, surf, surf, use_ctx) ? 0 : -1;
  } else {
    ret = SDL_GL_MakeCurrent(egl_window, ctx->sdl_context);
  }
  if (ret == 0) {
    has_real_gl = 1;
    current_context = ctx;
    debugPrintf("egl_shim: restored current context [tid=%lx] [ctx_id=%d]\n",
                (unsigned long)pthread_self(), ctx->id);
    return 1;
  }

  debugPrintf("egl_shim: failed to restore current context [tid=%lx] [ctx_id=%d]: %s\n",
              (unsigned long)pthread_self(), ctx->id, SDL_GetError());
  return 0;
}

/* --- EGL API --- */

EGLDisplay egl_shim_GetDisplay(EGLNativeDisplayType display_id) {
  (void)display_id;
  debugPrintf("egl_shim: eglGetDisplay()\n");
  return (EGLDisplay)strdup("display");
}

EGLBoolean egl_shim_Initialize(EGLDisplay dpy, EGLint *major, EGLint *minor) {
  (void)dpy;
  if (major) *major = 1;
  if (minor) *minor = 4;
  debugPrintf("egl_shim: eglInitialize() -> 1.4\n");
  return EGL_TRUE;
}

EGLBoolean egl_shim_Terminate(EGLDisplay dpy) {
  (void)dpy;
  debugPrintf("egl_shim: eglTerminate()\n");
  if (egl_share_root) {
    SDL_GL_DeleteContext(egl_share_root);
    egl_share_root = NULL;
  }
  if (egl_window) {
    SDL_DestroyWindow(egl_window);
    egl_window = NULL;
  }
  return EGL_TRUE;
}

EGLBoolean egl_shim_ChooseConfig(EGLDisplay dpy, const EGLint *attrib_list,
                                  EGLConfig *configs, EGLint config_size,
                                  EGLint *num_config) {
  (void)dpy;
  static int request_logged;
  if (!request_logged) {
    fprintf(stderr,
            "egl_shim: pedido EGLConfig da Unity "
            "rgba=%d/%d/%d/%d depth=%d stencil=%d "
            "samples=%d renderable=0x%x surfaces=0x%x\n",
            hc_egl_attribute_value(
                attrib_list, HC_EGL_RED_SIZE, -1),
            hc_egl_attribute_value(
                attrib_list, HC_EGL_GREEN_SIZE, -1),
            hc_egl_attribute_value(
                attrib_list, HC_EGL_BLUE_SIZE, -1),
            hc_egl_attribute_value(
                attrib_list, HC_EGL_ALPHA_SIZE, -1),
            hc_egl_attribute_value(
                attrib_list, HC_EGL_DEPTH_SIZE, -1),
            hc_egl_attribute_value(
                attrib_list, HC_EGL_STENCIL_SIZE, -1),
            hc_egl_attribute_value(
                attrib_list, HC_EGL_SAMPLES, -1),
            hc_egl_attribute_value(
                attrib_list, HC_EGL_RENDERABLE_TYPE, -1),
            hc_egl_attribute_value(
                attrib_list, HC_EGL_SURFACE_TYPE, -1));
    request_logged = 1;
  }
  if (configs && config_size > 0)
    configs[0] = g_real_cfg
                     ? (EGLConfig)g_real_cfg
                     : (EGLConfig)&g_fallback_config_token;
  if (num_config)
    *num_config = 1;
  return EGL_TRUE;
}

EGLSurface egl_shim_CreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                         EGLNativeWindowType win,
                                         const EGLint *attrib_list) {
  (void)dpy; (void)config; (void)win; (void)attrib_list;
  EGLSurface s = (EGLSurface)strdup("window");
  debugPrintf("egl_shim: eglCreateWindowSurface() -> %p\n", s);
  return s;
}

EGLSurface egl_shim_CreatePbufferSurface(EGLDisplay dpy, EGLConfig config,
                                          const EGLint *attrib_list) {
  (void)dpy; (void)config; (void)attrib_list;
  EGLSurface s = (EGLSurface)strdup("pbuffer");
  debugPrintf("egl_shim: eglCreatePbufferSurface() -> %p\n", s);
  return s;
}

EGLContext egl_shim_CreateContext(EGLDisplay dpy, EGLConfig config,
                                  EGLContext share_context,
                                  const EGLint *attrib_list) {
  (void)dpy; (void)config; (void)share_context; (void)attrib_list;
  _egl_context *c = (_egl_context *)calloc(1, sizeof(_egl_context));
  if (!c)
    return EGL_NO_CONTEXT;

  pthread_mutex_lock(&egl_context_create_mutex);
  /* mesma versão/formato que o share-root negociado */
  if (g_es_major)
    egl_set_ctx_attrs(g_es_major, g_es_minor, g_depth_size,
                      g_stencil_size);
  SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
  if (egl_share_root)
    SDL_GL_MakeCurrent(egl_window, egl_share_root);
  c->sdl_context = SDL_GL_CreateContext(egl_window);
  SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
  SDL_GL_MakeCurrent(egl_window, NULL);
  pthread_mutex_unlock(&egl_context_create_mutex);

  if (!c->sdl_context) {
    debugPrintf("egl_shim: eglCreateContext(share=%p) FAILED: %s\n",
                share_context, SDL_GetError());
    free(c);
    return EGL_NO_CONTEXT;
  }

  c->id = next_context_id++;
  debugPrintf("egl_shim: eglCreateContext(share=%p) -> %p [ctx_id=%d]\n",
              share_context, c, c->id);
  return (EGLContext)c;
}

EGLBoolean egl_shim_MakeCurrent(EGLDisplay dpy, EGLSurface draw,
                                 EGLSurface read, EGLContext ctx) {
  (void)dpy;

  _egl_context *context = (_egl_context *)ctx;
  static _Thread_local int mc_count = 0;
  int mc = ++mc_count;

  /* === UNBIND === */
  if (context == NULL || draw == NULL) {
    current_context = NULL;
    if (!hc_pure_sdl_contexts() &&
        g_real_dpy && r_eglMakeCurrent && g_win_surf)
      r_eglMakeCurrent(g_real_dpy, NULL, NULL, NULL);  /* release real */
    else if (egl_window)
      SDL_GL_MakeCurrent(egl_window, NULL);
    has_real_gl = 0;
    return EGL_TRUE;
  }

  int is_window = (((char *)draw)[0] == 'w');
  context->is_pbuffer = is_window ? EGL_FALSE : EGL_TRUE;
  /*
   * EGL requires eglGetCurrentSurface to return the exact handle bound by
   * eglMakeCurrent. Unity compares handle identity before every Present; the
   * old string literal "window" had identical contents but a different
   * address from the strdup-backed surface, so Unity treated the context as
   * non-current and skipped eglSwapBuffers forever.
   */
  context->current_draw = draw;
  context->current_read = read;
  current_context = context;
  last_context = context;

  if (!egl_window || !context->sdl_context)
    return EGL_TRUE;

  /* Caminho EGL REAL (multi-contexto do Unity): o 1º contexto a pedir a WINDOW vira
     dono e usa a surface real da window (renderiza/swap); os outros recebem o pbuffer
     (worker de upload, recursos compartilhados) -> elimina o EGL_BAD_ACCESS de 2
     contextos na mesma surface. Usado quando temos display+surfaces reais. */
  if (!hc_pure_sdl_contexts() &&
      g_real_dpy && r_eglMakeCurrent && g_win_surf) {
    /* dono da window = a RENDER THREAD do Unity (GfxDeviceWorker), que faz o GL+present.
       A main thread (tid==pid) só submete comandos → recebe pbuffer (contexto próprio,
       recursos compartilhados). Sem isto a main pegava a window e o worker o pbuffer →
       worker não apresentava → livelock no nativeRender. CUP_MAINGL força a main na window. */
    int is_main = (syscall(SYS_gettid) == getpid()) && !getenv("CUP_MAINGL");
    pthread_mutex_lock(&g_owner_mtx2);
    if (is_window && !is_main && !g_have_owner) { g_have_owner = 1; g_owner_thread = pthread_self(); }
    int am_owner = !is_main && g_have_owner && pthread_equal(g_owner_thread, pthread_self());
    pthread_mutex_unlock(&g_owner_mtx2);

    void *use_ctx, *surf;
    if (am_owner) {
      use_ctx = (void *)context->sdl_context;   /* contexto real do jogo na window */
      surf = g_win_surf;
      tls_is_window = 1;
    } else {
      /* contexto real PRÓPRIO desta thread (compartilha recursos c/ o share-root) */
      if (!tls_real_ctx && r_eglCreateContext) {
        int ca[] = { 0x3098 /*EGL_CONTEXT_CLIENT_VERSION*/, g_es_major, 0x3038 };
        tls_real_ctx = r_eglCreateContext(g_real_dpy, g_real_cfg, egl_share_root, ca);
      }
      use_ctx = tls_real_ctx ? tls_real_ctx : (void *)context->sdl_context;
      surf = g_pbuf ? g_pbuf : g_win_surf;
      tls_is_window = 0;
    }
    unsigned ok = r_eglMakeCurrent(g_real_dpy, surf, surf, use_ctx);
    if (ok) {
      has_real_gl = 1;
      static _Thread_local int acq_log = 0;
      if (acq_log < 6) {
        debugPrintf("egl_shim: MakeCurrent #%d [tid=%lx] REAL OK [ctx_id=%d] %s\n",
                    mc, (unsigned long)pthread_self(), context->id,
                    am_owner ? "WIN(owner)" : "PBUF(worker)");
        acq_log++;
      }
      return EGL_TRUE;
    }
    has_real_gl = 0;
    debugPrintf("egl_shim: MakeCurrent #%d [tid=%lx] REAL FAILED [ctx_id=%d] %s\n",
                mc, (unsigned long)pthread_self(), context->id,
                am_owner ? "WIN" : "PBUF");
    return EGL_TRUE;
  }

  int ret = SDL_GL_MakeCurrent(egl_window, context->sdl_context);
  if (ret == 0) {
    has_real_gl = 1;
    if (hc_pure_sdl_contexts() && hc_env_on("HC_EGL_TRACE")) {
      static unsigned pure_logs;
      if (pure_logs++ < 12)
        fprintf(stderr,
                "egl_shim: MakeCurrent SDL puro tid=%ld ctx=%p %s\n",
                (long)syscall(SYS_gettid), context->sdl_context,
                is_window ? "WINDOW" : "PBUFFER");
    }
  } else {
    has_real_gl = 0;
    debugPrintf("egl_shim: MakeCurrent #%d %s [tid=%lx] SDL FAILED [ctx_id=%d]: %s\n",
                mc, is_window ? "WINDOW" : "PBUFFER",
                (unsigned long)pthread_self(), context->id, SDL_GetError());
  }

  return EGL_TRUE;
}

EGLBoolean egl_shim_SwapBuffers(EGLDisplay dpy, EGLSurface surface) {
  (void)dpy; (void)surface;
  if (!egl_window) return EGL_TRUE;

  if (has_real_gl && (tls_is_window || (current_context && !current_context->is_pbuffer))) {
    { extern void ter_shot_hook(void); ter_shot_hook(); }  /* captura na thread DONA da window (antes do swap) */
    SDL_GL_SwapWindow(egl_window);
    int fc = ++frame_count;
    if (fc <= 10 || fc % 60 == 0) {
      //debugPrintf("egl_shim: SwapBuffers #%d [tid=%lx]\n",
      //            fc, (unsigned long)pthread_self());
    }
  } else {
    static int noswap_log = 0;
    if (noswap_log < 3) {
      debugPrintf("egl_shim: SwapBuffers SKIPPED (no real GL) [tid=%lx]\n",
                  (unsigned long)pthread_self());
      noswap_log++;
    }
  }
  return EGL_TRUE;
}

EGLBoolean egl_shim_DestroySurface(EGLDisplay dpy, EGLSurface surface) {
  (void)dpy;
  free(surface);
  return EGL_TRUE;
}

EGLBoolean egl_shim_DestroyContext(EGLDisplay dpy, EGLContext ctx) {
  (void)dpy;
  _egl_context *context = (_egl_context *)ctx;
  if (context) {
    if (context->sdl_context)
      SDL_GL_DeleteContext(context->sdl_context);
    free(context);
  }
  return EGL_TRUE;
}

EGLBoolean egl_shim_QuerySurface(EGLDisplay dpy, EGLSurface surface,
                                  EGLint attribute, EGLint *value) {
  (void)dpy; (void)surface;
  if (attribute == 0x3057 && value) *value = SCREEN_WIDTH;
  else if (attribute == 0x3056 && value) *value = SCREEN_HEIGHT;
  return EGL_TRUE;
}

EGLBoolean egl_shim_GetConfigAttrib(EGLDisplay dpy, EGLConfig config,
                                     EGLint attribute, EGLint *value) {
  (void)dpy; (void)config;
  if (!value) return EGL_TRUE;
  /* A config real é a autoridade quando o backend SDL a expõe. */
  if (g_real_dpy && g_real_cfg && r_eglGetConfigAttrib &&
      r_eglGetConfigAttrib(g_real_dpy, g_real_cfg, attribute, value))
    return EGL_TRUE;
  switch (attribute) {
  case HC_EGL_BUFFER_SIZE:
    *value = HC_EGL_RGBA_CHANNEL_BITS * 4;
    break;
  case HC_EGL_ALPHA_SIZE:
  case HC_EGL_BLUE_SIZE:
  case HC_EGL_GREEN_SIZE:
  case HC_EGL_RED_SIZE:
    *value = HC_EGL_RGBA_CHANNEL_BITS;
    break;
  case HC_EGL_DEPTH_SIZE: *value = g_depth_size; break;
  case HC_EGL_STENCIL_SIZE: *value = g_stencil_size; break;
  case 0x3027: *value = 0x3038; break; /* EGL_CONFIG_CAVEAT = EGL_NONE */
  case 0x3028: *value = 1; break;   /* EGL_CONFIG_ID */
  case HC_EGL_NATIVE_VISUAL_TYPE: *value = 0; break;
  case HC_EGL_SAMPLES:
  case HC_EGL_SAMPLE_BUFFERS:
  case HC_EGL_COVERAGE_SAMPLES_NV:
    *value = 0;
    break;
  case HC_EGL_DEPTH_ENCODING_NV: *value = 0; break;
  case HC_EGL_SURFACE_TYPE:
    *value = HC_EGL_WINDOW_BIT | HC_EGL_PBUFFER_BIT;
    break;
  case HC_EGL_RENDERABLE_TYPE:
  case HC_EGL_CONFORMANT:
    *value = g_es_major >= 3
                 ? HC_EGL_OPENGL_ES3_BIT_KHR
                 : HC_EGL_OPENGL_ES2_BIT;
    break;
  case HC_EGL_COLOR_BUFFER_TYPE: *value = HC_EGL_RGB_BUFFER; break;
  default:
    debugPrintf("egl_shim: GetConfigAttrib(0x%x) -> 0 (default)\n", attribute);
    *value = 0; break;
  }
  return EGL_TRUE;
}

EGLint egl_shim_GetError(void) { return EGL_SUCCESS; }

/*
 * The KMS path replaces Unity's eglGetProcAddress with this SDL-backed shim.
 * Every resolved GL function must still pass through Horizon's lightweight
 * router so texture limits, compressed-format handling and GL state mirrors
 * are identical to the fbdev/dlsym path.
 */
extern void *hc_gl_route_proc(const char *name, void *real);

void *egl_shim_GetProcAddress(const char *procname) {
  void *ptr = SDL_GL_GetProcAddress(procname);
  if (ptr) return hc_gl_route_proc(procname, ptr);

  size_t len = strlen(procname);
  if (len > 3 && strcmp(procname + len - 3, "OES") == 0) {
    char stripped[256];
    if (len - 3 < sizeof(stripped)) {
      memcpy(stripped, procname, len - 3);
      stripped[len - 3] = '\0';
      ptr = SDL_GL_GetProcAddress(stripped);
      if (ptr) return hc_gl_route_proc(procname, ptr);
    }
  }

  debugPrintf("egl_shim: eglGetProcAddress(%s) -> NOT FOUND\n", procname);
  return NULL;
}

EGLBoolean egl_shim_BindAPI(unsigned int api) {
  (void)api;
  return EGL_TRUE;
}

const char *egl_shim_QueryString(EGLDisplay dpy, EGLint name) {
  (void)dpy;
  switch (name) {
  case 0x3053: return "Horizon Chase so-loader"; /* EGL_VENDOR */
  case 0x3054: return "1.4";                    /* EGL_VERSION */
  case 0x3055: return "";            /* EGL_EXTENSIONS */
  case 0x308D: return "OpenGL_ES";   /* EGL_CLIENT_APIS */
  default: return "";
  }
}

EGLBoolean egl_shim_SwapInterval(EGLDisplay dpy, EGLint interval) {
  (void)dpy;
  SDL_GL_SetSwapInterval(interval);
  return EGL_TRUE;
}

EGLContext egl_shim_GetCurrentContext(void) {
  return (EGLContext)current_context;
}

EGLSurface egl_shim_GetCurrentSurface(EGLint readdraw) {
  if (!current_context)
    return EGL_NO_SURFACE;
  return readdraw == 0x305A /* EGL_READ */
             ? current_context->current_read
             : current_context->current_draw;
}

EGLBoolean egl_shim_SurfaceAttrib(EGLDisplay dpy, EGLSurface s, EGLint a,
                                  EGLint v) {
  (void)dpy; (void)s; (void)a; (void)v;
  return EGL_TRUE;
}
