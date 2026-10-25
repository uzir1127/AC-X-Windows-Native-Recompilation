#ifndef PSPRECO_H264_DEC_H
#define PSPRECO_H264_DEC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct H264Dec H264Dec;

H264Dec *h264_create(void);
void h264_destroy(H264Dec *d);

void h264_feed(H264Dec *d, const uint8_t *data, uint32_t len);

int h264_frame(H264Dec *d, int eos, uint8_t *dst, int frameWidth,
               int pixelMode);
int h264_frame_dimensions(H264Dec *d, int *width, int *height);

#ifdef __cplusplus
}
#endif
#endif
