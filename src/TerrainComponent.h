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

#include "glm/mat4x4.hpp"
#include "glm/vec3.hpp"

#include "TerrainFormat.h"
#include "TerrainGenerator.h"
#include "TerrainMeshBuilder.h"

class IRenderBackend;
class Mesh;
class Scene;
class Material;
class Buffer;

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

		// Replace the owning Mesh's DrawCalls with one per chunk whose AABB
		// passes the conservative 8-corner frustum test against the supplied
		// world-space view*proj matrix. Returns the visible chunk count.
		// Assumes GLM_FORCE_DEPTH_ZERO_TO_ONE (D3D-style depth) — Corona's
		// default for both DX12 and Vulkan paths.
		uint32_t UpdateCulling(const glm::mat4& viewProj);

		// Bilinear height lookup in centered world space. Mesh vertices are
		// pre-centered around origin (see TerrainMeshBuilder); pass post-
		// centering world (X, Z). Out-of-bounds clamps to the edge.
		float SampleHeight(float worldX, float worldZ) const;

		const std::shared_ptr<Scene>& GetScene() const { return ScenePtr; }
		const std::vector<ChunkMeshInfo>& GetChunks() const { return ChunkInfos; }
		const std::shared_ptr<Mesh>& GetMesh() const { return MeshPtr; }
		// Linear (width × depth) decoded heightmap as a structured-buffer
		// SRV. Used by the procedural grass shader to anchor each blade to
		// the terrain surface. Width/depth/scale match Data.Header.
		const std::shared_ptr<Buffer>& GetHeightBuffer() const { return HeightBuffer; }
		const GenerateStats& GetStats() const { return Stats; }
		const TerrainData& GetData() const { return Data; }
		uint32_t GetLastVisibleChunkCount() const { return LastVisibleChunkCount; }
		uint32_t GetTotalChunkCount() const { return static_cast<uint32_t>(ChunkInfos.size()); }

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
		std::shared_ptr<Buffer> HeightBuffer;
		uint32_t LastVisibleChunkCount = 0;
	};
}
