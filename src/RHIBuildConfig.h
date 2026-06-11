#pragma once

#include <cstdint>

// Build-time switches that decide which platform/vendor libraries are linked
// into the binary. Mobile/Linux ports flip the appropriate macros to 0 at
// configure time. Each macro defaults based on what is statically reasonable
// for the current target so an unmodified Windows build keeps every feature.

#define CORONA_PLATFORM_UNKNOWN 0
#define CORONA_PLATFORM_WINDOWS 1
#define CORONA_PLATFORM_ANDROID 2
#define CORONA_PLATFORM_LINUX 3
#define CORONA_PLATFORM_MACOS 4
#define CORONA_PLATFORM_IOS 5

#ifndef CORONA_TARGET_PLATFORM
#  if defined(_WIN32)
#    define CORONA_TARGET_PLATFORM CORONA_PLATFORM_WINDOWS
#  elif defined(__ANDROID__)
#    define CORONA_TARGET_PLATFORM CORONA_PLATFORM_ANDROID
#  elif defined(__APPLE__) && defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
#    define CORONA_TARGET_PLATFORM CORONA_PLATFORM_IOS
#  elif defined(__APPLE__)
#    define CORONA_TARGET_PLATFORM CORONA_PLATFORM_MACOS
#  elif defined(__linux__)
#    define CORONA_TARGET_PLATFORM CORONA_PLATFORM_LINUX
#  else
#    define CORONA_TARGET_PLATFORM CORONA_PLATFORM_UNKNOWN
#  endif
#endif

#define CORONA_PLATFORM_IS_WINDOWS (CORONA_TARGET_PLATFORM == CORONA_PLATFORM_WINDOWS)
#define CORONA_PLATFORM_IS_ANDROID (CORONA_TARGET_PLATFORM == CORONA_PLATFORM_ANDROID)
#define CORONA_PLATFORM_IS_LINUX (CORONA_TARGET_PLATFORM == CORONA_PLATFORM_LINUX)
#define CORONA_PLATFORM_IS_MACOS (CORONA_TARGET_PLATFORM == CORONA_PLATFORM_MACOS)
#define CORONA_PLATFORM_IS_IOS (CORONA_TARGET_PLATFORM == CORONA_PLATFORM_IOS)
#define CORONA_PLATFORM_DESKTOP (CORONA_PLATFORM_IS_WINDOWS || CORONA_PLATFORM_IS_LINUX || CORONA_PLATFORM_IS_MACOS)
#define CORONA_PLATFORM_MOBILE (CORONA_PLATFORM_IS_ANDROID || CORONA_PLATFORM_IS_IOS)

enum class ECoronaPlatform
{
	Unknown = CORONA_PLATFORM_UNKNOWN,
	Windows = CORONA_PLATFORM_WINDOWS,
	Android = CORONA_PLATFORM_ANDROID,
	Linux = CORONA_PLATFORM_LINUX,
	MacOS = CORONA_PLATFORM_MACOS,
	IOS = CORONA_PLATFORM_IOS,
};

inline constexpr ECoronaPlatform CoronaTargetPlatform =
	static_cast<ECoronaPlatform>(CORONA_TARGET_PLATFORM);

#if !CORONA_PLATFORM_IS_WINDOWS
using UINT = unsigned int;
using UINT8 = std::uint8_t;
using UINT16 = std::uint16_t;
using UINT32 = std::uint32_t;
using UINT64 = std::uint64_t;
using BYTE = std::uint8_t;
using WCHAR = wchar_t;
using LPCWSTR = const wchar_t*;

#ifndef _In_reads_
#define _In_reads_(count)
#endif
#endif

#ifndef CORONA_HAS_D3D12
#  if CORONA_PLATFORM_IS_WINDOWS && !defined(CORONA_DISABLE_D3D12)
#    define CORONA_HAS_D3D12 1
#  else
#    define CORONA_HAS_D3D12 0
#  endif
#endif

// PIX (WinPixEventRuntime) - DX12 debugger marker support.
#ifndef CORONA_HAS_PIX
#  define CORONA_HAS_PIX CORONA_HAS_D3D12
#endif

// DXC runtime shader compile (dxcompiler.dll). Vulkan path consumes prebuilt
// SPIR-V so runtime DXC is a DX12-only concern in this project.
#ifndef CORONA_HAS_DXC_RUNTIME
#  define CORONA_HAS_DXC_RUNTIME CORONA_HAS_D3D12
#endif

// DirectXTex - texture load/save on Windows (HDR/PNG via WIC, format convert).
#ifndef CORONA_HAS_DIRECTXTEX
#  define CORONA_HAS_DIRECTXTEX CORONA_HAS_D3D12
#endif

// Assimp and PhysX are currently wired to checked-in Windows binaries. They
// are feature dependencies, not rendering-API dependencies; mobile ports can
// re-enable them once platform-native packages are provided.
#ifndef CORONA_HAS_ASSIMP
#  define CORONA_HAS_ASSIMP CORONA_PLATFORM_IS_WINDOWS
#endif

#ifndef CORONA_HAS_PHYSX
#  define CORONA_HAS_PHYSX CORONA_PLATFORM_IS_WINDOWS
#endif

// NVIDIA Aftermath GPU crash dump (DX12 only).
#ifndef CORONA_HAS_AFTERMATH
#  define CORONA_HAS_AFTERMATH 0
#endif

// Experimental NVIDIA NRI (NVIDIA Render Interface) RHI backend. A single
// IRenderBackend implementation that targets D3D12 or Vulkan through NRI, with
// the goal of eventually unifying the two hand-written backends. Enabled by the
// CMake option CORONA_WITH_NRI (default OFF) which defines CORONA_HAS_NRI=1 and
// links the vendored NRI library. Default OFF keeps the stock build untouched.
#ifndef CORONA_HAS_NRI
#  define CORONA_HAS_NRI 0
#endif

// NVIDIA Streamline (DLSS RR/SR) is DX12-only. The CMake build also exposes
// WITH_STREAMLINE; force it off when the D3D12 backend itself isn't compiled,
// to keep mobile / Vulkan-only builds from pulling in DX12-specific
// Streamline call sites in Corona.cpp.
#if !CORONA_HAS_D3D12
#  ifdef WITH_STREAMLINE
#    undef WITH_STREAMLINE
#  endif
#  define WITH_STREAMLINE 0
#endif
