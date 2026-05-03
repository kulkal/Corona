//*********************************************************
//
// Luau scripting bridge for high-level scene/object handles.
//
//*********************************************************

#include "stdafx.h"
#include "Corona.h"
#include "Win32Application.h"

#include "imgui.h"
#include "imGuIZMO.h"

#include "lua.h"
#include "lualib.h"
#include "luacode.h"

#include <algorithm>
#include <cctype>
#include <codecvt>
#include <cmath>
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

	int LuaCoronaCreateBlockCharacterScene(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		UINT32 seed = 20260503u;
		if (lua_istable(L, 1))
		{
			lua_getfield(L, 1, "seed");
			if (lua_isnumber(L, -1))
				seed = static_cast<UINT32>(std::max<lua_Integer>(1, lua_tointeger(L, -1)));
			lua_pop(L, 1);
		}
		else if (lua_isnumber(L, 1))
		{
			seed = static_cast<UINT32>(std::max<lua_Integer>(1, lua_tointeger(L, 1)));
		}

		const Corona::ScriptSceneHandle handle = host->CreateProceduralBlockCharacterSceneForScript(seed);
		if (handle == Corona::InvalidScriptSceneHandle)
		{
			luaL_error(L, "failed to create procedural block character scene");
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

	int LuaCoronaGetMouse(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		int x = 0;
		int y = 0;
		int deltaX = 0;
		int deltaY = 0;
		bool rightDown = false;
		bool rightPressed = false;
		bool rightReleased = false;
		host->GetScriptMouseForScript(x, y, deltaX, deltaY, rightDown, rightPressed, rightReleased);

		lua_newtable(L);
		lua_pushinteger(L, x);
		lua_setfield(L, -2, "x");
		lua_pushinteger(L, y);
		lua_setfield(L, -2, "y");
		lua_pushinteger(L, deltaX);
		lua_setfield(L, -2, "dx");
		lua_pushinteger(L, deltaY);
		lua_setfield(L, -2, "dy");
		lua_pushboolean(L, rightDown ? 1 : 0);
		lua_setfield(L, -2, "right_down");
		lua_pushboolean(L, rightPressed ? 1 : 0);
		lua_setfield(L, -2, "right_pressed");
		lua_pushboolean(L, rightReleased ? 1 : 0);
		lua_setfield(L, -2, "right_released");
		return 1;
	}

	int LuaCoronaGetUiState(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		host->PushLuauUiStateForScript(L);
		return 1;
	}

	int LuaCoronaSetUiValue(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const char* name = luaL_checkstring(L, 1);
		lua_pushboolean(L, host->SetLuauUiValueForScript(name ? name : "", L, 2) ? 1 : 0);
		return 1;
	}

	int LuaCoronaRunUiCommand(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const char* name = luaL_checkstring(L, 1);
		lua_pushboolean(L, host->RunLuauUiCommandForScript(name ? name : "", L, 2) ? 1 : 0);
		return 1;
	}

	int LuaCoronaGetPersistentControl(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const char* name = luaL_checkstring(L, 1);
		host->PushPersistentScriptControlForScript(L, name ? name : "", 2);
		return 1;
	}

	int LuaCoronaSetPersistentControl(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const char* name = luaL_checkstring(L, 1);
		lua_pushboolean(L, host->SetPersistentScriptControlForScript(name ? name : "", L, 2) ? 1 : 0);
		return 1;
	}

	bool RequireImGuiFrame(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return false;
		}
		if (!host->IsLuauImGuiFrameActive())
		{
			luaL_error(L, "imgui.* can only be called from script.imgui");
			return false;
		}
		return true;
	}

	int LuaImGuiBegin(lua_State* L)
	{
		if (!RequireImGuiFrame(L))
			return 0;

		const char* name = luaL_checkstring(L, 1);
		lua_pushboolean(L, ImGui::Begin(name) ? 1 : 0);
		return 1;
	}

	int LuaImGuiEnd(lua_State* L)
	{
		if (!RequireImGuiFrame(L))
			return 0;

		ImGui::End();
		return 0;
	}

	int LuaImGuiText(lua_State* L)
	{
		if (!RequireImGuiFrame(L))
			return 0;

		ImGui::TextUnformatted(luaL_checkstring(L, 1));
		return 0;
	}

	int LuaImGuiTextWrapped(lua_State* L)
	{
		if (!RequireImGuiFrame(L))
			return 0;

		ImGui::TextWrapped("%s", luaL_checkstring(L, 1));
		return 0;
	}

	int LuaImGuiTextDisabled(lua_State* L)
	{
		if (!RequireImGuiFrame(L))
			return 0;

		ImGui::TextDisabled("%s", luaL_checkstring(L, 1));
		return 0;
	}

	int LuaImGuiSeparator(lua_State* L)
	{
		if (!RequireImGuiFrame(L))
			return 0;

		ImGui::Separator();
		return 0;
	}

	int LuaImGuiSameLine(lua_State* L)
	{
		if (!RequireImGuiFrame(L))
			return 0;

		const int top = lua_gettop(L);
		if (top >= 1)
		{
			const float offset = static_cast<float>(luaL_checknumber(L, 1));
			const float spacing = top >= 2 ? static_cast<float>(luaL_checknumber(L, 2)) : -1.0f;
			ImGui::SameLine(offset, spacing);
		}
		else
		{
			ImGui::SameLine();
		}
		return 0;
	}

	int LuaImGuiSetNextItemWidth(lua_State* L)
	{
		if (!RequireImGuiFrame(L))
			return 0;

		ImGui::SetNextItemWidth(static_cast<float>(luaL_checknumber(L, 1)));
		return 0;
	}

	int LuaImGuiBeginDisabled(lua_State* L)
	{
		if (!RequireImGuiFrame(L))
			return 0;

		const bool disabled = lua_gettop(L) < 1 || lua_toboolean(L, 1) != 0;
		ImGui::BeginDisabled(disabled);
		return 0;
	}

	int LuaImGuiEndDisabled(lua_State* L)
	{
		if (!RequireImGuiFrame(L))
			return 0;

		ImGui::EndDisabled();
		return 0;
	}

	int LuaImGuiButton(lua_State* L)
	{
		if (!RequireImGuiFrame(L))
			return 0;

		lua_pushboolean(L, ImGui::Button(luaL_checkstring(L, 1)) ? 1 : 0);
		return 1;
	}

	int LuaImGuiCheckbox(lua_State* L)
	{
		if (!RequireImGuiFrame(L))
			return 0;

		bool value = luaL_checkboolean(L, 2) != 0;
		const bool changed = ImGui::Checkbox(luaL_checkstring(L, 1), &value);
		lua_pushboolean(L, value ? 1 : 0);
		lua_pushboolean(L, changed ? 1 : 0);
		return 2;
	}

	int LuaImGuiCombo(lua_State* L)
	{
		if (!RequireImGuiFrame(L))
			return 0;

		const char* label = luaL_checkstring(L, 1);
		int currentIndex = static_cast<int>(luaL_checkinteger(L, 2)) - 1;
		luaL_checktype(L, 3, LUA_TTABLE);

		const int itemCount = lua_objlen(L, 3);
		std::vector<std::string> itemStorage;
		std::vector<const char*> itemPointers;
		itemStorage.reserve(itemCount);
		itemPointers.reserve(itemCount);
		for (int itemIndex = 1; itemIndex <= itemCount; ++itemIndex)
		{
			lua_rawgeti(L, 3, itemIndex);
			itemStorage.emplace_back(lua_isstring(L, -1) ? lua_tostring(L, -1) : "");
			lua_pop(L, 1);
		}
		for (const std::string& item : itemStorage)
			itemPointers.push_back(item.c_str());

		currentIndex = std::clamp(currentIndex, 0, std::max(0, itemCount - 1));
		const bool changed = itemCount > 0 && ImGui::Combo(label, &currentIndex, itemPointers.data(), itemCount);
		lua_pushinteger(L, currentIndex + 1);
		lua_pushboolean(L, changed ? 1 : 0);
		return 2;
	}

	int LuaImGuiSliderFloat(lua_State* L)
	{
		if (!RequireImGuiFrame(L))
			return 0;

		float value = static_cast<float>(luaL_checknumber(L, 2));
		const float minValue = static_cast<float>(luaL_checknumber(L, 3));
		const float maxValue = static_cast<float>(luaL_checknumber(L, 4));
		const char* format = lua_gettop(L) >= 5 && lua_isstring(L, 5) ? lua_tostring(L, 5) : "%.2f";
		const bool changed = ImGui::SliderFloat(luaL_checkstring(L, 1), &value, minValue, maxValue, format);
		lua_pushnumber(L, static_cast<double>(value));
		lua_pushboolean(L, changed ? 1 : 0);
		return 2;
	}

	int LuaImGuiSliderInt(lua_State* L)
	{
		if (!RequireImGuiFrame(L))
			return 0;

		int value = static_cast<int>(luaL_checkinteger(L, 2));
		const int minValue = static_cast<int>(luaL_checkinteger(L, 3));
		const int maxValue = static_cast<int>(luaL_checkinteger(L, 4));
		const bool changed = ImGui::SliderInt(luaL_checkstring(L, 1), &value, minValue, maxValue);
		lua_pushinteger(L, value);
		lua_pushboolean(L, changed ? 1 : 0);
		return 2;
	}

	int LuaImGuiDragFloat(lua_State* L)
	{
		if (!RequireImGuiFrame(L))
			return 0;

		float value = static_cast<float>(luaL_checknumber(L, 2));
		const int top = lua_gettop(L);
		const float speed = top >= 3 ? static_cast<float>(luaL_checknumber(L, 3)) : 1.0f;
		const float minValue = top >= 4 ? static_cast<float>(luaL_checknumber(L, 4)) : 0.0f;
		const float maxValue = top >= 5 ? static_cast<float>(luaL_checknumber(L, 5)) : 0.0f;
		const char* format = top >= 6 && lua_isstring(L, 6) ? lua_tostring(L, 6) : "%.2f";
		const bool changed = ImGui::DragFloat(luaL_checkstring(L, 1), &value, speed, minValue, maxValue, format);
		lua_pushnumber(L, static_cast<double>(value));
		lua_pushboolean(L, changed ? 1 : 0);
		return 2;
	}

	int LuaImGuiColorEdit3(lua_State* L)
	{
		if (!RequireImGuiFrame(L))
			return 0;

		float color[3] = {};
		if (lua_istable(L, 2))
		{
			for (int componentIndex = 1; componentIndex <= 3; ++componentIndex)
			{
				lua_rawgeti(L, 2, componentIndex);
				color[componentIndex - 1] = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : 0.0f;
				lua_pop(L, 1);
			}
		}
		else
		{
			color[0] = static_cast<float>(luaL_checknumber(L, 2));
			color[1] = static_cast<float>(luaL_checknumber(L, 3));
			color[2] = static_cast<float>(luaL_checknumber(L, 4));
		}

		const bool changed = ImGui::ColorEdit3(luaL_checkstring(L, 1), color);
		lua_newtable(L);
		for (int componentIndex = 1; componentIndex <= 3; ++componentIndex)
		{
			lua_pushnumber(L, static_cast<double>(color[componentIndex - 1]));
			lua_rawseti(L, -2, componentIndex);
		}
		lua_pushboolean(L, changed ? 1 : 0);
		return 2;
	}

	int LuaImGuiGizmo3D(lua_State* L)
	{
		if (!RequireImGuiFrame(L))
			return 0;

		const char* label = luaL_checkstring(L, 1);
		luaL_checktype(L, 2, LUA_TTABLE);
		const float size = lua_gettop(L) >= 3 && lua_isnumber(L, 3) ? static_cast<float>(lua_tonumber(L, 3)) : 200.0f;
		const int mode = lua_gettop(L) >= 4 && lua_isnumber(L, 4) ? static_cast<int>(lua_tointeger(L, 4)) : 200;

		glm::vec3 direction(0.0f, 1.0f, 0.0f);
		for (int componentIndex = 1; componentIndex <= 3; ++componentIndex)
		{
			lua_rawgeti(L, 2, componentIndex);
			if (lua_isnumber(L, -1))
				direction[componentIndex - 1] = static_cast<float>(lua_tonumber(L, -1));
			lua_pop(L, 1);
		}

		const bool changed = ImGui::gizmo3D(label, direction, size, mode);
		lua_newtable(L);
		for (int componentIndex = 1; componentIndex <= 3; ++componentIndex)
		{
			lua_pushnumber(L, static_cast<double>(direction[componentIndex - 1]));
			lua_rawseti(L, -2, componentIndex);
		}
		lua_pushboolean(L, changed ? 1 : 0);
		return 2;
	}

	int LuaUiText(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		host->QueueScriptUiTextForScript(luaL_checkstring(L, 1));
		return 0;
	}

	int LuaUiSeparator(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		host->QueueScriptUiSeparatorForScript();
		return 0;
	}

	int LuaUiSameLine(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		host->QueueScriptUiSameLineForScript();
		return 0;
	}

	int LuaUiBeginWindow(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const char* title = luaL_checkstring(L, 1);
		host->QueueScriptUiBeginWindowForScript(title ? title : "");
		return 0;
	}

	int LuaUiEndWindow(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		host->QueueScriptUiEndWindowForScript();
		return 0;
	}

	int LuaUiOverlayText(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const char* text = luaL_checkstring(L, 1);
		const float x = static_cast<float>(luaL_optnumber(L, 2, 10.0));
		const float y = static_cast<float>(luaL_optnumber(L, 3, 28.0));
		host->QueueScriptUiOverlayTextForScript(text ? text : "", x, y);
		return 0;
	}

	int LuaUiWorldAxis(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const char* id = luaL_checkstring(L, 1);
		glm::vec3 position(0.0f);
		if (!ReadVec3At(L, 2, position))
		{
			luaL_error(L, "ui.world_axis expects a vec3 table as its second argument");
			return 0;
		}
		const float length = static_cast<float>(luaL_optnumber(L, 3, 120.0));
		const float thickness = static_cast<float>(luaL_optnumber(L, 4, 2.0));
		host->QueueScriptUiWorldAxisForScript(id ? id : "", position, length, thickness);
		return 0;
	}

	int LuaUiGizmo3D(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const char* label = luaL_checkstring(L, 1);
		const char* id = luaL_checkstring(L, 2);
		glm::vec3 value(0.0f, 1.0f, 0.0f);
		if (!ReadVec3At(L, 3, value))
		{
			luaL_error(L, "ui.gizmo3d expects a vec3 table as its third argument");
			return 0;
		}
		const float size = static_cast<float>(luaL_optnumber(L, 4, 200.0));
		const int mode = static_cast<int>(luaL_optinteger(L, 5, 200));
		PushVec3(L, host->QueueScriptUiGizmo3DForScript(id ? id : "", label ? label : "", value, size, mode));
		return 1;
	}

	int LuaUiButton(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const char* label = luaL_checkstring(L, 1);
		const char* id = luaL_checkstring(L, 2);
		lua_pushboolean(L, host->QueueScriptUiButtonForScript(id ? id : "", label ? label : "") ? 1 : 0);
		return 1;
	}

	int LuaUiSliderFloat(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const char* label = luaL_checkstring(L, 1);
		const char* id = luaL_checkstring(L, 2);
		const float value = static_cast<float>(luaL_checknumber(L, 3));
		const float minValue = static_cast<float>(luaL_checknumber(L, 4));
		const float maxValue = static_cast<float>(luaL_checknumber(L, 5));
		lua_pushnumber(L, static_cast<double>(host->QueueScriptUiSliderFloatForScript(
			id ? id : "",
			label ? label : "",
			value,
			minValue,
			maxValue)));
		return 1;
	}

	int LuaUiSliderInt(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const char* label = luaL_checkstring(L, 1);
		const char* id = luaL_checkstring(L, 2);
		const int value = static_cast<int>(luaL_checkinteger(L, 3));
		const int minValue = static_cast<int>(luaL_checkinteger(L, 4));
		const int maxValue = static_cast<int>(luaL_checkinteger(L, 5));
		lua_pushinteger(L, host->QueueScriptUiSliderIntForScript(
			id ? id : "",
			label ? label : "",
			value,
			minValue,
			maxValue));
		return 1;
	}

	int LuaUiCombo(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const char* label = luaL_checkstring(L, 1);
		const char* id = luaL_checkstring(L, 2);
		const int selectedIndex = static_cast<int>(luaL_checkinteger(L, 3));
		luaL_checktype(L, 4, LUA_TTABLE);

		const int itemCount = lua_objlen(L, 4);
		std::vector<std::string> items;
		items.reserve(itemCount);
		for (int itemIndex = 1; itemIndex <= itemCount; ++itemIndex)
		{
			lua_rawgeti(L, 4, itemIndex);
			items.emplace_back(lua_isstring(L, -1) ? lua_tostring(L, -1) : "");
			lua_pop(L, 1);
		}

		lua_pushinteger(L, host->QueueScriptUiComboForScript(
			id ? id : "",
			label ? label : "",
			selectedIndex,
			items));
		return 1;
	}

	int LuaUiCheckbox(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const char* label = luaL_checkstring(L, 1);
		const char* id = luaL_checkstring(L, 2);
		const bool value = luaL_checkboolean(L, 3) != 0;
		lua_pushboolean(L, host->QueueScriptUiCheckboxForScript(id ? id : "", label ? label : "", value) ? 1 : 0);
		return 1;
	}

	void RegisterCoronaApi(lua_State* L)
	{
		lua_newtable(L);

		lua_pushcfunction(L, LuaCoronaLog, "corona.log");
		lua_setfield(L, -2, "log");
		lua_pushcfunction(L, LuaCoronaLoadScene, "corona.load_scene");
		lua_setfield(L, -2, "load_scene");
		lua_pushcfunction(L, LuaCoronaCreateBlockCharacterScene, "corona.create_block_character_scene");
		lua_setfield(L, -2, "create_block_character_scene");
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
		lua_pushcfunction(L, LuaCoronaGetMouse, "corona.get_mouse");
		lua_setfield(L, -2, "get_mouse");
		lua_pushcfunction(L, LuaCoronaGetUiState, "corona.get_ui_state");
		lua_setfield(L, -2, "get_ui_state");
		lua_pushcfunction(L, LuaCoronaSetUiValue, "corona.set_ui_value");
		lua_setfield(L, -2, "set_ui_value");
		lua_pushcfunction(L, LuaCoronaRunUiCommand, "corona.run_ui_command");
		lua_setfield(L, -2, "run_ui_command");
		lua_pushcfunction(L, LuaCoronaGetPersistentControl, "corona.get_persistent_control");
		lua_setfield(L, -2, "get_persistent_control");
		lua_pushcfunction(L, LuaCoronaSetPersistentControl, "corona.set_persistent_control");
		lua_setfield(L, -2, "set_persistent_control");

		lua_setglobal(L, "corona");
	}

	void RegisterImGuiApi(lua_State* L)
	{
		lua_newtable(L);

		lua_pushcfunction(L, LuaImGuiBegin, "imgui.begin");
		lua_setfield(L, -2, "begin");
		lua_pushcfunction(L, LuaImGuiEnd, "imgui.end");
		lua_setfield(L, -2, "end");
		lua_pushcfunction(L, LuaImGuiEnd, "imgui.end_window");
		lua_setfield(L, -2, "end_window");
		lua_pushcfunction(L, LuaImGuiText, "imgui.text");
		lua_setfield(L, -2, "text");
		lua_pushcfunction(L, LuaImGuiTextWrapped, "imgui.text_wrapped");
		lua_setfield(L, -2, "text_wrapped");
		lua_pushcfunction(L, LuaImGuiTextDisabled, "imgui.text_disabled");
		lua_setfield(L, -2, "text_disabled");
		lua_pushcfunction(L, LuaImGuiSeparator, "imgui.separator");
		lua_setfield(L, -2, "separator");
		lua_pushcfunction(L, LuaImGuiSameLine, "imgui.same_line");
		lua_setfield(L, -2, "same_line");
		lua_pushcfunction(L, LuaImGuiSetNextItemWidth, "imgui.set_next_item_width");
		lua_setfield(L, -2, "set_next_item_width");
		lua_pushcfunction(L, LuaImGuiBeginDisabled, "imgui.begin_disabled");
		lua_setfield(L, -2, "begin_disabled");
		lua_pushcfunction(L, LuaImGuiEndDisabled, "imgui.end_disabled");
		lua_setfield(L, -2, "end_disabled");
		lua_pushcfunction(L, LuaImGuiButton, "imgui.button");
		lua_setfield(L, -2, "button");
		lua_pushcfunction(L, LuaImGuiCheckbox, "imgui.checkbox");
		lua_setfield(L, -2, "checkbox");
		lua_pushcfunction(L, LuaImGuiCombo, "imgui.combo");
		lua_setfield(L, -2, "combo");
		lua_pushcfunction(L, LuaImGuiSliderFloat, "imgui.slider_float");
		lua_setfield(L, -2, "slider_float");
		lua_pushcfunction(L, LuaImGuiSliderInt, "imgui.slider_int");
		lua_setfield(L, -2, "slider_int");
		lua_pushcfunction(L, LuaImGuiDragFloat, "imgui.drag_float");
		lua_setfield(L, -2, "drag_float");
		lua_pushcfunction(L, LuaImGuiColorEdit3, "imgui.color_edit3");
		lua_setfield(L, -2, "color_edit3");
		lua_pushcfunction(L, LuaImGuiGizmo3D, "imgui.gizmo3d");
		lua_setfield(L, -2, "gizmo3d");

		lua_setglobal(L, "imgui");
	}

	void RegisterQueuedUiApi(lua_State* L)
	{
		lua_newtable(L);

		lua_pushcfunction(L, LuaUiText, "ui.text");
		lua_setfield(L, -2, "text");
		lua_pushcfunction(L, LuaUiSeparator, "ui.separator");
		lua_setfield(L, -2, "separator");
		lua_pushcfunction(L, LuaUiSameLine, "ui.same_line");
		lua_setfield(L, -2, "same_line");
		lua_pushcfunction(L, LuaUiBeginWindow, "ui.begin_window");
		lua_setfield(L, -2, "begin_window");
		lua_pushcfunction(L, LuaUiEndWindow, "ui.end_window");
		lua_setfield(L, -2, "end_window");
		lua_pushcfunction(L, LuaUiOverlayText, "ui.overlay_text");
		lua_setfield(L, -2, "overlay_text");
		lua_pushcfunction(L, LuaUiWorldAxis, "ui.world_axis");
		lua_setfield(L, -2, "world_axis");
		lua_pushcfunction(L, LuaUiGizmo3D, "ui.gizmo3d");
		lua_setfield(L, -2, "gizmo3d");
		lua_pushcfunction(L, LuaUiButton, "ui.button");
		lua_setfield(L, -2, "button");
		lua_pushcfunction(L, LuaUiSliderFloat, "ui.slider_float");
		lua_setfield(L, -2, "slider_float");
		lua_pushcfunction(L, LuaUiSliderInt, "ui.slider_int");
		lua_setfield(L, -2, "slider_int");
		lua_pushcfunction(L, LuaUiCombo, "ui.combo");
		lua_setfield(L, -2, "combo");
		lua_pushcfunction(L, LuaUiCheckbox, "ui.checkbox");
		lua_setfield(L, -2, "checkbox");

		lua_setglobal(L, "ui");
	}

	void PushBoolField(lua_State* L, const char* name, bool value)
	{
		lua_pushboolean(L, value ? 1 : 0);
		lua_setfield(L, -2, name);
	}

	void PushIntegerField(lua_State* L, const char* name, lua_Integer value)
	{
		lua_pushinteger(L, value);
		lua_setfield(L, -2, name);
	}

	void PushNumberField(lua_State* L, const char* name, double value)
	{
		lua_pushnumber(L, value);
		lua_setfield(L, -2, name);
	}

	void PushStringField(lua_State* L, const char* name, const std::string& value)
	{
		lua_pushstring(L, value.c_str());
		lua_setfield(L, -2, name);
	}

	void PushWideStringField(lua_State* L, const char* name, const std::wstring& value)
	{
		PushStringField(L, name, WideToUtf8Local(value));
	}

	void PushVec3Field(lua_State* L, const char* name, const glm::vec3& value)
	{
		lua_newtable(L);
		lua_pushnumber(L, static_cast<double>(value.x));
		lua_rawseti(L, -2, 1);
		lua_pushnumber(L, static_cast<double>(value.y));
		lua_rawseti(L, -2, 2);
		lua_pushnumber(L, static_cast<double>(value.z));
		lua_rawseti(L, -2, 3);
		lua_setfield(L, -2, name);
	}

	bool ReadVec3(lua_State* L, int index, glm::vec3& value)
	{
		if (!lua_istable(L, index))
			return false;

		index = lua_absindex(L, index);
		float components[3] = {};
		for (int componentIndex = 1; componentIndex <= 3; ++componentIndex)
		{
			lua_rawgeti(L, index, componentIndex);
			if (!lua_isnumber(L, -1))
			{
				lua_pop(L, 1);
				return false;
			}
			components[componentIndex - 1] = static_cast<float>(lua_tonumber(L, -1));
			lua_pop(L, 1);
		}
		value = glm::vec3(components[0], components[1], components[2]);
		return true;
	}

	bool IsDLSSUiMode(Corona::EAntiAliasingMode mode)
	{
		return mode == Corona::EAntiAliasingMode::DLSS_SR || mode == Corona::EAntiAliasingMode::DLSS_RR;
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

	int RefStackValueAndPop(lua_State* L)
	{
		const int ref = lua_ref(L, -1);
		lua_pop(L, 1);
		return ref;
	}

	int RefTableFunction(lua_State* L, int tableIndex, const char* fieldName)
	{
		tableIndex = lua_absindex(L, tableIndex);
		lua_getfield(L, tableIndex, fieldName);
		if (!lua_isfunction(L, -1))
		{
			lua_pop(L, 1);
			return LUA_REFNIL;
		}

		return RefStackValueAndPop(L);
	}

	int RefGlobalFunctionAndClear(lua_State* L, const char* globalName)
	{
		lua_getglobal(L, globalName);
		const int ref = lua_isfunction(L, -1) ? RefStackValueAndPop(L) : LUA_REFNIL;
		if (ref == LUA_REFNIL)
			lua_pop(L, 1);

		lua_pushnil(L);
		lua_setglobal(L, globalName);
		return ref;
	}

	void ClearLegacyScriptGlobals(lua_State* L)
	{
		lua_pushnil(L);
		lua_setglobal(L, "update");
		lua_pushnil(L);
		lua_setglobal(L, "shutdown");
	}
}

Corona::ScriptSceneHandle Corona::CreateProceduralBlockCharacterSceneForScript(UINT32 seed)
{
	if (!renderBackend)
		return InvalidScriptSceneHandle;

	const std::wstring key = L"procedural://block_character/" + std::to_wstring(seed);
	const auto cachedIt = ScriptSceneByPath.find(key);
	if (cachedIt != ScriptSceneByPath.end())
		return cachedIt->second;

	shared_ptr<Scene> scene = CreateProceduralBlockCharacterScene(seed);
	if (!scene)
		return InvalidScriptSceneHandle;

	ScriptSceneHandle handle = NextScriptSceneHandle++;
	if (handle == InvalidScriptSceneHandle)
		handle = NextScriptSceneHandle++;

	ScriptScenes[handle] = { scene, key };
	ScriptSceneByPath[key] = handle;
	AppendCpuRuntimeTrace(L"[Luau] create_block_character_scene handle=" + std::to_wstring(handle) + L" seed=" + std::to_wstring(seed));
	return handle;
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

	auto normalizeAssetPath = [this](const wchar_t* assetPath)
	{
		std::error_code pathEc;
		std::filesystem::path path = GetAssetFullPath(assetPath);
		std::filesystem::path normalized = std::filesystem::weakly_canonical(path, pathEc);
		if (pathEc)
			normalized = std::filesystem::absolute(path, pathEc).lexically_normal();
		return normalized.wstring();
	};

	const bool bIsPistol = _wcsicmp(key.c_str(), normalizeAssetPath(L"assets\\pistol\\pistol.obj").c_str()) == 0;
	const bool bIsBuddha = _wcsicmp(key.c_str(), normalizeAssetPath(L"assets\\buddha\\buddha.obj").c_str()) == 0;
	const bool bIsShaderBall = _wcsicmp(key.c_str(), normalizeAssetPath(L"assets\\shaderBall\\shaderBall.fbx").c_str()) == 0;

	shared_ptr<Scene> scene;
	if (bIsPistol && Pistol)
		scene = Pistol;
	else if (bIsBuddha && Buddha)
		scene = Buddha;
	else if (bIsShaderBall && ShaderBall)
		scene = ShaderBall;
	else
		scene = LoadModel(WideToUtf8Local(key));

	if (!scene)
		return InvalidScriptSceneHandle;
	if (bIsPistol)
		Pistol = scene;
	if (bIsBuddha)
		Buddha = scene;
	if (bIsShaderBall)
		ShaderBall = scene;

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
	else if (desc.ScenePtr == Buddha && BuddhaObject == InvalidSceneObjectHandle)
	{
		BuddhaObject = handle;
		BuddhaCenterPosition = position;
		BuddhaCenterRotationDegrees = rotationDegrees;
	}
	else if (desc.ScenePtr == ShaderBall && ShaderBallObject == InvalidSceneObjectHandle)
	{
		ShaderBallObject = handle;
		ShaderBallCenterPosition = position;
		ShaderBallCenterRotationDegrees = rotationDegrees;
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
	else if (handle == BuddhaObject)
	{
		BuddhaCenterPosition = position;
		BuddhaCenterRotationDegrees = rotationDegrees;
	}
	else if (handle == ShaderBallObject)
	{
		ShaderBallCenterPosition = position;
		ShaderBallCenterRotationDegrees = rotationDegrees;
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
	const bool bChanged = bScriptCameraControlEnabled != enabled;
	bScriptCameraControlEnabled = enabled;
	if (bChanged)
	{
		m_camera.m_keysPressed = {};
		m_camera.m_mouseButtonDown = false;
	}
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

void Corona::RecordScriptRButtonDown(int x, int y)
{
	if (!bScriptRightMouseDown)
		bScriptRightMousePressed = true;
	bScriptRightMouseDown = true;
	ScriptMouseX = x;
	ScriptMouseY = y;
	ScriptMouseDeltaX = 0;
	ScriptMouseDeltaY = 0;
	bScriptMousePositionInitialized = true;
}

void Corona::RecordScriptRButtonUp()
{
	if (bScriptRightMouseDown)
		bScriptRightMouseReleased = true;
	bScriptRightMouseDown = false;
}

void Corona::RecordScriptMouseMove(int x, int y)
{
	if (!bScriptMousePositionInitialized)
	{
		ScriptMouseX = x;
		ScriptMouseY = y;
		bScriptMousePositionInitialized = true;
		return;
	}

	const int deltaX = x - ScriptMouseX;
	const int deltaY = y - ScriptMouseY;
	ScriptMouseDeltaX += deltaX;
	ScriptMouseDeltaY += deltaY;
	ScriptMouseX = x;
	ScriptMouseY = y;
}

void Corona::PollScriptMouseState()
{
	HWND hwnd = Win32Application::GetHwnd();
	if (!hwnd)
		return;

	const bool bWindowCanReceiveMouse =
		GetForegroundWindow() == hwnd ||
		GetCapture() == hwnd;
	if (!bWindowCanReceiveMouse)
	{
		if (bScriptRightMouseDown)
			RecordScriptRButtonUp();
		return;
	}

	POINT cursorPosition = {};
	if (!GetCursorPos(&cursorPosition))
		return;
	if (!ScreenToClient(hwnd, &cursorPosition))
		return;

	const bool bRightDownNow = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
	if (bRightDownNow && GetCapture() == nullptr)
		SetCapture(hwnd);
	else if (!bRightDownNow && GetCapture() == hwnd)
		ReleaseCapture();

	if (bRightDownNow && !bScriptRightMouseDown)
		RecordScriptRButtonDown(cursorPosition.x, cursorPosition.y);
	else if (!bRightDownNow && bScriptRightMouseDown)
		RecordScriptRButtonUp();

	if (bRightDownNow)
		RecordScriptMouseMove(cursorPosition.x, cursorPosition.y);
	else if (!bScriptMousePositionInitialized)
	{
		ScriptMouseX = cursorPosition.x;
		ScriptMouseY = cursorPosition.y;
		bScriptMousePositionInitialized = true;
	}
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

void Corona::GetScriptMouseForScript(
	int& x,
	int& y,
	int& deltaX,
	int& deltaY,
	bool& rightDown,
	bool& rightPressed,
	bool& rightReleased) const
{
	x = ScriptMouseX;
	y = ScriptMouseY;
	deltaX = ScriptMouseDeltaX;
	deltaY = ScriptMouseDeltaY;
	rightDown = bScriptRightMouseDown;
	rightPressed = bScriptRightMousePressed;
	rightReleased = bScriptRightMouseReleased;
}

void Corona::ClearScriptInputFrameState()
{
	ScriptKeyPressed.fill(false);
	ScriptKeyReleased.fill(false);
	bScriptRightMousePressed = false;
	bScriptRightMouseReleased = false;
	ScriptMouseDeltaX = 0;
	ScriptMouseDeltaY = 0;
}

void Corona::QueueScriptUiSeparatorForScript()
{
	ScriptUiCommand command;
	command.Type = ScriptUiCommandType::Separator;
	ScriptUiBuildCommands.push_back(command);
}

void Corona::QueueScriptUiTextForScript(const std::string& text)
{
	ScriptUiCommand command;
	command.Type = ScriptUiCommandType::Text;
	command.Label = text;
	ScriptUiBuildCommands.push_back(command);
}

void Corona::QueueScriptUiSameLineForScript()
{
	ScriptUiCommand command;
	command.Type = ScriptUiCommandType::SameLine;
	ScriptUiBuildCommands.push_back(command);
}

void Corona::QueueScriptUiBeginWindowForScript(const std::string& title)
{
	ScriptUiCommand command;
	command.Type = ScriptUiCommandType::BeginWindow;
	command.Label = title;
	ScriptUiBuildCommands.push_back(command);
}

void Corona::QueueScriptUiEndWindowForScript()
{
	ScriptUiCommand command;
	command.Type = ScriptUiCommandType::EndWindow;
	ScriptUiBuildCommands.push_back(command);
}

void Corona::QueueScriptUiOverlayTextForScript(const std::string& text, float x, float y)
{
	ScriptUiCommand command;
	command.Type = ScriptUiCommandType::OverlayText;
	command.Label = text;
	command.FloatValue = x;
	command.MinValue = y;
	ScriptUiBuildCommands.push_back(command);
}

void Corona::QueueScriptUiWorldAxisForScript(
	const std::string& id,
	const glm::vec3& position,
	float length,
	float thickness)
{
	ScriptUiCommand command;
	command.Type = ScriptUiCommandType::WorldAxis;
	command.Id = id;
	command.Vec3Value = position;
	command.FloatValue = length;
	command.MinValue = thickness;
	ScriptUiBuildCommands.push_back(command);
}

glm::vec3 Corona::QueueScriptUiGizmo3DForScript(
	const std::string& id,
	const std::string& label,
	const glm::vec3& value,
	float size,
	int mode)
{
	glm::vec3 effectiveValue = value;
	{
		std::lock_guard<std::mutex> lock(ScriptUiMutex);
		const auto result = ScriptUiVec3Results.find(id);
		if (result != ScriptUiVec3Results.end())
		{
			effectiveValue = result->second;
			ScriptUiVec3Results.erase(result);
		}
	}

	ScriptUiCommand command;
	command.Type = ScriptUiCommandType::Gizmo3D;
	command.Id = id;
	command.Label = label;
	command.Vec3Value = effectiveValue;
	command.FloatValue = size;
	command.IntValue = mode;
	ScriptUiBuildCommands.push_back(command);
	return effectiveValue;
}

bool Corona::QueueScriptUiButtonForScript(const std::string& id, const std::string& label)
{
	ScriptUiCommand command;
	command.Type = ScriptUiCommandType::Button;
	command.Id = id;
	command.Label = label;
	ScriptUiBuildCommands.push_back(command);

	std::lock_guard<std::mutex> lock(ScriptUiMutex);
	const auto result = ScriptUiClickedResults.find(id);
	if (result == ScriptUiClickedResults.end())
		return false;

	const bool clicked = result->second;
	ScriptUiClickedResults.erase(result);
	return clicked;
}

float Corona::QueueScriptUiSliderFloatForScript(
	const std::string& id,
	const std::string& label,
	float value,
	float minValue,
	float maxValue)
{
	float effectiveValue = value;
	{
		std::lock_guard<std::mutex> lock(ScriptUiMutex);
		const auto result = ScriptUiFloatResults.find(id);
		if (result != ScriptUiFloatResults.end())
		{
			effectiveValue = result->second;
			ScriptUiFloatResults.erase(result);
		}
	}

	ScriptUiCommand command;
	command.Type = ScriptUiCommandType::SliderFloat;
	command.Id = id;
	command.Label = label;
	command.FloatValue = effectiveValue;
	command.MinValue = minValue;
	command.MaxValue = maxValue;
	ScriptUiBuildCommands.push_back(command);
	return effectiveValue;
}

int Corona::QueueScriptUiSliderIntForScript(
	const std::string& id,
	const std::string& label,
	int value,
	int minValue,
	int maxValue)
{
	int effectiveValue = value;
	{
		std::lock_guard<std::mutex> lock(ScriptUiMutex);
		const auto result = ScriptUiIntResults.find(id);
		if (result != ScriptUiIntResults.end())
		{
			effectiveValue = result->second;
			ScriptUiIntResults.erase(result);
		}
	}

	ScriptUiCommand command;
	command.Type = ScriptUiCommandType::SliderInt;
	command.Id = id;
	command.Label = label;
	command.IntValue = std::clamp(effectiveValue, minValue, maxValue);
	command.MinIntValue = minValue;
	command.MaxIntValue = maxValue;
	ScriptUiBuildCommands.push_back(command);
	return command.IntValue;
}

int Corona::QueueScriptUiComboForScript(
	const std::string& id,
	const std::string& label,
	int selectedIndex,
	const std::vector<std::string>& items)
{
	int effectiveIndex = selectedIndex;
	{
		std::lock_guard<std::mutex> lock(ScriptUiMutex);
		const auto result = ScriptUiIntResults.find(id);
		if (result != ScriptUiIntResults.end())
		{
			effectiveIndex = result->second;
			ScriptUiIntResults.erase(result);
		}
	}

	const int maxIndex = std::max(1, static_cast<int>(items.size()));
	ScriptUiCommand command;
	command.Type = ScriptUiCommandType::Combo;
	command.Id = id;
	command.Label = label;
	command.IntValue = std::clamp(effectiveIndex, 1, maxIndex);
	command.Items = items;
	ScriptUiBuildCommands.push_back(command);
	return command.IntValue;
}

bool Corona::QueueScriptUiCheckboxForScript(const std::string& id, const std::string& label, bool value)
{
	bool effectiveValue = value;
	{
		std::lock_guard<std::mutex> lock(ScriptUiMutex);
		const auto result = ScriptUiBoolResults.find(id);
		if (result != ScriptUiBoolResults.end())
		{
			effectiveValue = result->second;
			ScriptUiBoolResults.erase(result);
		}
	}

	ScriptUiCommand command;
	command.Type = ScriptUiCommandType::Checkbox;
	command.Id = id;
	command.Label = label;
	command.BoolValue = effectiveValue;
	ScriptUiBuildCommands.push_back(command);
	return effectiveValue;
}

void Corona::PushLuauUiStateForScript(lua_State* L)
{
	if (bCameraPathListDirty)
		RefreshCameraPathList();

	lua_newtable(L);

	PushIntegerField(L, "fps", static_cast<lua_Integer>(m_timer.GetFramesPerSecond()));
	PushStringField(L, "backend", renderBackend ? renderBackend->GetBackendName() : "None");
	PushBoolField(L, "final_screenshot_busy", bFinalScreenshotRequested || bFinalScreenshotCaptureInFlight);
	PushWideStringField(L, "final_screenshot_status", LastFinalScreenshotStatus);

	PushBoolField(L, "camera_path_recording", bCameraPathRecording);
	PushBoolField(L, "camera_path_playing", bCameraPathPlaying);
	PushBoolField(L, "camera_path_dumping", bCameraPathDumping);
	PushBoolField(L, "camera_path_has_last_dump", !LastCameraPathDumpDir.empty());
	PushIntegerField(L, "camera_path_keyframes", static_cast<lua_Integer>(CameraPathKeyframes.size()));
	PushNumberField(L, "camera_path_duration", GetCameraPathDurationSeconds());
	PushIntegerField(L, "camera_path_dump_frame_index", static_cast<lua_Integer>(CameraPathDumpFrameIndex));
	PushIntegerField(L, "camera_path_dump_frame_count", static_cast<lua_Integer>(CameraPathDumpFrameCount));
	PushIntegerField(L, "camera_path_selected_index", SelectedCameraPathIndex >= 0 ? SelectedCameraPathIndex + 1 : 0);
	PushWideStringField(L, "camera_path_status", LastCameraPathStatus);
	PushWideStringField(L, "camera_path_video_command", LastCameraPathVideoCommand);
	lua_newtable(L);
	for (int entryIndex = 0; entryIndex < static_cast<int>(CameraPathEntries.size()); ++entryIndex)
	{
		lua_pushstring(L, WideToUtf8Local(CameraPathEntries[entryIndex].DisplayName).c_str());
		lua_rawseti(L, -2, entryIndex + 1);
	}
	lua_setfield(L, -2, "camera_path_entries");

	PushIntegerField(L, "gpu_timing_average_frames", static_cast<lua_Integer>(GpuTimingAverageFrameCount));
	PushNumberField(L, "cpu_update_last_ms", CpuUpdateLastTimeMs);
	PushNumberField(L, "cpu_update_average_ms", CpuUpdateAverageTimeMs);
	PushIntegerField(L, "cpu_update_sample_count", static_cast<lua_Integer>(CpuUpdateHistoryMs.size()));
	lua_newtable(L);
	for (UINT phaseIndex = 0; phaseIndex < CpuUpdatePhaseCount; ++phaseIndex)
	{
		lua_newtable(L);
		PushStringField(L, "name", GetCpuUpdatePhaseName(static_cast<ECpuUpdatePhase>(phaseIndex)));
		PushNumberField(L, "last_ms", CpuUpdatePhaseLastTimeMs[phaseIndex]);
		PushNumberField(L, "average_ms", CpuUpdatePhaseAverageTimeMs[phaseIndex]);
		PushIntegerField(L, "sample_count", static_cast<lua_Integer>(CpuUpdatePhaseHistoryMs[phaseIndex].size()));
		lua_rawseti(L, -2, phaseIndex + 1);
	}
	lua_setfield(L, -2, "cpu_update_phases");
	lua_newtable(L);
	int pushedPassIndex = 1;
	for (UINT passIndex = 0; passIndex < GpuPassCount; ++passIndex)
	{
		if (GpuPassLastTimeMs[passIndex] <= 0.0f && GpuPassAverageTimeMs[passIndex] <= 0.0f)
			continue;

		lua_newtable(L);
		PushStringField(L, "name", GetGpuPassName(static_cast<EGpuPass>(passIndex)));
		PushNumberField(L, "last_ms", GpuPassLastTimeMs[passIndex]);
		PushNumberField(L, "average_ms", GpuPassAverageTimeMs[passIndex]);
		PushIntegerField(L, "sample_count", static_cast<lua_Integer>(GpuPassHistoryMs[passIndex].size()));
		lua_rawseti(L, -2, pushedPassIndex++);
	}
	lua_setfield(L, -2, "gpu_passes");

	PushIntegerField(L, "aa_mode", static_cast<lua_Integer>(AntiAliasingMode));
	PushIntegerField(L, "dlss_quality", static_cast<lua_Integer>(DLSSQualityMode));
	PushBoolField(L, "dlss_available", bDLSSAvailable);
	PushBoolField(L, "dlss_rr_available", bDLSSRRAvailable);
	PushIntegerField(L, "render_width", static_cast<lua_Integer>(RenderWidth));
	PushIntegerField(L, "render_height", static_cast<lua_Integer>(RenderHeight));
	PushIntegerField(L, "dlss_jitter_phase_count", static_cast<lua_Integer>(DLSSJitterPhaseCount));
	PushIntegerField(L, "dlss_jitter_phase_count_auto", static_cast<lua_Integer>(DLSSJitterPhaseCountAuto));
	PushIntegerField(L, "dlss_jitter_phase_override", static_cast<lua_Integer>(DLSSJitterPhaseCountOverride));
	PushNumberField(L, "dlss_jitter_phase_scale", DLSSJitterPhaseScale);
	PushNumberField(L, "camera_turn_speed", m_turnSpeed);
	PushBoolField(L, "debug_visualization_available",
		renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::D3D12 && BufferVisualizePSO != nullptr);
	PushBoolField(L, "visualize_buffers", bDebugDraw);
	PushBoolField(L, "draw_histogram", bDrawHistogram);
	PushIntegerField(L, "fullscreen_debug_buffer", static_cast<lua_Integer>(FullscreenDebugBuffer));
	PushIntegerField(L, "rendering_mode", static_cast<lua_Integer>(RenderingMode));

	PushBoolField(L, "enable_direct_diffuse", bEnableDirectDiffuse);
	PushBoolField(L, "enable_direct_specular", bEnableDirectSpecular);
	PushBoolField(L, "enable_specular_gi", bEnableSpecularGI);
	PushBoolField(L, "enable_diffuse_gi", bEnableDiffuseGI);
	PushBoolField(L, "enable_rtao", bEnableRTAO);
	PushBoolField(L, "enable_sky_lighting", bEnableSkyLighting);
	PushBoolField(L, "enable_ray_traced_sky_lighting", bEnableRayTracedSkyLighting);
	PushNumberField(L, "sun_angular_radius", RTShadowViewParam.ShadowLightRadius);
	PushIntegerField(L, "hybrid_shadow_samples", static_cast<lua_Integer>(RTShadowViewParam.ShadowSampleCount));
	PushIntegerField(L, "rtao_samples", static_cast<lua_Integer>(RTAOViewParam.SampleCount));
	PushNumberField(L, "rtao_radius", RTAOViewParam.Radius);
	PushNumberField(L, "rtao_power", RTAOViewParam.Power);
	PushNumberField(L, "rtao_normal_bias", RTAOViewParam.NormalBias);
	PushIntegerField(L, "sky_lighting_samples", static_cast<lua_Integer>(RTSkyLightingViewParam.SampleCount));
	PushNumberField(L, "sky_lighting_ray_length", RTSkyLightingViewParam.RayLength);
	PushNumberField(L, "sky_lighting_strength", SkyLightingStrength);
	PushNumberField(L, "sky_lighting_normal_bias", RTSkyLightingViewParam.NormalBias);
	PushNumberField(L, "sky_lighting_up_bias", RTSkyLightingViewParam.SkyUpBias);
	PushNumberField(L, "sky_lighting_direction_power", RTSkyLightingViewParam.SkyDirectionPower);
	PushNumberField(L, "sky_lighting_min_world_y", RTSkyLightingViewParam.SkyMinWorldY);
	PushIntegerField(L, "sky_lighting_denoise_radius", static_cast<lua_Integer>(SkyLightingDenoiseParam.Radius));
	PushNumberField(L, "surface_bounce_strength", SurfaceBounceStrength);
	PushNumberField(L, "surface_bounce_saturation", SurfaceBounceSaturation);
	PushIntegerField(L, "diffuse_gi_mode", static_cast<lua_Integer>(DiffuseGIMode));
	PushIntegerField(L, "screen_probe_spacing", static_cast<lua_Integer>(ScreenProbeGICB.ProbeSpacing));
	PushIntegerField(L, "screen_probe_gather_radius", static_cast<lua_Integer>(ScreenProbeGICB.GatherRadius));
	PushIntegerField(L, "screen_probe_rays_per_probe", static_cast<lua_Integer>(RTScreenProbeGIViewParam.RaysPerProbe));
	PushIntegerField(L, "screen_probe_sh_mode", ScreenProbeGICB.SHCoefficientCount <= 4u ? 0 : 1);
	PushNumberField(L, "screen_probe_raw_blend", ScreenProbeGICB.RawBlend);
	PushNumberField(L, "screen_probe_resolve_depth", ScreenProbeGICB.ResolveDepthWeight);
	PushNumberField(L, "screen_probe_resolve_normal", ScreenProbeGICB.ResolveNormalWeight);
	PushNumberField(L, "screen_probe_edge_depth", ScreenProbeGICB.EdgeDepthWeight);
	PushNumberField(L, "screen_probe_edge_normal", ScreenProbeGICB.EdgeNormalWeight);
	PushIntegerField(L, "screen_probe_edge_samples", static_cast<lua_Integer>(ScreenProbeGICB.EdgeSampleCount));
	PushNumberField(L, "spatial_hash_cell_size", SpatialHashGICB.CellSize);
	PushIntegerField(L, "spatial_hash_rays_per_cell", static_cast<lua_Integer>(RTSpatialHashGIViewParam.RaysPerCell));
	PushIntegerField(L, "spatial_hash_max_bounces", static_cast<lua_Integer>(RTSpatialHashGIViewParam.MaxBounces));
	PushNumberField(L, "spatial_hash_interpolation", SpatialHashGICB.InterpolationStrength);
	PushNumberField(L, "spatial_hash_smoothing", SpatialHashGICB.SmoothingStrength);
	PushNumberField(L, "spatial_hash_temporal_alpha", SpatialHashGICB.TemporalAlpha);
	PushIntegerField(L, "ray_noise_mode", static_cast<lua_Integer>(RayNoiseMode));

	PushIntegerField(L, "path_max_bounces", static_cast<lua_Integer>(PathTracingViewParam.MaxBounces));
	PushIntegerField(L, "path_samples_per_pixel", static_cast<lua_Integer>(PathTracingViewParam.SamplesPerPixel));
	PushIntegerField(L, "path_direct_light_samples", static_cast<lua_Integer>(PathTracingViewParam.DirectLightSampleCount));
	PushIntegerField(L, "path_debug_mode", static_cast<lua_Integer>(PathTracingViewParam.DebugMode));
	PushIntegerField(L, "frame_counter", static_cast<lua_Integer>(FrameCounter));

	PushIntegerField(L, "tone_map_mode", static_cast<lua_Integer>(ToneMapMode));
	PushNumberField(L, "tonemap_whitepoint_hejl", ToneMapCB.WhitePoint_Hejl);
	PushNumberField(L, "tonemap_shoulder_strength", ToneMapCB.ShoulderStrength);
	PushNumberField(L, "tonemap_linear_strength", ToneMapCB.LinearStrength);
	PushNumberField(L, "tonemap_linear_angle", ToneMapCB.LinearAngle);
	PushNumberField(L, "tonemap_toe_strength", ToneMapCB.ToeStrength);
	PushNumberField(L, "tonemap_whitepoint_hable", ToneMapCB.WhitePoint_Hable);

	PushVec3Field(L, "light_dir", LightDir);
	PushNumberField(L, "light_intensity", LightIntensity);
	PushBoolField(L, "camera_path_owns_light_controls", bCameraPathPlaying || bCameraPathDumping);
	PushIntegerField(L, "point_light_max_count", static_cast<lua_Integer>(MaxPointLights));
	lua_newtable(L);
	for (int pointLightIndex = 0; pointLightIndex < static_cast<int>(PointLights.size()); ++pointLightIndex)
	{
		const PointLightState& pointLight = PointLights[pointLightIndex];
		lua_newtable(L);
		PushIntegerField(L, "id", static_cast<lua_Integer>(pointLight.Id));
		PushIntegerField(L, "index", static_cast<lua_Integer>(pointLightIndex + 1));
		PushBoolField(L, "enabled", pointLight.bEnabled);
		PushVec3Field(L, "position", pointLight.Position);
		PushNumberField(L, "radius", pointLight.Radius);
		PushNumberField(L, "intensity", pointLight.Intensity);
		PushVec3Field(L, "color", pointLight.Color);
		lua_rawseti(L, -2, pointLightIndex + 1);
	}
	lua_setfield(L, -2, "point_lights");
	PushVec3Field(L, "sky_color_top", SkyColorTop);
	PushVec3Field(L, "sky_color_bottom", SkyColorBottom);
	PushNumberField(L, "sky_intensity", SkyIntensity);
	PushBoolField(L, "enable_prefiltered_env_specular", bEnablePrefilteredEnvSpecular);
	PushNumberField(L, "prefiltered_env_roughness_threshold", PrefilteredEnvRoughnessThreshold);
	PushNumberField(L, "prefiltered_env_roughness_fade", PrefilteredEnvRoughnessFade);
	PushStringField(L, "shader_error", renderBackend ? renderBackend->GetErrorString() : "");
}

void Corona::PushPersistentScriptControlForScript(lua_State* L, const std::string& name, int defaultIndex)
{
	const auto result = PersistentScriptControls.find(name);
	if (result != PersistentScriptControls.end())
	{
		const PersistentScriptControlValue& value = result->second;
		switch (value.Type)
		{
		case PersistentScriptControlType::Number:
			lua_pushnumber(L, value.Number);
			return;
		case PersistentScriptControlType::Bool:
			lua_pushboolean(L, value.Bool ? 1 : 0);
			return;
		case PersistentScriptControlType::Vec3:
			PushVec3(L, value.Vec3);
			return;
		default:
			break;
		}
	}

	if (lua_gettop(L) >= defaultIndex)
		lua_pushvalue(L, defaultIndex);
	else
		lua_pushnil(L);
}

bool Corona::SetPersistentScriptControlForScript(const std::string& name, lua_State* L, int valueIndex)
{
	if (name.empty())
		return false;
	for (char c : name)
	{
		if (static_cast<unsigned char>(c) <= 0x20)
			return false;
	}

	PersistentScriptControlValue value;
	if (lua_isboolean(L, valueIndex))
	{
		value.Type = PersistentScriptControlType::Bool;
		value.Bool = lua_toboolean(L, valueIndex) != 0;
	}
	else if (lua_isnumber(L, valueIndex))
	{
		value.Type = PersistentScriptControlType::Number;
		value.Number = static_cast<float>(lua_tonumber(L, valueIndex));
	}
	else
	{
		glm::vec3 vecValue(0.0f);
		if (!ReadVec3At(L, valueIndex, vecValue))
			return false;

		value.Type = PersistentScriptControlType::Vec3;
		value.Vec3 = vecValue;
	}

	bool changed = true;
	const auto existing = PersistentScriptControls.find(name);
	if (existing != PersistentScriptControls.end() && existing->second.Type == value.Type)
	{
		const PersistentScriptControlValue& oldValue = existing->second;
		switch (value.Type)
		{
		case PersistentScriptControlType::Number:
			changed = std::abs(oldValue.Number - value.Number) > 0.0001f;
			break;
		case PersistentScriptControlType::Bool:
			changed = oldValue.Bool != value.Bool;
			break;
		case PersistentScriptControlType::Vec3:
			changed = glm::length(oldValue.Vec3 - value.Vec3) > 0.0001f;
			break;
		default:
			break;
		}
	}

	if (!changed)
		return true;

	PersistentScriptControls[name] = value;
	bPersistentSceneStateDirty = true;
	SaveSceneState();
	return true;
}

bool Corona::SetLuauUiValueForScript(const std::string& name, lua_State* L, int valueIndex)
{
	auto resetTemporal = [&]()
	{
		FrameCounter = 0;
		PrevJitter = glm::vec2(0.0f);
		bTemporalAAHistoryValid = false;
		bTemporalDenoiserHistoryValid = false;
		bResetTemporalStateNextUpdate = true;
	};

	auto resetPathTracing = [&]()
	{
		FrameCounter = 0;
		PrevPathTracingViewMat = glm::mat4x4(0.0f);
		PrevPathTracingLightDir = glm::vec3(0.0f);
		PrevPathTracingLightIntensity = 0.0f;
	};

	auto readFloat = [&]() { return static_cast<float>(luaL_checknumber(L, valueIndex)); };
	auto readInt = [&]() { return static_cast<int>(luaL_checkinteger(L, valueIndex)); };
	auto readBool = [&]() { return lua_toboolean(L, valueIndex) != 0; };
	auto nearlyEqual = [](float a, float b, float epsilon = 0.0001f)
	{
		return std::abs(a - b) <= epsilon;
	};
	auto vecNearlyEqual = [&](const glm::vec3& a, const glm::vec3& b, float epsilon = 0.0001f)
	{
		return glm::length(a - b) <= epsilon;
	};
	bool lastSetterChanged = false;
	auto setFloat = [&](const char* key, float& target, bool resetLighting = false)
	{
		if (name != key)
			return false;
		lastSetterChanged = false;
		const float newValue = readFloat();
		if (nearlyEqual(target, newValue))
			return true;
		target = newValue;
		lastSetterChanged = true;
		if (resetLighting)
			ResetAllAccumulationState(false);
		return true;
	};
	auto setBool = [&](const char* key, bool& target, bool resetLighting = false)
	{
		if (name != key)
			return false;
		lastSetterChanged = false;
		const bool newValue = readBool();
		if (target == newValue)
			return true;
		target = newValue;
		lastSetterChanged = true;
		if (resetLighting)
			ResetAllAccumulationState(false);
		return true;
	};
	auto setUInt = [&](const char* key, UINT32& target, int minValue, int maxValue, bool resetLighting = false)
	{
		if (name != key)
			return false;
		lastSetterChanged = false;
		const UINT32 newValue = static_cast<UINT32>(std::clamp(readInt(), minValue, maxValue));
		if (target == newValue)
			return true;
		target = newValue;
		lastSetterChanged = true;
		if (resetLighting)
			ResetAllAccumulationState(false);
		return true;
	};

	if (name == "aa_mode")
	{
		const EAntiAliasingMode previousMode = AntiAliasingMode;
		EAntiAliasingMode requestedMode = static_cast<EAntiAliasingMode>(
			std::clamp(readInt(), 0, static_cast<int>(EAntiAliasingMode::COUNT) - 1));
		if (requestedMode == EAntiAliasingMode::DLSS_SR && !bDLSSAvailable)
			requestedMode = EAntiAliasingMode::TAA;
		if (requestedMode == EAntiAliasingMode::DLSS_RR && !bDLSSRRAvailable)
			requestedMode = EAntiAliasingMode::TAA;
		if (requestedMode == previousMode)
			return true;
		AntiAliasingMode = requestedMode;
		ResetAllAccumulationState(IsDLSSUiMode(previousMode) || IsDLSSUiMode(requestedMode));
		return true;
	}
	if (name == "dlss_quality")
	{
		const EDLSSQualityMode requestedMode = static_cast<EDLSSQualityMode>(
			std::clamp(readInt(), 0, static_cast<int>(EDLSSQualityMode::COUNT) - 1));
		if (DLSSQualityMode == requestedMode)
			return true;
		DLSSQualityMode = requestedMode;
		ResetAllAccumulationState(true);
		return true;
	}
	if (name == "dlss_jitter_phase_scale")
	{
		const float newValue = std::max(0.25f, readFloat());
		if (nearlyEqual(DLSSJitterPhaseScale, newValue))
			return true;
		DLSSJitterPhaseScale = newValue;
		ResetAllAccumulationState(false);
		return true;
	}
	if (name == "dlss_jitter_phase_override")
	{
		const UINT32 newValue = static_cast<UINT32>(std::clamp(readInt(), 0, 512));
		if (DLSSJitterPhaseCountOverride == newValue)
			return true;
		DLSSJitterPhaseCountOverride = newValue;
		ResetAllAccumulationState(false);
		return true;
	}
	if (name == "gpu_timing_average_frames")
	{
		const UINT32 newValue = static_cast<UINT32>(std::clamp(readInt(), 1, 240));
		if (GpuTimingAverageFrameCount == newValue)
			return true;
		GpuTimingAverageFrameCount = newValue;
		for (UINT passIndex = 0; passIndex < GpuPassCount; ++passIndex)
		{
			auto& history = GpuPassHistoryMs[passIndex];
			while (history.size() > GpuTimingAverageFrameCount)
				history.pop_front();
			float sumMs = 0.0f;
			for (float sampleMs : history)
				sumMs += sampleMs;
			GpuPassAverageTimeMs[passIndex] = history.empty() ? 0.0f : (sumMs / static_cast<float>(history.size()));
		}
		TrimCpuUpdateTimingHistory();
		return true;
	}
	if (name == "rendering_mode")
	{
		const int mode = std::clamp(readInt(), 0, 1);
		const ERenderingMode previousMode = RenderingMode;
		RenderingMode = static_cast<ERenderingMode>(mode);
		if (RenderingMode == previousMode)
			return true;
		if (RenderingMode == ERenderingMode::PATHTRACING)
			resetPathTracing();
		MarkRayTracingSceneDirty();
		return true;
	}
	if (name == "fullscreen_debug_buffer")
	{
		const EDebugVisualization requestedMode =
			static_cast<EDebugVisualization>(std::clamp(readInt(), 0, static_cast<int>(EDebugVisualization::NO_FULLSCREEN)));
		if (FullscreenDebugBuffer == requestedMode)
			return true;
		FullscreenDebugBuffer = requestedMode;
		return true;
	}
	if (name == "diffuse_gi_mode")
	{
		const EDiffuseGIMode requestedMode =
			static_cast<EDiffuseGIMode>(std::clamp(readInt(), 0, static_cast<int>(EDiffuseGIMode::COUNT) - 1));
		if (DiffuseGIMode == requestedMode)
			return true;
		DiffuseGIMode = requestedMode;
		ResetAllAccumulationState(false);
		return true;
	}
	if (name == "ray_noise_mode")
	{
		const ERayNoiseMode requestedMode =
			static_cast<ERayNoiseMode>(std::clamp(readInt(), 0, static_cast<int>(ERayNoiseMode::COUNT) - 1));
		if (RayNoiseMode == requestedMode)
			return true;
		RayNoiseMode = requestedMode;
		ResetAllAccumulationState(false);
		return true;
	}
	if (name == "tone_map_mode")
	{
		const UINT32 requestedMode = static_cast<UINT32>(std::clamp(readInt(), 0, 3));
		if (ToneMapMode == requestedMode)
			return true;
		ToneMapMode = requestedMode;
		return true;
	}
	if (name == "path_debug_mode")
	{
		const UINT32 requestedMode = static_cast<UINT32>(std::clamp(readInt(), 0, 6));
		if (PathTracingViewParam.DebugMode == requestedMode)
			return true;
		PathTracingViewParam.DebugMode = requestedMode;
		resetPathTracing();
		return true;
	}
	if (name == "light_dir")
	{
		glm::vec3 value;
		if (!ReadVec3(L, valueIndex, value))
			return false;
		if (glm::length(value) > 0.0f)
		{
			const glm::vec3 normalized = glm::normalize(value);
			if (!vecNearlyEqual(LightDir, normalized))
				LightDir = normalized;
		}
		return true;
	}
	if (name == "point_light")
	{
		if (!lua_istable(L, valueIndex))
			return false;

		const int tableIndex = lua_absindex(L, valueIndex);
		UINT32 id = 0;
		float idAsFloat = 0.0f;
		if (ReadNumberField(L, tableIndex, "id", idAsFloat))
			id = static_cast<UINT32>(std::max(0.0f, idAsFloat));

		int index = -1;
		float indexAsFloat = 0.0f;
		if (ReadNumberField(L, tableIndex, "index", indexAsFloat))
			index = static_cast<int>(indexAsFloat) - 1;

		PointLightState* pointLight = nullptr;
		if (id != 0)
		{
			for (PointLightState& candidate : PointLights)
			{
				if (candidate.Id == id)
				{
					pointLight = &candidate;
					break;
				}
			}
		}
		if (!pointLight && index >= 0 && index < static_cast<int>(PointLights.size()))
			pointLight = &PointLights[index];
		if (!pointLight)
			return false;

		bool changed = false;
		bool enabled = pointLight->bEnabled;
		if (ReadBoolField(L, tableIndex, "enabled", enabled))
		{
			if (pointLight->bEnabled != enabled)
			{
				pointLight->bEnabled = enabled;
				changed = true;
			}
		}

		glm::vec3 position = pointLight->Position;
		if (ReadVec3Field(L, tableIndex, "position", position))
		{
			if (!vecNearlyEqual(pointLight->Position, position))
			{
				pointLight->Position = position;
				changed = true;
			}
		}

		glm::vec3 color = pointLight->Color;
		if (ReadVec3Field(L, tableIndex, "color", color))
		{
			const glm::vec3 newColor = glm::max(color, glm::vec3(0.0f));
			if (!vecNearlyEqual(pointLight->Color, newColor))
			{
				pointLight->Color = newColor;
				changed = true;
			}
		}

		float radius = pointLight->Radius;
		if (ReadNumberField(L, tableIndex, "radius", radius))
		{
			const float newRadius = std::clamp(radius, 1.0f, 100000.0f);
			if (!nearlyEqual(pointLight->Radius, newRadius))
			{
				pointLight->Radius = newRadius;
				changed = true;
			}
		}

		float intensity = pointLight->Intensity;
		if (ReadNumberField(L, tableIndex, "intensity", intensity))
		{
			const float newIntensity = std::max(0.0f, intensity);
			if (!nearlyEqual(pointLight->Intensity, newIntensity))
			{
				pointLight->Intensity = newIntensity;
				changed = true;
			}
		}

		if (changed)
		{
			MarkPointLightRenderDirty(pointLight->Id, kPointLightDirtyAll);
			ResetAllAccumulationState(false);
			bPersistentSceneStateDirty = true;
			SaveSceneState();
		}
		return true;
	}
	if (name == "sky_color_top")
	{
		glm::vec3 value;
		if (!ReadVec3(L, valueIndex, value))
			return false;
		if (!vecNearlyEqual(SkyColorTop, value))
			SkyColorTop = value;
		return true;
	}
	if (name == "sky_color_bottom")
	{
		glm::vec3 value;
		if (!ReadVec3(L, valueIndex, value))
			return false;
		if (!vecNearlyEqual(SkyColorBottom, value))
			SkyColorBottom = value;
		return true;
	}

	if (setFloat("camera_turn_speed", m_turnSpeed)) return true;
	if (setBool("visualize_buffers", bDebugDraw)) return true;
	if (setBool("draw_histogram", bDrawHistogram)) return true;
	if (setBool("enable_direct_diffuse", bEnableDirectDiffuse, true)) return true;
	if (setBool("enable_direct_specular", bEnableDirectSpecular, true)) return true;
	if (setBool("enable_specular_gi", bEnableSpecularGI, true)) return true;
	if (setBool("enable_diffuse_gi", bEnableDiffuseGI, true)) return true;
	if (setBool("enable_rtao", bEnableRTAO, true)) return true;
	if (setBool("enable_sky_lighting", bEnableSkyLighting, true)) return true;
	if (setBool("enable_ray_traced_sky_lighting", bEnableRayTracedSkyLighting, true)) return true;
	if (setFloat("sun_angular_radius", RTShadowViewParam.ShadowLightRadius, true)) return true;
	if (setUInt("hybrid_shadow_samples", RTShadowViewParam.ShadowSampleCount, 1, 16, true)) return true;
	if (setUInt("rtao_samples", RTAOViewParam.SampleCount, 1, 16, true)) return true;
	if (setFloat("rtao_radius", RTAOViewParam.Radius, true)) return true;
	if (setFloat("rtao_power", RTAOViewParam.Power, true)) return true;
	if (setFloat("rtao_normal_bias", RTAOViewParam.NormalBias, true)) return true;
	if (setUInt("sky_lighting_samples", RTSkyLightingViewParam.SampleCount, 1, 32, true)) return true;
	if (setFloat("sky_lighting_ray_length", RTSkyLightingViewParam.RayLength, true)) return true;
	if (setFloat("sky_lighting_strength", SkyLightingStrength, true)) return true;
	if (setFloat("sky_lighting_normal_bias", RTSkyLightingViewParam.NormalBias, true)) return true;
	if (setFloat("sky_lighting_up_bias", RTSkyLightingViewParam.SkyUpBias, true)) return true;
	if (setFloat("sky_lighting_direction_power", RTSkyLightingViewParam.SkyDirectionPower, true)) return true;
	if (setFloat("sky_lighting_min_world_y", RTSkyLightingViewParam.SkyMinWorldY, true)) return true;
	if (setUInt("sky_lighting_denoise_radius", SkyLightingDenoiseParam.Radius, 1, 6, true)) return true;
	if (setFloat("surface_bounce_strength", SurfaceBounceStrength, true)) return true;
	if (setFloat("surface_bounce_saturation", SurfaceBounceSaturation, true)) return true;
	if (setUInt("screen_probe_spacing", ScreenProbeGICB.ProbeSpacing, 4, 64, true)) return true;
	if (setUInt("screen_probe_gather_radius", ScreenProbeGICB.GatherRadius, 1, 3, true)) return true;
	if (setUInt("screen_probe_rays_per_probe", RTScreenProbeGIViewParam.RaysPerProbe, 1, 4, true)) return true;
	if (name == "screen_probe_sh_mode")
	{
		const UINT32 coefficientCount = readInt() == 0 ? 4u : 9u;
		if (ScreenProbeGICB.SHCoefficientCount == coefficientCount)
			return true;
		ScreenProbeGICB.SHCoefficientCount = coefficientCount;
		ResetAllAccumulationState(false);
		return true;
	}
	if (setFloat("screen_probe_raw_blend", ScreenProbeGICB.RawBlend, true)) return true;
	if (setFloat("screen_probe_resolve_depth", ScreenProbeGICB.ResolveDepthWeight, true)) return true;
	if (setFloat("screen_probe_resolve_normal", ScreenProbeGICB.ResolveNormalWeight, true)) return true;
	if (setFloat("screen_probe_edge_depth", ScreenProbeGICB.EdgeDepthWeight, true)) return true;
	if (setFloat("screen_probe_edge_normal", ScreenProbeGICB.EdgeNormalWeight, true)) return true;
	if (setUInt("screen_probe_edge_samples", ScreenProbeGICB.EdgeSampleCount, 1, 4, true)) return true;
	if (setFloat("spatial_hash_cell_size", SpatialHashGICB.CellSize, true)) return true;
	if (setUInt("spatial_hash_rays_per_cell", RTSpatialHashGIViewParam.RaysPerCell, 1, 8, true)) return true;
	if (setUInt("spatial_hash_max_bounces", RTSpatialHashGIViewParam.MaxBounces, 1, 8, true)) return true;
	if (setFloat("spatial_hash_interpolation", SpatialHashGICB.InterpolationStrength, true)) return true;
	if (setFloat("spatial_hash_smoothing", SpatialHashGICB.SmoothingStrength, true)) return true;
	if (setFloat("spatial_hash_temporal_alpha", SpatialHashGICB.TemporalAlpha, true)) return true;
	if (setUInt("path_max_bounces", PathTracingViewParam.MaxBounces, 1, 8)) { if (lastSetterChanged) resetPathTracing(); return true; }
	if (setUInt("path_samples_per_pixel", PathTracingViewParam.SamplesPerPixel, 1, 16)) { if (lastSetterChanged) resetPathTracing(); return true; }
	if (setUInt("path_direct_light_samples", PathTracingViewParam.DirectLightSampleCount, 1, 8, true)) return true;
	if (setFloat("tonemap_whitepoint_hejl", ToneMapCB.WhitePoint_Hejl)) return true;
	if (setFloat("tonemap_shoulder_strength", ToneMapCB.ShoulderStrength)) return true;
	if (setFloat("tonemap_linear_strength", ToneMapCB.LinearStrength)) return true;
	if (setFloat("tonemap_linear_angle", ToneMapCB.LinearAngle)) return true;
	if (setFloat("tonemap_toe_strength", ToneMapCB.ToeStrength)) return true;
	if (setFloat("tonemap_whitepoint_hable", ToneMapCB.WhitePoint_Hable)) return true;
	if (setFloat("light_intensity", LightIntensity)) return true;
	if (setFloat("sky_intensity", SkyIntensity, true)) return true;
	if (setBool("enable_prefiltered_env_specular", bEnablePrefilteredEnvSpecular, true)) return true;
	if (setFloat("prefiltered_env_roughness_threshold", PrefilteredEnvRoughnessThreshold, true)) return true;
	if (setFloat("prefiltered_env_roughness_fade", PrefilteredEnvRoughnessFade, true)) return true;

	return false;
}

bool Corona::RunLuauUiCommandForScript(const std::string& name, lua_State* L, int argIndex)
{
	if (name == "capture_final_backbuffer")
	{
		if (bFinalScreenshotRequested || bFinalScreenshotCaptureInFlight)
			return false;
		bFinalScreenshotRequested = true;
		LastFinalScreenshotStatus = L"Screenshot will be captured on the next frame without ImGui.";
		return true;
	}
	if (name == "start_camera_path")
	{
		StartCameraPathRecording();
		return true;
	}
	if (name == "end_camera_path")
	{
		EndCameraPathRecording();
		return true;
	}
	if (name == "refresh_camera_paths")
	{
		RefreshCameraPathList();
		LastCameraPathStatus = L"Found " + std::to_wstring(static_cast<unsigned long long>(CameraPathEntries.size())) + L" saved camera path(s).";
		return true;
	}
	if (name == "select_camera_path")
	{
		const int selectedIndex = static_cast<int>(luaL_checkinteger(L, argIndex)) - 1;
		SelectedCameraPathIndex = selectedIndex >= 0 && selectedIndex < static_cast<int>(CameraPathEntries.size()) ? selectedIndex : -1;
		return true;
	}
	if (name == "play_camera_path")
	{
		if (bCameraPathPlaying)
			StopCameraPathPlayback();
		else
			StartCameraPathPlayback();
		return true;
	}
	if (name == "load_selected_path")
		return LoadSelectedCameraPath();
	if (name == "load_latest_path")
		return LoadLatestCameraPath();
	if (name == "play_selected_path")
	{
		if (LoadSelectedCameraPath())
		{
			StartCameraPathPlayback();
			return true;
		}
		return false;
	}
	if (name == "dump_selected_path")
	{
		if (LoadSelectedCameraPath())
		{
			StartCameraPathDump();
			return true;
		}
		return false;
	}
	if (name == "start_camera_path_dump")
	{
		StartCameraPathDump();
		return true;
	}
	if (name == "stop_playback_dump")
	{
		StopCameraPathPlayback();
		return true;
	}
	if (name == "convert_last_dump_to_mp4")
	{
		LaunchCameraPathVideoEncode();
		return true;
	}
	if (name == "recompile_shaders")
	{
		bRecompileShaders = true;
		return true;
	}
	if (name == "reset_accumulation")
	{
		FrameCounter = 0;
		PrevPathTracingViewMat = glm::mat4x4(0.0f);
		PrevPathTracingLightDir = glm::vec3(0.0f);
		PrevPathTracingLightIntensity = 0.0f;
		ResetAllAccumulationState(false);
		return true;
	}
	if (name == "clear_shader_error")
	{
		if (renderBackend)
			renderBackend->ClearErrorString();
		return true;
	}
	if (name == "add_point_light")
	{
		if (PointLights.size() >= MaxPointLights)
			return false;

		PointLightState pointLight;
		pointLight.Id = NextPointLightId++;
		pointLight.bEnabled = true;
		const glm::vec3 forward =
			glm::length(m_camera.m_lookDirection) > 0.0f ?
			glm::normalize(m_camera.m_lookDirection) :
			glm::vec3(0.0f, 0.0f, -1.0f);
		pointLight.Position = m_camera.m_position + forward * 260.0f + glm::vec3(0.0f, 80.0f, 0.0f);
		pointLight.Radius = 420.0f;
		pointLight.Intensity = 16.0f;
		pointLight.Color = glm::vec3(1.0f, 0.92f, 0.78f);
		PointLights.push_back(pointLight);
		MarkPointLightRenderDirty(pointLight.Id, kPointLightDirtyAll);
		ResetAllAccumulationState(false);
		bPersistentSceneStateDirty = true;
		SaveSceneState();
		return true;
	}
	if (name == "remove_point_light")
	{
		if (PointLights.empty())
			return false;

		UINT32 id = 0;
		if (lua_gettop(L) >= argIndex && lua_isnumber(L, argIndex))
			id = static_cast<UINT32>(std::max<lua_Integer>(0, lua_tointeger(L, argIndex)));

		if (id == 0)
		{
			MarkPointLightRenderRemoved(PointLights.back().Id);
			PointLights.pop_back();
			ResetAllAccumulationState(false);
			bPersistentSceneStateDirty = true;
			SaveSceneState();
			return true;
		}

		const auto it = std::find_if(PointLights.begin(), PointLights.end(), [id](const PointLightState& pointLight)
		{
			return pointLight.Id == id;
		});
		if (it == PointLights.end())
			return false;

		MarkPointLightRenderRemoved(it->Id);
		PointLights.erase(it);
		ResetAllAccumulationState(false);
		bPersistentSceneStateDirty = true;
		SaveSceneState();
		return true;
	}
	if (name == "clear_point_lights")
	{
		if (PointLights.empty())
			return false;
		for (const PointLightState& pointLight : PointLights)
			MarkPointLightRenderRemoved(pointLight.Id);
		PointLights.clear();
		ResetAllAccumulationState(false);
		bPersistentSceneStateDirty = true;
		SaveSceneState();
		return true;
	}

	return false;
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
	RegisterImGuiApi(L);
	RegisterQueuedUiApi(L);
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

	std::vector<std::filesystem::path> scriptPaths;
	const std::filesystem::path startupDir = GetAssetFullPath(L"scripts\\startup");
	std::error_code ec;
	if (std::filesystem::is_directory(startupDir, ec))
	{
		for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(startupDir, ec))
		{
			if (ec)
				break;
			if (entry.is_regular_file(ec) && entry.path().extension() == L".luau")
				scriptPaths.push_back(entry.path());
		}
	}

	std::sort(scriptPaths.begin(), scriptPaths.end(), [](const std::filesystem::path& a, const std::filesystem::path& b)
	{
		return _wcsicmp(a.filename().c_str(), b.filename().c_str()) < 0;
	});

	if (scriptPaths.empty())
	{
		const std::filesystem::path legacyScriptPath = GetAssetFullPath(L"scripts\\startup_pistol_spin.luau");
		if (std::filesystem::exists(legacyScriptPath, ec))
			scriptPaths.push_back(legacyScriptPath);
	}

	AppendCpuRuntimeTrace(L"[Luau] startup script count=" + std::to_wstring(scriptPaths.size()));
	for (size_t scriptIndex = 0; scriptIndex < scriptPaths.size(); ++scriptIndex)
	{
		const std::filesystem::path& scriptPath = scriptPaths[scriptIndex];
		const float scriptProgress =
			0.95f + 0.025f * (scriptPaths.empty() ? 1.0f : static_cast<float>(scriptIndex) / static_cast<float>(scriptPaths.size()));
		UpdateStartupLoadingProgress(scriptProgress, L"Running startup script: " + scriptPath.filename().wstring());
		LoadLuauScriptFile(scriptPath);
	}
}

bool Corona::LoadLuauScriptFile(const std::filesystem::path& scriptPath)
{
	if (!ScriptState || !ScriptState->L)
		InitLuauScripting();
	if (!ScriptState || !ScriptState->L)
		return false;

	const std::string source = ReadTextFileUtf8(scriptPath);
	if (source.empty())
	{
		AppendCpuRuntimeTrace(L"[Luau] script missing or empty: " + scriptPath.wstring());
		return false;
	}

	lua_State* L = ScriptState->L;
	ClearLegacyScriptGlobals(L);

	size_t bytecodeSize = 0;
	char* bytecode = luau_compile(source.data(), source.size(), nullptr, &bytecodeSize);
	if (!bytecode)
	{
		AppendCpuRuntimeTrace(L"[Luau] compile failed: " + scriptPath.wstring());
		return false;
	}

	const std::string chunkName = "=" + WideToUtf8Local(scriptPath.wstring());
	const int loadResult = luau_load(L, chunkName.c_str(), bytecode, bytecodeSize, 0);
	std::free(bytecode);
	if (loadResult != 0)
	{
		AppendCpuRuntimeTrace(L"[Luau] load failed: " + Utf8ToWideLocal(LuaToString(L, -1)));
		lua_pop(L, 1);
		return false;
	}

	const int callResult = lua_pcall(L, 0, 1, 0);
	if (callResult != 0)
	{
		AppendCpuRuntimeTrace(L"[Luau] startup script error: " + Utf8ToWideLocal(LuaToString(L, -1)));
		lua_pop(L, 1);
		return false;
	}

	LuauScriptState::LoadedScript loadedScript;
	loadedScript.Path = scriptPath.wstring();

	if (lua_istable(L, -1))
	{
		const int tableIndex = lua_gettop(L);
		loadedScript.UpdateRef = RefTableFunction(L, tableIndex, "update");
		loadedScript.ShutdownRef = RefTableFunction(L, tableIndex, "shutdown");
		loadedScript.ImGuiRef = RefTableFunction(L, tableIndex, "imgui");
		loadedScript.UiRef = RefTableFunction(L, tableIndex, "ui");
	}
	lua_pop(L, 1);

	if (loadedScript.UpdateRef == LUA_REFNIL && loadedScript.ShutdownRef == LUA_REFNIL && loadedScript.ImGuiRef == LUA_REFNIL && loadedScript.UiRef == LUA_REFNIL)
	{
		loadedScript.UpdateRef = RefGlobalFunctionAndClear(L, "update");
		loadedScript.ShutdownRef = RefGlobalFunctionAndClear(L, "shutdown");
	}
	else
	{
		ClearLegacyScriptGlobals(L);
	}

	if (loadedScript.UpdateRef != LUA_REFNIL || loadedScript.ShutdownRef != LUA_REFNIL || loadedScript.ImGuiRef != LUA_REFNIL || loadedScript.UiRef != LUA_REFNIL)
	{
		const bool hasUpdate = loadedScript.UpdateRef != LUA_REFNIL;
		const bool hasShutdown = loadedScript.ShutdownRef != LUA_REFNIL;
		const bool hasImGui = loadedScript.ImGuiRef != LUA_REFNIL;
		const bool hasUi = loadedScript.UiRef != LUA_REFNIL;
		ScriptState->Scripts.push_back(loadedScript);
		AppendCpuRuntimeTrace(
			L"[Luau] script registered update=" + std::to_wstring(hasUpdate ? 1 : 0) +
			L", shutdown=" + std::to_wstring(hasShutdown ? 1 : 0) +
			L", imgui=" + std::to_wstring(hasImGui ? 1 : 0) +
			L", ui=" + std::to_wstring(hasUi ? 1 : 0) +
			L", path=" + scriptPath.wstring());
	}
	else
	{
		AppendCpuRuntimeTrace(L"[Luau] script executed without callbacks: " + scriptPath.wstring());
	}

	return true;
}

void Corona::CallLuauShutdownCallbacks()
{
	if (!ScriptState || !ScriptState->L)
		return;

	lua_State* L = ScriptState->L;
	for (auto it = ScriptState->Scripts.rbegin(); it != ScriptState->Scripts.rend(); ++it)
	{
		if (it->ShutdownRef == LUA_REFNIL)
			continue;

		lua_getref(L, it->ShutdownRef);
		const int result = lua_pcall(L, 0, 0, 0);
		if (result != 0)
		{
			AppendCpuRuntimeTrace(L"[Luau] shutdown error in " + it->Path + L": " + Utf8ToWideLocal(LuaToString(L, -1)));
			lua_pop(L, 1);
		}

		lua_unref(L, it->ShutdownRef);
		it->ShutdownRef = LUA_REFNIL;
	}
}

void Corona::ReloadLuauScripting()
{
	AppendCpuRuntimeTrace(L"[Luau] reload requested");

	CallLuauShutdownCallbacks();

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
	if (!ScriptState || !ScriptState->L || ScriptState->Scripts.empty())
		return;

	BuildLuauUi();

	lua_State* L = ScriptState->L;
	for (LuauScriptState::LoadedScript& script : ScriptState->Scripts)
	{
		if (script.UpdateRef == LUA_REFNIL)
			continue;

		const glm::vec3 cameraPositionBeforeScript = m_camera.m_position;
		lua_getref(L, script.UpdateRef);
		lua_pushnumber(L, static_cast<double>(dt));
		const int result = lua_pcall(L, 1, 0, 0);
		if (result != 0)
		{
			AppendCpuRuntimeTrace(L"[Luau] update error in " + script.Path + L": " + Utf8ToWideLocal(LuaToString(L, -1)));
			lua_pop(L, 1);
			lua_unref(L, script.UpdateRef);
			script.UpdateRef = LUA_REFNIL;
		}

		const bool bScriptMovedCamera = glm::length(m_camera.m_position - cameraPositionBeforeScript) > 0.0001f;
		if (bScriptCameraControlEnabled && bScriptMovedCamera)
			m_camera.m_position = ResolveCameraPhysicsMovement(cameraPositionBeforeScript, m_camera.m_position);
	}
}

void Corona::BuildLuauUi()
{
	if (!ScriptState || !ScriptState->L || ScriptState->Scripts.empty())
	{
		std::lock_guard<std::mutex> lock(ScriptUiMutex);
		ScriptUiRenderCommands.clear();
		return;
	}

	ScriptUiBuildCommands.clear();

	lua_State* L = ScriptState->L;
	for (LuauScriptState::LoadedScript& script : ScriptState->Scripts)
	{
		if (script.UiRef == LUA_REFNIL)
			continue;

		lua_getref(L, script.UiRef);
		const int result = lua_pcall(L, 0, 0, 0);
		if (result != 0)
		{
			AppendCpuRuntimeTrace(L"[Luau] ui error in " + script.Path + L": " + Utf8ToWideLocal(LuaToString(L, -1)));
			lua_pop(L, 1);
			lua_unref(L, script.UiRef);
			script.UiRef = LUA_REFNIL;
		}
	}

	std::lock_guard<std::mutex> lock(ScriptUiMutex);
	ScriptUiRenderCommands = ScriptUiBuildCommands;
}

void Corona::RenderQueuedLuauUi()
{
	std::vector<ScriptUiCommand> commands;
	{
		std::lock_guard<std::mutex> lock(ScriptUiMutex);
		commands = ScriptUiRenderCommands;
	}

	std::map<std::string, bool> clickedResults;
	std::map<std::string, float> floatResults;
	std::map<std::string, int> intResults;
	std::map<std::string, bool> boolResults;
	std::map<std::string, glm::vec3> vec3Results;
	int queuedWindowDepth = 0;
	auto makeImGuiLabel = [](const ScriptUiCommand& command)
	{
		return command.Id.empty() ? command.Label : command.Label + "##" + command.Id;
	};
	for (const ScriptUiCommand& command : commands)
	{
		switch (command.Type)
		{
		case ScriptUiCommandType::Separator:
			ImGui::Separator();
			break;
		case ScriptUiCommandType::Text:
			ImGui::TextUnformatted(command.Label.c_str());
			break;
		case ScriptUiCommandType::SameLine:
			ImGui::SameLine();
			break;
		case ScriptUiCommandType::BeginWindow:
			ImGui::Begin(command.Label.c_str());
			++queuedWindowDepth;
			break;
		case ScriptUiCommandType::EndWindow:
			if (queuedWindowDepth > 0)
			{
				ImGui::End();
				--queuedWindowDepth;
			}
			break;
		case ScriptUiCommandType::OverlayText:
		{
			ImDrawList* foregroundDrawList = ImGui::GetForegroundDrawList();
			const ImVec2 pos(command.FloatValue, command.MinValue);
			foregroundDrawList->AddText(
				ImVec2(pos.x + 1.0f, pos.y + 1.0f),
				IM_COL32(0, 0, 0, 180),
				command.Label.c_str());
			foregroundDrawList->AddText(
				pos,
				IM_COL32(255, 255, 255, 235),
				command.Label.c_str());
			break;
		}
		case ScriptUiCommandType::WorldAxis:
		{
			ImGuiIO& io = ImGui::GetIO();
			const float displayWidth = io.DisplaySize.x > 0.0f ? io.DisplaySize.x : static_cast<float>(m_width);
			const float displayHeight = io.DisplaySize.y > 0.0f ? io.DisplaySize.y : static_cast<float>(m_height);
			if (displayWidth <= 0.0f || displayHeight <= 0.0f)
				break;

			auto projectWorldToScreen = [&](const glm::vec3& worldPosition, ImVec2& screenPosition)
			{
				glm::vec4 clipPosition = glm::vec4(worldPosition, 1.0f) * glm::transpose(UnjitteredViewProjMat);
				if (clipPosition.w <= 0.0001f)
					return false;

				const float invW = 1.0f / clipPosition.w;
				const float ndcX = clipPosition.x * invW;
				const float ndcY = clipPosition.y * invW;
				if (ndcX < -4.0f || ndcX > 4.0f || ndcY < -4.0f || ndcY > 4.0f)
					return false;

				screenPosition.x = (ndcX * 0.5f + 0.5f) * displayWidth;
				screenPosition.y = (-ndcY * 0.5f + 0.5f) * displayHeight;
				return true;
			};

			const glm::vec3 origin = command.Vec3Value;
			const float axisLength = std::max(command.FloatValue, 1.0f);
			const float thickness = std::clamp(command.MinValue, 1.0f, 8.0f);
			ImVec2 originScreen;
			if (!projectWorldToScreen(origin, originScreen))
				break;

			ImDrawList* foregroundDrawList = ImGui::GetForegroundDrawList();
			foregroundDrawList->AddCircleFilled(originScreen, 4.0f, IM_COL32(255, 255, 255, 220));
			foregroundDrawList->AddCircle(originScreen, 4.0f, IM_COL32(0, 0, 0, 190), 12, 1.0f);

			const glm::vec3 axisEnds[3] = {
				origin + glm::vec3(axisLength, 0.0f, 0.0f),
				origin + glm::vec3(0.0f, axisLength, 0.0f),
				origin + glm::vec3(0.0f, 0.0f, axisLength),
			};
			const ImU32 axisColors[3] = {
				IM_COL32(255, 68, 68, 235),
				IM_COL32(76, 220, 86, 235),
				IM_COL32(90, 150, 255, 235),
			};
			const char* axisLabels[3] = { "X", "Y", "Z" };

			for (int axisIndex = 0; axisIndex < 3; ++axisIndex)
			{
				ImVec2 endScreen;
				if (!projectWorldToScreen(axisEnds[axisIndex], endScreen))
					continue;

				foregroundDrawList->AddLine(originScreen, endScreen, IM_COL32(0, 0, 0, 180), thickness + 2.0f);
				foregroundDrawList->AddLine(originScreen, endScreen, axisColors[axisIndex], thickness);
				foregroundDrawList->AddText(
					ImVec2(endScreen.x + 1.0f, endScreen.y + 1.0f),
					IM_COL32(0, 0, 0, 190),
					axisLabels[axisIndex]);
				foregroundDrawList->AddText(endScreen, axisColors[axisIndex], axisLabels[axisIndex]);
			}
			break;
		}
		case ScriptUiCommandType::Gizmo3D:
		{
			glm::vec3 value = command.Vec3Value;
			const std::string imguiLabel = makeImGuiLabel(command);
			if (ImGui::gizmo3D(imguiLabel.c_str(), value, command.FloatValue, command.IntValue))
				vec3Results[command.Id] = value;
			break;
		}
		case ScriptUiCommandType::Button:
		{
			const std::string imguiLabel = makeImGuiLabel(command);
			if (ImGui::Button(imguiLabel.c_str()))
				clickedResults[command.Id] = true;
			break;
		}
		case ScriptUiCommandType::SliderFloat:
		{
			float value = command.FloatValue;
			const std::string imguiLabel = makeImGuiLabel(command);
			if (ImGui::SliderFloat(imguiLabel.c_str(), &value, command.MinValue, command.MaxValue))
				floatResults[command.Id] = value;
			break;
		}
		case ScriptUiCommandType::SliderInt:
		{
			int value = command.IntValue;
			const std::string imguiLabel = makeImGuiLabel(command);
			if (ImGui::SliderInt(imguiLabel.c_str(), &value, command.MinIntValue, command.MaxIntValue))
				intResults[command.Id] = value;
			break;
		}
		case ScriptUiCommandType::Combo:
		{
			if (command.Items.empty())
				break;

			std::vector<const char*> itemPointers;
			itemPointers.reserve(command.Items.size());
			for (const std::string& item : command.Items)
				itemPointers.push_back(item.c_str());

			int itemIndex = std::clamp(command.IntValue - 1, 0, static_cast<int>(command.Items.size()) - 1);
			const std::string imguiLabel = makeImGuiLabel(command);
			if (ImGui::Combo(imguiLabel.c_str(), &itemIndex, itemPointers.data(), static_cast<int>(itemPointers.size())))
				intResults[command.Id] = itemIndex + 1;
			break;
		}
		case ScriptUiCommandType::Checkbox:
		{
			bool value = command.BoolValue;
			const std::string imguiLabel = makeImGuiLabel(command);
			if (ImGui::Checkbox(imguiLabel.c_str(), &value))
				boolResults[command.Id] = value;
			break;
		}
		default:
			break;
		}
	}
	while (queuedWindowDepth > 0)
	{
		ImGui::End();
		--queuedWindowDepth;
	}

	{
		std::lock_guard<std::mutex> lock(ScriptUiMutex);
		for (const auto& result : clickedResults)
			ScriptUiClickedResults[result.first] = result.second;
		for (const auto& result : floatResults)
			ScriptUiFloatResults[result.first] = result.second;
		for (const auto& result : intResults)
			ScriptUiIntResults[result.first] = result.second;
		for (const auto& result : boolResults)
			ScriptUiBoolResults[result.first] = result.second;
		for (const auto& result : vec3Results)
			ScriptUiVec3Results[result.first] = result.second;
	}
}

void Corona::DrawLuauImGui()
{
	if (!ScriptState || !ScriptState->L || ScriptState->Scripts.empty())
		return;

	lua_State* L = ScriptState->L;
	const bool previousFrameActive = bLuauImGuiFrameActive;
	bLuauImGuiFrameActive = true;

	for (LuauScriptState::LoadedScript& script : ScriptState->Scripts)
	{
		if (script.ImGuiRef == LUA_REFNIL)
			continue;

		lua_getref(L, script.ImGuiRef);
		const int result = lua_pcall(L, 0, 0, 0);
		if (result != 0)
		{
			AppendCpuRuntimeTrace(L"[Luau] imgui error in " + script.Path + L": " + Utf8ToWideLocal(LuaToString(L, -1)));
			lua_pop(L, 1);
			lua_unref(L, script.ImGuiRef);
			script.ImGuiRef = LUA_REFNIL;
		}
	}

	bLuauImGuiFrameActive = previousFrameActive;
}

bool Corona::IsLuauImGuiFrameActive() const
{
	return bLuauImGuiFrameActive;
}

void Corona::ShutdownLuauScripting()
{
	if (!ScriptState)
		return;

	if (ScriptState->L)
	{
		CallLuauShutdownCallbacks();
		for (LuauScriptState::LoadedScript& script : ScriptState->Scripts)
		{
			if (script.UpdateRef != LUA_REFNIL)
			{
				lua_unref(ScriptState->L, script.UpdateRef);
				script.UpdateRef = LUA_REFNIL;
			}
			if (script.ShutdownRef != LUA_REFNIL)
			{
				lua_unref(ScriptState->L, script.ShutdownRef);
				script.ShutdownRef = LUA_REFNIL;
			}
			if (script.ImGuiRef != LUA_REFNIL)
			{
				lua_unref(ScriptState->L, script.ImGuiRef);
				script.ImGuiRef = LUA_REFNIL;
			}
			if (script.UiRef != LUA_REFNIL)
			{
				lua_unref(ScriptState->L, script.UiRef);
				script.UiRef = LUA_REFNIL;
			}
		}
		ScriptState->Scripts.clear();
		lua_close(ScriptState->L);
		ScriptState->L = nullptr;
	}
	ScriptState.reset();
	ScriptScenes.clear();
	ScriptSceneByPath.clear();
	ScriptObjects.clear();
	ScriptUiBuildCommands.clear();
	ScriptUiRenderCommands.clear();
	ScriptUiClickedResults.clear();
	ScriptUiFloatResults.clear();
	ScriptUiIntResults.clear();
	ScriptUiBoolResults.clear();
	ScriptUiVec3Results.clear();
	bScriptCameraControlEnabled = false;
	ScriptKeyDown.fill(false);
	bScriptRightMouseDown = false;
	bScriptMousePositionInitialized = false;
	ClearScriptInputFrameState();
}
