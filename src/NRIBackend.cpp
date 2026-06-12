#include "stdafx.h"
#include "NRIBackend.h"

// The entire implementation is gated on CORONA_HAS_NRI. When the CMake option
// CORONA_WITH_NRI is OFF (the default), this translation unit is empty and the
// stock D3D12 / Vulkan build is byte-for-byte unaffected.
#if CORONA_HAS_NRI

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>

#include <wrl/client.h>
#include <dxcapi.use.h>

#include "imgui.h"

#include "NRI.h"
#include "Extensions/NRIDeviceCreation.h"
#include "Extensions/NRIHelper.h"
#include "Extensions/NRISwapChain.h"
#include "Extensions/NRIStreamer.h"
#include "Extensions/NRIImgui.h"
#include "Extensions/NRIRayTracing.h"
#include "Extensions/NRIWrapperD3D12.h"

// ---------------------------------------------------------------------------
// Shader compilation: HLSL -> DXIL via dxcompiler.dll. NRI consumes bytecode;
// when NRI runs on D3D12 it wants DXIL (when on Vulkan it would want SPIR-V,
// handled later). Self-contained DXC instance so we don't couple to the DX12
// backend's compiler.
// ---------------------------------------------------------------------------
static dxc::DxcDllSupport gNriDxc;

static std::vector<uint8_t> CompileHLSLToDXIL(
	const void* source, size_t sourceSize, const wchar_t* sourceName,
	const wchar_t* entryPoint, const wchar_t* target, std::string& outError)
{
	using Microsoft::WRL::ComPtr;
	if (FAILED(gNriDxc.Initialize()))
	{
		outError = "DXC: failed to load dxcompiler.dll";
		return {};
	}
	ComPtr<IDxcCompiler> compiler;
	ComPtr<IDxcLibrary> library;
	if (FAILED(gNriDxc.CreateInstance(CLSID_DxcCompiler, __uuidof(IDxcCompiler), &compiler)) ||
		FAILED(gNriDxc.CreateInstance(CLSID_DxcLibrary, __uuidof(IDxcLibrary), &library)))
	{
		outError = "DXC: CreateInstance failed";
		return {};
	}

	ComPtr<IDxcBlobEncoding> textBlob;
	library->CreateBlobWithEncodingFromPinned((LPBYTE)source, (uint32_t)sourceSize, 0, &textBlob);

	ComPtr<IDxcOperationResult> result;
	HRESULT hr = compiler->Compile(textBlob.Get(), sourceName, entryPoint, target, nullptr, 0, nullptr, 0, nullptr, &result);
	if (FAILED(hr) || !result)
	{
		outError = "DXC: Compile call failed";
		return {};
	}
	HRESULT status = E_FAIL;
	result->GetStatus(&status);
	if (FAILED(status))
	{
		ComPtr<IDxcBlobEncoding> errBlob;
		result->GetErrorBuffer(&errBlob);
		outError = errBlob ? std::string((const char*)errBlob->GetBufferPointer(), errBlob->GetBufferSize()) : "DXC: compile error";
		return {};
	}
	ComPtr<IDxcBlob> blob;
	result->GetResult(&blob);
	if (!blob || blob->GetBufferSize() == 0)
	{
		outError = "DXC: empty result";
		return {};
	}
	const uint8_t* p = (const uint8_t*)blob->GetBufferPointer();
	return std::vector<uint8_t>(p, p + blob->GetBufferSize());
}

// ETextureFormat / ETextureUsageFlags -> NRI.
static nri::Format ToNRIFormat(ETextureFormat f)
{
	switch (f)
	{
	case ETextureFormat::RGBA16Float: return nri::Format::RGBA16_SFLOAT;
	case ETextureFormat::RGBA32Float: return nri::Format::RGBA32_SFLOAT;
	case ETextureFormat::RG16Float:   return nri::Format::RG16_SFLOAT;
	case ETextureFormat::RGBA8Unorm:  return nri::Format::RGBA8_UNORM;
	case ETextureFormat::BGRA8Unorm:  return nri::Format::BGRA8_UNORM;
	case ETextureFormat::D32Float:    return nri::Format::D32_SFLOAT;
	case ETextureFormat::R32Float:    return nri::Format::R32_SFLOAT;
	case ETextureFormat::R8Uint:      return nri::Format::R8_UINT;
	}
	return nri::Format::RGBA8_UNORM;
}

static nri::Format ToNRIVertexFormat(EVertexAttributeFormat f)
{
	switch (f)
	{
	case EVertexAttributeFormat::Float2: return nri::Format::RG32_SFLOAT;
	case EVertexAttributeFormat::Float3: return nri::Format::RGB32_SFLOAT;
	case EVertexAttributeFormat::Float4: return nri::Format::RGBA32_SFLOAT;
	}
	return nri::Format::RGBA32_SFLOAT;
}

static nri::TextureUsageBits ToNRITextureUsage(ETextureUsageFlags u)
{
	nri::TextureUsageBits bits = nri::TextureUsageBits::SHADER_RESOURCE;
	if (HasTextureUsage(u, TextureUsage_RenderTarget))    bits = bits | nri::TextureUsageBits::COLOR_ATTACHMENT;
	if (HasTextureUsage(u, TextureUsage_UnorderedAccess)) bits = bits | nri::TextureUsageBits::SHADER_RESOURCE_STORAGE;
	if (HasTextureUsage(u, TextureUsage_DepthStencil))    bits = bits | nri::TextureUsageBits::DEPTH_STENCIL_ATTACHMENT;
	return bits;
}

// Corona EResourceState -> NRI AccessStage (for CmdBarrier on buffers).
static nri::AccessStage ToAccessStage(EResourceState s)
{
	nri::AccessStage a = {};
	switch (s)
	{
	case EResourceState::ShaderRead:      a.access = nri::AccessBits::SHADER_RESOURCE;         a.stages = nri::StageBits::ALL;  break;
	case EResourceState::UnorderedAccess: a.access = nri::AccessBits::SHADER_RESOURCE_STORAGE; a.stages = nri::StageBits::ALL;  break;
	case EResourceState::CopySource:      a.access = nri::AccessBits::COPY_SOURCE;             a.stages = nri::StageBits::COPY; break;
	case EResourceState::CopyDest:        a.access = nri::AccessBits::COPY_DESTINATION;        a.stages = nri::StageBits::COPY; break;
	case EResourceState::VertexBuffer:    a.access = nri::AccessBits::VERTEX_BUFFER;           a.stages = nri::StageBits::ALL;  break;
	case EResourceState::RenderTarget:    a.access = nri::AccessBits::COLOR_ATTACHMENT;        a.stages = nri::StageBits::ALL;  break;
	case EResourceState::DepthWrite:      a.access = nri::AccessBits::DEPTH_STENCIL_ATTACHMENT_WRITE; a.stages = nri::StageBits::ALL; break;
	case EResourceState::Present:         a.access = nri::AccessBits::NONE;                    a.stages = nri::StageBits::ALL;  break;
	default:                              a.access = nri::AccessBits::NONE;                    a.stages = nri::StageBits::ALL;  break;
	}
	return a;
}

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
	nri::CommandBuffer* ActiveCmd = nullptr; // command buffer currently open for recording (set by the frame lifecycle / smokes)
	nri::Fence* Fence = nullptr;
	uint64_t FenceValue = 0;

	// Swap chain
	nri::SwapChainInterface SwapChainI{};
	nri::SwapChain* SwapChain = nullptr;
	std::vector<nri::Texture*> BackBuffers;          // NRI backbuffer textures
	std::vector<nri::Descriptor*> BackBufferViews;   // COLOR_ATTACHMENT views
	std::vector<std::shared_ptr<Texture>> BackBufferWrappers; // Corona wrappers for GetSwapChainTexture
	std::vector<nri::Fence*> AcquireSem;
	std::vector<nri::Fence*> ReleaseSem;
	nri::Fence* FrameFence = nullptr;
	uint64_t SwapFrameIndex = 0;
	uint32_t CurrentBackBuffer = 0;
	nri::Format SwapFormat = nri::Format::RGBA8_UNORM;
	nri::Fence* CurAcquire = nullptr;
	bool FrameHasBackbuffer = false;
	nri::Layout BBLayout = nri::Layout::UNDEFINED; // current backbuffer layout this frame

	// ImGui (NRIImgui + Streamer)
	nri::StreamerInterface StreamerI{};
	nri::Streamer* Streamer = nullptr;
	nri::ImguiInterface ImguiI{};
	nri::Imgui* Imgui = nullptr;
	float PendingClear[4] = { 0, 0, 0, 1 };
	bool HasPendingClear = false;
	Texture* CurrentWindowRT = nullptr;
	GraphicsPipelineHandle* CurrentGfx = nullptr;
	// Future milestones add: RayTracingInterface, descriptor pools.

	// Transition the current backbuffer to a target access/layout, recording on ActiveCmd.
	void TransitionBackbuffer(nri::AccessBits access, nri::Layout layout, nri::StageBits stages)
	{
		if (!ActiveCmd || !FrameHasBackbuffer || CurrentBackBuffer >= BackBuffers.size())
			return;
		nri::TextureBarrierDesc tb = {};
		tb.texture = BackBuffers[CurrentBackBuffer];
		tb.before.access = (BBLayout == nri::Layout::UNDEFINED) ? nri::AccessBits::NONE : (BBLayout == nri::Layout::COLOR_ATTACHMENT ? nri::AccessBits::COLOR_ATTACHMENT : nri::AccessBits::NONE);
		tb.before.layout = BBLayout;
		tb.before.stages = nri::StageBits::ALL;
		tb.after.access = access; tb.after.layout = layout; tb.after.stages = stages;
		tb.mipNum = 1; tb.layerNum = 1;
		nri::BarrierDesc bd = {}; bd.textures = &tb; bd.textureNum = 1;
		Core.CmdBarrier(*ActiveCmd, bd);
		BBLayout = layout;
	}

	// Backend-owned GPU allocation behind a Corona Buffer wrapper (VulkanBackend
	// keeps native handles in a side table keyed by the wrapper pointer; same here).
	struct BufferAlloc
	{
		nri::Buffer* buffer = nullptr;
		std::vector<nri::Memory*> memory;
	};
	std::unordered_map<Buffer*, BufferAlloc> Buffers;

	struct TextureAlloc
	{
		nri::Texture* texture = nullptr;
		std::vector<nri::Memory*> memory;
	};
	std::unordered_map<Texture*, TextureAlloc> Textures;
	std::unordered_map<Sampler*, nri::Descriptor*> Samplers;

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

	void FreeBuffer(nri::Buffer* b, std::vector<nri::Memory*>& mem)
	{
		if (b) Core.DestroyBuffer(b);
		for (nri::Memory* m : mem) if (m) Core.FreeMemory(m);
		mem.clear();
	}

	bool CreateBoundTexture(const nri::TextureDesc& td, nri::Texture*& outTex, std::vector<nri::Memory*>& outMem)
	{
		outTex = nullptr;
		outMem.clear();
		if (!Device)
			return false;
		if (Core.CreateTexture(*Device, td, outTex) != nri::Result::SUCCESS || outTex == nullptr)
			return false;

		nri::Texture* texList[1] = { outTex };
		nri::ResourceGroupDesc rg = {};
		rg.memoryLocation = nri::MemoryLocation::DEVICE;
		rg.textures = texList;
		rg.textureNum = 1;

		const uint32_t allocNum = Helper.CalculateAllocationNumber(*Device, rg);
		outMem.resize(allocNum, nullptr);
		if (Helper.AllocateAndBindMemory(*Device, rg, outMem.data()) != nri::Result::SUCCESS)
		{
			Core.DestroyTexture(outTex);
			outTex = nullptr;
			outMem.clear();
			return false;
		}
		return true;
	}

	void FreeTexture(nri::Texture* t, std::vector<nri::Memory*>& mem)
	{
		if (t) Core.DestroyTexture(t);
		for (nri::Memory* m : mem) if (m) Core.FreeMemory(m);
		mem.clear();
	}

	// End-to-end compute: compile a trivial cs_6_5 kernel that writes a sentinel
	// into a RWStructuredBuffer (root UAV), dispatch it, copy the result to a
	// readback buffer, map it and verify. Returns a short human-readable result.
	std::string RunComputeSmoke()
	{
		using nri::Result;
		const uint32_t N = 4;
		const uint64_t bufSize = N * sizeof(uint32_t);
		const uint32_t sentinel = 0xCAFEu;

		static const char* kCS =
			"RWStructuredBuffer<uint> OutBuf : register(u0);\n"
			"[numthreads(1,1,1)] void main(uint3 id : SV_DispatchThreadID) { OutBuf[id.x] = 0xCAFEu; }\n";
		std::string err;
		std::vector<uint8_t> dxil = CompileHLSLToDXIL(kCS, strlen(kCS), L"nri_compute.cs", L"main", L"cs_6_5", err);
		if (dxil.empty())
			return "FAIL(shader)";

		nri::Buffer* outBuf = nullptr; std::vector<nri::Memory*> outMem;
		nri::Buffer* rbBuf = nullptr;  std::vector<nri::Memory*> rbMem;
		nri::Descriptor* outView = nullptr;
		nri::PipelineLayout* layout = nullptr;
		nri::Pipeline* pipeline = nullptr;
		std::string result = "FAIL";

		do
		{
			if (!CreateBoundBuffer(bufSize, sizeof(uint32_t), nri::BufferUsageBits::SHADER_RESOURCE_STORAGE, nri::MemoryLocation::DEVICE, outBuf, outMem))
				{ result = "FAIL(outBuf)"; break; }
			if (!CreateBoundBuffer(bufSize, 0, nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_READBACK, rbBuf, rbMem))
				{ result = "FAIL(rbBuf)"; break; }

			nri::BufferViewDesc bvd = {};
			bvd.buffer = outBuf;
			bvd.type = nri::BufferView::STORAGE_STRUCTURED_BUFFER;
			bvd.offset = 0;
			bvd.size = bufSize;
			bvd.structureStride = sizeof(uint32_t);
			if (Core.CreateBufferView(bvd, outView) != Result::SUCCESS)
				{ result = "FAIL(view)"; break; }

			nri::RootDescriptorDesc rd = {};
			rd.registerIndex = 0;
			rd.descriptorType = nri::DescriptorType::STORAGE_STRUCTURED_BUFFER;
			rd.shaderStages = nri::StageBits::COMPUTE_SHADER;
			nri::PipelineLayoutDesc pld = {};
			pld.rootRegisterSpace = 0;
			pld.rootDescriptors = &rd;
			pld.rootDescriptorNum = 1;
			pld.shaderStages = nri::StageBits::COMPUTE_SHADER;
			if (Core.CreatePipelineLayout(*Device, pld, layout) != Result::SUCCESS)
				{ result = "FAIL(layout)"; break; }

			nri::ComputePipelineDesc cpd = {};
			cpd.pipelineLayout = layout;
			cpd.shader.stage = nri::StageBits::COMPUTE_SHADER;
			cpd.shader.bytecode = dxil.data();
			cpd.shader.size = dxil.size();
			cpd.shader.entryPointName = "main";
			if (Core.CreateComputePipeline(*Device, cpd, pipeline) != Result::SUCCESS)
				{ result = "FAIL(pipeline)"; break; }

			Core.ResetCommandAllocator(*CmdAllocator);
			Core.BeginCommandBuffer(*CmdBuffer, nullptr);

			nri::BufferBarrierDesc toUav = {};
			toUav.buffer = outBuf;
			toUav.before.access = nri::AccessBits::NONE; toUav.before.stages = nri::StageBits::ALL;
			toUav.after.access = nri::AccessBits::SHADER_RESOURCE_STORAGE; toUav.after.stages = nri::StageBits::COMPUTE_SHADER;
			nri::BarrierDesc bar1 = {}; bar1.buffers = &toUav; bar1.bufferNum = 1;
			Core.CmdBarrier(*CmdBuffer, bar1);

			Core.CmdSetPipelineLayout(*CmdBuffer, nri::BindPoint::COMPUTE, *layout);
			nri::SetRootDescriptorDesc srd = {};
			srd.rootDescriptorIndex = 0;
			srd.descriptor = outView;
			srd.offset = 0;
			srd.bindPoint = nri::BindPoint::COMPUTE;
			Core.CmdSetRootDescriptor(*CmdBuffer, srd);
			Core.CmdSetPipeline(*CmdBuffer, *pipeline);
			nri::DispatchDesc disp = { N, 1, 1 };
			Core.CmdDispatch(*CmdBuffer, disp);

			nri::BufferBarrierDesc toCopy = {};
			toCopy.buffer = outBuf;
			toCopy.before.access = nri::AccessBits::SHADER_RESOURCE_STORAGE; toCopy.before.stages = nri::StageBits::COMPUTE_SHADER;
			toCopy.after.access = nri::AccessBits::COPY_SOURCE; toCopy.after.stages = nri::StageBits::COPY;
			nri::BarrierDesc bar2 = {}; bar2.buffers = &toCopy; bar2.bufferNum = 1;
			Core.CmdBarrier(*CmdBuffer, bar2);
			Core.CmdCopyBuffer(*CmdBuffer, *rbBuf, 0, *outBuf, 0, bufSize);

			if (Core.EndCommandBuffer(*CmdBuffer) != Result::SUCCESS)
				{ result = "FAIL(record)"; break; }

			nri::FenceSubmitDesc sf = {};
			sf.fence = Fence; sf.value = ++FenceValue; sf.stages = nri::StageBits::ALL;
			nri::CommandBuffer* cbs[1] = { CmdBuffer };
			nri::QueueSubmitDesc qs = {};
			qs.commandBuffers = cbs; qs.commandBufferNum = 1;
			qs.signalFences = &sf; qs.signalFenceNum = 1;
			if (Core.QueueSubmit(*GraphicsQueue, qs) != Result::SUCCESS)
				{ result = "FAIL(submit)"; break; }
			Core.Wait(*Fence, FenceValue);

			uint32_t* mapped = (uint32_t*)Core.MapBuffer(*rbBuf, 0, bufSize);
			uint32_t v0 = mapped ? mapped[0] : 0u;
			uint32_t vN = mapped ? mapped[N - 1] : 0u;
			Core.UnmapBuffer(*rbBuf);

			char buf[64];
			const bool ok = mapped && v0 == sentinel && vN == sentinel;
			_snprintf_s(buf, _TRUNCATE, ok ? "PASS(0x%X)" : "FAIL(val=0x%X)", v0);
			result = buf;
		} while (false);

		if (pipeline) Core.DestroyPipeline(pipeline);
		if (layout) Core.DestroyPipelineLayout(layout);
		if (outView) Core.DestroyDescriptor(outView);
		FreeBuffer(outBuf, outMem);
		FreeBuffer(rbBuf, rbMem);
		return result;
	}

	// Acquire a backbuffer, clear it to a color via a render pass, and present.
	bool PresentClear(float r, float g, float b, float a)
	{
		if (!SwapChain || BackBuffers.empty() || !CmdBuffer || !FrameFence)
			return false;
		const uint32_t n = (uint32_t)BackBuffers.size();
		nri::Fence* acquire = AcquireSem[SwapFrameIndex % n];
		uint32_t idx = 0;
		if (SwapChainI.AcquireNextTexture(*SwapChain, *acquire, idx) != nri::Result::SUCCESS || idx >= n)
			return false;
		CurrentBackBuffer = idx;

		Core.ResetCommandAllocator(*CmdAllocator);
		if (Core.BeginCommandBuffer(*CmdBuffer, nullptr) != nri::Result::SUCCESS)
			return false;

		nri::TextureBarrierDesc toRT = {};
		toRT.texture = BackBuffers[idx];
		toRT.before.access = nri::AccessBits::NONE;             toRT.before.layout = nri::Layout::UNDEFINED;        toRT.before.stages = nri::StageBits::ALL;
		toRT.after.access  = nri::AccessBits::COLOR_ATTACHMENT; toRT.after.layout  = nri::Layout::COLOR_ATTACHMENT; toRT.after.stages  = nri::StageBits::ALL;
		toRT.mipNum = 1; toRT.layerNum = 1;
		nri::BarrierDesc b1 = {}; b1.textures = &toRT; b1.textureNum = 1;
		Core.CmdBarrier(*CmdBuffer, b1);

		nri::AttachmentDesc colorAtt = {};
		colorAtt.descriptor = BackBufferViews[idx];
		colorAtt.clearValue.color.f = nri::Color32f{ r, g, b, a };
		colorAtt.loadOp = nri::LoadOp::CLEAR;
		colorAtt.storeOp = nri::StoreOp::STORE;
		nri::RenderingDesc rd = {};
		rd.colors = &colorAtt; rd.colorNum = 1;
		Core.CmdBeginRendering(*CmdBuffer, rd);
		Core.CmdEndRendering(*CmdBuffer);

		nri::TextureBarrierDesc toPresent = {};
		toPresent.texture = BackBuffers[idx];
		toPresent.before.access = nri::AccessBits::COLOR_ATTACHMENT; toPresent.before.layout = nri::Layout::COLOR_ATTACHMENT; toPresent.before.stages = nri::StageBits::ALL;
		toPresent.after.access  = nri::AccessBits::NONE;             toPresent.after.layout  = nri::Layout::PRESENT;          toPresent.after.stages  = nri::StageBits::NONE;
		toPresent.mipNum = 1; toPresent.layerNum = 1;
		nri::BarrierDesc b2 = {}; b2.textures = &toPresent; b2.textureNum = 1;
		Core.CmdBarrier(*CmdBuffer, b2);

		if (Core.EndCommandBuffer(*CmdBuffer) != nri::Result::SUCCESS)
			return false;

		nri::Fence* release = ReleaseSem[idx];
		nri::FenceSubmitDesc waitAcq = {}; waitAcq.fence = acquire; waitAcq.stages = nri::StageBits::ALL;
		nri::FenceSubmitDesc sigRel = {}; sigRel.fence = release;
		nri::FenceSubmitDesc sigFrame = {}; sigFrame.fence = FrameFence; sigFrame.value = 1 + SwapFrameIndex;
		nri::FenceSubmitDesc signals[2] = { sigRel, sigFrame };
		nri::CommandBuffer* cbs[1] = { CmdBuffer };
		nri::QueueSubmitDesc qs = {};
		qs.waitFences = &waitAcq; qs.waitFenceNum = 1;
		qs.commandBuffers = cbs; qs.commandBufferNum = 1;
		qs.signalFences = signals; qs.signalFenceNum = 2;
		if (Core.QueueSubmit(*GraphicsQueue, qs) != nri::Result::SUCCESS)
			return false;
		if (SwapChainI.QueuePresent(*SwapChain, *release) != nri::Result::SUCCESS)
			return false;

		SwapFrameIndex++;
		Core.Wait(*FrameFence, SwapFrameIndex); // synchronous pacing for bring-up
		return true;
	}
};

// ---------------------------------------------------------------------------
// By-name binding adapter: maps Corona's ComputePipelineStateObject (resources
// bound by string name + HLSL register) onto an NRI pipeline layout + compute
// pipeline. This milestone supports buffer bindings via NRI root descriptors
// (UAV -> STORAGE_STRUCTURED_BUFFER, SRV -> STRUCTURED_BUFFER); texture / CBV /
// sampler bindings (descriptor sets) come next.
// ---------------------------------------------------------------------------
class NRIComputePSO : public ComputePipelineStateObject
{
public:
	explicit NRIComputePSO(NRIBackend::Impl* impl) : m(impl) {}
	~NRIComputePSO() override
	{
		if (!m || !m->Device) return;
		if (Pipeline) m->Core.DestroyPipeline(Pipeline);
		if (Layout) m->Core.DestroyPipelineLayout(Layout);
		if (Pool) m->Core.DestroyDescriptorPool(Pool);
		for (auto& b : Bindings) if (b.ownsDesc && b.desc) m->Core.DestroyDescriptor(b.desc);
		for (auto& kv : CbvBuffers) m->FreeBuffer(kv.second.buffer, kv.second.memory);
	}

	enum class RegClass { SRV, UAV, CBV, Sampler };
	enum class ResKind { None, TexSRV, TexUAV, BufSRV, BufUAV, CBV, Sampler };
	struct Binding
	{
		std::string name;
		uint32_t reg = 0;
		RegClass regClass = RegClass::SRV;
		ResKind kind = ResKind::None;
		Texture* tex = nullptr;
		Buffer* buf = nullptr;
		Sampler* samp = nullptr;
		nri::Descriptor* desc = nullptr;
		bool ownsDesc = false;           // desc created by us (texture/buffer view) vs borrowed (cbv/sampler)
		void* lastResource = nullptr;    // resource the current desc was built for (dirty check)
		uint32_t setIndex = 0;
		uint32_t rangeIndex = 0;
	};

	void BindSRV(const std::string& name, uint32_t baseRegister, uint32_t) override { AddBinding(name, baseRegister, RegClass::SRV); }
	void BindUAV(const std::string& name, uint32_t baseRegister) override { AddBinding(name, baseRegister, RegClass::UAV); }
	void BindCBV(const std::string& name, uint32_t baseRegister, uint32_t size) override { AddBinding(name, baseRegister, RegClass::CBV); CbvSizeByName[name] = size; }
	void BindSampler(const std::string& name, uint32_t baseRegister) override { AddBinding(name, baseRegister, RegClass::Sampler); }

	// Defer pipeline/layout creation to the first Apply: by then the Set* calls
	// have revealed each binding's resource type (texture vs buffer), which the
	// by-name Bind* calls don't carry.
	bool InitCS(const std::wstring& shaderFile, const std::string& entryPoint) override
	{
		if (!m->Device) return false;
		std::ifstream f(shaderFile, std::ios::binary);
		if (!f.good()) return false;
		std::stringstream ss; ss << f.rdbuf();
		const std::string src = ss.str();
		const std::wstring entryW(entryPoint.begin(), entryPoint.end());
		std::string err;
		Dxil = CompileHLSLToDXIL(src.data(), src.size(), shaderFile.c_str(), entryW.c_str(), L"cs_6_5", err);
		EntryPoint = entryPoint;
		return !Dxil.empty();
	}

	void SetTextureSRV(const std::string& name, Texture* t) override { SetRes(name, ResKind::TexSRV, t, nullptr, nullptr); }
	void SetTextureUAV(const std::string& name, Texture* t) override { SetRes(name, ResKind::TexUAV, t, nullptr, nullptr); }
	void SetBufferSRV(const std::string& name, Buffer* b) override { SetRes(name, ResKind::BufSRV, nullptr, b, nullptr); }
	void SetBufferUAV(const std::string& name, Buffer* b) override { SetRes(name, ResKind::BufUAV, nullptr, b, nullptr); }
	void SetVertexBufferUAV(const std::string&, VertexBuffer*) override {} // skinning output: later
	void SetSampler(const std::string& name, Sampler* s) override { SetRes(name, ResKind::Sampler, nullptr, nullptr, s); }
	void SetCBVValue(const std::string& name, void* data) override
	{
		if (!m->Device || !data) return;
		auto sit = CbvSizeByName.find(name);
		const uint32_t size = (sit != CbvSizeByName.end()) ? sit->second : 0u;
		if (size == 0) return;
		const uint32_t alignedSize = (size + 255u) & ~255u; // D3D12 CB alignment
		CbvBuf& cb = CbvBuffers[name];
		if (!cb.buffer)
		{
			if (!m->CreateBoundBuffer(alignedSize, 0, nri::BufferUsageBits::CONSTANT_BUFFER, nri::MemoryLocation::HOST_UPLOAD, cb.buffer, cb.memory))
			{ CbvBuffers.erase(name); return; }
			cb.size = alignedSize;
			nri::BufferViewDesc bvd = {};
			bvd.buffer = cb.buffer; bvd.type = nri::BufferView::CONSTANT_BUFFER; bvd.offset = 0; bvd.size = alignedSize;
			m->Core.CreateBufferView(bvd, cb.view);
		}
		if (Binding* b = Find(name)) { b->kind = ResKind::CBV; b->desc = cb.view; b->ownsDesc = false; }
		void* mapped = m->Core.MapBuffer(*cb.buffer, 0, size);
		if (mapped) { memcpy(mapped, data, size); m->Core.UnmapBuffer(*cb.buffer); }
	}

	void Apply() override
	{
		if (!m->ActiveCmd) return;
		if (!EnsureInit()) return;

		for (Binding& b : Bindings)
			RefreshDescriptor(b);

		// Write descriptors into the sets.
		for (Binding& b : Bindings)
		{
			if (b.kind == ResKind::None || !b.desc || b.setIndex >= Sets.size() || !Sets[b.setIndex])
				continue;
			nri::Descriptor* d = b.desc;
			nri::UpdateDescriptorRangeDesc upd = {};
			upd.descriptorSet = Sets[b.setIndex];
			upd.rangeIndex = b.rangeIndex;
			upd.baseDescriptor = 0;
			upd.descriptors = &d;
			upd.descriptorNum = 1;
			m->Core.UpdateDescriptorRanges(&upd, 1);
		}

		if (Pool) m->Core.CmdSetDescriptorPool(*m->ActiveCmd, *Pool);
		m->Core.CmdSetPipelineLayout(*m->ActiveCmd, nri::BindPoint::COMPUTE, *Layout);
		for (uint32_t s = 0; s < Sets.size(); ++s)
		{
			if (!Sets[s]) continue;
			nri::SetDescriptorSetDesc sd = {};
			sd.setIndex = s;
			sd.descriptorSet = Sets[s];
			sd.bindPoint = nri::BindPoint::COMPUTE;
			m->Core.CmdSetDescriptorSet(*m->ActiveCmd, sd);
		}
		m->Core.CmdSetPipeline(*m->ActiveCmd, *Pipeline);
	}

	nri::Pipeline* GetPipeline() const { return Pipeline; }

private:
	struct CbvBuf { nri::Buffer* buffer = nullptr; std::vector<nri::Memory*> memory; uint32_t size = 0; nri::Descriptor* view = nullptr; };

	void AddBinding(const std::string& name, uint32_t reg, RegClass rc)
	{
		if (Find(name)) return;
		Binding b; b.name = name; b.reg = reg; b.regClass = rc;
		Bindings.push_back(b);
	}
	Binding* Find(const std::string& name)
	{
		for (Binding& b : Bindings) if (b.name == name) return &b;
		return nullptr;
	}
	void SetRes(const std::string& name, ResKind k, Texture* t, Buffer* bufp, Sampler* s)
	{
		Binding* b = Find(name);
		if (!b) { AddBinding(name, 0, k == ResKind::Sampler ? RegClass::Sampler : (k == ResKind::TexUAV || k == ResKind::BufUAV) ? RegClass::UAV : RegClass::SRV); b = Find(name); }
		if (!b) return;
		b->kind = k; b->tex = t; b->buf = bufp; b->samp = s;
	}
	static nri::DescriptorType ToDescriptorType(ResKind k)
	{
		switch (k)
		{
		case ResKind::TexSRV: return nri::DescriptorType::TEXTURE;
		case ResKind::TexUAV: return nri::DescriptorType::STORAGE_TEXTURE;
		case ResKind::BufSRV: return nri::DescriptorType::STRUCTURED_BUFFER;
		case ResKind::BufUAV: return nri::DescriptorType::STORAGE_STRUCTURED_BUFFER;
		case ResKind::CBV:    return nri::DescriptorType::CONSTANT_BUFFER;
		case ResKind::Sampler:return nri::DescriptorType::SAMPLER;
		default:              return nri::DescriptorType::TEXTURE;
		}
	}

	bool EnsureInit()
	{
		if (InitAttempted) return Pipeline != nullptr;
		InitAttempted = true;
		if (Dxil.empty() || !m->Device) return false;

		std::vector<nri::DescriptorRangeDesc> resRanges, sampRanges;
		const bool haveSamplers = [&] { for (auto& b : Bindings) if (b.kind == ResKind::Sampler) return true; return false; }();
		const uint32_t resSetIndex = 0;
		const uint32_t sampSetIndex = haveSamplers ? 1u : 0u;

		uint32_t texN = 0, storTexN = 0, sbufN = 0, storSbufN = 0, cbvN = 0, sampN = 0;
		for (Binding& b : Bindings)
		{
			if (b.kind == ResKind::None) continue;
			nri::DescriptorRangeDesc r = {};
			r.baseRegisterIndex = b.reg;
			r.descriptorNum = 1;
			r.descriptorType = ToDescriptorType(b.kind);
			r.shaderStages = nri::StageBits::COMPUTE_SHADER;
			if (b.kind == ResKind::Sampler) { b.setIndex = sampSetIndex; b.rangeIndex = (uint32_t)sampRanges.size(); sampRanges.push_back(r); ++sampN; }
			else
			{
				b.setIndex = resSetIndex; b.rangeIndex = (uint32_t)resRanges.size(); resRanges.push_back(r);
				switch (b.kind)
				{
				case ResKind::TexSRV: ++texN; break;
				case ResKind::TexUAV: ++storTexN; break;
				case ResKind::BufSRV: ++sbufN; break;
				case ResKind::BufUAV: ++storSbufN; break;
				case ResKind::CBV:    ++cbvN; break;
				default: break;
				}
			}
		}

		std::vector<nri::DescriptorSetDesc> sets;
		if (!resRanges.empty()) { nri::DescriptorSetDesc s = {}; s.registerSpace = 0; s.ranges = resRanges.data(); s.rangeNum = (uint32_t)resRanges.size(); sets.push_back(s); }
		if (!sampRanges.empty()) { nri::DescriptorSetDesc s = {}; s.registerSpace = 0; s.ranges = sampRanges.data(); s.rangeNum = (uint32_t)sampRanges.size(); sets.push_back(s); }

		nri::PipelineLayoutDesc pld = {};
		pld.rootRegisterSpace = 0;
		pld.descriptorSets = sets.empty() ? nullptr : sets.data();
		pld.descriptorSetNum = (uint32_t)sets.size();
		pld.shaderStages = nri::StageBits::COMPUTE_SHADER;
		if (m->Core.CreatePipelineLayout(*m->Device, pld, Layout) != nri::Result::SUCCESS)
			return false;

		if (!sets.empty())
		{
			nri::DescriptorPoolDesc pd = {};
			pd.descriptorSetMaxNum = (uint32_t)sets.size();
			pd.textureMaxNum = texN; pd.storageTextureMaxNum = storTexN;
			pd.structuredBufferMaxNum = sbufN; pd.storageStructuredBufferMaxNum = storSbufN;
			pd.constantBufferMaxNum = cbvN; pd.samplerMaxNum = sampN;
			if (m->Core.CreateDescriptorPool(*m->Device, pd, Pool) != nri::Result::SUCCESS)
				return false;
			Sets.resize(sets.size(), nullptr);
			for (uint32_t s = 0; s < sets.size(); ++s)
				m->Core.AllocateDescriptorSets(*Pool, *Layout, s, &Sets[s], 1, 0);
		}

		nri::ComputePipelineDesc cpd = {};
		cpd.pipelineLayout = Layout;
		cpd.shader.stage = nri::StageBits::COMPUTE_SHADER;
		cpd.shader.bytecode = Dxil.data();
		cpd.shader.size = Dxil.size();
		cpd.shader.entryPointName = EntryPoint.c_str();
		return m->Core.CreateComputePipeline(*m->Device, cpd, Pipeline) == nri::Result::SUCCESS;
	}

	void RefreshDescriptor(Binding& b)
	{
		if (b.kind == ResKind::None) return;
		if (b.kind == ResKind::CBV) return;       // CBV view set in SetCBVValue
		if (b.kind == ResKind::Sampler)
		{
			if (b.samp) { auto it = m->Samplers.find(b.samp); b.desc = (it != m->Samplers.end()) ? it->second : nullptr; b.ownsDesc = false; }
			return;
		}
		void* res = b.tex ? (void*)b.tex : (void*)b.buf;
		if (res == b.lastResource && b.desc) return; // unchanged
		if (b.ownsDesc && b.desc) { m->Core.DestroyDescriptor(b.desc); b.desc = nullptr; }
		b.lastResource = res;

		if (b.kind == ResKind::TexSRV || b.kind == ResKind::TexUAV)
		{
			if (!b.tex) return;
			auto it = m->Textures.find(b.tex);
			if (it == m->Textures.end() || !it->second.texture) return;
			const nri::TextureDesc& td = m->Core.GetTextureDesc(*it->second.texture);
			nri::TextureViewDesc tvd = {};
			tvd.texture = it->second.texture;
			tvd.type = (b.kind == ResKind::TexUAV) ? nri::TextureView::STORAGE_TEXTURE : nri::TextureView::TEXTURE;
			tvd.format = td.format;
			tvd.mipNum = nri::REMAINING; tvd.layerNum = nri::REMAINING;
			nri::Descriptor* d = nullptr;
			if (m->Core.CreateTextureView(tvd, d) == nri::Result::SUCCESS) { b.desc = d; b.ownsDesc = true; }
		}
		else // BufSRV / BufUAV
		{
			if (!b.buf) return;
			auto it = m->Buffers.find(b.buf);
			if (it == m->Buffers.end() || !it->second.buffer) return;
			nri::BufferViewDesc bvd = {};
			bvd.buffer = it->second.buffer;
			bvd.type = (b.kind == ResKind::BufUAV) ? nri::BufferView::STORAGE_STRUCTURED_BUFFER : nri::BufferView::STRUCTURED_BUFFER;
			bvd.offset = 0;
			bvd.size = static_cast<uint64_t>(b.buf->NumElements) * b.buf->ElementSize;
			bvd.structureStride = b.buf->ElementSize ? b.buf->ElementSize : 4;
			nri::Descriptor* d = nullptr;
			if (m->Core.CreateBufferView(bvd, d) == nri::Result::SUCCESS) { b.desc = d; b.ownsDesc = true; }
		}
	}

	NRIBackend::Impl* m = nullptr;
	std::vector<Binding> Bindings;
	std::unordered_map<std::string, uint32_t> CbvSizeByName;
	std::unordered_map<std::string, CbvBuf> CbvBuffers;
	std::vector<uint8_t> Dxil;
	std::string EntryPoint;
	nri::PipelineLayout* Layout = nullptr;
	nri::Pipeline* Pipeline = nullptr;
	nri::DescriptorPool* Pool = nullptr;
	std::vector<nri::DescriptorSet*> Sets;
	bool InitAttempted = false;
};

// ---------------------------------------------------------------------------
// Graphics pipeline (by-name binding). Bindings are known up-front from the
// GraphicsPipelineDesc lists (texture SRV / buffer SRV / sampler / constant
// buffer), so no deferral is needed. This milestone builds the NRI graphics
// pipeline + descriptor sets; command recording (render pass + draws) is wired
// separately in the backend.
// ---------------------------------------------------------------------------
class NRIGraphicsPipeline : public GraphicsPipelineHandle
{
public:
	NRIGraphicsPipeline(NRIBackend::Impl* impl, const GraphicsPipelineDesc& desc) : m(impl)
	{
		Build(desc);
	}
	~NRIGraphicsPipeline() override
	{
		if (!m || !m->Device) return;
		if (Pipeline) m->Core.DestroyPipeline(Pipeline);
		if (Layout) m->Core.DestroyPipelineLayout(Layout);
		if (Pool) m->Core.DestroyDescriptorPool(Pool);
		for (auto& b : Bindings) if (b.ownsDesc && b.desc) m->Core.DestroyDescriptor(b.desc);
		if (Cbv.buffer) m->FreeBuffer(Cbv.buffer, Cbv.memory);
	}

	bool IsValid() const { return Pipeline != nullptr; }
	nri::Pipeline* GetPipeline() const { return Pipeline; }
	nri::PipelineLayout* GetLayout() const { return Layout; }

	enum class Kind { TexSRV, BufSRV, Sampler, CBV };
	struct Binding { std::string name; uint32_t reg; Kind kind; nri::Descriptor* desc = nullptr; bool ownsDesc = false; void* last = nullptr; Texture* tex = nullptr; Buffer* buf = nullptr; Sampler* samp = nullptr; uint32_t setIndex = 0; uint32_t rangeIndex = 0; };
	struct CbvState { nri::Buffer* buffer = nullptr; std::vector<nri::Memory*> memory; nri::Descriptor* view = nullptr; uint32_t size = 0; };

	void SetTexture(const std::string& name, Texture* t) { if (Binding* b = Find(name)) { b->tex = t; } }
	void SetBuffer(const std::string& name, Buffer* bb) { if (Binding* b = Find(name)) { b->buf = bb; } }
	void SetSampler(const std::string& name, Sampler* s) { if (Binding* b = Find(name)) { b->samp = s; } }
	void SetConstant(const void* data, uint32_t size)
	{
		if (!Cbv.buffer || !data || size == 0) return;
		void* mapped = m->Core.MapBuffer(*Cbv.buffer, 0, size > Cbv.size ? Cbv.size : size);
		if (mapped) { memcpy(mapped, data, size > Cbv.size ? Cbv.size : size); m->Core.UnmapBuffer(*Cbv.buffer); }
	}

	// Record layout + descriptor sets + pipeline onto the active command buffer
	// (the caller must already be inside a render pass with matching attachments).
	void Bind()
	{
		if (!Pipeline || !m->ActiveCmd) return;
		for (Binding& b : Bindings) RefreshDescriptor(b);
		for (Binding& b : Bindings)
		{
			if (!b.desc || b.setIndex >= Sets.size() || !Sets[b.setIndex]) continue;
			nri::Descriptor* d = b.desc;
			nri::UpdateDescriptorRangeDesc upd = {};
			upd.descriptorSet = Sets[b.setIndex]; upd.rangeIndex = b.rangeIndex; upd.baseDescriptor = 0;
			upd.descriptors = &d; upd.descriptorNum = 1;
			m->Core.UpdateDescriptorRanges(&upd, 1);
		}
		if (Pool) m->Core.CmdSetDescriptorPool(*m->ActiveCmd, *Pool);
		m->Core.CmdSetPipelineLayout(*m->ActiveCmd, nri::BindPoint::GRAPHICS, *Layout);
		for (uint32_t s = 0; s < Sets.size(); ++s)
		{
			if (!Sets[s]) continue;
			nri::SetDescriptorSetDesc sd = {}; sd.setIndex = s; sd.descriptorSet = Sets[s]; sd.bindPoint = nri::BindPoint::GRAPHICS;
			m->Core.CmdSetDescriptorSet(*m->ActiveCmd, sd);
		}
		m->Core.CmdSetPipeline(*m->ActiveCmd, *Pipeline);
	}

private:
	Binding* Find(const std::string& n) { for (Binding& b : Bindings) if (b.name == n) return &b; return nullptr; }

	void RefreshDescriptor(Binding& b)
	{
		if (b.kind == Kind::CBV) { b.desc = Cbv.view; b.ownsDesc = false; return; }
		if (b.kind == Kind::Sampler) { if (b.samp) { auto it = m->Samplers.find(b.samp); b.desc = it != m->Samplers.end() ? it->second : nullptr; } return; }
		void* res = b.tex ? (void*)b.tex : (void*)b.buf;
		if (res == b.last && b.desc) return;
		if (b.ownsDesc && b.desc) { m->Core.DestroyDescriptor(b.desc); b.desc = nullptr; }
		b.last = res;
		if (b.kind == Kind::TexSRV && b.tex)
		{
			auto it = m->Textures.find(b.tex);
			if (it == m->Textures.end() || !it->second.texture) return;
			const nri::TextureDesc& td = m->Core.GetTextureDesc(*it->second.texture);
			nri::TextureViewDesc tvd = {}; tvd.texture = it->second.texture; tvd.type = nri::TextureView::TEXTURE; tvd.format = td.format; tvd.mipNum = nri::REMAINING; tvd.layerNum = nri::REMAINING;
			nri::Descriptor* d = nullptr; if (m->Core.CreateTextureView(tvd, d) == nri::Result::SUCCESS) { b.desc = d; b.ownsDesc = true; }
		}
		else if (b.kind == Kind::BufSRV && b.buf)
		{
			auto it = m->Buffers.find(b.buf);
			if (it == m->Buffers.end() || !it->second.buffer) return;
			nri::BufferViewDesc bvd = {}; bvd.buffer = it->second.buffer; bvd.type = nri::BufferView::STRUCTURED_BUFFER; bvd.offset = 0;
			bvd.size = static_cast<uint64_t>(b.buf->NumElements) * b.buf->ElementSize; bvd.structureStride = b.buf->ElementSize ? b.buf->ElementSize : 4;
			nri::Descriptor* d = nullptr; if (m->Core.CreateBufferView(bvd, d) == nri::Result::SUCCESS) { b.desc = d; b.ownsDesc = true; }
		}
	}

	void Build(const GraphicsPipelineDesc& desc)
	{
		if (!m->Device) return;
		std::string verr, perr;
		const std::wstring vsW(desc.VertexEntryPoint.begin(), desc.VertexEntryPoint.end());
		const std::wstring psW(desc.PixelEntryPoint.begin(), desc.PixelEntryPoint.end());
		std::ifstream f(desc.ShaderPath, std::ios::binary);
		if (!f.good()) return;
		std::stringstream ss; ss << f.rdbuf(); const std::string src = ss.str();
		VsDxil = CompileHLSLToDXIL(src.data(), src.size(), desc.ShaderPath.c_str(), vsW.c_str(), L"vs_6_5", verr);
		PsDxil = CompileHLSLToDXIL(src.data(), src.size(), desc.ShaderPath.c_str(), psW.c_str(), L"ps_6_5", perr);
		if (VsDxil.empty() || PsDxil.empty()) return;
		VsEntry = desc.VertexEntryPoint; PsEntry = desc.PixelEntryPoint;

		// Bindings -> descriptor set ranges (resource set + sampler set).
		std::vector<nri::DescriptorRangeDesc> resR, sampR;
		const nri::StageBits gfxStages = nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;
		auto addRes = [&](const std::string& name, uint32_t reg, Kind k, nri::DescriptorType dt) {
			Binding b; b.name = name; b.reg = reg; b.kind = k; b.setIndex = 0; b.rangeIndex = (uint32_t)resR.size(); Bindings.push_back(b);
			nri::DescriptorRangeDesc r = {}; r.baseRegisterIndex = reg; r.descriptorNum = 1; r.descriptorType = dt; r.shaderStages = gfxStages; resR.push_back(r);
		};
		for (const auto& t : desc.TextureBindings) addRes(t.Name, t.Slot, Kind::TexSRV, nri::DescriptorType::TEXTURE);
		for (const auto& bb : desc.BufferBindings) addRes(bb.Name, bb.Slot, Kind::BufSRV, nri::DescriptorType::STRUCTURED_BUFFER);
		if (desc.ConstantBufferSize > 0)
		{
			addRes("$Globals", desc.ConstantBufferBinding, Kind::CBV, nri::DescriptorType::CONSTANT_BUFFER);
			const uint32_t aligned = (desc.ConstantBufferSize + 255u) & ~255u;
			if (m->CreateBoundBuffer(aligned, 0, nri::BufferUsageBits::CONSTANT_BUFFER, nri::MemoryLocation::HOST_UPLOAD, Cbv.buffer, Cbv.memory))
			{
				Cbv.size = aligned;
				nri::BufferViewDesc bvd = {}; bvd.buffer = Cbv.buffer; bvd.type = nri::BufferView::CONSTANT_BUFFER; bvd.offset = 0; bvd.size = aligned;
				m->Core.CreateBufferView(bvd, Cbv.view);
				if (Binding* cb = Find("$Globals")) { cb->desc = Cbv.view; }
			}
		}
		const uint32_t samplerSetIndex = 1;
		for (const auto& s : desc.SamplerBindings)
		{
			Binding b; b.name = s.Name; b.reg = s.Slot; b.kind = Kind::Sampler; b.setIndex = samplerSetIndex; b.rangeIndex = (uint32_t)sampR.size(); Bindings.push_back(b);
			nri::DescriptorRangeDesc r = {}; r.baseRegisterIndex = s.Slot; r.descriptorNum = 1; r.descriptorType = nri::DescriptorType::SAMPLER; r.shaderStages = gfxStages; sampR.push_back(r);
		}

		std::vector<nri::DescriptorSetDesc> sets;
		if (!resR.empty()) { nri::DescriptorSetDesc d = {}; d.registerSpace = 0; d.ranges = resR.data(); d.rangeNum = (uint32_t)resR.size(); sets.push_back(d); }
		if (!sampR.empty()) { nri::DescriptorSetDesc d = {}; d.registerSpace = 0; d.ranges = sampR.data(); d.rangeNum = (uint32_t)sampR.size(); sets.push_back(d); }
		// fix sampler set index if there is no resource set
		if (resR.empty()) for (Binding& b : Bindings) if (b.kind == Kind::Sampler) b.setIndex = 0;

		nri::PipelineLayoutDesc pld = {}; pld.rootRegisterSpace = 0; pld.descriptorSets = sets.empty() ? nullptr : sets.data(); pld.descriptorSetNum = (uint32_t)sets.size(); pld.shaderStages = gfxStages;
		if (m->Core.CreatePipelineLayout(*m->Device, pld, Layout) != nri::Result::SUCCESS) return;

		if (!sets.empty())
		{
			nri::DescriptorPoolDesc pd = {}; pd.descriptorSetMaxNum = (uint32_t)sets.size();
			pd.textureMaxNum = (uint32_t)desc.TextureBindings.size(); pd.structuredBufferMaxNum = (uint32_t)desc.BufferBindings.size();
			pd.constantBufferMaxNum = desc.ConstantBufferSize > 0 ? 1u : 0u; pd.samplerMaxNum = (uint32_t)desc.SamplerBindings.size();
			if (m->Core.CreateDescriptorPool(*m->Device, pd, Pool) != nri::Result::SUCCESS) return;
			Sets.resize(sets.size(), nullptr);
			for (uint32_t s = 0; s < sets.size(); ++s) m->Core.AllocateDescriptorSets(*Pool, *Layout, s, &Sets[s], 1, 0);
		}

		// Vertex input
		std::vector<nri::VertexAttributeDesc> attrs;
		for (const auto& ve : desc.VertexElements)
		{
			nri::VertexAttributeDesc a = {}; a.offset = ve.Offset; a.format = ToNRIVertexFormat(ve.Format); a.streamIndex = 0;
			a.d3d.semanticName = ve.SemanticName.c_str(); a.d3d.semanticIndex = ve.SemanticIndex;
			attrs.push_back(a);
		}
		nri::VertexStreamDesc stream = {}; stream.bindingSlot = 0; stream.stepRate = nri::VertexStreamStepRate::PER_VERTEX;
		nri::VertexInputDesc vinput = {}; vinput.attributes = attrs.data(); vinput.attributeNum = (uint8_t)attrs.size(); vinput.streams = &stream; vinput.streamNum = 1;

		// Output merger
		std::vector<nri::ColorAttachmentDesc> colors;
		for (ETextureFormat cf : desc.ColorFormats)
		{
			nri::ColorAttachmentDesc c = {}; c.format = ToNRIFormat(cf);
			c.colorWriteMask = nri::ColorWriteBits::R | nri::ColorWriteBits::G | nri::ColorWriteBits::B | nri::ColorWriteBits::A;
			c.blendEnabled = false;
			colors.push_back(c);
		}
		nri::OutputMergerDesc om = {}; om.colors = colors.empty() ? nullptr : colors.data(); om.colorNum = (uint32_t)colors.size();
		if (desc.DepthFormat.has_value()) { om.depthStencilFormat = ToNRIFormat(*desc.DepthFormat); om.depth.compareOp = desc.bDepthEnable ? nri::CompareOp::LESS_EQUAL : nri::CompareOp::NONE; om.depth.write = desc.bDepthWriteEnable; }

		nri::RasterizationDesc rast = {}; rast.fillMode = nri::FillMode::SOLID; rast.cullMode = desc.bCullBackFaces ? nri::CullMode::BACK : nri::CullMode::NONE; rast.frontCounterClockwise = false;
		nri::InputAssemblyDesc ia = {}; ia.topology = desc.bTriangleStrip ? nri::Topology::TRIANGLE_STRIP : nri::Topology::TRIANGLE_LIST;

		nri::ShaderDesc shaders[2] = {};
		shaders[0].stage = nri::StageBits::VERTEX_SHADER; shaders[0].bytecode = VsDxil.data(); shaders[0].size = VsDxil.size(); shaders[0].entryPointName = VsEntry.c_str();
		shaders[1].stage = nri::StageBits::FRAGMENT_SHADER; shaders[1].bytecode = PsDxil.data(); shaders[1].size = PsDxil.size(); shaders[1].entryPointName = PsEntry.c_str();

		nri::GraphicsPipelineDesc gpd = {};
		gpd.pipelineLayout = Layout;
		gpd.vertexInput = attrs.empty() ? nullptr : &vinput;
		gpd.inputAssembly = ia;
		gpd.rasterization = rast;
		gpd.outputMerger = om;
		gpd.shaders = shaders; gpd.shaderNum = 2;
		m->Core.CreateGraphicsPipeline(*m->Device, gpd, Pipeline);
	}

	NRIBackend::Impl* m = nullptr;
	std::vector<Binding> Bindings;
	CbvState Cbv;
	std::vector<uint8_t> VsDxil, PsDxil;
	std::string VsEntry, PsEntry;
	nri::PipelineLayout* Layout = nullptr;
	nri::Pipeline* Pipeline = nullptr;
	nri::DescriptorPool* Pool = nullptr;
	std::vector<nri::DescriptorSet*> Sets;
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

// Provided so NRI does NOT DebugBreak/abort on validation errors during bring-up:
// a failing resource (e.g. an unsupported binding shape) is reported and the
// creation call returns an error, letting the editor skip that pass and proceed.
static void NRI_CALL NRIAbortCallback(void* /*userArg*/) { /* no-op: do not break */ }

NRIBackend::NRIBackend() : m(std::make_unique<Impl>())
{
	using nri::CoreInterface;
	using nri::HelperInterface;
	using nri::SwapChainInterface;

	nri::DeviceCreationDesc desc = {};
	desc.graphicsAPI = nri::GraphicsAPI::D3D12;
	desc.enableNRIValidation = true;            // embedded NRI-specific validation
	desc.enableGraphicsAPIValidation = false;   // D3D12 debug layer (opt-in later)
	desc.callbackInterface.MessageCallback = NRIMessageCallback;
	desc.callbackInterface.AbortExecution = NRIAbortCallback;

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
	nri::nriGetInterface(*m->Device, NRI_INTERFACE(SwapChainInterface), &m->SwapChainI); // non-fatal
	{
		using nri::StreamerInterface;
		using nri::ImguiInterface;
		nri::nriGetInterface(*m->Device, NRI_INTERFACE(StreamerInterface), &m->StreamerI);
		nri::nriGetInterface(*m->Device, NRI_INTERFACE(ImguiInterface), &m->ImguiI);
	}

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

	// Shader smoke: compile a trivial compute shader HLSL -> DXIL via DXC, to
	// validate the bytecode path that the compute pipeline will consume.
	size_t shaderBytes = 0;
	{
		static const char* kCS =
			"RWStructuredBuffer<uint> OutBuf : register(u0);\n"
			"[numthreads(1,1,1)] void main(uint3 id : SV_DispatchThreadID) { OutBuf[id.x] = 0xCAFEu; }\n";
		std::string err;
		std::vector<uint8_t> dxil = CompileHLSLToDXIL(kCS, strlen(kCS), L"nri_smoke.cs", L"main", L"cs_6_5", err);
		shaderBytes = dxil.size();
	}

	// Texture smoke: create + bind + free a 64x64 RGBA16F UAV texture.
	bool texSmokeOk = false;
	{
		nri::TextureDesc td = {};
		td.type = nri::TextureType::TEXTURE_2D;
		td.usage = nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::SHADER_RESOURCE_STORAGE;
		td.format = nri::Format::RGBA16_SFLOAT;
		td.width = 64; td.height = 64; td.depth = 1; td.mipNum = 1; td.layerNum = 1; td.sampleNum = 1;
		nri::Texture* t = nullptr; std::vector<nri::Memory*> tm;
		texSmokeOk = m->CreateBoundTexture(td, t, tm);
		m->FreeTexture(t, tm);
	}

	// End-to-end compute dispatch (pipeline layout + compute pipeline + dispatch
	// + copy + readback verify).
	std::string computeResult = (m->CmdBuffer && m->GraphicsQueue) ? m->RunComputeSmoke() : std::string("skipped");

	// By-name binding adapter: drive the public ComputePipelineStateObject
	// interface (BindUAV + InitCS) and confirm it builds an NRI compute pipeline.
	bool psoInitOk = false;
	{
		static const char* kPsoCS =
			"RWStructuredBuffer<uint> OutBuf : register(u0);\n"
			"[numthreads(1,1,1)] void main(uint3 id : SV_DispatchThreadID) { OutBuf[id.x] = 1u; }\n";
		const std::wstring path = L"nri_pso_smoke.hlsl";
		{ std::ofstream of(path, std::ios::binary); of.write(kPsoCS, (std::streamsize)strlen(kPsoCS)); }
		NRIComputePSO pso(m.get());
		pso.BindUAV("OutBuf", 0);
		psoInitOk = pso.InitCS(path, "main"); // pipeline is created lazily on first Apply
	}

	// Full frame path: BeginFrame -> TransitionBuffer -> PSO Apply -> Dispatch ->
	// TransitionBuffer -> copy -> EndFrame, driving the public renderer interface
	// (the shader writes 1u into a UAV structured buffer). Verifies that the frame
	// command-buffer lifecycle + the by-name ComputePSO actually run on the GPU.
	std::string framePsoResult = "skipped";
	if (m->GraphicsQueue && m->CmdBuffer && psoInitOk)
	{
		const uint32_t N = 4;
		const uint64_t bufSize = N * sizeof(uint32_t);
		BufferCreateDesc bcd = {};
		bcd.NumElements = N; bcd.ElementSize = sizeof(uint32_t);
		bcd.bAllowUnorderedAccess = true; bcd.Shape = EBufferShape::Structured;
		std::shared_ptr<Buffer> outWrap = CreateBuffer(bcd);

		nri::Buffer* rb = nullptr; std::vector<nri::Memory*> rbMem;
		m->CreateBoundBuffer(bufSize, 0, nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_READBACK, rb, rbMem);

		std::shared_ptr<ComputePipelineStateObject> pso = CreateComputePipelineStateObject();
		pso->BindUAV("OutBuf", 0);
		if (outWrap && rb && pso->InitCS(L"nri_pso_smoke.hlsl", "main"))
		{
			pso->SetBufferUAV("OutBuf", outWrap.get());
			BeginFrame();
			TransitionBuffer(outWrap.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
			pso->Apply();
			Dispatch(N, 1, 1);
			TransitionBuffer(outWrap.get(), EResourceState::UnorderedAccess, EResourceState::CopySource);
			auto oit = m->Buffers.find(outWrap.get());
			if (m->ActiveCmd && oit != m->Buffers.end() && oit->second.buffer)
				m->Core.CmdCopyBuffer(*m->ActiveCmd, *rb, 0, *oit->second.buffer, 0, bufSize);
			EndFrame();

			uint32_t* mp = (uint32_t*)m->Core.MapBuffer(*rb, 0, bufSize);
			uint32_t v0 = mp ? mp[0] : 0u;
			uint32_t vN = mp ? mp[N - 1] : 0u;
			m->Core.UnmapBuffer(*rb);
			char b[48];
			const bool ok = mp && v0 == 1u && vN == 1u;
			_snprintf_s(b, _TRUNCATE, ok ? "PASS(%u)" : "FAIL(%u)", v0);
			framePsoResult = b;
		}
		else
		{
			framePsoResult = "FAIL(init)";
		}
		m->FreeBuffer(rb, rbMem);
		if (outWrap)
		{
			auto oit = m->Buffers.find(outWrap.get());
			if (oit != m->Buffers.end()) { m->FreeBuffer(oit->second.buffer, oit->second.memory); m->Buffers.erase(oit); }
		}
	}

	// CBV path: a compute PSO with a constant buffer (root CBV) + UAV output.
	// The kernel writes cbuffer.mul * 7; with mul=6 the result must be 42.
	std::string cbvResult = "skipped";
	if (m->GraphicsQueue && m->CmdBuffer)
	{
		static const char* kCbvCS =
			"cbuffer C : register(b0) { uint mul; };\n"
			"RWStructuredBuffer<uint> O : register(u0);\n"
			"[numthreads(1,1,1)] void main(uint3 id : SV_DispatchThreadID) { O[id.x] = mul * 7u; }\n";
		const std::wstring path = L"nri_cbv_smoke.hlsl";
		{ std::ofstream of(path, std::ios::binary); of.write(kCbvCS, (std::streamsize)strlen(kCbvCS)); }

		const uint32_t N = 4;
		const uint64_t bufSize = N * sizeof(uint32_t);
		BufferCreateDesc bcd = {};
		bcd.NumElements = N; bcd.ElementSize = sizeof(uint32_t);
		bcd.bAllowUnorderedAccess = true; bcd.Shape = EBufferShape::Structured;
		std::shared_ptr<Buffer> outWrap = CreateBuffer(bcd);
		nri::Buffer* rb = nullptr; std::vector<nri::Memory*> rbMem;
		m->CreateBoundBuffer(bufSize, 0, nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_READBACK, rb, rbMem);

		std::shared_ptr<ComputePipelineStateObject> pso = CreateComputePipelineStateObject();
		pso->BindCBV("C", 0, 16);
		pso->BindUAV("O", 0);
		if (outWrap && rb && pso->InitCS(path, "main"))
		{
			uint32_t cbData[4] = { 6u, 0u, 0u, 0u };
			pso->SetCBVValue("C", cbData);
			pso->SetBufferUAV("O", outWrap.get());
			BeginFrame();
			TransitionBuffer(outWrap.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
			pso->Apply();
			Dispatch(N, 1, 1);
			TransitionBuffer(outWrap.get(), EResourceState::UnorderedAccess, EResourceState::CopySource);
			auto oit = m->Buffers.find(outWrap.get());
			if (m->ActiveCmd && oit != m->Buffers.end() && oit->second.buffer)
				m->Core.CmdCopyBuffer(*m->ActiveCmd, *rb, 0, *oit->second.buffer, 0, bufSize);
			EndFrame();
			uint32_t* mp = (uint32_t*)m->Core.MapBuffer(*rb, 0, bufSize);
			uint32_t v0 = mp ? mp[0] : 0u;
			m->Core.UnmapBuffer(*rb);
			char b[48];
			_snprintf_s(b, _TRUNCATE, (v0 == 42u) ? "PASS(%u)" : "FAIL(%u)", v0);
			cbvResult = b;
		}
		else cbvResult = "FAIL(init)";
		m->FreeBuffer(rb, rbMem);
		if (outWrap)
		{
			auto oit = m->Buffers.find(outWrap.get());
			if (oit != m->Buffers.end()) { m->FreeBuffer(oit->second.buffer, oit->second.memory); m->Buffers.erase(oit); }
		}
	}

	char info[960];
	_snprintf_s(info, _TRUNCATE,
		"device ok [%s], rtTier=%u sm=%u queue=%s bufferSmoke=%s texSmoke=%s cmd=%s submitSmoke=%s shaderDXIL=%zuB compute=%s psoInit=%s framePSO=%s cbv=%s",
		dd.adapterDesc.name, (unsigned)dd.tiers.rayTracing, (unsigned)dd.shaderModel,
		m->GraphicsQueue ? "ok" : "null", bufferSmokeOk ? "PASS" : "FAIL", texSmokeOk ? "PASS" : "FAIL",
		m->CmdBuffer ? "ok" : "null", submitSmokeOk ? "PASS" : "FAIL", shaderBytes, computeResult.c_str(), psoInitOk ? "PASS" : "FAIL", framePsoResult.c_str(), cbvResult.c_str());
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

		for (auto& kv : m->Textures)
		{
			if (kv.second.texture)
				m->Core.DestroyTexture(kv.second.texture);
			for (nri::Memory* mem : kv.second.memory)
				if (mem) m->Core.FreeMemory(mem);
		}
		m->Textures.clear();

		for (auto& kv : m->Samplers) if (kv.second) m->Core.DestroyDescriptor(kv.second);
		m->Samplers.clear();

		if (m->Imgui) { m->ImguiI.DestroyImgui(m->Imgui); m->Imgui = nullptr; }
		if (m->Streamer) { m->StreamerI.DestroyStreamer(m->Streamer); m->Streamer = nullptr; }
		for (nri::Descriptor* v : m->BackBufferViews) if (v) m->Core.DestroyDescriptor(v);
		for (nri::Fence* fc : m->AcquireSem) if (fc) m->Core.DestroyFence(fc);
		for (nri::Fence* fc : m->ReleaseSem) if (fc) m->Core.DestroyFence(fc);
		m->BackBufferViews.clear(); m->AcquireSem.clear(); m->ReleaseSem.clear();
		m->BackBufferWrappers.clear(); m->BackBuffers.clear();
		if (m->FrameFence) { m->Core.DestroyFence(m->FrameFence); m->FrameFence = nullptr; }
		if (m->SwapChain && m->SwapChainI.DestroySwapChain) { m->SwapChainI.DestroySwapChain(m->SwapChain); m->SwapChain = nullptr; }

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
void NRIBackend::BeginFrame()
{
	if (!m->Device || !m->CmdAllocator || !m->CmdBuffer || m->ActiveCmd)
		return;
	m->FrameHasBackbuffer = false;
	m->HasPendingClear = false;
	m->BBLayout = nri::Layout::UNDEFINED;
	m->CurrentWindowRT = nullptr;

	m->Core.ResetCommandAllocator(*m->CmdAllocator);
	if (m->Core.BeginCommandBuffer(*m->CmdBuffer, nullptr) != nri::Result::SUCCESS)
		return;
	m->ActiveCmd = m->CmdBuffer;

	if (m->SwapChain && !m->BackBuffers.empty())
	{
		const uint32_t n = (uint32_t)m->BackBuffers.size();
		nri::Fence* acq = m->AcquireSem[m->SwapFrameIndex % n];
		uint32_t idx = 0;
		if (m->SwapChainI.AcquireNextTexture(*m->SwapChain, *acq, idx) == nri::Result::SUCCESS && idx < n)
		{
			m->CurrentBackBuffer = idx;
			m->CurAcquire = acq;
			m->FrameHasBackbuffer = true;
		}
	}
}
void NRIBackend::EndFrame()
{
	if (!m->ActiveCmd)
		return;
	if (m->Streamer)
		m->StreamerI.EndStreamerFrame(*m->Streamer);

	if (m->FrameHasBackbuffer)
	{
		// Make sure the backbuffer ends in PRESENT layout even if nothing drew to it.
		if (m->BBLayout != nri::Layout::PRESENT)
			m->TransitionBackbuffer(nri::AccessBits::NONE, nri::Layout::PRESENT, nri::StageBits::NONE);

		m->Core.EndCommandBuffer(*m->ActiveCmd);

		nri::Fence* release = m->ReleaseSem[m->CurrentBackBuffer];
		nri::FenceSubmitDesc waitAcq = {}; waitAcq.fence = m->CurAcquire; waitAcq.stages = nri::StageBits::ALL;
		nri::FenceSubmitDesc sigRel = {}; sigRel.fence = release;
		nri::FenceSubmitDesc sigFrame = {}; sigFrame.fence = m->FrameFence; sigFrame.value = 1 + m->SwapFrameIndex;
		nri::FenceSubmitDesc signals[2] = { sigRel, sigFrame };
		nri::CommandBuffer* cbs[1] = { m->ActiveCmd };
		nri::QueueSubmitDesc qs = {};
		qs.waitFences = &waitAcq; qs.waitFenceNum = 1;
		qs.commandBuffers = cbs; qs.commandBufferNum = 1;
		qs.signalFences = signals; qs.signalFenceNum = 2;
		m->Core.QueueSubmit(*m->GraphicsQueue, qs);
		m->SwapChainI.QueuePresent(*m->SwapChain, *release);
		m->SwapFrameIndex++;
		m->Core.Wait(*m->FrameFence, m->SwapFrameIndex); // synchronous pacing
	}
	else
	{
		m->Core.EndCommandBuffer(*m->ActiveCmd);
		nri::FenceSubmitDesc sf = {};
		sf.fence = m->Fence; sf.value = ++m->FenceValue; sf.stages = nri::StageBits::ALL;
		nri::CommandBuffer* cbs[1] = { m->ActiveCmd };
		nri::QueueSubmitDesc qs = {};
		qs.commandBuffers = cbs; qs.commandBufferNum = 1;
		qs.signalFences = &sf; qs.signalFenceNum = 1;
		m->Core.QueueSubmit(*m->GraphicsQueue, qs);
		m->Core.Wait(*m->Fence, m->FenceValue);
	}
	m->ActiveCmd = nullptr;
}
void NRIBackend::WaitForGpu() { NRI_TODO(); }
void NRIBackend::EmitGpuCrashMarker(const char*) { NRI_TODO(); }
const std::string& NRIBackend::GetErrorString() const { return ErrorString; }
void NRIBackend::ClearErrorString() { ErrorString.clear(); }
uint64_t NRIBackend::GetTimestampFrequency() const { return 0; }
uint32_t NRIBackend::GetFrameCount() const { return 0; }
uint32_t NRIBackend::GetCurrentFrameIndex() const { return m->CurrentBackBuffer; }
DX12Backend* NRIBackend::AsDX12Backend() { return nullptr; }

// === Resource creation ====================================================
std::shared_ptr<Texture> NRIBackend::CreateTexture2D(const TextureCreateDesc& desc)
{
	if (!m->Device)
		return nullptr;
	nri::TextureDesc td = {};
	td.type = nri::TextureType::TEXTURE_2D;
	td.usage = ToNRITextureUsage(desc.Usage);
	td.format = ToNRIFormat(desc.Format);
	td.width = static_cast<nri::Dim_t>(desc.Width);
	td.height = static_cast<nri::Dim_t>(desc.Height);
	td.depth = 1;
	td.mipNum = static_cast<nri::Dim_t>(desc.MipLevels > 0 ? desc.MipLevels : 1);
	td.layerNum = 1;
	td.sampleNum = 1;

	nri::Texture* tex = nullptr;
	std::vector<nri::Memory*> mem;
	if (!m->CreateBoundTexture(td, tex, mem))
	{
		ErrorString = "CreateTexture2D: CreateBoundTexture failed";
		return nullptr;
	}

	auto wrapper = std::make_shared<Texture>();
	wrapper->Width = desc.Width;
	wrapper->Height = desc.Height;
	wrapper->MipLevels = td.mipNum;
	wrapper->Format = desc.Format;
	wrapper->Usage = desc.Usage;

	Impl::TextureAlloc alloc;
	alloc.texture = tex;
	alloc.memory = std::move(mem);
	m->Textures[wrapper.get()] = std::move(alloc);
	return wrapper;
}
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
std::shared_ptr<Sampler> NRIBackend::CreateSampler(const SamplerCreateDesc& desc)
{
	if (!m->Device)
		return nullptr;
	const nri::Filter f = (desc.Filter == ESamplerFilter::Anisotropic || desc.Filter == ESamplerFilter::Linear) ? nri::Filter::LINEAR : nri::Filter::NEAREST;
	auto addr = [](ESamplerAddressMode a) { return a == ESamplerAddressMode::Clamp ? nri::AddressMode::CLAMP_TO_EDGE : nri::AddressMode::REPEAT; };

	nri::SamplerDesc sd = {};
	sd.filters.min = f; sd.filters.mag = f; sd.filters.mip = f;
	sd.anisotropy = (desc.Filter == ESamplerFilter::Anisotropic) ? (uint8_t)std::max(1u, desc.MaxAnisotropy) : (uint8_t)1;
	sd.mipBias = desc.MipLODBias;
	sd.mipMin = desc.MinLOD;
	sd.mipMax = desc.MaxLOD;
	sd.addressModes.u = addr(desc.AddressU);
	sd.addressModes.v = addr(desc.AddressV);
	sd.addressModes.w = addr(desc.AddressW);
	sd.compareOp = nri::CompareOp::NONE;

	nri::Descriptor* samplerDesc = nullptr;
	if (m->Core.CreateSampler(*m->Device, sd, samplerDesc) != nri::Result::SUCCESS || !samplerDesc)
	{
		ErrorString = "CreateSampler failed";
		return nullptr;
	}
	auto wrapper = std::make_shared<Sampler>();
	m->Samplers[wrapper.get()] = samplerDesc;
	return wrapper;
}
std::shared_ptr<Texture> NRIBackend::CreateTextureFromFile(const std::wstring&, bool) { NRI_TODO(); return nullptr; }
std::shared_ptr<Texture> NRIBackend::CreateTexture3D(
	ETextureFormat format, ETextureUsageFlags usage, EInitialResourceState /*initialState*/,
	int width, int height, int depth, int mipLevels)
{
	if (!m->Device)
		return nullptr;
	nri::TextureDesc td = {};
	td.type = nri::TextureType::TEXTURE_3D;
	td.usage = ToNRITextureUsage(usage);
	td.format = ToNRIFormat(format);
	td.width = static_cast<nri::Dim_t>(width);
	td.height = static_cast<nri::Dim_t>(height);
	td.depth = static_cast<nri::Dim_t>(depth);
	td.mipNum = static_cast<nri::Dim_t>(mipLevels > 0 ? mipLevels : 1);
	td.layerNum = 1;
	td.sampleNum = 1;

	nri::Texture* tex = nullptr;
	std::vector<nri::Memory*> mem;
	if (!m->CreateBoundTexture(td, tex, mem))
	{
		ErrorString = "CreateTexture3D: CreateBoundTexture failed";
		return nullptr;
	}

	auto wrapper = std::make_shared<Texture>();
	wrapper->Width = static_cast<uint32_t>(width);
	wrapper->Height = static_cast<uint32_t>(height);
	wrapper->MipLevels = td.mipNum;
	wrapper->Format = format;
	wrapper->Usage = usage;

	Impl::TextureAlloc alloc;
	alloc.texture = tex;
	alloc.memory = std::move(mem);
	m->Textures[wrapper.get()] = std::move(alloc);
	return wrapper;
}
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
std::shared_ptr<ComputePipelineStateObject> NRIBackend::CreateComputePipelineStateObject()
{
	return std::make_shared<NRIComputePSO>(m.get());
}
ShaderBytecode NRIBackend::CreateShader(const std::wstring& fileName, const std::string& entryPoint, const std::string& target)
{
	ShaderBytecode out;
	std::ifstream f(fileName, std::ios::binary);
	if (!f.good())
	{
		ErrorString = "CreateShader: cannot open shader file";
		return out;
	}
	std::stringstream ss;
	ss << f.rdbuf();
	const std::string src = ss.str();
	const std::wstring entryW(entryPoint.begin(), entryPoint.end());
	const std::wstring targetW(target.begin(), target.end());
	std::string err;
	out.Data = CompileHLSLToDXIL(src.data(), src.size(), fileName.c_str(), entryW.c_str(), targetW.c_str(), err);
	if (out.Data.empty())
		ErrorString = "CreateShader: " + err;
	return out;
}
void NRIBackend::ResetDynamicResources() { NRI_TODO(); }

// === Swapchain / present ==================================================
void NRIBackend::CreateSwapChainForWindow(WindowHandle window, uint32_t width, uint32_t height, ETextureFormat /*format*/)
{
	if (!m->Device || !m->GraphicsQueue || !m->SwapChainI.CreateSwapChain || !window.PlatformHandle)
	{
		ErrorString = "CreateSwapChain: prerequisites missing";
		return;
	}

	nri::SwapChainDesc scd = {};
	scd.window.windows.hwnd = window.PlatformHandle;
	scd.queue = m->GraphicsQueue;
	scd.width = static_cast<nri::Dim_t>(width);
	scd.height = static_cast<nri::Dim_t>(height);
	scd.textureNum = 3;
	scd.format = nri::SwapChainFormat::BT709_G22_8BIT;
	if (m->SwapChainI.CreateSwapChain(*m->Device, scd, m->SwapChain) != nri::Result::SUCCESS || !m->SwapChain)
	{
		ErrorString = "CreateSwapChain failed";
		return;
	}

	uint32_t num = 0;
	nri::Texture* const* texs = m->SwapChainI.GetSwapChainTextures(*m->SwapChain, num);
	for (uint32_t i = 0; i < num; ++i)
	{
		m->BackBuffers.push_back(texs[i]);
		const nri::TextureDesc& tdsc = m->Core.GetTextureDesc(*texs[i]);
		m->SwapFormat = tdsc.format;

		nri::TextureViewDesc tvd = {};
		tvd.texture = texs[i];
		tvd.type = nri::TextureView::COLOR_ATTACHMENT;
		tvd.format = tdsc.format;
		tvd.mipNum = 1;
		tvd.layerNum = 1;
		nri::Descriptor* view = nullptr;
		m->Core.CreateTextureView(tvd, view);
		m->BackBufferViews.push_back(view);

		nri::Fence* acq = nullptr; nri::Fence* rel = nullptr;
		m->Core.CreateFence(*m->Device, nri::SWAPCHAIN_SEMAPHORE, acq);
		m->Core.CreateFence(*m->Device, nri::SWAPCHAIN_SEMAPHORE, rel);
		m->AcquireSem.push_back(acq);
		m->ReleaseSem.push_back(rel);

		auto w = std::make_shared<Texture>();
		w->Width = width; w->Height = height;
		w->Format = ETextureFormat::BGRA8Unorm;
		w->Usage = TextureUsage_RenderTarget;
		m->BackBufferWrappers.push_back(w);
	}
	if (!m->FrameFence)
		m->Core.CreateFence(*m->Device, 0, m->FrameFence);

	char info[256];
	_snprintf_s(info, _TRUNCATE, "swapchain ok: backbuffers=%u format=%d", num, (int)m->SwapFormat);
	ErrorString = info;
	OutputDebugStringA("[NRI] ");
	OutputDebugStringA(info);
	OutputDebugStringA("\n");
}
std::shared_ptr<Texture> NRIBackend::GetSwapChainTexture(uint32_t bufferIndex)
{
	if (bufferIndex < m->BackBufferWrappers.size())
		return m->BackBufferWrappers[bufferIndex];
	return nullptr;
}
bool NRIBackend::CaptureTexture(Texture*, CapturedImage&, EResourceState) { NRI_TODO(); return false; }
Texture* NRIBackend::GetCurrentWindowRenderTarget()
{
	if (m->FrameHasBackbuffer && m->CurrentBackBuffer < m->BackBufferWrappers.size())
		return m->BackBufferWrappers[m->CurrentBackBuffer].get();
	return nullptr;
}
void NRIBackend::PrepareWindowRenderTarget(Texture* renderTarget) { m->CurrentWindowRT = renderTarget; }
void NRIBackend::FinalizeWindowRenderTarget(Texture*)
{
	// Leave the backbuffer in PRESENT layout for the present in EndFrame.
	if (m->FrameHasBackbuffer && m->BBLayout != nri::Layout::PRESENT)
		m->TransitionBackbuffer(nri::AccessBits::NONE, nri::Layout::PRESENT, nri::StageBits::NONE);
}
void NRIBackend::RequestWindowCapture(const std::wstring&) { NRI_TODO(); }
bool NRIBackend::ConsumeWindowCaptureResult(std::wstring*, bool*, std::wstring*) { NRI_TODO(); return false; }

// === ImGui ================================================================
void NRIBackend::InitializeImGuiBackend(WindowHandle, ETextureFormat)
{
	if (!m->Device || !m->ImguiI.CreateImgui || !m->StreamerI.CreateStreamer)
	{
		ErrorString = "ImGui: NRI Imgui/Streamer interface unavailable";
		return;
	}
	ImGuiIO& io = ImGui::GetIO();
	io.BackendRendererName = "Corona_NRI";
	io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // dynamic-font textures managed by NRIImgui

	nri::StreamerDesc sd = {};
	sd.constantBufferMemoryLocation = nri::MemoryLocation::HOST_UPLOAD;
	sd.constantBufferSize = 1u << 16;
	sd.dynamicBufferMemoryLocation = nri::MemoryLocation::HOST_UPLOAD;
	sd.dynamicBufferDesc.usage = nri::BufferUsageBits::VERTEX_BUFFER | nri::BufferUsageBits::INDEX_BUFFER;
	sd.queuedFrameNum = 2;
	if (m->StreamerI.CreateStreamer(*m->Device, sd, m->Streamer) != nri::Result::SUCCESS)
	{
		ErrorString = "ImGui: CreateStreamer failed";
		return;
	}
	nri::ImguiDesc id = {};
	id.descriptorPoolSize = 128;
	if (m->ImguiI.CreateImgui(*m->Device, id, m->Imgui) != nri::Result::SUCCESS)
		ErrorString = "ImGui: CreateImgui failed";
}
void NRIBackend::NewImGuiFrame() { /* ImGui::NewFrame() is driven by Corona; nothing NRI-specific here */ }
void NRIBackend::RenderImGuiDrawData(ImDrawData* drawData)
{
	if (!m->ActiveCmd || !m->Imgui || !m->Streamer || !m->FrameHasBackbuffer || !drawData)
		return;
	const uint32_t idx = m->CurrentBackBuffer;

	// 1) Stream + copy ImGui vertex/index/texture data (outside a render pass).
	nri::CopyImguiDataDesc copy = {};
	copy.drawLists = drawData->CmdLists.Data;
	copy.drawListNum = (uint32_t)drawData->CmdLists.Size;
	copy.textures = drawData->Textures ? drawData->Textures->Data : nullptr;
	copy.textureNum = drawData->Textures ? (uint32_t)drawData->Textures->Size : 0;
	m->ImguiI.CmdCopyImguiData(*m->ActiveCmd, *m->Streamer, *m->Imgui, copy);
	m->StreamerI.CmdCopyStreamedData(*m->ActiveCmd, *m->Streamer);

	// 2) Render pass on the backbuffer (clear if a ClearRenderTarget is pending).
	m->TransitionBackbuffer(nri::AccessBits::COLOR_ATTACHMENT, nri::Layout::COLOR_ATTACHMENT, nri::StageBits::ALL);
	nri::AttachmentDesc colorAtt = {};
	colorAtt.descriptor = m->BackBufferViews[idx];
	colorAtt.loadOp = m->HasPendingClear ? nri::LoadOp::CLEAR : nri::LoadOp::LOAD;
	colorAtt.storeOp = nri::StoreOp::STORE;
	if (m->HasPendingClear)
		colorAtt.clearValue.color.f = nri::Color32f{ m->PendingClear[0], m->PendingClear[1], m->PendingClear[2], m->PendingClear[3] };
	nri::RenderingDesc rd = {};
	rd.colors = &colorAtt; rd.colorNum = 1;
	m->Core.CmdBeginRendering(*m->ActiveCmd, rd);

	nri::DrawImguiDesc draw = {};
	draw.drawLists = copy.drawLists;
	draw.drawListNum = copy.drawListNum;
	draw.displaySize = { (nri::Dim_t)drawData->DisplaySize.x, (nri::Dim_t)drawData->DisplaySize.y };
	draw.hdrScale = 1.0f;
	draw.attachmentFormat = m->SwapFormat;
	draw.linearColor = false;
	m->ImguiI.CmdDrawImgui(*m->ActiveCmd, *m->Imgui, draw);

	m->Core.CmdEndRendering(*m->ActiveCmd);
	m->HasPendingClear = false;
}
void NRIBackend::ShutdownImGuiBackend()
{
	if (!m || !m->Device)
		return;
	if (m->Imgui) { m->ImguiI.DestroyImgui(m->Imgui); m->Imgui = nullptr; }
	if (m->Streamer) { m->StreamerI.DestroyStreamer(m->Streamer); m->Streamer = nullptr; }
}

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
void NRIBackend::SetRenderTarget(Texture* colorTarget, Texture*) { m->CurrentWindowRT = colorTarget; }
void NRIBackend::SetRenderTargets(Texture* const* colorTargets, uint32_t count, Texture*) { m->CurrentWindowRT = (count > 0 && colorTargets) ? colorTargets[0] : nullptr; }
void NRIBackend::ClearRenderTarget(Texture*, const float clearColor[4])
{
	m->PendingClear[0] = clearColor[0]; m->PendingClear[1] = clearColor[1];
	m->PendingClear[2] = clearColor[2]; m->PendingClear[3] = clearColor[3];
	m->HasPendingClear = true;
}
void NRIBackend::ClearDepth(Texture*, float) { NRI_TODO(); }
void NRIBackend::BindDefaultDescriptorHeaps() { NRI_TODO(); }
void NRIBackend::SetViewportAndScissor(uint32_t, uint32_t) { NRI_TODO(); }
void NRIBackend::DrawFullscreenQuad(VertexBuffer*) { NRI_TODO(); }
void NRIBackend::BindMeshBuffers(VertexBuffer*, IndexBuffer*) { NRI_TODO(); }
void NRIBackend::DrawIndexed(uint32_t, uint32_t, int32_t) { NRI_TODO(); }
void NRIBackend::DrawIndexedInstanced(uint32_t, uint32_t, uint32_t, int32_t, uint32_t) { NRI_TODO(); }
void NRIBackend::Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
	if (m->ActiveCmd)
	{
		nri::DispatchDesc d = { groupCountX, groupCountY, groupCountZ };
		m->Core.CmdDispatch(*m->ActiveCmd, d);
	}
}
void NRIBackend::ClearTextureUAVFloat(Texture*, const float[4]) { NRI_TODO(); }
void NRIBackend::ExecuteCurrentCommandList() { NRI_TODO(); }
void NRIBackend::BeginGpuMarker(uint64_t, const char*) { NRI_TODO(); }
void NRIBackend::EndGpuMarker() { NRI_TODO(); }
void NRIBackend::TransitionTexture(Texture*, EResourceState, EResourceState) { NRI_TODO(); }
void NRIBackend::TransitionBuffer(Buffer* buffer, EResourceState stateBefore, EResourceState stateAfter)
{
	if (!m->ActiveCmd || !buffer)
		return;
	auto it = m->Buffers.find(buffer);
	if (it == m->Buffers.end() || !it->second.buffer)
		return;
	nri::BufferBarrierDesc bb = {};
	bb.buffer = it->second.buffer;
	bb.before = ToAccessStage(stateBefore);
	bb.after = ToAccessStage(stateAfter);
	nri::BarrierDesc bd = {};
	bd.buffers = &bb; bd.bufferNum = 1;
	m->Core.CmdBarrier(*m->ActiveCmd, bd);
}
void NRIBackend::TransitionVertexBuffer(VertexBuffer*, EResourceState, EResourceState) { NRI_TODO(); }

// === Graphics pipelines (by-name binding) =================================
std::shared_ptr<GraphicsPipelineHandle> NRIBackend::CreateGraphicsPipeline(const GraphicsPipelineDesc& desc)
{
	return std::make_shared<NRIGraphicsPipeline>(m.get(), desc);
}
void NRIBackend::BindGraphicsPipeline(GraphicsPipelineHandle* pipeline) { m->CurrentGfx = pipeline; /* recorded inside the render pass in Stage 2 */ }
void NRIBackend::SetGraphicsPipelineConstantData(GraphicsPipelineHandle* pipeline, uint32_t, const void* data, uint32_t size) { if (auto* p = static_cast<NRIGraphicsPipeline*>(pipeline)) p->SetConstant(data, size); }
void NRIBackend::BindGraphicsPipelineTexture(GraphicsPipelineHandle* pipeline, const std::string& bindingName, Texture* texture) { if (auto* p = static_cast<NRIGraphicsPipeline*>(pipeline)) p->SetTexture(bindingName, texture); }
void NRIBackend::BindGraphicsPipelineBuffer(GraphicsPipelineHandle* pipeline, const std::string& bindingName, Buffer* buffer) { if (auto* p = static_cast<NRIGraphicsPipeline*>(pipeline)) p->SetBuffer(bindingName, buffer); }
void NRIBackend::BindGraphicsPipelineVertexBufferSRV(GraphicsPipelineHandle*, const std::string&, VertexBuffer*) { /* skinning motion-vector SRV: later */ }
void NRIBackend::BindGraphicsPipelineSampler(GraphicsPipelineHandle* pipeline, const std::string& bindingName, Sampler* sampler) { if (auto* p = static_cast<NRIGraphicsPipeline*>(pipeline)) p->SetSampler(bindingName, sampler); }

#endif // CORONA_HAS_NRI
