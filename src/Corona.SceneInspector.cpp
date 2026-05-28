#include "stdafx.h"
#include "Corona.SceneInspector.h"

#include "Corona.h"
#include "imgui.h"
#include "imGuIZMO.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
	// Sticky width for the left dock so toggling visibility doesn't jitter.
	constexpr float kPanelWidth = 320.0f;
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
	ImGui::SetNextWindowSize(ImVec2(kPanelWidth, h - kToggleBtnH - 4.0f));

	const ImGuiWindowFlags flags =
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
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

	ImGui::Separator();

	// Entity list — top 60% of the panel.
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

	if (entities.empty())
	{
		ImGui::TextDisabled("(scene is empty)");
	}
	else
	{
		for (auto e : entities)
			DrawEntityRow(e);
	}

	ImGui::EndChild();

	// Process any deferred delete request from the per-row context menu.
	// Done here (after the list loop) so we never destroy mid-iteration.
	if (PendingDeleteEntity.IsValid())
	{
		auto& mutableEcs = Host->GetEntityWorld();
		if (mutableEcs.IsAlive(PendingDeleteEntity))
			mutableEcs.DestroyEntity(PendingDeleteEntity);
		if (SelectedEntity == PendingDeleteEntity)
			SelectedEntity = CoronaECS::Entity();
		PendingDeleteEntity = CoronaECS::Entity();
	}

	ImGui::Separator();
	DrawSelectedEntityDetails();

	ImGui::End();
	ImGui::PopStyleVar();
	ImGui::PopStyleColor();
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
	if (ecs.HasLight(entity))    badges += "[L]";
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
			bool changed = false;
			if (ImGui::DragFloat3("Position##t", arrPos, 0.05f))
				changed = true;
			if (ImGui::DragFloat3("Rotation##t", arrRot, 1.0f))
				changed = true;
			if (useScale)
			{
				float arrScale[3] = { scale.x, scale.y, scale.z };
				if (ImGui::DragFloat3("Scale##t", arrScale, 0.01f, 0.001f, 100.0f))
				{
					scale = glm::vec3(arrScale[0], arrScale[1], arrScale[2]);
					changed = true;
				}
			}
			else
			{
				if (ImGui::DragFloat("TargetExtent##t", &targetExtent, 0.05f, 0.001f, 10000.0f))
					changed = true;
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

		if (ImGui::CollapsingHeader("Mesh"))
		{
			ImGui::Text("RenderObject: %u", mesh->RenderObjectHandle);
			ImGui::Text("Roughness: %.3f  Metallic: %.3f", mesh->Roughness, mesh->Metallic);
			ImGui::Text("OverrideRM: %s", mesh->bOverrideRoughnessMetallic ? "yes" : "no");
			ImGui::Text("Visible: %s  RayTracing: %s",
				mesh->bVisible ? "yes" : "no",
				mesh->bRayTracing ? "yes" : "no");
		}
	}

	if (ecs.HasLight(SelectedEntity))
	{
		if (ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen))
		{
			CoronaECS::LightComponent comp;
			Host->GetEntityLightForScript(SelectedEntity, comp);
			bool changed = false;
			const char* typeStr = (comp.Type == CoronaECS::LightType::Directional) ? "directional" : "point";
			ImGui::Text("Type: %s", typeStr);
			if (ImGui::Checkbox("Enabled##l", &comp.bEnabled)) changed = true;
			if (ImGui::DragFloat("Intensity##l", &comp.Intensity, 0.05f, 0.0f, 100.0f)) changed = true;
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
			if (ImGui::DragFloat("Radius##l", &comp.Radius, 1.0f, 0.0f, 4000.0f)) changed = true;
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
			ImGui::DragFloat("Far##c",  &c->FarPlane, 10.0f, 10.0f, 100000.0f);
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
