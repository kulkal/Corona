#pragma once

// Build-time switches that decide which platform/vendor libraries are linked
// into the binary. Mobile/Linux ports flip the appropriate macros to 0 at
// configure time. Each macro defaults based on what is statically reasonable
// for the current target so an unmodified Windows build keeps every feature.

#ifndef CORONA_HAS_D3D12
#  if defined(_WIN32) && !defined(CORONA_DISABLE_D3D12)
#    define CORONA_HAS_D3D12 1
#  else
#    define CORONA_HAS_D3D12 0
#  endif
#endif

// PIX (WinPixEventRuntime) — DX12 debugger marker support.
#ifndef CORONA_HAS_PIX
#  define CORONA_HAS_PIX CORONA_HAS_D3D12
#endif

// DXC runtime shader compile (dxcompiler.dll). Vulkan path consumes prebuilt
// SPIR-V so runtime DXC is a DX12-only concern in this project.
#ifndef CORONA_HAS_DXC_RUNTIME
#  define CORONA_HAS_DXC_RUNTIME CORONA_HAS_D3D12
#endif

// DirectXTex — texture load/save on Windows (HDR/PNG via WIC, format convert).
#ifndef CORONA_HAS_DIRECTXTEX
#  define CORONA_HAS_DIRECTXTEX CORONA_HAS_D3D12
#endif

// NVIDIA Aftermath GPU crash dump (DX12 only).
#ifndef CORONA_HAS_AFTERMATH
#  define CORONA_HAS_AFTERMATH 0
#endif
