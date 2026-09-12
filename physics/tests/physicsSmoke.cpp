#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <entt/entt.hpp>

#include "math/quat.hpp"
#include "math/vec3.h"
#include "physics/physicsWorld.h"
#include "scene/components.h"

// A console check of the Newton backend with no window, no renderer and no
// scene helpers: the registry is filled from components directly, so this test
// only fails when physics is wrong.
//
// What it covers, in the order it runs:
//
//   a ground plane, a four-box stack, a rolling sphere, a kinematic box swept
//   along X, and a two-part compound body     - Build, and the skip-with-warning
//                                               paths (a body with no collider,
//                                               a part with no body)
//   240 frames at 1/60                        - the one-step-per-frame case,
//                                               AddImpulse, SetVelocity
//   100 frames at 1/100                       - frames that run no fixed step
//                                               but still write transforms
//   one frame of dt = 1.0                     - maxStepsPerUpdate clamping and
//                                               the dropped time after it
//   ten paused frames                         - Update does nothing
//   Reset                                     - rest poses, and moved = true
//   180 settling frames                       - a settled scene stops reporting
//                                               motion, which is what the path
//                                               tracer needs to converge
//
// Exit code 0 means every check passed. Anything else means read the FAIL lines.

namespace
{

    int failures = 0;
    int checks = 0;

    void Check(bool passed, SDL_PRINTF_FORMAT_STRING const char *format, ...)
    {
        char message[1024];
        va_list arguments;
        va_start(arguments, format);
        SDL_vsnprintf(message, sizeof(message), format, arguments);
        va_end(arguments);

        ++checks;
        if (passed)
        {
            SDL_Log("  ok   %s", message);
        }
        else
        {
            ++failures;
            SDL_Log("  FAIL %s", message);
        }
    }

    // --- scene building ----------------------------------------------------

    entt::entity MakeStatic(entt::registry &registry, const Point3 &position, const ColliderComponent &collider)
    {
        const entt::entity entity = registry.create();
        registry.emplace<TransformComponent>(entity, TransformComponent{position, Quat<float>()});
        registry.emplace<ColliderComponent>(entity, collider);
        return entity;
    }

    entt::entity MakeBody(entt::registry &registry, const Point3 &position, const RigidbodyComponent &body,
                          const ColliderComponent *collider)
    {
        const entt::entity entity = registry.create();
        registry.emplace<TransformComponent>(entity, TransformComponent{position, Quat<float>()});
        registry.emplace<RigidbodyComponent>(entity, body);
        if (collider != nullptr)
        {
            registry.emplace<ColliderComponent>(entity, *collider);
        }
        return entity;
    }

    entt::entity MakePart(entt::registry &registry, entt::entity body, const Point3 &bodyPosition,
                          const Point3 &localPosition, const ColliderComponent &collider)
    {
        const entt::entity entity = registry.create();
        // The authored transform is the composed world pose, which is what a
        // scene builder would have written; physics overwrites it every frame.
        registry.emplace<TransformComponent>(entity, TransformComponent{bodyPosition + localPosition, Quat<float>()});
        registry.emplace<ColliderComponent>(entity, collider);
        registry.emplace<ShapeOfComponent>(entity, ShapeOfComponent{body, localPosition, Quat<float>()});
        return entity;
    }

    ColliderComponent BoxCollider(const Vec3<float> &halfExtent, float friction = 0.6f)
    {
        ColliderComponent collider;
        collider.shape = ColliderShape::Box;
        collider.halfExtent = halfExtent;
        collider.friction = friction;
        return collider;
    }

    // --- small helpers -----------------------------------------------------

    bool Finite(const Point3 &v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

    bool Finite(const Quat<float> &q)
    {
        return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
    }

    // Every transform in the registry, every frame: a NaN that appears once and
    // is then overwritten still means the solver went unstable.
    bool AnyNaN(entt::registry &registry)
    {
        for (auto [entity, transform] : registry.view<TransformComponent>().each())
        {
            if (!Finite(transform.position) || !Finite(transform.rotation))
            {
                return true;
            }
        }
        return false;
    }

    const TransformComponent &Transform(entt::registry &registry, entt::entity entity)
    {
        return registry.get<TransformComponent>(entity);
    }

    float Distance(const Point3 &a, const Point3 &b) { return (a - b).length(); }

    std::string PoseText(const TransformComponent &transform)
    {
        char text[256];
        SDL_snprintf(text, sizeof(text), "p(%7.4f %7.4f %7.4f) q(%6.3f %6.3f %6.3f %6.3f)",
                     transform.position.x, transform.position.y, transform.position.z, transform.rotation.x,
                     transform.rotation.y, transform.rotation.z, transform.rotation.w);
        return text;
    }

    struct Timing
    {
        float total = 0.0f;
        float worst = 0.0f;
        float best = 1.0e9f;
        int frames = 0;

        void Add(float milliseconds)
        {
            total += milliseconds;
            worst = SDL_max(worst, milliseconds);
            best = SDL_min(best, milliseconds);
            ++frames;
        }

        void Report(const char *what) const
        {
            SDL_Log("%s: %d frames, %.3f ms mean, %.3f ms min, %.3f ms max", what, frames,
                    frames > 0 ? total / static_cast<float>(frames) : 0.0f, frames > 0 ? best : 0.0f, worst);
        }
    };

    // The scene, with the handles the checks need.
    struct SmokeScene
    {
        entt::entity ground = entt::null;
        entt::entity stack[4] = {entt::null, entt::null, entt::null, entt::null};
        entt::entity sphere = entt::null;
        entt::entity kinematic = entt::null;
        entt::entity compound = entt::null;
        entt::entity compoundParts[2] = {entt::null, entt::null};

        // Deliberately malformed, to prove Build skips them with a warning
        // instead of failing or crashing.
        entt::entity bodyWithoutCollider = entt::null;
        entt::entity partWithoutBody = entt::null;

        static constexpr float kSphereRadius = 0.3f;
        static constexpr float kStackHalf = 0.25f;
        static constexpr float kPartOffset = 0.35f;
        Point3 stackStart[4];
        Point3 compoundStart = Point3(-2.5f, 1.0f, 0.0f);
        Point3 kinematicStart = Point3(2.0f, 0.31f, -1.5f);
    };

    SmokeScene BuildScene(entt::registry &registry)
    {
        SmokeScene scene;

        // Ground: an infinite plane, normal +Y, at the origin.
        ColliderComponent plane;
        plane.shape = ColliderShape::Plane;
        plane.friction = 0.8f;
        scene.ground = MakeStatic(registry, Point3(0.0f, 0.0f, 0.0f), plane);

        // A stack of four boxes, with 2 mm of air between them so nothing starts
        // interpenetrating.
        const float side = 2.0f * SmokeScene::kStackHalf;
        for (int level = 0; level < 4; ++level)
        {
            RigidbodyComponent body;
            body.type = RigidbodyType::Dynamic;
            body.mass = 1.0f;
            const ColliderComponent collider = BoxCollider(Vec3<float>(SmokeScene::kStackHalf), 0.8f);
            scene.stackStart[level] =
                Point3(0.0f, SmokeScene::kStackHalf + static_cast<float>(level) * (side + 0.002f), 0.0f);
            scene.stack[level] = MakeBody(registry, scene.stackStart[level], body, &collider);
        }

        // A sphere rolling along -X, on its own lane in Z so it cannot disturb
        // anything else in the scene.
        {
            RigidbodyComponent body;
            body.mass = 2.0f;
            body.linearVelocity = Vec3<float>(-1.2f, 0.0f, 0.0f);
            ColliderComponent collider;
            collider.shape = ColliderShape::Sphere;
            collider.radius = SmokeScene::kSphereRadius;
            collider.friction = 0.7f;
            scene.sphere = MakeBody(registry, Point3(2.5f, SmokeScene::kSphereRadius, 1.5f), body, &collider);
        }

        // A kinematic box, swept along X by the test every frame.
        {
            RigidbodyComponent body;
            body.type = RigidbodyType::Kinematic;
            const ColliderComponent collider = BoxCollider(Vec3<float>(0.3f));
            scene.kinematic = MakeBody(registry, scene.kinematicStart, body, &collider);
        }

        // A compound body: a pivot with no collider of its own and two box
        // parts hanging off it.
        {
            RigidbodyComponent body;
            body.mass = 2.0f;
            scene.compound = MakeBody(registry, scene.compoundStart, body, nullptr);
            const ColliderComponent part = BoxCollider(Vec3<float>(0.2f), 0.8f);
            scene.compoundParts[0] = MakePart(registry, scene.compound, scene.compoundStart,
                                             Point3(-SmokeScene::kPartOffset, 0.0f, 0.0f), part);
            scene.compoundParts[1] = MakePart(registry, scene.compound, scene.compoundStart,
                                              Point3(SmokeScene::kPartOffset, 0.0f, 0.0f), part);
        }

        // The two malformed entities.
        {
            RigidbodyComponent body;
            body.mass = 1.0f;
            scene.bodyWithoutCollider = MakeBody(registry, Point3(0.0f, 5.0f, 5.0f), body, nullptr);

            scene.partWithoutBody = registry.create();
            registry.emplace<TransformComponent>(scene.partWithoutBody,
                                                 TransformComponent{Point3(5.0f, 5.0f, 5.0f), Quat<float>()});
            registry.emplace<ColliderComponent>(scene.partWithoutBody, BoxCollider(Vec3<float>(0.1f)));
            registry.emplace<ShapeOfComponent>(scene.partWithoutBody,
                                               ShapeOfComponent{entt::null, Point3(0.0f, 0.0f, 0.0f), Quat<float>()});
        }

        return scene;
    }

    // --- checks ------------------------------------------------------------

    // A part is rigidly attached: its transform must be the body's pose composed
    // with the local pose it was authored with, to the float.
    void CheckPartsFollowBody(entt::registry &registry, const SmokeScene &scene, const char *when)
    {
        const TransformComponent &body = Transform(registry, scene.compound);
        for (int index = 0; index < 2; ++index)
        {
            const ShapeOfComponent &shapeOf = registry.get<ShapeOfComponent>(scene.compoundParts[index]);
            const TransformComponent &part = Transform(registry, scene.compoundParts[index]);
            const Point3 expected = body.position + rotate(normalize(body.rotation), shapeOf.localPosition);
            const float error = Distance(part.position, expected);
            Check(error < 1.0e-4f, "%s: compound part %d follows its body (%.6f m from where the body puts it)", when,
                  index, error);
        }
    }

    void CheckStackIntact(entt::registry &registry, const SmokeScene &scene, const char *when)
    {
        bool ordered = true;
        bool upright = true;
        float worstDrift = 0.0f;
        for (int level = 0; level < 4; ++level)
        {
            const TransformComponent &box = Transform(registry, scene.stack[level]);
            const float expectedY = SmokeScene::kStackHalf + static_cast<float>(level) * 2.0f * SmokeScene::kStackHalf;
            worstDrift = SDL_max(worstDrift, std::fabs(box.position.x));
            worstDrift = SDL_max(worstDrift, std::fabs(box.position.z));
            upright = upright && std::fabs(box.position.y - expectedY) < 0.06f;
            if (level > 0)
            {
                ordered = ordered && box.position.y > Transform(registry, scene.stack[level - 1]).position.y;
            }
        }
        Check(upright && ordered, "%s: the stack is still a stack (bottom to top, each box near its resting height)",
              when);
        Check(worstDrift < 0.08f, "%s: no box slid out of the stack (worst lateral drift %.4f m)", when, worstDrift);
    }

    void CheckSphereAtRest(entt::registry &registry, const SmokeScene &scene, const Point3 &earlier, const char *when)
    {
        const TransformComponent &sphere = Transform(registry, scene.sphere);
        const float height = sphere.position.y;
        Check(std::fabs(height - SmokeScene::kSphereRadius) < 0.02f,
              "%s: the sphere is resting on the ground (y = %.4f, radius %.2f)", when, height,
              SmokeScene::kSphereRadius);
        const float travelled = Distance(sphere.position, earlier);
        Check(travelled < 1.0e-3f, "%s: the sphere has come to rest (%.6f m over the last 10 frames)", when, travelled);
    }

} // namespace

int main(int, char **)
{
    // Only the timer is needed; SDL_Log works either way, but initialising
    // keeps SDL_GetPerformanceCounter honest on every platform.
    SDL_Init(0);

    entt::registry registry;
    const SmokeScene scene = BuildScene(registry);

    PhysicsSettings settings;
    settings.fixedTimeStep = 1.0f / 60.0f;
    settings.maxStepsPerUpdate = 4;
    settings.interpolate = true;
    SDL_Log("physics_smoke: creating the physics world");
    std::unique_ptr<IPhysicsWorld> physics = CreatePhysicsWorld(settings);

    SDL_Log("physics_smoke: backend is \"%s\"", physics->Description());
    const bool isNewton = SDL_strncmp(physics->Description(), "Newton", 6) == 0;
    Check(isNewton, "the Newton backend started (not the Null fallback)");
    if (!isNewton)
    {
        SDL_Log("physics_smoke: nothing else can be checked without Newton - %d/%d checks passed", checks - failures,
                checks);
        return 1;
    }

    const bool built = physics->Build(registry);
    Check(built, "Build succeeded");
    if (!built)
    {
        return 1;
    }

    // --- what Build was supposed to write ---------------------------------
    Check(physics->BodyCount() == 7, "BodyCount is 7 (4 stack + sphere + kinematic + compound pivot), got %zu",
          physics->BodyCount());
    Check(registry.get<RigidbodyComponent>(scene.stack[0]).physicsBody_ID >= 0,
          "Build wrote physicsBody_ID (stack bottom: %d)",
          registry.get<RigidbodyComponent>(scene.stack[0]).physicsBody_ID);
    Check(registry.get<ColliderComponent>(scene.ground).collider_ID >= 0, "Build wrote collider_ID (ground: %d)",
          registry.get<ColliderComponent>(scene.ground).collider_ID);
    Check(registry.get<ColliderComponent>(scene.compoundParts[1]).collider_ID >= 0,
          "Build wrote collider_ID on a compound part (%d)",
          registry.get<ColliderComponent>(scene.compoundParts[1]).collider_ID);
    Check(registry.all_of<RestTransformComponent>(scene.stack[3]),
          "Build snapshotted RestTransformComponent on a body");
    Check(registry.all_of<RestTransformComponent>(scene.compoundParts[0]),
          "Build snapshotted RestTransformComponent on a part");
    Check(registry.get<RigidbodyComponent>(scene.bodyWithoutCollider).physicsBody_ID < 0,
          "a body with no collider was skipped, not added");
    Check(registry.get<ColliderComponent>(scene.partWithoutBody).collider_ID < 0,
          "a part whose ShapeOfComponent names no body was skipped");

    // --- 240 frames at 1/60 -----------------------------------------------
    const float dt = 1.0f / 60.0f;
    Timing mainPhase;
    Point3 spherePositionAt230;
    bool sawNaN = false;
    int movedFrames = 0;
    for (int frame = 1; frame <= 240; ++frame)
    {
        // The kinematic body is driven through its TransformComponent, which is
        // the one component physics reads rather than writes.
        registry.get<TransformComponent>(scene.kinematic).position =
            scene.kinematicStart + Point3(0.005f * static_cast<float>(frame), 0.0f, 0.0f);

        if (frame == 150)
        {
            // A sideways kick on the compound body, after it has landed.
            physics->AddImpulse(scene.compound, Vec3<float>(1.5f, 0.0f, 0.0f));
        }
        if (frame == 180)
        {
            // Stop the sphere dead, so "did it come to rest" is a question
            // about the contact and not about rolling for ever (nothing models
            // rolling resistance, so a rolling sphere never stops).
            physics->SetVelocity(scene.sphere, Vec3<float>(0.0f), Vec3<float>(0.0f));
        }
        if (frame == 230)
        {
            spherePositionAt230 = Transform(registry, scene.sphere).position;
        }

        const PhysicsStepResult step = physics->Update(registry, dt);
        mainPhase.Add(step.milliseconds);
        movedFrames += step.moved ? 1 : 0;
        sawNaN = sawNaN || AnyNaN(registry);
        if (frame == 1)
        {
            Check(step.steps == 1, "a frame of exactly one fixed step runs one step (got %u)", step.steps);
            // Not step.moved: with interpolation on, a frame that lands on a
            // step boundary renders the pose from BEFORE that step, so the very
            // first frame legitimately shows nothing yet. The movedFrames total
            // below is what proves the scene is moving.
        }
    }
    Check(!sawNaN, "no NaN in any transform during 240 frames");
    Check(movedFrames > 150, "the scene was moving for most of the run (%d/240 frames reported motion)", movedFrames);
    mainPhase.Report("physics_smoke: Update at 1/60");

    CheckStackIntact(registry, scene, "after 240 frames");
    CheckSphereAtRest(registry, scene, spherePositionAt230, "after 240 frames");
    CheckPartsFollowBody(registry, scene, "after 240 frames");
    Check(Transform(registry, scene.compound).position.x > scene.compoundStart.x + 0.01f,
          "AddImpulse moved the compound body along +X (x = %.4f, started at %.4f)",
          Transform(registry, scene.compound).position.x, scene.compoundStart.x);
    {
        // Physics must not have written the kinematic body's transform: the
        // engine owns it.
        const Point3 expected = scene.kinematicStart + Point3(0.005f * 240.0f, 0.0f, 0.0f);
        Check(Distance(Transform(registry, scene.kinematic).position, expected) < 1.0e-6f,
              "the kinematic body's TransformComponent is still exactly what the test set");
    }

    // --- frames that are faster than the fixed step ------------------------
    //
    // At 100 Hz against a 60 Hz step, roughly two frames in five run no fixed
    // step at all and must still write an interpolated transform. A force is
    // pushed every frame to keep something in motion for that to be visible -
    // which is also what tests AddForce and AddTorque.
    Timing fastPhase;
    int emptyFrames = 0;
    int emptyFramesThatWrote = 0;
    const Point3 compoundBeforeForce = Transform(registry, scene.compound).position;
    for (int frame = 0; frame < 100; ++frame)
    {
        // 60 N, not 14: the body is 2 kg on surfaces with friction 0.8, so it
        // does not slide at all until the push clears mu * m * g, about 16 N.
        physics->AddForce(scene.compound, Vec3<float>(0.0f, 0.0f, 60.0f));
        physics->AddTorque(scene.compound, Vec3<float>(0.0f, 0.3f, 0.0f));

        const Point3 before = Transform(registry, scene.compound).position;
        const PhysicsStepResult step = physics->Update(registry, 1.0f / 100.0f);
        fastPhase.Add(step.milliseconds);
        if (step.steps == 0)
        {
            ++emptyFrames;
            emptyFramesThatWrote +=
                Distance(before, Transform(registry, scene.compound).position) > 0.0f ? 1 : 0;
        }
    }
    fastPhase.Report("physics_smoke: Update at 1/100");
    Check(emptyFrames > 20, "a frame faster than the fixed step often runs no step (%d/100 frames)", emptyFrames);
    Check(emptyFramesThatWrote > 0, "a frame that ran no fixed step still wrote an interpolated transform (%d of %d)",
          emptyFramesThatWrote, emptyFrames);
    Check(Transform(registry, scene.compound).position.z > compoundBeforeForce.z + 0.05f,
          "AddForce pushed the compound body along +Z (z = %.4f, was %.4f)",
          Transform(registry, scene.compound).position.z, compoundBeforeForce.z);
    Check(!AnyNaN(registry), "no NaN after 100 frames at 1/100");
    CheckPartsFollowBody(registry, scene, "after 100 frames at 1/100");

    // --- a long stall clamps, and drops the time it did not simulate -------
    {
        const PhysicsStepResult stalled = physics->Update(registry, 1.0f);
        Check(stalled.steps == settings.maxStepsPerUpdate, "a 1 s frame runs at most maxStepsPerUpdate steps (got %u)",
              stalled.steps);
        const PhysicsStepResult after = physics->Update(registry, dt);
        Check(after.steps == 1, "the 56 unsimulated steps were dropped, not queued (next frame ran %u)", after.steps);
    }

    // --- paused ------------------------------------------------------------
    {
        physics->SetPaused(true);
        Check(physics->Paused(), "SetPaused(true) is reflected by Paused()");
        const TransformComponent before = Transform(registry, scene.stack[3]);
        bool anyStep = false;
        bool anyMotion = false;
        for (int frame = 0; frame < 10; ++frame)
        {
            const PhysicsStepResult step = physics->Update(registry, dt);
            anyStep = anyStep || step.steps != 0;
            anyMotion = anyMotion || step.moved;
        }
        Check(!anyStep && !anyMotion, "a paused world runs no steps and reports no motion");
        Check(Distance(Transform(registry, scene.stack[3]).position, before.position) == 0.0f,
              "a paused world writes no transforms");
        physics->SetPaused(false);
        Check(!physics->Paused(), "SetPaused(false) is reflected by Paused()");
    }

    // --- Reset -------------------------------------------------------------
    {
        physics->Reset(registry);
        float worstBody = 0.0f;
        for (auto [entity, transform, rest] : registry.view<TransformComponent, RestTransformComponent>().each())
        {
            if (registry.all_of<ShapeOfComponent>(entity))
            {
                continue; // parts are checked against the composed pose below
            }
            worstBody = SDL_max(worstBody, Distance(transform.position, rest.position));
        }
        Check(worstBody < 1.0e-5f, "Reset put every body back on its RestTransformComponent (worst %.7f m)",
              worstBody);
        Check(Distance(Transform(registry, scene.sphere).position, Point3(2.5f, SmokeScene::kSphereRadius, 1.5f)) <
                  1.0e-5f,
              "Reset restored the sphere exactly");
        CheckPartsFollowBody(registry, scene, "after Reset");

        const PhysicsStepResult first = physics->Update(registry, dt);
        Check(first.moved, "the first Update after Reset reports moved = true");
    }

    // --- settling ----------------------------------------------------------
    {
        // Nothing is driven any more: the stack falls the 2 mm back into
        // contact, the sphere is stopped, and the kinematic box holds still.
        physics->SetVelocity(scene.sphere, Vec3<float>(0.0f), Vec3<float>(0.0f));
        Timing settlePhase;
        int quietFrames = 0;
        int quietFramesAtEnd = 0;
        Point3 sphereEarlier;
        for (int frame = 1; frame <= 180; ++frame)
        {
            if (frame == 170)
            {
                sphereEarlier = Transform(registry, scene.sphere).position;
            }
            const PhysicsStepResult step = physics->Update(registry, dt);
            settlePhase.Add(step.milliseconds);
            if (!step.moved)
            {
                ++quietFrames;
                if (frame > 150)
                {
                    ++quietFramesAtEnd;
                }
            }
        }
        settlePhase.Report("physics_smoke: Update while settling");
        Check(quietFramesAtEnd > 0,
              "a settled scene stops reporting motion (%d quiet frames, %d of them in the last 30)", quietFrames,
              quietFramesAtEnd);
        Check(!AnyNaN(registry), "no NaN after settling");
        CheckStackIntact(registry, scene, "after settling");
        CheckSphereAtRest(registry, scene, sphereEarlier, "after settling");
        CheckPartsFollowBody(registry, scene, "after settling");
    }

    // --- final poses -------------------------------------------------------
    SDL_Log("physics_smoke: final poses");
    SDL_Log("  ground        %s", PoseText(Transform(registry, scene.ground)).c_str());
    for (int level = 0; level < 4; ++level)
    {
        SDL_Log("  stack[%d]      %s", level, PoseText(Transform(registry, scene.stack[level])).c_str());
    }
    SDL_Log("  sphere        %s", PoseText(Transform(registry, scene.sphere)).c_str());
    SDL_Log("  kinematic     %s", PoseText(Transform(registry, scene.kinematic)).c_str());
    SDL_Log("  compound      %s", PoseText(Transform(registry, scene.compound)).c_str());
    SDL_Log("  part[0]       %s", PoseText(Transform(registry, scene.compoundParts[0])).c_str());
    SDL_Log("  part[1]       %s", PoseText(Transform(registry, scene.compoundParts[1])).c_str());

    physics.reset();

    SDL_Log("physics_smoke: %d/%d checks passed", checks - failures, checks);
    if (failures != 0)
    {
        SDL_Log("physics_smoke: FAILED (%d)", failures);
        return 1;
    }
    SDL_Log("physics_smoke: PASSED");
    return 0;
}
