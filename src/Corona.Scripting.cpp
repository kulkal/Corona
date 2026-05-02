//*********************************************************
//
// Luau scripting bridge for high-level scene/object handles.
//
//*********************************************************

#include "stdafx.h"
#include "Corona.h"

#include "lua.h"
#include "lualib.h"
#include "luacode.h"

#include <cctype>
#include <codecvt>
#include <cstdlib>
#include <fstream>
#include <locale>
#include <sstream>

void AppendCpuRuntimeTrace(const std::wstring& line);

namespace
{
	const char* kCoronaRegistryKey = "Corona.ScriptHost";

	std::wstring Utf8ToWideLocal(const std::string& value)
	{
		if (value.empty())
			return std::wstring();
		std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
		return converter.from_bytes(value);
	}

	std::string WideToUtf8Local(const std::wstring& value)
	{
		if (value.empty())
			return std::string();
		std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
		return converter.to_bytes(value);
	}

	std::string LuaToString(lua_State* L, int index)
	{
		const char* text = lua_tostring(L, index);
		return text ? std::string(text) : std::string();
	}

	std::string NormalizeKeyName(const char* name)
	{
		std::string result;
		if (!name)
			return result;
		for (const char* c = name; *c; ++c)
		{
			if (*c == '_' || *c == '-' || *c == ' ')
				continue;
			result.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(*c))));
		}
		return result;
	}

	bool TryReadKeyCode(lua_State* L, int index, UINT8& key)
	{
		if (lua_isnumber(L, index))
		{
			const int code = lua_tointeger(L, index);
			if (code < 0 || code > 255)
				return false;
			key = static_cast<UINT8>(code);
			return true;
		}

		if (!lua_isstring(L, index))
			return false;

		const std::string name = NormalizeKeyName(lua_tostring(L, index));
		if (name.empty())
			return false;
		if (name.size() == 1)
		{
			key = static_cast<UINT8>(name[0]);
			return true;
		}

		if (name == "SPACE")
			key = VK_SPACE;
		else if (name == "ENTER" || name == "RETURN")
			key = VK_RETURN;
		else if (name == "ESC" || name == "ESCAPE")
			key = VK_ESCAPE;
		else if (name == "TAB")
			key = VK_TAB;
		else if (name == "BACKSPACE")
			key = VK_BACK;
		else if (name == "LEFT")
			key = VK_LEFT;
		else if (name == "RIGHT")
			key = VK_RIGHT;
		else if (name == "UP")
			key = VK_UP;
		else if (name == "DOWN")
			key = VK_DOWN;
		else if (name == "SHIFT")
			key = VK_SHIFT;
		else if (name == "LSHIFT")
			key = VK_LSHIFT;
		else if (name == "RSHIFT")
			key = VK_RSHIFT;
		else if (name == "CTRL" || name == "CONTROL")
			key = VK_CONTROL;
		else if (name == "LCTRL" || name == "LCONTROL")
			key = VK_LCONTROL;
		else if (name == "RCTRL" || name == "RCONTROL")
			key = VK_RCONTROL;
		else if (name == "ALT")
			key = VK_MENU;
		else if (name == "LALT")
			key = VK_LMENU;
		else if (name == "RALT")
			key = VK_RMENU;
		else
			return false;

		return true;
	}

	Corona* GetHost(lua_State* L)
	{
		lua_getfield(L, LUA_REGISTRYINDEX, kCoronaRegistryKey);
		Corona* host = static_cast<Corona*>(lua_touserdata(L, -1));
		lua_pop(L, 1);
		return host;
	}

	bool ReadNumberAt(lua_State* L, int index, float& value)
	{
		if (!lua_isnumber(L, index))
			return false;
		value = static_cast<float>(lua_tonumber(L, index));
		return true;
	}

	bool ReadNumberField(lua_State* L, int tableIndex, const char* name, float& value)
	{
		lua_getfield(L, tableIndex, name);
		const bool ok = ReadNumberAt(L, -1, value);
		lua_pop(L, 1);
		return ok;
	}

	bool ReadBoolField(lua_State* L, int tableIndex, const char* name, bool& value)
	{
		lua_getfield(L, tableIndex, name);
		const bool ok = lua_isboolean(L, -1) != 0;
		if (ok)
			value = lua_toboolean(L, -1) != 0;
		lua_pop(L, 1);
		return ok;
	}

	bool ReadVec3At(lua_State* L, int index, glm::vec3& value)
	{
		if (!lua_istable(L, index))
			return false;

		index = lua_absindex(L, index);
		glm::vec3 result = value;
		bool readAny = false;

		float component = 0.0f;
		if (ReadNumberField(L, index, "x", component))
		{
			result.x = component;
			readAny = true;
		}
		if (ReadNumberField(L, index, "y", component))
		{
			result.y = component;
			readAny = true;
		}
		if (ReadNumberField(L, index, "z", component))
		{
			result.z = component;
			readAny = true;
		}

		if (!readAny)
		{
			lua_rawgeti(L, index, 1);
			const bool xOk = ReadNumberAt(L, -1, result.x);
			lua_pop(L, 1);
			lua_rawgeti(L, index, 2);
			const bool yOk = ReadNumberAt(L, -1, result.y);
			lua_pop(L, 1);
			lua_rawgeti(L, index, 3);
			const bool zOk = ReadNumberAt(L, -1, result.z);
			lua_pop(L, 1);
			readAny = xOk || yOk || zOk;
		}

		if (readAny)
			value = result;
		return readAny;
	}

	bool ReadVec3Field(lua_State* L, int tableIndex, const char* name, glm::vec3& value)
	{
		lua_getfield(L, tableIndex, name);
		const bool ok = ReadVec3At(L, -1, value);
		lua_pop(L, 1);
		return ok;
	}

	bool ReadTransformDesc(
		lua_State* L,
		int tableIndex,
		glm::vec3& position,
		glm::vec3& rotationDegrees,
		float& targetExtent,
		float* roughness,
		float* metallic,
		bool* overrideMaterial,
		bool* visible,
		bool* rayTracing,
		bool* physicsQuery)
	{
		if (!lua_istable(L, tableIndex))
			return false;

		tableIndex = lua_absindex(L, tableIndex);
		ReadVec3Field(L, tableIndex, "position", position);
		ReadVec3Field(L, tableIndex, "rotation", rotationDegrees);
		ReadVec3Field(L, tableIndex, "rotationDegrees", rotationDegrees);
		ReadVec3Field(L, tableIndex, "rotation_degrees", rotationDegrees);

		if (!ReadNumberField(L, tableIndex, "target_extent", targetExtent))
		{
			if (!ReadNumberField(L, tableIndex, "targetExtent", targetExtent))
				ReadNumberField(L, tableIndex, "scale", targetExtent);
		}

		if (roughness)
			ReadNumberField(L, tableIndex, "roughness", *roughness);
		if (metallic)
			ReadNumberField(L, tableIndex, "metallic", *metallic);
		if (overrideMaterial)
		{
			if (!ReadBoolField(L, tableIndex, "override_material", *overrideMaterial))
				ReadBoolField(L, tableIndex, "overrideMaterial", *overrideMaterial);
		}
		if (visible)
			ReadBoolField(L, tableIndex, "visible", *visible);
		if (rayTracing)
		{
			if (!ReadBoolField(L, tableIndex, "ray_tracing", *rayTracing))
				ReadBoolField(L, tableIndex, "rayTracing", *rayTracing);
		}
		if (physicsQuery)
		{
			if (!ReadBoolField(L, tableIndex, "physics", *physicsQuery))
			{
				if (!ReadBoolField(L, tableIndex, "physics_query", *physicsQuery))
					ReadBoolField(L, tableIndex, "physicsQuery", *physicsQuery);
			}
		}

		return true;
	}

	void PushVec3(lua_State* L, const glm::vec3& value)
	{
		lua_newtable(L);
		lua_pushnumber(L, static_cast<double>(value.x));
		lua_setfield(L, -2, "x");
		lua_pushnumber(L, static_cast<double>(value.y));
		lua_setfield(L, -2, "y");
		lua_pushnumber(L, static_cast<double>(value.z));
		lua_setfield(L, -2, "z");
		lua_pushnumber(L, static_cast<double>(value.x));
		lua_rawseti(L, -2, 1);
		lua_pushnumber(L, static_cast<double>(value.y));
		lua_rawseti(L, -2, 2);
		lua_pushnumber(L, static_cast<double>(value.z));
		lua_rawseti(L, -2, 3);
	}

	int LuaCoronaLog(lua_State* L)
	{
		const char* message = luaL_checkstring(L, 1);
		AppendCpuRuntimeTrace(L"[Luau] " + Utf8ToWideLocal(message ? message : ""));
		return 0;
	}

	int LuaCoronaLoadScene(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const char* path = luaL_checkstring(L, 1);
		const Corona::ScriptSceneHandle handle = host->LoadSceneForScript(Utf8ToWideLocal(path ? path : ""));
		if (handle == Corona::InvalidScriptSceneHandle)
		{
			luaL_error(L, "failed to load scene '%s'", path ? path : "");
			return 0;
		}

		lua_pushinteger(L, static_cast<int>(handle));
		return 1;
	}

	int LuaCoronaSpawn(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const Corona::ScriptSceneHandle sceneHandle = static_cast<Corona::ScriptSceneHandle>(luaL_checkinteger(L, 1));
		glm::vec3 position(0.0f);
		glm::vec3 rotationDegrees(0.0f);
		float targetExtent = 1.0f;
		float roughness = 1.0f;
		float metallic = 0.0f;
		bool overrideMaterial = false;
		bool visible = true;
		bool rayTracing = true;
		bool physicsQuery = true;
		if (!ReadTransformDesc(L, 2, position, rotationDegrees, targetExtent, &roughness, &metallic, &overrideMaterial, &visible, &rayTracing, &physicsQuery))
		{
			luaL_error(L, "corona.spawn expects a descriptor table");
			return 0;
		}

		const Corona::SceneObjectHandle handle = host->SpawnSceneObjectForScript(
			sceneHandle,
			position,
			rotationDegrees,
			targetExtent,
			roughness,
			metallic,
			overrideMaterial,
			visible,
			rayTracing,
			physicsQuery);
		if (handle == Corona::InvalidSceneObjectHandle)
		{
			luaL_error(L, "failed to spawn scene object");
			return 0;
		}

		lua_pushinteger(L, static_cast<int>(handle));
		return 1;
	}

	int LuaCoronaSetTransform(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const Corona::SceneObjectHandle handle = static_cast<Corona::SceneObjectHandle>(luaL_checkinteger(L, 1));
		glm::vec3 position(0.0f);
		glm::vec3 rotationDegrees(0.0f);
		float targetExtent = 1.0f;

		if (!ReadTransformDesc(L, 2, position, rotationDegrees, targetExtent, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr))
		{
			luaL_error(L, "corona.set_transform expects a descriptor table");
			return 0;
		}

		lua_pushboolean(L, host->SetSceneObjectTransformForScript(handle, position, rotationDegrees, targetExtent) ? 1 : 0);
		return 1;
	}

	int LuaCoronaGetTransform(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const Corona::SceneObjectHandle handle = static_cast<Corona::SceneObjectHandle>(luaL_checkinteger(L, 1));
		glm::vec3 position(0.0f);
		glm::vec3 rotationDegrees(0.0f);
		float targetExtent = 1.0f;
		if (!host->GetSceneObjectTransformForScript(handle, position, rotationDegrees, targetExtent))
		{
			lua_pushnil(L);
			return 1;
		}

		lua_newtable(L);
		PushVec3(L, position);
		lua_setfield(L, -2, "position");
		PushVec3(L, rotationDegrees);
		lua_setfield(L, -2, "rotation");
		lua_pushnumber(L, static_cast<double>(targetExtent));
		lua_setfield(L, -2, "target_extent");
		return 1;
	}

	int LuaCoronaSetVisible(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const Corona::SceneObjectHandle handle = static_cast<Corona::SceneObjectHandle>(luaL_checkinteger(L, 1));
		const bool visible = luaL_checkboolean(L, 2) != 0;
		lua_pushboolean(L, host->SetSceneObjectVisibility(handle, visible) ? 1 : 0);
		return 1;
	}

	int LuaCoronaSetRayTracing(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const Corona::SceneObjectHandle handle = static_cast<Corona::SceneObjectHandle>(luaL_checkinteger(L, 1));
		const bool enabled = luaL_checkboolean(L, 2) != 0;
		lua_pushboolean(L, host->SetSceneObjectRayTracingEnabled(handle, enabled) ? 1 : 0);
		return 1;
	}

	int LuaCoronaDestroy(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const Corona::SceneObjectHandle handle = static_cast<Corona::SceneObjectHandle>(luaL_checkinteger(L, 1));
		lua_pushboolean(L, host->RemoveSceneObject(handle) ? 1 : 0);
		return 1;
	}

	int LuaCoronaSetCameraControl(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		lua_pushboolean(L, host->SetScriptCameraControlForScript(luaL_checkboolean(L, 1) != 0) ? 1 : 0);
		return 1;
	}

	int LuaCoronaSetCamera(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}
		if (!lua_istable(L, 1))
		{
			luaL_error(L, "corona.set_camera expects a descriptor table");
			return 0;
		}

		const int tableIndex = lua_absindex(L, 1);
		glm::vec3 position(0.0f);
		glm::vec3 lookAt(0.0f);
		glm::vec3 upDirection(0.0f, 1.0f, 0.0f);
		const bool hasPosition = ReadVec3Field(L, tableIndex, "position", position);
		bool hasLookAt = ReadVec3Field(L, tableIndex, "look_at", lookAt);
		if (!hasLookAt)
			hasLookAt = ReadVec3Field(L, tableIndex, "lookAt", lookAt);
		if (!hasLookAt)
			hasLookAt = ReadVec3Field(L, tableIndex, "target", lookAt);
		ReadVec3Field(L, tableIndex, "up", upDirection);

		if (!hasPosition || !hasLookAt)
		{
			luaL_error(L, "corona.set_camera expects position and look_at vectors");
			return 0;
		}

		lua_pushboolean(L, host->SetCameraForScript(position, lookAt, upDirection) ? 1 : 0);
		return 1;
	}

	int LuaCoronaGetCamera(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		glm::vec3 position(0.0f);
		glm::vec3 forward(0.0f);
		glm::vec3 right(1.0f, 0.0f, 0.0f);
		glm::vec3 up(0.0f, 1.0f, 0.0f);
		float yawDegrees = 0.0f;
		float pitchDegrees = 0.0f;
		host->GetCameraForScript(position, forward, right, up, yawDegrees, pitchDegrees);

		lua_newtable(L);
		PushVec3(L, position);
		lua_setfield(L, -2, "position");
		PushVec3(L, forward);
		lua_setfield(L, -2, "forward");
		PushVec3(L, right);
		lua_setfield(L, -2, "right");
		PushVec3(L, up);
		lua_setfield(L, -2, "up");
		lua_pushnumber(L, static_cast<double>(yawDegrees));
		lua_setfield(L, -2, "yaw");
		lua_pushnumber(L, static_cast<double>(pitchDegrees));
		lua_setfield(L, -2, "pitch");
		return 1;
	}

	int LuaCoronaRaycast(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		glm::vec3 origin(0.0f);
		glm::vec3 direction(0.0f, 0.0f, 1.0f);
		float maxDistance = 10000.0f;
		if (lua_istable(L, 1))
		{
			const int tableIndex = lua_absindex(L, 1);
			if (!ReadVec3Field(L, tableIndex, "origin", origin))
			{
				luaL_error(L, "corona.raycast expects an origin vector");
				return 0;
			}
			if (!ReadVec3Field(L, tableIndex, "direction", direction))
			{
				luaL_error(L, "corona.raycast expects a direction vector");
				return 0;
			}
			if (!ReadNumberField(L, tableIndex, "max_distance", maxDistance))
				ReadNumberField(L, tableIndex, "maxDistance", maxDistance);
		}
		else
		{
			if (!ReadVec3At(L, 1, origin) || !ReadVec3At(L, 2, direction))
			{
				luaL_error(L, "corona.raycast expects origin and direction vectors");
				return 0;
			}
			if (lua_isnumber(L, 3))
				maxDistance = static_cast<float>(lua_tonumber(L, 3));
		}

		Corona::CpuPhysicsRaycastHit hit;
		if (!host->CpuPhysicsRaycastForScript(origin, direction, maxDistance, hit))
		{
			lua_pushnil(L);
			return 1;
		}

		lua_newtable(L);
		lua_pushinteger(L, static_cast<int>(hit.ObjectHandle));
		lua_setfield(L, -2, "object");
		lua_pushnumber(L, static_cast<double>(hit.Distance));
		lua_setfield(L, -2, "distance");
		PushVec3(L, hit.Position);
		lua_setfield(L, -2, "position");
		PushVec3(L, hit.Normal);
		lua_setfield(L, -2, "normal");
		return 1;
	}

	int LuaCoronaIsKeyDown(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		UINT8 key = 0;
		if (!TryReadKeyCode(L, 1, key))
		{
			luaL_error(L, "unknown key");
			return 0;
		}

		lua_pushboolean(L, host->IsScriptKeyDownForScript(key) ? 1 : 0);
		return 1;
	}

	int LuaCoronaWasKeyPressed(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		UINT8 key = 0;
		if (!TryReadKeyCode(L, 1, key))
		{
			luaL_error(L, "unknown key");
			return 0;
		}

		lua_pushboolean(L, host->WasScriptKeyPressedForScript(key) ? 1 : 0);
		return 1;
	}

	int LuaCoronaWasKeyReleased(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		UINT8 key = 0;
		if (!TryReadKeyCode(L, 1, key))
		{
			luaL_error(L, "unknown key");
			return 0;
		}

		lua_pushboolean(L, host->WasScriptKeyReleasedForScript(key) ? 1 : 0);
		return 1;
	}

	void RegisterCoronaApi(lua_State* L)
	{
		lua_newtable(L);

		lua_pushcfunction(L, LuaCoronaLog, "corona.log");
		lua_setfield(L, -2, "log");
		lua_pushcfunction(L, LuaCoronaLoadScene, "corona.load_scene");
		lua_setfield(L, -2, "load_scene");
		lua_pushcfunction(L, LuaCoronaSpawn, "corona.spawn");
		lua_setfield(L, -2, "spawn");
		lua_pushcfunction(L, LuaCoronaSetTransform, "corona.set_transform");
		lua_setfield(L, -2, "set_transform");
		lua_pushcfunction(L, LuaCoronaGetTransform, "corona.get_transform");
		lua_setfield(L, -2, "get_transform");
		lua_pushcfunction(L, LuaCoronaSetVisible, "corona.set_visible");
		lua_setfield(L, -2, "set_visible");
		lua_pushcfunction(L, LuaCoronaSetRayTracing, "corona.set_ray_tracing");
		lua_setfield(L, -2, "set_ray_tracing");
		lua_pushcfunction(L, LuaCoronaDestroy, "corona.destroy");
		lua_setfield(L, -2, "destroy");
		lua_pushcfunction(L, LuaCoronaSetCameraControl, "corona.set_camera_control");
		lua_setfield(L, -2, "set_camera_control");
		lua_pushcfunction(L, LuaCoronaSetCamera, "corona.set_camera");
		lua_setfield(L, -2, "set_camera");
		lua_pushcfunction(L, LuaCoronaGetCamera, "corona.get_camera");
		lua_setfield(L, -2, "get_camera");
		lua_pushcfunction(L, LuaCoronaRaycast, "corona.raycast");
		lua_setfield(L, -2, "raycast");
		lua_pushcfunction(L, LuaCoronaIsKeyDown, "corona.is_key_down");
		lua_setfield(L, -2, "is_key_down");
		lua_pushcfunction(L, LuaCoronaWasKeyPressed, "corona.was_key_pressed");
		lua_setfield(L, -2, "was_key_pressed");
		lua_pushcfunction(L, LuaCoronaWasKeyReleased, "corona.was_key_released");
		lua_setfield(L, -2, "was_key_released");

		lua_setglobal(L, "corona");
	}

	std::string ReadTextFileUtf8(const std::filesystem::path& path)
	{
		std::ifstream file(path, std::ios::binary);
		if (!file.is_open())
			return std::string();

		std::ostringstream ss;
		ss << file.rdbuf();
		return ss.str();
	}
}

Corona::ScriptSceneHandle Corona::LoadSceneForScript(const std::wstring& assetPath)
{
	if (!renderBackend || assetPath.empty())
		return InvalidScriptSceneHandle;

	std::filesystem::path path(assetPath);
	if (path.is_relative())
		path = GetAssetFullPath(assetPath.c_str());

	std::error_code ec;
	std::filesystem::path normalizedPath = std::filesystem::weakly_canonical(path, ec);
	if (ec)
		normalizedPath = std::filesystem::absolute(path, ec).lexically_normal();
	if (normalizedPath.empty())
		normalizedPath = path.lexically_normal();

	const std::wstring key = normalizedPath.wstring();
	const auto cachedIt = ScriptSceneByPath.find(key);
	if (cachedIt != ScriptSceneByPath.end())
		return cachedIt->second;

	std::filesystem::path pistolPath = GetAssetFullPath(L"assets\\pistol\\pistol.obj");
	std::filesystem::path normalizedPistolPath = std::filesystem::weakly_canonical(pistolPath, ec);
	if (ec)
		normalizedPistolPath = std::filesystem::absolute(pistolPath, ec).lexically_normal();
	const bool bIsPistol = _wcsicmp(key.c_str(), normalizedPistolPath.wstring().c_str()) == 0;

	shared_ptr<Scene> scene = (bIsPistol && Pistol) ? Pistol : LoadModel(WideToUtf8Local(key));
	if (!scene)
		return InvalidScriptSceneHandle;
	if (bIsPistol)
		Pistol = scene;

	ScriptSceneHandle handle = NextScriptSceneHandle++;
	if (handle == InvalidScriptSceneHandle)
		handle = NextScriptSceneHandle++;

	ScriptScenes[handle] = { scene, key };
	ScriptSceneByPath[key] = handle;
	AppendCpuRuntimeTrace(L"[Luau] load_scene handle=" + std::to_wstring(handle) + L" path=" + key);
	return handle;
}

Corona::SceneObjectHandle Corona::SpawnSceneObjectForScript(
	ScriptSceneHandle sceneHandle,
	const glm::vec3& position,
	const glm::vec3& rotationDegrees,
	float targetExtent,
	float roughness,
	float metallic,
	bool bOverrideRoughnessMetallic,
	bool bVisible,
	bool bRayTracing,
	bool bPhysicsQuery)
{
	const auto sceneIt = ScriptScenes.find(sceneHandle);
	if (sceneIt == ScriptScenes.end() || !sceneIt->second.ScenePtr)
		return InvalidSceneObjectHandle;

	SceneObjectDesc desc;
	desc.ScenePtr = sceneIt->second.ScenePtr;
	desc.Transform = BuildCenteredSceneTransform(desc.ScenePtr, std::max(targetExtent, 0.001f), position, rotationDegrees);
	desc.Roughness = roughness;
	desc.Metallic = metallic;
	desc.bOverrideRoughnessMetallic = bOverrideRoughnessMetallic;
	desc.bVisible = bVisible;
	desc.bRayTracing = bRayTracing;
	desc.bPhysicsQuery = bPhysicsQuery;

	const SceneObjectHandle handle = AddSceneObject(desc);
	if (handle == InvalidSceneObjectHandle)
		return handle;

	ScriptObjects[handle] = { sceneHandle, position, rotationDegrees, std::max(targetExtent, 0.001f) };
	if (desc.ScenePtr == Pistol && PistolObject == InvalidSceneObjectHandle)
	{
		PistolObject = handle;
		PistolCenterPosition = position;
		PistolCenterRotationDegrees = rotationDegrees;
	}

	AppendCpuRuntimeTrace(L"[Luau] spawn object=" + std::to_wstring(handle) + L" scene=" + std::to_wstring(sceneHandle));
	return handle;
}

bool Corona::SetSceneObjectTransformForScript(
	SceneObjectHandle handle,
	const glm::vec3& position,
	const glm::vec3& rotationDegrees,
	float targetExtent)
{
	const auto stateIt = ScriptObjects.find(handle);
	if (stateIt == ScriptObjects.end())
		return false;

	const auto sceneIt = ScriptScenes.find(stateIt->second.SceneHandle);
	if (sceneIt == ScriptScenes.end() || !sceneIt->second.ScenePtr)
		return false;

	const float safeTargetExtent = std::max(targetExtent, 0.001f);
	stateIt->second.Position = position;
	stateIt->second.RotationDegrees = rotationDegrees;
	stateIt->second.TargetExtent = safeTargetExtent;

	if (handle == PistolObject)
	{
		PistolCenterPosition = position;
		PistolCenterRotationDegrees = rotationDegrees;
	}

	return SetSceneObjectTransform(
		handle,
		BuildCenteredSceneTransform(sceneIt->second.ScenePtr, safeTargetExtent, position, rotationDegrees));
}

bool Corona::GetSceneObjectTransformForScript(
	SceneObjectHandle handle,
	glm::vec3& position,
	glm::vec3& rotationDegrees,
	float& targetExtent) const
{
	const auto stateIt = ScriptObjects.find(handle);
	if (stateIt == ScriptObjects.end())
		return false;

	position = stateIt->second.Position;
	rotationDegrees = stateIt->second.RotationDegrees;
	targetExtent = stateIt->second.TargetExtent;
	return true;
}

bool Corona::SetScriptCameraControlForScript(bool enabled)
{
	bScriptCameraControlEnabled = enabled;
	m_camera.m_keysPressed = {};
	m_camera.m_mouseButtonDown = false;
	return true;
}

bool Corona::SetCameraForScript(
	const glm::vec3& position,
	const glm::vec3& lookAt,
	const glm::vec3& upDirection)
{
	const glm::vec3 lookVector = lookAt - position;
	const float lookLengthSq = glm::dot(lookVector, lookVector);
	if (lookLengthSq < 1.0e-6f)
		return false;

	glm::vec3 safeUp = upDirection;
	if (glm::dot(safeUp, safeUp) < 1.0e-6f)
		safeUp = glm::vec3(0.0f, 1.0f, 0.0f);

	const glm::vec3 lookDirection = glm::normalize(lookVector);
	m_camera.m_initialPosition = position;
	m_camera.m_position = position;
	m_camera.m_upDirection = glm::normalize(safeUp);
	m_camera.m_lookDirection = lookDirection;
	m_camera.m_pitch = asinf(glm::clamp(lookDirection.y, -1.0f, 1.0f));
	m_camera.m_yaw = atan2f(lookDirection.x, lookDirection.z);
	m_camera.m_keysPressed = {};
	m_camera.m_mouseButtonDown = false;
	bScriptCameraControlEnabled = true;
	return true;
}

void Corona::GetCameraForScript(
	glm::vec3& position,
	glm::vec3& forward,
	glm::vec3& right,
	glm::vec3& up,
	float& yawDegrees,
	float& pitchDegrees) const
{
	position = m_camera.m_position;
	forward = glm::normalize(m_camera.m_lookDirection);
	up = glm::normalize(m_camera.m_upDirection);
	right = glm::cross(forward, up);
	if (glm::dot(right, right) < 1.0e-6f)
		right = glm::vec3(1.0f, 0.0f, 0.0f);
	else
		right = glm::normalize(right);
	yawDegrees = glm::degrees(m_camera.m_yaw);
	pitchDegrees = glm::degrees(m_camera.m_pitch);
}

void Corona::RecordScriptKeyDown(UINT8 key)
{
	const size_t index = static_cast<size_t>(key);
	if (!ScriptKeyDown[index])
		ScriptKeyPressed[index] = true;
	ScriptKeyDown[index] = true;
}

void Corona::RecordScriptKeyUp(UINT8 key)
{
	const size_t index = static_cast<size_t>(key);
	if (ScriptKeyDown[index])
		ScriptKeyReleased[index] = true;
	ScriptKeyDown[index] = false;
}

bool Corona::IsScriptKeyDownForScript(UINT8 key) const
{
	return ScriptKeyDown[static_cast<size_t>(key)];
}

bool Corona::WasScriptKeyPressedForScript(UINT8 key) const
{
	return ScriptKeyPressed[static_cast<size_t>(key)];
}

bool Corona::WasScriptKeyReleasedForScript(UINT8 key) const
{
	return ScriptKeyReleased[static_cast<size_t>(key)];
}

void Corona::ClearScriptInputFrameState()
{
	ScriptKeyPressed.fill(false);
	ScriptKeyReleased.fill(false);
}

void Corona::InitLuauScripting()
{
	if (ScriptState)
		return;

	ScriptState = std::make_unique<LuauScriptState>();
	ScriptState->L = luaL_newstate();
	if (!ScriptState->L)
	{
		AppendCpuRuntimeTrace(L"[Luau] failed to create state");
		ScriptState.reset();
		return;
	}

	lua_State* L = ScriptState->L;
	luaL_openlibs(L);
	lua_pushlightuserdata(L, this);
	lua_setfield(L, LUA_REGISTRYINDEX, kCoronaRegistryKey);
	RegisterCoronaApi(L);
	AppendCpuRuntimeTrace(L"[Luau] initialized");
}

void Corona::RunStartupLuauScript()
{
	if (!bEnableStartupLuauScript)
		return;
	if (!ScriptState || !ScriptState->L)
		InitLuauScripting();
	if (!ScriptState || !ScriptState->L)
		return;

	const std::filesystem::path scriptPath = GetAssetFullPath(L"scripts\\startup_pistol_spin.luau");
	const std::string source = ReadTextFileUtf8(scriptPath);
	if (source.empty())
	{
		AppendCpuRuntimeTrace(L"[Luau] startup script missing or empty: " + scriptPath.wstring());
		return;
	}

	lua_State* L = ScriptState->L;
	size_t bytecodeSize = 0;
	char* bytecode = luau_compile(source.data(), source.size(), nullptr, &bytecodeSize);
	if (!bytecode)
	{
		AppendCpuRuntimeTrace(L"[Luau] compile failed: " + scriptPath.wstring());
		return;
	}

	const std::string chunkName = "=" + WideToUtf8Local(scriptPath.wstring());
	const int loadResult = luau_load(L, chunkName.c_str(), bytecode, bytecodeSize, 0);
	std::free(bytecode);
	if (loadResult != 0)
	{
		AppendCpuRuntimeTrace(L"[Luau] load failed: " + Utf8ToWideLocal(LuaToString(L, -1)));
		lua_pop(L, 1);
		return;
	}

	const int callResult = lua_pcall(L, 0, 0, 0);
	if (callResult != 0)
	{
		AppendCpuRuntimeTrace(L"[Luau] startup script error: " + Utf8ToWideLocal(LuaToString(L, -1)));
		lua_pop(L, 1);
		return;
	}

	lua_getglobal(L, "update");
	if (lua_isfunction(L, -1))
	{
		if (ScriptState->UpdateRef != LUA_REFNIL)
			lua_unref(L, ScriptState->UpdateRef);
		ScriptState->UpdateRef = lua_ref(L, -1);
	}
	else
	{
		lua_pop(L, 1);
	}

	AppendCpuRuntimeTrace(L"[Luau] startup script executed: " + scriptPath.wstring());
}

void Corona::ReloadLuauScripting()
{
	AppendCpuRuntimeTrace(L"[Luau] reload requested");

	if (ScriptState && ScriptState->L)
	{
		lua_State* L = ScriptState->L;
		lua_getglobal(L, "shutdown");
		if (lua_isfunction(L, -1))
		{
			const int shutdownResult = lua_pcall(L, 0, 0, 0);
			if (shutdownResult != 0)
			{
				AppendCpuRuntimeTrace(L"[Luau] shutdown error during reload: " + Utf8ToWideLocal(LuaToString(L, -1)));
				lua_pop(L, 1);
			}
		}
		else
		{
			lua_pop(L, 1);
		}
	}

	std::vector<SceneObjectHandle> scriptObjectHandles;
	scriptObjectHandles.reserve(ScriptObjects.size());
	for (const auto& entry : ScriptObjects)
		scriptObjectHandles.push_back(entry.first);

	for (SceneObjectHandle handle : scriptObjectHandles)
		RemoveSceneObject(handle);
	ScriptObjects.clear();

	ShutdownLuauScripting();
	InitLuauScripting();
	RunStartupLuauScript();
	ResetAllAccumulationState(false);

	AppendCpuRuntimeTrace(L"[Luau] reload complete");
}

void Corona::UpdateLuauScripting(float dt)
{
	if (!ScriptState || !ScriptState->L || ScriptState->UpdateRef == LUA_REFNIL)
		return;

	lua_State* L = ScriptState->L;
	lua_getref(L, ScriptState->UpdateRef);
	lua_pushnumber(L, static_cast<double>(dt));
	const int result = lua_pcall(L, 1, 0, 0);
	if (result != 0)
	{
		AppendCpuRuntimeTrace(L"[Luau] update error: " + Utf8ToWideLocal(LuaToString(L, -1)));
		lua_pop(L, 1);
		lua_unref(L, ScriptState->UpdateRef);
		ScriptState->UpdateRef = LUA_REFNIL;
	}
}

void Corona::ShutdownLuauScripting()
{
	if (!ScriptState)
		return;

	if (ScriptState->L)
	{
		if (ScriptState->UpdateRef != LUA_REFNIL)
		{
			lua_unref(ScriptState->L, ScriptState->UpdateRef);
			ScriptState->UpdateRef = LUA_REFNIL;
		}
		lua_close(ScriptState->L);
		ScriptState->L = nullptr;
	}
	ScriptState.reset();
	ScriptScenes.clear();
	ScriptSceneByPath.clear();
	ScriptObjects.clear();
	bScriptCameraControlEnabled = false;
	ScriptKeyDown.fill(false);
	ClearScriptInputFrameState();
}
