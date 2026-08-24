#ifndef PSPRECO_QOL_INPUT_H
#define PSPRECO_QOL_INPUT_H

#include "pspreco.h"

RecompFn qol_override(u32 addr, RecompFn orig);

typedef struct {
    int   rstick_cam;
    int   rstick_invert;
    int   rstick_sens;
    int   analog_yaw;
    int   r3_view;
} QolCfg;

extern QolCfg g_qol;

void qol_input_init(void);

#endif
