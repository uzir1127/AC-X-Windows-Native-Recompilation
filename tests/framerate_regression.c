#include "pspreco.h"
#include "framerate.h"
#include "hooks.h"
#include "acx_symbols.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
static u64 clock_tick;
static RecompFn installed;
static int gate_calls, failures;
static const u32 hook_addrs[] = {
 G_ACX_VBLANKISR_FRAMEGATE_DIV2,0x08965218,0x08878170,0x08878994,
 0x088e799c,0x088ed6e0,0x0887dcb8,0x0887ddd8,0x0887dec0,0x0887dfa8,0x0887e0cc,0x08874ea0,0x0887b998,0x08874aac,0x0886ec48};
static RecompFn wrappers[15];
static int calls[15],draw_calls[15];
static unsigned rack_decrements;
static void legacy_original(int id,CpuCtx *c,u8 *ram) {
 float dt=mem_rf32(ram,G_FT_DELTA_60HZTICKS);
 float rd=mem_rf32(ram,G_FT_DELTA_60HZTICKS+4);
 if(id==14) {calls[id]++;}
 else if(id==13) {double epsilon;memcpy(&epsilon,ram+0x08a437c8,8);calls[id]++;if(fabs(epsilon-0.001*(double)dt)>1e-12)failures++;}
 else if(id==12) {calls[id]++;if(mem_r32(ram,0x08a82e6c)!=30)failures++;c->f[0]=c->f[13]+7;}
 else if(id==11) {calls[id]++;if(mem_r32(ram,0x08a82e6c)!=30||dt!=1)failures++;}
 else if(id==3) rack_decrements+=(unsigned)(rd*0.5f);
 else if((id==4||id==5)&&c->r[R_A1]!=1) draw_calls[id]++;
 else {calls[id]++;if(mem_r32(ram,0x08a82e6c)!=30)failures++;if(dt!=2||rd!=2||mem_rf32(ram,G_FT_DELTA_60HZTICKS+8)!=0.5f)failures++;}
 c->r[R_V0]=1;
}
#define ORIGINAL(n) static void original_##n(CpuCtx*c,u8*r){legacy_original(n,c,r);}
ORIGINAL(1) ORIGINAL(2) ORIGINAL(3) ORIGINAL(4) ORIGINAL(5)
ORIGINAL(6) ORIGINAL(7) ORIGINAL(8) ORIGINAL(9) ORIGINAL(10) ORIGINAL(11) ORIGINAL(12) ORIGINAL(13) ORIGINAL(14)
#undef ORIGINAL
static unsigned expected_gate = 1;
u64 threadman_vblank_count(void) { return clock_tick; }
int hook_available(void) { return 1; }
int hook_index(u32 a) { (void)a; return 0; }
static void original_gate(CpuCtx *c,u8 *ram) {
    (void)c;
    u32 counter=mem_r32(ram,G_VBLANKDIVIDER_COUNTTO2);
    if(counter!=expected_gate) failures++;
    mem_w32(ram,G_VBLANKDIVIDER_COUNTTO2,counter + 1u >= 2u ? 0u : counter + 1u);
    gate_calls++;
}
int hook_install(u32 a,RecompFn fn,RecompFn *orig) {
 RecompFn originals[]={original_gate,original_1,original_2,original_3,
 original_4,original_5,original_6,original_7,original_8,original_9,original_10,original_11,original_12,original_13,original_14};
 unsigned i;
 for(i=0;i<15;i++)if(a==hook_addrs[i]) {
   if(orig)*orig=originals[i];wrappers[i]=fn;
   if(i==0)installed=fn;return HOOK_OK;
 }
 return HOOK_ENOTFOUND;
}
static void expect(u8 *ram, u32 timer, float dt, const char *label) {
    float a=mem_rf32(ram,timer),b=mem_rf32(ram,timer+4),c=mem_rf32(ram,timer+8);
    float inv=dt>0?1.0f/dt:0;
    int ok=fabsf(a-dt)<1e-6f && fabsf(b-dt)<1e-6f && fabsf(c-inv)<1e-6f;
    printf("%s %s delta=%.6f rounded=%.6f reciprocal=%.6f\n",ok?"PASS":"FAIL",label,a,b,c);
    if(!ok)failures++;
}
int main(void) {
    u8 *ram=(u8*)calloc(1,PSP_RAM_SIZE);
    CpuCtx c={0}; u32 t=G_FT_DELTA_60HZTICKS;
    if(!ram)return 2;
    {double epsilon=.002;memcpy(ram+0x08a437c8,&epsilon,8);}
    _putenv_s("PSPRECO_FPS","30");framerate_init(ram);
    if(framerate_is_60())failures++;
    mem_wf32(ram,t,2);mem_wf32(ram,t+4,2);mem_wf32(ram,t+8,0.5f);
    framerate_update_delta(ram,t);expect(ram,t,2,"30 FPS leaves original timer untouched");
    _putenv_s("PSPRECO_FPS","90");framerate_init(ram);
    if(framerate_is_60())failures++;
    _putenv_s("PSPRECO_FPS","60");framerate_init(ram);
    if(framerate_is_60())failures++;
    mem_w32(ram,G_ACX_VBLANKISR_FRAMEGATE_DIV2+0x34,0x28420002);
    framerate_init(ram);if(!framerate_is_60()||!installed)return 3;
    installed(&c,ram);if(gate_calls!=1)failures++;
    mem_wf32(ram,t+12,4);
    clock_tick=100;framerate_update_delta(ram,t);expect(ram,t,1,"first tick");
    clock_tick=101;framerate_update_delta(ram,t);expect(ram,t,1,"one elapsed vblank");
    clock_tick=103;framerate_update_delta(ram,t);expect(ram,t,2,"two elapsed vblanks");
    framerate_update_delta(ram,t);expect(ram,t,0,"duplicate call cannot advance time");
    clock_tick=1000;framerate_update_delta(ram,t);expect(ram,t,4,"original long-frame clamp");
    clock_tick=0xffffffffu;framerate_update_delta(ram,t);
    clock_tick=0x100000000ull;framerate_update_delta(ram,t);expect(ram,t,1,"vblank counter wrap");
    mem_wf32(ram,t+128,7);framerate_update_delta(ram,t+128);
    if(mem_rf32(ram,t+128)!=7)failures++;
    /* Enter a movie, overlap contexts, and leave through normal/Finish
     * cleanup. The original divider and coherent two-tick delta must resume. */
    framerate_movie_begin();
    if(framerate_is_60())failures++;
    expected_gate=0;installed(&c,ram);
    expected_gate=1;installed(&c,ram);
    if(mem_r32(ram,G_VBLANKDIVIDER_COUNTTO2)!=0)failures++;
    framerate_movie_begin();
    framerate_movie_end();
    if(framerate_is_60())failures++;
    clock_tick=0x100000064ull;framerate_update_delta(ram,t);
    expect(ram,t,2,"movie entry has no accumulated-time jump");
    clock_tick+=2;framerate_update_delta(ram,t);
    expect(ram,t,2,"30 FPS movie advances two ticks per frame");
    framerate_movie_end();
    if(!framerate_is_60())failures++;
    installed(&c,ram);
    clock_tick+=100;framerate_update_delta(ram,t);
    expect(ram,t,1,"60 FPS restored without stale movie time");
    framerate_movie_end();
    if(!framerate_is_60())failures++;
    printf("PASS movie divider, nested contexts, and cleanup\n");
    /* Ten seconds of actual 60 Hz elapsed time must retain 300 original
     * mission/AI/gun/input updates, 300 integer rack ticks, and 600 draws.
     * Every wrapper must restore the shared timer for the 60 Hz player. */
    framerate_movie_begin();framerate_movie_end();
    for(int frame=0;frame<600;frame++) {
        clock_tick++;framerate_update_delta(ram,t);mem_w32(ram,0x08a82e6c,60);
        for(int id=1;id<11;id++) {
            c.r[R_A1]=1;wrappers[id](&c,ram);
            if(mem_rf32(ram,t)!=1||mem_rf32(ram,t+4)!=1||mem_rf32(ram,t+8)!=1)failures++;
        }
        c.f[13]=10;wrappers[12](&c,ram);if(c.f[0]!=(frame%2?10:17)||mem_r32(ram,0x08a82e6c)!=60)failures++;
        wrappers[11](&c,ram);if(mem_r32(ram,0x08a82e6c)!=60)failures++;
        wrappers[13](&c,ram);
        {double epsilon;memcpy(&epsilon,ram+0x08a437c8,8);if(epsilon!=.002)failures++;}
        c.r[R_A1]=2;wrappers[4](&c,ram);wrappers[5](&c,ram);
    }
    for(int id=1;id<11;id++)if(id!=3&&calls[id]!=300)failures++;
    if(calls[12]!=300||calls[11]!=600||rack_decrements!=300||draw_calls[4]!=600||draw_calls[5]!=600)failures++;
    printf("legacy mission=%d gun=%d plane=%d ground=%d rack=%u draws=%d/%d\n",
           calls[1],calls[2],calls[4],calls[5],rack_decrements,draw_calls[4],draw_calls[5]);
    /* A clamped four-vblank stall represents two original simulation steps. */
    clock_tick+=4;framerate_update_delta(ram,t);c.r[R_A1]=1;
    wrappers[1](&c,ram);wrappers[2](&c,ram);wrappers[3](&c,ram);
    if(calls[1]!=302||calls[2]!=302||rack_decrements!=302)failures++;
    if(mem_rf32(ram,t)!=4||mem_rf32(ram,t+4)!=4||mem_rf32(ram,t+8)!=0.25f)failures++;
    printf("PASS legacy cadence, integer reload carry, draw messages, stall catch-up, and timer restoration\n");
    c.f[13]=10;wrappers[12](&c,ram);if(c.f[0]!=24||mem_r32(ram,0x08a82e6c)!=60)failures++;
    printf("PASS axis velocity cadence, skipped-step carry, and catch-up\n");
    printf("Timing regression failures: %d\n",failures);
    return failures?1:0;
}
