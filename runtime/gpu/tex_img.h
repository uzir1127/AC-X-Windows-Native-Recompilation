#ifndef PSPRECO_TEX_IMG_H
#define PSPRECO_TEX_IMG_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t *img_decode(const uint8_t *data, size_t len, int *w, int *h);
int       img_write_png(const char *path, const uint32_t *px, int w, int h);
int       img_inflate(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen,
                      size_t *produced);

#ifdef __cplusplus
}
#endif

#endif
