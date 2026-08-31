#include "renderer.h"

int Renderer::Initialize()
{
    // Render settings. Camera framing and sky come from the scene itself and
    // are filled in below, once it's built.
    config = {};
    config.up_x = 0.0f;
    config.up_y = 1.0f;
    config.up_z = 0.0f;
    config.oof = 0.6f;
    config.width = 1980;
    config.height = 1080;
    config.samples = 1; // samples per frame - lower this if the frame rate is too low
    config.batches = 1; // each frame is one accumulation step
    config.batch = 0;   // Batch > 0 blends with the previous frame; driven by
                        // accumulationFrame below and reset when the camera moves

    config.renderType = RenderType::Path_Tracing; // Type of rendering (Path tracing or Ray Tracing)

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        SDL_Log("Failed to initialize SDL: %s", SDL_GetError());
        return;
    }
    window = SDL_CreateWindow("", config.width, config.height, 0);
    if (!window)
    {
        SDL_Log("Failed to create window: %s", SDL_GetError());
        return;
    }
    device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL, true, nullptr);
    if (!device)
    {
        SDL_Log("Failed to create GPU device: %s", SDL_GetError());
        return;
    }

    SDL_Log("Backend: %s", SDL_GetGPUDeviceDriver(device));

    if (!SDL_ClaimWindowForGPUDevice(device, window))
    {
        SDL_Log("Failed to claim window for GPU device: %s", SDL_GetError());
        return;
    }
    SDL_RaiseWindow(window);
    SDL_GPUComputePipeline *pipeline = CreatePathTraceComputePipeline(device);
    if (!pipeline)
    {
        SDL_Log("Failed to create compute pipeline.");
        return;
    }

    SDL_GPUSamplerCreateInfo samplerInfo{};
    samplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    linearSampler = SDL_CreateGPUSampler(device, &samplerInfo);

    if (!linearSampler)
    {
        SDL_Log("Failed to create sampler: %s", SDL_GetError());
        return;
    }

    SDL_GPUTextureCreateInfo textureInfo = {};
    textureInfo.type = SDL_GPU_TEXTURETYPE_2D;
    textureInfo.width = config.width;
    textureInfo.height = config.height;
    textureInfo.layer_count_or_depth = 1;
    textureInfo.num_levels = 1;
    // R16G16B16A16 instead of R32G32B32A32: halves the bandwidth on this
    // texture, which now matters more than before since the shader reads
    // its previous contents back every frame to blend for temporal
    // accumulation, not just writing it once.
    textureInfo.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    textureInfo.usage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_TEXTUREUSAGE_SAMPLER;

    texture1 = SDL_CreateGPUTexture(device, &textureInfo);
    if (!texture1)
    {
        SDL_Log("Failed to create texture: %s", SDL_GetError());
        return;
    }

    // The scene owns both the geometry and its textures, so ids are already
    // assigned by the time the objects exist - no separate registration pass.
    // Swapping scenes is a one-liner: each builder sets up its own geometry,
    // textures, camera framing and sky.

    config.sky_r = world.camera.sky.x;
    config.sky_g = world.camera.sky.y;
    config.sky_b = world.camera.sky.z;
    config.horizon_r = world.camera.horizon.x;
    config.horizon_g = world.camera.horizon.y;
    config.horizon_b = world.camera.horizon.z;
    config.depth = world.maxDepth;

    // The scene's own objects are the immutable rest pose; physics mutates the
    // clone, and each frame's BVH is built over the clone.
    std::vector<std::unique_ptr<Object>> liveObjects = world.GetObjects();

    std::vector<BVH_node *> nodes;
    std::vector<Object *> orderedObjects;
    std::vector<uint32_t> lightIDs;

    globalTextureArray = world.textures.BuildGPUArray(device);
    if (!globalTextureArray)
    {
        SDL_Log("Failed to build the scene texture array");
        return;
    }

    // Sized from the scene that was actually built, so adding geometry can't
    // quietly overrun a buffer dimensioned for some other scene.
    const Uint32 maxObjects = static_cast<Uint32>(scene.Count());
    const Uint32 maxLights = CountLights(liveObjects);
    sceneBuffers = CreateSceneBuffers(device, maxObjects, MaxNodesFor(maxObjects), maxLights);
    if (!sceneBuffers.objectBuffer || !sceneBuffers.bvhBuffer)
    {
        SDL_Log("Failed to create scene buffers");
        return;
    }

    // --- Camera setup: derive an initial yaw/pitch that reproduces the scene's
    // authored framing, then hand control over to mouse-look + WASD from here on.
    const Vec3<float> initialForward =
        normalize(world.camera.target - world.camera.position);

    world.camera.yaw = SDL_atan2f(initialForward.z, initialForward.x);
    world.camera.pitch = SDL_asinf(initialForward.y);
    world.camera.fov;
    world.camera.focus_dist;
    world.camera.defocus_angle;

    bool mouseCaptured = true;
    SDL_SetWindowRelativeMouseMode(world, true);

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
    size_t loggedNodeCount = static_cast<size_t>(-1);

    bool sceneDirty = true; // becomes true again once StepPhysics does real work

    BuildBVH(liveObjects, nodes, orderedObjects);
    lightIDs = LightIndices(orderedObjects);
    config.num_spheres = static_cast<Uint32>(orderedObjects.size());
    config.num_lights = static_cast<Uint32>(lightIDs.size());
    UploadScene(sceneBuffers, ObjectsToGPUObjects(orderedObjects), NodesToGPUNodes(nodes), lightIDs);
}

void Renderer::Render(float deltaTime, float currentTime, entt::registry &registry)
{
    

    UploadScene

}

SDL_GPUComputePipeline *Renderer::CreatePathTraceComputePipeline(SDL_GPUDevice *device)
{
    SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);
    const char *path;
    const char *entrypoint;
    SDL_GPUShaderFormat format;
    if (formats & SDL_GPU_SHADERFORMAT_SPIRV)
    {
        path = "path_trace.comp.spv";
        entrypoint = "main";
        format = SDL_GPU_SHADERFORMAT_SPIRV;
        SDL_Log("This computer supports SPIRV");
    }
    else if (formats & SDL_GPU_SHADERFORMAT_DXIL)
    {
        path = "path_trace.comp.dxil";
        entrypoint = "main";
        format = SDL_GPU_SHADERFORMAT_DXIL;

        SDL_Log("This computer supports DXIL");
    }
    else if (formats & SDL_GPU_SHADERFORMAT_MSL)
    {
        path = "path_trace.comp.msl";
        entrypoint = "main0";
        format = SDL_GPU_SHADERFORMAT_MSL;

        SDL_Log("This computer supports MSL");
    }
    else
    {
        SDL_Log("No supported shader format");
        return nullptr;
    }
    size_t size;
    void *data = SDL_LoadFile(path, &size);
    const char *basePath = nullptr;
    if (!data)
    {
        basePath = SDL_GetBasePath();
        if (basePath)
        {
            std::string baseShaderPath = std::string(basePath) + path;
            data = SDL_LoadFile(baseShaderPath.c_str(), &size);
        }
    }
    if (!data)
    {
        SDL_Log("Failed to load shader: %s", SDL_GetError());
        return nullptr;
    }
    SDL_Log("Loading compute shader: %s (%zu bytes)", path, size);
    SDL_GPUComputePipelineCreateInfo cpci = {0};
    cpci.code = static_cast<Uint8 *>(data);
    cpci.code_size = size;
    cpci.entrypoint = entrypoint;
    cpci.format = format;
    cpci.num_samplers = 1;
    cpci.num_readonly_storage_buffers = 2; // objects (t1) + BVH nodes (t2)
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

SDL_GPUComputePipeline *Renderer::CreateRayTraceComputePipeline(SDL_GPUDevice *device)
{
    SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);
    const char *path;
    const char *entrypoint;
    SDL_GPUShaderFormat format;
    if (formats & SDL_GPU_SHADERFORMAT_SPIRV)
    {
        path = "ray_trace.comp.spv";
        entrypoint = "main";
        format = SDL_GPU_SHADERFORMAT_SPIRV;
        SDL_Log("This computer supports SPIRV");
    }
    else if (formats & SDL_GPU_SHADERFORMAT_DXIL)
    {
        path = "ray_trace.comp.dxil";
        entrypoint = "main";
        format = SDL_GPU_SHADERFORMAT_DXIL;

        SDL_Log("This computer supports DXIL");
    }
    else if (formats & SDL_GPU_SHADERFORMAT_MSL)
    {
        path = "ray_trace.comp.msl";
        entrypoint = "main0";
        format = SDL_GPU_SHADERFORMAT_MSL;

        SDL_Log("This computer supports MSL");
    }
    else
    {
        SDL_Log("No supported shader format");
        return nullptr;
    }
    size_t size;
    void *data = SDL_LoadFile(path, &size);
    const char *basePath = nullptr;
    if (!data)
    {
        basePath = SDL_GetBasePath();
        if (basePath)
        {
            std::string baseShaderPath = std::string(basePath) + path;
            data = SDL_LoadFile(baseShaderPath.c_str(), &size);
        }
    }
    if (!data)
    {
        SDL_Log("Failed to load shader: %s", SDL_GetError());
        return nullptr;
    }
    SDL_Log("Loading compute shader: %s (%zu bytes)", path, size);
    SDL_GPUComputePipelineCreateInfo cpci = {0};
    cpci.code = static_cast<Uint8 *>(data);
    cpci.code_size = size;
    cpci.entrypoint = entrypoint;
    cpci.format = format;
    cpci.num_samplers = 1;
    cpci.num_readonly_storage_buffers = 3; // objects (t1) + BVH nodes (t2) + light IDs
    cpci.num_readwrite_storage_textures = 1;
    cpci.num_uniform_buffers = 1;
    cpci.threadcount_x = THREADS;
    cpci.threadcount_y = 1;
    cpci.threadcount_z = 1;
    pipeline = SDL_CreateGPUComputePipeline(device, &cpci);

    SDL_free(data);
    if (!pipeline)
    {
        SDL_Log("Failed to create compute pipeline: %s", SDL_GetError());
        return nullptr;
    }
    return pipeline;
}

std::vector<uint32_t> Renderer::LightIndices(const std::vector<Object *> &orderedObjects)
{
    std::vector<uint32_t> lights;
    for (uint32_t i = 0; i < orderedObjects.size(); i++)
        if (orderedObjects[i]->mat.type == MaterialType::DiffuseLight)
            lights.push_back(i);
    return lights;
}

// Allocates GPU-resident storage buffers plus matching upload transfer
// buffers, sized for the worst case so no reallocation is needed as the
// BVH shape changes frame to frame.
SceneBuffers Renderer::CreateSceneBuffers(SDL_GPUDevice *device, Uint32 maxObjects, Uint32 maxNodes, Uint32 maxLights)
{
    SceneBuffers sb{};
    sb.maxObjects = maxObjects;
    sb.maxNodes = maxNodes;
    sb.maxLights = maxLights;

    SDL_Log("chat");

    SDL_GPUBufferCreateInfo objectInfo{};
    objectInfo.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
    objectInfo.size = static_cast<Uint32>(maxObjects * sizeof(Object_GPU));
    SDL_GPUBufferCreateInfo bvhInfo{};
    bvhInfo.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
    bvhInfo.size = static_cast<Uint32>(maxNodes * sizeof(BVHNode_GPU));

    SDL_GPUBufferCreateInfo lightInfo{};
    lightInfo.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
    lightInfo.size = static_cast<Uint32>(maxLights * sizeof(uint32_t));

    sb.objectBuffer = SDL_CreateGPUBuffer(device, &objectInfo);
    sb.bvhBuffer = SDL_CreateGPUBuffer(device, &bvhInfo);
    sb.lightIDBuffer = SDL_CreateGPUBuffer(device, &lightInfo);

    SDL_GPUTransferBufferCreateInfo objectTransferInfo{};
    objectTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    objectTransferInfo.size = static_cast<Uint32>(maxObjects * sizeof(Object_GPU));
    SDL_GPUTransferBufferCreateInfo bvhTransferInfo{};
    bvhTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    bvhTransferInfo.size = static_cast<Uint32>(maxNodes * sizeof(BVHNode_GPU));
    SDL_GPUTransferBufferCreateInfo lightIDTransferInfo{};
    lightIDTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    lightIDTransferInfo.size = static_cast<Uint32>(maxLights * sizeof(uint32_t));

    sb.objectTransfer = SDL_CreateGPUTransferBuffer(device, &objectTransferInfo);
    sb.bvhTransfer = SDL_CreateGPUTransferBuffer(device, &bvhTransferInfo);
    sb.lightIDTransfer = SDL_CreateGPUTransferBuffer(device, &lightIDTransferInfo);

    if (!sb.objectTransfer || !sb.bvhBuffer || !sb.lightIDBuffer || !sb.objectTransfer || !sb.bvhTransfer || !sb.lightIDTransfer)
    {
        SDL_Log("Failed to create scene buffers: %s", SDL_GetError());
    }
    return sb;
}

std::vector<Object_GPU> Renderer::ObjectsToGPUObjects(const std::vector<Object *> &objects)
{
    std::vector<Object_GPU> list;
    list.reserve(objects.size());

    for (Object *obj : objects)
    {
        list.push_back(obj->CreateObjectGPU());
    }

    return list;
}

std::vector<BVHNode_GPU> Renderer::NodesToGPUNodes(const std::vector<BVH_node *> &nodes)
{
    std::vector<BVHNode_GPU> list;
    list.reserve(nodes.size());

    for (BVH_node *obj : nodes)
    {
        BVHNode_GPU obj_gpu = obj->CreateBVHNodeGPU();
        list.push_back(obj_gpu);
    }

    return list;
}

// Re-maps the persistent transfer buffers and re-uploads them into the
// persistent GPU buffers. `cycle = true` lets SDL_gpu double-buffer this
// resource internally instead of stalling on the previous frame's usage.
bool Renderer::UploadScene(
    SceneBuffers &sb,
    const std::vector<Object_GPU> &objects,
    const std::vector<BVHNode_GPU> &nodes,
    const std::vector<uint32_t> &lightIDs)
{
    // These buffers are fixed-size; without this check an oversized scene would
    // memcpy straight past the end of the mapped transfer buffer.
    if (objects.size() > sb.maxObjects || nodes.size() > sb.maxNodes || lightIDs.size() > sb.maxLights)
    {
        SDL_Log(
            "Scene too large for its buffers: %zu/%u objects, %zu/%u nodes",
            objects.size(), sb.maxObjects, nodes.size(), sb.maxNodes);
        return false;
    }

    void *objectData = SDL_MapGPUTransferBuffer(device, sb.objectTransfer, true);
    void *bvhData = SDL_MapGPUTransferBuffer(device, sb.bvhTransfer, true);
    void *lightIDData = SDL_MapGPUTransferBuffer(device, sb.lightIDTransfer, true);

    if (!objectData || !bvhData || !lightIDData)
    {
        SDL_Log("Failed to map scene transfer buffers: %s", SDL_GetError());
        return false;
    }
    SDL_memcpy(objectData, objects.data(), objects.size() * sizeof(Object_GPU));
    SDL_memcpy(bvhData, nodes.data(), nodes.size() * sizeof(BVHNode_GPU));
    SDL_memcpy(lightIDData, lightIDs.data(), lightIDs.size() * sizeof(uint32_t));
    SDL_UnmapGPUTransferBuffer(device, sb.objectTransfer);
    SDL_UnmapGPUTransferBuffer(device, sb.bvhTransfer);
    SDL_UnmapGPUTransferBuffer(device, sb.lightIDTransfer);

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

    SDL_GPUTransferBufferLocation objectSrc{};
    objectSrc.transfer_buffer = sb.objectTransfer;
    SDL_GPUBufferRegion objectDst{};
    objectDst.buffer = sb.objectBuffer;
    objectDst.size = static_cast<Uint32>(objects.size() * sizeof(Object_GPU));
    SDL_UploadToGPUBuffer(copy_pass, &objectSrc, &objectDst, true);

    SDL_GPUTransferBufferLocation bvhSrc{};
    bvhSrc.transfer_buffer = sb.bvhTransfer;
    SDL_GPUBufferRegion bvhDst{};
    bvhDst.buffer = sb.bvhBuffer;
    bvhDst.size = static_cast<Uint32>(nodes.size() * sizeof(BVHNode_GPU));
    SDL_UploadToGPUBuffer(copy_pass, &bvhSrc, &bvhDst, true);

    SDL_GPUTransferBufferLocation lightIDSrc{};
    lightIDSrc.transfer_buffer = sb.lightIDTransfer;
    SDL_GPUBufferRegion lightIDDst{};
    lightIDDst.buffer = sb.lightIDBuffer;
    lightIDDst.size = static_cast<Uint32>(lightIDs.size() * sizeof(uint32_t));
    SDL_UploadToGPUBuffer(copy_pass, &lightIDSrc, &lightIDDst, true);

    SDL_EndGPUCopyPass(copy_pass);
    if (!SDL_SubmitGPUCommandBuffer(command_buffer))
    {
        SDL_Log("Failed to submit scene upload: %s", SDL_GetError());
        return false;
    }
    return true;
}

uint32_t Renderer::CountLights(const std::vector<std::unique_ptr<Object>> &objects)
{
    return static_cast<uint32_t>(std::count_if(objects.begin(), objects.end(),
                                               [](const auto &o)
                                               { return o->mat.type == MaterialType::DiffuseLight; }));
}

void Renderer::TerminateRenderer()
{
    SDL_ReleaseGPUTexture(device, texture1);
    SDL_ReleaseGPUTexture(device, globalTextureArray);

    SDL_ReleaseGPUBuffer(device, sceneBuffers.objectBuffer);
    SDL_ReleaseGPUBuffer(device, sceneBuffers.bvhBuffer);

    SDL_ReleaseGPUTransferBuffer(device, sceneBuffers.objectTransfer);
    SDL_ReleaseGPUTransferBuffer(device, sceneBuffers.bvhTransfer);

    SDL_ReleaseGPUComputePipeline(device, pipeline);
    SDL_ReleaseGPUSampler(device, linearSampler);
    SDL_ReleaseWindowFromGPUDevice(device, window);
    SDL_DestroyGPUDevice(device);
    SDL_DestroyWindow(window);
}