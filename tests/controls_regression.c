#include "pspreco.h"
#include "symbols.h"
#include "hle.h"
#include "hooks.h"
#include "framerate.h"
#include "qol_input.h"
#include "../runtime/host/present.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
u8 *g_ram;
float g_vk_max_aniso = 1.0f;
static CpuCtx seed;
static u8 *snapshot;
static u32 aircraft, player, aux;
static u64 tick;
static int native;
static float rudder_strength=1.0f;
u64 test_vblank_count(void){return tick;}
u32 present_aux(void){return aux;}
float present_rudder(void){return rudder_strength*((aux&PRESENT_AUX_LB?1.0f:0.0f)-(aux&PRESENT_AUX_RB?1.0f:0.0f));}
static float simulate(int injected,u32 buttons,float direction,int other,int *bad){
 memcpy(g_ram+0x08000000,snapshot,0x02000000);
 mem_w32(g_ram,0x08a82ed8,other?player+0x2000:player);
 mem_wf32(g_ram,aircraft+0xe0,0);
 mem_wf32(g_ram,aircraft+0xf4,0);
 mem_wf32(g_ram,aircraft+0xb4,.37f);
 if(native){framerate_movie_begin();framerate_movie_end();}
 float angle=0;int dt=native?1:2;
 aux=buttons;
 for(int f=0;f<600;f+=dt){
  CpuCtx c=seed;
  float saved=injected?0.0f:(float)direction;
  mem_wf32(g_ram,aircraft+0x100,saved);
  mem_w32(g_ram,0x08a82e6c,60/dt);
  mem_wf32(g_ram,0x08a82e40,dt);mem_wf32(g_ram,0x08a82e44,dt);mem_wf32(g_ram,0x08a82e48,1.0f/dt);
  if(native){++tick;framerate_update_delta(g_ram,0x08a82e40);}
  if(injected) g_func_ptr[hook_index(0x0887211c)](&c,g_ram);
  else func_0887211c(&c,g_ram);
  if(mem_rf32(g_ram,aircraft+0x100)!=saved||mem_rf32(g_ram,aircraft+0xb4)!=.37f)(*bad)++;
  angle+=mem_rf32(g_ram,aircraft+0x134);
 }
 return angle;
}
int main(int argc,char**argv){
 if(argc!=3)return 2;
 FILE*f=fopen(argv[1],"rb");if(!f||fread(&seed,1,sizeof seed,f)!=sizeof seed)return 3;fclose(f);
 snapshot=malloc(0x02000000);g_ram=calloc(1,PSP_RAM_SIZE);
 f=fopen(argv[2],"rb");if(!f||fread(snapshot,1,0x02000000,f)!=0x02000000)return 4;fclose(f);
 aircraft=seed.r[R_A1];player=seed.r[R_A0];cpu_set_host_fpu_mode();hle_init();int bad=0,checks=0;
 g_qol.modern_controls=1;
 qol_input_install(g_ram);if(qol_modern_controls_active())bad++;
 if(qol_shoulder_buttons(0,1,0,0,0,1)!=0x100)bad++;
 memcpy(g_ram+0x08000000,snapshot,0x02000000);qol_input_install(g_ram);
 if(!qol_modern_controls_active())return 5;
 u32 bases[]={0,0xf0f9,0x100,0x200,0x300};
 for(int mode=0;mode<2;mode++)for(int flying=0;flying<2;flying++)for(int b=0;b<5;b++)for(int mask=0;mask<16;mask++){
  g_qol.modern_controls=mode;int lb=mask&1,rb=mask&2,lt=mask&4,rt=mask&8;
  u32 want=bases[b];if(lt||((!mode||!flying)&&lb))want|=0x100;if(rt||((!mode||!flying)&&rb))want|=0x200;
  if(qol_shoulder_buttons(bases[b],lb,rb,lt,rt,flying)!=want)bad++;
  checks++;
 }
 printf("Shoulder/menu/keyboard/weapon/pause/map combinations checked: %d\n",checks);
 for(native=0;native<2;native++){
  g_qol.modern_controls=1;
  if(native){mem_w32(g_ram,0x08804b74,0x28420002);_putenv_s("PSPRECO_FPS","60");framerate_init(g_ram);if(!framerate_is_60())return 6;}
  for(int d=-1;d<=1;d++){
   u32 bits=d>0?PRESENT_AUX_LB:(d<0?PRESENT_AUX_RB:(PRESENT_AUX_LB|PRESENT_AUX_RB));
   float ref=simulate(0,0,d,0,&bad),got=simulate(1,bits,d,0,&bad);
   printf("%d FPS direction=%d original_axis=%.9f modern_bumpers=%.9f difference=%.9f\n",native?60:30,d,ref,got,fabsf(ref-got));
   if(!isfinite(got)||fabsf(ref-got)>1e-6f||(d&&got*d<=0))bad++;
  }
  for(int step=1;step<=3;step++){
   rudder_strength=step*.25f;
   for(int sign=-1;sign<=1;sign+=2){
    float direction=sign*rudder_strength;
    float ref=simulate(0,0,direction,0,&bad),got=simulate(1,sign>0?PRESENT_AUX_LB:PRESENT_AUX_RB,direction,0,&bad);
    if(!isfinite(got)||fabsf(ref-got)>1e-6f)bad++;
    printf("%d FPS analog rudder %.2f original=%.9f injected=%.9f\n",native?60:30,direction,ref,got);
   }
  }
  rudder_strength=1;
  float ref=simulate(0,0,0,0,&bad),got=simulate(1,PRESENT_AUX_LB,0,1,&bad);
  if(ref!=got)bad++;
  g_qol.modern_controls=0;got=simulate(1,PRESENT_AUX_LB,0,0,&bad);if(ref!=got)bad++;
  g_qol.modern_controls=1;got=simulate(1,0,0,0,&bad);if(ref!=got)bad++;
 }
 puts("Verified original yaw physics, 30/60 FPS, opposed/released bumpers, NPC bypass, toggle-off, and input/throttle restoration.");
 printf("controls failures: %d\n",bad);return bad?1:0;
}
