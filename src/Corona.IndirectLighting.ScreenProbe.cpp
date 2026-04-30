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

void Corona::InitScreenProbeGIPass()
{
	shared_ptr<ComputePipelineStateObject> tempPSO = renderBackend->CreateComputePipelineStateObject();
	if (!tempPSO)
		return;

	tempPSO->BindSRV("DepthTex", 0, 1);
	tempPSO->BindSRV("WorldNormalTex", 1, 1);
	tempPSO->BindSRV("GeoNormalTex", 2, 1);
	tempPSO->BindSRV("ScreenProbeRadianceTex", 3, 1);
	tempPSO->BindSRV("ScreenProbeMetaTex", 4, 1);
	tempPSO->BindSRV("PrevScreenProbeGITex", 5, 1);
	tempPSO->BindSRV("VelocityTex", 6, 1);
	tempPSO->BindSRV("PrevDepthTex", 7, 1);
	tempPSO->BindSRV("PrevNormalTex", 8, 1);
	tempPSO->BindSRV("ScreenProbeSH0Tex", 9, 1);
	tempPSO->BindSRV("ScreenProbeSH1Tex", 10, 1);
	tempPSO->BindSRV("ScreenProbeSH2Tex", 11, 1);
	tempPSO->BindSRV("ScreenProbeSH3Tex", 12, 1);
	tempPSO->BindSRV("ScreenProbeSH4Tex", 13, 1);
	tempPSO->BindSRV("ScreenProbeSH5Tex", 14, 1);
	tempPSO->BindSRV("ScreenProbeSH6Tex", 15, 1);
	tempPSO->BindSRV("ScreenProbeSH7Tex", 16, 1);
	tempPSO->BindSRV("ScreenProbeSH8Tex", 17, 1);
	tempPSO->BindUAV("OutScreenProbeGI", 0);
	tempPSO->BindUAV("OutScreenProbeDebug", 1);
	tempPSO->BindUAV("OutScreenProbeHistory", 2);
	tempPSO->BindSampler("BilinearClamp", 0);
	tempPSO->BindCBV("ScreenProbeGIConstant", 0, sizeof(ScreenProbeGIConstant));

	if (tempPSO->InitCS(GetAssetFullPath(L"Shaders\\ScreenProbeGI.hlsl"), "ScreenProbeGI"))
		ScreenProbeGIPSO = tempPSO;
}


void Corona::InitRaytracingScreenProbePass()
{
		shared_ptr<RTPipelineStateObject> tempPSO = renderBackend->CreateRTPipelineStateObject();
		if (!tempPSO)
			return;
		tempPSO->SetNumInstances(static_cast<uint32_t>(RayTracingInstances.size()));

		tempPSO->AddHitGroup("HitGroup", "chs", "");
		tempPSO->AddShader("rayGen", RTPipelineStateObject::RAYGEN);

		tempPSO->BindUAV("global", "ProbeRadiance", 0);
		tempPSO->BindUAV("global", "ProbeMeta", 1);
		tempPSO->BindUAV("global", "ProbeSH0", 2);
		tempPSO->BindUAV("global", "ProbeSH1", 3);
		tempPSO->BindUAV("global", "ProbeSH2", 4);
		tempPSO->BindUAV("global", "ProbeSH3", 5);
		tempPSO->BindUAV("global", "ProbeSH4", 6);
		tempPSO->BindUAV("global", "ProbeSH5", 7);
		tempPSO->BindUAV("global", "ProbeSH6", 8);
		tempPSO->BindUAV("global", "ProbeSH7", 9);
		tempPSO->BindUAV("global", "ProbeSH8", 10);
		tempPSO->BindSRV("global", "gRtScene", 0);
		tempPSO->BindSRV("global", "DepthTex", 1);
		tempPSO->BindSRV("global", "WorldNormalTex", 2);
		tempPSO->BindSRV("global", "BlueNoiseTex", 7);
		tempPSO->BindSRV("global", "PrevProbeRadianceTex", 8);
		tempPSO->BindSRV("global", "PrevProbeMetaTex", 9);
		tempPSO->BindSRV("global", "VelocityTex", 10);
		tempPSO->BindSRV("global", "PrevDepthTex", 11);
		tempPSO->BindSRV("global", "PrevNormalTex", 12);
		tempPSO->BindSRV("global", "PrevProbeSH0Tex", 13);
		tempPSO->BindSRV("global", "PrevProbeSH1Tex", 14);
		tempPSO->BindSRV("global", "PrevProbeSH2Tex", 15);
		tempPSO->BindSRV("global", "PrevProbeSH3Tex", 16);
		tempPSO->BindSRV("global", "PrevProbeSH4Tex", 17);
		tempPSO->BindSRV("global", "PrevProbeSH5Tex", 18);
		tempPSO->BindSRV("global", "PrevProbeSH6Tex", 19);
		tempPSO->BindSRV("global", "PrevProbeSH7Tex", 20);
		tempPSO->BindSRV("global", "PrevProbeSH8Tex", 21);
		tempPSO->BindSRV("global", "GeoNormalTex", 22);
		tempPSO->BindCBV("global", "ViewParameter", 0, sizeof(RTScreenProbeGIViewParamCB), 1);
		tempPSO->BindSampler("global", "sampleWrap", 0);
		tempPSO->BindSampler("global", "historyClamp", 1);

		tempPSO->AddShader("miss", RTPipelineStateObject::MISS);
		tempPSO->AddShader("missShadow", RTPipelineStateObject::MISS);

		tempPSO->AddShader("chs", RTPipelineStateObject::HIT);
		tempPSO->BindSRV("chs", "vertices", 3);
		tempPSO->BindSRV("chs", "indices", 4);
		tempPSO->BindSRV("chs", "AlbedoTex", 5);
		tempPSO->BindSRV("chs", "InstanceProperty", 6);
		tempPSO->Configure(1, sizeof(float) * 12, sizeof(float) * 2);

		if (tempPSO->InitRS("Shaders\\ScreenProbeRaytracedGI.hlsl"))
			PSO_RT_SCREEN_PROBE_GI = tempPSO;
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

	if (!TLAS || !PSO_RT_SCREEN_PROBE_GI || !ScreenProbeGIRadiance[0] || !ScreenProbeGIRadiance[1] || !hasScreenProbeSHSet(0) || !hasScreenProbeSHSet(1) || !ScreenProbeGIMetadata[0] || !ScreenProbeGIMetadata[1])
		return;
	renderBackend->EmitGpuCrashMarker("ScreenProbeRaytraceGIPass");
	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::D3D12)
	{
		PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "ScreenProbeRaytraceGIPass");
	}

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

	RTScreenProbeGIViewParam.ProbeSpacing = probeSpacing;
	RTScreenProbeGIViewParam.ProbeGridSize = glm::vec2(probeGridWidth, probeGridHeight);
	RTScreenProbeGIViewParam.RTSize = glm::vec2(GetRenderWidth(), GetRenderHeight());
	RTScreenProbeGIViewParam.RaysPerProbe = std::clamp(RTScreenProbeGIViewParam.RaysPerProbe, 1u, 4u);
	RTScreenProbeGIViewParam.HistoryValid = (bScreenProbeGIAtlasHistoryValid && !bScreenProbeLightingBootstrapPending) ? 1u : 0u;
	RTScreenProbeGIViewParam.LightingBootstrap = bScreenProbeLightingBootstrapPending ? 1u : 0u;
	RTScreenProbeGIViewParam.SHCoefficientCount = ScreenProbeGICB.SHCoefficientCount <= 4u ? 4u : 9u;
	RTScreenProbeGIViewParam.TemporalAlpha = std::clamp(ScreenProbeGICB.TemporalAlpha, 0.02f, 1.0f);
	RTScreenProbeGIViewParam.HistoryDepthWeight = ScreenProbeGICB.HistoryDepthWeight;
	RTScreenProbeGIViewParam.HistoryNormalWeight = ScreenProbeGICB.HistoryNormalWeight;

	renderBackend->TransitionTexture(ScreenProbeGIRadiance[writeIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	for (UINT coefficientIndex = 0; coefficientIndex < ScreenProbeSHCoefficientCount; ++coefficientIndex)
		renderBackend->TransitionTexture(ScreenProbeGISH[writeIndex][coefficientIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionTexture(ScreenProbeGIMetadata[writeIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	PSO_RT_SCREEN_PROBE_GI->SetNumInstances(static_cast<uint32_t>(RayTracingInstances.size()));
	PSO_RT_SCREEN_PROBE_GI->BeginShaderTable();

	PSO_RT_SCREEN_PROBE_GI->SetTextureUAV("global", "ProbeRadiance", ScreenProbeGIRadiance[writeIndex].get());
	PSO_RT_SCREEN_PROBE_GI->SetTextureUAV("global", "ProbeMeta", ScreenProbeGIMetadata[writeIndex].get());
	for (UINT coefficientIndex = 0; coefficientIndex < ScreenProbeSHCoefficientCount; ++coefficientIndex)
		PSO_RT_SCREEN_PROBE_GI->SetTextureUAV("global", probeSHUAVNames[coefficientIndex], ScreenProbeGISH[writeIndex][coefficientIndex].get());
	PSO_RT_SCREEN_PROBE_GI->SetAccelerationStructure("global", "gRtScene", TLAS);
	PSO_RT_SCREEN_PROBE_GI->SetTextureSRV("global", "DepthTex", UnjitteredDepthBuffers[ColorBufferWriteIndex].get());
	PSO_RT_SCREEN_PROBE_GI->SetTextureSRV("global", "WorldNormalTex", NormalBuffers[ColorBufferWriteIndex].get());
	PSO_RT_SCREEN_PROBE_GI->SetTextureSRV("global", "BlueNoiseTex", BlueNoiseTex.get());
	PSO_RT_SCREEN_PROBE_GI->SetTextureSRV("global", "PrevProbeRadianceTex", ScreenProbeGIRadiance[readIndex].get());
	PSO_RT_SCREEN_PROBE_GI->SetTextureSRV("global", "PrevProbeMetaTex", ScreenProbeGIMetadata[readIndex].get());
	for (UINT coefficientIndex = 0; coefficientIndex < ScreenProbeSHCoefficientCount; ++coefficientIndex)
		PSO_RT_SCREEN_PROBE_GI->SetTextureSRV("global", prevProbeSHSRVNames[coefficientIndex], ScreenProbeGISH[readIndex][coefficientIndex].get());
	PSO_RT_SCREEN_PROBE_GI->SetTextureSRV("global", "VelocityTex", VelocityBuffer.get());
	PSO_RT_SCREEN_PROBE_GI->SetTextureSRV("global", "PrevDepthTex", UnjitteredDepthBuffers[1 - ColorBufferWriteIndex].get());
	PSO_RT_SCREEN_PROBE_GI->SetTextureSRV("global", "PrevNormalTex", NormalBuffers[1 - ColorBufferWriteIndex].get());
	PSO_RT_SCREEN_PROBE_GI->SetTextureSRV("global", "GeoNormalTex", GeomNormalBuffer.get());
	PSO_RT_SCREEN_PROBE_GI->SetCBVValue("global", "ViewParameter", &RTScreenProbeGIViewParam);
	PSO_RT_SCREEN_PROBE_GI->SetSampler("global", "sampleWrap", samplerWrap.get());
	PSO_RT_SCREEN_PROBE_GI->SetSampler("global", "historyClamp", samplerBilinearWrap.get());

	int i = 0;
	for (const RTInstanceDesc& instance : RayTracingInstances)
	{
		Mesh* mesh = instance.BottomLevelAS->MeshPtr;
		Texture* diffuseTex = mesh->Draws[0].mat->Diffuse.get();
		if (!diffuseTex)
			diffuseTex = DefaultWhiteTex.get();

		PSO_RT_SCREEN_PROBE_GI->ResetHitProgram(i);
		PSO_RT_SCREEN_PROBE_GI->StartHitProgram("HitGroup", i);
		PSO_RT_SCREEN_PROBE_GI->AddSceneGeometrySRVsToHitProgram("HitGroup", mesh->Vb.get(), mesh->Ib.get(), i);
		PSO_RT_SCREEN_PROBE_GI->AddTextureSRVToHitProgram("HitGroup", diffuseTex, i);
		PSO_RT_SCREEN_PROBE_GI->AddBufferSRVToHitProgram("HitGroup", InstancePropertyBuffer.get(), i);
		i++;
	}

	PSO_RT_SCREEN_PROBE_GI->EndShaderTable();
	PSO_RT_SCREEN_PROBE_GI->Apply(probeGridWidth, probeGridHeight);

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
	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::D3D12)
	{
		PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "ScreenProbeGIPass");
	}

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
	ScreenProbeGIPSO->SetTextureSRV("GeoNormalTex", GeomNormalBuffer.get());
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
	ScreenProbeGICB.ProbeGridSize = glm::vec2(
		static_cast<float>((GetRenderWidth() + ScreenProbeGICB.ProbeSpacing - 1u) / ScreenProbeGICB.ProbeSpacing),
		static_cast<float>((GetRenderHeight() + ScreenProbeGICB.ProbeSpacing - 1u) / ScreenProbeGICB.ProbeSpacing));
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
