#include "stdafx.h"
#include "NRIBackend.h"

// The entire implementation is gated on CORONA_HAS_NRI. When the CMake option
// CORONA_WITH_NRI is OFF (the default), this translation unit is empty and the
// stock D3D12 / Vulkan build is byte-for-byte unaffected.
#if CORONA_HAS_NRI

#include "NRI.h"
#include "Extensions/NRIDeviceCreation.h"
#include "Extensions/NRIHelper.h"
#include "Extensions/NRISwapChain.h"
#include "Extensions/NRIRayTracing.h"
#include "Extensions/NRIWrapperD3D12.h"

// ---------------------------------------------------------------------------
// PIMPL: all NRI state lives here so the header stays NRI-free.
// ---------------------------------------------------------------------------
struct NRIBackend::Impl
{
	// Filled in by the device-creation milestone:
	//   nri::Device*            Device = nullptr;
	//   nri::CoreInterface      NRICore{};
	//   nri::HelperInterface    NRIHelper{};
	//   nri::RayTracingInterface NRIRayTracing{};
	//   nri::SwapChainInterface NRISwapChain{};
	//   ...command buffers, queues, descriptor pools, side tables keyed by
	//     Texture*/Buffer*/VertexBuffer* (mirrors VulkanBackend's pattern).
	uint32_t FrameIndex = 0;
};

NRIBackend::NRIBackend() : m(std::make_unique<Impl>()) {}
NRIBackend::~NRIBackend() = default;

// Marks a method that is declared/wired but whose NRI implementation is still
// pending. Keeps the backend compiling/linking while it is built out incrementally.
#define NRI_TODO() do { } while (0)

// === Capabilities / identity =============================================
ERenderBackendAPI NRIBackend::GetAPI() const { return ERenderBackendAPI::NRI; }
const char* NRIBackend::GetBackendName() const { return "NRI"; }
uint32_t NRIBackend::GetMaxSupportedHybridStage() const { return 0; }
bool NRIBackend::SupportsRayTracing() const { return false; }
bool NRIBackend::SupportsShaderExecutionReordering() const { return false; }

// === Frame lifecycle / diagnostics =======================================
void NRIBackend::BeginFrame() { NRI_TODO(); }
void NRIBackend::EndFrame() { NRI_TODO(); }
void NRIBackend::WaitForGpu() { NRI_TODO(); }
void NRIBackend::EmitGpuCrashMarker(const char*) { NRI_TODO(); }
const std::string& NRIBackend::GetErrorString() const { return ErrorString; }
void NRIBackend::ClearErrorString() { ErrorString.clear(); }
uint64_t NRIBackend::GetTimestampFrequency() const { return 0; }
uint32_t NRIBackend::GetFrameCount() const { return 0; }
uint32_t NRIBackend::GetCurrentFrameIndex() const { return m->FrameIndex; }
DX12Backend* NRIBackend::AsDX12Backend() { return nullptr; }

// === Resource creation ====================================================
std::shared_ptr<Texture> NRIBackend::CreateTexture2D(const TextureCreateDesc&) { NRI_TODO(); return nullptr; }
std::shared_ptr<Buffer> NRIBackend::CreateBuffer(const BufferCreateDesc&) { NRI_TODO(); return nullptr; }
std::shared_ptr<Sampler> NRIBackend::CreateSampler(const SamplerCreateDesc&) { NRI_TODO(); return nullptr; }
std::shared_ptr<Texture> NRIBackend::CreateTextureFromFile(const std::wstring&, bool) { NRI_TODO(); return nullptr; }
std::shared_ptr<Texture> NRIBackend::CreateTexture3D(ETextureFormat, ETextureUsageFlags, EInitialResourceState, int, int, int, int) { NRI_TODO(); return nullptr; }
void NRIBackend::UploadTexture3D(Texture*, const void*, uint64_t, uint64_t) { NRI_TODO(); }
std::shared_ptr<VertexBuffer> NRIBackend::CreateVertexBuffer(uint32_t, uint32_t, void*) { NRI_TODO(); return nullptr; }
std::shared_ptr<IndexBuffer> NRIBackend::CreateIndexBuffer(EIndexFormat, uint32_t, void*) { NRI_TODO(); return nullptr; }
std::shared_ptr<VertexBuffer> NRIBackend::CreateUploadVertexBuffer(uint32_t, uint32_t, const void*) { NRI_TODO(); return nullptr; }
std::shared_ptr<IndexBuffer> NRIBackend::CreateUploadIndexBuffer(EIndexFormat, uint32_t, const void*) { NRI_TODO(); return nullptr; }
void NRIBackend::UpdateUploadVertexBuffer(VertexBuffer*, const void*, uint32_t) { NRI_TODO(); }
void NRIBackend::UpdateUploadIndexBuffer(IndexBuffer*, const void*, uint32_t) { NRI_TODO(); }
std::shared_ptr<VertexBuffer> NRIBackend::CreateRWVertexBuffer(uint32_t, uint32_t) { NRI_TODO(); return nullptr; }
std::shared_ptr<Buffer> NRIBackend::CreateUploadStructuredBuffer(uint32_t, uint32_t) { NRI_TODO(); return nullptr; }
void NRIBackend::UpdateUploadStructuredBuffer(Buffer*, const void*, uint32_t) { NRI_TODO(); }

// === Ray tracing ==========================================================
std::shared_ptr<RTAS> NRIBackend::CreateBLASForMesh(Mesh*) { NRI_TODO(); return nullptr; }
std::shared_ptr<RTAS> NRIBackend::CreateBLASForSkeletalMesh(Mesh*) { NRI_TODO(); return nullptr; }
void NRIBackend::RefitBLAS(RTAS*, Mesh*) { NRI_TODO(); }
std::shared_ptr<RTAS> NRIBackend::CreateTLAS(const std::vector<RTInstanceDesc>&) { NRI_TODO(); return nullptr; }
bool NRIBackend::UpdateTLAS(const std::shared_ptr<RTAS>&, const std::vector<RTInstanceDesc>&) { NRI_TODO(); return false; }

// === Pipelines / shaders ==================================================
std::shared_ptr<RTPipelineStateObject> NRIBackend::CreateRTPipelineStateObject() { NRI_TODO(); return nullptr; }
std::shared_ptr<ComputePipelineStateObject> NRIBackend::CreateComputePipelineStateObject() { NRI_TODO(); return nullptr; }
ShaderBytecode NRIBackend::CreateShader(const std::wstring&, const std::string&, const std::string&) { NRI_TODO(); return {}; }
void NRIBackend::ResetDynamicResources() { NRI_TODO(); }

// === Swapchain / present ==================================================
void NRIBackend::CreateSwapChainForWindow(WindowHandle, uint32_t, uint32_t, ETextureFormat) { NRI_TODO(); }
std::shared_ptr<Texture> NRIBackend::GetSwapChainTexture(uint32_t) { NRI_TODO(); return nullptr; }
bool NRIBackend::CaptureTexture(Texture*, CapturedImage&, EResourceState) { NRI_TODO(); return false; }
Texture* NRIBackend::GetCurrentWindowRenderTarget() { NRI_TODO(); return nullptr; }
void NRIBackend::PrepareWindowRenderTarget(Texture*) { NRI_TODO(); }
void NRIBackend::FinalizeWindowRenderTarget(Texture*) { NRI_TODO(); }
void NRIBackend::RequestWindowCapture(const std::wstring&) { NRI_TODO(); }
bool NRIBackend::ConsumeWindowCaptureResult(std::wstring*, bool*, std::wstring*) { NRI_TODO(); return false; }

// === ImGui ================================================================
void NRIBackend::InitializeImGuiBackend(WindowHandle, ETextureFormat) { NRI_TODO(); }
void NRIBackend::NewImGuiFrame() { NRI_TODO(); }
void NRIBackend::RenderImGuiDrawData(ImDrawData*) { NRI_TODO(); }
void NRIBackend::ShutdownImGuiBackend() { NRI_TODO(); }

// === Queries ==============================================================
void NRIBackend::InitializeGpuTimestampQueries(uint32_t) { NRI_TODO(); }
void NRIBackend::ShutdownGpuTimestampQueries() { NRI_TODO(); }
void NRIBackend::WriteGpuTimestamp(uint32_t) { NRI_TODO(); }
void NRIBackend::ResolveGpuTimestampRange(uint32_t, uint32_t) { NRI_TODO(); }
uint64_t NRIBackend::ReadGpuTimestampValue(uint32_t) const { return 0; }
void NRIBackend::InitializeOcclusionQueries(uint32_t) { NRI_TODO(); }
void NRIBackend::ShutdownOcclusionQueries() { NRI_TODO(); }
void NRIBackend::BeginOcclusionQuery(uint32_t) { NRI_TODO(); }
void NRIBackend::EndOcclusionQuery(uint32_t) { NRI_TODO(); }
void NRIBackend::ResolveOcclusionQueryRange(uint32_t, uint32_t) { NRI_TODO(); }
uint64_t NRIBackend::ReadOcclusionQueryValue(uint32_t) const { return 0; }

// === Command recording ====================================================
void NRIBackend::SetRenderTarget(Texture*, Texture*) { NRI_TODO(); }
void NRIBackend::SetRenderTargets(Texture* const*, uint32_t, Texture*) { NRI_TODO(); }
void NRIBackend::ClearRenderTarget(Texture*, const float[4]) { NRI_TODO(); }
void NRIBackend::ClearDepth(Texture*, float) { NRI_TODO(); }
void NRIBackend::BindDefaultDescriptorHeaps() { NRI_TODO(); }
void NRIBackend::SetViewportAndScissor(uint32_t, uint32_t) { NRI_TODO(); }
void NRIBackend::DrawFullscreenQuad(VertexBuffer*) { NRI_TODO(); }
void NRIBackend::BindMeshBuffers(VertexBuffer*, IndexBuffer*) { NRI_TODO(); }
void NRIBackend::DrawIndexed(uint32_t, uint32_t, int32_t) { NRI_TODO(); }
void NRIBackend::DrawIndexedInstanced(uint32_t, uint32_t, uint32_t, int32_t, uint32_t) { NRI_TODO(); }
void NRIBackend::Dispatch(uint32_t, uint32_t, uint32_t) { NRI_TODO(); }
void NRIBackend::ClearTextureUAVFloat(Texture*, const float[4]) { NRI_TODO(); }
void NRIBackend::ExecuteCurrentCommandList() { NRI_TODO(); }
void NRIBackend::BeginGpuMarker(uint64_t, const char*) { NRI_TODO(); }
void NRIBackend::EndGpuMarker() { NRI_TODO(); }
void NRIBackend::TransitionTexture(Texture*, EResourceState, EResourceState) { NRI_TODO(); }
void NRIBackend::TransitionBuffer(Buffer*, EResourceState, EResourceState) { NRI_TODO(); }
void NRIBackend::TransitionVertexBuffer(VertexBuffer*, EResourceState, EResourceState) { NRI_TODO(); }

// === Graphics pipelines (by-name binding) =================================
std::shared_ptr<GraphicsPipelineHandle> NRIBackend::CreateGraphicsPipeline(const GraphicsPipelineDesc&) { NRI_TODO(); return nullptr; }
void NRIBackend::BindGraphicsPipeline(GraphicsPipelineHandle*) { NRI_TODO(); }
void NRIBackend::SetGraphicsPipelineConstantData(GraphicsPipelineHandle*, uint32_t, const void*, uint32_t) { NRI_TODO(); }
void NRIBackend::BindGraphicsPipelineTexture(GraphicsPipelineHandle*, const std::string&, Texture*) { NRI_TODO(); }
void NRIBackend::BindGraphicsPipelineBuffer(GraphicsPipelineHandle*, const std::string&, Buffer*) { NRI_TODO(); }
void NRIBackend::BindGraphicsPipelineVertexBufferSRV(GraphicsPipelineHandle*, const std::string&, VertexBuffer*) { NRI_TODO(); }
void NRIBackend::BindGraphicsPipelineSampler(GraphicsPipelineHandle*, const std::string&, Sampler*) { NRI_TODO(); }

#endif // CORONA_HAS_NRI
