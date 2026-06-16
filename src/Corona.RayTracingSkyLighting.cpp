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

void Corona::InitRaytracingSkyLightingPass()
{
	shared_ptr<RTPipelineStateObject> tempPSO = renderBackend->CreateRTPipelineStateObject();
	if (!tempPSO)
		return;

	tempPSO->SetNumInstances(static_cast<uint32_t>(RayTracingInstances.size()));
	tempPSO->AddHitGroup("HitGroup", "", "anyhit");
	tempPSO->AddShader("rayGen", RTPipelineStateObject::RAYGEN);

	const RHIShaderStageMask rayGenStage = ToRHIShaderStageMask(RHIShaderStage::RayGeneration);
	const RHIShaderStageMask anyHitStage = ToRHIShaderStageMask(RHIShaderStage::AnyHit);
	tempPSO->BindUAV("global", MakeRHITextureUAV("SkyLightingResult", 0, rayGenStage));
	tempPSO->BindSRV("global", MakeRHIAccelerationStructureSRV("gRtScene", 0, rayGenStage));
	tempPSO->BindSRV("global", MakeRHITextureSRV("DepthTex", 1, rayGenStage));
	tempPSO->BindSRV("global", MakeRHITextureSRV("WorldNormalTex", 2, rayGenStage));
	tempPSO->BindSRV("global", MakeRHITextureSRV("RayNoiseBlueNoiseSource", 3, rayGenStage));
	tempPSO->BindSRV("global", MakeRHITextureSRV("GeoNormalTex", 8, rayGenStage));
	tempPSO->BindCBV("global", MakeRHICBV("ViewParameter", 0, sizeof(RTSkyLightingViewParam), rayGenStage));
	tempPSO->BindSampler("global", MakeRHISampler("sampleWrap", 0, rayGenStage | anyHitStage));
	BindRTBindlessMaterialSchema(*tempPSO, anyHitStage);
	BindRTBindlessGeometrySchema(*tempPSO, anyHitStage);

	tempPSO->AddShader("miss", RTPipelineStateObject::MISS);

	tempPSO->AddShader("anyhit", RTPipelineStateObject::ANYHIT);
	tempPSO->Configure(1, sizeof(float) * 4, sizeof(float) * 2);

	if (tempPSO->InitRS("Shaders\\RaytracedSkyLighting.hlsl"))
	{
		PSO_RT_SKY_LIGHTING = tempPSO;
	}
}

void Corona::RaytraceSkyLightingPass()
{
	if (!TLAS || !PSO_RT_SKY_LIGHTING || !SkyLightingBuffer || !UnjitteredDepthBuffers[ColorBufferWriteIndex] || !NormalBuffers[ColorBufferWriteIndex] || !GeomNormalBuffers[ColorBufferWriteIndex])
		return;

	if (!EnsureRTMaterialRecordBuffer())
		return;

	RTSkyLightingViewParam.ViewMatrix = glm::transpose(ViewMat);
	RTSkyLightingViewParam.InvViewMatrix = glm::transpose(InvViewMat);
	RTSkyLightingViewParam.ProjMatrix = glm::transpose(UnjitteredProjMat);
	RTSkyLightingViewParam.InvProjMatrix = glm::transpose(UnjitteredInvProjMat);
	RTSkyLightingViewParam.ProjectionParams = FrameProjectionParams;
	RTSkyLightingViewParam.RTSize = glm::vec2(GetRenderWidth(), GetRenderHeight());
	RTSkyLightingViewParam.RayLength = std::clamp(RTSkyLightingViewParam.RayLength, 1.0f, 100000.0f);
	RTSkyLightingViewParam.NormalBias = std::clamp(RTSkyLightingViewParam.NormalBias, 0.01f, 4.0f);
	RTSkyLightingViewParam.SkyColorTop = SkyColorTop;
	RTSkyLightingViewParam.SkyIntensity = SkyIntensity;
	RTSkyLightingViewParam.SkyColorBottom = SkyColorBottom;
	RTSkyLightingViewParam.SampleCount = std::clamp(RTSkyLightingViewParam.SampleCount, 1u, 32u);
	RTSkyLightingViewParam.FrameCounter = RenderFrameIndex;
	RTSkyLightingViewParam.NoiseMode = RenderFrameRayNoiseMode;
	RTSkyLightingViewParam.BlueNoiseOffsetStride = RTGIViewParam.BlueNoiseOffsetStride;
	RTSkyLightingViewParam.SkyUpBias = std::clamp(RTSkyLightingViewParam.SkyUpBias, 0.0f, 1.0f);
	RTSkyLightingViewParam.SkyDirectionPower = std::clamp(RTSkyLightingViewParam.SkyDirectionPower, 0.25f, 8.0f);
	RTSkyLightingViewParam.SkyMinWorldY = std::clamp(RTSkyLightingViewParam.SkyMinWorldY, -0.25f, 0.75f);
	RTSkyLightingViewParam.SkyMaxSampleAttempts = std::clamp(RTSkyLightingViewParam.SkyMaxSampleAttempts, 1u, 8u);

	RenderGraph rg(renderBackend.get());
	RGTextureRef skyLightingOutput = rg.ImportTexture("SkyLighting.Output", SkyLightingBuffer.get(), EResourceState::ShaderRead);
	RGTextureRef depthInput = rg.ImportTexture("SkyLighting.Depth", UnjitteredDepthBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead);
	RGTextureRef normalInput = rg.ImportTexture("SkyLighting.Normal", NormalBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead);
	RGTextureRef blueNoiseInput = rg.ImportTexture("SkyLighting.BlueNoise", BlueNoiseTex.get(), EResourceState::ShaderRead);
	RGTextureRef geomNormalInput = rg.ImportTexture("SkyLighting.GeomNormal", GeomNormalBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead);
	RGBufferRef rtMaterials = rg.ImportBuffer("SkyLighting.RtMaterials", RTMaterialRecordBuffer.get(), EResourceState::ShaderRead);

	rg.ExportTexture(skyLightingOutput, EResourceState::ShaderRead);
	rg.AddPass(
		"RaytraceSkyLightingPass",
		ERGPassFlags::RayTracing,
		[&](RGPassBuilder& builder)
		{
			builder.ReadWriteTexture(skyLightingOutput, EResourceState::UnorderedAccess)
				.ReadTexture(depthInput, EResourceState::ShaderRead)
				.ReadTexture(normalInput, EResourceState::ShaderRead)
				.ReadTexture(blueNoiseInput, EResourceState::ShaderRead)
				.ReadTexture(geomNormalInput, EResourceState::ShaderRead)
				.ReadBuffer(rtMaterials, EResourceState::ShaderRead);
		},
		[&](RGContext& ctx)
		{
			Texture* skyLightingTexture = ctx.GetTexture(skyLightingOutput);
			Texture* depthTexture = ctx.GetTexture(depthInput);
			Texture* normalTexture = ctx.GetTexture(normalInput);
			Texture* blueNoiseTexture = ctx.GetTexture(blueNoiseInput);
			Texture* geomNormalTexture = ctx.GetTexture(geomNormalInput);
			Buffer* materialBuffer = ctx.GetBuffer(rtMaterials);

			const FLOAT clearSkyLighting[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
			renderBackend->ClearTextureUAVFloat(skyLightingTexture, clearSkyLighting);

			RTPassBuilder pass(*this, PSO_RT_SKY_LIGHTING);
			pass.BeginScene()
				.SetTextureUAV("global", "SkyLightingResult", skyLightingTexture)
				.SetAccelerationStructure("global", "gRtScene", TLAS)
				.SetTextureSRV("global", "DepthTex", depthTexture)
				.SetTextureSRV("global", "WorldNormalTex", normalTexture)
				.SetTextureSRV("global", "RayNoiseBlueNoiseSource", blueNoiseTexture)
				.SetTextureSRV("global", "GeoNormalTex", geomNormalTexture)
				.SetCBVValue("global", "ViewParameter", &RTSkyLightingViewParam)
				.SetSampler("global", "sampleWrap", samplerWrap.get());
			pass.SetBindlessTextureTable("global", "MaterialTextures")
				.SetBufferSRV("global", "RtMaterials", materialBuffer);
			RTSceneHitProgramDesc hitProgramDesc;
			pass.BindSceneHitPrograms(hitProgramDesc);
			pass.Dispatch(GetRenderWidth(), GetRenderHeight());
		});

	if (!rg.Execute())
		return;

	bSkyLightingOutputValidThisFrame = true;
}
