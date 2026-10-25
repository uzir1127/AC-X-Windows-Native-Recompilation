#include "pspreco.h"
#include "qol_input.h"
#include "hooks.h"
#include <stdio.h>
#include "../host/present.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define G_CAMERA_MSG  0x08866DA0u
#define G_YAW_L       0x0887DDD8u
#define G_YAW_R       0x0887DEC0u
#define G_VIEW_CYCLE  0x0887E528u
#define G_CFG         0x08A80AF4u

#define CFG_VIEW      322u

#define CAM_BASE      224u
#define CAM_FLAGS      20u
#define CAM_MODE       24u
#define CAM_PITCH      28u
#define CAM_YAW        32u

#define CAM_MODE_FREE_LOOK 4u

#define VIEW_COCKPIT       1
#define VIEW_PLAYABLE_END  3

#define LOOK_YAW_WIDE   (-2.9670596f)
#define LOOK_YAW_TIGHT  (-1.3962634f)
#define LOOK_PITCH      (-1.5533431f)

#define LOOK_SMOOTHING_DIVISOR 4.0f
#define LOOK_SETTLED_EPSILON   1.0e-4f

#define VIEW_CYCLE_TRIGGER 1

QolCfg g_qol = { 1, 0, 100, 1, 1 };

#define G_PLAYER_PTR 0x08a82ed8u
#define G_YAW_AXIS   0x0887211cu
static RecompFn s_orig_axis;
static int s_rudder_ready;

int qol_player_active(const u8 *ram)
{
    u32 player = mem_r32(ram, G_PLAYER_PTR);
    return player >= 0x08800000u && player < 0x09fff000u;
}

int qol_modern_controls_active(void)
{
    return g_qol.modern_controls && s_rudder_ready;
}

u32 qol_shoulder_buttons(u32 base, int lb, int rb, int lt, int rt, int flying)
{
    if (!flying || !qol_modern_controls_active()) {
        lt |= lb;
        rt |= rb;
    }
    if (lt) base |= 1u << 8;
    if (rt) base |= 1u << 9;
    return base;
}

static void ov_yaw_axis(CpuCtx *c, u8 *ram)
{
    u32 flight = c->r[R_A1];
    u32 aux = present_aux();
    u32 saved;
    if (!qol_modern_controls_active() || !qol_player_active(ram)
        || c->r[R_A0] != mem_r32(ram, G_PLAYER_PTR)
        || !(aux & (PRESENT_AUX_LB | PRESENT_AUX_RB))) {
        s_orig_axis(c, ram);
        return;
    }
    /* Positive ACX rudder input turns left. Feed the original normalized
     * axis, preserving aircraft authority, rate limiter and frame delta.
     * Both bumpers cancel. Never leave injected input latched. */
    saved = mem_r32(ram, flight + 0x100u);
    mem_wf32(ram, flight + 0x100u, present_rudder());
    s_orig_axis(c, ram);
    mem_w32(ram, flight + 0x100u, saved);
}

void qol_input_install(u8 *ram)
{
    if (s_rudder_ready) return;
    qol_input_init();
    /* Verified US executable: original YawAxis begins addiu sp,sp,-48. */
    if (mem_r32(ram, G_YAW_AXIS) != 0x27bdffd0u
        || hook_install(G_YAW_AXIS, ov_yaw_axis, &s_orig_axis) != HOOK_OK) {
        fprintf(stderr, "[input] rudder hook unavailable; retaining PSP shoulders\n");
        return;
    }
    s_rudder_ready = 1;
    printf("[input] modern layout available: LB/RB yaw, LT/RT brake/accelerate\n");
}

void qol_input_init(void)
{
    static int off = -1;
    if (off < 0) off = getenv("SR_QOL_OFF") ? 1 : 0;
    if (!off) return;
    memset(&g_qol, 0, sizeof g_qol);
    g_qol.rstick_sens = 100;
}

static u32 call_guest3(CpuCtx *c, u8 *ram, u32 fn, u32 a0, u32 a1, u32 a2)
{
    RecompFn f = recomp_lookup(fn);
    CpuCtx save;
    u32 ret;
    if (!f) return 0;
    memcpy(&save, c, sizeof save);
    c->r[R_A0] = a0;
    c->r[R_A1] = a1;
    c->r[R_A2] = a2;
    c->r[R_RA] = 0;
    f(c, ram);
    ret = c->r[R_V0];
    memcpy(c, &save, sizeof save);
    return ret;
}

static int s_r3_held;

static void qol_view_cycle(CpuCtx *c, u8 *ram, u32 player, u32 flight)
{
    int down;
    if (!g_qol.r3_view) return;
    down = (present_aux() & PRESENT_AUX_R3) != 0;
    if (!down) { s_r3_held = 0; return; }
    if (s_r3_held) return;
    s_r3_held = 1;
    call_guest3(c, ram, G_VIEW_CYCLE, player, flight, VIEW_CYCLE_TRIGGER);
}

static void yaw_common(CpuCtx *c, u8 *ram, RecompFn orig)
{
    u32 player = c->r[R_A0];
    u32 flight = c->r[R_A1];

    (void)player;
    (void)flight;
    if (orig) orig(c, ram);

    qol_view_cycle(c, ram, player, flight);
}

static float stick_axis(u8 raw)
{
    float v = ((float)raw - 128.0f) / 127.0f;
    if (v < -1.0f) v = -1.0f;
    if (v >  1.0f) v =  1.0f;
    return v;
}

static float s_look_yaw, s_look_pitch;
static int   s_look_view = -1;

static void look_reset(void)
{
    s_look_yaw = s_look_pitch = 0.0f;
    s_look_view = -1;
}

static void camera_after(u8 *ram, u32 entity)
{
    u32 cam = entity + CAM_BASE;
    int view = (int)(s8)(u8)mem_r8(ram, G_CFG + CFG_VIEW);
    u8 rx = 128, ry = 128;
    float ax, ay, yaw, pitch, sens;

    if (view < 0 || view >= VIEW_PLAYABLE_END
        || mem_r8(ram, cam + CAM_MODE) == CAM_MODE_FREE_LOOK) {
        look_reset();
        return;
    }
    if (view != s_look_view) {
        s_look_yaw = s_look_pitch = 0.0f;
        s_look_view = view;
    }

    present_analog2(&rx, &ry);
    ax = stick_axis(rx);
    ay = stick_axis(ry);
    if (!g_qol.rstick_invert) ay = -ay;

    sens = (float)g_qol.rstick_sens / 100.0f;
    yaw   = ax * (view == VIEW_COCKPIT ? LOOK_YAW_TIGHT : LOOK_YAW_WIDE) * sens;
    pitch = ay * LOOK_PITCH * sens;
    if (view == VIEW_COCKPIT && pitch < 0.0f) pitch = 0.0f;

    s_look_pitch += (pitch - s_look_pitch) / LOOK_SMOOTHING_DIVISOR;
    s_look_yaw   += (yaw   - s_look_yaw)   / LOOK_SMOOTHING_DIVISOR;

    if (ax == 0.0f && ay == 0.0f
        && fabsf(s_look_yaw) < LOOK_SETTLED_EPSILON
        && fabsf(s_look_pitch) < LOOK_SETTLED_EPSILON) {
        s_look_yaw = s_look_pitch = 0.0f;
        return;
    }

    mem_wf32(ram, cam + CAM_PITCH, s_look_pitch);
    mem_wf32(ram, cam + CAM_YAW,   s_look_yaw);

    mem_w8(ram, cam + CAM_MODE, CAM_MODE_FREE_LOOK);
}

static RecompFn s_orig_cam, s_orig_yaw_l, s_orig_yaw_r;

#define CAMERA_MSG_UPDATE 1u

static void ov_camera(CpuCtx *c, u8 *ram)
{
    u32 entity = c->r[R_A0];
    u32 msg    = c->r[R_A1] & 0xFFu;
    if (s_orig_cam) s_orig_cam(c, ram);
    if (msg != CAMERA_MSG_UPDATE || !entity) return;
    if (g_qol.rstick_cam) camera_after(ram, entity);
    else                  look_reset();
}

static void ov_yaw_l(CpuCtx *c, u8 *ram) { yaw_common(c, ram, s_orig_yaw_l); }
static void ov_yaw_r(CpuCtx *c, u8 *ram) { yaw_common(c, ram, s_orig_yaw_r); }

RecompFn qol_override(u32 addr, RecompFn orig)
{
    RecompFn ov;
    switch (addr) {
    case G_CAMERA_MSG: s_orig_cam   = orig; ov = ov_camera; break;
    case G_YAW_L:      s_orig_yaw_l = orig; ov = ov_yaw_l;  break;
    case G_YAW_R:      s_orig_yaw_r = orig; ov = ov_yaw_r;  break;
    default:           return NULL;
    }
    qol_input_init();
    return ov;
}
