/*
 * ETC2 to RGBA8888 CPU decoder for GLES2-only Mali-450.
 *
 * Formats: ETC2 RGB8 (0x9274), ETC2 RGBA8 (0x9278, EAC alpha), and
 * ETC2 punch-through A1 (0x9276). Based on the Khronos Data Format
 * Specification and OpenGL ES 3.0 appendix C.1. This is the decoder used by
 * the validated NextOS GTA ports.
 */
#include <stdint.h>
#include <stdlib.h>

static const int etc_mod[8][2] = {
    {2, 8}, {5, 17}, {9, 29}, {13, 42},
    {18, 60}, {24, 80}, {33, 106}, {47, 183}};
static const int etc2_dist[8] = {3, 6, 11, 16, 23, 32, 41, 64};
static const int eac_tab[16][8] = {
    {-3, -6, -9, -15, 2, 5, 8, 14},
    {-3, -7, -10, -13, 2, 6, 9, 12},
    {-2, -5, -8, -13, 1, 4, 7, 12},
    {-2, -4, -6, -13, 1, 3, 5, 12},
    {-3, -6, -8, -12, 2, 5, 7, 11},
    {-3, -7, -9, -11, 2, 6, 8, 10},
    {-4, -7, -8, -11, 3, 6, 7, 10},
    {-3, -5, -8, -11, 2, 4, 7, 10},
    {-2, -6, -8, -10, 1, 5, 7, 9},
    {-2, -5, -8, -10, 1, 4, 7, 9},
    {-2, -4, -8, -10, 1, 3, 7, 9},
    {-2, -5, -7, -10, 1, 4, 6, 9},
    {-3, -4, -7, -10, 2, 3, 6, 9},
    {-1, -2, -3, -10, 0, 1, 2, 9},
    {-4, -6, -8, -9, 3, 5, 7, 8},
    {-3, -5, -7, -9, 2, 4, 6, 8}};

static inline uint8_t clamp8(int value) {
  return value < 0 ? 0 : (value > 255 ? 255 : (uint8_t)value);
}
static inline int ext4(int value) { return (value << 4) | value; }
static inline int ext5(int value) { return (value << 3) | (value >> 2); }
static inline int ext6(int value) { return (value << 2) | (value >> 4); }
static inline int ext7(int value) { return (value << 1) | (value >> 6); }
static inline int sext3(int value) { return value >= 4 ? value - 8 : value; }

static void etc2_color_block(const uint8_t *source, uint8_t out[16][4],
                             int punch) {
  uint32_t hi = ((uint32_t)source[0] << 24) |
                ((uint32_t)source[1] << 16) |
                ((uint32_t)source[2] << 8) | source[3];
  uint32_t lo = ((uint32_t)source[4] << 24) |
                ((uint32_t)source[5] << 16) |
                ((uint32_t)source[6] << 8) | source[7];
  int differential = (hi >> 1) & 1;
  int opaque = differential;
  int flip = hi & 1;
  for (int i = 0; i < 16; i++)
    out[i][3] = 255;

  if (!punch && !differential) {
    int r1 = ext4((hi >> 28) & 0xf), r2 = ext4((hi >> 24) & 0xf);
    int g1 = ext4((hi >> 20) & 0xf), g2 = ext4((hi >> 16) & 0xf);
    int b1 = ext4((hi >> 12) & 0xf), b2 = ext4((hi >> 8) & 0xf);
    int t1 = (hi >> 5) & 7, t2 = (hi >> 2) & 7;
    static const int sign[4] = {1, 1, -1, -1};
    static const int magnitude[4] = {0, 1, 0, 1};
    for (int p = 0; p < 16; p++) {
      int x = p >> 2, y = p & 3;
      int sub = flip ? (y >= 2) : (x >= 2);
      int index = (((lo >> (p + 16)) & 1) << 1) | ((lo >> p) & 1);
      int modifier =
          etc_mod[sub ? t2 : t1][magnitude[index]] * sign[index];
      int r = sub ? r2 : r1;
      int g = sub ? g2 : g1;
      int b = sub ? b2 : b1;
      int offset = y * 4 + x;
      out[offset][0] = clamp8(r + modifier);
      out[offset][1] = clamp8(g + modifier);
      out[offset][2] = clamp8(b + modifier);
    }
    return;
  }

  int r1_5 = (hi >> 27) & 0x1f, dr = sext3((hi >> 24) & 7);
  int g1_5 = (hi >> 19) & 0x1f, dg = sext3((hi >> 16) & 7);
  int b1_5 = (hi >> 11) & 0x1f, db = sext3((hi >> 8) & 7);
  int r2_5 = r1_5 + dr;
  int g2_5 = g1_5 + dg;
  int b2_5 = b1_5 + db;

  if (r2_5 < 0 || r2_5 > 31) {
    int r1 = ext4(((hi >> 27) & 0xc) | ((hi >> 24) & 3));
    int g1 = ext4((hi >> 20) & 0xf);
    int b1 = ext4((hi >> 16) & 0xf);
    int r2 = ext4((hi >> 12) & 0xf);
    int g2 = ext4((hi >> 8) & 0xf);
    int b2 = ext4((hi >> 4) & 0xf);
    int distance = etc2_dist[(((hi >> 2) & 3) << 1) | (hi & 1)];
    uint8_t paint[4][3] = {
        {(uint8_t)r1, (uint8_t)g1, (uint8_t)b1},
        {clamp8(r2 + distance), clamp8(g2 + distance),
         clamp8(b2 + distance)},
        {(uint8_t)r2, (uint8_t)g2, (uint8_t)b2},
        {clamp8(r2 - distance), clamp8(g2 - distance),
         clamp8(b2 - distance)}};
    for (int p = 0; p < 16; p++) {
      int x = p >> 2, y = p & 3;
      int index = (((lo >> (p + 16)) & 1) << 1) | ((lo >> p) & 1);
      int offset = y * 4 + x;
      out[offset][0] = paint[index][0];
      out[offset][1] = paint[index][1];
      out[offset][2] = paint[index][2];
      if (punch && !opaque && index == 2) {
        out[offset][0] = out[offset][1] = out[offset][2] = 0;
        out[offset][3] = 0;
      }
    }
    return;
  }

  if (g2_5 < 0 || g2_5 > 31) {
    int r1 = ext4((hi >> 27) & 0xf);
    int g1 = ext4((((hi >> 24) & 7) << 1) | ((hi >> 20) & 1));
    int b1 = ext4((((hi >> 19) & 1) << 3) | ((hi >> 15) & 7));
    int r2 = ext4((hi >> 11) & 0xf);
    int g2 = ext4((hi >> 7) & 0xf);
    int b2 = ext4((hi >> 3) & 0xf);
    int distance_index = (((hi >> 2) & 1) << 2) | ((hi & 1) << 1);
    int value1 = (r1 << 16) | (g1 << 8) | b1;
    int value2 = (r2 << 16) | (g2 << 8) | b2;
    if (value1 >= value2)
      distance_index |= 1;
    int distance = etc2_dist[distance_index];
    uint8_t paint[4][3] = {
        {clamp8(r1 + distance), clamp8(g1 + distance),
         clamp8(b1 + distance)},
        {clamp8(r1 - distance), clamp8(g1 - distance),
         clamp8(b1 - distance)},
        {clamp8(r2 + distance), clamp8(g2 + distance),
         clamp8(b2 + distance)},
        {clamp8(r2 - distance), clamp8(g2 - distance),
         clamp8(b2 - distance)}};
    for (int p = 0; p < 16; p++) {
      int x = p >> 2, y = p & 3;
      int index = (((lo >> (p + 16)) & 1) << 1) | ((lo >> p) & 1);
      int offset = y * 4 + x;
      out[offset][0] = paint[index][0];
      out[offset][1] = paint[index][1];
      out[offset][2] = paint[index][2];
      if (punch && !opaque && index == 2) {
        out[offset][0] = out[offset][1] = out[offset][2] = 0;
        out[offset][3] = 0;
      }
    }
    return;
  }

  if (b2_5 < 0 || b2_5 > 31) {
    int ro = ext6((hi >> 25) & 0x3f);
    int go = ext7((((hi >> 24) & 1) << 6) | ((hi >> 17) & 0x3f));
    int bo = ext6((((hi >> 16) & 1) << 5) |
                  (((hi >> 11) & 3) << 3) | ((hi >> 7) & 7));
    int rh = ext6((((hi >> 2) & 0x1f) << 1) | (hi & 1));
    int gh = ext7((lo >> 25) & 0x7f);
    int bh = ext6((lo >> 19) & 0x3f);
    int rv = ext6((lo >> 13) & 0x3f);
    int gv = ext7((lo >> 6) & 0x7f);
    int bv = ext6(lo & 0x3f);
    for (int y = 0; y < 4; y++) {
      for (int x = 0; x < 4; x++) {
        int offset = y * 4 + x;
        out[offset][0] =
            clamp8((x * (rh - ro) + y * (rv - ro) + 4 * ro + 2) >> 2);
        out[offset][1] =
            clamp8((x * (gh - go) + y * (gv - go) + 4 * go + 2) >> 2);
        out[offset][2] =
            clamp8((x * (bh - bo) + y * (bv - bo) + 4 * bo + 2) >> 2);
      }
    }
    return;
  }

  int r1 = ext5(r1_5), g1 = ext5(g1_5), b1 = ext5(b1_5);
  int r2 = ext5(r2_5), g2 = ext5(g2_5), b2 = ext5(b2_5);
  int t1 = (hi >> 5) & 7, t2 = (hi >> 2) & 7;
  static const int sign[4] = {1, 1, -1, -1};
  static const int magnitude[4] = {0, 1, 0, 1};
  for (int p = 0; p < 16; p++) {
    int x = p >> 2, y = p & 3;
    int sub = flip ? (y >= 2) : (x >= 2);
    int index = (((lo >> (p + 16)) & 1) << 1) | ((lo >> p) & 1);
    int modifier =
        etc_mod[sub ? t2 : t1][magnitude[index]] * sign[index];
    int offset = y * 4 + x;
    if (punch && !opaque) {
      if (index == 2) {
        out[offset][0] = out[offset][1] = out[offset][2] = 0;
        out[offset][3] = 0;
        continue;
      }
      if (index == 0)
        modifier = 0;
    }
    int r = sub ? r2 : r1;
    int g = sub ? g2 : g1;
    int b = sub ? b2 : b1;
    out[offset][0] = clamp8(r + modifier);
    out[offset][1] = clamp8(g + modifier);
    out[offset][2] = clamp8(b + modifier);
  }
}

static void eac_alpha_block(const uint8_t *source, uint8_t alpha[16]) {
  int base = source[0];
  int multiplier = (source[1] >> 4) & 0xf;
  const int *table = eac_tab[source[1] & 0xf];
  uint64_t bits = 0;
  for (int i = 2; i < 8; i++)
    bits = (bits << 8) | source[i];
  for (int p = 0; p < 16; p++) {
    int index = (int)((bits >> (45 - p * 3)) & 7);
    int x = p >> 2, y = p & 3;
    alpha[y * 4 + x] = clamp8(base + table[index] * multiplier);
  }
}

unsigned char *etc2_decode_rgba(unsigned format, int width, int height,
                                const void *data, int size) {
  int blocks_wide = (width + 3) / 4;
  int blocks_high = (height + 3) / 4;
  int has_eac = format == 0x9278 || format == 0x9279;
  int punch = format == 0x9276 || format == 0x9277;
  int rgb = format == 0x9274 || format == 0x9275;
  if (width <= 0 || height <= 0 || (!has_eac && !punch && !rgb))
    return NULL;
  int block_size = has_eac ? 16 : 8;
  if (size < blocks_wide * blocks_high * block_size)
    return NULL;
  if ((size_t)width > SIZE_MAX / 4 / (size_t)height)
    return NULL;
  unsigned char *out = malloc((size_t)width * (size_t)height * 4);
  if (!out)
    return NULL;

  const uint8_t *source = (const uint8_t *)data;
  for (int by = 0; by < blocks_high; by++) {
    for (int bx = 0; bx < blocks_wide; bx++) {
      const uint8_t *block =
          source + ((size_t)by * blocks_wide + bx) * block_size;
      uint8_t pixels[16][4];
      uint8_t alpha[16];
      if (has_eac) {
        eac_alpha_block(block, alpha);
        block += 8;
      }
      etc2_color_block(block, pixels, punch);
      for (int y = 0; y < 4; y++) {
        int target_y = by * 4 + y;
        if (target_y >= height)
          break;
        for (int x = 0; x < 4; x++) {
          int target_x = bx * 4 + x;
          if (target_x >= width)
            break;
          unsigned char *destination =
              out + ((size_t)target_y * width + target_x) * 4;
          destination[0] = pixels[y * 4 + x][0];
          destination[1] = pixels[y * 4 + x][1];
          destination[2] = pixels[y * 4 + x][2];
          destination[3] =
              has_eac ? alpha[y * 4 + x] : pixels[y * 4 + x][3];
        }
      }
    }
  }
  return out;
}
