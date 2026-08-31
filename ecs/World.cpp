/*
 * World.cpp
 * ---------
 * ECS World + Jolt Physics System implementation.
 *
 * System execution order inside Update():
 *   1. UpdateCameraSystem  — translates InputState → camera position / orientation
 *   2. PhysicsSystem::Update — steps the simulation (gravity, collisions, constraints)
 *   3. SyncPhysicsToTransforms — copies Jolt body state → TransformComponent
 *
 * Camera modes (selected automatically):
 *   Physics-driven — entity has both CameraComponent and PhysicsComponent.
 *                    Horizontal velocity is set each frame from input; gravity
 *                    and collision response come from Jolt.
 *   Free-fly       — entity has CameraComponent but NO PhysicsComponent.
 *                    Position is updated directly by Camera::ProcessKeyboard().
 */

#include <entt/entt.hpp>
#include "ecs/World.h"
#include "ecs/Components.h"


#include <thread>
#include <iostream>
#include <string>

// ── Constructor ───────────────────────────────────────────────────────────────

World::World()
{

}

// ── Destructor ────────────────────────────────────────────────────────────────

World::~World()
{

}

entt::entity World::SpawnCamera(Point3 pos, Vec3<float> size){
    
    auto entity = CreateEntity("MainCamera");
    
    auto& camComp = reg.emplace<CameraComponent>(entity);
    camComp.cameraInstance = new Camera(pos, glm::vec3(0.0f, 1.0f, 0.0f), -90.0f, -15.0f);



}

// ── Entity lifecycle ──────────────────────────────────────────────────────────

entt::entity World::CreateEntity(const std::string& tag)
{
    auto entity = m_Registry.create();
    m_Registry.emplace<TagComponent>(entity, tag);
    m_Registry.emplace<TransformComponent>(entity);
    return entity;
}

void World::DestroyEntity(entt::entity entity)
{
    m_Registry.destroy(entity);
}

// ── Update ────────────────────────────────────────────────────────────────────

void World::Update(float deltaTime)
{

}

// ── Camera system ─────────────────────────────────────────────────────────────

void World::UpdateCameraSystem(float deltaTime, const InputState& input)
{
}

// ── Physics → Transform sync ──────────────────────────────────────────────────

void World::SyncPhysicsToTransforms()
{

}