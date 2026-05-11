//*********************************************************
//
// CPU PhysX scene queries.
//
//*********************************************************

#include "stdafx.h"
#include "Corona.h"
#include "Utils.h"

#include "PxPhysicsAPI.h"
#include "cooking/PxCooking.h"
#include "cooking/PxTriangleMeshDesc.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <malloc.h>
#include <memory>
#include <unordered_map>

void AppendCpuRuntimeTrace(const std::wstring& line);

namespace
{
	using namespace physx;

	constexpr float kCameraPhysicsRadius = 24.0f;
	constexpr float kCameraPhysicsSkin = 1.0f;
	constexpr int kCameraPhysicsSweepIterations = 3;

	PxVec3 ToPxVec3(const glm::vec3& value)
	{
		return PxVec3(value.x, value.y, value.z);
	}

	glm::vec3 FromPxVec3(const PxVec3& value)
	{
		return glm::vec3(value.x, value.y, value.z);
	}

	class CoronaPhysXAllocator : public PxAllocatorCallback
	{
	public:
		void* allocate(size_t size, const char*, const char*, int) override
		{
			return _aligned_malloc(size, 16);
		}

		void deallocate(void* ptr) override
		{
			_aligned_free(ptr);
		}
	};

	class CoronaPhysXErrorCallback : public PxErrorCallback
	{
	public:
		void reportError(PxErrorCode::Enum, const char* message, const char* file, int line) override
		{
			std::wstring text = L"[PhysX] ";
			if (file)
				text += AnsiToWString(file) + L":" + std::to_wstring(line) + L" ";
			if (message)
				text += AnsiToWString(message);
			AppendCpuRuntimeTrace(text);
		}
	};

	class ImmediateCpuDispatcher : public PxCpuDispatcher
	{
	public:
		void submitTask(PxBaseTask& task) override
		{
			task.run();
			task.release();
		}

		PxU32 getWorkerCount() const override
		{
			return 1;
		}
	};

	class MemoryOutputStream : public PxOutputStream
	{
	public:
		PxU32 write(const void* src, PxU32 count) override
		{
			const PxU8* bytes = static_cast<const PxU8*>(src);
			Data.insert(Data.end(), bytes, bytes + count);
			return count;
		}

		std::vector<PxU8> Data;
	};

	class MemoryInputData : public PxInputData
	{
	public:
		MemoryInputData(const PxU8* data, PxU32 size) :
			Data(data),
			Size(size)
		{
		}

		PxU32 read(void* dest, PxU32 count) override
		{
			const PxU32 remaining = Size - Position;
			const PxU32 readCount = std::min(count, remaining);
			if (readCount > 0)
			{
				memcpy(dest, Data + Position, readCount);
				Position += readCount;
			}
			return readCount;
		}

		PxU32 getLength() const override
		{
			return Size;
		}

		void seek(PxU32 pos) override
		{
			Position = std::min(pos, Size);
		}

		PxU32 tell() const override
		{
			return Position;
		}

	private:
		const PxU8* Data = nullptr;
		PxU32 Size = 0;
		PxU32 Position = 0;
	};

	PxFilterFlags CoronaSimulationFilterShader(
		PxFilterObjectAttributes,
		PxFilterData,
		PxFilterObjectAttributes,
		PxFilterData,
		PxPairFlags& pairFlags,
		const void*,
		PxU32)
	{
		pairFlags = PxPairFlag::eCONTACT_DEFAULT;
		return PxFilterFlag::eDEFAULT;
	}

	struct DecomposedTransform
	{
		PxTransform Pose = PxTransform(PxIdentity);
		PxVec3 Scale = PxVec3(1.0f);
		bool bValid = false;
	};

	DecomposedTransform DecomposeTransform(const glm::mat4x4& transform)
	{
		DecomposedTransform result;
		glm::vec3 basisX(transform[0]);
		glm::vec3 basisY(transform[1]);
		glm::vec3 basisZ(transform[2]);

		const float scaleX = glm::length(basisX);
		const float scaleY = glm::length(basisY);
		const float scaleZ = glm::length(basisZ);
		if (scaleX < 1.0e-6f || scaleY < 1.0e-6f || scaleZ < 1.0e-6f)
			return result;

		basisX /= scaleX;
		basisY /= scaleY;
		basisZ /= scaleZ;
		glm::mat3 rotationMatrix;
		rotationMatrix[0] = basisX;
		rotationMatrix[1] = basisY;
		rotationMatrix[2] = basisZ;
		glm::quat rotation = glm::normalize(glm::quat_cast(rotationMatrix));

		result.Pose = PxTransform(
			ToPxVec3(glm::vec3(transform[3])),
			PxQuat(rotation.x, rotation.y, rotation.z, rotation.w).getNormalized());
		result.Scale = PxVec3(scaleX, scaleY, scaleZ);
		result.bValid = result.Pose.isValid() && result.Scale.x > 0.0f && result.Scale.y > 0.0f && result.Scale.z > 0.0f;
		return result;
	}
}

using namespace physx;

struct Corona::CpuPhysicsState
{
	struct ActorEntry
	{
		PxRigidStatic* Actor = nullptr;
		SceneObjectHandle Handle = InvalidSceneObjectHandle;
	};

	~CpuPhysicsState()
	{
		ClearSceneActors();

		for (auto& entry : MeshCache)
		{
			if (entry.second)
				entry.second->release();
		}
		MeshCache.clear();

		if (Scene)
		{
			Scene->release();
			Scene = nullptr;
		}
		if (Material)
		{
			Material->release();
			Material = nullptr;
		}
		if (Physics)
		{
			Physics->release();
			Physics = nullptr;
		}
		if (Foundation)
		{
			Foundation->release();
			Foundation = nullptr;
		}
	}

	void ClearSceneActors()
	{
		if (Scene)
		{
			for (ActorEntry& entry : Actors)
			{
				if (entry.Actor)
				{
					Scene->removeActor(*entry.Actor);
					entry.Actor->release();
					entry.Actor = nullptr;
				}
			}
		}
		Actors.clear();
	}

	PxTriangleMesh* GetOrCreateTriangleMesh(Mesh* mesh)
	{
		if (!mesh || mesh->CpuPositions.empty() || mesh->CpuIndices.size() < 3)
			return nullptr;

		const auto cacheIt = MeshCache.find(mesh);
		if (cacheIt != MeshCache.end())
			return cacheIt->second;

		if (!CookingParams || !Physics)
			return nullptr;

		std::vector<PxVec3> vertices;
		vertices.reserve(mesh->CpuPositions.size());
		for (const glm::vec3& position : mesh->CpuPositions)
			vertices.push_back(ToPxVec3(position));

		PxTriangleMeshDesc desc;
		desc.points.count = static_cast<PxU32>(vertices.size());
		desc.points.stride = sizeof(PxVec3);
		desc.points.data = vertices.data();
		desc.triangles.count = static_cast<PxU32>(mesh->CpuIndices.size() / 3);
		desc.triangles.stride = sizeof(UINT32) * 3;
		desc.triangles.data = mesh->CpuIndices.data();

		if (!desc.isValid())
		{
			AppendCpuRuntimeTrace(L"[PhysX] invalid triangle mesh desc");
			return nullptr;
		}

		MemoryOutputStream output;
		if (!PxCookTriangleMesh(*CookingParams, desc, output) || output.Data.empty())
		{
			AppendCpuRuntimeTrace(L"[PhysX] failed to cook triangle mesh");
			return nullptr;
		}

		MemoryInputData input(output.Data.data(), static_cast<PxU32>(output.Data.size()));
		PxTriangleMesh* triangleMesh = Physics->createTriangleMesh(input);
		if (!triangleMesh)
		{
			AppendCpuRuntimeTrace(L"[PhysX] failed to create triangle mesh");
			return nullptr;
		}

		MeshCache[mesh] = triangleMesh;
		return triangleMesh;
	}

	CoronaPhysXAllocator Allocator;
	CoronaPhysXErrorCallback ErrorCallback;
	ImmediateCpuDispatcher Dispatcher;
	PxFoundation* Foundation = nullptr;
	PxPhysics* Physics = nullptr;
	std::unique_ptr<PxCookingParams> CookingParams;
	PxScene* Scene = nullptr;
	PxMaterial* Material = nullptr;
	std::unordered_map<Mesh*, PxTriangleMesh*> MeshCache;
	std::vector<ActorEntry> Actors;
};

void Corona::CpuPhysicsStateDeleter::operator()(CpuPhysicsState* state) const
{
	delete state;
}

void Corona::InitCpuPhysics()
{
	if (CpuPhysics)
		return;

	std::unique_ptr<CpuPhysicsState, CpuPhysicsStateDeleter> state(new CpuPhysicsState());
	state->Foundation = PxCreateFoundation(PX_PHYSICS_VERSION, state->Allocator, state->ErrorCallback);
	if (!state->Foundation)
	{
		AppendCpuRuntimeTrace(L"[PhysX] PxCreateFoundation failed");
		return;
	}

	PxTolerancesScale scale;
	state->Physics = PxCreatePhysics(PX_PHYSICS_VERSION, *state->Foundation, scale, false, nullptr);
	if (!state->Physics)
	{
		AppendCpuRuntimeTrace(L"[PhysX] PxCreatePhysics failed");
		return;
	}

	PxCookingParams cookingParams(scale);
	cookingParams.meshPreprocessParams |= PxMeshPreprocessingFlag::eWELD_VERTICES;
	state->CookingParams = std::make_unique<PxCookingParams>(cookingParams);

	PxSceneDesc sceneDesc(scale);
	sceneDesc.gravity = PxVec3(0.0f, -9.81f, 0.0f);
	sceneDesc.cpuDispatcher = &state->Dispatcher;
	sceneDesc.filterShader = CoronaSimulationFilterShader;
	state->Scene = state->Physics->createScene(sceneDesc);
	if (!state->Scene)
	{
		AppendCpuRuntimeTrace(L"[PhysX] createScene failed");
		return;
	}

	state->Material = state->Physics->createMaterial(0.5f, 0.5f, 0.1f);
	if (!state->Material)
	{
		AppendCpuRuntimeTrace(L"[PhysX] createMaterial failed");
		return;
	}

	CpuPhysics = std::move(state);
	bCpuPhysicsSceneDirty = true;
	AppendCpuRuntimeTrace(L"[PhysX] initialized CPU scene query world");
}

void Corona::ShutdownCpuPhysics()
{
	CpuPhysics.reset();
	bCpuPhysicsSceneDirty = true;
}

void Corona::MarkCpuPhysicsSceneDirty()
{
	bCpuPhysicsSceneDirty = true;
}

void Corona::RebuildCpuPhysicsScene()
{
	if (!CpuPhysics)
		InitCpuPhysics();
	if (!CpuPhysics || !CpuPhysics->Scene || !CpuPhysics->Physics || !CpuPhysics->Material)
		return;

	CpuPhysics->ClearSceneActors();

	uint64_t actorCount = 0;
	uint64_t boxCount = 0;
	uint64_t triangleCount = 0;
	for (const SceneObject& object : SceneObjects)
	{
		if (!object.bVisible || !object.bPhysicsQuery || !object.ScenePtr)
			continue;

		if (object.PhysicsCollisionShape == EPhysicsCollisionShape::Box)
		{
			const DecomposedTransform decomposed = DecomposeTransform(object.Transform);
			if (!decomposed.bValid)
				continue;

			const PxVec3 halfExtents(
				decomposed.Scale.x * std::max(0.001f, object.PhysicsBoxHalfExtent.x),
				decomposed.Scale.y * std::max(0.001f, object.PhysicsBoxHalfExtent.y),
				decomposed.Scale.z * std::max(0.001f, object.PhysicsBoxHalfExtent.z));
			PxBoxGeometry geometry(halfExtents);
			if (!geometry.isValid())
				continue;

			PxRigidStatic* actor = CpuPhysics->Physics->createRigidStatic(decomposed.Pose);
			if (!actor)
				continue;

			PxShape* shape = CpuPhysics->Physics->createShape(geometry, *CpuPhysics->Material, true);
			if (!shape)
			{
				actor->release();
				continue;
			}

			actor->userData = reinterpret_cast<void*>(static_cast<uintptr_t>(object.Handle));
			shape->userData = reinterpret_cast<void*>(static_cast<uintptr_t>(object.Handle));
			actor->attachShape(*shape);
			shape->release();
			CpuPhysics->Scene->addActor(*actor);
			CpuPhysics->Actors.push_back({ actor, object.Handle });

			++actorCount;
			++boxCount;
			continue;
		}

		for (const shared_ptr<Mesh>& mesh : object.ScenePtr->meshes)
		{
			if (!mesh || mesh->CpuPositions.empty() || mesh->CpuIndices.empty())
				continue;

			PxTriangleMesh* triangleMesh = CpuPhysics->GetOrCreateTriangleMesh(mesh.get());
			if (!triangleMesh)
				continue;

			const glm::mat4x4 worldTransform = object.Transform * mesh->transform;
			const DecomposedTransform decomposed = DecomposeTransform(worldTransform);
			if (!decomposed.bValid)
				continue;

			PxTriangleMeshGeometry geometry(triangleMesh, PxMeshScale(decomposed.Scale));
			if (!geometry.isValid())
				continue;

			PxRigidStatic* actor = CpuPhysics->Physics->createRigidStatic(decomposed.Pose);
			if (!actor)
				continue;

			PxShape* shape = CpuPhysics->Physics->createShape(geometry, *CpuPhysics->Material, true);
			if (!shape)
			{
				actor->release();
				continue;
			}

			actor->userData = reinterpret_cast<void*>(static_cast<uintptr_t>(object.Handle));
			shape->userData = reinterpret_cast<void*>(static_cast<uintptr_t>(object.Handle));
			actor->attachShape(*shape);
			shape->release();
			CpuPhysics->Scene->addActor(*actor);
			CpuPhysics->Actors.push_back({ actor, object.Handle });

			++actorCount;
			triangleCount += mesh->CpuIndices.size() / 3;
		}
	}

	bCpuPhysicsSceneDirty = false;
	AppendCpuRuntimeTrace(
		L"[PhysX] rebuilt CPU scene query world actors=" + std::to_wstring(actorCount) +
		L", boxes=" + std::to_wstring(boxCount) +
		L", triangles=" + std::to_wstring(triangleCount));
}

bool Corona::CpuPhysicsRaycast(
	const glm::vec3& origin,
	const glm::vec3& direction,
	float maxDistance,
	CpuPhysicsRaycastHit& hit)
{
	hit = CpuPhysicsRaycastHit();
	if (maxDistance <= 0.0f)
		return false;

	if (!CpuPhysics)
		InitCpuPhysics();
	if (!CpuPhysics || !CpuPhysics->Scene)
		return false;
	if (bCpuPhysicsSceneDirty)
		RebuildCpuPhysicsScene();
	if (!CpuPhysics || !CpuPhysics->Scene)
		return false;

	glm::vec3 rayDirection = direction;
	const float directionLength = glm::length(rayDirection);
	if (directionLength < 1.0e-6f)
		return false;
	rayDirection /= directionLength;

	PxRaycastBuffer raycastHit;
	const PxHitFlags hitFlags = PxHitFlag::ePOSITION | PxHitFlag::eNORMAL;
	const bool bHit = CpuPhysics->Scene->raycast(
		ToPxVec3(origin),
		ToPxVec3(rayDirection),
		maxDistance,
		raycastHit,
		hitFlags);
	if (!bHit || !raycastHit.hasBlock)
		return false;

	const PxRaycastHit& block = raycastHit.block;
	hit.Distance = block.distance;
	hit.Position = FromPxVec3(block.position);
	hit.Normal = FromPxVec3(block.normal);
	if (block.actor)
		hit.ObjectHandle = static_cast<SceneObjectHandle>(reinterpret_cast<uintptr_t>(block.actor->userData));
	return true;
}

bool Corona::CpuPhysicsSphereSweep(
	const glm::vec3& origin,
	float radius,
	const glm::vec3& direction,
	float maxDistance,
	CpuPhysicsRaycastHit& hit)
{
	hit = CpuPhysicsRaycastHit();
	if (radius <= 0.0f || maxDistance <= 0.0f)
		return false;

	if (!CpuPhysics)
		InitCpuPhysics();
	if (!CpuPhysics || !CpuPhysics->Scene)
		return false;
	if (bCpuPhysicsSceneDirty)
		RebuildCpuPhysicsScene();
	if (!CpuPhysics || !CpuPhysics->Scene)
		return false;

	glm::vec3 sweepDirection = direction;
	const float directionLength = glm::length(sweepDirection);
	if (directionLength < 1.0e-6f)
		return false;
	sweepDirection /= directionLength;

	PxSphereGeometry sphereGeometry(radius);
	if (!sphereGeometry.isValid())
		return false;

	PxSweepBuffer sweepHit;
	const PxHitFlags hitFlags = PxHitFlag::ePOSITION | PxHitFlag::eNORMAL;
	const bool bHit = CpuPhysics->Scene->sweep(
		sphereGeometry,
		PxTransform(ToPxVec3(origin)),
		ToPxVec3(sweepDirection),
		maxDistance,
		sweepHit,
		hitFlags);
	if (!bHit || !sweepHit.hasBlock)
		return false;

	const PxSweepHit& block = sweepHit.block;
	hit.Distance = block.distance;
	hit.Position = FromPxVec3(block.position);
	hit.Normal = FromPxVec3(block.normal);
	if (block.actor)
		hit.ObjectHandle = static_cast<SceneObjectHandle>(reinterpret_cast<uintptr_t>(block.actor->userData));
	return true;
}

glm::vec3 Corona::ResolveCameraPhysicsMovement(
	const glm::vec3& startPosition,
	const glm::vec3& desiredPosition)
{
	glm::vec3 currentPosition = startPosition;
	glm::vec3 remainingMove = desiredPosition - startPosition;

	for (int iteration = 0; iteration < kCameraPhysicsSweepIterations; ++iteration)
	{
		const float moveDistance = glm::length(remainingMove);
		if (moveDistance < 1.0e-4f)
			break;

		const glm::vec3 moveDirection = remainingMove / moveDistance;
		CpuPhysicsRaycastHit hit;
		if (!CpuPhysicsSphereSweep(currentPosition, kCameraPhysicsRadius, moveDirection, moveDistance, hit))
		{
			currentPosition += remainingMove;
			break;
		}

		const float safeDistance = std::min(moveDistance, std::max(0.0f, hit.Distance - kCameraPhysicsSkin));
		currentPosition += moveDirection * safeDistance;

		glm::vec3 hitNormal = hit.Normal;
		const float hitNormalLength = glm::length(hitNormal);
		if (hitNormalLength < 1.0e-4f)
			break;
		hitNormal /= hitNormalLength;

		if (hit.Distance <= kCameraPhysicsSkin)
			currentPosition += hitNormal * (kCameraPhysicsSkin - hit.Distance);

		glm::vec3 slideMove = remainingMove - moveDirection * safeDistance;
		const float intoSurface = glm::dot(slideMove, hitNormal);
		if (intoSurface < 0.0f)
			slideMove -= hitNormal * intoSurface;

		remainingMove = slideMove;
	}

	return currentPosition;
}

bool Corona::CpuPhysicsRaycastForScript(
	const glm::vec3& origin,
	const glm::vec3& direction,
	float maxDistance,
	CpuPhysicsRaycastHit& hit)
{
	return CpuPhysicsRaycast(origin, direction, maxDistance, hit);
}
