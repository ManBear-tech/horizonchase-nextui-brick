/*
 * SPDX-License-Identifier: MIT
 *
 * nxextract-ui: backend-neutral first-run screen for NXEXTRACT_V1.
 *
 * SDL2 is loaded at runtime so the same binary follows the firmware's own video
 * backend. The extractor is a separate process: if this UI cannot open a visible
 * renderer, extraction continues headless and remains transactional.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct SDL_Window SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Rect {
  int x, y, w, h;
} SDL_Rect;
typedef union SDL_Event {
  uint32_t type;
  unsigned char padding[256];
} SDL_Event;

enum {
  SDL_INIT_VIDEO = 0x00000020u,
  SDL_WINDOW_FULLSCREEN_DESKTOP = 0x00001001u,
  SDL_RENDERER_SOFTWARE = 0x00000001u,
  SDL_RENDERER_ACCELERATED = 0x00000002u,
  SDL_RENDERER_PRESENTVSYNC = 0x00000004u,
  SDL_QUIT_EVENT = 0x00000100u
};

static int (*pSDL_Init)(uint32_t);
static void (*pSDL_Quit)(void);
static const char *(*pSDL_GetError)(void);
static const char *(*pSDL_GetCurrentVideoDriver)(void);
static int (*pSDL_GetNumVideoDrivers)(void);
static const char *(*pSDL_GetVideoDriver)(int);
static SDL_Window *(*pSDL_CreateWindow)(const char *, int, int, int, int,
                                        uint32_t);
static void (*pSDL_DestroyWindow)(SDL_Window *);
static SDL_Renderer *(*pSDL_CreateRenderer)(SDL_Window *, int, uint32_t);
static void (*pSDL_DestroyRenderer)(SDL_Renderer *);
static int (*pSDL_GetRendererOutputSize)(SDL_Renderer *, int *, int *);
static int (*pSDL_SetRenderDrawColor)(SDL_Renderer *, uint8_t, uint8_t, uint8_t,
                                      uint8_t);
static int (*pSDL_RenderClear)(SDL_Renderer *);
static int (*pSDL_RenderFillRect)(SDL_Renderer *, const SDL_Rect *);
static int (*pSDL_RenderDrawRect)(SDL_Renderer *, const SDL_Rect *);
static void (*pSDL_RenderPresent)(SDL_Renderer *);
static int (*pSDL_PollEvent)(SDL_Event *);
static void (*pSDL_Delay)(uint32_t);

static void *sdl_handle;

#define SDL_LOAD(symbol)                                                       \
  do {                                                                         \
    *(void **)(&p##symbol) = dlsym(sdl_handle, #symbol);                       \
    if (!p##symbol) {                                                          \
      fprintf(stderr, "nxextract-ui: missing %s\n", #symbol);                  \
      return 0;                                                                \
    }                                                                          \
  } while (0)

static int load_sdl(void) {
  static const char *names[] = {
      "libSDL2-2.0.so.0", "libSDL2.so.0", "libSDL2.so", NULL,
  };
  int index;
  for (index = 0; names[index] && !sdl_handle; index++)
    sdl_handle = dlopen(names[index], RTLD_NOW | RTLD_LOCAL);
  if (!sdl_handle) {
    fprintf(stderr, "nxextract-ui: SDL2 unavailable: %s\n", dlerror());
    return 0;
  }
  SDL_LOAD(SDL_Init);
  SDL_LOAD(SDL_Quit);
  SDL_LOAD(SDL_GetError);
  SDL_LOAD(SDL_GetCurrentVideoDriver);
  SDL_LOAD(SDL_GetNumVideoDrivers);
  SDL_LOAD(SDL_GetVideoDriver);
  SDL_LOAD(SDL_CreateWindow);
  SDL_LOAD(SDL_DestroyWindow);
  SDL_LOAD(SDL_CreateRenderer);
  SDL_LOAD(SDL_DestroyRenderer);
  SDL_LOAD(SDL_GetRendererOutputSize);
  SDL_LOAD(SDL_SetRenderDrawColor);
  SDL_LOAD(SDL_RenderClear);
  SDL_LOAD(SDL_RenderFillRect);
  SDL_LOAD(SDL_RenderDrawRect);
  SDL_LOAD(SDL_RenderPresent);
  SDL_LOAD(SDL_PollEvent);
  SDL_LOAD(SDL_Delay);
  return 1;
}

#undef SDL_LOAD

typedef struct ProgressState {
  int state;
  int active;
  int phase;
  int overall;
  int phase_progress;
  uint64_t done_bytes;
  uint64_t total_bytes;
  char message[256];
  char detail[256];
} ProgressState;

typedef struct Color {
  uint8_t r, g, b, a;
} Color;

static const char *phase_labels[] = {
    "PREPARING",       "SCANNING FILES",  "VALIDATING PACKAGES",
    "SELECTING DATA",  "EXTRACTING DATA", "PROCESSING DATA",
    "VALIDATING DATA", "INSTALLING DATA", "READY",
};

static int path_exists(const char *path) {
  struct stat value;
  return path && stat(path, &value) == 0;
}

static int directory_usable(const char *path) {
  struct stat value;
  if (!path || !*path || stat(path, &value) != 0 || !S_ISDIR(value.st_mode))
    return 0;
  return access(path, R_OK | W_OK | X_OK) == 0;
}

static int wayland_socket_exists(void) {
  const char *runtime = getenv("XDG_RUNTIME_DIR");
  const char *display = getenv("WAYLAND_DISPLAY");
  char path[512];
  struct stat value;
  if (!runtime || !*runtime || !display || !*display || strchr(display, '/'))
    return 0;
  if (snprintf(path, sizeof(path), "%s/%s", runtime, display) >=
      (int)sizeof(path))
    return 0;
  return stat(path, &value) == 0 && S_ISSOCK(value.st_mode);
}

/*
 * Reuse the session directories already created by the firmware. This is the
 * proven Sonic/Dysmantle fallback: it never starts a compositor and never
 * chooses a video backend. It only makes an existing Wayland socket visible.
 */
static void configure_session_runtime(void) {
  char run_user[64], var_run_user[64], run_legacy[64];
  const char *runtime = getenv("XDG_RUNTIME_DIR");
  const char *candidates[7];
  int index;

  snprintf(run_user, sizeof(run_user), "/run/user/%u", (unsigned)getuid());
  snprintf(var_run_user, sizeof(var_run_user), "/var/run/user/%u",
           (unsigned)getuid());
  snprintf(run_legacy, sizeof(run_legacy), "/run/%u-runtime-dir",
           (unsigned)getuid());
  candidates[0] = run_legacy;
  candidates[1] = run_user;
  candidates[2] = var_run_user;
  candidates[3] = "/run/0-runtime-dir";
  candidates[4] = "/var/run/0-runtime-dir";
  candidates[5] = NULL;

  if (!directory_usable(runtime)) {
    for (index = 0; candidates[index]; index++) {
      if (!directory_usable(candidates[index]))
        continue;
      if (setenv("XDG_RUNTIME_DIR", candidates[index], 1) == 0) {
        runtime = getenv("XDG_RUNTIME_DIR");
        fprintf(stderr, "nxextract-ui: session runtime=%s\n", runtime);
      }
      break;
    }
  }

  if (!wayland_socket_exists() && runtime && *runtime) {
    DIR *directory = opendir(runtime);
    struct dirent *entry;
    if (!directory)
      return;
    while ((entry = readdir(directory)) != NULL) {
      char path[512];
      struct stat value;
      if (strncmp(entry->d_name, "wayland-", 8) != 0 ||
          strstr(entry->d_name, ".lock") != NULL)
        continue;
      if (snprintf(path, sizeof(path), "%s/%s", runtime, entry->d_name) >=
          (int)sizeof(path))
        continue;
      if (stat(path, &value) != 0 || !S_ISSOCK(value.st_mode))
        continue;
      if (setenv("WAYLAND_DISPLAY", entry->d_name, 1) == 0)
        fprintf(stderr, "nxextract-ui: wayland socket=%s\n", entry->d_name);
      break;
    }
    closedir(directory);
  }
}

static int clamp_permille(int value) {
  if (value < 0)
    return 0;
  if (value > 1000)
    return 1000;
  return value;
}

static int read_progress(const char *path, ProgressState *state) {
  FILE *stream;
  ProgressState next = *state;
  char line[512];
  unsigned long long done = 0, total = 0;
  int legacy_total = 1000;

  stream = fopen(path, "r");
  if (!stream)
    return 0;
  if (!fgets(line, sizeof(line), stream) ||
      sscanf(line, "%d %d %d", &next.state, &next.overall, &legacy_total) != 3) {
    fclose(stream);
    return 0;
  }
  if (!fgets(next.message, sizeof(next.message), stream)) {
    fclose(stream);
    return 0;
  }
  next.message[strcspn(next.message, "\r\n")] = 0;
  if (!fgets(line, sizeof(line), stream) ||
      sscanf(line, "NXEXTRACT_V1 %d %d %d %llu %llu", &next.phase,
             &next.overall, &next.phase_progress, &done, &total) != 5) {
    next.phase = next.state == 3 ? 8 : 0;
    next.phase_progress =
        legacy_total > 0 ? next.overall * 1000 / legacy_total : 0;
    next.overall = next.phase_progress;
  }
  if (fgets(next.detail, sizeof(next.detail), stream))
    next.detail[strcspn(next.detail, "\r\n")] = 0;
  else
    next.detail[0] = 0;
  fclose(stream);

  next.state = next.state < 1 || next.state > 3 ? 1 : next.state;
  next.phase = next.phase < 0 || next.phase > 8 ? 0 : next.phase;
  next.overall = clamp_permille(next.overall);
  next.phase_progress = clamp_permille(next.phase_progress);
  next.done_bytes = (uint64_t)done;
  next.total_bytes = (uint64_t)total;
  next.active = next.state == 1 && next.phase_progress < 1000;
  *state = next;
  return 1;
}

static const char **glyph5x7(unsigned codepoint) {
  static const char *blank[] = {"00000", "00000", "00000", "00000",
                                "00000", "00000", "00000"};
  static const char *question[] = {"01110", "10001", "00001", "00010",
                                   "00100", "00000", "00100"};
  static const char *dot[] = {"00000", "00000", "00000", "00000",
                              "00000", "01100", "01100"};
  static const char *comma[] = {"00000", "00000", "00000", "00000",
                                "01100", "01100", "01000"};
  static const char *dash[] = {"00000", "00000", "00000", "11111",
                               "00000", "00000", "00000"};
  static const char *underscore[] = {"00000", "00000", "00000", "00000",
                                     "00000", "00000", "11111"};
  static const char *slash[] = {"00001", "00010", "00010", "00100",
                                "01000", "01000", "10000"};
  static const char *colon[] = {"00000", "01100", "01100", "00000",
                                "01100", "01100", "00000"};
  static const char *pct[] = {"11001", "11010", "00100", "01000",
                              "10110", "00110", "00000"};
  static const char *plus[] = {"00000", "00100", "00100", "11111",
                               "00100", "00100", "00000"};
  static const char *left[] = {"00010", "00100", "01000", "01000",
                               "01000", "00100", "00010"};
  static const char *right[] = {"01000", "00100", "00010", "00010",
                                "00010", "00100", "01000"};
  static const char *pipe[] = {"00100", "00100", "00100", "00100",
                               "00100", "00100", "00100"};
  static const char *n0[] = {"01110", "10001", "10011", "10101",
                             "11001", "10001", "01110"};
  static const char *n1[] = {"00100", "01100", "00100", "00100",
                             "00100", "00100", "01110"};
  static const char *n2[] = {"01110", "10001", "00001", "00010",
                             "00100", "01000", "11111"};
  static const char *n3[] = {"11110", "00001", "00001", "01110",
                             "00001", "00001", "11110"};
  static const char *n4[] = {"00010", "00110", "01010", "10010",
                             "11111", "00010", "00010"};
  static const char *n5[] = {"11111", "10000", "10000", "11110",
                             "00001", "00001", "11110"};
  static const char *n6[] = {"00110", "01000", "10000", "11110",
                             "10001", "10001", "01110"};
  static const char *n7[] = {"11111", "00001", "00010", "00100",
                             "01000", "01000", "01000"};
  static const char *n8[] = {"01110", "10001", "10001", "01110",
                             "10001", "10001", "01110"};
  static const char *n9[] = {"01110", "10001", "10001", "01111",
                             "00001", "00010", "01100"};
  static const char *letters[][7] = {
      {"01110", "10001", "10001", "11111", "10001", "10001", "10001"},
      {"11110", "10001", "10001", "11110", "10001", "10001", "11110"},
      {"01110", "10001", "10000", "10000", "10000", "10001", "01110"},
      {"11110", "10001", "10001", "10001", "10001", "10001", "11110"},
      {"11111", "10000", "10000", "11110", "10000", "10000", "11111"},
      {"11111", "10000", "10000", "11110", "10000", "10000", "10000"},
      {"01110", "10001", "10000", "10111", "10001", "10001", "01110"},
      {"10001", "10001", "10001", "11111", "10001", "10001", "10001"},
      {"01110", "00100", "00100", "00100", "00100", "00100", "01110"},
      {"00001", "00001", "00001", "00001", "10001", "10001", "01110"},
      {"10001", "10010", "10100", "11000", "10100", "10010", "10001"},
      {"10000", "10000", "10000", "10000", "10000", "10000", "11111"},
      {"10001", "11011", "10101", "10101", "10001", "10001", "10001"},
      {"10001", "11001", "10101", "10011", "10001", "10001", "10001"},
      {"01110", "10001", "10001", "10001", "10001", "10001", "01110"},
      {"11110", "10001", "10001", "11110", "10000", "10000", "10000"},
      {"01110", "10001", "10001", "10001", "10101", "10010", "01101"},
      {"11110", "10001", "10001", "11110", "10100", "10010", "10001"},
      {"01111", "10000", "10000", "01110", "00001", "00001", "11110"},
      {"11111", "00100", "00100", "00100", "00100", "00100", "00100"},
      {"10001", "10001", "10001", "10001", "10001", "10001", "01110"},
      {"10001", "10001", "10001", "10001", "10001", "01010", "00100"},
      {"10001", "10001", "10001", "10101", "10101", "10101", "01010"},
      {"10001", "10001", "01010", "00100", "01010", "10001", "10001"},
      {"10001", "10001", "01010", "00100", "00100", "00100", "00100"},
      {"11111", "00001", "00010", "00100", "01000", "10000", "11111"},
  };
  static const char **numbers[] = {n0, n1, n2, n3, n4, n5, n6, n7, n8, n9};

  if (codepoint >= 'a' && codepoint <= 'z')
    codepoint = (unsigned)toupper((int)codepoint);
  if (codepoint >= 'A' && codepoint <= 'Z')
    return letters[codepoint - 'A'];
  if (codepoint >= '0' && codepoint <= '9')
    return numbers[codepoint - '0'];
  switch (codepoint) {
  case ' ': return blank;
  case '.': return dot;
  case ',': return comma;
  case '-': return dash;
  case '_': return underscore;
  case '/': return slash;
  case ':': return colon;
  case '%': return pct;
  case '+': return plus;
  case '(': return left;
  case ')': return right;
  case '|': return pipe;
  default: return question;
  }
}

static unsigned next_codepoint(const unsigned char **cursor) {
  const unsigned char *value = *cursor;
  unsigned codepoint;
  if (*value < 0x80) {
    *cursor = value + 1;
    return *value;
  }
  if ((*value & 0xE0) == 0xC0 && value[1]) {
    codepoint = ((unsigned)(value[0] & 0x1F) << 6) | (value[1] & 0x3F);
    *cursor = value + 2;
  } else if ((*value & 0xF0) == 0xE0 && value[1] && value[2]) {
    codepoint = ((unsigned)(value[0] & 0x0F) << 12) |
                ((unsigned)(value[1] & 0x3F) << 6) | (value[2] & 0x3F);
    *cursor = value + 3;
  } else {
    *cursor = value + 1;
    return '?';
  }
  switch (codepoint) {
  case 0x00C1: case 0x00C0: case 0x00C2: case 0x00C3: case 0x00C4:
  case 0x00E1: case 0x00E0: case 0x00E2: case 0x00E3: case 0x00E4:
    return 'A';
  case 0x00C9: case 0x00C8: case 0x00CA: case 0x00CB:
  case 0x00E9: case 0x00E8: case 0x00EA: case 0x00EB:
    return 'E';
  case 0x00CD: case 0x00CC: case 0x00CE: case 0x00CF:
  case 0x00ED: case 0x00EC: case 0x00EE: case 0x00EF:
    return 'I';
  case 0x00D3: case 0x00D2: case 0x00D4: case 0x00D5: case 0x00D6:
  case 0x00F3: case 0x00F2: case 0x00F4: case 0x00F5: case 0x00F6:
    return 'O';
  case 0x00DA: case 0x00D9: case 0x00DB: case 0x00DC:
  case 0x00FA: case 0x00F9: case 0x00FB: case 0x00FC:
    return 'U';
  case 0x00C7: case 0x00E7:
    return 'C';
  default:
    return codepoint < 128 ? codepoint : '?';
  }
}

static int text_width(const char *text, int scale) {
  const unsigned char *cursor = (const unsigned char *)text;
  int count = 0;
  while (cursor && *cursor) {
    (void)next_codepoint(&cursor);
    count++;
  }
  return count ? count * 6 * scale - scale : 0;
}

static void set_color(SDL_Renderer *renderer, Color color) {
  pSDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
}

static void fill(SDL_Renderer *renderer, int x, int y, int width, int height,
                 Color color) {
  SDL_Rect rect = {x, y, width, height};
  if (width <= 0 || height <= 0)
    return;
  set_color(renderer, color);
  pSDL_RenderFillRect(renderer, &rect);
}

static void outline(SDL_Renderer *renderer, int x, int y, int width, int height,
                    Color color) {
  SDL_Rect rect = {x, y, width, height};
  set_color(renderer, color);
  pSDL_RenderDrawRect(renderer, &rect);
}

static void draw_text(SDL_Renderer *renderer, int x, int y, int scale,
                      const char *text, Color color) {
  SDL_Rect pixel = {0, 0, scale, scale};
  const unsigned char *cursor = (const unsigned char *)text;
  set_color(renderer, color);
  while (cursor && *cursor) {
    unsigned codepoint = next_codepoint(&cursor);
    const char **glyph = glyph5x7(codepoint);
    int yy, xx;
    for (yy = 0; yy < 7; yy++) {
      for (xx = 0; xx < 5; xx++) {
        if (glyph[yy][xx] == '1') {
          pixel.x = x + xx * scale;
          pixel.y = y + yy * scale;
          pSDL_RenderFillRect(renderer, &pixel);
        }
      }
    }
    x += 6 * scale;
  }
}

static void draw_text_centered(SDL_Renderer *renderer, int center, int y,
                               int scale, const char *text, Color color) {
  draw_text(renderer, center - text_width(text, scale) / 2, y, scale, text,
            color);
}

static void ellipsize(char *text, size_t capacity, int max_chars) {
  size_t length = strlen(text);
  if (max_chars < 1) {
    text[0] = 0;
    return;
  }
  if ((size_t)max_chars >= length)
    return;
  if ((size_t)max_chars >= capacity)
    max_chars = (int)capacity - 1;
  if (max_chars <= 3) {
    memset(text, '.', (size_t)max_chars);
    text[max_chars] = 0;
    return;
  }
  memcpy(text + max_chars - 3, "...", 4);
}

static int wrap_text(const char *input, char lines[][128], int max_lines,
                     int max_chars) {
  const char *cursor = input ? input : "";
  int count = 0;
  if (max_chars < 8)
    max_chars = 8;
  if (max_chars > 127)
    max_chars = 127;
  while (*cursor && count < max_lines) {
    int used = 0;
    while (*cursor == ' ' || *cursor == '\t')
      cursor++;
    while (*cursor && used < max_chars) {
      const char *word = cursor;
      int length = 0;
      while (word[length] && word[length] != ' ' && word[length] != '\t')
        length++;
      if (used && used + 1 + length > max_chars)
        break;
      if (!used && length > max_chars)
        length = max_chars;
      if (used)
        lines[count][used++] = ' ';
      memcpy(lines[count] + used, word, (size_t)length);
      used += length;
      lines[count][used] = 0;
      cursor += length;
      while (*cursor == ' ' || *cursor == '\t')
        cursor++;
      if (length == max_chars)
        break;
    }
    count++;
  }
  if (*cursor && count) {
    int used = (int)strlen(lines[count - 1]);
    if (used > max_chars - 3)
      used = max_chars - 3;
    memcpy(lines[count - 1] + used, "...", 4);
  }
  if (!count) {
    lines[0][0] = 0;
    count = 1;
  }
  return count;
}

static void draw_bar(SDL_Renderer *renderer, int x, int y, int width,
                     int height, int value, Color accent, int active,
                     unsigned tick) {
  Color track = {29, 35, 49, 255};
  Color border = {77, 88, 110, 255};
  int inset = height < 14 ? 2 : 3;
  int inner = width - inset * 2;
  int fill_width;
  value = clamp_permille(value);
  fill(renderer, x, y, width, height, track);
  outline(renderer, x, y, width, height, border);
  fill_width = inner * value / 1000;
  fill(renderer, x + inset, y + inset, fill_width, height - inset * 2, accent);
  if (active && value < 1000) {
    int remaining = inner - fill_width;
    int segment = remaining / 5;
    int travel, offset, left, right;
    Color light = {(uint8_t)(accent.r + (255 - accent.r) / 2),
                   (uint8_t)(accent.g + (255 - accent.g) / 2),
                   (uint8_t)(accent.b + (255 - accent.b) / 2), 255};
    if (segment < 8)
      segment = remaining < 8 ? remaining : 8;
    if (segment > 48)
      segment = 48;
    travel = remaining + segment;
    offset = (int)((tick / 4u) % (unsigned)(travel ? travel : 1)) - segment;
    left = x + inset + fill_width + offset;
    right = left + segment;
    if (left < x + inset + fill_width)
      left = x + inset + fill_width;
    if (right > x + inset + inner)
      right = x + inset + inner;
    fill(renderer, left, y + inset, right - left, height - inset * 2, light);
  }
}

static void draw_screen(SDL_Renderer *renderer, int width, int height,
                        const ProgressState *progress, const char *title,
                        const char *version, unsigned tick) {
  Color background_top = {11, 16, 29, 255};
  Color background_bottom = {24, 31, 48, 255};
  Color panel = {19, 25, 40, 255};
  Color panel_border = {55, 66, 90, 255};
  Color primary = {232, 238, 247, 255};
  Color secondary = {145, 158, 181, 255};
  Color accent = {55, 210, 154, 255};
  Color blue = {69, 156, 235, 255};
  Color error = {232, 83, 91, 255};
  Color success = {87, 218, 137, 255};
  Color dim = {45, 55, 74, 255};
  int stripe, margin, panel_x, panel_y, panel_w, panel_h;
  int scale_base, title_scale, version_scale, small_scale, message_scale;
  int content_x, content_w, title_y, step_y, phase_y, message_y;
  int bar_y, bar_height, overall_y, max_chars, line_count, line;
  int header_gap, title_limit, version_limit, version_width;
  int segment_gap, segment_width, index;
  char shown_title[128], version_text[80], phase[64];
  char message_lines[2][128], shown_detail[256];
  Color state_accent;

  for (stripe = 0; stripe < 32; stripe++) {
    int y0 = height * stripe / 32;
    int y1 = height * (stripe + 1) / 32;
    Color color = {
        (uint8_t)(background_top.r +
                  (background_bottom.r - background_top.r) * stripe / 31),
        (uint8_t)(background_top.g +
                  (background_bottom.g - background_top.g) * stripe / 31),
        (uint8_t)(background_top.b +
                  (background_bottom.b - background_top.b) * stripe / 31),
        255};
    fill(renderer, 0, y0, width, y1 - y0, color);
  }
  fill(renderer, 0, 0, width, height / 90 + 2,
       progress->state == 2 ? error : accent);

  margin = width / 18;
  if (margin < 10)
    margin = 10;
  if (margin > 56)
    margin = 56;
  panel_x = margin;
  panel_y = height / 14;
  panel_w = width - margin * 2;
  panel_h = height - panel_y - height / 16;
  fill(renderer, panel_x + 4, panel_y + 5, panel_w, panel_h,
       (Color){6, 9, 17, 120});
  fill(renderer, panel_x, panel_y, panel_w, panel_h, panel);
  outline(renderer, panel_x, panel_y, panel_w, panel_h, panel_border);

  content_x = panel_x + panel_w / 16;
  content_w = panel_w - panel_w / 8;
  scale_base = height < 300 ? 1 : (height < 700 ? 2 : 3);
  title_scale = scale_base + (height >= 440 ? 1 : 0);
  small_scale = scale_base;
  message_scale = scale_base;

  version_text[0] = 0;
  version_scale = small_scale;
  version_width = 0;
  if (version && *version) {
    snprintf(version_text, sizeof(version_text), "RECIPE %s", version);
    version_limit = content_w * 38 / 100;
    while (version_scale > 1 &&
           text_width(version_text, version_scale) > version_limit)
      version_scale--;
    if (text_width(version_text, version_scale) > version_limit)
      ellipsize(version_text, sizeof(version_text),
                version_limit / (6 * version_scale));
    version_width = text_width(version_text, version_scale);
  }

  header_gap = version_width ? small_scale * 8 : 0;
  title_limit = content_w - version_width - header_gap;
  if (title_limit < content_w / 3)
    title_limit = content_w / 3;
  snprintf(shown_title, sizeof(shown_title), "%s", title);
  while (title_scale > 1 &&
         text_width(shown_title, title_scale) > title_limit)
    title_scale--;
  if (text_width(shown_title, title_scale) > title_limit)
    ellipsize(shown_title, sizeof(shown_title),
              (title_limit + title_scale) / (6 * title_scale));

  title_y = panel_y + panel_h / 12;
  draw_text(renderer, content_x, title_y, title_scale, shown_title, primary);
  if (version_text[0]) {
    draw_text(renderer,
              content_x + content_w - version_width,
              title_y + (title_scale - version_scale) * 3, version_scale,
              version_text, secondary);
  }

  step_y = title_y + title_scale * 7 + panel_h / 13;
  segment_gap = content_w / 160;
  if (segment_gap < 3)
    segment_gap = 3;
  segment_width = (content_w - segment_gap * 8) / 9;
  for (index = 0; index < 9; index++) {
    Color color = index < progress->phase
                      ? accent
                      : index == progress->phase
                            ? (progress->state == 2 ? error : blue)
                            : dim;
    fill(renderer, content_x + index * (segment_width + segment_gap), step_y,
         segment_width, height < 300 ? 4 : 7, color);
  }

  snprintf(phase, sizeof(phase), "%s",
           progress->state == 2 ? "SETUP FAILED"
                                : phase_labels[progress->phase]);
  phase_y = step_y + (height < 300 ? 13 : 22);
  state_accent =
      progress->state == 2 ? error : (progress->state == 3 ? success : blue);
  draw_text(renderer, content_x, phase_y, small_scale, phase, state_accent);

  max_chars = content_w / (6 * message_scale);
  line_count =
      wrap_text(progress->message, message_lines, height < 260 ? 1 : 2, max_chars);
  message_y = phase_y + small_scale * 7 + panel_h / 18;
  for (line = 0; line < line_count; line++)
    draw_text(renderer, content_x, message_y + line * message_scale * 10,
              message_scale, message_lines[line], primary);

  bar_height = height < 300 ? 13 : (height < 700 ? 20 : 28);
  bar_y = panel_y + panel_h * 65 / 100;
  draw_bar(renderer, content_x, bar_y, content_w, bar_height,
           progress->phase_progress, state_accent, progress->active, tick);

  if (progress->detail[0]) {
    snprintf(shown_detail, sizeof(shown_detail), "%s", progress->detail);
    ellipsize(shown_detail, sizeof(shown_detail),
              content_w / (6 * small_scale));
    draw_text(renderer, content_x, bar_y + bar_height + small_scale * 5,
              small_scale, shown_detail, secondary);
  }

  overall_y = panel_y + panel_h * 86 / 100;
  draw_text(renderer, content_x, overall_y - small_scale * 10, small_scale,
            "OVERALL", secondary);
  draw_bar(renderer, content_x, overall_y, content_w, height < 300 ? 7 : 10,
           progress->overall, accent, 0, tick);
  {
    char percent[32];
    snprintf(percent, sizeof(percent), "%d%%", progress->overall / 10);
    draw_text(renderer, content_x + content_w - text_width(percent, small_scale),
              overall_y - small_scale * 10, small_scale, percent, secondary);
  }
  draw_text_centered(renderer, width / 2,
                     panel_y + panel_h - small_scale * 12, small_scale,
                     "NXEXTRACT UNIVERSAL DATA SETUP", secondary);
}

enum {
  MAX_VIDEO_CANDIDATES = 24,
  MAX_VIDEO_DRIVER_NAME = 64,
  FALLBACK_RETRIES_PER_DRIVER = 6
};

typedef struct VideoCandidates {
  char names[MAX_VIDEO_CANDIDATES][MAX_VIDEO_DRIVER_NAME];
  int count;
  int next;
  int exhausted;
} VideoCandidates;

static int driver_is_invisible(const char *name) {
  return name &&
         (!strcasecmp(name, "dummy") || !strcasecmp(name, "offscreen") ||
          !strcasecmp(name, "evdev"));
}

static int driver_is_plausible(const char *name) {
  if (!name || !*name || driver_is_invisible(name))
    return 0;
  if (!strcasecmp(name, "wayland"))
    return wayland_socket_exists();
  if (!strcasecmp(name, "x11"))
    return getenv("DISPLAY") && *getenv("DISPLAY");
  if (!strcasecmp(name, "kmsdrm"))
    return path_exists("/dev/dri/card0");
  if (!strcasecmp(name, "mali") || !strcasecmp(name, "fbcon") ||
      !strcasecmp(name, "directfb"))
    return path_exists("/dev/fb0");
  return 1;
}

static void collect_video_candidates(VideoCandidates *candidates) {
  int count = pSDL_GetNumVideoDrivers();
  int index;
  memset(candidates, 0, sizeof(*candidates));
  for (index = 0; index < count &&
                  candidates->count < MAX_VIDEO_CANDIDATES;
       index++) {
    const char *name = pSDL_GetVideoDriver(index);
    int duplicate = 0;
    int previous;
    if (!driver_is_plausible(name))
      continue;
    for (previous = 0; previous < candidates->count; previous++) {
      if (!strcasecmp(candidates->names[previous], name)) {
        duplicate = 1;
        break;
      }
    }
    if (duplicate)
      continue;
    snprintf(candidates->names[candidates->count],
             sizeof(candidates->names[candidates->count]), "%s", name);
    candidates->count++;
  }
}

static int select_next_video_candidate(VideoCandidates *candidates,
                                       int *fallback_attempts) {
  while (candidates->next < candidates->count) {
    const char *name = candidates->names[candidates->next++];
    if (setenv("SDL_VIDEODRIVER", name, 1) != 0)
      continue;
    *fallback_attempts = 0;
    fprintf(stderr,
            "nxextract-ui: automatic SDL selection was invisible; "
            "trying advertised backend=%s\n",
            name);
    return 1;
  }
  candidates->exhausted = 1;
  unsetenv("SDL_VIDEODRIVER");
  return 0;
}

static void note_video_failure(int inherited_mode, int *fallback_mode,
                               int *fallback_attempts,
                               VideoCandidates *candidates) {
  if (inherited_mode || candidates->exhausted)
    return;
  if (!*fallback_mode) {
    *fallback_mode =
        select_next_video_candidate(candidates, fallback_attempts);
    return;
  }
  (*fallback_attempts)++;
  if (*fallback_attempts >= FALLBACK_RETRIES_PER_DRIVER)
    *fallback_mode =
        select_next_video_candidate(candidates, fallback_attempts);
}

static int create_renderer(const char *stop_path, const char *title,
                           SDL_Window **window_out,
                           SDL_Renderer **renderer_out) {
  SDL_Window *window = NULL;
  SDL_Renderer *renderer = NULL;
  VideoCandidates candidates;
  int inherited_mode =
      getenv("SDL_VIDEODRIVER") && *getenv("SDL_VIDEODRIVER");
  int fallback_mode = 0;
  int fallback_attempts = 0;
  int attempt;

  collect_video_candidates(&candidates);
  for (attempt = 0; attempt < 30 && !renderer; attempt++) {
    const char *driver;
    if (path_exists(stop_path))
      return 0;
    if (attempt == 6 && inherited_mode) {
      fprintf(stderr,
              "nxextract-ui: inherited SDL_VIDEODRIVER=%s failed; retrying auto\n",
              getenv("SDL_VIDEODRIVER"));
      unsetenv("SDL_VIDEODRIVER");
      inherited_mode = 0;
      fallback_mode = 0;
    }
    if (pSDL_Init(SDL_INIT_VIDEO) != 0) {
      fprintf(stderr, "nxextract-ui: SDL_Init try %d: %s\n", attempt,
              pSDL_GetError());
      pSDL_Quit();
      note_video_failure(inherited_mode, &fallback_mode, &fallback_attempts,
                         &candidates);
      usleep(500000);
      continue;
    }
    driver = pSDL_GetCurrentVideoDriver();
    if (driver_is_invisible(driver)) {
      fprintf(stderr, "nxextract-ui: invisible SDL driver rejected: %s\n",
              driver);
      pSDL_Quit();
      note_video_failure(inherited_mode, &fallback_mode, &fallback_attempts,
                         &candidates);
      usleep(500000);
      continue;
    }
    window = pSDL_CreateWindow(title, 0, 0, 640, 480,
                               SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (window)
      renderer = pSDL_CreateRenderer(
          window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer && window)
      renderer = pSDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) {
      fprintf(stderr, "nxextract-ui: window/renderer try %d: %s\n", attempt,
              pSDL_GetError());
      if (window)
        pSDL_DestroyWindow(window);
      window = NULL;
      pSDL_Quit();
      note_video_failure(inherited_mode, &fallback_mode, &fallback_attempts,
                         &candidates);
      usleep(500000);
    }
  }
  if (!renderer)
    return 0;
  *window_out = window;
  *renderer_out = renderer;
  return 1;
}

int main(int argc, char **argv) {
  const char *progress_path, *stop_path, *title, *version;
  SDL_Window *window = NULL;
  SDL_Renderer *renderer = NULL;
  ProgressState progress = {
      .state = 1,
      .active = 1,
      .phase = 0,
      .overall = 0,
      .phase_progress = 0,
      .done_bytes = 0,
      .total_bytes = 0,
      .message = "PREPARING GAME DATA",
      .detail = "",
  };
  unsigned tick = 0;

  if (argc < 5) {
    fprintf(stderr,
            "usage: nxextract-ui PROGRESS_FILE STOP_FILE TITLE VERSION\n");
    return 2;
  }
  progress_path = argv[1];
  stop_path = argv[2];
  title = argv[3];
  version = argv[4];
  configure_session_runtime();
  if (!load_sdl())
    return 0;
  if (!create_renderer(stop_path, title, &window, &renderer)) {
    fprintf(stderr,
            "nxextract-ui: no visible renderer; extraction continues headless\n");
    return 0;
  }
  fprintf(stderr, "nxextract-ui: driver=%s\n",
          pSDL_GetCurrentVideoDriver() ? pSDL_GetCurrentVideoDriver() : "?");

  while (!path_exists(stop_path)) {
    SDL_Event event;
    int width = 640, height = 480;
    while (pSDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT_EVENT)
        goto out;
    }
    (void)read_progress(progress_path, &progress);
    if (pSDL_GetRendererOutputSize(renderer, &width, &height) != 0 ||
        width < 160 || height < 120) {
      width = 640;
      height = 480;
    }
    draw_screen(renderer, width, height, &progress, title, version, tick++);
    pSDL_RenderPresent(renderer);
    pSDL_Delay(50);
  }

out:
  if (renderer)
    pSDL_DestroyRenderer(renderer);
  if (window)
    pSDL_DestroyWindow(window);
  pSDL_Quit();
  if (sdl_handle)
    dlclose(sdl_handle);
  return 0;
}
