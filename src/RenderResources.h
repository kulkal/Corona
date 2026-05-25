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
	// API-neutral
	enum BufferType { BYTE_ADDRESS, STRUCTURED, UNKNOWN };
	BufferType Type = UNKNOWN;
	uint32_t NumElements = 0;
	uint32_t ElementSize = 0;

#if CORONA_HAS_D3D12
	DX12Backend* Owner = nullptr;
	Microsoft::WRL::ComPtr<ID3D12Resource> resource;
	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleSRV{};
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleSRV{};
	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleUAV{};
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleUAV{};
	// Persistent CPU pointer for UPLOAD-heap buffers — set by
	// CreateUploadStructuredBuffer. Null for DEFAULT-heap buffers.
	void* MappedPtr = nullptr;
	uint32_t MappedSizeInBytes = 0;

	void MakeByteAddressBufferSRV();
	void MakeStructuredBufferSRV();
#endif
};

class IndexBuffer
{
public:
	// API-neutral
	int numIndices = 0;

#if CORONA_HAS_D3D12
	Microsoft::WRL::ComPtr<ID3D12Resource> resource;
	D3D12_INDEX_BUFFER_VIEW view{};
	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleSRV{};
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleSRV{};
#endif
};

class VertexBuffer
{
public:
	// API-neutral
	int numVertices = 0;

#if CORONA_HAS_D3D12
	Microsoft::WRL::ComPtr<ID3D12Resource> resource;
	D3D12_VERTEX_BUFFER_VIEW view{};
	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleSRV{};
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleSRV{};
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
	// API-neutral
	uint32_t Width = 0;
	uint32_t Height = 0;
	uint32_t MipLevels = 1;
	ETextureFormat Format = ETextureFormat::RGBA8Unorm;
	ETextureUsageFlags Usage = TextureUsage_None;

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
};

class Mesh
{
public:
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
	};

	bool bTransparent = false;
	glm::mat4x4 transform = glm::mat4x4(1.0f);
	uint32_t NumIndices = 0;
	uint32_t NumVertices = 0;

	uint32_t VertexStride = 0;

	EIndexFormat IndexFormat = EIndexFormat::U32;

	std::shared_ptr<IndexBuffer> Ib;
	std::shared_ptr<VertexBuffer> Vb;
	bool bGpuSpineSkinned = false;
	bool bGpuSpineSkinningDispatched = false;
	bool bSpineMesh = false; // marks Spine-sourced meshes for unlit + two-sided rendering regardless of skinning path
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
	std::shared_ptr<Buffer> SkeletalInputVertices;
	std::shared_ptr<Buffer> SkeletalBoneMatrices;
	std::shared_ptr<VertexBuffer> SkeletalOutputVb;
	// Phase 11: companion ping-pong VB that held the previous frame's
	// skinning output. The swap had to be disabled because it changed the
	// BLAS source GPU VA each frame and broke D3D12 PERFORM_UPDATE on the
	// BLAS (the BLAS Result then tracked the *other* buffer, so shadow rays
	// missed the character). The buffer is still allocated so the spawn
	// path stays consistent, but it is no longer bound or written — the
	// upcoming motion-vector rework will replace it with a previous-frame
	// bone-matrix SBV (re-skin from bind in the VS).
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
};
