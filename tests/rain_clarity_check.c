#include "../runtime/host/rain_clarity.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char **argv)
{
    if(argc!=2)return 2;
    uint32_t source[4096],saved[4096],a[65536],b[65536];
    FILE *f=fopen(argv[1],"rb");if(!f)return 3;
    if(fread(source,4,4096,f)!=4096)return 4;fclose(f);
    memcpy(saved,source,sizeof source);
    if(!rain_source_matches(source,64,64)||rain_source_matches(source,32,64))return 5;
    source[0]^=1;if(rain_source_matches(source,64,64))return 6;source[0]^=1;
    rain_make_streaks(source,a);rain_make_streaks(source,b);
    if(memcmp(a,b,sizeof a)||memcmp(source,saved,sizeof source))return 7;
    unsigned nonzero=0,soft=0;uint64_t energy=0;
    for(int i=0;i<65536;i++) {
        unsigned v=a[i]&255;energy+=v;
        if(a[i]!=v*UINT32_C(0x01010101))return 8;
        nonzero+=v>0;soft+=v>0&&v<255;
    }
    if(nonzero<450||nonzero>14000||soft<nonzero*9/10)return 9;
    printf("Rain exact identity/negative match, source preservation, deterministic periodic soft streaks PASS; nonzero=%u soft=%u energy=%llu\n",nonzero,soft,(unsigned long long)energy);
    if(argc==2){f=fopen("rain-generated.rgba","wb");if(f){fwrite(a,4,65536,f);fclose(f);}}
    return 0;
}
