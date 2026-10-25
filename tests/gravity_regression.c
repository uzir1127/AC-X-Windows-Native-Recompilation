#include "pspreco.h"
#include "symbols.h"
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
u8 *g_ram;
static CpuCtx seed;
static u8 *snapshot;
static u32 obj,aircraft;
static float simulate(float dt,unsigned fps,float speed,float bank)
{
 memcpy(g_ram+0x08000000,snapshot,0x02000000);
 mem_wf32(g_ram,obj+0x48,bank);
 mem_wf32(g_ram,aircraft+0xa4,speed);
 mem_wf32(g_ram,aircraft+0xd8,0);
 mem_wf32(g_ram,aircraft+0x144,0);
 float y=mem_rf32(g_ram,obj+0x24);
 for(int i=0;i<(int)(600/dt);i++) {
  mem_w32(g_ram,0x08a82e6c,(unsigned)(60/dt));
  mem_wf32(g_ram,0x08a82e40,dt);mem_wf32(g_ram,0x08a82e44,dt);mem_wf32(g_ram,0x08a82e48,1/dt);
  CpuCtx c=seed;func_088747c8(&c,g_ram);
  mem_w32(g_ram,0x08a82e6c,fps); c=seed;func_08874ea0(&c,g_ram);
 }
 return mem_rf32(g_ram,obj+0x24)-y;
}
int main(int argc,char **argv) {
 if(argc!=3)return 2;
 FILE*f=fopen(argv[1],"rb");if(!f||fread(&seed,1,sizeof seed,f)!=sizeof seed)return 3;fclose(f);
 snapshot=malloc(0x02000000);g_ram=calloc(1,PSP_RAM_SIZE);
 f=fopen(argv[2],"rb");if(!f||fread(snapshot,1,0x02000000,f)!=0x02000000)return 4;fclose(f);
 obj=seed.r[R_A0];aircraft=seed.r[R_A1];cpu_set_host_fpu_mode();hle_init();
 puts("speed,bank,sink30,sink60_before,sink60_corrected,error");
 float speeds[]={5,10,13.888889f,20};float banks[]={0,0.3f,0.7f};int bad=0;
 for(int a=0;a<4;a++)for(int b=0;b<3;b++) {
  float x=simulate(2,30,speeds[a],banks[b]);float y=simulate(1,60,speeds[a],banks[b]);float z=simulate(1,30,speeds[a],banks[b]);
  float err=fabsf(x-z);printf("%f,%f,%f,%f,%f,%f\n",speeds[a],banks[b],x,y,z,err);
  if(!isfinite(z)||err>fmaxf(1.0f,fabsf(x)*.02f))bad++;
 }
 printf("gravity failures: %d\n",bad);return bad?1:0;
}