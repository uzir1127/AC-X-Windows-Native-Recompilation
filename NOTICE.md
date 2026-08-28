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
