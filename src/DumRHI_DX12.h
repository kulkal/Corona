#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <dxgi1_4.h>
#include <set>
#include <vector>
#include <list>
#include <map>
#include <memory>
#include <string>
#include "DXSampleHelper.h"

using namespace Microsoft::WRL;
using namespace std;

class DumRHI_DX12;
class Texture;
class ConstantBuffer;
class Sampler;
class ThreadDescriptorHeapPool;

class FrameResource
{
public:

	ComPtr<ID3D12CommandAllocator> CommandAllocator;
	vector<ComPtr<ID3D12CommandAllocator>> VecCommandAllocatorMeshDraw;

	UINT64 FenceValue = 0;
};

class Shader
{
public:


	struct BindingData
	{
		string name;
		UINT rootParamIndex;
		UINT baseRegister;
		UINT numDescriptors;
		UINT cbSize;

		Texture* texture;
		Sampler* sampler;

		shared_ptr<ConstantBuffer> cb;

		UINT rootConst;
	};

	

	D3D12_SHADER_BYTECODE ShaderByteCode;
	
private:
	
	
public:
	Shader(UINT8* ByteCode, UINT Size);
	~Shader()
	{
	}
};

class PipelineStateObject
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

		shared_ptr<ConstantBuffer> cb;

		UINT rootConst;
	};

	UINT currentDrawCallIndex = 0;
	UINT NumMaxDrawCall = 0;

	map<string, BindingData> uavBinding;
	map<string, BindingData> textureBinding;
	map<string, BindingData> constantBufferBinding;
	map<string, BindingData> samplerBinding;
	map<string, BindingData> rootBinding;

	UINT RootParamIndex = 0;
	shared_ptr<Shader> vs;
	shared_ptr<Shader> ps;
	shared_ptr<Shader> cs;

	ComPtr<ID3D12RootSignature> RS;
	ComPtr<ID3D12PipelineState> PSO;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC graphicsPSODesc;
	D3D12_COMPUTE_PIPELINE_STATE_DESC computePSODesc;


	void Init2();


	void Apply(ID3D12GraphicsCommandList* CommandList);

	void BindUAV(string name, int baseRegister);
	void BindTexture(string name, int baseRegister, int num);
	void BindConstantBuffer(string name, int baseRegister, int size, UINT numMaxDrawCall = 1);
	void BindRootConstant(string name, int baseRegister);
	void BindSampler(string name, int baseRegister);

	void SetTexture(string name, Texture* texture, ID3D12GraphicsCommandList* CommandList);
	void SetUAV(string name, Texture* texture, ID3D12GraphicsCommandList* CommandList);

	void SetSampler(string name, Sampler* sampler, ID3D12GraphicsCommandList* CommandList);

	void SetConstantValue(string name, void* pData, ID3D12GraphicsCommandList* CommandList);
	void SetRootConstant(string, UINT value, ID3D12GraphicsCommandList* CommandList);
	UINT GetGraphicsBindingDHSize();
};

class RTPipelineStateObject
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

		vector<shared_ptr<ConstantBuffer>> cbs; // multiple constant buffers are need for multiple buffering.
	};
	vector<BindingData> RaygenBinding;

	
public:
	ComPtr<ID3D12RootSignature> RaygenRS;
	ComPtr<ID3D12RootSignature> HitMissRS;
	ComPtr<ID3D12RootSignature> GlobalRS;
	ComPtr<ID3D12StateObject> RTPipelineState;

	enum ShaderType
	{
		RAYGEN,
		MISS,
		HIT
	};
	struct BindingInfo
	{
		ShaderType Type;
		wstring ShaderName;
		vector<BindingData> Binding;

		ComPtr<ID3D12RootSignature> RS;

		D3D12_STATE_SUBOBJECT subobject;
		ID3D12RootSignature* pInterface;
		vector<const WCHAR*> ExportName;

	};
	map<string, BindingInfo> ShaderBinding;

	struct HitGroupInfo
	{
		wstring name;
		wstring chs;
		wstring ahs;
	};

	vector<HitGroupInfo> VecHitGroup;
	map<string, HitGroupInfo> MapHitGroup;

	struct HitProgramData
	{
		wstring HitGroupName;
		vector< D3D12_GPU_DESCRIPTOR_HANDLE> VecData;
	};

	map<UINT, HitProgramData> HitProgramBinding;

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

	UINT NumInstance;

	void AddHitGroup(string name, string chs, string ahs);

	// new binding interface
	void AddShader(string shader, RTPipelineStateObject::ShaderType shaderType);
	void BindUAV(string shader, string name, UINT baseRegister);
	void BindSRV(string shader, string name, UINT baseRegister);
	void BindSampler(string shader, string name, UINT baseRegister);
	void BindCBV(string shader, string name, UINT baseRegister, UINT size, UINT numInstance);

	void BeginShaderTable();
	void EndShaderTable();
	void SetUAV(string shader, string bindingName, D3D12_GPU_DESCRIPTOR_HANDLE uavHandle, INT instanceIndex = -1);
	void SetSRV(string shader, string bindingName, D3D12_GPU_DESCRIPTOR_HANDLE srvHandle, INT instanceIndex = -1);

	void AddHitProgramDescriptor(string shader, D3D12_GPU_DESCRIPTOR_HANDLE srvHandle, UINT instanceIndex);
	void ResetHitProgramBinding(string shader, UINT instanceIndex, UINT size);

	void SetSampler(string shader, string bindingName, Sampler* sampler, INT instanceIndex = -1);
	void SetCBVValue(string shader, string bindingName, void* pData, INT size, INT instanceIndex = -1);
	void SetHitProgram(string shader, UINT instanceIndex);

	void InitRS(string ShaderFile);
	void Apply(UINT width, UINT height);
};

class Buffer
{
public:
	ComPtr<ID3D12Resource> resource;
	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleSRV;
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleSRV;
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

class ConstantBuffer
{
public:
	UINT Size;
	ComPtr<ID3D12Resource> resource = nullptr;
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle;

	// multiple descriptor is needed for multiple draw(ex : transform matrix)
	vector<D3D12_CPU_DESCRIPTOR_HANDLE> CpuHandleVec;
	vector<D3D12_GPU_DESCRIPTOR_HANDLE> GpuHandleVec;

	void* MemMapped = nullptr;

	ConstantBuffer(){}
	
	virtual ~ConstantBuffer()
	{
		resource->Unmap(0, nullptr);
		MemMapped = nullptr;
	}
};
class Texture : public std::enable_shared_from_this<Texture>
{
public:
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
	void MakeRTV();
	void MakeDSV();

	void UploadSRCData3D(D3D12_SUBRESOURCE_DATA* SrcData);
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
	void AllocDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE& cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE& gpuHandle);
	void Advance();

	DescriptorHeapRing(){}
	~DescriptorHeapRing() {}
};

class ThreadDescriptorHeapPool
{
public:
	DescriptorHeap* DHeap = nullptr;
	UINT PoolSize = 0;
	UINT StartIndex = 0;
	UINT PoolIndex = 0;

	// Descriptor in pool is for one thread only, so that it is not overwritten.
	void AllocPool(UINT InPoolSize)
	{
		PoolSize = InPoolSize;
		
		if (DHeap->NumAllocated + PoolSize >= DHeap->MaxNumDescriptors)
			DHeap->NumAllocated = 0;

		PoolIndex = StartIndex = DHeap->NumAllocated;
		DHeap->NumAllocated += PoolSize;
	}

	void AllocDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE& cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE& gpuHandle);

	ThreadDescriptorHeapPool();
	~ThreadDescriptorHeapPool() {}
};

class RTAS
{
public:
	D3D12_GPU_DESCRIPTOR_HANDLE GPUHandle;
	D3D12_CPU_DESCRIPTOR_HANDLE CPUHandle;

	ComPtr<ID3D12Resource> Scratch;
	ComPtr<ID3D12Resource> Result;
	ComPtr<ID3D12Resource> Instance;
};

class Material
{
public:
	shared_ptr<Texture> Diffuse;
	shared_ptr<Texture> Normal;
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
	UINT NumIndices;
	UINT NumVertices;

	UINT VertexStride;

	DXGI_FORMAT IndexFormat = DXGI_FORMAT_R32_UINT;


	shared_ptr<IndexBuffer> Ib;
	shared_ptr<VertexBuffer> Vb;
	vector<shared_ptr<Texture>> Textures;

	shared_ptr<Material> Mat;

	vector<DrawCall> Draws;

	shared_ptr<RTAS> CreateBLAS();
	shared_ptr<RTAS> CreateBLASArray();
};

class Scene
{
public:
	vector<shared_ptr<Mesh>> meshes;
	vector<shared_ptr<Material>> Materials;
public:
};

class DumRHI_DX12
{
public:
	friend class DescriptorHeap;

	const uint32_t NumFrame = 3;
	uint32_t CurrentFrameIndex = 0;

	ComPtr<ID3D12Device5> Device;
	ComPtr<ID3D12CommandQueue> CommandQueue;
	ComPtr<ID3D12GraphicsCommandList4> CommandList;

	ID3D12CommandAllocator* GetCurrentCA() 
	{ 
		return FrameResourceVec[CurrentFrameIndex].CommandAllocator.Get();
	}

	vector<FrameResource> FrameResourceVec;

	static const UINT NumDrawMeshCommandList = 8;
	ComPtr<ID3D12GraphicsCommandList> DrawMeshCommandList[NumDrawMeshCommandList];

	std::unique_ptr<DescriptorHeap> RTVDescriptorHeap;
	std::unique_ptr<DescriptorHeap> DSVDescriptorHeap;
	std::unique_ptr<DescriptorHeap> SamplerDescriptorHeapShaderVisible;
	std::unique_ptr<DescriptorHeap> SRVCBVDescriptorHeapShaderVisible;
	std::unique_ptr<DescriptorHeap> SRVCBVDescriptorHeapStorage;

	std::unique_ptr<DescriptorHeapRing> GlobalDHRing; // resources that changes every frame.
	std::unique_ptr<DescriptorHeapRing> TextureDHRing;
	std::unique_ptr<DescriptorHeapRing> GeomtryDHRing;

	std::vector<std::shared_ptr<Texture>> renderTargetTextures;
	std::list<std::shared_ptr<Texture>> DynamicTextures;

	HANDLE m_fenceEvent;
	ComPtr<ID3D12Fence> m_fence;
	UINT64 m_fenceValue;

	ComPtr<IDXGISwapChain3> m_swapChain;

	GFSDK_Aftermath_ContextHandle AM_CL_Handle;

public:
	void BeginFrame();
	void EndFrame();
	void WaitGPU();

	shared_ptr<Texture> CreateTexture2DFromResource(ComPtr<ID3D12Resource> InResource);
	shared_ptr<Texture> CreateTexture2D(DXGI_FORMAT format, D3D12_RESOURCE_FLAGS resFlags, D3D12_RESOURCE_STATES initResState, int width, int height, int mipLevels);
	shared_ptr<Texture> CreateTexture3D(DXGI_FORMAT format, D3D12_RESOURCE_FLAGS resFlags, D3D12_RESOURCE_STATES initResState, int width, int height, int depth, int mipLevels);

	shared_ptr<Texture> CreateTextureFromFile(wstring fileName, bool isNormal);
	shared_ptr<ConstantBuffer> CreateConstantBuffer(int Size, UINT NumView = 1);
	shared_ptr<Sampler> CreateSampler(D3D12_SAMPLER_DESC& InSamplerDesc);
	shared_ptr<Buffer> CreateBuffer(UINT Size);
	shared_ptr<IndexBuffer> CreateIndexBuffer(DXGI_FORMAT Format, UINT Size, void* SrcData);
	shared_ptr<VertexBuffer> CreateVertexBuffer(UINT Size, UINT Stride, void* SrcData);
	shared_ptr<RTAS> CreateTLAS(vector<shared_ptr<RTAS>>& VecBottomLevelAS);

	void PresentBarrier(Texture* rt);
	void ResourceBarrier(ID3D12Resource* Resource, D3D12_RESOURCE_STATES StateBefore, D3D12_RESOURCE_STATES StateAfter);

	DumRHI_DX12(ComPtr<ID3D12Device5> pDevice);
	virtual ~DumRHI_DX12();
};


