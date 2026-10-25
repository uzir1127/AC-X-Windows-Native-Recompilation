#include "../runtime/host/video_clarity.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

int main(void) {
    uint32_t src[4*3],tmp[8*3],guard[2+10*8];
    for(int i=0;i<12;i++) src[i]=0x7f419ac3u;
    guard[0]=guard[81]=0xdeadbeefu;
    video_upscale2x(src,4,4,3,guard+1,10,8,tmp);
    for(int i=1;i<=80;i++) assert(guard[i]==0x7f419ac3u);
    assert(guard[0]==0xdeadbeefu && guard[81]==0xdeadbeefu);
    /* A one-pixel image must extend identically into every padding pixel. */
    src[0]=0xfedcba98u;
    video_upscale2x(src,4,1,1,guard+1,10,8,tmp);
    for(int i=1;i<=80;i++)assert(guard[i]==src[0]);
    /* A hard edge gains intermediate samples without changing endpoints,
     * alpha, row alignment, channel order, or writing beyond the output. */
    src[0]=src[4]=src[8]=0xff000000u;
    src[1]=src[5]=src[9]=0xff000000u;
    src[2]=src[6]=src[10]=0xff0000ffu;
    src[3]=src[7]=src[11]=0xff0000ffu;
    video_upscale2x(src,4,4,3,guard+1,10,8,tmp);
    assert(guard[1]==0xff000000u && guard[8]==0xff0000ffu);
    assert((guard[4]&255)>0 && (guard[4]&255)<128);
    assert((guard[5]&255)>128 && (guard[5]&255)<255);
    for(int i=1;i<=80;i++)assert((guard[i]&0xffffff00u)==0xff000000u);
    assert(guard[0]==0xdeadbeefu && guard[81]==0xdeadbeefu);
    static uint32_t image[512*512],out[1024*1024],scratch[960*272];
    for(int i=0;i<512*512;i++)image[i]=0xff000000u|(uint32_t)(i*997u&0xffffffu);
    clock_t start=clock();
    for(int i=0;i<60;i++)video_upscale2x(image,512,480,272,out,1024,1024,scratch);
    printf("video_clarity: PASS; 480x272 to padded1024x1024: %.3f ms/frame\n",1000.0*(clock()-start)/CLOCKS_PER_SEC/60);
    return 0;
}
