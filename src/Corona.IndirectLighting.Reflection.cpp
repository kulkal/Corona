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

void Corona::InitRaytracingReflectionPass()
{
		shared_ptr<RTPipelineStateObject> TEMP_PSO_RT_REFLECTION = renderBackend->CreateRTPipelineStateObject();
		if (!TEMP_PSO_RT_REFLECTION)
			return;
		TEMP_PSO_RT_REFLECTION->SetNumInstances(static_cast<uint32_t>(RayTracingInstances.size()));

		TEMP_PSO_RT_REFLECTION->AddHitGroup("HitGroup", "chs", "");
		//TEMP_PSO_RT_REFLECTION->AddHitGroup("ShadowHitGroup", "chsShadow", "");


		TEMP_PSO_RT_REFLECTION->AddShader("rayGen", RTPipelineStateObject::RAYGEN);
		
		TEMP_PSO_RT_REFLECTION->BindUAV("global", "ReflectionResult", 0);
		TEMP_PSO_RT_REFLECTION->BindUAV("global", "SpecularHitDistanceResult", 1);
		TEMP_PSO_RT_REFLECTION->BindUAV("global", "SpecularMotionVectorResult", 2);
		TEMP_PSO_RT_REFLECTION->BindSRV("global", "gRtScene", 0);
		TEMP_PSO_RT_REFLECTION->BindSRV("global", "DepthTex", 1);
		TEMP_PSO_RT_REFLECTION->BindSRV("global", "GeoNormalTex", 2);
		TEMP_PSO_RT_REFLECTION->BindSRV("global", "RougnessMetallicTex", 6);
		TEMP_PSO_RT_REFLECTION->BindSRV("global", "RayNoiseBlueNoiseSource", 7);
		TEMP_PSO_RT_REFLECTION->BindSRV("global", "WorldNormalTex", 8);


		TEMP_PSO_RT_REFLECTION->BindCBV("global", "ViewParameter", 0, sizeof(RTReflectionViewParam), 1);
		TEMP_PSO_RT_REFLECTION->BindSampler("global", "sampleWrap", 0);

		TEMP_PSO_RT_REFLECTION->AddShader("miss", RTPipelineStateObject::MISS);
		TEMP_PSO_RT_REFLECTION->AddShader("missShadow", RTPipelineStateObject::MISS);


		TEMP_PSO_RT_REFLECTION->AddShader("chs", RTPipelineStateObject::HIT);
		TEMP_PSO_RT_REFLECTION->BindSRV("chs", "vertices", 3);
		TEMP_PSO_RT_REFLECTION->BindSRV("chs", "indices", 4);
		TEMP_PSO_RT_REFLECTION->BindSRV("chs", "AlbedoTex", 5);
		TEMP_PSO_RT_REFLECTION->BindSRV("chs", "InstanceProperty", 9);
		TEMP_PSO_RT_REFLECTION->Configure(1, sizeof(float) * 13, sizeof(float) * 2);

		bool bSuccess = TEMP_PSO_RT_REFLECTION->InitRS("Shaders\\RaytracedReflection.hlsl");

		if (bSuccess)
		{
			PSO_RT_REFLECTION = TEMP_PSO_RT_REFLECTION;
		}
}

void Corona::RaytraceReflectionPass()
{
	if (!TLAS || !PSO_RT_REFLECTION || !SpecularGIRaw ||
		!PathTracingSpecularHitDistanceBuffer ||
		!PathTracingSpecularMotionVectorBuffer)
		return;
	renderBackend->EmitGpuCrashMarker("RaytraceReflectionPass");
	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::D3D12)
	{
		PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "RaytraceReflectionPass");
	}

	renderBackend->TransitionTexture(SpecularGIRaw.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionTexture(PathTracingSpecularHitDistanceBuffer.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionTexture(PathTracingSpecularMotionVectorBuffer.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	const FLOAT clearReflection[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	const FLOAT clearHitDistance[4] = { Far, 0.0f, 0.0f, 0.0f };
	const FLOAT clearMotionVector[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	renderBackend->ClearTextureUAVFloat(SpecularGIRaw.get(), clearReflection);
	renderBackend->ClearTextureUAVFloat(PathTracingSpecularHitDistanceBuffer.get(), clearHitDistance);
	renderBackend->ClearTextureUAVFloat(PathTracingSpecularMotionVectorBuffer.get(), clearMotionVector);

	RTReflectionViewParam.ViewMatrix = glm::transpose(ViewMat);
	RTReflectionViewParam.InvViewMatrix = glm::transpose(InvViewMat);
	RTReflectionViewParam.ProjMatrix = glm::transpose(UnjitteredProjMat);
	RTReflectionViewParam.InvProjMatrix = glm::transpose(UnjitteredInvProjMat);
	RTReflectionViewParam.UnjitteredViewProjMatrix = glm::transpose(UnjitteredViewProjMat);
	RTReflectionViewParam.PrevUnjitteredViewProjMatrix = glm::transpose(PrevUnjitteredViewProjMat);
	RTReflectionViewParam.ProjectionParams = FrameProjectionParams;
	RTReflectionViewParam.LightDir = glm::vec4(RenderFrameNormalizedLightDir, LightIntensity);
	RTReflectionViewParam.RandomOffset = glm::vec2(RenderFrameShaderTime, RenderFrameShaderTime);
	RTReflectionViewParam.FrameCounter = RenderFrameIndex;
	RTReflectionViewParam.ViewSpreadAngle = glm::tan(Fov * 0.5f) / (0.5f * GetRenderHeight());
	RTReflectionViewParam.NoiseMode = RenderFrameRayNoiseMode;
	RTReflectionViewParam.SkyColorTop = SkyColorTop;
	RTReflectionViewParam.SkyIntensity = SkyIntensity;
	RTReflectionViewParam.SkyColorBottom = SkyColorBottom;
	RTReflectionViewParam.LightColor = RenderFrameLightColor;
	RTReflectionViewParam.PrefilteredEnvRoughnessThreshold = PrefilteredEnvRoughnessThreshold;
	RTReflectionViewParam.PrefilteredEnvRoughnessFade = PrefilteredEnvRoughnessFade;
	RTReflectionViewParam.bEnablePrefilteredEnvSpecular = bEnablePrefilteredEnvSpecular ? 1u : 0u;
	RTReflectionViewParam.SpecularMotionVectorScale = HybridRRSpecularMotionVectorScale;
	RTReflectionViewParam.bWriteRRSpecularMotionVectors =
		(IsDLSSRREnabled() && bEnableHybridRRSpecularMotionVectors) ? 1u : 0u;
	RTReflectionViewParam.bWriteRRSpecularHitDistance =
		(IsDLSSRREnabled() && bEnableHybridRRSpecularHitDistance) ? 1u : 0u;
	RTReflectionViewParam.bUseRRSpecularGuideRay = bEnableHybridRRSpecularGuideRay ? 1u : 0u;

	RTPassBuilder pass(*this, PSO_RT_REFLECTION);
	pass.BeginScene()
		.SetTextureUAV("global", "ReflectionResult", SpecularGIRaw.get())
		.SetTextureUAV("global", "SpecularHitDistanceResult", PathTracingSpecularHitDistanceBuffer.get())
		.SetTextureUAV("global", "SpecularMotionVectorResult", PathTracingSpecularMotionVectorBuffer.get())
		.SetAccelerationStructure("global", "gRtScene", TLAS)
		.SetTextureSRV("global", "DepthTex", UnjitteredDepthBuffers[ColorBufferWriteIndex].get())
		.SetTextureSRV("global", "GeoNormalTex", GeomNormalBuffers[ColorBufferWriteIndex].get())
		.SetTextureSRV("global", "RougnessMetallicTex", RoughnessMetalicBuffer.get())
		.SetTextureSRV("global", "RayNoiseBlueNoiseSource", BlueNoiseTex.get())
		.SetTextureSRV("global", "WorldNormalTex", NormalBuffers[ColorBufferWriteIndex].get())
		.SetCBVValue("global", "ViewParameter", &RTReflectionViewParam)
		.SetSampler("global", "sampleWrap", samplerWrap.get());
	pass.BindSceneHitPrograms();
	pass.Dispatch(GetRenderWidth(), GetRenderHeight());

	renderBackend->TransitionTexture(SpecularGIRaw.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(PathTracingSpecularHitDistanceBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(PathTracingSpecularMotionVectorBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	//PIXEndEvent();
}
