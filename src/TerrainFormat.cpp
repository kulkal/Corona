#include "TerrainFormat.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <limits>

namespace Terrain
{

TerrainData PackHeights(
	const std::vector<float>& heights,
	uint32_t width,
	uint32_t depth,
	uint32_t chunkSize,
	float worldScaleXZ,
	float heightMin,
	float heightMax,
	uint32_t seed)
{
	TerrainData data{};
	data.Header.Magic = kFormatMagic;
	data.Header.Version = kFormatVersion;
	data.Header.Width = width;
	data.Header.Depth = depth;
	data.Header.ChunkSize = chunkSize;
	data.Header.WorldScaleXZ = worldScaleXZ;
	data.Header.HeightMin = heightMin;
	data.Header.HeightMax = heightMax;
	data.Header.Seed = seed;

	// One chunk covers (chunkSize-1) quads; adjacent chunks share an edge row,
	// so the world wraps with NumChunks = ceil((width-1) / (chunkSize-1)) inputs.
	const uint32_t quads = chunkSize - 1;
	data.Header.NumChunksX = (width  - 1 + quads - 1) / quads;
	data.Header.NumChunksZ = (depth  - 1 + quads - 1) / quads;

	const float range = heightMax - heightMin;
	const float invRange = range > 0.0f ? 1.0f / range : 0.0f;
	const uint32_t numChunks = data.Header.NumChunksX * data.Header.NumChunksZ;
	data.Chunks.resize(numChunks);

	for (uint32_t cz = 0; cz < data.Header.NumChunksZ; ++cz)
	{
		for (uint32_t cx = 0; cx < data.Header.NumChunksX; ++cx)
		{
			ChunkData& chunk = data.Chunks[cz * data.Header.NumChunksX + cx];
			chunk.Heights.resize(static_cast<size_t>(chunkSize) * chunkSize);

			const uint32_t baseX = cx * quads;
			const uint32_t baseZ = cz * quads;

			float minH =  std::numeric_limits<float>::infinity();
			float maxH = -std::numeric_limits<float>::infinity();

			for (uint32_t lz = 0; lz < chunkSize; ++lz)
			{
				for (uint32_t lx = 0; lx < chunkSize; ++lx)
				{
					const uint32_t gx = std::min(baseX + lx, width  - 1);
					const uint32_t gz = std::min(baseZ + lz, depth - 1);
					const float h = heights[static_cast<size_t>(gz) * width + gx];
					const float h01 = std::clamp((h - heightMin) * invRange, 0.0f, 1.0f);
					chunk.Heights[static_cast<size_t>(lz) * chunkSize + lx] =
						static_cast<uint16_t>(h01 * 65535.0f + 0.5f);
					minH = std::min(minH, h);
					maxH = std::max(maxH, h);
				}
			}

			// World-space AABB: chunk spans baseX..baseX+(chunkSize-1) samples
			// in X (each sample is worldScaleXZ wide), similarly in Z.
			chunk.AabbMin[0] = static_cast<float>(baseX) * worldScaleXZ;
			chunk.AabbMin[1] = minH;
			chunk.AabbMin[2] = static_cast<float>(baseZ) * worldScaleXZ;
			chunk.AabbMax[0] = static_cast<float>(baseX + chunkSize - 1) * worldScaleXZ;
			chunk.AabbMax[1] = maxH;
			chunk.AabbMax[2] = static_cast<float>(baseZ + chunkSize - 1) * worldScaleXZ;
		}
	}

	return data;
}

bool SaveToFile(const std::filesystem::path& path, const TerrainData& data)
{
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);

	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out) return false;

	const uint32_t numChunks = data.Header.NumChunksX * data.Header.NumChunksZ;
	const uint32_t chunkBytes = data.Header.ChunkSize * data.Header.ChunkSize * 2u;
	const uint64_t headerBytes = sizeof(FileHeader);
	const uint64_t indexBytes = static_cast<uint64_t>(numChunks) * sizeof(ChunkIndexEntry);

	// Build index entries with absolute offsets.
	std::vector<ChunkIndexEntry> index(numChunks);
	for (uint32_t i = 0; i < numChunks; ++i)
	{
		const ChunkData& cd = data.Chunks[i];
		ChunkIndexEntry& e = index[i];
		e.OffsetInFile = headerBytes + indexBytes + static_cast<uint64_t>(i) * chunkBytes;
		e.SizeInBytes = chunkBytes;
		std::copy(std::begin(cd.AabbMin), std::end(cd.AabbMin), e.AabbMin);
		std::copy(std::begin(cd.AabbMax), std::end(cd.AabbMax), e.AabbMax);
		e.Reserved = 0;
	}

	out.write(reinterpret_cast<const char*>(&data.Header), sizeof(FileHeader));
	out.write(reinterpret_cast<const char*>(index.data()), static_cast<std::streamsize>(indexBytes));
	for (uint32_t i = 0; i < numChunks; ++i)
	{
		out.write(reinterpret_cast<const char*>(data.Chunks[i].Heights.data()),
			static_cast<std::streamsize>(chunkBytes));
	}
	return static_cast<bool>(out);
}

bool LoadFromFile(const std::filesystem::path& path, TerrainData& outData, std::string* outError)
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
	{
		if (outError) *outError = "Failed to open file: " + path.string();
		return false;
	}

	in.read(reinterpret_cast<char*>(&outData.Header), sizeof(FileHeader));
	if (!in)
	{
		if (outError) *outError = "Failed to read header";
		return false;
	}
	if (outData.Header.Magic != kFormatMagic)
	{
		if (outError) *outError = "Bad magic (not a .crtn file)";
		return false;
	}
	if (outData.Header.Version != kFormatVersion)
	{
		if (outError) *outError = "Unsupported version";
		return false;
	}

	const uint32_t numChunks = outData.Header.NumChunksX * outData.Header.NumChunksZ;
	const uint32_t chunkBytes = outData.Header.ChunkSize * outData.Header.ChunkSize * 2u;
	std::vector<ChunkIndexEntry> index(numChunks);
	in.read(reinterpret_cast<char*>(index.data()), static_cast<std::streamsize>(numChunks * sizeof(ChunkIndexEntry)));

	outData.Chunks.resize(numChunks);
	for (uint32_t i = 0; i < numChunks; ++i)
	{
		ChunkData& cd = outData.Chunks[i];
		cd.Heights.resize(static_cast<size_t>(outData.Header.ChunkSize) * outData.Header.ChunkSize);
		in.seekg(static_cast<std::streamoff>(index[i].OffsetInFile));
		in.read(reinterpret_cast<char*>(cd.Heights.data()), static_cast<std::streamsize>(chunkBytes));
		std::copy(std::begin(index[i].AabbMin), std::end(index[i].AabbMin), cd.AabbMin);
		std::copy(std::begin(index[i].AabbMax), std::end(index[i].AabbMax), cd.AabbMax);
		if (!in)
		{
			if (outError) *outError = "Failed reading chunk " + std::to_string(i);
			return false;
		}
	}
	return true;
}

}
