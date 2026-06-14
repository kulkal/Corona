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

		const RHIShaderStageMask rayGenStage = ToRHIShaderStageMask(RHIShaderStage::RayGeneration);
		const RHIShaderStageMask anyHitStage = ToRHIShaderStageMask(RHIShaderStage::AnyHit);
		TEMP_PSO_RT_SHADOW->BindUAV("global", MakeRHITextureUAV("ShadowResult", 0, rayGenStage));
		TEMP_PSO_RT_SHADOW->BindSRV("global", MakeRHIAccelerationStructureSRV("gRtScene", 0, rayGenStage));
		TEMP_PSO_RT_SHADOW->BindSRV("global", MakeRHITextureSRV("DepthTex", 1, rayGenStage));
		TEMP_PSO_RT_SHADOW->BindSRV("global", MakeRHITextureSRV("WorldNormalTex", 2, rayGenStage));
		TEMP_PSO_RT_SHADOW->BindSRV("global", MakeRHITextureSRV("GeoNormalTex", 7, rayGenStage));
		TEMP_PSO_RT_SHADOW->BindSRV("global", MakeRHITextureSRV("RayNoiseBlueNoiseSource", 8, rayGenStage));
		// ReSTIR Phase 2 temporal reuse — bound even in Option A mode so
		// the root signature stays uniform across mode toggles; the
		// shader only reads from these when ShadowMode == 1.
		TEMP_PSO_RT_SHADOW->BindSRV("global", MakeRHITextureSRV("ShadowReservoirPrev", 9, rayGenStage));
		TEMP_PSO_RT_SHADOW->BindSRV("global", MakeRHITextureSRV("VelocityTex", 10, rayGenStage));
		// Phase 2b — per-pixel M tracking. Separate single-channel buffer.
		TEMP_PSO_RT_SHADOW->BindUAV("global", MakeRHITextureUAV("ShadowReservoirM", 1, rayGenStage));
		TEMP_PSO_RT_SHADOW->BindSRV("global", MakeRHITextureSRV("ShadowReservoirMPrev", 11, rayGenStage));
		// Disocclusion test inputs for Option C (temporal/spatial reuse
		// rejection). Previous-frame depth + normal so Phase 2 can
		// reject the temporal sample when the reprojected surface
		// differs from current (motion-vector lag, dynamic geometry,
		// shadow caster motion).
		TEMP_PSO_RT_SHADOW->BindSRV("global", MakeRHITextureSRV("DepthTexPrev", 12, rayGenStage));
		TEMP_PSO_RT_SHADOW->BindSRV("global", MakeRHITextureSRV("WorldNormalTexPrev", 13, rayGenStage));
		TEMP_PSO_RT_SHADOW->BindSRV("global", MakeRHIBufferSRV("SpatialLightCellKeys", 14, rayGenStage, RHIBufferViewKind::Raw));
		TEMP_PSO_RT_SHADOW->BindSRV("global", MakeRHIBufferSRV("SpatialLightCellMask", 15, rayGenStage, RHIBufferViewKind::Raw));

		TEMP_PSO_RT_SHADOW->BindCBV("global", MakeRHICBV("ViewParameter", 0, sizeof(RTShadowViewParamCB), rayGenStage));
		TEMP_PSO_RT_SHADOW->BindSampler("global", MakeRHISampler("sampleWrap", 0, rayGenStage | anyHitStage));
		BindRTBindlessMaterialSchema(*TEMP_PSO_RT_SHADOW, anyHitStage);
		BindRTBindlessGeometrySchema(*TEMP_PSO_RT_SHADOW, anyHitStage);



		TEMP_PSO_RT_SHADOW->AddShader("miss", RTPipelineStateObject::MISS);
		
		TEMP_PSO_RT_SHADOW->AddShader("anyhit", RTPipelineStateObject::ANYHIT);
		TEMP_PSO_RT_SHADOW->Configure(1, sizeof(float) * 4, sizeof(float) * 2);

		bool bSuccess = TEMP_PSO_RT_SHADOW->InitRS("Shaders\\RaytracedShadow.hlsl");
		if (bSuccess)
		{
			PSO_RT_SHADOW = TEMP_PSO_RT_SHADOW;
		}
}

void Corona::InitShadowSpatialReusePass()
{
	// ReSTIR DI Phase 3 — proper 2-pass spatial reuse. Compiles with
	// cs_6_5 so the shader can RayQuery fresh visibility for the post-
	// spatial chosen light. SRV t-registers must match the .hlsl
	// layout (see RaytracedShadowSpatialReuse.hlsl).
	shared_ptr<ComputePipelineStateObject> tempPSO = renderBackend->CreateComputePipelineStateObject();
	if (!tempPSO)
		return;
	const RHIShaderStageMask computeStage = ToRHIShaderStageMask(RHIShaderStage::Compute);
	tempPSO->BindSRV(MakeRHITextureSRV("PreSpatialReservoir", 0, computeStage));
	tempPSO->BindSRV(MakeRHITextureSRV("PreSpatialM",         1, computeStage));
	tempPSO->BindSRV(MakeRHITextureSRV("DepthTex",            2, computeStage));
	tempPSO->BindSRV(MakeRHITextureSRV("WorldNormalTex",      3, computeStage));
	tempPSO->BindSRV(MakeRHITextureSRV("GeoNormalTex",        4, computeStage));
	tempPSO->BindSRV(MakeRHIAccelerationStructureSRV("gRtScene", 5, computeStage));
	tempPSO->BindUAV(MakeRHITextureUAV("ShadowResult",        0, computeStage));
	tempPSO->BindUAV(MakeRHITextureUAV("ShadowReservoirM",    1, computeStage));
	tempPSO->BindCBV(MakeRHICBV("ViewParameter",       0, sizeof(RTShadowViewParamCB), computeStage));
	tempPSO->BindSampler(MakeRHISampler("sampleWrap",      0, computeStage));
	const bool ok = tempPSO->InitCSWithInlineRT(
		GetAssetFullPath(L"Shaders\\RaytracedShadowSpatialReuse.hlsl"),
		"main");
	if (ok)
		PSO_SHADOW_SPATIAL_REUSE = tempPSO;
}

void Corona::RaytraceShadowPass()
{
	if (!ShadowBuffer || !BlueNoiseTex || !TLAS || !PSO_RT_SHADOW || !UnjitteredDepthBuffers[ColorBufferWriteIndex] || !NormalBuffers[ColorBufferWriteIndex] || !GeomNormalBuffers[ColorBufferWriteIndex])
		return;
	renderBackend->EmitGpuCrashMarker("RaytraceShadowPass");

	if (!EnsureRTMaterialRecordBuffer())
	return;

	RTShadowViewParam.ViewMatrix = glm::transpose(ViewMat);
	RTShadowViewParam.InvViewMatrix = glm::transpose(InvViewMat);
	RTShadowViewParam.ProjMatrix = glm::transpose(UnjitteredProjMat);
	RTShadowViewParam.InvProjMatrix = glm::transpose(UnjitteredInvProjMat);
	RTShadowViewParam.ProjectionParams = FrameProjectionParams;
	RTShadowViewParam.LightDir = glm::vec4(RenderFrameNormalizedLightDir, RenderFrameDirectionalLightCastShadow ? 1.0f : 0.0f);
	RTShadowViewParam.ShadowLightRadius = std::clamp(RTShadowViewParam.ShadowLightRadius, 0.0f, 0.03f);
	RTShadowViewParam.ShadowSampleCount = std::clamp(RTShadowViewParam.ShadowSampleCount, 1u, 16u);
	RTShadowViewParam.FrameCounter = RenderFrameIndex;
	RTShadowViewParam.BlueNoiseOffsetStride = RTGIViewParam.BlueNoiseOffsetStride;
	RTShadowViewParam.NoiseMode = RenderFrameRayNoiseMode;
	// Push the runtime-tunable temporal M cap. Clamp to a sensible
	// range so a slider drag past the rails doesn't produce a
	// degenerate reservoir (M < 1 effectively disables temporal reuse;
	// M > 64 makes the reservoir refuse to forget anything).
	RTShadowViewParam.ShadowMaxM = std::clamp(ReSTIRShadowMaxM, 1.0f, 64.0f);
	const bool bSpatialLightMaskReady =
		bEnableReSTIRDirectShadow &&
		DiffuseGIMode == EDiffuseGIMode::SPATIAL_HASH &&
		SpatialHashLightMaskFrameIndex == RenderFrameIndex &&
		SpatialHashGIResolvedKeys[0] &&
		SpatialHashGICellLightMask;
	RTShadowViewParam.SpatialLightCellSize = SpatialHashGICB.CellSize;
	RTShadowViewParam.SpatialLightHashEntryMask = SpatialHashGIEntryCount - 1u;
	RTShadowViewParam.SpatialLightMaxProbeSteps = SpatialHashGICB.MaxProbeSteps;
	RTShadowViewParam.bUseSpatialLightMask = bSpatialLightMaskReady ? 1u : 0u;
	RTShadowViewParam.SpatialHashLevelParams = SpatialHashGICB.SpatialHashLevelParams;

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

	std::vector<const PointLightState*> pointLightCandidates;
	// ReSTIR shares the spatial light mask with the GI pass, so it MUST use the same
	// stable-Id-ordered table (BuildReSTIRSharedPointLights) for bit/index parity.
	// Option A does its own distance sort over the full candidate set instead.
	if (bEnableReSTIRDirectShadow)
		BuildReSTIRSharedPointLights(pointLightCandidates);
	else
		BuildPointLightRenderCandidates(pointLightCandidates);

	uint32_t shadowedCount = 0;
	if (bEnableReSTIRDirectShadow)
	{
		// MUST mirror the LightingPS feed in `RasterPasses.cpp` exactly
		// (same iteration order over RenderWorld.PointLights, same
		// filter predicates) so the chosen index ReSTIR writes into
		// ShadowBuffer.g is a valid index into LightingPS's
		// PointLights[]. Diverging either filter breaks CB-index
		// parity → shading targets a different light than the one
		// the shadow ray was cast for.
		//
		// Filter:
		//   1. `bEnabled` — same as LightingPS.
		//   2. `Intensity > 0` is OMITTED here even though it'd save a
		//      candidate slot, because LightingPS doesn't filter on it
		//      (see RasterPasses.cpp:1430+). Adding it would shift
		//      indices.
		//   3. Sphere-frustum cull — also applied to LightingPS feed.
		//
		// Consume the table the spatial-hash GI light mask was actually built from
		// (cached by the GI pass last frame) so the shadow candidate index stays
		// bit-aligned with the persistent mask even while the camera moves and the
		// score-based top-N selection shifts. Fall back to the freshly built shared
		// table on the first frame / when the cache is unavailable.
		if (bReSTIRMaskLightCacheValid && !ReSTIRMaskLightCache.empty())
		{
			for (const PointLightParam& p : ReSTIRMaskLightCache)
			{
				if (shadowedCount >= MaxDiffuseGIPointLights)
					break;
				const float radius = std::max(p.PositionAndRadius.w, 0.01f);
				const bool castsShadow = p.SpotConeAndFlags.w > 0.5f;
				const float luma =
					std::max(0.0f, 0.2126f * p.ColorAndIntensity.x + 0.7152f * p.ColorAndIntensity.y + 0.0722f * p.ColorAndIntensity.z) *
					std::max(0.0f, p.ColorAndIntensity.w);
				RTShadowViewParam.ShadowedPointLights[shadowedCount] =
					glm::vec4(glm::vec3(p.PositionAndRadius), radius);
				RTShadowViewParam.ShadowedPointLightWeights[shadowedCount] =
					glm::vec4(castsShadow ? luma : 0.0f, 0.0f, 0.0f, 0.0f);
				++shadowedCount;
			}
		}
		else
		{
			for (const PointLightState* plPtr : pointLightCandidates)
			{
				if (shadowedCount >= MaxDiffuseGIPointLights)
					break;
				if (!plPtr)
					continue;
				const PointLightState& pl = *plPtr;
				const float radius = std::max(pl.Radius, 0.01f);
				RTShadowViewParam.ShadowedPointLights[shadowedCount] =
					glm::vec4(pl.Position, radius);
				RTShadowViewParam.ShadowedPointLightWeights[shadowedCount] =
					glm::vec4(pl.bCastShadow ? computeLuma(pl) : 0.0f, 0.0f, 0.0f, 0.0f);
				++shadowedCount;
			}
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
		const uint32_t searchCount = std::min<uint32_t>(static_cast<uint32_t>(pointLightCandidates.size()), MaxPointLights);
		candidates.reserve(searchCount);
		const glm::vec3 camPos = RenderFrameCameraPosition;
		for (uint32_t i = 0; i < searchCount; ++i)
		{
			const PointLightState* plPtr = pointLightCandidates[i];
			if (!plPtr)
				continue;
			const PointLightState& pl = *plPtr;
			if (!pl.bCastShadow)
				continue;
			const glm::vec3 toLight = pl.Position - camPos;
			candidates.push_back({ i, glm::dot(toLight, toLight) });
		}
		std::sort(candidates.begin(), candidates.end(),
			[](const Candidate& a, const Candidate& b) { return a.DistSq < b.DistSq; });
		const uint32_t pick = std::min<uint32_t>(static_cast<uint32_t>(candidates.size()), 3u);
		for (uint32_t k = 0; k < pick; ++k)
		{
			const PointLightState& pl = *pointLightCandidates[candidates[k].Index];
			RTShadowViewParam.ShadowedPointLights[k] =
				glm::vec4(pl.Position, std::max(pl.Radius, 0.01f));
			RTShadowViewParam.ShadowedPointLightWeights[k] =
				glm::vec4(computeLuma(pl), 0.0f, 0.0f, 0.0f);
		}
		shadowedCount = pick;
	}

	for (uint32_t i = shadowedCount; i < MaxDiffuseGIPointLights; ++i)
	{
		RTShadowViewParam.ShadowedPointLights[i] = glm::vec4(0.0f);
		RTShadowViewParam.ShadowedPointLightWeights[i] = glm::vec4(0.0f);
	}
	RTShadowViewParam.ShadowedPointLightCount = shadowedCount;

	// Phase 3 proper 2-pass:
	//   raygen  writes  ShadowBufferPreSpatial + ShadowReservoirMBufferPreSpatial
	//   compute reads   those, writes the final ShadowBuffer + ShadowReservoirMBuffer
	// When the proper 2-pass PSO isn't available (e.g. on hardware
	// without DXR Tier 1.1, or Vulkan path), fall back to writing
	// directly into the final ShadowBuffer.
	//
	// NOTE: currently OFF by default (bEnableShadowSpatialReuseCompute
	// = false) because the simple M-weighted RIS combine in the
	// compute shader produces an over-brightness bias under sponza —
	// proper balance-heuristic MIS is the follow-up. The
	// infrastructure (PreSpatial buffers, compute PSO with inline RT,
	// cs_6_5 compile path) stays in place so re-enabling once MIS is
	// implemented is a one-flag flip.
	const bool bUseSpatialReuseCompute =
		bEnableShadowSpatialReuseCompute &&
		bEnableReSTIRDirectShadow &&
		PSO_SHADOW_SPATIAL_REUSE != nullptr &&
		ShadowBufferPreSpatial != nullptr &&
		ShadowReservoirMBufferPreSpatial != nullptr;
	Texture* const raygenShadowTarget = bUseSpatialReuseCompute ? ShadowBufferPreSpatial.get() : ShadowBuffer.get();
	Texture* const raygenMTarget      = bUseSpatialReuseCompute ? ShadowReservoirMBufferPreSpatial.get() : ShadowReservoirMBuffer.get();

	renderBackend->TransitionTexture(raygenShadowTarget, EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	if (raygenMTarget)
		renderBackend->TransitionTexture(raygenMTarget, EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	RTPassBuilder pass(*this, PSO_RT_SHADOW);
	pass.BeginScene()
		.SetTextureUAV("global", "ShadowResult", raygenShadowTarget)
		.SetTextureUAV("global", "ShadowReservoirM", raygenMTarget)
		.SetAccelerationStructure("global", "gRtScene", TLAS)
		.SetTextureSRV("global", "DepthTex", UnjitteredDepthBuffers[ColorBufferWriteIndex].get())
		.SetTextureSRV("global", "WorldNormalTex", NormalBuffers[ColorBufferWriteIndex].get())
		.SetTextureSRV("global", "GeoNormalTex", GeomNormalBuffers[ColorBufferWriteIndex].get())
		.SetTextureSRV("global", "RayNoiseBlueNoiseSource", BlueNoiseTex.get())
		.SetTextureSRV("global", "ShadowReservoirPrev",
			ShadowReservoirPrevBuffer ? ShadowReservoirPrevBuffer.get() : ShadowBuffer.get())
		.SetTextureSRV("global", "ShadowReservoirMPrev",
			ShadowReservoirMPrevBuffer ? ShadowReservoirMPrevBuffer.get() : ShadowBuffer.get())
		.SetTextureSRV("global", "DepthTexPrev",
			UnjitteredDepthBuffers[1 - ColorBufferWriteIndex] ? UnjitteredDepthBuffers[1 - ColorBufferWriteIndex].get() : UnjitteredDepthBuffers[ColorBufferWriteIndex].get())
		.SetTextureSRV("global", "WorldNormalTexPrev",
			NormalBuffers[1 - ColorBufferWriteIndex] ? NormalBuffers[1 - ColorBufferWriteIndex].get() : NormalBuffers[ColorBufferWriteIndex].get())
		.SetBufferSRV("global", "SpatialLightCellKeys", SpatialHashGIResolvedKeys[0].get())
		.SetBufferSRV("global", "SpatialLightCellMask", SpatialHashGICellLightMask.get())
		.SetTextureSRV("global", "VelocityTex", VelocityBuffer.get())
		.SetCBVValue("global", "ViewParameter", &RTShadowViewParam)
		.SetSampler("global", "sampleWrap", samplerWrap.get());
	pass.SetBindlessTextureTable("global", "MaterialTextures")
		.SetBufferSRV("global", "RtMaterials", RTMaterialRecordBuffer.get());
	RTSceneHitProgramDesc hitProgramDesc;
	pass.BindSceneHitPrograms(hitProgramDesc);
	pass.Dispatch(GetRenderWidth(), GetRenderHeight());

	renderBackend->TransitionTexture(raygenShadowTarget, EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	if (raygenMTarget)
		renderBackend->TransitionTexture(raygenMTarget, EResourceState::UnorderedAccess, EResourceState::ShaderRead);

	// --- Phase 3 spatial reuse compute pass ---
	if (bUseSpatialReuseCompute)
	{
		renderBackend->EmitGpuCrashMarker("ShadowSpatialReusePass");
		renderBackend->TransitionTexture(ShadowBuffer.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->TransitionTexture(ShadowReservoirMBuffer.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

		PSO_SHADOW_SPATIAL_REUSE->SetTextureSRV("PreSpatialReservoir", ShadowBufferPreSpatial.get());
		PSO_SHADOW_SPATIAL_REUSE->SetTextureSRV("PreSpatialM",         ShadowReservoirMBufferPreSpatial.get());
		PSO_SHADOW_SPATIAL_REUSE->SetTextureSRV("DepthTex",            UnjitteredDepthBuffers[ColorBufferWriteIndex].get());
		PSO_SHADOW_SPATIAL_REUSE->SetTextureSRV("WorldNormalTex",      NormalBuffers[ColorBufferWriteIndex].get());
		PSO_SHADOW_SPATIAL_REUSE->SetTextureSRV("GeoNormalTex",        GeomNormalBuffers[ColorBufferWriteIndex].get());
		PSO_SHADOW_SPATIAL_REUSE->SetAccelerationStructure("gRtScene", TLAS);
		PSO_SHADOW_SPATIAL_REUSE->SetTextureUAV("ShadowResult",        ShadowBuffer.get());
		PSO_SHADOW_SPATIAL_REUSE->SetTextureUAV("ShadowReservoirM",    ShadowReservoirMBuffer.get());
		PSO_SHADOW_SPATIAL_REUSE->SetCBVValue("ViewParameter",         &RTShadowViewParam);
		PSO_SHADOW_SPATIAL_REUSE->SetSampler("sampleWrap",             samplerWrap.get());
		PSO_SHADOW_SPATIAL_REUSE->Apply();
		const UINT groupX = (GetRenderWidth() + 7) / 8;
		const UINT groupY = (GetRenderHeight() + 7) / 8;
		dx12_rhi->GetGraphicsCommandList()->Dispatch(groupX, groupY, 1);

		renderBackend->TransitionTexture(ShadowBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
		renderBackend->TransitionTexture(ShadowReservoirMBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	}

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
