// Host-interface stubs for the GPU bridge.
//
// PPSSPP's Common/Core libraries call back into a "host" the embedding app must provide
// (System_*, Native* in Common/System). A normal PPSSPP frontend implements these to talk to
// the OS UI, audio, camera, secrets store, etc. The GPU bridge needs none of that behaviour, but
// the symbols are referenced by objects dragged into the link, so we provide inert defaults.
// Anything that actually mattered to rendering would surface as visibly wrong output, not here.

#include <string>
#include <string_view>
#include <functional>
#include <cstdint>
#include <cstring>

#include "Common/System/System.h"
#include "Common/System/NativeApp.h"

// ---- System properties: report a minimal headless desktop profile. ----
std::string System_GetProperty(SystemProperty prop) { return ""; }
std::vector<std::string> System_GetPropertyStringVec(SystemProperty prop) { return {}; }
int64_t System_GetPropertyInt(SystemProperty prop) {
	switch (prop) {
		case SYSPROP_DISPLAY_XRES: return 480;
		case SYSPROP_DISPLAY_YRES: return 272;
		default: return -1;
	}
}
float System_GetPropertyFloat(SystemProperty prop) {
	switch (prop) {
		case SYSPROP_DISPLAY_REFRESH_RATE: return 60.0f;
		default: return -1.0f;
	}
}
bool System_GetPropertyBool(SystemProperty prop) {
	switch (prop) {
		case SYSPROP_HAS_ACCELEROMETER: return false;
		default: return false;
	}
}

// ---- Requests / notifications / main-thread dispatch: no host UI to drive. ----
bool System_MakeRequest(SystemRequestType type, int requestId, const std::string &param1,
                        const std::string &param2, int64_t param3, int64_t param4) { return false; }
void System_Notify(SystemNotification notification) {}
void System_PostUIMessage(UIMessage message, std::string_view param) {}
void System_RunOnMainThread(std::function<void()> func) { if (func) func(); }

// ---- Audio: silent; PPSSPP audio is handled elsewhere in the runtime (src/rt/hle.c). ----
void System_AudioClear() {}
void System_AudioGetDebugStats(char *buf, size_t bufSize) { if (buf && bufSize) buf[0] = 0; }
void System_AudioPushSamples(const int32_t *audio, int numSamples, float volume) {}

// ---- Secrets store: not persisted. ----
bool NativeSaveSecret(std::string_view nameOfSecret, std::string_view data) { return false; }
std::string NativeLoadSecret(std::string_view nameOfSecret) { return ""; }

// ---- Camera / microphone (sceUsbCam/sceUsbMic) ----
// The Windows camera implementation lives in the Windows frontend project, not in the libs we
// link. It is dead code for ACX, but Core.lib's sceUsbCam/sceUsbMic objects reference these
// symbols. We supply matching definitions (minimal type decls -- only the names/signatures
// matter for linkage; the bodies never run). Layout is irrelevant since nothing calls them.
#include <vector>
enum class CAPTUREDEVICE_TYPE { VIDEO, AUDIO };
struct CAPTUREDEVICE_MESSAGE { int command; void *opacity; unsigned long long value; };
class WindowsCaptureDevice {
public:
	WindowsCaptureDevice(CAPTUREDEVICE_TYPE type);
	~WindowsCaptureDevice();
	std::vector<std::string> getDeviceList(bool forceEnum = false, int *pActualCount = nullptr);
	void sendMessage(CAPTUREDEVICE_MESSAGE message);
};
WindowsCaptureDevice::WindowsCaptureDevice(CAPTUREDEVICE_TYPE) {}
WindowsCaptureDevice::~WindowsCaptureDevice() {}
std::vector<std::string> WindowsCaptureDevice::getDeviceList(bool, int *pActualCount) {
	if (pActualCount) *pActualCount = 0;
	return {};
}
void WindowsCaptureDevice::sendMessage(CAPTUREDEVICE_MESSAGE) {}
WindowsCaptureDevice *winCamera = nullptr;
WindowsCaptureDevice *winMic = nullptr;
