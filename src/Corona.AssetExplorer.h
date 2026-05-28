#pragma once

// Asset explorer panel — browse bin/assets/ recursively, right-click a file
// to spawn / load / delete it. Sibling to CoronaSceneInspector; toggled via
// a separate floating button next to the scene-inspector toggle.

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

class Corona;

class CoronaAssetExplorer
{
public:
	explicit CoronaAssetExplorer(Corona* host);
	~CoronaAssetExplorer();

	void RenderImGui();
	void Toggle() { bVisible = !bVisible; }
	bool IsVisible() const { return bVisible; }

private:
	struct Entry
	{
		std::filesystem::path Path;
		bool bIsDirectory = false;
	};

	void RefreshIfStale();
	void RenderEntry(const Entry& entry);
	void DrawBottomToggleButton();
	void HandleContextMenu(const Entry& entry);

	Corona* Host;
	bool bVisible = false;
	std::filesystem::path AssetsRoot;
	std::filesystem::path CurrentDir;
	std::vector<Entry> CurrentListing;
	std::chrono::steady_clock::time_point LastRefresh;
	std::string PendingErrorMessage;
};
