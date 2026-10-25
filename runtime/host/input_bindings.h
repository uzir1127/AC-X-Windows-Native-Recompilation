#ifndef ACX_INPUT_BINDINGS_H
#define ACX_INPUT_BINDINGS_H
#include <SDL3/SDL.h>
#ifdef __cplusplus
extern "C" {
#endif
enum {
    IA_ROLL_L, IA_ROLL_R, IA_PITCH_DOWN, IA_PITCH_UP,
    IA_YAW_L, IA_YAW_R, IA_BRAKE, IA_ACCEL,
    IA_GUN, IA_MISSILE, IA_WEAPON, IA_TARGET, IA_VIEW, IA_RADAR, IA_PAUSE,
    IA_LOOK_L, IA_LOOK_R, IA_LOOK_UP, IA_LOOK_DOWN,
    IA_DPAD_UP, IA_DPAD_DOWN, IA_DPAD_LEFT, IA_DPAD_RIGHT, IA_COUNT
};
enum { IB_AUTO = -1, IB_NONE, IB_KEY, IB_MOUSE, IB_WHEEL,
       IB_BUTTON, IB_AXIS, IB_HAT };
enum { IB_KEYBOARD, IB_MOUSE_DEVICE, IB_CONTROLLER };
typedef struct { int kind, index, direction; } InputBinding;
const char *input_action_name(int action);
void input_bindings_init(void);
void input_bindings_reload(void);
int input_bindings_save(void);
const char *input_bindings_path(void);
void input_bindings_device(SDL_Joystick *joy);
InputBinding input_binding_get(int source, int action);
void input_binding_set(int source, int action, InputBinding binding);
void input_binding_label(int source, int action, char *text, size_t size);
void input_bindings_reset(int source);
void input_bindings_capture(int source, int action);
int input_bindings_capturing(void);
int input_bindings_block_navigation(void);
void input_bindings_cancel_capture(void);
int input_bindings_event(const SDL_Event *event);
void input_bindings_values(int source, const bool *keys, SDL_Gamepad *pad,
                           SDL_Joystick *joy, float values[IA_COUNT]);
void input_bindings_end_poll(void);
void input_bindings_release(void);
#ifdef __cplusplus
}
#endif
#endif
