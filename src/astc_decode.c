/*
 * Software ASTC decoder bridge for GLES2-only NextOS devices.
 *
 * The decoder itself is Arm's astcenc library, shipped as libastcUtil.so.  This
 * small C bridge keeps the Unity host independent of the astcenc C++ ABI except
 * for the four stable entry points used by the proven NextOS ASTC decoder.
 */
#include <dlfcn.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASTCENC_PRF_LDR 1
#define ASTCENC_TYPE_U8 0
#define ASTCENC_SWZ_R 0
#define ASTCENC_SWZ_G 1
#define ASTCENC_SWZ_B 2
#define ASTCENC_SWZ_A 3
#define ASTCENC_PRE_FASTEST 0.0f
#define ASTCENC_FLG_DECOMPRESS_ONLY (1u << 4)

typedef struct astcenc_context astcenc_context;

struct astcenc_image {
  unsigned int dim_x;
  unsigned int dim_y;
  unsigned int dim_z;
  int data_type;
  void **data;
};

struct astcenc_swizzle {
  int r;
  int g;
  int b;
  int a;
};

/* Larger than every astcenc_config used by the bundled decoder. */
typedef unsigned char astcenc_config_blob[2048];

typedef int (*config_init_fn)(int, unsigned int, unsigned int, unsigned int,
                              float, unsigned int, void *);
typedef int (*context_alloc_fn)(const void *, unsigned int,
                                astcenc_context **);
typedef int (*decompress_image_fn)(astcenc_context *, const unsigned char *,
                                   size_t, struct astcenc_image *,
                                   const struct astcenc_swizzle *,
                                   unsigned int);
typedef int (*decompress_reset_fn)(astcenc_context *);

static config_init_fn p_config_init;
static context_alloc_fn p_context_alloc;
static decompress_image_fn p_decompress_image;
static decompress_reset_fn p_decompress_reset;
static astcenc_context *g_contexts[16][16];
static pthread_mutex_t g_astc_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_load_state;

static int astc_load_library(void) {
  if (g_load_state) return g_load_state > 0 ? 0 : -1;

  const char *paths[] = {"./libastcUtil.so", "libastcUtil.so", NULL};
  void *handle = NULL;
  for (int i = 0; paths[i] && !handle; i++) {
    handle = dlopen(paths[i], RTLD_NOW | RTLD_LOCAL);
  }
  if (!handle) {
    fprintf(stderr, "[HC-ASTC] libastcUtil.so não pôde ser carregada: %s\n",
            dlerror());
    g_load_state = -1;
    return -1;
  }

  p_config_init = (config_init_fn)dlsym(
      handle, "_Z19astcenc_config_init15astcenc_profilejjjfjP14astcenc_config");
  p_context_alloc = (context_alloc_fn)dlsym(
      handle,
      "_Z21astcenc_context_allocPK14astcenc_configjPP15astcenc_context");
  p_decompress_image = (decompress_image_fn)dlsym(
      handle,
      "_Z24astcenc_decompress_imageP15astcenc_contextPKhmP13astcenc_imagePK15astcenc_swizzlej");
  p_decompress_reset = (decompress_reset_fn)dlsym(
      handle, "_Z24astcenc_decompress_resetP15astcenc_context");

  if (!p_config_init || !p_context_alloc || !p_decompress_image) {
    fprintf(stderr,
            "[HC-ASTC] símbolos ausentes cfg=%p ctx=%p dec=%p reset=%p\n",
            p_config_init, p_context_alloc, p_decompress_image,
            p_decompress_reset);
    g_load_state = -1;
    return -1;
  }

  g_load_state = 1;
  fprintf(stderr, "[HC-ASTC] decoder Arm astcenc carregado\n");
  return 0;
}

static astcenc_context *astc_get_context(int block_x, int block_y) {
  if (block_x < 1 || block_x >= 16 || block_y < 1 || block_y >= 16)
    return NULL;
  if (g_contexts[block_x][block_y])
    return g_contexts[block_x][block_y];

  astcenc_config_blob config;
  memset(config, 0, sizeof(config));
  int result =
      p_config_init(ASTCENC_PRF_LDR, (unsigned)block_x, (unsigned)block_y, 1,
                    ASTCENC_PRE_FASTEST, ASTCENC_FLG_DECOMPRESS_ONLY, config);
  if (result) {
    fprintf(stderr, "[HC-ASTC] config %dx%d falhou: %d\n", block_x, block_y,
            result);
    return NULL;
  }

  astcenc_context *context = NULL;
  result = p_context_alloc(config, 1, &context);
  if (result || !context) {
    fprintf(stderr, "[HC-ASTC] contexto %dx%d falhou: %d\n", block_x,
            block_y, result);
    return NULL;
  }
  g_contexts[block_x][block_y] = context;
  fprintf(stderr, "[HC-ASTC] contexto %dx%d pronto\n", block_x, block_y);
  return context;
}

/*
 * Decode one raw ASTC mip level to tightly packed RGBA8888.
 * Returns zero on success.
 */
int astc_decode_rgba(void *destination, size_t destination_size,
                     const void *source, size_t source_size, int width,
                     int height, int block_x, int block_y) {
  if (!destination || !source || width <= 0 || height <= 0)
    return -1;
  if ((size_t)width > SIZE_MAX / 4 / (size_t)height)
    return -2;
  size_t rgba_size = (size_t)width * (size_t)height * 4;
  if (destination_size < rgba_size)
    return -3;

  int blocks_x = (width + block_x - 1) / block_x;
  int blocks_y = (height + block_y - 1) / block_y;
  if (blocks_x <= 0 || blocks_y <= 0 ||
      (size_t)blocks_x > SIZE_MAX / 16 / (size_t)blocks_y)
    return -4;
  size_t expected_source = (size_t)blocks_x * (size_t)blocks_y * 16;
  if (source_size < expected_source)
    return -5;

  pthread_mutex_lock(&g_astc_lock);
  if (astc_load_library()) {
    pthread_mutex_unlock(&g_astc_lock);
    return -6;
  }
  astcenc_context *context = astc_get_context(block_x, block_y);
  if (!context) {
    pthread_mutex_unlock(&g_astc_lock);
    return -7;
  }

  /*
   * Decode to block-aligned storage. Some astcenc versions write complete edge
   * blocks, so direct output into a non-aligned Unity mip can overrun its row.
   */
  int padded_width = blocks_x * block_x;
  int padded_height = blocks_y * block_y;
  size_t padded_size =
      (size_t)padded_width * (size_t)padded_height * (size_t)4;
  unsigned char *padded = (unsigned char *)malloc(padded_size);
  if (!padded) {
    pthread_mutex_unlock(&g_astc_lock);
    return -8;
  }

  void *slice = padded;
  struct astcenc_image image = {
      .dim_x = (unsigned)padded_width,
      .dim_y = (unsigned)padded_height,
      .dim_z = 1,
      .data_type = ASTCENC_TYPE_U8,
      .data = &slice,
  };
  const struct astcenc_swizzle swizzle = {
      ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
  int result = p_decompress_image(
      context, (const unsigned char *)source, expected_source, &image,
      &swizzle, 0);
  if (p_decompress_reset)
    p_decompress_reset(context);
  pthread_mutex_unlock(&g_astc_lock);

  if (result) {
    fprintf(stderr, "[HC-ASTC] decode %dx%d/%dx%d falhou: %d\n", width,
            height, block_x, block_y, result);
    free(padded);
    return -9;
  }

  unsigned char *out = (unsigned char *)destination;
  for (int y = 0; y < height; y++) {
    memcpy(out + (size_t)y * (size_t)width * 4,
           padded + (size_t)y * (size_t)padded_width * 4,
           (size_t)width * 4);
  }
  free(padded);
  return 0;
}
