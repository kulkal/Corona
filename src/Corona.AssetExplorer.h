#pragma once

// Asset explorer panel — browse bin/assets/ recursively, right-click a file
// to spawn / load / delete it. Sibling to CoronaSceneInspector; toggled via
// a separate floating button next to the scene-inspector toggle.

#include <chrono>
#include <filesystem>
#include <string>
#include <utility>
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
	void RenderWorldLayoutViewer();
	void RenderRoadGraphViewer();
	void RenderCityLayoutViewer();
	void DrawBottomToggleButton();
	void HandleContextMenu(const Entry& entry);

	// Spawn a mesh/model/.cmesh asset into the scene at the camera-forward
	// raycast midpoint (or 5 m ahead when nothing is hit), then close the panel.
	void SpawnMeshFromAsset(const std::filesystem::path& path);
	// Queue a .map load (replaces the scene) and close the panel.
	void LoadMapAsset(const std::filesystem::path& path);
	// Open a .worldlayout intermediate file in the built-in graph viewer.
	void OpenWorldLayoutAsset(const std::filesystem::path& path);
	bool SaveWorldLayoutAsset();
	void OpenRoadGraphAsset(const std::filesystem::path& path);
	void CreateProceduralRoadGraph();
	void GenerateProceduralRoadGraph();
	void PlanarizeRoadGraphIntersections();
	bool SaveRoadGraphAsset();
	void OpenCityLayoutAsset(const std::filesystem::path& path);
	void GenerateCityLayoutRoads();
	void RebuildCityLayoutBuildings();
	bool SaveCityLayoutAsset();
	bool CompileCityLayoutToMapAndLoad();

	// Double-click actions mutate CurrentListing / bVisible, which is unsafe
	// while iterating the listing during render. They are recorded here and
	// applied once after the entry loop.
	enum class PendingAction { None, Navigate, LoadMap, SpawnMesh, OpenWorldLayout, OpenRoadGraph, OpenCityLayout };

	struct WorldLayoutAttribute
	{
		std::string Key;
		std::string Value;
	};

	struct WorldLayoutNode
	{
		std::string Id;
		std::string Type;
		std::string Label;
		std::string Parent;
		float X = 0.0f;
		float Y = 0.0f;
		float Width = 0.0f;
		float Height2D = 0.0f;
		float BuildingHeight = 0.0f;
		int Floors = 0;
		bool bHasPosition = false;
		bool bHasSize = false;
		bool bClosedPolygon = false;
		std::vector<std::pair<float, float>> Points;
		std::vector<WorldLayoutAttribute> Attributes;
	};

	struct WorldLayoutEdge
	{
		std::string From;
		std::string To;
		std::string Type;
	};

	struct WorldLayoutDocument
	{
		std::filesystem::path SourcePath;
		std::string Name;
		std::vector<WorldLayoutNode> Nodes;
		std::vector<WorldLayoutEdge> Edges;
		std::string Error;
		bool bLoaded = false;
	};

	struct RoadGraphParams
	{
		int Seed = 42;
		float Width = 120000.0f;
		float Height = 90000.0f;
		float Density = 0.55f;
		float Curvature = 0.35f;
		float Branching = 0.55f;
		int Arterials = 7;
	};

	struct RoadGraphNode
	{
		std::string Id;
		std::string Kind;
		float X = 0.0f;
		float Y = 0.0f;
	};

	struct RoadGraphEdge
	{
		std::string Id;
		std::string From;
		std::string To;
		std::string Class;
		float Width = 600.0f;
		std::vector<std::pair<float, float>> Points;
	};

	struct RoadGraphDocument
	{
		std::filesystem::path SourcePath;
		std::string Name;
		RoadGraphParams Params;
		std::vector<RoadGraphNode> Nodes;
		std::vector<RoadGraphEdge> Edges;
		std::string Error;
		bool bLoaded = false;
	};

	struct CityLayoutParams
	{
		int Seed = 73;
		float Width = 9000.0f;
		float Depth = 7000.0f;
		float RoadSpacing = 1800.0f;
		float RoadJitter = 260.0f;
		float RoadWidth = 280.0f;
		float BuildingDensity = 0.74f;
		float BuildingSizeVariance = 1.0f;
		float LotSize = 620.0f;
		float Setback = 160.0f;
		float MinBuildingHeight = 180.0f;
		float MaxBuildingHeight = 1300.0f;
	};

	struct CityLayoutColor
	{
		float R = 0.7f;
		float G = 0.7f;
		float B = 0.7f;
	};

	struct CityLayoutRoad
	{
		std::string Id;
		std::string Class;
		float Width = 280.0f;
		bool bLocked = false;
		std::vector<std::pair<float, float>> Points;
	};

	struct CityLayoutBuildingType
	{
		std::string Id;
		float Weight = 1.0f;
		float MinHeight = 180.0f;
		float MaxHeight = 900.0f;
		CityLayoutColor Color;
		bool bBrickTexture = false;
	};

	struct CityLayoutBuilding
	{
		std::string Id;
		std::string Type;
		float X = 0.0f;
		float Y = 0.0f;
		float Width = 100.0f;
		float Depth = 100.0f;
		float Height = 200.0f;
		float Rotation = 0.0f;
		CityLayoutColor Color;
		bool bBrickTexture = false;
	};

	struct CityLayoutDocument
	{
		std::filesystem::path SourcePath;
		std::string Name;
		CityLayoutParams Params;
		std::vector<CityLayoutRoad> Roads;
		std::vector<CityLayoutBuildingType> BuildingTypes;
		std::vector<CityLayoutBuilding> Buildings;
		std::string Error;
		bool bLoaded = false;
	};

	int FindWorldLayoutNodeIndex(const std::string& id) const;
	bool IsWorldLayoutDescendantOf(int nodeIndex, const std::string& ancestorId) const;
	void KeepWorldLayoutBlocksAndBuildings();
	void MoveWorldLayoutNodeWithChildren(int nodeIndex, float dx, float dy);
	bool ResolveWorldLayoutOverlapsForNode(int nodeIndex);
	bool AlignWorldLayoutAdjacentEdgesForNode(int nodeIndex);
	void RemoveGeneratedOverlapRoadsForNode(const std::string& nodeId);

	Corona* Host;
	bool bVisible = false;
	std::filesystem::path AssetsRoot;
	std::filesystem::path CurrentDir;
	std::vector<Entry> CurrentListing;
	std::chrono::steady_clock::time_point LastRefresh;
	std::string PendingErrorMessage;
	PendingAction QueuedAction = PendingAction::None;
	std::filesystem::path QueuedActionPath;

	WorldLayoutDocument LayoutDocument;
	bool bWorldLayoutViewerVisible = false;
	float WorldLayoutZoom = 1.0f;
	float WorldLayoutPanX = 0.0f;
	float WorldLayoutPanY = 0.0f;
	bool bWorldLayoutAutoFit = true;
	bool bWorldLayoutShowImplicitRoads = true;
	int SelectedWorldLayoutNode = -1;
	int DraggingWorldLayoutNode = -1;
	int DraggingWorldLayoutVertex = -1;
	bool bWorldLayoutDragMoved = false;
	bool bWorldLayoutDirty = false;

	RoadGraphDocument RoadDocument;
	bool bRoadGraphViewerVisible = false;
	float RoadGraphZoom = 1.0f;
	float RoadGraphPanX = 0.0f;
	float RoadGraphPanY = 0.0f;
	bool bRoadGraphAutoFit = true;
	bool bRoadGraphDirty = false;
	bool bRoadGraphShowNodes = true;

	CityLayoutDocument CityDocument;
	bool bCityLayoutViewerVisible = false;
	float CityLayoutZoom = 1.0f;
	float CityLayoutPanX = 0.0f;
	float CityLayoutPanY = 0.0f;
	bool bCityLayoutAutoFit = true;
	bool bCityLayoutDirty = false;
	bool bCityLayoutShowBuildings = true;
	int SelectedCityRoad = -1;
	int DraggingCityRoad = -1;
	int DraggingCityVertex = -1;
};
