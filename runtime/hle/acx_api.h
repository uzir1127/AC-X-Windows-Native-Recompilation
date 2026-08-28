/* The surface a mod is written against.
 *
 * Lua scripts and native plugins want the same capabilities, so they are
 * implemented once here: script.c is a binding over this table and holds no
 * engine knowledge of its own, and a native plugin gets the same table.
 *
 * A handler sees the whole machine -- all 32 GPRs, all 32 FPRs, HI/LO and the
 * 128 VFPU words, readable and writable, before and after the original runs.
 * The flight model needs that: its functions take arguments in f12 upward, so
 * an API limited to a0..a3 could not reach them.
 *
 * ACX_API_VERSION is major<<16 | minor.  A plugin refuses to load when the
 * majors differ; a minor bump only appends to the end of AcxHostApi.
 */

#ifndef PSPRECO_ACX_API_H
#define PSPRECO_ACX_API_H

#include "pspreco.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ACX_API_VERSION 0x00010000u
#define ACX_API_MAJOR(v) ((v) >> 16)

/* ------------------------------------------------------------------ */
/* the machine, as a handler sees it                                   */
/* ------------------------------------------------------------------ */

enum { ACX_BEFORE = 0, ACX_AFTER = 1 };

typedef struct AcxCtx {
    CpuCtx *cpu;        /* the live context: every register, read and write */
    u8     *ram;        /* the flat guest window                            */
    u32     addr;       /* the guest function that was hooked               */

    /* a0..a3 are not preserved across a call, so an after-handler reading
     * the live context sees whatever the function left behind.  Snapshotted
     * at entry. */
    u32     arg[4];
    f32     farg[8];    /* f12..f19 at entry, same reason */

    int     when;       /* ACX_BEFORE or ACX_AFTER */
    int     skip;       /* a before-handler sets this to suppress the original */
    void   *user;       /* whatever was passed to hook() */
} AcxCtx;

typedef void (*AcxHookFn)(AcxCtx *ctx);

/* ------------------------------------------------------------------ */
/* calling into the game                                               */
/* ------------------------------------------------------------------ */

/* Seeded from the live context, then handed to invoke().  This is the whole
 * CpuCtx rather than an argument list because the engine uses o32 float
 * arguments, VFPU arguments and struct returns, each of which an argument
 * list would have to model separately.
 *
 * `stack` is the argument words past the fourth, which o32 puts at sp+16
 * upward.  invoke() reserves the 16-byte home area and repoints SP below the
 * caller's frame, so nothing live is overwritten. */
typedef struct AcxInvoke {
    CpuCtx cpu;
    u32    stack[16];
    int    nstack;
} AcxInvoke;

/* ------------------------------------------------------------------ */
/* symbols                                                             */
/* ------------------------------------------------------------------ */

typedef struct AcxSymInfo {
    const char *name;
    u32         addr;
    u32         size;
    int         is_func;
    const char *hook;        /* indirect|mixed|direct|orphan, "" for a var */
    int         recompiled;
} AcxSymInfo;

/* ------------------------------------------------------------------ */
/* events                                                              */
/* ------------------------------------------------------------------ */

/* One payload shape for every event, so adding an event is a fire() call
 * rather than a change to this header.  Field meanings are per event and
 * documented at the acx_event_fire call sites. */
typedef struct AcxEvent {
    const char *name;
    u32         a, b, c;
    f32         x, y, z;
    const char *s;
    void       *user;
} AcxEvent;

typedef void (*AcxEventFn)(const AcxEvent *e);

/* ------------------------------------------------------------------ */
/* the table                                                           */
/* ------------------------------------------------------------------ */

typedef struct AcxHostApi {
    u32 version;

    /* -- guest memory ------------------------------------------------ */
    u8 *(*ram)(void);
    u32  (*r8 )(u32 a);
    u32  (*r16)(u32 a);
    u32  (*r32)(u32 a);
    s32  (*rs8 )(u32 a);
    s32  (*rs16)(u32 a);
    s32  (*rs32)(u32 a);
    f32  (*rf32)(u32 a);
    void (*w8 )(u32 a, u32 v);
    void (*w16)(u32 a, u32 v);
    void (*w32)(u32 a, u32 v);
    void (*wf32)(u32 a, f32 v);
    int  (*rstr)(u32 a, char *out, int max);        /* returns length */
    void (*rbytes)(u32 a, void *out, u32 n);
    void (*wbytes)(u32 a, const void *in, u32 n);
    void (*memset)(u32 a, int byte, u32 n);

    /* -- the mod heap ------------------------------------------------ */
    u32  (*alloc)(u32 n);
    u32  (*calloc)(u32 n);
    void (*free)(u32 a);
    u32  (*block_size)(u32 a);
    u32  (*cstr)(const char *s);       /* copy a C string into guest memory */

    /* -- symbols ----------------------------------------------------- */
    int  (*sym)(const char *name, AcxSymInfo *out);
    u32  (*sym_addr)(const char *name);             /* 0 when unknown */
    const char *(*name_at)(u32 addr, u32 *offset);  /* NULL when unknown */

    /* -- hooks ------------------------------------------------------- */
    /* `when` is ACX_BEFORE or ACX_AFTER.  Returns a handle, or < 0.
     * Handlers chain in registration order; the original runs once, between
     * the before list and the after list. */
    int  (*hook)(u32 addr, AcxHookFn fn, void *user, int when,
                 const char *owner);
    int  (*unhook)(int handle);
    void (*unhook_owner)(const char *owner);
    int  (*hookable)(u32 addr);

    /* -- calling the game -------------------------------------------- */
    void (*invoke_init)(AcxInvoke *iv);
    u32  (*invoke)(AcxInvoke *iv, u32 addr);        /* returns v0 */
    u32  (*call)(u32 addr, int nargs, const u32 *args);   /* up to 8 words */

    /* -- events ------------------------------------------------------ */
    int  (*on)(const char *event, AcxEventFn fn, void *user,
               const char *owner);
    int  (*off)(int handle);
    void (*off_owner)(const char *owner);

    /* -- odds and ends ----------------------------------------------- */
    void (*log)(const char *fmt, ...);
    u64  (*frame)(void);
    int  (*moddable)(void);
    u32  (*module_base)(void);
} AcxHostApi;

/* The table, and the entry point a native plugin exports:
 *
 *     __declspec(dllexport) int acx_plugin_init(const AcxHostApi *api);
 *
 * returning 0 on success. */
const AcxHostApi *acx_api(void);
typedef int (*AcxPluginInit)(const AcxHostApi *api);

/* ------------------------------------------------------------------ */
/* used by the runtime, not by mods                                    */
/* ------------------------------------------------------------------ */

void acx_api_init(void);
void acx_api_set_context(CpuCtx *c, u8 *ram);   /* the tick's context */
void acx_api_tick(CpuCtx *c, u8 *ram);          /* fires "frame" */

/* Fire a named event at every handler registered for it.  An unknown name is
 * not an error in either direction. */
void acx_event_fire(const AcxEvent *e);
void acx_event_fire3(const char *name, u32 a, u32 b, u32 c, const char *s);

/* Register names, for the scripting binding and the debug UI.  Index is the
 * o32 register number; NULL past 31. */
const char *acx_gpr_name(int i);
int         acx_gpr_index(const char *name);    /* -1 when it is not one */

#ifdef __cplusplus
}
#endif
#endif /* PSPRECO_ACX_API_H */
