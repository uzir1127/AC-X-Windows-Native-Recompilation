/* ACX native 60 Hz. The display, audio and kernel clocks retain their PSP
 * rate. Rendering and player integration use coherent 60 Hz deltas while
 * original frame-counted gameplay systems retain their 30 Hz step.
 * PSPRECO_FPS=30 retains original behavior; the host selects the saved mode. */
#include "framerate.h"
#include "hooks.h"
#include "threadman.h"
#include "hle.h"
#include "acx_symbols.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static RecompFn s_gate;
static int s_enabled, s_have_tick;
static u32 s_previous_tick;
static unsigned s_movie_contexts;
static int s_reset_gate;
static float s_legacy_fraction;
static unsigned s_legacy_steps, s_rate_steps;
static RecompFn s_mission, s_gun, s_cooldowns, s_plane, s_ground, s_bank, s_rate, s_flow, s_euler;
static unsigned s_legacy_depth;
static RecompFn s_pad_level, s_pad_brake, s_pad_accel, s_pad_target, s_pad_missile;

/* Several ACX systems count original 30 Hz updates instead of consulting the
 * shared 60 Hz delta. Preserve their original step and all their internal
 * timers together. Player analog flight integration and rendering stay 60 Hz.
 * Using elapsed vblanks carries an odd tick across frames and handles a missed
 * presentation without permanently losing simulation time. */
static void legacy_call(RecompFn fn, CpuCtx *c, u8 *ram, u32 skipped_result)
{
    CpuCtx entry;
    u32 saved[3], fps;
    unsigned i;
    if (!framerate_is_60() || s_legacy_depth) { fn(c, ram); return; }
    if (!s_legacy_steps) { c->r[R_V0] = skipped_result; return; }
    entry = *c;
    for (i = 0; i < 3; ++i)
        saved[i] = mem_r32(ram, G_FT_DELTA_60HZTICKS + 4u * i);
    mem_wf32(ram, G_FT_DELTA_60HZTICKS, 2.0f);
    mem_wf32(ram, G_FT_DELTA_60HZTICKS + 4u, 2.0f);
    mem_wf32(ram, G_FT_DELTA_60HZTICKS + 8u, 0.5f);
    fps = mem_r32(ram, 0x08a82e6cu);
    mem_w32(ram, 0x08a82e6cu, 30u);
    ++s_legacy_depth;
    for (i = 0; i < s_legacy_steps; ++i) { *c = entry; fn(c, ram); }
    --s_legacy_depth;
    mem_w32(ram, 0x08a82e6cu, fps);
    for (i = 0; i < 3; ++i)
        mem_w32(ram, G_FT_DELTA_60HZTICKS + 4u * i, saved[i]);
}

static void mission_30(CpuCtx *c, u8 *ram) { legacy_call(s_mission, c, ram, 1); }
static void gun_30(CpuCtx *c, u8 *ram) { legacy_call(s_gun, c, ram, c->r[R_V0]); }
static void plane_30(CpuCtx *c, u8 *ram)
{
    /* Message 1 is simulation. Init, draw, destruction and damage messages
     * must always run, including on frames without a legacy simulation step. */
    if (c->r[R_A1] == 1u) legacy_call(s_plane, c, ram, 0);
    else s_plane(c, ram);
}
static void ground_30(CpuCtx *c, u8 *ram)
{
    if (c->r[R_A1] == 1u) legacy_call(s_ground, c, ram, 0);
    else s_ground(c, ram);
}
/* The stock rate limiter mixes FPS-normalized acceleration with a fixed
 * per-call damping factor. Preserve its original velocity response at 30 Hz;
 * its callers still integrate the resulting velocity with the 60 Hz delta.
 * NPC/legacy callers already have the original cadence and FPS normalization. */
static void rate_30(CpuCtx *c, u8 *ram)
{
    CpuCtx entry;
    float velocity;
    u32 fps;
    unsigned i;
    if (!framerate_is_60() || s_legacy_depth) { s_rate(c, ram); return; }
    velocity = c->f[13];
    if (!s_rate_steps) { c->f[0] = velocity; return; }
    entry = *c;
    fps = mem_r32(ram, 0x08a82e6cu);
    mem_w32(ram, 0x08a82e6cu, 30u);
    for (i = 0; i < s_rate_steps; ++i) {
        *c = entry;
        c->f[13] = velocity;
        s_rate(c, ram);
        velocity = c->f[0];
    }
    mem_w32(ram, 0x08a82e6cu, fps);
}

static void bank_60(CpuCtx *c, u8 *ram)
{
    u32 fps;
    if (!framerate_is_60()) { s_bank(c, ram); return; }
    /* The sink term is stored as velocity and later multiplied by the shared
     * frame delta. Stock BankTurn also divides that velocity by measured FPS,
     * which halves altitude loss again at 60 Hz. Retain the original 30 Hz
     * normalization here; other FPS-based per-frame corrections stay intact. */
    fps = mem_r32(ram, 0x08a82e6cu);
    mem_w32(ram, 0x08a82e6cu, 30u);
    s_bank(c, ram);
    mem_w32(ram, 0x08a82e6cu, fps);
}

static void flow_60(CpuCtx *c, u8 *ram)
{
    u32 saved[2];
    u32 air = c->r[R_A0], sp = c->r[R_SP], blend;
    u32 old_slip[2];
    int adjust_filter = 0;
    double deadband;
    float dt;
    if (!framerate_is_60() || s_legacy_depth) { s_flow(c, ram); return; }
    dt = mem_rf32(ram, G_FT_DELTA_60HZTICKS);
    if (!isfinite(dt) || dt <= 0.0f) { s_flow(c, ram); return; }
    /* FlowAlign rejects angular corrections below 0.002 per original frame.
     * A 60 Hz bank produces half that angle, so retaining the fixed threshold
     * can suppress a sustained turn altogether. Keep the same angular-rate
     * threshold. Scope the constant to this call; 30 Hz/NPC paths retain the
     * original value and the original alignment/filter implementation. */
    saved[0] = mem_r32(ram, 0x08a437c8u);
    saved[1] = mem_r32(ram, 0x08a437ccu);
    blend = mem_r32(ram, 0x08a437c0u);
    /* The original filter is (old + measuredSlip) * 0.5 each 30 Hz step.
     * Preserve its time constant: oldWeight = 0.5^(dt/2), with the remaining
     * weight assigned to the measurement. Pre-scaling the old state lets the
     * original routine perform that update and its existing rotation/clamp. */
    if (air >= 0x08800000u && air < 0x09fff000u
        && mem_rf32(ram, 0x08a437c0u) == 0.5f && dt != 2.0f) {
        float old_weight = powf(0.5f, dt * 0.5f);
        float measurement_weight = 1.0f - old_weight;
        float gain = old_weight / measurement_weight;
        old_slip[0] = mem_r32(ram, air + 0xf0u);
        old_slip[1] = mem_r32(ram, air + 0xf4u);
        mem_wf32(ram, air + 0xf0u, u2f(old_slip[0]) * gain);
        mem_wf32(ram, air + 0xf4u, u2f(old_slip[1]) * gain);
        mem_wf32(ram, 0x08a437c0u, measurement_weight);
        adjust_filter = 1;
    }
    memcpy(&deadband, saved, sizeof deadband);
    deadband *= (double)dt * 0.5;
    memcpy(ram + 0x08a437c8u, &deadband, sizeof deadband);
    s_flow(c, ram);
    /* Verified US FlowAlign uses a 0x170-byte frame and keeps local forward
     * velocity at frame+0xa8. A zero value skips the filter entirely; undo
     * our pre-scaling on that original early-return path. No guest call runs
     * between return and this read of the just-completed stack frame. */
    if (adjust_filter && sp >= 0x08804170u && sp <= 0x0a000000u
        && mem_rf32(ram, sp - 0xc8u) == 0.0f) {
        mem_w32(ram, air + 0xf0u, old_slip[0]);
        mem_w32(ram, air + 0xf4u, old_slip[1]);
    }
    mem_w32(ram, 0x08a437c0u, blend);
    mem_w32(ram, 0x08a437c8u, saved[0]);
    mem_w32(ram, 0x08a437ccu, saved[1]);
}

static void euler_60(CpuCtx *c, u8 *ram)
{
    u32 temp = c->r[R_A0], out = c->r[R_A1], sp = c->r[R_SP];
    int player = framerate_is_60() && !s_legacy_depth
              && mem_r32(ram, 0x08a82ed8u) >= 0x08800000u
              && mem_r32(ram, 0x08a82ed8u) < 0x09fff000u
              && out == mem_r32(ram, 0x08a82ed8u) + 0x40u;
    s_euler(c, ram);
    if (player && sp >= 0x08804170u && sp <= 0x0a000000u) {
        float roll = mem_rf32(ram, out + 8u);
        /* The original acos(cosRoll) loses precision close to level flight.
         * At 60 Hz it can invent a roll step from cos=0.99999994, sin=0,
         * which feeds back into the next orientation. The verified routine
         * leaves the decomposed sin/cos pair at frame+0x114/+0x110. Use the
         * equivalent, stable atan2 only in this near-level player case. */
        float cosine = mem_rf32(ram, sp - 0x60u);
        float sine = mem_rf32(ram, sp - 0x5cu);
        if (isfinite(roll) && isfinite(cosine) && isfinite(sine)
            && fabsf(roll) < 0.01f && fabsf(sine) < 0.01f && cosine > 0.0f) {
            roll = atan2f(sine, cosine);
            mem_wf32(ram, out + 8u, roll);
            mem_wf32(ram, temp + 8u, roll);
        }
    }
}
static void cooldowns_60(CpuCtx *c, u8 *ram)
{
    u32 saved;
    if (!framerate_is_60()) { s_cooldowns(c, ram); return; }
    /* Stock code truncates roundedDelta * 0.5 to an integer. A one-tick
     * 60 Hz frame truncates to zero forever, so carry half ticks explicitly.
     * Run readiness/attachment processing every frame; only its decrement
     * receives the accumulated original 30 Hz ticks. */
    saved = mem_r32(ram, G_FT_DELTA_60HZTICKS + 4u);
    mem_wf32(ram, G_FT_DELTA_60HZTICKS + 4u, (float)(2u * s_legacy_steps));
    s_cooldowns(c, ram);
    mem_w32(ram, G_FT_DELTA_60HZTICKS + 4u, saved);
}
#define PAD_WRAPPER(name) \
    static void name##_30(CpuCtx *c, u8 *ram) \
    { legacy_call(s_pad_##name, c, ram, c->r[R_V0]); }
PAD_WRAPPER(level)
PAD_WRAPPER(brake)
PAD_WRAPPER(accel)
PAD_WRAPPER(target)
PAD_WRAPPER(missile)
#undef PAD_WRAPPER


int framerate_is_60(void) { return s_enabled && s_movie_contexts == 0; }

void framerate_movie_begin(void)
{
    if (s_movie_contexts++ != 0) return;
    s_have_tick = 0;
    s_legacy_fraction = 0;
    s_legacy_steps = 0;
    s_rate_steps = 0;
    s_reset_gate = 1;
    if (s_enabled)
        printf("[framerate] cinematic: original 30 FPS buffer cadence\n");
}

void framerate_movie_end(void)
{
    if (!s_movie_contexts || --s_movie_contexts != 0) return;
    s_have_tick = 0;
    s_legacy_fraction = 0;
    s_legacy_steps = 0;
    s_rate_steps = 0;
    s_reset_gate = 1;
    if (s_enabled)
        printf("[framerate] cinematic ended: native 60 FPS resumed\n");
}

static void frame_gate_60(CpuCtx *c, u8 *ram)
{
    /* Keep the original pause/thread-status checks and interrupt handling. */
    if (s_reset_gate) {
        mem_w32(ram, G_VBLANKDIVIDER_COUNTTO2, 0u);
        s_reset_gate = 0;
    }
    /* Movie frames arrive at 29.97 Hz. The main renderer must retain its
     * original cadence then, or it flips to the other buffer before that
     * buffer has a new movie frame, showing frames backwards and forwards. */
    if (framerate_is_60())
        mem_w32(ram, G_VBLANKDIVIDER_COUNTTO2, 1u);
    s_gate(c, ram);
}

void framerate_init(u8 *ram)
{
    const char *fps = getenv("PSPRECO_FPS");
    if (!fps || !*fps || strcmp(fps, "30") == 0) {
        printf("[framerate] original 30 FPS timing\n");
        return;
    }
    if (strcmp(fps, "60") != 0) {
        fprintf(stderr, "[framerate] invalid PSPRECO_FPS=%s; retaining 30 FPS\n", fps);
        return;
    }
    /* Fail closed if this is not the verified US executable/dispatch table. */
    if (!hook_available()
        || hook_index(G_ACX_FRAMETIMER_UPDATE_DELTAPRODUCER) < 0
        || mem_r32(ram, G_ACX_VBLANKISR_FRAMEGATE_DIV2 + 0x34u) != 0x28420002u) {
        fprintf(stderr, "[framerate] unsupported image or hooks; retaining 30 FPS\n");
        return;
    }
    {
        struct HookSpec { u32 addr; RecompFn wrapper; RecompFn *original; };
        struct HookSpec specs[] = {
            {G_ACX_VBLANKISR_FRAMEGATE_DIV2, frame_gate_60, &s_gate},
            {0x08965218u, mission_30, &s_mission},
            {0x08878170u, gun_30, &s_gun},
            {0x08878994u, cooldowns_60, &s_cooldowns},
            {0x088e799cu, plane_30, &s_plane},
            {0x088ed6e0u, ground_30, &s_ground},
            {0x0887dcb8u, level_30, &s_pad_level},
            {0x0887ddd8u, brake_30, &s_pad_brake},
            {0x0887dec0u, accel_30, &s_pad_accel},
            {0x0887dfa8u, target_30, &s_pad_target},
            {0x0887e0ccu, missile_30, &s_pad_missile},
            {0x08874ea0u, bank_60, &s_bank},
            {0x0887b998u, rate_30, &s_rate},
            {0x08874aacu, flow_60, &s_flow},
            {0x0886ec48u, euler_60, &s_euler},
        };
        unsigned i, count = sizeof(specs) / sizeof(specs[0]);
        for (i = 0; i < count; ++i) {
            if (hook_index(specs[i].addr) < 0) {
                fprintf(stderr, "[framerate] missing gameplay hook; retaining 30 FPS\n");
                return;
            }
        }
        for (i = 0; i < count; ++i) {
            if (hook_install(specs[i].addr, specs[i].wrapper, specs[i].original) != HOOK_OK) {
                while (i-- > 0) hook_install(specs[i].addr, *specs[i].original, NULL);
                fprintf(stderr, "[framerate] gameplay hook failed; retaining 30 FPS\n");
                return;
            }
        }
    }
    s_enabled = 1;
    printf("[framerate] native 60 FPS; one engine tick per guest vblank, PSP clock unchanged\n");
    printf("[framerate] mission scripts, NPC simulation, gun and button timers retain 30 Hz; rack reloads carry half ticks; axis damping retains 30 Hz\n");
}

void framerate_update_delta(u8 *ram, u32 timer)
{
    u32 tick, elapsed;
    float dt, cap;
    if (!s_enabled || timer != G_FT_DELTA_60HZTICKS) return;
    tick = (u32)threadman_vblank_count();
    elapsed = s_have_tick ? tick - s_previous_tick
                          : (framerate_is_60() ? 1u : 2u);
    s_previous_tick = tick;
    s_have_tick = 1;
    dt = (float)elapsed;
    cap = mem_rf32(ram, timer + 12u);
    if (isfinite(cap) && cap > 0.0f && dt > cap) dt = cap;
    {
        unsigned was_pending = s_legacy_fraction > 0.0f;
        s_legacy_fraction += dt;
        s_legacy_steps = (unsigned)(s_legacy_fraction * 0.5f);
        s_legacy_fraction -= (float)(2u * s_legacy_steps);
        /* Resolve velocity at the beginning of each original 30 Hz interval,
         * then integrate it on both 60 Hz halves. Resolving at the end would
         * introduce a half-frame lag and a cumulative angle offset. */
        s_rate_steps = s_legacy_steps + (s_legacy_fraction > 0.0f) - was_pending;
    }
    /* The stock producer quantizes milliseconds and rounds upwards. At a
     * 16/17 ms cadence that can count an extra tick. Use actual guest vblanks
     * and update every derived field: physics, countdowns and rates agree.
     * Repeated calls in one vblank advance no simulation time. */
    mem_wf32(ram, timer, dt);
    mem_wf32(ram, timer + 4u, dt);
    mem_wf32(ram, timer + 8u, dt > 0.0f ? 1.0f / dt : 0.0f);
}
