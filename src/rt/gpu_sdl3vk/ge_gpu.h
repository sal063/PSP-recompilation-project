/* Phase 1 GPU rasterizer for the software GE (see README.md). C ABI used by gui.c. */
#ifndef GE_GPU_H
#define GE_GPU_H

#ifdef __cplusplus
extern "C" {
#endif

/* Create the Vulkan objects (shares the sdl3vk device; sdl3vk_init() must have
 * succeeded) and register the capture hooks with ge.c. Returns 1 on success; on
 * failure nothing is registered and the software rasterizer runs unchanged. */
int  gegpu_init(void);

/* GE sync points ("listend", "loadclut"); the backend decides what each requires.
 * Safe to call when idle/uninitialized. */
void gegpu_flush(const char *reason);

/* Present the framebuffer at fbaddr straight from its GPU image. Returns 1 when shown,
 * 0 when the user closed the window, -1 when the address is not GPU-resident
 * (CPU-written movie frames etc.) — the caller should convert guest VRAM instead. */
int  gegpu_present(unsigned int fbaddr, int fmt, unsigned int stride);

void gegpu_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
