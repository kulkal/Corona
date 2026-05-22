#include "stdafx.h"
#include "EntityComponentSystem.h"

#include <algorithm>

#include "glm/gtc/matrix_transform.hpp"

namespace CoronaECS
{
	TransformComponent TransformComponent::FromMatrix(const glm::mat4x4& matrix)
	{
		TransformComponent component;
		component.LocalToWorld = matrix;
		component.Position = glm::vec3(matrix[3]);
		return component;
	}

	TransformComponent TransformComponent::FromTRS(
		const glm::vec3& position,
		const glm::vec3& rotationDegrees,
		const glm::vec3& scale)
	{
		const glm::mat4x4 rotation =
			glm::rotate(glm::mat4x4(1.0f), glm::radians(rotationDegrees.z), glm::vec3(0.0f, 0.0f, 1.0f)) *
			glm::rotate(glm::mat4x4(1.0f), glm::radians(rotationDegrees.y), glm::vec3(0.0f, 1.0f, 0.0f)) *
			glm::rotate(glm::mat4x4(1.0f), glm::radians(rotationDegrees.x), glm::vec3(1.0f, 0.0f, 0.0f));

		TransformComponent component;
		component.LocalToWorld =
			glm::translate(glm::mat4x4(1.0f), position) *
			rotation *
			glm::scale(glm::mat4x4(1.0f), scale);
		component.Position = position;
		return component;
	}

	glm::vec3 TransformComponent::GetPosition() const
	{
		return Position;
	}

	void TransformComponent::SetPosition(const glm::vec3& position)
	{
		Position = position;
		LocalToWorld[3] = glm::vec4(position, LocalToWorld[3].w);
	}

	Entity EntityComponentSystem::CreateEntity(const std::string& name)
	{
		uint32_t id = 0;
		if (!FreeEntityIds.empty())
		{
			id = FreeEntityIds.back();
			FreeEntityIds.pop_back();
		}
		else
		{
			id = NextEntityId++;
			if (NextEntityId == 0)
				NextEntityId = 1;
		}

		uint32_t& generation = EntityGenerations[id];
		if (generation == 0)
			generation = 1;

		AliveEntityIds.insert(id);
		if (!name.empty())
			EntityNames[id] = name;
		else
			EntityNames.erase(id);

		return Entity(id, generation);
	}

	bool EntityComponentSystem::DestroyEntity(Entity entity)
	{
		if (!IsAlive(entity))
			return false;

		const uint32_t id = entity.GetId();
		AliveEntityIds.erase(id);
		EntityNames.erase(id);
		TransformComponents.erase(id);
		MeshComponents.erase(id);
		PhysicsComponents.erase(id);
		LightComponents.erase(id);
		CameraComponents.erase(id);
		ScriptComponents.erase(id);

		uint32_t& generation = EntityGenerations[id];
		++generation;
		if (generation == 0)
			generation = 1;

		if (ActiveCameraEntity == entity)
			ActiveCameraEntity = Entity();

		FreeEntityIds.push_back(id);
		return true;
	}

	bool EntityComponentSystem::IsAlive(Entity entity) const
	{
		if (!entity.IsValid())
			return false;

		const uint32_t id = entity.GetId();
		const auto generationIt = EntityGenerations.find(id);
		if (generationIt == EntityGenerations.end() || generationIt->second != entity.GetGeneration())
			return false;

		return AliveEntityIds.find(id) != AliveEntityIds.end();
	}

	Entity EntityComponentSystem::GetEntityById(uint32_t id) const
	{
		if (id == 0 || AliveEntityIds.find(id) == AliveEntityIds.end())
			return Entity();
		return MakeEntity(id);
	}

	void EntityComponentSystem::Clear()
	{
		NextEntityId = 1;
		FreeEntityIds.clear();
		EntityGenerations.clear();
		AliveEntityIds.clear();
		EntityNames.clear();
		TransformComponents.clear();
		MeshComponents.clear();
		PhysicsComponents.clear();
		LightComponents.clear();
		CameraComponents.clear();
		ScriptComponents.clear();
		ActiveCameraEntity = Entity();
	}

	void EntityComponentSystem::SetName(Entity entity, const std::string& name)
	{
		if (!IsAlive(entity))
			return;

		if (name.empty())
			EntityNames.erase(entity.GetId());
		else
			EntityNames[entity.GetId()] = name;
	}

	const std::string* EntityComponentSystem::GetName(Entity entity) const
	{
		if (!IsAlive(entity))
			return nullptr;

		const auto it = EntityNames.find(entity.GetId());
		return it != EntityNames.end() ? &it->second : nullptr;
	}

	TransformComponent* EntityComponentSystem::AddTransform(Entity entity, const TransformComponent& component)
	{
		if (!IsAlive(entity))
			return nullptr;

		TransformComponents[entity.GetId()] = component;
		return &TransformComponents[entity.GetId()];
	}

	MeshComponent* EntityComponentSystem::AddMesh(Entity entity, const MeshComponent& component)
	{
		if (!IsAlive(entity))
			return nullptr;

		MeshComponents[entity.GetId()] = component;
		return &MeshComponents[entity.GetId()];
	}

	PhysicsComponent* EntityComponentSystem::AddPhysics(Entity entity, const PhysicsComponent& component)
	{
		if (!IsAlive(entity))
			return nullptr;

		PhysicsComponents[entity.GetId()] = component;
		return &PhysicsComponents[entity.GetId()];
	}

	LightComponent* EntityComponentSystem::AddLight(Entity entity, const LightComponent& component)
	{
		if (!IsAlive(entity))
			return nullptr;

		LightComponents[entity.GetId()] = component;
		return &LightComponents[entity.GetId()];
	}

	CameraComponent* EntityComponentSystem::AddCamera(Entity entity, const CameraComponent& component)
	{
		if (!IsAlive(entity))
			return nullptr;

		CameraComponents[entity.GetId()] = component;
		if (component.bActive)
			SetActiveCamera(entity);

		return &CameraComponents[entity.GetId()];
	}

	ScriptComponent* EntityComponentSystem::AddScript(Entity entity, const ScriptComponent& component)
	{
		if (!IsAlive(entity))
			return nullptr;

		ScriptComponents[entity.GetId()] = component;
		return &ScriptComponents[entity.GetId()];
	}

	bool EntityComponentSystem::RemoveTransform(Entity entity)
	{
		if (!IsAlive(entity))
			return false;

		return TransformComponents.erase(entity.GetId()) > 0;
	}

	bool EntityComponentSystem::RemoveMesh(Entity entity)
	{
		if (!IsAlive(entity))
			return false;

		return MeshComponents.erase(entity.GetId()) > 0;
	}

	bool EntityComponentSystem::RemovePhysics(Entity entity)
	{
		if (!IsAlive(entity))
			return false;

		return PhysicsComponents.erase(entity.GetId()) > 0;
	}

	bool EntityComponentSystem::RemoveLight(Entity entity)
	{
		if (!IsAlive(entity))
			return false;

		return LightComponents.erase(entity.GetId()) > 0;
	}

	bool EntityComponentSystem::RemoveCamera(Entity entity)
	{
		if (!IsAlive(entity))
			return false;

		if (ActiveCameraEntity == entity)
			ActiveCameraEntity = Entity();

		return CameraComponents.erase(entity.GetId()) > 0;
	}

	bool EntityComponentSystem::RemoveScript(Entity entity)
	{
		if (!IsAlive(entity))
			return false;

		return ScriptComponents.erase(entity.GetId()) > 0;
	}

	TransformComponent* EntityComponentSystem::GetTransform(Entity entity)
	{
		if (!IsAlive(entity))
			return nullptr;

		const auto it = TransformComponents.find(entity.GetId());
		return it != TransformComponents.end() ? &it->second : nullptr;
	}

	const TransformComponent* EntityComponentSystem::GetTransform(Entity entity) const
	{
		if (!IsAlive(entity))
			return nullptr;

		const auto it = TransformComponents.find(entity.GetId());
		return it != TransformComponents.end() ? &it->second : nullptr;
	}

	MeshComponent* EntityComponentSystem::GetMesh(Entity entity)
	{
		if (!IsAlive(entity))
			return nullptr;

		const auto it = MeshComponents.find(entity.GetId());
		return it != MeshComponents.end() ? &it->second : nullptr;
	}

	const MeshComponent* EntityComponentSystem::GetMesh(Entity entity) const
	{
		if (!IsAlive(entity))
			return nullptr;

		const auto it = MeshComponents.find(entity.GetId());
		return it != MeshComponents.end() ? &it->second : nullptr;
	}

	PhysicsComponent* EntityComponentSystem::GetPhysics(Entity entity)
	{
		if (!IsAlive(entity))
			return nullptr;

		const auto it = PhysicsComponents.find(entity.GetId());
		return it != PhysicsComponents.end() ? &it->second : nullptr;
	}

	const PhysicsComponent* EntityComponentSystem::GetPhysics(Entity entity) const
	{
		if (!IsAlive(entity))
			return nullptr;

		const auto it = PhysicsComponents.find(entity.GetId());
		return it != PhysicsComponents.end() ? &it->second : nullptr;
	}

	LightComponent* EntityComponentSystem::GetLight(Entity entity)
	{
		if (!IsAlive(entity))
			return nullptr;

		const auto it = LightComponents.find(entity.GetId());
		return it != LightComponents.end() ? &it->second : nullptr;
	}

	const LightComponent* EntityComponentSystem::GetLight(Entity entity) const
	{
		if (!IsAlive(entity))
			return nullptr;

		const auto it = LightComponents.find(entity.GetId());
		return it != LightComponents.end() ? &it->second : nullptr;
	}

	CameraComponent* EntityComponentSystem::GetCamera(Entity entity)
	{
		if (!IsAlive(entity))
			return nullptr;

		const auto it = CameraComponents.find(entity.GetId());
		return it != CameraComponents.end() ? &it->second : nullptr;
	}

	ScriptComponent* EntityComponentSystem::GetScript(Entity entity)
	{
		if (!IsAlive(entity))
			return nullptr;

		const auto it = ScriptComponents.find(entity.GetId());
		return it != ScriptComponents.end() ? &it->second : nullptr;
	}

	const ScriptComponent* EntityComponentSystem::GetScript(Entity entity) const
	{
		if (!IsAlive(entity))
			return nullptr;

		const auto it = ScriptComponents.find(entity.GetId());
		return it != ScriptComponents.end() ? &it->second : nullptr;
	}

	const CameraComponent* EntityComponentSystem::GetCamera(Entity entity) const
	{
		if (!IsAlive(entity))
			return nullptr;

		const auto it = CameraComponents.find(entity.GetId());
		return it != CameraComponents.end() ? &it->second : nullptr;
	}

	bool EntityComponentSystem::HasTransform(Entity entity) const
	{
		return GetTransform(entity) != nullptr;
	}

	bool EntityComponentSystem::HasMesh(Entity entity) const
	{
		return GetMesh(entity) != nullptr;
	}

	bool EntityComponentSystem::HasPhysics(Entity entity) const
	{
		return GetPhysics(entity) != nullptr;
	}

	bool EntityComponentSystem::HasLight(Entity entity) const
	{
		return GetLight(entity) != nullptr;
	}

	bool EntityComponentSystem::HasCamera(Entity entity) const
	{
		return GetCamera(entity) != nullptr;
	}

	bool EntityComponentSystem::HasScript(Entity entity) const
	{
		return GetScript(entity) != nullptr;
	}

	bool EntityComponentSystem::SetActiveCamera(Entity entity)
	{
		if (!IsAlive(entity))
			return false;

		CameraComponent* camera = GetCamera(entity);
		if (!camera)
			return false;

		if (CameraComponent* activeCamera = GetActiveCamera())
			activeCamera->bActive = false;

		ActiveCameraEntity = entity;
		camera->bActive = true;
		return true;
	}

	Entity EntityComponentSystem::GetActiveCameraEntity() const
	{
		return IsAlive(ActiveCameraEntity) ? ActiveCameraEntity : Entity();
	}

	CameraComponent* EntityComponentSystem::GetActiveCamera()
	{
		return GetCamera(GetActiveCameraEntity());
	}

	const CameraComponent* EntityComponentSystem::GetActiveCamera() const
	{
		return GetCamera(GetActiveCameraEntity());
	}

	Entity EntityComponentSystem::FindEntityForRenderObject(uint32_t renderObjectHandle) const
	{
		if (renderObjectHandle == 0)
			return Entity();

		for (const auto& [id, mesh] : MeshComponents)
		{
			if (mesh.RenderObjectHandle == renderObjectHandle)
				return MakeEntity(id);
		}
		return Entity();
	}

	Entity EntityComponentSystem::FindEntityForRuntimeLight(uint32_t runtimeLightId) const
	{
		if (runtimeLightId == 0)
			return Entity();

		for (const auto& [id, light] : LightComponents)
		{
			if (light.RuntimeLightId == runtimeLightId)
				return MakeEntity(id);
		}
		return Entity();
	}

	std::vector<Entity> EntityComponentSystem::GetEntitiesWithMeshAndTransform() const
	{
		std::vector<Entity> entities;
		entities.reserve(MeshComponents.size());

		for (const auto& [id, mesh] : MeshComponents)
		{
			if (TransformComponents.find(id) != TransformComponents.end() &&
				AliveEntityIds.find(id) != AliveEntityIds.end())
			{
				entities.push_back(MakeEntity(id));
			}
		}
		return entities;
	}

	std::vector<Entity> EntityComponentSystem::GetEntitiesWithLight() const
	{
		std::vector<Entity> entities;
		entities.reserve(LightComponents.size());

		for (const auto& [id, light] : LightComponents)
		{
			if (AliveEntityIds.find(id) != AliveEntityIds.end())
				entities.push_back(MakeEntity(id));
		}
		return entities;
	}

	std::vector<Entity> EntityComponentSystem::GetEntitiesWithScript() const
	{
		std::vector<Entity> entities;
		entities.reserve(ScriptComponents.size());

		for (const auto& [id, script] : ScriptComponents)
		{
			if (AliveEntityIds.find(id) != AliveEntityIds.end())
				entities.push_back(MakeEntity(id));
		}
		return entities;
	}

	void EntityComponentSystem::ForEachMesh(const std::function<void(Entity, TransformComponent&, MeshComponent&)>& fn)
	{
		if (!fn)
			return;

		for (auto& [id, mesh] : MeshComponents)
		{
			auto transformIt = TransformComponents.find(id);
			if (transformIt == TransformComponents.end() || AliveEntityIds.find(id) == AliveEntityIds.end())
				continue;

			fn(MakeEntity(id), transformIt->second, mesh);
		}
	}

	void EntityComponentSystem::ForEachMesh(const std::function<void(Entity, const TransformComponent&, const MeshComponent&)>& fn) const
	{
		if (!fn)
			return;

		for (const auto& [id, mesh] : MeshComponents)
		{
			const auto transformIt = TransformComponents.find(id);
			if (transformIt == TransformComponents.end() || AliveEntityIds.find(id) == AliveEntityIds.end())
				continue;

			fn(MakeEntity(id), transformIt->second, mesh);
		}
	}

	Entity EntityComponentSystem::MakeEntity(uint32_t id) const
	{
		const auto generationIt = EntityGenerations.find(id);
		if (generationIt == EntityGenerations.end())
			return Entity();
		return Entity(id, generationIt->second);
	}
}
