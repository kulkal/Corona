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
#include <windowsx.h>
#include <cwchar>
#include <filesystem>
#include <fstream>

namespace
{
	constexpr COLORREF kStartupBackgroundColor = RGB(6, 9, 13);

	HBRUSH GetStartupBackgroundBrush()
	{
		static HBRUSH brush = CreateSolidBrush(kStartupBackgroundColor);
		return brush;
	}

	void AppendStartupTrace(const std::wstring& line)
	{
		const std::filesystem::path tracePath = RuntimePaths::LogFile(L"vulkan_runtime_trace.log");
		std::filesystem::create_directories(tracePath.parent_path());
		std::wofstream traceFile(tracePath, std::ios::app);
		if (traceFile.is_open())
			traceFile << line << L"\n";
	}

	void FillStartupBackground(HWND hWnd, HDC hdc)
	{
		RECT clientRect = {};
		GetClientRect(hWnd, &clientRect);
		FillRect(hdc, &clientRect, GetStartupBackgroundBrush());
	}
}

HWND Win32Application::m_hwnd = nullptr;
bool Win32Application::m_appInitialized = false;

int Win32Application::Run(Corona* app, HINSTANCE hInstance, int nCmdShow)
{
	AppendStartupTrace(L"[Run] enter");
	// Parse the command line parameters
	int argc;
	LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	bool bCommandLineAutoDump = false;
	for (int argIndex = 1; argIndex < argc; ++argIndex)
	{
		if (std::wcscmp(argv[argIndex], L"--auto-dump") == 0 ||
			std::wcscmp(argv[argIndex], L"--readme-dump") == 0 ||
			std::wcscmp(argv[argIndex], L"--path-tracing-dump") == 0 ||
			std::wcscmp(argv[argIndex], L"--lighting-compare-dump") == 0 ||
			std::wcscmp(argv[argIndex], L"--gi-compare-dump") == 0 ||
			std::wcscmp(argv[argIndex], L"--indirect-compare-dump") == 0)
		{
			bCommandLineAutoDump = true;
			break;
		}
	}
	app->ParseCommandLineArgs(argv, argc);
	LocalFree(argv);
	AppendStartupTrace(L"[Run] after ParseCommandLineArgs");
	m_appInitialized = false;

	// Initialize the window class.
	WNDCLASSEXW windowClass = { 0 };
	windowClass.cbSize = sizeof(WNDCLASSEX);
	windowClass.style = CS_HREDRAW | CS_VREDRAW;
	windowClass.lpfnWndProc = WindowProc;
	windowClass.hInstance = hInstance;
	windowClass.hCursor = LoadCursor(NULL, IDC_ARROW);
	windowClass.hbrBackground = GetStartupBackgroundBrush();
	windowClass.lpszClassName = L"CoronaWindowClass";
	RegisterClassExW(&windowClass);

	RECT windowRect = { 0, 0, static_cast<LONG>(app->GetWidth()), static_cast<LONG>(app->GetHeight()) };
	AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);

	// Create the window and store a handle to it.
	m_hwnd = CreateWindowW(
		windowClass.lpszClassName,
		app->GetTitle(),
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		windowRect.right - windowRect.left,
		windowRect.bottom - windowRect.top,
		nullptr,		// We have no parent window.
		nullptr,		// We aren't using menus.
		hInstance,
		app);
	AppendStartupTrace(m_hwnd ? L"[Run] after CreateWindowW ok" : L"[Run] after CreateWindowW failed");

	const int effectiveCmdShow = (!bCommandLineAutoDump && nCmdShow == SW_HIDE) ? SW_SHOWNORMAL : nCmdShow;
	if (!bCommandLineAutoDump && effectiveCmdShow != SW_HIDE && m_hwnd)
	{
		ShowWindow(m_hwnd, effectiveCmdShow);
		SetWindowPos(m_hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
		SetForegroundWindow(m_hwnd);
		UpdateWindow(m_hwnd);
		AppendStartupTrace(
			L"[Run] pre-init ShowWindow requested=" + std::to_wstring(nCmdShow) +
			L", effective=" + std::to_wstring(effectiveCmdShow) +
			L", visible=" + std::to_wstring(IsWindowVisible(m_hwnd) ? 1 : 0));
	}

	AppendStartupTrace(L"[Run] before OnInit");
	app->OnInit();
	m_appInitialized = true;
	app->StartGameThread();
	AppendStartupTrace(L"[Run] after OnInit");

	ShowWindow(m_hwnd, effectiveCmdShow);
	if (!bCommandLineAutoDump && effectiveCmdShow != SW_HIDE)
	{
		SetWindowPos(m_hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
		SetForegroundWindow(m_hwnd);
	}
	UpdateWindow(m_hwnd);
	AppendStartupTrace(
		L"[Run] after ShowWindow requested=" + std::to_wstring(nCmdShow) +
		L", effective=" + std::to_wstring(effectiveCmdShow) +
		L", visible=" + std::to_wstring(IsWindowVisible(m_hwnd) ? 1 : 0));

	// Main sample loop.
	MSG msg = {};
	while (msg.message != WM_QUIT)
	{
		// Process any messages in the queue.
		if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		else if (app)
		{
			app->RenderThreadTick();
		}
	}

	AppendStartupTrace(
		L"[Run] message loop exit message=" + std::to_wstring(msg.message) +
		L", wParam=" + std::to_wstring(static_cast<int>(msg.wParam)));
	if (app)
		app->StopGameThread();
	AppendStartupTrace(L"[Run] before OnDestroy");
	app->OnDestroy();
	AppendStartupTrace(L"[Run] after OnDestroy");

	// Return this part of the WM_QUIT message to Windows.
	return static_cast<char>(msg.wParam);
}

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Main message handler for the sample.
LRESULT CALLBACK Win32Application::WindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	Corona* app = reinterpret_cast<Corona*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));

	if (ImGui_ImplWin32_WndProcHandler(hWnd, message, wParam, lParam))
		return true;

	switch (message)
	{
	case WM_CREATE:
		{
			AppendStartupTrace(L"[WindowProc] WM_CREATE");
			LPCREATESTRUCT pCreateStruct = reinterpret_cast<LPCREATESTRUCT>(lParam);
			SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pCreateStruct->lpCreateParams));
		}
		return 0;

	case WM_KEYDOWN:
		if (app)
		{
			app->OnKeyDown(static_cast<UINT8>(wParam));
		}
		return 0;

	case WM_KEYUP:
		if (app)
		{
			app->OnKeyUp(static_cast<UINT8>(wParam));
		}
		return 0;

	case WM_RBUTTONDOWN:
		if (app)
		{
			SetCapture(hWnd);
			int xPos = GET_X_LPARAM(lParam);
			int yPos = GET_Y_LPARAM(lParam);
			app->OnRButtonDown(xPos, yPos);
		}
		return 0;

	case WM_RBUTTONUP:
		if (GetCapture() == hWnd)
			ReleaseCapture();
		if (app)
		{
			app->OnRButtonUp();
		}
		return 0;

	case WM_MOUSEMOVE:
		if (app)
		{
			int xPos = GET_X_LPARAM(lParam);
			int yPos = GET_Y_LPARAM(lParam);
			app->OnMouseMove(xPos, yPos);
		}
		return 0;

	case WM_ERASEBKGND:
		FillStartupBackground(hWnd, reinterpret_cast<HDC>(wParam));
		return 1;

	case WM_PAINT:
		if (app && app->IsStartupLoadingScreenActive())
		{
			PAINTSTRUCT paint;
			HDC hdc = BeginPaint(hWnd, &paint);
			FillStartupBackground(hWnd, hdc);
			EndPaint(hWnd, &paint);
			app->DrawStartupLoadingScreen();
		}
		else if (app && m_appInitialized)
		{
			app->RenderThreadTick();
		}
		else
		{
			PAINTSTRUCT paint;
			HDC hdc = BeginPaint(hWnd, &paint);
			FillStartupBackground(hWnd, hdc);
			EndPaint(hWnd, &paint);
		}
		return 0;

	case WM_CLOSE:
		AppendStartupTrace(L"[WindowProc] WM_CLOSE");
		break;

	case WM_DESTROY:
		AppendStartupTrace(L"[WindowProc] WM_DESTROY");
		PostQuitMessage(0);
		return 0;
	}

	// Handle any messages the switch statement didn't.
	return DefWindowProc(hWnd, message, wParam, lParam);
}

#endif
