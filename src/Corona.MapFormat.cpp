// Corona map serialization — see Corona::SaveMapToFile / LoadMapFromFile
// declared in Corona.h. The on-disk format is a Luau-syntax table that the
// engine's existing Lua VM executes at load time, so there's no separate
// JSON parser to maintain. Format is human-readable + diff-friendly.

#include "stdafx.h"
#include "Corona.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

#include "EntityComponentSystem.h"
#include "PlatformSystem.h"
#include "TerrainComponent.h"
#include "Utils.h"

extern "C"
{
#include "lua.h"
#include "lualib.h"
#include "luacode.h"
}

void AppendCpuRuntimeTrace(const std::wstring& line);

namespace
{
	std::string EscapeLuaString(const std::string& s)
	{
		std::string out;
		out.reserve(s.size() + 2);
		out.push_back('"');
		for (char c : s)
		{
			switch (c)
			{
			case '"':  out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n";  break;
			case '\r': out += "\\r";  break;
			case '\t': out += "\\t";  break;
			default:
				if (static_cast<unsigned char>(c) < 0x20)
				{
					char buf[8];
					snprintf(buf, sizeof(buf), "\\%d", static_cast<unsigned char>(c));
					out += buf;
				}
				else
				{
					out.push_back(c);
				}
				break;
			}
		}
		out.push_back('"');
		return out;
	}

	std::string Vec3Lua(const glm::vec3& v)
	{
		std::ostringstream o;
		o.setf(std::ios::fmtflags(0), std::ios::floatfield);
		o << "{" << v.x << "," << v.y << "," << v.z << "}";
		return o.str();
	}

	std::string Vec4Lua(const glm::vec4& v)
	{
		std::ostringstream o;
		o << "{" << v.x << "," << v.y << "," << v.z << "," << v.w << "}";
		return o.str();
	}

	std::string ReadFile(const std::filesystem::path& path)
	{
		std::ifstream f(path, std::ios::binary);
		if (!f)
			return std::string();
		std::stringstream ss;
		ss << f.rdbuf();
		return ss.str();
	}

	// Helpers for reading fields from a Lua table on the stack (always at -1
	// unless the index is overridden). Each helper restores the stack.
	bool LuaGetNumber(lua_State* L, int tableIdx, const char* key, double& out)
	{
		lua_getfield(L, tableIdx, key);
		const bool ok = lua_isnumber(L, -1) != 0;
		if (ok)
			out = lua_tonumber(L, -1);
		lua_pop(L, 1);
		return ok;
	}

	bool LuaGetBool(lua_State* L, int tableIdx, const char* key, bool& out)
	{
		lua_getfield(L, tableIdx, key);
		const bool ok = lua_isboolean(L, -1) != 0;
		if (ok)
			out = lua_toboolean(L, -1) != 0;
		lua_pop(L, 1);
		return ok;
	}

	bool LuaGetString(lua_State* L, int tableIdx, const char* key, std::string& out)
	{
		lua_getfield(L, tableIdx, key);
		const bool ok = lua_isstring(L, -1) != 0;
		if (ok)
			out = lua_tostring(L, -1);
		lua_pop(L, 1);
		return ok;
	}

	bool LuaGetVec3(lua_State* L, int tableIdx, const char* key, glm::vec3& out)
	{
		lua_getfield(L, tableIdx, key);
		if (!lua_istable(L, -1))
		{
			lua_pop(L, 1);
			return false;
		}
		lua_rawgeti(L, -1, 1); out.x = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : out.x; lua_pop(L, 1);
		lua_rawgeti(L, -1, 2); out.y = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : out.y; lua_pop(L, 1);
		lua_rawgeti(L, -1, 3); out.z = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : out.z; lua_pop(L, 1);
		lua_pop(L, 1);
		return true;
	}

	bool LuaGetVec4(lua_State* L, int tableIdx, const char* key, glm::vec4& out)
	{
		lua_getfield(L, tableIdx, key);
		if (!lua_istable(L, -1))
		{
			lua_pop(L, 1);
			return false;
		}
		lua_rawgeti(L, -1, 1); out.x = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : out.x; lua_pop(L, 1);
		lua_rawgeti(L, -1, 2); out.y = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : out.y; lua_pop(L, 1);
		lua_rawgeti(L, -1, 3); out.z = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : out.z; lua_pop(L, 1);
		lua_rawgeti(L, -1, 4); out.w = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : out.w; lua_pop(L, 1);
		lua_pop(L, 1);
		return true;
	}
}

std::filesystem::path Corona::ResolveMapPath(const std::wstring& name) const
{
	std::filesystem::path mapsDir = RuntimePaths::RootDirectory() / L"assets" / L"maps";
	std::error_code ec;
	std::filesystem::create_directories(mapsDir, ec);
	std::wstring fileName = name;
	if (fileName.find(L'.') == std::wstring::npos)
		fileName += L".map";
	return mapsDir / fileName;
}

void Corona::ClearScriptSpawnedScene()
{
	// Remove every SceneObject that the script layer created. Each removal
	// also cleans up the matching ECS Entity if the script owned it.
	std::vector<SceneObjectHandle> handles;
	handles.reserve(ScriptObjects.size());
	for (const auto& [h, _] : ScriptObjects)
		handles.push_back(h);
	for (SceneObjectHandle h : handles)
		RemoveSceneObject(h);

	ScriptObjects.clear();
	ScriptScenes.clear();
	ScriptSceneByPath.clear();

	// Drop the singleton terrain + grass references; the loader will rebuild
	// them via the recipe replay.
	ActiveTerrain.reset();
	ActiveGrassMesh.reset();
	ActiveGrassMaterial.reset();
	ActiveGrassChunks.clear();
	ActiveGrassScene.reset();

	// Clear any non-script-attached entities too (lights / cameras created
	// via Luau but without a MeshComponent linking to a SceneObject). A
	// load is "wipe then replay" so the safest default is full wipe.
	const std::vector<CoronaECS::Entity> remaining = EntityWorld.GetEntitiesWithMeshAndTransform();
	for (auto e : remaining)
		EntityWorld.DestroyEntity(e);
	for (auto e : EntityWorld.GetEntitiesWithLight())
		EntityWorld.DestroyEntity(e);
	// Cameras: handled by iterating active camera entity if present.
	const auto activeCam = EntityWorld.GetActiveCameraEntity();
	if (activeCam.IsValid())
		EntityWorld.DestroyEntity(activeCam);
	// Script-only entities (HUD / game controllers that have no mesh /
	// light / camera). Iterate after the others so we only catch the
	// remaining script anchors.
	for (auto e : EntityWorld.GetEntitiesWithScript())
		EntityWorld.DestroyEntity(e);

	// Reset the cached engine-singleton handles — without this, the
	// destroyed-light slot stays as a stale id and the next
	// InitializeMainDirectionalLightEntity() check thinks the slot is
	// missing, spawning a duplicate alongside whatever the loader replays.
	MainDirectionalLightEntity = CoronaECS::Entity();
	MainCameraEntity = CoronaECS::Entity();
}

bool Corona::SaveMapToFile(const std::wstring& name, std::wstring* outError)
{
	const std::filesystem::path path = ResolveMapPath(name);
	std::ostringstream out;
	out << "-- Corona map (auto-generated; safe to edit by hand)\n";
	out << "return {\n";
	out << "  version = 1,\n";
	out << "  name    = " << EscapeLuaString(PlatformWideToUtf8(name)) << ",\n";
	out << "  entities = {\n";

	const auto& ecs = EntityWorld;

	// --- Mesh entities ---
	auto meshEntities = ecs.GetEntitiesWithMeshAndTransform();
	for (auto entity : meshEntities)
	{
		const auto* mesh = ecs.GetMesh(entity);
		if (!mesh)
			continue;

		const SceneObjectHandle soh = static_cast<SceneObjectHandle>(mesh->RenderObjectHandle);
		const auto soIt = ScriptObjects.find(soh);
		if (soIt == ScriptObjects.end())
			continue; // not a script-spawned object — skip (e.g. engine-internal demo loads)
		const ScriptObjectState& so = soIt->second;
		const auto sceneIt = ScriptScenes.find(so.SceneHandle);
		if (sceneIt == ScriptScenes.end())
			continue;
		const SceneRecipe& recipe = sceneIt->second.Recipe;

		std::string entityName = "entity";
		if (const auto* n = ecs.GetName(entity))
			entityName = *n;

		out << "    {\n";
		out << "      name = " << EscapeLuaString(entityName) << ",\n";
		const char* primitiveStr = "asset";
		switch (recipe.RecipeKind)
		{
		case SceneRecipe::Kind::Terrain:        primitiveStr = "TERRAIN"; break;
		case SceneRecipe::Kind::GrassOnTerrain: primitiveStr = "GRASS_ON_TERRAIN"; break;
		case SceneRecipe::Kind::Grass:          primitiveStr = "GRASS"; break;
		case SceneRecipe::Kind::BlockCharacter: primitiveStr = "block_character"; break;
		case SceneRecipe::Kind::Asset:          primitiveStr = "asset"; break;
		}
		// Attached scripts (file + native). Saved alongside mesh so the
		// loader replays the attach calls after the entity is re-spawned.
		// Without this, gameplay-mode entities (dungeon character / enemy
		// / sun controllers, platformer spine controllers, etc.) lose
		// their behaviour on map reload — the entity exists but has no
		// controller bound to it.
		if (const auto* script = ecs.GetScript(entity); script && !script->Instances.empty())
		{
			out << "      scripts = {\n";
			for (const auto& inst : script->Instances)
			{
				if (!inst.SourceName.empty())
					out << "        { file = " << EscapeLuaString(PlatformWideToUtf8(inst.SourceName)) << " },\n";
				else if (!inst.NativeScriptName.empty())
					out << "        { native = " << EscapeLuaString(inst.NativeScriptName) << " },\n";
			}
			out << "      },\n";
		}
		out << "      mesh = {\n";
		out << "        primitive = " << EscapeLuaString(primitiveStr) << ",\n";
		switch (recipe.RecipeKind)
		{
		case SceneRecipe::Kind::Terrain:
			out << "        params = { seed = " << recipe.Seed << " },\n";
			break;
		case SceneRecipe::Kind::GrassOnTerrain:
			out << "        params = { blade_count = " << recipe.BladeCount
			    << ", blade_height = " << recipe.BladeHeight
			    << ", blade_segments = " << recipe.BladeSegments
			    << ", procedural = " << (recipe.bProceduralPath ? "true" : "false")
			    << ", seed = " << recipe.Seed << " },\n";
			break;
		case SceneRecipe::Kind::Grass:
			out << "        params = { blade_count = " << recipe.BladeCount
			    << ", area_size = " << recipe.AreaSize
			    << ", blade_height = " << recipe.BladeHeight
			    << ", blade_segments = " << recipe.BladeSegments
			    << ", seed = " << recipe.Seed << " },\n";
			break;
		case SceneRecipe::Kind::BlockCharacter:
			out << "        params = { seed = " << recipe.Seed << " },\n";
			break;
		case SceneRecipe::Kind::Asset:
			out << "        path = " << EscapeLuaString(PlatformWideToUtf8(recipe.AssetPath)) << ",\n";
			break;
		}
		out << "        transform = {\n";
		out << "          position = " << Vec3Lua(so.Position) << ",\n";
		out << "          rotation = " << Vec3Lua(so.RotationDegrees) << ",\n";
		if (so.bUseScale)
			out << "          scale = " << Vec3Lua(so.Scale) << ",\n";
		else
			out << "          target_extent = " << so.TargetExtent << ",\n";
		out << "        },\n";
		out << "        material = {\n";
		out << "          roughness = " << mesh->Roughness << ",\n";
		out << "          metallic  = " << mesh->Metallic << ",\n";
		out << "          override  = " << (mesh->bOverrideRoughnessMetallic ? "true" : "false") << ",\n";
		out << "        },\n";
		out << "        ray_tracing = " << (mesh->bRayTracing ? "true" : "false") << ",\n";
		out << "        visible     = " << (mesh->bVisible ? "true" : "false") << ",\n";
		out << "      },\n";
		out << "    },\n";
	}

	// --- Light entities ---
	for (auto entity : ecs.GetEntitiesWithLight())
	{
		const auto* light = ecs.GetLight(entity);
		if (!light)
			continue;
		std::string entityName = "light";
		if (const auto* n = ecs.GetName(entity))
			entityName = *n;
		out << "    {\n";
		out << "      name = " << EscapeLuaString(entityName) << ",\n";
		// Attached scripts for light entities (sun controllers, etc.).
		if (const auto* script = ecs.GetScript(entity); script && !script->Instances.empty())
		{
			out << "      scripts = {\n";
			for (const auto& inst : script->Instances)
			{
				if (!inst.SourceName.empty())
					out << "        { file = " << EscapeLuaString(PlatformWideToUtf8(inst.SourceName)) << " },\n";
				else if (!inst.NativeScriptName.empty())
					out << "        { native = " << EscapeLuaString(inst.NativeScriptName) << " },\n";
			}
			out << "      },\n";
		}
		out << "      light = {\n";
		out << "        type      = " << EscapeLuaString(light->Type == CoronaECS::LightType::Directional ? "directional" : "point") << ",\n";
		// Position only matters for point lights (directional sun is shared
		// by the whole world). Persisting it makes inspector position edits
		// survive map reload — without this the runtime PointLight.Position
		// got reset to the entity-creation default on every load.
		if (light->Type == CoronaECS::LightType::Point)
		{
			if (const auto* trans = ecs.GetTransform(entity))
				out << "        position  = " << Vec3Lua(trans->GetPosition()) << ",\n";
		}
		out << "        direction = " << Vec3Lua(light->Direction) << ",\n";
		out << "        color     = " << Vec3Lua(light->Color) << ",\n";
		out << "        intensity = " << light->Intensity << ",\n";
		out << "        radius    = " << light->Radius << ",\n";
		out << "        enabled   = " << (light->bEnabled ? "true" : "false") << ",\n";
		out << "      },\n";
		out << "    },\n";
	}

	// --- Camera (single active) ---
	if (auto activeCam = ecs.GetActiveCameraEntity(); activeCam.IsValid())
	{
		const auto* cam = ecs.GetCamera(activeCam);
		const auto* trans = ecs.GetTransform(activeCam);
		std::string entityName = "camera";
		if (const auto* n = ecs.GetName(activeCam))
			entityName = *n;
		if (cam)
		{
			out << "    {\n";
			out << "      name = " << EscapeLuaString(entityName) << ",\n";
			// Attached scripts for the camera entity (camera controllers).
			if (const auto* script = ecs.GetScript(activeCam); script && !script->Instances.empty())
			{
				out << "      scripts = {\n";
				for (const auto& inst : script->Instances)
				{
					if (!inst.SourceName.empty())
						out << "        { file = " << EscapeLuaString(PlatformWideToUtf8(inst.SourceName)) << " },\n";
					else if (!inst.NativeScriptName.empty())
						out << "        { native = " << EscapeLuaString(inst.NativeScriptName) << " },\n";
				}
				out << "      },\n";
			}
			out << "      camera = {\n";
			out << "        position       = " << Vec3Lua(trans ? trans->GetPosition() : glm::vec3(0.0f)) << ",\n";
			out << "        look_direction = " << Vec3Lua(cam->LookDirection) << ",\n";
			out << "        up             = " << Vec3Lua(cam->UpDirection) << ",\n";
			out << "        fov            = " << cam->Fov << ",\n";
			out << "        near_plane     = " << cam->NearPlane << ",\n";
			out << "        far_plane      = " << cam->FarPlane << ",\n";
			out << "        active         = " << (cam->bActive ? "true" : "false") << ",\n";
			out << "      },\n";
			out << "    },\n";
		}
	}

	// --- Script-only entities (HUD, game controllers) ---
	// Entities that have a ScriptComponent but NO mesh/light/camera —
	// they exist purely as anchor points for behaviour scripts. Without
	// this block, dungeon HUD / platformer state controllers vanish on
	// map reload and the gameplay loop breaks (no HUD, no input wiring).
	// IMPORTANT: skip the engine's `World` entity. Its script instances
	// are the *startup* luau scripts (sponza_demo / terrain_demo / ...
	// + 099_persist_map). Saving them here and re-attaching them on
	// load_map triggers infinite recursion: load_map runs each attached
	// script → 010_sponza_demo_scene.luau calls `corona.load_map(...)`
	// again on entry → reload loops until the engine dies.
	const CoronaECS::Entity worldEntity = WorldEntity;
	for (auto entity : ecs.GetEntitiesWithScript())
	{
		const auto* script = ecs.GetScript(entity);
		if (!script || script->Instances.empty())
			continue;
		// Skip if already serialized above as mesh / light / camera.
		if (ecs.HasMesh(entity) || ecs.HasLight(entity) || ecs.HasCamera(entity))
			continue;
		// Skip the engine-singleton World entity (startup scripts only).
		if (entity == worldEntity)
			continue;
		std::string entityName = "scripted";
		if (const auto* n = ecs.GetName(entity))
			entityName = *n;
		out << "    {\n";
		out << "      name = " << EscapeLuaString(entityName) << ",\n";
		out << "      scripts = {\n";
		for (const auto& inst : script->Instances)
		{
			if (!inst.SourceName.empty())
				out << "        { file = " << EscapeLuaString(PlatformWideToUtf8(inst.SourceName)) << " },\n";
			else if (!inst.NativeScriptName.empty())
				out << "        { native = " << EscapeLuaString(inst.NativeScriptName) << " },\n";
		}
		out << "      },\n";
		out << "    },\n";
	}

	out << "  },\n";

	// --- Globals ---
	out << "  globals = {\n";
	out << "    wind = {\n";
	out << "      params = " << Vec4Lua(RenderFrameWindParams) << ",\n";
	out << "      tuning = " << Vec4Lua(RenderFrameWindTuning) << ",\n";
	out << "    },\n";
	out << "    grass = {\n";
	out << "      bend_origin    = " << Vec4Lua(RenderFrameGrassBendOrigin) << ",\n";
	out << "      bend_params    = " << Vec4Lua(RenderFrameGrassBendParams) << ",\n";
	out << "      render_origin  = " << Vec3Lua(GrassRenderOrigin) << ",\n";
	out << "      render_distance = " << GrassRenderDistance << ",\n";
	out << "    },\n";
	out << "    terrain_deform_sphere = " << Vec4Lua(RenderFrameTerrainDeformSphere) << ",\n";
	out << "  },\n";
	out << "}\n";

	std::ofstream f(path, std::ios::binary);
	if (!f)
	{
		if (outError) *outError = L"cannot open for write: " + path.wstring();
		return false;
	}
	const std::string body = out.str();
	f.write(body.data(), static_cast<std::streamsize>(body.size()));
	AppendCpuRuntimeTrace(L"[Map] saved " + path.wstring() + L" (" + std::to_wstring(body.size()) + L" bytes)");
	CurrentMapName = name;
	return true;
}

bool Corona::GetEntityMeshRecipeForScript(CoronaECS::Entity entity, SceneRecipe& outRecipe) const
{
	if (!EntityWorld.IsAlive(entity))
		return false;
	const auto* mesh = EntityWorld.GetMesh(entity);
	if (!mesh)
		return false;
	const SceneObjectHandle soh = static_cast<SceneObjectHandle>(mesh->RenderObjectHandle);
	const auto soIt = ScriptObjects.find(soh);
	if (soIt == ScriptObjects.end())
		return false;
	const auto sceneIt = ScriptScenes.find(soIt->second.SceneHandle);
	if (sceneIt == ScriptScenes.end())
		return false;
	outRecipe = sceneIt->second.Recipe;
	return true;
}

bool Corona::IsEntityGrassMesh(CoronaECS::Entity entity) const
{
	if (!EntityWorld.IsAlive(entity))
		return false;
	const auto* mesh = EntityWorld.GetMesh(entity);
	if (!mesh)
		return false;
	const SceneObjectHandle soh = static_cast<SceneObjectHandle>(mesh->RenderObjectHandle);
	const auto soIt = ScriptObjects.find(soh);
	if (soIt == ScriptObjects.end())
		return false;
	const auto sceneIt = ScriptScenes.find(soIt->second.SceneHandle);
	if (sceneIt == ScriptScenes.end())
		return false;
	const SceneRecipe::Kind kind = sceneIt->second.Recipe.RecipeKind;
	return kind == SceneRecipe::Kind::Grass || kind == SceneRecipe::Kind::GrassOnTerrain;
}

bool Corona::SaveEntityAsAsset(CoronaECS::Entity entity, const std::string& assetName, std::wstring* outError)
{
	if (!EntityWorld.IsAlive(entity))
	{
		if (outError) *outError = L"entity is not alive";
		return false;
	}
	if (assetName.empty())
	{
		if (outError) *outError = L"asset name is empty";
		return false;
	}

	// Reuse the assets/scene_assets/ folder; create it on demand.
	const std::filesystem::path assetsDir =
		RuntimePaths::AssetDirectory() / L"scene_assets";
	std::error_code ec;
	std::filesystem::create_directories(assetsDir, ec);

	std::string sanitized;
	sanitized.reserve(assetName.size());
	for (char c : assetName)
	{
		const bool bAllowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
		sanitized.push_back(bAllowed ? c : '_');
	}
	const std::filesystem::path path = assetsDir / (sanitized + ".asset.lua");

	std::string sourceName = sanitized;
	if (const auto* n = EntityWorld.GetName(entity))
		sourceName = *n;

	std::ostringstream out;
	out << "-- Corona scene-asset capture\n";
	out << "-- saved entity name : " << EscapeLuaString(sourceName) << "\n";
	out << "return {\n";
	out << "  name = " << EscapeLuaString(assetName) << ",\n";
	out << "  source_name = " << EscapeLuaString(sourceName) << ",\n";

	bool bWroteAnyComponent = false;

	// --- Mesh component + procedural recipe ---
	if (const auto* mesh = EntityWorld.GetMesh(entity))
	{
		const SceneObjectHandle soh = static_cast<SceneObjectHandle>(mesh->RenderObjectHandle);
		const auto soIt = ScriptObjects.find(soh);
		const ScriptObjectState* so = (soIt != ScriptObjects.end()) ? &soIt->second : nullptr;
		const SceneRecipe* recipe = nullptr;
		if (so)
		{
			const auto sceneIt = ScriptScenes.find(so->SceneHandle);
			if (sceneIt != ScriptScenes.end())
				recipe = &sceneIt->second.Recipe;
		}
		if (recipe && so)
		{
			const char* primitiveStr = "asset";
			switch (recipe->RecipeKind)
			{
			case SceneRecipe::Kind::Terrain:        primitiveStr = "TERRAIN"; break;
			case SceneRecipe::Kind::GrassOnTerrain: primitiveStr = "GRASS_ON_TERRAIN"; break;
			case SceneRecipe::Kind::Grass:          primitiveStr = "GRASS"; break;
			case SceneRecipe::Kind::BlockCharacter: primitiveStr = "block_character"; break;
			case SceneRecipe::Kind::Asset:          primitiveStr = "asset"; break;
			}
			out << "  mesh = {\n";
			out << "    primitive = " << EscapeLuaString(primitiveStr) << ",\n";
			switch (recipe->RecipeKind)
			{
			case SceneRecipe::Kind::Terrain:
				out << "    params = { seed = " << recipe->Seed << " },\n";
				break;
			case SceneRecipe::Kind::GrassOnTerrain:
				out << "    params = { blade_count = " << recipe->BladeCount
				    << ", blade_height = " << recipe->BladeHeight
				    << ", blade_segments = " << recipe->BladeSegments
				    << ", seed = " << recipe->Seed << " },\n";
				break;
			case SceneRecipe::Kind::Grass:
				out << "    params = { blade_count = " << recipe->BladeCount
				    << ", area_size = " << recipe->AreaSize
				    << ", blade_height = " << recipe->BladeHeight
				    << ", blade_segments = " << recipe->BladeSegments
				    << ", seed = " << recipe->Seed << " },\n";
				break;
			case SceneRecipe::Kind::BlockCharacter:
				out << "    params = { seed = " << recipe->Seed << " },\n";
				break;
			case SceneRecipe::Kind::Asset:
				out << "    path = " << EscapeLuaString(PlatformWideToUtf8(recipe->AssetPath)) << ",\n";
				break;
			}
			out << "    transform = {\n";
			out << "      position = " << Vec3Lua(so->Position) << ",\n";
			out << "      rotation = " << Vec3Lua(so->RotationDegrees) << ",\n";
			if (so->bUseScale)
				out << "      scale = " << Vec3Lua(so->Scale) << ",\n";
			else
				out << "      target_extent = " << so->TargetExtent << ",\n";
			out << "    },\n";
			out << "    material = {\n";
			out << "      roughness = " << mesh->Roughness << ",\n";
			out << "      metallic  = " << mesh->Metallic << ",\n";
			out << "      override  = " << (mesh->bOverrideRoughnessMetallic ? "true" : "false") << ",\n";
			out << "    },\n";
			out << "    ray_tracing = " << (mesh->bRayTracing ? "true" : "false") << ",\n";
			out << "  },\n";

			// Grass-only environment block: wind / bend / render-distance
			// snapshot. These are global frame state in the engine but the
			// asset captures them so re-loading the grass restores its
			// "intended look". Loader applies them only when present.
			const SceneRecipe::Kind k = recipe->RecipeKind;
			if (k == SceneRecipe::Kind::Grass || k == SceneRecipe::Kind::GrassOnTerrain)
			{
				out << "  grass_env = {\n";
				out << "    render_distance = " << GrassRenderDistance << ",\n";
				out << "    bend_radius     = " << RenderFrameGrassBendParams.x << ",\n";
				out << "    bend_max_height = " << RenderFrameGrassBendParams.y << ",\n";
				out << "    wind_dir        = { "
				    << RenderFrameWindParams.x << ", "
				    << RenderFrameWindParams.z << " },\n";
				out << "    wind_strength   = " << RenderFrameWindParams.w << ",\n";
				out << "    wind_temp_freq  = " << RenderFrameWindTuning.x << ",\n";
				out << "    wind_space_freq = " << RenderFrameWindTuning.y << ",\n";
				out << "  },\n";
			}
			bWroteAnyComponent = true;
		}
	}

	// --- Light component ---
	if (const auto* light = EntityWorld.GetLight(entity))
	{
		out << "  light = {\n";
		out << "    type      = " << EscapeLuaString(light->Type == CoronaECS::LightType::Directional ? "directional" : "point") << ",\n";
		out << "    direction = " << Vec3Lua(light->Direction) << ",\n";
		out << "    color     = " << Vec3Lua(light->Color) << ",\n";
		out << "    intensity = " << light->Intensity << ",\n";
		out << "    radius    = " << light->Radius << ",\n";
		out << "    enabled   = " << (light->bEnabled ? "true" : "false") << ",\n";
		out << "  },\n";
		bWroteAnyComponent = true;
	}

	// --- Camera component (transform pulled from the entity's TransformComponent) ---
	if (const auto* cam = EntityWorld.GetCamera(entity))
	{
		const auto* trans = EntityWorld.GetTransform(entity);
		out << "  camera = {\n";
		out << "    position       = " << Vec3Lua(trans ? trans->GetPosition() : glm::vec3(0.0f)) << ",\n";
		out << "    look_direction = " << Vec3Lua(cam->LookDirection) << ",\n";
		out << "    up             = " << Vec3Lua(cam->UpDirection) << ",\n";
		out << "    fov            = " << cam->Fov << ",\n";
		out << "    near_plane     = " << cam->NearPlane << ",\n";
		out << "    far_plane      = " << cam->FarPlane << ",\n";
		out << "  },\n";
		bWroteAnyComponent = true;
	}

	out << "}\n";

	if (!bWroteAnyComponent)
	{
		if (outError) *outError = L"entity has no serializable component (mesh/light/camera)";
		return false;
	}

	std::ofstream f(path, std::ios::binary);
	if (!f)
	{
		if (outError) *outError = L"cannot open for write: " + path.wstring();
		return false;
	}
	const std::string body = out.str();
	f.write(body.data(), static_cast<std::streamsize>(body.size()));
	AppendCpuRuntimeTrace(
		L"[Asset] saved " + path.wstring() +
		L" (" + std::to_wstring(body.size()) + L" bytes)");
	return true;
}

bool Corona::LoadMapFromFile(const std::wstring& name, std::wstring* outError)
{
	const std::filesystem::path path = ResolveMapPath(name);
	const std::string source = ReadFile(path);
	if (source.empty())
	{
		if (outError) *outError = L"map not found / empty: " + path.wstring();
		return false;
	}
	if (!ScriptState || !ScriptState->L)
	{
		if (outError) *outError = L"Luau VM not initialized";
		return false;
	}
	lua_State* L = ScriptState->L;

	size_t bytecodeSize = 0;
	char* bytecode = luau_compile(source.data(), source.size(), nullptr, &bytecodeSize);
	if (!bytecode)
	{
		if (outError) *outError = L"map compile failed: " + path.wstring();
		return false;
	}
	const std::string chunkName = "=" + PlatformWideToUtf8(path.wstring());
	const int loadResult = luau_load(L, chunkName.c_str(), bytecode, bytecodeSize, 0);
	std::free(bytecode);
	if (loadResult != 0)
	{
		if (outError)
			*outError = L"map load failed: " + PlatformUtf8ToWide(lua_isstring(L, -1) ? lua_tostring(L, -1) : "");
		lua_pop(L, 1);
		return false;
	}
	if (lua_pcall(L, 0, 1, 0) != 0)
	{
		if (outError)
			*outError = L"map exec error: " + PlatformUtf8ToWide(lua_isstring(L, -1) ? lua_tostring(L, -1) : "");
		lua_pop(L, 1);
		return false;
	}
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 1);
		if (outError) *outError = L"map didn't return a table";
		return false;
	}

	// Wipe current scene before replaying.
	ClearScriptSpawnedScene();

	const int rootIdx = lua_gettop(L);

	// --- Entities ---
	lua_getfield(L, rootIdx, "entities");
	if (lua_istable(L, -1))
	{
		const int entitiesIdx = lua_gettop(L);
		const int n = static_cast<int>(lua_objlen(L, entitiesIdx));
		for (int i = 1; i <= n; ++i)
		{
			lua_rawgeti(L, entitiesIdx, i);
			if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }
			const int eIdx = lua_gettop(L);

			std::string entityName;
			LuaGetString(L, eIdx, "name", entityName);

			// Track the entity created by this entry so the `scripts`
			// block at the end can re-attach controllers. Mesh / light /
			// camera / script-only branches all set this.
			CoronaECS::Entity createdEntity;

			// Mesh entry?
			lua_getfield(L, eIdx, "mesh");
			if (lua_istable(L, -1))
			{
				const int mIdx = lua_gettop(L);

				std::string primitive;
				LuaGetString(L, mIdx, "primitive", primitive);

				ScriptSceneHandle sceneHandle = InvalidScriptSceneHandle;
				lua_getfield(L, mIdx, "params");
				const int paramsIdx = lua_gettop(L);
				if (primitive == "TERRAIN")
				{
					double seed = 1.0;
					LuaGetNumber(L, paramsIdx, "seed", seed);
					sceneHandle = CreateProceduralTerrainSceneForScript(static_cast<UINT32>(seed));
				}
				else if (primitive == "GRASS_ON_TERRAIN")
				{
					double bladeCount = 100000.0, bladeHeight = 30.0, seed = 1.0, bladeSegments = 4.0;
					bool bProcedural = false;
					LuaGetNumber(L, paramsIdx, "blade_count", bladeCount);
					LuaGetNumber(L, paramsIdx, "blade_height", bladeHeight);
					LuaGetNumber(L, paramsIdx, "blade_segments", bladeSegments);
					LuaGetNumber(L, paramsIdx, "seed", seed);
					{
						lua_getfield(L, paramsIdx, "procedural");
						if (lua_isboolean(L, -1)) bProcedural = lua_toboolean(L, -1) != 0;
						lua_pop(L, 1);
					}
					sceneHandle = bProcedural
						? CreateProceduralGrassOnTerrainSceneInstancedForScript(
							static_cast<UINT32>(bladeCount),
							static_cast<float>(bladeHeight),
							static_cast<UINT32>(seed),
							static_cast<UINT32>(bladeSegments))
						: CreateProceduralGrassOnTerrainSceneForScript(
							static_cast<UINT32>(bladeCount),
							static_cast<float>(bladeHeight),
							static_cast<UINT32>(seed),
							static_cast<UINT32>(bladeSegments));
				}
				else if (primitive == "GRASS")
				{
					double bladeCount = 100000.0, area = 1000.0, bladeHeight = 30.0, seed = 1.0, bladeSegments = 4.0;
					LuaGetNumber(L, paramsIdx, "blade_count", bladeCount);
					LuaGetNumber(L, paramsIdx, "area_size", area);
					LuaGetNumber(L, paramsIdx, "blade_height", bladeHeight);
					LuaGetNumber(L, paramsIdx, "blade_segments", bladeSegments);
					LuaGetNumber(L, paramsIdx, "seed", seed);
					sceneHandle = CreateProceduralGrassSceneForScript(
						static_cast<UINT32>(bladeCount),
						static_cast<float>(area),
						static_cast<float>(bladeHeight),
						static_cast<UINT32>(seed),
						static_cast<UINT32>(bladeSegments));
				}
				else if (primitive == "block_character")
				{
					double seed = 1.0;
					LuaGetNumber(L, paramsIdx, "seed", seed);
					sceneHandle = CreateProceduralBlockCharacterSceneForScript(static_cast<UINT32>(seed));
				}
				lua_pop(L, 1); // params

				if (sceneHandle == InvalidScriptSceneHandle && primitive == "asset")
				{
					std::string assetPath;
					LuaGetString(L, mIdx, "path", assetPath);
					if (!assetPath.empty())
						sceneHandle = LoadSceneForScript(PlatformUtf8ToWide(assetPath));
				}

				if (sceneHandle != InvalidScriptSceneHandle)
				{
					glm::vec3 position(0.0f), rotation(0.0f), scale(1.0f);
					float targetExtent = 1.0f;
					bool useScale = false;
					lua_getfield(L, mIdx, "transform");
					if (lua_istable(L, -1))
					{
						const int tIdx = lua_gettop(L);
						LuaGetVec3(L, tIdx, "position", position);
						LuaGetVec3(L, tIdx, "rotation", rotation);
						double targetExtentD = 1.0;
						if (LuaGetNumber(L, tIdx, "target_extent", targetExtentD))
							targetExtent = static_cast<float>(targetExtentD);
						if (LuaGetVec3(L, tIdx, "scale", scale))
							useScale = true;
					}
					lua_pop(L, 1); // transform

					float roughness = 1.0f, metallic = 0.0f;
					bool overrideMat = false, visible = true, rayTracing = true;
					lua_getfield(L, mIdx, "material");
					if (lua_istable(L, -1))
					{
						const int matIdx = lua_gettop(L);
						double r = 1.0, m = 0.0;
						if (LuaGetNumber(L, matIdx, "roughness", r)) roughness = static_cast<float>(r);
						if (LuaGetNumber(L, matIdx, "metallic", m))  metallic = static_cast<float>(m);
						LuaGetBool(L, matIdx, "override", overrideMat);
					}
					lua_pop(L, 1); // material
					LuaGetBool(L, mIdx, "visible", visible);
					LuaGetBool(L, mIdx, "ray_tracing", rayTracing);

					CoronaECS::Entity newEntity = CreateEntity(entityName);
					AddMeshComponentForScript(
						newEntity, sceneHandle,
						position, rotation, targetExtent, scale, useScale,
						roughness, metallic, overrideMat,
						visible, rayTracing, /*physicsQuery*/ true);
					createdEntity = newEntity;
				}
			}
			lua_pop(L, 1); // mesh

			// Light entry?
			lua_getfield(L, eIdx, "light");
			if (lua_istable(L, -1))
			{
				const int lIdx = lua_gettop(L);
				CoronaECS::LightComponent comp;
				std::string typeStr;
				if (LuaGetString(L, lIdx, "type", typeStr))
					comp.Type = (typeStr == "directional") ? CoronaECS::LightType::Directional : CoronaECS::LightType::Point;
				LuaGetVec3(L, lIdx, "direction", comp.Direction);
				LuaGetVec3(L, lIdx, "color", comp.Color);
				double intensity = 1.0, radius = 320.0;
				if (LuaGetNumber(L, lIdx, "intensity", intensity)) comp.Intensity = static_cast<float>(intensity);
				if (LuaGetNumber(L, lIdx, "radius", radius))       comp.Radius = static_cast<float>(radius);
				LuaGetBool(L, lIdx, "enabled", comp.bEnabled);
				// Point light position — restored before AddLight so the
				// TransformComponent created downstream has the saved value
				// instead of the entity-default origin.
				glm::vec3 pointLightPosition(0.0f);
				const bool bHasPointLightPosition =
					(comp.Type == CoronaECS::LightType::Point) &&
					LuaGetVec3(L, lIdx, "position", pointLightPosition);

				// Directional lights are an engine singleton — the very first
				// one we see fills `MainDirectionalLightEntity`, and any extra
				// directional entries (legacy duplicates from a map saved
				// before this fix) are skipped instead of spawning new
				// entities. Subsequent loads then heal the .map file on the
				// next save by only writing the single survivor.
				const bool bDirectional = (comp.Type == CoronaECS::LightType::Directional);
				const bool bAlreadyHaveDirectional =
					bDirectional &&
					EntityWorld.IsAlive(MainDirectionalLightEntity) &&
					EntityWorld.HasLight(MainDirectionalLightEntity);
				if (!bAlreadyHaveDirectional)
				{
					CoronaECS::Entity le = CreateEntity(entityName);
					createdEntity = le;
					comp.RuntimeLightId = 0;
					if (bDirectional)
					{
						EntityWorld.AddLight(le, comp);
						MainDirectionalLightEntity = le;
						ApplyDirectionalLightEntityToState();
					}
					else
					{
						// Point light: seed TransformComponent with the
						// saved position before SetEntityLightForScript
						// reads it back into the runtime PointLights[]
						// entry (PointLight.Position is initialized from
						// the entity's transform). Without this the saved
						// position would never reach the renderer because
						// SetEntityLightForScript runs after AddLight and
						// reads transform, not the loaded comp.
						if (bHasPointLightPosition)
						{
							auto* trans = EntityWorld.GetTransform(le);
							if (!trans)
								trans = EntityWorld.AddTransform(le, CoronaECS::TransformComponent::FromTRS(pointLightPosition));
							else
								trans->SetPosition(pointLightPosition);
						}
						SetEntityLightForScript(le, comp, /*persist*/ false);
					}
				}
			}
			lua_pop(L, 1); // light

			// Camera entry?
			lua_getfield(L, eIdx, "camera");
			if (lua_istable(L, -1))
			{
				const int cIdx = lua_gettop(L);
				CoronaECS::Entity ce = CreateEntity(entityName);
				createdEntity = ce;
				CoronaECS::CameraComponent comp;
				glm::vec3 position(0.0f);
				LuaGetVec3(L, cIdx, "position", position);
				LuaGetVec3(L, cIdx, "look_direction", comp.LookDirection);
				LuaGetVec3(L, cIdx, "up", comp.UpDirection);
				double fov = 0.8, np = 10.0, fp = 20000.0;
				if (LuaGetNumber(L, cIdx, "fov", fov))         comp.Fov = static_cast<float>(fov);
				if (LuaGetNumber(L, cIdx, "near_plane", np))   comp.NearPlane = static_cast<float>(np);
				if (LuaGetNumber(L, cIdx, "far_plane", fp))    comp.FarPlane = static_cast<float>(fp);
				LuaGetBool(L, cIdx, "active", comp.bActive);
				EntityWorld.AddTransform(ce, CoronaECS::TransformComponent::FromTRS(position));
				EntityWorld.AddCamera(ce, comp);
				if (comp.bActive)
				{
					EntityWorld.SetActiveCamera(ce);
					// Bind to the engine-singleton slot so the next
					// InitializeMainCameraEntity() call doesn't spawn a
					// stale-handle duplicate.
					if (!EntityWorld.IsAlive(MainCameraEntity) ||
						!EntityWorld.HasCamera(MainCameraEntity))
					{
						MainCameraEntity = ce;
					}
				}
			}
			lua_pop(L, 1); // camera

			// Script-only entry (HUD / game controllers without
			// mesh/light/camera). Detected by the entry having a
			// `scripts` table but none of the component blocks above
			// matched. Create a bare entity so the attach loop below
			// has somewhere to bind to.
			if (!createdEntity.IsValid())
			{
				lua_getfield(L, eIdx, "scripts");
				const bool bHasScripts = lua_istable(L, -1);
				lua_pop(L, 1);
				if (bHasScripts)
					createdEntity = CreateEntity(entityName);
			}

			// --- Re-attach scripts to the entity created above ---
			if (createdEntity.IsValid())
			{
				lua_getfield(L, eIdx, "scripts");
				if (lua_istable(L, -1))
				{
					const int scriptsIdx = lua_gettop(L);
					const int scriptCount = static_cast<int>(lua_objlen(L, scriptsIdx));
					for (int si = 1; si <= scriptCount; ++si)
					{
						lua_rawgeti(L, scriptsIdx, si);
						if (lua_istable(L, -1))
						{
							const int sIdx = lua_gettop(L);
							std::string filePath;
							std::string nativeName;
							if (LuaGetString(L, sIdx, "file", filePath) && !filePath.empty())
							{
								AttachEntityScriptFileForScript(createdEntity, PlatformUtf8ToWide(filePath));
							}
							else if (LuaGetString(L, sIdx, "native", nativeName) && !nativeName.empty())
							{
								AttachNativeEntityScriptForScript(createdEntity, nativeName);
							}
						}
						lua_pop(L, 1);
					}
				}
				lua_pop(L, 1); // scripts
			}

			lua_pop(L, 1); // entity
		}
	}
	lua_pop(L, 1); // entities

	// --- Globals ---
	lua_getfield(L, rootIdx, "globals");
	if (lua_istable(L, -1))
	{
		const int gIdx = lua_gettop(L);
		lua_getfield(L, gIdx, "wind");
		if (lua_istable(L, -1))
		{
			const int wIdx = lua_gettop(L);
			LuaGetVec4(L, wIdx, "params", RenderFrameWindParams);
			LuaGetVec4(L, wIdx, "tuning", RenderFrameWindTuning);
		}
		lua_pop(L, 1);

		lua_getfield(L, gIdx, "grass");
		if (lua_istable(L, -1))
		{
			const int grIdx = lua_gettop(L);
			LuaGetVec4(L, grIdx, "bend_origin",   RenderFrameGrassBendOrigin);
			LuaGetVec4(L, grIdx, "bend_params",   RenderFrameGrassBendParams);
			LuaGetVec3(L, grIdx, "render_origin", GrassRenderOrigin);
			double rd = 0.0;
			if (LuaGetNumber(L, grIdx, "render_distance", rd))
				GrassRenderDistance = static_cast<float>(rd);
		}
		lua_pop(L, 1);

		LuaGetVec4(L, gIdx, "terrain_deform_sphere", RenderFrameTerrainDeformSphere);
	}
	lua_pop(L, 1); // globals

	lua_pop(L, 1); // root
	AppendCpuRuntimeTrace(L"[Map] loaded " + path.wstring());
	CurrentMapName = name;
	return true;
}
