#include "Utils.h"


std::wstring AnsiToWString(const char* ansiString)
{
	WCHAR buffer[512];
	MultiByteToWideChar(CP_ACP, 0, ansiString, -1, buffer, 512);
	return std::wstring(buffer);
}

std::wstring GetDirectoryFromFilePath(const WCHAR* filePath_)
{
	std::wstring filePath(filePath_);
	size_t idx = filePath.rfind(L'\\');
	if (idx != std::wstring::npos)
		return filePath.substr(0, idx + 1);
	else
		return std::wstring(L"");
}

// Returns the name of the file given the path (extension included)
std::wstring GetFileName(const WCHAR* filePath_)
{

	std::wstring filePath(filePath_);
	size_t idx = filePath.rfind(L'\\');
	if (idx != std::wstring::npos && idx < filePath.length() - 1)
		return filePath.substr(idx + 1);
	else
	{
		idx = filePath.rfind(L'/');
		if (idx != std::wstring::npos && idx < filePath.length() - 1)
			return filePath.substr(idx + 1);
		else
			return filePath;
	}
}

bool FileExists(const WCHAR* filePath)
{
	if (filePath == NULL)
		return false;

	DWORD fileAttr = GetFileAttributes(filePath);
	if (fileAttr == INVALID_FILE_ATTRIBUTES)
		return false;

	return true;
}

std::wstring GetFileExtension(const WCHAR* filePath_)
{
	std::wstring filePath(filePath_);
	size_t idx = filePath.rfind(L'.');
	if (idx != std::wstring::npos)
		return filePath.substr(idx + 1, filePath.length() - idx - 1);
	else
		return std::wstring(L"");
}
