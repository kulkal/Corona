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
#include "Corona.h"
#include <dxcapi.use.h>
//#include <dxcapi.h>
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
#include "GFSDK_Aftermath/include/GFSDK_Aftermath.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"
#include "imGuIZMO.h"



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




Corona::Corona(UINT width, UINT height, std::wstring name) :
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

Corona::~Corona()
{

}


void Corona::OnInit()
{
	//_CrtSetBreakAlloc(4207117);

	CoInitialize(NULL);

	g_TS.Initialize(8);

	m_camera.Init({ 458, 781, 185 });
	m_camera.SetMoveSpeed(200);

	LoadPipeline();
	LoadAssets();
}

void Corona::LoadPipeline()
{
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

void Corona::LoadAssets()
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
	InitBloomPass();
	InitGenMipSpecularGIPass();

	InitRTPSO();

	
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
		D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);
	ColorBuffer0->MakeRTV();

	NAME_D3D12_OBJECT(ColorBuffer0->resource);

	ColorBuffer1 = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
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
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1, glm::vec4(0.0f, -0.1f, 0.0f, 0.0f));
	NormalBuffer->MakeRTV();

	NAME_D3D12_OBJECT(NormalBuffer->resource);

	// geometry world normal
	GeomNormalBuffer = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1, glm::vec4(0.0f, -0.1f, 0.0f, 0.0f));
	GeomNormalBuffer->MakeRTV();

	NAME_D3D12_OBJECT(GeomNormalBuffer->resource);

	// shadow result
	ShadowBuffer = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R8G8B8A8_UNORM,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);

	NAME_D3D12_OBJECT(ShadowBuffer->resource);

	// refleciton result
	SpeculaGIBufferRaw = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);

	NAME_D3D12_OBJECT(SpeculaGIBufferRaw->resource);

	SpeculaGIBufferTemporal[0] = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);

	NAME_D3D12_OBJECT(SpeculaGIBufferTemporal[0]->resource);

	SpeculaGIBufferTemporal[1] = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);

	NAME_D3D12_OBJECT(SpeculaGIBufferTemporal[1]->resource);

	SpeculaGIBufferSpatial[0] = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width/3, m_height/3, 1);

	NAME_D3D12_OBJECT(SpeculaGIBufferSpatial[0]->resource);

	SpeculaGIBufferSpatial[1] = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width / 3, m_height / 3, 1);

	NAME_D3D12_OBJECT(SpeculaGIBufferSpatial[1]->resource);

	// specular gi mips
	SpeculaGIBufferMip[0] = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width / 2, m_height / 2, 1);

	NAME_D3D12_OBJECT(SpeculaGIBufferMip[0]->resource);

	SpeculaGIBufferMip[1] = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width / 4, m_height / 4, 1);

	NAME_D3D12_OBJECT(SpeculaGIBufferMip[1]->resource);


	SpeculaGIBufferMip[2] = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width / 8, m_height / 8, 1);

	NAME_D3D12_OBJECT(SpeculaGIBufferMip[2]->resource);


	SpeculaGIBufferMip[3] = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width / 16, m_height / 16, 1);

	NAME_D3D12_OBJECT(SpeculaGIBufferMip[3]->resource);

	// diffuse gi

	DiffuseGISHRaw = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);

	NAME_D3D12_OBJECT(DiffuseGISHRaw->resource);

	DiffuseGICoCgRaw = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);

	NAME_D3D12_OBJECT(DiffuseGICoCgRaw->resource);

	// gi result sh
	DiffuseGISHTemporal[0] = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);

	NAME_D3D12_OBJECT(DiffuseGISHTemporal[0]->resource);

	DiffuseGISHTemporal[1] = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);

	NAME_D3D12_OBJECT(DiffuseGISHTemporal[1]->resource);

	// gi result color
	DiffuseGICoCgTemporal[0] = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);

	NAME_D3D12_OBJECT(DiffuseGICoCgTemporal[0]->resource);

	DiffuseGICoCgTemporal[1] = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);

	NAME_D3D12_OBJECT(DiffuseGICoCgTemporal[1]->resource);

	// albedo
	AlbedoBuffer = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R8G8B8A8_UNORM,
		D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);
	AlbedoBuffer->MakeRTV();

	NAME_D3D12_OBJECT(AlbedoBuffer->resource);

	// velocity
	VelocityBuffer = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1, glm::vec4(0.0f, 0.0f, 0.0f, 0.0f));
	VelocityBuffer->MakeRTV();
	NAME_D3D12_OBJECT(VelocityBuffer->resource);

	// pbr material
	RoughnessMetalicBuffer = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R8G8B8A8_UNORM,
		D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1, glm::vec4(0.001f, 0.0f, 0.0f, 0.0f ));
	RoughnessMetalicBuffer->MakeRTV();

	NAME_D3D12_OBJECT(RoughnessMetalicBuffer->resource);

	// depth 
	DepthBuffer = dx12_rhi->CreateTexture2D(DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);
	DepthBuffer->MakeDSV();
	NAME_D3D12_OBJECT(DepthBuffer->resource);

	DefaultWhiteTex = dx12_rhi->CreateTextureFromFile(L"assets/default/default_white.png", false);
	DefaultBlackTex = dx12_rhi->CreateTextureFromFile(L"assets/default/default_black.png", false);
	DefaultNormalTex = dx12_rhi->CreateTextureFromFile(L"assets/default/default_normal.png", true);
	DefaultRougnessTex = dx12_rhi->CreateTextureFromFile(L"assets/default/default_roughness.png", true);

	Sponza = LoadModel("assets/Sponza/Sponza.fbx");

	ShaderBall = LoadModel("assets/shaderball/shaderBall.fbx");

	glm::mat4x4 scaleMat = glm::scale(glm::vec3(2.5, 2.5, 2.5));
	glm::mat4x4 translatemat = glm::translate(glm::vec3(-150, 20, 0));
	ShaderBall->SetTransform(scaleMat* translatemat );
	
	//Buddha = LoadModel("buddha/buddha.obj");

	/*glm::mat4x4 buddhaTM = glm::scale(vec3(100, 100, 100));
	Buddha->SetTransform(buddhaTM);*/

	// Describe and create a sampler.
	{
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

	}

	{
		D3D12_SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
		samplerDesc.MipLODBias = -1.0f;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;

		samplerBilinearWrap = dx12_rhi->CreateSampler(samplerDesc);

	}
	InitRaytracingData();

}



shared_ptr<Scene> Corona::LoadModel(string fileName)
{
	map<wstring, wstring> SponzaRoughnessMap = {
	{L"Background_Albedo", L"Background_Roughness"},
	{L"ChainTexture_Albedo", L"ChainTexture_Roughness"},
	{L"Lion_Albedo", L"Lion_Roughness"},
	{L"Sponza_Arch_diffuse", L"Sponza_Arch_roughness"},
	{L"Sponza_Bricks_a_Albedo", L"Sponza_Bricks_a_Roughness"},
	{L"Sponza_Ceiling_diffuse", L"Sponza_Ceiling_roughness"},
	{L"Sponza_Column_a_diffuse", L"Sponza_Column_a_roughness"},
	{L"Sponza_Column_b_diffuse", L"Sponza_Column_b_roughness"},
	{L"Sponza_Column_c_diffuse", L"Sponza_Column_c_roughness"},
	{L"Sponza_Curtain_Blue_diffuse", L"Sponza_Curtain_roughness"},
	{L"Sponza_Curtain_Green_diffuse", L"Sponza_Curtain_roughness"},
	{L"Sponza_Curtain_Red_diffuse", L"Sponza_Curtain_roughness"},
	{L"Sponza_Details_diffuse", L"Sponza_Details_roughness"},
	{L"Sponza_Fabric_Blue_diffuse", L"Sponza_Fabric_roughness"},
	{L"Sponza_Fabric_Green_diffuse", L"Sponza_Fabric_roughness"},
	{L"Sponza_Fabric_Red_diffuse", L"Sponza_Fabric_roughness"},
	{L"Sponza_FlagPole_diffuse", L"Sponza_FlagPole_roughness"},
	{L"Sponza_Floor_diffuse", L"Sponza_Floor_roughness"},
	{L"Sponza_Roof_diffuse", L"Sponza_Roof_roughness"},
	{L"Sponza_Thorn_diffuse", L"Sponza_Thorn_roughness"},
	{L"Vase_diffuse", L"Vase_roughness"},
	{L"VaseHanging_diffuse", L"VaseHanging_roughness"},
	{L"VasePlant_diffuse", L"VasePlant_roughness"},
	{L"VaseRound_diffuse", L"VaseRound_roughness"}
	};

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
		//shared_ptr<Material> mat = shared_ptr<Material>(new Material);
		Material* mat = new Material;
		wstring wDiffuseTex;
		wstring wNormalTex;
		wstring wRoughnessTex;
		wstring wMetallicTex;


		aiString diffuseTexPath;
		aiString normalMapPath;
		aiString rougnessMapPath;
		aiString metallicMapPath;


		if (aiMat.GetTexture(aiTextureType_DIFFUSE, 0, &diffuseTexPath) == aiReturn_SUCCESS)
			wDiffuseTex = GetFileName(AnsiToWString(diffuseTexPath.C_Str()).c_str());
		if (wDiffuseTex.length() != 0)
		{
			mat->Diffuse = dx12_rhi->CreateTextureFromFile(dir + wDiffuseTex, false);
		}

		if (!mat->Diffuse)
			mat->Diffuse = DefaultWhiteTex;
		
		if (aiMat.GetTexture(aiTextureType_NORMALS, 0, &normalMapPath) == aiReturn_SUCCESS
			|| aiMat.GetTexture(aiTextureType_HEIGHT, 0, &normalMapPath) == aiReturn_SUCCESS)
			wNormalTex = GetFileName(AnsiToWString(normalMapPath.C_Str()).c_str());

		if (wNormalTex.length() != 0)
		{
			mat->Normal = dx12_rhi->CreateTextureFromFile(dir + wNormalTex, true);
		}

		if (!mat->Normal)
			mat->Normal = DefaultNormalTex;

		
		// aiTextureType_HEIGHT is normal in sponza
		// aiTextureType_AMBIENT is metallic in sponza

		if (aiMat.GetTexture(aiTextureType_AMBIENT, 0, &metallicMapPath) == aiReturn_SUCCESS)
			wMetallicTex = GetFileName(AnsiToWString(metallicMapPath.C_Str()).c_str());
		if (wMetallicTex.length() != 0)
		{
			mat->Metallic = dx12_rhi->CreateTextureFromFile(dir + wMetallicTex, true);
		}

		if (!mat->Metallic)
			mat->Metallic = DefaultBlackTex;
		
		if (wDiffuseTex.length() != 0)
		{
			wstring wNameStr = wstring(wDiffuseTex.substr(0, wDiffuseTex.length() - 4));
			map<wstring, wstring> ::iterator it = SponzaRoughnessMap.find(wNameStr);
			if (it != SponzaRoughnessMap.end())
			{
				wRoughnessTex = SponzaRoughnessMap[wNameStr] + L".png";
				mat->Roughness = dx12_rhi->CreateTextureFromFile(dir + wRoughnessTex, true);
			}
		}

		if (!mat->Roughness)
		{
			mat->Roughness = DefaultRougnessTex;
		}

		// HACK!
		if (wDiffuseTex == L"Sponza_Thorn_diffuse.png" || wDiffuseTex == L"VasePlant_diffuse.png" || wDiffuseTex == L"ChainTexture_Albedo.png")
			mat->bHasAlpha = true;

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
		if (dc.mat->bHasAlpha) mesh->bTransparent = true;
		
		mesh->Draws.push_back(dc);

		scene->meshes.push_back(shared_ptr<Mesh>(mesh));
	}

	shared_ptr<Scene> scenePtr = shared_ptr<Scene>(scene);

	return scenePtr;
}

void Corona::InitSpatialDenoisingPass()
{
	ComPtr<ID3DBlob> cs = dx12_rhi->CreateShader(GetAssetFullPath(L"Shaders\\SpatialDenoising.hlsl"), "SpatialFilter", "cs_5_0");

	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};

	SpatialDenoisingFilterPSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
	SpatialDenoisingFilterPSO->cs = cs;
	SpatialDenoisingFilterPSO->computePSODesc = computePsoDesc;
	SpatialDenoisingFilterPSO->BindSRV("DepthTex", 0, 1);
	SpatialDenoisingFilterPSO->BindSRV("GeoNormalTex", 1, 1);
	SpatialDenoisingFilterPSO->BindSRV("InGIResultSHTex", 2, 1);
	SpatialDenoisingFilterPSO->BindSRV("InGIResultColorTex", 3, 1);
	SpatialDenoisingFilterPSO->BindSRV("InSpecualrGITex", 4, 1);


	SpatialDenoisingFilterPSO->BindUAV("OutGIResultSH", 0);
	SpatialDenoisingFilterPSO->BindUAV("OutGIResultColor", 1);
	SpatialDenoisingFilterPSO->BindUAV("OutSpecularGI", 2);


	SpatialDenoisingFilterPSO->BindCBV("SpatialFilterConstant", 0, sizeof(SpatialFilterConstant));
	SpatialDenoisingFilterPSO->IsCompute = true;
	SpatialDenoisingFilterPSO->Init();

	UINT WidthGI = m_width / GIBufferScale;
	UINT HeightGI = m_height / GIBufferScale;

	DiffuseGISHSpatial[0] = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, WidthGI, HeightGI, 1);

	NAME_D3D12_OBJECT(DiffuseGISHSpatial[0]->resource);

	DiffuseGISHSpatial[1] = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, WidthGI, HeightGI, 1);

	NAME_D3D12_OBJECT(DiffuseGISHSpatial[1]->resource);

	DiffuseGICoCgSpatial[0] = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, WidthGI, HeightGI, 1);

	NAME_D3D12_OBJECT(DiffuseGICoCgSpatial[0]->resource);

	DiffuseGICoCgSpatial[1] = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, WidthGI, HeightGI, 1);

	NAME_D3D12_OBJECT(DiffuseGICoCgSpatial[1]->resource);
}

void Corona::InitTemporalDenoisingPass()
{
	ComPtr<ID3DBlob> cs = dx12_rhi->CreateShader(GetAssetFullPath(L"Shaders\\TemporalDenoising.hlsl"), "TemporalFilter", "cs_5_0");
	
	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};

	TemporalDenoisingFilterPSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
	TemporalDenoisingFilterPSO->cs = cs;
	TemporalDenoisingFilterPSO->computePSODesc = computePsoDesc;
	TemporalDenoisingFilterPSO->BindSRV("DepthTex", 0, 1);
	TemporalDenoisingFilterPSO->BindSRV("GeoNormalTex", 1, 1);
	TemporalDenoisingFilterPSO->BindSRV("InGIResultSHTex", 2, 1);
	TemporalDenoisingFilterPSO->BindSRV("InGIResultColorTex", 3, 1);
	TemporalDenoisingFilterPSO->BindSRV("InGIResultSHTexPrev", 4, 1);
	TemporalDenoisingFilterPSO->BindSRV("InGIResultColorTexPrev", 5, 1);
	TemporalDenoisingFilterPSO->BindSRV("VelocityTex", 6, 1);
	TemporalDenoisingFilterPSO->BindSRV("InSpecularGITex", 7, 1);
	TemporalDenoisingFilterPSO->BindSRV("InSpecularGITexPrev", 8, 1);



	TemporalDenoisingFilterPSO->BindUAV("OutGIResultSH", 0);
	TemporalDenoisingFilterPSO->BindUAV("OutGIResultColor", 1);
	TemporalDenoisingFilterPSO->BindUAV("OutGIResultSHDS", 2);
	TemporalDenoisingFilterPSO->BindUAV("OutGIResultColorDS", 3);
	TemporalDenoisingFilterPSO->BindUAV("OutSpecularGI", 4);
	TemporalDenoisingFilterPSO->BindUAV("OutSpecularGIDS", 5);



	TemporalDenoisingFilterPSO->BindCBV("TemporalFilterConstant", 0, sizeof(TemporalFilterConstant));
	TemporalDenoisingFilterPSO->IsCompute = true;
	TemporalDenoisingFilterPSO->Init();
}

void Corona::InitBloomPass()
{
	{
		ComPtr<ID3DBlob> cs = dx12_rhi->CreateShader(GetAssetFullPath(L"Shaders\\BloomBlur.hlsl"), "BloomExtract", "cs_5_0");
		D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};

		BloomExtractPSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
		BloomExtractPSO->cs = cs;
		BloomExtractPSO->computePSODesc = computePsoDesc;
		BloomExtractPSO->BindSRV("SrcTex", 0, 1);
		BloomExtractPSO->BindSRV("Exposure", 1, 1);
		BloomExtractPSO->BindUAV("DstTex", 0);
		BloomExtractPSO->BindUAV("LumaResult", 1);
		BloomExtractPSO->BindSampler("samplerWrap", 0);
		BloomExtractPSO->BindCBV("BloomCB", 0, sizeof(BloomCB));
		BloomExtractPSO->IsCompute = true;
		BloomExtractPSO->Init();
	}
	{
		ComPtr<ID3DBlob> cs = dx12_rhi->CreateShader(GetAssetFullPath(L"Shaders\\BloomBlur.hlsl"), "BloomBlur", "cs_5_0");
		D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};

		BloomBlurPSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
		BloomBlurPSO->cs = cs;
		BloomBlurPSO->computePSODesc = computePsoDesc;
		BloomBlurPSO->BindSRV("SrcTex", 0, 1);
		BloomBlurPSO->BindUAV("DstTex", 0);
		BloomBlurPSO->BindSampler("samplerWrap", 0);
		BloomBlurPSO->BindCBV("BloomCB", 0, sizeof(BloomCB));
		BloomBlurPSO->IsCompute = true;
		BloomBlurPSO->Init();
	}

	{
		ComPtr<ID3DBlob> cs = dx12_rhi->CreateShader(GetAssetFullPath(L"Shaders\\Histogram.hlsl"), "GenerateHistogram", "cs_5_0");
		D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};

		HistogramPSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
		HistogramPSO->cs = cs;
		HistogramPSO->computePSODesc = computePsoDesc;
		HistogramPSO->BindSRV("LumaTex", 0, 1);
		HistogramPSO->BindUAV("Histogram", 0);
		HistogramPSO->IsCompute = true;
		HistogramPSO->Init();

	}

	{
		ComPtr<ID3DBlob> cs = dx12_rhi->CreateShader(GetAssetFullPath(L"Shaders\\DrawHistogram.hlsl"), "DrawHistogram", "cs_5_0");
		D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};

		DrawHistogramPSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
		DrawHistogramPSO->cs = cs;
		DrawHistogramPSO->computePSODesc = computePsoDesc;
		DrawHistogramPSO->BindSRV("Histogram", 0, 1);
		DrawHistogramPSO->BindSRV("Exposure", 1, 1);
		DrawHistogramPSO->BindUAV("ColorBuffer", 0);
		DrawHistogramPSO->IsCompute = true;
		DrawHistogramPSO->Init();

	}

	{
		ComPtr<ID3DBlob> cs = dx12_rhi->CreateShader(GetAssetFullPath(L"Shaders\\Histogram.hlsl"), "ClearHistogram", "cs_5_0");
		D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};

		ClearHistogramPSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
		ClearHistogramPSO->cs = cs;
		ClearHistogramPSO->computePSODesc = computePsoDesc;
		ClearHistogramPSO->BindUAV("Histogram", 0);
		ClearHistogramPSO->IsCompute = true;
		ClearHistogramPSO->Init();

	}


	{
		ComPtr<ID3DBlob> cs = dx12_rhi->CreateShader(GetAssetFullPath(L"Shaders\\AdaptExposureCS.hlsl"), "AdaptExposure", "cs_5_0");
		D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};

		AdapteExposurePSO= shared_ptr<PipelineStateObject>(new PipelineStateObject);
		AdapteExposurePSO->cs = cs;
		AdapteExposurePSO->computePSODesc = computePsoDesc;
		AdapteExposurePSO->BindSRV("Histogram", 0, 1);
		AdapteExposurePSO->BindUAV("Exposure", 0);
		AdapteExposurePSO->BindUAV("Exposure", 0);
		AdapteExposurePSO->BindCBV("AdaptExposureCB", 0, sizeof(AdaptExposureCB));

		AdapteExposurePSO->IsCompute = true;
		AdapteExposurePSO->Init();

	}

	BloomBlurPingPong[0] = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, BloomBufferWidth, BloomBufferHeight, 1);

	NAME_D3D12_OBJECT(BloomBlurPingPong[0]->resource);

	BloomBlurPingPong[1] = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, BloomBufferWidth, BloomBufferHeight, 1);

	NAME_D3D12_OBJECT(BloomBlurPingPong[1]->resource);


	LumaBuffer = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R8_UINT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, BloomBufferWidth, BloomBufferHeight, 1);

	NAME_D3D12_OBJECT(LumaBuffer->resource);

	Histogram = dx12_rhi->CreateBuffer(256, sizeof(UINT32), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
	Histogram->MakeByteAddressBufferSRV();
	NAME_D3D12_OBJECT(Histogram->resource);

	__declspec(align(16)) float initExposure[] =
	{
		Exposure,
		1.0f / Exposure,
		0.01,
		Exposure,
		0.0f,
		kInitialMinLog,
		kInitialMaxLog,
		kInitialMaxLog - kInitialMinLog,
		1.0f / (kInitialMaxLog - kInitialMinLog)
	};

	ExposureData = dx12_rhi->CreateBuffer(8, sizeof(float), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true, initExposure);
	ExposureData->MakeStructuredBufferSRV();
	NAME_D3D12_OBJECT(ExposureData->resource);

}

void Corona::InitGenMipSpecularGIPass()
{
	ComPtr<ID3DBlob> cs = dx12_rhi->CreateShader(GetAssetFullPath(L"Shaders\\GenMipCS.hlsl"), "GenMip", "cs_5_0");

	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};

	GenMipPSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
	GenMipPSO->cs = cs;
	GenMipPSO->computePSODesc = computePsoDesc;
	GenMipPSO->BindSRV("SrcMip", 0, 1);

	GenMipPSO->BindUAV("OutMip1", 0);
	GenMipPSO->BindUAV("OutMip2", 1);
	GenMipPSO->BindUAV("OutMip3", 2);
	GenMipPSO->BindUAV("OutMip4", 3);

	GenMipPSO->BindSampler("BilinearClamp", 0);

	GenMipPSO->BindCBV("GenMipCB", 0, sizeof(GenMipCB));
	GenMipPSO->IsCompute = true;
	GenMipPSO->Init();
}

void Corona::InitDrawMeshRS()
{
	ComPtr<ID3DBlob> vs = dx12_rhi->CreateShader(GetAssetFullPath(L"Shaders\\GBuffer.hlsl"), "VSMain", "vs_5_0");
	ComPtr<ID3DBlob> ps = dx12_rhi->CreateShader(GetAssetFullPath(L"Shaders\\GBuffer.hlsl"), "PSMain", "ps_5_0");

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

	
	GBufferPassPSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
	GBufferPassPSO->ps = ps;
	GBufferPassPSO->vs = vs;
	GBufferPassPSO->graphicsPSODesc = psoDescMesh;
	GBufferPassPSO->BindSRV("AlbedoTex", 0, 1);
	GBufferPassPSO->BindSRV("NormalTex", 1, 1);
	GBufferPassPSO->BindSRV("RoughnessTex", 2, 1);
	GBufferPassPSO->BindSRV("MetallicTex", 3, 1);

	GBufferPassPSO->BindSampler("samplerWrap", 0);
	GBufferPassPSO->BindCBV("GBufferConstantBuffer", 0, sizeof(GBufferConstantBuffer));

	bool bSucess = GBufferPassPSO->Init();
}

void Corona::InitImgui()
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

void Corona::InitBlueNoiseTexture()
{
	string path = "assets/bluenoise/64_64_64/HDR_RGBA.raw";
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

void Corona::InitCopyPass()
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
	
	ComPtr<ID3DBlob> vs = dx12_rhi->CreateShader(GetAssetFullPath(L"Shaders\\ToneMapPS.hlsl"), "VSMain", "vs_5_0");
	ComPtr<ID3DBlob> ps = dx12_rhi->CreateShader(GetAssetFullPath(L"Shaders\\ToneMapPS.hlsl"), "PSMain", "ps_5_0");


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

	ToneMapPSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
	ToneMapPSO->ps = ps;
	ToneMapPSO->vs = vs;
	ToneMapPSO->graphicsPSODesc = psoDesc;

	ToneMapPSO->BindSRV("SrcTex", 0, 1);
	ToneMapPSO->BindSampler("samplerWrap", 0);
	ToneMapPSO->BindCBV("ScaleOffsetParams", 0, sizeof(ToneMapCB));

	ToneMapPSO->Init();
}

void Corona::InitDebugPass()
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

	ComPtr<ID3DBlob> vs = dx12_rhi->CreateShader(GetAssetFullPath(L"Shaders\\DebugPS.hlsl"), "VSMain", "vs_5_0");
	ComPtr<ID3DBlob> ps = dx12_rhi->CreateShader(GetAssetFullPath(L"Shaders\\DebugPS.hlsl"), "PSMain", "ps_5_0");

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

	BufferVisualizePSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
	BufferVisualizePSO->ps = ps;
	BufferVisualizePSO->vs = vs;
	BufferVisualizePSO->graphicsPSODesc = psoDesc;

	BufferVisualizePSO->BindSRV("SrcTex", 0, 1);
	BufferVisualizePSO->BindSRV("SrcTexSH", 1, 1);
	BufferVisualizePSO->BindSRV("SrcTexNormal", 2, 1);

	BufferVisualizePSO->BindSampler("samplerWrap", 0);
	BufferVisualizePSO->BindCBV("DebugPassCB", 0, sizeof(DebugPassCB));

	BufferVisualizePSO->Init();
}

void Corona::InitLightingPass()
{
	ComPtr<ID3DBlob> vs = dx12_rhi->CreateShader(GetAssetFullPath(L"Shaders\\LightingPS.hlsl"), "VSMain", "vs_5_0");
	ComPtr<ID3DBlob> ps = dx12_rhi->CreateShader(GetAssetFullPath(L"Shaders\\LightingPS.hlsl"), "PSMain", "ps_5_0");

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

	LightingPSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
	LightingPSO->ps = ps;
	LightingPSO->vs = vs;
	LightingPSO->graphicsPSODesc = psoDesc;

	LightingPSO->BindSRV("AlbedoTex", 0, 1);
	LightingPSO->BindSRV("NormalTex", 1, 1);
	LightingPSO->BindSRV("ShadowTex", 2, 1);
	LightingPSO->BindSRV("VelocityTex", 3, 1);
	LightingPSO->BindSRV("DepthTex", 4, 1);
	LightingPSO->BindSRV("GIResultSHTex", 5, 1);
	LightingPSO->BindSRV("GIResultColorTex", 6, 1);
	LightingPSO->BindSRV("SpecularGITex", 7, 1);
	LightingPSO->BindSRV("RoughnessMetalicTex", 8, 1);
	LightingPSO->BindSRV("SpecularGITex3x3", 9, 1);
	LightingPSO->BindSRV("SpecularGITexMip1", 10, 1);
	LightingPSO->BindSRV("SpecularGITexMip2", 11, 1);
	LightingPSO->BindSRV("SpecularGITexMip3", 12, 1);
	LightingPSO->BindSRV("SpecularGITexMip4", 13, 1);



	LightingPSO->BindSampler("samplerWrap", 0);
	LightingPSO->BindCBV("LightingParam", 0, sizeof(LightingParam));
	LightingPSO->Init();
}

void Corona::InitTemporalAAPass()
{
	ComPtr<ID3DBlob> vs = dx12_rhi->CreateShader(GetAssetFullPath(L"Shaders\\TemporalAA.hlsl"), "VSMain", "vs_5_0");
	ComPtr<ID3DBlob> ps = dx12_rhi->CreateShader(GetAssetFullPath(L"Shaders\\TemporalAA.hlsl"), "PSMain", "ps_5_0");
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

	TemporalAAPSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
	TemporalAAPSO->ps = ps;
	TemporalAAPSO->vs = vs;
	TemporalAAPSO->graphicsPSODesc = psoDesc;

	TemporalAAPSO->BindSRV("CurrentColorTex", 0, 1);
	TemporalAAPSO->BindSRV("PrevColorTex", 1, 1);
	TemporalAAPSO->BindSRV("VelocityTex", 2, 1);
	TemporalAAPSO->BindSRV("DepthTex", 3, 1);
	TemporalAAPSO->BindSRV("BloomTex", 4, 1);
	TemporalAAPSO->BindSRV("Exposure", 5, 1);


	TemporalAAPSO->BindSampler("samplerWrap", 0);
	TemporalAAPSO->BindCBV("LightingParam", 0, sizeof(LightingParam));
	TemporalAAPSO->Init();
}



void Corona::CopyPass()
{
#if USE_AFTERMATH
	NVAftermathMarker(dx12_rhi->AM_CL_Handle, "CopyPass");
#endif
	PIXScopedEvent(dx12_rhi->GlobalCmdList->CmdList.Get(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "CopyPass");


	Texture* backbuffer = framebuffers[dx12_rhi->CurrentFrameIndex].get();
	Texture* ResolveTarget = ColorBuffers[ColorBufferWriteIndex];//framebuffers[dx12_rhi->CurrentFrameIndex].get();

	ToneMapPSO->Apply(dx12_rhi->GlobalCmdList->CmdList.Get());


	ToneMapPSO->SetSampler("samplerWrap", samplerWrap.get(), dx12_rhi->GlobalCmdList->CmdList.Get());
	ToneMapPSO->SetSRV("SrcTex", ResolveTarget->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());

	ToneMapCB.Offset = glm::vec4(0, 0, 0, 0);
	ToneMapCB.Scale = glm::vec4(1, 1, 0, 0);
	ToneMapCB.ToneMapMode = ToneMapMode;
	ToneMapPSO->SetCBVValue("ScaleOffsetParams", &ToneMapCB, dx12_rhi->GlobalCmdList->CmdList.Get());

	ToneMapPSO->Apply(dx12_rhi->GlobalCmdList->CmdList.Get());

	UINT Width = m_width / 1;
	UINT Height = m_height / 1;
	CD3DX12_VIEWPORT viewport(0.0f, 0.0f, static_cast<float>(Width), static_cast<float>(Height));
	CD3DX12_RECT scissorRect(0, 0, static_cast<LONG>(Width), static_cast<LONG>(Height));

	dx12_rhi->GlobalCmdList->CmdList->RSSetViewports(1, &viewport);
	dx12_rhi->GlobalCmdList->CmdList->RSSetScissorRects(1, &scissorRect);

	dx12_rhi->GlobalCmdList->CmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	dx12_rhi->GlobalCmdList->CmdList->IASetVertexBuffers(0, 1, &FullScreenVB->view);
	
	dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);

	
	PIXEndEvent(dx12_rhi->GlobalCmdList->CmdList.Get());
}

void Corona::DebugPass()
{
#if USE_AFTERMATH
	NVAftermathMarker(dx12_rhi->AM_CL_Handle, "DebugPass");
#endif
	PIXScopedEvent(dx12_rhi->GlobalCmdList->CmdList.Get(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "DebugPass");

	BufferVisualizePSO->Apply(dx12_rhi->GlobalCmdList->CmdList.Get());
	BufferVisualizePSO->SetSampler("samplerWrap", samplerWrap.get(), dx12_rhi->GlobalCmdList->CmdList.Get());

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
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb, dx12_rhi->GlobalCmdList->CmdList.Get());
		BufferVisualizePSO->SetSRV("SrcTex", ShadowBuffer->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
		dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);
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
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb, dx12_rhi->GlobalCmdList->CmdList.Get());
		BufferVisualizePSO->SetSRV("SrcTex", NormalBuffer->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
		dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);
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
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb, dx12_rhi->GlobalCmdList->CmdList.Get());
		BufferVisualizePSO->SetSRV("SrcTex", GeomNormalBuffer->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
		dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);
	});

	functions.push_back([&](EDebugVisualization eFS) {
		// Blooom buffer
		DebugPassCB cb;
		if (eFS == EDebugVisualization::BLOOM)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(-0.25, 0.25, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}
		cb.DebugMode = RAW_COPY;
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb, dx12_rhi->GlobalCmdList->CmdList.Get());
		BufferVisualizePSO->SetSRV("SrcTex", BloomBlurPingPong[0]->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
		dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);
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

		cb.ProjectionParams.z = Near;
		cb.ProjectionParams.w = Far;
		cb.DebugMode = DEPTH;
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb, dx12_rhi->GlobalCmdList->CmdList.Get());
		BufferVisualizePSO->SetSRV("SrcTex", DepthBuffer->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
		dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);
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
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb, dx12_rhi->GlobalCmdList->CmdList.Get());
		BufferVisualizePSO->SetSRV("SrcTex", DiffuseGISHRaw->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
		dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// raw CoCg
		DebugPassCB cb;

		if (eFS == EDebugVisualization::RAW_CoCg)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(0.25, -0.25, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb, dx12_rhi->GlobalCmdList->CmdList.Get());
		BufferVisualizePSO->SetSRV("SrcTex", DiffuseGICoCgRaw->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
		dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);
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
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb, dx12_rhi->GlobalCmdList->CmdList.Get());
		BufferVisualizePSO->SetSRV("SrcTex", DiffuseGISHTemporal[GIBufferWriteIndex]->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
		dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);
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
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb, dx12_rhi->GlobalCmdList->CmdList.Get());
		BufferVisualizePSO->SetSRV("SrcTex", DiffuseGISHSpatial[0]->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
		dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);
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
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb, dx12_rhi->GlobalCmdList->CmdList.Get());
		BufferVisualizePSO->SetSRV("SrcTex", DiffuseGICoCgSpatial[0]->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
		BufferVisualizePSO->SetSRV("SrcTexSH", DiffuseGISHSpatial[0]->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
		BufferVisualizePSO->SetSRV("SrcTexNormal", NormalBuffer->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());

		dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);
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
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb, dx12_rhi->GlobalCmdList->CmdList.Get());
		BufferVisualizePSO->SetSRV("SrcTex", AlbedoBuffer->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
		dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);
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
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb, dx12_rhi->GlobalCmdList->CmdList.Get());
		BufferVisualizePSO->SetSRV("SrcTex", VelocityBuffer->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
		dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);
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
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb, dx12_rhi->GlobalCmdList->CmdList.Get());
		BufferVisualizePSO->SetSRV("SrcTex", RoughnessMetalicBuffer->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
		dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// specular raw
		DebugPassCB cb;


		if (eFS == EDebugVisualization::SPECULAR_RAW)
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
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb, dx12_rhi->GlobalCmdList->CmdList.Get());
		BufferVisualizePSO->SetSRV("SrcTex", SpeculaGIBufferRaw->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
		dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);
	});

	functions.push_back([&](EDebugVisualization eFS) {
		// temporal filtered specular
		DebugPassCB cb;


		if (eFS == EDebugVisualization::TEMPORAL_FILTERED_SPECULAR)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(-0.75, 0.25, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb, dx12_rhi->GlobalCmdList->CmdList.Get());
		BufferVisualizePSO->SetSRV("SrcTex", SpeculaGIBufferTemporal[GIBufferWriteIndex]->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
		dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);
	});

	functions.push_back([&](EDebugVisualization eFS) {
		// spatial filtered specular
		DebugPassCB cb;


		if (eFS == EDebugVisualization::TEMPORAL_FILTERED_SPECULAR_2X2)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(-0.75, 0.75, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb, dx12_rhi->GlobalCmdList->CmdList.Get());
		BufferVisualizePSO->SetSRV("SrcTex", SpeculaGIBufferMip[0]->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
		dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);
	});

	functions.push_back([&](EDebugVisualization eFS) {
		// spatial filtered specular
		DebugPassCB cb;


		if (eFS == EDebugVisualization::TEMPORAL_FILTERED_SPECULAR_4X4)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(-0.75, 0.75, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb, dx12_rhi->GlobalCmdList->CmdList.Get());
		BufferVisualizePSO->SetSRV("SrcTex", SpeculaGIBufferMip[1]->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
		dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);
	});

	functions.push_back([&](EDebugVisualization eFS) {
		// spatial filtered specular
		DebugPassCB cb;


		if (eFS == EDebugVisualization::TEMPORAL_FILTERED_SPECULAR_8X8)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(-0.75, 0.75, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb, dx12_rhi->GlobalCmdList->CmdList.Get());
		BufferVisualizePSO->SetSRV("SrcTex", SpeculaGIBufferMip[2]->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
		dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);
	});

	functions.push_back([&](EDebugVisualization eFS) {
		// spatial filtered specular
		DebugPassCB cb;


		if (eFS == EDebugVisualization::TEMPORAL_FILTERED_SPECULAR_16X16)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(-0.75, 0.75, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb, dx12_rhi->GlobalCmdList->CmdList.Get());
		BufferVisualizePSO->SetSRV("SrcTex", SpeculaGIBufferMip[3]->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
		dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);
	});

	EDebugVisualization FullScreenVisualize = EDebugVisualization::SPECULAR_RAW;

	for (auto& f : functions)
	{
		f(FullscreenDebugBuffer);
	}
}

void Corona::LightingPass()
{
#if USE_AFTERMATH
	NVAftermathMarker(dx12_rhi->AM_CL_Handle, "LightingPass");
#endif
	PIXScopedEvent(dx12_rhi->GlobalCmdList->CmdList.Get(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "LightingPass");

	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(LightingBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));

	LightingPSO->Apply(dx12_rhi->GlobalCmdList->CmdList.Get());

	LightingPSO->SetSampler("samplerWrap", samplerWrap.get(), dx12_rhi->GlobalCmdList->CmdList.Get());
	LightingPSO->SetSRV("AlbedoTex", AlbedoBuffer->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
	LightingPSO->SetSRV("NormalTex", NormalBuffer->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
	LightingPSO->SetSRV("ShadowTex", ShadowBuffer->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());

	LightingPSO->SetSRV("VelocityTex", VelocityBuffer->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
	LightingPSO->SetSRV("DepthTex", DepthBuffer->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
	LightingPSO->SetSRV("GIResultSHTex", DiffuseGISHSpatial[0]->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
	LightingPSO->SetSRV("GIResultColorTex", DiffuseGICoCgSpatial[0]->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
	LightingPSO->SetSRV("SpecularGITex", SpeculaGIBufferTemporal[GIBufferWriteIndex]->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
	LightingPSO->SetSRV("RoughnessMetalicTex", RoughnessMetalicBuffer->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
	LightingPSO->SetSRV("SpecularGITex3x3", SpeculaGIBufferSpatial[0]->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
	LightingPSO->SetSRV("SpecularGITexMip1", SpeculaGIBufferMip[0]->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
	LightingPSO->SetSRV("SpecularGITexMip2", SpeculaGIBufferMip[1]->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
	LightingPSO->SetSRV("SpecularGITexMip3", SpeculaGIBufferMip[2]->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
	LightingPSO->SetSRV("SpecularGITexMip4", SpeculaGIBufferMip[3]->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());



	glm::mat4x4 InvViewMat = glm::inverse(ViewMat);
	LightingParam Param;
	Param.ViewMatrix = glm::transpose(ViewMat);
	Param.InvViewMatrix = glm::transpose(InvViewMat);
	Param.LightDir = glm::vec4(glm::normalize(LightDir), LightIntensity);
	
	Param.RTSize.x = m_width;
	Param.RTSize.y = m_height;

	if(bEnableTAA)
		Param.TAABlendFactor = 0.1;
	else
		Param.TAABlendFactor = 1.0;

	Param.GIBufferScale = GIBufferScale;

	glm::normalize(Param.LightDir);
	LightingPSO->SetCBVValue("LightingParam", &Param, dx12_rhi->GlobalCmdList->CmdList.Get());


	LightingPSO->Apply(dx12_rhi->GlobalCmdList->CmdList.Get());

	dx12_rhi->GlobalCmdList->CmdList->OMSetRenderTargets(1, &LightingBuffer->CpuHandleRTV, FALSE, nullptr);
	dx12_rhi->GlobalCmdList->CmdList->RSSetViewports(1, &m_viewport);
	dx12_rhi->GlobalCmdList->CmdList->RSSetScissorRects(1, &m_scissorRect);
	dx12_rhi->GlobalCmdList->CmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	dx12_rhi->GlobalCmdList->CmdList->IASetVertexBuffers(0, 1, &FullScreenVB->view);
	dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(LightingBuffer->resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
}

void Corona::TemporalAAPass()
{
#if USE_AFTERMATH
	NVAftermathMarker(dx12_rhi->AM_CL_Handle, "TemporalAAPass");
#endif
	PIXScopedEvent(dx12_rhi->GlobalCmdList->CmdList.Get(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "TemporalAAPass");

	UINT PrevColorBufferIndex = 1 - ColorBufferWriteIndex;
	Texture* ResolveTarget = ColorBuffers[ColorBufferWriteIndex];//framebuffers[dx12_rhi->CurrentFrameIndex].get();

	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(ResolveTarget->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));

	TemporalAAPSO->Apply(dx12_rhi->GlobalCmdList->CmdList.Get());

	TemporalAAPSO->SetSampler("samplerWrap", samplerWrap.get(), dx12_rhi->GlobalCmdList->CmdList.Get());
	TemporalAAPSO->SetSRV("CurrentColorTex", LightingBuffer->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
	Texture* PrevColorBuffer = ColorBuffers[PrevColorBufferIndex];
	TemporalAAPSO->SetSRV("PrevColorTex", PrevColorBuffer->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
	TemporalAAPSO->SetSRV("VelocityTex", VelocityBuffer->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
	TemporalAAPSO->SetSRV("DepthTex", DepthBuffer->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
	TemporalAAPSO->SetSRV("BloomTex", BloomBlurPingPong[0]->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
	TemporalAAPSO->SetSRV("Exposure", ExposureData->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());

	TemporalAAParam Param;

	Param.RTSize.x = m_width;
	Param.RTSize.y = m_height;
	//Param.Exposure = Exposure;

	if (bEnableTAA)
		Param.TAABlendFactor = 0.1;
	else
		Param.TAABlendFactor = 1.0;

	Param.ClampMode = ClampMode;
	Param.BloomStrength = BloomStrength;

	TemporalAAPSO->SetCBVValue("LightingParam", &Param, dx12_rhi->GlobalCmdList->CmdList.Get());
	TemporalAAPSO->Apply(dx12_rhi->GlobalCmdList->CmdList.Get());

	dx12_rhi->GlobalCmdList->CmdList->OMSetRenderTargets(1, &ResolveTarget->CpuHandleRTV, FALSE, nullptr);
	dx12_rhi->GlobalCmdList->CmdList->RSSetViewports(1, &m_viewport);
	dx12_rhi->GlobalCmdList->CmdList->RSSetScissorRects(1, &m_scissorRect);
	dx12_rhi->GlobalCmdList->CmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	dx12_rhi->GlobalCmdList->CmdList->IASetVertexBuffers(0, 1, &FullScreenVB->view);
	dx12_rhi->GlobalCmdList->CmdList->DrawInstanced(4, 1, 0, 0);
	
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(ResolveTarget->resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

	if (bDrawHistogram)
	{
		DrawHistogramPSO->Apply(dx12_rhi->GlobalCmdList->CmdList.Get());
		DrawHistogramPSO->SetSRV("Histogram", Histogram->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
		DrawHistogramPSO->SetSRV("Exposure", ExposureData->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
		DrawHistogramPSO->SetUAV("ColorBuffer", ResolveTarget->GpuHandleUAV, dx12_rhi->GlobalCmdList->CmdList.Get());
		dx12_rhi->GlobalCmdList->CmdList->Dispatch(1, 32, 1);
	}
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(ResolveTarget->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

}

void Corona::GenMipSpecularGIPass()
{
#if USE_AFTERMATH
	NVAftermathMarker(dx12_rhi->AM_CL_Handle, "GenMipSpecularGIPass");
#endif
	PIXScopedEvent(dx12_rhi->GlobalCmdList->CmdList.Get(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "GenMipSpecularGIPass");
	for(int i=0;i<4;i++)
		dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(SpeculaGIBufferMip[i]->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

	GenMipPSO->Apply(dx12_rhi->GlobalCmdList->CmdList.Get());
	GenMipPSO->SetSRV("SrcMip", SpeculaGIBufferTemporal[GIBufferWriteIndex]->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
	GenMipPSO->SetUAV("OutMip1", SpeculaGIBufferMip[0]->GpuHandleUAV, dx12_rhi->GlobalCmdList->CmdList.Get());
	GenMipPSO->SetUAV("OutMip2", SpeculaGIBufferMip[1]->GpuHandleUAV, dx12_rhi->GlobalCmdList->CmdList.Get());
	GenMipPSO->SetUAV("OutMip3", SpeculaGIBufferMip[2]->GpuHandleUAV, dx12_rhi->GlobalCmdList->CmdList.Get());
	GenMipPSO->SetUAV("OutMip4", SpeculaGIBufferMip[3]->GpuHandleUAV, dx12_rhi->GlobalCmdList->CmdList.Get());
	GenMipPSO->SetSampler("BilinearClamp", samplerBilinearWrap.get(), dx12_rhi->GlobalCmdList->CmdList.Get());
	
	GenMipCB.NumMipLevels = 4;
	GenMipCB.TexelSize = glm::vec2(1.0 / (m_width * 0.5), 1.0/(m_height*0.5));
	GenMipPSO->SetCBVValue("GenMipCB", &GenMipCB, dx12_rhi->GlobalCmdList->CmdList.Get());


	dx12_rhi->GlobalCmdList->CmdList->Dispatch(m_width/8, m_height/8, 1);
	
	for (int i = 0; i < 4; i++)
		dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(SpeculaGIBufferMip[i]->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
}

void Corona::BloomPass()
{
#if USE_AFTERMATH
	NVAftermathMarker(dx12_rhi->AM_CL_Handle, "BloomPass");
#endif
	PIXScopedEvent(dx12_rhi->GlobalCmdList->CmdList.Get(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "BloomPass");

	BloomCB.RTSize.x = BloomBufferWidth;
	BloomCB.RTSize.y = BloomBufferHeight;

	float sigma_pixels = BloomSigma * m_height;

	float effective_sigma = sigma_pixels * 0.25f;
	effective_sigma = glm::min(effective_sigma, 100.f);
	effective_sigma = glm::max(effective_sigma, 1.f);
	BloomCB.NumSamples = glm::round(effective_sigma*4.f);
	BloomCB.WeightScale = -1.f / (2.0 * effective_sigma * effective_sigma);
	BloomCB.NormalizationScale = 1.f / (sqrtf(2 * glm::pi<float>()) * effective_sigma);;
	//BloomCB.Exposure = Exposure;
	/*BloomCB.MinLog = kInitialMinLog;
	BloomCB.RcpLogRange = 1.0f / (kInitialMaxLog - kInitialMinLog);*/

	// extraction pass
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(BloomBlurPingPong[0]->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(LumaBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

	BloomExtractPSO->Apply(dx12_rhi->GlobalCmdList->CmdList.Get());
	BloomExtractPSO->SetSRV("SrcTex", LightingBuffer->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
	BloomExtractPSO->SetSRV("Exposure", ExposureData->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
	BloomExtractPSO->SetUAV("DstTex", BloomBlurPingPong[0]->GpuHandleUAV, dx12_rhi->GlobalCmdList->CmdList.Get());
	BloomExtractPSO->SetUAV("LumaResult", LumaBuffer->GpuHandleUAV, dx12_rhi->GlobalCmdList->CmdList.Get());


	BloomExtractPSO->SetSampler("samplerWrap", samplerWrap.get(), dx12_rhi->GlobalCmdList->CmdList.Get());

	BloomExtractPSO->SetCBVValue("BloomCB", &BloomCB, dx12_rhi->GlobalCmdList->CmdList.Get());

	dx12_rhi->GlobalCmdList->CmdList->Dispatch(BloomBufferWidth/ 32, BloomBufferHeight / 32, 1);
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(BloomBlurPingPong[0]->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(LumaBuffer->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

	// horizontal pass
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(BloomBlurPingPong[1]->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));


	BloomBlurPSO->Apply(dx12_rhi->GlobalCmdList->CmdList.Get());

	BloomBlurPSO->SetSRV("SrcTex", BloomBlurPingPong[0]->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
	BloomBlurPSO->SetUAV("DstTex", BloomBlurPingPong[1]->GpuHandleUAV, dx12_rhi->GlobalCmdList->CmdList.Get());


	BloomBlurPSO->SetSampler("samplerWrap", samplerWrap.get(), dx12_rhi->GlobalCmdList->CmdList.Get());

	BloomCB.BlurDirection = glm::vec2(1, 0);
	BloomBlurPSO->SetCBVValue("BloomCB", &BloomCB, dx12_rhi->GlobalCmdList->CmdList.Get());

	dx12_rhi->GlobalCmdList->CmdList->Dispatch(BloomBufferWidth / 32, BloomBufferHeight / 32, 1);

	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(BloomBlurPingPong[1]->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));


	// vertical pass
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(BloomBlurPingPong[0]->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));


	BloomBlurPSO->Apply(dx12_rhi->GlobalCmdList->CmdList.Get());

	BloomBlurPSO->SetSRV("SrcTex", BloomBlurPingPong[1]->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
	BloomBlurPSO->SetUAV("DstTex", BloomBlurPingPong[0]->GpuHandleUAV, dx12_rhi->GlobalCmdList->CmdList.Get());


	BloomBlurPSO->SetSampler("samplerWrap", samplerWrap.get(), dx12_rhi->GlobalCmdList->CmdList.Get());

	BloomCB.BlurDirection = glm::vec2(0, 1);
	BloomBlurPSO->SetCBVValue("BloomCB", &BloomCB, dx12_rhi->GlobalCmdList->CmdList.Get());

	dx12_rhi->GlobalCmdList->CmdList->Dispatch(BloomBufferWidth / 32, BloomBufferHeight / 32, 1);

	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(BloomBlurPingPong[0]->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

	// histogram pass
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(Histogram->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

	//dx12_rhi->GlobalCmdList->CmdList->ClearUnorderedAccessViewUint(Histogram->GpuHandleUAV, Histogram->CpuHandleUAV, Histogram->resource.Get(), ClearColor, 0, nullptr);

	ClearHistogramPSO->Apply(dx12_rhi->GlobalCmdList->CmdList.Get());
	ClearHistogramPSO->SetUAV("Histogram", Histogram->GpuHandleUAV, dx12_rhi->GlobalCmdList->CmdList.Get());
	dx12_rhi->GlobalCmdList->CmdList->Dispatch(1, 1, 1);

	HistogramPSO->Apply(dx12_rhi->GlobalCmdList->CmdList.Get());
	HistogramPSO->SetSRV("LumaTex", LumaBuffer->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
	HistogramPSO->SetUAV("Histogram", Histogram->GpuHandleUAV, dx12_rhi->GlobalCmdList->CmdList.Get());
	dx12_rhi->GlobalCmdList->CmdList->Dispatch(BloomBufferWidth / 16, 1, 1);

	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(Histogram->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));


	// adapte exposure pass
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(ExposureData->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

	AdapteExposurePSO->Apply(dx12_rhi->GlobalCmdList->CmdList.Get());
	AdapteExposurePSO->SetSRV("Histogram", Histogram->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
	AdapteExposurePSO->SetUAV("Exposure", ExposureData->GpuHandleUAV, dx12_rhi->GlobalCmdList->CmdList.Get());


	AdaptExposureCB.PixelCount = BloomBufferWidth * BloomBufferHeight;
	
	AdapteExposurePSO->SetCBVValue("AdaptExposureCB", &AdaptExposureCB, dx12_rhi->GlobalCmdList->CmdList.Get());

	dx12_rhi->GlobalCmdList->CmdList->Dispatch(1, 1, 1);
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(ExposureData->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

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

void Corona::OnUpdate()
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
	ProjMat = m_camera.GetProjectionMatrix(Fov, m_aspectRatio, Near, Far);
	UnjitteredViewProjMat = ProjMat * ViewMat;

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
	RTReflectionViewParam.RandomOffset = glm::vec2(timeElapsed, timeElapsed);
	RTReflectionViewParam.FrameCounter = FrameCounter;

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
		//dx12_rhi->errorString += string("recompile all shaders\n");

		RecompileShaders();
		bRecompileShaders = false;
	}
}

// Render the scene.
void Corona::OnRender()
{
	dx12_rhi->BeginFrame();
	
	// Record all the commands we need to render the scene into the command list.

	GBufferPass();


	RaytraceShadowPass();


	RaytraceReflectionPass();
	

	RaytraceGIPass();

	TemporalDenoisingPass();

	GenMipSpecularGIPass();

	SpatialDenoisingPass();

	LightingPass();

	BloomPass();

	TemporalAAPass();

	
	Texture* backbuffer = framebuffers[dx12_rhi->CurrentFrameIndex].get();
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(backbuffer->resource.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));
	dx12_rhi->GlobalCmdList->CmdList->OMSetRenderTargets(1, &backbuffer->CpuHandleRTV, FALSE, nullptr);

	CopyPass();

	if(bDebugDraw)
		DebugPass();

	if (bShowImgui)
	{

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

		ImGui::Text("\nArrow keys : rotate camera imGui\
			\nWASD keys : move camera imGui\
			\nI : show/hide imGui\
			\nB : show/hide buffer visualization\
			\nT : enable/disable temporal aa\n\n");

		ImGui::SliderFloat("Camera turn speed", &m_turnSpeed, 0.0f, glm::half_pi<float>()*2);

		ImGui::Checkbox("Enable TemporalAA", &bEnableTAA);
		ImGui::Checkbox("Visualize Buffers", &bDebugDraw);
		ImGui::Checkbox("Draw Histogram", &bDrawHistogram);

	
		/*
		enum class EDebugVisualization
		{
		SHADOW,
		WORLD_NORMAL,
		GEO_NORMAL,
		DEPTH,
		RAW_SH,
		RAW_CoCg,
		TEMPORAL_FILTERED_SH,
		SPATIAL_FILTERED_SH,
		SH_LIGHTING,
		ALBEDO,
		VELOCITY,
		ROUGNESS_METALLIC,
		SPECULAR_RAW,
		TEMPORAL_FILTERED_SPECULAR,
		TEMPORAL_FILTERED_SPECULAR_2X2,
		TEMPORAL_FILTERED_SPECULAR_4X4,
		TEMPORAL_FILTERED_SPECULAR_8X8,
		TEMPORAL_FILTERED_SPECULAR_16X16,
		BLOOM,
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
			"RAW_CoCg",
			"TEMPORAL_FILTERED_SH",
			"SPATIAL_FILTERED_SH",
			"SH_LIGHTING",
			"ALBEDO",
			"VELOCITY",
			"ROUGNESS_METALLIC",
			"SPECULAR_RAW",
			"TEMPORAL_FILTERED_SPECULAR",
			"TEMPORAL_FILTERED_SPECULAR_2X2",
			"TEMPORAL_FILTERED_SPECULAR_4X4",
			"TEMPORAL_FILTERED_SPECULAR_8X8",
			"TEMPORAL_FILTERED_SPECULAR_16X16",
			"BLOOM",
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

		{
			static ImGuiComboFlags flags = 0;
			const char* items[] = {
				"LINEAR_TO_SRGB",
				"REINHARD",
				"FILMIC",
				"FILMIC_HABLE",
			};
			static const char* item_current = items[UINT(EToneMapMode::FILMIC_ALU)];
			if (ImGui::BeginCombo("Tone Map Operator", item_current, flags))
			{
				for (int n = 0; n < IM_ARRAYSIZE(items); n++)
				{
					bool is_selected = (item_current == items[n]);
					if (ImGui::Selectable(items[n], is_selected))\
					{
						item_current = items[n];
						ToneMapMode = (EToneMapMode)n;
					}
					if (is_selected)
					{
						ImGui::SetItemDefaultFocus();
					}

				}
				ImGui::EndCombo();
			}

		}

		if (ToneMapMode == FILMIC_HABLE)
		{

			ImGui::SliderFloat("WhitePoint_Hejl", &ToneMapCB.WhitePoint_Hejl, 0.1f, 5.0f);

			ImGui::SliderFloat("ShoulderStrength", &ToneMapCB.ShoulderStrength, 0.1f, 10.0f);

			ImGui::SliderFloat("LinearStrength", &ToneMapCB.LinearStrength, 0.1f, 10.0f);

			ImGui::SliderFloat("LinearAngle", &ToneMapCB.LinearAngle, 0.1f, 20.0f);

			ImGui::SliderFloat("ToeStrength", &ToneMapCB.ToeStrength, 0.1f, 20.0f);

			ImGui::SliderFloat("WhitePoint_Hable", &ToneMapCB.WhitePoint_Hable, 0.1f, 20.0f);
		}

		// ImGui::gizmo3D has memory leak.
		glm::vec3 LD = glm::vec3(LightDir.z, -LightDir.y, -LightDir.x);
		ImGui::gizmo3D("##gizmo1", LD, 200 /* mode */);
		LightDir = glm::vec3(-LD.z, -LD.y, LD.x);
		ImGui::SameLine();
		ImGui::Text("Light Direction");


		ImGui::SliderFloat("Light Brightness", &LightIntensity, 0.0f, 20.0f);

		ImGui::SliderFloat("SponzaRoughness multiplier", &SponzaRoughnessMultiplier, 0.0f, 1.0f);
		ImGui::SliderFloat("ShaderBallRoughness multiplier", &ShaderBallRoughnessMultiplier, 0.0f, 1.0f);


		ImGui::SliderFloat("IndirectDiffuse Depth Weight Factor", &SpatialFilterCB.IndirectDiffuseWeightFactorDepth, 0.0f, 20.0f);
		ImGui::SliderFloat("IndirectDiffuse Normal Weight Factor", &SpatialFilterCB.IndirectDiffuseWeightFactorNormal, 0.0f, 20.0f);

		ImGui::SliderFloat("TemporalValidParams.x", &TemporalFilterCB.TemporalValidParams.x, 0.0f, 128);

		ImGui::SliderFloat("BloomSigma", &BloomSigma, 0.0f, 2.0f);

		ImGui::SliderFloat("BloomThreshHold", &BloomCB.BloomThreshHold, 0.0f, 2.0f);

		ImGui::SliderFloat("BloomStrength", &BloomStrength, 0.0f, 4.0f);

		ImGui::SliderFloat("TargetLuminance", &AdaptExposureCB.TargetLuminance, 0.001f, 0.990f);

		ImGui::SliderFloat("AdaptationRate", &AdaptExposureCB.AdaptationRate, 0.01f, 1.0f);

		ImGui::SliderFloat("MinExposure", &AdaptExposureCB.MinExposure, -8.0f, 0.0f);
		ImGui::SliderFloat("MaxExposure", &AdaptExposureCB.MaxExposure, 0.0f, 8.0f);

	/*	AdaptExposureCB.TargetLuminance = 0.08;
		AdaptExposureCB.AdaptationRate = 0.05;
		AdaptExposureCB.MinExposure = 1.0f / 64.0f;
		AdaptExposureCB.MaxExposure = 64.0f;
		*/
		if (dx12_rhi->errorString.size() > 0)
		{
			if (!ImGui::IsPopupOpen("Msg"))
			{
				ImGui::SetNextWindowSize(ImVec2(1200, 800));
				ImGui::OpenPopup("Msg");
			}

			if (ImGui::BeginPopupModal("Msg"))
			{
				ImGui::TextWrapped(dx12_rhi->errorString.c_str());
			
				if (ImGui::Button("Compile again", ImVec2(120, 0)))
				{
					bRecompileShaders = true;
					dx12_rhi->errorString = "";
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				if (ImGui::Button("Close", ImVec2(80, 0)))
				{
					dx12_rhi->errorString = "";
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}
		
		}
	
		ImGui::End();

		ImGui::Render();
		ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), dx12_rhi->GlobalCmdList->CmdList.Get());

	}

	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(backbuffer->resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));


	dx12_rhi->CmdQ->ExecuteCommandList(dx12_rhi->GlobalCmdList);

	dx12_rhi->EndFrame();

	PrevViewProjMat = ViewProjMat;
	PrevViewMat = ViewMat;

	PrevUnjitteredViewProjMat = UnjitteredViewProjMat;
}

void Corona::OnDestroy()
{
	dx12_rhi->CmdQ->WaitGPU();

	ImGui_ImplDX12_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
}

void Corona::OnKeyDown(UINT8 key)
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
	case 'I':
		bShowImgui = !bShowImgui;
		break;
	default:
		break;
	}

	m_camera.OnKeyDown(key);
}

void Corona::OnKeyUp(UINT8 key)
{
	m_camera.OnKeyUp(key);
}

struct ParallelDrawTaskSet : enki::ITaskSet
{
	Corona* app;
	UINT StartIndex;
	UINT ThisDraw;
	UINT ThreadIndex;
	//ThreadDescriptorHeapPool* DHPool;

	ParallelDrawTaskSet(){}
	ParallelDrawTaskSet(ParallelDrawTaskSet &&) {}
	ParallelDrawTaskSet(const ParallelDrawTaskSet&) = delete;

	virtual void ExecuteRange(enki::TaskSetPartition range, uint32_t threadnum)
	{
		//app->RecordDraw(StartIndex, ThisDraw, ThreadIndex, const_cast<ThreadDescriptorHeapPool*>(DHPool));
	}
};

void Corona::DrawScene(shared_ptr<Scene> scene, float Roughness, float Metalic, bool bOverrideRoughnessMetallic)
{
	for (auto& mesh : scene->meshes)
	{
		dx12_rhi->GlobalCmdList->CmdList->IASetIndexBuffer(&mesh->Ib->view);
		dx12_rhi->GlobalCmdList->CmdList->IASetVertexBuffers(0, 1, &mesh->Vb->view);

		for (int i = 0; i < mesh->Draws.size(); i++)
		{
			Mesh::DrawCall& drawcall = mesh->Draws[i];
			GBufferConstantBuffer objCB;
			int sizea = sizeof(GBufferConstantBuffer);

			objCB.ViewProjectionMatrix = glm::transpose(ViewProjMat);
			objCB.PrevViewProjectionMatrix = glm::transpose(PrevViewProjMat);

			//glm::mat4 m; // Identity matrix
			objCB.WorldMatrix = glm::transpose(mesh->transform);

			objCB.UnjitteredViewProjMat = glm::transpose(UnjitteredViewProjMat);
			objCB.PrevUnjitteredViewProjMat = glm::transpose(PrevUnjitteredViewProjMat);
			objCB.ViewDir.x = m_camera.m_lookDirection.x;
			objCB.ViewDir.y = m_camera.m_lookDirection.y;
			objCB.ViewDir.z = m_camera.m_lookDirection.z;

			objCB.RTSize.x = m_width;
			objCB.RTSize.y = m_height;

			objCB.RougnessMetalic.x = Roughness;
			objCB.RougnessMetalic.y = Metalic;

			objCB.bOverrideRougnessMetallic = bOverrideRoughnessMetallic ? 1 : 0;

			GBufferPassPSO->SetCBVValue("GBufferConstantBuffer", (void*)&objCB, dx12_rhi->GlobalCmdList->CmdList.Get());

			Texture* AlbedoTex = drawcall.mat->Diffuse.get();
			if (AlbedoTex)
				GBufferPassPSO->SetSRV("AlbedoTex", AlbedoTex->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());

			Texture* NormalTex = drawcall.mat->Normal.get();
			if (NormalTex)
				GBufferPassPSO->SetSRV("NormalTex", NormalTex->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());

			Texture* RoughnessTex = drawcall.mat->Roughness.get();
			if (RoughnessTex)
				GBufferPassPSO->SetSRV("RoughnessTex", RoughnessTex->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());

			Texture* MetallicTex = drawcall.mat->Metallic.get();
			if (MetallicTex)
				GBufferPassPSO->SetSRV("MetallicTex", MetallicTex->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());


			dx12_rhi->GlobalCmdList->CmdList->DrawIndexedInstanced(drawcall.IndexCount, 1, drawcall.IndexStart, drawcall.VertexBase, 0);
		}
	}
}

void Corona::GBufferPass()
{
#if USE_AFTERMATH
	NVAftermathMarker(dx12_rhi->AM_CL_Handle, "GBufferPass");
#endif
	PIXScopedEvent(dx12_rhi->GlobalCmdList->CmdList.Get(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "GBufferPass");

	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(AlbedoBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(NormalBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(GeomNormalBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(VelocityBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(RoughnessMetalicBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));

	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(DepthBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE));

	const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
	dx12_rhi->GlobalCmdList->CmdList->ClearRenderTargetView(AlbedoBuffer->CpuHandleRTV, clearColor, 0, nullptr);
	const float normalClearColor[] = { 0.0f, -0.1f, 0.0f, 0.0f };
	dx12_rhi->GlobalCmdList->CmdList->ClearRenderTargetView(NormalBuffer->CpuHandleRTV, normalClearColor, 0, nullptr);
	dx12_rhi->GlobalCmdList->CmdList->ClearRenderTargetView(GeomNormalBuffer->CpuHandleRTV, normalClearColor, 0, nullptr);
	const float velocityClearColor[] = { 0.0f, 0.0f, 0.0f, 0.0f };
	dx12_rhi->GlobalCmdList->CmdList->ClearRenderTargetView(VelocityBuffer->CpuHandleRTV, velocityClearColor, 0, nullptr);
	const float roughnessClearColor[] = { 0.001f, 0.0f, 0.0f, 0.0f };
	dx12_rhi->GlobalCmdList->CmdList->ClearRenderTargetView(RoughnessMetalicBuffer->CpuHandleRTV, roughnessClearColor, 0, nullptr);

	dx12_rhi->GlobalCmdList->CmdList->ClearDepthStencilView(DepthBuffer->CpuHandleDSV, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
	
	ID3D12DescriptorHeap* ppHeaps[] = { dx12_rhi->SRVCBVDescriptorHeapShaderVisible->DH.Get(), dx12_rhi->SamplerDescriptorHeapShaderVisible->DH.Get() };
	dx12_rhi->GlobalCmdList->CmdList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

	dx12_rhi->GlobalCmdList->CmdList->RSSetViewports(1, &m_viewport);
	dx12_rhi->GlobalCmdList->CmdList->RSSetScissorRects(1, &m_scissorRect);
	dx12_rhi->GlobalCmdList->CmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	
	const D3D12_CPU_DESCRIPTOR_HANDLE Rendertargets[] = { AlbedoBuffer->CpuHandleRTV, NormalBuffer->CpuHandleRTV, GeomNormalBuffer->CpuHandleRTV, VelocityBuffer->CpuHandleRTV, RoughnessMetalicBuffer->CpuHandleRTV };
	dx12_rhi->GlobalCmdList->CmdList->OMSetRenderTargets(GBufferPassPSO->graphicsPSODesc.NumRenderTargets, Rendertargets, FALSE, &DepthBuffer->CpuHandleDSV);

	GBufferPassPSO->Apply(dx12_rhi->GlobalCmdList->CmdList.Get());
	
	GBufferPassPSO->SetSampler("samplerWrap", samplerWrap.get(), dx12_rhi->GlobalCmdList->CmdList.Get());

	if (!bMultiThreadRendering)
	{

		
		DrawScene(Sponza, SponzaRoughnessMultiplier, 0, false);
		DrawScene(ShaderBall, ShaderBallRoughnessMultiplier, 1, true);
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
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(RoughnessMetalicBuffer->resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));


	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(DepthBuffer->resource.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
}

void Corona::SpatialDenoisingPass()
{
#if USE_AFTERMATH
	NVAftermathMarker(dx12_rhi->AM_CL_Handle, "SpatialDenoisingPass");
#endif
	PIXScopedEvent(dx12_rhi->GlobalCmdList->CmdList.Get(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "SpatialDenoisingPass");

	UINT WriteIndex = 0;
	UINT ReadIndex = 1;
	for (int i = 0; i < 4; i++)
	{
		WriteIndex = 1 - WriteIndex; // 1
		ReadIndex = 1 - WriteIndex; // 0

		dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(DiffuseGISHSpatial[WriteIndex]->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
		dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(DiffuseGICoCgSpatial[WriteIndex]->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

		SpatialDenoisingFilterPSO->Apply(dx12_rhi->GlobalCmdList->CmdList.Get());

		SpatialDenoisingFilterPSO->SetSRV("DepthTex", DepthBuffer->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
		SpatialDenoisingFilterPSO->SetSRV("GeoNormalTex", GeomNormalBuffer->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
		SpatialDenoisingFilterPSO->SetSRV("InGIResultSHTex", DiffuseGISHSpatial[ReadIndex]->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
		SpatialDenoisingFilterPSO->SetSRV("InGIResultColorTex", DiffuseGICoCgSpatial[ReadIndex]->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
		SpatialDenoisingFilterPSO->SetSRV("InSpecualrGITex", SpeculaGIBufferSpatial[ReadIndex]->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());


		SpatialDenoisingFilterPSO->SetUAV("OutGIResultSH", DiffuseGISHSpatial[WriteIndex]->GpuHandleUAV, dx12_rhi->GlobalCmdList->CmdList.Get());
		SpatialDenoisingFilterPSO->SetUAV("OutGIResultColor", DiffuseGICoCgSpatial[WriteIndex]->GpuHandleUAV, dx12_rhi->GlobalCmdList->CmdList.Get());
		SpatialDenoisingFilterPSO->SetUAV("OutSpecularGI", SpeculaGIBufferSpatial[WriteIndex]->GpuHandleUAV, dx12_rhi->GlobalCmdList->CmdList.Get());

		SpatialFilterCB.Iteration = i;
		SpatialDenoisingFilterPSO->SetCBVValue("SpatialFilterConstant", &SpatialFilterCB, dx12_rhi->GlobalCmdList->CmdList.Get());

		UINT WidthGI = m_width / GIBufferScale;
		UINT HeightGI = m_height / GIBufferScale;

		dx12_rhi->GlobalCmdList->CmdList->Dispatch(WidthGI / 32, HeightGI / 32 + 1 , 1);

		dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(DiffuseGISHSpatial[WriteIndex]->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
		dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(DiffuseGICoCgSpatial[WriteIndex]->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
	}
}

void Corona::TemporalDenoisingPass()
{
#if USE_AFTERMATH
	NVAftermathMarker(dx12_rhi->AM_CL_Handle, "TemporalDenoisingPass");
#endif
	PIXScopedEvent(dx12_rhi->GlobalCmdList->CmdList.Get(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "TemporalDenoisingPass");

	// GIBufferSH : full scale
	// FilterIndirectDiffusePingPongSH : 3x3 downsample
	GIBufferWriteIndex = 1 - GIBufferWriteIndex;
	UINT WriteIndex = GIBufferWriteIndex;
	UINT ReadIndex = 1 - WriteIndex;

	// first pass
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(DiffuseGISHSpatial[0]->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(DiffuseGICoCgSpatial[0]->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(DiffuseGISHTemporal[WriteIndex]->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(DiffuseGICoCgTemporal[WriteIndex]->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(SpeculaGIBufferTemporal[WriteIndex]->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(SpeculaGIBufferSpatial[0]->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));


	TemporalDenoisingFilterPSO->Apply(dx12_rhi->GlobalCmdList->CmdList.Get());

	TemporalDenoisingFilterPSO->SetSRV("DepthTex", DepthBuffer->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
	TemporalDenoisingFilterPSO->SetSRV("GeoNormalTex", NormalBuffer->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
	TemporalDenoisingFilterPSO->SetSRV("InGIResultSHTex", DiffuseGISHRaw->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
	TemporalDenoisingFilterPSO->SetSRV("InGIResultColorTex", DiffuseGICoCgRaw->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
	TemporalDenoisingFilterPSO->SetSRV("InGIResultSHTexPrev", DiffuseGISHTemporal[ReadIndex]->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
	TemporalDenoisingFilterPSO->SetSRV("InGIResultColorTexPrev", DiffuseGICoCgTemporal[ReadIndex]->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
	TemporalDenoisingFilterPSO->SetSRV("VelocityTex", VelocityBuffer->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
	TemporalDenoisingFilterPSO->SetSRV("InSpecularGITex", SpeculaGIBufferRaw->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());
	TemporalDenoisingFilterPSO->SetSRV("InSpecularGITexPrev", SpeculaGIBufferTemporal[ReadIndex]->GpuHandleSRV, dx12_rhi->GlobalCmdList->CmdList.Get());


	TemporalDenoisingFilterPSO->SetUAV("OutGIResultSH", DiffuseGISHTemporal[WriteIndex]->GpuHandleUAV, dx12_rhi->GlobalCmdList->CmdList.Get());
	TemporalDenoisingFilterPSO->SetUAV("OutGIResultColor", DiffuseGICoCgTemporal[WriteIndex]->GpuHandleUAV, dx12_rhi->GlobalCmdList->CmdList.Get());
	TemporalDenoisingFilterPSO->SetUAV("OutGIResultSHDS", DiffuseGISHSpatial[0]->GpuHandleUAV, dx12_rhi->GlobalCmdList->CmdList.Get());
	TemporalDenoisingFilterPSO->SetUAV("OutGIResultColorDS", DiffuseGICoCgSpatial[0]->GpuHandleUAV, dx12_rhi->GlobalCmdList->CmdList.Get());
	TemporalDenoisingFilterPSO->SetUAV("OutSpecularGI", SpeculaGIBufferTemporal[WriteIndex]->GpuHandleUAV, dx12_rhi->GlobalCmdList->CmdList.Get());
	TemporalDenoisingFilterPSO->SetUAV("OutSpecularGIDS", SpeculaGIBufferSpatial[0]->GpuHandleUAV, dx12_rhi->GlobalCmdList->CmdList.Get());



	TemporalDenoisingFilterPSO->SetCBVValue("TemporalFilterConstant", &TemporalFilterCB, dx12_rhi->GlobalCmdList->CmdList.Get());

	dx12_rhi->GlobalCmdList->CmdList->Dispatch(m_width / 15 , m_height / 15, 1);

	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(DiffuseGISHSpatial[0]->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(DiffuseGICoCgSpatial[0]->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(DiffuseGISHTemporal[WriteIndex]->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(DiffuseGICoCgTemporal[WriteIndex]->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(SpeculaGIBufferTemporal[WriteIndex]->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(SpeculaGIBufferSpatial[0]->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
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

void Corona::RecompileShaders()
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
	InitBloomPass();
	InitGenMipSpecularGIPass();
}

void Corona::InitRaytracingData()
{
	UINT NumTotalMesh = Sponza->meshes.size() + ShaderBall->meshes.size();
	vecBLAS.reserve(NumTotalMesh);

	AddMeshToVec(vecBLAS, Sponza);
	AddMeshToVec(vecBLAS, ShaderBall);

	TLAS = dx12_rhi->CreateTLAS(vecBLAS);

	InstancePropertyBuffer = dx12_rhi->CreateBuffer(500, sizeof(InstanceProperty), D3D12_RESOURCE_STATE_GENERIC_READ, false);
	InstancePropertyBuffer->MakeByteAddressBufferSRV();

	NAME_D3D12_OBJECT(InstancePropertyBuffer->resource);


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

void Corona::InitRTPSO()
{
	// create shadow rtpso
	{
		shared_ptr<RTPipelineStateObject> TEMP_PSO_RT_SHADOW = shared_ptr<RTPipelineStateObject>(new RTPipelineStateObject);

		TEMP_PSO_RT_SHADOW->NumInstance = vecBLAS.size();// scene->meshes.size(); // important for cbv allocation & shadertable size.

		// new interface
		TEMP_PSO_RT_SHADOW->AddHitGroup("HitGroup", "", "anyhit");
		TEMP_PSO_RT_SHADOW->AddShader("rayGen", RTPipelineStateObject::RAYGEN);

		TEMP_PSO_RT_SHADOW->BindUAV("global", "ShadowResult", 0);
		TEMP_PSO_RT_SHADOW->BindSRV("global", "gRtScene", 0);
		TEMP_PSO_RT_SHADOW->BindSRV("global", "DepthTex", 1);
		TEMP_PSO_RT_SHADOW->BindSRV("global", "WorldNormalTex", 2);

		TEMP_PSO_RT_SHADOW->BindCBV("global", "ViewParameter", 0, sizeof(RTShadowViewParamCB), 1);
		TEMP_PSO_RT_SHADOW->BindSampler("global", "samplerWrap", 0);



		TEMP_PSO_RT_SHADOW->AddShader("miss", RTPipelineStateObject::MISS);
		
		TEMP_PSO_RT_SHADOW->AddShader("anyhit", RTPipelineStateObject::ANYHIT);
		TEMP_PSO_RT_SHADOW->BindSRV("anyhit", "vertices", 3);
		TEMP_PSO_RT_SHADOW->BindSRV("anyhit", "indices", 4);
		TEMP_PSO_RT_SHADOW->BindSRV("anyhit", "AlbedoTex", 5);
		TEMP_PSO_RT_SHADOW->BindSRV("anyhit", "InstanceProperty", 6);

		TEMP_PSO_RT_SHADOW->MaxRecursion = 1;
		TEMP_PSO_RT_SHADOW->MaxAttributeSizeInBytes = sizeof(float) * 2;
		TEMP_PSO_RT_SHADOW->MaxPayloadSizeInBytes = sizeof(float) * 2;

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
		//TEMP_PSO_RT_REFLECTION->AddHitGroup("ShadowHitGroup", "chsShadow", "");


		TEMP_PSO_RT_REFLECTION->AddShader("rayGen", RTPipelineStateObject::RAYGEN);
		
		TEMP_PSO_RT_REFLECTION->BindUAV("global", "ReflectionResult", 0);
		TEMP_PSO_RT_REFLECTION->BindSRV("global", "gRtScene", 0);
		TEMP_PSO_RT_REFLECTION->BindSRV("global", "DepthTex", 1);
		TEMP_PSO_RT_REFLECTION->BindSRV("global", "GeoNormalTex", 2);
		TEMP_PSO_RT_REFLECTION->BindSRV("global", "RougnessMetallicTex", 6);
		TEMP_PSO_RT_REFLECTION->BindSRV("global", "BlueNoiseTex", 7);
		TEMP_PSO_RT_REFLECTION->BindSRV("global", "WorldNormalTex", 8);


		TEMP_PSO_RT_REFLECTION->BindCBV("global", "ViewParameter", 0, sizeof(RTReflectionViewParam), 1);
		TEMP_PSO_RT_REFLECTION->BindSampler("global", "samplerWrap", 0);

		TEMP_PSO_RT_REFLECTION->AddShader("miss", RTPipelineStateObject::MISS);
		TEMP_PSO_RT_REFLECTION->AddShader("missShadow", RTPipelineStateObject::MISS);


		TEMP_PSO_RT_REFLECTION->AddShader("chs", RTPipelineStateObject::HIT);
		TEMP_PSO_RT_REFLECTION->BindSRV("chs", "vertices", 3);
		TEMP_PSO_RT_REFLECTION->BindSRV("chs", "indices", 4);
		TEMP_PSO_RT_REFLECTION->BindSRV("chs", "AlbedoTex", 5);
		TEMP_PSO_RT_REFLECTION->BindSRV("chs", "InstanceProperty", 9);

		TEMP_PSO_RT_REFLECTION->MaxRecursion = 1;
		TEMP_PSO_RT_REFLECTION->MaxAttributeSizeInBytes = sizeof(float) * 2;
		TEMP_PSO_RT_REFLECTION->MaxPayloadSizeInBytes = sizeof(float) * 12;


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
		
		TEMP_PSO_RT_GI->BindUAV("global", "GIResultSH", 0);
		TEMP_PSO_RT_GI->BindUAV("global", "GIResultColor", 1);
		TEMP_PSO_RT_GI->BindSRV("global", "gRtScene", 0);
		TEMP_PSO_RT_GI->BindSRV("global", "DepthTex", 1);
		TEMP_PSO_RT_GI->BindSRV("global", "WorldNormalTex", 2);
		TEMP_PSO_RT_GI->BindCBV("global", "ViewParameter", 0, sizeof(RTGIViewParam), 1);
		TEMP_PSO_RT_GI->BindSampler("global", "samplerWrap", 0);
		TEMP_PSO_RT_GI->BindSRV("global", "BlueNoiseTex", 7);

		TEMP_PSO_RT_GI->AddShader("miss", RTPipelineStateObject::MISS);
		TEMP_PSO_RT_GI->AddShader("missShadow", RTPipelineStateObject::MISS);


		TEMP_PSO_RT_GI->AddShader("chs", RTPipelineStateObject::HIT);
		TEMP_PSO_RT_GI->BindSRV("chs", "vertices", 3);
		TEMP_PSO_RT_GI->BindSRV("chs", "indices", 4);
		TEMP_PSO_RT_GI->BindSRV("chs", "AlbedoTex", 5);
		TEMP_PSO_RT_GI->BindSRV("chs", "InstanceProperty", 6);

		TEMP_PSO_RT_GI->MaxRecursion = 1;
		TEMP_PSO_RT_GI->MaxAttributeSizeInBytes = sizeof(float) * 2;

		TEMP_PSO_RT_GI->MaxPayloadSizeInBytes = sizeof(float) * 12;

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
void Corona::RaytraceShadowPass()
{
#if USE_AFTERMATH
	NVAftermathMarker(dx12_rhi->AM_CL_Handle, "RaytraceShadowPass");
#endif
	PIXScopedEvent(dx12_rhi->GlobalCmdList->CmdList.Get(), PIX_COLOR(rand()%255, rand() % 255, rand() % 255), "RaytraceShadowPass");

	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(ShadowBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

	PSO_RT_SHADOW->NumInstance = vecBLAS.size();

	PSO_RT_SHADOW->BeginShaderTable();

	int i = 0;
	for (auto&as : vecBLAS)
	{
		auto& mesh = as->mesh;
		Texture* diffuseTex = mesh->Draws[0].mat->Diffuse.get();

		if (!diffuseTex)
			diffuseTex = DefaultWhiteTex.get();

		PSO_RT_SHADOW->ResetHitProgram(i);
		PSO_RT_SHADOW->StartHitProgram("HitGroup", i);

		PSO_RT_SHADOW->AddDescriptor2HitProgram("HitGroup", mesh->Vb->GpuHandleSRV, i);
		PSO_RT_SHADOW->AddDescriptor2HitProgram("HitGroup", mesh->Ib->GpuHandleSRV, i);
		PSO_RT_SHADOW->AddDescriptor2HitProgram("HitGroup", diffuseTex->GpuHandleSRV, i);
		PSO_RT_SHADOW->AddDescriptor2HitProgram("HitGroup", InstancePropertyBuffer->GpuHandleSRV, i);

		i++;
	}

	PSO_RT_SHADOW->SetUAV("global", "ShadowResult", ShadowBuffer->GpuHandleUAV);
	PSO_RT_SHADOW->SetSRV("global", "gRtScene", TLAS->GPUHandle);
	PSO_RT_SHADOW->SetSRV("global", "DepthTex", DepthBuffer->GpuHandleSRV);
	PSO_RT_SHADOW->SetSRV("global", "WorldNormalTex", GeomNormalBuffer->GpuHandleSRV);
	PSO_RT_SHADOW->SetCBVValue("global", "ViewParameter", &RTShadowViewParam);
	PSO_RT_SHADOW->SetSampler("global", "samplerWrap", samplerWrap.get());


	PSO_RT_SHADOW->EndShaderTable();

	PSO_RT_SHADOW->Apply(m_width, m_height, dx12_rhi->GlobalCmdList);

	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(ShadowBuffer->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
}

void Corona::RaytraceReflectionPass()
{
#if USE_AFTERMATH
	NVAftermathMarker(dx12_rhi->AM_CL_Handle, "RaytraceReflectionPass");
#endif
	PIXScopedEvent(dx12_rhi->GlobalCmdList->CmdList.Get(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "RaytraceReflectionPass");

	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(SpeculaGIBufferRaw->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

	PSO_RT_REFLECTION->NumInstance = vecBLAS.size();
	PSO_RT_REFLECTION->BeginShaderTable();

	PSO_RT_REFLECTION->SetUAV("global", "ReflectionResult", SpeculaGIBufferRaw->GpuHandleUAV);
	PSO_RT_REFLECTION->SetSRV("global", "gRtScene", TLAS->GPUHandle);
	PSO_RT_REFLECTION->SetSRV("global", "DepthTex", DepthBuffer->GpuHandleSRV);
	PSO_RT_REFLECTION->SetSRV("global", "GeoNormalTex", GeomNormalBuffer->GpuHandleSRV);
	PSO_RT_REFLECTION->SetSRV("global", "RougnessMetallicTex", RoughnessMetalicBuffer->GpuHandleSRV);
	PSO_RT_REFLECTION->SetSRV("global", "BlueNoiseTex", BlueNoiseTex->GpuHandleSRV);
	PSO_RT_REFLECTION->SetSRV("global", "WorldNormalTex", NormalBuffer->GpuHandleSRV);

	RTReflectionViewParam.ViewSpreadAngle = glm::tan(Fov * 0.5) / (0.5f * m_height);
	PSO_RT_REFLECTION->SetCBVValue("global", "ViewParameter", &RTReflectionViewParam);
	PSO_RT_REFLECTION->SetSampler("global", "samplerWrap", samplerWrap.get());


	int i = 0;
	for(auto&as : vecBLAS)
	{
		auto& mesh = as->mesh;
		Texture* diffuseTex = mesh->Draws[0].mat->Diffuse.get();

		if (!diffuseTex)
			diffuseTex = DefaultWhiteTex.get();
		PSO_RT_REFLECTION->ResetHitProgram(i);

		PSO_RT_REFLECTION->StartHitProgram("HitGroup", i);
		PSO_RT_REFLECTION->AddDescriptor2HitProgram("HitGroup", mesh->Vb->GpuHandleSRV, i);
		PSO_RT_REFLECTION->AddDescriptor2HitProgram("HitGroup", mesh->Ib->GpuHandleSRV, i);
		PSO_RT_REFLECTION->AddDescriptor2HitProgram("HitGroup", diffuseTex->GpuHandleSRV, i);
		PSO_RT_REFLECTION->AddDescriptor2HitProgram("HitGroup", InstancePropertyBuffer->GpuHandleSRV, i);

		//PSO_RT_REFLECTION->StartHitProgram("ShadowHitGroup", i);
		/*
		PSO_RT_REFLECTION->AddDescriptor2HitProgram("ShadowHitGroup", mesh->Vb->GpuHandleSRV, i);
		PSO_RT_REFLECTION->AddDescriptor2HitProgram("ShadowHitGroup", mesh->Ib->GpuHandleSRV, i);
		PSO_RT_REFLECTION->AddDescriptor2HitProgram("ShadowHitGroup", diffuseTex->GpuHandleSRV, i);
		PSO_RT_REFLECTION->AddDescriptor2HitProgram("ShadowHitGroup", InstancePropertyBuffer->GpuHandleSRV, i);*/
		i++;
	}

	PSO_RT_REFLECTION->EndShaderTable();

	PSO_RT_REFLECTION->Apply(m_width, m_height, dx12_rhi->GlobalCmdList);

	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(SpeculaGIBufferRaw->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
	PIXEndEvent();
}

void Corona::RaytraceGIPass()
{
#if USE_AFTERMATH
	NVAftermathMarker(dx12_rhi->AM_CL_Handle, "RaytraceGIPass");
#endif
	PIXScopedEvent(dx12_rhi->GlobalCmdList->CmdList.Get(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "RaytraceGIPass");

	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(DiffuseGISHRaw->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(DiffuseGICoCgRaw->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

	PSO_RT_GI->NumInstance = vecBLAS.size();
	PSO_RT_GI->BeginShaderTable();

	PSO_RT_GI->SetUAV("global", "GIResultSH", DiffuseGISHRaw->GpuHandleUAV);
	PSO_RT_GI->SetUAV("global", "GIResultColor", DiffuseGICoCgRaw->GpuHandleUAV);
	PSO_RT_GI->SetSRV("global", "gRtScene", TLAS->GPUHandle);
	PSO_RT_GI->SetSRV("global", "DepthTex", DepthBuffer->GpuHandleSRV);
	PSO_RT_GI->SetSRV("global", "WorldNormalTex", NormalBuffer->GpuHandleSRV);
	PSO_RT_GI->SetSRV("global", "BlueNoiseTex", BlueNoiseTex->GpuHandleSRV);
	
	RTGIViewParam.ViewSpreadAngle = glm::tan(Fov * 0.5) / (0.5f * m_height);
	PSO_RT_GI->SetCBVValue("global", "ViewParameter", &RTGIViewParam);
	PSO_RT_GI->SetSampler("global", "samplerWrap", samplerWrap.get());

	int i = 0;
	for(auto&as : vecBLAS)
	{
		auto& mesh = as->mesh;
		
		Texture* diffuseTex = mesh->Draws[0].mat->Diffuse.get();
		if (!diffuseTex)
			diffuseTex = DefaultWhiteTex.get();

		PSO_RT_GI->ResetHitProgram(i);

		PSO_RT_GI->StartHitProgram("HitGroup", i);
		PSO_RT_GI->AddDescriptor2HitProgram("HitGroup", mesh->Vb->GpuHandleSRV, i);
		PSO_RT_GI->AddDescriptor2HitProgram("HitGroup", mesh->Ib->GpuHandleSRV, i);
		PSO_RT_GI->AddDescriptor2HitProgram("HitGroup", diffuseTex->GpuHandleSRV, i);
		PSO_RT_GI->AddDescriptor2HitProgram("HitGroup", InstancePropertyBuffer->GpuHandleSRV, i);

		/*PSO_RT_GI->StartHitProgram("ShadowHitGroup", i);
		PSO_RT_GI->AddDescriptor2HitProgram("ShadowHitGroup", mesh->Vb->GpuHandleSRV, i);
		PSO_RT_GI->AddDescriptor2HitProgram("ShadowHitGroup", mesh->Ib->GpuHandleSRV, i);
		PSO_RT_GI->AddDescriptor2HitProgram("ShadowHitGroup", diffuseTex->GpuHandleSRV, i);
		PSO_RT_GI->AddDescriptor2HitProgram("ShadowHitGroup", InstancePropertyBuffer->GpuHandleSRV, i);*/

		i++;
	}

	PSO_RT_GI->EndShaderTable();


	PSO_RT_GI->Apply(m_width, m_height, dx12_rhi->GlobalCmdList);

	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(DiffuseGISHRaw->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
	dx12_rhi->GlobalCmdList->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(DiffuseGICoCgRaw->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
}
