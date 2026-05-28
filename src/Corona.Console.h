#pragma once

// Quake-style in-engine console with natural-language → 3D asset dispatcher.
// Bound to the backtick/tilde key. The first slash-command supported is
// `generate <image_path>` which shells out to the TripoSR Python wrapper
// (tools/tripo_gen.py) and, when the .obj is ready, loads it into the
// scene in front of the active camera via Corona's existing LoadModel +
// AddCenteredSceneObject path.

#include <chrono>
#include <deque>
#include <future>
#include <memory>
#include <string>
#include <vector>

class Corona;

class CoronaConsole
{
public:
	explicit CoronaConsole(Corona* host);
	~CoronaConsole();

	// User pressed backtick/tilde. Toggles the console panel and refocuses
	// the input box on the next ImGui frame.
	void OnKeyToggle();
	bool IsVisible() const { return bVisible; }

	// Call once per frame inside the existing ImGui pass (after NewFrame).
	// The console renders its own scrollback + input widget.
	void RenderImGui();

	// Call once per frame on the render thread (LoadModel + scene-object
	// creation are render-thread-only). Polls async TripoSR jobs and, when
	// their .obj is ready, loads and spawns the asset.
	void Update();

	// Programmatic submission — useful for tests / hotkeys.
	void Submit(const std::string& cmd);

private:
	struct PendingJob
	{
		std::future<std::string> Result;
		std::string Prompt;
		std::chrono::steady_clock::time_point Started;
	};

	void DispatchTripoSR(const std::string& imagePath);
	void LoadAndPlaceObj(const std::string& objPath, const std::string& prompt);
	void Log(const std::string& line);

	Corona* Host;
	bool bVisible = false;
	bool bRequestFocus = false;
	std::deque<std::string> History;          // scrollback log (system + commands)
	char InputBuf[1024] = {};
	std::vector<std::unique_ptr<PendingJob>> Pending;

	// Command history (shell-style ↑/↓ recall). Most recent at the back.
	// HistoryCursor == -1 means the user is editing a fresh command;
	// 0..size-1 indexes into CommandHistory from oldest to newest. DraftBuf
	// stores whatever was being typed when ↑ was first pressed so ↓ back
	// past the newest entry restores it.
	std::deque<std::string> CommandHistory;
	int                     HistoryCursor = -1;
	std::string             DraftBuf;

	// Forward-typed via void* so the header doesn't pull in imgui.h.
	int HandleInputCallback(void* dataPtr);
};
