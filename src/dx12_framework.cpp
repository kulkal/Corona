//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

#include "stdafx.h"
#include "dx12_framework.h"
#include <iostream>
#include <algorithm>
#include <array>
#include "enkiTS/TaskScheduler.h""
#include "DXCAPI/dxcapi.use.h"
#include "Utils.h"

#include <dxgidebug.h>


#include "assimp/include/Importer.hpp"
#include "assimp/include/scene.h"
#include "assimp/include/postprocess.h"
//#pragma comment(lib, "assimp\\lib\\assimp.lib")

#include "GFSDK_Aftermath/include/GFSDK_Aftermath.h"

#include <sstream>
#include <fstream>
#include <variant>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"

#define arraysize(a) (sizeof(a)/sizeof(a[0]))
#define align_to(_alignment, _val) (((_val + _alignment - 1) / _alignment) * _alignment)


static dxc::DxcDllSupport gDxcDllHelper;

enki::TaskScheduler g_TS;


using namespace glm;

template<class BlotType>
std::string convertBlobToString(BlotType* pBlob)
{
	std::vector<char> infoLog(pBlob->GetBufferSize() + 1);
	memcpy(infoLog.data(), pBlob->GetBufferPointer(), pBlob->GetBufferSize());
	infoLog[pBlob->GetBufferSize()] = 0;
	return std::string(infoLog.data());
}

ComPtr<ID3DBlob> compileLibrary(const WCHAR* filename, const WCHAR* targetString)
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
		return nullptr;
	}

	ID3DBlob* pBlob;
	pResult->GetResult((IDxcBlob**)&pBlob);
	return ComPtr<ID3DBlob>(pBlob);
}


ComPtr<ID3D12RootSignature> createRootSignature(ComPtr<ID3D12Device5> pDevice, const D3D12_ROOT_SIGNATURE_DESC& desc)
{
	ComPtr<ID3DBlob> pSigBlob;
	ComPtr<ID3DBlob> pErrorBlob;
	HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &pSigBlob, &pErrorBlob);
	if (FAILED(hr))
	{
		std::string msg = convertBlobToString(pErrorBlob.Get());
		//msgBox(msg);
		return nullptr;
	}
	ComPtr<ID3D12RootSignature> pRootSig;
	pDevice->CreateRootSignature(0, pSigBlob->GetBufferPointer(), pSigBlob->GetBufferSize(), IID_PPV_ARGS(&pRootSig));
	return pRootSig;
}

struct RootSignatureDesc
{
	D3D12_ROOT_SIGNATURE_DESC desc = {};
	std::vector<D3D12_DESCRIPTOR_RANGE> range;
	std::vector<D3D12_ROOT_PARAMETER> rootParams;
};

RootSignatureDesc createRayGenRootDesc()
{
	// Create the root-signature
	RootSignatureDesc desc;
	desc.range.resize(3);
	// gOutput
	desc.range[0].BaseShaderRegister = 0;
	desc.range[0].NumDescriptors = 1;
	desc.range[0].RegisterSpace = 0;
	desc.range[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	desc.range[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	// gRtScene
	desc.range[1].BaseShaderRegister = 0;
	desc.range[1].NumDescriptors = 1;
	desc.range[1].RegisterSpace = 0;
	desc.range[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	desc.range[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	desc.range[2].BaseShaderRegister = 0;
	desc.range[2].NumDescriptors = 1;
	desc.range[2].RegisterSpace = 0;
	desc.range[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
	desc.range[2].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;


	desc.rootParams.resize(1);
	desc.rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	desc.rootParams[0].DescriptorTable.NumDescriptorRanges = 3;
	desc.rootParams[0].DescriptorTable.pDescriptorRanges = desc.range.data();

	// Create the desc
	desc.desc.NumParameters = 1;
	desc.desc.pParameters = desc.rootParams.data();
	desc.desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;

	return desc;
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


static const WCHAR* kRayGenShader = L"rayGen";
static const WCHAR* kMissShader = L"miss";
static const WCHAR* kClosestHitShader = L"chs";
static const WCHAR* kHitGroup = L"HitGroup";



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
		pRootSig = createRootSignature(pDevice, desc);
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
		pRootSig = createRootSignature(pDevice, desc);
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


dx12_framework::dx12_framework(UINT width, UINT height, std::wstring name) :
	DXSample(width, height, name),
	m_viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)),
	m_scissorRect(0, 0, static_cast<LONG>(width), static_cast<LONG>(height))
{
}

dx12_framework::~dx12_framework()
{
	
}

void dx12_framework::OnInit()
{
	CoInitialize(NULL);

	g_TS.Initialize(8);

	m_camera.Init({0, 0, 0 });
	m_camera.SetMoveSpeed(200);

	LoadPipeline();
	LoadAssets();
}

// Load the rendering pipeline dependencies.
void dx12_framework::LoadPipeline()
{
	

	UINT dxgiFactoryFlags = 0;

#if  defined(_DEBUG)
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
		spDebugController1->SetEnableGPUBasedValidation(true);
	}
#endif

	ComPtr<IDXGIFactory4> factory;
	ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory)));

	ComPtr<IDXGIAdapter1> hardwareAdapter;
	//GetHardwareAdapter(factory.Get(), &hardwareAdapter);
	for (uint32_t i = 0; DXGI_ERROR_NOT_FOUND != factory->EnumAdapters1(i, &hardwareAdapter); i++)
	{
		DXGI_ADAPTER_DESC1 desc;
		hardwareAdapter->GetDesc1(&desc);

		// Skip SW adapters
		if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

		ThrowIfFailed(D3D12CreateDevice(
			hardwareAdapter.Get(),
			D3D_FEATURE_LEVEL_12_1,
			IID_PPV_ARGS(&m_device)
		));

		D3D12_FEATURE_DATA_D3D12_OPTIONS5 features5;
		HRESULT hr = m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &features5, sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS5));
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
		if (SUCCEEDED(m_device->QueryInterface(__uuidof(ID3D12InfoQueue), (void**)&d3dInfoQueue)))
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

	dx12_rhi = std::make_unique<DumRHI_DX12>(m_device);

	// Describe and create the swap chain.
	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
	swapChainDesc.BufferCount = dx12_rhi->NumFrame;
	swapChainDesc.Width = m_width;
	swapChainDesc.Height = m_height;
	swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swapChainDesc.SampleDesc.Count = 1;

	ComPtr<IDXGISwapChain1> swapChain;
	ThrowIfFailed(factory->CreateSwapChainForHwnd(
		dx12_rhi->CommandQueue.Get(),		// Swap chain needs the queue so that it can force a flush on it.
		Win32Application::GetHwnd(),
		&swapChainDesc,
		nullptr,
		nullptr,
		&swapChain
		));

	// This sample does not support fullscreen transitions.
	ThrowIfFailed(factory->MakeWindowAssociation(Win32Application::GetHwnd(), DXGI_MWA_NO_ALT_ENTER));

	ThrowIfFailed(swapChain.As(&m_swapChain));
	//dx12_rhi->m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

	dx12_rhi->m_swapChain = m_swapChain;
}

// Load the sample assets.
void dx12_framework::LoadAssets()
{
	InitBlueNoiseTexture();
	InitImgui();
	InitComputeRS();
	InitDrawMeshRS();
	InitCopyPass();
	InitLightingPass();
	InitTemporalAAPass();
	InitDenoiserPass();
	
	for (UINT i = 0; i < dx12_rhi->NumFrame; i++)
	{

		ComPtr<ID3D12Resource> rendertarget;
		ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&rendertarget)));

		// create each rtv for one actual resource(swapchain)
		shared_ptr<Texture> rt = dx12_rhi->CreateTexture2DFromResource(rendertarget);
		rt->MakeRTV();
		framebuffers.push_back(rt);
	}

	// TAA pingping buffer
	ColorBuffer0 = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);
	ColorBuffer0->MakeRTV();

	NAME_D3D12_OBJECT(ColorBuffer0->resource);

	ColorBuffer1 = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);
	ColorBuffer1->MakeRTV();

	NAME_D3D12_OBJECT(ColorBuffer1->resource);

	ColorBuffers = { ColorBuffer0.get(), ColorBuffer1.get() };


	// lighting result
	LightingBuffer = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);
	LightingBuffer->MakeRTV();

	NAME_D3D12_OBJECT(LightingBuffer->resource);

	// world normal
	NormalBuffer = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);
	NormalBuffer->MakeRTV();

	NAME_D3D12_OBJECT(NormalBuffer->resource);

	// geometry world normal
	GeomNormalBuffer = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);
	GeomNormalBuffer->MakeRTV();

	NAME_D3D12_OBJECT(GeomNormalBuffer->resource);

	// shadow result
	ShadowBuffer = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R8G8B8A8_UNORM,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);

	NAME_D3D12_OBJECT(ShadowBuffer->resource);


	// refleciton result
	ReflectionBuffer = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);

	NAME_D3D12_OBJECT(ReflectionBuffer->resource);

	// gi result
	GIBuffer = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);

	NAME_D3D12_OBJECT(GIBuffer->resource);


	// gi result sh
	GIBufferSH = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);

	NAME_D3D12_OBJECT(GIBufferSH->resource);

	// gi result color
	GIBufferColor = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);

	NAME_D3D12_OBJECT(GIBufferColor->resource);


	// albedo
	AlbedoBuffer = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R8G8B8A8_UNORM,
		D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);
	AlbedoBuffer->MakeRTV();

	NAME_D3D12_OBJECT(AlbedoBuffer->resource);

	// velocity
	VelocityBuffer = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);
	VelocityBuffer->MakeRTV();
	NAME_D3D12_OBJECT(VelocityBuffer->resource);

	
	DepthBuffer = dx12_rhi->CreateTexture2D(DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);
	DepthBuffer->MakeDSV();
	NAME_D3D12_OBJECT(DepthBuffer->resource);

	LoadFbx();

	// Describe and create a sampler.
	D3D12_SAMPLER_DESC samplerDesc = {};
	samplerDesc.Filter = D3D12_FILTER_ANISOTROPIC;
	samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	samplerDesc.MinLOD = 0;
	samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
	samplerDesc.MipLODBias = -1.0f;
	samplerDesc.MaxAnisotropy = 1;
	samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;

	samplerWrap = dx12_rhi->CreateSampler(samplerDesc);

	InitRaytracing();
}

void dx12_framework::LoadMesh()
{
	//// Load scene assets.
	//UINT fileSize = 0;
	//UINT8* pAssetData;
	//ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(SampleAssetsIndoor::DataFileName).c_str(), &pAssetData, &fileSize));

	//mesh = make_unique<Mesh>();

	//// vertex buffer
	///*D3D12_SUBRESOURCE_DATA vertexData = {};
	//vertexData.pData = pAssetData + SampleAssetsIndoor::VertexDataOffset;
	//vertexData.RowPitch = SampleAssetsIndoor::VertexDataSize;
	//vertexData.SlicePitch = vertexData.RowPitch;*/

	//mesh->Vb = dx12_rhi->CreateVertexBuffer(SampleAssetsIndoor::VertexDataSize, SampleAssetsIndoor::StandardVertexStride, pAssetData + SampleAssetsIndoor::VertexDataOffset);
	//mesh->VertexStride = SampleAssetsIndoor::StandardVertexStride;
	//mesh->IndexFormat = SampleAssetsIndoor::StandardIndexFormat;


	//// index buffer
	///*D3D12_SUBRESOURCE_DATA indexData = {};
	//indexData.pData = pAssetData + SampleAssetsIndoor::IndexDataOffset;
	//indexData.RowPitch = SampleAssetsIndoor::IndexDataSize;
	//indexData.SlicePitch = indexData.RowPitch;*/
	//mesh->Ib = dx12_rhi->CreateIndexBuffer(SampleAssetsIndoor::StandardIndexFormat, SampleAssetsIndoor::IndexDataSize, pAssetData + SampleAssetsIndoor::IndexDataOffset);




	//// read textures
	//const UINT srvCount = _countof(SampleAssetsIndoor::Textures);

	//for (UINT i = 0; i < srvCount; i++)
	//{
	//	D3D12_SUBRESOURCE_DATA textureData = {};
	//	textureData.pData = pAssetData + SampleAssetsIndoor::Textures[i].Data[0].Offset;
	//	textureData.RowPitch = SampleAssetsIndoor::Textures[i].Data[0].Pitch;
	//	textureData.SlicePitch = SampleAssetsIndoor::Textures[i].Data[0].Size;

	//	//SquintRoom->Textures.push_back(dx12_rhi->CreateTexture2D(SampleAssetsIndoor::Textures[i].Format, SampleAssetsIndoor::Textures[i].Width, SampleAssetsIndoor::Textures[i].Height, SampleAssetsIndoor::Textures[i].MipLevels, &textureData));

	//	shared_ptr<Texture> tex = dx12_rhi->CreateTexture2D(SampleAssetsIndoor::Textures[i].Format, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, SampleAssetsIndoor::Textures[i].Width, SampleAssetsIndoor::Textures[i].Height, SampleAssetsIndoor::Textures[i].MipLevels);
	//	tex->MakeSRV();
	//	tex->UploadSRCData(&textureData);
	//	mesh->Textures.push_back(tex);
	//}

	//const UINT drawCount = _countof(SampleAssetsIndoor::Draws);

	//for (int i = 0; i < _countof(SampleAssetsIndoor::Draws); i++)
	//{
	//	SampleAssetsIndoor::DrawParameters drawArgs = SampleAssetsIndoor::Draws[i];
	//	Mesh::DrawCall drawcall;
	//	drawcall.DiffuseTextureIndex = drawArgs.DiffuseTextureIndex;
	//	drawcall.IndexCount = drawArgs.IndexCount;
	//	drawcall.IndexStart = drawArgs.IndexStart;
	//	drawcall.NormalTextureIndex = drawArgs.NormalTextureIndex;
	//	drawcall.SpecularTextureIndex = drawArgs.SpecularTextureIndex;
	//	drawcall.VertexBase = drawArgs.VertexBase;

	//	if (i > 0)
	//	{
	//		auto& prevDraw = mesh->Draws[i - 1];
	//		prevDraw.VertexCount = drawcall.VertexBase - prevDraw.VertexBase;
	//	}
	//	mesh->Draws.push_back(drawcall);
	//}
	//mesh->Draws[mesh->Draws.size() - 1].VertexCount = mesh->Vb->numVertices - mesh->Draws[mesh->Draws.size() - 1].VertexBase;
}

void dx12_framework::LoadFbx()
{
	Assimp::Importer importer;
	const aiScene* scene = importer.ReadFile("Sponza/Sponza.fbx", 0);
	wstring dir = L"Sponza/";

	UINT flags = aiProcess_CalcTangentSpace |
		aiProcess_Triangulate |
		aiProcess_JoinIdenticalVertices |
		aiProcess_MakeLeftHanded |
		aiProcess_RemoveRedundantMaterials |
		aiProcess_FlipUVs |
		aiProcess_FlipWindingOrder;

	/*if (settings.MergeMeshes)
		flags |= aiProcess_PreTransformVertices | aiProcess_OptimizeMeshes;*/

	scene = importer.ApplyPostProcessing(flags);

	const int numMaterials = scene->mNumMaterials;
	Materials.reserve(numMaterials);
	for (int i = 0; i < numMaterials; ++i)
	{
		const aiMaterial& aiMat = *scene->mMaterials[i];
		Material* mat = new Material;

		wstring wDiffuseTex;
		wstring wNormalTex;

		aiString diffuseTexPath;
		aiString normalMapPath;
		if (aiMat.GetTexture(aiTextureType_DIFFUSE, 0, &diffuseTexPath) == aiReturn_SUCCESS)
			wDiffuseTex = GetFileName(AnsiToWString(diffuseTexPath.C_Str()).c_str());
		if (wDiffuseTex.length() != 0)
		{
			shared_ptr<Texture> diffuseTex = dx12_rhi->CreateTextureFromFile(dir + wDiffuseTex, false);
			mat->Diffuse = diffuseTex;
		}


		if (aiMat.GetTexture(aiTextureType_NORMALS, 0, &normalMapPath) == aiReturn_SUCCESS
			|| aiMat.GetTexture(aiTextureType_HEIGHT, 0, &normalMapPath) == aiReturn_SUCCESS)
			wNormalTex = GetFileName(AnsiToWString(normalMapPath.C_Str()).c_str());

		if (wNormalTex.length() != 0)
		{
			shared_ptr<Texture> normalTex = dx12_rhi->CreateTextureFromFile(dir + wNormalTex, true);
			mat->Normal = normalTex;
		}
		
		Materials.push_back(shared_ptr<Material>(mat));


	}

	struct Vertex
	{
		glm::vec3 Position;
		glm::vec3 Normal;
		glm::vec2 UV;
		glm::vec3 Tangent;
	};
	const UINT numMeshes = scene->mNumMeshes;

	UINT totalNumVert = 0;
	for (UINT i = 0; i < numMeshes; ++i)
	{
		aiMesh* asMesh = scene->mMeshes[i];

		totalNumVert += asMesh->mNumVertices;
	}

	for (UINT i = 0; i < numMeshes; ++i)
	{
		aiMesh* asMesh = scene->mMeshes[i];

		Mesh* mesh = new Mesh;

		mesh->NumVertices = asMesh->mNumVertices;
		mesh->NumIndices = asMesh->mNumFaces * 3;

		vector<Vertex> vertices;
		vertices.resize(mesh->NumVertices);

		vector<UINT16> indices;
		indices.resize(mesh->NumIndices);
		//if (i > 0) break;

		if (asMesh->HasPositions())
		{
			for (int i = 0; i < mesh->NumVertices; ++i)
			{
				vertices[i].Position.x = asMesh->mVertices[i].x;
				vertices[i].Position.y = asMesh->mVertices[i].y;
				vertices[i].Position.z = asMesh->mVertices[i].z;

			}

		}

		if (asMesh->HasNormals())
		{
			for (int i = 0; i < mesh->NumVertices; ++i)
			{
				vertices[i].Normal.x = asMesh->mNormals[i].x;
				vertices[i].Normal.y = asMesh->mNormals[i].y;
				vertices[i].Normal.z = asMesh->mNormals[i].z;

			}
		}

		if (asMesh->HasTextureCoords(0))
		{
			for (int i = 0; i < mesh->NumVertices; ++i)
			{
				vertices[i].UV.x = asMesh->mTextureCoords[0][i].x;
				vertices[i].UV.y = asMesh->mTextureCoords[0][i].y;
			}
		}

		if (asMesh->HasTangentsAndBitangents())
		{
			for (int i = 0; i < mesh->NumVertices; ++i)
			{
				vertices[i].Tangent.x = asMesh->mTangents[i].x;
				vertices[i].Tangent.y = asMesh->mTangents[i].y;
				vertices[i].Tangent.z = asMesh->mTangents[i].z;
			}
		}

		const UINT numTriangles = asMesh->mNumFaces;
		for (int triIdx = 0; triIdx < numTriangles; ++triIdx)
		{
			indices[triIdx * 3 + 0] = UINT16(asMesh->mFaces[triIdx].mIndices[0]);
			indices[triIdx * 3 + 1] = UINT16(asMesh->mFaces[triIdx].mIndices[1]);
			indices[triIdx * 3 + 2] = UINT16(asMesh->mFaces[triIdx].mIndices[2]);
		}

		mesh->Vb = dx12_rhi->CreateVertexBuffer(sizeof(Vertex) * mesh->NumVertices, sizeof(Vertex), vertices.data());
		mesh->VertexStride = sizeof(Vertex);
		mesh->IndexFormat = DXGI_FORMAT_R16_UINT;

		mesh->Ib = dx12_rhi->CreateIndexBuffer(mesh->IndexFormat, sizeof(UINT16)*3*numTriangles, indices.data());


		Mesh::DrawCall dc;
		dc.IndexCount = numTriangles * 3;
		dc.IndexStart = 0;
		dc.VertexBase = 0;
		dc.VertexCount = vertices.size();
		dc.mat = Materials[asMesh->mMaterialIndex];
		
		mesh->Draws.push_back(dc);

		meshes.push_back(shared_ptr<Mesh>(mesh));
	}


	mesh = shared_ptr<Mesh>(new Mesh);
	const vec3 vertices[] =
	{
		vec3(0,          1,  0) * 100.f,
		vec3(0.866f,  -0.5f, 0) * 100.f,
		vec3(-0.866f, -0.5f, 0) * 100.f,
	};

	mesh->Vb = dx12_rhi->CreateVertexBuffer(sizeof(vec3) * 3, sizeof(vec3), (void*)vertices);
	mesh->VertexStride = sizeof(vec3);
	mesh->IndexFormat = DXGI_FORMAT_R16_UINT;

	const UINT16 indices[] =
	{
		0, 1, 2
	};

	mesh->Ib = dx12_rhi->CreateIndexBuffer(mesh->IndexFormat, sizeof(UINT16) * 3 * 1, (void*)indices);
}

void dx12_framework::InitComputeRS()
{
	ComPtr<ID3DBlob> computeShader;
#if defined(_DEBUG)
	// Enable better shader debugging with the graphics debugging tools.
	UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
	UINT compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

	ID3DBlob*compilationMsgs = nullptr;

	try
	{
		ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"Shaders\\SimpleCS.hlsl").c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_0", compileFlags, 0, &computeShader, &compilationMsgs));
	}
	catch (const std::exception& e)
	{
		OutputDebugStringA(reinterpret_cast<const char*>(compilationMsgs->GetBufferPointer()));
		compilationMsgs->Release();
	}

	Shader* cs = new Shader((UINT8*)computeShader->GetBufferPointer(), computeShader->GetBufferSize());

	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
	//computePsoDesc.pRootSignature = m_computeRootSignature.Get();

	RS_Compute = unique_ptr<PipelineStateObject>(new PipelineStateObject);
	RS_Compute->cs = unique_ptr<Shader>(cs);
	RS_Compute->computePSODesc = computePsoDesc;
	RS_Compute->BindUAV("output", 0);
	RS_Compute->IsCompute = true;
	RS_Compute->Init2();

	ComputeOuputTexture = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R8G8B8A8_UNORM,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, m_width, m_height, 1);

}

void dx12_framework::InitDenoiserPass()
{
	ComPtr<ID3DBlob> computeShader;
#if defined(_DEBUG)
	// Enable better shader debugging with the graphics debugging tools.
	UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
	UINT compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

	ID3DBlob*compilationMsgs = nullptr;

	try
	{
		ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"Shaders\\FilterIndirectDiffuseCS.hlsl").c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "FilterIndirectDiffuse", "cs_5_0", compileFlags, 0, &computeShader, &compilationMsgs));
	}
	catch (const std::exception& e)
	{
		OutputDebugStringA(reinterpret_cast<const char*>(compilationMsgs->GetBufferPointer()));
		compilationMsgs->Release();
	}

	Shader* cs = new Shader((UINT8*)computeShader->GetBufferPointer(), computeShader->GetBufferSize());

	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
	//computePsoDesc.pRootSignature = m_computeRootSignature.Get();

	RS_FilterIndirectDiffuse = unique_ptr<PipelineStateObject>(new PipelineStateObject);
	RS_FilterIndirectDiffuse->cs = unique_ptr<Shader>(cs);
	RS_FilterIndirectDiffuse->computePSODesc = computePsoDesc;
	RS_FilterIndirectDiffuse->BindTexture("DepthTex", 0, 1);
	RS_FilterIndirectDiffuse->BindTexture("GeoNormalTex", 1, 1);
	RS_FilterIndirectDiffuse->BindTexture("InGIResultSHTex", 2, 1);
	RS_FilterIndirectDiffuse->BindTexture("InGIResultColorTex", 3, 1);


	RS_FilterIndirectDiffuse->BindUAV("OutGIResultSH", 0);
	RS_FilterIndirectDiffuse->BindUAV("OutGIResultColor", 1);

	RS_FilterIndirectDiffuse->BindConstantBuffer("FilterIndirectDiffuseConstant", 0, sizeof(FilterIndirectDiffuseConstant), 1);
	RS_FilterIndirectDiffuse->IsCompute = true;
	RS_FilterIndirectDiffuse->Init2();

	FilterIndirectDiffusePingPong[0] = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);

	NAME_D3D12_OBJECT(FilterIndirectDiffusePingPong[0]->resource);

	FilterIndirectDiffusePingPong[1] = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);

	NAME_D3D12_OBJECT(FilterIndirectDiffusePingPong[1]->resource);


	FilterIndirectDiffusePingPongSH[0] = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);

	NAME_D3D12_OBJECT(FilterIndirectDiffusePingPongSH[0]->resource);

	FilterIndirectDiffusePingPongSH[1] = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);

	NAME_D3D12_OBJECT(FilterIndirectDiffusePingPongSH[1]->resource);

	FilterIndirectDiffusePingPongColor[0] = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);

	NAME_D3D12_OBJECT(FilterIndirectDiffusePingPongColor[0]->resource);

	FilterIndirectDiffusePingPongColor[1] = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);

	NAME_D3D12_OBJECT(FilterIndirectDiffusePingPongColor[1]->resource);

	
}

void dx12_framework::InitDrawMeshRS()
{
	ComPtr<ID3DBlob> vertexShader;
	ComPtr<ID3DBlob> pixelShader;


#if defined(_DEBUG)
	// Enable better shader debugging with the graphics debugging tools.
	UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
	UINT compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

	ID3DBlob*compilationMsgs = nullptr;

	try
	{
		ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"Shaders\\MeshDraw.hlsl").c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VSMain", "vs_5_0", compileFlags, 0, &vertexShader, &compilationMsgs));
	}
	catch (const std::exception& e)
	{
		OutputDebugStringA(reinterpret_cast<const char*>(compilationMsgs->GetBufferPointer()));
		compilationMsgs->Release();
	}

	try
	{
		ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"Shaders\\MeshDraw.hlsl").c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PSMain", "ps_5_0", compileFlags, 0, &pixelShader, &compilationMsgs));
	}
	catch (const std::exception& e)
	{
		OutputDebugStringA(reinterpret_cast<const char*>(compilationMsgs->GetBufferPointer()));
		compilationMsgs->Release();
	}

	Shader* ps = new Shader((UINT8*)pixelShader->GetBufferPointer(), pixelShader->GetBufferSize());
	//ps->BindTexture("diffuseMap", 0, 1);
	//ps->BindTexture("normalMap", 1, 1);

	//ps->BindSampler("samplerWrap", 0);

	Shader* vs = new Shader((UINT8*)vertexShader->GetBufferPointer(), vertexShader->GetBufferSize());
	//vs->BindConstantBuffer("ObjParameter", 0, sizeof(ObjConstantBuffer), 400);
	//ps->BindConstantBuffer("ObjParameter", 0, sizeof(ObjConstantBuffer), 400);


	CD3DX12_RASTERIZER_DESC rasterizerStateDesc(D3D12_DEFAULT);
	rasterizerStateDesc.CullMode = D3D12_CULL_MODE_NONE;

	const D3D12_INPUT_ELEMENT_DESC StandardVertexDescription[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};

	UINT StandardVertexDescriptionNumElements = _countof(StandardVertexDescription);

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDescMesh = {};
	psoDescMesh.InputLayout = { StandardVertexDescription, StandardVertexDescriptionNumElements };
	//psoDesc.pRootSignature = m_rootSignature.Get();
	/*psoDesc.VS = CD3DX12_SHADER_BYTECODE(pVertexShaderData, vertexShaderDataLength);
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(pPixelShaderData, pixelShaderDataLength);*/
	psoDescMesh.RasterizerState = rasterizerStateDesc;
	psoDescMesh.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDescMesh.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDescMesh.SampleMask = UINT_MAX;
	psoDescMesh.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDescMesh.NumRenderTargets = 4;
	psoDescMesh.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	psoDescMesh.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	psoDescMesh.RTVFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	psoDescMesh.RTVFormats[3] = DXGI_FORMAT_R16G16_FLOAT;


	psoDescMesh.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDescMesh.SampleDesc.Count = 1;

	RS_Mesh = unique_ptr<PipelineStateObject>(new PipelineStateObject);
	RS_Mesh->ps = shared_ptr<Shader>(ps);
	RS_Mesh->vs = shared_ptr<Shader>(vs);
	RS_Mesh->graphicsPSODesc = psoDescMesh;
	RS_Mesh->BindTexture("diffuseMap", 0, 1);
	RS_Mesh->BindTexture("normalMap", 1, 1);
	RS_Mesh->BindSampler("samplerWrap", 0);
	RS_Mesh->BindConstantBuffer("ObjParameter", 0, sizeof(ObjConstantBuffer), 400);

	RS_Mesh->Init2();
}

void dx12_framework::InitImgui()
{
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO(); (void)io;

	dx12_rhi->TextureDHRing->AllocDescriptor(CpuHandleImguiFontTex, GpuHandleImguiFontTex);

	ImGui_ImplWin32_Init(Win32Application::GetHwnd());
	ImGui_ImplDX12_Init(dx12_rhi->Device.Get(), dx12_rhi->NumFrame,
		DXGI_FORMAT_R8G8B8A8_UNORM, dx12_rhi->SRVCBVDescriptorHeapShaderVisible->DH.Get(),
		CpuHandleImguiFontTex,
		GpuHandleImguiFontTex);
}

void dx12_framework::InitBlueNoiseTexture()
{
	string path = "bluenoise/64_64_64/HDR_RGBA.raw";
	ifstream file(path.data(), ios::in | ios::binary);
	if (file.is_open())
	{
		file.seekg(0, file.end);
		int length = file.tellg();
		file.seekg(0, file.beg);

		UINT32 Version;
		file.read(reinterpret_cast<char*>(&Version), sizeof(UINT32));

		UINT32 nChannel;
		file.read(reinterpret_cast<char*>(&nChannel), sizeof(UINT32));

		UINT32 nDimension;
		file.read(reinterpret_cast<char*>(&nDimension), sizeof(UINT32));

		UINT32 Shape[3];
		for(int i=0;i< nDimension;i++)
			file.read(reinterpret_cast<char*>(&Shape[i]), sizeof(UINT32));

		UINT DataSize = sizeof(UINT32) * nChannel * Shape[0] * Shape[1] * Shape[2];
		UINT32* NoiseDataRaw = new UINT32[DataSize];

		file.read(reinterpret_cast<char*>(NoiseDataRaw), DataSize);
		size_t extracted = file.gcount();
		/*UINT32 NoiseData[128];
		file.read(reinterpret_cast<char*>(NoiseData), 128*sizeof(UINT32));*/
		int NumFloat = nChannel * Shape[0] * Shape[1] * Shape[2];

		float* NoiseDataFloat = new float[NumFloat];
		stringstream ss;

		float MaxValue = Shape[0] * Shape[1] * Shape[2];
		for (int i = 0; i < NumFloat; i++)
		{
			if (NoiseDataRaw[i] == 3452816845)
			{
				int a = 0;
			}
			NoiseDataFloat[i] = static_cast<float>(NoiseDataRaw[i]) / MaxValue;

			ss << NoiseDataFloat[i] << " ";
			if(i %(64*4) == 0)
				ss << "\n";
		}
		OutputDebugStringA(ss.str().c_str());

		D3D12_SUBRESOURCE_DATA textureData = {};
		textureData.pData = NoiseDataFloat;
		textureData.RowPitch = Shape[0] * nChannel * sizeof(UINT32);
		textureData.SlicePitch = textureData.RowPitch * Shape[1];

		BlueNoiseTex = dx12_rhi->CreateTexture3D(DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_FLAG_NONE,
			D3D12_RESOURCE_STATE_COPY_DEST, Shape[0], Shape[1], Shape[2], 1);
		BlueNoiseTex->UploadSRCData3D(&textureData);

		delete[] NoiseDataRaw;
		delete[] NoiseDataFloat;

	}
	
	file.close();
}

void dx12_framework::InitCopyPass()
{
	struct PostVertex
	{
		XMFLOAT4 position;
		XMFLOAT2 uv;
	};

	PostVertex quadVertices[] =
	{
		{ { -1.0f, -1.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },    // Bottom left.
		{ { -1.0f, 1.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } },    // Top left.
		{ { 1.0f, -1.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },    // Bottom right.
		{ { 1.0f, 1.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } }        // Top right.
	};

	const UINT vertexBufferSize = sizeof(quadVertices);
	const UINT vertexBufferStride = sizeof(PostVertex);

	D3D12_SUBRESOURCE_DATA vertexData = {};
	vertexData.pData = &quadVertices;
	vertexData.RowPitch = vertexBufferSize;
	vertexData.SlicePitch = vertexData.RowPitch;

	FullScreenVB = dx12_rhi->CreateVertexBuffer(vertexBufferSize, vertexBufferStride, &quadVertices);

	///
	ComPtr<ID3DBlob> vertexShader;
	ComPtr<ID3DBlob> pixelShader;


#if defined(_DEBUG)
	// Enable better shader debugging with the graphics debugging tools.
	UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
	UINT compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

	ID3DBlob*compilationMsgs = nullptr;

	try
	{
		ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"Shaders\\CopyPS.hlsl").c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VSMain", "vs_5_0", compileFlags, 0, &vertexShader, &compilationMsgs));
	}
	catch (const std::exception& e)
	{
		OutputDebugStringA(reinterpret_cast<const char*>(compilationMsgs->GetBufferPointer()));
		compilationMsgs->Release();
	}

	try
	{
		ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"Shaders\\CopyPS.hlsl").c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PSMain", "ps_5_0", compileFlags, 0, &pixelShader, &compilationMsgs));
	}
	catch (const std::exception& e)
	{
		OutputDebugStringA(reinterpret_cast<const char*>(compilationMsgs->GetBufferPointer()));
		compilationMsgs->Release();
	}

	Shader* ps = new Shader((UINT8*)pixelShader->GetBufferPointer(), pixelShader->GetBufferSize());
	/*ps->BindTexture("SrcTex", 0, 1);
	ps->BindSampler("samplerWrap", 0);*/

	Shader* vs = new Shader((UINT8*)vertexShader->GetBufferPointer(), vertexShader->GetBufferSize());
	//vs->BindConstantBuffer("ScaleOffsetParams", 0, sizeof(CopyScaleOffsetCB), 12);


	CD3DX12_RASTERIZER_DESC rasterizerStateDesc(D3D12_DEFAULT);
	rasterizerStateDesc.CullMode = D3D12_CULL_MODE_NONE;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};

	const D3D12_INPUT_ELEMENT_DESC StandardVertexDescription[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};
	UINT StandardVertexDescriptionNumElements = _countof(StandardVertexDescription);

	psoDesc.InputLayout = { StandardVertexDescription, StandardVertexDescriptionNumElements };
	//psoDesc.pRootSignature = m_rootSignature.Get();
	/*psoDesc.VS = CD3DX12_SHADER_BYTECODE(pVertexShaderData, vertexShaderDataLength);
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(pPixelShaderData, pixelShaderDataLength);*/
	psoDesc.RasterizerState = rasterizerStateDesc;
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState.DepthEnable = FALSE;
	psoDesc.DepthStencilState.StencilEnable = FALSE;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	//psoDescMesh.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleDesc.Count = 1;

	RS_Copy = unique_ptr<PipelineStateObject>(new PipelineStateObject);
	RS_Copy->ps = shared_ptr<Shader>(ps);
	RS_Copy->vs = shared_ptr<Shader>(vs);
	RS_Copy->graphicsPSODesc = psoDesc;

	RS_Copy->BindTexture("SrcTex", 0, 1);
	RS_Copy->BindSampler("samplerWrap", 0);
	RS_Copy->BindConstantBuffer("ScaleOffsetParams", 0, sizeof(CopyScaleOffsetCB), 15);

	RS_Copy->Init2();
}

void dx12_framework::InitLightingPass()
{
	ComPtr<ID3DBlob> vertexShader;
	ComPtr<ID3DBlob> pixelShader;


#if defined(_DEBUG)
	// Enable better shader debugging with the graphics debugging tools.
	UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
	UINT compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

	ID3DBlob*compilationMsgs = nullptr;

	try
	{
		ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"Shaders\\LightingPS.hlsl").c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VSMain", "vs_5_0", compileFlags, 0, &vertexShader, &compilationMsgs));
	}
	catch (const std::exception& e)
	{
		OutputDebugStringA(reinterpret_cast<const char*>(compilationMsgs->GetBufferPointer()));
		compilationMsgs->Release();
	}

	try
	{
		ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"Shaders\\LightingPS.hlsl").c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PSMain", "ps_5_0", compileFlags, 0, &pixelShader, &compilationMsgs));
	}
	catch (const std::exception& e)
	{
		OutputDebugStringA(reinterpret_cast<const char*>(compilationMsgs->GetBufferPointer()));
		compilationMsgs->Release();
	}

	Shader* ps = new Shader((UINT8*)pixelShader->GetBufferPointer(), pixelShader->GetBufferSize());
	//ps->BindTexture("AlbedoTex", 0, 1);
	//ps->BindTexture("NormalTex", 1, 1);
	//ps->BindTexture("ShadowTex", 2, 1);
	//ps->BindTexture("PrevColorTex", 3, 1);
	//ps->BindTexture("VelocityTex", 4, 1);


	//ps->BindSampler("samplerWrap", 0);
	//ps->BindConstantBuffer("LightingParam", 0, sizeof(LightingParam));

	Shader* vs = new Shader((UINT8*)vertexShader->GetBufferPointer(), vertexShader->GetBufferSize());

	CD3DX12_RASTERIZER_DESC rasterizerStateDesc(D3D12_DEFAULT);
	rasterizerStateDesc.CullMode = D3D12_CULL_MODE_NONE;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};

	const D3D12_INPUT_ELEMENT_DESC StandardVertexDescription[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};
	UINT StandardVertexDescriptionNumElements = _countof(StandardVertexDescription);

	psoDesc.InputLayout = { StandardVertexDescription, StandardVertexDescriptionNumElements };
	//psoDesc.pRootSignature = m_rootSignature.Get();
	/*psoDesc.VS = CD3DX12_SHADER_BYTECODE(pVertexShaderData, vertexShaderDataLength);
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(pPixelShaderData, pixelShaderDataLength);*/
	psoDesc.RasterizerState = rasterizerStateDesc;
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState.DepthEnable = FALSE;
	psoDesc.DepthStencilState.StencilEnable = FALSE;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	//psoDescMesh.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleDesc.Count = 1;

	RS_Lighting = unique_ptr<PipelineStateObject>(new PipelineStateObject);
	RS_Lighting->ps = shared_ptr<Shader>(ps);
	RS_Lighting->vs = shared_ptr<Shader>(vs);
	RS_Lighting->graphicsPSODesc = psoDesc;

	RS_Lighting->BindTexture("AlbedoTex", 0, 1);
	RS_Lighting->BindTexture("NormalTex", 1, 1);
	RS_Lighting->BindTexture("ShadowTex", 2, 1);
	RS_Lighting->BindTexture("VelocityTex", 3, 1);
	RS_Lighting->BindTexture("DepthTex", 4, 1);
	//RS_Lighting->BindTexture("IndirectDiffuseTex", 5, 1);
	RS_Lighting->BindTexture("GIResultSHTex", 5, 1);
	RS_Lighting->BindTexture("GIResultColorTex", 6, 1);




	RS_Lighting->BindSampler("samplerWrap", 0);
	RS_Lighting->BindConstantBuffer("LightingParam", 0, sizeof(LightingParam));
	RS_Lighting->Init2();
}

void dx12_framework::InitTemporalAAPass()
{
	ComPtr<ID3DBlob> vertexShader;
	ComPtr<ID3DBlob> pixelShader;


#if defined(_DEBUG)
	// Enable better shader debugging with the graphics debugging tools.
	UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
	UINT compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

	ID3DBlob*compilationMsgs = nullptr;

	try
	{
		ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"Shaders\\TemporalAA.hlsl").c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VSMain", "vs_5_0", compileFlags, 0, &vertexShader, &compilationMsgs));
	}
	catch (const std::exception& e)
	{
		OutputDebugStringA(reinterpret_cast<const char*>(compilationMsgs->GetBufferPointer()));
		compilationMsgs->Release();
	}

	try
	{
		ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"Shaders\\TemporalAA.hlsl").c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PSMain", "ps_5_0", compileFlags, 0, &pixelShader, &compilationMsgs));
	}
	catch (const std::exception& e)
	{
		OutputDebugStringA(reinterpret_cast<const char*>(compilationMsgs->GetBufferPointer()));
		compilationMsgs->Release();
	}

	Shader* ps = new Shader((UINT8*)pixelShader->GetBufferPointer(), pixelShader->GetBufferSize());
	

	Shader* vs = new Shader((UINT8*)vertexShader->GetBufferPointer(), vertexShader->GetBufferSize());

	CD3DX12_RASTERIZER_DESC rasterizerStateDesc(D3D12_DEFAULT);
	rasterizerStateDesc.CullMode = D3D12_CULL_MODE_NONE;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};

	const D3D12_INPUT_ELEMENT_DESC StandardVertexDescription[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};
	UINT StandardVertexDescriptionNumElements = _countof(StandardVertexDescription);

	psoDesc.InputLayout = { StandardVertexDescription, StandardVertexDescriptionNumElements };
	//psoDesc.pRootSignature = m_rootSignature.Get();
	/*psoDesc.VS = CD3DX12_SHADER_BYTECODE(pVertexShaderData, vertexShaderDataLength);
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(pPixelShaderData, pixelShaderDataLength);*/
	psoDesc.RasterizerState = rasterizerStateDesc;
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState.DepthEnable = FALSE;
	psoDesc.DepthStencilState.StencilEnable = FALSE;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	//psoDescMesh.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleDesc.Count = 1;

	RS_TemporalAA = unique_ptr<PipelineStateObject>(new PipelineStateObject);
	RS_TemporalAA->ps = shared_ptr<Shader>(ps);
	RS_TemporalAA->vs = shared_ptr<Shader>(vs);
	RS_TemporalAA->graphicsPSODesc = psoDesc;

	RS_TemporalAA->BindTexture("CurrentColorTex", 0, 1);
	RS_TemporalAA->BindTexture("PrevColorTex", 1, 1);
	RS_TemporalAA->BindTexture("VelocityTex", 2, 1);
	RS_TemporalAA->BindTexture("DepthTex", 3, 1);

	RS_TemporalAA->BindSampler("samplerWrap", 0);
	RS_TemporalAA->BindConstantBuffer("LightingParam", 0, sizeof(LightingParam));
	RS_TemporalAA->Init2();
}

void dx12_framework::CopyPass()
{
	/*PIXBeginEvent*/(dx12_rhi->CommandList.Get(), 0, L"CopyPass");

	Texture* backbuffer = framebuffers[dx12_rhi->CurrentFrameIndex].get();
	Texture* ResolveTarget = ColorBuffers[ColorBufferWriteIndex];//framebuffers[dx12_rhi->CurrentFrameIndex].get();

	

	/*const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
	dx12_rhi->CommandList->ClearRenderTargetView(backbuffer->CpuHandleRTV, clearColor, 0, nullptr);
	*/
	RS_Copy->currentDrawCallIndex = 0;
	RS_Copy->Apply(dx12_rhi->CommandList.Get());


	RS_Copy->SetSampler("samplerWrap", samplerWrap.get(), dx12_rhi->CommandList.Get());
	RS_Copy->SetTexture("SrcTex", ResolveTarget, dx12_rhi->CommandList.Get());

	CopyScaleOffsetCB cb;
	cb.Offset = glm::vec4(0, 0, 0, 0);
	cb.Scale = glm::vec4(1, 1, 0, 0);
	RS_Copy->SetConstantValue("ScaleOffsetParams", &cb, dx12_rhi->CommandList.Get());


	RS_Copy->Apply(dx12_rhi->CommandList.Get());


	UINT Width = m_width / 1;
	UINT Height = m_height / 1;
	CD3DX12_VIEWPORT viewport(0.0f, 0.0f, static_cast<float>(Width), static_cast<float>(Height));
	CD3DX12_RECT scissorRect(0, 0, static_cast<LONG>(Width), static_cast<LONG>(Height));


	dx12_rhi->CommandList->RSSetViewports(1, &viewport);


	dx12_rhi->CommandList->RSSetScissorRects(1, &scissorRect);

	dx12_rhi->CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	dx12_rhi->CommandList->IASetVertexBuffers(0, 1, &FullScreenVB->view);
	

	dx12_rhi->CommandList->DrawInstanced(4, 1, 0, 0);

	RS_Copy->currentDrawCallIndex++;

}

void dx12_framework::DebugPass()
{
	//RS_Copy->currentDrawCallIndex = 0;

	Texture* backbuffer = framebuffers[dx12_rhi->CurrentFrameIndex].get();

	RS_Copy->Apply(dx12_rhi->CommandList.Get());
	dx12_rhi->CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	dx12_rhi->CommandList->IASetVertexBuffers(0, 1, &FullScreenVB->view);

	RS_Copy->SetSampler("samplerWrap", samplerWrap.get(), dx12_rhi->CommandList.Get());
	CopyScaleOffsetCB cb;

	 //raytraced shadow
	RS_Copy->SetTexture("SrcTex", ShadowBuffer.get(), dx12_rhi->CommandList.Get());
	cb.Offset = glm::vec4(-0.75, -0.75, 0, 0);
	cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
	RS_Copy->SetConstantValue("ScaleOffsetParams", &cb, dx12_rhi->CommandList.Get());
	dx12_rhi->CommandList->DrawInstanced(4, 1, 0, 0);
	RS_Copy->currentDrawCallIndex++;


	// raytrace reflection
	RS_Copy->SetTexture("SrcTex", ReflectionBuffer.get(), dx12_rhi->CommandList.Get());
	cb.Offset = glm::vec4(-0.75, -0.25, 0, 0);
	cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
	RS_Copy->SetConstantValue("ScaleOffsetParams", &cb, dx12_rhi->CommandList.Get());
	dx12_rhi->CommandList->DrawInstanced(4, 1, 0, 0);
	RS_Copy->currentDrawCallIndex++;


	// world normal
	RS_Copy->SetTexture("SrcTex", NormalBuffer.get(), dx12_rhi->CommandList.Get());
	cb.Offset = glm::vec4(-0.25, -0.75, 0, 0);
	cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
	RS_Copy->SetConstantValue("ScaleOffsetParams", &cb, dx12_rhi->CommandList.Get());
	dx12_rhi->CommandList->DrawInstanced(4, 1, 0, 0);
	RS_Copy->currentDrawCallIndex++;


	// geom world normal
	cb.Offset = glm::vec4(-0.25, -0.25, 0, 0);
	cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
	RS_Copy->SetConstantValue("ScaleOffsetParams", &cb, dx12_rhi->CommandList.Get());
	RS_Copy->SetTexture("SrcTex", GeomNormalBuffer.get(), dx12_rhi->CommandList.Get());
	dx12_rhi->CommandList->DrawInstanced(4, 1, 0, 0);
	RS_Copy->currentDrawCallIndex++;

	// depth
	cb.Offset = glm::vec4(0.25, -0.75, 0, 0);
	cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
	RS_Copy->SetConstantValue("ScaleOffsetParams", &cb, dx12_rhi->CommandList.Get());
	RS_Copy->SetTexture("SrcTex", DepthBuffer.get(), dx12_rhi->CommandList.Get());
	dx12_rhi->CommandList->DrawInstanced(4, 1, 0, 0);
	RS_Copy->currentDrawCallIndex++;

	// gi
	cb.Offset = glm::vec4(0.25, -0.25, 0, 0);
	cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
	RS_Copy->SetConstantValue("ScaleOffsetParams", &cb, dx12_rhi->CommandList.Get());
	//RS_Copy->SetTexture("SrcTex", GIBuffer.get(), dx12_rhi->CommandList.Get());
	RS_Copy->SetTexture("SrcTex", GIBufferSH.get(), dx12_rhi->CommandList.Get());

	dx12_rhi->CommandList->DrawInstanced(4, 1, 0, 0);
	RS_Copy->currentDrawCallIndex++;

	// filtered gi
	cb.Offset = glm::vec4(0.25, 0.25, 0, 0);
	cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
	RS_Copy->SetConstantValue("ScaleOffsetParams", &cb, dx12_rhi->CommandList.Get());
	//RS_Copy->SetTexture("SrcTex", GIBuffer.get(), dx12_rhi->CommandList.Get());
	RS_Copy->SetTexture("SrcTex", FilterIndirectDiffusePingPongSH[1].get(), dx12_rhi->CommandList.Get());

	dx12_rhi->CommandList->DrawInstanced(4, 1, 0, 0);
	RS_Copy->currentDrawCallIndex++;

	// albedo

	cb.Offset = glm::vec4(0.750, -0.75, 0, 0);
	cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
	RS_Copy->SetConstantValue("ScaleOffsetParams", &cb, dx12_rhi->CommandList.Get());
	RS_Copy->SetTexture("SrcTex", AlbedoBuffer.get(), dx12_rhi->CommandList.Get());
	dx12_rhi->CommandList->DrawInstanced(4, 1, 0, 0);
	RS_Copy->currentDrawCallIndex++;

	// velocity
	cb.Offset = glm::vec4(0.75, -0.25, 0, 0);
	cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
	RS_Copy->SetConstantValue("ScaleOffsetParams", &cb, dx12_rhi->CommandList.Get());
	RS_Copy->SetTexture("SrcTex", VelocityBuffer.get(), dx12_rhi->CommandList.Get());
	dx12_rhi->CommandList->DrawInstanced(4, 1, 0, 0);
	RS_Copy->currentDrawCallIndex++;

	RS_Copy->Apply(dx12_rhi->CommandList.Get());
}

void dx12_framework::LightingPass()
{

	dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(LightingBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));


	RS_Lighting->Apply(dx12_rhi->CommandList.Get());


	RS_Lighting->SetSampler("samplerWrap", samplerWrap.get(), dx12_rhi->CommandList.Get());
	RS_Lighting->SetTexture("AlbedoTex", AlbedoBuffer.get(), dx12_rhi->CommandList.Get());
	RS_Lighting->SetTexture("NormalTex", NormalBuffer.get(), dx12_rhi->CommandList.Get());
	RS_Lighting->SetTexture("ShadowTex", ShadowBuffer.get(), dx12_rhi->CommandList.Get());

	RS_Lighting->SetTexture("VelocityTex", VelocityBuffer.get(), dx12_rhi->CommandList.Get());
	RS_Lighting->SetTexture("DepthTex", DepthBuffer.get(), dx12_rhi->CommandList.Get());
	//RS_Lighting->SetTexture("IndirectDiffuseTex", FilterIndirectDiffusePingPong[1].get(), dx12_rhi->CommandList.Get());
	RS_Lighting->SetTexture("GIResultSHTex", FilterIndirectDiffusePingPongSH[1].get(), dx12_rhi->CommandList.Get());
	RS_Lighting->SetTexture("GIResultColorTex", FilterIndirectDiffusePingPongColor[1].get(), dx12_rhi->CommandList.Get());


	LightingParam Param;
	Param.LightDir = glm::vec4(glm::normalize(LightDir), LightIntensity);
	
	Param.RTSize.x = m_width;
	Param.RTSize.y = m_height;

	if(bEnableTAA)
		Param.TAABlendFactor = 0.1;
	else
		Param.TAABlendFactor = 1.0;


	glm::normalize(Param.LightDir);
	RS_Lighting->SetConstantValue("LightingParam", &Param, dx12_rhi->CommandList.Get());


	RS_Lighting->Apply(dx12_rhi->CommandList.Get());

	dx12_rhi->CommandList->OMSetRenderTargets(1, &LightingBuffer->CpuHandleRTV, FALSE, nullptr);


	dx12_rhi->CommandList->RSSetViewports(1, &m_viewport);
	dx12_rhi->CommandList->RSSetScissorRects(1, &m_scissorRect);

	dx12_rhi->CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	dx12_rhi->CommandList->IASetVertexBuffers(0, 1, &FullScreenVB->view);


	dx12_rhi->CommandList->DrawInstanced(4, 1, 0, 0);


	dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(LightingBuffer->resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

}

void dx12_framework::TemporalAAPass()
{
	UINT PrevColorBufferIndex = 1 - ColorBufferWriteIndex;
	Texture* ResolveTarget = ColorBuffers[ColorBufferWriteIndex];//framebuffers[dx12_rhi->CurrentFrameIndex].get();

	dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(ResolveTarget->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));


	RS_TemporalAA->Apply(dx12_rhi->CommandList.Get());


	RS_TemporalAA->SetSampler("samplerWrap", samplerWrap.get(), dx12_rhi->CommandList.Get());
	
	RS_TemporalAA->SetTexture("CurrentColorTex", LightingBuffer.get(), dx12_rhi->CommandList.Get());
	
	Texture* PrevColorBuffer = ColorBuffers[PrevColorBufferIndex];
	RS_TemporalAA->SetTexture("PrevColorTex", PrevColorBuffer, dx12_rhi->CommandList.Get());
	
	RS_TemporalAA->SetTexture("VelocityTex", VelocityBuffer.get(), dx12_rhi->CommandList.Get());
	
	RS_TemporalAA->SetTexture("DepthTex", DepthBuffer.get(), dx12_rhi->CommandList.Get());



	TemporalAAParam Param;

	Param.RTSize.x = m_width;
	Param.RTSize.y = m_height;

	if (bEnableTAA)
		Param.TAABlendFactor = 0.1;
	else
		Param.TAABlendFactor = 1.0;

	Param.ClampMode = ClampMode;

	RS_TemporalAA->SetConstantValue("LightingParam", &Param, dx12_rhi->CommandList.Get());


	RS_TemporalAA->Apply(dx12_rhi->CommandList.Get());


	dx12_rhi->CommandList->OMSetRenderTargets(1, &ResolveTarget->CpuHandleRTV, FALSE, nullptr);


	dx12_rhi->CommandList->RSSetViewports(1, &m_viewport);
	dx12_rhi->CommandList->RSSetScissorRects(1, &m_scissorRect);

	dx12_rhi->CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	dx12_rhi->CommandList->IASetVertexBuffers(0, 1, &FullScreenVB->view);


	dx12_rhi->CommandList->DrawInstanced(4, 1, 0, 0);


	dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(ResolveTarget->resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
}

static const float OneMinusEpsilon = 0.9999999403953552f;

inline float RadicalInverseBase2(uint32 bits)
{
	bits = (bits << 16u) | (bits >> 16u);
	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
	bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
	bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
	return float(bits) * 2.3283064365386963e-10f; // / 0x100000000
}
inline glm::vec2 Hammersley2D(uint64 sampleIdx, uint64 numSamples)
{
	return glm::vec2(float(sampleIdx) / float(numSamples), RadicalInverseBase2(uint32(sampleIdx)));
}


// Update frame-based values.
void dx12_framework::OnUpdate()
{
	m_timer.Tick(NULL);

	if (m_frameCounter == 100)
	{
		// Update window text with FPS value.
		wchar_t fps[64];
		swprintf_s(fps, L"%ufps", m_timer.GetFramesPerSecond());
		SetCustomWindowText(fps);
		m_frameCounter = 0;
	}

	m_frameCounter++;

	m_camera.Update(static_cast<float>(m_timer.GetElapsedSeconds()));

	ViewMat = m_camera.GetViewMatrix();
	ProjMat = m_camera.GetProjectionMatrix(0.8f, m_aspectRatio, Near, Far);

	glm::mat4x4 InvViewMat = glm::inverse(ViewMat);
	RTShadowViewParam.ViewMatrix = glm::transpose(ViewMat);
	RTShadowViewParam.InvViewMatrix = glm::transpose(InvViewMat);
	RTShadowViewParam.ProjMatrix = glm::transpose(ProjMat);
	RTShadowViewParam.ProjectionParams.x = Far / (Far - Near);
	RTShadowViewParam.ProjectionParams.y = Near / (Near - Far);
	RTShadowViewParam.ProjectionParams.z = Near;
	RTShadowViewParam.ProjectionParams.w = Far;


	RTShadowViewParam.LightDir = glm::vec4(LightDir, 0);

	glm::vec2 Jitter;
	uint64 idx = FrameCounter % 4;
	Jitter = Hammersley2D(idx, 4) * 2.0f - glm::vec2(1.0f);
	Jitter *= JitterScale;

	const float offsetX = Jitter.x * (1.0f / m_width);
	const float offsetY = Jitter.y * (1.0f / m_height);

	if (bEnableTAA)
		JitterOffset = (Jitter - PrevJitter) * 0.5f;
	else
		JitterOffset = glm::vec2(0, 0);
	
	PrevJitter = Jitter;
	glm::mat4x4 JitterMat = glm::translate(glm::vec3(offsetX, -offsetY, 0));
	
	UnjitteredViewProjMat = ProjMat * ViewMat;

	if(bEnableTAA)
		ProjMat = JitterMat * ProjMat;


	ViewProjMat = ProjMat * ViewMat;

	InvViewProjMat = glm::inverse(ViewProjMat);



	// reflection view param
	RTReflectionViewParam.ViewMatrix = glm::transpose(ViewMat);
	RTReflectionViewParam.InvViewMatrix = glm::transpose(InvViewMat);
	RTReflectionViewParam.ProjMatrix = glm::transpose(ProjMat);
	RTReflectionViewParam.ProjectionParams.x = Far / (Far - Near);
	RTReflectionViewParam.ProjectionParams.y = Near / (Near - Far);
	RTReflectionViewParam.ProjectionParams.z = Near;
	RTReflectionViewParam.ProjectionParams.w = Far;

	RTReflectionViewParam.LightDir = glm::vec4(LightDir, 0);

	// GI view param
	RTGIViewParam.ViewMatrix = glm::transpose(ViewMat);
	RTGIViewParam.InvViewMatrix = glm::transpose(InvViewMat);
	RTGIViewParam.ProjMatrix = glm::transpose(ProjMat);
	RTGIViewParam.ProjectionParams.x = Far / (Far - Near);
	RTGIViewParam.ProjectionParams.y = Near / (Near - Far);
	RTGIViewParam.ProjectionParams.z = Near;
	RTGIViewParam.ProjectionParams.w = Far;
	RTGIViewParam.LightDir = glm::vec4(glm::normalize(LightDir), LightIntensity);
	float timeElapsed = m_timer.GetTotalSeconds();
	timeElapsed *= 0.01f;
	RTGIViewParam.RandomOffset = glm::vec2(timeElapsed, timeElapsed);
	RTGIViewParam.FrameCounter = FrameCounter;
	
	FilterIndirectDiffuseConstantCB.ProjectionParams.x = Near;
	FilterIndirectDiffuseConstantCB.ProjectionParams.y = Far;


	
	FrameCounter++;



	ColorBufferWriteIndex = FrameCounter % 2;
}

// Render the scene.
void dx12_framework::OnRender()
{

	dx12_rhi->BeginFrame();
	
	// Record all the commands we need to render the scene into the command list.
	Texture* backbuffer = framebuffers[dx12_rhi->CurrentFrameIndex].get();
	//NVAftermathMarker(dx12_rhi->AM_CL_Handle, "DrawMeshPass");

	DrawMeshPass();


	//NVAftermathMarker(dx12_rhi->AM_CL_Handle, "RaytracePass");

	RaytraceShadowPass();

	RaytraceReflectionPass();

	RaytraceGIPass();

	FilterIndirectDiffusePass();
	//ComputePass();

	//NVAftermathMarker(dx12_rhi->AM_CL_Handle, "LightingPass");

	LightingPass();


	//NVAftermathMarker(dx12_rhi->AM_CL_Handle, "CopyPass");

	



	TemporalAAPass();
	
	D3D12_RESOURCE_BARRIER BarrierDesc = {};
	BarrierDesc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	BarrierDesc.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	BarrierDesc.Transition.pResource = backbuffer->resource.Get();
	BarrierDesc.Transition.Subresource = 0;
	BarrierDesc.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
	BarrierDesc.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	dx12_rhi->CommandList->ResourceBarrier(1, &BarrierDesc);

	dx12_rhi->CommandList->OMSetRenderTargets(1, &backbuffer->CpuHandleRTV, FALSE, nullptr);
	CopyPass();

	if(bDebugDraw)
		DebugPass();



	ImGui_ImplDX12_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	bool show_demo_window = true;

	//ImGui::ShowDemoWindow(&show_demo_window);

	ImGui::Begin("Hello, world!");                          // Create a window called "Hello, world!" and append into it.
	ImGui::Text("This is some useful text.");               // Display some text (you can use a format strings too)
	ImGui::Checkbox("Enable TemporalAA", &bEnableTAA);      // Edit bools storing our window open/close state

	ImGui::SliderFloat3("Light direction", &LightDir.x, -1.0f, 1.0f);
	ImGui::SliderFloat("Light Brightness", &LightIntensity, 0.0f, 20.0f);

	ImGui::SliderFloat("IndirectDiffuse Depth Weight Factor", &FilterIndirectDiffuseConstantCB.IndirectDiffuseWeightFactorDepth, 0.0f, 2.0f);            // Edit 1 float using a slider from 0.0f to 1.0f
	ImGui::SliderFloat("IndirectDiffuse Normal Weight Factor", &FilterIndirectDiffuseConstantCB.IndirectDiffuseWeightFactorNormal, 0.0f, 20.0f);            // Edit 1 float using a slider from 0.0f to 1.0f

	ImGui::End();

	ImGui::Render();
	ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), dx12_rhi->CommandList.Get());


	D3D12_RESOURCE_BARRIER BarrierDescPresent = {};
	BarrierDescPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	BarrierDescPresent.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	BarrierDescPresent.Transition.pResource = backbuffer->resource.Get();
	BarrierDescPresent.Transition.Subresource = 0;
	BarrierDescPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	BarrierDescPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
	dx12_rhi->CommandList->ResourceBarrier(1, &BarrierDescPresent);


	dx12_rhi->CommandList->Close();
	ID3D12CommandList* ppCommandListsEnd[] = { dx12_rhi->CommandList.Get()
	};
	dx12_rhi->CommandQueue->ExecuteCommandLists(_countof(ppCommandListsEnd), ppCommandListsEnd);



	dx12_rhi->EndFrame();

	PrevViewProjMat = ViewProjMat;
	PrevViewMat = ViewMat;
}

void dx12_framework::OnDestroy()
{
	// Ensure that the GPU is no longer referencing resources that are about to be
	// cleaned up by the destructor.
	
	dx12_rhi->WaitGPU();

	ImGui_ImplDX12_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
}

void dx12_framework::OnKeyDown(UINT8 key)
{
	switch (key)
	{
	/*case 'M':
		bMultiThreadRendering = !bMultiThreadRendering;
		break;*/
	case 'B':
		bDebugDraw = !bDebugDraw;
		break;
	case 'T':
		bEnableTAA = !bEnableTAA;
		break;
	case 'C':
		ClampMode++;
		ClampMode = ClampMode % 3;
		break;
	default:
		break;
	}

	m_camera.OnKeyDown(key);
}

void dx12_framework::OnKeyUp(UINT8 key)
{
	m_camera.OnKeyUp(key);
}

struct ParallelDrawTaskSet : enki::ITaskSet
{
	dx12_framework* app;
	UINT StartIndex;
	UINT ThisDraw;
	UINT ThreadIndex;
	ThreadDescriptorHeapPool* DHPool;

	ParallelDrawTaskSet(){}
	ParallelDrawTaskSet(ParallelDrawTaskSet &&) {}
	ParallelDrawTaskSet(const ParallelDrawTaskSet&) = delete;

	virtual void ExecuteRange(enki::TaskSetPartition range, uint32_t threadnum)
	{
		app->RecordDraw(StartIndex, ThisDraw, ThreadIndex, const_cast<ThreadDescriptorHeapPool*>(DHPool));
	}
};

void dx12_framework::DrawMeshPass()
{
	dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(AlbedoBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));
	dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(NormalBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));
	dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(GeomNormalBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));
	dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(VelocityBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));

	dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(DepthBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE));

	const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
	dx12_rhi->CommandList->ClearRenderTargetView(AlbedoBuffer->CpuHandleRTV, clearColor, 0, nullptr);
	dx12_rhi->CommandList->ClearRenderTargetView(NormalBuffer->CpuHandleRTV, clearColor, 0, nullptr);
	dx12_rhi->CommandList->ClearRenderTargetView(GeomNormalBuffer->CpuHandleRTV, clearColor, 0, nullptr);
	dx12_rhi->CommandList->ClearRenderTargetView(VelocityBuffer->CpuHandleRTV, clearColor, 0, nullptr);


	dx12_rhi->CommandList->ClearDepthStencilView(DepthBuffer->CpuHandleDSV, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

	
	dx12_rhi->CommandList->Close();
	ID3D12CommandList* ppCommandLists[] = { dx12_rhi->CommandList.Get() };
	dx12_rhi->CommandQueue->ExecuteCommandLists(1, ppCommandLists);
	dx12_rhi->CommandList->Reset(dx12_rhi->GetCurrentCA(), nullptr);

	ID3D12DescriptorHeap* ppHeaps[] = { dx12_rhi->SRVCBVDescriptorHeapShaderVisible->DH.Get(), dx12_rhi->SamplerDescriptorHeapShaderVisible->DH.Get() };
	dx12_rhi->CommandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);


	if (!bMultiThreadRendering)
	{
		const D3D12_CPU_DESCRIPTOR_HANDLE Rendertargets[] = { AlbedoBuffer->CpuHandleRTV, NormalBuffer->CpuHandleRTV, GeomNormalBuffer->CpuHandleRTV, VelocityBuffer->CpuHandleRTV };



		dx12_rhi->CommandList->OMSetRenderTargets(RS_Mesh->graphicsPSODesc.NumRenderTargets, Rendertargets, FALSE, &DepthBuffer->CpuHandleDSV);

		RS_Mesh->Apply(dx12_rhi->CommandList.Get());
		//RS_Mesh->ps->SetSampler("samplerWrap", samplerWrap.get(), dx12_rhi->CommandList.Get());
		RS_Mesh->SetSampler("samplerWrap", samplerWrap.get(), dx12_rhi->CommandList.Get());


		dx12_rhi->CommandList->RSSetViewports(1, &m_viewport);
		dx12_rhi->CommandList->RSSetScissorRects(1, &m_scissorRect);
		dx12_rhi->CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		//dx12_rhi->CommandList->IASetIndexBuffer(&mesh->Ib->view);
		//dx12_rhi->CommandList->IASetVertexBuffers(0, 1, &mesh->Vb->view);

		RS_Mesh->currentDrawCallIndex = 0;
		/*for (int i = 0; i < mesh->Draws.size(); i++)
		{
			RS_Mesh->vs->currentDrawCallIndex = i;
			RS_Mesh->ps->currentDrawCallIndex = i;

			Mesh::DrawCall& drawcall = mesh->Draws[i];
			ObjConstantBuffer objCB;
			XMStoreFloat4x4(&objCB.WorldMatrix, XMMatrixRotationY(i * 0));
			objCB.ViewDir.x = m_camera.m_lookDirection.x;
			objCB.ViewDir.y = m_camera.m_lookDirection.y;
			objCB.ViewDir.z = m_camera.m_lookDirection.z;

			RS_Mesh->vs->SetConstantValue("ObjParameter", (void*)&objCB.WorldMatrix, dx12_rhi->CommandList.Get(), nullptr);

			Texture* diffuseTex = mesh->Textures[drawcall.DiffuseTextureIndex].get();
			RS_Mesh->ps->SetTexture("diffuseMap", diffuseTex, dx12_rhi->CommandList.Get(), nullptr);

			Texture* normalTex = mesh->Textures[drawcall.NormalTextureIndex].get();
			RS_Mesh->ps->SetTexture("normalMap", normalTex, dx12_rhi->CommandList.Get(), nullptr);

			dx12_rhi->CommandList->DrawIndexedInstanced(drawcall.IndexCount, 1, drawcall.IndexStart, drawcall.VertexBase, 0);
		}*/

		

		int nMesh = 0;

		for (auto& mesh : meshes)
		{
			dx12_rhi->CommandList->IASetIndexBuffer(&mesh->Ib->view);
			dx12_rhi->CommandList->IASetVertexBuffers(0, 1, &mesh->Vb->view);
			/*if (nMesh > 1) break;;
			nMesh++;*/
			for (int i = 0; i < mesh->Draws.size(); i++)
			{
				Mesh::DrawCall& drawcall = mesh->Draws[i];
				ObjConstantBuffer objCB;
				int sizea = sizeof(ObjConstantBuffer);

				objCB.ViewProjectionMatrix = glm::transpose(ViewProjMat);
				//objCB.InvViewProjectionMatrix = InvViewProjMat;
				//objCB.UnjitteredViewProjectionMatrix = UnjitteredViewProjMat;
				objCB.PrevViewProjectionMatrix = glm::transpose(PrevViewProjMat);


				glm::mat4 m; // Identity matrix
				objCB.WorldMatrix = m;
				objCB.ViewDir.x = m_camera.m_lookDirection.x;
				objCB.ViewDir.y = m_camera.m_lookDirection.y;
				objCB.ViewDir.z = m_camera.m_lookDirection.z;

				objCB.RTSize.x = m_width;
				objCB.RTSize.y = m_height;

				objCB.JitterOffset = JitterOffset;

				RS_Mesh->SetConstantValue("ObjParameter", (void*)&objCB, dx12_rhi->CommandList.Get());


				Texture* diffuseTex = drawcall.mat->Diffuse.get();
				if (diffuseTex == nullptr)
					diffuseTex = Materials[0]->Diffuse.get();
				if(diffuseTex)
					RS_Mesh->SetTexture("diffuseMap", diffuseTex, dx12_rhi->CommandList.Get());


				Texture* normalTex = drawcall.mat->Normal.get();
				if (normalTex == nullptr)
					normalTex = Materials[0]->Normal.get();
				if(normalTex)
					RS_Mesh->SetTexture("normalMap", normalTex, dx12_rhi->CommandList.Get());


				dx12_rhi->CommandList->DrawIndexedInstanced(drawcall.IndexCount, 1, drawcall.IndexStart, drawcall.VertexBase, 0);


				RS_Mesh->currentDrawCallIndex++;
			}
		}
	}
	else
	{
		UINT NumThread = dx12_rhi->NumDrawMeshCommandList;
		UINT RemainDraw = mesh->Draws.size();
		UINT NumDrawThread = mesh->Draws.size() / (NumThread);
		UINT StartIndex = 0;

		vector<ThreadDescriptorHeapPool> vecDHPool;
		vecDHPool.resize(NumThread);

		vector<ParallelDrawTaskSet> vecTask;
		vecTask.resize(NumThread);

		for (int i = 0; i < NumThread; i++)
		{
			UINT ThisDraw = NumDrawThread;
			
			if (i == NumThread - 1)
				ThisDraw = RemainDraw;

			ThreadDescriptorHeapPool& DHPool = vecDHPool[i];
			DHPool.AllocPool(RS_Mesh->GetGraphicsBindingDHSize()*ThisDraw);

			// draw
			ParallelDrawTaskSet& task = vecTask[i];
			task.app = this;
			task.StartIndex = StartIndex;
			task.ThisDraw = ThisDraw;
			task.ThreadIndex = i;
			task.DHPool = &DHPool;

			g_TS.AddTaskSetToPipe(&task);

			RemainDraw -= ThisDraw;
			StartIndex += ThisDraw;
		}

		g_TS.WaitforAll();


		UINT NumCL = dx12_rhi->NumDrawMeshCommandList;
		vector< ID3D12CommandList*> vecCL;

		for (int i = 0; i < NumCL; i++)
			vecCL.push_back(dx12_rhi->DrawMeshCommandList[i].Get());

		dx12_rhi->CommandQueue->ExecuteCommandLists(vecCL.size(), &vecCL[0]);
	}

	
	
	dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(AlbedoBuffer->resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
	dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(NormalBuffer->resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
	dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(GeomNormalBuffer->resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
	dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(VelocityBuffer->resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));


	dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(DepthBuffer->resource.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
}

void dx12_framework::RecordDraw (UINT StartIndex, UINT NumDraw, UINT CLIndex, ThreadDescriptorHeapPool* DHPool)
{
	//Texture* backbuffer = ColorBuffer.get();

	//ID3D12GraphicsCommandList* CL = dx12_rhi->DrawMeshCommandList[CLIndex].Get();

	//ID3D12CommandAllocator* CA = dx12_rhi->FrameResourceVec[dx12_rhi->CurrentFrameIndex].VecCommandAllocatorMeshDraw[CLIndex].Get();
	//CL->Reset(CA, nullptr);

	//CL->OMSetRenderTargets(1, &backbuffer->CpuHandleRTV, FALSE, &DepthBuffer->CpuHandleDSV);
	//CL->RSSetViewports(1, &m_viewport);
	//CL->RSSetScissorRects(1, &m_scissorRect);
	//CL->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	//CL->IASetIndexBuffer(&mesh->Ib->view);
	//CL->IASetVertexBuffers(0, 1, &mesh->Vb->view);

	//ID3D12DescriptorHeap* ppHeaps[] = { dx12_rhi->SRVCBVDescriptorHeapShaderVisible->DH.Get(), dx12_rhi->SamplerDescriptorHeapShaderVisible->DH.Get() };
	//CL->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
	//
	//RS_Mesh->ApplyGraphicsRSPSO(CL);
	//RS_Mesh->ps->SetSampler("samplerWrap", samplerWrap.get(), CL, DHPool);

	//glm::mat4x4 ViewMat;
	//glm::mat4x4 ProjMat;

	//memcpy(&ViewMat, &m_camera.GetViewMatrix(), sizeof(glm::mat4x4));
	//memcpy(&ProjMat, &m_camera.GetProjectionMatrix(0.8f, m_aspectRatio, Near, Far), sizeof(glm::mat4x4));

	//glm::mat4x4 ViewProjMat = glm::transpose(ProjMat * ViewMat);

	//for (int i = StartIndex; i < StartIndex + NumDraw; i++)
	//{
	//	Mesh::DrawCall& drawcall = mesh->Draws[i];
	//	ObjConstantBuffer objCB;
	//	objCB.ViewProjectionMatrix = ViewProjMat;
	//	glm::mat4 m; // Identity matrix
	//	objCB.WorldMatrix = m;
	//	objCB.ViewDir.x = m_camera.m_lookDirection.x;
	//	objCB.ViewDir.y = m_camera.m_lookDirection.y;
	//	objCB.ViewDir.z = m_camera.m_lookDirection.z;
	//	RS_Mesh->vs->SetConstantValue("ObjParameter", (void*)&objCB, CL, DHPool);


	//	Texture* diffuseTex = mesh->Textures[drawcall.DiffuseTextureIndex].get();
	//	RS_Mesh->ps->SetTexture("diffuseMap", diffuseTex, CL, DHPool);

	//	Texture* normalTex = mesh->Textures[drawcall.NormalTextureIndex].get();
	//	RS_Mesh->ps->SetTexture("normalMap", normalTex, CL, DHPool);


	//	CL->DrawIndexedInstanced(drawcall.IndexCount, 1, drawcall.IndexStart, drawcall.VertexBase, 0);
	//}
	//
	//CL->Close();
}



void dx12_framework::FilterIndirectDiffusePass()
{
	UINT WriteIndex = 0;
	UINT ReadIndex;

	// first pass
	dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(FilterIndirectDiffusePingPong[WriteIndex]->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
	dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(FilterIndirectDiffusePingPongSH[WriteIndex]->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
	dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(FilterIndirectDiffusePingPongColor[WriteIndex]->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));



	RS_FilterIndirectDiffuse->Apply(dx12_rhi->CommandList.Get());

	RS_FilterIndirectDiffuse->SetTexture("InputTex", GIBuffer.get(), dx12_rhi->CommandList.Get());
	RS_FilterIndirectDiffuse->SetTexture("DepthTex", DepthBuffer.get(), dx12_rhi->CommandList.Get());
	RS_FilterIndirectDiffuse->SetTexture("GeoNormalTex", GeomNormalBuffer.get(), dx12_rhi->CommandList.Get());
	RS_FilterIndirectDiffuse->SetTexture("InGIResultSHTex", GIBufferSH.get(), dx12_rhi->CommandList.Get());
	RS_FilterIndirectDiffuse->SetTexture("InGIResultColorTex", GIBufferColor.get(), dx12_rhi->CommandList.Get());



	RS_FilterIndirectDiffuse->SetUAV("OutputTex", FilterIndirectDiffusePingPong[0].get(), dx12_rhi->CommandList.Get());
	RS_FilterIndirectDiffuse->SetUAV("OutGIResultSH", FilterIndirectDiffusePingPongSH[0].get(), dx12_rhi->CommandList.Get());
	RS_FilterIndirectDiffuse->SetUAV("OutGIResultColor", FilterIndirectDiffusePingPongColor[0].get(), dx12_rhi->CommandList.Get());



	FilterIndirectDiffuseConstantCB.Iteration = 0;
	RS_FilterIndirectDiffuse->SetConstantValue("FilterIndirectDiffuseConstant", &FilterIndirectDiffuseConstantCB, dx12_rhi->CommandList.Get());

	dx12_rhi->CommandList->Dispatch(m_width / 32, m_height / 32, 1);

	dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(FilterIndirectDiffusePingPong[WriteIndex]->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
	dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(FilterIndirectDiffusePingPongSH[WriteIndex]->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
	dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(FilterIndirectDiffusePingPongColor[WriteIndex]->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));


	for (int i = 1; i < 4; i++)
	{
		WriteIndex = 1 - WriteIndex; // 1
		ReadIndex = 1 - WriteIndex; // 0

		dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(FilterIndirectDiffusePingPong[WriteIndex]->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
		dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(FilterIndirectDiffusePingPongSH[WriteIndex]->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
		dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(FilterIndirectDiffusePingPongColor[WriteIndex]->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));


		RS_FilterIndirectDiffuse->Apply(dx12_rhi->CommandList.Get());

		RS_FilterIndirectDiffuse->SetTexture("InputTex", FilterIndirectDiffusePingPong[ReadIndex].get(), dx12_rhi->CommandList.Get());
		RS_FilterIndirectDiffuse->SetTexture("DepthTex", DepthBuffer.get(), dx12_rhi->CommandList.Get());
		RS_FilterIndirectDiffuse->SetTexture("GeoNormalTex", GeomNormalBuffer.get(), dx12_rhi->CommandList.Get());
		RS_FilterIndirectDiffuse->SetTexture("InGIResultSHTex", FilterIndirectDiffusePingPongSH[ReadIndex].get(), dx12_rhi->CommandList.Get());
		RS_FilterIndirectDiffuse->SetTexture("InGIResultColorTex", FilterIndirectDiffusePingPongColor[ReadIndex].get(), dx12_rhi->CommandList.Get());


		RS_FilterIndirectDiffuse->SetUAV("OutputTex", FilterIndirectDiffusePingPong[WriteIndex].get(), dx12_rhi->CommandList.Get());
		RS_FilterIndirectDiffuse->SetUAV("OutGIResultSH", FilterIndirectDiffusePingPongSH[WriteIndex].get(), dx12_rhi->CommandList.Get());
		RS_FilterIndirectDiffuse->SetUAV("OutGIResultColor", FilterIndirectDiffusePingPongColor[WriteIndex].get(), dx12_rhi->CommandList.Get());



		FilterIndirectDiffuseConstantCB.Iteration = i;
		RS_FilterIndirectDiffuse->SetConstantValue("FilterIndirectDiffuseConstant", &FilterIndirectDiffuseConstantCB, dx12_rhi->CommandList.Get());

		dx12_rhi->CommandList->Dispatch(m_width / 32, m_height / 32, 1);

		dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(FilterIndirectDiffusePingPong[WriteIndex]->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
		dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(FilterIndirectDiffusePingPongSH[WriteIndex]->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
		dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(FilterIndirectDiffusePingPongColor[WriteIndex]->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

	}
}


void dx12_framework::InitRaytracing()
{
	// init raytracing
	vecBLAS.reserve(meshes.size());
	int i = 0;
	for (auto& mesh : meshes)
	{
		shared_ptr<RTAS> blas = mesh->CreateBLAS();
		if (blas == nullptr)
		{
			continue;
		}
		vecBLAS.push_back(blas);
		i++;

	}
	// TODO : per model world matrix
	TLAS = dx12_rhi->CreateTLAS(vecBLAS);
	
	dx12_rhi->CommandList->Close();
	ID3D12CommandList* ppCommandLists[] = { dx12_rhi->CommandList.Get() };
	dx12_rhi->CommandQueue->ExecuteCommandLists(1, ppCommandLists);

	dx12_rhi->WaitGPU();

	// create shadow rtpso
	PSO_RT_SHADOW = unique_ptr<RTPipelineStateObject>(new RTPipelineStateObject);
	

	PSO_RT_SHADOW->NumInstance = meshes.size(); // important for cbv allocation & shadertable size.

	// new interface
	PSO_RT_SHADOW->AddHitGroup("HitGroup", "chs", "");
	PSO_RT_SHADOW->AddShader("rayGen", RTPipelineStateObject::RAYGEN);
	PSO_RT_SHADOW->BindUAV("rayGen", "gOutput", 0);
	PSO_RT_SHADOW->BindSRV("rayGen", "gRtScene", 0);
	PSO_RT_SHADOW->BindSRV("rayGen", "DepthTex", 1);
	PSO_RT_SHADOW->BindSRV("rayGen", "WorldNormalTex", 2);
	PSO_RT_SHADOW->BindSRV("rayGen", "AlbedoTex", 3);

	//PSO_RT_SHADOW->BindSRV("rayGen", "dummy", 3);


	PSO_RT_SHADOW->BindCBV("rayGen", "ViewParameter", 0, sizeof(RTShadowViewParamCB), 1);
	PSO_RT_SHADOW->BindSampler("rayGen", "samplerWrap", 0);

	PSO_RT_SHADOW->AddShader("miss", RTPipelineStateObject::MISS);
	PSO_RT_SHADOW->AddShader("chs", RTPipelineStateObject::HIT);

	

	PSO_RT_SHADOW->MaxRecursion = 1;
	PSO_RT_SHADOW->MaxAttributeSizeInBytes = sizeof(float) * 2;
	PSO_RT_SHADOW->MaxPayloadSizeInBytes = sizeof(float) * 1;

	PSO_RT_SHADOW->InitRS("Shaders\\RaytracedShadow.hlsl");


	// create reflection rtpso

	PSO_RT_REFLECTION = unique_ptr<RTPipelineStateObject>(new RTPipelineStateObject);
	PSO_RT_REFLECTION->NumInstance = meshes.size();

	PSO_RT_REFLECTION->AddHitGroup("HitGroup", "chs", "");

	PSO_RT_REFLECTION->AddShader("rayGen", RTPipelineStateObject::RAYGEN);
	PSO_RT_REFLECTION->BindUAV("rayGen", "gOutput", 0);
	PSO_RT_REFLECTION->BindSRV("rayGen", "gRtScene", 0);
	PSO_RT_REFLECTION->BindSRV("rayGen", "DepthTex", 1);
	PSO_RT_REFLECTION->BindSRV("rayGen", "WorldNormalTex", 2);
	PSO_RT_REFLECTION->BindCBV("rayGen", "ViewParameter", 0, sizeof(RTReflectionViewParam), 1);
	PSO_RT_REFLECTION->BindSampler("rayGen", "samplerWrap", 0);

	PSO_RT_REFLECTION->AddShader("miss", RTPipelineStateObject::MISS);

	PSO_RT_REFLECTION->AddShader("chs", RTPipelineStateObject::HIT);
	PSO_RT_REFLECTION->BindSRV("chs", "vertices", 3);
	PSO_RT_REFLECTION->BindSRV("chs", "indices", 4);
	PSO_RT_REFLECTION->BindSRV("chs", "AlbedoTex", 5);
	PSO_RT_REFLECTION->BindSampler("chs", "samplerWrap", 0);


	PSO_RT_REFLECTION->MaxRecursion = 1;
	PSO_RT_REFLECTION->MaxAttributeSizeInBytes = sizeof(float) * 2;
	PSO_RT_REFLECTION->MaxPayloadSizeInBytes = sizeof(float) * 4;

	PSO_RT_REFLECTION->InitRS("Shaders\\RaytracedReflection.hlsl");

	// gi rtpso

	PSO_RT_GI = unique_ptr<RTPipelineStateObject>(new RTPipelineStateObject);
	PSO_RT_GI->NumInstance = meshes.size();

	PSO_RT_GI->AddHitGroup("HitGroup", "chs", "");

	PSO_RT_GI->AddShader("rayGen", RTPipelineStateObject::RAYGEN);
	PSO_RT_GI->BindUAV("rayGen", "GIResultSH", 0);
	PSO_RT_GI->BindUAV("rayGen", "GIResultColor", 1);


	PSO_RT_GI->BindSRV("rayGen", "gRtScene", 0);
	PSO_RT_GI->BindSRV("rayGen", "DepthTex", 1);
	PSO_RT_GI->BindSRV("rayGen", "WorldNormalTex", 2);
	PSO_RT_GI->BindCBV("rayGen", "ViewParameter", 0, sizeof(RTGIViewParam), 1);
	PSO_RT_GI->BindSampler("rayGen", "samplerWrap", 0);
	PSO_RT_GI->BindSRV("rayGen", "BlueNoiseTex", 7);


	PSO_RT_GI->AddShader("miss", RTPipelineStateObject::MISS);

	PSO_RT_GI->AddShader("chs", RTPipelineStateObject::HIT);
	PSO_RT_GI->BindSRV("chs", "vertices", 3);
	PSO_RT_GI->BindSRV("chs", "indices", 4);
	PSO_RT_GI->BindSRV("chs", "AlbedoTex", 5);
	PSO_RT_GI->BindSRV("chs", "InstanceProperty", 6);
	PSO_RT_GI->BindSampler("chs", "samplerWrap", 0);


	PSO_RT_GI->MaxRecursion = 1;
	PSO_RT_GI->MaxAttributeSizeInBytes = sizeof(float) * 2;
	/*
		float3 positin;
		float3 color;
		float3 normal;
		float distance;
	*/
	PSO_RT_GI->MaxPayloadSizeInBytes = sizeof(float) * 10;

	PSO_RT_GI->InitRS("Shaders\\RaytracedGI.hlsl");

	InstancePropertyBuffer = dx12_rhi->CreateBuffer(sizeof(InstanceProperty) * 500);

	uint8_t* pData;
	InstancePropertyBuffer->resource->Map(0, nullptr, (void**)&pData);

	for (auto& m : meshes)
	{
		glm::mat4x4 identityMat;
		memcpy(pData, &identityMat, sizeof(glm::mat4x4));
		pData += sizeof(InstanceProperty);
	}

	InstancePropertyBuffer->resource->Unmap(0, nullptr);
}

vector<UINT64> ResourceInt64array(ComPtr<ID3D12Resource> resource, int size)
{
	uint8_t* pData;
	HRESULT hr = resource->Map(0, nullptr, (void**)&pData);

	int size64 = size / sizeof(UINT64);
	vector<UINT64> mem;
	for (int i = 0; i < size64; i++)
	{
		UINT64 v = *(UINT64*)(pData + i * sizeof(UINT64));
		mem.push_back(v);
	}

	return mem;
}
void dx12_framework::RaytraceShadowPass()
{
	dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(ShadowBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));


	PSO_RT_SHADOW->BeginShaderTable();

	for (int i = 0; i < meshes.size(); i++)
	{
		PSO_RT_SHADOW->SetHitProgram("chs", i);
	}

	PSO_RT_SHADOW->SetUAV("rayGen", "gOutput", ShadowBuffer->GpuHandleUAV);
	PSO_RT_SHADOW->SetSRV("rayGen", "gRtScene", TLAS->GPUHandle);
	PSO_RT_SHADOW->SetSRV("rayGen", "DepthTex", DepthBuffer->GpuHandleSRV);
	PSO_RT_SHADOW->SetSRV("rayGen", "WorldNormalTex", GeomNormalBuffer->GpuHandleSRV);
	PSO_RT_SHADOW->SetSRV("rayGen", "AlbedoTex", AlbedoBuffer->GpuHandleSRV);


	PSO_RT_SHADOW->SetCBVValue("rayGen", "ViewParameter", &RTShadowViewParam, sizeof(RTShadowViewParamCB));
	PSO_RT_SHADOW->SetSampler("rayGen", "samplerWrap", samplerWrap.get());

	//PSO_RT_SHADOW->SetHitProgram("HitGroup", 0); // this pass use only 1 hit program

	PSO_RT_SHADOW->EndShaderTable();

	PSO_RT_SHADOW->Apply(m_width, m_height);

	dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(ShadowBuffer->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
}

void dx12_framework::RaytraceReflectionPass()
{
	dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(ReflectionBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));


	PSO_RT_REFLECTION->BeginShaderTable();

	PSO_RT_REFLECTION->SetUAV("rayGen", "gOutput", ReflectionBuffer->GpuHandleUAV);
	PSO_RT_REFLECTION->SetSRV("rayGen", "gRtScene", TLAS->GPUHandle);
	PSO_RT_REFLECTION->SetSRV("rayGen", "DepthTex", DepthBuffer->GpuHandleSRV);
	PSO_RT_REFLECTION->SetSRV("rayGen", "WorldNormalTex", GeomNormalBuffer->GpuHandleSRV);
	PSO_RT_REFLECTION->SetCBVValue("rayGen", "ViewParameter", &RTReflectionViewParam, sizeof(RTReflectionViewParamCB));
	PSO_RT_REFLECTION->SetSampler("rayGen", "samplerWrap", samplerWrap.get());


	for (int i = 0; i < meshes.size(); i++)
	{
		auto& mesh = meshes[i];

		PSO_RT_REFLECTION->SetHitProgram("chs", i);

		PSO_RT_REFLECTION->ResetHitProgramBinding("chs", i, 2);
		Texture* diffuseTex = mesh->Draws[0].mat->Diffuse.get();
		if (diffuseTex == nullptr)
			diffuseTex = Materials[0]->Diffuse.get();
		
		PSO_RT_REFLECTION->AddHitProgramDescriptor("chs", mesh->Vb->GpuHandleSRV, i);
		PSO_RT_REFLECTION->AddHitProgramDescriptor("chs", mesh->Ib->GpuHandleSRV, i);
		PSO_RT_REFLECTION->AddHitProgramDescriptor("chs", diffuseTex->GpuHandleSRV, i);
		PSO_RT_REFLECTION->AddHitProgramDescriptor("chs", samplerWrap->GpuHandle, i);

	}



	PSO_RT_REFLECTION->EndShaderTable();


	PSO_RT_REFLECTION->Apply(m_width, m_height);

	dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(ReflectionBuffer->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
}

void dx12_framework::RaytraceGIPass()
{
	dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(GIBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
	dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(GIBufferSH->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
	dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(GIBufferColor->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));


	PSO_RT_GI->BeginShaderTable();

	PSO_RT_GI->SetUAV("rayGen", "gOutput", GIBuffer->GpuHandleUAV);
	PSO_RT_GI->SetUAV("rayGen", "GIResultSH", GIBufferSH->GpuHandleUAV);
	PSO_RT_GI->SetUAV("rayGen", "GIResultColor", GIBufferColor->GpuHandleUAV);

	PSO_RT_GI->SetSRV("rayGen", "gRtScene", TLAS->GPUHandle);
	PSO_RT_GI->SetSRV("rayGen", "DepthTex", DepthBuffer->GpuHandleSRV);
	PSO_RT_GI->SetSRV("rayGen", "WorldNormalTex", GeomNormalBuffer->GpuHandleSRV);
	PSO_RT_GI->SetSRV("rayGen", "BlueNoiseTex", BlueNoiseTex->GpuHandleSRV);

	PSO_RT_GI->SetCBVValue("rayGen", "ViewParameter", &RTGIViewParam, sizeof(RTGIViewParamCB));
	PSO_RT_GI->SetSampler("rayGen", "samplerWrap", samplerWrap.get());


	for (int i = 0; i < meshes.size(); i++)
	{
		auto& mesh = meshes[i];

		PSO_RT_GI->SetHitProgram("chs", i);

		PSO_RT_GI->ResetHitProgramBinding("chs", i, 2);
		Texture* diffuseTex = mesh->Draws[0].mat->Diffuse.get();
		if (diffuseTex == nullptr)
			diffuseTex = Materials[0]->Diffuse.get();

		PSO_RT_GI->AddHitProgramDescriptor("chs", mesh->Vb->GpuHandleSRV, i);
		PSO_RT_GI->AddHitProgramDescriptor("chs", mesh->Ib->GpuHandleSRV, i);
		PSO_RT_GI->AddHitProgramDescriptor("chs", diffuseTex->GpuHandleSRV, i);
		PSO_RT_GI->AddHitProgramDescriptor("chs", InstancePropertyBuffer->GpuHandleSRV, i);
		PSO_RT_GI->AddHitProgramDescriptor("chs", samplerWrap->GpuHandle, i);


	}



	PSO_RT_GI->EndShaderTable();


	PSO_RT_GI->Apply(m_width, m_height);

	dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(GIBuffer->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
	dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(GIBufferSH->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
	dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(GIBufferColor->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
}
