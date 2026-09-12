#include <SDL3/SDL.h>
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <memory>

#include <entt/entt.hpp>

#include "core/common.h"
#include "physics/physicsWorld.h"
#include "renderer/renderer.h"
#include "scene/components.h"
#include "scene/scene.h"
#include "scene/scene_builders.h"
#include "systems/cameraSystem.h"
#include "systems/physicsDemoSystem.h"

#define TARGET_FPS 60.0

// Entry point and frame loop. Everything substantial lives elsewhere:
//
//   scene/      the world as components, plus the builders that author it
//   physics/    IPhysicsWorld: reads the physics components, writes transforms
//   renderer/   the GPU: device, pipeline, buffers, BVH upload, dispatch
//   systems/    things that read input and write components
//
// What is left here is the ordering between them, which is the one thing that
// genuinely belongs at the top level.

namespace
{

    // Command line, for running the app unattended: --frames quits after N
    // frames, and --no-mouse-capture leaves the pointer alone so a test run
    // does not steal it.
    struct DemoOptions
    {
        Uint64 frameLimit = 0; // 0 = run until the window closes
        Uint64 pauseAfter = 0; // 0 = never; otherwise pause physics after N frames
        bool captureMouse = true;
    };

    DemoOptions ParseArguments(int argc, char **argv)
    {
        DemoOptions options;

        for (int i = 1; i < argc; i++)
        {
            if (SDL_strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            {
                options.frameLimit = static_cast<Uint64>(SDL_atoi(argv[++i]));
            }
            else if (SDL_strcmp(argv[i], "--pause-after") == 0 && i + 1 < argc)
            {
                // Physics stops after N frames, which is how an unattended run
                // reaches a settled scene: the demo's sweeper never stops on
                // its own, and the path tracer only converges once nothing moves.
                options.pauseAfter = static_cast<Uint64>(SDL_atoi(argv[++i]));
            }
            else if (SDL_strcmp(argv[i], "--no-mouse-capture") == 0)
            {
                options.captureMouse = false;
            }
            else
            {
                SDL_Log("Usage: app [--frames N] [--pause-after N] [--no-mouse-capture]");
            }
        }

        return options;
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

    // The entity a given tag names, or entt::null. Used once at start-up to find
    // the cannonball, so the status line can report something that really moves.
    entt::entity FindTagged(const entt::registry &registry, const char *tag)
    {
        for (auto [entity, tagComponent] : registry.view<const TagComponent>().each())
        {
            if (tagComponent.tag == tag)
            {
                return entity;
            }
        }
        return entt::null;
    }

} // namespace

int main(int argc, char **argv)
{
    const DemoOptions options = ParseArguments(argc, argv);

    entt::registry registry;

    // The camera entity has to exist before the scene builder runs: builders
    // author their framing and sky by writing this entity's CameraComponent.
    const entt::entity camera = CreateCamera(registry);

    Scene scene;
    // Pick one: BuildFinalScene / BuildTestAllFeatureScene / BuildPhysicsScene
    BuildPhysicsScene(scene, registry);
    SDL_Log("Scene: %zu objects, %zu texture(s)", scene.Count(), scene.textures.Count());

    // Physics before the window, deliberately. The first Build compiles Newton's
    // GPU kernels, which takes anywhere from seconds to minutes on a cold kernel
    // cache; doing it here means the console shows progress instead of an
    // unresponsive window. Nothing in the scene builder needs the GPU - textures
    // are decoded on the CPU and uploaded later, by LoadScene.
    PhysicsSettings physicsSettings;
    std::unique_ptr<IPhysicsWorld> physics = CreatePhysicsWorld(physicsSettings);

    SDL_Log("Physics: %s - building bodies...", physics->Description());
    if (!physics->Build(registry))
    {
        SDL_Log("Physics: Build failed; the scene will not move");
    }
    SDL_Log("Physics: %zu bodies", physics->BodyCount());

    RendererSettings settings;
    settings.width = 1920;
    settings.height = 1080;
    settings.samples = 1; // per frame; accumulation does the rest
    settings.maxSamples = 50; // stop tracing once converged; 0 = never stop
    settings.renderType = RenderType::Ray_Tracing;
    settings.hybrid = true; // raster G-buffer + ray tracing; H toggles at runtime
    // Full traces every pixel; ThreeQuarter traces a 3/4-scale grid (9/16 of the
    // pixels) and Half a 1/2-scale grid (1/4) per frame, each upsampled back.
    settings.traceResolution = TraceResolution::ThreeQuarter;
    settings.title = "AviatorEngine";

    Renderer renderer;
    if (!renderer.Initialize(settings))
    {
        return 1;
    }

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

    const entt::entity cannonball = FindTagged(registry, "Cannonball");

    bool running = true;
    bool mouseCaptured = options.captureMouse;
    bool fpsCapEnabled = true;
    SDL_SetWindowRelativeMouseMode(renderer.Window(), mouseCaptured);

    SDL_Log("Physics keys: P pause, N single step (while paused), R reset, B blast, F fire the cannonball");

    const double targetFrameSeconds = 1.0 / TARGET_FPS;
    const Uint64 perfFreq = SDL_GetPerformanceFrequency();
    Uint64 lastCounter = SDL_GetPerformanceCounter();

    double fpsAccum = 0.0;
    int fpsFrameCount = 0;
    double currentFps = 0.0;
    size_t loggedNodeCount = static_cast<size_t>(-1);

    // Simulated time, which is what drives the kinematic bodies. It stops while
    // physics is paused, so the sweeper arm stops with everything else.
    double demoTime = 0.0;
    double statusAccum = 0.0;
    Uint64 frameIndex = 0;
    PhysicsStepResult physicsStep;

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
                else if (event.key.scancode == SDL_SCANCODE_H)
                {
                    renderer.SetHybrid(!renderer.Hybrid());
                    SDL_Log("Renderer: %s", renderer.Hybrid() ? "hybrid" : "ray traced primaries");
                }
                else if (event.key.scancode == SDL_SCANCODE_P)
                {
                    physics->SetPaused(!physics->Paused());
                    SDL_Log("Physics: %s", physics->Paused() ? "paused" : "running");
                }
                else if (event.key.scancode == SDL_SCANCODE_N && physics->Paused())
                {
                    // One fixed step while paused, for watching a contact
                    // resolve frame by frame.
                    physics->SetPaused(false);
                    const PhysicsStepResult single =
                        physics->Update(registry, physicsSettings.fixedTimeStep);
                    physics->SetPaused(true);

                    if (single.moved)
                    {
                        renderer.MarkSceneDirty();
                        renderer.ResetAccumulation();
                    }
                    SDL_Log("Physics: single step (%.2f ms)", single.milliseconds);
                }
                else if (event.key.scancode == SDL_SCANCODE_R)
                {
                    physics->Reset(registry);
                    renderer.MarkSceneDirty();
                    renderer.ResetAccumulation();
                    demoTime = 0.0;
                    SDL_Log("Physics: reset to the authored poses");
                }
                else if (event.key.scancode == SDL_SCANCODE_B)
                {
                    const int hit = BlastDynamicBodies(registry, *physics,
                                                       Point3(0.0f, 1.0f, -4.0f), 14.0f, 7.0f);
                    SDL_Log("Physics: blast hit %d bodies", hit);
                }
                else if (event.key.scancode == SDL_SCANCODE_F)
                {
                    const bool fired = KickCannonball(registry, *physics, 24.0f);
                    SDL_Log("Physics: cannonball %s", fired ? "fired" : "not found");
                }
                else if (event.key.scancode >= SDL_SCANCODE_F1 &&
                         event.key.scancode < SDL_SCANCODE_F1 + static_cast<int>(DebugView::Count))
                {
                    // F1 is the final image; F2 onward step through the
                    // G-buffer and visibility debug views. Only the hybrid
                    // renderer draws them.
                    renderer.SetDebugView(static_cast<DebugView>(event.key.scancode - SDL_SCANCODE_F1));
                    SDL_Log("Debug view: %s", DebugViewName(renderer.GetDebugView()));
                }
                else if (event.key.scancode == SDL_SCANCODE_LEFTBRACKET)
                {
                    // Exposure is a display-time grade, so it deliberately does
                    // NOT reset accumulation - the linear buffer is untouched
                    // and the next frame simply tone-maps it differently.
                    renderer.SetExposure(renderer.Exposure() / 1.25f);
                    SDL_Log("Exposure: %.3f", renderer.Exposure());
                }
                else if (event.key.scancode == SDL_SCANCODE_RIGHTBRACKET)
                {
                    renderer.SetExposure(renderer.Exposure() * 1.25f);
                    SDL_Log("Exposure: %.3f", renderer.Exposure());
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

        // 3. Physics. Kinematic bodies are driven by writing their transform, so
        // they move BEFORE the step that reads them as targets.
        if (!physics->Paused())
        {
            demoTime += dt;
        }
        AnimateKinematicBodies(registry, static_cast<float>(demoTime));

        physicsStep = physics->Update(registry, static_cast<float>(dt));

        // Anything that moves invalidates both the BVH and the accumulated
        // image - a progressive path tracer averages over frames, so geometry
        // that moved between them smears.
        if (physicsStep.moved)
        {
            renderer.MarkSceneDirty();
        }

        // Deliberately NOT ResetAccumulation for moving geometry. That drops
        // every pixel's history (HistoryReset in ReprojectHistory), and with
        // anything moving every frame - the sweeper arm never stops - the image
        // restarts at one sample forever, which is just noise.
        //
        // This path keeps the history and reprojects it instead. The shader
        // validates each pixel against last frame's surface: a pixel a body
        // moved through fails the plane test and restarts by itself, while the
        // static majority of the frame - ground, walls, a settled stack - keeps
        // accumulating, capped at RendererSettings::historyWhileMoving. Metal
        // and glass are capped harder (4 frames), because a reflection does not
        // move with the surface it is seen in.
        if (physicsStep.moved || cameraMoved)
        {
            renderer.CameraMoved();
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

        // A physics line every couple of seconds, so an unattended run leaves
        // evidence that bodies actually moved.
        statusAccum += dt;
        if (statusAccum >= 2.0)
        {
            statusAccum = 0.0;

            if (cannonball != entt::null)
            {
                const Point3 &p = registry.get<TransformComponent>(cannonball).position;
                SDL_Log("Physics: %s | %u steps, %.2f ms | %u spp%s | cannonball (%.2f, %.2f, %.2f)",
                        physicsStep.moved ? "moving" : "settled",
                        physicsStep.steps, physicsStep.milliseconds,
                        renderer.AccumulatedSamples(), renderer.Converged() ? " (converged)" : "",
                        p.x, p.y, p.z);
            }
            else
            {
                SDL_Log("Physics: %s | %u steps, %.2f ms",
                        physicsStep.moved ? "moving" : "settled",
                        physicsStep.steps, physicsStep.milliseconds);
            }
        }

        char title[384] = {0};
        SDL_snprintf(title, sizeof(title),
                     "%s | %s, view %s (H, F1-F7) | %.0f fps (%s, press V) | %zu nodes / %zu objects | %u spp%s | exp %.2f ([/]) | %s: %zu bodies, %.2f ms%s | mouse: %s (Esc)",
                     renderer.DriverName(),
                     renderer.Hybrid() ? "hybrid" : "ray traced",
                     DebugViewName(renderer.GetDebugView()),
                     currentFps,
                     fpsCapEnabled ? "capped" : "uncapped",
                     renderer.NodeCount(),
                     renderer.ObjectCount(),
                     renderer.AccumulatedSamples(),
                     renderer.Converged() ? " (done)" : "",
                     renderer.Exposure(),
                     physics->Description(),
                     physics->BodyCount(),
                     physicsStep.milliseconds,
                     physics->Paused() ? " (paused, P)" : "",
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

        frameIndex++;
        if (options.pauseAfter > 0 && frameIndex == options.pauseAfter && !physics->Paused())
        {
            physics->SetPaused(true);
            SDL_Log("Physics: paused after %llu frames (--pause-after)",
                    static_cast<unsigned long long>(options.pauseAfter));
        }
        if (options.frameLimit > 0 && frameIndex >= options.frameLimit)
        {
            SDL_Log("Reached --frames %llu; exiting", static_cast<unsigned long long>(options.frameLimit));
            running = false;
        }
    }

    // Physics owns an embedded Python interpreter and GPU buffers of its own, so
    // it goes down before the renderer takes the graphics device with it.
    physics.reset();

    registry.clear();
    renderer.Shutdown();
    return 0;
}
