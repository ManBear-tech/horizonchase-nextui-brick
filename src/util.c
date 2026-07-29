/*
 * util.c -- misc utility functions
 *
 * Based on max_arm64 by Jaakko Lukkari / fgsfds / Andy Nguyen
 * Adapted for Syberia ARM64 port
 */

#include <stdarg.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

#define LOG_NAME "debug.log"
#define HC_FALLBACK_GAMEDIR "/storage/roms/ports/horizonchase"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static pthread_once_t g_hc_game_dir_once = PTHREAD_ONCE_INIT;
static char g_hc_game_dir[PATH_MAX];

static void hc_init_game_dir(void) {
  const char *configured = getenv("HC_GAMEDIR");
  if (configured && configured[0] == '/') {
    snprintf(g_hc_game_dir, sizeof g_hc_game_dir, "%s", configured);
  } else if (!getcwd(g_hc_game_dir, sizeof g_hc_game_dir)) {
    snprintf(g_hc_game_dir, sizeof g_hc_game_dir, "%s",
             HC_FALLBACK_GAMEDIR);
  }

  size_t length = strlen(g_hc_game_dir);
  while (length > 1 && g_hc_game_dir[length - 1] == '/')
    g_hc_game_dir[--length] = '\0';
}

const char *hc_game_dir(void) {
  pthread_once(&g_hc_game_dir_once, hc_init_game_dir);
  return g_hc_game_dir;
}

int hc_game_path(char *dst, size_t dst_size, const char *relative) {
  if (!dst || dst_size == 0) {
    errno = EINVAL;
    return -1;
  }

  const char *root = hc_game_dir();
  while (relative && *relative == '/') relative++;
  int written = (relative && *relative)
                    ? snprintf(dst, dst_size, "%s/%s", root, relative)
                    : snprintf(dst, dst_size, "%s", root);
  if (written < 0 || (size_t)written >= dst_size) {
    dst[0] = '\0';
    errno = ENAMETOOLONG;
    return -1;
  }
  return 0;
}

int debugPrintf(const char *text, ...) {
  static int enabled = -1;
  if (enabled < 0)
    enabled = getenv("HC_VERBOSE") ? 1 : 0;
  if (!enabled)
    return 0;

  va_list list;

  FILE *f = fopen(LOG_NAME, "a");
  if (f) {
    va_start(list, text);
    vfprintf(f, text, list);
    va_end(list);
    fclose(f);
  }

  va_start(list, text);
  vprintf(text, list);
  va_end(list);

  return 0;
}

int ret0(void) { return 0; }
int ret1(void) { return 1; }
int retm1(void) { return -1; }
