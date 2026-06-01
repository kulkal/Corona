#include "stdafx.h"
#include "Corona.AssetExplorer.h"

#include "Corona.h"
#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <system_error>

#include "PlatformSystem.h"
#include "Utils.h"

void AppendCpuRuntimeTrace(const std::wstring& line);

namespace
{
	constexpr float kPanelWidth = 320.0f;
	constexpr float kToggleBtnH = 28.0f;
	constexpr float kBottomBarH = 38.0f; // shared with scene inspector

	bool EndsWithCi(const std::string& s, const std::string& suffix)
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
	}

	bool IsModelFile(const std::filesystem::path& p)
	{
		const std::string ext = p.extension().string();
		return EndsWithCi(ext, ".obj") || EndsWithCi(ext, ".fbx") ||
		       EndsWithCi(ext, ".gltf") || EndsWithCi(ext, ".glb") ||
		       EndsWithCi(ext, ".dae") || EndsWithCi(ext, ".ply") ||
		       EndsWithCi(ext, ".stl") || EndsWithCi(ext, ".3ds");
	}

	bool IsMapFile(const std::filesystem::path& p)
	{
		const std::string ext = p.extension().string();
		return EndsWithCi(ext, ".map");
	}

	bool IsImageFile(const std::filesystem::path& p)
	{
		const std::string ext = p.extension().string();
		return EndsWithCi(ext, ".png") || EndsWithCi(ext, ".jpg") ||
		       EndsWithCi(ext, ".jpeg") || EndsWithCi(ext, ".bmp");
	}

	bool IsMeshCacheFile(const std::filesystem::path& p)
	{
		return EndsWithCi(p.extension().string(), ".cmesh");
	}

	// For a source model path "foo.fbx", return "foo.cmesh" (sibling).
	std::filesystem::path MeshCachePathFor(const std::filesystem::path& sourcePath)
	{
		std::filesystem::path cachePath = sourcePath;
		cachePath.replace_extension(L".cmesh");
		return cachePath;
	}
}

CoronaAssetExplorer::CoronaAssetExplorer(Corona* host)
	: Host(host)
{
	AssetsRoot = RuntimePaths::RootDirectory() / L"assets";
	std::error_code ec;
	std::filesystem::create_directories(AssetsRoot, ec);
	CurrentDir = AssetsRoot;
}

CoronaAssetExplorer::~CoronaAssetExplorer() = default;

void CoronaAssetExplorer::RefreshIfStale()
{
	using clock = std::chrono::steady_clock;
	const auto now = clock::now();
	if (!CurrentListing.empty() && (now - LastRefresh) < std::chrono::milliseconds(500))
		return;
	LastRefresh = now;

	CurrentListing.clear();
	std::error_code ec;
	if (!std::filesystem::exists(CurrentDir, ec))
		CurrentDir = AssetsRoot;
	for (const auto& it : std::filesystem::directory_iterator(CurrentDir, ec))
	{
		Entry e;
		e.Path = it.path();
		e.bIsDirectory = it.is_directory(ec);
		CurrentListing.push_back(std::move(e));
	}
	// Directories first, then alpha-sorted.
	std::sort(CurrentListing.begin(), CurrentListing.end(),
		[](const Entry& a, const Entry& b)
		{
			if (a.bIsDirectory != b.bIsDirectory)
				return a.bIsDirectory > b.bIsDirectory;
			return a.Path.filename() < b.Path.filename();
		});
}

void CoronaAssetExplorer::RenderImGui()
{
	DrawBottomToggleButton();
	if (!bVisible)
		return;

	RefreshIfStale();

	const ImGuiViewport* vp = ImGui::GetMainViewport();
	const float h = vp->WorkSize.y;
	const float x = vp->WorkPos.x + kPanelWidth + 8.0f; // sit to the right of the scene inspector
	ImGui::SetNextWindowPos(ImVec2(x, vp->WorkPos.y));
	ImGui::SetNextWindowSize(ImVec2(kPanelWidth + 40.0f, h - kBottomBarH - 4.0f));

	const ImGuiWindowFlags flags =
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoSavedSettings;
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f, 0.06f, 0.09f, 0.94f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::Begin("##asset_explorer", nullptr, flags);

	ImGui::TextColored(ImVec4(1.0f, 0.92f, 0.85f, 1.0f), "Asset Explorer");
	const std::string relDir = std::filesystem::relative(CurrentDir, AssetsRoot).string();
	ImGui::TextDisabled("assets/%s", relDir.empty() ? "" : (relDir + "/").c_str());
	ImGui::Separator();

	if (CurrentDir != AssetsRoot)
	{
		if (ImGui::Selectable(".. (up)"))
		{
			CurrentDir = CurrentDir.parent_path();
			CurrentListing.clear(); // force refresh next frame
		}
	}

	for (const auto& entry : CurrentListing)
		RenderEntry(entry);

	if (!PendingErrorMessage.empty())
	{
		ImGui::Separator();
		ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "%s", PendingErrorMessage.c_str());
	}

	ImGui::End();
	ImGui::PopStyleVar();
	ImGui::PopStyleColor();
}

void CoronaAssetExplorer::RenderEntry(const Entry& entry)
{
	const std::string label = (entry.bIsDirectory ? std::string("[D] ") : std::string("    "))
		+ entry.Path.filename().string();

	const std::string popupId = "asset_ctx##" + entry.Path.filename().string();

	if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick))
	{
		if (entry.bIsDirectory && ImGui::IsMouseDoubleClicked(0))
		{
			CurrentDir = entry.Path;
			CurrentListing.clear();
		}
	}
	if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(1))
		ImGui::OpenPopup(popupId.c_str());

	HandleContextMenu(entry);
}

void CoronaAssetExplorer::HandleContextMenu(const Entry& entry)
{
	const std::string popupId = "asset_ctx##" + entry.Path.filename().string();
	if (!ImGui::BeginPopup(popupId.c_str()))
		return;

	const std::filesystem::path path = entry.Path;
	const std::string pathUtf8 = path.string();

	if (entry.bIsDirectory)
	{
		if (ImGui::MenuItem("Open"))
		{
			CurrentDir = path;
			CurrentListing.clear();
		}
	}
	else
	{
		if (IsModelFile(path))
		{
			// Sibling .cmesh cache (if any). Showing the menu only when
			// the cache actually exists keeps the UI honest — without
			// a cache there's nothing to invalidate.
			const std::filesystem::path cachePath = MeshCachePathFor(path);
			std::error_code cacheEc;
			const bool bCacheExists = std::filesystem::exists(cachePath, cacheEc);
			if (bCacheExists)
			{
				if (ImGui::MenuItem("Re-import (rebuild .cmesh)"))
				{
					std::wstring err;
					if (!Host->InvalidateMeshCacheForSource(path.wstring(), &err))
						PendingErrorMessage = "invalidate failed: " + PlatformWideToUtf8(err);
					else
					{
						PendingErrorMessage = "cache cleared — re-import on next load";
						CurrentListing.clear();
					}
				}
			}
			if (ImGui::MenuItem("Spawn model"))
			{
				const Corona::ScriptSceneHandle sh =
					Host->LoadSceneForScript(path.wstring());
				if (sh == Corona::InvalidScriptSceneHandle)
				{
					PendingErrorMessage = "LoadSceneForScript failed: " + pathUtf8;
				}
				else
				{
					const glm::vec3 camPos  = Host->GetCameraPositionForConsole();
					const glm::vec3 camLook = Host->GetCameraLookDirForConsole();
					const glm::vec3 spawnPos = camPos + camLook * 3.0f;
					const std::string name = "Spawn_" + path.stem().string();
					CoronaECS::Entity e = Host->CreateEntity(name);
					Host->AddMeshComponentForScript(
						e, sh, spawnPos, glm::vec3(0.0f),
						/*targetExtent*/ 1.5f, glm::vec3(1.0f), /*useScale*/ false,
						/*roughness*/ 0.6f, /*metallic*/ 0.0f,
						/*overrideRM*/ true,
						/*visible*/ true, /*rayTracing*/ true, /*physicsQuery*/ true);
					PendingErrorMessage.clear();
				}
			}
		}
		if (IsMapFile(path))
		{
			if (ImGui::MenuItem("Load map (replace scene)"))
			{
				// `.map` is the canonical extension; ResolveMapPath adds it
				// when the name has no dot, so we pass just the stem.
				const std::wstring name = path.stem().wstring();
				Host->QueueEditorMapLoad(name);
				PendingErrorMessage = "loadmap queued: " + path.stem().string();
			}
		}
		if (IsImageFile(path))
		{
			ImGui::TextDisabled("(image — use console `generate <path>`)");
		}
		if (IsMeshCacheFile(path))
		{
			// .cmesh files are derived; deleting forces a rebuild from
			// the sibling source asset on next load.
			if (ImGui::MenuItem("Delete cache (re-import on next load)"))
			{
				std::error_code ec;
				std::filesystem::remove(path, ec);
				if (ec)
					PendingErrorMessage = "delete failed: " + ec.message();
				else
				{
					PendingErrorMessage = "cache cleared";
					CurrentListing.clear();
					AppendCpuRuntimeTrace(L"[AssetExplorer] cleared cmesh cache " + path.wstring());
				}
			}
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Delete file"))
		{
			std::error_code ec;
			std::filesystem::remove(path, ec);
			if (ec)
				PendingErrorMessage = "delete failed: " + ec.message();
			else
			{
				PendingErrorMessage.clear();
				CurrentListing.clear();
				AppendCpuRuntimeTrace(L"[AssetExplorer] deleted " + path.wstring());
			}
		}
	}

	ImGui::EndPopup();
}

void CoronaAssetExplorer::DrawBottomToggleButton()
{
	const ImGuiViewport* vp = ImGui::GetMainViewport();
	const float bw = 110.0f;
	// Sit to the right of the scene inspector's toggle button so both fit at
	// the bottom edge without overlap.
	const float x = vp->WorkPos.x + 6.0f + 110.0f + 6.0f;
	ImGui::SetNextWindowPos(ImVec2(x, vp->WorkPos.y + vp->WorkSize.y - kToggleBtnH - 6.0f));
	ImGui::SetNextWindowSize(ImVec2(bw, kToggleBtnH));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(2.0f, 2.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
	const ImGuiWindowFlags flags =
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar;
	ImGui::Begin("##asset_explorer_toggle", nullptr, flags);
	if (ImGui::Button(bVisible ? "Hide Assets" : "Show Assets", ImVec2(-1, -1)))
		bVisible = !bVisible;
	ImGui::End();
	ImGui::PopStyleVar(2);
}
