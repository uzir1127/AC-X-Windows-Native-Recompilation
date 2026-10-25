/* Global time scaling; see timectl.h. */

#include "timectl.h"
#include "framerate.h"
#include "hooks.h"
#include "hle.h"
#include "acx_symbols.h"

#include <stdio.h>
#include <stdlib.h>

extern u8 *g_ram;

static RecompFn s_orig;
static float    s_scale = 1.0f;
static u64      s_ticks;
static u64      s_after;
static int      s_dbg = -1;

float timectl_scale(void)   { return s_scale; }
int   timectl_frozen(void)  { return s_scale == 0.0f; }

void timectl_set_scale(float s)
{
    if (s < 0.0f) s = 0.0f;
    if (s > 8.0f) s = 8.0f;
    s_scale = s;
}

/* After the original, which is what computes the delta. */
static void ov_frame_timer(CpuCtx *c, u8 *ram)
{
    /* a0 is the timer struct and the delta is its first field.  Taking it
     * from the argument rather than g_ft_delta_60HzTicks scales a second
     * timer too, if the game ever runs one.
     *
     * Snapshot before the call: a0..a3 are not preserved across one, so
     * reading afterwards gets whatever the producer left in the register. */
    u32 t = c->r[R_A0];

    if (s_orig) s_orig(c, ram);
    framerate_update_delta(ram, t);
    s_ticks++;
    /* The scale applies to the whole game, menu timers included, so freezing
     * from boot stops the frontend too.  PSPRECO_TIME_AFTER holds off until a
     * given tick so the freeze can be tested without a controller. */
    if (s_dbg < 0) s_dbg = getenv("PSPRECO_TIME_DEBUG") ? 1 : 0;
    if (s_dbg && (s_ticks % 300) == 0)
        printf("[time] tick %llu: struct %08x delta %.4f (scale %.2f, "
               "after %llu)\n", (unsigned long long)s_ticks, (unsigned)t,
               t ? (double)mem_rf32(ram, t) : -1.0, (double)s_scale,
               (unsigned long long)s_after);

    if (s_ticks < s_after) return;
    if (s_scale != 1.0f && t) mem_wf32(ram, t, mem_rf32(ram, t) * s_scale);
}

void timectl_init(void)
{
    const char *e = getenv("PSPRECO_TIME_SCALE");
    const char *a = getenv("PSPRECO_TIME_AFTER");
    if (hook_install(G_ACX_FRAMETIMER_UPDATE_DELTAPRODUCER, ov_frame_timer,
                     &s_orig) == HOOK_OK)
        printf("[time] scaling ACX_FrameTimer_Update_DeltaProducer (%08x)\n",
               (unsigned)G_ACX_FRAMETIMER_UPDATE_DELTAPRODUCER);
    if (a && *a) s_after = (u64)strtoull(a, NULL, 10);
    if (e && *e) {
        timectl_set_scale((float)atof(e));
        printf("[time] PSPRECO_TIME_SCALE=%s -> %.3fx\n", e,
               (double)s_scale);
    }
}
