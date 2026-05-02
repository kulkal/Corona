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

#include "stdafx.h"
#include "Corona.h"
#include "Win32Application.h"
#include <dxgidebug.h>
#include <cwchar>
#include <stdlib.h>

namespace
{
	constexpr DWORD kCrashDialogErrorMode =
		SEM_FAILCRITICALERRORS |
		SEM_NOGPFAULTERRORBOX |
		SEM_NOOPENFILEERRORBOX;

	bool HasCommandLineSwitch(const wchar_t* commandLine, const wchar_t* switchName)
	{
		return commandLine && switchName && std::wcsstr(commandLine, switchName) != nullptr;
	}

	void SuppressCrashReportDialogs()
	{
		SetErrorMode(kCrashDialogErrorMode);

		DWORD previousThreadErrorMode = 0;
		SetThreadErrorMode(kCrashDialogErrorMode, &previousThreadErrorMode);

		_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

		// Avoid linking Wer.lib for one process-wide flag. Logs and dump files are
		// still useful; the blocking report UI is not during automated captures.
		HMODULE werModule = LoadLibraryW(L"wer.dll");
		if (!werModule)
			return;

		using WerSetFlagsFn = HRESULT(WINAPI*)(DWORD);
		auto werSetFlags = reinterpret_cast<WerSetFlagsFn>(GetProcAddress(werModule, "WerSetFlags"));
		if (werSetFlags)
			werSetFlags(32); // WER_FAULT_REPORTING_NO_UI

		FreeLibrary(werModule);
	}
}

_Use_decl_annotations_
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
	const wchar_t* commandLine = GetCommandLineW();
	if (!HasCommandLineSwitch(commandLine, L"--enable-crash-dialog"))
		SuppressCrashReportDialogs();

	//Corona* sample = new Corona(2560, 1440, L"Corona");
	Corona* sample = new Corona(1920, 1080, L"Corona");
	const bool bAutomationRun =
		HasCommandLineSwitch(commandLine, L"--auto-dump") ||
		HasCommandLineSwitch(commandLine, L"--readme-dump") ||
		HasCommandLineSwitch(commandLine, L"--path-tracing-dump") ||
		HasCommandLineSwitch(commandLine, L"--lighting-compare-dump") ||
		HasCommandLineSwitch(commandLine, L"--gi-compare-dump") ||
		HasCommandLineSwitch(commandLine, L"--indirect-compare-dump") ||
		HasCommandLineSwitch(commandLine, L"--camera-path-dump");

	Win32Application::Run(sample, hInstance, nCmdShow);

	delete sample;

	if (bAutomationRun)
	{
		int crtFlags = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
		crtFlags &= ~_CRTDBG_LEAK_CHECK_DF;
		_CrtSetDbgFlag(crtFlags);
	}
	else
	{
		ComPtr<IDXGIDebug1> dxgiDebug;
		if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiDebug))))
		{
			dxgiDebug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_ALL);
		}

		_CrtCheckMemory();
		_CrtDumpMemoryLeaks();
	}

	return 0;
}
