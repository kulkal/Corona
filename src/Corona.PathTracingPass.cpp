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

void Corona::InitPathTracingPass()
{
	shared_ptr<RTPipelineStateObject> TEMP_PSO_PATH_TRACING = renderBackend->CreateRTPipelineStateObject();
	if (!TEMP_PSO_PATH_TRACING)
		return;
	TEMP_PSO_PATH_TRACING->SetNumInstances(static_cast<uint32_t>(RayTracingInstances.size()));

	TEMP_PSO_PATH_TRACING->AddHitGroup("HitGroup", "PathTracingClosestHit", "PathTracingAnyHit");

	TEMP_PSO_PATH_TRACING->AddShader("PathTracingRayGen", RTPipelineStateObject::RAYGEN);
	
	TEMP_PSO_PATH_TRACING->BindUAV("global", "OutputColor", 0);
	TEMP_PSO_PATH_TRACING->BindSRV("global", "gRtScene", 0);
	TEMP_PSO_PATH_TRACING->BindCBV("global", "ViewParameter", 0, sizeof(PathTracingViewParam), 1);
	TEMP_PSO_PATH_TRACING->BindSampler("global", "sampleWrap", 0);
	TEMP_PSO_PATH_TRACING->BindSRV("global", "BlueNoiseTex", 4);

	TEMP_PSO_PATH_TRACING->AddShader("PathTracingMiss", RTPipelineStateObject::MISS);
	TEMP_PSO_PATH_TRACING->AddShader("ShadowMiss", RTPipelineStateObject::MISS);

	TEMP_PSO_PATH_TRACING->AddShader("PathTracingClosestHit", RTPipelineStateObject::HIT);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingClosestHit", "vertices", 1);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingClosestHit", "indices", 2);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingClosestHit", "InstanceProperty", 3);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingClosestHit", "AlbedoTex", 5);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingClosestHit", "NormalTex", 6);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingClosestHit", "RoughnessTex", 7);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingClosestHit", "MetallicTex", 8);

	TEMP_PSO_PATH_TRACING->AddShader("PathTracingAnyHit", RTPipelineStateObject::ANYHIT);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingAnyHit", "vertices", 1);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingAnyHit", "indices", 2);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingAnyHit", "InstanceProperty", 3);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingAnyHit", "AlbedoTex", 5);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingAnyHit", "NormalTex", 6);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingAnyHit", "RoughnessTex", 7);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingAnyHit", "MetallicTex", 8);
	TEMP_PSO_PATH_TRACING->Configure(8, 192, sizeof(float) * 2);

	bool bSuccess = TEMP_PSO_PATH_TRACING->InitRS("Shaders\\PathTracing.hlsl");

	if (bSuccess)
	{
		PSO_PATH_TRACING = TEMP_PSO_PATH_TRACING;
	}
}

void Corona::PathTracingPass()
{
	if (!TLAS || !PathTracingAccumBuffer[PathTracingWriteIndex])
		return;
	renderBackend->EmitGpuCrashMarker("PathTracingPass");
	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::D3D12)
	{
		PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "PathTracingPass");
	}

	if (!PSO_PATH_TRACING)
	{
		InitPathTracingPass();
		if (!PSO_PATH_TRACING)
			return;
	}

	// Transition output buffer to UAV
	renderBackend->TransitionTexture(PathTracingAccumBuffer[PathTracingWriteIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	// Check if camera or light changed and reset accumulation
	bool cameraChanged = false;
	for (int i = 0; i < 4 && !cameraChanged; i++)
	{
		for (int j = 0; j < 4 && !cameraChanged; j++)
		{
			if (abs(PrevPathTracingViewMat[i][j] - ViewMat[i][j]) > 0.0001f)
			{
				cameraChanged = true;
			}
		}
	}
	
	// Check if light direction or intensity changed
	glm::vec3 currentLightDir = glm::normalize(LightDir);
	bool lightDirChanged = glm::length(currentLightDir - PrevPathTracingLightDir) > 0.0001f;
	bool lightIntensityChanged = abs(LightIntensity - PrevPathTracingLightIntensity) > 0.0001f;
	
	// Check if sky color changed
	bool skyColorChanged = glm::length(SkyColorTop - PrevSkyColorTop) > 0.0001f ||
	                       glm::length(SkyColorBottom - PrevSkyColorBottom) > 0.0001f ||
	                       abs(SkyIntensity - PrevSkyIntensity) > 0.0001f;
	
	if (cameraChanged || lightDirChanged || lightIntensityChanged || skyColorChanged)
	{
		FrameCounter = 0;
		PrevPathTracingViewMat = ViewMat;
		PrevPathTracingLightDir = currentLightDir;
		PrevPathTracingLightIntensity = LightIntensity;
		PrevSkyColorTop = SkyColorTop;
		PrevSkyColorBottom = SkyColorBottom;
		PrevSkyIntensity = SkyIntensity;
		
		// Note: Buffer will be cleared in shader when FrameCounter == 0
	}

	PSO_PATH_TRACING->SetNumInstances(static_cast<uint32_t>(RayTracingInstances.size()));
	PSO_PATH_TRACING->BeginShaderTable();

	PSO_PATH_TRACING->SetTextureUAV("global", "OutputColor", PathTracingAccumBuffer[PathTracingWriteIndex].get());
	PSO_PATH_TRACING->SetAccelerationStructure("global", "gRtScene", TLAS);
	PSO_PATH_TRACING->SetTextureSRV("global", "BlueNoiseTex", BlueNoiseTex.get());
	
	// PathTracingViewParam is already updated in OnUpdate()
	PSO_PATH_TRACING->SetCBVValue("global", "ViewParameter", &PathTracingViewParam);
	PSO_PATH_TRACING->SetSampler("global", "sampleWrap", samplerWrap.get());

	int i = 0;
	for (const RTInstanceDesc& instance : RayTracingInstances)
	{
		Mesh* mesh = instance.BottomLevelAS->MeshPtr;
		
		Texture* diffuseTex = mesh->Draws[0].mat->Diffuse.get();
		if (!diffuseTex)
			diffuseTex = DefaultWhiteTex.get();
		
		Texture* normalTex = mesh->Draws[0].mat->Normal.get();
		if (!normalTex)
			normalTex = DefaultNormalTex.get();
		
		Texture* roughnessTex = mesh->Draws[0].mat->Roughness.get();
		if (!roughnessTex)
			roughnessTex = DefaultRougnessTex.get();
		
		Texture* metallicTex = mesh->Draws[0].mat->Metallic.get();
		if (!metallicTex)
			metallicTex = DefaultBlackTex.get();

		PSO_PATH_TRACING->ResetHitProgram(i);

		PSO_PATH_TRACING->StartHitProgram("HitGroup", i);
		PSO_PATH_TRACING->AddSceneGeometrySRVsToHitProgram("HitGroup", mesh->Vb.get(), mesh->Ib.get(), i);
		PSO_PATH_TRACING->AddBufferSRVToHitProgram("HitGroup", InstancePropertyBuffer.get(), i);
		PSO_PATH_TRACING->AddTextureSRVToHitProgram("HitGroup", diffuseTex, i);
		PSO_PATH_TRACING->AddTextureSRVToHitProgram("HitGroup", normalTex, i);
		PSO_PATH_TRACING->AddTextureSRVToHitProgram("HitGroup", roughnessTex, i);
		PSO_PATH_TRACING->AddTextureSRVToHitProgram("HitGroup", metallicTex, i);

		i++;
	}

	PSO_PATH_TRACING->EndShaderTable();

	PSO_PATH_TRACING->Apply(m_width, m_height);

	// Transition output buffer back to SRV
	renderBackend->TransitionTexture(PathTracingAccumBuffer[PathTracingWriteIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
}
