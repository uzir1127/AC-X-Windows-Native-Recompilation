#ifndef PSPRECO_PRESENT_H
#define PSPRECO_PRESENT_H

#include "pspreco.h"

int  present_init(const char *title);

int  present_is_active(void);

int  present_frame(u8 *ram, u32 addr, u32 stride, int fpf, int w, int h);

u32  present_buttons(void);
void present_analog(u8 *lx, u8 *ly);

void present_analog2(u8 *rx, u8 *ry);
void present_triggers(float *lt, float *rt);

#define PRESENT_AUX_L3  (1u << 0)
#define PRESENT_AUX_R3  (1u << 1)
u32  present_aux(void);

void present_shutdown(void);

typedef struct PresentVkInfo {
    void *instance, *physical, *device, *queue;
    u32   queue_family;
} PresentVkInfo;
int  present_get_vk(PresentVkInfo *out);

int  present_image(void *vk_image, int src_w, int src_h);

#endif
