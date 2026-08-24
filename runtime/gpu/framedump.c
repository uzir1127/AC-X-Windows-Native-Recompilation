#include "pspreco.h"
#include "ge.h"
#include "framedump.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BMP_HEADER_BYTES      54u
#define BMP_DIB_HEADER_BYTES  40u
#define BMP_BITS_PER_PIXEL    24u

#define BMP_OFF_FILE_SIZE     2
#define BMP_OFF_PIXEL_DATA    10
#define BMP_OFF_DIB_SIZE      14
#define BMP_OFF_WIDTH         18
#define BMP_OFF_HEIGHT        22
#define BMP_OFF_PLANES        26
#define BMP_OFF_BPP           28
#define BMP_OFF_IMAGE_SIZE    34

static const char *g_prefix;
static unsigned g_every = 60;
static unsigned g_seq;
static int g_checked;

static void init_once(void)
{
    const char *e;
    if (g_checked) return;
    g_checked = 1;
    g_prefix = getenv("PSPRECO_FRAMEDUMP");
    e = getenv("PSPRECO_FRAMEDUMP_EVERY");
    if (e) {
        unsigned long v = strtoul(e, NULL, 10);
        if (v) g_every = (unsigned)v;
    }
    if (g_prefix)
        printf("[gpu] dumping every %u-th frame to %sNNNNNN.bmp\n",
               g_every, g_prefix);
}

int framedump_enabled(void)
{
    init_once();
    return g_prefix != NULL;
}

static void put_le16(u8 *p, unsigned v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put_le32(u8 *p, u32 v)
{
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

static int write_bmp(const char *path, u8 *ram, u32 addr, u32 stride,
                     int fpf, int w, int h)
{
    unsigned row_bytes = (unsigned)w * 3u;
    unsigned pad = (4u - (row_bytes & 3u)) & 3u;
    unsigned image = (row_bytes + pad) * (unsigned)h;
    u8 hdr[BMP_HEADER_BYTES];
    u8 *row;
    FILE *f;
    int x, y;
    unsigned bpp = (fpf == 3) ? 4u : 2u;
    const u32 height_positive_means_bottom_up = (u32)h;

    f = fopen(path, "wb");
    if (!f) return 0;

    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    put_le32(hdr + BMP_OFF_FILE_SIZE, BMP_HEADER_BYTES + image);
    put_le32(hdr + BMP_OFF_PIXEL_DATA, BMP_HEADER_BYTES);
    put_le32(hdr + BMP_OFF_DIB_SIZE, BMP_DIB_HEADER_BYTES);
    put_le32(hdr + BMP_OFF_WIDTH, (u32)w);
    put_le32(hdr + BMP_OFF_HEIGHT, height_positive_means_bottom_up);
    put_le16(hdr + BMP_OFF_PLANES, 1u);
    put_le16(hdr + BMP_OFF_BPP, BMP_BITS_PER_PIXEL);
    put_le32(hdr + BMP_OFF_IMAGE_SIZE, image);
    fwrite(hdr, 1, sizeof hdr, f);

    row = (u8 *)malloc(row_bytes + pad);
    if (!row) { fclose(f); return 0; }
    memset(row, 0, row_bytes + pad);

    for (y = h - 1; y >= 0; y--) {
        for (x = 0; x < w; x++) {
            u32 a = addr + (u32)y * stride * bpp + (u32)x * bpp;
            u32 raw = (bpp == 4) ? mem_r32(ram, a) : mem_r16(ram, a);
            u32 rgba = ge_unpack_pixel(fpf, raw);
            u8 blue  = (u8)((rgba >> 16) & 0xFFu);
            u8 green = (u8)((rgba >> 8) & 0xFFu);
            u8 red   = (u8)(rgba & 0xFFu);
            row[x * 3 + 0] = blue;
            row[x * 3 + 1] = green;
            row[x * 3 + 2] = red;
        }
        fwrite(row, 1, row_bytes + pad, f);
    }
    free(row);
    fclose(f);
    return 1;
}

void framedump_frame(u8 *ram, u32 addr, u32 stride, int fpf, int w, int h)
{
    char path[512];
    if (!framedump_enabled() || !addr) return;
    if (g_every > 1 && (g_seq % g_every) != 0) { g_seq++; return; }
    snprintf(path, sizeof path, "%s%06u.bmp", g_prefix, g_seq);
    if (write_bmp(path, ram, addr, stride, fpf, w, h))
        printf("[gpu] wrote %s (%dx%d from %08x, fpf=%d)\n",
               path, w, h, addr, fpf);
    else
        printf("[gpu] could not write %s\n", path);
    g_seq++;
}
