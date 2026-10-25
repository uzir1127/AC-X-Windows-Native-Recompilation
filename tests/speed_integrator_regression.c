#include "pspreco.h"
#include "symbols.h"
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

u8 *g_ram;
static CpuCtx seed;
static u8 *snapshot;
static u32 aircraft;

static float simulate(float delta, int seconds, float speed, float pitch)
{
    int i, steps = (int)(60 * seconds / delta);
    memcpy(g_ram + 0x08000000u, snapshot, 0x02000000u);
    mem_wf32(g_ram, aircraft + 0xa4, speed);
    mem_wf32(g_ram, aircraft + 0xb4, pitch);
    for (i=0; i<steps; i++) {
        CpuCtx c = seed;
        mem_wf32(g_ram, 0x08a82e40, delta);
        mem_wf32(g_ram, 0x08a82e44, delta);
        mem_wf32(g_ram, 0x08a82e48, 1.0f / delta);
        func_088729ec(&c, g_ram);
    }
    return mem_rf32(g_ram, aircraft + 0xa4);
}
int main(int argc, char **argv)
{
    FILE *f;
    int i, j, failures = 0;
    float speeds[] = {5.0f, 13.888889f, 20.0f, 30.0f};
    float pitches[] = {-0.25f, -0.1f, 0.0f, 0.1f, 0.25f};
    if (argc != 3) return 2;
    f=fopen(argv[1],"rb"); if(!f) return 3;
    if(fread(&seed,1,sizeof(seed),f)!=sizeof(seed)) return 4;
    fclose(f);
    snapshot=(u8*)malloc(0x02000000u);
    g_ram=(u8*)calloc(1,PSP_RAM_SIZE);
    if(!snapshot || !g_ram) return 5;
    f=fopen(argv[2],"rb");if(!f) return 6;
    if(fread(snapshot,1,0x02000000u,f)!=0x02000000u)return 7;
    fclose(f);
    aircraft=seed.r[R_A1];
    if(aircraft<0x08000000 || aircraft>=0x09fff000) return 8;
    cpu_set_host_fpu_mode();
    hle_init();
    printf("Actual captured flight integrator: aircraft=%08x params=%08x cpu=%zu\n",aircraft,seed.r[R_A2],sizeof(seed));
    printf("speed_in,attitude_field_b4,speed_30,speed_60,difference_kmh,relative_error\n");
    for(i=0;i<4;i++) for(j=0;j<5;j++) {
        float a=simulate(2.0f,10,speeds[i],pitches[j]);
        float b=simulate(1.0f,10,speeds[i],pitches[j]);
        float err=fabsf(a-b)/fmaxf(fabsf(a),0.01f);
        printf("%.6f,%.3f,%.9f,%.9f,%.6f,%.9f\n",speeds[i],pitches[j],a,b,fabsf(a-b)*43.2f,err);
        if(!isfinite(a)||!isfinite(b)||err>0.01f) failures++;
    }
    printf("Cases: 20, >1%% deviation or non-finite: %d\n",failures);
    return failures?1:0;
}