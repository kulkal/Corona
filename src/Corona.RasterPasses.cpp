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

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>

void Corona::InitBloomPass()
{
	{
		ComPtr<ID3DBlob> cs = renderBackend->CreateShader(GetAssetFullPath(L"Shaders\\BloomBlur.hlsl"), "BloomExtract", "cs_6_0");
		D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};

		shared_ptr<PipelineStateObject> TEMP_BloomExtractPSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
		TEMP_BloomExtractPSO->Owner = dx12_rhi;
		TEMP_BloomExtractPSO->cs = cs;
		TEMP_BloomExtractPSO->computePSODesc = computePsoDesc;
		TEMP_BloomExtractPSO->BindSRV("SrcTex", 0, 1);
		TEMP_BloomExtractPSO->BindSRV("Exposure", 1, 1);
		TEMP_BloomExtractPSO->BindUAV("DstTex", 0);
		TEMP_BloomExtractPSO->BindUAV("LumaResult", 1);
		TEMP_BloomExtractPSO->BindSampler("samplerWrap", 0);
		TEMP_BloomExtractPSO->BindCBV("BloomCB", 0, sizeof(BloomCB));
		TEMP_BloomExtractPSO->IsCompute = true;
		bool bSuccess = TEMP_BloomExtractPSO->Init();
		if (bSuccess)
			BloomExtractPSO = TEMP_BloomExtractPSO;
	}
	{
		ComPtr<ID3DBlob> cs = renderBackend->CreateShader(GetAssetFullPath(L"Shaders\\BloomBlur.hlsl"), "BloomBlur", "cs_6_0");
		D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};

		shared_ptr<PipelineStateObject> TEMP_BloomBlurPSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
		TEMP_BloomBlurPSO->Owner = dx12_rhi;
		TEMP_BloomBlurPSO->cs = cs;
		TEMP_BloomBlurPSO->computePSODesc = computePsoDesc;
		TEMP_BloomBlurPSO->BindSRV("SrcTex", 0, 1);
		TEMP_BloomBlurPSO->BindUAV("DstTex", 0);
		TEMP_BloomBlurPSO->BindSampler("samplerWrap", 0);
		TEMP_BloomBlurPSO->BindCBV("BloomCB", 0, sizeof(BloomCB));
		TEMP_BloomBlurPSO->IsCompute = true;
		bool bSucess = TEMP_BloomBlurPSO->Init();
		if (bSucess)
			BloomBlurPSO = TEMP_BloomBlurPSO;
	}

	{
		ComPtr<ID3DBlob> cs = renderBackend->CreateShader(GetAssetFullPath(L"Shaders\\Histogram.hlsl"), "GenerateHistogram", "cs_6_0");
		D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};

		shared_ptr<PipelineStateObject> TEMP_HistogramPSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
		TEMP_HistogramPSO->Owner = dx12_rhi;
		TEMP_HistogramPSO->cs = cs;
		TEMP_HistogramPSO->computePSODesc = computePsoDesc;
		TEMP_HistogramPSO->BindSRV("LumaTex", 0, 1);
		TEMP_HistogramPSO->BindUAV("Histogram", 0);
		TEMP_HistogramPSO->IsCompute = true;
		bool bSucess = TEMP_HistogramPSO->Init();
		if (bSucess)
			HistogramPSO = TEMP_HistogramPSO;

	}

	{
		ComPtr<ID3DBlob> cs = renderBackend->CreateShader(GetAssetFullPath(L"Shaders\\DrawHistogram.hlsl"), "DrawHistogram", "cs_6_0");
		D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};

		shared_ptr<PipelineStateObject> TEMP_DrawHistogramPSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
		TEMP_DrawHistogramPSO->Owner = dx12_rhi;
		TEMP_DrawHistogramPSO->cs = cs;
		TEMP_DrawHistogramPSO->computePSODesc = computePsoDesc;
		TEMP_DrawHistogramPSO->BindSRV("Histogram", 0, 1);
		TEMP_DrawHistogramPSO->BindSRV("Exposure", 1, 1);
		TEMP_DrawHistogramPSO->BindUAV("ColorBuffer", 0);
		TEMP_DrawHistogramPSO->IsCompute = true;
		bool bSuccess = TEMP_DrawHistogramPSO->Init();
		if (bSuccess)
			DrawHistogramPSO = TEMP_DrawHistogramPSO;

	}

	{
		ComPtr<ID3DBlob> cs = renderBackend->CreateShader(GetAssetFullPath(L"Shaders\\Histogram.hlsl"), "ClearHistogram", "cs_6_0");
		D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};

		shared_ptr<PipelineStateObject> TEMP_ClearHistogramPSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
		TEMP_ClearHistogramPSO->Owner = dx12_rhi;
		TEMP_ClearHistogramPSO->cs = cs;
		TEMP_ClearHistogramPSO->computePSODesc = computePsoDesc;
		TEMP_ClearHistogramPSO->BindUAV("Histogram", 0);
		TEMP_ClearHistogramPSO->IsCompute = true;
		bool bSuccess = TEMP_ClearHistogramPSO->Init();
		if (bSuccess)
			ClearHistogramPSO = TEMP_ClearHistogramPSO;

	}


	{
		ComPtr<ID3DBlob> cs = renderBackend->CreateShader(GetAssetFullPath(L"Shaders\\AdaptExposureCS.hlsl"), "AdaptExposure", "cs_6_0");
		D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};

		shared_ptr<PipelineStateObject> TEMP_AdapteExposurePSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
		TEMP_AdapteExposurePSO->Owner = dx12_rhi;
		TEMP_AdapteExposurePSO->cs = cs;
		TEMP_AdapteExposurePSO->computePSODesc = computePsoDesc;
		TEMP_AdapteExposurePSO->BindSRV("Histogram", 0, 1);
		TEMP_AdapteExposurePSO->BindUAV("Exposure", 0);
		TEMP_AdapteExposurePSO->BindUAV("Exposure", 0);
		TEMP_AdapteExposurePSO->BindCBV("AdaptExposureCB", 0, sizeof(AdaptExposureCB));

		TEMP_AdapteExposurePSO->IsCompute = true;
		bool bSucess = TEMP_AdapteExposurePSO->Init();
		if (bSucess)
			AdapteExposurePSO = TEMP_AdapteExposurePSO;

	}

	BloomBlurPingPong[0] = renderBackend->CreateTexture2D({ ETextureFormat::RGBA16Float, TextureUsage_UnorderedAccess, EInitialResourceState::ShaderRead, (int)BloomBufferWidth, (int)BloomBufferHeight, 1, std::nullopt });

	NAME_D3D12_OBJECT(BloomBlurPingPong[0]->resource);

	BloomBlurPingPong[1] = renderBackend->CreateTexture2D({ ETextureFormat::RGBA16Float, TextureUsage_UnorderedAccess, EInitialResourceState::ShaderRead, (int)BloomBufferWidth, (int)BloomBufferHeight, 1, std::nullopt });

	NAME_D3D12_OBJECT(BloomBlurPingPong[1]->resource);


	LumaBuffer = renderBackend->CreateTexture2D({ ETextureFormat::R8Uint, TextureUsage_UnorderedAccess, EInitialResourceState::ShaderRead, (int)BloomBufferWidth, (int)BloomBufferHeight, 1, std::nullopt });

	NAME_D3D12_OBJECT(LumaBuffer->resource);

	Histogram = renderBackend->CreateBuffer({ 256u, sizeof(UINT32), EInitialResourceState::ShaderRead, true, nullptr });
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

	ExposureData = renderBackend->CreateBuffer({ 8u, sizeof(float), EInitialResourceState::ShaderRead, true, initExposure });
	ExposureData->MakeStructuredBufferSRV();
	NAME_D3D12_OBJECT(ExposureData->resource);

}

void Corona::InitGBufferPass()
{
	GraphicsPipelineDesc desc{};
	desc.ShaderPath = GetAssetFullPath(L"Shaders\\GBuffer.hlsl");
	desc.VertexEntryPoint = "VSMain";
	desc.PixelEntryPoint = "PSMain";
	desc.VertexElements = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 12 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 24 },
		{ "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 32 },
	};
	desc.TextureBindings = {
		{ "AlbedoTex", 0 },
		{ "NormalTex", 1 },
		{ "RoughnessTex", 2 },
		{ "MetallicTex", 3 },
	};
	desc.SamplerBindings = {
		{ "samplerWrap", 0 },
	};
	desc.VertexStride = 44;
	desc.ColorFormats = {
		DXGI_FORMAT_R8G8B8A8_UNORM,
		DXGI_FORMAT_R8G8B8A8_UNORM,
		DXGI_FORMAT_R16G16B16A16_FLOAT,
		DXGI_FORMAT_R16G16B16A16_FLOAT,
		DXGI_FORMAT_R16G16_FLOAT,
		DXGI_FORMAT_R8G8B8A8_UNORM,
		DXGI_FORMAT_R32_FLOAT,
	};
	desc.DepthFormat = DXGI_FORMAT_D32_FLOAT;
	desc.bDepthEnable = true;
	desc.bCullBackFaces = false;
	desc.ConstantBufferSize = sizeof(GBufferConstantBuffer);
	desc.ConstantBufferBinding = 0;

	GBufferGraphicsPipeline = renderBackend->CreateGraphicsPipeline(desc);
}

void Corona::InitToneMapPass()
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

	FullScreenVB = renderBackend->CreateVertexBuffer(vertexBufferSize, vertexBufferStride, &quadVertices);

	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::Vulkan)
	{
		GraphicsPipelineDesc desc{};
		desc.ShaderPath = GetAssetFullPath(L"Shaders\\ToneMapPS.hlsl");
		desc.VertexEntryPoint = "VSMain";
		desc.PixelEntryPoint = "PSMain";
		desc.VertexStride = vertexBufferStride;
		desc.ColorFormats = { DXGI_FORMAT_R8G8B8A8_UNORM };
		desc.bDepthEnable = false;
		desc.bCullBackFaces = false;
		desc.bTriangleStrip = true;
		desc.ConstantBufferSize = sizeof(ToneMapCB);
		desc.ConstantBufferBinding = 0;
		desc.VertexElements = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 16 }
		};
		desc.TextureBindings = {
			{ "SrcTex", 0 }
		};
		desc.SamplerBindings = {
			{ "sampleWrap", 0 }
		};

		ToneMapGraphicsPipeline = renderBackend->CreateGraphicsPipeline(desc);
		return;
	}
	
	ComPtr<ID3DBlob> vs = renderBackend->CreateShader(GetAssetFullPath(L"Shaders\\ToneMapPS.hlsl"), "VSMain", "vs_6_0");
	ComPtr<ID3DBlob> ps = renderBackend->CreateShader(GetAssetFullPath(L"Shaders\\ToneMapPS.hlsl"), "PSMain", "ps_6_0");


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

	shared_ptr<PipelineStateObject> TEMP_ToneMapPSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
	TEMP_ToneMapPSO->Owner = dx12_rhi;
	TEMP_ToneMapPSO->ps = ps;
	TEMP_ToneMapPSO->vs = vs;
	TEMP_ToneMapPSO->graphicsPSODesc = psoDesc;

	TEMP_ToneMapPSO->BindSRV("SrcTex", 0, 1);
	TEMP_ToneMapPSO->BindSampler("samplerWrap", 0);
	TEMP_ToneMapPSO->BindCBV("ScaleOffsetParams", 0, sizeof(ToneMapCB));

	bool bSuccess = TEMP_ToneMapPSO->Init();
	if (bSuccess)
		ToneMapPSO = TEMP_ToneMapPSO;
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

	FullScreenVB = renderBackend->CreateVertexBuffer(vertexBufferSize, vertexBufferStride, &quadVertices);

	ComPtr<ID3DBlob> vs = renderBackend->CreateShader(GetAssetFullPath(L"Shaders\\DebugPS.hlsl"), "VSMain", "vs_6_0");
	ComPtr<ID3DBlob> ps = renderBackend->CreateShader(GetAssetFullPath(L"Shaders\\DebugPS.hlsl"), "PSMain", "ps_6_0");

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

	shared_ptr<PipelineStateObject> TEMP_BufferVisualizePSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
	TEMP_BufferVisualizePSO->Owner = dx12_rhi;
	TEMP_BufferVisualizePSO->ps = ps;
	TEMP_BufferVisualizePSO->vs = vs;
	TEMP_BufferVisualizePSO->graphicsPSODesc = psoDesc;

	TEMP_BufferVisualizePSO->BindSRV("SrcTex", 0, 1);
	TEMP_BufferVisualizePSO->BindSRV("SrcTexSH", 1, 1);
	TEMP_BufferVisualizePSO->BindSRV("SrcTexNormal", 2, 1);

	TEMP_BufferVisualizePSO->BindSampler("samplerWrap", 0);
	TEMP_BufferVisualizePSO->BindCBV("DebugPassCB", 0, sizeof(DebugPassCB));

	bool bSuccess = TEMP_BufferVisualizePSO->Init();
	if (bSuccess)
		BufferVisualizePSO = TEMP_BufferVisualizePSO;
}

void Corona::InitLightingPass()
{
	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::Vulkan)
	{
		struct PostVertex
		{
			XMFLOAT4 position;
			XMFLOAT2 uv;
		};

		PostVertex quadVertices[] =
		{
			{ { -1.0f, -1.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },
			{ { -1.0f, 1.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } },
			{ { 1.0f, -1.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },
			{ { 1.0f, 1.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } }
		};

		FullScreenVB = renderBackend->CreateVertexBuffer(sizeof(quadVertices), sizeof(PostVertex), &quadVertices);

		GraphicsPipelineDesc desc{};
		desc.ShaderPath = GetAssetFullPath(L"Shaders\\LightingPS.hlsl");
		desc.VertexEntryPoint = "VSMain";
		desc.PixelEntryPoint = "PSMain";
		desc.VertexStride = sizeof(PostVertex);
		desc.ColorFormats = { DXGI_FORMAT_R16G16B16A16_FLOAT };
		desc.bDepthEnable = false;
		desc.bCullBackFaces = false;
		desc.bTriangleStrip = true;
		desc.ConstantBufferSize = sizeof(LightingParam);
		desc.ConstantBufferBinding = 0;
		desc.VertexElements = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 16 }
		};
		desc.TextureBindings = {
			{ "AlbedoTex", 0 },
			{ "NormalTex", 1 },
			{ "ShadowTex", 2 },
			{ "VelocityTex", 3 },
			{ "DepthTex", 4 },
			{ "GIResultSHTex", 5 },
			{ "GIResultColorTex", 6 },
			{ "SpecularGITex", 7 },
			{ "RoughnessMetalicTex", 8 },
			{ "AmbientOcclusionTex", 14 },
			{ "SkyLightingTex", 15 }
		};
		desc.SamplerBindings = {
			{ "sampleWrap", 0 }
		};

		LightingGraphicsPipeline = renderBackend->CreateGraphicsPipeline(desc);
		return;
	}

	ComPtr<ID3DBlob> vs = renderBackend->CreateShader(GetAssetFullPath(L"Shaders\\LightingPS.hlsl"), "VSMain", "vs_6_0");
	ComPtr<ID3DBlob> ps = renderBackend->CreateShader(GetAssetFullPath(L"Shaders\\LightingPS.hlsl"), "PSMain", "ps_6_0");

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

	shared_ptr<PipelineStateObject> TEMP_LightingPSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
	TEMP_LightingPSO->Owner = dx12_rhi;
	TEMP_LightingPSO->ps = ps;
	TEMP_LightingPSO->vs = vs;
	TEMP_LightingPSO->graphicsPSODesc = psoDesc;
	
	TEMP_LightingPSO->BindSRV("AlbedoTex", 0, 1);
	TEMP_LightingPSO->BindSRV("NormalTex", 1, 1);
	TEMP_LightingPSO->BindSRV("ShadowTex", 2, 1);
	TEMP_LightingPSO->BindSRV("VelocityTex", 3, 1);
	TEMP_LightingPSO->BindSRV("DepthTex", 4, 1);
	TEMP_LightingPSO->BindSRV("GIResultSHTex", 5, 1);
	TEMP_LightingPSO->BindSRV("GIResultColorTex", 6, 1);
	TEMP_LightingPSO->BindSRV("SpecularGITex", 7, 1);
	TEMP_LightingPSO->BindSRV("RoughnessMetalicTex", 8, 1);
	TEMP_LightingPSO->BindSRV("SpecularGITex3x3", 9, 1);
	TEMP_LightingPSO->BindSRV("SpecularGITexMip1", 10, 1);
	TEMP_LightingPSO->BindSRV("SpecularGITexMip2", 11, 1);
	TEMP_LightingPSO->BindSRV("SpecularGITexMip3", 12, 1);
	TEMP_LightingPSO->BindSRV("SpecularGITexMip4", 13, 1);
	TEMP_LightingPSO->BindSRV("AmbientOcclusionTex", 14, 1);
	TEMP_LightingPSO->BindSRV("SkyLightingTex", 15, 1);
	
	
	
	TEMP_LightingPSO->BindSampler("samplerWrap", 0);
	TEMP_LightingPSO->BindCBV("LightingParam", 0, sizeof(LightingParam));
	bool bSuccess = TEMP_LightingPSO->Init();
	if (bSuccess)
		LightingPSO = TEMP_LightingPSO;
}

void Corona::InitTemporalAAPass()
{
	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::Vulkan)
	{
		struct PostVertex
		{
			XMFLOAT4 position;
			XMFLOAT2 uv;
		};

		PostVertex quadVertices[] =
		{
			{ { -1.0f, -1.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },
			{ { -1.0f, 1.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } },
			{ { 1.0f, -1.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },
			{ { 1.0f, 1.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } }
		};

		FullScreenVB = renderBackend->CreateVertexBuffer(sizeof(quadVertices), sizeof(PostVertex), &quadVertices);

		GraphicsPipelineDesc desc{};
		desc.ShaderPath = GetAssetFullPath(L"Shaders\\TemporalAA.hlsl");
		desc.VertexEntryPoint = "VSMain";
		desc.PixelEntryPoint = "PSMain";
		desc.VertexStride = sizeof(PostVertex);
		desc.ColorFormats = { DXGI_FORMAT_R16G16B16A16_FLOAT };
		desc.bDepthEnable = false;
		desc.bCullBackFaces = false;
		desc.bTriangleStrip = true;
		desc.ConstantBufferSize = sizeof(TemporalAAParam);
		desc.ConstantBufferBinding = 0;
		desc.VertexElements = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 16 }
		};
		desc.TextureBindings = {
			{ "CurrentColorTex", 0 },
			{ "PrevColorTex", 1 },
			{ "VelocityTex", 2 },
			{ "DepthTex", 3 },
			{ "BloomTex", 4 }
		};
		desc.BufferBindings = {
			{ "Exposure", 5 }
		};
		desc.SamplerBindings = {
			{ "sampleWrap", 0 }
		};

		TemporalAAGraphicsPipeline = renderBackend->CreateGraphicsPipeline(desc);
		return;
	}

	ComPtr<ID3DBlob> vs = renderBackend->CreateShader(GetAssetFullPath(L"Shaders\\TemporalAA.hlsl"), "VSMain", "vs_6_0");
	ComPtr<ID3DBlob> ps = renderBackend->CreateShader(GetAssetFullPath(L"Shaders\\TemporalAA.hlsl"), "PSMain", "ps_6_0");
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

	shared_ptr<PipelineStateObject> TEMP_TemporalAAPSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
	TEMP_TemporalAAPSO->Owner = dx12_rhi;
	TEMP_TemporalAAPSO->ps = ps;
	TEMP_TemporalAAPSO->vs = vs;
	TEMP_TemporalAAPSO->graphicsPSODesc = psoDesc;

	TEMP_TemporalAAPSO->BindSRV("CurrentColorTex", 0, 1);
	TEMP_TemporalAAPSO->BindSRV("PrevColorTex", 1, 1);
	TEMP_TemporalAAPSO->BindSRV("VelocityTex", 2, 1);
	TEMP_TemporalAAPSO->BindSRV("DepthTex", 3, 1);
	TEMP_TemporalAAPSO->BindSRV("BloomTex", 4, 1);
	TEMP_TemporalAAPSO->BindSRV("Exposure", 5, 1);


		TEMP_TemporalAAPSO->BindSampler("samplerWrap", 0);
		TEMP_TemporalAAPSO->BindCBV("TemporalAAParam", 0, sizeof(TemporalAAParam));
	bool bSuccess = TEMP_TemporalAAPSO->Init();
	if (bSuccess)
		TemporalAAPSO = TEMP_TemporalAAPSO;
}

void Corona::ToneMapPass()
{
	renderBackend->EmitGpuCrashMarker("CopyPass");
	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::D3D12)
	{
		PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "CopyPass");
	}

	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::Vulkan)
	{
		if (!ToneMapGraphicsPipeline)
			return;

		Texture* ResolveTarget = nullptr;
		if (RenderingMode == ERenderingMode::PATHTRACING)
			ResolveTarget = PathTracingAccumBuffer[PathTracingWriteIndex].get();
		else if (bUseLightingBufferFallbackForToneMap && LightingBuffer)
			ResolveTarget = LightingBuffer.get();
		else
			ResolveTarget = ColorBuffers[ResolvedColorBufferIndex].get();
		if (!ResolveTarget)
			return;

		ToneMapCB.Offset = glm::vec4(0, 0, 0, 0);
		ToneMapCB.Scale = glm::vec4(1, 1, 0, 0);
		ToneMapCB.ToneMapMode = ToneMapMode;

		renderBackend->BindGraphicsPipelineTexture(ToneMapGraphicsPipeline.get(), "SrcTex", ResolveTarget);
		renderBackend->BindGraphicsPipelineSampler(ToneMapGraphicsPipeline.get(), "sampleWrap", samplerWrap.get());
		renderBackend->SetGraphicsPipelineConstantData(ToneMapGraphicsPipeline.get(), 0, &ToneMapCB, sizeof(ToneMapCB));
		renderBackend->BindGraphicsPipeline(ToneMapGraphicsPipeline.get());
		renderBackend->SetViewportAndScissor(m_width, m_height);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
		return;
	}


	Texture* backbuffer = framebuffers[renderBackend->GetCurrentFrameIndex()].get();
	Texture* ResolveTarget = nullptr;
	
	// Select source texture based on rendering mode
	if (RenderingMode == ERenderingMode::PATHTRACING)
	{
		ResolveTarget = PathTracingAccumBuffer[PathTracingWriteIndex].get();
	}
	else
	{
		if (bUseLightingBufferFallbackForToneMap && LightingBuffer)
			ResolveTarget = LightingBuffer.get();
		else
			ResolveTarget = ColorBuffers[ResolvedColorBufferIndex].get();
	}

	ToneMapPSO->Apply();


	ToneMapPSO->SetSampler("samplerWrap", samplerWrap.get());
	ToneMapPSO->SetSRV("SrcTex", ResolveTarget->GpuHandleSRV);

	ToneMapCB.Offset = glm::vec4(0, 0, 0, 0);
	ToneMapCB.Scale = glm::vec4(1, 1, 0, 0);
	ToneMapCB.ToneMapMode = ToneMapMode;
	ToneMapPSO->SetCBVValue("ScaleOffsetParams", &ToneMapCB);

	ToneMapPSO->Apply();

	renderBackend->SetViewportAndScissor(m_width, m_height);
	renderBackend->DrawFullscreenQuad(FullScreenVB.get());

	
	//PIXEndEvent(renderBackend->GetGraphicsCommandList());
}

void Corona::DebugPass()
{
	if (!renderBackend ||
		renderBackend->GetAPI() != ERenderBackendAPI::D3D12 ||
		!BufferVisualizePSO)
	{
		bDebugDraw = false;
		return;
	}

	renderBackend->EmitGpuCrashMarker("DebugPass");
	PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "DebugPass");

	BufferVisualizePSO->Apply();
	BufferVisualizePSO->SetSampler("samplerWrap", samplerWrap.get());

	Texture* backbuffer = framebuffers[renderBackend->GetCurrentFrameIndex()].get();

	renderBackend->DrawFullscreenQuad(FullScreenVB.get());

	

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
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", ShadowBuffer->GpuHandleSRV);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	});

	functions.push_back([&](EDebugVisualization eFS) {
		// ray traced ambient occlusion
		DebugPassCB cb;

		if (eFS == EDebugVisualization::RTAO)
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
		if (!AmbientOcclusionBuffer)
			return;
		cb.DebugMode = CHANNEL_X;
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", AmbientOcclusionBuffer->GpuHandleSRV);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
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
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", NormalBuffers[ColorBufferWriteIndex]->GpuHandleSRV);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
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
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", GeomNormalBuffers[ColorBufferWriteIndex]->GpuHandleSRV);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
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
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", BloomBlurPingPong[0]->GpuHandleSRV);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
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
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", UnjitteredDepthBuffers[ColorBufferWriteIndex]->GpuHandleSRV);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// raw diffuse gi
		DebugPassCB cb;

		if (eFS == EDebugVisualization::RAW_DIFFUSE_GI)
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
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", DiffuseGIRaw->GpuHandleSRV);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// raw diffuse gi aux
		DebugPassCB cb;

		if (eFS == EDebugVisualization::RAW_DIFFUSE_GI_AUX)
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
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", DiffuseGIRawAux->GpuHandleSRV);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// screen probe diffuse gi resolve
		DebugPassCB cb;

		if (eFS == EDebugVisualization::SCREEN_PROBE_DIFFUSE_GI)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else
		{
			return;
		}

		if (!ScreenProbeGIResolved)
			return;
		cb.DebugMode = RAW_COPY;
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", ScreenProbeGIResolved->GpuHandleSRV);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// nearest screen probe radiance debug
		DebugPassCB cb;

		if (eFS == EDebugVisualization::SCREEN_PROBE_PROBES)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else
		{
			return;
		}

		if (!ScreenProbeGIProbeDebug)
			return;
		cb.DebugMode = RAW_COPY;
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", ScreenProbeGIProbeDebug->GpuHandleSRV);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// full-resolution screen-probe resolve history length
		DebugPassCB cb;

		if (eFS == EDebugVisualization::SCREEN_PROBE_HISTORY_LENGTH)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else
		{
			return;
		}

		if (!ScreenProbeGIHistory[ScreenProbeGIHistoryWriteIndex])
			return;
		cb.DebugMode = HISTORY_LENGTH;
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", ScreenProbeGIHistory[ScreenProbeGIHistoryWriteIndex]->GpuHandleSRV);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// probe-atlas radiance history length
		DebugPassCB cb;

		if (eFS == EDebugVisualization::SCREEN_PROBE_ATLAS_HISTORY_LENGTH)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else
		{
			return;
		}

		if (!ScreenProbeGIRadiance[ScreenProbeGIAtlasWriteIndex])
			return;
		cb.DebugMode = HISTORY_LENGTH;
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", ScreenProbeGIRadiance[ScreenProbeGIAtlasWriteIndex]->GpuHandleSRV);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// temporal filtered diffuse gi
		DebugPassCB cb;

		if (eFS == EDebugVisualization::TEMPORAL_FILTERED_DIFFUSE_GI)
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
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", DiffuseGITemporal[GIBufferWriteIndex]->GpuHandleSRV);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// spatial filtered diffuse gi
		DebugPassCB cb;

		if (eFS == EDebugVisualization::SPATIAL_FILTERED_DIFFUSE_GI)
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
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", DiffuseGISpatial[0]->GpuHandleSRV);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// final diffuse gi
		DebugPassCB cb;

		cb.RTSize = glm::vec2(GetRenderWidth(), GetRenderHeight());
		cb.GIBufferScale = GIBufferScale;
		if (eFS == EDebugVisualization::FINAL_DIFFUSE_GI)
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
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", DiffuseGISpatial[0]->GpuHandleSRV);
		BufferVisualizePSO->SetSRV("SrcTexSH", DiffuseGISpatialAux[0]->GpuHandleSRV);
		BufferVisualizePSO->SetSRV("SrcTexNormal", NormalBuffers[ColorBufferWriteIndex]->GpuHandleSRV);

		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
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
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", AlbedoBuffer->GpuHandleSRV);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
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
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", VelocityBuffer->GpuHandleSRV);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
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
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", RoughnessMetalicBuffer->GpuHandleSRV);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
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
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", SpecularGIRaw->GpuHandleSRV);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
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
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", SpecularGITemporal[GIBufferWriteIndex]->GpuHandleSRV);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
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
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", SpecularGITemporal[GIBufferWriteIndex]->GpuHandleSRV);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	});

	EDebugVisualization FullScreenVisualize = EDebugVisualization::SPECULAR_RAW;

	for (auto& f : functions)
	{
		f(FullscreenDebugBuffer);
	}
}

void Corona::LightingPass()
{
	renderBackend->EmitGpuCrashMarker("LightingPass");
	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::D3D12)
	{
		PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "LightingPass");
	}

	renderBackend->TransitionTexture(LightingBuffer.get(), EResourceState::ShaderRead, EResourceState::RenderTarget);

	glm::mat4x4 InvViewMat = glm::inverse(ViewMat);
	
	// Calculate light color from sky gradient (same as raytracing modes)
	glm::vec3 normalizedLightDir = glm::normalize(LightDir);
	float lightDirT = 0.5f * (normalizedLightDir.y + 1.0f);
	glm::vec3 lightColor = glm::mix(SkyColorBottom, SkyColorTop, lightDirT);
	
	LightingParam Param;
	Param.ViewMatrix = glm::transpose(ViewMat);
	Param.InvViewMatrix = glm::transpose(InvViewMat);
	Param.InvProjMatrix = glm::transpose(InvProjMat);
	Param.LightDir = glm::vec4(normalizedLightDir, LightIntensity);
	
	Param.RTSize.x = GetRenderWidth();
	Param.RTSize.y = GetRenderHeight();

	if (IsTemporalAAEnabled())
		Param.TAABlendFactor = 0.1;
	else
		Param.TAABlendFactor = 1.0;

	Param.GIBufferScale = GIBufferScale;
	Param.LightColor = lightColor;
	Param.bEnableDiffuseGI = bEnableDiffuseGI ? 1 : 0;
	Param.bEnableSpecularGI = bEnableSpecularGI ? 1 : 0;
	Param.bEnableDirectDiffuse = bEnableDirectDiffuse ? 1 : 0;
	Param.bEnableDirectSpecular = bEnableDirectSpecular ? 1 : 0;
	Param.bEnableRTAO = (bEnableRTAO && bRTAOOutputValidThisFrame && AmbientOcclusionBuffer) ? 1 : 0;
	Param.bEnableSkyLighting = (bEnableSkyLighting && bEnableRayTracedSkyLighting && bSkyLightingOutputValidThisFrame && SkyLightingBuffer) ? 1 : 0;
	Param.RTAOIndirectStrength = RTAOIndirectStrength;
	Param.RTAOIndirectFloor = RTAOIndirectFloor;
	Param.SurfaceBounceStrength = std::clamp(SurfaceBounceStrength, 0.0f, 1.0f);
	Param.SurfaceBounceSaturation = std::clamp(SurfaceBounceSaturation, 0.0f, 1.0f);
	Param.SkyLightingStrength = std::clamp(SkyLightingStrength, 0.0f, 1.0f);
	Param.LightingOutputMode = 0;
	Param.PointLightCount = 0;
	for (const PointLightState& pointLight : RenderWorld.PointLights)
	{
		if (!pointLight.bEnabled || Param.PointLightCount >= MaxPointLights)
			continue;

		const UINT32 pointLightIndex = Param.PointLightCount++;
		Param.PointLights[pointLightIndex].PositionAndRadius =
			glm::vec4(pointLight.Position, std::max(pointLight.Radius, 0.01f));
		Param.PointLights[pointLightIndex].ColorAndIntensity =
			glm::vec4(glm::max(pointLight.Color, glm::vec3(0.0f)), std::max(pointLight.Intensity, 0.0f));
	}

	glm::normalize(Param.LightDir);

	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::Vulkan)
	{
		if (!LightingGraphicsPipeline)
			return;

		renderBackend->BindGraphicsPipelineTexture(LightingGraphicsPipeline.get(), "AlbedoTex", AlbedoBuffer.get());
		renderBackend->BindGraphicsPipelineTexture(LightingGraphicsPipeline.get(), "NormalTex", NormalBuffers[ColorBufferWriteIndex].get());
		renderBackend->BindGraphicsPipelineTexture(LightingGraphicsPipeline.get(), "ShadowTex", ShadowDenoisedBuffer ? ShadowDenoisedBuffer.get() : ShadowBuffer.get());
		renderBackend->BindGraphicsPipelineTexture(LightingGraphicsPipeline.get(), "VelocityTex", VelocityBuffer.get());
		renderBackend->BindGraphicsPipelineTexture(LightingGraphicsPipeline.get(), "DepthTex", DepthBuffer.get());
		renderBackend->BindGraphicsPipelineTexture(LightingGraphicsPipeline.get(), "GIResultSHTex", DiffuseGISpatialAux[0].get());
		renderBackend->BindGraphicsPipelineTexture(LightingGraphicsPipeline.get(), "GIResultColorTex", DiffuseGISpatial[0].get());
		renderBackend->BindGraphicsPipelineTexture(LightingGraphicsPipeline.get(), "SpecularGITex", SpecularGISpatial[0].get());
		renderBackend->BindGraphicsPipelineTexture(LightingGraphicsPipeline.get(), "RoughnessMetalicTex", RoughnessMetalicBuffer.get());
		renderBackend->BindGraphicsPipelineTexture(LightingGraphicsPipeline.get(), "AmbientOcclusionTex", AmbientOcclusionBuffer ? AmbientOcclusionBuffer.get() : DefaultWhiteTex.get());
		renderBackend->BindGraphicsPipelineTexture(LightingGraphicsPipeline.get(), "SkyLightingTex", SkyLightingBuffer ? SkyLightingBuffer.get() : DefaultBlackTex.get());
		renderBackend->BindGraphicsPipelineSampler(LightingGraphicsPipeline.get(), "sampleWrap", samplerWrap.get());

		Param.LightingOutputMode = 0;
		renderBackend->SetGraphicsPipelineConstantData(LightingGraphicsPipeline.get(), 0, &Param, sizeof(Param));
		Texture* lightingTarget = LightingBuffer.get();
		renderBackend->SetRenderTargets(&lightingTarget, 1, nullptr);
		renderBackend->BindGraphicsPipeline(LightingGraphicsPipeline.get());
		renderBackend->SetViewportAndScissor(GetRenderWidth(), GetRenderHeight());
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
		renderBackend->TransitionTexture(LightingBuffer.get(), EResourceState::RenderTarget, EResourceState::ShaderRead);

		if (bAutoAADumpEnabled && DirectLightingBuffer)
		{
			Param.LightingOutputMode = 1;
			renderBackend->SetGraphicsPipelineConstantData(LightingGraphicsPipeline.get(), 0, &Param, sizeof(Param));
			renderBackend->TransitionTexture(DirectLightingBuffer.get(), EResourceState::ShaderRead, EResourceState::RenderTarget);
			Texture* directLightingTarget = DirectLightingBuffer.get();
			renderBackend->SetRenderTargets(&directLightingTarget, 1, nullptr);
			renderBackend->BindGraphicsPipeline(LightingGraphicsPipeline.get());
			renderBackend->SetViewportAndScissor(GetRenderWidth(), GetRenderHeight());
			renderBackend->DrawFullscreenQuad(FullScreenVB.get());
			renderBackend->TransitionTexture(DirectLightingBuffer.get(), EResourceState::RenderTarget, EResourceState::ShaderRead);
		}
		return;
	}

	LightingPSO->Apply();

	LightingPSO->SetSampler("samplerWrap", samplerWrap.get());
	LightingPSO->SetSRV("AlbedoTex", AlbedoBuffer->GpuHandleSRV);
	LightingPSO->SetSRV("NormalTex", NormalBuffers[ColorBufferWriteIndex]->GpuHandleSRV);
	LightingPSO->SetSRV("ShadowTex", ShadowDenoisedBuffer ? ShadowDenoisedBuffer->GpuHandleSRV : ShadowBuffer->GpuHandleSRV);

	LightingPSO->SetSRV("VelocityTex", VelocityBuffer->GpuHandleSRV);
	LightingPSO->SetSRV("DepthTex", DepthBuffer->GpuHandleSRV);
	LightingPSO->SetSRV("GIResultSHTex", DiffuseGISpatialAux[0]->GpuHandleSRV);
	LightingPSO->SetSRV("GIResultColorTex", DiffuseGISpatial[0]->GpuHandleSRV);
	LightingPSO->SetSRV("SpecularGITex", SpecularGISpatial[0]->GpuHandleSRV);
	LightingPSO->SetSRV("RoughnessMetalicTex", RoughnessMetalicBuffer->GpuHandleSRV);
	LightingPSO->SetSRV("AmbientOcclusionTex", AmbientOcclusionBuffer ? AmbientOcclusionBuffer->GpuHandleSRV : DefaultWhiteTex->GpuHandleSRV);
	LightingPSO->SetSRV("SkyLightingTex", SkyLightingBuffer ? SkyLightingBuffer->GpuHandleSRV : DefaultBlackTex->GpuHandleSRV);
	LightingPSO->SetCBVValue("LightingParam", &Param);


	LightingPSO->Apply();

	renderBackend->SetRenderTarget(LightingBuffer.get());
	renderBackend->SetViewportAndScissor(GetRenderWidth(), GetRenderHeight());
	renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	renderBackend->TransitionTexture(LightingBuffer.get(), EResourceState::RenderTarget, EResourceState::ShaderRead);

	if (bAutoAADumpEnabled && DirectLightingBuffer)
	{
		Param.LightingOutputMode = 1;
		LightingPSO->SetCBVValue("LightingParam", &Param);
		LightingPSO->Apply();

		renderBackend->TransitionTexture(DirectLightingBuffer.get(), EResourceState::ShaderRead, EResourceState::RenderTarget);
		renderBackend->SetRenderTarget(DirectLightingBuffer.get());
		renderBackend->SetViewportAndScissor(GetRenderWidth(), GetRenderHeight());
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
		renderBackend->TransitionTexture(DirectLightingBuffer.get(), EResourceState::RenderTarget, EResourceState::ShaderRead);
	}
}

void Corona::TemporalAAPass()
{
	renderBackend->EmitGpuCrashMarker("TemporalAAPass");
	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::D3D12)
	{
		PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "TemporalAAPass");
	}

	UINT PrevColorBufferIndex = 1 - ColorBufferWriteIndex;
	Texture* ResolveTarget = ColorBuffers[ColorBufferWriteIndex].get();

	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::Vulkan)
	{
		if (!TemporalAAGraphicsPipeline || !ResolveTarget || !LightingBuffer || !ExposureData)
		{
			bUseLightingBufferFallbackForToneMap = true;
			bTemporalAAHistoryValid = false;
			return;
		}

		Texture* PrevColorBuffer = ColorBuffers[PrevColorBufferIndex].get();
		Texture* BloomTexture = BloomBlurPingPong[0] ? BloomBlurPingPong[0].get() : DefaultBlackTex.get();
		if (!PrevColorBuffer || !BloomTexture)
		{
			bUseLightingBufferFallbackForToneMap = true;
			bTemporalAAHistoryValid = false;
			return;
		}

		TemporalAAParam Param;
		Param.RTSize.x = GetRenderWidth();
		Param.RTSize.y = GetRenderHeight();
		Param.TAABlendFactor = IsTemporalAAEnabled() ? 0.1f : 1.0f;
		Param.ClampMode = ClampMode;
		Param.BloomStrength = BloomBlurPingPong[0] ? BloomStrength : 0.0f;
		Param.HistoryValid = bTemporalAAHistoryValid ? 1u : 0u;
		Param.CurrentJitter = IsJitterEnabled() ? (CurrentJitter * 0.5f) : glm::vec2(0.0f);

		renderBackend->BindGraphicsPipelineTexture(TemporalAAGraphicsPipeline.get(), "CurrentColorTex", LightingBuffer.get());
		renderBackend->BindGraphicsPipelineTexture(TemporalAAGraphicsPipeline.get(), "PrevColorTex", PrevColorBuffer);
		renderBackend->BindGraphicsPipelineTexture(TemporalAAGraphicsPipeline.get(), "VelocityTex", VelocityBuffer.get());
		renderBackend->BindGraphicsPipelineTexture(TemporalAAGraphicsPipeline.get(), "DepthTex", DepthBuffer.get());
		renderBackend->BindGraphicsPipelineTexture(TemporalAAGraphicsPipeline.get(), "BloomTex", BloomTexture);
		renderBackend->BindGraphicsPipelineBuffer(TemporalAAGraphicsPipeline.get(), "Exposure", ExposureData.get());
		renderBackend->BindGraphicsPipelineSampler(TemporalAAGraphicsPipeline.get(), "sampleWrap", samplerBilinearWrap ? samplerBilinearWrap.get() : samplerWrap.get());
		renderBackend->SetGraphicsPipelineConstantData(TemporalAAGraphicsPipeline.get(), 0, &Param, sizeof(Param));

		renderBackend->TransitionTexture(ResolveTarget, EResourceState::ShaderRead, EResourceState::RenderTarget);
		Texture* temporalTarget = ResolveTarget;
		renderBackend->SetRenderTargets(&temporalTarget, 1, nullptr);
		renderBackend->BindGraphicsPipeline(TemporalAAGraphicsPipeline.get());
		renderBackend->SetViewportAndScissor(GetRenderWidth(), GetRenderHeight());
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
		renderBackend->TransitionTexture(ResolveTarget, EResourceState::RenderTarget, EResourceState::ShaderRead);

		bTemporalAAHistoryValid = IsTemporalAAEnabled();
		bUseLightingBufferFallbackForToneMap = false;
		ResolvedColorBufferIndex = ColorBufferWriteIndex;
		return;
	}

	renderBackend->TransitionTexture(ResolveTarget, EResourceState::ShaderRead, EResourceState::RenderTarget);

	TemporalAAPSO->Apply();

	TemporalAAPSO->SetSampler("samplerWrap", samplerBilinearWrap.get());
	TemporalAAPSO->SetSRV("CurrentColorTex", LightingBuffer->GpuHandleSRV);
	Texture* PrevColorBuffer = ColorBuffers[PrevColorBufferIndex].get();
	TemporalAAPSO->SetSRV("PrevColorTex", PrevColorBuffer->GpuHandleSRV);
	TemporalAAPSO->SetSRV("VelocityTex", VelocityBuffer->GpuHandleSRV);
	TemporalAAPSO->SetSRV("DepthTex", DepthBuffer->GpuHandleSRV);
	TemporalAAPSO->SetSRV("BloomTex", BloomBlurPingPong[0]->GpuHandleSRV);
	TemporalAAPSO->SetSRV("Exposure", ExposureData->GpuHandleSRV);

	TemporalAAParam Param;

	Param.RTSize.x = GetRenderWidth();
	Param.RTSize.y = GetRenderHeight();

	if (IsTemporalAAEnabled())
		Param.TAABlendFactor = 0.1;
	else
		Param.TAABlendFactor = 1.0;

	Param.ClampMode = ClampMode;
	Param.BloomStrength = BloomStrength;
	Param.HistoryValid = bTemporalAAHistoryValid ? 1u : 0u;
	Param.CurrentJitter = IsJitterEnabled() ? (CurrentJitter * 0.5f) : glm::vec2(0.0f);

	TemporalAAPSO->SetCBVValue("TemporalAAParam", &Param);
	TemporalAAPSO->Apply();

	renderBackend->SetRenderTarget(ResolveTarget);
	renderBackend->SetViewportAndScissor(static_cast<uint32_t>(m_width), static_cast<uint32_t>(m_height));
	renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	
	renderBackend->TransitionTexture(ResolveTarget, EResourceState::RenderTarget, EResourceState::UnorderedAccess);

	if (bDrawHistogram)
	{
		DrawHistogramPSO->Apply();
		DrawHistogramPSO->SetSRV("Histogram", Histogram->GpuHandleSRV);
		DrawHistogramPSO->SetSRV("Exposure", ExposureData->GpuHandleSRV);
		DrawHistogramPSO->SetUAV("ColorBuffer", ResolveTarget->GpuHandleUAV);
		renderBackend->Dispatch(1, 32, 1);
	}
	renderBackend->TransitionTexture(ResolveTarget, EResourceState::UnorderedAccess, EResourceState::ShaderRead);

	bTemporalAAHistoryValid = IsTemporalAAEnabled();
	bUseLightingBufferFallbackForToneMap = false;
	ResolvedColorBufferIndex = ColorBufferWriteIndex;

}

void Corona::BloomPass()
{
	renderBackend->EmitGpuCrashMarker("BloomPass");
	PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "BloomPass");

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
	renderBackend->TransitionTexture(BloomBlurPingPong[0].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionTexture(LumaBuffer.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	BloomExtractPSO->Apply();
	BloomExtractPSO->SetSRV("SrcTex", LightingBuffer->GpuHandleSRV);
	BloomExtractPSO->SetSRV("Exposure", ExposureData->GpuHandleSRV);
	BloomExtractPSO->SetUAV("DstTex", BloomBlurPingPong[0]->GpuHandleUAV);
	BloomExtractPSO->SetUAV("LumaResult", LumaBuffer->GpuHandleUAV);


	BloomExtractPSO->SetSampler("samplerWrap", samplerWrap.get());

	BloomExtractPSO->SetCBVValue("BloomCB", &BloomCB);

	renderBackend->Dispatch(BloomBufferWidth / 32, BloomBufferHeight / 32, 1);
	renderBackend->TransitionTexture(BloomBlurPingPong[0].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(LumaBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);

	// horizontal pass
	renderBackend->TransitionTexture(BloomBlurPingPong[1].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);


	BloomBlurPSO->Apply();

	BloomBlurPSO->SetSRV("SrcTex", BloomBlurPingPong[0]->GpuHandleSRV);
	BloomBlurPSO->SetUAV("DstTex", BloomBlurPingPong[1]->GpuHandleUAV);


	BloomBlurPSO->SetSampler("samplerWrap", samplerWrap.get());

	BloomCB.BlurDirection = glm::vec2(1, 0);
	BloomBlurPSO->SetCBVValue("BloomCB", &BloomCB);

	renderBackend->Dispatch(BloomBufferWidth / 32, BloomBufferHeight / 32, 1);

	renderBackend->TransitionTexture(BloomBlurPingPong[1].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);


	// vertical pass
	renderBackend->TransitionTexture(BloomBlurPingPong[0].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);


	BloomBlurPSO->Apply();

	BloomBlurPSO->SetSRV("SrcTex", BloomBlurPingPong[1]->GpuHandleSRV);
	BloomBlurPSO->SetUAV("DstTex", BloomBlurPingPong[0]->GpuHandleUAV);


	BloomBlurPSO->SetSampler("samplerWrap", samplerWrap.get());

	BloomCB.BlurDirection = glm::vec2(0, 1);
	BloomBlurPSO->SetCBVValue("BloomCB", &BloomCB);

	renderBackend->Dispatch(BloomBufferWidth / 32, BloomBufferHeight / 32, 1);

	renderBackend->TransitionTexture(BloomBlurPingPong[0].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);

	// histogram pass
	renderBackend->TransitionBuffer(Histogram.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	ClearHistogramPSO->Apply();
	ClearHistogramPSO->SetUAV("Histogram", Histogram->GpuHandleUAV);
	renderBackend->Dispatch(1, 1, 1);

	HistogramPSO->Apply();
	HistogramPSO->SetSRV("LumaTex", LumaBuffer->GpuHandleSRV);
	HistogramPSO->SetUAV("Histogram", Histogram->GpuHandleUAV);
	renderBackend->Dispatch(BloomBufferWidth / 16, 1, 1);

	renderBackend->TransitionBuffer(Histogram.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);


	// adapte exposure pass
	renderBackend->TransitionBuffer(ExposureData.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	AdapteExposurePSO->Apply();
	AdapteExposurePSO->SetSRV("Histogram", Histogram->GpuHandleSRV);
	AdapteExposurePSO->SetUAV("Exposure", ExposureData->GpuHandleUAV);


	AdaptExposureCB.PixelCount = BloomBufferWidth * BloomBufferHeight;
	
	AdapteExposurePSO->SetCBVValue("AdaptExposureCB", &AdaptExposureCB);

	renderBackend->Dispatch(1, 1, 1);
	renderBackend->TransitionBuffer(ExposureData.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);

}

void Corona::DrawScene(shared_ptr<Scene> scene, const glm::mat4x4& instanceTransform, float Roughness, float Metalic, bool bOverrideRoughnessMetallic)
{
	for (auto& mesh : scene->meshes)
	{
		renderBackend->BindMeshBuffers(mesh->Vb.get(), mesh->Ib.get());

		for (int i = 0; i < mesh->Draws.size(); i++)
		{
			Mesh::DrawCall& drawcall = mesh->Draws[i];
			GBufferConstantBuffer objCB = {};
			int sizea = sizeof(GBufferConstantBuffer);

			objCB.ViewProjectionMatrix = glm::transpose(ViewProjMat);
			objCB.PrevViewProjectionMatrix = glm::transpose(PrevViewProjMat);

			//glm::mat4 m; // Identity matrix
			objCB.WorldMatrix = glm::transpose(instanceTransform * mesh->transform);

			objCB.UnjitteredViewProjMat = glm::transpose(UnjitteredViewProjMat);
			objCB.PrevUnjitteredViewProjMat = glm::transpose(PrevUnjitteredViewProjMat);
			objCB.ViewDir.x = m_camera.m_lookDirection.x;
			objCB.ViewDir.y = m_camera.m_lookDirection.y;
			objCB.ViewDir.z = m_camera.m_lookDirection.z;
			objCB.ViewDir.w = 0.0f;
			objCB.BaseColorFactor = drawcall.mat ? drawcall.mat->BaseColorFactor : glm::vec4(1.0f);

			objCB.RTSize.x = GetRenderWidth();
			objCB.RTSize.y = GetRenderHeight();

			objCB.RougnessMetalic.x = Roughness;
			objCB.RougnessMetalic.y = Metalic;

			objCB.bOverrideRougnessMetallic = bOverrideRoughnessMetallic ? 1 : 0;

			renderBackend->SetGraphicsPipelineConstantData(GBufferGraphicsPipeline.get(), 0, &objCB, sizeof(objCB));

			Texture* AlbedoTex = drawcall.mat->Diffuse ? drawcall.mat->Diffuse.get() : DefaultWhiteTex.get();
			Texture* NormalTex = drawcall.mat->Normal ? drawcall.mat->Normal.get() : DefaultNormalTex.get();
			Texture* RoughnessTex = drawcall.mat->Roughness ? drawcall.mat->Roughness.get() : DefaultRougnessTex.get();
			Texture* MetallicTex = drawcall.mat->Metallic ? drawcall.mat->Metallic.get() : DefaultBlackTex.get();

			renderBackend->BindGraphicsPipelineTexture(GBufferGraphicsPipeline.get(), "AlbedoTex", AlbedoTex);
			renderBackend->BindGraphicsPipelineTexture(GBufferGraphicsPipeline.get(), "NormalTex", NormalTex);
			renderBackend->BindGraphicsPipelineTexture(GBufferGraphicsPipeline.get(), "RoughnessTex", RoughnessTex);
			renderBackend->BindGraphicsPipelineTexture(GBufferGraphicsPipeline.get(), "MetallicTex", MetallicTex);


			renderBackend->DrawIndexed(drawcall.IndexCount, drawcall.IndexStart, drawcall.VertexBase);
		}
	}
}

void Corona::GBufferPass()
{
	ColorBufferWriteIndex = 1 - ColorBufferWriteIndex;
	//DepthBufferWriteIndex = 1 - DepthBufferWriteIndex;
	renderBackend->EmitGpuCrashMarker("GBufferPass");
	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::D3D12)
	{
		PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "GBufferPass");
	}

	renderBackend->TransitionTexture(AlbedoBuffer.get(), EResourceState::ShaderRead, EResourceState::RenderTarget);
	renderBackend->TransitionTexture(SpecularAlbedoBuffer.get(), EResourceState::ShaderRead, EResourceState::RenderTarget);
	renderBackend->TransitionTexture(NormalBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead, EResourceState::RenderTarget);
	renderBackend->TransitionTexture(GeomNormalBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead, EResourceState::RenderTarget);
	renderBackend->TransitionTexture(VelocityBuffer.get(), EResourceState::ShaderRead, EResourceState::RenderTarget);
	renderBackend->TransitionTexture(RoughnessMetalicBuffer.get(), EResourceState::ShaderRead, EResourceState::RenderTarget);

	renderBackend->TransitionTexture(DepthBuffer.get(), EResourceState::ShaderRead, EResourceState::DepthWrite);
	renderBackend->TransitionTexture(UnjitteredDepthBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead, EResourceState::RenderTarget);

	const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
	renderBackend->ClearRenderTarget(AlbedoBuffer.get(), clearColor);
	renderBackend->ClearRenderTarget(SpecularAlbedoBuffer.get(), clearColor);
	const float normalClearColor[] = { 0.0f, -0.1f, 0.0f, 0.0f };
	renderBackend->ClearRenderTarget(NormalBuffers[ColorBufferWriteIndex].get(), normalClearColor);
	renderBackend->ClearRenderTarget(GeomNormalBuffers[ColorBufferWriteIndex].get(), normalClearColor);
	const float velocityClearColor[] = { 0.0f, 0.0f};
	renderBackend->ClearRenderTarget(VelocityBuffer.get(), velocityClearColor);
	const float roughnessClearColor[] = { 0.001f, 0.0f, 0.0f, 0.0f };
	renderBackend->ClearRenderTarget(RoughnessMetalicBuffer.get(), roughnessClearColor);

	renderBackend->ClearDepth(DepthBuffer.get(), 1.0f);
	const float ujitteredDepthClearColor[] = { 1.0f, 1.0f, 1.0f, 1.0f};
	renderBackend->ClearRenderTarget(UnjitteredDepthBuffers[ColorBufferWriteIndex].get(), ujitteredDepthClearColor);


	renderBackend->BindDefaultDescriptorHeaps();

	renderBackend->SetViewportAndScissor(GetRenderWidth(), GetRenderHeight());
	Texture* renderTargets[] = {
		AlbedoBuffer.get(),
		SpecularAlbedoBuffer.get(),
		NormalBuffers[ColorBufferWriteIndex].get(),
		GeomNormalBuffers[ColorBufferWriteIndex].get(),
		VelocityBuffer.get(),
		RoughnessMetalicBuffer.get(),
		UnjitteredDepthBuffers[ColorBufferWriteIndex].get()
	};
	renderBackend->SetRenderTargets(renderTargets, static_cast<uint32_t>(std::size(renderTargets)), DepthBuffer.get());

	renderBackend->BindGraphicsPipeline(GBufferGraphicsPipeline.get());
	renderBackend->BindGraphicsPipelineSampler(GBufferGraphicsPipeline.get(), "samplerWrap", samplerWrap.get());

	if (!bMultiThreadRendering)
	{
		for (const SceneObject& object : RenderWorld.SceneObjects)
		{
			if (!object.bVisible || !object.ScenePtr)
				continue;
			DrawScene(
				object.ScenePtr,
				object.Transform,
				object.Roughness,
				object.Metallic,
				object.bOverrideRoughnessMetallic);
		}
	}
	else
	{
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
	}
	
	renderBackend->TransitionTexture(AlbedoBuffer.get(), EResourceState::RenderTarget, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(SpecularAlbedoBuffer.get(), EResourceState::RenderTarget, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(NormalBuffers[ColorBufferWriteIndex].get(), EResourceState::RenderTarget, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(GeomNormalBuffers[ColorBufferWriteIndex].get(), EResourceState::RenderTarget, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(VelocityBuffer.get(), EResourceState::RenderTarget, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(RoughnessMetalicBuffer.get(), EResourceState::RenderTarget, EResourceState::ShaderRead);


	renderBackend->TransitionTexture(DepthBuffer.get(), EResourceState::DepthWrite, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(UnjitteredDepthBuffers[ColorBufferWriteIndex].get(), EResourceState::RenderTarget, EResourceState::ShaderRead);
}
