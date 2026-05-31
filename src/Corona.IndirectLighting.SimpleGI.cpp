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
// For dx12_rhi->GetGraphicsCommandList() used by the ReSTIR
// reservoir-snapshot CopyResource at end-of-pass.
#include "DX12Backend.h"

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
		// ReSTIR diffuse GI reservoir UAVs (current frame).
		TEMP_PSO_RT_GI->BindUAV("global", "DiffuseReservoirA", 2);
		TEMP_PSO_RT_GI->BindUAV("global", "DiffuseReservoirB", 3);
		TEMP_PSO_RT_GI->BindSRV("global", "gRtScene", 0);
		TEMP_PSO_RT_GI->BindSRV("global", "DepthTex", 1);
		TEMP_PSO_RT_GI->BindSRV("global", "WorldNormalTex", 2);
		TEMP_PSO_RT_GI->BindCBV("global", "ViewParameter", 0, sizeof(RTGIViewParam), 1);
		TEMP_PSO_RT_GI->BindSampler("global", "sampleWrap", 0);
		TEMP_PSO_RT_GI->BindSRV("global", "RayNoiseBlueNoiseSource", 7);
		// ReSTIR diffuse GI reservoir SRVs (previous frame) + velocity
		// for motion reprojection. Bound unconditionally so the root
		// signature stays stable; the raygen guards against the first
		// frame via prevM > 0 check.
		TEMP_PSO_RT_GI->BindSRV("global", "DiffuseReservoirAPrev", 8);
		TEMP_PSO_RT_GI->BindSRV("global", "DiffuseReservoirBPrev", 9);
		TEMP_PSO_RT_GI->BindSRV("global", "DiffuseVelocityTex",   10);
		// Prev-frame depth + normal for the disocclusion gate.
		TEMP_PSO_RT_GI->BindSRV("global", "PrevDepthTex",         11);
		TEMP_PSO_RT_GI->BindSRV("global", "PrevNormalTex",        12);

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

	renderBackend->TransitionTexture(DiffuseGIRawAux.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionTexture(DiffuseGIRaw.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	const FLOAT clearGI[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	renderBackend->ClearTextureUAVFloat(DiffuseGIRawAux.get(), clearGI);
	renderBackend->ClearTextureUAVFloat(DiffuseGIRaw.get(), clearGI);

	// ReSTIR diffuse GI: transition reservoir UAVs. Don't clear the
	// current reservoir — the raygen overwrites every pixel each frame.
	if (DiffuseGIReservoirA)
		renderBackend->TransitionTexture(DiffuseGIReservoirA.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	if (DiffuseGIReservoirB)
		renderBackend->TransitionTexture(DiffuseGIReservoirB.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	// One-time clear of the Prev reservoirs so the first frame reads
	// well-defined zeros (prevM == 0 → skip in raygen) instead of
	// uninitialized RGBA32Float memory.
	if (!bDiffuseGIReservoirsCleared && DiffuseGIReservoirAPrev && DiffuseGIReservoirBPrev)
	{
		const FLOAT clearReservoir[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		renderBackend->TransitionTexture(DiffuseGIReservoirAPrev.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->TransitionTexture(DiffuseGIReservoirBPrev.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->ClearTextureUAVFloat(DiffuseGIReservoirAPrev.get(), clearReservoir);
		renderBackend->ClearTextureUAVFloat(DiffuseGIReservoirBPrev.get(), clearReservoir);
		renderBackend->TransitionTexture(DiffuseGIReservoirAPrev.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
		renderBackend->TransitionTexture(DiffuseGIReservoirBPrev.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
		bDiffuseGIReservoirsCleared = true;
	}

	RTGIViewParam.ViewMatrix = glm::transpose(ViewMat);
	RTGIViewParam.InvViewMatrix = glm::transpose(InvViewMat);
	RTGIViewParam.ProjMatrix = glm::transpose(UnjitteredProjMat);
	RTGIViewParam.InvProjMatrix = glm::transpose(UnjitteredInvProjMat);
	RTGIViewParam.PrevUnjitteredViewProjMatrix = glm::transpose(PrevUnjitteredViewProjMat);
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
		.SetTextureUAV("global", "DiffuseReservoirA",
			DiffuseGIReservoirA ? DiffuseGIReservoirA.get() : DiffuseGIRaw.get())
		.SetTextureUAV("global", "DiffuseReservoirB",
			DiffuseGIReservoirB ? DiffuseGIReservoirB.get() : DiffuseGIRaw.get())
		.SetAccelerationStructure("global", "gRtScene", TLAS)
		.SetTextureSRV("global", "DepthTex", UnjitteredDepthBuffers[ColorBufferWriteIndex].get())
		.SetTextureSRV("global", "WorldNormalTex", NormalBuffers[ColorBufferWriteIndex].get())
		.SetTextureSRV("global", "RayNoiseBlueNoiseSource", BlueNoiseTex.get())
		.SetTextureSRV("global", "DiffuseReservoirAPrev",
			DiffuseGIReservoirAPrev ? DiffuseGIReservoirAPrev.get() : NormalBuffers[ColorBufferWriteIndex].get())
		.SetTextureSRV("global", "DiffuseReservoirBPrev",
			DiffuseGIReservoirBPrev ? DiffuseGIReservoirBPrev.get() : NormalBuffers[ColorBufferWriteIndex].get())
		.SetTextureSRV("global", "DiffuseVelocityTex",
			VelocityBuffer ? VelocityBuffer.get() : NormalBuffers[ColorBufferWriteIndex].get())
		.SetTextureSRV("global", "PrevDepthTex",
			UnjitteredDepthBuffers[1 - ColorBufferWriteIndex].get())
		.SetTextureSRV("global", "PrevNormalTex",
			NormalBuffers[1 - ColorBufferWriteIndex].get())
		.SetCBVValue("global", "ViewParameter", &RTGIViewParam)
		.SetSampler("global", "sampleWrap", samplerWrap.get());
	pass.BindSceneHitPrograms();
	pass.Dispatch(GetRenderWidth(), GetRenderHeight());

	renderBackend->TransitionTexture(DiffuseGIRawAux.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(DiffuseGIRaw.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);

	// ReSTIR diffuse GI: snapshot current reservoir → prev for next frame.
	if (DiffuseGIReservoirA && DiffuseGIReservoirAPrev && dx12_rhi)
	{
		renderBackend->TransitionTexture(DiffuseGIReservoirA.get(), EResourceState::UnorderedAccess, EResourceState::CopySource);
		renderBackend->TransitionTexture(DiffuseGIReservoirAPrev.get(), EResourceState::ShaderRead, EResourceState::CopyDest);
		dx12_rhi->GetGraphicsCommandList()->CopyResource(
			DiffuseGIReservoirAPrev->resource.Get(),
			DiffuseGIReservoirA->resource.Get());
		renderBackend->TransitionTexture(DiffuseGIReservoirA.get(), EResourceState::CopySource, EResourceState::ShaderRead);
		renderBackend->TransitionTexture(DiffuseGIReservoirAPrev.get(), EResourceState::CopyDest, EResourceState::ShaderRead);
	}
	if (DiffuseGIReservoirB && DiffuseGIReservoirBPrev && dx12_rhi)
	{
		renderBackend->TransitionTexture(DiffuseGIReservoirB.get(), EResourceState::UnorderedAccess, EResourceState::CopySource);
		renderBackend->TransitionTexture(DiffuseGIReservoirBPrev.get(), EResourceState::ShaderRead, EResourceState::CopyDest);
		dx12_rhi->GetGraphicsCommandList()->CopyResource(
			DiffuseGIReservoirBPrev->resource.Get(),
			DiffuseGIReservoirB->resource.Get());
		renderBackend->TransitionTexture(DiffuseGIReservoirB.get(), EResourceState::CopySource, EResourceState::ShaderRead);
		renderBackend->TransitionTexture(DiffuseGIReservoirBPrev.get(), EResourceState::CopyDest, EResourceState::ShaderRead);
	}
}

