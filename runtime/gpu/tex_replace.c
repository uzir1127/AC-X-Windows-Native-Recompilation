#include "tex_replace.h"
#include "tex_img.h"
#include "tex_ktx2.h"
#include "ge_shared.h"
#include "../host/settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if defined(_WIN32)
#  include <windows.h>
#  include <direct.h>
#  define TR_MKDIR(p) _mkdir(p)
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  define TR_MKDIR(p) mkdir((p), 0755)
#endif

extern uint8_t *g_ram;

#define TR_ADDR_MASK  0x1FFFFFFFu
#define TR_HOST(a)    (g_ram + ((a) & TR_ADDR_MASK))
#define TR_KERNEL_END 0x08400000u

enum { HK_QUICK = 0, HK_XXH32 = 1, HK_XXH64 = 2 };

static const uint8_t k_bpp[16] = {
    16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8, 0, 0, 0, 0, 0
};

static uint32_t rotl32(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }
static uint64_t rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

#define XP1 2654435761u
#define XP2 2246822519u
#define XP3 3266489917u
#define XP4  668265263u
#define XP5  374761393u

static uint32_t xxh32(const void *input, size_t len, uint32_t seed)
{
    const uint8_t *p = (const uint8_t *)input;
    const uint8_t *end = p + len;
    uint32_t h;

    if (len >= 16) {
        const uint8_t *limit = end - 16;
        uint32_t v1 = seed + XP1 + XP2;
        uint32_t v2 = seed + XP2;
        uint32_t v3 = seed;
        uint32_t v4 = seed - XP1;
        do {
            uint32_t k;
            memcpy(&k, p, 4); p += 4; v1 = rotl32(v1 + k * XP2, 13) * XP1;
            memcpy(&k, p, 4); p += 4; v2 = rotl32(v2 + k * XP2, 13) * XP1;
            memcpy(&k, p, 4); p += 4; v3 = rotl32(v3 + k * XP2, 13) * XP1;
            memcpy(&k, p, 4); p += 4; v4 = rotl32(v4 + k * XP2, 13) * XP1;
        } while (p <= limit);
        h = rotl32(v1, 1) + rotl32(v2, 7) + rotl32(v3, 12) + rotl32(v4, 18);
    } else {
        h = seed + XP5;
    }

    h += (uint32_t)len;
    while (p + 4 <= end) {
        uint32_t k;
        memcpy(&k, p, 4); p += 4;
        h = rotl32(h + k * XP3, 17) * XP4;
    }
    while (p < end) {
        h = rotl32(h + (uint32_t)(*p++) * XP5, 11) * XP1;
    }
    h ^= h >> 15; h *= XP2;
    h ^= h >> 13; h *= XP3;
    h ^= h >> 16;
    return h;
}

#define XQ1 11400714785074694791ull
#define XQ2 14029467366897019727ull
#define XQ3  1609587929392839161ull
#define XQ4  9650029242287828579ull
#define XQ5  2870177450012600261ull

static uint64_t xxh64_round(uint64_t acc, uint64_t v)
{
    acc += v * XQ2;
    acc = rotl64(acc, 31);
    acc *= XQ1;
    return acc;
}

static uint64_t xxh64_merge(uint64_t acc, uint64_t v)
{
    acc ^= xxh64_round(0, v);
    return acc * XQ1 + XQ4;
}

static uint64_t xxh64(const void *input, size_t len, uint64_t seed)
{
    const uint8_t *p = (const uint8_t *)input;
    const uint8_t *end = p + len;
    uint64_t h;

    if (len >= 32) {
        const uint8_t *limit = end - 32;
        uint64_t v1 = seed + XQ1 + XQ2;
        uint64_t v2 = seed + XQ2;
        uint64_t v3 = seed;
        uint64_t v4 = seed - XQ1;
        do {
            uint64_t k;
            memcpy(&k, p, 8); p += 8; v1 = xxh64_round(v1, k);
            memcpy(&k, p, 8); p += 8; v2 = xxh64_round(v2, k);
            memcpy(&k, p, 8); p += 8; v3 = xxh64_round(v3, k);
            memcpy(&k, p, 8); p += 8; v4 = xxh64_round(v4, k);
        } while (p <= limit);
        h = rotl64(v1, 1) + rotl64(v2, 7) + rotl64(v3, 12) + rotl64(v4, 18);
        h = xxh64_merge(h, v1);
        h = xxh64_merge(h, v2);
        h = xxh64_merge(h, v3);
        h = xxh64_merge(h, v4);
    } else {
        h = seed + XQ5;
    }

    h += (uint64_t)len;
    while (p + 8 <= end) {
        uint64_t k;
        memcpy(&k, p, 8); p += 8;
        h ^= xxh64_round(0, k);
        h = rotl64(h, 27) * XQ1 + XQ4;
    }
    if (p + 4 <= end) {
        uint32_t k;
        memcpy(&k, p, 4); p += 4;
        h ^= (uint64_t)k * XQ1;
        h = rotl64(h, 23) * XQ2 + XQ3;
    }
    while (p < end) {
        h ^= (uint64_t)(*p++) * XQ5;
        h = rotl64(h, 11) * XQ1;
    }
    h ^= h >> 33; h *= XQ2;
    h ^= h >> 29; h *= XQ3;
    h ^= h >> 32;
    return h;
}

static uint32_t quick_tex_hash(const void *checkp, uint32_t size)
{
    uint32_t check = 0;

    if ((((uintptr_t)checkp) & 0xf) == 0 && (size & 0x3f) == 0) {
        static const uint16_t init[8] = {
            0xc00bu, 0x9bd9u, 0x4b73u, 0xb651u, 0x4d9bu, 0x4309u, 0x0083u, 0x0001u
        };
        union { uint16_t x16[8]; uint32_t x32[4]; } cursor, c2;
        const uint8_t *p = (const uint8_t *)checkp;
        uint32_t i, j, blocks = size / 16;

        memset(&cursor, 0, sizeof cursor);
        memcpy(c2.x16, init, sizeof init);

        for (i = 0; i < blocks; i += 4) {
            uint16_t t16[8];
            uint32_t t32[4];
            memcpy(t16, p + (size_t)(i + 0) * 16, 16);
            for (j = 0; j < 8; j++)
                cursor.x16[j] = (uint16_t)(cursor.x16[j] + (uint16_t)(t16[j] * c2.x16[j]));
            memcpy(t32, p + (size_t)(i + 1) * 16, 16);
            for (j = 0; j < 4; j++) cursor.x32[j] ^= t32[j];
            memcpy(t32, p + (size_t)(i + 2) * 16, 16);
            for (j = 0; j < 4; j++) cursor.x32[j] += t32[j];
            memcpy(t16, p + (size_t)(i + 3) * 16, 16);
            for (j = 0; j < 8; j++)
                cursor.x16[j] ^= (uint16_t)(t16[j] * c2.x16[j]);
            for (j = 0; j < 8; j++) c2.x16[j] = (uint16_t)(c2.x16[j] + 0x2455u);
        }

        for (j = 0; j < 4; j++) cursor.x32[j] += c2.x32[j];
        check = cursor.x32[0] + cursor.x32[1] + cursor.x32[2] + cursor.x32[3];
    } else {
        const uint8_t *p = (const uint8_t *)checkp;
        uint32_t i, n = size / 8;
        for (i = 0; i < n; i++) {
            uint32_t a, b;
            memcpy(&a, p, 4); p += 4;
            memcpy(&b, p, 4); p += 4;
            check += a;
            check ^= b;
        }
    }
    return check;
}

static uint32_t max_size_at(uint32_t address)
{
    uint32_t a = address & 0x3FFFFFFFu;
    /* User RAM, extended through the mod heap: modloader.c stages injected
     * assets from 0x0A000000 up, and a .PDW there must hash like any other
     * or texture packs stop seeing it. */
    if ((address & 0x38000000u) == 0x08000000u) return 0x10000000u - a;
    if ((address & 0x3F800000u) == 0x04000000u) return 0x04800000u - a;
    if ((address & 0xBFFF0000u) == 0x00010000u) return 0x00014000u - a;
    return 0;
}

typedef struct {
    char    *name;
    uint32_t lho;
    uint32_t csize, usize;
    uint16_t method;
} ZipEnt;

typedef struct {
    int      is_zip;
    char     dir[1024];
    char     zip[1088];
    ZipEnt  *ents;
    int      nents;
} TexVfs;

static TexVfs s_vfs;

static uint8_t *read_whole(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long n;
    uint8_t *b;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    b = (uint8_t *)malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    if (n && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f);
    b[n] = 0;
    *len = (size_t)n;
    return b;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void zip_close(void)
{
    int i;
    for (i = 0; i < s_vfs.nents; i++) free(s_vfs.ents[i].name);
    free(s_vfs.ents);
    s_vfs.ents = NULL;
    s_vfs.nents = 0;
}

static int zip_open(const char *path)
{
    static uint8_t tail[66000];
    uint8_t *cd = NULL;
    FILE *f = fopen(path, "rb");
    long fsize;
    size_t tn, i;
    uint32_t cdoff = 0, cdsize = 0;
    int count = 0, found = 0, n = 0;

    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    fsize = ftell(f);
    if (fsize < 22) { fclose(f); return 0; }
    tn = (size_t)fsize < sizeof tail ? (size_t)fsize : sizeof tail;
    fseek(f, fsize - (long)tn, SEEK_SET);
    if (fread(tail, 1, tn, f) != tn) { fclose(f); return 0; }

    for (i = tn - 21; i-- > 0;) {
        if (tail[i] == 'P' && tail[i + 1] == 'K' && tail[i + 2] == 5 && tail[i + 3] == 6) {
            count = rd16(tail + i + 10);
            cdsize = rd32(tail + i + 12);
            cdoff = rd32(tail + i + 16);
            found = 1;
            break;
        }
    }
    if (!found || count <= 0 || cdsize == 0 ||
        (long)cdoff < 0 || (long)(cdoff + cdsize) > fsize) {
        fclose(f);
        return 0;
    }

    cd = (uint8_t *)malloc(cdsize);
    if (!cd) { fclose(f); return 0; }
    fseek(f, (long)cdoff, SEEK_SET);
    if (fread(cd, 1, cdsize, f) != cdsize) { free(cd); fclose(f); return 0; }
    fclose(f);

    s_vfs.ents = (ZipEnt *)calloc((size_t)count, sizeof(ZipEnt));
    if (!s_vfs.ents) { free(cd); return 0; }

    {
        size_t p = 0;
        while (p + 46 <= cdsize && n < count) {
            uint16_t nl, el, cl;
            if (rd32(cd + p) != 0x02014b50u) break;
            nl = rd16(cd + p + 28);
            el = rd16(cd + p + 30);
            cl = rd16(cd + p + 32);
            if (p + 46 + nl + el + cl > cdsize) break;
            s_vfs.ents[n].method = rd16(cd + p + 10);
            s_vfs.ents[n].csize  = rd32(cd + p + 20);
            s_vfs.ents[n].usize  = rd32(cd + p + 24);
            s_vfs.ents[n].lho    = rd32(cd + p + 42);
            s_vfs.ents[n].name   = (char *)malloc((size_t)nl + 1);
            if (!s_vfs.ents[n].name) break;
            memcpy(s_vfs.ents[n].name, cd + p + 46, nl);
            s_vfs.ents[n].name[nl] = 0;
            {
                char *c;
                for (c = s_vfs.ents[n].name; *c; c++) if (*c == '\\') *c = '/';
            }
            n++;
            p += 46u + nl + el + cl;
        }
    }
    free(cd);
    s_vfs.nents = n;
    if (!n) { zip_close(); return 0; }
    return 1;
}

static int name_eq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static uint8_t *zip_read(const char *name, size_t *len)
{
    int i;
    for (i = 0; i < s_vfs.nents; i++) {
        ZipEnt *e = &s_vfs.ents[i];
        uint8_t lh[30];
        uint8_t *raw, *out;
        FILE *f;
        size_t off;

        if (!name_eq_ci(e->name, name)) continue;
        if (e->method != 0 && e->method != 8) return NULL;
        f = fopen(s_vfs.zip, "rb");
        if (!f) return NULL;
        if (fseek(f, (long)e->lho, SEEK_SET) != 0 || fread(lh, 1, 30, f) != 30 ||
            rd32(lh) != 0x04034b50u) { fclose(f); return NULL; }
        off = (size_t)e->lho + 30u + rd16(lh + 26) + rd16(lh + 28);
        raw = (uint8_t *)malloc(e->csize ? e->csize : 1);
        if (!raw) { fclose(f); return NULL; }
        if (fseek(f, (long)off, SEEK_SET) != 0 ||
            (e->csize && fread(raw, 1, e->csize, f) != e->csize)) {
            free(raw); fclose(f); return NULL;
        }
        fclose(f);
        if (e->method == 0) {
            *len = e->csize;
            return raw;
        }
        out = (uint8_t *)malloc(e->usize ? e->usize : 1);
        if (!out) { free(raw); return NULL; }
        {
            size_t got = 0;
            int ok = img_inflate(raw, e->csize, out, e->usize, &got);
            free(raw);
            if (!ok || got != e->usize) { free(out); return NULL; }
        }
        *len = e->usize;
        return out;
    }
    return NULL;
}

static uint8_t *vfs_read(const char *name, size_t *len)
{
    char path[2048];
    if (s_vfs.is_zip) return zip_read(name, len);
    snprintf(path, sizeof path, "%s/%s", s_vfs.dir, name);
    return read_whole(path, len);
}

typedef void (*RootCb)(const char *name);

static void vfs_list_root(RootCb cb)
{
    if (s_vfs.is_zip) {
        int i;
        for (i = 0; i < s_vfs.nents; i++)
            if (!strchr(s_vfs.ents[i].name, '/')) cb(s_vfs.ents[i].name);
        return;
    }
#if defined(_WIN32)
    {
        char pat[2048];
        WIN32_FIND_DATAA fd;
        HANDLE hh;
        snprintf(pat, sizeof pat, "%s/*", s_vfs.dir);
        hh = FindFirstFileA(pat, &fd);
        if (hh == INVALID_HANDLE_VALUE) return;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) cb(fd.cFileName);
        } while (FindNextFileA(hh, &fd));
        FindClose(hh);
    }
#else
    {
        DIR *d = opendir(s_vfs.dir);
        struct dirent *de;
        if (!d) return;
        while ((de = readdir(d))) {
            struct stat st;
            char path[2048];
            snprintf(path, sizeof path, "%s/%s", s_vfs.dir, de->d_name);
            if (stat(path, &st) == 0 && !S_ISDIR(st.st_mode)) cb(de->d_name);
        }
        closedir(d);
    }
#endif
}

typedef struct Alias {
    struct Alias *next;
    uint64_t cachekey;
    uint32_t hash;
    int      nlev;
    int      failed;
    int      logged;
    char    *file[TEXREP_MAX_LEVELS];
} Alias;

typedef struct Filt {
    struct Filt *next;
    uint64_t cachekey;
    uint32_t hash;
    int      filter;
} Filt;

typedef struct Range {
    struct Range *next;
    uint64_t key;
    uint32_t a, b;
} Range;

static Alias **s_alias;
static uint32_t s_alias_buckets;
static int      s_alias_n;

static Filt   **s_filt;
static uint32_t s_filt_buckets;

static Range  **s_ranges;
static uint32_t s_ranges_buckets;

static Range  **s_reduce;
static uint32_t s_reduce_buckets;

static int      s_hash_kind = HK_QUICK;
static int      s_hash_seen;
static int      s_ignore_address;
static int      s_reduce_hash;
static float    s_reduce_global = 0.5f;
static int      s_skip_dxt1_128x64;
static int      s_skip_dxt1_128x128;

static int      s_ready;
static int      s_log = -1;
static int      s_replace_on;
static int      s_dump_on;
static char     s_base[1024];
static char     s_newdir[1088];
static int      s_newdir_ok;
static unsigned s_gen;
static char     s_note[256];

static uint32_t mix64(uint64_t v)
{
    v ^= v >> 33; v *= 0xFF51AFD7ED558CCDull;
    v ^= v >> 29; v *= 0xC4CEB9FE1A85EC53ull;
    v ^= v >> 32;
    return (uint32_t)v;
}

static uint32_t key_hash(uint64_t ck, uint32_t h)
{
    return mix64(ck ^ ((uint64_t)h * 0x9E3779B97F4A7C15ull));
}

static void alias_grow(void)
{
    uint32_t nb = s_alias_buckets ? s_alias_buckets * 2 : 1024;
    Alias **nt = (Alias **)calloc(nb, sizeof(Alias *));
    uint32_t i;
    if (!nt) return;
    for (i = 0; i < s_alias_buckets; i++) {
        Alias *a = s_alias[i];
        while (a) {
            Alias *nx = a->next;
            uint32_t b = key_hash(a->cachekey, a->hash) & (nb - 1);
            a->next = nt[b];
            nt[b] = a;
            a = nx;
        }
    }
    free(s_alias);
    s_alias = nt;
    s_alias_buckets = nb;
}

static Alias *alias_get(uint64_t ck, uint32_t h)
{
    Alias *a;
    if (!s_alias_buckets) return NULL;
    a = s_alias[key_hash(ck, h) & (s_alias_buckets - 1)];
    while (a) {
        if (a->cachekey == ck && a->hash == h) return a;
        a = a->next;
    }
    return NULL;
}

static Alias *alias_put(uint64_t ck, uint32_t h)
{
    Alias *a = alias_get(ck, h);
    uint32_t b;
    if (a) return a;
    if (!s_alias_buckets || (uint32_t)s_alias_n >= s_alias_buckets) alias_grow();
    if (!s_alias_buckets) return NULL;
    a = (Alias *)calloc(1, sizeof(Alias));
    if (!a) return NULL;
    a->cachekey = ck;
    a->hash = h;
    b = key_hash(ck, h) & (s_alias_buckets - 1);
    a->next = s_alias[b];
    s_alias[b] = a;
    s_alias_n++;
    return a;
}

static Filt *filt_get(uint64_t ck, uint32_t h)
{
    Filt *f;
    if (!s_filt_buckets) return NULL;
    f = s_filt[key_hash(ck, h) & (s_filt_buckets - 1)];
    while (f) {
        if (f->cachekey == ck && f->hash == h) return f;
        f = f->next;
    }
    return NULL;
}

static void filt_put(uint64_t ck, uint32_t h, int filter)
{
    Filt *f = filt_get(ck, h);
    uint32_t b;
    if (f) { f->filter = filter; return; }
    if (!s_filt_buckets) {
        s_filt = (Filt **)calloc(256, sizeof(Filt *));
        if (!s_filt) return;
        s_filt_buckets = 256;
    }
    f = (Filt *)calloc(1, sizeof(Filt));
    if (!f) return;
    f->cachekey = ck; f->hash = h; f->filter = filter;
    b = key_hash(ck, h) & (s_filt_buckets - 1);
    f->next = s_filt[b];
    s_filt[b] = f;
}

static Range *range_get(Range **tab, uint32_t buckets, uint64_t key)
{
    Range *r;
    if (!buckets || !tab) return NULL;
    r = tab[mix64(key) & (buckets - 1)];
    while (r) {
        if (r->key == key) return r;
        r = r->next;
    }
    return NULL;
}

static void range_put(Range ***tab, uint32_t *buckets, uint64_t key, uint32_t a, uint32_t b)
{
    Range *r;
    uint32_t bi;
    if (!*buckets) {
        *tab = (Range **)calloc(256, sizeof(Range *));
        if (!*tab) return;
        *buckets = 256;
    }
    r = range_get(*tab, *buckets, key);
    if (r) { r->a = a; r->b = b; return; }
    r = (Range *)calloc(1, sizeof(Range));
    if (!r) return;
    r->key = key; r->a = a; r->b = b;
    bi = mix64(key) & (*buckets - 1);
    r->next = (*tab)[bi];
    (*tab)[bi] = r;
}

static void tables_free(void)
{
    uint32_t i;
    int k;
    for (i = 0; i < s_alias_buckets; i++) {
        Alias *a = s_alias[i];
        while (a) {
            Alias *nx = a->next;
            for (k = 0; k < TEXREP_MAX_LEVELS; k++) free(a->file[k]);
            free(a);
            a = nx;
        }
    }
    free(s_alias); s_alias = NULL; s_alias_buckets = 0; s_alias_n = 0;

    for (i = 0; i < s_filt_buckets; i++) {
        Filt *f = s_filt[i];
        while (f) { Filt *nx = f->next; free(f); f = nx; }
    }
    free(s_filt); s_filt = NULL; s_filt_buckets = 0;

    for (i = 0; i < s_ranges_buckets; i++) {
        Range *r = s_ranges[i];
        while (r) { Range *nx = r->next; free(r); r = nx; }
    }
    free(s_ranges); s_ranges = NULL; s_ranges_buckets = 0;

    for (i = 0; i < s_reduce_buckets; i++) {
        Range *r = s_reduce[i];
        while (r) { Range *nx = r->next; free(r); r = nx; }
    }
    free(s_reduce); s_reduce = NULL; s_reduce_buckets = 0;
}

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = 0;
    return s;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_hashname(const char *s, uint64_t *ck, uint32_t *hash, int *level)
{
    uint64_t v = 0;
    uint32_t h = 0;
    int i;
    for (i = 0; i < 16; i++) {
        int d = hexval((unsigned char)s[i]);
        if (d < 0) return 0;
        v = (v << 4) | (uint64_t)d;
    }
    for (i = 16; i < 24; i++) {
        int d = hexval((unsigned char)s[i]);
        if (d < 0) return 0;
        h = (h << 4) | (uint32_t)d;
    }
    *ck = v;
    *hash = h;
    *level = 0;
    if (s[24] == '_') {
        int lv = 0, any = 0;
        for (i = 25; s[i]; i++) {
            if (s[i] < '0' || s[i] > '9') return 0;
            lv = lv * 10 + (s[i] - '0');
            any = 1;
        }
        if (!any) return 0;
        *level = lv;
    } else if (s[24] != 0) {
        return 0;
    }
    return 1;
}

static int has_parent_dir(const char *s)
{
    const char *p = s;
    if (!strcmp(s, "..")) return 1;
    if (!strncmp(p, "../", 3) || !strncmp(p, "..\\", 3)) return 1;
    while ((p = strpbrk(p, "/\\")) != NULL) {
        p++;
        if (!strncmp(p, "../", 3) || !strncmp(p, "..\\", 3)) return 1;
        if (!strcmp(p, "..")) return 1;
    }
    return 0;
}

static void alias_set_level(uint64_t ck, uint32_t hash, int level, const char *value)
{
    Alias *a;
    char *dup;
    size_t i;
    if (level < 0 || level >= TEXREP_MAX_LEVELS) return;
    if (has_parent_dir(value)) return;
    a = alias_put(ck, hash);
    if (!a) return;
    dup = (char *)malloc(strlen(value) + 1);
    if (!dup) return;
    strcpy(dup, value);
    for (i = 0; dup[i]; i++) if (dup[i] == '\\') dup[i] = '/';
    free(a->file[level]);
    a->file[level] = dup;
    if (level + 1 > a->nlev) a->nlev = level + 1;
}

static void scan_root_cb(const char *name)
{
    const char *dot = strrchr(name, '.');
    char stem[64];
    size_t stemlen;
    uint64_t ck;
    uint32_t hash;
    int level;

    if (!dot || name[0] == '.') return;
    if (!name_eq_ci(dot, ".png") && !name_eq_ci(dot, ".dds") &&
        !name_eq_ci(dot, ".ktx2")) return;
    stemlen = (size_t)(dot - name);
    if (stemlen < 24 || stemlen >= sizeof stem) return;
    memcpy(stem, name, stemlen);
    stem[stemlen] = 0;
    if (!parse_hashname(stem, &ck, &hash, &level)) return;
    alias_set_level(ck, hash, level, name);
}

static int parse_uint(const char *s, uint32_t *out)
{
    char *end;
    unsigned long v;
    if (!*s) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) v = strtoul(s + 2, &end, 16);
    else                                             v = strtoul(s, &end, 10);
    if (*end) return 0;
    *out = (uint32_t)v;
    return 1;
}

static int split_commas(char *s, char *parts[], int max)
{
    int n = 0;
    char *p = s;
    parts[n++] = p;
    while (n < max && (p = strchr(p, ',')) != NULL) {
        *p++ = 0;
        parts[n++] = p;
    }
    if (strchr(parts[n - 1], ',')) return -1;
    return n;
}

static void parse_hashrange(char *key, char *value)
{
    char *kp[4], *vp[4];
    char hexbuf[64];
    uint32_t addr, fw, fh, tw, th;
    const char *k0;

    if (split_commas(key, kp, 4) != 3) return;
    if (split_commas(value, vp, 4) != 2) return;
    k0 = trim(kp[0]);
    if (k0[0] == '0' && (k0[1] == 'x' || k0[1] == 'X'))
        snprintf(hexbuf, sizeof hexbuf, "%s", k0);
    else
        snprintf(hexbuf, sizeof hexbuf, "0x%s", k0);
    if (!parse_uint(hexbuf, &addr)) return;
    if (!parse_uint(trim(kp[1]), &fw) || !parse_uint(trim(kp[2]), &fh)) return;
    if (!parse_uint(trim(vp[0]), &tw) || !parse_uint(trim(vp[1]), &th)) return;
    if (tw > fw || th > fh) return;
    range_put(&s_ranges, &s_ranges_buckets,
              ((uint64_t)addr << 32) | ((uint64_t)fw << 16) | fh, tw, th);
}

static void parse_reducerange(char *key, char *value)
{
    char *kp[4], *vp[4];
    uint32_t fw, fh, bits;
    float v;

    if (split_commas(key, kp, 4) != 2) return;
    if (split_commas(value, vp, 4) != 1) return;
    if (!parse_uint(trim(kp[0]), &fw) || !parse_uint(trim(kp[1]), &fh)) return;
    v = (float)atof(trim(vp[0]));
    if (v == 0.0f) return;
    memcpy(&bits, &v, 4);
    range_put(&s_reduce, &s_reduce_buckets, ((uint64_t)fw << 16) | fh, bits, 0);
}

static int bool_val(const char *v)
{
    return name_eq_ci(v, "true") || !strcmp(v, "1") || name_eq_ci(v, "yes");
}

static void apply_ini_line(const char *sect, char *key, char *value,
                           int is_override, char *gameover, size_t gn)
{
    if (!strcmp(sect, "options")) {
        if (!strcmp(key, "hash")) {
            if (name_eq_ci(value, "quick"))      { s_hash_kind = HK_QUICK; s_hash_seen = 1; }
            else if (name_eq_ci(value, "xxh32")) { s_hash_kind = HK_XXH32; s_hash_seen = 1; }
            else if (name_eq_ci(value, "xxh64")) { s_hash_kind = HK_XXH64; s_hash_seen = 1; }
            else if (!is_override || *value)
                fprintf(stderr, "[tex] textures.ini: unsupported hash type '%s'\n", value);
        } else if (!strcmp(key, "ignoreAddress")) {
            s_ignore_address = bool_val(value);
        } else if (!strcmp(key, "reduceHash")) {
            s_reduce_hash = bool_val(value);
        } else if (!strcmp(key, "skipLastDXT1Blocks128x64")) {
            s_skip_dxt1_128x64 = atoi(value);
        } else if (!strcmp(key, "skipLastDXT1Blocks128x128")) {
            s_skip_dxt1_128x128 = atoi(value);
        }
        return;
    }
    if (!strcmp(sect, "games")) {
        const char *id = getenv("PSPRECO_TEXTURES_ID");
        if (!id || !*id) id = "ULUS10176";
        if (gameover && !strcmp(key, id) && strcmp(value, "true") != 0)
            snprintf(gameover, gn, "%s", value);
        return;
    }
    if (!strcmp(sect, "hashes")) {
        uint64_t ck;
        uint32_t hash;
        int level;
        if (strlen(key) < 24 || !parse_hashname(key, &ck, &hash, &level)) return;
        if (!*value) {
            Alias *a = alias_put(ck, hash);
            if (a && level == 0) {
                free(a->file[0]);
                a->file[0] = NULL;
                if (a->nlev < 1) a->nlev = 1;
            }
            return;
        }
        alias_set_level(ck, hash, level, value);
        return;
    }
    if (!strcmp(sect, "filtering")) {
        uint64_t ck;
        uint32_t hash;
        int level, f;
        if (strlen(key) < 24 || !parse_hashname(key, &ck, &hash, &level)) return;
        if (name_eq_ci(value, "nearest"))     f = TEXREP_FILTER_NEAREST;
        else if (name_eq_ci(value, "linear")) f = TEXREP_FILTER_LINEAR;
        else if (name_eq_ci(value, "auto"))   f = TEXREP_FILTER_AUTO;
        else return;
        filt_put(ck, hash, f);
        return;
    }
    if (!strcmp(sect, "hashranges")) parse_hashrange(key, value);
    else if (!strcmp(sect, "reducehashranges")) parse_reducerange(key, value);
}

static int load_ini(const char *filename, int is_override, char *gameover, size_t gn)
{
    size_t len = 0;
    uint8_t *data = vfs_read(filename, &len);
    char sect[64] = "";
    char *p, *endp;

    if (!data) return 0;
    p = (char *)data;
    endp = p + len;
    while (p < endp) {
        char *nl = (char *)memchr(p, '\n', (size_t)(endp - p));
        char *line = p;
        char *s, *eq;
        if (nl) { *nl = 0; p = nl + 1; } else { p = endp; }
        s = trim(line);
        if (!*s || *s == '#' || *s == ';') continue;
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (!e) continue;
            *e = 0;
            snprintf(sect, sizeof sect, "%s", s + 1);
            continue;
        }
        eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        apply_ini_line(sect, trim(s), trim(eq + 1), is_override, gameover, gn);
    }
    free(data);
    return 1;
}

static int dir_exists(const char *path)
{
#if defined(_WIN32)
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static int file_exists(const char *path)
{
#if defined(_WIN32)
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    struct stat st;
    return stat(path, &st) == 0 && !S_ISDIR(st.st_mode);
#endif
}

static int pack_candidate(const char *dir)
{
    char probe[1100];
    if (!dir_exists(dir)) return 0;
    snprintf(probe, sizeof probe, "%s/textures.ini", dir);
    if (file_exists(probe)) return 1;
    snprintf(probe, sizeof probe, "%s/textures.zip", dir);
    return file_exists(probe);
}

static const char *const k_game_ids[] = {
    "ULUS10176", "ULJS00086", "UCES00423", "ULJP00749"
};

static void find_pack_dir(void)
{
    const char *env = getenv("PSPRECO_TEXTURES");
    char probe[1024];
    size_t i;

    s_base[0] = 0;
    if (env && *env) {
        snprintf(s_base, sizeof s_base, "%s", env);
        return;
    }
    for (i = 0; i < sizeof k_game_ids / sizeof k_game_ids[0]; i++) {
        snprintf(probe, sizeof probe, "textures/%s", k_game_ids[i]);
        if (pack_candidate(probe)) { snprintf(s_base, sizeof s_base, "%s", probe); return; }
    }
    if (pack_candidate("textures")) { snprintf(s_base, sizeof s_base, "%s", "textures"); return; }
    for (i = 0; i < sizeof k_game_ids / sizeof k_game_ids[0]; i++) {
        snprintf(probe, sizeof probe, "textures/%s", k_game_ids[i]);
        if (dir_exists(probe)) { snprintf(s_base, sizeof s_base, "%s", probe); return; }
    }
    snprintf(s_base, sizeof s_base, "textures/%s", k_game_ids[0]);
}

static void reset_options(void)
{
    s_hash_kind = HK_QUICK;
    s_hash_seen = 0;
    s_ignore_address = 0;
    s_reduce_hash = 0;
    s_reduce_global = 0.5f;
    s_skip_dxt1_128x64 = 0;
    s_skip_dxt1_128x128 = 0;
}

static void pack_close(void)
{
    tables_free();
    zip_close();
    s_vfs.is_zip = 0;
    s_ready = 0;
}

static void pack_open(void)
{
    char zip[1088];
    char gameover[256];

    pack_close();
    reset_options();
    s_note[0] = 0;

    snprintf(s_vfs.dir, sizeof s_vfs.dir, "%s", s_base);
    snprintf(zip, sizeof zip, "%s/textures.zip", s_base);
    snprintf(s_vfs.zip, sizeof s_vfs.zip, "%s", zip);
    s_vfs.is_zip = file_exists(zip) && zip_open(zip);

    vfs_list_root(scan_root_cb);

    gameover[0] = 0;
    if (load_ini("textures.ini", 0, gameover, sizeof gameover)) {
        if (gameover[0] && strcmp(gameover, "textures.ini") != 0) {
            if (!load_ini(gameover, 1, NULL, 0))
                snprintf(s_note, sizeof s_note, "override ini '%s' missing", gameover);
        }
        if (!s_hash_seen)
            snprintf(s_note, sizeof s_note, "no [options] hash, assuming quick");
    } else {
        snprintf(s_note, sizeof s_note, "no textures.ini, hash-named files only");
    }

    if (s_ignore_address && s_hash_kind == HK_QUICK) s_ignore_address = 0;
    if (s_reduce_hash && s_hash_kind == HK_QUICK) s_reduce_hash = 0;

    s_ready = s_alias_n > 0;
}

static void announce(void)
{
    if (!s_replace_on) return;
    if (s_ready)
        printf("[tex] replacement pack: %s%s (%d entries)\n", s_base,
               s_vfs.is_zip ? "/textures.zip" : "", s_alias_n);
    else
        printf("[tex] no replacement textures found in %s\n", s_base);
}

void texrep_init(void)
{
    find_pack_dir();
    snprintf(s_newdir, sizeof s_newdir, "%s/new", s_base);
    s_gen = g_gfx_generation;
    s_replace_on = g_gfx.tex_replace;
    s_dump_on = g_gfx.tex_dump;
    if (s_replace_on || s_dump_on) {
        pack_open();
        announce();
    }
}

void texrep_shutdown(void)
{
    pack_close();
}

void texrep_reload(void)
{
    pack_close();
    texrep_init();
}

static void sync_settings(void)
{
    if (s_gen == g_gfx_generation) return;
    s_gen = g_gfx_generation;
    s_dump_on = g_gfx.tex_dump;
    if (g_gfx.tex_replace != s_replace_on) {
        s_replace_on = g_gfx.tex_replace;
        if (s_replace_on || s_dump_on) {
            pack_open();
            announce();
        } else {
            pack_close();
        }
    } else if (s_dump_on && !s_ready) {
        pack_open();
    }
}

int texrep_replace_on(void)
{
    sync_settings();
    return s_replace_on && s_ready;
}

int texrep_dump_on(void)
{
    sync_settings();
    return s_dump_on;
}

void texrep_status(char *out, int n)
{
    if (n <= 0) return;
    if (!s_replace_on)   snprintf(out, (size_t)n, "off");
    else if (!s_ready)   snprintf(out, (size_t)n, "no pack in %s", s_base);
    else                 snprintf(out, (size_t)n, "%s: %d entries%s%s", s_base, s_alias_n,
                                  s_note[0] ? " -- " : "", s_note);
}

static int lookup_hashrange(uint32_t addr, int w, int h, int *nw, int *nh)
{
    uint64_t k = ((uint64_t)addr << 32) | ((uint64_t)(uint32_t)w << 16) | (uint32_t)h;
    Range *r = range_get(s_ranges, s_ranges_buckets, k);
    if (r) { *nw = (int)r->a; *nh = (int)r->b; return 1; }
    *nw = w; *nh = h;
    return 0;
}

static float lookup_reduce(int w, int h)
{
    uint64_t k = ((uint64_t)(uint32_t)w << 16) | (uint32_t)h;
    Range *r = range_get(s_reduce, s_reduce_buckets, k);
    if (r) {
        float v;
        memcpy(&v, &r->a, 4);
        return v;
    }
    return s_reduce_global;
}

static uint32_t hash_bytes(const uint8_t *p, uint32_t size)
{
    switch (s_hash_kind) {
    case HK_XXH32: return xxh32(p, size, 0xBACD7814u);
    case HK_XXH64: return (uint32_t)xxh64(p, size, 0xBACD7814u);
    default:       return quick_tex_hash(p, size);
    }
}

static uint32_t compute_hash(uint32_t addr, int bufw, int w, int h, uint32_t fmt)
{
    const uint8_t *p = TR_HOST(addr);
    uint32_t bpp = k_bpp[fmt & 15];
    float reduce = 1.0f;

    if (bpp == 0 || w <= 0 || h <= 0) return 0;
    if (s_reduce_hash) reduce = lookup_reduce(w, h);

    if (bufw <= w) {
        uint32_t total = (uint32_t)(bufw * h + (w - bufw));
        uint32_t size = (bpp * total) / 8u;
        if (reduce != 1.0f) size = (uint32_t)((float)size * reduce);
        if (max_size_at(addr) < size) return 0;
        if (s_skip_dxt1_128x64 && fmt == 8 && w == 128 && h == 64)
            size -= 8u * (uint32_t)s_skip_dxt1_128x64;
        if (s_skip_dxt1_128x128 && fmt == 8 && w == 128 && h == 128)
            size -= 8u * (uint32_t)s_skip_dxt1_128x128;
        return hash_bytes(p, size);
    } else {
        uint32_t bpl = (bpp * (uint32_t)w) / 8u;
        uint32_t stride = (bpp * (uint32_t)bufw) / 8u;
        uint32_t result = 0;
        int y;
        if (reduce != 1.0f) bpl = (uint32_t)((float)bpl * reduce);
        for (y = 0; y < h; y++) {
            result = (result * 11u) ^ hash_bytes(p, bpl);
            p += stride;
        }
        return result;
    }
}

static uint32_t clut_hash_of(const GeState *g)
{
    static uint32_t cached;
    static uint32_t cgen = 0xFFFFFFFFu, cfmt = 0xFFFFFFFFu;
    static uint32_t cload = 0xFFFFFFFFu, cmax = 0xFFFFFFFFu;
    uint32_t clutfmt, base, base_bytes, total, ext;

    if (cgen == g->clut_gen && cfmt == g->clut_fmt &&
        cload == g->clut_load_bytes && cmax == g->clut_max_bytes)
        return cached;

    clutfmt = 0xC5000000u | (g->clut_fmt & 0x00FFFFFFu);
    base = ((g->clut_fmt >> 16) & 0x1Fu) << 4;
    base_bytes = ((g->clut_fmt & 3u) == 3u) ? base * 4u : base * 2u;
    total = g->clut_load_bytes + base_bytes;
    ext = total < g->clut_max_bytes ? total : g->clut_max_bytes;
    if (ext > sizeof g->clutram) ext = (uint32_t)sizeof g->clutram;

    cached = xxh32(g->clutram, ext, 0xC0108888u) ^ clutfmt;
    cgen = g->clut_gen;
    cfmt = g->clut_fmt;
    cload = g->clut_load_bytes;
    cmax = g->clut_max_bytes;
    return cached;
}

static int ilog2i(int v)
{
    int n = 0;
    while (n < 15 && (1 << n) < v) n++;
    return n;
}

int texrep_identify(TexRepId *id)
{
    const GeState *g = ge_state_ptr();
    uint32_t addr, fmt, dim, cluthash = 0;
    int w = g->tex_w, h = g->tex_h, bufw, hw, hh;

    if (w <= 0 || h <= 0) return 0;

    addr = g->tex_addr & 0x0FFFFFF0u;
    fmt = g->tex_fmt & 0xFu;
    if (fmt >= 11) fmt = 0;

    dim = (uint32_t)(ilog2i(w) | (ilog2i(h) << 8)) & 0xF0Fu;
    if (fmt >= 4 && fmt <= 7) cluthash = clut_hash_of(g);

    id->cachekey = ((uint64_t)(addr & 0x3FFFFFFFu) << 32) | dim;
    if (fmt & 4u) id->cachekey ^= cluthash;
    id->psp_w = w;
    id->psp_h = h;

    if (addr < TR_KERNEL_END) {
        bufw = (int)(g->tex_bufw & 0x1FFFu);
    } else {
        bufw = (int)(g->tex_bufw & 0x7FFu);
        if (bufw == 0 && k_bpp[fmt]) bufw = (8 * 16) / (int)k_bpp[fmt];
    }
    if (bufw <= 0) return 0;

    id->has_alt = 0;
    id->alt_hash = 0;
    if (lookup_hashrange(addr, w, h, &hw, &hh)) {
        id->hash = compute_hash(addr, bufw, hw, hh, fmt);
    } else {
        id->hash = compute_hash(addr, bufw, w, h, fmt);
        if (h == 512) {
            id->alt_hash = compute_hash(addr, bufw, w, 272, fmt);
            id->has_alt = id->alt_hash != id->hash;
        }
    }
    return 1;
}

static Alias *alias_wildcard(uint64_t cachekey, uint32_t hash)
{
    Alias *a;
    if ((a = alias_get(cachekey, hash)) != NULL) return a;
    if ((a = alias_get(cachekey & 0xFFFFFFFFull, 0)) != NULL) return a;
    if (!s_ignore_address && (a = alias_get(cachekey, 0)) != NULL) return a;
    if ((a = alias_get(cachekey & 0xFFFFFFFFull, hash)) != NULL) return a;
    if (!s_ignore_address) {
        if ((a = alias_get(cachekey & ~0xFFFFFFFFull, hash)) != NULL) return a;
        if ((a = alias_get(cachekey & ~0xFFFFFFFFull, 0)) != NULL) return a;
    }
    return alias_get(0, hash);
}

static int find_filtering(uint64_t cachekey, uint32_t hash)
{
    Filt *f;
    if ((f = filt_get(cachekey, hash)) != NULL) return f->filter;
    if ((f = filt_get(cachekey & 0xFFFFFFFFull, 0)) != NULL) return f->filter;
    if (!s_ignore_address && (f = filt_get(cachekey, 0)) != NULL) return f->filter;
    if ((f = filt_get(cachekey & 0xFFFFFFFFull, hash)) != NULL) return f->filter;
    if (!s_ignore_address) {
        if ((f = filt_get(cachekey & ~0xFFFFFFFFull, hash)) != NULL) return f->filter;
        if ((f = filt_get(cachekey & ~0xFFFFFFFFull, 0)) != NULL) return f->filter;
    }
    if ((f = filt_get(0, hash)) != NULL) return f->filter;
    if ((f = filt_get(0, 0)) != NULL) return f->filter;
    return TEXREP_FILTER_AUTO;
}

static int load_file_levels(const char *name, uint32_t **px, int *w, int *h, int maxlev)
{
    size_t len = 0;
    uint8_t *data = vfs_read(name, &len);
    int got = 0;

    if (!data) {
        fprintf(stderr, "[tex] replacement file missing: %s\n", name);
        return 0;
    }
    if (ktx2_is_ktx2(data, len)) {
        const char *err = NULL;
        got = ktx2_decode_levels(data, len, maxlev, px, w, h, &err);
        if (!got) fprintf(stderr, "[tex] %s: %s\n", name, err ? err : "decode failed");
    } else {
        px[0] = img_decode(data, len, &w[0], &h[0]);
        if (px[0]) got = 1;
        else fprintf(stderr, "[tex] cannot decode replacement: %s\n", name);
    }
    free(data);
    return got;
}

static void pad_level(TexRepImage *img, int lv, int fullw, int fullh)
{
    uint32_t *big;
    int y;
    if (fullw == img->w[lv] && fullh == img->h[lv]) return;
    if (fullw < img->w[lv] || fullh < img->h[lv]) return;
    big = (uint32_t *)calloc((size_t)fullw * (size_t)fullh, 4);
    if (!big) return;
    for (y = 0; y < img->h[lv]; y++)
        memcpy(big + (size_t)y * (size_t)fullw,
               img->px[lv] + (size_t)y * (size_t)img->w[lv],
               (size_t)img->w[lv] * 4);
    free(img->px[lv]);
    img->px[lv] = big;
    img->w[lv] = fullw;
    img->h[lv] = fullh;
}

void texrep_release(TexRepImage *img)
{
    int i;
    for (i = 0; i < TEXREP_MAX_LEVELS; i++) {
        free(img->px[i]);
        img->px[i] = NULL;
    }
    img->levels = 0;
}

int texrep_find(TexRepId *id, TexRepImage *img)
{
    Alias *a;
    uint32_t used_hash;
    uint64_t key;
    int lv, neww, newh;

    if (!texrep_replace_on()) return 0;

    key = id->cachekey;
    if (s_ignore_address) key &= 0xFFFFFFFFull;

    used_hash = id->hash;
    a = alias_wildcard(key, used_hash);
    if (!a && id->has_alt) {
        used_hash = id->alt_hash;
        a = alias_wildcard(key, used_hash);
    }
    if (!a || a->failed || a->nlev <= 0 || !a->file[0]) return 0;

    memset(img, 0, sizeof *img);
    img->filtering = find_filtering(key, used_hash);

    for (lv = 0; lv < a->nlev && lv < TEXREP_MAX_LEVELS;) {
        int got;
        if (!a->file[lv]) break;
        got = load_file_levels(a->file[lv], &img->px[lv], &img->w[lv], &img->h[lv],
                               TEXREP_MAX_LEVELS - lv);
        if (got <= 0) break;
        if (img->w[lv] > TEXREP_MAX_DIM || img->h[lv] > TEXREP_MAX_DIM) {
            fprintf(stderr, "[tex] %s is %dx%d, too large to use\n",
                    a->file[lv], img->w[lv], img->h[lv]);
            break;
        }
        if (lv > 0) {
            int ew = img->w[0] >> lv, eh = img->h[0] >> lv;
            if (ew < 1) ew = 1;
            if (eh < 1) eh = 1;
            if (img->w[lv] != ew || img->h[lv] != eh) {
                fprintf(stderr, "[tex] mip %d of %s is %dx%d, expected %dx%d\n",
                        lv, a->file[lv], img->w[lv], img->h[lv], ew, eh);
                free(img->px[lv]);
                img->px[lv] = NULL;
                break;
            }
        }
        img->levels = lv + got;
        if (got > 1) break;
        lv++;
    }

    if (!img->levels) {
        a->failed = 1;
        texrep_release(img);
        return 0;
    }

    lookup_hashrange((uint32_t)(id->cachekey >> 32), id->psp_w, id->psp_h, &neww, &newh);
    if (neww > 0 && newh > 0 && (neww != id->psp_w || newh != id->psp_h)) {
        for (lv = 0; lv < img->levels; lv++) {
            int fw = (int)(((long long)img->w[lv] * id->psp_w) / neww);
            int fh = (int)(((long long)img->h[lv] * id->psp_h) / newh);
            pad_level(img, lv, fw, fh);
        }
    }

    if (s_log < 0) {
        const char *e = getenv("PSPRECO_TEX_LOG");
        s_log = (e && e[0] && strcmp(e, "0") != 0) ? 1 : 0;
    }
    if (s_log && !a->logged) {
        a->logged = 1;
        fprintf(stderr, "[tex] %3dx%-3d -> %4dx%-4d %.2fx %d level(s) %016llx%08x  %s\n",
                id->psp_w, id->psp_h, img->w[0], img->h[0],
                (double)img->w[0] / (double)(id->psp_w ? id->psp_w : 1),
                img->levels, (unsigned long long)id->cachekey, used_hash, a->file[0]);
    }

    id->hash = used_hash;
    return 1;
}

typedef struct Dumped {
    struct Dumped *next;
    uint64_t cachekey;
    uint32_t hash;
} Dumped;

static Dumped **s_dumped;
static uint32_t s_dumped_buckets;

static int dumped_mark(uint64_t ck, uint32_t h)
{
    Dumped *d;
    uint32_t b;
    if (!s_dumped_buckets) {
        s_dumped = (Dumped **)calloc(4096, sizeof(Dumped *));
        if (!s_dumped) return 1;
        s_dumped_buckets = 4096;
    }
    b = key_hash(ck, h) & (s_dumped_buckets - 1);
    for (d = s_dumped[b]; d; d = d->next)
        if (d->cachekey == ck && d->hash == h) return 1;
    d = (Dumped *)calloc(1, sizeof(Dumped));
    if (!d) return 1;
    d->cachekey = ck;
    d->hash = h;
    d->next = s_dumped[b];
    s_dumped[b] = d;
    return 0;
}

void texrep_dump(const TexRepId *id, const uint32_t *rgba, int w, int h)
{
    char name[64];
    char path[2048];

    if (!texrep_dump_on() || w <= 0 || h <= 0) return;
    if (dumped_mark(id->cachekey, id->hash)) return;
    if (alias_wildcard(id->cachekey, id->hash)) return;

    if (!s_newdir_ok) {
        TR_MKDIR("textures");
        TR_MKDIR(s_base);
        TR_MKDIR(s_newdir);
        s_newdir_ok = dir_exists(s_newdir);
        if (!s_newdir_ok) {
            fprintf(stderr, "[tex] cannot create dump directory '%s'\n", s_newdir);
            return;
        }
    }

    snprintf(name, sizeof name, "%016llx%08x",
             (unsigned long long)id->cachekey, id->hash);
    snprintf(path, sizeof path, "%s/%s.png", s_base, name);
    if (file_exists(path)) return;
    snprintf(path, sizeof path, "%s/%s.png", s_newdir, name);
    if (file_exists(path)) return;

    if (!img_write_png(path, rgba, w, h))
        fprintf(stderr, "[tex] failed to write '%s'\n", path);
}
