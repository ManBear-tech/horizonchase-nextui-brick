#ifndef HORIZON_ETC2_DECODE_H
#define HORIZON_ETC2_DECODE_H

unsigned char *etc2_decode_rgba(unsigned format, int width, int height,
                                const void *data, int size);

#endif
