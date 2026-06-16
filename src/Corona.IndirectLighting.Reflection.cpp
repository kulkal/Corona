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
	BindRTBindlessGeometrySchema(*tempPSO, closestHitStage);

	tempPSO->AddShader("miss", RTPipelineStateObject::MISS);
	tempPSO->AddShader("missShadow", RTPipelineStateObject::MISS);

	tempPSO->AddShader("chs", RTPipelineStateObject::HIT);
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
		!PathTracingSpecularMotionVectorBuffer ||
		!UnjitteredDepthBuffers[ColorBufferWriteIndex] ||
		!GeomNormalBuffers[ColorBufferWriteIndex] ||
		!RoughnessMetalicBuffer ||
		!BlueNoiseTex ||
		!NormalBuffers[ColorBufferWriteIndex])
		return;
	shared_ptr<RTPipelineStateObject> pso = PSO_RT_REFLECTION;
	if (bEnableRTReflectionSER && renderBackend && renderBackend->SupportsShaderExecutionReordering() && InitRaytracingReflectionSERPass())
		pso = PSO_RT_REFLECTION_SER;

	if (!EnsureRTMaterialRecordBuffer())
		return;

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
	const bool bWriteRRSpecularMotionVectors = IsDLSSRREnabled() && bEnableHybridRRSpecularMotionVectors;
	const bool bWriteRRSpecularHitDistance =
		IsDLSSRREnabled() && !bWriteRRSpecularMotionVectors && bEnableHybridRRSpecularHitDistance;
	RTReflectionViewParam.bWriteRRSpecularMotionVectors = bWriteRRSpecularMotionVectors ? 1u : 0u;
	RTReflectionViewParam.bWriteRRSpecularHitDistance = bWriteRRSpecularHitDistance ? 1u : 0u;
	RTReflectionViewParam.bUseRRSpecularGuideRay =
		(bEnableHybridRRSpecularGuideRay && (bWriteRRSpecularMotionVectors || bWriteRRSpecularHitDistance)) ? 1u : 0u;

	RenderGraph rg(renderBackend.get());
	RGTextureRef reflectionOutput = rg.ImportTexture("Reflection.SpecularGI", SpecularGIRaw.get(), EResourceState::ShaderRead);
	RGTextureRef hitDistanceOutput = rg.ImportTexture("Reflection.HitDistance", PathTracingSpecularHitDistanceBuffer.get(), EResourceState::ShaderRead);
	RGTextureRef motionVectorOutput = rg.ImportTexture("Reflection.MotionVector", PathTracingSpecularMotionVectorBuffer.get(), EResourceState::ShaderRead);
	RGTextureRef reservoirAOutput = ReflectionReservoirA
		? rg.ImportTexture("Reflection.ReservoirA", ReflectionReservoirA.get(), EResourceState::ShaderRead)
		: RGTextureRef{};
	RGTextureRef reservoirBOutput = ReflectionReservoirB
		? rg.ImportTexture("Reflection.ReservoirB", ReflectionReservoirB.get(), EResourceState::ShaderRead)
		: RGTextureRef{};
	RGTextureRef depthInput = rg.ImportTexture("Reflection.Depth", UnjitteredDepthBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead);
	RGTextureRef geomNormalInput = rg.ImportTexture("Reflection.GeomNormal", GeomNormalBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead);
	RGTextureRef roughnessInput = rg.ImportTexture("Reflection.RoughnessMetallic", RoughnessMetalicBuffer.get(), EResourceState::ShaderRead);
	RGTextureRef blueNoiseInput = rg.ImportTexture("Reflection.BlueNoise", BlueNoiseTex.get(), EResourceState::ShaderRead);
	RGTextureRef normalInput = rg.ImportTexture("Reflection.Normal", NormalBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead);
	RGTextureRef reservoirAPrevInput = ReflectionReservoirAPrev
		? rg.ImportTexture("Reflection.ReservoirAPrev", ReflectionReservoirAPrev.get(), EResourceState::ShaderRead)
		: RGTextureRef{};
	RGTextureRef reservoirBPrevInput = ReflectionReservoirBPrev
		? rg.ImportTexture("Reflection.ReservoirBPrev", ReflectionReservoirBPrev.get(), EResourceState::ShaderRead)
		: RGTextureRef{};
	RGTextureRef velocityInput = VelocityBuffer
		? rg.ImportTexture("Reflection.Velocity", VelocityBuffer.get(), EResourceState::ShaderRead)
		: RGTextureRef{};
	RGBufferRef rtMaterials = rg.ImportBuffer("Reflection.RtMaterials", RTMaterialRecordBuffer.get(), EResourceState::ShaderRead);

	rg.ExportTexture(reflectionOutput, EResourceState::ShaderRead);
	rg.ExportTexture(hitDistanceOutput, EResourceState::ShaderRead);
	rg.ExportTexture(motionVectorOutput, EResourceState::ShaderRead);
	if (reservoirAOutput.IsValid())
		rg.ExportTexture(reservoirAOutput, EResourceState::ShaderRead);
	if (reservoirBOutput.IsValid())
		rg.ExportTexture(reservoirBOutput, EResourceState::ShaderRead);
	if (reservoirAPrevInput.IsValid())
		rg.ExportTexture(reservoirAPrevInput, EResourceState::ShaderRead);
	if (reservoirBPrevInput.IsValid())
		rg.ExportTexture(reservoirBPrevInput, EResourceState::ShaderRead);

	rg.AddPass(
		"RaytraceReflectionPass",
		ERGPassFlags::RayTracing,
		[&](RGPassBuilder& builder)
		{
			builder.ReadWriteTexture(reflectionOutput, EResourceState::UnorderedAccess)
				.ReadWriteTexture(hitDistanceOutput, EResourceState::UnorderedAccess)
				.ReadWriteTexture(motionVectorOutput, EResourceState::UnorderedAccess);
			if (reservoirAOutput.IsValid())
				builder.ReadWriteTexture(reservoirAOutput, EResourceState::UnorderedAccess);
			if (reservoirBOutput.IsValid())
				builder.ReadWriteTexture(reservoirBOutput, EResourceState::UnorderedAccess);
			builder.ReadTexture(depthInput, EResourceState::ShaderRead)
				.ReadTexture(geomNormalInput, EResourceState::ShaderRead)
				.ReadTexture(roughnessInput, EResourceState::ShaderRead)
				.ReadTexture(blueNoiseInput, EResourceState::ShaderRead)
				.ReadTexture(normalInput, EResourceState::ShaderRead);
			if (reservoirAPrevInput.IsValid())
				builder.ReadTexture(reservoirAPrevInput, EResourceState::ShaderRead);
			if (reservoirBPrevInput.IsValid())
				builder.ReadTexture(reservoirBPrevInput, EResourceState::ShaderRead);
			if (velocityInput.IsValid())
				builder.ReadTexture(velocityInput, EResourceState::ShaderRead);
			builder.ReadBuffer(rtMaterials, EResourceState::ShaderRead);
		},
		[&, pso](RGContext& ctx)
		{
			Texture* reflectionTexture = ctx.GetTexture(reflectionOutput);
			Texture* hitDistanceTexture = ctx.GetTexture(hitDistanceOutput);
			Texture* motionVectorTexture = ctx.GetTexture(motionVectorOutput);
			Texture* reservoirATexture = reservoirAOutput.IsValid() ? ctx.GetTexture(reservoirAOutput) : reflectionTexture;
			Texture* reservoirBTexture = reservoirBOutput.IsValid() ? ctx.GetTexture(reservoirBOutput) : reflectionTexture;
			Texture* normalTexture = ctx.GetTexture(normalInput);
			Texture* reservoirAPrevTexture = reservoirAPrevInput.IsValid() ? ctx.GetTexture(reservoirAPrevInput) : normalTexture;
			Texture* reservoirBPrevTexture = reservoirBPrevInput.IsValid() ? ctx.GetTexture(reservoirBPrevInput) : normalTexture;
			Texture* velocityTexture = velocityInput.IsValid() ? ctx.GetTexture(velocityInput) : normalTexture;
			Buffer* materialBuffer = ctx.GetBuffer(rtMaterials);

			const FLOAT clearReflection[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			const FLOAT clearHitDistance[4] = { Far, 0.0f, 0.0f, 0.0f };
			const FLOAT clearMotionVector[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			renderBackend->ClearTextureUAVFloat(reflectionTexture, clearReflection);
			renderBackend->ClearTextureUAVFloat(hitDistanceTexture, clearHitDistance);
			renderBackend->ClearTextureUAVFloat(motionVectorTexture, clearMotionVector);

			RTPassBuilder pass(*this, pso);
			pass.BeginScene()
				.SetTextureUAV("global", "ReflectionResult", reflectionTexture)
				.SetTextureUAV("global", "SpecularHitDistanceResult", hitDistanceTexture)
				.SetTextureUAV("global", "SpecularMotionVectorResult", motionVectorTexture)
				.SetTextureUAV("global", "ReflReservoirA", reservoirATexture)
				.SetTextureUAV("global", "ReflReservoirB", reservoirBTexture)
				.SetAccelerationStructure("global", "gRtScene", TLAS)
				.SetTextureSRV("global", "DepthTex", ctx.GetTexture(depthInput))
				.SetTextureSRV("global", "GeoNormalTex", ctx.GetTexture(geomNormalInput))
				.SetTextureSRV("global", "RougnessMetallicTex", ctx.GetTexture(roughnessInput))
				.SetTextureSRV("global", "RayNoiseBlueNoiseSource", ctx.GetTexture(blueNoiseInput))
				.SetTextureSRV("global", "WorldNormalTex", normalTexture)
				.SetTextureSRV("global", "ReflReservoirAPrev", reservoirAPrevTexture)
				.SetTextureSRV("global", "ReflReservoirBPrev", reservoirBPrevTexture)
				.SetTextureSRV("global", "ReflVelocityTex", velocityTexture)
				.SetCBVValue("global", "ViewParameter", &RTReflectionViewParam)
				.SetSampler("global", "sampleWrap", samplerWrap.get());
			pass.SetBindlessTextureTable("global", "MaterialTextures")
				.SetBufferSRV("global", "RtMaterials", materialBuffer);
			RTSceneHitProgramDesc hitProgramDesc;
			pass.BindSceneHitPrograms(hitProgramDesc);
			pass.Dispatch(GetRenderWidth(), GetRenderHeight());
		});

	// ReSTIR specular GI: snapshot current reservoir → prev for next frame.
	if (reservoirAOutput.IsValid() && reservoirAPrevInput.IsValid() && dx12_rhi)
	{
		rg.AddPass(
			"Reflection.CopyReservoirA",
			ERGPassFlags::Copy,
			[&](RGPassBuilder& builder)
			{
				builder.ReadTexture(reservoirAOutput, EResourceState::CopySource)
					.WriteTexture(reservoirAPrevInput, EResourceState::CopyDest);
			},
			[&](RGContext& ctx)
			{
				dx12_rhi->GetGraphicsCommandList()->CopyResource(
					ctx.GetTexture(reservoirAPrevInput)->resource.Get(),
					ctx.GetTexture(reservoirAOutput)->resource.Get());
			});
	}
	if (reservoirBOutput.IsValid() && reservoirBPrevInput.IsValid() && dx12_rhi)
	{
		rg.AddPass(
			"Reflection.CopyReservoirB",
			ERGPassFlags::Copy,
			[&](RGPassBuilder& builder)
			{
				builder.ReadTexture(reservoirBOutput, EResourceState::CopySource)
					.WriteTexture(reservoirBPrevInput, EResourceState::CopyDest);
			},
			[&](RGContext& ctx)
			{
				dx12_rhi->GetGraphicsCommandList()->CopyResource(
					ctx.GetTexture(reservoirBPrevInput)->resource.Get(),
					ctx.GetTexture(reservoirBOutput)->resource.Get());
			});
	}

	rg.Execute();
}
