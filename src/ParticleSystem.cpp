#include "ParticleSystem.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "glm/glm.hpp"

#include "CoronaImageIO.h"
#include "RenderBackend.h"
#include "RenderResources.h"
#include "TerrainComponent.h"
#include "Utils.h"

void AppendCpuRuntimeTrace(const std::wstring& line);

namespace Particles
{
namespace
{
	constexpr int kProceduralTexSize = 64;

	inline uint8_t F32ToU8(float v)
	{
		const float c = std::clamp(v, 0.0f, 1.0f);
		return static_cast<uint8_t>(c * 255.0f + 0.5f);
	}

	// Spark sprite: white-hot core with a sharp distance falloff. Stored
	// non-sRGB (alpha mask integrity matters more than perceptual color);
	// engine treats it via CreateTextureFromFile(nonSRGB=true).
	void GenerateSparkPixels(std::vector<uint8_t>& outRgba)
	{
		const int N = kProceduralTexSize;
		outRgba.resize(static_cast<size_t>(N) * N * 4u);
		for (int y = 0; y < N; ++y)
		{
			for (int x = 0; x < N; ++x)
			{
				const float cx = (static_cast<float>(x) + 0.5f) / N - 0.5f;
				const float cy = (static_cast<float>(y) + 0.5f) / N - 0.5f;
				const float r  = std::sqrt(cx * cx + cy * cy) * 2.0f; // 0 at center, 1 at edge inscribed circle
				const float fall = 1.0f - std::clamp(r, 0.0f, 1.0f);
				const float alpha = fall * fall;            // soft edge
				const float core  = fall * fall * fall * fall; // hot core boost
				const float rc = std::clamp(1.0f * core + 0.6f * alpha, 0.0f, 1.0f);
				const float gc = std::clamp(0.85f * core + 0.4f * alpha, 0.0f, 1.0f);
				const float bc = std::clamp(0.4f * core + 0.1f * alpha, 0.0f, 1.0f);

				const size_t i = (static_cast<size_t>(y) * N + x) * 4u;
				outRgba[i + 0] = F32ToU8(rc);
				outRgba[i + 1] = F32ToU8(gc);
				outRgba[i + 2] = F32ToU8(bc);
				outRgba[i + 3] = F32ToU8(alpha);
			}
		}
	}

	// Smoke sprite: soft radial puff (M1 placeholder — M3 will replace with
	// FBM-noise * radial mask once the alpha-blend pipeline lands).
	void GenerateSmokePixels(std::vector<uint8_t>& outRgba)
	{
		const int N = kProceduralTexSize;
		outRgba.resize(static_cast<size_t>(N) * N * 4u);
		for (int y = 0; y < N; ++y)
		{
			for (int x = 0; x < N; ++x)
			{
				const float cx = (static_cast<float>(x) + 0.5f) / N - 0.5f;
				const float cy = (static_cast<float>(y) + 0.5f) / N - 0.5f;
				const float r  = std::sqrt(cx * cx + cy * cy) * 2.0f;
				const float t = std::clamp(r, 0.0f, 1.0f);
				const float fall = t * t * (3.0f - 2.0f * t); // smoothstep
				const float alpha = (1.0f - fall) * 0.85f;
				const uint8_t gray = F32ToU8(0.65f);

				const size_t i = (static_cast<size_t>(y) * N + x) * 4u;
				outRgba[i + 0] = gray;
				outRgba[i + 1] = gray;
				outRgba[i + 2] = gray;
				outRgba[i + 3] = F32ToU8(alpha);
			}
		}
	}

	std::shared_ptr<Texture> LoadOrGeneratePNG(
		IRenderBackend* backend,
		const std::wstring& fileName,
		void (*gen)(std::vector<uint8_t>&))
	{
		if (!backend)
			return nullptr;

		const std::filesystem::path dir =
			RuntimePaths::AssetDirectory() / L"particle_cache";
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		const std::filesystem::path texPath = dir / fileName;

		if (!std::filesystem::exists(texPath))
		{
			std::vector<uint8_t> pixels;
			gen(pixels);

			CapturedImage img;
			img.Format = ETextureFormat::RGBA8Unorm;
			img.Width = kProceduralTexSize;
			img.Height = kProceduralTexSize;
			img.RowPitch = kProceduralTexSize * 4u;
			img.Pixels = std::move(pixels);

			std::wstring err;
			if (!CoronaImageIO::SavePNG(img, texPath.wstring(), &err))
			{
				AppendCpuRuntimeTrace(L"[Particles] PNG save failed: " + err);
				return nullptr;
			}
			AppendCpuRuntimeTrace(L"[Particles] PNG generated " + texPath.wstring());
		}

		// nonSRGB=true: spark texture is authored in linear with deliberate
		// alpha mask; sRGB decode would shift the additive contribution.
		return backend->CreateTextureFromFile(texPath.wstring(), /*nonSRGB=*/true);
	}
}

std::shared_ptr<Texture> CreateOrLoadSparkTexture(IRenderBackend* backend)
{
	return LoadOrGeneratePNG(backend, L"spark_64.png", &GenerateSparkPixels);
}

std::shared_ptr<Texture> CreateOrLoadSmokeTexture(IRenderBackend* backend)
{
	return LoadOrGeneratePNG(backend, L"smoke_64.png", &GenerateSmokePixels);
}

void PoolSoA::Resize(uint32_t capacity)
{
	Position.assign(capacity, glm::vec3(0.0f));
	Velocity.assign(capacity, glm::vec3(0.0f));
	Age.assign(capacity, 0.0f);
	Lifetime.assign(capacity, 0.0f);
	StartColor.assign(capacity, glm::vec4(1.0f));
	EndColor.assign(capacity, glm::vec4(0.0f));
	StartSize.assign(capacity, 0.1f);
	EndSize.assign(capacity, 0.1f);
	BounceCount.assign(capacity, 0u);
}

bool System::Initialize(IRenderBackend* backend, const EmitterParams& params)
{
	if (!backend || params.Capacity == 0)
		return false;

	Params = params;
	Pool.Resize(params.Capacity);
	ActiveCount = 0;
	NextSearchSlot = 0;
	RngState = params.Seed ? params.Seed : 0x5EEDu;

	// Build the static IB once. Index pattern per quad: (0,1,2, 0,2,3) with
	// every quad's vertices offset by 4. Vulkan and DX12 share U32 IBs.
	std::vector<uint32_t> indices;
	indices.reserve(static_cast<size_t>(params.Capacity) * 6u);
	for (uint32_t q = 0; q < params.Capacity; ++q)
	{
		const uint32_t base = q * 4u;
		indices.push_back(base + 0);
		indices.push_back(base + 1);
		indices.push_back(base + 2);
		indices.push_back(base + 0);
		indices.push_back(base + 2);
		indices.push_back(base + 3);
	}
	const uint32_t ibBytes = static_cast<uint32_t>(indices.size() * sizeof(uint32_t));
	Ib = backend->CreateUploadIndexBuffer(EIndexFormat::U32, ibBytes, indices.data());
	if (!Ib)
	{
		AppendCpuRuntimeTrace(L"[Particles] IB allocation failed");
		return false;
	}

	// Allocate the upload-heap VB at full capacity. UploadQuadVertices
	// rewrites a prefix [0, ActiveCount*4) each frame via in-place memcpy.
	CpuVerts.resize(static_cast<size_t>(params.Capacity) * 4u);
	std::memset(CpuVerts.data(), 0, CpuVerts.size() * sizeof(QuadVertex));
	const uint32_t vbBytes = static_cast<uint32_t>(CpuVerts.size() * sizeof(QuadVertex));
	Vb = backend->CreateUploadVertexBuffer(vbBytes, sizeof(QuadVertex), CpuVerts.data());
	if (!Vb)
	{
		AppendCpuRuntimeTrace(L"[Particles] VB allocation failed");
		return false;
	}

	Tex = (params.Kind == EKind::Smoke)
		? CreateOrLoadSmokeTexture(backend)
		: CreateOrLoadSparkTexture(backend);
	if (!Tex)
	{
		AppendCpuRuntimeTrace(L"[Particles] texture load failed");
		return false;
	}

	std::wostringstream w;
	w << L"[Particles] initialized kind="
	  << (params.Kind == EKind::Smoke ? L"smoke" : L"spark")
	  << L" capacity=" << params.Capacity
	  << L" vb=" << vbBytes << L"B ib=" << ibBytes << L"B"
	  << L" tex=" << Tex.get() << L" " << Tex->Width << L"x" << Tex->Height;
	AppendCpuRuntimeTrace(w.str());

	return true;
}

namespace
{
	// Tiny LCG; good enough for spread vectors, dramatically smaller than
	// std::mt19937 in CPU+memory footprint and trivially deterministic.
	inline uint32_t XorShift32(uint32_t& s)
	{
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		return s;
	}
	inline float RandUnitFloat(uint32_t& s)
	{
		return static_cast<float>(XorShift32(s) & 0xFFFFFFu) / 16777215.0f;
	}
	inline float RandSigned(uint32_t& s) { return RandUnitFloat(s) * 2.0f - 1.0f; }
}

uint32_t System::FindDeadSlot()
{
	const uint32_t cap = Pool.Capacity();
	if (cap == 0)
		return UINT32_MAX;
	for (uint32_t step = 0; step < cap; ++step)
	{
		const uint32_t idx = (NextSearchSlot + step) % cap;
		if (Pool.Lifetime[idx] <= 0.0f)
		{
			NextSearchSlot = (idx + 1u) % cap;
			return idx;
		}
	}
	return UINT32_MAX;
}

void System::Tick(float dt)
{
	if (Pool.Capacity() == 0)
	{
		ActiveCount = 0;
		return;
	}

	// Gravity is spark-default for now; smoke variant overrides in M3 via
	// a per-kind tuning struct. Keep the magnitude approximate to real
	// gravity so 1 m emitter rise → 1 m fall reads naturally.
	const glm::vec3 gravity(0.0f, -9.81f, 0.0f);

	// M1 anchor: pin slot 0 to Origin so a parked viewer always sees one
	// billboard, even before SpawnBurst is wired into Luau.
	uint32_t startIdx = 0;
	if (bStaticAnchor)
	{
		Pool.Position[0]   = Params.Origin;
		Pool.Velocity[0]   = glm::vec3(0.0f);
		Pool.Age[0]        = 0.0f;
		Pool.Lifetime[0]   = Params.Lifetime > 0.0f ? Params.Lifetime : 9999.0f;
		Pool.StartColor[0] = Params.StartColor;
		Pool.EndColor[0]   = Params.EndColor;
		Pool.StartSize[0]  = Params.StartSize;
		Pool.EndSize[0]    = Params.EndSize;
		startIdx = 1;
	}

	uint32_t alive = bStaticAnchor ? 1u : 0u;
	const uint32_t cap = Pool.Capacity();
	for (uint32_t i = startIdx; i < cap; ++i)
	{
		if (Pool.Lifetime[i] <= 0.0f)
			continue;
		Pool.Age[i] += dt;
		if (Pool.Age[i] >= Pool.Lifetime[i])
		{
			Pool.Lifetime[i] = 0.0f;
			continue;
		}
		Pool.Velocity[i] += gravity * dt;
		Pool.Position[i] += Pool.Velocity[i] * dt;

		// Terrain bounce. Sampled at the post-step XZ — cheap and good
		// enough for spark-scale particles. Normal is approximated by the
		// world-up axis (Phase 1 simplification); finite-difference normal
		// is a tunable swap-in once visual quality demands it.
		if (Terrain)
		{
			const float groundY = Terrain->SampleHeight(
				Pool.Position[i].x, Pool.Position[i].z);
			if (Pool.Position[i].y < groundY)
			{
				if (Pool.BounceCount[i] >= Params.MaxBounces)
				{
					Pool.Lifetime[i] = 0.0f;
					continue;
				}
				Pool.Position[i].y = groundY;
				// Reflect Y, apply tangential friction to XZ. Restitution
				// caps the rebound height; friction kills the slide so
				// sparks settle quickly instead of skating off forever.
				if (Pool.Velocity[i].y < 0.0f)
					Pool.Velocity[i].y = -Pool.Velocity[i].y * Params.Restitution;
				Pool.Velocity[i].x *= Params.TangentFriction;
				Pool.Velocity[i].z *= Params.TangentFriction;
				++Pool.BounceCount[i];
			}
		}
		++alive;
	}
	ActiveCount = alive;
}

uint32_t System::SpawnBurst(const glm::vec3& origin, uint32_t count)
{
	if (Pool.Capacity() == 0)
		return 0;

	const float baseSpeed = glm::length(Params.InitialSpeed);
	const glm::vec3 baseDir = (baseSpeed > 1e-4f)
		? Params.InitialSpeed / baseSpeed
		: glm::vec3(0.0f, 1.0f, 0.0f);

	uint32_t spawned = 0;
	for (uint32_t i = 0; i < count; ++i)
	{
		const uint32_t idx = FindDeadSlot();
		if (idx == UINT32_MAX)
			break;

		// Cone-spread velocity around baseDir. Build an orthonormal basis
		// (baseDir, t1, t2) and offset by (~±sin(spread)) in tangent plane.
		glm::vec3 t1 = std::fabs(baseDir.y) < 0.95f
			? glm::normalize(glm::cross(baseDir, glm::vec3(0, 1, 0)))
			: glm::normalize(glm::cross(baseDir, glm::vec3(1, 0, 0)));
		glm::vec3 t2 = glm::cross(baseDir, t1);
		const float spreadTan = 0.577f; // tan(30°)
		const float jitter = baseSpeed + Params.SpeedJitter * RandSigned(RngState);
		glm::vec3 vel = baseDir + t1 * (spreadTan * RandSigned(RngState))
			                    + t2 * (spreadTan * RandSigned(RngState));
		vel = glm::normalize(vel) * (std::max)(0.0f, jitter);

		Pool.Position[idx]    = origin;
		Pool.Velocity[idx]    = vel;
		Pool.Age[idx]         = 0.0f;
		Pool.Lifetime[idx]    = Params.Lifetime;
		Pool.StartColor[idx]  = Params.StartColor;
		Pool.EndColor[idx]    = Params.EndColor;
		Pool.StartSize[idx]   = Params.StartSize;
		Pool.EndSize[idx]     = Params.EndSize;
		Pool.BounceCount[idx] = 0u;
		++spawned;
	}
	return spawned;
}

void System::UploadQuadVertices(IRenderBackend* backend, const glm::mat4& viewMat)
{
	if (!backend || !Vb || ActiveCount == 0u)
		return;

	// Extract camera right/up basis from GLM view (column-major). The basis
	// is the *world-space* direction of view-space +X and +Y, used to expand
	// each particle's center into a screen-aligned quad in world space.
	const glm::vec3 camRight(viewMat[0][0], viewMat[1][0], viewMat[2][0]);
	const glm::vec3 camUp   (viewMat[0][1], viewMat[1][1], viewMat[2][1]);

	static const float kUv[4][2] = {
		{ 0.0f, 1.0f },
		{ 1.0f, 1.0f },
		{ 1.0f, 0.0f },
		{ 0.0f, 0.0f },
	};

	// Walk the whole pool and pack live slots contiguously into CpuVerts
	// (the index buffer expects contiguous quads starting at vertex 0). Dead
	// slots are skipped — ActiveCount limits the draw call.
	uint32_t writeQuad = 0;
	const uint32_t cap = Pool.Capacity();
	for (uint32_t i = 0; i < cap && writeQuad < ActiveCount; ++i)
	{
		if (Pool.Lifetime[i] <= 0.0f)
			continue;

		const float t = Pool.Lifetime[i] > 0.0f
			? std::clamp(Pool.Age[i] / Pool.Lifetime[i], 0.0f, 1.0f)
			: 0.0f;
		const float size = Pool.StartSize[i] + (Pool.EndSize[i] - Pool.StartSize[i]) * t;
		const glm::vec4 col = Pool.StartColor[i]
			+ (Pool.EndColor[i] - Pool.StartColor[i]) * t;

		const glm::vec3& p = Pool.Position[i];
		const glm::vec3 hr = camRight * size;
		const glm::vec3 hu = camUp    * size;
		const glm::vec3 c[4] = {
			p - hr - hu,
			p + hr - hu,
			p + hr + hu,
			p - hr + hu,
		};

		QuadVertex* dst = &CpuVerts[static_cast<size_t>(writeQuad) * 4u];
		for (int v = 0; v < 4; ++v)
		{
			dst[v].Position[0] = c[v].x;
			dst[v].Position[1] = c[v].y;
			dst[v].Position[2] = c[v].z;
			dst[v].Uv[0]       = kUv[v][0];
			dst[v].Uv[1]       = kUv[v][1];
			dst[v].Color[0]    = col.r;
			dst[v].Color[1]    = col.g;
			dst[v].Color[2]    = col.b;
			dst[v].Color[3]    = col.a;
		}
		++writeQuad;
	}

	const uint32_t bytes = writeQuad * 4u * static_cast<uint32_t>(sizeof(QuadVertex));
	if (bytes > 0)
		backend->UpdateUploadVertexBuffer(Vb.get(), CpuVerts.data(), bytes);
}

}
