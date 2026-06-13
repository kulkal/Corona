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
	tempPSO->BindSRV("closesthit", MakeRHIBufferSRV("vertices", 4, closestHitStage, RHIBufferViewKind::Raw));
	tempPSO->BindSRV("closesthit", MakeRHIBufferSRV("indices", 5, closestHitStage, RHIBufferViewKind::Raw));
	if (!UsesRTBindlessMaterials())
		tempPSO->BindSRV("closesthit", MakeRHITextureSRV("AlbedoTex", 6, closestHitStage));
	tempPSO->BindSRV("closesthit", MakeRHIBufferSRV("InstanceProperty", 7, closestHitStage, RHIBufferViewKind::Raw));
	tempPSO->AddShader("anyhit", RTPipelineStateObject::ANYHIT);
	tempPSO->BindSRV("anyhit", MakeRHIBufferSRV("vertices", 4, anyHitStage, RHIBufferViewKind::Raw));
	tempPSO->BindSRV("anyhit", MakeRHIBufferSRV("indices", 5, anyHitStage, RHIBufferViewKind::Raw));
	if (!UsesRTBindlessMaterials())
		tempPSO->BindSRV("anyhit", MakeRHITextureSRV("AlbedoTex", 6, anyHitStage));
	tempPSO->BindSRV("anyhit", MakeRHIBufferSRV("InstanceProperty", 7, anyHitStage, RHIBufferViewKind::Raw));
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

	renderBackend->EmitGpuCrashMarker("RaytraceAOPass");

	const bool bUseBindlessMaterials = UsesRTBindlessMaterials();
	if (bUseBindlessMaterials && !EnsureRTMaterialRecordBuffer())
		return;

	renderBackend->TransitionTexture(AmbientOcclusionBuffer.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	const FLOAT clearAO[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	renderBackend->ClearTextureUAVFloat(AmbientOcclusionBuffer.get(), clearAO);

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

	RTPassBuilder pass(*this, PSO_RT_AO);
	pass.BeginScene()
		.SetTextureUAV("global", "AmbientOcclusionResult", AmbientOcclusionBuffer.get())
		.SetAccelerationStructure("global", "gRtScene", TLAS)
		.SetTextureSRV("global", "DepthTex", UnjitteredDepthBuffers[ColorBufferWriteIndex].get())
		.SetTextureSRV("global", "WorldNormalTex", NormalBuffers[ColorBufferWriteIndex].get())
		.SetTextureSRV("global", "RayNoiseBlueNoiseSource", BlueNoiseTex.get())
		.SetTextureSRV("global", "GeoNormalTex", GeomNormalBuffers[ColorBufferWriteIndex].get())
		.SetCBVValue("global", "ViewParameter", &RTAOViewParam)
		.SetSampler("global", "sampleWrap", samplerWrap.get());
	if (bUseBindlessMaterials)
	{
		pass.SetBindlessTextureTable("global", "MaterialTextures")
			.SetBufferSRV("global", "RtMaterials", RTMaterialRecordBuffer.get());
	}
	RTSceneHitProgramDesc hitProgramDesc;
	hitProgramDesc.bBindDiffuseTexture = !bUseBindlessMaterials;
	pass.BindSceneHitPrograms(hitProgramDesc);
	pass.Dispatch(GetRenderWidth(), GetRenderHeight());

	renderBackend->TransitionTexture(AmbientOcclusionBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	bRTAOOutputValidThisFrame = true;
}
