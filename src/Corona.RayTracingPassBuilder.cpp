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

namespace
{
	void HashCombineRtBinding(uint64_t& seed, uint64_t value)
	{
		seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
	}

	void HashCombinePointer(uint64_t& seed, const void* value)
	{
		HashCombineRtBinding(seed, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(value)));
	}
}

Corona::RTPassBuilder::RTPassBuilder(Corona& owner, const shared_ptr<RTPipelineStateObject>& pso)
	: Owner(owner)
	, PSO(pso)
{
}

bool Corona::RTPassBuilder::IsValid() const
{
	return PSO != nullptr;
}

Corona::RTPassBuilder& Corona::RTPassBuilder::BeginScene()
{
	if (!PSO)
		return *this;

	const auto phaseStart = Corona::CpuClock::now();
	PSO->SetNumInstances(static_cast<uint32_t>(Owner.RayTracingInstances.size()));
	PSO->BeginShaderTable();
	bBegan = true;
	Owner.AddRtRecordPhaseTiming(ERtRecordPhase::BeginScene, phaseStart, Corona::CpuClock::now());
	return *this;
}

Corona::RTPassBuilder& Corona::RTPassBuilder::SetTextureUAV(const char* shader, const char* bindingName, Texture* texture)
{
	const auto phaseStart = Corona::CpuClock::now();
	if (PSO && texture)
		PSO->SetTextureUAV(shader, bindingName, texture);
	Owner.AddRtRecordPhaseTiming(ERtRecordPhase::BindResources, phaseStart, Corona::CpuClock::now());
	return *this;
}

Corona::RTPassBuilder& Corona::RTPassBuilder::SetBufferUAV(const char* shader, const char* bindingName, Buffer* buffer)
{
	const auto phaseStart = Corona::CpuClock::now();
	if (PSO && buffer)
		PSO->SetBufferUAV(shader, bindingName, buffer);
	Owner.AddRtRecordPhaseTiming(ERtRecordPhase::BindResources, phaseStart, Corona::CpuClock::now());
	return *this;
}

Corona::RTPassBuilder& Corona::RTPassBuilder::SetTextureSRV(const char* shader, const char* bindingName, Texture* texture)
{
	const auto phaseStart = Corona::CpuClock::now();
	if (PSO && texture)
		PSO->SetTextureSRV(shader, bindingName, texture);
	Owner.AddRtRecordPhaseTiming(ERtRecordPhase::BindResources, phaseStart, Corona::CpuClock::now());
	return *this;
}

Corona::RTPassBuilder& Corona::RTPassBuilder::SetBufferSRV(const char* shader, const char* bindingName, Buffer* buffer)
{
	const auto phaseStart = Corona::CpuClock::now();
	if (PSO && buffer)
		PSO->SetBufferSRV(shader, bindingName, buffer);
	Owner.AddRtRecordPhaseTiming(ERtRecordPhase::BindResources, phaseStart, Corona::CpuClock::now());
	return *this;
}

Corona::RTPassBuilder& Corona::RTPassBuilder::SetAccelerationStructure(const char* shader, const char* bindingName, const shared_ptr<RTAS>& rtas)
{
	const auto phaseStart = Corona::CpuClock::now();
	if (PSO && rtas)
		PSO->SetAccelerationStructure(shader, bindingName, rtas);
	Owner.AddRtRecordPhaseTiming(ERtRecordPhase::BindResources, phaseStart, Corona::CpuClock::now());
	return *this;
}

Corona::RTPassBuilder& Corona::RTPassBuilder::SetSampler(const char* shader, const char* bindingName, Sampler* sampler)
{
	const auto phaseStart = Corona::CpuClock::now();
	if (PSO && sampler)
		PSO->SetSampler(shader, bindingName, sampler);
	Owner.AddRtRecordPhaseTiming(ERtRecordPhase::BindResources, phaseStart, Corona::CpuClock::now());
	return *this;
}

Corona::RTPassBuilder& Corona::RTPassBuilder::SetCBVValue(const char* shader, const char* bindingName, void* data)
{
	const auto phaseStart = Corona::CpuClock::now();
	if (PSO && data)
		PSO->SetCBVValue(shader, bindingName, data);
	Owner.AddRtRecordPhaseTiming(ERtRecordPhase::BindResources, phaseStart, Corona::CpuClock::now());
	return *this;
}

uint32_t Corona::RTPassBuilder::BindSceneHitPrograms(const RTSceneHitProgramDesc& desc, const HitProgramBinder& customBinder)
{
	if (!PSO)
		return 0;

	if (!bBegan)
		BeginScene();

	const auto phaseStart = Corona::CpuClock::now();
	const bool bCanUseCache = !customBinder;
	const uint32_t instanceCount = static_cast<uint32_t>(Owner.RayTracingInstances.size());
	const uint64_t bindingSignature = bCanUseCache ? BuildHitProgramBindingSignature(desc) : 0;
	if (bCanUseCache && PSO->IsHitProgramBindingCacheValid(instanceCount, bindingSignature))
	{
		Owner.AddRtRecordPhaseTiming(ERtRecordPhase::BindHitPrograms, phaseStart, Corona::CpuClock::now());
		return instanceCount;
	}

	PSO->MarkHitProgramBindingCacheDirty();
	uint32_t boundCount = 0;
	uint32_t instanceIndex = 0;
	for (const RTInstanceDesc& instance : Owner.RayTracingInstances)
	{
		PSO->ResetHitProgram(instanceIndex);

		Mesh* mesh = (instance.BottomLevelAS) ? instance.BottomLevelAS->MeshPtr : nullptr;
		if (!mesh)
		{
			++instanceIndex;
			continue;
		}

		PSO->StartHitProgram(desc.HitGroup, instanceIndex);

		if (desc.bBindSceneGeometry && mesh->Vb && mesh->Ib)
			PSO->AddSceneGeometrySRVsToHitProgram(desc.HitGroup, mesh->Vb.get(), mesh->Ib.get(), instanceIndex);

		if (desc.bBindInstancePropertyBeforeDiffuse && desc.bBindInstanceProperty && Owner.InstancePropertyBuffer)
			PSO->AddBufferSRVToHitProgram(desc.HitGroup, Owner.InstancePropertyBuffer.get(), instanceIndex);

		if (desc.bBindDiffuseTexture)
			PSO->AddTextureSRVToHitProgram(desc.HitGroup, GetDiffuseTexture(*mesh), instanceIndex);

		if (!desc.bBindInstancePropertyBeforeDiffuse && desc.bBindInstanceProperty && Owner.InstancePropertyBuffer)
			PSO->AddBufferSRVToHitProgram(desc.HitGroup, Owner.InstancePropertyBuffer.get(), instanceIndex);

		if (customBinder)
			customBinder(*PSO, desc, *mesh, instanceIndex);

		++boundCount;
		++instanceIndex;
	}

	if (bCanUseCache)
		PSO->MarkHitProgramBindingCacheValid(instanceCount, bindingSignature);
	Owner.AddRtRecordPhaseTiming(ERtRecordPhase::BindHitPrograms, phaseStart, Corona::CpuClock::now());
	return boundCount;
}

void Corona::RTPassBuilder::Dispatch(uint32_t width, uint32_t height)
{
	if (!PSO)
		return;

	if (!bBegan)
		BeginScene();

	const auto endShaderTableStart = Corona::CpuClock::now();
	PSO->EndShaderTable();
	Owner.AddRtRecordPhaseTiming(ERtRecordPhase::EndShaderTable, endShaderTableStart, Corona::CpuClock::now());

	const auto applyStart = Corona::CpuClock::now();
	PSO->Apply(width, height);
	Owner.AddRtRecordPhaseTiming(ERtRecordPhase::ApplyDispatch, applyStart, Corona::CpuClock::now());
	bBegan = false;
}

Texture* Corona::RTPassBuilder::GetDiffuseTexture(const Mesh& mesh) const
{
	const Material* material = GetPrimaryMaterial(mesh);
	if (material && material->Diffuse)
		return material->Diffuse.get();
	return Owner.DefaultWhiteTex.get();
}

Texture* Corona::RTPassBuilder::GetNormalTexture(const Mesh& mesh) const
{
	const Material* material = GetPrimaryMaterial(mesh);
	if (material && material->Normal)
		return material->Normal.get();
	return Owner.DefaultNormalTex.get();
}

Texture* Corona::RTPassBuilder::GetRoughnessTexture(const Mesh& mesh) const
{
	const Material* material = GetPrimaryMaterial(mesh);
	if (material && material->Roughness)
		return material->Roughness.get();
	return Owner.DefaultRougnessTex.get();
}

Texture* Corona::RTPassBuilder::GetMetallicTexture(const Mesh& mesh) const
{
	const Material* material = GetPrimaryMaterial(mesh);
	if (material && material->Metallic)
		return material->Metallic.get();
	return Owner.DefaultBlackTex.get();
}

Material* Corona::RTPassBuilder::GetPrimaryMaterial(const Mesh& mesh) const
{
	if (!mesh.Draws.empty() && mesh.Draws[0].mat)
		return mesh.Draws[0].mat.get();
	return mesh.Mat.get();
}

uint64_t Corona::RTPassBuilder::BuildHitProgramBindingSignature(const RTSceneHitProgramDesc& desc) const
{
	uint64_t signature = 1469598103934665603ull;
	HashCombineRtBinding(signature, static_cast<uint64_t>(Owner.RayTracingInstances.size()));
	HashCombineRtBinding(signature, desc.bBindSceneGeometry ? 1ull : 0ull);
	HashCombineRtBinding(signature, desc.bBindDiffuseTexture ? 1ull : 0ull);
	HashCombineRtBinding(signature, desc.bBindInstanceProperty ? 1ull : 0ull);
	HashCombineRtBinding(signature, desc.bBindInstancePropertyBeforeDiffuse ? 1ull : 0ull);
	if (desc.HitGroup)
	{
		for (const char* c = desc.HitGroup; *c; ++c)
			HashCombineRtBinding(signature, static_cast<uint8_t>(*c));
	}

	HashCombinePointer(signature, Owner.InstancePropertyBuffer.get());
	for (const RTInstanceDesc& instance : Owner.RayTracingInstances)
	{
		HashCombinePointer(signature, instance.BottomLevelAS.get());
		Mesh* mesh = instance.BottomLevelAS ? instance.BottomLevelAS->MeshPtr : nullptr;
		HashCombinePointer(signature, mesh);
		if (!mesh)
			continue;

		if (desc.bBindSceneGeometry)
		{
			HashCombinePointer(signature, mesh->Vb.get());
			HashCombinePointer(signature, mesh->Ib.get());
		}
		if (desc.bBindDiffuseTexture)
			HashCombinePointer(signature, GetDiffuseTexture(*mesh));
	}
	return signature;
}
