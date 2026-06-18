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
		// Payload is 10 dwords (position3+color3+normal3+bHit1) after the slim;
		// shadow uses inline RayQuery so no shadow payload contributes.
		TEMP_PSO_RT_GI->Configure(1, sizeof(float) * 10, sizeof(float) * 2);

		const bool bSuccess = TEMP_PSO_RT_GI->InitRS("Shaders\\RaytracedGI.hlsl");

		return bSuccess ? TEMP_PSO_RT_GI : nullptr;
}

void Corona::InitRaytracingSimpleGIPass()
{
	if (renderBackend && renderBackend->UsesSimpleGIFallbackPath())
	{
		// Build the RT PSO; keep the compute
		// fallback PSO around as a safety net so RaytraceGIPass can degrade if the
		// RT PSO failed to build (deferred build can still fail at first Apply).
		PSO_RT_GI_SER = nullptr;
		PSO_RT_GI = CreateRaytracingSimpleGIPSO(false);
		InitSimpleGIFallbackPass();
		if (PSO_RT_GI)
			AppendCpuRuntimeTrace(L"[DiffuseGI][SimpleFallback] real RT GI PSO created");
		else
			AppendCpuRuntimeTrace(L"[DiffuseGI][SimpleFallback] RT GI PSO creation failed; using compute fallback");
		return;
	}

	PSO_RT_GI = CreateRaytracingSimpleGIPSO(false);
	if (bEnableRTDiffuseGISER && renderBackend && renderBackend->SupportsShaderExecutionReordering())
		InitRaytracingSimpleGISERPass();
}

void Corona::InitSimpleGIFallbackPass()
{
	if (!renderBackend || PSO_SIMPLE_GI_FALLBACK)
		return;

	shared_ptr<ComputePipelineStateObject> pso = renderBackend->CreateComputePipelineStateObject();
	if (!pso)
		return;

	pso->BindSRV("DepthTex", 0, 1);
	pso->BindSRV("WorldNormalTex", 1, 1);
	pso->BindUAV("GIResultColor", 0);
	pso->BindCBV("ViewParameter", 0, sizeof(RTGIViewParam));

	if (pso->InitCS(GetAssetFullPath(L"Shaders\\NRISimpleGIFallback.hlsl"), "NRISimpleGIFallback"))
	{
		PSO_SIMPLE_GI_FALLBACK = pso;
		AppendCpuRuntimeTrace(L"[DiffuseGI][SimpleFallback] compute PSO initialized");
	}
	else
	{
		AppendCpuRuntimeTrace(L"[DiffuseGI][SimpleFallback] compute PSO initialization failed");
	}
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

	auto fillViewParam = [&]()
	{
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
		RTGIViewParam.GISamplesPerPixel = std::clamp(SimpleGISamplesPerPixel, 1u, MaxDiffuseGIPointLights);
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
					L", pointLightSamples=" + std::to_wstring(RTGIViewParam.GISamplesPerPixel) +
					L", sky=" + std::to_wstring(RenderFrameDiffuseGISkyLightingEnabled));
			}
		}
	};

	fillViewParam();

	// Prefer the real RT GI path; fall back to the compute approximation only
	// when the RT PSO or the TLAS is not yet available this frame.
	if (renderBackend && renderBackend->UsesSimpleGIFallbackPath())
	{
		if (!pso || !TLAS)
		{
			SimpleGIFallbackPass();
			return;
		}
	}

	if (!TLAS || !pso)
		return;

	if (!EnsureRTMaterialRecordBuffer())
		return;

	RenderGraph rg(renderBackend.get());
	RGTextureRef giSHOutput = rg.ImportTexture("SimpleGI.RawSH", DiffuseGIRawAux.get(), EResourceState::ShaderRead);
	RGTextureRef giColorOutput = rg.ImportTexture("SimpleGI.RawColor", DiffuseGIRaw.get(), EResourceState::ShaderRead);
	RGTextureRef depthInput = rg.ImportTexture("SimpleGI.Depth", UnjitteredDepthBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead);
	RGTextureRef normalInput = rg.ImportTexture("SimpleGI.Normal", NormalBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead);
	RGTextureRef blueNoiseInput = rg.ImportTexture("SimpleGI.BlueNoise", BlueNoiseTex.get(), EResourceState::ShaderRead);
	RGBufferRef rtMaterials = rg.ImportBuffer("SimpleGI.RtMaterials", RTMaterialRecordBuffer.get(), EResourceState::ShaderRead);

	rg.ExportTexture(giSHOutput, EResourceState::ShaderRead);
	rg.ExportTexture(giColorOutput, EResourceState::ShaderRead);
	rg.AddPass(
		"RaytraceGIPass",
		ERGPassFlags::RayTracing,
		[&](RGPassBuilder& builder)
		{
			builder.ReadWriteTexture(giSHOutput, EResourceState::UnorderedAccess)
				.ReadWriteTexture(giColorOutput, EResourceState::UnorderedAccess)
				.ReadTexture(depthInput, EResourceState::ShaderRead)
				.ReadTexture(normalInput, EResourceState::ShaderRead)
				.ReadTexture(blueNoiseInput, EResourceState::ShaderRead)
				.ReadBuffer(rtMaterials, EResourceState::ShaderRead);
		},
		[&, pso](RGContext& ctx)
		{
			Texture* giSHTexture = ctx.GetTexture(giSHOutput);
			Texture* giColorTexture = ctx.GetTexture(giColorOutput);
			Texture* depthTexture = ctx.GetTexture(depthInput);
			Texture* normalTexture = ctx.GetTexture(normalInput);
			Texture* blueNoiseTexture = ctx.GetTexture(blueNoiseInput);
			Buffer* materialBuffer = ctx.GetBuffer(rtMaterials);

			RTPassBuilder pass(*this, pso);
			pass.BeginScene()
				.SetTextureUAV("global", "GIResultSH", giSHTexture)
				.SetTextureUAV("global", "GIResultColor", giColorTexture)
				.SetAccelerationStructure("global", "gRtScene", TLAS)
				.SetTextureSRV("global", "DepthTex", depthTexture)
				.SetTextureSRV("global", "WorldNormalTex", normalTexture)
				.SetTextureSRV("global", "RayNoiseBlueNoiseSource", blueNoiseTexture)
				.SetCBVValue("global", "ViewParameter", &RTGIViewParam)
				.SetSampler("global", "sampleWrap", samplerWrap.get());
			pass.SetBindlessTextureTable("global", "MaterialTextures")
				.SetBufferSRV("global", "RtMaterials", materialBuffer);
			RTSceneHitProgramDesc hitProgramDesc;
			pass.BindSceneHitPrograms(hitProgramDesc);
			pass.Dispatch(GetRenderWidth(), GetRenderHeight());
		});

	rg.Execute();
}

bool Corona::SimpleGIFallbackPass()
{
	if (!renderBackend || !DiffuseGIRaw)
		return false;
	if (!PSO_SIMPLE_GI_FALLBACK)
		InitSimpleGIFallbackPass();
	if (!PSO_SIMPLE_GI_FALLBACK)
		return false;

	renderBackend->EmitGpuCrashMarker("SimpleGIFallbackPass");

	renderBackend->TransitionTexture(DiffuseGIRaw.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	PSO_SIMPLE_GI_FALLBACK->SetTextureSRV("DepthTex", UnjitteredDepthBuffers[ColorBufferWriteIndex].get());
	PSO_SIMPLE_GI_FALLBACK->SetTextureSRV("WorldNormalTex", NormalBuffers[ColorBufferWriteIndex].get());
	PSO_SIMPLE_GI_FALLBACK->SetTextureUAV("GIResultColor", DiffuseGIRaw.get());
	PSO_SIMPLE_GI_FALLBACK->SetCBVValue("ViewParameter", &RTGIViewParam);
	PSO_SIMPLE_GI_FALLBACK->Apply();

	renderBackend->Dispatch((GetRenderWidth() + 7u) / 8u, (GetRenderHeight() + 7u) / 8u, 1u);

	renderBackend->TransitionTexture(DiffuseGIRaw.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	return true;
}
