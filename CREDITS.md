# Credits and Third-Party Attribution

This project is **not** built from scratch. Large parts of it are derived from,
or directly reuse, other people's work. This file documents what was used, where
it lives, and under which license — and what your obligations are if you
redistribute this project or anything built with it.

## License of this project

Because this project links against and incorporates PPSSPP (GPL-2.0-or-later),
the combined work is licensed **GPL-2.0-or-later**. See [LICENSE](LICENSE).

If you distribute binaries built with this toolkit (for example a recompiled
game `.exe` plus `SDL3.dll`), the GPL requires that you also make the **complete
corresponding source** available to your recipients, including this toolkit's
source and any modifications you made. You may not relicense the result or ship
it as a closed-source product.

## AI assistance disclosure

Much of this project — including the SDL3 + Vulkan renderer (`src/rt/gpu_sdl3vk/`,
i.e. `sdl3vk.c`, `ge_gpu.c`, and the `shaders/*.vert|*.frag`) — was written with
substantial assistance from a large language model (Anthropic's Claude). AI was
used across the runtime, the HLE layer and the renderer:
to draft code, to translate/port C++ from PPSSPP into C (see the next section),
and to write comments and documentation.

What this means for you:

- **Correctness is not guaranteed.** AI-generated code can contain subtle bugs,
  and the "faithfulness vs PPSSPP" claims in the comments are the model's
  assertions, not an independent audit. Validate before relying on anything.
- **Provenance.** Where code was translated from PPSSPP, the GPL-2.0-or-later
  obligations apply regardless of whether a human or a model did the translating;
  using an AI tool does not change the license of derived code. The specific
  translated code is itemised below.
- The renderer is an **original implementation** (it does not reuse PPSSPP's GPU),
  but it reproduces PSP/GE pixel semantics that were derived from PPSSPP's
  software renderer (see `src/rt/ge.c`), so it is still a derivative work.

## PPSSPP — the single biggest dependency

- **Project:** PPSSPP — https://github.com/hrydgard/ppsspp
- **License:** GPL-2.0-or-later (see `third_party/ppsspp/LICENSE.TXT`)
- **Authors:** Henrik Rydgård and the PPSSPP contributors
- **How it is used here (this is substantial, not incidental):**
  - **HLE / media port.** The sceMpeg/PSMF runtime (`src/rt/mpeg.c`) is a direct
    port of PPSSPP's `Core/HLE/sceMpeg.cpp` (control flow, ring-buffer
    accounting, AU getters); it is explicitly GPLv2+. Other HLE behaviour
    (sceGe handling, kernel semantics) is modelled on or derived from PPSSPP.
  - **GE semantics reference.** The software GE rasterizer was validated against
    PPSSPP's software renderer; the GPU backend (`src/rt/gpu_sdl3vk`) reproduces
    PPSSPP-documented GE rules.
  - **Verification oracle.** The build runs `PPSSPPHeadless.exe` (PPSSPP's
    headless interpreter) as the reference the recompiled code is diffed
    against, bit-for-bit, up to the first HLE call.
  - **System fonts.** `font/` ships PPSSPP's replacement system fonts, needed
    for native text dialogs.
  - **Source tree.** A full copy of PPSSPP lives under `third_party/ppsspp`
    (including its test suite `pspautotests`).

  Note: an earlier `src/rt/gpu_vk/` bridge reused PPSSPP's `GPU_Vulkan`
  wholesale behind a C ABI (modelled on `WindowsVulkanContext.cpp`, linking
  `GPU.lib`/`Common.lib`/`Core.lib`). It was never activated and has been
  removed; the active renderer is the project's own SDL3 + Vulkan backend.

Where this project's code is "PPSSPP rewritten in C" (HLE kernel behaviour,
sceGe handling, media engine glue, etc.), it is still a derivative of GPL'd
PPSSPP code and is covered by the same license.

## Translated / ported code from PPSSPP (specifics)

The following files are direct translations/ports of PPSSPP C++ into C, or are
generated from PPSSPP data. All are GPL-2.0-or-later. PPSSPP source paths are
relative to `third_party/ppsspp/`.

| File in this project | Translated/derived from (PPSSPP) | Nature |
| --- | --- | --- |
| `src/rt/mpeg.c`, `mpeg` wrappers in `src/rt/hle.c` | `Core/HLE/sceMpeg.cpp` (rev 4e109dd6) | Faithful port of PSMF header analysis, ring-buffer accounting, MPEG context/handle creation, stream registration, and AU getters. Excludes the ffmpeg-based MediaEngine sample decode. |
| `src/rt/pgf.c`, `src/rt/pgf.h` | `Core/Font/PGF.cpp` | C port of the PGF font reader: PGF parse, `PGFFontInfo`/`PGFCharInfo`/`GlyphImage` layouts, glyph rasterisation. |
| `src/rt/vfpu_interp.c`; VFPU prefix + transcendental helpers behind `src/rt/recomp.h`/`recomp.c` | `Core/MIPS/MIPS.cpp` and PPSSPP's VFPU kernels | Exact ports of the table-based VFPU transcendentals and the `vcst` constant table; source/destination prefix application ported from PPSSPP. |
| `src/rt/ge.c` (software GE rasterizer) | `GPU/Software/*` (`Clipper`, `TransformUnit`, `Sampler.cpp`, `Lighting.cpp`, `DrawPixel.cpp`, `SoftGpu.cpp`), `GPU/Common/VertexDecoder*`, `GPU/ge_constants.h` / `GECommands.h`, `GPU/Common/TextureCacheCommon` | Algorithm-for-algorithm reimplementation of PPSSPP's software renderer: vertex decode, T&L, near-clipping, primitive acceptance, culling-by-submission-order, sampler/CLUT offsets, depth/fog semantics, command decode. |
| `src/rt/hle.c` (kernel/HLE behaviour) | `Core/HLE/*` (`sceUtility.cpp`, `sceCtrl.cpp`, `sceAtrac.cpp`, kernel object/UID pool, `SetDeadbeefRegs`), `Core/HW/Display.cpp` | Modelled on PPSSPP: UID allocation, partition layout, `sceUtility` defaults, sceCtrl ring-buffer sampling, sceAtrac decode bookkeeping, deadbeef register poisoning, vblank/line timing. |
| `src/rt/nid_names.h` | `Core/HLE/*.cpp` function tables | Generated from PPSSPP's HLE NID tables (do not edit by hand). |
| `src/rt/recomp.h` (`CpuState` layout, semantics) | `Core/MIPS/MIPS*` | VFPU `v[128]` physical ordering, `fpcond`/`fcr31` split, unaligned LWL/LWR/SWL/SWR, kernel object pool numbering all match PPSSPP. |

The SDL3 + Vulkan renderer (`src/rt/gpu_sdl3vk/`) is **not** a port of PPSSPP
code — it is an original implementation (AI-assisted; see the disclosure above).
It does, however, consume screen-space primitives from `src/rt/ge.c` and
reproduces PSP/GE pixel rules that were themselves derived from PPSSPP, so it is
a derivative work for licensing purposes.

## Other third-party components

- **SDL3** (`SDL3.dll`, `src/rt/gpu_sdl3vk`) — zlib license — https://libsdl.org
- **Vulkan loader / headers** — Apache-2.0 — https://github.com/KhronosGroup/Vulkan-Loader
- **PSPSDK headers/defines/constants** — BSD-compatible (see top of
  `third_party/ppsspp/LICENSE.TXT`).
- **Media Foundation H.264 path** (`src/rt/h264_mf.c`) — uses Microsoft Media
  Foundation, part of Windows.

## If you only want the recompiler, not PPSSPP

The MIPS-to-C recompiler itself (`tools/`, parts of `src/rt`) and the SDL3 +
Vulkan renderer are independent of PPSSPP. But the runtime still depends on
PPSSPP in other ways — the ported sceMpeg HLE (`src/rt/mpeg.c`), the bundled
fonts, and the verification oracle — and the full PPSSPP source is bundled under
`third_party/ppsspp`. So a GPL-free build is not available today; relicensing
would require replacing those remaining PPSSPP-derived parts.
