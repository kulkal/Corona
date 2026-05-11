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

void Corona::InitRaytracingSimpleGIPass()
{
		shared_ptr<RTPipelineStateObject> TEMP_PSO_RT_GI = renderBackend->CreateRTPipelineStateObject();
		if (!TEMP_PSO_RT_GI)
			return;
		TEMP_PSO_RT_GI->SetNumInstances(static_cast<uint32_t>(RayTracingInstances.size()));

		TEMP_PSO_RT_GI->AddHitGroup("HitGroup", "chs", "");


		TEMP_PSO_RT_GI->AddShader("rayGen", RTPipelineStateObject::RAYGEN);
		
		TEMP_PSO_RT_GI->BindUAV("global", "GIResultSH", 0);
		TEMP_PSO_RT_GI->BindUAV("global", "GIResultColor", 1);
		TEMP_PSO_RT_GI->BindSRV("global", "gRtScene", 0);
		TEMP_PSO_RT_GI->BindSRV("global", "DepthTex", 1);
		TEMP_PSO_RT_GI->BindSRV("global", "WorldNormalTex", 2);
		TEMP_PSO_RT_GI->BindCBV("global", "ViewParameter", 0, sizeof(RTGIViewParam), 1);
		TEMP_PSO_RT_GI->BindSampler("global", "sampleWrap", 0);
		TEMP_PSO_RT_GI->BindSRV("global", "RayNoiseBlueNoiseSource", 7);

		TEMP_PSO_RT_GI->AddShader("miss", RTPipelineStateObject::MISS);
		TEMP_PSO_RT_GI->AddShader("missShadow", RTPipelineStateObject::MISS);


		TEMP_PSO_RT_GI->AddShader("chs", RTPipelineStateObject::HIT);
		TEMP_PSO_RT_GI->BindSRV("chs", "vertices", 3);
		TEMP_PSO_RT_GI->BindSRV("chs", "indices", 4);
		TEMP_PSO_RT_GI->BindSRV("chs", "AlbedoTex", 5);
		TEMP_PSO_RT_GI->BindSRV("chs", "InstanceProperty", 6);
		TEMP_PSO_RT_GI->Configure(1, sizeof(float) * 12, sizeof(float) * 2);

		bool bSuccess = TEMP_PSO_RT_GI->InitRS("Shaders\\RaytracedGI.hlsl");

		if (bSuccess)
		{
			PSO_RT_GI = TEMP_PSO_RT_GI;
		}
}

void Corona::RaytraceGIPass()
{
	if (!TLAS || !PSO_RT_GI)
		return;
	renderBackend->EmitGpuCrashMarker("RaytraceGIPass");
	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::D3D12)
	{
		PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "RaytraceGIPass");
	}

	renderBackend->TransitionTexture(DiffuseGIRawAux.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionTexture(DiffuseGIRaw.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	const FLOAT clearGI[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	renderBackend->ClearTextureUAVFloat(DiffuseGIRawAux.get(), clearGI);
	renderBackend->ClearTextureUAVFloat(DiffuseGIRaw.get(), clearGI);

	RTGIViewParam.ViewMatrix = glm::transpose(ViewMat);
	RTGIViewParam.InvViewMatrix = glm::transpose(InvViewMat);
	RTGIViewParam.ProjMatrix = glm::transpose(UnjitteredProjMat);
	RTGIViewParam.InvProjMatrix = glm::transpose(UnjitteredInvProjMat);
	RTGIViewParam.ProjectionParams = FrameProjectionParams;
	RTGIViewParam.LightDir = glm::vec4(RenderFrameNormalizedLightDir, LightIntensity);
	RTGIViewParam.RandomOffset = glm::vec2(RenderFrameShaderTime, RenderFrameShaderTime);
	RTGIViewParam.FrameCounter = RenderFrameIndex;
	RTGIViewParam.ViewSpreadAngle = glm::tan(Fov * 0.5f) / (0.5f * GetRenderHeight());
	RTGIViewParam.NoiseMode = RenderFrameRayNoiseMode;
	RTGIViewParam.bIncludeSkyLighting = RenderFrameDiffuseGISkyLightingEnabled;
	RTGIViewParam.SkyColorTop = SkyColorTop;
	RTGIViewParam.SkyIntensity = RenderFrameDiffuseGISkyIntensity;
	RTGIViewParam.SkyColorBottom = SkyColorBottom;
	RTGIViewParam.LightColor = RenderFrameLightColor;

	RTPassBuilder pass(*this, PSO_RT_GI);
	pass.BeginScene()
		.SetTextureUAV("global", "GIResultSH", DiffuseGIRawAux.get())
		.SetTextureUAV("global", "GIResultColor", DiffuseGIRaw.get())
		.SetAccelerationStructure("global", "gRtScene", TLAS)
		.SetTextureSRV("global", "DepthTex", UnjitteredDepthBuffers[ColorBufferWriteIndex].get())
		.SetTextureSRV("global", "WorldNormalTex", NormalBuffers[ColorBufferWriteIndex].get())
		.SetTextureSRV("global", "RayNoiseBlueNoiseSource", BlueNoiseTex.get())
		.SetCBVValue("global", "ViewParameter", &RTGIViewParam)
		.SetSampler("global", "sampleWrap", samplerWrap.get());
	pass.BindSceneHitPrograms();
	pass.Dispatch(GetRenderWidth(), GetRenderHeight());

	renderBackend->TransitionTexture(DiffuseGIRawAux.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(DiffuseGIRaw.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
}

