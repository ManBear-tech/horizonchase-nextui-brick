#ifndef HORIZON_ASTC_DECODE_H
#define HORIZON_ASTC_DECODE_H

#include <stddef.h>

int astc_decode_rgba(void *destination, size_t destination_size,
                     const void *source, size_t source_size, int width,
                     int height, int block_x, int block_y);

#endif
