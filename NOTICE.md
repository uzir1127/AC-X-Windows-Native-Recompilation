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

`third_party/ppsspp/assets/flash0/font/` contains the PSP system fonts that
PPSSPP bundles as test assets. They are used here so on-screen text has
something to render with.

## Dear ImGui — MIT

<https://github.com/ocornut/imgui>

`third_party/imgui/` — the F4 debug and settings overlay. Full licence text in
`third_party/imgui/LICENSE.txt`.

## FFmpeg

The ATRAC3/ATRAC3+ decoding code in `third_party/ppsspp/at3_standalone/`
originates in FFmpeg. See that directory's `README.txt` for provenance.

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
