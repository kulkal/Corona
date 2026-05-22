#include "stdafx.h"
#include "PlatformWindow.h"

#include "RHIBuildConfig.h"

#if CORONA_PLATFORM_IS_WINDOWS
#include "Win32Application.h"
#include "imgui_impl_win32.h"
#include <xinput.h>
#elif CORONA_PLATFORM_IS_ANDROID
#include <android/input.h>
#include <android/native_window.h>
#include "imgui_impl_android.h"
#endif

#include <algorithm>
#include <cstdlib>
#include <mutex>

namespace
{
	void* gMainPlatformWindowHandle = nullptr;

#if CORONA_PLATFORM_IS_ANDROID
	std::mutex gTouchStateMutex;
	PlatformTouchState gTouchState;
	int32_t gLookPointerId = -1;
	float gLookLastX = 0.0f;
	float gLookLastY = 0.0f;
	int32_t gMovePointerId = -1;
	int32_t gAttackPointerId = -1;

	bool GetAndroidWindowSize(float& width, float& height)
	{
		ANativeWindow* window = static_cast<ANativeWindow*>(gMainPlatformWindowHandle);
		if (!window)
			return false;

		const int32_t rawWidth = ANativeWindow_getWidth(window);
		const int32_t rawHeight = ANativeWindow_getHeight(window);
		if (rawWidth <= 0 || rawHeight <= 0)
			return false;

		width = static_cast<float>(rawWidth);
		height = static_cast<float>(rawHeight);
		return true;
	}

	float GetAndroidMoveRegionMaxX()
	{
		float width = 0.0f;
		float height = 0.0f;
		if (!GetAndroidWindowSize(width, height))
			return 0.0f;

		return width * 0.5f;
	}

	bool IsAndroidAttackTouch(float x, float y)
	{
		float width = 0.0f;
		float height = 0.0f;
		if (!GetAndroidWindowSize(width, height))
			return false;

		const float shortEdge = std::max(1.0f, std::min(width, height));
		const float radius = std::clamp(shortEdge * 0.085f, 62.0f, 96.0f);
		const float margin = std::clamp(shortEdge * 0.070f, 56.0f, 92.0f);
		const float centerX = width - margin - radius;
		const float centerY = height - margin - radius;
		const float hitRadius = radius * 1.25f;
		const float dx = x - centerX;
		const float dy = y - centerY;
		return (dx * dx + dy * dy) <= (hitRadius * hitRadius);
	}

	bool IsAndroidMoveTouch(float x)
	{
		const float moveRegionMaxX = GetAndroidMoveRegionMaxX();
		return moveRegionMaxX > 0.0f && x < moveRegionMaxX;
	}

	bool IsAndroidLookTouch(float x)
	{
		const float moveRegionMaxX = GetAndroidMoveRegionMaxX();
		return moveRegionMaxX <= 0.0f || x >= moveRegionMaxX;
	}

	int32_t FindAndroidPointerIndexById(const AInputEvent* event, int32_t pointerId)
	{
		const size_t pointerCount = AMotionEvent_getPointerCount(event);
		for (size_t pointerIndex = 0; pointerIndex < pointerCount; ++pointerIndex)
		{
			if (AMotionEvent_getPointerId(event, pointerIndex) == pointerId)
				return static_cast<int32_t>(pointerIndex);
		}
		return -1;
	}

	void BeginAndroidLookTouch(const AInputEvent* event, int32_t pointerIndex)
	{
		const float x = AMotionEvent_getX(event, pointerIndex);
		if (!IsAndroidLookTouch(x))
			return;

		const float y = AMotionEvent_getY(event, pointerIndex);
		std::lock_guard<std::mutex> lock(gTouchStateMutex);
		gLookPointerId = AMotionEvent_getPointerId(event, pointerIndex);
		gLookLastX = x;
		gLookLastY = y;
		gTouchState.bLookActive = true;
		gTouchState.X = x;
		gTouchState.Y = y;
	}

	void BeginAndroidMoveTouch(const AInputEvent* event, int32_t pointerIndex)
	{
		const float x = AMotionEvent_getX(event, pointerIndex);
		if (!IsAndroidMoveTouch(x))
			return;

		const float y = AMotionEvent_getY(event, pointerIndex);
		std::lock_guard<std::mutex> lock(gTouchStateMutex);
		gMovePointerId = AMotionEvent_getPointerId(event, pointerIndex);
		gTouchState.bMoveActive = true;
		gTouchState.MoveStartX = x;
		gTouchState.MoveStartY = y;
		gTouchState.MoveX = x;
		gTouchState.MoveY = y;
	}

	void BeginAndroidAttackTouch(const AInputEvent* event, int32_t pointerIndex)
	{
		const float x = AMotionEvent_getX(event, pointerIndex);
		const float y = AMotionEvent_getY(event, pointerIndex);
		if (!IsAndroidAttackTouch(x, y))
			return;

		std::lock_guard<std::mutex> lock(gTouchStateMutex);
		gAttackPointerId = AMotionEvent_getPointerId(event, pointerIndex);
		if (!gTouchState.bAttackActive)
			gTouchState.bAttackPressed = true;
		gTouchState.bAttackActive = true;
	}

	void UpdateAndroidTouchState(const AInputEvent* event)
	{
		if (!event || AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION)
			return;

		const int32_t action = AMotionEvent_getAction(event);
		const int32_t actionMasked = action & AMOTION_EVENT_ACTION_MASK;
		const int32_t actionPointerIndex =
			(action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
			AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;

		switch (actionMasked)
		{
		case AMOTION_EVENT_ACTION_DOWN:
		case AMOTION_EVENT_ACTION_POINTER_DOWN:
			{
				bool bAttackTouch = false;
				{
					std::lock_guard<std::mutex> lock(gTouchStateMutex);
					const float x = AMotionEvent_getX(event, actionPointerIndex);
					const float y = AMotionEvent_getY(event, actionPointerIndex);
					bAttackTouch = IsAndroidAttackTouch(x, y);
					if (bAttackTouch)
					{
						if (gAttackPointerId >= 0)
							break;
					}
					else if (IsAndroidMoveTouch(x))
					{
						if (gMovePointerId >= 0)
							break;
					}
					else if (gLookPointerId >= 0)
					{
						break;
					}
				}
				if (bAttackTouch)
				{
					BeginAndroidAttackTouch(event, actionPointerIndex);
					break;
				}
			}
			BeginAndroidMoveTouch(event, actionPointerIndex);
			BeginAndroidLookTouch(event, actionPointerIndex);
			break;

		case AMOTION_EVENT_ACTION_MOVE:
			{
				std::lock_guard<std::mutex> lock(gTouchStateMutex);
				const int32_t lookPointerIndex = FindAndroidPointerIndexById(event, gLookPointerId);
				if (lookPointerIndex >= 0)
				{
					const float x = AMotionEvent_getX(event, lookPointerIndex);
					const float y = AMotionEvent_getY(event, lookPointerIndex);
					gTouchState.DeltaX += x - gLookLastX;
					gTouchState.DeltaY += y - gLookLastY;
					gTouchState.X = x;
					gTouchState.Y = y;
					gTouchState.bLookActive = true;
					gLookLastX = x;
					gLookLastY = y;
				}

				const int32_t movePointerIndex = FindAndroidPointerIndexById(event, gMovePointerId);
				if (movePointerIndex >= 0)
				{
					gTouchState.MoveX = AMotionEvent_getX(event, movePointerIndex);
					gTouchState.MoveY = AMotionEvent_getY(event, movePointerIndex);
					gTouchState.bMoveActive = true;
				}

				const int32_t attackPointerIndex = FindAndroidPointerIndexById(event, gAttackPointerId);
				if (attackPointerIndex >= 0)
				{
					const float x = AMotionEvent_getX(event, attackPointerIndex);
					const float y = AMotionEvent_getY(event, attackPointerIndex);
					gTouchState.bAttackActive = IsAndroidAttackTouch(x, y);
				}
			}
			break;

		case AMOTION_EVENT_ACTION_UP:
		case AMOTION_EVENT_ACTION_POINTER_UP:
		case AMOTION_EVENT_ACTION_CANCEL:
			{
				const int32_t liftedPointerId = AMotionEvent_getPointerId(event, actionPointerIndex);
				std::lock_guard<std::mutex> lock(gTouchStateMutex);
				if (liftedPointerId == gLookPointerId || actionMasked == AMOTION_EVENT_ACTION_CANCEL)
				{
					gLookPointerId = -1;
					gTouchState.bLookActive = false;
				}
				if (liftedPointerId == gMovePointerId || actionMasked == AMOTION_EVENT_ACTION_CANCEL)
				{
					gMovePointerId = -1;
					gTouchState.bMoveActive = false;
					gTouchState.MoveStartX = 0.0f;
					gTouchState.MoveStartY = 0.0f;
					gTouchState.MoveX = 0.0f;
					gTouchState.MoveY = 0.0f;
				}
				if (liftedPointerId == gAttackPointerId || actionMasked == AMOTION_EVENT_ACTION_CANCEL)
				{
					gAttackPointerId = -1;
					if (gTouchState.bAttackActive)
						gTouchState.bAttackReleased = true;
					gTouchState.bAttackActive = false;
				}
			}
			break;

		default:
			break;
		}
	}
#endif

#if CORONA_PLATFORM_IS_WINDOWS
	using XInputGetStateProc = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

	XInputGetStateProc GetXInputGetStateProc()
	{
		static bool bTriedLoad = false;
		static HMODULE module = nullptr;
		static XInputGetStateProc getState = nullptr;
		if (bTriedLoad)
			return getState;

		bTriedLoad = true;
		const char* dllNames[] =
		{
			"xinput1_4.dll",
			"xinput1_3.dll",
			"xinput9_1_0.dll",
		};
		for (const char* dllName : dllNames)
		{
			module = LoadLibraryA(dllName);
			if (module)
			{
				getState = reinterpret_cast<XInputGetStateProc>(GetProcAddress(module, "XInputGetState"));
				if (getState)
					break;
				FreeLibrary(module);
				module = nullptr;
			}
		}
		return getState;
	}

	float NormalizeGamepadStick(SHORT value, SHORT deadZone)
	{
		const int magnitude = std::abs(static_cast<int>(value));
		if (magnitude <= deadZone)
			return 0.0f;

		const float normalized = static_cast<float>(magnitude - deadZone) / static_cast<float>(32767 - deadZone);
		return std::clamp(normalized, 0.0f, 1.0f) * (value < 0 ? -1.0f : 1.0f);
	}

	float NormalizeGamepadTrigger(BYTE value)
	{
		if (value <= XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
			return 0.0f;
		const float normalized =
			static_cast<float>(value - XINPUT_GAMEPAD_TRIGGER_THRESHOLD) /
			static_cast<float>(255 - XINPUT_GAMEPAD_TRIGGER_THRESHOLD);
		return std::clamp(normalized, 0.0f, 1.0f);
	}

	UINT ToWin32MessageBoxButtons(EPlatformDialogButtons buttons)
	{
		switch (buttons)
		{
		case EPlatformDialogButtons::YesNo:
			return MB_YESNO;
		case EPlatformDialogButtons::YesNoCancel:
			return MB_YESNOCANCEL;
		default:
			return MB_OK;
		}
	}

	UINT ToWin32MessageBoxIcon(EPlatformDialogIcon icon)
	{
		switch (icon)
		{
		case EPlatformDialogIcon::Info:
			return MB_ICONINFORMATION;
		case EPlatformDialogIcon::Warning:
			return MB_ICONWARNING;
		case EPlatformDialogIcon::Error:
			return MB_ICONERROR;
		case EPlatformDialogIcon::Question:
			return MB_ICONQUESTION;
		default:
			return 0;
		}
	}

	UINT ToWin32MessageBoxDefaultButton(EPlatformDialogDefaultButton defaultButton)
	{
		switch (defaultButton)
		{
		case EPlatformDialogDefaultButton::Button2:
			return MB_DEFBUTTON2;
		case EPlatformDialogDefaultButton::Button3:
			return MB_DEFBUTTON3;
		default:
			return MB_DEFBUTTON1;
		}
	}

	EPlatformDialogResult FromWin32MessageBoxResult(int result)
	{
		switch (result)
		{
		case IDOK:
			return EPlatformDialogResult::Ok;
		case IDYES:
			return EPlatformDialogResult::Yes;
		case IDNO:
			return EPlatformDialogResult::No;
		case IDCANCEL:
			return EPlatformDialogResult::Cancel;
		default:
			return EPlatformDialogResult::None;
		}
	}
#endif

	EPlatformDialogResult DefaultDialogResult(EPlatformDialogButtons buttons, EPlatformDialogDefaultButton defaultButton)
	{
		if (buttons == EPlatformDialogButtons::Ok)
			return EPlatformDialogResult::Ok;
		if (defaultButton == EPlatformDialogDefaultButton::Button1)
			return EPlatformDialogResult::Yes;
		if (defaultButton == EPlatformDialogDefaultButton::Button2)
			return EPlatformDialogResult::No;
		return buttons == EPlatformDialogButtons::YesNoCancel
			? EPlatformDialogResult::Cancel
			: EPlatformDialogResult::No;
	}
}

void SetMainPlatformWindowHandle(void* platformHandle)
{
	gMainPlatformWindowHandle = platformHandle;
}

WindowHandle GetMainPlatformWindowHandle()
{
#if CORONA_PLATFORM_IS_WINDOWS
	return WindowHandle{ Win32Application::GetHwnd() };
#else
	return WindowHandle{ gMainPlatformWindowHandle };
#endif
}

void SetMainPlatformWindowTitle(const std::wstring& title)
{
#if CORONA_PLATFORM_IS_WINDOWS
	if (HWND hwnd = Win32Application::GetHwnd())
		SetWindowTextW(hwnd, title.c_str());
#else
	(void)title;
#endif
}

bool RequestMainPlatformWindowClose()
{
#if CORONA_PLATFORM_IS_WINDOWS
	if (HWND hwnd = Win32Application::GetHwnd())
	{
		PostMessage(hwnd, WM_CLOSE, 0, 0);
		return true;
	}
#endif
	return false;
}

void QuitPlatformApplication(int exitCode)
{
#if CORONA_PLATFORM_IS_WINDOWS
	PostQuitMessage(exitCode);
#else
	(void)exitCode;
#endif
}

void PumpPlatformWindowMessages()
{
#if CORONA_PLATFORM_IS_WINDOWS
	MSG msg = {};
	while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
	{
		if (msg.message == WM_QUIT)
		{
			QuitPlatformApplication(static_cast<int>(msg.wParam));
			break;
		}
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
#endif
}

bool InitializePlatformImGuiBackend(WindowHandle window)
{
#if CORONA_PLATFORM_IS_WINDOWS
	return ImGui_ImplWin32_Init(window.PlatformHandle);
#elif CORONA_PLATFORM_IS_ANDROID
	return ImGui_ImplAndroid_Init(static_cast<ANativeWindow*>(window.PlatformHandle));
#else
	(void)window;
	return false;
#endif
}

void NewPlatformImGuiFrame()
{
#if CORONA_PLATFORM_IS_WINDOWS
	ImGui_ImplWin32_NewFrame();
#elif CORONA_PLATFORM_IS_ANDROID
	ImGui_ImplAndroid_NewFrame();
#endif
}

void ShutdownPlatformImGuiBackend()
{
#if CORONA_PLATFORM_IS_WINDOWS
	ImGui_ImplWin32_Shutdown();
#elif CORONA_PLATFORM_IS_ANDROID
	ImGui_ImplAndroid_Shutdown();
#endif
}

bool PollMainPlatformMouseState(PlatformMouseState& state)
{
	state = PlatformMouseState{};
#if CORONA_PLATFORM_IS_WINDOWS
	HWND hwnd = Win32Application::GetHwnd();
	if (!hwnd)
		return false;

	state.bWindowCanReceiveMouse =
		GetForegroundWindow() == hwnd ||
		GetCapture() == hwnd;
	if (!state.bWindowCanReceiveMouse)
		return true;

	POINT cursorPosition = {};
	if (!GetCursorPos(&cursorPosition))
		return false;
	if (!ScreenToClient(hwnd, &cursorPosition))
		return false;

	state.bRightButtonDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
	if (state.bRightButtonDown && GetCapture() == nullptr)
		SetCapture(hwnd);
	else if (!state.bRightButtonDown && GetCapture() == hwnd)
		ReleaseCapture();

	state.X = cursorPosition.x;
	state.Y = cursorPosition.y;
	return true;
#else
	return true;
#endif
}

bool PollMainPlatformGamepadState(PlatformGamepadState& state)
{
	state = PlatformGamepadState{};
#if CORONA_PLATFORM_IS_WINDOWS
	HWND hwnd = Win32Application::GetHwnd();
	if (!hwnd)
		return true;

	const bool bWindowCanReceiveInput =
		GetForegroundWindow() == hwnd ||
		GetCapture() == hwnd;
	if (!bWindowCanReceiveInput)
		return true;

	XInputGetStateProc getState = GetXInputGetStateProc();
	if (!getState)
		return true;

	XINPUT_STATE xinputState = {};
	if (getState(0, &xinputState) != ERROR_SUCCESS)
		return true;

	const XINPUT_GAMEPAD& pad = xinputState.Gamepad;
	state.bConnected = true;
	state.LeftX = NormalizeGamepadStick(pad.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
	state.LeftY = NormalizeGamepadStick(pad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
	state.RightX = NormalizeGamepadStick(pad.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
	state.RightY = NormalizeGamepadStick(pad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
	state.LeftTrigger = NormalizeGamepadTrigger(pad.bLeftTrigger);
	state.RightTrigger = NormalizeGamepadTrigger(pad.bRightTrigger);
	state.Buttons = static_cast<uint16_t>(pad.wButtons);
#endif
	return true;
}

bool PollMainPlatformTouchState(PlatformTouchState& state)
{
	state = PlatformTouchState{};
#if CORONA_PLATFORM_IS_ANDROID
	std::lock_guard<std::mutex> lock(gTouchStateMutex);
	state = gTouchState;
	gTouchState.DeltaX = 0.0f;
	gTouchState.DeltaY = 0.0f;
	gTouchState.bAttackPressed = false;
	gTouchState.bAttackReleased = false;
#endif
	return true;
}

int32_t HandlePlatformInputEvent(const void* platformInputEvent)
{
#if CORONA_PLATFORM_IS_ANDROID
	const AInputEvent* inputEvent = static_cast<const AInputEvent*>(platformInputEvent);
	if (!inputEvent)
		return 0;

	UpdateAndroidTouchState(inputEvent);
	const int32_t imguiHandled =
		ImGui::GetCurrentContext() ? ImGui_ImplAndroid_HandleInputEvent(inputEvent) : 0;
	return imguiHandled != 0 ? imguiHandled : 1;
#else
	(void)platformInputEvent;
	return 0;
#endif
}

EPlatformDialogResult ShowPlatformMessageBox(
	const std::wstring& message,
	const std::wstring& title,
	EPlatformDialogButtons buttons,
	EPlatformDialogIcon icon,
	EPlatformDialogDefaultButton defaultButton)
{
#if CORONA_PLATFORM_IS_WINDOWS
	const UINT flags =
		ToWin32MessageBoxButtons(buttons) |
		ToWin32MessageBoxIcon(icon) |
		ToWin32MessageBoxDefaultButton(defaultButton);
	return FromWin32MessageBoxResult(MessageBoxW(
		Win32Application::GetHwnd(),
		message.c_str(),
		title.c_str(),
		flags));
#else
	(void)message;
	(void)title;
	(void)icon;
	return DefaultDialogResult(buttons, defaultButton);
#endif
}
