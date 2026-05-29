//*********************************************************
//
// Luau scripting bridge for ECS components and engine controls.
//
//*********************************************************

#include "stdafx.h"
#include "Corona.h"
#include "Corona.Console.h"
#include "PlatformSystem.h"
#include "TerrainComponent.h"
#include "PlatformWindow.h"
#include "Utils.h"

#include "imgui.h"
#include "imGuIZMO.h"

#include "lua.h"
#include "lualib.h"
#include "luacode.h"

#include <algorithm>
#include <cctype>
#include <codecvt>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <locale>
#include <sstream>
#include <thread>
#include <utility>

void AppendCpuRuntimeTrace(const std::wstring& line);

namespace
{
	const char* kCoronaRegistryKey = "Corona.ScriptHost";

	int RefStackValueAndPop(lua_State* L);
	int RefTableFunction(lua_State* L, int tableIndex, const char* fieldName);
	void PushBoolField(lua_State* L, const char* name, bool value);
	void PushNumberField(lua_State* L, const char* name, double value);
	void PushIntegerField(lua_State* L, const char* name, lua_Integer value);
	void PushVec3Field(lua_State* L, const char* name, const glm::vec3& value);
	void LuauScriptProfileInterrupt(lua_State* L, int gcState);

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

	std::wstring ShortScriptSourceName(const std::wstring& sourceName)
	{
		if (sourceName.empty())
			return L"<script>";

		std::filesystem::path path(sourceName);
		if (path.has_filename())
			return path.filename().wstring();
		return sourceName;
	}

	std::wstring LuauSourceToWide(const char* source)
	{
		if (!source || source[0] == '\0')
			return L"<script>";

		const char* normalizedSource = source[0] == '=' ? source + 1 : source;
		return Utf8ToWideLocal(normalizedSource);
	}

	std::string MakeScriptFunctionDisplayName(
		const std::string& functionName,
		int lineDefined)
	{
		std::string name = functionName.empty() ? "<anonymous>" : functionName;
		if (lineDefined >= 0)
			name += ":" + std::to_string(lineDefined);
		return name;
	}

	std::string MakeScriptProfileKey(
		const std::wstring& sourceName,
		const std::string& callbackName,
		bool bNative)
	{
		return std::string(bNative ? "native:" : "luau:") +
			WideToUtf8Local(sourceName) + "::" + callbackName;
	}

	std::string MakeScriptSampleProfileKey(
		const std::wstring& sourceName,
		const std::string& functionName,
		int lineDefined)
	{
		return WideToUtf8Local(sourceName) + "::" + functionName + "::" + std::to_string(lineDefined);
	}

	std::string CsvEscape(const std::string& value)
	{
		if (value.find_first_of("\",\n\r") == std::string::npos)
			return value;

		std::string escaped;
		escaped.reserve(value.size() + 2);
		escaped.push_back('"');
		for (char c : value)
		{
			if (c == '"')
				escaped.push_back('"');
			escaped.push_back(c);
		}
		escaped.push_back('"');
		return escaped;
	}

	double ElapsedMilliseconds(
		const std::chrono::steady_clock::time_point& begin,
		const std::chrono::steady_clock::time_point& end)
	{
		return std::chrono::duration<double, std::milli>(end - begin).count();
	}

	bool HasNativeScriptCallbacks(const Corona::NativeEntityScriptCallbacks& callbacks)
	{
		return callbacks.Update || callbacks.Shutdown || callbacks.Ui || callbacks.ImGui;
	}

	bool IsStartupScriptSource(const std::wstring& sourceName)
	{
		std::wstring normalized = sourceName;
		std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
		std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t c)
		{
			return static_cast<wchar_t>(std::towlower(c));
		});
		return normalized.find(L"\\scripts\\startup\\") != std::wstring::npos;
	}

	bool IsPlatformerStartupScriptSource(const std::wstring& sourceName)
	{
		std::wstring normalized = sourceName;
		std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
		std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t c)
		{
			return static_cast<wchar_t>(std::towlower(c));
		});
		return normalized.find(L"\\scripts\\startup\\platformer\\") != std::wstring::npos;
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

	std::string NormalizeProceduralBoxAssetTexturePathForScript(const std::string& value)
	{
		const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) { return std::isspace(ch) != 0; });
		const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) { return std::isspace(ch) != 0; }).base();
		if (first >= last)
			return std::string();

		std::string path(first, last);
		std::replace(path.begin(), path.end(), '\\', '/');
		if (path.rfind("asset:", 0) == 0)
			path = path.substr(6);
		else if (path.rfind("tmx:", 0) == 0)
			path = std::string("assets/platformer/tmx_tiles/") + path.substr(4);

		while (!path.empty() && path.front() == '/')
			path.erase(path.begin());
		if (path.find("..") != std::string::npos)
			return std::string();

		std::string lowerPath = path;
		std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
		const bool hasImageExtension =
			lowerPath.size() >= 4 &&
			(lowerPath.rfind(".png") == lowerPath.size() - 4 ||
			 lowerPath.rfind(".bmp") == lowerPath.size() - 4 ||
			 lowerPath.rfind(".jpg") == lowerPath.size() - 4);
		if (!hasImageExtension)
			return std::string();
		if (lowerPath.rfind("assets/", 0) == 0)
			return path;
		if (lowerPath.rfind("platformer/tmx_tiles/", 0) == 0)
			return std::string("assets/") + path;
		return std::string();
	}

	std::string NormalizeProceduralBoxTextureKindForScript(const std::string& value)
	{
		const std::string assetPath = NormalizeProceduralBoxAssetTexturePathForScript(value);
		if (!assetPath.empty())
			return assetPath;

		const std::string key = NormalizeKeyName(value.c_str());
		if (key.empty() || key == "NONE" || key == "FLAT" || key == "SOLID" || key == "DEFAULT")
			return std::string();
		if (key == "BRICK" || key == "DUNGEONBRICK" || key == "DUNGEONSTONE")
			return "brick";
		if (key == "GRASS" || key == "MEADOW" || key == "FIELD" || key == "GROUND")
			return "grass";
		if (key == "LEAF" || key == "LEAVES" || key == "BUSH" || key == "FOLIAGE" || key == "VINE")
			return "leaf";
		if (key == "ROCK" || key == "STONE" || key == "BOULDER")
			return "rock";
		if (key == "FLOWER" || key == "FLOWERS" || key == "PETAL" || key == "PETALS")
			return "flower";
		if (key == "CLOUD" || key == "MIST")
			return "cloud";
		if (key == "SKY")
			return "sky";
		if (key == "DIRT" || key == "SOIL" || key == "EARTH")
			return "dirt";
		return std::string();
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

	void LuauScriptProfileInterrupt(lua_State* L, int gcState)
	{
		lua_Callbacks* callbacks = lua_callbacks(L);
		Corona* host = callbacks ? static_cast<Corona*>(callbacks->userdata) : nullptr;
		if (host)
			host->RecordLuauScriptProfileSample(L, gcState);
		if (callbacks)
			callbacks->interrupt = nullptr;
	}

	class ScopedLuauScriptProfileExecution
	{
	public:
		explicit ScopedLuauScriptProfileExecution(Corona* host)
			: Host(host)
		{
			if (Host)
				Host->BeginLuauScriptProfileExecution();
		}

	~ScopedLuauScriptProfileExecution()
	{
		if (Host)
			Host->EndLuauScriptProfileExecution();
	}

private:
	Corona* Host = nullptr;
};

	bool IsFiniteVec3Local(const glm::vec3& value)
	{
		return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
	}

	bool IsReasonableScriptWorldPosition(const glm::vec3& value)
	{
		constexpr float kMaxReasonableWorldCoordinate = 1000000.0f;
		return
			IsFiniteVec3Local(value) &&
			std::abs(value.x) <= kMaxReasonableWorldCoordinate &&
			std::abs(value.y) <= kMaxReasonableWorldCoordinate &&
			std::abs(value.z) <= kMaxReasonableWorldCoordinate;
	}

	bool ReadNumberAt(lua_State* L, int index, float& value)
	{
		if (!lua_isnumber(L, index))
			return false;
		const float readValue = static_cast<float>(lua_tonumber(L, index));
		if (!std::isfinite(readValue))
			return false;
		value = readValue;
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

	bool ReadVec4At(lua_State* L, int index, glm::vec4& value)
	{
		if (!lua_istable(L, index))
			return false;

		index = lua_absindex(L, index);
		glm::vec4 result = value;
		bool readAny = false;

		float component = 0.0f;
		if (ReadNumberField(L, index, "r", component) || ReadNumberField(L, index, "x", component))
		{
			result.x = component;
			readAny = true;
		}
		if (ReadNumberField(L, index, "g", component) || ReadNumberField(L, index, "y", component))
		{
			result.y = component;
			readAny = true;
		}
		if (ReadNumberField(L, index, "b", component) || ReadNumberField(L, index, "z", component))
		{
			result.z = component;
			readAny = true;
		}
		if (ReadNumberField(L, index, "a", component) || ReadNumberField(L, index, "w", component))
		{
			result.w = component;
			readAny = true;
		}

		if (!readAny)
		{
			lua_rawgeti(L, index, 1);
			const bool rOk = ReadNumberAt(L, -1, result.x);
			lua_pop(L, 1);
			lua_rawgeti(L, index, 2);
			const bool gOk = ReadNumberAt(L, -1, result.y);
			lua_pop(L, 1);
			lua_rawgeti(L, index, 3);
			const bool bOk = ReadNumberAt(L, -1, result.z);
			lua_pop(L, 1);
			lua_rawgeti(L, index, 4);
			const bool aOk = ReadNumberAt(L, -1, result.w);
			lua_pop(L, 1);
			readAny = rOk || gOk || bOk || aOk;
		}

		if (readAny)
		{
			if (result.x > 1.0f)
				result.x *= 1.0f / 255.0f;
			if (result.y > 1.0f)
				result.y *= 1.0f / 255.0f;
			if (result.z > 1.0f)
				result.z *= 1.0f / 255.0f;
			if (result.w > 1.0f)
				result.w *= 1.0f / 255.0f;
			result.x = std::clamp(result.x, 0.0f, 1.0f);
			result.y = std::clamp(result.y, 0.0f, 1.0f);
			result.z = std::clamp(result.z, 0.0f, 1.0f);
			result.w = std::clamp(result.w, 0.0f, 1.0f);
			value = result;
		}
		return readAny;
	}

	glm::vec4 ReadColorArg(lua_State* L, int index, const glm::vec4& fallback)
	{
		if (index > lua_gettop(L) || lua_isnil(L, index))
			return fallback;

		glm::vec4 color = fallback;
		ReadVec4At(L, index, color);
		return color;
	}

	ImU32 ToImU32(const glm::vec4& color)
	{
		return IM_COL32(
			static_cast<int>(std::clamp(color.x, 0.0f, 1.0f) * 255.0f + 0.5f),
			static_cast<int>(std::clamp(color.y, 0.0f, 1.0f) * 255.0f + 0.5f),
			static_cast<int>(std::clamp(color.z, 0.0f, 1.0f) * 255.0f + 0.5f),
			static_cast<int>(std::clamp(color.w, 0.0f, 1.0f) * 255.0f + 0.5f));
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
		glm::vec3* scale,
		bool* useScale,
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

		if (scale && useScale)
		{
			glm::vec3 readScale = *scale;
			bool hasScale = false;
			lua_getfield(L, tableIndex, "scale");
			if (lua_istable(L, -1) && ReadVec3At(L, -1, readScale))
				hasScale = true;
			lua_pop(L, 1);
			if (!hasScale)
				hasScale = ReadVec3Field(L, tableIndex, "size", readScale);
			if (!hasScale)
				hasScale = ReadVec3Field(L, tableIndex, "extent", readScale);
			if (!hasScale)
				hasScale = ReadVec3Field(L, tableIndex, "extents", readScale);

			if (hasScale)
			{
				*scale = glm::max(readScale, glm::vec3(0.001f));
				*useScale = true;
			}
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

	uint64_t EncodeScriptEntity(CoronaECS::Entity entity)
	{
		if (!entity.IsValid())
			return 0;

		return (static_cast<uint64_t>(entity.GetGeneration()) << 32) | static_cast<uint64_t>(entity.GetId());
	}

	void PushScriptEntity(lua_State* L, CoronaECS::Entity entity)
	{
		if (!entity.IsValid())
		{
			lua_pushnil(L);
			return;
		}

		lua_pushinteger(L, static_cast<lua_Integer>(EncodeScriptEntity(entity)));
	}

	CoronaECS::Entity DecodeScriptEntity(Corona* host, uint64_t encodedEntity)
	{
		if (!host || encodedEntity == 0)
			return CoronaECS::Entity();

		const uint32_t id = static_cast<uint32_t>(encodedEntity & 0xffffffffull);
		const uint32_t generation = static_cast<uint32_t>(encodedEntity >> 32);
		if (generation == 0)
			return host->GetEntityWorld().GetEntityById(id);

		CoronaECS::Entity entity(id, generation);
		return host->GetEntityWorld().IsAlive(entity) ? entity : CoronaECS::Entity();
	}

	CoronaECS::Entity ReadScriptEntity(lua_State* L, Corona* host, int index)
	{
		if (!lua_isnumber(L, index))
			return CoronaECS::Entity();

		const lua_Integer value = lua_tointeger(L, index);
		if (value <= 0)
			return CoronaECS::Entity();

		return DecodeScriptEntity(host, static_cast<uint64_t>(value));
	}

	bool ReadEntityTransformDesc(
		lua_State* L,
		int tableIndex,
		glm::vec3& position,
		glm::vec3& rotationDegrees,
		glm::vec3& scale)
	{
		if (!lua_istable(L, tableIndex))
			return false;

		tableIndex = lua_absindex(L, tableIndex);
		ReadVec3Field(L, tableIndex, "position", position);
		ReadVec3Field(L, tableIndex, "rotation", rotationDegrees);
		ReadVec3Field(L, tableIndex, "rotationDegrees", rotationDegrees);
		ReadVec3Field(L, tableIndex, "rotation_degrees", rotationDegrees);

		lua_getfield(L, tableIndex, "scale");
		if (lua_isnumber(L, -1))
		{
			const float uniformScale = static_cast<float>(lua_tonumber(L, -1));
			scale = glm::vec3(std::max(uniformScale, 0.001f));
		}
		else if (lua_istable(L, -1))
		{
			glm::vec3 readScale = scale;
			if (ReadVec3At(L, -1, readScale))
				scale = glm::max(readScale, glm::vec3(0.001f));
		}
		lua_pop(L, 1);

		glm::vec3 readScale = scale;
		if (ReadVec3Field(L, tableIndex, "size", readScale) ||
			ReadVec3Field(L, tableIndex, "extent", readScale) ||
			ReadVec3Field(L, tableIndex, "extents", readScale))
		{
			scale = glm::max(readScale, glm::vec3(0.001f));
		}

		return true;
	}

	bool ReadPhysicsShapeField(lua_State* L, int tableIndex, CoronaECS::PhysicsCollisionShape& shape)
	{
		lua_getfield(L, tableIndex, "shape");
		if (lua_isnil(L, -1))
		{
			lua_pop(L, 1);
			lua_getfield(L, tableIndex, "collision_shape");
		}
		if (lua_isnil(L, -1))
		{
			lua_pop(L, 1);
			lua_getfield(L, tableIndex, "collisionShape");
		}

		bool read = false;
		if (lua_isnumber(L, -1))
		{
			const int value = static_cast<int>(lua_tointeger(L, -1));
			shape = value == 1 ? CoronaECS::PhysicsCollisionShape::Box : CoronaECS::PhysicsCollisionShape::TriangleMesh;
			read = true;
		}
		else if (lua_isstring(L, -1))
		{
			const std::string name = NormalizeKeyName(lua_tostring(L, -1));
			if (name == "BOX")
			{
				shape = CoronaECS::PhysicsCollisionShape::Box;
				read = true;
			}
			else if (name == "TRIANGLEMESH" || name == "MESH" || name == "TRIANGLES")
			{
				shape = CoronaECS::PhysicsCollisionShape::TriangleMesh;
				read = true;
			}
		}
		lua_pop(L, 1);
		return read;
	}

	bool ReadPhysicsDesc(lua_State* L, int tableIndex, CoronaECS::PhysicsComponent& component)
	{
		if (!lua_istable(L, tableIndex))
			return false;

		tableIndex = lua_absindex(L, tableIndex);
		bool enabled = component.bQueryEnabled;
		if (ReadBoolField(L, tableIndex, "enabled", enabled) ||
			ReadBoolField(L, tableIndex, "query_enabled", enabled) ||
			ReadBoolField(L, tableIndex, "queryEnabled", enabled) ||
			ReadBoolField(L, tableIndex, "physics_query", enabled) ||
			ReadBoolField(L, tableIndex, "physicsQuery", enabled))
		{
			component.bQueryEnabled = enabled;
		}

		ReadPhysicsShapeField(L, tableIndex, component.CollisionShape);

		glm::vec3 boxHalfExtent = component.BoxHalfExtent;
		if (ReadVec3Field(L, tableIndex, "box_half_extent", boxHalfExtent) ||
			ReadVec3Field(L, tableIndex, "boxHalfExtent", boxHalfExtent) ||
			ReadVec3Field(L, tableIndex, "half_extent", boxHalfExtent) ||
			ReadVec3Field(L, tableIndex, "halfExtent", boxHalfExtent))
		{
			component.BoxHalfExtent = glm::max(boxHalfExtent, glm::vec3(0.001f));
		}
		return true;
	}

	bool ReadLightTypeField(lua_State* L, int tableIndex, CoronaECS::LightType& type)
	{
		lua_getfield(L, tableIndex, "type");
		bool read = false;
		if (lua_isnumber(L, -1))
		{
			const int value = static_cast<int>(lua_tointeger(L, -1));
			type = value == 0 ? CoronaECS::LightType::Directional : CoronaECS::LightType::Point;
			read = true;
		}
		else if (lua_isstring(L, -1))
		{
			const std::string name = NormalizeKeyName(lua_tostring(L, -1));
			if (name == "DIRECTIONAL" || name == "SUN")
			{
				type = CoronaECS::LightType::Directional;
				read = true;
			}
			else if (name == "POINT")
			{
				type = CoronaECS::LightType::Point;
				read = true;
			}
		}
		lua_pop(L, 1);
		return read;
	}

	bool ReadLightDesc(lua_State* L, int tableIndex, CoronaECS::LightComponent& component)
	{
		if (!lua_istable(L, tableIndex))
			return false;

		tableIndex = lua_absindex(L, tableIndex);
		ReadLightTypeField(L, tableIndex, component.Type);

		bool enabled = component.bEnabled;
		if (ReadBoolField(L, tableIndex, "enabled", enabled))
			component.bEnabled = enabled;

		glm::vec3 color = component.Color;
		if (ReadVec3Field(L, tableIndex, "color", color))
			component.Color = glm::max(color, glm::vec3(0.0f));

		float intensity = component.Intensity;
		if (ReadNumberField(L, tableIndex, "intensity", intensity))
			component.Intensity = std::max(0.0f, intensity);

		float radius = component.Radius;
		if (ReadNumberField(L, tableIndex, "radius", radius))
			component.Radius = std::clamp(radius, 1.0f, 100000.0f);

		glm::vec3 direction = component.Direction;
		if (ReadVec3Field(L, tableIndex, "direction", direction) ||
			ReadVec3Field(L, tableIndex, "light_dir", direction) ||
			ReadVec3Field(L, tableIndex, "lightDir", direction))
		{
			if (glm::length(direction) > 0.0001f)
				component.Direction = glm::normalize(direction);
		}
		return true;
	}

	std::string ReadFirstStringField(lua_State* L, int tableIndex, std::initializer_list<const char*> names)
	{
		tableIndex = lua_absindex(L, tableIndex);
		for (const char* name : names)
		{
			lua_getfield(L, tableIndex, name);
			if (lua_isstring(L, -1))
			{
				std::string value = lua_tostring(L, -1);
				lua_pop(L, 1);
				return value;
			}
			lua_pop(L, 1);
		}
		return std::string();
	}

	bool HasAnyField(lua_State* L, int tableIndex, std::initializer_list<const char*> names)
	{
		tableIndex = lua_absindex(L, tableIndex);
		for (const char* name : names)
		{
			lua_getfield(L, tableIndex, name);
			const bool found = !lua_isnil(L, -1);
			lua_pop(L, 1);
			if (found)
				return true;
		}
		return false;
	}

	void PushPhysicsDesc(lua_State* L, const CoronaECS::PhysicsComponent& component)
	{
		lua_newtable(L);
		PushBoolField(L, "enabled", component.bQueryEnabled);
		lua_pushstring(L, component.CollisionShape == CoronaECS::PhysicsCollisionShape::Box ? "box" : "triangle_mesh");
		lua_setfield(L, -2, "shape");
		PushVec3Field(L, "box_half_extent", component.BoxHalfExtent);
	}

	void PushLightDesc(lua_State* L, const CoronaECS::LightComponent& component)
	{
		lua_newtable(L);
		lua_pushstring(L, component.Type == CoronaECS::LightType::Directional ? "directional" : "point");
		lua_setfield(L, -2, "type");
		PushBoolField(L, "enabled", component.bEnabled);
		PushVec3Field(L, "color", component.Color);
		PushNumberField(L, "intensity", component.Intensity);
		PushVec3Field(L, "direction", component.Direction);
		PushNumberField(L, "radius", component.Radius);
		PushIntegerField(L, "runtime_light_id", static_cast<lua_Integer>(component.RuntimeLightId));
	}

	void PushMeshDesc(lua_State* L, const CoronaECS::MeshComponent& component)
	{
		lua_newtable(L);
		PushBoolField(L, "visible", component.bVisible);
		PushBoolField(L, "ray_tracing", component.bRayTracing);
		PushNumberField(L, "roughness", component.Roughness);
		PushNumberField(L, "metallic", component.Metallic);
		PushBoolField(L, "override_material", component.bOverrideRoughnessMetallic);
		PushIntegerField(L, "render_object_handle", static_cast<lua_Integer>(component.RenderObjectHandle));
	}

	void PushCameraDesc(lua_State* L, const CoronaECS::CameraComponent& component)
	{
		lua_newtable(L);
		PushVec3Field(L, "look_direction", component.LookDirection);
		PushVec3Field(L, "up_direction", component.UpDirection);
		PushNumberField(L, "fov", component.Fov);
		PushNumberField(L, "near_plane", component.NearPlane);
		PushNumberField(L, "far_plane", component.FarPlane);
		PushBoolField(L, "active", component.bActive);
	}

	void UnrefLuaRef(lua_State* L, int& ref)
	{
		if (!L || ref == LUA_REFNIL)
			return;

		lua_unref(L, ref);
		ref = LUA_REFNIL;
	}

	bool HasScriptCallbacks(const CoronaECS::ScriptInstance& script)
	{
		return
			script.UpdateRef != LUA_REFNIL ||
			script.ShutdownRef != LUA_REFNIL ||
			script.ImGuiRef != LUA_REFNIL ||
			script.UiRef != LUA_REFNIL ||
			!script.NativeScriptName.empty();
	}

	CoronaECS::ScriptInstance* FindScriptInstance(CoronaECS::ScriptComponent* component, uint32_t instanceId)
	{
		if (!component)
			return nullptr;

		for (CoronaECS::ScriptInstance& script : component->Instances)
		{
			if (script.InstanceId == instanceId)
				return &script;
		}
		return nullptr;
	}

	void UnrefScriptInstance(lua_State* L, CoronaECS::ScriptInstance& script)
	{
		UnrefLuaRef(L, script.UpdateRef);
		UnrefLuaRef(L, script.ShutdownRef);
		UnrefLuaRef(L, script.ImGuiRef);
		UnrefLuaRef(L, script.UiRef);
	}

	int LuaCoronaLog(lua_State* L)
	{
		const char* message = luaL_checkstring(L, 1);
		AppendCpuRuntimeTrace(L"[Luau] " + Utf8ToWideLocal(message ? message : ""));
		return 0;
	}

	int LuaCoronaRequestExit(lua_State* L)
	{
		(void)L;
		QuitPlatformApplication(0);
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

	int LuaCoronaCreateBoxScene(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		glm::vec3 color(0.72f, 0.72f, 0.72f);
		bool brickTexture = false;
		float uvRepeat = 1.0f;
		float uvRepeatY = -1.0f;
		bool frontOnly = false;
		std::string textureKind;
		if (lua_istable(L, 1))
		{
			const int tableIndex = lua_absindex(L, 1);
			if (!ReadVec3Field(L, tableIndex, "color", color))
			{
				if (!ReadVec3Field(L, tableIndex, "base_color", color))
					ReadVec3Field(L, tableIndex, "baseColor", color);
			}
			if (!ReadBoolField(L, tableIndex, "brick_texture", brickTexture))
				ReadBoolField(L, tableIndex, "brickTexture", brickTexture);
			if (!ReadNumberField(L, tableIndex, "uv_repeat", uvRepeat))
			{
				if (!ReadNumberField(L, tableIndex, "uvRepeat", uvRepeat))
					ReadNumberField(L, tableIndex, "texture_repeat", uvRepeat);
			}
			if (!ReadNumberField(L, tableIndex, "uv_repeat_y", uvRepeatY))
			{
				if (!ReadNumberField(L, tableIndex, "uvRepeatY", uvRepeatY))
				{
					if (!ReadNumberField(L, tableIndex, "texture_repeat_y", uvRepeatY))
						ReadNumberField(L, tableIndex, "textureRepeatY", uvRepeatY);
				}
			}
			if (!ReadNumberField(L, tableIndex, "uv_repeat_x", uvRepeat))
			{
				if (!ReadNumberField(L, tableIndex, "uvRepeatX", uvRepeat))
				{
					if (!ReadNumberField(L, tableIndex, "texture_repeat_x", uvRepeat))
						ReadNumberField(L, tableIndex, "textureRepeatX", uvRepeat);
				}
			}
			if (!ReadBoolField(L, tableIndex, "front_only", frontOnly))
			{
				if (!ReadBoolField(L, tableIndex, "frontOnly", frontOnly))
				{
					if (!ReadBoolField(L, tableIndex, "surface_only", frontOnly))
						ReadBoolField(L, tableIndex, "surfaceOnly", frontOnly);
				}
			}
			textureKind = NormalizeProceduralBoxTextureKindForScript(ReadFirstStringField(
				L,
				tableIndex,
				{ "texture_kind", "textureKind", "texture", "terrain_texture", "terrainTexture", "material_kind", "materialKind" }));
		}

		const Corona::ScriptSceneHandle handle = host->CreateProceduralBoxSceneForScript(color, brickTexture, uvRepeat, Utf8ToWideLocal(textureKind), uvRepeatY, frontOnly);
		if (handle == Corona::InvalidScriptSceneHandle)
		{
			luaL_error(L, "failed to create procedural box scene");
			return 0;
		}

		lua_pushinteger(L, static_cast<int>(handle));
		return 1;
	}

	int LuaCoronaCreateSpineScene(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		std::string path;
		std::string animation;
		float timeSeconds = 0.0f;
		float sourceScale = 1.0f;
		if (lua_istable(L, 1))
		{
			const int tableIndex = lua_absindex(L, 1);
			path = ReadFirstStringField(L, tableIndex, { "path", "asset", "asset_path", "assetPath", "skeleton" });
			animation = ReadFirstStringField(L, tableIndex, { "animation", "anim", "animation_name", "animationName" });
			if (!ReadNumberField(L, tableIndex, "time", timeSeconds))
			{
				if (!ReadNumberField(L, tableIndex, "time_seconds", timeSeconds))
				{
					if (!ReadNumberField(L, tableIndex, "timeSeconds", timeSeconds))
						ReadNumberField(L, tableIndex, "sample_time", timeSeconds);
				}
			}
			if (!ReadNumberField(L, tableIndex, "source_scale", sourceScale))
			{
				if (!ReadNumberField(L, tableIndex, "sourceScale", sourceScale))
					ReadNumberField(L, tableIndex, "scale", sourceScale);
			}
		}
		else
		{
			const char* pathArg = luaL_checkstring(L, 1);
			path = pathArg ? pathArg : "";
			if (lua_gettop(L) >= 2 && lua_isstring(L, 2))
				animation = lua_tostring(L, 2);
			if (lua_gettop(L) >= 3 && lua_isnumber(L, 3))
				timeSeconds = static_cast<float>(lua_tonumber(L, 3));
			if (lua_gettop(L) >= 4 && lua_isnumber(L, 4))
				sourceScale = static_cast<float>(lua_tonumber(L, 4));
		}

		if (path.empty())
		{
			luaL_error(L, "SpineComponent.create_scene expects a spine skeleton path");
			return 0;
		}

		const Corona::ScriptSceneHandle handle = host->CreateSpineSceneForScript(
			Utf8ToWideLocal(path),
			animation,
			timeSeconds,
			sourceScale);
		if (handle == Corona::InvalidScriptSceneHandle)
		{
			luaL_error(L, "failed to create spine scene '%s'", path.c_str());
			return 0;
		}

		lua_pushinteger(L, static_cast<int>(handle));
		return 1;
	}

	int LuaCoronaCreateLiveSpine(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}
		const char* pathArg = luaL_checkstring(L, 1);
		std::string path = pathArg ? pathArg : "";
		std::string animation;
		float sourceScale = 1.0f;
		if (lua_gettop(L) >= 2 && lua_isstring(L, 2))
			animation = lua_tostring(L, 2);
		if (lua_gettop(L) >= 3 && lua_isnumber(L, 3))
			sourceScale = static_cast<float>(lua_tonumber(L, 3));
		if (path.empty())
		{
			luaL_error(L, "SpineComponent.create_live expects a spine skeleton path");
			return 0;
		}
		const Corona::ScriptSceneHandle handle = host->CreateLiveSpineForScript(
			Utf8ToWideLocal(path), animation, sourceScale);
		if (handle == Corona::InvalidScriptSceneHandle)
		{
			lua_pushnil(L);
			return 1;
		}
		lua_pushinteger(L, static_cast<int>(handle));
		return 1;
	}

	int LuaCoronaUpdateLiveSpine(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}
		const lua_Integer rawHandle = luaL_checkinteger(L, 1);
		const float dt = static_cast<float>(luaL_checknumber(L, 2));
		const bool ok = host->UpdateLiveSpineForScript(
			static_cast<Corona::ScriptSceneHandle>(rawHandle), dt);
		lua_pushboolean(L, ok ? 1 : 0);
		return 1;
	}

	int LuaCoronaDestroyLiveSpine(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}
		const lua_Integer rawHandle = luaL_checkinteger(L, 1);
		host->DestroyLiveSpineForScript(static_cast<Corona::ScriptSceneHandle>(rawHandle));
		return 0;
	}

	int LuaCoronaGetSpineSceneHeight(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		Corona::ScriptSceneHandle sceneHandle = Corona::InvalidScriptSceneHandle;
		if (lua_isnumber(L, 1))
		{
			const lua_Integer rawHandle = lua_tointeger(L, 1);
			if (rawHandle > 0)
				sceneHandle = static_cast<Corona::ScriptSceneHandle>(rawHandle);
		}

		lua_pushnumber(L, static_cast<lua_Number>(host->GetScriptSceneHeightForScript(sceneHandle)));
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
		glm::vec3 scale(1.0f);
		bool useScale = false;
		float roughness = 1.0f;
		float metallic = 0.0f;
		bool overrideMaterial = false;
		bool visible = true;
		bool rayTracing = true;
		bool physicsQuery = true;
		if (!ReadTransformDesc(L, 2, position, rotationDegrees, targetExtent, &scale, &useScale, &roughness, &metallic, &overrideMaterial, &visible, &rayTracing, &physicsQuery))
		{
			luaL_error(L, "corona.spawn expects a descriptor table");
			return 0;
		}

		const Corona::SceneObjectHandle handle = host->SpawnSceneObjectForScript(
			sceneHandle,
			position,
			rotationDegrees,
			targetExtent,
			scale,
			useScale,
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

	int LuaCoronaEntitySpawn(lua_State* L)
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
		glm::vec3 scale(1.0f);
		bool useScale = false;
		float roughness = 1.0f;
		float metallic = 0.0f;
		bool overrideMaterial = false;
		bool visible = true;
		bool rayTracing = true;
		bool physicsQuery = true;
		if (!ReadTransformDesc(L, 2, position, rotationDegrees, targetExtent, &scale, &useScale, &roughness, &metallic, &overrideMaterial, &visible, &rayTracing, &physicsQuery))
		{
			luaL_error(L, "corona.entity_spawn expects a descriptor table");
			return 0;
		}

		const CoronaECS::Entity entity = host->SpawnEntityForScript(
			sceneHandle,
			position,
			rotationDegrees,
			targetExtent,
			scale,
			useScale,
			roughness,
			metallic,
			overrideMaterial,
			visible,
			rayTracing,
			physicsQuery);
		if (!entity.IsValid())
		{
			luaL_error(L, "failed to spawn entity");
			return 0;
		}

		PushScriptEntity(L, entity);
		return 1;
	}

	int LuaCoronaEntityCreate(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		std::string name;
		if (lua_isstring(L, 1))
		{
			name = lua_tostring(L, 1);
		}
		else if (lua_istable(L, 1))
		{
			name = ReadFirstStringField(L, 1, { "name", "debug_name", "debugName" });
		}

		PushScriptEntity(L, host->CreateEntity(name));
		return 1;
	}

	int LuaCoronaGetEntity(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const Corona::SceneObjectHandle handle = static_cast<Corona::SceneObjectHandle>(luaL_checkinteger(L, 1));
		PushScriptEntity(L, host->GetSceneObjectEntity(handle));
		return 1;
	}

	int LuaCoronaGetEntityObject(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const CoronaECS::Entity entity = ReadScriptEntity(L, host, 1);
		const Corona::SceneObjectHandle handle = host->GetEntitySceneObject(entity);
		if (handle == Corona::InvalidSceneObjectHandle)
			lua_pushnil(L);
		else
			lua_pushinteger(L, static_cast<int>(handle));
		return 1;
	}

	int LuaCoronaGetMainCameraEntity(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		PushScriptEntity(L, host->GetMainCameraEntityForScript());
		return 1;
	}

	int LuaCoronaGetWorldEntity(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		PushScriptEntity(L, host->GetWorldEntityForScript());
		return 1;
	}

	int LuaCoronaGetLevelEntity(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		PushScriptEntity(L, host->GetLevelEntityForScript());
		return 1;
	}

	int LuaCoronaGetMainDirectionalLightEntity(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		PushScriptEntity(L, host->GetMainDirectionalLightEntityForScript());
		return 1;
	}

	int LuaCoronaPointLightSpawn(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}
		if (!lua_istable(L, 1))
		{
			luaL_error(L, "corona.point_light_spawn expects a descriptor table");
			return 0;
		}

		const int tableIndex = lua_absindex(L, 1);
		glm::vec3 position(0.0f);
		glm::vec3 color(1.0f);
		float radius = 320.0f;
		float intensity = 10.0f;
		bool enabled = true;
		ReadVec3Field(L, tableIndex, "position", position);
		ReadVec3Field(L, tableIndex, "color", color);
		ReadNumberField(L, tableIndex, "radius", radius);
		ReadNumberField(L, tableIndex, "intensity", intensity);
		ReadBoolField(L, tableIndex, "enabled", enabled);

		const CoronaECS::Entity entity = host->SpawnPointLightEntityForScript(
			position,
			std::clamp(radius, 1.0f, 100000.0f),
			glm::max(color, glm::vec3(0.0f)),
			std::max(0.0f, intensity),
			enabled);
		if (!entity.IsValid())
		{
			lua_pushnil(L);
			return 1;
		}

		PushScriptEntity(L, entity);
		return 1;
	}

	int LuaCoronaEntityExists(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const CoronaECS::Entity entity = ReadScriptEntity(L, host, 1);
		lua_pushboolean(L, host->GetEntityWorld().IsAlive(entity) ? 1 : 0);
		return 1;
	}

	// corona.Entity.by_name(name) — first alive entity with that name, or nil.
	// Useful after corona.load_map() to fetch the handles the map restored.
	int LuaCoronaEntityByName(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host) { luaL_error(L, "corona host is not available"); return 0; }
		const char* name = luaL_checkstring(L, 1);
		const CoronaECS::Entity entity = host->GetEntityWorld().FindEntityByName(name ? name : "");
		if (!entity.IsValid())
		{
			lua_pushnil(L);
			return 1;
		}
		PushScriptEntity(L, entity);
		return 1;
	}

	int LuaCoronaEntitySetTransform(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const CoronaECS::Entity entity = ReadScriptEntity(L, host, 1);
		glm::vec3 position(0.0f);
		host->GetEntityTransformForScript(entity, position);
		glm::vec3 rotationDegrees(0.0f);
		glm::vec3 scale(1.0f);
		if (!ReadEntityTransformDesc(L, 2, position, rotationDegrees, scale))
		{
			luaL_error(L, "corona.entity_set_transform expects an entity and descriptor table");
			return 0;
		}

		lua_pushboolean(L, host->SetEntityTransformForScript(entity, position, rotationDegrees, scale) ? 1 : 0);
		return 1;
	}

	int LuaCoronaEntityGetTransform(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const CoronaECS::Entity entity = ReadScriptEntity(L, host, 1);
		glm::vec3 position(0.0f);
		if (!host->GetEntityTransformForScript(entity, position))
		{
			lua_pushnil(L);
			return 1;
		}

		lua_newtable(L);
		PushVec3(L, position);
		lua_setfield(L, -2, "position");
		return 1;
	}

	int LuaCoronaEntitySetPhysics(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const CoronaECS::Entity entity = ReadScriptEntity(L, host, 1);
		CoronaECS::PhysicsComponent component;
		host->GetEntityPhysicsForScript(entity, component);
		if (!ReadPhysicsDesc(L, 2, component))
		{
			luaL_error(L, "corona.entity_set_physics expects an entity and descriptor table");
			return 0;
		}

		lua_pushboolean(L, host->SetEntityPhysicsForScript(entity, component.bQueryEnabled, component.CollisionShape, component.BoxHalfExtent) ? 1 : 0);
		return 1;
	}

	int LuaCoronaEntityGetPhysics(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const CoronaECS::Entity entity = ReadScriptEntity(L, host, 1);
		CoronaECS::PhysicsComponent component;
		if (!host->GetEntityPhysicsForScript(entity, component))
		{
			lua_pushnil(L);
			return 1;
		}

		PushPhysicsDesc(L, component);
		return 1;
	}

	int LuaCoronaEntitySetLight(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const CoronaECS::Entity entity = ReadScriptEntity(L, host, 1);
		CoronaECS::LightComponent component;
		host->GetEntityLightForScript(entity, component);
		if (!ReadLightDesc(L, 2, component))
		{
			luaL_error(L, "corona.entity_set_light expects an entity and descriptor table");
			return 0;
		}

		bool persistSceneState = true;
		if (!ReadBoolField(L, lua_absindex(L, 2), "persistent", persistSceneState) &&
			!ReadBoolField(L, lua_absindex(L, 2), "persist", persistSceneState) &&
			!ReadBoolField(L, lua_absindex(L, 2), "save_state", persistSceneState))
		{
			ReadBoolField(L, lua_absindex(L, 2), "saveState", persistSceneState);
		}

		lua_pushboolean(L, host->SetEntityLightForScript(entity, component, persistSceneState) ? 1 : 0);
		return 1;
	}

	int LuaCoronaEntityGetLight(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const CoronaECS::Entity entity = ReadScriptEntity(L, host, 1);
		CoronaECS::LightComponent component;
		if (!host->GetEntityLightForScript(entity, component))
		{
			lua_pushnil(L);
			return 1;
		}

		PushLightDesc(L, component);
		return 1;
	}

	int LuaCoronaMeshComponentAdd(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const CoronaECS::Entity entity = ReadScriptEntity(L, host, 1);
		if (!lua_istable(L, 2))
		{
			luaL_error(L, "MeshComponent.add expects an entity and descriptor table");
			return 0;
		}

		const int tableIndex = lua_absindex(L, 2);
		Corona::ScriptSceneHandle sceneHandle = Corona::InvalidScriptSceneHandle;
		lua_getfield(L, tableIndex, "scene_handle");
		if (lua_isnumber(L, -1))
			sceneHandle = static_cast<Corona::ScriptSceneHandle>(lua_tointeger(L, -1));
		lua_pop(L, 1);

		if (sceneHandle == Corona::InvalidScriptSceneHandle)
		{
			const std::string primitive = NormalizeKeyName(ReadFirstStringField(L, tableIndex, { "primitive", "type" }).c_str());
			if (primitive == "BOX" || primitive == "CUBE" || primitive == "PLANE" || primitive == "QUAD")
			{
				glm::vec3 color(0.72f, 0.72f, 0.72f);
				bool brickTexture = false;
				float uvRepeat = 1.0f;
				float uvRepeatY = -1.0f;
				bool frontOnly = primitive == "PLANE" || primitive == "QUAD";
				std::string textureKind;
				if (!ReadVec3Field(L, tableIndex, "color", color))
				{
					if (!ReadVec3Field(L, tableIndex, "base_color", color))
						ReadVec3Field(L, tableIndex, "baseColor", color);
				}
				if (!ReadBoolField(L, tableIndex, "brick_texture", brickTexture))
					ReadBoolField(L, tableIndex, "brickTexture", brickTexture);
				if (!ReadNumberField(L, tableIndex, "uv_repeat", uvRepeat))
				{
					if (!ReadNumberField(L, tableIndex, "uvRepeat", uvRepeat))
						ReadNumberField(L, tableIndex, "texture_repeat", uvRepeat);
				}
				if (!ReadNumberField(L, tableIndex, "uv_repeat_y", uvRepeatY))
				{
					if (!ReadNumberField(L, tableIndex, "uvRepeatY", uvRepeatY))
					{
						if (!ReadNumberField(L, tableIndex, "texture_repeat_y", uvRepeatY))
							ReadNumberField(L, tableIndex, "textureRepeatY", uvRepeatY);
					}
				}
				if (!ReadNumberField(L, tableIndex, "uv_repeat_x", uvRepeat))
				{
					if (!ReadNumberField(L, tableIndex, "uvRepeatX", uvRepeat))
					{
						if (!ReadNumberField(L, tableIndex, "texture_repeat_x", uvRepeat))
							ReadNumberField(L, tableIndex, "textureRepeatX", uvRepeat);
					}
				}
				if (!ReadBoolField(L, tableIndex, "front_only", frontOnly))
				{
					if (!ReadBoolField(L, tableIndex, "frontOnly", frontOnly))
					{
						if (!ReadBoolField(L, tableIndex, "surface_only", frontOnly))
							ReadBoolField(L, tableIndex, "surfaceOnly", frontOnly);
					}
				}
				textureKind = NormalizeProceduralBoxTextureKindForScript(ReadFirstStringField(
					L,
					tableIndex,
					{ "texture_kind", "textureKind", "texture", "terrain_texture", "terrainTexture", "material_kind", "materialKind" }));
				sceneHandle = host->CreateProceduralBoxSceneForScript(color, brickTexture, uvRepeat, Utf8ToWideLocal(textureKind), uvRepeatY, frontOnly);
			}
			else if (primitive == "BLOCKCHARACTER" || primitive == "CHARACTER")
			{
				UINT32 seed = 20260503u;
				lua_getfield(L, tableIndex, "seed");
				if (lua_isnumber(L, -1))
					seed = static_cast<UINT32>(std::max<lua_Integer>(1, lua_tointeger(L, -1)));
				lua_pop(L, 1);
				sceneHandle = host->CreateProceduralBlockCharacterSceneForScript(seed);
			}
			else if (primitive == "GRASS")
			{
				float numBladesF  = 5000.0f;
				float areaSize    = 1000.0f;
				float bladeHeight = 30.0f;
				UINT32 seed       = 20260527u;
				if (!ReadNumberField(L, tableIndex, "blade_count", numBladesF))
				{
					if (!ReadNumberField(L, tableIndex, "bladeCount", numBladesF))
						ReadNumberField(L, tableIndex, "count", numBladesF);
				}
				if (!ReadNumberField(L, tableIndex, "area_size", areaSize))
				{
					if (!ReadNumberField(L, tableIndex, "areaSize", areaSize))
						ReadNumberField(L, tableIndex, "area", areaSize);
				}
				if (!ReadNumberField(L, tableIndex, "blade_height", bladeHeight))
				{
					if (!ReadNumberField(L, tableIndex, "bladeHeight", bladeHeight))
						ReadNumberField(L, tableIndex, "height", bladeHeight);
				}
				lua_getfield(L, tableIndex, "seed");
				if (lua_isnumber(L, -1))
					seed = static_cast<UINT32>(std::max<lua_Integer>(1, lua_tointeger(L, -1)));
				lua_pop(L, 1);
				float bladeSegmentsF = 4.0f;
				if (!ReadNumberField(L, tableIndex, "blade_segments", bladeSegmentsF))
					ReadNumberField(L, tableIndex, "bladeSegments", bladeSegmentsF);
				const UINT32 numBlades = static_cast<UINT32>(std::max(1.0f, std::round(numBladesF)));
				const UINT32 bladeSegments = static_cast<UINT32>(std::max(1.0f, std::round(bladeSegmentsF)));
				sceneHandle = host->CreateProceduralGrassSceneForScript(numBlades, areaSize, bladeHeight, seed, bladeSegments);
			}
			else if (primitive == "TERRAIN")
			{
				UINT32 seed = 1u;
				lua_getfield(L, tableIndex, "seed");
				if (lua_isnumber(L, -1))
					seed = static_cast<UINT32>(std::max<lua_Integer>(1, lua_tointeger(L, -1)));
				lua_pop(L, 1);
				sceneHandle = host->CreateProceduralTerrainSceneForScript(seed);
			}
			else if (primitive == "GRASSONTERRAIN")
			{
				float numBladesF  = 80000.0f;
				float bladeHeight = 30.0f;
				UINT32 seed       = 20260527u;
				if (!ReadNumberField(L, tableIndex, "blade_count", numBladesF))
				{
					if (!ReadNumberField(L, tableIndex, "bladeCount", numBladesF))
						ReadNumberField(L, tableIndex, "count", numBladesF);
				}
				if (!ReadNumberField(L, tableIndex, "blade_height", bladeHeight))
				{
					if (!ReadNumberField(L, tableIndex, "bladeHeight", bladeHeight))
						ReadNumberField(L, tableIndex, "height", bladeHeight);
				}
				lua_getfield(L, tableIndex, "seed");
				if (lua_isnumber(L, -1))
					seed = static_cast<UINT32>(std::max<lua_Integer>(1, lua_tointeger(L, -1)));
				lua_pop(L, 1);
				float bladeSegmentsF = 4.0f;
				if (!ReadNumberField(L, tableIndex, "blade_segments", bladeSegmentsF))
					ReadNumberField(L, tableIndex, "bladeSegments", bladeSegmentsF);
				bool bProcedural = false;
				ReadBoolField(L, tableIndex, "procedural", bProcedural);
				const UINT32 numBlades = static_cast<UINT32>(std::max(1.0f, std::round(numBladesF)));
				const UINT32 bladeSegments = static_cast<UINT32>(std::max(1.0f, std::round(bladeSegmentsF)));
				sceneHandle = bProcedural
					? host->CreateProceduralGrassOnTerrainSceneInstancedForScript(numBlades, bladeHeight, seed, bladeSegments)
					: host->CreateProceduralGrassOnTerrainSceneForScript(numBlades, bladeHeight, seed, bladeSegments);
			}
			else
			{
				bool isSpine = false;
				ReadBoolField(L, tableIndex, "spine", isSpine);
				if (!isSpine)
					isSpine = HasAnyField(L, tableIndex, { "skeleton", "spine_path", "spinePath", "animation", "anim" });
				const std::string path = ReadFirstStringField(
					L,
					tableIndex,
					isSpine ?
						std::initializer_list<const char*>{ "path", "asset", "asset_path", "assetPath", "skeleton", "spine_path", "spinePath" } :
						std::initializer_list<const char*>{ "path", "asset", "asset_path", "assetPath", "model" });
				if (!path.empty())
				{
					if (isSpine)
					{
						const std::string animation = ReadFirstStringField(L, tableIndex, { "animation", "anim", "animation_name", "animationName" });
						float timeSeconds = 0.0f;
						if (!ReadNumberField(L, tableIndex, "time", timeSeconds))
						{
							if (!ReadNumberField(L, tableIndex, "time_seconds", timeSeconds))
								ReadNumberField(L, tableIndex, "timeSeconds", timeSeconds);
						}
						float sourceScale = 1.0f;
						if (!ReadNumberField(L, tableIndex, "source_scale", sourceScale))
						{
							if (!ReadNumberField(L, tableIndex, "sourceScale", sourceScale))
								ReadNumberField(L, tableIndex, "scale", sourceScale);
						}
						sceneHandle = host->CreateSpineSceneForScript(Utf8ToWideLocal(path), animation, timeSeconds, sourceScale);
					}
					else
					{
						sceneHandle = host->LoadSceneForScript(Utf8ToWideLocal(path));
					}
				}
			}
		}

		if (sceneHandle == Corona::InvalidScriptSceneHandle)
		{
			glm::vec3 position(0.0f);
			host->GetEntityTransformForScript(entity, position);
			glm::vec3 rotationDegrees(0.0f);
			float targetExtent = 1.0f;
			glm::vec3 scale(1.0f);
			bool useScale = false;
			const Corona::SceneObjectHandle objectHandle = host->GetEntitySceneObject(entity);
			if (objectHandle != Corona::InvalidSceneObjectHandle)
				host->GetSceneObjectTransformForScript(objectHandle, position, rotationDegrees, targetExtent, scale, useScale);

			float roughness = 1.0f;
			float metallic = 0.0f;
			bool overrideMaterial = false;
			bool visible = true;
			bool rayTracing = true;
			bool physicsQuery = true;
			CoronaECS::MeshComponent meshComponent;
			if (host->GetEntityMeshForScript(entity, meshComponent))
			{
				roughness = meshComponent.Roughness;
				metallic = meshComponent.Metallic;
				overrideMaterial = meshComponent.bOverrideRoughnessMetallic;
				visible = meshComponent.bVisible;
				rayTracing = meshComponent.bRayTracing;
			}
			CoronaECS::PhysicsComponent physicsComponent;
			if (host->GetEntityPhysicsForScript(entity, physicsComponent))
				physicsQuery = physicsComponent.bQueryEnabled;

			ReadTransformDesc(L, tableIndex, position, rotationDegrees, targetExtent, &scale, &useScale, &roughness, &metallic, &overrideMaterial, &visible, &rayTracing, &physicsQuery);
			const bool hasRuntimeField = HasAnyField(L, tableIndex, {
				"roughness", "metallic", "override_material", "overrideMaterial",
				"visible", "ray_tracing", "rayTracing", "physics", "physics_query", "physicsQuery"
			});
			if (hasRuntimeField)
			{
				lua_pushboolean(L, host->SetEntityMeshComponentForScript(
					entity,
					position,
					rotationDegrees,
					targetExtent,
					scale,
					useScale,
					roughness,
					metallic,
					overrideMaterial,
					visible,
					rayTracing,
					physicsQuery) ? 1 : 0);
			}
			else
			{
				lua_pushboolean(L, host->SetEntityMeshTransformForScript(entity, position, rotationDegrees, targetExtent, scale, useScale) ? 1 : 0);
			}
			return 1;
		}

		glm::vec3 position(0.0f);
		host->GetEntityTransformForScript(entity, position);
		glm::vec3 rotationDegrees(0.0f);
		float targetExtent = 1.0f;
		glm::vec3 scale(1.0f);
		bool useScale = false;
		float roughness = 1.0f;
		float metallic = 0.0f;
		bool overrideMaterial = false;
		bool visible = true;
		bool rayTracing = true;
		bool physicsQuery = true;
		ReadTransformDesc(L, tableIndex, position, rotationDegrees, targetExtent, &scale, &useScale, &roughness, &metallic, &overrideMaterial, &visible, &rayTracing, &physicsQuery);

		lua_pushboolean(L, host->AddMeshComponentForScript(
			entity,
			sceneHandle,
			position,
			rotationDegrees,
			targetExtent,
			scale,
			useScale,
			roughness,
			metallic,
			overrideMaterial,
			visible,
			rayTracing,
			physicsQuery) ? 1 : 0);
		return 1;
	}

	int LuaCoronaMeshComponentGet(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const CoronaECS::Entity entity = ReadScriptEntity(L, host, 1);
		CoronaECS::MeshComponent component;
		if (!host->GetEntityMeshForScript(entity, component))
		{
			lua_pushnil(L);
			return 1;
		}

		PushMeshDesc(L, component);
		return 1;
	}

	int LuaCoronaSpineSetPose(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const CoronaECS::Entity entity = ReadScriptEntity(L, host, 1);
		Corona::ScriptSceneHandle sceneHandle = Corona::InvalidScriptSceneHandle;
		if (lua_isnumber(L, 2))
		{
			const lua_Integer rawHandle = lua_tointeger(L, 2);
			if (rawHandle > 0)
				sceneHandle = static_cast<Corona::ScriptSceneHandle>(rawHandle);
		}

		const glm::vec3 position(
			static_cast<float>(luaL_checknumber(L, 3)),
			static_cast<float>(luaL_checknumber(L, 4)),
			static_cast<float>(luaL_checknumber(L, 5)));
		const glm::vec3 rotationDegrees(0.0f, static_cast<float>(luaL_checknumber(L, 6)), 0.0f);
		const float targetHeight = static_cast<float>(luaL_checknumber(L, 7));
		const float roughness = static_cast<float>(luaL_optnumber(L, 8, 1.0));
		const float metallic = static_cast<float>(luaL_optnumber(L, 9, 0.0));
		const bool bMirrorX = lua_gettop(L) >= 10 && lua_toboolean(L, 10) != 0;
		const bool bUseWorldScale = lua_gettop(L) >= 11 && lua_toboolean(L, 11) != 0;
		const bool bRayTracing = lua_gettop(L) >= 12 && lua_toboolean(L, 12) != 0;

		const bool ok = host->SetSpinePoseForScript(
			entity,
			sceneHandle,
			position,
			rotationDegrees,
			targetHeight,
			roughness,
			metallic,
			bMirrorX,
			bUseWorldScale,
			bRayTracing);

		lua_pushboolean(L, ok ? 1 : 0);
		return 1;
	}

	int LuaCoronaCameraComponentSet(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const CoronaECS::Entity entity = ReadScriptEntity(L, host, 1);
		CoronaECS::CameraComponent component;
		host->GetEntityCameraComponentForScript(entity, component);
		if (!lua_istable(L, 2))
		{
			luaL_error(L, "CameraComponent.set expects an entity and descriptor table");
			return 0;
		}

		const int tableIndex = lua_absindex(L, 2);
		glm::vec3 position(0.0f);
		host->GetEntityTransformForScript(entity, position);
		const bool hasPosition = ReadVec3Field(L, tableIndex, "position", position);

		glm::vec3 direction = component.LookDirection;
		if (ReadVec3Field(L, tableIndex, "look_direction", direction) ||
			ReadVec3Field(L, tableIndex, "lookDirection", direction) ||
			ReadVec3Field(L, tableIndex, "direction", direction) ||
			ReadVec3Field(L, tableIndex, "forward", direction))
		{
			if (glm::length(direction) > 0.0001f)
				component.LookDirection = glm::normalize(direction);
		}
		glm::vec3 lookAt(0.0f);
		if (ReadVec3Field(L, tableIndex, "look_at", lookAt) ||
			ReadVec3Field(L, tableIndex, "lookAt", lookAt) ||
			ReadVec3Field(L, tableIndex, "target", lookAt))
		{
			direction = lookAt - position;
			if (glm::length(direction) > 0.0001f)
				component.LookDirection = glm::normalize(direction);
		}
		glm::vec3 up = component.UpDirection;
		if (ReadVec3Field(L, tableIndex, "up_direction", up) ||
			ReadVec3Field(L, tableIndex, "upDirection", up) ||
			ReadVec3Field(L, tableIndex, "up", up))
		{
			if (glm::length(up) > 0.0001f)
				component.UpDirection = glm::normalize(up);
		}
		ReadNumberField(L, tableIndex, "fov", component.Fov);
		if (!ReadNumberField(L, tableIndex, "near_plane", component.NearPlane))
			ReadNumberField(L, tableIndex, "nearPlane", component.NearPlane);
		if (!ReadNumberField(L, tableIndex, "far_plane", component.FarPlane))
			ReadNumberField(L, tableIndex, "farPlane", component.FarPlane);
		ReadBoolField(L, tableIndex, "active", component.bActive);

		bool ok = true;
		glm::vec3 storedAfterTransform(0.0f);
		bool bHasStoredAfterTransform = false;
		if (hasPosition)
		{
			ok = host->SetEntityTransformForScript(entity, position, glm::vec3(0.0f), glm::vec3(1.0f));
			bHasStoredAfterTransform = host->GetEntityTransformForScript(entity, storedAfterTransform);
		}
		ok = host->SetEntityCameraComponentForScript(entity, component) && ok;
		static int cameraSetLogCount = 0;
		if (cameraSetLogCount < 12 && (component.bActive || hasPosition))
		{
			glm::vec3 storedPosition(0.0f);
			const bool bHasStoredPosition = host->GetEntityTransformForScript(entity, storedPosition);
			AppendCpuRuntimeTrace(
				L"[Luau][CameraComponent.set] entity=" + std::to_wstring(entity.GetId()) +
				L", ok=" + std::to_wstring(ok ? 1 : 0) +
				L", active=" + std::to_wstring(component.bActive ? 1 : 0) +
				L", hasPosition=" + std::to_wstring(hasPosition ? 1 : 0) +
				L", inputPosition=(" + std::to_wstring(position.x) +
				L"," + std::to_wstring(position.y) +
				L"," + std::to_wstring(position.z) +
				L"), afterTransform=(" + std::to_wstring(storedAfterTransform.x) +
				L"," + std::to_wstring(storedAfterTransform.y) +
				L"," + std::to_wstring(storedAfterTransform.z) +
				L"), hasAfterTransform=" + std::to_wstring(bHasStoredAfterTransform ? 1 : 0) +
				L", storedPosition=(" + std::to_wstring(storedPosition.x) +
				L"," + std::to_wstring(storedPosition.y) +
				L"," + std::to_wstring(storedPosition.z) +
				L"), hasStored=" + std::to_wstring(bHasStoredPosition ? 1 : 0) +
				L", look=(" + std::to_wstring(component.LookDirection.x) +
				L"," + std::to_wstring(component.LookDirection.y) +
				L"," + std::to_wstring(component.LookDirection.z) + L")");
			++cameraSetLogCount;
		}
		lua_pushboolean(L, ok ? 1 : 0);
		return 1;
	}

	int LuaCoronaCameraComponentGet(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const CoronaECS::Entity entity = ReadScriptEntity(L, host, 1);
		CoronaECS::CameraComponent component;
		if (!host->GetEntityCameraComponentForScript(entity, component))
		{
			lua_pushnil(L);
			return 1;
		}

		PushCameraDesc(L, component);
		return 1;
	}

	int LuaCoronaEntitySetVisible(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const CoronaECS::Entity entity = ReadScriptEntity(L, host, 1);
		const bool visible = luaL_checkboolean(L, 2) != 0;
		lua_pushboolean(L, host->SetEntityVisibilityForScript(entity, visible) ? 1 : 0);
		return 1;
	}

	int LuaCoronaEntitySetRayTracing(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const CoronaECS::Entity entity = ReadScriptEntity(L, host, 1);
		const bool enabled = luaL_checkboolean(L, 2) != 0;
		lua_pushboolean(L, host->SetEntityRayTracingForScript(entity, enabled) ? 1 : 0);
		return 1;
	}

	int LuaCoronaEntityDestroy(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const CoronaECS::Entity entity = ReadScriptEntity(L, host, 1);
		lua_pushboolean(L, host->DestroyEntityForScript(entity) ? 1 : 0);
		return 1;
	}

	int LuaCoronaAttachScript(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const CoronaECS::Entity entity = ReadScriptEntity(L, host, 1);
		if (!entity.IsValid())
		{
			luaL_error(L, "corona.attach_script expects a live entity");
			return 0;
		}

		int updateRef = LUA_REFNIL;
		int shutdownRef = LUA_REFNIL;
		int imguiRef = LUA_REFNIL;
		int uiRef = LUA_REFNIL;

		if (lua_isfunction(L, 2))
		{
			lua_pushvalue(L, 2);
			updateRef = RefStackValueAndPop(L);
		}
		else if (lua_istable(L, 2))
		{
			const int tableIndex = lua_absindex(L, 2);
			updateRef = RefTableFunction(L, tableIndex, "update");
			shutdownRef = RefTableFunction(L, tableIndex, "shutdown");
			imguiRef = RefTableFunction(L, tableIndex, "imgui");
			uiRef = RefTableFunction(L, tableIndex, "ui");
		}
		else
		{
			luaL_error(L, "corona.attach_script expects a function or callback table");
			return 0;
		}

		if (updateRef == LUA_REFNIL && shutdownRef == LUA_REFNIL && imguiRef == LUA_REFNIL && uiRef == LUA_REFNIL)
		{
			lua_pushboolean(L, 0);
			return 1;
		}

		const std::wstring sourceName = L"entity_script:" + std::to_wstring(entity.GetId());
		if (!host->AttachEntityScriptForScript(entity, updateRef, shutdownRef, imguiRef, uiRef, sourceName))
		{
			lua_State* state = L;
			UnrefLuaRef(state, updateRef);
			UnrefLuaRef(state, shutdownRef);
			UnrefLuaRef(state, imguiRef);
			UnrefLuaRef(state, uiRef);
			lua_pushboolean(L, 0);
			return 1;
		}

		lua_pushboolean(L, 1);
		return 1;
	}

	int LuaCoronaAttachScriptFile(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const CoronaECS::Entity entity = ReadScriptEntity(L, host, 1);
		if (!entity.IsValid())
		{
			luaL_error(L, "corona.attach_script_file expects a live entity");
			return 0;
		}

		const char* path = luaL_checkstring(L, 2);
		lua_pushboolean(L, host->AttachEntityScriptFileForScript(entity, Utf8ToWideLocal(path ? path : "")) ? 1 : 0);
		return 1;
	}

	int LuaCoronaAttachNativeScript(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const CoronaECS::Entity entity = ReadScriptEntity(L, host, 1);
		if (!entity.IsValid())
		{
			luaL_error(L, "corona.attach_native_script expects a live entity");
			return 0;
		}

		const char* nativeName = luaL_checkstring(L, 2);
		lua_pushboolean(L, host->AttachNativeEntityScriptForScript(entity, nativeName ? nativeName : "") ? 1 : 0);
		return 1;
	}

	int LuaCoronaHasNativeScript(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const char* nativeName = luaL_checkstring(L, 1);
		lua_pushboolean(L, host->HasNativeEntityScript(nativeName ? nativeName : "") ? 1 : 0);
		return 1;
	}

	int LuaCoronaProfileFunction(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const char* profileName = luaL_checkstring(L, 1);
		if (!lua_isfunction(L, 2))
		{
			luaL_error(L, "corona.profile_function expects a name and function");
			return 0;
		}

		std::wstring sourceName = L"<luau>";
		lua_Debug caller = {};
		if (lua_getinfo(L, 1, "s", &caller) && caller.source)
			sourceName = LuauSourceToWide(caller.source);

		const int originalTop = lua_gettop(L);
		const int argCount = std::max(0, originalTop - 2);
		lua_pushvalue(L, 2);
		for (int argIndex = 0; argIndex < argCount; ++argIndex)
			lua_pushvalue(L, 3 + argIndex);

		const auto callStart = std::chrono::steady_clock::now();
		int result = LUA_OK;
		{
			ScopedLuauScriptProfileExecution profileExecution(host);
			result = lua_pcall(L, argCount, LUA_MULTRET, 0);
		}
		const double elapsedMs = ElapsedMilliseconds(callStart, std::chrono::steady_clock::now());
		host->RecordScriptFunctionProfile(
			sourceName,
			std::string("manual:") + (profileName ? profileName : "<function>"),
			elapsedMs,
			false);

		if (result != 0)
		{
			lua_error(L);
			return 0;
		}

		return lua_gettop(L) - originalTop;
	}

	int LuaCoronaDetachScript(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const CoronaECS::Entity entity = ReadScriptEntity(L, host, 1);
		lua_pushboolean(L, host->DetachEntityScriptForScript(entity) ? 1 : 0);
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
		glm::vec3 scale(1.0f);
		bool useScale = false;

		if (!ReadTransformDesc(L, 2, position, rotationDegrees, targetExtent, &scale, &useScale, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr))
		{
			luaL_error(L, "corona.set_transform expects a descriptor table");
			return 0;
		}

		lua_pushboolean(L, host->SetSceneObjectTransformForScript(handle, position, rotationDegrees, targetExtent, scale, useScale) ? 1 : 0);
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
		glm::vec3 scale(1.0f);
		bool useScale = false;
		if (!host->GetSceneObjectTransformForScript(handle, position, rotationDegrees, targetExtent, scale, useScale))
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
		if (useScale)
		{
			PushVec3(L, scale);
			lua_setfield(L, -2, "scale");
		}
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

	int LuaCoronaSetDefaultWorldVisible(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		lua_pushboolean(L, host->SetDefaultWorldVisibleForScript(luaL_checkboolean(L, 1) != 0) ? 1 : 0);
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

	int LuaCoronaSetGrassBendOrigin(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}
		const float x        = static_cast<float>(luaL_checknumber(L, 1));
		const float y        = static_cast<float>(luaL_checknumber(L, 2));
		const float z        = static_cast<float>(luaL_checknumber(L, 3));
		const float strength = static_cast<float>(luaL_optnumber(L, 4, 20.0));
		host->SetGrassBendOriginForScript(x, y, z, strength);
		return 0;
	}

	int LuaCoronaSetGrassBendParams(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}
		const float radius    = static_cast<float>(luaL_checknumber(L, 1));
		const float maxHeight = static_cast<float>(luaL_optnumber(L, 2, 30.0));
		host->SetGrassBendParamsForScript(radius, maxHeight);
		return 0;
	}

	// corona.use_native_camera(enabled) — switch between the script-owned
	// active camera (orbit, follow, etc.) and the engine's SimpleCamera
	// (free-fly WASD). When enabled=true the active ECS camera is
	// deactivated and bScriptCameraControlEnabled is cleared so m_camera.
	// Update() resumes consuming keyboard input. The Luau script remains
	// responsible for re-activating its camera via CameraComponent.set when
	// the user toggles back.
	// Getters so Luau init paths can synchronize their locals with engine
	// state instead of unconditionally broadcasting defaults (which would
	// clobber values just loaded from a saved map).
	int LuaCoronaGetGrassRenderDistance(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host) { luaL_error(L, "corona host is not available"); return 0; }
		lua_pushnumber(L, host->GetGrassRenderDistanceForScript());
		return 1;
	}

	int LuaCoronaGetWindParams(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host) { luaL_error(L, "corona host is not available"); return 0; }
		const glm::vec4 wp = host->GetRenderFrameWindParamsForScript();
		const glm::vec4 wt = host->GetRenderFrameWindTuningForScript();
		lua_newtable(L);
		lua_pushnumber(L, wp.x); lua_setfield(L, -2, "dir_x");
		lua_pushnumber(L, wp.z); lua_setfield(L, -2, "dir_z");
		lua_pushnumber(L, wp.w); lua_setfield(L, -2, "strength");
		lua_pushnumber(L, wt.x); lua_setfield(L, -2, "temp_freq");
		lua_pushnumber(L, wt.y); lua_setfield(L, -2, "space_freq");
		return 1;
	}

	int LuaCoronaGetGrassBendParams(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host) { luaL_error(L, "corona host is not available"); return 0; }
		const glm::vec4 bp = host->GetRenderFrameGrassBendParamsForScript();
		lua_newtable(L);
		lua_pushnumber(L, bp.x); lua_setfield(L, -2, "radius");
		lua_pushnumber(L, bp.y); lua_setfield(L, -2, "max_height");
		return 1;
	}

	int LuaCoronaUseNativeCamera(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host) { luaL_error(L, "corona host is not available"); return 0; }
		const bool enabled = lua_toboolean(L, 1) != 0;
		host->UseNativeCameraForScript(enabled);
		return 0;
	}

	// corona.particle_burst(x, y, z, count) — spawn `count` spark particles
	// centered at (x, y, z) on the first active particle system. Returns the
	// number actually spawned (may be less than `count` if the pool is full).
	int LuaCoronaParticleBurst(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host) { luaL_error(L, "corona host is not available"); return 0; }
		const float x = static_cast<float>(luaL_checknumber(L, 1));
		const float y = static_cast<float>(luaL_checknumber(L, 2));
		const float z = static_cast<float>(luaL_checknumber(L, 3));
		const int count = static_cast<int>(luaL_optinteger(L, 4, 16));
		const uint32_t spawned = host->ParticleBurstForScript(
			x, y, z, static_cast<uint32_t>((std::max)(0, count)));
		lua_pushinteger(L, static_cast<lua_Integer>(spawned));
		return 1;
	}

	// corona.save_map(name) — writes the script-spawned scene to bin/maps/<name>.crmap
	// Returns true on success, false (+ error string on stack) on failure.
	int LuaCoronaSaveMap(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host) { luaL_error(L, "corona host is not available"); return 0; }
		const std::string name = luaL_checkstring(L, 1);
		std::wstring err;
		const bool ok = host->SaveMapToFile(PlatformUtf8ToWide(name), &err);
		lua_pushboolean(L, ok ? 1 : 0);
		if (!ok) { lua_pushstring(L, PlatformWideToUtf8(err).c_str()); return 2; }
		return 1;
	}

	// corona.load_map(name) — clears current script-spawned scene + replays
	// the named map from bin/maps/<name>.crmap.
	int LuaCoronaLoadMap(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host) { luaL_error(L, "corona host is not available"); return 0; }
		const std::string name = luaL_checkstring(L, 1);
		std::wstring err;
		const bool ok = host->LoadMapFromFile(PlatformUtf8ToWide(name), &err);
		lua_pushboolean(L, ok ? 1 : 0);
		if (!ok) { lua_pushstring(L, PlatformWideToUtf8(err).c_str()); return 2; }
		return 1;
	}

	// corona.map_exists(name) — true if bin/maps/<name>.crmap can be opened.
	int LuaCoronaMapExists(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host) { luaL_error(L, "corona host is not available"); return 0; }
		const std::string name = luaL_checkstring(L, 1);
		const auto path = host->ResolveMapPath(PlatformUtf8ToWide(name));
		lua_pushboolean(L, std::filesystem::exists(path) ? 1 : 0);
		return 1;
	}

	// corona.set_terrain_deform_sphere(x, y, z, radius)
	// World-space sphere whose lower hemisphere is carved out of the terrain
	// mesh in the vertex shader. radius=0 disables. Visual-only — does not
	// affect SampleHeight or PhysX collision.
	int LuaCoronaSetTerrainDeformSphere(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}
		const float x      = static_cast<float>(luaL_checknumber(L, 1));
		const float y      = static_cast<float>(luaL_checknumber(L, 2));
		const float z      = static_cast<float>(luaL_checknumber(L, 3));
		const float radius = static_cast<float>(luaL_checknumber(L, 4));
		host->SetTerrainDeformSphereForScript(x, y, z, std::max(0.0f, radius));
		return 0;
	}

	// corona.set_entity_exclude_deform_sphere(entity, exclude)
	// Opt the given entity's mesh(es) out of the global deform sphere — used
	// to keep the player avatar (which sits at the sphere center) from being
	// carved into its own crater. Boolean arg; defaults to true if omitted.
	int LuaCoronaSetEntityExcludeDeformSphere(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}
		const CoronaECS::Entity entity = ReadScriptEntity(L, host, 1);
		const bool bExclude = lua_isboolean(L, 2) ? (lua_toboolean(L, 2) != 0) : true;
		lua_pushboolean(L, host->SetEntityExcludeFromDeformSphereForScript(entity, bExclude) ? 1 : 0);
		return 1;
	}

	// corona.set_grass_render_distance(d)
	// World units within which a grass chunk is allowed to render. 0 disables
	// distance culling (only frustum cull is applied).
	int LuaCoronaSetGrassRenderDistance(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}
		const float d = static_cast<float>(luaL_checknumber(L, 1));
		host->GrassRenderDistance = std::max(0.0f, d);
		return 0;
	}

	// corona.set_grass_render_origin(x, y, z)
	// World-space point that GrassRenderDistance is measured from. Typically
	// the player position — pass it every frame so the culling tracks motion.
	int LuaCoronaSetGrassRenderOrigin(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}
		host->GrassRenderOrigin = glm::vec3(
			static_cast<float>(luaL_checknumber(L, 1)),
			static_cast<float>(luaL_checknumber(L, 2)),
			static_cast<float>(luaL_checknumber(L, 3)));
		return 0;
	}

	// corona.terrain_sample_height(x, z) → float
	// Returns the active terrain's bilinear-interpolated height at world XZ.
	// 0 if no terrain is active. Useful for placing characters on the surface.
	int LuaCoronaTerrainSampleHeight(lua_State* L)
	{
		Corona* host = GetHost(L);
		Terrain::Component* terrain = host ? host->GetActiveTerrain() : nullptr;
		if (!terrain)
		{
			lua_pushnumber(L, 0.0);
			return 1;
		}
		const float x = static_cast<float>(luaL_checknumber(L, 1));
		const float z = static_cast<float>(luaL_checknumber(L, 2));
		lua_pushnumber(L, terrain->SampleHeight(x, z));
		return 1;
	}

	// corona.set_wind_params(dirX, dirZ, strength, [tempFreq], [spaceFreq])
	// dirX/dirZ are auto-normalized in the XZ plane. strength = 0 disables
	// wind. tempFreq/spaceFreq default to 0 → "keep current tuning".
	int LuaCoronaSetWindParams(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}
		float dirX     = static_cast<float>(luaL_checknumber(L, 1));
		float dirZ     = static_cast<float>(luaL_checknumber(L, 2));
		float strength = static_cast<float>(luaL_checknumber(L, 3));
		const float tempFreq  = static_cast<float>(luaL_optnumber(L, 4, 0.0));
		const float spaceFreq = static_cast<float>(luaL_optnumber(L, 5, 0.0));
		const float lenSq = dirX * dirX + dirZ * dirZ;
		if (lenSq > 1e-6f)
		{
			const float inv = 1.0f / std::sqrt(lenSq);
			dirX *= inv;
			dirZ *= inv;
		}
		host->SetWindParamsForScript(dirX, dirZ, strength, tempFreq, spaceFreq);
		return 0;
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

	void PushGamepadButton(lua_State* L, const char* name, uint16_t mask, uint16_t down, uint16_t pressed, uint16_t released)
	{
		lua_pushboolean(L, (down & mask) != 0);
		lua_setfield(L, -2, name);

		std::string pressedName = std::string(name) + "_pressed";
		lua_pushboolean(L, (pressed & mask) != 0);
		lua_setfield(L, -2, pressedName.c_str());

		std::string releasedName = std::string(name) + "_released";
		lua_pushboolean(L, (released & mask) != 0);
		lua_setfield(L, -2, releasedName.c_str());
	}

	int LuaCoronaGetGamepad(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		bool connected = false;
		float leftX = 0.0f;
		float leftY = 0.0f;
		float rightX = 0.0f;
		float rightY = 0.0f;
		float leftTrigger = 0.0f;
		float rightTrigger = 0.0f;
		uint16_t buttonsDown = 0;
		uint16_t buttonsPressed = 0;
		uint16_t buttonsReleased = 0;
		host->GetScriptGamepadForScript(
			connected,
			leftX,
			leftY,
			rightX,
			rightY,
			leftTrigger,
			rightTrigger,
			buttonsDown,
			buttonsPressed,
			buttonsReleased);

		lua_newtable(L);
		lua_pushboolean(L, connected ? 1 : 0);
		lua_setfield(L, -2, "connected");
		lua_pushnumber(L, leftX);
		lua_setfield(L, -2, "left_x");
		lua_pushnumber(L, leftY);
		lua_setfield(L, -2, "left_y");
		lua_pushnumber(L, rightX);
		lua_setfield(L, -2, "right_x");
		lua_pushnumber(L, rightY);
		lua_setfield(L, -2, "right_y");
		lua_pushnumber(L, leftTrigger);
		lua_setfield(L, -2, "left_trigger");
		lua_pushnumber(L, rightTrigger);
		lua_setfield(L, -2, "right_trigger");
		lua_pushinteger(L, buttonsDown);
		lua_setfield(L, -2, "buttons");

		PushGamepadButton(L, "a", PlatformGamepadButton::A, buttonsDown, buttonsPressed, buttonsReleased);
		PushGamepadButton(L, "b", PlatformGamepadButton::B, buttonsDown, buttonsPressed, buttonsReleased);
		PushGamepadButton(L, "x", PlatformGamepadButton::X, buttonsDown, buttonsPressed, buttonsReleased);
		PushGamepadButton(L, "y", PlatformGamepadButton::Y, buttonsDown, buttonsPressed, buttonsReleased);
		PushGamepadButton(L, "left_shoulder", PlatformGamepadButton::LeftShoulder, buttonsDown, buttonsPressed, buttonsReleased);
		PushGamepadButton(L, "right_shoulder", PlatformGamepadButton::RightShoulder, buttonsDown, buttonsPressed, buttonsReleased);
		PushGamepadButton(L, "back", PlatformGamepadButton::Back, buttonsDown, buttonsPressed, buttonsReleased);
		PushGamepadButton(L, "start", PlatformGamepadButton::Start, buttonsDown, buttonsPressed, buttonsReleased);
		PushGamepadButton(L, "left_thumb", PlatformGamepadButton::LeftThumb, buttonsDown, buttonsPressed, buttonsReleased);
		PushGamepadButton(L, "right_thumb", PlatformGamepadButton::RightThumb, buttonsDown, buttonsPressed, buttonsReleased);
		PushGamepadButton(L, "dpad_up", PlatformGamepadButton::DPadUp, buttonsDown, buttonsPressed, buttonsReleased);
		PushGamepadButton(L, "dpad_down", PlatformGamepadButton::DPadDown, buttonsDown, buttonsPressed, buttonsReleased);
		PushGamepadButton(L, "dpad_left", PlatformGamepadButton::DPadLeft, buttonsDown, buttonsPressed, buttonsReleased);
		PushGamepadButton(L, "dpad_right", PlatformGamepadButton::DPadRight, buttonsDown, buttonsPressed, buttonsReleased);
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

		const char* mode = lua_isstring(L, 1) ? lua_tostring(L, 1) : "";
		host->PushLuauUiStateForScript(L, mode ? mode : "");
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

	int LuaUiOverlayLine(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const float x0 = static_cast<float>(luaL_checknumber(L, 1));
		const float y0 = static_cast<float>(luaL_checknumber(L, 2));
		const float x1 = static_cast<float>(luaL_checknumber(L, 3));
		const float y1 = static_cast<float>(luaL_checknumber(L, 4));
		const glm::vec4 color = ReadColorArg(L, 5, glm::vec4(1.0f));
		const float thickness = static_cast<float>(luaL_optnumber(L, 6, 1.0));
		host->QueueScriptUiOverlayLineForScript(x0, y0, x1, y1, color, thickness);
		return 0;
	}

	int LuaUiOverlayRect(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const float x = static_cast<float>(luaL_checknumber(L, 1));
		const float y = static_cast<float>(luaL_checknumber(L, 2));
		const float width = static_cast<float>(luaL_checknumber(L, 3));
		const float height = static_cast<float>(luaL_checknumber(L, 4));
		const glm::vec4 color = ReadColorArg(L, 5, glm::vec4(1.0f));
		const float thickness = static_cast<float>(luaL_optnumber(L, 6, 1.0));
		const float rounding = static_cast<float>(luaL_optnumber(L, 7, 0.0));
		host->QueueScriptUiOverlayRectForScript(x, y, width, height, color, thickness, rounding);
		return 0;
	}

	int LuaUiOverlayRectFilled(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const float x = static_cast<float>(luaL_checknumber(L, 1));
		const float y = static_cast<float>(luaL_checknumber(L, 2));
		const float width = static_cast<float>(luaL_checknumber(L, 3));
		const float height = static_cast<float>(luaL_checknumber(L, 4));
		const glm::vec4 color = ReadColorArg(L, 5, glm::vec4(0.0f, 0.0f, 0.0f, 0.65f));
		const float rounding = static_cast<float>(luaL_optnumber(L, 6, 0.0));
		host->QueueScriptUiOverlayRectFilledForScript(x, y, width, height, color, rounding);
		return 0;
	}

	int LuaUiOverlayButton(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const char* id = luaL_checkstring(L, 1);
		const char* label = luaL_checkstring(L, 2);
		const float x = static_cast<float>(luaL_checknumber(L, 3));
		const float y = static_cast<float>(luaL_checknumber(L, 4));
		const float width = static_cast<float>(luaL_checknumber(L, 5));
		const float height = static_cast<float>(luaL_checknumber(L, 6));
		const glm::vec4 fillColor = ReadColorArg(L, 7, glm::vec4(0.12f, 0.18f, 0.24f, 0.92f));
		const glm::vec4 hoverColor = ReadColorArg(L, 8, glm::vec4(0.18f, 0.38f, 0.48f, 0.96f));
		const glm::vec4 borderColor = ReadColorArg(L, 9, glm::vec4(0.80f, 0.92f, 1.0f, 0.88f));
		const float rounding = static_cast<float>(luaL_optnumber(L, 10, 6.0));
		lua_pushboolean(
			L,
			host->QueueScriptUiOverlayButtonForScript(
				id ? id : "",
				label ? label : "",
				x,
				y,
				width,
				height,
				fillColor,
				hoverColor,
				borderColor,
				rounding) ? 1 : 0);
		return 1;
	}

	int LuaUiOverlayProgressBar(lua_State* L)
	{
		Corona* host = GetHost(L);
		if (!host)
		{
			luaL_error(L, "corona host is not available");
			return 0;
		}

		const char* id = luaL_checkstring(L, 1);
		const float x = static_cast<float>(luaL_checknumber(L, 2));
		const float y = static_cast<float>(luaL_checknumber(L, 3));
		const float width = static_cast<float>(luaL_checknumber(L, 4));
		const float height = static_cast<float>(luaL_checknumber(L, 5));
		const float fraction = static_cast<float>(luaL_checknumber(L, 6));
		const char* label = (lua_gettop(L) >= 7 && !lua_isnil(L, 7)) ? luaL_checkstring(L, 7) : "";
		const glm::vec4 fillColor = ReadColorArg(L, 8, glm::vec4(0.20f, 0.78f, 0.32f, 0.95f));
		const glm::vec4 backgroundColor = ReadColorArg(L, 9, glm::vec4(0.08f, 0.08f, 0.09f, 0.75f));
		const glm::vec4 borderColor = ReadColorArg(L, 10, glm::vec4(1.0f, 1.0f, 1.0f, 0.75f));
		host->QueueScriptUiOverlayProgressBarForScript(
			id ? id : "",
			x,
			y,
			width,
			height,
			fraction,
			label ? label : "",
			fillColor,
			backgroundColor,
			borderColor);
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

	int LuaUiWorldText(lua_State* L)
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
			luaL_error(L, "ui.world_text expects a vec3 table as its second argument");
			return 0;
		}
		const char* text = luaL_checkstring(L, 3);
		const float xOffset = static_cast<float>(luaL_optnumber(L, 4, 0.0));
		const float yOffset = static_cast<float>(luaL_optnumber(L, 5, 0.0));
		const glm::vec4 color = ReadColorArg(L, 6, glm::vec4(1.0f));
		host->QueueScriptUiWorldTextForScript(id ? id : "", position, text ? text : "", xOffset, yOffset, color);
		return 0;
	}

	int LuaUiWorldProgressBar(lua_State* L)
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
			luaL_error(L, "ui.world_progress_bar expects a vec3 table as its second argument");
			return 0;
		}
		const float fraction = static_cast<float>(luaL_optnumber(L, 3, 1.0));
		const char* label = (lua_gettop(L) >= 4 && !lua_isnil(L, 4)) ? luaL_checkstring(L, 4) : "";
		const float width = static_cast<float>(luaL_optnumber(L, 5, 72.0));
		const float height = static_cast<float>(luaL_optnumber(L, 6, 8.0));
		const glm::vec4 fillColor = ReadColorArg(L, 7, glm::vec4(0.20f, 0.78f, 0.32f, 0.95f));
		const glm::vec4 backgroundColor = ReadColorArg(L, 8, glm::vec4(0.08f, 0.08f, 0.09f, 0.75f));
		const glm::vec4 borderColor = ReadColorArg(L, 9, glm::vec4(1.0f, 1.0f, 1.0f, 0.75f));
		host->QueueScriptUiWorldProgressBarForScript(
			id ? id : "",
			position,
			fraction,
			label ? label : "",
			width,
			height,
			fillColor,
			backgroundColor,
			borderColor);
		return 0;
	}

	int LuaUiWorldHealthBar(lua_State* L)
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
			luaL_error(L, "ui.world_health_bar expects a vec3 table as its second argument");
			return 0;
		}
		const float fraction = static_cast<float>(luaL_optnumber(L, 3, 1.0));
		const char* label = (lua_gettop(L) >= 4 && !lua_isnil(L, 4)) ? luaL_checkstring(L, 4) : "";
		const float width = static_cast<float>(luaL_optnumber(L, 5, 72.0));
		const float height = static_cast<float>(luaL_optnumber(L, 6, 8.0));
		host->QueueScriptUiWorldHealthBarForScript(
			id ? id : "",
			position,
			fraction,
			label ? label : "",
			width,
			height);
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
		lua_pushcfunction(L, LuaCoronaRequestExit, "corona.request_exit");
		lua_setfield(L, -2, "request_exit");

		lua_newtable(L);
		lua_pushcfunction(L, LuaCoronaEntityCreate, "corona.Entity.create");
		lua_setfield(L, -2, "create");
		lua_pushcfunction(L, LuaCoronaEntityDestroy, "corona.Entity.destroy");
		lua_setfield(L, -2, "destroy");
		lua_pushcfunction(L, LuaCoronaEntityExists, "corona.Entity.exists");
		lua_setfield(L, -2, "exists");
		lua_pushcfunction(L, LuaCoronaEntityByName, "corona.Entity.by_name");
		lua_setfield(L, -2, "by_name");
		lua_pushcfunction(L, LuaCoronaGetWorldEntity, "corona.Entity.world");
		lua_setfield(L, -2, "world");
		lua_pushcfunction(L, LuaCoronaGetLevelEntity, "corona.Entity.level");
		lua_setfield(L, -2, "level");
		lua_pushcfunction(L, LuaCoronaGetMainCameraEntity, "corona.Entity.main_camera");
		lua_setfield(L, -2, "main_camera");
		lua_pushcfunction(L, LuaCoronaGetMainDirectionalLightEntity, "corona.Entity.main_directional_light");
		lua_setfield(L, -2, "main_directional_light");
		lua_pushcfunction(L, LuaCoronaGetEntity, "corona.Entity.from_scene_object");
		lua_setfield(L, -2, "from_scene_object");
		lua_pushcfunction(L, LuaCoronaGetEntityObject, "corona.Entity.scene_object");
		lua_setfield(L, -2, "scene_object");
		lua_setfield(L, -2, "Entity");

		lua_newtable(L);
		lua_pushcfunction(L, LuaCoronaEntitySetTransform, "corona.TransformComponent.set");
		lua_setfield(L, -2, "set");
		lua_pushcfunction(L, LuaCoronaEntitySetTransform, "corona.TransformComponent.add");
		lua_setfield(L, -2, "add");
		lua_pushcfunction(L, LuaCoronaEntityGetTransform, "corona.TransformComponent.get");
		lua_setfield(L, -2, "get");
		lua_setfield(L, -2, "TransformComponent");

		lua_newtable(L);
		lua_pushcfunction(L, LuaCoronaMeshComponentAdd, "corona.MeshComponent.add");
		lua_setfield(L, -2, "add");
		lua_pushcfunction(L, LuaCoronaMeshComponentAdd, "corona.MeshComponent.set");
		lua_setfield(L, -2, "set");
		lua_pushcfunction(L, LuaCoronaMeshComponentGet, "corona.MeshComponent.get");
		lua_setfield(L, -2, "get");
		lua_pushcfunction(L, LuaCoronaEntitySetVisible, "corona.MeshComponent.set_visible");
		lua_setfield(L, -2, "set_visible");
		lua_pushcfunction(L, LuaCoronaEntitySetRayTracing, "corona.MeshComponent.set_ray_tracing");
		lua_setfield(L, -2, "set_ray_tracing");
		lua_setfield(L, -2, "MeshComponent");

		lua_newtable(L);
		lua_pushcfunction(L, LuaCoronaCreateSpineScene, "corona.SpineComponent.create_scene");
		lua_setfield(L, -2, "create_scene");
		lua_pushcfunction(L, LuaCoronaCreateSpineScene, "corona.SpineComponent.sample_scene");
		lua_setfield(L, -2, "sample_scene");
		lua_pushcfunction(L, LuaCoronaGetSpineSceneHeight, "corona.SpineComponent.get_scene_height");
		lua_setfield(L, -2, "get_scene_height");
		lua_pushcfunction(L, LuaCoronaSpineSetPose, "corona.SpineComponent.set_pose");
		lua_setfield(L, -2, "set_pose");
		lua_pushcfunction(L, LuaCoronaCreateLiveSpine, "corona.SpineComponent.create_live");
		lua_setfield(L, -2, "create_live");
		lua_pushcfunction(L, LuaCoronaUpdateLiveSpine, "corona.SpineComponent.update_live");
		lua_setfield(L, -2, "update_live");
		lua_pushcfunction(L, LuaCoronaDestroyLiveSpine, "corona.SpineComponent.destroy_live");
		lua_setfield(L, -2, "destroy_live");
		lua_setfield(L, -2, "SpineComponent");

		lua_newtable(L);
		lua_pushcfunction(L, LuaCoronaCreateSpineScene, "corona.Spine.create_scene");
		lua_setfield(L, -2, "create_scene");
		lua_pushcfunction(L, LuaCoronaCreateSpineScene, "corona.Spine.sample_scene");
		lua_setfield(L, -2, "sample_scene");
		lua_pushcfunction(L, LuaCoronaGetSpineSceneHeight, "corona.Spine.get_scene_height");
		lua_setfield(L, -2, "get_scene_height");
		lua_pushcfunction(L, LuaCoronaSpineSetPose, "corona.Spine.set_pose");
		lua_setfield(L, -2, "set_pose");
		lua_setfield(L, -2, "Spine");

		lua_newtable(L);
		lua_pushcfunction(L, LuaCoronaEntitySetPhysics, "corona.PhysicsComponent.set");
		lua_setfield(L, -2, "set");
		lua_pushcfunction(L, LuaCoronaEntitySetPhysics, "corona.PhysicsComponent.add");
		lua_setfield(L, -2, "add");
		lua_pushcfunction(L, LuaCoronaEntityGetPhysics, "corona.PhysicsComponent.get");
		lua_setfield(L, -2, "get");
		lua_setfield(L, -2, "PhysicsComponent");

		lua_newtable(L);
		lua_pushcfunction(L, LuaCoronaEntitySetLight, "corona.LightComponent.set");
		lua_setfield(L, -2, "set");
		lua_pushcfunction(L, LuaCoronaEntitySetLight, "corona.LightComponent.add");
		lua_setfield(L, -2, "add");
		lua_pushcfunction(L, LuaCoronaEntityGetLight, "corona.LightComponent.get");
		lua_setfield(L, -2, "get");
		lua_pushcfunction(L, LuaCoronaPointLightSpawn, "corona.LightComponent.spawn_point");
		lua_setfield(L, -2, "spawn_point");
		lua_setfield(L, -2, "LightComponent");

		lua_newtable(L);
		lua_pushcfunction(L, LuaCoronaCameraComponentSet, "corona.CameraComponent.set");
		lua_setfield(L, -2, "set");
		lua_pushcfunction(L, LuaCoronaCameraComponentSet, "corona.CameraComponent.add");
		lua_setfield(L, -2, "add");
		lua_pushcfunction(L, LuaCoronaCameraComponentGet, "corona.CameraComponent.get");
		lua_setfield(L, -2, "get");
		lua_setfield(L, -2, "CameraComponent");

		lua_newtable(L);
		lua_pushcfunction(L, LuaCoronaAttachScript, "corona.ScriptComponent.attach");
		lua_setfield(L, -2, "attach");
		lua_pushcfunction(L, LuaCoronaAttachScriptFile, "corona.ScriptComponent.attach_file");
		lua_setfield(L, -2, "attach_file");
		lua_pushcfunction(L, LuaCoronaAttachNativeScript, "corona.ScriptComponent.attach_native");
		lua_setfield(L, -2, "attach_native");
		lua_pushcfunction(L, LuaCoronaHasNativeScript, "corona.ScriptComponent.has_native");
		lua_setfield(L, -2, "has_native");
		lua_pushcfunction(L, LuaCoronaDetachScript, "corona.ScriptComponent.detach");
		lua_setfield(L, -2, "detach");
		lua_setfield(L, -2, "ScriptComponent");

		lua_pushcfunction(L, LuaCoronaGetEntity, "corona.get_entity");
		lua_setfield(L, -2, "get_entity");
		lua_pushcfunction(L, LuaCoronaGetEntityObject, "corona.entity_get_object");
		lua_setfield(L, -2, "entity_get_object");
		lua_pushcfunction(L, LuaCoronaGetMainCameraEntity, "corona.get_main_camera_entity");
		lua_setfield(L, -2, "get_main_camera_entity");
		lua_pushcfunction(L, LuaCoronaGetWorldEntity, "corona.get_world_entity");
		lua_setfield(L, -2, "get_world_entity");
		lua_pushcfunction(L, LuaCoronaGetLevelEntity, "corona.get_level_entity");
		lua_setfield(L, -2, "get_level_entity");
		lua_pushcfunction(L, LuaCoronaGetMainDirectionalLightEntity, "corona.get_main_directional_light_entity");
		lua_setfield(L, -2, "get_main_directional_light_entity");
		lua_pushcfunction(L, LuaCoronaEntityExists, "corona.entity_exists");
		lua_setfield(L, -2, "entity_exists");
		lua_pushcfunction(L, LuaCoronaEntitySetTransform, "corona.entity_set_transform");
		lua_setfield(L, -2, "entity_set_transform");
		lua_pushcfunction(L, LuaCoronaEntityGetTransform, "corona.entity_get_transform");
		lua_setfield(L, -2, "entity_get_transform");
		lua_pushcfunction(L, LuaCoronaEntitySetPhysics, "corona.entity_set_physics");
		lua_setfield(L, -2, "entity_set_physics");
		lua_pushcfunction(L, LuaCoronaEntityGetPhysics, "corona.entity_get_physics");
		lua_setfield(L, -2, "entity_get_physics");
		lua_pushcfunction(L, LuaCoronaEntitySetLight, "corona.entity_set_light");
		lua_setfield(L, -2, "entity_set_light");
		lua_pushcfunction(L, LuaCoronaEntityGetLight, "corona.entity_get_light");
		lua_setfield(L, -2, "entity_get_light");
		lua_pushcfunction(L, LuaCoronaEntitySetVisible, "corona.entity_set_visible");
		lua_setfield(L, -2, "entity_set_visible");
		lua_pushcfunction(L, LuaCoronaEntitySetRayTracing, "corona.entity_set_ray_tracing");
		lua_setfield(L, -2, "entity_set_ray_tracing");
		lua_pushcfunction(L, LuaCoronaEntityDestroy, "corona.entity_destroy");
		lua_setfield(L, -2, "entity_destroy");
		lua_pushcfunction(L, LuaCoronaAttachScript, "corona.attach_script");
		lua_setfield(L, -2, "attach_script");
		lua_pushcfunction(L, LuaCoronaAttachScriptFile, "corona.attach_script_file");
		lua_setfield(L, -2, "attach_script_file");
		lua_pushcfunction(L, LuaCoronaAttachNativeScript, "corona.attach_native_script");
		lua_setfield(L, -2, "attach_native_script");
		lua_pushcfunction(L, LuaCoronaHasNativeScript, "corona.has_native_script");
		lua_setfield(L, -2, "has_native_script");
		lua_pushcfunction(L, LuaCoronaDetachScript, "corona.detach_script");
		lua_setfield(L, -2, "detach_script");
		lua_pushcfunction(L, LuaCoronaCreateSpineScene, "corona.create_spine_scene");
		lua_setfield(L, -2, "create_spine_scene");
		lua_pushcfunction(L, LuaCoronaGetSpineSceneHeight, "corona.get_spine_scene_height");
		lua_setfield(L, -2, "get_spine_scene_height");
		lua_pushcfunction(L, LuaCoronaSetDefaultWorldVisible, "corona.set_default_world_visible");
		lua_setfield(L, -2, "set_default_world_visible");
		lua_pushcfunction(L, LuaCoronaSetCameraControl, "corona.set_camera_control");
		lua_setfield(L, -2, "set_camera_control");
		lua_pushcfunction(L, LuaCoronaSetGrassBendOrigin, "corona.set_grass_bend_origin");
		lua_setfield(L, -2, "set_grass_bend_origin");
		lua_pushcfunction(L, LuaCoronaParticleBurst, "corona.particle_burst");
		lua_setfield(L, -2, "particle_burst");
		lua_pushcfunction(L, LuaCoronaUseNativeCamera, "corona.use_native_camera");
		lua_setfield(L, -2, "use_native_camera");
		lua_pushcfunction(L, LuaCoronaGetGrassRenderDistance, "corona.get_grass_render_distance");
		lua_setfield(L, -2, "get_grass_render_distance");
		lua_pushcfunction(L, LuaCoronaGetWindParams, "corona.get_wind_params");
		lua_setfield(L, -2, "get_wind_params");
		lua_pushcfunction(L, LuaCoronaGetGrassBendParams, "corona.get_grass_bend_params");
		lua_setfield(L, -2, "get_grass_bend_params");
		lua_pushcfunction(L, LuaCoronaSetGrassBendParams, "corona.set_grass_bend_params");
		lua_setfield(L, -2, "set_grass_bend_params");
		lua_pushcfunction(L, LuaCoronaSetTerrainDeformSphere, "corona.set_terrain_deform_sphere");
		lua_setfield(L, -2, "set_terrain_deform_sphere");
		lua_pushcfunction(L, LuaCoronaSetEntityExcludeDeformSphere, "corona.set_entity_exclude_deform_sphere");
		lua_setfield(L, -2, "set_entity_exclude_deform_sphere");
		lua_pushcfunction(L, LuaCoronaSaveMap, "corona.save_map");
		lua_setfield(L, -2, "save_map");
		lua_pushcfunction(L, LuaCoronaLoadMap, "corona.load_map");
		lua_setfield(L, -2, "load_map");
		lua_pushcfunction(L, LuaCoronaMapExists, "corona.map_exists");
		lua_setfield(L, -2, "map_exists");
		lua_pushcfunction(L, LuaCoronaSetWindParams, "corona.set_wind_params");
		lua_setfield(L, -2, "set_wind_params");
		lua_pushcfunction(L, LuaCoronaTerrainSampleHeight, "corona.terrain_sample_height");
		lua_setfield(L, -2, "terrain_sample_height");
		lua_pushcfunction(L, LuaCoronaSetGrassRenderDistance, "corona.set_grass_render_distance");
		lua_setfield(L, -2, "set_grass_render_distance");
		lua_pushcfunction(L, LuaCoronaSetGrassRenderOrigin, "corona.set_grass_render_origin");
		lua_setfield(L, -2, "set_grass_render_origin");
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
		lua_pushcfunction(L, LuaCoronaGetGamepad, "corona.get_gamepad");
		lua_setfield(L, -2, "get_gamepad");
		lua_pushcfunction(L, LuaCoronaGetUiState, "corona.get_ui_state");
		lua_setfield(L, -2, "get_ui_state");
		lua_pushcfunction(L, LuaCoronaProfileFunction, "corona.profile_function");
		lua_setfield(L, -2, "profile_function");
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
		lua_pushcfunction(L, LuaUiOverlayLine, "ui.overlay_line");
		lua_setfield(L, -2, "overlay_line");
		lua_pushcfunction(L, LuaUiOverlayRect, "ui.overlay_rect");
		lua_setfield(L, -2, "overlay_rect");
		lua_pushcfunction(L, LuaUiOverlayRectFilled, "ui.overlay_rect_filled");
		lua_setfield(L, -2, "overlay_rect_filled");
		lua_pushcfunction(L, LuaUiOverlayButton, "ui.overlay_button");
		lua_setfield(L, -2, "overlay_button");
		lua_pushcfunction(L, LuaUiOverlayProgressBar, "ui.progress_bar");
		lua_setfield(L, -2, "progress_bar");
		lua_pushcfunction(L, LuaUiOverlayProgressBar, "ui.overlay_progress_bar");
		lua_setfield(L, -2, "overlay_progress_bar");
		lua_pushcfunction(L, LuaUiWorldAxis, "ui.world_axis");
		lua_setfield(L, -2, "world_axis");
		lua_pushcfunction(L, LuaUiWorldText, "ui.world_text");
		lua_setfield(L, -2, "world_text");
		lua_pushcfunction(L, LuaUiWorldProgressBar, "ui.world_progress_bar");
		lua_setfield(L, -2, "world_progress_bar");
		lua_pushcfunction(L, LuaUiWorldHealthBar, "ui.world_health_bar");
		lua_setfield(L, -2, "world_health_bar");
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

	float SanitizeScriptScaleAxis(float value)
	{
		if (!std::isfinite(value))
			return 1.0f;

		const float sign = value < 0.0f ? -1.0f : 1.0f;
		return sign * std::max(std::abs(value), 0.001f);
	}

	glm::vec3 SanitizeScriptScale(const glm::vec3& scale)
	{
		return glm::vec3(
			SanitizeScriptScaleAxis(scale.x),
			SanitizeScriptScaleAxis(scale.y),
			SanitizeScriptScaleAxis(scale.z));
	}

	float ComputeSceneSourceHeight(const shared_ptr<Scene>& scene)
	{
		if (!scene || !scene->bHasBounds)
			return 0.0f;

		const glm::vec3 boundsSize = scene->BoundsMax - scene->BoundsMin;
		float sourceHeight = boundsSize.y;
		if (sourceHeight <= 1.0e-4f)
			sourceHeight = std::max(std::max(boundsSize.x, boundsSize.y), boundsSize.z);
		return sourceHeight;
	}

	float ComputeSceneTargetHeightScale(const shared_ptr<Scene>& scene, float targetHeight)
	{
		const float sourceHeight = ComputeSceneSourceHeight(scene);
		return sourceHeight > 1.0e-4f ? std::max(targetHeight, 0.001f) / sourceHeight : 1.0f;
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
	{
		SceneRecipe& r = ScriptScenes[handle].Recipe;
		r.RecipeKind = SceneRecipe::Kind::BlockCharacter;
		r.Seed = seed;
	}
	ScriptSceneByPath[key] = handle;
	AppendCpuRuntimeTrace(L"[Luau][MeshComponent] procedural_block_character handle=" + std::to_wstring(handle) + L" seed=" + std::to_wstring(seed));
	return handle;
}

Corona::ScriptSceneHandle Corona::CreateProceduralBoxSceneForScript(const glm::vec3& baseColor, bool bUseBrickTexture, float uvRepeat, const std::wstring& textureKind, float uvRepeatY, bool bFrontOnly)
{
	if (!renderBackend)
		return InvalidScriptSceneHandle;

	const std::string normalizedTextureKindUtf8 = NormalizeProceduralBoxTextureKindForScript(WideToUtf8Local(textureKind));
	const std::wstring normalizedTextureKind = Utf8ToWideLocal(normalizedTextureKindUtf8);
	const std::wstring textureKey = normalizedTextureKind.empty()
		? (bUseBrickTexture ? L"brick" : L"flat")
		: normalizedTextureKind;
	const glm::ivec3 quantizedColor = glm::clamp(
		glm::ivec3(glm::round(glm::clamp(baseColor, glm::vec3(0.0f), glm::vec3(1.0f)) * 255.0f)),
		glm::ivec3(0),
		glm::ivec3(255));
	const int quantizedUvRepeatX = std::clamp(static_cast<int>(std::round(std::clamp(uvRepeat, 1.0f, 64.0f) * 100.0f)), 100, 6400);
	const int quantizedUvRepeatY = std::clamp(static_cast<int>(std::round(std::clamp(uvRepeatY > 0.0f ? uvRepeatY : uvRepeat, 1.0f, 64.0f) * 100.0f)), 100, 6400);
	const std::wstring key =
		L"procedural://box/" +
		std::to_wstring(quantizedColor.x) + L"/" +
		std::to_wstring(quantizedColor.y) + L"/" +
		std::to_wstring(quantizedColor.z) + L"/" +
		(bFrontOnly ? L"front/" : L"solid/") +
		textureKey + L"/" +
		std::to_wstring(quantizedUvRepeatX) + L"/" +
		std::to_wstring(quantizedUvRepeatY);
	const auto cachedIt = ScriptSceneByPath.find(key);
	if (cachedIt != ScriptSceneByPath.end())
		return cachedIt->second;

	shared_ptr<Scene> scene = CreateProceduralBoxScene(
		glm::vec3(quantizedColor) / 255.0f,
		bUseBrickTexture,
		static_cast<float>(quantizedUvRepeatX) / 100.0f,
		normalizedTextureKind,
		static_cast<float>(quantizedUvRepeatY) / 100.0f,
		bFrontOnly);
	if (!scene)
		return InvalidScriptSceneHandle;

	ScriptSceneHandle handle = NextScriptSceneHandle++;
	if (handle == InvalidScriptSceneHandle)
		handle = NextScriptSceneHandle++;

	ScriptScenes[handle] = { scene, key, EPhysicsCollisionShape::Box, glm::vec3(0.5f) };
	ScriptSceneByPath[key] = handle;
	AppendCpuRuntimeTrace(L"[Luau][MeshComponent] procedural_box handle=" + std::to_wstring(handle) + L" key=" + key);
	return handle;
}

Corona::ScriptSceneHandle Corona::CreateProceduralGrassSceneForScript(UINT32 numBlades, float areaSize, float bladeHeight, UINT32 seed, UINT32 bladeSegments)
{
	if (!renderBackend)
		return InvalidScriptSceneHandle;

	const UINT32 clampedBlades = std::clamp<UINT32>(numBlades, 1u, 200000u);
	const float clampedArea    = std::clamp(areaSize, 10.0f, 100000.0f);
	const float clampedHeight  = std::clamp(bladeHeight, 1.0f, 1000.0f);
	const UINT32 normalizedSeed = (seed == 0u) ? 1u : seed;
	const UINT32 clampedSegments = std::clamp<UINT32>(bladeSegments == 0u ? 4u : bladeSegments, 1u, 32u);

	const std::wstring key =
		L"procedural://grass/" +
		std::to_wstring(clampedBlades) + L"/" +
		std::to_wstring(static_cast<int>(std::round(clampedArea))) + L"/" +
		std::to_wstring(static_cast<int>(std::round(clampedHeight))) + L"/" +
		std::to_wstring(normalizedSeed) + L"/s" + std::to_wstring(clampedSegments);
	const auto cachedIt = ScriptSceneByPath.find(key);
	if (cachedIt != ScriptSceneByPath.end())
		return cachedIt->second;

	shared_ptr<Scene> scene = CreateProceduralGrassScene(clampedBlades, clampedArea, clampedHeight, normalizedSeed, clampedSegments);
	if (!scene)
		return InvalidScriptSceneHandle;

	ScriptSceneHandle handle = NextScriptSceneHandle++;
	if (handle == InvalidScriptSceneHandle)
		handle = NextScriptSceneHandle++;

	ScriptScenes[handle] = { scene, key, EPhysicsCollisionShape::TriangleMesh, glm::vec3(0.5f) };
	{
		SceneRecipe& r = ScriptScenes[handle].Recipe;
		r.RecipeKind = SceneRecipe::Kind::Grass;
		r.BladeCount = clampedBlades;
		r.AreaSize = clampedArea;
		r.BladeHeight = clampedHeight;
		r.Seed = normalizedSeed;
		r.BladeSegments = clampedSegments;
	}
	ScriptSceneByPath[key] = handle;
	AppendCpuRuntimeTrace(
		L"[Luau][MeshComponent] procedural_grass handle=" + std::to_wstring(handle) +
		L" blades=" + std::to_wstring(clampedBlades) +
		L" area=" + std::to_wstring(static_cast<int>(std::round(clampedArea))) +
		L" height=" + std::to_wstring(static_cast<int>(std::round(clampedHeight))));
	return handle;
}

Corona::ScriptSceneHandle Corona::CreateProceduralGrassOnTerrainSceneForScript(UINT32 numBlades, float bladeHeight, UINT32 seed, UINT32 bladeSegments)
{
	if (!renderBackend)
		return InvalidScriptSceneHandle;

	const UINT32 clampedBlades = std::clamp<UINT32>(numBlades, 1u, 20000000u);
	// Allow sub-meter blades (terrain_demo uses 0.6 m); upper bound stays
	// for the legacy giant grass demo at 28 m.
	const float clampedHeight  = std::clamp(bladeHeight, 0.01f, 1000.0f);
	const UINT32 normalizedSeed = (seed == 0u) ? 1u : seed;
	const UINT32 clampedSegments = std::clamp<UINT32>(bladeSegments == 0u ? 4u : bladeSegments, 1u, 32u);

	// Cache key incorporates terrain seed + blade params so the same terrain
	// shares blade meshes. ActiveTerrain only exists post-Terrain-spawn —
	// callers should add TERRAIN entity first, then GRASS_ON_TERRAIN.
	const std::wstring terrainTag = ActiveTerrain ?
		(L"_t" + std::to_wstring(ActiveTerrain->GetData().Header.Seed)) : L"_flat";
	const std::wstring key =
		L"procedural://grass_on_terrain/" +
		std::to_wstring(clampedBlades) + L"/" +
		std::to_wstring(static_cast<int>(std::round(clampedHeight * 100.0f))) + L"/" +
		std::to_wstring(normalizedSeed) + L"/s" + std::to_wstring(clampedSegments) +
		terrainTag;
	const auto cachedIt = ScriptSceneByPath.find(key);
	if (cachedIt != ScriptSceneByPath.end())
		return cachedIt->second;

	shared_ptr<Scene> scene = CreateProceduralGrassOnTerrainScene(clampedBlades, clampedHeight, normalizedSeed, clampedSegments);
	if (!scene)
		return InvalidScriptSceneHandle;

	ScriptSceneHandle handle = NextScriptSceneHandle++;
	if (handle == InvalidScriptSceneHandle)
		handle = NextScriptSceneHandle++;

	ScriptScenes[handle] = { scene, key, EPhysicsCollisionShape::Box, glm::vec3(0.5f) };
	{
		SceneRecipe& r = ScriptScenes[handle].Recipe;
		r.RecipeKind = SceneRecipe::Kind::GrassOnTerrain;
		r.BladeCount = clampedBlades;
		r.BladeHeight = clampedHeight;
		r.Seed = normalizedSeed;
		r.BladeSegments = clampedSegments;
	}
	ScriptSceneByPath[key] = handle;
	AppendCpuRuntimeTrace(
		L"[Luau][MeshComponent] procedural_grass_on_terrain handle=" + std::to_wstring(handle) +
		L" blades=" + std::to_wstring(clampedBlades));
	return handle;
}

Corona::ScriptSceneHandle Corona::CreateProceduralSphereSceneForScript(float radius, uint32_t rings, uint32_t segments)
{
	if (!renderBackend) return InvalidScriptSceneHandle;
	const float r = std::clamp(radius, 0.05f, 100.0f);
	const uint32_t R = std::clamp<uint32_t>(rings,    4u, 128u);
	const uint32_t S = std::clamp<uint32_t>(segments, 6u, 256u);
	const std::wstring key =
		L"procedural://sphere/" +
		std::to_wstring(static_cast<int>(std::round(r * 100.0f))) + L"/" +
		std::to_wstring(R) + L"x" + std::to_wstring(S);
	const auto it = ScriptSceneByPath.find(key);
	if (it != ScriptSceneByPath.end()) return it->second;

	auto scene = CreateProceduralSphereScene(r, R, S);
	if (!scene) return InvalidScriptSceneHandle;
	ScriptSceneHandle handle = NextScriptSceneHandle++;
	if (handle == InvalidScriptSceneHandle) handle = NextScriptSceneHandle++;
	ScriptScenes[handle] = { scene, key, EPhysicsCollisionShape::TriangleMesh, glm::vec3(r) };
	ScriptSceneByPath[key] = handle;
	AppendCpuRuntimeTrace(L"[Luau][MeshComponent] procedural_sphere handle=" + std::to_wstring(handle));
	return handle;
}

Corona::ScriptSceneHandle Corona::CreateProceduralGrassOnTerrainSceneInstancedForScript(
	UINT32 numBlades, float bladeHeight, UINT32 seed, UINT32 bladeSegments)
{
	if (!renderBackend)
		return InvalidScriptSceneHandle;

	const UINT32 clampedBlades = std::clamp<UINT32>(numBlades, 1u, 200'000'000u);
	const float  clampedHeight = std::clamp(bladeHeight, 0.01f, 1000.0f);
	const UINT32 normalizedSeed = (seed == 0u) ? 1u : seed;
	const UINT32 clampedSegments = std::clamp<UINT32>(bladeSegments == 0u ? 4u : bladeSegments, 1u, 32u);

	const std::wstring terrainTag = ActiveTerrain ?
		(L"_t" + std::to_wstring(ActiveTerrain->GetData().Header.Seed)) : L"_flat";
	const std::wstring key =
		L"procedural://grass_on_terrain_instanced/" +
		std::to_wstring(clampedBlades) + L"/" +
		std::to_wstring(static_cast<int>(std::round(clampedHeight * 100.0f))) + L"/" +
		std::to_wstring(normalizedSeed) + L"/s" + std::to_wstring(clampedSegments) +
		terrainTag;
	const auto cachedIt = ScriptSceneByPath.find(key);
	if (cachedIt != ScriptSceneByPath.end())
		return cachedIt->second;

	shared_ptr<Scene> scene = CreateProceduralGrassOnTerrainSceneInstanced(
		clampedBlades, clampedHeight, normalizedSeed, clampedSegments);
	if (!scene)
		return InvalidScriptSceneHandle;

	ScriptSceneHandle handle = NextScriptSceneHandle++;
	if (handle == InvalidScriptSceneHandle)
		handle = NextScriptSceneHandle++;
	ScriptScenes[handle] = { scene, key, EPhysicsCollisionShape::Box, glm::vec3(0.5f) };
	{
		SceneRecipe& r = ScriptScenes[handle].Recipe;
		r.RecipeKind = SceneRecipe::Kind::GrassOnTerrain;
		r.BladeCount = clampedBlades;
		r.BladeHeight = clampedHeight;
		r.Seed = normalizedSeed;
		r.BladeSegments = clampedSegments;
		r.bProceduralPath = true;
	}
	ScriptSceneByPath[key] = handle;
	AppendCpuRuntimeTrace(
		L"[Luau][MeshComponent] procedural_grass_on_terrain_instanced handle=" + std::to_wstring(handle) +
		L" blades=" + std::to_wstring(clampedBlades));
	return handle;
}

bool Corona::RegenerateGrassEntityForScript(
	CoronaECS::Entity entity,
	UINT32 bladeCount,
	float bladeHeight,
	UINT32 seed,
	UINT32 bladeSegments,
	bool bProcedural)
{
	if (!EntityWorld.IsAlive(entity))
		return false;

	// Snapshot the old grass instance so the regenerated entity inherits
	// transform / material / name. We can't keep the entity ID because the
	// SceneObject removal path destroys the ECS entity wholesale.
	const auto* mesh = EntityWorld.GetMesh(entity);
	if (!mesh)
		return false;
	const SceneObjectHandle oldHandle = static_cast<SceneObjectHandle>(mesh->RenderObjectHandle);
	const auto soIt = ScriptObjects.find(oldHandle);
	if (soIt == ScriptObjects.end())
		return false;
	const auto sceneIt = ScriptScenes.find(soIt->second.SceneHandle);
	if (sceneIt == ScriptScenes.end())
		return false;
	const SceneRecipe::Kind kind = sceneIt->second.Recipe.RecipeKind;
	if (kind != SceneRecipe::Kind::GrassOnTerrain && kind != SceneRecipe::Kind::Grass)
		return false;

	const ScriptObjectState oldState = soIt->second;
	const float oldRoughness = mesh->Roughness;
	const float oldMetallic = mesh->Metallic;
	const bool  oldOverrideRM = mesh->bOverrideRoughnessMetallic;
	const bool  oldVisible = mesh->bVisible;
	const bool  oldRayTracing = mesh->bRayTracing;
	const std::string entityName =
		EntityWorld.GetName(entity) ? *EntityWorld.GetName(entity) : std::string{};
	const float oldArea =
		(kind == SceneRecipe::Kind::Grass) ? sceneIt->second.Recipe.AreaSize : 0.0f;

	// Destroy the old SceneObject (this also destroys the ECS entity).
	RemoveSceneObject(oldHandle);

	// Build a fresh scene with the requested params.
	ScriptSceneHandle newSceneHandle = InvalidScriptSceneHandle;
	if (kind == SceneRecipe::Kind::GrassOnTerrain)
	{
		newSceneHandle = bProcedural
			? CreateProceduralGrassOnTerrainSceneInstancedForScript(
				bladeCount, bladeHeight, seed, bladeSegments)
			: CreateProceduralGrassOnTerrainSceneForScript(
				bladeCount, bladeHeight, seed, bladeSegments);
	}
	else
	{
		// Flat grass path doesn't have a procedural variant yet; the VB
		// route is the only available path so the flag is ignored.
		newSceneHandle = CreateProceduralGrassSceneForScript(
			bladeCount, oldArea, bladeHeight, seed, bladeSegments);
	}
	if (newSceneHandle == InvalidScriptSceneHandle)
		return false;

	const SceneObjectHandle newHandle = SpawnSceneObjectForScript(
		newSceneHandle,
		oldState.Position,
		oldState.RotationDegrees,
		oldState.TargetExtent,
		oldState.Scale,
		oldState.bUseScale,
		oldRoughness, oldMetallic, oldOverrideRM,
		oldVisible, oldRayTracing,
		/*bPhysicsQuery=*/ false);
	if (newHandle == InvalidSceneObjectHandle)
		return false;

	// Preserve the user's chosen entity name so the inspector still finds
	// the regenerated grass under the same label.
	if (!entityName.empty())
	{
		const CoronaECS::Entity newEntity = GetSceneObjectEntity(newHandle);
		if (newEntity.IsValid())
			EntityWorld.SetName(newEntity, entityName);
	}

	AppendCpuRuntimeTrace(
		L"[Grass] regenerated blades=" + std::to_wstring(bladeCount) +
		L" height=" + std::to_wstring(bladeHeight) +
		L" segments=" + std::to_wstring(bladeSegments) +
		L" seed=" + std::to_wstring(seed));
	return true;
}

Corona::ScriptSceneHandle Corona::CreateProceduralTerrainSceneForScript(UINT32 seed)
{
	if (!renderBackend)
		return InvalidScriptSceneHandle;

	const UINT32 normalizedSeed = (seed == 0u) ? 1u : seed;
	const std::wstring key = L"procedural://terrain/v1/" + std::to_wstring(normalizedSeed);
	const auto cachedIt = ScriptSceneByPath.find(key);
	if (cachedIt != ScriptSceneByPath.end())
		return cachedIt->second;

	shared_ptr<Scene> scene = CreateProceduralTerrainScene(normalizedSeed);
	if (!scene)
		return InvalidScriptSceneHandle;

	ScriptSceneHandle handle = NextScriptSceneHandle++;
	if (handle == InvalidScriptSceneHandle)
		handle = NextScriptSceneHandle++;

	// Phase 1: skip physics triangle-mesh build (terrain has 6M indices —
	// PhysX bake is heavy and we don't need collision yet). Box shape is
	// the cheap fallback the loader honors when ray-tracing/physics is off.
	ScriptScenes[handle] = { scene, key, EPhysicsCollisionShape::Box, glm::vec3(0.5f) };
	{
		SceneRecipe& r = ScriptScenes[handle].Recipe;
		r.RecipeKind = SceneRecipe::Kind::Terrain;
		r.Seed = normalizedSeed;
	}
	ScriptSceneByPath[key] = handle;
	AppendCpuRuntimeTrace(
		L"[Luau][MeshComponent] procedural_terrain handle=" + std::to_wstring(handle) +
		L" seed=" + std::to_wstring(normalizedSeed));
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
	{
		SceneRecipe& r = ScriptScenes[handle].Recipe;
		r.RecipeKind = SceneRecipe::Kind::Asset;
		r.AssetPath = key;
	}
	ScriptSceneByPath[key] = handle;
	AppendCpuRuntimeTrace(L"[Luau][MeshComponent] load_asset handle=" + std::to_wstring(handle) + L" path=" + key);
	return handle;
}

Corona::SceneObjectHandle Corona::SpawnSceneObjectForScript(
	ScriptSceneHandle sceneHandle,
	const glm::vec3& position,
	const glm::vec3& rotationDegrees,
	float targetExtent,
	const glm::vec3& scale,
	bool bUseScale,
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
	const float safeTargetExtent = std::max(targetExtent, 0.001f);
	const glm::vec3 safeScale = SanitizeScriptScale(scale);
	desc.Transform = bUseScale ?
		BuildScaledSceneTransform(desc.ScenePtr, safeScale, position, rotationDegrees) :
		BuildCenteredSceneTransform(desc.ScenePtr, safeTargetExtent, position, rotationDegrees);
	desc.Roughness = roughness;
	desc.Metallic = metallic;
	desc.bOverrideRoughnessMetallic = bOverrideRoughnessMetallic;
	desc.bVisible = bVisible;
	desc.bRayTracing = bRayTracing;
	desc.bPhysicsQuery = bPhysicsQuery;
	desc.PhysicsCollisionShape = sceneIt->second.PhysicsCollisionShape;
	desc.PhysicsBoxHalfExtent = sceneIt->second.PhysicsBoxHalfExtent;

	const SceneObjectHandle handle = AddSceneObject(desc);
	if (handle == InvalidSceneObjectHandle)
		return handle;

	ScriptObjects[handle] = { sceneHandle, position, rotationDegrees, safeTargetExtent, safeScale, bUseScale };
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

	AppendCpuRuntimeTrace(L"[Luau][MeshComponent] scene_object=" + std::to_wstring(handle) + L" scene=" + std::to_wstring(sceneHandle));
	return handle;
}

CoronaECS::Entity Corona::SpawnEntityForScript(
	ScriptSceneHandle sceneHandle,
	const glm::vec3& position,
	const glm::vec3& rotationDegrees,
	float targetExtent,
	const glm::vec3& scale,
	bool bUseScale,
	float roughness,
	float metallic,
	bool bOverrideRoughnessMetallic,
	bool bVisible,
	bool bRayTracing,
	bool bPhysicsQuery)
{
	const SceneObjectHandle handle = SpawnSceneObjectForScript(
		sceneHandle,
		position,
		rotationDegrees,
		targetExtent,
		scale,
		bUseScale,
		roughness,
		metallic,
		bOverrideRoughnessMetallic,
		bVisible,
		bRayTracing,
		bPhysicsQuery);
	return GetSceneObjectEntity(handle);
}

bool Corona::SetSceneObjectTransformForScript(
	SceneObjectHandle handle,
	const glm::vec3& position,
	const glm::vec3& rotationDegrees,
	float targetExtent,
	const glm::vec3& scale,
	bool bUseScale)
{
	const auto stateIt = ScriptObjects.find(handle);
	if (stateIt == ScriptObjects.end())
		return false;

	const auto sceneIt = ScriptScenes.find(stateIt->second.SceneHandle);
	if (sceneIt == ScriptScenes.end() || !sceneIt->second.ScenePtr)
		return false;

	const float safeTargetExtent = std::max(targetExtent, 0.001f);
	const glm::vec3 safeScale = SanitizeScriptScale(scale);
	stateIt->second.Position = position;
	stateIt->second.RotationDegrees = rotationDegrees;
	stateIt->second.TargetExtent = safeTargetExtent;
	stateIt->second.Scale = safeScale;
	stateIt->second.bUseScale = bUseScale;

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
		bUseScale ?
			BuildScaledSceneTransform(sceneIt->second.ScenePtr, safeScale, position, rotationDegrees) :
			BuildCenteredSceneTransform(sceneIt->second.ScenePtr, safeTargetExtent, position, rotationDegrees));
}

bool Corona::GetSceneObjectTransformForScript(
	SceneObjectHandle handle,
	glm::vec3& position,
	glm::vec3& rotationDegrees,
	float& targetExtent,
	glm::vec3& scale,
	bool& bUseScale) const
{
	const auto stateIt = ScriptObjects.find(handle);
	if (stateIt == ScriptObjects.end())
		return false;

	position = stateIt->second.Position;
	rotationDegrees = stateIt->second.RotationDegrees;
	targetExtent = stateIt->second.TargetExtent;
	scale = stateIt->second.Scale;
	bUseScale = stateIt->second.bUseScale;
	return true;
}

float Corona::GetScriptSceneHeightForScript(ScriptSceneHandle sceneHandle) const
{
	const auto sceneIt = ScriptScenes.find(sceneHandle);
	if (sceneIt == ScriptScenes.end() || !sceneIt->second.ScenePtr)
		return 0.0f;

	return ComputeSceneSourceHeight(sceneIt->second.ScenePtr);
}

bool Corona::SetSpinePoseForScript(
	CoronaECS::Entity entity,
	ScriptSceneHandle sceneHandle,
	const glm::vec3& position,
	const glm::vec3& rotationDegrees,
	float targetHeight,
	float roughness,
	float metallic,
	bool bMirrorX,
	bool bUseWorldScale,
	bool bRayTracing)
{
	if (!EntityWorld.IsAlive(entity))
		return false;

	const float safeScaleOrTargetHeight = std::max(targetHeight, 0.001f);
	ScriptSceneHandle resolvedSceneHandle = sceneHandle;
	shared_ptr<Scene> scene;
	if (sceneHandle != InvalidScriptSceneHandle)
	{
		const auto sceneIt = ScriptScenes.find(sceneHandle);
		if (sceneIt == ScriptScenes.end() || !sceneIt->second.ScenePtr)
			return false;
		scene = sceneIt->second.ScenePtr;
	}

	const SceneObjectHandle objectHandle = GetEntitySceneObject(entity);
	if (!scene && objectHandle != InvalidSceneObjectHandle)
	{
		const auto objectIt = std::find_if(SceneObjects.begin(), SceneObjects.end(), [objectHandle](const SceneObject& object)
		{
			return object.Handle == objectHandle;
		});
		if (objectIt == SceneObjects.end() || !objectIt->ScenePtr)
			return false;

		scene = objectIt->ScenePtr;
		const auto scriptStateIt = ScriptObjects.find(objectHandle);
		if (scriptStateIt != ScriptObjects.end())
			resolvedSceneHandle = scriptStateIt->second.SceneHandle;
	}

	if (!scene)
		return false;

	const float uniformScale = bUseWorldScale ?
		safeScaleOrTargetHeight :
		ComputeSceneTargetHeightScale(scene, safeScaleOrTargetHeight);
	const glm::vec3 signedScale = SanitizeScriptScale(glm::vec3(
		bMirrorX ? -uniformScale : uniformScale,
		uniformScale,
		uniformScale));
	const glm::mat4x4 rotation =
		glm::rotate(glm::radians(rotationDegrees.z), glm::vec3(0.0f, 0.0f, 1.0f)) *
		glm::rotate(glm::radians(rotationDegrees.y), glm::vec3(0.0f, 1.0f, 0.0f)) *
		glm::rotate(glm::radians(rotationDegrees.x), glm::vec3(1.0f, 0.0f, 0.0f));
	const glm::mat4x4 transform = bUseWorldScale ?
		(glm::translate(position) * rotation * glm::scale(signedScale)) :
		BuildScaledSceneTransform(scene, signedScale, position, rotationDegrees);

	if (objectHandle != InvalidSceneObjectHandle)
	{
		const auto objectIt = std::find_if(SceneObjects.begin(), SceneObjects.end(), [objectHandle](const SceneObject& object)
		{
			return object.Handle == objectHandle;
		});
		if (objectIt == SceneObjects.end())
			return false;

		UINT32 dirtyBits = kSceneObjectDirtyTransform;
		if (objectIt->ScenePtr != scene)
		{
			objectIt->ScenePtr = scene;
			objectIt->PhysicsCollisionShape = EPhysicsCollisionShape::TriangleMesh;
			objectIt->PhysicsBoxHalfExtent = glm::vec3(0.5f);
			dirtyBits |= kSceneObjectDirtyAll;
		}
		if (objectIt->Roughness != roughness ||
			objectIt->Metallic != metallic ||
			objectIt->bOverrideRoughnessMetallic)
		{
			dirtyBits |= kSceneObjectDirtyMaterial;
		}
		if (!objectIt->bVisible)
			dirtyBits |= kSceneObjectDirtyVisibility;
		if (objectIt->bRayTracing != bRayTracing)
			dirtyBits |= kSceneObjectDirtyRayTracing;

		const bool bPhysicsQueryChanged = objectIt->bPhysicsQuery;
		objectIt->Transform = transform;
		objectIt->Roughness = roughness;
		objectIt->Metallic = metallic;
		objectIt->bOverrideRoughnessMetallic = false;
		objectIt->bVisible = true;
		objectIt->bRayTracing = bRayTracing;
		objectIt->bPhysicsQuery = false;
		UpdateSceneObjectEntity(*objectIt);
		MarkSceneObjectRenderDirty(objectHandle, dirtyBits);
		if (bPhysicsQueryChanged)
			MarkCpuPhysicsSceneDirty();

		ScriptObjects[objectHandle] = { resolvedSceneHandle, position, rotationDegrees, safeScaleOrTargetHeight, signedScale, true };
		return true;
	}

	if (sceneHandle == InvalidScriptSceneHandle)
		return false;

	SceneObjectDesc desc;
	desc.EntityHandle = entity;
	desc.ScenePtr = scene;
	desc.Transform = transform;
	desc.Roughness = roughness;
	desc.Metallic = metallic;
	desc.bOverrideRoughnessMetallic = false;
	desc.bVisible = true;
	desc.bRayTracing = bRayTracing;
	desc.bPhysicsQuery = false;
	desc.PhysicsCollisionShape = EPhysicsCollisionShape::TriangleMesh;
	desc.PhysicsBoxHalfExtent = glm::vec3(0.5f);

	const SceneObjectHandle handle = AddSceneObject(desc);
	if (handle == InvalidSceneObjectHandle)
		return false;

	ScriptObjects[handle] = { sceneHandle, position, rotationDegrees, safeScaleOrTargetHeight, signedScale, true };
	return true;
}

bool Corona::AddMeshComponentForScript(
	CoronaECS::Entity entity,
	ScriptSceneHandle sceneHandle,
	const glm::vec3& position,
	const glm::vec3& rotationDegrees,
	float targetExtent,
	const glm::vec3& scale,
	bool bUseScale,
	float roughness,
	float metallic,
	bool bOverrideRoughnessMetallic,
	bool bVisible,
	bool bRayTracing,
	bool bPhysicsQuery)
{
	if (!EntityWorld.IsAlive(entity))
		return false;

	const auto sceneIt = ScriptScenes.find(sceneHandle);
	if (sceneIt == ScriptScenes.end() || !sceneIt->second.ScenePtr)
		return false;

	const float safeTargetExtent = std::max(targetExtent, 0.001f);
	const glm::vec3 safeScale = SanitizeScriptScale(scale);
	const glm::mat4x4 transform = bUseScale ?
		BuildScaledSceneTransform(sceneIt->second.ScenePtr, safeScale, position, rotationDegrees) :
		BuildCenteredSceneTransform(sceneIt->second.ScenePtr, safeTargetExtent, position, rotationDegrees);

	const SceneObjectHandle objectHandle = GetEntitySceneObject(entity);
	if (objectHandle != InvalidSceneObjectHandle)
	{
		const auto objectIt = std::find_if(SceneObjects.begin(), SceneObjects.end(), [objectHandle](const SceneObject& object)
		{
			return object.Handle == objectHandle;
		});
		if (objectIt == SceneObjects.end())
			return false;

		objectIt->ScenePtr = sceneIt->second.ScenePtr;
		objectIt->Transform = transform;
		objectIt->Roughness = roughness;
		objectIt->Metallic = metallic;
		objectIt->bOverrideRoughnessMetallic = bOverrideRoughnessMetallic;
		objectIt->bVisible = bVisible;
		objectIt->bRayTracing = bRayTracing;
		objectIt->bPhysicsQuery = bPhysicsQuery;
		objectIt->PhysicsCollisionShape = sceneIt->second.PhysicsCollisionShape;
		objectIt->PhysicsBoxHalfExtent = sceneIt->second.PhysicsBoxHalfExtent;
		UpdateSceneObjectEntity(*objectIt);
		MarkSceneObjectRenderDirty(objectHandle, kSceneObjectDirtyAll);
		MarkCpuPhysicsSceneDirty();
		ScriptObjects[objectHandle] = { sceneHandle, position, rotationDegrees, safeTargetExtent, safeScale, bUseScale };
		return true;
	}

	SceneObjectDesc desc;
	desc.EntityHandle = entity;
	desc.ScenePtr = sceneIt->second.ScenePtr;
	desc.Transform = transform;
	desc.Roughness = roughness;
	desc.Metallic = metallic;
	desc.bOverrideRoughnessMetallic = bOverrideRoughnessMetallic;
	desc.bVisible = bVisible;
	desc.bRayTracing = bRayTracing;
	desc.bPhysicsQuery = bPhysicsQuery;
	desc.PhysicsCollisionShape = sceneIt->second.PhysicsCollisionShape;
	desc.PhysicsBoxHalfExtent = sceneIt->second.PhysicsBoxHalfExtent;

	const SceneObjectHandle handle = AddSceneObject(desc);
	if (handle == InvalidSceneObjectHandle)
		return false;

	ScriptObjects[handle] = { sceneHandle, position, rotationDegrees, safeTargetExtent, safeScale, bUseScale };
	return true;
}

bool Corona::GetEntityMeshForScript(
	CoronaECS::Entity entity,
	CoronaECS::MeshComponent& component) const
{
	if (!EntityWorld.IsAlive(entity))
		return false;

	const CoronaECS::MeshComponent* meshComponent = EntityWorld.GetMesh(entity);
	if (!meshComponent)
		return false;

	component = *meshComponent;
	return true;
}

bool Corona::SetEntityMeshTransformForScript(
	CoronaECS::Entity entity,
	const glm::vec3& position,
	const glm::vec3& rotationDegrees,
	float targetExtent,
	const glm::vec3& scale,
	bool bUseScale)
{
	const SceneObjectHandle objectHandle = GetEntitySceneObject(entity);
	if (objectHandle == InvalidSceneObjectHandle)
		return false;

	return SetSceneObjectTransformForScript(
		objectHandle,
		position,
		rotationDegrees,
		targetExtent,
		scale,
		bUseScale);
}

bool Corona::SetEntityExcludeFromDeformSphereForScript(CoronaECS::Entity entity, bool bExclude)
{
	const SceneObjectHandle objectHandle = GetEntitySceneObject(entity);
	if (objectHandle == InvalidSceneObjectHandle)
		return false;

	const auto objectIt = std::find_if(SceneObjects.begin(), SceneObjects.end(),
		[objectHandle](const SceneObject& object) { return object.Handle == objectHandle; });
	if (objectIt == SceneObjects.end() || !objectIt->ScenePtr)
		return false;

	// Set on every mesh resource the scene owns. The flag is currently
	// MeshResource-scoped, so meshes shared with other scene objects
	// would inherit this opt-out — fine for procedural single-instance
	// meshes like the block_character avatar.
	for (auto& mesh : objectIt->ScenePtr->meshes)
	{
		if (mesh)
			mesh->bExcludeFromDeformSphere = bExclude;
	}
	return true;
}

bool Corona::SetEntityMeshComponentForScript(
	CoronaECS::Entity entity,
	const glm::vec3& position,
	const glm::vec3& rotationDegrees,
	float targetExtent,
	const glm::vec3& scale,
	bool bUseScale,
	float roughness,
	float metallic,
	bool bOverrideRoughnessMetallic,
	bool bVisible,
	bool bRayTracing,
	bool bPhysicsQuery)
{
	if (!EntityWorld.IsAlive(entity))
		return false;

	const SceneObjectHandle objectHandle = GetEntitySceneObject(entity);
	if (objectHandle == InvalidSceneObjectHandle)
		return false;

	const auto objectIt = std::find_if(SceneObjects.begin(), SceneObjects.end(), [objectHandle](const SceneObject& object)
	{
		return object.Handle == objectHandle;
	});
	if (objectIt == SceneObjects.end() || !objectIt->ScenePtr)
		return false;

	const float safeTargetExtent = std::max(targetExtent, 0.001f);
	const glm::vec3 safeScale = SanitizeScriptScale(scale);
	const glm::mat4x4 nextTransform = bUseScale ?
		BuildScaledSceneTransform(objectIt->ScenePtr, safeScale, position, rotationDegrees) :
		BuildCenteredSceneTransform(objectIt->ScenePtr, safeTargetExtent, position, rotationDegrees);
	const bool bPhysicsQueryChanged = objectIt->bPhysicsQuery != bPhysicsQuery;
	UINT32 dirtyBits = kSceneObjectDirtyTransform;
	if (objectIt->Roughness != roughness ||
		objectIt->Metallic != metallic ||
		objectIt->bOverrideRoughnessMetallic != bOverrideRoughnessMetallic)
	{
		dirtyBits |= kSceneObjectDirtyMaterial;
	}
	if (objectIt->bVisible != bVisible)
		dirtyBits |= kSceneObjectDirtyVisibility;
	if (objectIt->bRayTracing != bRayTracing)
		dirtyBits |= kSceneObjectDirtyRayTracing;

	objectIt->Transform = nextTransform;
	objectIt->Roughness = roughness;
	objectIt->Metallic = metallic;
	objectIt->bOverrideRoughnessMetallic = bOverrideRoughnessMetallic;
	objectIt->bVisible = bVisible;
	objectIt->bRayTracing = bRayTracing;
	objectIt->bPhysicsQuery = bPhysicsQuery;

	if (const CoronaECS::PhysicsComponent* physicsComponent = EntityWorld.GetPhysics(entity))
	{
		objectIt->PhysicsCollisionShape =
			physicsComponent->CollisionShape == CoronaECS::PhysicsCollisionShape::Box ?
			EPhysicsCollisionShape::Box :
			EPhysicsCollisionShape::TriangleMesh;
		objectIt->PhysicsBoxHalfExtent = physicsComponent->BoxHalfExtent;
	}

	UpdateSceneObjectEntity(*objectIt);
	MarkSceneObjectRenderDirty(objectHandle, dirtyBits);
	if (objectIt->bVisible && (objectIt->bPhysicsQuery || bPhysicsQueryChanged))
		MarkCpuPhysicsSceneDirty();

	const auto scriptObjectIt = ScriptObjects.find(objectHandle);
	if (scriptObjectIt != ScriptObjects.end())
	{
		scriptObjectIt->second.Position = position;
		scriptObjectIt->second.RotationDegrees = rotationDegrees;
		scriptObjectIt->second.TargetExtent = safeTargetExtent;
		scriptObjectIt->second.Scale = safeScale;
		scriptObjectIt->second.bUseScale = bUseScale;
	}

	if (objectHandle == PistolObject)
	{
		PistolCenterPosition = position;
		PistolCenterRotationDegrees = rotationDegrees;
	}
	else if (objectHandle == BuddhaObject)
	{
		BuddhaCenterPosition = position;
		BuddhaCenterRotationDegrees = rotationDegrees;
	}
	else if (objectHandle == ShaderBallObject)
	{
		ShaderBallCenterPosition = position;
		ShaderBallCenterRotationDegrees = rotationDegrees;
	}
	return true;
}

CoronaECS::Entity Corona::GetMainCameraEntityForScript() const
{
	return EntityWorld.IsAlive(MainCameraEntity) ? MainCameraEntity : CoronaECS::Entity();
}

CoronaECS::Entity Corona::GetWorldEntityForScript()
{
	InitializeWorldEntity();
	return EntityWorld.IsAlive(WorldEntity) ? WorldEntity : CoronaECS::Entity();
}

CoronaECS::Entity Corona::GetLevelEntityForScript()
{
	InitializeLevelEntity();
	return EntityWorld.IsAlive(LevelEntity) ? LevelEntity : CoronaECS::Entity();
}

CoronaECS::Entity Corona::GetMainDirectionalLightEntityForScript()
{
	InitializeMainDirectionalLightEntity();
	return EntityWorld.IsAlive(MainDirectionalLightEntity) ? MainDirectionalLightEntity : CoronaECS::Entity();
}

CoronaECS::Entity Corona::SpawnPointLightEntityForScript(
	const glm::vec3& position,
	float radius,
	const glm::vec3& color,
	float intensity,
	bool bEnabled)
{
	if (PointLights.size() >= MaxPointLights)
		return CoronaECS::Entity();

	PointLightState pointLight;
	pointLight.Id = NextPointLightId++;
	pointLight.bEnabled = bEnabled;
	pointLight.Position = position;
	pointLight.Radius = std::clamp(radius, 1.0f, 100000.0f);
	pointLight.Color = glm::max(color, glm::vec3(0.0f));
	pointLight.Intensity = std::max(0.0f, intensity);
	CreatePointLightEntity(pointLight);

	const CoronaECS::Entity entity = pointLight.EntityHandle;
	PointLights.push_back(pointLight);
	MarkPointLightRenderDirty(pointLight.Id, kPointLightDirtyAll);
	ResetAllAccumulationState(false);
	bPersistentSceneStateDirty = true;
	SaveSceneState();
	return entity;
}

bool Corona::SetEntityTransformForScript(
	CoronaECS::Entity entity,
	const glm::vec3& position,
	const glm::vec3& rotationDegrees,
	const glm::vec3& scale)
{
	if (!EntityWorld.IsAlive(entity))
		return false;
	if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z) ||
		!std::isfinite(rotationDegrees.x) || !std::isfinite(rotationDegrees.y) || !std::isfinite(rotationDegrees.z) ||
		!std::isfinite(scale.x) || !std::isfinite(scale.y) || !std::isfinite(scale.z))
	{
		return false;
	}
	if (!IsReasonableScriptWorldPosition(position))
	{
		AppendCpuRuntimeTrace(
			L"[Luau][TransformComponent] rejected unreasonable position entity=" +
			std::to_wstring(entity.GetId()) +
			L", position=(" + std::to_wstring(position.x) +
			L"," + std::to_wstring(position.y) +
			L"," + std::to_wstring(position.z) + L")");
		return false;
	}

	const CoronaECS::TransformComponent transformComponentValue =
		CoronaECS::TransformComponent::FromTRS(position, rotationDegrees, glm::max(scale, glm::vec3(0.001f)));

	const bool bIsActiveCamera = EntityWorld.GetActiveCameraEntity() == entity;
	if (bIsActiveCamera && entity == MainCameraEntity)
	{
		m_camera.m_position = position;
		UpdateMainCameraEntityFromSimpleCamera();
		return true;
	}

	const SceneObjectHandle objectHandle = GetEntitySceneObject(entity);
	if (objectHandle != InvalidSceneObjectHandle)
		return SetSceneObjectTransform(objectHandle, transformComponentValue.LocalToWorld);

	if (PointLightState* pointLight = FindPointLightByEntity(entity))
	{
		pointLight->Position = position;
		UpdatePointLightEntity(*pointLight);
		MarkPointLightRenderDirty(pointLight->Id, kPointLightDirtyTransform);
		ResetAllAccumulationState(false);
		bPersistentSceneStateDirty = true;
		SaveSceneState();
		return true;
	}

	CoronaECS::TransformComponent* transformComponent = EntityWorld.GetTransform(entity);
	if (!transformComponent)
		transformComponent = EntityWorld.AddTransform(entity);
	if (!transformComponent)
		return false;

	*transformComponent = transformComponentValue;
	return true;
}

bool Corona::SetEntityPhysicsForScript(
	CoronaECS::Entity entity,
	bool bQueryEnabled,
	CoronaECS::PhysicsCollisionShape collisionShape,
	const glm::vec3& boxHalfExtent)
{
	if (!EntityWorld.IsAlive(entity))
		return false;

	CoronaECS::PhysicsComponent physicsComponent;
	physicsComponent.bQueryEnabled = bQueryEnabled;
	physicsComponent.CollisionShape = collisionShape;
	physicsComponent.BoxHalfExtent = glm::max(boxHalfExtent, glm::vec3(0.001f));

	const SceneObjectHandle objectHandle = GetEntitySceneObject(entity);
	if (objectHandle != InvalidSceneObjectHandle)
	{
		const auto objectIt = std::find_if(SceneObjects.begin(), SceneObjects.end(), [objectHandle](const SceneObject& object)
		{
			return object.Handle == objectHandle;
		});
		if (objectIt == SceneObjects.end())
			return false;

		objectIt->bPhysicsQuery = physicsComponent.bQueryEnabled;
		objectIt->PhysicsCollisionShape =
			physicsComponent.CollisionShape == CoronaECS::PhysicsCollisionShape::Box ?
			EPhysicsCollisionShape::Box :
			EPhysicsCollisionShape::TriangleMesh;
		objectIt->PhysicsBoxHalfExtent = physicsComponent.BoxHalfExtent;
		EntityWorld.AddPhysics(entity, physicsComponent);
		if (objectIt->bVisible)
			MarkCpuPhysicsSceneDirty();
		return true;
	}

	return EntityWorld.AddPhysics(entity, physicsComponent) != nullptr;
}

bool Corona::GetEntityPhysicsForScript(
	CoronaECS::Entity entity,
	CoronaECS::PhysicsComponent& component) const
{
	if (!EntityWorld.IsAlive(entity))
		return false;

	if (const CoronaECS::PhysicsComponent* physicsComponent = EntityWorld.GetPhysics(entity))
	{
		component = *physicsComponent;
		return true;
	}
	return false;
}

bool Corona::SetEntityLightForScript(
	CoronaECS::Entity entity,
	const CoronaECS::LightComponent& component,
	bool bPersistSceneState)
{
	if (!EntityWorld.IsAlive(entity))
		return false;

	CoronaECS::LightComponent lightComponent = component;
	lightComponent.Color = glm::max(lightComponent.Color, glm::vec3(0.0f));
	lightComponent.Intensity = std::max(0.0f, lightComponent.Intensity);
	lightComponent.Radius = std::clamp(lightComponent.Radius, 1.0f, 100000.0f);
	if (glm::length(lightComponent.Direction) > 0.0001f)
		lightComponent.Direction = glm::normalize(lightComponent.Direction);
	else
		lightComponent.Direction = glm::vec3(0.0f, 1.0f, 0.0f);

	auto vecNearlyEqual = [](const glm::vec3& a, const glm::vec3& b, float epsilon = 0.0001f)
	{
		return glm::length(a - b) <= epsilon;
	};
	auto lightNearlyEqual = [&](const CoronaECS::LightComponent& a, const CoronaECS::LightComponent& b)
	{
		return
			a.Type == b.Type &&
			a.bEnabled == b.bEnabled &&
			vecNearlyEqual(a.Direction, b.Direction) &&
			vecNearlyEqual(a.Color, b.Color) &&
			std::abs(a.Intensity - b.Intensity) <= 0.0001f &&
			std::abs(a.Radius - b.Radius) <= 0.0001f;
	};
	CoronaECS::LightComponent previousComponent;
	const bool bHadPreviousLight = GetEntityLightForScript(entity, previousComponent);
	const bool bLightChanged = !bHadPreviousLight || !lightNearlyEqual(previousComponent, lightComponent);

	if (entity == MainDirectionalLightEntity || lightComponent.Type == CoronaECS::LightType::Directional)
	{
		const auto pointLightIt = std::find_if(PointLights.begin(), PointLights.end(), [entity](const PointLightState& pointLight)
		{
			return pointLight.EntityHandle == entity;
		});
		if (pointLightIt != PointLights.end())
		{
			MarkPointLightRenderRemoved(pointLightIt->Id);
			PointLights.erase(pointLightIt);
		}

		lightComponent.Type = CoronaECS::LightType::Directional;
		lightComponent.RuntimeLightId = 0;
		MainDirectionalLightEntity = entity;
		EntityWorld.AddLight(entity, lightComponent);
		ApplyDirectionalLightEntityToState();
		// If this setter ran on the render thread (e.g. Scene Inspector gizmo),
		// the legacy LightDir/LightIntensity globals just got updated. Mark
		// them so ApplyFrameSourceRenderSync skips the next game-thread
		// snapshot — without this the stale capture would revert the edit
		// for one frame and the game-thread UpdateMainDirectionalLightEntityFromState
		// would then write the stale value back into this entity.
		bRenderThreadOwnsLightDirNextFrame = true;
		// Directional-light deltas are handled in BuildRenderFrameDerivedState.
		// Spatial hash GI intentionally keeps history across gradual sun motion.
		if (bPersistSceneState && bLightChanged)
		{
			bPersistentSceneStateDirty = true;
			SaveSceneState();
		}
		return true;
	}

	PointLightState* pointLight = FindPointLightByEntity(entity);
	if (!pointLight)
	{
		if (PointLights.size() >= MaxPointLights)
			return false;

		PointLightState newPointLight;
		newPointLight.Id = NextPointLightId++;
		newPointLight.EntityHandle = entity;
		PointLights.push_back(newPointLight);
		pointLight = &PointLights.back();
	}

	pointLight->bEnabled = lightComponent.bEnabled;
	pointLight->Color = lightComponent.Color;
	pointLight->Intensity = lightComponent.Intensity;
	pointLight->Radius = lightComponent.Radius;

	const CoronaECS::TransformComponent* transformComponent = EntityWorld.GetTransform(entity);
	if (transformComponent)
		pointLight->Position = transformComponent->GetPosition();

	lightComponent.Type = CoronaECS::LightType::Point;
	lightComponent.RuntimeLightId = pointLight->Id;
	EntityWorld.AddLight(entity, lightComponent);
	UpdatePointLightEntity(*pointLight);
	if (bLightChanged)
	{
		MarkPointLightRenderDirty(pointLight->Id, kPointLightDirtyAll);
		ResetAllAccumulationState(false);
	}
	if (bPersistSceneState && bLightChanged)
	{
		bPersistentSceneStateDirty = true;
		SaveSceneState();
	}
	return true;
}

bool Corona::GetEntityLightForScript(
	CoronaECS::Entity entity,
	CoronaECS::LightComponent& component) const
{
	if (!EntityWorld.IsAlive(entity))
		return false;

	if (entity == MainDirectionalLightEntity)
	{
		if (const CoronaECS::LightComponent* lightComponent = EntityWorld.GetLight(entity))
		{
			component = *lightComponent;
			return true;
		}
	}

	if (const PointLightState* pointLight = FindPointLightByEntity(entity))
	{
		component.Type = CoronaECS::LightType::Point;
		component.bEnabled = pointLight->bEnabled;
		component.Color = pointLight->Color;
		component.Intensity = pointLight->Intensity;
		component.Radius = pointLight->Radius;
		component.RuntimeLightId = pointLight->Id;
		return true;
	}

	if (const CoronaECS::LightComponent* lightComponent = EntityWorld.GetLight(entity))
	{
		component = *lightComponent;
		return true;
	}
	return false;
}

bool Corona::SetEntityCameraComponentForScript(
	CoronaECS::Entity entity,
	const CoronaECS::CameraComponent& component)
{
	if (!EntityWorld.IsAlive(entity))
		return false;

	CoronaECS::CameraComponent cameraComponent = component;
	if (!std::isfinite(cameraComponent.LookDirection.x) ||
		!std::isfinite(cameraComponent.LookDirection.y) ||
		!std::isfinite(cameraComponent.LookDirection.z))
	{
		cameraComponent.LookDirection = glm::vec3(0.0f, 0.0f, 1.0f);
	}
	if (glm::length(cameraComponent.LookDirection) > 0.0001f)
		cameraComponent.LookDirection = glm::normalize(cameraComponent.LookDirection);
	else
		cameraComponent.LookDirection = glm::vec3(0.0f, 0.0f, 1.0f);
	if (!std::isfinite(cameraComponent.UpDirection.x) ||
		!std::isfinite(cameraComponent.UpDirection.y) ||
		!std::isfinite(cameraComponent.UpDirection.z))
	{
		cameraComponent.UpDirection = glm::vec3(0.0f, 1.0f, 0.0f);
	}
	if (glm::length(cameraComponent.UpDirection) > 0.0001f)
		cameraComponent.UpDirection = glm::normalize(cameraComponent.UpDirection);
	else
		cameraComponent.UpDirection = glm::vec3(0.0f, 1.0f, 0.0f);
	if (!std::isfinite(cameraComponent.Fov))
		cameraComponent.Fov = Fov;
	if (!std::isfinite(cameraComponent.NearPlane))
		cameraComponent.NearPlane = Near;
	if (!std::isfinite(cameraComponent.FarPlane))
		cameraComponent.FarPlane = Far;
	cameraComponent.Fov = std::clamp(cameraComponent.Fov, 0.05f, glm::pi<float>() - 0.05f);
	cameraComponent.NearPlane = std::max(0.001f, cameraComponent.NearPlane);
	cameraComponent.FarPlane = std::max(cameraComponent.NearPlane + 1.0f, cameraComponent.FarPlane);

	if (!EntityWorld.GetTransform(entity))
		EntityWorld.AddTransform(entity);

	CoronaECS::CameraComponent* storedCamera = EntityWorld.AddCamera(entity, cameraComponent);
	if (!storedCamera)
		return false;

	if (cameraComponent.bActive)
		EntityWorld.SetActiveCamera(entity);
	if (entity == MainCameraEntity && (cameraComponent.bActive || EntityWorld.GetActiveCameraEntity() == entity))
	{
		m_camera.m_lookDirection = cameraComponent.LookDirection;
		m_camera.m_upDirection = cameraComponent.UpDirection;
		m_camera.m_yaw = static_cast<float>(std::atan2(cameraComponent.LookDirection.x, cameraComponent.LookDirection.z));
		m_camera.m_pitch = static_cast<float>(std::asin(std::clamp(cameraComponent.LookDirection.y, -1.0f, 1.0f)));
		Fov = cameraComponent.Fov;
		Near = cameraComponent.NearPlane;
		Far = cameraComponent.FarPlane;
	}
	return true;
}

bool Corona::GetEntityCameraComponentForScript(
	CoronaECS::Entity entity,
	CoronaECS::CameraComponent& component) const
{
	if (!EntityWorld.IsAlive(entity))
		return false;

	const CoronaECS::CameraComponent* cameraComponent = EntityWorld.GetCamera(entity);
	if (!cameraComponent)
		return false;

	component = *cameraComponent;
	return true;
}

bool Corona::GetEntityTransformForScript(
	CoronaECS::Entity entity,
	glm::vec3& position) const
{
	if (!EntityWorld.IsAlive(entity))
		return false;

	if (EntityWorld.GetActiveCameraEntity() == entity && entity == MainCameraEntity)
	{
		position = m_camera.m_position;
		return true;
	}

	if (const PointLightState* pointLight = FindPointLightByEntity(entity))
	{
		position = pointLight->Position;
		return true;
	}

	const CoronaECS::TransformComponent* transformComponent = EntityWorld.GetTransform(entity);
	if (!transformComponent)
		return false;

	position = transformComponent->GetPosition();
	return true;
}

bool Corona::SetEntityVisibilityForScript(CoronaECS::Entity entity, bool visible)
{
	if (!EntityWorld.IsAlive(entity))
		return false;

	const SceneObjectHandle objectHandle = GetEntitySceneObject(entity);
	if (objectHandle != InvalidSceneObjectHandle)
		return SetSceneObjectVisibility(objectHandle, visible);

	if (PointLightState* pointLight = FindPointLightByEntity(entity))
	{
		if (pointLight->bEnabled == visible)
			return true;
		pointLight->bEnabled = visible;
		UpdatePointLightEntity(*pointLight);
		MarkPointLightRenderDirty(pointLight->Id, kPointLightDirtyEnabled);
		ResetAllAccumulationState(false);
		bPersistentSceneStateDirty = true;
		SaveSceneState();
		return true;
	}

	if (entity == MainDirectionalLightEntity)
	{
		CoronaECS::LightComponent* lightComponent = EntityWorld.GetLight(entity);
		if (!lightComponent)
			return false;
		if (lightComponent->bEnabled == visible)
			return true;
		lightComponent->bEnabled = visible;
		ApplyDirectionalLightEntityToState();
		// Directional visibility changes should follow the same lighting-change path
		// as intensity/direction edits, preserving spatial hash history when possible.
		bPersistentSceneStateDirty = true;
		SaveSceneState();
		return true;
	}

	CoronaECS::MeshComponent* meshComponent = EntityWorld.GetMesh(entity);
	CoronaECS::LightComponent* lightComponent = EntityWorld.GetLight(entity);
	if (!meshComponent && !lightComponent)
		return false;
	if (meshComponent)
		meshComponent->bVisible = visible;
	if (lightComponent)
		lightComponent->bEnabled = visible;
	return true;
}

bool Corona::SetEntityRayTracingForScript(CoronaECS::Entity entity, bool enabled)
{
	if (!EntityWorld.IsAlive(entity))
		return false;

	const SceneObjectHandle objectHandle = GetEntitySceneObject(entity);
	if (objectHandle != InvalidSceneObjectHandle)
		return SetSceneObjectRayTracingEnabled(objectHandle, enabled);

	CoronaECS::MeshComponent* meshComponent = EntityWorld.GetMesh(entity);
	if (!meshComponent)
		return false;
	meshComponent->bRayTracing = enabled;
	return true;
}

bool Corona::DestroyEntityForScript(CoronaECS::Entity entity)
{
	if (!EntityWorld.IsAlive(entity) ||
		entity == WorldEntity ||
		entity == LevelEntity ||
		entity == MainCameraEntity ||
		entity == MainDirectionalLightEntity)
	{
		return false;
	}

	const SceneObjectHandle objectHandle = GetEntitySceneObject(entity);
	if (objectHandle != InvalidSceneObjectHandle)
		return RemoveSceneObject(objectHandle);

	const auto pointLightIt = std::find_if(PointLights.begin(), PointLights.end(), [entity](const PointLightState& pointLight)
	{
		return pointLight.EntityHandle == entity;
	});
	if (pointLightIt != PointLights.end())
	{
		MarkPointLightRenderRemoved(pointLightIt->Id);
		PointLights.erase(pointLightIt);
		DestroyEntityScriptComponent(entity);
		const bool destroyed = EntityWorld.DestroyEntity(entity);
		ResetAllAccumulationState(false);
		bPersistentSceneStateDirty = true;
		SaveSceneState();
		return destroyed;
	}

	DestroyEntityScriptComponent(entity);
	return EntityWorld.DestroyEntity(entity);
}

bool Corona::AttachEntityScriptForScript(
	CoronaECS::Entity entity,
	int updateRef,
	int shutdownRef,
	int imguiRef,
	int uiRef,
	const std::wstring& sourceName,
	bool bPassEntityToCallbacks)
{
	if (!ScriptState || !ScriptState->L)
		InitLuauScripting();
	if (!ScriptState || !ScriptState->L || !EntityWorld.IsAlive(entity))
		return false;

	CoronaECS::ScriptComponent* scriptComponent = EntityWorld.GetScript(entity);
	if (!scriptComponent)
		scriptComponent = EntityWorld.AddScript(entity);
	if (!scriptComponent)
		return false;

	CoronaECS::ScriptInstance scriptInstance;
	scriptInstance.InstanceId = scriptComponent->NextInstanceId++;
	if (scriptComponent->NextInstanceId == 0)
		scriptComponent->NextInstanceId = 1;
	scriptInstance.UpdateRef = updateRef;
	scriptInstance.ShutdownRef = shutdownRef;
	scriptInstance.ImGuiRef = imguiRef;
	scriptInstance.UiRef = uiRef;
	scriptInstance.SourceName = sourceName;
	scriptInstance.bPassEntityToCallbacks = bPassEntityToCallbacks;
	scriptComponent->Instances.push_back(scriptInstance);
	return true;
}

bool Corona::AttachEntityScriptFileForScript(
	CoronaECS::Entity entity,
	const std::wstring& scriptPath)
{
	if (!EntityWorld.IsAlive(entity))
		return false;

	if (!ScriptState || !ScriptState->L)
		InitLuauScripting();
	if (!ScriptState || !ScriptState->L)
		return false;

	std::filesystem::path path(scriptPath);
	if (path.is_relative())
		path = GetAssetFullPath(scriptPath.c_str());

	const std::string source = ReadTextFileUtf8(path);
	if (source.empty())
	{
		AppendCpuRuntimeTrace(L"[Luau] entity script missing or empty: " + path.wstring());
		return false;
	}

	lua_State* L = ScriptState->L;
	ClearLegacyScriptGlobals(L);

	size_t bytecodeSize = 0;
	char* bytecode = luau_compile(source.data(), source.size(), nullptr, &bytecodeSize);
	if (!bytecode)
	{
		AppendCpuRuntimeTrace(L"[Luau] entity script compile failed: " + path.wstring());
		return false;
	}

	const std::string chunkName = "=" + WideToUtf8Local(path.wstring());
	const int loadResult = luau_load(L, chunkName.c_str(), bytecode, bytecodeSize, 0);
	std::free(bytecode);
	if (loadResult != 0)
	{
		AppendCpuRuntimeTrace(L"[Luau] entity script load failed: " + Utf8ToWideLocal(LuaToString(L, -1)));
		lua_pop(L, 1);
		return false;
	}

	int callResult = LUA_OK;
	{
		ScopedLuauScriptProfileExecution profileExecution(this);
		callResult = lua_pcall(L, 0, 1, 0);
	}
	if (callResult != 0)
	{
		AppendCpuRuntimeTrace(L"[Luau] entity script error: " + Utf8ToWideLocal(LuaToString(L, -1)));
		lua_pop(L, 1);
		return false;
	}

	CoronaECS::ScriptInstance loadedScript;
	loadedScript.SourceName = path.wstring();
	loadedScript.bPassEntityToCallbacks = true;

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

	if (!HasScriptCallbacks(loadedScript))
	{
		AppendCpuRuntimeTrace(L"[Luau] entity script has no callbacks: " + path.wstring());
		return false;
	}

	const bool hasUpdate = loadedScript.UpdateRef != LUA_REFNIL;
	const bool hasShutdown = loadedScript.ShutdownRef != LUA_REFNIL;
	const bool hasImGui = loadedScript.ImGuiRef != LUA_REFNIL;
	const bool hasUi = loadedScript.UiRef != LUA_REFNIL;
	if (!AttachEntityScriptForScript(
		entity,
		loadedScript.UpdateRef,
		loadedScript.ShutdownRef,
		loadedScript.ImGuiRef,
		loadedScript.UiRef,
		loadedScript.SourceName,
		loadedScript.bPassEntityToCallbacks))
	{
		UnrefLuaRef(L, loadedScript.UpdateRef);
		UnrefLuaRef(L, loadedScript.ShutdownRef);
		UnrefLuaRef(L, loadedScript.ImGuiRef);
		UnrefLuaRef(L, loadedScript.UiRef);
		return false;
	}

	AppendCpuRuntimeTrace(
		L"[Luau] entity script attached entity=" + std::to_wstring(entity.GetId()) +
		L", update=" + std::to_wstring(hasUpdate ? 1 : 0) +
		L", shutdown=" + std::to_wstring(hasShutdown ? 1 : 0) +
		L", imgui=" + std::to_wstring(hasImGui ? 1 : 0) +
		L", ui=" + std::to_wstring(hasUi ? 1 : 0) +
		L", path=" + path.wstring());
	return true;
}

void Corona::RegisterNativeEntityScript(
	const std::string& name,
	NativeEntityScriptCallbacks callbacks)
{
	if (name.empty() || !HasNativeScriptCallbacks(callbacks))
		return;

	NativeEntityScripts[name] = std::move(callbacks);
}

bool Corona::HasNativeEntityScript(const std::string& name) const
{
	return NativeEntityScripts.find(name) != NativeEntityScripts.end();
}

bool Corona::AttachNativeEntityScriptForScript(
	CoronaECS::Entity entity,
	const std::string& nativeScriptName)
{
	if (!EntityWorld.IsAlive(entity) || nativeScriptName.empty())
		return false;

	const auto nativeIt = NativeEntityScripts.find(nativeScriptName);
	if (nativeIt == NativeEntityScripts.end() || !HasNativeScriptCallbacks(nativeIt->second))
		return false;

	CoronaECS::ScriptComponent* scriptComponent = EntityWorld.GetScript(entity);
	if (!scriptComponent)
		scriptComponent = EntityWorld.AddScript(entity);
	if (!scriptComponent)
		return false;

	CoronaECS::ScriptInstance scriptInstance;
	scriptInstance.InstanceId = scriptComponent->NextInstanceId++;
	if (scriptComponent->NextInstanceId == 0)
		scriptComponent->NextInstanceId = 1;
	scriptInstance.SourceName = L"native:" + Utf8ToWideLocal(nativeScriptName);
	scriptInstance.NativeScriptName = nativeScriptName;
	scriptInstance.bPassEntityToCallbacks = true;
	scriptComponent->Instances.push_back(scriptInstance);

	AppendCpuRuntimeTrace(
		L"[Luau] native entity script attached entity=" + std::to_wstring(entity.GetId()) +
		L", name=" + Utf8ToWideLocal(nativeScriptName));
	return true;
}

bool Corona::DetachEntityScriptForScript(CoronaECS::Entity entity)
{
	if (!EntityWorld.IsAlive(entity))
		return false;

	DestroyEntityScriptComponent(entity);
	return true;
}

void Corona::RecordScriptFunctionProfile(
	const std::wstring& sourceName,
	const std::string& callbackName,
	double elapsedMs,
	bool bNative)
{
	if (callbackName.empty())
		return;

	const std::string key = MakeScriptProfileKey(sourceName, callbackName, bNative);
	std::lock_guard<std::mutex> lock(ScriptProfileMutex);
	ScriptFunctionProfileStat& stat = ScriptProfileStats[key];
	if (stat.CallCount == 0)
	{
		stat.SourceName = sourceName;
		stat.CallbackName = callbackName;
		stat.bNative = bNative;
		stat.MinMs = elapsedMs;
	}
	++stat.CallCount;
	stat.TotalMs += elapsedMs;
	stat.LastMs = elapsedMs;
	stat.MinMs = std::min(stat.MinMs, elapsedMs);
	stat.MaxMs = std::max(stat.MaxMs, elapsedMs);

	ScriptFunctionProfileStat& frameStat = ScriptProfileCurrentFrameStats[key];
	if (frameStat.CallCount == 0)
	{
		frameStat.SourceName = sourceName;
		frameStat.CallbackName = callbackName;
		frameStat.bNative = bNative;
		frameStat.MinMs = elapsedMs;
	}
	++frameStat.CallCount;
	frameStat.TotalMs += elapsedMs;
	frameStat.LastMs = elapsedMs;
	frameStat.MinMs = std::min(frameStat.MinMs, elapsedMs);
	frameStat.MaxMs = std::max(frameStat.MaxMs, elapsedMs);
	ScriptProfileCurrentFrameTotalMs += elapsedMs;
}

void Corona::PublishScriptProfileFrame()
{
	std::lock_guard<std::mutex> lock(ScriptProfileMutex);
	ScriptProfileLastFrameStats.clear();
	ScriptProfileLastFrameStats.reserve(ScriptProfileCurrentFrameStats.size());
	for (const auto& entry : ScriptProfileCurrentFrameStats)
		ScriptProfileLastFrameStats.push_back(entry.second);

	std::sort(ScriptProfileLastFrameStats.begin(), ScriptProfileLastFrameStats.end(), [](const ScriptFunctionProfileStat& a, const ScriptFunctionProfileStat& b)
	{
		if (a.TotalMs != b.TotalMs)
			return a.TotalMs > b.TotalMs;
		return a.CallCount > b.CallCount;
	});

	ScriptProfileLastFrameTotalMs = ScriptProfileCurrentFrameTotalMs;
	ScriptProfileCurrentFrameStats.clear();
	ScriptProfileCurrentFrameTotalMs = 0.0;
}

void Corona::BeginLuauScriptProfileExecution()
{
	const int previousDepth = ScriptProfileActiveDepth.fetch_add(1, std::memory_order_acq_rel);
	if (previousDepth == 0)
	{
		lua_Callbacks* callbacks = ScriptProfileLuaCallbacks.load(std::memory_order_acquire);
		if (callbacks && callbacks->interrupt == LuauScriptProfileInterrupt)
			callbacks->interrupt = nullptr;
		ScriptProfileLastSampleTickNs = ScriptProfileSamplerTicksNs.load(std::memory_order_relaxed);
	}
}

void Corona::EndLuauScriptProfileExecution()
{
	const int previousDepth = ScriptProfileActiveDepth.fetch_sub(1, std::memory_order_acq_rel);
	if (previousDepth <= 1)
	{
		ScriptProfileActiveDepth.store(0, std::memory_order_release);
		lua_Callbacks* callbacks = ScriptProfileLuaCallbacks.load(std::memory_order_acquire);
		if (callbacks && callbacks->interrupt == LuauScriptProfileInterrupt)
			callbacks->interrupt = nullptr;
		ScriptProfileLastSampleTickNs = ScriptProfileSamplerTicksNs.load(std::memory_order_relaxed);
	}
}

void Corona::RecordLuauScriptProfileSample(lua_State* L, int gcState)
{
	const uint64_t currentTicksNs = ScriptProfileSamplerTicksNs.load(std::memory_order_relaxed);
	if (currentTicksNs <= ScriptProfileLastSampleTickNs)
		return;
	const uint64_t elapsedTicksNs = currentTicksNs - ScriptProfileLastSampleTickNs;
	if (elapsedTicksNs == 0)
		return;

	ScriptProfileLastSampleTickNs = currentTicksNs;
	const double elapsedMs = static_cast<double>(elapsedTicksNs) / 1000000.0;

	struct LuaFrame
	{
		std::wstring SourceName;
		std::string FunctionName;
		int LineDefined = -1;
	};

	std::vector<LuaFrame> frames;
	if (gcState > 0)
		frames.push_back({ L"[GC]", "GC", gcState });

	for (int level = 0; ; ++level)
	{
		lua_Debug ar = {};
		if (!lua_getinfo(L, level, "sln", &ar))
			break;
		if (ar.what && std::strcmp(ar.what, "C") == 0)
			continue;

		std::string functionName;
		if (ar.name && ar.name[0] != '\0')
			functionName = ar.name;
		else if (ar.linedefined <= 1)
			functionName = "<main>";
		else
			functionName = "<anonymous>";

		frames.push_back({ LuauSourceToWide(ar.source), functionName, ar.linedefined });
	}

	if (frames.empty())
		return;

	std::lock_guard<std::mutex> lock(ScriptProfileMutex);
	for (size_t index = 0; index < frames.size(); ++index)
	{
		const LuaFrame& frame = frames[index];
		const std::string key = MakeScriptSampleProfileKey(frame.SourceName, frame.FunctionName, frame.LineDefined);
		ScriptFunctionSampleStat& stat = ScriptProfileSamples[key];
		if (stat.InclusiveSamples == 0 && stat.SelfSamples == 0)
		{
			stat.SourceName = frame.SourceName;
			stat.FunctionName = frame.FunctionName;
			stat.LineDefined = frame.LineDefined;
		}

		++stat.InclusiveSamples;
		stat.InclusiveMs += elapsedMs;
		if (index == 0)
		{
			++stat.SelfSamples;
			stat.SelfMs += elapsedMs;
		}
	}
}

void Corona::ResetScriptProfileStats()
{
	std::lock_guard<std::mutex> lock(ScriptProfileMutex);
	ScriptProfileStats.clear();
	ScriptProfileCurrentFrameStats.clear();
	ScriptProfileLastFrameStats.clear();
	ScriptProfileCurrentFrameTotalMs = 0.0;
	ScriptProfileLastFrameTotalMs = 0.0;
	ScriptProfileSamples.clear();
	ScriptProfileLastSampleTickNs = ScriptProfileSamplerTicksNs.load(std::memory_order_relaxed);
	ScriptProfileLastDumpStatus = L"Script profile reset.";
}

bool Corona::DumpScriptProfileStats()
{
	std::vector<ScriptFunctionProfileStat> stats;
	std::vector<ScriptFunctionSampleStat> samples;
	{
		std::lock_guard<std::mutex> lock(ScriptProfileMutex);
		stats.reserve(ScriptProfileStats.size());
		for (const auto& entry : ScriptProfileStats)
			stats.push_back(entry.second);
		samples.reserve(ScriptProfileSamples.size());
		for (const auto& entry : ScriptProfileSamples)
			samples.push_back(entry.second);
	}

	std::sort(stats.begin(), stats.end(), [](const ScriptFunctionProfileStat& a, const ScriptFunctionProfileStat& b)
	{
		if (a.TotalMs != b.TotalMs)
			return a.TotalMs > b.TotalMs;
		return a.CallCount > b.CallCount;
	});
	std::sort(samples.begin(), samples.end(), [](const ScriptFunctionSampleStat& a, const ScriptFunctionSampleStat& b)
	{
		if (a.SelfMs != b.SelfMs)
			return a.SelfMs > b.SelfMs;
		if (a.InclusiveMs != b.InclusiveMs)
			return a.InclusiveMs > b.InclusiveMs;
		return a.SelfSamples > b.SelfSamples;
	});

	const std::filesystem::path logPath = RuntimePaths::LogFile(L"script_profile.csv");
	const std::filesystem::path sampleLogPath = RuntimePaths::LogFile(L"script_profile_samples.csv");
	std::error_code ec;
	std::filesystem::create_directories(logPath.parent_path(), ec);
	std::ofstream file(logPath, std::ios::trunc);
	if (!file.is_open())
	{
		const std::wstring status = L"Failed to write script profile: " + logPath.wstring();
		{
			std::lock_guard<std::mutex> lock(ScriptProfileMutex);
			ScriptProfileLastDumpStatus = status;
		}
		AppendCpuRuntimeTrace(L"[Luau] " + status);
		return false;
	}

	file << "rank,name,callback,source,is_native,call_count,total_ms,average_ms,last_ms,min_ms,max_ms\n";
	for (size_t index = 0; index < stats.size(); ++index)
	{
		const ScriptFunctionProfileStat& stat = stats[index];
		const double averageMs = stat.CallCount > 0 ? stat.TotalMs / static_cast<double>(stat.CallCount) : 0.0;
		const std::string shortName = WideToUtf8Local(ShortScriptSourceName(stat.SourceName));
		const std::string sourceName = WideToUtf8Local(stat.SourceName);
		file << (index + 1)
			<< "," << CsvEscape(shortName + ":" + stat.CallbackName)
			<< "," << CsvEscape(stat.CallbackName)
			<< "," << CsvEscape(sourceName)
			<< "," << (stat.bNative ? 1 : 0)
			<< "," << stat.CallCount
			<< "," << std::fixed << std::setprecision(6) << stat.TotalMs
			<< "," << averageMs
			<< "," << stat.LastMs
			<< "," << stat.MinMs
			<< "," << stat.MaxMs
			<< "\n";
	}

	std::ofstream sampleFile(sampleLogPath, std::ios::trunc);
	if (!sampleFile.is_open())
	{
		const std::wstring status = L"Failed to write script profile samples: " + sampleLogPath.wstring();
		{
			std::lock_guard<std::mutex> lock(ScriptProfileMutex);
			ScriptProfileLastDumpStatus = status;
		}
		AppendCpuRuntimeTrace(L"[Luau] " + status);
		return false;
	}

	sampleFile << "rank,name,function,source,line_defined,self_samples,inclusive_samples,self_ms,inclusive_ms\n";
	for (size_t index = 0; index < samples.size(); ++index)
	{
		const ScriptFunctionSampleStat& stat = samples[index];
		const std::string shortName = WideToUtf8Local(ShortScriptSourceName(stat.SourceName));
		const std::string sourceName = WideToUtf8Local(stat.SourceName);
		const std::string functionName = MakeScriptFunctionDisplayName(stat.FunctionName, stat.LineDefined);
		sampleFile << (index + 1)
			<< "," << CsvEscape(shortName + ":" + functionName)
			<< "," << CsvEscape(stat.FunctionName)
			<< "," << CsvEscape(sourceName)
			<< "," << stat.LineDefined
			<< "," << stat.SelfSamples
			<< "," << stat.InclusiveSamples
			<< "," << std::fixed << std::setprecision(6) << stat.SelfMs
			<< "," << stat.InclusiveMs
			<< "\n";
	}

	const std::wstring status = L"Wrote script profile: " + logPath.wstring() + L" and " + sampleLogPath.wstring();
	{
		std::lock_guard<std::mutex> lock(ScriptProfileMutex);
		ScriptProfileLastDumpStatus = status;
	}
	AppendCpuRuntimeTrace(L"[Luau] " + status);
	return true;
}

void Corona::PushScriptProfileStatsForScript(lua_State* L)
{
	std::vector<ScriptFunctionProfileStat> stats;
	double frameTotalMs = 0.0;
	{
		std::lock_guard<std::mutex> lock(ScriptProfileMutex);
		stats = ScriptProfileLastFrameStats;
		frameTotalMs = ScriptProfileLastFrameTotalMs;
	}

	lua_newtable(L);
	PushNumberField(L, "frame_total_ms", frameTotalMs);

	lua_newtable(L);
	int pushedIndex = 1;
	for (const ScriptFunctionProfileStat& stat : stats)
	{
		lua_newtable(L);
		const std::string shortName = WideToUtf8Local(ShortScriptSourceName(stat.SourceName));
		const std::string sourceName = WideToUtf8Local(stat.SourceName);
		PushStringField(L, "name", shortName + ":" + stat.CallbackName);
		PushStringField(L, "source", sourceName);
		PushStringField(L, "callback", stat.CallbackName);
		PushBoolField(L, "native", stat.bNative);
		PushIntegerField(L, "call_count", static_cast<lua_Integer>(stat.CallCount));
		PushNumberField(L, "total_ms", stat.TotalMs);
		PushNumberField(L, "average_ms", stat.CallCount > 0 ? stat.TotalMs / static_cast<double>(stat.CallCount) : 0.0);
		PushNumberField(L, "last_ms", stat.LastMs);
		PushNumberField(L, "min_ms", stat.MinMs);
		PushNumberField(L, "max_ms", stat.MaxMs);
		lua_rawseti(L, -2, pushedIndex++);
	}
	lua_setfield(L, -2, "functions");
}

void Corona::PushScriptProfileSamplesForScript(lua_State* L)
{
	std::vector<ScriptFunctionSampleStat> samples;
	{
		std::lock_guard<std::mutex> lock(ScriptProfileMutex);
		samples.reserve(ScriptProfileSamples.size());
		for (const auto& entry : ScriptProfileSamples)
			samples.push_back(entry.second);
	}

	std::sort(samples.begin(), samples.end(), [](const ScriptFunctionSampleStat& a, const ScriptFunctionSampleStat& b)
	{
		if (a.SelfMs != b.SelfMs)
			return a.SelfMs > b.SelfMs;
		if (a.InclusiveMs != b.InclusiveMs)
			return a.InclusiveMs > b.InclusiveMs;
		return a.SelfSamples > b.SelfSamples;
	});

	lua_newtable(L);
	int pushedIndex = 1;
	for (const ScriptFunctionSampleStat& stat : samples)
	{
		lua_newtable(L);
		const std::string shortName = WideToUtf8Local(ShortScriptSourceName(stat.SourceName));
		const std::string sourceName = WideToUtf8Local(stat.SourceName);
		const std::string functionName = MakeScriptFunctionDisplayName(stat.FunctionName, stat.LineDefined);
		PushStringField(L, "name", shortName + ":" + functionName);
		PushStringField(L, "source", sourceName);
		PushStringField(L, "function", stat.FunctionName);
		PushIntegerField(L, "line_defined", static_cast<lua_Integer>(stat.LineDefined));
		PushIntegerField(L, "self_samples", static_cast<lua_Integer>(stat.SelfSamples));
		PushIntegerField(L, "inclusive_samples", static_cast<lua_Integer>(stat.InclusiveSamples));
		PushNumberField(L, "self_ms", stat.SelfMs);
		PushNumberField(L, "inclusive_ms", stat.InclusiveMs);
		lua_rawseti(L, -2, pushedIndex++);
	}
}

bool Corona::SetDefaultWorldVisibleForScript(bool visible)
{
	const SceneObjectHandle handles[] =
	{
		SponzaObject,
		BuddhaObject,
		ShaderBallObject,
		PistolObject,
		MirrorCubeObject,
	};

	bool ok = true;
	for (SceneObjectHandle handle : handles)
	{
		if (handle != InvalidSceneObjectHandle)
			ok = SetSceneObjectVisibility(handle, visible) && ok;
	}
	return ok;
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
	if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z) ||
		!std::isfinite(lookAt.x) || !std::isfinite(lookAt.y) || !std::isfinite(lookAt.z))
	{
		return false;
	}
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
	UpdateMainCameraEntityFromSimpleCamera();
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

void Corona::UpdateMobileVirtualMoveFromTouch(const PlatformTouchState& touchState)
{
#if CORONA_PLATFORM_MOBILE
	const float shortEdge = static_cast<float>(std::max<UINT>(1u, std::min(m_width, m_height)));
	const float joystickRadius = std::clamp(shortEdge * 0.12f, 72.0f, 150.0f);
	const float attackRadius = std::clamp(shortEdge * 0.085f, 62.0f, 96.0f);
	const float attackMargin = std::clamp(shortEdge * 0.070f, 56.0f, 92.0f);
	MobileVirtualJoystickRadius = joystickRadius;
	MobileVirtualAttackRadius = attackRadius;
	// Jump pad pinned to the bottom-right corner; the two attack pads stack
	// above it. Layout mirrors PlatformWindow.cpp GetAndroidAttackButtonLayout.
	MobileVirtualAttack3Center = glm::vec2(
		static_cast<float>(m_width) - attackMargin - attackRadius,
		static_cast<float>(m_height) - attackMargin - attackRadius);
	MobileVirtualAttackCenter = glm::vec2(
		MobileVirtualAttack3Center.x,
		std::max(attackMargin + attackRadius, MobileVirtualAttack3Center.y - attackRadius * 2.45f));
	MobileVirtualAttack2Center = glm::vec2(
		MobileVirtualAttackCenter.x,
		std::max(attackMargin + attackRadius, MobileVirtualAttackCenter.y - attackRadius * 2.45f));
	bMobileVirtualAttackDown = touchState.bAttackActive;
	bMobileVirtualAttackPressed = touchState.bAttackPressed;
	bMobileVirtualAttackReleased = touchState.bAttackReleased;
	bMobileVirtualAttack2Down = touchState.bAttack2Active;
	bMobileVirtualAttack2Pressed = touchState.bAttack2Pressed;
	bMobileVirtualAttack2Released = touchState.bAttack2Released;
	bMobileVirtualAttack3Down = touchState.bAttack3Active;
	bMobileVirtualAttack3Pressed = touchState.bAttack3Pressed;
	bMobileVirtualAttack3Released = touchState.bAttack3Released;

	if (!touchState.bMoveActive)
	{
		bMobileVirtualJoystickActive = false;
		MobileVirtualMoveAxis = glm::vec2(0.0f);
		MobileVirtualJoystickCenter = glm::vec2(0.0f);
		MobileVirtualJoystickDrag = glm::vec2(0.0f);
		bMobileVirtualMoveForward = false;
		bMobileVirtualMoveBackward = false;
		bMobileVirtualMoveLeft = false;
		bMobileVirtualMoveRight = false;
		return;
	}

	bMobileVirtualJoystickActive = true;
	MobileVirtualJoystickCenter = glm::vec2(touchState.MoveStartX, touchState.MoveStartY);
	MobileVirtualJoystickDrag = glm::vec2(touchState.MoveX, touchState.MoveY);

	glm::vec2 axis(
		(touchState.MoveX - touchState.MoveStartX) / joystickRadius,
		-(touchState.MoveY - touchState.MoveStartY) / joystickRadius);
	const float axisLength = glm::length(axis);
	if (axisLength > 1.0f)
		axis /= axisLength;
	else if (axisLength < 0.08f)
		axis = glm::vec2(0.0f);

	MobileVirtualMoveAxis = axis;
	bMobileVirtualMoveForward = axis.y > 0.30f;
	bMobileVirtualMoveBackward = axis.y < -0.30f;
	bMobileVirtualMoveLeft = axis.x < -0.30f;
	bMobileVirtualMoveRight = axis.x > 0.30f;
#else
	(void)touchState;
	bMobileVirtualAttackDown = false;
	bMobileVirtualAttackPressed = false;
	bMobileVirtualAttackReleased = false;
	bMobileVirtualAttack2Down = false;
	bMobileVirtualAttack2Pressed = false;
	bMobileVirtualAttack2Released = false;
	bMobileVirtualAttack3Down = false;
	bMobileVirtualAttack3Pressed = false;
	bMobileVirtualAttack3Released = false;
#endif
}

void Corona::PollScriptMouseState()
{
#if CORONA_PLATFORM_MOBILE
	PlatformTouchState touchState;
	if (PollMainPlatformTouchState(touchState))
	{
		UpdateMobileVirtualMoveFromTouch(touchState);
		const int x = static_cast<int>(std::lround(touchState.X));
		const int y = static_cast<int>(std::lround(touchState.Y));
		if (touchState.bLookActive)
		{
			if (!bScriptRightMouseDown)
				RecordScriptRButtonDown(x, y);
			else
				RecordScriptMouseMove(x, y);
			return;
		}

		if (bScriptRightMouseDown)
			RecordScriptRButtonUp();
		bScriptMousePositionInitialized = false;
		return;
	}
#endif

	PlatformMouseState mouseState;
	if (!PollMainPlatformMouseState(mouseState))
		return;

	if (!mouseState.bWindowCanReceiveMouse)
	{
		if (bScriptRightMouseDown)
			RecordScriptRButtonUp();
		bScriptMousePositionInitialized = false;
		return;
	}

	if (mouseState.bRightButtonDown && !bScriptRightMouseDown)
		RecordScriptRButtonDown(mouseState.X, mouseState.Y);
	else if (!mouseState.bRightButtonDown && bScriptRightMouseDown)
		RecordScriptRButtonUp();

	RecordScriptMouseMove(mouseState.X, mouseState.Y);
}

void Corona::PollScriptGamepadState()
{
	const uint16_t previousButtons = ScriptGamepadButtonsDown;
	auto clearState = [this, previousButtons]()
	{
		bScriptGamepadConnected = false;
		ScriptGamepadLeftX = 0.0f;
		ScriptGamepadLeftY = 0.0f;
		ScriptGamepadRightX = 0.0f;
		ScriptGamepadRightY = 0.0f;
		ScriptGamepadLeftTrigger = 0.0f;
		ScriptGamepadRightTrigger = 0.0f;
		ScriptGamepadButtonsDown = 0;
		ScriptGamepadButtonsPressed = 0;
		ScriptGamepadButtonsReleased = previousButtons;
	};

	PlatformGamepadState state;
	if (!PollMainPlatformGamepadState(state))
		state = PlatformGamepadState{};

#if CORONA_PLATFORM_MOBILE
	const float virtualLeftX = MobileVirtualMoveAxis.x;
	const float virtualLeftY = MobileVirtualMoveAxis.y;
	const bool bVirtualAttack = bMobileVirtualAttackDown || bMobileVirtualAttackPressed;
	const bool bVirtualAttack2 = bMobileVirtualAttack2Down || bMobileVirtualAttack2Pressed;
	const bool bVirtualAttack3 = bMobileVirtualAttack3Down || bMobileVirtualAttack3Pressed;
	const bool bHasVirtualMove =
		std::abs(virtualLeftX) > 0.001f ||
		std::abs(virtualLeftY) > 0.001f;
	if (bHasVirtualMove || bVirtualAttack || bVirtualAttack2 || bVirtualAttack3)
	{
		state.bConnected = true;
		state.LeftX = std::clamp(state.LeftX + virtualLeftX, -1.0f, 1.0f);
		state.LeftY = std::clamp(state.LeftY + virtualLeftY, -1.0f, 1.0f);
		if (bVirtualAttack)
			state.Buttons |= PlatformGamepadButton::B;
		if (bVirtualAttack2)
		{
			state.Buttons |= PlatformGamepadButton::Y;
			state.RightTrigger = std::max(state.RightTrigger, 1.0f);
		}
		if (bVirtualAttack3)
			state.Buttons |= PlatformGamepadButton::A;
	}
#endif

	if (!state.bConnected)
	{
		clearState();
		return;
	}

	const uint16_t currentButtons = state.Buttons;
	bScriptGamepadConnected = true;
	ScriptGamepadLeftX = state.LeftX;
	ScriptGamepadLeftY = state.LeftY;
	ScriptGamepadRightX = state.RightX;
	ScriptGamepadRightY = state.RightY;
	ScriptGamepadLeftTrigger = state.LeftTrigger;
	ScriptGamepadRightTrigger = state.RightTrigger;
	ScriptGamepadButtonsDown = currentButtons;
	ScriptGamepadButtonsPressed = static_cast<uint16_t>(currentButtons & ~previousButtons);
	ScriptGamepadButtonsReleased = static_cast<uint16_t>(previousButtons & ~currentButtons);
}

bool Corona::IsScriptKeyDownForScript(UINT8 key) const
{
	// Console captures the keyboard → keep all script-side key queries
	// as "up" so the character doesn't drift / camera doesn't pan while
	// the user is typing a command.
	if (Console && Console->IsVisible())
		return false;
	return ScriptKeyDown[static_cast<size_t>(key)];
}

bool Corona::WasScriptKeyPressedForScript(UINT8 key) const
{
	if (Console && Console->IsVisible())
		return false;
	return ScriptKeyPressed[static_cast<size_t>(key)];
}

bool Corona::WasScriptKeyReleasedForScript(UINT8 key) const
{
	if (Console && Console->IsVisible())
		return false;
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

void Corona::GetScriptGamepadForScript(
	bool& connected,
	float& leftX,
	float& leftY,
	float& rightX,
	float& rightY,
	float& leftTrigger,
	float& rightTrigger,
	uint16_t& buttonsDown,
	uint16_t& buttonsPressed,
	uint16_t& buttonsReleased) const
{
	connected = bScriptGamepadConnected;
	leftX = ScriptGamepadLeftX;
	leftY = ScriptGamepadLeftY;
	rightX = ScriptGamepadRightX;
	rightY = ScriptGamepadRightY;
	leftTrigger = ScriptGamepadLeftTrigger;
	rightTrigger = ScriptGamepadRightTrigger;
	buttonsDown = ScriptGamepadButtonsDown;
	buttonsPressed = ScriptGamepadButtonsPressed;
	buttonsReleased = ScriptGamepadButtonsReleased;
}

void Corona::ClearScriptInputFrameState()
{
	ScriptKeyPressed.fill(false);
	ScriptKeyReleased.fill(false);
	bScriptRightMousePressed = false;
	bScriptRightMouseReleased = false;
	ScriptMouseDeltaX = 0;
	ScriptMouseDeltaY = 0;
	ScriptGamepadButtonsPressed = 0;
	ScriptGamepadButtonsReleased = 0;
}

void Corona::QueueScriptUiCommandForScript(ScriptUiCommand command)
{
	command.bGameUi = bCurrentScriptUiIsGame;
	ScriptUiBuildCommands.push_back(std::move(command));
}

void Corona::QueueScriptUiSeparatorForScript()
{
	ScriptUiCommand command;
	command.Type = ScriptUiCommandType::Separator;
	QueueScriptUiCommandForScript(std::move(command));
}

void Corona::QueueScriptUiTextForScript(const std::string& text)
{
	ScriptUiCommand command;
	command.Type = ScriptUiCommandType::Text;
	command.Label = text;
	QueueScriptUiCommandForScript(std::move(command));
}

void Corona::QueueScriptUiSameLineForScript()
{
	ScriptUiCommand command;
	command.Type = ScriptUiCommandType::SameLine;
	QueueScriptUiCommandForScript(std::move(command));
}

void Corona::QueueScriptUiBeginWindowForScript(const std::string& title)
{
	ScriptUiCommand command;
	command.Type = ScriptUiCommandType::BeginWindow;
	command.Label = title;
	QueueScriptUiCommandForScript(std::move(command));
}

void Corona::QueueScriptUiEndWindowForScript()
{
	ScriptUiCommand command;
	command.Type = ScriptUiCommandType::EndWindow;
	QueueScriptUiCommandForScript(std::move(command));
}

void Corona::QueueScriptUiOverlayTextForScript(const std::string& text, float x, float y)
{
	ScriptUiCommand command;
	command.Type = ScriptUiCommandType::OverlayText;
	command.Label = text;
	command.FloatValue = x;
	command.MinValue = y;
	QueueScriptUiCommandForScript(std::move(command));
}

void Corona::QueueScriptUiOverlayLineForScript(
	float x0,
	float y0,
	float x1,
	float y1,
	const glm::vec4& color,
	float thickness)
{
	ScriptUiCommand command;
	command.Type = ScriptUiCommandType::OverlayLine;
	command.FloatValue = x0;
	command.MinValue = y0;
	command.MaxValue = x1;
	command.Vec3Value = glm::vec3(y1, 0.0f, 0.0f);
	command.ColorValue = color;
	command.IntValue = static_cast<int>(std::max(thickness, 1.0f) * 100.0f);
	QueueScriptUiCommandForScript(std::move(command));
}

void Corona::QueueScriptUiOverlayRectForScript(
	float x,
	float y,
	float width,
	float height,
	const glm::vec4& color,
	float thickness,
	float rounding)
{
	ScriptUiCommand command;
	command.Type = ScriptUiCommandType::OverlayRect;
	command.FloatValue = x;
	command.MinValue = y;
	command.MaxValue = width;
	command.Vec3Value = glm::vec3(height, 0.0f, 0.0f);
	command.ColorValue = color;
	command.IntValue = static_cast<int>(std::max(thickness, 1.0f) * 100.0f);
	command.MinIntValue = static_cast<int>(std::max(rounding, 0.0f) * 100.0f);
	QueueScriptUiCommandForScript(std::move(command));
}

void Corona::QueueScriptUiOverlayRectFilledForScript(
	float x,
	float y,
	float width,
	float height,
	const glm::vec4& color,
	float rounding)
{
	ScriptUiCommand command;
	command.Type = ScriptUiCommandType::OverlayRectFilled;
	command.FloatValue = x;
	command.MinValue = y;
	command.MaxValue = width;
	command.Vec3Value = glm::vec3(height, 0.0f, 0.0f);
	command.ColorValue = color;
	command.MinIntValue = static_cast<int>(std::max(rounding, 0.0f) * 100.0f);
	QueueScriptUiCommandForScript(std::move(command));
}

bool Corona::QueueScriptUiOverlayButtonForScript(
	const std::string& id,
	const std::string& label,
	float x,
	float y,
	float width,
	float height,
	const glm::vec4& fillColor,
	const glm::vec4& hoverColor,
	const glm::vec4& borderColor,
	float rounding)
{
	ScriptUiCommand command;
	command.Type = ScriptUiCommandType::OverlayButton;
	command.Id = id;
	command.Label = label;
	command.FloatValue = x;
	command.MinValue = y;
	command.MaxValue = width;
	command.Vec3Value = glm::vec3(height, 0.0f, 0.0f);
	command.ColorValue = fillColor;
	command.SecondaryColorValue = hoverColor;
	command.TertiaryColorValue = borderColor;
	command.MinIntValue = static_cast<int>(std::max(rounding, 0.0f) * 100.0f);
	QueueScriptUiCommandForScript(std::move(command));

	const auto result = ScriptUiClickedResults.find(id);
	if (result == ScriptUiClickedResults.end())
		return false;

	const bool clicked = result->second;
	ScriptUiClickedResults.erase(result);
	return clicked;
}

void Corona::QueueScriptUiOverlayProgressBarForScript(
	const std::string& id,
	float x,
	float y,
	float width,
	float height,
	float fraction,
	const std::string& label,
	const glm::vec4& fillColor,
	const glm::vec4& backgroundColor,
	const glm::vec4& borderColor)
{
	ScriptUiCommand command;
	command.Type = ScriptUiCommandType::OverlayProgressBar;
	command.Id = id;
	command.Label = label;
	command.FloatValue = x;
	command.MinValue = y;
	command.MaxValue = width;
	command.Vec3Value = glm::vec3(height, 0.0f, 0.0f);
	command.IntValue = static_cast<int>(std::clamp(fraction, 0.0f, 1.0f) * 10000.0f);
	command.ColorValue = fillColor;
	command.SecondaryColorValue = backgroundColor;
	command.TertiaryColorValue = borderColor;
	QueueScriptUiCommandForScript(std::move(command));
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
	QueueScriptUiCommandForScript(std::move(command));
}

void Corona::QueueScriptUiWorldTextForScript(
	const std::string& id,
	const glm::vec3& position,
	const std::string& text,
	float xOffset,
	float yOffset,
	const glm::vec4& color)
{
	ScriptUiCommand command;
	command.Type = ScriptUiCommandType::WorldText;
	command.Id = id;
	command.Label = text;
	command.Vec3Value = position;
	command.FloatValue = xOffset;
	command.MinValue = yOffset;
	command.ColorValue = color;
	QueueScriptUiCommandForScript(std::move(command));
}

void Corona::QueueScriptUiWorldProgressBarForScript(
	const std::string& id,
	const glm::vec3& position,
	float fraction,
	const std::string& label,
	float width,
	float height,
	const glm::vec4& fillColor,
	const glm::vec4& backgroundColor,
	const glm::vec4& borderColor)
{
	ScriptUiCommand command;
	command.Type = ScriptUiCommandType::WorldProgressBar;
	command.Id = id;
	command.Label = label;
	command.Vec3Value = position;
	command.FloatValue = std::clamp(fraction, 0.0f, 1.0f);
	command.MinValue = width;
	command.MaxValue = height;
	command.ColorValue = fillColor;
	command.SecondaryColorValue = backgroundColor;
	command.TertiaryColorValue = borderColor;
	QueueScriptUiCommandForScript(std::move(command));
}

void Corona::QueueScriptUiWorldHealthBarForScript(
	const std::string& id,
	const glm::vec3& position,
	float fraction,
	const std::string& label,
	float width,
	float height)
{
	ScriptUiCommand command;
	command.Type = ScriptUiCommandType::WorldHealthBar;
	command.Id = id;
	command.Label = label;
	command.Vec3Value = position;
	command.FloatValue = std::clamp(fraction, 0.0f, 1.0f);
	command.MinValue = width;
	command.MaxValue = height;
	QueueScriptUiCommandForScript(std::move(command));
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
	QueueScriptUiCommandForScript(std::move(command));
	return effectiveValue;
}

bool Corona::QueueScriptUiButtonForScript(const std::string& id, const std::string& label)
{
	ScriptUiCommand command;
	command.Type = ScriptUiCommandType::Button;
	command.Id = id;
	command.Label = label;
	QueueScriptUiCommandForScript(std::move(command));

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
	QueueScriptUiCommandForScript(std::move(command));
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
	QueueScriptUiCommandForScript(std::move(command));
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
	QueueScriptUiCommandForScript(std::move(command));
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
	QueueScriptUiCommandForScript(std::move(command));
	return effectiveValue;
}

void Corona::PushLuauUiStateForScript(lua_State* L, const std::string& mode)
{
	const bool bCompactMode = mode == "compact" || mode == "profile" || mode == "compact_no_profile";
	const bool bSkipScriptProfileStats = mode == "compact_no_profile" || mode == "full_no_profile";
	if (bCameraPathListDirty && !bCompactMode)
		RefreshCameraPathList();

	lua_newtable(L);

	PushIntegerField(L, "fps", static_cast<lua_Integer>(m_timer.GetFramesPerSecond()));
	PushStringField(L, "backend", renderBackend ? renderBackend->GetBackendName() : "None");
	PushBoolField(L, "show_culling_overlay", bShowCullingTextOverlay);
	PushBoolField(L, "final_screenshot_busy", bFinalScreenshotRequested || bFinalScreenshotCaptureInFlight);
	PushWideStringField(L, "final_screenshot_status", LastFinalScreenshotStatus);

	if (!bCompactMode)
	{
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
	}

	PushIntegerField(L, "gpu_timing_average_frames", static_cast<lua_Integer>(GpuTimingAverageFrameCount));
	PushNumberField(L, "frame_time_last_ms", FramePerfLastFrameMs);
	PushNumberField(L, "frame_time_average_ms", FramePerfAverageFrameMs);
	PushNumberField(L, "frame_begin_frame_last_ms", FramePerfLastBeginFrameMs);
	PushNumberField(L, "frame_begin_frame_average_ms", FramePerfAverageBeginFrameMs);
	PushNumberField(L, "frame_record_last_ms", FramePerfLastRecordMs);
	PushNumberField(L, "frame_record_average_ms", FramePerfAverageRecordMs);
	PushNumberField(L, "frame_execute_last_ms", FramePerfLastExecuteMs);
	PushNumberField(L, "frame_execute_average_ms", FramePerfAverageExecuteMs);
	PushNumberField(L, "frame_end_frame_last_ms", FramePerfLastEndFrameMs);
	PushNumberField(L, "frame_end_frame_average_ms", FramePerfAverageEndFrameMs);
	PushNumberField(L, "frame_render_wait_last_ms", FramePerfLastRenderWaitMs);
	PushNumberField(L, "frame_render_wait_average_ms", FramePerfAverageRenderWaitMs);
	PushNumberField(L, "cpu_update_last_ms", CpuUpdateLastTimeMs);
	PushNumberField(L, "cpu_update_average_ms", CpuUpdateAverageTimeMs);
	lua_newtable(L);
	for (UINT phaseIndex = 0; phaseIndex < RenderCommandPhaseCount; ++phaseIndex)
	{
		lua_newtable(L);
		PushStringField(L, "name", GetRenderCommandPhaseName(static_cast<ERenderCommandPhase>(phaseIndex)));
		PushNumberField(L, "last_ms", RenderCommandPhaseCompletedLastTimeMs[phaseIndex]);
		PushNumberField(L, "average_ms", RenderCommandPhaseAverageTimeMs[phaseIndex]);
		PushIntegerField(L, "sample_count", static_cast<lua_Integer>(RenderCommandPhaseHistoryMs[phaseIndex].size()));
		lua_rawseti(L, -2, phaseIndex + 1);
	}
	lua_setfield(L, -2, "render_command_phases");
	lua_newtable(L);
	for (UINT phaseIndex = 0; phaseIndex < SceneFlushPhaseCount; ++phaseIndex)
	{
		lua_newtable(L);
		PushStringField(L, "name", GetSceneFlushPhaseName(static_cast<ESceneFlushPhase>(phaseIndex)));
		PushNumberField(L, "last_ms", SceneFlushPhaseCompletedLastTimeMs[phaseIndex]);
		PushNumberField(L, "average_ms", SceneFlushPhaseAverageTimeMs[phaseIndex]);
		PushIntegerField(L, "sample_count", static_cast<lua_Integer>(SceneFlushPhaseHistoryMs[phaseIndex].size()));
		lua_rawseti(L, -2, phaseIndex + 1);
	}
	lua_setfield(L, -2, "scene_flush_phases");
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
	if (!bSkipScriptProfileStats)
	{
		PushScriptProfileStatsForScript(L);
		lua_setfield(L, -2, "script_profile_functions");
	}
	if (!bCompactMode && !bSkipScriptProfileStats)
	{
		PushScriptProfileSamplesForScript(L);
		lua_setfield(L, -2, "script_profile_samples");
	}
	std::wstring scriptProfileStatus;
	{
		std::lock_guard<std::mutex> lock(ScriptProfileMutex);
		scriptProfileStatus = ScriptProfileLastDumpStatus;
	}
	PushWideStringField(L, "script_profile_status", scriptProfileStatus);
	lua_newtable(L);
	int pushedPassIndex = 1;
	for (UINT passIndex = 0; passIndex < GpuPassCount; ++passIndex)
	{
		if (GpuPassLastTimeMs[passIndex] <= 0.0f && GpuPassAverageTimeMs[passIndex] <= 0.0f &&
			CpuPassLastTimeMs[passIndex] <= 0.0f && CpuPassAverageTimeMs[passIndex] <= 0.0f)
			continue;

		lua_newtable(L);
		PushStringField(L, "name", GetGpuPassName(static_cast<EGpuPass>(passIndex)));
		PushNumberField(L, "last_ms", GpuPassLastTimeMs[passIndex]);
		PushNumberField(L, "average_ms", GpuPassAverageTimeMs[passIndex]);
		PushIntegerField(L, "sample_count", static_cast<lua_Integer>(GpuPassHistoryMs[passIndex].size()));
		PushNumberField(L, "cpu_last_ms", CpuPassLastTimeMs[passIndex]);
		PushNumberField(L, "cpu_average_ms", CpuPassAverageTimeMs[passIndex]);
		PushIntegerField(L, "cpu_sample_count", static_cast<lua_Integer>(CpuPassHistoryMs[passIndex].size()));
		lua_rawseti(L, -2, pushedPassIndex++);
	}
	lua_setfield(L, -2, "gpu_passes");

	if (bCompactMode)
	{
		PushStringField(L, "shader_error", renderBackend ? renderBackend->GetErrorString() : "");
		return;
	}

	PushIntegerField(L, "aa_mode", static_cast<lua_Integer>(AntiAliasingMode));
	PushIntegerField(L, "dlss_quality", static_cast<lua_Integer>(DLSSQualityMode));
	PushBoolField(L, "dlss_available", bDLSSAvailable);
	PushBoolField(L, "dlss_rr_available", bDLSSRRAvailable);
	PushIntegerField(L, "render_width", static_cast<lua_Integer>(RenderWidth));
	PushIntegerField(L, "render_height", static_cast<lua_Integer>(RenderHeight));
	{
		ImGuiIO& io = ImGui::GetIO();
		const float displayWidth = io.DisplaySize.x > 0.0f ? io.DisplaySize.x : static_cast<float>(m_width);
		const float displayHeight = io.DisplaySize.y > 0.0f ? io.DisplaySize.y : static_cast<float>(m_height);
		PushNumberField(L, "display_width", displayWidth);
		PushNumberField(L, "display_height", displayHeight);
		PushIntegerField(L, "window_width", static_cast<lua_Integer>(m_width));
		PushIntegerField(L, "window_height", static_cast<lua_Integer>(m_height));
	}
	PushIntegerField(L, "dlss_jitter_phase_count", static_cast<lua_Integer>(DLSSJitterPhaseCount));
	PushIntegerField(L, "dlss_jitter_phase_count_auto", static_cast<lua_Integer>(DLSSJitterPhaseCountAuto));
	PushIntegerField(L, "dlss_jitter_phase_override", static_cast<lua_Integer>(DLSSJitterPhaseCountOverride));
	PushNumberField(L, "dlss_jitter_phase_scale", DLSSJitterPhaseScale);
	PushNumberField(L, "camera_turn_speed", m_turnSpeed);
	PushBoolField(L, "debug_visualization_available",
		renderBackend && BufferVisualizeGraphicsPipeline);
	PushBoolField(L, "visualize_buffers", bDebugDraw);
	PushBoolField(L, "draw_histogram", bDrawHistogram);
	PushIntegerField(L, "fullscreen_debug_buffer", static_cast<lua_Integer>(FullscreenDebugBuffer));
	PushIntegerField(L, "rendering_mode", static_cast<lua_Integer>(RenderingMode));

	PushBoolField(L, "enable_direct_diffuse", bEnableDirectDiffuse);
	PushBoolField(L, "enable_direct_specular", bEnableDirectSpecular);
	PushBoolField(L, "enable_specular_gi", bEnableSpecularGI);
	PushBoolField(L, "rt_reflection_ser", bEnableRTReflectionSER);
	PushBoolField(L, "rt_reflection_ser_available", renderBackend && renderBackend->SupportsShaderExecutionReordering());
	PushBoolField(L, "enable_diffuse_gi", bEnableDiffuseGI);
	PushBoolField(L, "rt_diffuse_gi_ser", bEnableRTDiffuseGISER);
	PushBoolField(L, "rt_diffuse_gi_ser_available", renderBackend && renderBackend->SupportsShaderExecutionReordering());
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
		PushScriptEntity(L, pointLight.EntityHandle);
		lua_setfield(L, -2, "entity");
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
		const EAntiAliasingMode requestedMode = static_cast<EAntiAliasingMode>(
			std::clamp(readInt(), 0, static_cast<int>(EAntiAliasingMode::COUNT) - 1));
		const EAntiAliasingMode normalizedMode = NormalizeAntiAliasingMode(RenderingMode, requestedMode);
		if (normalizedMode == previousMode)
			return true;
		ApplyRenderingAndAAMode(RenderingMode, normalizedMode);
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
		const ERenderingMode requestedMode = static_cast<ERenderingMode>(std::clamp(readInt(), 0, 1));
		const ERenderingMode previousMode = RenderingMode;
		const EAntiAliasingMode previousAAMode = AntiAliasingMode;
		ApplyRenderingAndAAMode(requestedMode, AntiAliasingMode);
		if (RenderingMode == previousMode && AntiAliasingMode == previousAAMode)
			return true;
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
			{
				LightDir = normalized;
				UpdateMainDirectionalLightEntityFromState();
			}
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
			UpdatePointLightEntity(*pointLight);
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
	if (setBool("hide_game_ui", bScriptGameUiHidden)) return true;
	if (setBool("show_culling_overlay", bShowCullingTextOverlay)) return true;
	if (setBool("draw_histogram", bDrawHistogram)) return true;
	if (setBool("enable_direct_diffuse", bEnableDirectDiffuse, true)) return true;
	if (setBool("enable_direct_specular", bEnableDirectSpecular, true)) return true;
	if (setBool("enable_specular_gi", bEnableSpecularGI, true)) return true;
	if (setBool("rt_reflection_ser", bEnableRTReflectionSER, true)) return true;
	if (setBool("enable_diffuse_gi", bEnableDiffuseGI, true)) return true;
	if (setBool("rt_diffuse_gi_ser", bEnableRTDiffuseGISER, true)) return true;
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
	if (setFloat("light_intensity", LightIntensity)) { if (lastSetterChanged) UpdateMainDirectionalLightEntityFromState(); return true; }
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
	if (name == "reset_script_profile")
	{
		ResetScriptProfileStats();
		return true;
	}
	if (name == "dump_script_profile")
	{
		return DumpScriptProfileStats();
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
			DestroyPointLightEntity(PointLights.back());
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
		DestroyPointLightEntity(*it);
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
		for (PointLightState& pointLight : PointLights)
			DestroyPointLightEntity(pointLight);
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

	{
		std::lock_guard<std::mutex> lock(ScriptProfileMutex);
		ScriptProfileStats.clear();
		ScriptProfileCurrentFrameStats.clear();
		ScriptProfileLastFrameStats.clear();
		ScriptProfileCurrentFrameTotalMs = 0.0;
		ScriptProfileLastFrameTotalMs = 0.0;
		ScriptProfileSamples.clear();
		ScriptProfileLastSampleTickNs = 0;
		ScriptProfileLastDumpStatus.clear();
	}
	bScriptGameUiHidden = false;

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
	lua_callbacks(L)->userdata = this;
	lua_pushlightuserdata(L, this);
	lua_setfield(L, LUA_REGISTRYINDEX, kCoronaRegistryKey);
	RegisterCoronaApi(L);
	RegisterImGuiApi(L);
	RegisterQueuedUiApi(L);
	StartLuauScriptProfileSampler(L);
	AppendCpuRuntimeTrace(L"[Luau] initialized");
}

void Corona::StartLuauScriptProfileSampler(lua_State* L)
{
	StopLuauScriptProfileSampler();
	if (!L)
		return;

	ScriptProfileLuaCallbacks.store(lua_callbacks(L), std::memory_order_release);
	ScriptProfileActiveDepth.store(0, std::memory_order_release);
	ScriptProfileSamplerTicksNs.store(0, std::memory_order_relaxed);
	ScriptProfileSamplerRequests.store(0, std::memory_order_relaxed);
	ScriptProfileLastSampleTickNs = 0;
	bScriptProfileSamplerStop.store(false, std::memory_order_release);
	ScriptProfileSamplerThread = std::thread(&Corona::ScriptProfileSamplerLoop, this);
}

void Corona::StopLuauScriptProfileSampler()
{
	bScriptProfileSamplerStop.store(true, std::memory_order_release);
	ScriptProfileActiveDepth.store(0, std::memory_order_release);
	if (ScriptProfileSamplerThread.joinable())
		ScriptProfileSamplerThread.join();

	lua_Callbacks* callbacks = ScriptProfileLuaCallbacks.exchange(nullptr, std::memory_order_acq_rel);
	if (callbacks && callbacks->interrupt == LuauScriptProfileInterrupt)
		callbacks->interrupt = nullptr;
}

void Corona::ScriptProfileSamplerLoop()
{
	const int sampleHz = std::max(1, ScriptProfileSampleHz);
	const auto sampleInterval = std::chrono::nanoseconds(1000000000LL / sampleHz);
	auto lastSampleTime = std::chrono::steady_clock::now();
	bool bWasActive = false;

	while (!bScriptProfileSamplerStop.load(std::memory_order_acquire))
	{
		const auto now = std::chrono::steady_clock::now();
		if (ScriptProfileActiveDepth.load(std::memory_order_acquire) <= 0)
		{
			lastSampleTime = now;
			bWasActive = false;
			std::this_thread::sleep_for(std::chrono::microseconds(100));
			continue;
		}

		if (!bWasActive)
		{
			lastSampleTime = now;
			bWasActive = true;
			std::this_thread::sleep_for(std::chrono::microseconds(100));
			continue;
		}

		if (now - lastSampleTime >= sampleInterval)
		{
			const uint64_t elapsedNs = static_cast<uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(now - lastSampleTime).count());
			ScriptProfileSamplerTicksNs.fetch_add(elapsedNs, std::memory_order_relaxed);
			ScriptProfileSamplerRequests.fetch_add(1, std::memory_order_relaxed);

			lua_Callbacks* callbacks = ScriptProfileLuaCallbacks.load(std::memory_order_acquire);
			if (callbacks)
				callbacks->interrupt = LuauScriptProfileInterrupt;

			lastSampleTime = now;
		}
		else
		{
			std::this_thread::sleep_for(std::chrono::microseconds(100));
		}
	}
}

void Corona::RunStartupLuauScript(bool bShowLoadingProgress)
{
	if (!bEnableStartupLuauScript)
		return;
	if (!ScriptState || !ScriptState->L)
		InitLuauScripting();
	if (!ScriptState || !ScriptState->L)
		return;

	auto sortScriptPaths = [](std::vector<std::filesystem::path>& paths)
	{
		std::sort(paths.begin(), paths.end(), [](const std::filesystem::path& a, const std::filesystem::path& b)
		{
#if CORONA_PLATFORM_IS_WINDOWS
			return _wcsicmp(a.filename().c_str(), b.filename().c_str()) < 0;
#else
			std::string lhs = a.filename().string();
			std::string rhs = b.filename().string();
			std::transform(lhs.begin(), lhs.end(), lhs.begin(), [](unsigned char c)
			{
				return static_cast<char>(std::tolower(c));
			});
			std::transform(rhs.begin(), rhs.end(), rhs.begin(), [](unsigned char c)
			{
				return static_cast<char>(std::tolower(c));
			});
			return lhs < rhs;
#endif
		});
	};

	auto appendDirectLuauScripts = [&](const std::filesystem::path& directory, std::vector<std::filesystem::path>& output)
	{
		std::error_code entryEc;
		if (!std::filesystem::is_directory(directory, entryEc))
			return;

		std::vector<std::filesystem::path> paths;
		for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory, entryEc))
		{
			if (entryEc)
				break;
			if (entry.is_regular_file(entryEc) && entry.path().extension() == L".luau")
				paths.push_back(entry.path());
		}
		sortScriptPaths(paths);
		output.insert(output.end(), paths.begin(), paths.end());
	};

	const std::filesystem::path startupDir = GetAssetFullPath(L"scripts\\startup");
	std::wstring startupMode = StartupLuauMode.empty() ? L"platformer" : StartupLuauMode;
	if (startupMode != L"dungeon" && startupMode != L"sandbox" && startupMode != L"sponza" && startupMode != L"spine_benchmark" && startupMode != L"grass_demo" && startupMode != L"terrain_demo" && startupMode != L"particle_demo")
		startupMode = L"platformer";

	std::vector<std::filesystem::path> scriptPaths;
	// "sponza" mode is the free-flight Sponza sandbox — it intentionally
	// runs *only* the common scripts (imgui controls etc.) and skips the
	// per-mode game scripts so the scene stays a clean RT playground.
	if (startupMode != L"sponza")
		appendDirectLuauScripts(startupDir / startupMode, scriptPaths);
	appendDirectLuauScripts(startupDir / L"common", scriptPaths);
	appendDirectLuauScripts(startupDir, scriptPaths);

	if (scriptPaths.empty())
	{
		std::error_code ec;
		const std::filesystem::path legacyScriptPath = GetAssetFullPath(L"scripts\\startup_pistol_spin.luau");
		if (std::filesystem::exists(legacyScriptPath, ec))
			scriptPaths.push_back(legacyScriptPath);
	}

	AppendCpuRuntimeTrace(
		L"[Luau] startup mode=" + startupMode +
		L", script count=" + std::to_wstring(scriptPaths.size()));
	for (size_t scriptIndex = 0; scriptIndex < scriptPaths.size(); ++scriptIndex)
	{
		const std::filesystem::path& scriptPath = scriptPaths[scriptIndex];
		const float scriptProgress =
			0.95f + 0.025f * (scriptPaths.empty() ? 1.0f : static_cast<float>(scriptIndex) / static_cast<float>(scriptPaths.size()));
		if (bShowLoadingProgress)
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

	int callResult = LUA_OK;
	{
		ScopedLuauScriptProfileExecution profileExecution(this);
		callResult = lua_pcall(L, 0, 1, 0);
	}
	if (callResult != 0)
	{
		AppendCpuRuntimeTrace(L"[Luau] startup script error: " + Utf8ToWideLocal(LuaToString(L, -1)));
		lua_pop(L, 1);
		return false;
	}

	CoronaECS::ScriptInstance loadedScript;
	loadedScript.SourceName = scriptPath.wstring();
	loadedScript.bPassEntityToCallbacks = false;

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

	if (HasScriptCallbacks(loadedScript))
	{
		const bool hasUpdate = loadedScript.UpdateRef != LUA_REFNIL;
		const bool hasShutdown = loadedScript.ShutdownRef != LUA_REFNIL;
		const bool hasImGui = loadedScript.ImGuiRef != LUA_REFNIL;
		const bool hasUi = loadedScript.UiRef != LUA_REFNIL;
		if (AttachEntityScriptForScript(
			GetWorldEntityForScript(),
			loadedScript.UpdateRef,
			loadedScript.ShutdownRef,
			loadedScript.ImGuiRef,
			loadedScript.UiRef,
			loadedScript.SourceName,
			loadedScript.bPassEntityToCallbacks))
		{
			AppendCpuRuntimeTrace(
				L"[Luau] script attached to World update=" + std::to_wstring(hasUpdate ? 1 : 0) +
				L", shutdown=" + std::to_wstring(hasShutdown ? 1 : 0) +
				L", imgui=" + std::to_wstring(hasImGui ? 1 : 0) +
				L", ui=" + std::to_wstring(hasUi ? 1 : 0) +
				L", path=" + scriptPath.wstring());
		}
		else
		{
			UnrefScriptInstance(L, loadedScript);
			AppendCpuRuntimeTrace(L"[Luau] failed to attach script to World: " + scriptPath.wstring());
			return false;
		}
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

	CallEntityScriptShutdownCallbacks();
}

void Corona::CallEntityScriptShutdownCallbacks()
{
	if (!ScriptState || !ScriptState->L)
		return;

	lua_State* L = ScriptState->L;
	const std::vector<CoronaECS::Entity> scriptEntities = EntityWorld.GetEntitiesWithScript();
	for (CoronaECS::Entity entity : scriptEntities)
	{
		CoronaECS::ScriptComponent* scriptComponent = EntityWorld.GetScript(entity);
		if (!scriptComponent || !scriptComponent->bEnabled)
			continue;

		std::vector<uint32_t> instanceIds;
		instanceIds.reserve(scriptComponent->Instances.size());
		for (auto scriptIt = scriptComponent->Instances.rbegin(); scriptIt != scriptComponent->Instances.rend(); ++scriptIt)
			instanceIds.push_back(scriptIt->InstanceId);

		for (uint32_t instanceId : instanceIds)
		{
			scriptComponent = EntityWorld.GetScript(entity);
			CoronaECS::ScriptInstance* script = FindScriptInstance(scriptComponent, instanceId);
			if (!script || !script->bEnabled)
				continue;

			const std::wstring sourceName = script->SourceName;
			const std::string nativeScriptName = script->NativeScriptName;
			if (!nativeScriptName.empty())
			{
				const auto nativeIt = NativeEntityScripts.find(nativeScriptName);
				if (nativeIt == NativeEntityScripts.end() || !nativeIt->second.Shutdown)
					continue;

				bool bNativeOk = true;
				const auto callStart = CpuClock::now();
				try
				{
					nativeIt->second.Shutdown(*this, entity);
				}
				catch (const std::exception& e)
				{
					bNativeOk = false;
					AppendCpuRuntimeTrace(L"[NativeScript] shutdown error in " + sourceName + L": " + Utf8ToWideLocal(e.what()));
				}
				catch (...)
				{
					bNativeOk = false;
					AppendCpuRuntimeTrace(L"[NativeScript] shutdown error in " + sourceName);
				}
				RecordScriptFunctionProfile(sourceName, "shutdown", ElapsedMilliseconds(callStart, CpuClock::now()), true);
				if (!bNativeOk)
				{
					scriptComponent = EntityWorld.GetScript(entity);
					if (CoronaECS::ScriptInstance* currentScript = FindScriptInstance(scriptComponent, instanceId))
						currentScript->bEnabled = false;
				}
				continue;
			}

			if (script->ShutdownRef == LUA_REFNIL)
				continue;

			const int shutdownRef = script->ShutdownRef;
			const bool bPassEntity = script->bPassEntityToCallbacks;
			lua_getref(L, shutdownRef);
			if (bPassEntity)
				PushScriptEntity(L, entity);
			const auto callStart = CpuClock::now();
			int result = LUA_OK;
			{
				ScopedLuauScriptProfileExecution profileExecution(this);
				result = lua_pcall(L, bPassEntity ? 1 : 0, 0, 0);
			}
			RecordScriptFunctionProfile(sourceName, "shutdown", ElapsedMilliseconds(callStart, CpuClock::now()), false);
			if (result != 0)
			{
				AppendCpuRuntimeTrace(L"[Luau] shutdown error in " + sourceName + L": " + Utf8ToWideLocal(LuaToString(L, -1)));
				lua_pop(L, 1);
			}

			scriptComponent = EntityWorld.GetScript(entity);
			if (CoronaECS::ScriptInstance* currentScript = FindScriptInstance(scriptComponent, instanceId))
			{
				if (currentScript->ShutdownRef == shutdownRef)
					UnrefLuaRef(L, currentScript->ShutdownRef);
			}
		}
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
	RunStartupLuauScript(false);
	MarkAllSceneObjectsForRenderSync();
	MarkAllPointLightsForRenderSync();
	CollectRenderFrameDeltas();
	ResetAllAccumulationState(false);

	AppendCpuRuntimeTrace(L"[Luau] reload complete");
}

void Corona::UpdateLuauScripting(float dt)
{
	PublishScriptProfileFrame();

	if (!ScriptState || !ScriptState->L)
		return;

	const bool bHasEntityScripts = EntityWorld.GetScriptComponentCount() > 0;
	if (!bHasEntityScripts)
		return;

	BuildLuauUi();
	UpdateEntityScripts(dt);
}

void Corona::UpdateEntityScripts(float dt)
{
	if (!ScriptState || !ScriptState->L)
		return;

	lua_State* L = ScriptState->L;
	const std::vector<CoronaECS::Entity> scriptEntities = EntityWorld.GetEntitiesWithScript();
	for (CoronaECS::Entity entity : scriptEntities)
	{
		CoronaECS::ScriptComponent* scriptComponent = EntityWorld.GetScript(entity);
		if (!scriptComponent || !scriptComponent->bEnabled)
			continue;

		std::vector<uint32_t> instanceIds;
		instanceIds.reserve(scriptComponent->Instances.size());
		for (const CoronaECS::ScriptInstance& script : scriptComponent->Instances)
			instanceIds.push_back(script.InstanceId);

		for (uint32_t instanceId : instanceIds)
		{
			scriptComponent = EntityWorld.GetScript(entity);
			CoronaECS::ScriptInstance* script = FindScriptInstance(scriptComponent, instanceId);
			if (!script || !script->bEnabled)
				continue;

			const std::wstring sourceName = script->SourceName;
			const std::string nativeScriptName = script->NativeScriptName;
			const glm::vec3 cameraPositionBeforeScript = m_camera.m_position;
			if (!nativeScriptName.empty())
			{
				const auto nativeIt = NativeEntityScripts.find(nativeScriptName);
				if (nativeIt != NativeEntityScripts.end() && nativeIt->second.Update)
				{
					bool bNativeOk = true;
					const auto callStart = CpuClock::now();
					try
					{
						nativeIt->second.Update(*this, entity, dt);
					}
					catch (const std::exception& e)
					{
						bNativeOk = false;
						AppendCpuRuntimeTrace(L"[NativeScript] update error in " + sourceName + L": " + Utf8ToWideLocal(e.what()));
					}
					catch (...)
					{
						bNativeOk = false;
						AppendCpuRuntimeTrace(L"[NativeScript] update error in " + sourceName);
					}
					RecordScriptFunctionProfile(sourceName, "update", ElapsedMilliseconds(callStart, CpuClock::now()), true);
					if (!bNativeOk)
					{
						scriptComponent = EntityWorld.GetScript(entity);
						if (CoronaECS::ScriptInstance* currentScript = FindScriptInstance(scriptComponent, instanceId))
							currentScript->bEnabled = false;
					}
				}

				const bool bNativeMovedCamera = glm::length(m_camera.m_position - cameraPositionBeforeScript) > 0.0001f;
				if (bScriptCameraControlEnabled && bNativeMovedCamera)
					m_camera.m_position = ResolveCameraPhysicsMovement(cameraPositionBeforeScript, m_camera.m_position);
				continue;
			}

			if (script->UpdateRef == LUA_REFNIL)
				continue;

			const int updateRef = script->UpdateRef;
			const bool bPassEntity = script->bPassEntityToCallbacks;
			lua_getref(L, updateRef);
			if (bPassEntity)
				PushScriptEntity(L, entity);
			lua_pushnumber(L, static_cast<double>(dt));
			const auto callStart = CpuClock::now();
			int result = LUA_OK;
			{
				ScopedLuauScriptProfileExecution profileExecution(this);
				result = lua_pcall(L, bPassEntity ? 2 : 1, 0, 0);
			}
			RecordScriptFunctionProfile(sourceName, "update", ElapsedMilliseconds(callStart, CpuClock::now()), false);
			if (result != 0)
			{
				AppendCpuRuntimeTrace(L"[Luau] update error in " + sourceName + L": " + Utf8ToWideLocal(LuaToString(L, -1)));
				lua_pop(L, 1);
				scriptComponent = EntityWorld.GetScript(entity);
				if (CoronaECS::ScriptInstance* currentScript = FindScriptInstance(scriptComponent, instanceId))
				{
					if (currentScript->UpdateRef == updateRef)
						UnrefLuaRef(L, currentScript->UpdateRef);
				}
			}

			const bool bScriptMovedCamera = glm::length(m_camera.m_position - cameraPositionBeforeScript) > 0.0001f;
			if (bScriptCameraControlEnabled && bScriptMovedCamera)
				m_camera.m_position = ResolveCameraPhysicsMovement(cameraPositionBeforeScript, m_camera.m_position);
		}
	}
}

void Corona::BuildLuauUi()
{
	if (!ScriptState || !ScriptState->L ||
		EntityWorld.GetScriptComponentCount() == 0)
	{
		std::lock_guard<std::mutex> lock(ScriptUiMutex);
		ScriptUiRenderCommands.clear();
		return;
	}

	ScriptUiBuildCommands.clear();
	BuildEntityScriptUi();

	std::lock_guard<std::mutex> lock(ScriptUiMutex);
	ScriptUiRenderCommands = ScriptUiBuildCommands;
}

void Corona::BuildEntityScriptUi()
{
	if (!ScriptState || !ScriptState->L)
		return;

	struct ScopedScriptUiSource
	{
		Corona* Owner = nullptr;
		bool bPreviousGameUi = false;

		ScopedScriptUiSource(Corona* owner, bool bGameUi)
			: Owner(owner)
			, bPreviousGameUi(owner ? owner->bCurrentScriptUiIsGame : false)
		{
			if (Owner)
				Owner->bCurrentScriptUiIsGame = bGameUi;
		}

		~ScopedScriptUiSource()
		{
			if (Owner)
				Owner->bCurrentScriptUiIsGame = bPreviousGameUi;
		}
	};

	lua_State* L = ScriptState->L;
	const std::vector<CoronaECS::Entity> scriptEntities = EntityWorld.GetEntitiesWithScript();
	for (CoronaECS::Entity entity : scriptEntities)
	{
		CoronaECS::ScriptComponent* scriptComponent = EntityWorld.GetScript(entity);
		if (!scriptComponent || !scriptComponent->bEnabled)
			continue;

		std::vector<uint32_t> instanceIds;
		instanceIds.reserve(scriptComponent->Instances.size());
		for (const CoronaECS::ScriptInstance& script : scriptComponent->Instances)
			instanceIds.push_back(script.InstanceId);

		for (uint32_t instanceId : instanceIds)
		{
			scriptComponent = EntityWorld.GetScript(entity);
			CoronaECS::ScriptInstance* script = FindScriptInstance(scriptComponent, instanceId);
			if (!script || !script->bEnabled)
				continue;

			const std::wstring sourceName = script->SourceName;
			const bool bStartupSource = IsStartupScriptSource(sourceName);
			const bool bGameUiSource = !bStartupSource || IsPlatformerStartupScriptSource(sourceName);
			if (bScriptGameUiHidden && bGameUiSource)
				continue;
			ScopedScriptUiSource uiSourceScope(this, bGameUiSource);

			const std::string nativeScriptName = script->NativeScriptName;
			if (!nativeScriptName.empty())
			{
				const auto nativeIt = NativeEntityScripts.find(nativeScriptName);
				if (nativeIt == NativeEntityScripts.end() || !nativeIt->second.Ui)
					continue;

				bool bNativeOk = true;
				const auto callStart = CpuClock::now();
				try
				{
					nativeIt->second.Ui(*this, entity);
				}
				catch (const std::exception& e)
				{
					bNativeOk = false;
					AppendCpuRuntimeTrace(L"[NativeScript] ui error in " + sourceName + L": " + Utf8ToWideLocal(e.what()));
				}
				catch (...)
				{
					bNativeOk = false;
					AppendCpuRuntimeTrace(L"[NativeScript] ui error in " + sourceName);
				}
				RecordScriptFunctionProfile(sourceName, "ui", ElapsedMilliseconds(callStart, CpuClock::now()), true);
				if (!bNativeOk)
				{
					scriptComponent = EntityWorld.GetScript(entity);
					if (CoronaECS::ScriptInstance* currentScript = FindScriptInstance(scriptComponent, instanceId))
						currentScript->bEnabled = false;
				}
				continue;
			}

			if (script->UiRef == LUA_REFNIL)
				continue;

			const int uiRef = script->UiRef;
			const bool bPassEntity = script->bPassEntityToCallbacks;
			lua_getref(L, uiRef);
			if (bPassEntity)
				PushScriptEntity(L, entity);
			const auto callStart = CpuClock::now();
			int result = LUA_OK;
			{
				ScopedLuauScriptProfileExecution profileExecution(this);
				result = lua_pcall(L, bPassEntity ? 1 : 0, 0, 0);
			}
			RecordScriptFunctionProfile(sourceName, "ui", ElapsedMilliseconds(callStart, CpuClock::now()), false);
			if (result != 0)
			{
				AppendCpuRuntimeTrace(L"[Luau] ui error in " + sourceName + L": " + Utf8ToWideLocal(LuaToString(L, -1)));
				lua_pop(L, 1);
				scriptComponent = EntityWorld.GetScript(entity);
				if (CoronaECS::ScriptInstance* currentScript = FindScriptInstance(scriptComponent, instanceId))
				{
					if (currentScript->UiRef == uiRef)
						UnrefLuaRef(L, currentScript->UiRef);
				}
			}
		}
	}
}

void Corona::RenderQueuedLuauUi(bool bRenderToolUi, bool bRenderGameUi)
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
		if ((command.bGameUi && !bRenderGameUi) || (!command.bGameUi && !bRenderToolUi))
			continue;

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
		case ScriptUiCommandType::OverlayLine:
		{
			ImDrawList* foregroundDrawList = ImGui::GetForegroundDrawList();
			foregroundDrawList->AddLine(
				ImVec2(command.FloatValue, command.MinValue),
				ImVec2(command.MaxValue, command.Vec3Value.x),
				ToImU32(command.ColorValue),
				std::max(static_cast<float>(command.IntValue) / 100.0f, 1.0f));
			break;
		}
		case ScriptUiCommandType::OverlayRect:
		{
			ImDrawList* foregroundDrawList = ImGui::GetForegroundDrawList();
			const float width = std::max(command.MaxValue, 0.0f);
			const float height = std::max(command.Vec3Value.x, 0.0f);
			const float thickness = std::max(static_cast<float>(command.IntValue) / 100.0f, 1.0f);
			const float rounding = std::max(static_cast<float>(command.MinIntValue) / 100.0f, 0.0f);
			foregroundDrawList->AddRect(
				ImVec2(command.FloatValue, command.MinValue),
				ImVec2(command.FloatValue + width, command.MinValue + height),
				ToImU32(command.ColorValue),
				rounding,
				0,
				thickness);
			break;
		}
		case ScriptUiCommandType::OverlayRectFilled:
		{
			ImDrawList* foregroundDrawList = ImGui::GetForegroundDrawList();
			const float width = std::max(command.MaxValue, 0.0f);
			const float height = std::max(command.Vec3Value.x, 0.0f);
			const float rounding = std::max(static_cast<float>(command.MinIntValue) / 100.0f, 0.0f);
			foregroundDrawList->AddRectFilled(
				ImVec2(command.FloatValue, command.MinValue),
				ImVec2(command.FloatValue + width, command.MinValue + height),
				ToImU32(command.ColorValue),
				rounding);
			break;
		}
		case ScriptUiCommandType::OverlayButton:
		{
			ImDrawList* foregroundDrawList = ImGui::GetForegroundDrawList();
			const float width = std::max(command.MaxValue, 1.0f);
			const float height = std::clamp(command.Vec3Value.x, 16.0f, 180.0f);
			const float rounding = std::max(static_cast<float>(command.MinIntValue) / 100.0f, 0.0f);
			const ImVec2 buttonMin(command.FloatValue, command.MinValue);
			const ImVec2 buttonMax(command.FloatValue + width, command.MinValue + height);
			const ImGuiIO& io = ImGui::GetIO();
			const bool hovered =
				io.MousePos.x >= buttonMin.x &&
				io.MousePos.x <= buttonMax.x &&
				io.MousePos.y >= buttonMin.y &&
				io.MousePos.y <= buttonMax.y;
			if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
				clickedResults[command.Id] = true;

			const glm::vec4 fillColor = hovered ? command.SecondaryColorValue : command.ColorValue;
			foregroundDrawList->AddRectFilled(buttonMin, buttonMax, ToImU32(fillColor), rounding);
			foregroundDrawList->AddRect(buttonMin, buttonMax, ToImU32(command.TertiaryColorValue), rounding, 0, 1.35f);

			const float accentWidth = std::min(5.0f, width * 0.08f);
			foregroundDrawList->AddRectFilled(
				buttonMin,
				ImVec2(buttonMin.x + accentWidth, buttonMax.y),
				ToImU32(command.TertiaryColorValue),
				rounding);

			if (!command.Label.empty())
			{
				const ImVec2 textSize = ImGui::CalcTextSize(command.Label.c_str());
				const ImVec2 textPos(
					buttonMin.x + width * 0.5f - textSize.x * 0.5f,
					buttonMin.y + height * 0.5f - textSize.y * 0.5f);
				foregroundDrawList->AddText(
					ImVec2(textPos.x + 1.0f, textPos.y + 1.0f),
					IM_COL32(0, 0, 0, 210),
					command.Label.c_str());
				foregroundDrawList->AddText(textPos, IM_COL32(246, 252, 255, 245), command.Label.c_str());
			}
			break;
		}
		case ScriptUiCommandType::OverlayProgressBar:
		{
			ImDrawList* foregroundDrawList = ImGui::GetForegroundDrawList();
			const float width = std::max(command.MaxValue, 1.0f);
			const float height = std::clamp(command.Vec3Value.x, 2.0f, 80.0f);
			const float fraction = std::clamp(static_cast<float>(command.IntValue) / 10000.0f, 0.0f, 1.0f);
			const ImVec2 barMin(command.FloatValue, command.MinValue);
			const ImVec2 barMax(command.FloatValue + width, command.MinValue + height);
			const ImVec2 fillMax(command.FloatValue + width * fraction, barMax.y);
			foregroundDrawList->AddRectFilled(barMin, barMax, ToImU32(command.SecondaryColorValue), 3.0f);
			if (fillMax.x > barMin.x)
				foregroundDrawList->AddRectFilled(barMin, fillMax, ToImU32(command.ColorValue), 3.0f);
			foregroundDrawList->AddRect(barMin, barMax, ToImU32(command.TertiaryColorValue), 3.0f, 0, 1.0f);
			if (!command.Label.empty())
			{
				const ImVec2 textSize = ImGui::CalcTextSize(command.Label.c_str());
				const ImVec2 textPos(
					command.FloatValue + width * 0.5f - textSize.x * 0.5f,
					command.MinValue + height * 0.5f - textSize.y * 0.5f);
				foregroundDrawList->AddText(
					ImVec2(textPos.x + 1.0f, textPos.y + 1.0f),
					IM_COL32(0, 0, 0, 200),
					command.Label.c_str());
				foregroundDrawList->AddText(textPos, IM_COL32(255, 255, 255, 235), command.Label.c_str());
			}
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
		case ScriptUiCommandType::WorldText:
		{
			ImGuiIO& io = ImGui::GetIO();
			const float displayWidth = io.DisplaySize.x > 0.0f ? io.DisplaySize.x : static_cast<float>(m_width);
			const float displayHeight = io.DisplaySize.y > 0.0f ? io.DisplaySize.y : static_cast<float>(m_height);
			if (displayWidth <= 0.0f || displayHeight <= 0.0f || command.Label.empty())
				break;

			glm::vec4 clipPosition = glm::vec4(command.Vec3Value, 1.0f) * glm::transpose(UnjitteredViewProjMat);
			if (clipPosition.w <= 0.0001f)
				break;

			const float invW = 1.0f / clipPosition.w;
			const float ndcX = clipPosition.x * invW;
			const float ndcY = clipPosition.y * invW;
			if (ndcX < -1.25f || ndcX > 1.25f || ndcY < -1.25f || ndcY > 1.25f)
				break;

			const float screenX = (ndcX * 0.5f + 0.5f) * displayWidth + command.FloatValue;
			const float screenY = (-ndcY * 0.5f + 0.5f) * displayHeight + command.MinValue;
			const ImVec2 textSize = ImGui::CalcTextSize(command.Label.c_str());
			const ImVec2 textPos(screenX - textSize.x * 0.5f, screenY - textSize.y * 0.5f);
			ImDrawList* foregroundDrawList = ImGui::GetForegroundDrawList();
			foregroundDrawList->AddText(
				ImVec2(textPos.x + 1.0f, textPos.y + 1.0f),
				IM_COL32(0, 0, 0, 210),
				command.Label.c_str());
			foregroundDrawList->AddText(textPos, ToImU32(command.ColorValue), command.Label.c_str());
			break;
		}
		case ScriptUiCommandType::WorldProgressBar:
		{
			ImGuiIO& io = ImGui::GetIO();
			const float displayWidth = io.DisplaySize.x > 0.0f ? io.DisplaySize.x : static_cast<float>(m_width);
			const float displayHeight = io.DisplaySize.y > 0.0f ? io.DisplaySize.y : static_cast<float>(m_height);
			if (displayWidth <= 0.0f || displayHeight <= 0.0f)
				break;

			glm::vec4 clipPosition = glm::vec4(command.Vec3Value, 1.0f) * glm::transpose(UnjitteredViewProjMat);
			if (clipPosition.w <= 0.0001f)
				break;

			const float invW = 1.0f / clipPosition.w;
			const float ndcX = clipPosition.x * invW;
			const float ndcY = clipPosition.y * invW;
			if (ndcX < -1.25f || ndcX > 1.25f || ndcY < -1.25f || ndcY > 1.25f)
				break;

			const float screenX = (ndcX * 0.5f + 0.5f) * displayWidth;
			const float screenY = (-ndcY * 0.5f + 0.5f) * displayHeight;
			const float width = std::max(command.MinValue, 24.0f);
			const float height = std::clamp(command.MaxValue, 4.0f, 28.0f);
			const float fraction = std::clamp(command.FloatValue, 0.0f, 1.0f);
			const ImVec2 barMin(screenX - width * 0.5f, screenY);
			const ImVec2 barMax(screenX + width * 0.5f, screenY + height);
			const ImVec2 fillMax(barMin.x + width * fraction, barMax.y);
			ImDrawList* foregroundDrawList = ImGui::GetForegroundDrawList();
			foregroundDrawList->AddRectFilled(
				ImVec2(barMin.x - 2.0f, barMin.y - 2.0f),
				ImVec2(barMax.x + 2.0f, barMax.y + 2.0f),
				IM_COL32(0, 0, 0, 155),
				3.0f);
			foregroundDrawList->AddRectFilled(barMin, barMax, ToImU32(command.SecondaryColorValue), 2.0f);
			if (fillMax.x > barMin.x)
				foregroundDrawList->AddRectFilled(barMin, fillMax, ToImU32(command.ColorValue), 2.0f);
			foregroundDrawList->AddRect(barMin, barMax, ToImU32(command.TertiaryColorValue), 2.0f);

			if (!command.Label.empty())
			{
				const ImVec2 textSize = ImGui::CalcTextSize(command.Label.c_str());
				const ImVec2 textPos(screenX - textSize.x * 0.5f, barMin.y - textSize.y - 3.0f);
				foregroundDrawList->AddText(
					ImVec2(textPos.x + 1.0f, textPos.y + 1.0f),
					IM_COL32(0, 0, 0, 210),
					command.Label.c_str());
				foregroundDrawList->AddText(textPos, IM_COL32(255, 255, 255, 235), command.Label.c_str());
			}
			break;
		}
		case ScriptUiCommandType::WorldHealthBar:
		{
			ImGuiIO& io = ImGui::GetIO();
			const float displayWidth = io.DisplaySize.x > 0.0f ? io.DisplaySize.x : static_cast<float>(m_width);
			const float displayHeight = io.DisplaySize.y > 0.0f ? io.DisplaySize.y : static_cast<float>(m_height);
			if (displayWidth <= 0.0f || displayHeight <= 0.0f)
				break;

			glm::vec4 clipPosition = glm::vec4(command.Vec3Value, 1.0f) * glm::transpose(UnjitteredViewProjMat);
			if (clipPosition.w <= 0.0001f)
				break;

			const float invW = 1.0f / clipPosition.w;
			const float ndcX = clipPosition.x * invW;
			const float ndcY = clipPosition.y * invW;
			if (ndcX < -1.25f || ndcX > 1.25f || ndcY < -1.25f || ndcY > 1.25f)
				break;

			const float screenX = (ndcX * 0.5f + 0.5f) * displayWidth;
			const float screenY = (-ndcY * 0.5f + 0.5f) * displayHeight;
			const float width = std::max(command.MinValue, 24.0f);
			const float height = std::clamp(command.MaxValue, 4.0f, 28.0f);
			const float fraction = std::clamp(command.FloatValue, 0.0f, 1.0f);
			const ImVec2 barMin(screenX - width * 0.5f, screenY);
			const ImVec2 barMax(screenX + width * 0.5f, screenY + height);
			const ImVec2 fillMax(barMin.x + width * fraction, barMax.y);
			const ImU32 fillColor =
				fraction > 0.55f ? IM_COL32(70, 220, 92, 235) :
				fraction > 0.25f ? IM_COL32(244, 189, 66, 235) :
				IM_COL32(236, 72, 72, 235);

			ImDrawList* foregroundDrawList = ImGui::GetForegroundDrawList();
			foregroundDrawList->AddRectFilled(
				ImVec2(barMin.x - 2.0f, barMin.y - 2.0f),
				ImVec2(barMax.x + 2.0f, barMax.y + 2.0f),
				IM_COL32(0, 0, 0, 155),
				3.0f);
			foregroundDrawList->AddRectFilled(barMin, barMax, IM_COL32(42, 35, 35, 225), 2.0f);
			if (fillMax.x > barMin.x)
				foregroundDrawList->AddRectFilled(barMin, fillMax, fillColor, 2.0f);
			foregroundDrawList->AddRect(barMin, barMax, IM_COL32(255, 255, 255, 190), 2.0f);

			if (!command.Label.empty())
			{
				const ImVec2 textSize = ImGui::CalcTextSize(command.Label.c_str());
				const ImVec2 textPos(screenX - textSize.x * 0.5f, barMin.y - textSize.y - 3.0f);
				foregroundDrawList->AddText(
					ImVec2(textPos.x + 1.0f, textPos.y + 1.0f),
					IM_COL32(0, 0, 0, 210),
					command.Label.c_str());
				foregroundDrawList->AddText(textPos, IM_COL32(255, 255, 255, 235), command.Label.c_str());
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
	if (!ScriptState || !ScriptState->L ||
		EntityWorld.GetScriptComponentCount() == 0)
		return;

	const bool previousFrameActive = bLuauImGuiFrameActive;
	bLuauImGuiFrameActive = true;
	DrawEntityScriptImGui();

	bLuauImGuiFrameActive = previousFrameActive;
}

void Corona::DrawEntityScriptImGui()
{
	if (!ScriptState || !ScriptState->L)
		return;

	lua_State* L = ScriptState->L;
	const std::vector<CoronaECS::Entity> scriptEntities = EntityWorld.GetEntitiesWithScript();
	for (CoronaECS::Entity entity : scriptEntities)
	{
		CoronaECS::ScriptComponent* scriptComponent = EntityWorld.GetScript(entity);
		if (!scriptComponent || !scriptComponent->bEnabled)
			continue;

		std::vector<uint32_t> instanceIds;
		instanceIds.reserve(scriptComponent->Instances.size());
		for (const CoronaECS::ScriptInstance& script : scriptComponent->Instances)
			instanceIds.push_back(script.InstanceId);

		for (uint32_t instanceId : instanceIds)
		{
			scriptComponent = EntityWorld.GetScript(entity);
			CoronaECS::ScriptInstance* script = FindScriptInstance(scriptComponent, instanceId);
			if (!script || !script->bEnabled)
				continue;

			const std::wstring sourceName = script->SourceName;
			const std::string nativeScriptName = script->NativeScriptName;
			if (!nativeScriptName.empty())
			{
				const auto nativeIt = NativeEntityScripts.find(nativeScriptName);
				if (nativeIt == NativeEntityScripts.end() || !nativeIt->second.ImGui)
					continue;

				bool bNativeOk = true;
				const auto callStart = CpuClock::now();
				try
				{
					nativeIt->second.ImGui(*this, entity);
				}
				catch (const std::exception& e)
				{
					bNativeOk = false;
					AppendCpuRuntimeTrace(L"[NativeScript] imgui error in " + sourceName + L": " + Utf8ToWideLocal(e.what()));
				}
				catch (...)
				{
					bNativeOk = false;
					AppendCpuRuntimeTrace(L"[NativeScript] imgui error in " + sourceName);
				}
				RecordScriptFunctionProfile(sourceName, "imgui", ElapsedMilliseconds(callStart, CpuClock::now()), true);
				if (!bNativeOk)
				{
					scriptComponent = EntityWorld.GetScript(entity);
					if (CoronaECS::ScriptInstance* currentScript = FindScriptInstance(scriptComponent, instanceId))
						currentScript->bEnabled = false;
				}
				continue;
			}

			if (script->ImGuiRef == LUA_REFNIL)
				continue;

			const int imguiRef = script->ImGuiRef;
			const bool bPassEntity = script->bPassEntityToCallbacks;
			lua_getref(L, imguiRef);
			if (bPassEntity)
				PushScriptEntity(L, entity);
			const auto callStart = CpuClock::now();
			int result = LUA_OK;
			{
				ScopedLuauScriptProfileExecution profileExecution(this);
				result = lua_pcall(L, bPassEntity ? 1 : 0, 0, 0);
			}
			RecordScriptFunctionProfile(sourceName, "imgui", ElapsedMilliseconds(callStart, CpuClock::now()), false);
			if (result != 0)
			{
				AppendCpuRuntimeTrace(L"[Luau] imgui error in " + sourceName + L": " + Utf8ToWideLocal(LuaToString(L, -1)));
				lua_pop(L, 1);
				scriptComponent = EntityWorld.GetScript(entity);
				if (CoronaECS::ScriptInstance* currentScript = FindScriptInstance(scriptComponent, instanceId))
				{
					if (currentScript->ImGuiRef == imguiRef)
						UnrefLuaRef(L, currentScript->ImGuiRef);
				}
			}
		}
	}
}

void Corona::DestroyEntityScriptComponent(CoronaECS::Entity entity)
{
	CoronaECS::ScriptComponent* scriptComponent = EntityWorld.GetScript(entity);
	if (!scriptComponent)
		return;

	if (ScriptState && ScriptState->L)
	{
		for (CoronaECS::ScriptInstance& script : scriptComponent->Instances)
			UnrefScriptInstance(ScriptState->L, script);
	}

	EntityWorld.RemoveScript(entity);
}

bool Corona::IsLuauImGuiFrameActive() const
{
	return bLuauImGuiFrameActive;
}

void Corona::ShutdownLuauScripting()
{
	if (!ScriptState)
		return;

	StopLuauScriptProfileSampler();

	if (ScriptState->L)
	{
		CallLuauShutdownCallbacks();
		const std::vector<CoronaECS::Entity> scriptEntities = EntityWorld.GetEntitiesWithScript();
		for (CoronaECS::Entity entity : scriptEntities)
			DestroyEntityScriptComponent(entity);
		lua_close(ScriptState->L);
		ScriptState->L = nullptr;
	}
	bool bHadScriptProfileStats = false;
	{
		std::lock_guard<std::mutex> lock(ScriptProfileMutex);
		bHadScriptProfileStats = !ScriptProfileStats.empty() || !ScriptProfileSamples.empty();
	}
	if (bHadScriptProfileStats)
		DumpScriptProfileStats();
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
	{
		std::lock_guard<std::mutex> lock(ScriptProfileMutex);
		ScriptProfileStats.clear();
		ScriptProfileCurrentFrameStats.clear();
		ScriptProfileLastFrameStats.clear();
		ScriptProfileCurrentFrameTotalMs = 0.0;
		ScriptProfileLastFrameTotalMs = 0.0;
		ScriptProfileSamples.clear();
		ScriptProfileLastDumpStatus.clear();
	}
	bScriptCameraControlEnabled = false;
	bScriptGameUiHidden = false;
	ScriptKeyDown.fill(false);
	bScriptRightMouseDown = false;
	bScriptMousePositionInitialized = false;
	bScriptGamepadConnected = false;
	ScriptGamepadLeftX = 0.0f;
	ScriptGamepadLeftY = 0.0f;
	ScriptGamepadRightX = 0.0f;
	ScriptGamepadRightY = 0.0f;
	ScriptGamepadLeftTrigger = 0.0f;
	ScriptGamepadRightTrigger = 0.0f;
	ScriptGamepadButtonsDown = 0;
	ClearScriptInputFrameState();
}
