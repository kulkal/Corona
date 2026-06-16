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
		ShadowReservoirMBufferPreSpatial != nullptr &&
		ShadowReservoirMBuffer != nullptr &&
		dx12_rhi != nullptr;
	Texture* const raygenShadowTarget = bUseSpatialReuseCompute ? ShadowBufferPreSpatial.get() : ShadowBuffer.get();
	Texture* const raygenMTarget      = bUseSpatialReuseCompute ? ShadowReservoirMBufferPreSpatial.get() : ShadowReservoirMBuffer.get();

	Texture* const depthTexture = UnjitteredDepthBuffers[ColorBufferWriteIndex].get();
	Texture* const normalTexture = NormalBuffers[ColorBufferWriteIndex].get();
	Texture* const geomNormalTexture = GeomNormalBuffers[ColorBufferWriteIndex].get();
	Texture* const prevDepthTexture = UnjitteredDepthBuffers[1 - ColorBufferWriteIndex]
		? UnjitteredDepthBuffers[1 - ColorBufferWriteIndex].get()
		: depthTexture;
	Texture* const prevNormalTexture = NormalBuffers[1 - ColorBufferWriteIndex]
		? NormalBuffers[1 - ColorBufferWriteIndex].get()
		: normalTexture;
	Buffer* const spatialLightCellKeys = SpatialHashGIResolvedKeys[0].get();
	Buffer* const spatialLightCellMask = SpatialHashGICellLightMask.get();

	RenderGraph rg(renderBackend.get());
	RGTextureRef raygenShadowOutput = rg.ImportTexture("Shadow.RaygenReservoir", raygenShadowTarget, EResourceState::ShaderRead);
	RGTextureRef raygenMOutput = raygenMTarget
		? rg.ImportTexture("Shadow.RaygenReservoirM", raygenMTarget, EResourceState::ShaderRead)
		: RGTextureRef{};
	RGTextureRef finalShadowOutput = bUseSpatialReuseCompute
		? rg.ImportTexture("Shadow.Reservoir", ShadowBuffer.get(), EResourceState::ShaderRead)
		: raygenShadowOutput;
	RGTextureRef finalMOutput = bUseSpatialReuseCompute
		? rg.ImportTexture("Shadow.ReservoirM", ShadowReservoirMBuffer.get(), EResourceState::ShaderRead)
		: raygenMOutput;
	RGTextureRef depthInput = rg.ImportTexture("Shadow.Depth", depthTexture, EResourceState::ShaderRead);
	RGTextureRef normalInput = rg.ImportTexture("Shadow.WorldNormal", normalTexture, EResourceState::ShaderRead);
	RGTextureRef geomNormalInput = rg.ImportTexture("Shadow.GeoNormal", geomNormalTexture, EResourceState::ShaderRead);
	RGTextureRef blueNoiseInput = rg.ImportTexture("Shadow.BlueNoise", BlueNoiseTex.get(), EResourceState::ShaderRead);
	RGTextureRef prevDepthInput = prevDepthTexture == depthTexture
		? depthInput
		: rg.ImportTexture("Shadow.PrevDepth", prevDepthTexture, EResourceState::ShaderRead);
	RGTextureRef prevNormalInput = prevNormalTexture == normalTexture
		? normalInput
		: rg.ImportTexture("Shadow.PrevWorldNormal", prevNormalTexture, EResourceState::ShaderRead);
	RGTextureRef shadowPrevInput = ShadowReservoirPrevBuffer
		? rg.ImportTexture("Shadow.PrevReservoir", ShadowReservoirPrevBuffer.get(), EResourceState::ShaderRead)
		: RGTextureRef{};
	RGTextureRef shadowMPrevInput = ShadowReservoirMPrevBuffer
		? rg.ImportTexture("Shadow.PrevReservoirM", ShadowReservoirMPrevBuffer.get(), EResourceState::ShaderRead)
		: RGTextureRef{};
	RGTextureRef velocityInput = VelocityBuffer
		? rg.ImportTexture("Shadow.Velocity", VelocityBuffer.get(), EResourceState::ShaderRead)
		: RGTextureRef{};
	RGBufferRef spatialLightCellKeysInput = spatialLightCellKeys
		? rg.ImportBuffer("Shadow.SpatialLightCellKeys", spatialLightCellKeys, EResourceState::ShaderRead)
		: RGBufferRef{};
	RGBufferRef spatialLightCellMaskInput = spatialLightCellMask
		? rg.ImportBuffer("Shadow.SpatialLightCellMask", spatialLightCellMask, EResourceState::ShaderRead)
		: RGBufferRef{};
	RGBufferRef rtMaterialsInput = rg.ImportBuffer("Shadow.RtMaterials", RTMaterialRecordBuffer.get(), EResourceState::ShaderRead);

	rg.ExportTexture(finalShadowOutput, EResourceState::ShaderRead);
	if (finalMOutput.IsValid())
		rg.ExportTexture(finalMOutput, EResourceState::ShaderRead);
	if (shadowPrevInput.IsValid())
		rg.ExportTexture(shadowPrevInput, EResourceState::ShaderRead);
	if (shadowMPrevInput.IsValid())
		rg.ExportTexture(shadowMPrevInput, EResourceState::ShaderRead);

	rg.AddPass(
		"RaytraceShadowPass",
		ERGPassFlags::RayTracing,
		[&](RGPassBuilder& builder)
		{
			builder.ReadWriteTexture(raygenShadowOutput, EResourceState::UnorderedAccess);
			if (raygenMOutput.IsValid())
				builder.ReadWriteTexture(raygenMOutput, EResourceState::UnorderedAccess);
			builder.ReadTexture(depthInput, EResourceState::ShaderRead)
				.ReadTexture(normalInput, EResourceState::ShaderRead)
				.ReadTexture(geomNormalInput, EResourceState::ShaderRead)
				.ReadTexture(blueNoiseInput, EResourceState::ShaderRead);
			if (prevDepthInput.Index != depthInput.Index)
				builder.ReadTexture(prevDepthInput, EResourceState::ShaderRead);
			if (prevNormalInput.Index != normalInput.Index)
				builder.ReadTexture(prevNormalInput, EResourceState::ShaderRead);
			if (shadowPrevInput.IsValid())
				builder.ReadTexture(shadowPrevInput, EResourceState::ShaderRead);
			if (shadowMPrevInput.IsValid())
				builder.ReadTexture(shadowMPrevInput, EResourceState::ShaderRead);
			if (bUseSpatialReuseCompute && (!shadowPrevInput.IsValid() || !shadowMPrevInput.IsValid()))
				builder.ReadTexture(finalShadowOutput, EResourceState::ShaderRead);
			if (velocityInput.IsValid())
				builder.ReadTexture(velocityInput, EResourceState::ShaderRead);
			if (spatialLightCellKeysInput.IsValid())
				builder.ReadBuffer(spatialLightCellKeysInput, EResourceState::ShaderRead);
			if (spatialLightCellMaskInput.IsValid())
				builder.ReadBuffer(spatialLightCellMaskInput, EResourceState::ShaderRead);
			builder.ReadBuffer(rtMaterialsInput, EResourceState::ShaderRead);
		},
		[&](RGContext& ctx)
		{
			Texture* const shadowPrevTexture = shadowPrevInput.IsValid()
				? ctx.GetTexture(shadowPrevInput)
				: ShadowBuffer.get();
			Texture* const shadowMPrevTexture = shadowMPrevInput.IsValid()
				? ctx.GetTexture(shadowMPrevInput)
				: ShadowBuffer.get();

			RTPassBuilder pass(*this, PSO_RT_SHADOW);
			pass.BeginScene()
				.SetTextureUAV("global", "ShadowResult", ctx.GetTexture(raygenShadowOutput))
				.SetTextureUAV("global", "ShadowReservoirM", raygenMOutput.IsValid() ? ctx.GetTexture(raygenMOutput) : nullptr)
				.SetAccelerationStructure("global", "gRtScene", TLAS)
				.SetTextureSRV("global", "DepthTex", ctx.GetTexture(depthInput))
				.SetTextureSRV("global", "WorldNormalTex", ctx.GetTexture(normalInput))
				.SetTextureSRV("global", "GeoNormalTex", ctx.GetTexture(geomNormalInput))
				.SetTextureSRV("global", "RayNoiseBlueNoiseSource", ctx.GetTexture(blueNoiseInput))
				.SetTextureSRV("global", "ShadowReservoirPrev", shadowPrevTexture)
				.SetTextureSRV("global", "ShadowReservoirMPrev", shadowMPrevTexture)
				.SetTextureSRV("global", "DepthTexPrev", prevDepthInput.Index == depthInput.Index ? ctx.GetTexture(depthInput) : ctx.GetTexture(prevDepthInput))
				.SetTextureSRV("global", "WorldNormalTexPrev", prevNormalInput.Index == normalInput.Index ? ctx.GetTexture(normalInput) : ctx.GetTexture(prevNormalInput))
				.SetBufferSRV("global", "SpatialLightCellKeys", spatialLightCellKeysInput.IsValid() ? ctx.GetBuffer(spatialLightCellKeysInput) : nullptr)
				.SetBufferSRV("global", "SpatialLightCellMask", spatialLightCellMaskInput.IsValid() ? ctx.GetBuffer(spatialLightCellMaskInput) : nullptr)
				.SetTextureSRV("global", "VelocityTex", velocityInput.IsValid() ? ctx.GetTexture(velocityInput) : nullptr)
				.SetCBVValue("global", "ViewParameter", &RTShadowViewParam)
				.SetSampler("global", "sampleWrap", samplerWrap.get());
			pass.SetBindlessTextureTable("global", "MaterialTextures")
				.SetBufferSRV("global", "RtMaterials", ctx.GetBuffer(rtMaterialsInput));
			RTSceneHitProgramDesc hitProgramDesc;
			pass.BindSceneHitPrograms(hitProgramDesc);
			pass.Dispatch(GetRenderWidth(), GetRenderHeight());
		});

	// --- Phase 3 spatial reuse compute pass ---
	if (bUseSpatialReuseCompute)
	{
		rg.AddPass(
			"ShadowSpatialReusePass",
			ERGPassFlags::Compute,
			[&](RGPassBuilder& builder)
			{
				builder.ReadTexture(raygenShadowOutput, EResourceState::ShaderRead)
					.ReadTexture(raygenMOutput, EResourceState::ShaderRead)
					.ReadTexture(depthInput, EResourceState::ShaderRead)
					.ReadTexture(normalInput, EResourceState::ShaderRead)
					.ReadTexture(geomNormalInput, EResourceState::ShaderRead)
					.ReadWriteTexture(finalShadowOutput, EResourceState::UnorderedAccess)
					.ReadWriteTexture(finalMOutput, EResourceState::UnorderedAccess);
			},
			[&](RGContext& ctx)
			{
				PSO_SHADOW_SPATIAL_REUSE->SetTextureSRV("PreSpatialReservoir", ctx.GetTexture(raygenShadowOutput));
				PSO_SHADOW_SPATIAL_REUSE->SetTextureSRV("PreSpatialM",         ctx.GetTexture(raygenMOutput));
				PSO_SHADOW_SPATIAL_REUSE->SetTextureSRV("DepthTex",            ctx.GetTexture(depthInput));
				PSO_SHADOW_SPATIAL_REUSE->SetTextureSRV("WorldNormalTex",      ctx.GetTexture(normalInput));
				PSO_SHADOW_SPATIAL_REUSE->SetTextureSRV("GeoNormalTex",        ctx.GetTexture(geomNormalInput));
				PSO_SHADOW_SPATIAL_REUSE->SetAccelerationStructure("gRtScene", TLAS);
				PSO_SHADOW_SPATIAL_REUSE->SetTextureUAV("ShadowResult",        ctx.GetTexture(finalShadowOutput));
				PSO_SHADOW_SPATIAL_REUSE->SetTextureUAV("ShadowReservoirM",    ctx.GetTexture(finalMOutput));
				PSO_SHADOW_SPATIAL_REUSE->SetCBVValue("ViewParameter",         &RTShadowViewParam);
				PSO_SHADOW_SPATIAL_REUSE->SetSampler("sampleWrap",             samplerWrap.get());
				PSO_SHADOW_SPATIAL_REUSE->Apply();
				const UINT groupX = (GetRenderWidth() + 7) / 8;
				const UINT groupY = (GetRenderHeight() + 7) / 8;
				dx12_rhi->GetGraphicsCommandList()->Dispatch(groupX, groupY, 1);
			});
	}

	// ReSTIR Phase 2 temporal feedback: cache this frame's reservoirs for
	// next-frame reproject via raw DX12 CopyResource. Only meaningful in
	// ReSTIR mode; in Option A the copy is skipped. DX12-only for now —
	// Vulkan path will follow with a backend-abstracted texture copy.
	if (bEnableReSTIRDirectShadow && shadowPrevInput.IsValid() && dx12_rhi)
	{
		rg.AddPass(
			"Shadow.CopyReservoir",
			ERGPassFlags::Copy,
			[&](RGPassBuilder& builder)
			{
				builder.ReadTexture(finalShadowOutput, EResourceState::CopySource)
					.WriteTexture(shadowPrevInput, EResourceState::CopyDest);
			},
			[&](RGContext& ctx)
			{
				dx12_rhi->GetGraphicsCommandList()->CopyResource(
					ctx.GetTexture(shadowPrevInput)->resource.Get(),
					ctx.GetTexture(finalShadowOutput)->resource.Get());
			});

		if (finalMOutput.IsValid() && shadowMPrevInput.IsValid())
		{
			rg.AddPass(
				"Shadow.CopyReservoirM",
				ERGPassFlags::Copy,
				[&](RGPassBuilder& builder)
				{
					builder.ReadTexture(finalMOutput, EResourceState::CopySource)
						.WriteTexture(shadowMPrevInput, EResourceState::CopyDest);
				},
				[&](RGContext& ctx)
				{
					dx12_rhi->GetGraphicsCommandList()->CopyResource(
						ctx.GetTexture(shadowMPrevInput)->resource.Get(),
						ctx.GetTexture(finalMOutput)->resource.Get());
				});
		}
	}

	if (!rg.Execute())
		return;

	bShadowOutputValidThisFrame = true;
}
