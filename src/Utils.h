#pragma once

#include <string>
#include <Windows.h>
#include <d3d12.h>

#include "GFSDK_Aftermath/include/GFSDK_Aftermath.h"


std::wstring AnsiToWString(const char* ansiString);

std::wstring GetDirectoryFromFilePath(const WCHAR* filePath_);

std::wstring GetFileName(const WCHAR* filePath_);

bool FileExists(const WCHAR* filePath);

std::wstring GetFileExtension(const WCHAR* filePath_);

void NVAftermathMarker(GFSDK_Aftermath_ContextHandle ah, std::string markerName);