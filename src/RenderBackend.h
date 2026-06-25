#pragma once

#include <cfloat>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "glm/mat4x4.hpp"
#include "glm/vec4.hpp"
#include "ComputePipelineStateObject.h"
#include "RHIBinding.h"
#include "RTAS.h"
#include "RTPipelineStateObject.h"

struct ImDrawData;

class DX12Backend;
class Texture;
class Buffer;
class Sampler;
class VertexBuffer;
class IndexBuffer;
class Mesh;
class GraphicsPipelineHandle;

enum class ERenderBackendAPI
{
	D3D12,
	Vulkan,
	// Experimental: a single backend over NVIDIA NRI that itself targets D3D12
	// or Vulkan. Selected only when built with CORONA_WITH_NRI (CORONA_HAS_NRI).
	NRI,
};

enum class ETextureFormat
{
	RGBA16Float,
	RGBA32Float,
	RG16Float,
	RGBA8Unorm,
	BGRA8Unorm,
	D32Float,
	R32Float,
	R8Uint,
};

enum class EVertexAttributeFormat
{
	Float2,
	Float3,
	Float4,
};

enum class EIndexFormat
{
	U16,
	U32,
};

struct ShaderBytecode
{
	std::vector<uint8_t> Data;
	bool IsValid() const { return !Data.empty(); }
	const void* GetPointer() const { return Data.data(); }
	size_t GetSize() const { return Data.size(); }
};

// Opaque native window handle. PlatformHandle stores an HWND on Windows, an
// ANativeWindow* on Android, a CAMetalLayer* on iOS (MoltenVK), etc. Backends
// interpret it based on their platform; the abstract interface does not.
struct WindowHandle
{
	void* PlatformHandle = nullptr;
};

// Raw 2D image payload produced by IRenderBackend::CaptureTexture. The data is
// expected to be the first mip of the first array slice, laid out row-by-row.
struct CapturedImage
{
	ETextureFormat Format = ETextureFormat::RGBA8Unorm;
	uint32_t Width = 0;
	uint32_t Height = 0;
	uint32_t RowPitch = 0;
	std::vector<uint8_t> Pixels;

	bool IsEmpty() const { return Pixels.empty(); }
};

enum class EInitialResourceState
{
	ShaderRead,
	GenericRead,
	CopyDest,
};

enum class EResourceState
{
	ShaderRead,
	RenderTarget,
	UnorderedAccess,
	Present,
	DepthWrite,
	CopyDest,
	CopySource,
	VertexBuffer,
	IndirectArgument,
};

enum ETextureUsageFlags : uint32_t
{
	TextureUsage_None = 0,
	TextureUsage_RenderTarget = 1 << 0,
	TextureUsage_UnorderedAccess = 1 << 1,
	TextureUsage_DepthStencil = 1 << 2,
};

inline ETextureUsageFlags operator|(ETextureUsageFlags lhs, ETextureUsageFlags rhs)
{
	return static_cast<ETextureUsageFlags>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}

inline bool HasTextureUsage(ETextureUsageFlags flags, ETextureUsageFlags value)
{
	return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(value)) != 0;
}

struct TextureCreateDesc
{
	ETextureFormat Format = ETextureFormat::RGBA16Float;
	ETextureUsageFlags Usage = TextureUsage_None;
	EInitialResourceState InitialState = EInitialResourceState::ShaderRead;
	int Width = 1;
	int Height = 1;
	int MipLevels = 1;
	std::optional<glm::vec4> ClearColor = std::nullopt;
};

enum class EBufferShape
{
	// Default: SRV created as a byte-address buffer (raw load4 access in HLSL).
	ByteAddress,
	// Structured buffer SRV (typed structured load in HLSL).
	Structured,
};

enum class EBufferAccess
{
	// GPU-resident default. InitialData is uploaded through staging.
	GpuOnly,
	// CPU-visible upload memory. Shader-visible use must opt in explicitly.
	Upload,
	// CPU-written, GPU-read streaming data. Backends may use an upload ring/pool.
	Stream,
	// CPU-readable transfer destination.
	Readback,
};

enum class EBufferLifetime
{
	Persistent,
	PerFrame,
};

enum class EBufferAllocationPolicy
{
	Dedicated,
	Suballocated,
};

struct BufferCreateDesc
{
	uint32_t NumElements = 0;
	uint32_t ElementSize = 0;
	EInitialResourceState InitialState = EInitialResourceState::ShaderRead;
	bool bAllowUnorderedAccess = false;
	void* InitialData = nullptr;
	EBufferShape Shape = EBufferShape::ByteAddress;
	EBufferAccess Access = EBufferAccess::GpuOnly;
	EBufferLifetime Lifetime = EBufferLifetime::Persistent;
	EBufferAllocationPolicy AllocationPolicy = EBufferAllocationPolicy::Dedicated;
};

struct DrawIndexedIndirectArguments
{
	uint32_t IndexCountPerInstance = 0;
	uint32_t InstanceCount = 0;
	uint32_t StartIndexLocation = 0;
	int32_t BaseVertexLocation = 0;
	uint32_t StartInstanceLocation = 0;
};

struct DrawIndirectArguments
{
	uint32_t VertexCountPerInstance = 0;
	uint32_t InstanceCount = 0;
	uint32_t StartVertexLocation = 0;
	uint32_t StartInstanceLocation = 0;
};

enum class ESamplerFilter
{
	Anisotropic,
	Linear,
};

enum class ESamplerAddressMode
{
	Wrap,
	Clamp,
};

struct SamplerCreateDesc
{
	ESamplerFilter Filter = ESamplerFilter::Linear;
	ESamplerAddressMode AddressU = ESamplerAddressMode::Wrap;
	ESamplerAddressMode AddressV = ESamplerAddressMode::Wrap;
	ESamplerAddressMode AddressW = ESamplerAddressMode::Wrap;
	float MinLOD = 0.0f;
	float MaxLOD = FLT_MAX;
	float MipLODBias = 0.0f;
	uint32_t MaxAnisotropy = 1;
};

struct GraphicsVertexElementDesc
{
	std::string SemanticName;
	uint32_t SemanticIndex = 0;
	EVertexAttributeFormat Format = EVertexAttributeFormat::Float4;
	uint32_t Offset = 0;
};

struct GraphicsTextureBindingDesc
{
	std::string Name;
	uint32_t Slot = 0;
};

struct GraphicsBufferBindingDesc
{
	std::string Name;
	uint32_t Slot = 0;
};

struct GraphicsSamplerBindingDesc
{
	std::string Name;
	uint32_t Slot = 0;
};

// Blend mode for a graphics PSO. Default Opaque preserves prior behavior:
// every existing pipeline was opaque-only and didn't carry a blend field.
// Additive / AlphaBlend write the blended result to RT 0 only (RT >= 1 stays
// fully masked off) so PSOs that target multi-RT GBuffer can sit beside the
// opaque mesh path without scribbling into Normal/Velocity/Roughness.
enum class EBlendMode : uint8_t
{
	Opaque,
	Additive,
	AlphaBlend,
};

enum class EDepthCompareOp : uint8_t
{
	BackendDefault,
	Less,
	LessEqual,
	Equal,
	Always,
};

struct GraphicsPipelineDesc
{
	std::wstring ShaderPath;
	std::string VertexEntryPoint;
	std::string PixelEntryPoint;
	RHIPipelineLayoutDesc PipelineLayout;
	std::vector<GraphicsVertexElementDesc> VertexElements;
	std::vector<GraphicsTextureBindingDesc> TextureBindings;
	std::vector<GraphicsBufferBindingDesc> BufferBindings;
	std::vector<GraphicsSamplerBindingDesc> SamplerBindings;
	uint32_t VertexStride = 0;
	std::vector<ETextureFormat> ColorFormats = { ETextureFormat::RGBA8Unorm };
	std::optional<ETextureFormat> DepthFormat;
	bool bDepthEnable = false;
	bool bDepthWriteEnable = true;
	EDepthCompareOp DepthCompareOp = EDepthCompareOp::BackendDefault;
	bool bCullBackFaces = true;
	bool bTriangleStrip = false;
	bool bDepthBiasEnable = false;
	float DepthBiasConstantFactor = 0.0f;
	float DepthBiasClamp = 0.0f;
	float DepthBiasSlopeFactor = 0.0f;
	uint32_t ConstantBufferSize = 0;
	uint32_t ConstantBufferBinding = 0;
	EBlendMode BlendMode = EBlendMode::Opaque;
};

class GraphicsPipelineHandle
{
public:
	virtual ~GraphicsPipelineHandle() = default;
};

enum class EGraphicsBindGroupEntryType : uint8_t
{
	TextureSRV,
	BufferSRV,
	VertexBufferSRV,
	Sampler,
	ConstantData,
};

struct GraphicsBindGroupEntry
{
	EGraphicsBindGroupEntryType Type = EGraphicsBindGroupEntryType::TextureSRV;
	std::string BindingName;
	uint32_t Slot = 0;
	Texture* TextureValue = nullptr;
	Buffer* BufferValue = nullptr;
	VertexBuffer* VertexBufferValue = nullptr;
	Sampler* SamplerValue = nullptr;
	const void* ConstantData = nullptr;
	uint32_t ConstantDataSize = 0;

	static GraphicsBindGroupEntry TextureSRV(std::string bindingName, Texture* texture)
	{
		GraphicsBindGroupEntry entry{};
		entry.Type = EGraphicsBindGroupEntryType::TextureSRV;
		entry.BindingName = std::move(bindingName);
		entry.TextureValue = texture;
		return entry;
	}

	static GraphicsBindGroupEntry BufferSRV(std::string bindingName, Buffer* buffer)
	{
		GraphicsBindGroupEntry entry{};
		entry.Type = EGraphicsBindGroupEntryType::BufferSRV;
		entry.BindingName = std::move(bindingName);
		entry.BufferValue = buffer;
		return entry;
	}

	static GraphicsBindGroupEntry VertexBufferSRV(std::string bindingName, VertexBuffer* vertexBuffer)
	{
		GraphicsBindGroupEntry entry{};
		entry.Type = EGraphicsBindGroupEntryType::VertexBufferSRV;
		entry.BindingName = std::move(bindingName);
		entry.VertexBufferValue = vertexBuffer;
		return entry;
	}

	static GraphicsBindGroupEntry SamplerBinding(std::string bindingName, Sampler* sampler)
	{
		GraphicsBindGroupEntry entry{};
		entry.Type = EGraphicsBindGroupEntryType::Sampler;
		entry.BindingName = std::move(bindingName);
		entry.SamplerValue = sampler;
		return entry;
	}

	static GraphicsBindGroupEntry Constant(uint32_t slot, const void* data, uint32_t size)
	{
		GraphicsBindGroupEntry entry{};
		entry.Type = EGraphicsBindGroupEntryType::ConstantData;
		entry.Slot = slot;
		entry.ConstantData = data;
		entry.ConstantDataSize = size;
		return entry;
	}
};

struct GraphicsBindGroupDesc
{
	GraphicsPipelineHandle* Pipeline = nullptr;
	uint32_t Slot = 0;
	std::vector<GraphicsBindGroupEntry> Entries;
};

inline constexpr uint32_t kMaxGraphicsBindGroupSlots = 4;
inline constexpr uint32_t kGraphicsBindGroupSlot_All = 0;
inline constexpr uint32_t kGraphicsBindGroupSlot_Frame = 0;
inline constexpr uint32_t kGraphicsBindGroupSlot_Material = 1;
inline constexpr uint32_t kGraphicsBindGroupSlot_Draw = 2;

class GraphicsBindGroupHandle
{
public:
	virtual ~GraphicsBindGroupHandle() = default;
};

inline constexpr uint32_t kRTRayMaskAll = 0xFFu;
inline constexpr uint32_t kRTRayMaskShadow = 0x02u;

struct RTInstanceDesc
{
	std::shared_ptr<RTAS> BottomLevelAS;
	glm::mat4x4 Transform = glm::mat4x4(1.0f);
	float Roughness = 1.0f;
	float Metallic = 0.0f;
	uint32_t bOverrideRoughnessMetallic = 0;
	uint32_t Flags = 0;
	uint32_t SceneObjectIndex = UINT32_MAX;
	uint32_t InstanceMask = kRTRayMaskAll;
};

struct RenderBackendCapabilities
{
	bool SupportsTypedBindingSchema = false;
	bool SupportsBindlessTextures = false;
	bool SupportsBindlessBuffers = false;
	bool SupportsRuntimeDescriptorArrays = false;
	bool SupportsPartiallyBoundDescriptors = false;
	bool SupportsUpdateAfterBind = false;
	bool SupportsDrawIndexedIndirect = false;
	bool SupportsDrawIndirect = false;
	bool SupportsDrawIndirectCount = false;
	bool SupportsMultiDrawIndirect = false;
	bool SupportsDrawIndirectFirstInstance = false;
	bool SupportsGBufferOcclusionQueries = false;
	bool RequiresStartupLoadingScreenGBufferFallback = false;
	bool RequiresFullPrecisionHybridUAVTargets = false;
	bool UsesWindowFramebufferCache = false;
	uint32_t MaxBindlessTextureCount = 0;
	uint32_t MaxBindlessBufferCount = 0;
};

struct RenderBackendAllocatorStats
{
	uint32_t PersistentStructuredBlockCount = 0;
	uint64_t PersistentStructuredReservedBytes = 0;
	uint64_t PersistentStructuredCommittedBytes = 0;
	uint64_t PersistentStructuredReusableBytes = 0;
	uint64_t PersistentStructuredPendingFreeBytes = 0;
	uint64_t PersistentStructuredPendingUploadBytes = 0;
	uint64_t PersistentStructuredBytesIssued = 0;

	uint32_t TransientStructuredBlockCount = 0;
	uint64_t TransientStructuredReservedBytes = 0;
	uint64_t TransientStructuredCurrentFrameBytes = 0;
	uint64_t TransientStructuredBytesIssued = 0;

	uint32_t BindlessTextureSlotsUsed = 0;
	uint32_t BindlessTextureSlotsCapacity = 0;
	uint32_t BindlessBufferSlotsUsed = 0;
	uint32_t BindlessBufferSlotsCapacity = 0;
};

struct StreamlineTextureResourceDesc
{
	void* Native = nullptr;
	void* Memory = nullptr;
	void* View = nullptr;
	uint32_t State = (std::numeric_limits<uint32_t>::max)();
	uint32_t Width = 0;
	uint32_t Height = 0;
	uint32_t NativeFormat = 0;
	uint32_t MipLevels = 1;
	uint32_t ArrayLayers = 1;
	uint32_t Flags = 0;
	uint32_t Usage = 0;
};

struct StreamlineVulkanDeviceInfo
{
	void* Device = nullptr;
	void* Instance = nullptr;
	void* PhysicalDevice = nullptr;
	std::array<uint8_t, 8> DeviceLUID{};
	uint32_t DeviceLUIDSizeInBytes = 0;
	uint32_t ComputeQueueIndex = 0;
	uint32_t ComputeQueueFamily = 0;
	uint32_t GraphicsQueueIndex = 0;
	uint32_t GraphicsQueueFamily = 0;
	uint32_t OpticalFlowQueueIndex = 0;
	uint32_t OpticalFlowQueueFamily = 0;
	uint32_t ComputeQueueCreateFlags = 0;
	uint32_t GraphicsQueueCreateFlags = 0;
	uint32_t OpticalFlowQueueCreateFlags = 0;
};

class IRenderBackend
{
public:
	virtual ~IRenderBackend() = default;

	virtual ERenderBackendAPI GetAPI() const = 0;
	virtual const char* GetBackendName() const = 0;
	virtual RenderBackendCapabilities GetCapabilities() const { return {}; }
	virtual RenderBackendAllocatorStats GetAllocatorStats() const { return {}; }
	virtual bool GetStreamlineTextureResource(Texture* texture, EResourceState state, StreamlineTextureResourceDesc& outDesc) const
	{
		(void)texture;
		(void)state;
		(void)outDesc;
		return false;
	}
	virtual void* GetStreamlineCommandBuffer() { return nullptr; }
	virtual void NotifyExternalCommandListStateChanged() {}
	// Native graphics device for Streamline (slSetD3DDevice / adapter LUID query).
	// DX12 owns its device in Corona directly; the NRI backend creates its own, so
	// it exposes the native ID3D12Device* here. Returns null when not applicable.
	virtual void* GetStreamlineNativeDevice() const { return nullptr; }
	virtual bool GetStreamlineVulkanDeviceInfo(StreamlineVulkanDeviceInfo& outInfo) const
	{
		(void)outInfo;
		return false;
	}
	virtual uint32_t GetMaxSupportedHybridStage() const = 0;
	virtual bool SupportsRayTracing() const = 0;
	virtual bool SupportsShaderExecutionReordering() const = 0;
	virtual bool UsesSimpleGIFallbackPath() const { return false; }
	virtual void BeginFrame() = 0;
	virtual void EndFrame() = 0;
	virtual void WaitForGpu() = 0;
	virtual bool SupportsAsyncRtOverlap() const { return false; }
	virtual bool BeginAsyncRtRecordingAfterGraphicsSubmit() { return false; }
	virtual uint64_t EndAsyncRtRecordingAndResumeGraphics() { return 0; }
	virtual bool HasPendingAsyncRtWork() const { return false; }
	virtual void SubmitGraphicsWorkAndWaitForAsyncRt() {}
	virtual void EmitGpuCrashMarker(const char* markerName) = 0;
	virtual bool IsDeviceLost() const { return false; }
	virtual const std::string& GetErrorString() const = 0;
	virtual void ClearErrorString() = 0;
	virtual uint64_t GetTimestampFrequency() const = 0;
	virtual uint32_t GetFrameCount() const = 0;
	virtual uint32_t GetCurrentFrameIndex() const = 0;
	virtual DX12Backend* AsDX12Backend() = 0;
	virtual std::shared_ptr<Texture> CreateTexture2D(const TextureCreateDesc& desc) = 0;
	virtual std::shared_ptr<Buffer> CreateBuffer(const BufferCreateDesc& desc) = 0;
	virtual std::shared_ptr<Sampler> CreateSampler(const SamplerCreateDesc& desc) = 0;
	virtual std::shared_ptr<Texture> CreateTextureFromFile(const std::wstring& fileName, bool nonSRGB) = 0;
	virtual RHITextureHandle RegisterBindlessTexture(Texture* texture) { (void)texture; return {}; }
	virtual bool UpdateBindlessTexture(Texture* texture) { (void)texture; return false; }
	virtual void UnregisterBindlessTexture(Texture* texture) { (void)texture; }
	virtual RHITextureHandle GetBindlessTextureHandle(const Texture* texture) const { (void)texture; return {}; }
	virtual uint32_t GetBindlessTextureIndex(const Texture* texture) const
	{
		const RHITextureHandle handle = GetBindlessTextureHandle(texture);
		return handle.IsValid() ? handle.Index : RHI_INVALID_BINDLESS_INDEX;
	}
	virtual RHIBufferHandle RegisterBindlessBuffer(Buffer* buffer) { (void)buffer; return {}; }
	virtual RHIBufferHandle RegisterBindlessVertexBuffer(VertexBuffer* buffer) { (void)buffer; return {}; }
	virtual RHIBufferHandle RegisterBindlessIndexBuffer(IndexBuffer* buffer) { (void)buffer; return {}; }
	virtual void UnregisterBindlessBuffer(Buffer* buffer) { (void)buffer; }
	virtual void UnregisterBindlessVertexBuffer(VertexBuffer* buffer) { (void)buffer; }
	virtual void UnregisterBindlessIndexBuffer(IndexBuffer* buffer) { (void)buffer; }
	virtual RHIBufferHandle GetBindlessBufferHandle(const Buffer* buffer) const { (void)buffer; return {}; }
	virtual RHIBufferHandle GetBindlessVertexBufferHandle(const VertexBuffer* buffer) const { (void)buffer; return {}; }
	virtual RHIBufferHandle GetBindlessIndexBufferHandle(const IndexBuffer* buffer) const { (void)buffer; return {}; }
	virtual uint32_t GetBindlessBufferIndex(const Buffer* buffer) const
	{
		const RHIBufferHandle handle = GetBindlessBufferHandle(buffer);
		return handle.IsValid() ? handle.Index : RHI_INVALID_BINDLESS_INDEX;
	}
	virtual uint32_t GetBindlessVertexBufferIndex(const VertexBuffer* buffer) const
	{
		const RHIBufferHandle handle = GetBindlessVertexBufferHandle(buffer);
		return handle.IsValid() ? handle.Index : RHI_INVALID_BINDLESS_INDEX;
	}
	virtual uint32_t GetBindlessIndexBufferIndex(const IndexBuffer* buffer) const
	{
		const RHIBufferHandle handle = GetBindlessIndexBufferHandle(buffer);
		return handle.IsValid() ? handle.Index : RHI_INVALID_BINDLESS_INDEX;
	}
	virtual std::shared_ptr<Texture> CreateTexture3D(
		ETextureFormat format,
		ETextureUsageFlags usage,
		EInitialResourceState initialState,
		int width,
		int height,
		int depth,
		int mipLevels) = 0;
	virtual void UploadTexture3D(Texture* texture, const void* data, uint64_t rowPitch, uint64_t slicePitch) = 0;
	virtual std::shared_ptr<VertexBuffer> CreateVertexBuffer(uint32_t size, uint32_t stride, void* srcData) = 0;
	virtual std::shared_ptr<IndexBuffer> CreateIndexBuffer(EIndexFormat format, uint32_t size, void* srcData) = 0;
	// Cheap creation path for short-lived, frequently-changing vertex/index
	// data: allocate in an UPLOAD heap (DX12) / HOST_VISIBLE memory
	// (Vulkan) and write through a CPU mapping. No staging copy, no
	// ExecuteCommandList, no WaitGPU on creation. Returned buffers are
	// immediately usable for drawing — the GPU reads through PCIe (DX12)
	// or host-coherent memory (Vulkan), which is fine for sprite-scale or
	// per-frame geometry. Anything driving many small CreateVertexBuffer /
	// CreateIndexBuffer calls during gameplay (Spine sprites, dynamic
	// debug meshes, immediate-mode UI, etc.) should prefer these.
	virtual std::shared_ptr<VertexBuffer> CreateUploadVertexBuffer(uint32_t size, uint32_t stride, const void* srcData) = 0;
	virtual std::shared_ptr<IndexBuffer> CreateUploadIndexBuffer(EIndexFormat format, uint32_t size, const void* srcData) = 0;
	// In-place refresh of an UPLOAD-heap VB/IB created above. Used by live
	// Spine instances that regenerate geometry every frame without
	// allocating new GPU buffers. `sizeInBytes` must fit within the
	// buffer's original capacity.
	virtual void UpdateUploadVertexBuffer(VertexBuffer* buffer, const void* srcData, uint32_t sizeInBytes) = 0;
	virtual void UpdateUploadIndexBuffer(IndexBuffer* buffer, const void* srcData, uint32_t sizeInBytes) = 0;
	// GPU-writeable vertex buffer for skeletal skinning output. Allocated in
	// DEFAULT heap with ALLOW_UNORDERED_ACCESS. The returned VertexBuffer
	// can be bound as a UAV through any ComputePipelineStateObject and then
	// transitioned to VertexBuffer state for IA fetch.
	virtual std::shared_ptr<VertexBuffer> CreateRWVertexBuffer(uint32_t size, uint32_t stride) = 0;
	// Persistent-mapped UPLOAD-heap structured buffer for dynamic SBV data
	// (e.g. per-character bone matrices). Reuse the same buffer across
	// frames; call UpdateUploadStructuredBuffer to refresh contents.
	virtual std::shared_ptr<Buffer> CreateUploadStructuredBuffer(uint32_t numElements, uint32_t elementSize) = 0;
	virtual void UpdateUploadStructuredBuffer(Buffer* buffer, const void* srcData, uint32_t sizeInBytes) = 0;
	virtual bool UpdateDefaultStructuredBuffer(Buffer* buffer, const void* srcData, uint32_t sizeInBytes)
	{
		(void)buffer;
		(void)srcData;
		(void)sizeInBytes;
		return false;
	}
	virtual bool CreateOrUpdateRayTracingInstancePropertyBuffer(
		std::shared_ptr<Buffer>& buffer,
		uint32_t numElements,
		uint32_t elementSize,
		const void* srcData,
		uint32_t sizeInBytes,
		std::wstring* outFailureReason) = 0;
	// Frame-transient structured buffer backed by a backend-owned suballocated
	// upload pool. The returned Buffer handle is kept alive by the backend
	// until the frame's fence is retired, so callers can bind it immediately
	// without maintaining their own per-draw cache.
	virtual std::shared_ptr<Buffer> AllocateTransientUploadStructuredBuffer(uint32_t numElements, uint32_t elementSize, const void* srcData) = 0;
	// Like AllocateTransientUploadStructuredBuffer, but the GPU reads the data
	// from device-local (VRAM) memory instead of a host-visible/sysmem heap.
	// Costs one staging copy + barrier per frame, but keeps heavy per-vertex/
	// per-pixel SRV reads off the SysL2/sysmem aperture (which contends with the
	// host/PCIe path when the CPU is memory-busy). Default forwards to the upload
	// path; backends that can do better override it.
	virtual std::shared_ptr<Buffer> AllocateTransientDefaultStructuredBuffer(uint32_t numElements, uint32_t elementSize, const void* srcData)
	{
		return AllocateTransientUploadStructuredBuffer(numElements, elementSize, srcData);
	}
	virtual std::shared_ptr<RTAS> CreateBLASForMesh(Mesh* mesh) = 0;
	// Build a BLAS from the skeletal-skinning output VB (SkeletalOutputVb)
	// with the ALLOW_UPDATE flag so RefitBLAS can refresh it cheaply each
	// frame. Returns nullptr for meshes without skeletal data.
	virtual std::shared_ptr<RTAS> CreateBLASForSkeletalMesh(Mesh* mesh) = 0;
	// In-place refresh of a BLAS built with ALLOW_UPDATE. The acceleration
	// structure resource address is unchanged; existing TLAS references stay
	// valid.
	virtual void RefitBLAS(RTAS* rtas, Mesh* mesh) = 0;
	virtual std::shared_ptr<RTAS> CreateTLAS(const std::vector<RTInstanceDesc>& instances) = 0;
	virtual bool UpdateTLAS(const std::shared_ptr<RTAS>& topLevelAS, const std::vector<RTInstanceDesc>& instances) = 0;
	virtual std::shared_ptr<RTPipelineStateObject> CreateRTPipelineStateObject() = 0;
	virtual std::shared_ptr<ComputePipelineStateObject> CreateComputePipelineStateObject() = 0;
	virtual ShaderBytecode CreateShader(const std::wstring& fileName, const std::string& entryPoint, const std::string& target) = 0;
	virtual void ResetDynamicResources() = 0;
	virtual void ForgetDynamicTexture(Texture* texture) { (void)texture; }
	virtual void CreateSwapChainForWindow(
		WindowHandle window,
		uint32_t width,
		uint32_t height,
		ETextureFormat format) = 0;
	virtual std::shared_ptr<Texture> GetSwapChainTexture(uint32_t bufferIndex) = 0;
	virtual bool CaptureTexture(Texture* source, CapturedImage& captured, EResourceState beforeState) = 0;
	virtual void InitializeImGuiBackend(WindowHandle window, ETextureFormat rtvFormat) = 0;
	virtual void NewImGuiFrame() = 0;
	virtual void RenderImGuiDrawData(ImDrawData* drawData) = 0;
	virtual void ShutdownImGuiBackend() = 0;
	virtual void InitializeGpuTimestampQueries(uint32_t queryCount) = 0;
	virtual void ShutdownGpuTimestampQueries() = 0;
	virtual void WriteGpuTimestamp(uint32_t queryIndex) = 0;
	virtual void ResolveGpuTimestampRange(uint32_t startQueryIndex, uint32_t queryCount) = 0;
	virtual uint64_t ReadGpuTimestampValue(uint32_t queryIndex) const = 0;
	virtual void InitializeOcclusionQueries(uint32_t queryCount) = 0;
	virtual void ShutdownOcclusionQueries() = 0;
	virtual void BeginOcclusionQuery(uint32_t queryIndex) = 0;
	virtual void EndOcclusionQuery(uint32_t queryIndex) = 0;
	virtual void ResolveOcclusionQueryRange(uint32_t startQueryIndex, uint32_t queryCount) = 0;
	virtual uint64_t ReadOcclusionQueryValue(uint32_t queryIndex) const = 0;
	virtual void SetRenderTarget(Texture* colorTarget, Texture* depthTarget = nullptr) = 0;
	virtual void SetRenderTargets(Texture* const* colorTargets, uint32_t colorTargetCount, Texture* depthTarget = nullptr) = 0;
	virtual void ClearRenderTarget(Texture* colorTarget, const float clearColor[4]) = 0;
	virtual void ClearDepth(Texture* depthTarget, float depthValue) = 0;
	virtual void BindDefaultDescriptorHeaps() = 0;
	virtual void SetViewportAndScissor(uint32_t width, uint32_t height) = 0;
	virtual void DrawFullscreenQuad(VertexBuffer* vertexBuffer) = 0;
	virtual void BindMeshBuffers(VertexBuffer* vertexBuffer, IndexBuffer* indexBuffer) = 0;
	virtual void DrawInstanced(uint32_t vertexCountPerInstance, uint32_t instanceCount, uint32_t startVertexLocation, uint32_t startInstanceLocation) = 0;
	virtual void DrawIndexed(uint32_t indexCount, uint32_t startIndexLocation, int32_t baseVertexLocation) = 0;
	virtual void DrawIndexedInstanced(uint32_t indexCountPerInstance, uint32_t instanceCount, uint32_t startIndexLocation, int32_t baseVertexLocation, uint32_t startInstanceLocation) = 0;
	virtual bool DrawIndirect(Buffer* indirectArgumentBuffer, uint64_t byteOffset, uint32_t drawCount) = 0;
	virtual bool DrawIndirectCount(Buffer* indirectArgumentBuffer, uint64_t byteOffset, Buffer* countBuffer, uint64_t countByteOffset, uint32_t maxDrawCount)
	{
		(void)indirectArgumentBuffer;
		(void)byteOffset;
		(void)countBuffer;
		(void)countByteOffset;
		(void)maxDrawCount;
		return false;
	}
	virtual bool DrawIndexedIndirect(Buffer* indirectArgumentBuffer, uint64_t byteOffset, uint32_t drawCount) = 0;
	virtual void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) = 0;
	virtual void ClearTextureUAVFloat(Texture* texture, const float clearColor[4]) = 0;
	virtual void CopyTexture(Texture* dstTexture, Texture* srcTexture) = 0;
	virtual void ExecuteCurrentCommandList() = 0;
	virtual void BeginGpuMarker(uint64_t color, const char* label) = 0;
	virtual void EndGpuMarker() = 0;
	virtual void TransitionTexture(Texture* texture, EResourceState stateBefore, EResourceState stateAfter) = 0;
	virtual void TransitionBuffer(Buffer* buffer, EResourceState stateBefore, EResourceState stateAfter) = 0;
	virtual void TransitionVertexBuffer(VertexBuffer* vertexBuffer, EResourceState stateBefore, EResourceState stateAfter) = 0;
	virtual void UAVBarrier(Texture* texture) { (void)texture; }
	virtual void UAVBarrier(Buffer* buffer) = 0;
	virtual Texture* GetCurrentWindowRenderTarget() = 0;
	virtual void PrepareWindowRenderTarget(Texture* renderTarget) = 0;
	virtual void FinalizeWindowRenderTarget(Texture* renderTarget) = 0;
	virtual void RequestWindowCapture(const std::wstring& outputPath) = 0;
	virtual bool ConsumeWindowCaptureResult(std::wstring* outputPath, bool* success, std::wstring* errorMessage) = 0;
	virtual std::shared_ptr<GraphicsPipelineHandle> CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) = 0;
	virtual std::shared_ptr<GraphicsBindGroupHandle> CreateGraphicsBindGroup(const GraphicsBindGroupDesc& desc) = 0;
	virtual void BindGraphicsPipeline(GraphicsPipelineHandle* pipeline) = 0;
	virtual void BindGraphicsBindGroup(GraphicsPipelineHandle* pipeline, uint32_t slot, const std::shared_ptr<GraphicsBindGroupHandle>& bindGroup) = 0;
};

inline std::shared_ptr<GraphicsBindGroupHandle> CreateAndBindGraphicsBindGroup(
	IRenderBackend* backend,
	GraphicsPipelineHandle* pipeline,
	uint32_t slot,
	const std::vector<GraphicsBindGroupEntry>& entries)
{
	if (!backend || !pipeline)
		return nullptr;

	GraphicsBindGroupDesc desc{};
	desc.Pipeline = pipeline;
	desc.Slot = slot;
	desc.Entries = entries;
	auto bindGroup = backend->CreateGraphicsBindGroup(desc);
	backend->BindGraphicsBindGroup(pipeline, slot, bindGroup);
	return bindGroup;
}

inline std::shared_ptr<GraphicsBindGroupHandle> CreateAndBindGraphicsBindGroup(
	IRenderBackend* backend,
	GraphicsPipelineHandle* pipeline,
	const std::vector<GraphicsBindGroupEntry>& entries)
{
	return CreateAndBindGraphicsBindGroup(backend, pipeline, kGraphicsBindGroupSlot_All, entries);
}

inline std::shared_ptr<GraphicsBindGroupHandle> CreateAndBindGraphicsBindGroup(
	IRenderBackend* backend,
	GraphicsPipelineHandle* pipeline,
	uint32_t slot,
	std::initializer_list<GraphicsBindGroupEntry> entries)
{
	return CreateAndBindGraphicsBindGroup(
		backend,
		pipeline,
		slot,
		std::vector<GraphicsBindGroupEntry>(entries.begin(), entries.end()));
}

inline std::shared_ptr<GraphicsBindGroupHandle> CreateAndBindGraphicsBindGroup(
	IRenderBackend* backend,
	GraphicsPipelineHandle* pipeline,
	std::initializer_list<GraphicsBindGroupEntry> entries)
{
	return CreateAndBindGraphicsBindGroup(backend, pipeline, kGraphicsBindGroupSlot_All, entries);
}

// API-neutral factory. DX12 path requires a pre-created device so the bootstrap
// constructs DX12Backend directly via #include "DX12Backend.h"; this factory
// handles non-DX12 backends and returns nullptr for D3D12.
std::unique_ptr<IRenderBackend> CreateRenderBackend(ERenderBackendAPI api);
