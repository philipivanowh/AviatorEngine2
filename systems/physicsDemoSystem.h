#ifndef PHYSICS_DEMO_SYSTEM_H
#define PHYSICS_DEMO_SYSTEM_H

#include <cmath>

#include <SDL3/SDL.h>
#include <entt/entt.hpp>

#include "math/quat.hpp"
#include "math/vec3.h"
#include "physics/physicsWorld.h"
#include "scene/components.h"

// The demo's own physics behaviour: what drives its kinematic bodies, and what
// the physics debug keys do. None of this is engine machinery - it is the
// "gameplay" half of BuildPhysicsScene, kept out of main.cpp so the frame loop
// stays a list of ordered steps.

// How fast the sweeper arm turns, in radians per second. The arm is 7 m across,
// so its tips move at about 6 m/s: fast enough to throw the spheres, slow enough
// that a 60 Hz step still resolves the contact instead of tunnelling through.
inline constexpr float kSweeperSpeed = 1.6f;

// Kinematic bodies are driven by WRITING TransformComponent - physics reads it
// as this step's target rather than writing it (see physics/physicsWorld.h), so
// this has to run before IPhysicsWorld::Update.
inline void AnimateKinematicBodies(entt::registry &registry, float seconds)
{
    auto view = registry.view<TransformComponent, RigidbodyComponent, TagComponent>();
    for (entt::entity e : view)
    {
        if (view.get<RigidbodyComponent>(e).type != RigidbodyType::Kinematic)
        {
            continue;
        }
        if (view.get<TagComponent>(e).tag != "Sweeper")
        {
            continue;
        }

        // Spin about world Y, in place: the arm was authored centred on its
        // pivot, so its own rotation is the whole animation.
        view.get<TransformComponent>(e).rotation = QuatY(seconds * kSweeperSpeed);
    }
}

// An upward and outward impulse on every dynamic body within `radius` of
// `center` - the "what happens if" key. Returns how many bodies it hit.
inline int BlastDynamicBodies(entt::registry &registry, IPhysicsWorld &physics,
                              const Point3 &center, float radius, float strength)
{
    int hit = 0;

    auto view = registry.view<TransformComponent, RigidbodyComponent>();
    for (entt::entity e : view)
    {
        const RigidbodyComponent &body = view.get<RigidbodyComponent>(e);
        if (body.type != RigidbodyType::Dynamic)
        {
            continue;
        }

        const Vec3<float> offset = view.get<TransformComponent>(e).position - center;
        const float distance = offset.length();
        if (distance > radius)
        {
            continue;
        }

        // Linear falloff, and always some lift: a purely outward impulse would
        // just scrape everything along the floor and look like nothing happened.
        const float falloff = 1.0f - distance / radius;
        const Vec3<float> direction =
            normalize(Vec3<float>(offset.x, 0.0f, offset.z) + Vec3<float>(0.0f, 2.5f, 0.0f));

        // Scaled by mass so light spheres and heavy boxes leave together, which
        // is the point of an impulse rather than a velocity.
        physics.AddImpulse(e, direction * (strength * falloff * body.mass));
        hit++;
    }

    return hit;
}

// Sends the cannonball at the pyramid again, from wherever it came to rest.
// IPhysicsWorld has no teleport on purpose - Reset is what puts the whole scene
// back - so this sets a velocity instead of respawning anything.
inline bool KickCannonball(entt::registry &registry, IPhysicsWorld &physics, float speed)
{
    auto view = registry.view<TransformComponent, RigidbodyComponent, TagComponent>();
    for (entt::entity e : view)
    {
        if (view.get<TagComponent>(e).tag != "Cannonball")
        {
            continue;
        }

        const Point3 target(0.0f, 1.0f, -4.0f); // the pyramid
        const Vec3<float> toTarget = target - view.get<TransformComponent>(e).position;
        const Vec3<float> flat(toTarget.x, 0.0f, toTarget.z);
        if (flat.near_zero(0.01f))
        {
            return false; // already there; nothing sensible to aim at
        }

        // A little loft, proportional to the distance, so it arcs in rather than
        // ploughing along the ground.
        const Vec3<float> direction =
            normalize(flat + Vec3<float>(0.0f, 0.35f * flat.length(), 0.0f));

        physics.SetVelocity(e, direction * speed, Vec3<float>(0.0f, 0.0f, 0.0f));
        return true;
    }

    return false;
}

#endif
