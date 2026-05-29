#include "TerrainComponent.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>
#include <string>

#include "glm/glm.hpp"

#include "CoronaImageIO.h"
#include "RenderBackend.h"
#include "RenderResources.h"
#include "TerrainGenerator.h"
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

	// sRGB encode (linear → sRGB). The GBuffer pipeline loads diffuse PNGs as
	// sRGB (nonSRGB=false) and the GPU sample auto-decodes to linear, so we
	// pre-encode here to preserve the linear values we picked.
	inline uint8_t LinearToSrgb8(float linear)
	{
		linear = std::clamp(linear, 0.0f, 1.0f);
		const float srgb = (linear <= 0.0031308f)
			? (linear * 12.92f)
			: (1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f);
		return static_cast<uint8_t>(srgb * 255.0f + 0.5f);
	}

	// Procedural grass+dirt albedo for the terrain. Low-freq Simplex picks a
	// blend weight (0 = grass, 1 = dirt) and a second high-freq Simplex layer
	// adds per-texel variation within each material. Output is a tileable
	// RGBA8 image; UV tiling is handled at the mesh side (uvTileSize=16 m).
	void GenerateTerrainAlbedoPixels(int width, int height, uint32_t seed,
		std::vector<uint8_t>& outRgba)
	{
		outRgba.resize(static_cast<size_t>(width) * height * 4u);

		// Materials: grass (mossy green) and dirt (warm brown). Similar
		// luminance to the grass-blade material so the terrain doesn't
		// dominate auto-exposure relative to the wind-swayed grass field.
		const glm::vec3 grass(0.12f, 0.28f, 0.08f);
		const glm::vec3 dirt (0.36f, 0.24f, 0.12f);

		const uint32_t blendSeed  = seed ^ 0xA1B2C3D4u;
		const uint32_t detailSeed = seed ^ 0x5E5E5E5Eu;

		// 2 cycles across the texture → ~8 m patch wavelength on the ground
		// (UV tiles every 16 m). Detail noise breaks up the patch boundaries.
		const float blendFreq  = 2.0f / static_cast<float>(width);
		const float detailFreq = 24.0f / static_cast<float>(width);

		// Noise periods (in noise-space units) so the texture seams seamlessly
		// when the GPU sampler wraps the UV. Equal to (width * freq) — one
		// full noise cycle covers the texture, so sampling at (x - W) gives
		// the same value at the opposite edge.
		const float blendPeriod  = blendFreq  * static_cast<float>(width);  // = 2.0
		const float detailPeriod = detailFreq * static_cast<float>(width);  // = 24.0

		// Tileable Simplex via 4-corner blend: sample at (x,y), (x-W,y),
		// (x,y-H), (x-W,y-H) and lerp by (x/W, y/H). At any edge, the
		// contribution shifts to the wrapped-coord sample, which matches
		// the value at the opposite edge — eliminates the UV-wrap seam
		// that produces the cross-hatch on the rendered terrain.
		auto tileableSimplex = [](float fx, float fy, float W, float H, uint32_t s)
		{
			const float n00 = Simplex2D(fx,     fy,     s);
			const float n10 = Simplex2D(fx - W, fy,     s);
			const float n01 = Simplex2D(fx,     fy - H, s);
			const float n11 = Simplex2D(fx - W, fy - H, s);
			const float tx = fx / W;
			const float ty = fy / H;
			const float a = n00 * (1.0f - tx) + n10 * tx;
			const float b = n01 * (1.0f - tx) + n11 * tx;
			return a * (1.0f - ty) + b * ty;
		};

		for (int y = 0; y < height; ++y)
		{
			for (int x = 0; x < width; ++x)
			{
				const float fx = static_cast<float>(x);
				const float fy = static_cast<float>(y);

				// Three-octave tileable FBM for the blend mask. Each octave
				// must use a period that's an integer multiple of the texture
				// width — at octave k (period multiplier mk), use period
				// blendPeriod * mk so wrap stays seamless.
				const float n0 = tileableSimplex(fx * blendFreq,        fy * blendFreq,        blendPeriod,        blendPeriod,        blendSeed);
				const float n1 = tileableSimplex(fx * blendFreq * 2.0f, fy * blendFreq * 2.0f, blendPeriod * 2.0f, blendPeriod * 2.0f, blendSeed ^ 0x55555555u);
				const float n2 = tileableSimplex(fx * blendFreq * 4.0f, fy * blendFreq * 4.0f, blendPeriod * 4.0f, blendPeriod * 4.0f, blendSeed ^ 0xAAAAAAAAu);
				const float blendNoise = 0.55f * n0 + 0.30f * n1 + 0.15f * n2;
				// Smoothstep (cubic Hermite) instead of linear+clamp — gives
				// a soft ease in/out around the threshold so patches blur
				// into each other rather than crossing a hard line.
				const float blendRaw = std::clamp(blendNoise * 0.65f + 0.5f, 0.0f, 1.0f);
				const float t = std::clamp((blendRaw - 0.30f) / 0.40f, 0.0f, 1.0f);
				const float blendWeight = t * t * (3.0f - 2.0f * t);

				const float dn = tileableSimplex(fx * detailFreq, fy * detailFreq, detailPeriod, detailPeriod, detailSeed);
				const float detail = dn * 0.5f + 0.5f;     // 0..1
				const float jitter = (detail - 0.5f) * 0.18f; // ±0.09 RGB variation

				glm::vec3 color = glm::mix(grass, dirt, blendWeight);
				color += glm::vec3(jitter * 0.7f, jitter, jitter * 0.6f);
				color = glm::clamp(color, glm::vec3(0.0f), glm::vec3(1.0f));

				const size_t i = (static_cast<size_t>(y) * width + x) * 4u;
				outRgba[i + 0] = LinearToSrgb8(color.r);
				outRgba[i + 1] = LinearToSrgb8(color.g);
				outRgba[i + 2] = LinearToSrgb8(color.b);
				outRgba[i + 3] = 255u;
			}
		}
	}

	std::shared_ptr<Texture> CreateOrLoadTerrainAlbedoTexture(
		IRenderBackend* backend, uint32_t seed)
	{
		if (!backend)
			return nullptr;

		const int kTexSize = 512;
		std::filesystem::path dir =
			RuntimePaths::AssetDirectory() / L"terrain_cache";
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		std::filesystem::path texPath =
			dir / (L"terrain_albedo_" + std::to_wstring(seed) +
				   L"_" + std::to_wstring(kTexSize) + L".png");

		if (!std::filesystem::exists(texPath))
		{
			std::vector<uint8_t> pixels;
			GenerateTerrainAlbedoPixels(kTexSize, kTexSize, seed, pixels);

			CapturedImage img;
			img.Format = ETextureFormat::RGBA8Unorm;
			img.Width = kTexSize;
			img.Height = kTexSize;
			img.RowPitch = kTexSize * 4u;
			img.Pixels = std::move(pixels);

			std::wstring err;
			if (!CoronaImageIO::SavePNG(img, texPath.wstring(), &err))
			{
				AppendCpuRuntimeTrace(L"[Terrain] albedo PNG save failed: " + err);
				return nullptr;
			}
			AppendCpuRuntimeTrace(L"[Terrain] albedo PNG generated " + texPath.wstring());
		}

		return backend->CreateTextureFromFile(texPath.wstring(), /*nonSRGB=*/false);
	}
}

std::filesystem::path Component::ResolveCachePath(const std::wstring& cacheName) const
{
	std::filesystem::path dir = RuntimePaths::AssetDirectory() / L"terrain_cache";
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

	// 257 verts per side = 256 quads per chunk → at 1 m vertex spacing a
	// chunk covers 256 m × 256 m of world. For a 2km × 2km terrain that
	// gives 8 × 8 = 64 chunks total — large enough that the CPU cull cost
	// is trivial but small enough that frustum culling still rejects a
	// meaningful fraction once the camera flies inside the world.
	const uint32_t chunkVerts = 257u;
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

float Component::SampleHeight(float worldX, float worldZ) const
{
	if (Data.Chunks.empty() || Data.Header.Width == 0 || Data.Header.Depth == 0)
		return 0.0f;

	const float worldScale = Data.Header.WorldScaleXZ;
	const uint32_t cs = Data.Header.ChunkSize;
	if (cs == 0 || worldScale <= 0.0f)
		return 0.0f;

	// Vertices are pre-centered: world (-halfX..halfX) maps to sample (0..Width-1).
	const float halfX = static_cast<float>(Data.Header.Width  - 1) * worldScale * 0.5f;
	const float halfZ = static_cast<float>(Data.Header.Depth  - 1) * worldScale * 0.5f;

	const float sxF = (worldX + halfX) / worldScale;
	const float szF = (worldZ + halfZ) / worldScale;

	const float widthMinus1 = static_cast<float>(Data.Header.Width  - 1);
	const float depthMinus1 = static_cast<float>(Data.Header.Depth  - 1);

	const float clX = std::clamp(sxF, 0.0f, widthMinus1);
	const float clZ = std::clamp(szF, 0.0f, depthMinus1);

	const uint32_t x0 = static_cast<uint32_t>(clX);
	const uint32_t z0 = static_cast<uint32_t>(clZ);
	const uint32_t x1 = (std::min)(x0 + 1u, Data.Header.Width  - 1u);
	const uint32_t z1 = (std::min)(z0 + 1u, Data.Header.Depth  - 1u);
	const float fx = clX - static_cast<float>(x0);
	const float fz = clZ - static_cast<float>(z0);

	auto sampleAt = [&](uint32_t sx, uint32_t sz) -> float
	{
		const uint32_t quads = cs - 1u;
		const uint32_t cx = (std::min)(sx / quads, Data.Header.NumChunksX - 1u);
		const uint32_t cz = (std::min)(sz / quads, Data.Header.NumChunksZ - 1u);
		const uint32_t lx = sx - cx * quads;
		const uint32_t lz = sz - cz * quads;
		const uint32_t lxC = (std::min)(lx, cs - 1u);
		const uint32_t lzC = (std::min)(lz, cs - 1u);
		const ChunkData& chunk = Data.Chunks[cz * Data.Header.NumChunksX + cx];
		return Data.DecodeHeight(chunk.Heights[static_cast<size_t>(lzC) * cs + lxC]);
	};

	const float h00 = sampleAt(x0, z0);
	const float h10 = sampleAt(x1, z0);
	const float h01 = sampleAt(x0, z1);
	const float h11 = sampleAt(x1, z1);

	// Match TerrainMeshBuilder's triangulation: diagonal from i01 to i10.
	// Lower-left tri (fx + fz <= 1) uses (h00, h01, h10); upper-right tri
	// uses (h10, h01, h11). Bilinear ≠ triangular interp on sloped quads —
	// using triangular here keeps grass roots glued to the rendered surface
	// instead of sliding above/below it on hillsides.
	if (fx + fz <= 1.0f)
	{
		return (1.0f - fx - fz) * h00 + fz * h01 + fx * h10;
	}
	return (1.0f - fz) * h10 + (1.0f - fx) * h01 + (fx + fz - 1.0f) * h11;
}

uint32_t Component::UpdateCulling(const glm::mat4& viewProj)
{
	if (!MeshPtr || ChunkInfos.empty())
		return 0;

	MeshPtr->Draws.clear();
	MeshPtr->Draws.reserve(ChunkInfos.size());

	static uint32_t s_frameCounter = 0;
	static uint32_t s_lastVisible = 0xFFFFFFFFu;
	++s_frameCounter;

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

	LastVisibleChunkCount = visible;

	// Log when visible-chunk count changes (rate-limited to once per ~30
	// frames per change) so the trace shows that per-chunk frustum culling
	// is actually active — the engine-side GBufferCulling counter only
	// sees the terrain Mesh as a single entity.
	const bool bChanged = (visible != s_lastVisible);
	if (bChanged && (s_frameCounter % 30u) == 0u)
	{
		AppendCpuRuntimeTrace(
			L"[Terrain] chunk cull visible=" + std::to_wstring(visible) +
			L"/" + std::to_wstring(ChunkInfos.size()));
		s_lastVisible = visible;
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

	// Procedural grass+dirt albedo (cached PNG keyed by seed). BaseColorFactor
	// stays white so the shader uses the texture unmodulated; pass 2 of
	// terrain texturing (normal/roughness maps) can layer on top.
	MaterialPtr = std::make_shared<Material>();
	MaterialPtr->BaseColorFactor = glm::vec4(1.0f);
	MaterialPtr->Diffuse = CreateOrLoadTerrainAlbedoTexture(backend, Params.Seed);
	if (!MaterialPtr->Diffuse)
	{
		AppendCpuRuntimeTrace(L"[Terrain] albedo texture load failed; falling back to flat gray");
		MaterialPtr->BaseColorFactor = glm::vec4(0.55f, 0.55f, 0.55f, 1.0f);
	}
	else
	{
		std::wostringstream w;
		w << L"[Terrain] albedo texture bound: ptr=" << MaterialPtr->Diffuse.get()
		  << L" w=" << MaterialPtr->Diffuse->Width
		  << L" h=" << MaterialPtr->Diffuse->Height
		  << L" mips=" << MaterialPtr->Diffuse->MipLevels;
		AppendCpuRuntimeTrace(w.str());
	}

	auto mesh = std::make_shared<Mesh>(backend);
	mesh->transform = glm::mat4x4(1.0f);
	mesh->NumVertices = static_cast<uint32_t>(built.Vertices.size());
	mesh->NumIndices = static_cast<uint32_t>(built.Indices.size());
	mesh->VertexStride = sizeof(MeshVertex);
	mesh->IndexFormat = EIndexFormat::U32;
	mesh->Mat = MaterialPtr;
	mesh->Vb = vb;
	mesh->Ib = ib;
	mesh->bTerrainMesh = true;

	// CPU-side mirror intentionally left empty — Phase 1 terrain doesn't
	// participate in physics or RT. Filling 1M positions also caused a
	// downstream consumer to crash on DX12 (under investigation).

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
	// Mesh vertices are pre-centered around origin (see TerrainMeshBuilder).
	// Reflect that in Scene bounds so BuildCenteredSceneTransform's center
	// is (0,0,0) → its auto-translation is identity → world == mesh space.
	const float halfX = static_cast<float>(Params.Width  - 1) * Params.WorldScaleXZ * 0.5f;
	const float halfZ = static_cast<float>(Params.Depth  - 1) * Params.WorldScaleXZ * 0.5f;
	ScenePtr->BoundsMin = glm::vec3(-halfX, Params.HeightMin, -halfZ);
	ScenePtr->BoundsMax = glm::vec3( halfX, Params.HeightMax,  halfZ);

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

	// Linear decoded heightmap → structured buffer for procedural grass.
	// Reassembled from the chunked uint16 layout once at terrain init —
	// the procedural grass VS samples it every frame to anchor each blade
	// to the terrain surface.
	{
		const uint32_t W = Data.Header.Width;
		const uint32_t D = Data.Header.Depth;
		const uint32_t chunkSize = Data.Header.ChunkSize;
		const uint32_t numChunksX = Data.Header.NumChunksX;
		std::vector<float> linear(static_cast<size_t>(W) * D, 0.0f);
		for (size_t c = 0; c < Data.Chunks.size(); ++c)
		{
			const auto& chunk = Data.Chunks[c];
			const uint32_t cx = static_cast<uint32_t>(c % numChunksX);
			const uint32_t cz = static_cast<uint32_t>(c / numChunksX);
			const uint32_t baseX = cx * (chunkSize - 1u);
			const uint32_t baseZ = cz * (chunkSize - 1u);
			for (uint32_t lz = 0; lz < chunkSize; ++lz)
			{
				for (uint32_t lx = 0; lx < chunkSize; ++lx)
				{
					const uint32_t worldX = baseX + lx;
					const uint32_t worldZ = baseZ + lz;
					if (worldX >= W || worldZ >= D) continue;
					const size_t srcIdx = static_cast<size_t>(lz) * chunkSize + lx;
					const size_t dstIdx = static_cast<size_t>(worldZ) * W + worldX;
					linear[dstIdx] = Data.DecodeHeight(chunk.Heights[srcIdx]);
				}
			}
		}
		HeightBuffer = backend->CreateUploadStructuredBuffer(
			static_cast<uint32_t>(linear.size()), sizeof(float));
		if (HeightBuffer)
		{
			backend->UpdateUploadStructuredBuffer(
				HeightBuffer.get(), linear.data(),
				static_cast<uint32_t>(linear.size() * sizeof(float)));
			AppendCpuRuntimeTrace(
				L"[Terrain] heightfield buffer " + std::to_wstring(W) + L"x" + std::to_wstring(D) +
				L" (" + std::to_wstring(linear.size() * sizeof(float) / (1024 * 1024)) + L" MB)");
		}
		else
		{
			AppendCpuRuntimeTrace(L"[Terrain] heightfield buffer allocation failed");
		}
	}

	return true;
}

}
