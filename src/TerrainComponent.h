#pragma once

// High-level terrain "component" — wraps the generator + .crtn cache +
// chunk mesh upload into a single Scene that the existing renderer/ECS
// stack consumes through MeshComponent::ScenePtr.
//
// Phase 1: single LOD, no geomorphing, gray material, all-chunks draw.
// Frustum culling (Step 1.5) edits Mesh::Draws at runtime to limit
// per-frame draw calls to visible chunks.

#include <filesystem>
#include <memory>
#include <vector>

#include "TerrainFormat.h"
#include "TerrainGenerator.h"
#include "TerrainMeshBuilder.h"

class IRenderBackend;
class Mesh;
class Scene;
class Material;

namespace Terrain
{
	class Component
	{
	public:
		Component() = default;
		~Component() = default;

		// Generate (or load cached) heightmap, build chunk mesh, upload to GPU,
		// produce a Scene. `cacheName` is a stable identifier used to derive
		// the .crtn cache path. Pass empty to skip caching.
		bool Initialize(
			IRenderBackend* backend,
			const GenerateParams& params,
			const std::wstring& cacheName);

		const std::shared_ptr<Scene>& GetScene() const { return ScenePtr; }
		const std::vector<ChunkMeshInfo>& GetChunks() const { return ChunkInfos; }
		const std::shared_ptr<Mesh>& GetMesh() const { return MeshPtr; }
		const GenerateStats& GetStats() const { return Stats; }
		const TerrainData& GetData() const { return Data; }

	private:
		std::filesystem::path ResolveCachePath(const std::wstring& cacheName) const;
		bool LoadOrGenerate(const GenerateParams& params, const std::filesystem::path& cachePath);

		GenerateParams Params{};
		GenerateStats Stats{};
		TerrainData Data{};
		std::vector<ChunkMeshInfo> ChunkInfos;
		std::shared_ptr<Mesh> MeshPtr;
		std::shared_ptr<Material> MaterialPtr;
		std::shared_ptr<Scene> ScenePtr;
	};
}
