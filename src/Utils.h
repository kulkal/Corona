#pragma once

#include <filesystem>
#include <string>
#include <Windows.h>
#include <d3d12.h>

#include "AftermathConfig.h"
#include "external/GFSDK_Aftermath/include/GFSDK_Aftermath.h"


std::wstring AnsiToWString(const char* ansiString);

std::wstring GetDirectoryFromFilePath(const WCHAR* filePath_);

std::wstring GetFileName(const WCHAR* filePath_);

bool FileExists(const WCHAR* filePath);

std::wstring GetFileExtension(const WCHAR* filePath_);

void NVAftermathMarker(GFSDK_Aftermath_ContextHandle ah, std::string markerName);

namespace RuntimePaths
{
	std::filesystem::path RootDirectory();
	std::filesystem::path SourceDirectory();
	std::filesystem::path ConfigDirectory();
	std::filesystem::path LogDirectory();
	std::filesystem::path DumpDirectory();
	std::filesystem::path SourceFile(const wchar_t* relativePath);
	std::filesystem::path ConfigFile(const wchar_t* fileName);
	std::filesystem::path LogFile(const wchar_t* fileName);
}
