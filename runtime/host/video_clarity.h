#ifndef PSPRECO_VIDEO_CLARITY_H
#define PSPRECO_VIDEO_CLARITY_H
#include <stdint.h>

/* Only successfully decoded buffers in a live sceMpeg context qualify.
 * Address aliases are normalized by the implementation; ordinary textures
 * and in-engine cinematics retain the renderer's normal settings. */
int mpeg_video_surface(uint32_t address, int format, uint32_t stride,
                       int *width, int *height);
void mpeg_video_copy(uint32_t destination, uint32_t source, uint32_t bytes);

/* Separable Catmull-Rom at the two quarter-pixel phases of a 2x upscale.
 * src/dst may use RGBA or BGRA consistently. Scratch needs width*2*height
 * pixels. Clamp the source edge, including the unused texture padding, so
 * PSP movie slices cannot pull black/old framebuffer pixels into the edge.
 * Caller validates extents and provides non-overlapping buffers. */
static inline int video_edge(int x, int n) { return x < 0 ? 0 : x >= n ? n-1 : x; }
static inline uint32_t video_cubic_pixel(const uint32_t p[4], int phase)
{
    static const int k[2][4] = {{-3,29,111,-9}, {-9,111,29,-3}};
    uint32_t out = 0;
    for (int c=0; c<4; ++c) {
        int v=64;
        for (int j=0; j<4; ++j) v += (int)((p[j] >> (c*8)) & 255u) * k[phase][j];
        v = v < 0 ? 0 : v / 128;
        if (v > 255) v=255;
        out |= (uint32_t)v << (c*8);
    }
    return out;
}
static inline void video_upscale2x(const uint32_t *src, int stride, int w, int h,
                                  uint32_t *dst, int dw, int dh, uint32_t *tmp)
{
    int uw=w*2, uh=h*2;
    for (int y=0; y<h; ++y) for (int x=0; x<uw; ++x) {
        int i=(x+1)/2-1; uint32_t p[4];
        for (int j=0;j<4;++j) p[j]=src[y*stride+video_edge(i+j-1,w)];
        tmp[y*uw+x]=video_cubic_pixel(p,x&1);
    }
    for (int y=0; y<uh; ++y) {
        int i=(y+1)/2-1;
        for (int x=0; x<uw; ++x) {
            uint32_t p[4];
            for (int j=0;j<4;++j) p[j]=tmp[video_edge(i+j-1,h)*uw+x];
            dst[y*dw+x]=video_cubic_pixel(p,y&1);
        }
        for (int x=uw; x<dw; ++x) dst[y*dw+x]=dst[y*dw+uw-1];
    }
    for (int y=uh; y<dh; ++y)
        for (int x=0; x<dw; ++x) dst[y*dw+x]=dst[(uh-1)*dw+x];
}
#endif
