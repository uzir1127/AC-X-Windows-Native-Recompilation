/* The Lua binding over the host API.
 *
 * No engine knowledge lives here: every capability is a call into acx_api.c,
 * the same table a native plugin receives.  What is Lua-shaped -- marshalling,
 * per-script environments, reload -- lives here.
 *
 * `ctx` is a userdata over the live CpuCtx rather than a snapshot, so reading
 * `ctx.a0` reads the register and writing it writes the register before the
 * original runs.  All 32 GPRs by o32 name, all 32 FPRs as `f0`..`f31`, HI/LO,
 * and the VFPU through ctx:vfpu().  The flight model takes its arguments in
 * f12 upward, so the float registers are not optional.
 *
 * `ctx.arg0`..`arg3` and `ctx.farg0`..`farg7` are the values at entry, since
 * argument registers are not preserved across a call and an after-handler
 * reading `ctx.a0` would see whatever the function left behind.
 *
 * Names resolve through generated/acx_symtab.c, emitted by acxsdk.py from the
 * IDA database, so scripts survive a regenerated or retargeted symbol set.
 * Raw addresses are also accepted, since new work starts without a name.
 *
 * One Lua state, one environment per script, falling back to the real globals
 * for reads: an accidental global cannot break another script, while a shared
 * state still lets one mod publish a table another reads.
 *
 * The standard library is opened whole, `io` and `os` included.  A script is
 * as trusted as the native plugin it would otherwise have to be.
 */

#include "script.h"
#include "acx_api.h"
#include "guestheap.h"
#include "hooks.h"
#include "hle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dirent.h>
#endif

#define SCRIPT_MAX           64
#define SCRIPT_MAX_BINDINGS 512
#define SCRIPT_RELOAD_FRAMES 30      /* twice a second is plenty for editing */
#define CTX_MT "acx.ctx"

typedef struct {
    char path[1024];
    char name[128];
    long mtime;
    int  env_ref;
    int  ok;
} Script;

/* A hook or event registration made from Lua.  The Lua ref is owned here so a
 * reload can drop exactly this script's registrations; acx_api.c frees its own
 * node but cannot unref a Lua function. */
typedef struct {
    int  handle;                 /* from api->hook / api->on; 0 = free slot */
    int  ref;                    /* the Lua function */
    int  is_event;
    char owner[128];
} Binding;

static const AcxHostApi *A;
static lua_State  *L;
static Script      s_scripts[SCRIPT_MAX];
static int         s_script_count;
static Binding     s_bind[SCRIPT_MAX_BINDINGS];
static int         s_ready_ref = LUA_NOREF;
static int         s_ready_pending;
static const char *s_loading;
static int         s_on;

/* ------------------------------------------------------------------ */
/* addresses and symbols                                               */
/* ------------------------------------------------------------------ */

/* A script may pass a name or an address.  An unresolved name raises rather
 * than returning 0, which would become a write to guest address 0. */
static u32 arg_addr(lua_State *S, int idx)
{
    if (lua_type(S, idx) == LUA_TSTRING) {
        const char *n = lua_tostring(S, idx);
        u32 a = A->sym_addr(n);
        if (!a) luaL_error(S, "unknown symbol '%s'", n);
        return a;
    }
    return (u32)luaL_checkinteger(S, idx);
}

/* ------------------------------------------------------------------ */
/* memory                                                              */
/* ------------------------------------------------------------------ */

#define RD(NAME, CALL)                                                    \
    static int l_##NAME(lua_State *S) {                                   \
        u32 a = arg_addr(S, 1);                                           \
        lua_pushinteger(S, (lua_Integer)(CALL));                          \
        return 1; }

RD(r8,   A->r8(a))
RD(r16,  A->r16(a))
RD(r32,  (lua_Integer)(u32)A->r32(a))
RD(rs8,  A->rs8(a))
RD(rs16, A->rs16(a))
RD(rs32, A->rs32(a))

static int l_rf32(lua_State *S)
{
    lua_pushnumber(S, (lua_Number)A->rf32(arg_addr(S, 1)));
    return 1;
}

#define WR(NAME, CALL)                                                    \
    static int l_##NAME(lua_State *S) {                                   \
        u32 a = arg_addr(S, 1);                                           \
        u32 v = (u32)luaL_checkinteger(S, 2);                             \
        CALL; return 0; }

WR(w8,  A->w8(a, v))
WR(w16, A->w16(a, v))
WR(w32, A->w32(a, v))

static int l_wf32(lua_State *S)
{
    u32 a = arg_addr(S, 1);
    A->wf32(a, (f32)luaL_checknumber(S, 2));
    return 0;
}

static int l_rstr(lua_State *S)
{
    u32 a = arg_addr(S, 1);
    int max = (int)luaL_optinteger(S, 2, 256), n;
    char buf[4096];
    if (max > (int)sizeof buf) max = (int)sizeof buf;
    n = A->rstr(a, buf, max);
    lua_pushlstring(S, buf, (size_t)n);
    return 1;
}

static int l_rbytes(lua_State *S)
{
    u32 a = arg_addr(S, 1);
    lua_Integer n = luaL_checkinteger(S, 2);
    char *buf;
    if (n < 0 || n > (1 << 24)) return luaL_error(S, "silly length %d", (int)n);
    buf = (char *)malloc((size_t)n + 1u);
    if (!buf) return luaL_error(S, "out of memory");
    A->rbytes(a, buf, (u32)n);
    lua_pushlstring(S, buf, (size_t)n);
    free(buf);
    return 1;
}

static int l_wbytes(lua_State *S)
{
    u32 a = arg_addr(S, 1);
    size_t n = 0;
    const char *s = luaL_checklstring(S, 2, &n);
    A->wbytes(a, s, (u32)n);
    lua_pushinteger(S, (lua_Integer)n);
    return 1;
}

static int l_memset(lua_State *S)
{
    u32 a = arg_addr(S, 1);
    int byte = (int)luaL_checkinteger(S, 2);
    lua_Integer n = luaL_checkinteger(S, 3);
    if (n < 0) return luaL_error(S, "negative length");
    A->memset(a, byte, (u32)n);
    return 0;
}

/* ------------------------------------------------------------------ */
/* the mod heap                                                        */
/* ------------------------------------------------------------------ */

static int l_alloc(lua_State *S)
{
    u32 a = A->alloc((u32)luaL_checkinteger(S, 1));
    if (!a) return luaL_error(S, "the mod heap is full");
    lua_pushinteger(S, (lua_Integer)a);
    return 1;
}

static int l_calloc(lua_State *S)
{
    u32 a = A->calloc((u32)luaL_checkinteger(S, 1));
    if (!a) return luaL_error(S, "the mod heap is full");
    lua_pushinteger(S, (lua_Integer)a);
    return 1;
}

static int l_free(lua_State *S)
{
    A->free((u32)luaL_checkinteger(S, 1));
    return 0;
}

/* A Lua string is not in guest memory, and the engine looks names up out of
 * guest memory. */
static int l_cstr(lua_State *S)
{
    size_t n = 0;
    const char *s = luaL_checklstring(S, 1, &n);
    u32 a = A->alloc((u32)n + 1u);
    if (!a) return luaL_error(S, "the mod heap is full");
    A->wbytes(a, s, (u32)n);
    A->w8(a + (u32)n, 0);
    lua_pushinteger(S, (lua_Integer)a);
    return 1;
}

static int l_heap(lua_State *S)
{
    u32 used = 0, freeb = 0;
    int blocks = 0;
    ghp_stats(&used, &freeb, &blocks);
    lua_newtable(S);
    lua_pushinteger(S, (lua_Integer)used);   lua_setfield(S, -2, "used");
    lua_pushinteger(S, (lua_Integer)freeb);  lua_setfield(S, -2, "free");
    lua_pushinteger(S, blocks);              lua_setfield(S, -2, "blocks");
    lua_pushinteger(S, (lua_Integer)MOD_HEAP_BASE); lua_setfield(S, -2, "base");
    return 1;
}

/* ------------------------------------------------------------------ */
/* symbols                                                             */
/* ------------------------------------------------------------------ */

static int l_sym(lua_State *S)
{
    AcxSymInfo si;
    if (!A->sym(luaL_checkstring(S, 1), &si)) { lua_pushnil(S); return 1; }
    lua_newtable(S);
    lua_pushinteger(S, (lua_Integer)si.addr); lua_setfield(S, -2, "addr");
    lua_pushinteger(S, (lua_Integer)si.size); lua_setfield(S, -2, "size");
    lua_pushstring(S, si.is_func ? "func" : "var"); lua_setfield(S, -2, "kind");
    lua_pushstring(S, si.hook);               lua_setfield(S, -2, "hook");
    lua_pushboolean(S, si.recompiled);        lua_setfield(S, -2, "recompiled");
    return 1;
}

static int l_name_at(lua_State *S)
{
    u32 off = 0;
    const char *n = A->name_at((u32)luaL_checkinteger(S, 1), &off);
    if (!n) { lua_pushnil(S); return 1; }
    lua_pushstring(S, n);
    lua_pushinteger(S, (lua_Integer)off);
    return 2;
}

static int l_hookable(lua_State *S)
{
    lua_pushboolean(S, A->hookable(arg_addr(S, 1)));
    return 1;
}

/* ------------------------------------------------------------------ */
/* ctx -- the machine, live                                            */
/* ------------------------------------------------------------------ */

/* "f12" and friends.  acx_gpr_index is tried first so `fp` stays r30 rather
 * than parsing as float register `p`. */
static int fpr_index(const char *k)
{
    int n = 0;
    if (k[0] != 'f' || !k[1]) return -1;
    for (k++; *k; k++) {
        if (*k < '0' || *k > '9') return -1;
        n = n * 10 + (*k - '0');
        if (n > 31) return -1;
    }
    return n;
}

/* "arg0".."arg3" / "farg0".."farg7" -- the entry snapshot. */
static int suffix_index(const char *k, const char *pre, int limit)
{
    size_t n = strlen(pre);
    int v;
    if (strncmp(k, pre, n) != 0) return -1;
    k += n;
    if (!k[0] || k[1]) return -1;
    if (k[0] < '0' || k[0] > '9') return -1;
    v = k[0] - '0';
    return v < limit ? v : -1;
}

static AcxCtx *check_ctx(lua_State *S)
{
    AcxCtx **p = (AcxCtx **)luaL_checkudata(S, 1, CTX_MT);
    if (!*p) luaL_error(S, "this ctx is no longer live");
    return *p;
}

static int l_ctx_index(lua_State *S)
{
    AcxCtx *c = check_ctx(S);
    const char *k = luaL_checkstring(S, 2);
    int i;

    if ((i = acx_gpr_index(k)) >= 0) {
        lua_pushinteger(S, (lua_Integer)(u32)c->cpu->r[i]);
        return 1;
    }
    if ((i = fpr_index(k)) >= 0) {
        lua_pushnumber(S, (lua_Number)c->cpu->f[i]);
        return 1;
    }
    if ((i = suffix_index(k, "farg", 8)) >= 0) {
        lua_pushnumber(S, (lua_Number)c->farg[i]);
        return 1;
    }
    if ((i = suffix_index(k, "arg", 4)) >= 0) {
        lua_pushinteger(S, (lua_Integer)(u32)c->arg[i]);
        return 1;
    }
    if (strcmp(k, "addr") == 0) { lua_pushinteger(S, (lua_Integer)c->addr); return 1; }
    if (strcmp(k, "hi") == 0)   { lua_pushinteger(S, (lua_Integer)c->cpu->hi); return 1; }
    if (strcmp(k, "lo") == 0)   { lua_pushinteger(S, (lua_Integer)c->cpu->lo); return 1; }
    if (strcmp(k, "when") == 0) {
        lua_pushstring(S, c->when == ACX_AFTER ? "after" : "before");
        return 1;
    }
    /* not a register: the methods table, kept as an upvalue */
    lua_pushvalue(S, 2);
    lua_gettable(S, lua_upvalueindex(1));
    return 1;
}

static int l_ctx_newindex(lua_State *S)
{
    AcxCtx *c = check_ctx(S);
    const char *k = luaL_checkstring(S, 2);
    int i;

    if ((i = acx_gpr_index(k)) >= 0) {
        /* r0 is hardwired zero and the emitter never writes it, so accepting
         * a write here would only look like it worked. */
        if (i == 0) return luaL_error(S, "$zero is hardwired");
        c->cpu->r[i] = (u32)luaL_checkinteger(S, 3);
        return 0;
    }
    if ((i = fpr_index(k)) >= 0) {
        c->cpu->f[i] = (f32)luaL_checknumber(S, 3);
        return 0;
    }
    if (strcmp(k, "hi") == 0) { c->cpu->hi = (u32)luaL_checkinteger(S, 3); return 0; }
    if (strcmp(k, "lo") == 0) { c->cpu->lo = (u32)luaL_checkinteger(S, 3); return 0; }
    return luaL_error(S, "cannot set '%s' on a ctx", k);
}

static int l_ctx_skip(lua_State *S)
{
    AcxCtx *c = check_ctx(S);
    c->skip = 1;
    return 0;
}

static int l_ctx_vfpu(lua_State *S)
{
    AcxCtx *c = check_ctx(S);
    lua_Integer n = luaL_checkinteger(S, 2);
    if (n < 0 || n > 127) return luaL_error(S, "vfpu register %d", (int)n);
    if (lua_gettop(S) >= 3) {                       /* setter */
        c->cpu->v[n] = (u32)luaL_checkinteger(S, 3);
        return 0;
    }
    lua_pushinteger(S, (lua_Integer)c->cpu->v[n]);
    return 1;
}

/* The same word read as a float. */
static int l_ctx_vfpuf(lua_State *S)
{
    AcxCtx *c = check_ctx(S);
    lua_Integer n = luaL_checkinteger(S, 2);
    if (n < 0 || n > 127) return luaL_error(S, "vfpu register %d", (int)n);
    if (lua_gettop(S) >= 3) {
        c->cpu->v[n] = f2u((f32)luaL_checknumber(S, 3));
        return 0;
    }
    lua_pushnumber(S, (lua_Number)u2f(c->cpu->v[n]));
    return 1;
}

static int l_ctx_reg(lua_State *S)
{
    AcxCtx *c = check_ctx(S);
    lua_Integer n = luaL_checkinteger(S, 2);
    if (n < 0 || n > 31) return luaL_error(S, "register %d", (int)n);
    if (lua_gettop(S) >= 3) {
        if (n == 0) return luaL_error(S, "$zero is hardwired");
        c->cpu->r[n] = (u32)luaL_checkinteger(S, 3);
        return 0;
    }
    lua_pushinteger(S, (lua_Integer)(u32)c->cpu->r[n]);
    return 1;
}

static const luaL_Reg ctx_methods[] = {
    { "skip",  l_ctx_skip },
    { "vfpu",  l_ctx_vfpu },
    { "vfpuf", l_ctx_vfpuf },
    { "reg",   l_ctx_reg },
    { NULL, NULL }
};

static void ctx_register(lua_State *S)
{
    luaL_newmetatable(S, CTX_MT);

    luaL_newlib(S, ctx_methods);            /* the methods table */
    lua_pushvalue(S, -1);                   /* one copy becomes the upvalue */
    lua_pushcclosure(S, l_ctx_index, 1);
    lua_setfield(S, -3, "__index");
    lua_pop(S, 1);                          /* the methods table itself */

    lua_pushcfunction(S, l_ctx_newindex);
    lua_setfield(S, -2, "__newindex");

    lua_pushstring(S, "acx ctx");
    lua_setfield(S, -2, "__name");

    lua_pop(S, 1);
}

static void push_ctx(lua_State *S, AcxCtx *c)
{
    AcxCtx **p = (AcxCtx **)lua_newuserdatauv(S, sizeof *p, 0);
    *p = c;
    luaL_setmetatable(S, CTX_MT);
}

/* ------------------------------------------------------------------ */
/* hooks and events                                                    */
/* ------------------------------------------------------------------ */

static Binding *binding_alloc(void)
{
    int i;
    for (i = 0; i < SCRIPT_MAX_BINDINGS; i++)
        if (!s_bind[i].handle) return &s_bind[i];
    return NULL;
}

static Binding *binding_by_handle(int handle)
{
    int i;
    for (i = 0; i < SCRIPT_MAX_BINDINGS; i++)
        if (s_bind[i].handle == handle) return &s_bind[i];
    return NULL;
}

/* A handler that throws is reported and the rest still run, so one broken mod
 * does not silence a working one. */
static void hook_thunk(AcxCtx *c)
{
    Binding *b = (Binding *)c->user;
    int rc;

    if (!b || !L) return;
    push_ctx(L, c);                                 /* [ud] */
    lua_rawgeti(L, LUA_REGISTRYINDEX, b->ref);      /* [ud][fn] */
    lua_pushvalue(L, -2);                           /* [ud][fn][ud] */
    rc = lua_pcall(L, 1, 1, 0);                     /* [ud][result|err] */
    if (rc != LUA_OK) {
        printf("[lua] hook at %08x (%s) failed: %s\n", (unsigned)c->addr,
               b->owner, lua_tostring(L, -1));
    } else if (lua_type(L, -1) == LUA_TBOOLEAN && !lua_toboolean(L, -1)) {
        /* Returning false suppresses the original for the whole chain, so it
         * affects every mod hooking this address. */
        c->skip = 1;
    }
    lua_pop(L, 1);                                  /* [ud] */
    /* ctx->cpu points at a CpuCtx about to leave the stack.  A handler that
     * stashed the userdata then gets an error from check_ctx rather than a
     * write into a dead frame. */
    *(AcxCtx **)lua_touserdata(L, -1) = NULL;
    lua_pop(L, 1);
}

static void event_thunk(const AcxEvent *e)
{
    Binding *b = (Binding *)e->user;
    int nargs = 0;
    if (!b || !L) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, b->ref);
    if (strcmp(e->name, "register") == 0) {
        lua_pushstring(L, e->s ? e->s : "");
        lua_pushinteger(L, (lua_Integer)(s32)e->a);
        lua_pushinteger(L, (lua_Integer)e->b);
        nargs = 3;
    } else {
        lua_pushinteger(L, (lua_Integer)e->a);
        nargs = 1;
    }
    if (lua_pcall(L, nargs, 0, 0) != LUA_OK) {
        printf("[lua] %s handler (%s) failed: %s\n", e->name, b->owner,
               lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

static int hook_common(lua_State *S, int when)
{
    u32 addr = arg_addr(S, 1);
    Binding *b;
    int handle;

    luaL_checktype(S, 2, LUA_TFUNCTION);
    b = binding_alloc();
    if (!b) return luaL_error(S, "too many registrations (max %d)",
                              SCRIPT_MAX_BINDINGS);
    lua_pushvalue(S, 2);
    b->ref = luaL_ref(S, LUA_REGISTRYINDEX);
    b->is_event = 0;
    snprintf(b->owner, sizeof b->owner, "%s", s_loading ? s_loading : "?");

    handle = A->hook(addr, hook_thunk, b, when, b->owner);
    if (handle < 0) {
        luaL_unref(S, LUA_REGISTRYINDEX, b->ref);
        memset(b, 0, sizeof *b);
        return luaL_error(S, "cannot hook %08x: %s", (unsigned)addr,
                          handle == HOOK_ENOTMODDABLE
                              ? "this build routes no direct calls"
                              : "not a recompiled function");
    }
    b->handle = handle;
    lua_pushinteger(S, handle);
    return 1;
}

static int l_hook(lua_State *S)       { return hook_common(S, ACX_BEFORE); }
static int l_hook_after(lua_State *S) { return hook_common(S, ACX_AFTER); }

static int l_unhook(lua_State *S)
{
    int handle = (int)luaL_checkinteger(S, 1);
    Binding *b = binding_by_handle(handle);
    int ok = A->unhook(handle);
    if (!ok) ok = A->off(handle);
    if (b) {
        luaL_unref(S, LUA_REGISTRYINDEX, b->ref);
        memset(b, 0, sizeof *b);
    }
    lua_pushboolean(S, ok);
    return 1;
}

static int l_on(lua_State *S)
{
    const char *ev = luaL_checkstring(S, 1);
    Binding *b;
    int handle;

    luaL_checktype(S, 2, LUA_TFUNCTION);
    b = binding_alloc();
    if (!b) return luaL_error(S, "too many registrations (max %d)",
                              SCRIPT_MAX_BINDINGS);
    lua_pushvalue(S, 2);
    b->ref = luaL_ref(S, LUA_REGISTRYINDEX);
    b->is_event = 1;
    snprintf(b->owner, sizeof b->owner, "%s", s_loading ? s_loading : "?");

    handle = A->on(ev, event_thunk, b, b->owner);
    if (handle < 0) {
        luaL_unref(S, LUA_REGISTRYINDEX, b->ref);
        memset(b, 0, sizeof *b);
        return luaL_error(S, "cannot register for '%s'", ev);
    }
    b->handle = handle;
    lua_pushinteger(S, handle);
    return 1;
}

/* Scripts load in one alphabetical pass, so a script cannot assume another
 * has published to acx.shared yet.  Anything registered here runs once after
 * the pass completes. */
static int l_ready(lua_State *S)
{
    luaL_checktype(S, 1, LUA_TFUNCTION);
    if (s_ready_ref == LUA_NOREF) {
        lua_newtable(S);
        s_ready_ref = luaL_ref(S, LUA_REGISTRYINDEX);
    }
    lua_rawgeti(S, LUA_REGISTRYINDEX, s_ready_ref);
    lua_newtable(S);
    lua_pushvalue(S, 1);                        lua_setfield(S, -2, "fn");
    lua_pushstring(S, s_loading ? s_loading : "?");
    lua_setfield(S, -2, "owner");
    lua_rawseti(S, -2, (lua_Integer)luaL_len(S, -2) + 1);
    lua_pop(S, 1);
    s_ready_pending = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* calling into the game                                               */
/* ------------------------------------------------------------------ */

static int l_call(lua_State *S)
{
    u32 addr = arg_addr(S, 1), args[8];
    int n = lua_gettop(S) - 1, i;

    if (n < 0) n = 0;
    if (n > 8) return luaL_error(S, "acx.call takes at most 8 arguments; use "
                                 "acx.invoke for anything larger");
    for (i = 0; i < n; i++) args[i] = (u32)luaL_optinteger(S, 2 + i, 0);
    lua_pushinteger(S, (lua_Integer)A->call(addr, n, args));
    return 1;
}

/* acx.invoke(fn, { a0 = ..., f12 = ..., stack = { ... } }, { "v0", "f0" })
 *
 * Register-shaped rather than an argument list: o32 puts float arguments in
 * f12 upward and the fifth word onward on the stack, and the engine has
 * functions that do both, so an argument list would have to model each
 * convention. */
static int l_invoke(lua_State *S)
{
    u32 addr = arg_addr(S, 1);
    AcxInvoke iv;
    int nret = 0;

    A->invoke_init(&iv);

    if (!lua_isnoneornil(S, 2)) {
        luaL_checktype(S, 2, LUA_TTABLE);
        lua_pushnil(S);
        while (lua_next(S, 2)) {
            /* key at -2, value at -1.  lua_tostring on a number key would
             * convert it in place and break lua_next, so only string keys
             * reach it. */
            if (lua_type(S, -2) == LUA_TSTRING) {
                const char *k = lua_tostring(S, -2);
                int i;
                if (strcmp(k, "stack") == 0) {
                    int n, j;
                    luaL_checktype(S, -1, LUA_TTABLE);
                    n = (int)luaL_len(S, -1);
                    if (n > (int)(sizeof iv.stack / sizeof iv.stack[0]))
                        n = (int)(sizeof iv.stack / sizeof iv.stack[0]);
                    for (j = 0; j < n; j++) {
                        lua_rawgeti(S, -1, j + 1);
                        iv.stack[j] = (u32)luaL_checkinteger(S, -1);
                        lua_pop(S, 1);
                    }
                    iv.nstack = n;
                } else if ((i = acx_gpr_index(k)) >= 0) {
                    if (i) iv.cpu.r[i] = (u32)luaL_checkinteger(S, -1);
                } else if ((i = fpr_index(k)) >= 0) {
                    iv.cpu.f[i] = (f32)luaL_checknumber(S, -1);
                } else {
                    return luaL_error(S, "'%s' is not a register", k);
                }
            }
            lua_pop(S, 1);
        }
    }

    A->invoke(&iv, addr);

    if (lua_isnoneornil(S, 3)) {
        lua_pushinteger(S, (lua_Integer)iv.cpu.r[R_V0]);
        return 1;
    }
    luaL_checktype(S, 3, LUA_TTABLE);
    {
        int n = (int)luaL_len(S, 3), j;
        for (j = 1; j <= n; j++) {
            const char *k;
            int i;
            lua_rawgeti(S, 3, j);
            k = luaL_checkstring(S, -1);
            lua_pop(S, 1);
            if ((i = acx_gpr_index(k)) >= 0)
                lua_pushinteger(S, (lua_Integer)iv.cpu.r[i]);
            else if ((i = fpr_index(k)) >= 0)
                lua_pushnumber(S, (lua_Number)iv.cpu.f[i]);
            else if (strcmp(k, "hi") == 0)
                lua_pushinteger(S, (lua_Integer)iv.cpu.hi);
            else if (strcmp(k, "lo") == 0)
                lua_pushinteger(S, (lua_Integer)iv.cpu.lo);
            else
                return luaL_error(S, "'%s' is not a register", k);
            nret++;
        }
    }
    return nret;
}

/* ------------------------------------------------------------------ */
/* odds and ends                                                       */
/* ------------------------------------------------------------------ */

static int l_log(lua_State *S)
{
    int n = lua_gettop(S), i;
    printf("[lua]");
    for (i = 1; i <= n; i++) {
        const char *str = luaL_tolstring(S, i, NULL);
        printf(" %s", str);
        lua_pop(S, 1);
    }
    printf("\n");
    return 0;
}

static int l_moddable(lua_State *S)
{
    lua_pushboolean(S, A->moddable());
    return 1;
}

static int l_frame(lua_State *S)
{
    lua_pushinteger(S, (lua_Integer)A->frame());
    return 1;
}

static const luaL_Reg acx_lib[] = {
    { "r8", l_r8 }, { "r16", l_r16 }, { "r32", l_r32 },
    { "rs8", l_rs8 }, { "rs16", l_rs16 }, { "rs32", l_rs32 },
    { "rf32", l_rf32 },
    { "w8", l_w8 }, { "w16", l_w16 }, { "w32", l_w32 }, { "wf32", l_wf32 },
    { "rstr", l_rstr }, { "rbytes", l_rbytes }, { "wbytes", l_wbytes },
    { "memset", l_memset },
    { "alloc", l_alloc }, { "calloc", l_calloc }, { "free", l_free },
    { "cstr", l_cstr }, { "heap", l_heap },
    { "sym", l_sym }, { "name_at", l_name_at }, { "hookable", l_hookable },
    { "call", l_call }, { "invoke", l_invoke },
    { "hook", l_hook }, { "hook_after", l_hook_after }, { "unhook", l_unhook },
    { "on", l_on }, { "ready", l_ready },
    { "log", l_log }, { "moddable", l_moddable }, { "frame", l_frame },
    { NULL, NULL }
};

/* ------------------------------------------------------------------ */
/* loading and reloading                                               */
/* ------------------------------------------------------------------ */

static long file_mtime(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_mtime : 0;
}

/* Drop one script's registrations so a reload replaces them rather than
 * leaving a second copy. */
static void drop_owner(const char *owner)
{
    int i, n, w;

    A->unhook_owner(owner);
    A->off_owner(owner);
    for (i = 0; i < SCRIPT_MAX_BINDINGS; i++) {
        if (!s_bind[i].handle) continue;
        if (strcmp(s_bind[i].owner, owner) != 0) continue;
        luaL_unref(L, LUA_REGISTRYINDEX, s_bind[i].ref);
        memset(&s_bind[i], 0, sizeof s_bind[i]);
    }
    if (s_ready_ref == LUA_NOREF) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, s_ready_ref);
    n = (int)luaL_len(L, -1);
    w = 1;
    for (i = 1; i <= n; i++) {
        const char *o;
        lua_rawgeti(L, -1, i);
        lua_getfield(L, -1, "owner");
        o = lua_tostring(L, -1);
        lua_pop(L, 1);
        if (o && strcmp(o, owner) == 0) { lua_pop(L, 1); continue; }
        lua_rawseti(L, -2, w++);
    }
    for (; w <= n; w++) { lua_pushnil(L); lua_rawseti(L, -2, w); }
    lua_pop(L, 1);
}

static int run_script(Script *sc)
{
    drop_owner(sc->name);
    s_loading = sc->name;

    /* A fresh environment each load, so a reload cannot inherit state the
     * edited script no longer creates.  Reads fall through to the real
     * globals, keeping the standard library and the acx table visible. */
    lua_newtable(L);
    lua_newtable(L);
    lua_pushglobaltable(L);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    if (sc->env_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, sc->env_ref);
    lua_pushvalue(L, -1);
    sc->env_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    if (luaL_loadfile(L, sc->path) != LUA_OK) {
        printf("[lua] %s: %s\n", sc->name, lua_tostring(L, -1));
        lua_pop(L, 2);
        s_loading = NULL;
        return 0;
    }
    lua_pushvalue(L, -2);                           /* the environment */
    if (!lua_setupvalue(L, -2, 1)) lua_pop(L, 1);   /* _ENV */
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        printf("[lua] %s: %s\n", sc->name, lua_tostring(L, -1));
        lua_pop(L, 2);
        s_loading = NULL;
        return 0;
    }
    lua_pop(L, 1);
    s_loading = NULL;
    return 1;
}

static void add_script(const char *path, const char *name)
{
    Script *sc;
    if (s_script_count >= SCRIPT_MAX) {
        fprintf(stderr, "[lua] too many scripts, skipping %s\n", name);
        return;
    }
    sc = &s_scripts[s_script_count];
    snprintf(sc->path, sizeof sc->path, "%s", path);
    snprintf(sc->name, sizeof sc->name, "%s", name);
    sc->mtime = file_mtime(path);
    sc->env_ref = LUA_NOREF;
    s_script_count++;
    sc->ok = run_script(sc);
    printf("[lua] %s %s\n", sc->ok ? "loaded" : "FAILED", sc->name);
}

static void scan_dir(const char *dir, const char *owner)
{
    char path[1024], label[128];
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char pat[1024];
    snprintf(pat, sizeof pat, "%s\\*.lua", dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        snprintf(path, sizeof path, "%s\\%s", dir, fd.cFileName);
        snprintf(label, sizeof label, "%s/%s", owner, fd.cFileName);
        add_script(path, label);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    struct dirent *de;
    size_t n;
    if (!d) return;
    while ((de = readdir(d))) {
        n = strlen(de->d_name);
        if (n < 5 || strcmp(de->d_name + n - 4, ".lua") != 0) continue;
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        snprintf(label, sizeof label, "%s/%s", owner, de->d_name);
        add_script(path, label);
    }
    closedir(d);
#endif
}

void script_init(void)
{
    const char *root = getenv("PSPRECO_MODS");
    char sub[1024];
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char pat[1024];
#else
    DIR *d;
    struct dirent *de;
#endif

    A = acx_api();
    if (!root || !*root) root = "mods";

    L = luaL_newstate();
    if (!L) { fprintf(stderr, "[lua] cannot create a Lua state\n"); return; }
    luaL_openlibs(L);
    ctx_register(L);

    luaL_newlib(L, acx_lib);
    lua_pushinteger(L, (lua_Integer)PSPRECO_MODULE_BASE);
    lua_setfield(L, -2, "guest_base");
    lua_pushinteger(L, (lua_Integer)MOD_HEAP_BASE);
    lua_setfield(L, -2, "heap_base");
    lua_pushinteger(L, (lua_Integer)ACX_API_VERSION);
    lua_setfield(L, -2, "api_version");
    /* A table every script may write to.  Per-script environments stop one
     * script creating a global another sees, so this is the channel for a mod
     * to publish something another builds on. */
    lua_newtable(L);
    lua_setfield(L, -2, "shared");
    lua_setglobal(L, "acx");

#if defined(_WIN32)
    snprintf(pat, sizeof pat, "%s\\*", root);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) goto done;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == '.') continue;
        snprintf(sub, sizeof sub, "%s\\%s\\scripts", root, fd.cFileName);
        scan_dir(sub, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    d = opendir(root);
    if (!d) goto done;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        snprintf(sub, sizeof sub, "%s/%s/scripts", root, de->d_name);
        scan_dir(sub, de->d_name);
    }
    closedir(d);
#endif

done:
    if (s_script_count) {
        s_on = 1;
        printf("[lua] %d script(s), API %u.%u, hooks %s\n", s_script_count,
               (unsigned)ACX_API_MAJOR(ACX_API_VERSION),
               (unsigned)(ACX_API_VERSION & 0xFFFFu),
               A->moddable() ? "available"
                             : "UNAVAILABLE (direct calls are not routed)");
    } else {
        lua_close(L);
        L = NULL;
    }
}

void script_tick(CpuCtx *c, u8 *ram)
{
    int i;
    (void)c; (void)ram;
    if (!s_on) return;

    if (A->frame() % SCRIPT_RELOAD_FRAMES == 0) {
        for (i = 0; i < s_script_count; i++) {
            long m = file_mtime(s_scripts[i].path);
            if (!m || m == s_scripts[i].mtime) continue;
            s_scripts[i].mtime = m;
            printf("[lua] reloading %s\n", s_scripts[i].name);
            s_scripts[i].ok = run_script(&s_scripts[i]);
        }
    }

    /* Every script has loaded by the first tick, so deferred registrations
     * are safe here. */
    if (s_ready_pending) {
        int n;
        s_ready_pending = 0;
        lua_rawgeti(L, LUA_REGISTRYINDEX, s_ready_ref);
        n = (int)luaL_len(L, -1);
        for (i = 1; i <= n; i++) {
            lua_rawgeti(L, -1, i);
            lua_getfield(L, -1, "fn");
            lua_remove(L, -2);
            if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
                printf("[lua] ready handler failed: %s\n", lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        luaL_unref(L, LUA_REGISTRYINDEX, s_ready_ref);
        s_ready_ref = LUA_NOREF;
    }
}
