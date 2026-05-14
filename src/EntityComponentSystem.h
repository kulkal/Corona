#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "glm/glm.hpp"
#include "glm/mat4x4.hpp"

class Scene;

namespace CoronaECS
{
	class Entity
	{
	public:
		constexpr Entity() = default;
		constexpr Entity(uint32_t id, uint32_t generation) : Id(id), Generation(generation) {}

		constexpr uint32_t GetId() const { return Id; }
		constexpr uint32_t GetGeneration() const { return Generation; }
		constexpr bool IsValid() const { return Id != 0; }

		constexpr bool operator==(const Entity& other) const
		{
			return Id == other.Id && Generation == other.Generation;
		}

		constexpr bool operator!=(const Entity& other) const
		{
			return !(*this == other);
		}

	private:
		uint32_t Id = 0;
		uint32_t Generation = 0;
	};

	struct TransformComponent
	{
		glm::mat4x4 LocalToWorld = glm::mat4x4(1.0f);

		static TransformComponent FromMatrix(const glm::mat4x4& matrix);
		static TransformComponent FromTRS(
			const glm::vec3& position,
			const glm::vec3& rotationDegrees = glm::vec3(0.0f),
			const glm::vec3& scale = glm::vec3(1.0f));

		glm::vec3 GetPosition() const;
		void SetPosition(const glm::vec3& position);
	};

	enum class PhysicsCollisionShape : uint8_t
	{
		TriangleMesh = 0,
		Box = 1,
	};

	struct MeshComponent
	{
		std::shared_ptr<Scene> ScenePtr;
		float Roughness = 1.0f;
		float Metallic = 0.0f;
		bool bOverrideRoughnessMetallic = false;
		bool bVisible = true;
		bool bRayTracing = true;
		uint32_t RenderObjectHandle = 0;
	};

	struct PhysicsComponent
	{
		bool bQueryEnabled = true;
		PhysicsCollisionShape CollisionShape = PhysicsCollisionShape::TriangleMesh;
		glm::vec3 BoxHalfExtent = glm::vec3(0.5f);
	};

	enum class LightType : uint8_t
	{
		Directional = 0,
		Point = 1,
	};

	struct LightComponent
	{
		LightType Type = LightType::Point;
		bool bEnabled = true;
		glm::vec3 Color = glm::vec3(1.0f);
		float Intensity = 1.0f;
		glm::vec3 Direction = glm::vec3(0.0f, 1.0f, 0.0f);
		float Radius = 320.0f;
		uint32_t RuntimeLightId = 0;
	};

	struct CameraComponent
	{
		glm::vec3 LookDirection = glm::vec3(0.0f, 0.0f, 1.0f);
		glm::vec3 UpDirection = glm::vec3(0.0f, 1.0f, 0.0f);
		float Fov = 0.8f;
		float NearPlane = 10.0f;
		float FarPlane = 20000.0f;
		bool bActive = false;
	};

	struct ScriptInstance
	{
		static constexpr int InvalidRef = -1;

		uint32_t InstanceId = 0;
		int UpdateRef = InvalidRef;
		int ShutdownRef = InvalidRef;
		int ImGuiRef = InvalidRef;
		int UiRef = InvalidRef;
		bool bEnabled = true;
		bool bPassEntityToCallbacks = true;
		std::wstring SourceName;
		std::string NativeScriptName;
	};

	struct ScriptComponent
	{
		static constexpr int InvalidRef = ScriptInstance::InvalidRef;

		std::vector<ScriptInstance> Instances;
		uint32_t NextInstanceId = 1;
		bool bEnabled = true;
	};

	class EntityComponentSystem
	{
	public:
		Entity CreateEntity(const std::string& name = std::string());
		bool DestroyEntity(Entity entity);
		bool IsAlive(Entity entity) const;
		Entity GetEntityById(uint32_t id) const;
		void Clear();

		void SetName(Entity entity, const std::string& name);
		const std::string* GetName(Entity entity) const;

		TransformComponent* AddTransform(Entity entity, const TransformComponent& component = TransformComponent());
		MeshComponent* AddMesh(Entity entity, const MeshComponent& component);
		PhysicsComponent* AddPhysics(Entity entity, const PhysicsComponent& component = PhysicsComponent());
		LightComponent* AddLight(Entity entity, const LightComponent& component = LightComponent());
		CameraComponent* AddCamera(Entity entity, const CameraComponent& component = CameraComponent());
		ScriptComponent* AddScript(Entity entity, const ScriptComponent& component = ScriptComponent());

		bool RemoveTransform(Entity entity);
		bool RemoveMesh(Entity entity);
		bool RemovePhysics(Entity entity);
		bool RemoveLight(Entity entity);
		bool RemoveCamera(Entity entity);
		bool RemoveScript(Entity entity);

		TransformComponent* GetTransform(Entity entity);
		const TransformComponent* GetTransform(Entity entity) const;
		MeshComponent* GetMesh(Entity entity);
		const MeshComponent* GetMesh(Entity entity) const;
		PhysicsComponent* GetPhysics(Entity entity);
		const PhysicsComponent* GetPhysics(Entity entity) const;
		LightComponent* GetLight(Entity entity);
		const LightComponent* GetLight(Entity entity) const;
		CameraComponent* GetCamera(Entity entity);
		const CameraComponent* GetCamera(Entity entity) const;
		ScriptComponent* GetScript(Entity entity);
		const ScriptComponent* GetScript(Entity entity) const;

		bool HasTransform(Entity entity) const;
		bool HasMesh(Entity entity) const;
		bool HasPhysics(Entity entity) const;
		bool HasLight(Entity entity) const;
		bool HasCamera(Entity entity) const;
		bool HasScript(Entity entity) const;

		bool SetActiveCamera(Entity entity);
		Entity GetActiveCameraEntity() const;
		CameraComponent* GetActiveCamera();
		const CameraComponent* GetActiveCamera() const;

		Entity FindEntityForRenderObject(uint32_t renderObjectHandle) const;
		Entity FindEntityForRuntimeLight(uint32_t runtimeLightId) const;
		std::vector<Entity> GetEntitiesWithMeshAndTransform() const;
		std::vector<Entity> GetEntitiesWithLight() const;
		std::vector<Entity> GetEntitiesWithScript() const;

		void ForEachMesh(const std::function<void(Entity, TransformComponent&, MeshComponent&)>& fn);
		void ForEachMesh(const std::function<void(Entity, const TransformComponent&, const MeshComponent&)>& fn) const;

		size_t GetAliveEntityCount() const { return AliveEntityIds.size(); }
		size_t GetTransformComponentCount() const { return TransformComponents.size(); }
		size_t GetMeshComponentCount() const { return MeshComponents.size(); }
		size_t GetPhysicsComponentCount() const { return PhysicsComponents.size(); }
		size_t GetLightComponentCount() const { return LightComponents.size(); }
		size_t GetCameraComponentCount() const { return CameraComponents.size(); }
		size_t GetScriptComponentCount() const { return ScriptComponents.size(); }

	private:
		Entity MakeEntity(uint32_t id) const;

		uint32_t NextEntityId = 1;
		std::vector<uint32_t> FreeEntityIds;
		std::unordered_map<uint32_t, uint32_t> EntityGenerations;
		std::unordered_set<uint32_t> AliveEntityIds;
		std::unordered_map<uint32_t, std::string> EntityNames;
		std::unordered_map<uint32_t, TransformComponent> TransformComponents;
		std::unordered_map<uint32_t, MeshComponent> MeshComponents;
		std::unordered_map<uint32_t, PhysicsComponent> PhysicsComponents;
		std::unordered_map<uint32_t, LightComponent> LightComponents;
		std::unordered_map<uint32_t, CameraComponent> CameraComponents;
		std::unordered_map<uint32_t, ScriptComponent> ScriptComponents;
		Entity ActiveCameraEntity;
	};
}
