#pragma once

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
	void SpawnBox();
	void SpawnSphere();

	Corona* Host;
	bool bVisible = false;
	int NextSpawnId = 1;
};
