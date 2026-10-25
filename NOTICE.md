# NOTICE

Attribution and licence notices for pspreco. This file is the single place these
statements live; source files carry none.

pspreco itself is licensed under the GNU General Public License, version 2 or
later. See `LICENSE`.

---

## PPSSPP — GPLv2 or later

<https://github.com/hrydgard/ppsspp>

The Graphics Engine implementation in `runtime/gpu/` reproduces PSP GE pixel
rules derived from PPSSPP's software renderer. This covers, among other things:
the triangle coverage and fill rules, the primitive-acceptance and near-clip
behaviour, texture swizzling, CLUT decoding, the lighting model, the depth-range
test, and the block-transfer register decode. Because those rules are derived
from PPSSPP, `runtime/gpu/` is GPLv2-or-later.

`runtime/hle/pgf.c` and `runtime/hle/pgf.h` are a C port of PPSSPP's
`Core/Font/PGF.cpp` (GPLv2), serving `sceLibFont`.

`third_party/ppsspp/at3_standalone/` contains the ATRAC3 and ATRAC3+ decoders
extracted from FFmpeg by the PPSSPP project to be standalone. See that
directory's own `README.txt`.

`third_party/ppsspp/assets/flash0/font/` contains replacement PGF fonts
distributed by PPSSPP. They are used here for on-screen text.
See https://dev.ppsspp.org/docs/getting-started/using-original-fonts/.

The texture replacement system in `runtime/gpu/tex_replace.c` reproduces
PPSSPP's `GPU/Common/TextureReplacer.cpp` behaviour so that texture packs built
for PPSSPP load unchanged: the `textures.ini` schema, the cache-key and
content-hash construction, `StableQuickTexHash`, the wildcard lookup order, and
the `%016llx%08x` hash filenames. It is GPLv2-or-later along with the rest of
`runtime/gpu/`.

## Dear ImGui — MIT

<https://github.com/ocornut/imgui>

`third_party/imgui/` — the F4 debug and settings overlay. Full licence text in
`third_party/imgui/LICENSE.txt`.

## Basis Universal — Apache 2.0

<https://github.com/BinomialLLC/basis_universal>

`third_party/basis_universal/transcoder/` is the transcoder half of Basis
Universal (release v1_60), used to read the `.ktx2` files in a PPSSPP texture
pack: UASTC and ETC1S are transcoded to RGBA8888. Only that target is compiled
in; every GPU block format is switched off in `build.py`. Full licence text in
`third_party/basis_universal/LICENSE`.

## Zstandard — BSD 3-Clause

<https://github.com/facebook/zstd>

`third_party/basis_universal/zstd/` is zstd's decompression-only single-file
amalgamation, shipped inside the Basis Universal source tree and needed for
KTX2 files that use Zstandard supercompression. Full licence text in
`third_party/basis_universal/zstd/LICENSE`.

## FFmpeg

The ATRAC3/ATRAC3+ decoding code in `third_party/ppsspp/at3_standalone/`
originates in FFmpeg. See that directory's `README.txt` for provenance.

## Lua — MIT

<https://www.lua.org/>

`third_party/lua/` is Lua 5.4.9, unmodified, from
<https://www.lua.org/ftp/lua-5.4.9.tar.gz>
(sha256 `2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6`).
It is the scripting host mods are written against; see
`runtime/hle/script.c`. Only the library is vendored — upstream's `lua.c` and
`luac.c` are the standalone interpreter and bytecode compiler and each carry
their own `main()`, so neither is included. Licence text is in
`third_party/lua/README`.

## AI assistance disclosure

The Vulkan backend in `runtime/gpu/` is an original implementation for this
project rather than a port of PPSSPP's Vulkan renderer, and was written with
substantial assistance from a large language model (Anthropic Claude). It
reproduces PSP GE pixel rules derived from PPSSPP as described above, and is
licensed accordingly.

## The game

No part of "Ace Combat X: Skies of Deception" ships with this repository — no
disc image, no extracted files, no asset archive, no executable, and no save
data. This project is a compiler pipeline plus an OS-reimplementation runtime.
You supply the game's data yourself, from media you own.

## Local Windows build - 2026-10-25

Base project: https://github.com/sal063/Ace-Combat-X-Recompilation

Local changes include physics-aware 60 FPS timing and cinematic cadence,
modern Xbox controls, direct read-only ISO startup, F4 settings persistence,
and portable Windows packaging. The engine-uncapping prototype is excluded.
The matching generated C, runtime and build sources are in licenses/source.zip.

This package includes OstrichMods' ACX HD Texture Pack v2.0.8 unchanged.
Author: https://ostrichmods.com/acx.html
Complete pack credits are in textures/ULUS10176/CREDITS.txt.
The original pack ZIP fingerprint is recorded there for provenance.
See VALIDATION.txt for the measured testing scope; full campaign completion
is not implied by the timing and mission-entry regression checks.

The modern input update adds Zero-style keyboard/mouse defaults and F4
bindings for SDL gamepads and arbitrary SDL joystick axes/buttons/hats.
The action-layout reference is the Zero PC dinputSample.ini published by
Foxvic/ACZeroPCremap; no code from that remapper is included.
The Windows controller mapping subset comes from mdqinc/SDL_GameControllerDB.
Its upstream blob identifier is in gamecontrollerdb.txt, and its zlib license
is included at licenses/SDL_GameControllerDB/LICENSE.txt.

## Enhancement fork credits and linked runtimes - 2026-10-26

See CREDITS.md in the source repository and CREDITS.txt in the Windows
download for the original-project, artwork and technical-reference credits.
The baseline F4 menu and input/camera support are part of sal063's project.
Nukem9's documented ACX patch locations were used as a cross-check; the
native implementation is described in README.txt. The included texture
artwork is by OstrichMods and the artists identified in the pack's credits.

SDL3 and the SDL_GameControllerDB subset retain their zlib notices.
Linked compiler/runtime notices are supplied in licenses/LLVM and
licenses/MinGW-w64 in the Windows download (and packaging/licenses in
the source). Those notices cover LLVM runtime components, MinGW-w64
runtime code and winpthreads; they do not mean the compiler is needed to play.

For this fork's Windows release, the upstream game-content exclusion above
refers to original disc data: the native runtime executable and credited
third-party replacement textures ARE included. No ISO, original extracted
game files, original movie/audio archives, or personal saves are included.
