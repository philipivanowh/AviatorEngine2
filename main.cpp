#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include "aabb.h"
#include "bvh.h"
#include "common.h"
#include "camera.h"
#include "material.h"
#include "texture.h"

#define THREADS 256
 #define SCENE_SPHERES (4 + 22 * 22)

// #define SCENE_SPHERES (4)
#define MAX_SPHERES SCENE_SPHERES
#define MAX_NODES (MAX_SPHERES * 2) // safe upper bound for a binary tree over MAX_SPHERES leaves
#define TARGET_FPS 60.0

struct Config
{
    float source_x;
    float source_y;
    float source_z;
    float fov;
    float target_x;
    float target_y;
    float target_z;
    float focus_dist;
    float defocus_angle;
    float up_x;
    float up_y;
    float up_z;
    float oof;
    float sky_r;
    float sky_g;
    float sky_b;
    Uint32 width;
    float horizon_r;
    float horizon_g;
    float horizon_b;
    Uint32 height;
    Uint32 samples;
    Uint32 batches;
    Uint32 batch;
    Uint32 depth;
    Uint32 num_spheres;
    Uint32 padding[3];
};

struct SceneBuffers
{
    SDL_GPUBuffer *sphereBuffer;
    SDL_GPUBuffer *bvhBuffer;
    SDL_GPUBuffer *textureBuffer;
    SDL_GPUTransferBuffer *sphereTransfer;
    SDL_GPUTransferBuffer *bvhTransfer;
    SDL_GPUTransferBuffer *textureTransfer;
};

SDL_GPUDevice *device;

std::vector<Sphere_GPU> spheres;
std::vector<Texture *> textures;
Uint32 textureIDCounter = 0;

SDL_GPUComputePipeline *CreateComputePipeline(SDL_GPUDevice *device)
{
    SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);
    const char *path;
    const char *entrypoint;
    SDL_GPUShaderFormat format;
    if (formats & SDL_GPU_SHADERFORMAT_SPIRV)
    {
        path = "shader.comp.spv";
        entrypoint = "main";
        format = SDL_GPU_SHADERFORMAT_SPIRV;
    }
    else if (formats & SDL_GPU_SHADERFORMAT_DXIL)
    {
        path = "shader.comp.dxil";
        entrypoint = "main";
        format = SDL_GPU_SHADERFORMAT_DXIL;
    }
    else if (formats & SDL_GPU_SHADERFORMAT_MSL)
    {
        path = "shader.comp.msl";
        entrypoint = "main0";
        format = SDL_GPU_SHADERFORMAT_MSL;
    }
    else
    {
        SDL_Log("No supported shader format");
        return nullptr;
    }
    size_t size;
    void *data = SDL_LoadFile(path, &size);
    if (!data)
    {
        SDL_Log("Failed to load shader: %s", SDL_GetError());
        return nullptr;
    }
    SDL_GPUComputePipelineCreateInfo cpci = {0};
    cpci.code = static_cast<Uint8 *>(data);
    cpci.code_size = size;
    cpci.entrypoint = entrypoint;
    cpci.format = format;
    cpci.num_samplers = 1;
    cpci.num_readonly_storage_buffers = 2; // spheres (t0) + bvh nodes (t1)
    cpci.num_readwrite_storage_textures = 1;
    cpci.num_uniform_buffers = 1;
    cpci.threadcount_x = THREADS;
    cpci.threadcount_y = 1;
    cpci.threadcount_z = 1;
    SDL_GPUComputePipeline *pipeline = SDL_CreateGPUComputePipeline(device, &cpci);
    SDL_free(data);
    if (!pipeline)
    {
        SDL_Log("Failed to create compute pipeline: %s", SDL_GetError());
        return nullptr;
    }
    return pipeline;
}

void CreateSphere(point3 pos, point3 pos2, float radius, Uint32 type, Material mat)
{

    Sphere_GPU sphereGPU = Sphere_GPU{
        .x = pos.x,
        .y = pos.y,
        .z = pos.z,
        .x2 = pos2.x,
        .y2 = pos2.y,
        .z2 = pos2.z,
        .radius = radius,
        .r = mat.color.x,
        .g = mat.color.y,
        .b = mat.color.z,
        .fuzz = mat.fuzz,
        .refraction = mat.refraction,
        .type = mat.type,
    };
    if (!mat.texture)
    {
        sphereGPU.textureID = -1;
    }
    else
    {
        sphereGPU.textureID = textureIDCounter;
        textures.push_back(mat.texture);
        textureIDCounter++;
    }

    spheres.push_back(sphereGPU);
}

void CreateSphere(point3 pos, float radius, Material mat)
{

    Sphere_GPU sphereGPU = Sphere_GPU{
        .x = pos.x,
        .y = pos.y,
        .z = pos.z,
        .x2 = pos.x,
        .y2 = pos.y,
        .z2 = pos.z,
        .radius = radius,
        .r = mat.color.x,
        .g = mat.color.y,
        .b = mat.color.z,
        .fuzz = mat.fuzz,
        .refraction = mat.refraction,
        .type = mat.type};
    if (!mat.texture)
    {
        sphereGPU.textureID = -1;
    }
    else
    {
        sphereGPU.textureID = textureIDCounter;
        textures.push_back(mat.texture);
        textureIDCounter++;
    }

    spheres.push_back(sphereGPU);
}

SDL_GPUTexture *CreateTextureArray(
    SDL_GPUDevice *device,
    const std::vector<SDL_GPUTexture *> &textures,
    SDL_GPUTexture *fallbackTexture,
    Uint32 width,
    Uint32 height)
{
    const Uint32 layerCount =
        textures.empty()
            ? 1
            : static_cast<Uint32>(textures.size());

    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = layerCount;
    info.num_levels = 1;

    SDL_GPUTexture *textureArray =
        SDL_CreateGPUTexture(device, &info);

    if (!textureArray)
    {
        SDL_Log(
            "Failed to create texture array: %s",
            SDL_GetError());
        return nullptr;
    }

    SDL_GPUCommandBuffer *commandBuffer =
        SDL_AcquireGPUCommandBuffer(device);

    if (!commandBuffer)
    {
        SDL_Log(
            "Failed to acquire command buffer: %s",
            SDL_GetError());

        SDL_ReleaseGPUTexture(device, textureArray);
        return nullptr;
    }

    SDL_GPUCopyPass *copyPass =
        SDL_BeginGPUCopyPass(commandBuffer);

    if (!copyPass)
    {
        SDL_Log(
            "Failed to begin texture array copy pass: %s",
            SDL_GetError());

        SDL_SubmitGPUCommandBuffer(commandBuffer);
        SDL_ReleaseGPUTexture(device, textureArray);
        return nullptr;
    }

    if (textures.empty())
    {
        SDL_GPUTextureLocation source{
            .texture = fallbackTexture,
            .mip_level = 0,
        };

        SDL_GPUTextureLocation destination{
            .texture = textureArray,
            .mip_level = 0,
        };

        SDL_CopyGPUTextureToTexture(
            copyPass,
            &source,
            &destination,
            width,
            height,
            1,
            false);
    }
    else
    {
        for (Uint32 i = 0; i < layerCount; ++i)
        {
            SDL_GPUTextureLocation source{
                .texture = textures[i],
                .mip_level = 0,
                .layer = i};

            SDL_GPUTextureLocation destination{
                .texture = textureArray,
                .mip_level = 0,
                .layer = i};

            SDL_CopyGPUTextureToTexture(
                copyPass,
                &source,
                &destination,
                width,
                height,
                1,
                false);
        }
    }

    SDL_EndGPUCopyPass(copyPass);
    SDL_SubmitGPUCommandBuffer(commandBuffer);

    return textureArray;
}

std::vector<Sphere_GPU> BuildInitialScene2()
{
    Texture *tex = new Texture(device, "brick/textures/red_brick_diff_4k.jpg");
    CreateSphere(point3(0.0f, -700.0f, 0.0f), 700.0f, Material(Color(.5f, .5f, .5f), 0.0f, 0.5f, LAMBERTIAN, nullptr));

    CreateSphere(point3(0.0f, 1.2f, 0.0f), 1.0f, Material(Color(.5f, .1f, .0f), 0.0f, 1.5f, DIAELECTRIC, nullptr));

    CreateSphere(point3(-4.0f, 0.5f, 0.0f), 1.0f, Material(Color(.4f, .2f, .1f), 0.0f, 1.5f, LAMBERTIAN, nullptr));

    CreateSphere(point3(1.0f, 0.5f, 0.0f), 1.0f, Material(Color(.7f, .5f, .4f), 0.0f, 1.5f, METAL, nullptr));

    return spheres;
}

// Builds the Ray Tracing In One Weekend "final scene" - same random
// layout as before, just returned as a vector instead of being uploaded
// directly. This is the *rest* position each small sphere bobs around
// once StepPhysics starts moving them.
std::vector<Sphere_GPU> BuildInitialScene()
{
    Texture *tex = new Texture(device, "brick/textures/red_brick_diff_4k.jpg");
    CreateSphere(point3(0.0f, -1000.0f, 0.0f), 1000.0f, Material(Color(.5f, .5f, .5f), 0.0f, 0.5f, LAMBERTIAN, nullptr));

    CreateSphere(point3(0.0f, 1.0f, 0.0f), 1.0f, Material(Color(.5f, .1f, .0f), 0.0f, 1.5f, DIAELECTRIC, nullptr));

    CreateSphere(point3(-4.0f, 1.0f, 0.0f), 1.0f, Material(Color(.4f, .2f, .1f), 0.0f, 0.0f, LAMBERTIAN, tex));

    CreateSphere(point3(4.0f, 1.0f, 0.0f), 1.0f, Material(Color(.7f, .6f, .5f), 0.0f, 0.0f, METAL, nullptr));

    SDL_srand(0);
    for (int a = -11; a < 11; a++)
        for (int b = -11; b < 11; b++)
        {
            const int i = 4 + (a + 11) * 22 + b + 11;
            const float material = SDL_randf();
            point3 position(a + 0.9f * SDL_randf(), 0.2f, b + 0.9f * SDL_randf());
            Material mat(Color(SDL_randf(), SDL_randf(), SDL_randf()), 0.0f, 0.0f, LAMBERTIAN, nullptr);
            if (material < 0.8f)
            {
                mat.type = LAMBERTIAN;
                mat.fuzz = 0.0f;
                mat.refraction = 0.0f;
            }
            else if (material < 0.95f)
            {
                mat.type = METAL;
                mat.fuzz = SDL_randf() * 0.2f;
                mat.refraction = 0.0f;
            }
            else
            {
                mat.type = DIAELECTRIC;
                mat.color = Color(0.0f, 0.0f, 0.0f);
                mat.fuzz = 0.0f;
                mat.refraction = 2.5f;
            }
            CreateSphere(position, 0.2f, mat);
        }
    return spheres;
}

// Placeholder motion so the per-frame BVH rebuild has something to
// rebuild around - replace this with your actual physics step. It bobs
// each small sphere vertically and records its previous y into y2, so
// SphereBounds() in bvh.h produces a correct swept AABB for the frame.
// `spheres` is mutated in place and must persist across frames (its
// current y becomes next frame's "previous position").
void StepPhysics(std::vector<Sphere_GPU> &spheres, const std::vector<Sphere_GPU> &restPose, float time)
{
    (void)spheres;
    (void)restPose;
    (void)time;
    // for (size_t i = 4; i < spheres.size(); i++)
    // {
    //     const float prevY = spheres[i].y;
    //     spheres[i].y = restPose[i].y + 0.15f * SDL_sinf(time * 2.0f + static_cast<float>(i) * 0.37f);
    //     spheres[i].y2 = prevY;
    // }
}
// Allocates GPU-resident storage buffers plus matching upload transfer
// buffers, sized for the worst case so no reallocation is needed as the
// BVH shape changes frame to frame.
SceneBuffers CreateSceneBuffers(SDL_GPUDevice *device, Uint32 maxSpheres, Uint32 maxNodes)
{
    SceneBuffers sb{};

    SDL_GPUBufferCreateInfo sphereInfo{
        .usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
        .size = static_cast<Uint32>(maxSpheres * sizeof(Sphere_GPU))};
    SDL_GPUBufferCreateInfo bvhInfo{
        .usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
        .size = static_cast<Uint32>(maxNodes * sizeof(BVHNode_GPU))};
    sb.sphereBuffer = SDL_CreateGPUBuffer(device, &sphereInfo);
    sb.bvhBuffer = SDL_CreateGPUBuffer(device, &bvhInfo);

    SDL_GPUTransferBufferCreateInfo sphereTransferInfo{
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size = static_cast<Uint32>(maxSpheres * sizeof(Sphere_GPU))};
    SDL_GPUTransferBufferCreateInfo bvhTransferInfo{
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size = static_cast<Uint32>(maxNodes * sizeof(BVHNode_GPU))};
    sb.sphereTransfer = SDL_CreateGPUTransferBuffer(device, &sphereTransferInfo);
    sb.bvhTransfer = SDL_CreateGPUTransferBuffer(device, &bvhTransferInfo);

    if (!sb.sphereBuffer || !sb.bvhBuffer || !sb.sphereTransfer || !sb.bvhTransfer)
    {
        SDL_Log("Failed to create scene buffers: %s", SDL_GetError());
    }
    return sb;
}

// Re-maps the persistent transfer buffers and re-uploads them into the
// persistent GPU buffers. `cycle = true` lets SDL_gpu double-buffer this
// resource internally instead of stalling on the previous frame's usage.
bool UploadScene(
    SceneBuffers &sb,
    const std::vector<Sphere_GPU> &spheres,
    const std::vector<BVHNode_GPU> &nodes)
{
    void *sphereData = SDL_MapGPUTransferBuffer(device, sb.sphereTransfer, true);
    void *bvhData = SDL_MapGPUTransferBuffer(device, sb.bvhTransfer, true);
    if (!sphereData || !bvhData)
    {
        SDL_Log("Failed to map scene transfer buffers: %s", SDL_GetError());
        return false;
    }
    SDL_memcpy(sphereData, spheres.data(), spheres.size() * sizeof(Sphere_GPU));
    SDL_memcpy(bvhData, nodes.data(), nodes.size() * sizeof(BVHNode_GPU));
    SDL_UnmapGPUTransferBuffer(device, sb.sphereTransfer);
    SDL_UnmapGPUTransferBuffer(device, sb.bvhTransfer);

    SDL_GPUCommandBuffer *command_buffer = SDL_AcquireGPUCommandBuffer(device);
    if (!command_buffer)
    {
        SDL_Log("Failed to acquire command buffer: %s", SDL_GetError());
        return false;
    }
    SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(command_buffer);
    if (!copy_pass)
    {
        SDL_Log("Failed to begin copy pass: %s", SDL_GetError());
        return false;
    }

    SDL_GPUTransferBufferLocation sphereSrc{.transfer_buffer = sb.sphereTransfer};
    SDL_GPUBufferRegion sphereDst{
        .buffer = sb.sphereBuffer,
        .size = static_cast<Uint32>(spheres.size() * sizeof(Sphere_GPU))};
    SDL_UploadToGPUBuffer(copy_pass, &sphereSrc, &sphereDst, true);

    SDL_GPUTransferBufferLocation bvhSrc{.transfer_buffer = sb.bvhTransfer};
    SDL_GPUBufferRegion bvhDst{
        .buffer = sb.bvhBuffer,
        .size = static_cast<Uint32>(nodes.size() * sizeof(BVHNode_GPU))};
    SDL_UploadToGPUBuffer(copy_pass, &bvhSrc, &bvhDst, true);

    SDL_EndGPUCopyPass(copy_pass);
    SDL_SubmitGPUCommandBuffer(command_buffer);
    return true;
}

int main()
{
    Config config = {
        .source_x = 13.0f,
        .source_y = 2.0f,
        .source_z = 3.0f,
        .fov = 15.0f,
        .target_x = 0.0f,
        .target_y = 0.0f,
        .target_z = -1.0f,
        .focus_dist = 20.0f,
        .defocus_angle = 0.6f,
        .up_x = 0.0f,
        .up_y = 1.0f,
        .up_z = 0.0f,
        .oof = 0.6f,
        .sky_r = 0.5f,
        .sky_g = 0.7f,
        .sky_b = 1.0f,
        .width = 1980,
        .horizon_r = 1.0f,
        .horizon_g = 1.0f,
        .horizon_b = 1.0f,
        .height = 1080,
        .samples = 1, // samples per frame - lower this if the frame rate is too low
        .batches = 1, // fixed: each frame is its own accumulation (see note below)
        .batch = 0,   // fixed at 0 - Batch>0 in the shader means "blend with last frame",
                      // which would ghost since both the scene and camera move every frame
        .depth = 5,
        .num_spheres = SCENE_SPHERES};

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        SDL_Log("Failed to initialize SDL: %s", SDL_GetError());
        return 1;
    }
    SDL_Window *window = SDL_CreateWindow("", config.width, config.height, 0);
    if (!window)
    {
        SDL_Log("Failed to create window: %s", SDL_GetError());
        return 1;
    }
    device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL, false, nullptr);
    if (!device)
    {
        SDL_Log("Failed to create GPU device: %s", SDL_GetError());
        return 1;
    }

    SDL_Log("Backend: %s", SDL_GetGPUDeviceDriver(device));

    Texture *defaultWhiteTexturePtr = Texture::CreateSolidColor(device, 255, 255, 255, 255);
    if (!defaultWhiteTexturePtr->gpuTexture)
    {
        SDL_Log("Failed to create default white texture");
        return 1;
    }
    SDL_GPUTexture *defaultWhiteTexture = defaultWhiteTexturePtr->gpuTexture;

    if (!SDL_ClaimWindowForGPUDevice(device, window))
    {
        SDL_Log("Failed to claim window for GPU device: %s", SDL_GetError());
        return 1;
    }
    SDL_RaiseWindow(window);
    SDL_GPUComputePipeline *pipeline = CreateComputePipeline(device);
    if (!pipeline)
    {
        SDL_Log("Failed to create compute pipeline.");
        return 1;
    }

    SDL_GPUSamplerCreateInfo samplerInfo{};
    samplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    SDL_GPUSampler *linearSampler = SDL_CreateGPUSampler(device, &samplerInfo);
    if (!linearSampler)
    {
        SDL_Log("Failed to create sampler: %s", SDL_GetError());
        return 1;
    }

    SDL_GPUTextureCreateInfo textureInfo{
        .usage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_TEXTUREUSAGE_SAMPLER,
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT,
        .width = config.width,
        .height = config.height,
        .num_levels = 1,
        .layer_count_or_depth = 1};
    SDL_GPUTexture *texture1 = SDL_CreateGPUTexture(device, &textureInfo);
    if (!texture1)
    {
        SDL_Log("Failed to create texture: %s", SDL_GetError());
        return 1;
    }

    // Rest pose (immutable) and the live, physics-mutated copy.
    const std::vector<Sphere_GPU> restPose = BuildInitialScene();
    std::vector<Sphere_GPU> liveSpheres = restPose;

    std::vector<BVHNode_GPU> nodes;
    std::vector<Sphere_GPU> orderedSpheres;

    // Build the GPU texture array from all textures used by the scene.
    std::vector<SDL_GPUTexture *> textureHandles;

    for (Texture *texture : textures)
    {
        if (texture != nullptr && texture->gpuTexture != nullptr)
        {
            textureHandles.push_back(texture->gpuTexture);
        }
    }

    Uint32 textureWidth = 1;
    Uint32 textureHeight = 1;

    if (!textures.empty())
    {
        textureWidth = static_cast<Uint32>(textures[0]->Width());
        textureHeight = static_cast<Uint32>(textures[0]->Height());
    }

    SDL_GPUTexture *globalTextureArray =
        CreateTextureArray(
            device,
            textureHandles,
            defaultWhiteTexture,
            textureWidth,
            textureHeight);

    if (!globalTextureArray)
    {
        SDL_Log("Failed to create global texture array");
        return 1;
    }

    SceneBuffers sceneBuffers = CreateSceneBuffers(device, MAX_SPHERES, MAX_NODES);
    if (!sceneBuffers.sphereBuffer || !sceneBuffers.bvhBuffer)
    {
        SDL_Log("Failed to create scene buffers");
        return 1;
    }

    // --- Camera setup: derive an initial yaw/pitch that reproduces the
    // original fixed Source/Target framing, then hand control over to
    // mouse-look + WASD from here on.
    const Vec3<float> initialForward = normalize(
        Vec3<float>{config.target_x, config.target_y, config.target_z} -
        Vec3<float>{config.source_x, config.source_y, config.source_z});
    Camera camera(
        Vec3<float>{config.source_x, config.source_y, config.source_z},
        SDL_atan2f(initialForward.z, initialForward.x),
        SDL_asinf(initialForward.y));

    bool mouseCaptured = true;
    SDL_SetWindowRelativeMouseMode(window, true);

    Uint32 accumulationFrame = 0;
    bool fpsCapEnabled = true;
    const double targetFrameSeconds = 1.0 / TARGET_FPS;

    const Uint64 perfFreq = SDL_GetPerformanceFrequency();
    Uint64 lastCounter = SDL_GetPerformanceCounter();
    double elapsedTime = 0.0;
    double fpsAccum = 0.0;
    int fpsFrameCount = 0;
    double currentFps = 0.0;

    bool running = true;

    while (running)
    {
        const Uint64 frameStart = SDL_GetPerformanceCounter();
        const float dt = static_cast<float>((frameStart - lastCounter) / static_cast<double>(perfFreq));
        lastCounter = frameStart;
        elapsedTime += dt;

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

        Vec3<float> oldCameraPosition = camera.position;
        float oldYaw = camera.yaw;
        float oldPitch = camera.pitch;
        if (mouseCaptured)
        {
            camera.Update(keys, mouseDX, mouseDY, dt);
        }

        const Vec3<float> forward = camera.Forward();
        config.source_x = camera.position.x;
        config.source_y = camera.position.y;
        config.source_z = camera.position.z;
        config.target_x = camera.position.x + forward.x;
        config.target_y = camera.position.y + forward.y;
        config.target_z = camera.position.z + forward.z;
        config.focus_dist = camera.focus_dist;
        config.defocus_angle = camera.defocus_angle;
        config.fov = camera.fov;
        // up_x/up_y/up_z stay fixed at (0,1,0) - this is a roll-free FPS camera.

        // Did the camera moved
        bool cameraMoved =
            camera.position.x != oldCameraPosition.x ||
            camera.position.y != oldCameraPosition.y ||
            camera.position.z != oldCameraPosition.z ||
            camera.yaw != oldYaw ||
            camera.pitch != oldPitch;

        // Reset accumulation
        if (cameraMoved)
        {
            accumulationFrame = 0;
        }

        config.batch = accumulationFrame;

        // 2. Step motion/physics.
        StepPhysics(liveSpheres, restPose, static_cast<float>(elapsedTime));

        // 3. Rebuild the BVH around the new positions.
        BuildBVH(liveSpheres, nodes, orderedSpheres);
        SDL_Log(
            "BVH: %zu nodes, %zu ordered spheres",
            nodes.size(),
            orderedSpheres.size());

        for (size_t i = 0; i < nodes.size(); ++i)
        {
            const auto &n = nodes[i];

            SDL_Log(
                "Node %zu: min=(%.2f, %.2f, %.2f) "
                "max=(%.2f, %.2f, %.2f)",
                i,
                n.min_x,
                n.min_y,
                n.min_z,
                n.max_x,
                n.max_y,
                n.max_z);
        }
        config.num_spheres = static_cast<Uint32>(orderedSpheres.size());

        // 4. Upload the reordered spheres + flattened nodes.
        if (!UploadScene(sceneBuffers, orderedSpheres, nodes))
        {
            SDL_Log("Failed to upload scene");
            break;
        }

        SDL_GPUCommandBuffer *command_buffer = SDL_AcquireGPUCommandBuffer(device);
        if (!command_buffer)
        {
            SDL_Log("Failed to acquire command buffer: %s", SDL_GetError());
            continue;
        }

        SDL_PushGPUComputeUniformData(command_buffer, 0, &config, sizeof(config));
        SDL_GPUStorageTextureReadWriteBinding storageTextureBinding = {
            .texture = texture1};
        SDL_GPUComputePass *compute_pass = SDL_BeginGPUComputePass(command_buffer, &storageTextureBinding, 1, nullptr, 0);
        if (!compute_pass)
        {
            SDL_Log("Failed to begin compute pass: %s", SDL_GetError());
            return 1;
        }
        SDL_BindGPUComputePipeline(compute_pass, pipeline);
        SDL_GPUBuffer *storageBuffers[2] = {sceneBuffers.sphereBuffer, sceneBuffers.bvhBuffer};
        SDL_BindGPUComputeStorageBuffers(compute_pass, 0, storageBuffers, 2);

        SDL_GPUTextureSamplerBinding textureBinding{
            .texture = globalTextureArray,
            .sampler = linearSampler};

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
        SDL_WaitForGPUSwapchain(device, window);
        if (!SDL_AcquireGPUSwapchainTexture(command_buffer, window, &swapchain, &width, &height))
        {
            SDL_Log("Failed to acquire swapchain texture: %s", SDL_GetError());
            SDL_SubmitGPUCommandBuffer(command_buffer);
            continue;
        }
        if (!swapchain)
        {
            SDL_SubmitGPUCommandBuffer(command_buffer);
            continue;
        }
        SDL_GPUBlitInfo blit = {
            .source.texture = texture1,
            .source.w = config.width,
            .source.h = config.height,
            .destination.texture = swapchain,
            .destination.w = width,
            .destination.h = height};
        SDL_BlitGPUTexture(command_buffer, &blit);
        SDL_SubmitGPUCommandBuffer(command_buffer);

        // 5. FPS bookkeeping - update the readout twice a second so it's
        // readable instead of flickering every frame.
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
                     "%s | %.0f fps (%s, press V) | %u nodes / %u spheres | mouse: %s (Esc)",
                     SDL_GetGPUDeviceDriver(device),
                     currentFps,
                     fpsCapEnabled ? "capped" : "uncapped",
                     static_cast<Uint32>(nodes.size()),
                     static_cast<Uint32>(orderedSpheres.size()),
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

    SDL_ReleaseGPUTexture(device, texture1);
    SDL_ReleaseGPUTexture(device, globalTextureArray);

    SDL_ReleaseGPUBuffer(device, sceneBuffers.sphereBuffer);
    SDL_ReleaseGPUBuffer(device, sceneBuffers.bvhBuffer);

    SDL_ReleaseGPUTransferBuffer(device, sceneBuffers.sphereTransfer);
    SDL_ReleaseGPUTransferBuffer(device, sceneBuffers.bvhTransfer);

    SDL_ReleaseGPUComputePipeline(device, pipeline);
    SDL_ReleaseGPUSampler(device, linearSampler);
    SDL_ReleaseWindowFromGPUDevice(device, window);
    SDL_DestroyGPUDevice(device);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}