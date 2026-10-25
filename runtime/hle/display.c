#include "hle.h"
#include "framerate.h"
#include "modloader.h"
#include "script.h"
#include "acx_api.h"
#include "sce_errors.h"
#include "threadman.h"
#include "../gpu/framedump.h"
#include "../gpu/ge.h"
#include "../host/present.h"
#include "../host/settings.h"

#include <stdio.h>

extern unsigned long g_ge_us_acc;
extern unsigned long long g_prof_xform_tk, g_prof_gpu_tk;
extern unsigned long long g_prof_prim_tk, g_prof_tex_tk, g_prof_rback_tk;
extern unsigned long g_prof_on;
extern unsigned long long g_prof_wait_tk;
extern unsigned long g_prof_wait_n;
u64 ge_prof_now_tk(void);

#define PSP_REFRESH_MILLIHZ 59940u
#define PSP_FRAME_US        (1000000000u / PSP_REFRESH_MILLIHZ)
#define PSP_LINES_PER_FRAME 286u
#define PACE_RESYNC_FRAMES  4u

static int g_mode, g_width = 480, g_height = 272;
static u32 g_framebuf, g_bufwidth, g_pixfmt, g_sync;
static u32 g_vcount;
static u64 g_accumulated_hcount;
static unsigned g_game_frames;

u32 display_vcount(void) { return g_vcount; }
unsigned display_game_frames(void) { return g_game_frames; }

#if defined(_WIN32)
#  include <windows.h>
static u64 pace_now_us(void)
{
    static double inv_freq;
    LARGE_INTEGER t;
    if (inv_freq == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        inv_freq = 1000000.0 / (double)f.QuadPart;
    }
    QueryPerformanceCounter(&t);
    return (u64)((double)t.QuadPart * inv_freq);
}

#define PACE_SPIN_MARGIN_US 1000u
#define PACE_MIN_SLEEP_US   2000u

static void pace_sleep_us(u64 us)
{
    static int period_set;
    if (!period_set) {
        period_set = 1;
        if (timeBeginPeriod(1) != TIMERR_NOERROR)
            period_set = -1;
    }
    if (us > PACE_MIN_SLEEP_US)
        Sleep((DWORD)((us - PACE_SPIN_MARGIN_US) / 1000));
}
#else
#  include <time.h>
#define PACE_SPIN_MARGIN_US 1000u
#define PACE_MIN_SLEEP_US   2000u
static u64 pace_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000u + (u64)(ts.tv_nsec / 1000);
}
static void pace_sleep_us(u64 us)
{
    struct timespec ts;
    if (us <= PACE_MIN_SLEEP_US) return;
    ts.tv_sec = (time_t)((us - PACE_SPIN_MARGIN_US) / 1000000u);
    ts.tv_nsec = (long)(((us - PACE_SPIN_MARGIN_US) % 1000000u) * 1000u);
    nanosleep(&ts, NULL);
}
#endif

static void pace_frame(void)
{
    static int checked, env_off;
    static u64 next_us;
    u64 now, frame_us;

    if (!checked) {
        const char *e = getenv("PSPRECO_UNTHROTTLED");
        checked = 1;
        env_off = (e && e[0] == '1');
    }
    if (!present_is_active() || env_off || !g_gfx.throttle) return;

    frame_us = (u64)PSP_FRAME_US * 100u
             / (u32)(g_gfx.speed_percent > 0 ? g_gfx.speed_percent : 100);
    if (!frame_us) frame_us = 1;

    now = pace_now_us();
    if (!next_us || now > next_us + PACE_RESYNC_FRAMES * frame_us)
        next_us = now;
    if (next_us > now) {
        pace_sleep_us(next_us - now);
        while (pace_now_us() < next_us) { }
    }
    next_us += frame_us;
}

void hle_sceDisplaySetMode(CpuCtx *c, u8 *ram)
{
    (void)ram;
    g_mode = (int)HLE_A0;
    g_width = (int)HLE_A1;
    g_height = (int)HLE_A2;
    hle_log("[hle] sceDisplaySetMode(mode=%d, %dx%d)", g_mode, g_width, g_height);
    HLE_RET(0);
}

void hle_sceDisplaySetFrameBuf(CpuCtx *c, u8 *ram)
{
    (void)ram;
    g_framebuf = HLE_A0;
    g_bufwidth = HLE_A1;
    g_pixfmt   = HLE_A2;
    g_sync     = HLE_A3;
    g_game_frames++;
    hle_log("[hle] sceDisplaySetFrameBuf(addr=%08x, bufw=%u, fmt=%u, sync=%u)",
            g_framebuf, g_bufwidth, g_pixfmt, g_sync);
    HLE_RET(0);
}

void hle_sceDisplayGetFrameBuf(CpuCtx *c, u8 *ram)
{
    if (HLE_A0) mem_w32(ram, HLE_A0, g_framebuf);
    if (HLE_A1) mem_w32(ram, HLE_A1, g_bufwidth);
    if (HLE_A2) mem_w32(ram, HLE_A2, g_pixfmt);
    HLE_RET(0);
}

void display_vblank(CpuCtx *c, u8 *ram)
{
    g_vcount++;
    g_accumulated_hcount += PSP_LINES_PER_FRAME;
    /* The context is published first so a tick can call into the game.
     * script_tick runs the reload and the deferred `ready` pass before
     * acx_api_tick fires "frame", so a frame handler cannot run ahead of its
     * own ready handler on the first tick. */
    acx_api_set_context(c, ram);
    mod_tick(c, ram);
    script_tick(c, ram);
    acx_api_tick(c, ram);
    ge_set_frame(g_vcount);
    pace_frame();
    if (framedump_enabled())
        framedump_frame(ram, g_framebuf, g_bufwidth, (int)g_pixfmt,
                        g_width, g_height);
    {
        static unsigned skip_phase;
        int present_this = 1;
        if (g_gfx.frameskip > 0) {
            present_this = (skip_phase == 0);
            skip_phase = (skip_phase + 1) % (unsigned)(g_gfx.frameskip + 1);
        } else {
            skip_phase = 0;
        }
        if (present_this) {
            int shown = -1;
            if (ge_gpu_present_fn)
                shown = ge_gpu_present_fn(g_framebuf, (int)g_pixfmt, g_bufwidth);
            if (shown == 0)
                threadman_request_exit();
            else if (shown < 0
                     && !present_frame(ram, g_framebuf, g_bufwidth, (int)g_pixfmt,
                                       g_width, g_height))
                threadman_request_exit();
        }
    }
    {
        static u32 last_v;
        static unsigned last_g;
        static u64 last_wall;
        if (g_vcount - last_v >= 60u) {
            u64 now = pace_now_us();
            if (last_wall) {
                u64 dt = now - last_wall;
                unsigned games = g_game_frames - last_g;
                double secs = (double)dt / 1000000.0;
                unsigned long ge_us = g_ge_us_acc;
                char prof[192];
                g_ge_us_acc = 0;
                prof[0] = 0;
                if (g_prof_on) {
                    static u64 last_tk;
                    u64 tk_now = ge_prof_now_tk();
                    u64 span = last_tk ? tk_now - last_tk : 0;
                    double ms = (double)(dt / 1000u);
                    if (span)
                        snprintf(prof, sizeof prof,
                                 " [prim=%.0f state=%.0f xform=%.0f hook=%.0f "
                                 "tex=%.0f rback=%.0f wait=%.0f/%lu]",
                                 (double)g_prof_prim_tk / (double)span * ms,
                                 (double)ge_us / 1000.0
                                 - (double)g_prof_prim_tk / (double)span * ms,
                                 (double)g_prof_xform_tk / (double)span * ms,
                                 (double)g_prof_gpu_tk / (double)span * ms,
                                 (double)g_prof_tex_tk / (double)span * ms,
                                 (double)g_prof_rback_tk / (double)span * ms,
                                 (double)g_prof_wait_tk / (double)span * ms,
                                 g_prof_wait_n);
                    last_tk = tk_now;
                    g_prof_xform_tk = g_prof_gpu_tk = g_prof_wait_tk = 0;
                    g_prof_prim_tk = g_prof_tex_tk = g_prof_rback_tk = 0;
                    g_prof_wait_n = 0;
                }
                fprintf(stderr,
                        "[fps] real=%.1f game=%u vblank=%u wall=%llums "
                        "(budget %.0f%%) ge=%lums (%.0f%% of wall) guest=%lums "
                        "res=%d skip=%d%s\n",
                        secs > 0.0 ? (double)games / secs : 0.0,
                        games, g_vcount - last_v,
                        (unsigned long long)(dt / 1000u),
                        secs > 0.0 ? 100.0 * secs : 0.0,
                        ge_us / 1000u,
                        dt ? 100.0 * (double)ge_us / (double)dt : 0.0,
                        (unsigned long)((dt > ge_us ? dt - ge_us : 0) / 1000u),
                        g_gfx.res_scale, g_gfx.frameskip,
                        prof);
            }
            last_wall = now;
            last_v = g_vcount;
            last_g = g_game_frames;
        }
    }
    ge_deliver_interrupts(c, ram);
    intrman_dispatch(c, ram, (u32)intrman_vblank_intr());
}

u32 display_frame_us(void) { return PSP_FRAME_US; }

void hle_sceDisplayWaitVblankStart(CpuCtx *c, u8 *ram)
{
    /* ACX_Frame_Render has already slept until the frame-gate interrupt
     * before calling the main flip (a1 == 1). Its original flip then asks
     * IsVblank, which this runtime cannot time accurately and reports false.
     * Reuse that completed wait only at this verified game call site. Other
     * flip paths (movies/loading/UI, a1 == 0) still wait normally, and guest
     * display/audio/kernel time remains 59.94 Hz. */
    if (framerate_is_60() && c->r[R_RA] == 0x089e1708u
        && mem_r32(ram, c->r[R_FP] + 4u) == 1u) {
        HLE_RET(0);
        return;
    }
    HLE_RET(0);
    threadman_wait_vblank();
}

void hle_sceDisplayIsVblank(CpuCtx *c, u8 *ram)
{
    (void)ram;
    HLE_RET(0);
}

#define VCOUNT_POLL_US 100u

void hle_sceDisplayGetVcount(CpuCtx *c, u8 *ram)
{
    (void)ram;
    HLE_RET(g_vcount);
    threadman_block_us(VCOUNT_POLL_US);
}

void hle_sceDisplayGetCurrentHcount(CpuCtx *c, u8 *ram)
{
    u64 into_frame = threadman_now_us() % PSP_FRAME_US;
    (void)ram;
    HLE_RET((u32)(into_frame * PSP_LINES_PER_FRAME / PSP_FRAME_US));
}

void hle_sceDisplayGetAccumulatedHcount(CpuCtx *c, u8 *ram)
{
    (void)ram;
    HLE_RET((u32)g_accumulated_hcount);
}

void hle_sceDisplayGetFramePerSec(CpuCtx *c, u8 *ram)
{
    (void)ram;
    c->f[0] = 59.940059f;
    HLE_RET(0);
}
