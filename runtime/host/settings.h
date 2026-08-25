#ifndef PSPRECO_SETTINGS_H
#define PSPRECO_SETTINGS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { OVR_GAME = 0, OVR_OFF = 1, OVR_ON = 2 };
enum { TEXF_GAME = 0, TEXF_LINEAR = 1, TEXF_NEAREST = 2 };
enum { ASPECT_KEEP = 0, ASPECT_INTEGER = 1, ASPECT_STRETCH = 2, ASPECT_4_3 = 3 };
enum { PRESENT_FIFO = 0, PRESENT_MAILBOX = 1, PRESENT_IMMEDIATE = 2 };
enum { SHADE_GAME = 0, SHADE_FLAT = 1, SHADE_GOURAUD = 2 };

typedef struct {
    int   fullscreen;
    int   window_scale;
    int   aspect_mode;
    int   present_filter;
    int   present_mode;
    float letterbox[3];

    int   throttle;
    int   speed_percent;
    int   frameskip;
    int   show_fps;
    int   show_stats;

    int   res_scale;
    int   tex_filter;
    int   mipmaps_on;
    int   anisotropy;
    int   rtt;
    int   tex_replace;

    int   fog;
    int   lighting;
    int   culling;
    int   alpha_test;
    int   dither;
    int   depth_test;
    int   depth_write;
    int   blending;
    int   texturing;
    int   shading;

    float fog_distance;
    float fog_start;
    int   fog_color_force;
    float fog_color[3];
    int   skip_lines, skip_points;

    int   rstick_cam;
    int   rstick_invert;
    int   rstick_sens;
    int   analog_yaw;
    int   r3_view;

    float brightness;
    float contrast;
    float saturation;
    float gamma;

    int   ge_stats_log;
    int   ge_cmd_log;
    int   frame_dump_every;
    int   tex_dump;
} GfxSettings;

extern GfxSettings g_gfx;

enum { PRESET_ULTRA = 0, PRESET_HIGH = 1, PRESET_MEDIUM = 2, PRESET_LOW = 3 };
const char *gfx_preset_name(int preset);
void        gfx_preset_apply(int preset);
int         gfx_preset_current(void);

void gfx_settings_defaults(void);
int  gfx_settings_load(void);
int  gfx_settings_save(void);
const char *gfx_settings_path(void);

extern unsigned g_gfx_generation;
void gfx_settings_touch(void);

#ifdef __cplusplus
}
#endif

#endif
