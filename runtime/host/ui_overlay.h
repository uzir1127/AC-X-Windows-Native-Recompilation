#ifndef PSPRECO_UI_OVERLAY_H
#define PSPRECO_UI_OVERLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void    *window;
    void    *instance;
    void    *physical;
    void    *device;
    void    *queue;
    uint32_t queue_family;
} UiVkInfo;

int  ui_init(const UiVkInfo *vk);
void ui_shutdown(void);

void ui_swapchain(uint32_t format, uint32_t width, uint32_t height,
                  uint32_t count, void *const *images);

int  ui_handle_event(const void *sdl_event);

int  ui_visible(void);
void ui_toggle(void);
void ui_toggle_readout(void);

void ui_new_frame(void);

int  ui_render(void *cmd, uint32_t index);

void ui_note_frame(double frame_ms);

/* The parameter-table editor in ui_tableedit.cpp, on its own key so it can be
 * open while the graphics menu is not. */
void plane_ui_draw(void);
void plane_ui_toggle(void);
int  plane_ui_visible(void);

#ifdef __cplusplus
}
#endif

#endif
