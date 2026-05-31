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
#include "ParticleSystem.h"
#include "TerrainComponent.h"
#include "VulkanBackend.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <limits>

#include "glm/gtc/matrix_transform.hpp"

void AppendCpuRuntimeTrace(const std::wstring& line);

void Corona::InitBloomPass()
{
	auto createPSO = [&](const wchar_t* shaderFile, const std::string& entryPoint,
		std::initializer_list<std::tuple<const char*, uint32_t, uint32_t>> srvBindings,
		std::initializer_list<std::tuple<const char*, uint32_t>> uavBindings,
		std::initializer_list<std::tuple<const char*, uint32_t>> samplerBindings,
		std::initializer_list<std::tuple<const char*, uint32_t, uint32_t>> cbvBindings)
		-> std::shared_ptr<ComputePipelineStateObject>
	{
		auto pso = renderBackend->CreateComputePipelineStateObject();
		if (!pso) return nullptr;
		for (const auto& b : srvBindings) pso->BindSRV(std::get<0>(b), std::get<1>(b), std::get<2>(b));
		for (const auto& b : uavBindings) pso->BindUAV(std::get<0>(b), std::get<1>(b));
		for (const auto& b : samplerBindings) pso->BindSampler(std::get<0>(b), std::get<1>(b));
		for (const auto& b : cbvBindings) pso->BindCBV(std::get<0>(b), std::get<1>(b), std::get<2>(b));
		if (!pso->InitCS(GetAssetFullPath(shaderFile), entryPoint))
			return nullptr;
		return pso;
	};

	if (auto pso = createPSO(L"Shaders\\BloomBlur.hlsl", "BloomExtract",
		{ {"SrcTex", 0, 1}, {"Exposure", 1, 1} },
		{ {"DstTex", 0}, {"LumaResult", 1} },
		{ {"samplerWrap", 0} },
		{ {"BloomCB", 0, sizeof(BloomCB)} }))
		BloomExtractPSO = pso;

	if (auto pso = createPSO(L"Shaders\\BloomBlur.hlsl", "BloomBlur",
		{ {"SrcTex", 0, 1} },
		{ {"DstTex", 0} },
		{ {"samplerWrap", 0} },
		{ {"BloomCB", 0, sizeof(BloomCB)} }))
		BloomBlurPSO = pso;

	if (auto pso = createPSO(L"Shaders\\Histogram.hlsl", "GenerateHistogram",
		{ {"LumaTex", 0, 1} },
		{ {"Histogram", 0} },
		{},
		{}))
		HistogramPSO = pso;

	if (auto pso = createPSO(L"Shaders\\DrawHistogram.hlsl", "DrawHistogram",
		{ {"Histogram", 0, 1}, {"Exposure", 1, 1} },
		{ {"ColorBuffer", 0} },
		{},
		{}))
		DrawHistogramPSO = pso;

	if (auto pso = createPSO(L"Shaders\\Histogram.hlsl", "ClearHistogram",
		{},
		{ {"Histogram", 0} },
		{},
		{}))
		ClearHistogramPSO = pso;

	if (auto pso = createPSO(L"Shaders\\AdaptExposureCS.hlsl", "AdaptExposure",
		{ {"Histogram", 0, 1} },
		{ {"Exposure", 0} },
		{},
		{ {"AdaptExposureCB", 0, sizeof(AdaptExposureCB)} }))
		AdapteExposurePSO = pso;

	BloomBlurPingPong[0] = renderBackend->CreateTexture2D({ ETextureFormat::RGBA16Float, TextureUsage_UnorderedAccess, EInitialResourceState::ShaderRead, (int)BloomBufferWidth, (int)BloomBufferHeight, 1, std::nullopt });
	NAME_D3D12_OBJECT(BloomBlurPingPong[0]->resource);

	BloomBlurPingPong[1] = renderBackend->CreateTexture2D({ ETextureFormat::RGBA16Float, TextureUsage_UnorderedAccess, EInitialResourceState::ShaderRead, (int)BloomBufferWidth, (int)BloomBufferHeight, 1, std::nullopt });
	NAME_D3D12_OBJECT(BloomBlurPingPong[1]->resource);

	LumaBuffer = renderBackend->CreateTexture2D({ ETextureFormat::R8Uint, TextureUsage_UnorderedAccess, EInitialResourceState::ShaderRead, (int)BloomBufferWidth, (int)BloomBufferHeight, 1, std::nullopt });
	NAME_D3D12_OBJECT(LumaBuffer->resource);

	Histogram = renderBackend->CreateBuffer({ 256u, sizeof(UINT32), EInitialResourceState::ShaderRead, true, nullptr, EBufferShape::ByteAddress });
	NAME_D3D12_OBJECT(Histogram->resource);

	__declspec(align(16)) float initExposure[] =
	{
		Exposure,
		1.0f / Exposure,
		0.01,
		Exposure,
		0.0f,
		kInitialMinLog,
		kInitialMaxLog,
		kInitialMaxLog - kInitialMinLog,
		1.0f / (kInitialMaxLog - kInitialMinLog)
	};

	ExposureData = renderBackend->CreateBuffer({ 8u, sizeof(float), EInitialResourceState::ShaderRead, true, initExposure, EBufferShape::Structured });
	NAME_D3D12_OBJECT(ExposureData->resource);
}

void Corona::InitGBufferPass()
{
	GraphicsPipelineDesc desc{};
	desc.ShaderPath = GetAssetFullPath(CORONA_PLATFORM_MOBILE ? L"Shaders\\GBufferMobile.hlsl" : L"Shaders\\GBuffer.hlsl");
	desc.VertexEntryPoint = "VSMain";
	desc.PixelEntryPoint = "PSMain";
	// Default mesh layout (44 B legacy "Vertex"): POSITION float3 @ 0,
	// NORMAL float3 @ 12, UV float2 @ 24, TANGENT float3 @ 32. This is
	// what CreateMeshFromVertices and the platformer scene meshes upload.
	// Skeletal meshes use a different 48 B StandardVertex layout; their
	// pipelines override this stride + offsets explicitly. Vulkan reads
	// the stride from the PSO, so getting the default wrong slides every
	// non-skeletal vertex 4 B on mobile.
	desc.VertexElements = {
		{ "POSITION", 0, EVertexAttributeFormat::Float3, 0 },
		{ "NORMAL",   0, EVertexAttributeFormat::Float3, 12 },
		{ "TEXCOORD", 0, EVertexAttributeFormat::Float2, 24 },
		{ "TANGENT",  0, EVertexAttributeFormat::Float3, 32 },
	};
	desc.TextureBindings = {
		{ "AlbedoTex", 0 },
		{ "NormalTex", 1 },
		{ "RoughnessTex", 2 },
		{ "MetallicTex", 3 },
	};
	desc.SamplerBindings = {
		{ "samplerWrap", 0 },
	};
	desc.VertexStride = 44;
	if (CORONA_PLATFORM_MOBILE)
	{
		desc.ColorFormats = {
			ETextureFormat::RGBA8Unorm,
			ETextureFormat::RGBA8Unorm,
			ETextureFormat::RG16Float,
			ETextureFormat::RGBA8Unorm,
		};
	}
	else
	{
		desc.ColorFormats = {
			ETextureFormat::RGBA8Unorm,
			ETextureFormat::RGBA8Unorm,
			ETextureFormat::RGBA16Float,
			ETextureFormat::RGBA16Float,
			ETextureFormat::RG16Float,
			ETextureFormat::RGBA8Unorm,
			ETextureFormat::R32Float,
		};
	}
	desc.DepthFormat = ETextureFormat::D32Float;
	desc.bDepthEnable = true;
	desc.bCullBackFaces = false;
	desc.ConstantBufferSize = sizeof(GBufferConstantBuffer);
	desc.ConstantBufferBinding = 0;

	GBufferGraphicsPipeline = renderBackend->CreateGraphicsPipeline(desc);

	// Procedural grass PSO — no IA stream; the VS reads SV_VertexID +
	// SV_InstanceID and the per-mesh params (PG_*) from b0. Sharing the
	// GBuffer CB lets DrawScene's existing CB fill code populate the
	// world/view matrices in one place.
	{
		GraphicsPipelineDesc pgDesc = desc;
		pgDesc.ShaderPath = GetAssetFullPath(L"Shaders\\GrassProcedural.hlsl");
		pgDesc.VertexEntryPoint = "VSMain";
		pgDesc.PixelEntryPoint  = "PSMain";
		pgDesc.VertexElements.clear();
		pgDesc.VertexStride = 0;
		pgDesc.TextureBindings.clear();
		pgDesc.SamplerBindings.clear();
		// TerrainHeights at t4 (matches binding slot used by SpineVertices
		// in legacy GBuffer; safe to reuse since procedural has no Spine).
		pgDesc.BufferBindings = {
			{ "TerrainHeights", 4 },
		};
		ProceduralGrassGraphicsPipeline = renderBackend->CreateGraphicsPipeline(pgDesc);
		if (!ProceduralGrassGraphicsPipeline)
			AppendCpuRuntimeTrace(L"[InitGBufferPass] failed to create ProceduralGrass pipeline");

		// Shared sequential IB (0,1,2,…) — sized for the maximum supported
		// blade tessellation (32 segments × 6 = 192 indices). Created once,
		// reused by every procedural-grass draw.
		if (!ProceduralGrassSequentialIb)
		{
			constexpr uint32_t kMaxSegments = 32u;
			constexpr uint32_t kMaxIndices  = kMaxSegments * 6u;
			std::vector<uint32_t> indices(kMaxIndices);
			for (uint32_t i = 0; i < kMaxIndices; ++i)
				indices[i] = i;
			ProceduralGrassSequentialIb = renderBackend->CreateIndexBuffer(
				EIndexFormat::U32,
				static_cast<uint32_t>(indices.size() * sizeof(uint32_t)),
				indices.data());
		}
	}

	// CPU Spine path uses `mesh->Vb` filled with SpineSampleVertex (44 B:
	// POSITION @ 0, NORMAL @ 12, UV @ 24, TANGENT @ 32). The base desc
	// declares the 48 B StandardVertex layout for Skeletal — that stride
	// would slip every Spine vertex 4 B on Vulkan (PSO stride wins).
	// Override here so the CPU-skinned VB is fetched correctly.
	GraphicsPipelineDesc cpuSpineDesc = desc;
	cpuSpineDesc.bDepthWriteEnable = false;
	cpuSpineDesc.VertexStride = 44;
	cpuSpineDesc.VertexElements = {
		{ "POSITION", 0, EVertexAttributeFormat::Float3, 0 },
		{ "NORMAL",   0, EVertexAttributeFormat::Float3, 12 },
		{ "TEXCOORD", 0, EVertexAttributeFormat::Float2, 24 },
		{ "TANGENT",  0, EVertexAttributeFormat::Float3, 32 },
	};
	CpuSpineGBufferGraphicsPipeline = renderBackend->CreateGraphicsPipeline(cpuSpineDesc);
	if (!CpuSpineGBufferGraphicsPipeline)
		AppendCpuRuntimeTrace(L"[InitGBufferPass] failed to create CPU Spine GBuffer pipeline");

	GraphicsPipelineDesc spineDesc = desc;
	spineDesc.VertexEntryPoint = "SpineVSMain";
	spineDesc.VertexElements.clear();
	spineDesc.VertexStride = 0;
	spineDesc.bDepthWriteEnable = false;
	spineDesc.BufferBindings = {
		{ "SpineVertices", 4 },
	};
	SpineGBufferGraphicsPipeline = renderBackend->CreateGraphicsPipeline(spineDesc);
	if (!SpineGBufferGraphicsPipeline)
		AppendCpuRuntimeTrace(L"[InitGBufferPass] failed to create Spine GBuffer pipeline");

	// Spine VS-inline PSO: same vertex-fetch-via-SBV layout as
	// SpineGBufferGraphicsPipeline but the VS skins from
	// SpineVsInlineInputVertices/Influences/Bones instead of reading
	// pre-skinned vertices. Desktop and mobile both expose this entry
	// (GBuffer.hlsl + GBufferMobile.hlsl) -- on mobile this replaces the
	// compute pre-pass entirely. try/catch so missing-entry failures
	// degrade gracefully.
	{
		GraphicsPipelineDesc spineVsInlineDesc = spineDesc;
		spineVsInlineDesc.VertexEntryPoint = "SpineVsInlineVSMain";
		spineVsInlineDesc.BufferBindings = {
			{ "SpineVsInlineInputVertices", 9 },
			{ "SpineVsInlineInfluences", 10 },
			{ "SpineVsInlineBones", 11 },
		};
		try
		{
			SpineVsInlineGBufferGraphicsPipeline = renderBackend->CreateGraphicsPipeline(spineVsInlineDesc);
		}
		catch (const std::exception& ex)
		{
			AppendCpuRuntimeTrace(L"[InitGBufferPass] SpineVsInline pipeline create exception");
			(void)ex;
		}
		if (!SpineVsInlineGBufferGraphicsPipeline)
			AppendCpuRuntimeTrace(L"[InitGBufferPass] failed to create Spine VS-inline GBuffer pipeline");
	}

	// Phase 11 (revised): Skeletal GBuffer PSO. Same IA layout as the
	// standard GBufferGraphicsPipeline, but the VS variant re-skins the
	// bind-pose vertex with the previous frame's bone palette so velocity
	// reflects per-vertex skinning motion, not just camera/world delta.
	// Two SBVs:
	//   t5 = SkeletalInputs    (bind position + bone indices/weights)
	//   t6 = SkeletalPrevBones (previous frame's mat3x4 per bone)
	GraphicsPipelineDesc skeletalDesc = desc;
	skeletalDesc.VertexEntryPoint = "SkeletalVSMain";
	skeletalDesc.BufferBindings = {
		{ "SkeletalInputs", 5 },
		{ "SkeletalPrevBones", 6 },
	};
	// Skeletal output / bind-pose VBs use the StandardVertex layout
	// (POSITION float4 @ 0, TEXCOORD @ 16, NORMAL @ 24, TANGENT @ 36,
	// stride 48), not the standard 44-byte sponza layout. Vulkan reads
	// stride from the PSO binding description, so we have to override
	// here or every vertex slips by 4 bytes. Note: Vulkan assigns
	// VkVertexInputAttributeDescription location by index in this
	// vector — keep the order matching VSInput in GBuffer.hlsl
	// (POSITION / NORMAL / TEXCOORD0 / TANGENT).
	skeletalDesc.VertexStride = 48;
	skeletalDesc.VertexElements = {
		{ "POSITION", 0, EVertexAttributeFormat::Float3, 0 },
		{ "NORMAL",   0, EVertexAttributeFormat::Float3, 24 },
		{ "TEXCOORD", 0, EVertexAttributeFormat::Float2, 16 },
		{ "TANGENT",  0, EVertexAttributeFormat::Float3, 36 },
	};
	try
	{
		SkeletalGBufferGraphicsPipeline = renderBackend->CreateGraphicsPipeline(skeletalDesc);
	}
	catch (const std::exception& ex)
	{
		AppendCpuRuntimeTrace(L"[InitGBufferPass] SkeletalGBuffer pipeline create exception");
		(void)ex;
	}
	if (!SkeletalGBufferGraphicsPipeline)
		AppendCpuRuntimeTrace(L"[InitGBufferPass] failed to create Skeletal GBuffer pipeline");

	// Path C: VS inline skinning PSO. Same IA layout, different VS entry,
	// adds one SBV for the current-frame bone palette.
	GraphicsPipelineDesc vsInlineDesc = desc;
	vsInlineDesc.VertexEntryPoint = "SkeletalVsInlineVSMain";
	vsInlineDesc.BufferBindings = {
		{ "SkeletalInputs", 5 },
		{ "SkeletalPrevBones", 6 },
		{ "SkeletalCurrBones", 7 },
	};
	// Same StandardVertex IA layout as skeletalDesc (48 B stride).
	vsInlineDesc.VertexStride = 48;
	vsInlineDesc.VertexElements = skeletalDesc.VertexElements;
	try
	{
		SkeletalVsInlineGraphicsPipeline = renderBackend->CreateGraphicsPipeline(vsInlineDesc);
	}
	catch (const std::exception& ex)
	{
		AppendCpuRuntimeTrace(L"[InitGBufferPass] SkeletalVsInline pipeline create exception");
		(void)ex;
	}
	if (!SkeletalVsInlineGraphicsPipeline)
		AppendCpuRuntimeTrace(L"[InitGBufferPass] failed to create Skeletal VS-inline GBuffer pipeline");

	// Phase D (desktop only): instanced cluster PSO. One DrawIndexedInstanced
	// draws every skeletal character; the VS reads its world matrix from
	// SkeletalInstanceTransforms[SV_InstanceID]. Mobile shaders
	// (GBufferMobile.hlsl) deliberately do not contain this entry — the
	// Adreno SPIR-V compiler couldn't handle the SV_InstanceID +
	// SkeletalInstanceTransforms variant. We try/catch so missing-entry
	// failures on mobile leave the PSO null and the caller falls back.
	if (!CORONA_PLATFORM_MOBILE)
	{
		GraphicsPipelineDesc vsClusterDesc = desc;
		vsClusterDesc.VertexEntryPoint = "SkeletalVsInlineClusterVSMain";
		vsClusterDesc.BufferBindings = {
			{ "SkeletalInputs", 5 },
			{ "SkeletalPrevBones", 6 },
			{ "SkeletalCurrBones", 7 },
			{ "SkeletalInstanceTransforms", 8 },
		};
		vsClusterDesc.VertexStride = 48;
		vsClusterDesc.VertexElements = skeletalDesc.VertexElements;
		try
		{
			SkeletalVsInlineClusterGraphicsPipeline = renderBackend->CreateGraphicsPipeline(vsClusterDesc);
		}
		catch (const std::exception& ex)
		{
			AppendCpuRuntimeTrace(L"[InitGBufferPass] SkeletalVsInlineCluster pipeline create exception");
			(void)ex;
		}
		if (!SkeletalVsInlineClusterGraphicsPipeline)
			AppendCpuRuntimeTrace(L"[InitGBufferPass] failed to create Skeletal VS-inline cluster GBuffer pipeline");
	}

	auto spineSkinningPSO = renderBackend->CreateComputePipelineStateObject();
	if (spineSkinningPSO)
	{
		spineSkinningPSO->BindSRV("InputVertices", 0, 1);
		spineSkinningPSO->BindSRV("Influences", 1, 1);
		spineSkinningPSO->BindSRV("Bones", 2, 1);
		spineSkinningPSO->BindUAV("OutputVertices", 0);
		spineSkinningPSO->BindCBV("SpineSkinningConstant", 0, sizeof(SpineSkinningConstant));
		if (spineSkinningPSO->InitCS(GetAssetFullPath(L"Shaders\\SpineSkinningCS.hlsl"), "SkinMain"))
			SpineSkinningPSO = spineSkinningPSO;
		else
			AppendCpuRuntimeTrace(L"[InitGBufferPass] failed to create Spine skinning compute PSO");
	}

	// 3D skeletal skinning PSO (separate path from Spine).
	InitSkeletalSkinningPSO();
}

void Corona::InitToneMapPass()
{
	struct PostVertex
	{
		XMFLOAT4 position;
		XMFLOAT2 uv;
	};

	PostVertex quadVertices[] =
	{
		{ { -1.0f, -1.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },    // Bottom left.
		{ { -1.0f, 1.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } },    // Top left.
		{ { 1.0f, -1.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },    // Bottom right.
		{ { 1.0f, 1.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } }        // Top right.
	};

	const UINT vertexBufferSize = sizeof(quadVertices);
	const UINT vertexBufferStride = sizeof(PostVertex);

	FullScreenVB = renderBackend->CreateVertexBuffer(vertexBufferSize, vertexBufferStride, &quadVertices);

	GraphicsPipelineDesc desc{};
	desc.ShaderPath = GetAssetFullPath(L"Shaders\\ToneMapPS.hlsl");
	desc.VertexEntryPoint = "VSMain";
	desc.PixelEntryPoint = "PSMain";
	desc.VertexStride = vertexBufferStride;
	desc.ColorFormats = { ETextureFormat::RGBA8Unorm };
	desc.bDepthEnable = false;
	desc.bCullBackFaces = false;
	desc.bTriangleStrip = true;
	desc.ConstantBufferSize = sizeof(ToneMapCB);
	desc.ConstantBufferBinding = 0;
	desc.VertexElements = {
		{ "POSITION", 0, EVertexAttributeFormat::Float4, 0 },
		{ "TEXCOORD", 0, EVertexAttributeFormat::Float2, 16 }
	};
	desc.TextureBindings = {
		{ "SrcTex", 0 }
	};
	desc.SamplerBindings = {
		{ "sampleWrap", 0 }
	};

	ToneMapGraphicsPipeline = renderBackend->CreateGraphicsPipeline(desc);
}

void Corona::InitParticlePass()
{
	if (!renderBackend)
		return;
	// Pipeline only needs render-target *formats*, not the live textures, so
	// this init order is safe even before LightingBuffer is allocated. The
	// runtime ParticlePass() guards against a missing LightingBuffer.

	GraphicsPipelineDesc desc{};
	desc.ShaderPath = GetAssetFullPath(L"Shaders\\Particle.hlsl");
	desc.VertexEntryPoint = "VSMain";
	desc.PixelEntryPoint = "PSMain";
	desc.VertexStride = sizeof(Particles::QuadVertex);
	desc.VertexElements = {
		{ "POSITION", 0, EVertexAttributeFormat::Float3, 0  },
		{ "TEXCOORD", 0, EVertexAttributeFormat::Float2, 12 },
		{ "COLOR",    0, EVertexAttributeFormat::Float4, 20 },
	};
	desc.TextureBindings = {
		{ "ParticleTex", 0 },
	};
	desc.SamplerBindings = {
		{ "samplerClamp", 0 },
	};
	// Single-RT pass: draws into the post-light HDR LightingBuffer
	// (RGBA16Float). Depth-tested against the GBuffer DepthBuffer but never
	// writes to it. Additive blend by default for M1 (sparks); M3 adds an
	// alpha-blend variant for smoke.
	desc.ColorFormats = { ETextureFormat::RGBA16Float };
	desc.DepthFormat = ETextureFormat::D32Float;
	desc.bDepthEnable = true;
	desc.bDepthWriteEnable = false;
	desc.bCullBackFaces = false;
	desc.BlendMode = EBlendMode::Additive;
	desc.ConstantBufferSize = sizeof(ParticleCB);
	desc.ConstantBufferBinding = 0;

	ParticleGraphicsPipeline = renderBackend->CreateGraphicsPipeline(desc);
	if (!ParticleGraphicsPipeline)
		AppendCpuRuntimeTrace(L"[InitParticlePass] pipeline create failed");
	else
		AppendCpuRuntimeTrace(L"[InitParticlePass] pipeline created");
}

void Corona::UpdateParticleSystems(float dt)
{
	Terrain::Component* terrain = ActiveTerrain.get();
	for (auto& sys : ActiveParticleSystems)
	{
		if (!sys)
			continue;
		sys->SetTerrainCollider(terrain);
		sys->Tick(dt);
	}
}

uint32_t Corona::ParticleBurstForScript(float x, float y, float z, uint32_t count)
{
	if (ActiveParticleSystems.empty() || count == 0)
		return 0;
	auto& sys = ActiveParticleSystems.front();
	if (!sys)
		return 0;
	return sys->SpawnBurst(glm::vec3(x, y, z), count);
}

void Corona::UseNativeCameraForScript(bool enabled)
{
	if (enabled)
	{
		// Seed the SimpleCamera with the active script camera's pose so
		// the free-fly takeover doesn't jump to a stale location. Pull
		// position straight from the entity transform, the look direction
		// from the CameraComponent.
		if (auto activeCam = EntityWorld.GetActiveCameraEntity(); activeCam.IsValid())
		{
			const auto* trans = EntityWorld.GetTransform(activeCam);
			const auto* cam = EntityWorld.GetCamera(activeCam);
			if (trans && cam)
			{
				m_camera.m_position = trans->GetPosition();
				const glm::vec3 look = cam->LookDirection;
				m_camera.m_lookDirection = look;
				m_camera.m_yaw = std::atan2(look.x, look.z);
				m_camera.m_pitch = std::asin(std::clamp(look.y, -1.0f, 1.0f));
				m_camera.m_upDirection = cam->UpDirection;
			}
		}
		EntityWorld.ClearActiveCamera();
		bScriptCameraControlEnabled = false;
	}
	else
	{
		// Re-enable script-driven camera updates. The Luau script is
		// responsible for re-activating its CameraComponent on the next
		// tick (corona.CameraComponent.set with active=true).
		bScriptCameraControlEnabled = true;
	}
}

void Corona::ParticlePass()
{
	if (!renderBackend || !ParticleGraphicsPipeline || ActiveParticleSystems.empty())
		return;
	if (!LightingBuffer || !DepthBuffer)
		return;

	renderBackend->EmitGpuCrashMarker("ParticlePass");

	// LightingPass ends by transitioning LightingBuffer → ShaderRead. Bring
	// it back to RenderTarget for the additive sprite draws, then restore
	// the state the downstream TemporalAA/ToneMap path expects.
	renderBackend->TransitionTexture(LightingBuffer.get(),
		EResourceState::ShaderRead, EResourceState::RenderTarget);

	Texture* color = LightingBuffer.get();
	renderBackend->SetRenderTargets(&color, 1, DepthBuffer.get());
	renderBackend->SetViewportAndScissor(GetRenderWidth(), GetRenderHeight());

	ParticleCB cb{};
	cb.ViewProj = glm::transpose(ViewProjMat);

	// DX12 SetGraphicsRootSignature (inside BindGraphicsPipeline) invalidates
	// root-table writes that came before it — bind first, then descriptors.
	renderBackend->BindGraphicsPipeline(ParticleGraphicsPipeline.get());
	renderBackend->BindGraphicsPipelineSampler(
		ParticleGraphicsPipeline.get(), "samplerClamp", samplerBilinearWrap.get());

	uint32_t totalActive = 0;
	for (auto& sys : ActiveParticleSystems)
	{
		if (!sys)
			continue;
		sys->UploadQuadVertices(renderBackend.get(), ViewMat);
		const uint32_t indexCount = sys->GetIndexCount();
		if (indexCount == 0)
			continue;
		auto& tex = sys->GetTexture();
		auto& vb  = sys->GetVb();
		auto& ib  = sys->GetIb();
		if (!tex || !vb || !ib)
			continue;

		renderBackend->BindGraphicsPipelineTexture(
			ParticleGraphicsPipeline.get(), "ParticleTex", tex.get());
		renderBackend->SetGraphicsPipelineConstantData(
			ParticleGraphicsPipeline.get(), 0, &cb, sizeof(cb));
		renderBackend->BindMeshBuffers(vb.get(), ib.get());
		renderBackend->DrawIndexed(indexCount, 0, 0);
		totalActive += sys->GetActiveCount();
	}

	renderBackend->TransitionTexture(LightingBuffer.get(),
		EResourceState::RenderTarget, EResourceState::ShaderRead);

	static uint32_t s_logCounter = 0;
	if ((++s_logCounter % 120u) == 0u)
	{
		AppendCpuRuntimeTrace(
			L"[Particles] active=" + std::to_wstring(totalActive) +
			L" systems=" + std::to_wstring(ActiveParticleSystems.size()));
	}
}

void Corona::InitDebugPass()
{
	struct PostVertex
	{
		XMFLOAT4 position;
		XMFLOAT2 uv;
	};

	PostVertex quadVertices[] =
	{
		{ { -1.0f, -1.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },    // Bottom left.
		{ { -1.0f, 1.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } },    // Top left.
		{ { 1.0f, -1.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },    // Bottom right.
		{ { 1.0f, 1.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } }        // Top right.
	};

	const UINT vertexBufferSize = sizeof(quadVertices);
	const UINT vertexBufferStride = sizeof(PostVertex);

	FullScreenVB = renderBackend->CreateVertexBuffer(vertexBufferSize, vertexBufferStride, &quadVertices);

	GraphicsPipelineDesc desc{};
	desc.ShaderPath = GetAssetFullPath(L"Shaders\\DebugPS.hlsl");
	desc.VertexEntryPoint = "VSMain";
	desc.PixelEntryPoint = "PSMain";
	desc.VertexStride = vertexBufferStride;
	desc.ColorFormats = { ETextureFormat::RGBA8Unorm };
	desc.bDepthEnable = false;
	desc.bCullBackFaces = false;
	desc.bTriangleStrip = true;
	desc.ConstantBufferSize = sizeof(DebugPassCB);
	desc.ConstantBufferBinding = 0;
	desc.VertexElements = {
		{ "POSITION", 0, EVertexAttributeFormat::Float4, 0 },
		{ "TEXCOORD", 0, EVertexAttributeFormat::Float2, 16 }
	};
	desc.TextureBindings = {
		{ "SrcTex", 0 },
		{ "SrcTexSH", 1 },
		{ "SrcTexNormal", 2 }
	};
	desc.SamplerBindings = {
		{ "samplerWrap", 0 }
	};

	BufferVisualizeGraphicsPipeline = renderBackend->CreateGraphicsPipeline(desc);
}

void Corona::InitLightingPass()
{
	struct PostVertex
	{
		XMFLOAT4 position;
		XMFLOAT2 uv;
	};

	PostVertex quadVertices[] =
	{
		{ { -1.0f, -1.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },
		{ { -1.0f, 1.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } },
		{ { 1.0f, -1.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },
		{ { 1.0f, 1.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } }
	};

	FullScreenVB = renderBackend->CreateVertexBuffer(sizeof(quadVertices), sizeof(PostVertex), &quadVertices);

	GraphicsPipelineDesc desc{};
	desc.ShaderPath = GetAssetFullPath(L"Shaders\\LightingPS.hlsl");
	desc.VertexEntryPoint = "VSMain";
	desc.PixelEntryPoint = "PSMain";
	desc.VertexStride = sizeof(PostVertex);
	desc.ColorFormats = { ETextureFormat::RGBA16Float };
	desc.bDepthEnable = false;
	desc.bCullBackFaces = false;
	desc.bTriangleStrip = true;
	desc.ConstantBufferSize = sizeof(LightingParam);
	desc.ConstantBufferBinding = 0;
	desc.VertexElements = {
		{ "POSITION", 0, EVertexAttributeFormat::Float4, 0 },
		{ "TEXCOORD", 0, EVertexAttributeFormat::Float2, 16 }
	};
	desc.TextureBindings = {
		{ "AlbedoTex", 0 },
		{ "NormalTex", 1 },
		{ "ShadowTex", 2 },
		{ "VelocityTex", 3 },
		{ "DepthTex", 4 },
		{ "GIResultSHTex", 5 },
		{ "GIResultColorTex", 6 },
		{ "SpecularGITex", 7 },
		{ "RoughnessMetalicTex", 8 },
		{ "AmbientOcclusionTex", 14 },
		{ "SkyLightingTex", 15 }
	};
	desc.SamplerBindings = {
		{ "sampleWrap", 0 }
	};

	LightingGraphicsPipeline = renderBackend->CreateGraphicsPipeline(desc);
}

void Corona::InitMobileShadowMapPass()
{
	if (!renderBackend)
		return;

	GraphicsPipelineDesc desc{};
	desc.ShaderPath = GetAssetFullPath(L"Shaders\\MobileShadowMap.hlsl");
	desc.VertexEntryPoint = "VSMain";
	desc.PixelEntryPoint = "PSMain";
	// Default mesh layout (44 B legacy "Vertex"). Skeletal-skinned meshes
	// upload a different 48 B StandardVertex layout; the Skeletal shadow
	// PSO below overrides stride + offsets to match.
	desc.VertexStride = 44;
	desc.ColorFormats.clear();
	desc.DepthFormat = ETextureFormat::D32Float;
	desc.bDepthEnable = true;
	desc.bCullBackFaces = false;
	desc.bDepthBiasEnable = true;
	desc.DepthBiasConstantFactor = 1.25f;
	desc.DepthBiasClamp = 0.0f;
	desc.DepthBiasSlopeFactor = 2.0f;
	desc.ConstantBufferSize = sizeof(ShadowMapConstantBuffer);
	desc.ConstantBufferBinding = 0;
	desc.VertexElements = {
		{ "POSITION", 0, EVertexAttributeFormat::Float3, 0 },
		{ "NORMAL",   0, EVertexAttributeFormat::Float3, 12 },
		{ "TEXCOORD", 0, EVertexAttributeFormat::Float2, 24 },
		{ "TANGENT",  0, EVertexAttributeFormat::Float3, 32 }
	};
	desc.TextureBindings.clear();
	desc.SamplerBindings.clear();

	MobileShadowMapGraphicsPipeline = renderBackend->CreateGraphicsPipeline(desc);
	if (!MobileShadowMapGraphicsPipeline)
		AppendCpuRuntimeTrace(L"[InitMobileShadowMapPass] failed to create mobile shadow map pipeline");

	GraphicsPipelineDesc skeletalShadowDesc = desc;
	skeletalShadowDesc.VertexStride = 48;
	skeletalShadowDesc.VertexElements = {
		{ "POSITION", 0, EVertexAttributeFormat::Float3, 0 },
		{ "NORMAL",   0, EVertexAttributeFormat::Float3, 24 },
		{ "TEXCOORD", 0, EVertexAttributeFormat::Float2, 16 },
		{ "TANGENT",  0, EVertexAttributeFormat::Float3, 36 },
	};
	SkeletalMobileShadowMapGraphicsPipeline = renderBackend->CreateGraphicsPipeline(skeletalShadowDesc);
	if (!SkeletalMobileShadowMapGraphicsPipeline)
		AppendCpuRuntimeTrace(L"[InitMobileShadowMapPass] failed to create Skeletal mobile shadow map pipeline");

	GraphicsPipelineDesc spineDesc = desc;
	spineDesc.VertexEntryPoint = "SpineVSMain";
	spineDesc.VertexElements.clear();
	spineDesc.VertexStride = 0;
	spineDesc.BufferBindings = {
		{ "SpineVertices", 4 },
	};
	SpineMobileShadowMapGraphicsPipeline = renderBackend->CreateGraphicsPipeline(spineDesc);
	if (!SpineMobileShadowMapGraphicsPipeline)
		AppendCpuRuntimeTrace(L"[InitMobileShadowMapPass] failed to create Spine mobile shadow map pipeline");
}

void Corona::InitTemporalAAPass()
{
	struct PostVertex
	{
		XMFLOAT4 position;
		XMFLOAT2 uv;
	};

	PostVertex quadVertices[] =
	{
		{ { -1.0f, -1.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },
		{ { -1.0f, 1.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } },
		{ { 1.0f, -1.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },
		{ { 1.0f, 1.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } }
	};

	FullScreenVB = renderBackend->CreateVertexBuffer(sizeof(quadVertices), sizeof(PostVertex), &quadVertices);

	GraphicsPipelineDesc desc{};
	desc.ShaderPath = GetAssetFullPath(L"Shaders\\TemporalAA.hlsl");
	desc.VertexEntryPoint = "VSMain";
	desc.PixelEntryPoint = "PSMain";
	desc.VertexStride = sizeof(PostVertex);
	desc.ColorFormats = { ETextureFormat::RGBA16Float };
	desc.bDepthEnable = false;
	desc.bCullBackFaces = false;
	desc.bTriangleStrip = true;
	desc.ConstantBufferSize = sizeof(TemporalAAParam);
	desc.ConstantBufferBinding = 0;
	desc.VertexElements = {
		{ "POSITION", 0, EVertexAttributeFormat::Float4, 0 },
		{ "TEXCOORD", 0, EVertexAttributeFormat::Float2, 16 }
	};
	desc.TextureBindings = {
		{ "CurrentColorTex", 0 },
		{ "PrevColorTex", 1 },
		{ "VelocityTex", 2 },
		{ "DepthTex", 3 },
		{ "BloomTex", 4 }
	};
	desc.BufferBindings = {
		{ "Exposure", 5 }
	};
	desc.SamplerBindings = {
		{ "sampleWrap", 0 }
	};

	TemporalAAGraphicsPipeline = renderBackend->CreateGraphicsPipeline(desc);
}

void Corona::ToneMapPass()
{
	renderBackend->EmitGpuCrashMarker("ToneMapPass");

	if (!ToneMapGraphicsPipeline)
		return;

	Texture* ResolveTarget = GetCurrentResolveSource();
	if (!ResolveTarget)
		return;

	ToneMapCB.Offset = glm::vec4(0, 0, 0, 0);
	ToneMapCB.Scale = glm::vec4(1, 1, 0, 0);
	ToneMapCB.ToneMapMode = ToneMapMode;

	// DX12 invalidates root-table bindings when SetGraphicsRootSignature
	// is called inside BindGraphicsPipeline. Bind the pipeline first so
	// subsequent texture/sampler/CB writes land on the right root sig.
	renderBackend->BindGraphicsPipeline(ToneMapGraphicsPipeline.get());
	renderBackend->BindGraphicsPipelineTexture(ToneMapGraphicsPipeline.get(), "SrcTex", ResolveTarget);
	renderBackend->BindGraphicsPipelineSampler(ToneMapGraphicsPipeline.get(), "sampleWrap", samplerWrap.get());
	renderBackend->SetGraphicsPipelineConstantData(ToneMapGraphicsPipeline.get(), 0, &ToneMapCB, sizeof(ToneMapCB));
	renderBackend->SetViewportAndScissor(m_width, m_height);
	renderBackend->DrawFullscreenQuad(FullScreenVB.get());
}

void Corona::DebugPass()
{
	if (!renderBackend || !BufferVisualizeGraphicsPipeline)
	{
		bDebugDraw = false;
		return;
	}

	renderBackend->EmitGpuCrashMarker("DebugPass");

	// DX12 SetGraphicsRootSignature (inside BindGraphicsPipeline) invalidates root
	// descriptor tables. Each visualize call must bind the pipeline BEFORE writing
	// textures + CB; the same goes for the once-only sampler bind below.
	auto visualize = [&](const DebugPassCB& cb, Texture* tex) {
		if (!tex) return;
		renderBackend->BindGraphicsPipeline(BufferVisualizeGraphicsPipeline.get());
		renderBackend->BindGraphicsPipelineSampler(BufferVisualizeGraphicsPipeline.get(), "samplerWrap", samplerWrap.get());
		renderBackend->BindGraphicsPipelineTexture(BufferVisualizeGraphicsPipeline.get(), "SrcTex", tex);
		renderBackend->SetGraphicsPipelineConstantData(BufferVisualizeGraphicsPipeline.get(), 0, &cb, sizeof(cb));
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	};
	auto visualizeMulti = [&](const DebugPassCB& cb, Texture* tex, Texture* texSH, Texture* texNormal) {
		if (!tex || !texSH || !texNormal) return;
		renderBackend->BindGraphicsPipeline(BufferVisualizeGraphicsPipeline.get());
		renderBackend->BindGraphicsPipelineSampler(BufferVisualizeGraphicsPipeline.get(), "samplerWrap", samplerWrap.get());
		renderBackend->BindGraphicsPipelineTexture(BufferVisualizeGraphicsPipeline.get(), "SrcTex", tex);
		renderBackend->BindGraphicsPipelineTexture(BufferVisualizeGraphicsPipeline.get(), "SrcTexSH", texSH);
		renderBackend->BindGraphicsPipelineTexture(BufferVisualizeGraphicsPipeline.get(), "SrcTexNormal", texNormal);
		renderBackend->SetGraphicsPipelineConstantData(BufferVisualizeGraphicsPipeline.get(), 0, &cb, sizeof(cb));
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	};



	std::vector<std::function<void(EDebugVisualization eFS)>> functions;
	functions.push_back([&](EDebugVisualization eFS){
		//raytraced shadow
		DebugPassCB cb;

		if (eFS == EDebugVisualization::SHADOW)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if(eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(-0.75, -0.75, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}
		cb.DebugMode = RAW_COPY;
		visualize(cb, ShadowBuffer.get());
	});

	functions.push_back([&](EDebugVisualization eFS) {
		// ray traced ambient occlusion
		DebugPassCB cb;

		if (eFS == EDebugVisualization::RTAO)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(-0.25, 0.75, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}
		if (!AmbientOcclusionBuffer)
			return;
		cb.DebugMode = CHANNEL_X;
		visualize(cb, AmbientOcclusionBuffer.get());
	});

	functions.push_back([&](EDebugVisualization eFS) {
		// world normal
		DebugPassCB cb;

		if (eFS ==  EDebugVisualization::WORLD_NORMAL)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(-0.25, -0.75, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}
		cb.DebugMode = RAW_COPY;
		visualize(cb, NormalBuffers[ColorBufferWriteIndex].get());
	});

	functions.push_back([&](EDebugVisualization eFS) {
		// geom world normal
		DebugPassCB cb;
		if (eFS == EDebugVisualization::GEO_NORMAL)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if(eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(-0.25, -0.25, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}
		cb.DebugMode = RAW_COPY;
		visualize(cb, GeomNormalBuffers[ColorBufferWriteIndex].get());
	});

	functions.push_back([&](EDebugVisualization eFS) {
		// Blooom buffer
		DebugPassCB cb;
		if (eFS == EDebugVisualization::BLOOM)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(-0.25, 0.25, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}
		cb.DebugMode = RAW_COPY;
		visualize(cb, BloomBlurPingPong[0].get());
	});


	functions.push_back([&](EDebugVisualization eFS) {
		// depth
		DebugPassCB cb;

		if (eFS == EDebugVisualization::DEPTH)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if(eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(0.25, -0.75, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.ProjectionParams.z = Near;
		cb.ProjectionParams.w = Far;
		cb.DebugMode = DEPTH;
		visualize(cb, UnjitteredDepthBuffers[ColorBufferWriteIndex].get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// raw diffuse gi
		DebugPassCB cb;

		if (eFS == EDebugVisualization::RAW_DIFFUSE_GI)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else
		{
			// Grid-thumbnail slot removed — only the "Final Diffuse GI"
			// thumbnail is kept in the overview. Remaining diffuse-GI
			// debug buffers are still selectable via the fullscreen
			// dropdown above.
			return;
		}

		cb.DebugMode = RAW_COPY;
		visualize(cb, DiffuseGIRaw.get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// raw diffuse gi aux
		DebugPassCB cb;

		if (eFS == EDebugVisualization::RAW_DIFFUSE_GI_AUX)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		visualize(cb, DiffuseGIRawAux.get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// screen probe diffuse gi resolve
		DebugPassCB cb;

		if (eFS == EDebugVisualization::SCREEN_PROBE_DIFFUSE_GI)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else
		{
			return;
		}

		if (!ScreenProbeGIResolved)
			return;
		cb.DebugMode = RAW_COPY;
		visualize(cb, ScreenProbeGIResolved.get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// nearest screen probe radiance debug
		DebugPassCB cb;

		if (eFS == EDebugVisualization::SCREEN_PROBE_PROBES)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else
		{
			return;
		}

		if (!ScreenProbeGIProbeDebug)
			return;
		cb.DebugMode = RAW_COPY;
		visualize(cb, ScreenProbeGIProbeDebug.get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// full-resolution screen-probe resolve history length
		DebugPassCB cb;

		if (eFS == EDebugVisualization::SCREEN_PROBE_HISTORY_LENGTH)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else
		{
			return;
		}

		if (!ScreenProbeGIHistory[ScreenProbeGIHistoryWriteIndex])
			return;
		cb.DebugMode = HISTORY_LENGTH;
		visualize(cb, ScreenProbeGIHistory[ScreenProbeGIHistoryWriteIndex].get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// probe-atlas radiance history length
		DebugPassCB cb;

		if (eFS == EDebugVisualization::SCREEN_PROBE_ATLAS_HISTORY_LENGTH)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else
		{
			return;
		}

		if (!ScreenProbeGIRadiance[ScreenProbeGIAtlasWriteIndex])
			return;
		cb.DebugMode = HISTORY_LENGTH;
		visualize(cb, ScreenProbeGIRadiance[ScreenProbeGIAtlasWriteIndex].get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// temporal filtered diffuse gi
		DebugPassCB cb;

		if (eFS == EDebugVisualization::TEMPORAL_FILTERED_DIFFUSE_GI)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		visualize(cb, DiffuseGITemporal[GIBufferWriteIndex].get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// resolved diffuse gi
		DebugPassCB cb;

		if (eFS == EDebugVisualization::RESOLVED_DIFFUSE_GI)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		Texture* resolvedDiffuse =
			(DiffuseGIMode == EDiffuseGIMode::SCREEN_PROBE && ScreenProbeGIResolved) ?
			ScreenProbeGIResolved.get() :
			DiffuseGITemporal[GIBufferWriteIndex].get();
		visualize(cb, resolvedDiffuse);
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// final diffuse gi
		DebugPassCB cb;

		cb.RTSize = glm::vec2(GetRenderWidth(), GetRenderHeight());
		cb.GIBufferScale = GIBufferScale;
		if (eFS == EDebugVisualization::FINAL_DIFFUSE_GI)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(0.75, 0.75, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = SH_LIGHTING;
		Texture* resolvedDiffuse =
			(DiffuseGIMode == EDiffuseGIMode::SCREEN_PROBE && ScreenProbeGIResolved) ?
			ScreenProbeGIResolved.get() :
			DiffuseGITemporal[GIBufferWriteIndex].get();
		visualizeMulti(cb, resolvedDiffuse, DiffuseGITemporalAux[GIBufferWriteIndex].get(), NormalBuffers[ColorBufferWriteIndex].get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// albedo
		DebugPassCB cb;

		if (eFS == EDebugVisualization::ALBEDO)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else  if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(0.75, -0.75, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		visualize(cb, AlbedoBuffer.get());
	});
	
	functions.push_back([&](EDebugVisualization eFS) {
		// velocity
		DebugPassCB cb;

		if (eFS == EDebugVisualization::VELOCITY)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if(eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(0.75, -0.25, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		visualize(cb, VelocityBuffer.get());
	});
	
	functions.push_back([&](EDebugVisualization eFS) {
		// material
		DebugPassCB cb;
		if (eFS == EDebugVisualization::ROUGNESS_METALLIC)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(0.75, 0.25, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		visualize(cb, RoughnessMetalicBuffer.get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// specular raw
		DebugPassCB cb;


		if (eFS == EDebugVisualization::SPECULAR_RAW)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(-0.75, -0.25, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		visualize(cb, SpecularGIRaw.get());
	});

	functions.push_back([&](EDebugVisualization eFS) {
		// temporal filtered specular
		DebugPassCB cb;


		if (eFS == EDebugVisualization::TEMPORAL_FILTERED_SPECULAR)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else
		{
			// Grid-thumbnail slot freed — TemporalDenoisingPass no
			// longer feeds LightingPS for specular (ReSTIR in the
			// reflection raygen handles temporal). SpecularGITemporal
			// is now a stale buffer; thumbnail kept only for the
			// fullscreen dropdown for now.
			return;
		}

		cb.DebugMode = RAW_COPY;
		visualize(cb, SpecularGITemporal[GIBufferWriteIndex].get());
	});

	functions.push_back([&](EDebugVisualization eFS) {
		// history length
		DebugPassCB cb;


		if (eFS == EDebugVisualization::SPEC_HISTORY_LENGTH)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else
		{
			// Grid-thumbnail slot freed — same reason as the
			// "temporal filtered specular" slot above.
			return;
		}

		cb.DebugMode = CHANNEL_W;
		visualize(cb, SpecularGITemporal[GIBufferWriteIndex].get());
	});

	EDebugVisualization FullScreenVisualize = EDebugVisualization::SPECULAR_RAW;

	for (auto& f : functions)
	{
		f(FullscreenDebugBuffer);
	}
}

void Corona::LightingPass()
{
	renderBackend->EmitGpuCrashMarker("LightingPass");

	renderBackend->TransitionTexture(LightingBuffer.get(), EResourceState::ShaderRead, EResourceState::RenderTarget);

	glm::mat4x4 InvViewMat = glm::inverse(ViewMat);
	
	// Calculate light color from sky gradient (same as raytracing modes)
	glm::vec3 normalizedLightDir = glm::normalize(LightDir);
	float lightDirT = 0.5f * (normalizedLightDir.y + 1.0f);
	glm::vec3 lightColor = glm::mix(SkyColorBottom, SkyColorTop, lightDirT);
	
	LightingParam Param;
	Param.ViewMatrix = glm::transpose(ViewMat);
	Param.InvViewMatrix = glm::transpose(InvViewMat);
	Param.InvProjMatrix = glm::transpose(InvProjMat);
	Param.ShadowViewProjectionMatrix = glm::transpose(MobileShadowViewProjMat);
	Param.LightDir = glm::vec4(normalizedLightDir, LightIntensity);
	
	Param.RTSize.x = GetRenderWidth();
	Param.RTSize.y = GetRenderHeight();

	if (IsTemporalAAEnabled())
		Param.TAABlendFactor = 0.1;
	else
		Param.TAABlendFactor = 1.0;

	const bool bMobileHybridDirectOnly =
		CORONA_PLATFORM_MOBILE &&
		RenderingMode == ERenderingMode::HYBRID;
	Param.GIBufferScale = GIBufferScale;
	Param.LightColor = lightColor;
	Param.bEnableDiffuseGI = (!bMobileHybridDirectOnly && bEnableDiffuseGI) ? 1 : 0;
	Param.bEnableSpecularGI = (!bMobileHybridDirectOnly && bEnableSpecularGI) ? 1 : 0;
	Param.bEnableDirectDiffuse = bEnableDirectDiffuse ? 1 : 0;
	Param.bEnableDirectSpecular = bEnableDirectSpecular ? 1 : 0;
	Param.bEnableRTAO = (!bMobileHybridDirectOnly && bEnableRTAO && bRTAOOutputValidThisFrame && AmbientOcclusionBuffer) ? 1 : 0;
	Param.bEnableSkyLighting = (!bMobileHybridDirectOnly && bEnableSkyLighting && bEnableRayTracedSkyLighting && bSkyLightingOutputValidThisFrame && SkyLightingBuffer) ? 1 : 0;
	Param.bEnableSimpleSkyLighting = bEnableSkyLighting ? 1u : 0u;
	Param.RTAOIndirectStrength = RTAOIndirectStrength;
	Param.RTAOIndirectFloor = RTAOIndirectFloor;
	Param.SurfaceBounceStrength = std::clamp(SurfaceBounceStrength, 0.0f, 1.0f);
	Param.SurfaceBounceSaturation = std::clamp(SurfaceBounceSaturation, 0.0f, 1.0f);
	Param.SkyLightingStrength = std::clamp(SkyLightingStrength, 0.0f, 1.0f);
	Param.LightingOutputMode = bMobileHybridDirectOnly ? 2u : 0u;
	const bool bUseMobileShadowMap =
		bMobileHybridDirectOnly &&
		bMobileShadowMapValidThisFrame &&
		ShadowBuffer;
	const bool bDirectionalShadowAvailable =
		bUseMobileShadowMap ||
		(!bMobileHybridDirectOnly && bShadowOutputValidThisFrame && ShadowBuffer);
	Param.bEnableDirectionalShadow = bDirectionalShadowAvailable ? 1u : 0u;
	Param.bUseShadowMap = bUseMobileShadowMap ? 1u : 0u;
	Param.ShadowMode = bEnableReSTIRDirectShadow ? 1u : 0u;
	static bool bLoggedMissingDesktopShadowOutput = false;
	if (!bMobileHybridDirectOnly &&
		ShadowBuffer &&
		!bShadowOutputValidThisFrame &&
		!bLoggedMissingDesktopShadowOutput)
	{
		AppendCpuRuntimeTrace(L"[LightingPass] desktop shadow buffer bound before a valid shadow output; preserving previous-frame shadow visibility");
		bLoggedMissingDesktopShadowOutput = true;
	}
	if (bMobileHybridDirectOnly)
	{
		Param.AmbientSkyColorAndStrength = glm::vec4(glm::max(SkyColorTop, glm::vec3(0.0f)), 0.18f);
		Param.AmbientGroundColorAndStrength = glm::vec4(glm::max(SkyColorBottom, glm::vec3(0.0f)), 0.075f);
	}
	else
	{
		Param.AmbientSkyColorAndStrength = glm::vec4(glm::max(SkyColorTop, glm::vec3(0.0f)), 0.0f);
		Param.AmbientGroundColorAndStrength = glm::vec4(glm::max(SkyColorBottom, glm::vec3(0.0f)), 0.0f);
	}
	Param.PointLightCount = 0;
	// Stash original PointLights[] indices so we can map them to shadow
	// channels later (the RT shadow pass picks the same top-3 in-frustum
	// closest lights C++-side and writes their visibility into .gba).
	uint32_t lightSrcGlobalIndex[MaxPointLights] = {};
	for (uint32_t srcIdx = 0; srcIdx < RenderWorld.PointLights.size(); ++srcIdx)
	{
		const PointLightState& pointLight = RenderWorld.PointLights[srcIdx];
		if (!pointLight.bEnabled || Param.PointLightCount >= MaxPointLights)
			continue;

		// Sphere-AABB frustum cull. A light whose influence sphere
		// (centre = position, radius = falloff radius) doesn't touch
		// the view frustum can't reach any visible pixel — the
		// `rangeAtten = saturate(1 - dist/radius)` term goes to 0
		// before any in-frustum surface is reached. Dropping it from
		// the CB means ReSTIR's per-pixel candidate budget
		// (`kFreshBudget = 8` in the raygen) only spends slots on
		// reachable lights, which roughly halves variance on a
		// scene where most lights illuminate off-screen geometry.
		const float radius = std::max(pointLight.Radius, 0.01f);
		const glm::vec3 sphereMin = pointLight.Position - glm::vec3(radius);
		const glm::vec3 sphereMax = pointLight.Position + glm::vec3(radius);
		if (!IsWorldAabbInViewFrustum(sphereMin, sphereMax))
			continue;

		const UINT32 pointLightIndex = Param.PointLightCount++;
		Param.PointLights[pointLightIndex].PositionAndRadius =
			glm::vec4(pointLight.Position, radius);
		Param.PointLights[pointLightIndex].ColorAndIntensity =
			glm::vec4(glm::max(pointLight.Color, glm::vec3(0.0f)), std::max(pointLight.Intensity, 0.0f));
		lightSrcGlobalIndex[pointLightIndex] = srcIdx;
	}

	// Initialize shadow channel map to "no shadow" sentinel for every
	// slot. Option A fills the entries for its top-3 in-frustum closest
	// lights below; ReSTIR leaves the map unused.
	for (auto& slot : Param.ShadowChannelMap)
		slot = glm::uvec4(0xFFFFFFFFu);

	if (!bEnableReSTIRDirectShadow && Param.PointLightCount > 0)
	{
		// Mirror the RaytraceShadowPass candidate-selection (in-frustum
		// closest 3). The shadow pass writes ShadowResult.g/b/a in the
		// order it picked, so the channel index here matches the C++
		// candidate order. Use the same sort criterion (distance² to
		// camera) so the two stay in lockstep frame to frame.
		struct LocalCandidate { uint32_t LightingPSIndex; float DistSq; };
		std::vector<LocalCandidate> cands;
		cands.reserve(Param.PointLightCount);
		const glm::vec3 camPos = RenderFrameCameraPosition;
		for (uint32_t i = 0; i < Param.PointLightCount; ++i)
		{
			const glm::vec3 pos = glm::vec3(Param.PointLights[i].PositionAndRadius);
			const float radius = std::max(Param.PointLights[i].PositionAndRadius.w, 0.01f);
			const glm::vec3 boundsMin = pos - glm::vec3(radius);
			const glm::vec3 boundsMax = pos + glm::vec3(radius);
			if (!IsWorldAabbInViewFrustum(boundsMin, boundsMax))
				continue;
			const glm::vec3 toLight = pos - camPos;
			cands.push_back({ i, glm::dot(toLight, toLight) });
		}
		std::sort(cands.begin(), cands.end(),
			[](const LocalCandidate& a, const LocalCandidate& b) { return a.DistSq < b.DistSq; });
		const uint32_t pick = std::min<uint32_t>(static_cast<uint32_t>(cands.size()), 3u);
		for (uint32_t k = 0; k < pick; ++k)
		{
			const uint32_t lpsIdx = cands[k].LightingPSIndex;
			Param.ShadowChannelMap[lpsIdx >> 2u][lpsIdx & 3u] = k;
		}
		(void)lightSrcGlobalIndex; // already correctly indexed via lpsIdx
	}

	glm::normalize(Param.LightDir);
	Texture* lightingDiffuseAuxTex =
		(!bMobileHybridDirectOnly && DiffuseGITemporalAux[GIBufferWriteIndex]) ?
		DiffuseGITemporalAux[GIBufferWriteIndex].get() :
		DefaultBlackTex.get();
	// Diffuse GI source for LightingPS — same logic as specular below:
	// the second-stage screen-space TemporalDenoisingPass reprojects
	// with the surface motion vector, which smears prev-frame indirect
	// onto disoccluded pixels during camera panning (the indirect
	// bounce doesn't follow the surface). The SpatialHashGI cache is
	// world-space, so its cell-level temporal already covers the
	// "stable across frames" axis correctly. Prefer the per-pixel
	// query result (DiffuseGIHashCached) directly when SpatialHash
	// is the active mode; legacy RT-GI / screen-probe paths stay
	// on the screen-space-denoised buffer.
	Texture* lightingDiffuseTex =
		(!bMobileHybridDirectOnly &&
		 DiffuseGIMode == EDiffuseGIMode::SPATIAL_HASH &&
		 DiffuseGIHashFiltered) ?
			DiffuseGIHashFiltered.get() :
		(!bMobileHybridDirectOnly && DiffuseGITemporal[GIBufferWriteIndex]) ?
			DiffuseGITemporal[GIBufferWriteIndex].get() :
			DefaultBlackTex.get();
	if (!bMobileHybridDirectOnly && DiffuseGIMode == EDiffuseGIMode::SCREEN_PROBE && ScreenProbeGIResolved)
		lightingDiffuseTex = ScreenProbeGIResolved.get();
	// Specular GI source for LightingPS: prefer the ReSTIR-combined
	// raw output over the legacy second-stage TemporalDenoisingPass
	// result. The second-stage denoiser reprojects with the SURFACE
	// motion vector, which is wrong for specular reflections (mirrors
	// follow the reflected world, not the surface) and was the cause
	// of the "noise flowing in random directions" artifact on
	// low-roughness flat surfaces during camera translation. ReSTIR
	// (in RaytracedReflection.hlsl) already does specular-correct
	// temporal reuse via hit-position reprojection + GGX-D-aware
	// target_pdf, so the redundant denoiser pass is bypassed.
	Texture* lightingSpecularTex =
		(!bMobileHybridDirectOnly && SpecularGIRaw) ?
		SpecularGIRaw.get() :
		DefaultBlackTex.get();
	Texture* shadowTex =
		bDirectionalShadowAvailable ?
		ShadowBuffer.get() :
		DefaultWhiteTex.get();
	Texture* ambientOcclusionTex =
		(!bMobileHybridDirectOnly && AmbientOcclusionBuffer) ?
		AmbientOcclusionBuffer.get() :
		DefaultWhiteTex.get();
	Texture* skyLightingTex =
		(!bMobileHybridDirectOnly && SkyLightingBuffer) ?
		SkyLightingBuffer.get() :
		DefaultBlackTex.get();

	if (!LightingGraphicsPipeline)
		return;

	// DX12 SetGraphicsRootSignature (called inside BindGraphicsPipeline) invalidates
	// previously-written root descriptor tables — bind the pipeline first, then write
	// textures/samplers/CB. Same order as InitGBufferPass + DrawScene.
	Texture* lightingTarget = LightingBuffer.get();
	renderBackend->SetRenderTargets(&lightingTarget, 1, nullptr);
	renderBackend->BindGraphicsPipeline(LightingGraphicsPipeline.get());
	renderBackend->BindGraphicsPipelineTexture(LightingGraphicsPipeline.get(), "AlbedoTex", AlbedoBuffer.get());
	renderBackend->BindGraphicsPipelineTexture(LightingGraphicsPipeline.get(), "NormalTex", NormalBuffers[ColorBufferWriteIndex].get());
	renderBackend->BindGraphicsPipelineTexture(LightingGraphicsPipeline.get(), "ShadowTex", shadowTex);
	renderBackend->BindGraphicsPipelineTexture(LightingGraphicsPipeline.get(), "VelocityTex", VelocityBuffer.get());
	renderBackend->BindGraphicsPipelineTexture(LightingGraphicsPipeline.get(), "DepthTex", DepthBuffer.get());
	renderBackend->BindGraphicsPipelineTexture(LightingGraphicsPipeline.get(), "GIResultSHTex", lightingDiffuseAuxTex);
	renderBackend->BindGraphicsPipelineTexture(LightingGraphicsPipeline.get(), "GIResultColorTex", lightingDiffuseTex);
	renderBackend->BindGraphicsPipelineTexture(LightingGraphicsPipeline.get(), "SpecularGITex", lightingSpecularTex);
	renderBackend->BindGraphicsPipelineTexture(LightingGraphicsPipeline.get(), "RoughnessMetalicTex", RoughnessMetalicBuffer.get());
	renderBackend->BindGraphicsPipelineTexture(LightingGraphicsPipeline.get(), "AmbientOcclusionTex", ambientOcclusionTex);
	renderBackend->BindGraphicsPipelineTexture(LightingGraphicsPipeline.get(), "SkyLightingTex", skyLightingTex);
	renderBackend->BindGraphicsPipelineSampler(LightingGraphicsPipeline.get(), "sampleWrap", samplerWrap.get());
	renderBackend->SetGraphicsPipelineConstantData(LightingGraphicsPipeline.get(), 0, &Param, sizeof(Param));
	renderBackend->SetViewportAndScissor(GetRenderWidth(), GetRenderHeight());
	renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	renderBackend->TransitionTexture(LightingBuffer.get(), EResourceState::RenderTarget, EResourceState::ShaderRead);

	if (bAutoAADumpEnabled && DirectLightingBuffer)
	{
		Param.LightingOutputMode = 1;
		renderBackend->TransitionTexture(DirectLightingBuffer.get(), EResourceState::ShaderRead, EResourceState::RenderTarget);
		Texture* directLightingTarget = DirectLightingBuffer.get();
		renderBackend->SetRenderTargets(&directLightingTarget, 1, nullptr);
		// Re-bind pipeline before re-writing CB — same DX12 root-table invalidation rule.
		renderBackend->BindGraphicsPipeline(LightingGraphicsPipeline.get());
		renderBackend->SetGraphicsPipelineConstantData(LightingGraphicsPipeline.get(), 0, &Param, sizeof(Param));
		renderBackend->SetViewportAndScissor(GetRenderWidth(), GetRenderHeight());
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
		renderBackend->TransitionTexture(DirectLightingBuffer.get(), EResourceState::RenderTarget, EResourceState::ShaderRead);
	}
}

void Corona::TemporalAAPass()
{
	renderBackend->EmitGpuCrashMarker("TemporalAAPass");

	UINT PrevColorBufferIndex = 1 - ColorBufferWriteIndex;
	Texture* ResolveTarget = ColorBuffers[ColorBufferWriteIndex].get();

	if (!TemporalAAGraphicsPipeline || !ResolveTarget || !LightingBuffer || !ExposureData)
	{
		bUseLightingBufferFallbackForToneMap = true;
		bTemporalAAHistoryValid = false;
		return;
	}

	Texture* PrevColorBuffer = ColorBuffers[PrevColorBufferIndex].get();
	Texture* BloomTexture = BloomBlurPingPong[0] ? BloomBlurPingPong[0].get() : DefaultBlackTex.get();
	if (!PrevColorBuffer || !BloomTexture)
	{
		bUseLightingBufferFallbackForToneMap = true;
		bTemporalAAHistoryValid = false;
		return;
	}

	TemporalAAParam Param;
	Param.RTSize.x = GetRenderWidth();
	Param.RTSize.y = GetRenderHeight();
	Param.TAABlendFactor = IsTemporalAAEnabled() ? 0.1f : 1.0f;
	Param.ClampMode = ClampMode;
	Param.BloomStrength = BloomBlurPingPong[0] ? BloomStrength : 0.0f;
	Param.HistoryValid = bTemporalAAHistoryValid ? 1u : 0u;
	Param.CurrentJitter = IsJitterEnabled() ? (CurrentJitter * 0.5f) : glm::vec2(0.0f);

	// DX12 root-table invalidation: bind pipeline before per-pipeline writes.
	renderBackend->TransitionTexture(ResolveTarget, EResourceState::ShaderRead, EResourceState::RenderTarget);
	Texture* temporalTarget = ResolveTarget;
	renderBackend->SetRenderTargets(&temporalTarget, 1, nullptr);
	renderBackend->BindGraphicsPipeline(TemporalAAGraphicsPipeline.get());
	renderBackend->BindGraphicsPipelineTexture(TemporalAAGraphicsPipeline.get(), "CurrentColorTex", LightingBuffer.get());
	renderBackend->BindGraphicsPipelineTexture(TemporalAAGraphicsPipeline.get(), "PrevColorTex", PrevColorBuffer);
	renderBackend->BindGraphicsPipelineTexture(TemporalAAGraphicsPipeline.get(), "VelocityTex", VelocityBuffer.get());
	renderBackend->BindGraphicsPipelineTexture(TemporalAAGraphicsPipeline.get(), "DepthTex", DepthBuffer.get());
	renderBackend->BindGraphicsPipelineTexture(TemporalAAGraphicsPipeline.get(), "BloomTex", BloomTexture);
	renderBackend->BindGraphicsPipelineBuffer(TemporalAAGraphicsPipeline.get(), "Exposure", ExposureData.get());
	renderBackend->BindGraphicsPipelineSampler(TemporalAAGraphicsPipeline.get(), "sampleWrap", samplerBilinearWrap ? samplerBilinearWrap.get() : samplerWrap.get());
	renderBackend->SetGraphicsPipelineConstantData(TemporalAAGraphicsPipeline.get(), 0, &Param, sizeof(Param));
	renderBackend->SetViewportAndScissor(GetRenderWidth(), GetRenderHeight());
	renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	renderBackend->TransitionTexture(ResolveTarget, EResourceState::RenderTarget, EResourceState::ShaderRead);

	if (bDrawHistogram && DrawHistogramPSO)
	{
		renderBackend->TransitionTexture(ResolveTarget, EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		DrawHistogramPSO->SetBufferSRV("Histogram", Histogram.get());
		DrawHistogramPSO->SetBufferSRV("Exposure", ExposureData.get());
		DrawHistogramPSO->SetTextureUAV("ColorBuffer", ResolveTarget);
		DrawHistogramPSO->Apply();
		renderBackend->Dispatch(1, 32, 1);
		renderBackend->TransitionTexture(ResolveTarget, EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	}

	bTemporalAAHistoryValid = IsTemporalAAEnabled();
	bUseLightingBufferFallbackForToneMap = false;
	ResolvedColorBufferIndex = ColorBufferWriteIndex;
}

void Corona::BloomPass()
{
	renderBackend->EmitGpuCrashMarker("BloomPass");

	BloomCB.RTSize.x = BloomBufferWidth;
	BloomCB.RTSize.y = BloomBufferHeight;

	float sigma_pixels = BloomSigma * m_height;

	float effective_sigma = sigma_pixels * 0.25f;
	effective_sigma = glm::min(effective_sigma, 100.f);
	effective_sigma = glm::max(effective_sigma, 1.f);
	BloomCB.NumSamples = glm::round(effective_sigma*4.f);
	BloomCB.WeightScale = -1.f / (2.0 * effective_sigma * effective_sigma);
	BloomCB.NormalizationScale = 1.f / (sqrtf(2 * glm::pi<float>()) * effective_sigma);;
	//BloomCB.Exposure = Exposure;
	/*BloomCB.MinLog = kInitialMinLog;
	BloomCB.RcpLogRange = 1.0f / (kInitialMaxLog - kInitialMinLog);*/

	// extraction pass
	renderBackend->TransitionTexture(BloomBlurPingPong[0].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionTexture(LumaBuffer.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	BloomExtractPSO->SetTextureSRV("SrcTex", LightingBuffer.get());
	BloomExtractPSO->SetBufferSRV("Exposure", ExposureData.get());
	BloomExtractPSO->SetTextureUAV("DstTex", BloomBlurPingPong[0].get());
	BloomExtractPSO->SetTextureUAV("LumaResult", LumaBuffer.get());
	BloomExtractPSO->SetSampler("samplerWrap", samplerWrap.get());
	BloomExtractPSO->SetCBVValue("BloomCB", &BloomCB);
	BloomExtractPSO->Apply();

	renderBackend->Dispatch(BloomBufferWidth / 32, BloomBufferHeight / 32, 1);
	renderBackend->TransitionTexture(BloomBlurPingPong[0].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(LumaBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);

	// horizontal pass
	renderBackend->TransitionTexture(BloomBlurPingPong[1].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	BloomBlurPSO->SetTextureSRV("SrcTex", BloomBlurPingPong[0].get());
	BloomBlurPSO->SetTextureUAV("DstTex", BloomBlurPingPong[1].get());
	BloomBlurPSO->SetSampler("samplerWrap", samplerWrap.get());
	BloomCB.BlurDirection = glm::vec2(1, 0);
	BloomBlurPSO->SetCBVValue("BloomCB", &BloomCB);
	BloomBlurPSO->Apply();

	renderBackend->Dispatch(BloomBufferWidth / 32, BloomBufferHeight / 32, 1);
	renderBackend->TransitionTexture(BloomBlurPingPong[1].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);

	// vertical pass
	renderBackend->TransitionTexture(BloomBlurPingPong[0].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	BloomBlurPSO->SetTextureSRV("SrcTex", BloomBlurPingPong[1].get());
	BloomBlurPSO->SetTextureUAV("DstTex", BloomBlurPingPong[0].get());
	BloomBlurPSO->SetSampler("samplerWrap", samplerWrap.get());
	BloomCB.BlurDirection = glm::vec2(0, 1);
	BloomBlurPSO->SetCBVValue("BloomCB", &BloomCB);
	BloomBlurPSO->Apply();

	renderBackend->Dispatch(BloomBufferWidth / 32, BloomBufferHeight / 32, 1);
	renderBackend->TransitionTexture(BloomBlurPingPong[0].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);

	// histogram pass
	renderBackend->TransitionBuffer(Histogram.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	ClearHistogramPSO->SetBufferUAV("Histogram", Histogram.get());
	ClearHistogramPSO->Apply();
	renderBackend->Dispatch(1, 1, 1);

	HistogramPSO->SetTextureSRV("LumaTex", LumaBuffer.get());
	HistogramPSO->SetBufferUAV("Histogram", Histogram.get());
	HistogramPSO->Apply();
	renderBackend->Dispatch(BloomBufferWidth / 16, 1, 1);

	renderBackend->TransitionBuffer(Histogram.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);

	// adapte exposure pass
	renderBackend->TransitionBuffer(ExposureData.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	AdapteExposurePSO->SetBufferSRV("Histogram", Histogram.get());
	AdapteExposurePSO->SetBufferUAV("Exposure", ExposureData.get());
	AdaptExposureCB.PixelCount = BloomBufferWidth * BloomBufferHeight;
	AdapteExposurePSO->SetCBVValue("AdaptExposureCB", &AdaptExposureCB);
	AdapteExposurePSO->Apply();

	renderBackend->Dispatch(1, 1, 1);
	renderBackend->TransitionBuffer(ExposureData.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
}

void Corona::DispatchSpineSkinningForMesh(Mesh* mesh)
{
	// Spine VS-inline mode bypasses the compute pre-pass; the VS reads the
	// input SBVs directly each frame. Mark the mesh "dispatched" so the
	// downstream readiness checks treat it as ready to draw.
	if (bSpineUseVsInlineSkinning && mesh && mesh->bGpuSpineSkinned)
	{
		mesh->bGpuSpineSkinningDispatched = true;
		return;
	}

	if (!renderBackend ||
		!bEnableGpuSpineSkinning ||
		!SpineSkinningPSO ||
		!mesh ||
		!mesh->bGpuSpineSkinned ||
		mesh->bGpuSpineSkinningDispatched ||
		mesh->GpuSpineSkinningVertexCount == 0 ||
		!mesh->GpuSpineInputVertices ||
		!mesh->GpuSpineInfluences ||
		!mesh->GpuSpineBones ||
		!mesh->GpuSpineSkinnedVertices)
	{
		return;
	}


	SpineSkinningConstant constants = {};
	constants.VertexCount = mesh->GpuSpineSkinningVertexCount;
	constants.SourceScale = mesh->GpuSpineSkinningSourceScale;

	renderBackend->TransitionBuffer(mesh->GpuSpineSkinnedVertices.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	SpineSkinningPSO->SetBufferSRV("InputVertices", mesh->GpuSpineInputVertices.get());
	SpineSkinningPSO->SetBufferSRV("Influences", mesh->GpuSpineInfluences.get());
	SpineSkinningPSO->SetBufferSRV("Bones", mesh->GpuSpineBones.get());
	SpineSkinningPSO->SetBufferUAV("OutputVertices", mesh->GpuSpineSkinnedVertices.get());
	SpineSkinningPSO->SetCBVValue("SpineSkinningConstant", &constants);
	SpineSkinningPSO->Apply();
	renderBackend->Dispatch((mesh->GpuSpineSkinningVertexCount + 63u) / 64u, 1, 1);
	renderBackend->TransitionBuffer(mesh->GpuSpineSkinnedVertices.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);

	mesh->bGpuSpineSkinningDispatched = true;
}

void Corona::DispatchSpineSkinningForScene(const shared_ptr<Scene>& scene)
{
	if (!scene)
		return;

	for (const auto& mesh : scene->meshes)
		DispatchSpineSkinningForMesh(mesh.get());
}

void Corona::DispatchSpineSkinningForRenderWorld()
{
	if (!renderBackend || !bEnableGpuSpineSkinning || !SpineSkinningPSO)
		return;

	for (const SceneObject& object : RenderWorld.SceneObjects)
	{
		if (!object.bVisible || !object.ScenePtr)
			continue;
		DispatchSpineSkinningForScene(object.ScenePtr);
	}
}

void Corona::DrawScene(shared_ptr<Scene> scene, const glm::mat4x4& instanceTransform, float Roughness, float Metalic, bool bOverrideRoughnessMetallic)
{
	for (auto& mesh : scene->meshes)
	{
		if (!mesh)
			continue;

		// Spine VS-inline path: skinning math runs in the VS, reading
		// directly from the input SBVs each frame instead of a
		// pre-skinned vertex buffer. No compute pre-pass needed, so we
		// don't gate on bGpuSpineSkinningDispatched. Works on both
		// desktop and mobile (mobile keeps compute disabled separately).
		const bool bUseSpineVsInline =
			bSpineUseVsInlineSkinning &&
			mesh->bGpuSpineSkinned &&
			mesh->GpuSpineInputVertices &&
			mesh->GpuSpineInfluences &&
			mesh->GpuSpineBones &&
			SpineVsInlineGBufferGraphicsPipeline;
		const bool bUseSpineVertexFetch =
			!bUseSpineVsInline &&
			mesh->bGpuSpineSkinned &&
			mesh->bGpuSpineSkinningDispatched &&
			mesh->GpuSpineSkinnedVertices &&
			SpineGBufferGraphicsPipeline;
		const bool bUseCpuSpinePipeline =
			!bUseSpineVsInline &&
			!bUseSpineVertexFetch &&
			mesh->bSpineMesh &&
			CpuSpineGBufferGraphicsPipeline;
		// Phase 11 / Phase A / Path C skeletal paths. Each picks a different
		// PSO + IA source VB but all share the same SkeletalInputs +
		// SkeletalPrevBones bindings.
		//   Path C (VS inline): IA reads bind-pose, VS does skinning inline,
		//                        binds SkeletalCurrBones too.
		//   CPU "Spine-style":  IA reads SkeletalUnifiedCpuSkinnedVb (CPU
		//                        skinned this frame), VS reads PrevBones for
		//                        motion vectors only.
		//   GPU compute:        IA reads the compute-output VB, same VS as CPU.
		const bool bSkeletalReady =
			mesh->bSkeletalSkinned &&
			mesh->bSkeletalSkinningDispatched &&
			mesh->SkeletalInputVertices &&
			mesh->SkeletalPrevBoneMatrices;
		const bool bUseSkeletalVsInline =
			bSkeletalReady &&
			bSkeletalUseVsInlineSkinning &&
			SkeletalUnifiedBoneMatrices &&
			SkeletalUnifiedBindVb &&
			SkeletalVsInlineGraphicsPipeline;
		const bool bUseSkeletalSkinned =
			!bUseSkeletalVsInline &&
			bSkeletalReady &&
			mesh->SkeletalOutputVb &&
			SkeletalGBufferGraphicsPipeline;
		const bool bUseProceduralGrass = mesh->bProceduralGrass && ProceduralGrassGraphicsPipeline;
		GraphicsPipelineHandle* activeGBufferPipeline =
			bUseProceduralGrass ? ProceduralGrassGraphicsPipeline.get() :
			(bUseSkeletalVsInline ? SkeletalVsInlineGraphicsPipeline.get() :
			(bUseSkeletalSkinned ? SkeletalGBufferGraphicsPipeline.get() :
			(bUseSpineVsInline ? SpineVsInlineGBufferGraphicsPipeline.get() :
			(bUseSpineVertexFetch ? SpineGBufferGraphicsPipeline.get() :
			(bUseCpuSpinePipeline ? CpuSpineGBufferGraphicsPipeline.get() : GBufferGraphicsPipeline.get())))));
		renderBackend->BindGraphicsPipeline(activeGBufferPipeline);
		renderBackend->BindGraphicsPipelineSampler(activeGBufferPipeline, "samplerWrap", samplerWrap.get());
		if (bUseSpineVertexFetch)
			renderBackend->BindGraphicsPipelineBuffer(activeGBufferPipeline, "SpineVertices", mesh->GpuSpineSkinnedVertices.get());
		if (bUseSpineVsInline)
		{
			renderBackend->BindGraphicsPipelineBuffer(activeGBufferPipeline, "SpineVsInlineInputVertices", mesh->GpuSpineInputVertices.get());
			renderBackend->BindGraphicsPipelineBuffer(activeGBufferPipeline, "SpineVsInlineInfluences", mesh->GpuSpineInfluences.get());
			renderBackend->BindGraphicsPipelineBuffer(activeGBufferPipeline, "SpineVsInlineBones", mesh->GpuSpineBones.get());
		}
		if (bUseSkeletalSkinned || bUseSkeletalVsInline)
		{
			renderBackend->BindGraphicsPipelineBuffer(activeGBufferPipeline, "SkeletalInputs", mesh->SkeletalInputVertices.get());
			renderBackend->BindGraphicsPipelineBuffer(activeGBufferPipeline, "SkeletalPrevBones", mesh->SkeletalPrevBoneMatrices.get());
		}
		if (bUseSkeletalVsInline)
		{
			renderBackend->BindGraphicsPipelineBuffer(activeGBufferPipeline, "SkeletalCurrBones", SkeletalUnifiedBoneMatrices.get());
		}

		// 3D skeletal skinning: swap the bind-pose VB for the compute-skinned
		// output VB. Layout matches the standard IA so the GBuffer PSO is
		// unchanged. Path C (VS inline) reads bind-pose; CPU mode reads the
		// per-frame UPLOAD VB the CPU skinner produced; default uses the
		// compute-output VB.
		VertexBuffer* drawVb = mesh->Vb.get();
		if (bUseSkeletalVsInline)
		{
			drawVb = SkeletalUnifiedBindVb.get();
		}
		else if (bUseSkeletalSkinned)
		{
			drawVb = (bSkeletalUseCpuSkinning && SkeletalUnifiedCpuSkinnedVb)
				? SkeletalUnifiedCpuSkinnedVb.get()
				: mesh->SkeletalOutputVb.get();
		}
		// Procedural grass has no VB/IB — the VS synthesizes geometry. The
		// existing IA bindings are harmless if we skip BindMeshBuffers.
		if (!bUseProceduralGrass)
			renderBackend->BindMeshBuffers(drawVb, mesh->Ib.get());

		for (int i = 0; i < mesh->Draws.size(); i++)
		{
			Mesh::DrawCall& drawcall = mesh->Draws[i];
			GBufferConstantBuffer objCB = {};
			int sizea = sizeof(GBufferConstantBuffer);

			objCB.ViewProjectionMatrix = glm::transpose(ViewProjMat);
			objCB.PrevViewProjectionMatrix = glm::transpose(PrevViewProjMat);

			//glm::mat4 m; // Identity matrix
			objCB.WorldMatrix = glm::transpose(instanceTransform * mesh->transform);

			objCB.UnjitteredViewProjMat = glm::transpose(UnjitteredViewProjMat);
			objCB.PrevUnjitteredViewProjMat = glm::transpose(PrevUnjitteredViewProjMat);
			objCB.ViewDir.x = RenderFrameCameraLookDirection.x;
			objCB.ViewDir.y = RenderFrameCameraLookDirection.y;
			objCB.ViewDir.z = RenderFrameCameraLookDirection.z;
			objCB.ViewDir.w = 0.0f;
			objCB.BaseColorFactor = drawcall.mat ? drawcall.mat->BaseColorFactor : glm::vec4(1.0f);

			objCB.RTSize.x = GetRenderWidth();
			objCB.RTSize.y = GetRenderHeight();

			objCB.RougnessMetalic.x = Roughness;
			objCB.RougnessMetalic.y = Metalic;

			objCB.bOverrideRougnessMetallic = bOverrideRoughnessMetallic ? 1 : 0;
			// Spine meshes always render unlit + two-sided regardless of
			// whether they go through the compute-skinning vertex-fetch
			// pipeline, the VS-inline path, or the CPU-skinned VBO path.
			const bool bSpineUnlit = bUseSpineVertexFetch || bUseSpineVsInline || mesh->bSpineMesh;
			// Grass blades are thin and translucent; force two-sided so
			// back-of-blade pixels get a normal pointing at the camera and
			// the LightingPS SSS / back-light branch can fire.
			objCB.bTwoSidedLighting = (bSpineUnlit || mesh->bGrassMesh) ? 1u : 0u;
			objCB.bUnlitMaterial = bSpineUnlit ? 1u : 0u;
			objCB.SpineVertexBase = bUseSpineVertexFetch ? drawcall.VertexBase : 0u;
			objCB.SpineSourceScale = (bUseSpineVsInline && mesh->GpuSpineSkinningSourceScale > 0.0f)
				? mesh->GpuSpineSkinningSourceScale : 1.0f;
			const bool bAnySkeletalPath = bUseSkeletalSkinned || bUseSkeletalVsInline;
			objCB.SkeletalCharIndex = bAnySkeletalPath ? mesh->SkeletalCharIndex : 0u;
			objCB.SkeletalVertsPerChar = bAnySkeletalPath ? SkeletalUnifiedVertsPerChar : 0u;
			objCB.SkeletalBoneCount = bAnySkeletalPath ? SkeletalUnifiedBoneCount : 0u;
			// Layer 2 deformation: shared across all draws via render-
			// frame snapshot. Grass bend only fires on meshes flagged
			// bGrassMesh, which DrawScene sets here per draw.
			objCB.MeshDeformParams = glm::vec4(RenderFrameWindTime, 0.0f, 0.0f, 0.0f);
			objCB.GrassBendOrigin = RenderFrameGrassBendOrigin;
			objCB.GrassBendParams = RenderFrameGrassBendParams;
			objCB.bGrassMesh = mesh->bGrassMesh ? 1u : 0u;
			objCB.bTerrainMesh = mesh->bTerrainMesh ? 1u : 0u;
			objCB.bExcludeFromDeformSphere = mesh->bExcludeFromDeformSphere ? 1u : 0u;
			objCB.WindParams = RenderFrameWindParams;
			objCB.WindTuning = RenderFrameWindTuning;
			objCB.TerrainDeformSphere = RenderFrameTerrainDeformSphere;
			if (bUseProceduralGrass)
			{
				objCB.PG_BladeCount    = mesh->Procedural.BladeCount;
				objCB.PG_BladeSegments = mesh->Procedural.BladeSegments;
				objCB.PG_BladeHeight   = (std::max)(0.05f, GrassProceduralBladeHeight);
				objCB.PG_HalfAreaXZ    = mesh->Procedural.HalfAreaXZ;
				objCB.PG_Seed          = mesh->Procedural.Seed;
				objCB.PG_BaseY = 0.5f * (mesh->Procedural.HeightMin + mesh->Procedural.HeightMax);
				if (ActiveTerrain && ActiveTerrain->GetHeightBuffer())
				{
					const auto& th = ActiveTerrain->GetData().Header;
					objCB.PG_TerrainWidth   = th.Width;
					objCB.PG_TerrainDepth   = th.Depth;
					objCB.PG_TerrainScaleXZ = th.WorldScaleXZ;
				}
				objCB.PG_RenderDistance   = (std::max)(15.0f, GrassRenderDistance);
				objCB.PG_BladesPerCell    = (std::max)(1, GrassProceduralBladesPerCell);
				objCB.PG_BladeWidthScale  = (std::max)(0.001f, GrassProceduralBladeWidthScale);
				objCB.PG_BladeTipWidthScale = std::clamp(GrassProceduralBladeTipWidthScale, 0.0f, 1.0f);
			}

			renderBackend->SetGraphicsPipelineConstantData(activeGBufferPipeline, 0, &objCB, sizeof(objCB));

			Texture* AlbedoTex = drawcall.mat->Diffuse ? drawcall.mat->Diffuse.get() : DefaultWhiteTex.get();
			Texture* NormalTex = drawcall.mat->Normal ? drawcall.mat->Normal.get() : DefaultNormalTex.get();
			Texture* RoughnessTex = drawcall.mat->Roughness ? drawcall.mat->Roughness.get() : DefaultRougnessTex.get();
			Texture* MetallicTex = drawcall.mat->Metallic ? drawcall.mat->Metallic.get() : DefaultBlackTex.get();

			renderBackend->BindGraphicsPipelineTexture(activeGBufferPipeline, "AlbedoTex", AlbedoTex);
			renderBackend->BindGraphicsPipelineTexture(activeGBufferPipeline, "NormalTex", NormalTex);
			renderBackend->BindGraphicsPipelineTexture(activeGBufferPipeline, "RoughnessTex", RoughnessTex);
			renderBackend->BindGraphicsPipelineTexture(activeGBufferPipeline, "MetallicTex", MetallicTex);

			static bool bLoggedFirstGBufferDraw = false;
			if (!bLoggedFirstGBufferDraw && !mesh->CpuPositions.empty() && !mesh->CpuIndices.empty() && drawcall.IndexCount >= 3)
			{
				const glm::mat4 world = instanceTransform * mesh->transform;
				auto matrixFinite = [](const glm::mat4& matrix)
				{
					for (int column = 0; column < 4; ++column)
					{
						for (int row = 0; row < 4; ++row)
						{
							if (!std::isfinite(matrix[column][row]))
								return false;
						}
					}
					return true;
				};
				std::wstring logLine =
					L"[GBufferFirstDraw] stride=" + std::to_wstring(mesh->VertexStride) +
					L", vertices=" + std::to_wstring(mesh->NumVertices) +
					L", indices=" + std::to_wstring(mesh->NumIndices) +
					L", indexStart=" + std::to_wstring(drawcall.IndexStart) +
					L", indexCount=" + std::to_wstring(drawcall.IndexCount) +
					L", vertexBase=" + std::to_wstring(drawcall.VertexBase) +
					L", worldFinite=" + std::to_wstring(matrixFinite(world) ? 1 : 0) +
					L", viewProjFinite=" + std::to_wstring(matrixFinite(ViewProjMat) ? 1 : 0) +
					L", viewFinite=" + std::to_wstring(matrixFinite(ViewMat) ? 1 : 0) +
					L", projFinite=" + std::to_wstring(matrixFinite(ProjMat) ? 1 : 0) +
					L", world00=" + std::to_wstring(world[0][0]) +
					L", world30=" + std::to_wstring(world[3][0]) +
					L", world31=" + std::to_wstring(world[3][1]) +
					L", world32=" + std::to_wstring(world[3][2]) +
					L", vp00=" + std::to_wstring(ViewProjMat[0][0]);
				for (uint32_t cornerIndex = 0; cornerIndex < 3; ++cornerIndex)
				{
					const uint32_t indexOffset = drawcall.IndexStart + cornerIndex;
					if (indexOffset >= mesh->CpuIndices.size())
						break;
					const int32_t vertexIndex = static_cast<int32_t>(mesh->CpuIndices[indexOffset]) + drawcall.VertexBase;
					if (vertexIndex < 0 || static_cast<size_t>(vertexIndex) >= mesh->CpuPositions.size())
						break;
					const glm::vec4 local = glm::vec4(mesh->CpuPositions[vertexIndex], 1.0f);
					const glm::vec4 worldPos = world * local;
					const glm::vec4 clip = ViewProjMat * worldPos;
					const glm::vec3 ndc =
						std::abs(clip.w) > 1e-6f ?
						glm::vec3(clip.x / clip.w, clip.y / clip.w, clip.z / clip.w) :
						glm::vec3(0.0f);
					logLine +=
						L", v" + std::to_wstring(cornerIndex) +
						L"Idx=" + std::to_wstring(vertexIndex) +
						L", local=(" + std::to_wstring(local.x) +
						L"," + std::to_wstring(local.y) +
						L"," + std::to_wstring(local.z) +
						L"), world=(" + std::to_wstring(worldPos.x) +
						L"," + std::to_wstring(worldPos.y) +
						L"," + std::to_wstring(worldPos.z) +
						L"," + std::to_wstring(worldPos.w) +
						L")" +
						L", clip=(" + std::to_wstring(clip.x) +
						L"," + std::to_wstring(clip.y) +
						L"," + std::to_wstring(clip.z) +
						L"," + std::to_wstring(clip.w) +
						L"), ndc=(" + std::to_wstring(ndc.x) +
						L"," + std::to_wstring(ndc.y) +
						L"," + std::to_wstring(ndc.z) + L")";
				}
				AppendCpuRuntimeTrace(logLine);
				bLoggedFirstGBufferDraw = true;
			}

			if (bUseProceduralGrass)
			{
				const uint32_t vertsPerBlade = mesh->Procedural.BladeSegments * 6u;
				if (ProceduralGrassSequentialIb)
				{
					if (ActiveTerrain && ActiveTerrain->GetHeightBuffer())
					{
						renderBackend->BindGraphicsPipelineBuffer(
							activeGBufferPipeline, "TerrainHeights",
							ActiveTerrain->GetHeightBuffer().get());
					}
					// Instance count = cells_in_grid × bladesPerCell, clamped
					// to the recipe's BladeCount as a hard upper bound.
					constexpr float kCellSize = 30.0f;
					const uint32_t halfExt = (std::max)(1u,
						static_cast<uint32_t>(objCB.PG_RenderDistance / kCellSize));
					const uint32_t gridSide = halfExt * 2u + 1u;
					const uint32_t cells = gridSide * gridSide;
					const uint64_t want = static_cast<uint64_t>(cells) * objCB.PG_BladesPerCell;
					const uint32_t instanceCount = static_cast<uint32_t>(
						(std::min<uint64_t>)(want, mesh->Procedural.BladeCount));
					renderBackend->BindMeshBuffers(nullptr, ProceduralGrassSequentialIb.get());
					renderBackend->DrawIndexedInstanced(
						vertsPerBlade,
						instanceCount,
						0, 0, 0);
				}
			}
			else
			{
				renderBackend->DrawIndexed(
					drawcall.IndexCount,
					drawcall.IndexStart,
					bUseSpineVertexFetch ? 0 : drawcall.VertexBase);
			}
		}
	}
}

void Corona::DrawSceneShadowMap(shared_ptr<Scene> scene, const glm::mat4x4& instanceTransform)
{
	if (!scene || !MobileShadowMapGraphicsPipeline)
		return;

	for (auto& mesh : scene->meshes)
	{
		if (!mesh)
			continue;

		const bool bUseSpineVertexFetch =
			mesh->bGpuSpineSkinned &&
			mesh->bGpuSpineSkinningDispatched &&
			mesh->GpuSpineSkinnedVertices &&
			SpineMobileShadowMapGraphicsPipeline;
		// Skeletal meshes upload StandardVertex (48 B). Their skinned shadow
		// VBs share the same layout. The base shadow PSO uses the 44 B
		// platformer Vertex layout, so route Skeletal to its own PSO.
		const bool bUseSkeletalShadow =
			!bUseSpineVertexFetch &&
			mesh->bSkeletalSkinned &&
			SkeletalMobileShadowMapGraphicsPipeline;
		GraphicsPipelineHandle* activeShadowPipeline =
			bUseSpineVertexFetch ? SpineMobileShadowMapGraphicsPipeline.get() :
			(bUseSkeletalShadow ? SkeletalMobileShadowMapGraphicsPipeline.get() :
			MobileShadowMapGraphicsPipeline.get());
		renderBackend->BindGraphicsPipeline(activeShadowPipeline);
		if (bUseSpineVertexFetch)
			renderBackend->BindGraphicsPipelineBuffer(activeShadowPipeline, "SpineVertices", mesh->GpuSpineSkinnedVertices.get());

		// Skeletal meshes share a single-copy bind-pose VB; using
		// drawcall.VertexBase against that buffer reads past the end for
		// every char after #0. Route to the per-frame skinned VB instead
		// (CPU-skinned, GPU-compute output, or bind-pose for VS inline).
		VertexBuffer* shadowVb = mesh->Vb.get();
		if (mesh->bSkeletalSkinned && mesh->bSkeletalSkinningDispatched)
		{
			if (bSkeletalUseVsInlineSkinning && SkeletalUnifiedBindVb)
			{
				// VS inline doesn't skin shadows — fall back to bind pose
				// but use BaseVertexLocation = 0 since the shared VB only
				// holds one char's worth of data. Skinned shadow is lost
				// in this mode; acceptable for the benchmark.
				shadowVb = SkeletalUnifiedBindVb.get();
			}
			else if (bSkeletalUseCpuSkinning && SkeletalUnifiedCpuSkinnedVb)
			{
				shadowVb = SkeletalUnifiedCpuSkinnedVb.get();
			}
			else if (mesh->SkeletalOutputVb)
			{
				shadowVb = mesh->SkeletalOutputVb.get();
			}
		}
		renderBackend->BindMeshBuffers(shadowVb, mesh->Ib.get());

		for (int i = 0; i < mesh->Draws.size(); i++)
		{
			Mesh::DrawCall& drawcall = mesh->Draws[i];
			ShadowMapConstantBuffer objCB = {};
			objCB.LightViewProjectionMatrix = glm::transpose(MobileShadowViewProjMat);
			objCB.WorldMatrix = glm::transpose(instanceTransform * mesh->transform);
			objCB.BaseColorFactor = glm::vec4(1.0f);
			objCB.SpineVertexBase = bUseSpineVertexFetch ? drawcall.VertexBase : 0u;

			renderBackend->SetGraphicsPipelineConstantData(activeShadowPipeline, 0, &objCB, sizeof(objCB));

			renderBackend->DrawIndexed(
				drawcall.IndexCount,
				drawcall.IndexStart,
				bUseSpineVertexFetch ? 0 : drawcall.VertexBase);
		}
	}
}

bool Corona::BuildMobileShadowViewProjection(glm::mat4x4& lightViewProj)
{
	auto isFinite3 = [](const glm::vec3& value)
	{
		return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
	};

	auto makeAabbCorners = [](const glm::vec3& boundsMin, const glm::vec3& boundsMax)
	{
		return std::array<glm::vec3, 8>
		{
			glm::vec3(boundsMin.x, boundsMin.y, boundsMin.z),
			glm::vec3(boundsMax.x, boundsMin.y, boundsMin.z),
			glm::vec3(boundsMin.x, boundsMax.y, boundsMin.z),
			glm::vec3(boundsMax.x, boundsMax.y, boundsMin.z),
			glm::vec3(boundsMin.x, boundsMin.y, boundsMax.z),
			glm::vec3(boundsMax.x, boundsMin.y, boundsMax.z),
			glm::vec3(boundsMin.x, boundsMax.y, boundsMax.z),
			glm::vec3(boundsMax.x, boundsMax.y, boundsMax.z),
		};
	};

	auto expandBounds = [](glm::vec3& boundsMin, glm::vec3& boundsMax, const glm::vec3& point)
	{
		boundsMin = glm::min(boundsMin, point);
		boundsMax = glm::max(boundsMax, point);
	};

	struct MobileShadowObjectBounds
	{
		uint32_t ObjectIndex = 0;
		glm::vec3 BoundsMin = glm::vec3(0.0f);
		glm::vec3 BoundsMax = glm::vec3(0.0f);
		glm::vec3 Extents = glm::vec3(0.0f);
		glm::vec3 Center = glm::vec3(0.0f);
		float Radius = 0.0f;
		float CameraDistance = 0.0f;
		bool bCameraVisible = false;
		bool bCameraReceiver = false;
		bool bPlayerCharacter = false;
	};

	MobileShadowCasterObjectIndices.clear();
	MobileShadowLastTotalObjectCount = 0;
	MobileShadowLastCandidateObjectCount = 0;
	MobileShadowLastReceiverObjectCount = 0;
	MobileShadowLastCasterObjectCount = 0;
	MobileShadowLastGuaranteedCasterCount = 0;
	MobileShadowLastCulledObjectCount = 0;
	MobileShadowLastDistanceCulledObjectCount = 0;

	std::vector<MobileShadowObjectBounds> boundedObjects;
	boundedObjects.reserve(RenderWorld.SceneObjects.size());

	const glm::vec3 cameraPosition =
		isFinite3(m_camera.m_position) ?
		m_camera.m_position :
		glm::vec3(InvViewMat[3]);
	glm::vec3 cameraForward =
		isFinite3(m_camera.m_lookDirection) && glm::length(m_camera.m_lookDirection) > 0.0001f ?
		glm::normalize(m_camera.m_lookDirection) :
		glm::vec3(0.0f, 0.0f, 1.0f);
	const glm::vec3 focusReceiverCenter = cameraPosition + cameraForward * MobileShadowFocusDistance;
	const glm::vec3 focusReceiverHalfExtent(
		MobileShadowFocusRadius,
		MobileShadowFocusRadius * 0.60f,
		MobileShadowFocusRadius);
	glm::vec3 receiverMin(std::numeric_limits<float>::max());
	glm::vec3 receiverMax(-std::numeric_limits<float>::max());
	bool bHasReceiverBounds = false;

	for (uint32_t objectIndex = 0; objectIndex < static_cast<uint32_t>(RenderWorld.SceneObjects.size()); ++objectIndex)
	{
		const SceneObject& object = RenderWorld.SceneObjects[objectIndex];
		if (!object.bVisible || !object.ScenePtr || !object.bRayTracing)
			continue;

		++MobileShadowLastTotalObjectCount;

		glm::vec3 boundsMin(0.0f);
		glm::vec3 boundsMax(0.0f);
		glm::vec3 boundsCenter(0.0f);
		float boundsRadius = 0.0f;
		if (!GetSceneObjectWorldBounds(object, boundsMin, boundsMax, boundsCenter, boundsRadius))
			continue;
		if (!isFinite3(boundsMin) || !isFinite3(boundsMax))
			continue;

		MobileShadowObjectBounds objectBounds = {};
		objectBounds.ObjectIndex = objectIndex;
		objectBounds.BoundsMin = boundsMin;
		objectBounds.BoundsMax = boundsMax;
		objectBounds.Extents = glm::max(boundsMax - boundsMin, glm::vec3(0.0f));
		objectBounds.Center = boundsCenter;
		objectBounds.Radius = boundsRadius;
		objectBounds.CameraDistance = std::max(0.0f, glm::length(boundsCenter - cameraPosition) - boundsRadius);
		objectBounds.bCameraVisible = IsWorldAabbInViewFrustum(boundsMin, boundsMax);
		if (const std::string* entityName = EntityWorld.GetName(object.EntityHandle))
			objectBounds.bPlayerCharacter = *entityName == "DungeonCharacter";
		boundedObjects.push_back(objectBounds);
	}

	const MobileShadowObjectBounds* playerBounds = nullptr;
	for (const MobileShadowObjectBounds& objectBounds : boundedObjects)
	{
		if (objectBounds.bPlayerCharacter)
		{
			playerBounds = &objectBounds;
			break;
		}
	}

	const glm::vec3 shadowFocusCenter = playerBounds ? playerBounds->Center : focusReceiverCenter;
	receiverMin = shadowFocusCenter - focusReceiverHalfExtent;
	receiverMax = shadowFocusCenter + focusReceiverHalfExtent;
	bHasReceiverBounds = true;
	if (playerBounds)
	{
		receiverMin = glm::min(receiverMin, playerBounds->BoundsMin);
		receiverMax = glm::max(receiverMax, playerBounds->BoundsMax);
	}

	if (!bHasReceiverBounds)
		return false;

	glm::vec3 lightDir = RenderFrameNormalizedLightDir;
	if (!isFinite3(lightDir) || glm::length(lightDir) < 0.0001f)
		lightDir = LightDir;
	if (!isFinite3(lightDir) || glm::length(lightDir) < 0.0001f)
		lightDir = glm::vec3(0.3f, 0.8f, 0.4f);
	lightDir = glm::normalize(lightDir);

	const glm::vec3 receiverCenter = (receiverMin + receiverMax) * 0.5f;
	const float receiverRadius = std::max(glm::length(receiverMax - receiverMin) * 0.5f, 8.0f);
	const glm::vec3 eye = receiverCenter + lightDir * (receiverRadius + 32.0f);
	glm::vec3 up = std::abs(glm::dot(lightDir, glm::vec3(0.0f, 1.0f, 0.0f))) > 0.95f ?
		glm::vec3(0.0f, 0.0f, 1.0f) :
		glm::vec3(0.0f, 1.0f, 0.0f);

	const glm::mat4x4 lightView = glm::lookAtRH(eye, receiverCenter, up);

	struct LightSpaceBounds
	{
		glm::vec3 Min = glm::vec3(std::numeric_limits<float>::max());
		glm::vec3 Max = glm::vec3(-std::numeric_limits<float>::max());
	};

	auto computeLightSpaceBounds = [&](const glm::vec3& boundsMin, const glm::vec3& boundsMax)
	{
		LightSpaceBounds lightBounds;
		for (const glm::vec3& corner : makeAabbCorners(boundsMin, boundsMax))
		{
			const glm::vec3 lightSpaceCorner = glm::vec3(lightView * glm::vec4(corner, 1.0f));
			expandBounds(lightBounds.Min, lightBounds.Max, lightSpaceCorner);
		}
		return lightBounds;
	};

	glm::vec3 receiverLightMin(std::numeric_limits<float>::max());
	glm::vec3 receiverLightMax(-std::numeric_limits<float>::max());
	const LightSpaceBounds focusLightBounds = computeLightSpaceBounds(receiverMin, receiverMax);
	expandBounds(receiverLightMin, receiverLightMax, focusLightBounds.Min);
	expandBounds(receiverLightMin, receiverLightMax, focusLightBounds.Max);

	const float xyPadding = std::clamp(receiverRadius * 0.03f, 4.0f, 64.0f);
	const float zPadding = std::clamp(receiverRadius * 0.05f, 6.0f, 128.0f);
	glm::vec3 projectionLightMin = receiverLightMin;
	glm::vec3 projectionLightMax = receiverLightMax;

	float shadowMinZ = receiverLightMin.z;
	float shadowMaxZ = receiverLightMax.z;

	struct NearbyCasterCandidate
	{
		const MobileShadowObjectBounds* Bounds = nullptr;
		float DistanceSq = 0.0f;
	};

	std::vector<const MobileShadowObjectBounds*> selectedCasterBounds;
	selectedCasterBounds.reserve(static_cast<size_t>(MobileShadowNearbyCasterCount + 1));
	std::vector<NearbyCasterCandidate> nearbyCandidates;
	nearbyCandidates.reserve(boundedObjects.size());
	for (const MobileShadowObjectBounds& objectBounds : boundedObjects)
	{
		if (objectBounds.bPlayerCharacter)
		{
			selectedCasterBounds.push_back(&objectBounds);
			++MobileShadowLastGuaranteedCasterCount;
			continue;
		}

		if (!objectBounds.bCameraVisible || objectBounds.Extents.y < MobileShadowMinCasterHeight)
			continue;

		const glm::vec3 delta = objectBounds.Center - shadowFocusCenter;
		nearbyCandidates.push_back({ &objectBounds, glm::dot(delta, delta) });
	}
	MobileShadowLastCandidateObjectCount =
		static_cast<uint64_t>(nearbyCandidates.size() + selectedCasterBounds.size());
	std::sort(nearbyCandidates.begin(), nearbyCandidates.end(), [](const NearbyCasterCandidate& lhs, const NearbyCasterCandidate& rhs)
	{
		if (lhs.DistanceSq != rhs.DistanceSq)
			return lhs.DistanceSq < rhs.DistanceSq;
		const float lhsRadius = lhs.Bounds ? lhs.Bounds->Radius : 0.0f;
		const float rhsRadius = rhs.Bounds ? rhs.Bounds->Radius : 0.0f;
		return lhsRadius < rhsRadius;
	});
	for (const NearbyCasterCandidate& candidate : nearbyCandidates)
	{
		if (selectedCasterBounds.size() >= static_cast<size_t>(MobileShadowNearbyCasterCount + (playerBounds ? 1u : 0u)))
			break;
		if (candidate.Bounds)
			selectedCasterBounds.push_back(candidate.Bounds);
	}

	for (const MobileShadowObjectBounds* objectBounds : selectedCasterBounds)
	{
		if (!objectBounds)
			continue;

		MobileShadowCasterObjectIndices.push_back(objectBounds->ObjectIndex);
		const LightSpaceBounds casterLightBounds = computeLightSpaceBounds(objectBounds->BoundsMin, objectBounds->BoundsMax);
		expandBounds(projectionLightMin, projectionLightMax, casterLightBounds.Min);
		expandBounds(projectionLightMin, projectionLightMax, casterLightBounds.Max);
		shadowMinZ = std::min(shadowMinZ, casterLightBounds.Min.z);
		shadowMaxZ = std::max(shadowMaxZ, casterLightBounds.Max.z);
		if (objectBounds->bCameraVisible)
		{
			++MobileShadowLastReceiverObjectCount;
			receiverMin = glm::min(receiverMin, objectBounds->BoundsMin);
			receiverMax = glm::max(receiverMax, objectBounds->BoundsMax);
		}
	}

	MobileShadowLastCasterObjectCount = MobileShadowCasterObjectIndices.size();
	MobileShadowLastCulledObjectCount =
		MobileShadowLastTotalObjectCount > MobileShadowLastCasterObjectCount ?
		MobileShadowLastTotalObjectCount - MobileShadowLastCasterObjectCount :
		0;
	if (MobileShadowCasterObjectIndices.empty())
		return false;

	float left = projectionLightMin.x - xyPadding;
	float right = projectionLightMax.x + xyPadding;
	float bottom = projectionLightMin.y - xyPadding;
	float top = projectionLightMax.y + xyPadding;
	const float minExtent = 8.0f;
	if (right - left < minExtent)
	{
		const float center = (left + right) * 0.5f;
		left = center - minExtent * 0.5f;
		right = center + minExtent * 0.5f;
	}
	if (top - bottom < minExtent)
	{
		const float center = (bottom + top) * 0.5f;
		bottom = center - minExtent * 0.5f;
		top = center + minExtent * 0.5f;
	}

	auto snapBoundsToShadowTexels = [](float& minValue, float& maxValue)
	{
		const float extent = maxValue - minValue;
		if (extent <= 0.0f)
			return;

		const float texelSize = extent / static_cast<float>(MobileShadowMapResolution);
		if (texelSize <= 1.0e-5f)
			return;

		const float center = (minValue + maxValue) * 0.5f;
		const float snappedCenter = std::floor((center / texelSize) + 0.5f) * texelSize;
		minValue = snappedCenter - extent * 0.5f;
		maxValue = snappedCenter + extent * 0.5f;
	};
	snapBoundsToShadowTexels(left, right);
	snapBoundsToShadowTexels(bottom, top);

	const float nearPlane = std::max(0.1f, -shadowMaxZ - zPadding);
	const float farPlane = std::max(nearPlane + 1.0f, -shadowMinZ + zPadding);
	const glm::mat4x4 lightProjection = glm::orthoRH(left, right, bottom, top, nearPlane, farPlane);
	lightViewProj = lightProjection * lightView;
	return true;
}

void Corona::MobileShadowMapPass()
{
	bMobileShadowMapValidThisFrame = false;
	if (!renderBackend ||
		RenderingMode != ERenderingMode::HYBRID ||
		!MobileShadowMapGraphicsPipeline ||
		!ShadowBuffer)
	{
		return;
	}

	glm::mat4x4 lightViewProj(1.0f);
	if (!BuildMobileShadowViewProjection(lightViewProj))
		return;

	MobileShadowViewProjMat = lightViewProj;
	renderBackend->EmitGpuCrashMarker("MobileShadowMapPass");
	DispatchSpineSkinningForRenderWorld();
	renderBackend->TransitionTexture(ShadowBuffer.get(), EResourceState::ShaderRead, EResourceState::DepthWrite);
	renderBackend->ClearDepth(ShadowBuffer.get(), 1.0f);
	renderBackend->SetViewportAndScissor(MobileShadowMapResolution, MobileShadowMapResolution);
	renderBackend->SetRenderTargets(nullptr, 0, ShadowBuffer.get());
	renderBackend->BindGraphicsPipeline(MobileShadowMapGraphicsPipeline.get());

	for (uint32_t objectIndex : MobileShadowCasterObjectIndices)
	{
		if (objectIndex >= RenderWorld.SceneObjects.size())
			continue;
		const SceneObject& object = RenderWorld.SceneObjects[objectIndex];
		if (!object.bVisible || !object.ScenePtr)
			continue;
		DrawSceneShadowMap(object.ScenePtr, object.Transform);
	}

	if ((FrameCounter % 120u) == 0u &&
		(MobileShadowLastTotalObjectCount != 0 ||
		 MobileShadowLastCasterObjectCount != 0 ||
		 MobileShadowLastCulledObjectCount != 0))
	{
		AppendCpuRuntimeTrace(
			L"[MobileShadowCulling] casters=" + std::to_wstring(MobileShadowLastCasterObjectCount) +
			L"/" + std::to_wstring(MobileShadowLastTotalObjectCount) +
			L", candidates=" + std::to_wstring(MobileShadowLastCandidateObjectCount) +
			L", receivers=" + std::to_wstring(MobileShadowLastReceiverObjectCount) +
			L", guaranteed=" + std::to_wstring(MobileShadowLastGuaranteedCasterCount) +
			L", culled=" + std::to_wstring(MobileShadowLastCulledObjectCount) +
			L", distance=" + std::to_wstring(MobileShadowLastDistanceCulledObjectCount));
	}

	renderBackend->TransitionTexture(ShadowBuffer.get(), EResourceState::DepthWrite, EResourceState::ShaderRead);
	bMobileShadowMapValidThisFrame = true;
	bShadowOutputValidThisFrame = true;
}

bool Corona::GetSceneObjectWorldBounds(
	const SceneObject& object,
	glm::vec3& boundsMin,
	glm::vec3& boundsMax,
	glm::vec3& center,
	float& radius) const
{
	if (!object.ScenePtr || !object.ScenePtr->bHasBounds)
		return false;

	const glm::vec3 localMin = object.ScenePtr->BoundsMin;
	const glm::vec3 localMax = object.ScenePtr->BoundsMax;
	const std::array<glm::vec3, 8> corners =
	{
		glm::vec3(localMin.x, localMin.y, localMin.z),
		glm::vec3(localMax.x, localMin.y, localMin.z),
		glm::vec3(localMin.x, localMax.y, localMin.z),
		glm::vec3(localMax.x, localMax.y, localMin.z),
		glm::vec3(localMin.x, localMin.y, localMax.z),
		glm::vec3(localMax.x, localMin.y, localMax.z),
		glm::vec3(localMin.x, localMax.y, localMax.z),
		glm::vec3(localMax.x, localMax.y, localMax.z),
	};

	boundsMin = glm::vec3(std::numeric_limits<float>::max());
	boundsMax = glm::vec3(-std::numeric_limits<float>::max());
	for (const glm::vec3& corner : corners)
	{
		const glm::vec3 worldCorner = glm::vec3(object.Transform * glm::vec4(corner, 1.0f));
		boundsMin = glm::min(boundsMin, worldCorner);
		boundsMax = glm::max(boundsMax, worldCorner);
	}

	center = (boundsMin + boundsMax) * 0.5f;
	radius = glm::length(boundsMax - boundsMin) * 0.5f;
	return radius > 0.001f;
}

bool Corona::IsWorldAabbInViewFrustum(const glm::vec3& boundsMin, const glm::vec3& boundsMax) const
{
	const std::array<glm::vec3, 8> corners =
	{
		glm::vec3(boundsMin.x, boundsMin.y, boundsMin.z),
		glm::vec3(boundsMax.x, boundsMin.y, boundsMin.z),
		glm::vec3(boundsMin.x, boundsMax.y, boundsMin.z),
		glm::vec3(boundsMax.x, boundsMax.y, boundsMin.z),
		glm::vec3(boundsMin.x, boundsMin.y, boundsMax.z),
		glm::vec3(boundsMax.x, boundsMin.y, boundsMax.z),
		glm::vec3(boundsMin.x, boundsMax.y, boundsMax.z),
		glm::vec3(boundsMax.x, boundsMax.y, boundsMax.z),
	};

	std::array<uint32_t, 6> outsideCounts = {};
	for (const glm::vec3& corner : corners)
	{
		const glm::vec4 clip = UnjitteredViewProjMat * glm::vec4(corner, 1.0f);
		if (clip.x < -clip.w) ++outsideCounts[0];
		if (clip.x >  clip.w) ++outsideCounts[1];
		if (clip.y < -clip.w) ++outsideCounts[2];
		if (clip.y >  clip.w) ++outsideCounts[3];
		if (clip.z < -clip.w) ++outsideCounts[4];
		if (clip.z >  clip.w) ++outsideCounts[5];
	}

	for (uint32_t outsideCount : outsideCounts)
	{
		if (outsideCount == corners.size())
			return false;
	}
	return true;
}

void Corona::PrepareGBufferCulling(uint32_t sceneObjectCount)
{
	GBufferLastTotalObjectCount = 0;
	GBufferLastVisibleObjectCount = 0;
	GBufferLastFrustumCulledObjectCount = 0;
	GBufferLastOcclusionCulledObjectCount = 0;
	GBufferOcclusionQueryCount = 0;
	bGBufferOcclusionQueriesActive = false;

	if (!renderBackend || renderBackend->GetAPI() != ERenderBackendAPI::D3D12 || sceneObjectCount == 0)
		return;

	uint32_t capacityPerFrame = 256u;
	while (capacityPerFrame < sceneObjectCount + 32u)
		capacityPerFrame *= 2u;

	if (capacityPerFrame != GBufferOcclusionQueryCapacityPerFrame)
	{
		GBufferOcclusionQueryCapacityPerFrame = capacityPerFrame;
		SceneObjectCullingStates.clear();
		renderBackend->InitializeOcclusionQueries(GBufferOcclusionQueryCapacityPerFrame * std::max<uint32_t>(1u, renderBackend->GetFrameCount()));
	}

	GBufferOcclusionFrameIndex = renderBackend->GetCurrentFrameIndex();
	bGBufferOcclusionQueriesActive = GBufferOcclusionQueryCapacityPerFrame > 0;
}

bool Corona::ShouldDrawSceneObjectInGBuffer(const SceneObject& object, const glm::vec3& boundsCenter, float boundsRadius)
{
	if (!bGBufferOcclusionQueriesActive || object.Handle == InvalidSceneObjectHandle)
		return true;

	SceneObjectCullingState& state = SceneObjectCullingStates[object.Handle];
	const float movementThreshold = std::max(4.0f, boundsRadius * 0.05f);
	const bool boundsChanged =
		!state.HasBounds ||
		glm::length(boundsCenter - state.LastBoundsCenter) > movementThreshold ||
		std::abs(boundsRadius - state.LastBoundsRadius) > movementThreshold;
	if (boundsChanged)
	{
		state.LastVisible = true;
		state.HasPendingOcclusionQuery = false;
		state.LastTestFrame = 0;
		state.LastBoundsCenter = boundsCenter;
		state.LastBoundsRadius = boundsRadius;
		state.HasBounds = true;
	}

	if (state.HasPendingOcclusionQuery && state.LastQueryFrameIndex == GBufferOcclusionFrameIndex)
	{
		state.LastVisible = renderBackend->ReadOcclusionQueryValue(state.LastQueryIndex) != 0;
		state.HasPendingOcclusionQuery = false;
	}

	constexpr uint64_t kMaxOcclusionSkipFrames = 8;
	const uint64_t framesSinceTest =
		FrameCounter >= state.LastTestFrame ?
		static_cast<uint64_t>(FrameCounter) - state.LastTestFrame :
		kMaxOcclusionSkipFrames + 1u;
	if (!state.LastVisible && framesSinceTest <= kMaxOcclusionSkipFrames)
	{
		++GBufferLastOcclusionCulledObjectCount;
		return false;
	}

	return true;
}

uint32_t Corona::BeginGBufferOcclusionQuery(SceneObjectHandle handle)
{
	if (!bGBufferOcclusionQueriesActive || handle == InvalidSceneObjectHandle || GBufferOcclusionQueryCount >= GBufferOcclusionQueryCapacityPerFrame)
		return std::numeric_limits<uint32_t>::max();

	const uint32_t queryIndex = GBufferOcclusionFrameIndex * GBufferOcclusionQueryCapacityPerFrame + GBufferOcclusionQueryCount;
	++GBufferOcclusionQueryCount;
	renderBackend->BeginOcclusionQuery(queryIndex);
	return queryIndex;
}

void Corona::EndGBufferOcclusionQuery(SceneObjectHandle handle, uint32_t queryIndex)
{
	if (queryIndex == std::numeric_limits<uint32_t>::max() || handle == InvalidSceneObjectHandle)
		return;

	renderBackend->EndOcclusionQuery(queryIndex);
	SceneObjectCullingState& state = SceneObjectCullingStates[handle];
	state.HasPendingOcclusionQuery = true;
	state.LastQueryIndex = queryIndex;
	state.LastQueryFrameIndex = GBufferOcclusionFrameIndex;
	state.LastTestFrame = FrameCounter;
}

void Corona::FinishGBufferCulling()
{
	if (!bGBufferOcclusionQueriesActive || GBufferOcclusionQueryCount == 0)
		return;

	const uint32_t firstQuery = GBufferOcclusionFrameIndex * GBufferOcclusionQueryCapacityPerFrame;
	renderBackend->ResolveOcclusionQueryRange(firstQuery, GBufferOcclusionQueryCount);
}

void Corona::GBufferPass()
{
	ColorBufferWriteIndex = 1 - ColorBufferWriteIndex;
	//DepthBufferWriteIndex = 1 - DepthBufferWriteIndex;
	renderBackend->EmitGpuCrashMarker("GBufferPass");
	const bool bMobileDirectGBuffer =
		CORONA_PLATFORM_MOBILE &&
		RenderingMode == ERenderingMode::HYBRID;

	renderBackend->TransitionTexture(AlbedoBuffer.get(), EResourceState::ShaderRead, EResourceState::RenderTarget);
	if (!bMobileDirectGBuffer)
		renderBackend->TransitionTexture(SpecularAlbedoBuffer.get(), EResourceState::ShaderRead, EResourceState::RenderTarget);
	renderBackend->TransitionTexture(NormalBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead, EResourceState::RenderTarget);
	if (!bMobileDirectGBuffer)
		renderBackend->TransitionTexture(GeomNormalBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead, EResourceState::RenderTarget);
	renderBackend->TransitionTexture(VelocityBuffer.get(), EResourceState::ShaderRead, EResourceState::RenderTarget);
	renderBackend->TransitionTexture(RoughnessMetalicBuffer.get(), EResourceState::ShaderRead, EResourceState::RenderTarget);

	renderBackend->TransitionTexture(DepthBuffer.get(), EResourceState::ShaderRead, EResourceState::DepthWrite);
	if (!bMobileDirectGBuffer)
		renderBackend->TransitionTexture(UnjitteredDepthBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead, EResourceState::RenderTarget);

	const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
	renderBackend->ClearRenderTarget(AlbedoBuffer.get(), clearColor);
	if (!bMobileDirectGBuffer)
		renderBackend->ClearRenderTarget(SpecularAlbedoBuffer.get(), clearColor);
	const float normalClearColor[] =
	{
		bMobileDirectGBuffer ? 0.5f : 0.0f,
		bMobileDirectGBuffer ? 0.45f : -0.1f,
		bMobileDirectGBuffer ? 0.5f : 0.0f,
		0.0f
	};
	renderBackend->ClearRenderTarget(NormalBuffers[ColorBufferWriteIndex].get(), normalClearColor);
	if (!bMobileDirectGBuffer)
		renderBackend->ClearRenderTarget(GeomNormalBuffers[ColorBufferWriteIndex].get(), normalClearColor);
	const float velocityClearColor[] = { 0.0f, 0.0f};
	renderBackend->ClearRenderTarget(VelocityBuffer.get(), velocityClearColor);
	const float roughnessClearColor[] = { 0.001f, 0.0f, 0.0f, 0.0f };
	renderBackend->ClearRenderTarget(RoughnessMetalicBuffer.get(), roughnessClearColor);

	renderBackend->ClearDepth(DepthBuffer.get(), 1.0f);
	if (!bMobileDirectGBuffer)
	{
		const float ujitteredDepthClearColor[] = { 1.0f, 1.0f, 1.0f, 1.0f};
		renderBackend->ClearRenderTarget(UnjitteredDepthBuffers[ColorBufferWriteIndex].get(), ujitteredDepthClearColor);
	}


	renderBackend->BindDefaultDescriptorHeaps();
	DispatchSpineSkinningForRenderWorld();
	BeginGpuPassTiming(EGpuPass::SkeletalSkinning);
	DispatchSkeletalSkinningForRenderWorld();
	EndGpuPassTiming(EGpuPass::SkeletalSkinning);

	renderBackend->SetViewportAndScissor(GetRenderWidth(), GetRenderHeight());
	if (bMobileDirectGBuffer)
	{
		Texture* renderTargets[] = {
			AlbedoBuffer.get(),
			NormalBuffers[ColorBufferWriteIndex].get(),
			VelocityBuffer.get(),
			RoughnessMetalicBuffer.get()
		};
		renderBackend->SetRenderTargets(renderTargets, static_cast<uint32_t>(std::size(renderTargets)), DepthBuffer.get());
	}
	else
	{
		Texture* renderTargets[] = {
			AlbedoBuffer.get(),
			SpecularAlbedoBuffer.get(),
			NormalBuffers[ColorBufferWriteIndex].get(),
			GeomNormalBuffers[ColorBufferWriteIndex].get(),
			VelocityBuffer.get(),
			RoughnessMetalicBuffer.get(),
			UnjitteredDepthBuffers[ColorBufferWriteIndex].get()
		};
		renderBackend->SetRenderTargets(renderTargets, static_cast<uint32_t>(std::size(renderTargets)), DepthBuffer.get());
	}

	renderBackend->BindGraphicsPipeline(GBufferGraphicsPipeline.get());
	renderBackend->BindGraphicsPipelineSampler(GBufferGraphicsPipeline.get(), "samplerWrap", samplerWrap.get());

	PrepareGBufferCulling(static_cast<uint32_t>(RenderWorld.SceneObjects.size()));

	// Phase D (desktop only): if the cluster PSO is live AND we're in the
	// VS-inline skinning mode, draw all skeletal characters with a single
	// DrawIndexedInstanced (SV_InstanceID picks the per-char world matrix
	// from SkeletalInstanceTransforms). Mobile leaves
	// SkeletalVsInlineClusterGraphicsPipeline null (Adreno couldn't
	// compile the SV_InstanceID + SBV variant) and falls back to the
	// per-mesh DrawScene path below.
	const bool bClusterDrawActive =
		bSkeletalUseVsInlineSkinning &&
		SkeletalVsInlineClusterGraphicsPipeline &&
		SkeletalUnifiedCharCount > 0;
	if (bClusterDrawActive)
	{
		UpdateSkeletalUnifiedInstanceTransforms();
		if (!DrawSkeletalVsInlineClusterDesktop())
		{
			// Fall back to per-mesh path this frame if the draw bailed.
		}
		renderBackend->BindGraphicsPipeline(GBufferGraphicsPipeline.get());
		renderBackend->BindGraphicsPipelineSampler(GBufferGraphicsPipeline.get(), "samplerWrap", samplerWrap.get());
	}

	auto isSkeletalUnifiedObject = [bClusterDrawActive](const std::shared_ptr<Scene>& scene)
	{
		if (!bClusterDrawActive || !scene)
			return false;
		for (const auto& mesh : scene->meshes)
		{
			if (mesh && mesh->bSkeletalSkinned)
				return true;
		}
		return false;
	};

	if (!bMultiThreadRendering)
	{
		auto sceneUsesSpineMesh = [](const std::shared_ptr<Scene>& scene)
		{
			if (!scene)
				return false;
			for (const auto& mesh : scene->meshes)
			{
				if (mesh && mesh->bSpineMesh)
					return true;
			}
			return false;
		};

		for (int drawSpinePass = 0; drawSpinePass < 2; ++drawSpinePass)
		{
			for (const SceneObject& object : RenderWorld.SceneObjects)
			{
				if (!object.bVisible || !object.ScenePtr)
					continue;

				const bool bSpineObject = sceneUsesSpineMesh(object.ScenePtr);
				if ((drawSpinePass == 0 && bSpineObject) || (drawSpinePass == 1 && !bSpineObject))
					continue;

				if (isSkeletalUnifiedObject(object.ScenePtr))
					continue;

				++GBufferLastTotalObjectCount;

				glm::vec3 boundsMin(0.0f);
				glm::vec3 boundsMax(0.0f);
				glm::vec3 boundsCenter(0.0f);
				float boundsRadius = 0.0f;
				if (GetSceneObjectWorldBounds(object, boundsMin, boundsMax, boundsCenter, boundsRadius))
				{
					if (!IsWorldAabbInViewFrustum(boundsMin, boundsMax))
					{
						++GBufferLastFrustumCulledObjectCount;
						auto stateIt = SceneObjectCullingStates.find(object.Handle);
						if (stateIt != SceneObjectCullingStates.end())
						{
							stateIt->second.LastVisible = true;
							stateIt->second.HasPendingOcclusionQuery = false;
						}
						continue;
					}

					if (!ShouldDrawSceneObjectInGBuffer(object, boundsCenter, boundsRadius))
						continue;
				}

				const uint32_t occlusionQueryIndex = BeginGBufferOcclusionQuery(object.Handle);
				const std::shared_ptr<Scene> activeGrassScene = ActiveGrassScene.lock();
				const bool bTerrainScene =
					ActiveTerrain && object.ScenePtr == ActiveTerrain->GetScene();
				// Procedural-grass scenes route through the vertex-pulling PSO
				// regardless of whether they're the active terrain-anchored
				// grass (script-spawned) or a toolbox-spawned grass entity, so
				// detect it from the mesh flag rather than the active-scene
				// pointer. Legacy VB-backed grass keeps the original Grass
				// bucket; procedural lands in its own bucket so the overlay
				// can show their costs separately.
				bool bProceduralGrassScene = false;
				for (const std::shared_ptr<Mesh>& sceneMesh : object.ScenePtr->meshes)
				{
					if (sceneMesh && sceneMesh->bProceduralGrass)
					{
						bProceduralGrassScene = true;
						break;
					}
				}
				const bool bLegacyGrassScene =
					!bProceduralGrassScene && activeGrassScene && object.ScenePtr == activeGrassScene;
				if (bTerrainScene)
					BeginGpuPassTiming(EGpuPass::Terrain);
				else if (bProceduralGrassScene)
					BeginGpuPassTiming(EGpuPass::ProceduralGrass);
				else if (bLegacyGrassScene)
					BeginGpuPassTiming(EGpuPass::Grass);
				DrawScene(
					object.ScenePtr,
					object.Transform,
					object.Roughness,
					object.Metallic,
					object.bOverrideRoughnessMetallic);
				if (bTerrainScene)
					EndGpuPassTiming(EGpuPass::Terrain);
				else if (bProceduralGrassScene)
					EndGpuPassTiming(EGpuPass::ProceduralGrass);
				else if (bLegacyGrassScene)
					EndGpuPassTiming(EGpuPass::Grass);
				EndGBufferOcclusionQuery(object.Handle, occlusionQueryIndex);
				++GBufferLastVisibleObjectCount;
			}
		}
	}
	else
	{
		//UINT RemainDraw = mesh->Draws.size();
		//UINT NumDrawThread = mesh->Draws.size() / (NumThread);
		//UINT StartIndex = 0;

		//vector<ThreadDescriptorHeapPool> vecDHPool;
		//vecDHPool.resize(NumThread);

		//vector<ParallelDrawTaskSet> vecTask;
		//vecTask.resize(NumThread);

		//for (int i = 0; i < NumThread; i++)
		//{
		//	UINT ThisDraw = NumDrawThread;
		//	
		//	if (i == NumThread - 1)
		//		ThisDraw = RemainDraw;

		//	ThreadDescriptorHeapPool& DHPool = vecDHPool[i];
		//	DHPool.AllocPool(RS_Mesh->GetGraphicsBindingDHSize()*ThisDraw);

		//	// draw
		//	ParallelDrawTaskSet& task = vecTask[i];
		//	task.app = this;
		//	task.StartIndex = StartIndex;
		//	task.ThisDraw = ThisDraw;
		//	task.ThreadIndex = i;
		//	task.DHPool = &DHPool;

		//	g_TS.AddTaskSetToPipe(&task);

		//	RemainDraw -= ThisDraw;
		//	StartIndex += ThisDraw;
		//}

		//g_TS.WaitforAll();
	}

	FinishGBufferCulling();

	if ((FrameCounter % 120u) == 0u &&
		(GBufferLastTotalObjectCount != 0 ||
		 GBufferLastVisibleObjectCount != 0 ||
		 GBufferLastFrustumCulledObjectCount != 0 ||
		 GBufferLastOcclusionCulledObjectCount != 0))
	{
		AppendCpuRuntimeTrace(
			L"[GBufferCulling] rendered=" + std::to_wstring(GBufferLastVisibleObjectCount) +
			L"/" + std::to_wstring(GBufferLastTotalObjectCount) +
			L", frustum=" + std::to_wstring(GBufferLastFrustumCulledObjectCount) +
			L", occlusion=" + std::to_wstring(GBufferLastOcclusionCulledObjectCount) +
			L", queries=" + std::to_wstring(GBufferOcclusionQueryCount) +
			L", active=" + std::to_wstring(bGBufferOcclusionQueriesActive ? 1 : 0));
	}
	
	renderBackend->TransitionTexture(AlbedoBuffer.get(), EResourceState::RenderTarget, EResourceState::ShaderRead);
	if (!bMobileDirectGBuffer)
		renderBackend->TransitionTexture(SpecularAlbedoBuffer.get(), EResourceState::RenderTarget, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(NormalBuffers[ColorBufferWriteIndex].get(), EResourceState::RenderTarget, EResourceState::ShaderRead);
	if (!bMobileDirectGBuffer)
		renderBackend->TransitionTexture(GeomNormalBuffers[ColorBufferWriteIndex].get(), EResourceState::RenderTarget, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(VelocityBuffer.get(), EResourceState::RenderTarget, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(RoughnessMetalicBuffer.get(), EResourceState::RenderTarget, EResourceState::ShaderRead);


	renderBackend->TransitionTexture(DepthBuffer.get(), EResourceState::DepthWrite, EResourceState::ShaderRead);
	if (!bMobileDirectGBuffer)
		renderBackend->TransitionTexture(UnjitteredDepthBuffers[ColorBufferWriteIndex].get(), EResourceState::RenderTarget, EResourceState::ShaderRead);
}
