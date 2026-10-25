#ifndef PSPRECO_QOL_INPUT_H
#define PSPRECO_QOL_INPUT_H

#include "pspreco.h"

RecompFn qol_override(u32 addr, RecompFn orig);

typedef struct {
    int   rstick_cam;
    int   rstick_invert;
    int   rstick_sens;
    int   modern_controls;
    int   r3_view;
} QolCfg;

extern QolCfg g_qol;

void qol_input_init(void);
void qol_input_install(u8 *ram);
int  qol_player_active(const u8 *ram);
int  qol_modern_controls_active(void);
u32  qol_shoulder_buttons(u32 base, int lb, int rb, int lt, int rt, int flying);

#endif
