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

void AppendCpuRuntimeTrace(const std::wstring& line);

shared_ptr<RTPipelineStateObject> Corona::CreateRaytracingSimpleGIPSO(bool bUseSER)
{
		shared_ptr<RTPipelineStateObject> TEMP_PSO_RT_GI = renderBackend->CreateRTPipelineStateObject();
		if (!TEMP_PSO_RT_GI)
			return nullptr;
		if (bUseSER)
		{
			TEMP_PSO_RT_GI->SetShaderDefine("RT_DIFFUSE_GI_USE_SER", "1");
			TEMP_PSO_RT_GI->SetShaderDefine("RT_DIFFUSE_GI_SER_MATERIAL_HINT_BITS", "8");
			TEMP_PSO_RT_GI->SetShaderLibraryTarget("lib_6_9");
		}
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

		const bool bSuccess = TEMP_PSO_RT_GI->InitRS("Shaders\\RaytracedGI.hlsl");

		return bSuccess ? TEMP_PSO_RT_GI : nullptr;
}

void Corona::InitRaytracingSimpleGIPass()
{
	PSO_RT_GI = CreateRaytracingSimpleGIPSO(false);
	if (bEnableRTDiffuseGISER && renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::D3D12)
		InitRaytracingSimpleGISERPass();
}

bool Corona::InitRaytracingSimpleGISERPass()
{
	if (PSO_RT_GI_SER)
		return true;
	if (bRTDiffuseGISimpleSERInitFailed)
		return false;
	if (!renderBackend || renderBackend->GetAPI() != ERenderBackendAPI::D3D12)
		return false;
	if (!bD3D12ShaderModel69Supported)
	{
		bRTDiffuseGISimpleSERInitFailed = true;
		AppendCpuRuntimeTrace(L"[RTDiffuseGI][SER] Simple Raytrace SER skipped: D3D12 Shader Model 6.9 is not supported");
		return false;
	}

	PSO_RT_GI_SER = CreateRaytracingSimpleGIPSO(true);
	if (!PSO_RT_GI_SER)
	{
		bRTDiffuseGISimpleSERInitFailed = true;
		AppendCpuRuntimeTrace(L"[RTDiffuseGI][SER] Simple Raytrace SER PSO creation failed");
	}
	return PSO_RT_GI_SER != nullptr;
}

void Corona::RaytraceGIPass()
{
	shared_ptr<RTPipelineStateObject> pso = PSO_RT_GI;
	if (bEnableRTDiffuseGISER && renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::D3D12 && InitRaytracingSimpleGISERPass())
		pso = PSO_RT_GI_SER;

	if (!TLAS || !pso)
		return;
	renderBackend->EmitGpuCrashMarker("RaytraceGIPass");

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

	RTPassBuilder pass(*this, pso);
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

