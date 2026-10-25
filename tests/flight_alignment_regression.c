/* Exercise the complete captured player update, including FlowAlign and
 * BankTurn feedback. Private CPU/RAM fixtures are supplied by the maintainer. */
#include "pspreco.h"
#include "symbols.h"
#include "hle.h"
#include "framerate.h"
#include "hooks.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

u8 *g_ram;
static CpuCtx seed;
static u8 *snapshot;
static u64 tick;
static int input_pattern, input_frame;
static RecompFn pitch_orig, yaw_orig, roll_orig;
u64 test_vblank_count(void) { return tick; }

static float axis_input(int axis)
{
    int f = input_frame;
    if (input_pattern == 1 && axis == 2) return f < 40 ? .75f : (f < 80 ? -.75f : 0);
    if (input_pattern == 2 && axis == 0) return f < 40 ? .50f : (f < 80 ? -.50f : 0);
    if (input_pattern == 3 && axis == 1) return f < 40 ? .75f : (f < 80 ? -.75f : 0);
    if (input_pattern == 4) {
        if (axis == 2) return f < 40 ? .50f : (f < 80 ? -.50f : 0);
        if (axis == 0) return f >= 80 && f < 120 ? .40f : (f >= 120 && f < 160 ? -.40f : 0);
        if (axis == 1) return f >= 160 && f < 200 ? .50f : (f >= 200 && f < 240 ? -.50f : 0);
    }
    return 0;
}
static void pitch_input(CpuCtx *c, u8 *ram) { mem_wf32(ram,c->r[R_A1]+0xf8u,axis_input(0));pitch_orig(c,ram); }
static void yaw_input(CpuCtx *c, u8 *ram) { mem_wf32(ram,c->r[R_A1]+0x100u,axis_input(1));yaw_orig(c,ram); }
static void roll_input(CpuCtx *c, u8 *ram) { mem_wf32(ram,c->r[R_A1]+0xfcu,axis_input(2));roll_orig(c,ram); }

static void simulate(int native, float bank, float out[3])
{
    u32 obj = seed.r[R_A0], air = seed.r[R_A1];
    int dt = native ? 1 : 2;
    memcpy(g_ram + 0x08000000u, snapshot, 0x02000000u);
    mem_wf32(g_ram, obj + 0x40u, 0);
    mem_wf32(g_ram, obj + 0x44u, 0);
    mem_wf32(g_ram, obj + 0x48u, bank);
    mem_wf32(g_ram, air + 0xe8u, 0);
    mem_wf32(g_ram, air + 0xecu, 0);
    mem_wf32(g_ram, air + 0x140u, 0);
    mem_wf32(g_ram, air + 0x144u, 0);
    mem_wf32(g_ram, air + 0x148u, -13.888889f);
    if (native) { framerate_movie_begin(); framerate_movie_end(); }
    for (int f = 0; f < 600; f += dt) {
        input_frame = f;
        CpuCtx c = seed;
        mem_w32(g_ram, 0x08a82e6cu, 60u / dt);
        mem_wf32(g_ram, 0x08a82e40u, (float)dt);
        mem_wf32(g_ram, 0x08a82e44u, (float)dt);
        mem_wf32(g_ram, 0x08a82e48u, 1.0f / dt);
        if (native) { ++tick; framerate_update_delta(g_ram, 0x08a82e40u); }
        /* Rebuild the current flight basis as the live frame prelude does. */
        c.r[R_A0] = 0x08a8be10u;
        c.r[R_A1] = obj + 0x40u;
        c.r[R_A2] = 0;
        func_0885a194(&c, g_ram);
        mem_wf32(g_ram, air + 0xf8u, 0);
        mem_wf32(g_ram, air + 0xfcu, 0);
        mem_wf32(g_ram, air + 0x100u, 0);
        c = seed;
        c.r[R_A1] = 1;
        c.r[R_A2] = 0;
        func_08867adc(&c, g_ram);
    }
    for (int k = 0; k < 3; ++k) out[k] = mem_rf32(g_ram, obj + 0x40u + 4u*k);
}

int main(int argc, char **argv)
{
    FILE *f;
    float banks[] = {-.9f, -.7f, -.5f, -.3f, -.1f, .1f, .3f, .5f, .7f, .9f};
    float ref[14][3];
    int bad = 0;
    if (argc != 3) return 2;
    f = fopen(argv[1], "rb");
    if (!f || fread(&seed, 1, sizeof seed, f) != sizeof seed) return 3;
    fclose(f);
    snapshot = malloc(0x02000000u);
    g_ram = calloc(1, PSP_RAM_SIZE);
    if (!snapshot || !g_ram) return 4;
    f = fopen(argv[2], "rb");
    if (!f || fread(snapshot, 1, 0x02000000u, f) != 0x02000000u) return 5;
    fclose(f);
    cpu_set_host_fpu_mode();
    hle_init();
    if (hook_install(0x08871a4cu,pitch_input,&pitch_orig) != HOOK_OK
        || hook_install(0x0887211cu,yaw_input,&yaw_orig) != HOOK_OK
        || hook_install(0x088722acu,roll_input,&roll_orig) != HOOK_OK) return 7;
    for (int i = 0; i < 10; ++i) simulate(0, banks[i], ref[i]);
    for (int p = 1; p <= 4; ++p) { input_pattern=p;simulate(0,0,ref[9+p]); }
    input_pattern=0;
    _putenv_s("PSPRECO_FPS", "60");
    mem_w32(g_ram, 0x08804b74u, 0x28420002u);
    framerate_init(g_ram);
    if (!framerate_is_60()) return 6;
    puts("bank,pitch30,pitch60,yaw30,yaw60,roll30,roll60,max_angle_error");
    for (int i = 0; i < 10; ++i) {
        float out[3], err = 0;
        simulate(1, banks[i], out);
        for (int k = 0; k < 3; ++k) {
            float d = fabsf(remainderf(out[k] - ref[i][k], 6.28318530718f));
            if (!isfinite(out[k]) || d > .003f) ++bad;
            if (d > err) err = d;
        }
        printf("%.2f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f\n", banks[i],
               ref[i][0], out[0], ref[i][1], out[1], ref[i][2], out[2], err);
    }
    for (int p = 1; p <= 4; ++p) {
        float out[3], err=0;
        input_pattern=p;simulate(1,0,out);
        for (int k=0;k<3;++k) {
            float d=fabsf(remainderf(out[k]-ref[9+p][k],6.28318530718f));
            if (!isfinite(out[k]) || d>.003f) ++bad;
            if(d>err)err=d;
        }
        printf("pattern=%d pitch=%.9f/%.9f yaw=%.9f/%.9f roll=%.9f/%.9f error=%.9f\n",p,
               ref[9+p][0],out[0],ref[9+p][1],out[1],ref[9+p][2],out[2],err);
        if (p==3 && (fabsf(out[0])>1e-5f || fabsf(out[2])>1e-5f)) ++bad;
    }
    /* A sideways velocity takes FlowAlign's original zero-forward early
     * return. Filter pre-scaling must not remain latched on that path. */
    memcpy(g_ram+0x08000000u,snapshot,0x02000000u);
    {
        CpuCtx c=seed;
        u32 air=seed.r[R_A1], matrix=0x09ff0000u;
        double epsilon, after;
        memcpy(&epsilon,g_ram+0x08a437c8u,8);
        for(int k=0;k<16;++k)mem_wf32(g_ram,matrix+4u*k,k%5==0?1.0f:0.0f);
        mem_wf32(g_ram,air+0x140u,1);mem_wf32(g_ram,air+0x144u,0);mem_wf32(g_ram,air+0x148u,0);mem_wf32(g_ram,air+0x14cu,0);
        mem_wf32(g_ram,air+0xf0u,.25f);mem_wf32(g_ram,air+0xf4u,-.5f);
        mem_wf32(g_ram,0x08a82e40u,1);
        c.r[R_A0]=air;c.r[R_A1]=seed.r[R_A2];c.r[R_A2]=matrix;
        g_func_ptr[hook_index(0x08874aacu)](&c,g_ram);
        memcpy(&after,g_ram+0x08a437c8u,8);
        if(mem_rf32(g_ram,air+0xf0u)!=.25f || mem_rf32(g_ram,air+0xf4u)!=-.5f
           || mem_rf32(g_ram,0x08a437c0u)!=.5f || after!=epsilon) ++bad;
        puts("Verified zero-forward filter early return and shared constant restoration.");
    }
    printf("flight alignment failures: %d\n", bad);
    return bad ? 1 : 0;
}
