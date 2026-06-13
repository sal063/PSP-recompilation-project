// GPU bridge: drives PPSSPP's Vulkan GPU from the static-recompiled runtime.
//
// Why this exists / how it fits the project (ARCHITECTURE.md sections 3, 9):
//   The runtime + the 1.4M-line recompiled game are built with MinGW gcc (ucrt64), whose C++
//   ABI is incompatible with the MSVC-built PPSSPP libraries (GPU.lib/Common.lib/Core.lib).
//   So PPSSPP's renderer lives behind a pure C ABI in this DLL, built with the MSVC toolchain
//   so it is ABI-compatible with those prebuilt libs. The gcc runtime calls the extern "C"
//   entry points below; nothing but plain data (pointers, ints) crosses the boundary.
//
//   Guest memory is shared, not copied: the runtime hands us the host address that backs guest
//   physical 0 (Memory::base), and PPSSPP's GPU reads vertices/textures/display-lists straight
//   out of the same arena the recompiled code wrote (recomp.h SR_HOST == Memory::base + addr).
//
// This file is the Stage 0 skeleton: it stands up a Vulkan device + thin3d DrawContext and
// constructs GPU_Vulkan (adapted from Windows/GPU/WindowsVulkanContext.cpp), and exposes the C
// ABI the runtime will call. Display-list submission and present are filled in subsequent stages.

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define BLOG(...) do { fprintf(stderr, "[acx_gpu] " __VA_ARGS__); fflush(stderr); } while (0)

#include "Common/GraphicsContext.h"
#include "Common/GPU/Vulkan/VulkanLoader.h"
#include "Common/GPU/Vulkan/VulkanContext.h"
#include "Common/GPU/Vulkan/VulkanRenderManager.h"
#include "Common/GPU/thin3d.h"
#include "Common/GPU/thin3d_create.h"

#include "Core/Config.h"
#include "Core/System.h"
#include "Core/MemMap.h"
#include "Core/HW/MediaEngine.h"

#include "GPU/GPU.h"
#include "GPU/GPUCommon.h"
#include "GPU/GPUDefinitions.h"
#include "GPU/GPUState.h"
#include "GPU/ge_constants.h"
#include "GPU/Vulkan/VulkanUtil.h"
#include "GPU/Common/FramebufferManagerCommon.h"
#include "Core/CoreParameter.h"
#include "Core/Config.h"
#include "Core/HLE/sceGe.h"
#include "Common/Thread/ThreadManager.h"
#include "Common/CPUDetect.h"
#include "Common/Log/LogManager.h"

using namespace PPSSPP_VK;

// A GraphicsContext that owns the Vulkan device + thin3d draw context, modelled directly on
// WindowsVulkanContext. GPU_Init wants a GraphicsContext* and a Draw::DrawContext*.
class BridgeVulkanContext : public GraphicsContext {
public:
	bool Init(void *hInst, void *hWnd, std::string *error_message);
	void Shutdown() override;
	void Resize() override;
	void Poll() override;
	void *GetAPIContext() override { return vulkan_; }
	Draw::DrawContext *GetDrawContext() override { return draw_; }
	VulkanContext *Vulkan() { return vulkan_; }
	VulkanRenderManager *RenderManager() { return renderManager_; }

private:
	VulkanContext *vulkan_ = nullptr;
	Draw::DrawContext *draw_ = nullptr;
	VulkanRenderManager *renderManager_ = nullptr;
};

static BridgeVulkanContext *g_ctx = nullptr;
static std::map<uint32_t, std::unique_ptr<MediaEngine>> g_media;

bool BridgeVulkanContext::Init(void *hInst, void *hWnd, std::string *error_message) {
	*error_message = "N/A";
	init_glslang();

	std::string errorStr;
	if (!VulkanLoad(&errorStr)) {
		*error_message = "Failed to load Vulkan driver library: " + errorStr;
		return false;
	}

	vulkan_ = new VulkanContext();
	VulkanContext::CreateInfo info{};
	InitVulkanCreateInfoFromConfig(&info);
	// With SR_VKLOG, turn on the Khronos validation layers (Vulkan SDK provides them). Silent
	// render-pass/store-op/barrier mistakes are the most likely reason 621 error-free draws leave
	// the VFB black; validation surfaces them. Requires the SDK's validation layer to be installed.
	if (getenv("SR_VKLOG"))
		info.flags |= VulkanInitFlags::VALIDATE;
	if (VK_SUCCESS != vulkan_->CreateInstance(info)) {
		*error_message = vulkan_->InitError();
		delete vulkan_; vulkan_ = nullptr;
		return false;
	}
	int deviceNum = vulkan_->GetBestPhysicalDevice();
	if (vulkan_->CreateDevice(deviceNum) != VK_SUCCESS) {
		*error_message = vulkan_->InitError();
		delete vulkan_; vulkan_ = nullptr;
		return false;
	}
	vulkan_->InitSurface(WINDOWSYSTEM_WIN32, hInst, hWnd);

	draw_ = Draw::T3DCreateVulkanContext(vulkan_, false);
	VkPresentModeKHR presentMode = ConfigPresentModeToVulkan(draw_);
	if (!vulkan_->InitSwapchain(presentMode)) {
		*error_message = vulkan_->InitError();
		Shutdown();
		return false;
	}
	SetGPUBackend(GPUBackend::VULKAN, vulkan_->GetPhysicalDeviceProperties(deviceNum).properties.deviceName);
	bool presets = draw_->CreatePresets();
	BLOG("CreatePresets -> %d\n", presets);
	draw_->HandleEvent(Draw::Event::GOT_BACKBUFFER, vulkan_->GetBackbufferWidth(), vulkan_->GetBackbufferHeight());
	renderManager_ = (VulkanRenderManager *)draw_->GetNativeObject(Draw::NativeObject::RENDER_MANAGER);
	if (!renderManager_->HasBackbuffers()) {
		Shutdown();
		return false;
	}
	return true;
}

void BridgeVulkanContext::Shutdown() {
	if (draw_)
		draw_->HandleEvent(Draw::Event::LOST_BACKBUFFER, vulkan_->GetBackbufferWidth(), vulkan_->GetBackbufferHeight());
	delete draw_; draw_ = nullptr;
	if (vulkan_) {
		vulkan_->WaitUntilQueueIdle();
		vulkan_->DestroySwapchain();
		vulkan_->DestroySurface();
		vulkan_->DestroyDevice();
		vulkan_->DestroyInstance();
		delete vulkan_; vulkan_ = nullptr;
	}
	renderManager_ = nullptr;
	finalize_glslang();
}

void BridgeVulkanContext::Resize() {
	draw_->HandleEvent(Draw::Event::LOST_BACKBUFFER, vulkan_->GetBackbufferWidth(), vulkan_->GetBackbufferHeight());
	VkPresentModeKHR presentMode = ConfigPresentModeToVulkan(draw_);
	vulkan_->InitSwapchain(presentMode);
	draw_->HandleEvent(Draw::Event::GOT_BACKBUFFER, vulkan_->GetBackbufferWidth(), vulkan_->GetBackbufferHeight());
}

void BridgeVulkanContext::Poll() {
	if (vulkan_->IsSwapchainInited() && renderManager_->NeedsSwapchainRecreate())
		Resize();
}

// ---------------------------------------------------------------------------
// C ABI consumed by the gcc-built runtime (declared in src/rt/gpu_vk/gpu_bridge.h).
// ---------------------------------------------------------------------------
extern "C" {

// Phase 1 of init: own the guest memory mapping. PPSSPP's x64 libs read guest RAM as
// (Memory::base + address) with the OS aliasing every cached/uncached/kernel mirror to the same
// physical pages -- a layout the runtime's plain malloc arena cannot reproduce. So PPSSPP's
// Memory owns the mapping, and the runtime points its g_mem at (base + 0x08000000). Then the
// runtime's SR_HOST(a) == base + SR_PHYS(a) lands on the exact same bytes PPSSPP sees, mirrors
// included. Returns base (guest physical 0); the runtime adds 0x08000000 for its RAM window.
__declspec(dllexport) void *acx_gpu_mem_init(void) {
	Memory::g_MemorySize = Memory::RAM_NORMAL_SIZE;   // 32 MB, PSP-1000
	Memory::g_PSPModel = PSP_MODEL_FAT;
	if (!Memory::Init(Memory::MemMapSetupFlags::Default)) {
		BLOG("Memory::Init FAILED\n");
		return nullptr;
	}
	BLOG("Memory::Init ok, base=%p (guest 0x08000000 -> %p)\n", Memory::base, Memory::base + 0x08000000);
	return Memory::base;
}

// Phase 2: create the Vulkan device on the runtime's existing window and bring up GPU_Vulkan.
__declspec(dllexport) int acx_gpu_init(void *hInst, void *hWnd) {
	// Surface PPSSPP's own diagnostics (pipeline/shader compile failures, render-pass mismatches,
	// validation) to stderr when SR_VKLOG is set. Without this the engine's ERROR/WARN logs go
	// nowhere, which makes a black screen (e.g. every draw skipped because its pipeline failed to
	// compile) impossible to diagnose from the outside.
	if (getenv("SR_VKLOG")) {
		static bool logEnabled = true;
		g_logManager.Init(&logEnabled, false);
		g_logManager.SetOutputsEnabled(LogOutput::Stdio);
		g_logManager.SetAllLogLevels(LogLevel::LINFO);
		BLOG("PPSSPP logging enabled (SR_VKLOG)\n");
	}

	// PPSSPP's render manager / shader + texture pipelines schedule work on the global thread
	// pool; it must be initialized before any GPU object is created (as the headless host does).
	int cores = cpu_info.num_cores > 0 ? cpu_info.num_cores : 4;
	int logical = cpu_info.logical_cpu_count > 0 ? cpu_info.logical_cpu_count : 1;
	g_threadManager.Init(cores, logical);

	BLOG("acx_gpu_init: creating Vulkan context (hWnd=%p)\n", hWnd);
	g_ctx = new BridgeVulkanContext();
	std::string err;
	if (!g_ctx->Init(hInst, hWnd, &err)) {
		BLOG("BridgeVulkanContext::Init FAILED: %s\n", err.c_str());
		delete g_ctx; g_ctx = nullptr;
		return 0;
	}
	// The framebuffer manager's presentation reads the output size from CoreParameter; without it
	// the present target is 0x0 and CopyDisplayToOutput dereferences a degenerate layout. Set the
	// output (window/backbuffer) size and a 1x internal render resolution (480x272).
	int bbw = g_ctx->Vulkan()->GetBackbufferWidth();
	int bbh = g_ctx->Vulkan()->GetBackbufferHeight();
	PSP_CoreParameter().pixelWidth = bbw;
	PSP_CoreParameter().pixelHeight = bbh;
	PSP_CoreParameter().renderWidth = 480;
	PSP_CoreParameter().renderHeight = 272;
	PSP_CoreParameter().renderScaleFactor = 1;
	// Force 1x internal resolution. Otherwise iInternalResolution defaults to Auto, and PPSSPP's
	// framebuffer manager auto-resize (FramebufferManagerCommon.cpp ~2916) recomputes the scale from
	// the 960x544 window and overwrites renderScaleFactor back to 2 -- leaving the VFBs rendered at
	// 960x544 while CoreParameter says 480x272/1x. That inconsistency desynchronizes the display copy
	// (and readbacks) from the buffers the GE actually rendered into. Pin everything to 1x.
	g_Config.iInternalResolution = 1;
	BLOG("backbuffer %dx%d\n", bbw, bbh);

	BLOG("Vulkan context up; calling GPU_Init(VULKAN)\n");
	Draw::DrawContext *draw = g_ctx->GetDrawContext();
	if (!GPU_Init(GPUCORE_VULKAN, g_ctx, draw)) {
		BLOG("GPU_Init FAILED\n");
		return 0;
	}
	gpu->DeviceRestore(draw);   // finish backend-specific init now that the device exists
	BLOG("GPU_Init ok, gpu=%p\n", (void *)gpu);
	return 1;
}

// PPSSPP's render manager records GE rendering into an open host frame, so a frame must be begun
// (draw->BeginFrame + gpu->BeginHostFrame) before any display list is processed and closed at
// present. The game submits lists as it runs, interleaved with emulation, so we begin the frame
// lazily on the first list/present and end it at present -- the emulator-frame structure PPSSPP's
// own host loop uses.
static DisplayLayoutConfig s_layout;
static bool s_inFrame = false;
static void ensure_frame(void) {
	if (s_inFrame || !gpu || !g_ctx) return;
	Draw::DrawContext *draw = g_ctx->GetDrawContext();
	draw->BeginFrame(Draw::DebugFlags::NONE);
	gpu->BeginHostFrame(s_layout);
	s_inFrame = true;
}

// Submit a GE display list (from sceGeListEnQueue) and run it. Mirrors Core/HLE/sceGe.cpp:
// EnqueueList then, if it became the head of the queue, ProcessDLQueue drains it.
__declspec(dllexport) void acx_gpu_enqueue_list(uint32_t listpc, uint32_t stall) {
	if (!gpu) return;
	static int n = 0;
	ensure_frame();
	bool runList = false;
	gpu->EnqueueList(listpc, stall, -1, PSPPointer<PspGeListArgs>(), false, &runList);
	if (runList)
		gpu->ProcessDLQueue();
	if (n++ < 8) BLOG("enqueue_list #%d pc=%08x stall=%08x runList=%d\n", n, listpc, stall, runList);
	// One-shot gstate dump (SR_VKLOG): the rasterizer region after the list ran. If scissor/region
	// is degenerate (zero area) or the framebuffer/viewport is off, draws are clipped to nothing,
	// which matches "draws recorded, VFB black, no errors".
	if (getenv("SR_VKLOG")) {
		static int dumped = 0;
		if (dumped < 3) { dumped++;
			BLOG("gstate: fb=%08x stride=%d clear=%d through=%d  scissor=(%d,%d)-(%d,%d) region=(%d,%d)-(%d,%d)\n"
			     "        vpScale=(%.1f,%.1f) vpCenter=(%.1f,%.1f) offset=(%.1f,%.1f)\n",
			     gstate.getFrameBufAddress(), gstate.FrameBufStride(), gstate.isModeClear(), gstate.isModeThrough(),
			     gstate.getScissorX1(), gstate.getScissorY1(), gstate.getScissorX2(), gstate.getScissorY2(),
			     gstate.getRegionX1(), gstate.getRegionY1(), gstate.getRegionX2(), gstate.getRegionY2(),
			     gstate.getViewportXScale(), gstate.getViewportYScale(),
			     gstate.getViewportXCenter(), gstate.getViewportYCenter(),
			     gstate.getOffsetX(), gstate.getOffsetY());
		}
	}
}

// Record the display framebuffer the game just flipped to (from sceDisplaySetFrameBuf).
// format: 0=565, 1=5551, 2=4444, 3=8888 -- the PSP GEBufferFormat encoding the runtime passes.
static uint32_t s_lastFb = 0;
__declspec(dllexport) void acx_gpu_set_framebuf(uint32_t addr, uint32_t stride, int format) {
	s_lastFb = addr;
	if (gpu)
		gpu->SetDisplayFramebuffer(addr, stride, (GEBufferFormat)format);
}

// Composite the PSP framebuffer to the swapchain and present one host frame. The display layout
// uses PPSSPP's defaults (1:1, centered). Frame pacing stays with the runtime/gui.c.
__declspec(dllexport) void acx_gpu_present(void) {
	if (!gpu || !g_ctx) return;
	static int n = 0;
	if (n++ < 8) BLOG("present #%d (inFrame=%d)\n", n, s_inFrame);
	Draw::DrawContext *draw = g_ctx->GetDrawContext();
	static int trace = -1; if (trace < 0) trace = getenv("SR_VKTRACE") ? 1 : 0;
	bool dbg = trace && n <= 8;
	g_ctx->Poll();
	ensure_frame();                 // open a frame if the game submitted no lists this frame
	if (dbg) BLOG("  PrepareCopy...\n");
	gpu->PrepareCopyDisplayToOutput(s_layout);
	// Finalize the GE/host frame BEFORE compositing. This mirrors PPSSPP's EmuScreen, which calls
	// PrepareCopyDisplayToOutput + EndHostFrame at the end of RunEmulation and only then binds the
	// backbuffer and calls CopyDisplayToOutput. EndHostFrame runs drawEngine_.EndFrame() (flushing
	// pending draws) and decimates/resolves the VFB render passes; doing it AFTER CopyDisplayToOutput
	// -- as this bridge originally did -- composited (and read back) the display VFB before the
	// scene was committed to it, which left both VFBs and the backbuffer black despite 621 draws.
	if (dbg) BLOG("  EndHostFrame...\n");
	gpu->EndHostFrame();

	// Headless proof-of-render: with SR_VKDUMP=<period>, read each VFB's own color buffer to a PPM.
	// Done HERE -- after EndHostFrame (GE draws flushed) but BEFORE the backbuffer is bound -- so the
	// BLOCK readback's FlushSync submits only the offscreen VFB render passes and never touches the
	// swapchain (reading the backbuffer out-of-band caused a spurious acquire-semaphore validation
	// error). This is the truthful answer to "did the GE actually rasterize anything into the VFB".
	{
		static int period = -2;
		if (period == -2) { const char *e = getenv("SR_VKDUMP"); period = e ? atoi(e) : -1; }
		int frame = n - 1;
		if (period > 0 && (frame % period) == 0) {
			BLOG("frame %d: dispFb=0x%08x draws=%d culled=%d verts=%d texDecoded=%d fbos=%d clears=%d\n",
			     frame, s_lastFb, gpuStats.perFrame.numDrawCalls, gpuStats.perFrame.numCulledDraws,
			     gpuStats.perFrame.numVertsSubmitted, gpuStats.perFrame.numTexturesDecoded,
			     gpuStats.perFrame.numFBOsCreated, gpuStats.perFrame.numClears);
			auto fbs = gpu->GetFramebufferManagerCommon()->GetFramebufferList();
			for (auto *v : fbs) {
				int vw = v->renderWidth, vh = v->renderHeight; size_t vnb = 0;
				std::vector<uint8_t> vp((size_t)vw * vh * 4);
				Draw::Framebuffer *fbo = v->fbo;
				if (fbo && draw->CopyFramebufferToMemory(fbo, Draw::Aspect::COLOR_BIT, 0, 0, vw, vh,
				        Draw::DataFormat::R8G8B8A8_UNORM, vp.data(), vw, Draw::ReadbackMode::BLOCK, "vfb")) {
					for (size_t i = 0; i < vp.size(); i += 4) if (vp[i]|vp[i+1]|vp[i+2]) vnb++;
					char vpath[128]; snprintf(vpath, sizeof(vpath), "build/acx/vk_vfb_%08x_f%05d.ppm", v->fb_address, frame);
					FILE *vf = fopen(vpath, "wb");
					if (vf) { fprintf(vf, "P6\n%d %d\n255\n", vw, vh);
						for (int i = 0; i < vw*vh; i++) fwrite(&vp[(size_t)i*4], 1, 3, vf); fclose(vf); }
					BLOG("    VFB addr=0x%08x %dx%d (render %dx%d) fmt=%d nonblack=%zu -> %s\n",
					     v->fb_address, v->width, v->height, vw, vh, (int)v->fb_format, vnb, vpath);
				} else {
					BLOG("    VFB addr=0x%08x %dx%d fmt=%d (readback failed)\n",
					     v->fb_address, v->width, v->height, (int)v->fb_format);
				}
			}
		}
	}

	// Bind the swapchain backbuffer as the render target before compositing -- PPSSPP's UI screen
	// framework normally does this; without an active backbuffer pass CopyDisplayToOutput's draws
	// have no target and crash (this was the first-frame segfault).
	if (dbg) BLOG("  BindBackbuffer...\n");
	Draw::RenderPassInfo rp{ Draw::RPAction::CLEAR, Draw::RPAction::CLEAR, Draw::RPAction::CLEAR,
	                        0u, 0.0f, (uint8_t)0, "BackBuffer" };
	draw->BindFramebufferAsRenderTarget(nullptr, rp, "BackBuffer");
	if (dbg) BLOG("  CopyDisplayToOutput...\n");
	gpu->CopyDisplayToOutput(s_layout);

	if (dbg) BLOG("  draw->EndFrame...\n");
	draw->EndFrame();   // renderManager Finish() -- SUBMITS all recorded VFB + backbuffer passes
	if (dbg) BLOG("  draw->Present...\n");
	draw->Present(Draw::PresentMode::FIFO);
	s_inFrame = false;
	if (dbg) BLOG("present #%d done\n", n);
}

__declspec(dllexport) void acx_gpu_shutdown(void) {
	GPU_Shutdown();
	if (g_ctx) { g_ctx->Shutdown(); delete g_ctx; g_ctx = nullptr; }
	g_media.clear();
}

static MediaEngine *media_for(uint32_t key, bool create) {
	auto it = g_media.find(key);
	if (it != g_media.end())
		return it->second.get();
	if (!create)
		return nullptr;
	auto me = std::make_unique<MediaEngine>();
	MediaEngine *ptr = me.get();
	g_media.emplace(key, std::move(me));
	return ptr;
}

__declspec(dllexport) void acx_media_destroy(uint32_t key) {
	g_media.erase(key);
}

__declspec(dllexport) int acx_media_load_stream(uint32_t key, uint32_t header_addr, int header_size, int ringbuffer_size) {
	if (header_size <= 0 || ringbuffer_size <= 0 || !Memory::IsValidRange(header_addr, (uint32_t)header_size))
		return 0;
	MediaEngine *me = media_for(key, true);
	if (!me)
		return 0;
	const uint8_t *header = Memory::GetPointer(header_addr);
	int ok = me->loadStream(header, header_size, ringbuffer_size) ? 1 : 0;
	if (getenv("SR_MPEGLOG"))
		BLOG("media_load key=0x%08x header=0x%08x size=%d ring=%d -> %d\n", key, header_addr, header_size, ringbuffer_size, ok);
	return ok;
}

__declspec(dllexport) int acx_media_add_video_stream(uint32_t key, int stream_num, int stream_id) {
	MediaEngine *me = media_for(key, true);
	if (!me)
		return 0;
	int ok = me->addVideoStream(stream_num, stream_id) ? 1 : 0;
	if (getenv("SR_MPEGLOG"))
		BLOG("media_add_video key=0x%08x stream=%d id=%d -> %d\n", key, stream_num, stream_id, ok);
	return ok;
}

__declspec(dllexport) int acx_media_add_stream_data(uint32_t key, uint32_t data_addr, int size) {
	MediaEngine *me = media_for(key, false);
	if (!me || size <= 0 || !Memory::IsValidAddress(data_addr))
		return 0;
	uint32_t valid = Memory::ClampValidSizeAt(data_addr, (uint32_t)size);
	if ((int)valid > size)
		valid = (uint32_t)size;
	const uint8_t *data = Memory::GetPointer(data_addr);
	int added = me->addStreamData(data, (int)valid);
	if (getenv("SR_MPEGLOG")) {
		static int n = 0;
		if (n++ < 32 || added != (int)valid)
			BLOG("media_add_data key=0x%08x addr=0x%08x size=%d valid=%u -> %d\n", key, data_addr, size, valid, added);
	}
	return added;
}

__declspec(dllexport) int acx_media_step_video(uint32_t key, int stream_num, int pixel_mode, uint32_t buffer_addr, int frame_width) {
	MediaEngine *me = media_for(key, false);
	if (!me)
		return 0;
	if (stream_num >= 0)
		me->setVideoStream(stream_num);
	if (me->IsVideoEnd())
		return -1;
	if (!me->stepVideo(pixel_mode))
		return 0;
	int w = frame_width ? frame_width : 512;
	int bytes = me->writeVideoImage(buffer_addr, w, pixel_mode);
	if (bytes > 0 && gpu)
		gpu->PerformWriteFormattedFromMemory(buffer_addr, bytes, w, (GEBufferFormat)pixel_mode);
	if (getenv("SR_MPEGLOG")) {
		static int n = 0;
		if (n++ < 24)
			BLOG("media_step key=0x%08x stream=%d pix=%d dst=0x%08x fw=%d -> bytes=%d pts=%lld\n",
			     key, stream_num, pixel_mode, buffer_addr, w, bytes, (long long)me->getVideoTimeStamp());
	}
	return bytes > 0 ? 1 : 0;
}

__declspec(dllexport) int acx_media_get_remain_size(uint32_t key) {
	MediaEngine *me = media_for(key, false);
	return me ? me->getRemainSize() : 0;
}

__declspec(dllexport) int acx_media_is_video_end(uint32_t key) {
	MediaEngine *me = media_for(key, false);
	return me && me->IsVideoEnd() ? 1 : 0;
}

__declspec(dllexport) int64_t acx_media_get_video_timestamp(uint32_t key) {
	MediaEngine *me = media_for(key, false);
	return me ? me->getVideoTimeStamp() : 0;
}

__declspec(dllexport) int64_t acx_media_get_last_timestamp(uint32_t key) {
	MediaEngine *me = media_for(key, false);
	return me ? me->getLastTimeStamp() : 0;
}

}  // extern "C"
