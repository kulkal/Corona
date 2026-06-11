#include "stdafx.h"
#include "NRIBackend.h"

// The entire implementation is gated on CORONA_HAS_NRI. When the CMake option
// CORONA_WITH_NRI is OFF (the default), this translation unit is empty and the
// stock D3D12 / Vulkan build is byte-for-byte unaffected.
#if CORONA_HAS_NRI

#include <cstdio>
#include <unordered_map>
#include <vector>

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
	nri::Device* Device = nullptr;
	nri::CoreInterface Core{};
	nri::HelperInterface Helper{};
	nri::Queue* GraphicsQueue = nullptr;
	nri::CommandAllocator* CmdAllocator = nullptr;
	nri::CommandBuffer* CmdBuffer = nullptr;
	nri::Fence* Fence = nullptr;
	uint64_t FenceValue = 0;
	// Future milestones add: RayTracingInterface, SwapChainInterface, descriptor
	// pools, plus side tables for Texture*/VertexBuffer*.

	// Backend-owned GPU allocation behind a Corona Buffer wrapper (VulkanBackend
	// keeps native handles in a side table keyed by the wrapper pointer; same here).
	struct BufferAlloc
	{
		nri::Buffer* buffer = nullptr;
		std::vector<nri::Memory*> memory;
	};
	std::unordered_map<Buffer*, BufferAlloc> Buffers;

	std::string BackendName = "NRI (uninitialized)";
	uint8_t RayTracingTier = 0;   // 0=none, 1=DXR1.0, 2=DXR1.1, 3=DXR1.2 (SER)
	uint32_t FrameIndex = 0;

	// Create an NRI buffer and allocate+bind backing memory for it. Shared by
	// CreateBuffer and the bring-up smoke test.
	bool CreateBoundBuffer(uint64_t size, uint32_t structureStride, nri::BufferUsageBits usage,
		nri::MemoryLocation location, nri::Buffer*& outBuffer, std::vector<nri::Memory*>& outMemory)
	{
		outBuffer = nullptr;
		outMemory.clear();
		if (!Device)
			return false;

		nri::BufferDesc bd = {};
		bd.size = size;
		bd.structureStride = structureStride;
		bd.usage = usage;
		if (Core.CreateBuffer(*Device, bd, outBuffer) != nri::Result::SUCCESS || outBuffer == nullptr)
			return false;

		nri::Buffer* bufferList[1] = { outBuffer };
		nri::ResourceGroupDesc rg = {};
		rg.memoryLocation = location;
		rg.buffers = bufferList;
		rg.bufferNum = 1;

		const uint32_t allocNum = Helper.CalculateAllocationNumber(*Device, rg);
		outMemory.resize(allocNum, nullptr);
		if (Helper.AllocateAndBindMemory(*Device, rg, outMemory.data()) != nri::Result::SUCCESS)
		{
			Core.DestroyBuffer(outBuffer);
			outBuffer = nullptr;
			outMemory.clear();
			return false;
		}
		return true;
	}
};

// NRI routes validation / driver messages here. Surface them to the debugger
// output so problems during bring-up are visible.
static void NRI_CALL NRIMessageCallback(nri::Message messageType, const char* file, uint32_t line, const char* message, void* /*userArg*/)
{
	char buffer[2048];
	const char* sev = (messageType == nri::Message::ERROR) ? "ERROR" : (messageType == nri::Message::WARNING) ? "WARN" : "INFO";
	_snprintf_s(buffer, _TRUNCATE, "[NRI][%s] %s (%s:%u)\n", sev, message ? message : "", file ? file : "?", line);
	OutputDebugStringA(buffer);
}

NRIBackend::NRIBackend() : m(std::make_unique<Impl>())
{
	using nri::CoreInterface;
	using nri::HelperInterface;

	nri::DeviceCreationDesc desc = {};
	desc.graphicsAPI = nri::GraphicsAPI::D3D12;
	desc.enableNRIValidation = true;            // embedded NRI-specific validation
	desc.enableGraphicsAPIValidation = false;   // D3D12 debug layer (opt-in later)
	desc.callbackInterface.MessageCallback = NRIMessageCallback;

	nri::Result r = nri::nriCreateDevice(desc, m->Device);
	if (r != nri::Result::SUCCESS || m->Device == nullptr)
	{
		ErrorString = "nriCreateDevice(D3D12) failed";
		OutputDebugStringA("[NRI] nriCreateDevice(D3D12) failed\n");
		return;
	}

	r = nri::nriGetInterface(*m->Device, NRI_INTERFACE(CoreInterface), &m->Core);
	if (r != nri::Result::SUCCESS)
	{
		ErrorString = "nriGetInterface(CoreInterface) failed";
		nri::nriDestroyDevice(m->Device);
		m->Device = nullptr;
		return;
	}

	r = nri::nriGetInterface(*m->Device, NRI_INTERFACE(HelperInterface), &m->Helper);
	if (r != nri::Result::SUCCESS)
	{
		ErrorString = "nriGetInterface(HelperInterface) failed";
		nri::nriDestroyDevice(m->Device);
		m->Device = nullptr;
		return;
	}
	m->Core.GetQueue(*m->Device, nri::QueueType::GRAPHICS, 0, m->GraphicsQueue);

	const nri::DeviceDesc& dd = m->Core.GetDeviceDesc(*m->Device);
	m->RayTracingTier = dd.tiers.rayTracing;
	m->BackendName = std::string("NRI [D3D12] ") + dd.adapterDesc.name;

	// Bring-up smoke test: exercise the resource + memory path end-to-end
	// (Core.CreateBuffer + Helper.AllocateAndBindMemory + Core.DestroyBuffer/
	// FreeMemory). Result is surfaced via GetErrorString() so the bootstrap can
	// log it to the runtime trace.
	bool bufferSmokeOk = false;
	{
		nri::Buffer* smokeBuf = nullptr;
		std::vector<nri::Memory*> smokeMem;
		bufferSmokeOk = m->CreateBoundBuffer(
			256, 0, nri::BufferUsageBits::SHADER_RESOURCE | nri::BufferUsageBits::SHADER_RESOURCE_STORAGE,
			nri::MemoryLocation::DEVICE, smokeBuf, smokeMem);
		if (smokeBuf)
			m->Core.DestroyBuffer(smokeBuf);
		for (nri::Memory* mem : smokeMem)
			if (mem) m->Core.FreeMemory(mem);
	}

	// Command + sync objects, plus an empty submit cycle that validates the
	// command-buffer / queue-submit / host-fence-wait path end-to-end.
	bool submitSmokeOk = false;
	if (m->GraphicsQueue &&
		m->Core.CreateCommandAllocator(*m->GraphicsQueue, m->CmdAllocator) == nri::Result::SUCCESS &&
		m->Core.CreateCommandBuffer(*m->CmdAllocator, m->CmdBuffer) == nri::Result::SUCCESS &&
		m->Core.CreateFence(*m->Device, 0, m->Fence) == nri::Result::SUCCESS)
	{
		if (m->Core.BeginCommandBuffer(*m->CmdBuffer, nullptr) == nri::Result::SUCCESS &&
			m->Core.EndCommandBuffer(*m->CmdBuffer) == nri::Result::SUCCESS)
		{
			nri::FenceSubmitDesc signalFence = {};
			signalFence.fence = m->Fence;
			signalFence.value = 1;
			signalFence.stages = nri::StageBits::ALL;

			nri::CommandBuffer* cbs[1] = { m->CmdBuffer };
			nri::QueueSubmitDesc submit = {};
			submit.commandBuffers = cbs;
			submit.commandBufferNum = 1;
			submit.signalFences = &signalFence;
			submit.signalFenceNum = 1;

			if (m->Core.QueueSubmit(*m->GraphicsQueue, submit) == nri::Result::SUCCESS)
			{
				m->Core.Wait(*m->Fence, 1);
				m->FenceValue = 1;
				submitSmokeOk = (m->Core.GetFenceValue(*m->Fence) >= 1);
			}
		}
	}

	char info[768];
	_snprintf_s(info, _TRUNCATE,
		"device ok [%s], rtTier=%u sm=%u queue=%s bufferSmoke=%s cmd=%s submitSmoke=%s",
		dd.adapterDesc.name, (unsigned)dd.tiers.rayTracing, (unsigned)dd.shaderModel,
		m->GraphicsQueue ? "ok" : "null", bufferSmokeOk ? "PASS" : "FAIL",
		m->CmdBuffer ? "ok" : "null", submitSmokeOk ? "PASS" : "FAIL");
	ErrorString = info;          // diagnostic status (not an error); logged by bootstrap
	OutputDebugStringA("[NRI] ");
	OutputDebugStringA(info);
	OutputDebugStringA("\n");
}

NRIBackend::~NRIBackend()
{
	if (!m)
		return;
	if (m->Device)
	{
		for (auto& kv : m->Buffers)
		{
			if (kv.second.buffer)
				m->Core.DestroyBuffer(kv.second.buffer);
			for (nri::Memory* mem : kv.second.memory)
				if (mem) m->Core.FreeMemory(mem);
		}
		m->Buffers.clear();

		if (m->Fence) { m->Core.DestroyFence(m->Fence); m->Fence = nullptr; }
		if (m->CmdBuffer) { m->Core.DestroyCommandBuffer(m->CmdBuffer); m->CmdBuffer = nullptr; }
		if (m->CmdAllocator) { m->Core.DestroyCommandAllocator(m->CmdAllocator); m->CmdAllocator = nullptr; }

		nri::nriDestroyDevice(m->Device);
		m->Device = nullptr;
		nri::nriReportLiveObjects();
	}
}

// Marks a method that is declared/wired but whose NRI implementation is still
// pending. Keeps the backend compiling/linking while it is built out incrementally.
#define NRI_TODO() do { } while (0)

// === Capabilities / identity =============================================
ERenderBackendAPI NRIBackend::GetAPI() const { return ERenderBackendAPI::NRI; }
const char* NRIBackend::GetBackendName() const { return m->BackendName.c_str(); }
uint32_t NRIBackend::GetMaxSupportedHybridStage() const { return 0; }
bool NRIBackend::SupportsRayTracing() const { return m->RayTracingTier >= 1; }
bool NRIBackend::SupportsShaderExecutionReordering() const { return m->RayTracingTier >= 3; }

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
std::shared_ptr<Buffer> NRIBackend::CreateBuffer(const BufferCreateDesc& desc)
{
	if (!m->Device)
		return nullptr;
	const uint64_t size = static_cast<uint64_t>(desc.NumElements) * desc.ElementSize;
	if (size == 0)
		return nullptr;

	nri::BufferUsageBits usage = nri::BufferUsageBits::SHADER_RESOURCE;
	if (desc.bAllowUnorderedAccess)
		usage = usage | nri::BufferUsageBits::SHADER_RESOURCE_STORAGE;
	const uint32_t stride = (desc.Shape == EBufferShape::Structured) ? desc.ElementSize : 0;

	nri::Buffer* nbuf = nullptr;
	std::vector<nri::Memory*> nmem;
	if (!m->CreateBoundBuffer(size, stride, usage, nri::MemoryLocation::DEVICE, nbuf, nmem))
	{
		ErrorString = "CreateBuffer: CreateBoundBuffer failed";
		return nullptr;
	}

	if (desc.InitialData && m->GraphicsQueue)
	{
		nri::BufferUploadDesc up = {};
		up.buffer = nbuf;
		up.data = desc.InitialData;
		up.after.access = nri::AccessBits::SHADER_RESOURCE;
		up.after.stages = nri::StageBits::ALL;
		m->Helper.UploadData(*m->GraphicsQueue, nullptr, 0, &up, 1);
	}

	auto wrapper = std::make_shared<Buffer>();
	wrapper->Type = (desc.Shape == EBufferShape::Structured) ? Buffer::STRUCTURED : Buffer::BYTE_ADDRESS;
	wrapper->NumElements = desc.NumElements;
	wrapper->ElementSize = desc.ElementSize;

	Impl::BufferAlloc alloc;
	alloc.buffer = nbuf;
	alloc.memory = std::move(nmem);
	m->Buffers[wrapper.get()] = std::move(alloc);
	return wrapper;
}
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
