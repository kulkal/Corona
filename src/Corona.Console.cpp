#include "stdafx.h"
#include "Corona.Console.h"

#include "Corona.h"
#include "Corona.MotionClip.h"
#include "TerrainComponent.h"
#include "imgui.h"
#include "PlatformSystem.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sstream>

#include "Utils.h"

namespace
{
	// The TripoSR install. Configurable via the TRIPOSR_REPO env var picked up
	// by the Python wrapper; the Corona side hard-codes the same default so the
	// console works out-of-the-box without env setup.
	const char* kTripoVenvPython = R"(D:\llm\tripoSR\venv\Scripts\python.exe)";
	const char* kTripoWrapperRel = "tools/tripo_gen.py";
	const char* kTripoOutSubdir  = "assets/generated";
	const char* kTripoHfCache    = R"(D:\llm\tripoSR\hf_cache)";

	// LLM motion generation (tools/motion_gen.py). Mock mode is stdlib-only
	// Python so we use the py launcher rather than a dedicated venv. When the
	// momask backend lands it may want its own interpreter; keep this as a
	// single configuration point.
	const char* kMotionPython    = "py -3";
	const char* kMotionWrapperRel = "tools/motion_gen.py";
	const char* kMotionOutSubdir  = "_motions";
	const char* kMotionMode       = "mock";

	std::string Trim(const std::string& s)
	{
		size_t a = s.find_first_not_of(" \t\r\n");
		if (a == std::string::npos)
			return std::string();
		size_t b = s.find_last_not_of(" \t\r\n");
		return s.substr(a, b - a + 1);
	}

	std::string RunPythonCaptureStdout(const std::string& command)
	{
		// _popen captures the child's stdout. stderr is left attached to the
		// parent so TripoSR's progress lines surface in the engine's debug
		// console if it's running with one.
		FILE* pipe = _popen(command.c_str(), "r");
		if (!pipe)
			return std::string();
		std::string out;
		char buf[1024];
		while (fgets(buf, sizeof(buf), pipe))
			out.append(buf);
		_pclose(pipe);
		return out;
	}

	// Locate the last "RESULT_<KEY>=<path>" line in captured stdout. Used by
	// both the TripoSR (KEY=OBJ) and motion (KEY=BVH) dispatchers.
	std::string ExtractResult(const std::string& stdoutText, const std::string& key)
	{
		std::istringstream iss(stdoutText);
		std::string line;
		std::string lastResult;
		while (std::getline(iss, line))
		{
			while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
				line.pop_back();
			if (line.rfind(key, 0) == 0)
				lastResult = line.substr(key.size());
		}
		return lastResult;
	}

	std::string ExtractResultObj(const std::string& stdoutText)
	{
		return ExtractResult(stdoutText, "RESULT_OBJ=");
	}

	std::string ExtractResultBvh(const std::string& stdoutText)
	{
		return ExtractResult(stdoutText, "RESULT_BVH=");
	}
}

CoronaConsole::CoronaConsole(Corona* host) : Host(host) {}
CoronaConsole::~CoronaConsole() = default;

void CoronaConsole::OnKeyToggle()
{
	bVisible = !bVisible;
	if (bVisible)
		bRequestFocus = true;
}

namespace
{
	// Static list of known commands. Tab autocomplete searches this for the
	// first-token completion. Extend here when adding new commands in Submit().
	const char* const kKnownCommands[] = {
		"generate",
		"genmotion",
		"loadmodel",
		"savemap",
		"loadmap",
	};

	std::vector<std::string> ListMapNames()
	{
		std::vector<std::string> out;
		std::error_code ec;
		const auto dir = RuntimePaths::RootDirectory() / L"assets" / L"maps";
		if (!std::filesystem::exists(dir, ec))
			return out;
		for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
		{
			if (!entry.is_regular_file())
				continue;
			out.push_back(entry.path().stem().string());
		}
		std::sort(out.begin(), out.end());
		return out;
	}

	std::string LongestCommonPrefix(const std::vector<std::string>& items)
	{
		if (items.empty())
			return std::string();
		std::string common = items[0];
		for (size_t i = 1; i < items.size(); ++i)
		{
			size_t j = 0;
			while (j < common.size() && j < items[i].size() && common[j] == items[i][j])
				++j;
			common.resize(j);
			if (common.empty())
				break;
		}
		return common;
	}
}

int CoronaConsole::HandleInputCallback(void* dataPtr)
{
	auto* data = static_cast<ImGuiInputTextCallbackData*>(dataPtr);
	if (!data)
		return 0;

	switch (data->EventFlag)
	{
	case ImGuiInputTextFlags_CallbackCharFilter:
		// Drop the toggle key so pressing ~ to open the console doesn't
		// also leak a '~' character into the prompt.
		if (data->EventChar == L'~' || data->EventChar == L'`')
			return 1;
		return 0;

	case ImGuiInputTextFlags_CallbackCompletion:
	{
		const std::string buf(data->Buf, data->BufTextLen);
		const size_t firstSpace = buf.find(' ');
		std::vector<std::string> candidates;
		size_t prefixStart = 0;
		std::string prefix;
		bool completingCommand = false;

		if (firstSpace == std::string::npos)
		{
			// Completing the command (first token).
			prefix = buf;
			completingCommand = true;
			for (const char* cmd : kKnownCommands)
			{
				const std::string s = cmd;
				if (s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0)
					candidates.push_back(s);
			}
		}
		else
		{
			// Completing an argument. Context = first token.
			const std::string cmd  = buf.substr(0, firstSpace);
			std::string arg = Trim(buf.substr(firstSpace + 1));
			prefixStart = firstSpace + 1;
			while (prefixStart < buf.size() && buf[prefixStart] == ' ')
				++prefixStart;
			prefix = buf.substr(prefixStart);

			if (cmd == "loadmap")
			{
				for (const auto& m : ListMapNames())
				{
					if (m.size() >= prefix.size() && m.compare(0, prefix.size(), prefix) == 0)
						candidates.push_back(m);
				}
			}
			// generate / loadmodel: filesystem path completion is heavier
			// to do well; leave as user-typed for now.
		}

		if (candidates.empty())
			return 0;
		if (candidates.size() == 1)
		{
			std::string repl = candidates[0];
			if (completingCommand)
				repl += " ";
			data->DeleteChars(static_cast<int>(prefixStart), data->BufTextLen - static_cast<int>(prefixStart));
			data->InsertChars(static_cast<int>(prefixStart), repl.c_str());
		}
		else
		{
			// Extend to the longest common prefix among matches, then list
			// the remaining candidates in the scrollback so the user can
			// see what they're picking between.
			const std::string common = LongestCommonPrefix(candidates);
			if (common.size() > prefix.size())
			{
				data->DeleteChars(static_cast<int>(prefixStart), data->BufTextLen - static_cast<int>(prefixStart));
				data->InsertChars(static_cast<int>(prefixStart), common.c_str());
			}
			std::string line = "candidates:";
			for (const auto& c : candidates)
				line += " " + c;
			Log(line);
		}
		return 0;
	}

	case ImGuiInputTextFlags_CallbackHistory:
	{
		const int historySize = static_cast<int>(CommandHistory.size());
		const int prev = HistoryCursor;

		if (data->EventKey == ImGuiKey_UpArrow)
		{
			if (historySize == 0)
				return 0;
			if (HistoryCursor == -1)
			{
				// Entering history: stash the in-progress draft so ↓ back
				// past the newest entry can restore it.
				DraftBuf.assign(data->Buf, data->BufTextLen);
				HistoryCursor = historySize - 1;
			}
			else if (HistoryCursor > 0)
			{
				--HistoryCursor;
			}
		}
		else if (data->EventKey == ImGuiKey_DownArrow)
		{
			if (HistoryCursor == -1)
				return 0;
			if (HistoryCursor + 1 < historySize)
				++HistoryCursor;
			else
				HistoryCursor = -1; // back to the live draft
		}
		else
		{
			return 0;
		}

		if (HistoryCursor != prev)
		{
			const std::string& replacement = (HistoryCursor == -1)
				? DraftBuf
				: CommandHistory[static_cast<size_t>(HistoryCursor)];
			data->DeleteChars(0, data->BufTextLen);
			data->InsertChars(0, replacement.c_str());
		}
		return 0;
	}

	default:
		return 0;
	}
}

void CoronaConsole::Log(const std::string& line)
{
	History.push_back(line);
	while (History.size() > 256)
		History.pop_front();
}

void CoronaConsole::Submit(const std::string& cmd)
{
	const std::string trimmed = Trim(cmd);
	if (trimmed.empty())
		return;

	// Command-history bookkeeping: dedupe consecutive duplicates so spamming
	// Enter on the same prompt doesn't bloat the deque. Cap to 200 entries.
	if (CommandHistory.empty() || CommandHistory.back() != trimmed)
	{
		CommandHistory.push_back(trimmed);
		while (CommandHistory.size() > 200)
			CommandHistory.pop_front();
	}
	HistoryCursor = -1;
	DraftBuf.clear();

	Log("> " + trimmed);

	// Command grammar (v0): `generate <image_path>` OR a bare path that ends
	// with a recognized image extension. Anything else: log as unknown.
	auto endsWithCi = [](const std::string& s, const std::string& suffix)
	{
		if (s.size() < suffix.size())
			return false;
		for (size_t i = 0; i < suffix.size(); ++i)
		{
			if (std::tolower(static_cast<unsigned char>(s[s.size() - suffix.size() + i])) !=
				std::tolower(static_cast<unsigned char>(suffix[i])))
				return false;
		}
		return true;
	};
	auto isImagePath = [&](const std::string& s)
	{
		return endsWithCi(s, ".png") || endsWithCi(s, ".jpg") || endsWithCi(s, ".jpeg") || endsWithCi(s, ".bmp");
	};

	// savemap [name] / loadmap <name> — map serialization
	const std::string saveMapPrefix = "savemap";
	const std::string loadMapPrefix = "loadmap";
	auto consumePrefix = [&](const std::string& prefix) -> std::string
	{
		if (trimmed.size() < prefix.size() || trimmed.compare(0, prefix.size(), prefix) != 0)
			return std::string();
		if (trimmed.size() == prefix.size())
			return std::string("__bare__");
		if (trimmed[prefix.size()] != ' ' && trimmed[prefix.size()] != '\t')
			return std::string();
		return Trim(trimmed.substr(prefix.size()));
	};
	{
		std::string saveArg = consumePrefix(saveMapPrefix);
		if (!saveArg.empty())
		{
			std::wstring name;
			if (saveArg == "__bare__")
			{
				// No name given → overwrite the currently-loaded map.
				name = Host->GetCurrentMapName();
				if (name.empty())
				{
					Log("savemap: no map loaded yet — pass a name (`savemap <name>`) to create one.");
					return;
				}
			}
			else
			{
				name.assign(saveArg.begin(), saveArg.end());
			}
			std::wstring err;
			if (!Host->SaveMapToFile(name, &err))
				Log("savemap FAILED: " + std::string(err.begin(), err.end()));
			else
				Log("savemap OK: " + std::string(name.begin(), name.end()));
			return;
		}
	}
	{
		std::string loadArg = consumePrefix(loadMapPrefix);
		if (!loadArg.empty())
		{
			if (loadArg == "__bare__")
			{
				Log("usage: loadmap <name>");
				return;
			}
			const std::wstring name(loadArg.begin(), loadArg.end());
			std::wstring err;
			if (!Host->LoadMapFromFile(name, &err))
				Log("loadmap FAILED: " + std::string(err.begin(), err.end()));
			else
				Log("loadmap OK: " + std::string(name.begin(), name.end()));
			return;
		}
	}

	// loadmodel <path> — direct asset import (any format Assimp accepts:
	// OBJ/FBX/glTF/glb/DAE/PLY/STL/3DS/X). Skips the TripoSR step and
	// places the result in front of the camera the same way `generate` does.
	{
		std::string arg = consumePrefix(std::string("loadmodel"));
		if (!arg.empty())
		{
			if (arg == "__bare__")
			{
				Log("usage: loadmodel <model_path>");
				return;
			}
			std::string modelPath = arg;
			if (modelPath.size() >= 2 && modelPath.front() == '"' && modelPath.back() == '"')
				modelPath = modelPath.substr(1, modelPath.size() - 2);
			if (!std::filesystem::exists(modelPath))
			{
				Log("model not found: " + modelPath);
				return;
			}
			Log("loading " + modelPath + " ...");
			LoadAndPlaceObj(modelPath, modelPath);
			return;
		}
	}

	// genmotion <natural-language prompt> [duration_sec] [seed]
	// Dispatches tools/motion_gen.py to synth a BVH on the SMPL 24-joint
	// skeleton. Defaults: duration 3.0s, seed 0.
	{
		std::string arg = consumePrefix(std::string("genmotion"));
		if (!arg.empty())
		{
			if (arg == "__bare__")
			{
				Log("usage: genmotion <text> [duration_sec] [seed]");
				return;
			}
			// Optional trailing numeric tokens become duration / seed. Walk
			// tokens from the right so we don't accidentally eat words out of
			// the prompt; only convert when the rightmost token parses as a
			// pure number.
			float duration = 3.0f;
			int   seed     = 0;
			std::string promptText = arg;

			auto stripTrailingNumber = [](std::string& s, float& outNum) -> bool
			{
				size_t end = s.find_last_not_of(" \t");
				if (end == std::string::npos) return false;
				size_t start = s.find_last_of(" \t", end);
				start = (start == std::string::npos) ? 0 : start + 1;
				const std::string tail = s.substr(start, end - start + 1);
				try { size_t consumed; outNum = std::stof(tail, &consumed);
					  if (consumed != tail.size()) return false; }
				catch (...) { return false; }
				s = (start == 0) ? std::string() : Trim(s.substr(0, start));
				return true;
			};

			// Try seed first (rightmost), then duration.
			float numBuf = 0.0f;
			if (stripTrailingNumber(promptText, numBuf))
			{
				seed = static_cast<int>(numBuf);
				if (stripTrailingNumber(promptText, numBuf))
					duration = numBuf;
				else
				{
					// Single trailing number — treat it as duration, not seed.
					duration = numBuf;
					seed = 0;
				}
			}

			// Strip surrounding quotes if user typed them.
			if (promptText.size() >= 2 && promptText.front() == '"' && promptText.back() == '"')
				promptText = promptText.substr(1, promptText.size() - 2);
			if (promptText.empty())
			{
				Log("usage: genmotion <text> [duration_sec] [seed]");
				return;
			}
			DispatchMotionGen(promptText, duration, seed);
			return;
		}
	}

	std::string imagePath;
	const std::string genPrefix = "generate ";
	if (trimmed.rfind(genPrefix, 0) == 0)
		imagePath = Trim(trimmed.substr(genPrefix.size()));
	else if (isImagePath(trimmed))
		imagePath = trimmed;

	if (imagePath.empty())
	{
		Log("unknown command. usage: generate <image_path> | genmotion <text> [duration] [seed] | loadmodel <model_path> | savemap [name] | loadmap <name>");
		return;
	}

	// Strip optional surrounding quotes (Windows users often paste these).
	if (imagePath.size() >= 2 && imagePath.front() == '"' && imagePath.back() == '"')
		imagePath = imagePath.substr(1, imagePath.size() - 2);

	if (!std::filesystem::exists(imagePath))
	{
		Log("image not found: " + imagePath);
		return;
	}

	DispatchTripoSR(imagePath);
}

void CoronaConsole::DispatchTripoSR(const std::string& imagePath)
{
	const std::filesystem::path repoRoot = RuntimePaths::RootDirectory();
	const std::filesystem::path wrapper  = repoRoot / kTripoWrapperRel;
	const std::filesystem::path outDir   = repoRoot / kTripoOutSubdir;

	if (!std::filesystem::exists(wrapper))
	{
		Log("wrapper not found: " + wrapper.string());
		return;
	}
	std::error_code ec;
	std::filesystem::create_directories(outDir, ec);

	// Python wrapper sets HF_HOME via os.environ.setdefault so we don't
	// need a `cmd /c set ... &&` chain (which complicates quoting on
	// Windows and was masking real errors). Redirect stderr to stdout so
	// failures from import / argparse / TripoSR surface in our log.
	std::ostringstream cmd;
	cmd << "\"\"" << kTripoVenvPython << "\" "
	    << "\"" << wrapper.string() << "\" "
	    << "--image \"" << imagePath << "\" "
	    << "--output-dir \"" << outDir.string() << "\" 2>&1\"";
	const std::string cmdStr = cmd.str();

	Log("dispatching TripoSR ... (model load ~6s, inference ~5s)");

	auto job = std::make_unique<PendingJob>();
	job->Prompt = imagePath;
	job->Started = std::chrono::steady_clock::now();
	job->Kind = JobKind::TripoMesh;
	job->Result = std::async(std::launch::async, [cmdStr]() -> std::string
	{
		const std::string output = RunPythonCaptureStdout(cmdStr);
		const std::string objPath = ExtractResultObj(output);
		// On failure, return the raw captured output (prefixed) so the
		// UI thread can dump it into the log. RESULT_OBJ lines never
		// start with this prefix so it's safe as a sentinel.
		if (objPath.empty())
			return std::string("ERROR_OUTPUT\n") + output;
		return objPath;
	});
	Pending.push_back(std::move(job));
}

void CoronaConsole::DispatchMotionGen(const std::string& promptText, float durationSec, int seed)
{
	const std::filesystem::path repoRoot = RuntimePaths::RootDirectory();
	const std::filesystem::path wrapper  = repoRoot / kMotionWrapperRel;
	const std::filesystem::path outDir   = repoRoot / kMotionOutSubdir;

	if (!std::filesystem::exists(wrapper))
	{
		Log("wrapper not found: " + wrapper.string());
		return;
	}
	std::error_code ec;
	std::filesystem::create_directories(outDir, ec);

	// Escape embedded double-quotes in the prompt so the cmd line stays
	// well-formed. Backslash-escape for cmd.exe.
	std::string safePrompt = promptText;
	for (size_t i = 0; i < safePrompt.size(); ++i)
	{
		if (safePrompt[i] == '"')
		{
			safePrompt.insert(i, "\\");
			++i;
		}
	}

	// `py -3` is the Python launcher with an arg, not a quoted single path,
	// so we don't use the doubled-quote trick that DispatchTripoSR needs for
	// its venv interpreter path. _popen runs through cmd.exe which parses
	// this fine as long as each path-with-spaces is individually quoted.
	std::ostringstream cmd;
	cmd << kMotionPython << " "
	    << "\"" << wrapper.string() << "\" "
	    << "--text \"" << safePrompt << "\" "
	    << "--output-dir \"" << outDir.string() << "\" "
	    << "--duration " << durationSec << " "
	    << "--seed " << seed << " "
	    << "--mode " << kMotionMode << " 2>&1";
	const std::string cmdStr = cmd.str();

	Log("dispatching motion_gen (mode=" + std::string(kMotionMode)
		+ ", duration=" + std::to_string(durationSec) + "s, seed=" + std::to_string(seed) + ") ...");

	auto job = std::make_unique<PendingJob>();
	job->Prompt = promptText;
	job->Started = std::chrono::steady_clock::now();
	job->Kind = JobKind::LlmMotion;
	job->Result = std::async(std::launch::async, [cmdStr]() -> std::string
	{
		const std::string output = RunPythonCaptureStdout(cmdStr);
		const std::string bvhPath = ExtractResultBvh(output);
		if (bvhPath.empty())
			return std::string("ERROR_OUTPUT\n") + output;
		return bvhPath;
	});
	Pending.push_back(std::move(job));
}

void CoronaConsole::Update()
{
	for (auto it = Pending.begin(); it != Pending.end();)
	{
		auto& job = **it;
		if (job.Result.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		{
			++it;
			continue;
		}

		const std::string result = job.Result.get();
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - job.Started).count();

		const std::string kErrPrefix = "ERROR_OUTPUT\n";
		if (result.rfind(kErrPrefix, 0) == 0)
		{
			Log("FAILED (" + std::to_string(elapsed) + "ms). subprocess output:");
			const std::string body = result.substr(kErrPrefix.size());
			std::istringstream iss(body);
			std::string line;
			while (std::getline(iss, line))
			{
				while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
					line.pop_back();
				if (!line.empty())
					Log("  | " + line);
			}
		}
		else
		{
			Log("OK   (" + std::to_string(elapsed) + "ms) " + result);
			switch (job.Kind)
			{
			case JobKind::TripoMesh:
				LoadAndPlaceObj(result, job.Prompt);
				break;
			case JobKind::LlmMotion:
				LoadAndPlayMotion(result, job.Prompt);
				break;
			}
		}
		it = Pending.erase(it);
	}
}

void CoronaConsole::LoadAndPlayMotion(const std::string& bvhPath, const std::string& prompt)
{
	CoronaMotion::MotionClip clip;
	std::string err;
	if (!CoronaMotion::LoadBVH(bvhPath, clip, err))
	{
		Log("BVH load FAILED: " + err);
		return;
	}
	{
		std::ostringstream os;
		os << "loaded clip '" << prompt << "': "
			<< clip.FrameCount << " frames @ " << clip.Fps << " fps ("
			<< (clip.FrameCount * clip.FrameTime) << "s)";
		Log(os.str());
	}
	// Park the clip on the console for now. The motion playback system
	// (Step 6) reads from here and drives a procedural SMPL character.
	ActiveMotionClip = std::move(clip);
	bActiveMotionClipValid = true;
}

void CoronaConsole::LoadAndPlaceObj(const std::string& objPath, const std::string& /*prompt*/)
{
	// Route through the script-asset path so the spawn is fully tracked:
	//  · ScriptSceneEntry (+ Recipe = Asset/<path>) → savemap picks it up
	//  · MeshComponent on a real ECS Entity → shows up in scene inspector
	//  · SceneObject via AddMeshComponentForScript → render + raycast
	const std::wstring wpath = PlatformUtf8ToWide(objPath);
	const Corona::ScriptSceneHandle sceneHandle = Host->LoadSceneForScript(wpath);
	if (sceneHandle == Corona::InvalidScriptSceneHandle)
	{
		Log("LoadSceneForScript failed for " + objPath);
		return;
	}

	constexpr float kTargetExtent = 1.5f;
	constexpr float kHalfExtent   = kTargetExtent * 0.5f;
	constexpr float kSpawnFwdDist = 4.0f;
	constexpr float kMinFwdDist   = 1.5f;

	const glm::vec3 cameraPos  = Host->GetCameraPositionForConsole();
	const glm::vec3 cameraLook = Host->GetCameraLookDirForConsole();
	glm::vec3 spawnPos = cameraPos + cameraLook * kSpawnFwdDist;

	Corona::CpuPhysicsRaycastHit sweepHit;
	if (Host->CpuPhysicsSphereSweep(
			cameraPos + cameraLook * 0.5f,
			kHalfExtent, cameraLook, kSpawnFwdDist, sweepHit))
	{
		const float fwd = std::max(kMinFwdDist, sweepHit.Distance - kHalfExtent * 0.5f);
		spawnPos = cameraPos + cameraLook * fwd;
		Log("sphere sweep hit existing object at " + std::to_string(sweepHit.Distance) + "m");
	}
	if (Terrain::Component* terrain = Host->GetActiveTerrainForConsole())
	{
		const float groundY = terrain->SampleHeight(spawnPos.x, spawnPos.z);
		spawnPos.y = groundY + kHalfExtent;
	}

	// Name the entity after the file stem so the inspector / map dump are
	// readable. Collisions are fine — Entity ids stay unique either way.
	const std::filesystem::path p(objPath);
	const std::string entityName = std::string("Spawn_") + p.stem().string();
	const CoronaECS::Entity entity = Host->CreateEntity(entityName);

	const bool ok = Host->AddMeshComponentForScript(
		entity,
		sceneHandle,
		spawnPos,
		/*rotationDegrees*/ glm::vec3(0.0f),
		kTargetExtent,
		/*scale*/ glm::vec3(1.0f),
		/*useScale*/ false,
		/*roughness*/ 0.6f,
		/*metallic*/  0.0f,
		/*overrideRM*/ true,
		/*visible*/   true,
		/*rayTracing*/ true,
		/*physicsQuery*/ true);
	if (!ok)
	{
		Log("AddMeshComponentForScript failed");
		return;
	}
	Log("spawned '" + entityName + "' at "
		+ std::to_string(spawnPos.x) + "," + std::to_string(spawnPos.y) + "," + std::to_string(spawnPos.z));
}

void CoronaConsole::RenderImGui()
{
	if (!bVisible)
		return;

	const ImGuiViewport* vp = ImGui::GetMainViewport();
	const float w = vp->WorkSize.x;
	const float h = vp->WorkSize.y;
	ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y));
	ImGui::SetNextWindowSize(ImVec2(w, h * 0.35f));
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.04f, 0.04f, 0.06f, 0.92f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);

	const ImGuiWindowFlags flags =
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings;
	ImGui::Begin("##corona_console", nullptr, flags);

	// Scrollback area — rendered as a read-only multi-line InputText so
	// the user can click-drag to select and Ctrl+C to copy.
	std::string flat;
	flat.reserve(History.size() * 80);
	for (const auto& line : History)
	{
		flat.append(line);
		flat.push_back('\n');
	}
	if (flat.empty())
		flat.push_back('\0');                  // ImGui requires a NUL even for empty bufs
	const float footerH = ImGui::GetFrameHeightWithSpacing();
	const ImVec2 logSize(-1.0f, -footerH - ImGui::GetStyle().ItemSpacing.y);
	ImGui::InputTextMultiline(
		"##log",
		flat.data(),
		flat.size() + 1,
		logSize,
		ImGuiInputTextFlags_ReadOnly);

	ImGui::Separator();
	if (bRequestFocus)
	{
		ImGui::SetKeyboardFocusHere();
		bRequestFocus = false;
	}
	ImGui::PushItemWidth(-1.0f);
	auto trampoline = [](ImGuiInputTextCallbackData* data) -> int
	{
		auto* self = static_cast<CoronaConsole*>(data->UserData);
		return self ? self->HandleInputCallback(data) : 0;
	};
	const bool entered = ImGui::InputText(
		"##cmd", InputBuf, sizeof(InputBuf),
		ImGuiInputTextFlags_EnterReturnsTrue
			| ImGuiInputTextFlags_CallbackCharFilter
			| ImGuiInputTextFlags_CallbackHistory
			| ImGuiInputTextFlags_CallbackCompletion,
		trampoline,
		this);
	ImGui::PopItemWidth();
	if (entered)
	{
		Submit(InputBuf);
		InputBuf[0] = '\0';
		bRequestFocus = true;
	}

	ImGui::End();
	ImGui::PopStyleVar();
	ImGui::PopStyleColor();
}
