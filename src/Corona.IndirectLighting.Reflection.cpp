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

void Corona::InitRaytracingReflectionPass()
{
		shared_ptr<RTPipelineStateObject> TEMP_PSO_RT_REFLECTION = renderBackend->CreateRTPipelineStateObject();
		if (!TEMP_PSO_RT_REFLECTION)
			return;
		TEMP_PSO_RT_REFLECTION->SetNumInstances(static_cast<uint32_t>(RayTracingInstances.size()));

		TEMP_PSO_RT_REFLECTION->AddHitGroup("HitGroup", "chs", "");
		//TEMP_PSO_RT_REFLECTION->AddHitGroup("ShadowHitGroup", "chsShadow", "");


		TEMP_PSO_RT_REFLECTION->AddShader("rayGen", RTPipelineStateObject::RAYGEN);
		
		TEMP_PSO_RT_REFLECTION->BindUAV("global", "ReflectionResult", 0);
		TEMP_PSO_RT_REFLECTION->BindSRV("global", "gRtScene", 0);
		TEMP_PSO_RT_REFLECTION->BindSRV("global", "DepthTex", 1);
		TEMP_PSO_RT_REFLECTION->BindSRV("global", "GeoNormalTex", 2);
		TEMP_PSO_RT_REFLECTION->BindSRV("global", "RougnessMetallicTex", 6);
		TEMP_PSO_RT_REFLECTION->BindSRV("global", "BlueNoiseTex", 7);
		TEMP_PSO_RT_REFLECTION->BindSRV("global", "WorldNormalTex", 8);


		TEMP_PSO_RT_REFLECTION->BindCBV("global", "ViewParameter", 0, sizeof(RTReflectionViewParam), 1);
		TEMP_PSO_RT_REFLECTION->BindSampler("global", "sampleWrap", 0);

		TEMP_PSO_RT_REFLECTION->AddShader("miss", RTPipelineStateObject::MISS);
		TEMP_PSO_RT_REFLECTION->AddShader("missShadow", RTPipelineStateObject::MISS);


		TEMP_PSO_RT_REFLECTION->AddShader("chs", RTPipelineStateObject::HIT);
		TEMP_PSO_RT_REFLECTION->BindSRV("chs", "vertices", 3);
		TEMP_PSO_RT_REFLECTION->BindSRV("chs", "indices", 4);
		TEMP_PSO_RT_REFLECTION->BindSRV("chs", "AlbedoTex", 5);
		TEMP_PSO_RT_REFLECTION->BindSRV("chs", "InstanceProperty", 9);
		TEMP_PSO_RT_REFLECTION->Configure(1, sizeof(float) * 13, sizeof(float) * 2);

		bool bSuccess = TEMP_PSO_RT_REFLECTION->InitRS("Shaders\\RaytracedReflection.hlsl");

		if (bSuccess)
		{
			PSO_RT_REFLECTION = TEMP_PSO_RT_REFLECTION;
		}
}

void Corona::RaytraceReflectionPass()
{
	if (!TLAS || !PSO_RT_REFLECTION)
		return;
	renderBackend->EmitGpuCrashMarker("RaytraceReflectionPass");
	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::D3D12)
	{
		PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "RaytraceReflectionPass");
	}

	renderBackend->TransitionTexture(SpecularGIRaw.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	const FLOAT clearReflection[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	renderBackend->ClearTextureUAVFloat(SpecularGIRaw.get(), clearReflection);

	RTReflectionViewParam.ViewSpreadAngle = glm::tan(Fov * 0.5) / (0.5f * GetRenderHeight());

	RTPassBuilder pass(*this, PSO_RT_REFLECTION);
	pass.BeginScene()
		.SetTextureUAV("global", "ReflectionResult", SpecularGIRaw.get())
		.SetAccelerationStructure("global", "gRtScene", TLAS)
		.SetTextureSRV("global", "DepthTex", UnjitteredDepthBuffers[ColorBufferWriteIndex].get())
		.SetTextureSRV("global", "GeoNormalTex", GeomNormalBuffers[ColorBufferWriteIndex].get())
		.SetTextureSRV("global", "RougnessMetallicTex", RoughnessMetalicBuffer.get())
		.SetTextureSRV("global", "BlueNoiseTex", BlueNoiseTex.get())
		.SetTextureSRV("global", "WorldNormalTex", NormalBuffers[ColorBufferWriteIndex].get())
		.SetCBVValue("global", "ViewParameter", &RTReflectionViewParam)
		.SetSampler("global", "sampleWrap", samplerWrap.get());
	pass.BindSceneHitPrograms();
	pass.Dispatch(GetRenderWidth(), GetRenderHeight());

	renderBackend->TransitionTexture(SpecularGIRaw.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	//PIXEndEvent();
}
