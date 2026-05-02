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
#include <map>

namespace
{
	constexpr uint32_t kRTInstanceFlagAlphaTested = 1u << 0;

	void AddMeshesToRayTracingInstances(
		vector<RTInstanceDesc>& instances,
		map<Mesh*, shared_ptr<RTAS>>& blasCache,
		const shared_ptr<Scene>& scene,
		const glm::mat4x4& instanceTransform)
	{
		if (!scene)
			return;

		for (auto& mesh : scene->meshes)
		{
			if (!mesh)
				continue;

			shared_ptr<RTAS> blas;
			const auto cacheIt = blasCache.find(mesh.get());
			if (cacheIt != blasCache.end())
			{
				blas = cacheIt->second;
			}
			else
			{
				blas = mesh->CreateBLAS();
				if (blas)
					blasCache[mesh.get()] = blas;
			}

			if (blas == nullptr)
				continue;

			RTInstanceDesc instance;
			instance.BottomLevelAS = blas;
			instance.Transform = instanceTransform * mesh->transform;
			for (const Mesh::DrawCall& draw : mesh->Draws)
			{
				if (draw.mat && draw.mat->bHasAlpha)
				{
					instance.Flags |= kRTInstanceFlagAlphaTested;
					break;
				}
			}
			instances.push_back(instance);
		}
	}
}

Corona::SceneObjectHandle Corona::AddSceneObject(const SceneObjectDesc& desc)
{
	if (!desc.ScenePtr)
		return InvalidSceneObjectHandle;

	SceneObject object;
	object.Handle = NextSceneObjectHandle++;
	if (NextSceneObjectHandle == InvalidSceneObjectHandle)
		NextSceneObjectHandle = 1;
	object.ScenePtr = desc.ScenePtr;
	object.Roughness = desc.Roughness;
	object.Metallic = desc.Metallic;
	object.bOverrideRoughnessMetallic = desc.bOverrideRoughnessMetallic;
	object.bVisible = desc.bVisible;
	object.bRayTracing = desc.bRayTracing;
	object.bPhysicsQuery = desc.bPhysicsQuery;
	object.Transform = desc.Transform;
	SceneObjects.push_back(object);

	MarkRayTracingSceneDirty();
	MarkCpuPhysicsSceneDirty();
	return object.Handle;
}

Corona::SceneObjectHandle Corona::AddSceneInstance(const shared_ptr<Scene>& scene, const glm::mat4x4& transform)
{
	SceneObjectDesc desc;
	desc.ScenePtr = scene;
	desc.Transform = transform;
	return AddSceneObject(desc);
}

bool Corona::RemoveSceneObject(SceneObjectHandle handle)
{
	if (handle == InvalidSceneObjectHandle)
		return false;

	const auto it = std::find_if(SceneObjects.begin(), SceneObjects.end(), [handle](const SceneObject& object)
	{
		return object.Handle == handle;
	});
	if (it == SceneObjects.end())
		return false;

	SceneObjects.erase(it);
	ScriptObjects.erase(handle);
	if (SponzaObject == handle)
		SponzaObject = InvalidSceneObjectHandle;
	if (BuddhaObject == handle)
		BuddhaObject = InvalidSceneObjectHandle;
	if (ShaderBallObject == handle)
		ShaderBallObject = InvalidSceneObjectHandle;
	if (PistolObject == handle)
		PistolObject = InvalidSceneObjectHandle;
	if (MirrorCubeObject == handle)
		MirrorCubeObject = InvalidSceneObjectHandle;
	MarkRayTracingSceneDirty();
	MarkCpuPhysicsSceneDirty();
	return true;
}

bool Corona::SetSceneObjectTransform(SceneObjectHandle handle, const glm::mat4x4& transform)
{
	const auto it = std::find_if(SceneObjects.begin(), SceneObjects.end(), [handle](const SceneObject& object)
	{
		return object.Handle == handle;
	});
	if (it == SceneObjects.end())
		return false;

	it->Transform = transform;
	if (it->bVisible && it->bRayTracing)
		MarkRayTracingTransformsDirty();
	if (it->bVisible && it->bPhysicsQuery)
		MarkCpuPhysicsSceneDirty();
	return true;
}

bool Corona::SetSceneObjectVisibility(SceneObjectHandle handle, bool visible)
{
	const auto it = std::find_if(SceneObjects.begin(), SceneObjects.end(), [handle](const SceneObject& object)
	{
		return object.Handle == handle;
	});
	if (it == SceneObjects.end())
		return false;

	if (it->bVisible != visible)
	{
		it->bVisible = visible;
		MarkRayTracingSceneDirty();
		if (it->bPhysicsQuery)
			MarkCpuPhysicsSceneDirty();
	}
	return true;
}

bool Corona::SetSceneObjectRayTracingEnabled(SceneObjectHandle handle, bool enabled)
{
	const auto it = std::find_if(SceneObjects.begin(), SceneObjects.end(), [handle](const SceneObject& object)
	{
		return object.Handle == handle;
	});
	if (it == SceneObjects.end())
		return false;

	if (it->bRayTracing != enabled)
	{
		it->bRayTracing = enabled;
		MarkRayTracingSceneDirty();
	}
	return true;
}

void Corona::MarkRayTracingSceneDirty()
{
	bRayTracingSceneDirty = true;
	bRayTracingTransformDirty = false;
	PrevPathTracingViewMat = glm::mat4x4(0.0f);
}

void Corona::MarkRayTracingTransformsDirty()
{
	if (!bRayTracingSceneDirty)
		bRayTracingTransformDirty = true;
	PrevPathTracingViewMat = glm::mat4x4(0.0f);
}

void Corona::FlushSceneObjectChanges()
{
	if (!bRayTracingSceneDirty || !renderBackend)
	{
		if (bRayTracingTransformDirty && renderBackend)
			UpdateRayTracingInstanceTransforms();
		return;
	}

	RebuildAccelerationStructures();
}

void Corona::UpdateRayTracingInstanceTransforms()
{
	if (!renderBackend)
		return;

	if (!TLAS)
	{
		bRayTracingSceneDirty = true;
		bRayTracingTransformDirty = false;
		RebuildAccelerationStructures();
		return;
	}

	vector<RTInstanceDesc> updatedInstances;
	size_t meshCount = 0;
	for (const SceneObject& object : SceneObjects)
	{
		if (object.bVisible && object.bRayTracing && object.ScenePtr)
			meshCount += object.ScenePtr->meshes.size();
	}
	updatedInstances.reserve(meshCount);
	for (const SceneObject& object : SceneObjects)
	{
		if (object.bVisible && object.bRayTracing)
			AddMeshesToRayTracingInstances(updatedInstances, RayTracingBLASCache, object.ScenePtr, object.Transform);
	}

	if (updatedInstances.size() != RayTracingInstances.size())
	{
		bRayTracingSceneDirty = true;
		bRayTracingTransformDirty = false;
		RebuildAccelerationStructures();
		return;
	}

	RayTracingInstances = std::move(updatedInstances);
	if (!renderBackend->UpdateTLAS(TLAS, RayTracingInstances))
	{
		bRayTracingSceneDirty = true;
		bRayTracingTransformDirty = false;
		RebuildAccelerationStructures();
		return;
	}

	UpdateInstancePropertyBuffer();
	bRayTracingTransformDirty = false;
}

void Corona::UpdateInstancePropertyBuffer()
{
	constexpr UINT32 kMinInstancePropertyCapacity = 500u;
	const UINT32 instanceCapacity = std::max(kMinInstancePropertyCapacity, static_cast<UINT32>(RayTracingInstances.size()));
	std::vector<InstanceProperty> instanceProperties(instanceCapacity);
	const size_t instanceCount = (std::min)(instanceProperties.size(), RayTracingInstances.size());
	for (size_t i = 0; i < instanceCount; ++i)
	{
		instanceProperties[i].WorldMatrix = glm::transpose(RayTracingInstances[i].Transform);
		instanceProperties[i].VertexOffset = 0;
		instanceProperties[i].IndexOffset = 0;
		instanceProperties[i].Flags = RayTracingInstances[i].Flags;
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
	if (!renderBackend)
		return;

	const size_t previousInstanceCount = RayTracingInstances.size();

	// Wait for GPU to finish using current structures
	renderBackend->WaitForGpu();

	RayTracingInstances.clear();
	size_t meshCount = 0;
	vector<Mesh*> retainedMeshes;
	for (const SceneObject& object : SceneObjects)
	{
		if (!object.ScenePtr)
			continue;

		for (const shared_ptr<Mesh>& mesh : object.ScenePtr->meshes)
		{
			if (mesh && std::find(retainedMeshes.begin(), retainedMeshes.end(), mesh.get()) == retainedMeshes.end())
				retainedMeshes.push_back(mesh.get());
		}

		if (object.bVisible && object.bRayTracing)
			meshCount += object.ScenePtr->meshes.size();
	}
	RayTracingInstances.reserve(meshCount);
	for (const SceneObject& object : SceneObjects)
	{
		if (object.bVisible && object.bRayTracing)
			AddMeshesToRayTracingInstances(RayTracingInstances, RayTracingBLASCache, object.ScenePtr, object.Transform);
	}

	for (auto it = RayTracingBLASCache.begin(); it != RayTracingBLASCache.end();)
	{
		if (std::find(retainedMeshes.begin(), retainedMeshes.end(), it->first) == retainedMeshes.end())
			it = RayTracingBLASCache.erase(it);
		else
			++it;
	}

	const bool bInstanceCountChanged = previousInstanceCount != RayTracingInstances.size();
	if (RayTracingInstances.empty())
	{
		TLAS = nullptr;
	}
	else if (!TLAS || bInstanceCountChanged || !renderBackend->UpdateTLAS(TLAS, RayTracingInstances))
	{
		TLAS = renderBackend->CreateTLAS(RayTracingInstances);
	}
	
	// Update instance property buffer
	UpdateInstancePropertyBuffer();

	if (bInstanceCountChanged && (PSO_RT_SHADOW || PSO_RT_AO || PSO_RT_SKY_LIGHTING || PSO_RT_REFLECTION || PSO_RT_GI || PSO_RT_SCREEN_PROBE_GI || PSO_RT_SPATIAL_HASH_GI))
		InitRTPSO();
	if (PSO_PATH_TRACING)
		InitPathTracingPass();

	bRayTracingSceneDirty = false;
}

void Corona::InitRaytracingData()
{
	size_t NumTotalMesh = 0;
	for (const SceneObject& object : SceneObjects)
	{
		if (object.bVisible && object.bRayTracing && object.ScenePtr)
			NumTotalMesh += object.ScenePtr->meshes.size();
	}
	RayTracingInstances.reserve(NumTotalMesh);

	// Create initial instance property buffer (large enough for many instances)
	InstancePropertyBuffer = renderBackend->CreateBuffer({ 500u, sizeof(InstanceProperty), EInitialResourceState::GenericRead, false, nullptr });
	InstancePropertyBuffer->MakeByteAddressBufferSRV();
	NAME_D3D12_OBJECT(InstancePropertyBuffer->resource);

	bRayTracingSceneDirty = true;
	RebuildAccelerationStructures();
}

void Corona::InitRTPSO()
{
	const uint32_t maxSupportedHybridStage = renderBackend ? renderBackend->GetMaxSupportedHybridStage() : 7u;
	const bool bInitReflectionRT = !renderBackend || renderBackend->GetAPI() != ERenderBackendAPI::Vulkan || maxSupportedHybridStage >= 3u;
	const bool bInitGIRT = !renderBackend || renderBackend->GetAPI() != ERenderBackendAPI::Vulkan || maxSupportedHybridStage >= 4u;

	InitRaytracingShadowPass();
	InitRaytracingAOPass();
	InitRaytracingSkyLightingPass();
	if (bInitReflectionRT)
		InitRaytracingReflectionPass();
	if (bInitGIRT)
	{
		InitRaytracingSimpleGIPass();
		InitRaytracingScreenProbePass();
		InitRaytracingSpatialHashPass();
	}
}
