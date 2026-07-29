#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "astc_decode.h"

static int read_u24(const unsigned char *value) {
  return value[0] | ((int)value[1] << 8) | ((int)value[2] << 16);
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s texture.astc | --contexts\n", argv[0]);
    return 2;
  }
  if (strcmp(argv[1], "--contexts") == 0) {
    static const unsigned char blocks[][2] = {
        {4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6},
        {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    unsigned char source[16] = {0};
    unsigned char rgba[12 * 12 * 4];
    for (size_t i = 0; i < sizeof blocks / sizeof blocks[0]; i++) {
      int width = blocks[i][0], height = blocks[i][1];
      int result = astc_decode_rgba(rgba, sizeof rgba, source, sizeof source,
                                    width, height, width, height);
      if (result) {
        fprintf(stderr, "context %dx%d failed: %d\n", width, height, result);
        return 1;
      }
    }
    puts("all 14 ASTC LDR block contexts decoded");
    return 0;
  }

  FILE *file = fopen(argv[1], "rb");
  if (!file) {
    perror(argv[1]);
    return 1;
  }
  unsigned char header[16];
  if (fread(header, 1, sizeof header, file) != sizeof header ||
      header[0] != 0x13 || header[1] != 0xab ||
      header[2] != 0xa1 || header[3] != 0x5c) {
    fprintf(stderr, "invalid ASTC header\n");
    fclose(file);
    return 1;
  }
  int block_x = header[4], block_y = header[5];
  int width = read_u24(header + 7);
  int height = read_u24(header + 10);
  int depth = read_u24(header + 13);
  int blocks_x = (width + block_x - 1) / block_x;
  int blocks_y = (height + block_y - 1) / block_y;
  size_t source_size = (size_t)blocks_x * (size_t)blocks_y * 16;
  size_t rgba_size = (size_t)width * (size_t)height * 4;
  unsigned char *source = malloc(source_size);
  unsigned char *rgba = malloc(rgba_size);
  if (depth != 1 || !source || !rgba ||
      fread(source, 1, source_size, file) != source_size) {
    fprintf(stderr, "invalid or truncated 2D ASTC payload\n");
    fclose(file);
    free(source);
    free(rgba);
    return 1;
  }
  fclose(file);

  int result = astc_decode_rgba(rgba, rgba_size, source, source_size,
                                width, height, block_x, block_y);
  free(source);
  if (result) {
    fprintf(stderr, "decode failed: %d\n", result);
    free(rgba);
    return 1;
  }

  uint64_t hash = UINT64_C(1469598103934665603);
  unsigned minimum = 255, maximum = 0;
  for (size_t i = 0; i < rgba_size; i++) {
    unsigned value = rgba[i];
    if (value < minimum) minimum = value;
    if (value > maximum) maximum = value;
    hash = (hash ^ value) * UINT64_C(1099511628211);
  }
  free(rgba);
  printf("ASTC %dx%d block=%dx%d rgba=%zu min=%u max=%u fnv64=%016llx\n",
         width, height, block_x, block_y, rgba_size, minimum, maximum,
         (unsigned long long)hash);
  return minimum == maximum;
}
