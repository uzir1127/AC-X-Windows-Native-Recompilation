/* The host API.
 *
 * A RecompFn is (CpuCtx *, u8 *), so one shared C function cannot tell which
 * hooked address it was entered for and each hooked site needs its own
 * trampoline.  ACX_TRAMP_ALL generates 256 of them.
 *
 * The indices are hex: `0x08` is a valid C token and `08` is not a valid
 * octal one, so a decimal version of the macro miscompiles at indices 8 and 9
 * of every row.
 *
 * The limit is on distinct hooked addresses, not handlers -- each site chains
 * as many as it likes -- and sites are recycled on unhook.  A fourth nesting
 * level would take it to 4096.
 */

#include "acx_api.h"
#include "guestheap.h"
#include "hooks.h"
#include "hle.h"
#include "../../generated/acx_symtab.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern u8 *g_ram;

#define ACX_MAX_SITES 256
#define ACX_OWNER_MAX 64

/* ------------------------------------------------------------------ */
/* the live context                                                     */
/* ------------------------------------------------------------------ */

/* Whatever context the runtime is currently inside: the frame tick's, or the
 * one a hook was entered with.  invoke() seeds from it, and hook dispatch
 * saves and restores it so nested calls into the game work. */
static CpuCtx *s_cpu;
static u8     *s_ram;
static u64     s_frames;
static int     s_inited;

void acx_api_set_context(CpuCtx *c, u8 *ram)
{
    s_cpu = c;
    s_ram = ram ? ram : g_ram;
}

static u8 *cur_ram(void) { return s_ram ? s_ram : g_ram; }

/* ------------------------------------------------------------------ */
/* memory                                                               */
/* ------------------------------------------------------------------ */

static u8  *api_ram(void)          { return cur_ram(); }
static u32  api_r8 (u32 a)         { return mem_r8 (cur_ram(), a); }
static u32  api_r16(u32 a)         { return mem_r16(cur_ram(), a); }
static u32  api_r32(u32 a)         { return mem_r32(cur_ram(), a); }
static s32  api_rs8 (u32 a)        { return (s8 )(u8 )mem_r8 (cur_ram(), a); }
static s32  api_rs16(u32 a)        { return (s16)(u16)mem_r16(cur_ram(), a); }
static s32  api_rs32(u32 a)        { return (s32)mem_r32(cur_ram(), a); }
static f32  api_rf32(u32 a)        { return mem_rf32(cur_ram(), a); }
static void api_w8 (u32 a, u32 v)  { mem_w8 (cur_ram(), a, v); }
static void api_w16(u32 a, u32 v)  { mem_w16(cur_ram(), a, v); }
static void api_w32(u32 a, u32 v)  { mem_w32(cur_ram(), a, v); }
static void api_wf32(u32 a, f32 v) { mem_wf32(cur_ram(), a, v); }

static int api_rstr(u32 a, char *out, int max)
{
    u8 *ram = cur_ram();
    int i;
    if (max <= 0) return 0;
    for (i = 0; i < max - 1; i++) {
        u8 ch = (u8)mem_r8(ram, a + (u32)i);
        if (!ch) break;
        out[i] = (char)ch;
    }
    out[i] = '\0';
    return i;
}

/* memcpy rather than a byte loop: the window is one host allocation and the
 * mask only matters at the base.  A copy that wrapped the 512 MB window would
 * be a caller bug. */
static void api_rbytes(u32 a, void *out, u32 n)
{
    memcpy(out, cur_ram() + (a & PSP_ADDR_MASK), (size_t)n);
}

static void api_wbytes(u32 a, const void *in, u32 n)
{
    memcpy(cur_ram() + (a & PSP_ADDR_MASK), in, (size_t)n);
}

static void api_memset(u32 a, int byte, u32 n)
{
    memset(cur_ram() + (a & PSP_ADDR_MASK), byte, (size_t)n);
}

/* ------------------------------------------------------------------ */
/* the mod heap                                                         */
/* ------------------------------------------------------------------ */

static u32  api_alloc(u32 n)      { return ghp_alloc(n); }
static u32  api_calloc(u32 n)     { return ghp_calloc(cur_ram(), n); }
static void api_free(u32 a)       { ghp_free(a); }
static u32  api_block_size(u32 a) { return ghp_size(a); }

static u32 api_cstr(const char *s)
{
    u32 n = (u32)strlen(s) + 1u, a = ghp_alloc(n);
    if (a) memcpy(cur_ram() + (a & PSP_ADDR_MASK), s, (size_t)n);
    return a;
}

/* ------------------------------------------------------------------ */
/* symbols                                                              */
/* ------------------------------------------------------------------ */

/* g_acx_syms is sorted by address within functions and again within globals,
 * so it is not globally sorted and name_at cannot bisect it directly.  Build
 * one index instead. */
static const AcxSym **s_by_addr;
static int            s_by_addr_n;

static int cmp_sym(const void *a, const void *b)
{
    const AcxSym *x = *(const AcxSym *const *)a;
    const AcxSym *y = *(const AcxSym *const *)b;
    return x->guest < y->guest ? -1 : (x->guest > y->guest ? 1 : 0);
}

static void build_sym_index(void)
{
    int i;
    if (s_by_addr || g_acx_sym_count <= 0) return;
    s_by_addr = (const AcxSym **)malloc(sizeof *s_by_addr *
                                        (size_t)g_acx_sym_count);
    if (!s_by_addr) return;
    for (i = 0; i < g_acx_sym_count; i++) s_by_addr[i] = &g_acx_syms[i];
    s_by_addr_n = g_acx_sym_count;
    qsort(s_by_addr, (size_t)s_by_addr_n, sizeof *s_by_addr, cmp_sym);
}

static const AcxSym *sym_find(const char *name)
{
    int i;
    for (i = 0; i < g_acx_sym_count; i++)
        if (strcmp(g_acx_syms[i].name, name) == 0) return &g_acx_syms[i];
    return NULL;
}

static int api_sym(const char *name, AcxSymInfo *out)
{
    const AcxSym *s = sym_find(name);
    if (!s) return 0;
    if (out) {
        out->name       = s->name;
        out->addr       = s->guest;
        out->size       = s->size;
        out->is_func    = (s->kind == ACX_SYM_FUNC);
        out->hook       = s->hook;
        out->recompiled = s->recompiled;
    }
    return 1;
}

static u32 api_sym_addr(const char *name)
{
    const AcxSym *s = sym_find(name);
    return s ? s->guest : 0u;
}

static const char *api_name_at(u32 addr, u32 *offset)
{
    int lo, hi, best = -1;

    build_sym_index();
    if (!s_by_addr) return NULL;
    lo = 0;
    hi = s_by_addr_n;
    while (lo < hi) {                       /* last symbol at or below addr */
        int mid = lo + (hi - lo) / 2;
        if (s_by_addr[mid]->guest <= addr) { best = mid; lo = mid + 1; }
        else                                 hi = mid;
    }
    if (best < 0) return NULL;
    {
        const AcxSym *s = s_by_addr[best];
        u32 span = s->size ? s->size : 4u;
        if (addr - s->guest >= span) return NULL;
        if (offset) *offset = addr - s->guest;
        return s->name;
    }
}

/* ------------------------------------------------------------------ */
/* hooks                                                                */
/* ------------------------------------------------------------------ */

typedef struct HookNode {
    int              id;
    AcxHookFn        fn;
    void            *user;
    char             owner[ACX_OWNER_MAX];
    struct HookNode *next;
} HookNode;

typedef struct {
    u32       guest;
    RecompFn  orig;
    int       used;
    int       depth;            /* >0 while a dispatch is on the stack */
    HookNode *before;
    HookNode *after;
} Site;

static Site s_sites[ACX_MAX_SITES];
static int  s_next_handle = 1;

static Site *site_for(u32 addr, int *out_index)
{
    int i, freeslot = -1;
    for (i = 0; i < ACX_MAX_SITES; i++) {
        if (s_sites[i].used && s_sites[i].guest == addr) {
            if (out_index) *out_index = i;
            return &s_sites[i];
        }
        if (!s_sites[i].used && freeslot < 0) freeslot = i;
    }
    if (freeslot < 0) return NULL;
    if (out_index) *out_index = freeslot;
    return &s_sites[freeslot];
}

/* Run one chain.  The next pointer is read before the handler runs, so a
 * one-shot that unhooks itself does not walk into freed memory. */
static void run_chain(HookNode *n, AcxCtx *ctx)
{
    while (n) {
        HookNode *next = n->next;
        ctx->user = n->user;
        n->fn(ctx);
        n = next;
    }
}

static void site_dispatch(int index, CpuCtx *c, u8 *ram)
{
    Site   *st = &s_sites[index];
    AcxCtx  ctx;
    CpuCtx *saved_cpu = s_cpu;
    u8     *saved_ram = s_ram;
    int     i;

    s_cpu = c;
    s_ram = ram;
    st->depth++;

    ctx.cpu  = c;
    ctx.ram  = ram;
    ctx.addr = st->guest;
    ctx.when = ACX_BEFORE;
    ctx.skip = 0;
    ctx.user = NULL;
    for (i = 0; i < 4; i++) ctx.arg[i] = c->r[R_A0 + i];
    for (i = 0; i < 8; i++) ctx.farg[i] = c->f[12 + i];

    run_chain(st->before, &ctx);

    if (!ctx.skip && st->orig) st->orig(c, ram);

    if (st->after) {
        ctx.when = ACX_AFTER;
        ctx.user = NULL;
        run_chain(st->after, &ctx);
    }

    st->depth--;
    s_cpu = saved_cpu;
    s_ram = saved_ram;
}

/* One trampoline per site; see the file header for the hex indexing. */
#define ACX_TRAMP_ROW(M, p) \
    M(p##0) M(p##1) M(p##2) M(p##3) M(p##4) M(p##5) M(p##6) M(p##7) \
    M(p##8) M(p##9) M(p##a) M(p##b) M(p##c) M(p##d) M(p##e) M(p##f)
#define ACX_TRAMP_ALL(M) \
    ACX_TRAMP_ROW(M, 0x0) ACX_TRAMP_ROW(M, 0x1) \
    ACX_TRAMP_ROW(M, 0x2) ACX_TRAMP_ROW(M, 0x3) \
    ACX_TRAMP_ROW(M, 0x4) ACX_TRAMP_ROW(M, 0x5) \
    ACX_TRAMP_ROW(M, 0x6) ACX_TRAMP_ROW(M, 0x7) \
    ACX_TRAMP_ROW(M, 0x8) ACX_TRAMP_ROW(M, 0x9) \
    ACX_TRAMP_ROW(M, 0xa) ACX_TRAMP_ROW(M, 0xb) \
    ACX_TRAMP_ROW(M, 0xc) ACX_TRAMP_ROW(M, 0xd) \
    ACX_TRAMP_ROW(M, 0xe) ACX_TRAMP_ROW(M, 0xf)

#define ACX_TRAMP_DEF(n) \
    static void acx_tramp_##n(CpuCtx *c, u8 *ram) { site_dispatch((n), c, ram); }
#define ACX_TRAMP_REF(n) acx_tramp_##n,

ACX_TRAMP_ALL(ACX_TRAMP_DEF)

static const RecompFn s_tramps[ACX_MAX_SITES] = { ACX_TRAMP_ALL(ACX_TRAMP_REF) };

static int api_hookable(u32 addr)
{
    return hook_index(addr) >= 0;
}

static int api_hook(u32 addr, AcxHookFn fn, void *user, int when,
                    const char *owner)
{
    Site *st;
    HookNode *n, **list;
    int index = -1;

    if (!fn) return -1;
    st = site_for(addr, &index);
    if (!st) {
        fprintf(stderr, "[acx] no free hook sites (%d distinct addresses); "
                "raise ACX_MAX_SITES\n", ACX_MAX_SITES);
        return -1;
    }
    if (!st->used) {
        int rc = hook_install(addr, s_tramps[index], &st->orig);
        if (rc != HOOK_OK) return rc;
        st->guest  = addr;
        st->before = NULL;
        st->after  = NULL;
        st->depth  = 0;
        st->used   = 1;
    }
    /* Already hooked: append.  Taking the original again would capture the
     * trampoline and the real function would never run. */

    n = (HookNode *)calloc(1, sizeof *n);
    if (!n) return -1;
    n->id   = s_next_handle++;
    n->fn   = fn;
    n->user = user;
    snprintf(n->owner, sizeof n->owner, "%s", owner ? owner : "?");

    list = (when == ACX_AFTER) ? &st->after : &st->before;
    while (*list) list = &(*list)->next;    /* registration order */
    *list = n;
    return n->id;
}

/* Remove one node.  The site keeps its trampoline even when both chains empty:
 * restoring the original would race a dispatch already on the stack, and an
 * empty site costs one indirect call.  The next hook on the same address
 * reclaims the slot. */
static int unlink_node(HookNode **list, int id)
{
    while (*list) {
        if ((*list)->id == id) {
            HookNode *dead = *list;
            *list = dead->next;
            free(dead);
            return 1;
        }
        list = &(*list)->next;
    }
    return 0;
}

static int api_unhook(int handle)
{
    int i;
    for (i = 0; i < ACX_MAX_SITES; i++) {
        if (!s_sites[i].used) continue;
        if (unlink_node(&s_sites[i].before, handle)) return 1;
        if (unlink_node(&s_sites[i].after,  handle)) return 1;
    }
    return 0;
}

static void unlink_owner(HookNode **list, const char *owner)
{
    while (*list) {
        if (strcmp((*list)->owner, owner) == 0) {
            HookNode *dead = *list;
            *list = dead->next;
            free(dead);
            continue;
        }
        list = &(*list)->next;
    }
}

static void api_unhook_owner(const char *owner)
{
    int i;
    if (!owner) return;
    for (i = 0; i < ACX_MAX_SITES; i++) {
        if (!s_sites[i].used) continue;
        unlink_owner(&s_sites[i].before, owner);
        unlink_owner(&s_sites[i].after,  owner);
    }
}

/* ------------------------------------------------------------------ */
/* calling into the game                                                */
/* ------------------------------------------------------------------ */

static void api_invoke_init(AcxInvoke *iv)
{
    memset(iv, 0, sizeof *iv);
    if (s_cpu) iv->cpu = *s_cpu;
}

/* o32: the caller reserves a 16-byte home area for a0..a3 and puts the fifth
 * argument onward at sp+16.  SP moves below the interrupted frame so the
 * callee's frame lands in unused stack. */
static u32 api_invoke(AcxInvoke *iv, u32 addr)
{
    RecompFn f;
    u8 *ram = cur_ram();
    u32 sp, need;
    int i, n;

    if (!iv) return 0;
    if (!s_cpu) {
        fprintf(stderr, "[acx] invoke(%08x) before there is a guest context\n",
                (unsigned)addr);
        return 0;
    }
    f = recomp_lookup(addr);
    if (!f) {
        fprintf(stderr, "[acx] invoke(%08x): not a recompiled function\n",
                (unsigned)addr);
        return 0;
    }

    n = iv->nstack;
    if (n < 0) n = 0;
    if (n > (int)(sizeof iv->stack / sizeof iv->stack[0]))
        n = (int)(sizeof iv->stack / sizeof iv->stack[0]);

    need = 16u + 4u * (u32)n;
    sp = (iv->cpu.r[R_SP] - need) & ~7u;
    for (i = 0; i < n; i++) mem_w32(ram, sp + 16u + 4u * (u32)i, iv->stack[i]);

    iv->cpu.r[R_SP] = sp;
    iv->cpu.r[R_RA] = 0;                    /* recompiled bodies just return */
    f(&iv->cpu, ram);
    return iv->cpu.r[R_V0];
}

static u32 api_call(u32 addr, int nargs, const u32 *args)
{
    AcxInvoke iv;
    int i;

    api_invoke_init(&iv);
    if (nargs < 0) nargs = 0;
    if (nargs > 8) nargs = 8;
    for (i = 0; i < nargs && i < 4; i++) iv.cpu.r[R_A0 + i] = args[i];
    for (i = 4; i < nargs; i++) iv.stack[i - 4] = args[i];
    iv.nstack = nargs > 4 ? nargs - 4 : 0;
    return api_invoke(&iv, addr);
}

/* ------------------------------------------------------------------ */
/* events                                                               */
/* ------------------------------------------------------------------ */

typedef struct EventNode {
    int               id;
    char              name[32];
    char              owner[ACX_OWNER_MAX];
    AcxEventFn        fn;
    void             *user;
    struct EventNode *next;
} EventNode;

static EventNode *s_events;

static int api_on(const char *event, AcxEventFn fn, void *user,
                  const char *owner)
{
    EventNode *n, **tail = &s_events;
    if (!event || !fn) return -1;
    n = (EventNode *)calloc(1, sizeof *n);
    if (!n) return -1;
    n->id   = s_next_handle++;
    n->fn   = fn;
    n->user = user;
    snprintf(n->name,  sizeof n->name,  "%s", event);
    snprintf(n->owner, sizeof n->owner, "%s", owner ? owner : "?");
    while (*tail) tail = &(*tail)->next;
    *tail = n;
    return n->id;
}

static int api_off(int handle)
{
    EventNode **p = &s_events;
    while (*p) {
        if ((*p)->id == handle) {
            EventNode *dead = *p;
            *p = dead->next;
            free(dead);
            return 1;
        }
        p = &(*p)->next;
    }
    return 0;
}

static void api_off_owner(const char *owner)
{
    EventNode **p = &s_events;
    if (!owner) return;
    while (*p) {
        if (strcmp((*p)->owner, owner) == 0) {
            EventNode *dead = *p;
            *p = dead->next;
            free(dead);
            continue;
        }
        p = &(*p)->next;
    }
}

void acx_event_fire(const AcxEvent *e)
{
    EventNode *n = s_events;
    AcxEvent   local;

    if (!e) return;
    while (n) {
        EventNode *next = n->next;           /* a handler may unregister */
        if (strcmp(n->name, e->name) == 0) {
            local = *e;
            local.user = n->user;
            n->fn(&local);
        }
        n = next;
    }
}

void acx_event_fire3(const char *name, u32 a, u32 b, u32 c, const char *s)
{
    AcxEvent e;
    memset(&e, 0, sizeof e);
    e.name = name;
    e.a = a; e.b = b; e.c = c;
    e.s = s;
    acx_event_fire(&e);
}

/* ------------------------------------------------------------------ */
/* odds and ends                                                        */
/* ------------------------------------------------------------------ */

static void api_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static u64 api_frame(void)       { return s_frames; }
static int api_moddable(void)    { return hook_available(); }
static u32 api_module_base(void) { return PSPRECO_MODULE_BASE; }

/* o32 register names, in register-number order.  r30 is `fp` here and `s8` in
 * some assemblers; acx_gpr_index accepts both. */
static const char *const k_gpr[32] = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"
};

const char *acx_gpr_name(int i)
{
    return (i >= 0 && i < 32) ? k_gpr[i] : NULL;
}

int acx_gpr_index(const char *name)
{
    int i;
    if (!name) return -1;
    if (strcmp(name, "s8") == 0) return 30;
    for (i = 0; i < 32; i++)
        if (strcmp(name, k_gpr[i]) == 0) return i;
    return -1;
}

/* ------------------------------------------------------------------ */
/* the table                                                            */
/* ------------------------------------------------------------------ */

static const AcxHostApi s_api = {
    ACX_API_VERSION,

    api_ram,
    api_r8, api_r16, api_r32,
    api_rs8, api_rs16, api_rs32,
    api_rf32,
    api_w8, api_w16, api_w32, api_wf32,
    api_rstr, api_rbytes, api_wbytes, api_memset,

    api_alloc, api_calloc, api_free, api_block_size, api_cstr,

    api_sym, api_sym_addr, api_name_at,

    api_hook, api_unhook, api_unhook_owner, api_hookable,

    api_invoke_init, api_invoke, api_call,

    api_on, api_off, api_off_owner,

    api_log, api_frame, api_moddable, api_module_base
};

const AcxHostApi *acx_api(void) { return &s_api; }

void acx_api_init(void)
{
    if (s_inited) return;
    s_inited = 1;
    ghp_init();
    build_sym_index();
}

void acx_api_tick(CpuCtx *c, u8 *ram)
{
    acx_api_set_context(c, ram);
    s_frames++;
    acx_event_fire3("frame", (u32)s_frames, 0, 0, NULL);
}
