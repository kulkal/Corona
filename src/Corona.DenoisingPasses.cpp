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

void Corona::InitSpatialDenoisingPass()
{
	shared_ptr<ComputePipelineStateObject> TEMP_SpatialDenoisingFilterPSO = renderBackend->CreateComputePipelineStateObject();
	if (!TEMP_SpatialDenoisingFilterPSO)
		return;
	TEMP_SpatialDenoisingFilterPSO->BindSRV("DepthTex", 0, 1);
	TEMP_SpatialDenoisingFilterPSO->BindSRV("GeoNormalTex", 1, 1);
	TEMP_SpatialDenoisingFilterPSO->BindSRV("InGIResultSHTex", 2, 1);
	TEMP_SpatialDenoisingFilterPSO->BindSRV("InGIResultColorTex", 3, 1);
	TEMP_SpatialDenoisingFilterPSO->BindSRV("InSpecularGITex", 4, 1);
	
	
	TEMP_SpatialDenoisingFilterPSO->BindUAV("OutGIResultSH", 0);
	TEMP_SpatialDenoisingFilterPSO->BindUAV("OutGIResultColor", 1);
	TEMP_SpatialDenoisingFilterPSO->BindUAV("OutSpecularGI", 2);
	
	
	TEMP_SpatialDenoisingFilterPSO->BindCBV("SpatialFilterConstant", 0, sizeof(SpatialFilterConstant));
	bool bSuccess = TEMP_SpatialDenoisingFilterPSO->InitCS(GetAssetFullPath(L"Shaders\\SpatialDenoising.hlsl"), "SpatialFilter");
	if (bSuccess)
		SpatialDenoisingFilterPSO = TEMP_SpatialDenoisingFilterPSO;

	UINT WidthGI = GetRenderWidth();
	UINT HeightGI = GetRenderHeight();
	const ETextureFormat HybridFloat4UAVFormat =
		(renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::Vulkan)
		? ETextureFormat::RGBA32Float
		: ETextureFormat::RGBA16Float;

	DiffuseGISpatialAux[0] = renderBackend->CreateTexture2D({ HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, EInitialResourceState::ShaderRead, (int)WidthGI, (int)HeightGI, 1, std::nullopt });

	NAME_D3D12_OBJECT(DiffuseGISpatialAux[0]->resource);

	DiffuseGISpatialAux[1] = renderBackend->CreateTexture2D({ HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, EInitialResourceState::ShaderRead, (int)WidthGI, (int)HeightGI, 1, std::nullopt });

	NAME_D3D12_OBJECT(DiffuseGISpatialAux[1]->resource);

	DiffuseGISpatial[0] = renderBackend->CreateTexture2D({ HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, EInitialResourceState::ShaderRead, (int)WidthGI, (int)HeightGI, 1, std::nullopt });

	NAME_D3D12_OBJECT(DiffuseGISpatial[0]->resource);

	DiffuseGISpatial[1] = renderBackend->CreateTexture2D({ HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, EInitialResourceState::ShaderRead, (int)WidthGI, (int)HeightGI, 1, std::nullopt });

	NAME_D3D12_OBJECT(DiffuseGISpatial[1]->resource);

	SpecularGISpatial[0] = renderBackend->CreateTexture2D({ HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, EInitialResourceState::ShaderRead, (int)WidthGI, (int)HeightGI, 1, std::nullopt });

	NAME_D3D12_OBJECT(SpecularGISpatial[0]->resource);

	SpecularGISpatial[1] = renderBackend->CreateTexture2D({ HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, EInitialResourceState::ShaderRead, (int)WidthGI, (int)HeightGI, 1, std::nullopt });

	NAME_D3D12_OBJECT(SpecularGISpatial[1]->resource);
}

void Corona::InitTemporalDenoisingPass()
{
	shared_ptr<ComputePipelineStateObject> TEMP_TemporalDenoisingFilterPSO = renderBackend->CreateComputePipelineStateObject();
	if (!TEMP_TemporalDenoisingFilterPSO)
		return;
	TEMP_TemporalDenoisingFilterPSO->BindSRV("DepthTex", 0, 1);
	TEMP_TemporalDenoisingFilterPSO->BindSRV("NormalTex", 1, 1);
	TEMP_TemporalDenoisingFilterPSO->BindSRV("InGIResultSHTex", 2, 1);
	TEMP_TemporalDenoisingFilterPSO->BindSRV("InGIResultColorTex", 3, 1);
	TEMP_TemporalDenoisingFilterPSO->BindSRV("InGIResultSHTexPrev", 4, 1);
	TEMP_TemporalDenoisingFilterPSO->BindSRV("InGIResultColorTexPrev", 5, 1);
	TEMP_TemporalDenoisingFilterPSO->BindSRV("VelocityTex", 6, 1);
	TEMP_TemporalDenoisingFilterPSO->BindSRV("InSpecularGITex", 7, 1);
	TEMP_TemporalDenoisingFilterPSO->BindSRV("InSpecularGITexPrev", 8, 1);
	TEMP_TemporalDenoisingFilterPSO->BindSRV("RougnessMetalicTex", 9, 1);
	TEMP_TemporalDenoisingFilterPSO->BindSRV("PrevDepthTex", 10, 1);
	TEMP_TemporalDenoisingFilterPSO->BindSRV("PrevNormalTex", 11, 1);
	TEMP_TemporalDenoisingFilterPSO->BindSRV("PrevMomentsTex", 12, 1);





	TEMP_TemporalDenoisingFilterPSO->BindUAV("OutGIResultSH", 0);
	TEMP_TemporalDenoisingFilterPSO->BindUAV("OutGIResultColor", 1);
	TEMP_TemporalDenoisingFilterPSO->BindUAV("OutGIResultSHDS", 2);
	TEMP_TemporalDenoisingFilterPSO->BindUAV("OutGIResultColorDS", 3);
	TEMP_TemporalDenoisingFilterPSO->BindUAV("OutSpecularGI", 4);
	TEMP_TemporalDenoisingFilterPSO->BindUAV("OutMoments", 5);

	//TemporalDenoisingFilterPSO->BindUAV("OutSpecularGIDS", 5);

	TEMP_TemporalDenoisingFilterPSO->BindSampler("BilinearClamp", 0);


	TEMP_TemporalDenoisingFilterPSO->BindCBV("TemporalFilterConstant", 0, sizeof(TemporalFilterConstant));
	bool bSuccess = TEMP_TemporalDenoisingFilterPSO->InitCS(GetAssetFullPath(L"Shaders\\TemporalDenoising.hlsl"), "TemporalFilter");
	if (bSuccess)
		TemporalDenoisingFilterPSO = TEMP_TemporalDenoisingFilterPSO;
}

void Corona::InitShadowDenoisePass()
{
	shared_ptr<ComputePipelineStateObject> tempPSO = renderBackend->CreateComputePipelineStateObject();
	if (!tempPSO)
		return;
	tempPSO->BindSRV("ShadowTex", 0, 1);
	tempPSO->BindSRV("DepthTex", 1, 1);
	tempPSO->BindSRV("GeoNormalTex", 2, 1);
	tempPSO->BindUAV("OutShadow", 0);
	tempPSO->BindCBV("ShadowDenoiseCB", 0, sizeof(ShadowDenoiseCB));
	if (tempPSO->InitCS(GetAssetFullPath(L"Shaders\\ShadowDenoise.hlsl"), "ShadowDenoiseCS"))
		ShadowDenoisePSO = tempPSO;
}

void Corona::InitSkyLightingDenoisePass()
{
	shared_ptr<ComputePipelineStateObject> tempPSO = renderBackend->CreateComputePipelineStateObject();
	if (!tempPSO)
		return;

	tempPSO->BindSRV("SkyLightingRawTex", 0, 1);
	tempPSO->BindSRV("DepthTex", 1, 1);
	tempPSO->BindSRV("GeoNormalTex", 2, 1);
	tempPSO->BindUAV("OutSkyLighting", 0);
	tempPSO->BindCBV("SkyLightingDenoiseCB", 0, sizeof(SkyLightingDenoiseCB));
	if (tempPSO->InitCS(GetAssetFullPath(L"Shaders\\SkyLightingDenoise.hlsl"), "SkyLightingDenoiseCS"))
		SkyLightingDenoisePSO = tempPSO;
}

void Corona::SpatialDenoisingPass()
{
	if (!SpatialDenoisingFilterPSO)
		return;
	renderBackend->EmitGpuCrashMarker("SpatialDenoisingPass");
	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::D3D12)
	{
		PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "SpatialDenoisingPass");
	}

	UINT WriteIndex = 0;
	UINT ReadIndex = 1;
	SpatialFilterCB.ProjectionParams = FrameProjectionParams;
	SpatialFilterCB.AccumulatedFrames = IndirectAccumulatedFrames;
	for (int i = 0; i < 4; i++)
	{
		WriteIndex = 1 - WriteIndex; // 1
		ReadIndex = 1 - WriteIndex; // 0

		renderBackend->TransitionTexture(DiffuseGISpatialAux[WriteIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->TransitionTexture(DiffuseGISpatial[WriteIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->TransitionTexture(SpecularGISpatial[WriteIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

		SpatialDenoisingFilterPSO->SetTextureSRV("DepthTex", UnjitteredDepthBuffers[ColorBufferWriteIndex].get());
		SpatialDenoisingFilterPSO->SetTextureSRV("GeoNormalTex", GeomNormalBuffers[ColorBufferWriteIndex].get());
		if (i == 0)
		{
			Texture* diffuseSpatialInput =
				(DiffuseGIMode == EDiffuseGIMode::SCREEN_PROBE && ScreenProbeGIResolved) ?
				ScreenProbeGIResolved.get() :
				DiffuseGITemporal[GIBufferWriteIndex].get();
			SpatialDenoisingFilterPSO->SetTextureSRV("InGIResultSHTex", DiffuseGITemporalAux[GIBufferWriteIndex].get());
			SpatialDenoisingFilterPSO->SetTextureSRV("InGIResultColorTex", diffuseSpatialInput);
			SpatialDenoisingFilterPSO->SetTextureSRV("InSpecularGITex", SpecularGITemporal[GIBufferWriteIndex].get());
		}
		else
		{
			SpatialDenoisingFilterPSO->SetTextureSRV("InGIResultSHTex", DiffuseGISpatialAux[ReadIndex].get());
			SpatialDenoisingFilterPSO->SetTextureSRV("InGIResultColorTex", DiffuseGISpatial[ReadIndex].get());
			SpatialDenoisingFilterPSO->SetTextureSRV("InSpecularGITex", SpecularGISpatial[ReadIndex].get());
		}


		SpatialDenoisingFilterPSO->SetTextureUAV("OutGIResultSH", DiffuseGISpatialAux[WriteIndex].get());
		SpatialDenoisingFilterPSO->SetTextureUAV("OutGIResultColor", DiffuseGISpatial[WriteIndex].get());
		SpatialDenoisingFilterPSO->SetTextureUAV("OutSpecularGI", SpecularGISpatial[WriteIndex].get());

		SpatialFilterCB.Iteration = i;
		SpatialDenoisingFilterPSO->SetCBVValue("SpatialFilterConstant", &SpatialFilterCB);
		SpatialDenoisingFilterPSO->Apply();

		UINT WidthGI = GetRenderWidth();
		UINT HeightGI = GetRenderHeight();

		renderBackend->Dispatch((WidthGI + 31) / 32, (HeightGI + 31) / 32, 1);

		renderBackend->TransitionTexture(DiffuseGISpatialAux[WriteIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
		renderBackend->TransitionTexture(DiffuseGISpatial[WriteIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
		renderBackend->TransitionTexture(SpecularGISpatial[WriteIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	}
}

void Corona::TemporalDenoisingPass()
{
	if (!TemporalDenoisingFilterPSO)
		return;
	renderBackend->EmitGpuCrashMarker("TemporalDenoisingPass");
	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::D3D12)
	{
		PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "TemporalDenoisingPass");
	}

	// GIBufferSH : full scale
	// FilterIndirectDiffusePingPongSH : 3x3 downsample
	GIBufferWriteIndex = 1 - GIBufferWriteIndex;
	UINT WriteIndex = GIBufferWriteIndex;
	UINT ReadIndex = 1 - WriteIndex;

	// first pass
	renderBackend->TransitionTexture(DiffuseGISpatialAux[0].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionTexture(DiffuseGISpatial[0].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionTexture(DiffuseGITemporalAux[WriteIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionTexture(DiffuseGITemporal[WriteIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionTexture(SpecularGITemporal[WriteIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionTexture(SpecularGIMoments[WriteIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	TemporalDenoisingFilterPSO->SetTextureSRV("DepthTex", UnjitteredDepthBuffers[ColorBufferWriteIndex].get());
	TemporalDenoisingFilterPSO->SetTextureSRV("NormalTex", GeomNormalBuffers[ColorBufferWriteIndex].get());
	const bool bUseSpatialHashDiffuseInput =
		DiffuseGIMode == EDiffuseGIMode::SPATIAL_HASH &&
		bSpatialHashGIHistoryValid &&
		DiffuseGIHashCached &&
		DiffuseGIHashCachedAux;
	Texture* temporalDiffuseInputAux =
		bUseSpatialHashDiffuseInput ?
		DiffuseGIHashCachedAux.get() :
		DiffuseGIRawAux.get();
	Texture* temporalDiffuseInput =
		bUseSpatialHashDiffuseInput ?
		DiffuseGIHashCached.get() :
		DiffuseGIRaw.get();
	TemporalDenoisingFilterPSO->SetTextureSRV("InGIResultSHTex", temporalDiffuseInputAux);
	TemporalDenoisingFilterPSO->SetTextureSRV("InGIResultColorTex", temporalDiffuseInput);
	TemporalDenoisingFilterPSO->SetTextureSRV("InGIResultSHTexPrev", DiffuseGITemporalAux[ReadIndex].get());
	TemporalDenoisingFilterPSO->SetTextureSRV("InGIResultColorTexPrev", DiffuseGITemporal[ReadIndex].get());
	TemporalDenoisingFilterPSO->SetTextureSRV("VelocityTex", VelocityBuffer.get());
	TemporalDenoisingFilterPSO->SetTextureSRV("InSpecularGITex", SpecularGIRaw.get());
	TemporalDenoisingFilterPSO->SetTextureSRV("InSpecularGITexPrev", SpecularGITemporal[ReadIndex].get());
	TemporalDenoisingFilterPSO->SetTextureSRV("RougnessMetalicTex", RoughnessMetalicBuffer.get());
	TemporalDenoisingFilterPSO->SetTextureSRV("PrevDepthTex", UnjitteredDepthBuffers[1 - ColorBufferWriteIndex].get());
	TemporalDenoisingFilterPSO->SetTextureSRV("PrevNormalTex", GeomNormalBuffers[1 - ColorBufferWriteIndex].get());
	TemporalDenoisingFilterPSO->SetTextureSRV("PrevMomentsTex", SpecularGIMoments[ReadIndex].get());


	TemporalDenoisingFilterPSO->SetTextureUAV("OutGIResultSH", DiffuseGITemporalAux[WriteIndex].get());
	TemporalDenoisingFilterPSO->SetTextureUAV("OutGIResultColor", DiffuseGITemporal[WriteIndex].get());
	TemporalDenoisingFilterPSO->SetTextureUAV("OutGIResultSHDS", DiffuseGISpatialAux[0].get());
	TemporalDenoisingFilterPSO->SetTextureUAV("OutGIResultColorDS", DiffuseGISpatial[0].get());
	TemporalDenoisingFilterPSO->SetTextureUAV("OutSpecularGI", SpecularGITemporal[WriteIndex].get());
	TemporalDenoisingFilterPSO->SetTextureUAV("OutMoments", SpecularGIMoments[WriteIndex].get());
	//TemporalDenoisingFilterPSO->SetUAV("OutSpecularGIDS", SpecularGISpatial[0]->GpuHandleUAV, renderBackend->GetGraphicsCommandList());

	TemporalDenoisingFilterPSO->SetSampler("BilinearClamp", samplerBilinearWrap.get());
	TemporalFilterCB.InvViewMatrix = glm::transpose(InvViewMat);
	TemporalFilterCB.InvProjMatrix = glm::transpose(InvProjMat);
	TemporalFilterCB.ProjectionParams = FrameProjectionParams;
	TemporalFilterCB.RTSize = glm::vec2(GetRenderWidth(), GetRenderHeight());
	TemporalFilterCB.FrameIndex = RenderFrameIndex;
	TemporalFilterCB.AccumulationAlpha = bTemporalDenoiserHistoryValid ? (1.0f / float(std::min(IndirectAccumulatedFrames + 1u, 32u))) : 1.0f;
	TemporalFilterCB.SpecularAccumulationAlpha = bTemporalDenoiserHistoryValid ? (1.0f / float(std::min(IndirectAccumulatedFrames + 1u, 64u))) : 1.0f;
	TemporalFilterCB.JitterOffset = IsJitterEnabled() ? JitterOffset : glm::vec2(0.0f);
	TemporalFilterCB.HistoryValid = bTemporalDenoiserHistoryValid ? 1u : 0u;

	TemporalDenoisingFilterPSO->SetCBVValue("TemporalFilterConstant", &TemporalFilterCB);
	TemporalDenoisingFilterPSO->Apply();

	renderBackend->Dispatch((GetRenderWidth() + 14) / 15, (GetRenderHeight() + 14) / 15, 1);

	renderBackend->TransitionTexture(DiffuseGISpatialAux[0].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(DiffuseGISpatial[0].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(DiffuseGITemporalAux[WriteIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(DiffuseGITemporal[WriteIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(SpecularGITemporal[WriteIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(SpecularGIMoments[WriteIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	bTemporalDenoiserHistoryValid = true;
	IndirectAccumulatedFrames = std::min(IndirectAccumulatedFrames + 1u, 1024u);
}

void Corona::ShadowDenoisePass()
{
	if (!ShadowDenoisePSO || !ShadowBuffer || !ShadowDenoisedBuffer)
		return;

	renderBackend->EmitGpuCrashMarker("ShadowDenoisePass");

	renderBackend->TransitionTexture(ShadowDenoisedBuffer.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	ShadowDenoisePSO->SetTextureSRV("ShadowTex", ShadowBuffer.get());
	ShadowDenoisePSO->SetTextureSRV("DepthTex", DepthBuffer.get());
	ShadowDenoisePSO->SetTextureSRV("GeoNormalTex", GeomNormalBuffers[ColorBufferWriteIndex].get());
	ShadowDenoiseParam.ProjectionParams = RTShadowViewParam.ProjectionParams;
	ShadowDenoiseParam.RTSize = glm::vec2(GetRenderWidth(), GetRenderHeight());
	ShadowDenoisePSO->SetCBVValue("ShadowDenoiseCB", &ShadowDenoiseParam);
	ShadowDenoisePSO->SetTextureUAV("OutShadow", ShadowDenoisedBuffer.get());
	ShadowDenoisePSO->Apply();
	renderBackend->Dispatch((GetRenderWidth() + 7) / 8, (GetRenderHeight() + 7) / 8, 1);

	renderBackend->TransitionTexture(ShadowDenoisedBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
}

void Corona::SkyLightingDenoisePass()
{
	if (!SkyLightingDenoisePSO || !SkyLightingRawBuffer || !SkyLightingBuffer || !UnjitteredDepthBuffers[ColorBufferWriteIndex] || !GeomNormalBuffers[ColorBufferWriteIndex])
		return;

	renderBackend->EmitGpuCrashMarker("SkyLightingDenoisePass");

	renderBackend->TransitionTexture(SkyLightingBuffer.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	SkyLightingDenoisePSO->SetTextureSRV("SkyLightingRawTex", SkyLightingRawBuffer.get());
	SkyLightingDenoisePSO->SetTextureSRV("DepthTex", UnjitteredDepthBuffers[ColorBufferWriteIndex].get());
	SkyLightingDenoisePSO->SetTextureSRV("GeoNormalTex", GeomNormalBuffers[ColorBufferWriteIndex].get());
	SkyLightingDenoiseParam.ProjectionParams = RTSkyLightingViewParam.ProjectionParams;
	SkyLightingDenoiseParam.RTSize = glm::vec2(GetRenderWidth(), GetRenderHeight());
	SkyLightingDenoiseParam.DepthSigma = std::clamp(SkyLightingDenoiseParam.DepthSigma, 1.0f, 192.0f);
	SkyLightingDenoiseParam.NormalSigma = std::clamp(SkyLightingDenoiseParam.NormalSigma, 1.0f, 192.0f);
	SkyLightingDenoiseParam.VisibilitySigma = std::clamp(SkyLightingDenoiseParam.VisibilitySigma, 0.0f, 32.0f);
	SkyLightingDenoiseParam.Radius = std::clamp(SkyLightingDenoiseParam.Radius, 1u, 6u);
	SkyLightingDenoisePSO->SetCBVValue("SkyLightingDenoiseCB", &SkyLightingDenoiseParam);
	SkyLightingDenoisePSO->SetTextureUAV("OutSkyLighting", SkyLightingBuffer.get());
	SkyLightingDenoisePSO->Apply();
	renderBackend->Dispatch((GetRenderWidth() + 7) / 8, (GetRenderHeight() + 7) / 8, 1);

	renderBackend->TransitionTexture(SkyLightingBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
}
