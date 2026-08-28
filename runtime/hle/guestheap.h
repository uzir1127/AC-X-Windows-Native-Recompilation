#ifndef PSPRECO_GUESTHEAP_H
#define PSPRECO_GUESTHEAP_H

#include "pspreco.h"

/* Extended guest RAM, and the allocator over it.
 *
 * The user partition ends at 0x09FFFF00 and the flat window runs to
 * PSP_RAM_SIZE, so everything above is addressable, backed and unused by
 * anything else (see sysmem.c).  The hardware had no such memory; the window
 * is one host allocation and the address mask collapses the mirrors.
 *
 * Two callers share it:
 *
 *   modloader.c  stages replacement assets at boot and never frees them.
 *   acx_api.c    hands blocks to mods at runtime, which do free.
 *
 * Block metadata is kept host-side rather than in a header before each block,
 * so a mod writing past the end of its own allocation cannot corrupt the
 * heap's bookkeeping. */

#define MOD_HEAP_BASE 0x0A000000u
#define MOD_HEAP_END  0x10000000u

/* 64 bytes, matching the arena alignment in sub_1D1FDC, so a staged asset
 * sits where the engine would have put it. */
#define GHEAP_ALIGN   64u

void ghp_init(void);

/* Guest address, or 0 when the heap is full or the request is silly.
 * ghp_calloc zeroes; ghp_alloc does not. */
u32  ghp_alloc(u32 bytes);
u32  ghp_calloc(u8 *ram, u32 bytes);
void ghp_free(u32 addr);

/* Bytes usable at `addr`, or 0 if that is not the start of a live block.
 * The API bounds checks go through this. */
u32  ghp_size(u32 addr);

/* Two compares; the GE asks per texture bind. */
static inline int ghp_contains(u32 a)
{
    return a >= MOD_HEAP_BASE && a < MOD_HEAP_END;
}

void ghp_stats(u32 *used, u32 *free_bytes, int *blocks);

#endif
