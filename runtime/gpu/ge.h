#ifndef PSPRECO_GE_H
#define PSPRECO_GE_H

#include "pspreco.h"

int  ge_state_command(u8 *ram, u32 word, u32 base);
void ge_draw_prim(u8 *ram, u32 word);

void ge_raster_init(void);
void ge_raster_report(void);
void ge_raster_probe(u8 *ram, const char *when);
int  ge_raster_selftest(u8 *ram);

u32  ge_unpack_pixel(int fpf, u32 v);
u32  ge_framebuffer_addr(void);

void ge_set_frame(u32 frame);

void ge_gpu_flush(const char *reason);
void ge_gpu_vram_dirty(u32 addr, u32 bytes);

extern int (*ge_gpu_present_fn)(u32 fbaddr, int fmt, u32 stride);

int  present_gpu_init(void);

#endif
