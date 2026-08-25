#ifndef GE_SHARED_H
#define GE_SHARED_H

#include <stdint.h>

typedef struct {
    uint32_t base, offset;
    uint32_t fbp, fbw, fbfmt;
    uint32_t zbp, zbw;
    uint32_t vaddr, iaddr;
    uint32_t vtype;
    int scis_x1, scis_y1, scis_x2, scis_y2;
    int ztest_enable, zwrite_disable, cull_enable;
    uint32_t ztest, cull, minz, maxz;
    uint32_t depth_clip;
    float world[12], view[12], proj[16];
    int world_num, view_num, proj_num;
    float bone[96];
    int bone_num;
    uint32_t viewport_raw[6];
    uint32_t offsetx, offsety;
    uint32_t material;
    int material_alpha, material_alpha_set, material_set;
    int tex_enable, tex_swizzle, clear;
    uint32_t clear_mode;
    int blend_enable;
    uint32_t blend_mode, blend_fixa, blend_fixb;
    uint32_t tex_addr, tex_bufw, tex_fmt;
    uint32_t tex_wrap, tex_func, tex_env, tex_filter;
    int tex_w, tex_h;
    uint32_t clut_addr, clut_fmt;
    uint32_t clut_load_bytes, clut_max_bytes;
    uint8_t clutram[2048];
    float tex_scale_u, tex_scale_v, tex_off_u, tex_off_v;
    uint32_t tex_map_mode;
    uint32_t tex_shade_ls;
    float tgen[12];
    int tgen_num;
    int atest_enable; uint32_t atest;
    int fog_enable; float fog_end, fog_slope; uint32_t fog_color;
    int shade_gouraud;
    int lighting_enable, light_on[4], reverse_normal, light_mode;
    uint32_t light_type[4];
    float light_pos[4][3], light_dir[4][3], light_att[4][3];
    float light_spot_exp[4], light_spot_cut[4];
    uint32_t light_acol[4], light_dcol[4], light_scol[4];
    uint32_t amb_color; int amb_alpha;
    uint32_t mat_emissive, mat_diffuse, mat_specular, mat_update;
    float spec_coef;
    int dither_enable; int8_t dith[4][4];
    uint32_t mask_rgb; int mask_alpha;
    uint32_t clut_gen;
    uint32_t xf_src, xf_srcw, xf_dst, xf_dstw, xf_spos, xf_dpos, xf_size;
    uint32_t xf_bpp;
} GeState;

typedef struct GeVtx { float x, y, z, rw; float u, v; float fog; int r, g, b, a; } GeVtx;

typedef struct GeGpuHooks {
    int  (*tri)(const GeVtx *a, const GeVtx *b, const GeVtx *c, int persp);
    int  (*sprite)(const GeVtx *p0, const GeVtx *p1, int persp);
    int  (*line)(const GeVtx *a, const GeVtx *b, int persp);
    int  (*point)(const GeVtx *a, int persp);
    void (*flush)(const char *reason);
    void (*vram_dirty)(uint32_t addr, uint32_t bytes);
    int  (*xfer)(uint32_t startdata);
} GeGpuHooks;

void      ge_set_gpu_hooks(const GeGpuHooks *h);
GeState  *ge_state_ptr(void);
uint16_t *ge_zbuf_ptr(void);
uint32_t  ge_zbuf_stride(void);
void      ge_decode_tex_rgba(uint32_t *out);

#endif
