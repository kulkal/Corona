#pragma once

#include <filesystem>
#include <string>

#include "AftermathConfig.h"
#include "RHIBuildConfig.h"


std::wstring AnsiToWString(const char* ansiString);

std::wstring GetDirectoryFromFilePath(const wchar_t* filePath_);

std::wstring GetFileName(const wchar_t* filePath_);

bool FileExists(const wchar_t* filePath);

std::wstring GetFileExtension(const wchar_t* filePath_);

#if USE_AFTERMATH && CORONA_HAS_D3D12
#ifndef GFSDK_Aftermath_H
struct GFSDK_Aftermath_ContextHandle__;
using GFSDK_Aftermath_ContextHandle = GFSDK_Aftermath_ContextHandle__*;
#endif
void NVAftermathMarker(GFSDK_Aftermath_ContextHandle ah, std::string markerName);
#endif

namespace RuntimePaths
{
	std::filesystem::path RootDirectory();
	std::filesystem::path SourceDirectory();
	std::filesystem::path AssetDirectory();
	std::filesystem::path ConfigDirectory();
	std::filesystem::path LogDirectory();
	std::filesystem::path DumpDirectory();
	std::filesystem::path SourceFile(const wchar_t* relativePath);
	std::filesystem::path ConfigFile(const wchar_t* fileName);
	std::filesystem::path LogFile(const wchar_t* fileName);
}
