#pragma once

#include <d3d12.h>
#include <wrl.h>
#include <dxgi1_5.h>
#include <set>
#include <vector>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <optional>
#include <mutex>
#include <array>
#define GLM_FORCE_CTOR_INIT

#include "glm/glm.hpp"
#define GLM_ENABLE_EXPERIMENTAL
#include "glm/gtx/transform.hpp"
#include "glm/mat4x4.hpp"
#include "glm/fwd.hpp"

#include <crtdbg.h>

#include "AftermathConfig.h"
#if _DEBUG
#define DEBUG_CLIENTBLOCK   new( _CLIENT_BLOCK, __FILE__, __LINE__)
#else
#define DEBUG_CLIENTBLOCK
#endif // _DEBUG

#include "D3D12Helpers.h"
#include "RenderBackend.h"
#include "RenderResources.h"

using namespace Microsoft::WRL;
using namespace std;

class DX12Backend;
class Texture;
class Sampler;
struct CoronaBvhViewerD3D12Handle;
//class ThreadDescriptorHeapPool;

class CommandList
{
public:
	ComPtr<ID3D12GraphicsCommandList4> CmdList;
	ComPtr<ID3D12CommandAllocator> CmdAllocator;
	std::optional<UINT64> Fence;
#if USE_AFTERMATH
	GFSDK_Aftermath_ContextHandle AftermathContext = nullptr;
	bool bAftermathContextCreateAttempted = false;
#endif

public:
	void Reset();
};

class CommandQueue
{
public:
	const UINT32 CommandListPoolSize = 4096;

	DX12Backend* Owner = nullptr;
	ComPtr<ID3D12CommandQueue> CmdQueue;
	D3D12_COMMAND_LIST_TYPE Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

	std::vector<shared_ptr<CommandList>> CommandListPool;
	UINT32 CurrentIndex = 0;

	std::mutex CmdAllocMtx;

	HANDLE m_fenceEvent;
	ComPtr<ID3D12Fence> m_fence;
	UINT64 CurrentFenceValue = 2;
#if USE_AFTERMATH
	bool bAftermathMarkersEnabled = false;
#endif
public:
	CommandQueue(
		DX12Backend* owner,
		ID3D12Device5* device,
		D3D12_COMMAND_LIST_TYPE commandListType = D3D12_COMMAND_LIST_TYPE_DIRECT,
		const wchar_t* queueDebugName = L"Corona Graphics Queue",
		const wchar_t* allocatorDebugName = L"Corona Command Allocator",
		const wchar_t* listDebugName = L"Corona Command List",
		bool bEnableAftermathMarkers = false);
	virtual ~CommandQueue();

	CommandList* AllocCmdList();

	UINT64 ExecuteCommandList(CommandList* cmd);

	void WaitGPU();
	
	void WaitFenceValue(UINT64 fenceValue);

	void SignalCurrentFence();
};

class FrameResource
{
public:
	UINT64 FenceValue = 0;
};

class PipelineStateObject
{
public:
	PipelineStateObject() = default;
	explicit PipelineStateObject(DX12Backend* owner) : Owner(owner) {}

	DX12Backend* Owner = nullptr;
	bool IsCompute = false;
	struct BindingData
	{
		string name;
		UINT rootParamIndex;
		UINT baseRegister;
		UINT numDescriptors;
		UINT sourceSize = 0;
		UINT cbSize = 0;

		Texture* texture;
		Sampler* sampler;

		UINT rootConst;
		RHIBindingDesc Schema;
	};


	map<string, BindingData> uavBinding;
	map<string, BindingData> textureBinding;
	map<string, BindingData> constantBufferBinding;
	map<string, BindingData> samplerBinding;
	map<string, BindingData> rootBinding;

	UINT RootParamIndex = 0;
	//shared_ptr<Shader> vs;
	//shared_ptr<Shader> ps;
	//shared_ptr<Shader> cs;

	ShaderBytecode vs;
	ShaderBytecode ps;
	ShaderBytecode cs;




	ComPtr<ID3D12RootSignature> RS;
	ComPtr<ID3D12PipelineState> PSO;
	std::wstring DebugName;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC graphicsPSODesc;
	D3D12_COMPUTE_PIPELINE_STATE_DESC computePSODesc;


	bool Init();


	void Apply(ID3D12GraphicsCommandList* CommandList = nullptr);

	void BindUAV(string name, int baseRegister);
	void BindSRV(string name, int baseRegister, int num);
	void BindCBV(string name, int baseRegister, int size);
	void BindRootConstant(string name, int baseRegister);
	void BindSampler(string name, int baseRegister);

	void SetSRV(string name, D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleSRV, ID3D12GraphicsCommandList* CommandList = nullptr);
	void SetUAV(string name, D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleUAV, ID3D12GraphicsCommandList* CommandList = nullptr);

	void SetSampler(string name, Sampler* sampler, ID3D12GraphicsCommandList* CommandList = nullptr);

	void SetCBVValue(string name, void* pData, ID3D12GraphicsCommandList* CommandList = nullptr);
	void SetRootConstant(string, UINT value, ID3D12GraphicsCommandList* CommandList = nullptr);
};

class D3D12ComputePipelineStateObject : public ComputePipelineStateObject
{
public:
	DX12Backend* Owner = nullptr;
	std::shared_ptr<PipelineStateObject> PSO;
	std::map<std::string, D3D12_GPU_DESCRIPTOR_HANDLE> PendingSRVs;
	std::map<std::string, D3D12_GPU_DESCRIPTOR_HANDLE> PendingUAVs;
	std::map<std::string, Sampler*> PendingSamplers;
	std::map<std::string, std::vector<uint8_t>> PendingCBVs;

	void BindSRV(const RHIBindingDesc& binding) override;
	void BindUAV(const RHIBindingDesc& binding) override;
	void BindCBV(const RHIBindingDesc& binding) override;
	void BindSampler(const RHIBindingDesc& binding) override;
	void BindSRV(const std::string& name, uint32_t baseRegister, uint32_t numDescriptors) override;
	void BindUAV(const std::string& name, uint32_t baseRegister) override;
	void BindCBV(const std::string& name, uint32_t baseRegister, uint32_t size) override;
	void BindSampler(const std::string& name, uint32_t baseRegister) override;
	bool InitCS(const std::wstring& shaderFile, const std::string& entryPoint) override;
	bool InitCSWithInlineRT(const std::wstring& shaderFile, const std::string& entryPoint) override;
	void Apply() override;
	void SetTextureSRV(const std::string& name, Texture* texture) override;
	void SetTextureUAV(const std::string& name, Texture* texture) override;
	void SetBufferSRV(const std::string& name, Buffer* buffer) override;
	void SetBufferUAV(const std::string& name, Buffer* buffer) override;
	void SetVertexBufferUAV(const std::string& name, VertexBuffer* vertexBuffer) override;
	void SetSampler(const std::string& name, Sampler* sampler) override;
	void SetCBVValue(const std::string& name, void* pData) override;
	void SetAccelerationStructure(const std::string& name, const std::shared_ptr<RTAS>& rtas) override;
};

class D3D12RTPipelineStateObject : public RTPipelineStateObject
{
public:
	DX12Backend* Owner = nullptr;
private:
	struct BindingData
	{
		D3D12_DESCRIPTOR_RANGE_TYPE Type;
		string name;
		UINT sourceSize = 0;
		UINT cbSize = 0;

		Texture* texture;
		Sampler* sampler;

		UINT BaseRegister;
		D3D12_CPU_DESCRIPTOR_HANDLE CPUHandle; // for multiple instances
		D3D12_GPU_DESCRIPTOR_HANDLE GPUHandle; // for multiple instances
		RHIBindingDesc Schema;
	};
	vector<BindingData> RaygenBinding;

	
public:
	ComPtr<ID3D12RootSignature> RaygenRS;
	ComPtr<ID3D12RootSignature> HitMissRS;
	ComPtr<ID3D12RootSignature> GlobalRS;
	ComPtr<ID3D12StateObject> RTPipelineState;

	enum ShaderType
	{
		GLOBAL,
		RAYGEN,
		MISS,
		HIT,
		ANYHIT
	};
	struct BindingInfo
	{
		ShaderType Type = GLOBAL;
		wstring ShaderName;
		vector<BindingData> Binding;

		ComPtr<ID3D12RootSignature> RS;

		D3D12_STATE_SUBOBJECT subobject;
		ID3D12RootSignature* pInterface;
		vector<const WCHAR*> ExportName;
	};
	map<string, BindingInfo> ShaderBinding;

	vector<BindingData> GlobalBinding;

	

	struct HitProgramData
	{
		//wstring HitGroupName;
		vector< D3D12_GPU_DESCRIPTOR_HANDLE> VecData;
	};

	struct HitGroupInfo
	{
		wstring name;
		wstring chs;
		wstring ahs;

		map<UINT, HitProgramData> HitProgramBinding;
	};

	vector<HitGroupInfo> VecHitGroup;
public:
	wstring kRayGenShader;
	wstring kMissShader;
	wstring kClosestHitShader;
	wstring kHitGroup;

	UINT MaxRecursion = 1;
	UINT MaxPayloadSizeInBytes = 0;
	UINT MaxAttributeSizeInBytes = 0;
	std::string ShaderLibraryTarget = "lib_6_3";
	std::vector<std::pair<std::string, std::string>> ShaderDefines;

	uint32_t ShaderTableEntrySize = 0;
	UINT ShaderTableSize = 0;
	ComPtr<ID3D12Resource> ShaderTable;
	ComPtr<ID3D12Resource> ShaderTableUpload;
	D3D12_RESOURCE_STATES ShaderTableState = D3D12_RESOURCE_STATE_COPY_DEST;
	ComPtr<ID3D12CommandSignature> DispatchRaysCommandSignature;
	std::vector<uint8_t> ShaderTableFrameValid;
	std::vector<uint32_t> ShaderTableFrameInstanceCount;
	std::vector<uint64_t> ShaderTableFrameSignature;
	bool HitProgramBindingPendingValid = false;
	uint32_t HitProgramBindingPendingInstanceCount = 0;
	uint64_t HitProgramBindingPendingSignature = 0;

	UINT NumInstance = 0;

	void SetNumInstances(uint32_t numInstances) override;
	void Configure(uint32_t maxRecursion, uint32_t maxPayloadSizeInBytes, uint32_t maxAttributeSizeInBytes) override;
	void AddHitGroup(const std::string& name, const std::string& chs, const std::string& ahs) override;
	void AddShader(const std::string& shader, RTPipelineStateObject::ShaderType shaderType) override;
	void BindUAV(const std::string& shader, const RHIBindingDesc& binding) override;
	void BindSRV(const std::string& shader, const RHIBindingDesc& binding) override;
	void BindSampler(const std::string& shader, const RHIBindingDesc& binding) override;
	void BindCBV(const std::string& shader, const RHIBindingDesc& binding) override;
	void BindUAV(const std::string& shader, const std::string& name, uint32_t baseRegister) override;
	void BindSRV(const std::string& shader, const std::string& name, uint32_t baseRegister) override;
	void BindSampler(const std::string& shader, const std::string& name, uint32_t baseRegister) override;
	void BindCBV(const std::string& shader, const std::string& name, uint32_t baseRegister, uint32_t size, uint32_t numInstance) override;
	void SetShaderDefine(const std::string& name, const std::string& value) override;
	void SetShaderLibraryTarget(const std::string& target) override;
	bool IsHitProgramBindingCacheValid(uint32_t numInstances, uint64_t signature) const override;
	void MarkHitProgramBindingCacheDirty() override;
	void MarkHitProgramBindingCacheValid(uint32_t numInstances, uint64_t signature) override;
	void BeginShaderTable() override;
	void EndShaderTable() override;
	void SetTextureUAV(const std::string& shader, const std::string& bindingName, Texture* texture, int instanceIndex = -1) override;
	void SetBufferUAV(const std::string& shader, const std::string& bindingName, Buffer* buffer, int instanceIndex = -1) override;
	void SetTextureSRV(const std::string& shader, const std::string& bindingName, Texture* texture, int instanceIndex = -1) override;
	void SetBufferSRV(const std::string& shader, const std::string& bindingName, Buffer* buffer, int instanceIndex = -1) override;
	bool SetBindlessTextureTable(const std::string& shader, const std::string& bindingName) override;
	bool SetBindlessBufferTable(const std::string& shader, const std::string& bindingName) override;
	void SetAccelerationStructure(const std::string& shader, const std::string& bindingName, const std::shared_ptr<RTAS>& rtas, int instanceIndex = -1) override;
	void SetSampler(const std::string& shader, const std::string& bindingName, Sampler* sampler, int instanceIndex = -1) override;
	void SetCBVValue(const std::string& shader, const std::string& bindingName, void* pData, int instanceIndex = -1) override;
	void ResetHitProgram(uint32_t instanceIndex) override;
	void StartHitProgram(const std::string& hitGroup, uint32_t instanceIndex) override;
	bool InitRS(const std::string& shaderFile) override;
	void Apply(uint32_t width, uint32_t height) override;
	bool GetDispatchRaysIndirectTemplate(uint32_t width, uint32_t height, RtDispatchRaysIndirectTemplate& outTemplate) const override;
	bool ApplyIndirect(Buffer* indirectArgumentBuffer, uint64_t byteOffset) override;

	void SetGlobalBinding(CommandList* CommandList = nullptr);
	void SetUAVHandle(const std::string& shader, const std::string& bindingName, D3D12_GPU_DESCRIPTOR_HANDLE uavHandle, INT instanceIndex = -1);
	void SetSRVHandle(const std::string& shader, const std::string& bindingName, D3D12_GPU_DESCRIPTOR_HANDLE srvHandle, INT instanceIndex = -1);
	bool BuildDispatchRaysDesc(uint32_t width, uint32_t height, D3D12_DISPATCH_RAYS_DESC& outDesc) const;
	bool EnsureDispatchRaysCommandSignature();
};

// Buffer / IndexBuffer / VertexBuffer / Sampler / Texture are defined in
// RenderResources.h. Their DX12-specific members are gated by CORONA_HAS_D3D12
// and only this backend builds method bodies for them.

class DescriptorHeap
{
public:
	ID3D12Device5* Device = nullptr;
	D3D12_DESCRIPTOR_HEAP_DESC HeapDesc;
	ComPtr<ID3D12DescriptorHeap> DH;
	UINT DescriptorSize = 0;

	UINT64 CPUHeapStart;
	UINT64 GPUHeapStart;

	UINT NumAllocated = 0;
	UINT MaxNumDescriptors = 0;
	bool bShaderVisible = true;
public:
	DescriptorHeap()
	{
	}
	void Init(ID3D12Device5* InDevice, D3D12_DESCRIPTOR_HEAP_DESC& InHeapDesc);

	void AllocDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE& cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE& gpuHandle);

	void AllocDescriptors(D3D12_CPU_DESCRIPTOR_HANDLE& cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE& gpuHandle, UINT num);
};

// allocate region of descriptors from descriptor heap. (numDescriptors * numFrame)
class DescriptorHeapRing
{
public:
	DescriptorHeap* DHeap = nullptr;

	D3D12_CPU_DESCRIPTOR_HANDLE CPUHeapStart;
	D3D12_GPU_DESCRIPTOR_HANDLE GPUHeapStart;
	UINT NumFrame = 0;
	UINT CurrentFrame = 0;
	UINT NumDescriptors = 0;
	UINT NumAllocated = 0;
	UINT DescriptorSize;

public:
	void Init(DescriptorHeap* InDHHeap, UINT InNumDescriptors, UINT InNumFrame);
	void AllocDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE& cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE& gpuHandle);
	void Advance();

	DescriptorHeapRing(){}
	virtual ~DescriptorHeapRing() {}
};


class ConstantBufferRingBuffer
{
	ID3D12Device5* Device = nullptr;
	UINT NumFrame = 0;
	UINT CurrentFrame = 0;
	UINT TotalSize;

	UINT AllocPos = 0;
	
	void* MemMapped = nullptr;

	ComPtr<ID3D12Resource> CBMem = nullptr;

public:

	std::tuple<UINT64, UINT8*> AllocGPUMemory(UINT InSize);
	void Advance();

	ConstantBufferRingBuffer(ID3D12Device5* InDevice, UINT InSize, UINT InNumFrame);
	virtual ~ConstantBufferRingBuffer();
};

//class ThreadDescriptorHeapPool
//{
//public:
//	DescriptorHeap* DHeap = nullptr;
//	UINT PoolSize = 0;
//	UINT StartIndex = 0;
//	UINT PoolIndex = 0;
//
//	// Descriptor in pool is for one thread only, so that it is not overwritten.
//	void AllocPool(UINT InPoolSize)
//	{
//		PoolSize = InPoolSize;
//		
//		if (DHeap->NumAllocated + PoolSize >= DHeap->MaxNumDescriptors)
//			DHeap->NumAllocated = 0;
//
//		PoolIndex = StartIndex = DHeap->NumAllocated;
//		DHeap->NumAllocated += PoolSize;
//	}
//
//	void AllocDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE& cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE& gpuHandle);
//
//	ThreadDescriptorHeapPool();
//	~ThreadDescriptorHeapPool() {}
//};

class D3D12RTAS : public RTAS
{
public:
	D3D12_GPU_DESCRIPTOR_HANDLE GPUHandle = {};
	D3D12_CPU_DESCRIPTOR_HANDLE CPUHandle = {};

	ComPtr<ID3D12Resource> Scratch;
	ComPtr<ID3D12Resource> Result;
	ComPtr<ID3D12Resource> Instance;
	UINT NumInstances = 0;
};

class DX12Backend : public IRenderBackend
{
public:
	friend class DescriptorHeap;

	const uint32_t NumFrame = 3;

	uint32_t CurrentFrameIndex = 0;

	ComPtr<ID3D12Device5> Device;

	unique_ptr<CommandQueue> CmdQ;
	unique_ptr<CommandQueue> AsyncRtCmdQ;
	CommandList* GlobalCmdList = nullptr;
	CommandList* ActiveAsyncRtCmdList = nullptr;
	UINT64 PendingAsyncRtFenceValue = 0;

	vector<UINT32> FrameFenceValueVec;

	std::unique_ptr<DescriptorHeap> RTVDescriptorHeap;
	std::unique_ptr<DescriptorHeap> DSVDescriptorHeap;
	std::unique_ptr<DescriptorHeap> SamplerDescriptorHeapShaderVisible;
	std::unique_ptr<DescriptorHeap> SRVCBVDescriptorHeapShaderVisible;
	std::unique_ptr<DescriptorHeap> SRVCBVDescriptorHeapStorage;

	std::unique_ptr<DescriptorHeapRing> GlobalDHRing; // resources that changes every frame.
	std::unique_ptr<DescriptorHeapRing> TextureDHRing;
	std::unique_ptr<DescriptorHeapRing> GeomtryDHRing;

	// Phase 3.5: large UPLOAD-heap blocks that CreateUploadVertexBuffer /
	// CreateUploadIndexBuffer sub-allocate from. Each block is a single
	// CreateCommittedResource + persistent Map; sub-allocs are returned as
	// shared ComPtrs to the block's resource + offset/CPU pointer. Block
	// lifetime is extended by every issued VB/IB ComPtr — the active block
	// pointer here only tracks the *current bump target*; retired blocks
	// stay alive automatically until their last sub-allocation dies.
	struct UploadHeapBlock
	{
		ComPtr<ID3D12Resource> resource;
		uint8_t* mappedBase = nullptr;
		UINT64 gpuVA = 0;
		UINT64 capacity = 0;
		UINT64 cursor = 0;
	};
	std::shared_ptr<UploadHeapBlock> ActiveUploadBlock;
	UINT64 UploadBlockDefaultSize = 16ull * 1024ull * 1024ull; // 16 MiB
	UINT32 UploadBlockCount = 0;
	UINT32 UploadAllocationCount = 0;
	UINT64 UploadBytesIssued = 0;
	UINT64 UploadBytesReserved = 0;

	struct UploadAllocation
	{
		ComPtr<ID3D12Resource> resource;
		UINT64 offset = 0;
		void* cpu = nullptr;
		UINT64 gpuVA = 0;
	};
	UploadAllocation AllocateUploadBytes(UINT64 size, UINT64 alignment);

	struct TransientUploadStructuredBlock
	{
		ComPtr<ID3D12Resource> resource;
		uint8_t* mappedBase = nullptr;
		UINT64 gpuVA = 0;
		UINT64 capacity = 0;
		UINT64 cursor = 0;
	};
	struct TransientUploadStructuredAllocation
	{
		ComPtr<ID3D12Resource> resource;
		UINT64 offset = 0;
		void* cpu = nullptr;
		UINT64 gpuVA = 0;
	};
	std::vector<std::vector<std::shared_ptr<TransientUploadStructuredBlock>>> TransientUploadStructuredBlocks;
	std::vector<std::vector<std::shared_ptr<Buffer>>> TransientUploadStructuredKeepAlive;
	UINT64 TransientUploadStructuredBlockDefaultSize = 4ull * 1024ull * 1024ull;
	UINT32 TransientUploadStructuredBlockCount = 0;
	UINT32 TransientUploadStructuredAllocationCount = 0;
	UINT64 TransientUploadStructuredBytesIssued = 0;
	UINT64 TransientUploadStructuredBytesReserved = 0;
	void ResetTransientUploadStructuredFrame(uint32_t frameIndex);
	TransientUploadStructuredAllocation AllocateTransientUploadStructuredBytes(UINT64 size, UINT64 alignment);

	struct PersistentStructuredBufferBlock
	{
		ComPtr<ID3D12Resource> resource;
		UINT64 capacity = 0;
		UINT64 cursor = 0;
		D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
		struct FreeRange
		{
			UINT64 offset = 0;
			UINT64 size = 0;
		};
		std::vector<FreeRange> freeRanges;
	};
	struct PersistentStructuredBufferAllocation
	{
		ComPtr<ID3D12Resource> resource;
		std::shared_ptr<PersistentStructuredBufferBlock> block;
		UINT64 offset = 0;
		UINT64 size = 0;
	};
	struct PendingPersistentStructuredBufferFree
	{
		std::shared_ptr<PersistentStructuredBufferBlock> block;
		UINT64 offset = 0;
		UINT64 size = 0;
		UINT64 fenceValue = 0;
	};
	std::vector<std::shared_ptr<PersistentStructuredBufferBlock>> PersistentStructuredBufferBlocks;
	std::vector<PendingPersistentStructuredBufferFree> PendingPersistentStructuredBufferFrees;
	UINT64 PersistentStructuredBufferBlockDefaultSize = 16ull * 1024ull * 1024ull;
	UINT32 PersistentStructuredBufferBlockCount = 0;
	UINT32 PersistentStructuredBufferAllocationCount = 0;
	UINT64 PersistentStructuredBufferBytesIssued = 0;
	UINT64 PersistentStructuredBufferBytesReserved = 0;
	PersistentStructuredBufferAllocation AllocatePersistentStructuredBufferBytes(UINT64 size, UINT64 alignment);
	void ReleasePersistentStructuredBufferBytes(
		const std::shared_ptr<PersistentStructuredBufferBlock>& block,
		UINT64 offset,
		UINT64 size);
	void RetireCompletedPersistentStructuredBufferFrees();
	void AddPersistentStructuredBufferFreeRange(
		const std::shared_ptr<PersistentStructuredBufferBlock>& block,
		UINT64 offset,
		UINT64 size);
	std::shared_ptr<Buffer> CreateSuballocatedStructuredBuffer(const BufferCreateDesc& desc);

	std::unique_ptr<ConstantBufferRingBuffer> GlobalCBRing;

	std::vector<std::shared_ptr<Texture>> renderTargetTextures;
	std::list<std::shared_ptr<Texture>> DynamicTextures;
	std::list<std::shared_ptr<Buffer>> DynamicBuffers;

	// Texture-streaming Stage 1: non-blocking uploads. CreateTextureFromFile no
	// longer stalls (WaitGPU) per texture; the staging upload heap is parked
	// here with the submission fence and freed once the GPU copy completes.
	// Because every copy and every later sampling run on the same ordered queue,
	// the copy is guaranteed to finish before the texture is ever read, so this
	// is correct without per-texture synchronization. A byte budget caps how
	// much staging memory can be in flight before we drain.
	struct PendingTextureUpload
	{
		UINT64 FenceValue = 0;
		UINT64 Bytes = 0;
		Microsoft::WRL::ComPtr<ID3D12Resource> UploadHeap;
	};
	std::vector<PendingTextureUpload> PendingTextureUploads;
	UINT64 PendingTextureUploadBytes = 0;
	static constexpr UINT64 kMaxInFlightTextureUploadBytes = 256ull * 1024ull * 1024ull;
	void RetireCompletedTextureUploads();


	ComPtr<IDXGISwapChain3> m_swapChain;
	bool bTearingSupported = false;
	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleImguiFontTex{};
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleImguiFontTex{};
	ComPtr<ID3D12QueryHeap> GpuTimestampQueryHeap;
	ComPtr<ID3D12Resource> GpuTimestampReadbackBuffer;
	UINT64* GpuTimestampReadbackMapped = nullptr;
	uint32_t GpuTimestampQueryCount = 0;
	ComPtr<ID3D12QueryHeap> OcclusionQueryHeap;
	ComPtr<ID3D12Resource> OcclusionReadbackBuffer;
	UINT64* OcclusionReadbackMapped = nullptr;
	uint32_t OcclusionQueryCount = 0;

	string errorString;
	CoronaBvhViewerD3D12Handle* BvhViewerD3D12 = nullptr;
	bool bBvhViewerD3D12Allowed = true;

	static constexpr uint32_t kMaxDX12BindlessTextureSlots = 65536;
	struct DX12BindlessTextureSlot
	{
		Texture* TexturePtr = nullptr;
		uint32_t Generation = 1;
		bool Occupied = false;
		D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleSRV{};
		D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleSRV{};
	};
	D3D12_CPU_DESCRIPTOR_HANDLE BindlessTextureTableCpuBase{};
	D3D12_GPU_DESCRIPTOR_HANDLE BindlessTextureTableGpuBase{};
	bool bBindlessTextureTableAllocated = false;
	std::vector<DX12BindlessTextureSlot> BindlessTextureSlots;
	std::vector<uint32_t> BindlessTextureFreeList;
	mutable std::mutex BindlessTextureMutex;

	static constexpr uint32_t kMaxDX12BindlessBufferSlots = 65536;
	struct DX12BindlessBufferSlot
	{
		const void* BufferPtr = nullptr;
		uint8_t ResourceType = 0;
		uint32_t Generation = 1;
		bool Occupied = false;
		D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleSRV{};
		D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleSRV{};
	};
	D3D12_CPU_DESCRIPTOR_HANDLE BindlessBufferTableCpuBase{};
	D3D12_GPU_DESCRIPTOR_HANDLE BindlessBufferTableGpuBase{};
	bool bBindlessBufferTableAllocated = false;
	std::vector<DX12BindlessBufferSlot> BindlessBufferSlots;
	std::vector<uint32_t> BindlessBufferFreeList;
	mutable std::mutex BindlessBufferMutex;

#if USE_AFTERMATH
	bool bAftermathEnabled = false;
#endif
public:
	ERenderBackendAPI GetAPI() const override { return ERenderBackendAPI::D3D12; }
	const char* GetBackendName() const override { return "Direct3D 12"; }
	RenderBackendCapabilities GetCapabilities() const override
	{
		RenderBackendCapabilities capabilities{};
		capabilities.SupportsTypedBindingSchema = true;
		capabilities.SupportsBindlessTextures = true;
		capabilities.SupportsBindlessBuffers = true;
		capabilities.SupportsRuntimeDescriptorArrays = true;
		capabilities.SupportsPartiallyBoundDescriptors = true;
		capabilities.MaxBindlessTextureCount = kMaxDX12BindlessTextureSlots;
		capabilities.MaxBindlessBufferCount = kMaxDX12BindlessBufferSlots;
		return capabilities;
	}
	bool GetStreamlineTextureResource(Texture* texture, EResourceState state, StreamlineTextureResourceDesc& outDesc) const override;
	void* GetStreamlineCommandBuffer() override;
	uint32_t GetMaxSupportedHybridStage() const override { return 7; }
	bool SupportsRayTracing() const override;
	bool SupportsShaderExecutionReordering() const override;
	void BeginFrame() override;
	void EndFrame() override;
	void WaitForGpu() override { CmdQ->WaitGPU(); }
	void EmitGpuCrashMarker(const char* markerName) override;
	const std::string& GetErrorString() const override { return errorString; }
	void ClearErrorString() override { errorString.clear(); }
	uint64_t GetTimestampFrequency() const override { UINT64 frequency = 0; CmdQ->CmdQueue->GetTimestampFrequency(&frequency); return frequency; }
	uint32_t GetFrameCount() const override { return NumFrame; }
	uint32_t GetCurrentFrameIndex() const override { return CurrentFrameIndex; }
	DX12Backend* AsDX12Backend() override { return this; }
	bool IsBvhViewerD3D12Available() const;
	bool IsBvhViewerD3D12Allowed() const { return bBvhViewerD3D12Allowed; }
	void SetBvhViewerD3D12Allowed(bool allowed);
	bool IsBvhViewerD3D12WindowVisible() const;
	bool ShowBvhViewerD3D12Window(uint32_t width = 1280, uint32_t height = 720);
	void HideBvhViewerD3D12Window();
	std::shared_ptr<Texture> CreateTexture2D(const TextureCreateDesc& desc) override;
	std::shared_ptr<Buffer> CreateBuffer(const BufferCreateDesc& desc) override;
	std::shared_ptr<Sampler> CreateSampler(const SamplerCreateDesc& desc) override;
	std::shared_ptr<Texture> CreateTextureFromFile(const std::wstring& fileName, bool nonSRGB) override;
	RHITextureHandle RegisterBindlessTexture(Texture* texture) override;
	bool UpdateBindlessTexture(Texture* texture) override;
	void UnregisterBindlessTexture(Texture* texture) override;
	RHITextureHandle GetBindlessTextureHandle(const Texture* texture) const override;
	bool IsBindlessTextureTableReady() const { return bBindlessTextureTableAllocated; }
	D3D12_GPU_DESCRIPTOR_HANDLE GetBindlessTextureTableGpuHandle() const { return BindlessTextureTableGpuBase; }
	RHIBufferHandle RegisterBindlessBuffer(Buffer* buffer) override;
	RHIBufferHandle RegisterBindlessVertexBuffer(VertexBuffer* buffer) override;
	RHIBufferHandle RegisterBindlessIndexBuffer(IndexBuffer* buffer) override;
	void UnregisterBindlessBuffer(Buffer* buffer) override;
	void UnregisterBindlessVertexBuffer(VertexBuffer* buffer) override;
	void UnregisterBindlessIndexBuffer(IndexBuffer* buffer) override;
	RHIBufferHandle GetBindlessBufferHandle(const Buffer* buffer) const override;
	RHIBufferHandle GetBindlessVertexBufferHandle(const VertexBuffer* buffer) const override;
	RHIBufferHandle GetBindlessIndexBufferHandle(const IndexBuffer* buffer) const override;
	bool IsBindlessBufferTableReady() const { return bBindlessBufferTableAllocated; }
	D3D12_GPU_DESCRIPTOR_HANDLE GetBindlessBufferTableGpuHandle() const { return BindlessBufferTableGpuBase; }
	std::shared_ptr<Texture> WrapNativeTexture(const Microsoft::WRL::ComPtr<ID3D12Resource>& resource);
	std::shared_ptr<Texture> CreateTexture3D(ETextureFormat format, ETextureUsageFlags usage, EInitialResourceState initialState, int width, int height, int depth, int mipLevels) override;
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
	std::shared_ptr<Buffer> AllocateTransientUploadStructuredBuffer(uint32_t numElements, uint32_t elementSize, const void* srcData) override;
	std::shared_ptr<RTAS> CreateBLASForMesh(Mesh* mesh) override;
	std::shared_ptr<RTAS> CreateBLASForSkeletalMesh(Mesh* mesh) override;
	void RefitBLAS(RTAS* rtas, Mesh* mesh) override;
	std::shared_ptr<RTAS> CreateTLAS(const std::vector<RTInstanceDesc>& instances) override;
	bool UpdateTLAS(const std::shared_ptr<RTAS>& topLevelAS, const std::vector<RTInstanceDesc>& instances) override;
	std::shared_ptr<RTPipelineStateObject> CreateRTPipelineStateObject() override;
	std::shared_ptr<ComputePipelineStateObject> CreateComputePipelineStateObject() override;
	ShaderBytecode CreateShader(const std::wstring& fileName, const std::string& entryPoint, const std::string& target) override;
	void ResetDynamicResources() override { DynamicTextures.clear(); DynamicBuffers.clear(); }
	void ForgetDynamicTexture(Texture* texture) { if (texture) DynamicTextures.remove_if([texture](const std::shared_ptr<Texture>& entry) { return entry.get() == texture; }); }
	void ForgetDynamicBuffer(Buffer* buffer) { if (buffer) DynamicBuffers.remove_if([buffer](const std::shared_ptr<Buffer>& entry) { return entry.get() == buffer; }); }
	void CreateSwapChainForWindow(WindowHandle window, uint32_t width, uint32_t height, ETextureFormat format) override;
	std::shared_ptr<Texture> GetSwapChainTexture(uint32_t bufferIndex) override;
	bool CaptureTexture(Texture* source, CapturedImage& captured, EResourceState beforeState) override;
	void InitializeImGuiBackend(WindowHandle window, ETextureFormat rtvFormat) override;
	void SetExternalDXGIFactory(IDXGIFactory4* factory) { ExternalDXGIFactory = factory; }
	void NewImGuiFrame() override;
	void RenderImGuiDrawData(ImDrawData* drawData) override;
	void ShutdownImGuiBackend() override;
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
	void SetRenderTarget(Texture* colorTarget, Texture* depthTarget = nullptr) override;
	void SetRenderTargets(Texture* const* colorTargets, uint32_t colorTargetCount, Texture* depthTarget = nullptr) override;
	void ClearRenderTarget(Texture* colorTarget, const float clearColor[4]) override;
	void ClearDepth(Texture* depthTarget, float depthValue) override;
	void BindDefaultDescriptorHeaps() override;
	void SetViewportAndScissor(uint32_t width, uint32_t height) override;
	void DrawFullscreenQuad(VertexBuffer* vertexBuffer) override;
	void BindMeshBuffers(VertexBuffer* vertexBuffer, IndexBuffer* indexBuffer) override;
	void DrawIndexed(uint32_t indexCount, uint32_t startIndexLocation, int32_t baseVertexLocation) override;
	void DrawIndexedInstanced(uint32_t indexCountPerInstance, uint32_t instanceCount, uint32_t startIndexLocation, int32_t baseVertexLocation, uint32_t startInstanceLocation) override;
	void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override;
	void ClearTextureUAVFloat(Texture* texture, const float clearColor[4]) override;
	void ExecuteCurrentCommandList() override;
	void BeginNewGraphicsCommandList();
	UINT64 SubmitCurrentCommandList();
	UINT64 SubmitCurrentCommandListAndRestart();
	bool BeginAsyncRtRecordingAfterGraphicsSubmit();
	UINT64 EndAsyncRtRecordingAndResumeGraphics();
	bool HasPendingAsyncRtWork() const { return PendingAsyncRtFenceValue != 0; }
	void SubmitGraphicsWorkAndWaitForAsyncRt();
	void WaitForAsyncRtOnGraphicsQueue();
	void BeginGpuMarker(uint64_t color, const char* label) override;
	void EndGpuMarker() override;
	ID3D12GraphicsCommandList* GetGraphicsCommandList() { return GlobalCmdList ? GlobalCmdList->CmdList.Get() : nullptr; }
	void TransitionTexture(Texture* texture, EResourceState stateBefore, EResourceState stateAfter) override;
	void TransitionBuffer(Buffer* buffer, EResourceState stateBefore, EResourceState stateAfter) override;
	void TransitionVertexBuffer(VertexBuffer* vertexBuffer, EResourceState stateBefore, EResourceState stateAfter) override;
	void UAVBarrier(Buffer* buffer) override;
	Texture* GetCurrentWindowRenderTarget() override;
	void PrepareWindowRenderTarget(Texture* renderTarget) override;
	void FinalizeWindowRenderTarget(Texture* renderTarget) override;
	void RequestWindowCapture(const std::wstring& outputPath) override;
	bool ConsumeWindowCaptureResult(std::wstring* outputPath, bool* success, std::wstring* errorMessage) override;
	std::shared_ptr<GraphicsPipelineHandle> CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) override;
	std::shared_ptr<GraphicsBindGroupHandle> CreateGraphicsBindGroup(const GraphicsBindGroupDesc& desc) override;
	void BindGraphicsPipeline(GraphicsPipelineHandle* pipeline) override;
	void BindGraphicsBindGroup(GraphicsPipelineHandle* pipeline, uint32_t slot, const std::shared_ptr<GraphicsBindGroupHandle>& bindGroup) override;
	void DrawTriangleList(VertexBuffer* vertexBuffer, uint32_t vertexCount);
	void RenderWindowTriangleFrame(uint32_t width, uint32_t height, float timeSeconds);

	shared_ptr<Texture> CreateTexture2DFromResource(ComPtr<ID3D12Resource> InResource);
	shared_ptr<Texture> CreateTexture2D(DXGI_FORMAT format, D3D12_RESOURCE_FLAGS resFlags, D3D12_RESOURCE_STATES initResState, int width, int height, int mipLevels, std::optional<glm::vec4> clearColor = std::nullopt);
	shared_ptr<Texture> CreateTexture3D(DXGI_FORMAT format, D3D12_RESOURCE_FLAGS resFlags, D3D12_RESOURCE_STATES initResState, int width, int height, int depth, int mipLevels);

	shared_ptr<Sampler> CreateSampler(D3D12_SAMPLER_DESC& InSamplerDesc);
	shared_ptr<Buffer> CreateBuffer(UINT InNumElements, UINT InElementSize, D3D12_RESOURCE_STATES initResState, bool isUAV, void* SrcData = nullptr, EBufferAccess access = EBufferAccess::GpuOnly);
	shared_ptr<Buffer> CreateDefaultByteAddressBuffer(UINT InNumElements, UINT InElementSize, EInitialResourceState initialState = EInitialResourceState::ShaderRead);
	bool UploadToDefaultBuffer(Buffer* buffer, const void* srcData, UINT sizeInBytes, EResourceState stateBefore, EResourceState stateAfter);

	std::vector<std::shared_ptr<Texture>> SwapChainRenderTargets;
	std::vector<std::shared_ptr<Texture>> SwapChainWrappedTextures;
	IDXGIFactory4* ExternalDXGIFactory = nullptr;
	std::wstring PendingWindowCapturePath;
	std::wstring LastWindowCapturePath;
	std::wstring LastWindowCaptureError;
	bool bLastWindowCaptureResultValid = false;
	bool bLastWindowCaptureSucceeded = false;

	// Used by BindMeshBuffers to validate VB stride against the currently
	// bound PSO. DX12 itself reads stride from VBV (so it would still
	// render correctly), but a mismatch indicates the same code will
	// silently break on Vulkan. Non-owning — caller owns the handle.
	GraphicsPipelineHandle* BoundGraphicsPipelineForDiag = nullptr;
	GraphicsPipelineHandle* BoundGraphicsPipeline = nullptr;
	VertexBuffer* BoundVertexBuffer = nullptr;
	IndexBuffer* BoundIndexBuffer = nullptr;
	D3D12_PRIMITIVE_TOPOLOGY BoundPrimitiveTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;

	void InvalidateGraphicsCommandStateCache();
	void PresentBarrier(Texture* rt);
	void ResourceBarrier(ID3D12Resource* Resource, D3D12_RESOURCE_STATES StateBefore, D3D12_RESOURCE_STATES StateAfter);

	DX12Backend(ComPtr<ID3D12Device5> pDevice);
	virtual ~DX12Backend();
};
