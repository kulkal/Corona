#include "TerrainMeshBuilder.h"

#include <algorithm>
#include <cmath>

namespace Terrain
{
namespace
{
	inline float Sample(const ChunkData& chunk, uint32_t chunkSize, int x, int z, const TerrainData& data)
	{
		// Clamp to chunk bounds. Edges are shared with neighbors in the source
		// heightmap so this gives matching positions across the seam.
		x = std::clamp(x, 0, static_cast<int>(chunkSize) - 1);
		z = std::clamp(z, 0, static_cast<int>(chunkSize) - 1);
		return data.DecodeHeight(chunk.Heights[static_cast<size_t>(z) * chunkSize + x]);
	}

	inline void Normalize3(float v[3])
	{
		const float len = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
		if (len > 1e-8f)
		{
			v[0] /= len;
			v[1] /= len;
			v[2] /= len;
		}
		else
		{
			v[0] = 0; v[1] = 1; v[2] = 0;
		}
	}
}

BuiltMesh BuildMesh(const TerrainData& data, float uvTileSize)
{
	BuiltMesh out{};
	const uint32_t cs = data.Header.ChunkSize;
	const uint32_t quads = cs - 1;
	const uint32_t vertsPerChunk = cs * cs;
	const uint32_t trisPerChunk = quads * quads * 2u;
	const uint32_t idxPerChunk = trisPerChunk * 3u;
	const uint32_t numChunks = data.Header.NumChunksX * data.Header.NumChunksZ;

	out.Vertices.reserve(static_cast<size_t>(numChunks) * vertsPerChunk);
	out.Indices.reserve(static_cast<size_t>(numChunks) * idxPerChunk);
	out.Chunks.resize(numChunks);

	const float worldScale = data.Header.WorldScaleXZ;
	const float invUvTile = uvTileSize > 0.0f ? (1.0f / uvTileSize) : 1.0f;

	// Pre-center the mesh around origin so BuildCenteredSceneTransform's
	// auto-centering becomes a no-op and CPU frustum-cull AABBs (in mesh
	// space) match the rendered world positions exactly.
	const float centerX = static_cast<float>(data.Header.Width  - 1) * worldScale * 0.5f;
	const float centerZ = static_cast<float>(data.Header.Depth  - 1) * worldScale * 0.5f;

	for (uint32_t cz = 0; cz < data.Header.NumChunksZ; ++cz)
	{
		for (uint32_t cx = 0; cx < data.Header.NumChunksX; ++cx)
		{
			const uint32_t chunkIdx = cz * data.Header.NumChunksX + cx;
			const ChunkData& chunk = data.Chunks[chunkIdx];

			ChunkMeshInfo& info = out.Chunks[chunkIdx];
			info.VertexBase = chunkIdx * vertsPerChunk;
			info.VertexCount = vertsPerChunk;
			info.IndexStart = chunkIdx * idxPerChunk;
			info.IndexCount = idxPerChunk;
			// Apply the same XZ centering to the per-chunk AABB so the
			// CPU culling test against world-space view*proj sees the
			// same coordinates the GPU rasterizes.
			info.AabbMin[0] = chunk.AabbMin[0] - centerX;
			info.AabbMin[1] = chunk.AabbMin[1];
			info.AabbMin[2] = chunk.AabbMin[2] - centerZ;
			info.AabbMax[0] = chunk.AabbMax[0] - centerX;
			info.AabbMax[1] = chunk.AabbMax[1];
			info.AabbMax[2] = chunk.AabbMax[2] - centerZ;

			const uint32_t baseSampleX = cx * quads;
			const uint32_t baseSampleZ = cz * quads;

			for (uint32_t lz = 0; lz < cs; ++lz)
			{
				for (uint32_t lx = 0; lx < cs; ++lx)
				{
					MeshVertex v{};
					const float wx = static_cast<float>(baseSampleX + lx) * worldScale - centerX;
					const float wz = static_cast<float>(baseSampleZ + lz) * worldScale - centerZ;
					const float wy = Sample(chunk, cs, static_cast<int>(lx), static_cast<int>(lz), data);
					v.Position[0] = wx;
					v.Position[1] = wy;
					v.Position[2] = wz;

					// Finite-difference normal (central). 2 * worldScale denominator
					// folds out into the cross product, but we normalize anyway.
					const float hL = Sample(chunk, cs, static_cast<int>(lx) - 1, static_cast<int>(lz),     data);
					const float hR = Sample(chunk, cs, static_cast<int>(lx) + 1, static_cast<int>(lz),     data);
					const float hD = Sample(chunk, cs, static_cast<int>(lx),     static_cast<int>(lz) - 1, data);
					const float hU = Sample(chunk, cs, static_cast<int>(lx),     static_cast<int>(lz) + 1, data);
					float n[3] = { (hL - hR), 2.0f * worldScale, (hD - hU) };
					Normalize3(n);
					v.Normal[0] = n[0]; v.Normal[1] = n[1]; v.Normal[2] = n[2];

					v.UV[0] = wx * invUvTile;
					v.UV[1] = wz * invUvTile;

					// Tangent: world +X projected onto the surface plane (cross of
					// up and normal-bitangent). Cheap approximation good enough
					// for Phase 1 (single gray material; no normal mapping).
					float t[3] = { 1.0f, 0.0f, 0.0f };
					// Remove component along normal: t = t - (t·n) n
					const float tdotn = t[0]*n[0] + t[1]*n[1] + t[2]*n[2];
					t[0] -= tdotn * n[0]; t[1] -= tdotn * n[1]; t[2] -= tdotn * n[2];
					Normalize3(t);
					v.Tangent[0] = t[0]; v.Tangent[1] = t[1]; v.Tangent[2] = t[2];

					out.Vertices.push_back(v);
				}
			}

			// Indices (LOCAL — VertexBase is applied at draw time).
			for (uint32_t lz = 0; lz < quads; ++lz)
			{
				for (uint32_t lx = 0; lx < quads; ++lx)
				{
					const uint32_t i00 = lz * cs + lx;
					const uint32_t i10 = i00 + 1;
					const uint32_t i01 = i00 + cs;
					const uint32_t i11 = i01 + 1;
					// Triangle winding: CCW assuming +Y up, +X right, +Z fwd
					// (matches existing grass/mesh paths).
					out.Indices.push_back(i00);
					out.Indices.push_back(i01);
					out.Indices.push_back(i10);
					out.Indices.push_back(i10);
					out.Indices.push_back(i01);
					out.Indices.push_back(i11);
				}
			}
		}
	}

	return out;
}

}
