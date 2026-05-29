#include "stdafx.h"
#include "Corona.Toolbox.h"

#include "Corona.h"
#include "EntityComponentSystem.h"
#include "TerrainComponent.h"
#include "imgui.h"

#include <cstdio>
#include <string>

namespace
{
	constexpr float kToggleBtnH = 28.0f;

	std::string NextName(const char* prefix, int& counter)
	{
		char buf[64];
		snprintf(buf, sizeof(buf), "%s_%d", prefix, counter++);
		return std::string(buf);
	}

	glm::vec3 SpawnPosInFront(Corona* host, float dist = 3.0f, bool snapToTerrain = true)
	{
		const glm::vec3 cp = host->GetCameraPositionForConsole();
		const glm::vec3 cl = host->GetCameraLookDirForConsole();
		glm::vec3 pos = cp + cl * dist;
		if (snapToTerrain)
		{
			if (Terrain::Component* terrain = host->GetActiveTerrainForConsole())
				pos.y = terrain->SampleHeight(pos.x, pos.z) + 0.75f;
		}
		return pos;
	}
}

CoronaToolbox::CoronaToolbox(Corona* host) : Host(host) {}
CoronaToolbox::~CoronaToolbox() = default;

void CoronaToolbox::RenderImGui()
{
	DrawBottomToggleButton();
	if (!bVisible)
		return;

	const ImGuiViewport* vp = ImGui::GetMainViewport();
	const ImVec2 size(260.0f, 220.0f);
	ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - size.x - 8.0f, vp->WorkPos.y + 40.0f),
		ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);

	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
	if (ImGui::Begin("Toolbox", &bVisible, flags))
	{
		ImGui::TextDisabled("spawn in front of camera");
		ImGui::Separator();
		if (ImGui::Button("Directional Light", ImVec2(-1, 0))) SpawnDirectionalLight();
		if (ImGui::Button("Point Light",       ImVec2(-1, 0))) SpawnPointLight();
		if (ImGui::Button("Box",               ImVec2(-1, 0))) SpawnBox();
		if (ImGui::Button("Sphere",            ImVec2(-1, 0))) SpawnSphere();
		if (ImGui::Button("Grass (procedural)",ImVec2(-1, 0))) SpawnGrass();
	}
	ImGui::End();
}

void CoronaToolbox::SpawnDirectionalLight()
{
	const std::string name = NextName("Light_Dir", NextSpawnId);
	const CoronaECS::Entity e = Host->CreateEntity(name);
	CoronaECS::LightComponent comp;
	comp.Type = CoronaECS::LightType::Directional;
	comp.bEnabled = true;
	comp.Direction = glm::normalize(glm::vec3(-0.35f, -1.0f, 0.45f));
	comp.Color = glm::vec3(1.0f, 0.96f, 0.85f);
	comp.Intensity = 1.5f;
	comp.Radius = 320.0f;
	Host->SetEntityLightForScript(e, comp, /*persist*/ false);
}

void CoronaToolbox::SpawnPointLight()
{
	const std::string name = NextName("Light_Pt", NextSpawnId);
	const CoronaECS::Entity e = Host->CreateEntity(name);
	const glm::vec3 pos = SpawnPosInFront(Host, /*dist*/ 4.0f, /*snap*/ true) + glm::vec3(0, 1.5f, 0);
	Host->SetEntityTransformForScript(e, pos, glm::vec3(0.0f), glm::vec3(1.0f));
	CoronaECS::LightComponent comp;
	comp.Type = CoronaECS::LightType::Point;
	comp.bEnabled = true;
	comp.Color = glm::vec3(1.0f, 0.85f, 0.4f);
	comp.Intensity = 10.0f;
	comp.Radius = 20.0f;
	Host->SetEntityLightForScript(e, comp, /*persist*/ false);
}

void CoronaToolbox::SpawnBox()
{
	const std::string name = NextName("Box", NextSpawnId);
	const Corona::ScriptSceneHandle sh = Host->CreateProceduralBoxSceneForScript(
		glm::vec3(0.55f, 0.55f, 0.6f),
		/*bUseBrickTexture*/ false,
		/*uvRepeat*/         1.0f,
		/*textureKind*/      L"flat",
		/*uvRepeatY*/        0.0f,
		/*bFrontOnly*/       false);
	if (sh == Corona::InvalidScriptSceneHandle)
		return;
	const CoronaECS::Entity e = Host->CreateEntity(name);
	const glm::vec3 pos = SpawnPosInFront(Host, 3.5f, /*snap*/ true);
	Host->AddMeshComponentForScript(
		e, sh, pos, glm::vec3(0.0f),
		/*targetExtent*/ 1.0f, glm::vec3(1.0f), /*useScale*/ false,
		/*roughness*/ 0.6f, /*metallic*/ 0.0f, /*overrideRM*/ true,
		/*visible*/ true, /*rayTracing*/ true, /*physicsQuery*/ true);
}

void CoronaToolbox::SpawnSphere()
{
	const std::string name = NextName("Sphere", NextSpawnId);
	// Radius 1.0 (was 0.5) — the previous tiny sphere was hard to see in
	// scenes the size of Sponza.
	const Corona::ScriptSceneHandle sh = Host->CreateProceduralSphereSceneForScript(
		/*radius*/ 1.0f, /*rings*/ 32, /*segments*/ 48);
	if (sh == Corona::InvalidScriptSceneHandle)
		return;
	const CoronaECS::Entity e = Host->CreateEntity(name);
	const glm::vec3 pos = SpawnPosInFront(Host, 3.5f, /*snap*/ true);
	Host->AddMeshComponentForScript(
		e, sh, pos, glm::vec3(0.0f),
		/*targetExtent*/ 1.0f, glm::vec3(1.0f), /*useScale*/ false,
		/*roughness*/ 0.4f, /*metallic*/ 0.0f, /*overrideRM*/ true,
		/*visible*/ true, /*rayTracing*/ true, /*physicsQuery*/ true);
}

void CoronaToolbox::SpawnGrass()
{
	const std::string name = NextName("Grass", NextSpawnId);
	// Each spawn picks a distinct seed so placement hashes diverge — when
	// the user drops several grass entities they form a varied carpet
	// instead of stacking identical blades.
	const uint32_t seed = static_cast<uint32_t>(20260529u + NextSpawnId * 7919u);
	const Corona::ScriptSceneHandle sh = Host->CreateProceduralGrassOnTerrainSceneInstancedForScript(
		/*numBlades*/   200'000u,
		/*bladeHeight*/ 1.2f,
		/*seed*/        seed,
		/*bladeSegments*/ 8u);
	if (sh == Corona::InvalidScriptSceneHandle)
		return;
	const CoronaECS::Entity e = Host->CreateEntity(name);
	// Procedural grass mesh is centered at origin and the shader anchors
	// blade XZ to the player's grass-bend origin, so the entity transform
	// is identity. Toolbox positions the entity at the camera target so
	// the inspector "Focus" button finds it easily.
	const glm::vec3 pos = SpawnPosInFront(Host, 3.0f, /*snap*/ true);
	Host->AddMeshComponentForScript(
		e, sh, pos, glm::vec3(0.0f),
		/*targetExtent*/ 1.0f, glm::vec3(1.0f, 1.0f, 1.0f), /*useScale*/ true,
		/*roughness*/ 0.85f, /*metallic*/ 0.0f, /*overrideRM*/ true,
		/*visible*/ true, /*rayTracing*/ false, /*physicsQuery*/ false);
}

void CoronaToolbox::DrawBottomToggleButton()
{
	const ImGuiViewport* vp = ImGui::GetMainViewport();
	const float bw = 110.0f;
	// Third button in the bottom-left row (scene, assets, toolbox).
	const float x = vp->WorkPos.x + 6.0f + 110.0f + 6.0f + 110.0f + 6.0f;
	ImGui::SetNextWindowPos(ImVec2(x, vp->WorkPos.y + vp->WorkSize.y - kToggleBtnH - 6.0f));
	ImGui::SetNextWindowSize(ImVec2(bw, kToggleBtnH));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(2.0f, 2.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
	const ImGuiWindowFlags flags =
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar;
	ImGui::Begin("##toolbox_toggle", nullptr, flags);
	if (ImGui::Button(bVisible ? "Hide Toolbox" : "Show Toolbox", ImVec2(-1, -1)))
		bVisible = !bVisible;
	ImGui::End();
	ImGui::PopStyleVar(2);
}
