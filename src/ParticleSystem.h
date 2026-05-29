#pragma once

// ParticleSystem — CPU-sim 2D billboard particles.
//
// Phase 1 / M1: single static particle, fixed world position, no sim, no
// collision. Renders camera-facing through a procedural spark texture as an
// additive HDR sprite injected after LightingPass. Subsequent milestones add
// spawn/integrate/lifetime kill (M2), alpha-blend smoke (M3), terrain
// collision (M4), Luau bindings (M5), and Vulkan parity (M6).
//
// CPU expands 4 verts per particle in world space along the view-space
// right/up basis (computed once per frame from the view matrix), so the VS
// only does the world→clip transform.

#include <cstdint>
#include <memory>
#include <vector>

#include "glm/mat4x4.hpp"
#include "glm/vec3.hpp"
#include "glm/vec4.hpp"

class IRenderBackend;
class VertexBuffer;
class IndexBuffer;
class Texture;
namespace Terrain { class Component; }

namespace Particles
{
	enum class EKind : uint8_t
	{
		Spark = 0,
		Smoke = 1,
	};

	struct EmitterParams
	{
		EKind     Kind          = EKind::Spark;
		uint32_t  Capacity      = 256;
		float     SpawnRate     = 0.0f;   // particles/sec; M1 keeps this 0
		float     Lifetime      = 1.5f;
		glm::vec3 Origin        = glm::vec3(0.0f);
		glm::vec3 InitialSpeed  = glm::vec3(0.0f, 6.0f, 0.0f);
		float     SpeedJitter   = 2.0f;
		float     StartSize     = 0.25f;
		float     EndSize       = 0.10f;
		glm::vec4 StartColor    = glm::vec4(1.0f, 0.85f, 0.3f, 1.0f);
		glm::vec4 EndColor      = glm::vec4(0.4f, 0.05f, 0.0f, 0.0f);
		uint32_t  Seed          = 0x5EEDu;
		// Collision tuning. MaxBounces=0 disables collision response (purely
		// kill-on-hit); >=1 enables bouncing physics with the supplied
		// restitution and tangential friction.
		uint8_t   MaxBounces    = 2;
		float     Restitution   = 0.45f;
		float     TangentFriction = 0.55f;
	};

	// CPU-side SoA particle pool. M1 uses only Position/StartColor/StartSize
	// for the single static particle; M2 fills Velocity/Age/Lifetime.
	struct PoolSoA
	{
		std::vector<glm::vec3> Position;
		std::vector<glm::vec3> Velocity;
		std::vector<float>     Age;
		std::vector<float>     Lifetime;
		std::vector<glm::vec4> StartColor;
		std::vector<glm::vec4> EndColor;
		std::vector<float>     StartSize;
		std::vector<float>     EndSize;
		// Bounce count consumed by terrain collision. Particles are killed
		// after exceeding EmitterParams::MaxBounces.
		std::vector<uint8_t>   BounceCount;

		void Resize(uint32_t capacity);
		uint32_t Capacity() const { return static_cast<uint32_t>(Position.size()); }
	};

	// 36 B per vertex. Matches Shaders/Particle.hlsl VSInput layout.
	struct QuadVertex
	{
		float Position[3];
		float Uv[2];
		float Color[4];
	};

	class System
	{
	public:
		System() = default;
		~System() = default;

		System(const System&) = delete;
		System& operator=(const System&) = delete;

		// Allocate the pool, build the static IB (6 indices per particle, all
		// quads point at the same shared index pattern), allocate the upload-
		// heap VB, and load (or generate-and-cache) the kind-specific procedural
		// texture. Returns false on allocation failure.
		bool Initialize(IRenderBackend* backend, const EmitterParams& params);

		// Integrate the live pool by `dt`: gravity, lifetime aging, dead-slot
		// recycling. M1 also pins a single static particle at EmitterParams::
		// Origin when `bStaticAnchor` is set, so the smoke test works without
		// any spawn call.
		void Tick(float dt);

		// Spawn `count` particles centered at `origin` (overrides Origin for
		// this burst only). Emission cone is upward ±30° with magnitude
		// derived from EmitterParams::InitialSpeed + SpeedJitter. Returns the
		// number actually spawned — may be smaller than `count` if the pool
		// is full of live particles.
		uint32_t SpawnBurst(const glm::vec3& origin, uint32_t count);

		void SetStaticAnchor(bool value) { bStaticAnchor = value; }
		// Late-bound terrain collider. Corona resets this every frame from
		// the live ActiveTerrain pointer so that maps which spawn/destroy
		// terrain at runtime don't leave a stale collider behind.
		void SetTerrainCollider(Terrain::Component* terrain) { Terrain = terrain; }

		// Build the per-particle 4 verts (camera-aligned) and memcpy into the
		// upload-heap VB. Must be called every frame after Tick so the VB
		// matches the active particle set.
		void UploadQuadVertices(IRenderBackend* backend, const glm::mat4& viewMat);

		const EmitterParams& GetParams() const { return Params; }
		const std::shared_ptr<VertexBuffer>& GetVb() const { return Vb; }
		const std::shared_ptr<IndexBuffer>&  GetIb() const { return Ib; }
		const std::shared_ptr<Texture>&      GetTexture() const { return Tex; }
		uint32_t GetActiveCount() const { return ActiveCount; }
		uint32_t GetIndexCount() const { return ActiveCount * 6u; }

	private:
		uint32_t FindDeadSlot();

		EmitterParams Params{};
		PoolSoA Pool{};
		uint32_t ActiveCount = 0;
		uint32_t NextSearchSlot = 0;
		uint32_t RngState = 0;
		bool bStaticAnchor = false;
		Terrain::Component* Terrain = nullptr;

		std::shared_ptr<VertexBuffer> Vb;
		std::shared_ptr<IndexBuffer>  Ib;
		std::shared_ptr<Texture>      Tex;

		std::vector<QuadVertex> CpuVerts; // staging — copied each frame into Vb
	};

	// Procedural texture factories. Public so the demo bootstrap path can
	// preload them ahead of the first particle system. Returns a cached
	// texture that's safe to share across systems of the same kind.
	std::shared_ptr<Texture> CreateOrLoadSparkTexture(IRenderBackend* backend);
	std::shared_ptr<Texture> CreateOrLoadSmokeTexture(IRenderBackend* backend);
}
