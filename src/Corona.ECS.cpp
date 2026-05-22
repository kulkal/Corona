#include "stdafx.h"
#include "Corona.h"

#include <algorithm>

namespace
{
	glm::vec3 NormalizeOrFallback(const glm::vec3& value, const glm::vec3& fallback)
	{
		const float length = glm::length(value);
		return length > 0.0001f ? value / length : fallback;
	}
}

CoronaECS::Entity Corona::CreateEntity(const std::string& name)
{
	return EntityWorld.CreateEntity(name);
}

void Corona::InitializeWorldEntity()
{
	if (EntityWorld.IsAlive(WorldEntity))
		return;

	WorldEntity = EntityWorld.CreateEntity("World");
}

void Corona::InitializeLevelEntity()
{
	if (EntityWorld.IsAlive(LevelEntity))
		return;

	LevelEntity = EntityWorld.CreateEntity("Level");
}

CoronaECS::Entity Corona::GetSceneObjectEntity(SceneObjectHandle handle) const
{
	if (handle == InvalidSceneObjectHandle)
		return CoronaECS::Entity();

	const auto it = std::find_if(SceneObjects.begin(), SceneObjects.end(), [handle](const SceneObject& object)
	{
		return object.Handle == handle;
	});
	return it != SceneObjects.end() ? it->EntityHandle : CoronaECS::Entity();
}

Corona::SceneObjectHandle Corona::GetEntitySceneObject(CoronaECS::Entity entity) const
{
	if (!EntityWorld.IsAlive(entity))
		return InvalidSceneObjectHandle;

	if (const CoronaECS::MeshComponent* meshComponent = EntityWorld.GetMesh(entity))
	{
		if (meshComponent->RenderObjectHandle != InvalidSceneObjectHandle)
			return static_cast<SceneObjectHandle>(meshComponent->RenderObjectHandle);
	}

	const auto it = std::find_if(SceneObjects.begin(), SceneObjects.end(), [entity](const SceneObject& object)
	{
		return object.EntityHandle == entity;
	});
	return it != SceneObjects.end() ? it->Handle : InvalidSceneObjectHandle;
}

CoronaECS::EntityComponentSystem& Corona::GetEntityWorld()
{
	return EntityWorld;
}

const CoronaECS::EntityComponentSystem& Corona::GetEntityWorld() const
{
	return EntityWorld;
}

CoronaECS::Entity Corona::CreateSceneObjectEntity(const SceneObject& object)
{
	if (!object.ScenePtr || object.Handle == InvalidSceneObjectHandle)
		return CoronaECS::Entity();

	CoronaECS::Entity entity = EntityWorld.CreateEntity("SceneObject_" + std::to_string(object.Handle));

	EntityWorld.AddTransform(entity, CoronaECS::TransformComponent::FromMatrix(object.Transform));

	CoronaECS::MeshComponent meshComponent;
	meshComponent.ScenePtr = object.ScenePtr;
	meshComponent.Roughness = object.Roughness;
	meshComponent.Metallic = object.Metallic;
	meshComponent.bOverrideRoughnessMetallic = object.bOverrideRoughnessMetallic;
	meshComponent.bVisible = object.bVisible;
	meshComponent.bRayTracing = object.bRayTracing;
	meshComponent.RenderObjectHandle = object.Handle;
	EntityWorld.AddMesh(entity, meshComponent);

	CoronaECS::PhysicsComponent physicsComponent;
	physicsComponent.bQueryEnabled = object.bPhysicsQuery;
	physicsComponent.CollisionShape = static_cast<CoronaECS::PhysicsCollisionShape>(object.PhysicsCollisionShape);
	physicsComponent.BoxHalfExtent = object.PhysicsBoxHalfExtent;
	EntityWorld.AddPhysics(entity, physicsComponent);

	return entity;
}

void Corona::UpdateSceneObjectEntity(const SceneObject& object)
{
	if (!EntityWorld.IsAlive(object.EntityHandle))
		return;

	CoronaECS::TransformComponent* transformComponent = EntityWorld.GetTransform(object.EntityHandle);
	if (!transformComponent)
		transformComponent = EntityWorld.AddTransform(object.EntityHandle);
	if (transformComponent)
	{
		transformComponent->LocalToWorld = object.Transform;
		transformComponent->Position = glm::vec3(object.Transform[3]);
	}

	CoronaECS::MeshComponent meshComponent;
	meshComponent.ScenePtr = object.ScenePtr;
	meshComponent.Roughness = object.Roughness;
	meshComponent.Metallic = object.Metallic;
	meshComponent.bOverrideRoughnessMetallic = object.bOverrideRoughnessMetallic;
	meshComponent.bVisible = object.bVisible;
	meshComponent.bRayTracing = object.bRayTracing;
	meshComponent.RenderObjectHandle = object.Handle;
	EntityWorld.AddMesh(object.EntityHandle, meshComponent);

	CoronaECS::PhysicsComponent physicsComponent;
	physicsComponent.bQueryEnabled = object.bPhysicsQuery;
	physicsComponent.CollisionShape = static_cast<CoronaECS::PhysicsCollisionShape>(object.PhysicsCollisionShape);
	physicsComponent.BoxHalfExtent = object.PhysicsBoxHalfExtent;
	EntityWorld.AddPhysics(object.EntityHandle, physicsComponent);
}

void Corona::InitializeMainCameraEntity()
{
	if (EntityWorld.IsAlive(MainCameraEntity) && EntityWorld.HasCamera(MainCameraEntity))
		return;

	MainCameraEntity = EntityWorld.CreateEntity("MainCamera");
	EntityWorld.AddTransform(MainCameraEntity, CoronaECS::TransformComponent::FromTRS(m_camera.m_position));

	CoronaECS::CameraComponent cameraComponent;
	cameraComponent.LookDirection = NormalizeOrFallback(m_camera.m_lookDirection, glm::vec3(0.0f, 0.0f, 1.0f));
	cameraComponent.UpDirection = NormalizeOrFallback(m_camera.m_upDirection, glm::vec3(0.0f, 1.0f, 0.0f));
	cameraComponent.Fov = Fov;
	cameraComponent.NearPlane = Near;
	cameraComponent.FarPlane = Far;
	cameraComponent.bActive = true;
	EntityWorld.AddCamera(MainCameraEntity, cameraComponent);
	EntityWorld.SetActiveCamera(MainCameraEntity);
}

void Corona::UpdateMainCameraEntityFromSimpleCamera()
{
	InitializeMainCameraEntity();

	CoronaECS::TransformComponent* transformComponent = EntityWorld.GetTransform(MainCameraEntity);
	if (!transformComponent)
		transformComponent = EntityWorld.AddTransform(MainCameraEntity);
	if (transformComponent)
		transformComponent->SetPosition(m_camera.m_position);

	CoronaECS::CameraComponent* cameraComponent = EntityWorld.GetCamera(MainCameraEntity);
	if (!cameraComponent)
		cameraComponent = EntityWorld.AddCamera(MainCameraEntity);
	if (!cameraComponent)
		return;

	cameraComponent->LookDirection = NormalizeOrFallback(m_camera.m_lookDirection, glm::vec3(0.0f, 0.0f, 1.0f));
	cameraComponent->UpDirection = NormalizeOrFallback(m_camera.m_upDirection, glm::vec3(0.0f, 1.0f, 0.0f));
	cameraComponent->Fov = Fov;
	cameraComponent->NearPlane = Near;
	cameraComponent->FarPlane = Far;
	const CoronaECS::Entity activeCameraEntity = EntityWorld.GetActiveCameraEntity();
	if (!activeCameraEntity.IsValid() || activeCameraEntity == MainCameraEntity)
		EntityWorld.SetActiveCamera(MainCameraEntity);
}

void Corona::InitializeMainDirectionalLightEntity()
{
	if (EntityWorld.IsAlive(MainDirectionalLightEntity) && EntityWorld.HasLight(MainDirectionalLightEntity))
		return;

	MainDirectionalLightEntity = EntityWorld.CreateEntity("MainDirectionalLight");

	CoronaECS::LightComponent lightComponent;
	lightComponent.Type = CoronaECS::LightType::Directional;
	lightComponent.bEnabled = LightIntensity > 0.0f;
	lightComponent.Direction = NormalizeOrFallback(LightDir, glm::vec3(0.0f, 1.0f, 0.0f));
	lightComponent.Color = glm::vec3(1.0f);
	lightComponent.Intensity = LightIntensity;
	lightComponent.RuntimeLightId = 0;
	EntityWorld.AddLight(MainDirectionalLightEntity, lightComponent);
}

void Corona::UpdateMainDirectionalLightEntityFromState()
{
	InitializeMainDirectionalLightEntity();

	CoronaECS::LightComponent* lightComponent = EntityWorld.GetLight(MainDirectionalLightEntity);
	if (!lightComponent)
		return;

	lightComponent->Type = CoronaECS::LightType::Directional;
	lightComponent->bEnabled = LightIntensity > 0.0f;
	lightComponent->Direction = NormalizeOrFallback(LightDir, glm::vec3(0.0f, 1.0f, 0.0f));
	lightComponent->Color = glm::vec3(1.0f);
	lightComponent->Intensity = LightIntensity;
	lightComponent->RuntimeLightId = 0;
}

void Corona::ApplyDirectionalLightEntityToState()
{
	CoronaECS::LightComponent* lightComponent = EntityWorld.GetLight(MainDirectionalLightEntity);
	if (!lightComponent || lightComponent->Type != CoronaECS::LightType::Directional)
		return;

	LightDir = NormalizeOrFallback(lightComponent->Direction, glm::vec3(0.0f, 1.0f, 0.0f));
	LightIntensity = lightComponent->bEnabled ? std::max(0.0f, lightComponent->Intensity) : 0.0f;
}

CoronaECS::Entity Corona::CreatePointLightEntity(PointLightState& pointLight)
{
	if (pointLight.Id == 0)
		return CoronaECS::Entity();
	if (EntityWorld.IsAlive(pointLight.EntityHandle))
		return pointLight.EntityHandle;

	pointLight.EntityHandle = EntityWorld.CreateEntity("PointLight_" + std::to_string(pointLight.Id));
	UpdatePointLightEntity(pointLight);
	return pointLight.EntityHandle;
}

void Corona::UpdatePointLightEntity(const PointLightState& pointLight)
{
	if (pointLight.Id == 0 || !EntityWorld.IsAlive(pointLight.EntityHandle))
		return;

	CoronaECS::TransformComponent* transformComponent = EntityWorld.GetTransform(pointLight.EntityHandle);
	if (!transformComponent)
		transformComponent = EntityWorld.AddTransform(pointLight.EntityHandle);
	if (transformComponent)
		transformComponent->SetPosition(pointLight.Position);

	CoronaECS::LightComponent lightComponent;
	lightComponent.Type = CoronaECS::LightType::Point;
	lightComponent.bEnabled = pointLight.bEnabled;
	lightComponent.Color = glm::max(pointLight.Color, glm::vec3(0.0f));
	lightComponent.Intensity = std::max(0.0f, pointLight.Intensity);
	lightComponent.Radius = std::clamp(pointLight.Radius, 1.0f, 100000.0f);
	lightComponent.RuntimeLightId = pointLight.Id;
	EntityWorld.AddLight(pointLight.EntityHandle, lightComponent);
}

void Corona::DestroyPointLightEntity(PointLightState& pointLight)
{
	if (!EntityWorld.IsAlive(pointLight.EntityHandle))
		return;

	DestroyEntityScriptComponent(pointLight.EntityHandle);
	EntityWorld.DestroyEntity(pointLight.EntityHandle);
	pointLight.EntityHandle = CoronaECS::Entity();
}

Corona::PointLightState* Corona::FindPointLightByEntity(CoronaECS::Entity entity)
{
	if (!EntityWorld.IsAlive(entity))
		return nullptr;

	for (PointLightState& pointLight : PointLights)
	{
		if (pointLight.EntityHandle == entity)
			return &pointLight;
	}
	return nullptr;
}

const Corona::PointLightState* Corona::FindPointLightByEntity(CoronaECS::Entity entity) const
{
	if (!EntityWorld.IsAlive(entity))
		return nullptr;

	for (const PointLightState& pointLight : PointLights)
	{
		if (pointLight.EntityHandle == entity)
			return &pointLight;
	}
	return nullptr;
}
