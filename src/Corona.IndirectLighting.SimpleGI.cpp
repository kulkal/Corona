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
		
		const RHIShaderStageMask rayGenStage = ToRHIShaderStageMask(RHIShaderStage::RayGeneration);
		const RHIShaderStageMask closestHitStage = ToRHIShaderStageMask(RHIShaderStage::ClosestHit);
		TEMP_PSO_RT_GI->BindUAV("global", MakeRHITextureUAV("GIResultSH", 0, rayGenStage));
		TEMP_PSO_RT_GI->BindUAV("global", MakeRHITextureUAV("GIResultColor", 1, rayGenStage));
		TEMP_PSO_RT_GI->BindSRV("global", MakeRHIAccelerationStructureSRV("gRtScene", 0, rayGenStage));
		TEMP_PSO_RT_GI->BindSRV("global", MakeRHITextureSRV("DepthTex", 1, rayGenStage));
		TEMP_PSO_RT_GI->BindSRV("global", MakeRHITextureSRV("WorldNormalTex", 2, rayGenStage));
		TEMP_PSO_RT_GI->BindCBV("global", MakeRHICBV("ViewParameter", 0, sizeof(RTGIViewParam), rayGenStage));
		TEMP_PSO_RT_GI->BindSampler("global", MakeRHISampler("sampleWrap", 0, rayGenStage | closestHitStage));
		TEMP_PSO_RT_GI->BindSRV("global", MakeRHITextureSRV("RayNoiseBlueNoiseSource", 7, rayGenStage));
		BindRTBindlessMaterialSchema(*TEMP_PSO_RT_GI, closestHitStage);
		BindRTBindlessGeometrySchema(*TEMP_PSO_RT_GI, closestHitStage);

		TEMP_PSO_RT_GI->AddShader("miss", RTPipelineStateObject::MISS);
		TEMP_PSO_RT_GI->AddShader("missShadow", RTPipelineStateObject::MISS);


		TEMP_PSO_RT_GI->AddShader("chs", RTPipelineStateObject::HIT);
		TEMP_PSO_RT_GI->BindSRV("chs", MakeRHIBufferSRV("vertices", 3, closestHitStage, RHIBufferViewKind::Raw));
		TEMP_PSO_RT_GI->BindSRV("chs", MakeRHIBufferSRV("indices", 4, closestHitStage, RHIBufferViewKind::Raw));
		if (!UsesRTBindlessMaterials())
			TEMP_PSO_RT_GI->BindSRV("chs", MakeRHITextureSRV("AlbedoTex", 5, closestHitStage));
		TEMP_PSO_RT_GI->BindSRV("chs", MakeRHIBufferSRV("InstanceProperty", 6, closestHitStage, RHIBufferViewKind::Raw));
		TEMP_PSO_RT_GI->Configure(1, sizeof(float) * 12, sizeof(float) * 2);

		const bool bSuccess = TEMP_PSO_RT_GI->InitRS("Shaders\\RaytracedGI.hlsl");

		return bSuccess ? TEMP_PSO_RT_GI : nullptr;
}

void Corona::InitRaytracingSimpleGIPass()
{
	PSO_RT_GI = CreateRaytracingSimpleGIPSO(false);
	if (bEnableRTDiffuseGISER && renderBackend && renderBackend->SupportsShaderExecutionReordering())
		InitRaytracingSimpleGISERPass();
}

bool Corona::InitRaytracingSimpleGISERPass()
{
	if (PSO_RT_GI_SER)
		return true;
	if (bRTDiffuseGISimpleSERInitFailed)
		return false;
	if (!renderBackend)
		return false;
	if (!renderBackend->SupportsShaderExecutionReordering())
	{
		bRTDiffuseGISimpleSERInitFailed = true;
		AppendCpuRuntimeTrace(L"[RTDiffuseGI][SER] Simple Raytrace SER skipped: backend does not support shader execution reordering");
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
	if (bEnableRTDiffuseGISER && renderBackend && renderBackend->SupportsShaderExecutionReordering() && InitRaytracingSimpleGISERPass())
		pso = PSO_RT_GI_SER;

	if (!TLAS || !pso)
		return;
	renderBackend->EmitGpuCrashMarker("RaytraceGIPass");

	const bool bUseBindlessMaterials = UsesRTBindlessMaterials();
	if (bUseBindlessMaterials && !EnsureRTMaterialRecordBuffer())
		return;

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
	RTGIViewParam.GISamplesPerPixel = std::clamp(SimpleGISamplesPerPixel, 1u, 8u);
	RTGIViewParam.SkyColorTop = SkyColorTop;
	RTGIViewParam.SkyIntensity = RenderFrameDiffuseGISkyIntensity;
	RTGIViewParam.SkyColorBottom = SkyColorBottom;
	RTGIViewParam.LightColor = RenderFrameLightColor;
	FillPointLightParams(
		RTGIViewParam.PointLights,
		RTGIViewParam.PointLightCount,
		std::min(MaxDiffuseGIPointLights, DiffuseGIPointLightLimit));
	{
		static float sLastLoggedLightIntensity = -1.0f;
		static UINT32 sLastLoggedPointLightCount = 0xFFFFFFFFu;
		if (std::abs(sLastLoggedLightIntensity - LightIntensity) > 0.0001f ||
			sLastLoggedPointLightCount != RTGIViewParam.PointLightCount)
		{
			sLastLoggedLightIntensity = LightIntensity;
			sLastLoggedPointLightCount = RTGIViewParam.PointLightCount;
			AppendCpuRuntimeTrace(
				L"[DiffuseGI][Simple] lightIntensity=" + std::to_wstring(LightIntensity) +
				L", lightDir=" + std::to_wstring(RenderFrameNormalizedLightDir.x) + L"," +
				std::to_wstring(RenderFrameNormalizedLightDir.y) + L"," +
				std::to_wstring(RenderFrameNormalizedLightDir.z) +
				L", pointLights=" + std::to_wstring(RTGIViewParam.PointLightCount) +
				L", pointLightLimit=" + std::to_wstring(DiffuseGIPointLightLimit) +
				L", sky=" + std::to_wstring(RenderFrameDiffuseGISkyLightingEnabled));
		}
	}

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
	if (bUseBindlessMaterials)
	{
		pass.SetBindlessTextureTable("global", "MaterialTextures")
			.SetBufferSRV("global", "RtMaterials", RTMaterialRecordBuffer.get());
	}
	RTSceneHitProgramDesc hitProgramDesc;
	hitProgramDesc.bBindDiffuseTexture = !bUseBindlessMaterials;
	pass.BindSceneHitPrograms(hitProgramDesc);
	pass.Dispatch(GetRenderWidth(), GetRenderHeight());

	renderBackend->TransitionTexture(DiffuseGIRawAux.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(DiffuseGIRaw.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
}

