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
	TEMP_TemporalDenoisingFilterPSO->BindUAV("OutSpecularGI", 2);
	TEMP_TemporalDenoisingFilterPSO->BindUAV("OutMoments", 3);

	TEMP_TemporalDenoisingFilterPSO->BindSampler("BilinearClamp", 0);


	TEMP_TemporalDenoisingFilterPSO->BindCBV("TemporalFilterConstant", 0, sizeof(TemporalFilterConstant));
	bool bSuccess = TEMP_TemporalDenoisingFilterPSO->InitCS(GetAssetFullPath(L"Shaders\\TemporalDenoising.hlsl"), "TemporalFilter");
	if (bSuccess)
		TemporalDenoisingFilterPSO = TEMP_TemporalDenoisingFilterPSO;
}

void Corona::TemporalDenoisingPass()
{
	if (!TemporalDenoisingFilterPSO)
		return;
	renderBackend->EmitGpuCrashMarker("TemporalDenoisingPass");

	GIBufferWriteIndex = 1 - GIBufferWriteIndex;
	UINT WriteIndex = GIBufferWriteIndex;
	UINT ReadIndex = 1 - WriteIndex;

	// first pass
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
	TemporalDenoisingFilterPSO->SetTextureUAV("OutSpecularGI", SpecularGITemporal[WriteIndex].get());
	TemporalDenoisingFilterPSO->SetTextureUAV("OutMoments", SpecularGIMoments[WriteIndex].get());

	TemporalDenoisingFilterPSO->SetSampler("BilinearClamp", samplerBilinearWrap.get());
	TemporalFilterCB.InvViewMatrix = glm::transpose(InvViewMat);
	TemporalFilterCB.InvProjMatrix = glm::transpose(InvProjMat);
	TemporalFilterCB.PrevUnjitteredViewProjMatrix = glm::transpose(PrevUnjitteredViewProjMat);
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

	renderBackend->TransitionTexture(DiffuseGITemporalAux[WriteIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(DiffuseGITemporal[WriteIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(SpecularGITemporal[WriteIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(SpecularGIMoments[WriteIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	bTemporalDenoiserHistoryValid = true;
	IndirectAccumulatedFrames = std::min(IndirectAccumulatedFrames + 1u, 1024u);
}

