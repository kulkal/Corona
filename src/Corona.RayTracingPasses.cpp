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
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <unordered_set>

void AppendCpuRuntimeTrace(const std::wstring& line);

namespace
{
	constexpr uint32_t kRTInstanceFlagAlphaTested = 1u << 0;
	constexpr float kDefaultRtExtendedFrustumMargin = 8192.0f;

	using RtFrustumPlaneArray = std::array<glm::vec4, 6>;

	struct RtExtendedFrustumCullStats
	{
		uint64_t TotalObjects = 0;
		uint64_t IncludedObjects = 0;
		uint64_t CulledObjects = 0;
		uint64_t IncludedMeshes = 0;
	};

	enum class ERtFrustumAabbRelation : uint8_t
	{
		Outside,
		Intersect,
		Inside
	};

	bool IsRtExtendedFrustumCullingEnabled()
	{
		static const bool enabled = []
		{
			const char* value = std::getenv("CORONA_RT_EXTENDED_FRUSTUM_CULL");
			if (!value || value[0] == '\0')
				return false;
			return value[0] != '\0' && value[0] != '0';
		}();
		return enabled;
	}

	float GetRtExtendedFrustumMargin()
	{
		static const float margin = []
		{
			const char* value = std::getenv("CORONA_RT_EXTENDED_FRUSTUM_MARGIN");
			if (!value || value[0] == '\0')
				return kDefaultRtExtendedFrustumMargin;
			const float parsed = static_cast<float>(std::atof(value));
			return std::isfinite(parsed) ? std::max(0.0f, parsed) : kDefaultRtExtendedFrustumMargin;
		}();
		return margin;
	}

	bool IsFiniteMatrix(const glm::mat4x4& matrix)
	{
		for (int col = 0; col < 4; ++col)
		{
			for (int row = 0; row < 4; ++row)
			{
				if (!std::isfinite(matrix[col][row]))
					return false;
			}
		}
		return true;
	}

	RtFrustumPlaneArray BuildRtFrustumPlanes(const glm::mat4x4& viewProj)
	{
		auto row = [](const glm::mat4x4& matrix, int index)
		{
			return glm::vec4(matrix[0][index], matrix[1][index], matrix[2][index], matrix[3][index]);
		};
		auto normalizePlane = [](const glm::vec4& plane)
		{
			const glm::vec3 normal(plane.x, plane.y, plane.z);
			const float length = glm::length(normal);
			return length > 0.0f && std::isfinite(length) ? plane / length : plane;
		};
		const glm::vec4 row0 = row(viewProj, 0);
		const glm::vec4 row1 = row(viewProj, 1);
		const glm::vec4 row2 = row(viewProj, 2);
		const glm::vec4 row3 = row(viewProj, 3);
		return
		{
			normalizePlane(row3 + row0),
			normalizePlane(row3 - row0),
			normalizePlane(row3 + row1),
			normalizePlane(row3 - row1),
			normalizePlane(row3 + row2),
			normalizePlane(row3 - row2),
		};
	}

	ERtFrustumAabbRelation ClassifyAabbAgainstExtendedFrustum(
		const RtFrustumPlaneArray& planes,
		const glm::vec3& boundsMin,
		const glm::vec3& boundsMax,
		float extensionMargin)
	{
		const glm::vec3 center = (boundsMin + boundsMax) * 0.5f;
		const glm::vec3 extents = glm::max((boundsMax - boundsMin) * 0.5f, glm::vec3(0.0f));
		bool bFullyInside = true;
		for (const glm::vec4& plane : planes)
		{
			const glm::vec3 normal(plane.x, plane.y, plane.z);
			const float distance = glm::dot(normal, center) + plane.w;
			const float radius = glm::dot(glm::abs(normal), extents);
			if (distance + radius < -extensionMargin)
				return ERtFrustumAabbRelation::Outside;
			if (distance - radius < -extensionMargin)
				bFullyInside = false;
		}
		return bFullyInside ? ERtFrustumAabbRelation::Inside : ERtFrustumAabbRelation::Intersect;
	}

	bool IsAabbInsideExtendedFrustum(
		const RtFrustumPlaneArray& planes,
		const glm::vec3& boundsMin,
		const glm::vec3& boundsMax,
		float extensionMargin)
	{
		return ClassifyAabbAgainstExtendedFrustum(planes, boundsMin, boundsMax, extensionMargin) != ERtFrustumAabbRelation::Outside;
	}

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

	size_t AddMeshesToRayTracingInstances(
		vector<RTInstanceDesc>& instances,
		map<Mesh*, shared_ptr<RTAS>>& blasCache,
		const shared_ptr<Scene>& scene,
		const glm::mat4x4& instanceTransform,
		float roughness,
		float metallic,
		bool bOverrideRoughnessMetallic,
		uint32_t sceneObjectIndex,
		bool& bBuildSuspended)
	{
		if (!scene || bBuildSuspended)
			return 0;

		size_t addedInstanceCount = 0;
		for (auto& mesh : scene->meshes)
		{
			if (bBuildSuspended)
				return addedInstanceCount;
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
					return addedInstanceCount;
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
					return addedInstanceCount;
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
			instance.SceneObjectIndex = sceneObjectIndex;
			for (const Mesh::DrawCall& draw : mesh->Draws)
			{
				if (draw.mat && draw.mat->bHasAlpha)
				{
					instance.Flags |= kRTInstanceFlagAlphaTested;
					break;
				}
			}
			instances.push_back(instance);
			++addedInstanceCount;
		}
		return addedInstanceCount;
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
	if (bMapReplayInProgress)
	{
		object.RenderDirtyBits = 0;
	}
	else
	{
		object.RenderDirtyBits = kSceneObjectDirtyAll;
		DirtySceneObjectHandles.push_back(object.Handle);
	}
	SceneObjects.push_back(object);

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

bool Corona::IsRayTracingExtendedFrustumCullingActive() const
{
	return RenderingMode != ERenderingMode::PATHTRACING &&
		FrameCounter > 0 &&
		IsRtExtendedFrustumCullingEnabled() &&
		IsFiniteMatrix(UnjitteredViewProjMat);
}

bool Corona::ShouldIncludeSceneObjectInRayTracingExtendedFrustum(const SceneObject& object, bool& outFrustumCulled) const
{
	outFrustumCulled = false;
	if (!ShouldIncludeSceneObjectInRayTracingAS(object))
		return false;
	if (!IsRayTracingExtendedFrustumCullingActive())
		return true;

	glm::vec3 boundsMin(0.0f);
	glm::vec3 boundsMax(0.0f);
	glm::vec3 boundsCenter(0.0f);
	float boundsRadius = 0.0f;
	if (!GetSceneObjectWorldBounds(object, boundsMin, boundsMax, boundsCenter, boundsRadius))
		return true;

	const RtFrustumPlaneArray planes = BuildRtFrustumPlanes(UnjitteredViewProjMat);
	if (IsAabbInsideExtendedFrustum(planes, boundsMin, boundsMax, GetRtExtendedFrustumMargin()))
		return true;

	outFrustumCulled = true;
	return false;
}

const std::vector<uint32_t>& Corona::GatherRayTracingVisibleObjectIndices(uint64_t& outTotalObjects, uint64_t& outCulledObjects)
{
	outTotalObjects = 0;
	outCulledObjects = 0;
	RayTracingVisibleObjectIndices.clear();
	RayTracingSpatialFrustumCandidateIndices.clear();

	const bool bExtendedFrustumCullActive = IsRayTracingExtendedFrustumCullingActive();
	if (!bExtendedFrustumCullActive)
	{
		RayTracingVisibleObjectIndices.reserve(RenderWorld.SceneObjects.size());
		for (uint32_t objectIndex = 0; objectIndex < static_cast<uint32_t>(RenderWorld.SceneObjects.size()); ++objectIndex)
		{
			const SceneObject& object = RenderWorld.SceneObjects[objectIndex];
			if (!ShouldIncludeSceneObjectInRayTracingAS(object))
				continue;
			++outTotalObjects;
			RayTracingVisibleObjectIndices.push_back(objectIndex);
		}
		return RayTracingVisibleObjectIndices;
	}

	if (RenderWorld.bSceneObjectCullingIndexDirty ||
		RenderWorld.SceneObjectCullingData.size() != RenderWorld.SceneObjects.size())
	{
		RebuildRenderWorldCullingIndex();
	}

	const RtFrustumPlaneArray extendedFrustumPlanes = BuildRtFrustumPlanes(UnjitteredViewProjMat);
	const float extendedFrustumMargin = GetRtExtendedFrustumMargin();
	RayTracingVisibleObjectIndices.reserve(RenderWorld.SceneObjects.size() / 8u + RenderWorld.UnboundedSceneObjectIndices.size() + 64u);
	for (uint32_t objectIndex : RenderWorld.UnboundedSceneObjectIndices)
	{
		if (objectIndex >= RenderWorld.SceneObjects.size())
			continue;
		const SceneObject& object = RenderWorld.SceneObjects[objectIndex];
		if (!ShouldIncludeSceneObjectInRayTracingAS(object))
			continue;
		++outTotalObjects;
		RayTracingVisibleObjectIndices.push_back(objectIndex);
	}

	auto includeObjectIndex = [&](uint32_t objectIndex)
	{
		if (objectIndex >= RenderWorld.SceneObjects.size())
			return;
		const SceneObject& object = RenderWorld.SceneObjects[objectIndex];
		if (!ShouldIncludeSceneObjectInRayTracingAS(object))
			return;
		++outTotalObjects;
		RayTracingVisibleObjectIndices.push_back(objectIndex);
	};

	auto testObjectIndex = [&](uint32_t objectIndex)
	{
		if (objectIndex >= RenderWorld.SceneObjects.size())
			return;
		const SceneObject& object = RenderWorld.SceneObjects[objectIndex];
		if (!ShouldIncludeSceneObjectInRayTracingAS(object))
			return;
		++outTotalObjects;
		if (objectIndex >= RenderWorld.SceneObjectCullingData.size())
		{
			RayTracingVisibleObjectIndices.push_back(objectIndex);
			return;
		}
		const RenderWorldMirror::SceneObjectCullingRecord& objectData =
			RenderWorld.SceneObjectCullingData[objectIndex];
		if (!objectData.Visible)
			return;
		if (!objectData.HasBounds ||
			IsAabbInsideExtendedFrustum(extendedFrustumPlanes, objectData.BoundsMin, objectData.BoundsMax, extendedFrustumMargin))
		{
			RayTracingVisibleObjectIndices.push_back(objectIndex);
			return;
		}
		++outCulledObjects;
	};

	for (const RenderWorldMirror::SceneObjectCullingCell& cell : RenderWorld.SceneObjectCullingCells)
	{
		if (cell.ObjectIndices.empty())
			continue;

		const ERtFrustumAabbRelation cellRelation =
			ClassifyAabbAgainstExtendedFrustum(extendedFrustumPlanes, cell.BoundsMin, cell.BoundsMax, extendedFrustumMargin);
		if (cellRelation == ERtFrustumAabbRelation::Outside)
		{
			for (uint32_t objectIndex : cell.ObjectIndices)
			{
				if (objectIndex >= RenderWorld.SceneObjects.size())
					continue;
				if (!ShouldIncludeSceneObjectInRayTracingAS(RenderWorld.SceneObjects[objectIndex]))
					continue;
				++outTotalObjects;
				++outCulledObjects;
			}
			continue;
		}

		if (cellRelation == ERtFrustumAabbRelation::Inside)
		{
			for (uint32_t objectIndex : cell.ObjectIndices)
				includeObjectIndex(objectIndex);
			continue;
		}

		RayTracingSpatialFrustumCandidateIndices.insert(
			RayTracingSpatialFrustumCandidateIndices.end(),
			cell.ObjectIndices.begin(),
			cell.ObjectIndices.end());
	}

	for (uint32_t objectIndex : RayTracingSpatialFrustumCandidateIndices)
		testObjectIndex(objectIndex);

	return RayTracingVisibleObjectIndices;
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

void Corona::MarkRayTracingInstanceListChanged()
{
	++RayTracingInstancesRevision;
	if (RayTracingInstancesRevision == 0)
		RayTracingInstancesRevision = 1;
}

void Corona::FlushSceneObjectChanges()
{
	if (!renderBackend)
		return;
	if (renderBackend->IsDeviceLost())
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
		MarkRayTracingInstanceListChanged();
		RTGeometryRecordHash = 0;
		RTGeometryRecordBuffer.reset();
		TLAS = nullptr;
		InstancePropertyBuffer = nullptr;
		RTInstanceSceneObjectIndexBuffer = nullptr;
		TLASFrameResources.clear();
		TLASFrameInstanceCounts.clear();
		InstancePropertyFrameBuffers.clear();
		RTInstanceSceneObjectIndexFrameBuffers.clear();
		bRayTracingSceneDirty = false;
		bRayTracingTransformDirty = false;
		return;
	}

	if (!bRayTracingSceneDirty)
	{
		const bool bCameraDependentRayTracingAS = IsRayTracingExtendedFrustumCullingActive();
		// Even when no transforms changed, a per-frame BLAS refit (e.g.
		// skeletal compute skinning) requires the TLAS to be re-issued each
		// frame: TLAS slots cycle with triple buffering, and a slot that
		// only saw an old activate (no UpdateTLAS / no Build) keeps a stale
		// reference to the BLAS data — observed as a one-frame-in-three
		// shadow drop when a skinned character was present. PERFORM_UPDATE
		// on the active TLAS slot every frame fixes the flicker; the cost
		// is microseconds for typical instance counts.
		// Skip the forced update for static scenes after all frame slots are ready.
		const bool bNeedsPerFrameTlasUpdate = SkeletalStats.BlasUpdates > 0;
		if (!bRayTracingTransformDirty &&
			!bNeedsPerFrameTlasUpdate &&
			!bCameraDependentRayTracingAS &&
			IsCurrentRayTracingFrameResourceReady())
		{
			ActivateCurrentRayTracingFrameResources();
			return;
		}
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
	if (RTInstanceSceneObjectIndexFrameBuffers.size() != frameCount)
		RTInstanceSceneObjectIndexFrameBuffers.resize(frameCount);
}

void Corona::ActivateCurrentRayTracingFrameResources()
{
	EnsureRayTracingFrameResourceSlots();
	const UINT32 frameIndex = GetRayTracingFrameResourceIndex();
	if (frameIndex < TLASFrameResources.size() && TLASFrameResources[frameIndex])
		TLAS = TLASFrameResources[frameIndex];
	if (frameIndex < InstancePropertyFrameBuffers.size() && InstancePropertyFrameBuffers[frameIndex])
		InstancePropertyBuffer = InstancePropertyFrameBuffers[frameIndex];
	if (frameIndex < RTInstanceSceneObjectIndexFrameBuffers.size() && RTInstanceSceneObjectIndexFrameBuffers[frameIndex])
		RTInstanceSceneObjectIndexBuffer = RTInstanceSceneObjectIndexFrameBuffers[frameIndex];
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
	const ERenderBackendAPI backendAPI = renderBackend->GetAPI();
	if ((backendAPI == ERenderBackendAPI::D3D12 ||
		 backendAPI == ERenderBackendAPI::NRI) &&
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
	if (renderBackend->IsDeviceLost())
		return;

	vector<RTInstanceDesc> updatedInstances;
	auto phaseStart = CpuClock::now();
	RtExtendedFrustumCullStats cullStats;
	const bool bExtendedFrustumCullActive = IsRayTracingExtendedFrustumCullingActive();
	const float extendedFrustumMargin = bExtendedFrustumCullActive ? GetRtExtendedFrustumMargin() : 0.0f;
	const std::vector<uint32_t>& rtVisibleObjectIndices =
		GatherRayTracingVisibleObjectIndices(cullStats.TotalObjects, cullStats.CulledObjects);
	cullStats.IncludedObjects = rtVisibleObjectIndices.size();
	updatedInstances.reserve(std::max<size_t>(RayTracingInstances.size(), rtVisibleObjectIndices.size()));
	for (uint32_t objectIndex : rtVisibleObjectIndices)
	{
		if (objectIndex >= RenderWorld.SceneObjects.size())
			continue;
		const SceneObject& object = RenderWorld.SceneObjects[objectIndex];
		cullStats.IncludedMeshes += AddMeshesToRayTracingInstances(
			updatedInstances,
			RayTracingBLASCache,
			object.ScenePtr,
			object.Transform,
			object.Roughness,
			object.Metallic,
			object.bOverrideRoughnessMetallic,
			objectIndex,
			bRayTracingBLASBuildSuspended);
	}
	if (bExtendedFrustumCullActive && (FrameCounter % 120u) == 0u)
	{
		AppendCpuRuntimeTrace(
			L"[RTASFrustumCulling] update objects=" +
			std::to_wstring(cullStats.IncludedObjects) +
			L"/" + std::to_wstring(cullStats.TotalObjects) +
			L", culled=" + std::to_wstring(cullStats.CulledObjects) +
			L", meshes=" + std::to_wstring(cullStats.IncludedMeshes) +
			L", margin=" + std::to_wstring(extendedFrustumMargin) +
			L", instances=" + std::to_wstring(updatedInstances.size()));
	}
	AddSceneFlushPhaseTiming(ESceneFlushPhase::UpdateGatherInstances, phaseStart, CpuClock::now());

	if (bRayTracingBLASBuildSuspended)
	{
		static bool bLoggedTransformSuspend = false;
		if (!bLoggedTransformSuspend)
		{
			bLoggedTransformSuspend = true;
			AppendCpuRuntimeTrace(
				L"[RTAS] transform update skipped after BLAS build suspension; falling back to raster for current scene"
				L", addedInstancesBeforeSuspend=" + std::to_wstring(cullStats.IncludedMeshes) +
				L", builtInstancesBeforeSuspend=" + std::to_wstring(updatedInstances.size()) +
				L" (further occurrences suppressed)");
		}
		RayTracingInstances.clear();
		MarkRayTracingInstanceListChanged();
		RTGeometryRecordHash = 0;
		RTGeometryRecordBuffer.reset();
		TLAS = nullptr;
		bRayTracingSceneDirty = false;
		bRayTracingTransformDirty = false;
		return;
	}

	if (updatedInstances.empty())
	{
		RayTracingInstances.clear();
		MarkRayTracingInstanceListChanged();
		RTGeometryRecordHash = 0;
		RTGeometryRecordBuffer.reset();
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
	const bool bInstanceBindingsChanged =
		RayTracingInstances.size() != updatedInstances.size() ||
		!std::equal(
			RayTracingInstances.begin(),
			RayTracingInstances.end(),
			updatedInstances.begin(),
			[](const RTInstanceDesc& a, const RTInstanceDesc& b)
			{
				return a.BottomLevelAS.get() == b.BottomLevelAS.get();
			});
	RayTracingInstances = std::move(updatedInstances);
	if (bInstanceBindingsChanged)
		MarkRayTracingInstanceListChanged();
	std::shared_ptr<RTAS>& frameTLAS = TLASFrameResources[frameIndex];
	const bool bCanUpdateFrameTLAS =
		frameTLAS &&
		frameIndex < TLASFrameInstanceCounts.size() &&
		TLASFrameInstanceCounts[frameIndex] == static_cast<UINT32>(RayTracingInstances.size());
	if (bCanUpdateFrameTLAS)
	{
		if (!renderBackend->UpdateTLAS(frameTLAS, RayTracingInstances))
			frameTLAS.reset();
		if (renderBackend->IsDeviceLost())
			return;
	}
	if (!frameTLAS)
	{
		frameTLAS = renderBackend->CreateTLAS(RayTracingInstances);
		if (renderBackend->IsDeviceLost())
			return;
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
	if (renderBackend->IsDeviceLost())
		return;
	bRayTracingTransformDirty = false;
}

void Corona::UpdateInstancePropertyBuffer()
{
	if (!renderBackend)
		return;
	if (renderBackend->IsDeviceLost())
		return;

	constexpr uint32_t kMinInstancePropertyCapacity = 500u;
	const uint32_t instanceCapacity = std::max(kMinInstancePropertyCapacity, static_cast<uint32_t>(RayTracingInstances.size()));
	InstancePropertyUploadScratch.resize(instanceCapacity);
	RTInstanceSceneObjectIndexUploadScratch.assign(instanceCapacity, UINT32_MAX);
	const size_t instanceCount = (std::min)(InstancePropertyUploadScratch.size(), RayTracingInstances.size());
	for (size_t i = 0; i < instanceCount; ++i)
	{
		InstanceProperty& property = InstancePropertyUploadScratch[i];
		property.WorldMatrix = glm::transpose(RayTracingInstances[i].Transform);
		property.VertexOffset = 0;
		property.IndexOffset = 0;
		property.Flags = RayTracingInstances[i].Flags;
		property.bOverrideRoughnessMetallic = RayTracingInstances[i].bOverrideRoughnessMetallic;
		property.RoughnessMetallic = glm::vec2(RayTracingInstances[i].Roughness, RayTracingInstances[i].Metallic);
		RTInstanceSceneObjectIndexUploadScratch[i] = RayTracingInstances[i].SceneObjectIndex;
	}

	const uint32_t instancePropertyBytes = static_cast<uint32_t>(InstancePropertyUploadScratch.size() * sizeof(InstanceProperty));
	const uint32_t instanceSceneObjectIndexBytes =
		static_cast<uint32_t>(RTInstanceSceneObjectIndexUploadScratch.size() * sizeof(uint32_t));

	auto ClearFailedFrameResources = [&](const wchar_t* reason)
	{
		const uint32_t failedFrameIndex = GetRayTracingFrameResourceIndex();
		AppendCpuRuntimeTrace(
			L"[RTAS] InstancePropertyBuffer unavailable: " + std::wstring(reason ? reason : L"unknown") +
			L", capacity=" + std::to_wstring(instanceCapacity) +
			L", instances=" + std::to_wstring(RayTracingInstances.size()) +
			L", frameIndex=" + std::to_wstring(failedFrameIndex));
		InstancePropertyBuffer = nullptr;
		RTInstanceSceneObjectIndexBuffer = nullptr;
		TLAS = nullptr;
		if (failedFrameIndex < TLASFrameResources.size())
			TLASFrameResources[failedFrameIndex].reset();
		if (failedFrameIndex < TLASFrameInstanceCounts.size())
			TLASFrameInstanceCounts[failedFrameIndex] = 0u;
		if (failedFrameIndex < InstancePropertyFrameBuffers.size())
			InstancePropertyFrameBuffers[failedFrameIndex].reset();
		if (failedFrameIndex < RTInstanceSceneObjectIndexFrameBuffers.size())
			RTInstanceSceneObjectIndexFrameBuffers[failedFrameIndex].reset();
	};

	EnsureRayTracingFrameResourceSlots();
	const uint32_t frameIndex = GetRayTracingFrameResourceIndex();
	if (frameIndex >= InstancePropertyFrameBuffers.size())
	{
		ClearFailedFrameResources(L"frame resource slot unavailable");
		return;
	}

	std::shared_ptr<Buffer>& frameInstancePropertyBuffer = InstancePropertyFrameBuffers[frameIndex];
	std::shared_ptr<Buffer>& frameInstanceSceneObjectIndexBuffer = RTInstanceSceneObjectIndexFrameBuffers[frameIndex];
	std::wstring failureReason;
	if (!renderBackend->CreateOrUpdateRayTracingInstancePropertyBuffer(
		frameInstancePropertyBuffer,
		instanceCapacity,
		sizeof(InstanceProperty),
		InstancePropertyUploadScratch.data(),
		instancePropertyBytes,
		&failureReason))
	{
		ClearFailedFrameResources(failureReason.empty() ? L"backend update failed" : failureReason.c_str());
		return;
	}
	failureReason.clear();
	if (!renderBackend->CreateOrUpdateRayTracingInstancePropertyBuffer(
		frameInstanceSceneObjectIndexBuffer,
		instanceCapacity,
		sizeof(uint32_t),
		RTInstanceSceneObjectIndexUploadScratch.data(),
		instanceSceneObjectIndexBytes,
		&failureReason))
	{
		ClearFailedFrameResources(
			failureReason.empty() ? L"rt instance scene-object index update failed" : failureReason.c_str());
		return;
	}
	if (!frameInstancePropertyBuffer)
	{
		ClearFailedFrameResources(L"backend returned null buffer");
		return;
	}
	if (!frameInstanceSceneObjectIndexBuffer)
	{
		ClearFailedFrameResources(L"backend returned null scene-object index buffer");
		return;
	}
	InstancePropertyBuffer = frameInstancePropertyBuffer;
	RTInstanceSceneObjectIndexBuffer = frameInstanceSceneObjectIndexBuffer;
}

void Corona::RebuildAccelerationStructures()
{
	if (!renderBackend || !renderBackend->SupportsRayTracing())
		return;
	if (renderBackend->IsDeviceLost())
		return;

	const size_t previousInstanceCount = RayTracingInstances.size();

	// Wait for GPU to finish using current structures
	auto phaseStart = CpuClock::now();
	renderBackend->WaitForGpu();
	AddSceneFlushPhaseTiming(ESceneFlushPhase::RebuildGpuWait, phaseStart, CpuClock::now());
	if (renderBackend->IsDeviceLost())
		return;
	EnsureRayTracingFrameResourceSlots();
	std::fill(TLASFrameResources.begin(), TLASFrameResources.end(), std::shared_ptr<RTAS>());
	std::fill(TLASFrameInstanceCounts.begin(), TLASFrameInstanceCounts.end(), 0u);
	std::fill(InstancePropertyFrameBuffers.begin(), InstancePropertyFrameBuffers.end(), std::shared_ptr<Buffer>());
	std::fill(RTInstanceSceneObjectIndexFrameBuffers.begin(), RTInstanceSceneObjectIndexFrameBuffers.end(), std::shared_ptr<Buffer>());
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
	MarkRayTracingInstanceListChanged();
	RTGeometryRecordHash = 0;
	RTGeometryRecordBuffer.reset();
	RtExtendedFrustumCullStats cullStats;
	const bool bExtendedFrustumCullActive = IsRayTracingExtendedFrustumCullingActive();
	const float extendedFrustumMargin = bExtendedFrustumCullActive ? GetRtExtendedFrustumMargin() : 0.0f;
	std::unordered_set<Mesh*> retainedMeshes;
	retainedMeshes.reserve(RenderWorld.SceneObjects.size() * 2u);
	for (const SceneObject& object : RenderWorld.SceneObjects)
	{
		if (!object.ScenePtr)
			continue;

		for (const shared_ptr<Mesh>& mesh : object.ScenePtr->meshes)
		{
			if (mesh)
				retainedMeshes.insert(mesh.get());
		}
	}
	const std::vector<uint32_t>& rtVisibleObjectIndices =
		GatherRayTracingVisibleObjectIndices(cullStats.TotalObjects, cullStats.CulledObjects);
	cullStats.IncludedObjects = rtVisibleObjectIndices.size();
	RayTracingInstances.reserve(std::max<size_t>(RayTracingInstances.capacity(), rtVisibleObjectIndices.size()));
	for (uint32_t objectIndex : rtVisibleObjectIndices)
	{
		if (objectIndex >= RenderWorld.SceneObjects.size())
			continue;
		const SceneObject& object = RenderWorld.SceneObjects[objectIndex];
		cullStats.IncludedMeshes += AddMeshesToRayTracingInstances(
			RayTracingInstances,
			RayTracingBLASCache,
			object.ScenePtr,
			object.Transform,
			object.Roughness,
			object.Metallic,
			object.bOverrideRoughnessMetallic,
			objectIndex,
			bRayTracingBLASBuildSuspended);
	}
	if (bExtendedFrustumCullActive)
	{
		AppendCpuRuntimeTrace(
			L"[RTASFrustumCulling] rebuild objects=" +
			std::to_wstring(cullStats.IncludedObjects) +
			L"/" + std::to_wstring(cullStats.TotalObjects) +
			L", culled=" + std::to_wstring(cullStats.CulledObjects) +
			L", meshes=" + std::to_wstring(cullStats.IncludedMeshes) +
			L", margin=" + std::to_wstring(extendedFrustumMargin) +
			L", instances=" + std::to_wstring(RayTracingInstances.size()));
	}

	if (bRayTracingBLASBuildSuspended)
	{
		AppendCpuRuntimeTrace(
			L"[RTAS] BLAS build suspended; falling back to raster for current scene"
			L", addedInstancesBeforeSuspend=" + std::to_wstring(cullStats.IncludedMeshes) +
		L", builtInstancesBeforeSuspend=" + std::to_wstring(RayTracingInstances.size()));
		RayTracingInstances.clear();
		MarkRayTracingInstanceListChanged();
		RTGeometryRecordHash = 0;
		RTGeometryRecordBuffer.reset();
	}

	for (auto it = RayTracingBLASCache.begin(); it != RayTracingBLASCache.end();)
	{
		if (retainedMeshes.find(it->first) == retainedMeshes.end())
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
		if (renderBackend->IsDeviceLost())
		{
			AddSceneFlushPhaseTiming(ESceneFlushPhase::RebuildTlas, phaseStart, CpuClock::now());
			return;
		}
		TLASFrameInstanceCounts[frameIndex] = frameTLAS ? static_cast<UINT32>(RayTracingInstances.size()) : 0u;
		TLAS = frameTLAS;
	}
	AddSceneFlushPhaseTiming(ESceneFlushPhase::RebuildTlas, phaseStart, CpuClock::now());
	
	// Update instance property buffer
	phaseStart = CpuClock::now();
	UpdateInstancePropertyBuffer();
	AddSceneFlushPhaseTiming(ESceneFlushPhase::RebuildInstanceProperties, phaseStart, CpuClock::now());
	if (renderBackend->IsDeviceLost())
		return;
	if (!IsCurrentRayTracingFrameResourceReady())
	{
		AppendCpuRuntimeTrace(
			L"[RTAS] rebuild incomplete; skipping RT pipeline refresh"
			L", instances=" + std::to_wstring(RayTracingInstances.size()) +
			L", currentFrame=" + std::to_wstring(GetRayTracingFrameResourceIndex()));
		bRayTracingSceneDirty = true;
		return;
	}

	phaseStart = CpuClock::now();
	if (bInstanceCountChanged && (PSO_RT_SHADOW || PSO_RT_AO || PSO_RT_REFLECTION || PSO_RT_GI || PSO_RT_SCREEN_PROBE_GI || PSO_RT_SPATIAL_HASH_GI))
		InitRTPSO();
	if (PSO_PATH_TRACING)
		InitPathTracingPass();
	AddSceneFlushPhaseTiming(ESceneFlushPhase::RebuildPipelineState, phaseStart, CpuClock::now());

	bRayTracingSceneDirty = false;
}

void Corona::InitRaytracingData()
{
	if (!renderBackend)
		return;

	// Sync the render-world scene-object snapshot regardless of ray-tracing
	// support: the raster GBuffer pass iterates RenderWorld.SceneObjects, so a
	// backend with RT disabled (e.g. NRI today, GetMaxSupportedHybridStage()==0)
	// still needs this snapshot or it renders an empty scene.
	RenderWorld.SceneObjects = SceneObjects;
	for (SceneObject& object : RenderWorld.SceneObjects)
	{
		object.RenderDirtyBits = 0;
		object.bWorldBoundsCacheValid = false;
	}
	MarkRenderWorldCullingIndexDirty();

	if (!renderBackend->SupportsRayTracing())
		return; // RenderWorld is synced for raster; skip building RT acceleration structures

	bRayTracingSceneDirty = true;
	RebuildAccelerationStructures();
}

void Corona::InitRTPSO()
{
	const auto totalStart = std::chrono::steady_clock::now();
	const uint32_t maxSupportedHybridStage = renderBackend ? renderBackend->GetMaxSupportedHybridStage() : 7u;
	// Hybrid stages are cumulative. Stage 1 only brings up ray-traced direct
	// shadows, so keep later reflection/GI pipelines disabled until the backend
	// explicitly advertises the resources those passes require.
	const bool bBackendSupportsRT = maxSupportedHybridStage >= 1u;
	const bool bPartialHybridDiffuseGIBringup =
		maxSupportedHybridStage >= 4u &&
		maxSupportedHybridStage < 7u;
	// AO/reflections are decoupled from the NRI "simple GI" flag now that the RT
	// pipeline (bindless tables + SBT + accel descriptors) is solid — bringup only
	// still selects simple GI over screen-probe / spatial-hash GI below.
	const bool bInitReflectionRT = bBackendSupportsRT && maxSupportedHybridStage >= 3u;
	const bool bInitGIRT = bBackendSupportsRT && maxSupportedHybridStage >= 4u;

	AppendCpuRuntimeTrace(
		L"[StartupTiming][RTPSO] begin maxSupportedHybridStage=" + std::to_wstring(maxSupportedHybridStage) +
		L", backendSupportsRT=" + std::to_wstring(bBackendSupportsRT ? 1 : 0) +
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

	if (!bBackendSupportsRT)
	{
		AppendCpuRuntimeTrace(L"[StartupTiming][RTPSO] skip all passes (backend has no ray tracing support)");
		AppendCpuRuntimeTrace(L"[StartupTiming][RTPSO] complete totalMs=0");
		return;
	}
	// Partial hybrid bringup: RT pipelines are deferred-built at first Apply, and
	// only the RT shadow pass is dispatched (GI uses the compute fallback,
	// reflection/AO are gated off). So set up the shadow pass and let the rest be
	// declared but never built.

	timePass(L"ShadowRayQuery", [&]() { InitShadowRayQueryPass(); });
	if (!PSO_SHADOW_RAYQUERY)
		timePass(L"RaytracingShadow", [&]() { InitRaytracingShadowPass(); });
	else
		AppendCpuRuntimeTrace(L"[StartupTiming][RTPSO] skip pass=\"RaytracingShadow\" (ShadowRayQuery active)");
	if (maxSupportedHybridStage >= 2u)
	{
		timePass(L"ShadowSpatialReuse", [&]() { InitShadowSpatialReusePass(); });
		timePass(L"RaytracingAO", [&]() { InitRaytracingAOPass(); });
	}
	else
	{
		AppendCpuRuntimeTrace(L"[StartupTiming][RTPSO] skip pass=\"ShadowSpatialReuse\"");
		AppendCpuRuntimeTrace(L"[StartupTiming][RTPSO] skip pass=\"RaytracingAO\"");
	}
	if (bInitReflectionRT)
		timePass(L"RaytracingReflection", [&]() { InitRaytracingReflectionPass(); });
	else
		AppendCpuRuntimeTrace(L"[StartupTiming][RTPSO] skip pass=\"RaytracingReflection\"");
	if (bInitGIRT)
	{
		timePass(L"RaytracingSimpleGI", [&]() { InitRaytracingSimpleGIPass(); });
		// Spatial-hash GI is brought up on partial hybrid backends (its RT cell-trace PSO uses
		// the same bindless RT path as simple GI; the rest are compute passes).
		// Screen-probe GI stays off during partial hybrid bring-up.
		if (bPartialHybridDiffuseGIBringup)
		{
			AppendCpuRuntimeTrace(L"[StartupTiming][RTPSO] skip pass=\"RaytracingScreenProbeGI\"");
			timePass(L"RaytracingSpatialHashGI", [&]() { InitRaytracingSpatialHashPass(); });
		}
		else
		{
			timePass(L"RaytracingScreenProbeGI", [&]() { InitRaytracingScreenProbePass(); });
			timePass(L"RaytracingSpatialHashGI", [&]() { InitRaytracingSpatialHashPass(); });
		}
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
