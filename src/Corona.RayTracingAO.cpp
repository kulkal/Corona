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

void Corona::InitRaytracingAOPass()
{
	shared_ptr<RTPipelineStateObject> tempPSO = renderBackend->CreateRTPipelineStateObject();
	if (!tempPSO)
		return;

	tempPSO->SetNumInstances(static_cast<uint32_t>(RayTracingInstances.size()));
	tempPSO->AddHitGroup("HitGroup", "closesthit", "anyhit");
	tempPSO->AddShader("rayGen", RTPipelineStateObject::RAYGEN);

	const RHIShaderStageMask rayGenStage = ToRHIShaderStageMask(RHIShaderStage::RayGeneration);
	const RHIShaderStageMask closestHitStage = ToRHIShaderStageMask(RHIShaderStage::ClosestHit);
	const RHIShaderStageMask anyHitStage = ToRHIShaderStageMask(RHIShaderStage::AnyHit);
	tempPSO->BindUAV("global", MakeRHITextureUAV("AmbientOcclusionResult", 0, rayGenStage));
	tempPSO->BindSRV("global", MakeRHIAccelerationStructureSRV("gRtScene", 0, rayGenStage));
	tempPSO->BindSRV("global", MakeRHITextureSRV("DepthTex", 1, rayGenStage));
	tempPSO->BindSRV("global", MakeRHITextureSRV("WorldNormalTex", 2, rayGenStage));
	tempPSO->BindSRV("global", MakeRHITextureSRV("RayNoiseBlueNoiseSource", 3, rayGenStage));
	tempPSO->BindSRV("global", MakeRHITextureSRV("GeoNormalTex", 8, rayGenStage));
	tempPSO->BindCBV("global", MakeRHICBV("ViewParameter", 0, sizeof(RTAOViewParam), rayGenStage));
	tempPSO->BindSampler("global", MakeRHISampler("sampleWrap", 0, rayGenStage | closestHitStage | anyHitStage));
	BindRTBindlessMaterialSchema(*tempPSO, anyHitStage);
	BindRTBindlessGeometrySchema(*tempPSO, closestHitStage | anyHitStage);

	tempPSO->AddShader("miss", RTPipelineStateObject::MISS);

	tempPSO->AddShader("closesthit", RTPipelineStateObject::HIT);
	tempPSO->AddShader("anyhit", RTPipelineStateObject::ANYHIT);
	tempPSO->Configure(1, sizeof(float) * 4, sizeof(float) * 2);

	if (tempPSO->InitRS("Shaders\\RaytracedAO.hlsl"))
	{
		PSO_RT_AO = tempPSO;
	}
}

void Corona::RaytraceAOPass()
{
	if (!TLAS || !PSO_RT_AO || !AmbientOcclusionBuffer || !UnjitteredDepthBuffers[ColorBufferWriteIndex] || !NormalBuffers[ColorBufferWriteIndex] || !GeomNormalBuffers[ColorBufferWriteIndex])
		return;

	if (!EnsureRTMaterialRecordBuffer())
		return;

	RenderGraph rg(renderBackend.get());
	RGTextureRef aoOutput = rg.ImportTexture("RTAO.AmbientOcclusion", AmbientOcclusionBuffer.get(), EResourceState::ShaderRead);
	RGTextureRef depthInput = rg.ImportTexture("RTAO.Depth", UnjitteredDepthBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead);
	RGTextureRef normalInput = rg.ImportTexture("RTAO.Normal", NormalBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead);
	RGTextureRef blueNoiseInput = rg.ImportTexture("RTAO.BlueNoise", BlueNoiseTex.get(), EResourceState::ShaderRead);
	RGTextureRef geomNormalInput = rg.ImportTexture("RTAO.GeomNormal", GeomNormalBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead);
	RGBufferRef rtMaterials = rg.ImportBuffer("RTAO.RtMaterials", RTMaterialRecordBuffer.get(), EResourceState::ShaderRead);

	rg.ExportTexture(aoOutput, EResourceState::ShaderRead);
	rg.AddPass(
		"RaytraceAOPass",
		ERGPassFlags::RayTracing,
		[&](RGPassBuilder& builder)
		{
			builder.ReadWriteTexture(aoOutput, EResourceState::UnorderedAccess)
				.ReadTexture(depthInput, EResourceState::ShaderRead)
				.ReadTexture(normalInput, EResourceState::ShaderRead)
				.ReadTexture(blueNoiseInput, EResourceState::ShaderRead)
				.ReadTexture(geomNormalInput, EResourceState::ShaderRead)
				.ReadBuffer(rtMaterials, EResourceState::ShaderRead);
		},
		[&](RGContext& ctx)
		{
			Texture* aoTexture = ctx.GetTexture(aoOutput);
			Texture* depthTexture = ctx.GetTexture(depthInput);
			Texture* normalTexture = ctx.GetTexture(normalInput);
			Texture* blueNoiseTexture = ctx.GetTexture(blueNoiseInput);
			Texture* geomNormalTexture = ctx.GetTexture(geomNormalInput);
			Buffer* materialBuffer = ctx.GetBuffer(rtMaterials);

			const FLOAT clearAO[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
			renderBackend->ClearTextureUAVFloat(aoTexture, clearAO);

			RTAOViewParam.ViewMatrix = glm::transpose(ViewMat);
			RTAOViewParam.InvViewMatrix = glm::transpose(InvViewMat);
			RTAOViewParam.ProjMatrix = glm::transpose(UnjitteredProjMat);
			RTAOViewParam.InvProjMatrix = glm::transpose(UnjitteredInvProjMat);
			RTAOViewParam.ProjectionParams = FrameProjectionParams;
			RTAOViewParam.RTSize = glm::vec2(GetRenderWidth(), GetRenderHeight());
			RTAOViewParam.Radius = std::clamp(RTAOViewParam.Radius, 2.0f, 256.0f);
			RTAOViewParam.Power = std::clamp(RTAOViewParam.Power, 0.25f, 4.0f);
			RTAOViewParam.SampleCount = std::clamp(RTAOViewParam.SampleCount, 1u, 16u);
			RTAOViewParam.FrameCounter = RenderFrameIndex;
			RTAOViewParam.NoiseMode = RenderFrameRayNoiseMode;
			RTAOViewParam.BlueNoiseOffsetStride = RTGIViewParam.BlueNoiseOffsetStride;
			RTAOViewParam.NormalBias = std::clamp(RTAOViewParam.NormalBias, 0.01f, 2.0f);

			RTPassBuilder pass(*this, PSO_RT_AO, ERtProfilePass::AO);
			pass.BeginScene()
				.SetTextureUAV("global", "AmbientOcclusionResult", aoTexture)
				.SetAccelerationStructure("global", "gRtScene", TLAS)
				.SetTextureSRV("global", "DepthTex", depthTexture)
				.SetTextureSRV("global", "WorldNormalTex", normalTexture)
				.SetTextureSRV("global", "RayNoiseBlueNoiseSource", blueNoiseTexture)
				.SetTextureSRV("global", "GeoNormalTex", geomNormalTexture)
				.SetCBVValue("global", "ViewParameter", &RTAOViewParam)
				.SetSampler("global", "sampleWrap", samplerWrap.get());
			pass.SetBindlessTextureTable("global", "MaterialTextures")
				.SetBufferSRV("global", "RtMaterials", materialBuffer);
			RTSceneHitProgramDesc hitProgramDesc;
			pass.BindSceneHitPrograms(hitProgramDesc);
			pass.Dispatch(GetRenderWidth(), GetRenderHeight());
		});

	if (!rg.Execute())
		return;

	bRTAOOutputValidThisFrame = true;
}
