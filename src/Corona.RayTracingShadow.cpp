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
// DX12 raw command list access for the ReSTIR Phase 2 CopyResource path.
// Vulkan-side support pending; the copy is gated on dx12_rhi being valid.
#include "DX12Backend.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>

void Corona::InitRaytracingShadowPass()
{
		shared_ptr<RTPipelineStateObject> TEMP_PSO_RT_SHADOW = renderBackend->CreateRTPipelineStateObject();
		if (!TEMP_PSO_RT_SHADOW)
			return;
		TEMP_PSO_RT_SHADOW->SetNumInstances(static_cast<uint32_t>(RayTracingInstances.size())); // important for cbv allocation & shadertable size.

		// new interface
		TEMP_PSO_RT_SHADOW->AddHitGroup("HitGroup", "", "anyhit");
		TEMP_PSO_RT_SHADOW->AddShader("rayGen", RTPipelineStateObject::RAYGEN);

		TEMP_PSO_RT_SHADOW->BindUAV("global", "ShadowResult", 0);
		TEMP_PSO_RT_SHADOW->BindSRV("global", "gRtScene", 0);
		TEMP_PSO_RT_SHADOW->BindSRV("global", "DepthTex", 1);
		TEMP_PSO_RT_SHADOW->BindSRV("global", "WorldNormalTex", 2);
		TEMP_PSO_RT_SHADOW->BindSRV("global", "GeoNormalTex", 7);
		TEMP_PSO_RT_SHADOW->BindSRV("global", "RayNoiseBlueNoiseSource", 8);
		// ReSTIR Phase 2 temporal reuse — bound even in Option A mode so
		// the root signature stays uniform across mode toggles; the
		// shader only reads from these when ShadowMode == 1.
		TEMP_PSO_RT_SHADOW->BindSRV("global", "ShadowReservoirPrev", 9);
		TEMP_PSO_RT_SHADOW->BindSRV("global", "VelocityTex", 10);
		// Phase 2b — per-pixel M tracking. Separate single-channel buffer.
		TEMP_PSO_RT_SHADOW->BindUAV("global", "ShadowReservoirM", 1);
		TEMP_PSO_RT_SHADOW->BindSRV("global", "ShadowReservoirMPrev", 11);

		TEMP_PSO_RT_SHADOW->BindCBV("global", "ViewParameter", 0, sizeof(RTShadowViewParamCB), 1);
		TEMP_PSO_RT_SHADOW->BindSampler("global", "sampleWrap", 0);



		TEMP_PSO_RT_SHADOW->AddShader("miss", RTPipelineStateObject::MISS);
		
		TEMP_PSO_RT_SHADOW->AddShader("anyhit", RTPipelineStateObject::ANYHIT);
		TEMP_PSO_RT_SHADOW->BindSRV("anyhit", "vertices", 3);
		TEMP_PSO_RT_SHADOW->BindSRV("anyhit", "indices", 4);
		TEMP_PSO_RT_SHADOW->BindSRV("anyhit", "AlbedoTex", 5);
		TEMP_PSO_RT_SHADOW->BindSRV("anyhit", "InstanceProperty", 6);
		TEMP_PSO_RT_SHADOW->Configure(1, sizeof(float) * 4, sizeof(float) * 2);

		bool bSuccess = TEMP_PSO_RT_SHADOW->InitRS("Shaders\\RaytracedShadow.hlsl");
		if (bSuccess)
		{
			PSO_RT_SHADOW = TEMP_PSO_RT_SHADOW;
		}
}

void Corona::RaytraceShadowPass()
{
	if (!ShadowBuffer || !BlueNoiseTex || !TLAS || !PSO_RT_SHADOW || !UnjitteredDepthBuffers[ColorBufferWriteIndex] || !NormalBuffers[ColorBufferWriteIndex] || !GeomNormalBuffers[ColorBufferWriteIndex])
		return;
	renderBackend->EmitGpuCrashMarker("RaytraceShadowPass");

	RTShadowViewParam.ViewMatrix = glm::transpose(ViewMat);
	RTShadowViewParam.InvViewMatrix = glm::transpose(InvViewMat);
	RTShadowViewParam.ProjMatrix = glm::transpose(UnjitteredProjMat);
	RTShadowViewParam.InvProjMatrix = glm::transpose(UnjitteredInvProjMat);
	RTShadowViewParam.ProjectionParams = FrameProjectionParams;
	RTShadowViewParam.LightDir = glm::vec4(RenderFrameNormalizedLightDir, 0.0f);
	RTShadowViewParam.ShadowLightRadius = std::clamp(RTShadowViewParam.ShadowLightRadius, 0.0f, 0.03f);
	RTShadowViewParam.ShadowSampleCount = std::clamp(RTShadowViewParam.ShadowSampleCount, 1u, 16u);
	RTShadowViewParam.FrameCounter = RenderFrameIndex;
	RTShadowViewParam.BlueNoiseOffsetStride = RTGIViewParam.BlueNoiseOffsetStride;
	RTShadowViewParam.NoiseMode = RenderFrameRayNoiseMode;

	// Shadow mode handling:
	//   Option A (default): first 3 enabled lights packed into ShadowBuffer
	//                       GBA channels. The shader caps at 3.
	//   ReSTIR Phase 1    : up to 8 candidates, each weighted by luma *
	//                       intensity for RIS. The shader picks 1 per pixel.
	RTShadowViewParam.ShadowMode = bEnableReSTIRDirectShadow ? 1u : 0u;

	// Candidate gather:
	//   Option A   — only 3 channels available, so pick the 3 lights most
	//                relevant to what the camera sees: in-frustum (sphere
	//                test) ∩ nearest to camera. Frustum culling avoids
	//                wasting GBA slots on lights outside the view.
	//   ReSTIR Phase 1 — feed every enabled light as a candidate; the RIS
	//                pass weights by luma * NdotL * range so the picker
	//                stays meaningful even at 100+ lights.
	auto computeLuma = [](const PointLightState& pl) {
		const float luma = 0.2126f * pl.Color.r + 0.7152f * pl.Color.g + 0.0722f * pl.Color.b;
		return std::max(0.0f, luma) * std::max(0.0f, pl.Intensity);
	};

	uint32_t shadowedCount = 0;
	if (bEnableReSTIRDirectShadow)
	{
		for (const PointLightState& pl : RenderWorld.PointLights)
		{
			if (shadowedCount >= MaxPointLights)
				break;
			if (!pl.bEnabled || pl.Intensity <= 0.0f)
				continue;
			RTShadowViewParam.ShadowedPointLights[shadowedCount] =
				glm::vec4(pl.Position, std::max(pl.Radius, 0.01f));
			RTShadowViewParam.ShadowedPointLightWeights[shadowedCount] =
				glm::vec4(computeLuma(pl), 0.0f, 0.0f, 0.0f);
			++shadowedCount;
		}
	}
	else
	{
		// Option A — score and sort.
		struct Candidate {
			uint32_t Index;
			float DistSq;
		};
		std::vector<Candidate> candidates;
		candidates.reserve(RenderWorld.PointLights.size());
		const glm::vec3 camPos = RenderFrameCameraPosition;
		for (uint32_t i = 0; i < RenderWorld.PointLights.size(); ++i)
		{
			const PointLightState& pl = RenderWorld.PointLights[i];
			if (!pl.bEnabled || pl.Intensity <= 0.0f)
				continue;
			const float radius = std::max(pl.Radius, 0.01f);
			// Sphere-AABB frustum test approximated as point-AABB test
			// against a slightly inflated bounding box of the light's
			// reach. Cheap; lights right on the frustum boundary may flip
			// in/out as the camera rotates but soft shadow / TAA hides it.
			const glm::vec3 boundsMin = pl.Position - glm::vec3(radius);
			const glm::vec3 boundsMax = pl.Position + glm::vec3(radius);
			if (!IsWorldAabbInViewFrustum(boundsMin, boundsMax))
				continue;
			const glm::vec3 toLight = pl.Position - camPos;
			candidates.push_back({ i, glm::dot(toLight, toLight) });
		}
		std::sort(candidates.begin(), candidates.end(),
			[](const Candidate& a, const Candidate& b) { return a.DistSq < b.DistSq; });
		const uint32_t pick = std::min<uint32_t>(static_cast<uint32_t>(candidates.size()), 3u);
		for (uint32_t k = 0; k < pick; ++k)
		{
			const PointLightState& pl = RenderWorld.PointLights[candidates[k].Index];
			RTShadowViewParam.ShadowedPointLights[k] =
				glm::vec4(pl.Position, std::max(pl.Radius, 0.01f));
			RTShadowViewParam.ShadowedPointLightWeights[k] =
				glm::vec4(computeLuma(pl), 0.0f, 0.0f, 0.0f);
		}
		shadowedCount = pick;
	}

	for (uint32_t i = shadowedCount; i < MaxPointLights; ++i)
	{
		RTShadowViewParam.ShadowedPointLights[i] = glm::vec4(0.0f);
		RTShadowViewParam.ShadowedPointLightWeights[i] = glm::vec4(0.0f);
	}
	RTShadowViewParam.ShadowedPointLightCount = shadowedCount;

	renderBackend->TransitionTexture(ShadowBuffer.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	if (ShadowReservoirMBuffer)
		renderBackend->TransitionTexture(ShadowReservoirMBuffer.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	RTPassBuilder pass(*this, PSO_RT_SHADOW);
	pass.BeginScene()
		.SetTextureUAV("global", "ShadowResult", ShadowBuffer.get())
		.SetTextureUAV("global", "ShadowReservoirM", ShadowReservoirMBuffer.get())
		.SetAccelerationStructure("global", "gRtScene", TLAS)
		.SetTextureSRV("global", "DepthTex", UnjitteredDepthBuffers[ColorBufferWriteIndex].get())
		.SetTextureSRV("global", "WorldNormalTex", NormalBuffers[ColorBufferWriteIndex].get())
		.SetTextureSRV("global", "GeoNormalTex", GeomNormalBuffers[ColorBufferWriteIndex].get())
		.SetTextureSRV("global", "RayNoiseBlueNoiseSource", BlueNoiseTex.get())
		.SetTextureSRV("global", "ShadowReservoirPrev",
			ShadowReservoirPrevBuffer ? ShadowReservoirPrevBuffer.get() : ShadowBuffer.get())
		.SetTextureSRV("global", "ShadowReservoirMPrev",
			ShadowReservoirMPrevBuffer ? ShadowReservoirMPrevBuffer.get() : ShadowBuffer.get())
		.SetTextureSRV("global", "VelocityTex", VelocityBuffer.get())
		.SetCBVValue("global", "ViewParameter", &RTShadowViewParam)
		.SetSampler("global", "sampleWrap", samplerWrap.get());
	pass.BindSceneHitPrograms();
	pass.Dispatch(GetRenderWidth(), GetRenderHeight());

	renderBackend->TransitionTexture(ShadowBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	if (ShadowReservoirMBuffer)
		renderBackend->TransitionTexture(ShadowReservoirMBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);

	// ReSTIR Phase 2 temporal feedback: cache this frame's reservoirs for
	// next-frame reproject via raw DX12 CopyResource. Only meaningful in
	// ReSTIR mode; in Option A the copy is skipped. DX12-only for now —
	// Vulkan path will follow with a backend-abstracted texture copy.
	if (bEnableReSTIRDirectShadow && ShadowReservoirPrevBuffer && dx12_rhi)
	{
		renderBackend->TransitionTexture(ShadowReservoirPrevBuffer.get(),
			EResourceState::ShaderRead, EResourceState::CopyDest);
		renderBackend->TransitionTexture(ShadowBuffer.get(),
			EResourceState::ShaderRead, EResourceState::CopySource);
		dx12_rhi->GetGraphicsCommandList()->CopyResource(
			ShadowReservoirPrevBuffer->resource.Get(),
			ShadowBuffer->resource.Get());
		renderBackend->TransitionTexture(ShadowReservoirPrevBuffer.get(),
			EResourceState::CopyDest, EResourceState::ShaderRead);
		renderBackend->TransitionTexture(ShadowBuffer.get(),
			EResourceState::CopySource, EResourceState::ShaderRead);

		if (ShadowReservoirMBuffer && ShadowReservoirMPrevBuffer)
		{
			renderBackend->TransitionTexture(ShadowReservoirMPrevBuffer.get(),
				EResourceState::ShaderRead, EResourceState::CopyDest);
			renderBackend->TransitionTexture(ShadowReservoirMBuffer.get(),
				EResourceState::ShaderRead, EResourceState::CopySource);
			dx12_rhi->GetGraphicsCommandList()->CopyResource(
				ShadowReservoirMPrevBuffer->resource.Get(),
				ShadowReservoirMBuffer->resource.Get());
			renderBackend->TransitionTexture(ShadowReservoirMPrevBuffer.get(),
				EResourceState::CopyDest, EResourceState::ShaderRead);
			renderBackend->TransitionTexture(ShadowReservoirMBuffer.get(),
				EResourceState::CopySource, EResourceState::ShaderRead);
		}
	}

	bShadowOutputValidThisFrame = true;
}
