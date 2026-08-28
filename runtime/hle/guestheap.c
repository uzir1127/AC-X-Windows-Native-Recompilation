/* Allocator over the 96 MB above the PSP's RAM.
 *
 * First fit with splitting and coalescing, over a flat array of block records
 * in host memory (see the header for why the metadata is not guest-side).
 * Blocks stay sorted by address, which makes coalescing a neighbour check and
 * lets ghp_size bisect.
 */

#include "guestheap.h"

#include <stdio.h>
#include <string.h>

/* One record per live or free extent.  Coalescing keeps an allocate/free
 * every frame from fragmenting the array. */
#define GHP_MAX_BLOCKS 8192

typedef struct {
    u32 addr;
    u32 size;
    int used;
} Block;

static Block s_b[GHP_MAX_BLOCKS];
static int   s_n;
static int   s_inited;

static u32 align_up(u32 v)
{
    return (v + (GHEAP_ALIGN - 1u)) & ~(GHEAP_ALIGN - 1u);
}

void ghp_init(void)
{
    if (s_inited) return;
    s_inited = 1;
    s_b[0].addr = MOD_HEAP_BASE;
    s_b[0].size = MOD_HEAP_END - MOD_HEAP_BASE;
    s_b[0].used = 0;
    s_n = 1;
}

/* Index of the block containing `addr`, or -1.  Blocks are sorted and
 * contiguous, so bisect. */
static int find_block(u32 addr)
{
    int lo = 0, hi = s_n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (s_b[mid].addr + s_b[mid].size <= addr) lo = mid + 1;
        else if (s_b[mid].addr > addr)             hi = mid;
        else                                       return mid;
    }
    return -1;
}

/* Split block `i` so its first `want` bytes stand alone.  The remainder
 * becomes a free block after it. */
static int split(int i, u32 want)
{
    int j;
    if (s_b[i].size <= want) return 1;            /* nothing worth splitting */
    if (s_n >= GHP_MAX_BLOCKS) return 0;
    for (j = s_n; j > i + 1; j--) s_b[j] = s_b[j - 1];
    s_n++;
    s_b[i + 1].addr = s_b[i].addr + want;
    s_b[i + 1].size = s_b[i].size - want;
    s_b[i + 1].used = 0;
    s_b[i].size = want;
    return 1;
}

static void merge_with_next(int i)
{
    int j;
    if (i < 0 || i + 1 >= s_n) return;
    if (s_b[i].used || s_b[i + 1].used) return;
    s_b[i].size += s_b[i + 1].size;
    for (j = i + 1; j + 1 < s_n; j++) s_b[j] = s_b[j + 1];
    s_n--;
}

u32 ghp_alloc(u32 bytes)
{
    u32 want;
    int i;

    ghp_init();
    if (!bytes) return 0;
    want = align_up(bytes);
    if (want < bytes) return 0;                   /* wrapped: absurd request */

    for (i = 0; i < s_n; i++) {
        if (s_b[i].used || s_b[i].size < want) continue;
        if (!split(i, want)) {
            fprintf(stderr, "[heap] out of block records (%d); a mod is "
                    "fragmenting the mod heap\n", GHP_MAX_BLOCKS);
            return 0;
        }
        s_b[i].used = 1;
        return s_b[i].addr;
    }
    fprintf(stderr, "[heap] mod heap exhausted: %u bytes wanted, %u MB total\n",
            (unsigned)bytes,
            (unsigned)((MOD_HEAP_END - MOD_HEAP_BASE) >> 20));
    return 0;
}

u32 ghp_calloc(u8 *ram, u32 bytes)
{
    u32 a = ghp_alloc(bytes);
    if (a && ram) memset(ram + (a & PSP_ADDR_MASK), 0, (size_t)align_up(bytes));
    return a;
}

void ghp_free(u32 addr)
{
    int i;

    ghp_init();
    if (!addr) return;                            /* free(NULL), as usual */
    i = find_block(addr);
    if (i < 0 || s_b[i].addr != addr || !s_b[i].used) {
        fprintf(stderr, "[heap] free(%08x) is not the start of a live block\n",
                (unsigned)addr);
        return;
    }
    s_b[i].used = 0;
    /* Next first, then previous: merging the previous one moves this index. */
    merge_with_next(i);
    if (i > 0) merge_with_next(i - 1);
}

u32 ghp_size(u32 addr)
{
    int i;
    ghp_init();
    i = find_block(addr);
    if (i < 0 || s_b[i].addr != addr || !s_b[i].used) return 0;
    return s_b[i].size;
}

void ghp_stats(u32 *used, u32 *free_bytes, int *blocks)
{
    u32 u = 0, f = 0;
    int i;
    ghp_init();
    for (i = 0; i < s_n; i++) {
        if (s_b[i].used) u += s_b[i].size;
        else             f += s_b[i].size;
    }
    if (used)       *used = u;
    if (free_bytes) *free_bytes = f;
    if (blocks)     *blocks = s_n;
}
