Ace Combat X: Skies of Deception -- static recompilation
==========================================================

pspreco takes the PSP game's MIPS code and recompiles it, ahead of time, into
native C. The PSP's kernel calls (sceIo, sceKernel, sceGe, ...) are
reimplemented on the host in runtime/hle, and runtime/host wires that up to a
real window, GPU and audio backend. There is no MIPS interpreter and no JIT at
runtime -- every recompiled function is plain C, compiled once by a normal C
compiler.

This is not an emulator and it is not a copy of the game. It is a compiler
pipeline (recomp/, tools/) plus a small OS-reimplementation runtime
(runtime/). Nothing under PSP_GAME/, no ISO, no CDI archive, and no save data
ships here -- you provide those yourself from a disc you own, the same way you
would for any other PSP homebrew or recompilation project.

This repository does not include:
  - The game's disc image, extracted files, or any asset archive.


Portable Windows download
-------------------------
End users need a prebuilt portable Windows package: extract it, drop an original
US ULUS10176 ISO beside Play.bat, then open Play. F4 opens Settings, including
physics-aware 60 FPS gameplay (restart to switch to/from 30 FPS), graphics, and
modern Xbox controls. No Python, compiler, extracted disc, or image.bin is needed.
F5 toggles FPS/frame time and detected CPU/GPU names with utilization percentages.
The portable texture pack includes exact recomp PGF-page aliases and targeted
font filtering for clearer small text in briefings, dialogue and menus.
Font aliases match the bitmap and palette independently of its memory address.
F4 > Renderer > Cinematic clarity enables a 2x cubic upscale of decoded movie
frames, including their rotating DMA display copies. It is enabled by default.
Only live MPEG surfaces qualify; context release/ordinary overwrites remove the
tag. It changes no video timestamps, audio clocks or gameplay timing. Movies
retain the detail present in the original ISO; this is a playback filter.
The ISO is read-only. Saves/settings stay in the portable folder.

The GitHub source-only Download ZIP is for developers and cannot run without a
compiled executable. A maintainer builds the SDL3 runtime, then makes the
ready-to-run download with:

    python tools/package_windows.py --out "dist/Ace Combat X - Portable Starter" \
        --sdl path/to/SDL3.dll --sdl-license path/to/SDL3/LICENSE.txt --zip

The packager uses a fixed file allowlist. It never includes the ISO, extracted
image, local saves/settings, mods, debug captures, or optional HD texture packs unless explicitly supplied with
--texture-pack path/to/ULUS10176.
It refuses to overwrite an existing installation. Publish the portable ZIP
alongside the corresponding source/build instructions; this command does not
upload anything or create a GitHub release.

Native startup also accepts `pspreco-sdl3 --iso path/to/game.iso`; omit the path
to auto-detect one local ISO. `--check-iso` validates without opening a window.
It verifies BOOT.BIN's SHA-256, checks the original archive extent, and applies
PRX relocations in RAM using the same semantics as tools/prxload. Missing,
ambiguous, truncated, or unsupported executable/layout checks fail before
gameplay. The developer
`image.bin ISO` launch remains available.

Layout
------
recomp/          the static recompiler: ELF/PRX analysis, function-boundary
                  analysis, and the C-emitter
runtime/cpu/      MIPS/VFPU interpreter core used by traps and cold paths
runtime/gpu/      the PSP's GE (graphics engine) reimplemented against Vulkan
runtime/hle/      high-level reimplementations of the PSP's kernel modules
runtime/host/     window, audio, input, video-decode, and the F4 debug overlay
tools/            standalone utilities (ISO/CDI extraction, ELF/PRX loading,
                  disassembly, VFPU tables, trace comparison)
third_party/      Dear ImGui (MIT), a standalone ATRAC3/ATRAC3+ decoder lifted
                  from FFmpeg/PPSSPP, the PSP system fonts PPSSPP ships as test
                  assets (needed to draw any text at all), and the Basis
                  Universal transcoder + zstd for .ktx2 texture packs
data/             the syscall NID database and VFPU instruction table the
                  recompiler and disassembler read as input
tests/            regression checks: a boot trace, and a decompression
                  self-check against the disc's mission archives
build.py          compiles runtime/ + generated/ into the pspreco executable

generated/ (recompiled C output) is not checked in -- it's produced by
recomp/analyze/analyze.py and recomp/emit/emit_c.py, see "Building the
recompiled game" below.


Requirements
------------
- Python 3.10+
- clang (or another C11/C++14 compiler on your PATH)
- Windows + SDL3 + a Vulkan loader for the windowed build; a headless build
  needs neither


Extracting your own copy of the game
-------------------------------------
Everything below assumes you own "Ace Combat X: Skies of Deception" (US,
ULUS10176) and have dumped your own UMD to an ISO. This project does not
provide a dumping tool -- use whatever you already use to image your own PSP
media.

1. List and pull files out of the ISO (ISO9660, no Joliet/UDF):

       python tools/isoextract.py "Ace Combat X.iso" game/

   This writes the disc's files under game/, plus game/disc.manifest. The
   game addresses its data by absolute disc sector rather than by file name,
   so the manifest -- which records each file's original LBA -- is what lets
   the runtime serve reads out of a plain directory instead of the ISO.

2. Almost all of the game's data lives in one archive,
   PSP_GAME/USRDIR/REGFILE.CDI. Unpack it into individually named files:

       python tools/cdiextract.py game

   This writes game/cdi/<real names> plus game/cdi/cdi.manifest, covering
   every byte of the archive including the streamed audio and movies that
   aren't in its own directory table.

3. If you ever need to change an asset's size rather than just its bytes
   (repacking, not something this project does for you), acxreindex.py
   rewrites the archive's index and both manifests together so the three
   stay consistent:

       python tools/acxreindex.py apply game

The runtime prefers an extracted game/ directory over a raw ISO; point it at
whichever you have (see "Running" below).


Building the recompiled game
-------------------------------
The recompiler works from EBOOT.elf, the game's main executable. EBOOT.BIN on
the disc is encrypted; decrypt your own copy to ELF with whatever PSP devkit
tooling you already use for that (out of scope here) and place it at the
repository root as EBOOT.elf.

1. Regenerate the VFPU decode tables (checked into generated/, but this is
   how they were produced, and it's how you'd rebuild them if data/vfpu_table.json
   ever changes):

       python tools/pspdisasm/gen_vfpu_decode.py

2. Run the analysis pass, which finds function boundaries and reports how
   much of the executable's code the recompiler accounts for:

       python recomp/analyze/analyze.py EBOOT.elf

3. Emit the recompiled C (also already checked into generated/, so this step
   is only needed if you're re-recompiling from a different dump or a patched
   EBOOT):

       python recomp/emit/emit_c.py EBOOT.elf

4. Produce the program's initial-data image, which the runtime loads into
   guest RAM at startup (this is the one file this repo deliberately does not
   ship, since it's a byte-for-byte copy of the game's own data section):

       python tools/prxload/prxload.py EBOOT.elf --emit-image generated/image.bin

4a. The mod-enabled runtime also needs metadata omitted from the upstream tree.
    Recover it from the original symbol-bearing local release executable after
    emit_c.py, which empties generated/:

        python tools/recover_mod_metadata.py --exe ORIGINAL/pspreco-sdl3.exe --out generated

5. Compile:

       python build.py               # headless, software-rasterised
       python build.py --sdl3        # windowed, SDL3 + Vulkan

   build/pspreco.exe (or build/pspreco-sdl3.exe) is the result.


Running
-------
    build/pspreco-sdl3.exe generated/image.bin game

The second argument is either a game.iso file or an extracted game/
directory (see "Extracting your own copy of the game" above); it defaults to
game.iso in the current directory if omitted. PSPRECO_ISO also works as an
environment variable if you'd rather not pass it on the command line.


Native 60 FPS
-------------
The portable Windows build defaults to corrected 60 FPS. F4 can select
original 30 FPS after a restart. PSPRECO_FPS=60 explicitly enables native
updates and rendering at PSP refresh
(about 59.94 FPS). Keep throttle on, speed_percent=100, and frameskip=0.

This retains the kernel/display/audio clock, changes the engine's divide-by-two
frame gate, reuses the vblank wait already completed by the main render path,
and computes the shared simulation delta from guest vblanks. Delta, rounded
delta, and reciprocal are updated together so countdowns and physics agree.
The original maximum-delta clamp is retained. Encoded cinematic playback keeps
its original 29.97 FPS main-buffer cadence while any MPEG context is active;
otherwise alternating movie buffers can replay older images. The last context
cleanup restores 60 FPS automatically. Timer and divider phase are reset at
both transitions to avoid a time jump. Unsupported images or unavailable
hooks leave 60 FPS disabled. PSPRECO_FPS=30 keeps the original timing path.

Original frame-counted mission scripts, NPC simulation, the player gun, and
button-hold/ramp handlers retain a fixed 30 Hz step inside the 60 Hz frame loop.
NPC init, rendering and damage messages still run immediately. Legacy steps use
the original two-tick shared delta and 30 FPS normalization locally, restore
both afterwards, and carry odd
vblanks forward. Missile/special rack reloads carry the integer half ticks that
would otherwise truncate to zero at 60 Hz. BankTurn retains the original sink
velocity normalization. The shared angular rate limiter retains its original
30 Hz acceleration/damping response, resolving velocity at the beginning of
each legacy interval while its callers integrate movement on both 60 Hz halves.

Tests: tests/framerate_regression.c exercises the actual timing module with a
controlled guest clock, including fixed legacy steps, rack tick carry, draw and
non-update messages, stall catch-up, timer/FPS restoration and movie transitions.
tests/speed_integrator_regression.c compares captured flight integration at
30 x 2 ticks and 60 x 1 tick. tests/gravity_regression.c exercises the original
movement and BankTurn functions for low-speed sink and bank cases. The tests
contain no game snapshots; provide locally captured inputs yourself.
tests/axis_regression.c links the actual native hooks and original pitch/yaw/roll
functions, with a controlled guest clock, across 36 captured-state cases.
After an SDL3 moddable build, run all checks on Windows with LLVM-MinGW:

    python tests/build_native60_checks.py --cc <clang.exe> --sdk <SDL-Vulkan-prefix> --cpu <local-cpu.bin> --ram <local-32MB-ram.bin>

The capture is an 800-byte CpuCtx at entry to Player_SpeedIntegrator plus
guest RAM 0x08000000..0x0a000000. These private game-data captures are not
distributed. Results are written under build/native60-checks/.

The mission/gun/cooldown frame-count paths were cross-checked against the MIPS
locations documented by Nukem9 (https://github.com/Nukem9/acex-60fps-patch).
This implementation uses native C hooks rather than injecting that CWCheat patch.



Modern input controls
--------------------
Modern shoulder controls are enabled by default in this build:
LB/RB yaw left/right; LT/RT brake/accelerate. Left stick pitch/roll, right
stick camera, Menu pause and View radar/map retain their mappings. A/B/X/Y
weapon/target controls are unchanged; R3 still changes view.

F4 > Input > Modern shoulder controls toggles back to the original PSP
shoulder mapping. The setting persists as [input] modern_controls = true/false.
It replaces the former analog_yaw setting, which had no implementation.
Menus keep PSP shoulder navigation on both bumpers and triggers.

Bumpers supply the original normalized player rudder axis. The original
YawAxis function retains aircraft authority, damping and frame timing;
its input is restored after each call. Opposed bumpers cancel, release
returns to the original neutral response, NPCs are unaffected, and yaw
can be used while braking/accelerating. If the verified hook cannot be
installed, controller mapping falls back to PSP shoulders.

tests/controls_regression.c checks mapping, input restoration and actual
original yaw physics at 30/60 FPS. tests/controller_mapping_regression.c
uses an in-process SDL virtual controller to exercise production polling,
including both shoulder pairs, face buttons, Menu/View, sticks, dead zones,
the layout toggle and disconnect release. Its polling seam is compiled
only with PSPRECO_INPUT_TEST; normal builds have no virtual-controller code.
Both checks run with tests/build_native60_checks.py alongside the 60 FPS tests.

The Zero-style modern keyboard preset adds real analog pitch/roll on WASD,
Q/E yaw, Ctrl/Space brake/accelerate, J/L/K/T gun/missile/weapon/target,
V view, M radar and Esc pause. Menu keys are separate, including Enter/Space
confirm. F1 always supplies Start/skip. F4 can restore the legacy preset.
Mouse flight uses relative movement normalized by elapsed host time, with a
40 ms smoothing time constant; Alt changes to camera look. Buttons/wheel
match the default Zero-style actions. Mouse flight is independently optional.
Functional reference: Foxvic/ACZeroPCremap's published dinputSample.ini.

runtime/host/input_bindings.c handles rebindable keyboard/mouse actions and
per-GUID SDL joystick profiles. Standard gamepads retain semantic SDL
position mappings. Raw joystick/HOTAS buttons, axes and hats can be captured
in F4 > Input; full-range/reversed axes are selectable for throttle levers.
Centered pitch/roll/rudder are proportional. ACX throttle remains its original
digital brake/accelerate behavior, even when driven by a lever. Connected
USB stick/throttle/pedal devices can contribute actions simultaneously.
Bindings persist atomically to controls.ini beside pspreco.toml. Keyboard
menus remain usable with an unmapped device. Focus loss and F4/F6 clear game
input; controller removal releases held input and selects an available device.
Unsupported hardware still needs a Windows/SDL driver or mapping.

packaging/windows/gamecontrollerdb.txt is a Windows-only subset of
https://github.com/mdqinc/SDL_GameControllerDB with its original mapping lines
and license retained. Place it beside the executable; an explicit environment
SDL_GAMECONTROLLERCONFIG_FILE overrides it. The portable packager includes it.
Input tests cover SDL mapping parsing, keyboard/menu/pause/aux contexts,
30/60 Hz mouse response, remapping, profiles/save replacement, two different
gamepad raw layouts, unmapped HOTAS axes/buttons/hats, multiple-device mixing,
disconnect/focus release and original 30/60 FPS rudder at fractional strength.
These virtual-device tests do not certify every physical controller/driver.

Replacement textures
---------------------
The Vulkan backend reads PPSSPP-format texture packs, so packs published for
PPSSPP work here unmodified. Put the pack's contents -- textures.ini and its
subdirectories, or a textures.zip -- in:

    textures/ULUS10176/

next to the executable, or point PSPRECO_TEXTURES at the directory instead.
It is on by default; the F4 overlay has a switch under Renderer that also
shows which pack was picked up and how many entries it has.

Supported: png (any colour type and bit depth), dds (DXT1/3/5 and
uncompressed 32-bit), and ktx2 (UASTC or ETC1S, with or without Zstandard
supercompression), loose in a directory or inside textures.zip. The ini's
[hashes], [hashranges], [filtering], [reducehashranges], [options] and [games]
sections are read with the same meaning PPSSPP gives them, including the quick,
xxh32 and xxh64 hashes, ignoreAddress, reduceHash, and per-level "hash_1"
entries. Files in the pack root named after their hash are picked up without
an ini entry at all. ktx2 files supply their own mip chain; anything else gets
mips generated the way the game's own textures do.

To build a pack, turn on "Dump textures" under Debug. Every texture the game
uploads is written to textures/ULUS10176/new as <hash>.png, skipping the ones
the pack already replaces. Move a file up one directory to see it in game, or
list it in textures.ini under a friendlier name. The hashes are the same ones
PPSSPP computes, so a pack made here works there and vice versa.

Replacement textures are read at the moment the game first uploads the texture,
which costs a few milliseconds each. Note that a full HD pack can hold a lot
more texture memory than the game does -- roughly a gigabyte for a 1024x1024
pack -- so the cache evicts by least-recently-used above a budget that
PSPRECO_TEX_BUDGET_MB sets (1024 by default).


Tests
-----
    python tests/boot_check.py build/pspreco.exe
    python tests/chpak_check.py "Ace Combat X.iso"

boot_check runs the recompiled module to its first HLE call and diffs the
trace against tests/golden/boot_trace.txt, so a regression in the recompiler
shows up as a diff instead of a silent hang. chpak_check needs your own ISO
and isn't run in CI for that reason.


Third-party components
-----------------------
- third_party/imgui -- Dear ImGui, MIT licensed (see third_party/imgui/LICENSE.txt)
- third_party/ppsspp/at3_standalone -- ATRAC3/ATRAC3+ decoding, extracted from
  FFmpeg by the PPSSPP project (see its own README.txt for provenance)
- third_party/ppsspp/assets/flash0/font -- the PSP system fonts PPSSPP bundles
  as test assets; used here so on-screen text has something to render with
- third_party/basis_universal -- the Basis Universal transcoder, Apache 2.0
  licensed, plus zstd's decompression-only amalgamation, BSD 3-Clause; these
  read the .ktx2 files in a PPSSPP texture pack
