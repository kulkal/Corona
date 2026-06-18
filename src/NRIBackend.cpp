#include "stdafx.h"
#include "NRIBackend.h"

// The entire implementation is gated on CORONA_HAS_NRI. When the CMake option
// CORONA_WITH_NRI is OFF (the default), this translation unit is empty and the
// stock D3D12 / Vulkan build is byte-for-byte unaffected.
#if CORONA_HAS_NRI

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <memory>
#include <string>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <set>
#include <utility>
#include <atomic>
#include <chrono>

#include <wrl/client.h>
#include <dxcapi.use.h>
#include <d3d12.h> // Streamline (DLSS-RR) interop needs native ID3D12Resource/state enums
#include <filesystem>
#include "DirectXTex.h"

#include "imgui.h"
#include "CoronaImageIO.h"
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

static std::string NarrowAsciiForLog(const std::wstring& text)
{
	std::string out;
	out.reserve(text.size());
	for (wchar_t ch : text)
		out.push_back((ch >= 0 && ch <= 0x7f) ? static_cast<char>(ch) : '?');
	return out;
}

namespace
{
	using NriCpuProfileClock = std::chrono::steady_clock;

	bool IsNriRecordProfileEnabled()
	{
		static const bool enabled = []
		{
			const char* value = std::getenv("CORONA_NRI_RECORD_PROFILE");
			return value && value[0] != '\0' && value[0] != '0';
		}();
		return enabled;
	}

	double NriCpuProfileElapsedMs(NriCpuProfileClock::time_point begin)
	{
		return std::chrono::duration<double, std::milli>(NriCpuProfileClock::now() - begin).count();
	}

	void NriCpuProfileAdd(bool enabled, double& totalMs, uint64_t& count, NriCpuProfileClock::time_point begin)
	{
		if (!enabled)
			return;
		totalMs += NriCpuProfileElapsedMs(begin);
		++count;
	}

	struct NriCpuProfileScope
	{
		bool Enabled = false;
		NriCpuProfileClock::time_point Begin{};
		double& TotalMs;
		uint64_t& Count;

		NriCpuProfileScope(bool enabled, double& totalMs, uint64_t& count)
			: Enabled(enabled)
			, Begin(enabled ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{})
			, TotalMs(totalMs)
			, Count(count)
		{
		}

		~NriCpuProfileScope()
		{
			if (Enabled)
			{
				TotalMs += NriCpuProfileElapsedMs(Begin);
				++Count;
			}
		}
	};
}

// ---------------------------------------------------------------------------
// Shader compilation: HLSL -> DXIL via dxcompiler.dll. NRI consumes bytecode;
// when NRI runs on D3D12 it wants DXIL (when on Vulkan it would want SPIR-V,
// handled later). Self-contained DXC instance so we don't couple to the DX12
// backend's compiler.
// ---------------------------------------------------------------------------
static dxc::DxcDllSupport gNriDxc;

namespace NRIFileCache
{
	constexpr uint32_t kDxilCacheVersion = 2;
	constexpr uint32_t kPipelineCacheVersion = 1;
	constexpr uint64_t kFnvOffset = 1469598103934665603ull;
	constexpr uint64_t kFnvPrime = 1099511628211ull;

	uint64_t HashBytes(const void* data, size_t size, uint64_t seed)
	{
		const uint8_t* bytes = static_cast<const uint8_t*>(data);
		uint64_t hash = seed;
		for (size_t i = 0; i < size; ++i)
		{
			hash ^= bytes[i];
			hash *= kFnvPrime;
		}
		return hash;
	}

	bool Enabled()
	{
		static const bool enabled = []
		{
			const wchar_t* disable = _wgetenv(L"CORONA_DISABLE_SHADER_CACHE");
			return !(disable && disable[0] != L'\0' && disable[0] != L'0');
		}();
		return enabled;
	}

	std::filesystem::path CacheDir()
	{
		return RuntimePaths::RootDirectory() / L"bin" / L"shadercache";
	}

	std::filesystem::path CachePath(uint64_t key, const wchar_t* extension)
	{
		std::wstringstream name;
		name << std::hex << std::setw(16) << std::setfill(L'0') << key << extension;
		return CacheDir() / name.str();
	}

	bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& out)
	{
		std::error_code ec;
		const auto size = std::filesystem::file_size(path, ec);
		if (ec || size == 0)
			return false;
		std::ifstream file(path, std::ios::binary);
		if (!file.good())
			return false;
		out.resize(static_cast<size_t>(size));
		file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
		return static_cast<size_t>(file.gcount()) == out.size();
	}

	void WriteFile(const std::filesystem::path& path, const void* data, size_t size)
	{
		if (!Enabled() || !data || size == 0)
			return;
		std::error_code ec;
		std::filesystem::create_directories(CacheDir(), ec);
		std::filesystem::path tmp = path;
		tmp += L".tmp";
		{
			std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
			if (!file.good())
				return;
			file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
			if (!file.good())
				return;
		}
		std::filesystem::rename(tmp, path, ec);
		if (ec)
			std::filesystem::remove(tmp, ec);
	}

	uint64_t CompilerStamp()
	{
		static const uint64_t stamp = []
		{
			std::error_code ec;
			const std::filesystem::path compilerPath = RuntimePaths::RootDirectory() / L"bin" / L"dxcompiler.dll";
			const auto size = std::filesystem::file_size(compilerPath, ec);
			if (ec)
				return 0ull;
			const auto writeTime = std::filesystem::last_write_time(compilerPath, ec);
			const uint64_t timeStamp = ec ? 0ull : static_cast<uint64_t>(writeTime.time_since_epoch().count());
			return static_cast<uint64_t>(size) ^ (timeStamp * 0x9E3779B185EBCA87ull);
		}();
		return stamp;
	}

	uint64_t HashSourceTree(const std::filesystem::path& file, std::set<std::filesystem::path>& visited, uint64_t seed)
	{
		std::error_code ec;
		const std::filesystem::path canonical = std::filesystem::weakly_canonical(file, ec);
		const std::filesystem::path keyPath = ec ? file.lexically_normal() : canonical;
		if (!visited.insert(keyPath).second)
			return seed;

		std::ifstream stream(file, std::ios::binary);
		if (!stream.good())
			return seed;

		std::stringstream buffer;
		buffer << stream.rdbuf();
		const std::string content = buffer.str();
		uint64_t hash = HashBytes(content.data(), content.size(), seed);

		size_t pos = 0;
		while ((pos = content.find("#include", pos)) != std::string::npos)
		{
			pos += 8;
			const size_t newline = content.find('\n', pos);
			const size_t open = content.find('"', pos);
			if (open == std::string::npos)
				break;
			if (newline != std::string::npos && open > newline)
			{
				pos = newline + 1;
				continue;
			}
			const size_t close = content.find('"', open + 1);
			if (close == std::string::npos)
				break;
			const std::string relative = content.substr(open + 1, close - (open + 1));
			pos = close + 1;
			if (!relative.empty())
				hash = HashSourceTree(file.parent_path() / std::filesystem::path(relative), visited, hash);
		}
		return hash;
	}

	uint64_t DxilKey(const void* source, size_t sourceSize, const wchar_t* sourceName, const wchar_t* entryPoint, const wchar_t* target)
	{
		uint64_t key = kFnvOffset;
		key = HashBytes(&kDxilCacheVersion, sizeof(kDxilCacheVersion), key);
		const uint64_t compilerStamp = CompilerStamp();
		key = HashBytes(&compilerStamp, sizeof(compilerStamp), key);
		if (source && sourceSize > 0)
			key = HashBytes(source, sourceSize, key);
		if (sourceName && sourceName[0] != L'\0')
		{
			const size_t sourceNameBytes = wcslen(sourceName) * sizeof(wchar_t);
			key = HashBytes(sourceName, sourceNameBytes, key);
			std::error_code ec;
			const std::filesystem::path sourcePath(sourceName);
			if (std::filesystem::exists(sourcePath, ec))
			{
				std::set<std::filesystem::path> visited;
				key = HashSourceTree(sourcePath, visited, key);
			}
		}
		const uint8_t hasEntryPoint = (entryPoint && entryPoint[0] != L'\0') ? 1u : 0u;
		key = HashBytes(&hasEntryPoint, sizeof(hasEntryPoint), key);
		if (hasEntryPoint)
			key = HashBytes(entryPoint, wcslen(entryPoint) * sizeof(wchar_t), key);
		if (target)
			key = HashBytes(target, wcslen(target) * sizeof(wchar_t), key);
#if defined(_DEBUG)
		const char config = 'D';
#else
		const char config = 'R';
#endif
		key = HashBytes(&config, sizeof(config), key);
		return key;
	}

	size_t BoundedCStringLength(const char* value, size_t maxLength);

	uint64_t PipelineCacheKey(const nri::DeviceDesc& desc)
	{
		uint64_t key = kFnvOffset;
		key = HashBytes(&kPipelineCacheVersion, sizeof(kPipelineCacheVersion), key);
		key = HashBytes(desc.adapterDesc.name, BoundedCStringLength(desc.adapterDesc.name, sizeof(desc.adapterDesc.name)), key);
		key = HashBytes(&desc.adapterDesc.uid, sizeof(desc.adapterDesc.uid), key);
		key = HashBytes(&desc.adapterDesc.deviceId, sizeof(desc.adapterDesc.deviceId), key);
		key = HashBytes(&desc.adapterDesc.driverVersion, sizeof(desc.adapterDesc.driverVersion), key);
		key = HashBytes(&desc.graphicsAPI, sizeof(desc.graphicsAPI), key);
		key = HashBytes(&desc.shaderModel, sizeof(desc.shaderModel), key);
		return key;
	}

	std::vector<uint8_t> LoadDxil(uint64_t key)
	{
		std::vector<uint8_t> bytes;
		if (Enabled())
			ReadFile(CachePath(key, L".nri.dxil"), bytes);
		return bytes;
	}

	void StoreDxil(uint64_t key, const std::vector<uint8_t>& bytes)
	{
		if (!bytes.empty())
			WriteFile(CachePath(key, L".nri.dxil"), bytes.data(), bytes.size());
	}

	size_t BoundedCStringLength(const char* value, size_t maxLength)
	{
		size_t length = 0;
		while (length < maxLength && value[length] != '\0')
			++length;
		return length;
	}
}

static std::vector<uint8_t> CompileHLSLToDXIL(
	const void* source, size_t sourceSize, const wchar_t* sourceName,
	const wchar_t* entryPoint, const wchar_t* target, std::string& outError)
{
	using Microsoft::WRL::ComPtr;
	outError.clear();
	const uint64_t cacheKey = NRIFileCache::DxilKey(source, sourceSize, sourceName, entryPoint, target);
	std::vector<uint8_t> cachedDxil = NRIFileCache::LoadDxil(cacheKey);
	if (!cachedDxil.empty())
		return cachedDxil;

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
#if defined(_DEBUG)
	args.push_back(DXC_ARG_DEBUG);
	args.push_back(DXC_ARG_SKIP_OPTIMIZATIONS);
#else
	args.push_back(DXC_ARG_OPTIMIZATION_LEVEL3);
#endif
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
	std::vector<uint8_t> dxil(p, p + blob->GetBufferSize());
	NRIFileCache::StoreDxil(cacheKey, dxil);
	return dxil;
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
	case EResourceState::IndirectArgument:a.access = nri::AccessBits::ARGUMENT_BUFFER;         a.stages = nri::StageBits::INDIRECT; break;
	case EResourceState::RenderTarget:    a.access = nri::AccessBits::COLOR_ATTACHMENT;        a.stages = nri::StageBits::ALL;  break;
	case EResourceState::DepthWrite:      a.access = nri::AccessBits::DEPTH_STENCIL_ATTACHMENT_WRITE; a.stages = nri::StageBits::ALL; break;
	case EResourceState::Present:         a.access = nri::AccessBits::NONE;                    a.stages = nri::StageBits::ALL;  break;
	default:                              a.access = nri::AccessBits::NONE;                    a.stages = nri::StageBits::ALL;  break;
	}
	return a;
}

static nri::StageBits ToNRIGraphicsStageBits(RHIShaderStageMask stages)
{
	nri::StageBits out = nri::StageBits::NONE;
	if (stages & ToRHIShaderStageMask(RHIShaderStage::Vertex))
		out = out | nri::StageBits::VERTEX_SHADER;
	if (stages & ToRHIShaderStageMask(RHIShaderStage::Pixel))
		out = out | nri::StageBits::FRAGMENT_SHADER;
	if (out == nri::StageBits::NONE)
		out = nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;
	return out;
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
	nri::PipelineCache* PipelineCache = nullptr;
	std::filesystem::path PipelineCachePath;
	nri::QueryPool* TimestampQueryPool = nullptr;
	nri::Buffer* TimestampReadbackBuffer = nullptr;
	std::vector<nri::Memory*> TimestampReadbackMemory;
	uint8_t* TimestampReadbackMapped = nullptr;
	uint32_t TimestampQueryCount = 0;
	uint32_t TimestampQueryStride = 0;
	nri::QueryPool* OcclusionQueryPool = nullptr;
	nri::Buffer* OcclusionReadbackBuffer = nullptr;
	std::vector<nri::Memory*> OcclusionReadbackMemory;
	uint8_t* OcclusionReadbackMapped = nullptr;
	uint32_t OcclusionQueryCount = 0;
	uint32_t OcclusionQueryStride = 0;

	void InitializePipelineCache()
	{
		if (!Device || !Core.CreatePipelineCache || !Core.GetPipelineCacheData || !Core.DestroyPipelineCache || !NRIFileCache::Enabled())
			return;

		const nri::DeviceDesc& deviceDesc = Core.GetDeviceDesc(*Device);
		if (!deviceDesc.features.pipelineCache)
		{
			AppendCpuRuntimeTrace(L"[NRICache] pipeline cache unsupported by device");
			return;
		}

		PipelineCachePath = NRIFileCache::CachePath(NRIFileCache::PipelineCacheKey(deviceDesc), L".nri.pso");
		std::vector<uint8_t> initialData;
		NRIFileCache::ReadFile(PipelineCachePath, initialData);

		nri::PipelineCacheDesc cacheDesc = {};
		cacheDesc.data = initialData.empty() ? nullptr : initialData.data();
		cacheDesc.size = initialData.size();
		nri::Result result = Core.CreatePipelineCache(*Device, cacheDesc, PipelineCache);
		if (result != nri::Result::SUCCESS && !initialData.empty())
		{
			AppendCpuRuntimeTrace(L"[NRICache] stale pipeline cache ignored result=" + std::to_wstring(static_cast<int>(result)));
			initialData.clear();
			cacheDesc.data = nullptr;
			cacheDesc.size = 0;
			result = Core.CreatePipelineCache(*Device, cacheDesc, PipelineCache);
		}
		if (result == nri::Result::SUCCESS && PipelineCache)
		{
			AppendCpuRuntimeTrace(L"[NRICache] pipeline cache ready initialBytes=" + std::to_wstring(initialData.size()));
		}
		else
		{
			PipelineCache = nullptr;
			AppendCpuRuntimeTrace(L"[NRICache] CreatePipelineCache failed result=" + std::to_wstring(static_cast<int>(result)));
		}
	}

	void SaveAndDestroyPipelineCache()
	{
		if (!PipelineCache || !Core.GetPipelineCacheData || !Core.DestroyPipelineCache)
			return;
		if (NRIFileCache::Enabled())
		{
			uint64_t size = 0;
			nri::Result result = Core.GetPipelineCacheData(*PipelineCache, nullptr, size);
			if (result == nri::Result::SUCCESS && size > 0)
			{
				std::vector<uint8_t> bytes(static_cast<size_t>(size));
				result = Core.GetPipelineCacheData(*PipelineCache, bytes.data(), size);
				if (result == nri::Result::SUCCESS && size > 0)
				{
					NRIFileCache::WriteFile(PipelineCachePath, bytes.data(), static_cast<size_t>(size));
					AppendCpuRuntimeTrace(L"[NRICache] pipeline cache saved bytes=" + std::to_wstring(size));
				}
			}
		}
		Core.DestroyPipelineCache(PipelineCache);
		PipelineCache = nullptr;
	}

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
	bool DeviceLost = false;
	nri::Layout BBLayout = nri::Layout::UNDEFINED; // current backbuffer layout this frame
	std::wstring PendingWindowCapturePath;
	std::wstring LastWindowCapturePath;
	std::wstring LastWindowCaptureError;
	bool LastWindowCaptureResultValid = false;
	bool LastWindowCaptureSucceeded = false;
	static constexpr uint32_t kQueuedFrameNum = 2;
	struct FrameContext
	{
		nri::CommandAllocator* allocator = nullptr;
		nri::CommandBuffer* commandBuffer = nullptr;
		uint64_t fenceValue = 0;
		std::vector<std::shared_ptr<Buffer>> transientBuffers;
	};
	std::vector<FrameContext> FrameContexts;
	uint32_t ActiveFrameContextIndex = 0;
	struct ImmediateContext
	{
		nri::CommandAllocator* allocator = nullptr;
		nri::CommandBuffer* commandBuffer = nullptr;
		uint64_t fenceValue = 0;
	};
	std::vector<ImmediateContext> ImmediateContexts;
	uint64_t ImmediateSubmitIndex = 0;

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
	struct GpuBuf
	{
		nri::Buffer* buffer = nullptr;
		std::vector<nri::Memory*> memory;
		uint32_t stride = 0;
		nri::IndexType indexType = nri::IndexType::UINT32;
		void* mapped = nullptr;
		uint32_t capacity = 0;
		nri::AccessStage access = {};
		bool accessValid = false;
	};
	std::unordered_map<VertexBuffer*, GpuBuf> VBs;
	std::unordered_map<IndexBuffer*, GpuBuf> IBs;
	// Last-bound mesh buffers (for per-draw OOB diagnosis; see DrawIndexed).
	VertexBuffer* LastVB = nullptr; IndexBuffer* LastIB = nullptr;
	uint32_t LastVBVerts = 0, LastVBStride = 0, LastIBIndices = 0, LastIBCapacity = 0;
	nri::IndexType LastIBType = nri::IndexType::UINT32;
	bool DrawLog = false;          // CORONA_NRI_DRAWLOG: flush a line per GBuffer draw
	uint32_t DbgOOBDraws = 0;      // draws skipped because index range exceeds IB capacity
	uint32_t DbgBadXform = 0;      // draws skipped because the per-draw transform was non-finite/huge
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
	std::shared_ptr<ComputePipelineStateObject> ClearTextureUavFloat1Pso;
	std::shared_ptr<ComputePipelineStateObject> ClearTextureUavFloat2Pso;
	std::shared_ptr<ComputePipelineStateObject> ClearTextureUavFloat4Pso;

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
		const bool profile = IsNriRecordProfileEnabled();
		NriCpuProfileScope scope(profile, RecordProfile.TransitionTextureMs, RecordProfile.TransitionTextureCount);
		nri::Layout before = TexLayout.count(t) ? TexLayout[t] : nri::Layout::UNDEFINED;
		if (before == layout)
			return;
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
		const bool profile = IsNriRecordProfileEnabled();
		NriCpuProfileScope scope(profile, RecordProfile.OpenRenderPassMs, RecordProfile.OpenRenderPassCount);
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
		const bool profile = IsNriRecordProfileEnabled();
		NriCpuProfileScope scope(profile, RecordProfile.TransitionTextureMs, RecordProfile.TransitionTextureCount);
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
	// Spec that makes two buffer allocations interchangeable for reuse pooling.
	struct BufKey
	{
		uint64_t size = 0;
		uint32_t stride = 0;
		uint32_t usage = 0;     // nri::BufferUsageBits
		uint32_t location = 0;  // nri::MemoryLocation
		bool operator==(const BufKey& o) const
		{
			return size == o.size && stride == o.stride && usage == o.usage && location == o.location;
		}
	};
	struct BufKeyHash
	{
		size_t operator()(const BufKey& k) const
		{
			size_t h = std::hash<uint64_t>()(k.size);
			h ^= (size_t)k.stride * 0x9E3779B1u + (h << 6) + (h >> 2);
			h ^= (size_t)k.usage * 0x85EBCA6Bu + (h << 6) + (h >> 2);
			h ^= (size_t)k.location * 0xC2B2AE35u + (h << 6) + (h >> 2);
			return h;
		}
	};
	struct BufferAlloc
	{
		nri::Buffer* buffer = nullptr;
		std::vector<nri::Memory*> memory;
		void* mapped = nullptr; // non-null for persistently-mapped HOST_UPLOAD buffers
		BufKey key{};           // spec for the reuse pool
		nri::AccessStage access = {};
		bool accessValid = false;
	};
	std::shared_ptr<std::atomic_bool> Alive = std::make_shared<std::atomic_bool>(true);
	std::unordered_map<Buffer*, BufferAlloc> Buffers;

	// Buffer reuse pool. NRI buffers were never freed when their wrapper died (no
	// NRI free callback on ~Buffer — Owner is DX12-only), so per-frame buffers
	// (InstancePropertyBuffer, transients) LEAKED a committed GPU heap every frame
	// -> VRAM growth -> CreateHeap DEVICE_REMOVED (the freeze) -> and the per-frame
	// committed allocation is also the dominant CPU recording cost. The pool both
	// fixes the leak and recycles allocations by spec so AllocateAndBindMemory is
	// amortized. Reuse across frames is safe because EndFrame waits on the GPU.
	std::unordered_map<BufKey, std::vector<BufferAlloc>, BufKeyHash> BufferReusePool;
	static constexpr size_t kMaxPooledPerKey = 8;
	bool TryReuseBuffer(const BufKey& key, BufferAlloc& out)
	{
		auto it = BufferReusePool.find(key);
		if (it == BufferReusePool.end() || it->second.empty())
			return false;
		out = std::move(it->second.back());
		it->second.pop_back();
		return true;
	}
	void RecycleBuffer(BufferAlloc&& a)
	{
		if (a.key.size == 0 || !a.buffer)
		{
			FreeBuffer(a.buffer, a.memory);
			return;
		}
		auto& v = BufferReusePool[a.key];
		if (v.size() < kMaxPooledPerKey)
			v.push_back(std::move(a));
		else
			FreeBuffer(a.buffer, a.memory); // pool full -> actually free (mapped freed with memory)
	}
	void FlushBufferReusePool()
	{
		for (auto& kv : BufferReusePool)
			for (auto& a : kv.second)
				FreeBuffer(a.buffer, a.memory);
		BufferReusePool.clear();
	}

	struct CpuRecordProfile
	{
		uint32_t Frames = 0;

		double BeginFrameMs = 0.0;
		double WaitFrameContextMs = 0.0;
		double ResetBeginCommandBufferMs = 0.0;
		double AcquireNextTextureMs = 0.0;
		double EndFrameMs = 0.0;
		double EndCommandBufferMs = 0.0;
		double QueueSubmitMs = 0.0;
		double QueuePresentMs = 0.0;
		double FrameFenceWaitMs = 0.0;
		double ImmediateFenceWaitMs = 0.0;
		double HelperUploadDataMs = 0.0;
		double CreateBoundBufferMs = 0.0;
		double AllocateAndBindBufferMemoryMs = 0.0;
		double CreateBoundTextureMs = 0.0;
		double AllocateAndBindTextureMemoryMs = 0.0;
		double TransientUploadAllocMs = 0.0;
		double TransientUploadCreateMs = 0.0;
		double TransientUploadMapMs = 0.0;
		double TransientUploadMemcpyMs = 0.0;
		double TransientDefaultAllocMs = 0.0;
		double TransientDefaultCopyRecordMs = 0.0;
		double TransientDefaultMemcpyMs = 0.0;
		double FreeTransientBuffersMs = 0.0;
		double BindGraphicsPipelineMs = 0.0;
		double CreateGraphicsBindGroupMs = 0.0;
		double BindGraphicsBindGroupMs = 0.0;
		double ApplyForDrawMs = 0.0;
		double RefreshDescriptorMs = 0.0;
		double UpdateDescriptorRangesMs = 0.0;
		double SetDescriptorSetMs = 0.0;
		double OpenRenderPassMs = 0.0;
		double TransitionTextureMs = 0.0;
		double DrawMs = 0.0;
		double DrawIndexedMs = 0.0;
		double DrawIndirectMs = 0.0;
		double DrawIndexedIndirectMs = 0.0;
		double DispatchMs = 0.0;
		double ComputeApplyMs = 0.0;
		double ComputeCbvUpdateMs = 0.0;
		double RtApplyMs = 0.0;
		double RtCbvUpdateMs = 0.0;
		double RtUpdateDescriptorRangesMs = 0.0;
		double RtSetDescriptorSetMs = 0.0;
		double RtDispatchRaysMs = 0.0;

		uint64_t BeginFrameCount = 0;
		uint64_t WaitFrameContextCount = 0;
		uint64_t ResetBeginCommandBufferCount = 0;
		uint64_t AcquireNextTextureCount = 0;
		uint64_t EndFrameCount = 0;
		uint64_t EndCommandBufferCount = 0;
		uint64_t QueueSubmitCount = 0;
		uint64_t QueuePresentCount = 0;
		uint64_t FrameFenceWaitCount = 0;
		uint64_t ImmediateFenceWaitCount = 0;
		uint64_t HelperUploadDataCount = 0;
		uint64_t CreateBoundBufferCount = 0;
		uint64_t AllocateAndBindBufferMemoryCount = 0;
		uint64_t CreateBoundTextureCount = 0;
		uint64_t AllocateAndBindTextureMemoryCount = 0;
		uint64_t TransientUploadAllocCount = 0;
		uint64_t TransientUploadCreateCount = 0;
		uint64_t TransientUploadMapCount = 0;
		uint64_t TransientUploadMemcpyCount = 0;
		uint64_t FreeTransientBuffersCount = 0;
		uint64_t TransientUploadReuseHit = 0;
		uint64_t TransientUploadReuseMiss = 0;
		uint64_t TransientUploadBytes = 0;
		uint64_t TransientDefaultAllocCount = 0;
		uint64_t TransientDefaultCopyRecordCount = 0;
		uint64_t TransientDefaultMemcpyCount = 0;
		uint64_t TransientDefaultReuseHit = 0;
		uint64_t TransientDefaultReuseMiss = 0;
		uint64_t TransientDefaultBytes = 0;
		uint64_t BindGraphicsPipelineCount = 0;
		uint64_t CreateGraphicsBindGroupCount = 0;
		uint64_t BindGraphicsBindGroupCount = 0;
		uint64_t ApplyForDrawCount = 0;
		uint64_t RefreshDescriptorCount = 0;
		uint64_t UpdateDescriptorRangesCount = 0;
		uint64_t SetDescriptorSetCount = 0;
		uint64_t OpenRenderPassCount = 0;
		uint64_t TransitionTextureCount = 0;
		uint64_t DrawCount = 0;
		uint64_t DrawIndexedCount = 0;
		uint64_t DrawIndirectCallCount = 0;
		uint64_t DrawIndirectDrawCount = 0;
		uint64_t DrawIndexedIndirectCallCount = 0;
		uint64_t DrawIndexedIndirectDrawCount = 0;
		uint64_t DispatchCount = 0;
		uint64_t ComputeApplyCount = 0;
		uint64_t ComputeCbvUpdateCount = 0;
		uint64_t RtApplyCount = 0;
		uint64_t RtCbvUpdateCount = 0;
		uint64_t RtUpdateDescriptorRangesCount = 0;
		uint64_t RtSetDescriptorSetCount = 0;
		uint64_t RtDispatchRaysCount = 0;

		void Reset()
		{
			*this = CpuRecordProfile{};
		}
	};
	CpuRecordProfile RecordProfile;

	void FlushRecordProfileIfReady()
	{
		if (!IsNriRecordProfileEnabled() || RecordProfile.Frames < 120)
			return;

		const CpuRecordProfile& p = RecordProfile;
		const double frames = static_cast<double>(std::max(1u, p.Frames));
		auto avgMs = [frames](double totalMs) { return totalMs / frames; };
		auto perFrame = [frames](uint64_t count) { return static_cast<double>(count) / frames; };
		auto bytesPerFrameKB = [frames](uint64_t bytes) { return (static_cast<double>(bytes) / frames) / 1024.0; };
		auto metric = [&](std::wstringstream& ss, const wchar_t* name, double ms, uint64_t count)
		{
			ss << L", " << name << L"=" << std::fixed << std::setprecision(3) << avgMs(ms)
				<< L"ms/" << std::setprecision(1) << perFrame(count) << L"c";
		};

		std::wstringstream sync;
		sync << L"[NRIRecordProfile][sync] frames=" << p.Frames;
		metric(sync, L"beginFrame", p.BeginFrameMs, p.BeginFrameCount);
		metric(sync, L"waitFrameCtx", p.WaitFrameContextMs, p.WaitFrameContextCount);
		metric(sync, L"resetBeginCmd", p.ResetBeginCommandBufferMs, p.ResetBeginCommandBufferCount);
		metric(sync, L"acquire", p.AcquireNextTextureMs, p.AcquireNextTextureCount);
		metric(sync, L"endFrame", p.EndFrameMs, p.EndFrameCount);
		metric(sync, L"endCmd", p.EndCommandBufferMs, p.EndCommandBufferCount);
		metric(sync, L"submit", p.QueueSubmitMs, p.QueueSubmitCount);
		metric(sync, L"present", p.QueuePresentMs, p.QueuePresentCount);
		metric(sync, L"frameWait", p.FrameFenceWaitMs, p.FrameFenceWaitCount);
		metric(sync, L"immWait", p.ImmediateFenceWaitMs, p.ImmediateFenceWaitCount);
		metric(sync, L"uploadData", p.HelperUploadDataMs, p.HelperUploadDataCount);
		AppendCpuRuntimeTrace(sync.str());

		std::wstringstream alloc;
		alloc << L"[NRIRecordProfile][alloc] frames=" << p.Frames;
		metric(alloc, L"createBuf", p.CreateBoundBufferMs, p.CreateBoundBufferCount);
		metric(alloc, L"allocBindBuf", p.AllocateAndBindBufferMemoryMs, p.AllocateAndBindBufferMemoryCount);
		metric(alloc, L"createTex", p.CreateBoundTextureMs, p.CreateBoundTextureCount);
		metric(alloc, L"allocBindTex", p.AllocateAndBindTextureMemoryMs, p.AllocateAndBindTextureMemoryCount);
		metric(alloc, L"transient", p.TransientUploadAllocMs, p.TransientUploadAllocCount);
		metric(alloc, L"transCreate", p.TransientUploadCreateMs, p.TransientUploadCreateCount);
		metric(alloc, L"transMap", p.TransientUploadMapMs, p.TransientUploadMapCount);
		metric(alloc, L"transMemcpy", p.TransientUploadMemcpyMs, p.TransientUploadMemcpyCount);
		metric(alloc, L"defaultTransient", p.TransientDefaultAllocMs, p.TransientDefaultAllocCount);
		metric(alloc, L"defaultCopyRecord", p.TransientDefaultCopyRecordMs, p.TransientDefaultCopyRecordCount);
		metric(alloc, L"defaultMemcpy", p.TransientDefaultMemcpyMs, p.TransientDefaultMemcpyCount);
		metric(alloc, L"freeTransient", p.FreeTransientBuffersMs, p.FreeTransientBuffersCount);
		alloc << L", transKB/frame=" << std::fixed << std::setprecision(1) << bytesPerFrameKB(p.TransientUploadBytes)
			<< L", reuseHit/frame=" << perFrame(p.TransientUploadReuseHit)
			<< L", reuseMiss/frame=" << perFrame(p.TransientUploadReuseMiss)
			<< L", defaultKB/frame=" << bytesPerFrameKB(p.TransientDefaultBytes)
			<< L", defaultReuseHit/frame=" << perFrame(p.TransientDefaultReuseHit)
			<< L", defaultReuseMiss/frame=" << perFrame(p.TransientDefaultReuseMiss);
		AppendCpuRuntimeTrace(alloc.str());

		std::wstringstream record;
		record << L"[NRIRecordProfile][record] frames=" << p.Frames;
		metric(record, L"openRP", p.OpenRenderPassMs, p.OpenRenderPassCount);
		metric(record, L"transitionTex", p.TransitionTextureMs, p.TransitionTextureCount);
		metric(record, L"bindPSO", p.BindGraphicsPipelineMs, p.BindGraphicsPipelineCount);
		metric(record, L"createBG", p.CreateGraphicsBindGroupMs, p.CreateGraphicsBindGroupCount);
		metric(record, L"bindBG", p.BindGraphicsBindGroupMs, p.BindGraphicsBindGroupCount);
		metric(record, L"applyDraw", p.ApplyForDrawMs, p.ApplyForDrawCount);
		metric(record, L"refreshDesc", p.RefreshDescriptorMs, p.RefreshDescriptorCount);
		metric(record, L"updateDesc", p.UpdateDescriptorRangesMs, p.UpdateDescriptorRangesCount);
		metric(record, L"setDescSet", p.SetDescriptorSetMs, p.SetDescriptorSetCount);
		metric(record, L"draw", p.DrawMs, p.DrawCount);
		metric(record, L"drawIdx", p.DrawIndexedMs, p.DrawIndexedCount);
		metric(record, L"drawIndirect", p.DrawIndirectMs, p.DrawIndirectCallCount);
		metric(record, L"drawIdxIndirect", p.DrawIndexedIndirectMs, p.DrawIndexedIndirectCallCount);
		metric(record, L"dispatch", p.DispatchMs, p.DispatchCount);
		metric(record, L"computeApply", p.ComputeApplyMs, p.ComputeApplyCount);
		metric(record, L"computeCBV", p.ComputeCbvUpdateMs, p.ComputeCbvUpdateCount);
		metric(record, L"rtApply", p.RtApplyMs, p.RtApplyCount);
		metric(record, L"rtCBV", p.RtCbvUpdateMs, p.RtCbvUpdateCount);
		metric(record, L"rtUpdateDesc", p.RtUpdateDescriptorRangesMs, p.RtUpdateDescriptorRangesCount);
		metric(record, L"rtSetDesc", p.RtSetDescriptorSetMs, p.RtSetDescriptorSetCount);
		metric(record, L"rtDispatchRays", p.RtDispatchRaysMs, p.RtDispatchRaysCount);
		record << L", indirectDraws/frame=" << std::fixed << std::setprecision(1) << perFrame(p.DrawIndirectDrawCount)
			<< L", indexedIndirectDraws/frame=" << perFrame(p.DrawIndexedIndirectDrawCount);
		AppendCpuRuntimeTrace(record.str());

		RecordProfile.Reset();
	}

	// TEMP perf instrumentation (CORONA_NRI_STATS): per-frame counts of the hot
	// recording ops to find the remaining CPU bottleneck without guessing.
	uint64_t StatAlloc = 0;   // CreateBoundBuffer (committed GPU heap allocations)
	uint64_t StatUDR = 0;     // UpdateDescriptorRanges calls
	uint64_t StatMap = 0;     // MapBuffer calls
	uint32_t StatFrames = 0;
	// Transient upload buffers are owned by the frame context that recorded them.
	// They are released only after that context's fence has completed.
	std::vector<std::shared_ptr<Buffer>> TransientBuffers;
	void FreeTransientBuffers(std::vector<std::shared_ptr<Buffer>>& transientBuffers)
	{
		const bool profile = IsNriRecordProfileEnabled();
		NriCpuProfileScope scope(profile, RecordProfile.FreeTransientBuffersMs, RecordProfile.FreeTransientBuffersCount);
		for (auto& w : transientBuffers)
		{
			auto it = Buffers.find(w.get());
			if (it != Buffers.end())
			{
				RecycleBuffer(std::move(it->second));
				Buffers.erase(it);
			}
		}
		transientBuffers.clear();
	}
	void FreeTransientBuffers()
	{
		FreeTransientBuffers(TransientBuffers);
	}
	uint32_t FrameRingCount() const
	{
		return FrameContexts.empty() ? 1u : static_cast<uint32_t>(FrameContexts.size());
	}
	FrameContext* ActiveFrameContext()
	{
		return ActiveFrameContextIndex < FrameContexts.size() ? &FrameContexts[ActiveFrameContextIndex] : nullptr;
	}
	const FrameContext* ActiveFrameContext() const
	{
		return ActiveFrameContextIndex < FrameContexts.size() ? &FrameContexts[ActiveFrameContextIndex] : nullptr;
	}
	bool EnsureFrameContexts(uint32_t count)
	{
		if (!Device || !GraphicsQueue)
			return false;
		count = std::max(1u, count);
		if (FrameContexts.size() >= count)
			return true;
		const size_t oldCount = FrameContexts.size();
		FrameContexts.resize(count);
		for (size_t i = oldCount; i < FrameContexts.size(); ++i)
		{
			FrameContext& frame = FrameContexts[i];
			if (Core.CreateCommandAllocator(*GraphicsQueue, frame.allocator) != nri::Result::SUCCESS || !frame.allocator ||
				Core.CreateCommandBuffer(*frame.allocator, frame.commandBuffer) != nri::Result::SUCCESS || !frame.commandBuffer)
			{
				return false;
			}
		}
		return true;
	}
	void WaitForFrameContext(FrameContext& frame)
	{
		if (FrameFence && frame.fenceValue != 0)
		{
			const bool profile = IsNriRecordProfileEnabled();
			const auto start = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
			Core.Wait(*FrameFence, frame.fenceValue);
			NriCpuProfileAdd(profile, RecordProfile.WaitFrameContextMs, RecordProfile.WaitFrameContextCount, start);
		}
	}
	void WaitForFrameContexts()
	{
		for (FrameContext& frame : FrameContexts)
			WaitForFrameContext(frame);
	}
	bool EnsureImmediateContexts(uint32_t count)
	{
		if (!Device || !GraphicsQueue)
			return false;
		count = std::max(1u, count);
		if (ImmediateContexts.size() >= count)
			return true;
		const size_t oldCount = ImmediateContexts.size();
		ImmediateContexts.resize(count);
		for (size_t i = oldCount; i < ImmediateContexts.size(); ++i)
		{
			ImmediateContext& ctx = ImmediateContexts[i];
			if (Core.CreateCommandAllocator(*GraphicsQueue, ctx.allocator) != nri::Result::SUCCESS || !ctx.allocator ||
				Core.CreateCommandBuffer(*ctx.allocator, ctx.commandBuffer) != nri::Result::SUCCESS || !ctx.commandBuffer)
			{
				return false;
			}
		}
		return true;
	}
	void WaitForImmediateContext(ImmediateContext& ctx)
	{
		if (Fence && ctx.fenceValue != 0)
		{
			const bool profile = IsNriRecordProfileEnabled();
			const auto start = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
			Core.Wait(*Fence, ctx.fenceValue);
			NriCpuProfileAdd(profile, RecordProfile.ImmediateFenceWaitMs, RecordProfile.ImmediateFenceWaitCount, start);
		}
	}
	void WaitForImmediateContexts()
	{
		for (ImmediateContext& ctx : ImmediateContexts)
			WaitForImmediateContext(ctx);
	}
	ImmediateContext* AcquireImmediateContext()
	{
		if (!EnsureImmediateContexts(kQueuedFrameNum + 1u) || ImmediateContexts.empty())
			return nullptr;
		ImmediateContext& ctx = ImmediateContexts[ImmediateSubmitIndex % ImmediateContexts.size()];
		++ImmediateSubmitIndex;
		WaitForImmediateContext(ctx);
		return &ctx;
	}
	void DestroyImmediateContexts()
	{
		WaitForImmediateContexts();
		for (ImmediateContext& ctx : ImmediateContexts)
		{
			if (ctx.commandBuffer) { Core.DestroyCommandBuffer(ctx.commandBuffer); ctx.commandBuffer = nullptr; }
			if (ctx.allocator) { Core.DestroyCommandAllocator(ctx.allocator); ctx.allocator = nullptr; }
			ctx.fenceValue = 0;
		}
		ImmediateContexts.clear();
		ImmediateSubmitIndex = 0;
	}
	void DestroyFrameContexts()
	{
		WaitForFrameContexts();
		for (FrameContext& frame : FrameContexts)
		{
			FreeTransientBuffers(frame.transientBuffers);
			if (frame.commandBuffer) { Core.DestroyCommandBuffer(frame.commandBuffer); frame.commandBuffer = nullptr; }
			if (frame.allocator) { Core.DestroyCommandAllocator(frame.allocator); frame.allocator = nullptr; }
			frame.fenceValue = 0;
		}
		FrameContexts.clear();
		ActiveFrameContextIndex = 0;
	}

	struct TextureAlloc
	{
		nri::Texture* texture = nullptr;
		std::vector<nri::Memory*> memory;
	};
	std::unordered_map<Texture*, TextureAlloc> Textures;
	std::unordered_map<Sampler*, nri::Descriptor*> Samplers;

	// --- Bindless registry (Phase C foundation) ---------------------------------
	// Global index assignment + cached SRV descriptors for material textures
	// (shader space10 MaterialTextures[]) and geometry byte-address buffers
	// (space12 GeometryBuffers[]). RT pipelines write these into their bindless
	// descriptor sets at Apply. Indices are stable for a resource's lifetime;
	// generation distinguishes stale handles. Free-list reuse on unregister.
	std::unordered_map<const void*, RHIBindlessHandle> BindlessTexHandles;
	std::vector<nri::Descriptor*> BindlessTexDescs;   // index -> texture SRV (owned)
	std::vector<uint32_t> BindlessTexGen;
	std::vector<uint32_t> BindlessTexFreeList;
	std::unordered_map<const void*, RHIBindlessHandle> BindlessBufHandles;
	std::vector<nri::Descriptor*> BindlessBufDescs;   // index -> raw buffer SRV (owned)
	std::vector<uint32_t> BindlessBufGen;
	std::vector<uint32_t> BindlessBufFreeList;

	RHIBindlessHandle RegisterBindlessTextureImpl(nri::Texture* nt, const void* key)
	{
		if (!nt || !key) return {};
		auto existing = BindlessTexHandles.find(key);
		if (existing != BindlessTexHandles.end() && existing->second.IsValid())
			return existing->second;
		const nri::TextureDesc& td = Core.GetTextureDesc(*nt);
		nri::TextureViewDesc tvd = {}; tvd.texture = nt; tvd.type = nri::TextureView::TEXTURE;
		tvd.format = td.format; tvd.mipNum = nri::REMAINING; tvd.layerNum = nri::REMAINING;
		nri::Descriptor* d = nullptr;
		if (Core.CreateTextureView(tvd, d) != nri::Result::SUCCESS || !d) return {};
		uint32_t idx;
		if (!BindlessTexFreeList.empty()) { idx = BindlessTexFreeList.back(); BindlessTexFreeList.pop_back(); BindlessTexDescs[idx] = d; }
		else { idx = (uint32_t)BindlessTexDescs.size(); BindlessTexDescs.push_back(d); BindlessTexGen.push_back(0); }
		if (BindlessTexGen[idx] == 0) BindlessTexGen[idx] = 1;
		RHIBindlessHandle h; h.Index = idx; h.Generation = BindlessTexGen[idx];
		BindlessTexHandles[key] = h;
		return h;
	}
	RHIBindlessHandle RegisterBindlessBufferImpl(nri::Buffer* nb, uint64_t sizeBytes, const void* key)
	{
		if (!nb || !key || sizeBytes == 0) return {};
		auto existing = BindlessBufHandles.find(key);
		if (existing != BindlessBufHandles.end() && existing->second.IsValid())
			return existing->second;
		// ByteAddressBuffer SRV (raw).
		nri::BufferViewDesc bvd = {}; bvd.buffer = nb; bvd.type = nri::BufferView::BYTE_ADDRESS_BUFFER; bvd.offset = 0; bvd.size = sizeBytes;
		nri::Descriptor* d = nullptr;
		if (Core.CreateBufferView(bvd, d) != nri::Result::SUCCESS || !d) return {};
		uint32_t idx;
		if (!BindlessBufFreeList.empty()) { idx = BindlessBufFreeList.back(); BindlessBufFreeList.pop_back(); BindlessBufDescs[idx] = d; }
		else { idx = (uint32_t)BindlessBufDescs.size(); BindlessBufDescs.push_back(d); BindlessBufGen.push_back(0); }
		if (BindlessBufGen[idx] == 0) BindlessBufGen[idx] = 1;
		RHIBindlessHandle h; h.Index = idx; h.Generation = BindlessBufGen[idx];
		BindlessBufHandles[key] = h;
		return h;
	}

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
		const bool profile = IsNriRecordProfileEnabled();
		NriCpuProfileScope scope(profile, RecordProfile.CreateBoundBufferMs, RecordProfile.CreateBoundBufferCount);

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
		const auto allocStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
		if (Helper.AllocateAndBindMemory(*Device, rg, outMemory.data()) != nri::Result::SUCCESS)
		{
			NriCpuProfileAdd(profile, RecordProfile.AllocateAndBindBufferMemoryMs, RecordProfile.AllocateAndBindBufferMemoryCount, allocStart);
			Core.DestroyBuffer(outBuffer);
			outBuffer = nullptr;
			outMemory.clear();
			return false;
		}
		NriCpuProfileAdd(profile, RecordProfile.AllocateAndBindBufferMemoryMs, RecordProfile.AllocateAndBindBufferMemoryCount, allocStart);
		++StatAlloc;
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
		const bool profile = IsNriRecordProfileEnabled();
		NriCpuProfileScope scope(profile, RecordProfile.CreateBoundTextureMs, RecordProfile.CreateBoundTextureCount);
		if (Core.CreateTexture(*Device, td, outTex) != nri::Result::SUCCESS || outTex == nullptr)
			return false;

		nri::Texture* texList[1] = { outTex };
		nri::ResourceGroupDesc rg = {};
		rg.memoryLocation = nri::MemoryLocation::DEVICE;
		rg.textures = texList;
		rg.textureNum = 1;

		const uint32_t allocNum = Helper.CalculateAllocationNumber(*Device, rg);
		outMem.resize(allocNum, nullptr);
		const auto allocStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
		if (Helper.AllocateAndBindMemory(*Device, rg, outMem.data()) != nri::Result::SUCCESS)
		{
			NriCpuProfileAdd(profile, RecordProfile.AllocateAndBindTextureMemoryMs, RecordProfile.AllocateAndBindTextureMemoryCount, allocStart);
			Core.DestroyTexture(outTex);
			outTex = nullptr;
			outMem.clear();
			return false;
		}
		NriCpuProfileAdd(profile, RecordProfile.AllocateAndBindTextureMemoryMs, RecordProfile.AllocateAndBindTextureMemoryCount, allocStart);
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
			cpd.cache = PipelineCache;
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
	bool SubmitImmediate(const char* label, RecordFn record, bool waitForCompletion = true)
	{
		if (!Device || !GraphicsQueue || !Fence || ActiveCmd)
		{
			if (label)
				OutputDebugStringA(label);
			return false;
		}

		ImmediateContext* immediateContext = AcquireImmediateContext();
		nri::CommandAllocator* allocator = immediateContext ? immediateContext->allocator : CmdAllocator;
		nri::CommandBuffer* commandBuffer = immediateContext ? immediateContext->commandBuffer : CmdBuffer;
		if (!allocator || !commandBuffer)
			return false;

		if (!immediateContext && FenceValue != 0)
		{
			const bool profile = IsNriRecordProfileEnabled();
			const auto waitStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
			Core.Wait(*Fence, FenceValue);
			NriCpuProfileAdd(profile, RecordProfile.ImmediateFenceWaitMs, RecordProfile.ImmediateFenceWaitCount, waitStart);
		}

		Core.ResetCommandAllocator(*allocator);
		if (Core.BeginCommandBuffer(*commandBuffer, nullptr) != nri::Result::SUCCESS)
			return false;

		record(*commandBuffer);

		if (Core.EndCommandBuffer(*commandBuffer) != nri::Result::SUCCESS)
			return false;

		nri::FenceSubmitDesc signalFence = {};
		signalFence.fence = Fence;
		signalFence.value = ++FenceValue;
		signalFence.stages = nri::StageBits::ALL;

		nri::CommandBuffer* commandBuffers[1] = { commandBuffer };
		nri::QueueSubmitDesc submit = {};
		submit.commandBuffers = commandBuffers;
		submit.commandBufferNum = 1;
		submit.signalFences = &signalFence;
		submit.signalFenceNum = 1;

		if (Core.QueueSubmit(*GraphicsQueue, submit) != nri::Result::SUCCESS)
			return false;
		if (immediateContext)
			immediateContext->fenceValue = signalFence.value;
		if (!waitForCompletion)
			return true;
		{
			const bool profile = IsNriRecordProfileEnabled();
			const auto waitStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
			Core.Wait(*Fence, FenceValue);
			NriCpuProfileAdd(profile, RecordProfile.ImmediateFenceWaitMs, RecordProfile.ImmediateFenceWaitCount, waitStart);
		}
		if (immediateContext)
			immediateContext->fenceValue = 0;
		return Core.GetFenceValue(*Fence) >= FenceValue;
	}

	bool ResetQueries(nri::QueryPool* queryPool, uint32_t offset, uint32_t count, const char* label)
	{
		if (!queryPool || count == 0 || !Core.CmdResetQueries)
			return false;
		if (ActiveCmd)
		{
			EndRP();
			Core.CmdResetQueries(*ActiveCmd, *queryPool, offset, count);
			return true;
		}
		return SubmitImmediate(label, [&](nri::CommandBuffer& cmd)
		{
			Core.CmdResetQueries(cmd, *queryPool, offset, count);
		});
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
		rtd.cache = PipelineCache;
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
	uint64_t AccelerationStructureHandle = 0;
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
		if (blas->AccelerationStructureHandle == 0)
			blas->AccelerationStructureHandle = as->Owner->RT.GetAccelerationStructureHandle(*blas->AccelerationStructure);
		dst[i].accelerationStructureHandle = blas->AccelerationStructureHandle;
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
	as->AccelerationStructureHandle = m->RT.GetAccelerationStructureHandle(*as->AccelerationStructure);
	if (as->AccelerationStructureHandle == 0)
	{
		AppendCpuRuntimeTrace(L"[NRIRTAS] BLAS handle unavailable");
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
	explicit NRIComputePSO(NRIBackend::Impl* impl) : m(impl)
	{
		if (m)
			Alive = m->Alive;
	}
	~NRIComputePSO() override
	{
		if (!BackendAlive()) return;
		if (Pipeline) m->Core.DestroyPipeline(Pipeline);
		if (Layout) m->Core.DestroyPipelineLayout(Layout);
		if (Pool) m->Core.DestroyDescriptorPool(Pool);
		for (auto& b : Bindings) if (b.ownsDesc && b.desc) m->Core.DestroyDescriptor(b.desc);
		for (auto& kv : CbvBuffers)
		{
			for (CbvBuf& cb : kv.second)
			{
				if (cb.view) m->Core.DestroyDescriptor(cb.view);
				m->FreeBuffer(cb.buffer, cb.memory);
			}
		}
	}

	enum class RegClass { SRV, UAV, CBV, Sampler };
	enum class ResKind { None, TexSRV, TexUAV, BufSRV, BufUAV, VertexUAV, CBV, Sampler };
	struct Binding
	{
		std::string name;
		uint32_t reg = 0;
		RegClass regClass = RegClass::SRV;
		ResKind kind = ResKind::None;
		Texture* tex = nullptr;
		Buffer* buf = nullptr;
		VertexBuffer* vb = nullptr;
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
	void SetVertexBufferUAV(const std::string& name, VertexBuffer* vertexBuffer) override
	{
		Binding* b = Find(name);
		if (!b)
		{
			AddBinding(name, 0, RegClass::UAV);
			b = Find(name);
		}
		if (!b)
			return;
		b->kind = ResKind::VertexUAV;
		b->tex = nullptr;
		b->buf = nullptr;
		b->vb = vertexBuffer;
		b->samp = nullptr;
	}
	void SetSampler(const std::string& name, Sampler* s) override { SetRes(name, ResKind::Sampler, nullptr, nullptr, s); }
	void SetCBVValue(const std::string& name, void* data) override
	{
		if (!m->Device || !data) return;
		const bool profile = IsNriRecordProfileEnabled();
		NriCpuProfileScope cbvScope(profile, m->RecordProfile.ComputeCbvUpdateMs, m->RecordProfile.ComputeCbvUpdateCount);
		auto sit = CbvSizeByName.find(name);
		const uint32_t size = (sit != CbvSizeByName.end()) ? sit->second : 0u;
		if (size == 0) return;
		const uint32_t alignedSize = (size + 255u) & ~255u; // D3D12 CB alignment
		std::vector<CbvBuf>& cbRing = CbvBuffers[name];
		if (cbRing.size() < NRIBackend::Impl::kQueuedFrameNum)
			cbRing.resize(NRIBackend::Impl::kQueuedFrameNum);
		const uint32_t frameSlot = m->ActiveFrameContextIndex % std::max(1u, m->FrameRingCount());
		CbvBuf& cb = cbRing[frameSlot];
		if (!cb.buffer)
		{
			if (!m->CreateBoundBuffer(alignedSize, 0, nri::BufferUsageBits::CONSTANT_BUFFER, nri::MemoryLocation::HOST_UPLOAD, cb.buffer, cb.memory))
			{ CbvBuffers.erase(name); return; }
			cb.size = alignedSize;
			nri::BufferViewDesc bvd = {};
			bvd.buffer = cb.buffer; bvd.type = nri::BufferView::CONSTANT_BUFFER; bvd.offset = 0; bvd.size = alignedSize;
			m->Core.CreateBufferView(bvd, cb.view);
			cb.mapped = m->Core.MapBuffer(*cb.buffer, 0, alignedSize);
			++m->StatMap;
		}
		if (Binding* b = Find(name)) { b->kind = ResKind::CBV; b->desc = cb.view; b->ownsDesc = false; }
		if (cb.mapped)
			memcpy(cb.mapped, data, size);
	}

	void Apply() override
	{
		if (!m->ActiveCmd) return;
		const bool profile = IsNriRecordProfileEnabled();
		NriCpuProfileScope applyScope(profile, m->RecordProfile.ComputeApplyMs, m->RecordProfile.ComputeApplyCount);
		m->EndRP();
		if (!EnsureInit()) return;

		for (Binding& b : Bindings)
			RefreshDescriptor(b);
		const uint32_t frameSlot = m->ActiveFrameContextIndex % std::max(1u, m->FrameRingCount());
		std::vector<nri::DescriptorSet*>* setsForFrame = frameSlot < Sets.size() ? &Sets[frameSlot] : nullptr;

		// Write descriptors into the sets. Batch the range writes to avoid a driver
		// call per binding on compute-heavy frames.
		static std::vector<nri::UpdateDescriptorRangeDesc> updateScratch;
		updateScratch.clear();
		for (Binding& b : Bindings)
		{
			if (b.kind == ResKind::None || !b.desc || !setsForFrame || b.setIndex >= setsForFrame->size() || !(*setsForFrame)[b.setIndex])
				continue;
			nri::UpdateDescriptorRangeDesc upd = {};
			b.descSingle[0] = b.desc;
			upd.descriptorSet = (*setsForFrame)[b.setIndex];
			upd.rangeIndex = b.rangeIndex;
			upd.baseDescriptor = 0;
			upd.descriptors = b.descSingle;
			upd.descriptorNum = 1;
			updateScratch.push_back(upd);
		}
		if (!updateScratch.empty())
		{
			++m->StatUDR;
			m->Core.UpdateDescriptorRanges(updateScratch.data(), static_cast<uint32_t>(updateScratch.size()));
		}

		if (Pool) m->Core.CmdSetDescriptorPool(*m->ActiveCmd, *Pool);
		m->Core.CmdSetPipelineLayout(*m->ActiveCmd, nri::BindPoint::COMPUTE, *Layout);
		const uint32_t setCount = setsForFrame ? static_cast<uint32_t>(setsForFrame->size()) : 0u;
		for (uint32_t s = 0; s < setCount; ++s)
		{
			if (!(*setsForFrame)[s]) continue;
			nri::SetDescriptorSetDesc sd = {};
			sd.setIndex = s;
			sd.descriptorSet = (*setsForFrame)[s];
			sd.bindPoint = nri::BindPoint::COMPUTE;
			m->Core.CmdSetDescriptorSet(*m->ActiveCmd, sd);
		}
		m->Core.CmdSetPipeline(*m->ActiveCmd, *Pipeline);
	}

	nri::Pipeline* GetPipeline() const { return Pipeline; }

private:
	struct CbvBuf { nri::Buffer* buffer = nullptr; std::vector<nri::Memory*> memory; uint32_t size = 0; nri::Descriptor* view = nullptr; void* mapped = nullptr; };

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
		b->kind = k; b->tex = t; b->buf = bufp; b->vb = nullptr; b->samp = s;
	}
	static nri::DescriptorType ToDescriptorType(ResKind k)
	{
		switch (k)
		{
		case ResKind::TexSRV: return nri::DescriptorType::TEXTURE;
		case ResKind::TexUAV: return nri::DescriptorType::STORAGE_TEXTURE;
		case ResKind::BufSRV: return nri::DescriptorType::STRUCTURED_BUFFER;
		case ResKind::BufUAV: return nri::DescriptorType::STORAGE_STRUCTURED_BUFFER;
		case ResKind::VertexUAV: return nri::DescriptorType::STORAGE_STRUCTURED_BUFFER;
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
			case ResKind::VertexUAV: ++storSbufN; break;
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
			const uint32_t frameSlots = NRIBackend::Impl::kQueuedFrameNum;
			nri::DescriptorPoolDesc pd = {};
			pd.descriptorSetMaxNum = static_cast<uint32_t>(sets.size()) * frameSlots;
			pd.textureMaxNum = texN * frameSlots; pd.storageTextureMaxNum = storTexN * frameSlots;
			pd.structuredBufferMaxNum = sbufN * frameSlots; pd.storageStructuredBufferMaxNum = storSbufN * frameSlots;
			pd.constantBufferMaxNum = cbvN * frameSlots; pd.samplerMaxNum = sampN * frameSlots;
			if (m->Core.CreateDescriptorPool(*m->Device, pd, Pool) != nri::Result::SUCCESS)
				return false;
			Sets.assign(frameSlots, std::vector<nri::DescriptorSet*>(sets.size(), nullptr));
			for (uint32_t frameSlot = 0; frameSlot < frameSlots; ++frameSlot)
				for (uint32_t s = 0; s < sets.size(); ++s)
					m->Core.AllocateDescriptorSets(*Pool, *Layout, s, &Sets[frameSlot][s], 1, 0);
		}

		nri::ComputePipelineDesc cpd = {};
		cpd.pipelineLayout = Layout;
		cpd.shader.stage = nri::StageBits::COMPUTE_SHADER;
		cpd.shader.bytecode = Dxil.data();
		cpd.shader.size = Dxil.size();
		cpd.shader.entryPointName = EntryPoint.c_str();
		cpd.cache = m->PipelineCache;
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
		void* res = b.tex ? (void*)b.tex : (b.buf ? (void*)b.buf : (void*)b.vb);
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
		else if (b.kind == ResKind::VertexUAV)
		{
			if (!b.vb) return;
			auto it = m->VBs.find(b.vb);
			if (it == m->VBs.end() || !it->second.buffer || it->second.capacity == 0) return;
			nri::BufferViewDesc bvd = {};
			bvd.buffer = it->second.buffer;
			bvd.type = nri::BufferView::STORAGE_BYTE_ADDRESS_BUFFER;
			bvd.offset = 0;
			bvd.size = it->second.capacity;
			nri::Descriptor* d = nullptr;
			if (m->Core.CreateBufferView(bvd, d) == nri::Result::SUCCESS) { b.desc = d; b.ownsDesc = true; }
		}
		else // BufSRV / BufUAV
		{
			if (!b.buf) return;
			auto it = m->Buffers.find(b.buf);
			if (it == m->Buffers.end() || !it->second.buffer) return;
			nri::BufferViewDesc bvd = {};
			bvd.buffer = it->second.buffer;
			bvd.offset = 0;
			bvd.size = static_cast<uint64_t>(b.buf->NumElements) * b.buf->ElementSize;
			if (b.buf->Type == Buffer::BYTE_ADDRESS)
			{
				bvd.type = (b.kind == ResKind::BufUAV) ? nri::BufferView::STORAGE_BYTE_ADDRESS_BUFFER : nri::BufferView::BYTE_ADDRESS_BUFFER;
			}
			else
			{
				bvd.type = (b.kind == ResKind::BufUAV) ? nri::BufferView::STORAGE_STRUCTURED_BUFFER : nri::BufferView::STRUCTURED_BUFFER;
				bvd.structureStride = b.buf->ElementSize ? b.buf->ElementSize : 4;
			}
			nri::Descriptor* d = nullptr;
			if (m->Core.CreateBufferView(bvd, d) == nri::Result::SUCCESS) { b.desc = d; b.ownsDesc = true; }
		}
	}

	bool BackendAlive() const
	{
		const std::shared_ptr<std::atomic_bool> alive = Alive.lock();
		return m && alive && alive->load(std::memory_order_acquire) && m->Device;
	}

	NRIBackend::Impl* m = nullptr;
	std::weak_ptr<std::atomic_bool> Alive;
	std::vector<Binding> Bindings;
	std::unordered_map<std::string, uint32_t> CbvSizeByName;
	std::unordered_map<std::string, std::vector<CbvBuf>> CbvBuffers;
	std::vector<uint8_t> Dxil;
	std::string EntryPoint;
	nri::PipelineLayout* Layout = nullptr;
	nri::Pipeline* Pipeline = nullptr;
	nri::DescriptorPool* Pool = nullptr;
	std::vector<std::vector<nri::DescriptorSet*>> Sets;
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
	explicit NRIRTPipelineStateObject(NRIBackend::Impl* impl) : m(impl)
	{
		if (m)
			Alive = m->Alive;
	}
	~NRIRTPipelineStateObject() override
	{
		if (!BackendAlive()) return;
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
			for (CbvBuf& cb : kv.second)
			{
				if (cb.view)
					m->Core.DestroyDescriptor(cb.view);
				m->FreeBuffer(cb.buffer, cb.memory);
			}
		}
		m->FreeBuffer(SbtUploadBuffer, SbtUploadMemory);
		m->FreeBuffer(SbtBuffer, SbtMemory);
	}

	void SetNumInstances(uint32_t numInstances) override { NumInstances = std::max(1u, numInstances); }
	bool UsesSharedHitRecords() const override { return true; }
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

	// Typed binding schema (bindless RHI). The base default forwards to the
	// by-name overloads and drops RegisterSpace/RuntimeArray/Bindless, which the
	// RT bindless tables (space10/12 runtime arrays) need — capture them here.
	void BindSRV(const std::string& shader, const RHIBindingDesc& d) override
	{
		AddBinding(shader, d.Name, d.RegisterIndex, RegClass::SRV, 0);
		ApplyBindingSchema(shader, d, false);
	}
	void BindUAV(const std::string& shader, const RHIBindingDesc& d) override
	{
		AddBinding(shader, d.Name, d.RegisterIndex, RegClass::UAV, 0);
		ApplyBindingSchema(shader, d, true);
	}
	void ApplyBindingSchema(const std::string& shader, const RHIBindingDesc& d, bool isUAV)
	{
		Binding* b = Find(shader, d.Name);
		if (!b) return;
		b->registerSpace = d.RegisterSpace;
		// Acceleration structures (gRtScene) must be typed ACCELERATION_STRUCTURE, not
		// TEXTURE — otherwise the descriptor range type won't match the bound TLAS
		// descriptor and UpdateDescriptorRanges validation rejects the write.
		if (d.ResourceKind == RHIResourceKind::AccelerationStructure ||
			d.DescriptorKind == RHIDescriptorKind::AccelerationStructure)
		{
			b->kind = ResKind::Accel;
			b->descriptorType = nri::DescriptorType::ACCELERATION_STRUCTURE;
			return;
		}
		const bool isBuf = (d.ResourceKind == RHIResourceKind::Buffer);
		if (isUAV) { b->kind = isBuf ? ResKind::BufUAV : ResKind::TexUAV; b->descriptorType = isBuf ? nri::DescriptorType::STORAGE_STRUCTURED_BUFFER : nri::DescriptorType::STORAGE_TEXTURE; }
		else       { b->kind = isBuf ? ResKind::BufSRV : ResKind::TexSRV; b->descriptorType = isBuf ? nri::DescriptorType::STRUCTURED_BUFFER : nri::DescriptorType::TEXTURE; }
		if (d.RuntimeArray || d.DescriptorCount == RHI_BINDLESS_ARRAY)
		{
			b->descriptorNum = kRtBindlessCapacity;
			b->bindless = true;
			b->bindlessSrc = isBuf ? 2 : 1;
		}
		else if (d.DescriptorCount > 1)
			b->descriptorNum = d.DescriptorCount;
	}
	// Global bindless tables: create the binding at the BindlessResources.hlsli
	// convention space (textures=10, byte-address buffers=12) and fill it from the
	// backend registry at Apply.
	bool SetBindlessTextureTable(const std::string& shader, const std::string& bindingName) override
	{
		AddBinding(shader, bindingName, 0, RegClass::SRV, 0);
		Binding* b = Find(shader, bindingName);
		if (!b) return false;
		b->registerSpace = 10; b->kind = ResKind::TexSRV; b->descriptorType = nri::DescriptorType::TEXTURE;
		b->descriptorNum = kRtBindlessCapacity; b->bindless = true; b->bindlessSrc = 1;
		return true;
	}
	bool SetBindlessBufferTable(const std::string& shader, const std::string& bindingName) override
	{
		AddBinding(shader, bindingName, 0, RegClass::SRV, 0);
		Binding* b = Find(shader, bindingName);
		if (!b) return false;
		b->registerSpace = 12; b->kind = ResKind::BufSRV; b->descriptorType = nri::DescriptorType::STRUCTURED_BUFFER;
		b->descriptorNum = kRtBindlessCapacity; b->bindless = true; b->bindlessSrc = 2;
		return true;
	}
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
		Binding* b = Find(shader, bindingName);
		if (!b)
		{
			// The bindless-RHI record buffers are referenced by reflection on DX12
			// but never explicitly BindSRV'd; auto-declare them at the
			// BindlessResources.hlsli convention space so the NRI root signature
			// matches the hit shaders (single structured/byte-address SRVs).
			uint32_t space = UINT32_MAX;
			if (bindingName == "RtMaterials") space = 11;
			else if (bindingName == "RtGeometries") space = 13;
			else if (bindingName == "RtInstanceProperties") space = 14;
			if (space == UINT32_MAX) return;
			AddBinding(shader, bindingName, 0, RegClass::SRV, 0);
			b = Find(shader, bindingName);
			if (!b) return;
			b->registerSpace = space;
		}
		b->kind = ResKind::BufSRV; b->buf = buffer; b->descriptorType = nri::DescriptorType::STRUCTURED_BUFFER;
	}
	void SetAccelerationStructure(const std::string& shader, const std::string& bindingName, const std::shared_ptr<RTAS>& rtas, int = -1) override
	{
		if (Binding* b = Find(shader, bindingName)) { b->kind = ResKind::Accel; b->descriptorType = nri::DescriptorType::ACCELERATION_STRUCTURE; b->rtas = rtas; }
	}
	void SetSampler(const std::string& shader, const std::string& bindingName, Sampler* sampler, int = -1) override
	{
		if (Binding* b = Find(shader, bindingName)) { b->kind = ResKind::Sampler; b->samp = sampler; }
	}
	void SetCBVValue(const std::string& shader, const std::string& bindingName, void* pData, int = -1) override
	{
		if (!m || !m->Device || !pData)
			return;
		const bool profile = IsNriRecordProfileEnabled();
		NriCpuProfileScope cbvScope(profile, m->RecordProfile.RtCbvUpdateMs, m->RecordProfile.RtCbvUpdateCount);
		Binding* b = Find(shader, bindingName);
		if (!b || b->cbSize == 0)
			return;

		const uint32_t alignedSize = (b->cbSize + 255u) & ~255u;
		std::vector<CbvBuf>& cbRing = CbvBuffers[bindingName];
		if (cbRing.size() < NRIBackend::Impl::kQueuedFrameNum)
			cbRing.resize(NRIBackend::Impl::kQueuedFrameNum);
		const uint32_t frameSlot = m->ActiveFrameContextIndex % std::max(1u, m->FrameRingCount());
		CbvBuf& cb = cbRing[frameSlot];
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
			cb.mapped = m->Core.MapBuffer(*cb.buffer, 0, alignedSize);
			++m->StatMap;
		}
		if (cb.mapped)
			memcpy(cb.mapped, pData, b->cbSize);
		b->kind = ResKind::CBV;
		b->desc = cb.view;
		b->ownsDesc = false;
	}

	void ResetHitProgram(uint32_t) override { HitBindSlot = 0; }
	void StartHitProgram(const std::string&, uint32_t) override { HitBindSlot = 0; }
	void AddTextureSRVToHitProgram(const std::string&, Texture* texture, uint32_t instanceIndex)
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
	void AddBufferSRVToHitProgram(const std::string&, Buffer* buffer, uint32_t instanceIndex)
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
	void AddSceneGeometrySRVsToHitProgram(const std::string&, VertexBuffer* sceneVertexBuffer, IndexBuffer* sceneIndexBuffer, uint32_t)
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

	bool ApplyIndirect(Buffer* indirectArgumentBuffer, uint64_t byteOffset) override
	{
		if (!m || !m->ActiveCmd || !m->RT.CmdDispatchRaysIndirect || !indirectArgumentBuffer)
			return false;
		if (!EnsureBuilt())
			return false;
		auto it = m->Buffers.find(indirectArgumentBuffer);
		if (it == m->Buffers.end() || !it->second.buffer)
			return false;

		Apply(0, 0);
		const bool profile = IsNriRecordProfileEnabled();
		const auto dispatchStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
		m->RT.CmdDispatchRaysIndirect(*m->ActiveCmd, *it->second.buffer, byteOffset);
		NriCpuProfileAdd(profile, m->RecordProfile.RtDispatchRaysMs, m->RecordProfile.RtDispatchRaysCount, dispatchStart);
		return true;
	}
	bool GetDispatchRaysIndirectTemplate(uint32_t width, uint32_t height, RtDispatchRaysIndirectTemplate& outTemplate) const override
	{
		static_assert(sizeof(RtDispatchRaysIndirectTemplate) == sizeof(nri::DispatchRaysIndirectDesc), "NRI DispatchRays indirect args must match Corona template.");
		outTemplate = {};
		if (!m || !m->Core.GetBufferDeviceAddress || !m->RT.CmdDispatchRaysIndirect)
			return false;
		if (!Pipeline && !const_cast<NRIRTPipelineStateObject*>(this)->EnsureBuilt())
			return false;
		if (!Pipeline || !SbtBuffer)
			return false;
		const uint64_t sbtAddress = m->Core.GetBufferDeviceAddress(*SbtBuffer);
		if (sbtAddress == 0 || SbtEntrySize == 0)
			return false;

		outTemplate.RayGenerationStartAddress = sbtAddress + RaygenOffset;
		outTemplate.RayGenerationSizeInBytes = SbtEntrySize;
		outTemplate.MissStartAddress = sbtAddress + MissOffset;
		outTemplate.MissSizeInBytes = SbtEntrySize * std::max(1u, MissGroupCount);
		outTemplate.MissStrideInBytes = SbtEntrySize;
		if (HitGroupCount > 0)
		{
			outTemplate.HitGroupStartAddress = sbtAddress + HitOffset;
			outTemplate.HitGroupSizeInBytes = SbtEntrySize * HitGroupCount;
			outTemplate.HitGroupStrideInBytes = SbtEntrySize;
		}
		outTemplate.Width = width;
		outTemplate.Height = height;
		outTemplate.Depth = 1;
		return true;
	}

	// Defer the actual pipeline build to the first Apply: the renderer declares
	// the bindless tables (SetBindlessTextureTable / record buffers) at dispatch
	// time, AFTER InitRS, so building the root signature here would miss them.
	bool InitRS(const std::string& shaderFile) override
	{
		PendingShaderFile = shaderFile;
		return m && m->Device && m->HasRayTracing;
	}
	bool EnsureBuilt()
	{
		if (Pipeline) return true;
		if (bBuildAttempted) return false;
		bBuildAttempted = true;
		return BuildPipelineFromShader(PendingShaderFile);
	}
	std::string PendingShaderFile;
	bool bBuildAttempted = false;

	bool BuildPipelineFromShader(const std::string& shaderFile)
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
		rtd.cache = m->PipelineCache;
		if (m->RT.CreateRayTracingPipeline(*m->Device, rtd, Pipeline) != nri::Result::SUCCESS || !Pipeline)
		{
			AppendCpuRuntimeTrace(L"[NRIRTPSO] CreateRayTracingPipeline failed shader=\"" + shaderPath.wstring() + L"\"");
			return false;
		}

		const bool sbtOk = BuildShaderBindingTable(static_cast<uint32_t>(groups.size()));
		AppendCpuRuntimeTrace(
			L"[NRIRTPSO][SBT] shader=\"" + shaderPath.filename().wstring() +
			L"\" shaders=" + std::to_wstring(Shaders.size()) +
			L" groups=" + std::to_wstring(groups.size()) +
			L" raygen=" + std::to_wstring(RaygenGroupCount) +
			L" miss=" + std::to_wstring(MissGroupCount) +
			L" hit=" + std::to_wstring(HitGroupCount) +
			L" entrySize=" + std::to_wstring(SbtEntrySize) +
			L" missOff=" + std::to_wstring(MissOffset) +
			L" hitOff=" + std::to_wstring(HitOffset) +
			L" ok=" + std::to_wstring(sbtOk ? 1 : 0));
		if (!sbtOk)
		{
			m->FreeBuffer(SbtUploadBuffer, SbtUploadMemory);
			m->FreeBuffer(SbtBuffer, SbtMemory);
			if (Pipeline)
			{
				m->Core.DestroyPipeline(Pipeline);
				Pipeline = nullptr;
			}
			return false;
		}
		return true;
	}

	void Apply(uint32_t width, uint32_t height) override
	{
		if (!m || !m->ActiveCmd)
			return;
		const bool profile = IsNriRecordProfileEnabled();
		NriCpuProfileScope applyScope(profile, m->RecordProfile.RtApplyMs, m->RecordProfile.RtApplyCount);
		EnsureBuilt(); // lazily build the pipeline now that all bindings are declared
		if (!Pipeline || !Layout || !SbtBuffer)
			return;
		m->EndRP();

		for (Binding& b : Bindings)
		{
			if (b.bindless)
				continue; // filled from the global registry below
			if (b.descriptorNum > 1)
				RefreshDescriptorArray(b);
			else
				RefreshDescriptor(b);
		}
		const uint32_t frameSlot = m->ActiveFrameContextIndex % std::max(1u, m->FrameRingCount());
		std::vector<nri::DescriptorSet*>* setsForFrame = frameSlot < Sets.size() ? &Sets[frameSlot] : nullptr;

		if (setsForFrame && !setsForFrame->empty())
		{
			static std::vector<nri::UpdateDescriptorRangeDesc> updateScratch;
			updateScratch.clear();

			// Bindless tables: write the backend's global registry descriptors
			// (material textures / geometry buffers) into the bindless ranges.
			// PARTIALLY_BOUND covers slots past the registered count.
			for (Binding& b : Bindings)
			{
				if (!b.bindless || b.rangeIndex == UINT32_MAX || b.setIndex >= setsForFrame->size() || !(*setsForFrame)[b.setIndex])
					continue;
				std::vector<nri::Descriptor*>& regDescs = (b.bindlessSrc == 2) ? m->BindlessBufDescs : m->BindlessTexDescs;
				uint32_t count = std::min<uint32_t>((uint32_t)regDescs.size(), b.descriptorNum);
				while (count > 0 && !regDescs[count - 1]) --count; // trailing freed slots
				if (count == 0)
					continue;
				nri::UpdateDescriptorRangeDesc upd = {};
				upd.descriptorSet = (*setsForFrame)[b.setIndex];
				upd.rangeIndex = b.rangeIndex;
				upd.baseDescriptor = 0;
				upd.descriptors = regDescs.data();
				upd.descriptorNum = count;
				updateScratch.push_back(upd);
			}
			for (Binding& b : Bindings)
			{
				if (b.bindless)
					continue;
				if (b.rangeIndex == UINT32_MAX || b.setIndex >= setsForFrame->size() || !(*setsForFrame)[b.setIndex])
					continue;
				nri::UpdateDescriptorRangeDesc upd = {};
				upd.descriptorSet = (*setsForFrame)[b.setIndex];
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
				updateScratch.push_back(upd);
			}
			if (!updateScratch.empty())
			{
				++m->StatUDR;
				const auto updateStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
				m->Core.UpdateDescriptorRanges(updateScratch.data(), static_cast<uint32_t>(updateScratch.size()));
				NriCpuProfileAdd(profile, m->RecordProfile.RtUpdateDescriptorRangesMs, m->RecordProfile.RtUpdateDescriptorRangesCount, updateStart);
			}
			m->Core.CmdSetDescriptorPool(*m->ActiveCmd, *Pool);
		}
		m->Core.CmdSetPipelineLayout(*m->ActiveCmd, nri::BindPoint::RAY_TRACING, *Layout);
		const uint32_t setCount = setsForFrame ? static_cast<uint32_t>(setsForFrame->size()) : 0u;
		for (uint32_t setIndex = 0; setIndex < setCount; ++setIndex)
		{
			if (!(*setsForFrame)[setIndex])
				continue;
			nri::SetDescriptorSetDesc sd = {};
			sd.setIndex = setIndex;
			sd.descriptorSet = (*setsForFrame)[setIndex];
			sd.bindPoint = nri::BindPoint::RAY_TRACING;
			const auto setStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
			m->Core.CmdSetDescriptorSet(*m->ActiveCmd, sd);
			NriCpuProfileAdd(profile, m->RecordProfile.RtSetDescriptorSetMs, m->RecordProfile.RtSetDescriptorSetCount, setStart);
		}
		m->Core.CmdSetPipeline(*m->ActiveCmd, *Pipeline);
		if (width == 0 || height == 0)
			return;

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
		const auto dispatchStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
		m->RT.CmdDispatchRays(*m->ActiveCmd, dispatch);
		NriCpuProfileAdd(profile, m->RecordProfile.RtDispatchRaysMs, m->RecordProfile.RtDispatchRaysCount, dispatchStart);
	}

private:
	enum class RegClass { SRV, UAV, CBV, Sampler };
	enum class ResKind { None, TexSRV, TexUAV, BufSRV, BufUAV, CBV, Sampler, Accel, VertexSRV, IndexSRV };
	struct ShaderEntry { std::string name; RTPipelineStateObject::ShaderType type = RTPipelineStateObject::GLOBAL; std::vector<uint8_t> dxil; };
	struct HitGroupEntry { std::string name; std::string chs; std::string ahs; };
	struct CbvBuf { nri::Buffer* buffer = nullptr; std::vector<nri::Memory*> memory; nri::Descriptor* view = nullptr; void* mapped = nullptr; };
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
		ResKind lastKind = ResKind::None;
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
		// Bindless global table (homecoming RHI): filled from the backend registry
		// (m->BindlessTexDescs / BindlessBufDescs) at Apply, not per-instance.
		bool bindless = false;
		int bindlessSrc = 0; // 1 = texture registry (space10), 2 = buffer registry (space12)
	};
	static constexpr uint32_t kRtBindlessCapacity = 4096;

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
		// Bindless runtime arrays (MaterialTextures space10, GeometryBuffers space12,
		// and any typed RuntimeArray SRV) declare their full capacity in descriptorNum
		// — must be honored so the descriptor range is sized for the registry, not 1.
		// Without this the range is built with descriptorNum=1 and UpdateDescriptorRanges
		// rejects the bindless writes (validation: count > range size).
		if (b.bindless || b.descriptorNum > 1)
			return b.descriptorNum;
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
			const uint32_t frameSlots = NRIBackend::Impl::kQueuedFrameNum;
			nri::DescriptorPoolDesc pd = {};
			pd.descriptorSetMaxNum = static_cast<uint32_t>(sets.size()) * frameSlots;
			pd.textureMaxNum = textureNum * frameSlots;
			pd.storageTextureMaxNum = storageTextureNum * frameSlots;
			pd.structuredBufferMaxNum = structuredBufferNum * frameSlots;
			pd.storageStructuredBufferMaxNum = storageStructuredBufferNum * frameSlots;
			pd.constantBufferMaxNum = cbvNum * frameSlots;
			pd.samplerMaxNum = samplerNum * frameSlots;
			pd.accelerationStructureMaxNum = accelNum * frameSlots;
			if (m->Core.CreateDescriptorPool(*m->Device, pd, Pool) != nri::Result::SUCCESS || !Pool)
				return false;
			Sets.assign(frameSlots, std::vector<nri::DescriptorSet*>(sets.size(), nullptr));
			for (uint32_t frameSlot = 0; frameSlot < frameSlots; ++frameSlot)
			{
				for (uint32_t setIndex = 0; setIndex < sets.size(); ++setIndex)
				{
					if (m->Core.AllocateDescriptorSets(*Pool, *Layout, setIndex, &Sets[frameSlot][setIndex], 1, 0) != nri::Result::SUCCESS || !Sets[frameSlot][setIndex])
						return false;
				}
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
		if (idSize == 0)
			return false;

		if (!m->CreateBoundBuffer(
			sbtSize,
			0,
			nri::BufferUsageBits::SHADER_BINDING_TABLE,
			nri::MemoryLocation::DEVICE,
			SbtBuffer,
			SbtMemory))
		{
			return false;
		}
		if (!m->CreateBoundBuffer(
			sbtSize,
			0,
			nri::BufferUsageBits::NONE,
			nri::MemoryLocation::HOST_UPLOAD,
			SbtUploadBuffer,
			SbtUploadMemory))
		{
			m->FreeBuffer(SbtBuffer, SbtMemory);
			return false;
		}

		uint8_t* mapped = static_cast<uint8_t*>(m->Core.MapBuffer(*SbtUploadBuffer, 0, sbtSize));
		if (!mapped)
		{
			m->FreeBuffer(SbtUploadBuffer, SbtUploadMemory);
			m->FreeBuffer(SbtBuffer, SbtMemory);
			return false;
		}
		memset(mapped, 0, static_cast<size_t>(sbtSize));
		// WriteShaderGroupIdentifiers packs identifiers at shaderGroupIdentifierSize
		// stride, but the shader binding table addresses records at SbtEntrySize
		// stride (>= idSize, aligned). Writing a multi-group run in one call would
		// place the 2nd+ identifiers at idSize spacing — wrong for any region with
		// more than one group (e.g. GI's two miss shaders). Write each group at its
		// own strided record offset instead.
		auto writeGroups = [&](uint32_t groupIndex, uint32_t groupNum, uint64_t offset)
		{
			if (groupNum == 0)
				return true;
			if (groupIndex >= groupCount || groupIndex + groupNum > groupCount)
				return false;
			for (uint32_t i = 0; i < groupNum; ++i)
			{
				if (m->RT.WriteShaderGroupIdentifiers(*Pipeline, groupIndex + i, 1, mapped + offset + static_cast<uint64_t>(i) * SbtEntrySize) != nri::Result::SUCCESS)
					return false;
			}
			return true;
		};
		bool ok = writeGroups(0, RaygenGroupCount, RaygenOffset);
		ok = writeGroups(RaygenGroupCount, MissGroupCount, MissOffset) && ok;
		if (HitGroupCount > 0)
			ok = writeGroups(HitGroupStart, HitGroupCount, HitOffset) && ok;
		m->Core.UnmapBuffer(*SbtUploadBuffer);
		if (!ok)
		{
			m->FreeBuffer(SbtUploadBuffer, SbtUploadMemory);
			m->FreeBuffer(SbtBuffer, SbtMemory);
			return false;
		}

		auto recordCopy = [&](nri::CommandBuffer& cmd)
		{
			const nri::AccessStage noneAccess{ nri::AccessBits::NONE, nri::StageBits::ALL };
			const nri::AccessStage copySource{ nri::AccessBits::COPY_SOURCE, nri::StageBits::COPY };
			const nri::AccessStage copyDest{ nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY };
			const nri::AccessStage sbtRead{ nri::AccessBits::SHADER_BINDING_TABLE, nri::StageBits::RAY_TRACING_SHADERS };

			nri::BufferBarrierDesc toCopy[2] = {};
			toCopy[0].buffer = SbtUploadBuffer;
			toCopy[0].before = noneAccess;
			toCopy[0].after = copySource;
			toCopy[1].buffer = SbtBuffer;
			toCopy[1].before = noneAccess;
			toCopy[1].after = copyDest;
			nri::BarrierDesc copyBarrier = {};
			copyBarrier.buffers = toCopy;
			copyBarrier.bufferNum = 2;
			m->Core.CmdBarrier(cmd, copyBarrier);

			m->Core.CmdCopyBuffer(cmd, *SbtBuffer, 0, *SbtUploadBuffer, 0, sbtSize);

			nri::BufferBarrierDesc toSbt = {};
			toSbt.buffer = SbtBuffer;
			toSbt.before = copyDest;
			toSbt.after = sbtRead;
			nri::BarrierDesc sbtBarrier = {};
			sbtBarrier.buffers = &toSbt;
			sbtBarrier.bufferNum = 1;
			m->Core.CmdBarrier(cmd, sbtBarrier);
		};

		if (m->ActiveCmd)
		{
			m->EndRP();
			recordCopy(*m->ActiveCmd);
			return true;
		}

		if (!m->SubmitImmediate("[NRIRTPSO] SBT upload immediate submit failed\n", recordCopy, true))
		{
			m->FreeBuffer(SbtUploadBuffer, SbtUploadMemory);
			m->FreeBuffer(SbtBuffer, SbtMemory);
			return false;
		}
		return true;
	}

	void ResetOwnedDescriptor(Binding& b)
	{
		if (b.ownsDesc && b.desc)
			m->Core.DestroyDescriptor(b.desc);
		b.desc = nullptr;
		b.ownsDesc = false;
		b.last = nullptr;
		b.lastKind = ResKind::None;
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
			Texture* texture = nullptr;
			Buffer* buffer = nullptr;
			nri::Texture* nativeTexture = nullptr;
			nri::Buffer* nativeBuffer = nullptr;
			void* current = nullptr;

			if (b.kind == ResKind::TexSRV || b.kind == ResKind::TexUAV)
			{
				texture = (i < b.texArray.size() && b.texArray[i]) ? b.texArray[i] : fallbackTexture;
				auto it = m->Textures.find(texture);
				if (it == m->Textures.end() || !it->second.texture)
					continue;
				nativeTexture = it->second.texture;
				current = nativeTexture;
			}
			else if (b.kind == ResKind::BufSRV || b.kind == ResKind::BufUAV)
			{
				buffer = (i < b.bufArray.size() && b.bufArray[i]) ? b.bufArray[i] : fallbackBuffer;
				auto it = m->Buffers.find(buffer);
				if (it == m->Buffers.end() || !it->second.buffer)
					continue;
				nativeBuffer = it->second.buffer;
				current = nativeBuffer;
			}
			if (!current || (b.lastArray[i] == current && b.descArray[i]))
				continue;

			ResetOwnedDescriptorArraySlot(b, i);
			b.lastArray[i] = current;

			if (texture)
			{
				const nri::TextureDesc& td = m->Core.GetTextureDesc(*nativeTexture);
				nri::TextureViewDesc tvd = {};
				tvd.texture = nativeTexture;
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
				nri::BufferViewDesc bvd = {};
				bvd.buffer = nativeBuffer;
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

		if (b.kind == ResKind::TexSRV || b.kind == ResKind::TexUAV)
		{
			auto it = m->Textures.find(b.tex);
			if (it == m->Textures.end() || !it->second.texture)
				return;
			void* current = it->second.texture;
			if (b.last == current && b.lastKind == b.kind && b.desc)
				return;
			ResetOwnedDescriptor(b);
			b.last = current;
			b.lastKind = b.kind;
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
			void* current = it->second.buffer;
			if (b.last == current && b.lastKind == b.kind && b.desc)
				return;
			ResetOwnedDescriptor(b);
			b.last = current;
			b.lastKind = b.kind;
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
			void* current = it->second.buffer;
			if (b.last == current && b.lastKind == b.kind && b.desc)
				return;
			ResetOwnedDescriptor(b);
			b.last = current;
			b.lastKind = b.kind;
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
			void* current = it->second.buffer;
			if (b.last == current && b.lastKind == b.kind && b.desc)
				return;
			ResetOwnedDescriptor(b);
			b.last = current;
			b.lastKind = b.kind;
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

	bool BackendAlive() const
	{
		const std::shared_ptr<std::atomic_bool> alive = Alive.lock();
		return m && alive && alive->load(std::memory_order_acquire) && m->Device;
	}

	NRIBackend::Impl* m = nullptr;
	std::weak_ptr<std::atomic_bool> Alive;
	std::vector<ShaderEntry> Shaders;
	std::vector<HitGroupEntry> HitGroups;
	std::vector<Binding> Bindings;
	std::vector<std::pair<std::string, std::string>> ShaderDefines;
	std::unordered_map<std::string, std::vector<CbvBuf>> CbvBuffers;
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
	std::vector<std::vector<nri::DescriptorSet*>> Sets;
	nri::Buffer* SbtBuffer = nullptr;
	std::vector<nri::Memory*> SbtMemory;
	nri::Buffer* SbtUploadBuffer = nullptr;
	std::vector<nri::Memory*> SbtUploadMemory;
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
		if (m)
			Alive = m->Alive;
		Build(desc);
	}
	~NRIGraphicsPipeline() override
	{
		if (!BackendAlive()) return;
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

	enum class Kind { TexSRV, BufSRV, VertexSRV, Sampler, CBV };
	struct Binding
	{
		std::string name;
		uint32_t reg = 0;
		uint32_t registerSpace = 0;
		Kind kind = Kind::TexSRV;
		nri::DescriptorType descriptorType = nri::DescriptorType::TEXTURE;
		RHIBufferViewKind bufferView = RHIBufferViewKind::Structured;
		nri::Descriptor* desc = nullptr;
		nri::Descriptor* descSingle[1] = {};
		bool ownsDesc = false;
		bool bindless = false;
		uint32_t bindlessSrc = 0; // 1 = texture registry, 2 = raw buffer registry
		uint32_t descriptorNum = 1;
		void* last = nullptr;
		Texture* tex = nullptr;
		Buffer* buf = nullptr;
		VertexBuffer* vb = nullptr;
		Sampler* samp = nullptr;
		uint32_t setIndex = UINT32_MAX;
		uint32_t rangeIndex = UINT32_MAX;
	};
	struct SetState
	{
		uint32_t registerSpace = 0;
		bool usesRing = false;
	};
	struct CbvState { nri::Buffer* buffer = nullptr; std::vector<nri::Memory*> memory; nri::Descriptor* view = nullptr; uint32_t size = 0; void* mapped = nullptr; };

	void SetTexture(const std::string& name, Texture* t) { if (Binding* b = Find(name)) { b->tex = t; } }
	void SetBuffer(const std::string& name, Buffer* bb) { if (Binding* b = Find(name)) { b->buf = bb; } }
	void SetVertexBuffer(const std::string& name, VertexBuffer* vb) { if (Binding* b = Find(name)) { b->kind = Kind::VertexSRV; b->vb = vb; b->buf = nullptr; } }
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
	bool ApplyForDraw()
	{
		if (!Pipeline || !m->ActiveCmd)
			return false;
		const bool profile = IsNriRecordProfileEnabled();
		NriCpuProfileScope applyScope(profile, m->RecordProfile.ApplyForDrawMs, m->RecordProfile.ApplyForDrawCount);
		if (!HasResources || !Pool)
			return true;

		// Reset the ring at the start of each frame (EndFrame waits on the GPU,
		// so previous-frame ring entries are safe to overwrite).
		if (RingFrame != m->SwapFrameIndex) { RingFrame = m->SwapFrameIndex; RingIdx = 0; }
		// Each draw needs its OWN descriptor set + CB slot for the frame; the GPU
		// consumes them in order and EndFrame waits before the ring resets. The old
		// code CLAMPED slot to kRing-1 once draws exceeded kRing, so every overflow
		// draw shared one descriptor set that later draws kept overwriting WHILE the
		// GPU was still executing earlier ones — descriptor aliasing that made a draw
		// read a wrong/out-of-range resource and HANG the GPU (DEVICE_HUNG), which is
		// the wide-view (>1024 draws) camera-move freeze. Use a unique slot per draw;
		// on genuine overflow skip the draw (visible gap) instead of aliasing.
		const uint32_t ringSlotsPerFrame = std::max(1u, RingSlotsPerFrame);
		const uint32_t drawSlot = RingIdx++;
		if (drawSlot >= ringSlotsPerFrame)
		{
			static bool s_ringWarned = false;
			if (!s_ringWarned) { s_ringWarned = true; AppendCpuRuntimeTrace(L"[NRI] per-draw descriptor ring overflow (>kRing draws/frame) — raise kRing"); }
			return false;
		}

		const uint32_t frameSlot = m->ActiveFrameContextIndex % std::max(1u, m->FrameRingCount());
		const uint32_t slot = frameSlot * ringSlotsPerFrame + drawSlot;

		// Lazily allocate this ring slot's dynamic descriptor sets + CB buffer/view.
		// Bindless-only spaces use a single static set; only spaces carrying per-draw
		// CB/local SRVs need a ring to avoid D3D12 descriptor aliasing.
		if (SetRing.size() <= slot) { SetRing.resize(slot + 1); CbRing.resize(slot + 1); }
		if (SetRing[slot].size() < SetStates.size())
			SetRing[slot].resize(SetStates.size(), nullptr);
		for (uint32_t setIndex = 0; setIndex < SetStates.size(); ++setIndex)
		{
			if (!SetStates[setIndex].usesRing)
				continue;
			if (!SetRing[slot][setIndex])
				m->Core.AllocateDescriptorSets(*Pool, *Layout, setIndex, &SetRing[slot][setIndex], 1, 0);
			if (!SetRing[slot][setIndex])
				return false;
		}

		auto descriptorSetFor = [&](const Binding& b) -> nri::DescriptorSet*
		{
			if (b.setIndex >= SetStates.size())
				return nullptr;
			if (SetStates[b.setIndex].usesRing)
				return SetRing[slot][b.setIndex];
			return (frameSlot < StaticSets.size() && b.setIndex < StaticSets[frameSlot].size()) ? StaticSets[frameSlot][b.setIndex] : nullptr;
		};

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
					// Map once for the slot's lifetime (HOST_UPLOAD). Per-draw Map/Unmap
					// was a driver call pair on every single draw — at a wide view that's
					// thousands/frame. Keep it mapped and just memcpy below.
					cb.mapped = m->Core.MapBuffer(*cb.buffer, 0, cb.size);
				}
			}
			if (!cb.view || !cb.mapped)
				return false;
			if (cb.mapped && PendingCBSize > 0)
				memcpy(cb.mapped, PendingCB.data(), PendingCBSize);
			cbView = cb.view;
		}

		// Write every binding's current descriptor into this ring set in a SINGLE
		// UpdateDescriptorRanges call (one range per binding) rather than one driver
		// call per binding. Per-draw descriptor updates are the dominant NRI CPU
		// recording cost (it scales with draw count); batching cuts the call count
		// from ~5/draw to 1/draw. Scratch is static (single render thread) so no
		// per-draw allocation. NOTE: descriptors must stay alive until the call, so
		// each binding keeps its own descSingle slot (referenced by the desc array).
		static std::vector<nri::UpdateDescriptorRangeDesc> updScratch;
		updScratch.clear();
		for (Binding& b : Bindings)
		{
			if (b.rangeIndex == UINT32_MAX)
				continue;
			nri::DescriptorSet* set = descriptorSetFor(b);
			if (!set)
				continue;
			b.descSingle[0] = b.desc;
			nri::UpdateDescriptorRangeDesc upd = {};
			upd.descriptorSet = set; upd.rangeIndex = b.rangeIndex; upd.baseDescriptor = 0;
			if (b.bindless)
			{
				std::vector<nri::Descriptor*>& regDescs = (b.bindlessSrc == 2) ? m->BindlessBufDescs : m->BindlessTexDescs;
				uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(regDescs.size()), b.descriptorNum);
				while (count > 0 && !regDescs[count - 1])
					--count;
				if (count == 0)
					continue;
				upd.descriptors = regDescs.data();
				upd.descriptorNum = count;
			}
			else
			{
				if (b.kind == Kind::CBV) { b.desc = cbView; b.ownsDesc = false; }
				else
				{
					const auto refreshStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
					RefreshDescriptor(b);
					NriCpuProfileAdd(profile, m->RecordProfile.RefreshDescriptorMs, m->RecordProfile.RefreshDescriptorCount, refreshStart);
				}
				if (!b.desc)
					continue;
				b.descSingle[0] = b.desc;
				upd.descriptors = b.descSingle;
				upd.descriptorNum = 1;
			}
			updScratch.push_back(upd);
		}
		if (!updScratch.empty())
		{
			++m->StatUDR;
			const auto updateStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
			m->Core.UpdateDescriptorRanges(updScratch.data(), static_cast<uint32_t>(updScratch.size()));
			NriCpuProfileAdd(profile, m->RecordProfile.UpdateDescriptorRangesMs, m->RecordProfile.UpdateDescriptorRangesCount, updateStart);
		}

		for (uint32_t setIndex = 0; setIndex < SetStates.size(); ++setIndex)
		{
			nri::DescriptorSet* set = SetStates[setIndex].usesRing ?
				SetRing[slot][setIndex] :
				((frameSlot < StaticSets.size() && setIndex < StaticSets[frameSlot].size()) ? StaticSets[frameSlot][setIndex] : nullptr);
			if (!set)
				continue;
			nri::SetDescriptorSetDesc sd = {};
			sd.setIndex = setIndex;
			sd.descriptorSet = set;
			sd.bindPoint = nri::BindPoint::GRAPHICS;
			const auto setStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
			m->Core.CmdSetDescriptorSet(*m->ActiveCmd, sd);
			NriCpuProfileAdd(profile, m->RecordProfile.SetDescriptorSetMs, m->RecordProfile.SetDescriptorSetCount, setStart);
		}
		return true;
	}

	// Sanity-check the per-draw transform the GPU will consume. The GBuffer/shadow CB
	// layout begins with ViewProjectionMatrix (offset 0) and has WorldMatrix (offset
	// 128); a non-finite or astronomically large component yields a degenerate/giant
	// triangle that overruns the rasterizer and trips the 2 s TDR (DEVICE_HUNG draw
	// timeout). Returns false → the draw should be skipped. Outputs the worst value
	// for logging. Only meaningful for CBs >= 192 B (transform CBs); others pass.
	bool DebugTransformSane(float& worstOut) const
	{
		worstOut = 0.0f;
		if (PendingCBSize < 192) return true;
		auto chk = [&](uint32_t off) -> bool {
			const float* mtx = reinterpret_cast<const float*>(PendingCB.data() + off);
			bool ok = true;
			for (int i = 0; i < 16; ++i) { float v = mtx[i]; float a = std::fabs(v); if (a > worstOut) worstOut = a; if (!std::isfinite(v) || a > 1e7f) ok = false; }
			return ok;
		};
		bool a = chk(0), b = chk(128);
		return a && b;
	}

private:
	Binding* Find(const std::string& n) { for (Binding& b : Bindings) if (b.name == n) return &b; return nullptr; }

	void RefreshDescriptor(Binding& b)
	{
		if (b.kind == Kind::CBV) { b.desc = Cbv.view; b.ownsDesc = false; return; }
		if (b.kind == Kind::Sampler) { if (b.samp) { auto it = m->Samplers.find(b.samp); b.desc = it != m->Samplers.end() ? it->second : nullptr; } return; }
		if (b.kind == Kind::TexSRV && b.tex)
		{
			auto it = m->Textures.find(b.tex);
			if (it == m->Textures.end() || !it->second.texture) return;
			void* res = it->second.texture;
			if (res == b.last && b.desc) return;
			if (b.ownsDesc && b.desc) { m->Core.DestroyDescriptor(b.desc); b.desc = nullptr; }
			b.last = res;
			const nri::TextureDesc& td = m->Core.GetTextureDesc(*it->second.texture);
			nri::TextureViewDesc tvd = {}; tvd.texture = it->second.texture; tvd.type = nri::TextureView::TEXTURE; tvd.format = td.format; tvd.mipNum = nri::REMAINING; tvd.layerNum = nri::REMAINING;
			nri::Descriptor* d = nullptr; if (m->Core.CreateTextureView(tvd, d) == nri::Result::SUCCESS) { b.desc = d; b.ownsDesc = true; }
		}
		else if (b.kind == Kind::BufSRV && b.buf)
		{
			auto it = m->Buffers.find(b.buf);
			if (it == m->Buffers.end() || !it->second.buffer) return;
			void* res = it->second.buffer;
			if (res == b.last && b.desc) return;
			if (b.ownsDesc && b.desc) { m->Core.DestroyDescriptor(b.desc); b.desc = nullptr; }
			b.last = res;
			nri::BufferViewDesc bvd = {};
			bvd.buffer = it->second.buffer;
			bvd.type = (b.bufferView == RHIBufferViewKind::Raw) ? nri::BufferView::BYTE_ADDRESS_BUFFER : nri::BufferView::STRUCTURED_BUFFER;
			bvd.offset = 0;
			bvd.size = static_cast<uint64_t>(b.buf->NumElements) * b.buf->ElementSize;
			bvd.structureStride = (b.bufferView == RHIBufferViewKind::Raw) ? 0 : (b.buf->ElementSize ? b.buf->ElementSize : 4);
			nri::Descriptor* d = nullptr; if (m->Core.CreateBufferView(bvd, d) == nri::Result::SUCCESS) { b.desc = d; b.ownsDesc = true; }
		}
		else if (b.kind == Kind::VertexSRV && b.vb)
		{
			auto it = m->VBs.find(b.vb);
			if (it == m->VBs.end() || !it->second.buffer || it->second.capacity == 0) return;
			void* res = it->second.buffer;
			if (res == b.last && b.desc) return;
			if (b.ownsDesc && b.desc) { m->Core.DestroyDescriptor(b.desc); b.desc = nullptr; }
			b.last = res;
			nri::BufferViewDesc bvd = {};
			bvd.buffer = it->second.buffer;
			bvd.type = nri::BufferView::BYTE_ADDRESS_BUFFER;
			bvd.offset = 0;
			bvd.size = it->second.capacity;
			nri::Descriptor* d = nullptr; if (m->Core.CreateBufferView(bvd, d) == nri::Result::SUCCESS) { b.desc = d; b.ownsDesc = true; }
		}
	}

	void Build(const GraphicsPipelineDesc& desc)
	{
		auto plog = [&](const char* what) {
			std::ofstream l("nri_pso.log", std::ios::app);
			if (l) { const std::string sp = NarrowAsciiForLog(desc.ShaderPath);
				l << "GFXPSO FAIL [" << what << "] shader=" << sp << " vs=" << desc.VertexEntryPoint << " ps=" << desc.PixelEntryPoint << "\n"; }
		};
		if (!m->Device) { plog("no device"); return; }
		std::string verr, perr;
		const std::wstring vsW(desc.VertexEntryPoint.begin(), desc.VertexEntryPoint.end());
		const std::wstring psW(desc.PixelEntryPoint.begin(), desc.PixelEntryPoint.end());
		std::ifstream f(desc.ShaderPath, std::ios::binary);
		if (!f.good()) { plog("shader file not found"); return; }
		std::stringstream ss; ss << f.rdbuf(); const std::string src = ss.str();
		const wchar_t* vsTarget = (desc.VertexEntryPoint == "VSMainBindlessIndirect") ? L"vs_6_8" : L"vs_6_5";
		VsDxil = CompileHLSLToDXIL(src.data(), src.size(), desc.ShaderPath.c_str(), vsW.c_str(), vsTarget, verr);
		PsDxil = CompileHLSLToDXIL(src.data(), src.size(), desc.ShaderPath.c_str(), psW.c_str(), L"ps_6_5", perr);
		if (VsDxil.empty() || PsDxil.empty()) {
			std::ofstream l("nri_pso.log", std::ios::app);
			if (l) { const std::string sp = NarrowAsciiForLog(desc.ShaderPath);
				l << "GFXPSO FAIL [shader compile] shader=" << sp << " vsEmpty=" << VsDxil.empty() << " psEmpty=" << PsDxil.empty()
				  << " vsErr=" << verr << " psErr=" << perr << "\n"; }
			return;
		}
		VsEntry = desc.VertexEntryPoint; PsEntry = desc.PixelEntryPoint;

		// Build descriptor ranges from the typed pipeline-layout schema (bindless
		// RHI contract). NRI maps HLSL register spaces to descriptor sets, so
		// GBuffer's space10 MaterialTextures[] and space12 GeometryBuffers[] must
		// become separate sets from the per-draw space0 CB/SRV/sampler bindings.
		struct SetBuild
		{
			uint32_t registerSpace = 0;
			std::vector<nri::DescriptorRangeDesc> ranges;
			bool hasDynamicBinding = false;
		};
		std::vector<SetBuild> setBuilds;
		const nri::StageBits gfxStages = nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;
		auto findOrAddSet = [&](uint32_t registerSpace) -> uint32_t
		{
			for (uint32_t i = 0; i < setBuilds.size(); ++i)
			{
				if (setBuilds[i].registerSpace == registerSpace)
					return i;
			}
			SetBuild setBuild;
			setBuild.registerSpace = registerSpace;
			setBuild.ranges.reserve(desc.PipelineLayout.Bindings.size());
			setBuilds.push_back(std::move(setBuild));
			return static_cast<uint32_t>(setBuilds.size() - 1);
		};

		for (const RHIBindingDesc& bd : desc.PipelineLayout.Bindings)
		{
			Kind k = Kind::TexSRV; nri::DescriptorType dt = nri::DescriptorType::TEXTURE;
			switch (bd.DescriptorKind)
			{
			case RHIDescriptorKind::CBV:     k = Kind::CBV;     dt = nri::DescriptorType::CONSTANT_BUFFER; break;
			case RHIDescriptorKind::Sampler: k = Kind::Sampler; dt = nri::DescriptorType::SAMPLER; break;
			case RHIDescriptorKind::UAV:
				if (bd.ResourceKind == RHIResourceKind::Buffer) { k = Kind::BufSRV; dt = nri::DescriptorType::STORAGE_STRUCTURED_BUFFER; }
				else { k = Kind::TexSRV; dt = nri::DescriptorType::STORAGE_TEXTURE; }
				break;
			case RHIDescriptorKind::SRV:
			default:
				if (bd.ResourceKind == RHIResourceKind::Buffer) { k = Kind::BufSRV; dt = nri::DescriptorType::STRUCTURED_BUFFER; }
				else { k = Kind::TexSRV; dt = nri::DescriptorType::TEXTURE; }
				break;
			}
			const bool isBindless = bd.Bindless || bd.RuntimeArray || bd.DescriptorCount == RHI_BINDLESS_ARRAY;
			const uint32_t descriptorCount = isBindless ? 4096u : (bd.DescriptorCount ? bd.DescriptorCount : 1u);
			const uint32_t setIndex = findOrAddSet(bd.RegisterSpace);
			setBuilds[setIndex].hasDynamicBinding |= !isBindless;

			Binding b;
			b.name = bd.Name;
			b.reg = bd.RegisterIndex;
			b.registerSpace = bd.RegisterSpace;
			b.kind = k;
			b.descriptorType = dt;
			b.bufferView = bd.BufferView;
			b.bindless = isBindless;
			b.bindlessSrc = (bd.ResourceKind == RHIResourceKind::Buffer) ? 2u : 1u;
			b.descriptorNum = descriptorCount;
			b.setIndex = setIndex;
			b.rangeIndex = static_cast<uint32_t>(setBuilds[setIndex].ranges.size());
			Bindings.push_back(b);

			nri::DescriptorRangeDesc r = {};
			r.baseRegisterIndex = bd.RegisterIndex;
			r.descriptorNum = descriptorCount;
			r.descriptorType = dt;
			r.shaderStages = ToNRIGraphicsStageBits(bd.Stages);
			if (bd.PartiallyBound || isBindless)
				r.flags = r.flags | nri::DescriptorRangeBits::PARTIALLY_BOUND;
			if (descriptorCount > 1)
				r.flags = r.flags | nri::DescriptorRangeBits::ARRAY;
			setBuilds[setIndex].ranges.push_back(r);
			if (bd.DescriptorKind == RHIDescriptorKind::CBV && !Cbv.buffer)
			{
				const uint32_t aligned = ((bd.SizeInBytes ? bd.SizeInBytes : 256u) + 255u) & ~255u;
				if (m->CreateBoundBuffer(aligned, 0, nri::BufferUsageBits::CONSTANT_BUFFER, nri::MemoryLocation::HOST_UPLOAD, Cbv.buffer, Cbv.memory))
				{
					Cbv.size = aligned;
					nri::BufferViewDesc bvd = {}; bvd.buffer = Cbv.buffer; bvd.type = nri::BufferView::CONSTANT_BUFFER; bvd.offset = 0; bvd.size = aligned;
					m->Core.CreateBufferView(bvd, Cbv.view);
					if (Binding* cb = Find(bd.Name)) cb->desc = Cbv.view;
				}
			}
		}

		std::vector<nri::DescriptorSetDesc> sets;
		sets.reserve(setBuilds.size());
		SetStates.clear();
		SetStates.reserve(setBuilds.size());
		for (const SetBuild& setBuild : setBuilds)
		{
			nri::DescriptorSetDesc set = {};
			set.registerSpace = setBuild.registerSpace;
			set.ranges = setBuild.ranges.data();
			set.rangeNum = static_cast<uint32_t>(setBuild.ranges.size());
			sets.push_back(set);

			SetState state;
			state.registerSpace = setBuild.registerSpace;
			state.usesRing = setBuild.hasDynamicBinding;
			SetStates.push_back(state);
		}

		nri::PipelineLayoutDesc pld = {}; pld.rootRegisterSpace = 0; pld.descriptorSets = sets.empty() ? nullptr : sets.data(); pld.descriptorSetNum = (uint32_t)sets.size(); pld.shaderStages = gfxStages;
		if (m->Core.CreatePipelineLayout(*m->Device, pld, Layout) != nri::Result::SUCCESS) { plog("CreatePipelineLayout"); return; }

		if (!sets.empty())
		{
			const uint32_t frameSlots = NRIBackend::Impl::kQueuedFrameNum;
			bool dynamicSamplerInRing = false;
			for (const Binding& b : Bindings)
			{
				if (b.descriptorType == nri::DescriptorType::SAMPLER &&
					b.setIndex < SetStates.size() &&
					SetStates[b.setIndex].usesRing)
				{
					dynamicSamplerInRing = true;
					break;
				}
			}
			RingSlotsPerFrame = dynamicSamplerInRing ? std::max(1u, kRing / frameSlots) : kRing;
			uint32_t descriptorSetMaxNum = 0;
			uint32_t texN = 0, bufN = 0, cbvN = 0, sampN = 0, storTexN = 0, storBufN = 0;
			for (uint32_t setIndex = 0; setIndex < SetStates.size(); ++setIndex)
				descriptorSetMaxNum += (SetStates[setIndex].usesRing ? RingSlotsPerFrame : 1u) * frameSlots;
			for (const Binding& b : Bindings)
			{
				const uint32_t multiplier = ((b.setIndex < SetStates.size() && SetStates[b.setIndex].usesRing) ? RingSlotsPerFrame : 1u) * frameSlots;
				const uint32_t count = b.descriptorNum * multiplier;
				switch (b.descriptorType)
				{
				case nri::DescriptorType::TEXTURE: ++texN; texN += count - 1; break;
				case nri::DescriptorType::STORAGE_TEXTURE: ++storTexN; storTexN += count - 1; break;
				case nri::DescriptorType::STRUCTURED_BUFFER: ++bufN; bufN += count - 1; break;
				case nri::DescriptorType::STORAGE_STRUCTURED_BUFFER: ++storBufN; storBufN += count - 1; break;
				case nri::DescriptorType::CONSTANT_BUFFER: ++cbvN; cbvN += count - 1; break;
				case nri::DescriptorType::SAMPLER: ++sampN; sampN += count - 1; break;
				default: break;
				}
			}

			HasResources = true;
			nri::DescriptorPoolDesc pd = {}; pd.descriptorSetMaxNum = descriptorSetMaxNum;
			pd.textureMaxNum = texN;
			pd.storageTextureMaxNum = storTexN;
			pd.structuredBufferMaxNum = bufN;
			pd.storageStructuredBufferMaxNum = storBufN;
			pd.constantBufferMaxNum = cbvN;
			pd.samplerMaxNum = std::min(sampN, 2048u); // D3D12 shader-visible sampler heap cap
			if (m->Core.CreateDescriptorPool(*m->Device, pd, Pool) != nri::Result::SUCCESS) { plog("CreateDescriptorPool"); return; }

			StaticSets.assign(frameSlots, std::vector<nri::DescriptorSet*>(SetStates.size(), nullptr));
			for (uint32_t frameSlot = 0; frameSlot < frameSlots; ++frameSlot)
			{
				for (uint32_t setIndex = 0; setIndex < SetStates.size(); ++setIndex)
				{
					if (SetStates[setIndex].usesRing)
						continue;
					if (m->Core.AllocateDescriptorSets(*Pool, *Layout, setIndex, &StaticSets[frameSlot][setIndex], 1, 0) != nri::Result::SUCCESS)
					{
						plog("AllocateStaticDescriptorSet");
						return;
					}
				}
			}
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
		gpd.cache = m->PipelineCache;
		nri::Result gr = m->Core.CreateGraphicsPipeline(*m->Device, gpd, Pipeline);
		if (gr != nri::Result::SUCCESS || !Pipeline) {
			std::ofstream l("nri_pso.log", std::ios::app);
			if (l) { const std::string sp = NarrowAsciiForLog(desc.ShaderPath);
				l << "GFXPSO FAIL [CreateGraphicsPipeline] result=" << (int)gr << " shader=" << sp
				  << " colorNum=" << om.colorNum << " hasDepth=" << desc.DepthFormat.has_value()
				  << " attrs=" << attrs.size() << "\n"; }
		}
	}

	bool BackendAlive() const
	{
		const std::shared_ptr<std::atomic_bool> alive = Alive.lock();
		return m && alive && alive->load(std::memory_order_acquire) && m->Device;
	}

	NRIBackend::Impl* m = nullptr;
	std::weak_ptr<std::atomic_bool> Alive;
	std::vector<Binding> Bindings;
	CbvState Cbv;                       // size template for the per-draw CB ring
	std::vector<uint8_t> VsDxil, PsDxil;
	std::string VsEntry, PsEntry;
	nri::PipelineLayout* Layout = nullptr;
	nri::Pipeline* Pipeline = nullptr;
	nri::DescriptorPool* Pool = nullptr;
	std::vector<SetState> SetStates;
	std::vector<std::vector<nri::DescriptorSet*>> StaticSets;
	bool HasResources = false;
	// Per-draw ring: one descriptor set + one CB slot per draw, reused each frame.
	// Must exceed the max draws in a single frame (wide views hit ~1.5k); overflow
	// now skips the draw rather than aliasing (see ApplyForDraw). Capped at 2048
	// because the per-draw set holds a sampler and the D3D12 shader-visible sampler
	// heap maxes at 2048 (samplerMaxNum = sampN*kRing). Larger needs samplers split
	// out of the ring into a static set.
	static constexpr uint32_t kRing = 2048;
	std::vector<std::vector<nri::DescriptorSet*>> SetRing;
	std::vector<CbvState> CbRing;
	uint32_t RingSlotsPerFrame = kRing;
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
	// NRI's DeviceVal layer validates EVERY command (state tracking, per-descriptor
	// type checks + scratch allocation on UpdateDescriptorRanges, etc.). With the RT
	// bindless write loop pushing thousands of descriptors per dispatch it dominates
	// CPU recording (tens of x slower than DX12/Vulkan). Default OFF for performance;
	// opt in via CORONA_NRI_VALIDATION=1 when debugging binding/descriptor issues.
	desc.enableNRIValidation = std::getenv("CORONA_NRI_VALIDATION") != nullptr;
	desc.enableGraphicsAPIValidation = false;
	m->DrawLog = std::getenv("CORONA_NRI_DRAWLOG") != nullptr;
	desc.callbackInterface.MessageCallback = NRIMessageCallback;
	desc.callbackInterface.AbortExecution = NRIAbortCallback;

	// Enable D3D12 DRED (Device Removed Extended Data) BEFORE device creation so a
	// GPU fault / TDR (the camera-move freeze: Submit -> DXGI_ERROR_DEVICE_REMOVED)
	// can be diagnosed — auto-breadcrumbs identify the last GPU command executed and
	// page-fault output gives the offending VA + allocation. Must be set before the
	// device NRI creates. Gated on CORONA_NRI_DRED to avoid the (small) always-on cost.
	if (std::getenv("CORONA_NRI_DRED") != nullptr)
	{
		Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> dred;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred))) && dred)
		{
			dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			AppendCpuRuntimeTrace(L"[NRIDRED] DRED auto-breadcrumbs + page-fault enabled");
		}
	}

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
	m->InitializePipelineCache();

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
	m->ClearTextureUavFloat1Pso.reset();
	m->ClearTextureUavFloat2Pso.reset();
	m->ClearTextureUavFloat4Pso.reset();
	if (m->Alive)
		m->Alive->store(false, std::memory_order_release);
	if (m->Device)
	{
		for (std::weak_ptr<NRIRTAS>& weakAS : m->RayTracingAS)
		{
			if (std::shared_ptr<NRIRTAS> as = weakAS.lock())
				as->Destroy();
		}
		m->RayTracingAS.clear();
		m->DestroyFrameContexts();
		m->DestroyImmediateContexts();
		ShutdownGpuTimestampQueries();
		ShutdownOcclusionQueries();

		for (auto& kv : m->Buffers)
		{
			if (kv.second.buffer)
				m->Core.DestroyBuffer(kv.second.buffer);
			for (nri::Memory* mem : kv.second.memory)
				if (mem) m->Core.FreeMemory(mem);
		}
		m->Buffers.clear();
		m->FlushBufferReusePool(); // destroy recycled-but-idle buffers

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
		m->SaveAndDestroyPipelineCache();

		nri::nriDestroyDevice(m->Device);
		m->Device = nullptr;
		nri::nriReportLiveObjects();
	}
}

// === Capabilities / identity =============================================
ERenderBackendAPI NRIBackend::GetAPI() const { return ERenderBackendAPI::NRI; }
const char* NRIBackend::GetBackendName() const { return m->BackendName.c_str(); }
// Stage 6: GBuffer + direct lighting + RT shadows + RT diffuse GI + RT AO + RT
// reflections (all real, bindless). Stays < 7 so the renderer keeps the "simple"
// diffuse-GI path (RaytracedGI.hlsl) instead of the screen-probe / spatial-hash
// GI and the ray-traced sky lighting, which are not brought up on NRI yet.
uint32_t NRIBackend::GetMaxSupportedHybridStage() const { return 6u; }
// True now that the RT shadow path is being brought up: InitRaytracingData builds
// BLAS/TLAS and the RT shadow pipeline (deferred-built at first Apply with the
// bindless tables declared). GI still uses the compute fallback.
bool NRIBackend::SupportsRayTracing() const { return m->RayTracingTier >= 1 && m->HasRayTracing; }
bool NRIBackend::SupportsShaderExecutionReordering() const { return m->RayTracingTier >= 3; }

// The NRI backend implements the typed binding schema and a bindless registry
// (RegisterBindlessTexture/Buffer) backed by PARTIALLY_BOUND/ARRAY descriptor
// ranges (= D3D12 DESCRIPTORS_VOLATILE). Reporting these capabilities is what
// gates the engine's RT bindless material/geometry paths (UsesRTBindlessMaterials
// / UsesRTBindlessGeometry) — without it EnsureRTMaterialRecordBuffer bails and
// the RT shadow pass early-returns.
RenderBackendCapabilities NRIBackend::GetCapabilities() const
{
	RenderBackendCapabilities capabilities{};
	capabilities.SupportsTypedBindingSchema = true;
	capabilities.SupportsBindlessTextures = true;
	capabilities.SupportsBindlessBuffers = true;
	capabilities.SupportsRuntimeDescriptorArrays = true;
	capabilities.SupportsPartiallyBoundDescriptors = true;
	capabilities.SupportsGBufferOcclusionQueries =
		m->Core.CreateQueryPool &&
		m->Core.DestroyQueryPool &&
		m->Core.GetQuerySize &&
		m->Core.CmdResetQueries &&
		m->Core.CmdBeginQuery &&
		m->Core.CmdEndQuery &&
		m->Core.CmdCopyQueries;
	if (m->Device && m->Core.CmdDrawIndirect && m->Core.CmdDrawIndexedIndirect)
	{
		const nri::DeviceDesc& dd = m->Core.GetDeviceDesc(*m->Device);
		const bool supportsNativeDrawParameters =
			dd.shaderFeatures.drawParameters &&
			dd.shaderModel >= NriShaderModel(6, 8);
		capabilities.SupportsDrawIndexedIndirect = true;
		capabilities.SupportsDrawIndirect = true;
		capabilities.SupportsMultiDrawIndirect = dd.other.drawIndirectMaxNum > 1;
		capabilities.SupportsDrawIndirectFirstInstance = supportsNativeDrawParameters;
	}
	capabilities.MaxBindlessTextureCount = 4096;
	capabilities.MaxBindlessBufferCount = 4096;
	return capabilities;
}

// === Frame lifecycle / diagnostics =======================================
static void LogD3D12DeviceRemoved(ID3D12Device* dev, const wchar_t* where); // defined below

void NRIBackend::MarkDeviceLost(const char* where, int result)
{
	if (!m || m->DeviceLost)
		return;

	m->DeviceLost = true;
	const char* safeWhere = where ? where : "unknown";
	std::ostringstream message;
	message << "NRI device lost at " << safeWhere << " result=" << result;
	ErrorString = message.str();
	AppendCpuRuntimeTrace(L"[NRIDeviceLost] " + std::wstring(ErrorString.begin(), ErrorString.end()));

	if (m->Device)
	{
		auto* nativeDevice = reinterpret_cast<ID3D12Device*>(m->Core.GetDeviceNativeObject(m->Device));
		if (nativeDevice && nativeDevice->GetDeviceRemovedReason() != S_OK)
		{
			std::wstring wideWhere;
			for (const char* p = safeWhere; *p; ++p)
				wideWhere.push_back((*p >= 0 && *p <= 0x7f) ? static_cast<wchar_t>(*p) : L'?');
			LogD3D12DeviceRemoved(nativeDevice, wideWhere.c_str());
		}
	}
}

void NRIBackend::BeginFrame()
{
	if (!m->Device || m->ActiveCmd || m->DeviceLost)
		return;
	const bool profile = IsNriRecordProfileEnabled();
	NriCpuProfileScope frameScope(profile, m->RecordProfile.BeginFrameMs, m->RecordProfile.BeginFrameCount);
	// Frame-start GPU-fault check: if the device was removed (the camera-move
	// freeze), dump DRED once. More reliable than hooking a specific submit — once
	// removed, every later frame's GetDeviceRemovedReason reports it.
	{
		static bool s_dredDumped = false;
		static const bool s_dred = std::getenv("CORONA_NRI_DRED") != nullptr;
		if (s_dred && !s_dredDumped)
		{
			auto* dev = reinterpret_cast<ID3D12Device*>(m->Core.GetDeviceNativeObject(m->Device));
			if (dev && dev->GetDeviceRemovedReason() != S_OK)
			{
				LogD3D12DeviceRemoved(dev, L"BeginFrame check");
				s_dredDumped = true;
			}
		}
	}
	if (m->DrawLog)
	{
		std::ofstream l("nri_drawlog.log", std::ios::app);
		if (l) l << "=== FRAME " << m->SwapFrameIndex << " (draws=" << m->DbgDraws << " skipped=" << m->DbgDrawsSkipped << " oob=" << m->DbgOOBDraws << " badXform=" << m->DbgBadXform << ") ===" << std::endl;
	}
	m->FrameHasBackbuffer = false;
	m->HasPendingClear = false;
	m->BBLayout = nri::Layout::UNDEFINED;
	m->CurAcquire = nullptr;
	m->CurrentWindowRT = nullptr;

	nri::CommandAllocator* allocator = m->CmdAllocator;
	nri::CommandBuffer* commandBuffer = m->CmdBuffer;
	if (!m->FrameContexts.empty())
	{
		m->ActiveFrameContextIndex = static_cast<uint32_t>(m->SwapFrameIndex % m->FrameContexts.size());
		NRIBackend::Impl::FrameContext& frame = m->FrameContexts[m->ActiveFrameContextIndex];
		m->WaitForFrameContext(frame);
		m->FreeTransientBuffers(frame.transientBuffers);
		allocator = frame.allocator;
		commandBuffer = frame.commandBuffer;
	}
	else
	{
		m->FreeTransientBuffers();
	}

	if (!allocator || !commandBuffer)
		return;
	const auto resetBeginStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
	m->Core.ResetCommandAllocator(*allocator);
	const nri::Result beginResult = m->Core.BeginCommandBuffer(*commandBuffer, nullptr);
	NriCpuProfileAdd(profile, m->RecordProfile.ResetBeginCommandBufferMs, m->RecordProfile.ResetBeginCommandBufferCount, resetBeginStart);
	if (beginResult != nri::Result::SUCCESS)
	{
		MarkDeviceLost("BeginCommandBuffer", static_cast<int>(beginResult));
		return;
	}
	m->ActiveCmd = commandBuffer;

	if (m->SwapChain && !m->BackBuffers.empty())
	{
		const uint32_t n = (uint32_t)m->BackBuffers.size();
		nri::Fence* acq = m->AcquireSem[m->SwapFrameIndex % n];
		uint32_t idx = 0;
		const auto acquireStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
		const nri::Result acquireResult = m->SwapChainI.AcquireNextTexture(*m->SwapChain, *acq, idx);
		NriCpuProfileAdd(profile, m->RecordProfile.AcquireNextTextureMs, m->RecordProfile.AcquireNextTextureCount, acquireStart);
		if (acquireResult == nri::Result::SUCCESS && idx < n)
		{
			m->CurrentBackBuffer = idx;
			m->CurAcquire = acq;
			m->FrameHasBackbuffer = true;
		}
		else
		{
			m->Core.EndCommandBuffer(*m->ActiveCmd);
			m->ActiveCmd = nullptr;
			MarkDeviceLost("AcquireNextTexture", static_cast<int>(acquireResult));
		}
	}
}
// DRED dump on device removal: GetDeviceRemovedReason + the in-flight GPU command
// (auto-breadcrumb at the last completed value) + page-fault VA / recently-freed
// allocations. Distinguishes a HANG (dispatch timeout) from a page fault (bad/
// freed resource access). Enabled only when CORONA_NRI_DRED set at startup.
static void LogD3D12DeviceRemoved(ID3D12Device* dev, const wchar_t* where)
{
	if (!dev)
		return;
	const HRESULT reason = dev->GetDeviceRemovedReason();
	const wchar_t* name =
		reason == DXGI_ERROR_DEVICE_HUNG ? L"DEVICE_HUNG(dispatch/draw timeout)" :
		reason == DXGI_ERROR_DEVICE_REMOVED ? L"DEVICE_REMOVED" :
		reason == DXGI_ERROR_DEVICE_RESET ? L"DEVICE_RESET" :
		reason == DXGI_ERROR_DRIVER_INTERNAL_ERROR ? L"DRIVER_INTERNAL_ERROR" :
		reason == S_OK ? L"S_OK(not removed)" : L"other";
	wchar_t buf[160];
	swprintf_s(buf, L"[NRIDRED] removed at %s reason=0x%08X %s", where, (unsigned)reason, name);
	AppendCpuRuntimeTrace(buf);

	Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
	if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dred))) || !dred)
	{
		AppendCpuRuntimeTrace(L"[NRIDRED] no DRED data (set CORONA_NRI_DRED before launch)");
		return;
	}
	D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 bc = {};
	if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&bc)))
	{
		auto opName = [](UINT o) -> const wchar_t*
		{
			switch (o)
			{
			case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return L"DRAW";
			case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return L"DRAWINDEXED";
			case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return L"DISPATCH";
			case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return L"EXECUTEINDIRECT";
			case D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS: return L"DISPATCHRAYS";
			case D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE: return L"BUILD_AS";
			case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return L"COPYBUFFER";
			case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return L"COPYTEX";
			case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE: return L"RESOLVE";
			case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return L"CLEARRTV";
			case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW: return L"CLEARUAV";
			case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return L"MARKER";
			case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return L"BEGINEVENT";
			case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return L"ENDEVENT";
			default: return L"op";
			}
		};
		for (const D3D12_AUTO_BREADCRUMB_NODE1* n = bc.pHeadAutoBreadcrumbNode; n; n = n->pNext)
		{
			const UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
			const UINT total = n->BreadcrumbCount;
			if (done >= total || !n->pCommandHistory)
				continue; // this list finished — not the faulting one
			wchar_t b2[256];
			swprintf_s(b2, L"[NRIDRED] in-flight cmdlist=\"%s\" completedOps=%u/%u faultingOp=%u(%s)",
				n->pCommandListDebugNameW ? n->pCommandListDebugNameW : L"?", done, total,
				(UINT)n->pCommandHistory[done], opName(n->pCommandHistory[done]));
			AppendCpuRuntimeTrace(b2);
			// Op-type window leading up to the hang: a run of DRAWINDEXED => GBuffer;
			// DISPATCH/DISPATCHRAYS just before => an RT/compute pass; a lone DRAW after
			// dispatches => a fullscreen lighting/tonemap/imgui pass.
			const UINT lo = done > 8 ? done - 8 : 0;
			std::wstring seq;
			for (UINT i = lo; i <= done && i < total; ++i)
			{
				seq += std::to_wstring(i); seq += L":"; seq += opName(n->pCommandHistory[i]); seq += L" ";
			}
			AppendCpuRuntimeTrace(L"[NRIDRED] ops " + seq);
		}
	}
	D3D12_DRED_PAGE_FAULT_OUTPUT1 pf = {};
	if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&pf)) && pf.PageFaultVA != 0)
	{
		wchar_t b3[128];
		swprintf_s(b3, L"[NRIDRED] PAGE FAULT VA=0x%llX (bad/freed resource access)", (unsigned long long)pf.PageFaultVA);
		AppendCpuRuntimeTrace(b3);
		for (const D3D12_DRED_ALLOCATION_NODE1* a = pf.pHeadRecentFreedAllocationNode; a; a = a->pNext)
			AppendCpuRuntimeTrace(std::wstring(L"[NRIDRED]   recently-freed near VA: ") + (a->ObjectNameW ? a->ObjectNameW : L"?"));
	}
}

void NRIBackend::EndFrame()
{
	if (!m->ActiveCmd || m->DeviceLost)
		return;
	m->FlushRecordProfileIfReady();
	const bool profile = IsNriRecordProfileEnabled();
	NriCpuProfileScope frameScope(profile, m->RecordProfile.EndFrameMs, m->RecordProfile.EndFrameCount);
	// TEMP perf instrumentation: per-120-frame hot-op counts (CORONA_NRI_STATS).
	{
		static const bool s_stats = std::getenv("CORONA_NRI_STATS") != nullptr;
		if (s_stats && ++m->StatFrames >= 120)
		{
			AppendCpuRuntimeTrace(
				L"[NRIStat] per120frames committedBufAllocs=" + std::to_wstring(m->StatAlloc) +
				L", updateDescriptorRanges=" + std::to_wstring(m->StatUDR) +
				L", mapBuffer=" + std::to_wstring(m->StatMap));
			m->StatAlloc = 0; m->StatUDR = 0; m->StatMap = 0; m->StatFrames = 0;
		}
	}
	m->EndRP();
	m->RTColors.clear(); m->RTDepth = nullptr;
	if (m->Streamer)
		m->StreamerI.EndStreamerFrame(*m->Streamer);

	if (m->FrameHasBackbuffer)
	{
		NRIBackend::Impl::FrameContext* frame = m->ActiveFrameContext();
		const bool usesFrameContext = frame && frame->commandBuffer == m->ActiveCmd;
		// Make sure the backbuffer ends in PRESENT layout even if nothing drew to it.
		if (m->BBLayout != nri::Layout::PRESENT)
			m->TransitionBackbuffer(nri::AccessBits::NONE, nri::Layout::PRESENT, nri::StageBits::NONE);

		const auto endCmdStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
		m->Core.EndCommandBuffer(*m->ActiveCmd);
		NriCpuProfileAdd(profile, m->RecordProfile.EndCommandBufferMs, m->RecordProfile.EndCommandBufferCount, endCmdStart);

		nri::Fence* release = m->ReleaseSem[m->CurrentBackBuffer];
		nri::FenceSubmitDesc waitAcq = {}; waitAcq.fence = m->CurAcquire; waitAcq.stages = nri::StageBits::ALL;
		nri::FenceSubmitDesc sigRel = {}; sigRel.fence = release;
		const uint64_t submittedFenceValue = 1 + m->SwapFrameIndex;
		nri::FenceSubmitDesc sigFrame = {}; sigFrame.fence = m->FrameFence; sigFrame.value = submittedFenceValue;
		nri::FenceSubmitDesc signals[2] = { sigRel, sigFrame };
		nri::CommandBuffer* cbs[1] = { m->ActiveCmd };
		nri::QueueSubmitDesc qs = {};
		qs.waitFences = &waitAcq; qs.waitFenceNum = 1;
		qs.commandBuffers = cbs; qs.commandBufferNum = 1;
		qs.signalFences = signals; qs.signalFenceNum = 2;
		const auto submitStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
		const nri::Result submitResult = m->Core.QueueSubmit(*m->GraphicsQueue, qs);
		NriCpuProfileAdd(profile, m->RecordProfile.QueueSubmitMs, m->RecordProfile.QueueSubmitCount, submitStart);
		const bool submitted = submitResult == nri::Result::SUCCESS;
		bool waitedSubmittedFence = false;
		if (!submitted)
		{
			LogD3D12DeviceRemoved(reinterpret_cast<ID3D12Device*>(m->Core.GetDeviceNativeObject(m->Device)), L"EndFrame QueueSubmit");
			MarkDeviceLost("QueueSubmit", static_cast<int>(submitResult));
		}
		else if (usesFrameContext)
			frame->fenceValue = submittedFenceValue;
		if (submitted)
		{
			if (!m->PendingWindowCapturePath.empty())
			{
				const auto waitStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
				m->Core.Wait(*m->FrameFence, submittedFenceValue);
				NriCpuProfileAdd(profile, m->RecordProfile.FrameFenceWaitMs, m->RecordProfile.FrameFenceWaitCount, waitStart);
				waitedSubmittedFence = true;

				m->LastWindowCapturePath = m->PendingWindowCapturePath;
				m->PendingWindowCapturePath.clear();
				m->LastWindowCaptureError.clear();
				m->LastWindowCaptureSucceeded = false;
				m->LastWindowCaptureResultValid = true;

				Texture* renderTarget =
					(m->CurrentBackBuffer < m->BackBufferWrappers.size()) ?
					m->BackBufferWrappers[m->CurrentBackBuffer].get() :
					nullptr;
				if (renderTarget)
				{
					CapturedImage captured;
					nri::CommandBuffer* submittedCommandBuffer = m->ActiveCmd;
					m->ActiveCmd = nullptr;
					const bool captureOk = CaptureTexture(renderTarget, captured, EResourceState::Present);
					m->ActiveCmd = submittedCommandBuffer;
					if (captureOk)
					{
						if ((captured.Format == ETextureFormat::RGBA8Unorm || captured.Format == ETextureFormat::BGRA8Unorm) &&
							captured.RowPitch >= captured.Width * 4u)
						{
							for (uint32_t y = 0; y < captured.Height; ++y)
							{
								uint8_t* row = captured.Pixels.data() + static_cast<size_t>(y) * captured.RowPitch;
								for (uint32_t x = 0; x < captured.Width; ++x)
									row[x * 4u + 3u] = 0xff;
							}
						}
						m->LastWindowCaptureSucceeded = CoronaImageIO::SavePNG(
							captured,
							m->LastWindowCapturePath,
							&m->LastWindowCaptureError);
					}
					else
					{
						m->LastWindowCaptureError = ErrorString.empty() ?
							L"NRI window capture failed." :
							AnsiToWString(ErrorString.c_str());
					}
				}
				else
				{
					m->LastWindowCaptureError = L"NRI window capture render target unavailable.";
				}
			}

			const auto presentStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
			const nri::Result presentResult = m->SwapChainI.QueuePresent(*m->SwapChain, *release);
			NriCpuProfileAdd(profile, m->RecordProfile.QueuePresentMs, m->RecordProfile.QueuePresentCount, presentStart);
			if (presentResult != nri::Result::SUCCESS)
				MarkDeviceLost("QueuePresent", static_cast<int>(presentResult));
		}
		m->SwapFrameIndex++;
		if (submitted && !usesFrameContext && !waitedSubmittedFence)
		{
			const auto waitStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
			m->Core.Wait(*m->FrameFence, submittedFenceValue);
			NriCpuProfileAdd(profile, m->RecordProfile.FrameFenceWaitMs, m->RecordProfile.FrameFenceWaitCount, waitStart);
		}
	}
	else
	{
		const auto endCmdStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
		m->Core.EndCommandBuffer(*m->ActiveCmd);
		NriCpuProfileAdd(profile, m->RecordProfile.EndCommandBufferMs, m->RecordProfile.EndCommandBufferCount, endCmdStart);
		nri::FenceSubmitDesc sf = {};
		sf.fence = m->Fence; sf.value = ++m->FenceValue; sf.stages = nri::StageBits::ALL;
		nri::CommandBuffer* cbs[1] = { m->ActiveCmd };
		nri::QueueSubmitDesc qs = {};
		qs.commandBuffers = cbs; qs.commandBufferNum = 1;
		qs.signalFences = &sf; qs.signalFenceNum = 1;
		const auto submitStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
		const nri::Result submitResult = m->Core.QueueSubmit(*m->GraphicsQueue, qs);
		NriCpuProfileAdd(profile, m->RecordProfile.QueueSubmitMs, m->RecordProfile.QueueSubmitCount, submitStart);
		if (submitResult == nri::Result::SUCCESS)
		{
			const auto waitStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
			m->Core.Wait(*m->Fence, m->FenceValue);
			NriCpuProfileAdd(profile, m->RecordProfile.FrameFenceWaitMs, m->RecordProfile.FrameFenceWaitCount, waitStart);
		}
		else
			MarkDeviceLost("QueueSubmit(no backbuffer)", static_cast<int>(submitResult));
	}
	if (profile)
		++m->RecordProfile.Frames;
	m->ActiveCmd = nullptr;
}
void NRIBackend::WaitForGpu()
{
	if (!m || !m->Device)
		return;
	if (m->DeviceLost)
		return;
	m->WaitForFrameContexts();
	m->WaitForImmediateContexts();
	if (m->Fence && m->FenceValue != 0)
	{
		const bool profile = IsNriRecordProfileEnabled();
		const auto waitStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
		m->Core.Wait(*m->Fence, m->FenceValue);
		NriCpuProfileAdd(profile, m->RecordProfile.ImmediateFenceWaitMs, m->RecordProfile.ImmediateFenceWaitCount, waitStart);
	}
}
void NRIBackend::EmitGpuCrashMarker(const char* markerName)
{
	if (!m || !m->ActiveCmd || !markerName || !m->Core.CmdBeginAnnotation || !m->Core.CmdEndAnnotation)
		return;
	m->Core.CmdBeginAnnotation(*m->ActiveCmd, markerName, 0xff3399ffu);
	m->Core.CmdEndAnnotation(*m->ActiveCmd);
}
bool NRIBackend::IsDeviceLost() const { return m && m->DeviceLost; }
const std::string& NRIBackend::GetErrorString() const { return ErrorString; }
void NRIBackend::ClearErrorString() { ErrorString.clear(); }
uint64_t NRIBackend::GetTimestampFrequency() const
{
	if (!m || !m->Device || !m->Core.GetDeviceDesc)
		return 0;
	return m->Core.GetDeviceDesc(*m->Device).other.timestampFrequencyHz;
}
uint32_t NRIBackend::GetFrameCount() const
{
	return m ? m->FrameRingCount() : 1u;
}
uint32_t NRIBackend::GetCurrentFrameIndex() const
{
	if (!m)
		return 0;
	return m->ActiveFrameContextIndex % std::max(1u, m->FrameRingCount());
}
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
	if (desc.Shape == EBufferShape::ByteAddress)
		usage = usage | nri::BufferUsageBits::ARGUMENT_BUFFER;
	const uint32_t stride = (desc.Shape == EBufferShape::Structured) ? desc.ElementSize : 0;

	const Impl::BufKey key{ size, stride, static_cast<uint32_t>(usage), static_cast<uint32_t>(nri::MemoryLocation::DEVICE) };

	// Reuse a pooled allocation of the same spec if available (no committed GPU
	// heap allocation), else allocate. Either way the wrapper's deleter recycles
	// it back to the pool on destruction so per-frame buffers don't leak/realloc.
	Impl::BufferAlloc alloc;
	if (!m->TryReuseBuffer(key, alloc))
	{
		nri::Buffer* nbuf = nullptr;
		std::vector<nri::Memory*> nmem;
		if (!m->CreateBoundBuffer(size, stride, usage, nri::MemoryLocation::DEVICE, nbuf, nmem))
		{
			ErrorString = "CreateBuffer: CreateBoundBuffer failed";
			return nullptr;
		}
		alloc.buffer = nbuf;
		alloc.memory = std::move(nmem);
		alloc.mapped = nullptr;
	}
	alloc.key = key;

	bool uploadedInitialData = false;
	if (desc.InitialData && m->GraphicsQueue)
	{
		nri::BufferUploadDesc up = {};
		up.buffer = alloc.buffer;
		up.data = desc.InitialData;
		up.after.access = nri::AccessBits::SHADER_RESOURCE;
		up.after.stages = nri::StageBits::ALL;
		const bool profile = IsNriRecordProfileEnabled();
		const auto uploadStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
		m->Helper.UploadData(*m->GraphicsQueue, nullptr, 0, &up, 1);
		NriCpuProfileAdd(profile, m->RecordProfile.HelperUploadDataMs, m->RecordProfile.HelperUploadDataCount, uploadStart);
		uploadedInitialData = true;
	}
	if (!uploadedInitialData && desc.InitialState == EInitialResourceState::CopyDest)
	{
		alloc.access = { nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY };
		alloc.accessValid = true;
	}
	else
	{
		alloc.access = { nri::AccessBits::SHADER_RESOURCE, nri::StageBits::ALL };
		alloc.accessValid = true;
	}

	// Custom deleter: when the last reference dies while the backend is alive,
	// recycle the NRI allocation into the reuse pool and drop the side-table
	// entry. If Corona destroys the backend before older persistent Buffer
	// wrappers, the backend destructor has already released the NRI allocations;
	// the deleter must then only delete the wrapper object.
	Impl* impl = m.get();
	std::weak_ptr<std::atomic_bool> alive = m->Alive;
	std::shared_ptr<Buffer> wrapper(new Buffer(), [impl, alive](Buffer* b)
	{
		const std::shared_ptr<std::atomic_bool> aliveFlag = alive.lock();
		if (aliveFlag && aliveFlag->load(std::memory_order_acquire) && impl)
		{
			auto it = impl->Buffers.find(b);
			if (it != impl->Buffers.end())
			{
				impl->RecycleBuffer(std::move(it->second));
				impl->Buffers.erase(it);
			}
		}
		delete b;
	});
	wrapper->Type = (desc.Shape == EBufferShape::Structured) ? Buffer::STRUCTURED : Buffer::BYTE_ADDRESS;
	wrapper->NumElements = desc.NumElements;
	wrapper->ElementSize = desc.ElementSize;

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
				const bool profile = IsNriRecordProfileEnabled();
				const auto uploadStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
				bool uploadOk = false;
				if (subresourcesOk)
					uploadOk = m->Helper.UploadData(*m->GraphicsQueue, &up, 1, nullptr, 0) == nri::Result::SUCCESS;
				NriCpuProfileAdd(profile, m->RecordProfile.HelperUploadDataMs, m->RecordProfile.HelperUploadDataCount, uploadStart);
				if (uploadOk)
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
	{
		const bool profile = IsNriRecordProfileEnabled();
		const auto uploadStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
		m->Helper.UploadData(*m->GraphicsQueue, &up, 1, nullptr, 0);
		NriCpuProfileAdd(profile, m->RecordProfile.HelperUploadDataMs, m->RecordProfile.HelperUploadDataCount, uploadStart);
	}

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
void NRIBackend::UploadTexture3D(Texture* texture, const void* data, uint64_t rowPitch, uint64_t slicePitch)
{
	if (!m || !m->Device || !m->GraphicsQueue || !texture || !data || rowPitch == 0 || slicePitch == 0)
		return;
	auto it = m->Textures.find(texture);
	if (it == m->Textures.end() || !it->second.texture)
		return;

	const nri::TextureDesc& td = m->Core.GetTextureDesc(*it->second.texture);
	if (td.type != nri::TextureType::TEXTURE_3D || td.mipNum == 0 || td.depth == 0)
		return;
	if (rowPitch > UINT32_MAX || slicePitch > UINT32_MAX)
	{
		ErrorString = "UploadTexture3D: pitch is too large";
		return;
	}

	nri::TextureSubresourceUploadDesc sub = {};
	sub.slices = data;
	sub.sliceNum = td.depth;
	sub.rowPitch = static_cast<uint32_t>(rowPitch);
	sub.slicePitch = static_cast<uint32_t>(slicePitch);

	nri::TextureUploadDesc up = {};
	up.subresources = &sub;
	up.texture = it->second.texture;
	up.planes = nri::PlaneBits::ALL;
	up.after.access = nri::AccessBits::SHADER_RESOURCE;
	up.after.layout = nri::Layout::SHADER_RESOURCE;
	up.after.stages = nri::StageBits::ALL;

	const bool profile = IsNriRecordProfileEnabled();
	const auto uploadStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
	const nri::Result result = m->Helper.UploadData(*m->GraphicsQueue, &up, 1, nullptr, 0);
	NriCpuProfileAdd(profile, m->RecordProfile.HelperUploadDataMs, m->RecordProfile.HelperUploadDataCount, uploadStart);
	if (result == nri::Result::SUCCESS)
	{
		m->TexLayout[texture] = nri::Layout::SHADER_RESOURCE;
	}
	else
	{
		ErrorString = "UploadTexture3D: UploadData failed";
	}
}
std::shared_ptr<VertexBuffer> NRIBackend::CreateVertexBuffer(uint32_t size, uint32_t stride, void* srcData)
{
	if (!m->RasterEnabled || !m->Device || size == 0) return nullptr;
	// Static geometry lives in DEVICE memory; the one-time upload cost is paid at load time.
	nri::Buffer* nb = nullptr; std::vector<nri::Memory*> mem;
	if (!m->CreateBoundBuffer(size, 0,
		nri::BufferUsageBits::VERTEX_BUFFER | nri::BufferUsageBits::SHADER_RESOURCE | nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT,
		nri::MemoryLocation::DEVICE, nb, mem)) return nullptr;
	if (srcData)
	{
		if (!m->GraphicsQueue)
		{
			m->FreeBuffer(nb, mem);
			return nullptr;
		}
		nri::BufferUploadDesc up = {};
		up.buffer = nb;
		up.data = srcData;
		up.after.access = nri::AccessBits::VERTEX_BUFFER | nri::AccessBits::SHADER_RESOURCE;
		up.after.stages = nri::StageBits::ALL;
		const bool profile = IsNriRecordProfileEnabled();
		const auto uploadStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
		const bool uploadOk = m->Helper.UploadData(*m->GraphicsQueue, nullptr, 0, &up, 1) == nri::Result::SUCCESS;
		NriCpuProfileAdd(profile, m->RecordProfile.HelperUploadDataMs, m->RecordProfile.HelperUploadDataCount, uploadStart);
		if (!uploadOk)
		{
			m->FreeBuffer(nb, mem);
			return nullptr;
		}
	}
	auto w = std::make_shared<VertexBuffer>(); w->numVertices = stride ? (int)(size / stride) : 0;
	Impl::GpuBuf gb; gb.buffer = nb; gb.memory = std::move(mem); gb.stride = stride; gb.mapped = nullptr; gb.capacity = size;
	gb.access = { nri::AccessBits::VERTEX_BUFFER | nri::AccessBits::SHADER_RESOURCE, nri::StageBits::ALL };
	gb.accessValid = true;
	m->VBs[w.get()] = std::move(gb);
	return w;
}
std::shared_ptr<IndexBuffer> NRIBackend::CreateIndexBuffer(EIndexFormat format, uint32_t size, void* srcData)
{
	if (!m->RasterEnabled || !m->Device || size == 0) return nullptr;
	nri::Buffer* nb = nullptr; std::vector<nri::Memory*> mem;
	if (!m->CreateBoundBuffer(size, 0,
		nri::BufferUsageBits::INDEX_BUFFER | nri::BufferUsageBits::SHADER_RESOURCE | nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT,
		nri::MemoryLocation::DEVICE, nb, mem)) return nullptr;
	if (srcData)
	{
		if (!m->GraphicsQueue)
		{
			m->FreeBuffer(nb, mem);
			return nullptr;
		}
		nri::BufferUploadDesc up = {};
		up.buffer = nb;
		up.data = srcData;
		up.after.access = nri::AccessBits::INDEX_BUFFER | nri::AccessBits::SHADER_RESOURCE;
		up.after.stages = nri::StageBits::ALL;
		const bool profile = IsNriRecordProfileEnabled();
		const auto uploadStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
		const bool uploadOk = m->Helper.UploadData(*m->GraphicsQueue, nullptr, 0, &up, 1) == nri::Result::SUCCESS;
		NriCpuProfileAdd(profile, m->RecordProfile.HelperUploadDataMs, m->RecordProfile.HelperUploadDataCount, uploadStart);
		if (!uploadOk)
		{
			m->FreeBuffer(nb, mem);
			return nullptr;
		}
	}
	const uint32_t idxSize = (format == EIndexFormat::U16) ? 2u : 4u;
	auto w = std::make_shared<IndexBuffer>(); w->numIndices = (int)(size / idxSize);
	Impl::GpuBuf gb; gb.buffer = nb; gb.memory = std::move(mem); gb.indexType = (format == EIndexFormat::U16) ? nri::IndexType::UINT16 : nri::IndexType::UINT32; gb.mapped = nullptr; gb.capacity = size;
	gb.access = { nri::AccessBits::INDEX_BUFFER | nri::AccessBits::SHADER_RESOURCE, nri::StageBits::ALL };
	gb.accessValid = true;
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
	gb.access = { nri::AccessBits::VERTEX_BUFFER | nri::AccessBits::SHADER_RESOURCE, nri::StageBits::ALL };
	gb.accessValid = true;
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
	gb.access = { nri::AccessBits::INDEX_BUFFER | nri::AccessBits::SHADER_RESOURCE, nri::StageBits::ALL };
	gb.accessValid = true;
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
	gb.access = { nri::AccessBits::VERTEX_BUFFER | nri::AccessBits::SHADER_RESOURCE, nri::StageBits::ALL };
	gb.accessValid = true;
	m->VBs[w.get()] = std::move(gb);
	return w;
}
std::shared_ptr<Buffer> NRIBackend::CreateUploadStructuredBuffer(uint32_t numElements, uint32_t elementSize)
{
	if (!m->Device || numElements == 0 || elementSize == 0) return nullptr;
	const uint64_t size = static_cast<uint64_t>(numElements) * elementSize;
	nri::Buffer* nb = nullptr; std::vector<nri::Memory*> mem;
	if (!m->CreateBoundBuffer(
		size,
		elementSize,
		nri::BufferUsageBits::SHADER_RESOURCE | nri::BufferUsageBits::ARGUMENT_BUFFER,
		nri::MemoryLocation::HOST_UPLOAD,
		nb,
		mem))
	{
		ErrorString = "CreateUploadStructuredBuffer: CreateBoundBuffer failed";
		return nullptr;
	}
	void* mapped = m->Core.MapBuffer(*nb, 0, size); // persistent map (HOST_UPLOAD)
	auto w = std::make_shared<Buffer>();
	w->Type = Buffer::STRUCTURED; w->NumElements = numElements; w->ElementSize = elementSize;
	Impl::BufferAlloc alloc; alloc.buffer = nb; alloc.memory = std::move(mem); alloc.mapped = mapped;
	alloc.key = Impl::BufKey{
		size,
		elementSize,
		static_cast<uint32_t>(nri::BufferUsageBits::SHADER_RESOURCE | nri::BufferUsageBits::ARGUMENT_BUFFER),
		static_cast<uint32_t>(nri::MemoryLocation::HOST_UPLOAD)
	};
	m->Buffers[w.get()] = std::move(alloc);
	return w;
}
void NRIBackend::UpdateUploadStructuredBuffer(Buffer* buffer, const void* srcData, uint32_t sizeInBytes)
{
	auto it = m->Buffers.find(buffer);
	if (it != m->Buffers.end() && it->second.mapped && srcData)
		memcpy(it->second.mapped, srcData, sizeInBytes);
}
bool NRIBackend::UpdateDefaultStructuredBuffer(Buffer* buffer, const void* srcData, uint32_t sizeInBytes)
{
	if (!m->Device || !buffer || !srcData)
		return false;
	const uint64_t capacity = static_cast<uint64_t>(buffer->NumElements) * buffer->ElementSize;
	if (sizeInBytes == 0)
		return true;
	if (static_cast<uint64_t>(sizeInBytes) > capacity)
		return false;

	auto dstIt = m->Buffers.find(buffer);
	if (dstIt == m->Buffers.end() || !dstIt->second.buffer)
		return false;

	const bool profile = IsNriRecordProfileEnabled();
	if (profile)
		m->RecordProfile.TransientDefaultBytes += sizeInBytes;

	const nri::AccessStage copyDest{ nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY };
	const nri::AccessStage shaderRead{ nri::AccessBits::SHADER_RESOURCE, nri::StageBits::ALL };

	if (!m->ActiveCmd)
	{
		if (!m->GraphicsQueue)
			return false;
		nri::BufferUploadDesc up = {};
		up.buffer = dstIt->second.buffer;
		up.data = srcData;
		up.after = shaderRead;
		const auto uploadStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
		const bool uploadOk = m->Helper.UploadData(*m->GraphicsQueue, nullptr, 0, &up, 1) == nri::Result::SUCCESS;
		NriCpuProfileAdd(profile, m->RecordProfile.HelperUploadDataMs, m->RecordProfile.HelperUploadDataCount, uploadStart);
		if (!uploadOk)
			return false;
		dstIt->second.access = shaderRead;
		dstIt->second.accessValid = true;
		return true;
	}

	const Impl::BufKey stagingKey{
		sizeInBytes,
		0,
		static_cast<uint32_t>(nri::BufferUsageBits::NONE),
		static_cast<uint32_t>(nri::MemoryLocation::HOST_UPLOAD)
	};

	Impl::BufferAlloc stagingAlloc;
	if (!m->TryReuseBuffer(stagingKey, stagingAlloc))
	{
		if (profile)
			++m->RecordProfile.TransientDefaultReuseMiss;
		nri::Buffer* sbuf = nullptr;
		std::vector<nri::Memory*> smem;
		if (!m->CreateBoundBuffer(sizeInBytes, 0, nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_UPLOAD, sbuf, smem))
		{
			ErrorString = "UpdateDefaultStructuredBuffer: CreateBoundBuffer staging failed";
			return false;
		}
		stagingAlloc.buffer = sbuf;
		stagingAlloc.memory = std::move(smem);
		stagingAlloc.mapped = m->Core.MapBuffer(*sbuf, 0, sizeInBytes);
		++m->StatMap;
	}
	else if (profile)
	{
		++m->RecordProfile.TransientDefaultReuseHit;
	}
	stagingAlloc.key = stagingKey;
	if (!stagingAlloc.mapped)
	{
		m->RecycleBuffer(std::move(stagingAlloc));
		return false;
	}

	{
		const auto memcpyStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
		memcpy(stagingAlloc.mapped, srcData, sizeInBytes);
		NriCpuProfileAdd(profile, m->RecordProfile.TransientDefaultMemcpyMs, m->RecordProfile.TransientDefaultMemcpyCount, memcpyStart);
	}

	nri::Buffer* dstBuffer = dstIt->second.buffer;
	const nri::AccessStage dstBefore = dstIt->second.accessValid ? dstIt->second.access : shaderRead;
	m->EndRP();
	{
		const auto copyStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
		const nri::AccessStage noneAccess{ nri::AccessBits::NONE, nri::StageBits::ALL };
		const nri::AccessStage copySource{ nri::AccessBits::COPY_SOURCE, nri::StageBits::COPY };

		nri::BufferBarrierDesc toCopy[2] = {};
		toCopy[0].buffer = stagingAlloc.buffer;
		toCopy[0].before = stagingAlloc.accessValid ? stagingAlloc.access : noneAccess;
		toCopy[0].after = copySource;
		toCopy[1].buffer = dstBuffer;
		toCopy[1].before = dstBefore;
		toCopy[1].after = copyDest;
		nri::BarrierDesc copyBarrier = {};
		copyBarrier.buffers = toCopy;
		copyBarrier.bufferNum = 2;
		m->Core.CmdBarrier(*m->ActiveCmd, copyBarrier);

		m->Core.CmdCopyBuffer(*m->ActiveCmd, *dstBuffer, 0, *stagingAlloc.buffer, 0, sizeInBytes);

		nri::BufferBarrierDesc toShader = {};
		toShader.buffer = dstBuffer;
		toShader.before = copyDest;
		toShader.after = shaderRead;
		nri::BarrierDesc shaderBarrier = {};
		shaderBarrier.buffers = &toShader;
		shaderBarrier.bufferNum = 1;
		m->Core.CmdBarrier(*m->ActiveCmd, shaderBarrier);

		stagingAlloc.access = copySource;
		stagingAlloc.accessValid = true;
		dstIt->second.access = shaderRead;
		dstIt->second.accessValid = true;
		NriCpuProfileAdd(profile, m->RecordProfile.TransientDefaultCopyRecordMs, m->RecordProfile.TransientDefaultCopyRecordCount, copyStart);
	}

	auto staging = std::make_shared<Buffer>();
	staging->Type = Buffer::BYTE_ADDRESS;
	staging->NumElements = sizeInBytes;
	staging->ElementSize = 1;
	m->Buffers[staging.get()] = std::move(stagingAlloc);
	if (NRIBackend::Impl::FrameContext* frame = m->ActiveFrameContext())
		frame->transientBuffers.push_back(staging);
	else
		m->TransientBuffers.push_back(staging);
	return true;
}

bool NRIBackend::CreateOrUpdateRayTracingInstancePropertyBuffer(
	std::shared_ptr<Buffer>& buffer,
	uint32_t numElements,
	uint32_t elementSize,
	const void* srcData,
	uint32_t sizeInBytes,
	std::wstring* outFailureReason)
{
	auto fail = [&](const wchar_t* reason)
	{
		if (outFailureReason)
			*outFailureReason = reason ? reason : L"unknown";
		return false;
	};

	if (!m->Device)
		return fail(L"device unavailable");
	if (numElements == 0 || elementSize == 0)
		return fail(L"invalid buffer dimensions");
	if (!srcData || sizeInBytes == 0)
		return fail(L"missing source data");
	if (static_cast<uint64_t>(sizeInBytes) > static_cast<uint64_t>(numElements) * static_cast<uint64_t>(elementSize))
		return fail(L"source data exceeds buffer capacity");

	const bool needsCreate =
		!buffer ||
		buffer->NumElements < numElements ||
		buffer->ElementSize != elementSize ||
		buffer->Type != Buffer::BYTE_ADDRESS;
	if (needsCreate)
	{
		try
		{
			buffer = CreateBuffer({
				numElements,
				elementSize,
				EInitialResourceState::ShaderRead,
				false,
				const_cast<void*>(srcData),
				EBufferShape::ByteAddress
			});
		}
		catch (...)
		{
			buffer = nullptr;
			return fail(L"CreateBuffer threw");
		}
		if (!buffer)
			return fail(L"CreateBuffer returned null");
	}
	else if (!UpdateDefaultStructuredBuffer(buffer.get(), srcData, sizeInBytes))
	{
		return fail(L"UpdateDefaultStructuredBuffer failed");
	}

	if (outFailureReason)
		outFailureReason->clear();
	return true;
}

std::shared_ptr<Buffer> NRIBackend::AllocateTransientUploadStructuredBuffer(uint32_t numElements, uint32_t elementSize, const void* srcData)
{
	if (!m->Device || numElements == 0 || elementSize == 0)
		return nullptr;
	const bool profile = IsNriRecordProfileEnabled();
	NriCpuProfileScope allocScope(profile, m->RecordProfile.TransientUploadAllocMs, m->RecordProfile.TransientUploadAllocCount);
	const uint64_t size = static_cast<uint64_t>(numElements) * elementSize;
	if (profile)
		m->RecordProfile.TransientUploadBytes += size;
	const nri::BufferUsageBits usage =
		nri::BufferUsageBits::SHADER_RESOURCE |
		nri::BufferUsageBits::ARGUMENT_BUFFER;
	const Impl::BufKey key{
		size,
		elementSize,
		static_cast<uint32_t>(usage),
		static_cast<uint32_t>(nri::MemoryLocation::HOST_UPLOAD)
	};

	Impl::BufferAlloc alloc;
	if (!m->TryReuseBuffer(key, alloc))
	{
		if (profile)
			++m->RecordProfile.TransientUploadReuseMiss;
		nri::Buffer* nb = nullptr;
		std::vector<nri::Memory*> mem;
		const auto createStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
		if (!m->CreateBoundBuffer(size, elementSize, usage, nri::MemoryLocation::HOST_UPLOAD, nb, mem))
		{
			NriCpuProfileAdd(profile, m->RecordProfile.TransientUploadCreateMs, m->RecordProfile.TransientUploadCreateCount, createStart);
			ErrorString = "AllocateTransientUploadStructuredBuffer: CreateBoundBuffer failed";
			return nullptr;
		}
		NriCpuProfileAdd(profile, m->RecordProfile.TransientUploadCreateMs, m->RecordProfile.TransientUploadCreateCount, createStart);
		alloc.buffer = nb;
		alloc.memory = std::move(mem);
		const auto mapStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
		alloc.mapped = m->Core.MapBuffer(*nb, 0, size);
		NriCpuProfileAdd(profile, m->RecordProfile.TransientUploadMapMs, m->RecordProfile.TransientUploadMapCount, mapStart);
		++m->StatMap;
	}
	else if (profile)
	{
		++m->RecordProfile.TransientUploadReuseHit;
	}
	alloc.key = key;

	auto buf = std::make_shared<Buffer>();
	buf->Type = Buffer::STRUCTURED;
	buf->NumElements = numElements;
	buf->ElementSize = elementSize;
	if (srcData)
	{
		if (alloc.mapped)
		{
			const auto memcpyStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
			memcpy(alloc.mapped, srcData, static_cast<size_t>(size));
			NriCpuProfileAdd(profile, m->RecordProfile.TransientUploadMemcpyMs, m->RecordProfile.TransientUploadMemcpyCount, memcpyStart);
		}
	}
	m->Buffers[buf.get()] = std::move(alloc);
	if (NRIBackend::Impl::FrameContext* frame = m->ActiveFrameContext())
		frame->transientBuffers.push_back(buf);
	else
		m->TransientBuffers.push_back(buf);
	return buf;
}

std::shared_ptr<Buffer> NRIBackend::AllocateTransientDefaultStructuredBuffer(uint32_t numElements, uint32_t elementSize, const void* srcData)
{
	if (!m->Device || numElements == 0 || elementSize == 0)
		return nullptr;
	if (!m->ActiveCmd || !srcData)
		return AllocateTransientUploadStructuredBuffer(numElements, elementSize, srcData);

	const bool profile = IsNriRecordProfileEnabled();
	NriCpuProfileScope allocScope(profile, m->RecordProfile.TransientDefaultAllocMs, m->RecordProfile.TransientDefaultAllocCount);
	const uint64_t size = static_cast<uint64_t>(numElements) * elementSize;
	if (profile)
		m->RecordProfile.TransientDefaultBytes += size;

	const nri::BufferUsageBits deviceUsage = nri::BufferUsageBits::SHADER_RESOURCE;
	const Impl::BufKey deviceKey{
		size,
		elementSize,
		static_cast<uint32_t>(deviceUsage),
		static_cast<uint32_t>(nri::MemoryLocation::DEVICE)
	};

	Impl::BufferAlloc deviceAlloc;
	if (!m->TryReuseBuffer(deviceKey, deviceAlloc))
	{
		if (profile)
			++m->RecordProfile.TransientDefaultReuseMiss;
		nri::Buffer* nbuf = nullptr;
		std::vector<nri::Memory*> nmem;
		if (!m->CreateBoundBuffer(size, elementSize, deviceUsage, nri::MemoryLocation::DEVICE, nbuf, nmem))
		{
			ErrorString = "AllocateTransientDefaultStructuredBuffer: CreateBoundBuffer failed";
			return nullptr;
		}
		deviceAlloc.buffer = nbuf;
		deviceAlloc.memory = std::move(nmem);
		deviceAlloc.mapped = nullptr;
	}
	else if (profile)
	{
		++m->RecordProfile.TransientDefaultReuseHit;
	}
	deviceAlloc.key = deviceKey;

	const Impl::BufKey stagingKey{
		size,
		0,
		static_cast<uint32_t>(nri::BufferUsageBits::NONE),
		static_cast<uint32_t>(nri::MemoryLocation::HOST_UPLOAD)
	};
	Impl::BufferAlloc stagingAlloc;
	if (!m->TryReuseBuffer(stagingKey, stagingAlloc))
	{
		nri::Buffer* sbuf = nullptr;
		std::vector<nri::Memory*> smem;
		if (!m->CreateBoundBuffer(size, 0, nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_UPLOAD, sbuf, smem))
		{
			m->RecycleBuffer(std::move(deviceAlloc));
			ErrorString = "AllocateTransientDefaultStructuredBuffer: CreateBoundBuffer staging failed";
			return nullptr;
		}
		stagingAlloc.buffer = sbuf;
		stagingAlloc.memory = std::move(smem);
		stagingAlloc.mapped = m->Core.MapBuffer(*sbuf, 0, size);
		++m->StatMap;
	}
	stagingAlloc.key = stagingKey;
	if (!stagingAlloc.mapped)
	{
		m->RecycleBuffer(std::move(stagingAlloc));
		m->RecycleBuffer(std::move(deviceAlloc));
		return nullptr;
	}

	{
		const auto memcpyStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
		memcpy(stagingAlloc.mapped, srcData, static_cast<size_t>(size));
		NriCpuProfileAdd(profile, m->RecordProfile.TransientDefaultMemcpyMs, m->RecordProfile.TransientDefaultMemcpyCount, memcpyStart);
	}

	m->EndRP();
	{
		const auto copyStart = profile ? NriCpuProfileClock::now() : NriCpuProfileClock::time_point{};
		const nri::AccessStage noneAccess{ nri::AccessBits::NONE, nri::StageBits::ALL };
		const nri::AccessStage copySource{ nri::AccessBits::COPY_SOURCE, nri::StageBits::COPY };
		const nri::AccessStage copyDest{ nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY };
		const nri::AccessStage shaderRead{ nri::AccessBits::SHADER_RESOURCE, nri::StageBits::ALL };

		nri::BufferBarrierDesc toCopy[2] = {};
		toCopy[0].buffer = stagingAlloc.buffer;
		toCopy[0].before = stagingAlloc.accessValid ? stagingAlloc.access : noneAccess;
		toCopy[0].after = copySource;
		toCopy[1].buffer = deviceAlloc.buffer;
		toCopy[1].before = deviceAlloc.accessValid ? deviceAlloc.access : noneAccess;
		toCopy[1].after = copyDest;
		nri::BarrierDesc copyBarrier = {};
		copyBarrier.buffers = toCopy;
		copyBarrier.bufferNum = 2;
		m->Core.CmdBarrier(*m->ActiveCmd, copyBarrier);

		m->Core.CmdCopyBuffer(*m->ActiveCmd, *deviceAlloc.buffer, 0, *stagingAlloc.buffer, 0, size);

		nri::BufferBarrierDesc toShader = {};
		toShader.buffer = deviceAlloc.buffer;
		toShader.before = copyDest;
		toShader.after = shaderRead;
		nri::BarrierDesc shaderBarrier = {};
		shaderBarrier.buffers = &toShader;
		shaderBarrier.bufferNum = 1;
		m->Core.CmdBarrier(*m->ActiveCmd, shaderBarrier);

		stagingAlloc.access = copySource;
		stagingAlloc.accessValid = true;
		deviceAlloc.access = shaderRead;
		deviceAlloc.accessValid = true;
		NriCpuProfileAdd(profile, m->RecordProfile.TransientDefaultCopyRecordMs, m->RecordProfile.TransientDefaultCopyRecordCount, copyStart);
	}

	auto pushTransient = [&](const std::shared_ptr<Buffer>& b)
	{
		if (NRIBackend::Impl::FrameContext* frame = m->ActiveFrameContext())
			frame->transientBuffers.push_back(b);
		else
			m->TransientBuffers.push_back(b);
	};

	auto staging = std::make_shared<Buffer>();
	staging->Type = Buffer::STRUCTURED;
	staging->NumElements = numElements;
	staging->ElementSize = elementSize;
	m->Buffers[staging.get()] = std::move(stagingAlloc);
	pushTransient(staging);

	auto buf = std::make_shared<Buffer>();
	buf->Type = Buffer::STRUCTURED;
	buf->NumElements = numElements;
	buf->ElementSize = elementSize;
	m->Buffers[buf.get()] = std::move(deviceAlloc);
	pushTransient(buf);
	return buf;
}

// === Bindless registry ====================================================
RHITextureHandle NRIBackend::RegisterBindlessTexture(Texture* texture)
{
	if (!texture) return {};
	auto it = m->Textures.find(texture);
	if (it == m->Textures.end() || !it->second.texture) return {};
	return m->RegisterBindlessTextureImpl(it->second.texture, texture);
}
bool NRIBackend::UpdateBindlessTexture(Texture* texture)
{
	// Texture content/view is stable for now; re-register if not present.
	return texture && RegisterBindlessTexture(texture).IsValid();
}
void NRIBackend::UnregisterBindlessTexture(Texture* texture)
{
	auto it = m->BindlessTexHandles.find(texture);
	if (it == m->BindlessTexHandles.end()) return;
	const uint32_t idx = it->second.Index;
	if (idx < m->BindlessTexDescs.size() && m->BindlessTexDescs[idx])
	{
		m->Core.DestroyDescriptor(m->BindlessTexDescs[idx]);
		m->BindlessTexDescs[idx] = nullptr;
		if (idx < m->BindlessTexGen.size()) ++m->BindlessTexGen[idx];
		m->BindlessTexFreeList.push_back(idx);
	}
	m->BindlessTexHandles.erase(it);
}
RHITextureHandle NRIBackend::GetBindlessTextureHandle(const Texture* texture) const
{
	auto it = m->BindlessTexHandles.find(texture);
	return it != m->BindlessTexHandles.end() ? it->second : RHITextureHandle{};
}
RHIBufferHandle NRIBackend::RegisterBindlessBuffer(Buffer* buffer)
{
	if (!buffer) return {};
	auto it = m->Buffers.find(buffer);
	if (it == m->Buffers.end() || !it->second.buffer) return {};
	const uint64_t size = static_cast<uint64_t>(buffer->NumElements) * buffer->ElementSize;
	return m->RegisterBindlessBufferImpl(it->second.buffer, size, buffer);
}
RHIBufferHandle NRIBackend::RegisterBindlessVertexBuffer(VertexBuffer* buffer)
{
	if (!buffer) return {};
	auto it = m->VBs.find(buffer);
	if (it == m->VBs.end() || !it->second.buffer) return {};
	return m->RegisterBindlessBufferImpl(it->second.buffer, it->second.capacity, buffer);
}
RHIBufferHandle NRIBackend::RegisterBindlessIndexBuffer(IndexBuffer* buffer)
{
	if (!buffer) return {};
	auto it = m->IBs.find(buffer);
	if (it == m->IBs.end() || !it->second.buffer) return {};
	return m->RegisterBindlessBufferImpl(it->second.buffer, it->second.capacity, buffer);
}
void NRIBackend::UnregisterBindlessBuffer(Buffer* buffer) { m->BindlessBufHandles.erase(buffer); }
void NRIBackend::UnregisterBindlessVertexBuffer(VertexBuffer* buffer) { m->BindlessBufHandles.erase(buffer); }
void NRIBackend::UnregisterBindlessIndexBuffer(IndexBuffer* buffer) { m->BindlessBufHandles.erase(buffer); }
RHIBufferHandle NRIBackend::GetBindlessBufferHandle(const Buffer* buffer) const
{
	auto it = m->BindlessBufHandles.find(buffer);
	return it != m->BindlessBufHandles.end() ? it->second : RHIBufferHandle{};
}
RHIBufferHandle NRIBackend::GetBindlessVertexBufferHandle(const VertexBuffer* buffer) const
{
	auto it = m->BindlessBufHandles.find(buffer);
	return it != m->BindlessBufHandles.end() ? it->second : RHIBufferHandle{};
}
RHIBufferHandle NRIBackend::GetBindlessIndexBufferHandle(const IndexBuffer* buffer) const
{
	auto it = m->BindlessBufHandles.find(buffer);
	return it != m->BindlessBufHandles.end() ? it->second : RHIBufferHandle{};
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

	auto recordRefit = [&](nri::CommandBuffer& cmd)
	{
		m->RT.CmdBuildBottomLevelAccelerationStructures(cmd, &build, 1);
		m->BarrierAccelerationStructure(cmd, as->AccelerationStructure);
	};
	if (m->ActiveCmd)
	{
		m->EndRP();
		recordRefit(*m->ActiveCmd);
	}
	else
	{
		m->SubmitImmediate("[NRIRTAS] RefitBLAS immediate submit failed\n", recordRefit, false);
	}
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

	auto recordBuild = [&](nri::CommandBuffer& cmd)
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
	};
	bool submitted = false;
	if (m->ActiveCmd)
	{
		m->EndRP();
		recordBuild(*m->ActiveCmd);
		submitted = true;
	}
	else
	{
		submitted = m->SubmitImmediate("[NRIRTAS] BuildTLAS immediate submit failed\n", recordBuild, false);
	}
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

	auto recordUpdate = [&](nri::CommandBuffer& cmd)
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
	};
	bool submitted = false;
	if (m->ActiveCmd)
	{
		m->EndRP();
		recordUpdate(*m->ActiveCmd);
		submitted = true;
	}
	else
	{
		submitted = m->SubmitImmediate("[NRIRTAS] UpdateTLAS immediate submit failed\n", recordUpdate, false);
	}
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
void NRIBackend::ResetDynamicResources()
{
	if (!m)
		return;
	if (m->ActiveCmd)
		m->EndRP();
	WaitForGpu();
	for (NRIBackend::Impl::FrameContext& frame : m->FrameContexts)
		m->FreeTransientBuffers(frame.transientBuffers);
	m->FreeTransientBuffers();
	m->FlushBufferReusePool();
	m->PendingColorClears.clear();
	m->PendingDepthClears.clear();
}

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
	scd.flags = nri::SwapChainBits::ALLOW_TEARING;
	scd.queuedFrameNum = static_cast<uint8_t>(NRIBackend::Impl::kQueuedFrameNum);
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
	if (!m->EnsureFrameContexts(NRIBackend::Impl::kQueuedFrameNum))
	{
		ErrorString = "CreateSwapChain: frame context allocation failed";
		return;
	}

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
// === Streamline (DLSS-RR) native D3D12 interop ===========================
// DLSS Ray Reconstruction runs through Streamline, which needs the native
// ID3D12Device / ID3D12Resource / ID3D12GraphicsCommandList behind the NRI
// objects. NRI's CoreInterface exposes those via Get*NativeObject.
namespace
{
	uint32_t NRIToD3D12StreamlineState(EResourceState state)
	{
		switch (state)
		{
		case EResourceState::ShaderRead:
			return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		case EResourceState::UnorderedAccess: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		case EResourceState::RenderTarget:    return D3D12_RESOURCE_STATE_RENDER_TARGET;
		case EResourceState::DepthWrite:      return D3D12_RESOURCE_STATE_DEPTH_WRITE;
		case EResourceState::CopyDest:        return D3D12_RESOURCE_STATE_COPY_DEST;
		case EResourceState::CopySource:      return D3D12_RESOURCE_STATE_COPY_SOURCE;
		case EResourceState::Present:         return D3D12_RESOURCE_STATE_PRESENT;
		default:                              return D3D12_RESOURCE_STATE_COMMON;
		}
	}
}

bool NRIBackend::GetStreamlineTextureResource(Texture* texture, EResourceState state, StreamlineTextureResourceDesc& outDesc) const
{
	if (!m || !texture)
		return false;
	nri::Texture* nt = m->NriTex(texture);
	if (!nt)
		return false;
	auto* res = reinterpret_cast<ID3D12Resource*>(static_cast<uintptr_t>(m->Core.GetTextureNativeObject(nt)));
	if (!res)
		return false;
	const D3D12_RESOURCE_DESC d = res->GetDesc();
	outDesc = {};
	outDesc.Native = res;
	outDesc.State = NRIToD3D12StreamlineState(state);
	outDesc.Width = static_cast<uint32_t>(d.Width);
	outDesc.Height = d.Height;
	outDesc.NativeFormat = static_cast<uint32_t>(d.Format);
	outDesc.MipLevels = d.MipLevels;
	outDesc.ArrayLayers = d.DepthOrArraySize;
	outDesc.Flags = static_cast<uint32_t>(d.Flags);
	return true;
}

void* NRIBackend::GetStreamlineCommandBuffer()
{
	if (!m || !m->ActiveCmd)
		return nullptr;
	return m->Core.GetCommandBufferNativeObject(m->ActiveCmd);
}

void* NRIBackend::GetStreamlineNativeDevice() const
{
	if (!m || !m->Device)
		return nullptr;
	return m->Core.GetDeviceNativeObject(m->Device);
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
		NRIBackend::Impl::FrameContext* frame = m->ActiveFrameContext();
		nri::CommandAllocator* restartAllocator = frame ? frame->allocator : m->CmdAllocator;
		nri::CommandBuffer* restartCommandBuffer = frame ? frame->commandBuffer : m->CmdBuffer;
		if (restartAllocator && restartCommandBuffer)
		{
			m->Core.ResetCommandAllocator(*restartAllocator);
			if (m->Core.BeginCommandBuffer(*restartCommandBuffer, nullptr) == nri::Result::SUCCESS)
			{
				m->ActiveCmd = restartCommandBuffer;
				restartedActiveCmd = true;
			}
		}
	}
	else
	{
		uint32_t immediateBackBufferIndex = 0;
		const bool immediateBackBuffer = m->IsBackbuffer(source, immediateBackBufferIndex);
		const nri::Layout immediateBeforeLayout = immediateBackBuffer ?
			m->BBLayout :
			(m->TexLayout.count(source) ? m->TexLayout[source] : nri::Layout::UNDEFINED);
		submitted = m->SubmitImmediate("[NRI] CaptureTexture immediate submit failed\n", [&](nri::CommandBuffer& cmd)
		{
			const nri::Layout beforeLayout = immediateBeforeLayout;
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
		if (submitted)
		{
			if (immediateBackBuffer)
				m->BBLayout = immediateBeforeLayout;
		}
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
void NRIBackend::RequestWindowCapture(const std::wstring& outputPath)
{
	if (!m)
		return;
	m->PendingWindowCapturePath = outputPath;
	m->LastWindowCaptureResultValid = false;
	m->LastWindowCapturePath.clear();
	m->LastWindowCaptureError.clear();
	m->LastWindowCaptureSucceeded = false;
}
bool NRIBackend::ConsumeWindowCaptureResult(std::wstring* outputPath, bool* success, std::wstring* errorMessage)
{
	if (!m || !m->LastWindowCaptureResultValid)
		return false;

	if (outputPath)
		*outputPath = m->LastWindowCapturePath;
	if (success)
		*success = m->LastWindowCaptureSucceeded;
	if (errorMessage)
		*errorMessage = m->LastWindowCaptureError;

	m->LastWindowCaptureResultValid = false;
	m->LastWindowCapturePath.clear();
	m->LastWindowCaptureError.clear();
	m->LastWindowCaptureSucceeded = false;
	return true;
}

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
	sd.queuedFrameNum = m->FrameRingCount();
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
void NRIBackend::InitializeGpuTimestampQueries(uint32_t queryCount)
{
	if (!m || !m->Device || queryCount == 0)
		return;
	if (m->TimestampQueryPool && m->TimestampQueryCount == queryCount)
		return;

	ShutdownGpuTimestampQueries();

	if (!m->Core.CreateQueryPool || !m->Core.DestroyQueryPool || !m->Core.GetQuerySize ||
		!m->Core.CmdResetQueries ||
		!m->Core.CmdEndQuery || !m->Core.CmdCopyQueries)
	{
		AppendCpuRuntimeTrace(L"[NRI] GPU timestamp queries unsupported by interface");
		return;
	}

	nri::QueryPoolDesc queryDesc = {};
	queryDesc.queryType = nri::QueryType::TIMESTAMP;
	queryDesc.capacity = queryCount;
	if (m->Core.CreateQueryPool(*m->Device, queryDesc, m->TimestampQueryPool) != nri::Result::SUCCESS ||
		!m->TimestampQueryPool)
	{
		m->TimestampQueryPool = nullptr;
		AppendCpuRuntimeTrace(L"[NRI] CreateQueryPool(TIMESTAMP) failed");
		return;
	}

	m->TimestampQueryStride = std::max<uint32_t>(sizeof(uint64_t), m->Core.GetQuerySize(*m->TimestampQueryPool));
	const uint64_t readbackSize = static_cast<uint64_t>(m->TimestampQueryStride) * queryCount;
	if (!m->CreateBoundBuffer(readbackSize, 0, nri::BufferUsageBits::NONE,
		nri::MemoryLocation::HOST_READBACK, m->TimestampReadbackBuffer, m->TimestampReadbackMemory))
	{
		AppendCpuRuntimeTrace(L"[NRI] timestamp readback buffer allocation failed");
		ShutdownGpuTimestampQueries();
		return;
	}

	m->TimestampReadbackMapped = static_cast<uint8_t*>(m->Core.MapBuffer(*m->TimestampReadbackBuffer, 0, readbackSize));
	if (!m->TimestampReadbackMapped)
	{
		AppendCpuRuntimeTrace(L"[NRI] timestamp readback map failed");
		ShutdownGpuTimestampQueries();
		return;
	}

	std::memset(m->TimestampReadbackMapped, 0, static_cast<size_t>(readbackSize));
	if (!m->ResetQueries(m->TimestampQueryPool, 0, queryCount, "[NRI] timestamp query reset failed\n"))
	{
		AppendCpuRuntimeTrace(L"[NRI] timestamp query reset failed");
		ShutdownGpuTimestampQueries();
		return;
	}
	m->TimestampQueryCount = queryCount;
	AppendCpuRuntimeTrace(
		L"[NRI] GPU timestamp queries initialized count=" + std::to_wstring(queryCount) +
		L" stride=" + std::to_wstring(m->TimestampQueryStride) +
		L" frequency=" + std::to_wstring(GetTimestampFrequency()));
}
void NRIBackend::ShutdownGpuTimestampQueries()
{
	if (!m)
		return;
	if (m->TimestampReadbackMapped && m->TimestampReadbackBuffer)
	{
		m->Core.UnmapBuffer(*m->TimestampReadbackBuffer);
		m->TimestampReadbackMapped = nullptr;
	}
	if (m->TimestampReadbackBuffer)
	{
		m->FreeBuffer(m->TimestampReadbackBuffer, m->TimestampReadbackMemory);
		m->TimestampReadbackBuffer = nullptr;
	}
	else
	{
		m->TimestampReadbackMemory.clear();
	}
	if (m->TimestampQueryPool && m->Core.DestroyQueryPool)
	{
		m->Core.DestroyQueryPool(m->TimestampQueryPool);
		m->TimestampQueryPool = nullptr;
	}
	m->TimestampQueryCount = 0;
	m->TimestampQueryStride = 0;
}
void NRIBackend::WriteGpuTimestamp(uint32_t queryIndex)
{
	if (!m || !m->ActiveCmd || !m->TimestampQueryPool || queryIndex >= m->TimestampQueryCount)
		return;
	m->Core.CmdEndQuery(*m->ActiveCmd, *m->TimestampQueryPool, queryIndex);
}
void NRIBackend::ResolveGpuTimestampRange(uint32_t startQueryIndex, uint32_t queryCount)
{
	if (!m || !m->ActiveCmd || !m->TimestampQueryPool || !m->TimestampReadbackBuffer || queryCount == 0)
		return;
	if (startQueryIndex >= m->TimestampQueryCount || queryCount > m->TimestampQueryCount - startQueryIndex)
		return;

	m->EndRP();
	const uint64_t dstOffset = static_cast<uint64_t>(startQueryIndex) * m->TimestampQueryStride;
	m->Core.CmdCopyQueries(*m->ActiveCmd, *m->TimestampQueryPool, startQueryIndex, queryCount,
		*m->TimestampReadbackBuffer, dstOffset);
	m->Core.CmdResetQueries(*m->ActiveCmd, *m->TimestampQueryPool, startQueryIndex, queryCount);
}
uint64_t NRIBackend::ReadGpuTimestampValue(uint32_t queryIndex) const
{
	if (!m || !m->TimestampReadbackMapped || queryIndex >= m->TimestampQueryCount || m->TimestampQueryStride < sizeof(uint64_t))
		return 0;
	uint64_t value = 0;
	std::memcpy(&value, m->TimestampReadbackMapped + static_cast<size_t>(queryIndex) * m->TimestampQueryStride, sizeof(value));
	return value;
}
void NRIBackend::InitializeOcclusionQueries(uint32_t queryCount)
{
	if (!m || !m->Device)
		return;
	if (m->OcclusionQueryPool && m->OcclusionQueryCount == queryCount)
		return;

	ShutdownOcclusionQueries();
	if (queryCount == 0)
		return;

	if (!m->Core.CreateQueryPool || !m->Core.DestroyQueryPool || !m->Core.GetQuerySize ||
		!m->Core.CmdResetQueries ||
		!m->Core.CmdBeginQuery || !m->Core.CmdEndQuery || !m->Core.CmdCopyQueries)
	{
		AppendCpuRuntimeTrace(L"[NRI] occlusion queries unsupported by interface");
		return;
	}

	nri::QueryPoolDesc queryDesc = {};
	queryDesc.queryType = nri::QueryType::OCCLUSION;
	queryDesc.capacity = queryCount;
	if (m->Core.CreateQueryPool(*m->Device, queryDesc, m->OcclusionQueryPool) != nri::Result::SUCCESS ||
		!m->OcclusionQueryPool)
	{
		m->OcclusionQueryPool = nullptr;
		AppendCpuRuntimeTrace(L"[NRI] CreateQueryPool(OCCLUSION) failed");
		return;
	}

	m->OcclusionQueryStride = std::max<uint32_t>(sizeof(uint64_t), m->Core.GetQuerySize(*m->OcclusionQueryPool));
	const uint64_t readbackSize = static_cast<uint64_t>(m->OcclusionQueryStride) * queryCount;
	if (!m->CreateBoundBuffer(readbackSize, 0, nri::BufferUsageBits::NONE,
		nri::MemoryLocation::HOST_READBACK, m->OcclusionReadbackBuffer, m->OcclusionReadbackMemory))
	{
		AppendCpuRuntimeTrace(L"[NRI] occlusion readback buffer allocation failed");
		ShutdownOcclusionQueries();
		return;
	}

	m->OcclusionReadbackMapped = static_cast<uint8_t*>(m->Core.MapBuffer(*m->OcclusionReadbackBuffer, 0, readbackSize));
	if (!m->OcclusionReadbackMapped)
	{
		AppendCpuRuntimeTrace(L"[NRI] occlusion readback map failed");
		ShutdownOcclusionQueries();
		return;
	}

	for (uint32_t i = 0; i < queryCount; ++i)
	{
		const uint64_t visible = 1;
		std::memcpy(m->OcclusionReadbackMapped + static_cast<size_t>(i) * m->OcclusionQueryStride, &visible, sizeof(visible));
	}
	if (!m->ResetQueries(m->OcclusionQueryPool, 0, queryCount, "[NRI] occlusion query reset failed\n"))
	{
		AppendCpuRuntimeTrace(L"[NRI] occlusion query reset failed");
		ShutdownOcclusionQueries();
		return;
	}
	m->OcclusionQueryCount = queryCount;
	AppendCpuRuntimeTrace(
		L"[NRI] occlusion queries initialized count=" + std::to_wstring(queryCount) +
		L" stride=" + std::to_wstring(m->OcclusionQueryStride));
}
void NRIBackend::ShutdownOcclusionQueries()
{
	if (!m)
		return;
	if (m->OcclusionReadbackMapped && m->OcclusionReadbackBuffer)
	{
		m->Core.UnmapBuffer(*m->OcclusionReadbackBuffer);
		m->OcclusionReadbackMapped = nullptr;
	}
	if (m->OcclusionReadbackBuffer)
	{
		m->FreeBuffer(m->OcclusionReadbackBuffer, m->OcclusionReadbackMemory);
		m->OcclusionReadbackBuffer = nullptr;
	}
	else
	{
		m->OcclusionReadbackMemory.clear();
	}
	if (m->OcclusionQueryPool && m->Core.DestroyQueryPool)
	{
		m->Core.DestroyQueryPool(m->OcclusionQueryPool);
		m->OcclusionQueryPool = nullptr;
	}
	m->OcclusionQueryCount = 0;
	m->OcclusionQueryStride = 0;
}
void NRIBackend::BeginOcclusionQuery(uint32_t queryIndex)
{
	if (!m || !m->ActiveCmd || !m->OcclusionQueryPool || queryIndex >= m->OcclusionQueryCount)
		return;
	m->Core.CmdBeginQuery(*m->ActiveCmd, *m->OcclusionQueryPool, queryIndex);
}
void NRIBackend::EndOcclusionQuery(uint32_t queryIndex)
{
	if (!m || !m->ActiveCmd || !m->OcclusionQueryPool || queryIndex >= m->OcclusionQueryCount)
		return;
	m->Core.CmdEndQuery(*m->ActiveCmd, *m->OcclusionQueryPool, queryIndex);
}
void NRIBackend::ResolveOcclusionQueryRange(uint32_t startQueryIndex, uint32_t queryCount)
{
	if (!m || !m->ActiveCmd || !m->OcclusionQueryPool || !m->OcclusionReadbackBuffer || queryCount == 0)
		return;
	if (startQueryIndex >= m->OcclusionQueryCount)
		return;

	queryCount = std::min(queryCount, m->OcclusionQueryCount - startQueryIndex);
	m->EndRP();
	const uint64_t dstOffset = static_cast<uint64_t>(startQueryIndex) * m->OcclusionQueryStride;
	m->Core.CmdCopyQueries(*m->ActiveCmd, *m->OcclusionQueryPool, startQueryIndex, queryCount,
		*m->OcclusionReadbackBuffer, dstOffset);
	m->Core.CmdResetQueries(*m->ActiveCmd, *m->OcclusionQueryPool, startQueryIndex, queryCount);
}
uint64_t NRIBackend::ReadOcclusionQueryValue(uint32_t queryIndex) const
{
	if (!m || !m->OcclusionReadbackMapped || queryIndex >= m->OcclusionQueryCount || m->OcclusionQueryStride < sizeof(uint64_t))
		return 1;
	uint64_t value = 1;
	std::memcpy(&value, m->OcclusionReadbackMapped + static_cast<size_t>(queryIndex) * m->OcclusionQueryStride, sizeof(value));
	return value;
}

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
void NRIBackend::BindDefaultDescriptorHeaps() {}
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
	const bool profile = IsNriRecordProfileEnabled();
	NriCpuProfileScope drawScope(profile, m->RecordProfile.DrawMs, m->RecordProfile.DrawCount);
	m->OpenRP();
	if (m->CurrentGfx && !static_cast<NRIGraphicsPipeline*>(m->CurrentGfx)->ApplyForDraw())
	{
		++m->DbgDrawsSkipped;
		return;
	}
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
		m->LastVB = vertexBuffer; m->LastVBVerts = (uint32_t)std::max(0, vertexBuffer->numVertices); m->LastVBStride = vit->second.stride;
	}
	else { m->LastVB = nullptr; m->LastVBVerts = 0; m->LastVBStride = 0; }
	auto iit = m->IBs.find(indexBuffer);
	if (iit != m->IBs.end() && iit->second.buffer)
	{
		m->Core.CmdSetIndexBuffer(*m->ActiveCmd, *iit->second.buffer, 0, iit->second.indexType);
		m->LastIB = indexBuffer; m->LastIBIndices = indexBuffer ? (uint32_t)std::max(0, indexBuffer->numIndices) : 0;
		m->LastIBCapacity = iit->second.capacity; m->LastIBType = iit->second.indexType;
	}
	else { m->LastIB = nullptr; m->LastIBIndices = 0; m->LastIBCapacity = 0; }
}
void NRIBackend::DrawIndexed(uint32_t indexCount, uint32_t startIndexLocation, int32_t baseVertexLocation)
{
	if (!m->ActiveCmd || !m->RasterEnabled || !m->HasBoundGfx) { ++m->DbgDrawsSkipped; return; }
	const bool profile = IsNriRecordProfileEnabled();
	NriCpuProfileScope drawScope(profile, m->RecordProfile.DrawIndexedMs, m->RecordProfile.DrawIndexedCount);
	m->OpenRP();
	// Index-range OOB guard. A draw whose (baseIndex+indexCount) exceeds the bound
	// IB's index capacity fetches out-of-bounds — a classic GPU DEVICE_HUNG cause that
	// only fires once the camera brings the offending mesh into the cull set. Skip such
	// draws (visible gap, no crash) and count them; CORONA_NRI_DRAWLOG flushes a line
	// per draw so the LAST line before a freeze names the culprit mesh's params.
	const uint32_t idxSize = m->LastIBType == nri::IndexType::UINT16 ? 2u : 4u;
	const uint32_t ibCapIdx = m->LastIBCapacity / idxSize;
	const bool oob = (m->LastIB != nullptr) &&
		((uint64_t)startIndexLocation + indexCount > (ibCapIdx ? ibCapIdx : m->LastIBIndices));
	if (m->DrawLog)
	{
		std::ofstream l("nri_drawlog.log", std::ios::app);
		if (l) l << "DRAWIDX vb=" << (void*)m->LastVB << " verts=" << m->LastVBVerts << " stride=" << m->LastVBStride
			<< " ib=" << (void*)m->LastIB << " idxCap=" << ibCapIdx << " numIdx=" << m->LastIBIndices
			<< " baseIdx=" << startIndexLocation << " idxCount=" << indexCount << " baseVtx=" << baseVertexLocation
			<< (oob ? "  <<<OOB-SKIP" : "") << std::endl;
	}
	if (oob) { ++m->DbgOOBDraws; ++m->DbgDrawsSkipped; return; }
	if (m->CurrentGfx)
	{
		auto* gp = static_cast<NRIGraphicsPipeline*>(m->CurrentGfx);
		float worst = 0.0f;
		if (!gp->DebugTransformSane(worst))
		{
			++m->DbgBadXform; ++m->DbgDrawsSkipped;
			if (m->DrawLog) { std::ofstream l("nri_drawlog.log", std::ios::app); if (l) l << "DRAWIDX vb=" << (void*)m->LastVB << " verts=" << m->LastVBVerts << " idxCount=" << indexCount << " worstXform=" << worst << "  <<<BAD-XFORM-SKIP" << std::endl; }
			return;
		}
		if (!gp->ApplyForDraw())
		{
			++m->DbgDrawsSkipped;
			return;
		}
	}
	nri::DrawIndexedDesc dd = {}; dd.indexNum = indexCount; dd.instanceNum = 1; dd.baseIndex = startIndexLocation; dd.baseVertex = baseVertexLocation;
	m->Core.CmdDrawIndexed(*m->ActiveCmd, dd);
	++m->DbgDraws;
}

void NRIBackend::DrawInstanced(uint32_t vertexCountPerInstance, uint32_t instanceCount, uint32_t startVertexLocation, uint32_t startInstanceLocation)
{
	if (!m->ActiveCmd || !m->RasterEnabled || !m->HasBoundGfx) { ++m->DbgDrawsSkipped; return; }
	const bool profile = IsNriRecordProfileEnabled();
	NriCpuProfileScope drawScope(profile, m->RecordProfile.DrawMs, m->RecordProfile.DrawCount);
	m->OpenRP();
	if (m->CurrentGfx)
	{
		auto* gp = static_cast<NRIGraphicsPipeline*>(m->CurrentGfx);
		float worst = 0.0f;
		if (!gp->DebugTransformSane(worst))
		{
			++m->DbgBadXform; ++m->DbgDrawsSkipped;
			if (m->DrawLog) { std::ofstream l("nri_drawlog.log", std::ios::app); if (l) l << "DRAW vb=" << (void*)m->LastVB << " vertexCount=" << vertexCountPerInstance << " worstXform=" << worst << "  <<<BAD-XFORM-SKIP" << std::endl; }
			return;
		}
		if (!gp->ApplyForDraw())
		{
			++m->DbgDrawsSkipped;
			return;
		}
	}
	nri::DrawDesc dd = {};
	dd.vertexNum = vertexCountPerInstance;
	dd.instanceNum = instanceCount;
	dd.baseVertex = startVertexLocation;
	dd.baseInstance = startInstanceLocation;
	m->Core.CmdDraw(*m->ActiveCmd, dd);
	++m->DbgDraws;
}

void NRIBackend::DrawIndexedInstanced(uint32_t indexCountPerInstance, uint32_t instanceCount, uint32_t startIndexLocation, int32_t baseVertexLocation, uint32_t startInstanceLocation)
{
	if (!m->ActiveCmd || !m->RasterEnabled || !m->HasBoundGfx) { ++m->DbgDrawsSkipped; return; }
	const bool profile = IsNriRecordProfileEnabled();
	NriCpuProfileScope drawScope(profile, m->RecordProfile.DrawIndexedMs, m->RecordProfile.DrawIndexedCount);
	m->OpenRP();
	const uint32_t idxSize = m->LastIBType == nri::IndexType::UINT16 ? 2u : 4u;
	const uint32_t ibCapIdx = m->LastIBCapacity / idxSize;
	const bool idxOob = (m->LastIB != nullptr) &&
		((uint64_t)startIndexLocation + indexCountPerInstance > (ibCapIdx ? ibCapIdx : m->LastIBIndices));
	// A pathological instanceCount (stale/garbage per-frame count that grows with the
	// camera-driven cull/grid extent) makes the GPU process billions of primitives and
	// blows past the 2 s TDR — a DEVICE_HUNG that only fires on certain wide views. Cap
	// it; the cap is far above any legitimate per-draw instance count for this engine.
	const uint32_t kMaxInstances = 1u << 20; // 1,048,576
	const bool instOob = instanceCount > kMaxInstances;
	if (m->DrawLog)
	{
		std::ofstream l("nri_drawlog.log", std::ios::app);
		if (l) l << "DRAWIDXINST vb=" << (void*)m->LastVB << " verts=" << m->LastVBVerts << " stride=" << m->LastVBStride
			<< " ib=" << (void*)m->LastIB << " idxCap=" << ibCapIdx << " numIdx=" << m->LastIBIndices
			<< " baseIdx=" << startIndexLocation << " idxPerInst=" << indexCountPerInstance
			<< " instCount=" << instanceCount << " baseVtx=" << baseVertexLocation
			<< (idxOob ? "  <<<IDX-OOB-SKIP" : "") << (instOob ? "  <<<INST-OOB-SKIP" : "") << std::endl;
	}
	if (idxOob || instOob) { ++m->DbgOOBDraws; ++m->DbgDrawsSkipped; return; }
	if (m->CurrentGfx)
	{
		auto* gp = static_cast<NRIGraphicsPipeline*>(m->CurrentGfx);
		float worst = 0.0f;
		if (!gp->DebugTransformSane(worst))
		{
			++m->DbgBadXform; ++m->DbgDrawsSkipped;
			if (m->DrawLog) { std::ofstream l("nri_drawlog.log", std::ios::app); if (l) l << "DRAWIDXINST vb=" << (void*)m->LastVB << " instCount=" << instanceCount << " worstXform=" << worst << "  <<<BAD-XFORM-SKIP" << std::endl; }
			return;
		}
		if (!gp->ApplyForDraw())
		{
			++m->DbgDrawsSkipped;
			return;
		}
	}
	nri::DrawIndexedDesc dd = {}; dd.indexNum = indexCountPerInstance; dd.instanceNum = instanceCount; dd.baseIndex = startIndexLocation; dd.baseVertex = baseVertexLocation; dd.baseInstance = startInstanceLocation;
	m->Core.CmdDrawIndexed(*m->ActiveCmd, dd);
	++m->DbgDraws;
}
bool NRIBackend::DrawIndexedIndirect(Buffer* indirectArgumentBuffer, uint64_t byteOffset, uint32_t drawCount)
{
	static_assert(sizeof(DrawIndexedIndirectArguments) == sizeof(nri::DrawIndexedDesc), "Draw indexed indirect argument layout must match NRI.");
	if (drawCount == 0)
		return true;
	if (!m->ActiveCmd || !m->RasterEnabled || !m->HasBoundGfx || !indirectArgumentBuffer)
	{
		++m->DbgDrawsSkipped;
		return false;
	}
	const bool profile = IsNriRecordProfileEnabled();
	NriCpuProfileScope drawScope(profile, m->RecordProfile.DrawIndexedIndirectMs, m->RecordProfile.DrawIndexedIndirectCallCount);
	if (profile)
		m->RecordProfile.DrawIndexedIndirectDrawCount += drawCount;
	auto it = m->Buffers.find(indirectArgumentBuffer);
	if (it == m->Buffers.end() || !it->second.buffer)
		return false;
	const uint64_t argsBytes = static_cast<uint64_t>(drawCount) * sizeof(DrawIndexedIndirectArguments);
	const uint64_t bufferBytes = static_cast<uint64_t>(indirectArgumentBuffer->NumElements) * indirectArgumentBuffer->ElementSize;
	if (byteOffset > bufferBytes || argsBytes > bufferBytes - byteOffset)
		return false;

	m->OpenRP();
	if (m->CurrentGfx && !static_cast<NRIGraphicsPipeline*>(m->CurrentGfx)->ApplyForDraw())
	{
		++m->DbgDrawsSkipped;
		return false;
	}
	m->Core.CmdDrawIndexedIndirect(
		*m->ActiveCmd,
		*it->second.buffer,
		byteOffset,
		drawCount,
		sizeof(DrawIndexedIndirectArguments),
		nullptr,
		0);
	m->DbgDraws += drawCount;
	return true;
}

bool NRIBackend::DrawIndirect(Buffer* indirectArgumentBuffer, uint64_t byteOffset, uint32_t drawCount)
{
	static_assert(sizeof(DrawIndirectArguments) == sizeof(nri::DrawDesc), "Draw indirect argument layout must match NRI.");
	if (drawCount == 0)
		return true;
	if (!m->ActiveCmd || !m->RasterEnabled || !m->HasBoundGfx || !indirectArgumentBuffer)
	{
		++m->DbgDrawsSkipped;
		return false;
	}
	const bool profile = IsNriRecordProfileEnabled();
	NriCpuProfileScope drawScope(profile, m->RecordProfile.DrawIndirectMs, m->RecordProfile.DrawIndirectCallCount);
	if (profile)
		m->RecordProfile.DrawIndirectDrawCount += drawCount;
	auto it = m->Buffers.find(indirectArgumentBuffer);
	if (it == m->Buffers.end() || !it->second.buffer)
		return false;
	const uint64_t argsBytes = static_cast<uint64_t>(drawCount) * sizeof(DrawIndirectArguments);
	const uint64_t bufferBytes = static_cast<uint64_t>(indirectArgumentBuffer->NumElements) * indirectArgumentBuffer->ElementSize;
	if (byteOffset > bufferBytes || argsBytes > bufferBytes - byteOffset)
		return false;

	m->OpenRP();
	if (m->CurrentGfx && !static_cast<NRIGraphicsPipeline*>(m->CurrentGfx)->ApplyForDraw())
	{
		++m->DbgDrawsSkipped;
		return false;
	}
	m->Core.CmdDrawIndirect(
		*m->ActiveCmd,
		*it->second.buffer,
		byteOffset,
		drawCount,
		sizeof(DrawIndirectArguments),
		nullptr,
		0);
	m->DbgDraws += drawCount;
	return true;
}

void NRIBackend::Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
	if (m->ActiveCmd)
	{
		const bool profile = IsNriRecordProfileEnabled();
		NriCpuProfileScope dispatchScope(profile, m->RecordProfile.DispatchMs, m->RecordProfile.DispatchCount);
		m->EndRP();
		nri::DispatchDesc d = { groupCountX, groupCountY, groupCountZ };
		m->Core.CmdDispatch(*m->ActiveCmd, d);
	}
}
void NRIBackend::ClearTextureUAVFloat(Texture* texture, const float clearColor[4])
{
	if (!m || !m->ActiveCmd || !texture || !clearColor)
		return;
	auto it = m->Textures.find(texture);
	if (it == m->Textures.end() || !it->second.texture)
		return;

	const char* uavName = "OutTex4";
	const char* entryPoint = "ClearTextureFloat4";
	const wchar_t* shaderFile = L"Shaders\\NRITextureClear.hlsl";
	std::shared_ptr<ComputePipelineStateObject>* psoSlot = &m->ClearTextureUavFloat4Pso;
	switch (texture->Format)
	{
	case ETextureFormat::R32Float:
		uavName = "OutTex1";
		entryPoint = "ClearTextureFloat1";
		shaderFile = L"Shaders\\NRITextureClearFloat1.hlsl";
		psoSlot = &m->ClearTextureUavFloat1Pso;
		break;
	case ETextureFormat::RG16Float:
		uavName = "OutTex2";
		entryPoint = "ClearTextureFloat2";
		shaderFile = L"Shaders\\NRITextureClearFloat2.hlsl";
		psoSlot = &m->ClearTextureUavFloat2Pso;
		break;
	case ETextureFormat::RGBA16Float:
	case ETextureFormat::RGBA32Float:
	case ETextureFormat::RGBA8Unorm:
	case ETextureFormat::BGRA8Unorm:
		break;
	default:
	{
		static bool s_warnedUnsupportedClear = false;
		if (!s_warnedUnsupportedClear)
		{
			s_warnedUnsupportedClear = true;
			AppendCpuRuntimeTrace(L"[NRI] ClearTextureUAVFloat skipped unsupported texture format");
		}
		return;
	}
	}

	if (!*psoSlot)
	{
		std::shared_ptr<ComputePipelineStateObject> pso = CreateComputePipelineStateObject();
		if (!pso)
			return;
		pso->BindUAV(uavName, 0);
		pso->BindCBV("ClearParams", 0, 32);
		const std::wstring shaderPath = RuntimePaths::SourceFile(shaderFile).wstring();
		if (!pso->InitCS(shaderPath, entryPoint))
		{
			AppendCpuRuntimeTrace(L"[NRI] ClearTextureUAVFloat clear shader init failed");
			return;
		}
		*psoSlot = pso;
	}

	struct ClearParams
	{
		float ClearValue[4];
		uint32_t Extent[2];
		uint32_t Pad[2];
	};
	ClearParams params = {};
	params.ClearValue[0] = clearColor[0];
	params.ClearValue[1] = clearColor[1];
	params.ClearValue[2] = clearColor[2];
	params.ClearValue[3] = clearColor[3];
	params.Extent[0] = texture->Width;
	params.Extent[1] = texture->Height;

	(*psoSlot)->SetTextureUAV(uavName, texture);
	(*psoSlot)->SetCBVValue("ClearParams", &params);
	(*psoSlot)->Apply();
	Dispatch((texture->Width + 7u) / 8u, (texture->Height + 7u) / 8u, 1u);
	m->TransitionTex(texture, nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::Layout::SHADER_RESOURCE_STORAGE, nri::StageBits::ALL);
}
void NRIBackend::CopyTexture(Texture* dstTexture, Texture* srcTexture)
{
	if (!m || !m->ActiveCmd || !dstTexture || !srcTexture || dstTexture == srcTexture)
		return;

	auto srcIt = m->Textures.find(srcTexture);
	auto dstIt = m->Textures.find(dstTexture);
	if (srcIt == m->Textures.end() || dstIt == m->Textures.end() || !srcIt->second.texture || !dstIt->second.texture)
		return;

	m->EndRP();
	const nri::TextureDesc& srcDesc = m->Core.GetTextureDesc(*srcIt->second.texture);
	const nri::TextureDesc& dstDesc = m->Core.GetTextureDesc(*dstIt->second.texture);
	if (srcDesc.type != dstDesc.type || srcDesc.format != dstDesc.format ||
		srcDesc.width != dstDesc.width || srcDesc.height != dstDesc.height || srcDesc.depth != dstDesc.depth ||
		srcDesc.mipNum != dstDesc.mipNum || srcDesc.layerNum != dstDesc.layerNum)
	{
		static bool s_warnedCopyMismatch = false;
		if (!s_warnedCopyMismatch)
		{
			s_warnedCopyMismatch = true;
			AppendCpuRuntimeTrace(L"[NRI] CopyTexture skipped incompatible source/destination");
		}
		return;
	}
	if (!m->TexLayout.count(srcTexture) || m->TexLayout[srcTexture] != nri::Layout::COPY_SOURCE)
		m->TransitionTex(srcTexture, nri::AccessBits::COPY_SOURCE, nri::Layout::COPY_SOURCE, nri::StageBits::COPY);
	if (!m->TexLayout.count(dstTexture) || m->TexLayout[dstTexture] != nri::Layout::COPY_DESTINATION)
		m->TransitionTex(dstTexture, nri::AccessBits::COPY_DESTINATION, nri::Layout::COPY_DESTINATION, nri::StageBits::COPY);
	m->Core.CmdCopyTexture(*m->ActiveCmd, *dstIt->second.texture, nullptr, *srcIt->second.texture, nullptr);
}
void NRIBackend::ExecuteCurrentCommandList()
{
	if (!m || !m->ActiveCmd)
		return;
	m->EndRP();
}
void NRIBackend::BeginGpuMarker(uint64_t color, const char* label)
{
	if (!m || !m->ActiveCmd || !label)
		return;
	m->Core.CmdBeginAnnotation(*m->ActiveCmd, label, static_cast<uint32_t>(color));
}

void NRIBackend::EndGpuMarker()
{
	if (!m || !m->ActiveCmd)
		return;
	m->Core.CmdEndAnnotation(*m->ActiveCmd);
}
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
	it->second.access = bb.after;
	it->second.accessValid = true;
}
void NRIBackend::TransitionVertexBuffer(VertexBuffer* vertexBuffer, EResourceState stateBefore, EResourceState stateAfter)
{
	if (!m || !m->ActiveCmd || !vertexBuffer)
		return;
	auto it = m->VBs.find(vertexBuffer);
	if (it == m->VBs.end() || !it->second.buffer)
		return;
	m->EndRP();
	nri::BufferBarrierDesc bb = {};
	bb.buffer = it->second.buffer;
	bb.before = ToAccessStage(stateBefore);
	bb.after = ToAccessStage(stateAfter);
	nri::BarrierDesc bd = {};
	bd.buffers = &bb;
	bd.bufferNum = 1;
	m->Core.CmdBarrier(*m->ActiveCmd, bd);
	it->second.access = bb.after;
	it->second.accessValid = true;
}
void NRIBackend::UAVBarrier(Texture* texture)
{
	if (!m->ActiveCmd || !texture)
		return;
	nri::Texture* nt = m->NriTex(texture);
	if (!nt)
		return;
	const bool profile = IsNriRecordProfileEnabled();
	NriCpuProfileScope scope(profile, m->RecordProfile.TransitionTextureMs, m->RecordProfile.TransitionTextureCount);
	nri::TextureBarrierDesc tb = {};
	tb.texture = nt;
	tb.before.access = nri::AccessBits::SHADER_RESOURCE_STORAGE;
	tb.before.layout = nri::Layout::SHADER_RESOURCE_STORAGE;
	tb.before.stages = nri::StageBits::ALL;
	tb.after = tb.before;
	tb.mipNum = 1;
	tb.layerNum = 1;
	nri::BarrierDesc bd = {};
	bd.textures = &tb;
	bd.textureNum = 1;
	m->Core.CmdBarrier(*m->ActiveCmd, bd);
	m->TexLayout[texture] = nri::Layout::SHADER_RESOURCE_STORAGE;
}
void NRIBackend::UAVBarrier(Buffer* buffer)
{
	if (!m->ActiveCmd || !buffer) return;
	auto it = m->Buffers.find(buffer);
	if (it == m->Buffers.end() || !it->second.buffer) return;
	nri::BufferBarrierDesc bb = {};
	bb.buffer = it->second.buffer;
	bb.before = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::ALL };
	bb.after  = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::ALL };
	nri::BarrierDesc bd = {};
	bd.buffers = &bb; bd.bufferNum = 1;
	m->Core.CmdBarrier(*m->ActiveCmd, bd);
	it->second.access = bb.after;
	it->second.accessValid = true;
}

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
	const bool profile = IsNriRecordProfileEnabled();
	NriCpuProfileScope bindScope(profile, m->RecordProfile.BindGraphicsPipelineMs, m->RecordProfile.BindGraphicsPipelineCount);
	auto* p = static_cast<NRIGraphicsPipeline*>(pipeline);
	if (!p->GetPipeline()) { ++m->DbgGfxBindFail; return; }  // PSO failed to create — don't issue draws with no pipeline
	m->OpenRP();
	p->Bind();
	m->HasBoundGfx = true;
	++m->DbgGfxBindOk;
}
// Bind-group binding (explicit-binding RHI). A bind group is a batched set of
// by-name resource bindings for one draw; CreateGraphicsBindGroup copies the
// entries (owning constant bytes) and BindGraphicsBindGroup replays them onto the
// pipeline's existing by-name Set* state, which the next Draw* turns into a fresh
// per-draw NRI descriptor set in NRIGraphicsPipeline::ApplyForDraw.
struct NRIGraphicsBindGroup final : GraphicsBindGroupHandle
{
	struct Entry
	{
		EGraphicsBindGroupEntryType Type = EGraphicsBindGroupEntryType::TextureSRV;
		std::string Name;
		Texture* Tex = nullptr;
		Buffer* Buf = nullptr;
		VertexBuffer* Vb = nullptr;
		Sampler* Samp = nullptr;
		std::vector<uint8_t> Constant;
	};
	GraphicsPipelineHandle* Pipeline = nullptr;
	std::vector<Entry> Entries;
};

std::shared_ptr<GraphicsBindGroupHandle> NRIBackend::CreateGraphicsBindGroup(const GraphicsBindGroupDesc& desc)
{
	const bool profile = IsNriRecordProfileEnabled();
	NriCpuProfileScope createScope(profile, m->RecordProfile.CreateGraphicsBindGroupMs, m->RecordProfile.CreateGraphicsBindGroupCount);
	auto handle = std::make_shared<NRIGraphicsBindGroup>();
	handle->Pipeline = desc.Pipeline;
	handle->Entries.reserve(desc.Entries.size());
	for (const GraphicsBindGroupEntry& src : desc.Entries)
	{
		NRIGraphicsBindGroup::Entry e;
		e.Type = src.Type; e.Name = src.BindingName;
		e.Tex = src.TextureValue; e.Buf = src.BufferValue; e.Samp = src.SamplerValue;
		e.Vb = src.VertexBufferValue;
		if (src.Type == EGraphicsBindGroupEntryType::ConstantData && src.ConstantData && src.ConstantDataSize > 0)
		{
			const auto* bytes = static_cast<const uint8_t*>(src.ConstantData);
			e.Constant.assign(bytes, bytes + src.ConstantDataSize);
		}
		handle->Entries.push_back(std::move(e));
	}
	return handle;
}

void NRIBackend::BindGraphicsBindGroup(GraphicsPipelineHandle* pipeline, uint32_t, const std::shared_ptr<GraphicsBindGroupHandle>& bindGroup)
{
	const bool profile = IsNriRecordProfileEnabled();
	NriCpuProfileScope bindScope(profile, m->RecordProfile.BindGraphicsBindGroupMs, m->RecordProfile.BindGraphicsBindGroupCount);
	auto* p = static_cast<NRIGraphicsPipeline*>(pipeline);
	auto* bg = static_cast<NRIGraphicsBindGroup*>(bindGroup.get());
	if (!p || !bg) return;
	for (const NRIGraphicsBindGroup::Entry& e : bg->Entries)
	{
		switch (e.Type)
		{
		case EGraphicsBindGroupEntryType::TextureSRV:       if (e.Tex)  p->SetTexture(e.Name, e.Tex); break;
		case EGraphicsBindGroupEntryType::BufferSRV:        if (e.Buf)  p->SetBuffer(e.Name, e.Buf); break;
		case EGraphicsBindGroupEntryType::VertexBufferSRV:  if (e.Vb)   p->SetVertexBuffer(e.Name, e.Vb); break;
		case EGraphicsBindGroupEntryType::Sampler:          if (e.Samp) p->SetSampler(e.Name, e.Samp); break;
		case EGraphicsBindGroupEntryType::ConstantData:     if (!e.Constant.empty()) p->SetConstant(e.Constant.data(), (uint32_t)e.Constant.size()); break;
		}
	}
}

#endif // CORONA_HAS_NRI
