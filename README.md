# Ace Combat X - Recompilation

An unofficial Windows enhancement fork of [sal063/Ace-Combat-X-Recompilation](https://github.com/sal063/Ace-Combat-X-Recompilation), maintained here by **uzir1127**.

## Download and play

Get the **Windows ZIP asset** from [Releases](https://github.com/uzir1127/Ace-Combat-X-Recompilation/releases). The green Code button and GitHub's automatic “Source code” downloads are for development, not the ready-to-play package.

1. Extract the entire Windows ZIP.
2. Put your own **US Ace Combat X ISO (ULUS10176)** beside `Play.bat`.
3. Double-click **Play.bat**.

The ISO can have any filename. Keep one ISO beside the launcher, or drag the chosen ISO onto `Play.bat`. **No Python, LLVM, compiler, installer or separate image.bin is needed to play.** Use 64-bit Windows with a working Vulkan driver and Media Foundation. The ISO is read-only and is not included.

## What this fork adds

- Native 60 FPS gameplay with corrected physics, mission timers, weapon cadence and frame-counted systems.
- Original-cadence prerecorded movie playback, with optional 2x cinematic scaling and rotating-buffer frame handling.
- Included **OstrichMods HD Texture Pack v2.0.8**, with recomp-specific font matching/filtering for clearer briefings, dialogue and UI. Artwork credits remain with the pack's creators.
- Modern keyboard/mouse and controller defaults, plus F4 bindings for SDL gamepads, joysticks, HOTAS buttons, axes and hats. Separate devices can work together.
- Extended **F4 settings** and **F5 performance overlay** with FPS/frame time and CPU/GPU names and utilization.
- Direct ISO startup and a portable Windows package with matching source and license notices.

sal063's original project already provided the recompiler, native runtime/renderer, baseline F4 menu, controller input and right-stick camera. This fork extends those features.

## Controls

| Action | Keyboard / mouse | Standard controller positions |
|---|---|---|
| Pitch / roll | W/S and A/D; mouse movement | Left stick |
| Yaw | Q/E | LB/RB, L1/R1 or L/R |
| Brake / accelerate | Ctrl / Space | LT/RT, L2/R2 or ZL/ZR |
| Gun | J / left click | Bottom face button |
| Missile / selected special | L / right click | Right face button |
| Switch weapon | K / mouse wheel | Left face button |
| Change target | T / middle click | Top face button |
| Camera | Numpad 4/6/8/2; Alt+WASD; Alt+mouse | Right stick |
| View | V | Right stick click |
| Radar / map | M or Tab | View / Share / Create / minus |
| Pause | Escape | Menu / Options / plus |

Menus use WASD/arrows, Enter/Space to confirm and Escape/Backspace to go back. Controller bottom confirms and right goes back. F1 supplies Start/skip; Alt+F4 exits. **F4 > Input > Rebind flight controls** changes bindings. Unknown devices need manual setup and a compatible Windows/SDL driver. Throttle axes operate ACX's original brake/accelerate actions; pitch/roll/rudder retain proportional input.

## Updates and settings

Keep **memstick/**, **pspreco.toml** and **controls.ini** when updating. Saves and personal settings are not included in the downloadable ZIP. Pause a mission before opening F4. Keep normal pacing, 100% speed and frame skipping off for intended game speed.

## Validation and remaining work

This is a **release candidate**. Seven native timing/physics/input suites pass. The current binary completed a 30-second mission-1 section at the expected simulation/timer/weapon cadence. A fresh Windows ZIP was extracted and launched through its unmodified Play.bat with Python/LLVM/SDK paths removed; F4 and F5 were checked.

Full-campaign objective/debrief/save completion is **not yet certified**. Intermittent missing radio voices during the Gleipnir fight remain under investigation. Physical PlayStation/Switch/HOTAS and complete raw-mouse checks remain pending; virtual-device tests do not certify every driver. Performance depends on hardware. Cinematic scaling retains the original PSP video's available detail. Uncapped FPS is not part of this release.

See **VALIDATION.txt** in the Windows ZIP for the measured scope and earlier checks.

## Credits and source

Read [CREDITS.md](CREDITS.md) and [NOTICE.md](NOTICE.md) for source, artwork, reference and library attribution. Runtime licensing is in [LICENSE](LICENSE); each third-party component retains its own license. This is not affiliated with or endorsed by Bandai Namco or the original creators.

The Windows ZIP includes matching source at **licenses/source.zip**. A separate matching-source asset is also provided on the release page. It contains the generated C needed for that release's build; game-derived generated output remains ignored in the maintained repository. Developer instructions are in **SOURCE-BUILD.txt** inside the source asset and [README.txt](README.txt). Development requires Python/compiler/SDL/Vulkan development files; playing the Windows release does not.
