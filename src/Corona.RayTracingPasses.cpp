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

namespace
{
	void AddMeshesToBLAS(vector<shared_ptr<RTAS>>& vecBLAS, shared_ptr<Scene> scene)
	{
		for (auto& mesh : scene->meshes)
		{
			shared_ptr<RTAS> blas = mesh->CreateBLAS();
			if (blas == nullptr)
			{
				continue;
			}
			vecBLAS.push_back(blas);
		}
	}
}

void Corona::UpdateInstancePropertyBuffer()
{
	constexpr UINT32 kMinInstancePropertyCapacity = 500u;
	const UINT32 instanceCapacity = std::max(kMinInstancePropertyCapacity, static_cast<UINT32>(vecBLAS.size()));
	std::vector<InstanceProperty> instanceProperties(instanceCapacity);
	const size_t instanceCount = (std::min)(instanceProperties.size(), vecBLAS.size());
	for (size_t i = 0; i < instanceCount; ++i)
	{
		instanceProperties[i].WorldMatrix = glm::transpose(vecBLAS[i]->MeshPtr->transform);
		instanceProperties[i].VertexOffset = 0;
		instanceProperties[i].IndexOffset = 0;
	}

	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::Vulkan)
	{
		InstancePropertyBuffer = renderBackend->CreateBuffer({
			instanceCapacity,
			sizeof(InstanceProperty),
			EInitialResourceState::ShaderRead,
			false,
			instanceProperties.data()
		});
		InstancePropertyBuffer->MakeByteAddressBufferSRV();
		NAME_D3D12_OBJECT(InstancePropertyBuffer->resource);
		return;
	}

	if (!InstancePropertyBuffer || InstancePropertyBuffer->NumElements < instanceCapacity)
	{
		InstancePropertyBuffer = renderBackend->CreateBuffer({ instanceCapacity, sizeof(InstanceProperty), EInitialResourceState::GenericRead, false, nullptr });
		InstancePropertyBuffer->MakeByteAddressBufferSRV();
		NAME_D3D12_OBJECT(InstancePropertyBuffer->resource);
	}

	// Map and update instance properties
	uint8_t* pData;
	InstancePropertyBuffer->resource->Map(0, nullptr, (void**)&pData);

	memcpy(pData, instanceProperties.data(), instanceProperties.size() * sizeof(InstanceProperty));

	InstancePropertyBuffer->resource->Unmap(0, nullptr);
}

void Corona::RebuildAccelerationStructures()
{
	// Wait for GPU to finish using current structures
	renderBackend->WaitForGpu();
	
	// Recreate TLAS with current BLAS list
	TLAS = renderBackend->CreateTLAS(vecBLAS);
	
	// Update instance property buffer
	UpdateInstancePropertyBuffer();

	if (PSO_PATH_TRACING)
		InitPathTracingPass();
}

void Corona::AddScene(shared_ptr<Scene> scene)
{
	// Add all meshes from scene to BLAS vector
	AddMeshesToBLAS(vecBLAS, scene);
	
	// Rebuild acceleration structures and update buffers
	RebuildAccelerationStructures();
}

void Corona::InitRaytracingData()
{
	UINT NumTotalMesh = Sponza ? static_cast<UINT>(Sponza->meshes.size()) : 0u;
	if (Buddha)
		NumTotalMesh += static_cast<UINT>(Buddha->meshes.size());
	vecBLAS.reserve(NumTotalMesh);

	// Create initial instance property buffer (large enough for many instances)
	InstancePropertyBuffer = renderBackend->CreateBuffer({ 500u, sizeof(InstanceProperty), EInitialResourceState::GenericRead, false, nullptr });
	InstancePropertyBuffer->MakeByteAddressBufferSRV();
	NAME_D3D12_OBJECT(InstancePropertyBuffer->resource);

	// Add initial scene(s)
	if (Sponza)
		AddScene(Sponza);
	if (Buddha)
		AddScene(Buddha);
	//AddScene(ShaderBall);
}

void Corona::InitRTPSO()
{
	const uint32_t maxSupportedHybridStage = renderBackend ? renderBackend->GetMaxSupportedHybridStage() : 7u;
	const bool bInitReflectionRT = !renderBackend || renderBackend->GetAPI() != ERenderBackendAPI::Vulkan || maxSupportedHybridStage >= 3u;
	const bool bInitGIRT = !renderBackend || renderBackend->GetAPI() != ERenderBackendAPI::Vulkan || maxSupportedHybridStage >= 4u;

	InitRaytracingShadowPass();
	if (bInitReflectionRT)
		InitRaytracingReflectionPass();
	if (bInitGIRT)
	{
		InitRaytracingSimpleGIPass();
		InitRaytracingScreenProbePass();
		InitRaytracingSpatialHashPass();
	}
}
