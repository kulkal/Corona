#include "DX12Backend.h"

#include <DirectXMath.h>
#include "DirectXTex.h"
#include "Utils.h"
#include "imgui_impl_dx12.h"
#include "d3dx12.h"
#define USE_PIX
#include "pix3.h"
#define GLM_FORCE_CTOR_INIT

#include "glm/glm.hpp"
#define GLM_ENABLE_EXPERIMENTAL
#include "glm/gtx/transform.hpp"
#include "glm/mat4x4.hpp"
#include "glm/fwd.hpp"
#include "Utils.h"
#include <dxcapi.use.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <tuple>

#include <assert.h>

#include <comdef.h>
#include <windows.h>
#include <wincodec.h>

// DirectX Tex
#include "DirectXTex July 2017/Include/DirectXTex.h"
#ifdef _DEBUG
#pragma comment(lib, "Debug/DirectXTex.lib")
#else
#pragma comment(lib, "Release/DirectXTex.lib")
#endif

#define align_to(_alignment, _val) (((_val + _alignment - 1) / _alignment) * _alignment)

using namespace std;

ComPtr<ID3DBlob> compileShaderDXC(DX12Backend* owner, const WCHAR* filename, const std::string& entryPoint, const WCHAR* targetString);
void AppendCpuRuntimeTrace(const std::wstring& line);

// ---------------------------------------------------------------------------
// On-disk pipeline binary cache.
//
// Two layers, both keyed by a 64-bit FNV-1a content hash and stored under
// bin/shadercache:
//   * .dxil files — compiled DXC bytecode, so repeat launches skip the
//                    (dominant) shader-compilation cost.
//   * .pso  files — ID3D12PipelineState::GetCachedBlob output, fed back through
//                    D3D12_CACHED_PIPELINE_STATE so the driver skips its own PSO
//                    compilation. A stale blob (driver/HW change) is detected by
//                    Create*PipelineState failing; we then recompile + rewrite.
//
// Keys fold in the resolved #include tree, entry point, target, defines, build
// config and the dxcompiler.dll identity, so editing any shader (root or
// header) or swapping the compiler invalidates the affected entries
// automatically. Set CORONA_DISABLE_SHADER_CACHE=1 to bypass entirely.
// ---------------------------------------------------------------------------
namespace PipelineCache
{
	// Bump when the on-disk format or compile flags change in a way that would
	// otherwise let a stale blob be reused.
	constexpr uint32_t kDxilCacheVersion = 1;
	constexpr uint32_t kPsoCacheVersion = 1;

	constexpr uint64_t kFnvOffset = 1469598103934665603ull;
	constexpr uint64_t kFnvPrime = 1099511628211ull;

	// Identity (size ^ write-time) of the loaded dxcompiler.dll, folded into
	// every DXIL key so a compiler upgrade invalidates the cache. Populated by
	// InitializeDxcCompiler; 0 until then (still safe — the version constant
	// differentiates formats regardless).
	uint64_t gCompilerStamp = 0;

	inline uint64_t HashBytes(const void* data, size_t size, uint64_t seed)
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
		if (size == 0)
			return;
		std::error_code ec;
		std::filesystem::create_directories(CacheDir(), ec);
		// Write to a temp file then rename so a crash mid-write can't leave a
		// truncated blob that a later run would treat as valid.
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

	// Recursively hash a shader source file and every file it #includes so any
	// edit to the translation unit (root or shared header) changes the key.
	uint64_t HashSourceTree(const std::filesystem::path& file, std::set<std::filesystem::path>& visited, uint64_t seed)
	{
		const std::filesystem::path canonical = file.lexically_normal();
		if (!visited.insert(canonical).second)
			return seed;

		std::ifstream stream(file, std::ios::binary);
		if (!stream.good())
			return seed; // missing file — let the real compile surface the error

		std::stringstream buffer;
		buffer << stream.rdbuf();
		const std::string content = buffer.str();

		uint64_t hash = HashBytes(content.data(), content.size(), seed);

		// Follow #include "relative/path" directives (quoted form only, which is
		// all Corona shaders use); angle-bracket system includes are skipped.
		size_t pos = 0;
		while ((pos = content.find("#include", pos)) != std::string::npos)
		{
			pos += 8;
			const size_t open = content.find('"', pos);
			if (open == std::string::npos)
				break;
			const size_t newline = content.find('\n', pos);
			if (newline != std::string::npos && open > newline)
			{
				pos = newline; // no quoted path on this line (e.g. #include <...>)
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

	uint64_t ShaderKey(
		const wchar_t* filename,
		const std::string& entryPoint,
		const wchar_t* target,
		const std::vector<std::pair<std::string, std::string>>& defines)
	{
		uint64_t key = kFnvOffset;
		key = HashBytes(&kDxilCacheVersion, sizeof(kDxilCacheVersion), key);
		key = HashBytes(&gCompilerStamp, sizeof(gCompilerStamp), key);
		{
			std::set<std::filesystem::path> visited;
			key = HashSourceTree(std::filesystem::path(filename), visited, key);
		}
		key = HashBytes(entryPoint.data(), entryPoint.size(), key);
		key = HashBytes(target, wcslen(target) * sizeof(wchar_t), key);
		for (const auto& define : defines)
		{
			key = HashBytes(define.first.data(), define.first.size(), key);
			key = HashBytes(define.second.data(), define.second.size(), key);
		}
		// Debug and Release builds pass different optimization flags to DXC.
#if defined(_DEBUG)
		const char config = 'D';
#else
		const char config = 'R';
#endif
		key = HashBytes(&config, sizeof(config), key);
		return key;
	}

	ComPtr<ID3DBlob> LoadDxil(uint64_t key)
	{
		if (!Enabled())
			return nullptr;
		std::vector<uint8_t> bytes;
		if (!ReadFile(CachePath(key, L".dxil"), bytes) || bytes.empty())
			return nullptr;
		ComPtr<ID3DBlob> blob;
		if (FAILED(D3DCreateBlob(bytes.size(), &blob)) || !blob)
			return nullptr;
		std::memcpy(blob->GetBufferPointer(), bytes.data(), bytes.size());
		return blob;
	}

	void StoreDxil(uint64_t key, ID3DBlob* blob)
	{
		if (!Enabled() || !blob || blob->GetBufferSize() == 0)
			return;
		WriteFile(CachePath(key, L".dxil"), blob->GetBufferPointer(), blob->GetBufferSize());
	}
}

namespace
{
	std::wstring ToWide(const std::string& value)
	{
		return std::wstring(value.begin(), value.end());
	}

	std::wstring FormatHexHRESULT(HRESULT hr)
	{
		std::wstringstream stream;
		stream << L"0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
		return stream.str();
	}

	bool IsDX12DeviceLostHRESULT(HRESULT hr)
	{
		return hr == DXGI_ERROR_DEVICE_REMOVED ||
			hr == DXGI_ERROR_DEVICE_HUNG ||
			hr == DXGI_ERROR_DEVICE_RESET ||
			hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
	}

	std::string NarrowAscii(const std::wstring& value)
	{
		std::string result;
		result.reserve(value.size());
		for (wchar_t ch : value)
			result.push_back((ch >= 0 && ch <= 0x7f) ? static_cast<char>(ch) : '?');
		return result;
	}

	const wchar_t* CommandListTypeName(D3D12_COMMAND_LIST_TYPE type)
	{
		switch (type)
		{
		case D3D12_COMMAND_LIST_TYPE_DIRECT: return L"direct";
		case D3D12_COMMAND_LIST_TYPE_COMPUTE: return L"compute";
		case D3D12_COMMAND_LIST_TYPE_COPY: return L"copy";
		case D3D12_COMMAND_LIST_TYPE_BUNDLE: return L"bundle";
		default: return L"unknown";
		}
	}

	void RestoreCoronaDescriptorHeaps(DX12Backend* owner, ID3D12GraphicsCommandList* commandList)
	{
		if (!owner || !commandList || !owner->SRVCBVDescriptorHeapShaderVisible || !owner->SamplerDescriptorHeapShaderVisible)
			return;

		ID3D12DescriptorHeap* ppHeaps[] =
		{
			owner->SRVCBVDescriptorHeapShaderVisible->DH.Get(),
			owner->SamplerDescriptorHeapShaderVisible->DH.Get()
		};
		commandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
	}
	void AppendD3D12InfoQueueMessages(ID3D12Device* device, const std::wstring& context)
	{
		if (!device)
			return;

		ComPtr<ID3D12InfoQueue> infoQueue;
		if (FAILED(device->QueryInterface(IID_PPV_ARGS(&infoQueue))) || !infoQueue)
			return;

		const UINT64 messageCount = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
		const UINT64 firstMessage = messageCount > 16 ? messageCount - 16 : 0;
		AppendCpuRuntimeTrace(
			L"[D3D12InfoQueue] context=\"" + context +
			L"\", storedMessages=" + std::to_wstring(messageCount));

		for (UINT64 messageIndex = firstMessage; messageIndex < messageCount; ++messageIndex)
		{
			SIZE_T messageLength = 0;
			if (FAILED(infoQueue->GetMessage(messageIndex, nullptr, &messageLength)) || messageLength == 0)
				continue;

			std::vector<char> messageData(messageLength);
			D3D12_MESSAGE* message = reinterpret_cast<D3D12_MESSAGE*>(messageData.data());
			if (FAILED(infoQueue->GetMessage(messageIndex, message, &messageLength)) || !message->pDescription)
				continue;

			AppendCpuRuntimeTrace(
				L"[D3D12InfoQueue] id=" + std::to_wstring(message->ID) +
				L", severity=" + std::to_wstring(message->Severity) +
				L", desc=\"" + ToWide(std::string(message->pDescription)) + L"\"");
		}
	}

	std::wstring DredName(const wchar_t* value)
	{
		return value && value[0] ? std::wstring(value) : L"<unnamed>";
	}

	const wchar_t* DredOpName(D3D12_AUTO_BREADCRUMB_OP op)
	{
		switch (op)
		{
		case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return L"MARKER";
		case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return L"BEGINEVENT";
		case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return L"ENDEVENT";
		case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return L"DRAW";
		case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return L"DRAWINDEXED";
		case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return L"EXECUTEINDIRECT";
		case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return L"DISPATCH";
		case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return L"COPYBUFFER";
		case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return L"COPYTEX";
		case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return L"COPYRESOURCE";
		case D3D12_AUTO_BREADCRUMB_OP_COPYTILES: return L"COPYTILES";
		case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE: return L"RESOLVE";
		case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return L"CLEARRTV";
		case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW: return L"CLEARUAV";
		case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return L"CLEARDSV";
		case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return L"BARRIER";
		case D3D12_AUTO_BREADCRUMB_OP_PRESENT: return L"PRESENT";
		case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA: return L"RESOLVEQUERY";
		case D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS: return L"DISPATCHRAYS";
		case D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE: return L"BUILD_AS";
		case D3D12_AUTO_BREADCRUMB_OP_EMITRAYTRACINGACCELERATIONSTRUCTUREPOSTBUILDINFO: return L"AS_POSTBUILD";
		case D3D12_AUTO_BREADCRUMB_OP_COPYRAYTRACINGACCELERATIONSTRUCTURE: return L"COPY_AS";
		case D3D12_AUTO_BREADCRUMB_OP_SETPIPELINESTATE1: return L"SETPSO1";
		default: return L"op";
		}
	}

	void AppendD3D12DredAllocationList(const D3D12_DRED_ALLOCATION_NODE* head, const wchar_t* label)
	{
		UINT logged = 0;
		for (const D3D12_DRED_ALLOCATION_NODE* node = head; node && logged < 8; node = node->pNext, ++logged)
		{
			AppendCpuRuntimeTrace(
				L"[D3D12DRED] " + std::wstring(label) +
				L" type=" + std::to_wstring(static_cast<UINT>(node->AllocationType)) +
				L", name=\"" + DredName(node->ObjectNameW) + L"\"");
		}
	}

	void AppendD3D12DredAllocationList1(const D3D12_DRED_ALLOCATION_NODE1* head, const wchar_t* label)
	{
		UINT logged = 0;
		for (const D3D12_DRED_ALLOCATION_NODE1* node = head; node && logged < 8; node = node->pNext, ++logged)
		{
			AppendCpuRuntimeTrace(
				L"[D3D12DRED] " + std::wstring(label) +
				L" type=" + std::to_wstring(static_cast<UINT>(node->AllocationType)) +
				L", name=\"" + DredName(node->ObjectNameW) + L"\"");
		}
	}

	void AppendD3D12DeviceRemovedData(ID3D12Device* device, const std::wstring& context)
	{
		if (!device)
			return;

		const HRESULT reason = device->GetDeviceRemovedReason();
		ComPtr<ID3D12DeviceRemovedExtendedData> dred;
		if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred))) || !dred)
			return;

		AppendCpuRuntimeTrace(
			L"[D3D12DRED] context=\"" + context +
			L"\", reason=" + FormatHexHRESULT(reason));

		ComPtr<ID3D12DeviceRemovedExtendedData1> dred1;
		if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dred1))) && dred1)
		{
			D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs1{};
			if (SUCCEEDED(dred1->GetAutoBreadcrumbsOutput1(&breadcrumbs1)))
			{
				UINT logged = 0;
				for (const D3D12_AUTO_BREADCRUMB_NODE1* node = breadcrumbs1.pHeadAutoBreadcrumbNode;
					node && logged < 8;
					node = node->pNext, ++logged)
				{
					const UINT done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
					const UINT total = node->BreadcrumbCount;
					const UINT faultIndex = done < total ? done : (total > 0 ? total - 1 : 0);
					const D3D12_AUTO_BREADCRUMB_OP faultOp =
						(node->pCommandHistory && total > 0) ?
						node->pCommandHistory[faultIndex] :
						static_cast<D3D12_AUTO_BREADCRUMB_OP>(0);
					AppendCpuRuntimeTrace(
						L"[D3D12DRED] breadcrumb1 cmdList=\"" + DredName(node->pCommandListDebugNameW) +
						L"\", queue=\"" + DredName(node->pCommandQueueDebugNameW) +
						L"\", count=" + std::to_wstring(total) +
						L", completed=" + std::to_wstring(done) +
						L", op=" + std::to_wstring(static_cast<UINT>(faultOp)) +
						L"(" + DredOpName(faultOp) + L")");

					if (node->pCommandHistory && total > 0)
					{
						const UINT lo = faultIndex > 8 ? faultIndex - 8 : 0;
						std::wstring seq;
						for (UINT i = lo; i <= faultIndex && i < total; ++i)
						{
							seq += std::to_wstring(i);
							seq += L":";
							seq += DredOpName(node->pCommandHistory[i]);
							seq += L" ";
						}
						AppendCpuRuntimeTrace(L"[D3D12DRED] ops " + seq);
					}
				}
			}

			D3D12_DRED_PAGE_FAULT_OUTPUT1 pageFault1{};
			if (SUCCEEDED(dred1->GetPageFaultAllocationOutput1(&pageFault1)))
			{
				std::wstringstream stream;
				stream << L"[D3D12DRED] pageFault1VA=0x" << std::hex << std::uppercase << pageFault1.PageFaultVA;
				AppendCpuRuntimeTrace(stream.str());
				AppendD3D12DredAllocationList1(pageFault1.pHeadExistingAllocationNode, L"existingAllocation1");
				AppendD3D12DredAllocationList1(pageFault1.pHeadRecentFreedAllocationNode, L"recentFreedAllocation1");
			}
			return;
		}

		D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs{};
		if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs)))
		{
			UINT logged = 0;
			for (const D3D12_AUTO_BREADCRUMB_NODE* node = breadcrumbs.pHeadAutoBreadcrumbNode;
				node && logged < 8;
				node = node->pNext, ++logged)
			{
				const UINT last = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
				const UINT lastOp =
					(node->pCommandHistory && node->BreadcrumbCount > 0 && last < node->BreadcrumbCount) ?
					static_cast<UINT>(node->pCommandHistory[last]) :
					0;
				AppendCpuRuntimeTrace(
					L"[D3D12DRED] breadcrumb cmdList=\"" + DredName(node->pCommandListDebugNameW) +
					L"\", queue=\"" + DredName(node->pCommandQueueDebugNameW) +
					L"\", count=" + std::to_wstring(node->BreadcrumbCount) +
					L", last=" + std::to_wstring(last) +
					L", lastOp=" + std::to_wstring(lastOp));
			}
		}

		D3D12_DRED_PAGE_FAULT_OUTPUT pageFault{};
		if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pageFault)))
		{
			std::wstringstream stream;
			stream << L"[D3D12DRED] pageFaultVA=0x" << std::hex << std::uppercase << pageFault.PageFaultVA;
			AppendCpuRuntimeTrace(stream.str());
			AppendD3D12DredAllocationList(pageFault.pHeadExistingAllocationNode, L"existingAllocation");
			AppendD3D12DredAllocationList(pageFault.pHeadRecentFreedAllocationNode, L"recentFreedAllocation");
		}
	}

	std::wstring ShaderDebugStem(const std::wstring& shaderPath)
	{
		if (shaderPath.empty())
			return L"UnknownShader";

		std::filesystem::path path(shaderPath);
		std::wstring stem = path.stem().wstring();
		if (stem.empty())
			stem = path.filename().wstring();
		return stem.empty() ? L"UnknownShader" : stem;
	}

	std::wstring ShaderDebugStem(const std::string& shaderPath)
	{
		return ShaderDebugStem(ToWide(shaderPath));
	}

	std::wstring MakePipelineDebugName(
		const wchar_t* pipelineType,
		const std::wstring& shaderPath,
		const std::string& primaryEntry,
		const std::string& secondaryEntry = {})
	{
		std::wstring name = std::wstring(pipelineType) + L": " + ShaderDebugStem(shaderPath);
		if (!primaryEntry.empty())
			name += L"." + ToWide(primaryEntry);
		if (!secondaryEntry.empty())
			name += L"/" + ToWide(secondaryEntry);
		return name;
	}

	std::wstring FormatDx12InitMilliseconds(double milliseconds)
	{
		std::wostringstream stream;
		stream << std::fixed << std::setprecision(3) << milliseconds;
		return stream.str();
	}

	std::wstring FormatDx12Hex(uint64_t value)
	{
		std::wostringstream stream;
		stream << L"0x" << std::hex << std::uppercase << value;
		return stream.str();
	}

	double ElapsedDx12InitMilliseconds(
		const std::chrono::steady_clock::time_point& begin,
		const std::chrono::steady_clock::time_point& end)
	{
		return std::chrono::duration<double, std::milli>(end - begin).count();
	}

	UINT AlignConstantBufferSize(UINT size)
	{
		return (size + 255u) & ~255u;
	}

	UINT64 AlignUploadOffset(UINT64 value, UINT64 alignment)
	{
		if (alignment <= 1)
			return value;
		return ((value + alignment - 1) / alignment) * alignment;
	}

	void CopyConstantBufferData(UINT8* destination, UINT destinationSize, const void* source, UINT sourceSize)
	{
		if (!destination || destinationSize == 0)
			return;

		std::memset(destination, 0, destinationSize);
		if (!source || sourceSize == 0)
			return;

		const UINT copySize = sourceSize < destinationSize ? sourceSize : destinationSize;
		std::memcpy(destination, source, copySize);
	}

	struct DX12GraphicsPipelineHandle final : GraphicsPipelineHandle
	{
		std::shared_ptr<PipelineStateObject> PSO;
		uint32_t ConstantBufferSize = 0;
		// Cached at create time so BindMeshBuffers can sanity-check the
		// bound VB stride against what the IA layout expects. DX12
		// itself reads the runtime stride from the VBV, but a mismatch
		// here means the same code will silently corrupt vertices on
		// Vulkan — log it so Vulkan-only bugs are caught on the desktop
		// dev cycle.
		uint32_t VertexStride = 0;
		std::wstring ShaderPathForDiag;
	};

	struct DX12GraphicsBindGroupEntry
	{
		EGraphicsBindGroupEntryType Type = EGraphicsBindGroupEntryType::TextureSRV;
		std::string BindingName;
		uint32_t Slot = 0;
		Texture* TextureValue = nullptr;
		Buffer* BufferValue = nullptr;
		VertexBuffer* VertexBufferValue = nullptr;
		Sampler* SamplerValue = nullptr;
		std::vector<uint8_t> ConstantData;
	};

	struct DX12GraphicsBindGroupHandle final : GraphicsBindGroupHandle
	{
		DX12GraphicsPipelineHandle* Pipeline = nullptr;
		std::vector<DX12GraphicsBindGroupEntry> Entries;
	};

	void ForceOpaqueAlpha(const DirectX::Image* image)
	{
		if (!image || !image->pixels)
			return;

		if (image->format != DXGI_FORMAT_B8G8R8A8_UNORM && image->format != DXGI_FORMAT_R8G8B8A8_UNORM)
			return;

		for (size_t y = 0; y < image->height; ++y)
		{
			uint8_t* row = image->pixels + y * image->rowPitch;
			for (size_t x = 0; x < image->width; ++x)
				row[x * 4 + 3] = 0xff;
		}
	}

	bool SaveScratchImagePNG(const DirectX::ScratchImage& captured, const std::wstring& filePath, std::wstring* errorMessage)
	{
		const DirectX::Image* image = captured.GetImage(0, 0, 0);
		if (!image)
		{
			if (errorMessage)
				*errorMessage = L"DX12 window capture image is empty.";
			return false;
		}

		HRESULT hr = S_OK;
		DirectX::ScratchImage pngImage;
		if (image->format == DXGI_FORMAT_B8G8R8A8_UNORM || image->format == DXGI_FORMAT_R8G8B8A8_UNORM)
		{
			hr = pngImage.InitializeFromImage(*image);
		}
		else
		{
			hr = DirectX::Convert(
				*image,
				DXGI_FORMAT_R8G8B8A8_UNORM,
				DirectX::TEX_FILTER_DEFAULT,
				DirectX::TEX_THRESHOLD_DEFAULT,
				pngImage);
		}

		const DirectX::Image* outputImage = SUCCEEDED(hr) ? pngImage.GetImage(0, 0, 0) : nullptr;
		if (!outputImage)
			hr = E_FAIL;

		if (SUCCEEDED(hr))
		{
			ForceOpaqueAlpha(outputImage);
			hr = SaveToWICFile(*outputImage, DirectX::WIC_FLAGS_NONE, GUID_ContainerFormatPng, filePath.c_str());
		}

		if (FAILED(hr))
		{
			if (errorMessage)
				*errorMessage = L"Failed to save DX12 window capture PNG.";
			return false;
		}

		return true;
	}

	DXGI_FORMAT ToDXGIFormat(ETextureFormat format)
	{
		switch (format)
		{
		case ETextureFormat::RGBA16Float: return DXGI_FORMAT_R16G16B16A16_FLOAT;
		case ETextureFormat::RGBA32Float: return DXGI_FORMAT_R32G32B32A32_FLOAT;
		case ETextureFormat::RG16Float: return DXGI_FORMAT_R16G16_FLOAT;
		case ETextureFormat::RGBA8Unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
		case ETextureFormat::BGRA8Unorm: return DXGI_FORMAT_B8G8R8A8_UNORM;
		case ETextureFormat::D32Float: return DXGI_FORMAT_D32_FLOAT;
		case ETextureFormat::R32Float: return DXGI_FORMAT_R32_FLOAT;
		case ETextureFormat::R8Uint: return DXGI_FORMAT_R8_UINT;
		default: return DXGI_FORMAT_R16G16B16A16_FLOAT;
		}
	}

	DXGI_FORMAT ToDXGIFormat(EVertexAttributeFormat format)
	{
		switch (format)
		{
		case EVertexAttributeFormat::Float2: return DXGI_FORMAT_R32G32_FLOAT;
		case EVertexAttributeFormat::Float3: return DXGI_FORMAT_R32G32B32_FLOAT;
		case EVertexAttributeFormat::Float4: return DXGI_FORMAT_R32G32B32A32_FLOAT;
		}
		return DXGI_FORMAT_R32G32B32A32_FLOAT;
	}

	DXGI_FORMAT ToDXGIFormat(EIndexFormat format)
	{
		return format == EIndexFormat::U16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
	}

	D3D12_RESOURCE_FLAGS ToD3D12ResourceFlags(ETextureUsageFlags usage)
	{
		D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
		if (HasTextureUsage(usage, TextureUsage_RenderTarget))
			flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		if (HasTextureUsage(usage, TextureUsage_UnorderedAccess))
			flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		if (HasTextureUsage(usage, TextureUsage_DepthStencil))
			flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
		return flags;
	}

	D3D12_RESOURCE_STATES ToD3D12ResourceState(EInitialResourceState state)
	{
		switch (state)
		{
		case EInitialResourceState::CopyDest:
			return D3D12_RESOURCE_STATE_COPY_DEST;
		case EInitialResourceState::GenericRead:
			return D3D12_RESOURCE_STATE_GENERIC_READ;
		case EInitialResourceState::ShaderRead:
		default:
			return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		}
	}

	D3D12_RESOURCE_STATES ToD3D12ResourceState(EResourceState state)
	{
		switch (state)
		{
		case EResourceState::RenderTarget:
			return D3D12_RESOURCE_STATE_RENDER_TARGET;
		case EResourceState::UnorderedAccess:
			return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		case EResourceState::Present:
			return D3D12_RESOURCE_STATE_PRESENT;
		case EResourceState::DepthWrite:
			return D3D12_RESOURCE_STATE_DEPTH_WRITE;
		case EResourceState::CopyDest:
			return D3D12_RESOURCE_STATE_COPY_DEST;
		case EResourceState::CopySource:
			return D3D12_RESOURCE_STATE_COPY_SOURCE;
		case EResourceState::VertexBuffer:
			return D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		case EResourceState::IndirectArgument:
			return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
		case EResourceState::ShaderRead:
		default:
			return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		}
	}

	D3D12_TEXTURE_ADDRESS_MODE ToD3D12AddressMode(ESamplerAddressMode mode)
	{
		switch (mode)
		{
		case ESamplerAddressMode::Clamp: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		case ESamplerAddressMode::Wrap:
		default: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		}
	}

	D3D12_FILTER ToD3D12Filter(ESamplerFilter filter)
	{
		switch (filter)
		{
		case ESamplerFilter::Anisotropic: return D3D12_FILTER_ANISOTROPIC;
		case ESamplerFilter::Linear:
		default: return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		}
	}

	D3D12_SHADER_RESOURCE_VIEW_DESC MakeTextureSRVDesc(const Texture& texture)
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Format = (texture.textureDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
			? DXGI_FORMAT_R32_FLOAT
			: texture.textureDesc.Format;

		if (texture.textureDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
		{
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
			srvDesc.Texture3D.MipLevels = texture.textureDesc.MipLevels;
		}
		else
		{
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = texture.textureDesc.MipLevels;
		}
		return srvDesc;
	}

}
void DescriptorHeap::Init(ID3D12Device5* InDevice, D3D12_DESCRIPTOR_HEAP_DESC& InHeapDesc)
{
	Device = InDevice;
	assert(Device);
	HeapDesc = InHeapDesc;
	MaxNumDescriptors = InHeapDesc.NumDescriptors;
	DescriptorSize = Device->GetDescriptorHandleIncrementSize(HeapDesc.Type);

	ThrowIfFailed(Device->CreateDescriptorHeap(&HeapDesc, IID_PPV_ARGS(&DH)));
	NAME_D3D12_OBJECT(DH);

	CPUHeapStart = DH->GetCPUDescriptorHandleForHeapStart().ptr;
	
	if(bShaderVisible)
		GPUHeapStart = DH->GetGPUDescriptorHandleForHeapStart().ptr;
}

void DescriptorHeap::AllocDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE& cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE& gpuHandle)
{
	if (NumAllocated >= MaxNumDescriptors)
		NumAllocated = 0;

	cpuHandle.ptr = CPUHeapStart + NumAllocated * DescriptorSize ;
	gpuHandle.ptr = GPUHeapStart + NumAllocated * DescriptorSize;

	NumAllocated++;
}

void DescriptorHeap::AllocDescriptors(D3D12_CPU_DESCRIPTOR_HANDLE& cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE& gpuHandle, UINT num)
{
	cpuHandle.ptr = CPUHeapStart + NumAllocated * DescriptorSize;
	gpuHandle.ptr = GPUHeapStart + NumAllocated * DescriptorSize;

	NumAllocated += num;
}

void DX12Backend::InvalidateGraphicsCommandStateCache()
{
	BoundGraphicsPipeline = nullptr;
	BoundGraphicsPipelineForDiag = nullptr;
	BoundVertexBuffer = nullptr;
	BoundIndexBuffer = nullptr;
	BoundPrimitiveTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
}

void DX12Backend::InvalidateComputeCommandStateCache()
{
	BoundComputeRootSignature = nullptr;
	BoundComputePipelineState = nullptr;
	BoundComputeRootDescriptorTables.clear();
}

void DX12Backend::InvalidateRayTracingCommandStateCache()
{
	BoundRayTracingPipelineState = nullptr;
}

void DX12Backend::SetComputeRootSignatureIfNeeded(ID3D12GraphicsCommandList* commandList, ID3D12RootSignature* rootSignature)
{
	if (!commandList || !rootSignature)
		return;
	if (BoundComputeRootSignature == rootSignature)
		return;
	commandList->SetComputeRootSignature(rootSignature);
	BoundComputeRootSignature = rootSignature;
	BoundComputeRootDescriptorTables.clear();
}

void DX12Backend::SetComputePipelineStateIfNeeded(ID3D12GraphicsCommandList* commandList, ID3D12PipelineState* pipelineState)
{
	if (!commandList || !pipelineState)
		return;
	if (BoundComputePipelineState == pipelineState)
		return;
	commandList->SetPipelineState(pipelineState);
	BoundComputePipelineState = pipelineState;
	InvalidateRayTracingCommandStateCache();
}

void DX12Backend::SetComputeRootDescriptorTableIfNeeded(ID3D12GraphicsCommandList* commandList, UINT rootParamIndex, D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle)
{
	if (!commandList)
		return;
	const UINT64 kUnboundDescriptorTable = std::numeric_limits<UINT64>::max();
	if (BoundComputeRootDescriptorTables.size() <= rootParamIndex)
		BoundComputeRootDescriptorTables.resize(static_cast<size_t>(rootParamIndex) + 1, kUnboundDescriptorTable);
	if (BoundComputeRootDescriptorTables[rootParamIndex] == gpuHandle.ptr)
		return;
	commandList->SetComputeRootDescriptorTable(rootParamIndex, gpuHandle);
	BoundComputeRootDescriptorTables[rootParamIndex] = gpuHandle.ptr;
}

void DX12Backend::SetRayTracingPipelineStateIfNeeded(ID3D12GraphicsCommandList4* commandList, ID3D12StateObject* pipelineState)
{
	if (!commandList || !pipelineState)
		return;
	if (BoundRayTracingPipelineState == pipelineState)
		return;
	commandList->SetPipelineState1(pipelineState);
	BoundRayTracingPipelineState = pipelineState;
	BoundComputePipelineState = nullptr;
}

void DX12Backend::MarkDeviceLost(const wchar_t* context, HRESULT hr)
{
	const HRESULT deviceRemovedReason = Device ? Device->GetDeviceRemovedReason() : S_OK;
	if (deviceRemovedReason == S_OK && !IsDX12DeviceLostHRESULT(hr))
		return;

	const std::wstring safeContext = context ? std::wstring(context) : L"unknown";
	const std::wstring message =
		L"DX12 device lost at " + safeContext +
		L" hr=" + FormatHexHRESULT(hr) +
		L", reason=" + FormatHexHRESULT(deviceRemovedReason);

	if (!bDeviceLost)
	{
		bDeviceLost = true;
		errorString = NarrowAscii(message);
		AppendCpuRuntimeTrace(L"[DX12DeviceLost] " + message);
	}

	if (!bDeviceLostDiagnosticsLogged)
	{
		bDeviceLostDiagnosticsLogged = true;
		AppendD3D12InfoQueueMessages(Device.Get(), L"DX12Backend::DeviceLost " + safeContext);
		AppendD3D12DeviceRemovedData(Device.Get(), L"DX12Backend::DeviceLost " + safeContext);
	}
}

void DX12Backend::BeginFrame()
{
	if (bDeviceLost)
		return;
	if (Device)
	{
		const HRESULT deviceRemovedReason = Device->GetDeviceRemovedReason();
		if (deviceRemovedReason != S_OK)
		{
			MarkDeviceLost(L"DX12Backend::BeginFrame", deviceRemovedReason);
			return;
		}
	}

	// Reclaim texture-upload staging heaps whose GPU copy has finished.
	RetireCompletedTextureUploads();

	CurrentFrameIndex = m_swapChain->GetCurrentBackBufferIndex();

	// wait until gpu processing for this frame resource is completed
	UINT64 ThisFrameFenceValue = FrameFenceValueVec[CurrentFrameIndex];
	
	CmdQ->WaitFenceValue(ThisFrameFenceValue);
	RetireCompletedPersistentStructuredBufferUploads();
	RetireCompletedPersistentStructuredBufferFrees();
	ResetTransientUploadStructuredFrame(CurrentFrameIndex);
	RecycleTransientDefaultStructuredFrame(CurrentFrameIndex);

	BeginNewGraphicsCommandList();
	

	GlobalDHRing->Advance();

	GlobalCBRing->Advance();

	for (auto& tex : DynamicTextures)
	{
		if (tex->textureDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
		{
			GlobalDHRing->AllocDescriptor(tex->CpuHandleUAV, tex->GpuHandleUAV);


			D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Format = tex->textureDesc.Format;

			Device->CreateUnorderedAccessView(tex->resource.Get(), nullptr, &uavDesc, tex->CpuHandleUAV);
		}

		// alloc dh and create srv
		GlobalDHRing->AllocDescriptor(tex->CpuHandleSRV, tex->GpuHandleSRV);

		D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc = {};
		SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		if (tex->textureDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
			SrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
		else
			SrvDesc.Format = tex->textureDesc.Format;

		SrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		SrvDesc.Texture2D.MipLevels = tex->textureDesc.MipLevels;
		Device->CreateShaderResourceView(tex->resource.Get(), &SrvDesc, tex->CpuHandleSRV);
	}

	for (auto& buffer : DynamicBuffers)
	{
		GlobalDHRing->AllocDescriptor(buffer->CpuHandleUAV, buffer->GpuHandleUAV);

		if (buffer->Type == Buffer::BYTE_ADDRESS)
		{
			D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
			uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
			uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
			uavDesc.Buffer.NumElements = buffer->NumElements;

			Device->CreateUnorderedAccessView(buffer->resource.Get(), nullptr, &uavDesc, buffer->CpuHandleUAV);
		}
		else if (buffer->Type == Buffer::STRUCTURED)
		{
			D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
			uavDesc.Format = DXGI_FORMAT_UNKNOWN;
			uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
			uavDesc.Buffer.StructureByteStride = buffer->ElementSize;
			uavDesc.Buffer.NumElements = buffer->NumElements;

			Device->CreateUnorderedAccessView(buffer->resource.Get(), nullptr, &uavDesc, buffer->CpuHandleUAV);
		}
	}

}

void DX12Backend::EndFrame()
{
	if (bDeviceLost)
		return;

	const UINT presentFlags = bTearingSupported ? DXGI_PRESENT_ALLOW_TEARING : 0;
#if USE_AFTERMATH
	GFSDK_Aftermath_ContextHandle activeAftermathContext =
		(GlobalCmdList && GlobalCmdList->AftermathContext) ? GlobalCmdList->AftermathContext : nullptr;
#endif
	const HRESULT presentHr = m_swapChain->Present(0, presentFlags);
	if (FAILED(presentHr))
	{
		const HRESULT deviceRemovedReason = Device ? Device->GetDeviceRemovedReason() : S_OK;
		AppendCpuRuntimeTrace(
			L"[DX12Backend][Present] failed hr=" + FormatHexHRESULT(presentHr) +
			L", deviceRemovedReason=" + FormatHexHRESULT(deviceRemovedReason));
		MarkDeviceLost(L"DX12Backend::EndFrame Present", presentHr);
		if (bDeviceLost)
			return;
		AppendD3D12InfoQueueMessages(Device.Get(), L"DX12Backend::EndFrame Present");
	}
#if USE_AFTERMATH
	ThrowIfFailed(presentHr, activeAftermathContext ? &activeAftermathContext : nullptr);
#else
	ThrowIfFailed(presentHr, nullptr);
#endif
	FrameFenceValueVec[CurrentFrameIndex] = CmdQ->CurrentFenceValue;;
	CmdQ->SignalCurrentFence();
}

shared_ptr<Sampler> DX12Backend::CreateSampler(const SamplerCreateDesc& desc)
{
	D3D12_SAMPLER_DESC samplerDesc = {};
	samplerDesc.Filter = ToD3D12Filter(desc.Filter);
	samplerDesc.AddressU = ToD3D12AddressMode(desc.AddressU);
	samplerDesc.AddressV = ToD3D12AddressMode(desc.AddressV);
	samplerDesc.AddressW = ToD3D12AddressMode(desc.AddressW);
	samplerDesc.MinLOD = desc.MinLOD;
	samplerDesc.MaxLOD = desc.MaxLOD;
	samplerDesc.MipLODBias = desc.MipLODBias;
	samplerDesc.MaxAnisotropy = desc.MaxAnisotropy;
	samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	return CreateSampler(samplerDesc);
}

std::shared_ptr<Texture> DX12Backend::WrapNativeTexture(const Microsoft::WRL::ComPtr<ID3D12Resource>& resource)
{
	return CreateTexture2DFromResource(resource);
}

void DX12Backend::CreateSwapChainForWindow(WindowHandle window, uint32_t width, uint32_t height, ETextureFormat format)
{
	HWND hwnd = static_cast<HWND>(window.PlatformHandle);
	bTearingSupported = false;
	IDXGIFactory4* factory = ExternalDXGIFactory;
	ComPtr<IDXGIFactory4> ownedFactory;
	if (!factory)
	{
		ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&ownedFactory)));
		factory = ownedFactory.Get();
	}
	ComPtr<IDXGIFactory5> factory5;
	if (factory && SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory5))))
	{
		BOOL allowTearing = FALSE;
		if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing))))
			bTearingSupported = allowTearing == TRUE;
	}

	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
	swapChainDesc.BufferCount = GetFrameCount();
	swapChainDesc.Width = width;
	swapChainDesc.Height = height;
	swapChainDesc.Format = ToDXGIFormat(format);
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swapChainDesc.SampleDesc.Count = 1;
	swapChainDesc.Flags = bTearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

	ComPtr<IDXGISwapChain1> swapChain;
	ThrowIfFailed(factory->CreateSwapChainForHwnd(
		CmdQ->CmdQueue.Get(),
		hwnd,
		&swapChainDesc,
		nullptr,
		nullptr,
		&swapChain));

	ThrowIfFailed(factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER));
	ThrowIfFailed(swapChain.As(&m_swapChain));
	SwapChainRenderTargets.clear();
	SwapChainWrappedTextures.clear();
}

std::shared_ptr<Texture> DX12Backend::GetSwapChainTexture(uint32_t bufferIndex)
{
	if (bufferIndex < SwapChainWrappedTextures.size() && SwapChainWrappedTextures[bufferIndex])
		return SwapChainWrappedTextures[bufferIndex];

	ComPtr<ID3D12Resource> renderTarget;
	ThrowIfFailed(m_swapChain->GetBuffer(bufferIndex, IID_PPV_ARGS(&renderTarget)));
	std::shared_ptr<Texture> wrapped = CreateTexture2DFromResource(renderTarget);
	if (wrapped)
		wrapped->MakeRTV();
	if (bufferIndex >= SwapChainWrappedTextures.size())
		SwapChainWrappedTextures.resize(bufferIndex + 1);
	SwapChainWrappedTextures[bufferIndex] = wrapped;
	return wrapped;
}

namespace
{
	ETextureFormat FromDXGIFormat(DXGI_FORMAT f)
	{
		switch (f)
		{
		case DXGI_FORMAT_R16G16B16A16_FLOAT: return ETextureFormat::RGBA16Float;
		case DXGI_FORMAT_R32G32B32A32_FLOAT: return ETextureFormat::RGBA32Float;
		case DXGI_FORMAT_R16G16_FLOAT: return ETextureFormat::RG16Float;
		case DXGI_FORMAT_R8G8B8A8_UNORM: return ETextureFormat::RGBA8Unorm;
		case DXGI_FORMAT_B8G8R8A8_UNORM: return ETextureFormat::BGRA8Unorm;
		case DXGI_FORMAT_D32_FLOAT: return ETextureFormat::D32Float;
		case DXGI_FORMAT_R32_FLOAT: return ETextureFormat::R32Float;
		case DXGI_FORMAT_R8_UINT: return ETextureFormat::R8Uint;
		default: return ETextureFormat::RGBA8Unorm;
		}
	}
}

bool DX12Backend::CaptureTexture(Texture* source, CapturedImage& captured, EResourceState beforeState)
{
	if (!source || !CmdQ)
		return false;

	const D3D12_RESOURCE_STATES nativeState = ToD3D12ResourceState(beforeState);
	DirectX::ScratchImage scratch;
	const HRESULT hr = DirectX::CaptureTexture(CmdQ->CmdQueue.Get(), source->resource.Get(), false, scratch, nativeState, nativeState);
	if (FAILED(hr))
		return false;

	const DirectX::Image* image = scratch.GetImage(0, 0, 0);
	if (!image)
		return false;

	captured.Format = FromDXGIFormat(image->format);
	captured.Width = static_cast<uint32_t>(image->width);
	captured.Height = static_cast<uint32_t>(image->height);
	captured.RowPitch = static_cast<uint32_t>(image->rowPitch);
	captured.Pixels.assign(image->pixels, image->pixels + image->slicePitch);
	return true;
}

void DX12Backend::InitializeImGuiBackend(WindowHandle window, ETextureFormat rtvFormat)
{
	(void)window;
	TextureDHRing->AllocDescriptor(CpuHandleImguiFontTex, GpuHandleImguiFontTex);

	ImGui_ImplDX12_InitInfo initInfo = {};
	initInfo.Device = Device.Get();
	initInfo.CommandQueue = CmdQ->CmdQueue.Get();
	initInfo.NumFramesInFlight = GetFrameCount();
	initInfo.RTVFormat = ToDXGIFormat(rtvFormat);
	initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
	initInfo.SrvDescriptorHeap = SRVCBVDescriptorHeapShaderVisible->DH.Get();
	initInfo.LegacySingleSrvCpuDescriptor = CpuHandleImguiFontTex;
	initInfo.LegacySingleSrvGpuDescriptor = GpuHandleImguiFontTex;
	ImGui_ImplDX12_Init(&initInfo);
}

void DX12Backend::NewImGuiFrame()
{
	ImGui_ImplDX12_NewFrame();
}

void DX12Backend::RenderImGuiDrawData(ImDrawData* drawData)
{
	ImGui_ImplDX12_RenderDrawData(drawData, GlobalCmdList->CmdList.Get());
}

void DX12Backend::ShutdownImGuiBackend()
{
	ImGui_ImplDX12_Shutdown();
}

void DX12Backend::EmitGpuCrashMarker(const char* markerName)
{
#if USE_AFTERMATH
	if (bAftermathEnabled && markerName && GlobalCmdList && GlobalCmdList->AftermathContext)
	{
		NVAftermathMarker(GlobalCmdList->AftermathContext, markerName);
	}
#else
	(void)markerName;
#endif
}

void DX12Backend::InitializeGpuTimestampQueries(uint32_t queryCount)
{
	if (GpuTimestampQueryHeap && GpuTimestampQueryCount == queryCount)
		return;

	ShutdownGpuTimestampQueries();

	D3D12_QUERY_HEAP_DESC queryHeapDesc = {};
	queryHeapDesc.Count = queryCount;
	queryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
	ThrowIfFailed(Device->CreateQueryHeap(&queryHeapDesc, IID_PPV_ARGS(&GpuTimestampQueryHeap)));

	const UINT64 readbackSize = sizeof(UINT64) * queryCount;
	D3D12_HEAP_PROPERTIES heapProps = {};
	heapProps.Type = D3D12_HEAP_TYPE_READBACK;
	heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	heapProps.CreationNodeMask = 1;
	heapProps.VisibleNodeMask = 1;

	D3D12_RESOURCE_DESC bufferDesc = {};
	bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufferDesc.Width = readbackSize;
	bufferDesc.Height = 1;
	bufferDesc.DepthOrArraySize = 1;
	bufferDesc.MipLevels = 1;
	bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
	bufferDesc.SampleDesc.Count = 1;
	bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	ThrowIfFailed(Device->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_NONE,
		&bufferDesc,
		D3D12_RESOURCE_STATE_COPY_DEST,
		nullptr,
		IID_PPV_ARGS(&GpuTimestampReadbackBuffer)));

	CD3DX12_RANGE readRange(0, static_cast<SIZE_T>(readbackSize));
	ThrowIfFailed(GpuTimestampReadbackBuffer->Map(0, &readRange, reinterpret_cast<void**>(&GpuTimestampReadbackMapped)));
	std::fill_n(GpuTimestampReadbackMapped, queryCount, 0ull);
	GpuTimestampQueryCount = queryCount;
}

void DX12Backend::ShutdownGpuTimestampQueries()
{
	if (GpuTimestampReadbackBuffer && GpuTimestampReadbackMapped)
	{
		GpuTimestampReadbackBuffer->Unmap(0, nullptr);
		GpuTimestampReadbackMapped = nullptr;
	}

	GpuTimestampReadbackBuffer.Reset();
	GpuTimestampQueryHeap.Reset();
	GpuTimestampQueryCount = 0;
}

void DX12Backend::WriteGpuTimestamp(uint32_t queryIndex)
{
	if (!GpuTimestampQueryHeap)
		return;

	GlobalCmdList->CmdList->EndQuery(GpuTimestampQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryIndex);
}

void DX12Backend::ResolveGpuTimestampRange(uint32_t startQueryIndex, uint32_t queryCount)
{
	if (!GpuTimestampQueryHeap || !GpuTimestampReadbackBuffer || queryCount == 0)
		return;

	const UINT64 bufferOffset = static_cast<UINT64>(startQueryIndex) * sizeof(UINT64);
	GlobalCmdList->CmdList->ResolveQueryData(
		GpuTimestampQueryHeap.Get(),
		D3D12_QUERY_TYPE_TIMESTAMP,
		startQueryIndex,
		queryCount,
		GpuTimestampReadbackBuffer.Get(),
		bufferOffset);
}

uint64_t DX12Backend::ReadGpuTimestampValue(uint32_t queryIndex) const
{
	if (!GpuTimestampReadbackMapped || queryIndex >= GpuTimestampQueryCount)
		return 0;

	return GpuTimestampReadbackMapped[queryIndex];
}

void DX12Backend::InitializeOcclusionQueries(uint32_t queryCount)
{
	if (OcclusionQueryHeap && OcclusionQueryCount == queryCount)
		return;

	ShutdownOcclusionQueries();
	if (queryCount == 0)
		return;

	D3D12_QUERY_HEAP_DESC queryHeapDesc = {};
	queryHeapDesc.Count = queryCount;
	queryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
	HRESULT hr = Device->CreateQueryHeap(&queryHeapDesc, IID_PPV_ARGS(&OcclusionQueryHeap));
	if (FAILED(hr))
	{
		AppendCpuRuntimeTrace(
			L"[DX12Backend::InitializeOcclusionQueries] CreateQueryHeap failed hr=" +
			FormatHexHRESULT(hr) +
			L", queryCount=" + std::to_wstring(queryCount));
		AppendD3D12InfoQueueMessages(Device.Get(), L"InitializeOcclusionQueries CreateQueryHeap");
		ShutdownOcclusionQueries();
		return;
	}

	const UINT64 readbackSize = sizeof(UINT64) * queryCount;
	D3D12_HEAP_PROPERTIES heapProps = {};
	heapProps.Type = D3D12_HEAP_TYPE_READBACK;
	heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	heapProps.CreationNodeMask = 1;
	heapProps.VisibleNodeMask = 1;

	D3D12_RESOURCE_DESC bufferDesc = {};
	bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufferDesc.Width = readbackSize;
	bufferDesc.Height = 1;
	bufferDesc.DepthOrArraySize = 1;
	bufferDesc.MipLevels = 1;
	bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
	bufferDesc.SampleDesc.Count = 1;
	bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	hr = Device->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_NONE,
		&bufferDesc,
		D3D12_RESOURCE_STATE_COPY_DEST,
		nullptr,
		IID_PPV_ARGS(&OcclusionReadbackBuffer));
	if (FAILED(hr))
	{
		AppendCpuRuntimeTrace(
			L"[DX12Backend::InitializeOcclusionQueries] readback buffer allocation failed hr=" +
			FormatHexHRESULT(hr) +
			L", queryCount=" + std::to_wstring(queryCount) +
			L", bytes=" + std::to_wstring(readbackSize));
		AppendD3D12InfoQueueMessages(Device.Get(), L"InitializeOcclusionQueries ReadbackBuffer");
		ShutdownOcclusionQueries();
		return;
	}

	CD3DX12_RANGE readRange(0, static_cast<SIZE_T>(readbackSize));
	hr = OcclusionReadbackBuffer->Map(0, &readRange, reinterpret_cast<void**>(&OcclusionReadbackMapped));
	if (FAILED(hr) || !OcclusionReadbackMapped)
	{
		AppendCpuRuntimeTrace(
			L"[DX12Backend::InitializeOcclusionQueries] readback map failed hr=" +
			FormatHexHRESULT(hr) +
			L", queryCount=" + std::to_wstring(queryCount) +
			L", bytes=" + std::to_wstring(readbackSize));
		AppendD3D12InfoQueueMessages(Device.Get(), L"InitializeOcclusionQueries Map");
		ShutdownOcclusionQueries();
		return;
	}
	std::fill_n(OcclusionReadbackMapped, queryCount, 1ull);
	OcclusionQueryCount = queryCount;
}

void DX12Backend::ShutdownOcclusionQueries()
{
	if (OcclusionReadbackBuffer && OcclusionReadbackMapped)
	{
		OcclusionReadbackBuffer->Unmap(0, nullptr);
		OcclusionReadbackMapped = nullptr;
	}

	OcclusionReadbackBuffer.Reset();
	OcclusionQueryHeap.Reset();
	OcclusionQueryCount = 0;
}

void DX12Backend::BeginOcclusionQuery(uint32_t queryIndex)
{
	if (!OcclusionQueryHeap || !GlobalCmdList || queryIndex >= OcclusionQueryCount)
		return;

	GlobalCmdList->CmdList->BeginQuery(OcclusionQueryHeap.Get(), D3D12_QUERY_TYPE_BINARY_OCCLUSION, queryIndex);
}

void DX12Backend::EndOcclusionQuery(uint32_t queryIndex)
{
	if (!OcclusionQueryHeap || !GlobalCmdList || queryIndex >= OcclusionQueryCount)
		return;

	GlobalCmdList->CmdList->EndQuery(OcclusionQueryHeap.Get(), D3D12_QUERY_TYPE_BINARY_OCCLUSION, queryIndex);
}

void DX12Backend::ResolveOcclusionQueryRange(uint32_t startQueryIndex, uint32_t queryCount)
{
	if (!OcclusionQueryHeap || !OcclusionReadbackBuffer || !GlobalCmdList || queryCount == 0 || startQueryIndex >= OcclusionQueryCount)
		return;

	queryCount = std::min(queryCount, OcclusionQueryCount - startQueryIndex);
	const UINT64 bufferOffset = static_cast<UINT64>(startQueryIndex) * sizeof(UINT64);
	GlobalCmdList->CmdList->ResolveQueryData(
		OcclusionQueryHeap.Get(),
		D3D12_QUERY_TYPE_BINARY_OCCLUSION,
		startQueryIndex,
		queryCount,
		OcclusionReadbackBuffer.Get(),
		bufferOffset);
}

uint64_t DX12Backend::ReadOcclusionQueryValue(uint32_t queryIndex) const
{
	if (!OcclusionReadbackMapped || queryIndex >= OcclusionQueryCount)
		return 1;

	return OcclusionReadbackMapped[queryIndex];
}

void DX12Backend::SetRenderTarget(Texture* colorTarget, Texture* depthTarget)
{
	if (!colorTarget)
		return;

	GlobalCmdList->CmdList->OMSetRenderTargets(
		1,
		&colorTarget->CpuHandleRTV,
		FALSE,
		depthTarget ? &depthTarget->CpuHandleDSV : nullptr);
}

void DX12Backend::SetRenderTargets(Texture* const* colorTargets, uint32_t colorTargetCount, Texture* depthTarget)
{
	if (!colorTargets || colorTargetCount == 0)
	{
		if (depthTarget)
		{
			GlobalCmdList->CmdList->OMSetRenderTargets(
				0,
				nullptr,
				FALSE,
				&depthTarget->CpuHandleDSV);
		}
		return;
	}

	std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> renderTargetHandles;
	renderTargetHandles.reserve(colorTargetCount);
	for (uint32_t i = 0; i < colorTargetCount; ++i)
	{
		if (!colorTargets[i])
			return;
		renderTargetHandles.push_back(colorTargets[i]->CpuHandleRTV);
	}

	GlobalCmdList->CmdList->OMSetRenderTargets(
		colorTargetCount,
		renderTargetHandles.data(),
		FALSE,
		depthTarget ? &depthTarget->CpuHandleDSV : nullptr);
}

void DX12Backend::ClearRenderTarget(Texture* colorTarget, const float clearColor[4])
{
	if (!colorTarget)
		return;

	GlobalCmdList->CmdList->ClearRenderTargetView(colorTarget->CpuHandleRTV, clearColor, 0, nullptr);
}

void DX12Backend::ClearDepth(Texture* depthTarget, float depthValue)
{
	if (!depthTarget)
		return;

	GlobalCmdList->CmdList->ClearDepthStencilView(depthTarget->CpuHandleDSV, D3D12_CLEAR_FLAG_DEPTH, depthValue, 0, 0, nullptr);
}

void DX12Backend::BindDefaultDescriptorHeaps()
{
	ID3D12DescriptorHeap* ppHeaps[] = { SRVCBVDescriptorHeapShaderVisible->DH.Get(), SamplerDescriptorHeapShaderVisible->DH.Get() };
	GlobalCmdList->CmdList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
	BoundComputeRootDescriptorTables.clear();
}

void DX12Backend::SetViewportAndScissor(uint32_t width, uint32_t height)
{
	CD3DX12_VIEWPORT viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
	CD3DX12_RECT scissorRect(0, 0, static_cast<LONG>(width), static_cast<LONG>(height));
	GlobalCmdList->CmdList->RSSetViewports(1, &viewport);
	GlobalCmdList->CmdList->RSSetScissorRects(1, &scissorRect);
}

void DX12Backend::DrawFullscreenQuad(VertexBuffer* vertexBuffer)
{
	if (!vertexBuffer)
		return;

	if (BoundPrimitiveTopology != D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP)
	{
		GlobalCmdList->CmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		BoundPrimitiveTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
	}
	if (BoundVertexBuffer != vertexBuffer)
	{
		GlobalCmdList->CmdList->IASetVertexBuffers(0, 1, &vertexBuffer->view);
		BoundVertexBuffer = vertexBuffer;
	}
	BoundIndexBuffer = nullptr;
	GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);
}

void DX12Backend::BindMeshBuffers(VertexBuffer* vertexBuffer, IndexBuffer* indexBuffer)
{
	// IB-only binding is valid for vertex-pulling PSOs (e.g. procedural
	// grass) where the VS synthesizes positions from SV_VertexID and the
	// PSO has an empty IA layout. Skip the VB-stride diagnostic in that
	// case but still set the IB so DrawIndexedInstanced has indices.
	if (!indexBuffer)
		return;

	// Diagnostic: warn (and assert in debug) when the bound PSO's
	// vertex stride disagrees with the VB's. DX12 itself takes stride
	// from the VBV so this is rendered correctly, but the same setup
	// will silently corrupt vertices on Vulkan (which uses PSO stride).
	// Catching the mismatch on the desktop dev cycle is much cheaper
	// than chasing the artifact through an Android APK install.
	if (vertexBuffer && (BoundGraphicsPipelineForDiag != nullptr))
	if (auto* dxPipeline = dynamic_cast<DX12GraphicsPipelineHandle*>(BoundGraphicsPipelineForDiag))
	{
		const uint32_t psoStride = dxPipeline->VertexStride;
		const uint32_t vbStride = vertexBuffer->view.StrideInBytes;
		if (psoStride != 0 && vbStride != 0 && psoStride != vbStride)
		{
			// Track unique (psoStride, vbStride, path) tuples so the
			// trace doesn't drown but every distinct callsite is logged
			// at least once.
			static std::mutex sMismatchMutex;
			static std::set<std::tuple<uint32_t, uint32_t, std::wstring>> sLoggedMismatches;
			const auto key = std::make_tuple(psoStride, vbStride, dxPipeline->ShaderPathForDiag);
			bool fresh = false;
			{
				std::lock_guard<std::mutex> lock(sMismatchMutex);
				if (sLoggedMismatches.insert(key).second) fresh = true;
			}
			if (fresh)
			{
				AppendCpuRuntimeTrace(
					L"[DX12Backend::BindMeshBuffers] STRIDE MISMATCH — PSO=" +
					std::to_wstring(psoStride) +
					L" VB=" + std::to_wstring(vbStride) +
					L" vbNumVerts=" + std::to_wstring(vertexBuffer->numVertices) +
					L" path=" + dxPipeline->ShaderPathForDiag +
					L" — DX12 will still render (uses VBV stride) but Vulkan reads PSO stride and would slip " +
					std::to_wstring(int32_t(psoStride) - int32_t(vbStride)) +
					L" B per vertex.");
			}
#if defined(_DEBUG) || defined(DEBUG)
			assert(false && "DX12 PSO/VB stride mismatch — same code will corrupt vertices on Vulkan");
#endif
		}
	}

	if (BoundPrimitiveTopology != D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST)
	{
		GlobalCmdList->CmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		BoundPrimitiveTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	}
	if (BoundIndexBuffer != indexBuffer)
	{
		GlobalCmdList->CmdList->IASetIndexBuffer(&indexBuffer->view);
		BoundIndexBuffer = indexBuffer;
	}
	if (vertexBuffer && BoundVertexBuffer != vertexBuffer)
	{
		GlobalCmdList->CmdList->IASetVertexBuffers(0, 1, &vertexBuffer->view);
		BoundVertexBuffer = vertexBuffer;
	}
	else if (!vertexBuffer)
	{
		BoundVertexBuffer = nullptr;
	}
}

void DX12Backend::DrawIndexed(uint32_t indexCount, uint32_t startIndexLocation, int32_t baseVertexLocation)
{
	GlobalCmdList->CmdList->DrawIndexedInstanced(indexCount, 1, startIndexLocation, baseVertexLocation, 0);
}

void DX12Backend::DrawInstanced(uint32_t vertexCountPerInstance, uint32_t instanceCount, uint32_t startVertexLocation, uint32_t startInstanceLocation)
{
	if (BoundPrimitiveTopology != D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST)
	{
		GlobalCmdList->CmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		BoundPrimitiveTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	}
	if (BoundIndexBuffer)
	{
		GlobalCmdList->CmdList->IASetIndexBuffer(nullptr);
		BoundIndexBuffer = nullptr;
	}
	GlobalCmdList->CmdList->DrawInstanced(vertexCountPerInstance, instanceCount, startVertexLocation, startInstanceLocation);
}

void DX12Backend::DrawIndexedInstanced(uint32_t indexCountPerInstance, uint32_t instanceCount, uint32_t startIndexLocation, int32_t baseVertexLocation, uint32_t startInstanceLocation)
{
	GlobalCmdList->CmdList->DrawIndexedInstanced(indexCountPerInstance, instanceCount, startIndexLocation, baseVertexLocation, startInstanceLocation);
}

bool DX12Backend::DrawIndirect(Buffer* indirectArgumentBuffer, uint64_t byteOffset, uint32_t drawCount)
{
	static_assert(sizeof(DrawIndirectArguments) == sizeof(D3D12_DRAW_ARGUMENTS), "Draw indirect argument layout must match D3D12.");
	if (drawCount == 0)
		return true;
	if (!GlobalCmdList || !GlobalCmdList->CmdList || !indirectArgumentBuffer || !indirectArgumentBuffer->resource)
		return false;
	const uint64_t argsBytes = static_cast<uint64_t>(drawCount) * static_cast<uint64_t>(sizeof(DrawIndirectArguments));
	if (byteOffset > std::numeric_limits<uint64_t>::max() - indirectArgumentBuffer->SuballocationOffsetBytes)
		return false;
	if (indirectArgumentBuffer->MappedSizeInBytes > 0 &&
		(byteOffset > indirectArgumentBuffer->MappedSizeInBytes ||
		 argsBytes > static_cast<uint64_t>(indirectArgumentBuffer->MappedSizeInBytes) - byteOffset))
	{
		return false;
	}
	const uint64_t resourceByteOffset = indirectArgumentBuffer->SuballocationOffsetBytes + byteOffset;

	if (BoundPrimitiveTopology != D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST)
	{
		GlobalCmdList->CmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		BoundPrimitiveTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	}
	if (BoundIndexBuffer)
	{
		GlobalCmdList->CmdList->IASetIndexBuffer(nullptr);
		BoundIndexBuffer = nullptr;
	}

	if (!DrawIndirectCommandSignature)
	{
		D3D12_INDIRECT_ARGUMENT_DESC argumentDesc = {};
		argumentDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;

		D3D12_COMMAND_SIGNATURE_DESC signatureDesc = {};
		signatureDesc.ByteStride = sizeof(DrawIndirectArguments);
		signatureDesc.NumArgumentDescs = 1;
		signatureDesc.pArgumentDescs = &argumentDesc;

		HRESULT hr = Device->CreateCommandSignature(
			&signatureDesc,
			nullptr,
			IID_PPV_ARGS(&DrawIndirectCommandSignature));
		if (FAILED(hr))
		{
			AppendCpuRuntimeTrace(L"[DX12Backend] Create DRAW indirect command signature failed hr=" + FormatHexHRESULT(hr));
			return false;
		}
		SetName(DrawIndirectCommandSignature.Get(), L"Corona DrawIndirect CommandSignature");
	}

	GlobalCmdList->CmdList->ExecuteIndirect(
		DrawIndirectCommandSignature.Get(),
		drawCount,
		indirectArgumentBuffer->resource.Get(),
		resourceByteOffset,
		nullptr,
		0);
	return true;
}

bool DX12Backend::DrawIndexedIndirect(Buffer* indirectArgumentBuffer, uint64_t byteOffset, uint32_t drawCount)
{
	static_assert(sizeof(DrawIndexedIndirectArguments) == sizeof(D3D12_DRAW_INDEXED_ARGUMENTS), "Draw indexed indirect argument layout must match D3D12.");
	if (drawCount == 0)
		return true;
	if (!GlobalCmdList || !GlobalCmdList->CmdList || !indirectArgumentBuffer || !indirectArgumentBuffer->resource)
		return false;
	const uint64_t argsBytes = static_cast<uint64_t>(drawCount) * static_cast<uint64_t>(sizeof(DrawIndexedIndirectArguments));
	if (byteOffset > std::numeric_limits<uint64_t>::max() - indirectArgumentBuffer->SuballocationOffsetBytes)
		return false;
	if (indirectArgumentBuffer->MappedSizeInBytes > 0 &&
		(byteOffset > indirectArgumentBuffer->MappedSizeInBytes ||
		 argsBytes > static_cast<uint64_t>(indirectArgumentBuffer->MappedSizeInBytes) - byteOffset))
	{
		return false;
	}
	const uint64_t resourceByteOffset = indirectArgumentBuffer->SuballocationOffsetBytes + byteOffset;

	if (!DrawIndexedIndirectCommandSignature)
	{
		D3D12_INDIRECT_ARGUMENT_DESC argumentDesc = {};
		argumentDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

		D3D12_COMMAND_SIGNATURE_DESC signatureDesc = {};
		signatureDesc.ByteStride = sizeof(DrawIndexedIndirectArguments);
		signatureDesc.NumArgumentDescs = 1;
		signatureDesc.pArgumentDescs = &argumentDesc;

		HRESULT hr = Device->CreateCommandSignature(
			&signatureDesc,
			nullptr,
			IID_PPV_ARGS(&DrawIndexedIndirectCommandSignature));
		if (FAILED(hr))
		{
			AppendCpuRuntimeTrace(L"[DX12Backend] Create DRAW_INDEXED indirect command signature failed hr=" + FormatHexHRESULT(hr));
			return false;
		}
		SetName(DrawIndexedIndirectCommandSignature.Get(), L"Corona DrawIndexedIndirect CommandSignature");
	}

	GlobalCmdList->CmdList->ExecuteIndirect(
		DrawIndexedIndirectCommandSignature.Get(),
		drawCount,
		indirectArgumentBuffer->resource.Get(),
		resourceByteOffset,
		nullptr,
		0);
	return true;
}

void DX12Backend::Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
	InvalidateGraphicsCommandStateCache();
	GlobalCmdList->CmdList->Dispatch(groupCountX, groupCountY, groupCountZ);
}

void DX12Backend::ClearTextureUAVFloat(Texture* texture, const float clearColor[4])
{
	if (!texture)
		return;

	GlobalCmdList->CmdList->ClearUnorderedAccessViewFloat(
		texture->GpuHandleUAV,
		texture->CpuHandleUAV,
		texture->resource.Get(),
		clearColor,
		0,
		nullptr);
}

void DX12Backend::CopyTexture(Texture* dstTexture, Texture* srcTexture)
{
	if (!GlobalCmdList || !dstTexture || !srcTexture || dstTexture == srcTexture)
		return;
	if (!dstTexture->resource || !srcTexture->resource)
		return;

	GlobalCmdList->CmdList->CopyResource(dstTexture->resource.Get(), srcTexture->resource.Get());
}

void DX12Backend::BeginNewGraphicsCommandList()
{
	if (!CmdQ)
		return;

	GlobalCmdList = CmdQ->AllocCmdList();
	GlobalCmdList->Fence = CmdQ->CurrentFenceValue;
	InvalidateGraphicsCommandStateCache();
	InvalidateComputeCommandStateCache();
	InvalidateRayTracingCommandStateCache();
	RestoreCoronaDescriptorHeaps(this, GlobalCmdList->CmdList.Get());
}

UINT64 DX12Backend::SubmitCurrentCommandList()
{
	if (!GlobalCmdList)
		return 0;
	if (GlobalCmdList == ActiveAsyncRtCmdList)
		return 0;

	CommandList* submittedCmdList = GlobalCmdList;
	const UINT64 submittedFenceValue = CmdQ->ExecuteCommandList(submittedCmdList);
	GlobalCmdList = nullptr;
	InvalidateGraphicsCommandStateCache();
	InvalidateComputeCommandStateCache();
	InvalidateRayTracingCommandStateCache();
	return submittedFenceValue;
}

UINT64 DX12Backend::SubmitCurrentCommandListAndRestart()
{
	const UINT64 submittedFenceValue = SubmitCurrentCommandList();
	BeginNewGraphicsCommandList();
	return submittedFenceValue;
}

bool DX12Backend::BeginAsyncRtRecordingAfterGraphicsSubmit()
{
	if (!AsyncRtCmdQ || ActiveAsyncRtCmdList)
		return false;

	const UINT64 graphicsFenceValue = SubmitCurrentCommandList();
	if (graphicsFenceValue != 0)
		AsyncRtCmdQ->CmdQueue->Wait(CmdQ->m_fence.Get(), graphicsFenceValue);

	ActiveAsyncRtCmdList = AsyncRtCmdQ->AllocCmdList();
	ActiveAsyncRtCmdList->Fence = AsyncRtCmdQ->CurrentFenceValue;
	GlobalCmdList = ActiveAsyncRtCmdList;
	InvalidateGraphicsCommandStateCache();
	InvalidateComputeCommandStateCache();
	InvalidateRayTracingCommandStateCache();
	RestoreCoronaDescriptorHeaps(this, ActiveAsyncRtCmdList->CmdList.Get());
	static UINT sAsyncBeginTraceCount = 0;
	if (sAsyncBeginTraceCount < 8)
	{
		AppendCpuRuntimeTrace(
			L"[DX12AsyncRT] begin recording after graphics fence=" +
			std::to_wstring(graphicsFenceValue) +
			L", listType=" + CommandListTypeName(AsyncRtCmdQ->Type));
		++sAsyncBeginTraceCount;
	}
	return true;
}

UINT64 DX12Backend::EndAsyncRtRecordingAndResumeGraphics()
{
	if (!AsyncRtCmdQ || !ActiveAsyncRtCmdList)
	{
		BeginNewGraphicsCommandList();
		return 0;
	}

	CommandList* submittedCmdList = ActiveAsyncRtCmdList;
	const UINT64 submittedFenceValue = AsyncRtCmdQ->ExecuteCommandList(submittedCmdList);
	PendingAsyncRtFenceValue = submittedFenceValue;
	ActiveAsyncRtCmdList = nullptr;
	GlobalCmdList = nullptr;
	static UINT sAsyncEndTraceCount = 0;
	if (sAsyncEndTraceCount < 8)
	{
		AppendCpuRuntimeTrace(
			L"[DX12AsyncRT] submitted async fence=" +
			std::to_wstring(submittedFenceValue));
		++sAsyncEndTraceCount;
	}
	BeginNewGraphicsCommandList();
	return submittedFenceValue;
}

void DX12Backend::WaitForAsyncRtOnGraphicsQueue()
{
	if (!CmdQ || !AsyncRtCmdQ || PendingAsyncRtFenceValue == 0)
		return;

	static UINT sAsyncWaitTraceCount = 0;
	if (sAsyncWaitTraceCount < 8)
	{
		AppendCpuRuntimeTrace(
			L"[DX12AsyncRT] graphics queue wait async fence=" +
			std::to_wstring(PendingAsyncRtFenceValue));
		++sAsyncWaitTraceCount;
	}
	CmdQ->CmdQueue->Wait(AsyncRtCmdQ->m_fence.Get(), PendingAsyncRtFenceValue);
	PendingAsyncRtFenceValue = 0;
}

void DX12Backend::SubmitGraphicsWorkAndWaitForAsyncRt()
{
	if (!HasPendingAsyncRtWork())
		return;

	SubmitCurrentCommandList();
	WaitForAsyncRtOnGraphicsQueue();
	BeginNewGraphicsCommandList();
}

void DX12Backend::ExecuteCurrentCommandList()
{
	WaitForAsyncRtOnGraphicsQueue();
	SubmitCurrentCommandList();
}

void DX12Backend::BeginGpuMarker(uint64_t color, const char* label)
{
	if (!GlobalCmdList || !label)
		return;
	PIXBeginEvent(GlobalCmdList->CmdList.Get(), color, "%s", label);
}

void DX12Backend::EndGpuMarker()
{
	if (!GlobalCmdList)
		return;
	PIXEndEvent(GlobalCmdList->CmdList.Get());
}

void DX12Backend::TransitionTexture(Texture* texture, EResourceState stateBefore, EResourceState stateAfter)
{
	if (!texture)
		return;

	D3D12_RESOURCE_BARRIER barrierDesc = {};
	barrierDesc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrierDesc.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrierDesc.Transition.pResource = texture->resource.Get();
	barrierDesc.Transition.Subresource = 0;
	barrierDesc.Transition.StateBefore = ToD3D12ResourceState(stateBefore);
	barrierDesc.Transition.StateAfter = ToD3D12ResourceState(stateAfter);
	GlobalCmdList->CmdList->ResourceBarrier(1, &barrierDesc);
}

void DX12Backend::TransitionBuffer(Buffer* buffer, EResourceState stateBefore, EResourceState stateAfter)
{
	if (!buffer)
		return;

	D3D12_RESOURCE_BARRIER barrierDesc = {};
	barrierDesc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrierDesc.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrierDesc.Transition.pResource = buffer->resource.Get();
	barrierDesc.Transition.Subresource = 0;
	barrierDesc.Transition.StateBefore = ToD3D12ResourceState(stateBefore);
	barrierDesc.Transition.StateAfter = ToD3D12ResourceState(stateAfter);
	GlobalCmdList->CmdList->ResourceBarrier(1, &barrierDesc);
}

void DX12Backend::TransitionVertexBuffer(VertexBuffer* vertexBuffer, EResourceState stateBefore, EResourceState stateAfter)
{
	if (!vertexBuffer || !vertexBuffer->resource)
		return;

	D3D12_RESOURCE_BARRIER barrierDesc = {};
	barrierDesc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrierDesc.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrierDesc.Transition.pResource = vertexBuffer->resource.Get();
	barrierDesc.Transition.Subresource = 0;
	barrierDesc.Transition.StateBefore = ToD3D12ResourceState(stateBefore);
	barrierDesc.Transition.StateAfter = ToD3D12ResourceState(stateAfter);
	GlobalCmdList->CmdList->ResourceBarrier(1, &barrierDesc);
}

void DX12Backend::UAVBarrier(Buffer* buffer)
{
	if (!GlobalCmdList)
		return;

	D3D12_RESOURCE_BARRIER barrierDesc = {};
	barrierDesc.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	barrierDesc.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrierDesc.UAV.pResource = buffer ? buffer->resource.Get() : nullptr;
	GlobalCmdList->CmdList->ResourceBarrier(1, &barrierDesc);
}

void DX12Backend::UAVBarrier(Texture* texture)
{
	if (!GlobalCmdList)
		return;

	D3D12_RESOURCE_BARRIER barrierDesc = {};
	barrierDesc.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	barrierDesc.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrierDesc.UAV.pResource = texture ? texture->resource.Get() : nullptr;
	GlobalCmdList->CmdList->ResourceBarrier(1, &barrierDesc);
}

shared_ptr<Sampler> DX12Backend::CreateSampler(D3D12_SAMPLER_DESC& InSamplerDesc)
{
	Sampler* sampler = new Sampler;
	sampler->SamplerDesc = InSamplerDesc;
	SamplerDescriptorHeapShaderVisible->AllocDescriptor(sampler->CpuHandle, sampler->GpuHandle);
	Device->CreateSampler(&sampler->SamplerDesc, sampler->CpuHandle);

	return shared_ptr<Sampler>(sampler);
}

std::shared_ptr<Buffer> DX12Backend::CreateBuffer(const BufferCreateDesc& desc)
{
	if (desc.NumElements == 0 || desc.ElementSize == 0)
		return nullptr;

	if (desc.AllocationPolicy == EBufferAllocationPolicy::Suballocated &&
		desc.Lifetime == EBufferLifetime::Persistent &&
		desc.Shape == EBufferShape::Structured &&
		desc.Access == EBufferAccess::GpuOnly &&
		!desc.bAllowUnorderedAccess &&
		desc.InitialData)
	{
		return CreateSuballocatedStructuredBuffer(desc);
	}

	std::shared_ptr<Buffer> buffer = CreateBuffer(
		desc.NumElements,
		desc.ElementSize,
		ToD3D12ResourceState(desc.InitialState),
		desc.bAllowUnorderedAccess,
		desc.InitialData,
		desc.Access);
	if (buffer)
	{
		if (desc.Shape == EBufferShape::Structured)
			buffer->MakeStructuredBufferSRV();
		else
			buffer->MakeByteAddressBufferSRV();
	}
	return buffer;
}

std::shared_ptr<Buffer> DX12Backend::CreateBuffer(UINT InNumElements, UINT InElementSize, D3D12_RESOURCE_STATES initResState, bool isUAV, void* SrcData, EBufferAccess access)
{
	Buffer * buffer = new Buffer;
	buffer->Owner = this;
	D3D12_RESOURCE_DESC bufDesc = {};
	bufDesc.Alignment = 0;
	bufDesc.DepthOrArraySize = 1;
	bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
	if (isUAV)
		bufDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	bufDesc.Format = DXGI_FORMAT_UNKNOWN;
	bufDesc.Height = 1;
	bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	bufDesc.MipLevels = 1;
	bufDesc.SampleDesc.Count = 1;
	bufDesc.SampleDesc.Quality = 0;
	bufDesc.Width = InNumElements * InElementSize;

	buffer->NumElements = InNumElements;
	buffer->ElementSize = InElementSize;

	D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_DEFAULT;
	if (!isUAV)
	{
		switch (access)
		{
		case EBufferAccess::Upload:
		case EBufferAccess::Stream:
			heapType = D3D12_HEAP_TYPE_UPLOAD;
			break;
		case EBufferAccess::Readback:
			heapType = D3D12_HEAP_TYPE_READBACK;
			break;
		case EBufferAccess::GpuOnly:
		default:
			heapType = D3D12_HEAP_TYPE_DEFAULT;
			break;
		}
	}

	D3D12_RESOURCE_STATES createState = initResState;
	if (heapType == D3D12_HEAP_TYPE_UPLOAD)
		createState = D3D12_RESOURCE_STATE_GENERIC_READ;
	else if (heapType == D3D12_HEAP_TYPE_READBACK)
		createState = D3D12_RESOURCE_STATE_COPY_DEST;

	D3D12_HEAP_PROPERTIES heapProp = CD3DX12_HEAP_PROPERTIES(heapType);
	ThrowIfFailed(Device->CreateCommittedResource(&heapProp, D3D12_HEAP_FLAG_NONE, &bufDesc,
		createState, nullptr, IID_PPV_ARGS(&buffer->resource)));

	if (SrcData)
	{
		UINT Size = buffer->NumElements * buffer->ElementSize;

		if (heapType == D3D12_HEAP_TYPE_UPLOAD)
		{
			D3D12_RANGE readRange{ 0, 0 };
			ThrowIfFailed(buffer->resource->Map(0, &readRange, &buffer->MappedPtr));
			buffer->MappedSizeInBytes = Size;
			memcpy(buffer->MappedPtr, SrcData, Size);
		}
		else if (heapType == D3D12_HEAP_TYPE_DEFAULT)
		{
			CommandList* cmd = CmdQ->AllocCmdList();

			if (initResState != D3D12_RESOURCE_STATE_COPY_DEST)
				cmd->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(buffer->resource.Get(), initResState, D3D12_RESOURCE_STATE_COPY_DEST));

			ComPtr<ID3D12Resource> UploadHeap;

			ThrowIfFailed(Device->CreateCommittedResource(
				&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
				D3D12_HEAP_FLAG_NONE,
				&CD3DX12_RESOURCE_DESC::Buffer(Size),
				D3D12_RESOURCE_STATE_GENERIC_READ,
				nullptr,
				IID_PPV_ARGS(&UploadHeap)));

			UINT8* pData = nullptr;
			UploadHeap->Map(0, nullptr, reinterpret_cast<void**>(&pData));
			memcpy(reinterpret_cast<void*>(pData), SrcData, Size);
			UploadHeap->Unmap(0, nullptr);

			cmd->CmdList->CopyBufferRegion(buffer->resource.Get(), 0, UploadHeap.Get(), 0, Size);
			if (initResState != D3D12_RESOURCE_STATE_COPY_DEST)
				cmd->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(buffer->resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, initResState));


			CmdQ->ExecuteCommandList(cmd);
			CmdQ->WaitGPU();
		}
	}

	//// create shader resource view
	//D3D12_SHADER_RESOURCE_VIEW_DESC bufferSRVDesc;
	//bufferSRVDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	//bufferSRVDesc.Format = DXGI_FORMAT_R32_TYPELESS;
	//bufferSRVDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
	//bufferSRVDesc.Buffer.StructureByteStride = 0;
	//bufferSRVDesc.Buffer.FirstElement = 0;
	//bufferSRVDesc.Buffer.NumElements = InNumElements;// static_cast<UINT>(Size) / sizeof(float); // byte address buffer
	//bufferSRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

	//// TODO : should be GlobalDHRing? if object instance are to move.
	//TextureDHRing->AllocDescriptor(buffer->CpuHandleSRV, buffer->GpuHandleSRV);

	//Device->CreateShaderResourceView(buffer->resource.Get(), &bufferSRVDesc, buffer->CpuHandleSRV);

	shared_ptr<Buffer> ptr = shared_ptr<Buffer>(buffer);
	if (isUAV)
		DynamicBuffers.push_back(ptr);

	return ptr;
}

shared_ptr<Buffer> DX12Backend::CreateDefaultByteAddressBuffer(UINT InNumElements, UINT InElementSize, EInitialResourceState initialState)
{
	if (InNumElements == 0 || InElementSize == 0)
		return nullptr;

	Buffer* buffer = new Buffer;
	buffer->Owner = this;
	buffer->NumElements = InNumElements;
	buffer->ElementSize = InElementSize;

	const UINT64 Size = static_cast<UINT64>(InNumElements) * static_cast<UINT64>(InElementSize);
	D3D12_HEAP_PROPERTIES heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(Size, D3D12_RESOURCE_FLAG_NONE);
	const HRESULT hr = Device->CreateCommittedResource(
		&heapProp,
		D3D12_HEAP_FLAG_NONE,
		&desc,
		ToD3D12ResourceState(initialState),
		nullptr,
		IID_PPV_ARGS(&buffer->resource));
	if (FAILED(hr))
	{
		const HRESULT deviceRemovedReason = Device ? Device->GetDeviceRemovedReason() : S_OK;
		AppendCpuRuntimeTrace(
			L"[DX12Buffer] CreateDefaultByteAddressBuffer failed hr=" + FormatHexHRESULT(hr) +
			L", deviceRemovedReason=" + FormatHexHRESULT(deviceRemovedReason) +
			L", elements=" + std::to_wstring(InNumElements) +
			L", elementSize=" + std::to_wstring(InElementSize) +
			L", bytes=" + std::to_wstring(Size));
		MarkDeviceLost(L"DX12Backend::CreateDefaultByteAddressBuffer", hr);
		delete buffer;
		return nullptr;
	}

	NAME_D3D12_OBJECT(buffer->resource);
	buffer->MakeByteAddressBufferSRV();
	return shared_ptr<Buffer>(buffer);
}

bool DX12Backend::UploadToDefaultBuffer(Buffer* buffer, const void* srcData, UINT sizeInBytes, EResourceState stateBefore, EResourceState stateAfter)
{
	if (!buffer || !buffer->resource || !srcData || sizeInBytes == 0)
		return false;

	const UINT64 capacityBytes = static_cast<UINT64>(buffer->NumElements) * static_cast<UINT64>(buffer->ElementSize);
	if (static_cast<UINT64>(sizeInBytes) > capacityBytes)
		return false;

	if (!buffer->UploadResource || buffer->UploadMappedSizeInBytes < sizeInBytes)
	{
		buffer->UploadResource.Reset();
		buffer->UploadMappedPtr = nullptr;
		buffer->UploadMappedSizeInBytes = 0;

		const D3D12_HEAP_PROPERTIES uploadHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		const D3D12_RESOURCE_DESC uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(capacityBytes);
		HRESULT hr = Device->CreateCommittedResource(
			&uploadHeapProps,
			D3D12_HEAP_FLAG_NONE,
			&uploadDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&buffer->UploadResource));
		if (FAILED(hr))
		{
			AppendCpuRuntimeTrace(L"[DX12Upload] persistent staging buffer allocation failed hr=" + FormatHexHRESULT(hr));
			return false;
		}
		NAME_D3D12_OBJECT(buffer->UploadResource);

		D3D12_RANGE readRange{ 0, 0 };
		hr = buffer->UploadResource->Map(0, &readRange, &buffer->UploadMappedPtr);
		if (FAILED(hr) || !buffer->UploadMappedPtr)
		{
			AppendCpuRuntimeTrace(L"[DX12Upload] persistent staging buffer Map failed hr=" + FormatHexHRESULT(hr));
			buffer->UploadResource.Reset();
			buffer->UploadMappedPtr = nullptr;
			return false;
		}
		buffer->UploadMappedSizeInBytes = static_cast<UINT>(capacityBytes);
	}
	memcpy(buffer->UploadMappedPtr, srcData, sizeInBytes);

	CommandList* cmd = CmdQ->AllocCmdList();
	const D3D12_RESOURCE_STATES before = ToD3D12ResourceState(stateBefore);
	const D3D12_RESOURCE_STATES after = ToD3D12ResourceState(stateAfter);
	if (before != D3D12_RESOURCE_STATE_COPY_DEST)
	{
		cmd->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
			buffer->resource.Get(),
			before,
			D3D12_RESOURCE_STATE_COPY_DEST));
	}
	cmd->CmdList->CopyBufferRegion(buffer->resource.Get(), 0, buffer->UploadResource.Get(), 0, sizeInBytes);
	if (after != D3D12_RESOURCE_STATE_COPY_DEST)
	{
		cmd->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
			buffer->resource.Get(),
			D3D12_RESOURCE_STATE_COPY_DEST,
			after));
	}

	CmdQ->ExecuteCommandList(cmd);

	return true;
}

bool DX12Backend::CreateOrUpdateRayTracingInstancePropertyBuffer(
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

	if (numElements == 0 || elementSize == 0)
		return fail(L"invalid buffer dimensions");
	if (!srcData || sizeInBytes == 0)
		return fail(L"missing source data");

	if (!buffer || buffer->NumElements < numElements || buffer->ElementSize != elementSize)
	{
		try
		{
			buffer = CreateDefaultByteAddressBuffer(numElements, elementSize, EInitialResourceState::ShaderRead);
		}
		catch (...)
		{
			return fail(L"CreateDefaultByteAddressBuffer threw");
		}
		if (!buffer || !buffer->resource)
			return fail(L"CreateDefaultByteAddressBuffer returned null");

		AppendCpuRuntimeTrace(
			L"[RTAS] InstancePropertyBuffer DX12 heap=DEFAULT"
			L", capacity=" + std::to_wstring(numElements) +
			L", bytes=" + std::to_wstring(static_cast<uint64_t>(numElements) * static_cast<uint64_t>(elementSize)));
	}

	if (!buffer || !buffer->resource)
		return fail(L"frame buffer missing resource");

	bool uploaded = false;
	try
	{
		uploaded = UploadToDefaultBuffer(
			buffer.get(),
			srcData,
			sizeInBytes,
			EResourceState::ShaderRead,
			EResourceState::ShaderRead);
	}
	catch (...)
	{
		return fail(L"UploadToDefaultBuffer threw");
	}
	if (!uploaded)
		return fail(L"UploadToDefaultBuffer failed");

	if (outFailureReason)
		outFailureReason->clear();
	return true;
}

DX12Backend::GeometryPlacement DX12Backend::AllocateGeometryPlacement(UINT64 size, UINT64 alignment)
{
	const UINT64 align = alignment ? alignment : static_cast<UINT64>(D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
	UINT64 aligned = (GeometryPlacedHeapCursor + (align - 1)) & ~(align - 1);
	if (GeometryPlacedHeaps.empty() || aligned + size > GeometryPlacedHeapCapacity)
	{
		const UINT64 blockSize = std::max<UINT64>(GeometryPlacedHeapBlockSize, size + align);
		D3D12_HEAP_DESC heapDesc = {};
		heapDesc.SizeInBytes = blockSize;
		heapDesc.Properties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		heapDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
		heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
		ComPtr<ID3D12Heap> heap;
		ThrowIfFailed(Device->CreateHeap(&heapDesc, IID_PPV_ARGS(&heap)));
		GeometryPlacedHeaps.push_back(heap);
		GeometryPlacedHeapCapacity = blockSize;
		GeometryPlacedHeapCursor = 0;
		aligned = 0;
	}
	GeometryPlacement placement{ GeometryPlacedHeaps.back().Get(), aligned };
	GeometryPlacedHeapCursor = aligned + size;
	return placement;
}

shared_ptr<IndexBuffer> DX12Backend::CreateIndexBuffer(EIndexFormat Format, UINT Size, void* SrcData)
{
	CommandList* cmd = CmdQ->AllocCmdList();

	IndexBuffer* ib = new IndexBuffer;
	ib->Owner = this;
	const UINT rawSrvSize = (Size + 3u) & ~3u;

	/*stringstream ss;
	ss << "CreateIndexBuffer : " << Size << "\n";
	OutputDebugStringA(ss.str().c_str());*/

	{
		const GeometryPlacement placement = AllocateGeometryPlacement(rawSrvSize, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
		ThrowIfFailed(Device->CreatePlacedResource(
			placement.heap,
			placement.offset,
			&CD3DX12_RESOURCE_DESC::Buffer(rawSrvSize),
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_PPV_ARGS(&ib->resource)));
	}

	NAME_D3D12_OBJECT(ib->resource);

	ib->view.BufferLocation = ib->resource->GetGPUVirtualAddress();
	ib->view.Format = ToDXGIFormat(Format);
	ib->view.SizeInBytes = Size;

	ib->numIndices = Format == EIndexFormat::U32 ? (Size / 4) : (Size / 2);


	if (SrcData)
	{
		ComPtr<ID3D12Resource> UploadHeap;

		ThrowIfFailed(Device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(rawSrvSize),
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&UploadHeap)));

		UINT8* mappedData = nullptr;
		D3D12_RANGE readRange{ 0, 0 };
		ThrowIfFailed(UploadHeap->Map(0, &readRange, reinterpret_cast<void**>(&mappedData)));
		std::memset(mappedData, 0, rawSrvSize);
		std::memcpy(mappedData, SrcData, Size);
		UploadHeap->Unmap(0, nullptr);

		cmd->CmdList->CopyBufferRegion(ib->resource.Get(), 0, UploadHeap.Get(), 0, rawSrvSize);
		cmd->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(ib->resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));

		// create shader resource view
		D3D12_SHADER_RESOURCE_VIEW_DESC vertexSRVDesc;
		vertexSRVDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		vertexSRVDesc.Format = DXGI_FORMAT_R32_TYPELESS;
		vertexSRVDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
		vertexSRVDesc.Buffer.StructureByteStride = 0;
		vertexSRVDesc.Buffer.FirstElement = 0;
		vertexSRVDesc.Buffer.NumElements = rawSrvSize / sizeof(uint32_t); // byte address buffer
		vertexSRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

		GeomtryDHRing->AllocDescriptor(ib->CpuHandleSRV, ib->GpuHandleSRV);

		Device->CreateShaderResourceView(ib->resource.Get(), &vertexSRVDesc, ib->CpuHandleSRV);

		CmdQ->ExecuteCommandList(cmd);
		CmdQ->WaitGPU();
	}

	return shared_ptr<IndexBuffer>(ib);
}

shared_ptr<VertexBuffer> DX12Backend::CreateVertexBuffer(UINT Size, UINT Stride, void* SrcData)
{
	CommandList* cmd = CmdQ->AllocCmdList();

	VertexBuffer* vb = new VertexBuffer;
	vb->Owner = this;
	
	/*stringstream ss;
	ss << "CreateVertexBuffer : " << Size << "\n";
	OutputDebugStringA(ss.str().c_str());*/
	
	{
		const GeometryPlacement placement = AllocateGeometryPlacement(Size, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
		ThrowIfFailed(Device->CreatePlacedResource(
			placement.heap,
			placement.offset,
			&CD3DX12_RESOURCE_DESC::Buffer(Size),
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_PPV_ARGS(&vb->resource)));
	}

	NAME_D3D12_OBJECT(vb->resource);

	if (SrcData)
	{
		D3D12_SUBRESOURCE_DATA vertexData = {};
		vertexData.pData = SrcData;
		vertexData.RowPitch = Size;
		vertexData.SlicePitch = vertexData.RowPitch;

		ComPtr<ID3D12Resource> UploadHeap;

		ThrowIfFailed(Device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(Size),
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&UploadHeap)));

		UpdateSubresources<1>(cmd->CmdList.Get(), vb->resource.Get(), UploadHeap.Get(), 0, 0, 1, &vertexData);
		cmd->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(vb->resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));

		// Initialize the vertex buffer view.
		vb->view.BufferLocation = vb->resource->GetGPUVirtualAddress();
		vb->view.StrideInBytes = Stride;
		vb->view.SizeInBytes = Size;
		vb->numVertices = Size / Stride;

		// create shader resource view
		D3D12_SHADER_RESOURCE_VIEW_DESC vertexSRVDesc;
		vertexSRVDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		vertexSRVDesc.Format = DXGI_FORMAT_R32_TYPELESS;
		vertexSRVDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
		vertexSRVDesc.Buffer.StructureByteStride = 0;
		vertexSRVDesc.Buffer.FirstElement = 0;
		vertexSRVDesc.Buffer.NumElements = static_cast<UINT>(Size) / sizeof(float); // byte address buffer
		vertexSRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

		GeomtryDHRing->AllocDescriptor(vb->CpuHandleSRV, vb->GpuHandleSRV);

		Device->CreateShaderResourceView(vb->resource.Get(), &vertexSRVDesc, vb->CpuHandleSRV);

		CmdQ->ExecuteCommandList(cmd);
		CmdQ->WaitGPU();
	}

	return shared_ptr<VertexBuffer>(vb);
}

shared_ptr<Buffer> DX12Backend::CreateUploadStructuredBuffer(uint32_t NumElements, uint32_t ElementSize)
{
	if (NumElements == 0 || ElementSize == 0)
		return nullptr;

	const UINT SizeInBytes = NumElements * ElementSize;

	Buffer* buffer = new Buffer;
	buffer->Owner = this;
	buffer->NumElements = NumElements;
	buffer->ElementSize = ElementSize;

	D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(SizeInBytes, D3D12_RESOURCE_FLAG_NONE);
	D3D12_HEAP_PROPERTIES heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

	ThrowIfFailed(Device->CreateCommittedResource(
		&heapProp, D3D12_HEAP_FLAG_NONE, &desc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
		IID_PPV_ARGS(&buffer->resource)));

	NAME_D3D12_OBJECT(buffer->resource);

	// Persistent-map for the lifetime of the buffer. CPU writes flow through
	// PCIe to the GPU at read time; for SBV reads that are tiny per-character
	// this is markedly cheaper than the staged DEFAULT-heap upload path.
	D3D12_RANGE readRange{ 0, 0 };
	ThrowIfFailed(buffer->resource->Map(0, &readRange, &buffer->MappedPtr));
	buffer->MappedSizeInBytes = SizeInBytes;

	// Structured SRV.
	buffer->MakeStructuredBufferSRV();

	return shared_ptr<Buffer>(buffer);
}

void DX12Backend::UpdateUploadStructuredBuffer(Buffer* buffer, const void* srcData, uint32_t sizeInBytes)
{
	if (!buffer || !buffer->MappedPtr || !srcData || sizeInBytes == 0)
		return;
	if (sizeInBytes > buffer->MappedSizeInBytes)
		return;
	memcpy(buffer->MappedPtr, srcData, sizeInBytes);
}

shared_ptr<Buffer> DX12Backend::AllocateTransientUploadStructuredBuffer(uint32_t NumElements, uint32_t ElementSize, const void* srcData)
{
	if (NumElements == 0 || ElementSize == 0 || !Device || !GlobalDHRing)
		return nullptr;

	const UINT64 sizeInBytes64 = static_cast<UINT64>(NumElements) * static_cast<UINT64>(ElementSize);
	if (sizeInBytes64 > static_cast<UINT64>(std::numeric_limits<uint32_t>::max()))
		return nullptr;

	TransientUploadStructuredAllocation alloc = AllocateTransientUploadStructuredBytes(sizeInBytes64, ElementSize);
	if (!alloc.resource || !alloc.cpu)
		return nullptr;
	if (srcData)
		memcpy(alloc.cpu, srcData, static_cast<size_t>(sizeInBytes64));

	auto buffer = std::shared_ptr<Buffer>(new Buffer);
	buffer->Owner = this;
	buffer->resource = alloc.resource;
	buffer->NumElements = NumElements;
	buffer->ElementSize = ElementSize;
	buffer->MappedPtr = alloc.cpu;
	buffer->MappedSizeInBytes = static_cast<uint32_t>(sizeInBytes64);
	buffer->SuballocationOffsetBytes = alloc.offset;

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	srvDesc.Format = DXGI_FORMAT_UNKNOWN;
	srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
	srvDesc.Buffer.StructureByteStride = ElementSize;
	srvDesc.Buffer.FirstElement = static_cast<UINT>(alloc.offset / ElementSize);
	srvDesc.Buffer.NumElements = NumElements;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	GlobalDHRing->AllocDescriptor(buffer->CpuHandleSRV, buffer->GpuHandleSRV);
	Device->CreateShaderResourceView(buffer->resource.Get(), &srvDesc, buffer->CpuHandleSRV);
	buffer->Type = Buffer::STRUCTURED;

	if (TransientUploadStructuredKeepAlive.size() < NumFrame)
		TransientUploadStructuredKeepAlive.resize(NumFrame);
	const uint32_t frameIndex = CurrentFrameIndex < NumFrame ? CurrentFrameIndex : 0u;
	TransientUploadStructuredKeepAlive[frameIndex].push_back(buffer);
	return buffer;
}

shared_ptr<Buffer> DX12Backend::AllocateTransientDefaultStructuredBuffer(uint32_t NumElements, uint32_t ElementSize, const void* srcData)
{
	if (NumElements == 0 || ElementSize == 0 || !Device || !GlobalDHRing || !GlobalCmdList || !GlobalCmdList->CmdList)
		return nullptr;

	const UINT64 sizeInBytes64 = static_cast<UINT64>(NumElements) * static_cast<UINT64>(ElementSize);
	if (sizeInBytes64 > static_cast<UINT64>(std::numeric_limits<uint32_t>::max()))
		return nullptr;

	if (DefaultStructuredInUse.size() < NumFrame)
		DefaultStructuredInUse.resize(NumFrame);
	const uint32_t frameIndex = CurrentFrameIndex < NumFrame ? CurrentFrameIndex : 0u;

	// Reuse a recyclable buffer (>= needed capacity) from the free pool;
	// otherwise create one (rounded to a coarse bucket to maximise reuse).
	DefaultStructuredPoolEntry entry;
	bool found = false;
	for (size_t i = 0; i < DefaultStructuredFreePool.size(); ++i)
	{
		if (DefaultStructuredFreePool[i].capacityBytes >= sizeInBytes64)
		{
			entry = std::move(DefaultStructuredFreePool[i]);
			DefaultStructuredFreePool.erase(DefaultStructuredFreePool.begin() + i);
			found = true;
			break;
		}
	}
	if (!found)
	{
		const UINT64 gran = 256ull * 1024ull;
		const UINT64 capacity = ((sizeInBytes64 + gran - 1) / gran) * gran;
		auto buf = std::shared_ptr<Buffer>(new Buffer);
		buf->Owner = this;
		if (FAILED(Device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(capacity),
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_PPV_ARGS(&buf->resource))) || !buf->resource)
			return nullptr;
		entry.buffer = buf;
		entry.capacityBytes = capacity;
		entry.state = D3D12_RESOURCE_STATE_COPY_DEST;
	}

	Buffer* buffer = entry.buffer.get();
	const D3D12_RESOURCE_STATES srState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	if (srcData)
	{
		if (entry.state != D3D12_RESOURCE_STATE_COPY_DEST)
			GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(buffer->resource.Get(), entry.state, D3D12_RESOURCE_STATE_COPY_DEST));
		TransientUploadStructuredAllocation staging = AllocateTransientUploadStructuredBytes(sizeInBytes64, ElementSize);
		if (!staging.resource || !staging.cpu)
			return nullptr;
		memcpy(staging.cpu, srcData, static_cast<size_t>(sizeInBytes64));
		GlobalCmdList->CmdList->CopyBufferRegion(buffer->resource.Get(), 0, staging.resource.Get(), staging.offset, sizeInBytes64);
		GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(buffer->resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srState));
		entry.state = srState;
	}

	buffer->NumElements = NumElements;
	buffer->ElementSize = ElementSize;
	buffer->MappedPtr = nullptr;
	buffer->MappedSizeInBytes = 0;
	buffer->SuballocationOffsetBytes = 0;

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	srvDesc.Format = DXGI_FORMAT_UNKNOWN;
	srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
	srvDesc.Buffer.StructureByteStride = ElementSize;
	srvDesc.Buffer.FirstElement = 0;
	srvDesc.Buffer.NumElements = NumElements;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	GlobalDHRing->AllocDescriptor(buffer->CpuHandleSRV, buffer->GpuHandleSRV);
	Device->CreateShaderResourceView(buffer->resource.Get(), &srvDesc, buffer->CpuHandleSRV);
	buffer->Type = Buffer::STRUCTURED;

	std::shared_ptr<Buffer> result = entry.buffer;
	DefaultStructuredInUse[frameIndex].push_back(std::move(entry));
	return result;
}

void DX12Backend::RecycleTransientDefaultStructuredFrame(uint32_t frameIndex)
{
	if (frameIndex >= DefaultStructuredInUse.size())
		return;
	for (DefaultStructuredPoolEntry& e : DefaultStructuredInUse[frameIndex])
		DefaultStructuredFreePool.push_back(std::move(e));
	DefaultStructuredInUse[frameIndex].clear();
}

shared_ptr<VertexBuffer> DX12Backend::CreateRWVertexBuffer(uint32_t Size, uint32_t Stride)
{
	if (Size == 0 || Stride == 0)
		return nullptr;

	VertexBuffer* vb = new VertexBuffer;
	vb->Owner = this;

	D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(Size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

	ThrowIfFailed(Device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&desc,
		D3D12_RESOURCE_STATE_COMMON,
		nullptr,
		IID_PPV_ARGS(&vb->resource)));

	NAME_D3D12_OBJECT(vb->resource);

	vb->view.BufferLocation = vb->resource->GetGPUVirtualAddress();
	vb->view.StrideInBytes = Stride;
	vb->view.SizeInBytes = Size;
	vb->numVertices = Size / Stride;

	// Byte-address SRV/UAV. The compute shader binds Output as a
	// RWByteAddressBuffer; we expose the SRV in case any future consumer
	// wants vertex-pull mode through SV_VertexID.
	GeomtryDHRing->AllocDescriptor(vb->CpuHandleSRV, vb->GpuHandleSRV);
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	srvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
	srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
	srvDesc.Buffer.StructureByteStride = 0;
	srvDesc.Buffer.FirstElement = 0;
	srvDesc.Buffer.NumElements = static_cast<UINT>(Size) / sizeof(float);
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	Device->CreateShaderResourceView(vb->resource.Get(), &srvDesc, vb->CpuHandleSRV);

	GeomtryDHRing->AllocDescriptor(vb->CpuHandleUAV, vb->GpuHandleUAV);
	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
	uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
	uavDesc.Buffer.StructureByteStride = 0;
	uavDesc.Buffer.FirstElement = 0;
	uavDesc.Buffer.NumElements = static_cast<UINT>(Size) / sizeof(float);
	uavDesc.Buffer.CounterOffsetInBytes = 0;
	Device->CreateUnorderedAccessView(vb->resource.Get(), nullptr, &uavDesc, vb->CpuHandleUAV);

	return shared_ptr<VertexBuffer>(vb);
}

// Cheap VB/IB creation paths: sub-allocate from a large UPLOAD-heap block.
// Each block is a single CreateCommittedResource + persistent Map; the
// per-VB/IB cost is now just a bump-allocate cursor advance + memcpy. The
// block's ID3D12Resource is shared between every VB/IB ComPtr it issued,
// so the block stays alive automatically until its last sub-allocation
// dies. When the active block fills, a new block is committed and the old
// one is "retired" (the pool stops pointing at it; existing VB/IB ComPtrs
// still keep it alive). Designed for dynamic geometry like Spine sprite
// frames where the unique-key space ranges from hundreds to thousands of
// tiny buffers per session.
//
// SRV is intentionally NOT created on this path — the CPU-skinning Spine
// pipeline consumes VB/IB through BindMeshBuffers (no SRV). Callers that
// need an SRV-backed buffer should use the regular CreateVertexBuffer /
// CreateIndexBuffer.
DX12Backend::UploadAllocation DX12Backend::AllocateUploadBytes(UINT64 size, UINT64 alignment)
{
	if (size == 0)
		return {};

	const UINT64 align = alignment > 0 ? alignment : 4;
	auto allocateFromActive = [&]() -> UploadAllocation
	{
		UploadHeapBlock* block = ActiveUploadBlock.get();
		const UINT64 alignedCursor = AlignUploadOffset(block->cursor, align);
		if (alignedCursor + size > block->capacity)
			return {};
		const UINT64 offset = alignedCursor;
		block->cursor = alignedCursor + size;
		UploadAllocation alloc;
		alloc.resource = block->resource;
		alloc.offset = offset;
		alloc.cpu = block->mappedBase + offset;
		alloc.gpuVA = block->gpuVA + offset;
		++UploadAllocationCount;
		UploadBytesIssued += size;
		return alloc;
	};

	if (ActiveUploadBlock)
	{
		UploadAllocation alloc = allocateFromActive();
		if (alloc.resource)
			return alloc;
	}

	// Need a new block. Size it to fit at least this request, but never
	// smaller than the default so subsequent small allocations amortise the
	// CreateCommittedResource cost.
	const UINT64 requestedSize = size + align; // padding so the first aligned cursor still fits
	const UINT64 blockSize = std::max<UINT64>(UploadBlockDefaultSize, requestedSize);

	auto newBlock = std::make_shared<UploadHeapBlock>();
	ThrowIfFailed(Device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(blockSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&newBlock->resource)));
	NAME_D3D12_OBJECT(newBlock->resource);

	void* mapped = nullptr;
	ThrowIfFailed(newBlock->resource->Map(0, nullptr, &mapped));
	newBlock->mappedBase = static_cast<uint8_t*>(mapped);
	newBlock->gpuVA = newBlock->resource->GetGPUVirtualAddress();
	newBlock->capacity = blockSize;
	newBlock->cursor = 0;

	ActiveUploadBlock = std::move(newBlock);
	++UploadBlockCount;
	UploadBytesReserved += blockSize;

	UploadAllocation alloc = allocateFromActive();
	return alloc;
}

void DX12Backend::ResetTransientUploadStructuredFrame(uint32_t frameIndex)
{
	if (TransientUploadStructuredBlocks.size() < NumFrame)
		TransientUploadStructuredBlocks.resize(NumFrame);
	if (TransientUploadStructuredKeepAlive.size() < NumFrame)
		TransientUploadStructuredKeepAlive.resize(NumFrame);
	if (frameIndex >= TransientUploadStructuredBlocks.size())
		return;

	TransientUploadStructuredKeepAlive[frameIndex].clear();
	for (const std::shared_ptr<TransientUploadStructuredBlock>& block : TransientUploadStructuredBlocks[frameIndex])
	{
		if (block)
			block->cursor = 0;
	}
}

DX12Backend::TransientUploadStructuredAllocation DX12Backend::AllocateTransientUploadStructuredBytes(UINT64 size, UINT64 alignment)
{
	if (size == 0)
		return {};
	if (TransientUploadStructuredBlocks.size() < NumFrame)
		TransientUploadStructuredBlocks.resize(NumFrame);
	if (TransientUploadStructuredKeepAlive.size() < NumFrame)
		TransientUploadStructuredKeepAlive.resize(NumFrame);

	const uint32_t frameIndex = CurrentFrameIndex < NumFrame ? CurrentFrameIndex : 0u;
	std::vector<std::shared_ptr<TransientUploadStructuredBlock>>& blocks = TransientUploadStructuredBlocks[frameIndex];
	const UINT64 align = alignment > 0 ? alignment : 4u;

	auto allocateFromBlock = [&](const std::shared_ptr<TransientUploadStructuredBlock>& block) -> TransientUploadStructuredAllocation
	{
		if (!block || !block->resource)
			return {};
		const UINT64 alignedCursor = AlignUploadOffset(block->cursor, align);
		if (alignedCursor + size > block->capacity)
			return {};
		block->cursor = alignedCursor + size;

		TransientUploadStructuredAllocation alloc;
		alloc.resource = block->resource;
		alloc.offset = alignedCursor;
		alloc.cpu = block->mappedBase + alignedCursor;
		alloc.gpuVA = block->gpuVA + alignedCursor;
		++TransientUploadStructuredAllocationCount;
		TransientUploadStructuredBytesIssued += size;
		return alloc;
	};

	for (const std::shared_ptr<TransientUploadStructuredBlock>& block : blocks)
	{
		TransientUploadStructuredAllocation alloc = allocateFromBlock(block);
		if (alloc.resource)
			return alloc;
	}

	const UINT64 requestedSize = size + align;
	const UINT64 blockSize = std::max<UINT64>(TransientUploadStructuredBlockDefaultSize, requestedSize);
	auto block = std::make_shared<TransientUploadStructuredBlock>();
	ThrowIfFailed(Device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(blockSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&block->resource)));
	NAME_D3D12_OBJECT(block->resource);

	void* mapped = nullptr;
	D3D12_RANGE readRange{ 0, 0 };
	ThrowIfFailed(block->resource->Map(0, &readRange, &mapped));
	block->mappedBase = static_cast<uint8_t*>(mapped);
	block->gpuVA = block->resource->GetGPUVirtualAddress();
	block->capacity = blockSize;
	block->cursor = 0;
	blocks.push_back(block);
	++TransientUploadStructuredBlockCount;
	TransientUploadStructuredBytesReserved += blockSize;

	return allocateFromBlock(block);
}

DX12Backend::PersistentStructuredBufferAllocation DX12Backend::AllocatePersistentStructuredBufferBytes(UINT64 size, UINT64 alignment)
{
	if (size == 0)
		return {};
	const UINT64 align = alignment > 0 ? alignment : 4u;

	auto allocateFromBlock = [&](const std::shared_ptr<PersistentStructuredBufferBlock>& block) -> PersistentStructuredBufferAllocation
	{
		if (!block || !block->resource)
			return {};

		for (size_t rangeIndex = 0; rangeIndex < block->freeRanges.size(); ++rangeIndex)
		{
			PersistentStructuredBufferBlock::FreeRange range = block->freeRanges[rangeIndex];
			const UINT64 alignedOffset = AlignUploadOffset(range.offset, align);
			const UINT64 rangeEnd = range.offset + range.size;
			if (alignedOffset > rangeEnd || alignedOffset + size > rangeEnd)
				continue;

			auto insertIt = block->freeRanges.erase(block->freeRanges.begin() + rangeIndex);
			if (alignedOffset > range.offset)
			{
				insertIt = block->freeRanges.insert(insertIt, { range.offset, alignedOffset - range.offset });
				++insertIt;
			}
			const UINT64 allocEnd = alignedOffset + size;
			if (allocEnd < rangeEnd)
			{
				block->freeRanges.insert(insertIt, { allocEnd, rangeEnd - allocEnd });
			}

			PersistentStructuredBufferAllocation alloc;
			alloc.resource = block->resource;
			alloc.block = block;
			alloc.offset = alignedOffset;
			alloc.size = size;
			++PersistentStructuredBufferAllocationCount;
			PersistentStructuredBufferBytesIssued += size;
			return alloc;
		}

		const UINT64 alignedCursor = AlignUploadOffset(block->cursor, align);
		if (alignedCursor + size > block->capacity)
			return {};
		block->cursor = alignedCursor + size;

		PersistentStructuredBufferAllocation alloc;
		alloc.resource = block->resource;
		alloc.block = block;
		alloc.offset = alignedCursor;
		alloc.size = size;
		++PersistentStructuredBufferAllocationCount;
		PersistentStructuredBufferBytesIssued += size;
		return alloc;
	};

	for (const std::shared_ptr<PersistentStructuredBufferBlock>& block : PersistentStructuredBufferBlocks)
	{
		PersistentStructuredBufferAllocation alloc = allocateFromBlock(block);
		if (alloc.resource)
			return alloc;
	}

	const UINT64 requestedSize = size + align;
	const UINT64 blockSize = std::max<UINT64>(PersistentStructuredBufferBlockDefaultSize, requestedSize);
	auto block = std::make_shared<PersistentStructuredBufferBlock>();
	ThrowIfFailed(Device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(blockSize),
		D3D12_RESOURCE_STATE_COPY_DEST,
		nullptr,
		IID_PPV_ARGS(&block->resource)));
	NAME_D3D12_OBJECT(block->resource);
	block->capacity = blockSize;
	block->cursor = 0;
	block->state = D3D12_RESOURCE_STATE_COPY_DEST;
	PersistentStructuredBufferBlocks.push_back(block);
	++PersistentStructuredBufferBlockCount;
	PersistentStructuredBufferBytesReserved += blockSize;
	return allocateFromBlock(block);
}

void DX12Backend::AddPersistentStructuredBufferFreeRange(
	const std::shared_ptr<PersistentStructuredBufferBlock>& block,
	UINT64 offset,
	UINT64 size)
{
	if (!block || size == 0)
		return;

	PersistentStructuredBufferBlock::FreeRange newRange{ offset, size };
	auto& ranges = block->freeRanges;
	auto insertIt = ranges.begin();
	while (insertIt != ranges.end() && insertIt->offset < newRange.offset)
		++insertIt;
	insertIt = ranges.insert(insertIt, newRange);

	if (insertIt != ranges.begin())
	{
		auto prevIt = std::prev(insertIt);
		const UINT64 prevEnd = prevIt->offset + prevIt->size;
		if (prevEnd >= insertIt->offset)
		{
			prevIt->size = std::max(prevEnd, insertIt->offset + insertIt->size) - prevIt->offset;
			insertIt = ranges.erase(insertIt);
			insertIt = prevIt;
		}
	}

	auto nextIt = std::next(insertIt);
	while (nextIt != ranges.end())
	{
		const UINT64 rangeEnd = insertIt->offset + insertIt->size;
		if (rangeEnd < nextIt->offset)
			break;
		insertIt->size = std::max(rangeEnd, nextIt->offset + nextIt->size) - insertIt->offset;
		nextIt = ranges.erase(nextIt);
	}
}

void DX12Backend::ReleasePersistentStructuredBufferBytes(
	const std::shared_ptr<PersistentStructuredBufferBlock>& block,
	UINT64 offset,
	UINT64 size)
{
	if (!block || size == 0)
		return;

	if (!CmdQ || !CmdQ->m_fence)
	{
		AddPersistentStructuredBufferFreeRange(block, offset, size);
		return;
	}

	PendingPersistentStructuredBufferFrees.push_back({
		block,
		offset,
		size,
		CmdQ->CurrentFenceValue
	});
}

void DX12Backend::RetireCompletedPersistentStructuredBufferFrees()
{
	if (!CmdQ || !CmdQ->m_fence || PendingPersistentStructuredBufferFrees.empty())
		return;

	const UINT64 completedFenceValue = CmdQ->m_fence->GetCompletedValue();
	PendingPersistentStructuredBufferFrees.erase(
		std::remove_if(
			PendingPersistentStructuredBufferFrees.begin(),
			PendingPersistentStructuredBufferFrees.end(),
			[&](const PendingPersistentStructuredBufferFree& pending)
			{
				if (pending.fenceValue > completedFenceValue)
					return false;
				AddPersistentStructuredBufferFreeRange(pending.block, pending.offset, pending.size);
				return true;
			}),
		PendingPersistentStructuredBufferFrees.end());
}

void DX12Backend::RetireCompletedPersistentStructuredBufferUploads()
{
	if (!CmdQ || !CmdQ->m_fence || PendingPersistentStructuredBufferUploads.empty())
		return;

	const UINT64 completedFenceValue = CmdQ->m_fence->GetCompletedValue();
	size_t kept = 0;
	for (size_t i = 0; i < PendingPersistentStructuredBufferUploads.size(); ++i)
	{
		PendingPersistentStructuredBufferUpload& pending = PendingPersistentStructuredBufferUploads[i];
		if (pending.FenceValue <= completedFenceValue)
		{
			PendingPersistentStructuredBufferUploadBytes -=
				std::min(PendingPersistentStructuredBufferUploadBytes, pending.Bytes);
			continue;
		}
		if (kept != i)
			PendingPersistentStructuredBufferUploads[kept] = std::move(pending);
		++kept;
	}
	PendingPersistentStructuredBufferUploads.resize(kept);
}

std::shared_ptr<Buffer> DX12Backend::CreateSuballocatedStructuredBuffer(const BufferCreateDesc& desc)
{
	if (!Device || !TextureDHRing || desc.NumElements == 0 || desc.ElementSize == 0 || !desc.InitialData)
		return nullptr;

	const UINT64 sizeInBytes64 = static_cast<UINT64>(desc.NumElements) * static_cast<UINT64>(desc.ElementSize);
	if (sizeInBytes64 > static_cast<UINT64>(std::numeric_limits<uint32_t>::max()))
		return nullptr;

	PersistentStructuredBufferAllocation alloc = AllocatePersistentStructuredBufferBytes(sizeInBytes64, desc.ElementSize);
	if (!alloc.resource)
		return nullptr;

	std::shared_ptr<PersistentStructuredBufferBlock> ownerBlock = alloc.block;
	if (!ownerBlock)
		return nullptr;

	const D3D12_RESOURCE_STATES targetState = ToD3D12ResourceState(desc.InitialState);
	RetireCompletedPersistentStructuredBufferUploads();
	WaitForAsyncRtOnGraphicsQueue();

	CommandList* cmd = CmdQ->AllocCmdList();
	if (ownerBlock->state != D3D12_RESOURCE_STATE_COPY_DEST)
	{
		cmd->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
			ownerBlock->resource.Get(),
			ownerBlock->state,
			D3D12_RESOURCE_STATE_COPY_DEST));
		ownerBlock->state = D3D12_RESOURCE_STATE_COPY_DEST;
	}

	ComPtr<ID3D12Resource> uploadHeap;
	ThrowIfFailed(Device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(sizeInBytes64),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&uploadHeap)));
	NAME_D3D12_OBJECT(uploadHeap);

	void* mapped = nullptr;
	D3D12_RANGE readRange{ 0, 0 };
	ThrowIfFailed(uploadHeap->Map(0, &readRange, &mapped));
	memcpy(mapped, desc.InitialData, static_cast<size_t>(sizeInBytes64));
	uploadHeap->Unmap(0, nullptr);
	cmd->CmdList->CopyBufferRegion(ownerBlock->resource.Get(), alloc.offset, uploadHeap.Get(), 0, sizeInBytes64);

	if (targetState != D3D12_RESOURCE_STATE_COPY_DEST)
	{
		cmd->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
			ownerBlock->resource.Get(),
			D3D12_RESOURCE_STATE_COPY_DEST,
			targetState));
		ownerBlock->state = targetState;
	}

	CmdQ->ExecuteCommandList(cmd);

	PendingPersistentStructuredBufferUpload pendingUpload;
	pendingUpload.FenceValue = cmd->Fence.value_or(CmdQ->CurrentFenceValue);
	pendingUpload.Bytes = sizeInBytes64;
	pendingUpload.UploadHeap = uploadHeap;
	PendingPersistentStructuredBufferUploads.push_back(std::move(pendingUpload));
	PendingPersistentStructuredBufferUploadBytes += sizeInBytes64;
	if (PendingPersistentStructuredBufferUploadBytes > kMaxInFlightPersistentStructuredBufferUploadBytes)
	{
		CmdQ->WaitGPU();
		RetireCompletedPersistentStructuredBufferUploads();
		RetireCompletedPersistentStructuredBufferFrees();
	}

	auto buffer = std::shared_ptr<Buffer>(
		new Buffer,
		[this, ownerBlock, offset = alloc.offset, size = alloc.size](Buffer* rawBuffer)
		{
			delete rawBuffer;
			ReleasePersistentStructuredBufferBytes(ownerBlock, offset, size);
		});
	buffer->Owner = this;
	buffer->resource = alloc.resource;
	buffer->NumElements = desc.NumElements;
	buffer->ElementSize = desc.ElementSize;
	buffer->SuballocationOffsetBytes = alloc.offset;
	buffer->MakeStructuredBufferSRV();
	return buffer;
}

shared_ptr<VertexBuffer> DX12Backend::CreateUploadVertexBuffer(UINT size, UINT stride, const void* srcData)
{
	if (size == 0)
		return nullptr;

	const UINT64 align = stride > 0 ? stride : 4u;
	UploadAllocation alloc = AllocateUploadBytes(size, align);
	if (!alloc.resource)
		return nullptr;

	if (srcData)
		memcpy(alloc.cpu, srcData, size);

	auto* vb = new VertexBuffer;
	vb->Owner = this;
	vb->resource = alloc.resource; // shared with the block; block stays alive while VB lives
	vb->view.BufferLocation = alloc.gpuVA;
	vb->view.StrideInBytes = stride;
	vb->view.SizeInBytes = size;
	vb->numVertices = stride > 0 ? (size / stride) : 0;
	vb->MappedCpu = alloc.cpu;
	vb->MappedCapacityBytes = size;
	return shared_ptr<VertexBuffer>(vb);
}

shared_ptr<IndexBuffer> DX12Backend::CreateUploadIndexBuffer(EIndexFormat format, UINT size, const void* srcData)
{
	if (size == 0)
		return nullptr;

	const UINT64 align = format == EIndexFormat::U32 ? 4u : 2u;
	UploadAllocation alloc = AllocateUploadBytes(size, align);
	if (!alloc.resource)
		return nullptr;

	if (srcData)
		memcpy(alloc.cpu, srcData, size);

	auto* ib = new IndexBuffer;
	ib->Owner = this;
	ib->resource = alloc.resource; // shared with the block
	ib->view.BufferLocation = alloc.gpuVA;
	ib->view.Format = ToDXGIFormat(format);
	ib->view.SizeInBytes = size;
	ib->numIndices = format == EIndexFormat::U32 ? (size / 4) : (size / 2);
	ib->MappedCpu = alloc.cpu;
	ib->MappedCapacityBytes = size;
	return shared_ptr<IndexBuffer>(ib);
}

void DX12Backend::UpdateUploadVertexBuffer(VertexBuffer* buffer, const void* srcData, UINT sizeInBytes)
{
	if (!buffer || !buffer->MappedCpu || !srcData || sizeInBytes == 0)
		return;
	if (sizeInBytes > buffer->MappedCapacityBytes)
		sizeInBytes = buffer->MappedCapacityBytes;
	memcpy(buffer->MappedCpu, srcData, sizeInBytes);
	buffer->view.SizeInBytes = sizeInBytes;
	buffer->numVertices = buffer->view.StrideInBytes > 0 ? (sizeInBytes / buffer->view.StrideInBytes) : 0;
}

void DX12Backend::UpdateUploadIndexBuffer(IndexBuffer* buffer, const void* srcData, UINT sizeInBytes)
{
	if (!buffer || !buffer->MappedCpu || !srcData || sizeInBytes == 0)
		return;
	if (sizeInBytes > buffer->MappedCapacityBytes)
		sizeInBytes = buffer->MappedCapacityBytes;
	memcpy(buffer->MappedCpu, srcData, sizeInBytes);
	buffer->view.SizeInBytes = sizeInBytes;
	const UINT bpp = buffer->view.Format == DXGI_FORMAT_R32_UINT ? 4u : 2u;
	buffer->numIndices = sizeInBytes / bpp;
}

void DX12Backend::PresentBarrier(Texture* rt)
{
	D3D12_RESOURCE_BARRIER BarrierDesc = {};
	BarrierDesc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	BarrierDesc.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	BarrierDesc.Transition.pResource = rt->resource.Get();
	BarrierDesc.Transition.Subresource = 0;
	BarrierDesc.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	BarrierDesc.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
	GlobalCmdList->CmdList->ResourceBarrier(1, &BarrierDesc);
}

DX12Backend::DX12Backend(ComPtr<ID3D12Device5> InDevice)
	:Device(InDevice)
{
#if USE_AFTERMATH
	const GFSDK_Aftermath_Result result = GFSDK_Aftermath_DX12_Initialize(
		GFSDK_Aftermath_Version::GFSDK_Aftermath_Version_API,
		GFSDK_Aftermath_FeatureFlags::GFSDK_Aftermath_FeatureFlags_EnableMarkers |
			GFSDK_Aftermath_FeatureFlags::GFSDK_Aftermath_FeatureFlags_EnableResourceTracking,
		Device.Get());
	bAftermathEnabled =
		result == GFSDK_Aftermath_Result::GFSDK_Aftermath_Result_Success ||
		result == GFSDK_Aftermath_Result::GFSDK_Aftermath_Result_FAIL_AlreadyInitialized;
	if (!bAftermathEnabled)
		OutputDebugStringA("NVIDIA Aftermath DX12 initialization failed; GPU crash markers disabled.\n");
#endif

	FrameFenceValueVec.resize(NumFrame);

	CmdQ = unique_ptr<CommandQueue>(new CommandQueue(
		this,
		Device.Get(),
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		L"Corona Graphics Queue",
		L"Corona Command Allocator",
		L"Corona Command List"
#if USE_AFTERMATH
		, bAftermathEnabled
#endif
	));
	AsyncRtCmdQ = unique_ptr<CommandQueue>(new CommandQueue(
		this,
		Device.Get(),
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		L"Corona Async RT Queue",
		L"Corona Async RT Command Allocator",
		L"Corona Async RT Command List",
		false));

	{
		RTVDescriptorHeap = std::make_unique<DescriptorHeap>();
		RTVDescriptorHeap->bShaderVisible = false;
		D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
		HeapDesc.NumDescriptors = 100;
		HeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		RTVDescriptorHeap->Init(Device.Get(), HeapDesc);

		NAME_D3D12_OBJECT(RTVDescriptorHeap->DH);
	}
	

	{
		DSVDescriptorHeap = std::make_unique<DescriptorHeap>();
		DSVDescriptorHeap->bShaderVisible = false;
		D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
		HeapDesc.NumDescriptors = 2;
		HeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		DSVDescriptorHeap->Init(Device.Get(), HeapDesc);

		NAME_D3D12_OBJECT(DSVDescriptorHeap->DH);
	}
	
	// shader visible CBV_SRV_UAV
	{
		SRVCBVDescriptorHeapShaderVisible = std::make_unique<DescriptorHeap>();

		D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
		HeapDesc.NumDescriptors = 1000000;
		HeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		SRVCBVDescriptorHeapShaderVisible->Init(Device.Get(), HeapDesc);

		NAME_D3D12_OBJECT(SRVCBVDescriptorHeapShaderVisible->DH);

		SRVCBVDescriptorHeapShaderVisible->AllocDescriptors(
			BindlessTextureTableCpuBase,
			BindlessTextureTableGpuBase,
			kMaxDX12BindlessTextureSlots);
		bBindlessTextureTableAllocated = true;

		SRVCBVDescriptorHeapShaderVisible->AllocDescriptors(
			BindlessBufferTableCpuBase,
			BindlessBufferTableGpuBase,
			kMaxDX12BindlessBufferSlots);
		bBindlessBufferTableAllocated = true;
	}

	// non shader visible(storage) CBV_SRV_UAV
	{
		SRVCBVDescriptorHeapStorage = std::make_unique<DescriptorHeap>();
		SRVCBVDescriptorHeapStorage->bShaderVisible = false;
		D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
		HeapDesc.NumDescriptors = 2000000;
		HeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		SRVCBVDescriptorHeapStorage->Init(Device.Get(), HeapDesc);

		NAME_D3D12_OBJECT(SRVCBVDescriptorHeapStorage->DH);
	}

	// shader visible sampler
	{
		SamplerDescriptorHeapShaderVisible = std::make_unique<DescriptorHeap>();

		D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
		HeapDesc.NumDescriptors = 1024*2;
		HeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
		HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		SamplerDescriptorHeapShaderVisible->Init(Device.Get(), HeapDesc);

		NAME_D3D12_OBJECT(SamplerDescriptorHeapShaderVisible->DH);
	}
	
	GlobalDHRing = std::make_unique<DescriptorHeapRing>();
	GlobalDHRing->Init(SRVCBVDescriptorHeapShaderVisible.get(), 10000, NumFrame);

	TextureDHRing = std::make_unique<DescriptorHeapRing>();
	TextureDHRing->Init(SRVCBVDescriptorHeapShaderVisible.get(), 10000, NumFrame);

	GeomtryDHRing = std::make_unique<DescriptorHeapRing>();
	GeomtryDHRing->Init(SRVCBVDescriptorHeapShaderVisible.get(), 10000, NumFrame);

	GlobalCBRing = std::make_unique<ConstantBufferRingBuffer>(Device.Get(), 1024 * 1024 * 10, NumFrame);
	
	CmdQ->WaitGPU();
}

bool DX12Backend::SupportsRayTracing() const
{
	if (!Device)
		return false;

	D3D12_FEATURE_DATA_D3D12_OPTIONS5 features5 = {};
	const HRESULT hr = Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &features5, sizeof(features5));
	return SUCCEEDED(hr) && features5.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
}

bool DX12Backend::SupportsShaderExecutionReordering() const
{
	if (!Device)
		return false;

	D3D12_FEATURE_DATA_SHADER_MODEL shaderModel = {};
	shaderModel.HighestShaderModel = D3D_SHADER_MODEL_6_9;
	const HRESULT hr = Device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel));
	return SUCCEEDED(hr) && shaderModel.HighestShaderModel >= D3D_SHADER_MODEL_6_9;
}

RHITextureHandle DX12Backend::RegisterBindlessTexture(Texture* texture)
{
	if (!texture || !Device || !SRVCBVDescriptorHeapShaderVisible || !bBindlessTextureTableAllocated)
		return {};

	{
		std::lock_guard<std::mutex> lock(BindlessTextureMutex);
		const RHITextureHandle existing = texture->BindlessHandle;
		if (existing.IsValid() &&
			existing.Index < BindlessTextureSlots.size() &&
			BindlessTextureSlots[existing.Index].Occupied &&
			BindlessTextureSlots[existing.Index].Generation == existing.Generation &&
			BindlessTextureSlots[existing.Index].TexturePtr == texture)
		{
			return existing;
		}

		uint32_t slotIndex = RHI_INVALID_BINDLESS_INDEX;
		if (!BindlessTextureFreeList.empty())
		{
			slotIndex = BindlessTextureFreeList.back();
			BindlessTextureFreeList.pop_back();
		}
		else
		{
			if (BindlessTextureSlots.size() >= kMaxDX12BindlessTextureSlots)
				return {};
			slotIndex = static_cast<uint32_t>(BindlessTextureSlots.size());
			BindlessTextureSlots.emplace_back();
			DX12BindlessTextureSlot& newSlot = BindlessTextureSlots.back();
			const SIZE_T slotOffset = static_cast<SIZE_T>(slotIndex) * SRVCBVDescriptorHeapShaderVisible->DescriptorSize;
			newSlot.CpuHandleSRV.ptr = BindlessTextureTableCpuBase.ptr + slotOffset;
			newSlot.GpuHandleSRV.ptr = BindlessTextureTableGpuBase.ptr + slotOffset;
		}

		DX12BindlessTextureSlot& slot = BindlessTextureSlots[slotIndex];
		if (slot.Generation == 0)
			slot.Generation = 1;
		slot.TexturePtr = texture;
		slot.Occupied = true;
		texture->BindlessHandle = { slotIndex, slot.Generation };
		texture->CpuHandleBindlessSRV = slot.CpuHandleSRV;
		texture->GpuHandleBindlessSRV = slot.GpuHandleSRV;
	}

	if (!UpdateBindlessTexture(texture))
		return {};
	return texture->BindlessHandle;
}

bool DX12Backend::UpdateBindlessTexture(Texture* texture)
{
	if (!texture || !Device || !texture->resource)
		return false;

	std::lock_guard<std::mutex> lock(BindlessTextureMutex);
	const RHITextureHandle handle = texture->BindlessHandle;
	if (!handle.IsValid() ||
		handle.Index >= BindlessTextureSlots.size() ||
		!BindlessTextureSlots[handle.Index].Occupied ||
		BindlessTextureSlots[handle.Index].Generation != handle.Generation)
	{
		return false;
	}

	DX12BindlessTextureSlot& slot = BindlessTextureSlots[handle.Index];
	slot.TexturePtr = texture;
	texture->CpuHandleBindlessSRV = slot.CpuHandleSRV;
	texture->GpuHandleBindlessSRV = slot.GpuHandleSRV;

	const D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = MakeTextureSRVDesc(*texture);
	Device->CreateShaderResourceView(texture->resource.Get(), &srvDesc, slot.CpuHandleSRV);
	return true;
}

void DX12Backend::UnregisterBindlessTexture(Texture* texture)
{
	if (!texture)
		return;

	std::lock_guard<std::mutex> lock(BindlessTextureMutex);
	const RHITextureHandle handle = texture->BindlessHandle;
	if (handle.IsValid() &&
		handle.Index < BindlessTextureSlots.size())
	{
		DX12BindlessTextureSlot& slot = BindlessTextureSlots[handle.Index];
		if (slot.Occupied &&
			slot.Generation == handle.Generation &&
			slot.TexturePtr == texture)
		{
			if (Device)
			{
				D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc = {};
				nullSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				nullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				nullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				nullSrvDesc.Texture2D.MipLevels = 1;
				Device->CreateShaderResourceView(nullptr, &nullSrvDesc, slot.CpuHandleSRV);
			}
			slot.TexturePtr = nullptr;
			slot.Occupied = false;
			++slot.Generation;
			if (slot.Generation == 0)
				slot.Generation = 1;
			BindlessTextureFreeList.push_back(handle.Index);
		}
	}

	texture->BindlessHandle = {};
	texture->CpuHandleBindlessSRV = {};
	texture->GpuHandleBindlessSRV = {};
	texture->Owner = nullptr;
}

RHITextureHandle DX12Backend::GetBindlessTextureHandle(const Texture* texture) const
{
	if (!texture)
		return {};

	std::lock_guard<std::mutex> lock(BindlessTextureMutex);
	const RHITextureHandle handle = texture->BindlessHandle;
	if (!handle.IsValid() ||
		handle.Index >= BindlessTextureSlots.size() ||
		!BindlessTextureSlots[handle.Index].Occupied ||
		BindlessTextureSlots[handle.Index].Generation != handle.Generation ||
		BindlessTextureSlots[handle.Index].TexturePtr != texture)
	{
		return {};
	}
	return handle;
}

namespace
{
	bool IsValidCpuDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle)
	{
		return handle.ptr != 0;
	}

	void WriteNullRawBufferSRV(ID3D12Device5* device, D3D12_CPU_DESCRIPTOR_HANDLE destination)
	{
		if (!device || !IsValidCpuDescriptor(destination))
			return;

		D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc = {};
		nullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		nullSrvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
		nullSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
		nullSrvDesc.Buffer.StructureByteStride = 0;
		nullSrvDesc.Buffer.FirstElement = 0;
		nullSrvDesc.Buffer.NumElements = 1;
		nullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		device->CreateShaderResourceView(nullptr, &nullSrvDesc, destination);
	}

	RHIBufferHandle RegisterDX12BindlessBufferDescriptor(
		DX12Backend& owner,
		const void* bufferPtr,
		uint8_t resourceType,
		RHIBufferHandle& bindlessHandle,
		D3D12_CPU_DESCRIPTOR_HANDLE sourceCpuHandle,
		D3D12_CPU_DESCRIPTOR_HANDLE& bindlessCpuHandle,
		D3D12_GPU_DESCRIPTOR_HANDLE& bindlessGpuHandle)
	{
		if (!bufferPtr || !owner.Device || !owner.SRVCBVDescriptorHeapShaderVisible ||
			!owner.bBindlessBufferTableAllocated || !IsValidCpuDescriptor(sourceCpuHandle))
		{
			return {};
		}

		{
			std::lock_guard<std::mutex> lock(owner.BindlessBufferMutex);
			const RHIBufferHandle existing = bindlessHandle;
			if (existing.IsValid() &&
				existing.Index < owner.BindlessBufferSlots.size() &&
				owner.BindlessBufferSlots[existing.Index].Occupied &&
				owner.BindlessBufferSlots[existing.Index].Generation == existing.Generation &&
				owner.BindlessBufferSlots[existing.Index].BufferPtr == bufferPtr)
			{
				return existing;
			}

			uint32_t slotIndex = RHI_INVALID_BINDLESS_INDEX;
			if (!owner.BindlessBufferFreeList.empty())
			{
				slotIndex = owner.BindlessBufferFreeList.back();
				owner.BindlessBufferFreeList.pop_back();
			}
			else
			{
				if (owner.BindlessBufferSlots.size() >= DX12Backend::kMaxDX12BindlessBufferSlots)
					return {};
				slotIndex = static_cast<uint32_t>(owner.BindlessBufferSlots.size());
				owner.BindlessBufferSlots.emplace_back();
				DX12Backend::DX12BindlessBufferSlot& newSlot = owner.BindlessBufferSlots.back();
				const SIZE_T slotOffset = static_cast<SIZE_T>(slotIndex) * owner.SRVCBVDescriptorHeapShaderVisible->DescriptorSize;
				newSlot.CpuHandleSRV.ptr = owner.BindlessBufferTableCpuBase.ptr + slotOffset;
				newSlot.GpuHandleSRV.ptr = owner.BindlessBufferTableGpuBase.ptr + slotOffset;
			}

			DX12Backend::DX12BindlessBufferSlot& slot = owner.BindlessBufferSlots[slotIndex];
			if (slot.Generation == 0)
				slot.Generation = 1;
			slot.BufferPtr = bufferPtr;
			slot.ResourceType = resourceType;
			slot.Occupied = true;
			bindlessHandle = { slotIndex, slot.Generation };
			bindlessCpuHandle = slot.CpuHandleSRV;
			bindlessGpuHandle = slot.GpuHandleSRV;
			owner.Device->CopyDescriptorsSimple(
				1,
				slot.CpuHandleSRV,
				sourceCpuHandle,
				D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		}

		return bindlessHandle;
	}

	void UnregisterDX12BindlessBufferDescriptor(
		DX12Backend& owner,
		const void* bufferPtr,
		RHIBufferHandle& bindlessHandle,
		D3D12_CPU_DESCRIPTOR_HANDLE& bindlessCpuHandle,
		D3D12_GPU_DESCRIPTOR_HANDLE& bindlessGpuHandle)
	{
		std::lock_guard<std::mutex> lock(owner.BindlessBufferMutex);
		const RHIBufferHandle handle = bindlessHandle;
		if (handle.IsValid() &&
			handle.Index < owner.BindlessBufferSlots.size())
		{
			DX12Backend::DX12BindlessBufferSlot& slot = owner.BindlessBufferSlots[handle.Index];
			if (slot.Occupied &&
				slot.Generation == handle.Generation &&
				slot.BufferPtr == bufferPtr)
			{
				WriteNullRawBufferSRV(owner.Device.Get(), slot.CpuHandleSRV);
				slot.BufferPtr = nullptr;
				slot.ResourceType = 0;
				slot.Occupied = false;
				++slot.Generation;
				if (slot.Generation == 0)
					slot.Generation = 1;
				owner.BindlessBufferFreeList.push_back(handle.Index);
			}
		}

		bindlessHandle = {};
		bindlessCpuHandle = {};
		bindlessGpuHandle = {};
	}

	RHIBufferHandle GetDX12BindlessBufferDescriptorHandle(
		const DX12Backend& owner,
		const void* bufferPtr,
		const RHIBufferHandle& bindlessHandle)
	{
		if (!bufferPtr)
			return {};

		std::lock_guard<std::mutex> lock(owner.BindlessBufferMutex);
		if (!bindlessHandle.IsValid() ||
			bindlessHandle.Index >= owner.BindlessBufferSlots.size() ||
			!owner.BindlessBufferSlots[bindlessHandle.Index].Occupied ||
			owner.BindlessBufferSlots[bindlessHandle.Index].Generation != bindlessHandle.Generation ||
			owner.BindlessBufferSlots[bindlessHandle.Index].BufferPtr != bufferPtr)
		{
			return {};
		}
		return bindlessHandle;
	}
}

RHIBufferHandle DX12Backend::RegisterBindlessBuffer(Buffer* buffer)
{
	if (!buffer)
		return {};
	buffer->Owner = this;
	return RegisterDX12BindlessBufferDescriptor(
		*this,
		buffer,
		1,
		buffer->BindlessHandle,
		buffer->CpuHandleSRV,
		buffer->CpuHandleBindlessSRV,
		buffer->GpuHandleBindlessSRV);
}

RHIBufferHandle DX12Backend::RegisterBindlessVertexBuffer(VertexBuffer* buffer)
{
	if (!buffer)
		return {};
	buffer->Owner = this;
	return RegisterDX12BindlessBufferDescriptor(
		*this,
		buffer,
		2,
		buffer->BindlessHandle,
		buffer->CpuHandleSRV,
		buffer->CpuHandleBindlessSRV,
		buffer->GpuHandleBindlessSRV);
}

RHIBufferHandle DX12Backend::RegisterBindlessIndexBuffer(IndexBuffer* buffer)
{
	if (!buffer)
		return {};
	buffer->Owner = this;
	return RegisterDX12BindlessBufferDescriptor(
		*this,
		buffer,
		3,
		buffer->BindlessHandle,
		buffer->CpuHandleSRV,
		buffer->CpuHandleBindlessSRV,
		buffer->GpuHandleBindlessSRV);
}

void DX12Backend::UnregisterBindlessBuffer(Buffer* buffer)
{
	if (!buffer)
		return;
	UnregisterDX12BindlessBufferDescriptor(
		*this,
		buffer,
		buffer->BindlessHandle,
		buffer->CpuHandleBindlessSRV,
		buffer->GpuHandleBindlessSRV);
	buffer->Owner = nullptr;
}

void DX12Backend::UnregisterBindlessVertexBuffer(VertexBuffer* buffer)
{
	if (!buffer)
		return;
	UnregisterDX12BindlessBufferDescriptor(
		*this,
		buffer,
		buffer->BindlessHandle,
		buffer->CpuHandleBindlessSRV,
		buffer->GpuHandleBindlessSRV);
	buffer->Owner = nullptr;
}

void DX12Backend::UnregisterBindlessIndexBuffer(IndexBuffer* buffer)
{
	if (!buffer)
		return;
	UnregisterDX12BindlessBufferDescriptor(
		*this,
		buffer,
		buffer->BindlessHandle,
		buffer->CpuHandleBindlessSRV,
		buffer->GpuHandleBindlessSRV);
	buffer->Owner = nullptr;
}

RHIBufferHandle DX12Backend::GetBindlessBufferHandle(const Buffer* buffer) const
{
	if (!buffer)
		return {};
	return GetDX12BindlessBufferDescriptorHandle(*this, buffer, buffer->BindlessHandle);
}

RHIBufferHandle DX12Backend::GetBindlessVertexBufferHandle(const VertexBuffer* buffer) const
{
	if (!buffer)
		return {};
	return GetDX12BindlessBufferDescriptorHandle(*this, buffer, buffer->BindlessHandle);
}

RHIBufferHandle DX12Backend::GetBindlessIndexBufferHandle(const IndexBuffer* buffer) const
{
	if (!buffer)
		return {};
	return GetDX12BindlessBufferDescriptorHandle(*this, buffer, buffer->BindlessHandle);
}

RenderBackendAllocatorStats DX12Backend::GetAllocatorStats() const
{
	RenderBackendAllocatorStats stats{};
	stats.PersistentStructuredBlockCount = static_cast<uint32_t>(PersistentStructuredBufferBlocks.size());
	stats.PersistentStructuredBytesIssued = PersistentStructuredBufferBytesIssued;
	stats.PersistentStructuredPendingUploadBytes = PendingPersistentStructuredBufferUploadBytes;
	for (const std::shared_ptr<PersistentStructuredBufferBlock>& block : PersistentStructuredBufferBlocks)
	{
		if (!block)
			continue;
		stats.PersistentStructuredReservedBytes += block->capacity;
		stats.PersistentStructuredCommittedBytes += block->cursor;
		for (const PersistentStructuredBufferBlock::FreeRange& range : block->freeRanges)
		{
			stats.PersistentStructuredReusableBytes += range.size;
		}
	}
	for (const PendingPersistentStructuredBufferFree& pending : PendingPersistentStructuredBufferFrees)
	{
		stats.PersistentStructuredPendingFreeBytes += pending.size;
	}

	stats.TransientStructuredBlockCount = TransientUploadStructuredBlockCount;
	stats.TransientStructuredBytesIssued = TransientUploadStructuredBytesIssued;
	stats.TransientStructuredReservedBytes = TransientUploadStructuredBytesReserved;
	const uint32_t frameIndex = CurrentFrameIndex < TransientUploadStructuredBlocks.size() ? CurrentFrameIndex : 0u;
	if (frameIndex < TransientUploadStructuredBlocks.size())
	{
		for (const std::shared_ptr<TransientUploadStructuredBlock>& block : TransientUploadStructuredBlocks[frameIndex])
		{
			if (block)
				stats.TransientStructuredCurrentFrameBytes += block->cursor;
		}
	}

	{
		std::lock_guard<std::mutex> lock(BindlessTextureMutex);
		stats.BindlessTextureSlotsCapacity = static_cast<uint32_t>(BindlessTextureSlots.size());
		for (const DX12BindlessTextureSlot& slot : BindlessTextureSlots)
		{
			if (slot.Occupied)
				++stats.BindlessTextureSlotsUsed;
		}
	}
	{
		std::lock_guard<std::mutex> lock(BindlessBufferMutex);
		stats.BindlessBufferSlotsCapacity = static_cast<uint32_t>(BindlessBufferSlots.size());
		for (const DX12BindlessBufferSlot& slot : BindlessBufferSlots)
		{
			if (slot.Occupied)
				++stats.BindlessBufferSlotsUsed;
		}
	}
	return stats;
}

DX12Backend::~DX12Backend()
{
	CmdQ->WaitGPU();
	RetireCompletedPersistentStructuredBufferUploads();
	RetireCompletedPersistentStructuredBufferFrees();
	{
		std::lock_guard<std::mutex> lock(BindlessTextureMutex);
		for (DX12BindlessTextureSlot& slot : BindlessTextureSlots)
		{
			if (slot.TexturePtr)
			{
				slot.TexturePtr->BindlessHandle = {};
				slot.TexturePtr->CpuHandleBindlessSRV = {};
				slot.TexturePtr->GpuHandleBindlessSRV = {};
				slot.TexturePtr->Owner = nullptr;
			}
			slot.TexturePtr = nullptr;
			slot.Occupied = false;
		}
		BindlessTextureFreeList.clear();
	}
	{
		std::lock_guard<std::mutex> lock(BindlessBufferMutex);
		for (DX12BindlessBufferSlot& slot : BindlessBufferSlots)
		{
			if (slot.BufferPtr)
			{
				if (slot.ResourceType == 1)
				{
					Buffer* buffer = const_cast<Buffer*>(static_cast<const Buffer*>(slot.BufferPtr));
					buffer->BindlessHandle = {};
					buffer->CpuHandleBindlessSRV = {};
					buffer->GpuHandleBindlessSRV = {};
					buffer->Owner = nullptr;
				}
				else if (slot.ResourceType == 2)
				{
					VertexBuffer* buffer = const_cast<VertexBuffer*>(static_cast<const VertexBuffer*>(slot.BufferPtr));
					buffer->BindlessHandle = {};
					buffer->CpuHandleBindlessSRV = {};
					buffer->GpuHandleBindlessSRV = {};
					buffer->Owner = nullptr;
				}
				else if (slot.ResourceType == 3)
				{
					IndexBuffer* buffer = const_cast<IndexBuffer*>(static_cast<const IndexBuffer*>(slot.BufferPtr));
					buffer->BindlessHandle = {};
					buffer->CpuHandleBindlessSRV = {};
					buffer->GpuHandleBindlessSRV = {};
					buffer->Owner = nullptr;
				}
			}
			slot.BufferPtr = nullptr;
			slot.ResourceType = 0;
			slot.Occupied = false;
		}
		BindlessBufferFreeList.clear();
	}
	ShutdownOcclusionQueries();
	ShutdownGpuTimestampQueries();
}

static RHIBindingDesc MakeLegacyRHIBindingDesc(
	const string& name,
	RHIDescriptorKind descriptorKind,
	RHIResourceKind resourceKind,
	uint32_t baseRegister,
	uint32_t descriptorCount,
	uint32_t sizeInBytes = 0)
{
	RHIBindingDesc binding{};
	binding.Name = name;
	binding.DescriptorKind = descriptorKind;
	binding.ResourceKind = resourceKind;
	binding.Access = descriptorKind == RHIDescriptorKind::UAV ? RHIDescriptorAccess::ReadWrite : RHIDescriptorAccess::ReadOnly;
	binding.RegisterIndex = baseRegister;
	binding.DescriptorCount = descriptorCount;
	binding.SizeInBytes = sizeInBytes;
	return binding;
}

static UINT ToD3D12DescriptorCount(const RHIBindingDesc& binding)
{
	if (binding.RuntimeArray || binding.DescriptorCount == RHI_BINDLESS_ARRAY)
		return UINT_MAX;
	return binding.DescriptorCount == 0 ? 1 : binding.DescriptorCount;
}

static bool RequiresDX12VertexBindlessBufferShaderModel66(const GraphicsPipelineDesc& desc)
{
	const RHIShaderStageMask vertexStage = ToRHIShaderStageMask(RHIShaderStage::Vertex);
	for (const RHIBindingDesc& binding : desc.PipelineLayout.Bindings)
	{
		if ((binding.Stages & vertexStage) == 0)
			continue;
		if (binding.DescriptorKind != RHIDescriptorKind::SRV ||
			binding.ResourceKind != RHIResourceKind::Buffer)
		{
			continue;
		}
		if (binding.RuntimeArray || binding.DescriptorCount == RHI_BINDLESS_ARRAY)
			return true;
	}
	return false;
}

static UINT ToD3D12RegisterSpace(const RHIBindingDesc& binding)
{
	return binding.RegisterSpace;
}

static D3D12_SHADER_VISIBILITY ToD3D12GraphicsShaderVisibility(const RHIBindingDesc& binding)
{
	const RHIShaderStageMask vertexStage = ToRHIShaderStageMask(RHIShaderStage::Vertex);
	const RHIShaderStageMask pixelStage = ToRHIShaderStageMask(RHIShaderStage::Pixel);
	const RHIShaderStageMask graphicsStages = vertexStage | pixelStage;
	const RHIShaderStageMask stages = binding.Stages;

	if ((stages & ~graphicsStages) != 0)
		return D3D12_SHADER_VISIBILITY_ALL;
	if ((stages & vertexStage) != 0 && (stages & pixelStage) == 0)
		return D3D12_SHADER_VISIBILITY_VERTEX;
	if ((stages & pixelStage) != 0 && (stages & vertexStage) == 0)
		return D3D12_SHADER_VISIBILITY_PIXEL;
	return D3D12_SHADER_VISIBILITY_ALL;
}

static D3D12_SHADER_VISIBILITY ToD3D12ShaderVisibilityForPipeline(const RHIBindingDesc& binding, bool isCompute)
{
	if (isCompute)
		return D3D12_SHADER_VISIBILITY_ALL;
	return ToD3D12GraphicsShaderVisibility(binding);
}

void PipelineStateObject::BindUAV(string name, int baseRegister)
{
	BindingData binding;
	binding.name = name;
	binding.baseRegister = baseRegister;
	binding.numDescriptors = 1;
	binding.Schema = MakeLegacyRHIBindingDesc(name, RHIDescriptorKind::UAV, RHIResourceKind::Unknown, static_cast<uint32_t>(std::max(baseRegister, 0)), 1);
	uavBinding.insert(pair<string, BindingData>(name, binding));
}

void PipelineStateObject::BindSRV(string name, int baseRegister, int num)
{
	BindingData binding;
	binding.name = name;
	binding.baseRegister = baseRegister;
	binding.numDescriptors = num;
	binding.Schema = MakeLegacyRHIBindingDesc(name, RHIDescriptorKind::SRV, RHIResourceKind::Unknown, static_cast<uint32_t>(std::max(baseRegister, 0)), static_cast<uint32_t>(std::max(num, 0)));
	textureBinding.insert(pair<string, BindingData>(name, binding));
}

void PipelineStateObject::BindCBV(string name, int baseRegister, int size)
{
	BindingData binding;
	binding.name = name;
	binding.baseRegister = baseRegister;
	binding.numDescriptors = 1;
	binding.sourceSize = static_cast<UINT>(std::max(size, 0));
	binding.cbSize = AlignConstantBufferSize(binding.sourceSize);
	binding.Schema = MakeLegacyRHIBindingDesc(name, RHIDescriptorKind::CBV, RHIResourceKind::ConstantBuffer, static_cast<uint32_t>(std::max(baseRegister, 0)), 1, binding.sourceSize);

	constantBufferBinding.insert(pair<string, BindingData>(name, binding));
}

void PipelineStateObject::BindRootConstant(string name, int baseRegister)
{
	BindingData binding;
	binding.name = name;
	binding.baseRegister = baseRegister;
	binding.numDescriptors = 1;
	binding.Schema = MakeLegacyRHIBindingDesc(name, RHIDescriptorKind::CBV, RHIResourceKind::ConstantBuffer, static_cast<uint32_t>(std::max(baseRegister, 0)), 1);

	rootBinding.insert(pair<string, BindingData>(name, binding));
}

void PipelineStateObject::BindSampler(string name, int baseRegister)
{
	BindingData binding;
	binding.name = name;
	binding.baseRegister = baseRegister;
	binding.numDescriptors = 1;
	binding.Schema = MakeLegacyRHIBindingDesc(name, RHIDescriptorKind::Sampler, RHIResourceKind::Sampler, static_cast<uint32_t>(std::max(baseRegister, 0)), 1);

	samplerBinding.insert(pair<string, BindingData>(name, binding));
}

namespace
{
	ID3D12GraphicsCommandList* ResolveGraphicsCommandList(DX12Backend* owner, ID3D12GraphicsCommandList* CommandList)
	{
		if (CommandList)
			return CommandList;
		assert(owner && owner->GlobalCmdList);
		return owner->GlobalCmdList->CmdList.Get();
	}

	CommandList* ResolveCommandList(DX12Backend* owner, CommandList* commandList)
	{
		if (commandList)
			return commandList;
		assert(owner && owner->GlobalCmdList);
		return owner->GlobalCmdList;
	}
}

void PipelineStateObject::SetSRV(string name, D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleSRV, ID3D12GraphicsCommandList* CommandList)
{
	CommandList = ResolveGraphicsCommandList(Owner, CommandList);
	assert(CommandList);

	// std::map::operator[] would silently create a default BindingData
	// (rootParamIndex=0) for an unknown name, then write SRV to root slot 0
	// — which on PSOs whose root sig puts a CBV at slot 0 (e.g. procedural
	// grass with no textures) clobbers the CB. Guard explicitly.
	auto it = textureBinding.find(name);
	if (it == textureBinding.end())
		return;
	UINT RPI = it->second.rootParamIndex;
	if (IsCompute)
	{
		if (Owner)
			Owner->SetComputeRootDescriptorTableIfNeeded(CommandList, RPI, GpuHandleSRV);
		else
			CommandList->SetComputeRootDescriptorTable(RPI, GpuHandleSRV);
	}
	else
		CommandList->SetGraphicsRootDescriptorTable(RPI, GpuHandleSRV);
}

void PipelineStateObject::SetUAV(string name, D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleUAV, ID3D12GraphicsCommandList* CommandList)
{
	//uavBinding[name].texture = texture;
	CommandList = ResolveGraphicsCommandList(Owner, CommandList);
	assert(CommandList);
	D3D12_CPU_DESCRIPTOR_HANDLE ShaderVisibleCPUHandle;
	D3D12_GPU_DESCRIPTOR_HANDLE ShaderVisibleGpuHandle;

	UINT RPI = uavBinding[name].rootParamIndex;
	if (IsCompute)
	{
		if (Owner)
			Owner->SetComputeRootDescriptorTableIfNeeded(CommandList, RPI, GpuHandleUAV);
		else
			CommandList->SetComputeRootDescriptorTable(RPI, GpuHandleUAV);
	}
	else
		CommandList->SetGraphicsRootDescriptorTable(RPI, GpuHandleUAV);
}
void PipelineStateObject::SetSampler(string name, Sampler* sampler, ID3D12GraphicsCommandList* CommandList)
{
	CommandList = ResolveGraphicsCommandList(Owner, CommandList);
	assert(CommandList);
	// Same guard as SetSRV — silently creating an entry on a PSO that
	// doesn't expose this sampler would write to root slot 0 and clobber
	// the CB descriptor.
	auto it = samplerBinding.find(name);
	if (it == samplerBinding.end())
		return;
	it->second.sampler = sampler;
	if (IsCompute)
	{
		if (Owner)
			Owner->SetComputeRootDescriptorTableIfNeeded(CommandList, it->second.rootParamIndex, sampler->GpuHandle);
		else
			CommandList->SetComputeRootDescriptorTable(it->second.rootParamIndex, sampler->GpuHandle);
	}
	else
		CommandList->SetGraphicsRootDescriptorTable(it->second.rootParamIndex, sampler->GpuHandle);
}

void PipelineStateObject::SetCBVValue(string name, void* pData, ID3D12GraphicsCommandList* CommandList)
{
	CommandList = ResolveGraphicsCommandList(Owner, CommandList);
	assert(CommandList);
	map<string, BindingData> ::iterator it = constantBufferBinding.find(name);
	assert(it != constantBufferBinding.end());
	
	BindingData& binding = it->second;// constantBufferBinding[name];
	DX12Backend* owner = Owner;
	assert(owner);
	auto Alloc = owner->GlobalCBRing->AllocGPUMemory(binding.cbSize);
	UINT64 GPUAddr = std::get<0>(Alloc);
	UINT8* pMapped = std::get<1>(Alloc);

	CopyConstantBufferData(pMapped, binding.cbSize, pData, binding.sourceSize);

	if (IsCompute)
		CommandList->SetComputeRootConstantBufferView(binding.rootParamIndex, GPUAddr);
	else
		CommandList->SetGraphicsRootConstantBufferView(binding.rootParamIndex, GPUAddr);
}

void PipelineStateObject::SetRootConstant(string name, UINT value, ID3D12GraphicsCommandList* CommandList)
{
	CommandList = ResolveGraphicsCommandList(Owner, CommandList);
	assert(CommandList);
	rootBinding[name].rootConst = value;
	if(IsCompute)
		CommandList->SetComputeRoot32BitConstant(rootBinding[name].rootParamIndex, rootBinding[name].rootConst, 0);
	else
		CommandList->SetGraphicsRoot32BitConstant(rootBinding[name].rootParamIndex, rootBinding[name].rootConst, 0);
}

bool PipelineStateObject::Init()
{
	if (!IsCompute && (!vs.IsValid() || !ps.IsValid())) return false;
	if (IsCompute && !cs.IsValid()) return false;


	vector<CD3DX12_ROOT_PARAMETER1> rootParamVec;

	vector<CD3DX12_DESCRIPTOR_RANGE1> TextureRanges;
	if (textureBinding.size() != 0)
	{
		TextureRanges.resize(textureBinding.size());
		int i = 0;
		for (auto& bindingPair : textureBinding)
		{
			PipelineStateObject::BindingData& bindingData = bindingPair.second;

			CD3DX12_ROOT_PARAMETER1 TextureParam;
			TextureRanges[i].Init(
				D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
				ToD3D12DescriptorCount(bindingData.Schema),
				bindingData.baseRegister,
				ToD3D12RegisterSpace(bindingData.Schema),
				D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
			TextureParam.InitAsDescriptorTable(
				1,
				&TextureRanges[i],
				ToD3D12ShaderVisibilityForPipeline(bindingData.Schema, IsCompute));
			rootParamVec.push_back(TextureParam);
			i++;
			bindingData.rootParamIndex = RootParamIndex++;
		}
	}

	vector<CD3DX12_DESCRIPTOR_RANGE1> SamplerRanges;
	if (samplerBinding.size() != 0)
	{
		SamplerRanges.resize(samplerBinding.size());
		int i = 0;
		for (auto& bindingPair : samplerBinding)
		{
			PipelineStateObject::BindingData& bindingData = bindingPair.second;

			CD3DX12_ROOT_PARAMETER1 SamplerParam;
			SamplerRanges[i].Init(
				D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,
				ToD3D12DescriptorCount(bindingData.Schema),
				bindingData.baseRegister,
				ToD3D12RegisterSpace(bindingData.Schema));
			SamplerParam.InitAsDescriptorTable(
				1,
				&SamplerRanges[i],
				ToD3D12ShaderVisibilityForPipeline(bindingData.Schema, IsCompute));
			rootParamVec.push_back(SamplerParam);
			i++;
			bindingData.rootParamIndex = RootParamIndex++;
		}
	}


	if (rootBinding.size() != 0)
	{
		int i = 0;
		for (auto& bindingPair : rootBinding)
		{
			PipelineStateObject::BindingData& bindingData = bindingPair.second;

			CD3DX12_ROOT_PARAMETER1 ConstantParam;
			ConstantParam.InitAsConstants(
				bindingData.numDescriptors,
				bindingData.baseRegister,
				ToD3D12RegisterSpace(bindingData.Schema),
				ToD3D12ShaderVisibilityForPipeline(bindingData.Schema, IsCompute));
			rootParamVec.push_back(ConstantParam);
			i++;
			bindingData.rootParamIndex = RootParamIndex++;
		}
	}

	if (constantBufferBinding.size() != 0)
	{
		for (auto& bindingPair : constantBufferBinding)
		{
			PipelineStateObject::BindingData& bindingData = bindingPair.second;

			CD3DX12_ROOT_PARAMETER1 CBParam;
			CBParam.InitAsConstantBufferView(
				bindingData.baseRegister,
				ToD3D12RegisterSpace(bindingData.Schema),
				D3D12_ROOT_DESCRIPTOR_FLAG_NONE,
				ToD3D12ShaderVisibilityForPipeline(bindingData.Schema, IsCompute));

			rootParamVec.push_back(CBParam);
			bindingData.rootParamIndex = RootParamIndex++;
		}
	}

	vector<CD3DX12_DESCRIPTOR_RANGE1> UAVRanges;
	if (uavBinding.size() != 0)
	{
		UAVRanges.resize(uavBinding.size());
		int i = 0;
		for (auto& bindingPair : uavBinding)
		{
			PipelineStateObject::BindingData& bindingData = bindingPair.second;
			CD3DX12_ROOT_PARAMETER1 TextureParam;
			UAVRanges[i].Init(
				D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
				ToD3D12DescriptorCount(bindingData.Schema),
				bindingData.baseRegister,
				ToD3D12RegisterSpace(bindingData.Schema),
				D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
			TextureParam.InitAsDescriptorTable(
				1,
				&UAVRanges[i],
				ToD3D12ShaderVisibilityForPipeline(bindingData.Schema, IsCompute));
			rootParamVec.push_back(TextureParam);
			i++;
			bindingData.rootParamIndex = RootParamIndex++;
		}
	}

	CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
	rootSignatureDesc.Init_1_1(rootParamVec.size(), &rootParamVec[0], 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = {};
	featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;

	DX12Backend* owner = Owner;
	assert(owner);
	if (FAILED(owner->Device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData))))
	{
		featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
	}

	ComPtr<ID3DBlob> signature;
	ComPtr<ID3DBlob> error;

	try
	{
		ThrowIfFailed(D3DX12SerializeVersionedRootSignature(&rootSignatureDesc, featureData.HighestVersion, &signature, &error));
	}
	catch (const std::exception& e)
	{
		OutputDebugStringA(reinterpret_cast<const char*>(error->GetBufferPointer()));
	}

	try
	{
		ThrowIfFailed(owner->Device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&RS)));
	}
	catch (const std::exception& e)
	{
		OutputDebugStringA(reinterpret_cast<const char*>(error->GetBufferPointer()));
	}

	const std::wstring rootSignatureName =
		DebugName.empty()
		? (IsCompute ? L"ComputeRootSignature" : L"GraphicsRootSignature")
		: (L"RootSignature: " + DebugName);
	SetName(RS.Get(), rootSignatureName.c_str());

	// Build the driver-PSO cache key from everything that feeds the compiled
	// pipeline: shader bytecode (which already encodes the source), the
	// serialized root signature, the relevant fixed-function state and the
	// adapter LUID (cached blobs are device-specific). A key match is not
	// trusted blindly — if the blob is stale Create*PipelineState fails and we
	// recompile, so the key only needs to be unique enough to avoid collisions.
	uint64_t psoKey = PipelineCache::kFnvOffset;
	psoKey = PipelineCache::HashBytes(&PipelineCache::kPsoCacheVersion, sizeof(PipelineCache::kPsoCacheVersion), psoKey);
	{
		const LUID adapterLuid = owner->Device->GetAdapterLuid();
		psoKey = PipelineCache::HashBytes(&adapterLuid, sizeof(adapterLuid), psoKey);
	}
	if (signature)
		psoKey = PipelineCache::HashBytes(signature->GetBufferPointer(), signature->GetBufferSize(), psoKey);
	if (IsCompute)
	{
		psoKey = PipelineCache::HashBytes(cs.GetPointer(), cs.GetSize(), psoKey);
	}
	else
	{
		psoKey = PipelineCache::HashBytes(vs.GetPointer(), vs.GetSize(), psoKey);
		psoKey = PipelineCache::HashBytes(ps.GetPointer(), ps.GetSize(), psoKey);
		psoKey = PipelineCache::HashBytes(&graphicsPSODesc.BlendState, sizeof(graphicsPSODesc.BlendState), psoKey);
		psoKey = PipelineCache::HashBytes(&graphicsPSODesc.RasterizerState, sizeof(graphicsPSODesc.RasterizerState), psoKey);
		psoKey = PipelineCache::HashBytes(&graphicsPSODesc.DepthStencilState, sizeof(graphicsPSODesc.DepthStencilState), psoKey);
		psoKey = PipelineCache::HashBytes(&graphicsPSODesc.SampleMask, sizeof(graphicsPSODesc.SampleMask), psoKey);
		psoKey = PipelineCache::HashBytes(&graphicsPSODesc.PrimitiveTopologyType, sizeof(graphicsPSODesc.PrimitiveTopologyType), psoKey);
		psoKey = PipelineCache::HashBytes(&graphicsPSODesc.NumRenderTargets, sizeof(graphicsPSODesc.NumRenderTargets), psoKey);
		psoKey = PipelineCache::HashBytes(graphicsPSODesc.RTVFormats, sizeof(graphicsPSODesc.RTVFormats), psoKey);
		psoKey = PipelineCache::HashBytes(&graphicsPSODesc.DSVFormat, sizeof(graphicsPSODesc.DSVFormat), psoKey);
		psoKey = PipelineCache::HashBytes(&graphicsPSODesc.SampleDesc, sizeof(graphicsPSODesc.SampleDesc), psoKey);
		for (UINT elementIndex = 0; elementIndex < graphicsPSODesc.InputLayout.NumElements; ++elementIndex)
		{
			const D3D12_INPUT_ELEMENT_DESC& element = graphicsPSODesc.InputLayout.pInputElementDescs[elementIndex];
			if (element.SemanticName)
				psoKey = PipelineCache::HashBytes(element.SemanticName, std::strlen(element.SemanticName), psoKey);
			psoKey = PipelineCache::HashBytes(&element.SemanticIndex, sizeof(element.SemanticIndex), psoKey);
			psoKey = PipelineCache::HashBytes(&element.Format, sizeof(element.Format), psoKey);
			psoKey = PipelineCache::HashBytes(&element.AlignedByteOffset, sizeof(element.AlignedByteOffset), psoKey);
		}
	}

	const bool cacheEnabled = PipelineCache::Enabled();
	const std::filesystem::path psoCachePath = PipelineCache::CachePath(psoKey, L".pso");
	// Holds the cached blob bytes; must outlive the Create*PipelineState call
	// because D3D12_CACHED_PIPELINE_STATE only borrows the pointer.
	std::vector<uint8_t> cachedPsoBlob;
	if (cacheEnabled)
		PipelineCache::ReadFile(psoCachePath, cachedPsoBlob);

	const auto storePsoBlob = [&]()
	{
		if (!cacheEnabled || !PSO)
			return;
		ComPtr<ID3DBlob> blob;
		if (SUCCEEDED(PSO->GetCachedBlob(&blob)) && blob)
			PipelineCache::WriteFile(psoCachePath, blob->GetBufferPointer(), blob->GetBufferSize());
	};

	if (IsCompute)
	{
		computePSODesc.CS = CD3DX12_SHADER_BYTECODE(cs.GetPointer(), cs.GetSize());
		computePSODesc.pRootSignature = RS.Get();
		if (!cachedPsoBlob.empty())
		{
			computePSODesc.CachedPSO.pCachedBlob = cachedPsoBlob.data();
			computePSODesc.CachedPSO.CachedBlobSizeInBytes = cachedPsoBlob.size();
		}
		HRESULT hr = owner->Device->CreateComputePipelineState(&computePSODesc, IID_PPV_ARGS(&PSO));
		if (FAILED(hr) && !cachedPsoBlob.empty())
		{
			// Stale cached blob (driver/HW change). Discard it and recompile.
			computePSODesc.CachedPSO = {};
			cachedPsoBlob.clear();
			hr = owner->Device->CreateComputePipelineState(&computePSODesc, IID_PPV_ARGS(&PSO));
		}
		ThrowIfFailed(hr);
		const std::wstring psoName = DebugName.empty() ? L"ComputePSO" : DebugName;
		SetName(PSO.Get(), psoName.c_str());
		if (cachedPsoBlob.empty())
			storePsoBlob();
		return SUCCEEDED(hr);

	}
	else
	{
		graphicsPSODesc.VS = CD3DX12_SHADER_BYTECODE(vs.GetPointer(), vs.GetSize());
		graphicsPSODesc.PS = CD3DX12_SHADER_BYTECODE(ps.GetPointer(), ps.GetSize());

		graphicsPSODesc.pRootSignature = RS.Get();
		if (!cachedPsoBlob.empty())
		{
			graphicsPSODesc.CachedPSO.pCachedBlob = cachedPsoBlob.data();
			graphicsPSODesc.CachedPSO.CachedBlobSizeInBytes = cachedPsoBlob.size();
		}
		HRESULT hr = owner->Device->CreateGraphicsPipelineState(&graphicsPSODesc, IID_PPV_ARGS(&PSO));
		if (FAILED(hr) && !cachedPsoBlob.empty())
		{
			graphicsPSODesc.CachedPSO = {};
			cachedPsoBlob.clear();
			hr = owner->Device->CreateGraphicsPipelineState(&graphicsPSODesc, IID_PPV_ARGS(&PSO));
		}
		ThrowIfFailed(hr);
		const std::wstring psoName = DebugName.empty() ? L"GraphicsPSO" : DebugName;
		SetName(PSO.Get(), psoName.c_str());
		if (cachedPsoBlob.empty())
			storePsoBlob();
		return SUCCEEDED(hr);
	}
}

void PipelineStateObject::Apply(ID3D12GraphicsCommandList* CommandList)
{
	CommandList = ResolveGraphicsCommandList(Owner, CommandList);
	assert(CommandList);
	if (IsCompute)
	{
		if (Owner)
		{
			Owner->InvalidateGraphicsCommandStateCache();
			Owner->SetComputeRootSignatureIfNeeded(CommandList, RS.Get());
			Owner->SetComputePipelineStateIfNeeded(CommandList, PSO.Get());
		}
		else
		{
			CommandList->SetComputeRootSignature(RS.Get());
			CommandList->SetPipelineState(PSO.Get());
		}
	}
	else
	{
		if (Owner)
		{
			Owner->InvalidateComputeCommandStateCache();
			Owner->InvalidateRayTracingCommandStateCache();
		}
		CommandList->SetGraphicsRootSignature(RS.Get());
		CommandList->SetPipelineState(PSO.Get());
	}

	if (!Owner)
		return;

	for (const auto& bindingPair : textureBinding)
	{
		const BindingData& bindingData = bindingPair.second;
		if (!bindingData.Schema.Bindless)
			continue;

		D3D12_GPU_DESCRIPTOR_HANDLE tableHandle{};
		if (bindingData.Schema.ResourceKind == RHIResourceKind::Texture)
		{
			if (!Owner->IsBindlessTextureTableReady())
				continue;
			tableHandle = Owner->GetBindlessTextureTableGpuHandle();
		}
		else if (bindingData.Schema.ResourceKind == RHIResourceKind::Buffer)
		{
			if (!Owner->IsBindlessBufferTableReady())
				continue;
			tableHandle = Owner->GetBindlessBufferTableGpuHandle();
		}
		else
		{
			continue;
		}

		if (IsCompute)
		{
			if (Owner)
				Owner->SetComputeRootDescriptorTableIfNeeded(CommandList, bindingData.rootParamIndex, tableHandle);
			else
				CommandList->SetComputeRootDescriptorTable(bindingData.rootParamIndex, tableHandle);
		}
		else
			CommandList->SetGraphicsRootDescriptorTable(bindingData.rootParamIndex, tableHandle);
	}
}

void D3D12ComputePipelineStateObject::BindSRV(const std::string& name, uint32_t baseRegister, uint32_t numDescriptors)
{
	if (!PSO)
		PSO = std::make_shared<PipelineStateObject>();
	PSO->BindSRV(name, static_cast<int>(baseRegister), static_cast<int>(numDescriptors));
}

void D3D12ComputePipelineStateObject::BindSRV(const RHIBindingDesc& binding)
{
	BindSRV(binding.Name, binding.RegisterIndex, RHILegacyDescriptorCount(binding));
	if (PSO)
	{
		auto it = PSO->textureBinding.find(binding.Name);
		if (it != PSO->textureBinding.end())
			it->second.Schema = binding;
	}
}

void D3D12ComputePipelineStateObject::BindUAV(const std::string& name, uint32_t baseRegister)
{
	if (!PSO)
		PSO = std::make_shared<PipelineStateObject>();
	PSO->BindUAV(name, static_cast<int>(baseRegister));
}

void D3D12ComputePipelineStateObject::BindUAV(const RHIBindingDesc& binding)
{
	BindUAV(binding.Name, binding.RegisterIndex);
	if (PSO)
	{
		auto it = PSO->uavBinding.find(binding.Name);
		if (it != PSO->uavBinding.end())
			it->second.Schema = binding;
	}
}

void D3D12ComputePipelineStateObject::BindCBV(const std::string& name, uint32_t baseRegister, uint32_t size)
{
	if (!PSO)
		PSO = std::make_shared<PipelineStateObject>();
	PSO->BindCBV(name, static_cast<int>(baseRegister), static_cast<int>(size));
}

void D3D12ComputePipelineStateObject::BindCBV(const RHIBindingDesc& binding)
{
	BindCBV(binding.Name, binding.RegisterIndex, binding.SizeInBytes);
	if (PSO)
	{
		auto it = PSO->constantBufferBinding.find(binding.Name);
		if (it != PSO->constantBufferBinding.end())
			it->second.Schema = binding;
	}
}

void D3D12ComputePipelineStateObject::BindSampler(const std::string& name, uint32_t baseRegister)
{
	if (!PSO)
		PSO = std::make_shared<PipelineStateObject>();
	PSO->BindSampler(name, static_cast<int>(baseRegister));
}

void D3D12ComputePipelineStateObject::BindSampler(const RHIBindingDesc& binding)
{
	BindSampler(binding.Name, binding.RegisterIndex);
	if (PSO)
	{
		auto it = PSO->samplerBinding.find(binding.Name);
		if (it != PSO->samplerBinding.end())
			it->second.Schema = binding;
	}
}

bool D3D12ComputePipelineStateObject::InitCS(const std::wstring& shaderFile, const std::string& entryPoint)
{
	if (!Owner)
		return false;
	if (!PSO)
		PSO = std::make_shared<PipelineStateObject>();

	PSO->Owner = Owner;
	PSO->IsCompute = true;
	PSO->DebugName = MakePipelineDebugName(L"ComputePSO", shaderFile, entryPoint);
	PSO->computePSODesc = {};
	{
		ComPtr<ID3DBlob> csBlob = compileShaderDXC(Owner, shaderFile.c_str(), entryPoint, L"cs_6_0");
		if (!csBlob)
			return false;
		const uint8_t* src = static_cast<const uint8_t*>(csBlob->GetBufferPointer());
		PSO->cs.Data.assign(src, src + csBlob->GetBufferSize());
	}

	const bool bInit = PSO->Init();
	return bInit;
}

bool D3D12ComputePipelineStateObject::InitCSWithInlineRT(const std::wstring& shaderFile, const std::string& entryPoint)
{
	// Same as InitCS but compiles with cs_6_5 so the shader can use
	// RayQuery for inline ray tracing. Requires the device to support
	// D3D12_RAYTRACING_TIER_1_1; the caller is responsible for not
	// invoking this on hardware without it.
	if (!Owner)
		return false;
	if (!PSO)
		PSO = std::make_shared<PipelineStateObject>();

	PSO->Owner = Owner;
	PSO->IsCompute = true;
	PSO->DebugName = MakePipelineDebugName(L"ComputePSO_InlineRT", shaderFile, entryPoint);
	PSO->computePSODesc = {};
	{
		ComPtr<ID3DBlob> csBlob = compileShaderDXC(Owner, shaderFile.c_str(), entryPoint, L"cs_6_5");
		if (!csBlob)
			return false;
		const uint8_t* src = static_cast<const uint8_t*>(csBlob->GetBufferPointer());
		PSO->cs.Data.assign(src, src + csBlob->GetBufferSize());
	}

	const bool bInit = PSO->Init();
	return bInit;
}

void D3D12ComputePipelineStateObject::Apply()
{
	if (!PSO)
		return;

	PSO->Apply();

	for (const auto& binding : PendingSRVs)
		PSO->SetSRV(binding.first, binding.second);
	for (const auto& binding : PendingSamplers)
		PSO->SetSampler(binding.first, binding.second);
	for (const auto& binding : PendingCBVs)
		PSO->SetCBVValue(binding.first, const_cast<uint8_t*>(binding.second.data()));
	for (const auto& binding : PendingUAVs)
		PSO->SetUAV(binding.first, binding.second);
}

void D3D12ComputePipelineStateObject::SetTextureSRV(const std::string& name, Texture* texture)
{
	if (PSO && texture)
		PendingSRVs[name] = texture->GpuHandleSRV;
}

void D3D12ComputePipelineStateObject::SetAccelerationStructure(const std::string& name, const std::shared_ptr<RTAS>& rtas)
{
	// TLAS is bound as a regular SRV t-register from the compute shader's
	// perspective (HLSL declares it as `RaytracingAccelerationStructure`).
	// The GPU descriptor handle already points at the AS view created
	// during TLAS build; just queue it the same way other SRVs are.
	D3D12RTAS* dx12RTAS = dynamic_cast<D3D12RTAS*>(rtas.get());
	if (PSO && dx12RTAS)
		PendingSRVs[name] = dx12RTAS->GPUHandle;
}

void D3D12ComputePipelineStateObject::SetTextureUAV(const std::string& name, Texture* texture)
{
	if (PSO && texture)
		PendingUAVs[name] = texture->GpuHandleUAV;
}

void D3D12ComputePipelineStateObject::SetBufferSRV(const std::string& name, Buffer* buffer)
{
	if (PSO && buffer)
		PendingSRVs[name] = buffer->GpuHandleSRV;
}

void D3D12ComputePipelineStateObject::SetBufferUAV(const std::string& name, Buffer* buffer)
{
	if (PSO && buffer)
		PendingUAVs[name] = buffer->GpuHandleUAV;
}

void D3D12ComputePipelineStateObject::SetVertexBufferUAV(const std::string& name, VertexBuffer* vertexBuffer)
{
	if (PSO && vertexBuffer)
		PendingUAVs[name] = vertexBuffer->GpuHandleUAV;
}

void D3D12ComputePipelineStateObject::SetSampler(const std::string& name, Sampler* sampler)
{
	if (PSO && sampler)
		PendingSamplers[name] = sampler;
}

void D3D12ComputePipelineStateObject::SetCBVValue(const std::string& name, void* pData)
{
	if (!PSO || !pData)
		return;

	auto bindingIt = PSO->constantBufferBinding.find(name);
	if (bindingIt == PSO->constantBufferBinding.end())
		return;

	std::vector<uint8_t>& data = PendingCBVs[name];
	data.resize(bindingIt->second.cbSize);
	CopyConstantBufferData(data.data(), static_cast<UINT>(data.size()), pData, bindingIt->second.sourceSize);
}

Buffer::~Buffer()
{
	if (Owner && BindlessHandle.IsValid())
		Owner->UnregisterBindlessBuffer(this);
}

VertexBuffer::~VertexBuffer()
{
	if (Owner && BindlessHandle.IsValid())
		Owner->UnregisterBindlessVertexBuffer(this);
}

IndexBuffer::~IndexBuffer()
{
	if (Owner && BindlessHandle.IsValid())
		Owner->UnregisterBindlessIndexBuffer(this);
}

Texture::~Texture()
{
	if (Owner && BindlessHandle.IsValid())
		Owner->UnregisterBindlessTexture(this);
}

namespace
{
	uint32_t ToD3D12StreamlineState(EResourceState state)
	{
		switch (state)
		{
		case EResourceState::ShaderRead:
			return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		case EResourceState::UnorderedAccess:
			return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		case EResourceState::RenderTarget:
			return D3D12_RESOURCE_STATE_RENDER_TARGET;
		case EResourceState::DepthWrite:
			return D3D12_RESOURCE_STATE_DEPTH_WRITE;
		case EResourceState::CopyDest:
			return D3D12_RESOURCE_STATE_COPY_DEST;
		case EResourceState::CopySource:
			return D3D12_RESOURCE_STATE_COPY_SOURCE;
		case EResourceState::Present:
			return D3D12_RESOURCE_STATE_PRESENT;
		default:
			return D3D12_RESOURCE_STATE_COMMON;
		}
	}
}

bool DX12Backend::GetStreamlineTextureResource(Texture* texture, EResourceState state, StreamlineTextureResourceDesc& outDesc) const
{
	if (!texture || !texture->resource)
		return false;

	const D3D12_RESOURCE_DESC desc = texture->resource->GetDesc();
	outDesc = {};
	outDesc.Native = texture->resource.Get();
	outDesc.State = ToD3D12StreamlineState(state);
	outDesc.Width = static_cast<uint32_t>(desc.Width);
	outDesc.Height = desc.Height;
	outDesc.NativeFormat = static_cast<uint32_t>(desc.Format);
	outDesc.MipLevels = desc.MipLevels;
	outDesc.ArrayLayers = desc.DepthOrArraySize;
	outDesc.Flags = static_cast<uint32_t>(desc.Flags);
	return outDesc.Native != nullptr;
}

void* DX12Backend::GetStreamlineCommandBuffer()
{
	return GetGraphicsCommandList();
}

void DX12Backend::NotifyExternalCommandListStateChanged()
{
	InvalidateGraphicsCommandStateCache();
	InvalidateComputeCommandStateCache();
	InvalidateRayTracingCommandStateCache();
}

void Texture::MakeStaticSRV()
{
	DX12Backend* owner = Owner;
	if (!owner)
		return;
	owner->TextureDHRing->AllocDescriptor(CpuHandleSRV, GpuHandleSRV);

	D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc = MakeTextureSRVDesc(*this);
	owner->Device->CreateShaderResourceView(resource.Get(), &SrvDesc, CpuHandleSRV);
	owner->RegisterBindlessTexture(this);
}

void Texture::MakeRTV(bool isBackBuffer)
{
	DX12Backend* owner = Owner;
	if (!owner)
		return;
	owner->RTVDescriptorHeap->AllocDescriptor(CpuHandleRTV, GpuHandleRTV);
	
	D3D12_RENDER_TARGET_VIEW_DESC desc = {};
	//desc.Format = Format;
	desc.Format = isBackBuffer ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : textureDesc.Format;

	desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

	owner->Device->CreateRenderTargetView(resource.Get(), nullptr, CpuHandleRTV);
}

void Texture::MakeDSV()
{
	DX12Backend* owner = Owner;
	if (!owner)
		return;
	owner->DSVDescriptorHeap->AllocDescriptor(CpuHandleDSV, GpuHandleDSV);

	D3D12_DEPTH_STENCIL_VIEW_DESC depthStencilDesc = {};
	depthStencilDesc.Format = DXGI_FORMAT_D32_FLOAT;
	depthStencilDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	depthStencilDesc.Flags = D3D12_DSV_FLAG_NONE;
	owner->Device->CreateDepthStencilView(resource.Get(), &depthStencilDesc, CpuHandleDSV);
}

std::shared_ptr<Texture> DX12Backend::CreateTexture2DFromResource(ComPtr<ID3D12Resource> InResource)
{
	Texture* tex = new Texture;
	tex->Owner = this;

	if (InResource)
	{
		tex->resource = InResource;
	}

	return shared_ptr<Texture>(tex);
}

std::shared_ptr<Texture> DX12Backend::CreateTexture2D(const TextureCreateDesc& desc)
{
	std::shared_ptr<Texture> texture = CreateTexture2D(
		ToDXGIFormat(desc.Format),
		ToD3D12ResourceFlags(desc.Usage),
		ToD3D12ResourceState(desc.InitialState),
		desc.Width,
		desc.Height,
		desc.MipLevels,
		desc.ClearColor);
	if (texture)
	{
		texture->Width = static_cast<uint32_t>(desc.Width);
		texture->Height = static_cast<uint32_t>(desc.Height);
		texture->MipLevels = static_cast<uint32_t>(desc.MipLevels);
		texture->Format = desc.Format;
		texture->Usage = desc.Usage;
		if (HasTextureUsage(desc.Usage, TextureUsage_RenderTarget))
			texture->MakeRTV();
		if (HasTextureUsage(desc.Usage, TextureUsage_DepthStencil))
			texture->MakeDSV();
		RegisterBindlessTexture(texture.get());
	}
	return texture;
}

std::shared_ptr<Texture> DX12Backend::CreateTexture3D(ETextureFormat format, ETextureUsageFlags usage, EInitialResourceState initialState, int width, int height, int depth, int mipLevels)
{
	return CreateTexture3D(
		ToDXGIFormat(format),
		ToD3D12ResourceFlags(usage),
		ToD3D12ResourceState(initialState),
		width,
		height,
		depth,
		mipLevels);
}

void DX12Backend::UploadTexture3D(Texture* texture, const void* data, uint64_t rowPitch, uint64_t slicePitch)
{
	if (!texture || !data)
		return;

	D3D12_SUBRESOURCE_DATA textureData = {};
	textureData.pData = data;
	textureData.RowPitch = static_cast<LONG_PTR>(rowPitch);
	textureData.SlicePitch = static_cast<LONG_PTR>(slicePitch);
	texture->UploadSRCData3D(&textureData);
}

std::shared_ptr<Texture> DX12Backend::CreateTexture2D(DXGI_FORMAT format, D3D12_RESOURCE_FLAGS resFlags, D3D12_RESOURCE_STATES initResState, int width, int height, int mipLevels, std::optional<glm::vec4> clearColor)
{
	Texture* tex = new Texture;
	tex->Owner = this;

	D3D12_RESOURCE_DESC textureDesc = {};
	textureDesc.MipLevels = mipLevels;
	textureDesc.Format = format;
	textureDesc.Width = width;
	textureDesc.Height = height;
	textureDesc.DepthOrArraySize = 1;
	textureDesc.SampleDesc.Count = 1;
	textureDesc.SampleDesc.Quality = 0;
	textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;

	textureDesc.Flags = resFlags;

	tex->textureDesc = textureDesc;

	D3D12_HEAP_PROPERTIES heapProp;
	heapProp.Type = D3D12_HEAP_TYPE_DEFAULT;
	heapProp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heapProp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	heapProp.CreationNodeMask = 1;
	heapProp.VisibleNodeMask = 1;

	D3D12_RESOURCE_STATES ResStats = initResState;
	if (resFlags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
	{
		D3D12_CLEAR_VALUE optimizedClearValue = {};
		optimizedClearValue.Format = DXGI_FORMAT_D32_FLOAT;
		optimizedClearValue.DepthStencil = { 1.0f, 0 };

		ThrowIfFailed(Device->CreateCommittedResource(
			&heapProp,
			D3D12_HEAP_FLAG_NONE,
			&textureDesc,
			ResStats,
			&optimizedClearValue,
			IID_PPV_ARGS(&tex->resource)));
	}
	else
	{
		D3D12_CLEAR_VALUE optimizedClearValue = {};
		optimizedClearValue.Format = format;

		if (clearColor.has_value())
		{
			optimizedClearValue.Color[0] = clearColor.value().x;
			optimizedClearValue.Color[1] = clearColor.value().y;
			optimizedClearValue.Color[2] = clearColor.value().z;
			optimizedClearValue.Color[3] = clearColor.value().w;
		}
		else
		{
			optimizedClearValue.Color[0] = 0.0f;
			optimizedClearValue.Color[1] = 0.2f;
			optimizedClearValue.Color[2] = 0.4f;
			optimizedClearValue.Color[3] = 1.0f;
		}
		//D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET

		D3D12_CLEAR_VALUE* pClearValue = nullptr;
		if (resFlags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
			pClearValue = &optimizedClearValue;
		ThrowIfFailed(Device->CreateCommittedResource(
			&heapProp,
			D3D12_HEAP_FLAG_NONE,
			&textureDesc,
			ResStats,
			pClearValue,
			IID_PPV_ARGS(&tex->resource)));
	}
	shared_ptr<Texture> texPtr = shared_ptr<Texture>(tex);
	if (resFlags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET || resFlags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL || resFlags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
		DynamicTextures.push_back(texPtr);

	return texPtr;
}

std::shared_ptr<Texture> DX12Backend::CreateTexture3D(DXGI_FORMAT format, D3D12_RESOURCE_FLAGS resFlags, D3D12_RESOURCE_STATES initResState, int width, int height, int depth, int mipLevels)
{
	Texture* tex = new Texture;
	tex->Owner = this;

	D3D12_RESOURCE_DESC textureDesc = {};
	textureDesc.MipLevels = mipLevels;
	textureDesc.Format = format;
	textureDesc.Width = width;
	textureDesc.Height = height;
	textureDesc.DepthOrArraySize = depth;
	textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
	textureDesc.SampleDesc.Count = 1;
	textureDesc.SampleDesc.Quality = 0;
	textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
	textureDesc.Flags = resFlags;

	tex->textureDesc = textureDesc;

	D3D12_HEAP_PROPERTIES heapProp;
	heapProp.Type = D3D12_HEAP_TYPE_DEFAULT;
	heapProp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heapProp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	heapProp.CreationNodeMask = 1;
	heapProp.VisibleNodeMask = 1;

	D3D12_RESOURCE_STATES ResStats = initResState;

	D3D12_CLEAR_VALUE* pClearValue = nullptr;

	ThrowIfFailed(Device->CreateCommittedResource(
		&heapProp,
		D3D12_HEAP_FLAG_NONE,
		&textureDesc,
		ResStats,
		pClearValue,
		IID_PPV_ARGS(&tex->resource)));

	shared_ptr<Texture> texPtr = shared_ptr<Texture>(tex);

	if (resFlags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET || resFlags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL || resFlags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
		DynamicTextures.push_back(texPtr);
	else
		tex->MakeStaticSRV();

	return texPtr;
}

void Texture::UploadSRCData3D(D3D12_SUBRESOURCE_DATA* SrcData)
{
	DX12Backend* owner = Owner;
	if (!owner)
		return;
	CommandList* cmd = owner->CmdQ->AllocCmdList();

	// upload src data
	if (SrcData)
	{
		ComPtr<ID3D12Resource> textureUploadHeap;

		D3D12_PLACED_SUBRESOURCE_FOOTPRINT descFootPrint;
		UINT Rows = 0;
		UINT64 RowSize = 0;
		UINT64 TotalBytes = 0;
		owner->Device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &descFootPrint, &Rows, &RowSize, &TotalBytes);


		D3D12_HEAP_PROPERTIES heapProp;
		heapProp.Type = D3D12_HEAP_TYPE_UPLOAD;
		heapProp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		heapProp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
		heapProp.CreationNodeMask = 1;
		heapProp.VisibleNodeMask = 1;

		D3D12_RESOURCE_DESC resDesc;

		resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		resDesc.Alignment = 0;
		resDesc.Width = TotalBytes;
		resDesc.Height = 1;
		resDesc.DepthOrArraySize = 1;
		resDesc.MipLevels = textureDesc.MipLevels;
		resDesc.Format = DXGI_FORMAT_UNKNOWN;
		resDesc.SampleDesc.Count = 1;
		resDesc.SampleDesc.Quality = 0;
		resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		resDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

		/*stringstream ss;
		ss << "UploadSRCData : " << TotalBytes << "\n";
		OutputDebugStringA(ss.str().c_str());*/


		ThrowIfFailed(owner->Device->CreateCommittedResource(
			&heapProp,
			D3D12_HEAP_FLAG_NONE,
			&resDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&textureUploadHeap)));

		NAME_D3D12_OBJECT(textureUploadHeap);

		UINT8* pData = nullptr;
		textureUploadHeap->Map(0, nullptr, reinterpret_cast<void**>(&pData));
		memcpy(reinterpret_cast<void*>(pData), SrcData->pData, TotalBytes);
		textureUploadHeap->Unmap(0, nullptr);

		D3D12_TEXTURE_COPY_LOCATION dst = { };
		dst.pResource = resource.Get();
		dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dst.SubresourceIndex = UINT32(0);
		D3D12_TEXTURE_COPY_LOCATION src = { };
		src.pResource = textureUploadHeap.Get();
		src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		src.PlacedFootprint = descFootPrint;

		D3D12_BOX sourceRegion;
		sourceRegion.left = 0;
		sourceRegion.top = 64;
		sourceRegion.right = 64;
		sourceRegion.bottom = 0;
		sourceRegion.front = 64;
		sourceRegion.back = 0;
		cmd->CmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);


		D3D12_RESOURCE_BARRIER BarrierDesc = {};
		BarrierDesc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		BarrierDesc.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		BarrierDesc.Transition.pResource = resource.Get();
		BarrierDesc.Transition.Subresource = 0;
		BarrierDesc.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		BarrierDesc.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		cmd->CmdList->ResourceBarrier(1, &BarrierDesc);

		owner->CmdQ->ExecuteCommandList(cmd);
		owner->CmdQ->WaitGPU();
	}
}

std::shared_ptr<Texture> DX12Backend::CreateTextureFromFile(const std::wstring& fileName, bool nonSRGB)
{
	const auto totalStart = std::chrono::steady_clock::now();
	std::error_code fileSizeError;
	const uint64_t fileSize = std::filesystem::exists(fileName, fileSizeError) ?
		static_cast<uint64_t>(std::filesystem::file_size(fileName, fileSizeError)) : 0ull;

	if (FileExists(fileName.c_str()) == false)
	{
		AppendCpuRuntimeTrace(L"[StartupProfile][TextureLoad] missing file=\"" + fileName + L"\"");
		return nullptr;
	}

	DirectX::ScratchImage image;

	const std::wstring extension = GetFileExtension(fileName.c_str());
	const bool bIsDds = extension == L"DDS" || extension == L"dds";
	const bool bIsTga = extension == L"TGA" || extension == L"tga";
	HRESULT loadHr = S_OK;
	HRESULT mipHr = S_OK;
	double imageLoadMs = 0.0;
	double mipGenMs = 0.0;

	if (bIsDds)
	{
		const auto imageLoadStart = std::chrono::steady_clock::now();
		loadHr = DirectX::LoadFromDDSFile(fileName.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image);
		imageLoadMs = ElapsedDx12InitMilliseconds(imageLoadStart, std::chrono::steady_clock::now());
	}
	else if (bIsTga)
	{
		DirectX::ScratchImage tempImage;
		const auto imageLoadStart = std::chrono::steady_clock::now();
		loadHr = DirectX::LoadFromTGAFile(fileName.c_str(), nullptr, tempImage);
		imageLoadMs = ElapsedDx12InitMilliseconds(imageLoadStart, std::chrono::steady_clock::now());
		if (SUCCEEDED(loadHr) && tempImage.GetImage(0, 0, 0))
		{
			const auto mipStart = std::chrono::steady_clock::now();
			mipHr = DirectX::GenerateMipMaps(*tempImage.GetImage(0, 0, 0), DirectX::TEX_FILTER_DEFAULT, 0, image, false);
			mipGenMs = ElapsedDx12InitMilliseconds(mipStart, std::chrono::steady_clock::now());
		}
	}
	else
	{
		DirectX::ScratchImage tempImage;
		const auto imageLoadStart = std::chrono::steady_clock::now();
		loadHr = DirectX::LoadFromWICFile(fileName.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, tempImage);
		imageLoadMs = ElapsedDx12InitMilliseconds(imageLoadStart, std::chrono::steady_clock::now());
		if (SUCCEEDED(loadHr) && tempImage.GetImage(0, 0, 0))
		{
			const auto mipStart = std::chrono::steady_clock::now();
			mipHr = DirectX::GenerateMipMaps(*tempImage.GetImage(0, 0, 0), DirectX::TEX_FILTER_DEFAULT, 0, image, false);
			mipGenMs = ElapsedDx12InitMilliseconds(mipStart, std::chrono::steady_clock::now());
		}
	}

	if (FAILED(loadHr) || FAILED(mipHr) || image.GetImageCount() == 0)
	{
		AppendCpuRuntimeTrace(
			L"[StartupProfile][TextureLoad] failed file=\"" + fileName +
			L"\", ext=\"" + extension +
			L"\", bytes=" + std::to_wstring(fileSize) +
			L", loadHr=" + FormatHexHRESULT(loadHr) +
			L", mipHr=" + FormatHexHRESULT(mipHr) +
			L", imageLoadMs=" + FormatDx12InitMilliseconds(imageLoadMs) +
			L", mipGenMs=" + FormatDx12InitMilliseconds(mipGenMs) +
			L", totalMs=" + FormatDx12InitMilliseconds(ElapsedDx12InitMilliseconds(totalStart, std::chrono::steady_clock::now())));
		return nullptr;
	}

	const DirectX::TexMetadata& metaData = image.GetMetadata();
	DXGI_FORMAT format = metaData.format;

	if(!nonSRGB)
		format = DirectX::MakeSRGB(format);

	const bool is3D = metaData.dimension == DirectX::TEX_DIMENSION_TEXTURE3D;

	D3D12_RESOURCE_DESC textureDesc = { };
	textureDesc.MipLevels = UINT16(metaData.mipLevels);
	textureDesc.Format = format;
	textureDesc.Width = UINT64(metaData.width);
	textureDesc.Height = UINT64(metaData.height);
	textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
	textureDesc.DepthOrArraySize = is3D ? UINT16(metaData.depth) : UINT16(metaData.arraySize);
	textureDesc.SampleDesc.Count = 1;
	textureDesc.SampleDesc.Quality = 0;
	textureDesc.Dimension = is3D ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	textureDesc.Alignment = 0;

	std::unique_ptr<Texture> tex(new Texture);
	tex->Owner = this;
	double defaultResourceCreateMs = 0.0;
	double uploadHeapCreateMs = 0.0;
	double footprintMs = 0.0;
	double cpuCopyMs = 0.0;
	double commandRecordMs = 0.0;
	double executeMs = 0.0;
	double waitGpuMs = 0.0;
	double srvMs = 0.0;

	D3D12_HEAP_PROPERTIES heapProp;
	heapProp.Type = D3D12_HEAP_TYPE_DEFAULT;
	heapProp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heapProp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	heapProp.CreationNodeMask = 1;
	heapProp.VisibleNodeMask = 1;

	tex->textureDesc = textureDesc;

	const auto defaultResourceCreateStart = std::chrono::steady_clock::now();
	HRESULT resourceHr = Device->CreateCommittedResource(&heapProp, D3D12_HEAP_FLAG_NONE, &textureDesc,
		D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex->resource));
	defaultResourceCreateMs = ElapsedDx12InitMilliseconds(defaultResourceCreateStart, std::chrono::steady_clock::now());
	if (FAILED(resourceHr) || !tex->resource)
	{
		AppendCpuRuntimeTrace(
			L"[StartupProfile][TextureLoad] failed resource file=\"" + fileName +
			L"\", hr=" + FormatHexHRESULT(resourceHr) +
			L", totalMs=" + FormatDx12InitMilliseconds(ElapsedDx12InitMilliseconds(totalStart, std::chrono::steady_clock::now())));
		return nullptr;
	}
	tex->resource->SetName(fileName.c_str());
	D3D12_HEAP_PROPERTIES textureHeapProps = {};
	D3D12_HEAP_FLAGS textureHeapFlags = D3D12_HEAP_FLAG_NONE;
	if (SUCCEEDED(tex->resource->GetHeapProperties(&textureHeapProps, &textureHeapFlags)) &&
		textureHeapProps.Type != D3D12_HEAP_TYPE_DEFAULT)
	{
		AppendCpuRuntimeTrace(
			L"[TextureLoad] unexpected sampled texture heap file=\"" + fileName +
			L"\", heapType=" + std::to_wstring(static_cast<int>(textureHeapProps.Type)));
	}

	D3D12_HEAP_PROPERTIES heapPropUpload;
	heapPropUpload.Type = D3D12_HEAP_TYPE_UPLOAD;
	heapPropUpload.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heapPropUpload.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	heapPropUpload.CreationNodeMask = 1;
	heapPropUpload.VisibleNodeMask = 1;

	ComPtr<ID3D12Resource> uploadHeap;
	
	const UINT subresourceCount = textureDesc.DepthOrArraySize * textureDesc.MipLevels;
	const UINT64 uploadBufferSize = GetRequiredIntermediateSize(tex->resource.Get(), 0, subresourceCount);
	D3D12_RESOURCE_DESC resDesc;
	resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	resDesc.Alignment = 0;
	resDesc.Width = uploadBufferSize;
	resDesc.Height = 1;
	resDesc.DepthOrArraySize = 1;
	resDesc.MipLevels = 1;// textureDesc.MipLevels;
	resDesc.Format = DXGI_FORMAT_UNKNOWN;
	resDesc.SampleDesc.Count = 1;
	resDesc.SampleDesc.Quality = 0;
	resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	resDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

	/*stringstream ss;
	ss << "CreateTextureFromFile : " << uploadBufferSize << "\n";
	OutputDebugStringA(ss.str().c_str());*/

	const auto uploadHeapCreateStart = std::chrono::steady_clock::now();
	HRESULT uploadHr = Device->CreateCommittedResource(&heapPropUpload, D3D12_HEAP_FLAG_NONE, &resDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadHeap));
	uploadHeapCreateMs = ElapsedDx12InitMilliseconds(uploadHeapCreateStart, std::chrono::steady_clock::now());
	if (FAILED(uploadHr) || !uploadHeap)
	{
		AppendCpuRuntimeTrace(
			L"[StartupProfile][TextureLoad] failed uploadHeap file=\"" + fileName +
			L"\", hr=" + FormatHexHRESULT(uploadHr) +
			L", totalMs=" + FormatDx12InitMilliseconds(ElapsedDx12InitMilliseconds(totalStart, std::chrono::steady_clock::now())));
		return nullptr;
	}
	const std::wstring uploadHeapName = L"TextureUploadStaging:" + fileName;
	uploadHeap->SetName(uploadHeapName.c_str());

	const UINT64 numSubResources = metaData.mipLevels * metaData.arraySize;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT* layouts = (D3D12_PLACED_SUBRESOURCE_FOOTPRINT*)_alloca(sizeof(D3D12_PLACED_SUBRESOURCE_FOOTPRINT) * numSubResources);
	UINT32* numRows = (UINT32*)_alloca(sizeof(UINT32) * numSubResources);
	UINT64* rowSizes = (UINT64*)_alloca(sizeof(UINT64) * numSubResources);

	const auto footprintStart = std::chrono::steady_clock::now();
	UINT64 textureMemSize = 0;
	Device->GetCopyableFootprints(&textureDesc, 0, UINT32(numSubResources), 0, layouts, numRows, rowSizes, &textureMemSize);
	footprintMs = ElapsedDx12InitMilliseconds(footprintStart, std::chrono::steady_clock::now());

	UINT8* uploadMem = nullptr;

	const auto cpuCopyStart = std::chrono::steady_clock::now();
	D3D12_RANGE readRange = { };
	uploadHeap->Map(0, &readRange, reinterpret_cast<void**>(&uploadMem));
	for (UINT64 arrayIdx = 0; arrayIdx < metaData.arraySize; ++arrayIdx)
	{

		for (UINT64 mipIdx = 0; mipIdx < metaData.mipLevels; ++mipIdx)
		{
			const UINT64 subResourceIdx = mipIdx + (arrayIdx * metaData.mipLevels);

			const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& subResourceLayout = layouts[subResourceIdx];
			const UINT64 subResourceHeight = numRows[subResourceIdx];
			const UINT64 subResourcePitch = subResourceLayout.Footprint.RowPitch;
			const UINT64 subResourceDepth = subResourceLayout.Footprint.Depth;
			UINT8* dstSubResourceMem = reinterpret_cast<UINT8*>(uploadMem) + subResourceLayout.Offset;

			for (UINT64 z = 0; z < subResourceDepth; ++z)
			{
				const DirectX::Image* subImage = image.GetImage(mipIdx, arrayIdx, z);
				const UINT8* srcSubResourceMem = subImage->pixels;

				for (UINT64 y = 0; y < subResourceHeight; ++y)
				{
					const size_t bytesToCopy = static_cast<size_t>(std::min<UINT64>(subResourcePitch, static_cast<UINT64>(subImage->rowPitch)));
					memcpy(dstSubResourceMem, srcSubResourceMem, bytesToCopy);
					dstSubResourceMem += subResourcePitch;
					srcSubResourceMem += subImage->rowPitch;
				}
			}
		}
	}
	uploadHeap->Unmap(0, nullptr);
	cpuCopyMs = ElapsedDx12InitMilliseconds(cpuCopyStart, std::chrono::steady_clock::now());

	CommandList* cmd = CmdQ->AllocCmdList();
	const auto commandRecordStart = std::chrono::steady_clock::now();
	for (UINT64 subResourceIdx = 0; subResourceIdx < numSubResources; ++subResourceIdx)
	{
		D3D12_TEXTURE_COPY_LOCATION dst = { };
		dst.pResource = tex->resource.Get();
		dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dst.SubresourceIndex = UINT32(subResourceIdx);
		D3D12_TEXTURE_COPY_LOCATION src = { };
		src.pResource = uploadHeap.Get();
		src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		src.PlacedFootprint = layouts[subResourceIdx];
		//src.PlacedFootprint.Offset += 0;// uploadContext.ResourceOffset;
		//src.SubresourceIndex = UINT32(subResourceIdx);

		cmd->CmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	}

	D3D12_RESOURCE_BARRIER BarrierDesc = {};
	BarrierDesc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	BarrierDesc.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	BarrierDesc.Transition.pResource = tex->resource.Get();
	BarrierDesc.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	BarrierDesc.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	BarrierDesc.Transition.StateAfter =
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	cmd->CmdList->ResourceBarrier(1, &BarrierDesc);
	commandRecordMs = ElapsedDx12InitMilliseconds(commandRecordStart, std::chrono::steady_clock::now());

	// Free any staging heaps whose GPU copy already completed before queuing more.
	RetireCompletedTextureUploads();

	const auto executeStart = std::chrono::steady_clock::now();
	CmdQ->ExecuteCommandList(cmd);
	executeMs = ElapsedDx12InitMilliseconds(executeStart, std::chrono::steady_clock::now());

	// Stage 1 streaming: do NOT stall on the copy. Park the staging heap with the
	// submission fence; it is freed once the GPU finishes (the copy is ordered
	// before any later render that samples this texture, so no read-before-write).
	const auto waitStart = std::chrono::steady_clock::now();
	PendingTextureUpload pending;
	pending.FenceValue = cmd->Fence.value_or(CmdQ->CurrentFenceValue);
	pending.Bytes = uploadBufferSize;
	pending.UploadHeap = uploadHeap;
	PendingTextureUploads.push_back(std::move(pending));
	PendingTextureUploadBytes += uploadBufferSize;
	// Bound in-flight staging memory: drain once over budget (rare; large maps).
	if (PendingTextureUploadBytes > kMaxInFlightTextureUploadBytes)
	{
		CmdQ->WaitGPU();
		RetireCompletedTextureUploads();
	}
	waitGpuMs = ElapsedDx12InitMilliseconds(waitStart, std::chrono::steady_clock::now());

	const auto srvStart = std::chrono::steady_clock::now();
	tex->MakeStaticSRV();
	srvMs = ElapsedDx12InitMilliseconds(srvStart, std::chrono::steady_clock::now());

	AppendCpuRuntimeTrace(
		L"[StartupProfile][TextureLoad] file=\"" + fileName +
		L"\", ext=\"" + extension +
		L"\", bytes=" + std::to_wstring(fileSize) +
		L", width=" + std::to_wstring(static_cast<uint64_t>(metaData.width)) +
		L", height=" + std::to_wstring(static_cast<uint64_t>(metaData.height)) +
		L", mips=" + std::to_wstring(static_cast<uint64_t>(metaData.mipLevels)) +
		L", arraySize=" + std::to_wstring(static_cast<uint64_t>(metaData.arraySize)) +
		L", uploadBytes=" + std::to_wstring(uploadBufferSize) +
		L", textureMemBytes=" + std::to_wstring(textureMemSize) +
		L", format=" + std::to_wstring(static_cast<int>(format)) +
		L", sourceFormat=" + std::to_wstring(static_cast<int>(metaData.format)) +
		L", nonSRGB=" + std::to_wstring(nonSRGB ? 1 : 0) +
		L", imageLoadMs=" + FormatDx12InitMilliseconds(imageLoadMs) +
		L", mipGenMs=" + FormatDx12InitMilliseconds(mipGenMs) +
		L", defaultResourceCreateMs=" + FormatDx12InitMilliseconds(defaultResourceCreateMs) +
		L", uploadHeapCreateMs=" + FormatDx12InitMilliseconds(uploadHeapCreateMs) +
		L", footprintMs=" + FormatDx12InitMilliseconds(footprintMs) +
		L", cpuCopyMs=" + FormatDx12InitMilliseconds(cpuCopyMs) +
		L", commandRecordMs=" + FormatDx12InitMilliseconds(commandRecordMs) +
		L", executeMs=" + FormatDx12InitMilliseconds(executeMs) +
		L", waitGpuMs=" + FormatDx12InitMilliseconds(waitGpuMs) +
		L", srvMs=" + FormatDx12InitMilliseconds(srvMs) +
		L", totalMs=" + FormatDx12InitMilliseconds(ElapsedDx12InitMilliseconds(totalStart, std::chrono::steady_clock::now())));

	return shared_ptr<Texture>(tex.release());
}

void DX12Backend::RetireCompletedTextureUploads()
{
	if (PendingTextureUploads.empty() || !CmdQ || !CmdQ->m_fence)
		return;
	const UINT64 completed = CmdQ->m_fence->GetCompletedValue();
	size_t kept = 0;
	for (size_t i = 0; i < PendingTextureUploads.size(); ++i)
	{
		PendingTextureUpload& pending = PendingTextureUploads[i];
		if (pending.FenceValue <= completed)
		{
			PendingTextureUploadBytes -= std::min(PendingTextureUploadBytes, pending.Bytes);
			continue; // GPU done — drop the staging heap (ComPtr releases)
		}
		if (kept != i)
			PendingTextureUploads[kept] = std::move(pending);
		++kept;
	}
	PendingTextureUploads.resize(kept);
}

static const D3D12_HEAP_PROPERTIES kDefaultHeapProps =
{
	D3D12_HEAP_TYPE_DEFAULT,
	D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
	D3D12_MEMORY_POOL_UNKNOWN,
	0,
	0
};

static const D3D12_HEAP_PROPERTIES kUploadHeapProps =
{
	D3D12_HEAP_TYPE_UPLOAD,
	D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
	D3D12_MEMORY_POOL_UNKNOWN,
	0,
	0,
};

std::shared_ptr<RTAS> DX12Backend::CreateBLASForSkeletalMesh(Mesh* mesh)
{
	assert(mesh);
	const bool bHasOutputVb = mesh && mesh->SkeletalOutputVb;
	const bool bHasIb = mesh && mesh->Ib;
	const bool bHasOutputResource = bHasOutputVb && mesh->SkeletalOutputVb->resource.Get();
	const bool bHasIndexResource = bHasIb && mesh->Ib->resource.Get();
	if (!mesh ||
		!mesh->bSkeletalSkinned ||
		!bHasOutputResource ||
		!bHasIndexResource ||
		mesh->VertexStride == 0 ||
		mesh->SkeletalVertexCount == 0 ||
		mesh->Ib->numIndices < 3)
	{
		AppendCpuRuntimeTrace(
			L"[DX12RTAS] CreateBLASForSkeletalMesh skipped invalid mesh"
			L", mesh=" + FormatDx12Hex(reinterpret_cast<uint64_t>(mesh)) +
			L", hasOutputVb=" + std::to_wstring(bHasOutputVb ? 1 : 0) +
			L", hasIb=" + std::to_wstring(bHasIb ? 1 : 0) +
			L", hasOutputResource=" + std::to_wstring(bHasOutputResource ? 1 : 0) +
			L", hasIndexResource=" + std::to_wstring(bHasIndexResource ? 1 : 0) +
			L", vertexStride=" + std::to_wstring(mesh ? mesh->VertexStride : 0) +
			L", skeletalVertices=" + std::to_wstring(mesh ? mesh->SkeletalVertexCount : 0) +
			L", indices=" + std::to_wstring(bHasIb ? mesh->Ib->numIndices : 0));
		return nullptr;
	}

	DX12Backend* owner = this;
	D3D12RTAS* as = new D3D12RTAS;

	// Phase A: if the mesh's SkeletalOutputVb is the unified buffer, every
	// character's vertices are packed contiguously and this mesh owns the
	// slice starting at SkeletalCharIndex * SkeletalVertexCount. BLAS
	// VertexCount stays at the per-char vertex count; the StartAddress
	// shifts by the char's slot.
	const UINT64 vertexByteOffset =
		static_cast<UINT64>(mesh->SkeletalCharIndex) *
		static_cast<UINT64>(mesh->SkeletalVertexCount) *
		static_cast<UINT64>(mesh->VertexStride);

	D3D12_RAYTRACING_GEOMETRY_DESC geomDesc = {};
	geomDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
	geomDesc.Triangles.VertexBuffer.StartAddress =
		mesh->SkeletalOutputVb->resource->GetGPUVirtualAddress() + vertexByteOffset;
	geomDesc.Triangles.VertexBuffer.StrideInBytes = mesh->VertexStride;
	geomDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
	geomDesc.Triangles.VertexCount = mesh->SkeletalVertexCount;
	geomDesc.Triangles.IndexBuffer = mesh->Ib->resource->GetGPUVirtualAddress();
	geomDesc.Triangles.IndexFormat = ToDXGIFormat(mesh->IndexFormat);
	geomDesc.Triangles.IndexCount = mesh->Ib->numIndices;
	geomDesc.Triangles.Transform3x4 = 0;
	geomDesc.Flags = mesh->bTransparent
		? D3D12_RAYTRACING_GEOMETRY_FLAG_NONE
		: D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
	inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	// ALLOW_UPDATE so RefitBLAS can refresh in-place each frame. FAST_TRACE
	// is fine alongside ALLOW_UPDATE; FAST_BUILD would also work but trace
	// performance benefits the GBuffer-time RT shaders more.
	inputs.Flags =
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE |
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
	inputs.NumDescs = 1;
	inputs.pGeometryDescs = &geomDesc;
	inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
	owner->Device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
	if (info.ResultDataMaxSizeInBytes == 0 || info.ScratchDataSizeInBytes == 0)
	{
		AppendCpuRuntimeTrace(
			L"[DX12RTAS] CreateBLASForSkeletalMesh skipped empty prebuild info"
			L", resultBytes=" + std::to_wstring(info.ResultDataMaxSizeInBytes) +
			L", scratchBytes=" + std::to_wstring(info.ScratchDataSizeInBytes));
		delete as;
		return nullptr;
	}

	// scratch needs the larger of the initial-build and update-build sizes.
	const UINT64 scratchSize = (std::max)(info.ScratchDataSizeInBytes, info.UpdateScratchDataSizeInBytes);

	{
		D3D12_RESOURCE_DESC bufDesc = CD3DX12_RESOURCE_DESC::Buffer(scratchSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		owner->Device->CreateCommittedResource(&kDefaultHeapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&as->Scratch));
		if (as->Scratch)
			as->Scratch->SetName(L"Corona Skeletal BLAS Scratch");
	}
	{
		D3D12_RESOURCE_DESC bufDesc = CD3DX12_RESOURCE_DESC::Buffer(info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		// Place the traversed BLAS in the big-page geometry pool (see CreateBLASForMesh).
		const GeometryPlacement asPlacement = AllocateGeometryPlacement(info.ResultDataMaxSizeInBytes, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
		owner->Device->CreatePlacedResource(asPlacement.heap, asPlacement.offset, &bufDesc,
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr, IID_PPV_ARGS(&as->Result));
		if (as->Result)
			as->Result->SetName(L"Corona Skeletal BLAS Result");
	}
	if (!as->Scratch || !as->Result)
	{
		AppendCpuRuntimeTrace(
			L"[DX12RTAS] CreateBLASForSkeletalMesh resource allocation failed"
			L", hasScratch=" + std::to_wstring(as->Scratch ? 1 : 0) +
			L", hasResult=" + std::to_wstring(as->Result ? 1 : 0) +
			L", resultBytes=" + std::to_wstring(info.ResultDataMaxSizeInBytes) +
			L", scratchBytes=" + std::to_wstring(scratchSize));
		delete as;
		return nullptr;
	}
	as->ScratchGpuVA = as->Scratch->GetGPUVirtualAddress();
	as->ResultGpuVA = as->Result->GetGPUVirtualAddress();

	CommandList* cmd = owner->CmdQ->AllocCmdList();

	// BLAS build expects the geometry vertex buffer in
	// NON_PIXEL_SHADER_RESOURCE / a compatible read state. SkeletalOutputVb
	// lives in VertexBuffer state after Dispatch* completes; transition it
	// for the build, then back.
	D3D12_RESOURCE_BARRIER toSrv = {};
	toSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	toSrv.Transition.pResource = mesh->SkeletalOutputVb->resource.Get();
	toSrv.Transition.Subresource = 0;
	toSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	toSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	cmd->CmdList->ResourceBarrier(1, &toSrv);

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC asDesc = {};
	asDesc.Inputs = inputs;
	asDesc.DestAccelerationStructureData = as->ResultGpuVA;
	asDesc.ScratchAccelerationStructureData = as->ScratchGpuVA;
	cmd->CmdList->BuildRaytracingAccelerationStructure(&asDesc, 0, nullptr);

	D3D12_RESOURCE_BARRIER uavBarrier = {};
	uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	uavBarrier.UAV.pResource = as->Result.Get();
	cmd->CmdList->ResourceBarrier(1, &uavBarrier);

	// Leave the skinned VB in VertexBuffer + NPS read state to match what
	// DispatchSkeletalSkinningForRenderWorld assumes when it transitions
	// VertexBuffer -> UnorderedAccess at the start of the next frame.
	D3D12_RESOURCE_BARRIER toVb = {};
	toVb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	toVb.Transition.pResource = mesh->SkeletalOutputVb->resource.Get();
	toVb.Transition.Subresource = 0;
	toVb.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	toVb.Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	cmd->CmdList->ResourceBarrier(1, &toVb);

	owner->CmdQ->ExecuteCommandList(cmd);

	AppendCpuRuntimeTrace(
		L"[DX12RTAS] BuildSkeletalBLAS vertices=" + std::to_wstring(mesh->SkeletalOutputVb->numVertices) +
		L", indices=" + std::to_wstring(mesh->Ib->numIndices));

	as->MeshPtr = mesh;
	return shared_ptr<RTAS>(as);
}

void DX12Backend::RefitBLAS(RTAS* rtas, Mesh* mesh)
{
	D3D12RTAS* as = static_cast<D3D12RTAS*>(rtas);
	if (!as || !as->Result || !as->Scratch || !mesh || !mesh->SkeletalOutputVb || !mesh->Ib)
		return;

	// Phase A: per-char vertex offset into the unified output VB. Must
	// match what CreateBLASForSkeletalMesh used at build time.
	const UINT64 vertexByteOffset =
		static_cast<UINT64>(mesh->SkeletalCharIndex) *
		static_cast<UINT64>(mesh->SkeletalVertexCount) *
		static_cast<UINT64>(mesh->VertexStride);

	D3D12_RAYTRACING_GEOMETRY_DESC geomDesc = {};
	geomDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
	geomDesc.Triangles.VertexBuffer.StartAddress =
		mesh->SkeletalOutputVb->resource->GetGPUVirtualAddress() + vertexByteOffset;
	geomDesc.Triangles.VertexBuffer.StrideInBytes = mesh->VertexStride;
	geomDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
	geomDesc.Triangles.VertexCount = mesh->SkeletalVertexCount;
	geomDesc.Triangles.IndexBuffer = mesh->Ib->resource->GetGPUVirtualAddress();
	geomDesc.Triangles.IndexFormat = ToDXGIFormat(mesh->IndexFormat);
	geomDesc.Triangles.IndexCount = mesh->Ib->numIndices;
	geomDesc.Triangles.Transform3x4 = 0;
	geomDesc.Flags = mesh->bTransparent
		? D3D12_RAYTRACING_GEOMETRY_FLAG_NONE
		: D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
	inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	inputs.Flags =
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE |
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
	inputs.NumDescs = 1;
	inputs.pGeometryDescs = &geomDesc;
	inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;

	// The skinned vertex buffer is in VertexBuffer state after dispatch
	// transitions; flip to NON_PIXEL_SHADER_RESOURCE for the BLAS update,
	// then back.
	D3D12_RESOURCE_BARRIER toSrv = {};
	toSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	toSrv.Transition.pResource = mesh->SkeletalOutputVb->resource.Get();
	toSrv.Transition.Subresource = 0;
	toSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	toSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	// State already includes NPS_RESOURCE, so the barrier is a no-op when
	// the buffer is in the combined state. Skip emitting it; rely on
	// existing dispatch UA -> VertexBuffer transition. SkeletalOutputVb was
	// created with R32_TYPELESS RAW SRV available.

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC asDesc = {};
	asDesc.Inputs = inputs;
	asDesc.DestAccelerationStructureData = as->ResultGpuVA;
	asDesc.SourceAccelerationStructureData = as->ResultGpuVA;
	asDesc.ScratchAccelerationStructureData = as->ScratchGpuVA;

	GlobalCmdList->CmdList->BuildRaytracingAccelerationStructure(&asDesc, 0, nullptr);

	D3D12_RESOURCE_BARRIER uavBarrier = {};
	uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	uavBarrier.UAV.pResource = as->Result.Get();
	GlobalCmdList->CmdList->ResourceBarrier(1, &uavBarrier);
}

std::shared_ptr<RTAS> DX12Backend::CreateBLASForMesh(Mesh* mesh)
{
	assert(mesh);
	const bool bHasVb = mesh && mesh->Vb;
	const bool bHasIb = mesh && mesh->Ib;
	const bool bHasVbResource = bHasVb && mesh->Vb->resource.Get();
	const bool bHasIbResource = bHasIb && mesh->Ib->resource.Get();
	if (!mesh ||
		!bHasVbResource ||
		!bHasIbResource ||
		mesh->VertexStride == 0 ||
		mesh->Vb->numVertices <= 0 ||
		mesh->Ib->numIndices < 3)
	{
		AppendCpuRuntimeTrace(
			L"[DX12RTAS] CreateBLASForMesh skipped invalid mesh"
			L", mesh=" + FormatDx12Hex(reinterpret_cast<uint64_t>(mesh)) +
			L", hasVb=" + std::to_wstring(bHasVb ? 1 : 0) +
			L", hasIb=" + std::to_wstring(bHasIb ? 1 : 0) +
			L", hasVbResource=" + std::to_wstring(bHasVbResource ? 1 : 0) +
			L", hasIbResource=" + std::to_wstring(bHasIbResource ? 1 : 0) +
			L", vertexStride=" + std::to_wstring(mesh ? mesh->VertexStride : 0) +
			L", vertices=" + std::to_wstring(bHasVb ? mesh->Vb->numVertices : 0) +
			L", indices=" + std::to_wstring(bHasIb ? mesh->Ib->numIndices : 0));
		return nullptr;
	}

	DX12Backend* owner = this;
	D3D12RTAS* as = new D3D12RTAS;


	D3D12_RAYTRACING_GEOMETRY_DESC geomDesc = {};
	geomDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
	geomDesc.Triangles.VertexBuffer.StartAddress = mesh->Vb->resource->GetGPUVirtualAddress();
	geomDesc.Triangles.VertexBuffer.StrideInBytes = mesh->VertexStride;
	geomDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
	geomDesc.Triangles.VertexCount = mesh->Vb->numVertices;
	geomDesc.Triangles.IndexBuffer = mesh->Ib->resource->GetGPUVirtualAddress();
	geomDesc.Triangles.IndexFormat = ToDXGIFormat(mesh->IndexFormat);
	geomDesc.Triangles.IndexCount = mesh->Ib->numIndices;
	geomDesc.Triangles.Transform3x4 = 0;

	// Only alpha-tested geometry needs any-hit. Opaque geometry can use the faster traversal path.
	geomDesc.Flags = mesh->bTransparent
		? D3D12_RAYTRACING_GEOMETRY_FLAG_NONE
		: D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;



	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
	inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
	inputs.NumDescs = 1;
	inputs.pGeometryDescs = &geomDesc;
	inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
	owner->Device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
	if (info.ResultDataMaxSizeInBytes == 0 || info.ScratchDataSizeInBytes == 0)
	{
		AppendCpuRuntimeTrace(
			L"[DX12RTAS] CreateBLASForMesh skipped empty prebuild info"
			L", resultBytes=" + std::to_wstring(info.ResultDataMaxSizeInBytes) +
			L", scratchBytes=" + std::to_wstring(info.ScratchDataSizeInBytes));
		delete as;
		return nullptr;
	}

	{
		D3D12_RESOURCE_DESC bufDesc = {};
		bufDesc.Alignment = 0;
		bufDesc.DepthOrArraySize = 1;
		bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		bufDesc.Format = DXGI_FORMAT_UNKNOWN;
		bufDesc.Height = 1;
		bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		bufDesc.MipLevels = 1;
		bufDesc.SampleDesc.Count = 1;
		bufDesc.SampleDesc.Quality = 0;
		bufDesc.Width = info.ScratchDataSizeInBytes;

		/*stringstream ss;
		ss << "blas->scratch : " << bufDesc.Width << "\n";
		OutputDebugStringA(ss.str().c_str());*/

		owner->Device->CreateCommittedResource(&kDefaultHeapProps, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&as->Scratch));
		if (as->Scratch)
			as->Scratch->SetName(L"Corona BLAS Scratch");
	}
	
	{
		D3D12_RESOURCE_DESC bufDesc = {};
		bufDesc.Alignment = 0;
		bufDesc.DepthOrArraySize = 1;
		bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		bufDesc.Format = DXGI_FORMAT_UNKNOWN;
		bufDesc.Height = 1;
		bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		bufDesc.MipLevels = 1;
		bufDesc.SampleDesc.Count = 1;
		bufDesc.SampleDesc.Quality = 0;
		bufDesc.Width = info.ResultDataMaxSizeInBytes;
		
		/*stringstream ss;
		ss << "blas->result : " << bufDesc.Width << "\n";
		OutputDebugStringA(ss.str().c_str());*/


		// Place the traversed BLAS (BVH nodes) in the big-page geometry pool so
		// ray traversal's scattered node reads thrash the TPC uTLB less. Scratch
		// stays committed (build-time only, not traversed).
		const GeometryPlacement asPlacement = AllocateGeometryPlacement(info.ResultDataMaxSizeInBytes, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
		owner->Device->CreatePlacedResource(asPlacement.heap, asPlacement.offset, &bufDesc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr, IID_PPV_ARGS(&as->Result));
		if (as->Result)
			as->Result->SetName(L"Corona BLAS Result");
	}
	if (!as->Scratch || !as->Result)
	{
		AppendCpuRuntimeTrace(
			L"[DX12RTAS] CreateBLASForMesh resource allocation failed"
			L", hasScratch=" + std::to_wstring(as->Scratch ? 1 : 0) +
			L", hasResult=" + std::to_wstring(as->Result ? 1 : 0) +
			L", resultBytes=" + std::to_wstring(info.ResultDataMaxSizeInBytes) +
			L", scratchBytes=" + std::to_wstring(info.ScratchDataSizeInBytes));
		delete as;
		return nullptr;
	}
	as->ScratchGpuVA = as->Scratch->GetGPUVirtualAddress();
	as->ResultGpuVA = as->Result->GetGPUVirtualAddress();

	CommandList* cmd = owner->CmdQ->AllocCmdList();

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC asDesc = {};
	asDesc.Inputs = inputs;
	asDesc.DestAccelerationStructureData = as->ResultGpuVA;
	asDesc.ScratchAccelerationStructureData = as->ScratchGpuVA;

	AppendCpuRuntimeTrace(
		L"[DX12RTAS] BuildBLAS begin vertices=" + std::to_wstring(mesh->Vb->numVertices) +
		L", indices=" + std::to_wstring(mesh->Ib->numIndices) +
		L", result=" + FormatDx12Hex(asDesc.DestAccelerationStructureData) +
		L", scratch=" + FormatDx12Hex(asDesc.ScratchAccelerationStructureData) +
		L", resultBytes=" + std::to_wstring(info.ResultDataMaxSizeInBytes) +
		L", scratchBytes=" + std::to_wstring(info.ScratchDataSizeInBytes));
	cmd->CmdList->BuildRaytracingAccelerationStructure(&asDesc, 0, nullptr);

	D3D12_RESOURCE_BARRIER uavBarrier = {};
	uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	uavBarrier.UAV.pResource = as->Result.Get();
	cmd->CmdList->ResourceBarrier(1, &uavBarrier);

	owner->CmdQ->ExecuteCommandList(cmd);
	AppendCpuRuntimeTrace(
		L"[DX12RTAS] BuildBLAS submitted result=" + FormatDx12Hex(asDesc.DestAccelerationStructureData));

	as->MeshPtr = mesh;
	return shared_ptr<RTAS>(as);
}

static bool WriteD3D12TLASInstanceDescs(D3D12RTAS* as, const std::vector<RTInstanceDesc>& instances)
{
	if (!as || !as->Instance || instances.size() > static_cast<size_t>(UINT_MAX))
		return false;

	const UINT instanceCount = static_cast<UINT>(instances.size());
	if (as->InstanceCapacity != 0 && instanceCount > as->InstanceCapacity)
		return false;

	if (!as->InstanceMapped)
	{
		CD3DX12_RANGE readRange(0, 0);
		const HRESULT mapResult = as->Instance->Map(0, &readRange, reinterpret_cast<void**>(&as->InstanceMapped));
		if (FAILED(mapResult) || !as->InstanceMapped)
		{
			AppendCpuRuntimeTrace(
				L"[DX12RTAS] TLAS instance Map failed hr=" + FormatDx12Hex(static_cast<uint32_t>(mapResult)) +
				L", instances=" + std::to_wstring(instances.size()));
			return false;
		}
	}

	bool bValid = true;
	for (UINT i = 0; i < instanceCount; ++i)
	{
		D3D12RTAS* blas = static_cast<D3D12RTAS*>(instances[i].BottomLevelAS.get());
		if (!blas || !blas->Result || blas->ResultGpuVA == 0)
		{
			bValid = false;
			break;
		}

		D3D12_RAYTRACING_INSTANCE_DESC& instanceDesc = as->InstanceMapped[i];
		instanceDesc.InstanceID = i;
		// Per-instance material/geometry data comes from InstanceID() + bindless
		// instance buffers. Keep all instances on the same hit record so the SBT
		// does not scale with scene instance count.
		instanceDesc.InstanceContributionToHitGroupIndex = 0;
		instanceDesc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
		const glm::mat4x4& transform = instances[i].Transform;
		instanceDesc.Transform[0][0] = transform[0][0];
		instanceDesc.Transform[0][1] = transform[1][0];
		instanceDesc.Transform[0][2] = transform[2][0];
		instanceDesc.Transform[0][3] = transform[3][0];
		instanceDesc.Transform[1][0] = transform[0][1];
		instanceDesc.Transform[1][1] = transform[1][1];
		instanceDesc.Transform[1][2] = transform[2][1];
		instanceDesc.Transform[1][3] = transform[3][1];
		instanceDesc.Transform[2][0] = transform[0][2];
		instanceDesc.Transform[2][1] = transform[1][2];
		instanceDesc.Transform[2][2] = transform[2][2];
		instanceDesc.Transform[2][3] = transform[3][2];
		instanceDesc.AccelerationStructure = blas->ResultGpuVA;
		instanceDesc.InstanceMask = 0xFF;
	}

	return bValid;
}

std::shared_ptr<RTAS> DX12Backend::CreateTLAS(const std::vector<RTInstanceDesc>& instances)
{
	if (bDeviceLost || !Device || !CmdQ || instances.empty() || instances.size() > static_cast<size_t>(UINT_MAX))
		return nullptr;

	D3D12RTAS* as = new D3D12RTAS;
	auto failCreateTLAS = [&](const wchar_t* context, HRESULT hr) -> std::shared_ptr<RTAS>
	{
		const HRESULT deviceRemovedReason = Device ? Device->GetDeviceRemovedReason() : S_OK;
		AppendCpuRuntimeTrace(
			L"[DX12RTAS] CreateTLAS failed context=\"" +
			std::wstring(context ? context : L"unknown") +
			L"\", hr=" + FormatHexHRESULT(hr) +
			L", deviceRemovedReason=" + FormatHexHRESULT(deviceRemovedReason) +
			L", instances=" + std::to_wstring(instances.size()));
		MarkDeviceLost(context ? context : L"DX12Backend::CreateTLAS", hr);
		delete as;
		return nullptr;
	};

	// First, get the size of the TLAS buffers and create them
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
	inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	inputs.Flags =
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
	inputs.NumDescs = static_cast<UINT>(instances.size());
	inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info;
	Device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
	if (info.ResultDataMaxSizeInBytes == 0 || info.ScratchDataSizeInBytes == 0)
		return failCreateTLAS(L"DX12Backend::CreateTLAS PrebuildInfo", E_FAIL);
	UINT64 scratchDataSize = info.ScratchDataSizeInBytes;
	if (info.UpdateScratchDataSizeInBytes > scratchDataSize)
		scratchDataSize = info.UpdateScratchDataSizeInBytes;

	{
		D3D12_RESOURCE_DESC bufDesc = {};
		bufDesc.Alignment = 0;
		bufDesc.DepthOrArraySize = 1;
		bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		bufDesc.Format = DXGI_FORMAT_UNKNOWN;
		bufDesc.Height = 1;
		bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		bufDesc.MipLevels = 1;
		bufDesc.SampleDesc.Count = 1;
		bufDesc.SampleDesc.Quality = 0;
		bufDesc.Width = scratchDataSize;

		const HRESULT hr = Device->CreateCommittedResource(&kDefaultHeapProps, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&as->Scratch));
		if (FAILED(hr) || !as->Scratch)
			return failCreateTLAS(L"DX12Backend::CreateTLAS Scratch", hr);
		if (as->Scratch)
			as->Scratch->SetName(L"Corona TLAS Scratch");
	}

	{
		D3D12_RESOURCE_DESC bufDesc = {};
		bufDesc.Alignment = 0;
		bufDesc.DepthOrArraySize = 1;
		bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		bufDesc.Format = DXGI_FORMAT_UNKNOWN;
		bufDesc.Height = 1;
		bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		bufDesc.MipLevels = 1;
		bufDesc.SampleDesc.Count = 1;
		bufDesc.SampleDesc.Quality = 0;
		bufDesc.Width = info.ResultDataMaxSizeInBytes;

		const HRESULT hr = Device->CreateCommittedResource(&kDefaultHeapProps, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr, IID_PPV_ARGS(&as->Result));
		if (FAILED(hr) || !as->Result)
			return failCreateTLAS(L"DX12Backend::CreateTLAS Result", hr);
		if (as->Result)
			as->Result->SetName(L"Corona TLAS Result");
	}

	{
		D3D12_RESOURCE_DESC bufDesc = {};
		bufDesc.Alignment = 0;
		bufDesc.DepthOrArraySize = 1;
		bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bufDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
		bufDesc.Format = DXGI_FORMAT_UNKNOWN;
		bufDesc.Height = 1;
		bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		bufDesc.MipLevels = 1;
		bufDesc.SampleDesc.Count = 1;
		bufDesc.SampleDesc.Quality = 0;
		bufDesc.Width = sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * instances.size();

		const HRESULT hr = Device->CreateCommittedResource(&kUploadHeapProps, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&as->Instance));
		if (FAILED(hr) || !as->Instance)
			return failCreateTLAS(L"DX12Backend::CreateTLAS InstanceDescs", hr);
		if (as->Instance)
			as->Instance->SetName(L"Corona TLAS Instance Descs");
	}
	as->InstanceCapacity = static_cast<UINT>(instances.size());
	as->ScratchGpuVA = as->Scratch->GetGPUVirtualAddress();
	as->ResultGpuVA = as->Result->GetGPUVirtualAddress();
	as->InstanceGpuVA = as->Instance->GetGPUVirtualAddress();

	if (!WriteD3D12TLASInstanceDescs(as, instances))
	{
		const HRESULT deviceRemovedReason = Device ? Device->GetDeviceRemovedReason() : S_OK;
		MarkDeviceLost(L"DX12Backend::CreateTLAS WriteInstanceDescs", deviceRemovedReason);
		delete as;
		return nullptr;
	}
	
	// Create the TLAS

	CommandList* cmd = CmdQ->AllocCmdList();
	if (!cmd || bDeviceLost)
	{
		delete as;
		return nullptr;
	}

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC asDesc = {};
	asDesc.Inputs = inputs;

	if (!instances.empty())
		asDesc.Inputs.InstanceDescs = as->InstanceGpuVA;
	asDesc.DestAccelerationStructureData = as->ResultGpuVA;
	asDesc.ScratchAccelerationStructureData = as->ScratchGpuVA;

	AppendCpuRuntimeTrace(
		L"[DX12RTAS] BuildTLAS begin instances=" + std::to_wstring(instances.size()) +
		L", result=" + FormatDx12Hex(asDesc.DestAccelerationStructureData) +
		L", scratch=" + FormatDx12Hex(asDesc.ScratchAccelerationStructureData) +
		L", instanceDesc=" + FormatDx12Hex(asDesc.Inputs.InstanceDescs) +
		L", resultBytes=" + std::to_wstring(info.ResultDataMaxSizeInBytes) +
		L", scratchBytes=" + std::to_wstring(scratchDataSize));
	cmd->CmdList->BuildRaytracingAccelerationStructure(&asDesc, 0, nullptr);

	// We need to insert a UAV barrier before using the acceleration structures in a raytracing operation
	D3D12_RESOURCE_BARRIER uavBarrier = {};
	uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	uavBarrier.UAV.pResource = as->Result.Get();
	cmd->CmdList->ResourceBarrier(1, &uavBarrier);

	// create acceleration structure srv (not shader-visible yet)
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.RaytracingAccelerationStructure.Location = as->ResultGpuVA;

	// copydescriptor needed when being used.
	GeomtryDHRing->AllocDescriptor(as->CPUHandle, as->GPUHandle);

	Device->CreateShaderResourceView(nullptr, &srvDesc, as->CPUHandle);

	CmdQ->ExecuteCommandList(cmd);
	if (bDeviceLost)
	{
		delete as;
		return nullptr;
	}
	AppendCpuRuntimeTrace(
		L"[DX12RTAS] BuildTLAS submitted result=" + FormatDx12Hex(asDesc.DestAccelerationStructureData));

	as->NumInstances = static_cast<UINT>(instances.size());
	return shared_ptr<RTAS>(as);
}

bool DX12Backend::UpdateTLAS(const std::shared_ptr<RTAS>& topLevelAS, const std::vector<RTInstanceDesc>& instances)
{
	if (bDeviceLost || !Device || !CmdQ)
		return false;

	D3D12RTAS* as = dynamic_cast<D3D12RTAS*>(topLevelAS.get());
	if (!as || !as->Scratch || !as->Result || !as->Instance)
		return false;
	if (instances.empty() || instances.size() != as->NumInstances || instances.size() > static_cast<size_t>(UINT_MAX))
		return false;
	if (!WriteD3D12TLASInstanceDescs(as, instances))
	{
		const HRESULT deviceRemovedReason = Device ? Device->GetDeviceRemovedReason() : S_OK;
		MarkDeviceLost(L"DX12Backend::UpdateTLAS WriteInstanceDescs", deviceRemovedReason);
		return false;
	}

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
	inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	inputs.Flags =
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE |
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
	inputs.NumDescs = static_cast<UINT>(instances.size());
	inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
	inputs.InstanceDescs = as->InstanceGpuVA;

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC asDesc = {};
	asDesc.Inputs = inputs;
	asDesc.SourceAccelerationStructureData = as->ResultGpuVA;
	asDesc.DestAccelerationStructureData = as->ResultGpuVA;
	asDesc.ScratchAccelerationStructureData = as->ScratchGpuVA;

	static UINT64 sUpdateTlasTraceCount = 0;
	const UINT64 updateTraceIndex = sUpdateTlasTraceCount++;
	const bool bTraceUpdateTlas = updateTraceIndex < 8 || (updateTraceIndex % 120) == 0;
	if (bTraceUpdateTlas)
	{
		AppendCpuRuntimeTrace(
			L"[DX12RTAS] UpdateTLAS begin updateIndex=" + std::to_wstring(updateTraceIndex) +
			L", instances=" + std::to_wstring(instances.size()) +
			L", result=" + FormatDx12Hex(asDesc.DestAccelerationStructureData) +
			L", scratch=" + FormatDx12Hex(asDesc.ScratchAccelerationStructureData) +
			L", instanceDesc=" + FormatDx12Hex(asDesc.Inputs.InstanceDescs));
	}

	CommandList* cmd = GlobalCmdList;
	bool bSubmitImmediately = false;
	if (!cmd)
	{
		cmd = CmdQ->AllocCmdList();
		if (!cmd || bDeviceLost)
			return false;
		bSubmitImmediately = true;
	}

	cmd->CmdList->BuildRaytracingAccelerationStructure(&asDesc, 0, nullptr);

	D3D12_RESOURCE_BARRIER uavBarrier = {};
	uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	uavBarrier.UAV.pResource = as->Result.Get();
	cmd->CmdList->ResourceBarrier(1, &uavBarrier);

	if (bSubmitImmediately)
	{
		CmdQ->ExecuteCommandList(cmd);
		if (bDeviceLost)
			return false;
	}
	if (bTraceUpdateTlas)
	{
		AppendCpuRuntimeTrace(
			L"[DX12RTAS] UpdateTLAS " +
			std::wstring(bSubmitImmediately ? L"submitted" : L"recorded") +
			L" updateIndex=" + std::to_wstring(updateTraceIndex) +
			L", result=" + FormatDx12Hex(asDesc.DestAccelerationStructureData));
	}
	return true;
}

std::shared_ptr<RTPipelineStateObject> DX12Backend::CreateRTPipelineStateObject()
{
	auto pso = std::make_shared<D3D12RTPipelineStateObject>();
	pso->Owner = this;
	return pso;
}

std::shared_ptr<ComputePipelineStateObject> DX12Backend::CreateComputePipelineStateObject()
{
	auto pso = std::make_shared<D3D12ComputePipelineStateObject>();
	pso->Owner = this;
	return pso;
}

ShaderBytecode DX12Backend::CreateShader(const std::wstring& FilePath, const std::string& EntryPoint, const std::string& Target)
{
	const std::wstring targetWide = ToWide(Target);
	ComPtr<ID3DBlob> blob = compileShaderDXC(this, FilePath.c_str(), EntryPoint, targetWide.c_str());
	ShaderBytecode result;
	if (blob)
	{
		const size_t size = blob->GetBufferSize();
		const uint8_t* src = static_cast<const uint8_t*>(blob->GetBufferPointer());
		result.Data.assign(src, src + size);
	}
	return result;
}

template<class BlotType>
std::string convertBlobToString(BlotType* pBlob)
{
	std::vector<char> infoLog(pBlob->GetBufferSize() + 1);
	memcpy(infoLog.data(), pBlob->GetBufferPointer(), pBlob->GetBufferSize());
	infoLog[pBlob->GetBufferSize()] = 0;
	return std::string(infoLog.data());
}

static dxc::DxcDllSupport gDxcDllHelper;

static HRESULT InitializeDxcCompiler(DX12Backend* owner)
{
	static bool bInitialized = false;
	static HRESULT initResult = E_FAIL;
	static std::wstring loadedPath;

	if (bInitialized)
		return initResult;

	bInitialized = true;

#ifdef _WIN32
	std::vector<std::filesystem::path> candidateDlls;
	candidateDlls.push_back(RuntimePaths::RootDirectory() / L"bin" / L"dxcompiler.dll");

	wchar_t modulePath[MAX_PATH] = {};
	const DWORD modulePathLength = GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(std::size(modulePath)));
	if (modulePathLength > 0 && modulePathLength < std::size(modulePath))
		candidateDlls.push_back(std::filesystem::path(modulePath).parent_path() / L"dxcompiler.dll");

	if (const wchar_t* vulkanSdk = _wgetenv(L"VULKAN_SDK"))
		candidateDlls.push_back(std::filesystem::path(vulkanSdk) / L"Bin" / L"dxcompiler.dll");

	for (const std::filesystem::path& candidate : candidateDlls)
	{
		if (!std::filesystem::exists(candidate))
			continue;

		initResult = gDxcDllHelper.InitializeForDll(candidate.c_str(), "DxcCreateInstance");
		if (SUCCEEDED(initResult))
		{
			loadedPath = candidate.wstring();
			break;
		}
	}
#endif

	if (FAILED(initResult))
	{
		initResult = gDxcDllHelper.Initialize();
		if (SUCCEEDED(initResult))
			loadedPath = L"dxcompiler.dll";
	}

	if (SUCCEEDED(initResult))
	{
		// Fold the compiler binary's identity into the shader-cache key so a
		// dxcompiler.dll upgrade invalidates previously cached DXIL.
		std::error_code stampEc;
		const std::filesystem::path compilerPath(loadedPath);
		const auto compilerSize = std::filesystem::file_size(compilerPath, stampEc);
		uint64_t stamp = stampEc ? 0 : static_cast<uint64_t>(compilerSize);
		std::error_code timeEc;
		const auto compilerTime = std::filesystem::last_write_time(compilerPath, timeEc);
		if (!timeEc)
			stamp ^= static_cast<uint64_t>(compilerTime.time_since_epoch().count()) * PipelineCache::kFnvPrime;
		PipelineCache::gCompilerStamp = stamp;

		AppendCpuRuntimeTrace(L"[DXC] loaded " + loadedPath);
	}
	else
	{
		std::wstringstream hrStream;
		hrStream << std::hex << std::uppercase << static_cast<unsigned long>(initResult);
		AppendCpuRuntimeTrace(L"[DXC] failed to load dxcompiler.dll hr=0x" + hrStream.str());
		if (owner)
			owner->errorString += "Failed to load dxcompiler.dll.\n";
	}

	return initResult;
}

ComPtr<ID3DBlob> compileShaderDXC(DX12Backend* owner, const WCHAR* filename, const std::string& entryPoint, const WCHAR* targetString)
{
	if (FAILED(InitializeDxcCompiler(owner)))
		return nullptr;

	const uint64_t cacheKey = PipelineCache::ShaderKey(filename, entryPoint, targetString, {});
	if (ComPtr<ID3DBlob> cached = PipelineCache::LoadDxil(cacheKey))
		return cached;

	ComPtr<IDxcCompiler> pCompiler;
	ComPtr<IDxcLibrary> pLibrary;
	ComPtr<IDxcIncludeHandler> dxcIncludeHandler;
	gDxcDllHelper.CreateInstance(CLSID_DxcCompiler, __uuidof(IDxcCompiler), &pCompiler);
	gDxcDllHelper.CreateInstance(CLSID_DxcLibrary, __uuidof(IDxcLibrary), &pLibrary);
	pLibrary->CreateIncludeHandler(&dxcIncludeHandler);

	std::ifstream shaderFile(filename);
	if (shaderFile.good() == false)
	{
		if (owner)
			owner->errorString += "Can't open shader file.\n";
		return nullptr;
	}

	std::stringstream strStream;
	strStream << shaderFile.rdbuf();
	std::string shader = strStream.str();

	ComPtr<IDxcBlobEncoding> pTextBlob;
	pLibrary->CreateBlobWithEncodingFromPinned((LPBYTE)shader.c_str(), (uint32_t)shader.size(), 0, &pTextBlob);

	ComPtr<IDxcOperationResult> pResult;
#if defined(_DEBUG)
	LPCWSTR compileArgs[] = { DXC_ARG_DEBUG, DXC_ARG_SKIP_OPTIMIZATIONS };
#else
	LPCWSTR compileArgs[] = { DXC_ARG_OPTIMIZATION_LEVEL3 };
#endif
	const std::wstring entryPointWide = ToWide(entryPoint);
	pCompiler->Compile(
		pTextBlob.Get(),
		filename,
		entryPointWide.c_str(),
		targetString,
		compileArgs,
		_countof(compileArgs),
		nullptr,
		0,
		dxcIncludeHandler.Get(),
		&pResult);

	HRESULT resultCode;
	pResult->GetStatus(&resultCode);
	if (FAILED(resultCode))
	{
		ComPtr<IDxcBlobEncoding> pError;
		pResult->GetErrorBuffer(&pError);
		std::string log = convertBlobToString(pError.Get());
		if (owner)
			owner->errorString += log;
		OutputDebugStringA(log.c_str());
		return nullptr;
	}

	ID3DBlob* pBlob = nullptr;
	pResult->GetResult((IDxcBlob**)&pBlob);
	PipelineCache::StoreDxil(cacheKey, pBlob);
	return ComPtr<ID3DBlob>(pBlob);
}

ComPtr<ID3DBlob> compileShaderLibrary(
	DX12Backend* owner,
	const WCHAR* filename,
	const WCHAR* targetString,
	const std::vector<std::pair<std::string, std::string>>& defines = {})
{
	if (FAILED(InitializeDxcCompiler(owner)))
		return nullptr;

	const uint64_t cacheKey = PipelineCache::ShaderKey(filename, std::string(), targetString, defines);
	if (ComPtr<ID3DBlob> cached = PipelineCache::LoadDxil(cacheKey))
		return cached;

	ComPtr<IDxcCompiler> pCompiler;
	ComPtr<IDxcLibrary> pLibrary;
	ComPtr<IDxcIncludeHandler> dxcIncludeHandler;
	gDxcDllHelper.CreateInstance(CLSID_DxcCompiler, __uuidof(IDxcCompiler), &pCompiler);
	gDxcDllHelper.CreateInstance(CLSID_DxcLibrary, __uuidof(IDxcLibrary), &pLibrary);
	pLibrary->CreateIncludeHandler(&dxcIncludeHandler);

	// Open and read the file
	std::ifstream shaderFile(filename);
	if (shaderFile.good() == false)
	{
		//msgBox("Can't open file " + wstring_2_string(std::wstring(filename)));
		return nullptr;
	}
	std::stringstream strStream;
	strStream << shaderFile.rdbuf();
	std::string shader = strStream.str();

	// Create blob from the string
	ComPtr<IDxcBlobEncoding> pTextBlob;
	pLibrary->CreateBlobWithEncodingFromPinned((LPBYTE)shader.c_str(), (uint32_t)shader.size(), 0, &pTextBlob);

	// Compile
	ComPtr<IDxcOperationResult> pResult;
#if defined(_DEBUG)
	LPCWSTR compileArgs[] = { DXC_ARG_DEBUG, DXC_ARG_SKIP_OPTIMIZATIONS };
#else
	LPCWSTR compileArgs[] = { DXC_ARG_OPTIMIZATION_LEVEL3 };
#endif
	std::vector<std::wstring> defineNames;
	std::vector<std::wstring> defineValues;
	std::vector<DxcDefine> dxcDefines;
	defineNames.reserve(defines.size());
	defineValues.reserve(defines.size());
	dxcDefines.reserve(defines.size());
	for (const auto& define : defines)
	{
		defineNames.push_back(ToWide(define.first));
		defineValues.push_back(ToWide(define.second));
	}
	for (size_t defineIndex = 0; defineIndex < defines.size(); ++defineIndex)
	{
		DxcDefine dxcDefine{};
		dxcDefine.Name = defineNames[defineIndex].c_str();
		dxcDefine.Value = defineValues[defineIndex].c_str();
		dxcDefines.push_back(dxcDefine);
	}
	pCompiler->Compile(
		pTextBlob.Get(),
		filename,
		L"",
		targetString,
		compileArgs,
		_countof(compileArgs),
		dxcDefines.empty() ? nullptr : dxcDefines.data(),
		static_cast<UINT32>(dxcDefines.size()),
		dxcIncludeHandler.Get(),
		&pResult);

	// Verify the result
	HRESULT resultCode;
	pResult->GetStatus(&resultCode);
	if (FAILED(resultCode))
	{
		ComPtr<IDxcBlobEncoding> pError;
		pResult->GetErrorBuffer(&pError);
		std::string log = convertBlobToString(pError.Get());
		//msgBox("Compiler error:\n" + log);
		if (owner)
		{
			owner->errorString += log;
		}
		AppendCpuRuntimeTrace(
			L"[DXC] library compile failed shader=\"" + std::wstring(filename) +
			L"\" target=\"" + std::wstring(targetString) +
			L"\" log=\"" + ToWide(log.substr(0, 4096)) + L"\"");
		OutputDebugStringA(log.c_str());

		return nullptr;
	}

	ID3DBlob* pBlob;
	pResult->GetResult((IDxcBlob**)&pBlob);
	PipelineCache::StoreDxil(cacheKey, pBlob);
	return ComPtr<ID3DBlob>(pBlob);
}

struct DxilLibrary
{
	DxilLibrary(ComPtr<ID3DBlob> pBlob, const WCHAR* entryPoint[], uint32_t entryPointCount) : pShaderBlob(pBlob)
	{
		stateSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
		stateSubobject.pDesc = &dxilLibDesc;

		dxilLibDesc = {};
		exportDesc.resize(entryPointCount);
		exportName.resize(entryPointCount);
		if (pBlob)
		{
			dxilLibDesc.DXILLibrary.pShaderBytecode = pBlob->GetBufferPointer();
			dxilLibDesc.DXILLibrary.BytecodeLength = pBlob->GetBufferSize();
			dxilLibDesc.NumExports = entryPointCount;
			dxilLibDesc.pExports = exportDesc.data();

			for (uint32_t i = 0; i < entryPointCount; i++)
			{
				exportName[i] = entryPoint[i];
				exportDesc[i].Name = exportName[i].c_str();
				exportDesc[i].Flags = D3D12_EXPORT_FLAG_NONE;
				exportDesc[i].ExportToRename = nullptr;
			}
		}
	};

	DxilLibrary() : DxilLibrary(nullptr, nullptr, 0) {}

	D3D12_DXIL_LIBRARY_DESC dxilLibDesc = {};
	D3D12_STATE_SUBOBJECT stateSubobject{};
	ComPtr<ID3DBlob> pShaderBlob;
	std::vector<D3D12_EXPORT_DESC> exportDesc;
	std::vector<std::wstring> exportName;
};

#define arraysize(a) (sizeof(a)/sizeof(a[0]))

wstring StringToWString(const std::string &s)
{
	std::wstring wsTmp(s.begin(), s.end());

	wstring ws = wsTmp;

	return ws;
}

static D3D12_DESCRIPTOR_RANGE_FLAGS GetRtDescriptorRangeFlags(D3D12_DESCRIPTOR_RANGE_TYPE rangeType)
{
	D3D12_DESCRIPTOR_RANGE_FLAGS flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
	if (rangeType != D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER)
		flags |= D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
	return flags;
}

ComPtr<ID3D12RootSignature> CreateRootSignature(ComPtr<ID3D12Device5> pDevice, const D3D12_ROOT_SIGNATURE_DESC& desc)
{
	ComPtr<ID3DBlob> pSigBlob;
	ComPtr<ID3DBlob> pErrorBlob;

	D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = {};
	featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
	if (FAILED(pDevice->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData))))
		featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;

	HRESULT hr = E_FAIL;
	if (featureData.HighestVersion >= D3D_ROOT_SIGNATURE_VERSION_1_1)
	{
		UINT descriptorRangeCount = 0;
		for (UINT paramIndex = 0; paramIndex < desc.NumParameters; ++paramIndex)
		{
			const D3D12_ROOT_PARAMETER& param = desc.pParameters[paramIndex];
			if (param.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
				descriptorRangeCount += param.DescriptorTable.NumDescriptorRanges;
		}

		std::vector<D3D12_DESCRIPTOR_RANGE1> descriptorRanges;
		descriptorRanges.reserve(descriptorRangeCount);
		std::vector<D3D12_ROOT_PARAMETER1> rootParams(desc.NumParameters);
		for (UINT paramIndex = 0; paramIndex < desc.NumParameters; ++paramIndex)
		{
			const D3D12_ROOT_PARAMETER& src = desc.pParameters[paramIndex];
			D3D12_ROOT_PARAMETER1& dst = rootParams[paramIndex];
			dst.ParameterType = src.ParameterType;
			dst.ShaderVisibility = src.ShaderVisibility;
			switch (src.ParameterType)
			{
			case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
			{
				const UINT rangeOffset = static_cast<UINT>(descriptorRanges.size());
				for (UINT rangeIndex = 0; rangeIndex < src.DescriptorTable.NumDescriptorRanges; ++rangeIndex)
				{
					const D3D12_DESCRIPTOR_RANGE& srcRange = src.DescriptorTable.pDescriptorRanges[rangeIndex];
					D3D12_DESCRIPTOR_RANGE1 dstRange = {};
					dstRange.RangeType = srcRange.RangeType;
					dstRange.NumDescriptors = srcRange.NumDescriptors;
					dstRange.BaseShaderRegister = srcRange.BaseShaderRegister;
					dstRange.RegisterSpace = srcRange.RegisterSpace;
					dstRange.Flags = GetRtDescriptorRangeFlags(srcRange.RangeType);
					dstRange.OffsetInDescriptorsFromTableStart = srcRange.OffsetInDescriptorsFromTableStart;
					descriptorRanges.push_back(dstRange);
				}
				dst.DescriptorTable.NumDescriptorRanges = src.DescriptorTable.NumDescriptorRanges;
				dst.DescriptorTable.pDescriptorRanges = descriptorRanges.data() + rangeOffset;
				break;
			}
			case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
				dst.Constants = src.Constants;
				break;
			case D3D12_ROOT_PARAMETER_TYPE_CBV:
			case D3D12_ROOT_PARAMETER_TYPE_SRV:
			case D3D12_ROOT_PARAMETER_TYPE_UAV:
				dst.Descriptor.ShaderRegister = src.Descriptor.ShaderRegister;
				dst.Descriptor.RegisterSpace = src.Descriptor.RegisterSpace;
				dst.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
				break;
			default:
				break;
			}
		}

		D3D12_ROOT_SIGNATURE_FLAGS flags = desc.Flags;
		if ((flags & D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE) == 0)
		{
			flags |=
				D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
				D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
				D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
				D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS |
				D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS;
		}

		CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC versionedDesc;
		versionedDesc.Init_1_1(desc.NumParameters, rootParams.data(), desc.NumStaticSamplers, desc.pStaticSamplers, flags);
		hr = D3DX12SerializeVersionedRootSignature(&versionedDesc, D3D_ROOT_SIGNATURE_VERSION_1_1, &pSigBlob, &pErrorBlob);
	}
	else
	{
		hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &pSigBlob, &pErrorBlob);
	}

	if (FAILED(hr))
	{
		std::string msg = convertBlobToString(pErrorBlob.Get());
		OutputDebugStringA(msg.c_str());

		return nullptr;
	}
	ComPtr<ID3D12RootSignature> pRootSig;
	hr = pDevice->CreateRootSignature(0, pSigBlob->GetBufferPointer(), pSigBlob->GetBufferSize(), IID_PPV_ARGS(&pRootSig));

	return pRootSig;
}

struct HitProgram
{
	HitProgram(LPCWSTR ahsExport, LPCWSTR chsExport, const std::wstring& name) : exportName(name)
	{
		desc = {};
		desc.AnyHitShaderImport = ahsExport;
		desc.ClosestHitShaderImport = chsExport;
		desc.HitGroupExport = exportName.c_str();

		subObject.Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
		subObject.pDesc = &desc;
	}

	std::wstring exportName;
	D3D12_HIT_GROUP_DESC desc;
	D3D12_STATE_SUBOBJECT subObject;
};

struct ExportAssociation
{
	ExportAssociation(const WCHAR* exportNames[], uint32_t exportCount, const D3D12_STATE_SUBOBJECT* pSubobjectToAssociate)
	{
		association.NumExports = exportCount;
		association.pExports = exportNames;
		association.pSubobjectToAssociate = pSubobjectToAssociate;

		subobject.Type = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
		subobject.pDesc = &association;
	}

	D3D12_STATE_SUBOBJECT subobject = {};
	D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION association = {};
};

struct LocalRootSignature
{
	LocalRootSignature(ComPtr<ID3D12Device5> pDevice, const D3D12_ROOT_SIGNATURE_DESC& desc)
	{
		pRootSig = CreateRootSignature(pDevice, desc);
		pInterface = pRootSig.Get();
		subobject.pDesc = &pInterface;
		subobject.Type = D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE;
	}
	ComPtr<ID3D12RootSignature> pRootSig;
	ID3D12RootSignature* pInterface = nullptr;
	D3D12_STATE_SUBOBJECT subobject = {};
};

struct GlobalRootSignature
{
	GlobalRootSignature(ComPtr<ID3D12Device5> pDevice, const D3D12_ROOT_SIGNATURE_DESC& desc)
	{
		pRootSig = CreateRootSignature(pDevice, desc);
		pInterface = pRootSig.Get();
		subobject.pDesc = &pInterface;
		subobject.Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
	}
	ComPtr<ID3D12RootSignature> pRootSig;
	ID3D12RootSignature* pInterface = nullptr;
	D3D12_STATE_SUBOBJECT subobject = {};
};

struct ShaderConfig
{
	ShaderConfig(uint32_t maxAttributeSizeInBytes, uint32_t maxPayloadSizeInBytes)
	{
		shaderConfig.MaxAttributeSizeInBytes = maxAttributeSizeInBytes;
		shaderConfig.MaxPayloadSizeInBytes = maxPayloadSizeInBytes;

		subobject.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
		subobject.pDesc = &shaderConfig;
	}

	D3D12_RAYTRACING_SHADER_CONFIG shaderConfig = {};
	D3D12_STATE_SUBOBJECT subobject = {};
};

struct PipelineConfig
{
	PipelineConfig(uint32_t maxTraceRecursionDepth, bool bUseConfig1)
	{
		if (bUseConfig1)
		{
			config1.MaxTraceRecursionDepth = maxTraceRecursionDepth;
			config1.Flags = D3D12_RAYTRACING_PIPELINE_FLAG_NONE;
			subobject.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1;
			subobject.pDesc = &config1;
		}
		else
		{
			config.MaxTraceRecursionDepth = maxTraceRecursionDepth;
			subobject.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
			subobject.pDesc = &config;
		}
	}

	D3D12_RAYTRACING_PIPELINE_CONFIG config = {};
	D3D12_RAYTRACING_PIPELINE_CONFIG1 config1 = {};
	D3D12_STATE_SUBOBJECT subobject = {};
};

void D3D12RTPipelineStateObject::SetNumInstances(uint32_t numInstances)
{
	if (NumInstance != numInstances)
	{
		ShaderTable.Reset();
		ShaderTableUpload.Reset();
		ShaderTableEntrySize = 0;
		ShaderTableSize = 0;
		ShaderTableState = D3D12_RESOURCE_STATE_COPY_DEST;
		MarkHitProgramBindingCacheDirty();
	}
	NumInstance = numInstances;
}

void D3D12RTPipelineStateObject::Configure(uint32_t maxRecursion, uint32_t maxPayloadSizeInBytes, uint32_t maxAttributeSizeInBytes)
{
	MaxRecursion = maxRecursion;
	MaxPayloadSizeInBytes = maxPayloadSizeInBytes;
	MaxAttributeSizeInBytes = maxAttributeSizeInBytes;
}

void D3D12RTPipelineStateObject::AddHitGroup(const string& name, const string& chs, const string& ahs)
{
	HitGroupInfo info;
	info.name = StringToWString(name);
	info.chs = StringToWString(chs);
	info.ahs = StringToWString(ahs);

	VecHitGroup.push_back(info);
}

void D3D12RTPipelineStateObject::AddShader(const string& shader, RTPipelineStateObject::ShaderType shaderType)
{
	ShaderBinding[shader].ShaderName = StringToWString(shader);;
	ShaderBinding[shader].Type = static_cast<D3D12RTPipelineStateObject::ShaderType>(shaderType);
}

void D3D12RTPipelineStateObject::BindUAV(const string& shader, const string& name, uint32_t baseRegister)
{
	if (shader == "global")
	{
		BindingData binding;
		binding.name = name;
		binding.Type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;

		binding.BaseRegister = baseRegister;
		binding.Schema = MakeLegacyRHIBindingDesc(name, RHIDescriptorKind::UAV, RHIResourceKind::Unknown, baseRegister, 1);

		GlobalBinding.push_back(binding);
	}
	else
	{
		auto& bindingInfo = ShaderBinding[shader];

		BindingData binding;
		binding.name = name;
		binding.Type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;

		binding.BaseRegister = baseRegister;
		binding.Schema = MakeLegacyRHIBindingDesc(name, RHIDescriptorKind::UAV, RHIResourceKind::Unknown, baseRegister, 1);

		bindingInfo.Binding.push_back(binding);
	}
}

void D3D12RTPipelineStateObject::BindUAV(const string& shader, const RHIBindingDesc& binding)
{
	BindUAV(shader, binding.Name, binding.RegisterIndex);
	if (shader == "global")
		GlobalBinding.back().Schema = binding;
	else
		ShaderBinding[shader].Binding.back().Schema = binding;
}

void D3D12RTPipelineStateObject::BindSRV(const string& shader, const string& name, uint32_t baseRegister)
{
	if (shader == "global")
	{
		BindingData binding;
		binding.name = name;
		binding.Type = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;

		binding.BaseRegister = baseRegister;
		binding.Schema = MakeLegacyRHIBindingDesc(name, RHIDescriptorKind::SRV, RHIResourceKind::Unknown, baseRegister, 1);

		GlobalBinding.push_back(binding);
	}
	else
	{
		auto& bindingInfo = ShaderBinding[shader];

		BindingData binding;
		binding.name = name;
		binding.Type = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;

		binding.BaseRegister = baseRegister;
		binding.Schema = MakeLegacyRHIBindingDesc(name, RHIDescriptorKind::SRV, RHIResourceKind::Unknown, baseRegister, 1);

		bindingInfo.Binding.push_back(binding);
	}
}

void D3D12RTPipelineStateObject::BindSRV(const string& shader, const RHIBindingDesc& binding)
{
	BindSRV(shader, binding.Name, binding.RegisterIndex);
	if (shader == "global")
		GlobalBinding.back().Schema = binding;
	else
		ShaderBinding[shader].Binding.back().Schema = binding;
}



void D3D12RTPipelineStateObject::BindSampler(const string& shader, const string& name, uint32_t baseRegister)
{
	if (shader == "global")
	{
		BindingData binding;
		binding.name = name;
		binding.Type = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;

		binding.BaseRegister = baseRegister;
		binding.Schema = MakeLegacyRHIBindingDesc(name, RHIDescriptorKind::Sampler, RHIResourceKind::Sampler, baseRegister, 1);

		GlobalBinding.push_back(binding);
	}
	else
	{
		auto& bindingInfo = ShaderBinding[shader];

		BindingData binding;
		binding.name = name;
		binding.Type = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;

		binding.BaseRegister = baseRegister;
		binding.Schema = MakeLegacyRHIBindingDesc(name, RHIDescriptorKind::Sampler, RHIResourceKind::Sampler, baseRegister, 1);

		bindingInfo.Binding.push_back(binding);
	}
}

void D3D12RTPipelineStateObject::BindSampler(const string& shader, const RHIBindingDesc& binding)
{
	BindSampler(shader, binding.Name, binding.RegisterIndex);
	if (shader == "global")
		GlobalBinding.back().Schema = binding;
	else
		ShaderBinding[shader].Binding.back().Schema = binding;
}

void D3D12RTPipelineStateObject::BindCBV(const string& shader, const string& name, uint32_t baseRegister, uint32_t size, uint32_t numInstance)
{
	if (shader == "global")
	{
		BindingData binding;
		binding.name = name;
		binding.Type = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;

		binding.BaseRegister = baseRegister;
		binding.sourceSize = size;
		binding.cbSize = AlignConstantBufferSize(binding.sourceSize);
		binding.Schema = MakeLegacyRHIBindingDesc(name, RHIDescriptorKind::CBV, RHIResourceKind::ConstantBuffer, baseRegister, 1, size);

		GlobalBinding.push_back(binding);
	}
	else
	{
		auto& bindingInfo = ShaderBinding[shader];

		BindingData binding;
		binding.name = name;
		binding.Type = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;

		binding.BaseRegister = baseRegister;
		binding.sourceSize = size;
		binding.cbSize = AlignConstantBufferSize(binding.sourceSize);
		binding.Schema = MakeLegacyRHIBindingDesc(name, RHIDescriptorKind::CBV, RHIResourceKind::ConstantBuffer, baseRegister, 1, size);

		bindingInfo.Binding.push_back(binding);
	}
}

void D3D12RTPipelineStateObject::BindCBV(const string& shader, const RHIBindingDesc& binding)
{
	BindCBV(shader, binding.Name, binding.RegisterIndex, binding.SizeInBytes, binding.NumInstances);
	if (shader == "global")
		GlobalBinding.back().Schema = binding;
	else
		ShaderBinding[shader].Binding.back().Schema = binding;
}

void D3D12RTPipelineStateObject::SetShaderDefine(const string& name, const string& value)
{
	ShaderDefines.emplace_back(name, value);
}

void D3D12RTPipelineStateObject::SetShaderLibraryTarget(const string& target)
{
	if (!target.empty())
		ShaderLibraryTarget = target;
}

bool D3D12RTPipelineStateObject::IsHitProgramBindingCacheValid(uint32_t numInstances, uint64_t signature) const
{
	DX12Backend* owner = Owner;
	if (!owner)
		return false;
	const uint32_t frameIndex = owner->CurrentFrameIndex;
	return frameIndex < ShaderTableFrameValid.size() &&
		frameIndex < ShaderTableFrameInstanceCount.size() &&
		frameIndex < ShaderTableFrameSignature.size() &&
		ShaderTableFrameValid[frameIndex] &&
		ShaderTableFrameInstanceCount[frameIndex] == numInstances &&
		ShaderTableFrameSignature[frameIndex] == signature;
}

void D3D12RTPipelineStateObject::MarkHitProgramBindingCacheDirty()
{
	HitProgramBindingPendingValid = false;
	HitProgramBindingPendingInstanceCount = 0;
	HitProgramBindingPendingSignature = 0;
	DX12Backend* owner = Owner;
	if (owner)
	{
		const uint32_t frameIndex = owner->CurrentFrameIndex;
		if (frameIndex < ShaderTableFrameValid.size())
			ShaderTableFrameValid[frameIndex] = 0;
	}
}

void D3D12RTPipelineStateObject::MarkHitProgramBindingCacheValid(uint32_t numInstances, uint64_t signature)
{
	HitProgramBindingPendingValid = true;
	HitProgramBindingPendingInstanceCount = numInstances;
	HitProgramBindingPendingSignature = signature;
}

void D3D12RTPipelineStateObject::BeginShaderTable()
{
}

void D3D12RTPipelineStateObject::SetGlobalBinding(CommandList* CommandList)
{
	DX12Backend* owner = Owner;
	assert(owner);
	CommandList = ResolveCommandList(owner, CommandList);
	assert(CommandList);
	UINT RPI = 0;
	for (auto& bi : GlobalBinding)
	{
		owner->SetComputeRootDescriptorTableIfNeeded(CommandList->CmdList.Get(), RPI++, bi.GPUHandle);
	}
}

void D3D12RTPipelineStateObject::EndShaderTable()
{
	DX12Backend* owner = Owner;
	assert(owner);
	if (ShaderTableFrameValid.size() != owner->NumFrame)
		ShaderTableFrameValid.assign(owner->NumFrame, 0);
	if (ShaderTableFrameInstanceCount.size() != owner->NumFrame)
		ShaderTableFrameInstanceCount.assign(owner->NumFrame, 0);
	if (ShaderTableFrameSignature.size() != owner->NumFrame)
		ShaderTableFrameSignature.assign(owner->NumFrame, 0);
	const uint32_t frameIndex = owner->CurrentFrameIndex;
	if (!HitProgramBindingPendingValid &&
		ShaderTable != nullptr &&
		frameIndex < ShaderTableFrameValid.size() &&
		ShaderTableFrameValid[frameIndex])
	{
		return;
	}

	if (ShaderTable == nullptr)
	{
		// find biggiest binding size
		ShaderTableEntrySize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
		for (auto& sb : ShaderBinding)
		{
			UINT EntrySize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;

			BindingInfo& bindingInfo = sb.second;

			D3D12_ROOT_PARAMETER RootParam = {};
			for (auto& bindingData : bindingInfo.Binding)
			{
				EntrySize += 8;
			}

			if (EntrySize > ShaderTableEntrySize)
				ShaderTableEntrySize = EntrySize;
		}
		ShaderTableEntrySize = align_to(D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT, ShaderTableEntrySize);


		UINT NumShaderTableEntry = 0;
		for (auto& it : ShaderBinding)
		{
			BindingInfo& bi = it.second;
			if (bi.Type == RAYGEN || bi.Type == MISS || bi.Type == ANYHIT)
				NumShaderTableEntry++;

		}
		NumShaderTableEntry += GetHitShaderRecordCount();

		ShaderTableSize = ShaderTableEntrySize * NumShaderTableEntry;

		// Allocate the shader table in DEFAULT memory. The CPU writes records
		// into a staging upload buffer, then copies the current frame segment
		// before DispatchRays so per-hit shader-record fetches do not hit sysmem.
		{
			D3D12_RESOURCE_DESC bufDesc = {};
			bufDesc.Alignment = 0;
			bufDesc.DepthOrArraySize = 1;
			bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			bufDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
			bufDesc.Format = DXGI_FORMAT_UNKNOWN;
			bufDesc.Height = 1;
			bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			bufDesc.MipLevels = 1;
			bufDesc.SampleDesc.Count = 1;
			bufDesc.SampleDesc.Quality = 0;
			bufDesc.Width = ShaderTableSize * owner->NumFrame;

			const D3D12_HEAP_PROPERTIES kDefaultHeapProps =
			{
				D3D12_HEAP_TYPE_DEFAULT,
				D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
				D3D12_MEMORY_POOL_UNKNOWN,
				0,
				0,
			};
			const D3D12_HEAP_PROPERTIES kUploadHeapProps =
			{
				D3D12_HEAP_TYPE_UPLOAD,
				D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
				D3D12_MEMORY_POOL_UNKNOWN,
				0,
				0,
			};

			const HRESULT defaultHr = owner->Device->CreateCommittedResource(&kDefaultHeapProps, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&ShaderTable));
			const HRESULT uploadHr = owner->Device->CreateCommittedResource(&kUploadHeapProps, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ShaderTableUpload));
			if (FAILED(defaultHr) || FAILED(uploadHr) || !ShaderTable || !ShaderTableUpload)
			{
				AppendCpuRuntimeTrace(
					L"[DX12RT] shader table allocation failed defaultHr=" + FormatHexHRESULT(defaultHr) +
					L", uploadHr=" + FormatHexHRESULT(uploadHr));
				ShaderTable.Reset();
				ShaderTableUpload.Reset();
				return;
			}
			ShaderTableState = D3D12_RESOURCE_STATE_COPY_DEST;
			NAME_D3D12_OBJECT(ShaderTable);
			NAME_D3D12_OBJECT(ShaderTableUpload);
			AppendCpuRuntimeTrace(
				L"[DX12RT] shader table heap=DEFAULT, staging=UPLOAD, frameBytes=" +
				std::to_wstring(ShaderTableSize) +
				L", totalBytes=" + std::to_wstring(static_cast<UINT64>(bufDesc.Width)));
		}
	}
	

	// raygen : simple, it is just the begin of table
	// miss : raygen + miss index * EntrySize
	// hit : raygen + miss(N) + instanceIndex
	uint8_t* pData = nullptr;
	HRESULT hr = ShaderTableUpload->Map(0, nullptr, (void**)&pData);
	if (FAILED(hr) || !pData)
	{
		AppendCpuRuntimeTrace(L"[DX12RT] shader table upload Map failed hr=" + FormatHexHRESULT(hr));
		return;
	}

	pData += ShaderTableSize * owner->CurrentFrameIndex;

	ComPtr<ID3D12StateObjectProperties> RtsoProps;
	RTPipelineState->QueryInterface(IID_PPV_ARGS(&RtsoProps));

	uint8_t* pDataThis = pData;

	// calculate shader table offset for each shader
	// raygen
	int LastIndex = 0;
	for (auto& sb : ShaderBinding)
	{
		BindingInfo& bindingInfo = sb.second;
		if (bindingInfo.Type == ShaderType::RAYGEN)
		{
			memcpy(pDataThis, RtsoProps->GetShaderIdentifier(bindingInfo.ShaderName.c_str()), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
			pDataThis += D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;

			for (auto& bd : bindingInfo.Binding)
			{
				*(UINT64*)(pDataThis) = bd.GPUHandle.ptr;

				pDataThis += sizeof(UINT64);
			}
		}
	}
	LastIndex++;

	// miss
	for (auto& sb : ShaderBinding)
	{
		pDataThis = pData + LastIndex * ShaderTableEntrySize;

		BindingInfo& bindingInfo = sb.second;
		if (bindingInfo.Type == ShaderType::MISS)
		{
			memcpy(pDataThis, RtsoProps->GetShaderIdentifier(bindingInfo.ShaderName.c_str()), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

			LastIndex++;// multiple miss shader is available.

		}
	}

	// hit program
	if (UsesSharedHitRecords())
	{
		for (int iHitGroup = 0; iHitGroup < VecHitGroup.size(); iHitGroup++)
		{
			pDataThis = pData + LastIndex * ShaderTableEntrySize;

			map<UINT, HitProgramData>& HitProgram = VecHitGroup[iHitGroup].HitProgramBinding;
			auto& HitProgramInfo = HitProgram[0];

			memcpy(pDataThis, RtsoProps->GetShaderIdentifier(VecHitGroup[iHitGroup].name.c_str()), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

			pDataThis += D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;

			for (auto& bd : HitProgramInfo.VecData)
			{
				*(UINT64*)(pDataThis) = bd.ptr;

				pDataThis += sizeof(UINT64);
			}
			LastIndex++;

		}
	}
	else
	{
		for (int InstanceIndex = 0; InstanceIndex < NumInstance; InstanceIndex++)
		{
			for (int iHitGroup = 0; iHitGroup < VecHitGroup.size(); iHitGroup++)
			{
				pDataThis = pData + LastIndex * ShaderTableEntrySize;

				map<UINT, HitProgramData>& HitProgram = VecHitGroup[iHitGroup].HitProgramBinding;
				auto& HitProgramInfo = HitProgram[InstanceIndex];

				memcpy(pDataThis, RtsoProps->GetShaderIdentifier(VecHitGroup[iHitGroup].name.c_str()), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

				pDataThis += D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;

				for (auto& bd : HitProgramInfo.VecData)
				{
					*(UINT64*)(pDataThis) = bd.ptr;

					pDataThis += sizeof(UINT64);
				}
				LastIndex++;
			}
		}
	}


	ShaderTableUpload->Unmap(0, nullptr);
	CommandList* commandList = ResolveCommandList(owner, nullptr);
	if (commandList && ShaderTable && ShaderTableUpload)
	{
		if (ShaderTableState != D3D12_RESOURCE_STATE_COPY_DEST)
		{
			commandList->CmdList->ResourceBarrier(
				1,
				&CD3DX12_RESOURCE_BARRIER::Transition(
					ShaderTable.Get(),
					ShaderTableState,
					D3D12_RESOURCE_STATE_COPY_DEST));
			ShaderTableState = D3D12_RESOURCE_STATE_COPY_DEST;
		}

		const UINT64 frameOffset = static_cast<UINT64>(ShaderTableSize) * owner->CurrentFrameIndex;
		commandList->CmdList->CopyBufferRegion(
			ShaderTable.Get(),
			frameOffset,
			ShaderTableUpload.Get(),
			frameOffset,
			ShaderTableSize);
		commandList->CmdList->ResourceBarrier(
			1,
			&CD3DX12_RESOURCE_BARRIER::Transition(
				ShaderTable.Get(),
				D3D12_RESOURCE_STATE_COPY_DEST,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
		ShaderTableState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	}
	if (HitProgramBindingPendingValid &&
		frameIndex < ShaderTableFrameValid.size() &&
		frameIndex < ShaderTableFrameInstanceCount.size() &&
		frameIndex < ShaderTableFrameSignature.size())
	{
		ShaderTableFrameValid[frameIndex] = 1;
		ShaderTableFrameInstanceCount[frameIndex] = HitProgramBindingPendingInstanceCount;
		ShaderTableFrameSignature[frameIndex] = HitProgramBindingPendingSignature;
	}
	else if (frameIndex < ShaderTableFrameValid.size())
	{
		ShaderTableFrameValid[frameIndex] = 0;
	}
	HitProgramBindingPendingValid = false;
}

void D3D12RTPipelineStateObject::SetUAVHandle(const string& shader, const string& bindingName, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE uavHandle, INT instanceIndex /*= -1*/)
{
	// each bindings of raygen/miss shader is unique to shader name.
	if (instanceIndex == -1) // raygen, miss
	{
		if (shader == "global")
		{
			for (auto& bd : GlobalBinding)
			{
				if (bd.name == bindingName)
				{
					bd.CPUHandle = cpuHandle;
					bd.GPUHandle = uavHandle;
				}
			}
		}
		else
		{
			BindingInfo& bi = ShaderBinding[shader];
			for (auto& bd : bi.Binding)
			{
				if (bd.name == bindingName)
				{
					bd.CPUHandle = cpuHandle;
					bd.GPUHandle = uavHandle;

				}
			}
		}
	}
	//else // hitprogram : There can be multiple hitprogram entry with same shader name, so we need another data structure. (HitProgramBinding)
	//{
	//	// SetXXX should be called according to the binding order, because it is push_backed to vector.
	//	HitProgramBindingCHS[instanceIndex].HitGroupName = MapHitGroup[shader].name;
	//	HitProgramBindingCHS[instanceIndex].VecData.push_back(uavHandle);
	//}
}

void D3D12RTPipelineStateObject::SetSRVHandle(const string& shader, const string& bindingName, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE srvHandle, INT instanceIndex /*= -1*/)
{
	// each bindings of raygen/miss shader is unique to shader name.
	if (instanceIndex == -1) // raygen, miss
	{
		if (shader == "global")
		{
			for (auto& bd : GlobalBinding)
			{
				if (bd.name == bindingName)
				{
					bd.CPUHandle = cpuHandle;
					bd.GPUHandle = srvHandle;
				}
			}
		}
		else
		{
			BindingInfo& bi = ShaderBinding[shader];
			for (auto& bd : bi.Binding)
			{
				if (bd.name == bindingName)
				{
					bd.CPUHandle = cpuHandle;
					bd.GPUHandle = srvHandle;
				}
			}
		}
	}
	//else // hitprogram : There can be multiple hitprogram entry with same shader name, so we need another data structure. (HitProgramBinding)
	//{
	//	// SetXXX should be called according to the binding order, because it is push_backed to vector.
	//	HitProgramBindingCHS[instanceIndex].HitGroupName = MapHitGroup[shader].name;
	//	HitProgramBindingCHS[instanceIndex].VecData.push_back(srvHandle);
	//}
}

void D3D12RTPipelineStateObject::ResetHitProgram(uint32_t instanceIndex)
{
	for (auto& HG : VecHitGroup)
	{
		HG.HitProgramBinding[instanceIndex].VecData.clear();
	}
}

void D3D12RTPipelineStateObject::StartHitProgram(const string& HitGroup, uint32_t instanceIndex)
{
	map<UINT, HitProgramData>* HitProgram =nullptr;
	for (auto& HG : VecHitGroup)
	{
		if (HG.name == StringToWString(HitGroup))
			HitProgram = &HG.HitProgramBinding;
	}
	//(*HitProgram)[instanceIndex].HitGroupName = StringToWString(HitGroup);
	(*HitProgram)[instanceIndex].VecData.clear();
}

UINT D3D12RTPipelineStateObject::GetHitShaderRecordCount() const
{
	if (VecHitGroup.empty())
		return 0;
	return static_cast<UINT>(VecHitGroup.size()) * (UsesSharedHitRecords() ? 1u : NumInstance);
}

void D3D12RTPipelineStateObject::SetSampler(const string& shader, const string& bindingName, Sampler* sampler, INT instanceIndex /*= -1*/)
{
	// each bindings of raygen/miss shader is unique to shader name.
	if (instanceIndex == -1) // raygen, miss
	{
		if (shader == "global")
		{
			for (auto& bd : GlobalBinding)
			{
				if (bd.name == bindingName)
				{
					bd.CPUHandle = sampler->CpuHandle;
					bd.GPUHandle = sampler->GpuHandle;
				}
			}
		}
		else
		{
			BindingInfo& bi = ShaderBinding[shader];
			for (auto& bd : bi.Binding)
			{
				if (bd.name == bindingName)
				{
					bd.CPUHandle = sampler->CpuHandle;
					bd.GPUHandle = sampler->GpuHandle;
				}
			}
		}
	}
	//else // hitprogram : There can be multiple hitprogram entry with same shader name, so we need another data structure. (HitProgramBinding)
	//{
	//	// SetXXX should be called according to the binding order, because it is push_backed to vector.
	//	HitProgramBindingCHS[instanceIndex].HitGroupName = MapHitGroup[shader].name;
	//	HitProgramBindingCHS[instanceIndex].VecData.push_back(sampler->GpuHandle);
	//}
}

void D3D12RTPipelineStateObject::SetCBVValue(const string& shader, const string& bindingName, void* pData, INT instanceIndex /*= -1*/)
{
	DX12Backend* owner = Owner;
	assert(owner);
	// each bindings of raygen/miss shader is unique to shader name.
	if (instanceIndex == -1) // raygen, miss
	{
		if (shader == "global")
		{
			bool bFound = false;
			for (auto& bd : GlobalBinding)
			{
				if (bd.name == bindingName)
				{
					auto Alloc = owner->GlobalCBRing->AllocGPUMemory(bd.cbSize);
					UINT64 GPUAddr = std::get<0>(Alloc);
					UINT8* pMapped = std::get<1>(Alloc);

					CopyConstantBufferData(pMapped, bd.cbSize, pData, bd.sourceSize);

					D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle;
					D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle;

					// ring is advanced at the begining of frame. so descriptors from multiple frame is not overlapped.
					owner->GlobalDHRing->AllocDescriptor(CpuHandle, GpuHandle);

					D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
					cbvDesc.BufferLocation = GPUAddr;
					cbvDesc.SizeInBytes = bd.cbSize;
					owner->Device->CreateConstantBufferView(&cbvDesc, CpuHandle);

					bd.CPUHandle = CpuHandle;
					bd.GPUHandle = GpuHandle;
				
					bFound = true;
				}
			}
			assert(bFound == true);
		}
		else
		{
			bool bFound = false;

			BindingInfo& bi = ShaderBinding[shader];
			for (auto& bd : bi.Binding)
			{
				if (bd.name == bindingName)
				{
					auto Alloc = owner->GlobalCBRing->AllocGPUMemory(bd.cbSize);
					UINT64 GPUAddr = std::get<0>(Alloc);
					UINT8* pMapped = std::get<1>(Alloc);

					CopyConstantBufferData(pMapped, bd.cbSize, pData, bd.sourceSize);

					D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle;
					D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle;

					// ring is advanced at the begining of frame. so descriptors from multiple frame is not overlapped.
					owner->GlobalDHRing->AllocDescriptor(CpuHandle, GpuHandle);

					D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
					cbvDesc.BufferLocation = GPUAddr;
					cbvDesc.SizeInBytes = bd.cbSize;
					owner->Device->CreateConstantBufferView(&cbvDesc, CpuHandle);

					bd.CPUHandle = CpuHandle;
					bd.GPUHandle = GpuHandle;
				
					bFound = true;
				}
			}
			assert(bFound == true);
		}
	}
}


bool D3D12RTPipelineStateObject::InitRS(const string& ShaderFile)
{
	DX12Backend* owner = Owner;
	assert(owner);
	const auto totalStart = std::chrono::steady_clock::now();
	auto stepStart = totalStart;
	const std::wstring shaderFileWide = ToWide(ShaderFile);
	auto traceStep = [&](const wchar_t* label)
	{
		const auto now = std::chrono::steady_clock::now();
		AppendCpuRuntimeTrace(
			L"[StartupTiming][DX12RTInitRS] shader=\"" + shaderFileWide +
			L"\", step=\"" + std::wstring(label) +
			L"\", stepMs=" + FormatDx12InitMilliseconds(ElapsedDx12InitMilliseconds(stepStart, now)) +
			L", totalMs=" + FormatDx12InitMilliseconds(ElapsedDx12InitMilliseconds(totalStart, now)));
		stepStart = now;
	};
	AppendCpuRuntimeTrace(
		L"[StartupTiming][DX12RTInitRS] begin shader=\"" + shaderFileWide +
		L"\", shaderBindings=" + std::to_wstring(static_cast<uint32_t>(ShaderBinding.size())) +
		L", globalBindings=" + std::to_wstring(static_cast<uint32_t>(GlobalBinding.size())) +
		L", hitGroups=" + std::to_wstring(static_cast<uint32_t>(VecHitGroup.size())) +
		L", instances=" + std::to_wstring(NumInstance));
	vector<D3D12_STATE_SUBOBJECT> subobjects;

	uint32_t localRootBindingCount = 0;
	for (const auto& sb : ShaderBinding)
	{
		if (!sb.second.Binding.empty())
			++localRootBindingCount;
	}

	// dxil + Hitgroup count + non-empty local RS/export + shaderconfig + export + pipelineconfig + global RS
	int numSubobjects = 1 + VecHitGroup.size() + localRootBindingCount * 2 + 2 + 1 + 1;
	subobjects.resize(numSubobjects);
	traceStep(L"Allocate subobjects");

	uint32_t index = 0;

	// dxil lib
	std::filesystem::path shaderPath(ShaderFile);
	if (shaderPath.is_relative())
		shaderPath = RuntimePaths::SourceDirectory() / shaderPath;
	wstring wShaderFile = shaderPath.wstring();
	ComPtr<ID3DBlob> pDxilLib = compileShaderLibrary(owner, wShaderFile.c_str(), ToWide(ShaderLibraryTarget).c_str(), ShaderDefines);
	if (!pDxilLib)
	{
		traceStep(L"Compile DXIL library failed");
		return false;
	}
	traceStep((L"Compile DXIL library " + ToWide(ShaderLibraryTarget)).c_str());

	vector<const WCHAR*> entryPoints;
	entryPoints.reserve(ShaderBinding.size());
	for (auto& sb : ShaderBinding)
	{
		entryPoints.push_back(sb.second.ShaderName.c_str());
	}

	DxilLibrary dxilLib = DxilLibrary(pDxilLib, entryPoints.data(), entryPoints.size());
	subobjects[index++] = dxilLib.stateSubobject; // 0 Library
	traceStep(L"Build DXIL library subobject");

	// hit group
	vector<D3D12_HIT_GROUP_DESC> vecHitDesc;
	vecHitDesc.reserve(VecHitGroup.size());
	for (auto&hi : VecHitGroup)
	{
		const WCHAR* ahs = nullptr;
		const WCHAR* chs = nullptr;
		if (hi.chs.length() > 0)
			chs = hi.chs.c_str();
		if (hi.ahs.length() > 0)
			ahs = hi.ahs.c_str();


		//HitProgram hitProgram(ahs, chs, hi.name.c_str());
		D3D12_HIT_GROUP_DESC HitGroupDesc = {};
		HitGroupDesc.HitGroupExport = hi.name.c_str();
		HitGroupDesc.ClosestHitShaderImport = chs;
		HitGroupDesc.AnyHitShaderImport = ahs;
		HitGroupDesc.IntersectionShaderImport = nullptr;
		vecHitDesc.push_back(HitGroupDesc);

		D3D12_STATE_SUBOBJECT subObject;
		subObject.Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
		subObject.pDesc = &vecHitDesc.back();

		subobjects[index++] = subObject; // 1 Hit Group
	}
	traceStep(L"Build hit group subobjects");

	// root signature
	BindingInfo* pBI = nullptr;
	vector< D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION> vecAssociation;
	vecAssociation.reserve(ShaderBinding.size());
	for (auto& sb : ShaderBinding)
	{
		BindingInfo& bindingInfo = sb.second;
		if (bindingInfo.Binding.empty())
			continue;
		pBI = &bindingInfo;
		
		D3D12_ROOT_SIGNATURE_DESC Desc = {};
		Desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;

		vector<D3D12_ROOT_PARAMETER> rootParamVec;
		vector<D3D12_DESCRIPTOR_RANGE> Ranges;
		Ranges.resize(bindingInfo.Binding.size());

		int i = 0;

		if (bindingInfo.Binding.size() > 0)
		{
			// create root signature
			
			for (auto& bindingData : bindingInfo.Binding)
			{
				D3D12_DESCRIPTOR_RANGE& Range = Ranges[i++];;
				Range.RangeType = bindingData.Type;
				Range.BaseShaderRegister = bindingData.BaseRegister;
				Range.NumDescriptors = ToD3D12DescriptorCount(bindingData.Schema);
				Range.RegisterSpace = ToD3D12RegisterSpace(bindingData.Schema);
				Range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
				//Ranges.push_back(Range);


				D3D12_ROOT_PARAMETER RootParam = {};
				RootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
				RootParam.DescriptorTable.NumDescriptorRanges = 1;// Ranges.size();
				RootParam.DescriptorTable.pDescriptorRanges = &Range;// Ranges.data();
				RootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

				rootParamVec.push_back(RootParam);
			}

			Desc.NumParameters = rootParamVec.size();
			Desc.pParameters = rootParamVec.data();
		}

		bindingInfo.RS = CreateRootSignature(owner->Device, Desc);
		const std::wstring localRootName =
			L"RTLocalRootSignature: " + ShaderDebugStem(ShaderFile) + L"." + bindingInfo.ShaderName;
		SetName(bindingInfo.RS.Get(), localRootName.c_str());

		bindingInfo.pInterface = bindingInfo.RS.Get();
		bindingInfo.subobject.pDesc = &bindingInfo.pInterface;
		bindingInfo.subobject.Type = D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE;
		subobjects[index] = bindingInfo.subobject;
		uint32_t RSIndex = index++;

		// export association
		bindingInfo.ExportName = { bindingInfo.ShaderName.c_str() };

		D3D12_STATE_SUBOBJECT subobjectAssociation = {};
		D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION association = {};
		association.NumExports = 1;
		association.pExports = bindingInfo.ExportName.data();
		association.pSubobjectToAssociate = &subobjects[RSIndex];
		vecAssociation.push_back(association);
		subobjectAssociation.Type = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
		subobjectAssociation.pDesc = &vecAssociation.back();

		subobjects[index++] = subobjectAssociation;
	}
	traceStep(L"Create local root signatures");

	// shader config
	ShaderConfig shaderConfig(MaxAttributeSizeInBytes, MaxPayloadSizeInBytes);
	D3D12_STATE_SUBOBJECT* shaderConfigSubObject = &subobjects[index];
	subobjects[index++] = shaderConfig.subobject;

	// shaderconfig export association
	vector<const WCHAR*> vecShaderExports;
	vecShaderExports.reserve(ShaderBinding.size());
	for (auto& sb : ShaderBinding)
	{
		vecShaderExports.push_back(sb.second.ShaderName.c_str());
	}

	ExportAssociation configAssociation(vecShaderExports.data(), vecShaderExports.size(), shaderConfigSubObject);
	subobjects[index++] = configAssociation.subobject;


	// pipeline config
	const bool bUsePipelineConfig1 = ShaderLibraryTarget >= "lib_6_9";
	PipelineConfig config(MaxRecursion, bUsePipelineConfig1);
	subobjects[index++] = config.subobject;

	// global root signature
	D3D12_STATE_SUBOBJECT subobjectGlobalRS = {};
	ID3D12RootSignature* pInterfaceGlobalRS = nullptr;
	D3D12_ROOT_SIGNATURE_DESC GlobalRSDesc = {};
		
	D3D12_ROOT_SIGNATURE_DESC Desc = {};
	Desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

	vector<D3D12_ROOT_PARAMETER> rootParamVec;
	vector<D3D12_DESCRIPTOR_RANGE> Ranges;
	Ranges.resize(GlobalBinding.size());

	int i = 0;

	// create root signature

	for (auto& bindingData : GlobalBinding)
	{
		D3D12_DESCRIPTOR_RANGE& Range = Ranges[i++];;
		Range.RangeType = bindingData.Type;
		Range.BaseShaderRegister = bindingData.BaseRegister;
		Range.NumDescriptors = ToD3D12DescriptorCount(bindingData.Schema);
		Range.RegisterSpace = ToD3D12RegisterSpace(bindingData.Schema);
		Range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
		//Ranges.push_back(Range);


		D3D12_ROOT_PARAMETER RootParam = {};
		RootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		RootParam.DescriptorTable.NumDescriptorRanges = 1;// Ranges.size();
		RootParam.DescriptorTable.pDescriptorRanges = &Range;// Ranges.data();
		RootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		rootParamVec.push_back(RootParam);
	}

	Desc.NumParameters = rootParamVec.size();
	Desc.pParameters = rootParamVec.data();

	GlobalRS = CreateRootSignature(owner->Device, Desc);
	const std::wstring globalRootName = L"RTGlobalRootSignature: " + ShaderDebugStem(ShaderFile);
	SetName(GlobalRS.Get(), globalRootName.c_str());

	pInterfaceGlobalRS = GlobalRS.Get();
	subobjectGlobalRS.pDesc = &pInterfaceGlobalRS;
	subobjectGlobalRS.Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;

	subobjects[index++] = subobjectGlobalRS;
	traceStep(L"Create config and global root signature");

	
	// Create the RTPSO
	D3D12_STATE_OBJECT_DESC descRTSO;
	descRTSO.NumSubobjects = subobjects.size(); // 10
	descRTSO.pSubobjects = subobjects.data();
	descRTSO.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;

	HRESULT hr = owner->Device->CreateStateObject(&descRTSO, IID_PPV_ARGS(&RTPipelineState));
	if (FAILED(hr))
	{
		traceStep(L"CreateStateObject failed");
		AppendCpuRuntimeTrace(
			L"[DX12RTInitRS] CreateStateObject failed shader=\"" + shaderFileWide +
			L"\", target=\"" + ToWide(ShaderLibraryTarget) +
			L"\", hr=" + FormatHexHRESULT(hr));
		AppendD3D12InfoQueueMessages(owner->Device.Get(), L"CreateStateObject " + shaderFileWide + L" " + ToWide(ShaderLibraryTarget));
		{
			const std::wstring context = L"DX12Backend::CreateStateObject " + shaderFileWide;
			owner->MarkDeviceLost(context.c_str(), hr);
		}
		stringstream ss;
		ss << "Failed to compile shader : " << ShaderFile << "\n";
		owner->errorString += ss.str();
		OutputDebugStringA(ss.str().c_str());
		return false;
	}
	traceStep(L"CreateStateObject");

	const std::wstring rtPipelineName = L"RTPSO: " + ShaderDebugStem(ShaderFile);
	SetName(RTPipelineState.Get(), rtPipelineName.c_str());

	AppendCpuRuntimeTrace(
		L"[StartupTiming][DX12RTInitRS] complete shader=\"" + shaderFileWide +
		L"\", totalMs=" + FormatDx12InitMilliseconds(ElapsedDx12InitMilliseconds(totalStart, std::chrono::steady_clock::now())));

	return true;
}

bool D3D12RTPipelineStateObject::BuildDispatchRaysDesc(uint32_t width, uint32_t height, D3D12_DISPATCH_RAYS_DESC& outDesc) const
{
	DX12Backend* owner = Owner;
	if (!owner || !ShaderTable || ShaderTableSize == 0 || ShaderTableEntrySize == 0)
		return false;

	outDesc = {};
	outDesc.Width = width;
	outDesc.Height = height;
	outDesc.Depth = 1;

	D3D12_GPU_VIRTUAL_ADDRESS StartAddress = ShaderTable->GetGPUVirtualAddress() + ShaderTableSize * owner->CurrentFrameIndex;

	outDesc.RayGenerationShaderRecord.StartAddress = StartAddress;
	outDesc.RayGenerationShaderRecord.SizeInBytes = ShaderTableEntrySize;

	// Miss is the second entry in the shader-table
	UINT NumMissShader = 0;
	for (const auto& sb : ShaderBinding)
	{
		if (sb.second.Type == MISS)
			NumMissShader++;
	}
	size_t missOffset = ShaderTableEntrySize * 1;
	outDesc.MissShaderTable.StartAddress = StartAddress + missOffset;
	outDesc.MissShaderTable.StrideInBytes = ShaderTableEntrySize;
	outDesc.MissShaderTable.SizeInBytes = ShaderTableEntrySize * NumMissShader;

	
	 // Hit is the third entry in the shader-table
	size_t hitOffset = missOffset + NumMissShader * ShaderTableEntrySize;
	outDesc.HitGroupTable.StartAddress = StartAddress + hitOffset;
	outDesc.HitGroupTable.StrideInBytes = ShaderTableEntrySize;
	outDesc.HitGroupTable.SizeInBytes = ShaderTableEntrySize * GetHitShaderRecordCount();
	return true;
}

void D3D12RTPipelineStateObject::Apply(uint32_t width, uint32_t height)
{
	DX12Backend* owner = Owner;
	assert(owner);
	CommandList* CommandList = nullptr;
	CommandList = ResolveCommandList(owner, CommandList);
	assert(CommandList);
	D3D12_DISPATCH_RAYS_DESC raytraceDesc = {};
	if (!BuildDispatchRaysDesc(width, height, raytraceDesc))
		return;

	// Bind the empty root signature
	owner->InvalidateGraphicsCommandStateCache();
	owner->SetComputeRootSignatureIfNeeded(CommandList->CmdList.Get(), GlobalRS.Get());

	SetGlobalBinding(CommandList);

	owner->SetRayTracingPipelineStateIfNeeded(CommandList->CmdList.Get(), RTPipelineState.Get());
	
	CommandList->CmdList->DispatchRays(&raytraceDesc);
}

bool D3D12RTPipelineStateObject::GetDispatchRaysIndirectTemplate(uint32_t width, uint32_t height, RtDispatchRaysIndirectTemplate& outTemplate) const
{
	D3D12_DISPATCH_RAYS_DESC desc = {};
	if (!BuildDispatchRaysDesc(width, height, desc))
		return false;

	outTemplate = {};
	outTemplate.RayGenerationStartAddress = desc.RayGenerationShaderRecord.StartAddress;
	outTemplate.RayGenerationSizeInBytes = desc.RayGenerationShaderRecord.SizeInBytes;
	outTemplate.MissStartAddress = desc.MissShaderTable.StartAddress;
	outTemplate.MissSizeInBytes = desc.MissShaderTable.SizeInBytes;
	outTemplate.MissStrideInBytes = desc.MissShaderTable.StrideInBytes;
	outTemplate.HitGroupStartAddress = desc.HitGroupTable.StartAddress;
	outTemplate.HitGroupSizeInBytes = desc.HitGroupTable.SizeInBytes;
	outTemplate.HitGroupStrideInBytes = desc.HitGroupTable.StrideInBytes;
	outTemplate.CallableStartAddress = desc.CallableShaderTable.StartAddress;
	outTemplate.CallableSizeInBytes = desc.CallableShaderTable.SizeInBytes;
	outTemplate.CallableStrideInBytes = desc.CallableShaderTable.StrideInBytes;
	outTemplate.Width = desc.Width;
	outTemplate.Height = desc.Height;
	outTemplate.Depth = desc.Depth;
	return true;
}

bool D3D12RTPipelineStateObject::EnsureDispatchRaysCommandSignature()
{
	if (DispatchRaysCommandSignature)
		return true;

	DX12Backend* owner = Owner;
	if (!owner || !owner->Device)
		return false;

	D3D12_INDIRECT_ARGUMENT_DESC argumentDesc = {};
	argumentDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS;

	D3D12_COMMAND_SIGNATURE_DESC commandSignatureDesc = {};
	commandSignatureDesc.ByteStride = sizeof(D3D12_DISPATCH_RAYS_DESC);
	commandSignatureDesc.NumArgumentDescs = 1;
	commandSignatureDesc.pArgumentDescs = &argumentDesc;

	HRESULT hr = owner->Device->CreateCommandSignature(
		&commandSignatureDesc,
		nullptr,
		IID_PPV_ARGS(&DispatchRaysCommandSignature));
	if (FAILED(hr))
	{
		AppendCpuRuntimeTrace(L"[DX12RT] Create DISPATCH_RAYS command signature failed hr=" + FormatHexHRESULT(hr));
		return false;
	}

	SetName(DispatchRaysCommandSignature.Get(), L"Corona DispatchRaysIndirect CommandSignature");
	return true;
}

bool D3D12RTPipelineStateObject::ApplyIndirect(Buffer* indirectArgumentBuffer, uint64_t byteOffset)
{
	DX12Backend* owner = Owner;
	assert(owner);
	if (!indirectArgumentBuffer || !indirectArgumentBuffer->resource || !EnsureDispatchRaysCommandSignature())
		return false;

	CommandList* CommandList = nullptr;
	CommandList = ResolveCommandList(owner, CommandList);
	assert(CommandList);

	owner->InvalidateGraphicsCommandStateCache();
	owner->SetComputeRootSignatureIfNeeded(CommandList->CmdList.Get(), GlobalRS.Get());

	SetGlobalBinding(CommandList);

	owner->SetRayTracingPipelineStateIfNeeded(CommandList->CmdList.Get(), RTPipelineState.Get());
	CommandList->CmdList->ExecuteIndirect(
		DispatchRaysCommandSignature.Get(),
		1,
		indirectArgumentBuffer->resource.Get(),
		byteOffset,
		nullptr,
		0);
	return true;
}

void D3D12RTPipelineStateObject::SetTextureUAV(const string& shader, const string& bindingName, Texture* texture, int instanceIndex)
{
	assert(texture);
	SetUAVHandle(shader, bindingName, texture->CpuHandleUAV, texture->GpuHandleUAV, instanceIndex);
}

void D3D12RTPipelineStateObject::SetBufferUAV(const string& shader, const string& bindingName, Buffer* buffer, int instanceIndex)
{
	assert(buffer);
	SetUAVHandle(shader, bindingName, buffer->CpuHandleUAV, buffer->GpuHandleUAV, instanceIndex);
}

void D3D12RTPipelineStateObject::SetTextureSRV(const string& shader, const string& bindingName, Texture* texture, int instanceIndex)
{
	assert(texture);
	SetSRVHandle(shader, bindingName, texture->CpuHandleSRV, texture->GpuHandleSRV, instanceIndex);
}

void D3D12RTPipelineStateObject::SetBufferSRV(const string& shader, const string& bindingName, Buffer* buffer, int instanceIndex)
{
	assert(buffer);
	SetSRVHandle(shader, bindingName, buffer->CpuHandleSRV, buffer->GpuHandleSRV, instanceIndex);
}

bool D3D12RTPipelineStateObject::SetBindlessTextureTable(const string& shader, const string& bindingName)
{
	if (!Owner || !Owner->IsBindlessTextureTableReady())
		return false;

	SetSRVHandle(shader, bindingName, Owner->GetBindlessTextureTableCpuHandle(), Owner->GetBindlessTextureTableGpuHandle(), -1);
	return true;
}

bool D3D12RTPipelineStateObject::SetBindlessBufferTable(const string& shader, const string& bindingName)
{
	if (!Owner || !Owner->IsBindlessBufferTableReady())
		return false;

	SetSRVHandle(shader, bindingName, Owner->GetBindlessBufferTableCpuHandle(), Owner->GetBindlessBufferTableGpuHandle(), -1);
	return true;
}

void D3D12RTPipelineStateObject::SetAccelerationStructure(const string& shader, const string& bindingName, const std::shared_ptr<RTAS>& rtas, int instanceIndex)
{
	D3D12RTAS* dx12RTAS = dynamic_cast<D3D12RTAS*>(rtas.get());
	assert(dx12RTAS);
	SetSRVHandle(shader, bindingName, dx12RTAS->CPUHandle, dx12RTAS->GPUHandle, instanceIndex);
}

void DescriptorHeapRing::Init(DescriptorHeap* InDHHeap, UINT InNumDescriptors, UINT InNumFrame)
{
	DHeap = InDHHeap;
	NumDescriptors = InNumDescriptors;
	NumFrame = InNumFrame;
	DescriptorSize = DHeap->DescriptorSize;

	DHeap->AllocDescriptors(CPUHeapStart, GPUHeapStart, NumDescriptors * NumFrame);
}

void DescriptorHeapRing::AllocDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE& cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE& gpuHandle)
{
	cpuHandle.ptr = CPUHeapStart.ptr + NumAllocated * DescriptorSize + NumDescriptors * DescriptorSize * CurrentFrame;
	gpuHandle.ptr = GPUHeapStart.ptr + NumAllocated * DescriptorSize + NumDescriptors * DescriptorSize * CurrentFrame;

	NumAllocated++;
}

void DescriptorHeapRing::Advance()
{
	CurrentFrame = (CurrentFrame + 1) % NumFrame;
	NumAllocated = 0;
}

void Scene::SetTransform(glm::mat4x4 inTransform)
{
	for (auto& mesh : meshes)
	{
		mesh->transform = inTransform;
	}
}

CommandQueue::CommandQueue(
	DX12Backend* owner,
	ID3D12Device5* device,
	D3D12_COMMAND_LIST_TYPE commandListType,
	const wchar_t* queueDebugName,
	const wchar_t* allocatorDebugName,
	const wchar_t* listDebugName,
	bool bEnableAftermathMarkers)
{
	Owner = owner;
	Type = commandListType;
#if USE_AFTERMATH
	bAftermathMarkersEnabled = bEnableAftermathMarkers;
#else
	(void)bEnableAftermathMarkers;
#endif
	assert(device);
	ThrowIfFailed(device->CreateFence(CurrentFenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
	// Create an event handle to use for frame synchronization.
	m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	queueDesc.Type = commandListType;

	ThrowIfFailed(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&CmdQueue)));
	SetName(CmdQueue.Get(), queueDebugName ? queueDebugName : L"Corona Command Queue");

	CommandListPool.reserve(CommandListPoolSize);
	for (int i = 0; i < CommandListPoolSize; i++)
	{
		CommandList * cmdList = new CommandList;
		ThrowIfFailed(device->CreateCommandAllocator(commandListType, IID_PPV_ARGS(&cmdList->CmdAllocator)));
		SetNameIndexed(cmdList->CmdAllocator.Get(), allocatorDebugName ? allocatorDebugName : L"Corona Command Allocator", i);

		ThrowIfFailed(device->CreateCommandList(0, commandListType, cmdList->CmdAllocator.Get(), nullptr, IID_PPV_ARGS(&cmdList->CmdList)));
		cmdList->CmdList->Close();
		SetNameIndexed(cmdList->CmdList.Get(), listDebugName ? listDebugName : L"Corona Command List", i);

		CommandListPool.emplace_back(shared_ptr<CommandList>(cmdList));
	}
}

CommandQueue::~CommandQueue()
{
#if USE_AFTERMATH
	for (const std::shared_ptr<CommandList>& cmdList : CommandListPool)
	{
		if (cmdList && cmdList->AftermathContext)
		{
			GFSDK_Aftermath_ReleaseContextHandle(cmdList->AftermathContext);
			cmdList->AftermathContext = nullptr;
		}
	}
#endif
}

CommandList * CommandQueue::AllocCmdList()
{
	std::lock_guard<std::mutex> lock(CmdAllocMtx);
	if (Owner && Owner->IsDeviceLost())
		return nullptr;

	CommandList* cmdList = CommandListPool[CurrentIndex].get();
	if(cmdList->Fence.has_value())
		WaitFenceValue(cmdList->Fence.value());
	if (Owner && Owner->IsDeviceLost())
		return nullptr;

	CurrentIndex++;

	CurrentIndex = CurrentIndex % CommandListPoolSize;

	cmdList->Fence = std::nullopt;
	cmdList->Reset();
#if USE_AFTERMATH
	if (bAftermathMarkersEnabled && !cmdList->AftermathContext && !cmdList->bAftermathContextCreateAttempted)
	{
		cmdList->bAftermathContextCreateAttempted = true;
		const GFSDK_Aftermath_Result contextResult = GFSDK_Aftermath_DX12_CreateContextHandle(
			cmdList->CmdList.Get(),
			&cmdList->AftermathContext);
		if (contextResult != GFSDK_Aftermath_Result::GFSDK_Aftermath_Result_Success)
		{
			cmdList->AftermathContext = nullptr;
			OutputDebugStringA("Failed to create NVIDIA Aftermath command list context.\n");
		}
	}
#endif
	return cmdList;
}

UINT64 CommandQueue::ExecuteCommandList(CommandList * cmd)
{
	if (!cmd || !CmdQueue)
		return CurrentFenceValue;
	if (Owner && Owner->IsDeviceLost())
		return CurrentFenceValue;

	const HRESULT closeHr = cmd->CmdList->Close();
	if (FAILED(closeHr))
	{
		if (Owner)
			Owner->MarkDeviceLost(L"CommandQueue::ExecuteCommandList Close", closeHr);
		return CurrentFenceValue;
	}
	ID3D12CommandList* ppCommandListsEnd[] = { cmd->CmdList.Get() };
	CmdQueue->ExecuteCommandLists(_countof(ppCommandListsEnd), ppCommandListsEnd);

	const UINT64 submittedFenceValue = CurrentFenceValue;
	const HRESULT signalHr = CmdQueue->Signal(m_fence.Get(), submittedFenceValue);
	if (FAILED(signalHr))
	{
		if (Owner)
			Owner->MarkDeviceLost(L"CommandQueue::ExecuteCommandList Signal", signalHr);
		return submittedFenceValue;
	}
	cmd->Fence = submittedFenceValue;
	CurrentFenceValue++;
	return submittedFenceValue;
}

void CommandQueue::WaitGPU()
{
	if (!CmdQueue || !m_fence || (Owner && Owner->IsDeviceLost()))
		return;

	const HRESULT signalHr = CmdQueue->Signal(m_fence.Get(), CurrentFenceValue);
	if (FAILED(signalHr))
	{
		if (Owner)
			Owner->MarkDeviceLost(L"CommandQueue::WaitGPU Signal", signalHr);
		return;
	}
	const HRESULT waitHr = m_fence->SetEventOnCompletion(CurrentFenceValue, m_fenceEvent);
	if (FAILED(waitHr))
	{
		if (Owner)
			Owner->MarkDeviceLost(L"CommandQueue::WaitGPU SetEventOnCompletion", waitHr);
		return;
	}
	WaitForSingleObject(m_fenceEvent, INFINITE);
	
	CurrentFenceValue++;
}

void CommandQueue::WaitFenceValue(UINT64 fenceValue)
{
	if (!m_fence || (Owner && Owner->IsDeviceLost()))
		return;
	const HRESULT waitHr = m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent);
	if (FAILED(waitHr))
	{
		if (Owner)
			Owner->MarkDeviceLost(L"CommandQueue::WaitFenceValue SetEventOnCompletion", waitHr);
		return;
	}
	WaitForSingleObject(m_fenceEvent, INFINITE);
}

void CommandQueue::SignalCurrentFence()
{
	if (!CmdQueue || !m_fence || (Owner && Owner->IsDeviceLost()))
		return;
	const HRESULT signalHr = CmdQueue->Signal(m_fence.Get(), CurrentFenceValue);
	if (FAILED(signalHr))
	{
		if (Owner)
			Owner->MarkDeviceLost(L"CommandQueue::SignalCurrentFence", signalHr);
		return;
	}
	CurrentFenceValue++;
}

void CommandList::Reset()
{
	CmdAllocator->Reset();
	CmdList->Reset(CmdAllocator.Get(), nullptr);
}

std::tuple<UINT64, UINT8*> ConstantBufferRingBuffer::AllocGPUMemory(UINT InSize)
{

	UINT64 AllocGPUAddr = CBMem->GetGPUVirtualAddress() + CurrentFrame * TotalSize + AllocPos;

	UINT8* pMapped = (UINT8*)MemMapped + CurrentFrame * TotalSize + AllocPos;
	AllocPos += InSize;
	
	return std::make_tuple(AllocGPUAddr, pMapped);
}

void ConstantBufferRingBuffer::Advance()
{
	CurrentFrame = (CurrentFrame + 1) % NumFrame;
	AllocPos = 0;
}

ConstantBufferRingBuffer::ConstantBufferRingBuffer(ID3D12Device5* InDevice, UINT InSize, UINT InNumFrame)
{
	Device = InDevice;
	assert(Device);
	NumFrame = InNumFrame;
	TotalSize = InSize;

	D3D12_HEAP_PROPERTIES heapProp;
	heapProp.Type = D3D12_HEAP_TYPE_UPLOAD;
	heapProp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heapProp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	heapProp.CreationNodeMask = 1;
	heapProp.VisibleNodeMask = 1;

	D3D12_RESOURCE_DESC resDesc;

	resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	resDesc.Alignment = 0;
	resDesc.Width = InSize * NumFrame;
	resDesc.Height = 1;
	resDesc.DepthOrArraySize = 1;
	resDesc.MipLevels = 1;
	resDesc.Format = DXGI_FORMAT_UNKNOWN;
	resDesc.SampleDesc.Count = 1;
	resDesc.SampleDesc.Quality = 0;
	resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	resDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

	ThrowIfFailed(Device->CreateCommittedResource(
		&heapProp,
		D3D12_HEAP_FLAG_NONE,
		&resDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&CBMem)));

	NAME_D3D12_OBJECT(CBMem);

	CD3DX12_RANGE readRange(0, 0);		// We do not intend to read from this resource on the CPU.
	ThrowIfFailed(CBMem->Map(0, &readRange, reinterpret_cast<void**>(&MemMapped)));
}

ConstantBufferRingBuffer::~ConstantBufferRingBuffer()
{
	CBMem->Unmap(0, nullptr);
}

void Buffer::MakeByteAddressBufferSRV()
{
	DX12Backend* owner = Owner;
	if (!owner)
		return;
	// create shader resource view
	D3D12_SHADER_RESOURCE_VIEW_DESC bufferSRVDesc;
	bufferSRVDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	bufferSRVDesc.Format = DXGI_FORMAT_R32_TYPELESS;
	bufferSRVDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
	bufferSRVDesc.Buffer.StructureByteStride = 0;
	bufferSRVDesc.Buffer.FirstElement = 0;
	bufferSRVDesc.Buffer.NumElements = (NumElements * ElementSize) / sizeof(UINT32);
	bufferSRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

	owner->TextureDHRing->AllocDescriptor(CpuHandleSRV, GpuHandleSRV);

	owner->Device->CreateShaderResourceView(resource.Get(), &bufferSRVDesc, CpuHandleSRV);

	Type = BYTE_ADDRESS;
}

void Buffer::MakeStructuredBufferSRV()
{
	DX12Backend* owner = Owner;
	if (!owner)
		return;
	// create shader resource view
	D3D12_SHADER_RESOURCE_VIEW_DESC bufferSRVDesc;
	bufferSRVDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	bufferSRVDesc.Format = DXGI_FORMAT_UNKNOWN;
	bufferSRVDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
	bufferSRVDesc.Buffer.StructureByteStride = ElementSize;
	bufferSRVDesc.Buffer.FirstElement = ElementSize > 0 ? static_cast<UINT>(SuballocationOffsetBytes / ElementSize) : 0;
	bufferSRVDesc.Buffer.NumElements = NumElements;
	bufferSRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

	owner->TextureDHRing->AllocDescriptor(CpuHandleSRV, GpuHandleSRV);

	owner->Device->CreateShaderResourceView(resource.Get(), &bufferSRVDesc, CpuHandleSRV);

	Type = STRUCTURED;
}

Texture* DX12Backend::GetCurrentWindowRenderTarget()
{
	if (CurrentFrameIndex >= GetFrameCount())
		return nullptr;

	if (SwapChainRenderTargets.size() != GetFrameCount())
		SwapChainRenderTargets.resize(GetFrameCount());

	if (!SwapChainRenderTargets[CurrentFrameIndex])
	{
		SwapChainRenderTargets[CurrentFrameIndex] = GetSwapChainTexture(CurrentFrameIndex);
	}

	return SwapChainRenderTargets[CurrentFrameIndex].get();
}

void DX12Backend::PrepareWindowRenderTarget(Texture* renderTarget)
{
	if (!renderTarget)
		return;

	TransitionTexture(renderTarget, EResourceState::Present, EResourceState::RenderTarget);
}

void DX12Backend::FinalizeWindowRenderTarget(Texture* renderTarget)
{
	if (!renderTarget)
		return;

	TransitionTexture(renderTarget, EResourceState::RenderTarget, EResourceState::Present);
}

void DX12Backend::RequestWindowCapture(const std::wstring& outputPath)
{
	PendingWindowCapturePath = outputPath;
}

bool DX12Backend::ConsumeWindowCaptureResult(std::wstring* outputPath, bool* success, std::wstring* errorMessage)
{
	if (!bLastWindowCaptureResultValid && !PendingWindowCapturePath.empty())
	{
		Texture* renderTarget = GetCurrentWindowRenderTarget();
		if (!renderTarget)
			return false;

		std::filesystem::create_directories(std::filesystem::path(PendingWindowCapturePath).parent_path());
		DirectX::ScratchImage captured;
		LastWindowCapturePath = PendingWindowCapturePath;
		LastWindowCaptureError.clear();
		bLastWindowCaptureSucceeded = false;

		const HRESULT captureHr = DirectX::CaptureTexture(
			CmdQ->CmdQueue.Get(),
			renderTarget->resource.Get(),
			false,
			captured,
			D3D12_RESOURCE_STATE_PRESENT,
			D3D12_RESOURCE_STATE_PRESENT);
		if (SUCCEEDED(captureHr))
		{
			bLastWindowCaptureSucceeded = SaveScratchImagePNG(captured, PendingWindowCapturePath, &LastWindowCaptureError);
		}
		else
		{
			LastWindowCaptureError = L"Failed to capture DX12 window render target.";
		}

		bLastWindowCaptureResultValid = true;
		PendingWindowCapturePath.clear();
	}

	if (!bLastWindowCaptureResultValid)
		return false;

	if (outputPath)
		*outputPath = LastWindowCapturePath;
	if (success)
		*success = bLastWindowCaptureSucceeded;
	if (errorMessage)
		*errorMessage = LastWindowCaptureError;

	bLastWindowCaptureResultValid = false;
	LastWindowCapturePath.clear();
	LastWindowCaptureError.clear();
	bLastWindowCaptureSucceeded = false;
	return true;
}

std::shared_ptr<GraphicsPipelineHandle> DX12Backend::CreateGraphicsPipeline(const GraphicsPipelineDesc& desc)
{
	auto handle = std::make_shared<DX12GraphicsPipelineHandle>();

	const bool bRequiresVertexBindlessBufferSM66 = RequiresDX12VertexBindlessBufferShaderModel66(desc);
	const bool bRequiresDrawParametersSM68 = desc.VertexEntryPoint == "VSMainBindlessIndirect";
	const std::string vertexShaderTarget =
		bRequiresDrawParametersSM68 ? "vs_6_8" :
		(bRequiresVertexBindlessBufferSM66 ? "vs_6_6" : "vs_6_0");
	if (bRequiresVertexBindlessBufferSM66)
	{
		AppendCpuRuntimeTrace(
			L"[DX12GraphicsPipeline] using " + ToWide(vertexShaderTarget) +
			L" for vertex bindless pipeline shader=\"" + desc.ShaderPath +
			L"\", entry=\"" + ToWide(desc.VertexEntryPoint) + L"\"");
	}

	ShaderBytecode vs = CreateShader(desc.ShaderPath, desc.VertexEntryPoint, vertexShaderTarget);
	ShaderBytecode ps = CreateShader(desc.ShaderPath, desc.PixelEntryPoint, "ps_6_0");
	if (!vs.IsValid() || !ps.IsValid())
		return nullptr;

	std::vector<std::string> semantics;
	semantics.reserve(desc.VertexElements.size());
	std::vector<D3D12_INPUT_ELEMENT_DESC> vertexElements;
	vertexElements.reserve(desc.VertexElements.size());
	for (const auto& element : desc.VertexElements)
	{
		semantics.push_back(element.SemanticName);
		D3D12_INPUT_ELEMENT_DESC inputDesc{};
		inputDesc.SemanticName = semantics.back().c_str();
		inputDesc.SemanticIndex = element.SemanticIndex;
		inputDesc.Format = ToDXGIFormat(element.Format);
		inputDesc.InputSlot = 0;
		inputDesc.AlignedByteOffset = element.Offset;
		inputDesc.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
		inputDesc.InstanceDataStepRate = 0;
		vertexElements.push_back(inputDesc);
	}

	CD3DX12_RASTERIZER_DESC rasterizerStateDesc(D3D12_DEFAULT);
	rasterizerStateDesc.CullMode = desc.bCullBackFaces ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
	if (desc.bDepthBiasEnable)
	{
		rasterizerStateDesc.DepthBias = static_cast<INT>(std::lround(desc.DepthBiasConstantFactor));
		rasterizerStateDesc.DepthBiasClamp = desc.DepthBiasClamp;
		rasterizerStateDesc.SlopeScaledDepthBias = desc.DepthBiasSlopeFactor;
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.InputLayout = { vertexElements.data(), static_cast<UINT>(vertexElements.size()) };
	psoDesc.RasterizerState = rasterizerStateDesc;
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	if (desc.BlendMode != EBlendMode::Opaque)
	{
		// Only RT 0 blends; remaining RTs keep write-mask = 0 so additive/
		// alpha particle PSOs that share the multi-RT GBuffer pass don't
		// touch Normal/Velocity/Roughness.
		auto& rt0 = psoDesc.BlendState.RenderTarget[0];
		rt0.BlendEnable = TRUE;
		rt0.LogicOpEnable = FALSE;
		rt0.SrcBlendAlpha = D3D12_BLEND_ONE;
		rt0.DestBlendAlpha = D3D12_BLEND_ZERO;
		rt0.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		rt0.BlendOp = D3D12_BLEND_OP_ADD;
		rt0.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		if (desc.BlendMode == EBlendMode::Additive)
		{
			rt0.SrcBlend = D3D12_BLEND_SRC_ALPHA;
			rt0.DestBlend = D3D12_BLEND_ONE;
		}
		else // AlphaBlend
		{
			rt0.SrcBlend = D3D12_BLEND_SRC_ALPHA;
			rt0.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
		}
		for (UINT i = 1; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
			psoDesc.BlendState.RenderTarget[i].RenderTargetWriteMask = 0;
	}
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState.DepthEnable = desc.bDepthEnable ? TRUE : FALSE;
	psoDesc.DepthStencilState.DepthWriteMask =
		(desc.bDepthEnable && desc.bDepthWriteEnable) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
	psoDesc.DepthStencilState.StencilEnable = FALSE;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = static_cast<UINT>(desc.ColorFormats.size());
	for (UINT i = 0; i < psoDesc.NumRenderTargets; ++i)
		psoDesc.RTVFormats[i] = ToDXGIFormat(desc.ColorFormats[i]);
	psoDesc.DSVFormat = desc.DepthFormat.has_value() ? ToDXGIFormat(*desc.DepthFormat) : DXGI_FORMAT_UNKNOWN;
	psoDesc.SampleDesc.Count = 1;

	auto pso = std::make_shared<PipelineStateObject>();
	pso->Owner = this;
	pso->vs = std::move(vs);
	pso->ps = std::move(ps);
	pso->DebugName = MakePipelineDebugName(
		L"GraphicsPSO",
		desc.ShaderPath,
		desc.VertexEntryPoint,
		desc.PixelEntryPoint);
	pso->graphicsPSODesc = psoDesc;
	if (!desc.PipelineLayout.Bindings.empty())
	{
		for (const RHIBindingDesc& binding : desc.PipelineLayout.Bindings)
		{
			switch (binding.DescriptorKind)
			{
			case RHIDescriptorKind::SRV:
			case RHIDescriptorKind::AccelerationStructure:
				pso->BindSRV(binding.Name, binding.RegisterIndex, RHILegacyDescriptorCount(binding));
				if (auto it = pso->textureBinding.find(binding.Name); it != pso->textureBinding.end())
					it->second.Schema = binding;
				break;
			case RHIDescriptorKind::UAV:
				pso->BindUAV(binding.Name, binding.RegisterIndex);
				if (auto it = pso->uavBinding.find(binding.Name); it != pso->uavBinding.end())
					it->second.Schema = binding;
				break;
			case RHIDescriptorKind::CBV:
			{
				const uint32_t sizeInBytes = binding.SizeInBytes > 0 ? binding.SizeInBytes : desc.ConstantBufferSize;
				pso->BindCBV(binding.Name, binding.RegisterIndex, sizeInBytes);
				if (auto it = pso->constantBufferBinding.find(binding.Name); it != pso->constantBufferBinding.end())
					it->second.Schema = binding;
				break;
			}
			case RHIDescriptorKind::Sampler:
				pso->BindSampler(binding.Name, binding.RegisterIndex);
				if (auto it = pso->samplerBinding.find(binding.Name); it != pso->samplerBinding.end())
					it->second.Schema = binding;
				break;
			}
		}
	}
	else
	{
		if (desc.ConstantBufferSize > 0)
			pso->BindCBV("__CB0", desc.ConstantBufferBinding, desc.ConstantBufferSize);
		for (const auto& binding : desc.TextureBindings)
			pso->BindSRV(binding.Name, binding.Slot, 1);
		for (const auto& binding : desc.BufferBindings)
			pso->BindSRV(binding.Name, binding.Slot, 1);
		for (const auto& binding : desc.SamplerBindings)
			pso->BindSampler(binding.Name, binding.Slot);
	}
	if (!pso->Init())
		return nullptr;

	handle->PSO = pso;
	handle->ConstantBufferSize = desc.ConstantBufferSize;
	handle->VertexStride = desc.VertexStride;
	handle->ShaderPathForDiag = desc.ShaderPath;
	return handle;
}

std::shared_ptr<GraphicsBindGroupHandle> DX12Backend::CreateGraphicsBindGroup(const GraphicsBindGroupDesc& desc)
{
	auto* dxPipeline = dynamic_cast<DX12GraphicsPipelineHandle*>(desc.Pipeline);
	if (!dxPipeline || !dxPipeline->PSO)
		return nullptr;

	auto handle = std::make_shared<DX12GraphicsBindGroupHandle>();
	handle->Pipeline = dxPipeline;
	handle->Entries.reserve(desc.Entries.size());

	for (const GraphicsBindGroupEntry& src : desc.Entries)
	{
		DX12GraphicsBindGroupEntry dst{};
		dst.Type = src.Type;
		dst.BindingName = src.BindingName;
		dst.Slot = src.Slot;
		dst.TextureValue = src.TextureValue;
		dst.BufferValue = src.BufferValue;
		dst.VertexBufferValue = src.VertexBufferValue;
		dst.SamplerValue = src.SamplerValue;
		if (src.Type == EGraphicsBindGroupEntryType::ConstantData && src.ConstantData && src.ConstantDataSize > 0)
		{
			const auto* bytes = static_cast<const uint8_t*>(src.ConstantData);
			dst.ConstantData.assign(bytes, bytes + src.ConstantDataSize);
		}
		handle->Entries.push_back(std::move(dst));
	}

	return handle;
}

void DX12Backend::BindGraphicsPipeline(GraphicsPipelineHandle* pipeline)
{
	auto* dxPipeline = dynamic_cast<DX12GraphicsPipelineHandle*>(pipeline);
	if (dxPipeline && dxPipeline->PSO)
	{
		if (BoundGraphicsPipeline != dxPipeline)
		{
			dxPipeline->PSO->Apply();
			BoundGraphicsPipeline = dxPipeline;
		}
		BoundGraphicsPipelineForDiag = dxPipeline;
	}
	else
	{
		BoundGraphicsPipeline = nullptr;
		BoundGraphicsPipelineForDiag = nullptr;
	}
}

void DX12Backend::BindGraphicsBindGroup(GraphicsPipelineHandle* pipeline, uint32_t slot, const std::shared_ptr<GraphicsBindGroupHandle>& bindGroup)
{
	(void)slot;
	auto* dxPipeline = dynamic_cast<DX12GraphicsPipelineHandle*>(pipeline);
	auto* dxBindGroup = dynamic_cast<DX12GraphicsBindGroupHandle*>(bindGroup.get());
	if (!dxPipeline || !dxPipeline->PSO || !dxBindGroup || dxBindGroup->Pipeline != dxPipeline)
		return;

	for (const DX12GraphicsBindGroupEntry& entry : dxBindGroup->Entries)
	{
		switch (entry.Type)
		{
		case EGraphicsBindGroupEntryType::TextureSRV:
			if (entry.TextureValue)
				dxPipeline->PSO->SetSRV(entry.BindingName, entry.TextureValue->GpuHandleSRV);
			break;
		case EGraphicsBindGroupEntryType::BufferSRV:
			if (entry.BufferValue)
				dxPipeline->PSO->SetSRV(entry.BindingName, entry.BufferValue->GpuHandleSRV);
			break;
		case EGraphicsBindGroupEntryType::VertexBufferSRV:
			if (entry.VertexBufferValue)
				dxPipeline->PSO->SetSRV(entry.BindingName, entry.VertexBufferValue->GpuHandleSRV);
			break;
		case EGraphicsBindGroupEntryType::Sampler:
			if (entry.SamplerValue)
				dxPipeline->PSO->SetSampler(entry.BindingName, entry.SamplerValue);
			break;
		case EGraphicsBindGroupEntryType::ConstantData:
			if (!entry.ConstantData.empty() && entry.Slot == 0 && dxPipeline->ConstantBufferSize > 0)
			{
				const auto bindingIt = dxPipeline->PSO->constantBufferBinding.find("__CB0");
				if (bindingIt == dxPipeline->PSO->constantBufferBinding.end())
					break;

				const uint32_t sourceSize = bindingIt->second.sourceSize;
				if (sourceSize == 0)
					break;

				if (entry.ConstantData.size() >= sourceSize)
				{
					dxPipeline->PSO->SetCBVValue("__CB0", const_cast<uint8_t*>(entry.ConstantData.data()));
					break;
				}

				thread_local std::vector<uint8_t> sourceData;
				sourceData.assign(sourceSize, 0);
				std::memcpy(sourceData.data(), entry.ConstantData.data(), entry.ConstantData.size());
				dxPipeline->PSO->SetCBVValue("__CB0", sourceData.data());
			}
			break;
		}
	}
}
