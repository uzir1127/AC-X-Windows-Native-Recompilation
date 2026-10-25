/* Exercise the real registry/copy lifecycle without game data or a decoder. */
#define h264_frame fake_h264_frame
#define h264_frame_dimensions fake_h264_frame_dimensions
#include "../runtime/hle/libmpeg.c"
#undef h264_frame
#undef h264_frame_dimensions
#include <assert.h>
#include <stdio.h>
u8 *g_ram;
static int decode_result=1;
static u32 decode_pixel;
int fake_h264_frame(H264Dec *d,int eos,u8 *dst,int pitch,int format) {
 (void)d;(void)eos;assert(format==3);
 if(decode_result>0)for(int i=0;i<pitch*272;++i)((u32*)dst)[i]=decode_pixel;
 return decode_result;
}
int fake_h264_frame_dimensions(H264Dec *d,int *w,int *h) {(void)d;*w=2;*h=272;return 1;}
int main(void) {
 Mpeg *m=&g_mpeg[0];int w=0,h=0;
 m->used=1;m->h264_frames=1;
 m->surfaces[0].address=0x09700000u;m->surfaces[0].stride=512;
 m->surfaces[0].format=3;m->surfaces[0].width=480;m->surfaces[0].height=272;
 assert(mpeg_video_surface(0x49700000u,3,512,&w,&h)&&w==480&&h==272);
 assert(!mpeg_video_surface(0x09700000u,1,512,&w,&h));
 assert(!mpeg_video_surface(0x09700000u,3,256,&w,&h));
 assert(!mpeg_video_surface(0x09700010u,3,512,&w,&h));
 for(unsigned i=0;i<1000;++i) {
  u32 dst=i%2?0x040cc000u:0x04154000u;
  mpeg_video_copy(dst,0x09700000u,0x88000u);
  assert(mpeg_video_surface(dst,3,512,&w,&h));
  assert(mpeg_video_surface(0x09700000u,3,512,&w,&h));
 }
 mpeg_video_copy(0x040cc000u,0x09000000u,16);
 assert(!mpeg_video_surface(0x040cc000u,3,512,&w,&h));
 assert(mpeg_video_surface(0x04154000u,3,512,&w,&h));
 mpeg_video_copy(0x040cc000u,0x09700000u,0x87fffu);
 assert(!mpeg_video_surface(0x040cc000u,3,512,&w,&h));
 mpeg_free_ctx(m);
 assert(!mpeg_video_surface(0x04154000u,3,512,&w,&h));
 assert(!mpeg_video_surface(0x09700000u,3,512,&w,&h));
 /* Two different successful frames in alternating buffers, then late/failed
  * decode calls. Both buffers must hold the LAST good frame, never A/B/A. */
 g_ram=calloc(1,PSP_RAM_SIZE);assert(g_ram);
 m->used=1;m->handle=1;m->h264=(H264Dec*)1;m->pixelMode=3;
 u32 core=0x08800000u,outptr=core+4,a=0x08900000u,b=a+0x10000u;
 mem_w32(g_ram,core,1);
 CpuCtx c={0};c.r[R_A0]=core;c.r[R_A2]=2;c.r[R_A3]=outptr;c.r[R_SP]=0x08810000u;
 decode_pixel=0xff123456u;mem_w32(g_ram,outptr,a);hle_sceMpegAvcDecode(&c,g_ram);
 assert(mem_r32(g_ram,a)==decode_pixel);
 decode_pixel=0xffabcdefu;mem_w32(g_ram,outptr,b);hle_sceMpegAvcDecode(&c,g_ram);
 assert(mem_r32(g_ram,b)==decode_pixel);
 decode_result=0;mem_w32(g_ram,outptr,a);hle_sceMpegAvcDecode(&c,g_ram);
 assert(mem_r32(g_ram,a)==decode_pixel && m->h264_frames==2);
 decode_result=-1;mem_w32(g_ram,outptr,b);hle_sceMpegAvcDecode(&c,g_ram);
 assert(mem_r32(g_ram,b)==decode_pixel && m->h264_frames==2);
 for(int i=0;i<2*272;++i)assert(mem_r32(g_ram,a+i*4)==decode_pixel && mem_r32(g_ram,b+i*4)==decode_pixel);
 m->h264=NULL;mpeg_free_ctx(m);assert(!m->held_video && !m->held_bytes);free(g_ram);
 puts("video surface: PASS; alias/pitch/format isolation; 1000 rotating copies; partial overwrite; context release");
 puts("late decoder: PASS; alternate buffers hold latest good frame; successful decode count unchanged; cache released");
 return 0;
}
