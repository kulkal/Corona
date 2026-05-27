#pragma once

// Builds CPU-side vertex/index arrays for the chunked terrain mesh.
// Produces a single concatenated VB + IB ready for upload, plus per-chunk
// DrawCall metadata (IndexStart/Count, VertexBase, AABB) consumed by
// TerrainComponent for frustum culling and per-chunk draw issuance.

#include <cstdint>
#include <vector>

#include "TerrainFormat.h"

namespace Terrain
{
	// Matches existing 44B GBuffer Vertex layout. Keep in sync with the
	// PSO desc.VertexElements in InitGBufferPass (POSITION float3 @0,
	// NORMAL float3 @12, TEXCOORD float2 @24, TANGENT float3 @32).
	#pragma pack(push, 4)
	struct MeshVertex
	{
		float Position[3];
		float Normal[3];
		float UV[2];
		float Tangent[3];
	};
	#pragma pack(pop)
	static_assert(sizeof(MeshVertex) == 44, "TerrainMeshBuilder Vertex must be 44B (GBuffer compatible)");

	struct ChunkMeshInfo
	{
		uint32_t IndexStart = 0;     // into global IB
		uint32_t IndexCount = 0;
		uint32_t VertexBase = 0;     // into global VB
		uint32_t VertexCount = 0;
		float AabbMin[3] = {0, 0, 0};
		float AabbMax[3] = {0, 0, 0};
	};

	struct BuiltMesh
	{
		std::vector<MeshVertex> Vertices;   // concatenated, all chunks
		std::vector<uint32_t> Indices;      // concatenated, indices LOCAL per chunk ([0, ChunkSize^2))
		std::vector<ChunkMeshInfo> Chunks;
	};

	// Build the full chunked terrain mesh from packed TerrainData.
	// `uvTileSize` is the world-space distance over which UV repeats (1 / texture tile).
	BuiltMesh BuildMesh(const TerrainData& data, float uvTileSize);
}
