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

#if USE_AFTERMATH
#include "GFSDK_Aftermath/include/GFSDK_Aftermath.h"
#endif // USE_AFTERMATH

#if USE_IMGUI
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"
#include "imGuIZMO.h"
#endif // USE_IMGUI

#if USE_NRD
#include "NRIDescs.hpp"
#include "Helper.h"
#define PROPS_D3D12
#include "MathLib.h"
#include "NRDIntegration.hpp"
#endif //USE_NRD


#ifdef _DEBUG
#define new DEBUG_CLIENTBLOCK
#endif

#define arraysize(a) (sizeof(a)/sizeof(a[0]))
#define align_to(_alignment, _val) (((_val + _alignment - 1) / _alignment) * _alignment)

static dxc::DxcDllSupport gDxcDllHelper;

using namespace glm;

Corona::Corona(UINT width, UINT height, UINT renderWidth, UINT renderHeight, std::wstring name) :
	DXSample(width, height, name),
	m_scissorRect{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) }
{
	RenderWidth = renderWidth;
	RenderHeight = renderHeight;

	DisplayWidth = width;
	DisplayHeight = height;

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
	//_CrtSetBreakAlloc(4222159);


	g_TS.Initialize(8);


	m_camera.Init({ 458, 781, 185 });
	m_camera.SetMoveSpeed(200);

	LoadPipeline();

#if USE_DLSS
	InitDLSS();
#endif

	LoadAssets();
}

void Corona::LoadPipeline()
{
	gfx_api = std::unique_ptr<GfxAPI>(AbstractGfxLayer::CreateDX12API(Win32Application::GetHwnd(), DisplayWidth, DisplayHeight));
	framebuffers.clear();
	AbstractGfxLayer::GetFrameBuffers(framebuffers);
}

void Corona::LoadAssets()
{
	InitBlueNoiseTexture();

#if USE_IMGUI
	InitImgui();
#endif

	InitGBufferPass();
	InitToneMapPass();
	InitDebugPass();
	InitLightingPass();
	InitTemporalAAPass();
	InitSpatialDenoisingPass();
	InitTemporalDenoisingPass();
	InitBloomPass();
	InitResolvePixelVelocityPass();

#if USE_RTXGI
	InitRTXGI();
#endif

#if USE_NRD
	InitNRD();
#endif
	InitRTPSO();

	
	struct PostVertex
	{
		glm::vec4 position;
		glm::vec2 uv;
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

	FullScreenVB = shared_ptr<GfxVertexBuffer>(AbstractGfxLayer::CreateVertexBuffer(vertexBufferSize, vertexBufferStride, &quadVertices));

	
	// TAA pingping buffer
	ColorBuffers[0] = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R16G16B16A16_FLOAT,
		RESOURCE_FLAG_ALLOW_RENDER_TARGET | RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RenderWidth, RenderHeight, 1));

	NAME_TEXTURE(ColorBuffers[0]);


	ColorBuffers[1] = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R16G16B16A16_FLOAT,
		RESOURCE_FLAG_ALLOW_RENDER_TARGET | RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RenderWidth, RenderHeight, 1));

	NAME_TEXTURE(ColorBuffers[1]);


	// lighting result
	LightingBuffer = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R16G16B16A16_FLOAT,
		RESOURCE_FLAG_ALLOW_RENDER_TARGET,
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RenderWidth, RenderHeight, 1));

	NAME_TEXTURE(LightingBuffer);


	LightingWithBloomBuffer = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R16G16B16A16_FLOAT,
		RESOURCE_FLAG_ALLOW_RENDER_TARGET,
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RenderWidth, RenderHeight, 1));

	NAME_TEXTURE(LightingWithBloomBuffer);


	// world normal
	NormalBuffers[0] = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R16G16B16A16_FLOAT,
		RESOURCE_FLAG_ALLOW_RENDER_TARGET,
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RenderWidth, RenderHeight, 1, glm::vec4(0.0f, -0.1f, 0.0f, 0.0f)));

	NAME_TEXTURE(NormalBuffers[0]);

	NormalBuffers[1] = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R16G16B16A16_FLOAT,
		RESOURCE_FLAG_ALLOW_RENDER_TARGET,
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RenderWidth, RenderHeight, 1, glm::vec4(0.0f, -0.1f, 0.0f, 0.0f)));

	NAME_TEXTURE(NormalBuffers[1]);

	// geometry world normal
	GeomNormalBuffer = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R16G16B16A16_FLOAT,
		RESOURCE_FLAG_ALLOW_RENDER_TARGET,
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RenderWidth, RenderHeight, 1, glm::vec4(0.0f, -0.1f, 0.0f, 0.0f)));

	NAME_TEXTURE(GeomNormalBuffer);

	// shadow result
	ShadowBuffer = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R8G8B8A8_UNORM,
		RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RenderWidth, RenderHeight, 1));

	NAME_TEXTURE(ShadowBuffer);

	// refleciton result
	SpeculaGIBufferRaw = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R16G16B16A16_FLOAT,
		RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RenderWidth, RenderHeight, 1));

	NAME_TEXTURE(SpeculaGIBufferRaw);

	SpeculaGIBufferTemporal[0] = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R16G16B16A16_FLOAT,
		RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RenderWidth, RenderHeight, 1));

	NAME_TEXTURE(SpeculaGIBufferTemporal[0]);

	SpeculaGIBufferTemporal[1] = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R16G16B16A16_FLOAT,
		RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RenderWidth, RenderHeight, 1));

	NAME_TEXTURE(SpeculaGIBufferTemporal[1]);

	// moments
	SpeculaGIMoments[0] = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R16G16_FLOAT,
		RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RenderWidth, RenderHeight, 1));

	NAME_TEXTURE(SpeculaGIMoments[0]);

	SpeculaGIMoments[1] = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R16G16_FLOAT,
		RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RenderWidth, RenderHeight, 1));

	NAME_TEXTURE(SpeculaGIMoments[1]);
	// diffuse gi

	DiffuseGISHRaw = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R16G16B16A16_FLOAT,
		RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RenderWidth, RenderHeight, 1));

	NAME_TEXTURE(DiffuseGISHRaw);

	DiffuseGICoCgRaw = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R16G16B16A16_FLOAT,
		RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RenderWidth, RenderHeight, 1));

	NAME_TEXTURE(DiffuseGICoCgRaw);

	// gi result sh
	DiffuseGISHTemporal[0] = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R16G16B16A16_FLOAT,
		RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RenderWidth, RenderHeight, 1));

	NAME_TEXTURE(DiffuseGISHTemporal[0]);

	DiffuseGISHTemporal[1] = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R16G16B16A16_FLOAT,
		RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RenderWidth, RenderHeight, 1));

	NAME_TEXTURE(DiffuseGISHTemporal[1]);

	// gi result color
	DiffuseGICoCgTemporal[0] = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R16G16B16A16_FLOAT,
		RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RenderWidth, RenderHeight, 1));

	NAME_TEXTURE(DiffuseGICoCgTemporal[0]);

	DiffuseGICoCgTemporal[1] = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R16G16B16A16_FLOAT,
		RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RenderWidth, RenderHeight, 1));

	NAME_TEXTURE(DiffuseGICoCgTemporal[1]);

	// NRD result buffers
	// normal roughness for NRD input
	NormalRoughness_NRD = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R16G16B16A16_UNORM,
		RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RenderWidth, RenderHeight, 1));
	NAME_TEXTURE(NormalRoughness_NRD);

	//  LinearDepth_NRD
	LinearDepth_NRD = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R32_FLOAT,
		RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, 
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RenderWidth, RenderHeight, 1));
	NAME_TEXTURE(LinearDepth_NRD);
	
	// sh
	DiffuseGI_NRD = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R16G16B16A16_FLOAT,
		RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RenderWidth, RenderHeight, 1));

	NAME_TEXTURE(DiffuseGI_NRD);

	// spec
	SpecularGI_NRD = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R16G16B16A16_FLOAT,
		RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RenderWidth, RenderHeight, 1));

	NAME_TEXTURE(SpecularGI_NRD);

	// albedo
	AlbedoBuffer = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R8G8B8A8_UNORM,
		RESOURCE_FLAG_ALLOW_RENDER_TARGET,
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RenderWidth, RenderHeight, 1));

	NAME_TEXTURE(AlbedoBuffer);

	// velocity
	VelocityBuffer = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R16G16B16A16_FLOAT,
		RESOURCE_FLAG_ALLOW_RENDER_TARGET,
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RenderWidth, RenderHeight, 1, glm::vec4(0.0f, 0.0f, 0.0f, 0.0f)));
	NAME_TEXTURE(VelocityBuffer);

	// pixel velocity
	PixelVelocityBuffer = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R16G16_FLOAT,
		RESOURCE_FLAG_ALLOW_RENDER_TARGET,
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RenderWidth, RenderHeight, 1, glm::vec4(0.0f, 0.0f, 0.0f, 0.0f)));
	NAME_TEXTURE(PixelVelocityBuffer);

	// pbr material
	RoughnessMetalicBuffer = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R8G8B8A8_UNORM,
		RESOURCE_FLAG_ALLOW_RENDER_TARGET,
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RenderWidth, RenderHeight, 1, glm::vec4(0.001f, 0.0f, 0.0f, 0.0f )));

	NAME_TEXTURE(RoughnessMetalicBuffer);

	// depth 
	DepthBuffer = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R32_TYPELESS,
		RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, 
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RenderWidth, RenderHeight, 1));

	//NAME_TEXTURE(DepthBuffer);

	


	UnjitteredDepthBuffers[0] = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R32_FLOAT, 
		RESOURCE_FLAG_ALLOW_RENDER_TARGET, 
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RenderWidth, RenderHeight, 1, glm::vec4(1.0f, 1.0f, 1.0f, 1.0f)));
	NAME_TEXTURE(UnjitteredDepthBuffers[0]);

	UnjitteredDepthBuffers[1] = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R32_FLOAT, 
		RESOURCE_FLAG_ALLOW_RENDER_TARGET, 
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RenderWidth, RenderHeight, 1, glm::vec4(1.0f, 1.0f, 1.0f, 1.0f)));
	NAME_TEXTURE(UnjitteredDepthBuffers[1]);

	DefaultWhiteTex = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTextureFromFile(L"assets/default/default_white.png", false));
	DefaultBlackTex = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTextureFromFile(L"assets/default/default_black.png", false));
	DefaultNormalTex = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTextureFromFile(L"assets/default/default_normal.png", true));
	DefaultRougnessTex = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTextureFromFile(L"assets/default/default_roughness.png", true));

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
		SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = FILTER_ANISOTROPIC;
		samplerDesc.AddressU = TEXTURE_ADDRESS_MODE_WRAP;
		samplerDesc.AddressV = TEXTURE_ADDRESS_MODE_WRAP;
		samplerDesc.AddressW = TEXTURE_ADDRESS_MODE_WRAP;
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
		samplerDesc.MipLODBias = -1.0f;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.ComparisonFunc = COMPARISON_FUNC_ALWAYS;

		samplerAnisoWrap = shared_ptr<GfxSampler>(AbstractGfxLayer::CreateSampler(samplerDesc));

	}

	{
		SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = FILTER_MIN_MAG_LINEAR_MIP_POINT;
		samplerDesc.AddressU = TEXTURE_ADDRESS_MODE_CLAMP;
		samplerDesc.AddressV = TEXTURE_ADDRESS_MODE_CLAMP;
		samplerDesc.AddressW = TEXTURE_ADDRESS_MODE_CLAMP;
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
		samplerDesc.MipLODBias = -1.0f;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.ComparisonFunc = COMPARISON_FUNC_ALWAYS;

		samplerBilinearWrap = shared_ptr<GfxSampler>(AbstractGfxLayer::CreateSampler(samplerDesc));

	}

	{
		SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = TEXTURE_ADDRESS_MODE_CLAMP;
		samplerDesc.AddressV = TEXTURE_ADDRESS_MODE_CLAMP;
		samplerDesc.AddressW = TEXTURE_ADDRESS_MODE_CLAMP;
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
		samplerDesc.MipLODBias = -1.0f;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.ComparisonFunc = COMPARISON_FUNC_ALWAYS;

		samplerTrilinearClamp = shared_ptr<GfxSampler>(AbstractGfxLayer::CreateSampler(samplerDesc));

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
		GfxMaterial* mat = new GfxMaterial;
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
			mat->Diffuse = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTextureFromFile(dir + wDiffuseTex, false));
		}

		if (!mat->Diffuse)
			mat->Diffuse = DefaultWhiteTex;
		
		if (aiMat.GetTexture(aiTextureType_NORMALS, 0, &normalMapPath) == aiReturn_SUCCESS
			|| aiMat.GetTexture(aiTextureType_HEIGHT, 0, &normalMapPath) == aiReturn_SUCCESS)
			wNormalTex = GetFileName(AnsiToWString(normalMapPath.C_Str()).c_str());

		if (wNormalTex.length() != 0)
		{
			mat->Normal = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTextureFromFile(dir + wNormalTex, true));
		}

		if (!mat->Normal)
			mat->Normal = DefaultNormalTex;

		if (aiMat.GetTexture(aiTextureType_AMBIENT, 0, &metallicMapPath) == aiReturn_SUCCESS)
			wMetallicTex = GetFileName(AnsiToWString(metallicMapPath.C_Str()).c_str());
		if (wMetallicTex.length() != 0)
		{
			mat->Metallic = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTextureFromFile(dir + wMetallicTex, true));
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
				mat->Roughness = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTextureFromFile(dir + wRoughnessTex, true));
			}
		}

		if (!mat->Roughness)
		{
			mat->Roughness = DefaultRougnessTex;
		}

		// HACK!
		if (wDiffuseTex == L"Sponza_Thorn_diffuse.png" || wDiffuseTex == L"VasePlant_diffuse.png" || wDiffuseTex == L"ChainTexture_Albedo.png")
			mat->bHasAlpha = true;

		scene->Materials.push_back(shared_ptr<GfxMaterial>(mat));
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

		GfxMesh* mesh = new GfxMesh;

		mesh->NumVertices = asMesh->mNumVertices;
		mesh->NumIndices = asMesh->mNumFaces * 3;

		vector<Vertex> vertices;
		vertices.resize(mesh->NumVertices);

		vector<UINT16> indices;
		indices.resize(mesh->NumIndices);

		if (asMesh->HasPositions())
		{
			for (int i = 0; i < mesh->NumVertices; ++i)
			{
				vertices[i].Position.x = asMesh->mVertices[i].x;
				vertices[i].Position.y = asMesh->mVertices[i].y;
				vertices[i].Position.z = asMesh->mVertices[i].z;

				scene->AABBMin = glm::min(scene->AABBMin, vertices[i].Position);
				scene->AABBMax = glm::max(scene->AABBMax, vertices[i].Position);
				scene->BoundingRadius = glm::max(scene->BoundingRadius, glm::length(scene->AABBMin));
				scene->BoundingRadius = glm::max(scene->BoundingRadius, glm::length(scene->AABBMax));
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

		mesh->Vb = shared_ptr<GfxVertexBuffer>(AbstractGfxLayer::CreateVertexBuffer(sizeof(Vertex) * mesh->NumVertices, sizeof(Vertex), vertices.data()));

		mesh->VertexStride = sizeof(Vertex);
		mesh->IndexFormat = FORMAT_R16_UINT;

		mesh->Ib = shared_ptr<GfxIndexBuffer>(AbstractGfxLayer::CreateIndexBuffer(mesh->IndexFormat, sizeof(UINT16)*3*numTriangles, indices.data()));


		GfxMesh::DrawCall dc;
		dc.IndexCount = numTriangles * 3;
		dc.IndexStart = 0;
		dc.VertexBase = 0;
		dc.VertexCount = vertices.size();
		dc.mat = scene->Materials[asMesh->mMaterialIndex];
		if (dc.mat->bHasAlpha) mesh->bTransparent = true;
		
		mesh->Draws.push_back(dc);

		scene->meshes.push_back(shared_ptr<GfxMesh>(mesh));
	}

	shared_ptr<Scene> scenePtr = shared_ptr<Scene>(scene);

	return scenePtr;
}

void Corona::InitSpatialDenoisingPass()
{
	SHADER_CREATE_DESC csDesc =
	{
		GetAssetFullPath(L"Shaders\\SpatialDenoising.hlsl"), L"SpatialFilter", L"cs_6_0", nullopt
	};

	COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};

	computePsoDesc.csDesc = &csDesc;

	GfxPipelineStateObject* TEMP_SpatialDenoisingFilterPSO = AbstractGfxLayer::CreatePSO();

	AbstractGfxLayer::BindSRV(TEMP_SpatialDenoisingFilterPSO, "DepthTex", 0, 1);
	AbstractGfxLayer::BindSRV(TEMP_SpatialDenoisingFilterPSO, "GeoNormalTex", 1, 1);
	AbstractGfxLayer::BindSRV(TEMP_SpatialDenoisingFilterPSO, "InGIResultSHTex", 2, 1);
	AbstractGfxLayer::BindSRV(TEMP_SpatialDenoisingFilterPSO, "InGIResultColorTex", 3, 1);
	AbstractGfxLayer::BindUAV(TEMP_SpatialDenoisingFilterPSO, "OutGIResultSH", 0);
	AbstractGfxLayer::BindUAV(TEMP_SpatialDenoisingFilterPSO, "OutGIResultColor", 1);
	AbstractGfxLayer::BindCBV(TEMP_SpatialDenoisingFilterPSO, "SpatialFilterConstant", 0, sizeof(SpatialFilterConstant));

	bool bSucess = AbstractGfxLayer::InitPSO(TEMP_SpatialDenoisingFilterPSO, &computePsoDesc);
	if (bSucess)
		SpatialDenoisingFilterPSO = shared_ptr<GfxPipelineStateObject>(TEMP_SpatialDenoisingFilterPSO);

	UINT WidthGI = RenderWidth / GIBufferScale;
	UINT HeightGI = RenderHeight / GIBufferScale;

	DiffuseGISHSpatial[0] = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R16G16B16A16_FLOAT,
		RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, WidthGI, HeightGI, 1));

	NAME_TEXTURE(DiffuseGISHSpatial[0]);

	DiffuseGISHSpatial[1] = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R16G16B16A16_FLOAT,
		RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, WidthGI, HeightGI, 1));

	NAME_TEXTURE(DiffuseGISHSpatial[1]);

	DiffuseGICoCgSpatial[0] = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R16G16B16A16_FLOAT,
		RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, WidthGI, HeightGI, 1));

	NAME_TEXTURE(DiffuseGICoCgSpatial[0]);

	DiffuseGICoCgSpatial[1] = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R16G16B16A16_FLOAT,
		RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, WidthGI, HeightGI, 1));

	NAME_TEXTURE(DiffuseGICoCgSpatial[1]);
}

void Corona::InitTemporalDenoisingPass()
{
	SHADER_CREATE_DESC csDesc =
	{
		GetAssetFullPath(L"Shaders\\TemporalDenoising.hlsl"), L"TemporalFilter", L"cs_6_0", nullopt
	};

	COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};

	computePsoDesc.csDesc = &csDesc;

	GfxPipelineStateObject* TEMP_TemporalDenoisingFilterPSO = AbstractGfxLayer::CreatePSO();
	AbstractGfxLayer::BindSRV(TEMP_TemporalDenoisingFilterPSO, "DepthTex", 0, 1);
	AbstractGfxLayer::BindSRV(TEMP_TemporalDenoisingFilterPSO, "NormalTex", 1, 1);
	AbstractGfxLayer::BindSRV(TEMP_TemporalDenoisingFilterPSO, "InGIResultSHTex", 2, 1);
	AbstractGfxLayer::BindSRV(TEMP_TemporalDenoisingFilterPSO, "InGIResultColorTex", 3, 1);
	AbstractGfxLayer::BindSRV(TEMP_TemporalDenoisingFilterPSO, "InGIResultSHTexPrev", 4, 1);
	AbstractGfxLayer::BindSRV(TEMP_TemporalDenoisingFilterPSO, "InGIResultColorTexPrev", 5, 1);
	AbstractGfxLayer::BindSRV(TEMP_TemporalDenoisingFilterPSO, "VelocityTex", 6, 1);
	AbstractGfxLayer::BindSRV(TEMP_TemporalDenoisingFilterPSO, "InSpecularGITex", 7, 1);
	AbstractGfxLayer::BindSRV(TEMP_TemporalDenoisingFilterPSO, "InSpecularGITexPrev", 8, 1);
	AbstractGfxLayer::BindSRV(TEMP_TemporalDenoisingFilterPSO, "RougnessMetalicTex", 9, 1);
	AbstractGfxLayer::BindSRV(TEMP_TemporalDenoisingFilterPSO, "PrevDepthTex", 10, 1);
	AbstractGfxLayer::BindSRV(TEMP_TemporalDenoisingFilterPSO, "PrevNormalTex", 11, 1);
	AbstractGfxLayer::BindSRV(TEMP_TemporalDenoisingFilterPSO, "PrevMomentsTex", 12, 1);

	AbstractGfxLayer::BindUAV(TEMP_TemporalDenoisingFilterPSO, "OutGIResultSH", 0);
	AbstractGfxLayer::BindUAV(TEMP_TemporalDenoisingFilterPSO, "OutGIResultColor", 1);
	AbstractGfxLayer::BindUAV(TEMP_TemporalDenoisingFilterPSO, "OutGIResultSHDS", 2);
	AbstractGfxLayer::BindUAV(TEMP_TemporalDenoisingFilterPSO, "OutGIResultColorDS", 3);
	AbstractGfxLayer::BindUAV(TEMP_TemporalDenoisingFilterPSO, "OutSpecularGI", 4);
	AbstractGfxLayer::BindUAV(TEMP_TemporalDenoisingFilterPSO, "OutMoments", 5);

	AbstractGfxLayer::BindSampler(TEMP_TemporalDenoisingFilterPSO, "BilinearClamp", 0);

	AbstractGfxLayer::BindCBV(TEMP_TemporalDenoisingFilterPSO, "TemporalFilterConstant", 0, sizeof(TemporalFilterConstant));
		

	bool bSucess = AbstractGfxLayer::InitPSO(TEMP_TemporalDenoisingFilterPSO, &computePsoDesc);
	if (bSucess)
		TemporalDenoisingFilterPSO = shared_ptr<GfxPipelineStateObject>(TEMP_TemporalDenoisingFilterPSO);
}

void Corona::InitBloomPass()
{
	{
		SHADER_CREATE_DESC csDesc =
		{
			GetAssetFullPath(L"Shaders\\BloomBlur.hlsl"), L"BloomExtract", L"cs_6_0", nullopt
		};

		COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};

		computePsoDesc.csDesc = &csDesc;

		GfxPipelineStateObject* TEMP_BloomExtractPSO = AbstractGfxLayer::CreatePSO();

		AbstractGfxLayer::BindSRV(TEMP_BloomExtractPSO, "SrcTex", 0, 1);
		AbstractGfxLayer::BindSRV(TEMP_BloomExtractPSO, "Exposure", 1, 1);
		AbstractGfxLayer::BindUAV(TEMP_BloomExtractPSO, "DstTex", 0);
		AbstractGfxLayer::BindUAV(TEMP_BloomExtractPSO, "LumaResult", 1);
		AbstractGfxLayer::BindSampler(TEMP_BloomExtractPSO, "samplerWrap", 0);
		AbstractGfxLayer::BindCBV(TEMP_BloomExtractPSO, "BloomCB", 0, sizeof(BloomCB));

		bool bSucess = AbstractGfxLayer::InitPSO(TEMP_BloomExtractPSO, &computePsoDesc);
		if (bSucess)
			BloomExtractPSO = shared_ptr<GfxPipelineStateObject>(TEMP_BloomExtractPSO);
	}
	
	{
		SHADER_CREATE_DESC csDesc =
		{
			GetAssetFullPath(L"Shaders\\BloomBlur.hlsl"), L"BloomBlur", L"cs_6_0", nullopt
		};

		COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};

		computePsoDesc.csDesc = &csDesc;

		GfxPipelineStateObject* TEMP_BloomBlurPSO = AbstractGfxLayer::CreatePSO();

		AbstractGfxLayer::BindSRV(TEMP_BloomBlurPSO, "SrcTex", 0, 1);
		AbstractGfxLayer::BindUAV(TEMP_BloomBlurPSO, "DstTex", 0);
		AbstractGfxLayer::BindSampler(TEMP_BloomBlurPSO, "samplerWrap", 0);
		AbstractGfxLayer::BindCBV(TEMP_BloomBlurPSO, "BloomCB", 0, sizeof(BloomCB));

		bool bSucess = AbstractGfxLayer::InitPSO(TEMP_BloomBlurPSO, &computePsoDesc);
		if (bSucess)
			BloomBlurPSO = shared_ptr<GfxPipelineStateObject>(TEMP_BloomBlurPSO);
	}

	{
		SHADER_CREATE_DESC csDesc =
		{
			GetAssetFullPath(L"Shaders\\Histogram.hlsl"), L"GenerateHistogram", L"cs_6_0", nullopt
		};

		COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};

		computePsoDesc.csDesc = &csDesc;

		GfxPipelineStateObject* TEMP_HistogramPSO = AbstractGfxLayer::CreatePSO();

		AbstractGfxLayer::BindSRV(TEMP_HistogramPSO, "LumaTex", 0, 1);
		AbstractGfxLayer::BindUAV(TEMP_HistogramPSO, "Histogram", 0);

		bool bSucess = AbstractGfxLayer::InitPSO(TEMP_HistogramPSO, &computePsoDesc);
		if (bSucess)
			HistogramPSO = shared_ptr<GfxPipelineStateObject>(TEMP_HistogramPSO);
	}


	{
		SHADER_CREATE_DESC csDesc =
		{
			GetAssetFullPath(L"Shaders\\DrawHistogram.hlsl"), L"DrawHistogram", L"cs_6_0", nullopt
		};

		COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};

		computePsoDesc.csDesc = &csDesc;

		GfxPipelineStateObject* TEMP_DrawHistogramPSO = AbstractGfxLayer::CreatePSO();

		AbstractGfxLayer::BindSRV(TEMP_DrawHistogramPSO, "Histogram", 0, 1);
		AbstractGfxLayer::BindSRV(TEMP_DrawHistogramPSO, "Exposure", 1, 1);
		AbstractGfxLayer::BindUAV(TEMP_DrawHistogramPSO, "ColorBuffer", 0);


		bool bSucess = AbstractGfxLayer::InitPSO(TEMP_DrawHistogramPSO, &computePsoDesc);
		if (bSucess)
			DrawHistogramPSO = shared_ptr<GfxPipelineStateObject>(TEMP_DrawHistogramPSO);
	}

	{
		SHADER_CREATE_DESC csDesc =
		{
			GetAssetFullPath(L"Shaders\\Histogram.hlsl"), L"ClearHistogram", L"cs_6_0", nullopt
		};

		COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};

		computePsoDesc.csDesc = &csDesc;

		GfxPipelineStateObject* TEMP_ClearHistogramPSO = AbstractGfxLayer::CreatePSO();

		AbstractGfxLayer::BindUAV(TEMP_ClearHistogramPSO, "Histogram", 0);

		bool bSucess = AbstractGfxLayer::InitPSO(TEMP_ClearHistogramPSO, &computePsoDesc);
		if (bSucess)
			ClearHistogramPSO = shared_ptr<GfxPipelineStateObject>(TEMP_ClearHistogramPSO);
	}

	{
		SHADER_CREATE_DESC csDesc =
		{
			GetAssetFullPath(L"Shaders\\AdaptExposureCS.hlsl"), L"AdaptExposure", L"cs_6_0", nullopt
		};

		COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};

		computePsoDesc.csDesc = &csDesc;

		GfxPipelineStateObject* TEMP_AdapteExposurePSO = AbstractGfxLayer::CreatePSO();

		AbstractGfxLayer::BindSRV(TEMP_AdapteExposurePSO, "Histogram", 0, 1);
		AbstractGfxLayer::BindUAV(TEMP_AdapteExposurePSO, "Exposure", 0);
		AbstractGfxLayer::BindUAV(TEMP_AdapteExposurePSO, "Exposure", 0);
		AbstractGfxLayer::BindCBV(TEMP_AdapteExposurePSO, "AdaptExposureCB", 0, sizeof(AdaptExposureCB));
		bool bSucess = AbstractGfxLayer::InitPSO(TEMP_AdapteExposurePSO, &computePsoDesc);
		if (bSucess)
			AdapteExposurePSO = shared_ptr<GfxPipelineStateObject>(TEMP_AdapteExposurePSO);
	}

	{
		INPUT_ELEMENT_DESC StandardVertexDescription[] =
		{
			{ "POSITION", 0, FORMAT_R32G32B32A32_FLOAT, 0, 0,  INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, FORMAT_R32G32_FLOAT,    0, 16, INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		};

		UINT StandardVertexDescriptionNumElements = _countof(StandardVertexDescription);

		RENDER_TARGET_BLEND_DESC defaultRenderTargetBlendDesc =
		{
			/*BlendEnable=*/FALSE,
			/*LogicOpEnable=*/FALSE,
			/*SrcBlend=*/ BLEND_ONE,
			/*DestBlend=*/BLEND_ZERO,
			/*BlendOp=*/BLEND_OP_ADD,
			/*SrcBlendAlpha=*/BLEND_ONE,
			/*DestBlendAlpha=*/BLEND_ZERO,
			/*BlendOpAlpha=*/BLEND_OP_ADD,
			/*LogicOp=*/LOGIC_OP_NOOP,
			/*RenderTargetWriteMask=*/COLOR_WRITE_ENABLE_ALL,
		};

		BLEND_DESC blendState = {
			/*AlphaToCoverageEnable=*/FALSE,
			/*IndependentBlendEnable*/FALSE,
			/*RenderTarget[0]=*/defaultRenderTargetBlendDesc,
			/*RenderTarget[1]=*/defaultRenderTargetBlendDesc,
			/*RenderTarget[2]=*/defaultRenderTargetBlendDesc,
			/*RenderTarget[3]=*/defaultRenderTargetBlendDesc,
			/*RenderTarget[4]=*/defaultRenderTargetBlendDesc,
			/*RenderTarget[5]=*/defaultRenderTargetBlendDesc,
			/*RenderTarget[6]=*/defaultRenderTargetBlendDesc,
			/*RenderTarget[7]=*/defaultRenderTargetBlendDesc
		};


		const DEPTH_STENCILOP_DESC defaultStencilOp =
		{
			/*StencilFailOp =*/ STENCIL_OP_KEEP,
			/*StencilDepthFailOp =*/ STENCIL_OP_KEEP,
			/*StencilPassOp = */STENCIL_OP_KEEP,
			/*StencilFunc = */COMPARISON_FUNC_ALWAYS
		};

		/*psoDesc.DepthStencilState.DepthEnable = FALSE;
		psoDesc.DepthStencilState.StencilEnable = FALSE;*/
		DEPTH_STENCIL_DESC depthStencilState = {
			/*DepthEnable = */FALSE,
			/*DepthWriteMask = */DEPTH_WRITE_MASK_ALL,
			/*DepthFunc = */COMPARISON_FUNC_LESS,
			/*StencilEnable = */FALSE,
			/*StencilReadMask = */DEFAULT_STENCIL_READ_MASK,
			/*StencilWriteMask = */DEFAULT_STENCIL_WRITE_MASK,
			/*FrontFace = */defaultStencilOp,
			/*BackFace = */defaultStencilOp

		};

		GRAPHICS_PIPELINE_STATE_DESC psoDescMesh = {};
		psoDescMesh.InputLayout = { StandardVertexDescription, StandardVertexDescriptionNumElements };
		//psoDescMesh.RasterizerState = rasterizerStateDesc;
		psoDescMesh.CullMode = CULL_MODE_NONE; // rasterizer state is too big. and currently I use only CullMode.
		psoDescMesh.BlendState = blendState;

		psoDescMesh.DepthStencilState = depthStencilState;
		psoDescMesh.SampleMask = UINT_MAX;
		psoDescMesh.PrimitiveTopologyType = PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		psoDescMesh.NumRenderTargets = 1;
		psoDescMesh.RTVFormats[0] = FORMAT_R16G16B16A16_FLOAT;
		psoDescMesh.MultiSampleCount = 1;


		SHADER_CREATE_DESC vsDesc =
		{
			GetAssetFullPath(L"Shaders\\AddBloomPS.hlsl"), L"VSMain", L"vs_6_0", nullopt
		};

		SHADER_CREATE_DESC psDesc =
		{
			GetAssetFullPath(L"Shaders\\AddBloomPS.hlsl"), L"PSMain", L"ps_6_0", nullopt
		};
		psoDescMesh.vsDesc = &vsDesc;
		psoDescMesh.psDesc = &psDesc;



		GfxPipelineStateObject* TEMP_AddBloomPSO = AbstractGfxLayer::CreatePSO();

		AbstractGfxLayer::BindSRV(TEMP_AddBloomPSO, "SrcTex", 0, 1);
		AbstractGfxLayer::BindSRV(TEMP_AddBloomPSO, "BloomTex", 1, 1);

		AbstractGfxLayer::BindSampler(TEMP_AddBloomPSO, "samplerWrap", 0);
		AbstractGfxLayer::BindCBV(TEMP_AddBloomPSO, "AddBloomCB", 0, sizeof(AddBloomCB));
		bool bSuccess = AbstractGfxLayer::InitPSO(TEMP_AddBloomPSO, &psoDescMesh);

		if (bSuccess)
			AddBloomPSO = shared_ptr<GfxPipelineStateObject>(TEMP_AddBloomPSO);
	}

	BloomBlurPingPong[0] = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R16G16B16A16_FLOAT,
		RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, BloomBufferWidth, BloomBufferHeight, 1));

	NAME_TEXTURE(BloomBlurPingPong[0]);

	BloomBlurPingPong[1] = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R16G16B16A16_FLOAT,
		RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, BloomBufferWidth, BloomBufferHeight, 1));

	NAME_TEXTURE(BloomBlurPingPong[1]);


	LumaBuffer = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R8_UINT,
		RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, BloomBufferWidth, BloomBufferHeight, 1));

	NAME_TEXTURE(LumaBuffer);

	Histogram = shared_ptr<GfxBuffer>(AbstractGfxLayer::CreateByteAddressBuffer(256, sizeof(UINT32), HEAP_TYPE_DEFAULT, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
	NAME_BUFFER(Histogram);

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

	ExposureData = shared_ptr<GfxBuffer>(AbstractGfxLayer::CreateByteAddressBuffer(8, sizeof(float), HEAP_TYPE_DEFAULT, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, initExposure));
	NAME_BUFFER(ExposureData);

}

void Corona::InitResolvePixelVelocityPass()
{
	INPUT_ELEMENT_DESC StandardVertexDescription[] =
	{
		{ "POSITION", 0, FORMAT_R32G32B32A32_FLOAT, 0, 0,  INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, FORMAT_R32G32_FLOAT,    0, 16, INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};

	UINT StandardVertexDescriptionNumElements = _countof(StandardVertexDescription);

	RENDER_TARGET_BLEND_DESC defaultRenderTargetBlendDesc =
	{
		/*BlendEnable=*/FALSE,
		/*LogicOpEnable=*/FALSE,
		/*SrcBlend=*/ BLEND_ONE,
		/*DestBlend=*/BLEND_ZERO,
		/*BlendOp=*/BLEND_OP_ADD,
		/*SrcBlendAlpha=*/BLEND_ONE,
		/*DestBlendAlpha=*/BLEND_ZERO,
		/*BlendOpAlpha=*/BLEND_OP_ADD,
		/*LogicOp=*/LOGIC_OP_NOOP,
		/*RenderTargetWriteMask=*/COLOR_WRITE_ENABLE_ALL,
	};

	BLEND_DESC blendState = {
		/*AlphaToCoverageEnable=*/FALSE,
		/*IndependentBlendEnable*/FALSE,
		/*RenderTarget[0]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[1]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[2]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[3]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[4]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[5]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[6]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[7]=*/defaultRenderTargetBlendDesc
	};


	const DEPTH_STENCILOP_DESC defaultStencilOp =
	{
		/*StencilFailOp =*/ STENCIL_OP_KEEP,
		/*StencilDepthFailOp =*/ STENCIL_OP_KEEP,
		/*StencilPassOp = */STENCIL_OP_KEEP,
		/*StencilFunc = */COMPARISON_FUNC_ALWAYS
	};

	/*psoDesc.DepthStencilState.DepthEnable = FALSE;
	psoDesc.DepthStencilState.StencilEnable = FALSE;*/
	DEPTH_STENCIL_DESC depthStencilState = {
		/*DepthEnable = */FALSE,
		/*DepthWriteMask = */DEPTH_WRITE_MASK_ALL,
		/*DepthFunc = */COMPARISON_FUNC_LESS,
		/*StencilEnable = */FALSE,
		/*StencilReadMask = */DEFAULT_STENCIL_READ_MASK,
		/*StencilWriteMask = */DEFAULT_STENCIL_WRITE_MASK,
		/*FrontFace = */defaultStencilOp,
		/*BackFace = */defaultStencilOp

	};

	GRAPHICS_PIPELINE_STATE_DESC psoDescMesh = {};
	psoDescMesh.InputLayout = { StandardVertexDescription, StandardVertexDescriptionNumElements };
	//psoDescMesh.RasterizerState = rasterizerStateDesc;
	psoDescMesh.CullMode = CULL_MODE_NONE; // rasterizer state is too big. and currently I use only CullMode.
	psoDescMesh.BlendState = blendState;

	psoDescMesh.DepthStencilState = depthStencilState;
	psoDescMesh.SampleMask = UINT_MAX;
	psoDescMesh.PrimitiveTopologyType = PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDescMesh.NumRenderTargets = 1;
	psoDescMesh.RTVFormats[0] = FORMAT_R16G16_FLOAT;
	psoDescMesh.MultiSampleCount = 1;


	SHADER_CREATE_DESC vsDesc =
	{
		GetAssetFullPath(L"Shaders\\ResolveVelocityPS.hlsl"), L"VSMain", L"vs_6_0", nullopt
	};

	SHADER_CREATE_DESC psDesc =
	{
		GetAssetFullPath(L"Shaders\\ResolveVelocityPS.hlsl"), L"PSMain", L"ps_6_0", nullopt
	};
	psoDescMesh.vsDesc = &vsDesc;
	psoDescMesh.psDesc = &psDesc;



	GfxPipelineStateObject* TEMP_ResolvePixelVelocityPSO = AbstractGfxLayer::CreatePSO();


	AbstractGfxLayer::BindSRV(TEMP_ResolvePixelVelocityPSO, "SrcTex", 0, 1);

	AbstractGfxLayer::BindSampler(TEMP_ResolvePixelVelocityPSO, "samplerWrap", 0);
	AbstractGfxLayer::BindCBV(TEMP_ResolvePixelVelocityPSO, "ResolveVelocityCB", 0, sizeof(AddBloomCB));

	bool bSuccess = AbstractGfxLayer::InitPSO(TEMP_ResolvePixelVelocityPSO, &psoDescMesh);

	if (bSuccess)
		ResolvePixelVelocityPSO = shared_ptr<GfxPipelineStateObject>(TEMP_ResolvePixelVelocityPSO);
}

void Corona::InitGBufferPass()
{
	INPUT_ELEMENT_DESC StandardVertexDescription[] =
	{
		{ "POSITION", 0, FORMAT_R32G32B32_FLOAT, 0, 0,  INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL",   0, FORMAT_R32G32B32_FLOAT, 0, 12, INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, FORMAT_R32G32_FLOAT,    0, 24, INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT",  0, FORMAT_R32G32B32_FLOAT, 0, 32, INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};

	UINT StandardVertexDescriptionNumElements = _countof(StandardVertexDescription);

	RENDER_TARGET_BLEND_DESC defaultRenderTargetBlendDesc =
	{
		/*BlendEnable=*/FALSE,
		/*LogicOpEnable=*/FALSE,
		/*SrcBlend=*/ BLEND_ONE,
		/*DestBlend=*/BLEND_ZERO,
		/*BlendOp=*/BLEND_OP_ADD,
		/*SrcBlendAlpha=*/BLEND_ONE,
		/*DestBlendAlpha=*/BLEND_ZERO,
		/*BlendOpAlpha=*/BLEND_OP_ADD,
		/*LogicOp=*/LOGIC_OP_NOOP,
		/*RenderTargetWriteMask=*/COLOR_WRITE_ENABLE_ALL,
	};

	BLEND_DESC blendState = {
		/*AlphaToCoverageEnable=*/FALSE,
		/*IndependentBlendEnable*/FALSE,
		/*RenderTarget[0]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[1]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[2]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[3]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[4]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[5]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[6]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[7]=*/defaultRenderTargetBlendDesc
	};


	const DEPTH_STENCILOP_DESC defaultStencilOp =
	{ 
		/*StencilFailOp =*/ STENCIL_OP_KEEP,
		/*StencilDepthFailOp =*/ STENCIL_OP_KEEP,
		/*StencilPassOp = */STENCIL_OP_KEEP,
		/*StencilFunc = */COMPARISON_FUNC_ALWAYS
	};

	DEPTH_STENCIL_DESC depthStencilState = {
	/*DepthEnable = */TRUE,
	/*DepthWriteMask = */DEPTH_WRITE_MASK_ALL,
	/*DepthFunc = */COMPARISON_FUNC_LESS,
	/*StencilEnable = */FALSE,
	/*StencilReadMask = */DEFAULT_STENCIL_READ_MASK,
	/*StencilWriteMask = */DEFAULT_STENCIL_WRITE_MASK,
	/*FrontFace = */defaultStencilOp,
	/*BackFace = */defaultStencilOp

	};

	GRAPHICS_PIPELINE_STATE_DESC psoDescMesh = {};
	psoDescMesh.InputLayout = { StandardVertexDescription, StandardVertexDescriptionNumElements };
	//psoDescMesh.RasterizerState = rasterizerStateDesc;
	psoDescMesh.CullMode = CULL_MODE_NONE; // rasterizer state is too big. and currently I use only CullMode.
	psoDescMesh.BlendState = blendState;
	psoDescMesh.DepthStencilState = depthStencilState;
	psoDescMesh.SampleMask = UINT_MAX;
	psoDescMesh.PrimitiveTopologyType = PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDescMesh.NumRenderTargets = 6;
	psoDescMesh.RTVFormats[0] = FORMAT_R8G8B8A8_UNORM;
	psoDescMesh.RTVFormats[1] = FORMAT_R16G16B16A16_FLOAT;
	psoDescMesh.RTVFormats[2] = FORMAT_R16G16B16A16_FLOAT;
	psoDescMesh.RTVFormats[3] = FORMAT_R16G16B16A16_FLOAT;
	psoDescMesh.RTVFormats[4] = FORMAT_R8G8B8A8_UNORM;
	psoDescMesh.RTVFormats[5] = FORMAT_R32_FLOAT;
		
	psoDescMesh.DSVFormat = FORMAT_D32_FLOAT;// DXGI_FORMAT_D24_UNORM_S8_UINT;
	psoDescMesh.MultiSampleCount = 1;



	SHADER_CREATE_DESC vsDesc =
	{
		GetAssetFullPath(L"Shaders\\GBuffer.hlsl"), L"VSMain", L"vs_6_0", nullopt
	};

	SHADER_CREATE_DESC psDesc =
	{
		GetAssetFullPath(L"Shaders\\GBuffer.hlsl"), L"PSMain", L"ps_6_0", nullopt
	};
	psoDescMesh.vsDesc = &vsDesc;
	psoDescMesh.psDesc = &psDesc;


		
	GfxPipelineStateObject* TEMP_GBufferPassPSO = AbstractGfxLayer::CreatePSO();
		
	AbstractGfxLayer::BindSRV(TEMP_GBufferPassPSO, "AlbedoTex", 0, 1);
	AbstractGfxLayer::BindSRV(TEMP_GBufferPassPSO, "NormalTex", 1, 1);
	AbstractGfxLayer::BindSRV(TEMP_GBufferPassPSO, "RoughnessTex", 2, 1);
	AbstractGfxLayer::BindSRV(TEMP_GBufferPassPSO, "MetallicTex", 3, 1);
	AbstractGfxLayer::BindSampler(TEMP_GBufferPassPSO, "samplerWrap", 0);
	AbstractGfxLayer::BindCBV(TEMP_GBufferPassPSO, "GBufferConstantBuffer", 0, sizeof(GBufferConstantBuffer));

	bool bSuccess = AbstractGfxLayer::InitPSO(TEMP_GBufferPassPSO, &psoDescMesh);

	if (bSuccess)
		GBufferPassPSO = shared_ptr<GfxPipelineStateObject>(TEMP_GBufferPassPSO);
}

#if USE_IMGUI
void Corona::InitImgui()
{
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO(); (void)io;

	io.DisplaySize.x = DisplayWidth;
	io.DisplaySize.y = DisplayHeight;

	dx12_rhi->TextureDHRing->AllocDescriptor(CpuHandleImguiFontTex, GpuHandleImguiFontTex);

	ImGui_ImplWin32_Init(Win32Application::GetHwnd());
	ImGui_ImplDX12_Init(dx12_rhi->Device.Get(), dx12_rhi->NumFrame,
		DXGI_FORMAT_R8G8B8A8_UNORM, dx12_rhi->SRVCBVDescriptorHeapShaderVisible->DH.Get(),
		CpuHandleImguiFontTex,
		GpuHandleImguiFontTex);
}
#endif

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

		BlueNoiseTex = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture3D(FORMAT_R32G32B32A32_FLOAT, RESOURCE_FLAG_NONE,
			RESOURCE_STATE_COPY_DEST, Shape[0], Shape[1], Shape[2], 1));

		SUBRESOURCE_DATA data = {
			NoiseDataFloat, // pData
			Shape[0] * nChannel * sizeof(UINT32), // RowPitch
			data.RowPitch * Shape[1] // SlicePitch
		};
		
		AbstractGfxLayer::UploadSRCData3D(BlueNoiseTex.get(), &data);

		delete[] NoiseDataRaw;
		delete[] NoiseDataFloat;
	}
	
	file.close();
}

void Corona::InitToneMapPass()
{
	INPUT_ELEMENT_DESC StandardVertexDescription[] =
	{
		{ "POSITION", 0, FORMAT_R32G32B32A32_FLOAT, 0, 0,  INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, FORMAT_R32G32_FLOAT,    0, 16, INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};

	UINT StandardVertexDescriptionNumElements = _countof(StandardVertexDescription);

	RENDER_TARGET_BLEND_DESC defaultRenderTargetBlendDesc =
	{
		/*BlendEnable=*/FALSE,
		/*LogicOpEnable=*/FALSE,
		/*SrcBlend=*/ BLEND_ONE,
		/*DestBlend=*/BLEND_ZERO,
		/*BlendOp=*/BLEND_OP_ADD,
		/*SrcBlendAlpha=*/BLEND_ONE,
		/*DestBlendAlpha=*/BLEND_ZERO,
		/*BlendOpAlpha=*/BLEND_OP_ADD,
		/*LogicOp=*/LOGIC_OP_NOOP,
		/*RenderTargetWriteMask=*/COLOR_WRITE_ENABLE_ALL,
	};

	BLEND_DESC blendState = {
		/*AlphaToCoverageEnable=*/FALSE,
		/*IndependentBlendEnable*/FALSE,
		/*RenderTarget[0]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[1]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[2]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[3]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[4]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[5]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[6]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[7]=*/defaultRenderTargetBlendDesc
	};


	const DEPTH_STENCILOP_DESC defaultStencilOp =
	{
		/*StencilFailOp =*/ STENCIL_OP_KEEP,
		/*StencilDepthFailOp =*/ STENCIL_OP_KEEP,
		/*StencilPassOp = */STENCIL_OP_KEEP,
		/*StencilFunc = */COMPARISON_FUNC_ALWAYS
	};

	/*psoDesc.DepthStencilState.DepthEnable = FALSE;
	psoDesc.DepthStencilState.StencilEnable = FALSE;*/
	DEPTH_STENCIL_DESC depthStencilState = {
		/*DepthEnable = */FALSE,
		/*DepthWriteMask = */DEPTH_WRITE_MASK_ALL,
		/*DepthFunc = */COMPARISON_FUNC_LESS,
		/*StencilEnable = */FALSE,
		/*StencilReadMask = */DEFAULT_STENCIL_READ_MASK,
		/*StencilWriteMask = */DEFAULT_STENCIL_WRITE_MASK,
		/*FrontFace = */defaultStencilOp,
		/*BackFace = */defaultStencilOp

	};

	GRAPHICS_PIPELINE_STATE_DESC psoDescMesh = {};
	psoDescMesh.InputLayout = { StandardVertexDescription, StandardVertexDescriptionNumElements };
	//psoDescMesh.RasterizerState = rasterizerStateDesc;
	psoDescMesh.CullMode = CULL_MODE_NONE; // rasterizer state is too big. and currently I use only CullMode.
	psoDescMesh.BlendState = blendState;

	psoDescMesh.DepthStencilState = depthStencilState;
	psoDescMesh.SampleMask = UINT_MAX;
	psoDescMesh.PrimitiveTopologyType = PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDescMesh.NumRenderTargets = 1;
	psoDescMesh.RTVFormats[0] = FORMAT_R8G8B8A8_UNORM;
	psoDescMesh.MultiSampleCount = 1;


	SHADER_CREATE_DESC vsDesc =
	{
		GetAssetFullPath(L"Shaders\\ToneMapPS.hlsl"), L"VSMain", L"vs_6_0", nullopt
	};

	SHADER_CREATE_DESC psDesc =
	{
		GetAssetFullPath(L"Shaders\\ToneMapPS.hlsl"), L"PSMain", L"ps_6_0", nullopt
	};
	psoDescMesh.vsDesc = &vsDesc;
	psoDescMesh.psDesc = &psDesc;

	GfxPipelineStateObject* TEMP_ToneMapPSO = AbstractGfxLayer::CreatePSO();

	AbstractGfxLayer::BindSRV(TEMP_ToneMapPSO, "SrcTex", 0, 1);
	AbstractGfxLayer::BindSRV(TEMP_ToneMapPSO, "Exposure", 1, 1);

	AbstractGfxLayer::BindSampler(TEMP_ToneMapPSO, "samplerWrap", 0);
	AbstractGfxLayer::BindCBV(TEMP_ToneMapPSO, "ScaleOffsetParams", 0, sizeof(ToneMapCB));

	bool bSuccess = AbstractGfxLayer::InitPSO(TEMP_ToneMapPSO, &psoDescMesh);

	if (bSuccess)
		ToneMapPSO = shared_ptr<GfxPipelineStateObject>(TEMP_ToneMapPSO);
}

void Corona::InitDebugPass()
{
	INPUT_ELEMENT_DESC StandardVertexDescription[] =
	{
		{ "POSITION", 0, FORMAT_R32G32B32A32_FLOAT, 0, 0,  INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, FORMAT_R32G32_FLOAT,    0, 16, INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};

	UINT StandardVertexDescriptionNumElements = _countof(StandardVertexDescription);

	RENDER_TARGET_BLEND_DESC defaultRenderTargetBlendDesc =
	{
		/*BlendEnable=*/FALSE,
		/*LogicOpEnable=*/FALSE,
		/*SrcBlend=*/ BLEND_ONE,
		/*DestBlend=*/BLEND_ZERO,
		/*BlendOp=*/BLEND_OP_ADD,
		/*SrcBlendAlpha=*/BLEND_ONE,
		/*DestBlendAlpha=*/BLEND_ZERO,
		/*BlendOpAlpha=*/BLEND_OP_ADD,
		/*LogicOp=*/LOGIC_OP_NOOP,
		/*RenderTargetWriteMask=*/COLOR_WRITE_ENABLE_ALL,
	};

	BLEND_DESC blendState = {
		/*AlphaToCoverageEnable=*/FALSE,
		/*IndependentBlendEnable*/FALSE,
		/*RenderTarget[0]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[1]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[2]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[3]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[4]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[5]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[6]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[7]=*/defaultRenderTargetBlendDesc
	};


	const DEPTH_STENCILOP_DESC defaultStencilOp =
	{
		/*StencilFailOp =*/ STENCIL_OP_KEEP,
		/*StencilDepthFailOp =*/ STENCIL_OP_KEEP,
		/*StencilPassOp = */STENCIL_OP_KEEP,
		/*StencilFunc = */COMPARISON_FUNC_ALWAYS
	};

	/*psoDesc.DepthStencilState.DepthEnable = FALSE;
	psoDesc.DepthStencilState.StencilEnable = FALSE;*/
	DEPTH_STENCIL_DESC depthStencilState = {
		/*DepthEnable = */FALSE,
		/*DepthWriteMask = */DEPTH_WRITE_MASK_ALL,
		/*DepthFunc = */COMPARISON_FUNC_LESS,
		/*StencilEnable = */FALSE,
		/*StencilReadMask = */DEFAULT_STENCIL_READ_MASK,
		/*StencilWriteMask = */DEFAULT_STENCIL_WRITE_MASK,
		/*FrontFace = */defaultStencilOp,
		/*BackFace = */defaultStencilOp

	};

	GRAPHICS_PIPELINE_STATE_DESC psoDescMesh = {};
	psoDescMesh.InputLayout = { StandardVertexDescription, StandardVertexDescriptionNumElements };
	psoDescMesh.CullMode = CULL_MODE_NONE; // rasterizer state is too big. and currently I use only CullMode.
	psoDescMesh.BlendState = blendState;

	psoDescMesh.DepthStencilState = depthStencilState;
	psoDescMesh.SampleMask = UINT_MAX;
	psoDescMesh.PrimitiveTopologyType = PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDescMesh.NumRenderTargets = 1;
	psoDescMesh.RTVFormats[0] = FORMAT_R8G8B8A8_UNORM;
	psoDescMesh.MultiSampleCount = 1;


	SHADER_CREATE_DESC vsDesc =
	{
		GetAssetFullPath(L"Shaders\\DebugPS.hlsl"), L"VSMain", L"vs_6_0", nullopt
	};

	SHADER_CREATE_DESC psDesc =
	{
		GetAssetFullPath(L"Shaders\\DebugPS.hlsl"), L"PSMain", L"ps_6_0", nullopt
	};
	psoDescMesh.vsDesc = &vsDesc;
	psoDescMesh.psDesc = &psDesc;



	GfxPipelineStateObject* TEMP_BufferVisualizePSO = AbstractGfxLayer::CreatePSO();

	AbstractGfxLayer::BindSRV(TEMP_BufferVisualizePSO, "SrcTex", 0, 1);
	AbstractGfxLayer::BindSRV(TEMP_BufferVisualizePSO, "SrcTexSH", 1, 1);
	AbstractGfxLayer::BindSRV(TEMP_BufferVisualizePSO, "SrcTexNormal", 2, 1);
	AbstractGfxLayer::BindSRV(TEMP_BufferVisualizePSO, "DDGIProbeIrradianceSRV", 3, 1);
	AbstractGfxLayer::BindSRV(TEMP_BufferVisualizePSO, "DDGIProbeDistanceSRV", 4, 1);
	AbstractGfxLayer::BindSRV(TEMP_BufferVisualizePSO, "DepthTex", 5, 1);

	AbstractGfxLayer::BindUAV(TEMP_BufferVisualizePSO, "DDGIProbeStates", 0);
	AbstractGfxLayer::BindUAV(TEMP_BufferVisualizePSO, "DDGIProbeOffsets", 1);




	AbstractGfxLayer::BindSampler(TEMP_BufferVisualizePSO, "samplerWrap", 0);
	AbstractGfxLayer::BindSampler(TEMP_BufferVisualizePSO, "TrilinearSampler", 1);

	AbstractGfxLayer::BindCBV(TEMP_BufferVisualizePSO, "DebugPassCB", 0, sizeof(DebugPassCB));
	AbstractGfxLayer::BindCBV(TEMP_BufferVisualizePSO, "DDGIVolume", 1, rtxgi::GetDDGIVolumeConstantBufferSize());

	bool bSuccess = AbstractGfxLayer::InitPSO(TEMP_BufferVisualizePSO, &psoDescMesh);

	if (bSuccess)
		BufferVisualizePSO = shared_ptr<GfxPipelineStateObject>(TEMP_BufferVisualizePSO);
}

void Corona::InitLightingPass()
{
	INPUT_ELEMENT_DESC StandardVertexDescription[] =
	{
		{ "POSITION", 0, FORMAT_R32G32B32A32_FLOAT, 0, 0,  INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, FORMAT_R32G32_FLOAT,    0, 16, INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};

	UINT StandardVertexDescriptionNumElements = _countof(StandardVertexDescription);

	RENDER_TARGET_BLEND_DESC defaultRenderTargetBlendDesc =
	{
		/*BlendEnable=*/FALSE,
		/*LogicOpEnable=*/FALSE,
		/*SrcBlend=*/ BLEND_ONE,
		/*DestBlend=*/BLEND_ZERO,
		/*BlendOp=*/BLEND_OP_ADD,
		/*SrcBlendAlpha=*/BLEND_ONE,
		/*DestBlendAlpha=*/BLEND_ZERO,
		/*BlendOpAlpha=*/BLEND_OP_ADD,
		/*LogicOp=*/LOGIC_OP_NOOP,
		/*RenderTargetWriteMask=*/COLOR_WRITE_ENABLE_ALL,
	};

	BLEND_DESC blendState = {
		/*AlphaToCoverageEnable=*/FALSE,
		/*IndependentBlendEnable*/FALSE,
		/*RenderTarget[0]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[1]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[2]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[3]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[4]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[5]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[6]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[7]=*/defaultRenderTargetBlendDesc
	};


	const DEPTH_STENCILOP_DESC defaultStencilOp =
	{
		/*StencilFailOp =*/ STENCIL_OP_KEEP,
		/*StencilDepthFailOp =*/ STENCIL_OP_KEEP,
		/*StencilPassOp = */STENCIL_OP_KEEP,
		/*StencilFunc = */COMPARISON_FUNC_ALWAYS
	};

	DEPTH_STENCIL_DESC depthStencilState = {
		/*DepthEnable = */FALSE,
		/*DepthWriteMask = */DEPTH_WRITE_MASK_ALL,
		/*DepthFunc = */COMPARISON_FUNC_LESS,
		/*StencilEnable = */FALSE,
		/*StencilReadMask = */DEFAULT_STENCIL_READ_MASK,
		/*StencilWriteMask = */DEFAULT_STENCIL_WRITE_MASK,
		/*FrontFace = */defaultStencilOp,
		/*BackFace = */defaultStencilOp

	};

	GRAPHICS_PIPELINE_STATE_DESC psoDescMesh = {};
	psoDescMesh.InputLayout = { StandardVertexDescription, StandardVertexDescriptionNumElements };
	psoDescMesh.CullMode = CULL_MODE_NONE; // rasterizer state is too big. and currently I use only CullMode.
	psoDescMesh.BlendState = blendState;

	psoDescMesh.DepthStencilState = depthStencilState;
	psoDescMesh.SampleMask = UINT_MAX;
	psoDescMesh.PrimitiveTopologyType = PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDescMesh.NumRenderTargets = 1;
	psoDescMesh.RTVFormats[0] = FORMAT_R16G16B16A16_FLOAT;
	psoDescMesh.MultiSampleCount = 1;


	SHADER_CREATE_DESC vsDesc =
	{
		GetAssetFullPath(L"Shaders\\LightingPS.hlsl"), L"VSMain", L"vs_6_0", nullopt
	};

	SHADER_CREATE_DESC psDesc =
	{
		GetAssetFullPath(L"Shaders\\LightingPS.hlsl"), L"PSMain", L"ps_6_0", nullopt
	};
	psoDescMesh.vsDesc = &vsDesc;
	psoDescMesh.psDesc = &psDesc;

	GfxPipelineStateObject* TEMP_BufferVisualizePSO = AbstractGfxLayer::CreatePSO();

	AbstractGfxLayer::BindSRV(TEMP_BufferVisualizePSO, "AlbedoTex", 0, 1);
	AbstractGfxLayer::BindSRV(TEMP_BufferVisualizePSO, "NormalTex", 1, 1);
	AbstractGfxLayer::BindSRV(TEMP_BufferVisualizePSO, "ShadowTex", 2, 1);
	AbstractGfxLayer::BindSRV(TEMP_BufferVisualizePSO, "VelocityTex", 3, 1);
	AbstractGfxLayer::BindSRV(TEMP_BufferVisualizePSO, "DepthTex", 4, 1);
	AbstractGfxLayer::BindSRV(TEMP_BufferVisualizePSO, "GIResultSHTex", 5, 1);
	AbstractGfxLayer::BindSRV(TEMP_BufferVisualizePSO, "GIResultColorTex", 6, 1);
	AbstractGfxLayer::BindSRV(TEMP_BufferVisualizePSO, "SpecularGITex", 7, 1);
	AbstractGfxLayer::BindSRV(TEMP_BufferVisualizePSO, "RoughnessMetalicTex", 8, 1);
	AbstractGfxLayer::BindSRV(TEMP_BufferVisualizePSO, "DiffuseGITex", 9, 1);

	AbstractGfxLayer::BindSRV(TEMP_BufferVisualizePSO, "DDGIProbeIrradianceSRV", 10, 1);
	AbstractGfxLayer::BindSRV(TEMP_BufferVisualizePSO, "DDGIProbeDistanceSRV", 11, 1);

	AbstractGfxLayer::BindUAV(TEMP_BufferVisualizePSO, "DDGIProbeStates", 0);
	AbstractGfxLayer::BindUAV(TEMP_BufferVisualizePSO, "DDGIProbeOffsets", 1);

	AbstractGfxLayer::BindSampler(TEMP_BufferVisualizePSO, "samplerWrap", 0);
	AbstractGfxLayer::BindSampler(TEMP_BufferVisualizePSO, "TrilinearSampler", 1);

	AbstractGfxLayer::BindCBV(TEMP_BufferVisualizePSO, "LightingParam", 0, sizeof(LightingParam));
	AbstractGfxLayer::BindCBV(TEMP_BufferVisualizePSO, "DDGIVolume", 1, rtxgi::GetDDGIVolumeConstantBufferSize());

	bool bSuccess = AbstractGfxLayer::InitPSO(TEMP_BufferVisualizePSO, &psoDescMesh);

	if (bSuccess)
		LightingPSO = shared_ptr<GfxPipelineStateObject>(TEMP_BufferVisualizePSO);
}

void Corona::InitTemporalAAPass()
{
	INPUT_ELEMENT_DESC StandardVertexDescription[] =
	{
		{ "POSITION", 0, FORMAT_R32G32B32A32_FLOAT, 0, 0,  INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, FORMAT_R32G32_FLOAT,    0, 16, INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};

	UINT StandardVertexDescriptionNumElements = _countof(StandardVertexDescription);

	RENDER_TARGET_BLEND_DESC defaultRenderTargetBlendDesc =
	{
		/*BlendEnable=*/FALSE,
		/*LogicOpEnable=*/FALSE,
		/*SrcBlend=*/ BLEND_ONE,
		/*DestBlend=*/BLEND_ZERO,
		/*BlendOp=*/BLEND_OP_ADD,
		/*SrcBlendAlpha=*/BLEND_ONE,
		/*DestBlendAlpha=*/BLEND_ZERO,
		/*BlendOpAlpha=*/BLEND_OP_ADD,
		/*LogicOp=*/LOGIC_OP_NOOP,
		/*RenderTargetWriteMask=*/COLOR_WRITE_ENABLE_ALL,
	};

	BLEND_DESC blendState = {
		/*AlphaToCoverageEnable=*/FALSE,
		/*IndependentBlendEnable*/FALSE,
		/*RenderTarget[0]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[1]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[2]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[3]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[4]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[5]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[6]=*/defaultRenderTargetBlendDesc,
		/*RenderTarget[7]=*/defaultRenderTargetBlendDesc
	};


	const DEPTH_STENCILOP_DESC defaultStencilOp =
	{
		/*StencilFailOp =*/ STENCIL_OP_KEEP,
		/*StencilDepthFailOp =*/ STENCIL_OP_KEEP,
		/*StencilPassOp = */STENCIL_OP_KEEP,
		/*StencilFunc = */COMPARISON_FUNC_ALWAYS
	};

	DEPTH_STENCIL_DESC depthStencilState = {
		/*DepthEnable = */FALSE,
		/*DepthWriteMask = */DEPTH_WRITE_MASK_ALL,
		/*DepthFunc = */COMPARISON_FUNC_LESS,
		/*StencilEnable = */FALSE,
		/*StencilReadMask = */DEFAULT_STENCIL_READ_MASK,
		/*StencilWriteMask = */DEFAULT_STENCIL_WRITE_MASK,
		/*FrontFace = */defaultStencilOp,
		/*BackFace = */defaultStencilOp

	};

	GRAPHICS_PIPELINE_STATE_DESC psoDescMesh = {};
	psoDescMesh.InputLayout = { StandardVertexDescription, StandardVertexDescriptionNumElements };
	psoDescMesh.CullMode = CULL_MODE_NONE; // rasterizer state is too big. and currently I use only CullMode.
	psoDescMesh.BlendState = blendState;

	psoDescMesh.DepthStencilState = depthStencilState;
	psoDescMesh.SampleMask = UINT_MAX;
	psoDescMesh.PrimitiveTopologyType = PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDescMesh.NumRenderTargets = 1;
	psoDescMesh.RTVFormats[0] = FORMAT_R16G16B16A16_FLOAT;
	psoDescMesh.MultiSampleCount = 1;


	SHADER_CREATE_DESC vsDesc =
	{
		GetAssetFullPath(L"Shaders\\TemporalAA.hlsl"), L"VSMain", L"vs_6_0", nullopt
	};

	SHADER_CREATE_DESC psDesc =
	{
		GetAssetFullPath(L"Shaders\\TemporalAA.hlsl"), L"PSMain", L"ps_6_0", nullopt
	};
	psoDescMesh.vsDesc = &vsDesc;
	psoDescMesh.psDesc = &psDesc;

	GfxPipelineStateObject* TEMP_TemporalAAPSO = AbstractGfxLayer::CreatePSO();


	AbstractGfxLayer::BindSRV(TEMP_TemporalAAPSO, "CurrentColorTex", 0, 1);
	AbstractGfxLayer::BindSRV(TEMP_TemporalAAPSO, "PrevColorTex", 1, 1);
	AbstractGfxLayer::BindSRV(TEMP_TemporalAAPSO, "VelocityTex", 2, 1);
	AbstractGfxLayer::BindSRV(TEMP_TemporalAAPSO, "DepthTex", 3, 1);
	AbstractGfxLayer::BindSampler(TEMP_TemporalAAPSO, "samplerWrap", 0);
	AbstractGfxLayer::BindCBV(TEMP_TemporalAAPSO, "LightingParam", 0, sizeof(LightingParam));

	bool bSuccess = AbstractGfxLayer::InitPSO(TEMP_TemporalAAPSO, &psoDescMesh);

	if (bSuccess)
		TemporalAAPSO = shared_ptr<GfxPipelineStateObject>(TEMP_TemporalAAPSO);
}

void Corona::ToneMapPass()
{
#if USE_AFTERMATH
	NVAftermathMarker(dx12_rhi->AM_CL_Handle, "CopyPass");
#endif

	ProfileGPUScope(AbstractGfxLayer::GetGlobalCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "CopyPass");

	GfxTexture* ResolveTarget = ColorBuffers[ColorBufferWriteIndex].get();

	AbstractGfxLayer::SetPSO(ToneMapPSO.get(), AbstractGfxLayer::GetGlobalCommandList());

	AbstractGfxLayer::SetSampler("samplerWrap", AbstractGfxLayer::GetGlobalCommandList(), ToneMapPSO.get(), samplerBilinearWrap.get());

	AbstractGfxLayer::SetReadTexture(ToneMapPSO.get(), "SrcTex", ResolveTarget, AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetReadBuffer(ToneMapPSO.get(), "Exposure", ExposureData.get(), AbstractGfxLayer::GetGlobalCommandList());

	ToneMapCB.Offset = glm::vec4(0, 0, 0, 0);
	ToneMapCB.Scale = glm::vec4(1, 1, 0, 0);
	ToneMapCB.ToneMapMode = ToneMapMode;
	AbstractGfxLayer::SetUniformValue(ToneMapPSO.get(), "ScaleOffsetParams", &ToneMapCB, AbstractGfxLayer::GetGlobalCommandList());

	AbstractGfxLayer::SetPSO(ToneMapPSO.get(), AbstractGfxLayer::GetGlobalCommandList());

	UINT Width = DisplayWidth;
	UINT Height = DisplayHeight;

	ViewPort viewPort = { 0.0f, 0.0f, static_cast<float>(Width), static_cast<float>(Height) };
	AbstractGfxLayer::SetViewports(AbstractGfxLayer::GetGlobalCommandList(), 1, &viewPort);

	Rect scissorRect = { 0, 0, static_cast<LONG>(Width), static_cast<LONG>(Height) };
	AbstractGfxLayer::SetScissorRects(AbstractGfxLayer::GetGlobalCommandList(), 1, &scissorRect);

	AbstractGfxLayer::SetPrimitiveTopology(AbstractGfxLayer::GetGlobalCommandList(), PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	AbstractGfxLayer::SetVertexBuffer(AbstractGfxLayer::GetGlobalCommandList(), 0, 1, FullScreenVB.get());
	
	AbstractGfxLayer::DrawInstanced(AbstractGfxLayer::GetGlobalCommandList(), 4, 1, 0, 0);
}

void Corona::DebugPass()
{
#if USE_AFTERMATH
	NVAftermathMarker(dx12_rhi->AM_CL_Handle, "DebugPass");
#endif
	ProfileGPUScope(AbstractGfxLayer::GetGlobalCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "DebugPass");

	AbstractGfxLayer::SetPSO(BufferVisualizePSO.get(), AbstractGfxLayer::GetGlobalCommandList());

	AbstractGfxLayer::SetSampler("samplerWrap", AbstractGfxLayer::GetGlobalCommandList(), BufferVisualizePSO.get(), samplerBilinearWrap.get());


	AbstractGfxLayer::SetPrimitiveTopology(AbstractGfxLayer::GetGlobalCommandList(), PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	AbstractGfxLayer::SetVertexBuffer(AbstractGfxLayer::GetGlobalCommandList(), 0, 1, FullScreenVB.get());

#if USE_RTXGI
	if (bDrawIrradiance)
	{
		DebugPassCB cb;

		cb.Offset = glm::vec4(0, 0, 0, 0);
		cb.Scale = glm::vec4(IrradianceScale, IrradianceScale, 0, 0);

		cb.DebugMode = RAW_COPY;
		AbstractGfxLayer::SetUniformValue(BufferVisualizePSO.get(), "DebugPassCB", &cb, AbstractGfxLayer::GetGlobalCommandList());
		AbstractGfxLayer::SetReadTexture(BufferVisualizePSO.get(), "SrcTex", probeIrradiance.get(), AbstractGfxLayer::GetGlobalCommandList());

		AbstractGfxLayer::DrawInstanced(AbstractGfxLayer::GetGlobalCommandList(), 4, 1, 0, 0);
	}

	if (bDrawDistance)
	{
		DebugPassCB cb;

		cb.Offset = glm::vec4(0, 0, 0, 0);
		cb.Scale = glm::vec4(DistanceScale, DistanceScale, 0, 0);

		cb.DebugMode = RAW_COPY;
		AbstractGfxLayer::SetUniformValue(BufferVisualizePSO.get(), "DebugPassCB", &cb, AbstractGfxLayer::GetGlobalCommandList());
		AbstractGfxLayer::SetReadTexture(BufferVisualizePSO.get(), "SrcTex", probeDistance.get(), AbstractGfxLayer::GetGlobalCommandList());

		AbstractGfxLayer::DrawInstanced(AbstractGfxLayer::GetGlobalCommandList(), 4, 1, 0, 0);
	}

	if (bDrawDistance || bDrawIrradiance) return;
#endif

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
		AbstractGfxLayer::SetUniformValue(BufferVisualizePSO.get(), "DebugPassCB", &cb, AbstractGfxLayer::GetGlobalCommandList());
		AbstractGfxLayer::SetReadTexture(BufferVisualizePSO.get(), "SrcTex", ShadowBuffer.get(), AbstractGfxLayer::GetGlobalCommandList());

		AbstractGfxLayer::DrawInstanced(AbstractGfxLayer::GetGlobalCommandList(), 4, 1, 0, 0);
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
		AbstractGfxLayer::SetUniformValue(BufferVisualizePSO.get(), "DebugPassCB", &cb, AbstractGfxLayer::GetGlobalCommandList());
		AbstractGfxLayer::SetReadTexture(BufferVisualizePSO.get(), "SrcTex", NormalBuffers[ColorBufferWriteIndex].get(), AbstractGfxLayer::GetGlobalCommandList());

		AbstractGfxLayer::DrawInstanced(AbstractGfxLayer::GetGlobalCommandList(), 4, 1, 0, 0);
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
		AbstractGfxLayer::SetUniformValue(BufferVisualizePSO.get(), "DebugPassCB", &cb, AbstractGfxLayer::GetGlobalCommandList());
		AbstractGfxLayer::SetReadTexture(BufferVisualizePSO.get(), "SrcTex", GeomNormalBuffer.get(), AbstractGfxLayer::GetGlobalCommandList());

		AbstractGfxLayer::DrawInstanced(AbstractGfxLayer::GetGlobalCommandList(), 4, 1, 0, 0);
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
		AbstractGfxLayer::SetUniformValue(BufferVisualizePSO.get(), "DebugPassCB", &cb, AbstractGfxLayer::GetGlobalCommandList());
		AbstractGfxLayer::SetReadTexture(BufferVisualizePSO.get(), "SrcTex", BloomBlurPingPong[0].get(), AbstractGfxLayer::GetGlobalCommandList());

		AbstractGfxLayer::DrawInstanced(AbstractGfxLayer::GetGlobalCommandList(), 4, 1, 0, 0);
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
		AbstractGfxLayer::SetUniformValue(BufferVisualizePSO.get(), "DebugPassCB", &cb, AbstractGfxLayer::GetGlobalCommandList());
		AbstractGfxLayer::SetReadTexture(BufferVisualizePSO.get(), "SrcTex", UnjitteredDepthBuffers[ColorBufferWriteIndex].get(), AbstractGfxLayer::GetGlobalCommandList());

		AbstractGfxLayer::DrawInstanced(AbstractGfxLayer::GetGlobalCommandList(), 4, 1, 0, 0);
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
		AbstractGfxLayer::SetUniformValue(BufferVisualizePSO.get(), "DebugPassCB", &cb, AbstractGfxLayer::GetGlobalCommandList());
		AbstractGfxLayer::SetReadTexture(BufferVisualizePSO.get(), "SrcTex", DiffuseGISHRaw.get(), AbstractGfxLayer::GetGlobalCommandList());

		AbstractGfxLayer::DrawInstanced(AbstractGfxLayer::GetGlobalCommandList(), 4, 1, 0, 0);
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
		AbstractGfxLayer::SetUniformValue(BufferVisualizePSO.get(), "DebugPassCB", &cb, AbstractGfxLayer::GetGlobalCommandList());
		AbstractGfxLayer::SetReadTexture(BufferVisualizePSO.get(), "SrcTex", DiffuseGICoCgRaw.get(), AbstractGfxLayer::GetGlobalCommandList());

		AbstractGfxLayer::DrawInstanced(AbstractGfxLayer::GetGlobalCommandList(), 4, 1, 0, 0);
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
		AbstractGfxLayer::SetUniformValue(BufferVisualizePSO.get(), "DebugPassCB", &cb, AbstractGfxLayer::GetGlobalCommandList());
		AbstractGfxLayer::SetReadTexture(BufferVisualizePSO.get(), "SrcTex", DiffuseGISHTemporal[GIBufferWriteIndex].get(), AbstractGfxLayer::GetGlobalCommandList());

		AbstractGfxLayer::DrawInstanced(AbstractGfxLayer::GetGlobalCommandList(), 4, 1, 0, 0);
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
		AbstractGfxLayer::SetUniformValue(BufferVisualizePSO.get(), "DebugPassCB", &cb, AbstractGfxLayer::GetGlobalCommandList());
		AbstractGfxLayer::SetReadTexture(BufferVisualizePSO.get(), "SrcTex", DiffuseGISHSpatial[0].get(), AbstractGfxLayer::GetGlobalCommandList());

		AbstractGfxLayer::DrawInstanced(AbstractGfxLayer::GetGlobalCommandList(), 4, 1, 0, 0);
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// sh lighting result
		DebugPassCB cb;

		cb.RTSize = glm::vec2(RenderWidth, RenderHeight);
		cb.GIBufferScale = GIBufferScale;
		if (eFS == EDebugVisualization::FINAL_INDIRECT_DIFFUSE)
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

#if USE_NRD
		if (bNRDDenoising)
		{
			cb.DebugMode = RAW_COPY;
			AbstractGfxLayer::SetUniformValue(BufferVisualizePSO.get(), "DebugPassCB", &cb, AbstractGfxLayer::GetGlobalCommandList());
			AbstractGfxLayer::SetReadTexture(BufferVisualizePSO.get(), "SrcTex", DiffuseGI_NRD.get(), AbstractGfxLayer::GetGlobalCommandList());

		}
		else
#endif
		{
			cb.DebugMode = SH_LIGHTING;
			AbstractGfxLayer::SetUniformValue(BufferVisualizePSO.get(), "DebugPassCB", &cb, AbstractGfxLayer::GetGlobalCommandList());
			AbstractGfxLayer::SetReadTexture(BufferVisualizePSO.get(), "SrcTex", DiffuseGICoCgSpatial[0].get(), AbstractGfxLayer::GetGlobalCommandList());
			AbstractGfxLayer::SetReadTexture(BufferVisualizePSO.get(), "SrcTexSH", DiffuseGISHSpatial[0].get(), AbstractGfxLayer::GetGlobalCommandList());
			AbstractGfxLayer::SetReadTexture(BufferVisualizePSO.get(), "SrcTexNormal", NormalBuffers[0].get(), AbstractGfxLayer::GetGlobalCommandList());

		}
		AbstractGfxLayer::DrawInstanced(AbstractGfxLayer::GetGlobalCommandList(), 4, 1, 0, 0);
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
		AbstractGfxLayer::SetUniformValue(BufferVisualizePSO.get(), "DebugPassCB", &cb, AbstractGfxLayer::GetGlobalCommandList());
		AbstractGfxLayer::SetReadTexture(BufferVisualizePSO.get(), "SrcTex", AlbedoBuffer.get(), AbstractGfxLayer::GetGlobalCommandList());

		AbstractGfxLayer::DrawInstanced(AbstractGfxLayer::GetGlobalCommandList(), 4, 1, 0, 0);
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
		AbstractGfxLayer::SetUniformValue(BufferVisualizePSO.get(), "DebugPassCB", &cb, AbstractGfxLayer::GetGlobalCommandList());
		AbstractGfxLayer::SetReadTexture(BufferVisualizePSO.get(), "SrcTex", VelocityBuffer.get(), AbstractGfxLayer::GetGlobalCommandList());

		AbstractGfxLayer::DrawInstanced(AbstractGfxLayer::GetGlobalCommandList(), 4, 1, 0, 0);
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
		AbstractGfxLayer::SetUniformValue(BufferVisualizePSO.get(), "DebugPassCB", &cb, AbstractGfxLayer::GetGlobalCommandList());
		AbstractGfxLayer::SetReadTexture(BufferVisualizePSO.get(), "SrcTex", RoughnessMetalicBuffer.get(), AbstractGfxLayer::GetGlobalCommandList());

		AbstractGfxLayer::DrawInstanced(AbstractGfxLayer::GetGlobalCommandList(), 4, 1, 0, 0);
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
		AbstractGfxLayer::SetUniformValue(BufferVisualizePSO.get(), "DebugPassCB", &cb, AbstractGfxLayer::GetGlobalCommandList());
		AbstractGfxLayer::SetReadTexture(BufferVisualizePSO.get(), "SrcTex", SpeculaGIBufferRaw.get(), AbstractGfxLayer::GetGlobalCommandList());

		AbstractGfxLayer::DrawInstanced(AbstractGfxLayer::GetGlobalCommandList(), 4, 1, 0, 0);
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
#if USE_NRD
		if (bNRDDenoising)
		{
			cb.DebugMode = RAW_COPY;
			AbstractGfxLayer::SetUniformValue(BufferVisualizePSO.get(), "DebugPassCB", &cb, AbstractGfxLayer::GetGlobalCommandList());
			AbstractGfxLayer::SetReadTexture(BufferVisualizePSO.get(), "SrcTex", SpecularGI_NRD.get(), AbstractGfxLayer::GetGlobalCommandList());

		}
		else
#endif
		{
			cb.DebugMode = RAW_COPY;
			AbstractGfxLayer::SetUniformValue(BufferVisualizePSO.get(), "DebugPassCB", &cb, AbstractGfxLayer::GetGlobalCommandList());
			AbstractGfxLayer::SetReadTexture(BufferVisualizePSO.get(), "SrcTex", SpeculaGIBufferTemporal[GIBufferWriteIndex].get(), AbstractGfxLayer::GetGlobalCommandList());

		}

		
		AbstractGfxLayer::DrawInstanced(AbstractGfxLayer::GetGlobalCommandList(), 4, 1, 0, 0);
	});

	functions.push_back([&](EDebugVisualization eFS) {
		// history length
		DebugPassCB cb;


		if (eFS == EDebugVisualization::SPEC_HISTORY_LENGTH)
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

		cb.DebugMode = CHANNEL_W;
		AbstractGfxLayer::SetUniformValue(BufferVisualizePSO.get(), "DebugPassCB", &cb, AbstractGfxLayer::GetGlobalCommandList());
		AbstractGfxLayer::SetReadTexture(BufferVisualizePSO.get(), "SrcTex", SpeculaGIBufferTemporal[GIBufferWriteIndex].get(), AbstractGfxLayer::GetGlobalCommandList());

		AbstractGfxLayer::DrawInstanced(AbstractGfxLayer::GetGlobalCommandList(), 4, 1, 0, 0);
	});

#if USE_RTXGI
	functions.push_back([&, this](EDebugVisualization eFS) {
		// RTXGI result
		DebugPassCB cb;


		if (eFS == EDebugVisualization::RTXGI_RESULT)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(-0.25, 0.75, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RTXGI_LIGHTING;
		cb.RTSize = glm::vec2(RenderWidth, RenderHeight);

		cb.InvViewMatrix = glm::transpose(InvViewMat);
		cb.InvProjMatrix = glm::transpose(InvProjMat);
		cb.CameraPosition = glm::vec4(m_camera.m_position, 0);

		AbstractGfxLayer::SetUniformValue(BufferVisualizePSO.get(), "DebugPassCB", &cb, AbstractGfxLayer::GetGlobalCommandList());

		UINT64 offset = dx12_rhi->CurrentFrameIndex * rtxgi::GetDDGIVolumeConstantBufferSize();

		AbstractGfxLayer::SetUniformBuffer(BufferVisualizePSO.get(), "DDGIVolume", VolumeCB.get(), offset, AbstractGfxLayer::GetGlobalCommandList());
		AbstractGfxLayer::SetReadTexture(BufferVisualizePSO.get(), "DDGIProbeIrradianceSRV", probeIrradiance.get(), AbstractGfxLayer::GetGlobalCommandList());
		AbstractGfxLayer::SetReadTexture(BufferVisualizePSO.get(), "DDGIProbeDistanceSRV", probeDistance.get(), AbstractGfxLayer::GetGlobalCommandList());
		AbstractGfxLayer::SetReadTexture(BufferVisualizePSO.get(), "DepthTex", DepthBuffer.get(), AbstractGfxLayer::GetGlobalCommandList());
		AbstractGfxLayer::SetReadTexture(BufferVisualizePSO.get(), "SrcTexNormal", NormalBuffers[ColorBufferWriteIndex].get(), AbstractGfxLayer::GetGlobalCommandList());
		AbstractGfxLayer::SetWriteTexture(BufferVisualizePSO.get(), "DDGIProbeStates", probeStates.get(), AbstractGfxLayer::GetGlobalCommandList());
		AbstractGfxLayer::SetWriteTexture(BufferVisualizePSO.get(), "DDGIProbeOffsets", probeOffsets.get(), AbstractGfxLayer::GetGlobalCommandList());

		AbstractGfxLayer::SetSampler("TrilinearSampler", AbstractGfxLayer::GetGlobalCommandList(), BufferVisualizePSO.get(), samplerTrilinearClamp.get());

		AbstractGfxLayer::DrawInstanced(AbstractGfxLayer::GetGlobalCommandList(), 4, 1, 0, 0);
	});
#endif

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
	ProfileGPUScope(AbstractGfxLayer::GetGlobalCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "LightingPass");
	
	{
		std::vector<ResourceTransition> Transition = { {LightingBuffer.get(),  RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_RENDER_TARGET} };
		AbstractGfxLayer::TransitionResource(AbstractGfxLayer::GetGlobalCommandList(), Transition.size(), Transition.data());
	}
	AbstractGfxLayer::SetPSO(LightingPSO.get(), AbstractGfxLayer::GetGlobalCommandList());

	AbstractGfxLayer::SetSampler("samplerWrap", AbstractGfxLayer::GetGlobalCommandList(), LightingPSO.get(), samplerBilinearWrap.get());


	AbstractGfxLayer::SetSampler("TrilinearSampler", AbstractGfxLayer::GetGlobalCommandList(), LightingPSO.get(), samplerTrilinearClamp.get());


	AbstractGfxLayer::SetReadTexture(LightingPSO.get(), "AlbedoTex", AlbedoBuffer.get(), AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetReadTexture(LightingPSO.get(), "NormalTex", NormalBuffers[ColorBufferWriteIndex].get(), AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetReadTexture(LightingPSO.get(), "ShadowTex", ShadowBuffer.get(), AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetReadTexture(LightingPSO.get(), "VelocityTex", VelocityBuffer.get(), AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetReadTexture(LightingPSO.get(), "DepthTex", DepthBuffer.get(), AbstractGfxLayer::GetGlobalCommandList());

#if USE_NRD
	if (bNRDDenoising)
	{
		AbstractGfxLayer::SetReadTexture(LightingPSO.get(), "DiffuseGITex", DiffuseGI_NRD.get(), AbstractGfxLayer::GetGlobalCommandList());
		AbstractGfxLayer::SetReadTexture(LightingPSO.get(), "SpecularGITex", SpecularGI_NRD.get(), AbstractGfxLayer::GetGlobalCommandList());
	}
	else
#endif
	{
		AbstractGfxLayer::SetReadTexture(LightingPSO.get(), "GIResultSHTex", DiffuseGISHSpatial[0].get(), AbstractGfxLayer::GetGlobalCommandList());
		AbstractGfxLayer::SetReadTexture(LightingPSO.get(), "GIResultColorTex", DiffuseGICoCgSpatial[0].get(), AbstractGfxLayer::GetGlobalCommandList());
		AbstractGfxLayer::SetReadTexture(LightingPSO.get(), "SpecularGITex", SpeculaGIBufferTemporal[GIBufferWriteIndex].get(), AbstractGfxLayer::GetGlobalCommandList());

	}
	AbstractGfxLayer::SetReadTexture(LightingPSO.get(), "RoughnessMetalicTex", RoughnessMetalicBuffer.get(), AbstractGfxLayer::GetGlobalCommandList());

#if USE_RTXGI
	AbstractGfxLayer::SetReadTexture(LightingPSO.get(), "DDGIProbeIrradianceSRV", probeIrradiance.get(), AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetReadTexture(LightingPSO.get(), "DDGIProbeDistanceSRV", probeDistance.get(), AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetWriteTexture(LightingPSO.get(), "DDGIProbeStates", probeStates.get(), AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetWriteTexture(LightingPSO.get(), "DDGIProbeOffsets", probeOffsets.get(), AbstractGfxLayer::GetGlobalCommandList());
#endif

	glm::mat4x4 InvViewMat = glm::inverse(ViewMat);
	LightingParam Param;
	Param.ViewMatrix = glm::transpose(ViewMat);
	Param.InvViewMatrix = glm::transpose(InvViewMat);
	Param.InvProjMatrix = glm::transpose(InvProjMat);

	Param.LightDir = glm::vec4(glm::normalize(LightDir), LightIntensity);
	
	Param.CameraPosition = glm::vec4(m_camera.m_position, 0);

	Param.RTSize.x = RenderWidth;
	Param.RTSize.y = RenderHeight;

	Param.DiffuseGIMode = DiffuseGIMethod;
	Param.GIBufferScale = GIBufferScale;

#if USE_NRD
	if (bNRDDenoising)
		Param.UseNRD = 1;
	else
		Param.UseNRD = 0;
#endif

	glm::normalize(Param.LightDir);
	AbstractGfxLayer::SetUniformValue(LightingPSO.get(), "LightingParam", &Param, AbstractGfxLayer::GetGlobalCommandList());
#if USE_RTXGI
	UINT64 offset = dx12_rhi->CurrentFrameIndex * rtxgi::GetDDGIVolumeConstantBufferSize();
	AbstractGfxLayer::SetUniformBuffer(LightingPSO.get(), "DDGIVolume", VolumeCB.get(), offset, AbstractGfxLayer::GetGlobalCommandList());
#endif

	AbstractGfxLayer::SetPSO(LightingPSO.get(), AbstractGfxLayer::GetGlobalCommandList());


	std::vector<GfxTexture*> Rendertargets = { LightingBuffer.get()};
	AbstractGfxLayer::SetRenderTargets(AbstractGfxLayer::GetGlobalCommandList(), Rendertargets.size(), Rendertargets.data(), nullptr);

	ViewPort viewPort = { 0.0f, 0.0f, static_cast<float>(RenderWidth), static_cast<float>(RenderHeight), 0, 1 };
	AbstractGfxLayer::SetViewports(AbstractGfxLayer::GetGlobalCommandList(), 1, &viewPort);
	AbstractGfxLayer::SetScissorRects(AbstractGfxLayer::GetGlobalCommandList(), 1, &m_scissorRect);

	AbstractGfxLayer::SetPrimitiveTopology(AbstractGfxLayer::GetGlobalCommandList(), PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	AbstractGfxLayer::SetVertexBuffer(AbstractGfxLayer::GetGlobalCommandList(), 0, 1, FullScreenVB.get());

	AbstractGfxLayer::DrawInstanced(AbstractGfxLayer::GetGlobalCommandList(), 4, 1, 0, 0);
	
	std::vector<ResourceTransition> Transition = { {LightingBuffer.get(),  RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE} };
	AbstractGfxLayer::TransitionResource(AbstractGfxLayer::GetGlobalCommandList(), Transition.size(), Transition.data());
}

void Corona::TemporalAAPass()
{
#if USE_AFTERMATH
	NVAftermathMarker(dx12_rhi->AM_CL_Handle, "TemporalAAPass");
#endif
	ProfileGPUScope(AbstractGfxLayer::GetGlobalCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "TemporalAAPass");

	UINT PrevColorBufferIndex = 1 - ColorBufferWriteIndex;
	GfxTexture* ResolveTarget = ColorBuffers[ColorBufferWriteIndex].get();

	std::array<ResourceTransition, 1> Transition0 = { {
		{ResolveTarget, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_RENDER_TARGET},
	} };
	AbstractGfxLayer::TransitionResource(AbstractGfxLayer::GetGlobalCommandList(), Transition0.size(), Transition0.data());

	AbstractGfxLayer::SetPSO(TemporalAAPSO.get(), AbstractGfxLayer::GetGlobalCommandList());

	AbstractGfxLayer::SetSampler("samplerWrap", AbstractGfxLayer::GetGlobalCommandList(), TemporalAAPSO.get(), samplerBilinearWrap.get());


	AbstractGfxLayer::SetReadTexture(TemporalAAPSO.get(), "CurrentColorTex", LightingWithBloomBuffer.get(), AbstractGfxLayer::GetGlobalCommandList());


	GfxTexture* PrevColorBuffer = ColorBuffers[PrevColorBufferIndex].get();
	AbstractGfxLayer::SetReadTexture(TemporalAAPSO.get(), "PrevColorTex", PrevColorBuffer, AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetReadTexture(TemporalAAPSO.get(), "VelocityTex", VelocityBuffer.get(), AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetReadTexture(TemporalAAPSO.get(), "DepthTex", DepthBuffer.get(), AbstractGfxLayer::GetGlobalCommandList());

	TemporalAAParam Param;

	Param.RTSize.x = RenderWidth;
	Param.RTSize.y = RenderHeight;

	if(AAMethod == TEMPORAL_AA)
	Param.TAABlendFactor = 0.1;
	else
		Param.TAABlendFactor = 1.0;

	Param.ClampMode = ClampMode;

	AbstractGfxLayer::SetUniformValue(TemporalAAPSO.get(), "LightingParam", &Param, AbstractGfxLayer::GetGlobalCommandList());

	AbstractGfxLayer::SetPSO(TemporalAAPSO.get(), AbstractGfxLayer::GetGlobalCommandList());


	std::vector<GfxTexture*> Rendertargets = { ResolveTarget };
	AbstractGfxLayer::SetRenderTargets(AbstractGfxLayer::GetGlobalCommandList(), Rendertargets.size(), Rendertargets.data(), nullptr);

	ViewPort viewPort = { 0.0f, 0.0f, static_cast<float>(RenderWidth), static_cast<float>(RenderHeight), 0, 1 };
	AbstractGfxLayer::SetViewports(AbstractGfxLayer::GetGlobalCommandList(), 1, &viewPort);

	AbstractGfxLayer::SetScissorRects(AbstractGfxLayer::GetGlobalCommandList(), 1, &m_scissorRect);
	AbstractGfxLayer::SetPrimitiveTopology(AbstractGfxLayer::GetGlobalCommandList(), PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	AbstractGfxLayer::SetVertexBuffer(AbstractGfxLayer::GetGlobalCommandList(), 0, 1, FullScreenVB.get());
	
	AbstractGfxLayer::DrawInstanced(AbstractGfxLayer::GetGlobalCommandList(), 4, 1, 0, 0);
		
	std::array<ResourceTransition, 1> Transition1 = { {
		{ResolveTarget, RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_UNORDERED_ACCESS},
	} };
	AbstractGfxLayer::TransitionResource(AbstractGfxLayer::GetGlobalCommandList(), Transition1.size(), Transition1.data());

	if (bDrawHistogram)
	{
		AbstractGfxLayer::SetPSO(DrawHistogramPSO.get(), AbstractGfxLayer::GetGlobalCommandList());

		AbstractGfxLayer::SetReadBuffer(DrawHistogramPSO.get(), "Histogram", Histogram.get(), AbstractGfxLayer::GetGlobalCommandList());
		AbstractGfxLayer::SetReadBuffer(DrawHistogramPSO.get(), "Exposure", ExposureData.get(), AbstractGfxLayer::GetGlobalCommandList());
		AbstractGfxLayer::SetWriteTexture(DrawHistogramPSO.get(), "ColorBuffer", ResolveTarget, AbstractGfxLayer::GetGlobalCommandList());

		AbstractGfxLayer::Dispatch(AbstractGfxLayer::GetGlobalCommandList(), 1, 32, 1);
	}

	std::array<ResourceTransition, 1> Transition2 = { {
		{ResolveTarget, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE},
	} };
	AbstractGfxLayer::TransitionResource(AbstractGfxLayer::GetGlobalCommandList(), Transition2.size(), Transition2.data());
}

#if USE_DLSS
void Corona::DLSSPass()
{
#if USE_AFTERMATH
	NVAftermathMarker(dx12_rhi->AM_CL_Handle, "DLSSPass");
#endif
	ProfileGPUScope(AbstractGfxLayer::GetGlobalCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "DLSSPass");

	UINT PrevColorBufferIndex = 1 - ColorBufferWriteIndex;
	Texture* ResolveTarget = ColorBuffers[ColorBufferWriteIndex].get();

	std::array<ResourceTransition, 1> Transition0 = { {
	{ResolveTarget, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_UNORDERED_ACCESS},
	} };
	AbstractGfxLayer::TransitionResource(AbstractGfxLayer::GetGlobalCommandList(), Transition0.size(), Transition0.data());

	NVSDK_NGX_Result Result;

	ID3D12GraphicsCommandList* d3dcommandList = AbstractGfxLayer::GetGlobalCommandList()->CmdList.Get();
	ID3D12Resource* unresolvedColorBuffer = LightingWithBloomBuffer->resource.Get();
	ID3D12Resource* motionVectorsBuffer = PixelVelocityBuffer->resource.Get();
	ID3D12Resource* resolvedColorBuffer = ResolveTarget->resource.Get();
	ID3D12Resource* depthBuffer = DepthBuffer->resource.Get();

	D3D12_RESOURCE_DESC resDesc = depthBuffer->GetDesc();

	glm::vec2 JOffset;
	JOffset.x = JitterOffset.x * RenderWidth;
	JOffset.y = JitterOffset.y * RenderHeight;

	bool Reset = bResetDLSS;
	bResetDLSS = false;
	float fSharpness = 0.65f;
	glm::vec2 MVScale = { 1, 1 };
	Result = NGX_D3D12_EVALUATE_DLSS(m_dlssFeature, m_ngxParameters, d3dcommandList,
		unresolvedColorBuffer, motionVectorsBuffer, resolvedColorBuffer, depthBuffer,
		nullptr, nullptr, Reset, fSharpness, MVScale.x, MVScale.y, JOffset.x, JOffset.y);

	if (NVSDK_NGX_FAILED(Result))
	{
		stringstream ss;
		ss << "Failed to NVSDK_NGX_D3D12_EvaluateFeature for DLSS, code = " << Result << ", info: " << GetNGXResultAsString(Result) << "\n";
		OutputDebugStringA(ss.str().c_str());

	}
	std::array<ResourceTransition, 1> Transition1 = { {
	{ResolveTarget, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE},
	} };
	AbstractGfxLayer::TransitionResource(AbstractGfxLayer::GetGlobalCommandList(), Transition1.size(), Transition1.data());

	ID3D12DescriptorHeap* ppHeaps[] = { dx12_rhi->SRVCBVDescriptorHeapShaderVisible->DH.Get(), dx12_rhi->SamplerDescriptorHeapShaderVisible->DH.Get() };
	AbstractGfxLayer::GetGlobalCommandList()->CmdList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
}
#endif

void Corona::ResolvePixelVelocityPass()
{
	{
		std::array<ResourceTransition, 1> Transition = { {
		{PixelVelocityBuffer.get(), RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_RENDER_TARGET},
		} };
		AbstractGfxLayer::TransitionResource(AbstractGfxLayer::GetGlobalCommandList(), Transition.size(), Transition.data());
	}

	AbstractGfxLayer::SetPSO(ResolvePixelVelocityPSO.get(), AbstractGfxLayer::GetGlobalCommandList());


	AbstractGfxLayer::SetSampler("samplerWrap", AbstractGfxLayer::GetGlobalCommandList(), ResolvePixelVelocityPSO.get(), samplerBilinearWrap.get());

	AbstractGfxLayer::SetReadTexture(ResolvePixelVelocityPSO.get(), "SrcTex", VelocityBuffer.get(), AbstractGfxLayer::GetGlobalCommandList());

	struct ResolveVelocityCB
	{
		glm::vec2 RTSize;
	};
	ResolveVelocityCB ResolveVelocityCB;
	ResolveVelocityCB.RTSize = glm::vec2(RenderWidth, RenderHeight);

	AbstractGfxLayer::SetUniformValue(ResolvePixelVelocityPSO.get(), "ResolveVelocityCB", &ResolveVelocityCB, AbstractGfxLayer::GetGlobalCommandList());

	AbstractGfxLayer::SetPSO(ResolvePixelVelocityPSO.get(), AbstractGfxLayer::GetGlobalCommandList());

	UINT Width = RenderWidth;
	UINT Height = RenderHeight;

	ViewPort viewPort = { 0.0f, 0.0f, static_cast<float>(Width), static_cast<float>(Height) };
	AbstractGfxLayer::SetViewports(AbstractGfxLayer::GetGlobalCommandList(), 1, &viewPort);

	Rect scissorRect = { 0, 0, static_cast<LONG>(Width), static_cast<LONG>(Height) };
	AbstractGfxLayer::SetScissorRects(AbstractGfxLayer::GetGlobalCommandList(), 1, &scissorRect);


	std::vector<GfxTexture*> Rendertargets = { PixelVelocityBuffer.get() };
	AbstractGfxLayer::SetRenderTargets(AbstractGfxLayer::GetGlobalCommandList(), Rendertargets.size(), Rendertargets.data(), nullptr);

	AbstractGfxLayer::SetPrimitiveTopology(AbstractGfxLayer::GetGlobalCommandList(), PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	AbstractGfxLayer::SetVertexBuffer(AbstractGfxLayer::GetGlobalCommandList(), 0, 1, FullScreenVB.get());

	AbstractGfxLayer::DrawInstanced(AbstractGfxLayer::GetGlobalCommandList(), 4, 1, 0, 0);

	{
		std::array<ResourceTransition, 1> Transition = { {
		{PixelVelocityBuffer.get(), RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE},
		} };
		AbstractGfxLayer::TransitionResource(AbstractGfxLayer::GetGlobalCommandList(), Transition.size(), Transition.data());
	}
}

#if USE_RTXGI
void Corona::RTXGIPass()
{
	if (volume)
	{
		volume->SetOrigin(volumeTranslation);

		volume->Update(VolumeCB->resource.Get(), dx12_rhi->CurrentFrameIndex * rtxgi::GetDDGIVolumeConstantBufferSize());
	}


#if USE_AFTERMATH
	NVAftermathMarker(dx12_rhi->AM_CL_Handle, "RTXGIPass");
#endif
	ProfileGPUScope(AbstractGfxLayer::GetGlobalCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "RTXGIPass");


	PSO_RT_PROBE->NumInstance = vecBLAS.size();
	PSO_RT_PROBE->BeginShaderTable();

	PSO_RT_PROBE->SetUAV("global", "DDGIProbeRTRadiance", probeRTRadiance->GpuHandleUAV);
	PSO_RT_PROBE->SetUAV("global", "DDGIProbeStates", probeStates->GpuHandleUAV);
	PSO_RT_PROBE->SetSRV("global", "DDGIProbeOffsets", probeOffsets->GpuHandleUAV);


	PSO_RT_PROBE->SetSRV("global", "SceneBVH", TLAS->GPUHandle);
	PSO_RT_PROBE->SetSRV("global", "DDGIProbeIrradianceSRV", probeIrradiance->GpuHandleSRV);
	PSO_RT_PROBE->SetSRV("global", "DDGIProbeDistanceSRV", probeDistance->GpuHandleSRV);
	//PSO_RT_PROBE->SetSRV("global", "DDGIProbeStates", probeStates->GpuHandleSRV);
	//PSO_RT_PROBE->SetSRV("global", "DDGIProbeOffsets", probeOffsets->GpuHandleSRV);
	PSO_RT_PROBE->SetSRV("global", "BlueNoiseTex", BlueNoiseTex->GpuHandleSRV);
	
	UINT64 offset = dx12_rhi->CurrentFrameIndex * rtxgi::GetDDGIVolumeConstantBufferSize();

	PSO_RT_PROBE->SetCBVValue("global", "DDGIVolume", VolumeCB->resource->GetGPUVirtualAddress() + offset);
	LightInfoCB LightInfoCB;
	LightInfoCB.LightDirAndIntensity = glm::vec4(LightDir.x, LightDir.y, LightDir.z, LightIntensity);
	PSO_RT_PROBE->SetCBVValue("global", "LightInfoCB", &LightInfoCB);

	PSO_RT_PROBE->SetSampler("global", "TrilinearSampler", samplerTrilinearClamp.get());

	int i = 0;
	for (auto& as : vecBLAS)
	{
		auto& mesh = as->mesh;

		Texture* diffuseTex = mesh->Draws[0].mat->Diffuse.get();
		if (!diffuseTex)
			diffuseTex = DefaultWhiteTex.get();

		PSO_RT_PROBE->ResetHitProgram(i);

		PSO_RT_PROBE->StartHitProgram("HitGroup", i);
		PSO_RT_PROBE->AddDescriptor2HitProgram("HitGroup", mesh->Vb->GpuHandleSRV, i);
		PSO_RT_PROBE->AddDescriptor2HitProgram("HitGroup", mesh->Ib->GpuHandleSRV, i);
		PSO_RT_PROBE->AddDescriptor2HitProgram("HitGroup", InstancePropertyBuffer->GpuHandleSRV, i);
		PSO_RT_PROBE->AddDescriptor2HitProgram("HitGroup", diffuseTex->GpuHandleSRV, i);

		i++;
	}

	PSO_RT_PROBE->EndShaderTable(vecBLAS.size());


	PSO_RT_PROBE->Apply(volume->GetNumRaysPerProbe(), volume->GetNumProbes(), AbstractGfxLayer::GetGlobalCommandList());

	AbstractGfxLayer::GetGlobalCommandList()->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::UAV(probeRTRadiance->resource.Get()));
	

	if (volume)
	{
		volume->UpdateProbes(AbstractGfxLayer::GetGlobalCommandList()->CmdList.Get());
	}

	ID3D12DescriptorHeap* ppHeaps[] = { dx12_rhi->SRVCBVDescriptorHeapShaderVisible->DH.Get(), dx12_rhi->SamplerDescriptorHeapShaderVisible->DH.Get() };
	AbstractGfxLayer::GetGlobalCommandList()->CmdList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
}
#endif

void Corona::BloomPass()
{
#if USE_AFTERMATH
	NVAftermathMarker(dx12_rhi->AM_CL_Handle, "BloomPass");
#endif
	ProfileGPUScope(AbstractGfxLayer::GetGlobalCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "BloomPass");

	BloomCB.RTSize.x = BloomBufferWidth;
	BloomCB.RTSize.y = BloomBufferHeight;

	float sigma_pixels = BloomSigma * RenderHeight;

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
	
	std::array<ResourceTransition, 2> Transition0 = { {
		{BloomBlurPingPong[0].get(), RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_UNORDERED_ACCESS},
		{LumaBuffer.get(), RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_UNORDERED_ACCESS}
	} };
	AbstractGfxLayer::TransitionResource(AbstractGfxLayer::GetGlobalCommandList(), 2, Transition0.data());


	AbstractGfxLayer::SetPSO(BloomExtractPSO.get(), AbstractGfxLayer::GetGlobalCommandList());

	AbstractGfxLayer::SetReadTexture(BloomExtractPSO.get(), "SrcTex", LightingBuffer.get(), AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetReadBuffer(BloomExtractPSO.get(), "Exposure", ExposureData.get(), AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetWriteTexture(BloomExtractPSO.get(), "DstTex", BloomBlurPingPong[0].get(), AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetWriteTexture(BloomExtractPSO.get(), "LumaResult", LumaBuffer.get(), AbstractGfxLayer::GetGlobalCommandList());

	AbstractGfxLayer::SetSampler("samplerWrap", AbstractGfxLayer::GetGlobalCommandList(), BloomExtractPSO.get(), samplerBilinearWrap.get());

	AbstractGfxLayer::SetUniformValue(BloomExtractPSO.get(), "BloomCB", &BloomCB, AbstractGfxLayer::GetGlobalCommandList());


	AbstractGfxLayer::Dispatch(AbstractGfxLayer::GetGlobalCommandList(), BloomBufferWidth / 32, BloomBufferHeight / 32, 1);
	{
		std::array<ResourceTransition, 3> Transition = { {
			{BloomBlurPingPong[0].get(), RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE},
			{LumaBuffer.get(), RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE},
			{BloomBlurPingPong[1].get(), RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_UNORDERED_ACCESS},

		} };
		AbstractGfxLayer::TransitionResource(AbstractGfxLayer::GetGlobalCommandList(), Transition.size() , Transition.data());
	}
	

	AbstractGfxLayer::SetPSO(BloomBlurPSO.get(), AbstractGfxLayer::GetGlobalCommandList());

	AbstractGfxLayer::SetReadTexture(BloomBlurPSO.get(), "SrcTex", BloomBlurPingPong[0].get(), AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetWriteTexture(BloomBlurPSO.get(), "DstTex", BloomBlurPingPong[1].get(), AbstractGfxLayer::GetGlobalCommandList());

	AbstractGfxLayer::SetSampler("samplerWrap", AbstractGfxLayer::GetGlobalCommandList(), BloomBlurPSO.get(), samplerBilinearWrap.get());

	BloomCB.BlurDirection = glm::vec2(1, 0);
	AbstractGfxLayer::SetUniformValue(BloomBlurPSO.get(), "BloomCB", &BloomCB, AbstractGfxLayer::GetGlobalCommandList());

	AbstractGfxLayer::Dispatch(AbstractGfxLayer::GetGlobalCommandList(), BloomBufferWidth / 32, BloomBufferHeight / 32, 1);

	{
		std::array<ResourceTransition, 2> Transition = { {
		{BloomBlurPingPong[1].get(), RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE},
		{BloomBlurPingPong[0].get(), RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_UNORDERED_ACCESS}
		} };
		AbstractGfxLayer::TransitionResource(AbstractGfxLayer::GetGlobalCommandList(), Transition.size(), Transition.data());
	}

	AbstractGfxLayer::SetPSO(BloomBlurPSO.get(), AbstractGfxLayer::GetGlobalCommandList());

	AbstractGfxLayer::SetReadTexture(BloomBlurPSO.get(), "SrcTex", BloomBlurPingPong[1].get(), AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetWriteTexture(BloomBlurPSO.get(), "DstTex", BloomBlurPingPong[0].get(), AbstractGfxLayer::GetGlobalCommandList());

	AbstractGfxLayer::SetSampler("samplerWrap", AbstractGfxLayer::GetGlobalCommandList(), BloomBlurPSO.get(), samplerBilinearWrap.get());

	BloomCB.BlurDirection = glm::vec2(0, 1);
	AbstractGfxLayer::SetUniformValue(BloomBlurPSO.get(), "BloomCB", &BloomCB, AbstractGfxLayer::GetGlobalCommandList());

	AbstractGfxLayer::Dispatch(AbstractGfxLayer::GetGlobalCommandList(), BloomBufferWidth / 32, BloomBufferHeight / 32, 1);


	{
		std::vector<ResourceTransition> Transition = {
			ResourceTransition(BloomBlurPingPong[0].get(), RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
			ResourceTransition(Histogram.get(), RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_UNORDERED_ACCESS)
		};

		AbstractGfxLayer::TransitionResource(AbstractGfxLayer::GetGlobalCommandList(), Transition.size(), Transition.data());
	}


	AbstractGfxLayer::SetPSO(ClearHistogramPSO.get(), AbstractGfxLayer::GetGlobalCommandList());

	AbstractGfxLayer::SetWriteBuffer(ClearHistogramPSO.get(), "Histogram", Histogram.get(), AbstractGfxLayer::GetGlobalCommandList());

	AbstractGfxLayer::Dispatch(AbstractGfxLayer::GetGlobalCommandList(), 1, 1, 1);

	AbstractGfxLayer::SetPSO(HistogramPSO.get(), AbstractGfxLayer::GetGlobalCommandList());

	AbstractGfxLayer::SetReadTexture(HistogramPSO.get(), "LumaTex", LumaBuffer.get(), AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetWriteBuffer(HistogramPSO.get(), "Histogram", Histogram.get(), AbstractGfxLayer::GetGlobalCommandList());

	AbstractGfxLayer::Dispatch(AbstractGfxLayer::GetGlobalCommandList(), BloomBufferWidth / 16, 1, 1);

	{
		std::vector<ResourceTransition> Transition = {
			ResourceTransition(Histogram.get(), RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
			ResourceTransition(ExposureData.get(), RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_UNORDERED_ACCESS)
		};

		AbstractGfxLayer::TransitionResource(AbstractGfxLayer::GetGlobalCommandList(), Transition.size(), Transition.data());
	}

	AbstractGfxLayer::SetPSO(AdapteExposurePSO.get(), AbstractGfxLayer::GetGlobalCommandList());

	AbstractGfxLayer::SetReadBuffer(AdapteExposurePSO.get(), "Histogram", Histogram.get(), AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetWriteBuffer(AdapteExposurePSO.get(), "Exposure", ExposureData.get(), AbstractGfxLayer::GetGlobalCommandList());

	AdaptExposureCB.PixelCount = BloomBufferWidth * BloomBufferHeight;
	
	AbstractGfxLayer::SetUniformValue(AdapteExposurePSO.get(), "AdaptExposureCB", &AdaptExposureCB, AbstractGfxLayer::GetGlobalCommandList());

	AbstractGfxLayer::Dispatch(AbstractGfxLayer::GetGlobalCommandList(), 1, 1, 1);

	{
		std::vector<ResourceTransition> Transition = {
			ResourceTransition(ExposureData.get(), RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
			ResourceTransition(LightingWithBloomBuffer.get(), RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_RENDER_TARGET)
		};

		AbstractGfxLayer::TransitionResource(AbstractGfxLayer::GetGlobalCommandList(), Transition.size(), Transition.data());
	}

	AbstractGfxLayer::SetPSO(AddBloomPSO.get(), AbstractGfxLayer::GetGlobalCommandList());

	AbstractGfxLayer::SetSampler("samplerWrap", AbstractGfxLayer::GetGlobalCommandList(), AddBloomPSO.get(), samplerBilinearWrap.get());

	AbstractGfxLayer::SetReadTexture(AddBloomPSO.get(), "SrcTex", LightingBuffer.get(), AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetReadTexture(AddBloomPSO.get(), "BloomTex", BloomBlurPingPong[0].get(), AbstractGfxLayer::GetGlobalCommandList());

	AddBloomCB.Offset = glm::vec4(0, 0, 0, 0);
	AddBloomCB.Scale = glm::vec4(1, 1, 0, 0);
	AddBloomCB.BloomStrength = BloomStrength;
	AbstractGfxLayer::SetUniformValue(AddBloomPSO.get(), "AddBloomCB", &AddBloomCB, AbstractGfxLayer::GetGlobalCommandList());


	UINT Width = RenderWidth;
	UINT Height = RenderHeight;
	
	ViewPort viewPort = { 0.0f, 0.0f, static_cast<float>(Width), static_cast<float>(Height) };
	AbstractGfxLayer::SetViewports(AbstractGfxLayer::GetGlobalCommandList(), 1, &viewPort);

	Rect scissorRect = { 0, 0, static_cast<LONG>(Width), static_cast<LONG>(Height) };
	AbstractGfxLayer::SetScissorRects(AbstractGfxLayer::GetGlobalCommandList(), 1, &scissorRect);

	std::vector<GfxTexture*> Rendertargets = { LightingWithBloomBuffer.get() };
	AbstractGfxLayer::SetRenderTargets(AbstractGfxLayer::GetGlobalCommandList(), Rendertargets.size(), Rendertargets.data(), nullptr);

	AbstractGfxLayer::SetPrimitiveTopology(AbstractGfxLayer::GetGlobalCommandList(), PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	AbstractGfxLayer::SetVertexBuffer(AbstractGfxLayer::GetGlobalCommandList(), 0, 1, FullScreenVB.get());

	AbstractGfxLayer::DrawInstanced(AbstractGfxLayer::GetGlobalCommandList(), 4, 1, 0, 0);

	{
		std::vector<ResourceTransition> Transition = {
			ResourceTransition(LightingWithBloomBuffer.get(), RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
		};

		AbstractGfxLayer::TransitionResource(AbstractGfxLayer::GetGlobalCommandList(), Transition.size(), Transition.data());
	}
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

	InvViewMat = glm::inverse(ViewMat);
	InvProjMat = glm::inverse(ProjMat);

	RTShadowViewParam.ViewMatrix = glm::transpose(ViewMat);
	RTShadowViewParam.InvViewMatrix = glm::transpose(InvViewMat);
	RTShadowViewParam.ProjMatrix = glm::transpose(ProjMat);
	RTShadowViewParam.InvProjMatrix = glm::transpose(InvProjMat);
	RTShadowViewParam.ProjectionParams.x = Far / (Far - Near);
	RTShadowViewParam.ProjectionParams.y = Near / (Near - Far);
	RTShadowViewParam.ProjectionParams.z = Near;
	RTShadowViewParam.ProjectionParams.w = Far;
	RTShadowViewParam.LightDir = glm::vec4(LightDir, 0);

	glm::vec2 Jitter;
	uint64 idx = FrameCounter % 8;
	const vec2 offsets[] = {
		vec2(0.0625f, -0.1875f), vec2(-0.0625f, 0.1875f), vec2(0.3125f, 0.0625f), vec2(-0.1875f, -0.3125f),
		vec2(-0.3125f, 0.3125f), vec2(-0.4375f, 0.0625f), vec2(0.1875f, 0.4375f), vec2(0.4375f, -0.4375f)
	};
	Jitter = offsets[idx];// Hammersley2D(idx, 4) * 2.0f - glm::vec2(1.0f);
	Jitter *= JitterScale;

	const float offsetX = Jitter.x * (1.0f / RenderWidth);
	const float offsetY = Jitter.y * (1.0f / RenderHeight);

	//if (bEnableTAA)
	if (AAMethod == TEMPORAL_AA || AAMethod == DLSS)
		JitterOffset = glm::vec2(offsetX, offsetY);// (Jitter - PrevJitter) * 0.5f;
	/*else
		JitterOffset = glm::vec2(0, 0);*/
	
	PrevJitter = Jitter;
	glm::mat4x4 JitterMat = glm::translate(glm::vec3(offsetX, -offsetY, 0));
	

	//if(bEnableTAA)
	if(AAMethod == TEMPORAL_AA || AAMethod == DLSS)
		ProjMat = JitterMat * ProjMat;


	ViewProjMat = ProjMat * ViewMat;

	InvViewProjMat = glm::inverse(ViewProjMat);
	
	float timeElapsed = m_timer.GetTotalSeconds();
	timeElapsed *= 0.01f;
	// reflection view param
	RTReflectionViewParam.ViewMatrix = glm::transpose(ViewMat);
	RTReflectionViewParam.InvViewMatrix = glm::transpose(InvViewMat);
	RTReflectionViewParam.ProjMatrix = glm::transpose(ProjMat);
	RTReflectionViewParam.InvProjMatrix = glm::transpose(InvProjMat);
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
	RTGIViewParam.InvProjMatrix = glm::transpose(InvProjMat);
	RTGIViewParam.ProjectionParams.x = Far / (Far - Near);
	RTGIViewParam.ProjectionParams.y = Near / (Near - Far);
	RTGIViewParam.ProjectionParams.z = Near;
	RTGIViewParam.ProjectionParams.w = Far;
	RTGIViewParam.LightDir = glm::vec4(glm::normalize(LightDir), LightIntensity);
	RTGIViewParam.RandomOffset = glm::vec2(timeElapsed, timeElapsed);
	RTGIViewParam.FrameCounter = FrameCounter;
	
	SpatialFilterCB.ProjectionParams.z = Near;
	SpatialFilterCB.ProjectionParams.w = Far;


	TemporalFilterCB.InvViewMatrix = glm::transpose(InvViewMat);
	TemporalFilterCB.InvProjMatrix = glm::transpose(InvProjMat);
	TemporalFilterCB.ProjectionParams.z = Near;
	TemporalFilterCB.ProjectionParams.w = Far;
	TemporalFilterCB.RTSize.x = RenderWidth;
	TemporalFilterCB.RTSize.y = RenderHeight;
	TemporalFilterCB.FrameIndex = FrameCounter;

	FrameCounter++;

	if (bRecompileShaders)
	{
		RecompileShaders();
		bRecompileShaders = false;
	}

	if(AAMethod != PrevAAMethod)
	{
		if (AAMethod == TEMPORAL_AA || AAMethod == NO_AA)
		{
			ColorBuffers[0] = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R16G16B16A16_FLOAT,
				RESOURCE_FLAG_ALLOW_RENDER_TARGET | RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
				RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RenderWidth, RenderHeight, 1));

			NAME_TEXTURE(ColorBuffers[0]);

			ColorBuffers[1] = shared_ptr<GfxTexture>(AbstractGfxLayer::CreateTexture2D(FORMAT_R16G16B16A16_FLOAT,
				RESOURCE_FLAG_ALLOW_RENDER_TARGET | RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
				RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RenderWidth, RenderHeight, 1));

			NAME_TEXTURE(ColorBuffers[1]);
		}
#if USE_DLSS
		else if (AAMethod == DLSS)
		{
			ColorBuffers[0] = shared_ptr<Texture>(static_cast<Texture*>(AbstractGfxLayer::CreateTexture2D(FORMAT_R16G16B16A16_FLOAT,
				RESOURCE_FLAG_ALLOW_RENDER_TARGET | RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
				RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, DisplayWidth, DisplayHeight, 1)));

			NAME_TEXTURE(ColorBuffers[0]);

			ColorBuffers[1] = shared_ptr<Texture>(static_cast<Texture*>(AbstractGfxLayer::CreateTexture2D(FORMAT_R16G16B16A16_FLOAT,
				RESOURCE_FLAG_ALLOW_RENDER_TARGET | RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
				RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, DisplayWidth, DisplayHeight, 1)));

			NAME_TEXTURE(ColorBuffers[1]);

			bResetDLSS = true;
		}
#endif

		PrevAAMethod = AAMethod;
	}
}

// Render the scene.
void Corona::OnRender()
{
	std::list<GfxTexture*> DynamicTexture = {
	ColorBuffers[0].get(),
	ColorBuffers[1].get(),
	LightingBuffer.get(),
	LightingWithBloomBuffer.get(),
	NormalBuffers[0].get(),
	NormalBuffers[1].get(),
	GeomNormalBuffer.get(),
	ShadowBuffer.get(),
	SpeculaGIBufferRaw.get(),
	SpeculaGIBufferTemporal[0].get(),
	SpeculaGIBufferTemporal[1].get(),
	SpeculaGIMoments[0].get(),
	SpeculaGIMoments[1].get(),
	DiffuseGISHRaw.get(),
	DiffuseGICoCgRaw.get(),
	DiffuseGISHTemporal[0].get(),
	DiffuseGISHTemporal[1].get(),
	DiffuseGICoCgTemporal[0].get(),
	DiffuseGICoCgTemporal[1].get(),
	NormalRoughness_NRD.get(),
	LinearDepth_NRD.get(),
	DiffuseGI_NRD.get(),
	SpecularGI_NRD.get(),
	AlbedoBuffer.get(),
	VelocityBuffer.get(),
	PixelVelocityBuffer.get(),
	RoughnessMetalicBuffer.get(),
	DepthBuffer.get(),
	UnjitteredDepthBuffers[0].get(),
	UnjitteredDepthBuffers[1].get(),
	DiffuseGISHSpatial[0].get(),
	DiffuseGISHSpatial[1].get(),
	DiffuseGICoCgSpatial[0].get(),
	DiffuseGICoCgSpatial[1].get(),
	BloomBlurPingPong[0].get(),
	BloomBlurPingPong[1].get(),
	LumaBuffer.get(),

};

#if USE_RTXGI
	if (volume)
	{
		DynamicTexture.push_back(probeRTRadiance);
		DynamicTexture.push_back(probeIrradiance);
		DynamicTexture.push_back(probeDistance);
		DynamicTexture.push_back(probeOffsets);
		DynamicTexture.push_back(probeStates);
	}
#endif

	for (auto& fb : framebuffers)
		DynamicTexture.push_back(fb.get());

	AbstractGfxLayer::BeginFrame(DynamicTexture);
	
	// Record all the commands we need to render the scene into the command list.

	GBufferPass();

	ResolvePixelVelocityPass();


	RaytraceShadowPass();


	RaytraceReflectionPass();
	
	if (!bDebugDraw)
	{
		if(DiffuseGIMethod == PATH_TRACING)
			RaytraceGIPass();
#if USE_RTXGI
		else
			RTXGIPass();
#endif
	}
	else
	{
		RaytraceGIPass();
#if USE_RTXGI
		RTXGIPass();
#endif
	}
#if USE_NRD

	if (bNRDDenoising)
	{
		NRDPass();
	}
	else
#endif
	{
		TemporalDenoisingPass();


		SpatialDenoisingPass();
	}

	LightingPass();

	BloomPass();
	
	if (AAMethod == TEMPORAL_AA || AAMethod == NO_AA)
		TemporalAAPass();
#if USE_DLSS
	else if (AAMethod == DLSS)
		DLSSPass();
#endif

	
	GfxTexture* backbuffer = framebuffers[AbstractGfxLayer::GetCurrentFrameIndex()].get();

	{
		std::array<ResourceTransition, 1> Transition = { {
		{backbuffer, RESOURCE_STATE_PRESENT, RESOURCE_STATE_RENDER_TARGET},
		} };
		AbstractGfxLayer::TransitionResource(AbstractGfxLayer::GetGlobalCommandList(), Transition.size(), Transition.data());
	}
	
	std::vector<GfxTexture*> Rendertargets = { backbuffer };
	AbstractGfxLayer::SetRenderTargets(AbstractGfxLayer::GetGlobalCommandList(), Rendertargets.size(), Rendertargets.data(), nullptr);
	
	ToneMapPass();

	if(bDebugDraw)
		DebugPass();

	
	if (bShowImgui)
	{
#if USE_IMGUI

		ImGui_ImplDX12_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		bool show_demo_window = true;

		//ImGui::ShowDemoWindow(&show_demo_window);

		char fps[64];
		sprintf(fps, "FPS : %u fps", m_timer.GetFramesPerSecond());

		ImGui::Begin("Hi, Let's traceray!");
		ImGui::Text(fps);

		glm::vec4 test = glm::vec4(0, -0, 0, 1) * glm::transpose(UnjitteredViewProjMat);
		test.x /= test.w;
		test.y /= test.w;
		//test.z /= test.w;
		sprintf(fps, "Cam Pos : %f %f %f", m_camera.m_position.x, m_camera.m_position.y, m_camera.m_position.z);
		ImGui::Text(fps);

		if (ImGui::Button("Recompile all shaders"))
			bRecompileShaders = true;

		ImGui::Text("\nArrow keys : rotate camera imGui\
			\nWASD keys : move camera imGui\
			\nI : show/hide imGui\
			\nB : show/hide buffer visualization\
			\nT : cycle AA methods\n\n");

		ImGui::SliderFloat("Camera turn speed", &m_turnSpeed, 0.0f, glm::half_pi<float>()*2);

#if USE_RTXGI
		ImGui::Checkbox("Draw Irradiance Texture", &bDrawIrradiance);
		ImGui::SliderFloat("Irradiance Scale", &IrradianceScale, 0.1, 1.0f);

		ImGui::Checkbox("Draw Distance Texture", &bDrawDistance);
		ImGui::SliderFloat("Distance Scale", &DistanceScale, 0.1f, 1.0f);

		if (volume)
		{
			ImGui::SliderFloat("Probe Hysteresis", &probeHysteresis, 0.1f, 1.0f);
			volume->SetProbeHysteresis(probeHysteresis);

			ImGui::SliderFloat("Normal Bias", &normalBias, 0.0f, 10.0f);
			volume->SetNormalBias(normalBias);

			ImGui::SliderFloat("View Bias", &viewBias, 0.1f, 10.0f);
			volume->SetViewBias(viewBias);
		}
#endif

		ImGui::Checkbox("Visualize Buffers", &bDebugDraw);
		ImGui::Checkbox("Draw Histogram", &bDrawHistogram);

#if USE_NRD
		ImGui::Checkbox("Use NRD", &bNRDDenoising);
#endif 
		{
			static ImGuiComboFlags flags = 0;
			const char* items[] = {
					"PATH_TRACING",
					"RTXGI"
			};
			static const char* item_current = items[DiffuseGIMethod];
			if (ImGui::BeginCombo("DiffuseGIMode", item_current, flags))
			{
				for (int n = 0; n < IM_ARRAYSIZE(items); n++)
				{
					bool is_selected = (item_current == items[n]);
					if (ImGui::Selectable(items[n], is_selected))\
					{
						item_current = items[n];
						DiffuseGIMethod = (EDiffuseGIMode)n;
					}
					if (is_selected)
					{
						ImGui::SetItemDefaultFocus();
					}

				}
				ImGui::EndCombo();
			}

		}
	
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
		FINAL_INDIRECT_DIFFUSE,
		RTXGI_RESULT,
		ALBEDO,
		VELOCITY,
		ROUGNESS_METALLIC,
		SPECULAR_RAW,
		TEMPORAL_FILTERED_SPECULAR,
		BLOOM,
		SPEC_HISTORY_LENGTH,		
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
			"FINAL_INDIRECT_DIFFUSE",
			"RTXGI_RESULT",
			"ALBEDO",
			"VELOCITY",
			"ROUGNESS_METALLIC",
			"SPECULAR_RAW",
			"TEMPORAL_FILTERED_SPECULAR",
			"BLOOM",
			"SPEC_HISTORY_LENGTH",
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
				"FILMIC_ALU",
				"FILMIC_HABLE",
			};
			static const char* item_current = items[UINT(EToneMapMode::FILMIC_HABLE)];
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

		{
			static ImGuiComboFlags flags = 0;
			const char* items[] = {
				"TEMPORAL_AA",
				"DLSS",
				"NO_AA",
			};

			int idx = AAMethod;
			const char* item_current = items[idx];
			if (ImGui::BeginCombo("Anti aliasing method", item_current, flags))
			{
				for (int n = 0; n < IM_ARRAYSIZE(items); n++)
				{
					bool is_selected = (item_current == items[n]);
					if (ImGui::Selectable(items[n], is_selected))\
					{
						PrevAAMethod = AAMethod;
						item_current = items[n];
						AAMethod = (EAntialiasingMethod)n;
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
		//glm::vec3 LD = glm::vec3(LightDir.z, -LightDir.y, -LightDir.x);
		//ImGui::gizmo3D("##gizmo1", LD, 200 /* mode */);
		//LightDir = glm::vec3(-LD.z, -LD.y, LD.x);
		//ImGui::SameLine();
		//ImGui::Text("Light Direction");

#if USE_RTXGI
		ImGui::SliderFloat3("Volume Origin", (float*)&volumeTranslation, -3000, 3000);
#endif

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

		ImGui::SliderFloat("BayerRotScale", &TemporalFilterCB.BayerRotScale, 0.0f, 1.0f);

		ImGui::SliderFloat("SpecularBlurRadius", &TemporalFilterCB.SpecularBlurRadius, 0.0f, 5.0f);

		ImGui::SliderFloat("Point2PlaneDistScale", &TemporalFilterCB.Point2PlaneDistScale, 0.0f, 1000.0f);

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
		if(AbstractGfxLayer::IsDX12())
			ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), static_cast<CommandList*>(AbstractGfxLayer::GetGlobalCommandList())->CmdList.Get());

#endif // USE_IMGUI
	}

	{
		std::array<ResourceTransition, 1> Transition = { {
		{backbuffer, RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_PRESENT},
		} };
		AbstractGfxLayer::TransitionResource(AbstractGfxLayer::GetGlobalCommandList(), Transition.size(), Transition.data());
	}

	AbstractGfxLayer::ExecuteCommandList(AbstractGfxLayer::GetGlobalCommandList());

	AbstractGfxLayer::EndFrame();

	PrevViewProjMat = ViewProjMat;
	PrevViewMat = ViewMat;
	PrevProjMat = ProjMat;
	PrevInvViewMat = InvViewMat;
	PrevUnjitteredViewProjMat = UnjitteredViewProjMat;
}

void Corona::OnDestroy()
{
	AbstractGfxLayer::WaitGPUFlush();

#if USE_IMGUI
	ImGui_ImplDX12_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
#endif

#if USE_DLSS
	ShutDownDLSS();
#endif
	//m_NRD.Destroy();
}


void Corona::OnSizeChanged(UINT width, UINT height, bool minimized)
{
	// Determine if the swap buffers and other resources need to be resized or not.
	if ((width != m_width || height != m_height) && !minimized)
	{
		AbstractGfxLayer::OnSizeChanged(framebuffers, width, height, minimized);

		framebuffers.clear();
		AbstractGfxLayer::GetFrameBuffers(framebuffers);

	}

	m_windowVisible = !minimized;
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
		AAMethod = EAntialiasingMethod(AAMethod + 1);
		AAMethod = EAntialiasingMethod(AAMethod % (NO_AA + 1));
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
	/*case 'F':
		ThrowIfFailed(m_swapChain->SetFullscreenState(true, nullptr));
		break;*/
#if USE_NRD
	case 'N':

		bNRDDenoising = !bNRDDenoising;
		break;
#endif
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
		AbstractGfxLayer::SetIndexBuffer(AbstractGfxLayer::GetGlobalCommandList(), mesh->Ib.get());
		AbstractGfxLayer::SetVertexBuffer(AbstractGfxLayer::GetGlobalCommandList(), 0, 1, mesh->Vb.get());

		for (int i = 0; i < mesh->Draws.size(); i++)
		{
			GfxMesh::DrawCall& drawcall = mesh->Draws[i];
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

			objCB.RTSize.x = RenderWidth;
			objCB.RTSize.y = RenderHeight;

			objCB.RougnessMetalic.x = Roughness;
			objCB.RougnessMetalic.y = Metalic;

			objCB.bOverrideRougnessMetallic = bOverrideRoughnessMetallic ? 1 : 0;

			AbstractGfxLayer::SetUniformValue(GBufferPassPSO.get(), "GBufferConstantBuffer", &objCB, AbstractGfxLayer::GetGlobalCommandList());

			GfxTexture* AlbedoTex = drawcall.mat->Diffuse.get();
			if (AlbedoTex)
				AbstractGfxLayer::SetReadTexture(GBufferPassPSO.get(), "AlbedoTex", AlbedoTex, AbstractGfxLayer::GetGlobalCommandList());


			GfxTexture* NormalTex = drawcall.mat->Normal.get();
			if (NormalTex)
				AbstractGfxLayer::SetReadTexture(GBufferPassPSO.get(), "NormalTex", NormalTex, AbstractGfxLayer::GetGlobalCommandList());


			GfxTexture* RoughnessTex = drawcall.mat->Roughness.get();
			if (RoughnessTex)
				AbstractGfxLayer::SetReadTexture(GBufferPassPSO.get(), "RoughnessTex", RoughnessTex, AbstractGfxLayer::GetGlobalCommandList());

			GfxTexture* MetallicTex = drawcall.mat->Metallic.get();
			if (MetallicTex)
				AbstractGfxLayer::SetReadTexture(GBufferPassPSO.get(), "MetallicTex", MetallicTex, AbstractGfxLayer::GetGlobalCommandList());


			//AbstractGfxLayer::GetGlobalCommandList()->CmdList->DrawIndexedInstanced(drawcall.IndexCount, 1, drawcall.IndexStart, drawcall.VertexBase, 0);
			AbstractGfxLayer::DrawIndexedInstanced(AbstractGfxLayer::GetGlobalCommandList(), drawcall.IndexCount, 1, drawcall.IndexStart, drawcall.VertexBase, 0);
		}
	}
}

void Corona::GBufferPass()
{
	ColorBufferWriteIndex = 1 - ColorBufferWriteIndex;
	//DepthBufferWriteIndex = 1 - DepthBufferWriteIndex;
#if USE_AFTERMATH
	NVAftermathMarker(dx12_rhi->AM_CL_Handle, "GBufferPass");
#endif
	ProfileGPUScope(AbstractGfxLayer::GetGlobalCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "GBufferPass");

	{
		std::array<ResourceTransition, 7> Transition = { {
		{AlbedoBuffer.get(), RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_RENDER_TARGET},
		{NormalBuffers[ColorBufferWriteIndex].get(), RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_RENDER_TARGET},
		{GeomNormalBuffer.get(), RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_RENDER_TARGET},
		{VelocityBuffer.get(), RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_RENDER_TARGET},
		{RoughnessMetalicBuffer.get(), RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_RENDER_TARGET},
		{DepthBuffer.get(), RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_DEPTH_WRITE},
		{UnjitteredDepthBuffers[ColorBufferWriteIndex].get(), RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_RENDER_TARGET}
		} };
		AbstractGfxLayer::TransitionResource(AbstractGfxLayer::GetGlobalCommandList(), Transition.size(), Transition.data());
	}

	float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
	AbstractGfxLayer::ClearRenderTarget(AbstractGfxLayer::GetGlobalCommandList(), AlbedoBuffer.get(), clearColor, 0, nullptr);

	float normalClearColor[] = { 0.0f, -0.1f, 0.0f, 0.0f };
	AbstractGfxLayer::ClearRenderTarget(AbstractGfxLayer::GetGlobalCommandList(), NormalBuffers[ColorBufferWriteIndex].get(), normalClearColor, 0, nullptr);


	AbstractGfxLayer::ClearRenderTarget(AbstractGfxLayer::GetGlobalCommandList(), GeomNormalBuffer.get(), normalClearColor, 0, nullptr);

	float velocityClearColor[] = { 0.0f, 0.0f, 0.0f, 0.0f};
	AbstractGfxLayer::ClearRenderTarget(AbstractGfxLayer::GetGlobalCommandList(), VelocityBuffer.get(), velocityClearColor, 0, nullptr);

	float roughnessClearColor[] = { 0.001f, 0.0f, 0.0f, 0.0f };
	AbstractGfxLayer::ClearRenderTarget(AbstractGfxLayer::GetGlobalCommandList(), RoughnessMetalicBuffer.get(), roughnessClearColor, 0, nullptr);

	float ujitteredDepthClearColor[] = { 1.0f, 1.0f, 1.0f, 1.0f};
	AbstractGfxLayer::ClearRenderTarget(AbstractGfxLayer::GetGlobalCommandList(), UnjitteredDepthBuffers[ColorBufferWriteIndex].get(), ujitteredDepthClearColor, 0, nullptr);

	AbstractGfxLayer::ClearDepthStencil(AbstractGfxLayer::GetGlobalCommandList(), DepthBuffer.get(), CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

	AbstractGfxLayer::SetDescriptorHeap(AbstractGfxLayer::GetGlobalCommandList());

	ViewPort viewPort = { 0.0f, 0.0f, static_cast<float>(RenderWidth), static_cast<float>(RenderHeight), 0, 1};
	AbstractGfxLayer::SetViewports(AbstractGfxLayer::GetGlobalCommandList(), 1, &viewPort);

	AbstractGfxLayer::SetScissorRects(AbstractGfxLayer::GetGlobalCommandList(), 1, &m_scissorRect);
	AbstractGfxLayer::SetPrimitiveTopology(AbstractGfxLayer::GetGlobalCommandList(), PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	
	std::vector<GfxTexture*> Rendertarget = { AlbedoBuffer.get(), NormalBuffers[ColorBufferWriteIndex].get(),
		GeomNormalBuffer.get(), VelocityBuffer.get(), RoughnessMetalicBuffer.get(),
		UnjitteredDepthBuffers[ColorBufferWriteIndex].get()};
	AbstractGfxLayer::SetRenderTargets(AbstractGfxLayer::GetGlobalCommandList(), Rendertarget.size(), Rendertarget.data(), DepthBuffer.get());

	AbstractGfxLayer::SetPSO(GBufferPassPSO.get(), AbstractGfxLayer::GetGlobalCommandList());

	AbstractGfxLayer::SetSampler("samplerWrap", AbstractGfxLayer::GetGlobalCommandList(), GBufferPassPSO.get(), samplerAnisoWrap.get());

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
	
	{
		std::array<ResourceTransition, 7> Transition = { {
		{AlbedoBuffer.get(), RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE},
		{NormalBuffers[ColorBufferWriteIndex].get(), RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE},
		{GeomNormalBuffer.get(), RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE},
		{VelocityBuffer.get(), RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE},
		{RoughnessMetalicBuffer.get(), RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE},
		{DepthBuffer.get(), RESOURCE_STATE_DEPTH_WRITE, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE},
		{UnjitteredDepthBuffers[ColorBufferWriteIndex].get(), RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE},
		} };
		AbstractGfxLayer::TransitionResource(AbstractGfxLayer::GetGlobalCommandList(), Transition.size(), Transition.data());
	}
}

void Corona::SpatialDenoisingPass()
{
#if USE_AFTERMATH
	NVAftermathMarker(dx12_rhi->AM_CL_Handle, "SpatialDenoisingPass");
#endif
	ProfileGPUScope(AbstractGfxLayer::GetGlobalCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "SpatialDenoisingPass");

	UINT WriteIndex = 0;
	UINT ReadIndex = 1;
	for (int i = 0; i < 4; i++)
	{
		WriteIndex = 1 - WriteIndex; // 1
		ReadIndex = 1 - WriteIndex; // 0

		{
			std::array<ResourceTransition, 2> Transition = { {
				{DiffuseGISHSpatial[WriteIndex].get(), RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_UNORDERED_ACCESS},
				{DiffuseGICoCgSpatial[WriteIndex].get(), RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_UNORDERED_ACCESS}
			} };
			AbstractGfxLayer::TransitionResource(AbstractGfxLayer::GetGlobalCommandList(), Transition.size(), Transition.data());
		}

		AbstractGfxLayer::SetPSO(SpatialDenoisingFilterPSO.get(), AbstractGfxLayer::GetGlobalCommandList());

		AbstractGfxLayer::SetReadTexture(SpatialDenoisingFilterPSO.get(), "DepthTex", DepthBuffer.get(), AbstractGfxLayer::GetGlobalCommandList());
		AbstractGfxLayer::SetReadTexture(SpatialDenoisingFilterPSO.get(), "GeoNormalTex", GeomNormalBuffer.get(), AbstractGfxLayer::GetGlobalCommandList());
		AbstractGfxLayer::SetReadTexture(SpatialDenoisingFilterPSO.get(), "InGIResultSHTex", DiffuseGISHSpatial[ReadIndex].get(), AbstractGfxLayer::GetGlobalCommandList());
		AbstractGfxLayer::SetReadTexture(SpatialDenoisingFilterPSO.get(), "InGIResultColorTex", DiffuseGICoCgSpatial[ReadIndex].get(), AbstractGfxLayer::GetGlobalCommandList());


		AbstractGfxLayer::SetWriteTexture(SpatialDenoisingFilterPSO.get(), "OutGIResultSH", DiffuseGISHSpatial[WriteIndex].get(), AbstractGfxLayer::GetGlobalCommandList());
		AbstractGfxLayer::SetWriteTexture(SpatialDenoisingFilterPSO.get(), "OutGIResultColor", DiffuseGICoCgSpatial[WriteIndex].get(), AbstractGfxLayer::GetGlobalCommandList());

		SpatialFilterCB.Iteration = i;
		AbstractGfxLayer::SetUniformValue(SpatialDenoisingFilterPSO.get(), "SpatialFilterConstant", &SpatialFilterCB, AbstractGfxLayer::GetGlobalCommandList());

		UINT WidthGI = RenderWidth / GIBufferScale;
		UINT HeightGI = RenderHeight / GIBufferScale;

		AbstractGfxLayer::Dispatch(AbstractGfxLayer::GetGlobalCommandList(), WidthGI / 32, HeightGI / 32 + 1, 1);

		{
			std::array<ResourceTransition, 2> Transition = { {
				{DiffuseGISHSpatial[WriteIndex].get(), RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE},
				{DiffuseGICoCgSpatial[WriteIndex].get(), RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE}
			} };
			AbstractGfxLayer::TransitionResource(AbstractGfxLayer::GetGlobalCommandList(), Transition.size(), Transition.data());
		}
	}
}

void Corona::TemporalDenoisingPass()
{
#if USE_AFTERMATH
	NVAftermathMarker(dx12_rhi->AM_CL_Handle, "TemporalDenoisingPass");
#endif
	ProfileGPUScope(AbstractGfxLayer::GetGlobalCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "TemporalDenoisingPass");

	// GIBufferSH : full scale
	// FilterIndirectDiffusePingPongSH : 3x3 downsample
	GIBufferWriteIndex = 1 - GIBufferWriteIndex;
	UINT WriteIndex = GIBufferWriteIndex;
	UINT ReadIndex = 1 - WriteIndex;

	// first pass
	{
		std::array<ResourceTransition, 5> Transition = { {
		{DiffuseGISHSpatial[0].get(), RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_UNORDERED_ACCESS},
		{DiffuseGICoCgSpatial[0].get(), RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_UNORDERED_ACCESS},
		{DiffuseGISHTemporal[WriteIndex].get(), RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_UNORDERED_ACCESS},
		{DiffuseGICoCgTemporal[WriteIndex].get(), RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_UNORDERED_ACCESS},
		{SpeculaGIBufferTemporal[WriteIndex].get(), RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_UNORDERED_ACCESS}
		} };
		AbstractGfxLayer::TransitionResource(AbstractGfxLayer::GetGlobalCommandList(), Transition.size(), Transition.data());
	}

	AbstractGfxLayer::SetPSO(TemporalDenoisingFilterPSO.get(), AbstractGfxLayer::GetGlobalCommandList());

	AbstractGfxLayer::SetReadTexture(TemporalDenoisingFilterPSO.get(), "DepthTex", UnjitteredDepthBuffers[ColorBufferWriteIndex].get(), AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetReadTexture(TemporalDenoisingFilterPSO.get(), "NormalTex", NormalBuffers[ColorBufferWriteIndex].get(), AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetReadTexture(TemporalDenoisingFilterPSO.get(), "InGIResultSHTex", DiffuseGISHRaw.get(), AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetReadTexture(TemporalDenoisingFilterPSO.get(), "InGIResultColorTex", DiffuseGICoCgRaw.get(), AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetReadTexture(TemporalDenoisingFilterPSO.get(), "InGIResultSHTexPrev", DiffuseGISHTemporal[ReadIndex].get(), AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetReadTexture(TemporalDenoisingFilterPSO.get(), "InGIResultColorTexPrev", DiffuseGICoCgTemporal[ReadIndex].get(), AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetReadTexture(TemporalDenoisingFilterPSO.get(), "VelocityTex", VelocityBuffer.get(), AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetReadTexture(TemporalDenoisingFilterPSO.get(), "InSpecularGITex", SpeculaGIBufferRaw.get(), AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetReadTexture(TemporalDenoisingFilterPSO.get(), "InSpecularGITexPrev", SpeculaGIBufferTemporal[ReadIndex].get(), AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetReadTexture(TemporalDenoisingFilterPSO.get(), "RougnessMetalicTex", RoughnessMetalicBuffer.get(), AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetReadTexture(TemporalDenoisingFilterPSO.get(), "PrevDepthTex", UnjitteredDepthBuffers[1 - ColorBufferWriteIndex].get(), AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetReadTexture(TemporalDenoisingFilterPSO.get(), "PrevNormalTex", NormalBuffers[1 - ColorBufferWriteIndex].get(), AbstractGfxLayer::GetGlobalCommandList());


	AbstractGfxLayer::SetWriteTexture(TemporalDenoisingFilterPSO.get(), "OutGIResultSH", DiffuseGISHTemporal[WriteIndex].get(), AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetWriteTexture(TemporalDenoisingFilterPSO.get(), "OutGIResultColor", DiffuseGICoCgTemporal[WriteIndex].get(), AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetWriteTexture(TemporalDenoisingFilterPSO.get(), "OutGIResultSHDS", DiffuseGISHSpatial[0].get(), AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetWriteTexture(TemporalDenoisingFilterPSO.get(), "OutGIResultColorDS", DiffuseGICoCgSpatial[0].get(), AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetWriteTexture(TemporalDenoisingFilterPSO.get(), "OutSpecularGI", SpeculaGIBufferTemporal[WriteIndex].get(), AbstractGfxLayer::GetGlobalCommandList());

	AbstractGfxLayer::SetSampler("BilinearClamp", AbstractGfxLayer::GetGlobalCommandList(), TemporalDenoisingFilterPSO.get(), samplerBilinearWrap.get());


	AbstractGfxLayer::SetUniformValue(TemporalDenoisingFilterPSO.get(), "TemporalFilterConstant", &TemporalFilterCB, AbstractGfxLayer::GetGlobalCommandList());

	AbstractGfxLayer::Dispatch(AbstractGfxLayer::GetGlobalCommandList(), RenderWidth / 15, RenderHeight / 15, 1);

	{
		std::array<ResourceTransition, 5> Transition = { {
			{DiffuseGISHSpatial[0].get(), RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE},
			{DiffuseGICoCgSpatial[0].get(), RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE},
			{DiffuseGISHTemporal[WriteIndex].get(), RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE},
			{DiffuseGICoCgTemporal[WriteIndex].get(), RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE},
			{SpeculaGIBufferTemporal[WriteIndex].get(), RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE},
		} };
		AbstractGfxLayer::TransitionResource(AbstractGfxLayer::GetGlobalCommandList(), Transition.size(), Transition.data());
	}
}

#if USE_NRD
void Corona::NRDPass()
{
	ProfileGPUScope(AbstractGfxLayer::GetGlobalCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "NRDPass");

	// resolve normal-roughness 
	{
		std::array<ResourceTransition, 2> Transition = { {
			{NormalRoughness_NRD.get(), RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_UNORDERED_ACCESS},
			{LinearDepth_NRD.get(), RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_UNORDERED_ACCESS}
		} };
		AbstractGfxLayer::TransitionResource(AbstractGfxLayer::GetGlobalCommandList(), Transition.size(), Transition.data());
	}

	AbstractGfxLayer::SetPSO(ResolveNormalRoughnessPSO.get(), AbstractGfxLayer::GetGlobalCommandList());

	AbstractGfxLayer::SetReadTexture(ResolveNormalRoughnessPSO.get(), "Normal", NormalBuffers[ColorBufferWriteIndex].get(), AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetReadTexture(ResolveNormalRoughnessPSO.get(), "RoughnessMetallic", RoughnessMetalicBuffer.get(), AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetReadTexture(ResolveNormalRoughnessPSO.get(), "Depth", DepthBuffer.get(), AbstractGfxLayer::GetGlobalCommandList());

	AbstractGfxLayer::SetWriteTexture(ResolveNormalRoughnessPSO.get(), "NormalRoughness", NormalRoughness_NRD.get(), AbstractGfxLayer::GetGlobalCommandList());
	AbstractGfxLayer::SetWriteTexture(ResolveNormalRoughnessPSO.get(), "LinearDepth", LinearDepth_NRD.get(), AbstractGfxLayer::GetGlobalCommandList());

	ResolveNRDParam param;
	param.InvProjMatrix = glm::transpose(InvProjMat);
	param.Near = Near;
	param.Far = Far;

	AbstractGfxLayer::SetUniformValue(ResolveNormalRoughnessPSO.get(), "ResolveNRDParam", &param, AbstractGfxLayer::GetGlobalCommandList());

	AbstractGfxLayer::Dispatch(AbstractGfxLayer::GetGlobalCommandList(), RenderWidth / 32, RenderHeight / 32 + 1, 1);

	{
		std::array<ResourceTransition, 2> Transition = { {
			{NormalRoughness_NRD.get(), RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE},
			{LinearDepth_NRD.get(), RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE},
		} };
		AbstractGfxLayer::TransitionResource(AbstractGfxLayer::GetGlobalCommandList(), Transition.size(), Transition.data());
	}

	//Wrap the command buffer
	nri::CommandBufferD3D12Desc cmdDesc;
	cmdDesc.d3d12CommandList = (ID3D12GraphicsCommandList*)AbstractGfxLayer::GetGlobalCommandList()->CmdList.Get();
	cmdDesc.d3d12CommandAllocator = nullptr; // safe in this implementation.

	nri::CommandBuffer* cmdBuffer = nullptr;
	m_NRI.CreateCommandBufferD3D12(*m_NRIDevice, cmdDesc, cmdBuffer);

	//Wrap the textures
	nri::Texture* motion = { nullptr };
	nri::Texture* normal_roughness = { nullptr };
	nri::Texture* viewZ = { nullptr };
	//nri::Texture* unfiltered_shadow = { nullptr };
	//nri::Texture* unfiltered_transmittance = { nullptr };
	nri::Texture* unfiltered_DiffA = { nullptr };
	nri::Texture* unfiltered_DiffB = { nullptr };
	nri::Texture* unfiltered_SpecHit = { nullptr };
	//nri::Texture* transmittance = { nullptr };
	nri::Texture* diffHit = { nullptr };
	nri::Texture* specular = { nullptr };

	nri::TextureD3D12Desc nriTextureDesc = {};
	nriTextureDesc.d3d12Resource = LinearDepth_NRD->resource.Get();
	m_NRI.CreateTextureD3D12(*m_NRIDevice, nriTextureDesc, viewZ);

	nriTextureDesc.d3d12Resource = VelocityBuffer->resource.Get();
	m_NRI.CreateTextureD3D12(*m_NRIDevice, nriTextureDesc, motion);

	nriTextureDesc.d3d12Resource = NormalRoughness_NRD->resource.Get();
	m_NRI.CreateTextureD3D12(*m_NRIDevice, nriTextureDesc, normal_roughness);

	nriTextureDesc.d3d12Resource = DiffuseGISHRaw->resource.Get();
	m_NRI.CreateTextureD3D12(*m_NRIDevice, nriTextureDesc, unfiltered_DiffA);

	nriTextureDesc.d3d12Resource = DiffuseGICoCgRaw->resource.Get();
	m_NRI.CreateTextureD3D12(*m_NRIDevice, nriTextureDesc, unfiltered_DiffB);

	nriTextureDesc.d3d12Resource = SpeculaGIBufferRaw->resource.Get();
	m_NRI.CreateTextureD3D12(*m_NRIDevice, nriTextureDesc, unfiltered_SpecHit);

	nriTextureDesc.d3d12Resource = DiffuseGI_NRD->resource.Get();
	m_NRI.CreateTextureD3D12(*m_NRIDevice, nriTextureDesc, diffHit);

	nriTextureDesc.d3d12Resource = SpecularGI_NRD->resource.Get();
	m_NRI.CreateTextureD3D12(*m_NRIDevice, nriTextureDesc, specular);

	nri::TextureTransitionBarrierDesc texBarriers[] =
	{
		// nextAccess and nextLayout values should represent current resource state. 
		// If states required by NRD do not match the state of the incoming texture, NRD will resolve
		// barriers internally. In minceraft there is a resource state tracker, 
		// thus transitions are done via engine and all the resources in this example already come in the
		// required state. 
		{ motion, 0, 0, 0, 0,                   nri::AccessBits::UNKNOWN, nri::AccessBits::SHADER_RESOURCE, nri::TextureLayout::UNKNOWN, nri::TextureLayout::SHADER_RESOURCE },
		{ normal_roughness, 0, 0, 0, 0,         nri::AccessBits::UNKNOWN, nri::AccessBits::SHADER_RESOURCE, nri::TextureLayout::UNKNOWN, nri::TextureLayout::SHADER_RESOURCE },
		{ viewZ, 0, 0, 0, 0,                    nri::AccessBits::UNKNOWN, nri::AccessBits::SHADER_RESOURCE, nri::TextureLayout::UNKNOWN, nri::TextureLayout::SHADER_RESOURCE },
		{ nullptr, 0, 0, 0, 0,        nri::AccessBits::UNKNOWN, nri::AccessBits::SHADER_RESOURCE, nri::TextureLayout::UNKNOWN, nri::TextureLayout::SHADER_RESOURCE },
		{ unfiltered_DiffA, 0, 0, 0, 0,         nri::AccessBits::UNKNOWN, nri::AccessBits::SHADER_RESOURCE, nri::TextureLayout::UNKNOWN, nri::TextureLayout::SHADER_RESOURCE },
		{ unfiltered_DiffB, 0, 0, 0, 0,         nri::AccessBits::UNKNOWN, nri::AccessBits::SHADER_RESOURCE, nri::TextureLayout::UNKNOWN, nri::TextureLayout::SHADER_RESOURCE },
		{ unfiltered_SpecHit, 0, 0, 0, 0,       nri::AccessBits::UNKNOWN, nri::AccessBits::SHADER_RESOURCE, nri::TextureLayout::UNKNOWN, nri::TextureLayout::SHADER_RESOURCE },
		{ nullptr, 0, 0, 0, 0, nri::AccessBits::UNKNOWN, nri::AccessBits::SHADER_RESOURCE, nri::TextureLayout::UNKNOWN, nri::TextureLayout::SHADER_RESOURCE },
		{ nullptr, 0, 0, 0, 0,            nri::AccessBits::UNKNOWN, nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::TextureLayout::UNKNOWN, nri::TextureLayout::GENERAL },
		{ diffHit, 0, 0, 0, 0,                  nri::AccessBits::UNKNOWN, nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::TextureLayout::UNKNOWN, nri::TextureLayout::GENERAL },
		{ specular, 0, 0, 0, 0,                 nri::AccessBits::UNKNOWN, nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::TextureLayout::UNKNOWN, nri::TextureLayout::GENERAL },
	};

	NrdUserPool userPool =
	{ {
			// IN_MV
			{motion, &texBarriers[0], nri::Format::RGBA16_SFLOAT},

			// IN_NORMAL_ROUGHNESS
			{normal_roughness, &texBarriers[1], nri::Format::RGBA16_UNORM},

			// IN_VIEWZ
			{viewZ, &texBarriers[2], nri::Format::R32_SFLOAT},

			// IN_SHADOW
			{nullptr, nullptr, nri::Format::RG16_SFLOAT},

			// IN_DIFFA
			{unfiltered_DiffA, &texBarriers[4], nri::Format::RGBA16_SFLOAT},

			// IN_DIFFB
			{unfiltered_DiffB, &texBarriers[5], nri::Format::RGBA16_SFLOAT},

			// IN_SPEC_HIT
			{unfiltered_SpecHit, &texBarriers[6], nri::Format::RGBA16_SFLOAT},

			// IN_TRANSLUCENCY
			{nullptr, nullptr, nri::Format::RGBA8_UNORM},

			// OUT_SHADOW
			{nullptr, nullptr, nri::Format::UNKNOWN},
			//when using Translusent Shadows, OUT_SHADOWS texture is not required. and vice versa

			// OUT_TRANSMITTANCE
			{nullptr, nullptr, nri::Format::RGBA8_UNORM},

			// OUT_DIFF_HIT
			{diffHit, &texBarriers[9], nri::Format::RGBA16_SFLOAT},

			// OUT_SPEC_HIT
			{specular, &texBarriers[10], nri::Format::RGBA16_SFLOAT},
		} };

	float threshold0 = 0.08 * 5000 * 0.006* 0.01f;
	float b = Smoothstep(-0.9f, 0.05f, LightDir.z);
	threshold0 += 0.1f * b * b;
	threshold0 *= 1920.0f / RenderWidth; // Additionally it depends on the resolution (higher resolution = better samples)

	float resetHistoryFactor = 1.0f;
	float diffMaxAccumulatedFrameNum = 31;
	float specMaxAccumulatedFrameNum = 31;
	float disocclusionThreshold = 0.5;
	float specDenoisingRadius = 40;
	float specAdaptiveRadiusScale = 0.5;
	bool specularAnisotropicFiltering = true;

	nrd::AntilagSettings antilagSettings = {};
	antilagSettings.enable = true;
	antilagSettings.intensityThresholdMin = threshold0;
	antilagSettings.intensityThresholdMax = 3.0f * threshold0;


	nrd::NrdDiffuseSettings diffuseSettings = {};
	diffuseSettings.hitDistanceParameters = { 3.0f, 0.1f, 0.0f, 0.0f }; // see HIT_DISTANCE_LINEAR_SCALE
	diffuseSettings.antilagSettings = antilagSettings;
	diffuseSettings.maxAccumulatedFrameNum = uint32_t(31* resetHistoryFactor + 0.5f);
	diffuseSettings.disocclusionThreshold = disocclusionThreshold * 0.01f;
	diffuseSettings.denoisingRadius = 30;
	diffuseSettings.postBlurMaxAdaptiveRadiusScale = 5.0f;
	diffuseSettings.checkerboardMode = nrd::CheckerboardMode::OFF;
	m_NRD.SetMethodSettings(nrd::Method::NRD_DIFFUSE, &diffuseSettings);

	const vec3 trimmingParams = vec3(0.85, 0.04, 0.1);//GetTrimmingParams();

	nrd::NrdSpecularSettings specularSettings = {};
	specularSettings.hitDistanceParameters = { 7.0, 0.1f, 0.0f, 0.0f }; // see HIT_DISTANCE_LINEAR_SCALE
	specularSettings.lobeTrimmingParameters = { trimmingParams.x, trimmingParams.y, trimmingParams.z };
	specularSettings.antilagSettings = antilagSettings;
	specularSettings.maxAccumulatedFrameNum = uint32_t(specMaxAccumulatedFrameNum * resetHistoryFactor + 0.5f);
	specularSettings.disocclusionThreshold = disocclusionThreshold * 0.01f;
	specularSettings.denoisingRadius = specDenoisingRadius;
	specularSettings.postBlurRadiusScale = specAdaptiveRadiusScale;
	specularSettings.anisotropicFiltering = specularAnisotropicFiltering;
	specularSettings.checkerboardMode = nrd::CheckerboardMode::OFF;
	m_NRD.SetMethodSettings(nrd::Method::NRD_SPECULAR, &specularSettings);

	vec2 jitter = JitterOffset;
	//vec2 jitterPrev = m_Settings.temporal ? m_Camera.m_ViewportJitterPrev : 0.0f;

	nrd::CommonSettings commonSettings = {};
	memcpy(commonSettings.worldToViewMatrix, &glm::transpose(InvViewMat), sizeof(glm::mat4x4));
	memcpy(commonSettings.worldToViewMatrixPrev, &glm::transpose(InvViewMat), sizeof(glm::mat4x4));

	memcpy(commonSettings.viewToClipMatrix, &glm::transpose(ProjMat), sizeof(glm::mat4x4));
	memcpy(commonSettings.viewToClipMatrixPrev, &glm::transpose(PrevProjMat), sizeof(glm::mat4x4));

	commonSettings.metersToUnitsMultiplier = 1000;// / m_Settings.unitsToMetersMultiplier;
	//commonSettings.denoisingRange = m_Scene.aabb.GetRadius() * 4.0f;
	commonSettings.denoisingRange =  Sponza->BoundingRadius * 4;
	commonSettings.xMotionVectorScale = -1;// 1.0f / RenderWidth;
	commonSettings.yzMotionVectorScale = -1;// 1.0f / RenderHeight;
	commonSettings.xJitter = jitter.x;
	commonSettings.yJitter = jitter.y;
	commonSettings.debug = false;
	commonSettings.frameIndex = FrameCounter;
	commonSettings.worldSpaceMotion = false;
	commonSettings.forceReferenceAccumulation = false;

	m_NRD.Denoise(*cmdBuffer, commonSettings, userPool);


	m_NRI.DestroyCommandBuffer(*cmdBuffer);
	m_NRI.DestroyTexture(*viewZ);
	m_NRI.DestroyTexture(*motion);
	m_NRI.DestroyTexture(*normal_roughness);

	m_NRI.DestroyTexture(*unfiltered_DiffA);
	m_NRI.DestroyTexture(*unfiltered_DiffB);
	m_NRI.DestroyTexture(*unfiltered_SpecHit);

	m_NRI.DestroyTexture(*diffHit);
	m_NRI.DestroyTexture(*specular);

	ID3D12DescriptorHeap* ppHeaps[] = { dx12_rhi->SRVCBVDescriptorHeapShaderVisible->DH.Get(), dx12_rhi->SamplerDescriptorHeapShaderVisible->DH.Get() };
	AbstractGfxLayer::GetGlobalCommandList()->CmdList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
}
#endif USE_NRD


void AddMeshToVec(vector<shared_ptr<GfxRTAS>>& vecBLAS, shared_ptr<Scene> scene)
{
	for (auto& mesh : scene->meshes)
	{
		shared_ptr<GfxRTAS> blas = shared_ptr<GfxRTAS>(AbstractGfxLayer::CreateBLAS(mesh.get()));
		if (blas == nullptr)
		{
			continue;
		}
		vecBLAS.push_back(blas);
	}
}

void Corona::RecompileShaders()
{
	AbstractGfxLayer::WaitGPUFlush();

	InitRTPSO();

#if USE_RTXGI
	InitRTXGI();
#endif

	InitSpatialDenoisingPass();
	InitTemporalDenoisingPass();
	InitGBufferPass();
	InitToneMapPass();
	InitDebugPass();
	InitLightingPass();
	InitTemporalAAPass();
	InitBloomPass();
}

void Corona::InitRaytracingData()
{
	UINT NumTotalMesh = Sponza->meshes.size() + ShaderBall->meshes.size();
	vecBLAS.reserve(NumTotalMesh);

	AddMeshToVec(vecBLAS, Sponza);
	AddMeshToVec(vecBLAS, ShaderBall);

	TLAS = shared_ptr<GfxRTAS>(AbstractGfxLayer::CreateTLAS(vecBLAS));

	InstancePropertyBuffer = shared_ptr<GfxBuffer>(AbstractGfxLayer::CreateByteAddressBuffer(500, sizeof(InstanceProperty), HEAP_TYPE_UPLOAD, RESOURCE_STATE_GENERIC_READ, RESOURCE_FLAG_NONE));
	NAME_BUFFER(InstancePropertyBuffer);


	uint8_t* pData;
	AbstractGfxLayer::MapBuffer(InstancePropertyBuffer.get(), (void**)&pData);

	for (auto& m : vecBLAS)
	{
		glm::mat4x4 mat = glm::transpose(m->mesh->transform);
		memcpy(pData, &mat, sizeof(glm::mat4x4));
		pData += sizeof(InstanceProperty);
	}

	AbstractGfxLayer::UnmapBuffer(InstancePropertyBuffer.get());

}

#if USE_DLSS
void Corona::InitDLSS()
{
	NVSDK_NGX_Result Result = NVSDK_NGX_Result_Fail;

	Result = NVSDK_NGX_D3D12_Init(DLSS_APP_ID, L".", dx12_rhi->Device.Get());

	m_ngxInitialized = !NVSDK_NGX_FAILED(Result);
	if (!m_ngxInitialized)
	{
		if (Result == NVSDK_NGX_Result_FAIL_FeatureNotSupported || Result == NVSDK_NGX_Result_FAIL_PlatformError)
		{
			std::stringstream strStream;
			strStream << "NVIDIA NGX not available on this hardware/platform., code = " << Result << "info : " << GetNGXResultAsString(Result);
			dx12_rhi->errorString += strStream.str();
		}
		else
		{
			std::stringstream strStream;
			strStream << "Failed to initialize NGX, error code =" << Result << "info: " << GetNGXResultAsString(Result);
			dx12_rhi->errorString += strStream.str();

		}
		return;
	}

	Result = NVSDK_NGX_D3D12_GetParameters(&m_ngxParameters);

	if (NVSDK_NGX_FAILED(Result))
	{
		std::stringstream strStream;
		strStream << "NVSDK_NGX_D3D12_GetParameters failed, code ="<< Result <<  "info: " <<  GetNGXResultAsString(Result);
		dx12_rhi->errorString += strStream.str();

		ShutDownDLSS();
	}

	// Currently, the SDK and this sample are not in sync.  The sample is a bit forward looking,
// in this case.  This will likely be resolved very shortly, and therefore, the code below
// should be thought of as needed for a smooth user experience.
#if defined(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver)        \
    && defined (NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor) \
    && defined (NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor)

	// If NGX Successfully initialized then it should set those flags in return
	int needsUpdatedDriver = 0;
	unsigned int minDriverVersionMajor = 0;
	unsigned int minDriverVersionMinor = 0;
	NVSDK_NGX_Result ResultUpdatedDriver = m_ngxParameters->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsUpdatedDriver);
	NVSDK_NGX_Result ResultMinDriverVersionMajor = m_ngxParameters->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &minDriverVersionMajor);
	NVSDK_NGX_Result ResultMinDriverVersionMinor = m_ngxParameters->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minDriverVersionMinor);
	if (ResultUpdatedDriver == NVSDK_NGX_Result_Success &&
		ResultMinDriverVersionMajor == NVSDK_NGX_Result_Success &&
		ResultMinDriverVersionMinor == NVSDK_NGX_Result_Success)
	{
		if (needsUpdatedDriver)
		{
			std::stringstream strStream;
			strStream << "NVIDIA DLSS cannot be loaded due to outdated driver. Minimum Driver Version required :" << minDriverVersionMajor << "." << minDriverVersionMinor;
			dx12_rhi->errorString += strStream.str();

			ShutDownDLSS();
			return;
		}
		else
		{
			/*std::stringstream strStream;
			strStream << "NVIDIA DLSS Minimum driver version was reported as :" << minDriverVersionMajor << "." << minDriverVersionMinor;
			dx12_rhi->errorString += strStream.str();*/

		}
	}
	else
	{
		dx12_rhi->errorString += string("NVIDIA DLSS Minimum driver version was not reported.");
	}

#endif

	int dlssAvailable = 0;
	NVSDK_NGX_Result ResultDLSS = m_ngxParameters->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &dlssAvailable);
	if (ResultDLSS != NVSDK_NGX_Result_Success || !dlssAvailable)
	{
		std::stringstream strStream;
		strStream << "NVIDIA DLSS not available on this hardward/platform., result = " << Result << "info: " << GetNGXResultAsString(Result);
		dx12_rhi->errorString += strStream.str();

		ShutDownDLSS();
		return;
	}

	{
		NVSDK_NGX_PerfQuality_Value qualValue = NVSDK_NGX_PerfQuality_Value_MaxQuality;
		float depthScale = 1.0f;
		unsigned int CreationNodeMask = 1;
		unsigned int VisibilityNodeMask = 1;
		NVSDK_NGX_RTX_Value RTX = NVSDK_NGX_RTX_Value_Off;
		NVSDK_NGX_Result ResultDLSS = NVSDK_NGX_Result_Fail;

		int MotionVectorResolutionLow = 1; // we let the Snippet do the upsampling of the motion vector
		// Next create features	
		int DlssCreateFeatureFlags = NVSDK_NGX_DLSS_Feature_Flags_None;
		DlssCreateFeatureFlags |= MotionVectorResolutionLow ? NVSDK_NGX_DLSS_Feature_Flags_MVLowRes : 0;
		DlssCreateFeatureFlags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
		//DlssCreateFeatureFlags |= NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
		DlssCreateFeatureFlags |= NVSDK_NGX_DLSS_Feature_Flags_DepthJittered;
		DlssCreateFeatureFlags |= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;


		
		//DlssCreateFeatureFlags |= depthInverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0;

		NVSDK_NGX_DLSS_Create_Params DlssCreateParams;

		CommandList* cmd = dx12_rhi->CmdQ->AllocCmdList();

		DXGI_FORMAT targetFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
		ID3D12GraphicsCommandList* d3d12CommandList = cmd->CmdList.Get();


		ResultDLSS = NGX_D3D12_CREATE_DLSS(&m_dlssFeature, m_ngxParameters, d3d12CommandList,
			RenderWidth, RenderHeight, DisplayWidth, DisplayHeight,
			depthScale, qualValue, DlssCreateFeatureFlags, CreationNodeMask, VisibilityNodeMask);


		if (NVSDK_NGX_FAILED(ResultDLSS))
		{
			std::stringstream strStream;
			strStream << "Failed to create DLSS Features = " << ResultDLSS << "info: " << GetNGXResultAsString(ResultDLSS);
			dx12_rhi->errorString += strStream.str();
		}


		dx12_rhi->CmdQ->ExecuteCommandList(cmd);
		dx12_rhi->CmdQ->WaitGPU();
	}
}
#endif

#if USE_NRD
void Corona::InitNRD()
{
	//Wrap d3d12 device with NRI
	nri::DeviceCreationD3D12Desc deviceDesc = {};
	deviceDesc.d3d12Device = dx12_rhi->Device.Get();
	deviceDesc.d3d12PhysicalAdapter = dx12_rhi->m_hardwareAdapter.Get();
	deviceDesc.d3d12GraphicsQueue = dx12_rhi->CmdQ->CmdQueue.Get();
	deviceDesc.enableNRIValidation = false;
	uint32_t nriResult;

	nriResult = (uint32_t)nri::CreateDeviceFromD3D12Device(deviceDesc, m_NRIDevice);


	//Get NRI Interfaces. Core for main API functionality. WrapperD3D12Interface for further wrappings of D3D12 objects. In NRD case we
	//need only wrapped ID3D12CommandBuffer and ID3D12Resources. 
	nriResult |= (uint32_t)nri::GetInterface(*m_NRIDevice, NRI_INTERFACE(nri::WrapperD3D12Interface), (nri::WrapperD3D12Interface*)&m_NRI);
	nriResult |= (uint32_t)nri::GetInterface(*m_NRIDevice, NRI_INTERFACE(nri::CoreInterface), (nri::CoreInterface*)&m_NRI);
	if ((nri::Result)nriResult != nri::Result::SUCCESS)
		return;
	m_NRIInit = true;

	const nrd::MethodDesc methodDescs[] =
	{
		{ nrd::Method::NRD_DIFFUSE, (uint16_t)((float)RenderWidth), (uint16_t)RenderHeight},
		{ nrd::Method::NRD_SPECULAR, (uint16_t)((float)RenderWidth), (uint16_t)RenderHeight}

		/*{ nrd::Method::NRD_DIFFUSE, (uint16_t)((float)DisplayWidth), (uint16_t)DisplayHeight},
		{ nrd::Method::NRD_SPECULAR, (uint16_t)((float)DisplayWidth), (uint16_t)DisplayHeight}*/
	};

	nrd::DenoiserCreationDesc denoiserCreationDesc = {};
	denoiserCreationDesc.requestedMethods = methodDescs;
	denoiserCreationDesc.requestedMethodNum = _countof(methodDescs);

	m_NRDInit = m_NRD.Initialize(*m_NRIDevice, m_NRI, denoiserCreationDesc, false);

	// resolve normal roughness buffer pso
	{
		ComPtr<ID3DBlob> cs = dx12_rhi->CreateShaderDXC(GetAssetFullPath(L"Shaders\\ResolveNormalRoughnessCS.hlsl"), L"main", L"cs_6_0", nullopt);
		D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};

		shared_ptr<PipelineStateObject> TEMP_ResolveNormalRoughness = shared_ptr<PipelineStateObject>(new PipelineStateObject);
		TEMP_ResolveNormalRoughness->cs = cs;
		TEMP_ResolveNormalRoughness->computePSODesc = computePsoDesc;
		TEMP_ResolveNormalRoughness->BindSRV("Normal", 0, 1);
		TEMP_ResolveNormalRoughness->BindSRV("RoughnessMetallic", 1, 1);
		TEMP_ResolveNormalRoughness->BindSRV("Depth", 2, 1);

		TEMP_ResolveNormalRoughness->BindUAV("NormalRoughness", 0);
		TEMP_ResolveNormalRoughness->BindUAV("LinearDepth", 1);

		TEMP_ResolveNormalRoughness->BindCBV("ResolveNRDParam", 0, sizeof(ResolveNRDParam));


		TEMP_ResolveNormalRoughness->IsCompute = true;
		bool bSucess = TEMP_ResolveNormalRoughness->Init();
		if (bSucess)
			ResolveNormalRoughnessPSO = TEMP_ResolveNormalRoughness;
	}
}
#endif

#if USE_RTXGI
void Corona::InitRTXGI()
{
	vector<ComPtr<ID3DBlob>> shaders;

	// RTXGI irradiance blending
	{
		ComPtr<ID3DBlob> shaderByteCode;

		vector< DxcDefine> defines;
		defines.push_back({ L"RTXGI_DDGI_BLEND_RADIANCE", L"1" });
		defines.push_back({ L"RAYS_PER_PROBE", L"144" });
		defines.push_back({ L"PROBE_NUM_TEXELS", L"6" });
		defines.push_back({ L"PROBE_UAV_INDEX", L"0" });

		shaderByteCode = dx12_rhi->CreateShaderDXC(GetAssetFullPath(L"Shaders/rtxgi/ddgi/ProbeBlendingCS.hlsl"), L"DDGIProbeBlendingCS", L"cs_6_0", defines);
		shaders.push_back(shaderByteCode);
	}

	// RTXGI distance blending
	{
		ComPtr<ID3DBlob> shaderByteCode;

		vector< DxcDefine> defines;
		defines.push_back({ L"RTXGI_DDGI_BLEND_RADIANCE", L"0" });
		defines.push_back({ L"RAYS_PER_PROBE", L"144" });
		defines.push_back({ L"PROBE_NUM_TEXELS", L"6" });
		defines.push_back({ L"PROBE_UAV_INDEX", L"1" });


		shaderByteCode = dx12_rhi->CreateShaderDXC(GetAssetFullPath(L"Shaders/rtxgi/ddgi/ProbeBlendingCS.hlsl"), L"DDGIProbeBlendingCS", L"cs_6_0", defines);
		shaders.push_back(shaderByteCode);
	}

	// RTXGI border rows update
	{
		ComPtr<ID3DBlob> shaderByteCode;

		shaderByteCode = dx12_rhi->CreateShaderDXC(GetAssetFullPath(L"Shaders/rtxgi/ddgi/ProbeBorderUpdateCS.hlsl"), L"DDGIProbeBorderRowUpdateCS", L"cs_6_0", nullopt);
		shaders.push_back(shaderByteCode);
	}

	// RTXGI border columns update
	{
		ComPtr<ID3DBlob> shaderByteCode;

		shaderByteCode = dx12_rhi->CreateShaderDXC(GetAssetFullPath(L"Shaders/rtxgi/ddgi/ProbeBorderUpdateCS.hlsl"), L"DDGIProbeBorderColumnUpdateCS", L"cs_6_0", nullopt);
		shaders.push_back(shaderByteCode);
	}

	// RTXGI probe relocation
	{
		ComPtr<ID3DBlob> shaderByteCode;

		shaderByteCode = dx12_rhi->CreateShaderDXC(GetAssetFullPath(L"Shaders/rtxgi/ddgi/ProbeRelocationCS.hlsl"), L"DDGIProbeRelocationCS", L"cs_6_0", nullopt);
		shaders.push_back(shaderByteCode);
	}

	// RTXGI probe state classifier
	{
		ComPtr<ID3DBlob> shaderByteCode;

		shaderByteCode = dx12_rhi->CreateShaderDXC(GetAssetFullPath(L"Shaders/rtxgi/ddgi/ProbeStateClassifierCS.hlsl"), L"DDGIProbeStateClassifierCS", L"cs_6_0", nullopt);
		shaders.push_back(shaderByteCode);
	}

	// RTXGI probe state classifier, activate all probes
	{
		ComPtr<ID3DBlob> shaderByteCode;

		shaderByteCode = dx12_rhi->CreateShaderDXC(GetAssetFullPath(L"Shaders/rtxgi/ddgi/ProbeStateClassifierCS.hlsl"), L"DDGIProbeStateActivateAllCS", L"cs_6_0", nullopt);
		shaders.push_back(shaderByteCode);
	}


	// create Volume

	volumeDesc.origin = { 0.f, 0.f, 0.f };
	int mul = 1;
	volumeDesc.probeGridCounts = { 40* mul, 16 * mul, 20 * mul };

	volumeDesc.probeGridSpacing = { 100, 100, 100 };
	//volumeDesc.viewBias = 4.0f;
	//volumeDesc.normalBias = 1.0f;
	volumeDesc.probeMaxRayDistance = 10000.0f;
	volumeDesc.probeBrightnessThreshold = 2.00f;
	volumeDesc.probeChangeThreshold = 0.2;
	volumeDesc.numRaysPerProbe = 144;
	volumeDesc.numIrradianceTexels = 6;
	volumeDesc.numDistanceTexels = 14;
	//volumeDesc.probeHysteresis = 0.97;
	//volumeDesc.probeIrradianceEncodingGamma = 5;

	UINT size = rtxgi::GetDDGIVolumeConstantBufferSize() * 3;   // sized to triple buffer the data

	VolumeCB = dx12_rhi->CreateBuffer(size, sizeof(InstanceProperty), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);

	NAME_D3D12_OBJECT(VolumeCB->resource);


	UINT width = 0;
	UINT height = 0;

	// probe radiance
	rtxgi::GetDDGIVolumeTextureDimensions(volumeDesc, rtxgi::EDDGITextureType::RTRadiance, width, height);
	DXGI_FORMAT format = rtxgi::GetDDGIVolumeTextureFormat(rtxgi::EDDGITextureType::RTRadiance);
	probeRTRadiance = shared_ptr<Texture>(static_cast<Texture*>(AbstractGfxLayer::CreateTexture2D(static_cast<FORMAT>(format), 
		RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, 
		RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, width, height, 1)));
	NAME_TEXTURE(probeRTRadiance);

	volumeResources.probeRTRadiance = probeRTRadiance->resource.Get();

	
	// probe irradiance
	rtxgi::GetDDGIVolumeTextureDimensions(volumeDesc, rtxgi::EDDGITextureType::Irradiance, width, height);
	format = rtxgi::GetDDGIVolumeTextureFormat(rtxgi::EDDGITextureType::Irradiance);
	probeIrradiance = shared_ptr<Texture>(static_cast<Texture*>(AbstractGfxLayer::CreateTexture2D(static_cast<FORMAT>(format), 
		RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, 
		RESOURCE_STATE_PIXEL_SHADER_RESOURCE, width, height, 1)));
	NAME_TEXTURE(probeIrradiance);

	volumeResources.probeIrradiance = probeIrradiance->resource.Get();

	// probe distance
	rtxgi::GetDDGIVolumeTextureDimensions(volumeDesc, rtxgi::EDDGITextureType::Distance, width, height);
	format = rtxgi::GetDDGIVolumeTextureFormat(rtxgi::EDDGITextureType::Distance);
	probeDistance = shared_ptr<Texture>(static_cast<Texture*>(AbstractGfxLayer::CreateTexture2D(static_cast<FORMAT>(format), 
		RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, 
		RESOURCE_STATE_PIXEL_SHADER_RESOURCE, width, height, 1)));
	NAME_TEXTURE(probeDistance);
	
	volumeResources.probeDistance = probeDistance->resource.Get();
	

	// probe offsets
	rtxgi::GetDDGIVolumeTextureDimensions(volumeDesc, rtxgi::EDDGITextureType::Offsets, width, height);
	format = rtxgi::GetDDGIVolumeTextureFormat(rtxgi::EDDGITextureType::Offsets);
	probeOffsets = shared_ptr<Texture>(static_cast<Texture*>(AbstractGfxLayer::CreateTexture2D(static_cast<FORMAT>(format), 
		RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, 
		RESOURCE_STATE_UNORDERED_ACCESS, width, height, 1)));
	NAME_TEXTURE(probeOffsets);

	volumeResources.probeOffsets = probeOffsets->resource.Get();
	

	// probe states
	rtxgi::GetDDGIVolumeTextureDimensions(volumeDesc, rtxgi::EDDGITextureType::States, width, height);
	format = rtxgi::GetDDGIVolumeTextureFormat(rtxgi::EDDGITextureType::States);
	probeStates = shared_ptr<Texture>(static_cast<Texture*>(AbstractGfxLayer::CreateTexture2D(static_cast<FORMAT>(format), 
		RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, 
		RESOURCE_STATE_UNORDERED_ACCESS, width, height, 1)));
	NAME_TEXTURE(probeStates);

	volumeResources.probeStates = probeStates->resource.Get();


	// create descriptors
	UINT DescriptorSize = dx12_rhi->TextureDHRing->DescriptorSize;
	dx12_rhi->TextureDHRing->AllocDescriptor(volumeDescriptorTableCPUHandle, volumeDescriptorTableGPUHandle, 5); // descriptor table size is 5
	//DHOfsset = volumeDescriptorTableCPUHandle.ptr;
	volumeResources.descriptorHeap = dx12_rhi->SRVCBVDescriptorHeapShaderVisible->DH.Get();
	volumeResources.descriptorHeapDescSize = dx12_rhi->Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	volumeResources.descriptorHeapOffset = volumeDescriptorTableCPUHandle.ptr;

	D3D12_CPU_DESCRIPTOR_HANDLE probeRTRadianceHandleCPU = volumeDescriptorTableCPUHandle;
	// Create the RT radiance UAV 
	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	uavDesc.Format = rtxgi::GetDDGIVolumeTextureFormat(rtxgi::EDDGITextureType::RTRadiance);
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	dx12_rhi->Device->CreateUnorderedAccessView(probeRTRadiance->resource.Get(), nullptr, &uavDesc, probeRTRadianceHandleCPU);

	// irradiance
	D3D12_CPU_DESCRIPTOR_HANDLE probeIrradianceHandleCPU = volumeDescriptorTableCPUHandle;
	probeIrradianceHandleCPU.ptr += DescriptorSize * 1;
	uavDesc = {};
	uavDesc.Format = rtxgi::GetDDGIVolumeTextureFormat(rtxgi::EDDGITextureType::Irradiance);
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	dx12_rhi->Device->CreateUnorderedAccessView(probeIrradiance->resource.Get(), nullptr, &uavDesc, probeIrradianceHandleCPU);

	// distance
	D3D12_CPU_DESCRIPTOR_HANDLE probeDistanceHandleCPU = volumeDescriptorTableCPUHandle;
	probeDistanceHandleCPU.ptr += DescriptorSize * 2;
	uavDesc = {};
	uavDesc.Format = rtxgi::GetDDGIVolumeTextureFormat(rtxgi::EDDGITextureType::Distance);
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	dx12_rhi->Device->CreateUnorderedAccessView(probeDistance->resource.Get(), nullptr, &uavDesc, probeDistanceHandleCPU);

	// offsets
	D3D12_CPU_DESCRIPTOR_HANDLE probeOffsetsHandleCPU = volumeDescriptorTableCPUHandle;
	probeOffsetsHandleCPU.ptr += DescriptorSize * 3;
	uavDesc = {};
	uavDesc.Format = rtxgi::GetDDGIVolumeTextureFormat(rtxgi::EDDGITextureType::Offsets);
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	dx12_rhi->Device->CreateUnorderedAccessView(probeOffsets->resource.Get(), nullptr, &uavDesc, probeOffsetsHandleCPU);

	// states
	D3D12_CPU_DESCRIPTOR_HANDLE probeStatesHandleCPU = volumeDescriptorTableCPUHandle;
	probeStatesHandleCPU.ptr += DescriptorSize * 4;
	uavDesc = {};
	uavDesc.Format = rtxgi::GetDDGIVolumeTextureFormat(rtxgi::EDDGITextureType::States);
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	dx12_rhi->Device->CreateUnorderedAccessView(probeStates->resource.Get(), nullptr, &uavDesc, probeStatesHandleCPU);

	ComPtr<ID3DBlob> signature;
	rtxgi::GetDDGIVolumeRootSignatureDesc(0, &signature); // will use zero descriptorHeapOffset, because we will seperated descriptor table for volume
	if (signature == nullptr)
	{
		assert(false);
	}

	HRESULT hr = dx12_rhi->Device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&volumeResources.rootSignature));
	if (FAILED(hr))
	{
		assert(false);

	}
	volumeResources.rootSignature->SetName(L"volumeResources.rootSignature");

	// create volume pso

	// Create the radiance blending PSO
	D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.CS.BytecodeLength = shaders[0]->GetBufferSize();
	psoDesc.CS.pShaderBytecode = shaders[0]->GetBufferPointer();
	psoDesc.pRootSignature = volumeResources.rootSignature;

	hr = dx12_rhi->Device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&volumeResources.probeRadianceBlendingPSO));
	if (FAILED(hr))
	{
		assert(false);
	}

	// Create the distance blending PSO
	psoDesc.CS.BytecodeLength = shaders[1]->GetBufferSize();
	psoDesc.CS.pShaderBytecode = shaders[1]->GetBufferPointer();
	hr = dx12_rhi->Device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&volumeResources.probeDistanceBlendingPSO));
	if (FAILED(hr))
	{
		assert(false);
	}

	// Create the border row PSO
	psoDesc.CS.BytecodeLength = shaders[2]->GetBufferSize();
	psoDesc.CS.pShaderBytecode = shaders[2]->GetBufferPointer();
	hr = dx12_rhi->Device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&volumeResources.probeBorderRowPSO));
	if (FAILED(hr))
	{
		assert(false);
	}

	// Create the border column PSO
	psoDesc.CS.BytecodeLength = shaders[3]->GetBufferSize();
	psoDesc.CS.pShaderBytecode = shaders[3]->GetBufferPointer();
	hr = dx12_rhi->Device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&volumeResources.probeBorderColumnPSO));
	if (FAILED(hr))
	{
		assert(false);
	}

	// Create the probe relocation PSO
	psoDesc.CS.BytecodeLength = shaders[4]->GetBufferSize();
	psoDesc.CS.pShaderBytecode = shaders[4]->GetBufferPointer();
	hr = dx12_rhi->Device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&volumeResources.probeRelocationPSO));
	if (FAILED(hr))
	{
		assert(false);
	}

	// Create the probe classifier PSO
	psoDesc.CS.BytecodeLength = shaders[5]->GetBufferSize();
	psoDesc.CS.pShaderBytecode = shaders[5]->GetBufferPointer();
	hr = dx12_rhi->Device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&volumeResources.probeStateClassifierPSO));
	if (FAILED(hr))
	{
		assert(false);
	}

	// Create the probe classifier activate all PSO
	psoDesc.CS.BytecodeLength = shaders[6]->GetBufferSize();
	psoDesc.CS.pShaderBytecode = shaders[6]->GetBufferPointer();
	hr = dx12_rhi->Device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&volumeResources.probeStateClassifierActivateAllPSO));
	if (FAILED(hr))
	{
		assert(false);
	}


	rtxgi::ERTXGIStatus status = rtxgi::ERTXGIStatus::OK;

	volume = shared_ptr<rtxgi::DDGIVolume>(new rtxgi::DDGIVolume("Scene Volume"));


	volume->volumeDescriptorTableGPUHandle = volumeDescriptorTableGPUHandle;
	// Create the DDGIVolume
	status = volume->Create(volumeDesc, volumeResources);
	if (status != rtxgi::ERTXGIStatus::OK)
	{
		assert(false);
	}

	// gi rtpso
	{
		shared_ptr<RTPipelineStateObject> TEMP_PSO = shared_ptr<RTPipelineStateObject>(new RTPipelineStateObject);
		TEMP_PSO->NumInstance = vecBLAS.size();// scene->meshes.size();

		//TEMP_PSO->AddHitGroup("HitGroup", "chs", "");
		AbstractGfxLayer::AddHitGroup(TEMP_PSO.get(), "HitGroup", "chs", "");


		TEMP_PSO->AddShader("rayGen", RTPipelineStateObject::RAYGEN);

		TEMP_PSO->BindUAV("global", "DDGIProbeRTRadiance", 0);
		TEMP_PSO->BindUAV("global", "DDGIProbeStates", 1);
		TEMP_PSO->BindUAV("global", "DDGIProbeOffsets", 2);

		TEMP_PSO->BindSRV("global", "SceneBVH", 0);
		TEMP_PSO->BindSRV("global", "DDGIProbeIrradianceSRV", 1);
		TEMP_PSO->BindSRV("global", "DDGIProbeDistanceSRV", 2);
		//TEMP_PSO->BindSRV("global", "DDGIProbeStates", 3);
		//TEMP_PSO->BindSRV("global", "DDGIProbeOffsets", 4);
		TEMP_PSO->BindSRV("global", "BlueNoiseTex", 5);

		TEMP_PSO->BindCBV("global", "DDGIVolume", 0, sizeof(rtxgi::DDGIVolumeDesc));
		TEMP_PSO->BindCBV("global", "LightInfoCB", 1, sizeof(LightInfoCB));

		TEMP_PSO->BindSampler("global", "TrilinearSampler", 0);

		TEMP_PSO->AddShader("miss", RTPipelineStateObject::MISS);
		TEMP_PSO->AddShader("missShadow", RTPipelineStateObject::MISS);


		TEMP_PSO->AddShader("chs", RTPipelineStateObject::HIT);
		TEMP_PSO->BindSRV("chs", "vertices", 10);
		TEMP_PSO->BindSRV("chs", "indices", 11);
		TEMP_PSO->BindSRV("chs", "InstanceProperty", 12);
		TEMP_PSO->BindSRV("chs", "AlbedoTex", 13);

		TEMP_PSO->MaxRecursion = 1;
		TEMP_PSO->MaxAttributeSizeInBytes = sizeof(float) * 2;

		TEMP_PSO->MaxPayloadSizeInBytes = sizeof(float) * 12;

		vector< DxcDefine> def;
		def.push_back({L"RTXGI_DDGI_PROBE_RELOCATION", L"1"});
		def.push_back({ L"RTXGI_DDGI_PROBE_STATE_CLASSIFIER", L"1" });

		bool bSuccess = TEMP_PSO->InitRS("Shaders\\TraceProbe.hlsl");

		if (bSuccess)
		{
			PSO_RT_PROBE = TEMP_PSO;
		}
	}
}
#endif

#if USE_DLSS
void Corona::ShutDownDLSS()
{
	if (m_ngxInitialized)
	{
		NVSDK_NGX_Result ResultDLSS = (m_dlssFeature != nullptr) ? NVSDK_NGX_D3D12_ReleaseFeature(m_dlssFeature) : NVSDK_NGX_Result_Success;
		if (NVSDK_NGX_FAILED(ResultDLSS))
		{
			std::stringstream strStream;
			strStream << "Failed to NVSDK_NGX_D3D12_ReleaseFeature, code =" << ResultDLSS << "info: " << GetNGXResultAsString(ResultDLSS);
		}
	}

}
#endif

void Corona::InitRTPSO()
{
	// create shadow rtpso
	{
		shared_ptr<GfxRTPipelineStateObject> TEMP_PSO_RT_SHADOW = shared_ptr<GfxRTPipelineStateObject>(AbstractGfxLayer::CreateRTPSO());

		// new interface
		AbstractGfxLayer::AddHitGroup(TEMP_PSO_RT_SHADOW.get(), "HitGroup", "", "anyhit");
		AbstractGfxLayer::AddShader(TEMP_PSO_RT_SHADOW.get(), "rayGen", RAYGEN);
		AbstractGfxLayer::BindUAV(TEMP_PSO_RT_SHADOW.get(), "global", "ShadowResult", 0);
		AbstractGfxLayer::BindSRV(TEMP_PSO_RT_SHADOW.get(), "global", "gRtScene", 0);
		AbstractGfxLayer::BindSRV(TEMP_PSO_RT_SHADOW.get(), "global", "DepthTex", 1);
		AbstractGfxLayer::BindSRV(TEMP_PSO_RT_SHADOW.get(), "global", "WorldNormalTex", 2);
		AbstractGfxLayer::BindCBV(TEMP_PSO_RT_SHADOW.get(), "global", "ViewParameter", 0, sizeof(RTShadowViewParamCB));
		AbstractGfxLayer::BindSampler(TEMP_PSO_RT_SHADOW.get(), "global", "samplerWrap", 0);
		AbstractGfxLayer::AddShader(TEMP_PSO_RT_SHADOW.get(), "miss", MISS);
		AbstractGfxLayer::AddShader(TEMP_PSO_RT_SHADOW.get(), "anyhit", ANYHIT);
		AbstractGfxLayer::BindSRV(TEMP_PSO_RT_SHADOW.get(), "anyhit", "vertices", 3);
		AbstractGfxLayer::BindSRV(TEMP_PSO_RT_SHADOW.get(), "anyhit", "indices", 4);
		AbstractGfxLayer::BindSRV(TEMP_PSO_RT_SHADOW.get(), "anyhit", "AlbedoTex", 5);
		AbstractGfxLayer::BindSRV(TEMP_PSO_RT_SHADOW.get(), "anyhit", "InstanceProperty", 6);

		
		RTPSO_DESC desc = {
			1, //MaxRecursion
			sizeof(float) * 2, // MaxAttributeSizeInBytes
			sizeof(float) * 2, // MaxPayloadSizeInBytes
			"Shaders\\RaytracedShadow.hlsl", // shader file
			// Defines
		};

		bool bSuccess = AbstractGfxLayer::InitRTPSO(TEMP_PSO_RT_SHADOW.get(), &desc);

		if (bSuccess)
		{
			PSO_RT_SHADOW = TEMP_PSO_RT_SHADOW;
		}
	}

	// create reflection rtpso
	{
		shared_ptr<GfxRTPipelineStateObject> TEMP_PSO_RT_REFLECTION = shared_ptr<GfxRTPipelineStateObject>(AbstractGfxLayer::CreateRTPSO());

		AbstractGfxLayer::AddHitGroup(TEMP_PSO_RT_REFLECTION.get(), "HitGroup", "chs", "");
		AbstractGfxLayer::AddShader(TEMP_PSO_RT_REFLECTION.get(), "rayGen", RAYGEN);
		AbstractGfxLayer::BindUAV(TEMP_PSO_RT_REFLECTION.get(), "global", "ReflectionResult", 0);
		AbstractGfxLayer::BindSRV(TEMP_PSO_RT_REFLECTION.get(), "global", "gRtScene", 0);
		AbstractGfxLayer::BindSRV(TEMP_PSO_RT_REFLECTION.get(), "global", "DepthTex", 1);
		AbstractGfxLayer::BindSRV(TEMP_PSO_RT_REFLECTION.get(), "global", "GeoNormalTex", 2);
		AbstractGfxLayer::BindSRV(TEMP_PSO_RT_REFLECTION.get(), "global", "RougnessMetallicTex", 6);
		AbstractGfxLayer::BindSRV(TEMP_PSO_RT_REFLECTION.get(), "global", "BlueNoiseTex", 7);
		AbstractGfxLayer::BindSRV(TEMP_PSO_RT_REFLECTION.get(), "global", "WorldNormalTex", 8);
		AbstractGfxLayer::BindCBV(TEMP_PSO_RT_REFLECTION.get(), "global", "ViewParameter", 0, sizeof(RTReflectionViewParam));
		AbstractGfxLayer::BindSampler(TEMP_PSO_RT_REFLECTION.get(), "global", "samplerWrap", 0);
		AbstractGfxLayer::AddShader(TEMP_PSO_RT_REFLECTION.get(), "miss", MISS);
		AbstractGfxLayer::AddShader(TEMP_PSO_RT_REFLECTION.get(), "missShadow", MISS);
		AbstractGfxLayer::AddShader(TEMP_PSO_RT_REFLECTION.get(), "chs", HIT);
		AbstractGfxLayer::BindSRV(TEMP_PSO_RT_REFLECTION.get(), "chs", "vertices", 3);
		AbstractGfxLayer::BindSRV(TEMP_PSO_RT_REFLECTION.get(), "chs", "indices", 4);
		AbstractGfxLayer::BindSRV(TEMP_PSO_RT_REFLECTION.get(), "chs", "AlbedoTex", 5);
		AbstractGfxLayer::BindSRV(TEMP_PSO_RT_REFLECTION.get(), "chs", "InstanceProperty", 9);

		
		RTPSO_DESC desc = {
			1, //MaxRecursion
			sizeof(float) * 2, // MaxAttributeSizeInBytes
			sizeof(float) * 13, // MaxPayloadSizeInBytes
			"Shaders\\RaytracedReflection.hlsl", // shader file
			// Defines
		};

		bool bSuccess = AbstractGfxLayer::InitRTPSO(TEMP_PSO_RT_REFLECTION.get(), &desc);

		if (bSuccess)
		{
			PSO_RT_REFLECTION = TEMP_PSO_RT_REFLECTION;
		}
	}
	// gi rtpso
	{
		shared_ptr<GfxRTPipelineStateObject> TEMP_PSO_RT_GI = shared_ptr<GfxRTPipelineStateObject>(AbstractGfxLayer::CreateRTPSO());

		AbstractGfxLayer::AddHitGroup(TEMP_PSO_RT_GI.get(), "HitGroup", "chs", "");
		AbstractGfxLayer::AddShader(TEMP_PSO_RT_GI.get(), "rayGen", RAYGEN);
		AbstractGfxLayer::BindUAV(TEMP_PSO_RT_GI.get(), "global", "GIResultSH", 0);
		AbstractGfxLayer::BindUAV(TEMP_PSO_RT_GI.get(), "global", "GIResultColor", 1);
		AbstractGfxLayer::BindSRV(TEMP_PSO_RT_GI.get(), "global", "gRtScene", 0);
		AbstractGfxLayer::BindSRV(TEMP_PSO_RT_GI.get(), "global", "DepthTex", 1);
		AbstractGfxLayer::BindSRV(TEMP_PSO_RT_GI.get(), "global", "WorldNormalTex", 2);
		AbstractGfxLayer::BindCBV(TEMP_PSO_RT_GI.get(), "global", "ViewParameter", 0, sizeof(RTGIViewParam));
		AbstractGfxLayer::BindSampler(TEMP_PSO_RT_GI.get(), "global", "samplerWrap", 0);
		AbstractGfxLayer::BindSRV(TEMP_PSO_RT_GI.get(), "global", "BlueNoiseTex", 7);
		AbstractGfxLayer::AddShader(TEMP_PSO_RT_GI.get(), "miss", MISS);
		AbstractGfxLayer::AddShader(TEMP_PSO_RT_GI.get(), "missShadow", MISS);
		AbstractGfxLayer::AddShader(TEMP_PSO_RT_GI.get(), "chs", HIT);
		AbstractGfxLayer::BindSRV(TEMP_PSO_RT_GI.get(), "chs", "vertices", 3);
		AbstractGfxLayer::BindSRV(TEMP_PSO_RT_GI.get(), "chs", "indices", 4);
		AbstractGfxLayer::BindSRV(TEMP_PSO_RT_GI.get(), "chs", "AlbedoTex", 5);
		AbstractGfxLayer::BindSRV(TEMP_PSO_RT_GI.get(), "chs", "InstanceProperty", 6);

		RTPSO_DESC desc = {
			1, //MaxRecursion
			sizeof(float) * 2, // MaxAttributeSizeInBytes
			sizeof(float) * 13, // MaxPayloadSizeInBytes
			"Shaders\\RaytracedGI.hlsl", // shader file
			// Defines
		};

		bool bSuccess = AbstractGfxLayer::InitRTPSO(TEMP_PSO_RT_GI.get(), &desc);

		if (bSuccess)
		{
			PSO_RT_GI = TEMP_PSO_RT_GI;
		}
	}
}

void Corona::RaytraceShadowPass()
{
#if USE_AFTERMATH
	NVAftermathMarker(dx12_rhi->AM_CL_Handle, "RaytraceShadowPass");
#endif
	ProfileGPUScope(AbstractGfxLayer::GetGlobalCommandList(), PIX_COLOR(rand()%255, rand() % 255, rand() % 255), "RaytraceShadowPass");

	{
		std::array<ResourceTransition, 1> Transition = { {
			{ShadowBuffer.get(), RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_UNORDERED_ACCESS},
		} };
		AbstractGfxLayer::TransitionResource(AbstractGfxLayer::GetGlobalCommandList(), Transition.size(), Transition.data());
	}


	AbstractGfxLayer::BeginShaderTable(PSO_RT_SHADOW.get());

	int i = 0;
	for (auto&as : vecBLAS)
	{
		auto& mesh = as->mesh;
		GfxTexture* diffuseTex = mesh->Draws[0].mat->Diffuse.get();

		if (!diffuseTex)
			diffuseTex = DefaultWhiteTex.get();

		AbstractGfxLayer::ResetHitProgram(PSO_RT_SHADOW.get(), i);
		AbstractGfxLayer::StartHitProgram(PSO_RT_SHADOW.get(), "HitGroup", i);

		AbstractGfxLayer::AddSRVDescriptor2HitProgram(PSO_RT_SHADOW.get(), "HitGroup", mesh->Vb.get(), i);
		AbstractGfxLayer::AddSRVDescriptor2HitProgram(PSO_RT_SHADOW.get(), "HitGroup", mesh->Ib.get(), i);
		AbstractGfxLayer::AddSRVDescriptor2HitProgram(PSO_RT_SHADOW.get(), "HitGroup", diffuseTex, i);
		AbstractGfxLayer::AddSRVDescriptor2HitProgram(PSO_RT_SHADOW.get(), "HitGroup", InstancePropertyBuffer.get(), i);

		i++;
	}

	AbstractGfxLayer::SetUAV(PSO_RT_SHADOW.get(), "global", "ShadowResult", ShadowBuffer.get());

	AbstractGfxLayer::SetSRV(PSO_RT_SHADOW.get(), "global", "gRtScene", TLAS.get());

	AbstractGfxLayer::SetSRV(PSO_RT_SHADOW.get(), "global", "DepthTex", DepthBuffer.get());
	AbstractGfxLayer::SetSRV(PSO_RT_SHADOW.get(), "global", "WorldNormalTex", GeomNormalBuffer.get());
	AbstractGfxLayer::SetCBVValue(PSO_RT_SHADOW.get(), "global", "ViewParameter", &RTShadowViewParam);
	AbstractGfxLayer::SetSampler(PSO_RT_SHADOW.get(), "global", "samplerWrap", samplerBilinearWrap.get());


	AbstractGfxLayer::EndShaderTable(PSO_RT_SHADOW.get(), vecBLAS.size());

	AbstractGfxLayer::DispatchRay(PSO_RT_SHADOW.get(), RenderWidth, RenderHeight, AbstractGfxLayer::GetGlobalCommandList(), vecBLAS.size());

	{
		std::array<ResourceTransition, 1> Transition = { {
			{ShadowBuffer.get(), RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE},
		} };
		AbstractGfxLayer::TransitionResource(AbstractGfxLayer::GetGlobalCommandList(), Transition.size(), Transition.data());
	}
}

void Corona::RaytraceReflectionPass()
{
#if USE_AFTERMATH
	NVAftermathMarker(dx12_rhi->AM_CL_Handle, "RaytraceReflectionPass");
#endif
	ProfileGPUScope(AbstractGfxLayer::GetGlobalCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "RaytraceReflectionPass");

	{
		std::array<ResourceTransition, 1> Transition = { {
			{SpeculaGIBufferRaw.get(), RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_UNORDERED_ACCESS},
		} };
		AbstractGfxLayer::TransitionResource(AbstractGfxLayer::GetGlobalCommandList(), Transition.size(), Transition.data());
	}
	AbstractGfxLayer::BeginShaderTable(PSO_RT_REFLECTION.get());

	AbstractGfxLayer::SetUAV(PSO_RT_REFLECTION.get(), "global", "ReflectionResult", SpeculaGIBufferRaw.get());

	AbstractGfxLayer::SetSRV(PSO_RT_REFLECTION.get(), "global", "gRtScene", TLAS.get());
	AbstractGfxLayer::SetSRV(PSO_RT_REFLECTION.get(), "global", "DepthTex", DepthBuffer.get());
	AbstractGfxLayer::SetSRV(PSO_RT_REFLECTION.get(), "global", "GeoNormalTex", GeomNormalBuffer.get());
	AbstractGfxLayer::SetSRV(PSO_RT_REFLECTION.get(), "global", "RougnessMetallicTex", RoughnessMetalicBuffer.get());
	AbstractGfxLayer::SetSRV(PSO_RT_REFLECTION.get(), "global", "BlueNoiseTex", BlueNoiseTex.get());
	AbstractGfxLayer::SetSRV(PSO_RT_REFLECTION.get(), "global", "WorldNormalTex", NormalBuffers[ColorBufferWriteIndex].get());

	RTReflectionViewParam.ViewSpreadAngle = glm::tan(Fov * 0.5) / (0.5f * RenderHeight);
	AbstractGfxLayer::SetCBVValue(PSO_RT_REFLECTION.get(), "global", "ViewParameter", &RTReflectionViewParam);
	AbstractGfxLayer::SetSampler(PSO_RT_REFLECTION.get(), "global", "samplerWrap", samplerBilinearWrap.get());


	int i = 0;
	for(auto&as : vecBLAS)
	{
		auto& mesh = as->mesh;
		GfxTexture* diffuseTex = mesh->Draws[0].mat->Diffuse.get();

		if (!diffuseTex)
			diffuseTex = DefaultWhiteTex.get();
		AbstractGfxLayer::ResetHitProgram(PSO_RT_REFLECTION.get(), i);

		AbstractGfxLayer::StartHitProgram(PSO_RT_REFLECTION.get(), "HitGroup", i);
		AbstractGfxLayer::AddSRVDescriptor2HitProgram(PSO_RT_REFLECTION.get(), "HitGroup", mesh->Vb.get(), i);
		AbstractGfxLayer::AddSRVDescriptor2HitProgram(PSO_RT_REFLECTION.get(), "HitGroup", mesh->Ib.get(), i);
		AbstractGfxLayer::AddSRVDescriptor2HitProgram(PSO_RT_REFLECTION.get(), "HitGroup", diffuseTex, i);
		AbstractGfxLayer::AddSRVDescriptor2HitProgram(PSO_RT_REFLECTION.get(), "HitGroup", InstancePropertyBuffer.get(), i);

		
		i++;
	}

	AbstractGfxLayer::EndShaderTable(PSO_RT_REFLECTION.get(), vecBLAS.size());

	AbstractGfxLayer::DispatchRay(PSO_RT_REFLECTION.get(), RenderWidth, RenderHeight, AbstractGfxLayer::GetGlobalCommandList(), vecBLAS.size());

	{
		std::array<ResourceTransition, 1> Transition = { {
			{SpeculaGIBufferRaw.get(), RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE},
		} };
		AbstractGfxLayer::TransitionResource(AbstractGfxLayer::GetGlobalCommandList(), Transition.size(), Transition.data());
	}
}

void Corona::RaytraceGIPass()
{
#if USE_AFTERMATH
	NVAftermathMarker(dx12_rhi->AM_CL_Handle, "RaytraceGIPass");
#endif
	ProfileGPUScope(AbstractGfxLayer::GetGlobalCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "RaytraceGIPass");

	{
		std::array<ResourceTransition, 2> Transition = { {
			{DiffuseGISHRaw.get(), RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_UNORDERED_ACCESS},
			{DiffuseGICoCgRaw.get(), RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_UNORDERED_ACCESS}
		} };
		AbstractGfxLayer::TransitionResource(AbstractGfxLayer::GetGlobalCommandList(), Transition.size(), Transition.data());
	}

	AbstractGfxLayer::BeginShaderTable(PSO_RT_GI.get());

	AbstractGfxLayer::SetUAV(PSO_RT_GI.get(), "global", "GIResultSH", DiffuseGISHRaw.get());
	AbstractGfxLayer::SetUAV(PSO_RT_GI.get(), "global", "GIResultColor", DiffuseGICoCgRaw.get());
	AbstractGfxLayer::SetSRV(PSO_RT_GI.get(), "global", "gRtScene", TLAS.get());
	AbstractGfxLayer::SetSRV(PSO_RT_GI.get(), "global", "DepthTex", DepthBuffer.get());
	AbstractGfxLayer::SetSRV(PSO_RT_GI.get(), "global", "WorldNormalTex", NormalBuffers[ColorBufferWriteIndex].get());
	AbstractGfxLayer::SetSRV(PSO_RT_GI.get(), "global", "BlueNoiseTex", BlueNoiseTex.get());
	
	RTGIViewParam.ViewSpreadAngle = glm::tan(Fov * 0.5) / (0.5f * RenderHeight);

#if USE_NRD
	if(bNRDDenoising)
		RTGIViewParam.bPackNRD = 1;
	else
#endif
		RTGIViewParam.bPackNRD = 0;

	AbstractGfxLayer::SetCBVValue(PSO_RT_GI.get(), "global", "ViewParameter", &RTGIViewParam);
	AbstractGfxLayer::SetSampler(PSO_RT_GI.get(), "global", "samplerWrap", samplerBilinearWrap.get());

	int i = 0;
	for(auto&as : vecBLAS)
	{
		auto& mesh = as->mesh;
		
		GfxTexture* diffuseTex = mesh->Draws[0].mat->Diffuse.get();
		if (!diffuseTex)
			diffuseTex = DefaultWhiteTex.get();

		AbstractGfxLayer::ResetHitProgram(PSO_RT_GI.get(), i);

		AbstractGfxLayer::StartHitProgram(PSO_RT_GI.get(), "HitGroup", i);
		AbstractGfxLayer::AddSRVDescriptor2HitProgram(PSO_RT_GI.get(), "HitGroup", mesh->Vb.get(), i);
		AbstractGfxLayer::AddSRVDescriptor2HitProgram(PSO_RT_GI.get(), "HitGroup", mesh->Ib.get(), i);
		AbstractGfxLayer::AddSRVDescriptor2HitProgram(PSO_RT_GI.get(), "HitGroup", diffuseTex, i);
		AbstractGfxLayer::AddSRVDescriptor2HitProgram(PSO_RT_GI.get(), "HitGroup", InstancePropertyBuffer.get(), i);


		i++;
	}

	AbstractGfxLayer::EndShaderTable(PSO_RT_GI.get(), vecBLAS.size());


	AbstractGfxLayer::DispatchRay(PSO_RT_GI.get(), RenderWidth, RenderHeight, AbstractGfxLayer::GetGlobalCommandList(), vecBLAS.size());

	{
		std::array<ResourceTransition, 2> Transition = { {
			{DiffuseGISHRaw.get(), RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE},
			{DiffuseGICoCgRaw.get(), RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | RESOURCE_STATE_PIXEL_SHADER_RESOURCE}
		} };
		AbstractGfxLayer::TransitionResource(AbstractGfxLayer::GetGlobalCommandList(), Transition.size(), Transition.data());
	}
}
