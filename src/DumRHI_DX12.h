#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <dxgi1_4.h>
#include <set>
#include <vector>
#include <map>
#include <memory>
#include "DXSampleHelper.h"

using namespace Microsoft::WRL;
using namespace std;

// when RTV is created, free descriptor 

class DumRHI_DX12;
class Texture;
class ConstantBuffer;
class Sampler;
class ThreadDescriptorHeapPool;

class FrameResource
{
private:

public:

	ComPtr<ID3D12CommandAllocator> CommandAllocator;
	vector<ComPtr<ID3D12CommandAllocator>> VecCommandAllocatorMeshDraw;

	UINT64 FenceValue;
	
};

class GlobalConstantBuffer
{
	UINT cbSize;
public:
	vector<shared_ptr<ConstantBuffer>> cbs;

	void SetValue(void* pData);
	ConstantBuffer* GetCurrentFrameCB();

	GlobalConstantBuffer(UINT size);
	~GlobalConstantBuffer()
	{}
};

class Binder
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
		vector<shared_ptr<ConstantBuffer>> cbs; // multiple constant buffers are need for multiple buffering.

		UINT rootConst;
	};


	map<string, BindingData> uavBinding;
	map<string, BindingData> textureBinding;
	map<string, BindingData> constantBufferBinding;
	map<string, BindingData> samplerBinding;
	map<string, BindingData> rootBinding;

	void BindUAV(string name, int baseRegister);
	void BindTexture(string name, int baseRegister, int num);
	void BindConstantBuffer(string name, int baseRegister, int size, UINT numMaxDrawCall = 1);
	void BindGlobalConstantBuffer(string name, int baseRegister);
	void BindRootConstant(string name, int baseRegister);
	void BindSampler(string name, int baseRegister);

	void SetTexture(string name, Texture* texture);
	void SetUAV(string name, Texture* texture);

	void SetSampler(string name, Sampler* sampler);

	void SetGlobalConstantBuffer(string name, GlobalConstantBuffer* cb);
	void SetConstantValue(string name, void* pData, UINT drawCallIndex = 0);
	void SetRootConstant(string, UINT value);
};

class Shader
{
public:
	struct CBVersions
	{
		vector<shared_ptr<ConstantBuffer>> versions;
	};

	struct BindingData
	{
		string name;
		UINT rootParamIndex;
		UINT baseRegister;
		UINT numDescriptors;
		UINT cbSize;

		Texture* texture;
		Sampler* sampler;
		
		//vector<CBVersions> frameCBs;

		//vector<shared_ptr<ConstantBuffer>> cbs; // multiple constant buffers are need for multiple buffering.

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

	D3D12_SHADER_BYTECODE ShaderByteCode;
	
	void BindUAV(string name, int baseRegister);
	void BindTexture(string name, int baseRegister, int num);
	void BindConstantBuffer(string name, int baseRegister, int size, UINT numMaxDrawCall = 1);
	void BindGlobalConstantBuffer(string name, int baseRegister);
	void BindRootConstant(string name, int baseRegister);
	void BindSampler(string name, int baseRegister);

	void SetTexture(string name, Texture* texture, ID3D12GraphicsCommandList* CommandList, ThreadDescriptorHeapPool* DHPool);
	void SetUAV(string name, Texture* texture);

	void SetSampler(string name, Sampler* sampler, ID3D12GraphicsCommandList* CommandList, ThreadDescriptorHeapPool* DHPool = nullptr);
	
	void SetGlobalConstantBuffer(string name, GlobalConstantBuffer* cb, ID3D12GraphicsCommandList* CommandList, ThreadDescriptorHeapPool* DHPool);
	void SetConstantValue(string name, void* pData, ID3D12GraphicsCommandList* CommandList, ThreadDescriptorHeapPool* DHPool);
	void SetRootConstant(string, UINT value, ID3D12GraphicsCommandList* CommandList);

	Shader(UINT8* ByteCode, UINT Size);
	~Shader()
	{
	}
};

class PipelineStateObject
{
public:
	UINT RootParamIndex = 0;
	//DumRHI_DX12 * rhi;
	shared_ptr<Shader> vs;
	shared_ptr<Shader> ps;
	shared_ptr<Shader> cs;

	

	ComPtr<ID3D12RootSignature> RS;
	ComPtr<ID3D12PipelineState> PSO;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC graphicsPSODesc;
	D3D12_COMPUTE_PIPELINE_STATE_DESC computePSODesc;

	void Init(bool isCompute);
	//void ApplyMeshDraw(ID3D12GraphicsCommandList* CommandList, UINT DrawIndex, ThreadDescriptorHeapPool* DHPool);

	void ApplyGlobal(ID3D12GraphicsCommandList* CommandList);

	void ApplyCS(ID3D12GraphicsCommandList* CommandList);

	void ApplyGraphicsRSPSO(ID3D12GraphicsCommandList* CommandList);
	void ApplyComputeRSPSO(ID3D12GraphicsCommandList* CommandList);

	UINT GetGraphicsBindingDHSize();
};

class RTShaderLib
{
	
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
		vector< D3D12_CPU_DESCRIPTOR_HANDLE> VecData;
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
	void SetUAV(string shader, string bindingName, D3D12_CPU_DESCRIPTOR_HANDLE uavHandle, INT instanceIndex = -1);
	void SetSRV(string shader, string bindingName, D3D12_CPU_DESCRIPTOR_HANDLE srvHandle, INT instanceIndex = -1);
	void SetSampler(string shader, string bindingName, Sampler* sampler, INT instanceIndex = -1);
	void SetCBVValue(string shader, string bindingName, void* pData, INT size, INT instanceIndex = -1);
	void SetHitProgram(string shader, UINT instanceIndex);


	void InitRS(string ShaderFile);

	void Apply(UINT width, UINT height);

};


class IndexBuffer
{
public:
	int numIndices;
	ComPtr<ID3D12Resource> resource;
	D3D12_INDEX_BUFFER_VIEW view;
};

class VertexBuffer
{
public:
	int numVertices;
	ComPtr<ID3D12Resource> resource;
	D3D12_VERTEX_BUFFER_VIEW view;
};

class Sampler
{
public:
	D3D12_SAMPLER_DESC SamplerDesc;
	ComPtr<ID3D12Resource> resource;
	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle;
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle;
};

//template<class T>
class ConstantBuffer
{
public:
	bool isPopulatedThisFrame = false;
	ComPtr<ID3D12Resource> resource = nullptr;
	//D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle;
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle;

	// multiple descriptor is needed for multiple draw(ex : transform matrix)
	vector<D3D12_CPU_DESCRIPTOR_HANDLE> CpuHandleVec;

	void* MemMapped = nullptr;

	ConstantBuffer(){}
		
	
	virtual ~ConstantBuffer()
	{
		resource->Unmap(0, nullptr);
		MemMapped = nullptr;
	}
};

class Texture
{
public:
	DXGI_FORMAT Format;

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

	void VisibleThisFrame();
	void MakeUAV();
	void MakeSRV(bool isDepth = false);
	void MakeRTV();
	void MakeDSV();


	void UploadSRCData(D3D12_SUBRESOURCE_DATA* SrcData);
};

class DescriptorHeap
{
public:
	D3D12_DESCRIPTOR_HEAP_DESC HeapDesc;
	ComPtr<ID3D12DescriptorHeap> DH;
	UINT DescriptorSize;

	UINT64 CPUHeapStart;
	UINT64 GPUHeapStart;

	int DescriptorIndex;
	UINT MaxNumDescriptors;
public:
	DescriptorHeap()
	{

	}
	void Init(D3D12_DESCRIPTOR_HEAP_DESC& InHeapDesc);

	void AllocDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE& cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE& gpuHandle);


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
		
		if (DHeap->DescriptorIndex + PoolSize >= DHeap->MaxNumDescriptors)
			DHeap->DescriptorIndex = 0;

		PoolIndex = StartIndex = DHeap->DescriptorIndex;
		DHeap->DescriptorIndex += PoolSize;
	}

	void AllocDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE& cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE& gpuHandle);

	ThreadDescriptorHeapPool();
	~ThreadDescriptorHeapPool() {}
	
};

class RTAS
{
public:
	ComPtr<ID3D12Resource> Scratch;
	ComPtr<ID3D12Resource> Result;
	ComPtr<ID3D12Resource> Instance;
};



class Mesh
{
public:
	struct DrawCall
	{
		INT DiffuseTextureIndex;
		INT NormalTextureIndex;
		INT SpecularTextureIndex;
		UINT IndexStart;
		UINT IndexCount;
		UINT VertexBase;
		UINT VertexCount;
	};
public:
	UINT VertexStride;

	DXGI_FORMAT IndexFormat = DXGI_FORMAT_R32_UINT;


	shared_ptr<IndexBuffer> Ib;
	shared_ptr<VertexBuffer> Vb;
	vector<shared_ptr<Texture>> Textures;

	vector<DrawCall> Draws;

	shared_ptr<RTAS> CreateBLAS();
	shared_ptr<RTAS> CreateBLASArray();
};



class DumRHI_DX12
{


public:

	friend class DescriptorHeap;

	uint32_t NumFrame = 3;
	uint32_t CurrentFrameIndex = 0;

	UINT64 FrameFenceValue;

	ComPtr<ID3D12Device5> Device;
	ComPtr<ID3D12CommandQueue> CommandQueue;

	ComPtr<ID3D12GraphicsCommandList4> CommandList;

	ID3D12CommandAllocator* GetCurrentCA() { 
		
		return FrameResourceVec[CurrentFrameIndex].CommandAllocator.Get();
	}

	vector<FrameResource> FrameResourceVec;

	static const UINT NumDrawMeshCommandList = 8;
	ComPtr<ID3D12GraphicsCommandList> DrawMeshCommandList[NumDrawMeshCommandList];



	std::unique_ptr<DescriptorHeap> RTVDescriptorHeap;
	std::unique_ptr<DescriptorHeap> DSVDescriptorHeap;
	std::unique_ptr<DescriptorHeap> SamplerDescriptorHeapShaderVisible;

	std::unique_ptr<DescriptorHeap> SRVCBVDescriptorHeapShaderVisible;

	set<Texture*> FrameTextureSet;


	std::unique_ptr<DescriptorHeap> SRVCBVDescriptorHeapStorage;
	std::unique_ptr<DescriptorHeap> SamplerDescriptorHeapStorage;



	shared_ptr<Texture> depthTexture;
	shared_ptr<Texture> ShadowBuffer;

	std::vector<std::shared_ptr<Texture>> renderTargetTextures;




	vector<shared_ptr<ConstantBuffer>> cbVec;



	HANDLE m_fenceEvent;
	ComPtr<ID3D12Fence> m_fence;
	UINT64 m_fenceValue;
	// Synchronization objects.

	ComPtr<IDXGISwapChain3> m_swapChain;


	GFSDK_Aftermath_ContextHandle AM_CL_Handle;

public:
	void BeginFrame();
	void EndFrame();

	void WaitGPU();

	// pupulate unique set of srv descriptors to shader visible heap.
	void UploadeFrameTexture2ShaderVisibleHeap();

	
	shared_ptr<Texture> CreateTexture2DFromResource(ComPtr<ID3D12Resource> InResource);

	shared_ptr<Texture> CreateTexture2D(DXGI_FORMAT format, D3D12_RESOURCE_FLAGS resFlags, D3D12_RESOURCE_STATES initResState, int width, int height, int mipLevels);

	shared_ptr<ConstantBuffer> CreateConstantBuffer(int Size, UINT NumView = 1);
	shared_ptr<Sampler> CreateSampler(D3D12_SAMPLER_DESC& InSamplerDesc);
	shared_ptr<IndexBuffer> CreateIndexBuffer(DXGI_FORMAT Format, UINT Size, void* SrcData);
	shared_ptr<VertexBuffer> CreateVertexBuffer(UINT Size, UINT Stride, void* SrcData);
	
	shared_ptr<RTAS> CreateTLAS(vector<shared_ptr<RTAS>>& VecBottomLevelAS, uint64_t& tlasSize);


	void SetRendertarget(Texture* rt);
	void PresentBarrier(Texture* rt);
	void ResourceBarrier(ID3D12Resource* Resource, D3D12_RESOURCE_STATES StateBefore, D3D12_RESOURCE_STATES StateAfter);
	
	void CreateRendertargets(IDXGISwapChain3* InSwapChain, int width, int height);

	void SubmitCommandList(ID3D12GraphicsCommandList* CmdList);

	DumRHI_DX12(ID3D12Device5 * pDevice);
	virtual ~DumRHI_DX12();
};


