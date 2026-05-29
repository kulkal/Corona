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

void Corona::InitRaytracingShadowPass()
{
		shared_ptr<RTPipelineStateObject> TEMP_PSO_RT_SHADOW = renderBackend->CreateRTPipelineStateObject();
		if (!TEMP_PSO_RT_SHADOW)
			return;
		TEMP_PSO_RT_SHADOW->SetNumInstances(static_cast<uint32_t>(RayTracingInstances.size())); // important for cbv allocation & shadertable size.

		// new interface
		TEMP_PSO_RT_SHADOW->AddHitGroup("HitGroup", "", "anyhit");
		TEMP_PSO_RT_SHADOW->AddShader("rayGen", RTPipelineStateObject::RAYGEN);

		TEMP_PSO_RT_SHADOW->BindUAV("global", "ShadowResult", 0);
		TEMP_PSO_RT_SHADOW->BindSRV("global", "gRtScene", 0);
		TEMP_PSO_RT_SHADOW->BindSRV("global", "DepthTex", 1);
		TEMP_PSO_RT_SHADOW->BindSRV("global", "WorldNormalTex", 2);
		TEMP_PSO_RT_SHADOW->BindSRV("global", "GeoNormalTex", 7);
		TEMP_PSO_RT_SHADOW->BindSRV("global", "RayNoiseBlueNoiseSource", 8);

		TEMP_PSO_RT_SHADOW->BindCBV("global", "ViewParameter", 0, sizeof(RTShadowViewParamCB), 1);
		TEMP_PSO_RT_SHADOW->BindSampler("global", "sampleWrap", 0);



		TEMP_PSO_RT_SHADOW->AddShader("miss", RTPipelineStateObject::MISS);
		
		TEMP_PSO_RT_SHADOW->AddShader("anyhit", RTPipelineStateObject::ANYHIT);
		TEMP_PSO_RT_SHADOW->BindSRV("anyhit", "vertices", 3);
		TEMP_PSO_RT_SHADOW->BindSRV("anyhit", "indices", 4);
		TEMP_PSO_RT_SHADOW->BindSRV("anyhit", "AlbedoTex", 5);
		TEMP_PSO_RT_SHADOW->BindSRV("anyhit", "InstanceProperty", 6);
		TEMP_PSO_RT_SHADOW->Configure(1, sizeof(float) * 4, sizeof(float) * 2);

		bool bSuccess = TEMP_PSO_RT_SHADOW->InitRS("Shaders\\RaytracedShadow.hlsl");
		if (bSuccess)
		{
			PSO_RT_SHADOW = TEMP_PSO_RT_SHADOW;
		}
}

void Corona::RaytraceShadowPass()
{
	if (!ShadowBuffer || !BlueNoiseTex || !TLAS || !PSO_RT_SHADOW || !UnjitteredDepthBuffers[ColorBufferWriteIndex] || !NormalBuffers[ColorBufferWriteIndex] || !GeomNormalBuffers[ColorBufferWriteIndex])
		return;
	renderBackend->EmitGpuCrashMarker("RaytraceShadowPass");

	RTShadowViewParam.ViewMatrix = glm::transpose(ViewMat);
	RTShadowViewParam.InvViewMatrix = glm::transpose(InvViewMat);
	RTShadowViewParam.ProjMatrix = glm::transpose(UnjitteredProjMat);
	RTShadowViewParam.InvProjMatrix = glm::transpose(UnjitteredInvProjMat);
	RTShadowViewParam.ProjectionParams = FrameProjectionParams;
	RTShadowViewParam.LightDir = glm::vec4(RenderFrameNormalizedLightDir, 0.0f);
	RTShadowViewParam.ShadowLightRadius = std::clamp(RTShadowViewParam.ShadowLightRadius, 0.0f, 0.03f);
	RTShadowViewParam.ShadowSampleCount = std::clamp(RTShadowViewParam.ShadowSampleCount, 1u, 16u);
	RTShadowViewParam.FrameCounter = RenderFrameIndex;
	RTShadowViewParam.BlueNoiseOffsetStride = RTGIViewParam.BlueNoiseOffsetStride;
	RTShadowViewParam.NoiseMode = RenderFrameRayNoiseMode;

	// Channel-pack up to 3 point lights into ShadowBuffer.gba. Pick the
	// first 3 enabled lights in registration order — when richer importance
	// sampling is needed swap in a brightness/distance heuristic here.
	uint32_t shadowedCount = 0;
	for (const PointLightState& pl : RenderWorld.PointLights)
	{
		if (shadowedCount >= 3u)
			break;
		if (!pl.bEnabled || pl.Intensity <= 0.0f)
			continue;
		RTShadowViewParam.ShadowedPointLights[shadowedCount] =
			glm::vec4(pl.Position, std::max(pl.Radius, 0.01f));
		++shadowedCount;
	}
	for (uint32_t i = shadowedCount; i < 3u; ++i)
		RTShadowViewParam.ShadowedPointLights[i] = glm::vec4(0.0f);
	RTShadowViewParam.ShadowedPointLightCount = shadowedCount;

	renderBackend->TransitionTexture(ShadowBuffer.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	RTPassBuilder pass(*this, PSO_RT_SHADOW);
	pass.BeginScene()
		.SetTextureUAV("global", "ShadowResult", ShadowBuffer.get())
		.SetAccelerationStructure("global", "gRtScene", TLAS)
		.SetTextureSRV("global", "DepthTex", UnjitteredDepthBuffers[ColorBufferWriteIndex].get())
		.SetTextureSRV("global", "WorldNormalTex", NormalBuffers[ColorBufferWriteIndex].get())
		.SetTextureSRV("global", "GeoNormalTex", GeomNormalBuffers[ColorBufferWriteIndex].get())
		.SetTextureSRV("global", "RayNoiseBlueNoiseSource", BlueNoiseTex.get())
		.SetCBVValue("global", "ViewParameter", &RTShadowViewParam)
		.SetSampler("global", "sampleWrap", samplerWrap.get());
	pass.BindSceneHitPrograms();
	pass.Dispatch(GetRenderWidth(), GetRenderHeight());

	renderBackend->TransitionTexture(ShadowBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	bShadowOutputValidThisFrame = true;
}
