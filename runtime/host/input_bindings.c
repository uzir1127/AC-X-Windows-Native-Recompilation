/* Action bindings for keyboard, mouse, SDL gamepads and arbitrary SDL joysticks.
 * This translates input only; it does not advance or modify the guest clock. */
#include "input_bindings.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define PROFILES 32
typedef struct { char guid[33]; InputBinding b[IA_COUNT]; } Profile;
static Profile s_profiles[PROFILES];
static InputBinding s_keyboard[IA_COUNT], s_mouse[IA_COUNT];
static int s_count, s_current = -1, s_loaded, s_dirty;
static int s_capture_action = -1, s_capture_source;
static Uint64 s_nav_block_until;
static SDL_JoystickID s_device;
static Sint16 s_capture_axes[64];
static float s_wheel;
static SDL_MouseButtonFlags s_mouse_pressed;
static Uint64 s_mouse_until[32], s_wheel_until;
static float s_wheel_pulse;
#ifdef PSPRECO_INPUT_TEST
static SDL_MouseButtonFlags s_test_mouse;
void input_bindings_test_mouse(SDL_MouseButtonFlags buttons) { s_test_mouse = buttons; }
#endif
static char s_path[4096];
static const char *const s_names[IA_COUNT] = {
    "Roll left", "Roll right", "Pitch down", "Pitch up", "Yaw left", "Yaw right",
    "Brake", "Accelerate", "Gun / confirm", "Missile / cancel", "Switch weapon",
    "Change target", "Change view", "Radar / map", "Pause / Start",
    "Camera left", "Camera right", "Camera up", "Camera down",
    "D-pad up", "D-pad down", "D-pad left", "D-pad right"
};
const char *input_action_name(int a) { return a >= 0 && a < IA_COUNT ? s_names[a] : "?"; }
static void defaults(int source)
{
    int i;
    if (source == IB_KEYBOARD) {
        const int keys[IA_COUNT] = {
            SDL_SCANCODE_A, SDL_SCANCODE_D, SDL_SCANCODE_W, SDL_SCANCODE_S,
            SDL_SCANCODE_Q, SDL_SCANCODE_E, SDL_SCANCODE_LCTRL, SDL_SCANCODE_SPACE,
            SDL_SCANCODE_J, SDL_SCANCODE_L, SDL_SCANCODE_K, SDL_SCANCODE_T,
            SDL_SCANCODE_V, SDL_SCANCODE_M, SDL_SCANCODE_ESCAPE,
            SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_6, SDL_SCANCODE_KP_8, SDL_SCANCODE_KP_2,
            SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT
        };
        for (i = 0; i < IA_COUNT; ++i) s_keyboard[i] = (InputBinding){ IB_KEY, keys[i], 1 };
    } else if (source == IB_MOUSE_DEVICE) {
        memset(s_mouse, 0, sizeof s_mouse);
        s_mouse[IA_GUN] = (InputBinding){ IB_MOUSE, SDL_BUTTON_LEFT, 1 };
        s_mouse[IA_MISSILE] = (InputBinding){ IB_MOUSE, SDL_BUTTON_RIGHT, 1 };
        s_mouse[IA_TARGET] = (InputBinding){ IB_MOUSE, SDL_BUTTON_MIDDLE, 1 };
        s_mouse[IA_WEAPON] = (InputBinding){ IB_WHEEL, 0, 0 };
    } else if (s_current >= 0) {
        for (i = 0; i < IA_COUNT; ++i) s_profiles[s_current].b[i] = (InputBinding){ IB_AUTO, 0, 0 };
    }
}
const char *input_bindings_path(void)
{
    const char *settings = gfx_settings_path();
    const char *slash = strrchr(settings, '/'), *back = strrchr(settings, '\\');
    size_t n;
    if (!slash || (back && back > slash)) slash = back;
    n = slash ? (size_t)(slash - settings + 1) : 0;
    if (n + sizeof "controls.ini" > sizeof s_path) return "controls.ini";
    memcpy(s_path, settings, n); strcpy(s_path + n, "controls.ini");
    return s_path;
}
static int profile(const char *guid)
{
    int i;
    for (i = 0; i < s_count; ++i) if (!strcmp(s_profiles[i].guid, guid)) return i;
    if (s_count == PROFILES) return -1;
    i = s_count++;
    SDL_strlcpy(s_profiles[i].guid, guid, sizeof s_profiles[i].guid);
    for (int a = 0; a < IA_COUNT; ++a) s_profiles[i].b[a] = (InputBinding){ IB_AUTO, 0, 0 };
    return i;
}
static int valid(InputBinding b, int source)
{
    if (b.kind == IB_NONE || (source == IB_CONTROLLER && b.kind == IB_AUTO)) return 1;
    if (source == IB_KEYBOARD) return b.kind == IB_KEY && b.index > 0 && b.index < SDL_SCANCODE_COUNT
        && b.index != SDL_SCANCODE_F1 && b.index != SDL_SCANCODE_F4
        && b.index != SDL_SCANCODE_F5 && b.index != SDL_SCANCODE_F6;
    if (source == IB_MOUSE_DEVICE)
        return (b.kind == IB_MOUSE && b.index > 0 && b.index <= 32)
            || (b.kind == IB_WHEEL && b.direction >= -1 && b.direction <= 1);
    return (b.kind == IB_BUTTON && b.index >= 0 && b.index < 512)
        || (b.kind == IB_AXIS && b.index >= 0 && b.index < 64 && b.direction != 0 && abs(b.direction) <= 2)
        || (b.kind == IB_HAT && b.index >= 0 && b.index < 64 && b.direction > 0 && b.direction <= 15);
}
void input_bindings_reload(void)
{
    FILE *f; char line[256]; int source = -1, p = -1;
    s_count = 0; s_current = -1; s_device = 0; s_dirty = 0; s_capture_action = -1;
    defaults(IB_KEYBOARD); defaults(IB_MOUSE_DEVICE);
    f = fopen(input_bindings_path(), "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            char guid[33]; int a; InputBinding b;
            if (!strncmp(line, "[keyboard]", 10)) { source = IB_KEYBOARD; p = -1; }
            else if (!strncmp(line, "[mouse]", 7)) { source = IB_MOUSE_DEVICE; p = -1; }
            else if (sscanf(line, "[controller:%32[0123456789abcdef]]", guid) == 1 && strlen(guid) == 32) {
                source = IB_CONTROLLER; p = profile(guid);
            } else if (line[0] == '[') source = -1;
            else if (sscanf(line, "%d = %d , %d , %d", &a, &b.kind, &b.index, &b.direction) == 4
                     && a >= 0 && a < IA_COUNT && source >= 0 && valid(b, source)) {
                if (source == IB_KEYBOARD) s_keyboard[a] = b;
                else if (source == IB_MOUSE_DEVICE) s_mouse[a] = b;
                else if (p >= 0) s_profiles[p].b[a] = b;
            }
        }
        fclose(f);
    }
    s_loaded = 1;
}
void input_bindings_init(void) { if (!s_loaded) input_bindings_reload(); }
void input_bindings_device(SDL_Joystick *joy)
{
    char guid[33]; SDL_JoystickID id = joy ? SDL_GetJoystickID(joy) : 0;
    input_bindings_init();
    if (id == s_device) return;
    s_device = id; s_current = -1;
    if (joy) {
        SDL_GUIDToString(SDL_GetJoystickGUID(joy), guid, sizeof guid);
        s_current = profile(guid);
    }
    s_capture_action = -1;
}
InputBinding input_binding_get(int source, int a)
{
    input_bindings_init();
    if (a < 0 || a >= IA_COUNT) return (InputBinding){IB_NONE,0,0};
    if (source == IB_KEYBOARD) return s_keyboard[a];
    if (source == IB_MOUSE_DEVICE) return s_mouse[a];
    return s_current >= 0 ? s_profiles[s_current].b[a] : (InputBinding){IB_AUTO,0,0};
}
void input_binding_set(int source, int a, InputBinding b)
{
    input_bindings_init();
    if (a < 0 || a >= IA_COUNT || !valid(b, source)) return;
    if (source == IB_KEYBOARD) s_keyboard[a] = b;
    else if (source == IB_MOUSE_DEVICE) s_mouse[a] = b;
    else if (s_current >= 0) s_profiles[s_current].b[a] = b;
    else return;
    s_dirty = 1;
}
void input_bindings_reset(int source) { input_bindings_init(); defaults(source); s_dirty = 1; }
int input_bindings_save(void)
{
    char temp[4120]; FILE *f; int ok;
    input_bindings_init();
    if (!s_dirty) return 1;
    SDL_snprintf(temp, sizeof temp, "%s.tmp", input_bindings_path());
    f = fopen(temp, "w"); if (!f) return 0;
    fprintf(f, "# ACX input bindings. Edit in F4 > Input. GUID-specific controller profiles.\n");
    for (int src = 0; src < 2 + s_count; ++src) {
        InputBinding *b = src == 0 ? s_keyboard : src == 1 ? s_mouse : s_profiles[src-2].b;
        if (src < 2) fprintf(f, "\n[%s]\n", src == 0 ? "keyboard" : "mouse");
        else fprintf(f, "\n[controller:%s]\n", s_profiles[src-2].guid);
        for (int a = 0; a < IA_COUNT; ++a)
            fprintf(f, "%d = %d, %d, %d # %s\n", a, b[a].kind, b[a].index, b[a].direction, s_names[a]);
    }
    ok = !ferror(f); if (fclose(f)) ok = 0;
    /* SDL rename replaces atomically on Windows; a failed write retains the
     * previous usable profile. Never remove the previous file first. */
    if (ok) ok = SDL_RenamePath(temp, input_bindings_path());
    if (ok) s_dirty = 0;
    return ok;
}
void input_binding_label(int source, int a, char *text, size_t n)
{
    InputBinding b = input_binding_get(source, a);
    switch (b.kind) {
    case IB_AUTO: SDL_strlcpy(text, "Zero default (SDL)", n); break;
    case IB_NONE: SDL_strlcpy(text, "Unbound", n); break;
    case IB_KEY: SDL_strlcpy(text, SDL_GetScancodeName((SDL_Scancode)b.index), n); break;
    case IB_MOUSE: SDL_snprintf(text, n, "Mouse button %d", b.index); break;
    case IB_WHEEL: SDL_snprintf(text, n, "Wheel %s", b.direction == 0 ? "either" : b.direction > 0 ? "up" : "down"); break;
    case IB_BUTTON: SDL_snprintf(text, n, "Button %d", b.index + 1); break;
    case IB_AXIS: SDL_snprintf(text, n, "Axis %d %s", b.index + 1, b.direction == 1 ? "+" : b.direction == -1 ? "-" : b.direction == 2 ? "full" : "full reversed"); break;
    case IB_HAT: SDL_snprintf(text, n, "Hat %d direction %d", b.index + 1, b.direction); break;
    default: SDL_strlcpy(text, "?", n); break;
    }
}
void input_bindings_capture(int source, int a)
{
    s_capture_source = source; s_capture_action = a;
    SDL_Joystick *joy = SDL_GetJoystickFromID(s_device);
    for (int i = 0; i < 64; ++i) s_capture_axes[i] = joy && i < SDL_GetNumJoystickAxes(joy) ? SDL_GetJoystickAxis(joy, i) : 0;
}
int input_bindings_capturing(void) { return s_capture_action >= 0; }
int input_bindings_block_navigation(void)
{
    return s_capture_action >= 0 || SDL_GetTicks() < s_nav_block_until;
}
void input_bindings_cancel_capture(void) { s_capture_action = -1; }
int input_bindings_event(const SDL_Event *e)
{
    InputBinding b = { IB_NONE, 0, 0 };
    if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN && e->button.button > 0 && e->button.button <= 32) {
        s_mouse_pressed |= SDL_BUTTON_MASK(e->button.button);
        s_mouse_until[e->button.button - 1] = SDL_GetTicksNS() + 50000000;
    }
    if (e->type == SDL_EVENT_MOUSE_WHEEL) {
        s_wheel += e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e->wheel.y : e->wheel.y;
        s_wheel_pulse = s_wheel; s_wheel_until = SDL_GetTicksNS() + 50000000;
    }
    if (s_capture_action < 0) return 0;
    if (e->type == SDL_EVENT_KEY_DOWN && e->key.key == SDLK_ESCAPE) { s_capture_action = -1; return 1; }
    if (s_capture_source == IB_KEYBOARD && e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat)
        b = (InputBinding){ IB_KEY, e->key.scancode, 1 };
    if (s_capture_source == IB_MOUSE_DEVICE) {
        if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN) b = (InputBinding){ IB_MOUSE, e->button.button, 1 };
        if (e->type == SDL_EVENT_MOUSE_WHEEL && s_wheel != 0) b = (InputBinding){ IB_WHEEL, 0, s_wheel > 0 ? 1 : -1 };
    }
    if (s_capture_source == IB_CONTROLLER) {
        if (e->type == SDL_EVENT_JOYSTICK_BUTTON_DOWN && e->jbutton.which == s_device)
            b = (InputBinding){ IB_BUTTON, e->jbutton.button, 1 };
        if (e->type == SDL_EVENT_JOYSTICK_HAT_MOTION && e->jhat.which == s_device && e->jhat.value)
            b = (InputBinding){ IB_HAT, e->jhat.hat, e->jhat.value };
        if (e->type == SDL_EVENT_JOYSTICK_AXIS_MOTION && e->jaxis.which == s_device && e->jaxis.axis < 64
            && abs(e->jaxis.value - s_capture_axes[e->jaxis.axis]) > 18000)
            b = (InputBinding){ IB_AXIS, e->jaxis.axis, e->jaxis.value > s_capture_axes[e->jaxis.axis] ? 1 : -1 };
    }
    if (b.kind != IB_NONE) {
        input_binding_set(s_capture_source, s_capture_action, b);
        s_capture_action = -1; s_nav_block_until = SDL_GetTicks() + 250;
    }
    /* Capture must not leak presses to the ImGui menu or the game. */
    return e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_KEY_UP
        || e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP
        || e->type == SDL_EVENT_MOUSE_WHEEL || e->type == SDL_EVENT_JOYSTICK_BUTTON_DOWN
        || e->type == SDL_EVENT_JOYSTICK_BUTTON_UP || e->type == SDL_EVENT_JOYSTICK_AXIS_MOTION
        || e->type == SDL_EVENT_JOYSTICK_HAT_MOTION;
}
static float positive(float v) { return v > 1 ? 1 : v > 0 ? v : 0; }
static float joy_binding(InputBinding b, SDL_Joystick *j)
{
    float v;
    if (!j || !SDL_JoystickConnected(j)) return 0;
    if (b.kind == IB_BUTTON && b.index < SDL_GetNumJoystickButtons(j)) return SDL_GetJoystickButton(j, b.index);
    if (b.kind == IB_HAT && b.index < SDL_GetNumJoystickHats(j)) return (SDL_GetJoystickHat(j, b.index) & b.direction) == b.direction;
    if (b.kind != IB_AXIS || b.index >= SDL_GetNumJoystickAxes(j)) return 0;
    v = SDL_GetJoystickAxis(j, b.index);
    if (abs(b.direction) == 2) return positive(b.direction > 0 ? (v + 32768) / 65535 : (32767 - v) / 65535);
    v /= v < 0 ? 32768.0f : 32767.0f;
    if (fabsf(v) <= g_gfx.input_deadzone / 100.0f) return 0;
    return positive(v * b.direction);
}
static void pad_defaults(SDL_Gamepad *p, float v[IA_COUNT])
{
    if (!p) return;
    float x = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX) / 32768.0f;
    float y = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY) / 32768.0f;
    float rx = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTX) / 32768.0f;
    float ry = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTY) / 32768.0f;
    float dead = g_gfx.input_deadzone / 100.0f;
    if (fabsf(x) <= dead) x = 0; if (fabsf(y) <= dead) y = 0;
    if (fabsf(rx) <= dead) rx = 0; if (fabsf(ry) <= dead) ry = 0;
    v[IA_ROLL_L] = positive(-x); v[IA_ROLL_R] = positive(x);
    v[IA_PITCH_DOWN] = positive(-y); v[IA_PITCH_UP] = positive(y);
    v[IA_LOOK_L] = positive(-rx); v[IA_LOOK_R] = positive(rx);
    v[IA_LOOK_UP] = positive(-ry); v[IA_LOOK_DOWN] = positive(ry);
    v[IA_BRAKE] = positive(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 32767.0f);
    v[IA_ACCEL] = positive(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 32767.0f);
    const SDL_GamepadButton buttons[] = { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
        SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
        SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_START,
        SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT };
    const int actions[] = { IA_YAW_L, IA_YAW_R, IA_GUN, IA_MISSILE, IA_WEAPON, IA_TARGET,
        IA_VIEW, IA_RADAR, IA_PAUSE, IA_DPAD_UP, IA_DPAD_DOWN, IA_DPAD_LEFT, IA_DPAD_RIGHT };
    for (int i = 0; i < 13; ++i) v[actions[i]] = SDL_GetGamepadButton(p, buttons[i]);
}
void input_bindings_values(int src, const bool *keys, SDL_Gamepad *pad, SDL_Joystick *joy, float v[IA_COUNT])
{
    SDL_MouseButtonFlags mouse = 0;
    input_bindings_init(); memset(v, 0, sizeof(float) * IA_COUNT);
    if (src == IB_CONTROLLER) { input_bindings_device(joy); pad_defaults(pad, v); }
    if (src == IB_MOUSE_DEVICE) mouse = SDL_GetMouseState(NULL, NULL);
#ifdef PSPRECO_INPUT_TEST
    if (src == IB_MOUSE_DEVICE) mouse = s_test_mouse;
#endif
    if (src == IB_MOUSE_DEVICE) mouse |= s_mouse_pressed;
    if (src == IB_MOUSE_DEVICE) {
        Uint64 now = SDL_GetTicksNS();
        for (int i = 0; i < 32; ++i) if (now < s_mouse_until[i]) mouse |= SDL_BUTTON_MASK(i+1);
        if (now < s_wheel_until && s_wheel == 0) s_wheel = s_wheel_pulse;
    }
    for (int a = 0; a < IA_COUNT; ++a) {
        InputBinding b = input_binding_get(src, a);
        if (b.kind == IB_AUTO) continue;
        if (src == IB_CONTROLLER) v[a] = joy_binding(b, joy);
        else if (b.kind == IB_KEY) v[a] = keys && keys[b.index];
        else if (b.kind == IB_MOUSE) v[a] = (mouse & SDL_BUTTON_MASK(b.index)) != 0;
        else if (b.kind == IB_WHEEL) v[a] = b.direction == 0 ? s_wheel != 0 : s_wheel * b.direction > 0;
    }
}
void input_bindings_end_poll(void) { s_wheel = 0; s_mouse_pressed = 0; }
void input_bindings_release(void)
{
    input_bindings_end_poll(); s_wheel_until = 0;
    memset(s_mouse_until, 0, sizeof s_mouse_until);
}
