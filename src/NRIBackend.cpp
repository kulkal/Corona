#include "stdafx.h"
#include "NRIBackend.h"

// The entire implementation is gated on CORONA_HAS_NRI. When the CMake option
// CORONA_WITH_NRI is OFF (the default), this translation unit is empty and the
// stock D3D12 / Vulkan build is byte-for-byte unaffected.
#if CORONA_HAS_NRI

#include <cstdio>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <memory>
#include <string>
#include <fstream>
#include <sstream>
#include <utility>

#include <wrl/client.h>
#include <dxcapi.use.h>
#include <filesystem>
#include "DirectXTex.h"

#include "imgui.h"
#include "Utils.h"

#include "NRI.h"
#include "Extensions/NRIDeviceCreation.h"
#include "Extensions/NRIHelper.h"
#include "Extensions/NRISwapChain.h"
#include "Extensions/NRIStreamer.h"
#include "Extensions/NRIImgui.h"
#include "Extensions/NRIRayTracing.h"
#include "Extensions/NRIWrapperD3D12.h"

void AppendCpuRuntimeTrace(const std::wstring& line);

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

	// Include handler + include search path so `#include "Common.hlsl"` etc.
	// resolve. Includes are relative to the shader's own directory; the process
	// cwd is bin/, so an explicit -I<shaderDir> is required.
	ComPtr<IDxcIncludeHandler> includeHandler;
	library->CreateIncludeHandler(&includeHandler);
	std::wstring shaderDir;
	if (sourceName)
	{
		std::wstring sp(sourceName);
		const size_t slash = sp.find_last_of(L"/\\");
		if (slash != std::wstring::npos)
			shaderDir = sp.substr(0, slash);
	}
	std::vector<const wchar_t*> args;
	std::wstring includeArg;
	if (!shaderDir.empty())
	{
		includeArg = L"-I" + shaderDir;
		args.push_back(includeArg.c_str());
	}

	ComPtr<IDxcOperationResult> result;
	HRESULT hr = compiler->Compile(textBlob.Get(), sourceName, entryPoint, target,
		args.empty() ? nullptr : args.data(), (uint32_t)args.size(),
		nullptr, 0, includeHandler.Get(), &result);
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

static DXGI_FORMAT MakeLinearDXGIFormat(DXGI_FORMAT f)
{
	switch (f)
	{
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_BC1_TYPELESS:
	case DXGI_FORMAT_BC1_UNORM_SRGB: return DXGI_FORMAT_BC1_UNORM;
	case DXGI_FORMAT_BC2_TYPELESS:
	case DXGI_FORMAT_BC2_UNORM_SRGB: return DXGI_FORMAT_BC2_UNORM;
	case DXGI_FORMAT_BC3_TYPELESS:
	case DXGI_FORMAT_BC3_UNORM_SRGB: return DXGI_FORMAT_BC3_UNORM;
	case DXGI_FORMAT_BC4_TYPELESS: return DXGI_FORMAT_BC4_UNORM;
	case DXGI_FORMAT_BC5_TYPELESS: return DXGI_FORMAT_BC5_UNORM;
	case DXGI_FORMAT_BC6H_TYPELESS: return DXGI_FORMAT_BC6H_UF16;
	case DXGI_FORMAT_BC7_TYPELESS:
	case DXGI_FORMAT_BC7_UNORM_SRGB: return DXGI_FORMAT_BC7_UNORM;
	default: return f;
	}
}

static DXGI_FORMAT ResolveSampledDXGIFormat(DXGI_FORMAT f, bool nonSRGB)
{
	const DXGI_FORMAT linear = MakeLinearDXGIFormat(f);
	return nonSRGB ? linear : DirectX::MakeSRGB(linear);
}

static nri::Format ToNRISampledTextureFormat(DXGI_FORMAT f)
{
	switch (f)
	{
	case DXGI_FORMAT_R8_UNORM: return nri::Format::R8_UNORM;
	case DXGI_FORMAT_R8G8_UNORM: return nri::Format::RG8_UNORM;
	case DXGI_FORMAT_R8G8B8A8_UNORM: return nri::Format::RGBA8_UNORM;
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return nri::Format::RGBA8_SRGB;
	case DXGI_FORMAT_B8G8R8A8_UNORM: return nri::Format::BGRA8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return nri::Format::BGRA8_SRGB;
	case DXGI_FORMAT_BC1_UNORM: return nri::Format::BC1_RGBA_UNORM;
	case DXGI_FORMAT_BC1_UNORM_SRGB: return nri::Format::BC1_RGBA_SRGB;
	case DXGI_FORMAT_BC2_UNORM: return nri::Format::BC2_RGBA_UNORM;
	case DXGI_FORMAT_BC2_UNORM_SRGB: return nri::Format::BC2_RGBA_SRGB;
	case DXGI_FORMAT_BC3_UNORM: return nri::Format::BC3_RGBA_UNORM;
	case DXGI_FORMAT_BC3_UNORM_SRGB: return nri::Format::BC3_RGBA_SRGB;
	case DXGI_FORMAT_BC4_UNORM: return nri::Format::BC4_R_UNORM;
	case DXGI_FORMAT_BC4_SNORM: return nri::Format::BC4_R_SNORM;
	case DXGI_FORMAT_BC5_UNORM: return nri::Format::BC5_RG_UNORM;
	case DXGI_FORMAT_BC5_SNORM: return nri::Format::BC5_RG_SNORM;
	case DXGI_FORMAT_BC6H_UF16: return nri::Format::BC6H_RGB_UFLOAT;
	case DXGI_FORMAT_BC6H_SF16: return nri::Format::BC6H_RGB_SFLOAT;
	case DXGI_FORMAT_BC7_UNORM: return nri::Format::BC7_RGBA_UNORM;
	case DXGI_FORMAT_BC7_UNORM_SRGB: return nri::Format::BC7_RGBA_SRGB;
	default: return nri::Format::UNKNOWN;
	}
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

static uint32_t CaptureBytesPerPixel(ETextureFormat format)
{
	switch (format)
	{
	case ETextureFormat::RGBA16Float: return 8;
	case ETextureFormat::RGBA32Float: return 16;
	case ETextureFormat::RG16Float:   return 4;
	case ETextureFormat::RGBA8Unorm:  return 4;
	case ETextureFormat::BGRA8Unorm:  return 4;
	case ETextureFormat::D32Float:    return 4;
	case ETextureFormat::R32Float:    return 4;
	case ETextureFormat::R8Uint:      return 1;
	default:                          return 0;
	}
}

static nri::PlaneBits CapturePlaneBits(ETextureFormat format)
{
	return format == ETextureFormat::D32Float ? nri::PlaneBits::DEPTH : nri::PlaneBits::COLOR;
}

static uint64_t AlignUpU64(uint64_t value, uint64_t alignment)
{
	if (alignment <= 1)
		return value;
	return (value + alignment - 1) & ~(alignment - 1);
}

static nri::TextureUsageBits ToNRITextureUsage(ETextureUsageFlags u)
{
	nri::TextureUsageBits bits = nri::TextureUsageBits::SHADER_RESOURCE;
	if (HasTextureUsage(u, TextureUsage_RenderTarget))    bits = bits | nri::TextureUsageBits::COLOR_ATTACHMENT;
	if (HasTextureUsage(u, TextureUsage_UnorderedAccess)) bits = bits | nri::TextureUsageBits::SHADER_RESOURCE_STORAGE;
	if (HasTextureUsage(u, TextureUsage_DepthStencil))    bits = bits | nri::TextureUsageBits::DEPTH_STENCIL_ATTACHMENT;
	return bits;
}

// D3D12 enhanced barriers require AccessBefore to be consistent with LayoutBefore
// (NONE access is only valid with UNDEFINED layout / NONE stages). Derive the access.
static nri::AccessBits AccessForLayout(nri::Layout layout)
{
	switch (layout)
	{
	case nri::Layout::COLOR_ATTACHMENT:         return nri::AccessBits::COLOR_ATTACHMENT;
	case nri::Layout::DEPTH_STENCIL_ATTACHMENT: return nri::AccessBits::DEPTH_STENCIL_ATTACHMENT_WRITE;
	case nri::Layout::SHADER_RESOURCE:          return nri::AccessBits::SHADER_RESOURCE;
	case nri::Layout::SHADER_RESOURCE_STORAGE:  return nri::AccessBits::SHADER_RESOURCE_STORAGE;
	case nri::Layout::COPY_SOURCE:              return nri::AccessBits::COPY_SOURCE;
	case nri::Layout::COPY_DESTINATION:         return nri::AccessBits::COPY_DESTINATION;
	default:                                    return nri::AccessBits::NONE; // UNDEFINED / PRESENT
	}
}
static nri::StageBits StagesForLayout(nri::Layout layout)
{
	return (layout == nri::Layout::UNDEFINED || layout == nri::Layout::PRESENT) ? nri::StageBits::NONE : nri::StageBits::ALL;
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
struct NRIRTAS;

struct NRIBackend::Impl
{
	nri::Device* Device = nullptr;
	nri::CoreInterface Core{};
	nri::HelperInterface Helper{};
	nri::RayTracingInterface RT{};
	bool HasRayTracing = false;
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

	// Graphics command recording (Stage 2)
	struct GpuBuf { nri::Buffer* buffer = nullptr; std::vector<nri::Memory*> memory; uint32_t stride = 0; nri::IndexType indexType = nri::IndexType::UINT32; void* mapped = nullptr; uint32_t capacity = 0; };
	std::unordered_map<VertexBuffer*, GpuBuf> VBs;
	std::unordered_map<IndexBuffer*, GpuBuf> IBs;
	std::unordered_map<Texture*, nri::Layout> TexLayout;       // tracked per-texture layout
	std::unordered_map<Texture*, nri::Descriptor*> TexColorView;
	std::unordered_map<Texture*, nri::Descriptor*> TexDepthView;
	bool RPOpen = false;
	bool RasterEnabled = true; // scene graphics-pass recording (ImGui path is separate)
	bool HasBoundGfx = false;  // a valid graphics pipeline is bound in the current pass
	std::vector<Texture*> RTColors;
	Texture* RTDepth = nullptr;
	float RTClear[4] = { 0, 0, 0, 1 };
	bool RTHasClear = false;
	float RTDepthClear = 1.0f;
	bool RTHasDepthClear = false;
	// Per-target deferred clears. Corona issues ClearRenderTarget/ClearDepth per
	// texture (and may do so before SetRenderTargets), but NRI clears happen as
	// the render pass LoadOp. Track each target's clear so OpenRP can apply the
	// right color/value per attachment and SetRenderTargets doesn't wipe them.
	struct ClearColor { float v[4]; };
	std::unordered_map<Texture*, ClearColor> PendingColorClears;
	std::unordered_map<Texture*, float> PendingDepthClears;
	uint32_t VpW = 0, VpH = 0;
	// Per-frame diagnostics (Stage 2 bring-up): counts reset each BeginFrame.
	uint32_t DbgRPOpens = 0, DbgGfxBindOk = 0, DbgGfxBindFail = 0;
	uint32_t DbgDraws = 0, DbgDrawsSkipped = 0, DbgImguiDraws = 0, DbgClears = 0;
	std::vector<std::weak_ptr<NRIRTAS>> RayTracingAS;

	nri::Texture* NriTex(Texture* t)
	{
		auto it = Textures.find(t);
		return (it != Textures.end()) ? it->second.texture : nullptr;
	}
	bool IsBackbuffer(Texture* t, uint32_t& outIdx)
	{
		for (uint32_t i = 0; i < BackBufferWrappers.size(); ++i)
			if (BackBufferWrappers[i].get() == t) { outIdx = i; return true; }
		return false;
	}
	nri::Descriptor* ColorView(Texture* t)
	{
		uint32_t bi = 0;
		if (IsBackbuffer(t, bi)) return bi < BackBufferViews.size() ? BackBufferViews[bi] : nullptr;
		auto it = TexColorView.find(t);
		if (it != TexColorView.end()) return it->second;
		nri::Texture* nt = NriTex(t); if (!nt) return nullptr;
		const nri::TextureDesc& td = Core.GetTextureDesc(*nt);
		nri::TextureViewDesc tvd = {}; tvd.texture = nt; tvd.type = nri::TextureView::COLOR_ATTACHMENT; tvd.format = td.format; tvd.mipNum = 1; tvd.layerNum = 1;
		nri::Descriptor* d = nullptr; Core.CreateTextureView(tvd, d); TexColorView[t] = d; return d;
	}
	nri::Descriptor* DepthView(Texture* t)
	{
		auto it = TexDepthView.find(t);
		if (it != TexDepthView.end()) return it->second;
		nri::Texture* nt = NriTex(t); if (!nt) return nullptr;
		const nri::TextureDesc& td = Core.GetTextureDesc(*nt);
		nri::TextureViewDesc tvd = {}; tvd.texture = nt; tvd.type = nri::TextureView::DEPTH_STENCIL_ATTACHMENT; tvd.format = td.format; tvd.mipNum = 1; tvd.layerNum = 1;
		nri::Descriptor* d = nullptr; Core.CreateTextureView(tvd, d); TexDepthView[t] = d; return d;
	}
	void TransitionTex(Texture* t, nri::AccessBits access, nri::Layout layout, nri::StageBits stages)
	{
		if (!ActiveCmd) return;
		uint32_t bi = 0;
		if (IsBackbuffer(t, bi)) { TransitionBackbuffer(access, layout, stages); return; }
		nri::Texture* nt = NriTex(t); if (!nt) return;
		nri::Layout before = TexLayout.count(t) ? TexLayout[t] : nri::Layout::UNDEFINED;
		nri::TextureBarrierDesc tb = {};
		tb.texture = nt;
		tb.before.access = AccessForLayout(before); tb.before.layout = before; tb.before.stages = StagesForLayout(before);
		tb.after.access = access; tb.after.layout = layout; tb.after.stages = stages;
		tb.mipNum = 1; tb.layerNum = 1;
		nri::BarrierDesc bd = {}; bd.textures = &tb; bd.textureNum = 1;
		Core.CmdBarrier(*ActiveCmd, bd);
		TexLayout[t] = layout;
	}
	void OpenRP()
	{
		if (RPOpen || !ActiveCmd || (RTColors.empty() && !RTDepth)) return;
		std::vector<nri::AttachmentDesc> colors;
		for (Texture* t : RTColors)
		{
			TransitionTex(t, nri::AccessBits::COLOR_ATTACHMENT, nri::Layout::COLOR_ATTACHMENT, nri::StageBits::ALL);
			nri::AttachmentDesc a = {}; a.descriptor = ColorView(t);
			auto cit = PendingColorClears.find(t);
			if (cit != PendingColorClears.end())
			{
				a.loadOp = nri::LoadOp::CLEAR;
				a.clearValue.color.f = nri::Color32f{ cit->second.v[0], cit->second.v[1], cit->second.v[2], cit->second.v[3] };
				PendingColorClears.erase(cit);
			}
			else { a.loadOp = nri::LoadOp::LOAD; }
			a.storeOp = nri::StoreOp::STORE;
			colors.push_back(a);
		}
		nri::AttachmentDesc depthAtt = {};
		bool haveDepth = false;
		if (RTDepth)
		{
			TransitionTex(RTDepth, nri::AccessBits::DEPTH_STENCIL_ATTACHMENT_WRITE, nri::Layout::DEPTH_STENCIL_ATTACHMENT, nri::StageBits::ALL);
			depthAtt.descriptor = DepthView(RTDepth);
			auto dit = PendingDepthClears.find(RTDepth);
			if (dit != PendingDepthClears.end())
			{
				depthAtt.loadOp = nri::LoadOp::CLEAR;
				depthAtt.clearValue.depthStencil.depth = dit->second;
				PendingDepthClears.erase(dit);
			}
			else { depthAtt.loadOp = nri::LoadOp::LOAD; }
			depthAtt.storeOp = nri::StoreOp::STORE;
			haveDepth = depthAtt.descriptor != nullptr;
		}
		nri::RenderingDesc rd = {};
		rd.colors = colors.empty() ? nullptr : colors.data(); rd.colorNum = (uint32_t)colors.size();
		if (haveDepth) rd.depth = depthAtt;
		Core.CmdBeginRendering(*ActiveCmd, rd);
		if (VpW && VpH)
		{
			nri::Viewport vp = {}; vp.x = 0; vp.y = 0; vp.width = (float)VpW; vp.height = (float)VpH; vp.depthMin = 0; vp.depthMax = 1;
			nri::Rect sc = {}; sc.x = 0; sc.y = 0; sc.width = (nri::Dim_t)VpW; sc.height = (nri::Dim_t)VpH;
			Core.CmdSetViewports(*ActiveCmd, &vp, 1);
			Core.CmdSetScissors(*ActiveCmd, &sc, 1);
		}
		RPOpen = true;
		RTHasClear = false; RTHasDepthClear = false;
	}
	void EndRP()
	{
		HasBoundGfx = false;
		if (!RPOpen || !ActiveCmd) return;
		Core.CmdEndRendering(*ActiveCmd);
		RPOpen = false;
		// Make color targets samplable by later passes (skip the backbuffer).
		for (Texture* t : RTColors)
		{
			uint32_t bi = 0;
			if (!IsBackbuffer(t, bi))
				TransitionTex(t, nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::ALL);
		}
	}

	// Transition the current backbuffer to a target access/layout, recording on ActiveCmd.
	void TransitionBackbuffer(nri::AccessBits access, nri::Layout layout, nri::StageBits stages)
	{
		if (!ActiveCmd || !FrameHasBackbuffer || CurrentBackBuffer >= BackBuffers.size())
			return;
		nri::TextureBarrierDesc tb = {};
		tb.texture = BackBuffers[CurrentBackBuffer];
		tb.before.access = AccessForLayout(BBLayout);
		tb.before.layout = BBLayout;
		tb.before.stages = StagesForLayout(BBLayout);
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
		void* mapped = nullptr; // non-null for persistently-mapped HOST_UPLOAD buffers
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

	void BarrierAccelerationStructure(nri::CommandBuffer& commandBuffer, nri::AccelerationStructure* accelerationStructure)
	{
		if (!accelerationStructure || !RT.GetAccelerationStructureBuffer)
			return;

		nri::Buffer* buffer = RT.GetAccelerationStructureBuffer(*accelerationStructure);
		if (!buffer)
			return;

		nri::BufferBarrierDesc barrier = {};
		barrier.buffer = buffer;
		barrier.before.access = nri::AccessBits::ACCELERATION_STRUCTURE_WRITE;
		barrier.before.stages = nri::StageBits::ACCELERATION_STRUCTURE;
		barrier.after.access = nri::AccessBits::ACCELERATION_STRUCTURE_READ;
		barrier.after.stages = nri::StageBits::ACCELERATION_STRUCTURE | nri::StageBits::RAY_TRACING_SHADERS | nri::StageBits::COMPUTE_SHADER;

		nri::BarrierDesc barriers = {};
		barriers.buffers = &barrier;
		barriers.bufferNum = 1;
		Core.CmdBarrier(commandBuffer, barriers);
	}

	template <typename RecordFn>
	bool SubmitImmediate(const char* label, RecordFn record)
	{
		if (!Device || !GraphicsQueue || !CmdAllocator || !CmdBuffer || !Fence || ActiveCmd)
		{
			if (label)
				OutputDebugStringA(label);
			return false;
		}

		Core.ResetCommandAllocator(*CmdAllocator);
		if (Core.BeginCommandBuffer(*CmdBuffer, nullptr) != nri::Result::SUCCESS)
			return false;

		record(*CmdBuffer);

		if (Core.EndCommandBuffer(*CmdBuffer) != nri::Result::SUCCESS)
			return false;

		nri::FenceSubmitDesc signalFence = {};
		signalFence.fence = Fence;
		signalFence.value = ++FenceValue;
		signalFence.stages = nri::StageBits::ALL;

		nri::CommandBuffer* commandBuffers[1] = { CmdBuffer };
		nri::QueueSubmitDesc submit = {};
		submit.commandBuffers = commandBuffers;
		submit.commandBufferNum = 1;
		submit.signalFences = &signalFence;
		submit.signalFenceNum = 1;

		if (Core.QueueSubmit(*GraphicsQueue, submit) != nri::Result::SUCCESS)
			return false;
		Core.Wait(*Fence, FenceValue);
		return Core.GetFenceValue(*Fence) >= FenceValue;
	}

	std::string RunRayTracingASSmoke()
	{
		if (!HasRayTracing || RayTracingTier < 1)
			return "skipped";
		if (!CmdBuffer || !GraphicsQueue || !Fence)
			return "skipped";

		struct SmokeVertex { float x, y, z; };
		const SmokeVertex vertices[3] = {
			{ 0.0f, 0.0f, 0.0f },
			{ 1.0f, 0.0f, 0.0f },
			{ 0.0f, 1.0f, 0.0f }
		};
		const uint32_t indices[3] = { 0u, 1u, 2u };

		nri::Buffer* vertexBuffer = nullptr;
		nri::Buffer* indexBuffer = nullptr;
		nri::Buffer* blasScratch = nullptr;
		nri::Buffer* tlasScratch = nullptr;
		nri::Buffer* instanceBuffer = nullptr;
		std::vector<nri::Memory*> vertexMemory;
		std::vector<nri::Memory*> indexMemory;
		std::vector<nri::Memory*> blasScratchMemory;
		std::vector<nri::Memory*> tlasScratchMemory;
		std::vector<nri::Memory*> instanceMemory;
		nri::AccelerationStructure* blas = nullptr;
		nri::AccelerationStructure* tlas = nullptr;
		nri::Descriptor* tlasDescriptor = nullptr;
		std::string result = "FAIL";

		auto UploadToHostBuffer = [&](nri::Buffer* buffer, const void* data, uint64_t size) -> bool
		{
			void* mapped = Core.MapBuffer(*buffer, 0, size);
			if (!mapped)
				return false;
			memcpy(mapped, data, static_cast<size_t>(size));
			Core.UnmapBuffer(*buffer);
			return true;
		};

		auto SubmitSmoke = [&](auto record, const char*& failure) -> bool
		{
			nri::CommandAllocator* allocator = nullptr;
			nri::CommandBuffer* commandBuffer = nullptr;
			nri::Fence* fence = nullptr;
			uint64_t fenceValue = 1;

			auto Cleanup = [&]()
			{
				if (fence)
					Core.DestroyFence(fence);
				if (commandBuffer)
					Core.DestroyCommandBuffer(commandBuffer);
				if (allocator)
					Core.DestroyCommandAllocator(allocator);
			};

			if (Core.CreateCommandAllocator(*GraphicsQueue, allocator) != nri::Result::SUCCESS || !allocator)
			{
				failure = "Alloc";
				Cleanup();
				return false;
			}
			if (Core.CreateCommandBuffer(*allocator, commandBuffer) != nri::Result::SUCCESS || !commandBuffer)
			{
				failure = "Cmd";
				Cleanup();
				return false;
			}
			if (Core.CreateFence(*Device, 0, fence) != nri::Result::SUCCESS || !fence)
			{
				failure = "Fence";
				Cleanup();
				return false;
			}
			if (Core.BeginCommandBuffer(*commandBuffer, nullptr) != nri::Result::SUCCESS)
			{
				failure = "Begin";
				Cleanup();
				return false;
			}

			record(*commandBuffer);

			if (Core.EndCommandBuffer(*commandBuffer) != nri::Result::SUCCESS)
			{
				failure = "End";
				Cleanup();
				return false;
			}

			nri::FenceSubmitDesc signalFence = {};
			signalFence.fence = fence;
			signalFence.value = fenceValue;
			signalFence.stages = nri::StageBits::ALL;

			nri::CommandBuffer* commandBuffers[1] = { commandBuffer };
			nri::QueueSubmitDesc submit = {};
			submit.commandBuffers = commandBuffers;
			submit.commandBufferNum = 1;
			submit.signalFences = &signalFence;
			submit.signalFenceNum = 1;

			if (Core.QueueSubmit(*GraphicsQueue, submit) != nri::Result::SUCCESS)
			{
				failure = "Submit";
				Cleanup();
				return false;
			}
			Core.Wait(*fence, fenceValue);
			const bool done = Core.GetFenceValue(*fence) >= fenceValue;
			Cleanup();
			if (!done)
				failure = "Wait";
			return done;
		};

		do
		{
			if (!CreateBoundBuffer(sizeof(vertices), 0, nri::BufferUsageBits::SHADER_RESOURCE | nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT,
				nri::MemoryLocation::HOST_UPLOAD, vertexBuffer, vertexMemory))
			{
				result = "FAIL(vb)";
				break;
			}
			if (!UploadToHostBuffer(vertexBuffer, vertices, sizeof(vertices)))
			{
				result = "FAIL(vbUpload)";
				break;
			}
			if (!CreateBoundBuffer(sizeof(indices), 0, nri::BufferUsageBits::SHADER_RESOURCE | nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT,
				nri::MemoryLocation::HOST_UPLOAD, indexBuffer, indexMemory))
			{
				result = "FAIL(ib)";
				break;
			}
			if (!UploadToHostBuffer(indexBuffer, indices, sizeof(indices)))
			{
				result = "FAIL(ibUpload)";
				break;
			}

			nri::BottomLevelGeometryDesc geometry = {};
			geometry.flags = nri::BottomLevelGeometryBits::OPAQUE_GEOMETRY;
			geometry.type = nri::BottomLevelGeometryType::TRIANGLES;
			geometry.triangles.vertexBuffer = vertexBuffer;
			geometry.triangles.vertexNum = 3;
			geometry.triangles.vertexStride = sizeof(SmokeVertex);
			geometry.triangles.vertexFormat = nri::Format::RGB32_SFLOAT;
			geometry.triangles.indexBuffer = indexBuffer;
			geometry.triangles.indexNum = 3;
			geometry.triangles.indexType = nri::IndexType::UINT32;

			nri::AccelerationStructureDesc blasDesc = {};
			blasDesc.geometries = &geometry;
			blasDesc.geometryOrInstanceNum = 1;
			blasDesc.flags = nri::AccelerationStructureBits::PREFER_FAST_TRACE;
			blasDesc.type = nri::AccelerationStructureType::BOTTOM_LEVEL;
			if (RT.CreateCommittedAccelerationStructure(*Device, nri::MemoryLocation::DEVICE, 0.0f, blasDesc, blas) != nri::Result::SUCCESS || !blas)
			{
				result = "FAIL(blas)";
				break;
			}

			const uint64_t blasScratchSize = RT.GetAccelerationStructureBuildScratchBufferSize(*blas);
			if (blasScratchSize == 0 ||
				!CreateBoundBuffer(blasScratchSize, 0, nri::BufferUsageBits::SCRATCH_BUFFER, nri::MemoryLocation::DEVICE, blasScratch, blasScratchMemory))
			{
				result = "FAIL(blasScratch)";
				break;
			}

			nri::BuildBottomLevelAccelerationStructureDesc blasBuild = {};
			blasBuild.dst = blas;
			blasBuild.geometries = &geometry;
			blasBuild.geometryNum = 1;
			blasBuild.scratchBuffer = blasScratch;

			const char* submitFailure = "";
			const bool blasSubmitted = SubmitSmoke([&](nri::CommandBuffer& cmd)
			{
				RT.CmdBuildBottomLevelAccelerationStructures(cmd, &blasBuild, 1);
				BarrierAccelerationStructure(cmd, blas);
			}, submitFailure);
			if (!blasSubmitted)
			{
				result = std::string("FAIL(blas") + submitFailure + ")";
				break;
			}

			const uint64_t blasHandle = RT.GetAccelerationStructureHandle(*blas);
			if (blasHandle == 0)
			{
				result = "FAIL(blasHandle)";
				break;
			}

			nri::AccelerationStructureDesc tlasDesc = {};
			tlasDesc.geometryOrInstanceNum = 1;
			tlasDesc.flags = nri::AccelerationStructureBits::PREFER_FAST_TRACE;
			tlasDesc.type = nri::AccelerationStructureType::TOP_LEVEL;
			if (RT.CreateCommittedAccelerationStructure(*Device, nri::MemoryLocation::DEVICE, 0.0f, tlasDesc, tlas) != nri::Result::SUCCESS || !tlas)
			{
				result = "FAIL(tlas)";
				break;
			}

			const uint64_t tlasScratchSize = RT.GetAccelerationStructureBuildScratchBufferSize(*tlas);
			if (tlasScratchSize == 0 ||
				!CreateBoundBuffer(tlasScratchSize, 0, nri::BufferUsageBits::SCRATCH_BUFFER, nri::MemoryLocation::DEVICE, tlasScratch, tlasScratchMemory))
			{
				result = "FAIL(tlasScratch)";
				break;
			}

			nri::TopLevelInstance instance = {};
			instance.transform[0][0] = 1.0f;
			instance.transform[1][1] = 1.0f;
			instance.transform[2][2] = 1.0f;
			instance.instanceId = 0;
			instance.mask = 0xFF;
			instance.shaderBindingTableLocalOffset = 0;
			instance.flags = nri::TopLevelInstanceBits::NONE;
			instance.accelerationStructureHandle = blasHandle;

			if (!CreateBoundBuffer(sizeof(instance), 0, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT,
				nri::MemoryLocation::HOST_UPLOAD, instanceBuffer, instanceMemory))
			{
				result = "FAIL(instBuf)";
				break;
			}
			if (!UploadToHostBuffer(instanceBuffer, &instance, sizeof(instance)))
			{
				result = "FAIL(instUpload)";
				break;
			}

			nri::BuildTopLevelAccelerationStructureDesc tlasBuild = {};
			tlasBuild.dst = tlas;
			tlasBuild.instanceNum = 1;
			tlasBuild.instanceBuffer = instanceBuffer;
			tlasBuild.scratchBuffer = tlasScratch;

			submitFailure = "";
			const bool tlasSubmitted = SubmitSmoke([&](nri::CommandBuffer& cmd)
			{
				RT.CmdBuildTopLevelAccelerationStructures(cmd, &tlasBuild, 1);
				BarrierAccelerationStructure(cmd, tlas);
			}, submitFailure);
			if (!tlasSubmitted)
			{
				result = std::string("FAIL(tlas") + submitFailure + ")";
				break;
			}
			if (RT.GetAccelerationStructureHandle(*tlas) == 0)
			{
				result = "FAIL(tlasHandle)";
				break;
			}
			if (RT.CreateAccelerationStructureDescriptor(*tlas, tlasDescriptor) != nri::Result::SUCCESS || !tlasDescriptor)
			{
				result = "FAIL(tlasDesc)";
				break;
			}

			result = "PASS";
		} while (false);

		if (tlasDescriptor)
			Core.DestroyDescriptor(tlasDescriptor);
		if (tlas)
			RT.DestroyAccelerationStructure(tlas);
		if (blas)
			RT.DestroyAccelerationStructure(blas);
		FreeBuffer(instanceBuffer, instanceMemory);
		FreeBuffer(tlasScratch, tlasScratchMemory);
		FreeBuffer(blasScratch, blasScratchMemory);
		FreeBuffer(indexBuffer, indexMemory);
		FreeBuffer(vertexBuffer, vertexMemory);
		return result;
	}

	std::string RunRayTracingPipelineSmoke()
	{
		if (!HasRayTracing || RayTracingTier < 1 || !CmdBuffer || !GraphicsQueue || !Fence)
			return "skipped";

		static const char* kRayGen =
			"[shader(\"raygeneration\")] void RayGen() {}\n";
		static const char* kMiss =
			"struct Payload { uint value; };\n"
			"[shader(\"miss\")] void Miss(inout Payload payload) { payload.value = 0; }\n";

		std::string rayGenErr, missErr;
		std::vector<uint8_t> rayGenDxil = CompileHLSLToDXIL(kRayGen, strlen(kRayGen), L"nri_rt_smoke_raygen.hlsl", nullptr, L"lib_6_3", rayGenErr);
		std::vector<uint8_t> missDxil = CompileHLSLToDXIL(kMiss, strlen(kMiss), L"nri_rt_smoke_miss.hlsl", nullptr, L"lib_6_3", missErr);
		if (rayGenDxil.empty() || missDxil.empty())
			return "FAIL(dxc)";

		nri::PipelineLayout* layout = nullptr;
		nri::Pipeline* pipeline = nullptr;
		nri::Buffer* sbt = nullptr;
		std::vector<nri::Memory*> sbtMemory;

		auto Cleanup = [&]()
		{
			if (pipeline) Core.DestroyPipeline(pipeline);
			if (layout) Core.DestroyPipelineLayout(layout);
			FreeBuffer(sbt, sbtMemory);
		};

		nri::PipelineLayoutDesc pld = {};
		pld.shaderStages = nri::StageBits::RAY_TRACING_SHADERS;
		if (Core.CreatePipelineLayout(*Device, pld, layout) != nri::Result::SUCCESS || !layout)
		{
			Cleanup();
			return "FAIL(layout)";
		}

		nri::ShaderDesc shaders[2] = {};
		shaders[0].stage = nri::StageBits::RAYGEN_SHADER;
		shaders[0].bytecode = rayGenDxil.data();
		shaders[0].size = rayGenDxil.size();
		shaders[0].entryPointName = "RayGen";
		shaders[1].stage = nri::StageBits::MISS_SHADER;
		shaders[1].bytecode = missDxil.data();
		shaders[1].size = missDxil.size();
		shaders[1].entryPointName = "Miss";

		nri::ShaderLibraryDesc library = {};
		library.shaders = shaders;
		library.shaderNum = 2;

		nri::ShaderGroupDesc groups[2] = {};
		groups[0].shaderIndices[0] = 1;
		groups[1].shaderIndices[0] = 2;

		nri::RayTracingPipelineDesc rtd = {};
		rtd.pipelineLayout = layout;
		rtd.shaderLibrary = &library;
		rtd.shaderGroups = groups;
		rtd.shaderGroupNum = 2;
		rtd.recursionMaxDepth = 1;
		rtd.rayPayloadMaxSize = 4;
		rtd.rayHitAttributeMaxSize = 8;
		if (RT.CreateRayTracingPipeline(*Device, rtd, pipeline) != nri::Result::SUCCESS || !pipeline)
		{
			Cleanup();
			return "FAIL(pipeline)";
		}

		const nri::DeviceDesc& deviceDesc = Core.GetDeviceDesc(*Device);
		const uint32_t idSize = deviceDesc.shaderStage.rayTracing.shaderGroupIdentifierSize;
		const uint64_t sbtAlignment = std::max<uint32_t>(1u, deviceDesc.memoryAlignment.shaderBindingTable);
		const uint64_t sbtRecordSize = AlignUpU64(idSize, sbtAlignment);
		const uint64_t raygenOffset = 0;
		const uint64_t missOffset = AlignUpU64(raygenOffset + sbtRecordSize, sbtAlignment);
		const uint64_t sbtSize = missOffset + sbtRecordSize;
		if (idSize == 0 || !CreateBoundBuffer(sbtSize, 0, nri::BufferUsageBits::SHADER_BINDING_TABLE, nri::MemoryLocation::HOST_UPLOAD, sbt, sbtMemory))
		{
			Cleanup();
			return "FAIL(sbt)";
		}

		void* mapped = Core.MapBuffer(*sbt, 0, sbtSize);
		if (!mapped)
		{
			Cleanup();
			return "FAIL(map)";
		}
		uint8_t* mappedBytes = static_cast<uint8_t*>(mapped);
		nri::Result writeResult = RT.WriteShaderGroupIdentifiers(*pipeline, 0, 1, mappedBytes + raygenOffset);
		if (writeResult == nri::Result::SUCCESS)
			writeResult = RT.WriteShaderGroupIdentifiers(*pipeline, 1, 1, mappedBytes + missOffset);
		Core.UnmapBuffer(*sbt);
		if (writeResult != nri::Result::SUCCESS)
		{
			Cleanup();
			return "FAIL(sbtIds)";
		}

		const bool submitted = SubmitImmediate("[NRIRT] DispatchRays smoke immediate submit failed\n", [&](nri::CommandBuffer& cmd)
		{
			Core.CmdSetPipelineLayout(cmd, nri::BindPoint::RAY_TRACING, *layout);
			Core.CmdSetPipeline(cmd, *pipeline);

			nri::DispatchRaysDesc dispatch = {};
			dispatch.raygenShader.buffer = sbt;
			dispatch.raygenShader.offset = raygenOffset;
			dispatch.raygenShader.size = sbtRecordSize;
			dispatch.raygenShader.stride = sbtRecordSize;
			dispatch.missShaders.buffer = sbt;
			dispatch.missShaders.offset = missOffset;
			dispatch.missShaders.size = sbtRecordSize;
			dispatch.missShaders.stride = sbtRecordSize;
			dispatch.x = 1;
			dispatch.y = 1;
			dispatch.z = 1;
			RT.CmdDispatchRays(cmd, dispatch);
		});

		Cleanup();
		return submitted ? "PASS" : "FAIL(dispatch)";
	}
};

struct NRIRTAS : RTAS
{
	NRIBackend::Impl* Owner = nullptr;
	nri::AccelerationStructure* AccelerationStructure = nullptr;
	nri::Descriptor* Descriptor = nullptr;
	nri::BottomLevelGeometryDesc BottomGeometry = {};
	nri::Buffer* ScratchBuffer = nullptr;
	std::vector<nri::Memory*> ScratchMemory;
	nri::Buffer* InstanceBuffer = nullptr;
	std::vector<nri::Memory*> InstanceMemory;
	void* InstanceMapped = nullptr;
	uint32_t InstanceCount = 0;
	bool IsTopLevel = false;
	bool AllowUpdate = false;
	bool BuildPending = false;

	~NRIRTAS() override { Destroy(); }

	void Destroy()
	{
		if (!Owner || !Owner->Device)
		{
			Owner = nullptr;
			return;
		}

		if (Descriptor)
		{
			Owner->Core.DestroyDescriptor(Descriptor);
			Descriptor = nullptr;
		}
		if (ScratchBuffer)
		{
			Owner->FreeBuffer(ScratchBuffer, ScratchMemory);
			ScratchBuffer = nullptr;
		}
		if (InstanceBuffer)
		{
			Owner->FreeBuffer(InstanceBuffer, InstanceMemory);
			InstanceBuffer = nullptr;
			InstanceMapped = nullptr;
		}
		if (AccelerationStructure)
		{
			Owner->RT.DestroyAccelerationStructure(AccelerationStructure);
			AccelerationStructure = nullptr;
		}
		Owner = nullptr;
	}
};

static std::wstring FormatNRIHex(uint64_t value)
{
	std::wstringstream stream;
	stream << L"0x" << std::hex << std::uppercase << value;
	return stream.str();
}

static nri::IndexType ToNRIIndexType(EIndexFormat format)
{
	return format == EIndexFormat::U16 ? nri::IndexType::UINT16 : nri::IndexType::UINT32;
}

static bool FillNRIBottomGeometry(
	NRIBackend::Impl* m,
	Mesh* mesh,
	bool skeletal,
	nri::BottomLevelGeometryDesc& geometry,
	uint32_t& vertexCount,
	uint32_t& indexCount)
{
	if (!m || !mesh || !mesh->Ib || mesh->VertexStride == 0)
		return false;

	VertexBuffer* vb = skeletal ? mesh->SkeletalOutputVb.get() : mesh->Vb.get();
	if (!vb)
		return false;

	auto vbIt = m->VBs.find(vb);
	auto ibIt = m->IBs.find(mesh->Ib.get());
	if (vbIt == m->VBs.end() || !vbIt->second.buffer ||
		ibIt == m->IBs.end() || !ibIt->second.buffer)
		return false;

	vertexCount = skeletal ? mesh->SkeletalVertexCount : static_cast<uint32_t>(std::max(0, vb->numVertices));
	indexCount = static_cast<uint32_t>(std::max(0, mesh->Ib->numIndices));
	if (vertexCount == 0 || indexCount < 3)
		return false;

	const uint64_t vertexOffset = skeletal
		? static_cast<uint64_t>(mesh->SkeletalCharIndex) *
			static_cast<uint64_t>(mesh->SkeletalVertexCount) *
			static_cast<uint64_t>(mesh->VertexStride)
		: 0ull;

	geometry = {};
	geometry.flags = mesh->bTransparent
		? nri::BottomLevelGeometryBits::NONE
		: nri::BottomLevelGeometryBits::OPAQUE_GEOMETRY;
	geometry.type = nri::BottomLevelGeometryType::TRIANGLES;
	geometry.triangles.vertexBuffer = vbIt->second.buffer;
	geometry.triangles.vertexOffset = vertexOffset;
	geometry.triangles.vertexNum = vertexCount;
	geometry.triangles.vertexStride = static_cast<uint16_t>(mesh->VertexStride);
	geometry.triangles.vertexFormat = nri::Format::RGB32_SFLOAT;
	geometry.triangles.indexBuffer = ibIt->second.buffer;
	geometry.triangles.indexOffset = 0;
	geometry.triangles.indexNum = indexCount;
	geometry.triangles.indexType = ToNRIIndexType(mesh->IndexFormat);
	return true;
}

static bool WriteNRITLASInstances(NRIRTAS* as, const std::vector<RTInstanceDesc>& instances)
{
	if (!as || !as->Owner || !as->InstanceMapped || instances.size() > static_cast<size_t>(UINT32_MAX))
		return false;

	nri::TopLevelInstance* dst = static_cast<nri::TopLevelInstance*>(as->InstanceMapped);
	memset(dst, 0, sizeof(nri::TopLevelInstance) * instances.size());

	for (uint32_t i = 0; i < static_cast<uint32_t>(instances.size()); ++i)
	{
		NRIRTAS* blas = dynamic_cast<NRIRTAS*>(instances[i].BottomLevelAS.get());
		if (!blas || !blas->AccelerationStructure || !blas->Owner)
			return false;

		const glm::mat4x4 mat = glm::transpose(instances[i].Transform);
		memcpy(dst[i].transform, &mat, sizeof(dst[i].transform));
		dst[i].instanceId = i;
		dst[i].mask = 0xFF;
		// The NRI RT PSO currently emits one shared hit-group record. InstanceID()
		// still carries the scene instance index used by InstanceProperty.
		dst[i].shaderBindingTableLocalOffset = 0;
		dst[i].flags = nri::TopLevelInstanceBits::NONE;
		dst[i].accelerationStructureHandle = as->Owner->RT.GetAccelerationStructureHandle(*blas->AccelerationStructure);
		if (dst[i].accelerationStructureHandle == 0)
			return false;
	}

	return true;
}

static std::shared_ptr<RTAS> CreateNRIBLASForMeshInternal(NRIBackend::Impl* m, Mesh* mesh, bool skeletal, bool allowUpdate)
{
	if (!m || !m->Device || !m->HasRayTracing || !mesh)
		return nullptr;

	uint32_t vertexCount = 0;
	uint32_t indexCount = 0;
	nri::BottomLevelGeometryDesc geometry = {};
	if (!FillNRIBottomGeometry(m, mesh, skeletal, geometry, vertexCount, indexCount))
	{
		AppendCpuRuntimeTrace(
			L"[NRIRTAS] CreateBLAS skipped invalid mesh"
			L", skeletal=" + std::to_wstring(skeletal ? 1 : 0) +
			L", mesh=" + FormatNRIHex(reinterpret_cast<uint64_t>(mesh)) +
			L", hasVb=" + std::to_wstring(mesh->Vb ? 1 : 0) +
			L", hasSkeletalOutputVb=" + std::to_wstring(mesh->SkeletalOutputVb ? 1 : 0) +
			L", hasIb=" + std::to_wstring(mesh->Ib ? 1 : 0) +
			L", vertexStride=" + std::to_wstring(mesh->VertexStride));
		return nullptr;
	}

	nri::AccelerationStructureDesc desc = {};
	desc.geometries = &geometry;
	desc.geometryOrInstanceNum = 1;
	desc.flags = nri::AccelerationStructureBits::PREFER_FAST_TRACE;
	if (allowUpdate)
		desc.flags |= nri::AccelerationStructureBits::ALLOW_UPDATE;
	desc.type = nri::AccelerationStructureType::BOTTOM_LEVEL;

	auto as = std::make_shared<NRIRTAS>();
	as->Owner = m;
	as->MeshPtr = mesh;
	as->BottomGeometry = geometry;
	as->AllowUpdate = allowUpdate;

	if (m->RT.CreateCommittedAccelerationStructure(*m->Device, nri::MemoryLocation::DEVICE, 0.0f, desc, as->AccelerationStructure) != nri::Result::SUCCESS ||
		!as->AccelerationStructure)
	{
		AppendCpuRuntimeTrace(L"[NRIRTAS] CreateCommittedAccelerationStructure BLAS failed");
		return nullptr;
	}

	uint64_t scratchSize = m->RT.GetAccelerationStructureBuildScratchBufferSize(*as->AccelerationStructure);
	if (allowUpdate && m->RT.GetAccelerationStructureUpdateScratchBufferSize)
		scratchSize = std::max<uint64_t>(scratchSize, m->RT.GetAccelerationStructureUpdateScratchBufferSize(*as->AccelerationStructure));
	if (scratchSize == 0 ||
		!m->CreateBoundBuffer(scratchSize, 0, nri::BufferUsageBits::SCRATCH_BUFFER, nri::MemoryLocation::DEVICE, as->ScratchBuffer, as->ScratchMemory))
	{
		AppendCpuRuntimeTrace(L"[NRIRTAS] BLAS scratch allocation failed size=" + std::to_wstring(scratchSize));
		return nullptr;
	}

	as->BuildPending = true;
	m->RT.CreateAccelerationStructureDescriptor(*as->AccelerationStructure, as->Descriptor);
	m->RayTracingAS.push_back(as);
	return as;
}

static void CollectPendingNRIBLASBuilds(
	const std::vector<RTInstanceDesc>& instances,
	std::vector<std::shared_ptr<NRIRTAS>>& pendingAS,
	std::vector<nri::BuildBottomLevelAccelerationStructureDesc>& builds)
{
	std::unordered_set<NRIRTAS*> seen;
	for (const RTInstanceDesc& instance : instances)
	{
		std::shared_ptr<NRIRTAS> blas = std::dynamic_pointer_cast<NRIRTAS>(instance.BottomLevelAS);
		if (!blas || !blas->BuildPending || !blas->AccelerationStructure || !blas->ScratchBuffer)
			continue;
		if (!seen.insert(blas.get()).second)
			continue;

		nri::BuildBottomLevelAccelerationStructureDesc build = {};
		build.dst = blas->AccelerationStructure;
		build.geometries = &blas->BottomGeometry;
		build.geometryNum = 1;
		build.scratchBuffer = blas->ScratchBuffer;
		pendingAS.push_back(blas);
		builds.push_back(build);
	}
}

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
		nri::Descriptor* descSingle[1] = {};
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
		m->EndRP();
		if (!EnsureInit()) return;

		for (Binding& b : Bindings)
			RefreshDescriptor(b);

		// Write descriptors into the sets.
		for (Binding& b : Bindings)
		{
			if (b.kind == ResKind::None || !b.desc || b.setIndex >= Sets.size() || !Sets[b.setIndex])
				continue;
			nri::UpdateDescriptorRangeDesc upd = {};
			b.descSingle[0] = b.desc;
			upd.descriptorSet = Sets[b.setIndex];
			upd.rangeIndex = b.rangeIndex;
			upd.baseDescriptor = 0;
			upd.descriptors = b.descSingle;
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

		// One descriptor set (registerSpace 0) holding all ranges — resources and
		// samplers share HLSL space0, and NRI requires a unique registerSpace per set.
		std::vector<nri::DescriptorRangeDesc> allRanges;
		uint32_t texN = 0, storTexN = 0, sbufN = 0, storSbufN = 0, cbvN = 0, sampN = 0;
		for (Binding& b : Bindings)
		{
			if (b.kind == ResKind::None) continue;
			nri::DescriptorRangeDesc r = {};
			r.baseRegisterIndex = b.reg;
			r.descriptorNum = 1;
			r.descriptorType = ToDescriptorType(b.kind);
			r.shaderStages = nri::StageBits::COMPUTE_SHADER;
			b.setIndex = 0; b.rangeIndex = (uint32_t)allRanges.size(); allRanges.push_back(r);
			switch (b.kind)
			{
			case ResKind::TexSRV: ++texN; break;
			case ResKind::TexUAV: ++storTexN; break;
			case ResKind::BufSRV: ++sbufN; break;
			case ResKind::BufUAV: ++storSbufN; break;
			case ResKind::CBV:    ++cbvN; break;
			case ResKind::Sampler:++sampN; break;
			default: break;
			}
		}

		std::vector<nri::DescriptorSetDesc> sets;
		if (!allRanges.empty()) { nri::DescriptorSetDesc s = {}; s.registerSpace = 0; s.ranges = allRanges.data(); s.rangeNum = (uint32_t)allRanges.size(); sets.push_back(s); }

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
			tvd.mipNum = 1;
			tvd.layerNum = 1;
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
// Ray tracing pipeline (by-name binding). NRI exposes a Vulkan-like global
// descriptor-set model for RT; it does not mirror the DX12 backend's local-root
// SBT records. This initial adapter is intentionally conservative: it supports
// the shadow pass' global TLAS/UAV/SRV/CBV/sampler bindings and scene-global
// hit resources, enough to bring up opaque direct-light shadows.
// ---------------------------------------------------------------------------
class NRIRTPipelineStateObject : public RTPipelineStateObject
{
public:
	explicit NRIRTPipelineStateObject(NRIBackend::Impl* impl) : m(impl) {}
	~NRIRTPipelineStateObject() override
	{
		if (!m || !m->Device) return;
		if (Pipeline) m->Core.DestroyPipeline(Pipeline);
		if (Layout) m->Core.DestroyPipelineLayout(Layout);
		if (Pool) m->Core.DestroyDescriptorPool(Pool);
		for (Binding& b : Bindings)
		{
			if (b.ownsDesc && b.desc)
				m->Core.DestroyDescriptor(b.desc);
			for (nri::Descriptor* desc : b.descArray)
			{
				if (desc)
					m->Core.DestroyDescriptor(desc);
			}
		}
		for (auto& kv : CbvBuffers)
		{
			if (kv.second.view)
				m->Core.DestroyDescriptor(kv.second.view);
			m->FreeBuffer(kv.second.buffer, kv.second.memory);
		}
		m->FreeBuffer(SbtBuffer, SbtMemory);
	}

	void SetNumInstances(uint32_t numInstances) override { NumInstances = std::max(1u, numInstances); }
	void Configure(uint32_t maxRecursion, uint32_t maxPayloadSizeInBytes, uint32_t maxAttributeSizeInBytes) override
	{
		MaxRecursion = maxRecursion;
		MaxPayloadSize = maxPayloadSizeInBytes;
		MaxAttributeSize = maxAttributeSizeInBytes;
	}
	void AddHitGroup(const std::string& name, const std::string& chs, const std::string& ahs) override
	{
		HitGroups.push_back({ name, chs, ahs });
	}
	void AddShader(const std::string& shader, RTPipelineStateObject::ShaderType shaderType) override
	{
		Shaders.push_back({ shader, shaderType });
	}
	void BindUAV(const std::string& shader, const std::string& name, uint32_t baseRegister) override { AddBinding(shader, name, baseRegister, RegClass::UAV, 0); }
	void BindSRV(const std::string& shader, const std::string& name, uint32_t baseRegister) override { AddBinding(shader, name, baseRegister, RegClass::SRV, 0); }
	void BindSampler(const std::string& shader, const std::string& name, uint32_t baseRegister) override { AddBinding(shader, name, baseRegister, RegClass::Sampler, 0); }
	void BindCBV(const std::string& shader, const std::string& name, uint32_t baseRegister, uint32_t size, uint32_t) override { AddBinding(shader, name, baseRegister, RegClass::CBV, size); }
	void SetShaderDefine(const std::string& name, const std::string& value) override
	{
		if (!name.empty())
		{
			ShaderDefines.push_back({ name, value.empty() ? "1" : value });
			if (name == "CORONA_NRI_RT_HIT_RESOURCE_ARRAYS")
				UseHitResourceArrays = true;
		}
	}
	void SetShaderLibraryTarget(const std::string& target) override { if (!target.empty()) ShaderLibraryTarget = target; }
	void BeginShaderTable() override {}
	void EndShaderTable() override {}

	void SetTextureUAV(const std::string& shader, const std::string& bindingName, Texture* texture, int = -1) override
	{
		if (Binding* b = Find(shader, bindingName)) { b->kind = ResKind::TexUAV; b->tex = texture; }
	}
	void SetBufferUAV(const std::string& shader, const std::string& bindingName, Buffer* buffer, int = -1) override
	{
		if (Binding* b = Find(shader, bindingName)) { b->kind = ResKind::BufUAV; b->buf = buffer; }
	}
	void SetTextureSRV(const std::string& shader, const std::string& bindingName, Texture* texture, int = -1) override
	{
		if (Binding* b = Find(shader, bindingName)) { b->kind = ResKind::TexSRV; b->tex = texture; }
	}
	void SetBufferSRV(const std::string& shader, const std::string& bindingName, Buffer* buffer, int = -1) override
	{
		if (Binding* b = Find(shader, bindingName)) { b->kind = ResKind::BufSRV; b->buf = buffer; }
	}
	void SetAccelerationStructure(const std::string& shader, const std::string& bindingName, const std::shared_ptr<RTAS>& rtas, int = -1) override
	{
		if (Binding* b = Find(shader, bindingName)) { b->kind = ResKind::Accel; b->rtas = rtas; }
	}
	void SetSampler(const std::string& shader, const std::string& bindingName, Sampler* sampler, int = -1) override
	{
		if (Binding* b = Find(shader, bindingName)) { b->kind = ResKind::Sampler; b->samp = sampler; }
	}
	void SetCBVValue(const std::string& shader, const std::string& bindingName, void* pData, int = -1) override
	{
		if (!m || !m->Device || !pData)
			return;
		Binding* b = Find(shader, bindingName);
		if (!b || b->cbSize == 0)
			return;

		const uint32_t alignedSize = (b->cbSize + 255u) & ~255u;
		CbvBuf& cb = CbvBuffers[bindingName];
		if (!cb.buffer)
		{
			if (!m->CreateBoundBuffer(alignedSize, 0, nri::BufferUsageBits::CONSTANT_BUFFER, nri::MemoryLocation::HOST_UPLOAD, cb.buffer, cb.memory))
				return;
			nri::BufferViewDesc bvd = {};
			bvd.buffer = cb.buffer;
			bvd.type = nri::BufferView::CONSTANT_BUFFER;
			bvd.offset = 0;
			bvd.size = alignedSize;
			if (m->Core.CreateBufferView(bvd, cb.view) != nri::Result::SUCCESS)
				return;
		}
		void* mapped = m->Core.MapBuffer(*cb.buffer, 0, b->cbSize);
		if (mapped)
		{
			memcpy(mapped, pData, b->cbSize);
			m->Core.UnmapBuffer(*cb.buffer);
		}
		b->kind = ResKind::CBV;
		b->desc = cb.view;
		b->ownsDesc = false;
	}

	void ResetHitProgram(uint32_t) override { HitBindSlot = 0; }
	void StartHitProgram(const std::string&, uint32_t) override { HitBindSlot = 0; }
	void AddTextureSRVToHitProgram(const std::string&, Texture* texture, uint32_t instanceIndex) override
	{
		if (Binding* b = FindHitBindingAtSlot(HitBindSlot))
		{
			b->kind = ResKind::TexSRV;
			if (UsesHitDescriptorArray(*b))
			{
				EnsureHitArraySize(*b);
				if (instanceIndex < b->texArray.size())
					b->texArray[instanceIndex] = texture;
			}
			else if (!b->tex)
			{
				b->tex = texture;
			}
		}
		++HitBindSlot;
	}
	void AddBufferSRVToHitProgram(const std::string&, Buffer* buffer, uint32_t instanceIndex) override
	{
		if (Binding* b = FindHitBindingAtSlot(HitBindSlot))
		{
			b->kind = ResKind::BufSRV;
			if (UsesHitDescriptorArray(*b))
			{
				EnsureHitArraySize(*b);
				if (instanceIndex < b->bufArray.size())
					b->bufArray[instanceIndex] = buffer;
			}
			else
			{
				b->buf = buffer;
			}
		}
		++HitBindSlot;
	}
	void AddSceneGeometrySRVsToHitProgram(const std::string&, VertexBuffer* sceneVertexBuffer, IndexBuffer* sceneIndexBuffer, uint32_t) override
	{
		if (Binding* b = FindHitBindingAtSlot(HitBindSlot))
		{
			if (!b->vb)
			{
				b->kind = ResKind::VertexSRV;
				b->vb = sceneVertexBuffer;
			}
		}
		if (Binding* b = FindHitBindingAtSlot(HitBindSlot + 1))
		{
			if (!b->ib)
			{
				b->kind = ResKind::IndexSRV;
				b->ib = sceneIndexBuffer;
			}
		}
		HitBindSlot += 2;
	}

	bool InitRS(const std::string& shaderFile) override
	{
		if (!m || !m->Device || !m->HasRayTracing)
			return false;

		std::filesystem::path shaderPath(shaderFile);
		if (shaderPath.is_relative())
			shaderPath = RuntimePaths::SourceDirectory() / shaderPath;
		std::ifstream file(shaderPath, std::ios::binary);
		if (!file.good())
		{
			AppendCpuRuntimeTrace(L"[NRIRTPSO] shader file not found: " + shaderPath.wstring());
			return false;
		}
		std::stringstream ss;
		ss << file.rdbuf();
		const std::string source = BuildShaderDefinePrefix() + ss.str();

		if (!BuildLayout())
			return false;

		std::vector<nri::ShaderDesc> shaderDescs;
		shaderDescs.reserve(Shaders.size());
		for (ShaderEntry& shader : Shaders)
		{
			const std::string entrySource = KeepOnlyShaderEntry(source, shader.name);
			std::string error;
			const std::wstring sourceName = shaderPath.wstring();
			const std::wstring entryName(shader.name.begin(), shader.name.end());
			const std::wstring target(ShaderLibraryTarget.begin(), ShaderLibraryTarget.end());
			shader.dxil = CompileHLSLToDXIL(entrySource.data(), entrySource.size(), sourceName.c_str(), entryName.c_str(), target.c_str(), error);
			if (shader.dxil.empty())
			{
				AppendCpuRuntimeTrace(L"[NRIRTPSO] DXIL compile failed shader=\"" + sourceName + L"\" entry=\"" + entryName + L"\" target=\"" + target + L"\"");
				if (!error.empty())
					AppendCpuRuntimeTrace(L"[NRIRTPSO] DXIL error: " + std::wstring(error.begin(), error.end()));
				return false;
			}

			nri::ShaderDesc desc = {};
			desc.stage = ToStage(shader.type);
			desc.bytecode = shader.dxil.data();
			desc.size = shader.dxil.size();
			desc.entryPointName = shader.name.c_str();
			shaderDescs.push_back(desc);
		}

		nri::ShaderLibraryDesc library = {};
		library.shaders = shaderDescs.data();
		library.shaderNum = static_cast<uint32_t>(shaderDescs.size());

		std::vector<nri::ShaderGroupDesc> groups;
		RaygenGroupCount = MissGroupCount = HitGroupCount = 0;
		for (uint32_t i = 0; i < static_cast<uint32_t>(Shaders.size()); ++i)
		{
			const ShaderEntry& shader = Shaders[i];
			if (shader.type != RTPipelineStateObject::RAYGEN && shader.type != RTPipelineStateObject::MISS)
				continue;
			nri::ShaderGroupDesc group = {};
			group.shaderIndices[0] = i + 1;
			groups.push_back(group);
			if (shader.type == RTPipelineStateObject::RAYGEN)
				++RaygenGroupCount;
			else
				++MissGroupCount;
		}
		HitGroupStart = static_cast<uint32_t>(groups.size());
		for (const HitGroupEntry& hitGroup : HitGroups)
		{
			nri::ShaderGroupDesc group = {};
			uint32_t outIndex = 0;
			if (!hitGroup.chs.empty())
				group.shaderIndices[outIndex++] = FindShaderIndex(hitGroup.chs);
			if (!hitGroup.ahs.empty())
				group.shaderIndices[outIndex++] = FindShaderIndex(hitGroup.ahs);
			if (outIndex == 0)
				continue;
			groups.push_back(group);
			++HitGroupCount;
		}
		if (RaygenGroupCount == 0 || MissGroupCount == 0 || groups.empty())
			return false;

		nri::RayTracingPipelineDesc rtd = {};
		rtd.pipelineLayout = Layout;
		rtd.shaderLibrary = &library;
		rtd.shaderGroups = groups.data();
		rtd.shaderGroupNum = static_cast<uint32_t>(groups.size());
		rtd.recursionMaxDepth = MaxRecursion;
		rtd.rayPayloadMaxSize = MaxPayloadSize;
		rtd.rayHitAttributeMaxSize = MaxAttributeSize;
		if (m->RT.CreateRayTracingPipeline(*m->Device, rtd, Pipeline) != nri::Result::SUCCESS || !Pipeline)
		{
			AppendCpuRuntimeTrace(L"[NRIRTPSO] CreateRayTracingPipeline failed shader=\"" + shaderPath.wstring() + L"\"");
			return false;
		}

		return BuildShaderBindingTable(static_cast<uint32_t>(groups.size()));
	}

	void Apply(uint32_t width, uint32_t height) override
	{
		if (!m || !m->ActiveCmd || !Pipeline || !Layout || !SbtBuffer)
			return;
		m->EndRP();

		for (Binding& b : Bindings)
		{
			if (b.descriptorNum > 1)
				RefreshDescriptorArray(b);
			else
				RefreshDescriptor(b);
		}

		if (!Sets.empty())
		{
			for (Binding& b : Bindings)
			{
				if (b.rangeIndex == UINT32_MAX || b.setIndex >= Sets.size() || !Sets[b.setIndex])
					continue;
				nri::UpdateDescriptorRangeDesc upd = {};
				upd.descriptorSet = Sets[b.setIndex];
				upd.rangeIndex = b.rangeIndex;
				upd.baseDescriptor = 0;
				if (b.descriptorNum > 1)
				{
					if (b.descArray.size() < b.descriptorNum)
						continue;
					bool allDescriptorsValid = true;
					for (uint32_t i = 0; i < b.descriptorNum; ++i)
					{
						if (!b.descArray[i])
						{
							allDescriptorsValid = false;
							break;
						}
					}
					if (!allDescriptorsValid)
						continue;
					upd.descriptors = b.descArray.data();
					upd.descriptorNum = b.descriptorNum;
				}
				else
				{
					if (!b.desc)
						continue;
					b.descSingle[0] = b.desc;
					upd.descriptors = b.descSingle;
					upd.descriptorNum = 1;
				}
				m->Core.UpdateDescriptorRanges(&upd, 1);
			}
			m->Core.CmdSetDescriptorPool(*m->ActiveCmd, *Pool);
		}
		m->Core.CmdSetPipelineLayout(*m->ActiveCmd, nri::BindPoint::RAY_TRACING, *Layout);
		for (uint32_t setIndex = 0; setIndex < Sets.size(); ++setIndex)
		{
			if (!Sets[setIndex])
				continue;
			nri::SetDescriptorSetDesc sd = {};
			sd.setIndex = setIndex;
			sd.descriptorSet = Sets[setIndex];
			sd.bindPoint = nri::BindPoint::RAY_TRACING;
			m->Core.CmdSetDescriptorSet(*m->ActiveCmd, sd);
		}
		m->Core.CmdSetPipeline(*m->ActiveCmd, *Pipeline);

		nri::DispatchRaysDesc dispatch = {};
		dispatch.raygenShader.buffer = SbtBuffer;
		dispatch.raygenShader.offset = RaygenOffset;
		dispatch.raygenShader.size = SbtEntrySize;
		dispatch.raygenShader.stride = SbtEntrySize;
		dispatch.missShaders.buffer = SbtBuffer;
		dispatch.missShaders.offset = MissOffset;
		dispatch.missShaders.size = SbtEntrySize * std::max(1u, MissGroupCount);
		dispatch.missShaders.stride = SbtEntrySize;
		if (HitGroupCount > 0)
		{
			dispatch.hitShaderGroups.buffer = SbtBuffer;
			dispatch.hitShaderGroups.offset = HitOffset;
			dispatch.hitShaderGroups.size = SbtEntrySize * HitGroupCount;
			dispatch.hitShaderGroups.stride = SbtEntrySize;
		}
		dispatch.x = width;
		dispatch.y = height;
		dispatch.z = 1;
		m->RT.CmdDispatchRays(*m->ActiveCmd, dispatch);
	}

private:
	enum class RegClass { SRV, UAV, CBV, Sampler };
	enum class ResKind { None, TexSRV, TexUAV, BufSRV, BufUAV, CBV, Sampler, Accel, VertexSRV, IndexSRV };
	struct ShaderEntry { std::string name; RTPipelineStateObject::ShaderType type = RTPipelineStateObject::GLOBAL; std::vector<uint8_t> dxil; };
	struct HitGroupEntry { std::string name; std::string chs; std::string ahs; };
	struct CbvBuf { nri::Buffer* buffer = nullptr; std::vector<nri::Memory*> memory; nri::Descriptor* view = nullptr; };
	struct Binding
	{
		std::string shader;
		std::string name;
		uint32_t reg = 0;
		uint32_t registerSpace = 0;
		uint32_t setIndex = UINT32_MAX;
		uint32_t cbSize = 0;
		uint32_t rangeIndex = UINT32_MAX;
		uint32_t descriptorNum = 1;
		RegClass regClass = RegClass::SRV;
		ResKind kind = ResKind::None;
		nri::DescriptorType descriptorType = nri::DescriptorType::TEXTURE;
		nri::Descriptor* desc = nullptr;
		nri::Descriptor* descSingle[1] = {};
		bool ownsDesc = false;
		void* last = nullptr;
		std::vector<nri::Descriptor*> descArray;
		std::vector<void*> lastArray;
		Texture* tex = nullptr;
		Buffer* buf = nullptr;
		Sampler* samp = nullptr;
		VertexBuffer* vb = nullptr;
		IndexBuffer* ib = nullptr;
		std::vector<Texture*> texArray;
		std::vector<Buffer*> bufArray;
		std::shared_ptr<RTAS> rtas;
	};

	void AddBinding(const std::string& shader, const std::string& name, uint32_t reg, RegClass regClass, uint32_t cbSize)
	{
		if (Find(shader, name))
			return;
		Binding b;
		b.shader = shader;
		b.name = name;
		b.reg = reg;
		if (UseHitResourceArrays && shader != "global" && regClass == RegClass::SRV && !IsSharedHitBindingName(name))
			b.registerSpace = 1;
		b.regClass = regClass;
		b.cbSize = cbSize;
		b.descriptorType = InferDescriptorType(b);
		Bindings.push_back(b);
	}

	Binding* Find(const std::string& shader, const std::string& name)
	{
		for (Binding& b : Bindings)
			if (b.shader == shader && b.name == name)
				return &b;
		for (Binding& b : Bindings)
			if (b.name == name)
				return &b;
		return nullptr;
	}
	Binding* FindHitBinding(const std::string& name)
	{
		for (Binding& b : Bindings)
			if (b.shader != "global" && b.name == name)
				return &b;
		return Find("global", name);
	}
	Binding* FindHitBindingAtSlot(uint32_t slot)
	{
		uint32_t current = 0;
		for (Binding& b : Bindings)
		{
			if (b.shader == "global" || b.regClass != RegClass::SRV)
				continue;
			if (current == slot)
				return &b;
			++current;
		}
		return nullptr;
	}

	static bool IsSharedHitBindingName(const std::string& name)
	{
		return name == "vertices" || name == "indices" || name == "InstanceProperty";
	}

	bool UsesHitDescriptorArray(const Binding& b) const
	{
		return UseHitResourceArrays && b.shader != "global" && b.regClass == RegClass::SRV && !IsSharedHitBindingName(b.name) && NumInstances > 1;
	}

	uint32_t BindingDescriptorCount(const Binding& b) const
	{
		return UsesHitDescriptorArray(b) ? std::max(1u, NumInstances) : 1u;
	}

	void EnsureHitArraySize(Binding& b)
	{
		const uint32_t count = std::max(std::max(1u, NumInstances), b.descriptorNum);
		if (b.texArray.size() < count)
			b.texArray.resize(count, nullptr);
		if (b.bufArray.size() < count)
			b.bufArray.resize(count, nullptr);
	}

	static nri::StageBits ToStage(RTPipelineStateObject::ShaderType type)
	{
		switch (type)
		{
		case RTPipelineStateObject::RAYGEN: return nri::StageBits::RAYGEN_SHADER;
		case RTPipelineStateObject::MISS: return nri::StageBits::MISS_SHADER;
		case RTPipelineStateObject::HIT: return nri::StageBits::CLOSEST_HIT_SHADER;
		case RTPipelineStateObject::ANYHIT: return nri::StageBits::ANY_HIT_SHADER;
		default: return nri::StageBits::RAY_TRACING_SHADERS;
		}
	}

	nri::DescriptorType InferDescriptorType(const Binding& b) const
	{
		if (b.regClass == RegClass::UAV)
			return (b.name.find("Buffer") != std::string::npos) ? nri::DescriptorType::STORAGE_STRUCTURED_BUFFER : nri::DescriptorType::STORAGE_TEXTURE;
		if (b.regClass == RegClass::CBV)
			return nri::DescriptorType::CONSTANT_BUFFER;
		if (b.regClass == RegClass::Sampler)
			return nri::DescriptorType::SAMPLER;
		if (b.name == "gRtScene")
			return nri::DescriptorType::ACCELERATION_STRUCTURE;
		if (b.name == "vertices" || b.name == "indices" || b.name == "InstanceProperty" ||
			b.name.find("Cell") != std::string::npos || b.name.find("Keys") != std::string::npos || b.name.find("Mask") != std::string::npos)
		{
			return nri::DescriptorType::STRUCTURED_BUFFER;
		}
		return nri::DescriptorType::TEXTURE;
	}

	bool BuildLayout()
	{
		struct SetBuild
		{
			uint32_t registerSpace = 0;
			std::vector<nri::DescriptorRangeDesc> ranges;
		};

		std::vector<SetBuild> setBuilds;
		uint32_t textureNum = 0, storageTextureNum = 0, structuredBufferNum = 0, storageStructuredBufferNum = 0;
		uint32_t cbvNum = 0, samplerNum = 0, accelNum = 0;

		for (Binding& b : Bindings)
		{
			uint32_t setIndex = UINT32_MAX;
			for (uint32_t i = 0; i < setBuilds.size(); ++i)
			{
				if (setBuilds[i].registerSpace == b.registerSpace)
				{
					setIndex = i;
					break;
				}
			}
			if (setIndex == UINT32_MAX)
			{
				setIndex = static_cast<uint32_t>(setBuilds.size());
				SetBuild setBuild;
				setBuild.registerSpace = b.registerSpace;
				setBuild.ranges.reserve(Bindings.size());
				setBuilds.push_back(std::move(setBuild));
			}

			const uint32_t descriptorCount = BindingDescriptorCount(b);
			nri::DescriptorRangeDesc range = {};
			range.baseRegisterIndex = b.reg;
			range.descriptorNum = descriptorCount;
			range.descriptorType = b.descriptorType;
			range.shaderStages = nri::StageBits::RAY_TRACING_SHADERS;
			range.flags = nri::DescriptorRangeBits::PARTIALLY_BOUND;
			if (descriptorCount > 1)
				range.flags |= nri::DescriptorRangeBits::ARRAY;
			b.setIndex = setIndex;
			b.rangeIndex = static_cast<uint32_t>(setBuilds[setIndex].ranges.size());
			b.descriptorNum = descriptorCount;
			setBuilds[setIndex].ranges.push_back(range);

			switch (b.descriptorType)
			{
			case nri::DescriptorType::TEXTURE: textureNum += descriptorCount; break;
			case nri::DescriptorType::STORAGE_TEXTURE: storageTextureNum += descriptorCount; break;
			case nri::DescriptorType::STRUCTURED_BUFFER: structuredBufferNum += descriptorCount; break;
			case nri::DescriptorType::STORAGE_STRUCTURED_BUFFER: storageStructuredBufferNum += descriptorCount; break;
			case nri::DescriptorType::CONSTANT_BUFFER: cbvNum += descriptorCount; break;
			case nri::DescriptorType::SAMPLER: samplerNum += descriptorCount; break;
			case nri::DescriptorType::ACCELERATION_STRUCTURE: accelNum += descriptorCount; break;
			default: break;
			}
		}

		std::vector<nri::DescriptorSetDesc> sets;
		sets.reserve(setBuilds.size());
		for (const SetBuild& setBuild : setBuilds)
		{
			nri::DescriptorSetDesc set = {};
			set.registerSpace = setBuild.registerSpace;
			set.ranges = setBuild.ranges.data();
			set.rangeNum = static_cast<uint32_t>(setBuild.ranges.size());
			sets.push_back(set);
		}

		nri::PipelineLayoutDesc pld = {};
		pld.rootRegisterSpace = 0;
		pld.descriptorSets = sets.empty() ? nullptr : sets.data();
		pld.descriptorSetNum = static_cast<uint32_t>(sets.size());
		pld.shaderStages = nri::StageBits::RAY_TRACING_SHADERS;
		if (m->Core.CreatePipelineLayout(*m->Device, pld, Layout) != nri::Result::SUCCESS || !Layout)
			return false;

		if (!sets.empty())
		{
			nri::DescriptorPoolDesc pd = {};
			pd.descriptorSetMaxNum = static_cast<uint32_t>(sets.size());
			pd.textureMaxNum = textureNum;
			pd.storageTextureMaxNum = storageTextureNum;
			pd.structuredBufferMaxNum = structuredBufferNum;
			pd.storageStructuredBufferMaxNum = storageStructuredBufferNum;
			pd.constantBufferMaxNum = cbvNum;
			pd.samplerMaxNum = samplerNum;
			pd.accelerationStructureMaxNum = accelNum;
			if (m->Core.CreateDescriptorPool(*m->Device, pd, Pool) != nri::Result::SUCCESS || !Pool)
				return false;
			Sets.resize(sets.size(), nullptr);
			for (uint32_t setIndex = 0; setIndex < Sets.size(); ++setIndex)
			{
				if (m->Core.AllocateDescriptorSets(*Pool, *Layout, setIndex, &Sets[setIndex], 1, 0) != nri::Result::SUCCESS || !Sets[setIndex])
					return false;
			}
		}
		return true;
	}

	uint32_t FindShaderIndex(const std::string& name) const
	{
		for (uint32_t i = 0; i < static_cast<uint32_t>(Shaders.size()); ++i)
			if (Shaders[i].name == name)
				return i + 1;
		return 0;
	}

	static size_t FindFunctionEnd(const std::string& source, size_t functionNamePos)
	{
		const size_t open = source.find('{', functionNamePos);
		if (open == std::string::npos)
			return std::string::npos;
		int depth = 0;
		for (size_t i = open; i < source.size(); ++i)
		{
			if (source[i] == '{')
				++depth;
			else if (source[i] == '}')
			{
				--depth;
				if (depth == 0)
					return i + 1;
			}
		}
		return std::string::npos;
	}

	static std::string ShaderFunctionNameAt(const std::string& source, size_t marker)
	{
		const size_t voidPos = source.find("void", marker);
		if (voidPos == std::string::npos)
			return {};
		size_t namePos = voidPos + 4;
		while (namePos < source.size() && isspace(static_cast<unsigned char>(source[namePos])))
			++namePos;
		size_t nameEnd = namePos;
		while (nameEnd < source.size() && (isalnum(static_cast<unsigned char>(source[nameEnd])) || source[nameEnd] == '_'))
			++nameEnd;
		return source.substr(namePos, nameEnd - namePos);
	}

	static std::string KeepOnlyShaderEntry(const std::string& source, const std::string& entryName)
	{
		std::string out;
		size_t pos = 0;
		for (;;)
		{
			const size_t marker = source.find("[shader(", pos);
			if (marker == std::string::npos)
			{
				out.append(source, pos, std::string::npos);
				break;
			}
			const std::string fn = ShaderFunctionNameAt(source, marker);
			const size_t fnPos = source.find("void", marker);
			const size_t end = FindFunctionEnd(source, fnPos == std::string::npos ? marker : fnPos);
			if (end == std::string::npos)
			{
				out.append(source, pos, std::string::npos);
				break;
			}
			if (fn == entryName)
				out.append(source, pos, end - pos);
			else
				out.append(source, pos, marker - pos);
			pos = end;
		}
		return out;
	}

	std::string BuildShaderDefinePrefix() const
	{
		std::string prefix;
		for (const auto& define : ShaderDefines)
		{
			prefix += "#ifndef ";
			prefix += define.first;
			prefix += "\n#define ";
			prefix += define.first;
			prefix += " ";
			prefix += define.second.empty() ? "1" : define.second;
			prefix += "\n#endif\n";
		}
		if (!prefix.empty())
			prefix += "\n";
		return prefix;
	}

	bool BuildShaderBindingTable(uint32_t groupCount)
	{
		const nri::DeviceDesc& dd = m->Core.GetDeviceDesc(*m->Device);
		const uint32_t idSize = dd.shaderStage.rayTracing.shaderGroupIdentifierSize;
		const uint64_t alignment = std::max<uint32_t>(1u, dd.memoryAlignment.shaderBindingTable);
		SbtEntrySize = AlignUpU64(idSize, alignment);
		RaygenOffset = 0;
		MissOffset = AlignUpU64(RaygenOffset + SbtEntrySize * std::max(1u, RaygenGroupCount), alignment);
		HitOffset = AlignUpU64(MissOffset + SbtEntrySize * std::max(1u, MissGroupCount), alignment);
		const uint64_t sbtSize = HitOffset + SbtEntrySize * std::max(1u, HitGroupCount);
		if (idSize == 0 || !m->CreateBoundBuffer(sbtSize, 0, nri::BufferUsageBits::SHADER_BINDING_TABLE, nri::MemoryLocation::HOST_UPLOAD, SbtBuffer, SbtMemory))
			return false;

		uint8_t* mapped = static_cast<uint8_t*>(m->Core.MapBuffer(*SbtBuffer, 0, sbtSize));
		if (!mapped)
			return false;
		memset(mapped, 0, static_cast<size_t>(sbtSize));
		auto writeGroups = [&](uint32_t groupIndex, uint32_t groupNum, uint64_t offset)
		{
			return groupNum == 0 ||
				(groupIndex < groupCount && groupIndex + groupNum <= groupCount &&
					m->RT.WriteShaderGroupIdentifiers(*Pipeline, groupIndex, groupNum, mapped + offset) == nri::Result::SUCCESS);
		};
		bool ok = writeGroups(0, RaygenGroupCount, RaygenOffset);
		ok = writeGroups(RaygenGroupCount, MissGroupCount, MissOffset) && ok;
		if (HitGroupCount > 0)
			ok = writeGroups(HitGroupStart, HitGroupCount, HitOffset) && ok;
		m->Core.UnmapBuffer(*SbtBuffer);
		return ok;
	}

	void ResetOwnedDescriptor(Binding& b)
	{
		if (b.ownsDesc && b.desc)
			m->Core.DestroyDescriptor(b.desc);
		b.desc = nullptr;
		b.ownsDesc = false;
	}

	void ResetOwnedDescriptorArraySlot(Binding& b, uint32_t index)
	{
		if (index < b.descArray.size() && b.descArray[index])
		{
			m->Core.DestroyDescriptor(b.descArray[index]);
			b.descArray[index] = nullptr;
		}
	}

	Texture* FirstTextureArrayValue(const Binding& b) const
	{
		for (Texture* texture : b.texArray)
			if (texture)
				return texture;
		return b.tex;
	}

	Buffer* FirstBufferArrayValue(const Binding& b) const
	{
		for (Buffer* buffer : b.bufArray)
			if (buffer)
				return buffer;
		return b.buf;
	}

	void RefreshDescriptorArray(Binding& b)
	{
		if (b.kind == ResKind::None || b.descriptorNum <= 1)
			return;

		EnsureHitArraySize(b);
		if (b.descArray.size() < b.descriptorNum)
			b.descArray.resize(b.descriptorNum, nullptr);
		if (b.lastArray.size() < b.descriptorNum)
			b.lastArray.resize(b.descriptorNum, nullptr);

		Texture* fallbackTexture = FirstTextureArrayValue(b);
		Buffer* fallbackBuffer = FirstBufferArrayValue(b);
		for (uint32_t i = 0; i < b.descriptorNum; ++i)
		{
			void* current = nullptr;
			Texture* texture = nullptr;
			Buffer* buffer = nullptr;

			if (b.kind == ResKind::TexSRV || b.kind == ResKind::TexUAV)
			{
				texture = (i < b.texArray.size() && b.texArray[i]) ? b.texArray[i] : fallbackTexture;
				current = texture;
			}
			else if (b.kind == ResKind::BufSRV || b.kind == ResKind::BufUAV)
			{
				buffer = (i < b.bufArray.size() && b.bufArray[i]) ? b.bufArray[i] : fallbackBuffer;
				current = buffer;
			}
			if (!current || (b.lastArray[i] == current && b.descArray[i]))
				continue;

			ResetOwnedDescriptorArraySlot(b, i);
			b.lastArray[i] = current;

			if (texture)
			{
				auto it = m->Textures.find(texture);
				if (it == m->Textures.end() || !it->second.texture)
					continue;
				const nri::TextureDesc& td = m->Core.GetTextureDesc(*it->second.texture);
				nri::TextureViewDesc tvd = {};
				tvd.texture = it->second.texture;
				tvd.type = (b.kind == ResKind::TexUAV) ? nri::TextureView::STORAGE_TEXTURE : nri::TextureView::TEXTURE;
				tvd.format = td.format;
				tvd.mipNum = nri::REMAINING;
				tvd.layerNum = nri::REMAINING;
				nri::Descriptor* d = nullptr;
				if (m->Core.CreateTextureView(tvd, d) == nri::Result::SUCCESS)
					b.descArray[i] = d;
				continue;
			}

			if (buffer)
			{
				auto it = m->Buffers.find(buffer);
				if (it == m->Buffers.end() || !it->second.buffer)
					continue;
				nri::BufferViewDesc bvd = {};
				bvd.buffer = it->second.buffer;
				bvd.offset = 0;
				bvd.size = static_cast<uint64_t>(buffer->NumElements) * buffer->ElementSize;
				if (buffer->Type == Buffer::BYTE_ADDRESS)
					bvd.type = (b.kind == ResKind::BufUAV) ? nri::BufferView::STORAGE_BYTE_ADDRESS_BUFFER : nri::BufferView::BYTE_ADDRESS_BUFFER;
				else
				{
					bvd.type = (b.kind == ResKind::BufUAV) ? nri::BufferView::STORAGE_STRUCTURED_BUFFER : nri::BufferView::STRUCTURED_BUFFER;
					bvd.structureStride = buffer->ElementSize ? buffer->ElementSize : 4;
				}
				nri::Descriptor* d = nullptr;
				if (bvd.size > 0 && m->Core.CreateBufferView(bvd, d) == nri::Result::SUCCESS)
					b.descArray[i] = d;
			}
		}
	}

	void RefreshDescriptor(Binding& b)
	{
		if (b.kind == ResKind::None)
			return;
		if (b.kind == ResKind::CBV || b.kind == ResKind::Sampler || b.kind == ResKind::Accel)
		{
			if (b.kind == ResKind::Sampler && b.samp)
			{
				auto it = m->Samplers.find(b.samp);
				b.desc = (it != m->Samplers.end()) ? it->second : nullptr;
				b.ownsDesc = false;
			}
			else if (b.kind == ResKind::Accel)
			{
				std::shared_ptr<NRIRTAS> as = std::dynamic_pointer_cast<NRIRTAS>(b.rtas);
				b.desc = as ? as->Descriptor : nullptr;
				b.ownsDesc = false;
			}
			return;
		}

		void* current = nullptr;
		if (b.tex) current = b.tex;
		else if (b.buf) current = b.buf;
		else if (b.vb) current = b.vb;
		else if (b.ib) current = b.ib;
		if (!current)
			return;
		if (b.last == current && b.desc)
			return;
		ResetOwnedDescriptor(b);
		b.last = current;

		if (b.kind == ResKind::TexSRV || b.kind == ResKind::TexUAV)
		{
			auto it = m->Textures.find(b.tex);
			if (it == m->Textures.end() || !it->second.texture)
				return;
			const nri::TextureDesc& td = m->Core.GetTextureDesc(*it->second.texture);
			nri::TextureViewDesc tvd = {};
			tvd.texture = it->second.texture;
			tvd.type = (b.kind == ResKind::TexUAV) ? nri::TextureView::STORAGE_TEXTURE : nri::TextureView::TEXTURE;
			tvd.format = td.format;
			tvd.mipNum = nri::REMAINING;
			tvd.layerNum = nri::REMAINING;
			nri::Descriptor* d = nullptr;
			if (m->Core.CreateTextureView(tvd, d) == nri::Result::SUCCESS)
			{
				b.desc = d;
				b.ownsDesc = true;
			}
			return;
		}

		nri::BufferViewDesc bvd = {};
		if (b.kind == ResKind::BufSRV || b.kind == ResKind::BufUAV)
		{
			auto it = m->Buffers.find(b.buf);
			if (it == m->Buffers.end() || !it->second.buffer || !b.buf)
				return;
			bvd.buffer = it->second.buffer;
			bvd.offset = 0;
			bvd.size = static_cast<uint64_t>(b.buf->NumElements) * b.buf->ElementSize;
			if (b.buf->Type == Buffer::BYTE_ADDRESS)
				bvd.type = (b.kind == ResKind::BufUAV) ? nri::BufferView::STORAGE_BYTE_ADDRESS_BUFFER : nri::BufferView::BYTE_ADDRESS_BUFFER;
			else
			{
				bvd.type = (b.kind == ResKind::BufUAV) ? nri::BufferView::STORAGE_STRUCTURED_BUFFER : nri::BufferView::STRUCTURED_BUFFER;
				bvd.structureStride = b.buf->ElementSize ? b.buf->ElementSize : 4;
			}
		}
		else if (b.kind == ResKind::VertexSRV)
		{
			auto it = m->VBs.find(b.vb);
			if (it == m->VBs.end() || !it->second.buffer)
				return;
			bvd.buffer = it->second.buffer;
			bvd.type = nri::BufferView::BYTE_ADDRESS_BUFFER;
			bvd.offset = 0;
			bvd.size = it->second.capacity;
		}
		else if (b.kind == ResKind::IndexSRV)
		{
			auto it = m->IBs.find(b.ib);
			if (it == m->IBs.end() || !it->second.buffer)
				return;
			bvd.buffer = it->second.buffer;
			bvd.type = nri::BufferView::BYTE_ADDRESS_BUFFER;
			bvd.offset = 0;
			const uint32_t indexSize = it->second.indexType == nri::IndexType::UINT16 ? 2u : 4u;
			bvd.size = static_cast<uint64_t>(b.ib ? b.ib->numIndices : 0) * indexSize;
		}
		if (!bvd.buffer || bvd.size == 0)
			return;
		nri::Descriptor* d = nullptr;
		if (m->Core.CreateBufferView(bvd, d) == nri::Result::SUCCESS)
		{
			b.desc = d;
			b.ownsDesc = true;
		}
	}

	NRIBackend::Impl* m = nullptr;
	std::vector<ShaderEntry> Shaders;
	std::vector<HitGroupEntry> HitGroups;
	std::vector<Binding> Bindings;
	std::vector<std::pair<std::string, std::string>> ShaderDefines;
	std::unordered_map<std::string, CbvBuf> CbvBuffers;
	std::string ShaderLibraryTarget = "lib_6_3";
	bool UseHitResourceArrays = false;
	uint32_t NumInstances = 1;
	uint32_t MaxRecursion = 1;
	uint32_t MaxPayloadSize = 0;
	uint32_t MaxAttributeSize = 0;
	uint32_t HitBindSlot = 0;
	nri::PipelineLayout* Layout = nullptr;
	nri::Pipeline* Pipeline = nullptr;
	nri::DescriptorPool* Pool = nullptr;
	std::vector<nri::DescriptorSet*> Sets;
	nri::Buffer* SbtBuffer = nullptr;
	std::vector<nri::Memory*> SbtMemory;
	uint64_t SbtEntrySize = 0;
	uint64_t RaygenOffset = 0;
	uint64_t MissOffset = 0;
	uint64_t HitOffset = 0;
	uint32_t RaygenGroupCount = 0;
	uint32_t MissGroupCount = 0;
	uint32_t HitGroupStart = 0;
	uint32_t HitGroupCount = 0;
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
		for (auto& cb : CbRing) { if (cb.view) m->Core.DestroyDescriptor(cb.view); if (cb.buffer) m->FreeBuffer(cb.buffer, cb.memory); }
	}

	bool IsValid() const { return Pipeline != nullptr; }
	nri::Pipeline* GetPipeline() const { return Pipeline; }
	nri::PipelineLayout* GetLayout() const { return Layout; }

	enum class Kind { TexSRV, BufSRV, Sampler, CBV };
	struct Binding { std::string name; uint32_t reg; Kind kind; nri::Descriptor* desc = nullptr; nri::Descriptor* descSingle[1] = {}; bool ownsDesc = false; void* last = nullptr; Texture* tex = nullptr; Buffer* buf = nullptr; Sampler* samp = nullptr; uint32_t setIndex = 0; uint32_t rangeIndex = 0; };
	struct CbvState { nri::Buffer* buffer = nullptr; std::vector<nri::Memory*> memory; nri::Descriptor* view = nullptr; uint32_t size = 0; };

	void SetTexture(const std::string& name, Texture* t) { if (Binding* b = Find(name)) { b->tex = t; } }
	void SetBuffer(const std::string& name, Buffer* bb) { if (Binding* b = Find(name)) { b->buf = bb; } }
	void SetSampler(const std::string& name, Sampler* s) { if (Binding* b = Find(name)) { b->samp = s; } }
	void SetConstant(const void* data, uint32_t size)
	{
		// Stash for the next draw — the actual upload lands in a per-draw ring
		// slot in ApplyForDraw so each draw gets its own constants (no aliasing).
		if (!data || size == 0) return;
		const uint32_t n = size > Cbv.size ? Cbv.size : size;
		PendingCB.assign((const uint8_t*)data, (const uint8_t*)data + n);
		PendingCBSize = n;
	}

	// Bind pipeline + layout + pool. Descriptor sets are bound per-draw in
	// ApplyForDraw (textures/CB are set AFTER BindGraphicsPipeline by the caller,
	// and each draw needs its own set to avoid D3D12 execute-time aliasing).
	void Bind()
	{
		if (!Pipeline || !m->ActiveCmd) return;
		if (Pool) m->Core.CmdSetDescriptorPool(*m->ActiveCmd, *Pool);
		m->Core.CmdSetPipelineLayout(*m->ActiveCmd, nri::BindPoint::GRAPHICS, *Layout);
		m->Core.CmdSetPipeline(*m->ActiveCmd, *Pipeline);
	}

	// Called by the backend immediately before each Cmd*Draw. Grabs a fresh
	// per-draw descriptor set + constant-buffer slot from the per-frame ring,
	// uploads the pending constants, writes all current descriptors, and binds.
	void ApplyForDraw()
	{
		if (!Pipeline || !m->ActiveCmd || !HasResources || !Pool) return;

		// Reset the ring at the start of each frame (EndFrame waits on the GPU,
		// so previous-frame ring entries are safe to overwrite).
		if (RingFrame != m->SwapFrameIndex) { RingFrame = m->SwapFrameIndex; RingIdx = 0; }
		const uint32_t slot = RingIdx < kRing ? RingIdx : (kRing - 1);
		++RingIdx;

		// Lazily allocate this ring slot's descriptor set + CB buffer/view.
		if (SetRing.size() <= slot) { SetRing.resize(slot + 1, nullptr); CbRing.resize(slot + 1); }
		if (!SetRing[slot])
			m->Core.AllocateDescriptorSets(*Pool, *Layout, 0, &SetRing[slot], 1, 0);
		nri::DescriptorSet* set = SetRing[slot];
		if (!set) return;

		// Per-draw constant buffer slot.
		nri::Descriptor* cbView = nullptr;
		if (Cbv.size > 0)
		{
			CbvState& cb = CbRing[slot];
			if (!cb.buffer)
			{
				if (m->CreateBoundBuffer(Cbv.size, 0, nri::BufferUsageBits::CONSTANT_BUFFER, nri::MemoryLocation::HOST_UPLOAD, cb.buffer, cb.memory))
				{
					cb.size = Cbv.size;
					nri::BufferViewDesc bvd = {}; bvd.buffer = cb.buffer; bvd.type = nri::BufferView::CONSTANT_BUFFER; bvd.offset = 0; bvd.size = Cbv.size;
					m->Core.CreateBufferView(bvd, cb.view);
				}
			}
			if (cb.buffer && PendingCBSize > 0)
			{
				void* mapped = m->Core.MapBuffer(*cb.buffer, 0, PendingCBSize);
				if (mapped) { memcpy(mapped, PendingCB.data(), PendingCBSize); m->Core.UnmapBuffer(*cb.buffer); }
			}
			cbView = cb.view;
		}

		// Write every binding's current descriptor into this ring set.
		for (Binding& b : Bindings)
		{
			if (b.kind == Kind::CBV) { b.desc = cbView; b.ownsDesc = false; }
			else RefreshDescriptor(b);
			if (!b.desc) continue;
			b.descSingle[0] = b.desc;
			nri::UpdateDescriptorRangeDesc upd = {};
			upd.descriptorSet = set; upd.rangeIndex = b.rangeIndex; upd.baseDescriptor = 0;
			upd.descriptors = b.descSingle; upd.descriptorNum = 1;
			m->Core.UpdateDescriptorRanges(&upd, 1);
		}

		nri::SetDescriptorSetDesc sd = {}; sd.setIndex = 0; sd.descriptorSet = set; sd.bindPoint = nri::BindPoint::GRAPHICS;
		m->Core.CmdSetDescriptorSet(*m->ActiveCmd, sd);
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
		auto plog = [&](const char* what) {
			std::ofstream l("nri_pso.log", std::ios::app);
			if (l) { std::string sp(desc.ShaderPath.begin(), desc.ShaderPath.end());
				l << "GFXPSO FAIL [" << what << "] shader=" << sp << " vs=" << desc.VertexEntryPoint << " ps=" << desc.PixelEntryPoint << "\n"; }
		};
		if (!m->Device) { plog("no device"); return; }
		std::string verr, perr;
		const std::wstring vsW(desc.VertexEntryPoint.begin(), desc.VertexEntryPoint.end());
		const std::wstring psW(desc.PixelEntryPoint.begin(), desc.PixelEntryPoint.end());
		std::ifstream f(desc.ShaderPath, std::ios::binary);
		if (!f.good()) { plog("shader file not found"); return; }
		std::stringstream ss; ss << f.rdbuf(); const std::string src = ss.str();
		VsDxil = CompileHLSLToDXIL(src.data(), src.size(), desc.ShaderPath.c_str(), vsW.c_str(), L"vs_6_5", verr);
		PsDxil = CompileHLSLToDXIL(src.data(), src.size(), desc.ShaderPath.c_str(), psW.c_str(), L"ps_6_5", perr);
		if (VsDxil.empty() || PsDxil.empty()) {
			std::ofstream l("nri_pso.log", std::ios::app);
			if (l) { std::string sp(desc.ShaderPath.begin(), desc.ShaderPath.end());
				l << "GFXPSO FAIL [shader compile] shader=" << sp << " vsEmpty=" << VsDxil.empty() << " psEmpty=" << PsDxil.empty()
				  << " vsErr=" << verr << " psErr=" << perr << "\n"; }
			return;
		}
		VsEntry = desc.VertexEntryPoint; PsEntry = desc.PixelEntryPoint;

		// Bindings -> descriptor set ranges (resource set + sampler set).
		std::vector<nri::DescriptorRangeDesc> resR;
		const nri::StageBits gfxStages = nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;
		auto addRes = [&](const std::string& name, uint32_t reg, Kind k, nri::DescriptorType dt) {
			Binding b; b.name = name; b.reg = reg; b.kind = k; b.setIndex = 0; b.rangeIndex = (uint32_t)resR.size(); Bindings.push_back(b);
			nri::DescriptorRangeDesc r = {}; r.baseRegisterIndex = reg; r.descriptorNum = 1; r.descriptorType = dt; r.shaderStages = gfxStages; r.flags = nri::DescriptorRangeBits::PARTIALLY_BOUND; resR.push_back(r);
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
		for (const auto& s : desc.SamplerBindings)
		{
			Binding b; b.name = s.Name; b.reg = s.Slot; b.kind = Kind::Sampler; b.setIndex = 0; b.rangeIndex = (uint32_t)resR.size(); Bindings.push_back(b);
			nri::DescriptorRangeDesc r = {}; r.baseRegisterIndex = s.Slot; r.descriptorNum = 1; r.descriptorType = nri::DescriptorType::SAMPLER; r.shaderStages = gfxStages; r.flags = nri::DescriptorRangeBits::PARTIALLY_BOUND; resR.push_back(r);
		}

		// Single descriptor set (registerSpace 0) for all ranges (resources + samplers).
		std::vector<nri::DescriptorSetDesc> sets;
		if (!resR.empty()) { nri::DescriptorSetDesc d = {}; d.registerSpace = 0; d.ranges = resR.data(); d.rangeNum = (uint32_t)resR.size(); sets.push_back(d); }

		nri::PipelineLayoutDesc pld = {}; pld.rootRegisterSpace = 0; pld.descriptorSets = sets.empty() ? nullptr : sets.data(); pld.descriptorSetNum = (uint32_t)sets.size(); pld.shaderStages = gfxStages;
		if (m->Core.CreatePipelineLayout(*m->Device, pld, Layout) != nri::Result::SUCCESS) { plog("CreatePipelineLayout"); return; }

		if (!sets.empty())
		{
			// A single descriptor set reused across draws would alias in D3D12
			// (descriptors resolve at GPU-execute time, so every draw in the
			// command list would see the LAST set contents). Size the pool for a
			// RING of sets and hand out a fresh one per draw (ApplyForDraw).
			HasResources = true;
			nri::DescriptorPoolDesc pd = {}; pd.descriptorSetMaxNum = kRing;
			pd.textureMaxNum = (uint32_t)desc.TextureBindings.size() * kRing;
			pd.structuredBufferMaxNum = (uint32_t)desc.BufferBindings.size() * kRing;
			pd.constantBufferMaxNum = (desc.ConstantBufferSize > 0 ? 1u : 0u) * kRing;
			pd.samplerMaxNum = (uint32_t)desc.SamplerBindings.size() * kRing;
			if (m->Core.CreateDescriptorPool(*m->Device, pd, Pool) != nri::Result::SUCCESS) { plog("CreateDescriptorPool"); return; }
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
		nri::Result gr = m->Core.CreateGraphicsPipeline(*m->Device, gpd, Pipeline);
		if (gr != nri::Result::SUCCESS || !Pipeline) {
			std::ofstream l("nri_pso.log", std::ios::app);
			if (l) { std::string sp(desc.ShaderPath.begin(), desc.ShaderPath.end());
				l << "GFXPSO FAIL [CreateGraphicsPipeline] result=" << (int)gr << " shader=" << sp
				  << " colorNum=" << om.colorNum << " hasDepth=" << desc.DepthFormat.has_value()
				  << " attrs=" << attrs.size() << "\n"; }
		}
	}

	NRIBackend::Impl* m = nullptr;
	std::vector<Binding> Bindings;
	CbvState Cbv;                       // size template for the per-draw CB ring
	std::vector<uint8_t> VsDxil, PsDxil;
	std::string VsEntry, PsEntry;
	nri::PipelineLayout* Layout = nullptr;
	nri::Pipeline* Pipeline = nullptr;
	nri::DescriptorPool* Pool = nullptr;
	std::vector<nri::DescriptorSet*> Sets;
	bool HasResources = false;
	// Per-draw ring: one descriptor set + one CB slot per draw, reused each frame.
	static constexpr uint32_t kRing = 1024;
	std::vector<nri::DescriptorSet*> SetRing;
	std::vector<CbvState> CbRing;
	uint32_t RingIdx = 0;
	uint64_t RingFrame = ~0ull;
	std::vector<uint8_t> PendingCB;
	uint32_t PendingCBSize = 0;
};

// NRI routes validation / driver messages here. Surface them to the debugger
// output so problems during bring-up are visible.
static void NRI_CALL NRIMessageCallback(nri::Message messageType, const char* file, uint32_t line, const char* message, void* /*userArg*/)
{
	char buffer[2048];
	const char* sev = (messageType == nri::Message::ERROR) ? "ERROR" : (messageType == nri::Message::WARNING) ? "WARN" : "INFO";
	_snprintf_s(buffer, _TRUNCATE, "[NRI][%s] %s (%s:%u)\n", sev, message ? message : "", file ? file : "?", line);
	OutputDebugStringA(buffer);
	std::ofstream log("nri_messages.log", std::ios::app);
	if (log.is_open()) log << buffer;
}

// Provided so NRI does NOT DebugBreak/abort on validation errors during bring-up:
// a failing resource (e.g. an unsupported binding shape) is reported and the
// creation call returns an error, letting the editor skip that pass and proceed.
static void NRI_CALL NRIAbortCallback(void* /*userArg*/) { /* no-op: do not break */ }

NRIBackend::NRIBackend() : m(std::make_unique<Impl>())
{
	using nri::CoreInterface;
	using nri::HelperInterface;
	using nri::RayTracingInterface;
	using nri::SwapChainInterface;

	nri::DeviceCreationDesc desc = {};
	desc.graphicsAPI = nri::GraphicsAPI::D3D12;
	desc.enableNRIValidation = true;            // embedded NRI validation -> message log (fast)
	desc.enableGraphicsAPIValidation = false;   // D3D12 debug layer is very slow + breaks on error; enable only for deep debugging
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
	m->HasRayTracing =
		nri::nriGetInterface(*m->Device, NRI_INTERFACE(RayTracingInterface), &m->RT) == nri::Result::SUCCESS &&
		m->RT.CreateAccelerationStructureDescriptor &&
		m->RT.GetAccelerationStructureHandle &&
		m->RT.GetAccelerationStructureBuildScratchBufferSize &&
		m->RT.GetAccelerationStructureBuffer &&
		m->RT.DestroyAccelerationStructure &&
		m->RT.CreateCommittedAccelerationStructure &&
		m->RT.CreateAccelerationStructure &&
		m->RT.CmdBuildBottomLevelAccelerationStructures &&
		m->RT.CmdBuildTopLevelAccelerationStructures &&
		m->RT.CreateRayTracingPipeline &&
		m->RT.WriteShaderGroupIdentifiers &&
		m->RT.CmdDispatchRays;
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
	std::string rtASResult = (m->CmdBuffer && m->GraphicsQueue) ? m->RunRayTracingASSmoke() : std::string("skipped");
	std::string rtPipeResult = (m->CmdBuffer && m->GraphicsQueue) ? m->RunRayTracingPipelineSmoke() : std::string("skipped");

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

	// Texture UAV path: public ComputePSO writes RGBA16F to a Texture2D UAV,
	// then the backend captures the texture through the native D3D12 queue.
	std::string texPsoResult = "skipped";
	if (m->GraphicsQueue && m->CmdBuffer)
	{
		static const char* kTexCS =
			"RWTexture2D<float4> OutTex : register(u0);\n"
			"[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) { OutTex[id.xy] = float4(0.25f, 0.5f, 0.75f, 1.0f); }\n";
		const std::wstring path = L"nri_texture_pso_smoke.hlsl";
		{ std::ofstream of(path, std::ios::binary); of.write(kTexCS, (std::streamsize)strlen(kTexCS)); }

		TextureCreateDesc tcd = {};
		tcd.Width = 16;
		tcd.Height = 16;
		tcd.MipLevels = 1;
		tcd.Format = ETextureFormat::RGBA16Float;
		tcd.Usage = TextureUsage_UnorderedAccess;
		std::shared_ptr<Texture> outTex = CreateTexture2D(tcd);
		std::shared_ptr<ComputePipelineStateObject> pso = CreateComputePipelineStateObject();
		nri::Buffer* rb = nullptr;
		std::vector<nri::Memory*> rbMem;
		const uint32_t rowSize = 16u * 8u;
		const uint32_t rowAlign = std::max(1u, dd.memoryAlignment.uploadBufferTextureRow);
		const uint32_t rowPitch = static_cast<uint32_t>(AlignUpU64(rowSize, rowAlign));
		const uint32_t slicePitch = rowPitch * 16u;
		const bool rbOk = m->CreateBoundBuffer(slicePitch, 0, nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_READBACK, rb, rbMem);
		pso->BindUAV("OutTex", 0);
		if (outTex && rbOk && pso->InitCS(path, "main"))
		{
			pso->SetTextureUAV("OutTex", outTex.get());
			BeginFrame();
			TransitionTexture(outTex.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
			pso->Apply();
			Dispatch(2, 2, 1);
			TransitionTexture(outTex.get(), EResourceState::UnorderedAccess, EResourceState::CopySource);
			auto tit = m->Textures.find(outTex.get());
			if (m->ActiveCmd && tit != m->Textures.end() && tit->second.texture && rb)
			{
				nri::TextureRegionDesc region = {};
				region.width = 16;
				region.height = 16;
				region.depth = 1;
				region.planes = nri::PlaneBits::COLOR;
				nri::TextureDataLayoutDesc layout = {};
				layout.offset = 0;
				layout.rowPitch = rowPitch;
				layout.slicePitch = slicePitch;
				m->Core.CmdReadbackTextureToBuffer(*m->ActiveCmd, *rb, layout, *tit->second.texture, region);
			}
			EndFrame();

			const uint16_t* p = static_cast<const uint16_t*>(m->Core.MapBuffer(*rb, 0, slicePitch));
			if (p)
			{
				const bool ok = p[0] == 0x3400u && p[1] == 0x3800u && p[2] == 0x3A00u && p[3] == 0x3C00u;
				char b[96];
				_snprintf_s(b, _TRUNCATE, ok ? "PASS(%04X,%04X,%04X,%04X)" : "FAIL(%04X,%04X,%04X,%04X)",
					p[0], p[1], p[2], p[3]);
				texPsoResult = b;
				m->Core.UnmapBuffer(*rb);
			}
			else
			{
				texPsoResult = "FAIL(map)";
			}
		}
		else
		{
			texPsoResult = rbOk ? "FAIL(init)" : "FAIL(readback)";
		}

		m->FreeBuffer(rb, rbMem);
		if (outTex)
		{
			auto oit = m->Textures.find(outTex.get());
			if (oit != m->Textures.end()) { m->FreeTexture(oit->second.texture, oit->second.memory); m->Textures.erase(oit); }
		}
	}

	char info[1120];
	_snprintf_s(info, _TRUNCATE,
		"device ok [%s], rtTier=%u rtI=%s rtAS=%s rtPipe=%s sm=%u queue=%s bufferSmoke=%s texSmoke=%s cmd=%s submitSmoke=%s shaderDXIL=%zuB compute=%s psoInit=%s framePSO=%s cbv=%s texPSO=%s",
		dd.adapterDesc.name, (unsigned)dd.tiers.rayTracing, m->HasRayTracing ? "ok" : "null", rtASResult.c_str(), rtPipeResult.c_str(), (unsigned)dd.shaderModel,
		m->GraphicsQueue ? "ok" : "null", bufferSmokeOk ? "PASS" : "FAIL", texSmokeOk ? "PASS" : "FAIL",
		m->CmdBuffer ? "ok" : "null", submitSmokeOk ? "PASS" : "FAIL", shaderBytes, computeResult.c_str(), psoInitOk ? "PASS" : "FAIL", framePsoResult.c_str(), cbvResult.c_str(), texPsoResult.c_str());
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
		for (std::weak_ptr<NRIRTAS>& weakAS : m->RayTracingAS)
		{
			if (std::shared_ptr<NRIRTAS> as = weakAS.lock())
				as->Destroy();
		}
		m->RayTracingAS.clear();

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
		for (auto& kv : m->TexColorView) if (kv.second) m->Core.DestroyDescriptor(kv.second);
		for (auto& kv : m->TexDepthView) if (kv.second) m->Core.DestroyDescriptor(kv.second);
		m->TexColorView.clear(); m->TexDepthView.clear();
		for (auto& kv : m->VBs) m->FreeBuffer(kv.second.buffer, kv.second.memory);
		for (auto& kv : m->IBs) m->FreeBuffer(kv.second.buffer, kv.second.memory);
		m->VBs.clear(); m->IBs.clear();

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
uint32_t NRIBackend::GetMaxSupportedHybridStage() const { return (m->RayTracingTier >= 1 && m->HasRayTracing) ? 4u : 0u; }
bool NRIBackend::SupportsRayTracing() const { return GetMaxSupportedHybridStage() >= 1 && m->RayTracingTier >= 1 && m->HasRayTracing; }
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
	m->EndRP();
	m->RTColors.clear(); m->RTDepth = nullptr;
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
std::shared_ptr<Texture> NRIBackend::CreateTextureFromFile(const std::wstring& fileName, bool nonSRGB)
{
	if (!m->Device || !m->GraphicsQueue) return nullptr;
	std::error_code ec;
	if (!std::filesystem::exists(fileName, ec)) return nullptr;

	DirectX::ScratchImage loaded;
	const size_t dot = fileName.find_last_of(L'.');
	std::wstring ext = (dot != std::wstring::npos) ? fileName.substr(dot + 1) : L"";
	for (auto& c : ext) c = (wchar_t)towlower(c);
	HRESULT hr;
	if (ext == L"dds")      hr = DirectX::LoadFromDDSFile(fileName.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, loaded);
	else if (ext == L"tga") hr = DirectX::LoadFromTGAFile(fileName.c_str(), nullptr, loaded);
	else                    hr = DirectX::LoadFromWICFile(fileName.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, loaded);
	if (FAILED(hr)) { ErrorString = "CreateTextureFromFile: decode failed"; return nullptr; }

	const DirectX::TexMetadata& md = loaded.GetMetadata();
	if (ext == L"dds" && md.dimension == DirectX::TEX_DIMENSION_TEXTURE2D &&
		md.width > 0 && md.height > 0 && md.arraySize > 0 && md.mipLevels > 0)
	{
		const DXGI_FORMAT sampledFormat = ResolveSampledDXGIFormat(md.format, nonSRGB);
		const nri::Format nriFormat = ToNRISampledTextureFormat(sampledFormat);
		if (nriFormat != nri::Format::UNKNOWN)
		{
			nri::TextureDesc td = {};
			td.type = nri::TextureType::TEXTURE_2D;
			td.usage = nri::TextureUsageBits::SHADER_RESOURCE;
			td.format = nriFormat;
			td.width = static_cast<nri::Dim_t>(md.width);
			td.height = static_cast<nri::Dim_t>(md.height);
			td.depth = 1;
			td.mipNum = static_cast<nri::Dim_t>(md.mipLevels);
			td.layerNum = static_cast<nri::Dim_t>(md.arraySize);
			td.sampleNum = 1;

			nri::Texture* tex = nullptr;
			std::vector<nri::Memory*> mem;
			if (m->CreateBoundTexture(td, tex, mem))
			{
				std::vector<nri::TextureSubresourceUploadDesc> subs(md.arraySize * md.mipLevels);
				bool subresourcesOk = true;
				for (size_t arrayIdx = 0; arrayIdx < md.arraySize && subresourcesOk; ++arrayIdx)
				{
					for (size_t mipIdx = 0; mipIdx < md.mipLevels; ++mipIdx)
					{
						const DirectX::Image* img = loaded.GetImage(mipIdx, arrayIdx, 0);
						if (!img || !img->pixels)
						{
							subresourcesOk = false;
							break;
						}
						nri::TextureSubresourceUploadDesc& sub = subs[arrayIdx * md.mipLevels + mipIdx];
						sub.slices = img->pixels;
						sub.sliceNum = 1;
						sub.rowPitch = static_cast<uint32_t>(img->rowPitch);
						sub.slicePitch = static_cast<uint32_t>(img->slicePitch);
					}
				}

				nri::TextureUploadDesc up = {};
				up.subresources = subs.data();
				up.texture = tex;
				up.planes = nri::PlaneBits::ALL;
				up.after.access = nri::AccessBits::SHADER_RESOURCE;
				up.after.layout = nri::Layout::SHADER_RESOURCE;
				up.after.stages = nri::StageBits::ALL;
				if (subresourcesOk && m->Helper.UploadData(*m->GraphicsQueue, &up, 1, nullptr, 0) == nri::Result::SUCCESS)
				{
					auto wrapper = std::make_shared<Texture>();
					wrapper->Width = static_cast<uint32_t>(md.width);
					wrapper->Height = static_cast<uint32_t>(md.height);
					wrapper->MipLevels = static_cast<uint32_t>(md.mipLevels);
					wrapper->Format =
						(sampledFormat == DXGI_FORMAT_B8G8R8A8_UNORM || sampledFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
						? ETextureFormat::BGRA8Unorm : ETextureFormat::RGBA8Unorm;
					Impl::TextureAlloc alloc;
					alloc.texture = tex;
					alloc.memory = std::move(mem);
					m->Textures[wrapper.get()] = std::move(alloc);
					m->TexLayout[wrapper.get()] = nri::Layout::SHADER_RESOURCE;
					return wrapper;
				}

				m->FreeTexture(tex, mem);
			}
		}
	}

	// Fallback for non-DDS and NRI-unsupported DDS formats: flatten to RGBA8
	// mip-0. This keeps old behavior for uncommon inputs while common BC DDS
	// textures stay compressed in the fast path above.
	const DXGI_FORMAT target = DXGI_FORMAT_R8G8B8A8_UNORM;
	DirectX::ScratchImage converted;
	if (DirectX::IsCompressed(md.format))
		hr = DirectX::Decompress(loaded.GetImages(), loaded.GetImageCount(), md, target, converted);
	else if (md.format != target)
		hr = DirectX::Convert(loaded.GetImages(), loaded.GetImageCount(), md, target, DirectX::TEX_FILTER_DEFAULT, 0.0f, converted);
	if (FAILED(hr)) { ErrorString = "CreateTextureFromFile: convert failed"; return nullptr; }
	const DirectX::ScratchImage& src = (DirectX::IsCompressed(md.format) || md.format != target) ? converted : loaded;

	const DirectX::Image* img = src.GetImage(0, 0, 0);
	if (!img || !img->pixels) return nullptr;

	// Color/albedo textures are sRGB-encoded on disk and must be sampled with an
	// sRGB view so the GPU linearizes them; normal/roughness/data maps stay UNORM.
	const nri::Format texFormat = nonSRGB ? nri::Format::RGBA8_UNORM : nri::Format::RGBA8_SRGB;
	nri::TextureDesc td = {};
	td.type = nri::TextureType::TEXTURE_2D;
	td.usage = nri::TextureUsageBits::SHADER_RESOURCE;
	td.format = texFormat;
	td.width = static_cast<nri::Dim_t>(img->width);
	td.height = static_cast<nri::Dim_t>(img->height);
	td.depth = 1; td.mipNum = 1; td.layerNum = 1; td.sampleNum = 1;

	nri::Texture* tex = nullptr; std::vector<nri::Memory*> mem;
	if (!m->CreateBoundTexture(td, tex, mem)) { ErrorString = "CreateTextureFromFile: CreateBoundTexture failed"; return nullptr; }

	nri::TextureSubresourceUploadDesc sub = {};
	sub.slices = img->pixels; sub.sliceNum = 1;
	sub.rowPitch = static_cast<uint32_t>(img->rowPitch);
	sub.slicePitch = static_cast<uint32_t>(img->slicePitch);
	nri::TextureUploadDesc up = {};
	up.subresources = &sub; up.texture = tex; up.planes = nri::PlaneBits::ALL;
	up.after.access = nri::AccessBits::SHADER_RESOURCE; up.after.layout = nri::Layout::SHADER_RESOURCE; up.after.stages = nri::StageBits::ALL;
	m->Helper.UploadData(*m->GraphicsQueue, &up, 1, nullptr, 0);

	auto wrapper = std::make_shared<Texture>();
	wrapper->Width = static_cast<uint32_t>(img->width);
	wrapper->Height = static_cast<uint32_t>(img->height);
	wrapper->MipLevels = 1; wrapper->Format = ETextureFormat::RGBA8Unorm;
	Impl::TextureAlloc alloc; alloc.texture = tex; alloc.memory = std::move(mem);
	m->Textures[wrapper.get()] = std::move(alloc);
	m->TexLayout[wrapper.get()] = nri::Layout::SHADER_RESOURCE;
	return wrapper;
}
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
std::shared_ptr<VertexBuffer> NRIBackend::CreateVertexBuffer(uint32_t size, uint32_t stride, void* srcData)
{
	if (!m->RasterEnabled || !m->Device || size == 0) return nullptr;
	// HOST_UPLOAD + mapped memcpy — avoids a per-mesh Helper.UploadData queue submit
	// (which serialized loading to a crawl with thousands of meshes).
	nri::Buffer* nb = nullptr; std::vector<nri::Memory*> mem;
	if (!m->CreateBoundBuffer(size, 0,
		nri::BufferUsageBits::VERTEX_BUFFER | nri::BufferUsageBits::SHADER_RESOURCE | nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT,
		nri::MemoryLocation::HOST_UPLOAD, nb, mem)) return nullptr;
	void* mapped = m->Core.MapBuffer(*nb, 0, size);
	if (mapped && srcData) memcpy(mapped, srcData, size);
	auto w = std::make_shared<VertexBuffer>(); w->numVertices = stride ? (int)(size / stride) : 0;
	Impl::GpuBuf gb; gb.buffer = nb; gb.memory = std::move(mem); gb.stride = stride; gb.mapped = mapped; gb.capacity = size;
	m->VBs[w.get()] = std::move(gb);
	return w;
}
std::shared_ptr<IndexBuffer> NRIBackend::CreateIndexBuffer(EIndexFormat format, uint32_t size, void* srcData)
{
	if (!m->RasterEnabled || !m->Device || size == 0) return nullptr;
	nri::Buffer* nb = nullptr; std::vector<nri::Memory*> mem;
	if (!m->CreateBoundBuffer(size, 0,
		nri::BufferUsageBits::INDEX_BUFFER | nri::BufferUsageBits::SHADER_RESOURCE | nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT,
		nri::MemoryLocation::HOST_UPLOAD, nb, mem)) return nullptr;
	void* mapped = m->Core.MapBuffer(*nb, 0, size);
	if (mapped && srcData) memcpy(mapped, srcData, size);
	const uint32_t idxSize = (format == EIndexFormat::U16) ? 2u : 4u;
	auto w = std::make_shared<IndexBuffer>(); w->numIndices = (int)(size / idxSize);
	Impl::GpuBuf gb; gb.buffer = nb; gb.memory = std::move(mem); gb.indexType = (format == EIndexFormat::U16) ? nri::IndexType::UINT16 : nri::IndexType::UINT32; gb.mapped = mapped; gb.capacity = size;
	m->IBs[w.get()] = std::move(gb);
	return w;
}
std::shared_ptr<VertexBuffer> NRIBackend::CreateUploadVertexBuffer(uint32_t size, uint32_t stride, const void* srcData)
{
	if (!m->RasterEnabled || !m->Device || size == 0) return nullptr;
	nri::Buffer* nb = nullptr; std::vector<nri::Memory*> mem;
	if (!m->CreateBoundBuffer(size, 0,
		nri::BufferUsageBits::VERTEX_BUFFER | nri::BufferUsageBits::SHADER_RESOURCE | nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT,
		nri::MemoryLocation::HOST_UPLOAD, nb, mem)) return nullptr;
	void* mapped = m->Core.MapBuffer(*nb, 0, size);
	if (mapped && srcData) memcpy(mapped, srcData, size);
	auto w = std::make_shared<VertexBuffer>(); w->numVertices = stride ? (int)(size / stride) : 0; w->MappedCpu = mapped; w->MappedCapacityBytes = size;
	Impl::GpuBuf gb; gb.buffer = nb; gb.memory = std::move(mem); gb.stride = stride; gb.mapped = mapped; gb.capacity = size;
	m->VBs[w.get()] = std::move(gb);
	return w;
}
std::shared_ptr<IndexBuffer> NRIBackend::CreateUploadIndexBuffer(EIndexFormat format, uint32_t size, const void* srcData)
{
	if (!m->RasterEnabled || !m->Device || size == 0) return nullptr;
	nri::Buffer* nb = nullptr; std::vector<nri::Memory*> mem;
	if (!m->CreateBoundBuffer(size, 0,
		nri::BufferUsageBits::INDEX_BUFFER | nri::BufferUsageBits::SHADER_RESOURCE | nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT,
		nri::MemoryLocation::HOST_UPLOAD, nb, mem)) return nullptr;
	void* mapped = m->Core.MapBuffer(*nb, 0, size);
	if (mapped && srcData) memcpy(mapped, srcData, size);
	const uint32_t idxSize = (format == EIndexFormat::U16) ? 2u : 4u;
	auto w = std::make_shared<IndexBuffer>(); w->numIndices = (int)(size / idxSize); w->MappedCpu = mapped; w->MappedCapacityBytes = size;
	Impl::GpuBuf gb; gb.buffer = nb; gb.memory = std::move(mem); gb.indexType = (format == EIndexFormat::U16) ? nri::IndexType::UINT16 : nri::IndexType::UINT32; gb.mapped = mapped; gb.capacity = size;
	m->IBs[w.get()] = std::move(gb);
	return w;
}
void NRIBackend::UpdateUploadVertexBuffer(VertexBuffer* buffer, const void* srcData, uint32_t sizeInBytes)
{
	auto it = m->VBs.find(buffer);
	if (it != m->VBs.end() && it->second.mapped && srcData) memcpy(it->second.mapped, srcData, std::min(sizeInBytes, it->second.capacity));
}
void NRIBackend::UpdateUploadIndexBuffer(IndexBuffer* buffer, const void* srcData, uint32_t sizeInBytes)
{
	auto it = m->IBs.find(buffer);
	if (it != m->IBs.end() && it->second.mapped && srcData) memcpy(it->second.mapped, srcData, std::min(sizeInBytes, it->second.capacity));
}
std::shared_ptr<VertexBuffer> NRIBackend::CreateRWVertexBuffer(uint32_t size, uint32_t stride)
{
	if (!m->RasterEnabled || !m->Device || size == 0) return nullptr;
	nri::Buffer* nb = nullptr; std::vector<nri::Memory*> mem;
	if (!m->CreateBoundBuffer(size, 0,
		nri::BufferUsageBits::VERTEX_BUFFER | nri::BufferUsageBits::SHADER_RESOURCE | nri::BufferUsageBits::SHADER_RESOURCE_STORAGE | nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT,
		nri::MemoryLocation::DEVICE, nb, mem)) return nullptr;
	auto w = std::make_shared<VertexBuffer>(); w->numVertices = stride ? (int)(size / stride) : 0;
	Impl::GpuBuf gb; gb.buffer = nb; gb.memory = std::move(mem); gb.stride = stride; gb.capacity = size;
	m->VBs[w.get()] = std::move(gb);
	return w;
}
std::shared_ptr<Buffer> NRIBackend::CreateUploadStructuredBuffer(uint32_t numElements, uint32_t elementSize)
{
	if (!m->Device || numElements == 0 || elementSize == 0) return nullptr;
	const uint64_t size = static_cast<uint64_t>(numElements) * elementSize;
	nri::Buffer* nb = nullptr; std::vector<nri::Memory*> mem;
	if (!m->CreateBoundBuffer(size, elementSize, nri::BufferUsageBits::SHADER_RESOURCE, nri::MemoryLocation::HOST_UPLOAD, nb, mem))
	{
		ErrorString = "CreateUploadStructuredBuffer: CreateBoundBuffer failed";
		return nullptr;
	}
	void* mapped = m->Core.MapBuffer(*nb, 0, size); // persistent map (HOST_UPLOAD)
	auto w = std::make_shared<Buffer>();
	w->Type = Buffer::STRUCTURED; w->NumElements = numElements; w->ElementSize = elementSize;
	Impl::BufferAlloc alloc; alloc.buffer = nb; alloc.memory = std::move(mem); alloc.mapped = mapped;
	m->Buffers[w.get()] = std::move(alloc);
	return w;
}
void NRIBackend::UpdateUploadStructuredBuffer(Buffer* buffer, const void* srcData, uint32_t sizeInBytes)
{
	auto it = m->Buffers.find(buffer);
	if (it != m->Buffers.end() && it->second.mapped && srcData)
		memcpy(it->second.mapped, srcData, sizeInBytes);
}

// === Ray tracing ==========================================================
std::shared_ptr<RTAS> NRIBackend::CreateBLASForMesh(Mesh* mesh)
{
	return CreateNRIBLASForMeshInternal(m.get(), mesh, false, false);
}

std::shared_ptr<RTAS> NRIBackend::CreateBLASForSkeletalMesh(Mesh* mesh)
{
	return CreateNRIBLASForMeshInternal(m.get(), mesh, true, true);
}

void NRIBackend::RefitBLAS(RTAS* rtas, Mesh* mesh)
{
	NRIRTAS* as = dynamic_cast<NRIRTAS*>(rtas);
	if (!as || !as->Owner || !as->AllowUpdate || !as->AccelerationStructure || !as->ScratchBuffer || !mesh)
		return;

	uint32_t vertexCount = 0;
	uint32_t indexCount = 0;
	if (!FillNRIBottomGeometry(m.get(), mesh, true, as->BottomGeometry, vertexCount, indexCount))
		return;

	nri::BuildBottomLevelAccelerationStructureDesc build = {};
	build.dst = as->AccelerationStructure;
	build.src = as->AccelerationStructure;
	build.geometries = &as->BottomGeometry;
	build.geometryNum = 1;
	build.scratchBuffer = as->ScratchBuffer;

	m->SubmitImmediate("[NRIRTAS] RefitBLAS immediate submit failed\n", [&](nri::CommandBuffer& cmd)
	{
		m->RT.CmdBuildBottomLevelAccelerationStructures(cmd, &build, 1);
		m->BarrierAccelerationStructure(cmd, as->AccelerationStructure);
	});
}

std::shared_ptr<RTAS> NRIBackend::CreateTLAS(const std::vector<RTInstanceDesc>& instances)
{
	if (!m->Device || !m->HasRayTracing || instances.empty() || instances.size() > static_cast<size_t>(UINT32_MAX))
		return nullptr;

	auto as = std::make_shared<NRIRTAS>();
	as->Owner = m.get();
	as->IsTopLevel = true;
	as->AllowUpdate = true;
	as->InstanceCount = static_cast<uint32_t>(instances.size());

	nri::AccelerationStructureDesc desc = {};
	desc.geometryOrInstanceNum = as->InstanceCount;
	desc.flags = nri::AccelerationStructureBits::ALLOW_UPDATE | nri::AccelerationStructureBits::PREFER_FAST_TRACE;
	desc.type = nri::AccelerationStructureType::TOP_LEVEL;

	if (m->RT.CreateCommittedAccelerationStructure(*m->Device, nri::MemoryLocation::DEVICE, 0.0f, desc, as->AccelerationStructure) != nri::Result::SUCCESS ||
		!as->AccelerationStructure)
	{
		AppendCpuRuntimeTrace(L"[NRIRTAS] CreateCommittedAccelerationStructure TLAS failed");
		return nullptr;
	}

	uint64_t scratchSize = m->RT.GetAccelerationStructureBuildScratchBufferSize(*as->AccelerationStructure);
	if (m->RT.GetAccelerationStructureUpdateScratchBufferSize)
		scratchSize = std::max<uint64_t>(scratchSize, m->RT.GetAccelerationStructureUpdateScratchBufferSize(*as->AccelerationStructure));
	if (scratchSize == 0 ||
		!m->CreateBoundBuffer(scratchSize, 0, nri::BufferUsageBits::SCRATCH_BUFFER, nri::MemoryLocation::DEVICE, as->ScratchBuffer, as->ScratchMemory))
	{
		AppendCpuRuntimeTrace(L"[NRIRTAS] TLAS scratch allocation failed size=" + std::to_wstring(scratchSize));
		return nullptr;
	}

	const uint64_t instanceBytes = sizeof(nri::TopLevelInstance) * static_cast<uint64_t>(instances.size());
	if (!m->CreateBoundBuffer(instanceBytes, 0, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT,
		nri::MemoryLocation::HOST_UPLOAD, as->InstanceBuffer, as->InstanceMemory))
	{
		AppendCpuRuntimeTrace(L"[NRIRTAS] TLAS instance allocation failed bytes=" + std::to_wstring(instanceBytes));
		return nullptr;
	}
	as->InstanceMapped = m->Core.MapBuffer(*as->InstanceBuffer, 0, instanceBytes);
	if (!as->InstanceMapped || !WriteNRITLASInstances(as.get(), instances))
	{
		AppendCpuRuntimeTrace(L"[NRIRTAS] TLAS instance write failed instances=" + std::to_wstring(instances.size()));
		return nullptr;
	}

	nri::BuildTopLevelAccelerationStructureDesc build = {};
	build.dst = as->AccelerationStructure;
	build.instanceNum = as->InstanceCount;
	build.instanceBuffer = as->InstanceBuffer;
	build.scratchBuffer = as->ScratchBuffer;

	std::vector<std::shared_ptr<NRIRTAS>> pendingBLAS;
	std::vector<nri::BuildBottomLevelAccelerationStructureDesc> pendingBLASBuilds;
	CollectPendingNRIBLASBuilds(instances, pendingBLAS, pendingBLASBuilds);

	const bool submitted = m->SubmitImmediate("[NRIRTAS] BuildTLAS immediate submit failed\n", [&](nri::CommandBuffer& cmd)
	{
		if (!pendingBLASBuilds.empty())
		{
			for (size_t i = 0; i < pendingBLASBuilds.size(); ++i)
			{
				m->RT.CmdBuildBottomLevelAccelerationStructures(cmd, &pendingBLASBuilds[i], 1);
				m->BarrierAccelerationStructure(cmd, pendingBLAS[i]->AccelerationStructure);
			}
		}
		m->RT.CmdBuildTopLevelAccelerationStructures(cmd, &build, 1);
		m->BarrierAccelerationStructure(cmd, as->AccelerationStructure);
	});
	if (!submitted)
	{
		AppendCpuRuntimeTrace(L"[NRIRTAS] BuildTLAS submit failed");
		return nullptr;
	}
	for (const std::shared_ptr<NRIRTAS>& blas : pendingBLAS)
		blas->BuildPending = false;
	if (!pendingBLAS.empty())
		AppendCpuRuntimeTrace(L"[NRIRTAS] BuildBLAS batch submitted count=" + std::to_wstring(pendingBLAS.size()));

	if (m->RT.CreateAccelerationStructureDescriptor(*as->AccelerationStructure, as->Descriptor) != nri::Result::SUCCESS)
		AppendCpuRuntimeTrace(L"[NRIRTAS] TLAS descriptor creation failed");

	m->RayTracingAS.push_back(as);
	AppendCpuRuntimeTrace(L"[NRIRTAS] BuildTLAS submitted instances=" + std::to_wstring(instances.size()));
	return as;
}

bool NRIBackend::UpdateTLAS(const std::shared_ptr<RTAS>& topLevelAS, const std::vector<RTInstanceDesc>& instances)
{
	std::shared_ptr<NRIRTAS> as = std::dynamic_pointer_cast<NRIRTAS>(topLevelAS);
	if (!as || !as->Owner || !as->IsTopLevel || !as->AllowUpdate ||
		!as->AccelerationStructure || !as->ScratchBuffer || !as->InstanceBuffer ||
		instances.empty() || instances.size() != as->InstanceCount)
	{
		return false;
	}
	if (!WriteNRITLASInstances(as.get(), instances))
		return false;

	nri::BuildTopLevelAccelerationStructureDesc build = {};
	build.dst = as->AccelerationStructure;
	build.src = as->AccelerationStructure;
	build.instanceNum = as->InstanceCount;
	build.instanceBuffer = as->InstanceBuffer;
	build.scratchBuffer = as->ScratchBuffer;

	std::vector<std::shared_ptr<NRIRTAS>> pendingBLAS;
	std::vector<nri::BuildBottomLevelAccelerationStructureDesc> pendingBLASBuilds;
	CollectPendingNRIBLASBuilds(instances, pendingBLAS, pendingBLASBuilds);

	const bool submitted = m->SubmitImmediate("[NRIRTAS] UpdateTLAS immediate submit failed\n", [&](nri::CommandBuffer& cmd)
	{
		if (!pendingBLASBuilds.empty())
		{
			for (size_t i = 0; i < pendingBLASBuilds.size(); ++i)
			{
				m->RT.CmdBuildBottomLevelAccelerationStructures(cmd, &pendingBLASBuilds[i], 1);
				m->BarrierAccelerationStructure(cmd, pendingBLAS[i]->AccelerationStructure);
			}
		}
		m->RT.CmdBuildTopLevelAccelerationStructures(cmd, &build, 1);
		m->BarrierAccelerationStructure(cmd, as->AccelerationStructure);
	});
	if (submitted)
	{
		for (const std::shared_ptr<NRIRTAS>& blas : pendingBLAS)
			blas->BuildPending = false;
		if (!pendingBLAS.empty())
			AppendCpuRuntimeTrace(L"[NRIRTAS] BuildBLAS batch submitted count=" + std::to_wstring(pendingBLAS.size()));
	}
	return submitted;
}

// === Pipelines / shaders ==================================================
std::shared_ptr<RTPipelineStateObject> NRIBackend::CreateRTPipelineStateObject()
{
	if (!m->Device || !m->HasRayTracing)
		return nullptr;
	return std::make_shared<NRIRTPipelineStateObject>(m.get());
}
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
bool NRIBackend::CaptureTexture(Texture* source, CapturedImage& captured, EResourceState beforeState)
{
	captured = {};
	ErrorString.clear();
	if (!m || !source || !m->GraphicsQueue || !m->Device || !m->Fence)
	{
		ErrorString = "NRI CaptureTexture invalid source or queue";
		return false;
	}

	nri::Texture* texture = m->NriTex(source);
	uint32_t backBufferIndex = 0;
	if (!texture && m->IsBackbuffer(source, backBufferIndex) && backBufferIndex < m->BackBuffers.size())
		texture = m->BackBuffers[backBufferIndex];
	if (!texture)
	{
		ErrorString = "NRI CaptureTexture texture not found";
		return false;
	}

	const uint32_t bytesPerPixel = CaptureBytesPerPixel(source->Format);
	if (source->Width == 0 || source->Height == 0 || bytesPerPixel == 0)
	{
		ErrorString = "NRI CaptureTexture unsupported source format";
		return false;
	}

	const uint64_t rowSize64 = static_cast<uint64_t>(source->Width) * bytesPerPixel;
	if (rowSize64 > UINT32_MAX)
	{
		ErrorString = "NRI CaptureTexture row too large";
		return false;
	}
	const nri::DeviceDesc& dd = m->Core.GetDeviceDesc(*m->Device);
	const uint32_t rowAlign = std::max(1u, dd.memoryAlignment.uploadBufferTextureRow);
	const uint32_t sliceAlign = std::max(1u, dd.memoryAlignment.uploadBufferTextureSlice);
	const uint32_t rowPitch = static_cast<uint32_t>(AlignUpU64(rowSize64, rowAlign));
	const uint64_t slicePitch64 = AlignUpU64(static_cast<uint64_t>(rowPitch) * source->Height, sliceAlign);

	nri::Buffer* readback = nullptr;
	std::vector<nri::Memory*> readbackMemory;
	if (slicePitch64 == 0 || slicePitch64 > SIZE_MAX ||
		!m->CreateBoundBuffer(slicePitch64, 0, nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_READBACK, readback, readbackMemory))
	{
		ErrorString = "NRI CaptureTexture readback allocation failed";
		return false;
	}

	auto cleanupReadback = [&]()
	{
		m->FreeBuffer(readback, readbackMemory);
	};

	bool restartedActiveCmd = false;
	bool submitted = false;
	if (m->ActiveCmd)
	{
		m->EndRP();
		TransitionTexture(source, beforeState, EResourceState::CopySource);

		nri::TextureRegionDesc region = {};
		region.width = static_cast<nri::Dim_t>(source->Width);
		region.height = static_cast<nri::Dim_t>(source->Height);
		region.depth = 1;
		region.planes = CapturePlaneBits(source->Format);
		nri::TextureDataLayoutDesc layout = {};
		layout.offset = 0;
		layout.rowPitch = rowPitch;
		layout.slicePitch = static_cast<uint32_t>(slicePitch64);
		m->Core.CmdReadbackTextureToBuffer(*m->ActiveCmd, *readback, layout, *texture, region);

		TransitionTexture(source, EResourceState::CopySource, beforeState);

		if (m->Core.EndCommandBuffer(*m->ActiveCmd) == nri::Result::SUCCESS)
		{
			nri::FenceSubmitDesc signalFence = {};
			signalFence.fence = m->Fence;
			signalFence.value = ++m->FenceValue;
			signalFence.stages = nri::StageBits::ALL;
			nri::CommandBuffer* commandBuffers[1] = { m->ActiveCmd };
			nri::QueueSubmitDesc submit = {};
			submit.commandBuffers = commandBuffers;
			submit.commandBufferNum = 1;
			submit.signalFences = &signalFence;
			submit.signalFenceNum = 1;
			if (m->Core.QueueSubmit(*m->GraphicsQueue, submit) == nri::Result::SUCCESS)
			{
				m->Core.Wait(*m->Fence, m->FenceValue);
				submitted = m->Core.GetFenceValue(*m->Fence) >= m->FenceValue;
			}
		}

		m->ActiveCmd = nullptr;
		if (m->CmdAllocator && m->CmdBuffer)
		{
			m->Core.ResetCommandAllocator(*m->CmdAllocator);
			if (m->Core.BeginCommandBuffer(*m->CmdBuffer, nullptr) == nri::Result::SUCCESS)
			{
				m->ActiveCmd = m->CmdBuffer;
				restartedActiveCmd = true;
			}
		}
	}
	else
	{
		submitted = m->SubmitImmediate("[NRI] CaptureTexture immediate submit failed\n", [&](nri::CommandBuffer& cmd)
		{
			const nri::Layout beforeLayout = m->TexLayout.count(source) ? m->TexLayout[source] : nri::Layout::UNDEFINED;
			nri::TextureBarrierDesc toCopy = {};
			toCopy.texture = texture;
			toCopy.before.access = AccessForLayout(beforeLayout);
			toCopy.before.layout = beforeLayout;
			toCopy.before.stages = StagesForLayout(beforeLayout);
			toCopy.after.access = nri::AccessBits::COPY_SOURCE;
			toCopy.after.layout = nri::Layout::COPY_SOURCE;
			toCopy.after.stages = nri::StageBits::COPY;
			toCopy.mipNum = 1;
			toCopy.layerNum = 1;
			nri::BarrierDesc toCopyBarrier = {};
			toCopyBarrier.textures = &toCopy;
			toCopyBarrier.textureNum = 1;
			m->Core.CmdBarrier(cmd, toCopyBarrier);

			nri::TextureRegionDesc region = {};
			region.width = static_cast<nri::Dim_t>(source->Width);
			region.height = static_cast<nri::Dim_t>(source->Height);
			region.depth = 1;
			region.planes = CapturePlaneBits(source->Format);
			nri::TextureDataLayoutDesc layout = {};
			layout.offset = 0;
			layout.rowPitch = rowPitch;
			layout.slicePitch = static_cast<uint32_t>(slicePitch64);
			m->Core.CmdReadbackTextureToBuffer(cmd, *readback, layout, *texture, region);

			nri::TextureBarrierDesc toRead = {};
			toRead.texture = texture;
			toRead.before.access = nri::AccessBits::COPY_SOURCE;
			toRead.before.layout = nri::Layout::COPY_SOURCE;
			toRead.before.stages = nri::StageBits::COPY;
			toRead.after.access = AccessForLayout(beforeLayout);
			toRead.after.layout = beforeLayout;
			toRead.after.stages = StagesForLayout(beforeLayout);
			toRead.mipNum = 1;
			toRead.layerNum = 1;
			nri::BarrierDesc toReadBarrier = {};
			toReadBarrier.textures = &toRead;
			toReadBarrier.textureNum = 1;
			m->Core.CmdBarrier(cmd, toReadBarrier);
		});
	}

	if (!submitted)
	{
		ErrorString = restartedActiveCmd ?
			"NRI CaptureTexture command submit failed" :
			"NRI CaptureTexture command submit/restart failed";
		cleanupReadback();
		return false;
	}

	const uint8_t* mapped = static_cast<const uint8_t*>(m->Core.MapBuffer(*readback, 0, slicePitch64));
	if (!mapped)
	{
		ErrorString = "NRI CaptureTexture readback map failed";
		cleanupReadback();
		return false;
	}

	captured.Format = source->Format;
	captured.Width = source->Width;
	captured.Height = source->Height;
	captured.RowPitch = static_cast<uint32_t>(rowSize64);
	captured.Pixels.resize(static_cast<size_t>(rowSize64) * source->Height);
	for (uint32_t y = 0; y < source->Height; ++y)
	{
		std::memcpy(
			captured.Pixels.data() + static_cast<size_t>(y) * captured.RowPitch,
			mapped + static_cast<size_t>(y) * rowPitch,
			static_cast<size_t>(rowSize64));
	}
	m->Core.UnmapBuffer(*readback);
	cleanupReadback();
	return true;
}
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
	id.descriptorPoolSize = 16384; // upper bound of textures across in-flight frames; editor uses ImGui::Image for the viewport, so size generously
	if (m->ImguiI.CreateImgui(*m->Device, id, m->Imgui) != nri::Result::SUCCESS)
		ErrorString = "ImGui: CreateImgui failed";
}
void NRIBackend::NewImGuiFrame() { /* ImGui::NewFrame() is driven by Corona; nothing NRI-specific here */ }
void NRIBackend::RenderImGuiDrawData(ImDrawData* drawData)
{
	if (!m->ActiveCmd || !m->Imgui || !m->Streamer || !m->FrameHasBackbuffer || !drawData)
		return;
	++m->DbgImguiDraws;
	const uint32_t idx = m->CurrentBackBuffer;

	// Close any scene render pass first; the copy must be OUTSIDE a render pass.
	m->EndRP();

	nri::CopyImguiDataDesc copy = {};
	copy.drawLists = drawData->CmdLists.Data;
	copy.drawListNum = (uint32_t)drawData->CmdLists.Size;
	copy.textures = drawData->Textures ? drawData->Textures->Data : nullptr;
	copy.textureNum = drawData->Textures ? (uint32_t)drawData->Textures->Size : 0;
	m->ImguiI.CmdCopyImguiData(*m->ActiveCmd, *m->Streamer, *m->Imgui, copy);
	m->StreamerI.CmdCopyStreamedData(*m->ActiveCmd, *m->Streamer);

	// Self-contained backbuffer render pass for ImGui (decoupled from the scene
	// OpenRP machinery — this is the path verified to work).
	m->TransitionBackbuffer(nri::AccessBits::COLOR_ATTACHMENT, nri::Layout::COLOR_ATTACHMENT, nri::StageBits::ALL);
	nri::AttachmentDesc colorAtt = {};
	colorAtt.descriptor = m->BackBufferViews[idx];
	colorAtt.loadOp = m->RTHasClear ? nri::LoadOp::CLEAR : nri::LoadOp::LOAD;
	colorAtt.storeOp = nri::StoreOp::STORE;
	if (m->RTHasClear) colorAtt.clearValue.color.f = nri::Color32f{ m->RTClear[0], m->RTClear[1], m->RTClear[2], m->RTClear[3] };
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
	m->RTHasClear = false;
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
void NRIBackend::SetRenderTarget(Texture* colorTarget, Texture* depthTarget)
{
	m->EndRP();
	m->RTColors.clear();
	if (colorTarget) m->RTColors.push_back(colorTarget);
	m->RTDepth = depthTarget;
	m->RTHasClear = false; m->RTHasDepthClear = false;
	m->CurrentWindowRT = colorTarget;
}
void NRIBackend::SetRenderTargets(Texture* const* colorTargets, uint32_t count, Texture* depthTarget)
{
	m->EndRP();
	m->RTColors.assign(colorTargets, colorTargets + count);
	m->RTDepth = depthTarget;
	m->RTHasClear = false; m->RTHasDepthClear = false;
	m->CurrentWindowRT = (count > 0 && colorTargets) ? colorTargets[0] : nullptr;
}
void NRIBackend::ClearRenderTarget(Texture* target, const float clearColor[4])
{
	if (!target) return;
	Impl::ClearColor c; c.v[0] = clearColor[0]; c.v[1] = clearColor[1]; c.v[2] = clearColor[2]; c.v[3] = clearColor[3];
	m->PendingColorClears[target] = c;
	++m->DbgClears;
}
void NRIBackend::ClearDepth(Texture* target, float depthValue) { if (target) m->PendingDepthClears[target] = depthValue; }
void NRIBackend::BindDefaultDescriptorHeaps() { NRI_TODO(); }
void NRIBackend::SetViewportAndScissor(uint32_t width, uint32_t height)
{
	m->VpW = width; m->VpH = height;
	if (m->RPOpen && m->ActiveCmd)
	{
		nri::Viewport vp = {}; vp.width = (float)width; vp.height = (float)height; vp.depthMin = 0; vp.depthMax = 1;
		nri::Rect sc = {}; sc.width = (nri::Dim_t)width; sc.height = (nri::Dim_t)height;
		m->Core.CmdSetViewports(*m->ActiveCmd, &vp, 1);
		m->Core.CmdSetScissors(*m->ActiveCmd, &sc, 1);
	}
}
void NRIBackend::DrawFullscreenQuad(VertexBuffer* vertexBuffer)
{
	if (!m->ActiveCmd || !m->RasterEnabled || !m->HasBoundGfx) { ++m->DbgDrawsSkipped; return; }
	m->OpenRP();
	if (m->CurrentGfx) static_cast<NRIGraphicsPipeline*>(m->CurrentGfx)->ApplyForDraw();
	auto it = m->VBs.find(vertexBuffer);
	if (it != m->VBs.end() && it->second.buffer)
	{
		nri::VertexBufferDesc vbd = {}; vbd.buffer = it->second.buffer; vbd.offset = 0; vbd.stride = it->second.stride;
		m->Core.CmdSetVertexBuffers(*m->ActiveCmd, 0, &vbd, 1);
		nri::DrawDesc dd = {}; dd.vertexNum = (uint32_t)(vertexBuffer ? vertexBuffer->numVertices : 3); dd.instanceNum = 1;
		m->Core.CmdDraw(*m->ActiveCmd, dd);
		++m->DbgDraws;
	}
	else { ++m->DbgDrawsSkipped; }
}
void NRIBackend::BindMeshBuffers(VertexBuffer* vertexBuffer, IndexBuffer* indexBuffer)
{
	if (!m->ActiveCmd || !m->RasterEnabled) return;
	m->OpenRP();
	auto vit = m->VBs.find(vertexBuffer);
	if (vit != m->VBs.end() && vit->second.buffer)
	{
		nri::VertexBufferDesc vbd = {}; vbd.buffer = vit->second.buffer; vbd.offset = 0; vbd.stride = vit->second.stride;
		m->Core.CmdSetVertexBuffers(*m->ActiveCmd, 0, &vbd, 1);
	}
	auto iit = m->IBs.find(indexBuffer);
	if (iit != m->IBs.end() && iit->second.buffer)
		m->Core.CmdSetIndexBuffer(*m->ActiveCmd, *iit->second.buffer, 0, iit->second.indexType);
}
void NRIBackend::DrawIndexed(uint32_t indexCount, uint32_t startIndexLocation, int32_t baseVertexLocation)
{
	if (!m->ActiveCmd || !m->RasterEnabled || !m->HasBoundGfx) { ++m->DbgDrawsSkipped; return; }
	m->OpenRP();
	if (m->CurrentGfx) static_cast<NRIGraphicsPipeline*>(m->CurrentGfx)->ApplyForDraw();
	nri::DrawIndexedDesc dd = {}; dd.indexNum = indexCount; dd.instanceNum = 1; dd.baseIndex = startIndexLocation; dd.baseVertex = baseVertexLocation;
	m->Core.CmdDrawIndexed(*m->ActiveCmd, dd);
	++m->DbgDraws;
}
void NRIBackend::DrawIndexedInstanced(uint32_t indexCountPerInstance, uint32_t instanceCount, uint32_t startIndexLocation, int32_t baseVertexLocation, uint32_t startInstanceLocation)
{
	if (!m->ActiveCmd || !m->RasterEnabled || !m->HasBoundGfx) { ++m->DbgDrawsSkipped; return; }
	m->OpenRP();
	if (m->CurrentGfx) static_cast<NRIGraphicsPipeline*>(m->CurrentGfx)->ApplyForDraw();
	nri::DrawIndexedDesc dd = {}; dd.indexNum = indexCountPerInstance; dd.instanceNum = instanceCount; dd.baseIndex = startIndexLocation; dd.baseVertex = baseVertexLocation; dd.baseInstance = startInstanceLocation;
	m->Core.CmdDrawIndexed(*m->ActiveCmd, dd);
	++m->DbgDraws;
}
void NRIBackend::Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
	if (m->ActiveCmd)
	{
		m->EndRP();
		nri::DispatchDesc d = { groupCountX, groupCountY, groupCountZ };
		m->Core.CmdDispatch(*m->ActiveCmd, d);
	}
}
void NRIBackend::ClearTextureUAVFloat(Texture*, const float[4]) { NRI_TODO(); }
void NRIBackend::ExecuteCurrentCommandList() { NRI_TODO(); }
void NRIBackend::BeginGpuMarker(uint64_t, const char*) { NRI_TODO(); }
void NRIBackend::EndGpuMarker() { NRI_TODO(); }
void NRIBackend::TransitionTexture(Texture* texture, EResourceState, EResourceState stateAfter)
{
	if (!m->ActiveCmd || !texture) return;
	m->EndRP();
	nri::AccessBits acc = nri::AccessBits::SHADER_RESOURCE;
	nri::Layout lay = nri::Layout::SHADER_RESOURCE;
	nri::StageBits st = nri::StageBits::ALL;
	switch (stateAfter)
	{
	case EResourceState::ShaderRead:      acc = nri::AccessBits::SHADER_RESOURCE;             lay = nri::Layout::SHADER_RESOURCE; break;
	case EResourceState::RenderTarget:    acc = nri::AccessBits::COLOR_ATTACHMENT;            lay = nri::Layout::COLOR_ATTACHMENT; break;
	case EResourceState::UnorderedAccess: acc = nri::AccessBits::SHADER_RESOURCE_STORAGE;     lay = nri::Layout::SHADER_RESOURCE_STORAGE; break;
	case EResourceState::DepthWrite:      acc = nri::AccessBits::DEPTH_STENCIL_ATTACHMENT_WRITE; lay = nri::Layout::DEPTH_STENCIL_ATTACHMENT; break;
	case EResourceState::CopyDest:        acc = nri::AccessBits::COPY_DESTINATION;            lay = nri::Layout::COPY_DESTINATION; st = nri::StageBits::COPY; break;
	case EResourceState::CopySource:      acc = nri::AccessBits::COPY_SOURCE;                 lay = nri::Layout::COPY_SOURCE; st = nri::StageBits::COPY; break;
	case EResourceState::Present:         acc = nri::AccessBits::NONE;                        lay = nri::Layout::PRESENT; st = nri::StageBits::NONE; break;
	default:                              acc = nri::AccessBits::SHADER_RESOURCE;             lay = nri::Layout::SHADER_RESOURCE; break;
	}
	m->TransitionTex(texture, acc, lay, st);
}
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
void NRIBackend::BindGraphicsPipeline(GraphicsPipelineHandle* pipeline)
{
	m->CurrentGfx = pipeline;
	m->HasBoundGfx = false;
	if (!m->ActiveCmd || !pipeline || !m->RasterEnabled) return;
	auto* p = static_cast<NRIGraphicsPipeline*>(pipeline);
	if (!p->GetPipeline()) { ++m->DbgGfxBindFail; return; }  // PSO failed to create — don't issue draws with no pipeline
	m->OpenRP();
	p->Bind();
	m->HasBoundGfx = true;
	++m->DbgGfxBindOk;
}
void NRIBackend::SetGraphicsPipelineConstantData(GraphicsPipelineHandle* pipeline, uint32_t, const void* data, uint32_t size) { if (auto* p = static_cast<NRIGraphicsPipeline*>(pipeline)) p->SetConstant(data, size); }
void NRIBackend::BindGraphicsPipelineTexture(GraphicsPipelineHandle* pipeline, const std::string& bindingName, Texture* texture) { if (auto* p = static_cast<NRIGraphicsPipeline*>(pipeline)) p->SetTexture(bindingName, texture); }
void NRIBackend::BindGraphicsPipelineBuffer(GraphicsPipelineHandle* pipeline, const std::string& bindingName, Buffer* buffer) { if (auto* p = static_cast<NRIGraphicsPipeline*>(pipeline)) p->SetBuffer(bindingName, buffer); }
void NRIBackend::BindGraphicsPipelineVertexBufferSRV(GraphicsPipelineHandle*, const std::string&, VertexBuffer*) { /* skinning motion-vector SRV: later */ }
void NRIBackend::BindGraphicsPipelineSampler(GraphicsPipelineHandle* pipeline, const std::string& bindingName, Sampler* sampler) { if (auto* p = static_cast<NRIGraphicsPipeline*>(pipeline)) p->SetSampler(bindingName, sampler); }

#endif // CORONA_HAS_NRI
