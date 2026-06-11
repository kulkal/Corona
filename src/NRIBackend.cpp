#include "stdafx.h"
#include "NRIBackend.h"

// The entire implementation is gated on CORONA_HAS_NRI. When the CMake option
// CORONA_WITH_NRI is OFF (the default), this translation unit is empty and the
// stock D3D12 / Vulkan build is byte-for-byte unaffected.
#if CORONA_HAS_NRI

#include <cstdio>
#include <unordered_map>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>

#include <wrl/client.h>
#include <dxcapi.use.h>

#include "NRI.h"
#include "Extensions/NRIDeviceCreation.h"
#include "Extensions/NRIHelper.h"
#include "Extensions/NRISwapChain.h"
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

static nri::TextureUsageBits ToNRITextureUsage(ETextureUsageFlags u)
{
	nri::TextureUsageBits bits = nri::TextureUsageBits::SHADER_RESOURCE;
	if (HasTextureUsage(u, TextureUsage_RenderTarget))    bits = bits | nri::TextureUsageBits::COLOR_ATTACHMENT;
	if (HasTextureUsage(u, TextureUsage_UnorderedAccess)) bits = bits | nri::TextureUsageBits::SHADER_RESOURCE_STORAGE;
	if (HasTextureUsage(u, TextureUsage_DepthStencil))    bits = bits | nri::TextureUsageBits::DEPTH_STENCIL_ATTACHMENT;
	return bits;
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

	struct TextureAlloc
	{
		nri::Texture* texture = nullptr;
		std::vector<nri::Memory*> memory;
	};
	std::unordered_map<Texture*, TextureAlloc> Textures;

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
		if (m && m->Device)
		{
			if (Pipeline) m->Core.DestroyPipeline(Pipeline);
			if (Layout) m->Core.DestroyPipelineLayout(Layout);
			for (auto& kv : Views) if (kv.second) m->Core.DestroyDescriptor(kv.second);
		}
	}

	enum class Kind { UAV_Buffer, SRV_Buffer, CBV, Sampler, Texture_SRV, Texture_UAV };
	struct Decl { std::string name; Kind kind; uint32_t reg; };

	void BindSRV(const std::string& name, uint32_t baseRegister, uint32_t /*numDescriptors*/) override { Decls.push_back({ name, Kind::SRV_Buffer, baseRegister }); }
	void BindUAV(const std::string& name, uint32_t baseRegister) override { Decls.push_back({ name, Kind::UAV_Buffer, baseRegister }); }
	void BindCBV(const std::string& name, uint32_t baseRegister, uint32_t /*size*/) override { Decls.push_back({ name, Kind::CBV, baseRegister }); }
	void BindSampler(const std::string& name, uint32_t baseRegister) override { Decls.push_back({ name, Kind::Sampler, baseRegister }); }

	bool InitCS(const std::wstring& shaderFile, const std::string& entryPoint) override
	{
		if (!m->Device)
			return false;

		std::ifstream f(shaderFile, std::ios::binary);
		if (!f.good())
			return false;
		std::stringstream ss; ss << f.rdbuf();
		const std::string src = ss.str();
		const std::wstring entryW(entryPoint.begin(), entryPoint.end());
		std::string err;
		Dxil = CompileHLSLToDXIL(src.data(), src.size(), shaderFile.c_str(), entryW.c_str(), L"cs_6_5", err);
		if (Dxil.empty())
			return false;

		// Root descriptors for buffer bindings, ordered as declared.
		RootDescs.clear();
		RootDescIndexByName.clear();
		for (const Decl& d : Decls)
		{
			if (d.kind != Kind::UAV_Buffer && d.kind != Kind::SRV_Buffer)
				continue;
			nri::RootDescriptorDesc rd = {};
			rd.registerIndex = d.reg;
			rd.descriptorType = (d.kind == Kind::UAV_Buffer) ? nri::DescriptorType::STORAGE_STRUCTURED_BUFFER : nri::DescriptorType::STRUCTURED_BUFFER;
			rd.shaderStages = nri::StageBits::COMPUTE_SHADER;
			RootDescIndexByName[d.name] = (uint32_t)RootDescs.size();
			RootDescs.push_back(rd);
		}

		nri::PipelineLayoutDesc pld = {};
		pld.rootRegisterSpace = 0;
		pld.rootDescriptors = RootDescs.empty() ? nullptr : RootDescs.data();
		pld.rootDescriptorNum = (uint32_t)RootDescs.size();
		pld.shaderStages = nri::StageBits::COMPUTE_SHADER;
		if (m->Core.CreatePipelineLayout(*m->Device, pld, Layout) != nri::Result::SUCCESS)
			return false;

		nri::ComputePipelineDesc cpd = {};
		cpd.pipelineLayout = Layout;
		cpd.shader.stage = nri::StageBits::COMPUTE_SHADER;
		cpd.shader.bytecode = Dxil.data();
		cpd.shader.size = Dxil.size();
		cpd.shader.entryPointName = entryPoint.c_str();
		if (m->Core.CreateComputePipeline(*m->Device, cpd, Pipeline) != nri::Result::SUCCESS)
			return false;
		return true;
	}

	void SetBufferUAV(const std::string& name, Buffer* buffer) override { BindBufferView(name, buffer, nri::BufferView::STORAGE_STRUCTURED_BUFFER); }
	void SetBufferSRV(const std::string& name, Buffer* buffer) override { BindBufferView(name, buffer, nri::BufferView::STRUCTURED_BUFFER); }
	void SetTextureSRV(const std::string&, Texture*) override {}     // descriptor-set path: next milestone
	void SetTextureUAV(const std::string&, Texture*) override {}
	void SetVertexBufferUAV(const std::string&, VertexBuffer*) override {}
	void SetSampler(const std::string&, Sampler*) override {}
	void SetCBVValue(const std::string&, void*) override {}

	// Record pipeline layout + root descriptors + pipeline into the backend's
	// active command buffer. The backend's Dispatch() then issues CmdDispatch.
	void Apply() override
	{
		if (!Pipeline || !m->ActiveCmd)
			return;
		m->Core.CmdSetPipelineLayout(*m->ActiveCmd, nri::BindPoint::COMPUTE, *Layout);
		for (const auto& kv : Views)
		{
			auto it = RootDescIndexByName.find(kv.first);
			if (it == RootDescIndexByName.end() || !kv.second)
				continue;
			nri::SetRootDescriptorDesc srd = {};
			srd.rootDescriptorIndex = it->second;
			srd.descriptor = kv.second;
			srd.offset = 0;
			srd.bindPoint = nri::BindPoint::COMPUTE;
			m->Core.CmdSetRootDescriptor(*m->ActiveCmd, srd);
		}
		m->Core.CmdSetPipeline(*m->ActiveCmd, *Pipeline);
	}

	nri::Pipeline* GetPipeline() const { return Pipeline; }

private:
	void BindBufferView(const std::string& name, Buffer* buffer, nri::BufferView viewType)
	{
		if (!buffer)
			return;
		auto bit = m->Buffers.find(buffer);
		if (bit == m->Buffers.end() || !bit->second.buffer)
			return;
		nri::BufferViewDesc bvd = {};
		bvd.buffer = bit->second.buffer;
		bvd.type = viewType;
		bvd.offset = 0;
		bvd.size = static_cast<uint64_t>(buffer->NumElements) * buffer->ElementSize;
		bvd.structureStride = buffer->ElementSize ? buffer->ElementSize : 4;
		nri::Descriptor* view = nullptr;
		if (m->Core.CreateBufferView(bvd, view) == nri::Result::SUCCESS)
		{
			if (Views[name]) m->Core.DestroyDescriptor(Views[name]);
			Views[name] = view;
		}
	}

	NRIBackend::Impl* m = nullptr;
	std::vector<Decl> Decls;
	std::vector<nri::RootDescriptorDesc> RootDescs;
	std::unordered_map<std::string, uint32_t> RootDescIndexByName;
	std::unordered_map<std::string, nri::Descriptor*> Views;
	std::vector<uint8_t> Dxil;
	nri::PipelineLayout* Layout = nullptr;
	nri::Pipeline* Pipeline = nullptr;
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
		psoInitOk = pso.InitCS(path, "main") && pso.GetPipeline() != nullptr;
	}

	char info[800];
	_snprintf_s(info, _TRUNCATE,
		"device ok [%s], rtTier=%u sm=%u queue=%s bufferSmoke=%s texSmoke=%s cmd=%s submitSmoke=%s shaderDXIL=%zuB compute=%s psoInit=%s",
		dd.adapterDesc.name, (unsigned)dd.tiers.rayTracing, (unsigned)dd.shaderModel,
		m->GraphicsQueue ? "ok" : "null", bufferSmokeOk ? "PASS" : "FAIL", texSmokeOk ? "PASS" : "FAIL",
		m->CmdBuffer ? "ok" : "null", submitSmokeOk ? "PASS" : "FAIL", shaderBytes, computeResult.c_str(), psoInitOk ? "PASS" : "FAIL");
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
std::shared_ptr<Sampler> NRIBackend::CreateSampler(const SamplerCreateDesc&) { NRI_TODO(); return nullptr; }
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
