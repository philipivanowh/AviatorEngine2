 #pragma once
/*
 * World.h
 * -------
 * The engine's ECS World + Box3d Physics integration hub.
 *
 * Responsibilities:
 *   – Entity/component lifecycle  (CreateEntity / DestroyEntity)
 *   – Per-frame simulation        (Update → camera system → physics step → sync)
 *   – Physics-to-transform sync   (SyncPhysicsToTransforms)
 */

#include <memory>
#include <string>

#include <entt/entt.hpp>

#include "ecs/Components.h"

class World
{
public:
    World();
    ~World();

    // ── Frame update ──────────────────────────────────────────────────────────
    /// Runs the camera system, advances the physics simulation by deltaTime,
    /// then syncs all dynamic bodies back into TransformComponents.
    void Update(float deltaTime);
    
    entt::entity SpawnCamera(Point3 pos, Vec3<float> size);

    // ── Entity lifecycle ──────────────────────────────────────────────────────
    /// Creates an entity with TagComponent + TransformComponent and returns its handle.
    entt::entity CreateEntity(const std::string& tag = "Entity");

    /// Removes the entity's physics body (if any) then destroys it from the registry.
    void DestroyEntity(entt::entity entity);

    std::vector<std::unique_ptr<Object>> GetObjects(){

    }

    // ── Accessors ─────────────────────────────────────────────────────────────
    entt::registry&     GetRegistry()      { return m_Registry; }

    TextureLibrary textures;
    Camera camera;

private:
    void UpdateCameraSystem(float deltaTime, const InputState& input);
    void SyncPhysicsToTransforms();

    // ── ECS registry ─────────────────────────────────────────────────────────
    entt::registry m_Registry;
    
};