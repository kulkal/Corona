#pragma once

#include <cfloat>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
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

struct BufferCreateDesc
{
	uint32_t NumElements = 0;
	uint32_t ElementSize = 0;
	EInitialResourceState InitialState = EInitialResourceState::ShaderRead;
	bool bAllowUnorderedAccess = false;
	void* InitialData = nullptr;
	EBufferShape Shape = EBufferShape::ByteAddress;
	bool bUseDefaultHeap = false;
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

struct RTInstanceDesc
{
	std::shared_ptr<RTAS> BottomLevelAS;
	glm::mat4x4 Transform = glm::mat4x4(1.0f);
	float Roughness = 1.0f;
	float Metallic = 0.0f;
	uint32_t bOverrideRoughnessMetallic = 0;
	uint32_t Flags = 0;
};

struct RenderBackendCapabilities
{
	bool SupportsTypedBindingSchema = false;
	bool SupportsBindlessTextures = false;
	bool SupportsBindlessBuffers = false;
	bool SupportsRuntimeDescriptorArrays = false;
	bool SupportsPartiallyBoundDescriptors = false;
	bool SupportsUpdateAfterBind = false;
	uint32_t MaxBindlessTextureCount = 0;
	uint32_t MaxBindlessBufferCount = 0;
};

class IRenderBackend
{
public:
	virtual ~IRenderBackend() = default;

	virtual ERenderBackendAPI GetAPI() const = 0;
	virtual const char* GetBackendName() const = 0;
	virtual RenderBackendCapabilities GetCapabilities() const { return {}; }
	virtual uint32_t GetMaxSupportedHybridStage() const = 0;
	virtual bool SupportsRayTracing() const = 0;
	virtual bool SupportsShaderExecutionReordering() const = 0;
	virtual void BeginFrame() = 0;
	virtual void EndFrame() = 0;
	virtual void WaitForGpu() = 0;
	virtual void EmitGpuCrashMarker(const char* markerName) = 0;
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
	virtual RHITextureHandle GetBindlessTextureHandle(const Texture* texture) const { (void)texture; return {}; }
	virtual uint32_t GetBindlessTextureIndex(const Texture* texture) const
	{
		const RHITextureHandle handle = GetBindlessTextureHandle(texture);
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
	virtual void DrawIndexed(uint32_t indexCount, uint32_t startIndexLocation, int32_t baseVertexLocation) = 0;
	virtual void DrawIndexedInstanced(uint32_t indexCountPerInstance, uint32_t instanceCount, uint32_t startIndexLocation, int32_t baseVertexLocation, uint32_t startInstanceLocation) = 0;
	virtual void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) = 0;
	virtual void ClearTextureUAVFloat(Texture* texture, const float clearColor[4]) = 0;
	virtual void ExecuteCurrentCommandList() = 0;
	virtual void BeginGpuMarker(uint64_t color, const char* label) = 0;
	virtual void EndGpuMarker() = 0;
	virtual void TransitionTexture(Texture* texture, EResourceState stateBefore, EResourceState stateAfter) = 0;
	virtual void TransitionBuffer(Buffer* buffer, EResourceState stateBefore, EResourceState stateAfter) = 0;
	virtual void TransitionVertexBuffer(VertexBuffer* vertexBuffer, EResourceState stateBefore, EResourceState stateAfter) = 0;
	virtual void UAVBarrier(Buffer* buffer) = 0;
	virtual Texture* GetCurrentWindowRenderTarget() = 0;
	virtual void PrepareWindowRenderTarget(Texture* renderTarget) = 0;
	virtual void FinalizeWindowRenderTarget(Texture* renderTarget) = 0;
	virtual void RequestWindowCapture(const std::wstring& outputPath) = 0;
	virtual bool ConsumeWindowCaptureResult(std::wstring* outputPath, bool* success, std::wstring* errorMessage) = 0;
	virtual std::shared_ptr<GraphicsPipelineHandle> CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) = 0;
	virtual void BindGraphicsPipeline(GraphicsPipelineHandle* pipeline) = 0;
	virtual void SetGraphicsPipelineConstantData(GraphicsPipelineHandle* pipeline, uint32_t slot, const void* data, uint32_t size) = 0;
	virtual void BindGraphicsPipelineTexture(GraphicsPipelineHandle* pipeline, const std::string& bindingName, Texture* texture) = 0;
	virtual void BindGraphicsPipelineBuffer(GraphicsPipelineHandle* pipeline, const std::string& bindingName, Buffer* buffer) = 0;
	// Phase 11: bind a VertexBuffer's raw-byte SRV (registered by
	// CreateRWVertexBuffer) to a named SBV slot on a graphics PSO. Used by
	// the skeletal GBuffer VS to sample the previous frame's compute-skinned
	// vertex output for motion vectors.
	virtual void BindGraphicsPipelineVertexBufferSRV(GraphicsPipelineHandle* pipeline, const std::string& bindingName, VertexBuffer* vb) = 0;
	virtual void BindGraphicsPipelineSampler(GraphicsPipelineHandle* pipeline, const std::string& bindingName, Sampler* sampler) = 0;
};

// API-neutral factory. DX12 path requires a pre-created device so the bootstrap
// constructs DX12Backend directly via #include "DX12Backend.h"; this factory
// handles non-DX12 backends and returns nullptr for D3D12.
std::unique_ptr<IRenderBackend> CreateRenderBackend(ERenderBackendAPI api);
