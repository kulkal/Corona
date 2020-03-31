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
#include "MyToyDX12Renderer.h"
#include "DXCAPI/dxcapi.use.h"
#include "Utils.h"
#include <iostream>
#include <algorithm>
#include <array>
#include <sstream>
#include <fstream>
#include <variant>
#include <codecvt>
#include <dxgidebug.h>
#include "assimp/include/Importer.hpp"
#include "assimp/include/scene.h"
#include "assimp/include/postprocess.h"
//#pragma comment(lib, "assimp\\lib\\assimp.lib")
#include "external/GFSDK_Aftermath/include/GFSDK_Aftermath.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"



#ifdef _DEBUG
#define new DEBUG_CLIENTBLOCK
#endif

#define arraysize(a) (sizeof(a)/sizeof(a[0]))
#define align_to(_alignment, _val) (((_val + _alignment - 1) / _alignment) * _alignment)

static dxc::DxcDllSupport gDxcDllHelper;


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




MyToyDX12Renderer::MyToyDX12Renderer(UINT width, UINT height, std::wstring name) :
	DXSample(width, height, name),
	m_viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)),
	m_scissorRect(0, 0, static_cast<LONG>(width), static_cast<LONG>(height))
{
	int tmpFlag = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);

	// Turn on leak-checking bit.
	tmpFlag |= _CRTDBG_LEAK_CHECK_DF;
	tmpFlag |= _CRTDBG_ALLOC_MEM_DF;
	//tmpFlag |= _CRTDBG_CHECK_ALWAYS_DF;


	// Turn off CRT block checking bit.
	//tmpFlag &= ~_CRTDBG_CHECK_CRT_DF;

	// Set flag to the new value.
	_CrtSetDbgFlag(tmpFlag);
}

MyToyDX12Renderer::~MyToyDX12Renderer()
{
	
}


void MyToyDX12Renderer::OnInit()
{
	//_CrtSetBreakAlloc(196);

	CoInitialize(NULL);

	g_TS.Initialize(8);

	m_camera.Init({0, 0, 0 });
	m_camera.SetMoveSpeed(200);

	LoadPipeline();
	LoadAssets();
}

void MyToyDX12Renderer::LoadPipeline()
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
		//spDebugController1->SetEnableGPUBasedValidation(true);
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

	dx12_rhi = std::make_unique<SimpleDX12>(m_device);

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
		dx12_rhi->CmdQ->CmdQueue.Get(),		// Swap chain needs the queue so that it can force a flush on it.
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

void MyToyDX12Renderer::LoadAssets()
{
	InitBlueNoiseTexture();
	InitImgui();
	InitDrawMeshRS();
	InitCopyPass();
	InitDebugPass();
	InitLightingPass();
	InitTemporalAAPass();
	InitSpatialDenoisingPass();
	InitTemporalDenoisingPass();

	
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
	SpeculaGIBuffer = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);

	NAME_D3D12_OBJECT(SpeculaGIBuffer->resource);

	GIBufferSH = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);

	NAME_D3D12_OBJECT(GIBufferSH->resource);

	GIBufferColor = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);

	NAME_D3D12_OBJECT(GIBufferColor->resource);

	// gi result sh
	GIBufferSHTemporal[0] = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);

	NAME_D3D12_OBJECT(GIBufferSHTemporal[0]->resource);

	GIBufferSHTemporal[1] = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);

	NAME_D3D12_OBJECT(GIBufferSHTemporal[1]->resource);

	// gi result color
	GIBufferColorTemporal[0] = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);

	NAME_D3D12_OBJECT(GIBufferColorTemporal[0]->resource);

	GIBufferColorTemporal[1] = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);

	NAME_D3D12_OBJECT(GIBufferColorTemporal[1]->resource);

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

	// pbr material
	MaterialBuffer = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R8G8B8A8_UNORM,
		D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);
	MaterialBuffer->MakeRTV();

	NAME_D3D12_OBJECT(MaterialBuffer->resource);

	// depth 
	DepthBuffer = dx12_rhi->CreateTexture2D(DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);
	DepthBuffer->MakeDSV();
	NAME_D3D12_OBJECT(DepthBuffer->resource);

	DefaultWhiteTex = dx12_rhi->CreateTextureFromFile(L"default/default_white.png", false);
	DefaultNormalTex = dx12_rhi->CreateTextureFromFile(L"default/default_normal.png", true);

	Sponza = LoadModel("Sponza/Sponza.fbx");

	ShaderBall = LoadModel("shaderball/shaderBall.fbx");

	glm::mat4x4 scaleMat = glm::scale(vec3(2.5, 2.5, 2.5));
	glm::mat4x4 translatemat = glm::translate(glm::vec3(-150, 20, 0));
	ShaderBall->SetTransform(scaleMat* translatemat );
	
	//Buddha = LoadModel("buddha/buddha.obj");

	/*glm::mat4x4 buddhaTM = glm::scale(vec3(100, 100, 100));
	Buddha->SetTransform(buddhaTM);*/

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

shared_ptr<Scene> MyToyDX12Renderer::LoadModel(string fileName)
{
	Scene* scene = new Scene;

	Assimp::Importer importer;
	const aiScene* assimpScene = importer.ReadFile(fileName, 0);
	
	std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
	wstring wide = converter.from_bytes(fileName);

	wstring dir = GetDirectoryFromFilePath(wide.c_str());
	//wstring dir = L"Sponza/";

	UINT flags = aiProcess_CalcTangentSpace |
		aiProcess_Triangulate |
		aiProcess_JoinIdenticalVertices |
		aiProcess_MakeLeftHanded |
		aiProcess_RemoveRedundantMaterials |
		aiProcess_FlipUVs |
		aiProcess_FlipWindingOrder;

		flags |= aiProcess_PreTransformVertices /*| aiProcess_OptimizeMeshes*/;

	assimpScene = importer.ApplyPostProcessing(flags);

	const int numMaterials = assimpScene->mNumMaterials;
	scene->Materials.reserve(numMaterials);
	for (int i = 0; i < numMaterials; ++i)
	{
		const aiMaterial& aiMat = *assimpScene->mMaterials[i];
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
			if (diffuseTex)
				mat->Diffuse = diffuseTex;
			else
				mat->Diffuse = DefaultWhiteTex;
		}

		if (aiMat.GetTexture(aiTextureType_NORMALS, 0, &normalMapPath) == aiReturn_SUCCESS
			|| aiMat.GetTexture(aiTextureType_HEIGHT, 0, &normalMapPath) == aiReturn_SUCCESS)
			wNormalTex = GetFileName(AnsiToWString(normalMapPath.C_Str()).c_str());

		if (wNormalTex.length() != 0)
		{
			shared_ptr<Texture> normalTex = dx12_rhi->CreateTextureFromFile(dir + wNormalTex, true);
			if (normalTex)
				mat->Normal = normalTex;
			else
				mat->Normal = DefaultNormalTex;
		}
		
		scene->Materials.push_back(shared_ptr<Material>(mat));
	}

	struct Vertex
	{
		glm::vec3 Position;
		glm::vec3 Normal;
		glm::vec2 UV;
		glm::vec3 Tangent;
	};
	const UINT numMeshes = assimpScene->mNumMeshes;

	UINT totalNumVert = 0;
	for (UINT i = 0; i < numMeshes; ++i)
	{
		aiMesh* asMesh = assimpScene->mMeshes[i];

		totalNumVert += asMesh->mNumVertices;
	}

	for (UINT i = 0; i < numMeshes; ++i)
	{
		aiMesh* asMesh = assimpScene->mMeshes[i];

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
		dc.mat = scene->Materials[asMesh->mMaterialIndex];
		
		mesh->Draws.push_back(dc);

		scene->meshes.push_back(shared_ptr<Mesh>(mesh));
	}

	shared_ptr<Scene> scenePtr = shared_ptr<Scene>(scene);

	return scenePtr;
}

void MyToyDX12Renderer::InitSpatialDenoisingPass()
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
		ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"Shaders\\SpatialDenoising.hlsl").c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "SpatialFilter", "cs_5_0", compileFlags, 0, &computeShader, &compilationMsgs));
	}
	catch (const std::exception& e)
	{
		OutputDebugStringA(reinterpret_cast<const char*>(compilationMsgs->GetBufferPointer()));
		compilationMsgs->Release();
		return;
	}

	Shader* cs = new Shader((UINT8*)computeShader->GetBufferPointer(), computeShader->GetBufferSize());

	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};

	RS_SpatialDenoisingFilter = shared_ptr<PipelineStateObject>(new PipelineStateObject);
	RS_SpatialDenoisingFilter->cs = unique_ptr<Shader>(cs);
	RS_SpatialDenoisingFilter->computePSODesc = computePsoDesc;
	RS_SpatialDenoisingFilter->BindTexture("DepthTex", 0, 1);
	RS_SpatialDenoisingFilter->BindTexture("GeoNormalTex", 1, 1);
	RS_SpatialDenoisingFilter->BindTexture("InGIResultSHTex", 2, 1);
	RS_SpatialDenoisingFilter->BindTexture("InGIResultColorTex", 3, 1);

	RS_SpatialDenoisingFilter->BindUAV("OutGIResultSH", 0);
	RS_SpatialDenoisingFilter->BindUAV("OutGIResultColor", 1);

	RS_SpatialDenoisingFilter->BindConstantBuffer("SpatialFilterConstant", 0, sizeof(SpatialFilterConstant), 1);
	RS_SpatialDenoisingFilter->IsCompute = true;
	RS_SpatialDenoisingFilter->Init();

	UINT WidthGI = m_width / GIBufferScale;
	UINT HeightGI = m_height / GIBufferScale;

	FilterIndirectDiffusePingPongSH[0] = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, WidthGI, HeightGI, 1);

	NAME_D3D12_OBJECT(FilterIndirectDiffusePingPongSH[0]->resource);

	FilterIndirectDiffusePingPongSH[1] = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, WidthGI, HeightGI, 1);

	NAME_D3D12_OBJECT(FilterIndirectDiffusePingPongSH[1]->resource);

	FilterIndirectDiffusePingPongColor[0] = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, WidthGI, HeightGI, 1);

	NAME_D3D12_OBJECT(FilterIndirectDiffusePingPongColor[0]->resource);

	FilterIndirectDiffusePingPongColor[1] = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, WidthGI, HeightGI, 1);

	NAME_D3D12_OBJECT(FilterIndirectDiffusePingPongColor[1]->resource);
}

void MyToyDX12Renderer::InitTemporalDenoisingPass()
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
		ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"Shaders\\TemporalDenoising.hlsl").c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "TemporalFilter", "cs_5_0", compileFlags, 0, &computeShader, &compilationMsgs));
	}
	catch (const std::exception& e)
	{
		OutputDebugStringA(reinterpret_cast<const char*>(compilationMsgs->GetBufferPointer()));
		compilationMsgs->Release();
		return;
	}

	Shader* cs = new Shader((UINT8*)computeShader->GetBufferPointer(), computeShader->GetBufferSize());

	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};

	RS_TemporalDenoisingFilter = shared_ptr<PipelineStateObject>(new PipelineStateObject);
	RS_TemporalDenoisingFilter->cs = unique_ptr<Shader>(cs);
	RS_TemporalDenoisingFilter->computePSODesc = computePsoDesc;
	RS_TemporalDenoisingFilter->BindTexture("DepthTex", 0, 1);
	RS_TemporalDenoisingFilter->BindTexture("GeoNormalTex", 1, 1);
	RS_TemporalDenoisingFilter->BindTexture("InGIResultSHTex", 2, 1);
	RS_TemporalDenoisingFilter->BindTexture("InGIResultColorTex", 3, 1);
	RS_TemporalDenoisingFilter->BindTexture("InGIResultSHTexPrev", 4, 1);
	RS_TemporalDenoisingFilter->BindTexture("InGIResultColorTexPrev", 5, 1);
	RS_TemporalDenoisingFilter->BindTexture("VelocityTex", 6, 1);

	RS_TemporalDenoisingFilter->BindUAV("OutGIResultSH", 0);
	RS_TemporalDenoisingFilter->BindUAV("OutGIResultColor", 1);
	RS_TemporalDenoisingFilter->BindUAV("OutGIResultSHDS", 2);
	RS_TemporalDenoisingFilter->BindUAV("OutGIResultColorDS", 3);

	RS_TemporalDenoisingFilter->BindConstantBuffer("TemporalFilterConstant", 0, sizeof(TemporalFilterConstant), 1);
	RS_TemporalDenoisingFilter->IsCompute = true;
	RS_TemporalDenoisingFilter->Init();
}

void MyToyDX12Renderer::InitDrawMeshRS()
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
		return;
	}

	try
	{
		ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"Shaders\\MeshDraw.hlsl").c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PSMain", "ps_5_0", compileFlags, 0, &pixelShader, &compilationMsgs));
	}
	catch (const std::exception& e)
	{
		OutputDebugStringA(reinterpret_cast<const char*>(compilationMsgs->GetBufferPointer()));
		compilationMsgs->Release();
		return;
	}

	Shader* ps = new Shader((UINT8*)pixelShader->GetBufferPointer(), pixelShader->GetBufferSize());
	Shader* vs = new Shader((UINT8*)vertexShader->GetBufferPointer(), vertexShader->GetBufferSize());

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
	psoDescMesh.NumRenderTargets = 5;
	psoDescMesh.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	psoDescMesh.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	psoDescMesh.RTVFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	psoDescMesh.RTVFormats[3] = DXGI_FORMAT_R16G16_FLOAT;
	psoDescMesh.RTVFormats[4] = DXGI_FORMAT_R8G8B8A8_UNORM;

	psoDescMesh.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDescMesh.SampleDesc.Count = 1;

	
	RS_Mesh = shared_ptr<PipelineStateObject>(new PipelineStateObject);
	RS_Mesh->ps = shared_ptr<Shader>(ps);
	RS_Mesh->vs = shared_ptr<Shader>(vs);
	RS_Mesh->graphicsPSODesc = psoDescMesh;
	RS_Mesh->BindTexture("diffuseMap", 0, 1);
	RS_Mesh->BindTexture("normalMap", 1, 1);
	RS_Mesh->BindSampler("samplerWrap", 0);
	RS_Mesh->BindConstantBuffer("ObjParameter", 0, sizeof(MeshDrawConstantBuffer), 400);

	bool bSucess = RS_Mesh->Init();
}

void MyToyDX12Renderer::InitImgui()
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

void MyToyDX12Renderer::InitBlueNoiseTexture()
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

void MyToyDX12Renderer::InitCopyPass()
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
		return;
	}

	try
	{
		ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"Shaders\\CopyPS.hlsl").c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PSMain", "ps_5_0", compileFlags, 0, &pixelShader, &compilationMsgs));
	}
	catch (const std::exception& e)
	{
		OutputDebugStringA(reinterpret_cast<const char*>(compilationMsgs->GetBufferPointer()));
		compilationMsgs->Release();
		return;
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

	RS_Copy = shared_ptr<PipelineStateObject>(new PipelineStateObject);
	RS_Copy->ps = shared_ptr<Shader>(ps);
	RS_Copy->vs = shared_ptr<Shader>(vs);
	RS_Copy->graphicsPSODesc = psoDesc;

	RS_Copy->BindTexture("SrcTex", 0, 1);
	RS_Copy->BindSampler("samplerWrap", 0);
	RS_Copy->BindConstantBuffer("ScaleOffsetParams", 0, sizeof(CopyScaleOffsetCB), 15);

	RS_Copy->Init();
}

void MyToyDX12Renderer::InitDebugPass()
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
		ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"Shaders\\DebugPS.hlsl").c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VSMain", "vs_5_0", compileFlags, 0, &vertexShader, &compilationMsgs));
	}
	catch (const std::exception& e)
	{
		OutputDebugStringA(reinterpret_cast<const char*>(compilationMsgs->GetBufferPointer()));
		compilationMsgs->Release();
		return;
	}

	try
	{
		ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"Shaders\\DebugPS.hlsl").c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PSMain", "ps_5_0", compileFlags, 0, &pixelShader, &compilationMsgs));
	}
	catch (const std::exception& e)
	{
		OutputDebugStringA(reinterpret_cast<const char*>(compilationMsgs->GetBufferPointer()));
		compilationMsgs->Release();
		return;
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

	RS_Debug = shared_ptr<PipelineStateObject>(new PipelineStateObject);
	RS_Debug->ps = shared_ptr<Shader>(ps);
	RS_Debug->vs = shared_ptr<Shader>(vs);
	RS_Debug->graphicsPSODesc = psoDesc;

	RS_Debug->BindTexture("SrcTex", 0, 1);
	RS_Debug->BindTexture("SrcTexSH", 1, 1);
	RS_Debug->BindTexture("SrcTexNormal", 2, 1);

	RS_Debug->BindSampler("samplerWrap", 0);
	RS_Debug->BindConstantBuffer("DebugPassCB", 0, sizeof(DebugPassCB), 15);

	RS_Debug->Init();
}

void MyToyDX12Renderer::InitLightingPass()
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
		return;
	}

	try
	{
		ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"Shaders\\LightingPS.hlsl").c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PSMain", "ps_5_0", compileFlags, 0, &pixelShader, &compilationMsgs));
	}
	catch (const std::exception& e)
	{
		OutputDebugStringA(reinterpret_cast<const char*>(compilationMsgs->GetBufferPointer()));
		compilationMsgs->Release();
		return;
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

	RS_Lighting = shared_ptr<PipelineStateObject>(new PipelineStateObject);
	RS_Lighting->ps = shared_ptr<Shader>(ps);
	RS_Lighting->vs = shared_ptr<Shader>(vs);
	RS_Lighting->graphicsPSODesc = psoDesc;

	RS_Lighting->BindTexture("AlbedoTex", 0, 1);
	RS_Lighting->BindTexture("NormalTex", 1, 1);
	RS_Lighting->BindTexture("ShadowTex", 2, 1);
	RS_Lighting->BindTexture("VelocityTex", 3, 1);
	RS_Lighting->BindTexture("DepthTex", 4, 1);
	RS_Lighting->BindTexture("GIResultSHTex", 5, 1);
	RS_Lighting->BindTexture("GIResultColorTex", 6, 1);

	RS_Lighting->BindSampler("samplerWrap", 0);
	RS_Lighting->BindConstantBuffer("LightingParam", 0, sizeof(LightingParam));
	RS_Lighting->Init();
}

void MyToyDX12Renderer::InitTemporalAAPass()
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
		return;
	}

	try
	{
		ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"Shaders\\TemporalAA.hlsl").c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PSMain", "ps_5_0", compileFlags, 0, &pixelShader, &compilationMsgs));
	}
	catch (const std::exception& e)
	{
		OutputDebugStringA(reinterpret_cast<const char*>(compilationMsgs->GetBufferPointer()));
		compilationMsgs->Release();
		return;
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

	RS_TemporalAA = shared_ptr<PipelineStateObject>(new PipelineStateObject);
	RS_TemporalAA->ps = shared_ptr<Shader>(ps);
	RS_TemporalAA->vs = shared_ptr<Shader>(vs);
	RS_TemporalAA->graphicsPSODesc = psoDesc;

	RS_TemporalAA->BindTexture("CurrentColorTex", 0, 1);
	RS_TemporalAA->BindTexture("PrevColorTex", 1, 1);
	RS_TemporalAA->BindTexture("VelocityTex", 2, 1);
	RS_TemporalAA->BindTexture("DepthTex", 3, 1);

	RS_TemporalAA->BindSampler("samplerWrap", 0);
	RS_TemporalAA->BindConstantBuffer("LightingParam", 0, sizeof(LightingParam));
	RS_TemporalAA->Init();
}

void MyToyDX12Renderer::CopyPass()
{
	PIXScopedEvent(dx12_rhi->GlobalCmdList->CmdList.Get(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "CopyPass");

	Texture* backbuffer = framebuffers[dx12_rhi->CurrentFrameIndex].get();
	Texture* ResolveTarget = ColorBuffers[ColorBufferWriteIndex];//framebuffers[dx12_rhi->CurrentFrameIndex].get();

	RS_Copy->currentDrawCallIndex = 0;
	RS_Copy->Apply(dx12_rhi->GlobalCmdList->CmdList.Get());


	RS_Copy->SetSampler("samplerWrap", samplerWrap.get(), dx12_rhi->GlobalCmdList->CmdList.Get());
	RS_Copy->SetTexture("SrcTex", ResolveTarget, dx12_rhi->GlobalCmdList->CmdList.Get());

	CopyScaleOffsetCB cb;
	cb.Offset = glm::vec4(0, 0, 0, 0);
	cb.Scale = glm::vec4(1, 1, 0, 0);
	RS_Copy->SetConstantValue("ScaleOffsetParams", &cb, dx12_rhi->GlobalCmdList->CmdList.Get());

	RS_Copy->Apply(dx12_rhi->GlobalCmdList->CmdList.Get());

	UINT Width = m_width / 1;
	UINT Height = m_height / 1;
	CD3DX12_VIEWPORT viewport(0.0f, 0.0f, static_cast<float>(Width), static_cast<float>(Height));
	CD3DX12_RECT scissorRect(0, 0, static_cast<LONG>(Width), static_cast<LONG>(Height));

	dx12_rhi->GlobalCmdList->CmdList->RSSetViewports(1, &viewport);
	dx12_rhi->GlobalCmdList->CmdList->RSSetScissorRects(1, &scissorRect);

	dx12_rhi->GlobalCmdList->CmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	dx12_rhi->GlobalCmdList->CmdList->IASetVertexBuffers(0, 1, &FullScreenVB->view);
	
	dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);

	RS_Copy->currentDrawCallIndex++;
	
	PIXEndEvent(dx12_rhi->GlobalCmdList->CmdList.Get());
}

void MyToyDX12Renderer::DebugPass()
{
	PIXScopedEvent(dx12_rhi->GlobalCmdList->CmdList.Get(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "DebugPass");

	RS_Debug->currentDrawCallIndex = 0;
	RS_Debug->Apply(dx12_rhi->GlobalCmdList->CmdList.Get());
	RS_Debug->SetSampler("samplerWrap", samplerWrap.get(), dx12_rhi->GlobalCmdList->CmdList.Get());

	Texture* backbuffer = framebuffers[dx12_rhi->CurrentFrameIndex].get();

	dx12_rhi->GlobalCmdList->CmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	dx12_rhi->GlobalCmdList->CmdList->IASetVertexBuffers(0, 1, &FullScreenVB->view);

	

	std::vector<std::function<void(EDebugVisualization eFS)>> functions;
	functions.push_back([&](EDebugVisualization eFS){
		//raytraced shadow
		DebugPassCB cb;

		if (eFS == EDebugVisualization::SHADOW)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if(eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(-0.75, -0.75, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}
		cb.DebugMode = RAW_COPY;
		RS_Debug->SetConstantValue("DebugPassCB", &cb, dx12_rhi->GlobalCmdList->CmdList.Get());
		RS_Debug->SetTexture("SrcTex", ShadowBuffer.get(), dx12_rhi->GlobalCmdList->CmdList.Get());
		dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);
		RS_Debug->currentDrawCallIndex++;
	});

	functions.push_back([&](EDebugVisualization eFS) {
		// world normal
		DebugPassCB cb;

		if (eFS ==  EDebugVisualization::WORLD_NORMAL)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(-0.25, -0.75, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}
		cb.DebugMode = RAW_COPY;
		RS_Debug->SetConstantValue("DebugPassCB", &cb, dx12_rhi->GlobalCmdList->CmdList.Get());
		RS_Debug->SetTexture("SrcTex", NormalBuffer.get(), dx12_rhi->GlobalCmdList->CmdList.Get());
		dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);
		RS_Debug->currentDrawCallIndex++;
	});

	functions.push_back([&](EDebugVisualization eFS) {
		// geom world normal
		DebugPassCB cb;
		if (eFS == EDebugVisualization::GEO_NORMAL)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if(eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(-0.25, -0.25, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}
		cb.DebugMode = RAW_COPY;
		RS_Debug->SetConstantValue("DebugPassCB", &cb, dx12_rhi->GlobalCmdList->CmdList.Get());
		RS_Debug->SetTexture("SrcTex", GeomNormalBuffer.get(), dx12_rhi->GlobalCmdList->CmdList.Get());
		dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);
		RS_Debug->currentDrawCallIndex++;
	});

	functions.push_back([&](EDebugVisualization eFS) {
		// depth
		DebugPassCB cb;

		if (eFS == EDebugVisualization::DEPTH)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if(eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(0.25, -0.75, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		RS_Debug->SetConstantValue("DebugPassCB", &cb, dx12_rhi->GlobalCmdList->CmdList.Get());
		RS_Debug->SetTexture("SrcTex", DepthBuffer.get(), dx12_rhi->GlobalCmdList->CmdList.Get());
		dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);
		RS_Debug->currentDrawCallIndex++;
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// raw sh
		DebugPassCB cb;

		if (eFS == EDebugVisualization::RAW_SH)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if(eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(0.25, -0.25, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		RS_Debug->SetConstantValue("DebugPassCB", &cb, dx12_rhi->GlobalCmdList->CmdList.Get());
		RS_Debug->SetTexture("SrcTex", GIBufferSH.get(), dx12_rhi->GlobalCmdList->CmdList.Get());
		dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);
		RS_Debug->currentDrawCallIndex++;
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// temporal filtered sh
		DebugPassCB cb;

		if (eFS == EDebugVisualization::TEMPORAL_FILTERED_SH)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(0.25, 0.25, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		RS_Debug->SetConstantValue("DebugPassCB", &cb, dx12_rhi->GlobalCmdList->CmdList.Get());
		RS_Debug->SetTexture("SrcTex", GIBufferSHTemporal[GIBufferWriteIndex].get(), dx12_rhi->GlobalCmdList->CmdList.Get());
		dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);
		RS_Debug->currentDrawCallIndex++;
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// spatial filtered sh
		DebugPassCB cb;

		if (eFS == EDebugVisualization::SPATIAL_FILTERED_SH)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else  if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(0.25, 0.75, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		RS_Debug->SetConstantValue("DebugPassCB", &cb, dx12_rhi->GlobalCmdList->CmdList.Get());
		RS_Debug->SetTexture("SrcTex", FilterIndirectDiffusePingPongSH[0].get(), dx12_rhi->GlobalCmdList->CmdList.Get());
		dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);
		RS_Debug->currentDrawCallIndex++;
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// sh lighting result
		DebugPassCB cb;

		cb.RTSize = glm::vec2(m_width, m_height);
		cb.GIBufferScale = GIBufferScale;
		if (eFS == EDebugVisualization::SH_LIGHTING)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(0.75, 0.75, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = SH_LIGHTING;
		RS_Debug->SetConstantValue("DebugPassCB", &cb, dx12_rhi->GlobalCmdList->CmdList.Get());
		RS_Debug->SetTexture("SrcTex", FilterIndirectDiffusePingPongColor[0].get(), dx12_rhi->GlobalCmdList->CmdList.Get());
		RS_Debug->SetTexture("SrcTexSH", FilterIndirectDiffusePingPongSH[0].get(), dx12_rhi->GlobalCmdList->CmdList.Get());
		RS_Debug->SetTexture("SrcTexNormal", NormalBuffer.get(), dx12_rhi->GlobalCmdList->CmdList.Get());

		dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);
		RS_Debug->currentDrawCallIndex++;
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// albedo
		DebugPassCB cb;

		if (eFS == EDebugVisualization::ALBEDO)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else  if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(0.75, -0.75, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		RS_Debug->SetConstantValue("DebugPassCB", &cb, dx12_rhi->GlobalCmdList->CmdList.Get());
		RS_Debug->SetTexture("SrcTex", AlbedoBuffer.get(), dx12_rhi->GlobalCmdList->CmdList.Get());
		dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);
		RS_Debug->currentDrawCallIndex++;
	});
	
	functions.push_back([&](EDebugVisualization eFS) {
		// velocity
		DebugPassCB cb;

		if (eFS == EDebugVisualization::VELOCITY)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if(eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(0.75, -0.25, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		RS_Debug->SetConstantValue("DebugPassCB", &cb, dx12_rhi->GlobalCmdList->CmdList.Get());
		RS_Debug->SetTexture("SrcTex", VelocityBuffer.get(), dx12_rhi->GlobalCmdList->CmdList.Get());
		dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);
		RS_Debug->currentDrawCallIndex++;
	});
	
	functions.push_back([&](EDebugVisualization eFS) {
		// material
		DebugPassCB cb;
		if (eFS == EDebugVisualization::ROUGNESS_METALLIC)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(0.75, 0.25, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		RS_Debug->SetConstantValue("DebugPassCB", &cb, dx12_rhi->GlobalCmdList->CmdList.Get());
		RS_Debug->SetTexture("SrcTex", MaterialBuffer.get(), dx12_rhi->GlobalCmdList->CmdList.Get());
		dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);
		RS_Debug->currentDrawCallIndex++;
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// reflection
		DebugPassCB cb;


		if (eFS == EDebugVisualization::REFLECTION)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(-0.75, -0.25, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		RS_Debug->SetConstantValue("DebugPassCB", &cb, dx12_rhi->GlobalCmdList->CmdList.Get());
		RS_Debug->SetTexture("SrcTex", SpeculaGIBuffer.get(), dx12_rhi->GlobalCmdList->CmdList.Get());
		dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);
		RS_Debug->currentDrawCallIndex++;
	});


	EDebugVisualization FullScreenVisualize = EDebugVisualization::REFLECTION;

	for (auto& f : functions)
	{
		f(FullscreenDebugBuffer);
	}
}

void MyToyDX12Renderer::LightingPass()
{
	PIXScopedEvent(dx12_rhi->GlobalCmdList->CmdList.Get(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "LightingPass");

	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(LightingBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));

	RS_Lighting->Apply(dx12_rhi->GlobalCmdList->CmdList.Get());

	RS_Lighting->SetSampler("samplerWrap", samplerWrap.get(), dx12_rhi->GlobalCmdList->CmdList.Get());
	RS_Lighting->SetTexture("AlbedoTex", AlbedoBuffer.get(), dx12_rhi->GlobalCmdList->CmdList.Get());
	RS_Lighting->SetTexture("NormalTex", NormalBuffer.get(), dx12_rhi->GlobalCmdList->CmdList.Get());
	RS_Lighting->SetTexture("ShadowTex", ShadowBuffer.get(), dx12_rhi->GlobalCmdList->CmdList.Get());

	RS_Lighting->SetTexture("VelocityTex", VelocityBuffer.get(), dx12_rhi->GlobalCmdList->CmdList.Get());
	RS_Lighting->SetTexture("DepthTex", DepthBuffer.get(), dx12_rhi->GlobalCmdList->CmdList.Get());
	//RS_Lighting->SetTexture("IndirectDiffuseTex", FilterIndirectDiffusePingPong[1].get(), dx12_rhi->CommandList.Get());
	RS_Lighting->SetTexture("GIResultSHTex", FilterIndirectDiffusePingPongSH[0].get(), dx12_rhi->GlobalCmdList->CmdList.Get());
	RS_Lighting->SetTexture("GIResultColorTex", FilterIndirectDiffusePingPongColor[0].get(), dx12_rhi->GlobalCmdList->CmdList.Get());

	LightingParam Param;
	Param.LightDir = glm::vec4(glm::normalize(LightDir), LightIntensity);
	
	Param.RTSize.x = m_width;
	Param.RTSize.y = m_height;

	if(bEnableTAA)
		Param.TAABlendFactor = 0.1;
	else
		Param.TAABlendFactor = 1.0;

	Param.GIBufferScale = GIBufferScale;

	glm::normalize(Param.LightDir);
	RS_Lighting->SetConstantValue("LightingParam", &Param, dx12_rhi->GlobalCmdList->CmdList.Get());


	RS_Lighting->Apply(dx12_rhi->GlobalCmdList->CmdList.Get());

	dx12_rhi->GlobalCmdList->CmdList->OMSetRenderTargets(1, &LightingBuffer->CpuHandleRTV, FALSE, nullptr);
	dx12_rhi->GlobalCmdList->CmdList->RSSetViewports(1, &m_viewport);
	dx12_rhi->GlobalCmdList->CmdList->RSSetScissorRects(1, &m_scissorRect);
	dx12_rhi->GlobalCmdList->CmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	dx12_rhi->GlobalCmdList->CmdList->IASetVertexBuffers(0, 1, &FullScreenVB->view);
	dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(LightingBuffer->resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
}

void MyToyDX12Renderer::TemporalAAPass()
{
	PIXScopedEvent(dx12_rhi->GlobalCmdList->CmdList.Get(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "TemporalAAPass");

	UINT PrevColorBufferIndex = 1 - ColorBufferWriteIndex;
	Texture* ResolveTarget = ColorBuffers[ColorBufferWriteIndex];//framebuffers[dx12_rhi->CurrentFrameIndex].get();

	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(ResolveTarget->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));

	RS_TemporalAA->Apply(dx12_rhi->GlobalCmdList->CmdList.Get());

	RS_TemporalAA->SetSampler("samplerWrap", samplerWrap.get(), dx12_rhi->GlobalCmdList->CmdList.Get());
	RS_TemporalAA->SetTexture("CurrentColorTex", LightingBuffer.get(), dx12_rhi->GlobalCmdList->CmdList.Get());
	Texture* PrevColorBuffer = ColorBuffers[PrevColorBufferIndex];
	RS_TemporalAA->SetTexture("PrevColorTex", PrevColorBuffer, dx12_rhi->GlobalCmdList->CmdList.Get());
	RS_TemporalAA->SetTexture("VelocityTex", VelocityBuffer.get(), dx12_rhi->GlobalCmdList->CmdList.Get());
	RS_TemporalAA->SetTexture("DepthTex", DepthBuffer.get(), dx12_rhi->GlobalCmdList->CmdList.Get());

	TemporalAAParam Param;

	Param.RTSize.x = m_width;
	Param.RTSize.y = m_height;

	if (bEnableTAA)
		Param.TAABlendFactor = 0.1;
	else
		Param.TAABlendFactor = 1.0;

	Param.ClampMode = ClampMode;

	RS_TemporalAA->SetConstantValue("LightingParam", &Param, dx12_rhi->GlobalCmdList->CmdList.Get());
	RS_TemporalAA->Apply(dx12_rhi->GlobalCmdList->CmdList.Get());

	dx12_rhi->GlobalCmdList->CmdList->OMSetRenderTargets(1, &ResolveTarget->CpuHandleRTV, FALSE, nullptr);
	dx12_rhi->GlobalCmdList->CmdList->RSSetViewports(1, &m_viewport);
	dx12_rhi->GlobalCmdList->CmdList->RSSetScissorRects(1, &m_scissorRect);
	dx12_rhi->GlobalCmdList->CmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	dx12_rhi->GlobalCmdList->CmdList->IASetVertexBuffers(0, 1, &FullScreenVB->view);
	dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);

	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(ResolveTarget->resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
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

void MyToyDX12Renderer::OnUpdate()
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

	m_camera.SetTurnSpeed(m_turnSpeed);
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
	
	float timeElapsed = m_timer.GetTotalSeconds();
	timeElapsed *= 0.01f;
	// reflection view param
	RTReflectionViewParam.ViewMatrix = glm::transpose(ViewMat);
	RTReflectionViewParam.InvViewMatrix = glm::transpose(InvViewMat);
	RTReflectionViewParam.ProjMatrix = glm::transpose(ProjMat);
	RTReflectionViewParam.ProjectionParams.x = Far / (Far - Near);
	RTReflectionViewParam.ProjectionParams.y = Near / (Near - Far);
	RTReflectionViewParam.ProjectionParams.z = Near;
	RTReflectionViewParam.ProjectionParams.w = Far;
	RTReflectionViewParam.LightDir = glm::vec4(glm::normalize(LightDir), LightIntensity);
	RTGIViewParam.RandomOffset = glm::vec2(timeElapsed, timeElapsed);
	RTGIViewParam.FrameCounter = FrameCounter;

	// GI view param
	RTGIViewParam.ViewMatrix = glm::transpose(ViewMat);
	RTGIViewParam.InvViewMatrix = glm::transpose(InvViewMat);
	RTGIViewParam.ProjMatrix = glm::transpose(ProjMat);
	RTGIViewParam.ProjectionParams.x = Far / (Far - Near);
	RTGIViewParam.ProjectionParams.y = Near / (Near - Far);
	RTGIViewParam.ProjectionParams.z = Near;
	RTGIViewParam.ProjectionParams.w = Far;
	RTGIViewParam.LightDir = glm::vec4(glm::normalize(LightDir), LightIntensity);
	RTGIViewParam.RandomOffset = glm::vec2(timeElapsed, timeElapsed);
	RTGIViewParam.FrameCounter = FrameCounter;
	
	SpatialFilterCB.ProjectionParams.x = Near;
	SpatialFilterCB.ProjectionParams.y = Far;

	TemporalFilterCB.ProjectionParams.x = Near;
	TemporalFilterCB.ProjectionParams.y = Far;
	TemporalFilterCB.RTSize.x = m_width;
	TemporalFilterCB.RTSize.y = m_height;
	
	FrameCounter++;

	ColorBufferWriteIndex = FrameCounter % 2;

	if (bRecompileShaders)
	{
		RecompileShaders();
		bRecompileShaders = false;
	}
}

// Render the scene.
void MyToyDX12Renderer::OnRender()
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

	TemporalDenoisingPass();

	SpatialDenoisingPass();
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
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &BarrierDesc);

	dx12_rhi->GlobalCmdList->CmdList->OMSetRenderTargets(1, &backbuffer->CpuHandleRTV, FALSE, nullptr);
	CopyPass();

	if(bDebugDraw)
		DebugPass();

	ImGui_ImplDX12_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	bool show_demo_window = true;

	//ImGui::ShowDemoWindow(&show_demo_window);

	char fps[64];
	sprintf(fps, "FPS : %u fps", m_timer.GetFramesPerSecond());

	ImGui::Begin("Hi, Let's traceray!");
	ImGui::Text(fps);

	if (ImGui::Button("Recompile all shaders"))
		bRecompileShaders = true;

	ImGui::Text("Tuning knobs.");
	ImGui::SliderFloat("Camera turn speed", &m_turnSpeed, 0.0f, glm::half_pi<float>()*2);

	ImGui::Checkbox("Enable TemporalAA", &bEnableTAA);
	ImGui::Checkbox("Visualize Buffers", &bDebugDraw);

	
	/*
	enum class EDebugVisualization
	{
		SHADOW,
		WORLD_NORMAL,
		GEO_NORMAL,
		DEPTH,
		RAW_SH,
		TEMPORAL_FILTERED_SH,
		SPATIAL_FILTERED_SH,
		SH_LIGHTING,
		ALBEDO,
		VELOCITY,
		ROUGNESS_METALLIC,
		REFLECTION,
		NO_FULLSCREEN,
	};
	*/
	static ImGuiComboFlags flags = 0;
	const char* items[] = { 
		"SHADOW",
		"WORLD_NORMAL",
		"GEO_NORMAL",
		"DEPTH",
		"RAW_SH",
		"TEMPORAL_FILTERED_SH",
		"SPATIAL_FILTERED_SH",
		"SH_LIGHTING",
		"ALBEDO",
		"VELOCITY",
		"ROUGNESS_METALLIC",
		"REFLECTION",
		"NO_FULLSCREEN",
	};
	static const char* item_current = items[UINT(EDebugVisualization::NO_FULLSCREEN)];
	if (ImGui::BeginCombo("Visualize Full Screen", item_current, flags))
	{
		for (int n = 0; n < IM_ARRAYSIZE(items); n++)
		{
			bool is_selected = (item_current == items[n]);
			if (ImGui::Selectable(items[n], is_selected))\
			{
				item_current = items[n];
				FullscreenDebugBuffer = (EDebugVisualization)n;
			}
			if (is_selected)
			{
				ImGui::SetItemDefaultFocus(); 
			}

		}
		ImGui::EndCombo();
	}

	ImGui::SliderFloat3("Light direction", &LightDir.x, -1.0f, 1.0f);
	ImGui::SliderFloat("Light Brightness", &LightIntensity, 0.0f, 20.0f);

	ImGui::SliderFloat("SponzaRoughness", &SponzaRoughness, 0.0f, 1.0f);
	ImGui::SliderFloat("ShaderBallRoughness", &ShaderBallRoughness, 0.0f, 1.0f);


	ImGui::SliderFloat("IndirectDiffuse Depth Weight Factor", &SpatialFilterCB.IndirectDiffuseWeightFactorDepth, 0.0f, 20.0f);
	ImGui::SliderFloat("IndirectDiffuse Normal Weight Factor", &SpatialFilterCB.IndirectDiffuseWeightFactorNormal, 0.0f, 20.0f);

	ImGui::End();

	ImGui::Render();
	ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), dx12_rhi->GlobalCmdList->CmdList.Get());


	D3D12_RESOURCE_BARRIER BarrierDescPresent = {};
	BarrierDescPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	BarrierDescPresent.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	BarrierDescPresent.Transition.pResource = backbuffer->resource.Get();
	BarrierDescPresent.Transition.Subresource = 0;
	BarrierDescPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	BarrierDescPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &BarrierDescPresent);

	dx12_rhi->CmdQ->ExecuteCommandList(dx12_rhi->GlobalCmdList);

	dx12_rhi->EndFrame();

	PrevViewProjMat = ViewProjMat;
	PrevViewMat = ViewMat;
}

void MyToyDX12Renderer::OnDestroy()
{
	dx12_rhi->CmdQ->WaitGPU();

	ImGui_ImplDX12_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
}

void MyToyDX12Renderer::OnKeyDown(UINT8 key)
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
	case 'R':
		RecompileShaders();
		break;
	default:
		break;
	}

	m_camera.OnKeyDown(key);
}

void MyToyDX12Renderer::OnKeyUp(UINT8 key)
{
	m_camera.OnKeyUp(key);
}

struct ParallelDrawTaskSet : enki::ITaskSet
{
	MyToyDX12Renderer* app;
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

void MyToyDX12Renderer::DrawScene(shared_ptr<Scene> scene, float Roughness, float Metalic)
{
	for (auto& mesh : scene->meshes)
	{
		dx12_rhi->GlobalCmdList->CmdList->IASetIndexBuffer(&mesh->Ib->view);
		dx12_rhi->GlobalCmdList->CmdList->IASetVertexBuffers(0, 1, &mesh->Vb->view);

		for (int i = 0; i < mesh->Draws.size(); i++)
		{
			Mesh::DrawCall& drawcall = mesh->Draws[i];
			MeshDrawConstantBuffer objCB;
			int sizea = sizeof(MeshDrawConstantBuffer);

			objCB.ViewProjectionMatrix = glm::transpose(ViewProjMat);
			objCB.PrevViewProjectionMatrix = glm::transpose(PrevViewProjMat);

			//glm::mat4 m; // Identity matrix
			objCB.WorldMatrix = glm::transpose(mesh->transform);
			objCB.ViewDir.x = m_camera.m_lookDirection.x;
			objCB.ViewDir.y = m_camera.m_lookDirection.y;
			objCB.ViewDir.z = m_camera.m_lookDirection.z;

			objCB.RTSize.x = m_width;
			objCB.RTSize.y = m_height;

			objCB.JitterOffset = JitterOffset;
			objCB.RougnessMetalic.x = Roughness;
			objCB.RougnessMetalic.y = Metalic;

			RS_Mesh->SetConstantValue("ObjParameter", (void*)&objCB, dx12_rhi->GlobalCmdList->CmdList.Get());

			Texture* diffuseTex = drawcall.mat->Diffuse.get();
			/*if (diffuseTex == nullptr)
				diffuseTex = scene->Materials[0]->Diffuse.get();*/
			if (diffuseTex)
				RS_Mesh->SetTexture("diffuseMap", diffuseTex, dx12_rhi->GlobalCmdList->CmdList.Get());


			Texture* normalTex = drawcall.mat->Normal.get();
			/*if (normalTex == nullptr)
				normalTex = scene->Materials[0]->Normal.get();*/
			if (normalTex)
				RS_Mesh->SetTexture("normalMap", normalTex, dx12_rhi->GlobalCmdList->CmdList.Get());


			dx12_rhi->GlobalCmdList->CmdList->DrawIndexedInstanced(drawcall.IndexCount, 1, drawcall.IndexStart, drawcall.VertexBase, 0);

			RS_Mesh->currentDrawCallIndex++;
		}
	}
}

void MyToyDX12Renderer::DrawMeshPass()
{
	PIXScopedEvent(dx12_rhi->GlobalCmdList->CmdList.Get(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "DrawMeshPass");

	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(AlbedoBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(NormalBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(GeomNormalBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(VelocityBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(MaterialBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));

	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(DepthBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE));

	const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
	dx12_rhi->GlobalCmdList->CmdList->ClearRenderTargetView(AlbedoBuffer->CpuHandleRTV, clearColor, 0, nullptr);
	dx12_rhi->GlobalCmdList->CmdList->ClearRenderTargetView(NormalBuffer->CpuHandleRTV, clearColor, 0, nullptr);
	dx12_rhi->GlobalCmdList->CmdList->ClearRenderTargetView(GeomNormalBuffer->CpuHandleRTV, clearColor, 0, nullptr);
	dx12_rhi->GlobalCmdList->CmdList->ClearRenderTargetView(VelocityBuffer->CpuHandleRTV, clearColor, 0, nullptr);
	dx12_rhi->GlobalCmdList->CmdList->ClearDepthStencilView(DepthBuffer->CpuHandleDSV, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
	
	ID3D12DescriptorHeap* ppHeaps[] = { dx12_rhi->SRVCBVDescriptorHeapShaderVisible->DH.Get(), dx12_rhi->SamplerDescriptorHeapShaderVisible->DH.Get() };
	dx12_rhi->GlobalCmdList->CmdList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

	if (!bMultiThreadRendering)
	{
		const D3D12_CPU_DESCRIPTOR_HANDLE Rendertargets[] = { AlbedoBuffer->CpuHandleRTV, NormalBuffer->CpuHandleRTV, GeomNormalBuffer->CpuHandleRTV, VelocityBuffer->CpuHandleRTV, MaterialBuffer->CpuHandleRTV };

		dx12_rhi->GlobalCmdList->CmdList->OMSetRenderTargets(RS_Mesh->graphicsPSODesc.NumRenderTargets, Rendertargets, FALSE, &DepthBuffer->CpuHandleDSV);

		RS_Mesh->Apply(dx12_rhi->GlobalCmdList->CmdList.Get());
		RS_Mesh->SetSampler("samplerWrap", samplerWrap.get(), dx12_rhi->GlobalCmdList->CmdList.Get());

		dx12_rhi->GlobalCmdList->CmdList->RSSetViewports(1, &m_viewport);
		dx12_rhi->GlobalCmdList->CmdList->RSSetScissorRects(1, &m_scissorRect);
		dx12_rhi->GlobalCmdList->CmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		RS_Mesh->currentDrawCallIndex = 0;
		
		DrawScene(Sponza, SponzaRoughness, 0);
		DrawScene(ShaderBall, ShaderBallRoughness, 1);
	}
	else
	{
		//UINT NumThread = dx12_rhi->NumDrawMeshCommandList;
		//UINT RemainDraw = mesh->Draws.size();
		//UINT NumDrawThread = mesh->Draws.size() / (NumThread);
		//UINT StartIndex = 0;

		//vector<ThreadDescriptorHeapPool> vecDHPool;
		//vecDHPool.resize(NumThread);

		//vector<ParallelDrawTaskSet> vecTask;
		//vecTask.resize(NumThread);

		//for (int i = 0; i < NumThread; i++)
		//{
		//	UINT ThisDraw = NumDrawThread;
		//	
		//	if (i == NumThread - 1)
		//		ThisDraw = RemainDraw;

		//	ThreadDescriptorHeapPool& DHPool = vecDHPool[i];
		//	DHPool.AllocPool(RS_Mesh->GetGraphicsBindingDHSize()*ThisDraw);

		//	// draw
		//	ParallelDrawTaskSet& task = vecTask[i];
		//	task.app = this;
		//	task.StartIndex = StartIndex;
		//	task.ThisDraw = ThisDraw;
		//	task.ThreadIndex = i;
		//	task.DHPool = &DHPool;

		//	g_TS.AddTaskSetToPipe(&task);

		//	RemainDraw -= ThisDraw;
		//	StartIndex += ThisDraw;
		//}

		//g_TS.WaitforAll();


		//UINT NumCL = dx12_rhi->NumDrawMeshCommandList;
		//vector< ID3D12CommandList*> vecCL;

		//for (int i = 0; i < NumCL; i++)
		//	vecCL.push_back(dx12_rhi->DrawMeshCommandList[i].Get());

	}
	
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(AlbedoBuffer->resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(NormalBuffer->resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(GeomNormalBuffer->resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(VelocityBuffer->resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(MaterialBuffer->resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));


	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(DepthBuffer->resource.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
}

void MyToyDX12Renderer::RecordDraw (UINT StartIndex, UINT NumDraw, UINT CLIndex, ThreadDescriptorHeapPool* DHPool)
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

void MyToyDX12Renderer::SpatialDenoisingPass()
{
	PIXScopedEvent(dx12_rhi->GlobalCmdList->CmdList.Get(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "SpatialDenoisingPass");

	UINT WriteIndex = 0;
	UINT ReadIndex = 1;
	for (int i = 0; i < 4; i++)
	{
		WriteIndex = 1 - WriteIndex; // 1
		ReadIndex = 1 - WriteIndex; // 0

		dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(FilterIndirectDiffusePingPongSH[WriteIndex]->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
		dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(FilterIndirectDiffusePingPongColor[WriteIndex]->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

		RS_SpatialDenoisingFilter->Apply(dx12_rhi->GlobalCmdList->CmdList.Get());

		RS_SpatialDenoisingFilter->SetTexture("DepthTex", DepthBuffer.get(), dx12_rhi->GlobalCmdList->CmdList.Get());
		RS_SpatialDenoisingFilter->SetTexture("GeoNormalTex", GeomNormalBuffer.get(), dx12_rhi->GlobalCmdList->CmdList.Get());
		RS_SpatialDenoisingFilter->SetTexture("InGIResultSHTex", FilterIndirectDiffusePingPongSH[ReadIndex].get(), dx12_rhi->GlobalCmdList->CmdList.Get());
		RS_SpatialDenoisingFilter->SetTexture("InGIResultColorTex", FilterIndirectDiffusePingPongColor[ReadIndex].get(), dx12_rhi->GlobalCmdList->CmdList.Get());

		RS_SpatialDenoisingFilter->SetUAV("OutGIResultSH", FilterIndirectDiffusePingPongSH[WriteIndex].get(), dx12_rhi->GlobalCmdList->CmdList.Get());
		RS_SpatialDenoisingFilter->SetUAV("OutGIResultColor", FilterIndirectDiffusePingPongColor[WriteIndex].get(), dx12_rhi->GlobalCmdList->CmdList.Get());

		SpatialFilterCB.Iteration = i;
		RS_SpatialDenoisingFilter->SetConstantValue("SpatialFilterConstant", &SpatialFilterCB, dx12_rhi->GlobalCmdList->CmdList.Get());

		UINT WidthGI = m_width / GIBufferScale;
		UINT HeightGI = m_height / GIBufferScale;

		dx12_rhi->GlobalCmdList->CmdList->Dispatch(WidthGI / 32, HeightGI / 32 + 1 , 1);

		dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(FilterIndirectDiffusePingPongSH[WriteIndex]->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
		dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(FilterIndirectDiffusePingPongColor[WriteIndex]->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
	}
}

void MyToyDX12Renderer::TemporalDenoisingPass()
{
	PIXScopedEvent(dx12_rhi->GlobalCmdList->CmdList.Get(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "TemporalDenoisingPass");

	// GIBufferSH : full scale
	// FilterIndirectDiffusePingPongSH : 3x3 downsample
	GIBufferWriteIndex = 1 - GIBufferWriteIndex;
	UINT WriteIndex = GIBufferWriteIndex;
	UINT ReadIndex = 1 - WriteIndex;

	// first pass
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(FilterIndirectDiffusePingPongSH[0]->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(FilterIndirectDiffusePingPongColor[0]->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(GIBufferSHTemporal[WriteIndex]->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(GIBufferColorTemporal[WriteIndex]->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

	RS_TemporalDenoisingFilter->Apply(dx12_rhi->GlobalCmdList->CmdList.Get());

	RS_TemporalDenoisingFilter->SetTexture("DepthTex", DepthBuffer.get(), dx12_rhi->GlobalCmdList->CmdList.Get());
	RS_TemporalDenoisingFilter->SetTexture("GeoNormalTex", GeomNormalBuffer.get(), dx12_rhi->GlobalCmdList->CmdList.Get());
	RS_TemporalDenoisingFilter->SetTexture("InGIResultSHTex", GIBufferSH.get(), dx12_rhi->GlobalCmdList->CmdList.Get());
	RS_TemporalDenoisingFilter->SetTexture("InGIResultColorTex", GIBufferColor.get(), dx12_rhi->GlobalCmdList->CmdList.Get());
	RS_TemporalDenoisingFilter->SetTexture("InGIResultSHTexPrev", GIBufferSHTemporal[ReadIndex].get(), dx12_rhi->GlobalCmdList->CmdList.Get());
	RS_TemporalDenoisingFilter->SetTexture("InGIResultColorTexPrev", GIBufferColorTemporal[ReadIndex].get(), dx12_rhi->GlobalCmdList->CmdList.Get());
	RS_TemporalDenoisingFilter->SetTexture("VelocityTex", VelocityBuffer.get(), dx12_rhi->GlobalCmdList->CmdList.Get());

	RS_TemporalDenoisingFilter->SetUAV("OutGIResultSH", GIBufferSHTemporal[WriteIndex].get(), dx12_rhi->GlobalCmdList->CmdList.Get());
	RS_TemporalDenoisingFilter->SetUAV("OutGIResultColor", GIBufferColorTemporal[WriteIndex].get(), dx12_rhi->GlobalCmdList->CmdList.Get());
	RS_TemporalDenoisingFilter->SetUAV("OutGIResultSHDS", FilterIndirectDiffusePingPongSH[0].get(), dx12_rhi->GlobalCmdList->CmdList.Get());
	RS_TemporalDenoisingFilter->SetUAV("OutGIResultColorDS", FilterIndirectDiffusePingPongColor[0].get(), dx12_rhi->GlobalCmdList->CmdList.Get());

	RS_TemporalDenoisingFilter->SetConstantValue("TemporalFilterConstant", &TemporalFilterCB, dx12_rhi->GlobalCmdList->CmdList.Get());

	dx12_rhi->GlobalCmdList->CmdList->Dispatch(m_width / 15 , m_height / 15, 1);

	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(FilterIndirectDiffusePingPongSH[0]->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(FilterIndirectDiffusePingPongColor[0]->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(GIBufferSHTemporal[WriteIndex]->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(GIBufferColorTemporal[WriteIndex]->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
}

void AddMeshToVec(vector<shared_ptr<RTAS>>& vecBLAS, shared_ptr<Scene> scene)
{
	for (auto& mesh : scene->meshes)
	{
		shared_ptr<RTAS> blas = mesh->CreateBLAS();
		blas->mesh = mesh;
		if (blas == nullptr)
		{
			continue;
		}
		vecBLAS.push_back(blas);
	}
}

void MyToyDX12Renderer::RecompileShaders()
{
	dx12_rhi->CmdQ->WaitGPU();

	InitRTPSO();
	InitSpatialDenoisingPass();
	InitTemporalDenoisingPass();
	InitDrawMeshRS();
	InitCopyPass();
	InitDebugPass();
	InitLightingPass();
	InitTemporalAAPass();
}

void MyToyDX12Renderer::InitRaytracing()
{
	UINT NumTotalMesh = Sponza->meshes.size() + ShaderBall->meshes.size();
	vecBLAS.reserve(NumTotalMesh);

	AddMeshToVec(vecBLAS, Sponza);
	AddMeshToVec(vecBLAS, ShaderBall);


	// TODO : per model world matrix
	TLAS = dx12_rhi->CreateTLAS(vecBLAS);
	
	InitRTPSO();

	InstancePropertyBuffer = dx12_rhi->CreateBuffer(sizeof(InstanceProperty) * 500);

	uint8_t* pData;
	InstancePropertyBuffer->resource->Map(0, nullptr, (void**)&pData);

	for (auto& m : vecBLAS)
	{
		glm::mat4x4 mat = glm::transpose(m->mesh->transform);
		memcpy(pData, &mat, sizeof(glm::mat4x4));
		pData += sizeof(InstanceProperty);
	}

	InstancePropertyBuffer->resource->Unmap(0, nullptr);
}

void MyToyDX12Renderer::InitRTPSO()
{
	// create shadow rtpso
	{
		shared_ptr<RTPipelineStateObject> TEMP_PSO_RT_SHADOW = shared_ptr<RTPipelineStateObject>(new RTPipelineStateObject);

		TEMP_PSO_RT_SHADOW->NumInstance = vecBLAS.size();// scene->meshes.size(); // important for cbv allocation & shadertable size.

		// new interface
		TEMP_PSO_RT_SHADOW->AddHitGroup("HitGroup", "chs", "anyhit");
		TEMP_PSO_RT_SHADOW->AddShader("rayGen", RTPipelineStateObject::RAYGEN);
		TEMP_PSO_RT_SHADOW->BindUAV("rayGen", "ShadowResult", 0);
		TEMP_PSO_RT_SHADOW->BindSRV("rayGen", "gRtScene", 0);
		TEMP_PSO_RT_SHADOW->BindSRV("rayGen", "DepthTex", 1);
		TEMP_PSO_RT_SHADOW->BindSRV("rayGen", "WorldNormalTex", 2);
		TEMP_PSO_RT_SHADOW->BindSRV("rayGen", "AlbedoTex", 3);

		TEMP_PSO_RT_SHADOW->BindCBV("rayGen", "ViewParameter", 0, sizeof(RTShadowViewParamCB), 1);
		TEMP_PSO_RT_SHADOW->BindSampler("rayGen", "samplerWrap", 0);

		TEMP_PSO_RT_SHADOW->AddShader("miss", RTPipelineStateObject::MISS);
		TEMP_PSO_RT_SHADOW->AddShader("chs", RTPipelineStateObject::HIT);
		TEMP_PSO_RT_SHADOW->AddShader("anyhit", RTPipelineStateObject::ANYHIT);


		TEMP_PSO_RT_SHADOW->MaxRecursion = 1;
		TEMP_PSO_RT_SHADOW->MaxAttributeSizeInBytes = sizeof(float) * 2;
		TEMP_PSO_RT_SHADOW->MaxPayloadSizeInBytes = sizeof(float) * 1;

		bool bSuccess = TEMP_PSO_RT_SHADOW->InitRS("Shaders\\RaytracedShadow.hlsl");
		if (bSuccess)
		{
			PSO_RT_SHADOW = TEMP_PSO_RT_SHADOW;
		}
	}

	// create reflection rtpso
	{
		shared_ptr<RTPipelineStateObject> TEMP_PSO_RT_REFLECTION = shared_ptr<RTPipelineStateObject>(new RTPipelineStateObject);
		TEMP_PSO_RT_REFLECTION->NumInstance = vecBLAS.size();// scene->meshes.size();

		TEMP_PSO_RT_REFLECTION->AddHitGroup("HitGroup", "chs", "");
		TEMP_PSO_RT_REFLECTION->BindUAV("rayGen", "ReflectionResult", 0);

		TEMP_PSO_RT_REFLECTION->AddShader("rayGen", RTPipelineStateObject::RAYGEN);
		TEMP_PSO_RT_REFLECTION->BindSRV("rayGen", "gRtScene", 0);
		TEMP_PSO_RT_REFLECTION->BindSRV("rayGen", "DepthTex", 1);
		TEMP_PSO_RT_REFLECTION->BindSRV("rayGen", "GeoNormalTex", 2);
		TEMP_PSO_RT_REFLECTION->BindSRV("rayGen", "RougnessMetallicTex", 6);
		TEMP_PSO_RT_REFLECTION->BindSRV("rayGen", "BlueNoiseTex", 7);
		TEMP_PSO_RT_REFLECTION->BindSRV("rayGen", "WorldNormalTex", 8);


		TEMP_PSO_RT_REFLECTION->BindCBV("rayGen", "ViewParameter", 0, sizeof(RTReflectionViewParam), 1);
		TEMP_PSO_RT_REFLECTION->BindSampler("rayGen", "samplerWrap", 0);

		TEMP_PSO_RT_REFLECTION->AddShader("miss", RTPipelineStateObject::MISS);

		TEMP_PSO_RT_REFLECTION->AddShader("chs", RTPipelineStateObject::HIT);
		TEMP_PSO_RT_REFLECTION->BindSRV("chs", "vertices", 3);
		TEMP_PSO_RT_REFLECTION->BindSRV("chs", "indices", 4);
		TEMP_PSO_RT_REFLECTION->BindSRV("chs", "AlbedoTex", 5);

		TEMP_PSO_RT_REFLECTION->BindSampler("chs", "samplerWrap", 0);


		TEMP_PSO_RT_REFLECTION->MaxRecursion = 1;
		TEMP_PSO_RT_REFLECTION->MaxAttributeSizeInBytes = sizeof(float) * 2;
		TEMP_PSO_RT_REFLECTION->MaxPayloadSizeInBytes = sizeof(float) * 4;

		bool bSuccess = TEMP_PSO_RT_REFLECTION->InitRS("Shaders\\RaytracedReflection.hlsl");

		if (bSuccess)
		{
			PSO_RT_REFLECTION = TEMP_PSO_RT_REFLECTION;
		}
	}
	// gi rtpso
	{
		shared_ptr<RTPipelineStateObject> TEMP_PSO_RT_GI = shared_ptr<RTPipelineStateObject>(new RTPipelineStateObject);
		TEMP_PSO_RT_GI->NumInstance = vecBLAS.size();// scene->meshes.size();

		TEMP_PSO_RT_GI->AddHitGroup("HitGroup", "chs", "");

		TEMP_PSO_RT_GI->AddShader("rayGen", RTPipelineStateObject::RAYGEN);
		TEMP_PSO_RT_GI->BindUAV("rayGen", "GIResultSH", 0);
		TEMP_PSO_RT_GI->BindUAV("rayGen", "GIResultColor", 1);


		TEMP_PSO_RT_GI->BindSRV("rayGen", "gRtScene", 0);
		TEMP_PSO_RT_GI->BindSRV("rayGen", "DepthTex", 1);
		TEMP_PSO_RT_GI->BindSRV("rayGen", "WorldNormalTex", 2);
		TEMP_PSO_RT_GI->BindCBV("rayGen", "ViewParameter", 0, sizeof(RTGIViewParam), 1);
		TEMP_PSO_RT_GI->BindSampler("rayGen", "samplerWrap", 0);
		TEMP_PSO_RT_GI->BindSRV("rayGen", "BlueNoiseTex", 7);

		TEMP_PSO_RT_GI->AddShader("miss", RTPipelineStateObject::MISS);

		TEMP_PSO_RT_GI->AddShader("chs", RTPipelineStateObject::HIT);
		TEMP_PSO_RT_GI->BindSRV("chs", "vertices", 3);
		TEMP_PSO_RT_GI->BindSRV("chs", "indices", 4);
		TEMP_PSO_RT_GI->BindSRV("chs", "AlbedoTex", 5);
		TEMP_PSO_RT_GI->BindSRV("chs", "InstanceProperty", 6);
		TEMP_PSO_RT_GI->BindSampler("chs", "samplerWrap", 0);


		TEMP_PSO_RT_GI->MaxRecursion = 1;
		TEMP_PSO_RT_GI->MaxAttributeSizeInBytes = sizeof(float) * 2;

		TEMP_PSO_RT_GI->MaxPayloadSizeInBytes = sizeof(float) * 10;

		bool bSuccess = TEMP_PSO_RT_GI->InitRS("Shaders\\RaytracedGI.hlsl");

		if (bSuccess)
		{
			PSO_RT_GI = TEMP_PSO_RT_GI;
		}
	}
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
void MyToyDX12Renderer::RaytraceShadowPass()
{
	PIXScopedEvent(dx12_rhi->GlobalCmdList->CmdList.Get(), PIX_COLOR(rand()%255, rand() % 255, rand() % 255), "RaytraceShadowPass");

	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(ShadowBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

	PSO_RT_SHADOW->BeginShaderTable();

	int i = 0;
	for (auto&as : vecBLAS)
	{
		PSO_RT_SHADOW->SetHitProgram(i, "chs");
		i++;
	}

	PSO_RT_SHADOW->SetUAV("rayGen", "ShadowResult", ShadowBuffer->GpuHandleUAV);
	PSO_RT_SHADOW->SetSRV("rayGen", "gRtScene", TLAS->GPUHandle);
	PSO_RT_SHADOW->SetSRV("rayGen", "DepthTex", DepthBuffer->GpuHandleSRV);
	PSO_RT_SHADOW->SetSRV("rayGen", "WorldNormalTex", GeomNormalBuffer->GpuHandleSRV);
	PSO_RT_SHADOW->SetSRV("rayGen", "AlbedoTex", AlbedoBuffer->GpuHandleSRV);


	PSO_RT_SHADOW->SetCBVValue("rayGen", "ViewParameter", &RTShadowViewParam, sizeof(RTShadowViewParamCB));
	PSO_RT_SHADOW->SetSampler("rayGen", "samplerWrap", samplerWrap.get());

	//PSO_RT_SHADOW->SetHitProgram("HitGroup", 0); // this pass use only 1 hit program

	PSO_RT_SHADOW->EndShaderTable();

	PSO_RT_SHADOW->Apply(m_width, m_height);

	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(ShadowBuffer->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
}

void MyToyDX12Renderer::RaytraceReflectionPass()
{
	PIXScopedEvent(dx12_rhi->GlobalCmdList->CmdList.Get(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "RaytraceReflectionPass");

	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(SpeculaGIBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

	PSO_RT_REFLECTION->BeginShaderTable();

	PSO_RT_REFLECTION->SetUAV("rayGen", "ReflectionResult", SpeculaGIBuffer->GpuHandleUAV);
	PSO_RT_REFLECTION->SetSRV("rayGen", "gRtScene", TLAS->GPUHandle);
	PSO_RT_REFLECTION->SetSRV("rayGen", "DepthTex", DepthBuffer->GpuHandleSRV);
	PSO_RT_REFLECTION->SetSRV("rayGen", "GeoNormalTex", GeomNormalBuffer->GpuHandleSRV);
	PSO_RT_REFLECTION->SetSRV("rayGen", "RougnessMetallicTex", MaterialBuffer->GpuHandleSRV);
	PSO_RT_REFLECTION->SetSRV("rayGen", "BlueNoiseTex", BlueNoiseTex->GpuHandleSRV);
	PSO_RT_REFLECTION->SetSRV("rayGen", "WorldNormalTex", NormalBuffer->GpuHandleSRV);

	PSO_RT_REFLECTION->SetCBVValue("rayGen", "ViewParameter", &RTReflectionViewParam, sizeof(RTReflectionViewParamCB));
	PSO_RT_REFLECTION->SetSampler("rayGen", "samplerWrap", samplerWrap.get());


	//for (int i = 0; i < scene->meshes.size(); i++)
	int i = 0;
	for(auto&as : vecBLAS)
	{
		auto& mesh = as->mesh;

		PSO_RT_REFLECTION->SetHitProgram(i, "chs");

		PSO_RT_REFLECTION->ResetHitProgramBinding("chs", i, 4);
		Texture* diffuseTex = mesh->Draws[0].mat->Diffuse.get();
		
		if (!diffuseTex)
			diffuseTex = DefaultWhiteTex.get();
		
		PSO_RT_REFLECTION->AddHitProgramDescriptor("chs", mesh->Vb->GpuHandleSRV, i);
		PSO_RT_REFLECTION->AddHitProgramDescriptor("chs", mesh->Ib->GpuHandleSRV, i);
		PSO_RT_REFLECTION->AddHitProgramDescriptor("chs", diffuseTex->GpuHandleSRV, i);
		PSO_RT_REFLECTION->AddHitProgramDescriptor("chs", samplerWrap->GpuHandle, i);
		i++;
	}

	PSO_RT_REFLECTION->EndShaderTable();

	PSO_RT_REFLECTION->Apply(m_width, m_height);

	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(SpeculaGIBuffer->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
	PIXEndEvent();
}

void MyToyDX12Renderer::RaytraceGIPass()
{
	PIXScopedEvent(dx12_rhi->GlobalCmdList->CmdList.Get(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "RaytraceGIPass");

	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(GIBufferSH->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(GIBufferColor->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));


	PSO_RT_GI->BeginShaderTable();

	PSO_RT_GI->SetUAV("rayGen", "GIResultSH", GIBufferSH->GpuHandleUAV);
	PSO_RT_GI->SetUAV("rayGen", "GIResultColor", GIBufferColor->GpuHandleUAV);

	PSO_RT_GI->SetSRV("rayGen", "gRtScene", TLAS->GPUHandle);
	PSO_RT_GI->SetSRV("rayGen", "DepthTex", DepthBuffer->GpuHandleSRV);
	PSO_RT_GI->SetSRV("rayGen", "WorldNormalTex", GeomNormalBuffer->GpuHandleSRV);
	PSO_RT_GI->SetSRV("rayGen", "BlueNoiseTex", BlueNoiseTex->GpuHandleSRV);

	PSO_RT_GI->SetCBVValue("rayGen", "ViewParameter", &RTGIViewParam, sizeof(RTGIViewParamCB));
	PSO_RT_GI->SetSampler("rayGen", "samplerWrap", samplerWrap.get());

	int i = 0;
	for(auto&as : vecBLAS)
	{
		auto& mesh = as->mesh;

		PSO_RT_GI->SetHitProgram(i, "chs");

		PSO_RT_GI->ResetHitProgramBinding("chs", i, 5);
		Texture* diffuseTex = mesh->Draws[0].mat->Diffuse.get();
		if (!diffuseTex)
			diffuseTex = DefaultWhiteTex.get();
		PSO_RT_GI->AddHitProgramDescriptor("chs", mesh->Vb->GpuHandleSRV, i);
		PSO_RT_GI->AddHitProgramDescriptor("chs", mesh->Ib->GpuHandleSRV, i);
		PSO_RT_GI->AddHitProgramDescriptor("chs", diffuseTex->GpuHandleSRV, i);
		PSO_RT_GI->AddHitProgramDescriptor("chs", InstancePropertyBuffer->GpuHandleSRV, i);
		PSO_RT_GI->AddHitProgramDescriptor("chs", samplerWrap->GpuHandle, i);

		i++;
	}

	PSO_RT_GI->EndShaderTable();


	PSO_RT_GI->Apply(m_width, m_height);

	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(GIBufferSH->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(GIBufferColor->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
}
