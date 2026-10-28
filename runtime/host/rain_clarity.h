#ifndef ACX_RAIN_CLARITY_H
#define ACX_RAIN_CLARITY_H
#include <stdint.h>
#include <string.h>
#include <math.h>
/* Original ULUS10176 rain tile: 45 opaque white seeds, otherwise transparent.
 * Match decoded content, not a mission number or transient texture address.
 * Geometry, scrolling and weather simulation continue to come from the game. */
static inline int rain_source_matches(const uint32_t *px, int w, int h)
{
    if(w!=64 || h!=64) return 0;
    uint64_t hash=UINT64_C(14695981039346656037);
    for(int i=0;i<4096;i++) for(int c=0;c<4;c++) {
        hash^=(px[i]>>(8*c))&255u;hash*=UINT64_C(1099511628211);
    }
    return hash==UINT64_C(0xaef9b368b6ceafd1);
}
static inline void rain_make_streaks(const uint32_t *source, uint32_t *out)
{
    memset(out,0,256*256*sizeof *out);
    for(int sy=0;sy<64;sy++)for(int sx=0;sx<64;sx++) {
        if((source[sy*64+sx]&0xffffffu)==0)continue;
        unsigned seed=(unsigned)(sx*73856093u)^(unsigned)(sy*19349663u);
        float length=12.0f+(float)(seed%9u);
        float sigma=0.85f+0.08f*(float)((seed>>4)%5u);
        float peak=190.0f+(float)((seed>>8)%66u);
        /* A bright small head and a thin trailing streak. Periodic writes
         * keep wrap boundaries seamless as the original rain tile scrolls. */
        for(int dy=-24;dy<=2;dy++)for(int dx=-6;dx<=6;dx++) {
            float y=(float)dy+0.5f;
            float t=-y/length;
            if(t< -0.12f || t>1.0f)continue;
            float taper=t<0 ? (1.0f+t/0.12f) : (1.0f-t)*(1.0f-t);
            float x=(float)dx+0.5f-y*0.07f;
            float v=peak*taper*expf(-0.5f*x*x/(sigma*sigma));
            unsigned value=(unsigned)(v+0.5f);if(value>255)value=255;
            int ox=(sx*4+2+dx+256)&255,oy=(sy*4+2+dy+256)&255;
            unsigned old=out[oy*256+ox]&255u;
            if(value>old)out[oy*256+ox]=value*UINT32_C(0x01010101);
        }
    }
}
#endif
