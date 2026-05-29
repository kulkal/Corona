#pragma once

// Quake-style in-engine console with natural-language → 3D asset dispatcher.
// Bound to the backtick/tilde key. Commands:
//   generate <image>   — shell out to tools/tripo_gen.py (TripoSR)
//   genmotion "<text>" — shell out to tools/motion_gen.py (LLM motion)
//   loadmodel <path>   — direct mesh import
//   savemap/loadmap    — map serialization
//
// Subprocess outputs end with RESULT_OBJ=<path> or RESULT_BVH=<path>;
// the console parses that line, then either loads the .obj into the scene
// or feeds the BVH to the motion playback system.

#include <chrono>
#include <deque>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "Corona.MotionClip.h"

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
	enum class JobKind
	{
		TripoMesh,   // RESULT_OBJ=<path>, loaded as a mesh
		LlmMotion,   // RESULT_BVH=<path>, loaded as a motion clip
	};

	struct PendingJob
	{
		std::future<std::string> Result;
		std::string Prompt;
		std::chrono::steady_clock::time_point Started;
		JobKind     Kind = JobKind::TripoMesh;
	};

	void DispatchTripoSR(const std::string& imagePath);
	void DispatchMotionGen(const std::string& promptText, float durationSec, int seed);
	void LoadAndPlaceObj(const std::string& objPath, const std::string& prompt);
	void LoadAndPlayMotion(const std::string& bvhPath, const std::string& prompt);
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
