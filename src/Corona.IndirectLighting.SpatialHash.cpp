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

void Corona::InitSpatialHashGIPass()
{
	auto createSpatialHashPSO = [&](const std::string& entryPoint)
	{
		shared_ptr<ComputePipelineStateObject> pso = renderBackend->CreateComputePipelineStateObject();
		if (!pso)
			return shared_ptr<ComputePipelineStateObject>();

		pso->BindSRV("DepthTex", 0, 1);
		pso->BindSRV("WorldNormalTex", 1, 1);
		pso->BindSRV("GeoNormalTex", 2, 1);
		pso->BindSRV("ActiveCellSlotsIn", 5, 1);
		pso->BindSRV("CellPositionIn", 6, 1);
		pso->BindSRV("CellNormalIn", 7, 1);
		pso->BindSRV("TraceSH0In", 8, 1);
		pso->BindSRV("TraceSH1In", 9, 1);
		pso->BindSRV("TraceSH2In", 10, 1);
		pso->BindSRV("TraceSH3In", 11, 1);
		pso->BindSRV("PrevResolvedKeys", 12, 1);
		pso->BindSRV("PrevResolvedSH0", 13, 1);
		pso->BindSRV("PrevResolvedSH1", 14, 1);
		pso->BindSRV("PrevResolvedSH2", 15, 1);
		pso->BindSRV("PrevResolvedSH3", 16, 1);
		pso->BindSRV("ResolvedKeysIn", 17, 1);
		pso->BindSRV("ResolvedSH0In", 18, 1);
		pso->BindSRV("ResolvedSH1In", 19, 1);
		pso->BindSRV("ResolvedSH2In", 20, 1);
		pso->BindSRV("ResolvedSH3In", 21, 1);
		pso->BindSRV("ActiveCounterIn", 22, 1);
		pso->BindUAV("ActiveFlagsOut", 0);
		pso->BindUAV("CellPositionOut", 1);
		pso->BindUAV("CellNormalOut", 2);
		pso->BindUAV("CellScoreOut", 3);
		pso->BindUAV("ResolvedKeysOut", 4);
		pso->BindUAV("ResolvedSH0Out", 5);
		pso->BindUAV("ResolvedSH1Out", 6);
		pso->BindUAV("ResolvedSH2Out", 7);
		pso->BindUAV("ResolvedSH3Out", 8);
		pso->BindUAV("OutGIHashColor", 9);
		pso->BindUAV("OutGIHashSH", 10);
		pso->BindUAV("ActiveCellSlotsOut", 11);
		pso->BindUAV("ActiveCounterOut", 12);
		pso->BindCBV("SpatialHashGIConstant", 0, sizeof(SpatialHashGIConstant));

		if (!pso->InitCS(GetAssetFullPath(L"Shaders\\SpatialHashDiffuseGI.hlsl"), entryPoint))
			return shared_ptr<ComputePipelineStateObject>();

		return pso;
	};

	SpatialHashGIClearPSO = createSpatialHashPSO("SpatialHashClear");
	SpatialHashGIUpdatePSO = createSpatialHashPSO("SpatialHashUpdate");
	SpatialHashGIResolvePSO = createSpatialHashPSO("SpatialHashResolve");
	SpatialHashGIQueryPSO = createSpatialHashPSO("SpatialHashQuery");
}

void Corona::InitRaytracingSpatialHashPass()
{
		shared_ptr<RTPipelineStateObject> tempPSO = renderBackend->CreateRTPipelineStateObject();
		if (!tempPSO)
			return;
		tempPSO->SetNumInstances(static_cast<uint32_t>(RayTracingInstances.size()));

		tempPSO->AddHitGroup("HitGroup", "chs", "");
		tempPSO->AddShader("rayGen", RTPipelineStateObject::RAYGEN);

		tempPSO->BindUAV("global", "TraceSH0", 0);
		tempPSO->BindUAV("global", "TraceSH1", 1);
		tempPSO->BindUAV("global", "TraceSH2", 2);
		tempPSO->BindUAV("global", "TraceSH3", 3);
		tempPSO->BindSRV("global", "gRtScene", 0);
		tempPSO->BindSRV("global", "CellKeys", 1);
		tempPSO->BindSRV("global", "CellPosition", 2);
		tempPSO->BindSRV("global", "CellNormal", 3);
		tempPSO->BindSRV("global", "BlueNoiseTex", 4);
		tempPSO->BindSRV("global", "ActiveCellSlots", 9);
		tempPSO->BindSRV("global", "ActiveCounter", 10);
		tempPSO->BindCBV("global", "ViewParameter", 0, sizeof(RTSpatialHashGIViewParamCB), 1);
		tempPSO->BindSampler("global", "sampleWrap", 0);

		tempPSO->AddShader("miss", RTPipelineStateObject::MISS);
		tempPSO->AddShader("missShadow", RTPipelineStateObject::MISS);

		tempPSO->AddShader("chs", RTPipelineStateObject::HIT);
		tempPSO->BindSRV("chs", "vertices", 5);
		tempPSO->BindSRV("chs", "indices", 6);
		tempPSO->BindSRV("chs", "AlbedoTex", 7);
		tempPSO->BindSRV("chs", "InstanceProperty", 8);
		tempPSO->Configure(1, sizeof(float) * 12, sizeof(float) * 2);

		if (tempPSO->InitRS("Shaders\\SpatialHashCellGI.hlsl"))
			PSO_RT_SPATIAL_HASH_GI = tempPSO;
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

	if (!TLAS || !PSO_RT_SPATIAL_HASH_GI ||
		!SpatialHashGIClearPSO || !SpatialHashGIUpdatePSO || !SpatialHashGIResolvePSO || !SpatialHashGIQueryPSO ||
		!SpatialHashGIActiveFlags || !SpatialHashGIActiveCellSlots || !SpatialHashGIActiveCounter ||
		!SpatialHashGICellPosition || !SpatialHashGICellNormal || !SpatialHashGICellScore ||
		!SpatialHashGIResolvedKeys[0] ||
		!hasSpatialHashSHBuffers() ||
		!DiffuseGIHashCached || !DiffuseGIHashCachedAux)
		return;

	renderBackend->EmitGpuCrashMarker("SpatialHashGIPass");
	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::D3D12)
	{
		PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "SpatialHashGIPass");
	}

	const UINT32 cacheIndex = 0u;
	const UINT32 spatialHashTraceCellBudget = std::min(SpatialHashGITraceCellBudget, SpatialHashGIActiveCellCapacity);

	SpatialHashGICB.HashEntryCount = SpatialHashGIEntryCount;
	SpatialHashGICB.HashEntryMask = SpatialHashGIEntryCount - 1u;
	SpatialHashGICB.ActiveCellCapacity = SpatialHashGIActiveCellCapacity;
	SpatialHashGICB.TraceCellBudget = spatialHashTraceCellBudget;
	SpatialHashGICB.RTSize = glm::vec2(GetRenderWidth(), GetRenderHeight());
	SpatialHashGICB.FrameIndex = FrameCounter;
	SpatialHashGICB.HistoryValid = bSpatialHashGIHistoryValid ? 1u : 0u;
	SpatialHashGICB.MaxProbeSteps = std::clamp(SpatialHashGICB.MaxProbeSteps, 1u, 16u);
	SpatialHashGICB.CellSize = std::clamp(SpatialHashGICB.CellSize, 4.0f, 256.0f);
	SpatialHashGICB.HistorySampleDecay = std::clamp(SpatialHashGICB.HistorySampleDecay, 0.0f, 1.0f);
	SpatialHashGICB.SmoothingStrength = std::clamp(SpatialHashGICB.SmoothingStrength, 0.0f, 1.0f);
	SpatialHashGICB.TemporalAlpha = std::clamp(SpatialHashGICB.TemporalAlpha, 0.02f, 1.0f);
	SpatialHashGICB.InterpolationStrength = std::clamp(SpatialHashGICB.InterpolationStrength, 0.0f, 1.0f);
	RTSpatialHashGIViewParam.HashEntryCount = spatialHashTraceCellBudget;
	RTSpatialHashGIViewParam.FrameCounter = FrameCounter;
	RTSpatialHashGIViewParam.RaysPerCell = std::clamp(RTSpatialHashGIViewParam.RaysPerCell, 1u, 8u);
	RTSpatialHashGIViewParam.MaxBounces = std::clamp(RTSpatialHashGIViewParam.MaxBounces, 1u, 8u);
	RTSpatialHashGIViewParam.CellSize = SpatialHashGICB.CellSize;
	RTSpatialHashGIViewParam.RayBias = std::clamp(SpatialHashGICB.CellSize * 0.02f, 0.05f, 0.5f);
	RTSpatialHashGIViewParam.ViewSpreadAngle = glm::tan(Fov * 0.5f) / (0.5f * GetRenderHeight());
	RTSpatialHashGIViewParam.ActiveCellCapacity = SpatialHashGIActiveCellCapacity;

	renderBackend->TransitionBuffer(SpatialHashGIActiveFlags.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionBuffer(SpatialHashGIActiveCellSlots.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionBuffer(SpatialHashGIActiveCounter.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionBuffer(SpatialHashGICellPosition.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionBuffer(SpatialHashGICellNormal.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionBuffer(SpatialHashGICellScore.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionBuffer(SpatialHashGIResolvedKeys[cacheIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	for (UINT coefficientIndex = 0; coefficientIndex < SpatialHashGISHCoefficientCount; ++coefficientIndex)
	{
		renderBackend->TransitionBuffer(SpatialHashGITraceSH[coefficientIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->TransitionBuffer(SpatialHashGIResolvedSH[cacheIndex][coefficientIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	}

	SpatialHashGIClearPSO->SetBufferUAV("ActiveFlagsOut", SpatialHashGIActiveFlags.get());
	SpatialHashGIClearPSO->SetBufferUAV("CellPositionOut", SpatialHashGICellPosition.get());
	SpatialHashGIClearPSO->SetBufferUAV("CellNormalOut", SpatialHashGICellNormal.get());
	SpatialHashGIClearPSO->SetBufferUAV("CellScoreOut", SpatialHashGICellScore.get());
	SpatialHashGIClearPSO->SetBufferUAV("ResolvedKeysOut", SpatialHashGIResolvedKeys[cacheIndex].get());
	SpatialHashGIClearPSO->SetBufferUAV("ResolvedSH0Out", SpatialHashGIResolvedSH[cacheIndex][0].get());
	SpatialHashGIClearPSO->SetBufferUAV("ResolvedSH1Out", SpatialHashGIResolvedSH[cacheIndex][1].get());
	SpatialHashGIClearPSO->SetBufferUAV("ResolvedSH2Out", SpatialHashGIResolvedSH[cacheIndex][2].get());
	SpatialHashGIClearPSO->SetBufferUAV("ResolvedSH3Out", SpatialHashGIResolvedSH[cacheIndex][3].get());
	SpatialHashGIClearPSO->SetBufferUAV("ActiveCellSlotsOut", SpatialHashGIActiveCellSlots.get());
	SpatialHashGIClearPSO->SetBufferUAV("ActiveCounterOut", SpatialHashGIActiveCounter.get());
	SpatialHashGIClearPSO->SetCBVValue("SpatialHashGIConstant", &SpatialHashGICB);
	SpatialHashGIClearPSO->Apply();
	const UINT32 spatialHashClearEntryCount = bSpatialHashGIHistoryValid ? 1u : SpatialHashGIEntryCount;
	renderBackend->Dispatch((spatialHashClearEntryCount + 255u) / 256u, 1u, 1u);

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
	for (UINT coefficientIndex = 0; coefficientIndex < SpatialHashGISHCoefficientCount; ++coefficientIndex)
		renderBackend->TransitionBuffer(SpatialHashGIResolvedSH[cacheIndex][coefficientIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	SpatialHashGIUpdatePSO->SetTextureSRV("DepthTex", UnjitteredDepthBuffers[ColorBufferWriteIndex].get());
	SpatialHashGIUpdatePSO->SetTextureSRV("WorldNormalTex", NormalBuffers[ColorBufferWriteIndex].get());
	SpatialHashGIUpdatePSO->SetTextureSRV("GeoNormalTex", GeomNormalBuffer.get());
	SpatialHashGIUpdatePSO->SetBufferUAV("ActiveFlagsOut", SpatialHashGIActiveFlags.get());
	SpatialHashGIUpdatePSO->SetBufferUAV("CellPositionOut", SpatialHashGICellPosition.get());
	SpatialHashGIUpdatePSO->SetBufferUAV("CellNormalOut", SpatialHashGICellNormal.get());
	SpatialHashGIUpdatePSO->SetBufferUAV("CellScoreOut", SpatialHashGICellScore.get());
	SpatialHashGIUpdatePSO->SetBufferUAV("ResolvedKeysOut", SpatialHashGIResolvedKeys[cacheIndex].get());
	SpatialHashGIUpdatePSO->SetBufferUAV("ActiveCellSlotsOut", SpatialHashGIActiveCellSlots.get());
	SpatialHashGIUpdatePSO->SetBufferUAV("ActiveCounterOut", SpatialHashGIActiveCounter.get());
	SpatialHashGIUpdatePSO->SetCBVValue("SpatialHashGIConstant", &SpatialHashGICB);
	SpatialHashGIUpdatePSO->Apply();
	renderBackend->Dispatch((GetRenderWidth() + 7u) / 8u, (GetRenderHeight() + 7u) / 8u, 1u);

	renderBackend->TransitionBuffer(SpatialHashGIActiveFlags.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionBuffer(SpatialHashGIActiveCellSlots.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionBuffer(SpatialHashGIActiveCounter.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionBuffer(SpatialHashGICellPosition.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionBuffer(SpatialHashGICellNormal.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionBuffer(SpatialHashGICellScore.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionBuffer(SpatialHashGIResolvedKeys[cacheIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);

	PSO_RT_SPATIAL_HASH_GI->SetNumInstances(static_cast<uint32_t>(RayTracingInstances.size()));
	PSO_RT_SPATIAL_HASH_GI->BeginShaderTable();
	PSO_RT_SPATIAL_HASH_GI->SetBufferUAV("global", "TraceSH0", SpatialHashGITraceSH[0].get());
	PSO_RT_SPATIAL_HASH_GI->SetBufferUAV("global", "TraceSH1", SpatialHashGITraceSH[1].get());
	PSO_RT_SPATIAL_HASH_GI->SetBufferUAV("global", "TraceSH2", SpatialHashGITraceSH[2].get());
	PSO_RT_SPATIAL_HASH_GI->SetBufferUAV("global", "TraceSH3", SpatialHashGITraceSH[3].get());
	PSO_RT_SPATIAL_HASH_GI->SetAccelerationStructure("global", "gRtScene", TLAS);
	PSO_RT_SPATIAL_HASH_GI->SetBufferSRV("global", "CellKeys", SpatialHashGIResolvedKeys[cacheIndex].get());
	PSO_RT_SPATIAL_HASH_GI->SetBufferSRV("global", "CellPosition", SpatialHashGICellPosition.get());
	PSO_RT_SPATIAL_HASH_GI->SetBufferSRV("global", "CellNormal", SpatialHashGICellNormal.get());
	PSO_RT_SPATIAL_HASH_GI->SetTextureSRV("global", "BlueNoiseTex", BlueNoiseTex.get());
	PSO_RT_SPATIAL_HASH_GI->SetBufferSRV("global", "ActiveCellSlots", SpatialHashGIActiveCellSlots.get());
	PSO_RT_SPATIAL_HASH_GI->SetBufferSRV("global", "ActiveCounter", SpatialHashGIActiveCounter.get());
	PSO_RT_SPATIAL_HASH_GI->SetCBVValue("global", "ViewParameter", &RTSpatialHashGIViewParam);
	PSO_RT_SPATIAL_HASH_GI->SetSampler("global", "sampleWrap", samplerWrap.get());

	int i = 0;
	for (const RTInstanceDesc& instance : RayTracingInstances)
	{
		Mesh* mesh = instance.BottomLevelAS->MeshPtr;
		Texture* diffuseTex = mesh->Draws[0].mat->Diffuse.get();
		if (!diffuseTex)
			diffuseTex = DefaultWhiteTex.get();

		PSO_RT_SPATIAL_HASH_GI->ResetHitProgram(i);
		PSO_RT_SPATIAL_HASH_GI->StartHitProgram("HitGroup", i);
		PSO_RT_SPATIAL_HASH_GI->AddSceneGeometrySRVsToHitProgram("HitGroup", mesh->Vb.get(), mesh->Ib.get(), i);
		PSO_RT_SPATIAL_HASH_GI->AddTextureSRVToHitProgram("HitGroup", diffuseTex, i);
		PSO_RT_SPATIAL_HASH_GI->AddBufferSRVToHitProgram("HitGroup", InstancePropertyBuffer.get(), i);
		i++;
	}

	PSO_RT_SPATIAL_HASH_GI->EndShaderTable();
	PSO_RT_SPATIAL_HASH_GI->Apply(spatialHashTraceCellBudget, 1u);

	for (UINT coefficientIndex = 0; coefficientIndex < SpatialHashGISHCoefficientCount; ++coefficientIndex)
		renderBackend->TransitionBuffer(SpatialHashGITraceSH[coefficientIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);

	renderBackend->TransitionBuffer(SpatialHashGIResolvedKeys[cacheIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

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

	renderBackend->TransitionBuffer(SpatialHashGIResolvedKeys[cacheIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	for (UINT coefficientIndex = 0; coefficientIndex < SpatialHashGISHCoefficientCount; ++coefficientIndex)
		renderBackend->TransitionBuffer(SpatialHashGIResolvedSH[cacheIndex][coefficientIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);

	renderBackend->TransitionTexture(DiffuseGIHashCached.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionTexture(DiffuseGIHashCachedAux.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	SpatialHashGIQueryPSO->SetTextureSRV("DepthTex", UnjitteredDepthBuffers[ColorBufferWriteIndex].get());
	SpatialHashGIQueryPSO->SetTextureSRV("WorldNormalTex", NormalBuffers[ColorBufferWriteIndex].get());
	SpatialHashGIQueryPSO->SetTextureSRV("GeoNormalTex", GeomNormalBuffer.get());
	SpatialHashGIQueryPSO->SetBufferSRV("ResolvedKeysIn", SpatialHashGIResolvedKeys[cacheIndex].get());
	SpatialHashGIQueryPSO->SetBufferSRV("ResolvedSH0In", SpatialHashGIResolvedSH[cacheIndex][0].get());
	SpatialHashGIQueryPSO->SetBufferSRV("ResolvedSH1In", SpatialHashGIResolvedSH[cacheIndex][1].get());
	SpatialHashGIQueryPSO->SetBufferSRV("ResolvedSH2In", SpatialHashGIResolvedSH[cacheIndex][2].get());
	SpatialHashGIQueryPSO->SetBufferSRV("ResolvedSH3In", SpatialHashGIResolvedSH[cacheIndex][3].get());
	SpatialHashGIQueryPSO->SetTextureUAV("OutGIHashColor", DiffuseGIHashCached.get());
	SpatialHashGIQueryPSO->SetTextureUAV("OutGIHashSH", DiffuseGIHashCachedAux.get());
	SpatialHashGIQueryPSO->SetCBVValue("SpatialHashGIConstant", &SpatialHashGICB);
	SpatialHashGIQueryPSO->Apply();
	renderBackend->Dispatch((GetRenderWidth() + 7u) / 8u, (GetRenderHeight() + 7u) / 8u, 1u);

	renderBackend->TransitionTexture(DiffuseGIHashCached.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(DiffuseGIHashCachedAux.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	bSpatialHashGIHistoryValid = true;
}
