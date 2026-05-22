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

// stdafx.h : include file for standard system include files,
// or project specific include files that are used frequently, but
// are changed infrequently.

#pragma once

#include "RHIBuildConfig.h"

#ifndef GLM_FORCE_CTOR_INIT
#define GLM_FORCE_CTOR_INIT
#endif
#ifndef GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#endif

#include <cwchar>
#include <cwctype>
#include <cstdint>
#include <chrono>
#include <string>
#include <ctime>
#include <vector>

#if CORONA_PLATFORM_IS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers.
#endif

#include <windows.h>
#include <shellapi.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <DirectXMath.h>
#include "d3dx12.h"
#include <wrl.h>
#else
using BYTE = uint8_t;
using FLOAT = float;
using UINT = unsigned int;
using UINT32 = uint32_t;
using UINT64 = uint64_t;
using WCHAR = wchar_t;
using LPCWSTR = const wchar_t*;

struct SYSTEMTIME
{
	uint16_t wYear = 0;
	uint16_t wMonth = 0;
	uint16_t wDayOfWeek = 0;
	uint16_t wDay = 0;
	uint16_t wHour = 0;
	uint16_t wMinute = 0;
	uint16_t wSecond = 0;
	uint16_t wMilliseconds = 0;
};

inline void GetLocalTime(SYSTEMTIME* outLocalTime)
{
	if (!outLocalTime)
		return;

	const auto now = std::chrono::system_clock::now();
	const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
	const std::time_t timeNow = std::chrono::system_clock::to_time_t(now);
	std::tm localTime = {};
	if (const std::tm* tmValue = std::localtime(&timeNow))
		localTime = *tmValue;

	outLocalTime->wYear = static_cast<uint16_t>(localTime.tm_year + 1900);
	outLocalTime->wMonth = static_cast<uint16_t>(localTime.tm_mon + 1);
	outLocalTime->wDayOfWeek = static_cast<uint16_t>(localTime.tm_wday);
	outLocalTime->wDay = static_cast<uint16_t>(localTime.tm_mday);
	outLocalTime->wHour = static_cast<uint16_t>(localTime.tm_hour);
	outLocalTime->wMinute = static_cast<uint16_t>(localTime.tm_min);
	outLocalTime->wSecond = static_cast<uint16_t>(localTime.tm_sec);
	outLocalTime->wMilliseconds = static_cast<uint16_t>(millis < 0 ? millis + 1000 : millis);
}

struct XMFLOAT2
{
	float x;
	float y;
};

struct XMFLOAT4
{
	float x;
	float y;
	float z;
	float w;
};

struct D3D12_SUBRESOURCE_DATA
{
	const void* pData = nullptr;
	int64_t RowPitch = 0;
	int64_t SlicePitch = 0;
};

inline int _wcsicmp(const wchar_t* lhs, const wchar_t* rhs)
{
	if (!lhs || !rhs)
		return lhs == rhs ? 0 : (lhs ? 1 : -1);

	while (*lhs && *rhs)
	{
		const wchar_t a = static_cast<wchar_t>(std::towlower(*lhs));
		const wchar_t b = static_cast<wchar_t>(std::towlower(*rhs));
		if (a != b)
			return a < b ? -1 : 1;
		++lhs;
		++rhs;
	}
	if (*lhs == *rhs)
		return 0;
	return *lhs ? 1 : -1;
}

inline int _wcsnicmp(const wchar_t* lhs, const wchar_t* rhs, size_t count)
{
	if (count == 0)
		return 0;
	if (!lhs || !rhs)
		return lhs == rhs ? 0 : (lhs ? 1 : -1);

	for (size_t i = 0; i < count; ++i)
	{
		const wchar_t a = static_cast<wchar_t>(std::towlower(lhs[i]));
		const wchar_t b = static_cast<wchar_t>(std::towlower(rhs[i]));
		if (a != b)
			return a < b ? -1 : 1;
		if (lhs[i] == L'\0' || rhs[i] == L'\0')
			return 0;
	}
	return 0;
}

#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU 0xA4
#define VK_RMENU 0xA5
#define VK_F5 0x74

#ifndef _In_reads_
#define _In_reads_(x)
#endif

#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif
#endif
