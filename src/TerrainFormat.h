#pragma once

// Corona Terrain native binary format ("CRTN").
//
// Layout:
//   [Header]          fixed 64 B
//   [ChunkIndex]      NumChunksX * NumChunksZ entries × 40 B
//   [ChunkData N]     each: ChunkSampleCount × uint16
//
// Heights are quantized to uint16: world Y = HeightMin + (h/65535) * (HeightMax-HeightMin).
// Chunk index table carries pre-computed world-space AABB for CPU frustum culling.
// Endianness: little-endian (desktop/Windows only).

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Terrain
{
	static constexpr uint32_t kFormatMagic   = 0x4E545243u; // 'CRTN' little-endian
	static constexpr uint32_t kFormatVersion = 1u;

	#pragma pack(push, 4)
	struct FileHeader
	{
		uint32_t Magic;            // = kFormatMagic
		uint32_t Version;          // = kFormatVersion
		uint32_t Width;            // total samples X
		uint32_t Depth;            // total samples Z
		uint32_t ChunkSize;        // verts per chunk side (e.g. 65)
		uint32_t NumChunksX;
		uint32_t NumChunksZ;
		float    WorldScaleXZ;
		float    HeightMin;
		float    HeightMax;
		uint32_t Seed;
		uint32_t Reserved[5];      // pad to 64 B
	};
	static_assert(sizeof(FileHeader) == 64, "FileHeader size mismatch");

	struct ChunkIndexEntry
	{
		uint64_t OffsetInFile;     // byte offset to chunk's uint16 heights
		uint32_t SizeInBytes;      // ChunkSampleCount * 2
		float    AabbMin[3];       // world-space
		float    AabbMax[3];
		uint32_t Reserved;         // pad to 40 B
	};
	static_assert(sizeof(ChunkIndexEntry) == 40, "ChunkIndexEntry size mismatch");
	#pragma pack(pop)

	// Per-chunk in-memory representation after load.
	struct ChunkData
	{
		std::vector<uint16_t> Heights;  // ChunkSize * ChunkSize quantized heights
		float AabbMin[3] = {0, 0, 0};
		float AabbMax[3] = {0, 0, 0};
	};

	struct TerrainData
	{
		FileHeader Header{};
		std::vector<ChunkData> Chunks;  // NumChunksX * NumChunksZ, row-major (z-major)

		// Convenience: decode a sample back to world height.
		float DecodeHeight(uint16_t q) const
		{
			return Header.HeightMin + (static_cast<float>(q) / 65535.0f) * (Header.HeightMax - Header.HeightMin);
		}
	};

	// Pack a float heightmap (W*D values) into chunked uint16 layout + per-chunk
	// AABB. `chunkSize` is verts per side (typical 65). `heights` is row-major
	// (z-major). Returns chunked data ready for SaveToFile.
	TerrainData PackHeights(
		const std::vector<float>& heights,
		uint32_t width,
		uint32_t depth,
		uint32_t chunkSize,
		float worldScaleXZ,
		float heightMin,
		float heightMax,
		uint32_t seed);

	// Disk I/O. Returns true on success.
	bool SaveToFile(const std::filesystem::path& path, const TerrainData& data);
	bool LoadFromFile(const std::filesystem::path& path, TerrainData& outData, std::string* outError = nullptr);
}
