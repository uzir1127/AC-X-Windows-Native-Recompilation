#ifndef PSPRECO_TEX_KTX2_H
#define PSPRECO_TEX_KTX2_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int ktx2_is_ktx2(const uint8_t *data, size_t len);

int ktx2_decode_levels(const uint8_t *data, size_t len, int max_levels,
                       uint32_t **px, int *w, int *h, const char **err);

#ifdef __cplusplus
}
#endif

#endif
