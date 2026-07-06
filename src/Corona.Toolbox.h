#pragma once

#include <cstdint>

#include "glm/glm.hpp"

// Spawn toolbox — small ImGui window with buttons that drop a directional
// light, point light, procedural box, or procedural sphere in front of the
// active camera. Toggled by a bottom-of-screen button next to the scene
// inspector / asset explorer toggles.

class Corona;

class CoronaToolbox
{
public:
	explicit CoronaToolbox(Corona* host);
	~CoronaToolbox();

	void RenderImGui();
	void Toggle() { bVisible = !bVisible; }
	bool IsVisible() const { return bVisible; }

private:
	void DrawBottomToggleButton();
	void SpawnDirectionalLight();
	void SpawnPointLight();
	void SpawnSpotLight();
	void SpawnBox();
	void SpawnSphere();
	void SpawnGrass();
	void SpawnGlassCube();
	void SpawnGlassSphere();
	void SpawnGlassPlane();
	void SpawnGlassDiamond();
	void SpawnGlassMesh(
		const char* namePrefix,
		uint32_t sceneHandle,
		const glm::vec3& position,
		const glm::vec3& rotationDegrees,
		float targetExtent,
		const glm::vec3& scale,
		bool bUseScale,
		float roughness);

	Corona* Host;
	bool bVisible = false;
	int NextSpawnId = 1;
};
