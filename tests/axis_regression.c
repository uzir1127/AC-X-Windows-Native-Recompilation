#include "pspreco.h"
#include "symbols.h"
#include "hle.h"
#include "framerate.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
u8 *g_ram;
static CpuCtx seed;
static u8 *snapshot;
static u32 aircraft;
static u64 tick;
static int axis;
static unsigned inoff[]={0xf8,0x100,0xfc},veloff[]={0xdc,0xe0,0xe4},stepoff[]={0x130,0x134,0x138};
static RecompFn axis_fn[]={func_08871a4c,func_0887211c,func_088722ac};
u64 test_vblank_count(void){return tick;}
static float simulate(int native,float strength,int pattern,float *velocity){
 memcpy(g_ram+0x08000000,snapshot,0x02000000);
 mem_wf32(g_ram,aircraft+veloff[axis],0);
 if(native){framerate_movie_begin();framerate_movie_end();}
 float angle=0;
 int dt=native?1:2;
 for(int f=0;f<600;f+=dt){
  CpuCtx c=seed;
  float input=strength;
  if(pattern==1)input=((f/40)%2)?-strength:strength;
  if(pattern==2)input=(f<40)?strength:((f<80)?-strength:0);
  mem_wf32(g_ram,aircraft+inoff[axis],input);
  mem_w32(g_ram,0x08a82e6c,60/dt);
  mem_wf32(g_ram,0x08a82e40,dt);mem_wf32(g_ram,0x08a82e44,dt);mem_wf32(g_ram,0x08a82e48,1.0f/dt);
  if(native){++tick;framerate_update_delta(g_ram,0x08a82e40);}
  axis_fn[axis](&c,g_ram);
  angle+=mem_rf32(g_ram,aircraft+stepoff[axis]);
 }
 *velocity=mem_rf32(g_ram,aircraft+veloff[axis]);
 return angle;
}
int main(int argc,char**argv){
 if(argc!=3)return 2;
 FILE*f=fopen(argv[1],"rb");if(!f||fread(&seed,1,sizeof seed,f)!=sizeof seed)return 3;fclose(f);
 snapshot=malloc(0x02000000);g_ram=calloc(1,PSP_RAM_SIZE);
 f=fopen(argv[2],"rb");if(!f||fread(snapshot,1,0x02000000,f)!=0x02000000)return 4;fclose(f);
 aircraft=seed.r[R_A1];cpu_set_host_fpu_mode();hle_init();
 float strengths[]={-.9f,-.4f,.4f,.9f},angles[36],vel[36];int bad=0;
 for(axis=0;axis<3;axis++)for(int a=0;a<4;a++)for(int b=0;b<3;b++)angles[axis*12+a*3+b]=simulate(0,strengths[a],b,&vel[axis*12+a*3+b]);
 _putenv_s("PSPRECO_FPS","60");mem_w32(g_ram,0x08804b74,0x28420002);framerate_init(g_ram);if(!framerate_is_60())return 5;
 puts("axis,input,pattern,angle30,angle60,angle_error,final_velocity_error");
 for(axis=0;axis<3;axis++)for(int a=0;a<4;a++)for(int b=0;b<3;b++){
  float v,x=simulate(1,strengths[a],b,&v),e=fabsf(x-angles[axis*12+a*3+b]),ve=fabsf(v-vel[axis*12+a*3+b]);
  printf("%d,%.3f,%d,%.9f,%.9f,%.9f,%.9f\n",axis,strengths[a],b,angles[axis*12+a*3+b],x,e,ve);
  if(!isfinite(x)||e>.0002f||ve>1e-6f)bad++;
 }
 printf("axis failures: %d\n",bad);return bad?1:0;
}