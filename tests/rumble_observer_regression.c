#include "pspreco.h"
#include "hooks.h"
#include "rumble.h"
#include "../runtime/host/rumble_envelope.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static RecompFn table[4];
static int bad, spawned = 1, fire_missile, fire_special, fail_index = -1;
static unsigned calls[4];
static u64 tick=60;
#define CHECK(x) do { if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); ++bad; } } while (0)
int hook_available(void) { return 1; }
int hook_index(u32 a) { return a == 0x08878170 ? 0 : a == 0x089c6bec ? 1 : a == 0x08878f90 ? 2 : a == 0x088729ec ? 3 : -1; }
int hook_install(u32 a, RecompFn f, RecompFn *old) {
    int i = hook_index(a); if (i < 0 || i == fail_index) return -1;
    if (old) *old = table[i]; table[i] = f; return HOOK_OK;
}
u64 threadman_vblank_count(void) { return tick; }
static void original_speed(CpuCtx *c, u8 *ram) { (void)c; (void)ram; ++calls[3]; }
static void original_spawn(CpuCtx *c, u8 *ram) { (void)ram; ++calls[1]; c->r[R_V0] = spawned ? 0x08900000 : 0; }
static void original_gun(CpuCtx *c, u8 *ram) { ++calls[0]; table[1](c, ram); }
static void original_weapon(CpuCtx *c, u8 *ram) {
    ++calls[2];
    u32 s = c->r[R_A1];
    if (fire_missile && mem_r8(ram, s + 0x12)) mem_w8(ram, s + 0x12, mem_r8(ram,s+0x12)-1);
    if (fire_special && mem_r8(ram, s + 0x13)) mem_w8(ram, s + 0x13, mem_r8(ram,s+0x13)-1);
    c->r[R_V0] = 17;
}
int main(void) {
    u8 *ram = calloc(1, PSP_RAM_SIZE), *copy = malloc(0x2000000);
    if (!ram || !copy) return 2;
    const u32 p = 0x08880000, state = 0x08a8b9c0;
    table[0]=original_gun; table[1]=original_spawn; table[2]=original_weapon; table[3]=original_speed;
    /* Unsupported image and partial install must leave every hook alone. */
    rumble_install(ram); CHECK(table[0]==original_gun);
    mem_w32(ram,0x08878170,0x27bdff00);mem_w32(ram,0x089c6bec,0x27bdffd0);mem_w32(ram,0x08878f90,0x27bdfe70);
    mem_w32(ram,0x088729ec,0x27bdffa0);
    fail_index=2;rumble_install(ram);CHECK(table[0]==original_gun && table[1]==original_spawn && table[2]==original_weapon);
    fail_index=-1;rumble_install(ram);CHECK(table[0]!=original_gun);
    mem_w32(ram,0x08a82ed8,p);mem_w8(ram,0x08a82e08,12);mem_w32(ram,0x08b7f1b0,0);mem_w16(ram,p+0xb0,325);
    CpuCtx c={0}; c.r[R_A0]=p;c.r[R_A1]=state;
    unsigned damage=0;
    CHECK(rumble_poll(ram,1,&damage)==0);
    memcpy(copy,ram+0x08000000,0x2000000);
    CpuCtx expected=c; original_spawn(&expected,ram);calls[1]=0;
    table[0](&c,ram);
    CHECK(calls[0]==1&&calls[1]==1&&!memcmp(&c,&expected,sizeof c));
    CHECK(!memcmp(copy,ram+0x08000000,0x2000000));
    CHECK(rumble_poll(ram,1,&damage)==RUMBLE_GUN && damage==0);
    CHECK(rumble_poll(ram,1,&damage)==0);
    spawned=0;table[0](&c,ram);CHECK(rumble_poll(ram,1,&damage)==0);
    spawned=1;c.r[R_A0]=p+0x1000;table[0](&c,ram);CHECK(rumble_poll(ram,1,&damage)==0);
    c.r[R_A0]=p;table[1](&c,ram);CHECK(rumble_poll(ram,1,&damage)==0);
    mem_w8(ram,state+0x12,5);mem_w8(ram,state+0x13,3);
    table[2](&c,ram);CHECK(rumble_poll(ram,1,&damage)==0); /* cooldown/failed launch */
    fire_missile=1;table[2](&c,ram);CHECK(rumble_poll(ram,1,&damage)==RUMBLE_MISSILE);
    fire_missile=0;fire_special=1;table[2](&c,ram);CHECK(rumble_poll(ram,1,&damage)==RUMBLE_SPECIAL);
    c.r[R_A0]=p+0x1000;table[2](&c,ram);CHECK(rumble_poll(ram,1,&damage)==0);c.r[R_A0]=p;
    mem_w8(ram,state+0x13,0);table[2](&c,ram);CHECK(rumble_poll(ram,1,&damage)==0);
    mem_w16(ram,p+0xb0,300);memcpy(copy,ram+0x08000000,0x2000000);
    CHECK(rumble_poll(ram,1,&damage)==RUMBLE_DAMAGE&&damage==25);
    CHECK(!memcmp(copy,ram+0x08000000,0x2000000));
    mem_w16(ram,p+0xb0,325);CHECK(!rumble_poll(ram,1,&damage)); /* repairs */
    mem_w32(ram,0x08b7f1b0,1);table[0](&c,ram);CHECK(!rumble_poll(ram,1,&damage));
    mem_w16(ram,p+0xb0,200);mem_w32(ram,0x08b7f1b0,0);CHECK(!rumble_poll(ram,1,&damage));
    table[0](&c,ram);CHECK(!rumble_poll(ram,0,&damage));CHECK(!rumble_poll(ram,1,&damage));
    mem_w8(ram,0x08a80c3c,1);mem_w16(ram,p+0xb0,100);CHECK(!rumble_poll(ram,1,&damage));
    mem_w16(ram,p+0xb0,0);CHECK(rumble_poll(ram,1,&damage)==RUMBLE_DAMAGE&&damage==100);
    mem_w8(ram,0x08a82e08,21);table[0](&c,ram);CHECK(!rumble_poll(ram,1,&damage));
    mem_w32(ram,0x08a82ed8,0xffffffff);CHECK(!rumble_poll(ram,1,&damage));
    RumbleEnvelope e={0}; uint16_t lo,hi;
    rumble_envelope_event(&e,RUMBLE_DAMAGE,100,1000);
    rumble_envelope_event(&e,RUMBLE_GUN,0,1020);
    rumble_envelope_sample(&e,1020,100,&lo,&hi);CHECK(lo>50000&&hi>40000);
    rumble_envelope_sample(&e,1650,100,&lo,&hi);CHECK(!lo&&!hi);
    rumble_envelope_clear(&e);rumble_envelope_event(&e,RUMBLE_GUN,0,2000);
    rumble_envelope_sample(&e,2000,50,&lo,&hi);CHECK(lo>=24000&&hi>=30000);
    rumble_envelope_sample(&e,2000,0,&lo,&hi);CHECK(!lo&&!hi);
    /* Same elapsed time yields the same motor amplitude at every sampling rate. */
    for(int hz=30;hz<=240;hz+=30){for(int i=0;i<hz;i++)rumble_envelope_sample(&e,2000+(1000*i/hz),70,&lo,&hi);
        rumble_envelope_sample(&e,2030,70,&lo,&hi);CHECK(lo>=33000&&hi>=42000);}
    mem_w32(ram,0x08a82ed8,p);mem_w8(ram,0x08a82e08,12);mem_w32(ram,0x08b7f1b0,0);mem_w16(ram,p+0xb0,325);
    CHECK(rumble_airspeed_level(701.0f/43.2f)==0);
    CHECK(rumble_airspeed_level(700.0f/43.2f)==8000);
    CHECK(rumble_airspeed_level(525.0f/43.2f)==9000);
    CHECK(rumble_airspeed_level(350.0f/43.2f)==10000);
    CHECK(rumble_airspeed_level(100.0f/43.2f)==10000);
    CHECK(!rumble_airspeed_level(NAN)&&!rumble_airspeed_level(-1));
    unsigned last=0;
    for(int kmh=700;kmh>=0;--kmh){unsigned level=rumble_airspeed_level(kmh/43.2f);CHECK(level>=last&&level<=10000);last=level;}
    mem_wf32(ram,state+0xa4,800.0f/43.2f);table[3](&c,ram);CHECK(!rumble_poll(ram,1,&damage));
    mem_wf32(ram,state+0xa4,700.0f/43.2f);memcpy(copy,ram+0x08000000,0x2000000);expected=c;
    table[3](&c,ram);CHECK(!memcmp(&c,&expected,sizeof c)&&!memcmp(copy,ram+0x08000000,0x2000000));
    CHECK(rumble_poll(ram,1,&damage)==RUMBLE_STALL&&rumble_buffet_level(ram)==8000);
    tick+=5;CHECK(!rumble_poll(ram,1,&damage)&&!rumble_buffet_level(ram));
    table[3](&c,ram);CHECK(rumble_poll(ram,1,&damage)==RUMBLE_STALL);
    CHECK(!rumble_poll(ram,0,&damage));mem_w32(ram,0x08b7f1b0,1);CHECK(!rumble_poll(ram,1,&damage)&&!rumble_buffet_level(ram));mem_w32(ram,0x08b7f1b0,0);
    mem_wf32(ram,state+0xa4,350.0f/43.2f);table[3](&c,ram);CHECK(rumble_buffet_level(ram)==10000);
    mem_wf32(ram,state+0xa4,701.0f/43.2f);table[3](&c,ram);CHECK(!rumble_poll(ram,1,&damage));
    mem_wf32(ram,state+0xa4,NAN);table[3](&c,ram);CHECK(!rumble_poll(ram,1,&damage));
    for(int level=8000;level<=10000;level+=1000)for(int ms=0;ms<1000;ms+=7){lo=hi=0;rumble_envelope_stall(ms,100,level,&lo,&hi);CHECK(lo==(unsigned)(65535ull*level/10000)&&hi<=lo);}
    printf("Rumble observer/envelope failures: %d\n",bad);free(copy);free(ram);return bad?1:0;
}
