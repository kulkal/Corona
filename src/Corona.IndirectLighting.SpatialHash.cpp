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
// For dx12_rhi->GetGraphicsCommandList() used by the screen-resolve
// snapshot CopyResource at end of pass.
#include "DX12Backend.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>

void AppendCpuRuntimeTrace(const std::wstring& line);

void Corona::InitSpatialHashGIPass()
{
	auto createSpatialHashPSO = [&](const std::string& entryPoint)
	{
		shared_ptr<ComputePipelineStateObject> pso = renderBackend->CreateComputePipelineStateObject();
		if (!pso)
			return shared_ptr<ComputePipelineStateObject>();

		const RHIShaderStageMask computeStage = ToRHIShaderStageMask(RHIShaderStage::Compute);
		pso->BindSRV(MakeRHITextureSRV("DepthTex", 0, computeStage));
		pso->BindSRV(MakeRHITextureSRV("WorldNormalTex", 1, computeStage));
		pso->BindSRV(MakeRHITextureSRV("GeoNormalTex", 2, computeStage));
		pso->BindSRV(MakeRHIBufferSRV("ActiveCellSlotsIn", 5, computeStage));
		pso->BindSRV(MakeRHIBufferSRV("CellPositionIn", 6, computeStage));
		pso->BindSRV(MakeRHIBufferSRV("CellNormalIn", 7, computeStage));
		pso->BindSRV(MakeRHIBufferSRV("TraceSH0In", 8, computeStage));
		pso->BindSRV(MakeRHIBufferSRV("TraceSH1In", 9, computeStage));
		pso->BindSRV(MakeRHIBufferSRV("TraceSH2In", 10, computeStage));
		pso->BindSRV(MakeRHIBufferSRV("TraceSH3In", 11, computeStage));
		pso->BindSRV(MakeRHIBufferSRV("PrevResolvedKeys", 12, computeStage));
		pso->BindSRV(MakeRHIBufferSRV("PrevResolvedSH0", 13, computeStage));
		pso->BindSRV(MakeRHIBufferSRV("PrevResolvedSH1", 14, computeStage));
		pso->BindSRV(MakeRHIBufferSRV("PrevResolvedSH2", 15, computeStage));
		pso->BindSRV(MakeRHIBufferSRV("PrevResolvedSH3", 16, computeStage));
		pso->BindSRV(MakeRHIBufferSRV("ResolvedKeysIn", 17, computeStage));
		pso->BindSRV(MakeRHIBufferSRV("ResolvedSH0In", 18, computeStage));
		pso->BindSRV(MakeRHIBufferSRV("ResolvedSH1In", 19, computeStage));
		pso->BindSRV(MakeRHIBufferSRV("ResolvedSH2In", 20, computeStage));
		pso->BindSRV(MakeRHIBufferSRV("ResolvedSH3In", 21, computeStage));
		pso->BindSRV(MakeRHIBufferSRV("ActiveCounterIn", 22, computeStage));
		// Disocclusion detection inputs for SpatialHashQuery: prev
		// frame depth+normal (sampled via motion-reprojected pixel)
		// + velocity. Used to reset history toward the ambient
		// fallback on newly revealed pixels so previously-cached
		// neighbours can't leak into the disoccluded surface.
		pso->BindSRV(MakeRHITextureSRV("VelocityTex", 23, computeStage));
		pso->BindSRV(MakeRHITextureSRV("PrevDepthTex", 24, computeStage));
		pso->BindSRV(MakeRHITextureSRV("PrevNormalTex", 25, computeStage));
		// Option A screen-resolve — prev filtered output for temporal.
		pso->BindSRV(MakeRHITextureSRV("InDiffuseGIFilteredPrev", 26, computeStage));
		// And the resolve's output UAV.
		pso->BindUAV(MakeRHITextureUAV("OutDiffuseGIFiltered", 13, computeStage));
		pso->BindUAV(MakeRHIBufferUAV("ActiveFlagsOut", 0, computeStage));
		pso->BindUAV(MakeRHIBufferUAV("CellPositionOut", 1, computeStage));
		pso->BindUAV(MakeRHIBufferUAV("CellNormalOut", 2, computeStage));
		pso->BindUAV(MakeRHIBufferUAV("CellScoreOut", 3, computeStage));
		pso->BindUAV(MakeRHIBufferUAV("ResolvedKeysOut", 4, computeStage));
		pso->BindUAV(MakeRHIBufferUAV("ResolvedSH0Out", 5, computeStage));
		pso->BindUAV(MakeRHIBufferUAV("ResolvedSH1Out", 6, computeStage));
		pso->BindUAV(MakeRHIBufferUAV("ResolvedSH2Out", 7, computeStage));
		pso->BindUAV(MakeRHIBufferUAV("ResolvedSH3Out", 8, computeStage));
		pso->BindUAV(MakeRHITextureUAV("OutGIHashColor", 9, computeStage));
		pso->BindUAV(MakeRHITextureUAV("OutGIHashSH", 10, computeStage));
		pso->BindUAV(MakeRHIBufferUAV("ActiveCellSlotsOut", 11, computeStage));
		pso->BindUAV(MakeRHIBufferUAV("ActiveCounterOut", 12, computeStage));
		pso->BindUAV(MakeRHIBufferUAV("CellLightMaskOut", 14, computeStage));
		// Octahedral DDGI (GIMode==1): per-ray scratch + irradiance/depth atlas.
		pso->BindSRV(MakeRHIBufferSRV("OctRayDataIn", 27, computeStage));
		pso->BindSRV(MakeRHIBufferSRV("OctIrradianceIn", 28, computeStage));
		pso->BindSRV(MakeRHIBufferSRV("OctDepthIn", 29, computeStage));
		pso->BindSRV(MakeRHIBufferSRV("OctCellKeyIn", 30, computeStage));
		pso->BindUAV(MakeRHIBufferUAV("OctIrradianceOut", 15, computeStage));
		pso->BindUAV(MakeRHIBufferUAV("OctDepthOut", 16, computeStage));
		pso->BindUAV(MakeRHIBufferUAV("OctCellKeyOut", 17, computeStage));
		pso->BindUAV(MakeRHIBufferUAV("OctReservoirRayOut", 18, computeStage));
		pso->BindUAV(MakeRHIBufferUAV("OctReservoirRadianceOut", 19, computeStage));
		pso->BindCBV(MakeRHICBV("SpatialHashGIConstant", 0, sizeof(SpatialHashGIConstant), computeStage));

		if (!pso->InitCS(GetAssetFullPath(L"Shaders\\SpatialHashDiffuseGI.hlsl"), entryPoint))
			return shared_ptr<ComputePipelineStateObject>();

		return pso;
	};

	SpatialHashGIClearPSO = createSpatialHashPSO("SpatialHashClear");
	SpatialHashGIUpdatePSO = createSpatialHashPSO("SpatialHashUpdate");
	SpatialHashGIResolvePSO = createSpatialHashPSO("SpatialHashResolve");
	SpatialHashGIQueryPSO = createSpatialHashPSO("SpatialHashQuery");
	SpatialHashGIScreenResolvePSO = createSpatialHashPSO("SpatialHashScreenResolve");
	SpatialHashGIOctBlendPSO = createSpatialHashPSO("SpatialHashOctBlend");
	SpatialHashGIOctDepthBlendPSO = createSpatialHashPSO("SpatialHashOctDepthBlend");
}

shared_ptr<RTPipelineStateObject> Corona::CreateRaytracingSpatialHashGIPSO(bool bUseSER)
{
		shared_ptr<RTPipelineStateObject> tempPSO = renderBackend->CreateRTPipelineStateObject();
		if (!tempPSO)
			return nullptr;
		if (bUseSER)
		{
			tempPSO->SetShaderDefine("RT_DIFFUSE_GI_USE_SER", "1");
			tempPSO->SetShaderDefine("RT_DIFFUSE_GI_SER_MATERIAL_HINT_BITS", "8");
			tempPSO->SetShaderLibraryTarget("lib_6_9");
		}
		tempPSO->SetNumInstances(static_cast<uint32_t>(RayTracingInstances.size()));

		tempPSO->AddHitGroup("HitGroup", "chs", "");
		tempPSO->AddShader("rayGen", RTPipelineStateObject::RAYGEN);

		const RHIShaderStageMask rayGenStage = ToRHIShaderStageMask(RHIShaderStage::RayGeneration);
		const RHIShaderStageMask closestHitStage = ToRHIShaderStageMask(RHIShaderStage::ClosestHit);
		tempPSO->BindUAV("global", MakeRHIBufferUAV("TraceSH0", 0, rayGenStage));
		tempPSO->BindUAV("global", MakeRHIBufferUAV("TraceSH1", 1, rayGenStage));
		tempPSO->BindUAV("global", MakeRHIBufferUAV("TraceSH2", 2, rayGenStage));
		tempPSO->BindUAV("global", MakeRHIBufferUAV("TraceSH3", 3, rayGenStage));
		tempPSO->BindUAV("global", MakeRHIBufferUAV("OctRayData", 4, rayGenStage));
		tempPSO->BindSRV("global", MakeRHIAccelerationStructureSRV("gRtScene", 0, rayGenStage));
		tempPSO->BindSRV("global", MakeRHIBufferSRV("CellKeys", 1, rayGenStage));
		tempPSO->BindSRV("global", MakeRHIBufferSRV("CellPosition", 2, rayGenStage));
		tempPSO->BindSRV("global", MakeRHIBufferSRV("CellNormal", 3, rayGenStage));
		tempPSO->BindSRV("global", MakeRHITextureSRV("RayNoiseBlueNoiseSource", 4, rayGenStage));
		tempPSO->BindSRV("global", MakeRHIBufferSRV("ActiveCellSlots", 9, rayGenStage));
		tempPSO->BindSRV("global", MakeRHIBufferSRV("ActiveCounter", 10, rayGenStage));
		tempPSO->BindSRV("global", MakeRHIBufferSRV("CellLightMask", 11, rayGenStage));
		tempPSO->BindSRV("global", MakeRHIBufferSRV("OctIrradianceConverge", 12, rayGenStage));
		tempPSO->BindCBV("global", MakeRHICBV("ViewParameter", 0, sizeof(RTSpatialHashGIViewParamCB), rayGenStage));
		tempPSO->BindSampler("global", MakeRHISampler("sampleWrap", 0, rayGenStage | closestHitStage));
		BindRTBindlessMaterialSchema(*tempPSO, closestHitStage);

		tempPSO->AddShader("miss", RTPipelineStateObject::MISS);
		tempPSO->AddShader("missShadow", RTPipelineStateObject::MISS);

		tempPSO->AddShader("chs", RTPipelineStateObject::HIT);
		tempPSO->BindSRV("chs", MakeRHIBufferSRV("vertices", 5, closestHitStage, RHIBufferViewKind::Raw));
		tempPSO->BindSRV("chs", MakeRHIBufferSRV("indices", 6, closestHitStage, RHIBufferViewKind::Raw));
		if (!UsesRTBindlessMaterials())
			tempPSO->BindSRV("chs", MakeRHITextureSRV("AlbedoTex", 7, closestHitStage));
		tempPSO->BindSRV("chs", MakeRHIBufferSRV("InstanceProperty", 8, closestHitStage, RHIBufferViewKind::Raw));
		tempPSO->Configure(1, sizeof(float) * 12, sizeof(float) * 2);

		return tempPSO->InitRS("Shaders\\SpatialHashCellGI.hlsl") ? tempPSO : nullptr;
}

shared_ptr<RTPipelineStateObject> Corona::CreateRaytracingSpatialHashPrimaryDeepSeedPSO()
{
	shared_ptr<RTPipelineStateObject> tempPSO = renderBackend->CreateRTPipelineStateObject();
	if (!tempPSO)
		return nullptr;

	tempPSO->SetNumInstances(static_cast<uint32_t>(RayTracingInstances.size()));
	tempPSO->AddHitGroup("HitGroup", "chs", "");
	tempPSO->AddShader("rayGen", RTPipelineStateObject::RAYGEN);

	const RHIShaderStageMask rayGenStage = ToRHIShaderStageMask(RHIShaderStage::RayGeneration);
	const RHIShaderStageMask closestHitStage = ToRHIShaderStageMask(RHIShaderStage::ClosestHit);
	tempPSO->BindSRV("global", MakeRHIAccelerationStructureSRV("gRtScene", 0, rayGenStage));
	tempPSO->BindSRV("global", MakeRHITextureSRV("DepthTex", 1, rayGenStage));
	tempPSO->BindUAV("global", MakeRHIBufferUAV("ActiveFlagsOut", 0, rayGenStage));
	tempPSO->BindUAV("global", MakeRHIBufferUAV("CellPositionOut", 1, rayGenStage));
	tempPSO->BindUAV("global", MakeRHIBufferUAV("CellNormalOut", 2, rayGenStage));
	tempPSO->BindUAV("global", MakeRHIBufferUAV("CellScoreOut", 3, rayGenStage));
	tempPSO->BindUAV("global", MakeRHIBufferUAV("ResolvedKeysOut", 4, rayGenStage));
	tempPSO->BindUAV("global", MakeRHIBufferUAV("ResolvedSH0Out", 5, rayGenStage));
	tempPSO->BindUAV("global", MakeRHIBufferUAV("ActiveCellSlotsOut", 6, rayGenStage));
	tempPSO->BindUAV("global", MakeRHIBufferUAV("ActiveCounterOut", 7, rayGenStage));
	tempPSO->BindUAV("global", MakeRHIBufferUAV("CellLightMaskOut", 8, rayGenStage));
	tempPSO->BindCBV("global", MakeRHICBV("SpatialHashGIConstant", 0, sizeof(SpatialHashGIConstant), rayGenStage));
	tempPSO->BindCBV("global", MakeRHICBV("PrimaryDeepSeedConstant", 1, sizeof(RTSpatialHashPrimaryDeepSeedParamCB), rayGenStage));

	tempPSO->AddShader("miss", RTPipelineStateObject::MISS);

	tempPSO->AddShader("chs", RTPipelineStateObject::HIT);
	tempPSO->BindSRV("chs", MakeRHIBufferSRV("vertices", 2, closestHitStage, RHIBufferViewKind::Raw));
	tempPSO->BindSRV("chs", MakeRHIBufferSRV("indices", 3, closestHitStage, RHIBufferViewKind::Raw));
	tempPSO->BindSRV("chs", MakeRHIBufferSRV("InstanceProperty", 4, closestHitStage, RHIBufferViewKind::Raw));
	tempPSO->Configure(1, sizeof(float) * 8, sizeof(float) * 2);

	return tempPSO->InitRS("Shaders\\SpatialHashPrimaryDeepSeed.hlsl") ? tempPSO : nullptr;
}

void Corona::InitRaytracingSpatialHashPass()
{
	PSO_RT_SPATIAL_HASH_GI = CreateRaytracingSpatialHashGIPSO(false);
	if (bEnableRTDiffuseGISER && renderBackend && renderBackend->SupportsShaderExecutionReordering())
		InitRaytracingSpatialHashGISERPass();
}

bool Corona::InitRaytracingSpatialHashGISERPass()
{
	if (PSO_RT_SPATIAL_HASH_GI_SER)
		return true;
	if (bRTDiffuseGISpatialHashSERInitFailed)
		return false;
	if (!renderBackend)
		return false;
	if (!renderBackend->SupportsShaderExecutionReordering())
	{
		bRTDiffuseGISpatialHashSERInitFailed = true;
		AppendCpuRuntimeTrace(L"[RTDiffuseGI][SER] Spatial Hash SER skipped: backend does not support shader execution reordering");
		return false;
	}

	PSO_RT_SPATIAL_HASH_GI_SER = CreateRaytracingSpatialHashGIPSO(true);
	if (!PSO_RT_SPATIAL_HASH_GI_SER)
	{
		bRTDiffuseGISpatialHashSERInitFailed = true;
		AppendCpuRuntimeTrace(L"[RTDiffuseGI][SER] Spatial Hash SER PSO creation failed");
	}
	return PSO_RT_SPATIAL_HASH_GI_SER != nullptr;
}

bool Corona::InitRaytracingSpatialHashPrimaryDeepSeedPass()
{
	if (PSO_RT_SPATIAL_HASH_PRIMARY_DEEP_SEED)
		return true;
	if (bRTSpatialHashPrimaryDeepSeedInitFailed)
		return false;
	if (!renderBackend)
		return false;

	PSO_RT_SPATIAL_HASH_PRIMARY_DEEP_SEED = CreateRaytracingSpatialHashPrimaryDeepSeedPSO();
	if (!PSO_RT_SPATIAL_HASH_PRIMARY_DEEP_SEED)
	{
		bRTSpatialHashPrimaryDeepSeedInitFailed = true;
		AppendCpuRuntimeTrace(L"[SpatialHashGI][PrimaryDeepSeed] PSO creation failed");
	}
	return PSO_RT_SPATIAL_HASH_PRIMARY_DEEP_SEED != nullptr;
}

bool Corona::SpatialHashPrimaryDeepSeedPass()
{
	if (!bEnableSpatialHashPrimaryDeepSeed)
		return true;
	if (!TLAS || !UnjitteredDepthBuffers[ColorBufferWriteIndex])
		return false;
	if (!InitRaytracingSpatialHashPrimaryDeepSeedPass())
		return false;

	RTSpatialHashPrimaryDeepSeedParam.PixelStride =
		std::clamp(SpatialHashPrimaryDeepSeedPixelStride, 1u, 16u);
	RTSpatialHashPrimaryDeepSeedParam.FrameIndex = RenderFrameIndex;

	static UINT sTraceLogCount = 0;
	if (sTraceLogCount < 8)
	{
		AppendCpuRuntimeTrace(
			L"[SpatialHashGI][PrimaryDeepSeed] enabled stride=" +
			std::to_wstring(RTSpatialHashPrimaryDeepSeedParam.PixelStride) +
			L", approxPixelFraction=1/" +
			std::to_wstring(RTSpatialHashPrimaryDeepSeedParam.PixelStride * RTSpatialHashPrimaryDeepSeedParam.PixelStride));
		++sTraceLogCount;
	}

	renderBackend->EmitGpuCrashMarker("SpatialHashPrimaryDeepSeedPass");

	RTPassBuilder pass(*this, PSO_RT_SPATIAL_HASH_PRIMARY_DEEP_SEED);
	pass.BeginScene()
		.SetAccelerationStructure("global", "gRtScene", TLAS)
		.SetTextureSRV("global", "DepthTex", UnjitteredDepthBuffers[ColorBufferWriteIndex].get())
		.SetBufferUAV("global", "ActiveFlagsOut", SpatialHashGIActiveFlags.get())
		.SetBufferUAV("global", "CellPositionOut", SpatialHashGICellPosition.get())
		.SetBufferUAV("global", "CellNormalOut", SpatialHashGICellNormal.get())
		.SetBufferUAV("global", "CellScoreOut", SpatialHashGICellScore.get())
		.SetBufferUAV("global", "ResolvedKeysOut", SpatialHashGIResolvedKeys[0].get())
		.SetBufferUAV("global", "ResolvedSH0Out", SpatialHashGIResolvedSH[0][0].get())
		.SetBufferUAV("global", "ActiveCellSlotsOut", SpatialHashGIActiveCellSlots.get())
		.SetBufferUAV("global", "ActiveCounterOut", SpatialHashGIActiveCounter.get())
		.SetBufferUAV("global", "CellLightMaskOut", SpatialHashGICellLightMask.get())
		.SetCBVValue("global", "SpatialHashGIConstant", &SpatialHashGICB)
		.SetCBVValue("global", "PrimaryDeepSeedConstant", &RTSpatialHashPrimaryDeepSeedParam);

	RTSceneHitProgramDesc hitProgramDesc;
	hitProgramDesc.bBindDiffuseTexture = false;
	pass.BindSceneHitPrograms(hitProgramDesc);
	pass.Dispatch(GetRenderWidth(), GetRenderHeight());
	return true;
}

void Corona::PrepareSpatialHashGIFrameParams(UINT32 spatialHashTraceCellBudget)
{
	const UINT32 giMode = std::clamp(SpatialHashGICB.GIMode, 0u, 1u);
	const UINT32 octTraceCellBudget = std::min(
		SpatialHashGIOctTraceCellBudget,
		std::min(SpatialHashGIOctCellCapacity, SpatialHashGIActiveCellCapacity));
	const UINT32 traceCellBudgetForMode = (giMode == 1u) ? octTraceCellBudget : spatialHashTraceCellBudget;

	SpatialHashGICB.HashEntryCount = SpatialHashGIEntryCount;
	SpatialHashGICB.HashEntryMask = SpatialHashGIEntryCount - 1u;
	SpatialHashGICB.ActiveCellCapacity = SpatialHashGIActiveCellCapacity;
	SpatialHashGICB.TraceCellBudget = traceCellBudgetForMode;
	{
		const float fallbackStrength = std::clamp(SpatialHashSkyFallbackStrength, 0.0f, 1.0f);
		const glm::vec3 skyAvg = 0.5f * (SkyColorTop + SkyColorBottom) * RenderFrameDiffuseGISkyIntensity;
		SpatialHashGICB.SpatialHashSkyAmbient = glm::vec4(skyAvg * fallbackStrength, fallbackStrength);
	}
	SpatialHashGICB.InvViewMatrix = glm::transpose(InvViewMat);
	SpatialHashGICB.InvProjMatrix = glm::transpose(UnjitteredInvProjMat);
	SpatialHashGICB.ProjectionParams = FrameProjectionParams;
	SpatialHashGICB.RTSize = glm::vec2(GetRenderWidth(), GetRenderHeight());
	SpatialHashGICB.FrameIndex = RenderFrameIndex;
	SpatialHashGICB.HistoryValid = bSpatialHashGIHistoryValid ? 1u : 0u;
	SpatialHashGICB.MaxProbeSteps = std::clamp(SpatialHashGICB.MaxProbeSteps, 1u, 16u);
	SpatialHashGICB.CellSize = std::clamp(SpatialHashGICB.CellSize, 4.0f, 256.0f);
	SpatialHashGICB.HistorySampleDecay = std::clamp(SpatialHashGICB.HistorySampleDecay, 0.0f, 1.0f);
	SpatialHashGICB.SmoothingStrength = std::clamp(SpatialHashGICB.SmoothingStrength, 0.0f, 1.0f);
	SpatialHashGICB.TemporalAlpha = std::clamp(SpatialHashGICB.TemporalAlpha, 0.02f, 1.0f);
	SpatialHashGICB.InterpolationStrength = std::clamp(SpatialHashGICB.InterpolationStrength, 0.0f, 1.0f);
	SpatialHashGICB.GIMode = giMode;
	// Use the shared, stable-Id-ordered ReSTIR light table so the cell light mask
	// this pass writes is bit-compatible with the ReSTIR shadow candidate list and
	// the ReSTIR LightingPS feed (which use the same table). Cover the full
	// MaxDiffuseGIPointLights set so the mask has a valid bit for every shadow
	// candidate (a smaller DiffuseGIPointLightLimit would zero the high bits and the
	// shadow pass would wrongly cull those lights).
	{
		std::vector<const PointLightState*> sharedReSTIRLights;
		BuildReSTIRSharedPointLights(sharedReSTIRLights);
		FillPointLightParamsFromList(
			SpatialHashGICB.PointLights,
			SpatialHashGICB.PointLightCount,
			sharedReSTIRLights,
			MaxDiffuseGIPointLights);
		// Cache the exact table this pass's cell light mask is built from. The shadow
		// pass (next frame, runs before this pass) and the ReSTIR LightingPS feed read
		// this so their candidate index stays bit-aligned with the persistent mask.
		ReSTIRMaskLightCache.assign(
			SpatialHashGICB.PointLights,
			SpatialHashGICB.PointLights + SpatialHashGICB.PointLightCount);
		bReSTIRMaskLightCacheValid = SpatialHashGICB.PointLightCount > 0;
	}
	SpatialHashGICB.DebugDiffuseGIOverride =
		bDebugForceDiffuseGIColor ?
		glm::vec4(glm::max(DebugForceDiffuseGIColor, glm::vec3(0.0f)), 1.0f) :
		glm::vec4(0.0f);
	if (bDebugForceDiffuseGIColor)
	{
		static bool sLoggedForcedDiffuseGIColor = false;
		if (!sLoggedForcedDiffuseGIColor)
		{
			sLoggedForcedDiffuseGIColor = true;
			AppendCpuRuntimeTrace(
				L"[SpatialHashGI][Debug] forcing diffuse GI output color=" +
				std::to_wstring(DebugForceDiffuseGIColor.x) + L"," +
				std::to_wstring(DebugForceDiffuseGIColor.y) + L"," +
				std::to_wstring(DebugForceDiffuseGIColor.z));
		}
	}

	RTSpatialHashGIViewParam.LightDir = glm::vec4(RenderFrameNormalizedLightDir, LightIntensity);
	RTSpatialHashGIViewParam.HashEntryCount = traceCellBudgetForMode;
	RTSpatialHashGIViewParam.FrameCounter = RenderFrameIndex;
	RTSpatialHashGIViewParam.BlueNoiseOffsetStride = RTGIViewParam.BlueNoiseOffsetStride;
	RTSpatialHashGIViewParam.NoiseMode = RenderFrameRayNoiseMode;
	RTSpatialHashGIViewParam.RaysPerCell = std::clamp(RTSpatialHashGIViewParam.RaysPerCell, 1u, 8u);
	RTSpatialHashGIViewParam.MaxBounces = std::clamp(RTSpatialHashGIViewParam.MaxBounces, 1u, 8u);
	RTSpatialHashGIViewParam.CellSize = SpatialHashGICB.CellSize;
	RTSpatialHashGIViewParam.RayBias = std::clamp(SpatialHashGICB.CellSize * 0.02f, 0.05f, 0.5f);
	RTSpatialHashGIViewParam.ViewSpreadAngle = glm::tan(Fov * 0.5f) / (0.5f * GetRenderHeight());
	RTSpatialHashGIViewParam.SkyColorTop = SkyColorTop;
	RTSpatialHashGIViewParam.SkyIntensity = RenderFrameDiffuseGISkyIntensity;
	RTSpatialHashGIViewParam.SkyColorBottom = SkyColorBottom;
	RTSpatialHashGIViewParam.LightColor = RenderFrameLightColor;
	RTSpatialHashGIViewParam.ActiveCellCapacity = SpatialHashGIActiveCellCapacity;
	RTSpatialHashGIViewParam.bIncludeSkyLighting = RenderFrameDiffuseGISkyLightingEnabled;
	RTSpatialHashGIViewParam.HashEntryMask = SpatialHashGIEntryCount - 1u;
	RTSpatialHashGIViewParam.MaxProbeSteps = SpatialHashGICB.MaxProbeSteps;
	RTSpatialHashGIViewParam.GIMode = SpatialHashGICB.GIMode;
	RTSpatialHashGIViewParam.OctCellCapacity = SpatialHashGIOctCellCapacity;
	RTSpatialHashGIViewParam.OctRaysPerCell = SpatialHashGIOctRaysPerCell;
	RTSpatialHashGIViewParam.CameraPosition = glm::vec4(glm::vec3(InvViewMat[3]), 0.0f);
	RTSpatialHashGIViewParam.SpatialHashLevelParams = SpatialHashGICB.SpatialHashLevelParams;
	FillPointLightParams(
		RTSpatialHashGIViewParam.PointLights,
		RTSpatialHashGIViewParam.PointLightCount,
		std::min(MaxDiffuseGIPointLights, DiffuseGIPointLightLimit));

	if (SpatialHashGIFrameParamFrameIndex != RenderFrameIndex)
	{
		UINT32 h = 2166136261u;
		auto mix = [&h](const void* data, size_t bytes)
		{
			const uint8_t* p = static_cast<const uint8_t*>(data);
			for (size_t i = 0; i < bytes; ++i) { h ^= p[i]; h *= 16777619u; }
		};
		mix(&RTSpatialHashGIViewParam.PointLightCount, sizeof(UINT32));
		mix(RTSpatialHashGIViewParam.PointLights,
			sizeof(PointLightParam) * RTSpatialHashGIViewParam.PointLightCount);
		mix(&RTSpatialHashGIViewParam.LightDir, sizeof(glm::vec4));
		mix(&RTSpatialHashGIViewParam.LightColor, sizeof(glm::vec3));
		mix(&RTSpatialHashGIViewParam.SkyColorTop, sizeof(glm::vec3));
		mix(&RTSpatialHashGIViewParam.SkyColorBottom, sizeof(glm::vec3));
		mix(&RTSpatialHashGIViewParam.SkyIntensity, sizeof(float));
		mix(&RTSpatialHashGIViewParam.bIncludeSkyLighting, sizeof(UINT32));
		const bool lightingChanged = (h != LastSpatialHashLightingHash);
		LastSpatialHashLightingHash = h;
		SpatialHashGILightingChangedThisFrame = lightingChanged ? 1.0f : 0.0f;
		SpatialHashGIFrameParamFrameIndex = RenderFrameIndex;
	}
	SpatialHashGICB.LightingChangedFlag = SpatialHashGILightingChangedThisFrame;
	RTSpatialHashGIViewParam.PointLightPadding.x = SpatialHashGILightingChangedThisFrame;

	{
		static float sLastLoggedLightIntensity = -1.0f;
		static UINT32 sLastLoggedPointLightCount = 0xFFFFFFFFu;
		if (std::abs(sLastLoggedLightIntensity - LightIntensity) > 0.0001f ||
			sLastLoggedPointLightCount != RTSpatialHashGIViewParam.PointLightCount)
		{
			sLastLoggedLightIntensity = LightIntensity;
			sLastLoggedPointLightCount = RTSpatialHashGIViewParam.PointLightCount;
			AppendCpuRuntimeTrace(
				L"[DiffuseGI][SpatialHash] lightIntensity=" + std::to_wstring(LightIntensity) +
				L", lightDir=" + std::to_wstring(RenderFrameNormalizedLightDir.x) + L"," +
				std::to_wstring(RenderFrameNormalizedLightDir.y) + L"," +
				std::to_wstring(RenderFrameNormalizedLightDir.z) +
				L", pointLights=" + std::to_wstring(RTSpatialHashGIViewParam.PointLightCount) +
				L", pointLightLimit=" + std::to_wstring(DiffuseGIPointLightLimit) +
				L", sky=" + std::to_wstring(RenderFrameDiffuseGISkyLightingEnabled));
		}
	}
}

bool Corona::SpatialHashLightMaskPass()
{
	auto hasSpatialHashSHBuffers = [&]()
	{
		for (UINT coefficientIndex = 0; coefficientIndex < SpatialHashGISHCoefficientCount; ++coefficientIndex)
		{
			if (!SpatialHashGIResolvedSH[0][coefficientIndex])
				return false;
		}
		return true;
	};

	if (SpatialHashLightMaskFrameIndex == RenderFrameIndex)
		return true;

	if (!SpatialHashGIClearPSO || !SpatialHashGIUpdatePSO ||
		!SpatialHashGIActiveFlags || !SpatialHashGIActiveCellSlots || !SpatialHashGIActiveCounter ||
		!SpatialHashGICellPosition || !SpatialHashGICellNormal || !SpatialHashGICellScore ||
		!SpatialHashGICellLightMask || !SpatialHashGIResolvedKeys[0] || !hasSpatialHashSHBuffers() ||
		!UnjitteredDepthBuffers[ColorBufferWriteIndex] || !NormalBuffers[ColorBufferWriteIndex] ||
		!GeomNormalBuffers[ColorBufferWriteIndex])
		return false;

	renderBackend->EmitGpuCrashMarker("SpatialHashLightMaskPass");

	const UINT32 cacheIndex = 0u;
	const UINT32 spatialHashTraceCellBudget = std::min(SpatialHashGITraceCellBudget, SpatialHashGIActiveCellCapacity);
	PrepareSpatialHashGIFrameParams(spatialHashTraceCellBudget);

	renderBackend->TransitionBuffer(SpatialHashGIActiveFlags.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionBuffer(SpatialHashGIActiveCellSlots.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionBuffer(SpatialHashGIActiveCounter.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionBuffer(SpatialHashGICellPosition.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionBuffer(SpatialHashGICellNormal.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionBuffer(SpatialHashGICellScore.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionBuffer(SpatialHashGICellLightMask.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionBuffer(SpatialHashGIResolvedKeys[cacheIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	for (UINT coefficientIndex = 0; coefficientIndex < SpatialHashGISHCoefficientCount; ++coefficientIndex)
		renderBackend->TransitionBuffer(SpatialHashGIResolvedSH[cacheIndex][coefficientIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	SpatialHashGIClearPSO->SetBufferUAV("ActiveFlagsOut", SpatialHashGIActiveFlags.get());
	SpatialHashGIClearPSO->SetBufferUAV("CellPositionOut", SpatialHashGICellPosition.get());
	SpatialHashGIClearPSO->SetBufferUAV("CellNormalOut", SpatialHashGICellNormal.get());
	SpatialHashGIClearPSO->SetBufferUAV("CellScoreOut", SpatialHashGICellScore.get());
	SpatialHashGIClearPSO->SetBufferUAV("CellLightMaskOut", SpatialHashGICellLightMask.get());
	SpatialHashGIClearPSO->SetBufferUAV("ResolvedKeysOut", SpatialHashGIResolvedKeys[cacheIndex].get());
	SpatialHashGIClearPSO->SetBufferUAV("ResolvedSH0Out", SpatialHashGIResolvedSH[cacheIndex][0].get());
	SpatialHashGIClearPSO->SetBufferUAV("ResolvedSH1Out", SpatialHashGIResolvedSH[cacheIndex][1].get());
	SpatialHashGIClearPSO->SetBufferUAV("ResolvedSH2Out", SpatialHashGIResolvedSH[cacheIndex][2].get());
	SpatialHashGIClearPSO->SetBufferUAV("ResolvedSH3Out", SpatialHashGIResolvedSH[cacheIndex][3].get());
	SpatialHashGIClearPSO->SetBufferUAV("ActiveCellSlotsOut", SpatialHashGIActiveCellSlots.get());
	SpatialHashGIClearPSO->SetBufferUAV("ActiveCounterOut", SpatialHashGIActiveCounter.get());
	SpatialHashGIClearPSO->SetCBVValue("SpatialHashGIConstant", &SpatialHashGICB);
	SpatialHashGIClearPSO->Apply();
	renderBackend->Dispatch((SpatialHashGIEntryCount + 255u) / 256u, 1u, 1u);

	renderBackend->TransitionBuffer(SpatialHashGIActiveFlags.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionBuffer(SpatialHashGICellScore.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionBuffer(SpatialHashGIActiveCounter.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionBuffer(SpatialHashGIResolvedKeys[cacheIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	for (UINT coefficientIndex = 0; coefficientIndex < SpatialHashGISHCoefficientCount; ++coefficientIndex)
		renderBackend->TransitionBuffer(SpatialHashGIResolvedSH[cacheIndex][coefficientIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionBuffer(SpatialHashGIActiveFlags.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionBuffer(SpatialHashGICellScore.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionBuffer(SpatialHashGIActiveCounter.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionBuffer(SpatialHashGIResolvedKeys[cacheIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionBuffer(SpatialHashGIResolvedSH[cacheIndex][0].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	SpatialHashGIUpdatePSO->SetTextureSRV("DepthTex", UnjitteredDepthBuffers[ColorBufferWriteIndex].get());
	SpatialHashGIUpdatePSO->SetTextureSRV("WorldNormalTex", NormalBuffers[ColorBufferWriteIndex].get());
	SpatialHashGIUpdatePSO->SetTextureSRV("GeoNormalTex", GeomNormalBuffers[ColorBufferWriteIndex].get());
	SpatialHashGIUpdatePSO->SetBufferUAV("ActiveFlagsOut", SpatialHashGIActiveFlags.get());
	SpatialHashGIUpdatePSO->SetBufferUAV("CellPositionOut", SpatialHashGICellPosition.get());
	SpatialHashGIUpdatePSO->SetBufferUAV("CellNormalOut", SpatialHashGICellNormal.get());
	SpatialHashGIUpdatePSO->SetBufferUAV("CellScoreOut", SpatialHashGICellScore.get());
	SpatialHashGIUpdatePSO->SetBufferUAV("CellLightMaskOut", SpatialHashGICellLightMask.get());
	SpatialHashGIUpdatePSO->SetBufferUAV("ResolvedKeysOut", SpatialHashGIResolvedKeys[cacheIndex].get());
	SpatialHashGIUpdatePSO->SetBufferUAV("ResolvedSH0Out", SpatialHashGIResolvedSH[cacheIndex][0].get());
	SpatialHashGIUpdatePSO->SetBufferUAV("ActiveCellSlotsOut", SpatialHashGIActiveCellSlots.get());
	SpatialHashGIUpdatePSO->SetBufferUAV("ActiveCounterOut", SpatialHashGIActiveCounter.get());
	SpatialHashGIUpdatePSO->SetCBVValue("SpatialHashGIConstant", &SpatialHashGICB);
	SpatialHashGIUpdatePSO->Apply();
	renderBackend->Dispatch((GetRenderWidth() + 7u) / 8u, (GetRenderHeight() + 7u) / 8u, 1u);

	if (bEnableSpatialHashPrimaryDeepSeed)
	{
		renderBackend->TransitionBuffer(SpatialHashGIActiveFlags.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
		renderBackend->TransitionBuffer(SpatialHashGIActiveCellSlots.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
		renderBackend->TransitionBuffer(SpatialHashGIActiveCounter.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
		renderBackend->TransitionBuffer(SpatialHashGICellPosition.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
		renderBackend->TransitionBuffer(SpatialHashGICellNormal.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
		renderBackend->TransitionBuffer(SpatialHashGICellScore.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
		renderBackend->TransitionBuffer(SpatialHashGICellLightMask.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
		renderBackend->TransitionBuffer(SpatialHashGIResolvedKeys[cacheIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
		renderBackend->TransitionBuffer(SpatialHashGIResolvedSH[cacheIndex][0].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
		renderBackend->TransitionBuffer(SpatialHashGIActiveFlags.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->TransitionBuffer(SpatialHashGIActiveCellSlots.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->TransitionBuffer(SpatialHashGIActiveCounter.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->TransitionBuffer(SpatialHashGICellPosition.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->TransitionBuffer(SpatialHashGICellNormal.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->TransitionBuffer(SpatialHashGICellScore.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->TransitionBuffer(SpatialHashGICellLightMask.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->TransitionBuffer(SpatialHashGIResolvedKeys[cacheIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->TransitionBuffer(SpatialHashGIResolvedSH[cacheIndex][0].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

		BeginGpuPassTiming(EGpuPass::SpatialHashDeepSeed);
		const bool bDeepSeedOk = SpatialHashPrimaryDeepSeedPass();
		EndGpuPassTiming(EGpuPass::SpatialHashDeepSeed);
		if (!bDeepSeedOk)
		{
			static bool sLoggedDeepSeedFailure = false;
			if (!sLoggedDeepSeedFailure)
			{
				sLoggedDeepSeedFailure = true;
				AppendCpuRuntimeTrace(L"[SpatialHashGI][PrimaryDeepSeed] skipped: pass prerequisites missing");
			}
		}
	}

	renderBackend->TransitionBuffer(SpatialHashGIActiveFlags.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionBuffer(SpatialHashGIActiveCellSlots.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionBuffer(SpatialHashGIActiveCounter.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionBuffer(SpatialHashGICellPosition.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionBuffer(SpatialHashGICellNormal.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionBuffer(SpatialHashGICellScore.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionBuffer(SpatialHashGICellLightMask.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionBuffer(SpatialHashGIResolvedKeys[cacheIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionBuffer(SpatialHashGIResolvedSH[cacheIndex][0].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);

	SpatialHashLightMaskFrameIndex = RenderFrameIndex;
	return true;
}

void Corona::SpatialHashGIPass()
{
	auto hasSpatialHashSHBuffers = [&]()
	{
		for (UINT coefficientIndex = 0; coefficientIndex < SpatialHashGISHCoefficientCount; ++coefficientIndex)
		{
			if (!SpatialHashGITraceSH[coefficientIndex])
				return false;
			if (!SpatialHashGIResolvedSH[0][coefficientIndex])
				return false;
		}
		return true;
	};

	shared_ptr<RTPipelineStateObject> rtPSO = PSO_RT_SPATIAL_HASH_GI;
	if (bEnableRTDiffuseGISER && renderBackend && renderBackend->SupportsShaderExecutionReordering() && InitRaytracingSpatialHashGISERPass())
		rtPSO = PSO_RT_SPATIAL_HASH_GI_SER;

	if (!TLAS || !rtPSO ||
		!SpatialHashGIClearPSO || !SpatialHashGIUpdatePSO || !SpatialHashGIResolvePSO || !SpatialHashGIQueryPSO ||
		!SpatialHashGIActiveFlags || !SpatialHashGIActiveCellSlots || !SpatialHashGIActiveCounter ||
		!SpatialHashGICellPosition || !SpatialHashGICellNormal || !SpatialHashGICellScore ||
		!SpatialHashGICellLightMask ||
		!SpatialHashGIResolvedKeys[0] ||
		!hasSpatialHashSHBuffers() ||
		!DiffuseGIHashCached || !DiffuseGIHashCachedAux)
		return;

	renderBackend->EmitGpuCrashMarker("SpatialHashGIPass");

	const UINT32 cacheIndex = 0u;
	const UINT32 spatialHashTraceCellBudget = std::min(SpatialHashGITraceCellBudget, SpatialHashGIActiveCellCapacity);
	if (!SpatialHashLightMaskPass())
		return;
	const bool bUseBindlessMaterials = UsesRTBindlessMaterials();
	if (bUseBindlessMaterials && !EnsureRTMaterialRecordBuffer())
		return;

	const bool bOctMode = (SpatialHashGICB.GIMode == 1u);
	const bool bHasOctBuffers =
		SpatialHashGIOctRayData &&
		SpatialHashGIOctIrradiance[0] &&
		SpatialHashGIOctReservoirRay &&
		SpatialHashGIOctReservoirRadiance &&
		SpatialHashGIOctBlendPSO;
	const bool bUseOct = bOctMode && bHasOctBuffers;

	for (UINT coefficientIndex = 0; coefficientIndex < SpatialHashGISHCoefficientCount; ++coefficientIndex)
		renderBackend->TransitionBuffer(SpatialHashGITraceSH[coefficientIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	// OctRayData is bound as a UAV on the RT PSO in both modes (written only in
	// oct mode), so keep it in UnorderedAccess for the dispatch regardless.
	if (SpatialHashGIOctRayData)
		renderBackend->TransitionBuffer(SpatialHashGIOctRayData.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	RTPassBuilder pass(*this, rtPSO);
	pass.BeginScene()
		.SetBufferUAV("global", "TraceSH0", SpatialHashGITraceSH[0].get())
		.SetBufferUAV("global", "TraceSH1", SpatialHashGITraceSH[1].get())
		.SetBufferUAV("global", "TraceSH2", SpatialHashGITraceSH[2].get())
		.SetBufferUAV("global", "TraceSH3", SpatialHashGITraceSH[3].get())
		.SetBufferUAV("global", "OctRayData", SpatialHashGIOctRayData.get())
		.SetAccelerationStructure("global", "gRtScene", TLAS)
		.SetBufferSRV("global", "CellKeys", SpatialHashGIResolvedKeys[cacheIndex].get())
		.SetBufferSRV("global", "CellPosition", SpatialHashGICellPosition.get())
		.SetBufferSRV("global", "CellNormal", SpatialHashGICellNormal.get())
		.SetTextureSRV("global", "RayNoiseBlueNoiseSource", BlueNoiseTex.get())
		.SetBufferSRV("global", "ActiveCellSlots", SpatialHashGIActiveCellSlots.get())
		.SetBufferSRV("global", "ActiveCounter", SpatialHashGIActiveCounter.get())
		.SetBufferSRV("global", "CellLightMask", SpatialHashGICellLightMask.get())
		.SetBufferSRV("global", "OctIrradianceConverge", SpatialHashGIOctIrradiance[0].get())
		.SetCBVValue("global", "ViewParameter", &RTSpatialHashGIViewParam)
		.SetSampler("global", "sampleWrap", samplerWrap.get());
	if (bUseBindlessMaterials)
	{
		pass.SetBindlessTextureTable("global", "MaterialTextures")
			.SetBufferSRV("global", "RtMaterials", RTMaterialRecordBuffer.get());
	}
	RTSceneHitProgramDesc hitProgramDesc;
	hitProgramDesc.bBindDiffuseTexture = !bUseBindlessMaterials;
	pass.BindSceneHitPrograms(hitProgramDesc);
	// Oct mode dispatches one ray per (probe, ray); SH mode one thread per cell.
	const UINT32 octTraceCellBudget = std::min(
		SpatialHashGIOctTraceCellBudget,
		std::min(SpatialHashGIOctCellCapacity, SpatialHashGIActiveCellCapacity));
	const UINT32 octRayDispatch = octTraceCellBudget * SpatialHashGIOctRaysPerCell;
	pass.Dispatch(bUseOct ? octRayDispatch : spatialHashTraceCellBudget, 1u);

	if (SpatialHashGIOctRayData)
		renderBackend->TransitionBuffer(SpatialHashGIOctRayData.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);

	for (UINT coefficientIndex = 0; coefficientIndex < SpatialHashGISHCoefficientCount; ++coefficientIndex)
		renderBackend->TransitionBuffer(SpatialHashGITraceSH[coefficientIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);

	renderBackend->TransitionBuffer(SpatialHashGIResolvedKeys[cacheIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	// The SH resolve temporally blends TraceSH -> ResolvedSH. In oct mode the
	// query/blend read the octahedral atlas, never ResolvedSH, and the cell key
	// table is maintained by the UPDATE pass (not resolve) — so the entire resolve
	// dispatch is dead work in oct mode. Skip the dispatch (keep the surrounding
	// state transitions so ResolvedSH/Keys end in the same state the query expects).
	if (!bUseOct)
	{
		for (UINT coefficientIndex = 0; coefficientIndex < SpatialHashGISHCoefficientCount; ++coefficientIndex)
			renderBackend->TransitionBuffer(SpatialHashGIResolvedSH[cacheIndex][coefficientIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		SpatialHashGIResolvePSO->SetBufferSRV("ActiveCellSlotsIn", SpatialHashGIActiveCellSlots.get());
		SpatialHashGIResolvePSO->SetBufferSRV("ActiveCounterIn", SpatialHashGIActiveCounter.get());
		SpatialHashGIResolvePSO->SetBufferSRV("TraceSH0In", SpatialHashGITraceSH[0].get());
		SpatialHashGIResolvePSO->SetBufferSRV("TraceSH1In", SpatialHashGITraceSH[1].get());
		SpatialHashGIResolvePSO->SetBufferSRV("TraceSH2In", SpatialHashGITraceSH[2].get());
		SpatialHashGIResolvePSO->SetBufferSRV("TraceSH3In", SpatialHashGITraceSH[3].get());
		SpatialHashGIResolvePSO->SetBufferUAV("ResolvedKeysOut", SpatialHashGIResolvedKeys[cacheIndex].get());
		SpatialHashGIResolvePSO->SetBufferUAV("ResolvedSH0Out", SpatialHashGIResolvedSH[cacheIndex][0].get());
		SpatialHashGIResolvePSO->SetBufferUAV("ResolvedSH1Out", SpatialHashGIResolvedSH[cacheIndex][1].get());
		SpatialHashGIResolvePSO->SetBufferUAV("ResolvedSH2Out", SpatialHashGIResolvedSH[cacheIndex][2].get());
		SpatialHashGIResolvePSO->SetBufferUAV("ResolvedSH3Out", SpatialHashGIResolvedSH[cacheIndex][3].get());
		SpatialHashGIResolvePSO->SetCBVValue("SpatialHashGIConstant", &SpatialHashGICB);
		SpatialHashGIResolvePSO->Apply();
		renderBackend->Dispatch((spatialHashTraceCellBudget + 255u) / 256u, 1u, 1u);
	}

	renderBackend->TransitionBuffer(SpatialHashGIResolvedKeys[cacheIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	if (!bUseOct)
	{
		for (UINT coefficientIndex = 0; coefficientIndex < SpatialHashGISHCoefficientCount; ++coefficientIndex)
			renderBackend->TransitionBuffer(SpatialHashGIResolvedSH[cacheIndex][coefficientIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	}

	// Octahedral DDGI blend: convolve this frame's per-ray radiance (OctRayData)
	// into each probe's octahedral irradiance map with temporal hysteresis. Runs
	// in addition to the (now inert) SH resolve so all SH state transitions stay
	// intact; the query reads the oct atlas instead of the SH when GIMode==1.
	if (bUseOct)
	{
		renderBackend->TransitionBuffer(SpatialHashGIOctIrradiance[0].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		if (SpatialHashGIOctCellKey)
			renderBackend->TransitionBuffer(SpatialHashGIOctCellKey.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->TransitionBuffer(SpatialHashGIOctReservoirRay.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->TransitionBuffer(SpatialHashGIOctReservoirRadiance.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		SpatialHashGIOctBlendPSO->SetBufferSRV("ActiveCellSlotsIn", SpatialHashGIActiveCellSlots.get());
		SpatialHashGIOctBlendPSO->SetBufferSRV("ActiveCounterIn", SpatialHashGIActiveCounter.get());
		SpatialHashGIOctBlendPSO->SetBufferSRV("ResolvedKeysIn", SpatialHashGIResolvedKeys[cacheIndex].get());
		SpatialHashGIOctBlendPSO->SetBufferSRV("CellPositionIn", SpatialHashGICellPosition.get());
		SpatialHashGIOctBlendPSO->SetBufferSRV("CellNormalIn", SpatialHashGICellNormal.get());
		SpatialHashGIOctBlendPSO->SetBufferSRV("OctRayDataIn", SpatialHashGIOctRayData.get());
		SpatialHashGIOctBlendPSO->SetBufferUAV("OctIrradianceOut", SpatialHashGIOctIrradiance[0].get());
		SpatialHashGIOctBlendPSO->SetBufferUAV("OctCellKeyOut", SpatialHashGIOctCellKey.get());
		SpatialHashGIOctBlendPSO->SetBufferUAV("OctReservoirRayOut", SpatialHashGIOctReservoirRay.get());
		SpatialHashGIOctBlendPSO->SetBufferUAV("OctReservoirRadianceOut", SpatialHashGIOctReservoirRadiance.get());
		SpatialHashGIOctBlendPSO->SetCBVValue("SpatialHashGIConstant", &SpatialHashGICB);
		SpatialHashGIOctBlendPSO->Apply();
		const UINT32 octBlendThreads = octTraceCellBudget * SpatialHashGIOctIrradianceTexels;
		renderBackend->Dispatch((octBlendThreads + 63u) / 64u, 1u, 1u);
		renderBackend->TransitionBuffer(SpatialHashGIOctIrradiance[0].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);

		// Octahedral depth/visibility blend: build the per-probe octahedral depth
		// map (mean, mean^2 of hit distances) consumed by Chebyshev at query time.
		if (SpatialHashGIOctDepthBlendPSO && SpatialHashGIOctDepth[0])
		{
			renderBackend->TransitionBuffer(SpatialHashGIOctDepth[0].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
			SpatialHashGIOctDepthBlendPSO->SetBufferSRV("ActiveCellSlotsIn", SpatialHashGIActiveCellSlots.get());
			SpatialHashGIOctDepthBlendPSO->SetBufferSRV("ActiveCounterIn", SpatialHashGIActiveCounter.get());
			SpatialHashGIOctDepthBlendPSO->SetBufferSRV("ResolvedKeysIn", SpatialHashGIResolvedKeys[cacheIndex].get());
			SpatialHashGIOctDepthBlendPSO->SetBufferSRV("CellNormalIn", SpatialHashGICellNormal.get());
			SpatialHashGIOctDepthBlendPSO->SetBufferSRV("OctRayDataIn", SpatialHashGIOctRayData.get());
			SpatialHashGIOctDepthBlendPSO->SetBufferUAV("OctDepthOut", SpatialHashGIOctDepth[0].get());
			SpatialHashGIOctDepthBlendPSO->SetBufferUAV("OctCellKeyOut", SpatialHashGIOctCellKey.get());
			SpatialHashGIOctDepthBlendPSO->SetCBVValue("SpatialHashGIConstant", &SpatialHashGICB);
			SpatialHashGIOctDepthBlendPSO->Apply();
			const UINT32 octDepthThreads = octTraceCellBudget * SpatialHashGIOctDepthTexels;
			renderBackend->Dispatch((octDepthThreads + 63u) / 64u, 1u, 1u);
			renderBackend->TransitionBuffer(SpatialHashGIOctDepth[0].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
		}
		if (SpatialHashGIOctCellKey)
			renderBackend->TransitionBuffer(SpatialHashGIOctCellKey.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
		renderBackend->TransitionBuffer(SpatialHashGIOctReservoirRay.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
		renderBackend->TransitionBuffer(SpatialHashGIOctReservoirRadiance.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	}

	renderBackend->TransitionTexture(DiffuseGIHashCached.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionTexture(DiffuseGIHashCachedAux.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	SpatialHashGIQueryPSO->SetTextureSRV("DepthTex", UnjitteredDepthBuffers[ColorBufferWriteIndex].get());
	SpatialHashGIQueryPSO->SetTextureSRV("WorldNormalTex", NormalBuffers[ColorBufferWriteIndex].get());
	SpatialHashGIQueryPSO->SetTextureSRV("GeoNormalTex", GeomNormalBuffers[ColorBufferWriteIndex].get());
	// Disocclusion inputs (motion-vector based prev-frame compare).
	SpatialHashGIQueryPSO->SetTextureSRV("VelocityTex", VelocityBuffer ? VelocityBuffer.get() : NormalBuffers[ColorBufferWriteIndex].get());
	SpatialHashGIQueryPSO->SetTextureSRV("PrevDepthTex",
		UnjitteredDepthBuffers[1 - ColorBufferWriteIndex] ? UnjitteredDepthBuffers[1 - ColorBufferWriteIndex].get() : UnjitteredDepthBuffers[ColorBufferWriteIndex].get());
	SpatialHashGIQueryPSO->SetTextureSRV("PrevNormalTex",
		NormalBuffers[1 - ColorBufferWriteIndex] ? NormalBuffers[1 - ColorBufferWriteIndex].get() : NormalBuffers[ColorBufferWriteIndex].get());
	SpatialHashGIQueryPSO->SetBufferSRV("CellPositionIn", SpatialHashGICellPosition.get());
	SpatialHashGIQueryPSO->SetBufferSRV("CellNormalIn", SpatialHashGICellNormal.get());
	SpatialHashGIQueryPSO->SetBufferSRV("ResolvedKeysIn", SpatialHashGIResolvedKeys[cacheIndex].get());
	SpatialHashGIQueryPSO->SetBufferSRV("ResolvedSH0In", SpatialHashGIResolvedSH[cacheIndex][0].get());
	SpatialHashGIQueryPSO->SetBufferSRV("ResolvedSH1In", SpatialHashGIResolvedSH[cacheIndex][1].get());
	SpatialHashGIQueryPSO->SetBufferSRV("ResolvedSH2In", SpatialHashGIResolvedSH[cacheIndex][2].get());
	SpatialHashGIQueryPSO->SetBufferSRV("ResolvedSH3In", SpatialHashGIResolvedSH[cacheIndex][3].get());
	SpatialHashGIQueryPSO->SetBufferSRV("OctIrradianceIn", SpatialHashGIOctIrradiance[0].get());
	SpatialHashGIQueryPSO->SetBufferSRV("OctDepthIn", SpatialHashGIOctDepth[0].get());
	SpatialHashGIQueryPSO->SetBufferSRV("OctCellKeyIn", SpatialHashGIOctCellKey.get());
	SpatialHashGIQueryPSO->SetTextureUAV("OutGIHashColor", DiffuseGIHashCached.get());
	SpatialHashGIQueryPSO->SetTextureUAV("OutGIHashSH", DiffuseGIHashCachedAux.get());
	SpatialHashGIQueryPSO->SetCBVValue("SpatialHashGIConstant", &SpatialHashGICB);
	SpatialHashGIQueryPSO->Apply();
	renderBackend->Dispatch((GetRenderWidth() + 7u) / 8u, (GetRenderHeight() + 7u) / 8u, 1u);

	renderBackend->TransitionTexture(DiffuseGIHashCached.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(DiffuseGIHashCachedAux.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);

	// =================================================================
	// Option A — screen-space resolve pass (bilateral + temporal disocc)
	// =================================================================
	if (SpatialHashGIScreenResolvePSO && DiffuseGIHashFiltered && DiffuseGIHashFilteredPrev)
	{
		renderBackend->TransitionTexture(DiffuseGIHashFiltered.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		// The new pass reads DiffuseGIHashCached as a UAV inside the
		// shader (named OutGIHashColor — same UAV slot, different
		// access). Re-transition to ShaderResource isn't possible for
		// the same dispatch, so use the UAV-as-SRV pattern: dispatch
		// reads via OutGIHashColor (UAV) which is legal even when the
		// resource isn't transitioned. We'll keep it in
		// UnorderedAccess until after this pass.
		renderBackend->TransitionTexture(DiffuseGIHashCached.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

		SpatialHashGIScreenResolvePSO->SetTextureSRV("DepthTex",       UnjitteredDepthBuffers[ColorBufferWriteIndex].get());
		SpatialHashGIScreenResolvePSO->SetTextureSRV("WorldNormalTex", NormalBuffers[ColorBufferWriteIndex].get());
		SpatialHashGIScreenResolvePSO->SetTextureSRV("GeoNormalTex",   GeomNormalBuffers[ColorBufferWriteIndex].get());
		SpatialHashGIScreenResolvePSO->SetTextureSRV("VelocityTex",    VelocityBuffer ? VelocityBuffer.get() : NormalBuffers[ColorBufferWriteIndex].get());
		SpatialHashGIScreenResolvePSO->SetTextureSRV("PrevDepthTex",
			UnjitteredDepthBuffers[1 - ColorBufferWriteIndex] ? UnjitteredDepthBuffers[1 - ColorBufferWriteIndex].get() : UnjitteredDepthBuffers[ColorBufferWriteIndex].get());
		SpatialHashGIScreenResolvePSO->SetTextureSRV("PrevNormalTex",
			NormalBuffers[1 - ColorBufferWriteIndex] ? NormalBuffers[1 - ColorBufferWriteIndex].get() : NormalBuffers[ColorBufferWriteIndex].get());
		SpatialHashGIScreenResolvePSO->SetTextureSRV("InDiffuseGIFilteredPrev", DiffuseGIHashFilteredPrev.get());
		SpatialHashGIScreenResolvePSO->SetTextureUAV("OutGIHashColor",         DiffuseGIHashCached.get());
		SpatialHashGIScreenResolvePSO->SetTextureUAV("OutDiffuseGIFiltered",   DiffuseGIHashFiltered.get());
		SpatialHashGIScreenResolvePSO->SetCBVValue("SpatialHashGIConstant", &SpatialHashGICB);
		SpatialHashGIScreenResolvePSO->Apply();
		renderBackend->Dispatch((GetRenderWidth() + 7u) / 8u, (GetRenderHeight() + 7u) / 8u, 1u);

		renderBackend->TransitionTexture(DiffuseGIHashCached.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
		renderBackend->TransitionTexture(DiffuseGIHashFiltered.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);

		// End-of-frame snapshot for next frame's temporal blend.
		if (dx12_rhi)
		{
			renderBackend->TransitionTexture(DiffuseGIHashFiltered.get(), EResourceState::ShaderRead, EResourceState::CopySource);
			renderBackend->TransitionTexture(DiffuseGIHashFilteredPrev.get(), EResourceState::ShaderRead, EResourceState::CopyDest);
			dx12_rhi->GetGraphicsCommandList()->CopyResource(
				DiffuseGIHashFilteredPrev->resource.Get(),
				DiffuseGIHashFiltered->resource.Get());
			renderBackend->TransitionTexture(DiffuseGIHashFiltered.get(), EResourceState::CopySource, EResourceState::ShaderRead);
			renderBackend->TransitionTexture(DiffuseGIHashFilteredPrev.get(), EResourceState::CopyDest, EResourceState::ShaderRead);
		}
	}

	bSpatialHashGIHistoryValid = true;
}
