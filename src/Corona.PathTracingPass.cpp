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

void Corona::InitPathTracingPass()
{
	shared_ptr<RTPipelineStateObject> TEMP_PSO_PATH_TRACING = renderBackend->CreateRTPipelineStateObject();
	if (!TEMP_PSO_PATH_TRACING)
		return;
	TEMP_PSO_PATH_TRACING->SetNumInstances(static_cast<uint32_t>(RayTracingInstances.size()));

	TEMP_PSO_PATH_TRACING->AddHitGroup("HitGroup", "PathTracingClosestHit", "PathTracingAnyHit");

	TEMP_PSO_PATH_TRACING->AddShader("PathTracingRayGen", RTPipelineStateObject::RAYGEN);
	
	TEMP_PSO_PATH_TRACING->BindUAV("global", "OutputColor", 0);
	TEMP_PSO_PATH_TRACING->BindUAV("global", "OutAlbedo", 1);
	TEMP_PSO_PATH_TRACING->BindUAV("global", "OutSpecularAlbedo", 2);
	TEMP_PSO_PATH_TRACING->BindUAV("global", "OutNormal", 3);
	TEMP_PSO_PATH_TRACING->BindUAV("global", "OutGeomNormal", 4);
	TEMP_PSO_PATH_TRACING->BindUAV("global", "OutVelocity", 5);
	TEMP_PSO_PATH_TRACING->BindUAV("global", "OutRoughnessMetallic", 6);
	TEMP_PSO_PATH_TRACING->BindUAV("global", "OutDepth", 7);
	TEMP_PSO_PATH_TRACING->BindUAV("global", "OutSpecularHitDistance", 8);
	TEMP_PSO_PATH_TRACING->BindUAV("global", "OutSpecularMotionVector", 9);
	TEMP_PSO_PATH_TRACING->BindSRV("global", "gRtScene", 0);
	TEMP_PSO_PATH_TRACING->BindCBV("global", "ViewParameter", 0, sizeof(PathTracingViewParam), 1);
	TEMP_PSO_PATH_TRACING->BindSampler("global", "sampleWrap", 0);

	TEMP_PSO_PATH_TRACING->AddShader("PathTracingMiss", RTPipelineStateObject::MISS);
	TEMP_PSO_PATH_TRACING->AddShader("ShadowMiss", RTPipelineStateObject::MISS);

	TEMP_PSO_PATH_TRACING->AddShader("PathTracingClosestHit", RTPipelineStateObject::HIT);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingClosestHit", "vertices", 1);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingClosestHit", "indices", 2);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingClosestHit", "InstanceProperty", 3);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingClosestHit", "AlbedoTex", 5);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingClosestHit", "NormalTex", 6);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingClosestHit", "RoughnessTex", 7);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingClosestHit", "MetallicTex", 8);

	TEMP_PSO_PATH_TRACING->AddShader("PathTracingAnyHit", RTPipelineStateObject::ANYHIT);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingAnyHit", "vertices", 1);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingAnyHit", "indices", 2);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingAnyHit", "InstanceProperty", 3);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingAnyHit", "AlbedoTex", 5);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingAnyHit", "NormalTex", 6);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingAnyHit", "RoughnessTex", 7);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingAnyHit", "MetallicTex", 8);
	TEMP_PSO_PATH_TRACING->Configure(8, 256, sizeof(float) * 2);

	bool bSuccess = TEMP_PSO_PATH_TRACING->InitRS("Shaders\\PathTracing.hlsl");

	if (bSuccess)
	{
		PSO_PATH_TRACING = TEMP_PSO_PATH_TRACING;
	}
}

void Corona::PathTracingPass()
{
	const bool bWritePrimaryGBuffer =
		IsPathTracingDLSSRREnabled() &&
		DLSSRRBuffer &&
		AlbedoBuffer &&
		SpecularAlbedoBuffer &&
		NormalBuffers[ColorBufferWriteIndex] &&
		GeomNormalBuffers[ColorBufferWriteIndex] &&
		VelocityBuffer &&
		RoughnessMetalicBuffer &&
		UnjitteredDepthBuffers[ColorBufferWriteIndex] &&
		PathTracingSpecularHitDistanceBuffer &&
		PathTracingSpecularMotionVectorBuffer;
	Texture* outputColor = PathTracingAccumBuffer[PathTracingWriteIndex].get();
	if (!TLAS || !outputColor)
		return;
	renderBackend->EmitGpuCrashMarker("PathTracingPass");
	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::D3D12)
	{
		PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "PathTracingPass");
	}

	if (!PSO_PATH_TRACING)
	{
		InitPathTracingPass();
		if (!PSO_PATH_TRACING)
			return;
	}

	// Transition output buffer to UAV
	renderBackend->TransitionTexture(outputColor, EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	if (bWritePrimaryGBuffer)
	{
		renderBackend->TransitionTexture(AlbedoBuffer.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->TransitionTexture(SpecularAlbedoBuffer.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->TransitionTexture(NormalBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->TransitionTexture(GeomNormalBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->TransitionTexture(VelocityBuffer.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->TransitionTexture(RoughnessMetalicBuffer.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->TransitionTexture(UnjitteredDepthBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->TransitionTexture(PathTracingSpecularHitDistanceBuffer.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->TransitionTexture(PathTracingSpecularMotionVectorBuffer.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	}

	// Check if camera or light changed and reset accumulation
	bool cameraChanged = false;
	for (int i = 0; i < 4 && !cameraChanged; i++)
	{
		for (int j = 0; j < 4 && !cameraChanged; j++)
		{
			if (abs(PrevPathTracingViewMat[i][j] - ViewMat[i][j]) > 0.0001f)
			{
				cameraChanged = true;
			}
		}
	}
	
	// Check if light direction or intensity changed
	glm::vec3 currentLightDir = RenderFrameNormalizedLightDir;
	bool lightDirChanged = glm::length(currentLightDir - PrevPathTracingLightDir) > 0.0001f;
	bool lightIntensityChanged = abs(LightIntensity - PrevPathTracingLightIntensity) > 0.0001f;
	
	// Check if sky color changed
	bool skyColorChanged = glm::length(SkyColorTop - PrevSkyColorTop) > 0.0001f ||
	                       glm::length(SkyColorBottom - PrevSkyColorBottom) > 0.0001f ||
	                       abs(SkyIntensity - PrevSkyIntensity) > 0.0001f;
	const bool bStabilizePrimaryRaySamples =
		bWritePrimaryGBuffer &&
		bEnablePathTracingRRPrimaryRayStabilization &&
		cameraChanged;
	
	if (cameraChanged || lightDirChanged || lightIntensityChanged || skyColorChanged)
	{
		PathTracingAccumulatedFrames = 0;
#if WITH_STREAMLINE
		if (bWritePrimaryGBuffer && (lightDirChanged || lightIntensityChanged || skyColorChanged))
			bDLSSResetNeeded = true;
#endif
		PrevPathTracingViewMat = ViewMat;
		PrevPathTracingLightDir = currentLightDir;
		PrevPathTracingLightIntensity = LightIntensity;
		PrevSkyColorTop = SkyColorTop;
		PrevSkyColorBottom = SkyColorBottom;
		PrevSkyIntensity = SkyIntensity;
		
		// Note: Buffer will be cleared in shader when FrameCounter == 0
	}

	PathTracingViewParam.ViewMatrix = glm::transpose(ViewMat);
	PathTracingViewParam.InvViewMatrix = glm::transpose(InvViewMat);
	PathTracingViewParam.ProjMatrix = glm::transpose(UnjitteredProjMat);
	PathTracingViewParam.InvProjMatrix = glm::transpose(UnjitteredInvProjMat);
	PathTracingViewParam.UnjitteredViewProjMatrix = glm::transpose(UnjitteredViewProjMat);
	PathTracingViewParam.PrevUnjitteredViewProjMatrix = glm::transpose(PrevUnjitteredViewProjMat);
	PathTracingViewParam.ProjectionParams = FrameProjectionParams;
	PathTracingViewParam.LightDirAndIntensity = glm::vec4(RenderFrameNormalizedLightDir, LightIntensity);
	PathTracingViewParam.DirectLightAngularRadius = RTShadowViewParam.ShadowLightRadius;
	PathTracingViewParam.DirectLightSampleCount = std::clamp(PathTracingViewParam.DirectLightSampleCount, 1u, 8u);
	PathTracingViewParam.RandomOffset = glm::vec2(RenderFrameShaderTime, RenderFrameShaderTime);
	PathTracingViewParam.FrameCounter = PathTracingViewParam.DebugMode == 0 ? PathTracingAccumulatedFrames : 0u;
	PathTracingViewParam.BlueNoiseOffsetStride = PathTracingViewParam.DebugMode == 0 ? RenderFrameIndex : 0u;
	PathTracingViewParam.ViewSpreadAngle = glm::tan(Fov * 0.5f) / (0.5f * m_height);
	PathTracingViewParam.SkyColorTop = SkyColorTop;
	PathTracingViewParam.SkyIntensity = SkyIntensity;
	PathTracingViewParam.SkyColorBottom = SkyColorBottom;
	PathTracingViewParam.LightColor = RenderFrameLightColor;
	PathTracingViewParam.bEnableDiffuseGI = bEnableDiffuseGI ? 1u : 0u;
	PathTracingViewParam.bEnableSpecularGI = bEnableSpecularGI ? 1u : 0u;
	PathTracingViewParam.bEnableDirectDiffuse = bEnableDirectDiffuse ? 1u : 0u;
	PathTracingViewParam.bEnableDirectSpecular = bEnableDirectSpecular ? 1u : 0u;
	PathTracingViewParam.bEnableRTAO = 0u;
	PathTracingViewParam.bWritePrimaryGBuffer = bWritePrimaryGBuffer ? 1u : 0u;
	PathTracingViewParam.SpecularMotionVectorScale = PathTracingRRSpecularMotionVectorScale;
	PathTracingViewParam.bStabilizePrimaryRaySamples = bStabilizePrimaryRaySamples ? 1u : 0u;
	ApplyRenderPointLightsToFrameParams();
	const UINT32 targetSamplesPerPixel = std::clamp(PathTracingViewParam.SamplesPerPixel, 1u, 16u);
	UINT32 dispatchSamplesPerPixel = targetSamplesPerPixel;
	if (PathTracingViewParam.DebugMode == 0 && targetSamplesPerPixel > 1u && !bWritePrimaryGBuffer)
	{
		if (PathTracingAccumulatedFrames == 0u)
			dispatchSamplesPerPixel = 1u;
		else if (PathTracingAccumulatedFrames < 8u)
			dispatchSamplesPerPixel = std::min(targetSamplesPerPixel, 2u);
	}
	PathTracingLastDispatchSamplesPerPixel = dispatchSamplesPerPixel;

	PathTracingViewParamCB dispatchViewParam = PathTracingViewParam;
	dispatchViewParam.SamplesPerPixel = dispatchSamplesPerPixel;

	RTPassBuilder pass(*this, PSO_PATH_TRACING);
	pass.BeginScene()
		.SetTextureUAV("global", "OutputColor", outputColor)
		.SetTextureUAV("global", "OutAlbedo", AlbedoBuffer.get())
		.SetTextureUAV("global", "OutSpecularAlbedo", SpecularAlbedoBuffer.get())
		.SetTextureUAV("global", "OutNormal", NormalBuffers[ColorBufferWriteIndex].get())
		.SetTextureUAV("global", "OutGeomNormal", GeomNormalBuffers[ColorBufferWriteIndex].get())
		.SetTextureUAV("global", "OutVelocity", VelocityBuffer.get())
		.SetTextureUAV("global", "OutRoughnessMetallic", RoughnessMetalicBuffer.get())
		.SetTextureUAV("global", "OutDepth", UnjitteredDepthBuffers[ColorBufferWriteIndex].get())
		.SetTextureUAV("global", "OutSpecularHitDistance", PathTracingSpecularHitDistanceBuffer.get())
		.SetTextureUAV("global", "OutSpecularMotionVector", PathTracingSpecularMotionVectorBuffer.get())
		.SetAccelerationStructure("global", "gRtScene", TLAS)
		.SetCBVValue("global", "ViewParameter", &dispatchViewParam)
		.SetSampler("global", "sampleWrap", samplerWrap.get());

	RTSceneHitProgramDesc hitProgramDesc;
	hitProgramDesc.bBindDiffuseTexture = false;
	hitProgramDesc.bBindInstancePropertyBeforeDiffuse = true;
	pass.BindSceneHitPrograms(hitProgramDesc, [&pass](RTPipelineStateObject& pso, const RTSceneHitProgramDesc& desc, Mesh& mesh, uint32_t instanceIndex)
	{
		pso.AddTextureSRVToHitProgram(desc.HitGroup, pass.GetDiffuseTexture(mesh), instanceIndex);
		pso.AddTextureSRVToHitProgram(desc.HitGroup, pass.GetNormalTexture(mesh), instanceIndex);
		pso.AddTextureSRVToHitProgram(desc.HitGroup, pass.GetRoughnessTexture(mesh), instanceIndex);
		pso.AddTextureSRVToHitProgram(desc.HitGroup, pass.GetMetallicTexture(mesh), instanceIndex);
	});
	pass.Dispatch(m_width, m_height);

	if (PathTracingViewParam.DebugMode == 0)
	{
		PathTracingAccumulatedFrames++;
	}

	// Transition output buffer back to SRV
	renderBackend->TransitionTexture(outputColor, EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	if (bWritePrimaryGBuffer)
	{
		renderBackend->TransitionTexture(AlbedoBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
		renderBackend->TransitionTexture(SpecularAlbedoBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
		renderBackend->TransitionTexture(NormalBuffers[ColorBufferWriteIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
		renderBackend->TransitionTexture(GeomNormalBuffers[ColorBufferWriteIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
		renderBackend->TransitionTexture(VelocityBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
		renderBackend->TransitionTexture(RoughnessMetalicBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
		renderBackend->TransitionTexture(UnjitteredDepthBuffers[ColorBufferWriteIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
		renderBackend->TransitionTexture(PathTracingSpecularHitDistanceBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
		renderBackend->TransitionTexture(PathTracingSpecularMotionVectorBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	}
}
