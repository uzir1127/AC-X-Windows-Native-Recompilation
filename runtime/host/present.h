#ifndef PSPRECO_PRESENT_H
#define PSPRECO_PRESENT_H

#include "pspreco.h"

#ifdef __cplusplus
extern "C" {
#endif

int  present_init(const char *title);

int  present_is_active(void);

int  present_frame(u8 *ram, u32 addr, u32 stride, int fpf, int w, int h);

u32  present_buttons(void);
u32  present_flight_buttons(void);
void present_analog(u8 *lx, u8 *ly);

void present_analog2(u8 *rx, u8 *ry);
void present_triggers(float *lt, float *rt);

#define PRESENT_AUX_L3  (1u << 0)
#define PRESENT_AUX_R3  (1u << 1)
#define PRESENT_AUX_LB  (1u << 2)
#define PRESENT_AUX_RB  (1u << 3)
u32  present_aux(void);
const char *present_controller_name(void);
void present_controller_select(unsigned id);
int present_rumble_supported(void);
void present_rumble_test(void);
float present_rudder(void);

void present_shutdown(void);

typedef struct PresentVkInfo {
    void *instance, *physical, *device, *queue;
    u32   queue_family;
} PresentVkInfo;
int  present_get_vk(PresentVkInfo *out);

int  present_image(void *vk_image, int src_w, int src_h);

#ifdef __cplusplus
}
#endif
#endif
