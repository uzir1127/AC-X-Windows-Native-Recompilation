/* The parameter tables, live.
 *
 * How an edit reaches the game differs per table.
 *
 * WPN_PRM and GUN_PRM are indexed in place: Wpn_CacheParamTables keeps
 * `Res_GetByName("WPN_PRM.PRM") + 4` in a global, sub_1A672C computes
 * `8 * gun_type + GUN_PRM + 4` on demand, and Aircraft_Load caches row
 * pointers rather than copying rows.  Writing the table is enough.
 *
 * PL_PARAM is not.  Aircraft_Load does
 *
 *     sub_215DB8(a2 + 600, 392 * a3 + PL_PARAM + 4, 392);   // row -> object
 *     *(a2 + 992) = a2 + 600;
 *     sub_678A0(a1, a2, *(a2 + 992));                       // parts, in place
 *
 * so the model the engine integrates is a copy at object+600 with the fitted
 * parts folded in, and the table is read once at spawn.  An edit goes to both;
 * `live_kind` records which case a table is.  (docs/moddability.md quotes the
 * copy as `a2 + 150`, the same line with `a2` typed as `int *`; the byte
 * offset is 600.)
 *
 * Hooking Aircraft_Load needs a --moddable build -- three direct call sites
 * and one indirect.  Without it the tables still edit and PL_PARAM applies on
 * the next spawn.
 *
 * The menu tables are a third case.  WND_PRM is copied into a 36-byte block
 * when a window is built (sub_12E598), so an edit lands the next time that
 * window opens.  WND_ITEM and MMES are read through pointers every draw, so a
 * flag or label changes at once.
 *
 * Field descriptions come from docs/formats/PRM.md, which distinguishes a
 * field with a traced reader from one inferred from the upgrade part that
 * modifies it.  The UI keeps that distinction.  Most of PL_PARAM's 72 floats
 * have neither and are listed anyway.
 */

#include "datatables.h"
#include "hooks.h"
#include "hle.h"
#include "acx_symbols.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#  include <direct.h>
#else
#  include <sys/stat.h>
#endif

#define OBJ_FLIGHT_MODEL 600u

/* The field tables and the table list live in generated/acx_tables.c, emitted
 * by modding_tools/acxfmt.py from acxfmt/schemas/*.toml.  The TOML is the only
 * place a table is described. */

/* A ceiling rather than sizeof g_acx_tables, which is in another translation
 * unit.  Checked at use. */
#define TBL_MAX 16

/* ------------------------------------------------------------------ */

static RecompFn s_orig_load;
static u32      s_live_obj;
static int      s_live_row = -1;

static u8      *s_stock[TBL_MAX];
static int      s_stock_len[TBL_MAX];

static int table_index(const AcxTable *t)
{
    int i = (int)(t - g_acx_tables);
    return (i >= 0 && i < TBL_MAX) ? i : 0;
}

static void ov_aircraft_load(CpuCtx *c, u8 *ram)
{
    u32 obj = c->r[R_A1];
    int row = (int)c->r[R_A2];

    if (s_orig_load) s_orig_load(c, ram);

    /* After the original, which fills object+600 and folds the parts in. */
    s_live_obj = obj;
    s_live_row = row;
    printf("[table] aircraft %d loaded; flight model live at %08x\n",
           row, (unsigned)(obj + OBJ_FLIGHT_MODEL));
}

void table_init(void)
{
    if (!hook_available()) return;
    if (hook_install(G_AIRCRAFT_LOAD, ov_aircraft_load, &s_orig_load) == HOOK_OK)
        printf("[table] watching Aircraft_Load (%08x) for the live flight "
               "model\n", (unsigned)G_AIRCRAFT_LOAD);
}

u32 plane_live(void)     { return s_live_obj ? s_live_obj + OBJ_FLIGHT_MODEL : 0; }
int plane_live_row(void) { return s_live_row; }

/* The registry entry for a record, walked directly so the UI does not call
 * into the guest from its own thread. */
static int name_matches(const char *nm, const AcxTable *t)
{
    const char *a = t->aliases;
    size_t n;
    if (strcmp(nm, t->record) == 0) return 1;
    while (a && *a) {
        const char *comma = strchr(a, ',');
        n = comma ? (size_t)(comma - a) : strlen(a);
        if (strlen(nm) == n && strncmp(nm, a, n) == 0) return 1;
        a = comma ? comma + 1 : a + n;
    }
    return 0;
}

static u32 find_entry(u8 *ram, const AcxTable *t)
{
    u32 scope = mem_r32(ram, G_CDI_REGISTRY + 12);
    int count, i;
    if (!scope) return 0;
    count = (int)(s16)(u16)mem_r16(ram, scope + 4);
    for (i = 0; i < count; i++) {
        u32 e = G_CDI_REGISTRY + 16 + 24u * (u32)i;
        char nm[17];
        u32 j;
        for (j = 0; j < 16; j++) {
            nm[j] = (char)(u8)mem_r8(ram, e + j);
            if (!nm[j]) break;
        }
        nm[16] = 0;
        if (j < 16 && name_matches(nm, t)) return e;
    }
    return 0;
}

u32 table_base(u8 *ram, const AcxTable *t, int *out_rows)
{
    u32 e = find_entry(ram, t), base;
    int rows, idx = table_index(t);

    if (out_rows) *out_rows = 0;
    if (!e) return 0;
    base = mem_r32(ram, e + 16);
    if (!base) return 0;
    rows = (int)mem_r32(ram, base);
    if (rows <= 0 || rows > 4096) return 0;

    /* Capture the first version seen, so revert restores what the disc
     * shipped rather than this session's earlier edit. */
    if (!s_stock[idx]) {
        int len = (int)mem_r32(ram, e + 20) - 4;
        if (len > 4 && len < (1 << 20)) {
            s_stock[idx] = (u8 *)malloc((size_t)len);
            if (s_stock[idx]) {
                int i;
                for (i = 0; i < len; i++)
                    s_stock[idx][i] = (u8)mem_r8(ram, base + (u32)i);
                s_stock_len[idx] = len;
            }
        }
    }
    if (out_rows) *out_rows = rows;
    return base + t->header;
}

const TableField *table_field_at(const AcxTable *t, unsigned off)
{
    int i;
    for (i = 0; i < t->field_count; i++)
        if (t->fields[i].off == off) return &t->fields[i];
    return NULL;
}

/* ------------------------------------------------------------------ */

static double read_at(u8 *ram, u32 a, const TableField *f)
{
    switch (f->type) {
    case PF_F32: return (double)mem_rf32(ram, a);
    case PF_U8:  return (double)(u8)mem_r8(ram, a);
    case PF_S8:  return (double)(s8)(u8)mem_r8(ram, a);
    case PF_U16: return (double)(u16)mem_r16(ram, a);
    case PF_S16: return (double)(s16)(u16)mem_r16(ram, a);
    case PF_S32: return (double)(s32)mem_r32(ram, a);
    default:     return (double)(u32)mem_r32(ram, a);
    }
}

static void write_at(u8 *ram, u32 a, const TableField *f, double v)
{
    switch (f->type) {
    case PF_F32: mem_wf32(ram, a, (f32)v); break;
    case PF_U8:  mem_w8 (ram, a, (u32)v);  break;
    case PF_S8:  mem_w8 (ram, a, (u32)(s32)v); break;
    case PF_U16: case PF_S16: mem_w16(ram, a, (u32)(s32)v); break;
    default:     mem_w32(ram, a, (u32)(s64)v); break;
    }
}

static double read_buf(const u8 *p, const TableField *f)
{
    u32 v = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
    switch (f->type) {
    case PF_F32: { f32 g; memcpy(&g, &v, 4); return (double)g; }
    case PF_U8:  return (double)p[0];
    case PF_S8:  return (double)(s8)p[0];
    case PF_U16: return (double)(u16)(v & 0xFFFF);
    case PF_S16: return (double)(s16)(u16)(v & 0xFFFF);
    case PF_S32: return (double)(s32)v;
    default:     return (double)v;
    }
}

void table_get_str(u8 *ram, const AcxTable *t, int row, const TableField *f,
                   char *out, int out_len)
{
    u32 base = table_base(ram, t, NULL);
    int i, n = (int)f->len;
    if (n > out_len - 1) n = out_len - 1;
    out[0] = 0;
    if (!base) return;
    for (i = 0; i < n; i++) {
        char ch = (char)(u8)mem_r8(ram, base + (u32)row * t->stride + f->off
                                        + (u32)i);
        out[i] = ch;
        if (!ch) return;
    }
    out[n] = 0;
}

/* NUL-padded to the full cell: the engine reads a fixed width, so a short
 * write leaves the tail of the old label on screen. */
void table_set_str(u8 *ram, const AcxTable *t, int row, const TableField *f,
                   const char *str)
{
    u32 base = table_base(ram, t, NULL);
    u32 cell;
    unsigned i;

    if (!base) return;
    cell = base + (u32)row * t->stride + f->off;
    for (i = 0; i + 1 < f->len && str[i]; i++)
        mem_w8(ram, cell + i, (u8)str[i]);
    for (; i < f->len; i++)
        mem_w8(ram, cell + i, 0);
}

double table_get(u8 *ram, const AcxTable *t, int row, const TableField *f)
{
    u32 base = table_base(ram, t, NULL);
    if (!base) return 0.0;
    return read_at(ram, base + (u32)row * t->stride + f->off, f);
}

void table_set(u8 *ram, const AcxTable *t, int row, const TableField *f, double v)
{
    u32 base = table_base(ram, t, NULL);
    if (base) write_at(ram, base + (u32)row * t->stride + f->off, f, v);

    /* PL_PARAM only, and only for the row in the air.  Overwrites whatever
     * the parts pass contributed to this field; the rest of the live row
     * keeps its bonuses until the next spawn. */
    if (t->live_kind == LIVE_COPY && row == s_live_row && plane_live())
        write_at(ram, plane_live() + f->off, f, v);
}

double table_get_stock(const AcxTable *t, int row, const TableField *f, int *ok)
{
    int idx = table_index(t);
    int off = (int)t->header + row * (int)t->stride + (int)f->off;
    if (ok) *ok = 0;
    if (!s_stock[idx] || off + 4 > s_stock_len[idx]) return 0.0;
    if (ok) *ok = 1;
    return read_buf(s_stock[idx] + off, f);
}

void table_get_stock_str(const AcxTable *t, int row, const TableField *f,
                         char *out, int out_len)
{
    int idx = table_index(t);
    int off = (int)t->header + row * (int)t->stride + (int)f->off;
    int i, n = (int)f->len;

    out[0] = 0;
    if (!s_stock[idx] || off + n > s_stock_len[idx]) return;
    if (n > out_len - 1) n = out_len - 1;
    for (i = 0; i < n; i++) {
        out[i] = (char)s_stock[idx][off + i];
        if (!out[i]) return;
    }
    out[n] = 0;
}

void table_revert_row(u8 *ram, const AcxTable *t, int row)
{
    u32 base = table_base(ram, t, NULL);
    int idx = table_index(t);
    int off = (int)t->header + row * (int)t->stride;
    unsigned i;

    if (!base || !s_stock[idx] || off + (int)t->stride > s_stock_len[idx]) return;
    for (i = 0; i < t->stride; i++)
        mem_w8(ram, base + (u32)row * t->stride + i, s_stock[idx][off + (int)i]);
    if (t->live_kind == LIVE_COPY && row == s_live_row && plane_live())
        for (i = 0; i < t->stride; i++)
            mem_w8(ram, plane_live() + i, s_stock[idx][off + (int)i]);
}

/* ------------------------------------------------------------------ */

int table_save_mod(u8 *ram, const AcxTable *t, const char *mod,
                   char *out_path, int out_len)
{
    const char *root = getenv("PSPRECO_MODS");
    u32 e = find_entry(ram, t), base;
    int len, i;
    FILE *fp;
    char dir[768], one[768];

    if (!root || !*root) root = "mods";
    if (!e) return -1;
    base = mem_r32(ram, e + 16);
    len  = (int)mem_r32(ram, e + 20) - 4;   /* the payload, as .acxm counts it */
    if (!base || len <= 0 || len > (1 << 20)) return -1;

    snprintf(one, sizeof one, "%s/%s", root, mod);
    snprintf(dir, sizeof dir, "%s/assets", one);
#if defined(_WIN32)
    _mkdir(root); _mkdir(one); _mkdir(dir);
#else
    mkdir(root, 0755); mkdir(one, 0755); mkdir(dir, 0755);
#endif

    snprintf(out_path, (size_t)out_len, "%s/%s", dir, t->record);
    fp = fopen(out_path, "wb");
    if (!fp) return -2;
    for (i = 0; i < len; i++)
        fputc((int)(u8)mem_r8(ram, base + (u32)i), fp);
    fclose(fp);
    printf("[table] wrote %s (%d bytes)\n", out_path, len);
    return 0;
}
