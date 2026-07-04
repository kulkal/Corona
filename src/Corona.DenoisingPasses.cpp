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
#include "RenderGraph.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>

namespace
{
	RGTextureRef ImportTextureIfValid(RenderGraph& rg, const char* name, Texture* texture, EResourceState state = EResourceState::ShaderRead)
	{
		return texture ? rg.ImportTexture(name, texture, state) : RGTextureRef{};
	}
}

void Corona::InitTemporalDenoisingPass()
{
	shared_ptr<ComputePipelineStateObject> TEMP_TemporalDenoisingFilterPSO = renderBackend->CreateComputePipelineStateObject();
	if (!TEMP_TemporalDenoisingFilterPSO)
		return;
	const RHIShaderStageMask computeStage = ToRHIShaderStageMask(RHIShaderStage::Compute);
	TEMP_TemporalDenoisingFilterPSO->BindSRV(MakeRHITextureSRV("DepthTex", 0, computeStage));
	TEMP_TemporalDenoisingFilterPSO->BindSRV(MakeRHITextureSRV("NormalTex", 1, computeStage));
	TEMP_TemporalDenoisingFilterPSO->BindSRV(MakeRHITextureSRV("InGIResultSHTex", 2, computeStage));
	TEMP_TemporalDenoisingFilterPSO->BindSRV(MakeRHITextureSRV("InGIResultColorTex", 3, computeStage));
	TEMP_TemporalDenoisingFilterPSO->BindSRV(MakeRHITextureSRV("InGIResultSHTexPrev", 4, computeStage));
	TEMP_TemporalDenoisingFilterPSO->BindSRV(MakeRHITextureSRV("InGIResultColorTexPrev", 5, computeStage));
	TEMP_TemporalDenoisingFilterPSO->BindSRV(MakeRHITextureSRV("VelocityTex", 6, computeStage));
	TEMP_TemporalDenoisingFilterPSO->BindSRV(MakeRHITextureSRV("InSpecularGITex", 7, computeStage));
	TEMP_TemporalDenoisingFilterPSO->BindSRV(MakeRHITextureSRV("InSpecularGITexPrev", 8, computeStage));
	TEMP_TemporalDenoisingFilterPSO->BindSRV(MakeRHITextureSRV("RougnessMetalicTex", 9, computeStage));
	TEMP_TemporalDenoisingFilterPSO->BindSRV(MakeRHITextureSRV("PrevDepthTex", 10, computeStage));
	TEMP_TemporalDenoisingFilterPSO->BindSRV(MakeRHITextureSRV("PrevNormalTex", 11, computeStage));
	TEMP_TemporalDenoisingFilterPSO->BindSRV(MakeRHITextureSRV("PrevMomentsTex", 12, computeStage));





	TEMP_TemporalDenoisingFilterPSO->BindUAV(MakeRHITextureUAV("OutGIResultSH", 0, computeStage));
	TEMP_TemporalDenoisingFilterPSO->BindUAV(MakeRHITextureUAV("OutGIResultColor", 1, computeStage));
	TEMP_TemporalDenoisingFilterPSO->BindUAV(MakeRHITextureUAV("OutSpecularGI", 2, computeStage));
	TEMP_TemporalDenoisingFilterPSO->BindUAV(MakeRHITextureUAV("OutMoments", 3, computeStage));

	TEMP_TemporalDenoisingFilterPSO->BindSampler(MakeRHISampler("BilinearClamp", 0, computeStage));


	TEMP_TemporalDenoisingFilterPSO->BindCBV(MakeRHICBV("TemporalFilterConstant", 0, sizeof(TemporalFilterConstant), computeStage));
	bool bSuccess = TEMP_TemporalDenoisingFilterPSO->InitCS(GetAssetFullPath(L"Shaders\\TemporalDenoising.hlsl"), "TemporalFilter");
	if (bSuccess)
		TemporalDenoisingFilterPSO = TEMP_TemporalDenoisingFilterPSO;
}

void Corona::InitDiffuseGISpatialFilterPass()
{
	shared_ptr<ComputePipelineStateObject> tempPSO = renderBackend->CreateComputePipelineStateObject();
	if (!tempPSO)
		return;
	const RHIShaderStageMask computeStage = ToRHIShaderStageMask(RHIShaderStage::Compute);
	tempPSO->BindSRV(MakeRHITextureSRV("InGIColor", 0, computeStage));
	tempPSO->BindSRV(MakeRHITextureSRV("InGIAux", 1, computeStage));
	tempPSO->BindSRV(MakeRHITextureSRV("DepthTex", 2, computeStage));
	tempPSO->BindSRV(MakeRHITextureSRV("NormalTex", 3, computeStage));
	tempPSO->BindUAV(MakeRHITextureUAV("OutGIColor", 0, computeStage));
	tempPSO->BindUAV(MakeRHITextureUAV("OutGIAux", 1, computeStage));
	tempPSO->BindCBV(MakeRHICBV("SpatialFilterConstant", 0, sizeof(DiffuseGISpatialFilterConstant), computeStage));
	if (tempPSO->InitCS(GetAssetFullPath(L"Shaders\\DiffuseGISpatialFilter.hlsl"), "DiffuseGISpatialFilter"))
		DiffuseGISpatialFilterPSO = tempPSO;
}

// Post-temporal, variance-guided disocclusion filter. Runs AFTER TemporalDenoisingPass:
// reads the temporally-accumulated diffuse GI and cleans only the high-variance
// (freshly-disoccluded, no-history) pixels before they reach the DLSS-RR combined feed.
// Reuses the DiffuseGISpatialFilterPSO (same shader) but takes the temporal output as
// input instead of the raw GI.
void Corona::DiffuseGIDisocclusionFilterPass()
{
	if (!DiffuseGISpatialFilterPSO ||
		!DiffuseGITemporal[GIBufferWriteIndex] || !DiffuseGITemporalAux[GIBufferWriteIndex] ||
		!DiffuseGISpatialFiltered || !DiffuseGISpatialFilteredAux)
		return;

	RenderGraph rg(renderBackend.get());
	RGTextureRef inColor = rg.ImportTexture("DiffuseGI.DisocclusionFilter.InColor", DiffuseGITemporal[GIBufferWriteIndex].get(), EResourceState::ShaderRead);
	RGTextureRef inAux = rg.ImportTexture("DiffuseGI.DisocclusionFilter.InAux", DiffuseGITemporalAux[GIBufferWriteIndex].get(), EResourceState::ShaderRead);
	RGTextureRef depth = ImportTextureIfValid(rg, "DiffuseGI.DisocclusionFilter.Depth", UnjitteredDepthBuffers[ColorBufferWriteIndex].get());
	RGTextureRef normal = ImportTextureIfValid(rg, "DiffuseGI.DisocclusionFilter.Normal", GeomNormalBuffers[ColorBufferWriteIndex].get());
	RGTextureRef outColor = rg.ImportTexture("DiffuseGI.DisocclusionFilter.OutColor", DiffuseGISpatialFiltered.get(), EResourceState::ShaderRead);
	RGTextureRef outAux = rg.ImportTexture("DiffuseGI.DisocclusionFilter.OutAux", DiffuseGISpatialFilteredAux.get(), EResourceState::ShaderRead);

	rg.ExportTexture(outColor, EResourceState::ShaderRead);
	rg.ExportTexture(outAux, EResourceState::ShaderRead);
	rg.AddPass(
		"DiffuseGIDisocclusionFilterPass",
		ERGPassFlags::Compute,
		[&](RGPassBuilder& builder)
		{
			builder.ReadTexture(inColor, EResourceState::ShaderRead)
				.ReadTexture(inAux, EResourceState::ShaderRead)
				.ReadWriteTexture(outColor, EResourceState::UnorderedAccess)
				.ReadWriteTexture(outAux, EResourceState::UnorderedAccess);
			if (depth.IsValid())
				builder.ReadTexture(depth, EResourceState::ShaderRead);
			if (normal.IsValid())
				builder.ReadTexture(normal, EResourceState::ShaderRead);
		},
		[&](RGContext& ctx)
		{
			DiffuseGISpatialFilterPSO->SetTextureSRV("InGIColor", ctx.GetTexture(inColor));
			DiffuseGISpatialFilterPSO->SetTextureSRV("InGIAux", ctx.GetTexture(inAux));
			DiffuseGISpatialFilterPSO->SetTextureSRV("DepthTex", depth.IsValid() ? ctx.GetTexture(depth) : nullptr);
			DiffuseGISpatialFilterPSO->SetTextureSRV("NormalTex", normal.IsValid() ? ctx.GetTexture(normal) : nullptr);
			DiffuseGISpatialFilterPSO->SetTextureUAV("OutGIColor", ctx.GetTexture(outColor));
			DiffuseGISpatialFilterPSO->SetTextureUAV("OutGIAux", ctx.GetTexture(outAux));

			DiffuseGISpatialFilterCB.RTSize = glm::vec2(GetRenderWidth(), GetRenderHeight());
			DiffuseGISpatialFilterCB.ProjectionParams = glm::vec2(FrameProjectionParams.z, FrameProjectionParams.w);
			DiffuseGISpatialFilterPSO->SetCBVValue("SpatialFilterConstant", &DiffuseGISpatialFilterCB);
			DiffuseGISpatialFilterPSO->Apply();
			renderBackend->Dispatch((GetRenderWidth() + 7) / 8, (GetRenderHeight() + 7) / 8, 1);
		});
	if (!rg.Execute())
		return;
}

void Corona::TemporalDenoisingPass()
{
	if (!TemporalDenoisingFilterPSO)
		return;
	if (!DiffuseGITemporalAux[0] || !DiffuseGITemporalAux[1] ||
		!DiffuseGITemporal[0] || !DiffuseGITemporal[1] ||
		!SpecularGITemporal[0] || !SpecularGITemporal[1] ||
		!SpecularGIMoments[0] || !SpecularGIMoments[1])
		return;

	GIBufferWriteIndex = 1 - GIBufferWriteIndex;
	UINT WriteIndex = GIBufferWriteIndex;
	UINT ReadIndex = 1 - WriteIndex;

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

	Texture* const depthTexture = UnjitteredDepthBuffers[ColorBufferWriteIndex].get();
	Texture* const normalTexture = GeomNormalBuffers[ColorBufferWriteIndex].get();
	Texture* const prevDepthTexture = UnjitteredDepthBuffers[1 - ColorBufferWriteIndex].get();
	Texture* const prevNormalTexture = GeomNormalBuffers[1 - ColorBufferWriteIndex].get();

	RenderGraph rg(renderBackend.get());
	RGTextureRef outGIAux = rg.ImportTexture("TemporalDenoise.OutGIResultSH", DiffuseGITemporalAux[WriteIndex].get(), EResourceState::ShaderRead);
	RGTextureRef outGIColor = rg.ImportTexture("TemporalDenoise.OutGIResultColor", DiffuseGITemporal[WriteIndex].get(), EResourceState::ShaderRead);
	RGTextureRef outSpecularGI = rg.ImportTexture("TemporalDenoise.OutSpecularGI", SpecularGITemporal[WriteIndex].get(), EResourceState::ShaderRead);
	RGTextureRef outMoments = rg.ImportTexture("TemporalDenoise.OutMoments", SpecularGIMoments[WriteIndex].get(), EResourceState::ShaderRead);
	RGTextureRef depth = ImportTextureIfValid(rg, "TemporalDenoise.Depth", depthTexture);
	RGTextureRef normal = ImportTextureIfValid(rg, "TemporalDenoise.Normal", normalTexture);
	RGTextureRef inGIAux = ImportTextureIfValid(rg, "TemporalDenoise.InGIResultSH", temporalDiffuseInputAux);
	RGTextureRef inGIColor = ImportTextureIfValid(rg, "TemporalDenoise.InGIResultColor", temporalDiffuseInput);
	RGTextureRef prevGIAux = ImportTextureIfValid(rg, "TemporalDenoise.PrevGIResultSH", DiffuseGITemporalAux[ReadIndex].get());
	RGTextureRef prevGIColor = ImportTextureIfValid(rg, "TemporalDenoise.PrevGIResultColor", DiffuseGITemporal[ReadIndex].get());
	RGTextureRef velocity = ImportTextureIfValid(rg, "TemporalDenoise.Velocity", VelocityBuffer.get());
	RGTextureRef specularGI = ImportTextureIfValid(rg, "TemporalDenoise.SpecularGI", SpecularGIRaw.get());
	RGTextureRef prevSpecularGI = ImportTextureIfValid(rg, "TemporalDenoise.PrevSpecularGI", SpecularGITemporal[ReadIndex].get());
	RGTextureRef roughness = ImportTextureIfValid(rg, "TemporalDenoise.RoughnessMetallic", RoughnessMetalicBuffer.get());
	RGTextureRef prevDepth = (prevDepthTexture && prevDepthTexture == depthTexture) ? depth : ImportTextureIfValid(rg, "TemporalDenoise.PrevDepth", prevDepthTexture);
	RGTextureRef prevNormal = (prevNormalTexture && prevNormalTexture == normalTexture) ? normal : ImportTextureIfValid(rg, "TemporalDenoise.PrevNormal", prevNormalTexture);
	RGTextureRef prevMoments = ImportTextureIfValid(rg, "TemporalDenoise.PrevMoments", SpecularGIMoments[ReadIndex].get());

	rg.ExportTexture(outGIAux, EResourceState::ShaderRead);
	rg.ExportTexture(outGIColor, EResourceState::ShaderRead);
	rg.ExportTexture(outSpecularGI, EResourceState::ShaderRead);
	rg.ExportTexture(outMoments, EResourceState::ShaderRead);
	rg.AddPass(
		"TemporalDenoisingPass",
		ERGPassFlags::Compute,
		[&](RGPassBuilder& builder)
		{
			builder.ReadWriteTexture(outGIAux, EResourceState::UnorderedAccess)
				.ReadWriteTexture(outGIColor, EResourceState::UnorderedAccess)
				.ReadWriteTexture(outSpecularGI, EResourceState::UnorderedAccess)
				.ReadWriteTexture(outMoments, EResourceState::UnorderedAccess);
			if (depth.IsValid())
				builder.ReadTexture(depth, EResourceState::ShaderRead);
			if (normal.IsValid())
				builder.ReadTexture(normal, EResourceState::ShaderRead);
			if (inGIAux.IsValid())
				builder.ReadTexture(inGIAux, EResourceState::ShaderRead);
			if (inGIColor.IsValid())
				builder.ReadTexture(inGIColor, EResourceState::ShaderRead);
			if (prevGIAux.IsValid())
				builder.ReadTexture(prevGIAux, EResourceState::ShaderRead);
			if (prevGIColor.IsValid())
				builder.ReadTexture(prevGIColor, EResourceState::ShaderRead);
			if (velocity.IsValid())
				builder.ReadTexture(velocity, EResourceState::ShaderRead);
			if (specularGI.IsValid())
				builder.ReadTexture(specularGI, EResourceState::ShaderRead);
			if (prevSpecularGI.IsValid())
				builder.ReadTexture(prevSpecularGI, EResourceState::ShaderRead);
			if (roughness.IsValid())
				builder.ReadTexture(roughness, EResourceState::ShaderRead);
			if (prevDepth.IsValid() && prevDepth.Index != depth.Index)
				builder.ReadTexture(prevDepth, EResourceState::ShaderRead);
			if (prevNormal.IsValid() && prevNormal.Index != normal.Index)
				builder.ReadTexture(prevNormal, EResourceState::ShaderRead);
			if (prevMoments.IsValid())
				builder.ReadTexture(prevMoments, EResourceState::ShaderRead);
		},
		[&](RGContext& ctx)
		{
			TemporalDenoisingFilterPSO->SetTextureSRV("DepthTex", depth.IsValid() ? ctx.GetTexture(depth) : nullptr);
			TemporalDenoisingFilterPSO->SetTextureSRV("NormalTex", normal.IsValid() ? ctx.GetTexture(normal) : nullptr);
			TemporalDenoisingFilterPSO->SetTextureSRV("InGIResultSHTex", inGIAux.IsValid() ? ctx.GetTexture(inGIAux) : nullptr);
			TemporalDenoisingFilterPSO->SetTextureSRV("InGIResultColorTex", inGIColor.IsValid() ? ctx.GetTexture(inGIColor) : nullptr);
			TemporalDenoisingFilterPSO->SetTextureSRV("InGIResultSHTexPrev", prevGIAux.IsValid() ? ctx.GetTexture(prevGIAux) : nullptr);
			TemporalDenoisingFilterPSO->SetTextureSRV("InGIResultColorTexPrev", prevGIColor.IsValid() ? ctx.GetTexture(prevGIColor) : nullptr);
			TemporalDenoisingFilterPSO->SetTextureSRV("VelocityTex", velocity.IsValid() ? ctx.GetTexture(velocity) : nullptr);
			TemporalDenoisingFilterPSO->SetTextureSRV("InSpecularGITex", specularGI.IsValid() ? ctx.GetTexture(specularGI) : nullptr);
			TemporalDenoisingFilterPSO->SetTextureSRV("InSpecularGITexPrev", prevSpecularGI.IsValid() ? ctx.GetTexture(prevSpecularGI) : nullptr);
			TemporalDenoisingFilterPSO->SetTextureSRV("RougnessMetalicTex", roughness.IsValid() ? ctx.GetTexture(roughness) : nullptr);
			TemporalDenoisingFilterPSO->SetTextureSRV("PrevDepthTex", prevDepth.IsValid() ? ctx.GetTexture(prevDepth) : nullptr);
			TemporalDenoisingFilterPSO->SetTextureSRV("PrevNormalTex", prevNormal.IsValid() ? ctx.GetTexture(prevNormal) : nullptr);
			TemporalDenoisingFilterPSO->SetTextureSRV("PrevMomentsTex", prevMoments.IsValid() ? ctx.GetTexture(prevMoments) : nullptr);

			TemporalDenoisingFilterPSO->SetTextureUAV("OutGIResultSH", ctx.GetTexture(outGIAux));
			TemporalDenoisingFilterPSO->SetTextureUAV("OutGIResultColor", ctx.GetTexture(outGIColor));
			TemporalDenoisingFilterPSO->SetTextureUAV("OutSpecularGI", ctx.GetTexture(outSpecularGI));
			TemporalDenoisingFilterPSO->SetTextureUAV("OutMoments", ctx.GetTexture(outMoments));

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
		});

	if (!rg.Execute())
		return;

	bTemporalDenoiserHistoryValid = true;
	IndirectAccumulatedFrames = std::min(IndirectAccumulatedFrames + 1u, 1024u);
}

