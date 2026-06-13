/* C ABI for acx_gpu.dll -- PPSSPP's Vulkan GPU, callable from the MinGW-gcc runtime.
 *
 * The runtime (the src/rt C files, MinGW gcc) and PPSSPP's libraries (MSVC) have incompatible C++ ABIs,
 * so all interaction goes through these plain C functions across the DLL boundary. Only pointers
 * and scalars cross; no CRT objects (FILE*, std::string, malloc'd blocks) are passed either way.
 *
 * Init order (GUI/Vulkan runs only):
 *   1. base = acx_gpu_mem_init();   // BEFORE loading the game image. Runtime sets g_mem = base
 *                                   // + 0x08000000 so both sides share one mirror-mapped arena.
 *   2. acx_gpu_init(hInst, hWnd);   // after the window exists (gui.c)
 * Per frame:
 *   - acx_gpu_enqueue_list(listpc, stall)  from sceGeListEnQueue   (hle.c h_GeListEnQueue)
 *   - acx_gpu_set_framebuf(addr, stride, fmt) from sceDisplaySetFrameBuf (hle.c)
 *   - acx_gpu_present()                    once the frame's lists are drained
 * Teardown: acx_gpu_shutdown();
 */
#ifndef ACX_GPU_BRIDGE_H
#define ACX_GPU_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize PPSSPP's guest memory mapping; returns the host pointer backing guest physical 0
 * (Memory::base), or NULL on failure. The runtime uses (return + 0x08000000) as its g_mem. */
void *acx_gpu_mem_init(void);

/* Create the Vulkan device + GPU_Vulkan on the given Win32 window. Returns 1 on success. */
int acx_gpu_init(void *hInst, void *hWnd);

/* Submit and run one GE display list (guest addresses; read straight from shared memory). */
void acx_gpu_enqueue_list(uint32_t listpc, uint32_t stall);

/* Record the displayed framebuffer. fmt: 0=565, 1=5551, 2=4444, 3=8888. */
void acx_gpu_set_framebuf(uint32_t addr, uint32_t stride, int fmt);

/* Composite to the swapchain and present one host frame. */
void acx_gpu_present(void);

/* Tear down GPU + device. */
void acx_gpu_shutdown(void);

/* PPSSPP MediaEngine bridge for sceMpeg/PSMF playback.  The GUI build always shares guest memory
 * with acx_gpu.dll, even when Vulkan rendering is disabled, so these functions can read compressed
 * PMF packets and write decoded frames directly at guest addresses. */
void acx_media_destroy(uint32_t key);
int  acx_media_load_stream(uint32_t key, uint32_t header_addr, int header_size, int ringbuffer_size);
int  acx_media_add_video_stream(uint32_t key, int stream_num, int stream_id);
int  acx_media_add_stream_data(uint32_t key, uint32_t data_addr, int size);
int  acx_media_step_video(uint32_t key, int stream_num, int pixel_mode, uint32_t buffer_addr, int frame_width);
int  acx_media_get_remain_size(uint32_t key);
int  acx_media_is_video_end(uint32_t key);
int64_t acx_media_get_video_timestamp(uint32_t key);
int64_t acx_media_get_last_timestamp(uint32_t key);

#ifdef __cplusplus
}
#endif

#endif /* ACX_GPU_BRIDGE_H */
