#pragma once

#include <optional>
#include <string>

void InitializePlatformRuntime();
void ShutdownPlatformRuntime();

std::string PlatformWideToUtf8(const std::wstring& value);
std::wstring PlatformUtf8ToWide(const std::string& value);
std::optional<std::wstring> GetPlatformEnvironmentVariable(const wchar_t* name);
bool SetPlatformEnvironmentVariable(const wchar_t* name, const wchar_t* value);

class PlatformComScope
{
public:
	explicit PlatformComScope(bool multithreaded = false);
	~PlatformComScope();

	PlatformComScope(const PlatformComScope&) = delete;
	PlatformComScope& operator=(const PlatformComScope&) = delete;

private:
	bool bShouldUninitialize = false;
};
