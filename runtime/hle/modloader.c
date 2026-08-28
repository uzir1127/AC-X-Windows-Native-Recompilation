/* Asset injection at the resource registry.
 *
 * Every asset is reached through one registry: 1024 entries of
 * { char name[16]; u32 addr; u32 size } at unk_261260+16, built by sub_1D1FDC
 * as each region loads and read by Res_GetByName (sub_1C2F64), which returns
 * *(entry+16).  Nothing on that path validates where the bytes live, so a mod
 * needs no change to REGFILE.CDI, the manifests or the archive index, and the
 * archive limits (2730 index slots, 4128768 bytes a region, 2048-byte
 * alignment, 12-character names) do not apply.
 *
 * mods/<name>/assets/<RECORD> repoints an entry the game made.
 *
 * sub_1D1FDC calls the pointer at registry+24784 for every asset it registers;
 * ACX_EngineInit installs sub_6BC there.  sub_6BC has no code xrefs and three
 * data ones, so it is only reached through that pointer and can be overridden
 * through recomp_lookup like the three in qol_input.c.
 *
 * The override runs before the original, which does:
 *
 *     sub_1DCB2C(&gfx, *a3, *(entry + 16));
 *     *(entry + 20) = *(entry + 16);
 *     *(entry + 16) = (*a3)++;
 *
 * For class 0 (.PMD) and class 1 (.PDW) the address field becomes a manager
 * slot and the pointer moves to the size field, which is why Res_GetByName on
 * a model returns a handle.  Rewriting entry+16 on the way in registers the
 * mod's address with the manager, so one hook covers data, models and
 * textures.
 *
 * The engine has already filled its own arena copy by then.  That is left
 * alone, so a replacement of any size cannot overflow the ~18 MB pool
 * ACX_EngineInit carves the registry out of; the stock bytes stay resident.
 *
 * mods/<name>/new/<RECORD> appends an entry instead.
 *
 * A name the disc never had has nothing to repoint: take the next of the 1024
 * slots, bump the scope entry count, and point the u16 id table at
 * dword_38C53C at the new slot, since Res_GetById (sub_1C2F04, 68 call sites)
 * reads that table and not the names.  Mod ids start at 4096; the disc uses
 * 0..3929 of the 16384.
 *
 * Appends bypass sub_1D1FDC and so never invoke the registration callback,
 * which would leave a new .PMD or .PDW out of the model/texture manager.
 * sub_6BC is therefore called directly for those, with the class byte derived
 * from the extension as sub_1D1FDC derives it.
 *
 * Entries are indexed from registry+16 in every scope, so a pushed scope
 * writes above the parent count and a pop frees those slots; across a boot the
 * count runs 79, 254, 273, 237, 357.  The append therefore hooks
 * CDI_RegistryInit and runs on return, while the count is 0, taking slots
 * 0..n-1 of the root scope.  Only a re-init reclaims them, and a re-init calls
 * the hook again.
 *
 * CDI_RegistryInit is a direct call from ACX_EngineInit, so that path needs a
 * --moddable build.  Without one, mod_tick appends once the loader thread has
 * been quiet for half a second and a repair loop restores the records if a
 * scope pop takes them.
 */

#include "modloader.h"
#include "hle.h"
#include "hooks.h"
#include "guestheap.h"
#include "acx_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dirent.h>
#endif

/* Guest addresses come from the symbol database rather than literals, so
 * retargeting a different dump is a regenerate.  modding_tools/acxsdk.py
 * exports them from IDA and writes this header. */
#include "acx_symbols.h"

#define G_RES_REGISTERED G_CDI_ONRESOURCEREGISTERED  /* per-asset callback  */
#define G_RES_BY_NAME    G_RES_GETBYNAME
#define G_REGISTRY       G_CDI_REGISTRY
#define G_IDTABLE_PTR    G_CDI_IDTABLE               /* -> u16[16384]       */
#define G_WPN_PRM_CACHE  G_WPNPRMROWS                /* Res_GetByName(..)+4 */

#define REG_ENTRIES      16u          /* registry+16: entry[1024]              */
#define REG_ENTRY_SIZE   24u
#define REG_ENTRY_MAX    1024         /* sub_1D1C84 memsets exactly 24576 B    */
#define REG_SCOPE        12u          /* registry+12 -> { u32 bump; s16 n; }   */
#define REG_SCOPE_COUNT   4u          /* s16 entry count inside the scope      */
#define REG_SCOPE_SLOTS   6u          /* s16 model slot, s16 texture slot      */

#define MOD_MAX_ASSETS   512
#define MOD_MAX_NEW      256
#define MOD_NAME_MAX     16           /* the registry entry's name field */
#define MOD_ID_BASE      4096         /* the disc uses 0..3929 of 16,384 */
#define MOD_ID_MAX       16384

/* Vblanks of quiet before an append; region loading runs on its own thread
 * (sub_1D3A68). */
#define MOD_SETTLE_FRAMES 30

typedef struct {
    char name[MOD_NAME_MAX];          /* record name, as the registry holds it */
    u32  addr;                        /* guest address of our copy             */
    u32  size;                        /* what we report to the engine          */
    u32  file_size;
    int  hits;                        /* times the engine registered this name */
    char owner[32];                   /* which mod supplied it                 */
} ModAsset;

typedef struct {
    char name[MOD_NAME_MAX];
    u32  addr;
    u32  size;
    u32  name_ptr;                    /* the name, in guest memory, for lookups */
    int  rid;
    int  index;                       /* registry slot once appended, else -1   */
    char owner[32];
} ModNew;

static ModAsset s_assets[MOD_MAX_ASSETS];
static int      s_count;
static ModNew   s_new[MOD_MAX_NEW];
static int      s_new_count;
static int      s_next_rid = MOD_ID_BASE;

static int      s_on;
static int      s_verbose;
static RecompFn s_orig_reg;

static u64      s_frames;
static u64      s_last_reg_frame;
static int      s_saw_reg;

/* Defined below, beside the append they drive. */
static RecompFn s_orig_reg_init;
static RecompFn s_orig_load_index;
static void ov_registry_init(CpuCtx *c, u8 *ram);
static void ov_load_index(CpuCtx *c, u8 *ram);

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static int ci_eq(const char *a, const char *b)
{
    /* Res_GetByName resolves through str_casecmp (CDI_FindFolderIndex,
     * 0x1d2d78), so record names are case-insensitive. */
    for (; *a && *b; a++, b++) {
        int ca = (*a >= 'a' && *a <= 'z') ? *a - 32 : *a;
        int cb = (*b >= 'a' && *b <= 'z') ? *b - 32 : *b;
        if (ca != cb) return 0;
    }
    return *a == *b;
}

static ModAsset *find_asset(const char *name)
{
    int i;
    for (i = 0; i < s_count; i++)
        if (ci_eq(s_assets[i].name, name)) return &s_assets[i];
    return NULL;
}

/* The class byte sub_1D1FDC derives from the extension, so an appended record
 * reaches the same subsystem a loaded one would.  -1 means no special
 * handling: 2918 of the disc's 6662 records. */
static int class_of(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return -1;
    if (ci_eq(dot, ".PMD") || ci_eq(dot, ".MDZ") || ci_eq(dot, ".HIT")) return 0;
    if (ci_eq(dot, ".PDW") || ci_eq(dot, ".IMG")) return 1;
    return -1;
}

static void read_entry_name(u8 *ram, u32 entry, char *out)
{
    u32 i;
    for (i = 0; i < MOD_NAME_MAX - 1u; i++) {
        u8 ch = (u8)mem_r8(ram, entry + i);
        if (!ch) break;
        out[i] = (char)ch;
    }
    out[i] = '\0';
}

/* Run a guest function with up to three arguments and restore the context.
 * Same shape as call_guest3 in qol_input.c. */
static u32 call_guest(CpuCtx *c, u8 *ram, RecompFn f, u32 a0, u32 a1, u32 a2)
{
    CpuCtx save;
    u32 ret;
    if (!f) return 0;
    memcpy(&save, c, sizeof save);
    c->r[R_A0] = a0;
    c->r[R_A1] = a1;
    c->r[R_A2] = a2;
    c->r[R_RA] = 0;
    f(c, ram);
    ret = c->r[R_V0];
    memcpy(c, &save, sizeof save);
    return ret;
}

/* ------------------------------------------------------------------ */
/* loading                                                             */
/* ------------------------------------------------------------------ */

/* One asset file into the mod heap.
 *
 * The file is the payload as the engine registers it: past the record's size
 * dword and already inflated if it was DEF.-wrapped, the same convention
 * acxm.py uses.
 *
 * The registered size is file_size + 4.  A raw record reports its size dword,
 * which counts itself, so the engine reads four bytes past the payload; a
 * DEF. record reports its inflated length exactly.  +4 matches the first and
 * is generous on the second, and the slack zeroed below makes the over-read
 * safe.  Under-reporting would truncate. */
static u32 stage_file(u8 *ram, const char *path, const char *record, u32 *out_sz)
{
    FILE *fp;
    long  sz;
    u32   addr, need;

    fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "[mod] cannot open %s\n", path);
        return 0;
    }
    fseek(fp, 0, SEEK_END);
    sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) {
        fprintf(stderr, "[mod] %s is empty\n", path);
        fclose(fp);
        return 0;
    }
    /* 64 bytes of zeroed slack: the raw path over-reads four, and a consumer
     * walking to a 16-byte boundary reads a little more. */
    need = (u32)sz + 64u;
    addr = ghp_calloc(ram, need);     /* zeroed: the slack below is load-bearing */
    if (!addr) {
        fprintf(stderr, "[mod] skipping %s\n", record);
        fclose(fp);
        return 0;
    }
    if (fread(ram + (addr & PSP_ADDR_MASK), 1, (size_t)sz, fp) != (size_t)sz) {
        fprintf(stderr, "[mod] short read on %s\n", path);
        fclose(fp);
        return 0;
    }
    fclose(fp);
    *out_sz = (u32)sz;
    return addr;
}

static void add_replacement(u8 *ram, const char *path, const char *record,
                            const char *owner)
{
    u32 addr, sz;
    ModAsset *dup;

    if (s_count >= MOD_MAX_ASSETS) {
        fprintf(stderr, "[mod] too many replacements, skipping %s\n", record);
        return;
    }
    if (strlen(record) >= MOD_NAME_MAX) {
        fprintf(stderr, "[mod] %s: longer than the registry's %d-byte name "
                "field\n", record, MOD_NAME_MAX);
        return;
    }
    /* Two mods claiming one record is reported, not resolved.  On stdout as
     * well as stderr: every other [mod] line goes to stdout, so a redirected
     * log would otherwise lose the one line that explains the result. */
    dup = find_asset(record);
    if (dup) {
        printf("[mod] CONFLICT: %s and %s both replace %s -- keeping %s's.\n"
               "[mod]           %s's %s is NOT loaded; anything else it ships "
               "that depends on it\n"
               "[mod]           (a new roster row, a renumbered table) will "
               "not line up. Remove one.\n",
               dup->owner, owner, record, dup->owner, owner, record);
        fprintf(stderr, "[mod] CONFLICT: %s and %s both replace %s -- keeping "
                "%s, remove one of them\n", dup->owner, owner, record,
                dup->owner);
        return;
    }
    addr = stage_file(ram, path, record, &sz);
    if (!addr) return;

    snprintf(s_assets[s_count].name, MOD_NAME_MAX, "%s", record);
    snprintf(s_assets[s_count].owner, sizeof s_assets[0].owner, "%s", owner);
    s_assets[s_count].addr      = addr;
    s_assets[s_count].size      = sz + 4u;
    s_assets[s_count].file_size = sz;
    s_assets[s_count].hits      = 0;
    s_count++;
    printf("[mod] replace %-12s %8u bytes -> guest %08x  (%s)\n",
           record, (unsigned)sz, (unsigned)addr, owner);
}

static void add_new(u8 *ram, const char *path, const char *record,
                    const char *owner)
{
    u32 addr, sz, np;
    size_t i, n;

    if (s_new_count >= MOD_MAX_NEW) {
        fprintf(stderr, "[mod] too many new records, skipping %s\n", record);
        return;
    }
    n = strlen(record);
    if (n >= MOD_NAME_MAX) {
        fprintf(stderr, "[mod] %s: longer than the registry's %d-byte name "
                "field\n", record, MOD_NAME_MAX);
        return;
    }
    if (s_next_rid >= MOD_ID_MAX) {
        fprintf(stderr, "[mod] out of resource ids\n");
        return;
    }
    /* Same rule as a replacement.  sub_1D2D78 returns the first matching
     * name, so a second record under one name is unreachable. */
    for (i = 0; i < s_new_count; i++) {
        if (ci_eq(s_new[i].name, record)) {
            printf("[mod] CONFLICT: %s and %s both add %s -- keeping %s's.\n"
                   "[mod]           Res_GetByName answers with the first "
                   "match, so %s's is unreachable.\n",
                   s_new[i].owner, owner, record, s_new[i].owner, owner);
            fprintf(stderr, "[mod] CONFLICT: %s and %s both add %s -- keeping "
                    "%s, remove one of them\n",
                    s_new[i].owner, owner, record, s_new[i].owner);
            return;
        }
    }
    addr = stage_file(ram, path, record, &sz);
    if (!addr) return;

    /* The engine looks names up out of guest memory, so the name has to live
     * there to query it later. */
    np = ghp_alloc((u32)n + 1u);
    if (!np) return;
    for (i = 0; i < n; i++) mem_w8(ram, np + (u32)i, (u8)record[i]);
    mem_w8(ram, np + (u32)n, 0);

    snprintf(s_new[s_new_count].name, MOD_NAME_MAX, "%s", record);
    snprintf(s_new[s_new_count].owner, sizeof s_new[0].owner, "%s", owner);
    s_new[s_new_count].addr     = addr;
    s_new[s_new_count].size     = sz + 4u;
    s_new[s_new_count].name_ptr = np;
    s_new[s_new_count].rid      = s_next_rid++;
    s_new[s_new_count].index    = -1;
    s_new_count++;
    printf("[mod] new     %-12s %8u bytes -> guest %08x  rid %d  (%s)\n",
           record, (unsigned)sz, (unsigned)addr,
           s_new[s_new_count - 1].rid, owner);
}

static void scan_dir(u8 *ram, const char *dir, const char *owner, int is_new)
{
    char path[1024];
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char pat[1024];

    snprintf(pat, sizeof pat, "%s\\*", dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        snprintf(path, sizeof path, "%s\\%s", dir, fd.cFileName);
        if (is_new) add_new(ram, path, fd.cFileName, owner);
        else        add_replacement(ram, path, fd.cFileName, owner);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    struct dirent *de;

    if (!d) return;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        if (is_new) add_new(ram, path, de->d_name, owner);
        else        add_replacement(ram, path, de->d_name, owner);
    }
    closedir(d);
#endif
}

void mod_init(u8 *ram)
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

    if (!root || !*root) root = "mods";
    s_verbose = getenv("PSPRECO_MODS_VERBOSE") ? 1 : 0;

#if defined(_WIN32)
    snprintf(pat, sizeof pat, "%s\\*", root);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == '.') continue;
        snprintf(sub, sizeof sub, "%s\\%s\\assets", root, fd.cFileName);
        scan_dir(ram, sub, fd.cFileName, 0);
        snprintf(sub, sizeof sub, "%s\\%s\\new", root, fd.cFileName);
        scan_dir(ram, sub, fd.cFileName, 1);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    d = opendir(root);
    if (!d) return;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        snprintf(sub, sizeof sub, "%s/%s/assets", root, de->d_name);
        scan_dir(ram, sub, de->d_name, 0);
        snprintf(sub, sizeof sub, "%s/%s/new", root, de->d_name);
        scan_dir(ram, sub, de->d_name, 1);
    }
    closedir(d);
#endif

    if (s_count || s_new_count) {
        u32 used = 0;
        s_on = 1;
        ghp_stats(&used, NULL, NULL);
        printf("[mod] %d replacement(s), %d new record(s), %u KB of mod heap "
               "at %08x\n", s_count, s_new_count, (unsigned)(used >> 10),
               (unsigned)MOD_HEAP_BASE);
    }
    /* Claim the floor of the entry array at both points the engine resets it.
     * Runs before the module starts, so both hooks are in the table first. */
    if (s_new_count && hook_available()
        && hook_install(G_CDI_REGISTRYINIT, ov_registry_init,
                        &s_orig_reg_init) == HOOK_OK
        && hook_install(G_CDI_LOADINDEX, ov_load_index,
                        &s_orig_load_index) == HOOK_OK)
        printf("[mod] hooked CDI_RegistryInit and CDI_LoadIndex; new records "
               "claim the floor of the entry array\n");
}

/* ------------------------------------------------------------------ */
/* the hook                                                            */
/* ------------------------------------------------------------------ */

static void ov_res_registered(CpuCtx *c, u8 *ram)
{
    u32 entry = c->r[R_A0];
    int cls = (int)(s8)(u8)c->r[R_A1];
    char name[MOD_NAME_MAX];
    ModAsset *a;

    read_entry_name(ram, entry, name);
    a = find_asset(name);
    if (a) {
        u32 was = mem_r32(ram, entry + 16);
        mem_w32(ram, entry + 16, a->addr);
        mem_w32(ram, entry + 20, a->size);
        a->hits++;
        printf("[mod] INJECT %-12s %08x -> %08x  %u bytes, class %d  (%s)\n",
               name, (unsigned)was, (unsigned)a->addr, (unsigned)a->size,
               cls, a->owner);
    } else if (s_verbose) {
        printf("[mod] .      %-12s class %d at %08x\n", name, cls,
               (unsigned)mem_r32(ram, entry + 16));
    }

    if (s_orig_reg) s_orig_reg(c, ram);
    /* "register": a = class, b = the address the engine published, s = name. */
    acx_event_fire3("register", (u32)cls,
                    a ? a->addr : mem_r32(ram, entry + 16), 0, name);

    /* For class 0 and 1 the original has just swapped the entry's two words:
     * the address handed to the manager moved to +20 and +16 became a slot.
     * Reading them back says whether the published pointer is the mod's. */
    if (a && cls >= 0) {
        u32 slot = mem_r32(ram, entry + 16);
        u32 ptr  = mem_r32(ram, entry + 20);
        printf("[mod]        %-12s manager slot %u, published %08x -- %s\n",
               name, (unsigned)slot, (unsigned)ptr,
               ptr == a->addr ? "OURS (class swap carried the mod pointer)"
                              : "NOT ours");
    }

    s_saw_reg = 1;
    s_last_reg_frame = s_frames;
}

RecompFn mod_override(u32 addr, RecompFn orig)
{
    if (!s_on) return NULL;
    if (addr == G_RES_REGISTERED) {
        s_orig_reg = orig;
        return ov_res_registered;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* appending records the disc never had                                */
/* ------------------------------------------------------------------ */

static int append_record(CpuCtx *c, u8 *ram, ModNew *n)
{
    u32 scope = mem_r32(ram, G_REGISTRY + REG_SCOPE);
    int count, cls;
    u32 entry, idt, i;

    if (!scope) return 0;
    count = (int)(s16)(u16)mem_r16(ram, scope + REG_SCOPE_COUNT);
    if (count < 0 || count >= REG_ENTRY_MAX) {
        fprintf(stderr, "[mod] %s: the registry holds %d of %d entries, no "
                "room to append\n", n->name, count, REG_ENTRY_MAX);
        return 0;
    }

    entry = G_REGISTRY + REG_ENTRIES + REG_ENTRY_SIZE * (u32)count;
    for (i = 0; i < REG_ENTRY_SIZE; i++) mem_w8(ram, entry + i, 0);
    for (i = 0; n->name[i]; i++) mem_w8(ram, entry + i, (u8)n->name[i]);
    mem_w32(ram, entry + 16, n->addr);
    mem_w32(ram, entry + 20, n->size);
    mem_w16(ram, scope + REG_SCOPE_COUNT, (u32)(count + 1));

    /* Res_GetById (sub_1C2F04, 68 call sites) goes through this table rather
     * than the names, so an entry without an id fails any lookup by number. */
    idt = mem_r32(ram, G_IDTABLE_PTR);
    if (idt && n->rid >= 0 && n->rid < MOD_ID_MAX)
        mem_w16(ram, idt + 2u * (u32)n->rid, (u32)count);

    n->index = count;

    /* Nothing invoked the registration callback for this entry, so do what
     * sub_1D1FDC would and hand a model or texture to its manager. */
    cls = class_of(n->name);
    if (cls >= 0 && s_orig_reg)
        call_guest(c, ram, s_orig_reg, entry, (u32)cls,
                   scope + REG_SCOPE_SLOTS);

    printf("[mod] APPEND %-12s slot %d, rid %d, %08x, %u bytes, class %d (%s)\n",
           n->name, count, n->rid, (unsigned)n->addr, (unsigned)n->size, cls,
           n->owner);
    return 1;
}

/* Append into the root scope, which is what makes a mod record permanent.
 *
 * *(registry+12) selects one of sixteen 12-byte scope records (sub_1D3194
 * saves the current one with a 12-byte copy indexed by 12*level, which pins
 * the stride), all indexing entries from registry+16.  A pushed scope writes
 * above the parent count and a pop hands those slots back; across a boot the
 * count runs 79, 254, 273, 237, 357, so appending on a quiet loader lands
 * inside whatever scope the frontend is in.
 *
 * On return from CDI_RegistryInit the scope stack is zeroed, the entry count
 * is 0 and the arena and id table are set up, so appending there takes slots
 * 0..n-1 of the root scope, below anything the game registers.
 *
 * It is a direct call from ACX_EngineInit, so this needs a --moddable build;
 * mod_tick keeps the settle-and-repair path for builds without one. */

/* At registry-init time ACX_EngineInit has not installed sub_6BC yet, so ask
 * recomp_lookup, whose call into mod_override sets s_orig_reg. */
static void ensure_orig_reg(void)
{
    if (!s_orig_reg) (void)recomp_lookup(G_RES_REGISTERED);
}

static void mod_ensure_records(CpuCtx *c, u8 *ram, const char *why);

static void ov_registry_init(CpuCtx *c, u8 *ram)
{
    if (s_orig_reg_init) s_orig_reg_init(c, ram);
    ensure_orig_reg();
    mod_ensure_records(c, ram, "registry init");
}

/* CDI_LoadIndex resets the count to 32 after CDI_RegistryInit already left it
 * there, clearing anything claimed in between.  Claiming here too means mod
 * records resolve from the first frame. */
static void ov_load_index(CpuCtx *c, u8 *ram)
{
    if (s_orig_load_index) s_orig_load_index(c, ram);
    ensure_orig_reg();
    mod_ensure_records(c, ram, "index load");
}

/* Ask the engine whether the new record resolves. */
static void verify_new(CpuCtx *c, u8 *ram, ModNew *n)
{
    RecompFn f = recomp_lookup(G_RES_BY_NAME);
    u32 got;
    int cls = class_of(n->name);

    if (!f) {
        printf("[mod] %s: Res_GetByName is not in the dispatch table\n",
               n->name);
        return;
    }
    got = call_guest(c, ram, f, n->name_ptr, 0, 0);

    if (cls >= 0) {
        /* For a model or texture the engine returns a manager slot and the
         * pointer lives in the entry's second word. */
        u32 entry = G_REGISTRY + REG_ENTRIES + REG_ENTRY_SIZE * (u32)n->index;
        u32 ptr = mem_r32(ram, entry + 20);
        printf("[mod] PROOF: Res_GetByName(\"%s\") = slot %u, published %08x "
               "-- %s\n", n->name, (unsigned)got, (unsigned)ptr,
               ptr == n->addr ? "OURS" : "NOT ours");
    } else {
        printf("[mod] PROOF: Res_GetByName(\"%s\") = %08x -- %s\n",
               n->name, (unsigned)got,
               got == n->addr ? "OURS (a record the disc never had)"
                              : "NOT ours");
    }
}

/* ------------------------------------------------------------------ */
/* proof and lifecycle                                                 */
/* ------------------------------------------------------------------ */

/* Check that the engine consumes the injected bytes, not just that they were
 * registered.  sub_BB634 caches
 *
 *     dword_295A94 = Res_GetByName("WPN_PRM.PRM") + 4;
 *
 * at boot, so one pointer answers both halves: that it lands in the mod heap,
 * and that the eight bytes of slack past WPN_PRM's hundred 44-byte rows hold
 * the mod's marker rather than a stale copy. */
#define WPN_SLACK_OFF 4400u           /* 4 + 100*44, less the cached +4 */

static void proof_wpn(u8 *ram)
{
    static const char magic[8] = { 'A','C','X','M','O','D','0','1' };
    static int done;
    u32 p, i;
    int in_heap, marked = 1;

    if (done) return;
    p = mem_r32(ram, G_WPN_PRM_CACHE);
    if (!p) return;
    done = 1;

    in_heap = (p >= MOD_HEAP_BASE && p < MOD_HEAP_END);
    for (i = 0; i < sizeof magic; i++)
        if ((char)mem_r8(ram, p + WPN_SLACK_OFF + i) != magic[i]) marked = 0;

    printf("[mod] PROOF: engine cached WPN_PRM.PRM at %08x -- %s, marker %s\n",
           (unsigned)p, in_heap ? "INSIDE the mod heap" : "outside the mod heap",
           marked ? "FOUND" : "absent");
}

void mod_note_tex(u32 addr)
{
    static int done;
    if (done) return;
    done = 1;
    printf("[mod] PROOF: the GE bound a texture at %08x -- the GPU is sampling "
           "texels out of the mod heap\n", (unsigned)addr);
}

/* ------------------------------------------------------------------ */
/* Tier-1: a hook on a function nothing could reach before             */
/* ------------------------------------------------------------------ */

/* Res_GetByName has 180 direct call sites and one indirect, so recomp_lookup
 * saw effectively none of them.  A --moddable build routes the direct calls
 * through the dispatch table; this counts what arrives, which is 0 without
 * one and every by-name asset lookup with one. */
static RecompFn s_orig_res_by_name;
static unsigned s_res_calls;

static void ov_res_by_name(CpuCtx *c, u8 *ram)
{
    u32 nameptr = c->r[R_A0];

    if (s_res_calls < 8) {
        char nm[24];
        u32 i;
        for (i = 0; i < sizeof nm - 1u; i++) {
            u8 ch = (u8)mem_r8(ram, nameptr + i);
            if (!ch) break;
            nm[i] = (char)ch;
        }
        nm[i] = '\0';
        printf("[hook] Res_GetByName(\"%s\")\n", nm);
    }
    s_res_calls++;
    if (s_orig_res_by_name) s_orig_res_by_name(c, ram);
}

static void trace_res_report(void)
{
    printf("[hook] PROOF: Res_GetByName was intercepted %u time(s) -- %s\n",
           s_res_calls,
           s_res_calls ? "direct call sites are going through the table"
                       : "nothing arrived (is this a --moddable build?)");
}

static int s_appended;

/* Is the record still in its slot?
 *
 * Two ways it stops being: the scope count fell below the slot, putting it
 * outside what CDI_FindFolderIndex scans, or the count came back up and
 * something else was registered over it. */
static int record_intact(u8 *ram, ModNew *n, int count)
{
    char now[MOD_NAME_MAX] = "";
    if (n->index < 0 || n->index >= count) return 0;
    read_entry_name(ram, G_REGISTRY + REG_ENTRIES
                    + REG_ENTRY_SIZE * (u32)n->index, now);
    return ci_eq(now, n->name);
}

/* Append whatever is missing.  Idempotent, so the three places that want mod
 * records present -- registry init, index load, and every frame after -- can
 * all just call it, and none of them has to know whether another already did.
 *
 * The engine resets the registry more than once during boot: CDI_RegistryInit
 * leaves the count at 32 (sub_1D30C4 reserves entries 0..31), and loading the
 * region index puts it back to 32 again a few frames later, clearing anything
 * appended in between.  Claiming at each of those points closes the window
 * where a mod record would not resolve, and the per-frame call is the
 * guarantee that covers any reset this has not seen. */
static void mod_ensure_records(CpuCtx *c, u8 *ram, const char *why)
{
    u32 scope = mem_r32(ram, G_REGISTRY + REG_SCOPE);
    int count, i;

    if (!s_new_count || !scope) return;
    count = (int)(s16)(u16)mem_r16(ram, scope + REG_SCOPE_COUNT);
    for (i = 0; i < s_new_count; i++) {
        ModNew *n = &s_new[i];
        if (record_intact(ram, n, count)) continue;
        if (s_appended)
            printf("[mod] f%llu: %s is gone (count %d) -- re-claiming after "
                   "%s\n", (unsigned long long)s_frames, n->name, count, why);
        n->index = -1;
        if (append_record(c, ram, n)) {
            verify_new(c, ram, n);
            count++;
        }
    }
    s_appended = 1;
}

void mod_tick(CpuCtx *c, u8 *ram)
{
    static int traced;
    static u64 next_watch;

    if (!s_on) return;
    s_frames++;

    if (!traced && getenv("PSPRECO_MOD_TRACE_RES")) {
        traced = 1;
        if (hook_install(G_RES_BY_NAME, ov_res_by_name,
                         &s_orig_res_by_name) == HOOK_OK)
            printf("[hook] installed on Res_GetByName (%08x), %s\n",
                   (unsigned)G_RES_BY_NAME,
                   hook_available() ? "moddable build" : "NOT moddable");
    }
    if (traced && s_frames == 600) trace_res_report();

    proof_wpn(ram);

    /* Mod records, present.  In a --moddable build the registry-init and
     * index-load hooks have already claimed them and this confirms it; in a
     * build without one this is the only claim there is, and it waits for the
     * loader thread to go quiet because CDI_LoadRegion uses the entry count as
     * an id-table index right after registering and moving the count
     * underneath it would misfile the record it just read.
     *
     * Costs one 16-bit read and a name compare per record when nothing moved. */
    if (s_appended || (s_saw_reg
                       && s_frames > s_last_reg_frame + MOD_SETTLE_FRAMES))
        mod_ensure_records(c, ram, "a registry reset");

    if (s_appended && s_new_count && s_frames >= next_watch) {
        u32 scope = mem_r32(ram, G_REGISTRY + REG_SCOPE);
        int count = scope ? (int)(s16)(u16)mem_r16(ram, scope + REG_SCOPE_COUNT)
                          : -1;
        next_watch = s_frames + 900;
        printf("[mod] watch f%llu: registry holds %d of %d entries, %d mod "
               "record(s) at slot %d+\n", (unsigned long long)s_frames, count,
               REG_ENTRY_MAX, s_new_count, s_new[0].index);
    }
}
