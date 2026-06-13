# Builds acx_gpu.dll: PPSSPP's Vulkan GPU behind a C ABI, linked against PPSSPP's prebuilt
# MSVC libraries. Built with the MSVC toolchain so it is ABI-compatible with those libs (the
# runtime that calls it is MinGW gcc and talks to this DLL only across the extern "C" boundary).
#
# Configure/build (from repo root):
#   cmake -S src/rt/gpu_vk -B build/gpu_vk -G "Visual Studio 17 2022" -A x64
#   cmake --build build/gpu_vk --config Release
cmake_minimum_required(VERSION 3.20)
project(acx_gpu CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Match PPSSPP's prebuilt libs: static multithreaded CRT (/MT).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# Locations.
set(PPSSPP "${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/ppsspp")
set(PPLIB  "${PPSSPP}/Windows/x64/Release")
# Locate the Vulkan SDK: prefer the env var, else pick the newest C:/VulkanSDK/<ver>.
if(DEFINED ENV{VULKAN_SDK} AND EXISTS "$ENV{VULKAN_SDK}/Lib/vulkan-1.lib")
  set(VK "$ENV{VULKAN_SDK}")
else()
  file(GLOB _vk_dirs "C:/VulkanSDK/*")
  list(SORT _vk_dirs)
  list(REVERSE _vk_dirs)
  foreach(d ${_vk_dirs})
    if(EXISTS "${d}/Lib/vulkan-1.lib")
      set(VK "${d}")
      break()
    endif()
  endforeach()
endif()
if(NOT DEFINED VK)
  message(FATAL_ERROR "Vulkan SDK not found (set VULKAN_SDK or install under C:/VulkanSDK)")
endif()
message(STATUS "Using Vulkan SDK: ${VK}")

# glew.c provides the GL extension function pointers that PPSSPP's GL backend objects (pulled
# into Common.lib) reference. We never use the GL path, but its objects must still resolve.
# Common.lib references them as dllimport (__imp___glew*), so glew.c is compiled with GLEW_BUILD
# (dllexport) to define those import symbols within this DLL.
add_library(acx_gpu SHARED gpu_bridge.cpp host_stubs.cpp "${PPSSPP}/ext/glew/glew.c")
set_source_files_properties("${PPSSPP}/ext/glew/glew.c" PROPERTIES COMPILE_DEFINITIONS "GLEW_BUILD")

target_include_directories(acx_gpu PRIVATE
  "${PPSSPP}"
  "${PPSSPP}/Common"
  "${PPSSPP}/ext"
  "${PPSSPP}/ext/glew"
  "${PPSSPP}/ext/glslang"
  "${PPSSPP}/ext/snappy"
  "${PPSSPP}/ext/zstd/lib"
  "${VK}/Include"
)

target_compile_definitions(acx_gpu PRIVATE
  USING_WIN_UI _CRT_SECURE_NO_WARNINGS WIN32 _ARCH_64=1 _M_X64=1
  NDEBUG _UNICODE UNICODE _USE_MATH_DEFINES USE_FFMPEG)

# Debug info (PDB) so crashes in the bridge/PPSSPP can be symbolized with llvm-symbolizer.
target_compile_options(acx_gpu PRIVATE /Zi)
target_link_options(acx_gpu PRIVATE /DEBUG /OPT:NOREF /OPT:NOICF)

# PPSSPP module libs + their third-party deps + Vulkan loader + Win32 system libs.
target_link_libraries(acx_gpu PRIVATE
  "${PPLIB}/GPU.lib"
  "${PPLIB}/Common.lib"
  "${PPLIB}/Core.lib"
  "${PPLIB}/glslang.lib"
  "${PPSSPP}/Windows/x64/SPIRV-CrossRelease/SPIRV-Cross.lib"
  "${PPLIB}/zlib.lib"
  "${PPLIB}/libkirk.lib"
  "${PPLIB}/rcheevos.lib"
  "${PPSSPP}/ext/libchdr/bin/x64_Release/libchdr_static.lib"
  "${PPSSPP}/Windows/x64/cpu_featuresRelease/cpu_features.lib"
  "${PPSSPP}/ext/_Output/libarmips64.lib"
  opengl32
  "${PPSSPP}/ext/zstd/bin/x64_Release/libzstd_static.lib"
  "${VK}/Lib/vulkan-1.lib"
  # Optional PPSSPP subsystems dragged in transitively from Core/Common (MediaEngine, Camera,
  # PortManager, naett HTTP, UWP text). The GPU never calls them, but their object files are
  # pulled in, so satisfy their externals with the bundled/system libs.
  "${PPSSPP}/ffmpeg/Windows/x86_64/lib/avcodec.lib"
  "${PPSSPP}/ffmpeg/Windows/x86_64/lib/avformat.lib"
  "${PPSSPP}/ffmpeg/Windows/x86_64/lib/avutil.lib"
  "${PPSSPP}/ffmpeg/Windows/x86_64/lib/swscale.lib"
  "${PPSSPP}/ffmpeg/Windows/x86_64/lib/swresample.lib"
  "${PPLIB}/miniupnpc.lib"
  winhttp d2d1 dwrite d3d11
  ws2_32 winmm shlwapi dwmapi shell32 user32 gdi32 ole32 advapi32 iphlpapi version)
