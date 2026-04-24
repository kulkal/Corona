#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include "glm/vec4.hpp"

namespace DirectX
{
	class ScratchImage;
}

struct ImDrawData;

class SimpleDX12;
class Texture;
class Buffer;
class Sampler;
class VertexBuffer;
class IndexBuffer;
class RTAS;

enum class ERenderBackendAPI
{
	D3D12,
};

enum class ETextureFormat
{
	RGBA16Float,
	RGBA32Float,
	RG16Float,
	RGBA8Unorm,
	D32Float,
	R32Float,
	R8Uint,
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

struct BufferCreateDesc
{
	uint32_t NumElements = 0;
	uint32_t ElementSize = 0;
	EInitialResourceState InitialState = EInitialResourceState::ShaderRead;
	bool bAllowUnorderedAccess = false;
	void* InitialData = nullptr;
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
	float MaxLOD = D3D12_FLOAT32_MAX;
	float MipLODBias = 0.0f;
	uint32_t MaxAnisotropy = 1;
};

class IRenderBackend
{
public:
	virtual ~IRenderBackend() = default;

	virtual ERenderBackendAPI GetAPI() const = 0;
	virtual const char* GetBackendName() const = 0;
	virtual void BeginFrame() = 0;
	virtual void EndFrame() = 0;
	virtual void WaitForGpu() = 0;
	virtual void EmitGpuCrashMarker(const char* markerName) = 0;
	virtual const std::string& GetErrorString() const = 0;
	virtual void ClearErrorString() = 0;
	virtual uint64_t GetTimestampFrequency() const = 0;
	virtual uint32_t GetFrameCount() const = 0;
	virtual uint32_t GetCurrentFrameIndex() const = 0;
	virtual SimpleDX12* AsSimpleDX12() = 0;
	virtual std::shared_ptr<Texture> CreateTexture2D(const TextureCreateDesc& desc) = 0;
	virtual std::shared_ptr<Buffer> CreateBuffer(const BufferCreateDesc& desc) = 0;
	virtual std::shared_ptr<Sampler> CreateSampler(const SamplerCreateDesc& desc) = 0;
	virtual std::shared_ptr<Texture> CreateTextureFromFile(const std::wstring& fileName, bool nonSRGB) = 0;
	virtual std::shared_ptr<Texture> WrapNativeTexture(const Microsoft::WRL::ComPtr<ID3D12Resource>& resource) = 0;
	virtual std::shared_ptr<Texture> CreateTexture3D(
		ETextureFormat format,
		ETextureUsageFlags usage,
		EInitialResourceState initialState,
		int width,
		int height,
		int depth,
		int mipLevels) = 0;
	virtual std::shared_ptr<VertexBuffer> CreateVertexBuffer(uint32_t size, uint32_t stride, void* srcData) = 0;
	virtual std::shared_ptr<IndexBuffer> CreateIndexBuffer(DXGI_FORMAT format, uint32_t size, void* srcData) = 0;
	virtual std::shared_ptr<RTAS> CreateTLAS(std::vector<std::shared_ptr<RTAS>>& bottomLevelAS) = 0;
	virtual Microsoft::WRL::ComPtr<ID3DBlob> CreateShader(const std::wstring& fileName, const std::string& entryPoint, const std::string& target) = 0;
	virtual void ResetDynamicResources() = 0;
	virtual void CreateSwapChainForWindow(
		IDXGIFactory4* factory,
		HWND hwnd,
		uint32_t width,
		uint32_t height,
		DXGI_FORMAT format) = 0;
	virtual Microsoft::WRL::ComPtr<ID3D12Resource> GetSwapChainBuffer(uint32_t bufferIndex) = 0;
	virtual HRESULT CaptureTexture(Texture* source, DirectX::ScratchImage& captured, D3D12_RESOURCE_STATES beforeState) = 0;
	virtual void InitializeImGuiBackend(HWND hwnd, DXGI_FORMAT rtvFormat) = 0;
	virtual void NewImGuiFrame() = 0;
	virtual void RenderImGuiDrawData(ImDrawData* drawData) = 0;
	virtual void ShutdownImGuiBackend() = 0;
	virtual void InitializeGpuTimestampQueries(uint32_t queryCount) = 0;
	virtual void ShutdownGpuTimestampQueries() = 0;
	virtual void WriteGpuTimestamp(uint32_t queryIndex) = 0;
	virtual void ResolveGpuTimestampRange(uint32_t startQueryIndex, uint32_t queryCount) = 0;
	virtual uint64_t ReadGpuTimestampValue(uint32_t queryIndex) const = 0;
	virtual void SetRenderTarget(Texture* colorTarget, Texture* depthTarget = nullptr) = 0;
	virtual void SetRenderTargets(Texture* const* colorTargets, uint32_t colorTargetCount, Texture* depthTarget = nullptr) = 0;
	virtual void ClearRenderTarget(Texture* colorTarget, const float clearColor[4]) = 0;
	virtual void ClearDepth(Texture* depthTarget, float depthValue) = 0;
	virtual void BindDefaultDescriptorHeaps() = 0;
	virtual void SetViewportAndScissor(uint32_t width, uint32_t height) = 0;
	virtual void DrawFullscreenQuad(VertexBuffer* vertexBuffer) = 0;
	virtual void BindMeshBuffers(VertexBuffer* vertexBuffer, IndexBuffer* indexBuffer) = 0;
	virtual void DrawIndexed(uint32_t indexCount, uint32_t startIndexLocation, int32_t baseVertexLocation) = 0;
	virtual void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) = 0;
	virtual void ClearTextureUAVFloat(Texture* texture, const float clearColor[4]) = 0;
	virtual void ExecuteCurrentCommandList() = 0;
	virtual ID3D12GraphicsCommandList* GetGraphicsCommandList() = 0;
	virtual void TransitionTexture(Texture* texture, EResourceState stateBefore, EResourceState stateAfter) = 0;
	virtual void TransitionBuffer(Buffer* buffer, EResourceState stateBefore, EResourceState stateAfter) = 0;
};

std::unique_ptr<IRenderBackend> CreateRenderBackend(ERenderBackendAPI api, const Microsoft::WRL::ComPtr<ID3D12Device5>& device);
