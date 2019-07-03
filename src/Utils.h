#pragma once

#include <string>
#include <Windows.h>

std::wstring AnsiToWString(const char* ansiString);

std::wstring GetDirectoryFromFilePath(const WCHAR* filePath_);

std::wstring GetFileName(const WCHAR* filePath_);

bool FileExists(const WCHAR* filePath);

std::wstring GetFileExtension(const WCHAR* filePath_);
