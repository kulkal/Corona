#include "DumRHI_DX12.h"
#include "d3dx12.h"
#define GLM_FORCE_CTOR_INIT

#include "glm/glm.hpp"
#define GLM_ENABLE_EXPERIMENTAL
#include "glm/gtx/transform.hpp"
#include "glm/mat4x4.hpp"
#include "glm/fwd.hpp"
#include "DXCAPI/dxcapi.use.h"

#include <sstream>
#include <fstream>

#include <comdef.h>



#define align_to(_alignment, _val) (((_val + _alignment - 1) / _alignment) * _alignment)

DumRHI_DX12* g_dx12_rhi;

ThreadDescriptorHeapPool::ThreadDescriptorHeapPool()
{
	DHeap = g_dx12_rhi->SRVCBVDescriptorHeapShaderVisible.get();
}

void ThreadDescriptorHeapPool::AllocDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE& cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE& gpuHandle)
{
	if ((PoolIndex-StartIndex) >= PoolSize)
		PoolIndex = 0;

	cpuHandle.ptr = DHeap->CPUHeapStart /*DHeap->DH->GetCPUDescriptorHandleForHeapStart().ptr */+ PoolIndex * DHeap->DescriptorSize;// g_dx12_rhi->Device->GetDescriptorHandleIncrementSize(DHeap->HeapDesc.Type);

	gpuHandle.ptr = DHeap->GPUHeapStart/* DHeap->DH->GetGPUDescriptorHandleForHeapStart().ptr */+ PoolIndex * DHeap->DescriptorSize;// g_dx12_rhi->Device->GetDescriptorHandleIncrementSize(DHeap->HeapDesc.Type);

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
	if (DescriptorIndex >= MaxNumDescriptors)
		DescriptorIndex = 0;

	cpuHandle.ptr = CPUHeapStart /*DH->GetCPUDescriptorHandleForHeapStart().ptr */+ DescriptorIndex * DescriptorSize;// g_dx12_rhi->Device->GetDescriptorHandleIncrementSize(HeapDesc.Type);

	gpuHandle.ptr = GPUHeapStart /*DH->GetGPUDescriptorHandleForHeapStart().ptr */+ DescriptorIndex * DescriptorSize;// g_dx12_rhi->Device->GetDescriptorHandleIncrementSize(HeapDesc.Type);

	DescriptorIndex++;
}



void DumRHI_DX12::BeginFrame()
{
	const UINT64 lastCompletedFence = m_fence->GetCompletedValue();

	CurrentFrameIndex = m_swapChain->GetCurrentBackBufferIndex();


	// wait until gpu processing for this frame resource is completed
	if (FrameFenceValue > 2)
	{
		UINT64 ThisFrameFenceValue = FrameResourceVec[CurrentFrameIndex].FenceValue;
		ThrowIfFailed(m_fence->SetEventOnCompletion(ThisFrameFenceValue, m_fenceEvent));
		WaitForSingleObject(m_fenceEvent, INFINITE);
	}


	GetCurrentCA()->Reset();
	ThrowIfFailed(CommandList->Reset(GetCurrentCA(), nullptr));

	/*	ID3D12CommandAllocator* CA = dx12_rhi->FrameResourceVec[dx12_rhi->CurrentFrameIndex].VecCommandAllocatorMeshDraw[i].Get();
			CL->Reset(CA, nullptr);*/

	for (auto& CA : FrameResourceVec[CurrentFrameIndex].VecCommandAllocatorMeshDraw)
	{
		CA->Reset();
	}
	
	FrameTextureSet.clear();

	for (auto &cb : cbVec)
	{
		cb->isPopulatedThisFrame = false;
	}

	ID3D12DescriptorHeap* ppHeaps[] = { SRVCBVDescriptorHeapShaderVisible->DH.Get(), SamplerDescriptorHeapShaderVisible->DH.Get() };
	CommandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
	
}

void DumRHI_DX12::EndFrame()
{
	// Present and update the frame index for the next frame.
	ThrowIfFailed(m_swapChain->Present(0, 0));

	// Signal and increment the fence value.
	FrameResourceVec[CurrentFrameIndex].FenceValue = FrameFenceValue;
	ThrowIfFailed(CommandQueue->Signal(m_fence.Get(), FrameFenceValue));
	FrameFenceValue++;
}

void DumRHI_DX12::WaitGPU()
{
	// Create synchronization objects and wait until assets have been uploaded to the GPU.

	HANDLE m_fenceEvent;
	ComPtr<ID3D12Fence> m_fence;
	UINT64 mfenceValue = 0;

	ThrowIfFailed(g_dx12_rhi->Device->CreateFence(m_fenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
	m_fenceValue++;

	// Create an event handle to use for frame synchronization.
	m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	if (m_fenceEvent == nullptr)
	{
		ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
	}

	// Wait for the command list to execute; we are reusing the same command 
	// list in our main loop but for now, we just want to wait for setup to 
	// complete before continuing.

	// Signal and increment the fence value.
	const UINT64 fenceToWaitFor = m_fenceValue;
	ThrowIfFailed(CommandQueue->Signal(m_fence.Get(), fenceToWaitFor));
	m_fenceValue++;

	// Wait until the fence is completed.
	ThrowIfFailed(m_fence->SetEventOnCompletion(fenceToWaitFor, m_fenceEvent));
	WaitForSingleObject(m_fenceEvent, INFINITE);
}

void DumRHI_DX12::UploadeFrameTexture2ShaderVisibleHeap()
{
	std::set<Texture*>::iterator it;
	for (it = FrameTextureSet.begin(); it != FrameTextureSet.end(); ++it)
	{
		Texture* texture = *it;

		D3D12_CPU_DESCRIPTOR_HANDLE cpuHandleShaderVisible;
		g_dx12_rhi->SRVCBVDescriptorHeapShaderVisible->AllocDescriptor(cpuHandleShaderVisible, texture->GpuHandleSRV);

		g_dx12_rhi->Device->CopyDescriptorsSimple(1, cpuHandleShaderVisible, texture->CpuHandleSRV, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	}
}

std::shared_ptr<Texture> DumRHI_DX12::CreateTexture2DFromResource(ComPtr<ID3D12Resource> InResource)
{
	Texture* tex =  new Texture;

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
	

	return shared_ptr<Texture>(tex);

}

shared_ptr<ConstantBuffer> DumRHI_DX12::CreateConstantBuffer(int Size, UINT NumView)

{
	ConstantBuffer* cbuffer = new ConstantBuffer;

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

	// Describe and create a constant buffer view (CBV).
	for (int i = 0; i < NumView; i++)
	{
		D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle;
		D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle;

		SRVCBVDescriptorHeapStorage->AllocDescriptor(CpuHandle, GpuHandle);


		D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
		cbvDesc.BufferLocation = cbuffer->resource->GetGPUVirtualAddress() + i*Size;
		cbvDesc.SizeInBytes = Size;
		Device->CreateConstantBufferView(&cbvDesc, CpuHandle);
		
		cbuffer->CpuHandleVec.push_back(CpuHandle);
	}
	
	shared_ptr<ConstantBuffer> retPtr = shared_ptr<ConstantBuffer>(cbuffer);
	cbVec.push_back(retPtr);

	return retPtr;
}


shared_ptr<Sampler> DumRHI_DX12::CreateSampler(D3D12_SAMPLER_DESC& InSamplerDesc)
{
	Sampler* sampler = new Sampler;

	sampler->SamplerDesc = InSamplerDesc;

	SamplerDescriptorHeapStorage->AllocDescriptor(sampler->CpuHandle, sampler->GpuHandle);


	Device->CreateSampler(&sampler->SamplerDesc, sampler->CpuHandle);


	return shared_ptr<Sampler>(sampler);
}

shared_ptr<IndexBuffer> DumRHI_DX12::CreateIndexBuffer(DXGI_FORMAT Format, UINT Size, void* SrcData)
{
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

		UpdateSubresources<1>(CommandList.Get(), ib->resource.Get(), UploadHeap.Get(), 0, 0, 1, &indexData);
		CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(ib->resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER));


		ThrowIfFailed(CommandList->Close());
		ID3D12CommandList* ppCommandLists[] = { CommandList.Get() };
		CommandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
		//ThrowIfFailed(CommandList->Reset(CommandAllocator.Get(), nullptr));
		ThrowIfFailed(CommandList->Reset(GetCurrentCA(), nullptr));

		// Create synchronization objects and wait until assets have been uploaded to the GPU.
		{

			HANDLE m_fenceEvent;
			ComPtr<ID3D12Fence> m_fence;
			UINT64 m_fenceValue = 0;

			ThrowIfFailed(Device->CreateFence(m_fenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
			m_fenceValue++;

			// Create an event handle to use for frame synchronization.
			m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
			if (m_fenceEvent == nullptr)
			{
				ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
			}

			// Wait for the command list to execute; we are reusing the same command 
			// list in our main loop but for now, we just want to wait for setup to 
			// complete before continuing.

			// Signal and increment the fence value.
			const UINT64 fenceToWaitFor = m_fenceValue;
			ThrowIfFailed(CommandQueue->Signal(m_fence.Get(), fenceToWaitFor));
			m_fenceValue++;

			// Wait until the fence is completed.
			ThrowIfFailed(m_fence->SetEventOnCompletion(fenceToWaitFor, m_fenceEvent));
			WaitForSingleObject(m_fenceEvent, INFINITE);
		}

	}

	return shared_ptr<IndexBuffer>(ib);
}

shared_ptr<VertexBuffer> DumRHI_DX12::CreateVertexBuffer(UINT Size, UINT Stride, void* SrcData)
{
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

		UpdateSubresources<1>(CommandList.Get(), vb->resource.Get(), UploadHeap.Get(), 0, 0, 1, &vertexData);
		CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(vb->resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER));

		// Initialize the vertex buffer view.
		vb->view.BufferLocation = vb->resource->GetGPUVirtualAddress();
		vb->view.StrideInBytes = Stride;
		vb->view.SizeInBytes = Size;
		vb->numVertices = Size / Stride;

		ThrowIfFailed(CommandList->Close());
		ID3D12CommandList* ppCommandLists[] = { CommandList.Get() };
		CommandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
		//ThrowIfFailed(CommandList->Reset(CommandAllocator.Get(), nullptr));
		ThrowIfFailed(CommandList->Reset(GetCurrentCA(), nullptr));

		// Create synchronization objects and wait until assets have been uploaded to the GPU.
		{

			HANDLE m_fenceEvent;
			ComPtr<ID3D12Fence> m_fence;
			UINT64 m_fenceValue = 0;

			ThrowIfFailed(Device->CreateFence(m_fenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
			m_fenceValue++;

			// Create an event handle to use for frame synchronization.
			m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
			if (m_fenceEvent == nullptr)
			{
				ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
			}

			// Wait for the command list to execute; we are reusing the same command 
			// list in our main loop but for now, we just want to wait for setup to 
			// complete before continuing.

			// Signal and increment the fence value.
			const UINT64 fenceToWaitFor = m_fenceValue;
			ThrowIfFailed(CommandQueue->Signal(m_fence.Get(), fenceToWaitFor));
			m_fenceValue++;

			// Wait until the fence is completed.
			ThrowIfFailed(m_fence->SetEventOnCompletion(fenceToWaitFor, m_fenceEvent));
			WaitForSingleObject(m_fenceEvent, INFINITE);
		}

	}


	return shared_ptr<VertexBuffer>(vb);
}

void DumRHI_DX12::SetRendertarget(Texture* rt)
{
	/*D3D12_RESOURCE_BARRIER BarrierDesc = {};
	BarrierDesc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	BarrierDesc.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	BarrierDesc.Transition.pResource = rt->resource.Get();
	BarrierDesc.Transition.Subresource = 0;
	BarrierDesc.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
	BarrierDesc.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	CommandList->ResourceBarrier(1, &BarrierDesc);*/

	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rt->CpuHandleRTV;

	CommandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &depthTexture->CpuHandleDSV);

	const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
	CommandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
	CommandList->ClearDepthStencilView(depthTexture->CpuHandleDSV, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
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

void DumRHI_DX12::CreateRendertargets(IDXGISwapChain3* InSwapChain, int width, int height)
{
	// use swapchain not texture.
	{
		for (UINT i = 0; i < NumFrame; i++)
		{
			ComPtr<ID3D12Resource> rendertarget;
			ThrowIfFailed(InSwapChain->GetBuffer(i, IID_PPV_ARGS(&rendertarget)));

			renderTargetTextures.push_back(CreateTexture2DFromResource(rendertarget));
		}
	}
	
	depthTexture = CreateTexture2DFromResource(nullptr);
}

void DumRHI_DX12::SubmitCommandList(ID3D12GraphicsCommandList* CmdList)
{
	CmdList->Close();
	CommandQueue->ExecuteCommandLists(1, (ID3D12CommandList**)&CmdList);
	m_fenceValue++;
	CommandQueue->Signal(m_fence.Get(), m_fenceValue);
}

DumRHI_DX12::DumRHI_DX12(ID3D12Device5 * InDevice)
	:Device(InDevice)
{

	ID3D12Device* Device = InDevice;
	GFSDK_Aftermath_Result result = GFSDK_Aftermath_DX12_Initialize(GFSDK_Aftermath_Version::GFSDK_Aftermath_Version_API, GFSDK_Aftermath_FeatureFlags::GFSDK_Aftermath_FeatureFlags_Maximum, Device);

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

	GFSDK_Aftermath_Result ar = GFSDK_Aftermath_DX12_CreateContextHandle(CommandList.Get(), &AM_CL_Handle);

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
	
	{
		SamplerDescriptorHeapStorage = std::make_unique<DescriptorHeap>();

		D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
		HeapDesc.NumDescriptors = 1024*8;
		HeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
		HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		SamplerDescriptorHeapStorage->Init(HeapDesc);

		NAME_D3D12_OBJECT(SamplerDescriptorHeapStorage->DH);

	}
	
	
	


	

	// Close the command list and execute it to begin the initial GPU setup.
	ThrowIfFailed(CommandList->Close());
	ID3D12CommandList* ppCommandLists[] = { CommandList.Get() };
	CommandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
	//ThrowIfFailed(CommandList->Reset(CommandAllocator.Get(), nullptr));
	ThrowIfFailed(CommandList->Reset(GetCurrentCA(), nullptr));

	// Create synchronization objects and wait until assets have been uploaded to the GPU.
	{
		ThrowIfFailed(Device->CreateFence(m_fenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
		m_fenceValue++;

		// Create an event handle to use for frame synchronization.
		m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (m_fenceEvent == nullptr)
		{
			ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
		}

		// Wait for the command list to execute; we are reusing the same command 
		// list in our main loop but for now, we just want to wait for setup to 
		// complete before continuing.

		// Signal and increment the fence value.
		const UINT64 fenceToWaitFor = m_fenceValue;
		ThrowIfFailed(CommandQueue->Signal(m_fence.Get(), fenceToWaitFor));
		m_fenceValue++;

		// Wait until the fence is completed.
		ThrowIfFailed(m_fence->SetEventOnCompletion(fenceToWaitFor, m_fenceEvent));
		WaitForSingleObject(m_fenceEvent, INFINITE);
	}




}


DumRHI_DX12::~DumRHI_DX12()
{
	{
		const UINT64 fence = m_fenceValue;
		const UINT64 lastCompletedFence = m_fence->GetCompletedValue();

		// Signal and increment the fence value.
		ThrowIfFailed(CommandQueue->Signal(m_fence.Get(), m_fenceValue));
		m_fenceValue++;

		// Wait until the previous frame is finished.
		if (lastCompletedFence < fence)
		{
			ThrowIfFailed(m_fence->SetEventOnCompletion(fence, m_fenceEvent));
			WaitForSingleObject(m_fenceEvent, INFINITE);
		}
	}
}

void Shader::BindUAV(string name, int baseRegister)
{
	BindingData binding;
	binding.name = name;
	binding.baseRegister = baseRegister;
	binding.numDescriptors = 1;

	uavBinding.insert(pair<string, BindingData>(name, binding));
}

void Shader::BindTexture(string name, int baseRegister, int num)
{
	BindingData binding;
	binding.name = name;
	binding.baseRegister = baseRegister;
	binding.numDescriptors = num;

	textureBinding.insert(pair<string, BindingData>(name, binding));
}

void Shader::BindConstantBuffer(string name, int baseRegister, int size, UINT numMaxDrawCall)
{
	NumMaxDrawCall = numMaxDrawCall;

	BindingData binding;
	binding.name = name;
	binding.baseRegister = baseRegister;
	binding.numDescriptors = 1;
	//binding.cbSize = size;

	int div = size / 256;
	binding.cbSize = (div + 1) * 256;

	binding.cb = g_dx12_rhi->CreateConstantBuffer(binding.cbSize, g_dx12_rhi->NumFrame * numMaxDrawCall);

	constantBufferBinding.insert(pair<string, BindingData>(name, binding));
}

void Shader::BindGlobalConstantBuffer(string name, int baseRegister)
{
	BindingData binding;
	binding.name = name;
	binding.baseRegister = baseRegister;
	binding.numDescriptors = 1;

	constantBufferBinding.insert(pair<string, BindingData>(name, binding));
}

void Shader::BindRootConstant(string name, int baseRegister)
{
	BindingData binding;
	binding.name = name;
	binding.baseRegister = baseRegister;
	binding.numDescriptors = 1;

	rootBinding.insert(pair<string, BindingData>(name, binding));
}

void Shader::BindSampler(string name, int baseRegister)
{
	BindingData binding;
	binding.name = name;
	binding.baseRegister = baseRegister;
	binding.numDescriptors = 1;

	samplerBinding.insert(pair<string, BindingData>(name, binding));
}

void Shader::SetTexture(string name, Texture* texture, ID3D12GraphicsCommandList* CommandList, ThreadDescriptorHeapPool* DHPool)
{
	textureBinding[name].texture = texture;

	D3D12_CPU_DESCRIPTOR_HANDLE ShaderVisibleCPUHandle;
	D3D12_GPU_DESCRIPTOR_HANDLE ShaderVisibleGpuHandle;

	if (DHPool)
		DHPool->AllocDescriptor(ShaderVisibleCPUHandle, ShaderVisibleGpuHandle);
	else
		g_dx12_rhi->SRVCBVDescriptorHeapShaderVisible->AllocDescriptor(ShaderVisibleCPUHandle, ShaderVisibleGpuHandle);

	g_dx12_rhi->Device->CopyDescriptorsSimple(1, ShaderVisibleCPUHandle, texture->CpuHandleSRV, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	UINT RPI = textureBinding[name].rootParamIndex;
	CommandList->SetGraphicsRootDescriptorTable(RPI, ShaderVisibleGpuHandle);

}

void Shader::SetUAV(string name, Texture* texture)
{
	uavBinding[name].texture = texture;
}

void Shader::SetSampler(string name, Sampler* sampler, ID3D12GraphicsCommandList* CommandList, ThreadDescriptorHeapPool* DHPool)
{
	samplerBinding[name].sampler = sampler;

	D3D12_CPU_DESCRIPTOR_HANDLE ShaderVisibleCPUHandle;
	D3D12_GPU_DESCRIPTOR_HANDLE ShaderVisibleGpuHandle;

	// FIXME :: sampler also should be dh pool.
	/*if (DHPool)
		DHPool->AllocDescriptor(ShaderVisibleCPUHandle, ShaderVisibleGpuHandle);
	else*/
		g_dx12_rhi->SamplerDescriptorHeapShaderVisible->AllocDescriptor(ShaderVisibleCPUHandle, ShaderVisibleGpuHandle);

	g_dx12_rhi->Device->CopyDescriptorsSimple(1, ShaderVisibleCPUHandle, sampler->CpuHandle, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);


	CommandList->SetGraphicsRootDescriptorTable(samplerBinding[name].rootParamIndex, ShaderVisibleGpuHandle);

}

void Shader::SetGlobalConstantBuffer(string name, GlobalConstantBuffer* gcb, ID3D12GraphicsCommandList* CommandList, ThreadDescriptorHeapPool* DHPool)
{
	BindingData& binding = constantBufferBinding[name];

	//binding.cbs = gcb->cbs;

	ConstantBuffer* cb = nullptr;

	D3D12_CPU_DESCRIPTOR_HANDLE ShaderVisibleCPUHandle;
	D3D12_GPU_DESCRIPTOR_HANDLE ShaderVisibleGpuHandle;

	if (gcb->cbs.size() != 0)
		cb = gcb->cbs[g_dx12_rhi->CurrentFrameIndex].get();

	{
		UINT DrawCallIndex;
		
		DrawCallIndex = 0;

		if (cb->CpuHandleVec.size() == 1) // this is global constant buffer(can be same for all draw)
		{
			//if (!cb->isPopulatedThisFrame)
			{
				D3D12_CPU_DESCRIPTOR_HANDLE NonShadervisibleCPUHandle = cb->CpuHandleVec[DrawCallIndex];

				//g_dx12_rhi->SRVCBVDescriptorHeapShaderVisible->AllocDescriptor(ShaderVisibleCPUHandle, cb->GpuHandle);
				if (DHPool)
					DHPool->AllocDescriptor(ShaderVisibleCPUHandle, ShaderVisibleGpuHandle);
				else
					g_dx12_rhi->SRVCBVDescriptorHeapShaderVisible->AllocDescriptor(ShaderVisibleCPUHandle, ShaderVisibleGpuHandle);


				g_dx12_rhi->Device->CopyDescriptorsSimple(1, ShaderVisibleCPUHandle, NonShadervisibleCPUHandle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
				//cb->isPopulatedThisFrame = true;
			}
		}
	
	}

	CommandList->SetGraphicsRootDescriptorTable(binding.rootParamIndex, ShaderVisibleGpuHandle);
}

void Shader::SetConstantValue(string name, void* pData, ID3D12GraphicsCommandList* CommandList, ThreadDescriptorHeapPool* DHPool)
{
	BindingData& binding = constantBufferBinding[name];

	
	UINT CBViewIndex = g_dx12_rhi->CurrentFrameIndex * NumMaxDrawCall + currentDrawCallIndex;

	UINT8* pMapped = (UINT8*)binding.cb->MemMapped + binding.cbSize*CBViewIndex;
	memcpy((void*)pMapped, pData, binding.cbSize);


	D3D12_CPU_DESCRIPTOR_HANDLE ShaderVisibleCPUHandle;
	D3D12_GPU_DESCRIPTOR_HANDLE ShaderVisibleGpuHandle;


	D3D12_CPU_DESCRIPTOR_HANDLE NonShadervisibleCPUHandle = binding.cb->CpuHandleVec[CBViewIndex];

	if (DHPool)
		DHPool->AllocDescriptor(ShaderVisibleCPUHandle, ShaderVisibleGpuHandle);
	else
		g_dx12_rhi->SRVCBVDescriptorHeapShaderVisible->AllocDescriptor(ShaderVisibleCPUHandle, ShaderVisibleGpuHandle);


	g_dx12_rhi->Device->CopyDescriptorsSimple(1, ShaderVisibleCPUHandle, NonShadervisibleCPUHandle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	CommandList->SetGraphicsRootDescriptorTable(binding.rootParamIndex, ShaderVisibleGpuHandle);
}

void Shader::SetRootConstant(string name, UINT value, ID3D12GraphicsCommandList* CommandList)
{
	rootBinding[name].rootConst = value;
	CommandList->SetGraphicsRoot32BitConstant(rootBinding[name].rootParamIndex, rootBinding[name].rootConst, 0);
}

Shader::Shader(UINT8* ByteCode, UINT Size)
{
	ShaderByteCode = CD3DX12_SHADER_BYTECODE(ByteCode, Size);
}

void PipelineStateObject::Init(bool isCompute)
{

	vector<CD3DX12_ROOT_PARAMETER1> rootParamVec;

	vector<CD3DX12_DESCRIPTOR_RANGE1> TextureRanges;
	if (ps && ps->textureBinding.size() != 0)
	{
		TextureRanges.resize(ps->textureBinding.size());
		int i = 0;
		for (auto& bindingPair : ps->textureBinding)
		{
			Shader::BindingData& bindingData = bindingPair.second;

			CD3DX12_ROOT_PARAMETER1 TextureParam;
			TextureRanges[i].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, bindingData.numDescriptors, bindingData.baseRegister, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
			TextureParam.InitAsDescriptorTable(1, &TextureRanges[i], D3D12_SHADER_VISIBILITY_PIXEL);
			rootParamVec.push_back(TextureParam);
			i++;
			bindingData.rootParamIndex = RootParamIndex++;
		}
	}
	
	vector<CD3DX12_DESCRIPTOR_RANGE1> SamplerRanges;
	if (ps && ps->samplerBinding.size() != 0)
	{
		SamplerRanges.resize(ps->samplerBinding.size());
		int i = 0;
		for (auto& bindingPair : ps->samplerBinding)
		{
			Shader::BindingData& bindingData = bindingPair.second;

			CD3DX12_ROOT_PARAMETER1 SamplerParam;
			SamplerRanges[i].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, bindingData.numDescriptors, bindingData.baseRegister);
			SamplerParam.InitAsDescriptorTable(1, &SamplerRanges[i], D3D12_SHADER_VISIBILITY_PIXEL);
			rootParamVec.push_back(SamplerParam);
			i++;
			bindingData.rootParamIndex = RootParamIndex++;
		}
	}
	

	if (ps && ps->rootBinding.size() != 0)
	{
		int i = 0;
		for (auto& bindingPair : ps->rootBinding)
		{
			Shader::BindingData& bindingData = bindingPair.second;

			CD3DX12_ROOT_PARAMETER1 ConstantParam;
			ConstantParam.InitAsConstants(bindingData.numDescriptors, bindingData.baseRegister, 0, D3D12_SHADER_VISIBILITY_PIXEL);
			rootParamVec.push_back(ConstantParam);
			i++;
			bindingData.rootParamIndex = RootParamIndex++;
		}
	}

	vector<CD3DX12_DESCRIPTOR_RANGE1> CBRangesPS;
	if (ps && ps->constantBufferBinding.size() != 0)
	{

		CBRangesPS.resize(ps->constantBufferBinding.size());
		int i = 0;
		for (auto& bindingPair : ps->constantBufferBinding)
		{
			Shader::BindingData& bindingData = bindingPair.second;

			CD3DX12_ROOT_PARAMETER1 CBParam;
			CBRangesPS[i].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, bindingData.numDescriptors, bindingData.baseRegister, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
			CBParam.InitAsDescriptorTable(1, &CBRangesPS[i], D3D12_SHADER_VISIBILITY_PIXEL);

			rootParamVec.push_back(CBParam);
			i++;
			bindingData.rootParamIndex = RootParamIndex++;
		}
	}
	
	
	vector<CD3DX12_DESCRIPTOR_RANGE1> CBRanges;
	if (vs && vs->constantBufferBinding.size() != 0)
	{
		CBRanges.resize(vs->constantBufferBinding.size());
		int i = 0;
		for (auto& bindingPair : vs->constantBufferBinding)
		{
			Shader::BindingData& bindingData = bindingPair.second;

			CD3DX12_ROOT_PARAMETER1 CBParam;
			CBRanges[i].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, bindingData.numDescriptors, bindingData.baseRegister, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
			CBParam.InitAsDescriptorTable(1, &CBRanges[i], D3D12_SHADER_VISIBILITY_VERTEX);

			rootParamVec.push_back(CBParam);
			i++;
			bindingData.rootParamIndex = RootParamIndex++;
		}
	}
	
	vector<CD3DX12_DESCRIPTOR_RANGE1> TextureRangesCS;
	if (cs && cs->textureBinding.size() != 0)
	{
		TextureRangesCS.resize(cs->textureBinding.size());
		int i = 0;
		for (auto& bindingPair : cs->textureBinding)
		{
			Shader::BindingData& bindingData = bindingPair.second;

			CD3DX12_ROOT_PARAMETER1 TextureParam;
			TextureRangesCS[i].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, bindingData.numDescriptors, bindingData.baseRegister, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
			TextureParam.InitAsDescriptorTable(1, &TextureRangesCS[i], D3D12_SHADER_VISIBILITY_ALL);
			rootParamVec.push_back(TextureParam);
			i++;
			bindingData.rootParamIndex = RootParamIndex++;
		}
	}

	vector<CD3DX12_DESCRIPTOR_RANGE1> UAVRanges;
	if (cs && cs->uavBinding.size() != 0)
	{
		UAVRanges.resize(cs->uavBinding.size());
		int i = 0;
		for (auto& bindingPair : cs->uavBinding)
		{
			Shader::BindingData& bindingData = bindingPair.second;
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
	if (isCompute)
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

void PipelineStateObject::ApplyGlobal(ID3D12GraphicsCommandList* CommandList)
{
	CommandList->SetGraphicsRootSignature(RS.Get());
	CommandList->SetPipelineState(PSO.Get());

	for (auto&bd : ps->samplerBinding)
	{
		D3D12_CPU_DESCRIPTOR_HANDLE ShaderVisibleCPUHandle;
		D3D12_GPU_DESCRIPTOR_HANDLE ShaderVisibleGpuHandle;

		
		g_dx12_rhi->SamplerDescriptorHeapShaderVisible->AllocDescriptor(ShaderVisibleCPUHandle, ShaderVisibleGpuHandle);

		g_dx12_rhi->Device->CopyDescriptorsSimple(1, ShaderVisibleCPUHandle, bd.second.sampler->CpuHandle, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
		CommandList->SetGraphicsRootDescriptorTable(bd.second.rootParamIndex, ShaderVisibleGpuHandle);
	}

	for (auto& bd : ps->rootBinding)
	{
		CommandList->SetGraphicsRoot32BitConstant(bd.second.rootParamIndex, bd.second.rootConst, 0);
	}


	for (auto& bd : ps->textureBinding)
	{
		Texture* tex = bd.second.texture;
		D3D12_CPU_DESCRIPTOR_HANDLE ShaderVisibleCPUHandle;
		g_dx12_rhi->SRVCBVDescriptorHeapShaderVisible->AllocDescriptor(ShaderVisibleCPUHandle, tex->GpuHandleSRV);
		g_dx12_rhi->Device->CopyDescriptorsSimple(1, ShaderVisibleCPUHandle, tex->CpuHandleSRV, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		CommandList->SetGraphicsRootDescriptorTable(bd.second.rootParamIndex, bd.second.texture->GpuHandleSRV);
	}

	for (auto& bd : ps->uavBinding)
	{
		Texture* tex = bd.second.texture;
		D3D12_CPU_DESCRIPTOR_HANDLE ShaderVisibleCPUHandle;
		g_dx12_rhi->SRVCBVDescriptorHeapShaderVisible->AllocDescriptor(ShaderVisibleCPUHandle, tex->GpuHandleUAV);
		g_dx12_rhi->Device->CopyDescriptorsSimple(1, ShaderVisibleCPUHandle, tex->CpuHandleUAV, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		CommandList->SetGraphicsRootDescriptorTable(bd.second.rootParamIndex, tex->GpuHandleUAV);
	}
}

void PipelineStateObject::ApplyCS(ID3D12GraphicsCommandList* CommandList)
{
	CommandList->SetComputeRootSignature(RS.Get());
	CommandList->SetPipelineState(PSO.Get());

	for (auto&bd : cs->samplerBinding)
	{
		CommandList->SetComputeRootDescriptorTable(bd.second.rootParamIndex, bd.second.sampler->GpuHandle);
	}

	for (auto& bd : cs->rootBinding)
	{
		CommandList->SetComputeRoot32BitConstant(bd.second.rootParamIndex, bd.second.rootConst, 0);
	}


	for (auto& bd : cs->textureBinding)
	{
		Texture* tex = bd.second.texture;
		D3D12_CPU_DESCRIPTOR_HANDLE ShaderVisibleCPUHandle;
		g_dx12_rhi->SRVCBVDescriptorHeapShaderVisible->AllocDescriptor(ShaderVisibleCPUHandle, tex->GpuHandleSRV);
		g_dx12_rhi->Device->CopyDescriptorsSimple(1, ShaderVisibleCPUHandle, tex->CpuHandleSRV, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		CommandList->SetComputeRootDescriptorTable(bd.second.rootParamIndex, bd.second.texture->GpuHandleSRV);
	}

	for (auto& bd : cs->uavBinding)
	{
		Texture* tex = bd.second.texture;
		D3D12_CPU_DESCRIPTOR_HANDLE ShaderVisibleCPUHandle;
		g_dx12_rhi->SRVCBVDescriptorHeapShaderVisible->AllocDescriptor(ShaderVisibleCPUHandle, tex->GpuHandleUAV);
		g_dx12_rhi->Device->CopyDescriptorsSimple(1, ShaderVisibleCPUHandle, tex->CpuHandleUAV, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		CommandList->SetComputeRootDescriptorTable(bd.second.rootParamIndex, tex->GpuHandleUAV);
	}
}

void PipelineStateObject::ApplyGraphicsRSPSO(ID3D12GraphicsCommandList* CommandList)
{
	CommandList->SetGraphicsRootSignature(RS.Get());
	CommandList->SetPipelineState(PSO.Get());
}

void PipelineStateObject::ApplyComputeRSPSO(ID3D12GraphicsCommandList* CommandList)
{
	CommandList->SetComputeRootSignature(RS.Get());
	CommandList->SetPipelineState(PSO.Get());
}

UINT PipelineStateObject::GetGraphicsBindingDHSize()
{
	return	vs->constantBufferBinding.size() + ps->constantBufferBinding.size() + vs->textureBinding.size() + ps->textureBinding.size();
}

//UINT PipelineStateObject::GetGraphicsCBDHBindingSize()
//{
//	UINT Size = 0;
//	for (auto& cbbinding : vs->constantBufferBinding)
//	{
//		Size += cbbinding.second.cbs[g_dx12_rhi->CurrentFrameIndex]->CpuHandleVec.size();
//	}
//
//	for (auto& cbbinding : ps->constantBufferBinding)
//	{
//		Size += cbbinding.second.cbs[g_dx12_rhi->CurrentFrameIndex]->CpuHandleVec.size();
//	}
//	
//	return Size;
//}

void GlobalConstantBuffer::SetValue(void* pData)
{
	void* pMapped = cbs[g_dx12_rhi->CurrentFrameIndex]->MemMapped;
	memcpy(pMapped, pData, cbSize);
}

ConstantBuffer* GlobalConstantBuffer::GetCurrentFrameCB()
{
	return cbs[g_dx12_rhi->CurrentFrameIndex].get();
}

GlobalConstantBuffer::GlobalConstantBuffer(UINT size)
{
	cbSize = size;
	for (int i = 0; i < g_dx12_rhi->NumFrame; i++)
	{
		cbs.push_back(g_dx12_rhi->CreateConstantBuffer(size));
	}
}

void Texture::VisibleThisFrame()
{
	g_dx12_rhi->FrameTextureSet.insert(this);
}

void Texture::MakeUAV()
{
	g_dx12_rhi->SRVCBVDescriptorHeapStorage->AllocDescriptor(CpuHandleUAV, GpuHandleUAV);

	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	uavDesc.Format = Format;
	g_dx12_rhi->Device->CreateUnorderedAccessView(resource.Get(), nullptr, &uavDesc, CpuHandleUAV);
}

void Texture::MakeSRV(bool isDepth)
{
	g_dx12_rhi->SRVCBVDescriptorHeapStorage->AllocDescriptor(CpuHandleSRV, GpuHandleSRV);

	D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc = {};
	SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	if (isDepth)
		SrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
	else
		SrvDesc.Format = Format;

	SrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	SrvDesc.Texture2D.MipLevels = textureDesc.MipLevels;
	g_dx12_rhi->Device->CreateShaderResourceView(resource.Get(), &SrvDesc, CpuHandleSRV);
}

void Texture::MakeRTV()
{
	g_dx12_rhi->RTVDescriptorHeap->AllocDescriptor(CpuHandleRTV, GpuHandleRTV);
	
	D3D12_RENDER_TARGET_VIEW_DESC desc = {};
	desc.Format = Format;
	desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

	g_dx12_rhi->Device->CreateRenderTargetView(resource.Get(), &desc, CpuHandleRTV);
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



void Texture::UploadSRCData(D3D12_SUBRESOURCE_DATA* SrcData)
{
	// upload src data
	if (SrcData)
	{
		ComPtr<ID3D12Resource> textureUploadHeap;

		const UINT subresourceCount = textureDesc.DepthOrArraySize * textureDesc.MipLevels;
		const UINT64 uploadBufferSize = GetRequiredIntermediateSize(resource.Get(), 0, subresourceCount);

		D3D12_HEAP_PROPERTIES heapProp;
		heapProp.Type = D3D12_HEAP_TYPE_UPLOAD;
		heapProp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		heapProp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
		heapProp.CreationNodeMask = 1;
		heapProp.VisibleNodeMask = 1;

		D3D12_RESOURCE_DESC resDesc;

		resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		resDesc.Alignment = 0;
		resDesc.Width = uploadBufferSize;
		resDesc.Height = 1;
		resDesc.DepthOrArraySize = 1;
		resDesc.MipLevels = textureDesc.MipLevels;
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
			IID_PPV_ARGS(&textureUploadHeap)));

		NAME_D3D12_OBJECT(textureUploadHeap);


		UpdateSubresources(g_dx12_rhi->CommandList.Get(), resource.Get(), textureUploadHeap.Get(), 0, 0, subresourceCount, SrcData);

		D3D12_RESOURCE_BARRIER BarrierDesc = {};
		BarrierDesc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		BarrierDesc.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		BarrierDesc.Transition.pResource = resource.Get();
		BarrierDesc.Transition.Subresource = 0;
		BarrierDesc.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		BarrierDesc.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		g_dx12_rhi->CommandList->ResourceBarrier(1, &BarrierDesc);

		ThrowIfFailed(g_dx12_rhi->CommandList->Close());
		ID3D12CommandList* ppCommandLists[] = { g_dx12_rhi->CommandList.Get() };
		g_dx12_rhi->CommandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
		ThrowIfFailed(g_dx12_rhi->CommandList->Reset(g_dx12_rhi->GetCurrentCA(), nullptr));

		g_dx12_rhi->WaitGPU();

		//ThrowIfFailed(g_dx12_rhi->CommandList->Reset(g_dx12_rhi->CommandAllocator.Get(), nullptr));

	}
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

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC asDesc = {};
	asDesc.Inputs = inputs;
	asDesc.DestAccelerationStructureData = as->Result->GetGPUVirtualAddress();
	asDesc.ScratchAccelerationStructureData = as->Scratch->GetGPUVirtualAddress();

	g_dx12_rhi->CommandList->BuildRaytracingAccelerationStructure(&asDesc, 0, nullptr);

	D3D12_RESOURCE_BARRIER uavBarrier = {};
	uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	uavBarrier.UAV.pResource = as->Result.Get();
	g_dx12_rhi->CommandList->ResourceBarrier(1, &uavBarrier);

	return shared_ptr<RTAS>(as);
}

std::shared_ptr<RTAS> Mesh::CreateBLASArray()
{
	RTAS* as = new  RTAS;

	int indexSize;
	if (IndexFormat == DXGI_FORMAT_R32_UINT)
	{
		indexSize = 4;
	}
	else if (IndexFormat == DXGI_FORMAT_R16_UINT)
	{
		indexSize = 2;
	}
	
	int i = 0;
	vector< D3D12_RAYTRACING_GEOMETRY_DESC> geomVec;
	for (auto& drawcall : Draws)
	{
		if (i == 1024)
			int a = 0;
		D3D12_RAYTRACING_GEOMETRY_DESC geomDesc = {};
		geomDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
		uint64_t vbstart = Vb->resource->GetGPUVirtualAddress();
		geomDesc.Triangles.VertexBuffer.StartAddress = Vb->resource->GetGPUVirtualAddress() + drawcall.VertexBase *VertexStride;
		geomDesc.Triangles.VertexBuffer.StrideInBytes = VertexStride;;
		geomDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
		geomDesc.Triangles.VertexCount = drawcall.VertexCount;
		geomDesc.Triangles.IndexBuffer = Ib->resource->GetGPUVirtualAddress() + drawcall.IndexStart * indexSize;
		geomDesc.Triangles.IndexFormat = IndexFormat;
		geomDesc.Triangles.IndexCount = drawcall.IndexCount;// Ib->numIndices;
		geomDesc.Triangles.Transform3x4 = 0;

		geomDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
		geomVec.push_back(geomDesc);

		i++;
	}

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
	inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
	inputs.NumDescs = geomVec.size();
	inputs.pGeometryDescs = geomVec.data();
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

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC asDesc = {};
	asDesc.Inputs = inputs;
	asDesc.DestAccelerationStructureData = as->Result->GetGPUVirtualAddress();
	asDesc.ScratchAccelerationStructureData = as->Scratch->GetGPUVirtualAddress();

	g_dx12_rhi->CommandList->BuildRaytracingAccelerationStructure(&asDesc, 0, nullptr);

	D3D12_RESOURCE_BARRIER uavBarrier = {};
	uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	uavBarrier.UAV.pResource = as->Result.Get();
	g_dx12_rhi->CommandList->ResourceBarrier(1, &uavBarrier);

	return shared_ptr<RTAS>(as);

}

shared_ptr<RTAS> DumRHI_DX12::CreateTLAS(vector<shared_ptr<RTAS>>& VecBottomLevelAS, uint64_t& tlasSize)
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
		bufDesc.Width = sizeof(D3D12_RAYTRACING_INSTANCE_DESC);

		g_dx12_rhi->Device->CreateCommittedResource(&kUploadHeapProps, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&as->Instance));
	}

	D3D12_RAYTRACING_INSTANCE_DESC* pInstanceDesc;
	as->Instance->Map(0, nullptr, (void**)&pInstanceDesc);
	ZeroMemory(pInstanceDesc, sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * VecBottomLevelAS.size());

	for (int i = 0; i < VecBottomLevelAS.size(); i++)
	{
		pInstanceDesc[i].InstanceID = 0;                            // This value will be exposed to the shader via InstanceID()
		pInstanceDesc[i].InstanceContributionToHitGroupIndex = 0;   // This is the offset inside the shader-table. We only have a single geometry, so the offset 0
		pInstanceDesc[i].Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
		glm::mat4 m; // Identity matrix
		memcpy(pInstanceDesc[i].Transform, &m, sizeof(pInstanceDesc[i].Transform));
		pInstanceDesc[i].AccelerationStructure = VecBottomLevelAS[i]->Result->GetGPUVirtualAddress();
		pInstanceDesc[i].InstanceMask = 0xFF;
	}

	as->Instance->Unmap(0, nullptr);

	// Create the TLAS
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC asDesc = {};
	asDesc.Inputs = inputs;
	asDesc.Inputs.InstanceDescs = as->Instance->GetGPUVirtualAddress();
	asDesc.DestAccelerationStructureData = as->Result->GetGPUVirtualAddress();
	asDesc.ScratchAccelerationStructureData = as->Scratch->GetGPUVirtualAddress();

	CommandList->BuildRaytracingAccelerationStructure(&asDesc, 0, nullptr);

	// We need to insert a UAV barrier before using the acceleration structures in a raytracing operation
	D3D12_RESOURCE_BARRIER uavBarrier = {};
	uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	uavBarrier.UAV.pResource = as->Result.Get();
	CommandList->ResourceBarrier(1, &uavBarrier);
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
	gDxcDllHelper.CreateInstance(CLSID_DxcCompiler, __uuidof(IDxcCompiler), &pCompiler);
	gDxcDllHelper.CreateInstance(CLSID_DxcLibrary, __uuidof(IDxcLibrary), &pLibrary);

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
	pCompiler->Compile(pTextBlob.Get(), filename, L"", targetString, nullptr, 0, nullptr, 0, nullptr, &pResult);

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
	binding.cbSize = (div + 1) * 256;


	for (int iFrame = 0; iFrame < g_dx12_rhi->NumFrame; iFrame++)
	{
		binding.cbs.push_back(g_dx12_rhi->CreateConstantBuffer(binding.cbSize, numInstance));
	}


	bindingInfo.Binding.push_back(binding);
}

void RTPipelineStateObject::BeginShaderTable()
{
	HitProgramBinding.clear();
}

void RTPipelineStateObject::EndShaderTable()
{
	// raygen : simple, it is just the begin of table
	// miss : raygen + miss index * EntrySize
	// hit : raygen + miss(N) + instanceIndex
	uint8_t* pData;
	HRESULT hr = ShaderTable->Map(0, nullptr, (void**)&pData);
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
					D3D12_CPU_DESCRIPTOR_HANDLE CPUHandle = bd.CPUHandle;

					D3D12_CPU_DESCRIPTOR_HANDLE ShaderVisibleCPU;
					D3D12_GPU_DESCRIPTOR_HANDLE ShaderVisibleGPU;

					g_dx12_rhi->SamplerDescriptorHeapShaderVisible->AllocDescriptor(ShaderVisibleCPU, ShaderVisibleGPU);

					g_dx12_rhi->Device->CopyDescriptorsSimple(1, ShaderVisibleCPU, CPUHandle, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

					*(UINT64*)(pDataThis) = ShaderVisibleGPU.ptr;

				}
				else
				{
					D3D12_CPU_DESCRIPTOR_HANDLE CPUHandle = bd.CPUHandle;
					D3D12_GPU_DESCRIPTOR_HANDLE ShaderVisibleGPU;
					D3D12_CPU_DESCRIPTOR_HANDLE ShaderVisibleCPU;

					g_dx12_rhi->SRVCBVDescriptorHeapShaderVisible->AllocDescriptor(ShaderVisibleCPU, ShaderVisibleGPU);
					g_dx12_rhi->Device->CopyDescriptorsSimple(1, ShaderVisibleCPU, CPUHandle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
					*(UINT64*)(pDataThis) = ShaderVisibleGPU.ptr;

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
			D3D12_CPU_DESCRIPTOR_HANDLE CPUHandle = bd;
			D3D12_GPU_DESCRIPTOR_HANDLE ShaderVisibleGPU;
			D3D12_CPU_DESCRIPTOR_HANDLE ShaderVisibleCPU;

			g_dx12_rhi->SRVCBVDescriptorHeapShaderVisible->AllocDescriptor(ShaderVisibleCPU, ShaderVisibleGPU);
			g_dx12_rhi->Device->CopyDescriptorsSimple(1, ShaderVisibleCPU, CPUHandle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

			*(UINT64*)(pDataThis) = ShaderVisibleGPU.ptr;
			
			pDataThis += sizeof(UINT64);
		}

		LastIndex++;
	}

	ShaderTable->Unmap(0, nullptr);
}

void RTPipelineStateObject::SetUAV(string shader, string bindingName, D3D12_CPU_DESCRIPTOR_HANDLE uavHandle, INT instanceIndex /*= -1*/)
{
	// each bindings of raygen/miss shader is unique to shader name.
	if (instanceIndex == -1) // raygen, miss
	{
		BindingInfo& bi = ShaderBinding[shader];
		for (auto&bd : bi.Binding)
		{
			if (bd.name == bindingName)
			{
				bd.CPUHandle = uavHandle;

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

void RTPipelineStateObject::SetSRV(string shader, string bindingName, D3D12_CPU_DESCRIPTOR_HANDLE srvHandle, INT instanceIndex /*= -1*/)
{
	// each bindings of raygen/miss shader is unique to shader name.
	if (instanceIndex == -1) // raygen, miss
	{
		BindingInfo& bi = ShaderBinding[shader];
		for (auto&bd : bi.Binding)
		{
			if (bd.name == bindingName)
			{
				bd.CPUHandle = srvHandle;
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
				bd.CPUHandle = sampler->CpuHandle;
			}
		}
	}
	else // hitprogram : There can be multiple hitprogram entry with same shader name, so we need another data structure. (HitProgramBinding)
	{
		// SetXXX should be called according to the binding order, because it is push_backed to vector.
		HitProgramBinding[instanceIndex].HitGroupName = MapHitGroup[shader].name;
		HitProgramBinding[instanceIndex].VecData.push_back(sampler->CpuHandle);
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

				bd.CPUHandle = cb->CpuHandleVec[0];
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
				HitProgramBinding[instanceIndex].VecData.push_back(cb->CpuHandleVec[instanceIndex]);
			}
		}
	}
}

void RTPipelineStateObject::SetHitProgram(string shader, UINT instanceIndex)
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

		vector<D3D12_ROOT_PARAMETER> rootParamVecSampler;
		vector<D3D12_DESCRIPTOR_RANGE> RangesSampler;

		if (bindingInfo.Binding.size() > 0)
		{
			// create root signature
			
			for (auto& bindingData : bindingInfo.Binding)
			{
				D3D12_DESCRIPTOR_RANGE Range = {};
				Range.RangeType = bindingData.Type;
				Range.BaseShaderRegister = bindingData.BaseRegister;
				Range.NumDescriptors = 1;
				Range.RegisterSpace = 0;
				Range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

				if (bindingData.Type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER)
				{
					RangesSampler.push_back(Range);
				}
				else
				{
					Ranges.push_back(Range);
				}
				
			}

			D3D12_ROOT_PARAMETER RootParam = {};
			RootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			RootParam.DescriptorTable.NumDescriptorRanges = Ranges.size();
			RootParam.DescriptorTable.pDescriptorRanges = Ranges.data();
			RootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

			rootParamVec.push_back(RootParam);

			if (RangesSampler.size() > 0)
			{
				D3D12_ROOT_PARAMETER RootParamSampler = {};
				RootParamSampler.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
				RootParamSampler.DescriptorTable.NumDescriptorRanges = RangesSampler.size();
				RootParamSampler.DescriptorTable.pDescriptorRanges = RangesSampler.data();
				RootParamSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

				rootParamVec.push_back(RootParamSampler);
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
		bufDesc.Width = ShaderTableSize;

		const D3D12_HEAP_PROPERTIES kUploadHeapProps =
		{
			D3D12_HEAP_TYPE_UPLOAD,
			D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
			D3D12_MEMORY_POOL_UNKNOWN,
			0,
			0,
		};

		g_dx12_rhi->Device->CreateCommittedResource(&kUploadHeapProps, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ShaderTable));

	}

}

void RTPipelineStateObject::Apply(UINT width, UINT height)
{
	D3D12_DISPATCH_RAYS_DESC raytraceDesc = {};
	raytraceDesc.Width = width;
	raytraceDesc.Height = height;
	raytraceDesc.Depth = 1;

	raytraceDesc.RayGenerationShaderRecord.StartAddress = ShaderTable->GetGPUVirtualAddress() + 0 * ShaderTableEntrySize;
	raytraceDesc.RayGenerationShaderRecord.SizeInBytes = ShaderTableEntrySize;

	// Miss is the second entry in the shader-table
	size_t missOffset = 1 * ShaderTableEntrySize;
	raytraceDesc.MissShaderTable.StartAddress = ShaderTable->GetGPUVirtualAddress() + missOffset;
	raytraceDesc.MissShaderTable.StrideInBytes = ShaderTableEntrySize;
	raytraceDesc.MissShaderTable.SizeInBytes = ShaderTableEntrySize;   // Only a s single miss-entry


	 // Hit is the third entry in the shader-table
	size_t hitOffset = 2 * ShaderTableEntrySize;
	raytraceDesc.HitGroupTable.StartAddress = ShaderTable->GetGPUVirtualAddress() + hitOffset;
	raytraceDesc.HitGroupTable.StrideInBytes = ShaderTableEntrySize;
	raytraceDesc.HitGroupTable.SizeInBytes = ShaderTableEntrySize;

	// Bind the empty root signature

	g_dx12_rhi->CommandList->SetComputeRootSignature(GlobalRS.Get());
	g_dx12_rhi->CommandList->SetPipelineState1(RTPipelineState.Get());


	GFSDK_Aftermath_SetEventMarker(g_dx12_rhi->AM_CL_Handle, nullptr, 0);

	g_dx12_rhi->CommandList->DispatchRays(&raytraceDesc);
}

