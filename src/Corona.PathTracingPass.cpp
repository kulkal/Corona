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

#include <algorithm>
#include <cmath>
#include <cstring>
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
	constexpr uint32_t kRTMaterialDrawRangeBufferRegisterSpace = 15;
	constexpr UINT32 kRTMaterialFlagAlphaBlend = 1u << 0;
	constexpr float kTranslucentVolumeDensityWorldScale = 32.0f;

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

	constexpr UINT32 kMaterialRecordValidationFrameInterval = 120u;
	if (RTMaterialRecordBuffer &&
		RTMaterialDrawRangeRecordBuffer &&
		RTMaterialRecordHash != 0 &&
		RTMaterialRecordBackend == renderBackend.get() &&
		RTMaterialRecordInstanceRevision == RayTracingInstancesRevision &&
		static_cast<UINT32>(FrameCounter - RTMaterialRecordValidatedFrameCounter) < kMaterialRecordValidationFrameInterval)
	{
		return true;
	}

	auto getPrimaryMaterial = [](Mesh& mesh) -> Material*
	{
		if (!mesh.Draws.empty() && mesh.Draws[0].mat)
			return mesh.Draws[0].mat.get();
		return mesh.Mat.get();
	};

	uint64_t sourceHash = 1469598103934665603ull;
	HashCombinePathTracingMaterial(sourceHash, static_cast<uint64_t>(RayTracingInstances.size()));
	for (const RTInstanceDesc& instance : RayTracingInstances)
	{
		Mesh* mesh = instance.BottomLevelAS ? instance.BottomLevelAS->MeshPtr : nullptr;
		HashCombinePathTracingMaterial(sourceHash, reinterpret_cast<uintptr_t>(mesh));
		if (!mesh)
			continue;

		Material* material = getPrimaryMaterial(*mesh);
		HashCombinePathTracingMaterial(sourceHash, reinterpret_cast<uintptr_t>(material));
		auto hashMaterial = [&](Material* hashMaterialPtr)
		{
			HashCombinePathTracingMaterial(sourceHash, reinterpret_cast<uintptr_t>(hashMaterialPtr));
			if (!hashMaterialPtr)
				return;
			HashCombinePathTracingMaterial(sourceHash, reinterpret_cast<uintptr_t>(hashMaterialPtr->Diffuse.get()));
			HashCombinePathTracingMaterial(sourceHash, reinterpret_cast<uintptr_t>(hashMaterialPtr->Normal.get()));
			HashCombinePathTracingMaterial(sourceHash, reinterpret_cast<uintptr_t>(hashMaterialPtr->Roughness.get()));
			HashCombinePathTracingMaterial(sourceHash, reinterpret_cast<uintptr_t>(hashMaterialPtr->Metallic.get()));
			uint32_t baseColorBits[4] = {};
			std::memcpy(baseColorBits, &hashMaterialPtr->BaseColorFactor, sizeof(baseColorBits));
			for (uint32_t bits : baseColorBits)
				HashCombinePathTracingMaterial(sourceHash, bits);
			HashCombinePathTracingMaterial(sourceHash, hashMaterialPtr->bAlphaBlend ? 1u : 0u);
		};
		hashMaterial(material);
		HashCombinePathTracingMaterial(sourceHash, static_cast<uint64_t>(mesh->Draws.size()));
		for (const Mesh::DrawCall& draw : mesh->Draws)
		{
			HashCombinePathTracingMaterial(sourceHash, draw.IndexStart);
			HashCombinePathTracingMaterial(sourceHash, draw.IndexCount);
			hashMaterial(draw.mat ? draw.mat.get() : material);
		}
	}

	if (RTMaterialRecordBuffer &&
		RTMaterialDrawRangeRecordBuffer &&
		RTMaterialRecordHash != 0 &&
		RTMaterialRecordSourceHash == sourceHash &&
		RTMaterialRecordBackend == renderBackend.get())
	{
		RTMaterialRecordInstanceRevision = RayTracingInstancesRevision;
		RTMaterialRecordValidatedFrameCounter = FrameCounter;
		return true;
	}

	auto getBindlessTextureIndex = [&](Texture* texture) -> UINT32
	{
		if (!texture)
			return RHI_INVALID_BINDLESS_INDEX;

		RHITextureHandle handle = renderBackend->RegisterBindlessTexture(texture);
		if (!handle.IsValid())
			handle = renderBackend->GetBindlessTextureHandle(texture);
		return handle.IsValid() ? handle.Index : RHI_INVALID_BINDLESS_INDEX;
	};

	auto buildRecordForMaterial = [&](Material* material, bool& bAllTexturesRegistered) -> RTMaterialRecord
	{
		Texture* albedo = DefaultWhiteTex.get();
		Texture* normal = DefaultNormalTex.get();
		Texture* roughness = DefaultRougnessTex.get();
		Texture* metallic = DefaultBlackTex.get();
		glm::vec4 baseColorFactor(1.0f);
		if (material)
		{
			baseColorFactor = material->BaseColorFactor;
			if (material->Diffuse)
				albedo = material->Diffuse.get();
			if (material->Normal)
				normal = material->Normal.get();
			if (material->Roughness)
				roughness = material->Roughness.get();
			if (material->Metallic)
				metallic = material->Metallic.get();
		}

		RTMaterialRecord record;
		record.AlbedoTextureIndex = getBindlessTextureIndex(albedo);
		record.NormalTextureIndex = getBindlessTextureIndex(normal);
		record.RoughnessTextureIndex = getBindlessTextureIndex(roughness);
		record.MetallicTextureIndex = getBindlessTextureIndex(metallic);
		const uint32_t albedoTexels = std::max<uint32_t>(1u, albedo->Width) * std::max<uint32_t>(1u, albedo->Height);
		record.AlbedoLodConstant = 0.5f * std::log2(static_cast<float>(albedoTexels));
		record.BaseColorFactor[0] = baseColorFactor.r;
		record.BaseColorFactor[1] = baseColorFactor.g;
		record.BaseColorFactor[2] = baseColorFactor.b;
		record.BaseColorFactor[3] = baseColorFactor.a;
		record.Flags = (material && material->bAlphaBlend) || baseColorFactor.a < 0.999f ? kRTMaterialFlagAlphaBlend : 0u;
		bAllTexturesRegistered = bAllTexturesRegistered &&
			record.AlbedoTextureIndex != RHI_INVALID_BINDLESS_INDEX &&
			record.NormalTextureIndex != RHI_INVALID_BINDLESS_INDEX &&
			record.RoughnessTextureIndex != RHI_INVALID_BINDLESS_INDEX &&
			record.MetallicTextureIndex != RHI_INVALID_BINDLESS_INDEX;
		return record;
	};

	const size_t instanceRecordCount = std::max<size_t>(RayTracingInstances.size(), 1);
	std::vector<RTMaterialRecord> records(instanceRecordCount);
	std::vector<RTMaterialDrawRangeRecord> drawRanges;

	bool bAllTexturesRegistered = true;
	for (size_t instanceIndex = 0; instanceIndex < RayTracingInstances.size(); ++instanceIndex)
	{
		const RTInstanceDesc& instance = RayTracingInstances[instanceIndex];
		Mesh* mesh = instance.BottomLevelAS ? instance.BottomLevelAS->MeshPtr : nullptr;

		Material* primaryMaterial = nullptr;
		if (mesh)
			primaryMaterial = getPrimaryMaterial(*mesh);

		records[instanceIndex] = buildRecordForMaterial(primaryMaterial, bAllTexturesRegistered);

		if (!mesh)
			continue;

		for (const Mesh::DrawCall& draw : mesh->Draws)
		{
			if (draw.IndexCount == 0)
				continue;
			Material* drawMaterial = draw.mat ? draw.mat.get() : primaryMaterial;
			UINT32 materialRecordIndex = static_cast<UINT32>(instanceIndex);
			if (drawMaterial != primaryMaterial)
			{
				materialRecordIndex = static_cast<UINT32>(records.size());
				records.push_back(buildRecordForMaterial(drawMaterial, bAllTexturesRegistered));
			}

			RTMaterialDrawRangeRecord range;
			const UINT32 primitiveStart = draw.IndexStart / 3u;
			const UINT32 primitiveEnd = (draw.IndexStart + draw.IndexCount + 2u) / 3u;
			range.PrimitiveStart = primitiveStart;
			range.PrimitiveCount = primitiveEnd > primitiveStart ? primitiveEnd - primitiveStart : 0u;
			range.MaterialRecordIndex = materialRecordIndex;
			range.VertexBase = static_cast<INT32>(draw.VertexBase);
			if (range.PrimitiveCount != 0)
				drawRanges.push_back(range);
		}
	}
	if (drawRanges.empty())
		drawRanges.push_back({});

	uint64_t materialHash = 1469598103934665603ull;
	HashCombinePathTracingMaterial(materialHash, static_cast<uint64_t>(records.size()));
	for (const RTMaterialRecord& record : records)
	{
		HashCombinePathTracingMaterial(materialHash, record.AlbedoTextureIndex);
		HashCombinePathTracingMaterial(materialHash, record.NormalTextureIndex);
		HashCombinePathTracingMaterial(materialHash, record.RoughnessTextureIndex);
		HashCombinePathTracingMaterial(materialHash, record.MetallicTextureIndex);
		uint32_t baseColorBits[4] = {};
		std::memcpy(baseColorBits, record.BaseColorFactor, sizeof(baseColorBits));
		for (uint32_t bits : baseColorBits)
			HashCombinePathTracingMaterial(materialHash, bits);
		uint32_t lodBits = 0;
		std::memcpy(&lodBits, &record.AlbedoLodConstant, sizeof(lodBits));
		HashCombinePathTracingMaterial(materialHash, lodBits);
		HashCombinePathTracingMaterial(materialHash, record.Flags);
	}
	HashCombinePathTracingMaterial(materialHash, static_cast<uint64_t>(drawRanges.size()));
	for (const RTMaterialDrawRangeRecord& range : drawRanges)
	{
		HashCombinePathTracingMaterial(materialHash, range.PrimitiveStart);
		HashCombinePathTracingMaterial(materialHash, range.PrimitiveCount);
		HashCombinePathTracingMaterial(materialHash, range.MaterialRecordIndex);
		HashCombinePathTracingMaterial(materialHash, static_cast<uint32_t>(range.VertexBase));
	}

	if (RTMaterialRecordBuffer && RTMaterialDrawRangeRecordBuffer && RTMaterialRecordHash == materialHash)
	{
		RTMaterialRecordSourceHash = sourceHash;
		RTMaterialRecordInstanceRevision = RayTracingInstancesRevision;
		RTMaterialRecordValidatedFrameCounter = FrameCounter;
		RTMaterialRecordBackend = renderBackend.get();
		return true;
	}

	if (!bAllTexturesRegistered)
	{
		RTMaterialRecordHash = 0;
		RTMaterialRecordSourceHash = 0;
		RTMaterialRecordBackend = nullptr;
		RTMaterialDrawRangeRecordBuffer.reset();
		AppendCpuRuntimeTrace(L"[RTMaterial] bindless material texture registration failed");
		return false;
	}

	BufferCreateDesc desc = {};
	desc.NumElements = static_cast<uint32_t>(records.size());
	desc.ElementSize = sizeof(RTMaterialRecord);
	desc.InitialState = EInitialResourceState::ShaderRead;
	desc.InitialData = records.data();
	desc.Shape = EBufferShape::Structured;
	desc.Access = EBufferAccess::GpuOnly;
	desc.AllocationPolicy = EBufferAllocationPolicy::Suballocated;

	RTMaterialRecordBuffer = renderBackend->CreateBuffer(desc);
	if (!RTMaterialRecordBuffer)
	{
		RTMaterialRecordHash = 0;
		RTMaterialRecordSourceHash = 0;
		RTMaterialRecordBackend = nullptr;
		RTMaterialDrawRangeRecordBuffer.reset();
		AppendCpuRuntimeTrace(L"[RTMaterial] failed to create bindless material record buffer");
		return false;
	}

	BufferCreateDesc drawRangeDesc = {};
	drawRangeDesc.NumElements = static_cast<uint32_t>(drawRanges.size());
	drawRangeDesc.ElementSize = sizeof(RTMaterialDrawRangeRecord);
	drawRangeDesc.InitialState = EInitialResourceState::ShaderRead;
	drawRangeDesc.InitialData = drawRanges.data();
	drawRangeDesc.Shape = EBufferShape::Structured;
	drawRangeDesc.Access = EBufferAccess::GpuOnly;
	drawRangeDesc.AllocationPolicy = EBufferAllocationPolicy::Suballocated;

	RTMaterialDrawRangeRecordBuffer = renderBackend->CreateBuffer(drawRangeDesc);
	if (!RTMaterialDrawRangeRecordBuffer)
	{
		RTMaterialRecordHash = 0;
		RTMaterialRecordSourceHash = 0;
		RTMaterialRecordBackend = nullptr;
		RTMaterialRecordBuffer.reset();
		AppendCpuRuntimeTrace(L"[RTMaterial] failed to create material draw range buffer");
		return false;
	}

	RTMaterialRecordHash = materialHash;
	RTMaterialRecordSourceHash = sourceHash;
	RTMaterialRecordInstanceRevision = RayTracingInstancesRevision;
	RTMaterialRecordValidatedFrameCounter = FrameCounter;
	RTMaterialRecordBackend = renderBackend.get();
	AppendCpuRuntimeTrace(
		L"[RTMaterial] bindless material records uploaded, count=" +
		std::to_wstring(records.size()) +
		L", drawRanges=" +
		std::to_wstring(drawRanges.size()));
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

	UINT32 drawRangeOffset = 0;
	for (size_t instanceIndex = 0; instanceIndex < RayTracingInstances.size(); ++instanceIndex)
	{
		const RTInstanceDesc& instance = RayTracingInstances[instanceIndex];
		Mesh* mesh = instance.BottomLevelAS ? instance.BottomLevelAS->MeshPtr : nullptr;
		if (!mesh)
			continue;

		RTGeometryRecord& record = records[instanceIndex];
		record.VertexBufferIndex = getBindlessVertexBufferIndex(mesh->Vb.get());
		record.IndexBufferIndex = getBindlessIndexBufferIndex(mesh->Ib.get());
		record.DrawRangeOffset = drawRangeOffset;
		for (const Mesh::DrawCall& draw : mesh->Draws)
		{
			if (draw.IndexCount != 0)
				++record.DrawRangeCount;
		}
		drawRangeOffset += record.DrawRangeCount;
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
		HashCombinePathTracingMaterial(geometryHash, record.DrawRangeOffset);
		HashCombinePathTracingMaterial(geometryHash, record.DrawRangeCount);
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
	desc.Access = EBufferAccess::GpuOnly;
	desc.AllocationPolicy = EBufferAllocationPolicy::Suballocated;

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
	desc.Access = EBufferAccess::GpuOnly;
	desc.AllocationPolicy = EBufferAllocationPolicy::Suballocated;

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
	TEMP_PSO_PATH_TRACING->BindSRV(
		"global",
		MakeRHIBufferSRV(
			"RtMaterialDrawRanges",
			0,
			materialStage,
			RHIBufferViewKind::Structured,
			1,
			kRTMaterialDrawRangeBufferRegisterSpace));
	TEMP_PSO_PATH_TRACING->BindCBV("global", MakeRHICBV("ViewParameter", 0, sizeof(PathTracingViewParam), rayGenStage));
	TEMP_PSO_PATH_TRACING->BindSampler("global", MakeRHISampler("sampleWrap", 0, rayGenStage | closestHitStage | anyHitStage));

	TEMP_PSO_PATH_TRACING->AddShader("PathTracingMiss", RTPipelineStateObject::MISS);
	TEMP_PSO_PATH_TRACING->AddShader("ShadowMiss", RTPipelineStateObject::MISS);

	TEMP_PSO_PATH_TRACING->AddShader("PathTracingClosestHit", RTPipelineStateObject::HIT);

	TEMP_PSO_PATH_TRACING->AddShader("PathTracingAnyHit", RTPipelineStateObject::ANYHIT);
	TEMP_PSO_PATH_TRACING->Configure(8, 256, sizeof(float) * 2);

	bool bSuccess = TEMP_PSO_PATH_TRACING->InitRS("Shaders\\PathTracing.hlsl");

	if (bSuccess)
	{
		PSO_PATH_TRACING = TEMP_PSO_PATH_TRACING;
	}
}

bool Corona::RaytracePrimaryGBufferPass()
{
	if (!renderBackend || RenderingMode != ERenderingMode::HYBRID)
		return false;
	if (!renderBackend->SupportsRayTracing() || !TLAS)
		return false;
	const UINT nextColorBufferWriteIndex = 1 - ColorBufferWriteIndex;
	if (!AlbedoBuffer ||
		!SpecularAlbedoBuffer ||
		!NormalBuffers[nextColorBufferWriteIndex] ||
		!GeomNormalBuffers[nextColorBufferWriteIndex] ||
		!VelocityBuffer ||
		!RoughnessMetalicBuffer ||
		!UnjitteredDepthBuffers[nextColorBufferWriteIndex] ||
		!PathTracingSpecularHitDistanceBuffer ||
		!PathTracingSpecularMotionVectorBuffer ||
		!PathTracingAccumBuffer[PathTracingWriteIndex])
	{
		return false;
	}
	if (!UsesRTBindlessMaterials() || !UsesRTBindlessGeometry())
		return false;

	if (!PSO_PATH_TRACING)
	{
		InitPathTracingPass();
		if (!PSO_PATH_TRACING)
			return false;
	}

	renderBackend->EmitGpuCrashMarker("RTPrimaryGBufferPass");
	renderBackend->BindDefaultDescriptorHeaps();
	if (RenderWorld.bSceneObjectCullingIndexDirty ||
		RenderWorld.SceneObjectCullingData.size() != RenderWorld.SceneObjects.size())
	{
		RebuildRenderWorldCullingIndex();
	}
	DispatchSpineSkinningForRenderWorld();
	BeginGpuPassTiming(EGpuPass::SkeletalSkinning);
	DispatchSkeletalSkinningForRenderWorld();
	EndGpuPassTiming(EGpuPass::SkeletalSkinning);

	ApplyRenderPointLightsToFrameParams();
	const UINT32 pointLightStateHash = ComputePathTracingPointLightStateHash();
	if (!EnsurePathTracingPointLightBuffer(pointLightStateHash))
		return false;
	if (!EnsureRTMaterialRecordBuffer())
		return false;

	bGBufferDepthPrepassMainPassActive = false;
	GBufferLastTotalObjectCount = static_cast<uint64_t>(RenderWorld.SceneObjects.size());
	GBufferLastVisibleObjectCount = static_cast<uint64_t>(RayTracingInstances.size());
	GBufferLastFrustumCulledObjectCount = 0;
	GBufferLastOcclusionCulledObjectCount = 0;
	GBufferLastStaticInstancedBatchCount = 0;
	GBufferLastStaticInstancedObjectCount = 0;
	GBufferLastStaticInstancedDrawCount = 0;
	GBufferLastBindlessObjectBatchCount = 0;
	GBufferLastBindlessObjectCount = 0;
	GBufferLastBindlessObjectDrawCount = 0;
	GBufferLastDepthPrepassCandidateObjectCount = 0;
	GBufferLastDepthPrepassSelectedObjectCount = 0;
	GBufferLastDepthPrepassOpaqueDrawCount = 0;
	GBufferLastSpatialCellCount = 0;
	GBufferLastSpatialVisibleCellCount = 0;
	GBufferLastSpatialPartialCellCount = 0;
	GBufferLastSpatialCandidateObjectCount = 0;
	GBufferLastSpatialVisibleObjectCount = 0;

	ColorBufferWriteIndex = nextColorBufferWriteIndex;
	if (DepthBuffer)
	{
		renderBackend->TransitionTexture(DepthBuffer.get(), EResourceState::ShaderRead, EResourceState::DepthWrite);
		renderBackend->ClearDepth(DepthBuffer.get(), 1.0f);
		renderBackend->TransitionTexture(DepthBuffer.get(), EResourceState::DepthWrite, EResourceState::ShaderRead);
	}

	PathTracingViewParamCB dispatchViewParam = PathTracingViewParam;
	dispatchViewParam.ViewMatrix = glm::transpose(ViewMat);
	dispatchViewParam.InvViewMatrix = glm::transpose(InvViewMat);
	// RT primary builds raw RR input with a single explicitly jittered ray per
	// pixel. Keep the projection unjittered here and pass the same pixel jitter
	// that Streamline receives so the raygen sample position owns the jitter.
	dispatchViewParam.ProjMatrix = glm::transpose(UnjitteredProjMat);
	dispatchViewParam.InvProjMatrix = glm::transpose(UnjitteredInvProjMat);
	dispatchViewParam.UnjitteredViewProjMatrix = glm::transpose(UnjitteredViewProjMat);
	dispatchViewParam.PrevUnjitteredViewProjMatrix = glm::transpose(PrevUnjitteredViewProjMat);
	dispatchViewParam.ProjectionParams = FrameProjectionParams;
	dispatchViewParam.LightDirAndIntensity = glm::vec4(RenderFrameNormalizedLightDir, LightIntensity);
	dispatchViewParam.DirectLightAngularRadius = RTShadowViewParam.ShadowLightRadius;
	dispatchViewParam.DirectLightSampleCount = 1u;
	dispatchViewParam.PointLightSampleCount = 0u;
	dispatchViewParam.bDirectLightCastShadow = 0u;
	dispatchViewParam.RandomOffset = CurrentJitter * 0.5f;
	dispatchViewParam.FrameCounter = 0u;
	dispatchViewParam.BlueNoiseOffsetStride = RenderFrameIndex;
	dispatchViewParam.MaxBounces = 1u;
	dispatchViewParam.SamplesPerPixel = 1u;
	dispatchViewParam.ViewSpreadAngle = glm::tan(Fov * 0.5f) / (0.5f * GetRenderHeight());
	dispatchViewParam.DebugMode = 0u;
	dispatchViewParam.SkyColorTop = SkyColorTop;
	dispatchViewParam.SkyIntensity = SkyIntensity;
	dispatchViewParam.SkyColorBottom = SkyColorBottom;
	dispatchViewParam.LightColor = RenderFrameLightColor;
	dispatchViewParam.bEnableDiffuseGI = 0u;
	dispatchViewParam.bEnableSpecularGI = 0u;
	dispatchViewParam.bEnableDirectDiffuse = 0u;
	dispatchViewParam.bEnableDirectSpecular = 0u;
	dispatchViewParam.bEnableRTAO = 0u;
	dispatchViewParam.bWritePrimaryGBuffer = 1u;
	dispatchViewParam.SpecularMotionVectorScale = PathTracingRRSpecularMotionVectorScale;
	dispatchViewParam.bStabilizePrimaryRaySamples = 1u;
	dispatchViewParam.bPrimaryGBufferOnly = 1u;
	dispatchViewParam.PointLightCount = 0u;

	RenderGraph rg(renderBackend.get());
	RGTextureRef outputColorTarget = rg.ImportTexture(
		"RTPrimaryGBuffer.OutputColor",
		PathTracingAccumBuffer[PathTracingWriteIndex].get(),
		EResourceState::ShaderRead);
	RGTextureRef outAlbedo = rg.ImportTexture("RTPrimaryGBuffer.OutAlbedo", AlbedoBuffer.get(), EResourceState::ShaderRead);
	RGTextureRef outSpecularAlbedo = rg.ImportTexture("RTPrimaryGBuffer.OutSpecularAlbedo", SpecularAlbedoBuffer.get(), EResourceState::ShaderRead);
	RGTextureRef outNormal = rg.ImportTexture("RTPrimaryGBuffer.OutNormal", NormalBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead);
	RGTextureRef outGeomNormal = rg.ImportTexture("RTPrimaryGBuffer.OutGeomNormal", GeomNormalBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead);
	RGTextureRef outVelocity = rg.ImportTexture("RTPrimaryGBuffer.OutVelocity", VelocityBuffer.get(), EResourceState::ShaderRead);
	RGTextureRef outRoughnessMetallic = rg.ImportTexture("RTPrimaryGBuffer.OutRoughnessMetallic", RoughnessMetalicBuffer.get(), EResourceState::ShaderRead);
	RGTextureRef outDepth = rg.ImportTexture("RTPrimaryGBuffer.OutDepth", UnjitteredDepthBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead);
	RGTextureRef outSpecularHitDistance = rg.ImportTexture("RTPrimaryGBuffer.OutSpecularHitDistance", PathTracingSpecularHitDistanceBuffer.get(), EResourceState::ShaderRead);
	RGTextureRef outSpecularMotionVector = rg.ImportTexture("RTPrimaryGBuffer.OutSpecularMotionVector", PathTracingSpecularMotionVectorBuffer.get(), EResourceState::ShaderRead);
	RGBufferRef pointLightBuffer = rg.ImportBuffer("RTPrimaryGBuffer.PointLightBuffer", PathTracingPointLightBuffer.get(), EResourceState::ShaderRead);
	RGBufferRef rtMaterials = rg.ImportBuffer("RTPrimaryGBuffer.RtMaterials", RTMaterialRecordBuffer.get(), EResourceState::ShaderRead);
	RGBufferRef rtMaterialDrawRanges = rg.ImportBuffer("RTPrimaryGBuffer.RtMaterialDrawRanges", RTMaterialDrawRangeRecordBuffer.get(), EResourceState::ShaderRead);

	rg.ExportTexture(outputColorTarget, EResourceState::ShaderRead);
	rg.ExportTexture(outAlbedo, EResourceState::ShaderRead);
	rg.ExportTexture(outSpecularAlbedo, EResourceState::ShaderRead);
	rg.ExportTexture(outNormal, EResourceState::ShaderRead);
	rg.ExportTexture(outGeomNormal, EResourceState::ShaderRead);
	rg.ExportTexture(outVelocity, EResourceState::ShaderRead);
	rg.ExportTexture(outRoughnessMetallic, EResourceState::ShaderRead);
	rg.ExportTexture(outDepth, EResourceState::ShaderRead);
	rg.ExportTexture(outSpecularHitDistance, EResourceState::ShaderRead);
	rg.ExportTexture(outSpecularMotionVector, EResourceState::ShaderRead);

	rg.AddPass(
		"RTPrimaryGBufferPass",
		ERGPassFlags::RayTracing,
		[&](RGPassBuilder& builder)
		{
			builder.ReadWriteTexture(outputColorTarget, EResourceState::UnorderedAccess)
				.ReadWriteTexture(outAlbedo, EResourceState::UnorderedAccess)
				.ReadWriteTexture(outSpecularAlbedo, EResourceState::UnorderedAccess)
				.ReadWriteTexture(outNormal, EResourceState::UnorderedAccess)
				.ReadWriteTexture(outGeomNormal, EResourceState::UnorderedAccess)
				.ReadWriteTexture(outVelocity, EResourceState::UnorderedAccess)
				.ReadWriteTexture(outRoughnessMetallic, EResourceState::UnorderedAccess)
				.ReadWriteTexture(outDepth, EResourceState::UnorderedAccess)
				.ReadWriteTexture(outSpecularHitDistance, EResourceState::UnorderedAccess)
				.ReadWriteTexture(outSpecularMotionVector, EResourceState::UnorderedAccess)
				.ReadBuffer(pointLightBuffer, EResourceState::ShaderRead)
				.ReadBuffer(rtMaterials, EResourceState::ShaderRead)
				.ReadBuffer(rtMaterialDrawRanges, EResourceState::ShaderRead);
		},
		[&](RGContext& ctx)
		{
			RTPassBuilder pass(*this, PSO_PATH_TRACING, ERtProfilePass::PathTracing);
			pass.BeginScene()
				.SetTextureUAV("global", "OutputColor", ctx.GetTexture(outputColorTarget))
				.SetTextureUAV("global", "OutAlbedo", ctx.GetTexture(outAlbedo))
				.SetTextureUAV("global", "OutSpecularAlbedo", ctx.GetTexture(outSpecularAlbedo))
				.SetTextureUAV("global", "OutNormal", ctx.GetTexture(outNormal))
				.SetTextureUAV("global", "OutGeomNormal", ctx.GetTexture(outGeomNormal))
				.SetTextureUAV("global", "OutVelocity", ctx.GetTexture(outVelocity))
				.SetTextureUAV("global", "OutRoughnessMetallic", ctx.GetTexture(outRoughnessMetallic))
				.SetTextureUAV("global", "OutDepth", ctx.GetTexture(outDepth))
				.SetTextureUAV("global", "OutSpecularHitDistance", ctx.GetTexture(outSpecularHitDistance))
				.SetTextureUAV("global", "OutSpecularMotionVector", ctx.GetTexture(outSpecularMotionVector))
				.SetAccelerationStructure("global", "gRtScene", TLAS)
				.SetBufferSRV("global", "PointLightBuffer", ctx.GetBuffer(pointLightBuffer))
				.SetCBVValue("global", "ViewParameter", &dispatchViewParam)
				.SetSampler("global", "sampleWrap", samplerWrap.get());
			pass.SetBindlessTextureTable("global", "MaterialTextures")
				.SetBufferSRV("global", "RtMaterials", ctx.GetBuffer(rtMaterials))
				.SetBufferSRV("global", "RtMaterialDrawRanges", ctx.GetBuffer(rtMaterialDrawRanges));

			RTSceneHitProgramDesc hitProgramDesc;
			pass.BindSceneHitPrograms(hitProgramDesc);
			pass.Dispatch(GetRenderWidth(), GetRenderHeight());
		});

	if (!rg.Execute())
		return false;

	return true;
}

bool Corona::RaytraceTranslucentRefractedGuideGBufferPass()
{
	bRTTranslucentVolumeValidThisFrame = false;
	RTTranslucentVolumeWidth = 0u;
	RTTranslucentVolumeHeight = 0u;

	auto logSkip = [this](const std::wstring& reason)
	{
		if ((FrameCounter % 120u) == 0u)
			AppendCpuRuntimeTrace(std::wstring(L"[RTRefractedGuide] skipped: ") + reason);
	};

	if (!renderBackend ||
		RenderingMode != ERenderingMode::HYBRID ||
		GBufferGenerationMode != EGBufferGenerationMode::RtPrimary)
	{
		logSkip(L"not hybrid rt-primary");
		return false;
	}
	if (!renderBackend->SupportsRayTracing() || !TLAS)
	{
		logSkip(L"ray tracing unsupported or TLAS missing");
		return false;
	}
	if (!TranslucentGuideDepthBuffer ||
		!TranslucentGuideVelocityBuffer ||
		!TranslucentGuideNormalBuffer ||
		!TranslucentGuideRoughnessBuffer ||
		!TranslucentGuideAlbedoBuffer ||
		!TranslucentGuideSpecularAlbedoBuffer ||
		!TranslucentDistortionBuffer ||
		!RTRefractedColorBuffer ||
		!RTRefractedGuideDummySpecularHitDistanceBuffer ||
		!RTRefractedGuideDummySpecularMotionVectorBuffer)
	{
		logSkip(L"translucent guide buffers missing");
		return false;
	}
	if (!UsesRTBindlessMaterials() || !UsesRTBindlessGeometry())
	{
		logSkip(L"bindless material/geometry support unavailable");
		return false;
	}

	uint32_t visibleTranslucentMeshCount = 0;
	for (const SceneObject& object : RenderWorld.SceneObjects)
	{
		if (!object.bVisible || !object.ScenePtr)
			continue;
		for (const std::shared_ptr<Mesh>& mesh : object.ScenePtr->meshes)
		{
			if (mesh && mesh->bAlphaBlend)
				++visibleTranslucentMeshCount;
		}
	}

	uint32_t translucentInstanceCount = 0;
	for (const RTInstanceDesc& instance : RayTracingInstances)
	{
		Mesh* mesh = instance.BottomLevelAS ? instance.BottomLevelAS->MeshPtr : nullptr;
		if (mesh && mesh->bAlphaBlend)
			++translucentInstanceCount;
	}
	if (translucentInstanceCount == 0)
	{
		logSkip(
			std::wstring(L"no alpha-blend instances in TLAS, visibleAlphaMeshes=") +
			std::to_wstring(visibleTranslucentMeshCount) +
			L", rtInstances=" +
			std::to_wstring(RayTracingInstances.size()));
		return false;
	}

	if (!PSO_PATH_TRACING)
	{
		InitPathTracingPass();
		if (!PSO_PATH_TRACING)
		{
			logSkip(L"path tracing PSO unavailable");
			return false;
		}
	}

	ApplyRenderPointLightsToFrameParams();
	const UINT32 pointLightStateHash = ComputePathTracingPointLightStateHash();
	if (!EnsurePathTracingPointLightBuffer(pointLightStateHash))
	{
		logSkip(L"point light buffer unavailable");
		return false;
	}
	if (!EnsureRTMaterialRecordBuffer())
	{
		logSkip(L"RT material records unavailable");
		return false;
	}

	auto getPersistentNumber = [this](const char* name, float fallback)
	{
		const auto result = PersistentScriptControls.find(name);
		if (result != PersistentScriptControls.end() && result->second.Type == PersistentScriptControlType::Number)
			return result->second.Number;
		return fallback;
	};
	auto getPersistentBool = [this](const char* name, bool fallback)
	{
		const auto result = PersistentScriptControls.find(name);
		if (result != PersistentScriptControls.end() && result->second.Type == PersistentScriptControlType::Bool)
			return result->second.Bool;
		return fallback;
	};

	PathTracingViewParamCB dispatchViewParam = PathTracingViewParam;
	dispatchViewParam.ViewMatrix = glm::transpose(ViewMat);
	dispatchViewParam.InvViewMatrix = glm::transpose(InvViewMat);
	dispatchViewParam.ProjMatrix = glm::transpose(UnjitteredProjMat);
	dispatchViewParam.InvProjMatrix = glm::transpose(UnjitteredInvProjMat);
	dispatchViewParam.UnjitteredViewProjMatrix = glm::transpose(UnjitteredViewProjMat);
	dispatchViewParam.PrevUnjitteredViewProjMatrix = glm::transpose(PrevUnjitteredViewProjMat);
	dispatchViewParam.ProjectionParams = FrameProjectionParams;
	dispatchViewParam.LightDirAndIntensity = glm::vec4(RenderFrameNormalizedLightDir, LightIntensity);
	dispatchViewParam.DirectLightAngularRadius = RTShadowViewParam.ShadowLightRadius;
	dispatchViewParam.DirectLightSampleCount = 1u;
	dispatchViewParam.PointLightSampleCount = 0u;
	dispatchViewParam.bDirectLightCastShadow = 0u;
	dispatchViewParam.RandomOffset = CurrentJitter * 0.5f;
	dispatchViewParam.FrameCounter = 0u;
	dispatchViewParam.BlueNoiseOffsetStride = RenderFrameIndex;
	dispatchViewParam.MaxBounces = 1u;
	dispatchViewParam.SamplesPerPixel = 1u;
	dispatchViewParam.ViewSpreadAngle = glm::tan(Fov * 0.5f) / (0.5f * GetRenderHeight());
	dispatchViewParam.DebugMode = 0u;
	dispatchViewParam.SkyColorTop = SkyColorTop;
	dispatchViewParam.SkyIntensity = SkyIntensity;
	dispatchViewParam.SkyColorBottom = SkyColorBottom;
	dispatchViewParam.LightColor = RenderFrameLightColor;
	dispatchViewParam.bEnableDiffuseGI = 0u;
	dispatchViewParam.bEnableSpecularGI = 0u;
	dispatchViewParam.bEnableDirectDiffuse = 0u;
	dispatchViewParam.bEnableDirectSpecular = 0u;
	dispatchViewParam.bEnableRTAO = 0u;
	dispatchViewParam.bWritePrimaryGBuffer = 1u;
	dispatchViewParam.SpecularMotionVectorScale = PathTracingRRSpecularMotionVectorScale;
	dispatchViewParam.bStabilizePrimaryRaySamples = 1u;
	dispatchViewParam.bPrimaryGBufferOnly = 1u;
	dispatchViewParam.PointLightCount = 0u;
	dispatchViewParam.bRefractedGuideGBufferOnly = 1u;
	dispatchViewParam.bRefractedVolumeOnly = 0u;
	dispatchViewParam.RefractedGuideMaxLayers = static_cast<UINT32>(std::clamp(
		static_cast<int>(std::lround(getPersistentNumber("transparency_layers.rt_refraction_layers", 4.0f))),
		1,
		8));
	dispatchViewParam.RefractedGuideIOR = std::clamp(
		getPersistentNumber("transparency_layers.rt_refraction_ior", 1.45f),
		1.0001f,
		2.5f);
	dispatchViewParam.RefractedGuideRayBias = std::clamp(
		getPersistentNumber("transparency_layers.rt_refraction_ray_bias", 0.05f),
		0.0001f,
		2.0f);
	dispatchViewParam.bRefractedGuideDebug = getPersistentBool("transparency_layers.rt_refraction_debug", false) ? 1u : 0u;
	dispatchViewParam.RefractedGuideRoughness = std::clamp(
		getPersistentNumber("transparency_layers.roughness", 0.0f),
		0.0f,
		1.0f);

	const bool bVolumeScatteringEnabled = getPersistentBool("transparency_layers.rt_volume_scattering", false);
	const UINT32 volumeResolutionDivisor = static_cast<UINT32>(std::clamp(
		static_cast<int>(std::lround(getPersistentNumber("transparency_layers.rt_volume_resolution_divisor", 2.0f))),
		1,
		4));
	const UINT32 volumeWidth = (GetRenderWidth() + volumeResolutionDivisor - 1u) / volumeResolutionDivisor;
	const UINT32 volumeHeight = (GetRenderHeight() + volumeResolutionDivisor - 1u) / volumeResolutionDivisor;
	PathTracingViewParamCB volumeViewParam = dispatchViewParam;
	volumeViewParam.bRefractedGuideGBufferOnly = 0u;
	volumeViewParam.bRefractedVolumeOnly = 1u;
	volumeViewParam.RefractedVolumeParams = glm::vec4(
		std::clamp(getPersistentNumber("transparency_layers.rt_volume_density", 0.001f), 0.0f, 0.005f) *
			kTranslucentVolumeDensityWorldScale,
		std::clamp(getPersistentNumber("transparency_layers.rt_volume_scattering_strength", 0.65f), 0.0f, 8.0f),
		std::clamp(getPersistentNumber("transparency_layers.rt_volume_anisotropy", 0.0f), -0.9f, 0.9f),
		static_cast<float>(volumeResolutionDivisor));

	if ((FrameCounter % 120u) == 0u)
	{
		AppendCpuRuntimeTrace(
			L"[RTRefractedGuide] active=1, visibleAlphaMeshes=" + std::to_wstring(visibleTranslucentMeshCount) +
			L", alphaInstances=" + std::to_wstring(translucentInstanceCount) +
			L", rtInstances=" + std::to_wstring(RayTracingInstances.size()) +
			L", layers=" + std::to_wstring(dispatchViewParam.RefractedGuideMaxLayers) +
			L", ior=" + std::to_wstring(dispatchViewParam.RefractedGuideIOR) +
			L", roughness=" + std::to_wstring(dispatchViewParam.RefractedGuideRoughness) +
			L", rayBias=" + std::to_wstring(dispatchViewParam.RefractedGuideRayBias) +
			L", debug=" + std::to_wstring(dispatchViewParam.bRefractedGuideDebug) +
			L", volume=" + std::to_wstring(bVolumeScatteringEnabled ? 1u : 0u) +
			L", volumeDivisor=" + std::to_wstring(volumeResolutionDivisor));
	}

	RenderGraph rg(renderBackend.get());
	RGTextureRef outputColorTarget = rg.ImportTexture("RTRefractedGuide.SourceUV", TranslucentDistortionBuffer.get(), EResourceState::ShaderRead);
	RGTextureRef outAlbedo = rg.ImportTexture("RTRefractedGuide.OutAlbedo", TranslucentGuideAlbedoBuffer.get(), EResourceState::ShaderRead);
	RGTextureRef outSpecularAlbedo = rg.ImportTexture("RTRefractedGuide.OutSpecularAlbedo", TranslucentGuideSpecularAlbedoBuffer.get(), EResourceState::ShaderRead);
	RGTextureRef outNormal = rg.ImportTexture("RTRefractedGuide.OutNormal", TranslucentGuideNormalBuffer.get(), EResourceState::ShaderRead);
	RGTextureRef outGeomNormal = rg.ImportTexture("RTRefractedGuide.OutTransmissionColor", RTRefractedColorBuffer.get(), EResourceState::ShaderRead);
	RGTextureRef outVelocity = rg.ImportTexture("RTRefractedGuide.OutVelocity", TranslucentGuideVelocityBuffer.get(), EResourceState::ShaderRead);
	RGTextureRef outRoughnessMetallic = rg.ImportTexture("RTRefractedGuide.OutRoughnessMetallic", TranslucentGuideRoughnessBuffer.get(), EResourceState::ShaderRead);
	RGTextureRef outDepth = rg.ImportTexture("RTRefractedGuide.OutDepth", TranslucentGuideDepthBuffer.get(), EResourceState::ShaderRead);
	RGTextureRef outSpecularHitDistance = rg.ImportTexture("RTRefractedGuide.DummySpecularHitDistance", RTRefractedGuideDummySpecularHitDistanceBuffer.get(), EResourceState::ShaderRead);
	RGTextureRef outSpecularMotionVector = rg.ImportTexture("RTRefractedGuide.DummySpecularMotionVector", RTRefractedGuideDummySpecularMotionVectorBuffer.get(), EResourceState::ShaderRead);
	RGBufferRef pointLightBuffer = rg.ImportBuffer("RTRefractedGuide.PointLightBuffer", PathTracingPointLightBuffer.get(), EResourceState::ShaderRead);
	RGBufferRef rtMaterials = rg.ImportBuffer("RTRefractedGuide.RtMaterials", RTMaterialRecordBuffer.get(), EResourceState::ShaderRead);
	RGBufferRef rtMaterialDrawRanges = rg.ImportBuffer("RTRefractedGuide.RtMaterialDrawRanges", RTMaterialDrawRangeRecordBuffer.get(), EResourceState::ShaderRead);

	renderBackend->EmitGpuCrashMarker("RTRefractedGuideGBufferPass");

	rg.ExportTexture(outputColorTarget, EResourceState::ShaderRead);
	rg.ExportTexture(outAlbedo, EResourceState::ShaderRead);
	rg.ExportTexture(outSpecularAlbedo, EResourceState::ShaderRead);
	rg.ExportTexture(outNormal, EResourceState::ShaderRead);
	rg.ExportTexture(outVelocity, EResourceState::ShaderRead);
	rg.ExportTexture(outRoughnessMetallic, EResourceState::ShaderRead);
	rg.ExportTexture(outDepth, EResourceState::ShaderRead);
	rg.ExportTexture(outGeomNormal, EResourceState::ShaderRead);
	rg.ExportTexture(outSpecularHitDistance, EResourceState::ShaderRead);
	rg.ExportTexture(outSpecularMotionVector, EResourceState::ShaderRead);

	rg.AddPass(
		"RTRefractedGuideGBufferPass",
		ERGPassFlags::RayTracing,
		[&](RGPassBuilder& builder)
		{
			builder.ReadWriteTexture(outputColorTarget, EResourceState::UnorderedAccess)
				.ReadWriteTexture(outAlbedo, EResourceState::UnorderedAccess)
				.ReadWriteTexture(outSpecularAlbedo, EResourceState::UnorderedAccess)
				.ReadWriteTexture(outNormal, EResourceState::UnorderedAccess)
				.ReadWriteTexture(outVelocity, EResourceState::UnorderedAccess)
				.ReadWriteTexture(outRoughnessMetallic, EResourceState::UnorderedAccess)
				.ReadWriteTexture(outDepth, EResourceState::UnorderedAccess)
				.ReadBuffer(pointLightBuffer, EResourceState::ShaderRead)
				.ReadBuffer(rtMaterials, EResourceState::ShaderRead)
				.ReadBuffer(rtMaterialDrawRanges, EResourceState::ShaderRead);
			builder.WriteTexture(outGeomNormal, EResourceState::UnorderedAccess)
				.WriteTexture(outSpecularHitDistance, EResourceState::UnorderedAccess)
				.WriteTexture(outSpecularMotionVector, EResourceState::UnorderedAccess);
		},
		[&](RGContext& ctx)
		{
			RTPassBuilder pass(*this, PSO_PATH_TRACING, ERtProfilePass::PathTracing);
			pass.BeginScene()
				.SetTextureUAV("global", "OutputColor", ctx.GetTexture(outputColorTarget))
				.SetTextureUAV("global", "OutAlbedo", ctx.GetTexture(outAlbedo))
				.SetTextureUAV("global", "OutSpecularAlbedo", ctx.GetTexture(outSpecularAlbedo))
				.SetTextureUAV("global", "OutNormal", ctx.GetTexture(outNormal))
				.SetTextureUAV("global", "OutGeomNormal", ctx.GetTexture(outGeomNormal))
				.SetTextureUAV("global", "OutVelocity", ctx.GetTexture(outVelocity))
				.SetTextureUAV("global", "OutRoughnessMetallic", ctx.GetTexture(outRoughnessMetallic))
				.SetTextureUAV("global", "OutDepth", ctx.GetTexture(outDepth))
				.SetTextureUAV("global", "OutSpecularHitDistance", ctx.GetTexture(outSpecularHitDistance))
				.SetTextureUAV("global", "OutSpecularMotionVector", ctx.GetTexture(outSpecularMotionVector))
				.SetAccelerationStructure("global", "gRtScene", TLAS)
				.SetBufferSRV("global", "PointLightBuffer", ctx.GetBuffer(pointLightBuffer))
				.SetCBVValue("global", "ViewParameter", &dispatchViewParam)
				.SetSampler("global", "sampleWrap", samplerWrap.get());
			pass.SetBindlessTextureTable("global", "MaterialTextures")
				.SetBufferSRV("global", "RtMaterials", ctx.GetBuffer(rtMaterials))
				.SetBufferSRV("global", "RtMaterialDrawRanges", ctx.GetBuffer(rtMaterialDrawRanges));

			RTSceneHitProgramDesc hitProgramDesc;
			pass.BindSceneHitPrograms(hitProgramDesc);
			pass.Dispatch(GetRenderWidth(), GetRenderHeight());
		});

	if (bVolumeScatteringEnabled)
	{
		rg.AddPass(
			"RTTranslucentVolumePass",
			ERGPassFlags::RayTracing,
			[&](RGPassBuilder& builder)
			{
				builder.ReadWriteTexture(outputColorTarget, EResourceState::UnorderedAccess)
					.ReadWriteTexture(outAlbedo, EResourceState::UnorderedAccess)
					.ReadWriteTexture(outSpecularAlbedo, EResourceState::UnorderedAccess)
					.ReadWriteTexture(outNormal, EResourceState::UnorderedAccess)
					.ReadWriteTexture(outVelocity, EResourceState::UnorderedAccess)
					.ReadWriteTexture(outRoughnessMetallic, EResourceState::UnorderedAccess)
					.ReadWriteTexture(outDepth, EResourceState::UnorderedAccess)
					.ReadWriteTexture(outGeomNormal, EResourceState::UnorderedAccess)
					.ReadWriteTexture(outSpecularHitDistance, EResourceState::UnorderedAccess)
					.ReadWriteTexture(outSpecularMotionVector, EResourceState::UnorderedAccess)
					.ReadBuffer(pointLightBuffer, EResourceState::ShaderRead)
					.ReadBuffer(rtMaterials, EResourceState::ShaderRead)
					.ReadBuffer(rtMaterialDrawRanges, EResourceState::ShaderRead);
			},
			[&](RGContext& ctx)
			{
				RTPassBuilder pass(*this, PSO_PATH_TRACING, ERtProfilePass::PathTracing);
				pass.BeginScene()
					.SetTextureUAV("global", "OutputColor", ctx.GetTexture(outputColorTarget))
					.SetTextureUAV("global", "OutAlbedo", ctx.GetTexture(outAlbedo))
					.SetTextureUAV("global", "OutSpecularAlbedo", ctx.GetTexture(outSpecularAlbedo))
					.SetTextureUAV("global", "OutNormal", ctx.GetTexture(outNormal))
					.SetTextureUAV("global", "OutGeomNormal", ctx.GetTexture(outGeomNormal))
					.SetTextureUAV("global", "OutVelocity", ctx.GetTexture(outVelocity))
					.SetTextureUAV("global", "OutRoughnessMetallic", ctx.GetTexture(outRoughnessMetallic))
					.SetTextureUAV("global", "OutDepth", ctx.GetTexture(outDepth))
					.SetTextureUAV("global", "OutSpecularHitDistance", ctx.GetTexture(outSpecularHitDistance))
					.SetTextureUAV("global", "OutSpecularMotionVector", ctx.GetTexture(outSpecularMotionVector))
					.SetAccelerationStructure("global", "gRtScene", TLAS)
					.SetBufferSRV("global", "PointLightBuffer", ctx.GetBuffer(pointLightBuffer))
					.SetCBVValue("global", "ViewParameter", &volumeViewParam)
					.SetSampler("global", "sampleWrap", samplerWrap.get());
				pass.SetBindlessTextureTable("global", "MaterialTextures")
					.SetBufferSRV("global", "RtMaterials", ctx.GetBuffer(rtMaterials))
					.SetBufferSRV("global", "RtMaterialDrawRanges", ctx.GetBuffer(rtMaterialDrawRanges));

				RTSceneHitProgramDesc hitProgramDesc;
				pass.BindSceneHitPrograms(hitProgramDesc);
				pass.Dispatch(volumeWidth, volumeHeight);
			});
	}

	if (!rg.Execute())
	{
		std::wstring reason = L"render graph execution failed";
		if (!rg.GetDiagnostics().empty())
		{
			reason += L": ";
			for (size_t diagnosticIndex = 0; diagnosticIndex < rg.GetDiagnostics().size(); ++diagnosticIndex)
			{
				if (diagnosticIndex != 0)
					reason += L"; ";
				const std::string& diagnostic = rg.GetDiagnostics()[diagnosticIndex];
				reason.reserve(reason.size() + diagnostic.size());
				for (char c : diagnostic)
					reason.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
			}
		}
		logSkip(reason);
		return false;
	}

	bTranslucentDistortionGuideValidThisFrame = true;
	bTranslucentDistortionGuideIsOffsetThisFrame = false;
	bRTTranslucentVolumeValidThisFrame = bVolumeScatteringEnabled;
	RTTranslucentVolumeWidth = bVolumeScatteringEnabled ? volumeWidth : 0u;
	RTTranslucentVolumeHeight = bVolumeScatteringEnabled ? volumeHeight : 0u;
	return true;
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

	if (!EnsureRTMaterialRecordBuffer())
		return;

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
			return;
		}
	}
	else
	{
		bPathTracingCompactionFallbackLogged = false;
	}

	RenderGraph rg(renderBackend.get());
	RGTextureRef outputColorTarget = rg.ImportTexture("PathTracing.OutputColor", outputColor, EResourceState::ShaderRead);
	RGTextureRef outAlbedo = bWritePrimaryGBuffer
		? rg.ImportTexture("PathTracing.OutAlbedo", AlbedoBuffer.get(), EResourceState::ShaderRead)
		: RGTextureRef{};
	RGTextureRef outSpecularAlbedo = bWritePrimaryGBuffer
		? rg.ImportTexture("PathTracing.OutSpecularAlbedo", SpecularAlbedoBuffer.get(), EResourceState::ShaderRead)
		: RGTextureRef{};
	RGTextureRef outNormal = bWritePrimaryGBuffer
		? rg.ImportTexture("PathTracing.OutNormal", NormalBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead)
		: RGTextureRef{};
	RGTextureRef outGeomNormal = bWritePrimaryGBuffer
		? rg.ImportTexture("PathTracing.OutGeomNormal", GeomNormalBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead)
		: RGTextureRef{};
	RGTextureRef outVelocity = bWritePrimaryGBuffer
		? rg.ImportTexture("PathTracing.OutVelocity", VelocityBuffer.get(), EResourceState::ShaderRead)
		: RGTextureRef{};
	RGTextureRef outRoughnessMetallic = bWritePrimaryGBuffer
		? rg.ImportTexture("PathTracing.OutRoughnessMetallic", RoughnessMetalicBuffer.get(), EResourceState::ShaderRead)
		: RGTextureRef{};
	RGTextureRef outDepth = bWritePrimaryGBuffer
		? rg.ImportTexture("PathTracing.OutDepth", UnjitteredDepthBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead)
		: RGTextureRef{};
	RGTextureRef outSpecularHitDistance = bWritePrimaryGBuffer
		? rg.ImportTexture("PathTracing.OutSpecularHitDistance", PathTracingSpecularHitDistanceBuffer.get(), EResourceState::ShaderRead)
		: RGTextureRef{};
	RGTextureRef outSpecularMotionVector = bWritePrimaryGBuffer
		? rg.ImportTexture("PathTracing.OutSpecularMotionVector", PathTracingSpecularMotionVectorBuffer.get(), EResourceState::ShaderRead)
		: RGTextureRef{};
	RGBufferRef pointLightBuffer = rg.ImportBuffer("PathTracing.PointLightBuffer", PathTracingPointLightBuffer.get(), EResourceState::ShaderRead);
	RGBufferRef rtMaterials = rg.ImportBuffer("PathTracing.RtMaterials", RTMaterialRecordBuffer.get(), EResourceState::ShaderRead);
	RGBufferRef rtMaterialDrawRanges = rg.ImportBuffer("PathTracing.RtMaterialDrawRanges", RTMaterialDrawRangeRecordBuffer.get(), EResourceState::ShaderRead);

	rg.ExportTexture(outputColorTarget, EResourceState::ShaderRead);
	if (outAlbedo.IsValid())
		rg.ExportTexture(outAlbedo, EResourceState::ShaderRead);
	if (outSpecularAlbedo.IsValid())
		rg.ExportTexture(outSpecularAlbedo, EResourceState::ShaderRead);
	if (outNormal.IsValid())
		rg.ExportTexture(outNormal, EResourceState::ShaderRead);
	if (outGeomNormal.IsValid())
		rg.ExportTexture(outGeomNormal, EResourceState::ShaderRead);
	if (outVelocity.IsValid())
		rg.ExportTexture(outVelocity, EResourceState::ShaderRead);
	if (outRoughnessMetallic.IsValid())
		rg.ExportTexture(outRoughnessMetallic, EResourceState::ShaderRead);
	if (outDepth.IsValid())
		rg.ExportTexture(outDepth, EResourceState::ShaderRead);
	if (outSpecularHitDistance.IsValid())
		rg.ExportTexture(outSpecularHitDistance, EResourceState::ShaderRead);
	if (outSpecularMotionVector.IsValid())
		rg.ExportTexture(outSpecularMotionVector, EResourceState::ShaderRead);

	rg.AddPass(
		"PathTracingPass",
		ERGPassFlags::RayTracing,
		[&](RGPassBuilder& builder)
		{
			builder.ReadWriteTexture(outputColorTarget, EResourceState::UnorderedAccess)
				.ReadBuffer(pointLightBuffer, EResourceState::ShaderRead)
				.ReadBuffer(rtMaterials, EResourceState::ShaderRead)
				.ReadBuffer(rtMaterialDrawRanges, EResourceState::ShaderRead);
			if (outAlbedo.IsValid())
				builder.ReadWriteTexture(outAlbedo, EResourceState::UnorderedAccess);
			if (outSpecularAlbedo.IsValid())
				builder.ReadWriteTexture(outSpecularAlbedo, EResourceState::UnorderedAccess);
			if (outNormal.IsValid())
				builder.ReadWriteTexture(outNormal, EResourceState::UnorderedAccess);
			if (outGeomNormal.IsValid())
				builder.ReadWriteTexture(outGeomNormal, EResourceState::UnorderedAccess);
			if (outVelocity.IsValid())
				builder.ReadWriteTexture(outVelocity, EResourceState::UnorderedAccess);
			if (outRoughnessMetallic.IsValid())
				builder.ReadWriteTexture(outRoughnessMetallic, EResourceState::UnorderedAccess);
			if (outDepth.IsValid())
				builder.ReadWriteTexture(outDepth, EResourceState::UnorderedAccess);
			if (outSpecularHitDistance.IsValid())
				builder.ReadWriteTexture(outSpecularHitDistance, EResourceState::UnorderedAccess);
			if (outSpecularMotionVector.IsValid())
				builder.ReadWriteTexture(outSpecularMotionVector, EResourceState::UnorderedAccess);
		},
		[&](RGContext& ctx)
		{
			RTPassBuilder pass(*this, PSO_PATH_TRACING, ERtProfilePass::PathTracing);
			pass.BeginScene()
				.SetTextureUAV("global", "OutputColor", ctx.GetTexture(outputColorTarget))
				.SetTextureUAV("global", "OutAlbedo", outAlbedo.IsValid() ? ctx.GetTexture(outAlbedo) : nullptr)
				.SetTextureUAV("global", "OutSpecularAlbedo", outSpecularAlbedo.IsValid() ? ctx.GetTexture(outSpecularAlbedo) : nullptr)
				.SetTextureUAV("global", "OutNormal", outNormal.IsValid() ? ctx.GetTexture(outNormal) : nullptr)
				.SetTextureUAV("global", "OutGeomNormal", outGeomNormal.IsValid() ? ctx.GetTexture(outGeomNormal) : nullptr)
				.SetTextureUAV("global", "OutVelocity", outVelocity.IsValid() ? ctx.GetTexture(outVelocity) : nullptr)
				.SetTextureUAV("global", "OutRoughnessMetallic", outRoughnessMetallic.IsValid() ? ctx.GetTexture(outRoughnessMetallic) : nullptr)
				.SetTextureUAV("global", "OutDepth", outDepth.IsValid() ? ctx.GetTexture(outDepth) : nullptr)
				.SetTextureUAV("global", "OutSpecularHitDistance", outSpecularHitDistance.IsValid() ? ctx.GetTexture(outSpecularHitDistance) : nullptr)
				.SetTextureUAV("global", "OutSpecularMotionVector", outSpecularMotionVector.IsValid() ? ctx.GetTexture(outSpecularMotionVector) : nullptr)
				.SetAccelerationStructure("global", "gRtScene", TLAS)
				.SetBufferSRV("global", "PointLightBuffer", ctx.GetBuffer(pointLightBuffer))
				.SetCBVValue("global", "ViewParameter", &dispatchViewParam)
				.SetSampler("global", "sampleWrap", samplerWrap.get());
			pass.SetBindlessTextureTable("global", "MaterialTextures")
				.SetBufferSRV("global", "RtMaterials", ctx.GetBuffer(rtMaterials))
				.SetBufferSRV("global", "RtMaterialDrawRanges", ctx.GetBuffer(rtMaterialDrawRanges));

			RTSceneHitProgramDesc hitProgramDesc;
			pass.BindSceneHitPrograms(hitProgramDesc);
			pass.Dispatch(m_width, m_height);
		});

	if (!rg.Execute())
		return;

	if (PathTracingViewParam.DebugMode == 0)
	{
		PathTracingAccumulatedFrames++;
	}
}
