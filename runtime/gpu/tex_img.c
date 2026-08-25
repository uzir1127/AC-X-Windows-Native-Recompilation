#include "tex_img.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HUFF_MAX_BITS   15
#define HUFF_TAB_MAX    (1 << HUFF_MAX_BITS)

typedef struct {
    const uint8_t *src;
    size_t len, pos;
    uint64_t bits;
    int nbits;
    int overrun;
} BitIn;

typedef struct {
    int maxbits;
    uint16_t *tab;
} Huff;

typedef struct {
    uint16_t lit[HUFF_TAB_MAX];
    uint16_t dist[HUFF_TAB_MAX];
    uint16_t clen[1 << 7];
} InfTabs;

static const uint16_t k_len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
    67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t k_len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
    4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t k_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
    769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const uint8_t k_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
static const uint8_t k_clen_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static void br_init(BitIn *b, const uint8_t *src, size_t len)
{
    b->src = src; b->len = len; b->pos = 0;
    b->bits = 0; b->nbits = 0; b->overrun = 0;
}

static void br_refill(BitIn *b)
{
    while (b->nbits <= 56) {
        uint8_t byte = 0;
        if (b->pos < b->len) byte = b->src[b->pos++];
        else                 b->overrun++;
        b->bits |= (uint64_t)byte << b->nbits;
        b->nbits += 8;
    }
}

static uint32_t br_peek(BitIn *b, int n)
{
    if (b->nbits < n) br_refill(b);
    if (n <= 0) return 0;
    return (uint32_t)(b->bits & (((uint64_t)1 << n) - 1));
}

static void br_drop(BitIn *b, int n)
{
    b->bits >>= n;
    b->nbits -= n;
}

static uint32_t br_get(BitIn *b, int n)
{
    uint32_t v = br_peek(b, n);
    br_drop(b, n);
    return v;
}

static void br_align(BitIn *b)
{
    br_drop(b, b->nbits & 7);
}

static unsigned bitrev(unsigned v, int n)
{
    unsigned r = 0;
    int i;
    for (i = 0; i < n; i++) { r = (r << 1) | (v & 1u); v >>= 1; }
    return r;
}

static int huff_build(Huff *h, uint16_t *tab, const uint8_t *lens, int n)
{
    int cnt[HUFF_MAX_BITS + 1];
    int offs[HUFF_MAX_BITS + 2];
    uint16_t sorted[320];
    int i, len, maxb = 0, left, size, si;
    unsigned code;

    if (n > 320) return 0;
    for (i = 0; i <= HUFF_MAX_BITS; i++) cnt[i] = 0;
    for (i = 0; i < n; i++) {
        if (lens[i] > HUFF_MAX_BITS) return 0;
        cnt[lens[i]]++;
    }
    cnt[0] = 0;
    for (len = 1; len <= HUFF_MAX_BITS; len++) if (cnt[len]) maxb = len;
    h->tab = tab;
    h->maxbits = maxb;
    if (!maxb) return 1;

    left = 1;
    for (len = 1; len <= HUFF_MAX_BITS; len++) {
        left <<= 1;
        left -= cnt[len];
        if (left < 0) return 0;
    }

    offs[1] = 0;
    for (len = 1; len < HUFF_MAX_BITS; len++) offs[len + 1] = offs[len] + cnt[len];
    for (i = 0; i < n; i++)
        if (lens[i]) sorted[offs[lens[i]]++] = (uint16_t)i;

    size = 1 << maxb;
    for (i = 0; i < size; i++) tab[i] = 0xFFFFu;

    code = 0; si = 0;
    for (len = 1; len <= maxb; len++) {
        for (i = 0; i < cnt[len]; i++) {
            uint16_t sym = sorted[si++];
            uint16_t entry = (uint16_t)(((unsigned)len << 9) | sym);
            unsigned j, step = 1u << len;
            for (j = bitrev(code, len); j < (unsigned)size; j += step) tab[j] = entry;
            code++;
        }
        code <<= 1;
    }
    return 1;
}

static int huff_decode(BitIn *b, const Huff *h)
{
    uint32_t v;
    uint16_t e;
    if (!h->maxbits) return -1;
    v = br_peek(b, h->maxbits);
    e = h->tab[v];
    if (e == 0xFFFFu) return -1;
    br_drop(b, (int)(e >> 9));
    return (int)(e & 0x1FFu);
}

static int inflate_block(BitIn *b, const Huff *lit, const Huff *dist,
                         uint8_t *dst, size_t dstlen, size_t *outp)
{
    size_t out = *outp;
    for (;;) {
        int sym = huff_decode(b, lit);
        if (sym < 0) return 0;
        if (sym < 256) {
            if (out >= dstlen) return 0;
            dst[out++] = (uint8_t)sym;
        } else if (sym == 256) {
            *outp = out;
            return 1;
        } else {
            int li = sym - 257, di;
            size_t length, d, from;
            if (li >= 29) return 0;
            length = (size_t)k_len_base[li] + br_get(b, k_len_extra[li]);
            di = huff_decode(b, dist);
            if (di < 0 || di >= 30) return 0;
            d = (size_t)k_dist_base[di] + br_get(b, k_dist_extra[di]);
            if (d > out) return 0;
            if (length > dstlen - out) return 0;
            from = out - d;
            while (length--) dst[out++] = dst[from++];
        }
        if (b->overrun > 8) return 0;
    }
}

int img_inflate(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen,
                size_t *produced)
{
    BitIn b;
    InfTabs *t;
    Huff lit, dist, clen;
    size_t out = 0;
    int final = 0, ok = 1;

    t = (InfTabs *)malloc(sizeof(InfTabs));
    if (!t) return 0;
    br_init(&b, src, srclen);

    while (!final && ok) {
        int type;
        final = (int)br_get(&b, 1);
        type = (int)br_get(&b, 2);
        if (type == 0) {
            unsigned n, inv;
            br_align(&b);
            n = br_get(&b, 16);
            inv = br_get(&b, 16);
            if ((n ^ 0xFFFFu) != inv) { ok = 0; break; }
            if ((size_t)n > dstlen - out) { ok = 0; break; }
            while (n--) dst[out++] = (uint8_t)br_get(&b, 8);
            if (b.overrun > 8) { ok = 0; break; }
        } else if (type == 1) {
            uint8_t ll[288], dl[30];
            int i;
            for (i = 0; i < 144; i++) ll[i] = 8;
            for (; i < 256; i++) ll[i] = 9;
            for (; i < 280; i++) ll[i] = 7;
            for (; i < 288; i++) ll[i] = 8;
            for (i = 0; i < 30; i++) dl[i] = 5;
            if (!huff_build(&lit, t->lit, ll, 288) ||
                !huff_build(&dist, t->dist, dl, 30)) { ok = 0; break; }
            ok = inflate_block(&b, &lit, &dist, dst, dstlen, &out);
        } else if (type == 2) {
            uint8_t cl[19], lens[320];
            int hlit, hdist, hclen, i, n;
            memset(cl, 0, sizeof cl);
            memset(lens, 0, sizeof lens);
            hlit  = (int)br_get(&b, 5) + 257;
            hdist = (int)br_get(&b, 5) + 1;
            hclen = (int)br_get(&b, 4) + 4;
            if (hlit > 288 || hdist > 32) { ok = 0; break; }
            for (i = 0; i < hclen; i++) cl[k_clen_order[i]] = (uint8_t)br_get(&b, 3);
            if (!huff_build(&clen, t->clen, cl, 19)) { ok = 0; break; }
            n = hlit + hdist;
            i = 0;
            while (i < n) {
                int s = huff_decode(&b, &clen);
                if (s < 0) { ok = 0; break; }
                if (s < 16) {
                    lens[i++] = (uint8_t)s;
                } else {
                    int rep;
                    uint8_t v = 0;
                    if (s == 16) {
                        if (i == 0) { ok = 0; break; }
                        v = lens[i - 1];
                        rep = 3 + (int)br_get(&b, 2);
                    } else if (s == 17) {
                        rep = 3 + (int)br_get(&b, 3);
                    } else {
                        rep = 11 + (int)br_get(&b, 7);
                    }
                    if (i + rep > n) { ok = 0; break; }
                    while (rep--) lens[i++] = v;
                }
                if (b.overrun > 8) { ok = 0; break; }
            }
            if (!ok) break;
            if (!huff_build(&lit, t->lit, lens, hlit) ||
                !huff_build(&dist, t->dist, lens + hlit, hdist)) { ok = 0; break; }
            ok = inflate_block(&b, &lit, &dist, dst, dstlen, &out);
        } else {
            ok = 0;
        }
    }

    free(t);
    if (produced) *produced = out;
    return ok;
}

static uint32_t rd32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

typedef struct {
    int w, h, depth, color, interlace;
    int channels, bpp_bytes;
    uint8_t pal[256 * 3];
    uint8_t palA[256];
    int npal;
    int has_trns;
    uint16_t trns[3];
} PngHdr;

static const int k_ct_channels[7] = { 1, 0, 3, 1, 2, 0, 4 };

static int png_row_bytes(const PngHdr *p, int w)
{
    long bits = (long)w * p->channels * p->depth;
    return (int)((bits + 7) / 8);
}

static void png_unfilter(uint8_t *cur, const uint8_t *prev, int rb, int bpp, int ft)
{
    int i;
    switch (ft) {
    case 0:
        break;
    case 1:
        for (i = bpp; i < rb; i++) cur[i] = (uint8_t)(cur[i] + cur[i - bpp]);
        break;
    case 2:
        if (prev) for (i = 0; i < rb; i++) cur[i] = (uint8_t)(cur[i] + prev[i]);
        break;
    case 3:
        for (i = 0; i < rb; i++) {
            int a = i >= bpp ? cur[i - bpp] : 0;
            int bb = prev ? prev[i] : 0;
            cur[i] = (uint8_t)(cur[i] + ((a + bb) >> 1));
        }
        break;
    default:
        for (i = 0; i < rb; i++) {
            int a = i >= bpp ? cur[i - bpp] : 0;
            int bb = prev ? prev[i] : 0;
            int c = (prev && i >= bpp) ? prev[i - bpp] : 0;
            int pp = a + bb - c;
            int pa = pp > a ? pp - a : a - pp;
            int pb = pp > bb ? pp - bb : bb - pp;
            int pc = pp > c ? pp - c : c - pp;
            int pr = (pa <= pb && pa <= pc) ? a : (pb <= pc ? bb : c);
            cur[i] = (uint8_t)(cur[i] + pr);
        }
        break;
    }
}

static unsigned png_sample(const uint8_t *row, int idx, int depth)
{
    switch (depth) {
    case 1:  return (row[idx >> 3] >> (7 - (idx & 7))) & 1u;
    case 2:  return (row[idx >> 2] >> (6 - 2 * (idx & 3))) & 3u;
    case 4:  return (row[idx >> 1] >> (4 - 4 * (idx & 1))) & 15u;
    case 8:  return row[idx];
    default: return ((unsigned)row[idx * 2] << 8) | row[idx * 2 + 1];
    }
}

static void png_emit(const PngHdr *p, const uint8_t *row, int pw,
                     uint32_t *out, int outw, int y, int x0, int dx)
{
    static const unsigned scale[17] = {
        0, 255, 85, 0, 17, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1
    };
    int i;
    for (i = 0; i < pw; i++) {
        unsigned r, g, b, a = 255;
        int base = i * p->channels;
        switch (p->color) {
        case 0: {
            unsigned v = png_sample(row, base, p->depth);
            if (p->has_trns && v == p->trns[0]) a = 0;
            v = p->depth == 16 ? (v >> 8) : v * scale[p->depth];
            r = g = b = v;
            break;
        }
        case 2: {
            unsigned rv = png_sample(row, base, p->depth);
            unsigned gv = png_sample(row, base + 1, p->depth);
            unsigned bv = png_sample(row, base + 2, p->depth);
            if (p->has_trns && rv == p->trns[0] && gv == p->trns[1] && bv == p->trns[2])
                a = 0;
            if (p->depth == 16) { rv >>= 8; gv >>= 8; bv >>= 8; }
            r = rv; g = gv; b = bv;
            break;
        }
        case 3: {
            unsigned v = png_sample(row, base, p->depth);
            if ((int)v >= p->npal) v = 0;
            r = p->pal[v * 3 + 0]; g = p->pal[v * 3 + 1]; b = p->pal[v * 3 + 2];
            a = p->palA[v];
            break;
        }
        case 4: {
            unsigned v = png_sample(row, base, p->depth);
            unsigned av = png_sample(row, base + 1, p->depth);
            if (p->depth == 16) { v >>= 8; av >>= 8; }
            r = g = b = v; a = av;
            break;
        }
        default: {
            unsigned rv = png_sample(row, base, p->depth);
            unsigned gv = png_sample(row, base + 1, p->depth);
            unsigned bv = png_sample(row, base + 2, p->depth);
            unsigned av = png_sample(row, base + 3, p->depth);
            if (p->depth == 16) { rv >>= 8; gv >>= 8; bv >>= 8; av >>= 8; }
            r = rv; g = gv; b = bv; a = av;
            break;
        }
        }
        {
            int x = x0 + i * dx;
            if (x < outw)
                out[(size_t)y * (size_t)outw + (size_t)x] =
                    r | (g << 8) | (b << 16) | (a << 24);
        }
    }
}

static uint32_t *png_decode(const uint8_t *d, size_t len, int *ow, int *oh)
{
    static const int ax[7]  = { 0, 4, 0, 2, 0, 1, 0 };
    static const int ay[7]  = { 0, 0, 4, 0, 2, 0, 1 };
    static const int axs[7] = { 8, 8, 4, 4, 2, 2, 1 };
    static const int ays[7] = { 8, 8, 8, 4, 4, 2, 2 };
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };

    PngHdr p;
    size_t pos, idat_total = 0, idat_pos = 0, raw_need = 0, got = 0;
    uint8_t *idat = NULL, *raw = NULL, *cur = NULL, *prev = NULL;
    uint32_t *out = NULL;
    int i, pass, npass;

    if (len < 8 + 25 || memcmp(d, sig, 8) != 0) return NULL;
    memset(&p, 0, sizeof p);
    for (i = 0; i < 256; i++) p.palA[i] = 255;

    pos = 8;
    while (pos + 12 <= len) {
        uint32_t clen = rd32be(d + pos);
        const uint8_t *ct = d + pos + 4;
        const uint8_t *cd = d + pos + 8;
        if (clen > len || pos + 12 + (size_t)clen > len) break;
        if (!memcmp(ct, "IHDR", 4) && clen >= 13) {
            p.w = (int)rd32be(cd);
            p.h = (int)rd32be(cd + 4);
            p.depth = cd[8]; p.color = cd[9];
            if (cd[10] != 0 || cd[11] != 0) return NULL;
            p.interlace = cd[12];
        } else if (!memcmp(ct, "PLTE", 4)) {
            p.npal = (int)(clen / 3);
            if (p.npal > 256) p.npal = 256;
            memcpy(p.pal, cd, (size_t)p.npal * 3);
        } else if (!memcmp(ct, "tRNS", 4)) {
            if (p.color == 3) {
                size_t n = clen > 256 ? 256 : clen;
                memcpy(p.palA, cd, n);
            } else if (p.color == 0 && clen >= 2) {
                p.has_trns = 1;
                p.trns[0] = (uint16_t)(((unsigned)cd[0] << 8) | cd[1]);
            } else if (p.color == 2 && clen >= 6) {
                p.has_trns = 1;
                p.trns[0] = (uint16_t)(((unsigned)cd[0] << 8) | cd[1]);
                p.trns[1] = (uint16_t)(((unsigned)cd[2] << 8) | cd[3]);
                p.trns[2] = (uint16_t)(((unsigned)cd[4] << 8) | cd[5]);
            }
        } else if (!memcmp(ct, "IDAT", 4)) {
            idat_total += clen;
        } else if (!memcmp(ct, "IEND", 4)) {
            break;
        }
        pos += 12 + (size_t)clen;
    }

    if (p.w <= 0 || p.h <= 0 || p.w > 32768 || p.h > 32768) return NULL;
    if (p.color > 6 || k_ct_channels[p.color] == 0) return NULL;
    if (p.depth != 1 && p.depth != 2 && p.depth != 4 && p.depth != 8 && p.depth != 16)
        return NULL;
    if (p.color != 0 && p.color != 3 && p.depth < 8) return NULL;
    if (p.color == 3 && p.depth == 16) return NULL;
    if (p.color == 3 && p.npal == 0) return NULL;
    if (!idat_total) return NULL;

    p.channels = k_ct_channels[p.color];
    p.bpp_bytes = (p.channels * p.depth + 7) / 8;

    idat = (uint8_t *)malloc(idat_total);
    if (!idat) return NULL;
    pos = 8;
    while (pos + 12 <= len) {
        uint32_t clen = rd32be(d + pos);
        const uint8_t *ct = d + pos + 4;
        if (clen > len || pos + 12 + (size_t)clen > len) break;
        if (!memcmp(ct, "IDAT", 4)) {
            memcpy(idat + idat_pos, d + pos + 8, clen);
            idat_pos += clen;
        } else if (!memcmp(ct, "IEND", 4)) {
            break;
        }
        pos += 12 + (size_t)clen;
    }

    npass = p.interlace ? 7 : 1;
    for (pass = 0; pass < npass; pass++) {
        int pw = p.interlace ? (p.w - ax[pass] + axs[pass] - 1) / axs[pass] : p.w;
        int ph = p.interlace ? (p.h - ay[pass] + ays[pass] - 1) / ays[pass] : p.h;
        if (pw <= 0 || ph <= 0) continue;
        raw_need += (size_t)ph * (size_t)(1 + png_row_bytes(&p, pw));
    }

    raw = (uint8_t *)malloc(raw_need ? raw_need : 1);
    if (!raw) { free(idat); return NULL; }

    if (idat_pos < 2 ||
        !img_inflate(idat + 2, idat_pos - 2, raw, raw_need, &got) ||
        got != raw_need) {
        free(idat); free(raw);
        return NULL;
    }
    free(idat);

    out = (uint32_t *)calloc((size_t)p.w * (size_t)p.h, 4);
    if (!out) { free(raw); return NULL; }

    {
        size_t maxrb = (size_t)png_row_bytes(&p, p.w);
        cur = (uint8_t *)calloc(maxrb + 8, 1);
        prev = (uint8_t *)calloc(maxrb + 8, 1);
        if (!cur || !prev) { free(cur); free(prev); free(raw); free(out); return NULL; }
    }

    {
        size_t rp = 0;
        for (pass = 0; pass < npass; pass++) {
            int x0 = p.interlace ? ax[pass] : 0;
            int y0 = p.interlace ? ay[pass] : 0;
            int dx = p.interlace ? axs[pass] : 1;
            int dy = p.interlace ? ays[pass] : 1;
            int pw = p.interlace ? (p.w - x0 + dx - 1) / dx : p.w;
            int ph = p.interlace ? (p.h - y0 + dy - 1) / dy : p.h;
            int rb, y;
            if (pw <= 0 || ph <= 0) continue;
            rb = png_row_bytes(&p, pw);
            memset(prev, 0, (size_t)rb);
            for (y = 0; y < ph; y++) {
                int ft = raw[rp++];
                if (ft > 4) { free(cur); free(prev); free(raw); free(out); return NULL; }
                memcpy(cur, raw + rp, (size_t)rb);
                rp += (size_t)rb;
                png_unfilter(cur, y ? prev : NULL, rb, p.bpp_bytes, ft);
                png_emit(&p, cur, pw, out, p.w, y0 + y * dy, x0, dx);
                memcpy(prev, cur, (size_t)rb);
            }
        }
    }

    free(cur); free(prev); free(raw);
    *ow = p.w; *oh = p.h;
    return out;
}

static void dxt_colors(const uint8_t *blk, uint32_t out[4], int dxt1)
{
    unsigned c0 = (unsigned)blk[0] | ((unsigned)blk[1] << 8);
    unsigned c1 = (unsigned)blk[2] | ((unsigned)blk[3] << 8);
    unsigned r[4], g[4], b[4];
    int i;
    r[0] = ((c0 >> 11) & 31u) * 255u / 31u;
    g[0] = ((c0 >> 5) & 63u) * 255u / 63u;
    b[0] = (c0 & 31u) * 255u / 31u;
    r[1] = ((c1 >> 11) & 31u) * 255u / 31u;
    g[1] = ((c1 >> 5) & 63u) * 255u / 63u;
    b[1] = (c1 & 31u) * 255u / 31u;
    if (!dxt1 || c0 > c1) {
        r[2] = (2 * r[0] + r[1]) / 3; g[2] = (2 * g[0] + g[1]) / 3; b[2] = (2 * b[0] + b[1]) / 3;
        r[3] = (r[0] + 2 * r[1]) / 3; g[3] = (g[0] + 2 * g[1]) / 3; b[3] = (b[0] + 2 * b[1]) / 3;
    } else {
        r[2] = (r[0] + r[1]) / 2; g[2] = (g[0] + g[1]) / 2; b[2] = (b[0] + b[1]) / 2;
        r[3] = g[3] = b[3] = 0;
    }
    for (i = 0; i < 4; i++)
        out[i] = r[i] | (g[i] << 8) | (b[i] << 16) | 0xFF000000u;
    if (dxt1 && c0 <= c1) out[3] &= 0x00FFFFFFu;
}

static void dxt_block(const uint8_t *src, int fmt, uint32_t *dst, int dstw,
                      int x0, int y0, int w, int h)
{
    uint32_t col[4];
    const uint8_t *cb = src + (fmt == 1 ? 0 : 8);
    uint32_t bits;
    uint8_t alpha[16];
    int x, y;

    if (fmt == 3) {
        for (y = 0; y < 4; y++) {
            unsigned lo = (unsigned)src[y * 2] | ((unsigned)src[y * 2 + 1] << 8);
            for (x = 0; x < 4; x++) {
                unsigned a4 = (lo >> (x * 4)) & 15u;
                alpha[y * 4 + x] = (uint8_t)(a4 * 17u);
            }
        }
    } else if (fmt == 5) {
        unsigned a0 = src[0], a1 = src[1];
        uint64_t idx = 0;
        int i;
        for (i = 0; i < 6; i++) idx |= (uint64_t)src[2 + i] << (8 * i);
        for (i = 0; i < 16; i++) {
            unsigned code = (unsigned)((idx >> (3 * i)) & 7u);
            unsigned v;
            if (code == 0) v = a0;
            else if (code == 1) v = a1;
            else if (a0 > a1) v = ((8u - code) * a0 + (code - 1u) * a1) / 7u;
            else if (code == 6) v = 0;
            else if (code == 7) v = 255;
            else v = ((6u - code) * a0 + (code - 1u) * a1) / 5u;
            alpha[i] = (uint8_t)v;
        }
    } else {
        int i;
        for (i = 0; i < 16; i++) alpha[i] = 255;
    }

    dxt_colors(cb, col, fmt == 1);
    bits = rd32le(cb + 4);
    for (y = 0; y < 4; y++) {
        if (y0 + y >= h) break;
        for (x = 0; x < 4; x++) {
            uint32_t c;
            if (x0 + x >= w) break;
            c = col[(bits >> (2 * (y * 4 + x))) & 3u];
            if (fmt != 1) c = (c & 0x00FFFFFFu) | ((uint32_t)alpha[y * 4 + x] << 24);
            dst[(size_t)(y0 + y) * (size_t)dstw + (size_t)(x0 + x)] = c;
        }
    }
}

static int mask_shift(uint32_t m, int *bits)
{
    int s = 0, n = 0;
    if (!m) { *bits = 0; return 0; }
    while (!(m & 1u)) { m >>= 1; s++; }
    while (m & 1u) { m >>= 1; n++; }
    *bits = n;
    return s;
}

static uint32_t mask_scale(uint32_t v, int bits)
{
    if (bits <= 0) return 255;
    if (bits >= 8) return (v >> (bits - 8)) & 0xFFu;
    return (v * 255u) / ((1u << bits) - 1u);
}

static uint32_t *dds_decode(const uint8_t *d, size_t len, int *ow, int *oh)
{
    uint32_t hsize, flags, w, h, pfflags, fourcc, bitcount;
    uint32_t rm, gm, bm, am;
    size_t off = 128;
    uint32_t *out;

    if (len < 128 || memcmp(d, "DDS ", 4) != 0) return NULL;
    hsize = rd32le(d + 4);
    if (hsize != 124) return NULL;
    flags = rd32le(d + 8);
    (void)flags;
    h = rd32le(d + 12);
    w = rd32le(d + 16);
    pfflags = rd32le(d + 80);
    fourcc = rd32le(d + 84);
    bitcount = rd32le(d + 88);
    rm = rd32le(d + 92); gm = rd32le(d + 96); bm = rd32le(d + 100); am = rd32le(d + 104);

    if (w == 0 || h == 0 || w > 32768 || h > 32768) return NULL;

    if ((pfflags & 4u) && fourcc == 0x30315844u) {
        uint32_t dxgi;
        if (len < 148) return NULL;
        dxgi = rd32le(d + 128);
        off = 148;
        if (dxgi == 71 || dxgi == 72) fourcc = 0x31545844u;
        else if (dxgi == 74 || dxgi == 75) fourcc = 0x33545844u;
        else if (dxgi == 77 || dxgi == 78) fourcc = 0x35545844u;
        else return NULL;
    }

    out = (uint32_t *)calloc((size_t)w * (size_t)h, 4);
    if (!out) return NULL;

    if (pfflags & 4u) {
        int fmt, blk;
        uint32_t bx, by, x, y;
        if (fourcc == 0x31545844u)      { fmt = 1; blk = 8; }
        else if (fourcc == 0x33545844u) { fmt = 3; blk = 16; }
        else if (fourcc == 0x35545844u) { fmt = 5; blk = 16; }
        else { free(out); return NULL; }
        bx = (w + 3) / 4; by = (h + 3) / 4;
        if (len < off + (size_t)bx * by * (size_t)blk) { free(out); return NULL; }
        for (y = 0; y < by; y++)
            for (x = 0; x < bx; x++)
                dxt_block(d + off + ((size_t)y * bx + x) * (size_t)blk, fmt, out,
                          (int)w, (int)(x * 4), (int)(y * 4), (int)w, (int)h);
    } else if (bitcount == 32 || bitcount == 24) {
        uint32_t bpp = bitcount / 8, x, y;
        int rb, gb, bb, ab;
        int rs = mask_shift(rm, &rb), gs = mask_shift(gm, &gb);
        int bs = mask_shift(bm, &bb), as = mask_shift(am, &ab);
        if (len < off + (size_t)w * h * bpp) { free(out); return NULL; }
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                const uint8_t *s = d + off + ((size_t)y * w + x) * bpp;
                uint32_t v = (uint32_t)s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16);
                uint32_t r, g, b, a;
                if (bpp == 4) v |= (uint32_t)s[3] << 24;
                r = mask_scale((v & rm) >> rs, rb);
                g = mask_scale((v & gm) >> gs, gb);
                b = mask_scale((v & bm) >> bs, bb);
                a = am ? mask_scale((v & am) >> as, ab) : 255u;
                out[(size_t)y * w + x] = r | (g << 8) | (b << 16) | (a << 24);
            }
        }
    } else {
        free(out);
        return NULL;
    }

    *ow = (int)w; *oh = (int)h;
    return out;
}

uint32_t *img_decode(const uint8_t *data, size_t len, int *w, int *h)
{
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (!data || len < 16) return NULL;
    if (!memcmp(data, sig, 8)) return png_decode(data, len, w, h);
    if (!memcmp(data, "DDS ", 4)) return dds_decode(data, len, w, h);
    return NULL;
}

typedef struct {
    uint8_t *buf;
    size_t cap, len;
    uint32_t acc;
    int nbits;
    int fail;
} BitOut;

static void bo_reserve(BitOut *o, size_t extra)
{
    if (o->fail) return;
    if (o->len + extra <= o->cap) return;
    {
        size_t want = o->cap ? o->cap * 2 : 65536;
        uint8_t *nb;
        while (want < o->len + extra) want *= 2;
        nb = (uint8_t *)realloc(o->buf, want);
        if (!nb) { o->fail = 1; return; }
        o->buf = nb; o->cap = want;
    }
}

static void bo_bits(BitOut *o, uint32_t v, int n)
{
    o->acc |= (v & ((n >= 32) ? 0xFFFFFFFFu : ((1u << n) - 1u))) << o->nbits;
    o->nbits += n;
    while (o->nbits >= 8) {
        bo_reserve(o, 1);
        if (o->fail) return;
        o->buf[o->len++] = (uint8_t)(o->acc & 0xFFu);
        o->acc >>= 8;
        o->nbits -= 8;
    }
}

static void bo_huff(BitOut *o, unsigned code, int n)
{
    bo_bits(o, bitrev(code, n), n);
}

static void bo_flush(BitOut *o)
{
    if (o->nbits) {
        bo_reserve(o, 1);
        if (o->fail) return;
        o->buf[o->len++] = (uint8_t)(o->acc & 0xFFu);
        o->acc = 0; o->nbits = 0;
    }
}

static void put_litlen(BitOut *o, int sym)
{
    if (sym < 144)      bo_huff(o, 0x30u + (unsigned)sym, 8);
    else if (sym < 256) bo_huff(o, 0x190u + (unsigned)(sym - 144), 9);
    else if (sym < 280) bo_huff(o, (unsigned)(sym - 256), 7);
    else                bo_huff(o, 0xC0u + (unsigned)(sym - 280), 8);
}

#define DEF_WBITS   15
#define DEF_WSIZE   (1u << DEF_WBITS)
#define DEF_HBITS   15
#define DEF_HSIZE   (1u << DEF_HBITS)
#define DEF_CHAIN   32
#define DEF_MAXMATCH 258
#define DEF_MINMATCH 3

static unsigned def_hash(const uint8_t *p)
{
    unsigned v = ((unsigned)p[0] << 16) | ((unsigned)p[1] << 8) | (unsigned)p[2];
    return (v * 2654435761u) >> (32 - DEF_HBITS);
}

static uint8_t *deflate_fixed(const uint8_t *src, size_t len, size_t *outlen)
{
    BitOut o;
    int32_t *head = NULL, *prev = NULL;
    size_t pos = 0;
    unsigned i;

    memset(&o, 0, sizeof o);
    head = (int32_t *)malloc(sizeof(int32_t) * DEF_HSIZE);
    prev = (int32_t *)malloc(sizeof(int32_t) * DEF_WSIZE);
    if (!head || !prev) { free(head); free(prev); return NULL; }
    for (i = 0; i < DEF_HSIZE; i++) head[i] = -1;
    for (i = 0; i < DEF_WSIZE; i++) prev[i] = -1;

    bo_bits(&o, 1, 1);
    bo_bits(&o, 1, 2);

    while (pos < len) {
        size_t best_len = 0, best_dist = 0;
        if (pos + DEF_MINMATCH <= len) {
            unsigned hv = def_hash(src + pos);
            int32_t first = head[hv];
            int32_t cand = first;
            int chain = DEF_CHAIN;
            size_t maxlen = len - pos;
            if (maxlen > DEF_MAXMATCH) maxlen = DEF_MAXMATCH;
            while (cand >= 0 && chain--) {
                size_t dist = pos - (size_t)cand;
                size_t l = 0;
                if (dist == 0 || dist > DEF_WSIZE) break;
                while (l < maxlen && src[(size_t)cand + l] == src[pos + l]) l++;
                if (l > best_len) { best_len = l; best_dist = dist; if (l == maxlen) break; }
                cand = prev[(size_t)cand & (DEF_WSIZE - 1)];
            }
            prev[pos & (DEF_WSIZE - 1)] = first;
            head[hv] = (int32_t)pos;
        }
        if (best_len >= DEF_MINMATCH) {
            int lc = 0, dc = 0;
            while (lc < 28 && best_len >= (size_t)k_len_base[lc + 1]) lc++;
            while (dc < 29 && best_dist >= (size_t)k_dist_base[dc + 1]) dc++;
            put_litlen(&o, 257 + lc);
            bo_bits(&o, (uint32_t)(best_len - k_len_base[lc]), k_len_extra[lc]);
            bo_huff(&o, (unsigned)dc, 5);
            bo_bits(&o, (uint32_t)(best_dist - k_dist_base[dc]), k_dist_extra[dc]);
            {
                size_t k;
                for (k = 1; k < best_len; k++) {
                    if (pos + k + DEF_MINMATCH <= len) {
                        unsigned hv = def_hash(src + pos + k);
                        prev[(pos + k) & (DEF_WSIZE - 1)] = head[hv];
                        head[hv] = (int32_t)(pos + k);
                    }
                }
            }
            pos += best_len;
        } else {
            put_litlen(&o, src[pos]);
            pos++;
        }
        if (o.fail) break;
    }

    put_litlen(&o, 256);
    bo_flush(&o);
    free(head); free(prev);
    if (o.fail) { free(o.buf); return NULL; }
    *outlen = o.len;
    return o.buf;
}

static uint32_t crc32_of(const uint8_t *p, size_t n, uint32_t crc)
{
    static uint32_t tab[256];
    static int built;
    size_t i;
    if (!built) {
        uint32_t k, j, c;
        for (k = 0; k < 256; k++) {
            c = k;
            for (j = 0; j < 8; j++) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            tab[k] = c;
        }
        built = 1;
    }
    crc = ~crc;
    for (i = 0; i < n; i++) crc = tab[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

static uint32_t adler32_of(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        a += p[i]; if (a >= 65521u) a -= 65521u;
        b += a;    if (b >= 65521u) b -= 65521u;
    }
    return (b << 16) | a;
}

static void wr32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static int png_chunk(FILE *f, const char *tag, const uint8_t *data, size_t n)
{
    uint8_t hdr[8], crcb[4];
    uint32_t crc;
    wr32be(hdr, (uint32_t)n);
    memcpy(hdr + 4, tag, 4);
    if (fwrite(hdr, 1, 8, f) != 8) return 0;
    if (n && fwrite(data, 1, n, f) != n) return 0;
    crc = crc32_of((const uint8_t *)tag, 4, 0);
    if (n) crc = crc32_of(data, n, crc);
    wr32be(crcb, crc);
    return fwrite(crcb, 1, 4, f) == 4;
}

static int filter_cost(const uint8_t *row, size_t n)
{
    size_t i;
    int sum = 0;
    for (i = 0; i < n; i++) {
        int v = row[i];
        sum += v < 128 ? v : 256 - v;
    }
    return sum;
}

int img_write_png(const char *path, const uint32_t *px, int w, int h)
{
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    FILE *f;
    uint8_t ihdr[13];
    uint8_t *raw = NULL, *comp = NULL, *zbuf = NULL;
    uint8_t *cand[5] = { NULL, NULL, NULL, NULL, NULL };
    size_t rb, rawlen, complen = 0, zlen;
    int y, k, ok = 0;

    if (w <= 0 || h <= 0) return 0;
    rb = (size_t)w * 4;
    rawlen = (size_t)h * (rb + 1);
    raw = (uint8_t *)malloc(rawlen);
    if (!raw) return 0;
    for (k = 0; k < 5; k++) {
        cand[k] = (uint8_t *)malloc(rb);
        if (!cand[k]) goto done;
    }

    for (y = 0; y < h; y++) {
        const uint8_t *cur = (const uint8_t *)(px + (size_t)y * (size_t)w);
        const uint8_t *up = y ? (const uint8_t *)(px + (size_t)(y - 1) * (size_t)w) : NULL;
        size_t i;
        int bestk = 0, bestc = 0;
        for (i = 0; i < rb; i++) {
            int a = i >= 4 ? cur[i - 4] : 0;
            int b = up ? up[i] : 0;
            int c = (up && i >= 4) ? up[i - 4] : 0;
            int pp = a + b - c;
            int pa = pp > a ? pp - a : a - pp;
            int pb = pp > b ? pp - b : b - pp;
            int pc = pp > c ? pp - c : c - pp;
            int pr = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
            cand[0][i] = cur[i];
            cand[1][i] = (uint8_t)(cur[i] - a);
            cand[2][i] = (uint8_t)(cur[i] - b);
            cand[3][i] = (uint8_t)(cur[i] - ((a + b) >> 1));
            cand[4][i] = (uint8_t)(cur[i] - pr);
        }
        for (k = 0; k < 5; k++) {
            int cst = filter_cost(cand[k], rb);
            if (k == 0 || cst < bestc) { bestc = cst; bestk = k; }
        }
        raw[(size_t)y * (rb + 1)] = (uint8_t)bestk;
        memcpy(raw + (size_t)y * (rb + 1) + 1, cand[bestk], rb);
    }

    comp = deflate_fixed(raw, rawlen, &complen);
    if (!comp) goto done;

    zlen = complen + 6;
    zbuf = (uint8_t *)malloc(zlen);
    if (!zbuf) goto done;
    zbuf[0] = 0x78; zbuf[1] = 0x01;
    memcpy(zbuf + 2, comp, complen);
    wr32be(zbuf + 2 + complen, adler32_of(raw, rawlen));

    f = fopen(path, "wb");
    if (!f) goto done;

    wr32be(ihdr, (uint32_t)w);
    wr32be(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;

    ok = fwrite(sig, 1, 8, f) == 8
      && png_chunk(f, "IHDR", ihdr, sizeof ihdr)
      && png_chunk(f, "IDAT", zbuf, zlen)
      && png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    if (!ok) remove(path);

done:
    for (k = 0; k < 5; k++) free(cand[k]);
    free(raw); free(comp); free(zbuf);
    return ok;
}
