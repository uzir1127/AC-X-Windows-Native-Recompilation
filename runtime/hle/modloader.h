#ifndef PSPRECO_MODLOADER_H
#define PSPRECO_MODLOADER_H

#include "pspreco.h"
/* MOD_HEAP_BASE / MOD_HEAP_END and the allocator over them; ge_render.c
 * reaches the bounds through this header. */
#include "guestheap.h"

void     mod_init(u8 *ram);
RecompFn mod_override(u32 addr, RecompFn orig);
void     mod_tick(CpuCtx *c, u8 *ram);

/* Called from the GE when a texture base lands in the mod heap.  Reports
 * once. */
void     mod_note_tex(u32 addr);

#endif
