#pragma once

#include <cstdint>
#include <string>

#include "RenderBackend.h"

enum class EPlatformDialogButtons
{
	Ok,
	YesNo,
	YesNoCancel,
};

enum class EPlatformDialogDefaultButton
{
	Button1,
	Button2,
	Button3,
};

enum class EPlatformDialogIcon
{
	None,
	Info,
	Warning,
	Error,
	Question,
};

enum class EPlatformDialogResult
{
	None,
	Ok,
	Yes,
	No,
	Cancel,
};

struct PlatformMouseState
{
	bool bWindowCanReceiveMouse = false;
	bool bRightButtonDown = false;
	int X = 0;
	int Y = 0;
};

struct PlatformGamepadState
{
	bool bConnected = false;
	float LeftX = 0.0f;
	float LeftY = 0.0f;
	float RightX = 0.0f;
	float RightY = 0.0f;
	float LeftTrigger = 0.0f;
	float RightTrigger = 0.0f;
	uint16_t Buttons = 0;
};

struct PlatformTouchState
{
	bool bLookActive = false;
	float X = 0.0f;
	float Y = 0.0f;
	float DeltaX = 0.0f;
	float DeltaY = 0.0f;
	bool bMoveActive = false;
	float MoveStartX = 0.0f;
	float MoveStartY = 0.0f;
	float MoveX = 0.0f;
	float MoveY = 0.0f;
	bool bAttackActive = false;
	bool bAttackPressed = false;
	bool bAttackReleased = false;
};

namespace PlatformGamepadButton
{
	inline constexpr uint16_t DPadUp = 0x0001;
	inline constexpr uint16_t DPadDown = 0x0002;
	inline constexpr uint16_t DPadLeft = 0x0004;
	inline constexpr uint16_t DPadRight = 0x0008;
	inline constexpr uint16_t Start = 0x0010;
	inline constexpr uint16_t Back = 0x0020;
	inline constexpr uint16_t LeftThumb = 0x0040;
	inline constexpr uint16_t RightThumb = 0x0080;
	inline constexpr uint16_t LeftShoulder = 0x0100;
	inline constexpr uint16_t RightShoulder = 0x0200;
	inline constexpr uint16_t A = 0x1000;
	inline constexpr uint16_t B = 0x2000;
	inline constexpr uint16_t X = 0x4000;
	inline constexpr uint16_t Y = 0x8000;
}

void SetMainPlatformWindowHandle(void* platformHandle);
WindowHandle GetMainPlatformWindowHandle();
void SetMainPlatformWindowTitle(const std::wstring& title);
bool RequestMainPlatformWindowClose();
void QuitPlatformApplication(int exitCode = 0);
void PumpPlatformWindowMessages();
bool InitializePlatformImGuiBackend(WindowHandle window);
void NewPlatformImGuiFrame();
void ShutdownPlatformImGuiBackend();
bool PollMainPlatformMouseState(PlatformMouseState& state);
bool PollMainPlatformGamepadState(PlatformGamepadState& state);
bool PollMainPlatformTouchState(PlatformTouchState& state);
int32_t HandlePlatformInputEvent(const void* platformInputEvent);
EPlatformDialogResult ShowPlatformMessageBox(
	const std::wstring& message,
	const std::wstring& title,
	EPlatformDialogButtons buttons,
	EPlatformDialogIcon icon = EPlatformDialogIcon::None,
	EPlatformDialogDefaultButton defaultButton = EPlatformDialogDefaultButton::Button1);
