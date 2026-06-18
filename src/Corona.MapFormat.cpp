// Corona map serialization — see Corona::SaveMapToFile / LoadMapFromFile
// declared in Corona.h. The on-disk format is a Luau-syntax table that the
// engine's existing Lua VM executes at load time, so there's no separate
// JSON parser to maintain. Format is human-readable + diff-friendly.

#include "stdafx.h"
#include "Corona.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

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
	double MapFormatElapsedMilliseconds(
		std::chrono::steady_clock::time_point start,
		std::chrono::steady_clock::time_point end)
	{
		return std::chrono::duration<double, std::milli>(end - start).count();
	}

	std::wstring MapFormatFormatMilliseconds(double milliseconds)
	{
		wchar_t buffer[64] = {};
		swprintf_s(buffer, L"%.3f", milliseconds);
		return buffer;
	}

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

		std::error_code ec;
		const uint64_t size64 = std::filesystem::file_size(path, ec);
		if (!ec && size64 <= static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
		{
			std::string out;
			out.resize(static_cast<size_t>(size64));
			if (!out.empty())
				f.read(out.data(), static_cast<std::streamsize>(out.size()));
			return f ? out : std::string();
		}

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

	enum class CachedMapMeshPrimitive : uint8_t
	{
		Unknown = 0,
		Asset,
		Terrain,
		GrassOnTerrain,
		Grass,
		BlockCharacter,
		Box,
	};

	struct CachedMapScript
	{
		bool bNative = false;
		std::string Value;
	};

	struct CachedMapMesh
	{
		bool bPresent = false;
		CachedMapMeshPrimitive Primitive = CachedMapMeshPrimitive::Unknown;
		std::string AssetPath;
		uint32_t Seed = 1;
		uint32_t BladeCount = 100000;
		float BladeHeight = 30.0f;
		float AreaSize = 1000.0f;
		uint32_t BladeSegments = 4;
		bool bProcedural = false;
		glm::vec3 Color = glm::vec3(0.72f, 0.72f, 0.72f);
		bool bBrickTexture = false;
		bool bFrontOnly = false;
		float UvRepeat = 1.0f;
		float UvRepeatY = -1.0f;
		std::string TextureKind;
		glm::vec3 Position = glm::vec3(0.0f);
		glm::vec3 Rotation = glm::vec3(0.0f);
		glm::vec3 Scale = glm::vec3(1.0f);
		float TargetExtent = 1.0f;
		bool bUseScale = false;
		float Roughness = 1.0f;
		float Metallic = 0.0f;
		bool bOverrideMaterial = false;
		bool bVisible = true;
		bool bRayTracing = true;
	};

	struct CachedMapLight
	{
		bool bPresent = false;
		CoronaECS::LightComponent Component;
		bool bHasPosition = false;
		glm::vec3 Position = glm::vec3(0.0f);
	};

	struct CachedMapEntity
	{
		std::string Name;
		CachedMapMesh Mesh;
		CachedMapLight Light;
		bool bHasCamera = false;
		std::vector<CachedMapScript> Scripts;
		uint32_t OriginalOrder = 0;
		uint32_t ChunkId = std::numeric_limits<uint32_t>::max();
	};

	struct CachedMapChunk
	{
		uint32_t Id = 0;
		uint32_t EntityCount = 0;
		uint32_t MeshCount = 0;
		uint32_t LightCount = 0;
		glm::vec3 BoundsMin = glm::vec3(0.0f);
		glm::vec3 BoundsMax = glm::vec3(0.0f);
		glm::vec3 Center = glm::vec3(0.0f);
		float Radius = 0.0f;
	};

	struct CachedMapGlobals
	{
		bool bHasWindParams = false;
		bool bHasWindTuning = false;
		bool bHasGrassBendOrigin = false;
		bool bHasGrassBendParams = false;
		bool bHasGrassRenderOrigin = false;
		bool bHasGrassRenderDistance = false;
		bool bHasTerrainDeformSphere = false;
		glm::vec4 WindParams = glm::vec4(0.0f);
		glm::vec4 WindTuning = glm::vec4(0.0f);
		glm::vec4 GrassBendOrigin = glm::vec4(0.0f);
		glm::vec4 GrassBendParams = glm::vec4(0.0f);
		glm::vec3 GrassRenderOrigin = glm::vec3(0.0f);
		float GrassRenderDistance = 0.0f;
		glm::vec4 TerrainDeformSphere = glm::vec4(0.0f);
	};

	struct CachedMapScene
	{
		std::vector<CachedMapEntity> Entities;
		std::vector<CachedMapChunk> Chunks;
		CachedMapGlobals Globals;
	};

	struct CachedMapSourceMeta
	{
		uint64_t Size = 0;
		int64_t WriteTime = 0;
	};

	struct CachedMapSceneHeader
	{
		char Magic[8] = {};
		uint32_t Version = 0;
		uint32_t HeaderSize = 0;
		uint64_t SourceSize = 0;
		int64_t SourceWriteTime = 0;
		uint32_t EntityCount = 0;
		uint32_t ChunkCount = 0;
	};

	constexpr char kCachedMapSceneMagic[8] = { 'C', 'R', 'N', 'M', 'A', 'P', 'S', '\0' };
	constexpr uint32_t kCachedMapSceneVersion = 2;
	constexpr uint32_t kCachedMapSceneMaxStringBytes = 1024u * 1024u;
	constexpr uint32_t kCachedMapSceneMaxEntities = 2u * 1000u * 1000u;
	constexpr uint32_t kCachedMapSceneMaxChunks = 256u * 1024u;
	constexpr uint32_t kCachedMapSceneMaxScriptsPerEntity = 1024u;
	constexpr uint32_t kInvalidCachedMapChunkId = std::numeric_limits<uint32_t>::max();
	constexpr float kCachedMapChunkCellSize = 2048.0f;
	constexpr bool kMapLoadDetailedReplayProfile = false;
	constexpr size_t kCachedMapSceneIoBufferBytes = 4u * 1024u * 1024u;

	template<typename T>
	bool WriteCachedValue(std::ofstream& file, const T& value)
	{
		file.write(reinterpret_cast<const char*>(&value), sizeof(T));
		return static_cast<bool>(file);
	}

	bool WriteCachedBytes(std::ofstream& file, const void* data, size_t size)
	{
		if (size == 0)
			return true;
		file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
		return static_cast<bool>(file);
	}

	bool WriteCachedString(std::ofstream& file, const std::string& value)
	{
		if (value.size() > kCachedMapSceneMaxStringBytes)
			return false;
		const uint32_t size = static_cast<uint32_t>(value.size());
		return WriteCachedValue(file, size) && WriteCachedBytes(file, value.data(), value.size());
	}

	bool WriteCachedBool(std::ofstream& file, bool value)
	{
		const uint8_t byte = value ? 1u : 0u;
		return WriteCachedValue(file, byte);
	}

	bool WriteCachedVec3(std::ofstream& file, const glm::vec3& value)
	{
		return
			WriteCachedValue(file, value.x) &&
			WriteCachedValue(file, value.y) &&
			WriteCachedValue(file, value.z);
	}

	bool WriteCachedVec4(std::ofstream& file, const glm::vec4& value)
	{
		return
			WriteCachedValue(file, value.x) &&
			WriteCachedValue(file, value.y) &&
			WriteCachedValue(file, value.z) &&
			WriteCachedValue(file, value.w);
	}

	template<typename T>
	bool ReadCachedValue(std::ifstream& file, T& value)
	{
		file.read(reinterpret_cast<char*>(&value), sizeof(T));
		return static_cast<bool>(file);
	}

	bool ReadCachedBytes(std::ifstream& file, void* data, size_t size)
	{
		if (size == 0)
			return true;
		file.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
		return static_cast<bool>(file);
	}

	bool ReadCachedString(std::ifstream& file, std::string& value)
	{
		uint32_t size = 0;
		if (!ReadCachedValue(file, size) || size > kCachedMapSceneMaxStringBytes)
			return false;
		value.clear();
		value.resize(size);
		return ReadCachedBytes(file, value.data(), value.size());
	}

	bool ReadCachedBool(std::ifstream& file, bool& value)
	{
		uint8_t byte = 0;
		if (!ReadCachedValue(file, byte) || byte > 1u)
			return false;
		value = byte != 0;
		return true;
	}

	bool ReadCachedVec3(std::ifstream& file, glm::vec3& value)
	{
		return
			ReadCachedValue(file, value.x) &&
			ReadCachedValue(file, value.y) &&
			ReadCachedValue(file, value.z);
	}

	bool ReadCachedVec4(std::ifstream& file, glm::vec4& value)
	{
		return
			ReadCachedValue(file, value.x) &&
			ReadCachedValue(file, value.y) &&
			ReadCachedValue(file, value.z) &&
			ReadCachedValue(file, value.w);
	}

	bool GetCachedMapSourceMeta(const std::filesystem::path& path, CachedMapSourceMeta& outMeta)
	{
		std::error_code ec;
		outMeta.Size = static_cast<uint64_t>(std::filesystem::file_size(path, ec));
		if (ec)
			return false;
		const auto writeTime = std::filesystem::last_write_time(path, ec);
		if (ec)
			return false;
		outMeta.WriteTime = static_cast<int64_t>(writeTime.time_since_epoch().count());
		return true;
	}

	std::filesystem::path GetCachedMapScenePath(const std::filesystem::path& sourcePath)
	{
		return std::filesystem::path(sourcePath.wstring() + L".cscene");
	}

	CachedMapMeshPrimitive ParseCachedMapMeshPrimitive(const std::string& primitive)
	{
		if (primitive == "asset")
			return CachedMapMeshPrimitive::Asset;
		if (primitive == "TERRAIN")
			return CachedMapMeshPrimitive::Terrain;
		if (primitive == "GRASS_ON_TERRAIN")
			return CachedMapMeshPrimitive::GrassOnTerrain;
		if (primitive == "GRASS")
			return CachedMapMeshPrimitive::Grass;
		if (primitive == "block_character")
			return CachedMapMeshPrimitive::BlockCharacter;
		if (primitive == "BOX" || primitive == "CUBE" || primitive == "PLANE" || primitive == "QUAD")
			return CachedMapMeshPrimitive::Box;
		return CachedMapMeshPrimitive::Unknown;
	}

	bool WriteCachedMapGlobals(std::ofstream& file, const CachedMapGlobals& globals)
	{
		return
			WriteCachedBool(file, globals.bHasWindParams) &&
			WriteCachedBool(file, globals.bHasWindTuning) &&
			WriteCachedBool(file, globals.bHasGrassBendOrigin) &&
			WriteCachedBool(file, globals.bHasGrassBendParams) &&
			WriteCachedBool(file, globals.bHasGrassRenderOrigin) &&
			WriteCachedBool(file, globals.bHasGrassRenderDistance) &&
			WriteCachedBool(file, globals.bHasTerrainDeformSphere) &&
			WriteCachedVec4(file, globals.WindParams) &&
			WriteCachedVec4(file, globals.WindTuning) &&
			WriteCachedVec4(file, globals.GrassBendOrigin) &&
			WriteCachedVec4(file, globals.GrassBendParams) &&
			WriteCachedVec3(file, globals.GrassRenderOrigin) &&
			WriteCachedValue(file, globals.GrassRenderDistance) &&
			WriteCachedVec4(file, globals.TerrainDeformSphere);
	}

	bool ReadCachedMapGlobals(std::ifstream& file, CachedMapGlobals& globals)
	{
		return
			ReadCachedBool(file, globals.bHasWindParams) &&
			ReadCachedBool(file, globals.bHasWindTuning) &&
			ReadCachedBool(file, globals.bHasGrassBendOrigin) &&
			ReadCachedBool(file, globals.bHasGrassBendParams) &&
			ReadCachedBool(file, globals.bHasGrassRenderOrigin) &&
			ReadCachedBool(file, globals.bHasGrassRenderDistance) &&
			ReadCachedBool(file, globals.bHasTerrainDeformSphere) &&
			ReadCachedVec4(file, globals.WindParams) &&
			ReadCachedVec4(file, globals.WindTuning) &&
			ReadCachedVec4(file, globals.GrassBendOrigin) &&
			ReadCachedVec4(file, globals.GrassBendParams) &&
			ReadCachedVec3(file, globals.GrassRenderOrigin) &&
			ReadCachedValue(file, globals.GrassRenderDistance) &&
			ReadCachedVec4(file, globals.TerrainDeformSphere);
	}

	bool WriteCachedMapMesh(std::ofstream& file, const CachedMapMesh& mesh)
	{
		const uint8_t primitive = static_cast<uint8_t>(mesh.Primitive);
		return
			WriteCachedBool(file, mesh.bPresent) &&
			WriteCachedValue(file, primitive) &&
			WriteCachedString(file, mesh.AssetPath) &&
			WriteCachedValue(file, mesh.Seed) &&
			WriteCachedValue(file, mesh.BladeCount) &&
			WriteCachedValue(file, mesh.BladeHeight) &&
			WriteCachedValue(file, mesh.AreaSize) &&
			WriteCachedValue(file, mesh.BladeSegments) &&
			WriteCachedBool(file, mesh.bProcedural) &&
			WriteCachedVec3(file, mesh.Color) &&
			WriteCachedBool(file, mesh.bBrickTexture) &&
			WriteCachedBool(file, mesh.bFrontOnly) &&
			WriteCachedValue(file, mesh.UvRepeat) &&
			WriteCachedValue(file, mesh.UvRepeatY) &&
			WriteCachedString(file, mesh.TextureKind) &&
			WriteCachedVec3(file, mesh.Position) &&
			WriteCachedVec3(file, mesh.Rotation) &&
			WriteCachedVec3(file, mesh.Scale) &&
			WriteCachedValue(file, mesh.TargetExtent) &&
			WriteCachedBool(file, mesh.bUseScale) &&
			WriteCachedValue(file, mesh.Roughness) &&
			WriteCachedValue(file, mesh.Metallic) &&
			WriteCachedBool(file, mesh.bOverrideMaterial) &&
			WriteCachedBool(file, mesh.bVisible) &&
			WriteCachedBool(file, mesh.bRayTracing);
	}

	bool ReadCachedMapMesh(std::ifstream& file, CachedMapMesh& mesh)
	{
		uint8_t primitive = 0;
		if (!ReadCachedBool(file, mesh.bPresent) ||
			!ReadCachedValue(file, primitive) ||
			primitive > static_cast<uint8_t>(CachedMapMeshPrimitive::Box))
		{
			return false;
		}
		mesh.Primitive = static_cast<CachedMapMeshPrimitive>(primitive);
		return
			ReadCachedString(file, mesh.AssetPath) &&
			ReadCachedValue(file, mesh.Seed) &&
			ReadCachedValue(file, mesh.BladeCount) &&
			ReadCachedValue(file, mesh.BladeHeight) &&
			ReadCachedValue(file, mesh.AreaSize) &&
			ReadCachedValue(file, mesh.BladeSegments) &&
			ReadCachedBool(file, mesh.bProcedural) &&
			ReadCachedVec3(file, mesh.Color) &&
			ReadCachedBool(file, mesh.bBrickTexture) &&
			ReadCachedBool(file, mesh.bFrontOnly) &&
			ReadCachedValue(file, mesh.UvRepeat) &&
			ReadCachedValue(file, mesh.UvRepeatY) &&
			ReadCachedString(file, mesh.TextureKind) &&
			ReadCachedVec3(file, mesh.Position) &&
			ReadCachedVec3(file, mesh.Rotation) &&
			ReadCachedVec3(file, mesh.Scale) &&
			ReadCachedValue(file, mesh.TargetExtent) &&
			ReadCachedBool(file, mesh.bUseScale) &&
			ReadCachedValue(file, mesh.Roughness) &&
			ReadCachedValue(file, mesh.Metallic) &&
			ReadCachedBool(file, mesh.bOverrideMaterial) &&
			ReadCachedBool(file, mesh.bVisible) &&
			ReadCachedBool(file, mesh.bRayTracing);
	}

	bool WriteCachedMapLight(std::ofstream& file, const CachedMapLight& light)
	{
		const uint8_t type = static_cast<uint8_t>(light.Component.Type);
		return
			WriteCachedBool(file, light.bPresent) &&
			WriteCachedValue(file, type) &&
			WriteCachedBool(file, light.Component.bEnabled) &&
			WriteCachedVec3(file, light.Component.Color) &&
			WriteCachedValue(file, light.Component.Intensity) &&
			WriteCachedVec3(file, light.Component.Direction) &&
			WriteCachedValue(file, light.Component.Radius) &&
			WriteCachedBool(file, light.Component.bCastShadow) &&
			WriteCachedValue(file, light.Component.InnerConeAngle) &&
			WriteCachedValue(file, light.Component.OuterConeAngle) &&
			WriteCachedBool(file, light.bHasPosition) &&
			WriteCachedVec3(file, light.Position);
	}

	bool ReadCachedMapLight(std::ifstream& file, CachedMapLight& light)
	{
		uint8_t type = 0;
		if (!ReadCachedBool(file, light.bPresent) ||
			!ReadCachedValue(file, type) ||
			type > static_cast<uint8_t>(CoronaECS::LightType::Spot))
		{
			return false;
		}
		light.Component.Type = static_cast<CoronaECS::LightType>(type);
		return
			ReadCachedBool(file, light.Component.bEnabled) &&
			ReadCachedVec3(file, light.Component.Color) &&
			ReadCachedValue(file, light.Component.Intensity) &&
			ReadCachedVec3(file, light.Component.Direction) &&
			ReadCachedValue(file, light.Component.Radius) &&
			ReadCachedBool(file, light.Component.bCastShadow) &&
			ReadCachedValue(file, light.Component.InnerConeAngle) &&
			ReadCachedValue(file, light.Component.OuterConeAngle) &&
			ReadCachedBool(file, light.bHasPosition) &&
			ReadCachedVec3(file, light.Position);
	}

	bool WriteCachedMapChunk(std::ofstream& file, const CachedMapChunk& chunk)
	{
		return
			WriteCachedValue(file, chunk.Id) &&
			WriteCachedValue(file, chunk.EntityCount) &&
			WriteCachedValue(file, chunk.MeshCount) &&
			WriteCachedValue(file, chunk.LightCount) &&
			WriteCachedVec3(file, chunk.BoundsMin) &&
			WriteCachedVec3(file, chunk.BoundsMax) &&
			WriteCachedVec3(file, chunk.Center) &&
			WriteCachedValue(file, chunk.Radius);
	}

	bool ReadCachedMapChunk(std::ifstream& file, CachedMapChunk& chunk)
	{
		return
			ReadCachedValue(file, chunk.Id) &&
			ReadCachedValue(file, chunk.EntityCount) &&
			ReadCachedValue(file, chunk.MeshCount) &&
			ReadCachedValue(file, chunk.LightCount) &&
			ReadCachedVec3(file, chunk.BoundsMin) &&
			ReadCachedVec3(file, chunk.BoundsMax) &&
			ReadCachedVec3(file, chunk.Center) &&
			ReadCachedValue(file, chunk.Radius);
	}

	bool GetCachedMapEntitySpatialPosition(const CachedMapEntity& entity, glm::vec3& outPosition)
	{
		if (entity.Mesh.bPresent)
		{
			outPosition = entity.Mesh.Position;
			return true;
		}
		if (entity.Light.bPresent &&
			entity.Light.Component.Type != CoronaECS::LightType::Directional &&
			entity.Light.bHasPosition)
		{
			outPosition = entity.Light.Position;
			return true;
		}
		return false;
	}

	int32_t CachedMapChunkCoord(float value)
	{
		return static_cast<int32_t>(std::floor(static_cast<double>(value / kCachedMapChunkCellSize)));
	}

	uint64_t MakeCachedMapChunkKey(int32_t x, int32_t z)
	{
		return
			(static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) |
			static_cast<uint64_t>(static_cast<uint32_t>(z));
	}

	void ExpandCachedMapChunkBounds(CachedMapChunk& chunk, const glm::vec3& position)
	{
		if (chunk.EntityCount == 0)
		{
			chunk.BoundsMin = position;
			chunk.BoundsMax = position;
		}
		else
		{
			chunk.BoundsMin = glm::min(chunk.BoundsMin, position);
			chunk.BoundsMax = glm::max(chunk.BoundsMax, position);
		}
		++chunk.EntityCount;
	}

	void FinalizeCachedMapChunk(CachedMapChunk& chunk)
	{
		chunk.Center = (chunk.BoundsMin + chunk.BoundsMax) * 0.5f;
		const glm::vec3 extent = (chunk.BoundsMax - chunk.BoundsMin) * 0.5f;
		chunk.Radius = std::sqrt(extent.x * extent.x + extent.y * extent.y + extent.z * extent.z);
	}

	void BuildCachedMapChunks(CachedMapScene& scene)
	{
		scene.Chunks.clear();
		std::unordered_map<uint64_t, uint32_t> chunkByKey;
		chunkByKey.reserve(std::max<size_t>(scene.Entities.size() / 64u, 16u));

		for (uint32_t entityIndex = 0; entityIndex < static_cast<uint32_t>(scene.Entities.size()); ++entityIndex)
		{
			CachedMapEntity& entity = scene.Entities[entityIndex];
			entity.OriginalOrder = entityIndex;
			entity.ChunkId = kInvalidCachedMapChunkId;

			// Scripts and cameras stay in the non-spatial prefix during replay
			// until script dependency tracking becomes chunk-aware.
			if (!entity.Scripts.empty() || entity.bHasCamera)
				continue;

			glm::vec3 position(0.0f);
			if (!GetCachedMapEntitySpatialPosition(entity, position))
				continue;

			const int32_t chunkX = CachedMapChunkCoord(position.x);
			const int32_t chunkZ = CachedMapChunkCoord(position.z);
			const uint64_t key = MakeCachedMapChunkKey(chunkX, chunkZ);
			uint32_t chunkId = kInvalidCachedMapChunkId;
			const auto found = chunkByKey.find(key);
			if (found != chunkByKey.end())
			{
				chunkId = found->second;
			}
			else
			{
				chunkId = static_cast<uint32_t>(scene.Chunks.size());
				chunkByKey[key] = chunkId;
				CachedMapChunk chunk;
				chunk.Id = chunkId;
				scene.Chunks.push_back(chunk);
			}

			entity.ChunkId = chunkId;
			CachedMapChunk& chunk = scene.Chunks[chunkId];
			ExpandCachedMapChunkBounds(chunk, position);
			if (entity.Mesh.bPresent)
				++chunk.MeshCount;
			if (entity.Light.bPresent)
				++chunk.LightCount;
		}

		for (CachedMapChunk& chunk : scene.Chunks)
			FinalizeCachedMapChunk(chunk);
	}

	float CachedMapChunkDistanceSq(const CachedMapChunk& chunk, const glm::vec3& cameraPosition)
	{
		const glm::vec3 d = chunk.Center - cameraPosition;
		return d.x * d.x + d.y * d.y + d.z * d.z;
	}

	std::vector<uint32_t> BuildCachedMapReplayOrder(
		const CachedMapScene& scene,
		const glm::vec3& cameraPosition)
	{
		std::vector<uint32_t> order;
		order.reserve(scene.Entities.size());

		std::vector<uint32_t> chunkEntityCounts(scene.Chunks.size(), 0u);
		uint32_t spatialEntityCount = 0;
		for (uint32_t entityIndex = 0; entityIndex < static_cast<uint32_t>(scene.Entities.size()); ++entityIndex)
		{
			const CachedMapEntity& entity = scene.Entities[entityIndex];
			if (entity.ChunkId == kInvalidCachedMapChunkId || entity.ChunkId >= scene.Chunks.size())
			{
				order.push_back(entityIndex);
			}
			else
			{
				++chunkEntityCounts[entity.ChunkId];
				++spatialEntityCount;
			}
		}

		std::vector<uint32_t> sortedChunks;
		sortedChunks.reserve(scene.Chunks.size());
		std::vector<uint32_t> chunkOffsets(scene.Chunks.size(), 0u);
		uint32_t offset = 0;
		for (uint32_t chunkId = 0; chunkId < static_cast<uint32_t>(scene.Chunks.size()); ++chunkId)
		{
			chunkOffsets[chunkId] = offset;
			const uint32_t count = chunkEntityCounts[chunkId];
			offset += count;
			if (count != 0)
				sortedChunks.push_back(chunkId);
		}

		std::vector<uint32_t> spatialByChunk(spatialEntityCount);
		std::vector<uint32_t> chunkWriteOffsets = chunkOffsets;
		for (uint32_t entityIndex = 0; entityIndex < static_cast<uint32_t>(scene.Entities.size()); ++entityIndex)
		{
			const CachedMapEntity& entity = scene.Entities[entityIndex];
			if (entity.ChunkId == kInvalidCachedMapChunkId || entity.ChunkId >= scene.Chunks.size())
				continue;
			spatialByChunk[chunkWriteOffsets[entity.ChunkId]++] = entityIndex;
		}

		std::stable_sort(sortedChunks.begin(), sortedChunks.end(),
			[&](uint32_t aIndex, uint32_t bIndex)
			{
				const CachedMapChunk& aChunk = scene.Chunks[aIndex];
				const CachedMapChunk& bChunk = scene.Chunks[bIndex];
				const float aDist = CachedMapChunkDistanceSq(aChunk, cameraPosition);
				const float bDist = CachedMapChunkDistanceSq(bChunk, cameraPosition);
				if (aDist != bDist)
					return aDist < bDist;
				return aIndex < bIndex;
			});

		for (const uint32_t chunkId : sortedChunks)
		{
			const uint32_t begin = chunkOffsets[chunkId];
			const uint32_t end = begin + chunkEntityCounts[chunkId];
			order.insert(order.end(), spatialByChunk.begin() + begin, spatialByChunk.begin() + end);
		}
		return order;
	}

	bool WriteCachedMapScene(
		const std::filesystem::path& cachePath,
		const CachedMapSourceMeta& sourceMeta,
		const CachedMapScene& scene)
	{
		if (scene.Entities.size() > kCachedMapSceneMaxEntities ||
			scene.Chunks.size() > kCachedMapSceneMaxChunks)
			return false;

		std::error_code ec;
		std::filesystem::create_directories(cachePath.parent_path(), ec);
		std::vector<char> ioBuffer(kCachedMapSceneIoBufferBytes);
		std::ofstream file;
		file.rdbuf()->pubsetbuf(ioBuffer.data(), static_cast<std::streamsize>(ioBuffer.size()));
		file.open(cachePath, std::ios::binary | std::ios::trunc);
		if (!file)
			return false;

		CachedMapSceneHeader header = {};
		std::memcpy(header.Magic, kCachedMapSceneMagic, sizeof(header.Magic));
		header.Version = kCachedMapSceneVersion;
		header.HeaderSize = sizeof(CachedMapSceneHeader);
		header.SourceSize = sourceMeta.Size;
		header.SourceWriteTime = sourceMeta.WriteTime;
		header.EntityCount = static_cast<uint32_t>(scene.Entities.size());
		header.ChunkCount = static_cast<uint32_t>(scene.Chunks.size());
		if (!WriteCachedValue(file, header) || !WriteCachedMapGlobals(file, scene.Globals))
			return false;

		for (const CachedMapChunk& chunk : scene.Chunks)
		{
			if (!WriteCachedMapChunk(file, chunk))
				return false;
		}

		for (const CachedMapEntity& entity : scene.Entities)
		{
			if (entity.Scripts.size() > kCachedMapSceneMaxScriptsPerEntity)
				return false;
			if (!WriteCachedString(file, entity.Name) ||
				!WriteCachedValue(file, entity.OriginalOrder) ||
				!WriteCachedValue(file, entity.ChunkId) ||
				!WriteCachedMapMesh(file, entity.Mesh) ||
				!WriteCachedMapLight(file, entity.Light) ||
				!WriteCachedBool(file, entity.bHasCamera))
			{
				return false;
			}
			const uint32_t scriptCount = static_cast<uint32_t>(entity.Scripts.size());
			if (!WriteCachedValue(file, scriptCount))
				return false;
			for (const CachedMapScript& script : entity.Scripts)
			{
				if (!WriteCachedBool(file, script.bNative) || !WriteCachedString(file, script.Value))
					return false;
			}
		}
		return static_cast<bool>(file);
	}

	bool ReadCachedMapScene(
		const std::filesystem::path& cachePath,
		const CachedMapSourceMeta& sourceMeta,
		CachedMapScene& scene)
	{
		std::vector<char> ioBuffer(kCachedMapSceneIoBufferBytes);
		std::ifstream file;
		file.rdbuf()->pubsetbuf(ioBuffer.data(), static_cast<std::streamsize>(ioBuffer.size()));
		file.open(cachePath, std::ios::binary);
		if (!file)
			return false;

		CachedMapSceneHeader header = {};
		if (!ReadCachedValue(file, header) ||
			std::memcmp(header.Magic, kCachedMapSceneMagic, sizeof(header.Magic)) != 0 ||
			header.Version != kCachedMapSceneVersion ||
			header.HeaderSize != sizeof(CachedMapSceneHeader) ||
			header.SourceSize != sourceMeta.Size ||
			header.SourceWriteTime != sourceMeta.WriteTime ||
			header.EntityCount > kCachedMapSceneMaxEntities ||
			header.ChunkCount > kCachedMapSceneMaxChunks)
		{
			return false;
		}

		scene = {};
		scene.Chunks.reserve(header.ChunkCount);
		scene.Entities.reserve(header.EntityCount);
		if (!ReadCachedMapGlobals(file, scene.Globals))
			return false;

		for (uint32_t i = 0; i < header.ChunkCount; ++i)
		{
			CachedMapChunk chunk;
			if (!ReadCachedMapChunk(file, chunk) || chunk.Id != i)
				return false;
			scene.Chunks.push_back(chunk);
		}

		for (uint32_t i = 0; i < header.EntityCount; ++i)
		{
			CachedMapEntity entity;
			if (!ReadCachedString(file, entity.Name) ||
				!ReadCachedValue(file, entity.OriginalOrder) ||
				!ReadCachedValue(file, entity.ChunkId) ||
				!ReadCachedMapMesh(file, entity.Mesh) ||
				!ReadCachedMapLight(file, entity.Light) ||
				!ReadCachedBool(file, entity.bHasCamera))
			{
				return false;
			}
			if (entity.ChunkId != kInvalidCachedMapChunkId && entity.ChunkId >= header.ChunkCount)
				return false;
			uint32_t scriptCount = 0;
			if (!ReadCachedValue(file, scriptCount) || scriptCount > kCachedMapSceneMaxScriptsPerEntity)
				return false;
			entity.Scripts.reserve(scriptCount);
			for (uint32_t scriptIndex = 0; scriptIndex < scriptCount; ++scriptIndex)
			{
				CachedMapScript script;
				if (!ReadCachedBool(file, script.bNative) || !ReadCachedString(file, script.Value))
					return false;
				entity.Scripts.push_back(std::move(script));
			}
			scene.Entities.push_back(std::move(entity));
		}
		return static_cast<bool>(file);
	}

	void ParseCachedMapScripts(lua_State* L, int entityIdx, CachedMapEntity& entity)
	{
		lua_getfield(L, entityIdx, "scripts");
		if (lua_istable(L, -1))
		{
			const int scriptsIdx = lua_gettop(L);
			const int scriptCount = static_cast<int>(lua_objlen(L, scriptsIdx));
			entity.Scripts.reserve(std::max(scriptCount, 0));
			for (int si = 1; si <= scriptCount; ++si)
			{
				lua_rawgeti(L, scriptsIdx, si);
				if (lua_istable(L, -1))
				{
					const int sIdx = lua_gettop(L);
					CachedMapScript script;
					if (LuaGetString(L, sIdx, "file", script.Value) && !script.Value.empty())
					{
						script.bNative = false;
						entity.Scripts.push_back(std::move(script));
					}
					else if (LuaGetString(L, sIdx, "native", script.Value) && !script.Value.empty())
					{
						script.bNative = true;
						entity.Scripts.push_back(std::move(script));
					}
				}
				lua_pop(L, 1);
			}
		}
		lua_pop(L, 1);
	}

	void ParseCachedMapMesh(lua_State* L, int meshIdx, CachedMapMesh& mesh)
	{
		mesh = {};
		mesh.bPresent = true;

		std::string primitive;
		LuaGetString(L, meshIdx, "primitive", primitive);
		mesh.Primitive = ParseCachedMapMeshPrimitive(primitive);

		lua_getfield(L, meshIdx, "params");
		const int paramsIdx = lua_gettop(L);
		if (lua_istable(L, paramsIdx))
		{
			double value = 0.0;
			if (LuaGetNumber(L, paramsIdx, "seed", value))
				mesh.Seed = static_cast<uint32_t>(value);
			if (LuaGetNumber(L, paramsIdx, "blade_count", value))
				mesh.BladeCount = static_cast<uint32_t>(value);
			if (LuaGetNumber(L, paramsIdx, "blade_height", value))
				mesh.BladeHeight = static_cast<float>(value);
			if (LuaGetNumber(L, paramsIdx, "area_size", value))
				mesh.AreaSize = static_cast<float>(value);
			if (LuaGetNumber(L, paramsIdx, "blade_segments", value))
				mesh.BladeSegments = static_cast<uint32_t>(value);
			LuaGetBool(L, paramsIdx, "procedural", mesh.bProcedural);
		}
		lua_pop(L, 1);

		if (mesh.Primitive == CachedMapMeshPrimitive::Asset)
			LuaGetString(L, meshIdx, "path", mesh.AssetPath);

		if (mesh.Primitive == CachedMapMeshPrimitive::Box)
		{
			mesh.bFrontOnly = (primitive == "PLANE" || primitive == "QUAD");
			if (!LuaGetVec3(L, meshIdx, "color", mesh.Color))
				LuaGetVec3(L, meshIdx, "base_color", mesh.Color);
			LuaGetBool(L, meshIdx, "brick_texture", mesh.bBrickTexture);
			LuaGetBool(L, meshIdx, "front_only", mesh.bFrontOnly);
			double value = mesh.UvRepeat;
			if (LuaGetNumber(L, meshIdx, "uv_repeat", value))
				mesh.UvRepeat = static_cast<float>(value);
			value = mesh.UvRepeatY;
			if (LuaGetNumber(L, meshIdx, "uv_repeat_y", value))
				mesh.UvRepeatY = static_cast<float>(value);
			LuaGetString(L, meshIdx, "texture_kind", mesh.TextureKind);
		}

		lua_getfield(L, meshIdx, "transform");
		if (lua_istable(L, -1))
		{
			const int transformIdx = lua_gettop(L);
			LuaGetVec3(L, transformIdx, "position", mesh.Position);
			LuaGetVec3(L, transformIdx, "rotation", mesh.Rotation);
			double targetExtent = mesh.TargetExtent;
			if (LuaGetNumber(L, transformIdx, "target_extent", targetExtent))
				mesh.TargetExtent = static_cast<float>(targetExtent);
			if (LuaGetVec3(L, transformIdx, "scale", mesh.Scale))
				mesh.bUseScale = true;
		}
		lua_pop(L, 1);

		lua_getfield(L, meshIdx, "material");
		if (lua_istable(L, -1))
		{
			const int materialIdx = lua_gettop(L);
			double value = mesh.Roughness;
			if (LuaGetNumber(L, materialIdx, "roughness", value))
				mesh.Roughness = static_cast<float>(value);
			value = mesh.Metallic;
			if (LuaGetNumber(L, materialIdx, "metallic", value))
				mesh.Metallic = static_cast<float>(value);
			LuaGetBool(L, materialIdx, "override", mesh.bOverrideMaterial);
		}
		lua_pop(L, 1);

		LuaGetBool(L, meshIdx, "visible", mesh.bVisible);
		LuaGetBool(L, meshIdx, "ray_tracing", mesh.bRayTracing);
	}

	void ParseCachedMapLight(lua_State* L, int lightIdx, CachedMapLight& light)
	{
		light = {};
		light.bPresent = true;
		CoronaECS::LightComponent& comp = light.Component;

		std::string typeStr;
		if (LuaGetString(L, lightIdx, "type", typeStr))
		{
			std::transform(typeStr.begin(), typeStr.end(), typeStr.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			comp.Type =
				typeStr == "directional" || typeStr == "sun" ? CoronaECS::LightType::Directional :
				typeStr == "spot" || typeStr == "spotlight" ? CoronaECS::LightType::Spot :
				CoronaECS::LightType::Point;
		}
		LuaGetVec3(L, lightIdx, "direction", comp.Direction);
		LuaGetVec3(L, lightIdx, "color", comp.Color);
		double value = comp.Intensity;
		if (LuaGetNumber(L, lightIdx, "intensity", value))
			comp.Intensity = static_cast<float>(value);
		value = comp.Radius;
		if (LuaGetNumber(L, lightIdx, "radius", value))
			comp.Radius = static_cast<float>(value);
		value = comp.InnerConeAngle;
		if (LuaGetNumber(L, lightIdx, "inner_cone_angle", value) ||
			LuaGetNumber(L, lightIdx, "innerConeAngle", value))
		{
			comp.InnerConeAngle = std::clamp(static_cast<float>(value), 0.0f, glm::pi<float>() - 0.001f);
		}
		value = comp.OuterConeAngle;
		if (LuaGetNumber(L, lightIdx, "outer_cone_angle", value) ||
			LuaGetNumber(L, lightIdx, "outerConeAngle", value))
		{
			comp.OuterConeAngle = std::clamp(static_cast<float>(value), 0.001f, glm::pi<float>());
		}
		comp.InnerConeAngle = std::clamp(comp.InnerConeAngle, 0.0f, glm::pi<float>() - 0.001f);
		comp.OuterConeAngle = std::clamp(comp.OuterConeAngle, comp.InnerConeAngle + 0.001f, glm::pi<float>());
		if (!LuaGetBool(L, lightIdx, "cast_shadow", comp.bCastShadow))
			LuaGetBool(L, lightIdx, "castShadow", comp.bCastShadow);
		LuaGetBool(L, lightIdx, "enabled", comp.bEnabled);
		light.bHasPosition =
			(comp.Type != CoronaECS::LightType::Directional) &&
			LuaGetVec3(L, lightIdx, "position", light.Position);
	}

	void ParseCachedMapGlobals(lua_State* L, int rootIdx, CachedMapGlobals& globals)
	{
		lua_getfield(L, rootIdx, "globals");
		if (lua_istable(L, -1))
		{
			const int globalsIdx = lua_gettop(L);
			lua_getfield(L, globalsIdx, "wind");
			if (lua_istable(L, -1))
			{
				const int windIdx = lua_gettop(L);
				globals.bHasWindParams = LuaGetVec4(L, windIdx, "params", globals.WindParams);
				globals.bHasWindTuning = LuaGetVec4(L, windIdx, "tuning", globals.WindTuning);
			}
			lua_pop(L, 1);

			lua_getfield(L, globalsIdx, "grass");
			if (lua_istable(L, -1))
			{
				const int grassIdx = lua_gettop(L);
				globals.bHasGrassBendOrigin = LuaGetVec4(L, grassIdx, "bend_origin", globals.GrassBendOrigin);
				globals.bHasGrassBendParams = LuaGetVec4(L, grassIdx, "bend_params", globals.GrassBendParams);
				globals.bHasGrassRenderOrigin = LuaGetVec3(L, grassIdx, "render_origin", globals.GrassRenderOrigin);
				double renderDistance = globals.GrassRenderDistance;
				if (LuaGetNumber(L, grassIdx, "render_distance", renderDistance))
				{
					globals.GrassRenderDistance = static_cast<float>(renderDistance);
					globals.bHasGrassRenderDistance = true;
				}
			}
			lua_pop(L, 1);

			globals.bHasTerrainDeformSphere =
				LuaGetVec4(L, globalsIdx, "terrain_deform_sphere", globals.TerrainDeformSphere);
		}
		lua_pop(L, 1);
	}

	bool ParseCachedMapSceneFromLua(
		lua_State* L,
		const std::filesystem::path& path,
		const std::string& source,
		CachedMapScene& outScene,
		std::wstring* outError)
	{
		const int stackTop = lua_gettop(L);
		size_t bytecodeSize = 0;
		char* bytecode = luau_compile(source.data(), source.size(), nullptr, &bytecodeSize);
		if (!bytecode)
		{
			if (outError) *outError = L"map compile failed: " + path.wstring();
			lua_settop(L, stackTop);
			return false;
		}

		const std::string chunkName = "=" + PlatformWideToUtf8(path.wstring());
		const int loadResult = luau_load(L, chunkName.c_str(), bytecode, bytecodeSize, 0);
		std::free(bytecode);
		if (loadResult != 0)
		{
			if (outError)
				*outError = L"map load failed: " + PlatformUtf8ToWide(lua_isstring(L, -1) ? lua_tostring(L, -1) : "");
			lua_settop(L, stackTop);
			return false;
		}
		if (lua_pcall(L, 0, 1, 0) != 0)
		{
			if (outError)
				*outError = L"map exec error: " + PlatformUtf8ToWide(lua_isstring(L, -1) ? lua_tostring(L, -1) : "");
			lua_settop(L, stackTop);
			return false;
		}
		if (!lua_istable(L, -1))
		{
			if (outError) *outError = L"map didn't return a table";
			lua_settop(L, stackTop);
			return false;
		}

		CachedMapScene scene;
		const int rootIdx = lua_gettop(L);
		lua_getfield(L, rootIdx, "entities");
		if (lua_istable(L, -1))
		{
			const int entitiesIdx = lua_gettop(L);
			const int count = static_cast<int>(lua_objlen(L, entitiesIdx));
			scene.Entities.reserve(std::max(count, 0));
			for (int i = 1; i <= count; ++i)
			{
				lua_rawgeti(L, entitiesIdx, i);
				if (lua_istable(L, -1))
				{
					const int entityIdx = lua_gettop(L);
					CachedMapEntity entity;
					LuaGetString(L, entityIdx, "name", entity.Name);

					lua_getfield(L, entityIdx, "mesh");
					if (lua_istable(L, -1))
						ParseCachedMapMesh(L, lua_gettop(L), entity.Mesh);
					lua_pop(L, 1);

					lua_getfield(L, entityIdx, "light");
					if (lua_istable(L, -1))
						ParseCachedMapLight(L, lua_gettop(L), entity.Light);
					lua_pop(L, 1);

					lua_getfield(L, entityIdx, "camera");
					entity.bHasCamera = lua_istable(L, -1) != 0;
					lua_pop(L, 1);

					ParseCachedMapScripts(L, entityIdx, entity);
					scene.Entities.push_back(std::move(entity));
				}
				lua_pop(L, 1);
			}
		}
		lua_pop(L, 1);

		ParseCachedMapGlobals(L, rootIdx, scene.Globals);
		lua_settop(L, stackTop);
		BuildCachedMapChunks(scene);
		outScene = std::move(scene);
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

void Corona::PersistLastEditorMapName(const std::wstring& name) const
{
	// "editor_empty" is the internal blank-canvas default, not a user map —
	// never let it overwrite the remembered last map.
	if (name.empty() || name == L"editor_empty")
		return;
	const std::filesystem::path path = RuntimePaths::RootDirectory() / L"assets" / L"maps" / L".last_loaded_map";
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);
	std::wofstream f(path, std::ios::trunc);
	if (f.is_open())
		f << name;
}

std::wstring Corona::ReadPersistedLastEditorMapName() const
{
	const std::filesystem::path path = RuntimePaths::RootDirectory() / L"assets" / L"maps" / L".last_loaded_map";
	std::wifstream f(path);
	if (!f.is_open())
		return L"";
	std::wstring name;
	std::getline(f, name);
	while (!name.empty() && (name.back() == L'\n' || name.back() == L'\r' || name.back() == L' ' || name.back() == L'\t'))
		name.pop_back();
	return name;
}

void Corona::ClearScriptSpawnedScene()
{
	// Bulk remove script-owned SceneObjects. Calling RemoveSceneObject for
	// every handle erases from the middle of SceneObjects and becomes
	// quadratic on large streamed maps.
	if (!ScriptObjects.empty())
	{
		std::unordered_set<SceneObjectHandle> scriptHandles;
		scriptHandles.reserve(ScriptObjects.size());
		for (const auto& [h, _] : ScriptObjects)
			scriptHandles.insert(h);

		std::vector<SceneObject> keptObjects;
		keptObjects.reserve(SceneObjects.size() > scriptHandles.size() ? SceneObjects.size() - scriptHandles.size() : 0u);
		size_t removedObjectCount = 0;
		for (const SceneObject& object : SceneObjects)
		{
			if (scriptHandles.find(object.Handle) == scriptHandles.end())
			{
				keptObjects.push_back(object);
				continue;
			}

			const CoronaECS::Entity entity = object.EntityHandle;
			DestroyEntityScriptComponent(entity);
			if (!bMapReplayInProgress)
				MarkSceneObjectRenderRemoved(object.Handle);
			EntityWorld.DestroyEntity(entity);
			if (SponzaObject == object.Handle)
				SponzaObject = InvalidSceneObjectHandle;
			if (BuddhaObject == object.Handle)
				BuddhaObject = InvalidSceneObjectHandle;
			if (ShaderBallObject == object.Handle)
				ShaderBallObject = InvalidSceneObjectHandle;
			if (PistolObject == object.Handle)
				PistolObject = InvalidSceneObjectHandle;
			if (MirrorCubeObject == object.Handle)
				MirrorCubeObject = InvalidSceneObjectHandle;
			++removedObjectCount;
		}

		SceneObjects = std::move(keptObjects);
		if (removedObjectCount != 0)
			MarkCpuPhysicsSceneDirty();
	}

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
	// Drop the runtime PointLights[] mirror — without this, lights
	// loaded from scene_state.cfg (legacy path) or any prior runtime
	// state survive past the ECS wipe and load_map then appends the
	// .map's lights on top, duplicating the count and breaking ECS
	// <-> PointLights[] index parity that LightingPS depends on.
	for (PointLightState& pl : PointLights)
		MarkPointLightRenderRemoved(pl.Id);
	PointLights.clear();
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
		const char* typeName =
			light->Type == CoronaECS::LightType::Directional ? "directional" :
			light->Type == CoronaECS::LightType::Spot ? "spot" :
			"point";
		out << "        type      = " << EscapeLuaString(typeName) << ",\n";
		// Position only matters for point lights (directional sun is shared
		// by the whole world). Persisting it makes inspector position edits
		// survive map reload — without this the runtime PointLight.Position
		// got reset to the entity-creation default on every load.
		if (light->Type != CoronaECS::LightType::Directional)
		{
			if (const auto* trans = ecs.GetTransform(entity))
				out << "        position  = " << Vec3Lua(trans->GetPosition()) << ",\n";
		}
		out << "        direction = " << Vec3Lua(light->Direction) << ",\n";
		out << "        color     = " << Vec3Lua(light->Color) << ",\n";
		out << "        intensity = " << light->Intensity << ",\n";
		out << "        radius    = " << light->Radius << ",\n";
		out << "        cast_shadow = " << (light->bCastShadow ? "true" : "false") << ",\n";
		if (light->Type == CoronaECS::LightType::Spot)
		{
			out << "        inner_cone_angle = " << light->InnerConeAngle << ",\n";
			out << "        outer_cone_angle = " << light->OuterConeAngle << ",\n";
		}
		out << "        enabled   = " << (light->bEnabled ? "true" : "false") << ",\n";
		out << "      },\n";
		out << "    },\n";
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
	{
		const std::filesystem::path path = ResolveMapPath(name);
		CachedMapSourceMeta sourceMeta = {};
		if (!GetCachedMapSourceMeta(path, sourceMeta))
		{
			if (outError) *outError = L"map not found / unreadable: " + path.wstring();
			return false;
		}

		if (!ScriptState || !ScriptState->L)
		{
			if (outError) *outError = L"Luau VM not initialized";
			return false;
		}

		const std::filesystem::path cachePath = GetCachedMapScenePath(path);
		auto mapScene = std::make_shared<CachedMapScene>();
		bool bLoadedFromSceneCache = ReadCachedMapScene(cachePath, sourceMeta, *mapScene);
		std::future<bool> cacheWriteFuture;
		if (bLoadedFromSceneCache)
		{
			AppendCpuRuntimeTrace(
				L"[MapCache] scene cache hit: " + cachePath.wstring() +
				L", entities=" + std::to_wstring(mapScene->Entities.size()) +
				L", chunks=" + std::to_wstring(mapScene->Chunks.size()));
		}
		else
		{
			const std::string source = ReadFile(path);
			if (source.empty())
			{
				if (outError) *outError = L"map not found / empty: " + path.wstring();
				return false;
			}

			if (!ParseCachedMapSceneFromLua(ScriptState->L, path, source, *mapScene, outError))
				return false;

			AppendCpuRuntimeTrace(
				L"[MapCache] parsed source map: " + path.wstring() +
				L", bytes=" + std::to_wstring(source.size()) +
				L", entities=" + std::to_wstring(mapScene->Entities.size()) +
				L", chunks=" + std::to_wstring(mapScene->Chunks.size()) +
				L", cache=" + cachePath.wstring());
			cacheWriteFuture = std::async(
				std::launch::async,
				[cachePath, sourceMeta, mapScene]()
				{
					return WriteCachedMapScene(cachePath, sourceMeta, *mapScene);
				});
		}

		const bool bPreviousMapReplayInProgress = bMapReplayInProgress;
		bMapReplayInProgress = true;
		ClearScriptSpawnedScene();
		SceneObjects.reserve(SceneObjects.size() + mapScene->Entities.size());
		ScriptObjects.reserve(ScriptObjects.size() + mapScene->Entities.size());
		size_t expectedMeshEntities = 0;
		size_t expectedPointLights = 0;
		size_t expectedLightEntities = 0;
		size_t expectedScriptEntities = 0;
		for (const CachedMapEntity& entity : mapScene->Entities)
		{
			if (entity.Mesh.bPresent)
				++expectedMeshEntities;
			if (entity.Light.bPresent && entity.Light.Component.Type != CoronaECS::LightType::Directional)
				++expectedPointLights;
			if (entity.Light.bPresent)
				++expectedLightEntities;
			if (!entity.Scripts.empty())
				++expectedScriptEntities;
		}
		PointLights.reserve(PointLights.size() + expectedPointLights);
		EntityWorld.ReserveAdditional(
			mapScene->Entities.size(),
			expectedMeshEntities + expectedPointLights + 2u,
			expectedMeshEntities,
			expectedMeshEntities,
			expectedLightEntities,
			1u,
			expectedScriptEntities);

		bool bIgnoredCameraEntity = false;
		double replayOrderMs = 0.0;
		const auto replayOrderStart = CpuClock::now();
		const std::vector<uint32_t> replayOrder =
			BuildCachedMapReplayOrder(*mapScene, m_camera.m_position);
		replayOrderMs = MapFormatElapsedMilliseconds(replayOrderStart, CpuClock::now());
		const int entityCount = static_cast<int>(replayOrder.size());
		const int progressStep = std::max(1, entityCount / 200);
		if (bStartupLoadingScreenActive)
		{
			EditorMapLoadEntityIndex = 0;
			EditorMapLoadEntityCount = static_cast<uint32_t>(std::max(entityCount, 0));
			UpdateStartupLoadingProgress(
				0.05f,
				(bLoadedFromSceneCache ? L"Replaying cached map entities: " : L"Replaying map entities: ") +
				std::to_wstring(EditorMapLoadEntityCount));
		}

		const auto replayStart = CpuClock::now();
		double meshSceneResolveMs = 0.0;
		double meshEntityApplyMs = 0.0;
		double lightApplyMs = 0.0;
		double scriptApplyMs = 0.0;
		uint32_t meshEntityCount = 0;
		uint32_t lightEntityCount = 0;
		uint32_t scriptEntityCount = 0;
		std::unordered_map<std::string, ScriptSceneHandle> assetSceneHandleCache;
		assetSceneHandleCache.reserve(1024);
		for (int entityIndex = 0; entityIndex < entityCount; ++entityIndex)
		{
			const uint32_t orderedEntityIndex = replayOrder[static_cast<size_t>(entityIndex)];
			if (orderedEntityIndex >= mapScene->Entities.size())
				continue;
			const CachedMapEntity& cachedEntity = mapScene->Entities[orderedEntityIndex];
			const std::string& entityName = cachedEntity.Name;
			if (bStartupLoadingScreenActive &&
				(entityIndex == 0 || entityIndex + 1 == entityCount || (entityIndex % progressStep) == 0))
			{
				EditorMapLoadEntityIndex = static_cast<uint32_t>(entityIndex + 1);
				const float entityProgress =
					0.05f + 0.88f *
					(static_cast<float>(entityIndex) / static_cast<float>(std::max(entityCount, 1)));
				UpdateStartupLoadingProgress(
					entityProgress,
					L"Loading entity " + std::to_wstring(entityIndex + 1) +
					L"/" + std::to_wstring(std::max(entityCount, 0)) +
					(entityName.empty() ? std::wstring() : (L": " + PlatformUtf8ToWide(entityName))));
			}

			CoronaECS::Entity createdEntity;
			if (cachedEntity.Mesh.bPresent)
			{
				const CachedMapMesh& mesh = cachedEntity.Mesh;
				ScriptSceneHandle sceneHandle = InvalidScriptSceneHandle;
				const auto sceneResolveStart = kMapLoadDetailedReplayProfile ? CpuClock::now() : CpuClock::time_point();
				switch (mesh.Primitive)
				{
				case CachedMapMeshPrimitive::Terrain:
					sceneHandle = CreateProceduralTerrainSceneForScript(mesh.Seed);
					break;
				case CachedMapMeshPrimitive::GrassOnTerrain:
					sceneHandle = mesh.bProcedural
						? CreateProceduralGrassOnTerrainSceneInstancedForScript(
							mesh.BladeCount, mesh.BladeHeight, mesh.Seed, mesh.BladeSegments)
						: CreateProceduralGrassOnTerrainSceneForScript(
							mesh.BladeCount, mesh.BladeHeight, mesh.Seed, mesh.BladeSegments);
					break;
				case CachedMapMeshPrimitive::Grass:
					sceneHandle = CreateProceduralGrassSceneForScript(
						mesh.BladeCount, mesh.AreaSize, mesh.BladeHeight, mesh.Seed, mesh.BladeSegments);
					break;
				case CachedMapMeshPrimitive::BlockCharacter:
					sceneHandle = CreateProceduralBlockCharacterSceneForScript(mesh.Seed);
					break;
				case CachedMapMeshPrimitive::Box:
					sceneHandle = CreateProceduralBoxSceneForScript(
						mesh.Color,
						mesh.bBrickTexture,
						mesh.UvRepeat,
						PlatformUtf8ToWide(mesh.TextureKind),
						mesh.UvRepeatY,
						mesh.bFrontOnly);
					break;
				case CachedMapMeshPrimitive::Asset:
					if (!mesh.AssetPath.empty())
					{
						const auto cachedSceneHandle = assetSceneHandleCache.find(mesh.AssetPath);
						if (cachedSceneHandle != assetSceneHandleCache.end())
						{
							sceneHandle = cachedSceneHandle->second;
						}
						else
						{
							sceneHandle = LoadSceneForScript(PlatformUtf8ToWide(mesh.AssetPath));
							if (sceneHandle != InvalidScriptSceneHandle)
								assetSceneHandleCache[mesh.AssetPath] = sceneHandle;
						}
					}
					break;
				case CachedMapMeshPrimitive::Unknown:
				default:
					break;
				}
				if (kMapLoadDetailedReplayProfile)
					meshSceneResolveMs += MapFormatElapsedMilliseconds(sceneResolveStart, CpuClock::now());

				if (sceneHandle != InvalidScriptSceneHandle)
				{
					const auto meshApplyStart = kMapLoadDetailedReplayProfile ? CpuClock::now() : CpuClock::time_point();
					CoronaECS::Entity newEntity = CreateEntity(entityName);
					AddMeshComponentForScript(
						newEntity, sceneHandle,
						mesh.Position, mesh.Rotation, mesh.TargetExtent, mesh.Scale, mesh.bUseScale,
						mesh.Roughness, mesh.Metallic, mesh.bOverrideMaterial,
						mesh.bVisible, mesh.bRayTracing, /*physicsQuery*/ true);
					createdEntity = newEntity;
					if (kMapLoadDetailedReplayProfile)
						meshEntityApplyMs += MapFormatElapsedMilliseconds(meshApplyStart, CpuClock::now());
					++meshEntityCount;
				}
			}

			if (cachedEntity.Light.bPresent)
			{
				const auto lightApplyStart = kMapLoadDetailedReplayProfile ? CpuClock::now() : CpuClock::time_point();
				CoronaECS::LightComponent comp = cachedEntity.Light.Component;
				const bool bDirectional = (comp.Type == CoronaECS::LightType::Directional);
				if (bDirectional)
				{
					const float directionLength = glm::length(comp.Direction);
					comp.Direction = directionLength > 0.0001f ?
						(comp.Direction / directionLength) :
						glm::vec3(0.0f, 1.0f, 0.0f);
					if (comp.Direction.y < -0.0001f)
					{
						comp.Direction = -comp.Direction;
						AppendCpuRuntimeTrace(
							L"[MapLoad][Light] flipped downward directional vector to Corona surface-to-light convention: " +
							PlatformUtf8ToWide(entityName));
					}
				}
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
						if (cachedEntity.Light.bHasPosition)
						{
							auto* trans = EntityWorld.GetTransform(le);
							if (!trans)
								trans = EntityWorld.AddTransform(le, CoronaECS::TransformComponent::FromTRS(cachedEntity.Light.Position));
							else
								trans->SetPosition(cachedEntity.Light.Position);
						}
						SetEntityLightForScript(le, comp, /*persist*/ false);
					}
				}
				if (kMapLoadDetailedReplayProfile)
					lightApplyMs += MapFormatElapsedMilliseconds(lightApplyStart, CpuClock::now());
				++lightEntityCount;
			}

			if (cachedEntity.bHasCamera && !bIgnoredCameraEntity)
			{
				AppendCpuRuntimeTrace(L"[Map] ignored camera entity \"" + PlatformUtf8ToWide(entityName) + L"\"");
				bIgnoredCameraEntity = true;
			}

			if (!createdEntity.IsValid() && !cachedEntity.Scripts.empty())
				createdEntity = CreateEntity(entityName);

			if (createdEntity.IsValid())
			{
				const auto scriptApplyStart = kMapLoadDetailedReplayProfile ? CpuClock::now() : CpuClock::time_point();
				for (const CachedMapScript& script : cachedEntity.Scripts)
				{
					if (script.bNative)
						AttachNativeEntityScriptForScript(createdEntity, script.Value);
					else
						AttachEntityScriptFileForScript(createdEntity, PlatformUtf8ToWide(script.Value));
				}
				if (!cachedEntity.Scripts.empty())
				{
					if (kMapLoadDetailedReplayProfile)
						scriptApplyMs += MapFormatElapsedMilliseconds(scriptApplyStart, CpuClock::now());
					++scriptEntityCount;
				}
			}
		}
		bMapReplayInProgress = bPreviousMapReplayInProgress;
		const double replayMs = MapFormatElapsedMilliseconds(replayStart, CpuClock::now());
		AppendCpuRuntimeTrace(
			L"[MapLoadProfile] replayMs=" + MapFormatFormatMilliseconds(replayMs) +
			L", replayOrderMs=" + MapFormatFormatMilliseconds(replayOrderMs) +
			L", meshSceneResolveMs=" + MapFormatFormatMilliseconds(meshSceneResolveMs) +
			L", meshEntityApplyMs=" + MapFormatFormatMilliseconds(meshEntityApplyMs) +
			L", lightApplyMs=" + MapFormatFormatMilliseconds(lightApplyMs) +
			L", scriptApplyMs=" + MapFormatFormatMilliseconds(scriptApplyMs) +
			L", meshEntities=" + std::to_wstring(meshEntityCount) +
			L", lightEntities=" + std::to_wstring(lightEntityCount) +
			L", scriptEntities=" + std::to_wstring(scriptEntityCount) +
			L", assetSceneCacheEntries=" + std::to_wstring(assetSceneHandleCache.size()) +
			L", detailed=" + std::to_wstring(kMapLoadDetailedReplayProfile ? 1 : 0));

		if (bStartupLoadingScreenActive)
			UpdateStartupLoadingProgress(0.93f, L"Applying map globals");
		const CachedMapGlobals& globals = mapScene->Globals;
		if (globals.bHasWindParams) RenderFrameWindParams = globals.WindParams;
		if (globals.bHasWindTuning) RenderFrameWindTuning = globals.WindTuning;
		if (globals.bHasGrassBendOrigin) RenderFrameGrassBendOrigin = globals.GrassBendOrigin;
		if (globals.bHasGrassBendParams) RenderFrameGrassBendParams = globals.GrassBendParams;
		if (globals.bHasGrassRenderOrigin) GrassRenderOrigin = globals.GrassRenderOrigin;
		if (globals.bHasGrassRenderDistance) GrassRenderDistance = globals.GrassRenderDistance;
		if (globals.bHasTerrainDeformSphere) RenderFrameTerrainDeformSphere = globals.TerrainDeformSphere;

		if (cacheWriteFuture.valid())
		{
			const bool bWroteCache = cacheWriteFuture.get();
			AppendCpuRuntimeTrace(
				std::wstring(L"[MapCache] scene cache write ") +
				(bWroteCache ? L"ok: " : L"failed: ") +
				cachePath.wstring());
		}

		UpdateMainCameraEntityFromSimpleCamera();
		AppendCpuRuntimeTrace(
			L"[Map] loaded " + path.wstring() +
			L", sceneCache=" + std::wstring(bLoadedFromSceneCache ? L"hit" : L"miss") +
			L", entities=" + std::to_wstring(mapScene->Entities.size()) +
			L", chunks=" + std::to_wstring(mapScene->Chunks.size()));
		CurrentMapName = name;
		PersistLastEditorMapName(name);
		MarkAllSceneObjectsForRenderSync();
		MarkAllPointLightsForRenderSync();
		MarkCpuPhysicsSceneDirty();
		MarkRayTracingSceneDirty();
		bRayTracingBLASCacheResetPending = true;
		ResetAllAccumulationState(false);
		AppendCpuRuntimeTrace(L"[Map] requested full render sync after load " + path.wstring());
		return true;
	}

#if 0
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
	bool bIgnoredCameraEntity = false;

	// --- Entities ---
	lua_getfield(L, rootIdx, "entities");
	if (lua_istable(L, -1))
	{
		const int entitiesIdx = lua_gettop(L);
		const int n = static_cast<int>(lua_objlen(L, entitiesIdx));
		if (bStartupLoadingScreenActive)
		{
			EditorMapLoadEntityIndex = 0;
			EditorMapLoadEntityCount = static_cast<uint32_t>(std::max(n, 0));
			UpdateStartupLoadingProgress(
				0.05f,
				L"Reading map entities: " + std::to_wstring(EditorMapLoadEntityCount));
		}
		for (int i = 1; i <= n; ++i)
		{
			lua_rawgeti(L, entitiesIdx, i);
			if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }
			const int eIdx = lua_gettop(L);

			std::string entityName;
			LuaGetString(L, eIdx, "name", entityName);
			if (bStartupLoadingScreenActive)
			{
				EditorMapLoadEntityIndex = static_cast<uint32_t>(i);
				const float entityProgress =
					0.05f + 0.88f *
					(static_cast<float>(i - 1) / static_cast<float>(std::max(n, 1)));
				UpdateStartupLoadingProgress(
					entityProgress,
					L"Loading entity " + std::to_wstring(i) +
					L"/" + std::to_wstring(std::max(n, 0)) +
					(entityName.empty() ? std::wstring() : (L": " + PlatformUtf8ToWide(entityName))));
			}

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
				else if (primitive == "BOX" || primitive == "CUBE" || primitive == "PLANE" || primitive == "QUAD")
				{
					glm::vec3 color(0.72f, 0.72f, 0.72f);
					bool brickTexture = false;
					bool frontOnly = (primitive == "PLANE" || primitive == "QUAD");
					double uvRepeat = 1.0;
					double uvRepeatY = -1.0;
					std::string textureKind;
					if (!LuaGetVec3(L, mIdx, "color", color))
						LuaGetVec3(L, mIdx, "base_color", color);
					LuaGetBool(L, mIdx, "brick_texture", brickTexture);
					LuaGetBool(L, mIdx, "front_only", frontOnly);
					LuaGetNumber(L, mIdx, "uv_repeat", uvRepeat);
					LuaGetNumber(L, mIdx, "uv_repeat_y", uvRepeatY);
					LuaGetString(L, mIdx, "texture_kind", textureKind);
					sceneHandle = CreateProceduralBoxSceneForScript(
						color,
						brickTexture,
						static_cast<float>(uvRepeat),
						PlatformUtf8ToWide(textureKind),
						static_cast<float>(uvRepeatY),
						frontOnly);
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
				{
					std::transform(typeStr.begin(), typeStr.end(), typeStr.begin(),
						[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
					comp.Type =
						typeStr == "directional" || typeStr == "sun" ? CoronaECS::LightType::Directional :
						typeStr == "spot" || typeStr == "spotlight" ? CoronaECS::LightType::Spot :
						CoronaECS::LightType::Point;
				}
				LuaGetVec3(L, lIdx, "direction", comp.Direction);
				LuaGetVec3(L, lIdx, "color", comp.Color);
				double intensity = 1.0, radius = 320.0;
				if (LuaGetNumber(L, lIdx, "intensity", intensity)) comp.Intensity = static_cast<float>(intensity);
				if (LuaGetNumber(L, lIdx, "radius", radius))       comp.Radius = static_cast<float>(radius);
				double innerConeAngle = comp.InnerConeAngle;
				double outerConeAngle = comp.OuterConeAngle;
				if (LuaGetNumber(L, lIdx, "inner_cone_angle", innerConeAngle) ||
					LuaGetNumber(L, lIdx, "innerConeAngle", innerConeAngle))
					comp.InnerConeAngle = std::clamp(static_cast<float>(innerConeAngle), 0.0f, glm::pi<float>() - 0.001f);
				if (LuaGetNumber(L, lIdx, "outer_cone_angle", outerConeAngle) ||
					LuaGetNumber(L, lIdx, "outerConeAngle", outerConeAngle))
					comp.OuterConeAngle = std::clamp(static_cast<float>(outerConeAngle), 0.001f, glm::pi<float>());
				comp.InnerConeAngle = std::clamp(comp.InnerConeAngle, 0.0f, glm::pi<float>() - 0.001f);
				comp.OuterConeAngle = std::clamp(comp.OuterConeAngle, comp.InnerConeAngle + 0.001f, glm::pi<float>());
				if (!LuaGetBool(L, lIdx, "cast_shadow", comp.bCastShadow))
					LuaGetBool(L, lIdx, "castShadow", comp.bCastShadow);
				LuaGetBool(L, lIdx, "enabled", comp.bEnabled);
				// Point light position — restored before AddLight so the
				// TransformComponent created downstream has the saved value
				// instead of the entity-default origin.
				glm::vec3 pointLightPosition(0.0f);
				const bool bHasPointLightPosition =
					(comp.Type != CoronaECS::LightType::Directional) &&
					LuaGetVec3(L, lIdx, "position", pointLightPosition);

				// Directional lights are an engine singleton — the very first
				// one we see fills `MainDirectionalLightEntity`, and any extra
				// directional entries (legacy duplicates from a map saved
				// before this fix) are skipped instead of spawning new
				// entities. Subsequent loads then heal the .map file on the
				// next save by only writing the single survivor.
				const bool bDirectional = (comp.Type == CoronaECS::LightType::Directional);
				if (bDirectional)
				{
					const float directionLength = glm::length(comp.Direction);
					comp.Direction = directionLength > 0.0001f ?
						(comp.Direction / directionLength) :
						glm::vec3(0.0f, 1.0f, 0.0f);
					if (comp.Direction.y < -0.0001f)
					{
						comp.Direction = -comp.Direction;
						AppendCpuRuntimeTrace(
							L"[MapLoad][Light] flipped downward directional vector to Corona surface-to-light convention: " +
							PlatformUtf8ToWide(entityName));
					}
				}
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

			// Camera entries in .map files are intentionally ignored. The
			// editor/debug camera is persisted separately in camera_state.cfg
			// so loading a map does not clobber the user's current vantage.
			lua_getfield(L, eIdx, "camera");
			if (lua_istable(L, -1))
			{
				if (!bIgnoredCameraEntity)
				{
					AppendCpuRuntimeTrace(L"[Map] ignored camera entity \"" + PlatformUtf8ToWide(entityName) + L"\"");
					bIgnoredCameraEntity = true;
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
	if (bStartupLoadingScreenActive)
		UpdateStartupLoadingProgress(0.93f, L"Applying map globals");
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
	UpdateMainCameraEntityFromSimpleCamera();
	AppendCpuRuntimeTrace(L"[Map] loaded " + path.wstring());
	CurrentMapName = name;
	PersistLastEditorMapName(name);
	MarkAllSceneObjectsForRenderSync();
	MarkAllPointLightsForRenderSync();
	MarkCpuPhysicsSceneDirty();
	MarkRayTracingSceneDirty();
	bRayTracingBLASCacheResetPending = true;
	ResetAllAccumulationState(false);
	AppendCpuRuntimeTrace(L"[Map] requested full render sync after load " + path.wstring());
	return true;
#endif
}
