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

void AppendCpuRuntimeTrace(const std::wstring& line);

namespace
{
	constexpr uint32_t kPathTracingBindlessTextureRegisterSpace = 10;
	constexpr uint32_t kPathTracingMaterialBufferRegisterSpace = 11;
	constexpr uint32_t kRTBindlessGeometryBufferRegisterSpace = 12;
	constexpr uint32_t kRTGeometryRecordBufferRegisterSpace = 13;
	constexpr uint32_t kRTInstancePropertyBufferRegisterSpace = 14;

	void HashCombinePathTracingMaterial(uint64_t& seed, uint64_t value)
	{
		seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
	}
}

bool Corona::UsesRTBindlessMaterials() const
{
	if (!renderBackend)
		return false;

	const RenderBackendCapabilities capabilities = renderBackend->GetCapabilities();
	return capabilities.SupportsBindlessTextures && capabilities.SupportsRuntimeDescriptorArrays;
}

bool Corona::UsesRTBindlessGeometry() const
{
	if (!renderBackend)
		return false;

	const RenderBackendCapabilities capabilities = renderBackend->GetCapabilities();
	return capabilities.SupportsBindlessBuffers && capabilities.SupportsRuntimeDescriptorArrays;
}

void Corona::BindRTBindlessMaterialSchema(RTPipelineStateObject& pso, RHIShaderStageMask materialStages)
{
	if (!UsesRTBindlessMaterials())
		return;

	pso.SetShaderDefine("CORONA_BINDLESS_MATERIALS", "1");
	pso.BindSRV(
		"global",
		MakeRHIBindlessTextureSRV(
			"MaterialTextures",
			0,
			kPathTracingBindlessTextureRegisterSpace,
			materialStages));
	pso.BindSRV(
		"global",
		MakeRHIBufferSRV(
			"RtMaterials",
			0,
			materialStages,
			RHIBufferViewKind::Structured,
			1,
			kPathTracingMaterialBufferRegisterSpace));
}

void Corona::BindRTBindlessGeometrySchema(RTPipelineStateObject& pso, RHIShaderStageMask geometryStages)
{
	if (!UsesRTBindlessGeometry())
		return;

	pso.SetShaderDefine("CORONA_BINDLESS_GEOMETRY", "1");
	pso.BindSRV(
		"global",
		MakeRHIBindlessBufferSRV(
			"GeometryBuffers",
			0,
			kRTBindlessGeometryBufferRegisterSpace,
			geometryStages,
			RHIBufferViewKind::Raw));
	pso.BindSRV(
		"global",
		MakeRHIBufferSRV(
			"RtGeometries",
			0,
			geometryStages,
			RHIBufferViewKind::Structured,
			1,
			kRTGeometryRecordBufferRegisterSpace));
	pso.BindSRV(
		"global",
		MakeRHIBufferSRV(
			"RtInstanceProperties",
			0,
			geometryStages,
			RHIBufferViewKind::Raw,
			1,
			kRTInstancePropertyBufferRegisterSpace));
}

bool Corona::EnsureRTMaterialRecordBuffer()
{
	if (!UsesRTBindlessMaterials())
		return false;

	const size_t recordCount = std::max<size_t>(RayTracingInstances.size(), 1);
	std::vector<RTMaterialRecord> records(recordCount);

	auto getPrimaryMaterial = [](Mesh& mesh) -> Material*
	{
		if (!mesh.Draws.empty() && mesh.Draws[0].mat)
			return mesh.Draws[0].mat.get();
		return mesh.Mat.get();
	};

	auto getBindlessTextureIndex = [&](Texture* texture) -> UINT32
	{
		if (!texture)
			return RHI_INVALID_BINDLESS_INDEX;

		RHITextureHandle handle = renderBackend->RegisterBindlessTexture(texture);
		if (!handle.IsValid())
			handle = renderBackend->GetBindlessTextureHandle(texture);
		return handle.IsValid() ? handle.Index : RHI_INVALID_BINDLESS_INDEX;
	};

	bool bAllTexturesRegistered = true;
	for (size_t instanceIndex = 0; instanceIndex < RayTracingInstances.size(); ++instanceIndex)
	{
		const RTInstanceDesc& instance = RayTracingInstances[instanceIndex];
		Mesh* mesh = instance.BottomLevelAS ? instance.BottomLevelAS->MeshPtr : nullptr;

		Texture* albedo = DefaultWhiteTex.get();
		Texture* normal = DefaultNormalTex.get();
		Texture* roughness = DefaultRougnessTex.get();
		Texture* metallic = DefaultBlackTex.get();
		if (mesh)
		{
			if (Material* material = getPrimaryMaterial(*mesh))
			{
				if (material->Diffuse)
					albedo = material->Diffuse.get();
				if (material->Normal)
					normal = material->Normal.get();
				if (material->Roughness)
					roughness = material->Roughness.get();
				if (material->Metallic)
					metallic = material->Metallic.get();
			}
		}

		RTMaterialRecord& record = records[instanceIndex];
		record.AlbedoTextureIndex = getBindlessTextureIndex(albedo);
		record.NormalTextureIndex = getBindlessTextureIndex(normal);
		record.RoughnessTextureIndex = getBindlessTextureIndex(roughness);
		record.MetallicTextureIndex = getBindlessTextureIndex(metallic);
		bAllTexturesRegistered = bAllTexturesRegistered &&
			record.AlbedoTextureIndex != RHI_INVALID_BINDLESS_INDEX &&
			record.NormalTextureIndex != RHI_INVALID_BINDLESS_INDEX &&
			record.RoughnessTextureIndex != RHI_INVALID_BINDLESS_INDEX &&
			record.MetallicTextureIndex != RHI_INVALID_BINDLESS_INDEX;
	}

	uint64_t materialHash = 1469598103934665603ull;
	HashCombinePathTracingMaterial(materialHash, static_cast<uint64_t>(RayTracingInstances.size()));
	for (const RTMaterialRecord& record : records)
	{
		HashCombinePathTracingMaterial(materialHash, record.AlbedoTextureIndex);
		HashCombinePathTracingMaterial(materialHash, record.NormalTextureIndex);
		HashCombinePathTracingMaterial(materialHash, record.RoughnessTextureIndex);
		HashCombinePathTracingMaterial(materialHash, record.MetallicTextureIndex);
	}

	if (RTMaterialRecordBuffer && RTMaterialRecordHash == materialHash)
		return true;

	if (!bAllTexturesRegistered)
	{
		RTMaterialRecordHash = 0;
		AppendCpuRuntimeTrace(L"[RTMaterial] bindless material texture registration failed");
		return false;
	}

	BufferCreateDesc desc = {};
	desc.NumElements = static_cast<uint32_t>(records.size());
	desc.ElementSize = sizeof(RTMaterialRecord);
	desc.InitialState = EInitialResourceState::ShaderRead;
	desc.InitialData = records.data();
	desc.Shape = EBufferShape::Structured;
	desc.bUseDefaultHeap = true;

	RTMaterialRecordBuffer = renderBackend->CreateBuffer(desc);
	if (!RTMaterialRecordBuffer)
	{
		RTMaterialRecordHash = 0;
		AppendCpuRuntimeTrace(L"[RTMaterial] failed to create bindless material record buffer");
		return false;
	}

	RTMaterialRecordHash = materialHash;
	AppendCpuRuntimeTrace(
		L"[RTMaterial] bindless material records uploaded, count=" +
		std::to_wstring(records.size()));
	return true;
}

bool Corona::EnsureRTGeometryRecordBuffer()
{
	if (!UsesRTBindlessGeometry())
		return false;

	if (!InstancePropertyBuffer)
	{
		RTGeometryRecordHash = 0;
		AppendCpuRuntimeTrace(L"[RTGeometry] bindless geometry requires InstancePropertyBuffer");
		return false;
	}

	const size_t recordCount = std::max<size_t>(RayTracingInstances.size(), 1);
	std::vector<RTGeometryRecord> records(recordCount);
	bool bAllBuffersRegistered = true;

	auto getBindlessVertexBufferIndex = [&](VertexBuffer* buffer) -> UINT32
	{
		if (!buffer)
			return RHI_INVALID_BINDLESS_INDEX;

		RHIBufferHandle handle = renderBackend->RegisterBindlessVertexBuffer(buffer);
		if (!handle.IsValid())
			handle = renderBackend->GetBindlessVertexBufferHandle(buffer);
		return handle.IsValid() ? handle.Index : RHI_INVALID_BINDLESS_INDEX;
	};

	auto getBindlessIndexBufferIndex = [&](IndexBuffer* buffer) -> UINT32
	{
		if (!buffer)
			return RHI_INVALID_BINDLESS_INDEX;

		RHIBufferHandle handle = renderBackend->RegisterBindlessIndexBuffer(buffer);
		if (!handle.IsValid())
			handle = renderBackend->GetBindlessIndexBufferHandle(buffer);
		return handle.IsValid() ? handle.Index : RHI_INVALID_BINDLESS_INDEX;
	};

	for (size_t instanceIndex = 0; instanceIndex < RayTracingInstances.size(); ++instanceIndex)
	{
		const RTInstanceDesc& instance = RayTracingInstances[instanceIndex];
		Mesh* mesh = instance.BottomLevelAS ? instance.BottomLevelAS->MeshPtr : nullptr;
		if (!mesh)
			continue;

		RTGeometryRecord& record = records[instanceIndex];
		record.VertexBufferIndex = getBindlessVertexBufferIndex(mesh->Vb.get());
		record.IndexBufferIndex = getBindlessIndexBufferIndex(mesh->Ib.get());
		bAllBuffersRegistered = bAllBuffersRegistered &&
			record.VertexBufferIndex != RHI_INVALID_BINDLESS_INDEX &&
			record.IndexBufferIndex != RHI_INVALID_BINDLESS_INDEX;
	}

	uint64_t geometryHash = 1469598103934665603ull;
	HashCombinePathTracingMaterial(geometryHash, static_cast<uint64_t>(RayTracingInstances.size()));
	for (const RTGeometryRecord& record : records)
	{
		HashCombinePathTracingMaterial(geometryHash, record.VertexBufferIndex);
		HashCombinePathTracingMaterial(geometryHash, record.IndexBufferIndex);
	}

	if (RTGeometryRecordBuffer && RTGeometryRecordHash == geometryHash)
		return true;

	if (!bAllBuffersRegistered)
	{
		RTGeometryRecordHash = 0;
		AppendCpuRuntimeTrace(L"[RTGeometry] bindless geometry buffer registration failed");
		return false;
	}

	BufferCreateDesc desc = {};
	desc.NumElements = static_cast<uint32_t>(records.size());
	desc.ElementSize = sizeof(RTGeometryRecord);
	desc.InitialState = EInitialResourceState::ShaderRead;
	desc.InitialData = records.data();
	desc.Shape = EBufferShape::Structured;
	desc.bUseDefaultHeap = true;

	RTGeometryRecordBuffer = renderBackend->CreateBuffer(desc);
	if (!RTGeometryRecordBuffer)
	{
		RTGeometryRecordHash = 0;
		AppendCpuRuntimeTrace(L"[RTGeometry] failed to create bindless geometry record buffer");
		return false;
	}

	RTGeometryRecordHash = geometryHash;
	AppendCpuRuntimeTrace(
		L"[RTGeometry] bindless geometry records uploaded, count=" +
		std::to_wstring(records.size()));
	return true;
}

bool Corona::EnsurePathTracingPointLightBuffer(UINT32 pointLightStateHash)
{
	if (PathTracingPointLightBuffer && PathTracingPointLightBufferHash == pointLightStateHash)
		return true;

	std::array<PointLightParam, MaxPathTracingPointLights> uploadData = PathTracingPointLights;

	BufferCreateDesc desc = {};
	desc.NumElements = MaxPathTracingPointLights;
	desc.ElementSize = sizeof(PointLightParam);
	desc.InitialState = EInitialResourceState::ShaderRead;
	desc.InitialData = uploadData.data();
	desc.Shape = EBufferShape::Structured;
	desc.bUseDefaultHeap = true;

	PathTracingPointLightBuffer = renderBackend->CreateBuffer(desc);
	if (!PathTracingPointLightBuffer)
	{
		PathTracingPointLightBufferHash = 0xFFFFFFFFu;
		AppendCpuRuntimeTrace(L"[PathTracing] failed to create DEFAULT point light SRV buffer");
		return false;
	}

	PathTracingPointLightBufferHash = pointLightStateHash;
	AppendCpuRuntimeTrace(
		L"[PathTracing] point light buffer uploaded to DEFAULT structured SRV, count=" +
		std::to_wstring(PathTracingPointLightCount));
	return true;
}

void Corona::InitPathTracingPass()
{
	shared_ptr<RTPipelineStateObject> TEMP_PSO_PATH_TRACING = renderBackend->CreateRTPipelineStateObject();
	if (!TEMP_PSO_PATH_TRACING)
		return;
	TEMP_PSO_PATH_TRACING->SetNumInstances(static_cast<uint32_t>(RayTracingInstances.size()));

	TEMP_PSO_PATH_TRACING->AddHitGroup("HitGroup", "PathTracingClosestHit", "PathTracingAnyHit");

	TEMP_PSO_PATH_TRACING->AddShader("PathTracingRayGen", RTPipelineStateObject::RAYGEN);
	
	const RHIShaderStageMask rayGenStage = ToRHIShaderStageMask(RHIShaderStage::RayGeneration);
	const RHIShaderStageMask closestHitStage = ToRHIShaderStageMask(RHIShaderStage::ClosestHit);
	const RHIShaderStageMask anyHitStage = ToRHIShaderStageMask(RHIShaderStage::AnyHit);
	const RHIShaderStageMask materialStage = closestHitStage | anyHitStage;
	BindRTBindlessMaterialSchema(*TEMP_PSO_PATH_TRACING, materialStage);
	BindRTBindlessGeometrySchema(*TEMP_PSO_PATH_TRACING, materialStage);
	TEMP_PSO_PATH_TRACING->BindUAV("global", MakeRHITextureUAV("OutputColor", 0, rayGenStage));
	TEMP_PSO_PATH_TRACING->BindUAV("global", MakeRHITextureUAV("OutAlbedo", 1, rayGenStage));
	TEMP_PSO_PATH_TRACING->BindUAV("global", MakeRHITextureUAV("OutSpecularAlbedo", 2, rayGenStage));
	TEMP_PSO_PATH_TRACING->BindUAV("global", MakeRHITextureUAV("OutNormal", 3, rayGenStage));
	TEMP_PSO_PATH_TRACING->BindUAV("global", MakeRHITextureUAV("OutGeomNormal", 4, rayGenStage));
	TEMP_PSO_PATH_TRACING->BindUAV("global", MakeRHITextureUAV("OutVelocity", 5, rayGenStage));
	TEMP_PSO_PATH_TRACING->BindUAV("global", MakeRHITextureUAV("OutRoughnessMetallic", 6, rayGenStage));
	TEMP_PSO_PATH_TRACING->BindUAV("global", MakeRHITextureUAV("OutDepth", 7, rayGenStage));
	TEMP_PSO_PATH_TRACING->BindUAV("global", MakeRHITextureUAV("OutSpecularHitDistance", 8, rayGenStage));
	TEMP_PSO_PATH_TRACING->BindUAV("global", MakeRHITextureUAV("OutSpecularMotionVector", 9, rayGenStage));
	TEMP_PSO_PATH_TRACING->BindSRV("global", MakeRHIAccelerationStructureSRV("gRtScene", 0, rayGenStage));
	TEMP_PSO_PATH_TRACING->BindSRV("global", MakeRHIBufferSRV("PointLightBuffer", 4, rayGenStage));
	TEMP_PSO_PATH_TRACING->BindCBV("global", MakeRHICBV("ViewParameter", 0, sizeof(PathTracingViewParam), rayGenStage));
	TEMP_PSO_PATH_TRACING->BindSampler("global", MakeRHISampler("sampleWrap", 0, rayGenStage | closestHitStage | anyHitStage));

	TEMP_PSO_PATH_TRACING->AddShader("PathTracingMiss", RTPipelineStateObject::MISS);
	TEMP_PSO_PATH_TRACING->AddShader("ShadowMiss", RTPipelineStateObject::MISS);

	TEMP_PSO_PATH_TRACING->AddShader("PathTracingClosestHit", RTPipelineStateObject::HIT);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingClosestHit", MakeRHIBufferSRV("vertices", 1, closestHitStage, RHIBufferViewKind::Raw));
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingClosestHit", MakeRHIBufferSRV("indices", 2, closestHitStage, RHIBufferViewKind::Raw));
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingClosestHit", MakeRHIBufferSRV("InstanceProperty", 3, closestHitStage, RHIBufferViewKind::Raw));
	if (!UsesRTBindlessMaterials())
	{
		TEMP_PSO_PATH_TRACING->BindSRV("PathTracingClosestHit", MakeRHITextureSRV("AlbedoTex", 5, closestHitStage));
		TEMP_PSO_PATH_TRACING->BindSRV("PathTracingClosestHit", MakeRHITextureSRV("NormalTex", 6, closestHitStage));
		TEMP_PSO_PATH_TRACING->BindSRV("PathTracingClosestHit", MakeRHITextureSRV("RoughnessTex", 7, closestHitStage));
		TEMP_PSO_PATH_TRACING->BindSRV("PathTracingClosestHit", MakeRHITextureSRV("MetallicTex", 8, closestHitStage));
	}

	TEMP_PSO_PATH_TRACING->AddShader("PathTracingAnyHit", RTPipelineStateObject::ANYHIT);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingAnyHit", MakeRHIBufferSRV("vertices", 1, anyHitStage, RHIBufferViewKind::Raw));
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingAnyHit", MakeRHIBufferSRV("indices", 2, anyHitStage, RHIBufferViewKind::Raw));
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingAnyHit", MakeRHIBufferSRV("InstanceProperty", 3, anyHitStage, RHIBufferViewKind::Raw));
	if (!UsesRTBindlessMaterials())
	{
		TEMP_PSO_PATH_TRACING->BindSRV("PathTracingAnyHit", MakeRHITextureSRV("AlbedoTex", 5, anyHitStage));
		TEMP_PSO_PATH_TRACING->BindSRV("PathTracingAnyHit", MakeRHITextureSRV("NormalTex", 6, anyHitStage));
		TEMP_PSO_PATH_TRACING->BindSRV("PathTracingAnyHit", MakeRHITextureSRV("RoughnessTex", 7, anyHitStage));
		TEMP_PSO_PATH_TRACING->BindSRV("PathTracingAnyHit", MakeRHITextureSRV("MetallicTex", 8, anyHitStage));
	}
	TEMP_PSO_PATH_TRACING->Configure(8, 256, sizeof(float) * 2);

	bool bSuccess = TEMP_PSO_PATH_TRACING->InitRS("Shaders\\PathTracing.hlsl");

	if (bSuccess)
	{
		PSO_PATH_TRACING = TEMP_PSO_PATH_TRACING;
	}
}

void Corona::PathTracingPass()
{
	const bool bWritePrimaryGBuffer =
		IsPathTracingDLSSRREnabled() &&
		DLSSRRBuffer &&
		AlbedoBuffer &&
		SpecularAlbedoBuffer &&
		NormalBuffers[ColorBufferWriteIndex] &&
		GeomNormalBuffers[ColorBufferWriteIndex] &&
		VelocityBuffer &&
		RoughnessMetalicBuffer &&
		UnjitteredDepthBuffers[ColorBufferWriteIndex] &&
		PathTracingSpecularHitDistanceBuffer &&
		PathTracingSpecularMotionVectorBuffer;
	Texture* outputColor = PathTracingAccumBuffer[PathTracingWriteIndex].get();
	if (!TLAS || !outputColor)
		return;

	renderBackend->EmitGpuCrashMarker("PathTracingPass");

	if (!PSO_PATH_TRACING)
	{
		InitPathTracingPass();
		if (!PSO_PATH_TRACING)
			return;
	}

	ApplyRenderPointLightsToFrameParams();
	const UINT32 currentPointLightStateHash = ComputePathTracingPointLightStateHash();
	if (!EnsurePathTracingPointLightBuffer(currentPointLightStateHash))
		return;

	const bool bUseBindlessMaterials = UsesRTBindlessMaterials();
	if (bUseBindlessMaterials && !EnsureRTMaterialRecordBuffer())
		return;

	// Transition output buffer to UAV
	renderBackend->TransitionTexture(outputColor, EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	if (bWritePrimaryGBuffer)
	{
		renderBackend->TransitionTexture(AlbedoBuffer.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->TransitionTexture(SpecularAlbedoBuffer.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->TransitionTexture(NormalBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->TransitionTexture(GeomNormalBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->TransitionTexture(VelocityBuffer.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->TransitionTexture(RoughnessMetalicBuffer.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->TransitionTexture(UnjitteredDepthBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->TransitionTexture(PathTracingSpecularHitDistanceBuffer.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->TransitionTexture(PathTracingSpecularMotionVectorBuffer.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	}

	auto transitionPathTracingOutputsToShaderRead = [&]()
	{
		renderBackend->TransitionTexture(outputColor, EResourceState::UnorderedAccess, EResourceState::ShaderRead);
		if (bWritePrimaryGBuffer)
		{
			renderBackend->TransitionTexture(AlbedoBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
			renderBackend->TransitionTexture(SpecularAlbedoBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
			renderBackend->TransitionTexture(NormalBuffers[ColorBufferWriteIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
			renderBackend->TransitionTexture(GeomNormalBuffers[ColorBufferWriteIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
			renderBackend->TransitionTexture(VelocityBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
			renderBackend->TransitionTexture(RoughnessMetalicBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
			renderBackend->TransitionTexture(UnjitteredDepthBuffers[ColorBufferWriteIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
			renderBackend->TransitionTexture(PathTracingSpecularHitDistanceBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
			renderBackend->TransitionTexture(PathTracingSpecularMotionVectorBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
		}
	};

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
	glm::vec3 currentLightDir = RenderFrameNormalizedLightDir;
	bool lightDirChanged = glm::length(currentLightDir - PrevPathTracingLightDir) > 0.0001f;
	bool lightIntensityChanged = abs(LightIntensity - PrevPathTracingLightIntensity) > 0.0001f;
	bool lightCastShadowChanged = RenderFrameDirectionalLightCastShadow != PrevPathTracingDirectionalLightCastShadow;
	bool pointLightsChanged = currentPointLightStateHash != PrevPathTracingPointLightStateHash;
	
	// Check if sky color changed
	bool skyColorChanged = glm::length(SkyColorTop - PrevSkyColorTop) > 0.0001f ||
	                       glm::length(SkyColorBottom - PrevSkyColorBottom) > 0.0001f ||
	                       abs(SkyIntensity - PrevSkyIntensity) > 0.0001f;
	const bool bStabilizePrimaryRaySamples =
		bWritePrimaryGBuffer &&
		bEnablePathTracingRRPrimaryRayStabilization &&
		cameraChanged;
	
	if (cameraChanged || lightDirChanged || lightIntensityChanged || lightCastShadowChanged || pointLightsChanged || skyColorChanged)
	{
		PathTracingAccumulatedFrames = 0;
#if WITH_STREAMLINE
		if (bWritePrimaryGBuffer && (lightDirChanged || lightIntensityChanged || lightCastShadowChanged || pointLightsChanged || skyColorChanged))
			bDLSSResetNeeded = true;
#endif
		if (pointLightsChanged)
		{
			AppendCpuRuntimeTrace(
				L"[PathTracing] point light state changed, count=" +
				std::to_wstring(PathTracingViewParam.PointLightCount));
		}
		PrevPathTracingViewMat = ViewMat;
		PrevPathTracingLightDir = currentLightDir;
		PrevPathTracingLightIntensity = LightIntensity;
		PrevPathTracingDirectionalLightCastShadow = RenderFrameDirectionalLightCastShadow;
		PrevPathTracingPointLightStateHash = currentPointLightStateHash;
		PrevSkyColorTop = SkyColorTop;
		PrevSkyColorBottom = SkyColorBottom;
		PrevSkyIntensity = SkyIntensity;
		
		// Note: Buffer will be cleared in shader when FrameCounter == 0
	}

	PathTracingViewParam.ViewMatrix = glm::transpose(ViewMat);
	PathTracingViewParam.InvViewMatrix = glm::transpose(InvViewMat);
	PathTracingViewParam.ProjMatrix = glm::transpose(UnjitteredProjMat);
	PathTracingViewParam.InvProjMatrix = glm::transpose(UnjitteredInvProjMat);
	PathTracingViewParam.UnjitteredViewProjMatrix = glm::transpose(UnjitteredViewProjMat);
	PathTracingViewParam.PrevUnjitteredViewProjMatrix = glm::transpose(PrevUnjitteredViewProjMat);
	PathTracingViewParam.ProjectionParams = FrameProjectionParams;
	PathTracingViewParam.LightDirAndIntensity = glm::vec4(RenderFrameNormalizedLightDir, LightIntensity);
	PathTracingViewParam.DirectLightAngularRadius = RTShadowViewParam.ShadowLightRadius;
	PathTracingViewParam.DirectLightSampleCount = std::clamp(PathTracingViewParam.DirectLightSampleCount, 1u, 8u);
	PathTracingViewParam.PointLightSampleCount = std::clamp(PathTracingViewParam.PointLightSampleCount, 0u, MaxPathTracingPointLights);
	PathTracingViewParam.bDirectLightCastShadow = RenderFrameDirectionalLightCastShadow ? 1u : 0u;
	PathTracingViewParam.RandomOffset = glm::vec2(RenderFrameShaderTime, RenderFrameShaderTime);
	PathTracingViewParam.FrameCounter = PathTracingViewParam.DebugMode == 0 ? PathTracingAccumulatedFrames : 0u;
	PathTracingViewParam.BlueNoiseOffsetStride = PathTracingViewParam.DebugMode == 0 ? RenderFrameIndex : 0u;
	PathTracingViewParam.ViewSpreadAngle = glm::tan(Fov * 0.5f) / (0.5f * m_height);
	PathTracingViewParam.SkyColorTop = SkyColorTop;
	PathTracingViewParam.SkyIntensity = SkyIntensity;
	PathTracingViewParam.SkyColorBottom = SkyColorBottom;
	PathTracingViewParam.LightColor = RenderFrameLightColor;
	PathTracingViewParam.bEnableDiffuseGI = bEnableDiffuseGI ? 1u : 0u;
	PathTracingViewParam.bEnableSpecularGI = bEnableSpecularGI ? 1u : 0u;
	PathTracingViewParam.bEnableDirectDiffuse = bEnableDirectDiffuse ? 1u : 0u;
	PathTracingViewParam.bEnableDirectSpecular = bEnableDirectSpecular ? 1u : 0u;
	PathTracingViewParam.bEnableRTAO = 0u;
	PathTracingViewParam.bWritePrimaryGBuffer = bWritePrimaryGBuffer ? 1u : 0u;
	PathTracingViewParam.SpecularMotionVectorScale = PathTracingRRSpecularMotionVectorScale;
	PathTracingViewParam.bStabilizePrimaryRaySamples = bStabilizePrimaryRaySamples ? 1u : 0u;
	const UINT32 targetSamplesPerPixel = std::clamp(PathTracingViewParam.SamplesPerPixel, 1u, 16u);
	UINT32 dispatchSamplesPerPixel = targetSamplesPerPixel;
	if (PathTracingViewParam.DebugMode == 0 && targetSamplesPerPixel > 1u && !bWritePrimaryGBuffer)
	{
		if (PathTracingAccumulatedFrames == 0u)
			dispatchSamplesPerPixel = 1u;
		else if (PathTracingAccumulatedFrames < 8u)
			dispatchSamplesPerPixel = std::min(targetSamplesPerPixel, 2u);
	}
	else if (PathTracingViewParam.DebugMode == 0 && bWritePrimaryGBuffer && targetSamplesPerPixel > 1u &&
		!cameraChanged && !lightDirChanged && !lightIntensityChanged &&
		!lightCastShadowChanged && !pointLightsChanged && !skyColorChanged)
	{
		// RR keeps history when the camera is still. If the user asks for more
		// than 1 spp, ramp toward that target only after the history is stable;
		// sample 0 still owns the primary-hit GBuffer writes.
		dispatchSamplesPerPixel = 1u;
		if (PathTracingAccumulatedFrames >= 8u)
			dispatchSamplesPerPixel = targetSamplesPerPixel;
		else if (PathTracingAccumulatedFrames >= 2u)
			dispatchSamplesPerPixel = std::min(targetSamplesPerPixel, 2u);
	}
	else if (PathTracingViewParam.DebugMode == 0 && bWritePrimaryGBuffer && targetSamplesPerPixel > 1u)
	{
		dispatchSamplesPerPixel = 1u;
	}
	PathTracingLastDispatchSamplesPerPixel = dispatchSamplesPerPixel;

	PathTracingViewParamCB dispatchViewParam = PathTracingViewParam;
	dispatchViewParam.SamplesPerPixel = dispatchSamplesPerPixel;

	if (bEnablePathTracingCompaction)
	{
		if (PathTracingCompactionPass(outputColor, dispatchViewParam, bWritePrimaryGBuffer))
		{
			if (PathTracingViewParam.DebugMode == 0)
				PathTracingAccumulatedFrames++;
			transitionPathTracingOutputsToShaderRead();
			return;
		}
	}
	else
	{
		bPathTracingCompactionFallbackLogged = false;
	}

	RTPassBuilder pass(*this, PSO_PATH_TRACING);
	pass.BeginScene()
		.SetTextureUAV("global", "OutputColor", outputColor)
		.SetTextureUAV("global", "OutAlbedo", AlbedoBuffer.get())
		.SetTextureUAV("global", "OutSpecularAlbedo", SpecularAlbedoBuffer.get())
		.SetTextureUAV("global", "OutNormal", NormalBuffers[ColorBufferWriteIndex].get())
		.SetTextureUAV("global", "OutGeomNormal", GeomNormalBuffers[ColorBufferWriteIndex].get())
		.SetTextureUAV("global", "OutVelocity", VelocityBuffer.get())
		.SetTextureUAV("global", "OutRoughnessMetallic", RoughnessMetalicBuffer.get())
		.SetTextureUAV("global", "OutDepth", UnjitteredDepthBuffers[ColorBufferWriteIndex].get())
		.SetTextureUAV("global", "OutSpecularHitDistance", PathTracingSpecularHitDistanceBuffer.get())
		.SetTextureUAV("global", "OutSpecularMotionVector", PathTracingSpecularMotionVectorBuffer.get())
		.SetAccelerationStructure("global", "gRtScene", TLAS)
		.SetBufferSRV("global", "PointLightBuffer", PathTracingPointLightBuffer.get())
		.SetCBVValue("global", "ViewParameter", &dispatchViewParam)
		.SetSampler("global", "sampleWrap", samplerWrap.get());
	if (bUseBindlessMaterials)
	{
		pass.SetBindlessTextureTable("global", "MaterialTextures")
			.SetBufferSRV("global", "RtMaterials", RTMaterialRecordBuffer.get());
	}

	RTSceneHitProgramDesc hitProgramDesc;
	hitProgramDesc.bBindDiffuseTexture = false;
	hitProgramDesc.bBindInstancePropertyBeforeDiffuse = true;
	if (bUseBindlessMaterials)
	{
		pass.BindSceneHitPrograms(hitProgramDesc);
	}
	else
	{
		pass.BindSceneHitPrograms(hitProgramDesc, [&pass](RTPipelineStateObject& pso, const RTSceneHitProgramDesc& desc, Mesh& mesh, uint32_t instanceIndex)
		{
			pso.AddTextureSRVToHitProgram(desc.HitGroup, pass.GetDiffuseTexture(mesh), instanceIndex);
			pso.AddTextureSRVToHitProgram(desc.HitGroup, pass.GetNormalTexture(mesh), instanceIndex);
			pso.AddTextureSRVToHitProgram(desc.HitGroup, pass.GetRoughnessTexture(mesh), instanceIndex);
			pso.AddTextureSRVToHitProgram(desc.HitGroup, pass.GetMetallicTexture(mesh), instanceIndex);
		});
	}
	pass.Dispatch(m_width, m_height);

	if (PathTracingViewParam.DebugMode == 0)
	{
		PathTracingAccumulatedFrames++;
	}

	// Transition output buffer back to SRV
	transitionPathTracingOutputsToShaderRead();
}
