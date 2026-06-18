#include "stdafx.h"
#include "Corona.SceneInspector.h"

#include "Corona.h"
#include "imgui.h"
#include "imGuIZMO.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
	// Sticky width for the left dock so toggling visibility doesn't jitter.
	constexpr float kPanelWidth = 480.0f;
	constexpr float kToggleBtnH = 28.0f;

	std::string FormatEntityLabel(uint32_t id, const std::string& name)
	{
		char buf[256];
		if (name.empty())
			snprintf(buf, sizeof(buf), "Entity#%u", id);
		else
			snprintf(buf, sizeof(buf), "%s##e%u", name.c_str(), id);
		return std::string(buf);
	}

	const char* LightTypeDisplayName(CoronaECS::LightType type)
	{
		switch (type)
		{
		case CoronaECS::LightType::Directional: return "Directional";
		case CoronaECS::LightType::Spot: return "Spot";
		case CoronaECS::LightType::Point:
		default: return "Point";
		}
	}

	std::string ToLowerAscii(std::string value)
	{
		for (char& ch : value)
			ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
		return value;
	}
}

CoronaSceneInspector::CoronaSceneInspector(Corona* host) : Host(host) {}
CoronaSceneInspector::~CoronaSceneInspector() = default;

void CoronaSceneInspector::RenderImGui()
{
	// Always draw the bottom-left toggle button — even when the panel is
	// hidden the user needs a way to bring it back.
	DrawBottomToggleButton();

	if (!bVisible)
		return;

	const ImGuiViewport* vp = ImGui::GetMainViewport();
	const float h = vp->WorkSize.y;
	ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y));
	// Seed size on first appearance only; subsequent frames let the user's
	// drag-to-resize stick (NoSavedSettings still forgets on relaunch — fine
	// for now).
	ImGui::SetNextWindowSize(
		ImVec2(kPanelWidth, h - kToggleBtnH - 4.0f),
		ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints(
		ImVec2(240.0f, 200.0f),
		ImVec2(vp->WorkSize.x * 0.9f, h - kToggleBtnH - 4.0f));

	const ImGuiWindowFlags flags =
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoTitleBar |
		ImGuiWindowFlags_NoSavedSettings;
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.07f, 0.09f, 0.94f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);

	ImGui::Begin("##scene_inspector", nullptr, flags);

	ImGui::TextColored(ImVec4(0.85f, 0.92f, 1.0f, 1.0f), "Scene Inspector");

	// Top-of-panel toolbar: "Save Map" overwrites the currently-loaded map,
	// matching the console's bare `savemap` command. Disabled when no map
	// has been loaded yet so the user can't create an "untitled" file by
	// accident.
	const std::wstring& currentMap = Host->GetCurrentMapName();
	const bool hasMap = !currentMap.empty();
	if (!hasMap)
		ImGui::BeginDisabled();
	char btnLabel[160];
	if (hasMap)
	{
		// Truncate long names; convert from wstring → narrow for display.
		std::string narrowName;
		narrowName.reserve(currentMap.size());
		for (wchar_t wc : currentMap)
			narrowName.push_back(static_cast<char>(wc < 128 ? wc : '?'));
		snprintf(btnLabel, sizeof(btnLabel), "Save Map: '%s'", narrowName.c_str());
	}
	else
	{
		snprintf(btnLabel, sizeof(btnLabel), "Save Map (no map loaded)");
	}
	if (ImGui::Button(btnLabel, ImVec2(-1, 0)))
	{
		std::wstring err;
		if (!Host->SaveMapToFile(currentMap, &err))
			LastSaveStatus = std::string("save failed: ") + std::string(err.begin(), err.end());
		else
			LastSaveStatus = "saved";
		LastSaveStatusAt = std::chrono::steady_clock::now();
	}
	if (!hasMap)
		ImGui::EndDisabled();

	// Show the last save outcome for a few seconds so the user has feedback.
	if (!LastSaveStatus.empty())
	{
		const auto now = std::chrono::steady_clock::now();
		const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - LastSaveStatusAt).count();
		if (elapsed < 3)
			ImGui::TextColored(ImVec4(0.7f, 1.0f, 0.7f, 1.0f), "%s", LastSaveStatus.c_str());
		else
			LastSaveStatus.clear();
	}

	// Trigger the "Save as asset..." modal — context-menu sets the flag,
	// OpenPopup must be called inside the panel's window scope (right after
	// EndPopup() invalidates the prior frame's context).
	if (bOpenSaveAssetPopup)
	{
		ImGui::OpenPopup("Save entity as asset");
		bOpenSaveAssetPopup = false;
	}
	if (ImGui::BeginPopupModal("Save entity as asset", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text("Asset name (becomes scene_assets/<name>.asset.lua):");
		ImGui::InputText("##save_asset_name", SaveAssetNameBuf, IM_ARRAYSIZE(SaveAssetNameBuf));
		ImGui::Spacing();
		const bool bNameValid = SaveAssetNameBuf[0] != '\0';
		ImGui::BeginDisabled(!bNameValid || !SaveAssetEntity.IsValid());
		if (ImGui::Button("Save", ImVec2(120, 0)))
		{
			std::wstring err;
			const std::string name(SaveAssetNameBuf);
			const bool ok = Host->SaveEntityAsAsset(SaveAssetEntity, name, &err);
			LastSaveStatus = ok
				? ("Saved asset: " + name)
				: ("Asset save failed: " + std::string(err.begin(), err.end()));
			LastSaveStatusAt = std::chrono::steady_clock::now();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120, 0)))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	ImGui::Separator();

	// Entity list — top 60% of the panel.
	ImGui::InputTextWithHint("##scene_inspector_name_search", "Search name...", EntitySearchNameBuf, IM_ARRAYSIZE(EntitySearchNameBuf));
	ImGui::Checkbox("Mesh##scene_filter", &bFilterMeshEntities);
	ImGui::SameLine();
	ImGui::Checkbox("Light##scene_filter", &bFilterLightEntities);
	ImGui::SameLine();
	ImGui::Checkbox("Camera##scene_filter", &bFilterCameraEntities);

	const float listH = ImGui::GetContentRegionAvail().y * 0.60f;
	ImGui::BeginChild("##entity_list", ImVec2(0, listH), true);

	const auto& ecs = Host->GetEntityWorld();

	// Build a single sorted-by-id list of alive entities. We currently
	// pull from the union of mesh / light / camera holders since ECS doesn't
	// expose a "list all alive entities" view directly.
	std::vector<CoronaECS::Entity> entities;
	entities.reserve(64);
	auto append = [&](const std::vector<CoronaECS::Entity>& src)
	{
		for (auto e : src)
			entities.push_back(e);
	};
	append(ecs.GetEntitiesWithMeshAndTransform());
	append(ecs.GetEntitiesWithLight());
	if (auto cam = ecs.GetActiveCameraEntity(); cam.IsValid())
		entities.push_back(cam);

	// Dedupe by id (transform-only entities not listed yet — fine for v0).
	std::sort(entities.begin(), entities.end(),
		[](CoronaECS::Entity a, CoronaECS::Entity b) { return a.GetId() < b.GetId(); });
	entities.erase(std::unique(entities.begin(), entities.end()), entities.end());

	const std::string loweredQuery = ToLowerAscii(std::string(EntitySearchNameBuf));
	auto entityDisplayName = [&](CoronaECS::Entity e)
	{
		if (const auto* n = ecs.GetName(e); n && !n->empty())
			return *n;
		char fallback[32];
		snprintf(fallback, sizeof(fallback), "Entity#%u", e.GetId());
		return std::string(fallback);
	};
	auto passesTypeFilter = [&](CoronaECS::Entity e)
	{
		return (bFilterMeshEntities && ecs.HasMesh(e)) ||
			(bFilterLightEntities && ecs.HasLight(e)) ||
			(bFilterCameraEntities && ecs.HasCamera(e));
	};
	auto passesNameFilter = [&](CoronaECS::Entity e)
	{
		if (loweredQuery.empty())
			return true;
		return ToLowerAscii(entityDisplayName(e)).find(loweredQuery) != std::string::npos;
	};

	if (entities.empty())
	{
		ImGui::TextDisabled("(scene is empty)");
	}
	else
	{
		int visibleCount = 0;
		for (auto e : entities)
		{
			if (!passesTypeFilter(e) || !passesNameFilter(e))
				continue;
			DrawEntityRow(e);
			++visibleCount;
		}
		if (visibleCount == 0)
			ImGui::TextDisabled("(no matching entities)");
	}

	ImGui::EndChild();

	// Process any deferred delete request from the per-row context menu.
	// Done here (after the list loop) so we never destroy mid-iteration.
	if (PendingDeleteEntity.IsValid())
	{
		auto& mutableEcs = Host->GetEntityWorld();
		if (mutableEcs.IsAlive(PendingDeleteEntity))
		{
			// ECS::DestroyEntity only erases the components map; the
			// renderer-side SceneObject keeps drawing because it lives in a
			// parallel list. Strip the visual first, then drop the entity.
			const Corona::SceneObjectHandle sceneObj =
				Host->GetEntitySceneObject(PendingDeleteEntity);
			if (sceneObj != Corona::InvalidSceneObjectHandle)
				Host->RemoveSceneObject(sceneObj);
			mutableEcs.DestroyEntity(PendingDeleteEntity);
		}
		if (SelectedEntity == PendingDeleteEntity)
			SelectedEntity = CoronaECS::Entity();
		PendingDeleteEntity = CoronaECS::Entity();
	}

	ImGui::Separator();
	DrawSelectedEntityDetails();

	ImGui::End();
	ImGui::PopStyleVar();
	ImGui::PopStyleColor();

	// Viewport-space gizmo for the selected entity. Drawn as an ImGui foreground
	// overlay (axis lines + crosshair) so it sits on top of the world render.
	DrawSelectedEntityViewportGizmo();
}

void CoronaSceneInspector::DrawSelectedEntityViewportGizmo()
{
	if (!Host)
		return;
	if (!SelectedEntity.IsValid())
		return;

	const auto& ecs = Host->GetEntityWorld();
	if (!ecs.IsAlive(SelectedEntity))
		return;

	// Resolve world position of the selected entity. Mesh entities expose
	// authoritative position via SceneObject transform; non-mesh (lights,
	// cameras) fall back to ECS TransformComponent.
	glm::vec3 worldPos(0.0f);
	bool gotPos = false;
	if (const auto* m = ecs.GetMesh(SelectedEntity))
	{
		glm::vec3 spawnPos(0.0f), rot(0.0f), scl(1.0f);
		float te = 1.0f;
		bool us = false;
		if (Host->GetSceneObjectTransformForScript(
			static_cast<Corona::SceneObjectHandle>(m->RenderObjectHandle),
			spawnPos, rot, te, scl, us))
		{
			worldPos = spawnPos;
			gotPos = true;
		}
	}
	if (!gotPos)
	{
		if (const auto* t = ecs.GetTransform(SelectedEntity))
		{
			worldPos = t->GetPosition();
			gotPos = true;
		}
	}
	if (!gotPos)
		return;

	const glm::mat4 viewProj = Host->GetUnjitteredViewProjForOverlay();
	const ImGuiViewport* vp = ImGui::GetMainViewport();
	const float vw = vp->WorkSize.x;
	const float vh = vp->WorkSize.y;
	const float vx = vp->WorkPos.x;
	const float vy = vp->WorkPos.y;

	auto worldToScreen = [&](const glm::vec3& world, ImVec2& out) -> bool
	{
		const glm::vec4 clip = viewProj * glm::vec4(world, 1.0f);
		if (clip.w <= 0.0001f)
			return false;
		const float ndcX = clip.x / clip.w;
		const float ndcY = clip.y / clip.w;
		const float ndcZ = clip.z / clip.w;
		if (ndcZ < 0.0f || ndcZ > 1.0f)
			return false;
		out.x = vx + (ndcX * 0.5f + 0.5f) * vw;
		out.y = vy + (1.0f - (ndcY * 0.5f + 0.5f)) * vh;
		return true;
	};

	ImVec2 centerScreen;
	if (!worldToScreen(worldPos, centerScreen))
		return;

	// Axis length tuned to feel large at typical scene scale (Sponza extents
	// are ~10-20m, terrain is 2km wide). 30 m axes are visible across the
	// whole Sponza interior while not overwhelming up close.
	const float kAxisWorldLen = 30.0f;
	ImVec2 xTip, yTip, zTip;
	const bool gotX = worldToScreen(worldPos + glm::vec3(kAxisWorldLen, 0, 0), xTip);
	const bool gotY = worldToScreen(worldPos + glm::vec3(0, kAxisWorldLen, 0), yTip);
	const bool gotZ = worldToScreen(worldPos + glm::vec3(0, 0, kAxisWorldLen), zTip);

	ImDrawList* draw = ImGui::GetForegroundDrawList();
	const ImU32 colX = IM_COL32(235, 80, 80, 230);
	const ImU32 colY = IM_COL32(110, 220, 110, 230);
	const ImU32 colZ = IM_COL32(80, 140, 240, 230);
	const ImU32 colCenter = IM_COL32(255, 255, 255, 230);
	const ImU32 colCenterShadow = IM_COL32(0, 0, 0, 180);

	const float thickness = 2.5f;
	if (gotX) draw->AddLine(centerScreen, xTip, colX, thickness);
	if (gotY) draw->AddLine(centerScreen, yTip, colY, thickness);
	if (gotZ) draw->AddLine(centerScreen, zTip, colZ, thickness);

	// Center marker — small filled circle with shadow ring; tuned down from
	// the 5x first pass to ~1.67x original so it stays visible without
	// dominating the viewport at close range.
	draw->AddCircle(centerScreen, 12.0f, colCenterShadow, 20, 3.0f);
	draw->AddCircleFilled(centerScreen, 7.0f, colCenter);

	// Axis tips labels (X/Y/Z) for quick orientation while editing.
	if (gotX) draw->AddText(ImVec2(xTip.x + 4.0f, xTip.y - 6.0f), colX, "X");
	if (gotY) draw->AddText(ImVec2(yTip.x + 4.0f, yTip.y - 6.0f), colY, "Y");
	if (gotZ) draw->AddText(ImVec2(zTip.x + 4.0f, zTip.y - 6.0f), colZ, "Z");
}

void CoronaSceneInspector::DrawEntityRow(CoronaECS::Entity entity)
{
	const auto& ecs = Host->GetEntityWorld();
	std::string name;
	if (const auto* n = ecs.GetName(entity))
		name = *n;
	const std::string label = FormatEntityLabel(entity.GetId(), name);

	const bool bSelected = (SelectedEntity == entity);

	// Component badges so the list communicates type without expanding.
	std::string badges;
	if (ecs.HasMesh(entity))     badges += "[M]";
	if (const auto* light = ecs.GetLight(entity))
	{
		badges += "[L:";
		badges += LightTypeDisplayName(light->Type);
		badges += "]";
	}
	if (ecs.HasCamera(entity))   badges += "[C]";
	if (ecs.HasPhysics(entity))  badges += "[P]";
	if (ecs.HasScript(entity))   badges += "[S]";

	if (ImGui::Selectable(label.c_str(), bSelected,
			ImGuiSelectableFlags_SpanAllColumns))
	{
		SelectedEntity = entity;
	}

	// Right-click → context menu. Selecting the row on right-click matches
	// typical scene-editor UX (the menu acts on what you just targeted).
	char popupId[32];
	snprintf(popupId, sizeof(popupId), "##ctx_e%u", entity.GetId());
	if (ImGui::BeginPopupContextItem(popupId))
	{
		SelectedEntity = entity;
		if (ImGui::MenuItem("Save as asset..."))
		{
			SaveAssetEntity = entity;
			// Pre-fill the asset name with the entity name (sanitization
			// happens server-side in SaveEntityAsAsset).
			const auto& ecs = Host->GetEntityWorld();
			const std::string defaultName =
				ecs.GetName(entity) ? *ecs.GetName(entity) : "entity";
			std::snprintf(SaveAssetNameBuf, sizeof(SaveAssetNameBuf),
				"%s", defaultName.c_str());
			bOpenSaveAssetPopup = true;
		}
		// Re-import: only available for mesh entities whose Scene was
		// loaded from a source asset (FBX/OBJ/...). Procedural meshes
		// (box / sphere / grass) and entities without a mesh leave the
		// item out so the menu stays meaningful per entity type.
		if (const auto* m = ecs.GetMesh(entity); m && m->ScenePtr && !m->ScenePtr->SourceFilePath.empty())
		{
			if (ImGui::MenuItem("Re-import mesh (rebuild .cmesh)"))
			{
				std::wstring err;
				if (Host->InvalidateMeshCacheForSource(m->ScenePtr->SourceFilePath, &err))
					LastSaveStatus = "cache cleared — reload to re-import";
				else
					LastSaveStatus = "re-import failed: " +
						std::string(err.begin(), err.end());
				LastSaveStatusAt = std::chrono::steady_clock::now();
			}
		}
		if (ImGui::MenuItem("Delete"))
			PendingDeleteEntity = entity;
		ImGui::EndPopup();
	}

	if (!badges.empty())
	{
		ImGui::SameLine();
		ImGui::TextDisabled("%s", badges.c_str());
	}
}

void CoronaSceneInspector::DrawSelectedEntityDetails()
{
	auto& ecs = Host->GetEntityWorld();
	if (!SelectedEntity.IsValid() || !ecs.IsAlive(SelectedEntity))
	{
		ImGui::TextDisabled("Select an entity to edit");
		return;
	}

	std::string name = "<unnamed>";
	if (const auto* n = ecs.GetName(SelectedEntity))
		name = *n;

	ImGui::Text("Selected: %s  (id=%u)", name.c_str(), SelectedEntity.GetId());

	// Action buttons.
	if (ImGui::Button("Delete##sel"))
	{
		ecs.DestroyEntity(SelectedEntity);
		SelectedEntity = CoronaECS::Entity();
		return;
	}
	ImGui::SameLine();
	if (ImGui::Button("Focus##sel"))
	{
		// Pick the most accurate world position for the target. Mesh
		// entities expose a SceneObject transform (more authoritative than
		// the ECS TransformComponent which can lag behind script writes);
		// everything else falls back to the ECS transform.
		glm::vec3 targetPos(0.0f);
		if (const auto* m = ecs.GetMesh(SelectedEntity))
		{
			glm::vec3 spawnPos(0.0f), rot(0.0f), scl(1.0f);
			float te = 1.0f;
			bool us = false;
			if (Host->GetSceneObjectTransformForScript(
					static_cast<Corona::SceneObjectHandle>(m->RenderObjectHandle),
					spawnPos, rot, te, scl, us))
			{
				targetPos = spawnPos;
			}
		}
		else if (auto* t = ecs.GetTransform(SelectedEntity))
		{
			targetPos = t->GetPosition();
		}

		auto activeCam = ecs.GetActiveCameraEntity();
		if (activeCam.IsValid())
		{
			glm::vec3 viewDir = Host->GetCameraLookDirForConsole();
			if (glm::length(viewDir) < 0.01f) viewDir = glm::vec3(0.0f, -0.3f, 1.0f);
			viewDir = glm::normalize(viewDir);
			const glm::vec3 newCamPos = targetPos - viewDir * 5.0f;

			Host->SetEntityTransformForScript(activeCam, newCamPos, glm::vec3(0.0f), glm::vec3(1.0f));
			CoronaECS::CameraComponent cam;
			if (Host->GetEntityCameraComponentForScript(activeCam, cam))
			{
				cam.LookDirection = glm::normalize(targetPos - newCamPos);
				Host->SetEntityCameraComponentForScript(activeCam, cam);
			}
		}
	}
	ImGui::SameLine();
	if (ImGui::Button("Deselect"))
	{
		SelectedEntity = CoronaECS::Entity();
		return;
	}

	ImGui::Separator();

	// Transform — edits flow through SetEntityMeshTransformForScript so the
	// SceneObject's render-time Transform matrix actually gets rebuilt
	// (plain ECS-component mutation doesn't propagate to the renderer).
	if (const auto* mesh = ecs.GetMesh(SelectedEntity))
	{
		const Corona::SceneObjectHandle soh = static_cast<Corona::SceneObjectHandle>(mesh->RenderObjectHandle);
		glm::vec3 position(0.0f), rotation(0.0f), scale(1.0f);
		float targetExtent = 1.0f;
		bool useScale = false;
		const bool gotState = Host->GetSceneObjectTransformForScript(soh, position, rotation, targetExtent, scale, useScale);
		if (gotState && ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen))
		{
			float arrPos[3] = { position.x, position.y, position.z };
			float arrRot[3] = { rotation.x, rotation.y, rotation.z };
			// If the entity was spawned with auto-fit (useScale=false),
			// promote its current effective scale into the explicit slot
			// so the new always-explicit editor doesn't reset the mesh.
			if (!useScale)
			{
				scale = glm::vec3(targetExtent);
				useScale = true;
			}
			float arrScale[3] = { scale.x, scale.y, scale.z };
			bool changed = false;
			if (ImGui::DragFloat3("Position##t", arrPos, 0.05f))
				changed = true;
			if (ImGui::DragFloat3("Rotation##t", arrRot, 1.0f))
				changed = true;
			// Simplified Scale: X/Y/Z drag + a "Uniform" checkbox that
			// locks all three to the X-axis edit. No TargetExtent
			// confusion any more — every entity is on the explicit-scale
			// path now.
			if (ImGui::Checkbox("Uniform##scaleLock", &ScaleUniformLock))
				; // toggle only; no scale change
			ImGui::SameLine();
			if (ScaleUniformLock)
			{
				if (ImGui::DragFloat("Scale##tUniform", &arrScale[0], 0.01f, 0.001f, 1000.0f))
				{
					arrScale[1] = arrScale[0];
					arrScale[2] = arrScale[0];
					scale = glm::vec3(arrScale[0]);
					changed = true;
				}
			}
			else
			{
				if (ImGui::DragFloat3("Scale X/Y/Z##t", arrScale, 0.01f, 0.001f, 1000.0f))
				{
					scale = glm::vec3(arrScale[0], arrScale[1], arrScale[2]);
					changed = true;
				}
			}
			if (changed)
			{
				Host->SetEntityMeshTransformForScript(
					SelectedEntity,
					glm::vec3(arrPos[0], arrPos[1], arrPos[2]),
					glm::vec3(arrRot[0], arrRot[1], arrRot[2]),
					targetExtent,
					scale,
					useScale);
			}
		}
	}
	// Non-mesh entities (point lights, cameras, world entities) keep their
	// position on the ECS TransformComponent. The Mesh branch above writes
	// through the renderer's SceneObject for live preview; here we route to
	// SetEntityTransformForScript which is the same path the toolbox uses
	// when spawning a point light.
	else if (auto* t = ecs.GetTransform(SelectedEntity))
	{
		if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen))
		{
			// Non-mesh entities (lights, cameras, generic world entities) —
			// TransformComponent only stores Position + LocalToWorld matrix.
			// Rotation/Scale are tracked inspector-side because decomposing
			// the matrix every frame would lose euler precision and reset
			// the user's pending edit. Cache resets to identity each time
			// SelectedEntity changes.
			if (NonMeshTransformCacheEntity != SelectedEntity)
			{
				NonMeshTransformCacheEntity = SelectedEntity;
				NonMeshTransformCacheRotation = glm::vec3(0.0f);
				NonMeshTransformCacheScale = glm::vec3(1.0f);
			}
			glm::vec3 position = t->GetPosition();
			float arrPos[3] = { position.x, position.y, position.z };
			float arrRot[3] = {
				NonMeshTransformCacheRotation.x,
				NonMeshTransformCacheRotation.y,
				NonMeshTransformCacheRotation.z,
			};
			float arrScale[3] = {
				NonMeshTransformCacheScale.x,
				NonMeshTransformCacheScale.y,
				NonMeshTransformCacheScale.z,
			};
			bool changed = false;
			if (ImGui::DragFloat3("Position##tnm", arrPos, 0.05f))
				changed = true;
			if (ImGui::DragFloat3("Rotation##tnm", arrRot, 1.0f))
			{
				NonMeshTransformCacheRotation = glm::vec3(arrRot[0], arrRot[1], arrRot[2]);
				changed = true;
			}
			if (ImGui::DragFloat3("Scale##tnm", arrScale, 0.01f, 0.001f, 100.0f))
			{
				NonMeshTransformCacheScale = glm::vec3(arrScale[0], arrScale[1], arrScale[2]);
				changed = true;
			}
			if (changed)
			{
				Host->SetEntityTransformForScript(
					SelectedEntity,
					glm::vec3(arrPos[0], arrPos[1], arrPos[2]),
					NonMeshTransformCacheRotation,
					NonMeshTransformCacheScale);
			}
		}
	}

	if (const auto* mesh = ecs.GetMesh(SelectedEntity))
	{

		if (ImGui::CollapsingHeader("Mesh"))
		{
			ImGui::Text("RenderObject: %u", mesh->RenderObjectHandle);
			ImGui::Text("Roughness: %.3f  Metallic: %.3f", mesh->Roughness, mesh->Metallic);
			ImGui::Text("OverrideRM: %s", mesh->bOverrideRoughnessMetallic ? "yes" : "no");
			ImGui::Text("Visible: %s  RayTracing: %s",
				mesh->bVisible ? "yes" : "no",
				mesh->bRayTracing ? "yes" : "no");
		}

		// Grass-only block: wind / bend / render-distance affect every grass
		// mesh in the world (they're global frame state), but it only makes
		// sense to surface them when the user has actually selected a grass
		// entity. The script (terrain_demo.luau) still writes these every
		// frame — these sliders are applied *after* the script tick so they
		// stick within the same frame.
		if (Host->IsEntityGrassMesh(SelectedEntity))
		{
			// --- Per-blade recipe editor + Regenerate button ---
			if (ImGui::CollapsingHeader("Grass blades", ImGuiTreeNodeFlags_DefaultOpen))
			{
				Corona::SceneRecipe recipe;
				if (Host->GetEntityMeshRecipeForScript(SelectedEntity, recipe))
				{
					// Snap the inspector's pending buffer to the live recipe
					// whenever the selection changes — otherwise we'd carry
					// stale edits from one entity to another.
					if (GrassEditEntity != SelectedEntity)
					{
						GrassEditEntity = SelectedEntity;
						GrassEditBladeCount = static_cast<int>(recipe.BladeCount);
						GrassEditBladeHeight = recipe.BladeHeight;
						GrassEditBladeSegments = static_cast<int>(recipe.BladeSegments == 0u ? 4u : recipe.BladeSegments);
						GrassEditSeed = static_cast<int>(recipe.Seed == 0u ? 1u : recipe.Seed);
						GrassEditProcedural = recipe.bProceduralPath;
						// Sync the procedural live blade height to the recipe
						// so the unified slider starts in agreement.
						Host->GrassProceduralBladeHeight = recipe.BladeHeight;
					}
					ImGui::Text("primitive: %s",
						recipe.RecipeKind == Corona::SceneRecipe::Kind::GrassOnTerrain
							? "GRASS_ON_TERRAIN" : "GRASS");
					ImGui::Checkbox("Procedural shader (vertex-pulling, no VB)##gb", &GrassEditProcedural);
					ImGui::Separator();
					ImGui::TextDisabled("Live (no rebuild):");
					ImGui::SliderInt("density (blades / cell)##gb",
						&Host->GrassProceduralBladesPerCell, 1, 5000);
					ImGui::DragFloat("blade width scale##gb",
						&Host->GrassProceduralBladeWidthScale, 0.005f, 0.005f, 0.5f);
					ImGui::SliderFloat("blade tip width##gb",
						&Host->GrassProceduralBladeTipWidthScale, 0.0f, 1.0f);
					if (ImGui::DragFloat("blade height (m)##gb",
						&Host->GrassProceduralBladeHeight, 0.05f, 0.05f, 30.0f))
					{
						// Keep the recipe-staging height in lockstep so an
						// Apply rebuilds at the value the user just dialed in.
						GrassEditBladeHeight = Host->GrassProceduralBladeHeight;
					}
					ImGui::Separator();
					ImGui::TextDisabled("Recipe (Apply to rebuild):");
					ImGui::InputInt("blade count##gb",   &GrassEditBladeCount, 10000, 100000);
					ImGui::SliderInt("blade segments##gb", &GrassEditBladeSegments, 1, 16);
					ImGui::InputInt("seed##gb", &GrassEditSeed);
					if (GrassEditBladeCount < 1)    GrassEditBladeCount = 1;
					if (GrassEditBladeSegments < 1) GrassEditBladeSegments = 1;
					if (GrassEditSeed < 1)          GrassEditSeed = 1;
					// "Are the staged values different from what's actually
					// being drawn?" — if yes, surface a hint + highlight the
					// Regenerate button so the user knows the slider edits
					// don't apply until the mesh is rebuilt.
					const bool pendingChange =
						static_cast<uint32_t>(GrassEditBladeCount)    != recipe.BladeCount ||
						std::fabs(GrassEditBladeHeight - recipe.BladeHeight) > 1e-4f ||
						static_cast<uint32_t>(GrassEditBladeSegments) != (recipe.BladeSegments == 0u ? 4u : recipe.BladeSegments) ||
						static_cast<uint32_t>(GrassEditSeed)          != recipe.Seed ||
						GrassEditProcedural                            != recipe.bProceduralPath;
					if (pendingChange)
					{
						ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f),
							"pending changes — click Apply to rebuild");
						ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f, 0.35f, 0.10f, 1.0f));
						ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.50f, 0.15f, 1.0f));
						ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.85f, 0.60f, 0.20f, 1.0f));
					}
					const bool regenClicked = ImGui::Button(
						pendingChange ? "Apply pending changes##gb" : "Apply (rebuild mesh)##gb",
						ImVec2(-1, 0));
					if (pendingChange)
						ImGui::PopStyleColor(3);
					if (regenClicked)
					{
						const bool ok = Host->RegenerateGrassEntityForScript(
							SelectedEntity,
							static_cast<uint32_t>(GrassEditBladeCount),
							GrassEditBladeHeight,
							static_cast<uint32_t>(GrassEditSeed),
							static_cast<uint32_t>(GrassEditBladeSegments),
							GrassEditProcedural);
						// SelectedEntity was destroyed by Regenerate; clear
						// so the user can re-pick the new mesh in the list.
						SelectedEntity = CoronaECS::Entity();
						GrassEditEntity = CoronaECS::Entity();
						// Auto-persist to the active map so the new params
						// survive a relaunch. Without this the regenerated
						// mesh lives only in the running process.
						std::string statusMsg = ok ? "Grass regenerated" : "Grass regenerate failed";
						if (ok)
						{
							const std::wstring& mapName = Host->GetCurrentMapName();
							if (!mapName.empty())
							{
								std::wstring saveErr;
								if (Host->SaveMapToFile(mapName, &saveErr))
									statusMsg += " + map auto-saved";
								else
									statusMsg += " (map save failed: " +
										std::string(saveErr.begin(), saveErr.end()) + ")";
							}
							else
							{
								statusMsg += " (no map loaded — Save Map manually)";
							}
						}
						LastSaveStatus = std::move(statusMsg);
						LastSaveStatusAt = std::chrono::steady_clock::now();
						return;
					}
				}
			}

			if (ImGui::CollapsingHeader("Grass / Wind", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::DragFloat("Render distance (m)##g",
				&Host->GrassRenderDistance, 1.0f, 0.0f, 4000.0f);

			float bendRadius    = Host->RenderFrameGrassBendParams.x;
			float bendMaxHeight = Host->RenderFrameGrassBendParams.y;
			if (ImGui::DragFloat("Bend radius (m)##g", &bendRadius, 0.05f, 0.0f, 60.0f) ||
				ImGui::DragFloat("Bend max blade height (m)##g", &bendMaxHeight, 0.05f, 0.05f, 30.0f))
			{
				Host->SetGrassBendParamsForScript(bendRadius, bendMaxHeight);
			}

			// WindParams.xz = direction (XZ-only), .w = strength.
			float windDir[2] = {
				Host->RenderFrameWindParams.x,
				Host->RenderFrameWindParams.z,
			};
			float windStrength = Host->RenderFrameWindParams.w;
			float windTempFreq  = Host->RenderFrameWindTuning.x;
			float windSpaceFreq = Host->RenderFrameWindTuning.y;
			bool changed = false;
			if (ImGui::DragFloat2("Wind dir XZ##g", windDir, 0.01f, -1.0f, 1.0f))
				changed = true;
			if (ImGui::SliderFloat("Wind strength##g", &windStrength, 0.0f, 5.0f))
				changed = true;
			if (ImGui::SliderFloat("Wind temporal freq##g", &windTempFreq, 0.0f, 40.0f))
				changed = true;
			if (ImGui::SliderFloat("Wind spatial freq##g", &windSpaceFreq, 0.0f, 0.5f))
				changed = true;
			if (changed)
			{
				Host->SetWindParamsForScript(
					windDir[0], windDir[1], windStrength,
					windTempFreq > 0.0f ? windTempFreq : 0.0001f,
					windSpaceFreq > 0.0f ? windSpaceFreq : 0.0001f);
			}
		}
	}
	}

	if (ecs.HasLight(SelectedEntity))
	{
		CoronaECS::LightComponent comp;
		Host->GetEntityLightForScript(SelectedEntity, comp);
		char lightHeader[64];
		snprintf(lightHeader, sizeof(lightHeader), "Light (%s)", LightTypeDisplayName(comp.Type));
		if (ImGui::CollapsingHeader(lightHeader, ImGuiTreeNodeFlags_DefaultOpen))
		{
			bool changed = false;
			ImGui::Text("Type: %s", LightTypeDisplayName(comp.Type));
			if (ImGui::Checkbox("Enabled##l", &comp.bEnabled)) changed = true;
			if (ImGui::Checkbox("Cast Shadow##l", &comp.bCastShadow)) changed = true;
			if (ImGui::DragFloat("Intensity##l", &comp.Intensity, 0.5f, 0.0f, 5000.0f)) changed = true;
			if (comp.Type == CoronaECS::LightType::Directional)
			{
				// 3D rotation gizmo — same axis remap as the legacy debug
				// panel in Corona.cpp so the arrow points the way the sun
				// actually shines. Falls back to the numeric drag below
				// for fine-tuning.
				glm::vec3 gizmoDir(comp.Direction.z, -comp.Direction.y, -comp.Direction.x);
				if (ImGui::gizmo3D("##light_dir_gizmo", gizmoDir, 120.0f))
				{
					comp.Direction = glm::vec3(-gizmoDir.z, -gizmoDir.y, gizmoDir.x);
					changed = true;
				}
			}
			float dir[3] = { comp.Direction.x, comp.Direction.y, comp.Direction.z };
			if (ImGui::DragFloat3("Direction##l", dir, 0.01f, -1.0f, 1.0f))
			{
				comp.Direction = glm::vec3(dir[0], dir[1], dir[2]);
				changed = true;
			}
			float col[3] = { comp.Color.r, comp.Color.g, comp.Color.b };
			if (ImGui::ColorEdit3("Color##l", col))
			{
				comp.Color = glm::vec3(col[0], col[1], col[2]);
				changed = true;
			}
			if (ImGui::DragFloat("Radius##l", &comp.Radius, 5.0f, 0.0f, 20000.0f)) changed = true;
			if (comp.Type == CoronaECS::LightType::Spot)
			{
				constexpr float RadToDeg = 57.2957795f;
				constexpr float DegToRad = 0.0174532925f;
				float innerDeg = comp.InnerConeAngle * RadToDeg;
				float outerDeg = comp.OuterConeAngle * RadToDeg;
				if (ImGui::DragFloat("Inner Cone##l", &innerDeg, 0.25f, 0.0f, 179.0f))
				{
					comp.InnerConeAngle = innerDeg * DegToRad;
					changed = true;
				}
				if (ImGui::DragFloat("Outer Cone##l", &outerDeg, 0.25f, 0.1f, 180.0f))
				{
					comp.OuterConeAngle = outerDeg * DegToRad;
					changed = true;
				}
			}
			if (changed)
				Host->SetEntityLightForScript(SelectedEntity, comp, /*persist*/ false);
		}
	}

	if (auto* c = ecs.GetCamera(SelectedEntity))
	{
		if (ImGui::CollapsingHeader("Camera"))
		{
			ImGui::Text("Active: %s", c->bActive ? "yes" : "no");
			ImGui::DragFloat("FoV##c", &c->Fov, 0.005f, 0.1f, 3.0f);
			ImGui::DragFloat("Near##c", &c->NearPlane, 0.05f, 0.001f, 100.0f);
			ImGui::DragFloat("Far##c",  &c->FarPlane, 10.0f, 10.0f, 500000.0f);
			float look[3] = { c->LookDirection.x, c->LookDirection.y, c->LookDirection.z };
			if (ImGui::DragFloat3("Look##c", look, 0.01f, -1.0f, 1.0f))
				c->LookDirection = glm::vec3(look[0], look[1], look[2]);
		}
	}

	if (auto* p = ecs.GetPhysics(SelectedEntity))
	{
		if (ImGui::CollapsingHeader("Physics"))
		{
			ImGui::Text("Query: %s", p->bQueryEnabled ? "on" : "off");
			ImGui::Text("Shape: %s",
				p->CollisionShape == CoronaECS::PhysicsCollisionShape::Box
				? "box" : "triangle_mesh");
		}
	}
}

void CoronaSceneInspector::DrawBottomToggleButton()
{
	const ImGuiViewport* vp = ImGui::GetMainViewport();
	const float bw = 110.0f;
	ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 6.0f, vp->WorkPos.y + vp->WorkSize.y - kToggleBtnH - 6.0f));
	ImGui::SetNextWindowSize(ImVec2(bw, kToggleBtnH));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(2.0f, 2.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
	const ImGuiWindowFlags flags =
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar;
	ImGui::Begin("##scene_inspector_toggle", nullptr, flags);
	if (ImGui::Button(bVisible ? "Hide Scene" : "Show Scene", ImVec2(-1, -1)))
		bVisible = !bVisible;
	ImGui::End();
	ImGui::PopStyleVar(2);
}
