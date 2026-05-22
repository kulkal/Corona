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
#include "RHIBuildConfig.h"

#if CORONA_PLATFORM_IS_WINDOWS

#include "Corona.h"
#include "Win32Application.h"
#include "Utils.h"
#include <DbgHelp.h>
#if CORONA_HAS_D3D12
#include <dxgidebug.h>
#endif
#include <cwchar>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdlib.h>

#pragma comment(lib, "Dbghelp.lib")

#if CORONA_D3D12_AGILITY_ENABLED
extern "C"
{
	__declspec(dllexport) extern const unsigned int D3D12SDKVersion = CORONA_D3D12_AGILITY_SDK_VERSION;
	__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}
#endif

namespace
{
	constexpr DWORD kCrashDialogErrorMode =
		SEM_FAILCRITICALERRORS |
		SEM_NOGPFAULTERRORBOX |
		SEM_NOOPENFILEERRORBOX;

	std::wstring FormatHex(uint64_t value)
	{
		std::wstringstream stream;
		stream << L"0x" << std::hex << std::uppercase << value;
		return stream.str();
	}

	void AppendCrashTrace(const std::wstring& line)
	{
		const std::filesystem::path tracePath = RuntimePaths::LogFile(L"crash_trace.log");
		std::filesystem::create_directories(tracePath.parent_path());
		std::wofstream traceFile(tracePath, std::ios::app);
		if (traceFile.is_open())
			traceFile << line << L"\n";
	}

	std::wstring GetModulePathForAddress(void* address)
	{
		MEMORY_BASIC_INFORMATION memoryInfo = {};
		if (!address || VirtualQuery(address, &memoryInfo, sizeof(memoryInfo)) == 0 || !memoryInfo.AllocationBase)
			return L"<unknown module>";

		std::wstring modulePath(MAX_PATH, L'\0');
		const DWORD length = GetModuleFileNameW(
			reinterpret_cast<HMODULE>(memoryInfo.AllocationBase),
			modulePath.data(),
			static_cast<DWORD>(modulePath.size()));
		if (length == 0)
			return L"<unknown module>";

		modulePath.resize(length);
		return modulePath;
	}

	void AppendExceptionDetails(EXCEPTION_POINTERS* exceptionPointers)
	{
		if (!exceptionPointers || !exceptionPointers->ExceptionRecord)
			return;

		const EXCEPTION_RECORD* record = exceptionPointers->ExceptionRecord;
		AppendCrashTrace(L"[Crash] module=" + GetModulePathForAddress(record->ExceptionAddress));

		if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
		{
			const ULONG_PTR accessType = record->ExceptionInformation[0];
			const ULONG_PTR accessAddress = record->ExceptionInformation[1];
			const wchar_t* accessName =
				accessType == 0 ? L"read" :
				accessType == 1 ? L"write" :
				accessType == 8 ? L"execute" :
				L"unknown";
			AppendCrashTrace(
				L"[Crash] access=" + std::wstring(accessName) +
				L", target=" + FormatHex(static_cast<uint64_t>(accessAddress)) +
				L", targetModule=" + GetModulePathForAddress(reinterpret_cast<void*>(accessAddress)));
		}
	}

	void AppendStackTrace(EXCEPTION_POINTERS* exceptionPointers)
	{
		if (!exceptionPointers || !exceptionPointers->ContextRecord)
			return;

		HANDLE process = GetCurrentProcess();
		HANDLE thread = GetCurrentThread();
		SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
		static bool bSymbolsInitialized = SymInitialize(process, nullptr, TRUE) == TRUE;
		if (!bSymbolsInitialized)
		{
			AppendCrashTrace(L"[Crash] SymInitialize failed");
			return;
		}

		CONTEXT context = *exceptionPointers->ContextRecord;
		STACKFRAME64 frame = {};
		DWORD machineType = 0;
#if defined(_M_X64)
		machineType = IMAGE_FILE_MACHINE_AMD64;
		frame.AddrPC.Offset = context.Rip;
		frame.AddrFrame.Offset = context.Rbp;
		frame.AddrStack.Offset = context.Rsp;
#elif defined(_M_IX86)
		machineType = IMAGE_FILE_MACHINE_I386;
		frame.AddrPC.Offset = context.Eip;
		frame.AddrFrame.Offset = context.Ebp;
		frame.AddrStack.Offset = context.Esp;
#else
		AppendCrashTrace(L"[Crash] stack trace unsupported architecture");
		return;
#endif
		frame.AddrPC.Mode = AddrModeFlat;
		frame.AddrFrame.Mode = AddrModeFlat;
		frame.AddrStack.Mode = AddrModeFlat;

		AppendCrashTrace(L"[Crash] stack:");
		for (int frameIndex = 0; frameIndex < 64; ++frameIndex)
		{
			const BOOL walked = StackWalk64(
				machineType,
				process,
				thread,
				&frame,
				&context,
				nullptr,
				SymFunctionTableAccess64,
				SymGetModuleBase64,
				nullptr);
			if (!walked || frame.AddrPC.Offset == 0)
				break;

			DWORD64 displacement = 0;
			char symbolStorage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
			SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolStorage);
			symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
			symbol->MaxNameLen = MAX_SYM_NAME;

			std::wstring symbolText = L"<unknown>";
			if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol))
			{
				symbolText = AnsiToWString(symbol->Name);
				if (displacement != 0)
					symbolText += L"+" + FormatHex(displacement);
			}

			DWORD lineDisplacement = 0;
			IMAGEHLP_LINE64 line = {};
			line.SizeOfStruct = sizeof(line);
			std::wstring lineText;
			if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisplacement, &line) && line.FileName)
			{
				lineText =
					L" (" + AnsiToWString(line.FileName) +
					L":" + std::to_wstring(line.LineNumber) + L")";
			}

			AppendCrashTrace(
				L"  #" + std::to_wstring(frameIndex) +
				L" " + FormatHex(frame.AddrPC.Offset) +
				L" " + symbolText +
				lineText +
				L" [" + GetModulePathForAddress(reinterpret_cast<void*>(frame.AddrPC.Offset)) + L"]");
		}
	}

	std::filesystem::path BuildCrashDumpPath(DWORD processId)
	{
		SYSTEMTIME localTime = {};
		GetLocalTime(&localTime);

		std::wstringstream fileName;
		fileName
			<< L"CoronaCrash_"
			<< std::setfill(L'0')
			<< std::setw(4) << localTime.wYear
			<< std::setw(2) << localTime.wMonth
			<< std::setw(2) << localTime.wDay
			<< L"_"
			<< std::setw(2) << localTime.wHour
			<< std::setw(2) << localTime.wMinute
			<< std::setw(2) << localTime.wSecond
			<< L"_pid" << processId
			<< L".dmp";

		return RuntimePaths::DumpDirectory() / L"crash" / fileName.str();
	}

	void WriteCrashDump(EXCEPTION_POINTERS* exceptionPointers)
	{
		const DWORD processId = GetCurrentProcessId();
		const std::filesystem::path dumpPath = BuildCrashDumpPath(processId);
		std::filesystem::create_directories(dumpPath.parent_path());

		HANDLE dumpFile = CreateFileW(
			dumpPath.c_str(),
			GENERIC_WRITE,
			0,
			nullptr,
			CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL,
			nullptr);
		if (dumpFile == INVALID_HANDLE_VALUE)
		{
			AppendCrashTrace(L"[Crash] failed to create dump file: " + dumpPath.wstring());
			return;
		}

		MINIDUMP_EXCEPTION_INFORMATION exceptionInfo = {};
		exceptionInfo.ThreadId = GetCurrentThreadId();
		exceptionInfo.ExceptionPointers = exceptionPointers;
		exceptionInfo.ClientPointers = FALSE;

		const MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(
			MiniDumpWithDataSegs |
			MiniDumpWithHandleData |
			MiniDumpWithThreadInfo |
			MiniDumpWithUnloadedModules);
		const BOOL dumpOk = MiniDumpWriteDump(
			GetCurrentProcess(),
			processId,
			dumpFile,
			dumpType,
			exceptionPointers ? &exceptionInfo : nullptr,
			nullptr,
			nullptr);
		CloseHandle(dumpFile);

		AppendCrashTrace(
			std::wstring(L"[Crash] dump ") +
			(dumpOk ? L"written: " : L"failed: ") +
			dumpPath.wstring());
	}

	LONG WINAPI HandleUnhandledException(EXCEPTION_POINTERS* exceptionPointers)
	{
		DWORD exceptionCode = 0;
		void* exceptionAddress = nullptr;
		if (exceptionPointers && exceptionPointers->ExceptionRecord)
		{
			exceptionCode = exceptionPointers->ExceptionRecord->ExceptionCode;
			exceptionAddress = exceptionPointers->ExceptionRecord->ExceptionAddress;
		}

		AppendCrashTrace(
			L"[Crash] unhandled exception code=" + FormatHex(exceptionCode) +
			L", address=" + FormatHex(reinterpret_cast<uintptr_t>(exceptionAddress)) +
			L", thread=" + std::to_wstring(GetCurrentThreadId()));
		AppendExceptionDetails(exceptionPointers);
		AppendStackTrace(exceptionPointers);
		WriteCrashDump(exceptionPointers);
		return EXCEPTION_EXECUTE_HANDLER;
	}

	void HandleTerminate()
	{
		AppendCrashTrace(
			L"[Crash] std::terminate on thread=" + std::to_wstring(GetCurrentThreadId()));
		WriteCrashDump(nullptr);
		ExitProcess(3);
	}

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
	SetUnhandledExceptionFilter(HandleUnhandledException);
	std::set_terminate(HandleTerminate);

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
#if CORONA_HAS_D3D12
		ComPtr<IDXGIDebug1> dxgiDebug;
		if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiDebug))))
		{
			dxgiDebug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_ALL);
		}
#endif

		_CrtCheckMemory();
		_CrtDumpMemoryLeaks();
	}

	return 0;
}

#endif
