#include "pspreco.h"
#include "../runtime/host/present.h"
#include "../runtime/host/settings.h"
#include "../runtime/host/ui_overlay.h"
#include "rumble.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

u8 *g_ram;
void present_test_poll(SDL_JoystickID id);
void present_test_focus(int focused);
void present_test_input_shutdown(void);
typedef struct { unsigned calls, low, high; int fail; } Motors;
static int bad;
static int overlay;
int rumble_test_ui_visible(void) { return overlay; }
#define CHECK(x) do { if (!(x)) { printf("FAIL %d: %s\n",__LINE__,#x);bad++; } } while(0)
static bool SDLCALL motors(void *user, Uint16 l, Uint16 h) {
    Motors *m=user;m->calls++;if(m->fail)return false;m->low=l;m->high=h;return true;
}
static SDL_JoystickID attach(Motors *m, int raw, int supports) {
    SDL_VirtualJoystickDesc d;SDL_INIT_INTERFACE(&d);
    d.type=raw?SDL_JOYSTICK_TYPE_FLIGHT_STICK:SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes=6;d.nbuttons=16;d.name=raw?"ACX virtual raw rumble":"ACX virtual mapped rumble";
    d.userdata=m;d.Rumble=supports?motors:NULL;
    return SDL_AttachVirtualJoystick(&d);
}
static void sample(SDL_JoystickID id) { rumble_observe_flight(g_ram,0x08880000,0x08881000);SDL_UpdateJoysticks();present_test_poll(id); }
static void hurt(SDL_JoystickID id) {
    mem_w16(g_ram,0x088800b0,325);sample(id);SDL_Delay(25);
    mem_w16(g_ram,0x088800b0,300);sample(id);
}
int main(void) {
    char path[4096];SDL_snprintf(path,sizeof path,"%srumble-test-settings.toml",SDL_GetBasePath());
    _putenv_s("PSPRECO_SETTINGS",path);
    if(!SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD))return 2;
    gfx_settings_defaults();present_test_focus(1);g_ram=calloc(1,PSP_RAM_SIZE);
    if(!g_ram)return 3;
    mem_w32(g_ram,0x08a82ed8,0x08880000);mem_w8(g_ram,0x08a82e08,12);mem_w32(g_ram,0x08b7f1b0,0);mem_w16(g_ram,0x088800b0,325);
    mem_wf32(g_ram,0x088810a4,800.0f/43.2f);
    Motors a={0},b={0},c={0};
    SDL_JoystickID first=attach(&a,0,1);sample(first);CHECK(first&&present_rumble_supported());
    CHECK(!a.low&&!a.high); /* SDL may coalesce the initial zero command. */
    mem_wf32(g_ram,0x088810a4,700.0f/43.2f);sample(first);CHECK(a.calls>0&&a.low==36699); /* first effect, no F4 test */
    mem_wf32(g_ram,0x088810a4,350.0f/43.2f);SDL_Delay(25);sample(first);CHECK(a.low==45874);
    mem_wf32(g_ram,0x088810a4,800.0f/43.2f);sample(first);CHECK(!a.low&&!a.high);
    hurt(first);CHECK(a.low>0&&a.high>0);
    mem_w32(g_ram,0x08b7f1b0,1);sample(first);CHECK(!a.low&&!a.high); /* pause */
    mem_w8(g_ram,0x08a80c45,1);mem_w32(g_ram,0x08b7f1b0,0);sample(first);CHECK(!a.low&&!a.high); /* native Resume, no stale replay */
    hurt(first);present_test_focus(0);sample(first);CHECK(!a.low&&!a.high);
    present_test_focus(1);sample(first);CHECK(!a.low&&!a.high);
    hurt(first);g_gfx.rumble=0;sample(first);CHECK(!a.low&&!a.high);
    g_gfx.rumble=1;sample(first);CHECK(!a.low&&!a.high);
    hurt(first);g_gfx.rumble_strength=0;sample(first);CHECK(!a.low&&!a.high);g_gfx.rumble_strength=70;
    hurt(first);overlay=1;sample(first);CHECK(!a.low&&!a.high);
    present_rumble_test();CHECK(a.low==45874&&a.high==45874);
    g_gfx.rumble_strength=100;present_rumble_test();CHECK(a.low==65535&&a.high==65535);g_gfx.rumble_strength=70;
    SDL_Delay(620);sample(first);CHECK(!a.low&&!a.high);
    present_rumble_test();g_gfx.rumble=0;sample(first);CHECK(!a.low&&!a.high);g_gfx.rumble=1;
    present_rumble_test();present_test_focus(0);sample(first);CHECK(!a.low&&!a.high);present_test_focus(1);
    overlay=0;sample(first);
    hurt(first);SDL_JoystickID second=attach(&b,1,1);sample(second);
    CHECK(second&&present_rumble_supported()&&!a.low&&!a.high&&!b.low&&!b.high);
    hurt(second);CHECK(b.low>0&&b.high>0); /* unmapped joystick uses same SDL rumble path */
    mem_w8(g_ram,0x08a82e08,21);sample(second);CHECK(!b.low&&!b.high);mem_w8(g_ram,0x08a82e08,12);
    hurt(second);SDL_Delay(670);sample(second);CHECK(!b.low&&!b.high);
    mem_wf32(g_ram,0x088810a4,4);mem_wf32(g_ram,0x088810ac,5);SDL_Delay(25);sample(second);
    CHECK(b.low>=35000&&b.high>=33600);
    SDL_Delay(700);sample(second);CHECK(b.low>=35000&&b.high>=33600); /* sustained, no one-shot fade */
    mem_w32(g_ram,0x08b7f1b0,1);sample(second);CHECK(!b.low&&!b.high);mem_w32(g_ram,0x08b7f1b0,0);
    sample(second);CHECK(b.low>=35000);present_test_focus(0);sample(second);CHECK(!b.low&&!b.high);present_test_focus(1);
    mem_wf32(g_ram,0x088810a4,800.0f/43.2f);sample(second);CHECK(!b.low&&!b.high);
    b.fail=1;hurt(second);CHECK(!present_rumble_supported());
    unsigned calls=b.calls;hurt(second);CHECK(b.calls==calls); /* no per-frame retry flood */
    SDL_Delay(1050);b.fail=0;hurt(second);CHECK(present_rumble_supported()&&b.low>0&&b.high>0); /* recovery without test/reconnect */
    SDL_JoystickID third=attach(&c,1,0);sample(third);CHECK(!present_rumble_supported());
    hurt(third);CHECK(c.calls==0);
    sample(first);hurt(first);CHECK(a.low>0);
    SDL_DetachVirtualJoystick(first);sample(second);CHECK(!b.low&&!b.high);
    b.fail=0;SDL_DetachVirtualJoystick(second);SDL_DetachVirtualJoystick(third);
    Motors reconnected={0};SDL_JoystickID again=attach(&reconnected,0,1);sample(again);
    CHECK(present_rumble_supported());hurt(again);CHECK(reconnected.low>0);
    g_gfx.rumble=0;g_gfx.rumble_strength=42;CHECK(gfx_settings_save());
    gfx_settings_defaults();CHECK(gfx_settings_load());CHECK(!g_gfx.rumble&&g_gfx.rumble_strength==42);
    g_gfx.rumble=1;g_gfx.rumble_strength=70;hurt(again);
    present_test_input_shutdown();CHECK(!reconnected.low&&!reconnected.high);
    SDL_DetachVirtualJoystick(again);SDL_Quit();free(g_ram);
    printf("SDL rumble lifecycle/settings failures: %d\n",bad);return bad?1:0;
}
