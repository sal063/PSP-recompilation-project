# Makefile for rebuilding / running a recompiled PSP game
#
# Usage:
#   mingw32-make GAME_NAME=mygame GAME_ELF=eboot.elf GAME_BASE=0x08804000 GAME_ENTRY=0x08804128
#

GAME_NAME  ?= ace_combat_x
GAME_ELF   ?= games/ULUS10176/eboot/EBOOT.elf
GAME_BASE  ?= 0x08804000
GAME_ENTRY ?= 0x08804128

CC         ?= gcc
PYTHON     ?= python
CFLAGS     ?= -O1 -foptimize-sibling-calls -Isrc/rt -DSR_SDL3VK -Wall -Wextra
LDFLAGS    ?= -fuse-ld=lld
LIBS       ?= -lSDL3 -lvulkan-1 -lmfplat -lgdi32 -ldinput8 -ldxguid -lole32 -lwinmm

BUILD_DIR  ?= build/$(GAME_NAME)

RT_GE_O    := $(BUILD_DIR)/ge.o
RT_SRCS    := src/rt/recomp.c \
              src/rt/vfpu_interp.c \
              src/rt/hle.c \
              src/rt/sched.c \
              src/rt/iso.c \
              src/rt/mpeg.c \
              src/rt/pgf.c \
              src/rt/gui.c \
              src/rt/audio.c \
              src/rt/h264_mf.c \
              src/rt/savedata.c \
              src/rt/osk_win.c \
              src/rt/driver.c \
              src/rt/gpu_sdl3vk/sdl3vk.c \
              src/rt/gpu_sdl3vk/ge_gpu.c

.PHONY: all pipeline compile clean run

all: pipeline compile

pipeline: $(BUILD_DIR)/$(GAME_NAME)_image.bin $(BUILD_DIR)/$(GAME_NAME)_recomp.c $(BUILD_DIR)/$(GAME_NAME)_imports.toml

$(BUILD_DIR)/$(GAME_NAME)_image.bin: $(GAME_ELF) tools/prxload.py
	@mkdir -p $(BUILD_DIR)
	$(PYTHON) tools/prxload.py $(GAME_ELF) $(GAME_BASE) --out=$@

$(BUILD_DIR)/$(GAME_NAME)_recomp.c: $(GAME_ELF) tools/codegen.py tools/analyze.py tools/prxload.py
	@mkdir -p $(BUILD_DIR)
	$(PYTHON) tools/codegen.py $(GAME_ELF) $@ --base=$(GAME_BASE)

$(BUILD_DIR)/$(GAME_NAME)_imports.toml: $(GAME_ELF) tools/imports.py tools/analyze.py tools/prxload.py
	@mkdir -p $(BUILD_DIR)
	$(PYTHON) tools/imports.py $(GAME_ELF) $(GAME_BASE) --toml=$@

# ge.c is the software rasterizer fallback, compile with -O2
$(RT_GE_O): src/rt/ge.c src/rt/recomp.h
	@mkdir -p $(BUILD_DIR)
	$(CC) -O2 -fno-math-errno -Wall -Wextra -Isrc/rt -c src/rt/ge.c -o $@

# Generated C code gets huge. Cache the compiled object to avoid rebuilding it on runtime changes.
$(BUILD_DIR)/$(GAME_NAME)_recomp.o: $(BUILD_DIR)/$(GAME_NAME)_recomp.c src/rt/recomp.h
	$(CC) $(CFLAGS) -c $< -o $@

compile: $(BUILD_DIR)/$(GAME_NAME)_recomp.o $(RT_GE_O)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(BUILD_DIR)/$(GAME_NAME).exe \
		$(BUILD_DIR)/$(GAME_NAME)_recomp.o \
		$(RT_GE_O) \
		$(RT_SRCS) \
		$(LIBS)
	@cp SDL3.dll $(BUILD_DIR)/ 2>/dev/null || cp ../SDL3.dll $(BUILD_DIR)/ 2>/dev/null || true
	@cp -r font $(BUILD_DIR)/ 2>/dev/null || cp -r ../font $(BUILD_DIR)/ 2>/dev/null || true
	@echo "Build finished: $(BUILD_DIR)/$(GAME_NAME).exe"

clean:
	rm -rf $(BUILD_DIR)

run: all
	./$(BUILD_DIR)/$(GAME_NAME).exe --image $(BUILD_DIR)/$(GAME_NAME)_image.bin $(GAME_BASE) $(GAME_ENTRY) none none --gui
