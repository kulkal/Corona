						//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************
#pragma once
#include "external/GFSDK_Aftermath/include/GFSDK_Aftermath.h"
#include "DX12Impl.h"


inline void ThrowIfFailed(HRESULT hr, GFSDK_Aftermath_ContextHandle* AMH = nullptr)
{
	if (FAILED(hr))
	{

#if USE_AFTERMATH
		GFSDK_Aftermath_Device_Status st;
		GFSDK_Aftermath_GetDeviceStatus(&st);

		GFSDK_Aftermath_PageFaultInformation pf;
		GFSDK_Aftermath_GetPageFaultInformation(&pf);

		GFSDK_Aftermath_ContextHandle ch;
		GFSDK_Aftermath_ContextData cd;
		GFSDK_Aftermath_GetData(1, AMH, &cd);

		char str[256];
		memcpy(str, cd.markerData, cd.markerSize);
#endif
		throw std::exception();
	}
}

inline void GetAssetsPath(_Out_writes_(pathSize) WCHAR* path, UINT pathSize)
{
	if (path == nullptr)
	{
		throw std::exception();
	}

	DWORD size = GetCurrentDirectoryW(pathSize, path);
	if (size == 0 || size == pathSize)
	{
		// Method failed or path was truncated.
		throw std::exception();
	}
}


inline std::string HrToString(HRESULT hr)
{
	char s_str[64] = {};
	sprintf_s(s_str, "HRESULT of 0x%08X", static_cast<UINT>(hr));
	return std::string(s_str);
}

class HrException : public std::runtime_error
{
public:
	HrException(HRESULT hr) : std::runtime_error(HrToString(hr)), m_hr(hr) {}
	HRESULT Error() const { return m_hr; }
private:
	const HRESULT m_hr;
};
//
//inline HRESULT ReadDataFromFile(LPCWSTR filename, byte** data, UINT* size)
//{
//	using namespace Microsoft::WRL;
//
//	CREATEFILE2_EXTENDED_PARAMETERS extendedParams = {};
//	extendedParams.dwSize = sizeof(CREATEFILE2_EXTENDED_PARAMETERS);
//	extendedParams.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
//	extendedParams.dwFileFlags = FILE_FLAG_SEQUENTIAL_SCAN;
//	extendedParams.dwSecurityQosFlags = SECURITY_ANONYMOUS;
//	extendedParams.lpSecurityAttributes = nullptr;
//	extendedParams.hTemplateFile = nullptr;
//
//	Wrappers::FileHandle file(CreateFile2(filename, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, &extendedParams));
//	if (file.Get() == INVALID_HANDLE_VALUE)
//	{
//		throw std::exception();
//	}
//
//	FILE_STANDARD_INFO fileInfo = {};
//	if (!GetFileInformationByHandleEx(file.Get(), FileStandardInfo, &fileInfo, sizeof(fileInfo)))
//	{
//		throw std::exception();
//	}
//
//	if (fileInfo.EndOfFile.HighPart != 0)
//	{
//		throw std::exception();
//	}
//
//	*data = reinterpret_cast<byte*>(malloc(fileInfo.EndOfFile.LowPart));
//	*size = fileInfo.EndOfFile.LowPart;
//
//	if (!ReadFile(file.Get(), *data, fileInfo.EndOfFile.LowPart, nullptr, nullptr))
//	{
//		throw std::exception();
//	}
//
//	return S_OK;
//}

// Assign a name to the object to aid with debugging.
