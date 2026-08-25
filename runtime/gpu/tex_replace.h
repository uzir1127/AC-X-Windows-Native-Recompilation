#ifndef PSPRECO_TEX_REPLACE_H
#define PSPRECO_TEX_REPLACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { TEXREP_FILTER_AUTO = 0, TEXREP_FILTER_NEAREST = 1, TEXREP_FILTER_LINEAR = 2 };

#define TEXREP_MAX_LEVELS 8
#define TEXREP_MAX_DIM    8192

typedef struct {
    uint32_t *px[TEXREP_MAX_LEVELS];
    int       w[TEXREP_MAX_LEVELS];
    int       h[TEXREP_MAX_LEVELS];
    int       levels;
    int       filtering;
} TexRepImage;

typedef struct {
    uint64_t cachekey;
    uint32_t hash;
    uint32_t alt_hash;
    int      has_alt;
    int      psp_w, psp_h;
} TexRepId;

void texrep_init(void);
void texrep_shutdown(void);
void texrep_reload(void);

int  texrep_replace_on(void);
int  texrep_dump_on(void);

int  texrep_identify(TexRepId *id);
int  texrep_find(TexRepId *id, TexRepImage *img);
void texrep_release(TexRepImage *img);
void texrep_dump(const TexRepId *id, const uint32_t *rgba, int w, int h);

void texrep_status(char *out, int n);

#ifdef __cplusplus
}
#endif

#endif
