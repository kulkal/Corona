#pragma once

// API-neutral renderer resource model.
//
// `Mesh`, `Scene`, `Material`, `DrawCall` are pure data containers that the
// renderer manipulates regardless of the active backend.
//
// `Texture`, `Buffer`, `VertexBuffer`, `IndexBuffer`, `Sampler` carry a
// portable API-neutral header (dimensions, element counts, format) plus a
// backend-specific section that is gated by `CORONA_HAS_D3D12`. The Vulkan
// backend uses the same class as an opaque key and stores its native
// VkImage/VkBuffer in a side table (`VulkanBackend::*Allocations`). When
// `CORONA_HAS_D3D12=0`, the DX12-specific members and helper methods are
// compiled out entirely.

#include <cstdint>
#include <memory>
#include <vector>

#include "glm/mat4x4.hpp"
#include "glm/vec3.hpp"
#include "glm/vec4.hpp"

#include "RHIBuildConfig.h"
#include "RenderBackend.h"

#if CORONA_HAS_D3D12
#include <d3d12.h>
#include <wrl/client.h>
#endif

class IRenderBackend;
class DX12Backend;
class RTAS;

class Buffer
{
public:
	~Buffer();

	// API-neutral
	enum BufferType { BYTE_ADDRESS, STRUCTURED, UNKNOWN };
	BufferType Type = UNKNOWN;
	uint32_t NumElements = 0;
	uint32_t ElementSize = 0;
	RHIBufferHandle BindlessHandle{};

#if CORONA_HAS_D3D12
	DX12Backend* Owner = nullptr;
	Microsoft::WRL::ComPtr<ID3D12Resource> resource;
	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleSRV{};
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleSRV{};
	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleBindlessSRV{};
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleBindlessSRV{};
	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleUAV{};
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleUAV{};
	// Persistent CPU pointer for UPLOAD-heap buffers — set by
	// CreateUploadStructuredBuffer. Null for DEFAULT-heap buffers.
	void* MappedPtr = nullptr;
	uint32_t MappedSizeInBytes = 0;
	uint64_t SuballocationOffsetBytes = 0;
	// Persistent staging resource for dynamic updates into DEFAULT-heap buffers.
	Microsoft::WRL::ComPtr<ID3D12Resource> UploadResource;
	void* UploadMappedPtr = nullptr;
	uint32_t UploadMappedSizeInBytes = 0;

	void MakeByteAddressBufferSRV();
	void MakeStructuredBufferSRV();
#endif
};

class IndexBuffer
{
public:
	~IndexBuffer();

	// API-neutral
	int numIndices = 0;
	RHIBufferHandle BindlessHandle{};
	// Same in-place update path as VertexBuffer::MappedCpu.
	void* MappedCpu = nullptr;
	uint32_t MappedCapacityBytes = 0;

#if CORONA_HAS_D3D12
	DX12Backend* Owner = nullptr;
	Microsoft::WRL::ComPtr<ID3D12Resource> resource;
	D3D12_INDEX_BUFFER_VIEW view{};
	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleSRV{};
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleSRV{};
	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleBindlessSRV{};
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleBindlessSRV{};
#endif
};

class VertexBuffer
{
public:
	~VertexBuffer();

	// API-neutral
	int numVertices = 0;
	RHIBufferHandle BindlessHandle{};
	// Live-Spine fast path: for UPLOAD-heap VBs created via
	// CreateUploadVertexBuffer, holds the persistent CPU-mapped pointer +
	// capacity so UpdateUploadVertexBuffer can memcpy in place each frame
	// without going through the staged upload route. Null for non-upload
	// or non-mapped allocations.
	void* MappedCpu = nullptr;
	uint32_t MappedCapacityBytes = 0;

#if CORONA_HAS_D3D12
	DX12Backend* Owner = nullptr;
	Microsoft::WRL::ComPtr<ID3D12Resource> resource;
	D3D12_VERTEX_BUFFER_VIEW view{};
	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleSRV{};
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleSRV{};
	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleBindlessSRV{};
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleBindlessSRV{};
	// Optional UAV view — populated by CreateRWVertexBuffer for compute
	// skinning outputs. Zero-initialized for the read-only paths.
	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleUAV{};
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleUAV{};
#endif
};

class Sampler
{
public:
#if CORONA_HAS_D3D12
	D3D12_SAMPLER_DESC SamplerDesc{};
	Microsoft::WRL::ComPtr<ID3D12Resource> resource;
	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle{};
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle{};
#endif
};

class Texture
{
public:
	~Texture();

	// API-neutral
	uint32_t Width = 0;
	uint32_t Height = 0;
	uint32_t MipLevels = 1;
	ETextureFormat Format = ETextureFormat::RGBA8Unorm;
	ETextureUsageFlags Usage = TextureUsage_None;
	RHITextureHandle BindlessHandle{};

#if CORONA_HAS_D3D12
	DX12Backend* Owner = nullptr;
	D3D12_RESOURCE_DESC textureDesc{};
	Microsoft::WRL::ComPtr<ID3D12Resource> resource;

	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleUAV{};
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleUAV{};
	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleRTV{};
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleRTV{};
	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleDSV{};
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleDSV{};
	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleSRV{};
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleSRV{};
	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleBindlessSRV{};
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleBindlessSRV{};

	void MakeStaticSRV();
	void MakeRTV(bool isBackBuffer = false);
	void MakeDSV();

	void UploadSRCData3D(D3D12_SUBRESOURCE_DATA* SrcData);
#endif
};

class Material
{
public:
	bool bHasAlpha = false;
	glm::vec4 BaseColorFactor = glm::vec4(1.0f);

	std::shared_ptr<Texture> Diffuse;
	std::shared_ptr<Texture> Normal;
	std::shared_ptr<Texture> Roughness;
	std::shared_ptr<Texture> Metallic;

	GraphicsPipelineHandle* CachedGraphicsMaterialPipeline = nullptr;
	Sampler* CachedGraphicsMaterialSampler = nullptr;
	Texture* CachedGraphicsMaterialAlbedo = nullptr;
	Texture* CachedGraphicsMaterialNormal = nullptr;
	Texture* CachedGraphicsMaterialRoughness = nullptr;
	Texture* CachedGraphicsMaterialMetallic = nullptr;
	std::shared_ptr<GraphicsBindGroupHandle> CachedGraphicsMaterialBindGroup;
};

inline std::shared_ptr<GraphicsBindGroupHandle> CreateOrBindGraphicsMaterialBindGroup(
	IRenderBackend* backend,
	GraphicsPipelineHandle* pipeline,
	Material* material,
	Sampler* sampler,
	Texture* albedo,
	Texture* normal,
	Texture* roughness,
	Texture* metallic)
{
	if (!backend || !pipeline)
		return nullptr;

	if (material &&
		material->CachedGraphicsMaterialBindGroup &&
		material->CachedGraphicsMaterialPipeline == pipeline &&
		material->CachedGraphicsMaterialSampler == sampler &&
		material->CachedGraphicsMaterialAlbedo == albedo &&
		material->CachedGraphicsMaterialNormal == normal &&
		material->CachedGraphicsMaterialRoughness == roughness &&
		material->CachedGraphicsMaterialMetallic == metallic)
	{
		backend->BindGraphicsBindGroup(pipeline, kGraphicsBindGroupSlot_Material, material->CachedGraphicsMaterialBindGroup);
		return material->CachedGraphicsMaterialBindGroup;
	}

	GraphicsBindGroupDesc desc{};
	desc.Pipeline = pipeline;
	desc.Slot = kGraphicsBindGroupSlot_Material;
	desc.Entries = {
		GraphicsBindGroupEntry::SamplerBinding("samplerWrap", sampler),
		GraphicsBindGroupEntry::TextureSRV("AlbedoTex", albedo),
		GraphicsBindGroupEntry::TextureSRV("NormalTex", normal),
		GraphicsBindGroupEntry::TextureSRV("RoughnessTex", roughness),
		GraphicsBindGroupEntry::TextureSRV("MetallicTex", metallic),
	};
	std::shared_ptr<GraphicsBindGroupHandle> bindGroup = backend->CreateGraphicsBindGroup(desc);
	backend->BindGraphicsBindGroup(pipeline, kGraphicsBindGroupSlot_Material, bindGroup);

	if (material)
	{
		material->CachedGraphicsMaterialPipeline = pipeline;
		material->CachedGraphicsMaterialSampler = sampler;
		material->CachedGraphicsMaterialAlbedo = albedo;
		material->CachedGraphicsMaterialNormal = normal;
		material->CachedGraphicsMaterialRoughness = roughness;
		material->CachedGraphicsMaterialMetallic = metallic;
		material->CachedGraphicsMaterialBindGroup = bindGroup;
	}
	return bindGroup;
}

class Mesh
{
public:
	Mesh() = default;
	explicit Mesh(IRenderBackend* owner) : Owner(owner) {}

	IRenderBackend* Owner = nullptr;

	struct DrawCall
	{
		std::shared_ptr<Material> mat;
		int32_t DiffuseTextureIndex = -1;
		int32_t NormalTextureIndex = -1;
		int32_t SpecularTextureIndex = -1;
		uint32_t IndexStart = 0;
		uint32_t IndexCount = 0;
		uint32_t VertexBase = 0;
		uint32_t VertexCount = 0;
		uint32_t GBufferMaterialIndex = 0;
	};

	bool bTransparent = false;
	glm::mat4x4 transform = glm::mat4x4(1.0f);
	uint32_t NumIndices = 0;
	uint32_t NumVertices = 0;

	uint32_t VertexStride = 0;
	uint32_t GBufferGeometryIndex = 0;

	EIndexFormat IndexFormat = EIndexFormat::U32;

	std::shared_ptr<IndexBuffer> Ib;
	std::shared_ptr<VertexBuffer> Vb;
	bool bGpuSpineSkinned = false;
	bool bGpuSpineSkinningDispatched = false;
	bool bSpineMesh = false; // marks Spine-sourced meshes for unlit + two-sided rendering regardless of skinning path
	bool bGrassMesh = false; // marks the procedural grass mesh — DrawScene sets the matching CB flag so ApplyVertexDeformations runs grass bend on this mesh only
	// Vertex-pulling procedural grass: VS synthesizes positions from
	// SV_InstanceID + SV_VertexID, no VB/IB needed. Coexists with bGrassMesh
	// (wind/bend still apply); set when the entity was spawned via
	// CreateProceduralGrassOnTerrainSceneInstanced.
	bool bProceduralGrass = false;
	struct ProceduralGrassParams
	{
		uint32_t BladeCount = 0;
		uint32_t BladeSegments = 4;
		float    BladeHeight = 1.0f;
		float    HalfAreaXZ = 512.0f;
		uint32_t Seed = 1u;
		float    HeightMin = 0.0f;
		float    HeightMax = 0.0f;
	};
	ProceduralGrassParams Procedural{};
	bool bTerrainMesh = false; // marks the terrain mesh — kept for potential future terrain-only effects (no longer gates the deform sphere; that's global now)
	bool bExcludeFromDeformSphere = false; // opt-out from the global deform sphere — set on meshes you don't want carved (e.g. the player avatar at the sphere center)
	uint32_t GpuSpineSkinningVertexCount = 0;
	float GpuSpineSkinningSourceScale = 1.0f;
	std::shared_ptr<Buffer> GpuSpineInputVertices;
	std::shared_ptr<Buffer> GpuSpineInfluences;
	std::shared_ptr<Buffer> GpuSpineBones;
	std::shared_ptr<Buffer> GpuSpineSkinnedVertices;

	// 3D skeletal skinning (Corona.Skeletal.*). Fully separate from the Spine
	// fields above; a mesh is never both bSpineMesh and bSkeletalSkinned.
	bool bSkeletalSkinned = false;
	bool bSkeletalSkinningDispatched = false;
	uint32_t SkeletalVertexCount = 0;
	uint32_t SkeletalBoneCount = 0;
	// Slot inside the unified skeletal buffers. The shader uses this to pick
	// the right palette / output range. Stored zero-based.
	uint32_t SkeletalCharIndex = 0;
	std::shared_ptr<Buffer> SkeletalInputVertices;
	std::shared_ptr<Buffer> SkeletalBoneMatrices;
	std::shared_ptr<VertexBuffer> SkeletalOutputVb;
	// Phase 11 (revised): previous-frame bone palette uploaded alongside
	// SkeletalBoneMatrices each frame. The skeletal GBuffer VS re-skins
	// from the bind pose with these matrices to derive an accurate prev
	// clip position for motion vectors — no ping-pong VB, no extra GPU
	// memory bandwidth, and the BLAS source GPU VA stays constant so
	// PERFORM_UPDATE on the BLAS keeps working.
	std::shared_ptr<Buffer> SkeletalPrevBoneMatrices;
	// Leftover from the original ping-pong design; no longer bound or
	// written. Kept for the moment so the spawn path doesn't need to be
	// reworked in the same commit — will be removed once the new motion
	// vector path is validated.
	std::shared_ptr<VertexBuffer> SkeletalOutputVbPrev;
	// Cached BLAS built from SkeletalOutputVb with ALLOW_UPDATE. Refreshed
	// each frame by RefitBLAS after the compute skinning dispatch so RT
	// passes see the current skinned geometry.
	std::shared_ptr<RTAS> SkeletalBlas;

	std::vector<glm::vec3> CpuPositions;
	std::vector<uint32_t> CpuIndices;
	std::vector<std::shared_ptr<Texture>> Textures;

	std::shared_ptr<Material> Mat;

	std::vector<DrawCall> Draws;

	std::shared_ptr<RTAS> CreateBLAS()
	{
		if (!Owner) return nullptr;
		if (bSkeletalSkinned && SkeletalOutputVb)
		{
			if (!SkeletalBlas)
				SkeletalBlas = Owner->CreateBLASForSkeletalMesh(this);
			return SkeletalBlas;
		}
		return Owner->CreateBLASForMesh(this);
	}
};

class Scene
{
public:
	void SetTransform(glm::mat4x4 inTransform);

public:
	std::vector<std::shared_ptr<Mesh>> meshes;
	std::vector<std::shared_ptr<Material>> Materials;
	bool bHasBounds = false;
	glm::vec3 BoundsMin = glm::vec3(0.0f);
	glm::vec3 BoundsMax = glm::vec3(0.0f);
	// Original source asset (FBX/OBJ/etc.) this Scene was loaded from.
	// Used by tooling (Scene Inspector / Asset Explorer) to offer a
	// "Re-import" action that invalidates the on-disk cmesh cache.
	// Empty for procedural meshes (boxes, spheres, grass).
	std::wstring SourceFilePath;

	uint64_t GBufferMaterialRecordHash = 0;
	uint64_t GBufferMaterialDefaultsHash = 0;
	uint64_t GBufferMaterialRecordTopologyHash = 0;
	uint32_t GBufferMaterialRecordMeshCount = 0;
	uint32_t GBufferMaterialRecordDrawCount = 0;
	const IRenderBackend* GBufferMaterialRecordBackend = nullptr;
	bool bGBufferMaterialRecordCacheValid = false;
	std::shared_ptr<Buffer> GBufferMaterialRecordBuffer;
	uint64_t GBufferGeometryRecordHash = 0;
	uint64_t GBufferGeometryRecordTopologyHash = 0;
	uint32_t GBufferGeometryRecordMeshCount = 0;
	uint32_t GBufferGeometryRecordDrawCount = 0;
	const IRenderBackend* GBufferGeometryRecordBackend = nullptr;
	bool bGBufferGeometryRecordCacheValid = false;
	bool bGBufferMaxDrawIndexCountValid = false;
	uint32_t GBufferMaxDrawIndexCount = 0;
	std::shared_ptr<Buffer> GBufferGeometryRecordBuffer;

	struct GBufferResourceBindGroupCacheEntry
	{
		GraphicsPipelineHandle* Pipeline = nullptr;
		Sampler* Sampler = nullptr;
		Buffer* MaterialBuffer = nullptr;
		Buffer* GeometryBuffer = nullptr;
		std::shared_ptr<GraphicsBindGroupHandle> BindGroup;
	};
	std::vector<GBufferResourceBindGroupCacheEntry> CachedGBufferResourceBindGroups;
};
