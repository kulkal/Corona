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

#if _DEBUG
#define DEBUG_CLIENTBLOCK   new( _CLIENT_BLOCK, __FILE__, __LINE__)
#else
#define DEBUG_CLIENTBLOCK
#endif // _DEBUG

#include "DXSampleHelper.h"
#include "RenderBackend.h"

#define USE_AFTERMATH 0

using namespace Microsoft::WRL;
using namespace std;

class SimpleDX12;
class Texture;
class Sampler;
//class ThreadDescriptorHeapPool;

class CommandList
{
public:
	ComPtr<ID3D12GraphicsCommandList4> CmdList;
	ComPtr<ID3D12CommandAllocator> CmdAllocator;
	std::optional<UINT64> Fence;

public:
	void Reset();
};

class CommandQueue
{
public:
	const UINT32 CommandListPoolSize = 4096;

	ComPtr<ID3D12CommandQueue> CmdQueue;

	std::vector<shared_ptr<CommandList>> CommandListPool;
	UINT32 CurrentIndex = 0;

	std::mutex CmdAllocMtx;

	HANDLE m_fenceEvent;
	ComPtr<ID3D12Fence> m_fence;
	UINT64 CurrentFenceValue = 2;
public:
	CommandQueue(ID3D12Device5* device);
	virtual ~CommandQueue();

	CommandList* AllocCmdList();

	void ExecuteCommandList(CommandList* cmd);

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
	SimpleDX12* Owner = nullptr;
	bool IsCompute = false;
	struct BindingData
	{
		string name;
		UINT rootParamIndex;
		UINT baseRegister;
		UINT numDescriptors;
		UINT cbSize;

		Texture* texture;
		Sampler* sampler;

		UINT rootConst;
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

	ComPtr<ID3DBlob> vs;
	ComPtr<ID3DBlob> ps;
	ComPtr<ID3DBlob> cs;




	ComPtr<ID3D12RootSignature> RS;
	ComPtr<ID3D12PipelineState> PSO;

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
	SimpleDX12* Owner = nullptr;
	std::shared_ptr<PipelineStateObject> PSO;
	std::map<std::string, D3D12_GPU_DESCRIPTOR_HANDLE> PendingSRVs;
	std::map<std::string, D3D12_GPU_DESCRIPTOR_HANDLE> PendingUAVs;
	std::map<std::string, Sampler*> PendingSamplers;
	std::map<std::string, std::vector<uint8_t>> PendingCBVs;

	void BindSRV(const std::string& name, uint32_t baseRegister, uint32_t numDescriptors) override;
	void BindUAV(const std::string& name, uint32_t baseRegister) override;
	void BindCBV(const std::string& name, uint32_t baseRegister, uint32_t size) override;
	void BindSampler(const std::string& name, uint32_t baseRegister) override;
	bool InitCS(const std::wstring& shaderFile, const std::string& entryPoint) override;
	void Apply() override;
	void SetTextureSRV(const std::string& name, Texture* texture) override;
	void SetTextureUAV(const std::string& name, Texture* texture) override;
	void SetBufferSRV(const std::string& name, Buffer* buffer) override;
	void SetBufferUAV(const std::string& name, Buffer* buffer) override;
	void SetSampler(const std::string& name, Sampler* sampler) override;
	void SetCBVValue(const std::string& name, void* pData) override;
};

class D3D12RTPipelineStateObject : public RTPipelineStateObject
{
public:
	SimpleDX12* Owner = nullptr;
private:
	struct BindingData
	{
		D3D12_DESCRIPTOR_RANGE_TYPE Type;
		string name;
		UINT cbSize;

		Texture* texture;
		Sampler* sampler;

		UINT BaseRegister;
		D3D12_CPU_DESCRIPTOR_HANDLE CPUHandle; // for multiple instances
		D3D12_GPU_DESCRIPTOR_HANDLE GPUHandle; // for multiple instances
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

	uint32_t ShaderTableEntrySize = 0;
	UINT ShaderTableSize = 0;
	ComPtr<ID3D12Resource> ShaderTable;

	UINT NumInstance = 0;

	void SetNumInstances(uint32_t numInstances) override;
	void Configure(uint32_t maxRecursion, uint32_t maxPayloadSizeInBytes, uint32_t maxAttributeSizeInBytes) override;
	void AddHitGroup(const std::string& name, const std::string& chs, const std::string& ahs) override;
	void AddShader(const std::string& shader, RTPipelineStateObject::ShaderType shaderType) override;
	void BindUAV(const std::string& shader, const std::string& name, uint32_t baseRegister) override;
	void BindSRV(const std::string& shader, const std::string& name, uint32_t baseRegister) override;
	void BindSampler(const std::string& shader, const std::string& name, uint32_t baseRegister) override;
	void BindCBV(const std::string& shader, const std::string& name, uint32_t baseRegister, uint32_t size, uint32_t numInstance) override;
	void BeginShaderTable() override;
	void EndShaderTable() override;
	void SetTextureUAV(const std::string& shader, const std::string& bindingName, Texture* texture, int instanceIndex = -1) override;
	void SetBufferUAV(const std::string& shader, const std::string& bindingName, Buffer* buffer, int instanceIndex = -1) override;
	void SetTextureSRV(const std::string& shader, const std::string& bindingName, Texture* texture, int instanceIndex = -1) override;
	void SetBufferSRV(const std::string& shader, const std::string& bindingName, Buffer* buffer, int instanceIndex = -1) override;
	void SetAccelerationStructure(const std::string& shader, const std::string& bindingName, const std::shared_ptr<RTAS>& rtas, int instanceIndex = -1) override;
	void SetSampler(const std::string& shader, const std::string& bindingName, Sampler* sampler, int instanceIndex = -1) override;
	void SetCBVValue(const std::string& shader, const std::string& bindingName, void* pData, int instanceIndex = -1) override;
	void ResetHitProgram(uint32_t instanceIndex) override;
	void StartHitProgram(const std::string& hitGroup, uint32_t instanceIndex) override;
	void AddTextureSRVToHitProgram(const std::string& hitGroup, Texture* texture, uint32_t instanceIndex) override;
	void AddBufferSRVToHitProgram(const std::string& hitGroup, Buffer* buffer, uint32_t instanceIndex) override;
	void AddVertexBufferSRVToHitProgram(const std::string& hitGroup, VertexBuffer* buffer, uint32_t instanceIndex) override;
	void AddIndexBufferSRVToHitProgram(const std::string& hitGroup, IndexBuffer* buffer, uint32_t instanceIndex) override;
	bool InitRS(const std::string& shaderFile) override;
	void Apply(uint32_t width, uint32_t height) override;

	void SetGlobalBinding(CommandList* CommandList = nullptr);
	void SetUAVHandle(const std::string& shader, const std::string& bindingName, D3D12_GPU_DESCRIPTOR_HANDLE uavHandle, INT instanceIndex = -1);
	void SetSRVHandle(const std::string& shader, const std::string& bindingName, D3D12_GPU_DESCRIPTOR_HANDLE srvHandle, INT instanceIndex = -1);
	void AddDescriptor2HitProgram(const std::string& hitGroup, D3D12_GPU_DESCRIPTOR_HANDLE srvHandle, UINT instanceIndex);
};

class Buffer
{
public:
	SimpleDX12* Owner = nullptr;
	enum BufferType
	{
		BYTE_ADDRESS,
		STRUCTURED,
		UNKNOWN,
	};
	BufferType Type = UNKNOWN;
	UINT NumElements;
	UINT ElementSize;
	ComPtr<ID3D12Resource> resource;
	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleSRV;
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleSRV;

	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleUAV;
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleUAV;

	void MakeByteAddressBufferSRV();
	void MakeStructuredBufferSRV();
};

class IndexBuffer
{
public:
	int numIndices;
	ComPtr<ID3D12Resource> resource;
	D3D12_INDEX_BUFFER_VIEW view;

	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleSRV;
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleSRV;
};

class VertexBuffer
{
public:
	int numVertices;
	ComPtr<ID3D12Resource> resource;
	D3D12_VERTEX_BUFFER_VIEW view;

	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleSRV;
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleSRV;
};

class Sampler
{
public:
	D3D12_SAMPLER_DESC SamplerDesc;
	ComPtr<ID3D12Resource> resource;
	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle;
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle;
};

class Texture 
{
public:
	SimpleDX12* Owner = nullptr;
	D3D12_RESOURCE_DESC textureDesc;

	ComPtr<ID3D12Resource> resource;

	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleUAV;
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleUAV;


	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleRTV;
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleRTV;

	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleDSV;
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleDSV;

	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleSRV;
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleSRV;

	void MakeStaticSRV();
	void MakeRTV(bool isBackBuffer = false);
	void MakeDSV();

	void UploadSRCData3D(D3D12_SUBRESOURCE_DATA* SrcData);
	Texture(){}
	~Texture()
	{
		int a = 0;
	}
};

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
};

class Material
{
public:
	bool bHasAlpha = false;

	shared_ptr<Texture> Diffuse;
	shared_ptr<Texture> Normal;
	shared_ptr<Texture> Roughness;
	shared_ptr<Texture> Metallic;
	Material() {}
	~Material()
	{
		int a = 0;
	}
};

class Mesh
{
public:
	IRenderBackend* Owner = nullptr;
	struct DrawCall
	{
		shared_ptr<Material> mat;
		INT DiffuseTextureIndex;
		INT NormalTextureIndex;
		INT SpecularTextureIndex;
		UINT IndexStart;
		UINT IndexCount;
		UINT VertexBase;
		UINT VertexCount;
	};
public:
	bool bTransparent = false;
	glm::mat4x4 transform;
	UINT NumIndices;
	UINT NumVertices;

	UINT VertexStride;
	UINT RtVertexOffset = 0;
	UINT RtIndexOffset = 0;

	DXGI_FORMAT IndexFormat = DXGI_FORMAT_R32_UINT;

	shared_ptr<IndexBuffer> Ib;
	shared_ptr<VertexBuffer> Vb;
	vector<shared_ptr<Texture>> Textures;

	shared_ptr<Material> Mat;

	vector<DrawCall> Draws;

	shared_ptr<RTAS> CreateBLAS();
};

class Scene
{
public:
	
	void SetTransform(glm::mat4x4 inTransform);

public:
	vector<shared_ptr<Mesh>> meshes;
	vector<shared_ptr<Material>> Materials;
	shared_ptr<VertexBuffer> RtSceneVertexBuffer;
	shared_ptr<IndexBuffer> RtSceneIndexBuffer;
public:
};

class SimpleDX12 : public IRenderBackend
{
public:
	friend class DescriptorHeap;

	const uint32_t NumFrame = 3;

	uint32_t CurrentFrameIndex = 0;

	ComPtr<ID3D12Device5> Device;

	unique_ptr<CommandQueue> CmdQ;
	CommandList* GlobalCmdList = nullptr;

	vector<UINT32> FrameFenceValueVec;

	std::unique_ptr<DescriptorHeap> RTVDescriptorHeap;
	std::unique_ptr<DescriptorHeap> DSVDescriptorHeap;
	std::unique_ptr<DescriptorHeap> SamplerDescriptorHeapShaderVisible;
	std::unique_ptr<DescriptorHeap> SRVCBVDescriptorHeapShaderVisible;
	std::unique_ptr<DescriptorHeap> SRVCBVDescriptorHeapStorage;

	std::unique_ptr<DescriptorHeapRing> GlobalDHRing; // resources that changes every frame.
	std::unique_ptr<DescriptorHeapRing> TextureDHRing;
	std::unique_ptr<DescriptorHeapRing> GeomtryDHRing;

	std::unique_ptr<ConstantBufferRingBuffer> GlobalCBRing;

	std::vector<std::shared_ptr<Texture>> renderTargetTextures;
	std::list<std::shared_ptr<Texture>> DynamicTextures;
	std::list<std::shared_ptr<Buffer>> DynamicBuffers;


	ComPtr<IDXGISwapChain3> m_swapChain;
	bool bTearingSupported = false;
	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleImguiFontTex{};
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleImguiFontTex{};
	ComPtr<ID3D12QueryHeap> GpuTimestampQueryHeap;
	ComPtr<ID3D12Resource> GpuTimestampReadbackBuffer;
	UINT64* GpuTimestampReadbackMapped = nullptr;
	uint32_t GpuTimestampQueryCount = 0;

	string errorString;

#if USE_AFTERMATH
	GFSDK_Aftermath_ContextHandle AM_CL_Handle;
#endif
public:
	ERenderBackendAPI GetAPI() const override { return ERenderBackendAPI::D3D12; }
	const char* GetBackendName() const override { return "Direct3D 12"; }
	uint32_t GetMaxSupportedHybridStage() const override { return 7; }
	void BeginFrame() override;
	void EndFrame() override;
	void WaitForGpu() override { CmdQ->WaitGPU(); }
	void EmitGpuCrashMarker(const char* markerName) override;
	const std::string& GetErrorString() const override { return errorString; }
	void ClearErrorString() override { errorString.clear(); }
	uint64_t GetTimestampFrequency() const override { UINT64 frequency = 0; CmdQ->CmdQueue->GetTimestampFrequency(&frequency); return frequency; }
	uint32_t GetFrameCount() const override { return NumFrame; }
	uint32_t GetCurrentFrameIndex() const override { return CurrentFrameIndex; }
	SimpleDX12* AsSimpleDX12() override { return this; }
	std::shared_ptr<Texture> CreateTexture2D(const TextureCreateDesc& desc) override;
	std::shared_ptr<Buffer> CreateBuffer(const BufferCreateDesc& desc) override;
	std::shared_ptr<Sampler> CreateSampler(const SamplerCreateDesc& desc) override;
	std::shared_ptr<Texture> CreateTextureFromFile(const std::wstring& fileName, bool nonSRGB) override;
	std::shared_ptr<Texture> WrapNativeTexture(const Microsoft::WRL::ComPtr<ID3D12Resource>& resource) override;
	std::shared_ptr<Texture> CreateTexture3D(ETextureFormat format, ETextureUsageFlags usage, EInitialResourceState initialState, int width, int height, int depth, int mipLevels) override;
	void UploadTexture3D(Texture* texture, const void* data, uint64_t rowPitch, uint64_t slicePitch) override;
	std::shared_ptr<VertexBuffer> CreateVertexBuffer(uint32_t size, uint32_t stride, void* srcData) override;
	std::shared_ptr<IndexBuffer> CreateIndexBuffer(DXGI_FORMAT format, uint32_t size, void* srcData) override;
	std::shared_ptr<RTAS> CreateBLASForMesh(Mesh* mesh) override;
	std::shared_ptr<RTAS> CreateTLAS(vector<shared_ptr<RTAS>>& VecBottomLevelAS) override;
	std::shared_ptr<RTPipelineStateObject> CreateRTPipelineStateObject() override;
	std::shared_ptr<ComputePipelineStateObject> CreateComputePipelineStateObject() override;
	Microsoft::WRL::ComPtr<ID3DBlob> CreateShader(const std::wstring& fileName, const std::string& entryPoint, const std::string& target) override;
	void ResetDynamicResources() override { DynamicTextures.clear(); DynamicBuffers.clear(); }
	void CreateSwapChainForWindow(IDXGIFactory4* factory, HWND hwnd, uint32_t width, uint32_t height, DXGI_FORMAT format) override;
	Microsoft::WRL::ComPtr<ID3D12Resource> GetSwapChainBuffer(uint32_t bufferIndex) override;
	HRESULT CaptureTexture(Texture* source, DirectX::ScratchImage& captured, D3D12_RESOURCE_STATES beforeState) override;
	void InitializeImGuiBackend(HWND hwnd, DXGI_FORMAT rtvFormat) override;
	void NewImGuiFrame() override;
	void RenderImGuiDrawData(ImDrawData* drawData) override;
	void ShutdownImGuiBackend() override;
	void InitializeGpuTimestampQueries(uint32_t queryCount) override;
	void ShutdownGpuTimestampQueries() override;
	void WriteGpuTimestamp(uint32_t queryIndex) override;
	void ResolveGpuTimestampRange(uint32_t startQueryIndex, uint32_t queryCount) override;
	uint64_t ReadGpuTimestampValue(uint32_t queryIndex) const override;
	void SetRenderTarget(Texture* colorTarget, Texture* depthTarget = nullptr) override;
	void SetRenderTargets(Texture* const* colorTargets, uint32_t colorTargetCount, Texture* depthTarget = nullptr) override;
	void ClearRenderTarget(Texture* colorTarget, const float clearColor[4]) override;
	void ClearDepth(Texture* depthTarget, float depthValue) override;
	void BindDefaultDescriptorHeaps() override;
	void SetViewportAndScissor(uint32_t width, uint32_t height) override;
	void DrawFullscreenQuad(VertexBuffer* vertexBuffer) override;
	void BindMeshBuffers(VertexBuffer* vertexBuffer, IndexBuffer* indexBuffer) override;
	void DrawIndexed(uint32_t indexCount, uint32_t startIndexLocation, int32_t baseVertexLocation) override;
	void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override;
	void ClearTextureUAVFloat(Texture* texture, const float clearColor[4]) override;
	void ExecuteCurrentCommandList() override;
	ID3D12GraphicsCommandList* GetGraphicsCommandList() override { return GlobalCmdList ? GlobalCmdList->CmdList.Get() : nullptr; }
	void TransitionTexture(Texture* texture, EResourceState stateBefore, EResourceState stateAfter) override;
	void TransitionBuffer(Buffer* buffer, EResourceState stateBefore, EResourceState stateAfter) override;
	Texture* GetCurrentWindowRenderTarget() override;
	void PrepareWindowRenderTarget(Texture* renderTarget) override;
	void FinalizeWindowRenderTarget(Texture* renderTarget) override;
	void RequestWindowCapture(const std::wstring& outputPath) override;
	bool ConsumeWindowCaptureResult(std::wstring* outputPath, bool* success, std::wstring* errorMessage) override;
	std::shared_ptr<GraphicsPipelineHandle> CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) override;
	void BindGraphicsPipeline(GraphicsPipelineHandle* pipeline) override;
	void SetGraphicsPipelineConstantData(GraphicsPipelineHandle* pipeline, uint32_t slot, const void* data, uint32_t size) override;
	void BindGraphicsPipelineTexture(GraphicsPipelineHandle* pipeline, const std::string& bindingName, Texture* texture) override;
	void BindGraphicsPipelineBuffer(GraphicsPipelineHandle* pipeline, const std::string& bindingName, Buffer* buffer) override;
	void BindGraphicsPipelineSampler(GraphicsPipelineHandle* pipeline, const std::string& bindingName, Sampler* sampler) override;
	void DrawTriangleList(VertexBuffer* vertexBuffer, uint32_t vertexCount);
	void RenderWindowTriangleFrame(uint32_t width, uint32_t height, float timeSeconds);

	shared_ptr<Texture> CreateTexture2DFromResource(ComPtr<ID3D12Resource> InResource);
	shared_ptr<Texture> CreateTexture2D(DXGI_FORMAT format, D3D12_RESOURCE_FLAGS resFlags, D3D12_RESOURCE_STATES initResState, int width, int height, int mipLevels, std::optional<glm::vec4> clearColor = std::nullopt);
	shared_ptr<Texture> CreateTexture3D(DXGI_FORMAT format, D3D12_RESOURCE_FLAGS resFlags, D3D12_RESOURCE_STATES initResState, int width, int height, int depth, int mipLevels);

	shared_ptr<Sampler> CreateSampler(D3D12_SAMPLER_DESC& InSamplerDesc);
	shared_ptr<Buffer> CreateBuffer(UINT InNumElements, UINT InElementSize, D3D12_RESOURCE_STATES initResState, bool isUAV, void* SrcData = nullptr);

	std::vector<std::shared_ptr<Texture>> SwapChainRenderTargets;
	std::wstring PendingWindowCapturePath;
	std::wstring LastWindowCapturePath;
	std::wstring LastWindowCaptureError;
	bool bLastWindowCaptureResultValid = false;
	bool bLastWindowCaptureSucceeded = false;


	void PresentBarrier(Texture* rt);
	void ResourceBarrier(ID3D12Resource* Resource, D3D12_RESOURCE_STATES StateBefore, D3D12_RESOURCE_STATES StateAfter);

	SimpleDX12(ComPtr<ID3D12Device5> pDevice);
	virtual ~SimpleDX12();
};


