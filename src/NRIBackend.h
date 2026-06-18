#pragma once

// Experimental IRenderBackend implementation on top of NVIDIA NRI (NVIDIA Render
// Interface). One backend that targets D3D12 or Vulkan through NRI, with the goal
// of eventually replacing the two hand-written DX12Backend / VulkanBackend.
//
// Compiled only when CORONA_HAS_NRI (CMake option CORONA_WITH_NRI). NRI types are
// kept entirely inside NRIBackend.cpp via a PIMPL so this header — and any stock
// build that does not enable the backend — never sees NRI headers.

#include "RenderBackend.h"
#include "RenderResources.h"
#include <memory>
#include <string>
#include <vector>

class NRIBackend : public IRenderBackend
{
public:
	NRIBackend();
	~NRIBackend() override;

	// --- Capabilities / identity ---
	ERenderBackendAPI GetAPI() const override;
	const char* GetBackendName() const override;
	uint32_t GetMaxSupportedHybridStage() const override;
	RenderBackendCapabilities GetCapabilities() const override;
	bool SupportsRayTracing() const override;
	bool SupportsShaderExecutionReordering() const override;
	bool UsesSimpleGIFallbackPath() const override { return true; }

	// --- Frame lifecycle / diagnostics ---
	void BeginFrame() override;
	void EndFrame() override;
	void WaitForGpu() override;
	void EmitGpuCrashMarker(const char* markerName) override;
	bool IsDeviceLost() const override;
	const std::string& GetErrorString() const override;
	void ClearErrorString() override;
	uint64_t GetTimestampFrequency() const override;
	uint32_t GetFrameCount() const override;
	uint32_t GetCurrentFrameIndex() const override;
	DX12Backend* AsDX12Backend() override;

	// --- Resource creation ---
	std::shared_ptr<Texture> CreateTexture2D(const TextureCreateDesc& desc) override;
	std::shared_ptr<Buffer> CreateBuffer(const BufferCreateDesc& desc) override;
	std::shared_ptr<Sampler> CreateSampler(const SamplerCreateDesc& desc) override;
	std::shared_ptr<Texture> CreateTextureFromFile(const std::wstring& fileName, bool nonSRGB) override;
	std::shared_ptr<Texture> CreateTexture3D(
		ETextureFormat format,
		ETextureUsageFlags usage,
		EInitialResourceState initialState,
		int width, int height, int depth, int mipLevels) override;
	void UploadTexture3D(Texture* texture, const void* data, uint64_t rowPitch, uint64_t slicePitch) override;
	std::shared_ptr<VertexBuffer> CreateVertexBuffer(uint32_t size, uint32_t stride, void* srcData) override;
	std::shared_ptr<IndexBuffer> CreateIndexBuffer(EIndexFormat format, uint32_t size, void* srcData) override;
	std::shared_ptr<VertexBuffer> CreateUploadVertexBuffer(uint32_t size, uint32_t stride, const void* srcData) override;
	std::shared_ptr<IndexBuffer> CreateUploadIndexBuffer(EIndexFormat format, uint32_t size, const void* srcData) override;
	void UpdateUploadVertexBuffer(VertexBuffer* buffer, const void* srcData, uint32_t sizeInBytes) override;
	void UpdateUploadIndexBuffer(IndexBuffer* buffer, const void* srcData, uint32_t sizeInBytes) override;
	std::shared_ptr<VertexBuffer> CreateRWVertexBuffer(uint32_t size, uint32_t stride) override;
	std::shared_ptr<Buffer> CreateUploadStructuredBuffer(uint32_t numElements, uint32_t elementSize) override;
	void UpdateUploadStructuredBuffer(Buffer* buffer, const void* srcData, uint32_t sizeInBytes) override;
	bool UpdateDefaultStructuredBuffer(Buffer* buffer, const void* srcData, uint32_t sizeInBytes) override;
	bool CreateOrUpdateRayTracingInstancePropertyBuffer(
		std::shared_ptr<Buffer>& buffer,
		uint32_t numElements,
		uint32_t elementSize,
		const void* srcData,
		uint32_t sizeInBytes,
		std::wstring* outFailureReason) override;
	std::shared_ptr<Buffer> AllocateTransientUploadStructuredBuffer(uint32_t numElements, uint32_t elementSize, const void* srcData) override;
	std::shared_ptr<Buffer> AllocateTransientDefaultStructuredBuffer(uint32_t numElements, uint32_t elementSize, const void* srcData) override;

	// --- Bindless registry ---
	RHITextureHandle RegisterBindlessTexture(Texture* texture) override;
	bool UpdateBindlessTexture(Texture* texture) override;
	void UnregisterBindlessTexture(Texture* texture) override;
	RHITextureHandle GetBindlessTextureHandle(const Texture* texture) const override;
	RHIBufferHandle RegisterBindlessBuffer(Buffer* buffer) override;
	RHIBufferHandle RegisterBindlessVertexBuffer(VertexBuffer* buffer) override;
	RHIBufferHandle RegisterBindlessIndexBuffer(IndexBuffer* buffer) override;
	void UnregisterBindlessBuffer(Buffer* buffer) override;
	void UnregisterBindlessVertexBuffer(VertexBuffer* buffer) override;
	void UnregisterBindlessIndexBuffer(IndexBuffer* buffer) override;
	RHIBufferHandle GetBindlessBufferHandle(const Buffer* buffer) const override;
	RHIBufferHandle GetBindlessVertexBufferHandle(const VertexBuffer* buffer) const override;
	RHIBufferHandle GetBindlessIndexBufferHandle(const IndexBuffer* buffer) const override;

	// --- Ray tracing ---
	std::shared_ptr<RTAS> CreateBLASForMesh(Mesh* mesh) override;
	std::shared_ptr<RTAS> CreateBLASForSkeletalMesh(Mesh* mesh) override;
	void RefitBLAS(RTAS* rtas, Mesh* mesh) override;
	std::shared_ptr<RTAS> CreateTLAS(const std::vector<RTInstanceDesc>& instances) override;
	bool UpdateTLAS(const std::shared_ptr<RTAS>& topLevelAS, const std::vector<RTInstanceDesc>& instances) override;

	// --- Pipelines / shaders ---
	std::shared_ptr<RTPipelineStateObject> CreateRTPipelineStateObject() override;
	std::shared_ptr<ComputePipelineStateObject> CreateComputePipelineStateObject() override;
	ShaderBytecode CreateShader(const std::wstring& fileName, const std::string& entryPoint, const std::string& target) override;
	void ResetDynamicResources() override;

	// --- Swapchain / present ---
	void CreateSwapChainForWindow(WindowHandle window, uint32_t width, uint32_t height, ETextureFormat format) override;
	std::shared_ptr<Texture> GetSwapChainTexture(uint32_t bufferIndex) override;
	bool CaptureTexture(Texture* source, CapturedImage& captured, EResourceState beforeState) override;
	bool GetStreamlineTextureResource(Texture* texture, EResourceState state, StreamlineTextureResourceDesc& outDesc) const override;
	void* GetStreamlineCommandBuffer() override;
	void* GetStreamlineNativeDevice() const override;
	Texture* GetCurrentWindowRenderTarget() override;
	void PrepareWindowRenderTarget(Texture* renderTarget) override;
	void FinalizeWindowRenderTarget(Texture* renderTarget) override;
	void RequestWindowCapture(const std::wstring& outputPath) override;
	bool ConsumeWindowCaptureResult(std::wstring* outputPath, bool* success, std::wstring* errorMessage) override;

	// --- ImGui ---
	void InitializeImGuiBackend(WindowHandle window, ETextureFormat rtvFormat) override;
	void NewImGuiFrame() override;
	void RenderImGuiDrawData(ImDrawData* drawData) override;
	void ShutdownImGuiBackend() override;

	// --- Queries ---
	void InitializeGpuTimestampQueries(uint32_t queryCount) override;
	void ShutdownGpuTimestampQueries() override;
	void WriteGpuTimestamp(uint32_t queryIndex) override;
	void ResolveGpuTimestampRange(uint32_t startQueryIndex, uint32_t queryCount) override;
	uint64_t ReadGpuTimestampValue(uint32_t queryIndex) const override;
	void InitializeOcclusionQueries(uint32_t queryCount) override;
	void ShutdownOcclusionQueries() override;
	void BeginOcclusionQuery(uint32_t queryIndex) override;
	void EndOcclusionQuery(uint32_t queryIndex) override;
	void ResolveOcclusionQueryRange(uint32_t startQueryIndex, uint32_t queryCount) override;
	uint64_t ReadOcclusionQueryValue(uint32_t queryIndex) const override;

	// --- Command recording ---
	void SetRenderTarget(Texture* colorTarget, Texture* depthTarget = nullptr) override;
	void SetRenderTargets(Texture* const* colorTargets, uint32_t colorTargetCount, Texture* depthTarget = nullptr) override;
	void ClearRenderTarget(Texture* colorTarget, const float clearColor[4]) override;
	void ClearDepth(Texture* depthTarget, float depthValue) override;
	void BindDefaultDescriptorHeaps() override;
	void SetViewportAndScissor(uint32_t width, uint32_t height) override;
	void DrawFullscreenQuad(VertexBuffer* vertexBuffer) override;
	void BindMeshBuffers(VertexBuffer* vertexBuffer, IndexBuffer* indexBuffer) override;
	void DrawInstanced(uint32_t vertexCountPerInstance, uint32_t instanceCount, uint32_t startVertexLocation, uint32_t startInstanceLocation) override;
	void DrawIndexed(uint32_t indexCount, uint32_t startIndexLocation, int32_t baseVertexLocation) override;
	void DrawIndexedInstanced(uint32_t indexCountPerInstance, uint32_t instanceCount, uint32_t startIndexLocation, int32_t baseVertexLocation, uint32_t startInstanceLocation) override;
	bool DrawIndirect(Buffer* indirectArgumentBuffer, uint64_t byteOffset, uint32_t drawCount) override;
	bool DrawIndexedIndirect(Buffer* indirectArgumentBuffer, uint64_t byteOffset, uint32_t drawCount) override;
	void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override;
	void ClearTextureUAVFloat(Texture* texture, const float clearColor[4]) override;
	void CopyTexture(Texture* dstTexture, Texture* srcTexture) override;
	void ExecuteCurrentCommandList() override;
	void BeginGpuMarker(uint64_t color, const char* label) override;
	void EndGpuMarker() override;
	void TransitionTexture(Texture* texture, EResourceState stateBefore, EResourceState stateAfter) override;
	void TransitionBuffer(Buffer* buffer, EResourceState stateBefore, EResourceState stateAfter) override;
	void TransitionVertexBuffer(VertexBuffer* vertexBuffer, EResourceState stateBefore, EResourceState stateAfter) override;
	void UAVBarrier(Buffer* buffer) override;

	// --- Graphics pipelines (bind-group binding) ---
	std::shared_ptr<GraphicsPipelineHandle> CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) override;
	std::shared_ptr<GraphicsBindGroupHandle> CreateGraphicsBindGroup(const GraphicsBindGroupDesc& desc) override;
	void BindGraphicsPipeline(GraphicsPipelineHandle* pipeline) override;
	void BindGraphicsBindGroup(GraphicsPipelineHandle* pipeline, uint32_t slot, const std::shared_ptr<GraphicsBindGroupHandle>& bindGroup) override;

public:
	// Opaque to the rest of the engine; defined in NRIBackend.cpp. Public so the
	// file-local by-name binding adapter (NRIComputePSO) can reference the type.
	struct Impl;

private:
	void MarkDeviceLost(const char* where, int result);

	std::unique_ptr<Impl> m;
	std::string ErrorString;
};
