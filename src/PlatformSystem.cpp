#include "stdafx.h"
#include "PlatformSystem.h"

#include "RHIBuildConfig.h"

#include <cstdlib>
#include <codecvt>
#include <locale>
#include <vector>

#if CORONA_PLATFORM_IS_WINDOWS
#include <objbase.h>
#endif

namespace
{
#if CORONA_PLATFORM_IS_WINDOWS
	bool gPlatformRuntimeComInitialized = false;
#endif

	std::wstring FallbackUtf8ToWide(const std::string& value)
	{
		try
		{
			std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
			return converter.from_bytes(value);
		}
		catch (...)
		{
			return std::wstring(value.begin(), value.end());
		}
	}

	std::string FallbackWideToUtf8(const std::wstring& value)
	{
		try
		{
			std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
			return converter.to_bytes(value);
		}
		catch (...)
		{
			std::string result;
			result.reserve(value.size());
			for (wchar_t ch : value)
				result.push_back(ch >= 0 && ch <= 0x7F ? static_cast<char>(ch) : '?');
			return result;
		}
	}
}

void InitializePlatformRuntime()
{
#if CORONA_PLATFORM_IS_WINDOWS
	if (gPlatformRuntimeComInitialized)
		return;

	const HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	gPlatformRuntimeComInitialized = SUCCEEDED(result);
#endif
}

void ShutdownPlatformRuntime()
{
#if CORONA_PLATFORM_IS_WINDOWS
	if (!gPlatformRuntimeComInitialized)
		return;

	CoUninitialize();
	gPlatformRuntimeComInitialized = false;
#endif
}

std::string PlatformWideToUtf8(const std::wstring& value)
{
	if (value.empty())
		return {};

#if CORONA_PLATFORM_IS_WINDOWS
	const int valueLength = static_cast<int>(value.size());
	const int requiredSize = WideCharToMultiByte(CP_UTF8, 0, value.data(), valueLength, nullptr, 0, nullptr, nullptr);
	if (requiredSize <= 0)
		return FallbackWideToUtf8(value);

	std::string result(static_cast<size_t>(requiredSize), '\0');
	WideCharToMultiByte(CP_UTF8, 0, value.data(), valueLength, result.data(), requiredSize, nullptr, nullptr);
	return result;
#else
	return FallbackWideToUtf8(value);
#endif
}

std::wstring PlatformUtf8ToWide(const std::string& value)
{
	if (value.empty())
		return {};

#if CORONA_PLATFORM_IS_WINDOWS
	const int valueLength = static_cast<int>(value.size());
	const int requiredSize = MultiByteToWideChar(CP_UTF8, 0, value.data(), valueLength, nullptr, 0);
	if (requiredSize <= 0)
		return FallbackUtf8ToWide(value);

	std::wstring result(static_cast<size_t>(requiredSize), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, value.data(), valueLength, result.data(), requiredSize);
	return result;
#else
	return FallbackUtf8ToWide(value);
#endif
}

std::optional<std::wstring> GetPlatformEnvironmentVariable(const wchar_t* name)
{
	if (!name || !*name)
		return std::nullopt;

#if CORONA_PLATFORM_IS_WINDOWS
	std::vector<wchar_t> buffer(128);
	for (;;)
	{
		SetLastError(ERROR_SUCCESS);
		const DWORD length = GetEnvironmentVariableW(name, buffer.data(), static_cast<DWORD>(buffer.size()));
		if (length == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND)
			return std::nullopt;
		if (length < buffer.size())
			return std::wstring(buffer.data(), length);
		buffer.resize(static_cast<size_t>(length) + 1u);
	}
#else
	const std::string narrowName = PlatformWideToUtf8(name);
	const char* value = std::getenv(narrowName.c_str());
	if (!value)
		return std::nullopt;
	return PlatformUtf8ToWide(value);
#endif
}

bool SetPlatformEnvironmentVariable(const wchar_t* name, const wchar_t* value)
{
	if (!name || !*name)
		return false;

#if CORONA_PLATFORM_IS_WINDOWS
	return SetEnvironmentVariableW(name, value) != 0;
#else
	const std::string narrowName = PlatformWideToUtf8(name);
	if (!value)
		return unsetenv(narrowName.c_str()) == 0;

	const std::string narrowValue = PlatformWideToUtf8(value);
	return setenv(narrowName.c_str(), narrowValue.c_str(), 1) == 0;
#endif
}

PlatformComScope::PlatformComScope(bool multithreaded)
{
#if CORONA_PLATFORM_IS_WINDOWS
	const HRESULT result = CoInitializeEx(
		nullptr,
		multithreaded ? COINIT_MULTITHREADED : COINIT_APARTMENTTHREADED);
	bShouldUninitialize = SUCCEEDED(result);
#else
	(void)multithreaded;
#endif
}

PlatformComScope::~PlatformComScope()
{
#if CORONA_PLATFORM_IS_WINDOWS
	if (bShouldUninitialize)
		CoUninitialize();
#endif
}
