#include "TerrainComponent.h"

#include <chrono>
#include <sstream>
#include <string>

#include "glm/glm.hpp"

#include "RenderBackend.h"
#include "RenderResources.h"
#include "Utils.h"

void AppendCpuRuntimeTrace(const std::wstring& line);

namespace Terrain
{
namespace
{
	std::wstring HeightRangeKey(float lo, float hi)
	{
		std::wostringstream w;
		w << static_cast<int>(lo) << L"_" << static_cast<int>(hi);
		return w.str();
	}
}

std::filesystem::path Component::ResolveCachePath(const std::wstring& cacheName) const
{
	std::filesystem::path dir = RuntimePaths::RootDirectory() / L"bin" / L"terrain_cache";
	std::wstring fileName =
		cacheName + L"_" +
		std::to_wstring(Params.Seed) + L"_" +
		std::to_wstring(Params.Width) + L"x" + std::to_wstring(Params.Depth) +
		L"_" + HeightRangeKey(Params.HeightMin, Params.HeightMax) +
		L".crtn";
	return dir / fileName;
}

bool Component::LoadOrGenerate(const GenerateParams& params, const std::filesystem::path& cachePath)
{
	Params = params;

	if (!cachePath.empty() && std::filesystem::exists(cachePath))
	{
		std::string err;
		if (LoadFromFile(cachePath, Data, &err))
		{
			Stats.MinHeight = params.HeightMin;
			Stats.MaxHeight = params.HeightMax;
			Stats.MeanHeight = 0.0f; // unused for cached path
			AppendCpuRuntimeTrace(L"[Terrain] loaded cache " + cachePath.wstring());
			return true;
		}
		AppendCpuRuntimeTrace(L"[Terrain] cache load failed (" + std::wstring(err.begin(), err.end()) + L"), regenerating");
	}

	using clock = std::chrono::steady_clock;
	const auto t0 = clock::now();
	std::vector<float> heights;
	Stats = GenerateHeights(params, heights);
	const auto t1 = clock::now();
	const auto genMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

	const uint32_t chunkVerts = 65u; // ChunkSize quads = 64 → 65 verts per side; matches 44B Vertex grid
	Data = PackHeights(heights, params.Width, params.Depth, chunkVerts, params.WorldScaleXZ,
		params.HeightMin, params.HeightMax, params.Seed);

	const auto t2 = clock::now();
	const auto packMs = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();

	if (!cachePath.empty())
	{
		if (!SaveToFile(cachePath, Data))
			AppendCpuRuntimeTrace(L"[Terrain] cache save failed: " + cachePath.wstring());
		else
			AppendCpuRuntimeTrace(L"[Terrain] cache saved " + cachePath.wstring());
	}

	std::wostringstream w;
	w << L"[Terrain] generated " << params.Width << L"x" << params.Depth
	  << L" heights gen=" << genMs << L"ms pack=" << packMs << L"ms"
	  << L" minH=" << Stats.MinHeight << L" maxH=" << Stats.MaxHeight << L" meanH=" << Stats.MeanHeight;
	AppendCpuRuntimeTrace(w.str());
	return true;
}

uint32_t Component::UpdateCulling(const glm::mat4& viewProj)
{
	if (!MeshPtr || ChunkInfos.empty())
		return 0;

	MeshPtr->Draws.clear();
	MeshPtr->Draws.reserve(ChunkInfos.size());

	uint32_t visible = 0;
	for (const ChunkMeshInfo& ci : ChunkInfos)
	{
		// 8-corner conservative AABB-vs-frustum test in clip space.
		// If all 8 corners share the same outside half-space for any clip
		// plane, the chunk is culled. D3D depth convention: z ∈ [0, w].
		int outL = 0, outR = 0, outB = 0, outT = 0, outN = 0, outF = 0;
		for (int i = 0; i < 8; ++i)
		{
			const float x = (i & 1) ? ci.AabbMax[0] : ci.AabbMin[0];
			const float y = (i & 2) ? ci.AabbMax[1] : ci.AabbMin[1];
			const float z = (i & 4) ? ci.AabbMax[2] : ci.AabbMin[2];
			const glm::vec4 c = viewProj * glm::vec4(x, y, z, 1.0f);
			if (c.x < -c.w) ++outL;
			if (c.x >  c.w) ++outR;
			if (c.y < -c.w) ++outB;
			if (c.y >  c.w) ++outT;
			if (c.z <  0.0f) ++outN;
			if (c.z >  c.w) ++outF;
		}
		if (outL == 8 || outR == 8 || outB == 8 || outT == 8 || outN == 8 || outF == 8)
			continue;

		Mesh::DrawCall dc{};
		dc.mat = MaterialPtr;
		dc.IndexStart = ci.IndexStart;
		dc.IndexCount = ci.IndexCount;
		dc.VertexBase = ci.VertexBase;
		dc.VertexCount = ci.VertexCount;
		MeshPtr->Draws.push_back(dc);
		++visible;
	}
	return visible;
}

bool Component::Initialize(
	IRenderBackend* backend,
	const GenerateParams& params,
	const std::wstring& cacheName)
{
	if (!backend)
		return false;

	std::filesystem::path cachePath = cacheName.empty() ? std::filesystem::path{} : ResolveCachePath(cacheName);
	if (!LoadOrGenerate(params, cachePath))
		return false;

	// Build the chunked mesh on the CPU. UV tiles over 16m so a tile is
	// visible at the gray Phase-1 default; Phase 2 textures rebind this.
	BuiltMesh built = BuildMesh(Data, /*uvTileSize=*/16.0f);
	ChunkInfos = std::move(built.Chunks);

	using clock = std::chrono::steady_clock;
	const auto tUpload0 = clock::now();

	// Upload single concatenated VB + IB.
	const uint32_t vbBytes = static_cast<uint32_t>(built.Vertices.size() * sizeof(MeshVertex));
	const uint32_t ibBytes = static_cast<uint32_t>(built.Indices.size() * sizeof(uint32_t));

	auto vb = backend->CreateVertexBuffer(vbBytes, sizeof(MeshVertex), built.Vertices.data());
	auto ib = backend->CreateIndexBuffer(EIndexFormat::U32, ibBytes, built.Indices.data());
	if (!vb || !ib)
	{
		AppendCpuRuntimeTrace(L"[Terrain] VB/IB upload failed");
		return false;
	}

	// Gray Phase-1 material. Uses engine defaults so no asset loading needed.
	MaterialPtr = std::make_shared<Material>();
	MaterialPtr->BaseColorFactor = glm::vec4(0.55f, 0.55f, 0.55f, 1.0f);

	auto mesh = std::make_shared<Mesh>(backend);
	mesh->transform = glm::mat4x4(1.0f);
	mesh->NumVertices = static_cast<uint32_t>(built.Vertices.size());
	mesh->NumIndices = static_cast<uint32_t>(built.Indices.size());
	mesh->VertexStride = sizeof(MeshVertex);
	mesh->IndexFormat = EIndexFormat::U32;
	mesh->Mat = MaterialPtr;
	mesh->Vb = vb;
	mesh->Ib = ib;

	// Initial Draws = every chunk. Step 1.5 frustum culling will replace
	// this list per frame.
	mesh->Draws.reserve(ChunkInfos.size());
	for (const ChunkMeshInfo& ci : ChunkInfos)
	{
		Mesh::DrawCall dc{};
		dc.mat = MaterialPtr;
		dc.IndexStart = ci.IndexStart;
		dc.IndexCount = ci.IndexCount;
		dc.VertexBase = ci.VertexBase;
		dc.VertexCount = ci.VertexCount;
		mesh->Draws.push_back(dc);
	}
	MeshPtr = mesh;

	ScenePtr = std::make_shared<Scene>();
	ScenePtr->Materials.push_back(MaterialPtr);
	ScenePtr->meshes.push_back(MeshPtr);
	ScenePtr->bHasBounds = true;
	ScenePtr->BoundsMin = glm::vec3(0.0f, Params.HeightMin, 0.0f);
	ScenePtr->BoundsMax = glm::vec3(
		static_cast<float>(Params.Width  - 1) * Params.WorldScaleXZ,
		Params.HeightMax,
		static_cast<float>(Params.Depth  - 1) * Params.WorldScaleXZ);

	const auto tUpload1 = clock::now();
	const auto upMs = std::chrono::duration_cast<std::chrono::milliseconds>(tUpload1 - tUpload0).count();
	std::wostringstream w;
	w << L"[Terrain] uploaded chunks=" << ChunkInfos.size()
	  << L" verts=" << built.Vertices.size()
	  << L" idx=" << built.Indices.size()
	  << L" vb=" << (vbBytes / (1024u * 1024u)) << L"MB"
	  << L" ib=" << (ibBytes / (1024u * 1024u)) << L"MB"
	  << L" upload=" << upMs << L"ms";
	AppendCpuRuntimeTrace(w.str());

	return true;
}

}
