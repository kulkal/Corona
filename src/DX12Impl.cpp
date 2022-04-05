#include "DX12Impl.h"

#include <DirectXMath.h>
#include "DirectXTex.h"
#include "Utils.h"
#include "d3dx12.h"
#define GLM_FORCE_CTOR_INIT

#include "glm/glm.hpp"
#define GLM_ENABLE_EXPERIMENTAL
#include "glm/gtx/transform.hpp"
#include "glm/mat4x4.hpp"
#include "glm/fwd.hpp"
#include "Utils.h"

#include <sstream>
#include <fstream>
#include <D3Dcompiler.h>

#include <assert.h>

#include <comdef.h>
#include <windows.h>
#include <dxcapi.h>

// DirectX Tex
#include "DirectXTex July 2017/Include/DirectXTex.h"
//#ifdef _DEBUG
//#pragma comment(lib, "Debug/DirectXTex.lib")
//#else
//#pragma comment(lib, "Release/DirectXTex.lib")
//#endif

#define align_to(_alignment, _val) (((_val + _alignment - 1) / _alignment) * _alignment)

using namespace std;
using Microsoft::WRL::ComPtr;

DX12Impl* g_dx12_rhi;

void DescriptorHeap::Init(D3D12_DESCRIPTOR_HEAP_DESC& InHeapDesc)
{
	HeapDesc = InHeapDesc;
	MaxNumDescriptors = InHeapDesc.NumDescriptors;
	DescriptorSize = g_dx12_rhi->Device->GetDescriptorHandleIncrementSize(HeapDesc.Type);

	ThrowIfFailed(g_dx12_rhi->Device->CreateDescriptorHeap(&HeapDesc, IID_PPV_ARGS(&DH)));
	NAME_D3D12_OBJECT(DH);

	CPUHeapStart = DH->GetCPUDescriptorHandleForHeapStart().ptr;
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

void DX12Impl::BeginFrame(std::list<Texture*>& DynamicTexture)
{
	CurrentFrameIndex = m_swapChain->GetCurrentBackBufferIndex();

	// wait until gpu processing for this frame resource is completed
	UINT64 ThisFrameFenceValue = FrameFenceValueVec[CurrentFrameIndex];
	
	CmdQSync->WaitFenceValue(ThisFrameFenceValue);

	
	GlobalCmdList = CmdQSync->AllocCmdList();
	GlobalCmdList->Fence = CmdQSync->CurrentFenceValue;

	ID3D12DescriptorHeap* ppHeaps[] = { SRVCBVDescriptorHeapShaderVisible->DH.Get(), SamplerDescriptorHeapShaderVisible->DH.Get() };
	GlobalCmdList->CmdList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
	

	g_dx12_rhi->GlobalDHRing->Advance();

	g_dx12_rhi->GlobalCBRing->Advance();

	g_dx12_rhi->GlobalRTDHRing->Advance();


	
	for (auto& tex : DynamicTexture)
	{
		if (tex->textureDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
		{
			g_dx12_rhi->GlobalDHRing->AllocDescriptor(tex->UAV.CpuHandle, tex->UAV.GpuHandle);


			D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Format = tex->textureDesc.Format;

			g_dx12_rhi->Device->CreateUnorderedAccessView(tex->resource.Get(), nullptr, &uavDesc, tex->UAV.CpuHandle);
		}

		if (tex->textureDesc.Flags & RESOURCE_FLAG_ALLOW_RENDER_TARGET)
		{
			g_dx12_rhi->GlobalRTDHRing->AllocDescriptor(tex->RTV.CpuHandle, tex->RTV.GpuHandle);

			D3D12_RENDER_TARGET_VIEW_DESC desc = {};
			//desc.Format = Format;
			desc.Format = tex->isRT ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : tex->textureDesc.Format;

			desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

			g_dx12_rhi->Device->CreateRenderTargetView(tex->resource.Get(), nullptr, tex->RTV.CpuHandle);
		}


		if (!tex->isRT)
		{

			// alloc dh and create srv
			//g_dx12_rhi->GlobalDHRing->AllocDescriptor(tex->CpuHandleSRV, tex->GpuHandleSRV);
			g_dx12_rhi->GlobalDHRing->AllocDescriptor(tex->SRV.CpuHandle, tex->SRV.GpuHandle);


			D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc = {};
			SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			if (tex->textureDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
				SrvDesc.Format = DXGI_FORMAT_R32_FLOAT;// DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
			else
				SrvDesc.Format = tex->textureDesc.Format;

			SrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			SrvDesc.Texture2D.MipLevels = tex->textureDesc.MipLevels;
			g_dx12_rhi->Device->CreateShaderResourceView(tex->resource.Get(), &SrvDesc, tex->SRV.CpuHandle);
		}
	}

	for (auto& buffer : DynamicBuffers)
	{
		g_dx12_rhi->GlobalDHRing->AllocDescriptor(buffer->UAV.CpuHandle, buffer->UAV.GpuHandle);

		if (buffer->Type == Buffer::BYTE_ADDRESS)
		{
			D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
			uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
			uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
			uavDesc.Buffer.NumElements = buffer->NumElements;

			g_dx12_rhi->Device->CreateUnorderedAccessView(buffer->resource.Get(), nullptr, &uavDesc, buffer->UAV.CpuHandle);
		}
		else if (buffer->Type == Buffer::STRUCTURED)
		{
			D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
			uavDesc.Format = DXGI_FORMAT_UNKNOWN;
			uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
			uavDesc.Buffer.StructureByteStride = buffer->ElementSize;
			uavDesc.Buffer.NumElements = buffer->NumElements;

			g_dx12_rhi->Device->CreateUnorderedAccessView(buffer->resource.Get(), nullptr, &uavDesc, buffer->UAV.CpuHandle);
		}
	}

}

void DX12Impl::EndFrame()
{
#if USE_AFTERMATH
	ThrowIfFailed(m_swapChain->Present(0, 0), &g_dx12_rhi->AM_CL_Handle);
#else
	ThrowIfFailed(m_swapChain->Present(0, 0), nullptr);

#endif
	FrameFenceValueVec[CurrentFrameIndex] = CmdQSync->CurrentFenceValue;;
	CmdQSync->SignalCurrentFence();
}

Sampler* DX12Impl::CreateSampler(D3D12_SAMPLER_DESC& InSamplerDesc)
{
	Sampler* sampler = new Sampler;
	sampler->SamplerDesc = InSamplerDesc;
	SamplerDescriptorHeapShaderVisible->AllocDescriptor(sampler->Descriptor.CpuHandle, sampler->Descriptor.GpuHandle);
	Device->CreateSampler(&sampler->SamplerDesc, sampler->Descriptor.CpuHandle);

	return sampler;
}

Buffer* DX12Impl::CreateBuffer(UINT InNumElements, UINT InElementSize, D3D12_HEAP_TYPE InType, D3D12_RESOURCE_STATES initResState, D3D12_RESOURCE_FLAGS InFlags, void* SrcData)
{
	Buffer * buffer = new Buffer;
	D3D12_RESOURCE_DESC bufDesc = {};
	bufDesc.Alignment = 0;
	bufDesc.DepthOrArraySize = 1;
	bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufDesc.Flags = InFlags;// D3D12_RESOURCE_FLAG_NONE;
	//if (isUAV)
		//bufDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	bufDesc.Format = DXGI_FORMAT_UNKNOWN;
	bufDesc.Height = 1;
	bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	bufDesc.MipLevels = 1;
	bufDesc.SampleDesc.Count = 1;
	bufDesc.SampleDesc.Quality = 0;
	bufDesc.Width = InNumElements * InElementSize;

	buffer->NumElements = InNumElements;
	buffer->ElementSize = InElementSize;

	D3D12_HEAP_PROPERTIES heapProp;
	heapProp.Type = InType;
	heapProp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heapProp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	heapProp.CreationNodeMask = 1;
	heapProp.VisibleNodeMask = 1;
	
	ThrowIfFailed(Device->CreateCommittedResource(&heapProp, D3D12_HEAP_FLAG_NONE, &bufDesc,
		initResState, nullptr, IID_PPV_ARGS(&buffer->resource)));

	if (SrcData)
	{
		CommandList* cmd = g_dx12_rhi->CmdQSync->AllocCmdList();

		cmd->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(buffer->resource.Get(), initResState, D3D12_RESOURCE_STATE_COPY_DEST));

		UINT Size = buffer->NumElements * buffer->ElementSize;

		ComPtr<ID3D12Resource> UploadHeap;

		ThrowIfFailed(g_dx12_rhi->Device->CreateCommittedResource(
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
		cmd->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(buffer->resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, initResState));


		CmdQSync->ExecuteCommandList(cmd);
		CmdQSync->WaitGPU();
	}

	if (InFlags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
		g_dx12_rhi->DynamicBuffers.push_back(buffer);

	return buffer;
}

IndexBuffer* DX12Impl::CreateIndexBuffer(DXGI_FORMAT Format, UINT Size, void* SrcData)
{
	CommandList* cmd = CmdQSync->AllocCmdList();

	IndexBuffer* ib = new IndexBuffer;

	ThrowIfFailed(Device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(Size),
		D3D12_RESOURCE_STATE_COPY_DEST,
		nullptr,
		IID_PPV_ARGS(&ib->resource)));

	NAME_D3D12_OBJECT(ib->resource);

	ib->view.BufferLocation = ib->resource->GetGPUVirtualAddress();
	ib->view.Format = Format;
	ib->view.SizeInBytes = Size;

	if (Format == DXGI_FORMAT_R32_UINT)
	{
		ib->numIndices = Size / 4;
	}
	else if (Format == DXGI_FORMAT_R16_UINT)
	{
		ib->numIndices = Size / 2;
	}


	if (SrcData)
	{
		D3D12_SUBRESOURCE_DATA indexData = {};
		indexData.pData = SrcData;
		indexData.RowPitch = Size;
		indexData.SlicePitch = indexData.RowPitch;

		ComPtr<ID3D12Resource> UploadHeap;

		ThrowIfFailed(Device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(Size),
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&UploadHeap)));

		UpdateSubresources<1>(cmd->CmdList.Get(), ib->resource.Get(), UploadHeap.Get(), 0, 0, 1, &indexData);
		cmd->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(ib->resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));

		// create shader resource view
		D3D12_SHADER_RESOURCE_VIEW_DESC vertexSRVDesc;
		vertexSRVDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		vertexSRVDesc.Format = DXGI_FORMAT_R32_TYPELESS;
		vertexSRVDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
		vertexSRVDesc.Buffer.StructureByteStride = 0;
		vertexSRVDesc.Buffer.FirstElement = 0;
		vertexSRVDesc.Buffer.NumElements = static_cast<UINT>(Size) / sizeof(float); // byte address buffer
		vertexSRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

		//g_dx12_rhi->SRVCBVDescriptorHeapShaderVisible->AllocDescriptor(ib->CpuHandleSRV, ib->GpuHandleSRV);
		GeomtryDHRing->AllocDescriptor(ib->Descriptor.CpuHandle, ib->Descriptor.GpuHandle);

		Device->CreateShaderResourceView(ib->resource.Get(), &vertexSRVDesc, ib->Descriptor.CpuHandle);

		CmdQSync->ExecuteCommandList(cmd);
		CmdQSync->WaitGPU();
	}

	return ib;
}

VertexBuffer* DX12Impl::CreateVertexBuffer(UINT Size, UINT Stride, void* SrcData)
{
	CommandList* cmd = CmdQSync->AllocCmdList();

	VertexBuffer* vb = new VertexBuffer;
	
	ThrowIfFailed(Device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(Size),
		D3D12_RESOURCE_STATE_COPY_DEST,
		nullptr,
		IID_PPV_ARGS(&vb->resource)));

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

		//g_dx12_rhi->SRVCBVDescriptorHeapShaderVisible->AllocDescriptor(vb->CpuHandleSRV, vb->GpuHandleSRV);
		GeomtryDHRing->AllocDescriptor(vb->Descriptor.CpuHandle, vb->Descriptor.GpuHandle);

		Device->CreateShaderResourceView(vb->resource.Get(), &vertexSRVDesc, vb->Descriptor.CpuHandle);

		CmdQSync->ExecuteCommandList(cmd);
		CmdQSync->WaitGPU();
	}

	return vb;
}

void DX12Impl::GetFrameBuffers(std::vector<std::shared_ptr<Texture>>& FrameFuffers)
{
	for (UINT i = 0; i < NumFrame; i++)
	{
		ComPtr<ID3D12Resource> rendertarget;
		ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&rendertarget)));

		// create each rtv for one actual resource(swapchain)
		shared_ptr<Texture> rt = shared_ptr<Texture>(CreateTexture2DFromResource(rendertarget));
		rt->isRT = true;

		rt->textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		rt->textureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

		NAME_D3D12_TEXTURE(rt);
		FrameFuffers.push_back(rt);
	}
}

DX12Impl::DX12Impl(HWND hWnd, UINT DisplayWidth, UINT DisplayHeight)
{
	g_dx12_rhi = this;
	CoInitialize(NULL);

	UINT dxgiFactoryFlags = 0;

#if defined(_DEBUG)
	// Enable the debug layer (requires the Graphics Tools "optional feature").
	// NOTE: Enabling the debug layer after device creation will invalidate the active device.
	{
		ComPtr<ID3D12Debug> debugController;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
		{
			debugController->EnableDebugLayer();

			// Enable additional debug layers.
			dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;

		}

		ComPtr<ID3D12Debug1> spDebugController1;
		debugController->QueryInterface(IID_PPV_ARGS(&spDebugController1));
		//spDebugController1->SetEnableGPUBasedValidation(true);
	}
#endif

	ComPtr<IDXGIFactory4> factory;
	ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory)));

	// check tearing support
	{
		ComPtr<IDXGIFactory6> factory6;
		HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory6));
		BOOL allowTearing = FALSE;
		if (SUCCEEDED(hr))
		{
			hr = factory6->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing));
		}

		m_tearingSupport = SUCCEEDED(hr) && allowTearing;
	}

	//GetHardwareAdapter(factory.Get(), &hardwareAdapter);
	for (uint32_t i = 0; DXGI_ERROR_NOT_FOUND != factory->EnumAdapters1(i, &m_hardwareAdapter); i++)
	{
		DXGI_ADAPTER_DESC1 desc;
		m_hardwareAdapter->GetDesc1(&desc);

		// Skip SW adapters
		if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

		ThrowIfFailed(D3D12CreateDevice(
			m_hardwareAdapter.Get(),
			D3D_FEATURE_LEVEL_12_1,
			IID_PPV_ARGS(&Device)
		));

		D3D12_FEATURE_DATA_D3D12_OPTIONS5 features5;
		HRESULT hr = Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &features5, sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS5));
		if (FAILED(hr) || features5.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
		{
			//msgBox("Raytracing is not supported on this device. Make sure your GPU supports DXR (such as Nvidia's Volta or Turing RTX) and you're on the latest drivers. The DXR fallback layer is not supported.");
			ThrowIfFailed(hr);
		}


		/*ComPtr<IDXGIAdapter3> pDXGIAdapter3;
		hardwareAdapter->QueryInterface(IID_PPV_ARGS(&pDXGIAdapter3));

		ThrowIfFailed(pDXGIAdapter3->SetVideoMemoryReservation(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, 2213100441));


		DXGI_QUERY_VIDEO_MEMORY_INFO LocalVideoMemoryInfo;
		ThrowIfFailed(pDXGIAdapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &LocalVideoMemoryInfo));*/

		// break on error

		ComPtr<ID3D12InfoQueue> d3dInfoQueue;
		if (SUCCEEDED(Device->QueryInterface(__uuidof(ID3D12InfoQueue), (void**)&d3dInfoQueue)))
		{
			d3dInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
			d3dInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);

			//D3D12_MESSAGE_ID blockedIds[] = {
			//	/*	D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
			//		D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE, */
			//		D3D12_MESSAGE_ID_COPY_DESCRIPTORS_INVALID_RANGES
			//};
			//D3D12_INFO_QUEUE_FILTER filter = {};
			//filter.DenyList.pIDList = blockedIds;
			//filter.DenyList.NumIDs = 1;
			//d3dInfoQueue->AddRetrievalFilterEntries(&filter);
			//d3dInfoQueue->AddStorageFilterEntries(&filter);
		}
		break;
	}

	// Describe and create the swap chain.
	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
	swapChainDesc.BufferCount = NumFrame;
	swapChainDesc.Width = DisplayWidth;
	swapChainDesc.Height = DisplayHeight;
	swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swapChainDesc.SampleDesc.Count = 1;

	//g_dx12_rhi = this;

	CmdQSync = unique_ptr<CommandQueue>(new CommandQueue);


	ComPtr<IDXGISwapChain1> swapChain;
	ThrowIfFailed(factory->CreateSwapChainForHwnd(
		CmdQSync->CmdQueue.Get(),		// Swap chain needs the queue so that it can force a flush on it.
		hWnd,
		&swapChainDesc,
		nullptr,
		nullptr,
		&swapChain
	));

	// This sample does not support fullscreen transitions.
	//ThrowIfFailed(factory->MakeWindowAssociation(Win32Application::GetHwnd(), DXGI_MWA_NO_ALT_ENTER));

	ThrowIfFailed(swapChain.As(&m_swapChain));
	//dx12_rhi->m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

	m_swapChain = m_swapChain;

	


	//ThrowIfFailed(swapChain->SetFullscreenState(true, nullptr));


#if USE_AFTERMATH
	GFSDK_Aftermath_Result result = GFSDK_Aftermath_DX12_Initialize(GFSDK_Aftermath_Version::GFSDK_Aftermath_Version_API, GFSDK_Aftermath_FeatureFlags::GFSDK_Aftermath_FeatureFlags_Maximum, Device.Get());
#endif


	FrameFenceValueVec.resize(NumFrame);


	{
		RTVDescriptorHeap = std::make_unique<DescriptorHeap>();
		D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
		HeapDesc.NumDescriptors = 90;
		HeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		RTVDescriptorHeap->Init(HeapDesc);

		NAME_D3D12_OBJECT(RTVDescriptorHeap->DH);
	}
	

	{
		DSVDescriptorHeap = std::make_unique<DescriptorHeap>();

		D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
		HeapDesc.NumDescriptors = 2;
		HeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		DSVDescriptorHeap->Init(HeapDesc);

		NAME_D3D12_OBJECT(DSVDescriptorHeap->DH);
	}
	
	// shader visible CBV_SRV_UAV
	{
		SRVCBVDescriptorHeapShaderVisible = std::make_unique<DescriptorHeap>();

		D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
		HeapDesc.NumDescriptors = 1000000;
		HeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		SRVCBVDescriptorHeapShaderVisible->Init(HeapDesc);

		NAME_D3D12_OBJECT(SRVCBVDescriptorHeapShaderVisible->DH);
	}

	// non shader visible(storage) CBV_SRV_UAV
	{
		SRVCBVDescriptorHeapStorage = std::make_unique<DescriptorHeap>();

		D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
		HeapDesc.NumDescriptors = 2000000;
		HeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		SRVCBVDescriptorHeapStorage->Init(HeapDesc);

		NAME_D3D12_OBJECT(SRVCBVDescriptorHeapStorage->DH);
	}

	// shader visible sampler
	{
		SamplerDescriptorHeapShaderVisible = std::make_unique<DescriptorHeap>();

		D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
		HeapDesc.NumDescriptors = 1024*2;
		HeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
		HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		SamplerDescriptorHeapShaderVisible->Init(HeapDesc);

		NAME_D3D12_OBJECT(SamplerDescriptorHeapShaderVisible->DH);
	}
	


	TextureDHRing = std::make_unique<DescriptorHeapRing>();
	TextureDHRing->Init(SRVCBVDescriptorHeapShaderVisible.get(), 10000, NumFrame);

	GlobalDHRing = std::make_unique<DescriptorHeapRing>();
	GlobalDHRing->Init(SRVCBVDescriptorHeapShaderVisible.get(), 10000, NumFrame);

	GeomtryDHRing = std::make_unique<DescriptorHeapRing>();
	GeomtryDHRing->Init(SRVCBVDescriptorHeapShaderVisible.get(), 10000, NumFrame);

	GlobalCBRing = std::make_unique<ConstantBufferRingBuffer>(1024 * 1024 * 10, NumFrame);
	

	GlobalRTDHRing = std::make_unique<DescriptorHeapRing>();
	GlobalRTDHRing->Init(RTVDescriptorHeap.get(), 30, NumFrame);

	CmdQSync->WaitGPU();
}

DX12Impl::~DX12Impl()
{
	CmdQSync->WaitGPU();
}

void PipelineStateObject::BindUAV(string name, int baseRegister)
{
	BindingData binding;
	binding.name = name;
	binding.baseRegister = baseRegister;
	binding.numDescriptors = 1;
	uavBinding.insert(pair<string, BindingData>(name, binding));
}

void PipelineStateObject::BindSRV(string name, int baseRegister, int num)
{
	BindingData binding;
	binding.name = name;
	binding.baseRegister = baseRegister;
	binding.numDescriptors = num;
	textureBinding.insert(pair<string, BindingData>(name, binding));
}

void PipelineStateObject::BindCBV(string name, int baseRegister, int size)
{
	BindingData binding;
	binding.name = name;
	binding.baseRegister = baseRegister;
	binding.numDescriptors = 1;
	//binding.cbSize = size;

	int div = size / 256;
	binding.cbSize = (div) * 256;

	if (size % 256 > 0)
		binding.cbSize += 256;

	constantBufferBinding.insert(pair<string, BindingData>(name, binding));
}

void PipelineStateObject::BindRootConstant(string name, int baseRegister)
{
	BindingData binding;
	binding.name = name;
	binding.baseRegister = baseRegister;
	binding.numDescriptors = 1;

	rootBinding.insert(pair<string, BindingData>(name, binding));
}

void PipelineStateObject::BindSampler(string name, int baseRegister)
{
	BindingData binding;
	binding.name = name;
	binding.baseRegister = baseRegister;
	binding.numDescriptors = 1;

	samplerBinding.insert(pair<string, BindingData>(name, binding));
}

void PipelineStateObject::SetSRV(string name, D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleSRV, ID3D12GraphicsCommandList* CommandList)
{
	//textureBinding[name].texture = texture;

	UINT RPI = textureBinding[name].rootParamIndex;
	if (IsCompute)
		CommandList->SetComputeRootDescriptorTable(RPI, GpuHandleSRV);
	else
		CommandList->SetGraphicsRootDescriptorTable(RPI, GpuHandleSRV);
}

void PipelineStateObject::SetUAV(string name, D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleUAV, ID3D12GraphicsCommandList* CommandList)
{
	//uavBinding[name].texture = texture;
	D3D12_CPU_DESCRIPTOR_HANDLE ShaderVisibleCPUHandle;
	D3D12_GPU_DESCRIPTOR_HANDLE ShaderVisibleGpuHandle;

	UINT RPI = uavBinding[name].rootParamIndex;
	if (IsCompute)
		CommandList->SetComputeRootDescriptorTable(RPI, GpuHandleUAV);
	else
		CommandList->SetGraphicsRootDescriptorTable(RPI, GpuHandleUAV);
}
void PipelineStateObject::SetSampler(string name, Sampler* sampler, ID3D12GraphicsCommandList* CommandList)
{
	D3D12_CPU_DESCRIPTOR_HANDLE ShaderVisibleCPUHandle;
	D3D12_GPU_DESCRIPTOR_HANDLE ShaderVisibleGpuHandle;

	if (IsCompute)
		CommandList->SetComputeRootDescriptorTable(samplerBinding[name].rootParamIndex, sampler->Descriptor.GpuHandle);
	else
		CommandList->SetGraphicsRootDescriptorTable(samplerBinding[name].rootParamIndex, sampler->Descriptor.GpuHandle);
}

void PipelineStateObject::SetCBVValue(string name, void* pData, ID3D12GraphicsCommandList* CommandList)
{
	map<string, BindingData> ::iterator it = constantBufferBinding.find(name);
	assert(it != constantBufferBinding.end());
	
	BindingData& binding = it->second;// constantBufferBinding[name];
	auto Alloc = g_dx12_rhi->GlobalCBRing->AllocGPUMemory(binding.cbSize);
	UINT64 GPUAddr = std::get<0>(Alloc);
	UINT8* pMapped = std::get<1>(Alloc);

	memcpy((void*)pMapped, pData, binding.cbSize);

	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle;
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle;

	// ring is advanced at the begining of frame. so descriptors from multiple frame is not overlapped.
	g_dx12_rhi->GlobalDHRing->AllocDescriptor(CpuHandle, GpuHandle);

	D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
	cbvDesc.BufferLocation = GPUAddr;
	cbvDesc.SizeInBytes = binding.cbSize;

	g_dx12_rhi->Device->CreateConstantBufferView(&cbvDesc, CpuHandle);

	if (IsCompute)
		CommandList->SetComputeRootDescriptorTable(binding.rootParamIndex, GpuHandle);
	else
		CommandList->SetGraphicsRootDescriptorTable(binding.rootParamIndex, GpuHandle);
}

void PipelineStateObject::SetCBVValue(string name, UINT64 GPUAddr, ID3D12GraphicsCommandList* CommandList)
{
	map<string, BindingData> ::iterator it = constantBufferBinding.find(name);
	assert(it != constantBufferBinding.end());

	BindingData& binding = it->second;// constantBufferBinding[name];
	/*auto Alloc = g_dx12_rhi->GlobalCBRing->AllocGPUMemory(binding.cbSize);
	UINT64 GPUAddr = std::get<0>(Alloc);
	UINT8* pMapped = std::get<1>(Alloc);

	memcpy((void*)pMapped, pData, binding.cbSize);*/

	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle;
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle;

	// ring is advanced at the begining of frame. so descriptors from multiple frame is not overlapped.
	g_dx12_rhi->GlobalDHRing->AllocDescriptor(CpuHandle, GpuHandle);

	D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
	cbvDesc.BufferLocation = GPUAddr;
	cbvDesc.SizeInBytes = binding.cbSize;

	g_dx12_rhi->Device->CreateConstantBufferView(&cbvDesc, CpuHandle);

	if (IsCompute)
		CommandList->SetComputeRootDescriptorTable(binding.rootParamIndex, GpuHandle);
	else
		CommandList->SetGraphicsRootDescriptorTable(binding.rootParamIndex, GpuHandle);
}


void PipelineStateObject::SetRootConstant(string name, UINT value, ID3D12GraphicsCommandList* CommandList)
{
	rootBinding[name].rootConst = value;
	if(IsCompute)
		CommandList->SetComputeRoot32BitConstant(rootBinding[name].rootParamIndex, rootBinding[name].rootConst, 0);
	else
		CommandList->SetGraphicsRoot32BitConstant(rootBinding[name].rootParamIndex, rootBinding[name].rootConst, 0);
}

bool PipelineStateObject::Init()
{
	if (!IsCompute &&(!vs || !ps)) return false;
	if (IsCompute && !cs) return false;


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
			TextureRanges[i].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, bindingData.numDescriptors, bindingData.baseRegister, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
			TextureParam.InitAsDescriptorTable(1, &TextureRanges[i], D3D12_SHADER_VISIBILITY_ALL);
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
			SamplerRanges[i].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, bindingData.numDescriptors, bindingData.baseRegister);
			SamplerParam.InitAsDescriptorTable(1, &SamplerRanges[i], D3D12_SHADER_VISIBILITY_ALL);
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
			ConstantParam.InitAsConstants(bindingData.numDescriptors, bindingData.baseRegister, 0, D3D12_SHADER_VISIBILITY_ALL);
			rootParamVec.push_back(ConstantParam);
			i++;
			bindingData.rootParamIndex = RootParamIndex++;
		}
	}

	vector<CD3DX12_DESCRIPTOR_RANGE1> CBRanges;
	if (constantBufferBinding.size() != 0)
	{

		CBRanges.resize(constantBufferBinding.size());
		int i = 0;
		for (auto& bindingPair : constantBufferBinding)
		{
			PipelineStateObject::BindingData& bindingData = bindingPair.second;

			CD3DX12_ROOT_PARAMETER1 CBParam;
			CBRanges[i].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, bindingData.numDescriptors, bindingData.baseRegister, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
			CBParam.InitAsDescriptorTable(1, &CBRanges[i], D3D12_SHADER_VISIBILITY_ALL);

			rootParamVec.push_back(CBParam);
			i++;
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
			UAVRanges[i].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, bindingData.numDescriptors, bindingData.baseRegister, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
			TextureParam.InitAsDescriptorTable(1, &UAVRanges[i], D3D12_SHADER_VISIBILITY_ALL);
			rootParamVec.push_back(TextureParam);
			i++;
			bindingData.rootParamIndex = RootParamIndex++;
		}
	}

	CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
	rootSignatureDesc.Init_1_1(rootParamVec.size(), &rootParamVec[0], 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = {};
	featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;

	if (FAILED(g_dx12_rhi->Device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData))))
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
		ThrowIfFailed(g_dx12_rhi->Device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&RS)));
	}
	catch (const std::exception& e)
	{
		OutputDebugStringA(reinterpret_cast<const char*>(error->GetBufferPointer()));
	}

	NAME_D3D12_OBJECT(RS);
	if (IsCompute)
	{
		computePSODesc.CS = CD3DX12_SHADER_BYTECODE(cs->GetBufferPointer(), cs->GetBufferSize());
		computePSODesc.pRootSignature = RS.Get();
		HRESULT hr;
		ThrowIfFailed(hr = g_dx12_rhi->Device->CreateComputePipelineState(&computePSODesc, IID_PPV_ARGS(&PSO)));
		NAME_D3D12_OBJECT(PSO);
		return SUCCEEDED(hr);

	}
	else
	{
		graphicsPSODesc.VS = CD3DX12_SHADER_BYTECODE(vs->GetBufferPointer(), vs->GetBufferSize());
		graphicsPSODesc.PS = CD3DX12_SHADER_BYTECODE(ps->GetBufferPointer(), ps->GetBufferSize());

		graphicsPSODesc.pRootSignature = RS.Get();
		HRESULT hr;
		ThrowIfFailed(hr = g_dx12_rhi->Device->CreateGraphicsPipelineState(&graphicsPSODesc, IID_PPV_ARGS(&PSO)));
		NAME_D3D12_OBJECT(PSO);
		return SUCCEEDED(hr);
	}
}

void PipelineStateObject::Apply(ID3D12GraphicsCommandList* CommandList)
{
	if (IsCompute)
	{
		CommandList->SetComputeRootSignature(RS.Get());
		CommandList->SetPipelineState(PSO.Get());
	}
	else
	{
		CommandList->SetGraphicsRootSignature(RS.Get());
		CommandList->SetPipelineState(PSO.Get());
	}
}

void Texture::MakeStaticSRV()
{
	g_dx12_rhi->TextureDHRing->AllocDescriptor(SRV.CpuHandle, SRV.GpuHandle);

	D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc = {};
	SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	SrvDesc.Format = textureDesc.Format;

	if(textureDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
		SrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
	else 
		SrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	SrvDesc.Texture2D.MipLevels = textureDesc.MipLevels;
	g_dx12_rhi->Device->CreateShaderResourceView(resource.Get(), &SrvDesc, SRV.CpuHandle);
}

void Texture::MakeDSV()
{
	g_dx12_rhi->DSVDescriptorHeap->AllocDescriptor(DSV.CpuHandle, DSV.GpuHandle);

	D3D12_DEPTH_STENCIL_VIEW_DESC depthStencilDesc = {};
	depthStencilDesc.Format = DXGI_FORMAT_D32_FLOAT;// DXGI_FORMAT_D24_UNORM_S8_UINT;// textureDesc.Format;// DXGI_FORMAT_D32_FLOAT;
	depthStencilDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	depthStencilDesc.Flags = D3D12_DSV_FLAG_NONE;
	g_dx12_rhi->Device->CreateDepthStencilView(resource.Get(), &depthStencilDesc, DSV.CpuHandle);
}

Texture* DX12Impl::CreateTexture2DFromResource(ComPtr<ID3D12Resource> InResource)
{
	Texture* tex = new Texture;

	if (InResource)
	{
		tex->resource = InResource;
	}

	return tex;
}

Texture* DX12Impl::CreateTexture2D(DXGI_FORMAT format, D3D12_RESOURCE_FLAGS resFlags, D3D12_RESOURCE_STATES initResState, int width, int height, int mipLevels, std::optional<glm::vec4> clearColor)
{
	Texture* tex = new Texture;

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
		optimizedClearValue.Format = DXGI_FORMAT_D32_FLOAT;// DXGI_FORMAT_D24_UNORM_S8_UINT;
		optimizedClearValue.DepthStencil = { 1.0f, 0 };

		ThrowIfFailed(Device->CreateCommittedResource(
			&heapProp,
			D3D12_HEAP_FLAG_NONE,
			&textureDesc,
			ResStats,
			&optimizedClearValue,
			IID_PPV_ARGS(&tex->resource)));


		// create static dsv.
		// TODO : should I make dsv dynamic? like rtv & uav.
		g_dx12_rhi->DSVDescriptorHeap->AllocDescriptor(tex->DSV.CpuHandle, tex->DSV.GpuHandle);

		D3D12_DEPTH_STENCIL_VIEW_DESC depthStencilDesc = {};
		depthStencilDesc.Format = DXGI_FORMAT_D32_FLOAT;// DXGI_FORMAT_D24_UNORM_S8_UINT;// textureDesc.Format;// DXGI_FORMAT_D32_FLOAT;
		depthStencilDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
		depthStencilDesc.Flags = D3D12_DSV_FLAG_NONE;
		g_dx12_rhi->Device->CreateDepthStencilView(tex->resource.Get(), &depthStencilDesc, tex->DSV.CpuHandle);
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

	return tex;
}

Texture* DX12Impl::CreateTexture3D(DXGI_FORMAT format, D3D12_RESOURCE_FLAGS resFlags, D3D12_RESOURCE_STATES initResState, int width, int height, int depth, int mipLevels)
{
	Texture* tex = new Texture;

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

	//shared_ptr<Texture> texPtr = shared_ptr<Texture>(tex);

	/*if (resFlags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET || resFlags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL || resFlags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
		g_dx12_rhi->DynamicTextures.push_back(tex);
	else*/
		tex->MakeStaticSRV();

	return tex;
}

void Texture::UploadSRCData3D(D3D12_SUBRESOURCE_DATA* SrcData)
{
	CommandList* cmd = g_dx12_rhi->CmdQSync->AllocCmdList();

	// upload src data
	if (SrcData)
	{
		ComPtr<ID3D12Resource> textureUploadHeap;

		D3D12_PLACED_SUBRESOURCE_FOOTPRINT descFootPrint;
		UINT Rows = 0;
		UINT64 RowSize = 0;
		UINT64 TotalBytes = 0;
		g_dx12_rhi->Device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &descFootPrint, &Rows, &RowSize, &TotalBytes);


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


		ThrowIfFailed(g_dx12_rhi->Device->CreateCommittedResource(
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

		g_dx12_rhi->CmdQSync->ExecuteCommandList(cmd);
		g_dx12_rhi->CmdQSync->WaitGPU();
	}
}

Texture* DX12Impl::CreateTextureFromFile(wstring fileName, bool nonSRGB)
{
	if (FileExists(fileName.c_str()) == false)
		return nullptr;

	CommandList* cmd = g_dx12_rhi->CmdQSync->AllocCmdList();


	Texture* tex = new Texture;

	DirectX::ScratchImage image;

	const std::wstring extension = GetFileExtension(fileName.c_str());

	if (extension == L"DDS" || extension == L"dds")
	{
		DirectX::LoadFromDDSFile(fileName.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image);
	}
	else if (extension == L"TGA" || extension == L"tga")
	{
		DirectX::ScratchImage tempImage;
		DirectX::LoadFromTGAFile(fileName.c_str(), nullptr, tempImage);
		DirectX::GenerateMipMaps(*tempImage.GetImage(0, 0, 0), DirectX::TEX_FILTER_DEFAULT, 0, image, false);
	}
	else
	{
		DirectX::ScratchImage tempImage;
		DirectX::LoadFromWICFile(fileName.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, tempImage);
		DirectX::GenerateMipMaps(*tempImage.GetImage(0, 0, 0), DirectX::TEX_FILTER_DEFAULT, 0, image, false);
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

	D3D12_HEAP_PROPERTIES heapProp;
	heapProp.Type = D3D12_HEAP_TYPE_DEFAULT;
	heapProp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heapProp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	heapProp.CreationNodeMask = 1;
	heapProp.VisibleNodeMask = 1;

	tex->textureDesc = textureDesc;

	g_dx12_rhi->Device->CreateCommittedResource(&heapProp, D3D12_HEAP_FLAG_NONE, &textureDesc,
		D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex->resource));
	tex->resource->SetName(fileName.c_str());

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

	g_dx12_rhi->Device->CreateCommittedResource(&heapPropUpload, D3D12_HEAP_FLAG_NONE, &resDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadHeap));
	uploadHeap->SetName(L"TexUploadingHeap");

	const UINT64 numSubResources = metaData.mipLevels * metaData.arraySize;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT* layouts = (D3D12_PLACED_SUBRESOURCE_FOOTPRINT*)_alloca(sizeof(D3D12_PLACED_SUBRESOURCE_FOOTPRINT) * numSubResources);
	UINT32* numRows = (UINT32*)_alloca(sizeof(UINT32) * numSubResources);
	UINT64* rowSizes = (UINT64*)_alloca(sizeof(UINT64) * numSubResources);

	UINT64 textureMemSize = 0;
	g_dx12_rhi->Device->GetCopyableFootprints(&textureDesc, 0, UINT32(numSubResources), 0, layouts, numRows, rowSizes, &textureMemSize);

	UINT8* uploadMem = nullptr;

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
					memcpy(dstSubResourceMem, srcSubResourceMem, glm::min<float>(subResourcePitch, subImage->rowPitch));
					dstSubResourceMem += subResourcePitch;
					srcSubResourceMem += subImage->rowPitch;
				}
			}
		}
	}
	uploadHeap->Unmap(0, nullptr);

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
	BarrierDesc.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	cmd->CmdList->ResourceBarrier(1, &BarrierDesc);

	g_dx12_rhi->CmdQSync->ExecuteCommandList(cmd);
	g_dx12_rhi->CmdQSync->WaitGPU();

	tex->MakeStaticSRV();

	return tex;
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

//RTAS* Mesh::CreateBLAS()
//{
//	RTAS* as = new  RTAS;
//	as->mesh = this;
//
//	VertexBuffer* vb = static_cast<VertexBuffer*>(Vb.get());
//	IndexBuffer* ib = static_cast<IndexBuffer*>(Ib.get());
//	D3D12_RAYTRACING_GEOMETRY_DESC geomDesc = {};
//	geomDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
//	geomDesc.Triangles.VertexBuffer.StartAddress = vb->resource->GetGPUVirtualAddress();
//	geomDesc.Triangles.VertexBuffer.StrideInBytes = VertexStride;;
//	geomDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
//	geomDesc.Triangles.VertexCount = vb->numVertices;
//	geomDesc.Triangles.IndexBuffer = ib->resource->GetGPUVirtualAddress();
//	geomDesc.Triangles.IndexFormat = static_cast<DXGI_FORMAT>(IndexFormat);
//	geomDesc.Triangles.IndexCount = ib->numIndices;
//	geomDesc.Triangles.Transform3x4 = 0;
//
//	if(bTransparent)
//		geomDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
//	else
//		geomDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
//
//
//
//	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
//	inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
//	inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
//	inputs.NumDescs = 1;
//	inputs.pGeometryDescs = &geomDesc;
//	inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
//
//	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
//	g_dx12_rhi->Device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
//
//	{
//		D3D12_RESOURCE_DESC bufDesc = {};
//		bufDesc.Alignment = 0;
//		bufDesc.DepthOrArraySize = 1;
//		bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
//		bufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
//		bufDesc.Format = DXGI_FORMAT_UNKNOWN;
//		bufDesc.Height = 1;
//		bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
//		bufDesc.MipLevels = 1;
//		bufDesc.SampleDesc.Count = 1;
//		bufDesc.SampleDesc.Quality = 0;
//		bufDesc.Width = info.ScratchDataSizeInBytes;
//
//		/*stringstream ss;
//		ss << "blas->scratch : " << bufDesc.Width << "\n";
//		OutputDebugStringA(ss.str().c_str());*/
//
//		g_dx12_rhi->Device->CreateCommittedResource(&kDefaultHeapProps, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&as->Scratch));
//	}
//	
//	{
//		D3D12_RESOURCE_DESC bufDesc = {};
//		bufDesc.Alignment = 0;
//		bufDesc.DepthOrArraySize = 1;
//		bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
//		bufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
//		bufDesc.Format = DXGI_FORMAT_UNKNOWN;
//		bufDesc.Height = 1;
//		bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
//		bufDesc.MipLevels = 1;
//		bufDesc.SampleDesc.Count = 1;
//		bufDesc.SampleDesc.Quality = 0;
//		bufDesc.Width = info.ResultDataMaxSizeInBytes;
//		
//		/*stringstream ss;
//		ss << "blas->result : " << bufDesc.Width << "\n";
//		OutputDebugStringA(ss.str().c_str());*/
//
//
//		g_dx12_rhi->Device->CreateCommittedResource(&kDefaultHeapProps, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr, IID_PPV_ARGS(&as->Result));
//	}
//
//	CommandList* cmd = g_dx12_rhi->CmdQ->AllocCmdList();
//
//	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC asDesc = {};
//	asDesc.Inputs = inputs;
//	asDesc.DestAccelerationStructureData = as->Result->GetGPUVirtualAddress();
//	asDesc.ScratchAccelerationStructureData = as->Scratch->GetGPUVirtualAddress();
//
//	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC postInfo;
//	cmd->CmdList->BuildRaytracingAccelerationStructure(&asDesc, 0, &postInfo);
//
//	/*D3D12_RESOURCE_BARRIER uavBarrier = {};
//	uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
//	uavBarrier.UAV.pResource = as->Result.Get();
//	cmd->CmdList->ResourceBarrier(1, &uavBarrier);*/
//
//	g_dx12_rhi->CmdQ->ExecuteCommandList(cmd);
//
//	return as;
//}

RTAS* DX12Impl::CreateTLAS(vector<RTAS*>& VecBottomLevelAS)
{
	RTAS* as = new  RTAS;

	// First, get the size of the TLAS buffers and create them
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
	inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
	inputs.NumDescs = VecBottomLevelAS.size();
	inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info;
	g_dx12_rhi->Device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);

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

		g_dx12_rhi->Device->CreateCommittedResource(&kDefaultHeapProps, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&as->Scratch));
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

		g_dx12_rhi->Device->CreateCommittedResource(&kDefaultHeapProps, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr, IID_PPV_ARGS(&as->Result));
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
		bufDesc.Width = sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * VecBottomLevelAS.size();

		g_dx12_rhi->Device->CreateCommittedResource(&kUploadHeapProps, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&as->Instance));
	}

	if (VecBottomLevelAS.size() > 0)
	{
		D3D12_RAYTRACING_INSTANCE_DESC* pInstanceDesc;
		as->Instance->Map(0, nullptr, (void**)&pInstanceDesc);
		ZeroMemory(pInstanceDesc, sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * VecBottomLevelAS.size());

		for (int i = 0; i < VecBottomLevelAS.size(); i++)
		{
			pInstanceDesc[i].InstanceID = i;                            // This value will be exposed to the shader via InstanceID()
			pInstanceDesc[i].InstanceContributionToHitGroupIndex = i;   // This is the offset inside the shader-table. We only have a single geometry, so the offset 0
			pInstanceDesc[i].Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
			glm::mat4x4 mat = glm::transpose(VecBottomLevelAS[i]->mesh->transform);
			memcpy(pInstanceDesc[i].Transform, &mat, sizeof(pInstanceDesc[i].Transform));
			pInstanceDesc[i].AccelerationStructure = VecBottomLevelAS[i]->Result->GetGPUVirtualAddress();
			pInstanceDesc[i].InstanceMask = 0xFF;
		}
		as->Instance->Unmap(0, nullptr);
	}
	
	// Create the TLAS

	CommandList* cmd = g_dx12_rhi->CmdQSync->AllocCmdList();

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC asDesc = {};
	asDesc.Inputs = inputs;

	if (VecBottomLevelAS.size() > 0)
		asDesc.Inputs.InstanceDescs = as->Instance->GetGPUVirtualAddress();
	asDesc.DestAccelerationStructureData = as->Result->GetGPUVirtualAddress();
	asDesc.ScratchAccelerationStructureData = as->Scratch->GetGPUVirtualAddress();

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC postInfo;

	cmd->CmdList->BuildRaytracingAccelerationStructure(&asDesc, 0, &postInfo);

	// We need to insert a UAV barrier before using the acceleration structures in a raytracing operation
	D3D12_RESOURCE_BARRIER uavBarrier = {};
	uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	uavBarrier.UAV.pResource = as->Result.Get();
	cmd->CmdList->ResourceBarrier(1, &uavBarrier);

	// create acceleration structure srv (not shader-visible yet)
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.RaytracingAccelerationStructure.Location = as->Result->GetGPUVirtualAddress();

	// copydescriptor needed when being used.
	GeomtryDHRing->AllocDescriptor(as->Descriptor.CpuHandle, as->Descriptor.GpuHandle);

	g_dx12_rhi->Device->CreateShaderResourceView(nullptr, &srvDesc, as->Descriptor.CpuHandle);

	g_dx12_rhi->CmdQSync->ExecuteCommandList(cmd);

	return as;
}

RTAS* DX12Impl::CreateBLAS(GfxMesh* mesh)
{
	RTAS* as = new  RTAS;
	as->mesh = mesh;

	VertexBuffer* vb = static_cast<VertexBuffer*>(mesh->Vb.get());
	IndexBuffer* ib = static_cast<IndexBuffer*>(mesh->Ib.get());
	D3D12_RAYTRACING_GEOMETRY_DESC geomDesc = {};
	geomDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
	geomDesc.Triangles.VertexBuffer.StartAddress = vb->resource->GetGPUVirtualAddress();
	geomDesc.Triangles.VertexBuffer.StrideInBytes = mesh->VertexStride;;
	geomDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
	geomDesc.Triangles.VertexCount = vb->numVertices;
	geomDesc.Triangles.IndexBuffer = ib->resource->GetGPUVirtualAddress();
	geomDesc.Triangles.IndexFormat = static_cast<DXGI_FORMAT>(mesh->IndexFormat);
	geomDesc.Triangles.IndexCount = ib->numIndices;
	geomDesc.Triangles.Transform3x4 = 0;

	if (mesh->bTransparent)
		geomDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
	else
		geomDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;



	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
	inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
	inputs.NumDescs = 1;
	inputs.pGeometryDescs = &geomDesc;
	inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
	g_dx12_rhi->Device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);

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

		g_dx12_rhi->Device->CreateCommittedResource(&kDefaultHeapProps, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&as->Scratch));
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


		g_dx12_rhi->Device->CreateCommittedResource(&kDefaultHeapProps, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr, IID_PPV_ARGS(&as->Result));
	}

	CommandList* cmd = g_dx12_rhi->CmdQSync->AllocCmdList();

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC asDesc = {};
	asDesc.Inputs = inputs;
	asDesc.DestAccelerationStructureData = as->Result->GetGPUVirtualAddress();
	asDesc.ScratchAccelerationStructureData = as->Scratch->GetGPUVirtualAddress();

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC postInfo;
	cmd->CmdList->BuildRaytracingAccelerationStructure(&asDesc, 0, &postInfo);

	/*D3D12_RESOURCE_BARRIER uavBarrier = {};
	uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	uavBarrier.UAV.pResource = as->Result.Get();
	cmd->CmdList->ResourceBarrier(1, &uavBarrier);*/

	g_dx12_rhi->CmdQSync->ExecuteCommandList(cmd);

	return as;
}



template<class BlotType>
std::string convertBlobToString(BlotType* pBlob)
{
	std::vector<char> infoLog(pBlob->GetBufferSize() + 1);
	memcpy(infoLog.data(), pBlob->GetBufferPointer(), pBlob->GetBufferSize());
	infoLog[pBlob->GetBufferSize()] = 0;
	return std::string(infoLog.data());
}

//static dxc::DxcDllSupport gDxcDllHelper;

ComPtr<ID3DBlob> compileShaderLibrary(wstring dir, wstring filename, wstring targetString, std::optional<vector< DxcDefine>>  Defines)
{
	Microsoft::WRL::ComPtr<IDxcUtils> pUtils;
	DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(pUtils.GetAddressOf()));

	Microsoft::WRL::ComPtr<IDxcCompiler3> pCompiler;
	DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(pCompiler.GetAddressOf()));

	Microsoft::WRL::ComPtr<IDxcIncludeHandler> dxcIncludeHandler;
	pUtils->CreateDefaultIncludeHandler(&dxcIncludeHandler);

	wstring Path = dir + filename;
	// Open and read the file
	std::ifstream shaderFile(Path);
	if (shaderFile.good() == false)
	{
		//msgBox("Can't open file " + wstring_2_string(std::wstring(filename)));
		return nullptr;
	}
	std::stringstream strStream;
	strStream << shaderFile.rdbuf();
	std::string shader = strStream.str();

	// Create blob from the string
	Microsoft::WRL::ComPtr<IDxcBlobEncoding> pTextBlob;
	pUtils->CreateBlob((LPBYTE)shader.c_str(), (uint32_t)shader.size(), CP_UTF8, pTextBlob.GetAddressOf());
	// Compile

	std::vector<LPCWSTR> arguments;
	//arguments.push_back(L"-spirv");

	/*arguments.push_back(L"-E");
	arguments.push_back(EntryPoint.c_str());*/

	//-T for the target profile (eg. ps_6_2)
	arguments.push_back(L"-T");
	arguments.push_back(targetString.c_str());

	arguments.push_back(L"-I");
	arguments.push_back(dir.c_str());

	DxcDefine* defines = nullptr;
	UINT32 numDefines = 0;
	if (Defines.has_value())
	{
		for (auto& d : Defines.value())
		{
			arguments.push_back(L"-D");
			arguments.push_back(LPWSTR(d.Name));
		}
	}

	DxcBuffer sourceBuffer;
	sourceBuffer.Ptr = pTextBlob->GetBufferPointer();
	sourceBuffer.Size = pTextBlob->GetBufferSize();
	sourceBuffer.Encoding = 0;

	Microsoft::WRL::ComPtr<IDxcResult> pCompileResult;
	HRESULT hr = pCompiler->Compile(&sourceBuffer, arguments.data(), arguments.size(), dxcIncludeHandler.Get(), IID_PPV_ARGS(pCompileResult.GetAddressOf()));

	//Error Handling
	Microsoft::WRL::ComPtr<IDxcBlobUtf8> pErrors;
	pCompileResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(pErrors.GetAddressOf()), nullptr);
	if (pErrors && pErrors->GetStringLength() > 0)
	{
		std::string log = (char*)pErrors->GetBufferPointer();

		std::string fileName;
		fileName.assign(filename.begin(), filename.end());
		std::stringstream ss;
		ss << fileName << " \n" << log;
		//msgBox("Compiler error:\n" + log);
		g_dx12_rhi->errorString += log;
		OutputDebugStringA(ss.str().c_str());
	}

	Microsoft::WRL::ComPtr<ID3DBlob> pShader;
	pCompileResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(pShader.GetAddressOf()), nullptr);
	if (pShader)
	{
		return pShader;
	}
	else
		return nullptr;
}



//ComPtr<ID3DBlob> DX12Impl::CreateShader(wstring FilePath, string EntryPoint, string Target)
//{
//	ComPtr<ID3DBlob> shader;
//
//#if defined(_DEBUG)
//	// Enable better shader debugging with the graphics debugging tools.
//	UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
//#else
//	UINT compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
//#endif
//
//	ID3DBlob* compilationMsgs = nullptr;
//
//	try
//	{
//		ThrowIfFailed(D3DCompileFromFile(FilePath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, EntryPoint.c_str(), Target.c_str(), compileFlags, 0, &shader, &compilationMsgs));
//	}
//	catch (const std::exception& e)
//	{
//		string errorStr = reinterpret_cast<const char*>(compilationMsgs->GetBufferPointer());
//		OutputDebugStringA(errorStr.c_str());
//		g_dx12_rhi->errorString += errorStr;
//		compilationMsgs->Release();
//		return nullptr;
//	}
//
//	return shader;
//}

ComPtr<ID3DBlob> DX12Impl::CreateShaderDXC(wstring Dir, wstring FileName, wstring EntryPoint, wstring Target, std::optional<vector< DxcDefine>> Defines)
{
	Microsoft::WRL::ComPtr<IDxcUtils> pUtils;
	DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(pUtils.GetAddressOf()));

	Microsoft::WRL::ComPtr<IDxcCompiler3> pCompiler;
	DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(pCompiler.GetAddressOf()));

	Microsoft::WRL::ComPtr<IDxcIncludeHandler> dxcIncludeHandler;
	pUtils->CreateDefaultIncludeHandler(&dxcIncludeHandler);

	wstring Path = Dir + FileName;
	// Open and read the file
	std::ifstream shaderFile(Path);
	if (shaderFile.good() == false)
	{
		//msgBox("Can't open file " + wstring_2_string(std::wstring(filename)));
		return nullptr;
	}
	std::stringstream strStream;
	strStream << shaderFile.rdbuf();
	std::string shader = strStream.str();

	// Create blob from the string
	Microsoft::WRL::ComPtr<IDxcBlobEncoding> pTextBlob;
	pUtils->CreateBlob((LPBYTE)shader.c_str(), (uint32_t)shader.size(), CP_UTF8, pTextBlob.GetAddressOf());
	// Compile

	std::vector<LPCWSTR> arguments;
	//arguments.push_back(L"-spirv");

	arguments.push_back(L"-E");
	arguments.push_back(EntryPoint.c_str());

	//-T for the target profile (eg. ps_6_2)
	arguments.push_back(L"-T");
	arguments.push_back(Target.c_str());

	arguments.push_back(L"-I");
	arguments.push_back(Dir.c_str());

	DxcDefine* defines = nullptr;
	UINT32 numDefines = 0;
	if (Defines.has_value())
	{
		for (auto& d : Defines.value())
		{
			arguments.push_back(L"-D");
			arguments.push_back(LPWSTR(d.Name));
		}
	}

	DxcBuffer sourceBuffer;
	sourceBuffer.Ptr = pTextBlob->GetBufferPointer();
	sourceBuffer.Size = pTextBlob->GetBufferSize();
	sourceBuffer.Encoding = 0;

	Microsoft::WRL::ComPtr<IDxcResult> pCompileResult;
	HRESULT hr = pCompiler->Compile(&sourceBuffer, arguments.data(), arguments.size(), dxcIncludeHandler.Get(), IID_PPV_ARGS(pCompileResult.GetAddressOf()));

	//Error Handling
	Microsoft::WRL::ComPtr<IDxcBlobUtf8> pErrors;
	pCompileResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(pErrors.GetAddressOf()), nullptr);
	if (pErrors && pErrors->GetStringLength() > 0)
	{
		std::string log = (char*)pErrors->GetBufferPointer();

		std::string fileName;
		fileName.assign(FileName.begin(), FileName.end());
		std::stringstream ss;
		ss << fileName << " \n" << log;
		//msgBox("Compiler error:\n" + log);
		g_dx12_rhi->errorString += log;
		OutputDebugStringA(ss.str().c_str());
	}

	Microsoft::WRL::ComPtr<ID3DBlob> pShader;
	pCompileResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(pShader.GetAddressOf()), nullptr);
	if (pShader)
	{

		return pShader;

	}
	else
		return nullptr;

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

ComPtr<ID3D12RootSignature> CreateRootSignature(ComPtr<ID3D12Device5> pDevice, const D3D12_ROOT_SIGNATURE_DESC& desc)
{
	ComPtr<ID3DBlob> pSigBlob;
	ComPtr<ID3DBlob> pErrorBlob;
	HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &pSigBlob, &pErrorBlob);
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
	PipelineConfig(uint32_t maxTraceRecursionDepth)
	{
		config.MaxTraceRecursionDepth = maxTraceRecursionDepth;

		subobject.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
		subobject.pDesc = &config;
	}

	D3D12_RAYTRACING_PIPELINE_CONFIG config = {};
	D3D12_STATE_SUBOBJECT subobject = {};
};

void RTPipelineStateObject::AddHitGroup(string name, string chs, string ahs)
{
	HitGroupInfo info;
	info.name = StringToWString(name);
	info.chs = StringToWString(chs);
	info.ahs = StringToWString(ahs);

	VecHitGroup.push_back(info);
}

void RTPipelineStateObject::AddShader(string shader, RTPipelineStateObject::ShaderType shaderType)
{
	ShaderBinding[shader].ShaderName = StringToWString(shader);;
	ShaderBinding[shader].Type = shaderType;
}

void RTPipelineStateObject::BindUAV(string shader, string name, UINT baseRegister)
{
	if (shader == "global")
	{
		BindingData binding;
		binding.name = name;
		binding.Type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;

		binding.BaseRegister = baseRegister;

		GlobalBinding.push_back(binding);
	}
	else
	{
		auto& bindingInfo = ShaderBinding[shader];

		BindingData binding;
		binding.name = name;
		binding.Type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;

		binding.BaseRegister = baseRegister;

		bindingInfo.Binding.push_back(binding);
	}
}

void RTPipelineStateObject::BindSRV(string shader, string name, UINT baseRegister)
{
	if (shader == "global")
	{
		BindingData binding;
		binding.name = name;
		binding.Type = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;

		binding.BaseRegister = baseRegister;

		GlobalBinding.push_back(binding);
	}
	else
	{
		auto& bindingInfo = ShaderBinding[shader];

		BindingData binding;
		binding.name = name;
		binding.Type = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;

		binding.BaseRegister = baseRegister;

		bindingInfo.Binding.push_back(binding);
	}
}



void RTPipelineStateObject::BindSampler(string shader, string name, UINT baseRegister)
{
	if (shader == "global")
	{
		BindingData binding;
		binding.name = name;
		binding.Type = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;

		binding.BaseRegister = baseRegister;

		GlobalBinding.push_back(binding);
	}
	else
	{
		auto& bindingInfo = ShaderBinding[shader];

		BindingData binding;
		binding.name = name;
		binding.Type = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;

		binding.BaseRegister = baseRegister;

		bindingInfo.Binding.push_back(binding);
	}
}

void RTPipelineStateObject::BindCBV(string shader, string name, UINT baseRegister, UINT size)
{
	if (shader == "global")
	{
		BindingData binding;
		binding.name = name;
		binding.Type = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;

		binding.BaseRegister = baseRegister;

		int div = size / 256;
		binding.cbSize = (div) * 256;

		if (size % 256 > 0)
			binding.cbSize += 256;

		GlobalBinding.push_back(binding);
	}
	else
	{
		auto& bindingInfo = ShaderBinding[shader];

		BindingData binding;
		binding.name = name;
		binding.Type = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;

		binding.BaseRegister = baseRegister;

		int div = size / 256;
		binding.cbSize = (div) * 256;

		if (size % 256 > 0)
			binding.cbSize += 256;

		bindingInfo.Binding.push_back(binding);
	}
}

void RTPipelineStateObject::BeginShaderTable()
{
}

void RTPipelineStateObject::SetGlobalBinding(CommandList* CommandList)
{
	UINT RPI = 0;
	for (auto& bi : GlobalBinding)
	{
		CommandList->CmdList.Get()->SetComputeRootDescriptorTable(RPI++, bi.GPUHandle);
	}
}

void RTPipelineStateObject::EndShaderTable(UINT NumInstance)
{
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
		NumShaderTableEntry += NumInstance * VecHitGroup.size();

		ShaderTableSize = ShaderTableEntrySize * NumShaderTableEntry;

		// allocate shader table
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
			bufDesc.Width = ShaderTableSize * g_dx12_rhi->NumFrame;

			const D3D12_HEAP_PROPERTIES kUploadHeapProps =
			{
				D3D12_HEAP_TYPE_UPLOAD,
				D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
				D3D12_MEMORY_POOL_UNKNOWN,
				0,
				0,
			};

			g_dx12_rhi->Device->CreateCommittedResource(&kUploadHeapProps, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ShaderTable));
			NAME_D3D12_OBJECT(ShaderTable);
		}
	}
	

	// raygen : simple, it is just the begin of table
	// miss : raygen + miss index * EntrySize
	// hit : raygen + miss(N) + instanceIndex
	uint8_t* pData;
	HRESULT hr = ShaderTable->Map(0, nullptr, (void**)&pData);

	pData += ShaderTableSize * g_dx12_rhi->CurrentFrameIndex;;

	D3D12_GPU_VIRTUAL_ADDRESS va = ShaderTable->GetGPUVirtualAddress();

	ComPtr<ID3D12StateObjectProperties> RtsoProps;
	RTPipelineState->QueryInterface(IID_PPV_ARGS(&RtsoProps));

	if (FAILED(hr))
	{
		HRESULT hrRemoved = g_dx12_rhi->Device->GetDeviceRemovedReason();
		if (FAILED(hrRemoved))
		{
			for (auto& sb : ShaderBinding)
			{
				BindingInfo& bindingInfo = sb.second;
			}
		}
		
	}
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
	for(int InstanceIndex=0;InstanceIndex<NumInstance;InstanceIndex++)
	{

		//auto& HitProgramInfo = HitProgramBinding[InstanceIndex];
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


	ShaderTable->Unmap(0, nullptr);
}

void RTPipelineStateObject::SetUAV(string shader, string bindingName, D3D12_GPU_DESCRIPTOR_HANDLE uavHandle, INT instanceIndex /*= -1*/)
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

void RTPipelineStateObject::SetSRV(string shader, string bindingName, D3D12_GPU_DESCRIPTOR_HANDLE srvHandle, INT instanceIndex /*= -1*/)
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

void RTPipelineStateObject::ResetHitProgram(UINT instanceIndex)
{
	for (auto& HG : VecHitGroup)
	{
		HG.HitProgramBinding[instanceIndex].VecData.clear();
	}
}

void RTPipelineStateObject::StartHitProgram(string HitGroup, UINT instanceIndex)
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

void RTPipelineStateObject::AddDescriptor2HitProgram(string HitGroup, D3D12_GPU_DESCRIPTOR_HANDLE srvHandle, UINT instanceIndex)
{
	map<UINT, HitProgramData>* HitProgram = nullptr;
	for (auto& HG : VecHitGroup)
	{
		if (HG.name == StringToWString(HitGroup))
			HitProgram = &HG.HitProgramBinding;
	}

	(*HitProgram)[instanceIndex].VecData.push_back(srvHandle);
}

void RTPipelineStateObject::SetSampler(string shader, string bindingName, Sampler* sampler, INT instanceIndex /*= -1*/)
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
					bd.GPUHandle = sampler->Descriptor.GpuHandle;
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
					bd.GPUHandle = sampler->Descriptor.GpuHandle;
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

void RTPipelineStateObject::SetCBVValue(string shader, string bindingName, void* pData, INT instanceIndex /*= -1*/)
{
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
					auto Alloc = g_dx12_rhi->GlobalCBRing->AllocGPUMemory(bd.cbSize);
					UINT64 GPUAddr = std::get<0>(Alloc);
					UINT8* pMapped = std::get<1>(Alloc);

					memcpy((void*)pMapped, pData, bd.cbSize);

					D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle;
					D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle;

					// ring is advanced at the begining of frame. so descriptors from multiple frame is not overlapped.
					g_dx12_rhi->GlobalDHRing->AllocDescriptor(CpuHandle, GpuHandle);

					D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
					cbvDesc.BufferLocation = GPUAddr;
					cbvDesc.SizeInBytes = bd.cbSize;
					g_dx12_rhi->Device->CreateConstantBufferView(&cbvDesc, CpuHandle);

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
					auto Alloc = g_dx12_rhi->GlobalCBRing->AllocGPUMemory(bd.cbSize);
					UINT64 GPUAddr = std::get<0>(Alloc);
					UINT8* pMapped = std::get<1>(Alloc);

					memcpy((void*)pMapped, pData, bd.cbSize);

					D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle;
					D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle;

					// ring is advanced at the begining of frame. so descriptors from multiple frame is not overlapped.
					g_dx12_rhi->GlobalDHRing->AllocDescriptor(CpuHandle, GpuHandle);

					D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
					cbvDesc.BufferLocation = GPUAddr;
					cbvDesc.SizeInBytes = bd.cbSize;
					g_dx12_rhi->Device->CreateConstantBufferView(&cbvDesc, CpuHandle);

					bd.GPUHandle = GpuHandle;
				
					bFound = true;
				}
			}
			assert(bFound == true);
		}
	}
	//else // hitprogram : There can be multiple hitprogram entry with same shader name, so we need another data structure. (HitProgramBinding)
	//{
	//	// SetXXX should be called by the binding order, because it is "push_back"ed to vector.
	//	BindingInfo& bi = ShaderBinding[shader];
	//	for (auto&bd : bi.Binding)
	//	{
	//		if (bd.name == bindingName)
	//		{
	//			auto& cb = bd.cbs[g_dx12_rhi->CurrentFrameIndex];
	//			UINT8* pMapped = (UINT8*)cb->MemMapped + size * instanceIndex;
	//			memcpy((void*)pMapped, pData, size);

	//			HitProgramBindingCHS[instanceIndex].HitGroupName = MapHitGroup[shader].name;

	//			D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle;
	//			D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle;

	//			// ring is advanced at the begining of frame. so descriptors from multiple frame is not overlapped.
	//			g_dx12_rhi->GlobalDHRing->AllocDescriptor(CpuHandle, GpuHandle);

	//			D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
	//			cbvDesc.BufferLocation = cb->resource->GetGPUVirtualAddress();
	//			cbvDesc.SizeInBytes = cb->Size;
	//			g_dx12_rhi->Device->CreateConstantBufferView(&cbvDesc, CpuHandle);

	//			HitProgramBindingCHS[instanceIndex].VecData.push_back(GpuHandle);
	//		}
	//	}
	//}
}

void RTPipelineStateObject::SetCBVValue(string shader, string bindingName, UINT64 GPUAddr, INT instanceIndex /*= -1*/)
{
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
		/*			auto Alloc = g_dx12_rhi->GlobalCBRing->AllocGPUMemory(bd.cbSize);
					UINT64 GPUAddr = std::get<0>(Alloc);
					UINT8* pMapped = std::get<1>(Alloc);

					memcpy((void*)pMapped, pData, bd.cbSize);*/

					D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle;
					D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle;

					// ring is advanced at the begining of frame. so descriptors from multiple frame is not overlapped.
					g_dx12_rhi->GlobalDHRing->AllocDescriptor(CpuHandle, GpuHandle);

					D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
					cbvDesc.BufferLocation = GPUAddr;
					cbvDesc.SizeInBytes = bd.cbSize;
					g_dx12_rhi->Device->CreateConstantBufferView(&cbvDesc, CpuHandle);

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
					/*auto Alloc = g_dx12_rhi->GlobalCBRing->AllocGPUMemory(bd.cbSize);
					UINT64 GPUAddr = std::get<0>(Alloc);
					UINT8* pMapped = std::get<1>(Alloc);

					memcpy((void*)pMapped, pData, bd.cbSize);*/

					D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle;
					D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle;

					// ring is advanced at the begining of frame. so descriptors from multiple frame is not overlapped.
					g_dx12_rhi->GlobalDHRing->AllocDescriptor(CpuHandle, GpuHandle);

					D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
					cbvDesc.BufferLocation = GPUAddr;
					cbvDesc.SizeInBytes = bd.cbSize;
					g_dx12_rhi->Device->CreateConstantBufferView(&cbvDesc, CpuHandle);

					bd.GPUHandle = GpuHandle;

					bFound = true;
				}
			}
			assert(bFound == true);
		}
	}
}

bool RTPipelineStateObject::InitRS(std::wstring Dir, std::wstring ShaderFile, std::optional<vector< DxcDefine>>  Defines)
{
	vector<D3D12_STATE_SUBOBJECT> subobjects;

	// dxil + Hitgroup count + RS + Export + shaderconfig + export + pipelineconfig + global RS
	int numSubobjects = 1 + VecHitGroup.size() + ShaderBinding.size() * 2 + 2 + 1 + 1;
	subobjects.resize(numSubobjects);

	uint32_t index = 0;

	// dxil lib
	//wstring wShaderFile = StringToWString(ShaderFile);
	//wstring path = Dir + ShaderFile;
	ComPtr<ID3DBlob> pDxilLib = compileShaderLibrary(Dir, ShaderFile, L"lib_6_3", Defines);

	vector<const WCHAR*> entryPoints;
	entryPoints.reserve(ShaderBinding.size());
	for (auto& sb : ShaderBinding)
	{
		entryPoints.push_back(sb.second.ShaderName.c_str());
	}


	DxilLibrary dxilLib = DxilLibrary(pDxilLib, entryPoints.data(), entryPoints.size());
	subobjects[index++] = dxilLib.stateSubobject; // 0 Library

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

	// root signature
	BindingInfo* pBI = nullptr;
	vector< D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION> vecAssociation;
	vecAssociation.reserve(ShaderBinding.size());
	for (auto& sb : ShaderBinding)
	{
		BindingInfo& bindingInfo = sb.second;
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
				Range.NumDescriptors = 1;
				Range.RegisterSpace = 0;
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

		bindingInfo.RS = CreateRootSignature(g_dx12_rhi->Device, Desc);
		NAME_D3D12_OBJECT(bindingInfo.RS);

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

	// shader config
	ShaderConfig shaderConfig(MaxAttributeSizeInBytes, MaxPayloadSizeInBytes);
	D3D12_STATE_SUBOBJECT* shaderConfigSubObject = &subobjects[index];
	subobjects[index++] = shaderConfig.subobject;

	// shaderconfig export association
	vector<const WCHAR*> vecShaderExports;
	entryPoints.reserve(ShaderBinding.size());
	for (auto& sb : ShaderBinding)
	{
		vecShaderExports.push_back(sb.second.ShaderName.c_str());
	}

	ExportAssociation configAssociation(vecShaderExports.data(), vecShaderExports.size(), shaderConfigSubObject);
	subobjects[index++] = configAssociation.subobject;


	// pipeline config
	PipelineConfig config(MaxRecursion);
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
		Range.NumDescriptors = 1;
		Range.RegisterSpace = 0;
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

	GlobalRS = CreateRootSignature(g_dx12_rhi->Device, Desc);
	NAME_D3D12_OBJECT(GlobalRS);

	pInterfaceGlobalRS = GlobalRS.Get();
	subobjectGlobalRS.pDesc = &pInterfaceGlobalRS;
	subobjectGlobalRS.Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;

	subobjects[index++] = subobjectGlobalRS;

	
	// Create the RTPSO
	D3D12_STATE_OBJECT_DESC descRTSO;
	descRTSO.NumSubobjects = subobjects.size(); // 10
	descRTSO.pSubobjects = subobjects.data();
	descRTSO.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;

	HRESULT hr = g_dx12_rhi->Device->CreateStateObject(&descRTSO, IID_PPV_ARGS(&RTPipelineState));
	if (FAILED(hr))
	{
		std::string ShaderFileA;
		ShaderFileA.assign(ShaderFile.begin(), ShaderFile.end());

		std::stringstream ss;
		ss << "Failed to compile shader : " << ShaderFileA << "\n";
		g_dx12_rhi->errorString += ss.str();
		OutputDebugStringA(ss.str().c_str());
		return false;
	}

	NAME_D3D12_OBJECT(RTPipelineState);

	

	return true;
}

void RTPipelineStateObject::DispatchRay(UINT width, UINT height, CommandList* CommandList, UINT NumInstance)
{
	D3D12_DISPATCH_RAYS_DESC raytraceDesc = {};
	raytraceDesc.Width = width;
	raytraceDesc.Height = height;
	raytraceDesc.Depth = 1;

	D3D12_GPU_VIRTUAL_ADDRESS StartAddress = ShaderTable->GetGPUVirtualAddress() + ShaderTableSize * g_dx12_rhi->CurrentFrameIndex;

	raytraceDesc.RayGenerationShaderRecord.StartAddress = StartAddress ;
	raytraceDesc.RayGenerationShaderRecord.SizeInBytes = ShaderTableEntrySize;

	// Miss is the second entry in the shader-table
	UINT NumMissShader = 0;
	for (auto& sb : ShaderBinding)
	{
		if (sb.second.Type == MISS)
			NumMissShader++;
	}
	size_t missOffset = ShaderTableEntrySize * 1;
	raytraceDesc.MissShaderTable.StartAddress = StartAddress + missOffset;
	raytraceDesc.MissShaderTable.StrideInBytes = ShaderTableEntrySize;
	raytraceDesc.MissShaderTable.SizeInBytes = ShaderTableEntrySize * NumMissShader;

	
	 // Hit is the third entry in the shader-table
	size_t hitOffset = missOffset + NumMissShader * ShaderTableEntrySize;
	raytraceDesc.HitGroupTable.StartAddress = StartAddress + hitOffset;
	raytraceDesc.HitGroupTable.StrideInBytes = ShaderTableEntrySize;
	raytraceDesc.HitGroupTable.SizeInBytes = ShaderTableEntrySize * VecHitGroup.size() *NumInstance;

	// Bind the empty root signature
	g_dx12_rhi->GlobalCmdList->CmdList->SetComputeRootSignature(GlobalRS.Get());

	UINT RPI = 0;
	for (auto& bi : GlobalBinding)
	{
		CommandList->CmdList.Get()->SetComputeRootDescriptorTable(RPI++, bi.GPUHandle);
	}

	g_dx12_rhi->GlobalCmdList->CmdList->SetPipelineState1(RTPipelineState.Get());
	
	g_dx12_rhi->GlobalCmdList->CmdList->DispatchRays(&raytraceDesc);
}

void DescriptorHeapRing::Init(DescriptorHeap* InDHHeap, UINT InNumDescriptors, UINT InNumFrame)
{
	DHeap = InDHHeap;
	NumDescriptors = InNumDescriptors;
	NumFrame = InNumFrame;
	DescriptorSize = DHeap->DescriptorSize;

	DHeap->AllocDescriptors(CPUHeapStart, GPUHeapStart, NumDescriptors * NumFrame);
}

UINT DescriptorHeapRing::AllocDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE& cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE& gpuHandle, UINT32 Num)
{
	UINT offset = NumAllocated * DescriptorSize + NumDescriptors * DescriptorSize * CurrentFrame;
	cpuHandle.ptr = CPUHeapStart.ptr + offset;// NumAllocated* DescriptorSize + NumDescriptors * DescriptorSize * CurrentFrame;
	gpuHandle.ptr = GPUHeapStart.ptr + offset;//  NumAllocated* DescriptorSize + NumDescriptors * DescriptorSize * CurrentFrame;

	NumAllocated += Num;

	return offset;
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

CommandQueue::CommandQueue()
{
	ThrowIfFailed(g_dx12_rhi->Device->CreateFence(CurrentFenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
	// Create an event handle to use for frame synchronization.
	m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

	ThrowIfFailed(g_dx12_rhi->Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&CmdQueue)));
	NAME_D3D12_OBJECT(CmdQueue);

	CommandListPool.reserve(CommandListPoolSize);
	for (int i = 0; i < CommandListPoolSize; i++)
	{
		CommandList * cmdList = new CommandList;
		ThrowIfFailed(g_dx12_rhi->Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdList->CmdAllocator)));
		NAME_D3D12_OBJECT(cmdList->CmdAllocator);

		ThrowIfFailed(g_dx12_rhi->Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdList->CmdAllocator.Get(), nullptr, IID_PPV_ARGS(&cmdList->CmdList)));
		cmdList->CmdList->Close();
		NAME_D3D12_OBJECT(cmdList->CmdList);

		CommandListPool.emplace_back(shared_ptr<CommandList>(cmdList));
	}
}

CommandQueue::~CommandQueue()
{
}

CommandList * CommandQueue::AllocCmdList()
{
	std::lock_guard<std::mutex> lock(CmdAllocMtx);

	CommandList* cmdList = CommandListPool[CurrentIndex].get();
	if(cmdList->Fence.has_value())
		WaitFenceValue(cmdList->Fence.value());

	CurrentIndex++;

	CurrentIndex = CurrentIndex % CommandListPoolSize;

	cmdList->Fence = std::nullopt;
	cmdList->Reset();
	return cmdList;
}

void CommandQueue::ExecuteCommandList(CommandList * cmd)
{
	cmd->CmdList->Close();
	ID3D12CommandList* ppCommandListsEnd[] = { cmd->CmdList.Get() };
	CmdQueue->ExecuteCommandLists(_countof(ppCommandListsEnd), ppCommandListsEnd);
}

void CommandQueue::WaitGPU()
{
	CmdQueue->Signal(m_fence.Get(), CurrentFenceValue);
	m_fence->SetEventOnCompletion(CurrentFenceValue, m_fenceEvent);
	WaitForSingleObject(m_fenceEvent, INFINITE);
	
	CurrentFenceValue++;
}

void CommandQueue::WaitFenceValue(UINT64 fenceValue)
{
	m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent);
	WaitForSingleObject(m_fenceEvent, INFINITE);
}

void CommandQueue::SignalCurrentFence()
{
	CmdQueue->Signal(m_fence.Get(), CurrentFenceValue);
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

ConstantBufferRingBuffer::ConstantBufferRingBuffer(UINT InSize, UINT InNumFrame)
{
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
	resDesc.Width = InSize * g_dx12_rhi->NumFrame;;
	resDesc.Height = 1;
	resDesc.DepthOrArraySize = 1;
	resDesc.MipLevels = 1;
	resDesc.Format = DXGI_FORMAT_UNKNOWN;
	resDesc.SampleDesc.Count = 1;
	resDesc.SampleDesc.Quality = 0;
	resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	resDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

	ThrowIfFailed(g_dx12_rhi->Device->CreateCommittedResource(
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
	// create shader resource view
	D3D12_SHADER_RESOURCE_VIEW_DESC bufferSRVDesc;
	bufferSRVDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	bufferSRVDesc.Format = DXGI_FORMAT_R32_TYPELESS;
	bufferSRVDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
	bufferSRVDesc.Buffer.StructureByteStride = 0;
	bufferSRVDesc.Buffer.FirstElement = 0;
	bufferSRVDesc.Buffer.NumElements = NumElements;
	bufferSRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

	// TODO : should be GlobalDHRing? if object instance are to move.
	
	g_dx12_rhi->TextureDHRing->AllocDescriptor(SRV.CpuHandle, SRV.GpuHandle);

	g_dx12_rhi->Device->CreateShaderResourceView(resource.Get(), &bufferSRVDesc, SRV.CpuHandle);

	Type = BYTE_ADDRESS;
}

void Buffer::MakeStructuredBufferSRV()
{
	// create shader resource view
	D3D12_SHADER_RESOURCE_VIEW_DESC bufferSRVDesc;
	bufferSRVDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	bufferSRVDesc.Format = DXGI_FORMAT_UNKNOWN;
	bufferSRVDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
	bufferSRVDesc.Buffer.StructureByteStride = ElementSize;
	bufferSRVDesc.Buffer.FirstElement = 0;
	bufferSRVDesc.Buffer.NumElements = NumElements;
	bufferSRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

	// TODO : should be GlobalDHRing? if object instance are to move.

	g_dx12_rhi->TextureDHRing->AllocDescriptor(SRV.CpuHandle, SRV.GpuHandle);

	g_dx12_rhi->Device->CreateShaderResourceView(resource.Get(), &bufferSRVDesc, SRV.CpuHandle);

	Type = STRUCTURED;
}

