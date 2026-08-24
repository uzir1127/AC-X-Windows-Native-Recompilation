#ifndef PSPRECO_PGF_H
#define PSPRECO_PGF_H

#include <stdint.h>

typedef struct PGF PGF;

PGF *pgf_open(const char *path);

int pgf_has_char(const PGF *p, int charCode);

void pgf_get_font_info(uint8_t *ram, const PGF *p, uint32_t gfi);

int pgf_get_char_info(uint8_t *ram, const PGF *p, int charCode, int altCharCode, uint32_t gci);

int pgf_draw_glyph(uint8_t *ram, const PGF *p, int charCode, int altCharCode, uint32_t ggi);

#endif
