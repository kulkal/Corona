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

shared_ptr<RTPipelineStateObject> Corona::CreateRaytracingReflectionPSO(bool bUseSER)
{
	shared_ptr<RTPipelineStateObject> tempPSO = renderBackend->CreateRTPipelineStateObject();
	if (!tempPSO)
		return nullptr;

	tempPSO->SetNumInstances(static_cast<uint32_t>(RayTracingInstances.size()));
	if (bUseSER)
	{
		tempPSO->SetShaderDefine("RT_REFLECTION_USE_SER", "1");
		tempPSO->SetShaderDefine("RT_REFLECTION_SER_MATERIAL_HINT_BITS", "8");
		tempPSO->SetShaderLibraryTarget("lib_6_9");
	}

	tempPSO->AddHitGroup("HitGroup", "chs", "");
	//tempPSO->AddHitGroup("ShadowHitGroup", "chsShadow", "");

	tempPSO->AddShader("rayGen", RTPipelineStateObject::RAYGEN);
	
	const RHIShaderStageMask rayGenStage = ToRHIShaderStageMask(RHIShaderStage::RayGeneration);
	const RHIShaderStageMask closestHitStage = ToRHIShaderStageMask(RHIShaderStage::ClosestHit);
	tempPSO->BindUAV("global", MakeRHITextureUAV("ReflectionResult", 0, rayGenStage));
	tempPSO->BindUAV("global", MakeRHITextureUAV("SpecularHitDistanceResult", 1, rayGenStage));
	tempPSO->BindUAV("global", MakeRHITextureUAV("SpecularMotionVectorResult", 2, rayGenStage));
	// ReSTIR specular GI reservoir UAVs (current frame).
	tempPSO->BindUAV("global", MakeRHITextureUAV("ReflReservoirA", 3, rayGenStage));
	tempPSO->BindUAV("global", MakeRHITextureUAV("ReflReservoirB", 4, rayGenStage));
	tempPSO->BindSRV("global", MakeRHIAccelerationStructureSRV("gRtScene", 0, rayGenStage));
	tempPSO->BindSRV("global", MakeRHITextureSRV("DepthTex", 1, rayGenStage));
	tempPSO->BindSRV("global", MakeRHITextureSRV("GeoNormalTex", 2, rayGenStage));
	tempPSO->BindSRV("global", MakeRHITextureSRV("RougnessMetallicTex", 6, rayGenStage));
	tempPSO->BindSRV("global", MakeRHITextureSRV("RayNoiseBlueNoiseSource", 7, rayGenStage));
	tempPSO->BindSRV("global", MakeRHITextureSRV("WorldNormalTex", 8, rayGenStage));
	// ReSTIR specular GI reservoir SRVs (previous frame) + velocity
	// for motion reprojection. Bound unconditionally to keep the
	// root signature stable; the raygen only reads them after the
	// first frame has produced data.
	tempPSO->BindSRV("global", MakeRHITextureSRV("ReflReservoirAPrev", 10, rayGenStage));
	tempPSO->BindSRV("global", MakeRHITextureSRV("ReflReservoirBPrev", 11, rayGenStage));
	tempPSO->BindSRV("global", MakeRHITextureSRV("ReflVelocityTex",   12, rayGenStage));

	tempPSO->BindCBV("global", MakeRHICBV("ViewParameter", 0, sizeof(RTReflectionViewParam), rayGenStage));
	tempPSO->BindSampler("global", MakeRHISampler("sampleWrap", 0, rayGenStage | closestHitStage));
	BindRTBindlessMaterialSchema(*tempPSO, closestHitStage);

	tempPSO->AddShader("miss", RTPipelineStateObject::MISS);
	tempPSO->AddShader("missShadow", RTPipelineStateObject::MISS);

	tempPSO->AddShader("chs", RTPipelineStateObject::HIT);
	tempPSO->BindSRV("chs", MakeRHIBufferSRV("vertices", 3, closestHitStage, RHIBufferViewKind::Raw));
	tempPSO->BindSRV("chs", MakeRHIBufferSRV("indices", 4, closestHitStage, RHIBufferViewKind::Raw));
	tempPSO->BindSRV("chs", MakeRHITextureSRV("AlbedoTex", 5, closestHitStage));
	tempPSO->BindSRV("chs", MakeRHIBufferSRV("InstanceProperty", 9, closestHitStage, RHIBufferViewKind::Raw));
	tempPSO->Configure(1, sizeof(float) * 13, sizeof(float) * 2);

	return tempPSO->InitRS("Shaders\\RaytracedReflection.hlsl") ? tempPSO : nullptr;
}

void Corona::InitRaytracingReflectionPass()
{
	PSO_RT_REFLECTION = CreateRaytracingReflectionPSO(false);
	if (bEnableRTReflectionSER && renderBackend && renderBackend->SupportsShaderExecutionReordering())
		InitRaytracingReflectionSERPass();
}

bool Corona::InitRaytracingReflectionSERPass()
{
	if (PSO_RT_REFLECTION_SER)
		return true;
	if (bRTReflectionSERInitFailed)
		return false;
	if (!renderBackend || !renderBackend->SupportsShaderExecutionReordering())
	{
		bRTReflectionSERInitFailed = true;
		AppendCpuRuntimeTrace(L"[RTReflection][SER] SER skipped: backend does not support shader execution reordering");
		return false;
	}

	PSO_RT_REFLECTION_SER = CreateRaytracingReflectionPSO(true);
	if (!PSO_RT_REFLECTION_SER)
	{
		bRTReflectionSERInitFailed = true;
		AppendCpuRuntimeTrace(L"[RTReflection][SER] SER PSO creation failed");
	}
	return PSO_RT_REFLECTION_SER != nullptr;
}

void Corona::RaytraceReflectionPass()
{
	if (!TLAS || !PSO_RT_REFLECTION || !SpecularGIRaw ||
		!PathTracingSpecularHitDistanceBuffer ||
		!PathTracingSpecularMotionVectorBuffer)
		return;
	renderBackend->EmitGpuCrashMarker("RaytraceReflectionPass");
	shared_ptr<RTPipelineStateObject> pso = PSO_RT_REFLECTION;
	if (bEnableRTReflectionSER && renderBackend && renderBackend->SupportsShaderExecutionReordering() && InitRaytracingReflectionSERPass())
		pso = PSO_RT_REFLECTION_SER;

	const bool bUseBindlessMaterials = UsesRTBindlessMaterials();
	if (bUseBindlessMaterials && !EnsureRTMaterialRecordBuffer())
		return;

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

	// ReSTIR specular GI: transition reservoir UAVs.
	if (ReflectionReservoirA)
		renderBackend->TransitionTexture(ReflectionReservoirA.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	if (ReflectionReservoirB)
		renderBackend->TransitionTexture(ReflectionReservoirB.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	RTPassBuilder pass(*this, pso);
	pass.BeginScene()
		.SetTextureUAV("global", "ReflectionResult", SpecularGIRaw.get())
		.SetTextureUAV("global", "SpecularHitDistanceResult", PathTracingSpecularHitDistanceBuffer.get())
		.SetTextureUAV("global", "SpecularMotionVectorResult", PathTracingSpecularMotionVectorBuffer.get())
		.SetTextureUAV("global", "ReflReservoirA",
			ReflectionReservoirA ? ReflectionReservoirA.get() : SpecularGIRaw.get())
		.SetTextureUAV("global", "ReflReservoirB",
			ReflectionReservoirB ? ReflectionReservoirB.get() : SpecularGIRaw.get())
		.SetAccelerationStructure("global", "gRtScene", TLAS)
		.SetTextureSRV("global", "DepthTex", UnjitteredDepthBuffers[ColorBufferWriteIndex].get())
		.SetTextureSRV("global", "GeoNormalTex", GeomNormalBuffers[ColorBufferWriteIndex].get())
		.SetTextureSRV("global", "RougnessMetallicTex", RoughnessMetalicBuffer.get())
		.SetTextureSRV("global", "RayNoiseBlueNoiseSource", BlueNoiseTex.get())
		.SetTextureSRV("global", "WorldNormalTex", NormalBuffers[ColorBufferWriteIndex].get())
		.SetTextureSRV("global", "ReflReservoirAPrev",
			ReflectionReservoirAPrev ? ReflectionReservoirAPrev.get() : NormalBuffers[ColorBufferWriteIndex].get())
		.SetTextureSRV("global", "ReflReservoirBPrev",
			ReflectionReservoirBPrev ? ReflectionReservoirBPrev.get() : NormalBuffers[ColorBufferWriteIndex].get())
		.SetTextureSRV("global", "ReflVelocityTex",
			VelocityBuffer ? VelocityBuffer.get() : NormalBuffers[ColorBufferWriteIndex].get())
		.SetCBVValue("global", "ViewParameter", &RTReflectionViewParam)
		.SetSampler("global", "sampleWrap", samplerWrap.get());
	if (bUseBindlessMaterials)
	{
		pass.SetBindlessTextureTable("global", "MaterialTextures")
			.SetBufferSRV("global", "RtMaterials", RTMaterialRecordBuffer.get());
	}
	pass.BindSceneHitPrograms();
	pass.Dispatch(GetRenderWidth(), GetRenderHeight());

	renderBackend->TransitionTexture(SpecularGIRaw.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(PathTracingSpecularHitDistanceBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(PathTracingSpecularMotionVectorBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);

	// ReSTIR specular GI: snapshot current reservoir → prev for next frame.
	if (ReflectionReservoirA && ReflectionReservoirAPrev && dx12_rhi)
	{
		renderBackend->TransitionTexture(ReflectionReservoirA.get(), EResourceState::UnorderedAccess, EResourceState::CopySource);
		renderBackend->TransitionTexture(ReflectionReservoirAPrev.get(), EResourceState::ShaderRead, EResourceState::CopyDest);
		dx12_rhi->GetGraphicsCommandList()->CopyResource(
			ReflectionReservoirAPrev->resource.Get(),
			ReflectionReservoirA->resource.Get());
		renderBackend->TransitionTexture(ReflectionReservoirA.get(), EResourceState::CopySource, EResourceState::ShaderRead);
		renderBackend->TransitionTexture(ReflectionReservoirAPrev.get(), EResourceState::CopyDest, EResourceState::ShaderRead);
	}
	if (ReflectionReservoirB && ReflectionReservoirBPrev && dx12_rhi)
	{
		renderBackend->TransitionTexture(ReflectionReservoirB.get(), EResourceState::UnorderedAccess, EResourceState::CopySource);
		renderBackend->TransitionTexture(ReflectionReservoirBPrev.get(), EResourceState::ShaderRead, EResourceState::CopyDest);
		dx12_rhi->GetGraphicsCommandList()->CopyResource(
			ReflectionReservoirBPrev->resource.Get(),
			ReflectionReservoirB->resource.Get());
		renderBackend->TransitionTexture(ReflectionReservoirB.get(), EResourceState::CopySource, EResourceState::ShaderRead);
		renderBackend->TransitionTexture(ReflectionReservoirBPrev.get(), EResourceState::CopyDest, EResourceState::ShaderRead);
	}
	//PIXEndEvent();
}
