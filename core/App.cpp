/*
 * App.cpp
 * -------
 * Application lifecycle implementation.
 *
 * Initialize() acts as the engine's "startup system" — it boots the renderer,
 * then populates the world using scene-loader helpers (SpawnBox, SpawnPointLight, …).
 *
 * RunFrame() is the main loop body:
 *   1. Advance the Time resource.
 *   2. Run World::Update() (camera system → physics step → transform sync).
 *   3. Clear per-frame mouse delta.
 *   4. Hand the registry to the Renderer.
 */

#include "core/App.h"
#include "scene/SceneLoader.h"
#include "ecs/Components.h"

#include <array>
#include <iostream>

// ── Static member definitions ─────────────────────────────────────────────────

Renderer App::s_Renderer;
World App::s_World;
InputState App::s_Input;
Time App::s_Time;

// The classic Cornell box: two coloured side walls, three white ones, a ceiling
// light, and two boxes. Lit entirely by the ceiling light against a black sky.
// void BuildCornellBox()
// {
//     s_World.camera.position = Point3(278.0f, 278.0f, -800.0f);
//     s_World.camera.target = Point3(278.0f, 278.0f, 0.0f);
//     s_World.camera.fov = 40.0f;
//     s_World.camera.focus_dist = 800.0f;
//     s_World.camera.sky = Color(0.0f, 0.0f, 0.0f);
//     s_World.camera.horizon = Color(0.0f, 0.0f, 0.0f);
//     s_World.maxDepth = 2;

//     const Material white = Material::Lambertian(Color(0.73f, 0.73f, 0.73f));
//     const Material red = Material::Lambertian(Color(0.65f, 0.05f, 0.05f));
//     const Material green = Material::Lambertian(Color(0.12f, 0.45f, 0.15f));
//     const Material light = Material::Emissive(Color(1.0f, 1.0f, 1.0f), 65.0f);
//     const Material redLight = Material::Emissive(Color(0.65f, 0.65f, 0.65f), 10.0f);
//     const Material volume = Material::Volume(Color(0.22f, 0.22f, 0.1f), 0.3f);

//     // Walls
//     scene.AddQuad(Point3(555.0f, 0.0f, 0.0f), Vec3(0.0f, 555.0f, 0.0f), Vec3(0.0f, 0.0f, 555.0f), green);       // left
//     scene.AddQuad(Point3(0.0f, 0.0f, 0.0f), Vec3(0.0f, 555.0f, 0.0f), Vec3(0.0f, 0.0f, 555.0f), red);           // right
//     scene.AddQuad(Point3(0.0f, 0.0f, 0.0f), Vec3(555.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, 555.0f), white);         // floor
//     scene.AddQuad(Point3(555.0f, 555.0f, 555.0f), Vec3(-555.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, -555.0f), white); // ceiling
//     scene.AddQuad(Point3(0.0f, 0.0f, 555.0f), Vec3(555.0f, 0.0f, 0.0f), Vec3(0.0f, 555.0f, 0.0f), white);       // back

//     // Ceiling light, just below the ceiling so it isn't coplanar with it.
//     scene.AddQuad(Point3(343.0f, 554.0f, 332.0f), Vec3(-130.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, -105.0f), light);

//     scene.AddSphere(Point3(350.0f, 150.0f, 50.0f), 50.0f, redLight);

//     // Both boxes are built axis-aligned at the origin corner, then rotated as
//     // rigid bodies and translated into place - the canonical Cornell framing.
//     scene.AddBox(Point3(0.0f, 0.0f, 0.0f), Point3(165.0f, 330.0f, 165.0f), volume)
//         .RotateY(static_cast<float>(degrees_to_radians(15.0)))
//         .Translate(Vec3(265.0f, 0.0f, 295.0f));

//     scene.AddBox(Point3(0.0f, 0.0f, 0.0f), Point3(165.0f, 165.0f, 165.0f), volume)
//         .RotateY(static_cast<float>(degrees_to_radians(-18.0)))
//         .Translate(Vec3(130.0f, 0.0f, 65.0f));
// }

// A deliberately small scene for reading what `density` actually does. Three
// identical fog spheres sit in a row over a lit floor, an order of magnitude
// apart in density, with a solid red sphere buried in each so you can see how
// far into the medium you can still see.
//
// Density is per world unit: the chance of crossing distance d without
// scattering is exp(-density * d), so mean free path is 1/density. At the 2.0
// radius here that's 10 units for the thin one (barely visible), 1 unit for the
// middle one (smoke), and 0.1 for the thick one (nearly opaque).
// void BuildFogTest(Scene &scene)
// {
//     scene.camera.position = Point3(0.0f, 3.0f, -14.0f);
//     scene.camera.target = Point3(0.0f, 1.0f, 0.0f);
//     scene.camera.fov = 40.0f;
//     scene.camera.focus_dist = 14.0f;
//     scene.camera.sky = Color(0.02f, 0.03f, 0.05f);
//     scene.camera.horizon = Color(0.01f, 0.01f, 0.02f);
//     scene.maxDepth = 40; // dense fog: many scatters before a path finds the light

//     const Material floor = Material::Lambertian(Color(0.6f, 0.6f, 0.6f));
//     const Material marker = Material::Lambertian(Color(0.9f, 0.1f, 0.1f));

//     scene.AddQuad(Point3(-20.0f, 0.0f, -20.0f),
//                   Vec3(40.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, 40.0f), floor);

//     // Two overhead lights, so the fog is side-lit and its depth reads.
//     scene.AddQuad(Point3(-6.0f, 9.0f, -3.0f),
//                   Vec3(12.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, 6.0f),
//                   Material::Emissive(Color(1.0f, 0.95f, 0.9f), 6.0f));

//     const float densities[3] = {0.1f, 1.0f, 10.0f};
//     for (int i = 0; i < 3; i++)
//     {
//         const float x = (i - 1) * 5.5f;

//         // The marker sits at the centre of the fog ball.
//         scene.AddSphere(Point3(x, 2.0f, 0.0f), 0.6f, marker);
//         scene.AddVolume(Point3(x, 2.0f, 0.0f), 2.0f, Color(0.9f, 0.9f, 0.95f), densities[i]);
//     }

//     // A fog-filled box on the right, to check the Box boundary path as well as
//     // the sphere one - they take different span code in the shader.
//     scene.AddSphere(Point3(9.0f, 1.2f, 3.0f), 0.6f, marker);
//     scene.AddVolume(Point3(7.0f, 0.0f, 1.0f), Point3(11.0f, 3.0f, 5.0f),
//                     Color(0.35f, 0.55f, 0.95f), 1.0f)
//         .RotateY(static_cast<float>(degrees_to_radians(20.0)));

//     // A solid box on the left for reference, same size, same primitive.
//     scene.AddBox(Point3(-11.0f, 0.0f, 1.0f), Point3(-7.0f, 3.0f, 5.0f),
//                  Material::Lambertian(Color(0.3f, 0.7f, 0.4f)))
//         .RotateY(static_cast<float>(degrees_to_radians(-20.0)));
// }

Ray Tracing In One Weekend's "final scene": a big ground sphere, three hero
spheres showing off each material type, and a field of small random ones.
Lit by a blue sky gradient rather than by any emissive geometry.
void BuildFinalScene(Scene &scene)
{
    scene.camera.position = Point3(13.0f, 2.0f, 3.0f);
    scene.camera.target = Point3(0.0f, 0.0f, 0.0f);
    scene.camera.fov = 20.0f;
    scene.camera.focus_dist = 10.0f;
    scene.camera.defocus_angle = 0.6f;
    scene.camera.sky = Color(0.5f, 0.7f, 1.0f);
    scene.camera.horizon = Color(1.0f, 1.0f, 1.0f);

    scene.maxDepth = 5;

    const Texture *brick = scene.textures.Load("brick/textures/red_brick_diff_4k.jpg");

    scene.AddSphere(Point3(10.0f, 10.0f,0.0f), 5.0f,Material::Emissive(Color(0.54f,0.67f,0.3f),1.0f));

    scene.AddSphere(
        Point3(0.0f, -1000.0f, 0.0f), 1000.0f,
        Material::Lambertian(Color(0.5f, 0.5f, 0.5f)));

    // The three heroes: glass, matte (brick-textured), and metal.
    scene.AddSphere(Point3(0.0f, 1.0f, 0.0f), 1.0f, Material::Dielectric(1.5f));

    // Rotating a sphere can't change its shape, so RotateY only turns the
    // texture on it - which is exactly what you want for aiming a brick seam.
    scene.AddSphere(
             Point3(-4.0f, 1.0f, 0.0f), 1.0f,
             Material::Lambertian(Color(1.0f, 1.0f, 1.0f)).Textured(brick))
        .RotateY(static_cast<float>(degrees_to_radians(90.0)));
    scene.AddSphere(Point3(4.0f, 1.0f, 0.0f), 1.0f, Material::Metal(Color(0.7f, 0.6f, 0.5f)));

    SDL_srand(0);
    for (int a = -5; a < 5; a++)
    {
        for (int b = -5; b < 5; b++)
        {
            const Point3 position(a + 0.9f * SDL_randf(), 0.2f, b + 0.9f * SDL_randf());
            const float roll = SDL_randf();

            Material mat;
            if (roll < 0.8f)
            {
                mat = Material::Lambertian(Color(SDL_randf(), SDL_randf(), SDL_randf()));
            }
            else if (roll < 0.95f)
            {
                mat = Material::Metal(
                    Color(0.5f + 0.5f * SDL_randf(), 0.5f + 0.5f * SDL_randf(), 0.5f + 0.5f * SDL_randf()),
                    SDL_randf() * 0.2f);
            }
            else
            {
                mat = Material::Dielectric(1.5f);
            }

            scene.AddSphere(position, 0.2f, mat);
        }
    }
}


// ── Initialize ────────────────────────────────────────────────────────────────

SDL_AppResult App::Initialize()
{
    s_Renderer.Initialize();
    

    return SDL_APP_CONTINUE;
}

// ── RunFrame ──────────────────────────────────────────────────────────────────

SDL_AppResult App::RunFrame()
{
    // Advance time resource.
    const float now = (float)SDL_GetTicks() / 1000.0f;
    s_Time.deltaTime = now - s_Time.lastFrame;
    s_Time.elapsed += s_Time.deltaTime;
    s_Time.lastFrame = now;

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
                    SDL_SetWindowRelativeMouseMode(window, mouseCaptured);
                }
                else if (event.key.scancode == SDL_SCANCODE_V)
                {
                    fpsCapEnabled = !fpsCapEnabled;
                }
                break;
            }
        }
        if (!running)
        {
            break;
        }

        // 1. Move the camera (mouse-look + WASD/Space/Ctrl, sprint on Shift).
        const bool *keys = SDL_GetKeyboardState(nullptr);

        Vec3<float> oldCameraPosition = scene.camera.position;
        float oldYaw = scene.camera.yaw;
        float oldPitch = scene.camera.pitch;
        if (mouseCaptured)
        {
            scene.camera.Update(keys, mouseDX, mouseDY, s_Time.deltaTime);
        }

        const Vec3<float> forward = scene.camera.Forward();
        config.source_x = scene.camera.position.x;
        config.source_y = scene.camera.position.y;
        config.source_z = scene.camera.position.z;
        config.target_x = scene.camera.position.x + forward.x;
        config.target_y = scene.camera.position.y + forward.y;
        config.target_z = scene.camera.position.z + forward.z;
        config.focus_dist = scene.camera.focus_dist;
        config.defocus_angle = scene.camera.defocus_angle;
        config.fov = scene.camera.fov;
        // up_x/up_y/up_z stay fixed at (0,1,0) - this is a roll-free FPS camera.

        // Did the camera moved
        bool cameraMoved =
            scene.camera.position.x != oldCameraPosition.x ||
            scene.camera.position.y != oldCameraPosition.y ||
            scene.camera.position.z != oldCameraPosition.z ||
            scene.camera.yaw != oldYaw ||
            scene.camera.pitch != oldPitch;

        // Reset accumulation
        if (cameraMoved)
        {
            accumulationFrame = 0;
        }

        config.batch = accumulationFrame;

        // 2. Step motion/physics.
        // StepPhysics(liveObjects, restPose, static_cast<float>(elapsedTime));

        // 3. Rebuild the BVH around the new positions. Log only when its shape
        // changes - this runs every frame, and logging unconditionally buried
        // every other message under 60 lines a second.
        if (sceneDirty)
        {
            BuildBVH(liveObjects, nodes, orderedObjects);
            lightIDs = LightIndices(orderedObjects);
            config.num_spheres = static_cast<Uint32>(orderedObjects.size());
            config.num_lights = static_cast<Uint32>(lightIDs.size());
            // 4. Upload the reordered spheres + flattened nodes.
            if (!UploadScene(sceneBuffers, ObjectsToGPUObjects(orderedObjects), NodesToGPUNodes(nodes), lightIDs))
            {
                SDL_Log("Failed to upload scene");
                break;
            }
            sceneDirty = false;
        }

        if (nodes.size() != loggedNodeCount)
        {
            loggedNodeCount = nodes.size();
            SDL_Log("BVH: %zu nodes, %zu ordered objects", nodes.size(), orderedObjects.size());
        }

        SDL_GPUCommandBuffer *command_buffer = SDL_AcquireGPUCommandBuffer(device);
        if (!command_buffer)
        {
            SDL_Log("Failed to acquire command buffer: %s", SDL_GetError());
            continue;
        }

        SDL_PushGPUComputeUniformData(command_buffer, 0, &config, sizeof(config));
        SDL_GPUStorageTextureReadWriteBinding storageTextureBinding = {};
        storageTextureBinding.texture = texture1;
        SDL_GPUComputePass *compute_pass = SDL_BeginGPUComputePass(command_buffer, &storageTextureBinding, 1, nullptr, 0);
        if (!compute_pass)
        {
            SDL_Log("Failed to begin compute pass: %s", SDL_GetError());
            return 1;
        }
        SDL_BindGPUComputePipeline(compute_pass, pipeline);
        SDL_GPUBuffer *storageBuffers[3] = {sceneBuffers.objectBuffer, sceneBuffers.bvhBuffer, sceneBuffers.lightIDBuffer};
        SDL_BindGPUComputeStorageBuffers(compute_pass, 0, storageBuffers, 3);

        SDL_GPUTextureSamplerBinding textureBinding = {};
        textureBinding.texture = globalTextureArray;
        textureBinding.sampler = linearSampler;

        SDL_BindGPUComputeSamplers(
            compute_pass,
            0,
            &textureBinding,
            1);
        SDL_DispatchGPUCompute(compute_pass, (config.width + THREADS - 1) / THREADS, config.height, 1);
        SDL_EndGPUComputePass(compute_pass);
        SDL_GPUTexture *swapchain;
        Uint32 width;
        Uint32 height;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(
                command_buffer, window, &swapchain, &width, &height))
        {
            SDL_Log("Failed to acquire swapchain texture: %s", SDL_GetError());
            SDL_CancelGPUCommandBuffer(command_buffer);
            continue;
        }
        if (!swapchain)
        {
            SDL_CancelGPUCommandBuffer(command_buffer);
            continue;
        }
        SDL_GPUBlitInfo blit = {};
        blit.source = {};
        blit.source.texture = texture1;
        blit.source.w = config.width;
        blit.source.h = config.height;
        blit.destination = {};
        blit.destination.texture = swapchain;
        blit.destination.w = width;
        blit.destination.h = height;

        SDL_BlitGPUTexture(command_buffer, &blit);
        if (!SDL_SubmitGPUCommandBuffer(command_buffer))
        {
            SDL_Log("Failed to submit scene upload: %s", SDL_GetError());
            return false;
        }

        // 5. FPS bookkeeping - update the readout twice a second so it's
        // readable instead of flickering every frame.
        fpsAccum += s_Time.deltaTime;
        fpsFrameCount++;
        if (fpsAccum >= 0.5)
        {
            currentFps = fpsFrameCount / fpsAccum;
            fpsFrameCount = 0;
            fpsAccum = 0.0;
        }
        char title[192] = {0};
        SDL_snprintf(title, sizeof(title),
                     "%s | %.0f fps (%s, press V) | %u nodes / %u objects | %u spp | mouse: %s (Esc)",
                     SDL_GetGPUDeviceDriver(device),
                     currentFps,
                     fpsCapEnabled ? "capped" : "uncapped",
                     static_cast<Uint32>(nodes.size()),
                     static_cast<Uint32>(orderedObjects.size()),
                     (accumulationFrame + 1) * config.samples,
                     mouseCaptured ? "captured" : "free");
        SDL_SetWindowTitle(window, title);

        // 6. Pace the frame if capped - sleep off whatever's left of the
        // target frame budget instead of always sleeping a fixed amount,
        // so the cap is accurate regardless of how long the GPU/CPU work
        // above actually took.
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
        accumulationFrame++;
}

// ── ProcessInput ──────────────────────────────────────────────────────────────

SDL_AppResult App::ProcessInput(SDL_Event *event)
{
    if (event->type == SDL_EVENT_QUIT)
        return SDL_APP_SUCCESS;

    if (event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP)
    {
        const bool isDown = (event->type == SDL_EVENT_KEY_DOWN);
        switch (event->key.key)
        {
        case SDLK_W:
            s_Input.up = isDown;
            break;
        case SDLK_S:
            s_Input.down = isDown;
            break;
        case SDLK_A:
            s_Input.left = isDown;
            break;
        case SDLK_D:
            s_Input.right = isDown;
            break;
        case SDLK_LSHIFT:
            s_Input.shift = isDown;
            break;
        case SDLK_ESCAPE:
            return SDL_APP_SUCCESS;
        default:
            break;
        }
    }

    if (event->type == SDL_EVENT_MOUSE_MOTION)
    {
        s_Input.mouseOffset.x = (float)event->motion.xrel;
        s_Input.mouseOffset.y = (float)event->motion.yrel;
    }

    return SDL_APP_CONTINUE;
}

// ── Shutdown ──────────────────────────────────────────────────────────────────
void App::Shutdown()
{
    s_Renderer.TerminateRenderer();
    SDL_Quit();
}