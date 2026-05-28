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

	Corona* Host;
	bool bVisible = false;
	CoronaECS::Entity SelectedEntity;

	// Set by the per-row right-click context menu; processed after the
	// entity-list loop so we never destroy an entity mid-iteration.
	CoronaECS::Entity PendingDeleteEntity;

	// Transient status shown next to the Save Map button after a save attempt.
	std::string LastSaveStatus;
	std::chrono::steady_clock::time_point LastSaveStatusAt;
};
