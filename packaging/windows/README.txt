ACE COMBAT X - RECOMPILATION
60 FPS release candidate - Windows x64

PLAY
1. Extract this entire ZIP to a folder.
2. Put your own US Ace Combat X ISO (ULUS10176) inside the main
   "Ace Combat X - Recompilation" folder.
3. Open Play.bat. Press F4 for settings.
Press F5 to show/hide FPS, frame time, detected CPU/GPU names and utilization.
During a mission, pause with Menu before opening F4.

The ISO may have any filename. Keep one ISO inside the main folder, or drag
the chosen ISO onto Play.bat. No installer, Python, compiler, extraction tools,
or separate image.bin are needed to play. CSO/PBP and other regions are not
supported. The ISO is opened read-only.

FEATURES
- Corrected 60 FPS gameplay, with original game speed and timer behavior.
- Opening movie playback keeps its original 29.97 FPS cadence.
- OstrichMods HD Texture Pack v2.0.8 included and enabled.
- Corrected recomp font-page matching and font-only filtering improve briefing,
  dialogue and UI text clarity using the included HD artwork.
- Font matching follows bitmap/palette identity across mission memory layouts.
- Cinematic clarity upscales prerecorded video during playback from your ISO.
  Movies keep their original detail and cadence; no movie files are bundled.
- Zero-style keyboard/mouse, Xbox/PlayStation/Switch controls and right-stick camera.
- F4 flight bindings for keyboards, mice, gamepads and SDL joysticks/HOTAS.
- Controller vibration for actual gun shots, missile/special launches and damage.
- F4 settings saved on menu close or game exit; Save now confirms the result.
- Rain clarity replaces the native rain tile with finer tapered streaks;
  cockpit view adds subtle sliding water droplets and trails while raining.

F4 > Renderer > Rain clarity and cockpit water toggles the weather enhancement.
It follows the original game's rain animation wherever that rain texture is
used. Glass droplets appear only in cockpit view during rain; external views,
dry weather, menus and movies do not receive the glass overlay. This is a
lightweight visual treatment, without simulated water refraction.

CONTROLS
Xbox / standard SDL gamepad defaults:
Left stick: pitch / roll        Right stick: camera
LB / RB: yaw left / right       LT / RT: brake / accelerate
Menu: pause                     View: radar / map
A: gun   B: missile / special    X: switch weapon   Y: change target
R3: change view
Weapon controls keep the game's original mapping. F4 > Input changes the
modern shoulder layout, camera sensitivity, and camera inversion.

Vibration is enabled at 70% by default. F4 > Input has Controller vibration,
Vibration strength (0-100%) and Test vibration. At 100% the 600 ms test uses
full power on both motors. Weapon/impact pulses hold their initial strength
before fading so the motors have time to respond.
Gun fire produces short pulses, launches a heavier kick, and damage a stronger
impact. These are new console-style effects, not copied PS2 game rumble data.
Low-speed buffet starts at 80% at 700 km/h and rises smoothly to 100% at
350 km/h or below, rather than waiting for a mechanical stall. It stops above
700 km/h. The F4 strength slider scales the effect. Controller feedback
initializes automatically and retries transient output failures once per
second during flight; no manual test pulse is required by the gameplay path.
Only the selected device vibrates. Gameplay vibration stops while paused,
in F4/F6, or when the game loses focus. Effects expire in real time at both
30 and 60 FPS. Support depends on the controller, connection and SDL driver;
unsupported devices continue to work normally without vibration. This does
not implement force-feedback steering or adaptive trigger resistance.

The same physical layout applies to PlayStation and Nintendo controllers:
L1/R1 or L/R yaw; L2/R2 or ZL/ZR brake/accelerate.
Bottom face button: gun; right: missile; left: weapon; top: target.
Options/+ pauses; Create/Share/- changes radar; right stick click changes view.
In menus the bottom face button confirms and the right face button goes back.
The original ACX actions are retained; Zero-only actions are not added.

Keyboard flight defaults (Zero-style):
W/S: pitch down/up       A/D: roll left/right       Q/E: yaw left/right
Ctrl: brake             Space: accelerate
J: gun   L: missile/special   K: weapon   T: target
V: change view          M or Tab: radar            Esc: pause/resume
Numpad 4/6/8/2: camera; laptops can hold Alt + WASD instead.
Menus: arrows/WASD navigate, Enter/Space confirm, Esc/Backspace back,
Q/E change tabs. F1 is an always-available Start/skip key. Alt+F4 exits.
F4 > Input can restore the previous keyboard preset.

Mouse: move to pitch/roll; hold Alt to look around. Left click gun,
right click missile, middle click target, wheel weapon. F4 releases the
pointer. Mouse flight can be disabled independently in F4 > Input.

F4 > Input > Rebind flight controls edits keyboard, mouse or device bindings.
For an unfamiliar controller, joystick, HOTAS or pedals, select it in
Input device, choose Controller / joystick under Rebind device, then bind
its buttons, axes and hats. Move an axis in the desired direction to bind
that half; choose Full range / Full reversed for a throttle lever.
Pitch, roll and rudder axes are proportional. Throttle axes activate the
original ACX brake/accelerate actions rather than setting an absolute speed.
Separate connected stick/throttle/pedal devices can supply actions together.
Keyboard/menu navigation remains available while setting up a device.
Flight bindings save to controls.ini when F4 closes or with Save now.
Reset buttons restore the defaults for the chosen source/device.

SDL provides built-in device support plus the included Windows controller
mapping database. Unmapped devices require F4 binding setup. A device must
be exposed to Windows/SDL as a keyboard, mouse, gamepad or joystick; this
does not add drivers for arbitrary proprietary hardware. Avoid running both
native input and a third-party virtual-controller remapper for one device.

SETTINGS
F4 > Output: fullscreen, scaling, 30/60 FPS (restart), performance overlay (F5).
CPU utilization is total system load. GPU utilization comes from NVIDIA's
driver when supported, otherwise Windows' busiest-engine counters, matched
to the GPU rendering the game. It includes other apps using that GPU. Readings
update once per second while shown; unavailable counters display N/A.
The F5 visibility choice is saved when the game closes normally.
F4 > Renderer: resolution, Cinematic clarity, filtering, and HD Replacement textures.
F4 > Colour / Input: display colour and controller options.
Advanced settings exposes engine/debug options and speed overrides.
For correct real-time game speed, keep speed at 100% and normal pacing on.
Use the High preset (2x) first; lower resolution if your GPU needs headroom.
Quality presets keep frame skipping off. Advanced settings can change it.

SAVES AND UPDATES
Your saves are in memstick; F4 settings are in pspreco.toml.
Custom flight bindings are in controls.ini once you edit them.
Keep these when updating. The supplied ZIP contains no personal saves or ISO.
Share the supplied ZIP; a folder you have played in can contain your saves.

REQUIREMENTS / HELP
64-bit Windows, working Vulkan graphics drivers, and Media Foundation.
Windows N editions may need the Media Feature Pack for movies.
If Play reports missing files, extract the complete ZIP again.
If startup fails, Play keeps the error visible: see logs/last-run.log.
If multiple ISOs are found, keep one or drag the chosen ISO onto Play.bat.

CREDITS AND SOURCE
Complete project, artwork, reference and library credits: CREDITS.txt.
Base project: https://github.com/sal063/Ace-Combat-X-Recompilation
HD textures: OstrichMods - https://ostrichmods.com/acx.html
Full texture credits: textures/ULUS10176/CREDITS.txt.
Runtime license: LICENSE.txt. Third-party notices: NOTICE.md and licenses/.
Matching build sources are included in licenses/source.zip.
Controller mappings: https://github.com/mdqinc/SDL_GameControllerDB
Default action-layout reference: the Zero PC dinputSample.ini published at
https://github.com/Foxvic/ACZeroPCremap (functional mapping reference only).
See VALIDATION.txt for the exact testing scope and remaining release gates.
