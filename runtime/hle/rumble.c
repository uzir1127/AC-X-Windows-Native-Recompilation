/* Host-only feedback, inspired by console flight-game rumble. No guest
 * registers, memory, simulation clocks or input values are modified here. */
#include "rumble.h"
#include "game_state.h"
#include "hooks.h"
#include "threadman.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define PLAYER_PTR 0x08a82ed8u
static RecompFn s_gun, s_spawn, s_weapon, s_speed;
static int s_installed, s_inside_player_gun;
static unsigned s_pending;
static u32 s_pending_player, s_player;
static unsigned s_hp, s_mission;
static u32 s_flight_player;
static unsigned s_flight_mission;
static u64 s_flight_tick;
static int s_stall;
static unsigned s_buffet_level;
static int player_mission(u8 *ram);

unsigned rumble_airspeed_level(float native_speed)
{
    /* One native speed unit is 43.2 km/h. Basis points retain a smooth ramp. */
    const float begin=700.0f/43.2f, full=350.0f/43.2f;
    if(!isfinite(native_speed) || native_speed<0 || native_speed>begin)return 0;
    if(native_speed<=full)return 10000;
    return 8000u+(unsigned)((begin-native_speed)*2000.0f/(begin-full)+0.5f);
}

unsigned rumble_buffet_level(u8 *ram)
{
    if(!acx_flight_active(ram)
        || s_flight_player!=mem_r32(ram,PLAYER_PTR)
        || s_flight_mission!=mem_r8(ram,0x08a80c3cu)
        || threadman_vblank_count()-s_flight_tick>4u)return 0;
    return s_buffet_level;
}

static int player_mission(u8 *ram)
{
    if (!ram) return 0;
    u32 p = mem_r32(ram, PLAYER_PTR);
    unsigned phase = mem_r8(ram, 0x08a82e08u);
    return p >= 0x08800000u && p < 0x09fff000u
        && (phase == 12 || phase == 22);
}

static void trace(u8 *ram, unsigned kind)
{
    u32 p = mem_r32(ram, PLAYER_PTR);
    /* Opt-in maintainer trace; never created in ordinary play. */
    static FILE *log;
    static int tried;
    if (!tried) {
        const char *path = getenv("ACX_RUMBLE_TRACE");
        tried = 1;
        if (path && *path) {
            log = fopen(path, "w");
            if (log) fputs("vblank,event,player\n", log);
        }
    }
    if (log) {
        fprintf(log, "%llu,%u,%08x\n", (unsigned long long)threadman_vblank_count(), kind, p);
        fflush(log);
    }
}

static void event(u8 *ram, unsigned kind)
{
    u32 p = mem_r32(ram, PLAYER_PTR);
    if (s_pending_player != p) s_pending = 0;
    s_pending_player = p; s_pending |= kind;
    trace(ram, kind);
}

void rumble_observe_flight(u8 *ram, u32 player, u32 state)
{
    if (!player_mission(ram) || player != mem_r32(ram, PLAYER_PTR)
        || state < 0x08800000u || state >= 0x09fff000u) return;
    /* Observe the original integrator's actual airspeed. The feedback curve
     * is intentionally earlier than the aircraft's mechanical stall point. */
    float speed = mem_rf32(ram, state + 0xa4u);
    unsigned mission = mem_r8(ram, 0x08a80c3cu);
    unsigned level=mem_r16(ram,player+0xb0u)>0 ? rumble_airspeed_level(speed) : 0;
    int stalled = level!=0;
    if (s_flight_player != player || s_flight_mission != mission) s_stall = 0;
    if (stalled != s_stall) trace(ram, stalled ? RUMBLE_STALL : 32);
    s_flight_player = player; s_flight_mission = mission;
    s_flight_tick = threadman_vblank_count(); s_stall = stalled;
    s_buffet_level=level;
}

static void speed(CpuCtx *c, u8 *ram)
{
    u32 player = c->r[R_A0], state = c->r[R_A1];
    s_speed(c, ram);
    rumble_observe_flight(ram, player, state);
}

static void gun(CpuCtx *c, u8 *ram)
{
    int previous = s_inside_player_gun;
    s_inside_player_gun = player_mission(ram) && c->r[R_A0] == mem_r32(ram, PLAYER_PTR);
    s_gun(c, ram); /* includes the existing 30/60 Hz timing wrapper */
    s_inside_player_gun = previous;
}

static void spawn(CpuCtx *c, u8 *ram)
{
    s_spawn(c, ram);
    if (s_inside_player_gun && c->r[R_V0] != 0) event(ram, RUMBLE_GUN);
}

static void weapon(CpuCtx *c, u8 *ram)
{
    u32 state = c->r[R_A1];
    int player = player_mission(ram) && c->r[R_A0] == mem_r32(ram, PLAYER_PTR)
        && state >= 0x08800000u && state < 0x09fff000u;
    unsigned missile = player ? mem_r8(ram, state + 0x12) : 0;
    unsigned special = player ? mem_r8(ram, state + 0x13) : 0;
    s_weapon(c, ram);
    if (player) {
        if (mem_r8(ram, state + 0x12) < missile) event(ram, RUMBLE_MISSILE);
        if (mem_r8(ram, state + 0x13) < special) event(ram, RUMBLE_SPECIAL);
    }
}

void rumble_install(u8 *ram)
{
    struct { u32 address, instruction; RecompFn wrapper; RecompFn *original; } specs[] = {
        {0x08878170u, 0x27bdff00u, gun, &s_gun},
        {0x089c6becu, 0x27bdffd0u, spawn, &s_spawn},
        {0x08878f90u, 0x27bdfe70u, weapon, &s_weapon},
        {0x088729ecu, 0x27bdffa0u, speed, &s_speed},
    };
    if (s_installed) return;
    for (unsigned i = 0; i < 4; ++i) {
        if (!hook_available() || hook_index(specs[i].address) < 0
            || mem_r32(ram, specs[i].address) != specs[i].instruction) {
            fprintf(stderr, "[rumble] unsupported image; weapon feedback disabled\n");
            return;
        }
    }
    for (int i = 0; i < 4; ++i) {
        if (hook_install(specs[i].address, specs[i].wrapper, specs[i].original) != HOOK_OK) {
            while (i-- > 0) hook_install(specs[i].address, *specs[i].original, NULL);
            return;
        }
    }
    s_installed = 1;
    puts("[rumble] gameplay-ready; low-speed buffet 700km/h=80%, 350km/h=100%");
}

unsigned rumble_poll(u8 *ram, int active, unsigned *damage)
{
    unsigned pending = s_pending;
    s_pending = 0;
    *damage = 0;
    /* Observe actual shots independently of presentation state, but never
     * deliver or retain their effects while the world is paused/in menus. */
    if (!active || !acx_flight_active(ram)) {
        s_player = 0; return 0;
    }
    u32 p = mem_r32(ram, PLAYER_PTR);
    unsigned hp = mem_r16(ram, p + 0xb0u);
    unsigned mission = mem_r8(ram, 0x08a80c3cu);
    if (s_player == p && s_mission == mission && s_hp > hp) {
        *damage = s_hp - hp;
        event(ram, RUMBLE_DAMAGE);
        pending |= RUMBLE_DAMAGE;
        s_pending = 0;
    }
    s_player = p; s_hp = hp; s_mission = mission;
    if (s_pending_player != p) pending = 0;
    if (s_stall && s_flight_player == p && s_flight_mission == mission
        && threadman_vblank_count() - s_flight_tick <= 4u)
        pending |= RUMBLE_STALL;
    return pending;
}
