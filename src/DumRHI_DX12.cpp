#include "DumRHI_DX12.h"

#include <DirectXMath.h>
#include "DirectXTex.h"
#include "d3dx12.h"
#define GLM_FORCE_CTOR_INIT

#include "glm/glm.hpp"
#define GLM_ENABLE_EXPERIMENTAL
#include "glm/gtx/transform.hpp"
#include "glm/mat4x4.hpp"
#include "glm/fwd.hpp"
#include "DXCAPI/dxcapi.use.h"
#include "Utils.h"
#include <sstream>
#include <fstream>

#include <comdef.h>

// DirectX Tex
#include "DirectXTex July 2017/Include/DirectXTex.h"
#ifdef _DEBUG
#pragma comment(lib, "Debug/DirectXTex.lib")
#else
#pragma comment(lib, "Release/DirectXTex.lib")
#endif

#define align_to(_alignment, _val) (((_val + _alignment - 1) / _alignment) * _alignment)

using namespace std;

DumRHI_DX12* g_dx12_rhi;

ThreadDescriptorHeapPool::ThreadDescriptorHeapPool()
{
	DHeap = g_dx12_rhi->SRVCBVDescriptorHeapShaderVisible.get();
}

void ThreadDescriptorHeapPool::AllocDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE& cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE& gpuHandle)
{
	if ((PoolIndex-StartIndex) >= PoolSize)
		PoolIndex = 0;

	cpuHandle.ptr = DHeap->CPUHeapStart + PoolIndex * DHeap->DescriptorSize;
	gpuHandle.ptr = DHeap->GPUHeapStart + PoolIndex * DHeap->DescriptorSize;

	PoolIndex++;

}

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
#include <windows.h>

void DumRHI_DX12::BeginFrame()
{
	const UINT64 lastCompletedFence = m_fence->GetCompletedValue();

	CurrentFrameIndex = m_swapChain->GetCurrentBackBufferIndex();

	// wait until gpu processing for this frame resource is completed
	{
		UINT64 ThisFrameFenceValue = FrameResourceVec[CurrentFrameIndex].FenceValue;
		ThrowIfFailed(m_fence->SetEventOnCompletion(ThisFrameFenceValue, m_fenceEvent));
		WaitForSingleObject(m_fenceEvent, INFINITE);
	}


	GetCurrentCA()->Reset();
	ThrowIfFailed(CommandList->Reset(GetCurrentCA(), nullptr));

	for (auto& CA : FrameResourceVec[CurrentFrameIndex].VecCommandAllocatorMeshDraw)
	{
		CA->Reset();
	}
	


	ID3D12DescriptorHeap* ppHeaps[] = { SRVCBVDescriptorHeapShaderVisible->DH.Get(), SamplerDescriptorHeapShaderVisible->DH.Get() };
	CommandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
	

	g_dx12_rhi->GlobalDHRing->Advance();

	for (auto& tex : DynamicTextures)
	{
		if (tex->textureDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
		{
			g_dx12_rhi->GlobalDHRing->AllocDescriptor(tex->CpuHandleUAV, tex->GpuHandleUAV);


			D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Format = tex->textureDesc.Format;

			g_dx12_rhi->Device->CreateUnorderedAccessView(tex->resource.Get(), nullptr, &uavDesc, tex->CpuHandleUAV);
		}

		// alloc dh and create srv
		g_dx12_rhi->GlobalDHRing->AllocDescriptor(tex->CpuHandleSRV, tex->GpuHandleSRV);

		D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc = {};
		SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		if (tex->textureDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
			SrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
		else
			SrvDesc.Format = tex->textureDesc.Format;

		SrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		SrvDesc.Texture2D.MipLevels = tex->textureDesc.MipLevels;
		g_dx12_rhi->Device->CreateShaderResourceView(tex->resource.Get(), &SrvDesc, tex->CpuHandleSRV);
	}
}


void DumRHI_DX12::EndFrame()
{
	// Present and update the frame index for the next frame.
	ThrowIfFailed(m_swapChain->Present(0, 0), &g_dx12_rhi->AM_CL_Handle);

	// Signal and increment the fence value.
	m_fenceValue++;

	FrameResourceVec[CurrentFrameIndex].FenceValue = m_fenceValue;
	ThrowIfFailed(CommandQueue->Signal(m_fence.Get(), m_fenceValue));
}

void DumRHI_DX12::WaitGPU()
{
	m_fenceValue++;

	CommandQueue->Signal(m_fence.Get(), m_fenceValue);

	m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
	WaitForSingleObject(m_fenceEvent, INFINITE);
}

shared_ptr<ConstantBuffer> DumRHI_DX12::CreateConstantBuffer(int Size, UINT NumView)
{
	ConstantBuffer* cbuffer = new ConstantBuffer;

	cbuffer->Size = Size;

	D3D12_HEAP_PROPERTIES heapProp;
	heapProp.Type = D3D12_HEAP_TYPE_UPLOAD;
	heapProp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heapProp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	heapProp.CreationNodeMask = 1;
	heapProp.VisibleNodeMask = 1;

	D3D12_RESOURCE_DESC resDesc;

	resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	resDesc.Alignment = 0;
	resDesc.Width = Size * NumView;
	resDesc.Height = 1;
	resDesc.DepthOrArraySize = 1;
	resDesc.MipLevels = 1;
	resDesc.Format = DXGI_FORMAT_UNKNOWN;
	resDesc.SampleDesc.Count = 1;
	resDesc.SampleDesc.Quality = 0;
	resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	resDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

	stringstream ss;
	ss << "CreateConstantBuffer : " << resDesc.Width << "\n";
	OutputDebugStringA(ss.str().c_str());

	ThrowIfFailed(Device->CreateCommittedResource(
		&heapProp,
		D3D12_HEAP_FLAG_NONE,
		&resDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&cbuffer->resource)));

	NAME_D3D12_OBJECT(cbuffer->resource);


	CD3DX12_RANGE readRange(0, 0);		// We do not intend to read from this resource on the CPU.
	ThrowIfFailed(cbuffer->resource->Map(0, &readRange, reinterpret_cast<void**>(&cbuffer->MemMapped)));

	shared_ptr<ConstantBuffer> retPtr = shared_ptr<ConstantBuffer>(cbuffer);
	//cbVec.push_back(retPtr);

	return retPtr;
}

shared_ptr<Sampler> DumRHI_DX12::CreateSampler(D3D12_SAMPLER_DESC& InSamplerDesc)
{
	Sampler* sampler = new Sampler;
	sampler->SamplerDesc = InSamplerDesc;
	SamplerDescriptorHeapShaderVisible->AllocDescriptor(sampler->CpuHandle, sampler->GpuHandle);
	Device->CreateSampler(&sampler->SamplerDesc, sampler->CpuHandle);

	return shared_ptr<Sampler>(sampler);
}

std::shared_ptr<Buffer> DumRHI_DX12::CreateBuffer(UINT Size)
{
	Buffer * buffer = new Buffer;
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
	bufDesc.Width = Size;

	ThrowIfFailed(Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE, &bufDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer->resource)));

	NAME_D3D12_OBJECT(buffer->resource);

	// create shader resource view
	D3D12_SHADER_RESOURCE_VIEW_DESC bufferSRVDesc;
	bufferSRVDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	bufferSRVDesc.Format = DXGI_FORMAT_R32_TYPELESS;
	bufferSRVDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
	bufferSRVDesc.Buffer.StructureByteStride = 0;
	bufferSRVDesc.Buffer.FirstElement = 0;
	bufferSRVDesc.Buffer.NumElements = static_cast<UINT>(Size) / sizeof(float); // byte address buffer
	bufferSRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

	// TODO : should be GlobalDHRing? if object instance are to move.
	TextureDHRing->AllocDescriptor(buffer->CpuHandleSRV, buffer->GpuHandleSRV);

	Device->CreateShaderResourceView(buffer->resource.Get(), &bufferSRVDesc, buffer->CpuHandleSRV);

	return shared_ptr<Buffer>(buffer);;
}

shared_ptr<IndexBuffer> DumRHI_DX12::CreateIndexBuffer(DXGI_FORMAT Format, UINT Size, void* SrcData)
{
	IndexBuffer* ib = new IndexBuffer;

	stringstream ss;
	ss << "CreateIndexBuffer : " << Size << "\n";
	OutputDebugStringA(ss.str().c_str());

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

		UpdateSubresources<1>(CommandList.Get(), ib->resource.Get(), UploadHeap.Get(), 0, 0, 1, &indexData);
		CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(ib->resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));

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
		GeomtryDHRing->AllocDescriptor(ib->CpuHandleSRV, ib->GpuHandleSRV);

		Device->CreateShaderResourceView(ib->resource.Get(), &vertexSRVDesc, ib->CpuHandleSRV);


		ThrowIfFailed(CommandList->Close());
		ID3D12CommandList* ppCommandLists[] = { CommandList.Get() };
		CommandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
		//ThrowIfFailed(CommandList->Reset(CommandAllocator.Get(), nullptr));
		ThrowIfFailed(CommandList->Reset(GetCurrentCA(), nullptr));

		g_dx12_rhi->WaitGPU();
		// Create synchronization objects and wait until assets have been uploaded to the GPU.
	}

	return shared_ptr<IndexBuffer>(ib);
}

shared_ptr<VertexBuffer> DumRHI_DX12::CreateVertexBuffer(UINT Size, UINT Stride, void* SrcData)
{
	VertexBuffer* vb = new VertexBuffer;
	
	stringstream ss;
	ss << "CreateVertexBuffer : " << Size << "\n";
	OutputDebugStringA(ss.str().c_str());
	
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

		UpdateSubresources<1>(CommandList.Get(), vb->resource.Get(), UploadHeap.Get(), 0, 0, 1, &vertexData);
		CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(vb->resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));

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
		GeomtryDHRing->AllocDescriptor(vb->CpuHandleSRV, vb->GpuHandleSRV);

		Device->CreateShaderResourceView(vb->resource.Get(), &vertexSRVDesc, vb->CpuHandleSRV);

		ThrowIfFailed(CommandList->Close());
		ID3D12CommandList* ppCommandLists[] = { CommandList.Get() };
		CommandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
		//ThrowIfFailed(CommandList->Reset(CommandAllocator.Get(), nullptr));
		ThrowIfFailed(CommandList->Reset(GetCurrentCA(), nullptr));

		g_dx12_rhi->WaitGPU();
	}

	return shared_ptr<VertexBuffer>(vb);
}

void DumRHI_DX12::PresentBarrier(Texture* rt)
{
	D3D12_RESOURCE_BARRIER BarrierDesc = {};
	BarrierDesc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	BarrierDesc.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	BarrierDesc.Transition.pResource = rt->resource.Get();
	BarrierDesc.Transition.Subresource = 0;
	BarrierDesc.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	BarrierDesc.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
	CommandList->ResourceBarrier(1, &BarrierDesc);
}

DumRHI_DX12::DumRHI_DX12(ComPtr<ID3D12Device5> InDevice)
	:Device(InDevice)
{
	//GFSDK_Aftermath_Result result = GFSDK_Aftermath_DX12_Initialize(GFSDK_Aftermath_Version::GFSDK_Aftermath_Version_API, GFSDK_Aftermath_FeatureFlags::GFSDK_Aftermath_FeatureFlags_Maximum, Device.Get());

	g_dx12_rhi = this;

	// Describe and create the command queue.
	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

	ThrowIfFailed(Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&CommandQueue)));
	NAME_D3D12_OBJECT(CommandQueue);

	for (int i = 0; i < NumFrame; i++)
	{
		FrameResource frameRes;
		frameRes.CommandAllocator;
		ThrowIfFailed(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frameRes.CommandAllocator)));
		NAME_D3D12_OBJECT(frameRes.CommandAllocator);

		const int NumMeshDraw = NumDrawMeshCommandList;
		frameRes.VecCommandAllocatorMeshDraw.resize(NumMeshDraw);
		for (int m = 0; m < NumMeshDraw; m++)
		{

			ThrowIfFailed(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frameRes.VecCommandAllocatorMeshDraw[m])));
			NAME_D3D12_OBJECT(frameRes.VecCommandAllocatorMeshDraw[m]);
		}
		
		FrameResourceVec.push_back(frameRes);

	}

	ThrowIfFailed(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, FrameResourceVec[0].CommandAllocator.Get(), nullptr, IID_PPV_ARGS(&CommandList)));

	//GFSDK_Aftermath_Result ar = GFSDK_Aftermath_DX12_CreateContextHandle(CommandList.Get(), &AM_CL_Handle);

	NAME_D3D12_OBJECT(CommandList);

	for (int i = 0; i < NumDrawMeshCommandList; i++)
	{
		/*ThrowIfFailed(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&DrawMeshCommandAllocator[i])));
		NAME_D3D12_OBJECT(DrawMeshCommandAllocator[i]);*/
		FrameResource& frameRes = FrameResourceVec[0];

		ThrowIfFailed(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frameRes.VecCommandAllocatorMeshDraw[i].Get(), nullptr, IID_PPV_ARGS(&DrawMeshCommandList[i])));
		NAME_D3D12_OBJECT(DrawMeshCommandList[i]);

		DrawMeshCommandList[i]->Close();
	}

	vector< ID3D12CommandList*> vecCL;
	for (int i = 0; i < NumDrawMeshCommandList; i++)
		vecCL.push_back(DrawMeshCommandList[i].Get());
	CommandQueue->ExecuteCommandLists(vecCL.size(), &vecCL[0]);

	{
		RTVDescriptorHeap = std::make_unique<DescriptorHeap>();
		D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
		HeapDesc.NumDescriptors = 100;
		HeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		RTVDescriptorHeap->Init(HeapDesc);

		NAME_D3D12_OBJECT(RTVDescriptorHeap->DH);
	}
	

	{
		DSVDescriptorHeap = std::make_unique<DescriptorHeap>();

		D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
		HeapDesc.NumDescriptors = 1;
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
	
	GlobalDHRing = std::make_unique<DescriptorHeapRing>();
	GlobalDHRing->Init(SRVCBVDescriptorHeapShaderVisible.get(), 10000, NumFrame);

	TextureDHRing = std::make_unique<DescriptorHeapRing>();
	TextureDHRing->Init(SRVCBVDescriptorHeapShaderVisible.get(), 10000, NumFrame);

	GeomtryDHRing = std::make_unique<DescriptorHeapRing>();
	GeomtryDHRing->Init(SRVCBVDescriptorHeapShaderVisible.get(), 10000, NumFrame);
	
	m_fenceValue = 0;
	ThrowIfFailed(g_dx12_rhi->Device->CreateFence(m_fenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
	// Create an event handle to use for frame synchronization.
	m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

	// Close the command list and execute it to begin the initial GPU setup.
	ThrowIfFailed(CommandList->Close());
	ID3D12CommandList* ppCommandLists[] = { CommandList.Get() };
	CommandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
	//ThrowIfFailed(CommandList->Reset(CommandAllocator.Get(), nullptr));
	ThrowIfFailed(CommandList->Reset(GetCurrentCA(), nullptr));

	g_dx12_rhi->WaitGPU();
}


DumRHI_DX12::~DumRHI_DX12()
{
	m_fenceValue++;
	CommandQueue->Signal(m_fence.Get(), m_fenceValue);
	m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
	WaitForSingleObject(m_fenceEvent, INFINITE);
}

Shader::Shader(UINT8* ByteCode, UINT Size)
{
	ShaderByteCode = CD3DX12_SHADER_BYTECODE(ByteCode, Size);
}


void PipelineStateObject::BindUAV(string name, int baseRegister)
{
	BindingData binding;
	binding.name = name;
	binding.baseRegister = baseRegister;
	binding.numDescriptors = 1;
	uavBinding.insert(pair<string, BindingData>(name, binding));
}

void PipelineStateObject::BindTexture(string name, int baseRegister, int num)
{
	BindingData binding;
	binding.name = name;
	binding.baseRegister = baseRegister;
	binding.numDescriptors = num;
	textureBinding.insert(pair<string, BindingData>(name, binding));
}

void PipelineStateObject::BindConstantBuffer(string name, int baseRegister, int size, UINT numMaxDrawCall)
{
	NumMaxDrawCall = numMaxDrawCall;

	BindingData binding;
	binding.name = name;
	binding.baseRegister = baseRegister;
	binding.numDescriptors = 1;
	//binding.cbSize = size;

	int div = size / 256;
	binding.cbSize = (div) * 256;

	if (size % 256 > 0)
		binding.cbSize += 256;


	binding.cb = g_dx12_rhi->CreateConstantBuffer(binding.cbSize, g_dx12_rhi->NumFrame * numMaxDrawCall);

	D3D12_GPU_VIRTUAL_ADDRESS va = binding.cb->resource->GetGPUVirtualAddress();
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

void PipelineStateObject::SetTexture(string name, Texture* texture, ID3D12GraphicsCommandList* CommandList)
{
	textureBinding[name].texture = texture;

	UINT RPI = textureBinding[name].rootParamIndex;
	if (IsCompute)
		CommandList->SetComputeRootDescriptorTable(RPI, texture->GpuHandleSRV);
	else
		CommandList->SetGraphicsRootDescriptorTable(RPI, texture->GpuHandleSRV);
}

void PipelineStateObject::SetUAV(string name, Texture* texture, ID3D12GraphicsCommandList* CommandList)
{
	uavBinding[name].texture = texture;
	D3D12_CPU_DESCRIPTOR_HANDLE ShaderVisibleCPUHandle;
	D3D12_GPU_DESCRIPTOR_HANDLE ShaderVisibleGpuHandle;

	UINT RPI = uavBinding[name].rootParamIndex;
	if (IsCompute)
		CommandList->SetComputeRootDescriptorTable(RPI, texture->GpuHandleUAV);
	else
		CommandList->SetGraphicsRootDescriptorTable(RPI, texture->GpuHandleUAV);
}

void PipelineStateObject::SetSampler(string name, Sampler* sampler, ID3D12GraphicsCommandList* CommandList)
{
	samplerBinding[name].sampler = sampler;

	D3D12_CPU_DESCRIPTOR_HANDLE ShaderVisibleCPUHandle;
	D3D12_GPU_DESCRIPTOR_HANDLE ShaderVisibleGpuHandle;

	if (IsCompute)
		CommandList->SetComputeRootDescriptorTable(samplerBinding[name].rootParamIndex, sampler->GpuHandle);
	else
		CommandList->SetGraphicsRootDescriptorTable(samplerBinding[name].rootParamIndex, sampler->GpuHandle);
}

void PipelineStateObject::SetConstantValue(string name, void* pData, ID3D12GraphicsCommandList* CommandList)
{
	BindingData& binding = constantBufferBinding[name];

	UINT CBViewIndex = g_dx12_rhi->CurrentFrameIndex * NumMaxDrawCall + currentDrawCallIndex;
	UINT8* pMapped = (UINT8*)binding.cb->MemMapped + binding.cbSize*CBViewIndex;
	memcpy((void*)pMapped, pData, binding.cbSize);

	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle;
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle;

	// ring is advanced at the begining of frame. so descriptors from multiple frame is not overlapped.
	g_dx12_rhi->GlobalDHRing->AllocDescriptor(CpuHandle, GpuHandle);

	D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
	cbvDesc.BufferLocation = binding.cb->resource->GetGPUVirtualAddress() + CBViewIndex * binding.cb->Size;
	cbvDesc.SizeInBytes = binding.cb->Size;
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
//void PipelineStateObject::Init(bool isCompute)
//{
//
//	vector<CD3DX12_ROOT_PARAMETER1> rootParamVec;
//
//	vector<CD3DX12_DESCRIPTOR_RANGE1> TextureRanges;
//	if (ps && ps->textureBinding.size() != 0)
//	{
//		TextureRanges.resize(ps->textureBinding.size());
//		int i = 0;
//		for (auto& bindingPair : ps->textureBinding)
//		{
//			Shader::BindingData& bindingData = bindingPair.second;
//
//			CD3DX12_ROOT_PARAMETER1 TextureParam;
//			TextureRanges[i].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, bindingData.numDescriptors, bindingData.baseRegister, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
//			TextureParam.InitAsDescriptorTable(1, &TextureRanges[i], D3D12_SHADER_VISIBILITY_PIXEL);
//			rootParamVec.push_back(TextureParam);
//			i++;
//			bindingData.rootParamIndex = RootParamIndex++;
//		}
//	}
//	
//	vector<CD3DX12_DESCRIPTOR_RANGE1> SamplerRanges;
//	if (ps && ps->samplerBinding.size() != 0)
//	{
//		SamplerRanges.resize(ps->samplerBinding.size());
//		int i = 0;
//		for (auto& bindingPair : ps->samplerBinding)
//		{
//			Shader::BindingData& bindingData = bindingPair.second;
//
//			CD3DX12_ROOT_PARAMETER1 SamplerParam;
//			SamplerRanges[i].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, bindingData.numDescriptors, bindingData.baseRegister);
//			SamplerParam.InitAsDescriptorTable(1, &SamplerRanges[i], D3D12_SHADER_VISIBILITY_PIXEL);
//			rootParamVec.push_back(SamplerParam);
//			i++;
//			bindingData.rootParamIndex = RootParamIndex++;
//		}
//	}
//	
//
//	if (ps && ps->rootBinding.size() != 0)
//	{
//		int i = 0;
//		for (auto& bindingPair : ps->rootBinding)
//		{
//			Shader::BindingData& bindingData = bindingPair.second;
//
//			CD3DX12_ROOT_PARAMETER1 ConstantParam;
//			ConstantParam.InitAsConstants(bindingData.numDescriptors, bindingData.baseRegister, 0, D3D12_SHADER_VISIBILITY_PIXEL);
//			rootParamVec.push_back(ConstantParam);
//			i++;
//			bindingData.rootParamIndex = RootParamIndex++;
//		}
//	}
//
//	vector<CD3DX12_DESCRIPTOR_RANGE1> CBRangesPS;
//	if (ps && ps->constantBufferBinding.size() != 0)
//	{
//
//		CBRangesPS.resize(ps->constantBufferBinding.size());
//		int i = 0;
//		for (auto& bindingPair : ps->constantBufferBinding)
//		{
//			Shader::BindingData& bindingData = bindingPair.second;
//
//			CD3DX12_ROOT_PARAMETER1 CBParam;
//			CBRangesPS[i].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, bindingData.numDescriptors, bindingData.baseRegister, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
//			CBParam.InitAsDescriptorTable(1, &CBRangesPS[i], D3D12_SHADER_VISIBILITY_PIXEL);
//
//			rootParamVec.push_back(CBParam);
//			i++;
//			bindingData.rootParamIndex = RootParamIndex++;
//		}
//	}
//	
//	
//	vector<CD3DX12_DESCRIPTOR_RANGE1> CBRanges;
//	if (vs && vs->constantBufferBinding.size() != 0)
//	{
//		CBRanges.resize(vs->constantBufferBinding.size());
//		int i = 0;
//		for (auto& bindingPair : vs->constantBufferBinding)
//		{
//			Shader::BindingData& bindingData = bindingPair.second;
//
//			CD3DX12_ROOT_PARAMETER1 CBParam;
//			CBRanges[i].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, bindingData.numDescriptors, bindingData.baseRegister, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
//			CBParam.InitAsDescriptorTable(1, &CBRanges[i], D3D12_SHADER_VISIBILITY_VERTEX);
//
//			rootParamVec.push_back(CBParam);
//			i++;
//			bindingData.rootParamIndex = RootParamIndex++;
//		}
//	}
//	
//	vector<CD3DX12_DESCRIPTOR_RANGE1> TextureRangesCS;
//	if (cs && cs->textureBinding.size() != 0)
//	{
//		TextureRangesCS.resize(cs->textureBinding.size());
//		int i = 0;
//		for (auto& bindingPair : cs->textureBinding)
//		{
//			Shader::BindingData& bindingData = bindingPair.second;
//
//			CD3DX12_ROOT_PARAMETER1 TextureParam;
//			TextureRangesCS[i].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, bindingData.numDescriptors, bindingData.baseRegister, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
//			TextureParam.InitAsDescriptorTable(1, &TextureRangesCS[i], D3D12_SHADER_VISIBILITY_ALL);
//			rootParamVec.push_back(TextureParam);
//			i++;
//			bindingData.rootParamIndex = RootParamIndex++;
//		}
//	}
//
//	vector<CD3DX12_DESCRIPTOR_RANGE1> UAVRanges;
//	if (cs && cs->uavBinding.size() != 0)
//	{
//		UAVRanges.resize(cs->uavBinding.size());
//		int i = 0;
//		for (auto& bindingPair : cs->uavBinding)
//		{
//			Shader::BindingData& bindingData = bindingPair.second;
//			CD3DX12_ROOT_PARAMETER1 TextureParam;
//			UAVRanges[i].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, bindingData.numDescriptors, bindingData.baseRegister, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
//			TextureParam.InitAsDescriptorTable(1, &UAVRanges[i], D3D12_SHADER_VISIBILITY_ALL);
//			rootParamVec.push_back(TextureParam);
//			i++;
//			bindingData.rootParamIndex = RootParamIndex++;
//		}
//	}
//
//	CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
//	rootSignatureDesc.Init_1_1(rootParamVec.size(), &rootParamVec[0], 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
//
//	D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = {};
//	featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
//
//	if (FAILED(g_dx12_rhi->Device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData))))
//	{
//		featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
//	}
//
//	ComPtr<ID3DBlob> signature;
//	ComPtr<ID3DBlob> error;
//	
//	try
//	{
//		ThrowIfFailed(D3DX12SerializeVersionedRootSignature(&rootSignatureDesc, featureData.HighestVersion, &signature, &error));
//	}
//	catch (const std::exception& e)
//	{
//		OutputDebugStringA(reinterpret_cast<const char*>(error->GetBufferPointer()));
//	}
//	
//	try
//	{
//		ThrowIfFailed(g_dx12_rhi->Device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&RS)));
//	}
//	catch (const std::exception& e)
//	{
//		OutputDebugStringA(reinterpret_cast<const char*>(error->GetBufferPointer()));
//	}
//
//	NAME_D3D12_OBJECT(RS);
//	if (isCompute)
//	{
//		computePSODesc.CS = cs->ShaderByteCode;
//		computePSODesc.pRootSignature = RS.Get();
//
//		ThrowIfFailed(g_dx12_rhi->Device->CreateComputePipelineState(&computePSODesc, IID_PPV_ARGS(&PSO)));
//		NAME_D3D12_OBJECT(PSO);
//	}
//	else
//	{
//		graphicsPSODesc.VS = vs->ShaderByteCode;
//		graphicsPSODesc.PS = ps->ShaderByteCode;
//
//		graphicsPSODesc.pRootSignature = RS.Get();
//
//		ThrowIfFailed(g_dx12_rhi->Device->CreateGraphicsPipelineState(&graphicsPSODesc, IID_PPV_ARGS(&PSO)));
//		NAME_D3D12_OBJECT(PSO);
//	}
//}

void PipelineStateObject::Init2()
{

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
		computePSODesc.CS = cs->ShaderByteCode;
		computePSODesc.pRootSignature = RS.Get();

		ThrowIfFailed(g_dx12_rhi->Device->CreateComputePipelineState(&computePSODesc, IID_PPV_ARGS(&PSO)));
		NAME_D3D12_OBJECT(PSO);
	}
	else
	{
		graphicsPSODesc.VS = vs->ShaderByteCode;
		graphicsPSODesc.PS = ps->ShaderByteCode;

		graphicsPSODesc.pRootSignature = RS.Get();

		ThrowIfFailed(g_dx12_rhi->Device->CreateGraphicsPipelineState(&graphicsPSODesc, IID_PPV_ARGS(&PSO)));
		NAME_D3D12_OBJECT(PSO);
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

UINT PipelineStateObject::GetGraphicsBindingDHSize()
{
	return 0;//return	vs->constantBufferBinding.size() + ps->constantBufferBinding.size() + vs->textureBinding.size() + ps->textureBinding.size();
}

void Texture::MakeStaticSRV()
{
	g_dx12_rhi->TextureDHRing->AllocDescriptor(CpuHandleSRV, GpuHandleSRV);

	D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc = {};
	SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	SrvDesc.Format = textureDesc.Format;

	if(textureDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
		SrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
	else 
		SrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	SrvDesc.Texture2D.MipLevels = textureDesc.MipLevels;
	g_dx12_rhi->Device->CreateShaderResourceView(resource.Get(), &SrvDesc, CpuHandleSRV);
}

void Texture::MakeRTV()
{
	g_dx12_rhi->RTVDescriptorHeap->AllocDescriptor(CpuHandleRTV, GpuHandleRTV);
	
	D3D12_RENDER_TARGET_VIEW_DESC desc = {};
	//desc.Format = Format;
	desc.Format = textureDesc.Format;

	desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

	g_dx12_rhi->Device->CreateRenderTargetView(resource.Get(), nullptr, CpuHandleRTV);
}

void Texture::MakeDSV()
{
	g_dx12_rhi->DSVDescriptorHeap->AllocDescriptor(CpuHandleDSV, GpuHandleDSV);

	D3D12_DEPTH_STENCIL_VIEW_DESC depthStencilDesc = {};
	depthStencilDesc.Format = DXGI_FORMAT_D32_FLOAT;
	depthStencilDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	depthStencilDesc.Flags = D3D12_DSV_FLAG_NONE;
	g_dx12_rhi->Device->CreateDepthStencilView(resource.Get(), &depthStencilDesc, CpuHandleDSV);
}

std::shared_ptr<Texture> DumRHI_DX12::CreateTexture2DFromResource(ComPtr<ID3D12Resource> InResource)
{
	Texture* tex = new Texture;

	if (InResource)
	{
		tex->resource = InResource;
	}

	return shared_ptr<Texture>(tex);
}

std::shared_ptr<Texture> DumRHI_DX12::CreateTexture2D(DXGI_FORMAT format, D3D12_RESOURCE_FLAGS resFlags, D3D12_RESOURCE_STATES initResState, int width, int height, int mipLevels)
{
	Texture* tex = new Texture;



	D3D12_RESOURCE_DESC textureDesc = {};
	textureDesc.MipLevels = mipLevels;
	textureDesc.Format = format;
	textureDesc.Width = width;
	textureDesc.Height = height;
	textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
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
		optimizedClearValue.Color[0] = 0.0f;
		optimizedClearValue.Color[1] = 0.2f;
		optimizedClearValue.Color[2] = 0.4f;
		optimizedClearValue.Color[3] = 1.0f;

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
		g_dx12_rhi->DynamicTextures.push_back(texPtr);

	return texPtr;
}

std::shared_ptr<Texture> DumRHI_DX12::CreateTexture3D(DXGI_FORMAT format, D3D12_RESOURCE_FLAGS resFlags, D3D12_RESOURCE_STATES initResState, int width, int height, int depth, int mipLevels)
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


	shared_ptr<Texture> texPtr = shared_ptr<Texture>(tex);


	if (resFlags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET || resFlags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL || resFlags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
		g_dx12_rhi->DynamicTextures.push_back(texPtr);
	else
		tex->MakeStaticSRV();

	return texPtr;
}

void Texture::UploadSRCData3D(D3D12_SUBRESOURCE_DATA* SrcData)
{
	// upload src data
	if (SrcData)
	{

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

		stringstream ss;
		ss << "UploadSRCData : " << TotalBytes << "\n";
		OutputDebugStringA(ss.str().c_str());

		ComPtr<ID3D12Resource> textureUploadHeap;

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
		g_dx12_rhi->CommandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);


		D3D12_RESOURCE_BARRIER BarrierDesc = {};
		BarrierDesc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		BarrierDesc.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		BarrierDesc.Transition.pResource = resource.Get();
		BarrierDesc.Transition.Subresource = 0;
		BarrierDesc.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		BarrierDesc.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		g_dx12_rhi->CommandList->ResourceBarrier(1, &BarrierDesc);

		ThrowIfFailed(g_dx12_rhi->CommandList->Close());
		ID3D12CommandList* ppCommandLists[] = { g_dx12_rhi->CommandList.Get() };
		g_dx12_rhi->CommandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
		ThrowIfFailed(g_dx12_rhi->CommandList->Reset(g_dx12_rhi->GetCurrentCA(), nullptr));

		g_dx12_rhi->WaitGPU();
	}
}

std::shared_ptr<Texture> DumRHI_DX12::CreateTextureFromFile(wstring fileName, bool isNormal)
{
	if (FileExists(fileName.c_str()) == false)
		return nullptr;

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

	if(!isNormal)
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

	stringstream ss;
	ss << "CreateTextureFromFile : " << uploadBufferSize << "\n";
	OutputDebugStringA(ss.str().c_str());

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

		g_dx12_rhi->CommandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	}

	D3D12_RESOURCE_BARRIER BarrierDesc = {};
	BarrierDesc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	BarrierDesc.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	BarrierDesc.Transition.pResource = tex->resource.Get();
	BarrierDesc.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	BarrierDesc.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	BarrierDesc.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	g_dx12_rhi->CommandList->ResourceBarrier(1, &BarrierDesc);

	ThrowIfFailed(g_dx12_rhi->CommandList->Close());
	ID3D12CommandList* ppCommandLists[] = { g_dx12_rhi->CommandList.Get() };
	g_dx12_rhi->CommandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
	ThrowIfFailed(g_dx12_rhi->CommandList->Reset(g_dx12_rhi->GetCurrentCA(), nullptr));

	g_dx12_rhi->WaitGPU();

	tex->MakeStaticSRV();

	return shared_ptr<Texture>(tex);
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

shared_ptr<RTAS> Mesh::CreateBLAS()
{
	RTAS* as = new  RTAS;


	D3D12_RAYTRACING_GEOMETRY_DESC geomDesc = {};
	geomDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
	geomDesc.Triangles.VertexBuffer.StartAddress = Vb->resource->GetGPUVirtualAddress();
	geomDesc.Triangles.VertexBuffer.StrideInBytes = VertexStride;;
	geomDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
	geomDesc.Triangles.VertexCount = Vb->numVertices;
	geomDesc.Triangles.IndexBuffer = Ib->resource->GetGPUVirtualAddress();
	geomDesc.Triangles.IndexFormat = IndexFormat;
	geomDesc.Triangles.IndexCount = Ib->numIndices;
	geomDesc.Triangles.Transform3x4 = 0;

	geomDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;


	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
	inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
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

		stringstream ss;
		ss << "blas->scratch : " << bufDesc.Width << "\n";
		OutputDebugStringA(ss.str().c_str());

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

		
		stringstream ss;
		ss << "blas->result : " << bufDesc.Width << "\n";
		OutputDebugStringA(ss.str().c_str());


		g_dx12_rhi->Device->CreateCommittedResource(&kDefaultHeapProps, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr, IID_PPV_ARGS(&as->Result));
	}

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC asDesc = {};
	asDesc.Inputs = inputs;
	asDesc.DestAccelerationStructureData = as->Result->GetGPUVirtualAddress();
	asDesc.ScratchAccelerationStructureData = as->Scratch->GetGPUVirtualAddress();

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC postInfo;
	g_dx12_rhi->CommandList->BuildRaytracingAccelerationStructure(&asDesc, 0, &postInfo);

	D3D12_RESOURCE_BARRIER uavBarrier = {};
	uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	uavBarrier.UAV.pResource = as->Result.Get();
	g_dx12_rhi->CommandList->ResourceBarrier(1, &uavBarrier);

	g_dx12_rhi->WaitGPU();


	return shared_ptr<RTAS>(as);
}

std::shared_ptr<RTAS> DumRHI_DX12::CreateTLAS(vector<shared_ptr<RTAS>>& VecBottomLevelAS)
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


		stringstream ss;
		ss << "tlas->result : " << bufDesc.Width << "\n";
		OutputDebugStringA(ss.str().c_str());


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


		stringstream ss;
		ss << "tlas->result : " << bufDesc.Width << "\n";
		OutputDebugStringA(ss.str().c_str());


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


		stringstream ss;
		ss << "as->instance : " << bufDesc.Width << "\n";
		OutputDebugStringA(ss.str().c_str());


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
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC asDesc = {};
	asDesc.Inputs = inputs;

	if (VecBottomLevelAS.size() > 0)
		asDesc.Inputs.InstanceDescs = as->Instance->GetGPUVirtualAddress();
	asDesc.DestAccelerationStructureData = as->Result->GetGPUVirtualAddress();
	asDesc.ScratchAccelerationStructureData = as->Scratch->GetGPUVirtualAddress();

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC postInfo;

	CommandList->BuildRaytracingAccelerationStructure(&asDesc, 0, &postInfo);

	// We need to insert a UAV barrier before using the acceleration structures in a raytracing operation
	D3D12_RESOURCE_BARRIER uavBarrier = {};
	uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	uavBarrier.UAV.pResource = as->Result.Get();
	CommandList->ResourceBarrier(1, &uavBarrier);

	// create acceleration structure srv (not shader-visible yet)
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.RaytracingAccelerationStructure.Location = as->Result->GetGPUVirtualAddress();

	// copydescriptor needed when being used.
	//dx12_rhi->SRVCBVDescriptorHeapStorage->AllocDescriptor(RTASCPUHandle, RTASGPUHandle);
	//g_dx12_rhi->SRVCBVDescriptorHeapShaderVisible->AllocDescriptor(as->CPUHandle, as->GPUHandle);
	GeomtryDHRing->AllocDescriptor(as->CPUHandle, as->GPUHandle);

	g_dx12_rhi->Device->CreateShaderResourceView(nullptr, &srvDesc, as->CPUHandle);

	g_dx12_rhi->WaitGPU();

	return shared_ptr<RTAS>(as);
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

ComPtr<ID3DBlob> compileShaderLibrary(const WCHAR* filename, const WCHAR* targetString)
{
	// Initialize the helper
	gDxcDllHelper.Initialize();
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
	pCompiler->Compile(pTextBlob.Get(), filename, L"", targetString, nullptr, 0, nullptr, 0, dxcIncludeHandler.Get(), &pResult);

	// Verify the result
	HRESULT resultCode;
	pResult->GetStatus(&resultCode);
	if (FAILED(resultCode))
	{
		ComPtr<IDxcBlobEncoding> pError;
		pResult->GetErrorBuffer(&pError);
		std::string log = convertBlobToString(pError.Get());
		//msgBox("Compiler error:\n" + log);
		OutputDebugStringA(log.c_str());

		return nullptr;
	}

	ID3DBlob* pBlob;
	pResult->GetResult((IDxcBlob**)&pBlob);
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
	MapHitGroup[chs] = info;
}

void RTPipelineStateObject::AddShader(string shader, RTPipelineStateObject::ShaderType shaderType)
{
	ShaderBinding[shader].ShaderName = StringToWString(shader);;
	ShaderBinding[shader].Type = shaderType;
}

void RTPipelineStateObject::BindUAV(string shader, string name, UINT baseRegister)
{
	auto& bindingInfo = ShaderBinding[shader];

	BindingData binding;
	binding.name = name;
	binding.Type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;

	binding.BaseRegister = baseRegister;

	bindingInfo.Binding.push_back(binding);
}

void RTPipelineStateObject::BindSRV(string shader, string name, UINT baseRegister)
{
	auto& bindingInfo = ShaderBinding[shader];

	BindingData binding;
	binding.name = name;
	binding.Type = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;

	binding.BaseRegister = baseRegister;

	bindingInfo.Binding.push_back(binding);
}

void RTPipelineStateObject::BindSampler(string shader, string name, UINT baseRegister)
{
	auto& bindingInfo = ShaderBinding[shader];

	BindingData binding;
	binding.name = name;
	binding.Type = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;

	binding.BaseRegister = baseRegister;

	bindingInfo.Binding.push_back(binding);
}

void RTPipelineStateObject::BindCBV(string shader, string name, UINT baseRegister, UINT size, UINT numInstance)
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


	for (int iFrame = 0; iFrame < g_dx12_rhi->NumFrame; iFrame++)
	{
		binding.cbs.push_back(g_dx12_rhi->CreateConstantBuffer(binding.cbSize, numInstance));
	}


	bindingInfo.Binding.push_back(binding);
}

void RTPipelineStateObject::BeginShaderTable()
{
}

void RTPipelineStateObject::EndShaderTable()
{
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
				if (bd.Type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER)
				{
					*(UINT64*)(pDataThis) = bd.GPUHandle.ptr;

				}
				else
				{
					*(UINT64*)(pDataThis) = bd.GPUHandle.ptr;
					UINT64 v = *(UINT64*)(pDataThis);

				}

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
	//for (auto& sb : ShaderBinding)
	for(int InstanceIndex=0;InstanceIndex<NumInstance;InstanceIndex++)
	{
		pDataThis = pData + LastIndex * ShaderTableEntrySize;

		auto& HitProgramInfo = HitProgramBinding[InstanceIndex];
		
		memcpy(pDataThis, RtsoProps->GetShaderIdentifier(HitProgramInfo.HitGroupName.c_str()), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
		pDataThis += D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;

		for (auto& bd : HitProgramInfo.VecData)
		{
			*(UINT64*)(pDataThis) = bd.ptr;
			
			pDataThis += sizeof(UINT64);
		}

		LastIndex++;
	}


	ShaderTable->Unmap(0, nullptr);
}

void RTPipelineStateObject::SetUAV(string shader, string bindingName, D3D12_GPU_DESCRIPTOR_HANDLE uavHandle, INT instanceIndex /*= -1*/)
{
	// each bindings of raygen/miss shader is unique to shader name.
	if (instanceIndex == -1) // raygen, miss
	{
		BindingInfo& bi = ShaderBinding[shader];
		for (auto&bd : bi.Binding)
		{
			if (bd.name == bindingName)
			{
				bd.GPUHandle = uavHandle;

			}
		}
	}
	else // hitprogram : There can be multiple hitprogram entry with same shader name, so we need another data structure. (HitProgramBinding)
	{
		// SetXXX should be called according to the binding order, because it is push_backed to vector.
		HitProgramBinding[instanceIndex].HitGroupName = MapHitGroup[shader].name;
		HitProgramBinding[instanceIndex].VecData.push_back(uavHandle);
	}
}

void RTPipelineStateObject::SetSRV(string shader, string bindingName, D3D12_GPU_DESCRIPTOR_HANDLE srvHandle, INT instanceIndex /*= -1*/)
{
	// each bindings of raygen/miss shader is unique to shader name.
	if (instanceIndex == -1) // raygen, miss
	{
		BindingInfo& bi = ShaderBinding[shader];
		for (auto&bd : bi.Binding)
		{
			if (bd.name == bindingName)
			{
				bd.GPUHandle = srvHandle;
			}
		}
	}
	else // hitprogram : There can be multiple hitprogram entry with same shader name, so we need another data structure. (HitProgramBinding)
	{
		// SetXXX should be called according to the binding order, because it is push_backed to vector.
		HitProgramBinding[instanceIndex].HitGroupName = MapHitGroup[shader].name;
		HitProgramBinding[instanceIndex].VecData.push_back(srvHandle);
	}
}

void RTPipelineStateObject::AddHitProgramDescriptor(string shader, D3D12_GPU_DESCRIPTOR_HANDLE srvHandle, UINT instanceIndex)
{
	HitProgramBinding[instanceIndex].VecData.push_back(srvHandle);
}

void RTPipelineStateObject::ResetHitProgramBinding(string shader, UINT instanceIndex, UINT size)
{
	HitProgramBinding[instanceIndex].HitGroupName = MapHitGroup[shader].name;
	HitProgramBinding[instanceIndex].VecData.clear();
	HitProgramBinding[instanceIndex].VecData.reserve(size);
}

void RTPipelineStateObject::SetSampler(string shader, string bindingName, Sampler* sampler, INT instanceIndex /*= -1*/)
{
	// each bindings of raygen/miss shader is unique to shader name.
	if (instanceIndex == -1) // raygen, miss
	{
		BindingInfo& bi = ShaderBinding[shader];
		for (auto&bd : bi.Binding)
		{
			if (bd.name == bindingName)
			{
				bd.GPUHandle = sampler->GpuHandle;
			}
		}
	}
	else // hitprogram : There can be multiple hitprogram entry with same shader name, so we need another data structure. (HitProgramBinding)
	{
		// SetXXX should be called according to the binding order, because it is push_backed to vector.
		HitProgramBinding[instanceIndex].HitGroupName = MapHitGroup[shader].name;
		HitProgramBinding[instanceIndex].VecData.push_back(sampler->GpuHandle);
	}
}

void RTPipelineStateObject::SetCBVValue(string shader, string bindingName, void* pData, INT size, INT instanceIndex /*= -1*/)
{
	// each bindings of raygen/miss shader is unique to shader name.
	if (instanceIndex == -1) // raygen, miss
	{
		BindingInfo& bi = ShaderBinding[shader];
		for (auto&bd : bi.Binding)
		{
			if (bd.name == bindingName)
			{
				auto& cb = bd.cbs[g_dx12_rhi->CurrentFrameIndex];
				UINT8* pMapped = (UINT8*)cb->MemMapped + size * 0;
				memcpy((void*)pMapped, pData, size);

				D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle;
				D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle;

				// ring is advanced at the begining of frame. so descriptors from multiple frame is not overlapped.
				g_dx12_rhi->GlobalDHRing->AllocDescriptor(CpuHandle, GpuHandle);

				D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
				cbvDesc.BufferLocation = cb->resource->GetGPUVirtualAddress();
				cbvDesc.SizeInBytes = cb->Size;
				g_dx12_rhi->Device->CreateConstantBufferView(&cbvDesc, CpuHandle);

				bd.GPUHandle = GpuHandle;
			}
		}
	}
	else // hitprogram : There can be multiple hitprogram entry with same shader name, so we need another data structure. (HitProgramBinding)
	{
		// SetXXX should be called by the binding order, because it is "push_back"ed to vector.
		BindingInfo& bi = ShaderBinding[shader];
		for (auto&bd : bi.Binding)
		{
			if (bd.name == bindingName)
			{
				auto& cb = bd.cbs[g_dx12_rhi->CurrentFrameIndex];
				UINT8* pMapped = (UINT8*)cb->MemMapped + size * instanceIndex;
				memcpy((void*)pMapped, pData, size);

				HitProgramBinding[instanceIndex].HitGroupName = MapHitGroup[shader].name;

				D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle;
				D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle;

				// ring is advanced at the begining of frame. so descriptors from multiple frame is not overlapped.
				g_dx12_rhi->GlobalDHRing->AllocDescriptor(CpuHandle, GpuHandle);

				D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
				cbvDesc.BufferLocation = cb->resource->GetGPUVirtualAddress();
				cbvDesc.SizeInBytes = cb->Size;
				g_dx12_rhi->Device->CreateConstantBufferView(&cbvDesc, CpuHandle);

				HitProgramBinding[instanceIndex].VecData.push_back(GpuHandle);
			}
		}
	}
}

void RTPipelineStateObject::SetHitProgram(UINT instanceIndex, string shader)
{
	HitProgramBinding[instanceIndex].HitGroupName = MapHitGroup[shader].name;
}

void RTPipelineStateObject::InitRS(string ShaderFile)
{
	vector<D3D12_STATE_SUBOBJECT> subobjects;

	// dxil + Hitgroup count + RS + Export + shaderconfig + export + pipelineconfig + global RS
	int numSubobjects = 1 + VecHitGroup.size() + ShaderBinding.size() * 2 + 2 + 1 + 1;
	subobjects.resize(numSubobjects);

	uint32_t index = 0;

	// dxil lib
	wstring wShaderFile = StringToWString(ShaderFile);
	ComPtr<ID3DBlob> pDxilLib = compileShaderLibrary(wShaderFile.c_str(), L"lib_6_3");

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
		//ExportAssociation Association(bindingInfo.ExportName.data(), 1, &subobjects[RSIndex]);

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
	subobjects[index++] = shaderConfig.subobject;

	// shaderconfig export association
	// CHECK : all shader has same payload size?
	vector<const WCHAR*> vecShaderExports;// = { kMissShader.c_str(), kClosestHitShader.c_str(), kRayGenShader.c_str() };
	entryPoints.reserve(ShaderBinding.size());
	for (auto& sb : ShaderBinding)
	{
		vecShaderExports.push_back(sb.second.ShaderName.c_str());
	}

	ExportAssociation configAssociation(vecShaderExports.data(), vecShaderExports.size(), &shaderConfig.subobject);
	subobjects[index++] = configAssociation.subobject;


	// pipeline config
	PipelineConfig config(MaxRecursion);
	subobjects[index++] = config.subobject;

	// global root signature
	D3D12_STATE_SUBOBJECT subobjectGlobalRS = {};
	ID3D12RootSignature* pInterfaceGlobalRS = nullptr;
	D3D12_ROOT_SIGNATURE_DESC GlobalRSDesc = {};

	GlobalRS = CreateRootSignature(g_dx12_rhi->Device, GlobalRSDesc);
	NAME_D3D12_OBJECT(GlobalRS);

	pInterfaceGlobalRS = GlobalRS.Get();
	subobjectGlobalRS.pDesc = &pInterfaceGlobalRS;
	subobjectGlobalRS.Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;

	//subobjects.push_back(subobjectGlobalRS);
	subobjects[index++] = subobjectGlobalRS;

	// Create the RTPSO
	D3D12_STATE_OBJECT_DESC descRTSO;
	descRTSO.NumSubobjects = subobjects.size(); // 10
	descRTSO.pSubobjects = subobjects.data();
	descRTSO.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;

	HRESULT hr = g_dx12_rhi->Device->CreateStateObject(&descRTSO, IID_PPV_ARGS(&RTPipelineState));
	NAME_D3D12_OBJECT(RTPipelineState);

	if (FAILED(hr))
	{

	}


	// create shader table

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
		if (bi.Type == RAYGEN || bi.Type == MISS)
			NumShaderTableEntry++;

	}
	NumShaderTableEntry += NumInstance;

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


		stringstream ss;
		ss << "shadertable : " << bufDesc.Width << "\n";
		OutputDebugStringA(ss.str().c_str());


		g_dx12_rhi->Device->CreateCommittedResource(&kUploadHeapProps, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ShaderTable));
		NAME_D3D12_OBJECT(ShaderTable);

	}
}

void RTPipelineStateObject::Apply(UINT width, UINT height)
{
	D3D12_DISPATCH_RAYS_DESC raytraceDesc = {};
	raytraceDesc.Width = width;
	raytraceDesc.Height = height;
	raytraceDesc.Depth = 1;

	D3D12_GPU_VIRTUAL_ADDRESS StartAddress = ShaderTable->GetGPUVirtualAddress() + ShaderTableSize * g_dx12_rhi->CurrentFrameIndex;

	raytraceDesc.RayGenerationShaderRecord.StartAddress = StartAddress ;
	raytraceDesc.RayGenerationShaderRecord.SizeInBytes = ShaderTableEntrySize;

	// Miss is the second entry in the shader-table
	size_t missOffset = 1 * ShaderTableEntrySize;
	raytraceDesc.MissShaderTable.StartAddress = StartAddress + missOffset;
	raytraceDesc.MissShaderTable.StrideInBytes = ShaderTableEntrySize;
	raytraceDesc.MissShaderTable.SizeInBytes = ShaderTableEntrySize;   // Only a s single miss-entry


	 // Hit is the third entry in the shader-table
	size_t hitOffset = 2 * ShaderTableEntrySize;
	raytraceDesc.HitGroupTable.StartAddress = StartAddress + hitOffset;
	raytraceDesc.HitGroupTable.StrideInBytes = ShaderTableEntrySize;
	raytraceDesc.HitGroupTable.SizeInBytes = ShaderTableEntrySize;

	// Bind the empty root signature

	g_dx12_rhi->CommandList->SetComputeRootSignature(GlobalRS.Get());
	g_dx12_rhi->CommandList->SetPipelineState1(RTPipelineState.Get());

	
	g_dx12_rhi->CommandList->DispatchRays(&raytraceDesc);
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
