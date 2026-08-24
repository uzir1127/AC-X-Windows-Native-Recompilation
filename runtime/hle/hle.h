#ifndef PSPRECO_HLE_H
#define PSPRECO_HLE_H

#include "pspreco.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*HleFn)(CpuCtx *c, u8 *ram);

typedef struct HleEntry {
    u32         nid;
    const char *name;
    HleFn       fn;
} HleEntry;

extern const HleEntry g_hle_impl[];
extern const unsigned g_hle_impl_count;

void hle_init(void);
void hle_set_trace(int on);
int  hle_trace_enabled(void);

#define PSP_VOLATILE_BASE 0x08400000u
#define PSP_VOLATILE_SIZE 0x00400000u

#define HLE_A0 (c->r[R_A0])
#define HLE_A1 (c->r[R_A1])
#define HLE_A2 (c->r[R_A2])
#define HLE_A3 (c->r[R_A3])

static inline u32 hle_arg(CpuCtx *c, u8 *ram, unsigned index)
{
    (void)ram;
    return c->r[R_T0 + index];
}

#define HLE_RET(v)   do { c->r[R_V0] = (u32)(v); c->r[R_V1] = 0u; } while (0)
#define HLE_RET64(v) do { u64 _v = (u64)(v);                         \
                          c->r[R_V0] = (u32)_v;                      \
                          c->r[R_V1] = (u32)(_v >> 32); } while (0)

static inline u32 hle_r32(u8 *ram, u32 a) { return mem_r32(ram, a); }
static inline void hle_w32(u8 *ram, u32 a, u32 v) { mem_w32(ram, a, v); }
const char *hle_cstr(u8 *ram, u32 addr);

void hle_log(const char *fmt, ...);

int hle_module_id_for_path(const char *path);

#ifdef __cplusplus
}
#endif
#endif
