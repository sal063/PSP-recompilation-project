# Standalone PSP Static Recompiler Toolkit

This toolkit lets you translate a PSP game's relocatable executable (PRX/ELF) into C, link it with a native runtime (scheduler, HLE kernel, audio/video decoder, Vulkan/SDL3 GPU renderer), and compile it into a native Windows executable.

## Layout

- `tools/`: Offline compilation scripts (ELF loader, control flow analysis, MIPS-to-C code generator, and HLE import-stub mapper).
- `src/`: Native runtime libraries and the SDL3+Vulkan GPU renderer.
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
1. Place your dumped game UMD ISO in the directory containing `mygame.exe` (name it `AceCombatX.iso` or equivalent, or set the `PSP_ISO` environment variable to point to the ISO path).
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
