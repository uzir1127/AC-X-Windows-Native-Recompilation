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
static int animation_check(void){
 int bad=0;float expect_axis[12],expect_surface[12];
 RecompFn left=qol_override(0x0887d8e8,func_0887d8e8),right=qol_override(0x0887da18,func_0887da18);
 for(int mode=0;mode<5;mode++)for(int direction=-1;direction<=1;direction+=2){
  float strength=mode<3?(mode+1)*.25f:1.0f;
  for(int modern=0;modern<2;modern++){
   memcpy(g_ram+0x08000000,snapshot,0x02000000);
   mem_w32(g_ram,0x08a82ed8,mode==4?player+0x2000:player);
   mem_wf32(g_ram,aircraft+0xc8,0);mem_wf32(g_ram,aircraft+0xb4,.37f);
   g_qol.modern_controls=mode!=3;rudder_strength=strength;
   aux=modern?(direction>0?PRESENT_AUX_LB:PRESENT_AUX_RB):0;
   for(int frame=0;frame<12;frame++){
    mem_wf32(g_ram,aircraft+0x100,0);
    for(int side=1;side>=-1;side-=2){
     CpuCtx c=seed;c.r[R_A0]=player;c.r[R_A1]=aircraft;
     c.r[R_A2]=!modern&&mode<3&&side==direction;c.f[12]=modern?0:strength;
     if(modern)(side>0?left:right)(&c,g_ram);
     else(side>0?func_0887d8e8:func_0887da18)(&c,g_ram);
     if(modern&&(c.r[R_A2]!=0||c.f[12]!=0))bad++;
    }
    float axis=mem_rf32(g_ram,aircraft+0x100),surface=mem_rf32(g_ram,aircraft+0xc8);
    if(!modern){expect_axis[frame]=axis;expect_surface[frame]=surface;}
    else if(axis!=expect_axis[frame]||surface!=expect_surface[frame]||mem_rf32(g_ram,aircraft+0xb4)!=.37f)bad++;
   }
  }
  if(mode<3){
   printf("Rudder animation dir=%d strength=%.2f axis=%.6f surface=%.6f\n",direction,strength,expect_axis[11],expect_surface[11]);
   if(expect_axis[11]*direction<=0||expect_surface[11]*direction<=0)bad++;
  }
 }
 /* Opposed bumpers and released controls do not fabricate rudder input. */
 memcpy(g_ram+0x08000000,snapshot,0x02000000);mem_w32(g_ram,0x08a82ed8,player);g_qol.modern_controls=1;rudder_strength=1;
 for(int mask=0;mask<2;mask++){
  aux=mask?(PRESENT_AUX_LB|PRESENT_AUX_RB):0;mem_wf32(g_ram,aircraft+0x100,0);mem_wf32(g_ram,aircraft+0xc8,0);
  CpuCtx c=seed;c.r[R_A0]=player;c.r[R_A1]=aircraft;c.r[R_A2]=0;c.f[12]=0;left(&c,g_ram);right(&c,g_ram);
  if(mem_rf32(g_ram,aircraft+0x100)!=0||mem_rf32(g_ram,aircraft+0xc8)!=0)bad++;
 }
 aux=0;puts("Native D-pad versus modern rudder animation/axis: both directions, analog strength, release/opposition, toggle and NPC bypass checked.");
 return bad;
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
 bad+=animation_check();printf("controls failures: %d\n",bad);return bad?1:0;
}
