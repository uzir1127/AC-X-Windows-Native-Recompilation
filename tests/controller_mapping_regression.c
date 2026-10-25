#include "pspreco.h"
#include "qol_input.h"
#include "../runtime/host/present.h"
#include "../runtime/host/settings.h"
#include "../runtime/host/ui_overlay.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../runtime/host/input_bindings.h"
u8 *g_ram;
void present_test_poll(SDL_JoystickID id);
void present_test_keys(const bool *keys);
void present_test_focus(int focused);
void present_test_input_setup(void);
void present_test_input_shutdown(void);
void present_test_mouse(float dx,float dy,float dt);
void input_bindings_test_mouse(SDL_MouseButtonFlags buttons);
static bool keys[SDL_SCANCODE_COUNT];
static void sample(SDL_JoystickID id){SDL_UpdateJoysticks();present_test_poll(id);}
static int extended(SDL_JoystickID original) {
 int bad=0;u8 x,y,rx,ry;float tl,tr;
#define CHECK(expr) do{if(!(expr)){printf("FAIL line %d: %s\n",__LINE__,#expr);bad++;}}while(0)
#define CLEAR() do{memset(keys,0,sizeof keys);input_bindings_test_mouse(0);present_test_mouse(0,0,1.0f/60);sample(original);}while(0)
 mem_w32(g_ram,0x08a82ed8,0x08880000);mem_w8(g_ram,0x08a82e08,12);mem_w8(g_ram,0x08a80c45,1);
 CLEAR();
 keys[SDL_SCANCODE_A]=true;keys[SDL_SCANCODE_W]=true;sample(original);present_analog(&x,&y);CHECK(x==0&&y==0);CHECK(present_flight_buttons()==0);
 keys[SDL_SCANCODE_D]=true;keys[SDL_SCANCODE_S]=true;sample(original);present_analog(&x,&y);CHECK(x==128&&y==128);
 CLEAR();keys[SDL_SCANCODE_Q]=true;keys[SDL_SCANCODE_SPACE]=true;sample(original);present_triggers(&tl,&tr);
 CHECK(present_flight_buttons()==0x200&&present_rudder()==1&&tr==1&&tl==0);
 keys[SDL_SCANCODE_E]=true;keys[SDL_SCANCODE_LCTRL]=true;sample(original);CHECK(present_rudder()==0&&present_flight_buttons()==0x300);
 CLEAR();
 const int kk[]={SDL_SCANCODE_J,SDL_SCANCODE_L,SDL_SCANCODE_K,SDL_SCANCODE_T,SDL_SCANCODE_M,SDL_SCANCODE_ESCAPE};
 const u32 bb[]={0x4000,0x2000,0x8000,0x1000,1,8};
 for(int i=0;i<6;i++){keys[kk[i]]=true;sample(original);CHECK(present_flight_buttons()==bb[i]);keys[kk[i]]=false;sample(original);CHECK(!present_flight_buttons());}
 keys[SDL_SCANCODE_V]=true;sample(original);CHECK(present_aux()==PRESENT_AUX_R3);CLEAR();
 keys[SDL_SCANCODE_LALT]=keys[SDL_SCANCODE_W]=keys[SDL_SCANCODE_D]=true;sample(original);present_analog(&x,&y);present_analog2(&rx,&ry);
 CHECK(x==128&&y==128&&rx==255&&ry==0);CLEAR();
 keys[SDL_SCANCODE_SPACE]=true;sample(original);mem_w8(g_ram,0x08a80c45,0);sample(original);CHECK(!(present_flight_buttons()&0x4000));CLEAR();
 keys[SDL_SCANCODE_SPACE]=true;sample(original);CHECK(present_flight_buttons()==0x4000);CLEAR();
 keys[SDL_SCANCODE_W]=true;keys[SDL_SCANCODE_RETURN]=true;sample(original);present_analog(&x,&y);
 CHECK(x==128&&y==128&&present_flight_buttons()==(0x10|0x4000));CLEAR();
 keys[SDL_SCANCODE_ESCAPE]=true;sample(original);CHECK(present_flight_buttons()==8);CLEAR();
 keys[SDL_SCANCODE_BACKSPACE]=true;sample(original);CHECK(present_flight_buttons()==0x1000);CLEAR();
 SDL_Joystick*pausepad=SDL_GetJoystickFromID(original);SDL_SetJoystickVirtualButton(pausepad,SDL_GAMEPAD_BUTTON_EAST,true);sample(original);CHECK(present_flight_buttons()==0x1000);SDL_SetJoystickVirtualButton(pausepad,SDL_GAMEPAD_BUTTON_EAST,false);
 SDL_SetJoystickVirtualButton(pausepad,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,true);sample(original);CHECK(present_flight_buttons()==0x100&&present_rudder()==0&&!(present_aux()&PRESENT_AUX_LB));SDL_SetJoystickVirtualButton(pausepad,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,false);
 mem_w8(g_ram,0x08a82e08,10);keys[SDL_SCANCODE_ESCAPE]=true;sample(original);CHECK(present_buttons()==0x1000);CLEAR();
 keys[SDL_SCANCODE_RETURN]=true;sample(original);CHECK(present_buttons()==0x4000);CLEAR();
 mem_w8(g_ram,0x08a82e08,12);mem_w8(g_ram,0x08a80c45,1);
 mem_w8(g_ram,0x08a82e08,22);keys[SDL_SCANCODE_S]=true;sample(original);present_analog(&x,&y);CHECK(y==255);CLEAR();mem_w8(g_ram,0x08a82e08,12);
 input_binding_set(IB_KEYBOARD,IA_ROLL_L,(InputBinding){IB_KEY,SDL_SCANCODE_F,1});
 keys[SDL_SCANCODE_A]=true;sample(original);present_analog(&x,&y);CHECK(x==128);keys[SDL_SCANCODE_F]=true;sample(original);present_analog(&x,&y);CHECK(x==0);CLEAR();
 input_bindings_reset(IB_KEYBOARD);
 input_bindings_test_mouse(SDL_BUTTON_LMASK|SDL_BUTTON_RMASK|SDL_BUTTON_MMASK);sample(original);CHECK(present_flight_buttons()==0x7000);CLEAR();
 SDL_Event event;SDL_zero(event);event.type=SDL_EVENT_MOUSE_WHEEL;event.wheel.y=1;SDL_PushEvent(&event);sample(original);CHECK(present_flight_buttons()==0x8000);sample(original);CHECK(present_flight_buttons()==0x8000);SDL_Delay(60);sample(original);CHECK(!present_flight_buttons());
 SDL_zero(event);event.type=SDL_EVENT_MOUSE_BUTTON_DOWN;event.button.button=SDL_BUTTON_RIGHT;SDL_PushEvent(&event);event.type=SDL_EVENT_MOUSE_BUTTON_UP;SDL_PushEvent(&event);sample(original);CHECK(present_flight_buttons()==0x2000);sample(original);CHECK(present_flight_buttons()==0x2000);SDL_Delay(60);sample(original);CHECK(!present_flight_buttons());
 int positions[2];
 for(int rate=30;rate<=60;rate+=30){present_test_focus(0);sample(original);present_test_focus(1);present_test_mouse(300.0f/rate,-180.0f/rate,1.0f/rate);
  for(int i=0;i<rate/2;i++)sample(original);present_analog(&x,&y);positions[rate==60]=x;CHECK(x>160&&y<110);
 }
 CHECK(abs(positions[0]-positions[1])<=1);
 SDL_zero(event);event.type=SDL_EVENT_KEY_DOWN;event.key.key=SDLK_J;event.key.scancode=SDL_SCANCODE_J;SDL_PushEvent(&event);event.type=SDL_EVENT_KEY_UP;SDL_PushEvent(&event);sample(original);CHECK(present_flight_buttons()==0x4000);sample(original);CHECK(present_flight_buttons()==0x4000);SDL_Delay(60);sample(original);CHECK(!present_flight_buttons());
 present_test_mouse(0,0,1.0f/60);for(int i=0;i<60;i++)sample(original);present_analog(&x,&y);CHECK(x==128&&y==128);
 keys[SDL_SCANCODE_LALT]=true;present_test_mouse(8,-8,1.0f/60);sample(original);present_analog(&x,&y);present_analog2(&rx,&ry);CHECK(x==128&&y==128&&rx>128&&ry<128);CLEAR();
 keys[SDL_SCANCODE_Q]=keys[SDL_SCANCODE_J]=true;sample(original);present_test_focus(0);sample(original);present_analog(&x,&y);CHECK(!present_buttons()&&!present_aux()&&x==128&&y==128&&present_rudder()==0);present_test_focus(1);CLEAR();
 puts("Keyboard, pause/menu contexts, remap, mouse buttons/wheel/look, 30/60 Hz mouse response and focus release checked.");
 /* Different raw button/axis ordering, mapped through SDL just like PS and
  * Nintendo drivers. These are virtual devices, not physical-hardware QA. */
 const char* names[]={"Virtual PlayStation layout","Virtual Switch layout"};
 for(int type=0;type<2;type++){
  SDL_VirtualJoystickDesc d;SDL_INIT_INTERFACE(&d);d.type=SDL_JOYSTICK_TYPE_GAMEPAD;d.naxes=6;d.nbuttons=16;d.name=names[type];d.vendor_id=type?0x057e:0x054c;d.product_id=type?0x2009:0x0ce6;
  SDL_JoystickID id=SDL_AttachVirtualJoystick(&d);SDL_Joystick*j=SDL_OpenJoystick(id);CHECK(id&&j);
  char guid[33],mapping[768];SDL_GUIDToString(SDL_GetJoystickGUID(j),guid,sizeof guid);
  SDL_snprintf(mapping,sizeof mapping,"%s,%s,a:b2,b:b3,x:b0,y:b1,back:b4,start:b6,leftshoulder:b9,rightshoulder:b8,rightstick:b11,leftstick:b10,leftx:a3,lefty:a2,rightx:a1,righty:a0,lefttrigger:a5,righttrigger:a4,",guid,names[type]);
  CHECK(SDL_SetGamepadMapping(id,mapping));SDL_SetJoystickVirtualAxis(j,4,-32768);SDL_SetJoystickVirtualAxis(j,5,-32768);
  const int raw[]={2,3,0,1,4,6};const u32 bits[]={0x4000,0x2000,0x8000,0x1000,1,8};
  for(int i=0;i<6;i++){SDL_SetJoystickVirtualButton(j,raw[i],true);sample(id);CHECK(present_flight_buttons()==bits[i]);SDL_SetJoystickVirtualButton(j,raw[i],false);sample(id);}
  SDL_SetJoystickVirtualButton(j,9,true);SDL_SetJoystickVirtualAxis(j,4,32767);sample(id);CHECK(present_rudder()==1&&present_flight_buttons()==0x200);
  SDL_SetJoystickVirtualAxis(j,3,32767);SDL_SetJoystickVirtualAxis(j,2,-32768);sample(id);present_analog(&x,&y);CHECK(x==255&&y==0);
  SDL_CloseJoystick(j);SDL_DetachVirtualJoystick(id);sample(original);CHECK(!present_buttons()&&!present_aux());
 }
 puts("PlayStation- and Switch-style SDL mappings preserve Zero physical button positions.");
 SDL_VirtualJoystickDesc d;SDL_INIT_INTERFACE(&d);d.type=SDL_JOYSTICK_TYPE_FLIGHT_STICK;d.naxes=4;d.nbuttons=8;d.nhats=1;d.name="ACX unmapped HOTAS regression";d.vendor_id=0xfefe;d.product_id=0xac01;
 SDL_JoystickID id=SDL_AttachVirtualJoystick(&d);SDL_Joystick*j=SDL_OpenJoystick(id);CHECK(id&&j);CHECK(SDL_SetGamepadMapping(id,NULL));sample(id);CHECK(strstr(present_controller_name(),"HOTAS"));
 input_binding_set(IB_CONTROLLER,IA_ROLL_L,(InputBinding){IB_AXIS,0,-1});input_binding_set(IB_CONTROLLER,IA_ROLL_R,(InputBinding){IB_AXIS,0,1});
 input_binding_set(IB_CONTROLLER,IA_YAW_L,(InputBinding){IB_AXIS,2,-1});input_binding_set(IB_CONTROLLER,IA_YAW_R,(InputBinding){IB_AXIS,2,1});
 input_binding_set(IB_CONTROLLER,IA_ACCEL,(InputBinding){IB_AXIS,3,2});
 input_binding_set(IB_CONTROLLER,IA_GUN,(InputBinding){IB_BUTTON,0,1});input_binding_set(IB_CONTROLLER,IA_DPAD_UP,(InputBinding){IB_HAT,0,SDL_HAT_UP});
 SDL_SetJoystickVirtualAxis(j,2,-16384);SDL_SetJoystickVirtualAxis(j,3,-32768);sample(id);CHECK(fabsf(present_rudder()-.5f)<.001f&&!present_flight_buttons());
 SDL_SetJoystickVirtualAxis(j,3,32767);SDL_SetJoystickVirtualAxis(j,0,32767);SDL_SetJoystickVirtualButton(j,0,true);SDL_SetJoystickVirtualHat(j,0,SDL_HAT_UP);sample(id);present_analog(&x,&y);present_triggers(&tl,&tr);
 CHECK(x==255&&tr==1&&present_flight_buttons()==(0x4000|0x200|0x10));
 SDL_SetJoystickVirtualAxis(j,3,-32768);SDL_Joystick*original_joy=SDL_GetJoystickFromID(original);SDL_SetJoystickVirtualAxis(original_joy,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,32767);sample(id);
 CHECK((present_flight_buttons()&0x200)&&present_rudder()>.49f&&x==255);SDL_SetJoystickVirtualAxis(original_joy,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,-32768);sample(id);
 /* Save twice to check replacement of an existing settings file. */
 input_binding_set(IB_KEYBOARD,IA_VIEW,(InputBinding){IB_KEY,SDL_SCANCODE_F2,1});CHECK(input_bindings_save());
 input_binding_set(IB_MOUSE_DEVICE,IA_GUN,(InputBinding){IB_MOUSE,4,1});CHECK(input_bindings_save());input_bindings_reload();input_bindings_device(j);
 CHECK(input_binding_get(IB_CONTROLLER,IA_ACCEL).direction==2&&input_binding_get(IB_KEYBOARD,IA_VIEW).index==SDL_SCANCODE_F2&&input_binding_get(IB_MOUSE_DEVICE,IA_GUN).index==4);
 /* Actual virtual-joystick events drive capture, not a duplicated mapping. */
 input_bindings_capture(IB_CONTROLLER,IA_BRAKE);SDL_SetJoystickVirtualAxis(j,1,-32768);sample(id);CHECK(!input_bindings_capturing()&&input_binding_get(IB_CONTROLLER,IA_BRAKE).kind==IB_AXIS);
 input_bindings_capture(IB_KEYBOARD,IA_VIEW);SDL_zero(event);event.type=SDL_EVENT_KEY_DOWN;event.key.scancode=SDL_SCANCODE_G;event.key.key=SDLK_G;SDL_PushEvent(&event);sample(id);CHECK(input_binding_get(IB_KEYBOARD,IA_VIEW).index==SDL_SCANCODE_G);
 input_bindings_capture(IB_CONTROLLER,IA_VIEW);event.key.key=SDLK_ESCAPE;SDL_PushEvent(&event);sample(id);CHECK(!input_bindings_capturing());
 input_bindings_capture(IB_KEYBOARD,IA_VIEW);present_test_focus(0);sample(id);CHECK(!input_bindings_capturing());present_test_focus(1);
 input_bindings_capture(IB_CONTROLLER,IA_VIEW);SDL_SetJoystickVirtualButton(j,1,true);sample(id);CHECK(!input_bindings_capturing()&&input_binding_get(IB_CONTROLLER,IA_VIEW).index==1);
 SDL_CloseJoystick(j);SDL_DetachVirtualJoystick(id);sample(0);present_analog(&x,&y);CHECK(!present_buttons()&&!present_aux()&&x==128&&y==128&&present_rudder()==0);
 sample(original);CHECK(strstr(present_controller_name(),"local input"));
 input_bindings_reset(IB_KEYBOARD);input_bindings_reset(IB_MOUSE_DEVICE);input_bindings_save();
 puts("Unmapped HOTAS: half/full axes, proportional rudder, buttons/hat, capture, GUID profiles, atomic save/reload, disconnect and fallback checked.");
 return bad;
#undef CLEAR
#undef CHECK
}
int main(void){
 int bad=0,n=0;
 char profile[4096];SDL_snprintf(profile,sizeof profile,"%sinput-test-profile",SDL_GetBasePath());SDL_CreateDirectory(profile);char settings[4120];SDL_snprintf(settings,sizeof settings,"%s/pspreco.toml",profile);_putenv_s("PSPRECO_SETTINGS",settings);
 present_test_input_setup();present_test_keys(keys);present_test_focus(1);
 if(!SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD)){puts(SDL_GetError());return 2;}
 const char*db=getenv("ACX_TEST_CONTROLLER_DB");if(db){
  int loaded=SDL_AddGamepadMappingsFromFile(db),records=0,errors=0;char line[4096];FILE*f=fopen(db,"r");
  if(f){while(fgets(line,sizeof line,f)){if(line[0]=='#'||strlen(line)<33)continue;records++;if(SDL_AddGamepadMapping(line)<0)errors++;}fclose(f);}else errors++;
  printf("Bundled Windows mapping records: %d, newly added: %d, parse errors: %d\n",records,loaded,errors);if(loaded<0||records<800||errors)bad++;
 }
 SDL_JoystickID *physical=SDL_GetGamepads(&n);
 printf("Physical SDL gamepads detected before virtual test: %d\n",n);
 for(int i=0;i<n;i++)printf("  %s\n",SDL_GetGamepadNameForID(physical[i]));SDL_free(physical);
 SDL_VirtualJoystickDesc d;SDL_INIT_INTERFACE(&d);d.type=SDL_JOYSTICK_TYPE_GAMEPAD;
 d.naxes=SDL_GAMEPAD_AXIS_COUNT;d.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
 d.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;d.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;d.name="ACX local input regression";
 SDL_JoystickID id=SDL_AttachVirtualJoystick(&d);if(!id){puts(SDL_GetError());return 3;}
 SDL_Joystick*j=SDL_OpenJoystick(id);if(!j)return 4;
 gfx_settings_defaults();g_ram=calloc(1,PSP_RAM_SIZE);mem_w32(g_ram,0x0887211c,0x27bdffd0);qol_input_install(g_ram);
 mem_w32(g_ram,0x08a82ed8,0x08880000);mem_w8(g_ram,0x08a82e08,12);mem_w8(g_ram,0x08a80c45,1);
 input_bindings_reload();input_bindings_reset(IB_KEYBOARD);input_bindings_reset(IB_MOUSE_DEVICE);
 for(int m=0;m<16;m++){
  SDL_SetJoystickVirtualButton(j,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,(m&1)!=0);
  SDL_SetJoystickVirtualButton(j,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,(m&2)!=0);
  SDL_SetJoystickVirtualAxis(j,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,m&4?32767:-32768);
  SDL_SetJoystickVirtualAxis(j,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,m&8?32767:-32768);sample(id);
  u32 flight=(m&4?0x100:0)|(m&8?0x200:0),menu=((m&5)?0x100:0)|((m&10)?0x200:0);
  u32 aux=(m&1?PRESENT_AUX_LB:0)|(m&2?PRESENT_AUX_RB:0);
  if(present_flight_buttons()!=flight||present_buttons()!=menu||present_aux()!=aux){
   printf("FAIL shoulders %d flight=%x expected=%x menu=%x expected=%x aux=%x expected=%x\n",m,present_flight_buttons(),flight,present_buttons(),menu,present_aux(),aux);bad++;
  }
 }
 SDL_SetJoystickVirtualButton(j,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,false);SDL_SetJoystickVirtualButton(j,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,false);
 SDL_SetJoystickVirtualAxis(j,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,-32768);SDL_SetJoystickVirtualAxis(j,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,-32768);
 int buttons[]={SDL_GAMEPAD_BUTTON_SOUTH,SDL_GAMEPAD_BUTTON_EAST,SDL_GAMEPAD_BUTTON_WEST,SDL_GAMEPAD_BUTTON_NORTH,SDL_GAMEPAD_BUTTON_START,SDL_GAMEPAD_BUTTON_BACK};
 u32 expected[]={0x4000,0x2000,0x8000,0x1000,8,1};
 for(int i=0;i<6;i++){
  SDL_SetJoystickVirtualButton(j,buttons[i],true);sample(id);
  if(present_flight_buttons()!=expected[i]||present_buttons()!=expected[i])bad++;
  SDL_SetJoystickVirtualButton(j,buttons[i],false);sample(id);if(present_buttons()!=0)bad++;
 }
 SDL_SetJoystickVirtualAxis(j,SDL_GAMEPAD_AXIS_LEFTX,32767);SDL_SetJoystickVirtualAxis(j,SDL_GAMEPAD_AXIS_LEFTY,-32768);
 SDL_SetJoystickVirtualAxis(j,SDL_GAMEPAD_AXIS_RIGHTX,-32768);SDL_SetJoystickVirtualAxis(j,SDL_GAMEPAD_AXIS_RIGHTY,32767);sample(id);
 u8 x,y,rx,ry;present_analog(&x,&y);present_analog2(&rx,&ry);
 if(x!=255||y!=0||rx!=0||ry!=255)bad++;
 SDL_SetJoystickVirtualAxis(j,SDL_GAMEPAD_AXIS_LEFTX,0);SDL_SetJoystickVirtualAxis(j,SDL_GAMEPAD_AXIS_LEFTY,0);SDL_SetJoystickVirtualAxis(j,SDL_GAMEPAD_AXIS_RIGHTX,0);SDL_SetJoystickVirtualAxis(j,SDL_GAMEPAD_AXIS_RIGHTY,0);sample(id);
 present_analog(&x,&y);present_analog2(&rx,&ry);if(x!=128||y!=128||rx!=128||ry!=128)bad++;
 SDL_SetJoystickVirtualButton(j,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,true);sample(id);g_gfx.modern_controls=0;gfx_settings_touch();sample(id);if(present_flight_buttons()!=0x100)bad++;
 g_gfx.modern_controls=1;gfx_settings_touch();sample(id);if(present_flight_buttons()!=0||!(present_aux()&PRESENT_AUX_LB))bad++;
 SDL_SetJoystickVirtualButton(j,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,false);sample(id);
 bad+=extended(id);
 SDL_CloseJoystick(j);SDL_DetachVirtualJoystick(id);sample(0);if(present_buttons()!=0||present_aux()!=0)bad++;
 SDL_Event key;SDL_zero(key);key.type=SDL_EVENT_KEY_DOWN;key.key.key=SDLK_F5;
 g_gfx.show_fps=0;SDL_PushEvent(&key);sample(0);
 if(g_gfx.show_fps!=1||plane_ui_visible()||ui_visible())bad++;
 key.key.repeat=true;SDL_PushEvent(&key);sample(0);if(g_gfx.show_fps!=1)bad++;
 key.key.repeat=false;SDL_PushEvent(&key);sample(0);
 if(g_gfx.show_fps!=0||plane_ui_visible())bad++;
 key.key.key=SDLK_F6;SDL_PushEvent(&key);sample(0);
 if(!plane_ui_visible()||g_gfx.show_fps!=0)bad++;
 SDL_PushEvent(&key);sample(0);if(plane_ui_visible())bad++;
 puts("F5 readout toggle/repeat suppression and F6 editor routing exercised through SDL events.");
 puts("Exercised actual SDL polling: 16 shoulder combinations, A/B/X/Y, Menu/View, both sticks, dead zones, layout toggle, and disconnect release.");
 printf("SDL controller failures: %d\n",bad);present_test_input_shutdown();SDL_Quit();free(g_ram);return bad?1:0;
}
