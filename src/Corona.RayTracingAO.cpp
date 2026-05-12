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

	tempPSO->BindUAV("global", "AmbientOcclusionResult", 0);
	tempPSO->BindSRV("global", "gRtScene", 0);
	tempPSO->BindSRV("global", "DepthTex", 1);
	tempPSO->BindSRV("global", "WorldNormalTex", 2);
	tempPSO->BindSRV("global", "RayNoiseBlueNoiseSource", 3);
	tempPSO->BindSRV("global", "GeoNormalTex", 8);
	tempPSO->BindCBV("global", "ViewParameter", 0, sizeof(RTAOViewParam), 1);
	tempPSO->BindSampler("global", "sampleWrap", 0);

	tempPSO->AddShader("miss", RTPipelineStateObject::MISS);

	tempPSO->AddShader("closesthit", RTPipelineStateObject::HIT);
	tempPSO->BindSRV("closesthit", "vertices", 4);
	tempPSO->BindSRV("closesthit", "indices", 5);
	tempPSO->BindSRV("closesthit", "AlbedoTex", 6);
	tempPSO->BindSRV("closesthit", "InstanceProperty", 7);
	tempPSO->AddShader("anyhit", RTPipelineStateObject::ANYHIT);
	tempPSO->BindSRV("anyhit", "vertices", 4);
	tempPSO->BindSRV("anyhit", "indices", 5);
	tempPSO->BindSRV("anyhit", "AlbedoTex", 6);
	tempPSO->BindSRV("anyhit", "InstanceProperty", 7);
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
	pass.BindSceneHitPrograms();
	pass.Dispatch(GetRenderWidth(), GetRenderHeight());

	renderBackend->TransitionTexture(AmbientOcclusionBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	bRTAOOutputValidThisFrame = true;
}
