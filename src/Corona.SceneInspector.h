#pragma once

// Scene inspector panel — left-anchored ImGui list of every alive entity in
// the world with collapsible component details. Provides per-entity Delete
// + inline position / rotation edit; future extension will add nested
// parent/child hierarchy. Toggled via a floating "Scene" button pinned at
// the bottom-left of the screen.

#include <chrono>
#include <string>

#include "EntityComponentSystem.h"

class Corona;

class CoronaSceneInspector
{
public:
	explicit CoronaSceneInspector(Corona* host);
	~CoronaSceneInspector();

	void RenderImGui(); // call once per frame inside the ImGui pass
	void Toggle() { bVisible = !bVisible; }
	bool IsVisible() const { return bVisible; }

private:
	void DrawEntityRow(CoronaECS::Entity entity);
	void DrawSelectedEntityDetails();
	void DrawBottomToggleButton();
	void DrawSelectedEntityViewportGizmo();

	Corona* Host;
	bool bVisible = false;
	CoronaECS::Entity SelectedEntity;

	// Set by the per-row right-click context menu; processed after the
	// entity-list loop so we never destroy an entity mid-iteration.
	CoronaECS::Entity PendingDeleteEntity;

	// Save-as-asset popup state: the entity targeted by the context menu
	// and the in-progress filename buffer. Popup is opened on right-click
	// "Save as asset..." and drawn at the top of the panel each frame.
	CoronaECS::Entity SaveAssetEntity;
	bool bOpenSaveAssetPopup = false;
	char SaveAssetNameBuf[64] = {};

	// Cached rotation+scale for non-mesh entities (lights/cameras/world).
	// TransformComponent stores only Position + LocalToWorld matrix, so we
	// keep euler/scale inspector-side to avoid lossy matrix decomposition
	// every frame. Reset when SelectedEntity changes.
	CoronaECS::Entity NonMeshTransformCacheEntity;
	glm::vec3 NonMeshTransformCacheRotation = glm::vec3(0.0f);
	glm::vec3 NonMeshTransformCacheScale = glm::vec3(1.0f);

	// Uniform-axis scale lock for the mesh-entity Scale editor. When ON
	// the slider edits X and we splat the value onto Y/Z; when OFF the
	// three axes are independent. Persists across selections so a user
	// who likes uniform scaling doesn't have to re-toggle each time.
	bool ScaleUniformLock = true;

	// Grass-blade recipe editor staging buffer. Snapshotted from the live
	// recipe whenever the selected entity changes so the user's pending
	// edits don't leak between entities.
	CoronaECS::Entity GrassEditEntity;
	int   GrassEditBladeCount = 100000;
	float GrassEditBladeHeight = 1.0f;
	int   GrassEditBladeSegments = 4;
	int   GrassEditSeed = 1;
	bool  GrassEditProcedural = false;

	// Transient status shown next to the Save Map button after a save attempt.
	std::string LastSaveStatus;
	std::chrono::steady_clock::time_point LastSaveStatusAt;
};
