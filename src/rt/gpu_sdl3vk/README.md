# SDL3 + Vulkan GPU renderer

New GPU renderer for the recompiled runtime, replacing the Win32/GDI presentation path and
(eventually) the software GE rasterizer in `src/rt/ge.c`. Built from scratch in this
directory — it is the project's own Vulkan backend and does **not** reuse PPSSPP's GPU. (An
earlier PPSSPP-GPU bridge under `src/rt/gpu_vk/` was a frozen experiment and has been removed.)

> **AI disclosure:** this renderer was written with substantial assistance from an LLM
> (Anthropic Claude). It is an original implementation, but it reproduces PSP/GE pixel
> semantics derived from PPSSPP's software renderer (through `ge.c`), so it is GPL-2.0-or-later.
> See [`../../../CREDITS.md`](../../../CREDITS.md). Treat "faithfulness" claims below as
> intent, not an audited guarantee.

## Why this shape

The software GE in `ge.c` is now the reference-correct implementation (clipping, PSP
primitive acceptance, strip cull parity, depth semantics — all validated against PPSSPP's
software renderer and in-game). The GPU renderer is introduced *under* it in phases, so at
every step there is a known-good fallback and an A/B comparison target. `ge.c` stays the
arbiter of GE semantics; this backend starts as a presenter and absorbs stages of the
pipeline one at a time.

## Build / toolchain

- SDL3 (`mingw-w64-ucrt-x86_64-sdl3`, currently 3.2.20) and Vulkan headers + loader
  (`mingw-w64-ucrt-x86_64-vulkan-headers`, `-vulkan-loader`) from MSYS2 ucrt64 —
  installed already. Link: `-lSDL3 -lvulkan-1`. Deploy `SDL3.dll` beside the exe.
- Shader compiler for Phase 1+: `glslc` from the Vulkan SDK (`C:\VulkanSDK\1.4.350.0\Bin`).
- Built into the game exe by the top-level `Makefile` (the runtime always links this SDL3 +
  Vulkan backend; `-DSR_SDL3VK`). Inside that build, `SR_VIDEO=gdi` falls back to the classic
  Win32/GDI window at runtime.

## Phases

### Phase 0 — presentation (DONE, this directory)
SDL3 window + Vulkan instance/device/swapchain. The software GE keeps rasterizing into
guest VRAM; `gui_present` converts the flipped framebuffer to BGRA once (same loop as the
GDI path) and `sdl3vk_present_rgba()` uploads + blits it to the swapchain (FIFO/vsync,
aspect-correct letterbox). Input moves to SDL3 (keyboard + SDL_Gamepad, same PSP-bit
mapping as gui.c). Deliverable: pixel-identical output to the GDI build at lower CPU cost,
resizable window, fullscreen-capable.

### Phase 1 — GE draw capture → Vulkan rasterization (DONE, `ge_gpu.c`)
Enabled with `SR_GPU_GE=1`. Implementation:
- Capture seam: runtime hooks (`GeGpuHooks` in `src/rt/ge_shared.h`, registered via
  `ge_set_gpu_hooks`) at the top of `raster_tri` and `fill_sprite`. ge.c still does
  vertex decode, T&L, near-clipping, primitive acceptance and cull-order reordering;
  the backend receives finished screen-space primitives. The GDI build registers
  nothing and is bit-identical to before.
- Vulkan side: 512x272 RGBA8 + D16 offscreen target, classic render pass (LOAD/STORE),
  one uber fragment shader (`shaders/psp.frag`: texfunc 0-7 + RGBA/double bits, alpha
  test, fog, minz/maxz discard, clear mode), pipelines cached per
  blend/depth/cull/write-mask key, dynamic viewport/scissor/blend-constants, vertices
  pre-snapped to the 28.4 grid, noperspective interpolation of (u·rw, v·rw, rw) to
  match the software inner loop exactly.
- **Guest VRAM stays authoritative**: at every flush point (GE list END, framebuffer
  retarget, texture-from-VRAM, CLUT-from-VRAM, present, software fallback) the target
  is rendered and written back to guest VRAM + the software z-buffer in the native
  format (16-bit round trip is lossless). Anything unsupported — lines, points,
  doubled-alpha blend factors, min/max/absdiff blend equations, two distinct FIX
  constants, partial-byte write masks — flushes and falls back to the software
  rasterizer in correct order. Presentation/snapshot code is unchanged.
- Not reproduced on GPU: ordered dithering (±4 LSB on 16-bit targets) and the integer
  truncation of the software blender (±1 LSB).
- A/B harness: run twice with `SR_FBSNAP=1` (once `SR_GPU_GE=0`), then
  `python tools/ppmdiff.py dirA dirB`. Title/menu sequence matches pixel-exactly
  except dithering. Measured GE cost on the intro: ~1700ms/60-frames software →
  ~330ms/60-frames GPU (~5x).

### Phase 2 — textures on GPU (DONE, folded into Phase 1)
- Texture cache keyed by (addr, fmt, dims, bufw, swizzle, wrap, filter) + a sparse FNV
  content hash (CLUT RAM + format word included for CLUT4/CLUT8). Decode goes through
  `ge_decode_tex_rgba()` in ge.c — the exact software sampler path — so swizzle/CLUT
  behaviour is reference-correct by construction. 256 entries, full-drop eviction.
- Invalidation is the content hash ("hash on use", PPSSPP-style). Render-to-texture
  works because texturing from VRAM forces a flush+writeback first, then the hash
  sees the new pixels.

### Phase 3 — full GE on GPU / cleanup
- Move T&L (world/view/proj, skinning, morphing, lighting, texgen) into the vertex shader;
  CPU then only decodes vertex formats. Validate against `transform_vtx_clip` with the
  existing SR_VDUMP machinery.
- Render-to-texture chains (the hangar uses fbp swaps), depth readbacks if any game code
  reads zbuf, small-tri rasterization rules (top-left bias already documented in
  raster_tri) via conservative settings.
- Only after Phase 3 ships does GDI/software become optional at build time. The software
  GE is never deleted: it is the documentation of GE semantics.

## Files

- `sdl3vk.h` — C ABI used by `gui.c` (`-DSR_SDL3VK`); also exports the shared Vulkan
  device handles (`sdl3vk_get_vk`).
- `sdl3vk.c` — Phase 0: window, device, swapchain, upload/blit presenter, SDL input.
- `ge_gpu.h` / `ge_gpu.c` — Phase 1+2: capture, batching, pipeline/texture caches,
  flush + guest-VRAM writeback. Hooks defined in `src/rt/ge_shared.h`.
- `shaders/psp.vert`, `shaders/psp.frag` — compiled by glslc (`GLSLC` in the Makefile)
  into `psp_{vert,frag}.inc` at build time.
- `tools/ppmdiff.py`, `tools/ppm2png.py` (repo root) — A/B snapshot comparison.

## Environment variables

- `SR_GPU_GE=1` — enable the GPU rasterizer (SDL3 build only; default off).
- `SR_GPU_LOG=1` — one line per flush (reason, batches, verts, target).
- `SR_VIDEO=gdi` — Phase-0 fallback to the classic GDI window.

## PSP/GE semantics that MUST carry over (hard-won, see ge.c comments)

1. Primitive acceptance: whole-triangle drops on out-of-grid / z-range vertices
   (oor/ozp/ozn, depth-clamp on/off variants) happen **before** rasterization.
2. Near clip only against z >= -w; clip-created vertices outside the 4096-grid kill their
   sub-triangle (PPSSPP `clip_interpolate` rule).
3. Culling is submission-order: rasterizer keeps positive-winding only; cull mode reverses
   order, strips flip per parity. Map to VK_CULL_MODE with care (or pre-flip on CPU).
4. Depth: 16-bit, often inverted range (viewport zscale < 0), GEQUAL tests, minz/maxz is a
   per-pixel *discard* range, not a clamp.
5. CLUT-format quirks, swizzled textures, color-double texfunc bit, alpha 0..0xFF with
   0x80 = 1.0 semantics in blending.
