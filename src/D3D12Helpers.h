#pragma once

#include "AftermathConfig.h"
#include "RHIBuildConfig.h"

#if CORONA_HAS_D3D12

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <windows.h>
#include <d3d12.h>

#include "external/GFSDK_Aftermath/include/GFSDK_Aftermath.h"

inline void ThrowIfFailed(HRESULT hr, GFSDK_Aftermath_ContextHandle* aftermathContext = nullptr)
{
	if (FAILED(hr))
	{
#if USE_AFTERMATH
		if (aftermathContext)
		{
			GFSDK_Aftermath_Device_Status status;
			GFSDK_Aftermath_GetDeviceStatus(&status);

			GFSDK_Aftermath_PageFaultInformation pageFaultInfo;
			GFSDK_Aftermath_GetPageFaultInformation(&pageFaultInfo);

			GFSDK_Aftermath_ContextData contextData{};
			GFSDK_Aftermath_GetData(1, aftermathContext, &contextData);

			char marker[256] = {};
			const uint32_t markerSize = std::min<uint32_t>(
				contextData.markerSize,
				static_cast<uint32_t>(sizeof(marker) - 1));
			if (contextData.markerData && markerSize > 0)
			{
				memcpy(marker, contextData.markerData, markerSize);
				OutputDebugStringA(marker);
				OutputDebugStringA("\n");
			}
		}
#else
		(void)aftermathContext;
#endif
		throw std::exception();
	}
}

inline void SetName(ID3D12Object* object, LPCWSTR name)
{
	if (object)
		object->SetName(name);
}

inline void SetNameIndexed(ID3D12Object* object, LPCWSTR name, UINT index)
{
	WCHAR fullName[50];
	if (object && swprintf_s(fullName, L"%s[%u]", name, index) > 0)
		object->SetName(fullName);
}

#define NAME_D3D12_OBJECT(x) SetName(x.Get(), L#x)
#define NAME_D3D12_OBJECT_INDEXED(x, n) SetNameIndexed(x[n].Get(), L#x, n)

#else // CORONA_HAS_D3D12

// In non-DX12 builds these become no-ops; the macro argument is never
// evaluated, so renderer code touching DX12-only members (texture->resource,
// etc.) inside the call still compiles.
#define NAME_D3D12_OBJECT(x) ((void)0)
#define NAME_D3D12_OBJECT_INDEXED(x, n) ((void)0)

#endif // CORONA_HAS_D3D12
