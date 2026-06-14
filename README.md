# Standalone PSP Static Recompiler Toolkit

This toolkit lets you translate a PSP game's relocatable executable (PRX/ELF) into C, link it with a native runtime (scheduler, HLE kernel, audio/video decoder, GPU renderer), and compile it into a native Windows executable.

## License & attribution (read this first)

This project is **GPL-2.0-or-later** because it is a derivative/combined work of
**[PPSSPP](https://github.com/hrydgard/ppsspp)** (Henrik Rydgård and
contributors, GPL-2.0-or-later). See [`../LICENSE`](../LICENSE) and
[`../CREDITS.md`](../CREDITS.md).

To be clear about what is and is not this project's own work:

- **Rendering is this project's own backend.** The active renderer is the
  from-scratch SDL3 + Vulkan backend in `src/rt/gpu_sdl3vk/`. It does **not**
  reuse PPSSPP's GPU. (An earlier experiment, `src/rt/gpu_vk/`, *did* wrap
  PPSSPP's `GPU_Vulkan` behind a C ABI, but it was never activated and has been
  removed.) The GE semantics it implements were validated against PPSSPP's
  software renderer, but the code is original.
- **PPSSPP is still used in substantial ways**, which is why the GPL applies:
  it is the verification oracle (`PPSSPPHeadless.exe`), it supplies the bundled
  system fonts, the sceMpeg/PSMF runtime (`src/rt/mpeg.c`) is a direct port of
  PPSSPP's `Core/HLE/sceMpeg.cpp` (GPLv2+), and a full copy of PPSSPP is bundled
  under `third_party/ppsspp`.

The novel part here is the offline MIPS-to-C static recompiler, the runtime, and
the GPU backend; significant HLE behaviour is ported from or modelled on PPSSPP.

If you redistribute anything built with this toolkit, the GPL requires you to
provide complete corresponding source to your recipients.

## Layout

- `tools/`: Offline compilation scripts (ELF loader, control flow analysis, MIPS-to-C code generator, and HLE import-stub mapper).
- `src/`: Native runtime libraries and the from-scratch SDL3 + Vulkan GPU renderer (`src/rt/gpu_sdl3vk`).
- `font/`: Replacement system fonts from PPSSPP (needed for native text dialogs).
- `SDL3.dll`: Prebuilt SDL3 library.
- `Makefile`: Build driver for compiling the runtime and your recompiled game.

## Prerequisites

- **OS**: Windows 10/11 x64 with a Vulkan-capable GPU.
- **Python 3**: For running the offline compilation scripts.
- **MSYS2 (UCRT64)**:
  - GCC: `pacman -S mingw-w64-ucrt-x86_64-gcc`
  - SDL3: `pacman -S mingw-w64-ucrt-x86_64-sdl3`
  - Vulkan Loader: `pacman -S mingw-w64-ucrt-x86_64-vulkan-loader`
  - LLD Linker (Recommended; prevents MinGW's ld from crashing on huge generated files): `pacman -S mingw-w64-ucrt-x86_64-lld`
- **Game Executable**: A decrypted ELF file of the game (obtained from `EBOOT.PBP` using PPSSPP or PSDecrypter).

## Recompiling a Game

### 1. Relocate the ELF
Since PSP executables are relocatable PRXs, you first need to rebase the game's ELF to its target virtual base address (usually `0x08804000`) and dump it as a flat memory image:
```bash
python tools/prxload.py <path_to_eboot.elf> 0x08804000 --out=build/mygame/mygame_image.bin
```

### 2. Map Library Imports
Resolve MIPS NIDs (Name IDs) in the ELF to HLE function names:
```bash
python tools/imports.py <path_to_eboot.elf> 0x08804000 --toml=build/mygame/mygame_imports.toml
```

### 3. Generate C Code
Run the code generator to translate MIPS machine instructions into native C code:
```bash
python tools/codegen.py <path_to_eboot.elf> build/mygame/mygame_recomp.c --base=0x08804000
```
This produces a large C file containing translated functions mapped to their guest memory locations.

### 4. Compile the Runtime & Game
Build the game and compile the runtime objects:
```bash
mingw32-make GAME_NAME=mygame GAME_ELF=<path_to_eboot.elf> GAME_BASE=0x08804000 GAME_ENTRY=<entry_point_address>
```
*Note: Make sure `GAME_ENTRY` matches the entry point address of your specific game ELF.*

The output executable will land at `build/mygame/mygame.exe`.

### 5. Setup Game Assets & Run
1. Place your dumped game UMD ISO in the directory containing `mygame.exe` (name it `game.iso`, or set the `PSP_ISO` environment variable to point to the ISO path).
2. Run the game from a command prompt:
```bash
build/mygame/mygame.exe --image build/mygame/mygame_image.bin 0x08804000 <entry_point_address> none none --gui
```

## Runtime Controls

If you have a gamepad connected, SDL3 will map it automatically. Otherwise, use these keyboard mappings:
- **Arrow Keys**: PSP D-pad
- **I, K, J, L**: Analog Stick (Up, Down, Left, Right)
- **Z / X**: Cross (A) / Circle (B)
- **A / S**: Square (X) / Triangle (Y)
- **Q / W**: L-Trigger / R-Trigger
- **Enter / Space**: Start / Select

## Environment Settings

Set these environment variables before running the game to control runtime features:
- `SR_GPU_GE=1`: Enable Vulkan GPU rasterization (default, recommended).
- `SR_GPU_GE=0`: Fallback to software rasterizer (slow, useful for debugging visual bugs).
- `SR_VIDEO=gdi`: Fall back to the classic Win32/GDI presentation path.
- `SR_GPU_LOG=1`: Print GPU cache flushes and batch information to the console.
- `SR_IOLOG=1`: Print all file system access logs.
