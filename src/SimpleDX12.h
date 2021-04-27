#pragma once


#include <d3d12.h>
#include <wrl.h>
#include <dxgi1_4.h>
#include <dxgi1_6.h>

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
#include <dxcapi.use.h>
#include <dxcapi.h>
#if _DEBUG
#define DEBUG_CLIENTBLOCK   new( _CLIENT_BLOCK, __FILE__, __LINE__)
#else
#define DEBUG_CLIENTBLOCK
#endif // _DEBUG

#include "DXSampleHelper.h"

#include "AbstractGfxLayer.h"

#define USE_AFTERMATH 0

using namespace Microsoft::WRL;
using namespace std;


#if defined(_DEBUG)
inline void SetName(ID3D12Object* pObject, LPCWSTR name)
{
	if (wstring(name) == L"ColorBuffers[0]")
	{
		int a = 0;
	}
	pObject->SetName(name);
}
inline void SetNameIndexed(ID3D12Object* pObject, LPCWSTR name, UINT index)
{
	WCHAR fullName[50];
	if (swprintf_s(fullName, L"%s[%u]", name, index) > 0)
	{
		pObject->SetName(fullName);
	}
}
#else
inline void SetName(ID3D12Object*, LPCWSTR)
{
}
inline void SetNameIndexed(ID3D12Object*, LPCWSTR, UINT)
{
}
#endif

// Naming helper for ComPtr<T>.
// Assigns the name of the variable as the name of the object.
// The indexed variant will include the index in the name of the object.
#define NAME_D3D12_OBJECT(x) SetName(x.Get(), L#x)
#define NAME_D3D12_OBJECT_INDEXED(x, n) SetNameIndexed(x[n].Get(), L#x, n)
#define NAME_D3D12_TEXTURE(x) x->name = L#x;SetName(x->resource.Get(), L#x)

class SimpleDX12;
class Texture;
class Sampler;
//class ThreadDescriptorHeapPool;

struct Descriptor : public GfxDescriptor
{
	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle;
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle;
};

class CommandList : public GfxCommandList
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
	CommandQueue();
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

class PipelineStateObject : public GfxPipelineStateObject
{
public:
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


	void Apply(ID3D12GraphicsCommandList* CommandList);

	void BindUAV(string name, int baseRegister);
	void BindSRV(string name, int baseRegister, int num);
	void BindCBV(string name, int baseRegister, int size);
	void BindRootConstant(string name, int baseRegister);
	void BindSampler(string name, int baseRegister);

	void SetSRV(string name, D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleSRV, ID3D12GraphicsCommandList* CommandList);
	void SetUAV(string name, D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleUAV, ID3D12GraphicsCommandList* CommandList);

	void SetSampler(string name, Sampler* sampler, ID3D12GraphicsCommandList* CommandList);

	void SetCBVValue(string name, void* pData, ID3D12GraphicsCommandList* CommandList);
	void SetCBVValue(string name, UINT64 GPUAddr, ID3D12GraphicsCommandList* CommandList);

	void SetRootConstant(string, UINT value, ID3D12GraphicsCommandList* CommandList);

	PipelineStateObject() {}
	virtual ~PipelineStateObject() {}
};


class RTPipelineStateObject : public GfxRTPipelineStateObject
{
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
	UINT MaxPayloadSizeInBytes;
	UINT MaxAttributeSizeInBytes;

	uint32_t ShaderTableEntrySize;
	UINT ShaderTableSize;
	ComPtr<ID3D12Resource> ShaderTable;

	//UINT NumInstance;

	void AddHitGroup(string name, string chs, string ahs);

	// new binding interface
	void AddShader(string shader, RTPipelineStateObject::ShaderType shaderType);
	void BindUAV(string shader, string name, UINT baseRegister);
	void BindSRV(string shader, string name, UINT baseRegister);
	void BindSampler(string shader, string name, UINT baseRegister);
	void BindCBV(string shader, string name, UINT baseRegister, UINT size);

	void BeginShaderTable();
	void EndShaderTable(UINT NumInstance);
	void SetGlobalBinding(CommandList* CommandList);
	
	void SetUAV(string shader, string bindingName, D3D12_GPU_DESCRIPTOR_HANDLE uavHandle, INT instanceIndex = -1);
	void SetSRV(string shader, string bindingName, D3D12_GPU_DESCRIPTOR_HANDLE srvHandle, INT instanceIndex = -1);
	void SetSampler(string shader, string bindingName, Sampler* sampler, INT instanceIndex = -1);
	void SetCBVValue(string shader, string bindingName, void* pData, INT instanceIndex = -1);
	void SetCBVValue(string shader, string bindingName, UINT64 GPUAddr, INT instanceIndex = -1);

	void ResetHitProgram(UINT instanceIndex);
	void StartHitProgram(string HitGroup, UINT instanceIndex);
	void AddDescriptor2HitProgram(string HitGroup, D3D12_GPU_DESCRIPTOR_HANDLE srvHandle, UINT instanceIndex);

	bool InitRS(string ShaderFile, std::optional<vector< DxcDefine>>  Defines = nullopt);
	void DispatchRay(UINT width, UINT height, CommandList* CommandList, UINT NumInstance);

	RTPipelineStateObject() {}
	virtual ~RTPipelineStateObject() {}
};

class Buffer : public GfxBuffer
{
public:
	wstring name;

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

	Descriptor SRV;
	Descriptor UAV;


	void MakeByteAddressBufferSRV();
	void MakeStructuredBufferSRV();

	Buffer() {}
	virtual ~Buffer() {}
};

class IndexBuffer : public GfxIndexBuffer
{
public:
	int numIndices;
	ComPtr<ID3D12Resource> resource;
	D3D12_INDEX_BUFFER_VIEW view;

	Descriptor Descriptor;

	IndexBuffer() {}
	virtual ~IndexBuffer() {}
};

class VertexBuffer : public GfxVertexBuffer
{
public:
	int numVertices;
	ComPtr<ID3D12Resource> resource;
	D3D12_VERTEX_BUFFER_VIEW view;

	Descriptor Descriptor;

	VertexBuffer() {}
	virtual ~VertexBuffer() {}

};

class Sampler : public GfxSampler
{
public:
	D3D12_SAMPLER_DESC SamplerDesc;
	ComPtr<ID3D12Resource> resource;
	
	Descriptor Descriptor;
};

class Texture : public GfxTexture
{
public:
	bool isRT = false;
	wstring name;
	D3D12_RESOURCE_DESC textureDesc;

	ComPtr<ID3D12Resource> resource;

	Descriptor UAV;
	Descriptor RTV;
	Descriptor DSV;
	Descriptor SRV;

	void MakeStaticSRV();
	void MakeDSV();

	void UploadSRCData3D(D3D12_SUBRESOURCE_DATA* SrcData);
	Texture(){}
	virtual ~Texture()
	{
	}
};

class DescriptorHeap
{
public:
	D3D12_DESCRIPTOR_HEAP_DESC HeapDesc;
	ComPtr<ID3D12DescriptorHeap> DH;
	UINT DescriptorSize = 0;

	UINT64 CPUHeapStart;
	UINT64 GPUHeapStart;

	UINT NumAllocated = 0;
	UINT MaxNumDescriptors = 0;
public:
	DescriptorHeap()
	{
	}
	void Init(D3D12_DESCRIPTOR_HEAP_DESC& InHeapDesc);

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
	UINT AllocDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE& cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE& gpuHandle, UINT32 Num = 1);
	void Advance();

	DescriptorHeapRing(){}
	virtual ~DescriptorHeapRing() {}
};


class ConstantBufferRingBuffer
{
	UINT NumFrame = 0;
	UINT CurrentFrame = 0;
	UINT TotalSize;

	UINT AllocPos = 0;
	
	void* MemMapped = nullptr;

	ComPtr<ID3D12Resource> CBMem = nullptr;

public:

	std::tuple<UINT64, UINT8*> AllocGPUMemory(UINT InSize);
	void Advance();

	ConstantBufferRingBuffer(UINT InSize, UINT InNumFrame);
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

class Mesh;
class RTAS : public GfxRTAS
{
public:
	shared_ptr<Mesh> mesh;

	Descriptor Descriptor;

	ComPtr<ID3D12Resource> Scratch;
	ComPtr<ID3D12Resource> Result;
	ComPtr<ID3D12Resource> Instance;
};

class Material
{
public:
	bool bHasAlpha = false;

	shared_ptr<GfxTexture> Diffuse;
	shared_ptr<GfxTexture> Normal;
	shared_ptr<GfxTexture> Roughness;
	shared_ptr<GfxTexture> Metallic;
	Material() {}
	~Material()
	{
		int a = 0;
	}
};

class Mesh
{
public:
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

	FORMAT IndexFormat = FORMAT_R32_UINT;

	shared_ptr<GfxIndexBuffer> Ib;
	shared_ptr<GfxVertexBuffer> Vb;
	vector<shared_ptr<Texture>> Textures;

	shared_ptr<Material> Mat;

	vector<DrawCall> Draws;

	shared_ptr<RTAS> CreateBLAS();
};

class Scene
{
public:
	glm::vec3 AABBMin = glm::vec3(0, 0, 0);
	glm::vec3 AABBMax = glm::vec3(0, 0, 0);
	float BoundingRadius = 0.f;

	void SetTransform(glm::mat4x4 inTransform);

public:
	vector<shared_ptr<Mesh>> meshes;
	vector<shared_ptr<Material>> Materials;
public:
};

class SimpleDX12
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
	std::unique_ptr<DescriptorHeapRing> TextureDHRing; // can be changed only when new texture is added or removed. it works like static at this moment.
	std::unique_ptr<DescriptorHeapRing> GeomtryDHRing;

	std::unique_ptr<ConstantBufferRingBuffer> GlobalCBRing;

	std::unique_ptr<DescriptorHeapRing> GlobalRTDHRing; // can be changed only when new texture is added or removed. it works like static at this moment.


	std::vector<std::shared_ptr<Texture>> renderTargetTextures;
	std::list<Buffer*> DynamicBuffers;


	bool m_windowedMode;

	ComPtr<IDXGISwapChain3> m_swapChain;

	string errorString;

#if USE_AFTERMATH
	GFSDK_Aftermath_ContextHandle AM_CL_Handle;
#endif

	bool m_tearingSupport = false;
	ComPtr<IDXGIAdapter1> m_hardwareAdapter;

public:
	void BeginFrame(std::list<Texture*>& DynamicTexture);
	void EndFrame();

	Texture* CreateTexture2D(DXGI_FORMAT format, D3D12_RESOURCE_FLAGS resFlags, D3D12_RESOURCE_STATES initResState, int width, int height, int mipLevels, std::optional<glm::vec4> clearColor = std::nullopt);
	Texture* CreateTexture3D(DXGI_FORMAT format, D3D12_RESOURCE_FLAGS resFlags, D3D12_RESOURCE_STATES initResState, int width, int height, int depth, int mipLevels);
	Texture* CreateTextureFromFile(wstring fileName, bool nonSRGB);

	shared_ptr<Texture> CreateTexture2DFromResource(ComPtr<ID3D12Resource> InResource); // used only by SimpleDX12


	Sampler* CreateSampler(D3D12_SAMPLER_DESC& InSamplerDesc);
	Buffer* CreateBuffer(UINT InNumElements, UINT InElementSize, D3D12_HEAP_TYPE InType, D3D12_RESOURCE_STATES initResState, D3D12_RESOURCE_FLAGS InFlags, void* SrcData = nullptr);
	IndexBuffer* CreateIndexBuffer(DXGI_FORMAT Format, UINT Size, void* SrcData);
	VertexBuffer* CreateVertexBuffer(UINT Size, UINT Stride, void* SrcData);
	shared_ptr<RTAS> CreateTLAS(vector<shared_ptr<RTAS>>& VecBottomLevelAS);

	ComPtr<ID3DBlob> CreateShader(wstring FileName, string EntryPoint, string Target);
	ComPtr<ID3DBlob> CreateShaderDXC(wstring FileName, wstring EntryPoint, wstring Target, std::optional<vector< DxcDefine>>  Defines);


	void PresentBarrier(Texture* rt);
	void ResourceBarrier(ID3D12Resource* Resource, D3D12_RESOURCE_STATES StateBefore, D3D12_RESOURCE_STATES StateAfter);

	void OnSizeChanged(UINT width, UINT height);
	void GetFrameBuffers(std::vector<std::shared_ptr<Texture>>& FrameFuffers);

	SimpleDX12(HWND hWnd, UINT DisplayWidth, UINT DisplayHeight);
	virtual ~SimpleDX12();
};


