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
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iterator>
#include <sstream>

void AppendCpuRuntimeTrace(const std::wstring& line);

namespace
{
	std::wstring FormatScreenProbeInitMilliseconds(double milliseconds)
	{
		std::wostringstream stream;
		stream << std::fixed << std::setprecision(3) << milliseconds;
		return stream.str();
	}

	double ElapsedScreenProbeInitMilliseconds(
		const std::chrono::steady_clock::time_point& begin,
		const std::chrono::steady_clock::time_point& end)
	{
		return std::chrono::duration<double, std::milli>(end - begin).count();
	}
}

void Corona::InitScreenProbeGIPass()
{
	shared_ptr<ComputePipelineStateObject> tempPSO = renderBackend->CreateComputePipelineStateObject();
	if (!tempPSO)
		return;

	const RHIShaderStageMask computeStage = ToRHIShaderStageMask(RHIShaderStage::Compute);
	tempPSO->BindSRV(MakeRHITextureSRV("DepthTex", 0, computeStage));
	tempPSO->BindSRV(MakeRHITextureSRV("WorldNormalTex", 1, computeStage));
	tempPSO->BindSRV(MakeRHITextureSRV("GeoNormalTex", 2, computeStage));
	tempPSO->BindSRV(MakeRHITextureSRV("ScreenProbeRadianceTex", 3, computeStage));
	tempPSO->BindSRV(MakeRHITextureSRV("ScreenProbeMetaTex", 4, computeStage));
	tempPSO->BindSRV(MakeRHITextureSRV("PrevScreenProbeGITex", 5, computeStage));
	tempPSO->BindSRV(MakeRHITextureSRV("VelocityTex", 6, computeStage));
	tempPSO->BindSRV(MakeRHITextureSRV("PrevDepthTex", 7, computeStage));
	tempPSO->BindSRV(MakeRHITextureSRV("PrevNormalTex", 8, computeStage));
	tempPSO->BindSRV(MakeRHITextureSRV("ScreenProbeSH0Tex", 9, computeStage));
	tempPSO->BindSRV(MakeRHITextureSRV("ScreenProbeSH1Tex", 10, computeStage));
	tempPSO->BindSRV(MakeRHITextureSRV("ScreenProbeSH2Tex", 11, computeStage));
	tempPSO->BindSRV(MakeRHITextureSRV("ScreenProbeSH3Tex", 12, computeStage));
	tempPSO->BindSRV(MakeRHITextureSRV("ScreenProbeSH4Tex", 13, computeStage));
	tempPSO->BindSRV(MakeRHITextureSRV("ScreenProbeSH5Tex", 14, computeStage));
	tempPSO->BindSRV(MakeRHITextureSRV("ScreenProbeSH6Tex", 15, computeStage));
	tempPSO->BindSRV(MakeRHITextureSRV("ScreenProbeSH7Tex", 16, computeStage));
	tempPSO->BindSRV(MakeRHITextureSRV("ScreenProbeSH8Tex", 17, computeStage));
	tempPSO->BindUAV(MakeRHITextureUAV("OutScreenProbeGI", 0, computeStage));
	tempPSO->BindUAV(MakeRHITextureUAV("OutScreenProbeDebug", 1, computeStage));
	tempPSO->BindUAV(MakeRHITextureUAV("OutScreenProbeHistory", 2, computeStage));
	tempPSO->BindSampler(MakeRHISampler("BilinearClamp", 0, computeStage));
	tempPSO->BindCBV(MakeRHICBV("ScreenProbeGIConstant", 0, sizeof(ScreenProbeGIConstant), computeStage));

	if (tempPSO->InitCS(GetAssetFullPath(L"Shaders\\ScreenProbeGI.hlsl"), "ScreenProbeGI"))
		ScreenProbeGIPSO = tempPSO;
}


shared_ptr<RTPipelineStateObject> Corona::CreateRaytracingScreenProbeGIPSO(bool bUseSER)
{
		const auto totalStart = std::chrono::steady_clock::now();
		auto stepStart = totalStart;
		auto traceStep = [&](const wchar_t* label)
		{
			const auto now = std::chrono::steady_clock::now();
			AppendCpuRuntimeTrace(
				L"[StartupTiming][ScreenProbeRT] step=\"" + std::wstring(label) +
				L"\", stepMs=" + FormatScreenProbeInitMilliseconds(ElapsedScreenProbeInitMilliseconds(stepStart, now)) +
				L", totalMs=" + FormatScreenProbeInitMilliseconds(ElapsedScreenProbeInitMilliseconds(totalStart, now)));
			stepStart = now;
		};

		AppendCpuRuntimeTrace(
			L"[StartupTiming][ScreenProbeRT] begin instances=" +
			std::to_wstring(static_cast<uint32_t>(RayTracingInstances.size())));

		shared_ptr<RTPipelineStateObject> tempPSO = renderBackend->CreateRTPipelineStateObject();
		if (!tempPSO)
		{
			AppendCpuRuntimeTrace(L"[StartupTiming][ScreenProbeRT] create RTPipelineStateObject failed");
			return nullptr;
		}
		if (bUseSER)
		{
			tempPSO->SetShaderDefine("RT_DIFFUSE_GI_USE_SER", "1");
			tempPSO->SetShaderDefine("RT_DIFFUSE_GI_SER_MATERIAL_HINT_BITS", "8");
			tempPSO->SetShaderLibraryTarget("lib_6_9");
		}
		traceStep(L"CreateRTPipelineStateObject");

		tempPSO->SetNumInstances(static_cast<uint32_t>(RayTracingInstances.size()));

		tempPSO->AddHitGroup("HitGroup", "chs", "");
		tempPSO->AddShader("rayGen", RTPipelineStateObject::RAYGEN);

		const RHIShaderStageMask rayGenStage = ToRHIShaderStageMask(RHIShaderStage::RayGeneration);
		const RHIShaderStageMask closestHitStage = ToRHIShaderStageMask(RHIShaderStage::ClosestHit);
		tempPSO->BindUAV("global", MakeRHITextureUAV("ProbeRadiance", 0, rayGenStage));
		tempPSO->BindUAV("global", MakeRHITextureUAV("ProbeMeta", 1, rayGenStage));
		tempPSO->BindUAV("global", MakeRHITextureUAV("ProbeSH0", 2, rayGenStage));
		tempPSO->BindUAV("global", MakeRHITextureUAV("ProbeSH1", 3, rayGenStage));
		tempPSO->BindUAV("global", MakeRHITextureUAV("ProbeSH2", 4, rayGenStage));
		tempPSO->BindUAV("global", MakeRHITextureUAV("ProbeSH3", 5, rayGenStage));
		tempPSO->BindUAV("global", MakeRHITextureUAV("ProbeSH4", 6, rayGenStage));
		tempPSO->BindUAV("global", MakeRHITextureUAV("ProbeSH5", 7, rayGenStage));
		tempPSO->BindUAV("global", MakeRHITextureUAV("ProbeSH6", 8, rayGenStage));
		tempPSO->BindUAV("global", MakeRHITextureUAV("ProbeSH7", 9, rayGenStage));
		tempPSO->BindUAV("global", MakeRHITextureUAV("ProbeSH8", 10, rayGenStage));
		tempPSO->BindSRV("global", MakeRHIAccelerationStructureSRV("gRtScene", 0, rayGenStage));
		tempPSO->BindSRV("global", MakeRHITextureSRV("DepthTex", 1, rayGenStage));
		tempPSO->BindSRV("global", MakeRHITextureSRV("WorldNormalTex", 2, rayGenStage));
		tempPSO->BindSRV("global", MakeRHITextureSRV("RayNoiseBlueNoiseSource", 7, rayGenStage));
		tempPSO->BindSRV("global", MakeRHITextureSRV("PrevProbeRadianceTex", 8, rayGenStage));
		tempPSO->BindSRV("global", MakeRHITextureSRV("PrevProbeMetaTex", 9, rayGenStage));
		tempPSO->BindSRV("global", MakeRHITextureSRV("VelocityTex", 10, rayGenStage));
		tempPSO->BindSRV("global", MakeRHITextureSRV("PrevDepthTex", 11, rayGenStage));
		tempPSO->BindSRV("global", MakeRHITextureSRV("PrevNormalTex", 12, rayGenStage));
		tempPSO->BindSRV("global", MakeRHITextureSRV("PrevProbeSH0Tex", 13, rayGenStage));
		tempPSO->BindSRV("global", MakeRHITextureSRV("PrevProbeSH1Tex", 14, rayGenStage));
		tempPSO->BindSRV("global", MakeRHITextureSRV("PrevProbeSH2Tex", 15, rayGenStage));
		tempPSO->BindSRV("global", MakeRHITextureSRV("PrevProbeSH3Tex", 16, rayGenStage));
		tempPSO->BindSRV("global", MakeRHITextureSRV("PrevProbeSH4Tex", 17, rayGenStage));
		tempPSO->BindSRV("global", MakeRHITextureSRV("PrevProbeSH5Tex", 18, rayGenStage));
		tempPSO->BindSRV("global", MakeRHITextureSRV("PrevProbeSH6Tex", 19, rayGenStage));
		tempPSO->BindSRV("global", MakeRHITextureSRV("PrevProbeSH7Tex", 20, rayGenStage));
		tempPSO->BindSRV("global", MakeRHITextureSRV("PrevProbeSH8Tex", 21, rayGenStage));
		tempPSO->BindSRV("global", MakeRHITextureSRV("GeoNormalTex", 22, rayGenStage));
		tempPSO->BindCBV("global", MakeRHICBV("ViewParameter", 0, sizeof(RTScreenProbeGIViewParamCB), rayGenStage));
		tempPSO->BindSampler("global", MakeRHISampler("sampleWrap", 0, rayGenStage | closestHitStage));
		tempPSO->BindSampler("global", MakeRHISampler("historyClamp", 1, rayGenStage));
		BindRTBindlessMaterialSchema(*tempPSO, closestHitStage);
		traceStep(L"Bind global resources");

		tempPSO->AddShader("miss", RTPipelineStateObject::MISS);
		tempPSO->AddShader("missShadow", RTPipelineStateObject::MISS);

		tempPSO->AddShader("chs", RTPipelineStateObject::HIT);
		tempPSO->BindSRV("chs", MakeRHIBufferSRV("vertices", 3, closestHitStage, RHIBufferViewKind::Raw));
		tempPSO->BindSRV("chs", MakeRHIBufferSRV("indices", 4, closestHitStage, RHIBufferViewKind::Raw));
		tempPSO->BindSRV("chs", MakeRHITextureSRV("AlbedoTex", 5, closestHitStage));
		tempPSO->BindSRV("chs", MakeRHIBufferSRV("InstanceProperty", 6, closestHitStage, RHIBufferViewKind::Raw));
		tempPSO->Configure(1, sizeof(float) * 12, sizeof(float) * 2);
		traceStep(L"Bind hit program resources");

		const bool bSuccess = tempPSO->InitRS("Shaders\\ScreenProbeRaytracedGI.hlsl");
		traceStep(L"InitRS ScreenProbeRaytracedGI.hlsl");
		AppendCpuRuntimeTrace(
			L"[StartupTiming][ScreenProbeRT] complete totalMs=" +
			FormatScreenProbeInitMilliseconds(ElapsedScreenProbeInitMilliseconds(totalStart, std::chrono::steady_clock::now())) +
			L", success=" + std::to_wstring(bSuccess ? 1 : 0) +
			L", ser=" + std::to_wstring(bUseSER ? 1 : 0));
		return bSuccess ? tempPSO : nullptr;
}

void Corona::InitRaytracingScreenProbePass()
{
	PSO_RT_SCREEN_PROBE_GI = CreateRaytracingScreenProbeGIPSO(false);
	if (bEnableRTDiffuseGISER && renderBackend && renderBackend->SupportsShaderExecutionReordering())
		InitRaytracingScreenProbeGISERPass();
}

bool Corona::InitRaytracingScreenProbeGISERPass()
{
	if (PSO_RT_SCREEN_PROBE_GI_SER)
		return true;
	if (bRTDiffuseGIScreenProbeSERInitFailed)
		return false;
	if (!renderBackend)
		return false;
	if (!renderBackend->SupportsShaderExecutionReordering())
	{
		bRTDiffuseGIScreenProbeSERInitFailed = true;
		AppendCpuRuntimeTrace(L"[RTDiffuseGI][SER] Screen Probe SER skipped: backend does not support shader execution reordering");
		return false;
	}

	PSO_RT_SCREEN_PROBE_GI_SER = CreateRaytracingScreenProbeGIPSO(true);
	if (!PSO_RT_SCREEN_PROBE_GI_SER)
	{
		bRTDiffuseGIScreenProbeSERInitFailed = true;
		AppendCpuRuntimeTrace(L"[RTDiffuseGI][SER] Screen Probe SER PSO creation failed");
	}
	return PSO_RT_SCREEN_PROBE_GI_SER != nullptr;
}

void Corona::ScreenProbeRaytraceGIPass()
{
	auto hasScreenProbeSHSet = [&](UINT historyIndex)
	{
		for (UINT coefficientIndex = 0; coefficientIndex < ScreenProbeSHCoefficientCount; ++coefficientIndex)
		{
			if (!ScreenProbeGISH[historyIndex][coefficientIndex])
				return false;
		}
		return true;
	};

	shared_ptr<RTPipelineStateObject> pso = PSO_RT_SCREEN_PROBE_GI;
	if (bEnableRTDiffuseGISER && renderBackend && renderBackend->SupportsShaderExecutionReordering() && InitRaytracingScreenProbeGISERPass())
		pso = PSO_RT_SCREEN_PROBE_GI_SER;

	if (!TLAS || !pso || !ScreenProbeGIRadiance[0] || !ScreenProbeGIRadiance[1] || !hasScreenProbeSHSet(0) || !hasScreenProbeSHSet(1) || !ScreenProbeGIMetadata[0] || !ScreenProbeGIMetadata[1])
		return;
	renderBackend->EmitGpuCrashMarker("ScreenProbeRaytraceGIPass");

	const bool bUseBindlessMaterials = UsesRTBindlessMaterials();
	if (bUseBindlessMaterials && !EnsureRTMaterialRecordBuffer())
		return;

	ScreenProbeGIAtlasWriteIndex = 1 - ScreenProbeGIAtlasWriteIndex;
	const UINT writeIndex = ScreenProbeGIAtlasWriteIndex;
	const UINT readIndex = 1 - writeIndex;
	const char* probeSHUAVNames[ScreenProbeSHCoefficientCount] =
	{
		"ProbeSH0",
		"ProbeSH1",
		"ProbeSH2",
		"ProbeSH3",
		"ProbeSH4",
		"ProbeSH5",
		"ProbeSH6",
		"ProbeSH7",
		"ProbeSH8"
	};
	const char* prevProbeSHSRVNames[ScreenProbeSHCoefficientCount] =
	{
		"PrevProbeSH0Tex",
		"PrevProbeSH1Tex",
		"PrevProbeSH2Tex",
		"PrevProbeSH3Tex",
		"PrevProbeSH4Tex",
		"PrevProbeSH5Tex",
		"PrevProbeSH6Tex",
		"PrevProbeSH7Tex",
		"PrevProbeSH8Tex"
	};

	const UINT32 probeSpacing = std::clamp(ScreenProbeGICB.ProbeSpacing, 4u, 64u);
	const UINT32 probeGridWidth = std::max(1u, (GetRenderWidth() + probeSpacing - 1u) / probeSpacing);
	const UINT32 probeGridHeight = std::max(1u, (GetRenderHeight() + probeSpacing - 1u) / probeSpacing);
	const UINT32 screenProbeLightingBootstrapRays = 100u;

	RTScreenProbeGIViewParam.ViewMatrix = glm::transpose(ViewMat);
	RTScreenProbeGIViewParam.InvViewMatrix = glm::transpose(InvViewMat);
	RTScreenProbeGIViewParam.ProjMatrix = glm::transpose(UnjitteredProjMat);
	RTScreenProbeGIViewParam.InvProjMatrix = glm::transpose(UnjitteredInvProjMat);
	RTScreenProbeGIViewParam.ProjectionParams = FrameProjectionParams;
	RTScreenProbeGIViewParam.LightDir = glm::vec4(RenderFrameNormalizedLightDir, LightIntensity);
	RTScreenProbeGIViewParam.RandomOffset = glm::vec2(RenderFrameShaderTime, RenderFrameShaderTime);
	RTScreenProbeGIViewParam.ProbeSpacing = probeSpacing;
	RTScreenProbeGIViewParam.ProbeGridSize = glm::vec2(probeGridWidth, probeGridHeight);
	RTScreenProbeGIViewParam.RTSize = glm::vec2(GetRenderWidth(), GetRenderHeight());
	RTScreenProbeGIViewParam.FrameCounter = RenderFrameIndex;
	RTScreenProbeGIViewParam.BlueNoiseOffsetStride = RTGIViewParam.BlueNoiseOffsetStride;
	RTScreenProbeGIViewParam.NoiseMode = RenderFrameRayNoiseMode;
	RTScreenProbeGIViewParam.RaysPerProbe = std::clamp(RTScreenProbeGIViewParam.RaysPerProbe, 1u, 4u);
	RTScreenProbeGIViewParam.HistoryValid = (bScreenProbeGIAtlasHistoryValid && !bScreenProbeLightingBootstrapPending) ? 1u : 0u;
	RTScreenProbeGIViewParam.ViewSpreadAngle = glm::tan(Fov * 0.5f) / (0.5f * GetRenderHeight());
	RTScreenProbeGIViewParam.LightingBootstrap = bScreenProbeLightingBootstrapPending ? 1u : 0u;
	RTScreenProbeGIViewParam.BootstrapRays = screenProbeLightingBootstrapRays;
	RTScreenProbeGIViewParam.SHCoefficientCount = ScreenProbeGICB.SHCoefficientCount <= 4u ? 4u : 9u;
	RTScreenProbeGIViewParam.TemporalAlpha = std::clamp(ScreenProbeGICB.TemporalAlpha, 0.02f, 1.0f);
	RTScreenProbeGIViewParam.HistoryDepthWeight = ScreenProbeGICB.HistoryDepthWeight;
	RTScreenProbeGIViewParam.HistoryNormalWeight = ScreenProbeGICB.HistoryNormalWeight;
	RTScreenProbeGIViewParam.bIncludeSkyLighting = RenderFrameDiffuseGISkyLightingEnabled;
	RTScreenProbeGIViewParam.SkyColorTop = SkyColorTop;
	RTScreenProbeGIViewParam.SkyIntensity = RenderFrameDiffuseGISkyIntensity;
	RTScreenProbeGIViewParam.SkyColorBottom = SkyColorBottom;
	RTScreenProbeGIViewParam.LightColor = RenderFrameLightColor;
	FillPointLightParams(
		RTScreenProbeGIViewParam.PointLights,
		RTScreenProbeGIViewParam.PointLightCount,
		std::min(MaxDiffuseGIPointLights, DiffuseGIPointLightLimit));
	{
		static float sLastLoggedLightIntensity = -1.0f;
		static UINT32 sLastLoggedPointLightCount = 0xFFFFFFFFu;
		if (std::abs(sLastLoggedLightIntensity - LightIntensity) > 0.0001f ||
			sLastLoggedPointLightCount != RTScreenProbeGIViewParam.PointLightCount)
		{
			sLastLoggedLightIntensity = LightIntensity;
			sLastLoggedPointLightCount = RTScreenProbeGIViewParam.PointLightCount;
			AppendCpuRuntimeTrace(
				L"[DiffuseGI][ScreenProbe] lightIntensity=" + std::to_wstring(LightIntensity) +
				L", pointLights=" + std::to_wstring(RTScreenProbeGIViewParam.PointLightCount) +
				L", pointLightLimit=" + std::to_wstring(DiffuseGIPointLightLimit) +
				L", sky=" + std::to_wstring(RenderFrameDiffuseGISkyLightingEnabled));
		}
	}

	renderBackend->TransitionTexture(ScreenProbeGIRadiance[writeIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	for (UINT coefficientIndex = 0; coefficientIndex < ScreenProbeSHCoefficientCount; ++coefficientIndex)
		renderBackend->TransitionTexture(ScreenProbeGISH[writeIndex][coefficientIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionTexture(ScreenProbeGIMetadata[writeIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	RTPassBuilder pass(*this, pso);
	pass.BeginScene();

	pass.SetTextureUAV("global", "ProbeRadiance", ScreenProbeGIRadiance[writeIndex].get());
	pass.SetTextureUAV("global", "ProbeMeta", ScreenProbeGIMetadata[writeIndex].get());
	for (UINT coefficientIndex = 0; coefficientIndex < ScreenProbeSHCoefficientCount; ++coefficientIndex)
		pass.SetTextureUAV("global", probeSHUAVNames[coefficientIndex], ScreenProbeGISH[writeIndex][coefficientIndex].get());
	pass.SetAccelerationStructure("global", "gRtScene", TLAS);
	pass.SetTextureSRV("global", "DepthTex", UnjitteredDepthBuffers[ColorBufferWriteIndex].get());
	pass.SetTextureSRV("global", "WorldNormalTex", NormalBuffers[ColorBufferWriteIndex].get());
	pass.SetTextureSRV("global", "RayNoiseBlueNoiseSource", BlueNoiseTex.get());
	pass.SetTextureSRV("global", "PrevProbeRadianceTex", ScreenProbeGIRadiance[readIndex].get());
	pass.SetTextureSRV("global", "PrevProbeMetaTex", ScreenProbeGIMetadata[readIndex].get());
	for (UINT coefficientIndex = 0; coefficientIndex < ScreenProbeSHCoefficientCount; ++coefficientIndex)
		pass.SetTextureSRV("global", prevProbeSHSRVNames[coefficientIndex], ScreenProbeGISH[readIndex][coefficientIndex].get());
	pass.SetTextureSRV("global", "VelocityTex", VelocityBuffer.get());
	pass.SetTextureSRV("global", "PrevDepthTex", UnjitteredDepthBuffers[1 - ColorBufferWriteIndex].get());
	pass.SetTextureSRV("global", "PrevNormalTex", NormalBuffers[1 - ColorBufferWriteIndex].get());
	pass.SetTextureSRV("global", "GeoNormalTex", GeomNormalBuffers[ColorBufferWriteIndex].get());
	pass.SetCBVValue("global", "ViewParameter", &RTScreenProbeGIViewParam);
	pass.SetSampler("global", "sampleWrap", samplerWrap.get());
	pass.SetSampler("global", "historyClamp", samplerBilinearWrap.get());
	if (bUseBindlessMaterials)
	{
		pass.SetBindlessTextureTable("global", "MaterialTextures")
			.SetBufferSRV("global", "RtMaterials", RTMaterialRecordBuffer.get());
	}

	pass.BindSceneHitPrograms();
	pass.Dispatch(probeGridWidth, probeGridHeight);

	renderBackend->TransitionTexture(ScreenProbeGIRadiance[writeIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	for (UINT coefficientIndex = 0; coefficientIndex < ScreenProbeSHCoefficientCount; ++coefficientIndex)
		renderBackend->TransitionTexture(ScreenProbeGISH[writeIndex][coefficientIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(ScreenProbeGIMetadata[writeIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	bScreenProbeGIAtlasHistoryValid = true;
}

void Corona::ScreenProbeGIPass()
{
	auto hasScreenProbeSHSet = [&](UINT historyIndex)
	{
		for (UINT coefficientIndex = 0; coefficientIndex < ScreenProbeSHCoefficientCount; ++coefficientIndex)
		{
			if (!ScreenProbeGISH[historyIndex][coefficientIndex])
				return false;
		}
		return true;
	};

	if (!ScreenProbeGIPSO || !ScreenProbeGIResolved || !ScreenProbeGIProbeDebug || !ScreenProbeGIHistory[0] || !ScreenProbeGIHistory[1] ||
		!ScreenProbeGIRadiance[ScreenProbeGIAtlasWriteIndex] || !hasScreenProbeSHSet(ScreenProbeGIAtlasWriteIndex) || !ScreenProbeGIMetadata[ScreenProbeGIAtlasWriteIndex])
		return;
	renderBackend->EmitGpuCrashMarker("ScreenProbeGIPass");

	ScreenProbeGIHistoryWriteIndex = 1 - ScreenProbeGIHistoryWriteIndex;
	const UINT historyWriteIndex = ScreenProbeGIHistoryWriteIndex;
	const UINT historyReadIndex = 1 - historyWriteIndex;
	const char* screenProbeSHSRVNames[ScreenProbeSHCoefficientCount] =
	{
		"ScreenProbeSH0Tex",
		"ScreenProbeSH1Tex",
		"ScreenProbeSH2Tex",
		"ScreenProbeSH3Tex",
		"ScreenProbeSH4Tex",
		"ScreenProbeSH5Tex",
		"ScreenProbeSH6Tex",
		"ScreenProbeSH7Tex",
		"ScreenProbeSH8Tex"
	};

	renderBackend->TransitionTexture(ScreenProbeGIResolved.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionTexture(ScreenProbeGIProbeDebug.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionTexture(ScreenProbeGIHistory[historyWriteIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	ScreenProbeGIPSO->SetTextureSRV("DepthTex", UnjitteredDepthBuffers[ColorBufferWriteIndex].get());
	ScreenProbeGIPSO->SetTextureSRV("WorldNormalTex", NormalBuffers[ColorBufferWriteIndex].get());
	ScreenProbeGIPSO->SetTextureSRV("GeoNormalTex", GeomNormalBuffers[ColorBufferWriteIndex].get());
	ScreenProbeGIPSO->SetTextureSRV("ScreenProbeRadianceTex", ScreenProbeGIRadiance[ScreenProbeGIAtlasWriteIndex].get());
	ScreenProbeGIPSO->SetTextureSRV("ScreenProbeMetaTex", ScreenProbeGIMetadata[ScreenProbeGIAtlasWriteIndex].get());
	ScreenProbeGIPSO->SetTextureSRV("PrevScreenProbeGITex", ScreenProbeGIHistory[historyReadIndex].get());
	ScreenProbeGIPSO->SetTextureSRV("VelocityTex", VelocityBuffer.get());
	ScreenProbeGIPSO->SetTextureSRV("PrevDepthTex", UnjitteredDepthBuffers[1 - ColorBufferWriteIndex].get());
	ScreenProbeGIPSO->SetTextureSRV("PrevNormalTex", NormalBuffers[1 - ColorBufferWriteIndex].get());
	for (UINT coefficientIndex = 0; coefficientIndex < ScreenProbeSHCoefficientCount; ++coefficientIndex)
		ScreenProbeGIPSO->SetTextureSRV(screenProbeSHSRVNames[coefficientIndex], ScreenProbeGISH[ScreenProbeGIAtlasWriteIndex][coefficientIndex].get());
	ScreenProbeGIPSO->SetTextureUAV("OutScreenProbeGI", ScreenProbeGIResolved.get());
	ScreenProbeGIPSO->SetTextureUAV("OutScreenProbeDebug", ScreenProbeGIProbeDebug.get());
	ScreenProbeGIPSO->SetTextureUAV("OutScreenProbeHistory", ScreenProbeGIHistory[historyWriteIndex].get());
	ScreenProbeGIPSO->SetSampler("BilinearClamp", samplerBilinearWrap.get());
	ScreenProbeGICB.ProbeSpacing = std::clamp(ScreenProbeGICB.ProbeSpacing, 4u, 64u);
	ScreenProbeGICB.GatherRadius = std::clamp(ScreenProbeGICB.GatherRadius, 1u, 3u);
	ScreenProbeGICB.RawBlend = std::clamp(ScreenProbeGICB.RawBlend, 0.0f, 1.0f);
	ScreenProbeGICB.EdgeDepthWeight = std::clamp(ScreenProbeGICB.EdgeDepthWeight, 8.0f, 192.0f);
	ScreenProbeGICB.EdgeNormalWeight = std::clamp(ScreenProbeGICB.EdgeNormalWeight, 1.0f, 96.0f);
	ScreenProbeGICB.EdgeSampleCount = std::clamp(ScreenProbeGICB.EdgeSampleCount, 1u, 4u);
	ScreenProbeGICB.SHCoefficientCount = ScreenProbeGICB.SHCoefficientCount <= 4u ? 4u : 9u;
	ScreenProbeGICB.ProjectionParams = FrameProjectionParams;
	ScreenProbeGICB.RTSize = glm::vec2(GetRenderWidth(), GetRenderHeight());
	ScreenProbeGICB.ProbeGridSize = glm::vec2(
		static_cast<float>((GetRenderWidth() + ScreenProbeGICB.ProbeSpacing - 1u) / ScreenProbeGICB.ProbeSpacing),
		static_cast<float>((GetRenderHeight() + ScreenProbeGICB.ProbeSpacing - 1u) / ScreenProbeGICB.ProbeSpacing));
	ScreenProbeGICB.FrameIndex = RenderFrameIndex;
	ScreenProbeGICB.TemporalAlpha = std::clamp(ScreenProbeGICB.TemporalAlpha, 0.02f, 1.0f);
	ScreenProbeGICB.HistoryValid = (bScreenProbeGIHistoryValid && !bScreenProbeLightingBootstrapPending) ? 1u : 0u;
	ScreenProbeGIPSO->SetCBVValue("ScreenProbeGIConstant", &ScreenProbeGICB);
	ScreenProbeGIPSO->Apply();

	renderBackend->Dispatch((GetRenderWidth() + 7) / 8, (GetRenderHeight() + 7) / 8, 1);

	renderBackend->TransitionTexture(ScreenProbeGIResolved.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(ScreenProbeGIProbeDebug.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(ScreenProbeGIHistory[historyWriteIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	bScreenProbeGIHistoryValid = true;
	bScreenProbeLightingBootstrapPending = false;
}
