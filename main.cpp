#include <SDL3/SDL.h>
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <entt/entt.hpp>

#include "core/common.h"
#include "renderer/renderer.h"
#include "scene/components.h"
#include "scene/scene.h"
#include "scene/scene_builders.h"
#include "systems/cameraSystem.h"

#define TARGET_FPS 60.0

// Entry point and frame loop. Everything substantial lives elsewhere:
//
//   scene/      the world as components, plus the builders that author it
//   renderer/   the GPU: device, pipeline, buffers, BVH upload, dispatch
//   systems/    things that read input and write components
//
// What is left here is the ordering between them, which is the one thing that
// genuinely belongs at the top level.

namespace
{

    // Physics writes TransformComponent in place. There is no clone and no
    // rest-pose vector: RestTransformComponent holds the authored pose on the
    // same entity, so resetting a body is a component copy rather than a deep
    // copy of the whole scene.
    //
    // Returns true when anything moved, which is what drives the BVH rebuild
    // and the accumulation reset.
    bool StepPhysics(entt::registry &registry, float dt)
    {
        (void)registry;
        (void)dt;
        return false;
    }

    entt::entity CreateCamera(entt::registry &registry)
    {
        const entt::entity camera = registry.create();

        registry.emplace<TagComponent>(camera, "Camera");
        registry.emplace<CameraComponent>(camera,
                                          Vec3<float>(0.0f, 0.0f, 0.0f),
                                          0.0f,  // yaw
                                          0.0f,  // pitch
                                          40.0f, // fov
                                          10.0f, // focus distance
                                          0.2f,  // defocus angle
                                          Color(0.0f, 0.0f, 0.0f),
                                          Color(0.0f, 0.0f, 0.0f));

        // The camera's orientation is driven by CameraComponent's yaw/pitch,
        // not by the transform's quaternion - it only borrows
        // TransformComponent for the position.
        registry.emplace<TransformComponent>(camera,
                                             Point3(0.0f, 0.0f, 0.0f),
                                             Quat<float>(1.0f, 0.0f, 0.0f, 0.0f));

        return camera;
    }

} // namespace

int main()
{
    entt::registry registry;

    RendererSettings settings;
    settings.width = 1920;
    settings.height = 1080;
    settings.samples = 1; // per frame; accumulation does the rest
    settings.renderType = RenderType::Ray_Tracing;
    settings.title = "AviatorEngine";

    Renderer renderer;
    if (!renderer.Initialize(settings))
    {
        return 1;
    }

    // The camera entity has to exist before the scene builder runs: builders
    // author their framing and sky by writing this entity's CameraComponent.
    const entt::entity camera = CreateCamera(registry);

    Scene scene;
    // Pick one: BuildFinalScene / BuildTestAllFeatureScene
    BuildTestAllFeatureScene(scene, registry);
    SDL_Log("Scene: %zu objects, %zu texture(s)", scene.Count(), scene.textures.Count());

    // After the builder, never before - sky, horizon and max depth are the
    // scene's to decide.
    renderer.SyncSceneSettings(scene, registry, camera);

    if (!renderer.LoadScene(scene, registry))
    {
        renderer.Shutdown();
        return 1;
    }

    // Derive an initial yaw/pitch that reproduces the scene's authored framing,
    // then hand control to mouse-look and WASD from here on.
    CameraInitialize(registry);

    bool running = true;
    bool mouseCaptured = true;
    bool fpsCapEnabled = true;
    SDL_SetWindowRelativeMouseMode(renderer.Window(), true);

    const double targetFrameSeconds = 1.0 / TARGET_FPS;
    const Uint64 perfFreq = SDL_GetPerformanceFrequency();
    Uint64 lastCounter = SDL_GetPerformanceCounter();

    double fpsAccum = 0.0;
    int fpsFrameCount = 0;
    double currentFps = 0.0;
    size_t loggedNodeCount = static_cast<size_t>(-1);

    while (running)
    {
        const Uint64 frameStart = SDL_GetPerformanceCounter();
        const double dt = (frameStart - lastCounter) / static_cast<double>(perfFreq);
        lastCounter = frameStart;

        // 1. Input.
        float mouseDX = 0.0f;
        float mouseDY = 0.0f;

        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            switch (event.type)
            {
            case SDL_EVENT_QUIT:
                running = false;
                break;

            case SDL_EVENT_MOUSE_MOTION:
                if (mouseCaptured)
                {
                    mouseDX += event.motion.xrel;
                    mouseDY += event.motion.yrel;
                }
                break;

            case SDL_EVENT_KEY_DOWN:
                if (event.key.scancode == SDL_SCANCODE_ESCAPE)
                {
                    mouseCaptured = !mouseCaptured;
                    SDL_SetWindowRelativeMouseMode(renderer.Window(), mouseCaptured);
                }
                else if (event.key.scancode == SDL_SCANCODE_V)
                {
                    fpsCapEnabled = !fpsCapEnabled;
                }
                break;
            }
        }

        // 2. Camera. Compare before and after rather than tracking a dirty flag
        // in the input handler: WASD is polled, not evented, so a held key
        // produces motion without any event at all.
        const Point3 previousPosition = registry.get<TransformComponent>(camera).position;
        const float previousYaw = registry.get<CameraComponent>(camera).yaw;
        const float previousPitch = registry.get<CameraComponent>(camera).pitch;

        const bool *keys = SDL_GetKeyboardState(nullptr);
        if (mouseCaptured)
        {
            UpdateCamera(registry, keys, mouseDX, mouseDY, static_cast<float>(dt));
        }

        const auto &cameraTransform = registry.get<TransformComponent>(camera);
        const auto &cameraComponent = registry.get<CameraComponent>(camera);
        const bool cameraMoved =
            cameraTransform.position.x != previousPosition.x ||
            cameraTransform.position.y != previousPosition.y ||
            cameraTransform.position.z != previousPosition.z ||
            cameraComponent.yaw != previousYaw ||
            cameraComponent.pitch != previousPitch;

        renderer.SyncCamera(registry, camera);

        // 3. Physics. Anything that moves invalidates both the BVH and the
        // accumulated image - a progressive path tracer averages over frames,
        // so geometry that moved between them smears.
        const bool geometryMoved = StepPhysics(registry, static_cast<float>(dt));
        if (geometryMoved)
        {
            renderer.MarkSceneDirty();
        }
        if (cameraMoved || geometryMoved)
        {
            renderer.ResetAccumulation();
        }

        // 4. Draw.
        renderer.RenderFrame(registry);

        if (renderer.NodeCount() != loggedNodeCount)
        {
            loggedNodeCount = renderer.NodeCount();
            SDL_Log("BVH: %zu nodes, %zu ordered objects", renderer.NodeCount(), renderer.ObjectCount());
        }

        // 5. FPS readout - updated twice a second so it's readable instead of
        // flickering every frame.
        fpsAccum += dt;
        fpsFrameCount++;
        if (fpsAccum >= 0.5)
        {
            currentFps = fpsFrameCount / fpsAccum;
            fpsFrameCount = 0;
            fpsAccum = 0.0;
        }

        char title[192] = {0};
        SDL_snprintf(title, sizeof(title),
                     "%s | %.0f fps (%s, press V) | %zu nodes / %zu objects | %u spp | mouse: %s (Esc)",
                     renderer.DriverName(),
                     currentFps,
                     fpsCapEnabled ? "capped" : "uncapped",
                     renderer.NodeCount(),
                     renderer.ObjectCount(),
                     renderer.AccumulatedSamples(),
                     mouseCaptured ? "captured" : "free");
        SDL_SetWindowTitle(renderer.Window(), title);

        // 6. Pace the frame if capped - sleep off whatever is left of the
        // target budget rather than a fixed amount, so the cap holds regardless
        // of how long the work above actually took.
        if (fpsCapEnabled)
        {
            const Uint64 frameEnd = SDL_GetPerformanceCounter();
            const double frameSeconds = (frameEnd - frameStart) / static_cast<double>(perfFreq);
            const double remaining = targetFrameSeconds - frameSeconds;
            if (remaining > 0.0)
            {
                SDL_Delay(static_cast<Uint32>(remaining * 1000.0));
            }
        }
    }

    registry.clear();
    renderer.Shutdown();
    return 0;
}
