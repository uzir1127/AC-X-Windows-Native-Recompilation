# Credits and acknowledgements

## Original project

- **sal063** — [Ace-Combat-X-Recompilation](https://github.com/sal063/Ace-Combat-X-Recompilation): the original static recompiler, native runtime/renderer, baseline F4 settings menu, controller input and right-stick camera support. This enhancement fork retains that project's history, license and notices.
- This fork adds corrected native 60 FPS timing, direct ISO startup and portable packaging, modern input and rebinding, the F5 hardware readout, extended settings, font-pack compatibility and cinematic scaling/frame handling. These additions extend the original project.

## Texture and font artwork

The included **Ace Combat X HD Texture Pack v2.0.8** was created by **Ostrich / OstrichMods**: <https://ostrichmods.com/acx.html>.

- **Roach Typografics** — ACES5 HUD/menu font.
- **Dantofu** — F-14 SP2 cherry-blossom artwork.
- **UrvogelGallus** — Aquila Squadron emblem.
- The pack author's additional acknowledgements: **Abbanon** for bug reports, the **/aceg/** community for feedback, **Rythus** for expertise, and the **PPSSPP team**. Ostrich's creation tools include **GIMP** and **Blender**.

The artwork belongs to its credited creators. This fork changes font identity matching/filtering rather than claiming authorship of those images. Pack provenance and the original archive fingerprint are retained in `textures/ULUS10176/CREDITS.txt` in the Windows download. These credits do not relicense third-party artwork.

## Technical references

- **Nukem9** — [acex-60fps-patch](https://github.com/Nukem9/acex-60fps-patch), used to cross-check documented mission/gun/cooldown locations. This fork uses native hooks rather than injecting that CWCheat patch.
- **Foxvic** — the published Zero PC `dinputSample.ini` in [ACZeroPCremap](https://github.com/Foxvic/ACZeroPCremap), used as the functional input-layout reference. No remapper code is included.
- **SDL_GameControllerDB contributors** — [mdqinc/SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB). The Windows subset preserves its original mapping lines and records the upstream blob. Its zlib license is bundled.

## Libraries and platform components

- **PPSSPP / Henrik Rydgard and contributors** — PSP GE/font/texture-replacement behavior and replacement PGF fonts; GPLv2 or later. See `NOTICE.md` for the specific derivations.
- **FFmpeg developers and PPSSPP contributors** — standalone ATRAC3/ATRAC3+ decoder; its original notices and LGPL text are retained.
- **Omar Cornut and Dear ImGui contributors** — settings and editor UI; MIT.
- **Sam Lantinga and SDL contributors** — SDL3 window/input/audio support; zlib.
- **Binomial and Basis Universal contributors** — KTX2 transcoding; Apache 2.0.
- **Yann Collet / Meta and Zstandard contributors** — KTX2 decompression; BSD 3-Clause.
- **Lua.org, PUC-Rio and Lua authors** — Lua 5.4.9 scripting; MIT.
- **LLVM contributors, Martin Storsjo / LLVM-MinGW, and MinGW-w64/winpthreads contributors** — build toolchain and linked runtime components. Applicable LLVM, MinGW-w64 runtime and winpthreads notices are bundled. Players do not need to install the compiler.
- **Khronos Group** — Vulkan API; **Microsoft** — Windows/Media Foundation APIs; **NVIDIA** — NVML interface used when available for GPU utilization. These platform/driver components are supplied by Windows or the installed driver, not as a bundled SDK.

Full bundled notices are in `LICENSE`, `NOTICE.md`, `third_party/`, and the Windows download's `licenses/` directory. The project's runtime license does not override each third-party component's own license.

## Original game

Ace Combat X: Skies of Deception and the Ace Combat name, original game content and trademarks belong to Bandai Namco and their respective credited owners. This is an unofficial fan enhancement project, with no claimed affiliation or endorsement. Supply your own supported US ULUS10176 ISO. No ISO, original extracted game files, original movie/audio archives or personal saves are distributed.
