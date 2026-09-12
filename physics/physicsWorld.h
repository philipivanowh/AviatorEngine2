#ifndef PHYSICS_WORLD_H
#define PHYSICS_WORLD_H

#include <cstddef>
#include <memory>
#include <string>

#include <SDL3/SDL.h>
#include <entt/entt.hpp>

#include "math/vec3.h"

// The engine's whole view of physics. Game code talks to IPhysicsWorld and to
// components; which engine actually simulates is decided once, at creation.
//
//   scene builder   authors bodies as components (scene/components.h):
//                     RigidbodyComponent  how a body moves - dynamic or kinematic,
//                                         mass, start velocity
//                     ColliderComponent   what it collides as - shape, size,
//                                         friction, bounce
//                     ShapeOfComponent    a collider that is one part of another
//                                         entity's body
//   IPhysicsWorld   reads those in Build(); every frame Update() writes
//                   TransformComponent, the only component physics writes
//   Renderer        draws TransformComponent, exactly as it did before physics
//
// In main():
//
//     std::unique_ptr<IPhysicsWorld> physics = CreatePhysicsWorld(); // Newton, or Null
//     BuildPhysicsScene(scene, registry);
//     physics->Build(registry);                                      // once, before the loop
//
//     // every frame
//     const PhysicsStepResult step = physics->Update(registry, dt);
//     if (step.moved) renderer.MarkSceneDirty();
//
// docs/newton_physics.md describes the Newton backend underneath.

enum class PhysicsBackend
{
    Newton, // NVIDIA Newton on the GPU, through an embedded Python interpreter
    Null,   // no simulation: everything stays where the scene builder put it
};

// Newton's rigid-body solvers. XPBD is the default - it stacks stably, is cheap
// per substep and handles every ColliderShape. The others are there to compare.
enum class PhysicsSolver
{
    XPBD,
    MuJoCo,
    Featherstone,
    SemiImplicit,
};

struct PhysicsSettings
{
    PhysicsBackend backend = PhysicsBackend::Newton;
    PhysicsSolver solver = PhysicsSolver::XPBD;

    // AviatorEngine is Y-up.
    Vec3<float> gravity = Vec3<float>(0.0f, -9.81f, 0.0f);

    // Simulated seconds per fixed step, and the solver substeps each step is
    // split into. More substeps keep tall stacks stiff, at the cost of GPU time.
    float fixedTimeStep = 1.0f / 60.0f;
    Uint32 substeps = 10;
    Uint32 solverIterations = 2;

    // Update never runs more fixed steps than this in one call. A slow frame
    // slows simulated time down instead of queueing ever more steps behind it,
    // which would make the next frame slower still.
    Uint32 maxStepsPerUpdate = 4;

    // Blend between the last two fixed steps when writing transforms, so motion
    // stays smooth when the frame rate is not a multiple of the physics rate.
    bool interpolate = true;

    // A pose that moved by less than both thresholds since it was last written
    // is left alone. A settled scene therefore stops reporting motion, which is
    // what lets the path tracer's accumulation converge again.
    float sleepDistance = 1e-4f; // meters
    float sleepAngle = 1e-3f;    // radians

    // Newton's device: the first CUDA GPU when true (the CPU if there is none),
    // always the CPU when false. A CUDA graph replays each fixed step as one
    // recorded unit instead of launching every kernel separately.
    bool useGpu = true;
    bool useCudaGraph = true;

    // The Python environment Newton is installed in. Empty means the one CMake
    // found at configure time (AVIATOR_NEWTON_VENV, default <repo>/.venv-newton).
    std::string pythonEnvironment;
};

struct PhysicsStepResult
{
    bool moved = false;        // at least one TransformComponent was written
    Uint32 steps = 0;          // fixed steps simulated by this call
    float milliseconds = 0.0f; // wall time spent inside the backend
};

class IPhysicsWorld
{
public:
    virtual ~IPhysicsWorld() = default;

    // (Re)creates the simulation from the registry:
    //
    //   RigidbodyComponent + TransformComponent      a body. Its colliders are its
    //                                                own ColliderComponent plus every
    //                                                ShapeOfComponent part naming it.
    //   ColliderComponent + TransformComponent,      static geometry
    //     without RigidbodyComponent or
    //     ShapeOfComponent
    //
    // Writes physicsBody_ID and collider_ID, and snapshots each physics entity's
    // TransformComponent into RestTransformComponent - the pose Reset returns to.
    // A body with no collider at all is skipped with a warning.
    //
    // The first Build compiles GPU kernels and can take a while, so call it
    // before the frame loop. Calling it again rebuilds from scratch, which is
    // how physics entities are added or removed.
    virtual bool Build(entt::registry &registry) = 0;

    // Advances simulated time by frameSeconds in fixed steps, then writes the
    // TransformComponent of every dynamic body and every ShapeOfComponent part.
    // Kinematic bodies go the other way: their TransformComponent is read, and
    // the solver moves the body there, pushing dynamic bodies out of its path.
    // Does nothing while paused.
    virtual PhysicsStepResult Update(entt::registry &registry, float frameSeconds) = 0;

    // Every body back to its RestTransformComponent, with the start velocities
    // from its RigidbodyComponent; queued forces are dropped. Transforms are
    // written immediately, and the next Update reports moved = true.
    virtual void Reset(entt::registry &registry) = 0;

    // World space, through the body's center of mass. Forces and torques are
    // consumed by the next fixed step, so a frame too fast to run one keeps them
    // queued rather than dropping them - call again every frame to keep pushing.
    // Calls on an entity that is not a built dynamic body are ignored.
    virtual void AddForce(entt::entity body, const Vec3<float> &force) = 0;
    virtual void AddTorque(entt::entity body, const Vec3<float> &torque) = 0;

    // An instant change of velocity: linear velocity += impulse / mass.
    virtual void AddImpulse(entt::entity body, const Vec3<float> &impulse) = 0;
    virtual void SetVelocity(entt::entity body, const Vec3<float> &linear, const Vec3<float> &angular) = 0;

    virtual void SetPaused(bool paused) = 0;
    virtual bool Paused() const = 0;

    virtual size_t BodyCount() const = 0;

    // For logs and the window title, e.g. "Newton 1.6.0 (XPBD, cuda:0)".
    virtual const char *Description() const = 0;
};

// Never returns null. If the requested backend cannot start - Newton not
// compiled in, its Python environment missing, a Python error during start-up -
// it logs why and returns the Null backend, so the app still runs, unsimulated.
std::unique_ptr<IPhysicsWorld> CreatePhysicsWorld(const PhysicsSettings &settings = PhysicsSettings());

#endif
