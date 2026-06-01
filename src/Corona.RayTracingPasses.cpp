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
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iterator>
#include <map>
#include <set>
#include <sstream>

void AppendCpuRuntimeTrace(const std::wstring& line);

namespace
{
	constexpr uint32_t kRTInstanceFlagAlphaTested = 1u << 0;

	std::wstring FormatRTInitMilliseconds(double milliseconds)
	{
		std::wostringstream stream;
		stream << std::fixed << std::setprecision(3) << milliseconds;
		return stream.str();
	}

	std::wstring FormatRTHex(uint64_t value)
	{
		std::wostringstream stream;
		stream << L"0x" << std::hex << std::uppercase << value;
		return stream.str();
	}

	double ElapsedRTInitMilliseconds(
		const std::chrono::steady_clock::time_point& begin,
		const std::chrono::steady_clock::time_point& end)
	{
		return std::chrono::duration<double, std::milli>(end - begin).count();
	}

	bool IsMeshRayTracingBuildable(const Mesh& mesh)
	{
		if (mesh.bProceduralGrass)
			return false;

		if (mesh.bSkeletalSkinned)
		{
			return mesh.SkeletalOutputVb &&
				mesh.Ib &&
				mesh.VertexStride > 0 &&
				mesh.SkeletalVertexCount > 0 &&
				mesh.Ib->numIndices >= 3;
		}

		return mesh.Vb &&
			mesh.Ib &&
			mesh.VertexStride > 0 &&
			mesh.Vb->numVertices > 0 &&
			mesh.Ib->numIndices >= 3;
	}

	size_t CountBuildableRayTracingMeshes(const shared_ptr<Scene>& scene)
	{
		if (!scene)
			return 0;

		size_t count = 0;
		for (const shared_ptr<Mesh>& mesh : scene->meshes)
		{
			if (mesh && IsMeshRayTracingBuildable(*mesh))
				++count;
		}
		return count;
	}

	void TraceSkippedRayTracingMesh(const Mesh& mesh)
	{
		static std::set<const Mesh*> loggedMeshes;
		if (!loggedMeshes.insert(&mesh).second)
			return;

		AppendCpuRuntimeTrace(
			L"[RTAS] skip non-buildable mesh"
			L", proceduralGrass=" + std::to_wstring(mesh.bProceduralGrass ? 1 : 0) +
			L", skeletal=" + std::to_wstring(mesh.bSkeletalSkinned ? 1 : 0) +
			L", hasVb=" + std::to_wstring(mesh.Vb ? 1 : 0) +
			L", hasIb=" + std::to_wstring(mesh.Ib ? 1 : 0) +
			L", hasSkeletalOutputVb=" + std::to_wstring(mesh.SkeletalOutputVb ? 1 : 0) +
			L", vertexStride=" + std::to_wstring(mesh.VertexStride) +
			L", vertices=" + std::to_wstring(mesh.Vb ? mesh.Vb->numVertices : 0) +
			L", skeletalVertices=" + std::to_wstring(mesh.SkeletalVertexCount) +
			L", indices=" + std::to_wstring(mesh.Ib ? mesh.Ib->numIndices : 0));
	}

	void AddMeshesToRayTracingInstances(
		vector<RTInstanceDesc>& instances,
		map<Mesh*, shared_ptr<RTAS>>& blasCache,
		const shared_ptr<Scene>& scene,
		const glm::mat4x4& instanceTransform,
		float roughness,
		float metallic,
		bool bOverrideRoughnessMetallic,
		bool& bBuildSuspended)
	{
		if (!scene || bBuildSuspended)
			return;

		for (auto& mesh : scene->meshes)
		{
			if (bBuildSuspended)
				return;
			if (!mesh)
				continue;
			if (!IsMeshRayTracingBuildable(*mesh))
			{
				TraceSkippedRayTracingMesh(*mesh);
				continue;
			}

			shared_ptr<RTAS> blas;
			const auto cacheIt = blasCache.find(mesh.get());
			if (cacheIt != blasCache.end())
			{
				blas = cacheIt->second;
				if (!blas)
				{
					bBuildSuspended = true;
					AppendCpuRuntimeTrace(
						L"[RTAS] suspended BLAS build after cached failure for mesh=" +
						FormatRTHex(reinterpret_cast<uint64_t>(mesh.get())));
					return;
				}
			}
			else
			{
				blas = mesh->CreateBLAS();
				// Cache null results too. A failed BLAS allocation should not be
				// retried every frame for the same scene; the cache is cleared on
				// scene/map rebuilds when bRayTracingBLASCacheResetPending is set.
				blasCache[mesh.get()] = blas;
				if (!blas)
				{
					bBuildSuspended = true;
					AppendCpuRuntimeTrace(
						L"[RTAS] cached failed BLAS build and suspended RTAS rebuild for mesh=" +
						FormatRTHex(reinterpret_cast<uint64_t>(mesh.get())));
					return;
				}
			}

			if (blas == nullptr)
				continue;

			RTInstanceDesc instance;
			instance.BottomLevelAS = blas;
			instance.Transform = instanceTransform * mesh->transform;
			instance.Roughness = roughness;
			instance.Metallic = metallic;
			instance.bOverrideRoughnessMetallic = bOverrideRoughnessMetallic ? 1u : 0u;
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
	object.PhysicsCollisionShape = desc.PhysicsCollisionShape;
	object.PhysicsBoxHalfExtent = desc.PhysicsBoxHalfExtent;
	object.Transform = desc.Transform;
	object.EntityHandle = desc.EntityHandle;
	if (EntityWorld.IsAlive(object.EntityHandle))
		UpdateSceneObjectEntity(object);
	else
		object.EntityHandle = CreateSceneObjectEntity(object);
	SceneObjects.push_back(object);

	MarkSceneObjectRenderDirty(object.Handle, kSceneObjectDirtyAll);
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

	const CoronaECS::Entity entity = it->EntityHandle;
	DestroyEntityScriptComponent(entity);
	MarkSceneObjectRenderRemoved(handle);
	SceneObjects.erase(it);
	EntityWorld.DestroyEntity(entity);
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
	UpdateSceneObjectEntity(*it);
	MarkSceneObjectRenderDirty(handle, kSceneObjectDirtyTransform);
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
		UpdateSceneObjectEntity(*it);
		MarkSceneObjectRenderDirty(handle, kSceneObjectDirtyVisibility);
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
		UpdateSceneObjectEntity(*it);
		MarkSceneObjectRenderDirty(handle, kSceneObjectDirtyRayTracing);
	}
	return true;
}

bool Corona::ShouldIncludeSceneObjectInRayTracingAS(const SceneObject& object) const
{
	if (!object.bVisible || !object.ScenePtr)
		return false;

	// In hybrid mode this flag lets script objects opt out of RT effects.
	// Path tracing has no raster fallback, so every visible object must be in the AS.
	return object.bRayTracing || RenderingMode == ERenderingMode::PATHTRACING;
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
	if (!renderBackend)
		return;

	const bool bMobileHybridDirectOnly =
		CORONA_PLATFORM_MOBILE &&
		RenderingMode == ERenderingMode::HYBRID;
#if CORONA_PLATFORM_MOBILE
	const bool bPlatformerHybridDirectOnly = false;
#else
	const bool bPlatformerHybridDirectOnly =
		RenderingMode == ERenderingMode::HYBRID &&
		StartupLuauMode == L"platformer";
#endif
	if (bMobileHybridDirectOnly || bPlatformerHybridDirectOnly || !renderBackend->SupportsRayTracing())
	{
		RayTracingInstances.clear();
		TLAS = nullptr;
		InstancePropertyBuffer = nullptr;
		TLASFrameResources.clear();
		TLASFrameInstanceCounts.clear();
		InstancePropertyFrameBuffers.clear();
		bRayTracingSceneDirty = false;
		bRayTracingTransformDirty = false;
		return;
	}

	if (!bRayTracingSceneDirty)
	{
		// Even when no transforms changed, a per-frame BLAS refit (e.g.
		// skeletal compute skinning) requires the TLAS to be re-issued each
		// frame: TLAS slots cycle with triple buffering, and a slot that
		// only saw an old activate (no UpdateTLAS / no Build) keeps a stale
		// reference to the BLAS data — observed as a one-frame-in-three
		// shadow drop when a skinned character was present. PERFORM_UPDATE
		// on the active TLAS slot every frame fixes the flicker; the cost
		// is microseconds for typical instance counts.
		UpdateRayTracingInstanceTransforms();
		return;
	}

	RebuildAccelerationStructures();
}

UINT32 Corona::GetRayTracingFrameResourceIndex() const
{
	if (!renderBackend)
		return 0;

	const UINT32 frameCount = std::max<UINT32>(1u, renderBackend->GetFrameCount());
	return std::min(renderBackend->GetCurrentFrameIndex(), frameCount - 1u);
}

void Corona::EnsureRayTracingFrameResourceSlots()
{
	const UINT32 frameCount = renderBackend ? std::max<UINT32>(1u, renderBackend->GetFrameCount()) : 1u;
	if (TLASFrameResources.size() != frameCount)
		TLASFrameResources.resize(frameCount);
	if (TLASFrameInstanceCounts.size() != frameCount)
		TLASFrameInstanceCounts.resize(frameCount, 0u);
	if (InstancePropertyFrameBuffers.size() != frameCount)
		InstancePropertyFrameBuffers.resize(frameCount);
}

void Corona::ActivateCurrentRayTracingFrameResources()
{
	EnsureRayTracingFrameResourceSlots();
	const UINT32 frameIndex = GetRayTracingFrameResourceIndex();
	if (frameIndex < TLASFrameResources.size() && TLASFrameResources[frameIndex])
		TLAS = TLASFrameResources[frameIndex];
	if (frameIndex < InstancePropertyFrameBuffers.size() && InstancePropertyFrameBuffers[frameIndex])
		InstancePropertyBuffer = InstancePropertyFrameBuffers[frameIndex];
}

bool Corona::IsCurrentRayTracingFrameResourceReady() const
{
	if (!renderBackend || RayTracingInstances.empty())
		return true;

	const UINT32 frameIndex = GetRayTracingFrameResourceIndex();
	if (frameIndex >= TLASFrameResources.size() || frameIndex >= TLASFrameInstanceCounts.size())
		return false;
	if (!TLASFrameResources[frameIndex])
		return false;
	if (TLASFrameInstanceCounts[frameIndex] != static_cast<UINT32>(RayTracingInstances.size()))
		return false;
	if (renderBackend->GetAPI() == ERenderBackendAPI::D3D12 &&
		(frameIndex >= InstancePropertyFrameBuffers.size() || !InstancePropertyFrameBuffers[frameIndex]))
	{
		return false;
	}
	return true;
}

void Corona::UpdateRayTracingInstanceTransforms()
{
	if (!renderBackend || !renderBackend->SupportsRayTracing())
		return;

	vector<RTInstanceDesc> updatedInstances;
	auto phaseStart = CpuClock::now();
	size_t meshCount = 0;
	for (const SceneObject& object : RenderWorld.SceneObjects)
	{
		if (ShouldIncludeSceneObjectInRayTracingAS(object))
			meshCount += CountBuildableRayTracingMeshes(object.ScenePtr);
	}
	updatedInstances.reserve(meshCount);
	for (const SceneObject& object : RenderWorld.SceneObjects)
	{
		if (ShouldIncludeSceneObjectInRayTracingAS(object))
			AddMeshesToRayTracingInstances(
				updatedInstances,
				RayTracingBLASCache,
				object.ScenePtr,
				object.Transform,
				object.Roughness,
				object.Metallic,
				object.bOverrideRoughnessMetallic,
				bRayTracingBLASBuildSuspended);
	}
	AddSceneFlushPhaseTiming(ESceneFlushPhase::UpdateGatherInstances, phaseStart, CpuClock::now());

	if (bRayTracingBLASBuildSuspended)
	{
		AppendCpuRuntimeTrace(
			L"[RTAS] transform update skipped after BLAS build suspension; falling back to raster for current scene"
			L", requestedBuildableMeshes=" + std::to_wstring(meshCount) +
			L", builtInstancesBeforeSuspend=" + std::to_wstring(updatedInstances.size()));
		RayTracingInstances.clear();
		TLAS = nullptr;
		bRayTracingSceneDirty = false;
		bRayTracingTransformDirty = false;
		return;
	}

	if (updatedInstances.empty())
	{
		RayTracingInstances.clear();
		TLAS = nullptr;
		bRayTracingTransformDirty = false;
		return;
	}

	if (RayTracingInstances.empty() || updatedInstances.size() != RayTracingInstances.size())
	{
		bRayTracingSceneDirty = true;
		bRayTracingTransformDirty = false;
		RebuildAccelerationStructures();
		return;
	}

	EnsureRayTracingFrameResourceSlots();
	const UINT32 frameIndex = GetRayTracingFrameResourceIndex();

	phaseStart = CpuClock::now();
	// Frame-indexed TLAS resources are protected by BeginFrame's per-frame fence.
	// The old single-buffered path had to WaitForGpu() here before in-place updates.
	AddSceneFlushPhaseTiming(ESceneFlushPhase::UpdateGpuWait, phaseStart, CpuClock::now());

	phaseStart = CpuClock::now();
	RayTracingInstances = std::move(updatedInstances);
	std::shared_ptr<RTAS>& frameTLAS = TLASFrameResources[frameIndex];
	const bool bCanUpdateFrameTLAS =
		frameTLAS &&
		frameIndex < TLASFrameInstanceCounts.size() &&
		TLASFrameInstanceCounts[frameIndex] == static_cast<UINT32>(RayTracingInstances.size());
	if (bCanUpdateFrameTLAS)
	{
		if (!renderBackend->UpdateTLAS(frameTLAS, RayTracingInstances))
			frameTLAS.reset();
	}
	if (!frameTLAS)
	{
		frameTLAS = renderBackend->CreateTLAS(RayTracingInstances);
	}
	if (!frameTLAS)
	{
		AddSceneFlushPhaseTiming(ESceneFlushPhase::UpdateTlas, phaseStart, CpuClock::now());
		bRayTracingSceneDirty = true;
		bRayTracingTransformDirty = false;
		RebuildAccelerationStructures();
		return;
	}
	TLASFrameInstanceCounts[frameIndex] = static_cast<UINT32>(RayTracingInstances.size());
	TLAS = frameTLAS;
	AddSceneFlushPhaseTiming(ESceneFlushPhase::UpdateTlas, phaseStart, CpuClock::now());

	phaseStart = CpuClock::now();
	UpdateInstancePropertyBuffer();
	AddSceneFlushPhaseTiming(ESceneFlushPhase::UpdateInstanceProperties, phaseStart, CpuClock::now());
	bRayTracingTransformDirty = false;
}

void Corona::UpdateInstancePropertyBuffer()
{
	if (!renderBackend)
		return;
#if CORONA_HAS_D3D12
	// The DX12 path lays InstanceProperty out in a ByteAddressBuffer that hit
	// shaders look up via instance ID. The Vulkan path manages its own
	// instance descriptor copy inside VulkanBackend, so this whole helper is
	// DX12-only.

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
		instanceProperties[i].bOverrideRoughnessMetallic = RayTracingInstances[i].bOverrideRoughnessMetallic;
		instanceProperties[i].RoughnessMetallic = glm::vec2(RayTracingInstances[i].Roughness, RayTracingInstances[i].Metallic);
	}

	auto ClearFailedD3D12FrameResources = [&](const wchar_t* reason)
	{
		AppendCpuRuntimeTrace(
			L"[RTAS] InstancePropertyBuffer unavailable: " + std::wstring(reason ? reason : L"unknown") +
			L", capacity=" + std::to_wstring(instanceCapacity) +
			L", instances=" + std::to_wstring(RayTracingInstances.size()));
		InstancePropertyBuffer = nullptr;
		TLAS = nullptr;
		std::fill(TLASFrameResources.begin(), TLASFrameResources.end(), std::shared_ptr<RTAS>());
		std::fill(TLASFrameInstanceCounts.begin(), TLASFrameInstanceCounts.end(), 0u);
		std::fill(InstancePropertyFrameBuffers.begin(), InstancePropertyFrameBuffers.end(), std::shared_ptr<Buffer>());
	};

	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::Vulkan)
	{
		try
		{
			InstancePropertyBuffer = renderBackend->CreateBuffer({
				instanceCapacity,
				sizeof(InstanceProperty),
				EInitialResourceState::ShaderRead,
				false,
				instanceProperties.data()
			});
		}
		catch (...)
		{
			AppendCpuRuntimeTrace(
				L"[RTAS] Vulkan InstancePropertyBuffer CreateBuffer threw"
				L", capacity=" + std::to_wstring(instanceCapacity) +
				L", instances=" + std::to_wstring(RayTracingInstances.size()));
			InstancePropertyBuffer = nullptr;
			return;
		}
		if (!InstancePropertyBuffer)
		{
			AppendCpuRuntimeTrace(
				L"[RTAS] Vulkan InstancePropertyBuffer CreateBuffer returned null"
				L", capacity=" + std::to_wstring(instanceCapacity) +
				L", instances=" + std::to_wstring(RayTracingInstances.size()));
			return;
		}
		InstancePropertyBuffer->MakeByteAddressBufferSRV();
		NAME_D3D12_OBJECT(InstancePropertyBuffer->resource);
		return;
	}

	EnsureRayTracingFrameResourceSlots();
	const UINT32 frameIndex = GetRayTracingFrameResourceIndex();
	std::shared_ptr<Buffer>& frameInstancePropertyBuffer = InstancePropertyFrameBuffers[frameIndex];
	if (!frameInstancePropertyBuffer || frameInstancePropertyBuffer->NumElements < instanceCapacity)
	{
		try
		{
			frameInstancePropertyBuffer = renderBackend->CreateBuffer({ instanceCapacity, sizeof(InstanceProperty), EInitialResourceState::GenericRead, false, nullptr });
		}
		catch (...)
		{
			ClearFailedD3D12FrameResources(L"DX12 CreateBuffer threw");
			return;
		}
		if (!frameInstancePropertyBuffer || !frameInstancePropertyBuffer->resource)
		{
			ClearFailedD3D12FrameResources(L"DX12 CreateBuffer returned null");
			return;
		}
		frameInstancePropertyBuffer->MakeByteAddressBufferSRV();
		NAME_D3D12_OBJECT(frameInstancePropertyBuffer->resource);
	}
	if (!frameInstancePropertyBuffer || !frameInstancePropertyBuffer->resource)
	{
		ClearFailedD3D12FrameResources(L"DX12 frame buffer missing resource");
		return;
	}
	InstancePropertyBuffer = frameInstancePropertyBuffer;

	uint8_t* pData = nullptr;
	const HRESULT mapResult = InstancePropertyBuffer->resource->Map(0, nullptr, reinterpret_cast<void**>(&pData));
	if (FAILED(mapResult) || !pData)
	{
		AppendCpuRuntimeTrace(
			L"[RTAS] InstancePropertyBuffer Map failed hr=" + FormatRTHex(static_cast<uint32_t>(mapResult)) +
			L", capacity=" + std::to_wstring(instanceCapacity) +
			L", instances=" + std::to_wstring(RayTracingInstances.size()));
		return;
	}

	memcpy(pData, instanceProperties.data(), instanceProperties.size() * sizeof(InstanceProperty));
	InstancePropertyBuffer->resource->Unmap(0, nullptr);
#endif // CORONA_HAS_D3D12 (UpdateInstancePropertyBuffer DX12 path)
}

void Corona::RebuildAccelerationStructures()
{
	if (!renderBackend || !renderBackend->SupportsRayTracing())
		return;

	const size_t previousInstanceCount = RayTracingInstances.size();

	// Wait for GPU to finish using current structures
	auto phaseStart = CpuClock::now();
	renderBackend->WaitForGpu();
	AddSceneFlushPhaseTiming(ESceneFlushPhase::RebuildGpuWait, phaseStart, CpuClock::now());
	EnsureRayTracingFrameResourceSlots();
	std::fill(TLASFrameResources.begin(), TLASFrameResources.end(), std::shared_ptr<RTAS>());
	std::fill(TLASFrameInstanceCounts.begin(), TLASFrameInstanceCounts.end(), 0u);
	std::fill(InstancePropertyFrameBuffers.begin(), InstancePropertyFrameBuffers.end(), std::shared_ptr<Buffer>());
	if (bRayTracingBLASCacheResetPending)
	{
		if (!RayTracingBLASCache.empty())
		{
			AppendCpuRuntimeTrace(
				L"[RTAS] cleared BLAS cache before scene rebuild, count=" +
				std::to_wstring(RayTracingBLASCache.size()));
		}
		RayTracingBLASCache.clear();
		bRayTracingBLASCacheResetPending = false;
		bRayTracingBLASBuildSuspended = false;
	}

	phaseStart = CpuClock::now();
	RayTracingInstances.clear();
	size_t meshCount = 0;
	vector<Mesh*> retainedMeshes;
	for (const SceneObject& object : RenderWorld.SceneObjects)
	{
		if (!object.ScenePtr)
			continue;

		for (const shared_ptr<Mesh>& mesh : object.ScenePtr->meshes)
		{
			if (mesh && std::find(retainedMeshes.begin(), retainedMeshes.end(), mesh.get()) == retainedMeshes.end())
				retainedMeshes.push_back(mesh.get());
		}

		if (ShouldIncludeSceneObjectInRayTracingAS(object))
			meshCount += CountBuildableRayTracingMeshes(object.ScenePtr);
	}
	RayTracingInstances.reserve(meshCount);
	for (const SceneObject& object : RenderWorld.SceneObjects)
	{
		if (ShouldIncludeSceneObjectInRayTracingAS(object))
			AddMeshesToRayTracingInstances(
				RayTracingInstances,
				RayTracingBLASCache,
				object.ScenePtr,
				object.Transform,
				object.Roughness,
				object.Metallic,
				object.bOverrideRoughnessMetallic,
				bRayTracingBLASBuildSuspended);
	}

	if (bRayTracingBLASBuildSuspended)
	{
		AppendCpuRuntimeTrace(
			L"[RTAS] BLAS build suspended; falling back to raster for current scene"
			L", requestedBuildableMeshes=" + std::to_wstring(meshCount) +
			L", builtInstancesBeforeSuspend=" + std::to_wstring(RayTracingInstances.size()));
		RayTracingInstances.clear();
	}

	for (auto it = RayTracingBLASCache.begin(); it != RayTracingBLASCache.end();)
	{
		if (std::find(retainedMeshes.begin(), retainedMeshes.end(), it->first) == retainedMeshes.end())
			it = RayTracingBLASCache.erase(it);
		else
			++it;
	}
	AddSceneFlushPhaseTiming(ESceneFlushPhase::RebuildGatherInstances, phaseStart, CpuClock::now());

	phaseStart = CpuClock::now();
	const bool bInstanceCountChanged = previousInstanceCount != RayTracingInstances.size();
	if (RayTracingInstances.empty())
	{
		TLAS = nullptr;
	}
	else
	{
		const UINT32 frameIndex = GetRayTracingFrameResourceIndex();
		std::shared_ptr<RTAS>& frameTLAS = TLASFrameResources[frameIndex];
		frameTLAS = renderBackend->CreateTLAS(RayTracingInstances);
		TLASFrameInstanceCounts[frameIndex] = frameTLAS ? static_cast<UINT32>(RayTracingInstances.size()) : 0u;
		TLAS = frameTLAS;
	}
	AddSceneFlushPhaseTiming(ESceneFlushPhase::RebuildTlas, phaseStart, CpuClock::now());
	
	// Update instance property buffer
	phaseStart = CpuClock::now();
	UpdateInstancePropertyBuffer();
	AddSceneFlushPhaseTiming(ESceneFlushPhase::RebuildInstanceProperties, phaseStart, CpuClock::now());

	phaseStart = CpuClock::now();
	if (bInstanceCountChanged && (PSO_RT_SHADOW || PSO_RT_AO || PSO_RT_SKY_LIGHTING || PSO_RT_REFLECTION || PSO_RT_GI || PSO_RT_SCREEN_PROBE_GI || PSO_RT_SPATIAL_HASH_GI))
		InitRTPSO();
	if (PSO_PATH_TRACING)
		InitPathTracingPass();
	AddSceneFlushPhaseTiming(ESceneFlushPhase::RebuildPipelineState, phaseStart, CpuClock::now());

	bRayTracingSceneDirty = false;
}

void Corona::InitRaytracingData()
{
	if (!renderBackend || !renderBackend->SupportsRayTracing())
		return;

	RenderWorld.SceneObjects = SceneObjects;
	for (SceneObject& object : RenderWorld.SceneObjects)
		object.RenderDirtyBits = 0;

	size_t NumTotalMesh = 0;
	for (const SceneObject& object : RenderWorld.SceneObjects)
	{
		if (ShouldIncludeSceneObjectInRayTracingAS(object))
			NumTotalMesh += CountBuildableRayTracingMeshes(object.ScenePtr);
	}
	RayTracingInstances.reserve(NumTotalMesh);

	bRayTracingSceneDirty = true;
	RebuildAccelerationStructures();
}

void Corona::InitRTPSO()
{
	const auto totalStart = std::chrono::steady_clock::now();
	const uint32_t maxSupportedHybridStage = renderBackend ? renderBackend->GetMaxSupportedHybridStage() : 7u;
	const bool bInitReflectionRT =
		(!renderBackend || renderBackend->GetAPI() != ERenderBackendAPI::Vulkan || maxSupportedHybridStage >= 3u);
	const bool bInitGIRT =
		(!renderBackend || renderBackend->GetAPI() != ERenderBackendAPI::Vulkan || maxSupportedHybridStage >= 4u);

	AppendCpuRuntimeTrace(
		L"[StartupTiming][RTPSO] begin maxSupportedHybridStage=" + std::to_wstring(maxSupportedHybridStage) +
		L", initReflection=" + std::to_wstring(bInitReflectionRT ? 1 : 0) +
		L", initGI=" + std::to_wstring(bInitGIRT ? 1 : 0));

	auto timePass = [](const wchar_t* name, const auto& initFunc)
	{
		const auto passStart = std::chrono::steady_clock::now();
		AppendCpuRuntimeTrace(L"[StartupTiming][RTPSO] begin pass=\"" + std::wstring(name) + L"\"");
		initFunc();
		const double elapsedMs = ElapsedRTInitMilliseconds(passStart, std::chrono::steady_clock::now());
		AppendCpuRuntimeTrace(
			L"[StartupTiming][RTPSO] pass=\"" + std::wstring(name) +
			L"\", elapsedMs=" + FormatRTInitMilliseconds(elapsedMs));
	};

	timePass(L"RaytracingShadow", [&]() { InitRaytracingShadowPass(); });
	timePass(L"ShadowSpatialReuse", [&]() { InitShadowSpatialReusePass(); });
	timePass(L"RaytracingAO", [&]() { InitRaytracingAOPass(); });
	timePass(L"RaytracingSkyLighting", [&]() { InitRaytracingSkyLightingPass(); });
	if (bInitReflectionRT)
		timePass(L"RaytracingReflection", [&]() { InitRaytracingReflectionPass(); });
	else
		AppendCpuRuntimeTrace(L"[StartupTiming][RTPSO] skip pass=\"RaytracingReflection\"");
	if (bInitGIRT)
	{
		timePass(L"RaytracingSimpleGI", [&]() { InitRaytracingSimpleGIPass(); });
		timePass(L"RaytracingScreenProbeGI", [&]() { InitRaytracingScreenProbePass(); });
		timePass(L"RaytracingSpatialHashGI", [&]() { InitRaytracingSpatialHashPass(); });
	}
	else
	{
		AppendCpuRuntimeTrace(L"[StartupTiming][RTPSO] skip pass=\"RaytracingSimpleGI\"");
		AppendCpuRuntimeTrace(L"[StartupTiming][RTPSO] skip pass=\"RaytracingScreenProbeGI\"");
		AppendCpuRuntimeTrace(L"[StartupTiming][RTPSO] skip pass=\"RaytracingSpatialHashGI\"");
	}

	const double totalMs = ElapsedRTInitMilliseconds(totalStart, std::chrono::steady_clock::now());
	AppendCpuRuntimeTrace(L"[StartupTiming][RTPSO] complete totalMs=" + FormatRTInitMilliseconds(totalMs));
}
