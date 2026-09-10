#include "renderer/renderer.h"

#include <algorithm>

#include "core/common.h"
#include "scene/components.h"
#include "scene/shapes.h"
#include "systems/cameraSystem.h"

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

SDL_GPUComputePipeline *Renderer::CreateComputePipeline(const char *stem)
{
    const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);

    std::string path;
    const char *entrypoint = "main";
    SDL_GPUShaderFormat format;

    if (formats & SDL_GPU_SHADERFORMAT_SPIRV)
    {
        path = std::string(stem) + ".spv";
        format = SDL_GPU_SHADERFORMAT_SPIRV;
    }
    else if (formats & SDL_GPU_SHADERFORMAT_DXIL)
    {
        path = std::string(stem) + ".dxil";
        format = SDL_GPU_SHADERFORMAT_DXIL;
    }
    else if (formats & SDL_GPU_SHADERFORMAT_MSL)
    {
        path = std::string(stem) + ".msl";
        entrypoint = "main0";
        format = SDL_GPU_SHADERFORMAT_MSL;
    }
    else
    {
        SDL_Log("No supported shader format");
        return nullptr;
    }

    // Try the working directory first, then next to the executable, so the
    // build's staged copy is found however the app was launched.
    size_t size = 0;
    void *data = SDL_LoadFile(path.c_str(), &size);
    if (!data)
    {
        if (const char *basePath = SDL_GetBasePath())
        {
            data = SDL_LoadFile((std::string(basePath) + path).c_str(), &size);
        }
    }
    if (!data)
    {
        SDL_Log("Failed to load shader '%s': %s", path.c_str(), SDL_GetError());
        return nullptr;
    }

    SDL_Log("Loading compute shader: %s (%zu bytes)", path.c_str(), size);

    SDL_GPUComputePipelineCreateInfo cpci = {};
    cpci.code = static_cast<Uint8 *>(data);
    cpci.code_size = size;
    cpci.entrypoint = entrypoint;
    cpci.format = format;
    cpci.num_samplers = 1;
    // objects (t1), BVH nodes (t2), light ids (t3), mesh vertices (t4), mesh
    // indices (t5), BLAS nodes (t6), mesh instances (t7). Getting this count
    // wrong leaves the tail buffers unbound and the shader reads zeroes - which
    // for the light id buffer means every light silently disappears, and for
    // the mesh buffers means every mesh instance collapses to nothing. It must
    // stay in step with the bind array in RenderFrame and the register numbers
    // in the shader.
    cpci.num_readonly_storage_buffers = 7;
    // accumTexture (u0) and displayTexture (u1). Same rule as the storage
    // buffers: this count, the binding array in RenderFrame and the register
    // numbers in the shader all move together.
    cpci.num_readwrite_storage_textures = 2;
    cpci.num_uniform_buffers = 1;
    cpci.threadcount_x = THREADS;
    cpci.threadcount_y = 1;
    cpci.threadcount_z = 1;

    SDL_GPUComputePipeline *created = SDL_CreateGPUComputePipeline(device, &cpci);
    SDL_free(data);

    if (!created)
    {
        SDL_Log("Failed to create compute pipeline: %s", SDL_GetError());
    }
    return created;
}

bool Renderer::Initialize(const RendererSettings &settings)
{
    config.width = settings.width;
    config.height = settings.height;
    config.samples = settings.samples;
    config.batches = 1; // each frame is one accumulation step
    config.batch = 0;
    config.renderType = static_cast<Uint32>(settings.renderType);
    config.up_x = 0.0f;
    config.up_y = 1.0f;
    config.up_z = 0.0f;
    config.oof = 0.6f;
    config.exposure = 0.1f;

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        SDL_Log("Failed to initialize SDL: %s", SDL_GetError());
        return false;
    }

    window = SDL_CreateWindow(settings.title, config.width, config.height, 0);
    if (!window)
    {
        SDL_Log("Failed to create window: %s", SDL_GetError());
        return false;
    }

    // SPIRV and MSL only, deliberately. Adding DXIL here lets SDL pick the
    // D3D12 backend on Windows, and the build only produces .spv - the shaders
    // are authored and validated against Vulkan (the Object_GPU layout asserts
    // are checked with spirv-dis). Add DXIL to both this list and the CMake
    // shader rule together, or not at all.
    device = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL,
        true, nullptr);
    if (!device)
    {
        SDL_Log("Failed to create GPU device: %s", SDL_GetError());
        return false;
    }

    SDL_Log("Backend: %s", SDL_GetGPUDeviceDriver(device));

    if (!SDL_ClaimWindowForGPUDevice(device, window))
    {
        SDL_Log("Failed to claim window for GPU device: %s", SDL_GetError());
        return false;
    }
    SDL_RaiseWindow(window);

    pipeline = CreateComputePipeline(settings.shader);
    if (!pipeline)
    {
        return false;
    }

    SDL_GPUSamplerCreateInfo samplerInfo = {};
    samplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    linearSampler = SDL_CreateGPUSampler(device, &samplerInfo);
    if (!linearSampler)
    {
        SDL_Log("Failed to create sampler: %s", SDL_GetError());
        return false;
    }

    // Must match the storage-image format the shader declares - see the
    // [[vk::image_format("rgba16f")]] on `image` in ray_trace.comp.hlsl. When
    // the two disagree, every load and store to the texture is undefined, and
    // Vulkan validation says so. R16 halves the bandwidth on a texture the
    // shader reads back every frame for temporal accumulation.
    SDL_GPUTextureCreateInfo textureInfo = {};
    textureInfo.type = SDL_GPU_TEXTURETYPE_2D;
    textureInfo.width = config.width;
    textureInfo.height = config.height;
    textureInfo.layer_count_or_depth = 1;
    textureInfo.num_levels = 1;
    textureInfo.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    textureInfo.usage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_TEXTUREUSAGE_SAMPLER;

    // The shader reads image[id] back to blend this frame into the running
    // average, and reading a storage texture it also writes within one compute
    // pass requires SIMULTANEOUS_READ_WRITE - plain WRITE is not enough, and
    // without it the read is undefined (it happened to work on this driver).
    textureInfo.usage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE |
                        SDL_GPU_TEXTUREUSAGE_SAMPLER;

    accumTexture = SDL_CreateGPUTexture(device, &textureInfo);
    if (!accumTexture)
    {
        SDL_Log("Failed to create accumulation texture: %s", SDL_GetError());
        return false;
    }

    // The display target. 8-bit UNORM holding sRGB-ENCODED values: the shader
    // does the encode itself, so this must not be a _SRGB format or the encode
    // would be applied twice. Write-only, so no SIMULTANEOUS needed.
    SDL_GPUTextureCreateInfo displayInfo = textureInfo;
    displayInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    displayInfo.usage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_TEXTUREUSAGE_SAMPLER;

    displayTexture = SDL_CreateGPUTexture(device, &displayInfo);
    if (!displayTexture)
    {
        SDL_Log("Failed to create display texture: %s", SDL_GetError());
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Scene buffers
// ---------------------------------------------------------------------------

bool Renderer::CreateSceneBuffers(Uint32 maxObjectCount, Uint32 maxNodeCount, Uint32 maxLightCount)
{
    // A zero-sized GPU buffer is invalid, and a scene can legitimately have no
    // lights - keep at least one slot so binding stays valid.
    maxObjectCount = std::max(maxObjectCount, 1u);
    maxNodeCount = std::max(maxNodeCount, 1u);
    maxLightCount = std::max(maxLightCount, 1u);

    buffers.maxObjects = maxObjectCount;
    buffers.maxNodes = maxNodeCount;
    buffers.maxLights = maxLightCount;

    // Mesh geometry is sized from the library, which is fully built by the time
    // LoadScene runs. The same one-slot floor applies: a scene with no meshes
    // still has to leave t4..t7 bound to something valid, or the shader reads
    // an unbound buffer.
    const MeshLibrary empty;
    const MeshLibrary &library = meshLibrary ? *meshLibrary : empty;

    buffers.maxVertices = std::max(static_cast<Uint32>(library.Vertices().size()), 1u);
    buffers.maxIndices = std::max(static_cast<Uint32>(library.Indices().size()), 1u);
    buffers.maxBLASNodes = std::max(static_cast<Uint32>(library.BLASNodes().size()), 1u);
    buffers.maxInstances = maxObjectCount; // at most one instance per object

    const struct
    {
        SDL_GPUBuffer **buffer;
        SDL_GPUTransferBuffer **transfer;
        Uint32 size;
    } allocations[] = {
        {&buffers.objectBuffer, &buffers.objectTransfer, maxObjectCount * sizeof(Object_GPU)},
        {&buffers.bvhBuffer, &buffers.bvhTransfer, maxNodeCount * sizeof(BVHNode_GPU)},
        {&buffers.lightIDBuffer, &buffers.lightIDTransfer, maxLightCount * sizeof(uint32_t)},
        {&buffers.vertexBuffer, &buffers.vertexTransfer, buffers.maxVertices * sizeof(Vertex_GPU)},
        {&buffers.indexBuffer, &buffers.indexTransfer, buffers.maxIndices * sizeof(uint32_t)},
        {&buffers.blasBuffer, &buffers.blasTransfer, buffers.maxBLASNodes * sizeof(BVHNode_GPU)},
        {&buffers.instanceBuffer, &buffers.instanceTransfer, buffers.maxInstances * sizeof(MeshInstance_GPU)},
    };

    for (const auto &a : allocations)
    {
        SDL_GPUBufferCreateInfo bufferInfo = {};
        bufferInfo.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
        bufferInfo.size = a.size;
        *a.buffer = SDL_CreateGPUBuffer(device, &bufferInfo);

        SDL_GPUTransferBufferCreateInfo transferInfo = {};
        transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transferInfo.size = a.size;
        *a.transfer = SDL_CreateGPUTransferBuffer(device, &transferInfo);

        if (!*a.buffer || !*a.transfer)
        {
            SDL_Log("Failed to create scene buffers: %s", SDL_GetError());
            return false;
        }
    }

    return true;
}

void Renderer::ReleaseSceneBuffers()
{
    SDL_GPUBuffer *const gpuBuffers[] = {
        buffers.objectBuffer, buffers.bvhBuffer, buffers.lightIDBuffer,
        buffers.vertexBuffer, buffers.indexBuffer, buffers.blasBuffer, buffers.instanceBuffer};
    for (SDL_GPUBuffer *b : gpuBuffers)
    {
        if (b)
            SDL_ReleaseGPUBuffer(device, b);
    }

    SDL_GPUTransferBuffer *const transfers[] = {
        buffers.objectTransfer, buffers.bvhTransfer, buffers.lightIDTransfer,
        buffers.vertexTransfer, buffers.indexTransfer, buffers.blasTransfer, buffers.instanceTransfer};
    for (SDL_GPUTransferBuffer *t : transfers)
    {
        if (t)
            SDL_ReleaseGPUTransferBuffer(device, t);
    }

    buffers = SceneBuffers{};
}

bool Renderer::LoadScene(Scene &scene, entt::registry &registry)
{
    globalTextureArray = scene.textures.BuildGPUArray(device);
    if (!globalTextureArray)
    {
        SDL_Log("Failed to build the scene texture array");
        return false;
    }

    // Sized from the scene that was actually built, so adding geometry cannot
    // quietly overrun a buffer dimensioned for some other scene.
    // Adopt the mesh library before sizing anything: CreateSceneBuffers reads it
    // to dimension the geometry buffers. The Scene outlives the frame loop, so
    // holding a pointer is safe and avoids copying every vertex.
    meshLibrary = &scene.meshes;

    const Uint32 maxObjects = static_cast<Uint32>(scene.Count());
    if (!CreateSceneBuffers(maxObjects, MaxNodesFor(maxObjects), CountLights(registry)))
    {
        return false;
    }

    // Geometry is uploaded exactly once. That is the whole point of the
    // two-level structure: an instance moving rewrites 48 bytes, not a vertex
    // buffer, so nothing below this line runs again per frame.
    if (!UploadMeshGeometry(scene.meshes))
    {
        return false;
    }

    SDL_Log("Meshes: %zu mesh(es), %zu vertices, %zu triangles, %zu BLAS nodes",
            scene.meshes.Count(),
            scene.meshes.Vertices().size(),
            scene.meshes.Indices().size() / 3,
            scene.meshes.BLASNodes().size());

    sceneDirty = true;
    RebuildAcceleration(registry);
    return true;
}

bool Renderer::UploadMeshGeometry(const MeshLibrary &library)
{
    if (library.Empty())
    {
        return true; // buffers still exist at their one-slot floor, just unused
    }

    const std::vector<Vertex_GPU> &verts = library.Vertices();
    const std::vector<uint32_t> &inds = library.Indices();
    const std::vector<BVHNode_GPU> &nodes = library.BLASNodes();

    void *vertexData = SDL_MapGPUTransferBuffer(device, buffers.vertexTransfer, true);
    void *indexData = SDL_MapGPUTransferBuffer(device, buffers.indexTransfer, true);
    void *blasData = SDL_MapGPUTransferBuffer(device, buffers.blasTransfer, true);

    if (!vertexData || !indexData || !blasData)
    {
        SDL_Log("Failed to map mesh transfer buffers: %s", SDL_GetError());
        return false;
    }

    SDL_memcpy(vertexData, verts.data(), verts.size() * sizeof(Vertex_GPU));
    SDL_memcpy(indexData, inds.data(), inds.size() * sizeof(uint32_t));
    SDL_memcpy(blasData, nodes.data(), nodes.size() * sizeof(BVHNode_GPU));

    SDL_UnmapGPUTransferBuffer(device, buffers.vertexTransfer);
    SDL_UnmapGPUTransferBuffer(device, buffers.indexTransfer);
    SDL_UnmapGPUTransferBuffer(device, buffers.blasTransfer);

    SDL_GPUCommandBuffer *commandBuffer = SDL_AcquireGPUCommandBuffer(device);
    if (!commandBuffer)
    {
        SDL_Log("Failed to acquire command buffer: %s", SDL_GetError());
        return false;
    }

    SDL_GPUCopyPass *copyPass = SDL_BeginGPUCopyPass(commandBuffer);
    if (!copyPass)
    {
        SDL_Log("Failed to begin mesh copy pass: %s", SDL_GetError());
        SDL_CancelGPUCommandBuffer(commandBuffer);
        return false;
    }

    const struct
    {
        SDL_GPUTransferBuffer *transfer;
        SDL_GPUBuffer *buffer;
        Uint32 size;
    } uploads[] = {
        {buffers.vertexTransfer, buffers.vertexBuffer, static_cast<Uint32>(verts.size() * sizeof(Vertex_GPU))},
        {buffers.indexTransfer, buffers.indexBuffer, static_cast<Uint32>(inds.size() * sizeof(uint32_t))},
        {buffers.blasTransfer, buffers.blasBuffer, static_cast<Uint32>(nodes.size() * sizeof(BVHNode_GPU))},
    };

    for (const auto &u : uploads)
    {
        if (u.size == 0)
            continue;

        SDL_GPUTransferBufferLocation source = {};
        source.transfer_buffer = u.transfer;

        SDL_GPUBufferRegion destination = {};
        destination.buffer = u.buffer;
        destination.size = u.size;

        SDL_UploadToGPUBuffer(copyPass, &source, &destination, false);
    }

    SDL_EndGPUCopyPass(copyPass);

    if (!SDL_SubmitGPUCommandBuffer(commandBuffer))
    {
        SDL_Log("Failed to submit mesh geometry upload: %s", SDL_GetError());
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Per-frame data
// ---------------------------------------------------------------------------

uint32_t Renderer::CountLights(const entt::registry &registry)
{
    uint32_t count = 0;
    auto view = registry.view<MaterialComponent>();
    for (auto e : view)
    {
        if (view.get<MaterialComponent>(e).material.type == MaterialType::DiffuseLight)
            count++;
    }
    return count;
}

std::vector<uint32_t> Renderer::LightIndices(const entt::registry &registry,
                                             const std::vector<entt::entity> &ordered)
{
    std::vector<uint32_t> lights;
    for (uint32_t i = 0; i < ordered.size(); i++)
    {
        if (registry.get<MaterialComponent>(ordered[i]).material.type == MaterialType::DiffuseLight)
            lights.push_back(i);
    }
    return lights;
}

std::vector<Object_GPU> Renderer::PackObjects(const entt::registry &registry,
                                              const std::vector<entt::entity> &ordered,
                                              const MeshLibrary &library,
                                              std::vector<MeshInstance_GPU> &outInstances)
{
    std::vector<Object_GPU> list;
    list.reserve(ordered.size());

    outInstances.clear();
    outInstances.reserve(ordered.size());

    for (entt::entity e : ordered)
    {
        // Mesh instances are handled here rather than in EntityToGPU because
        // packing one needs the slot it will occupy in the instance buffer,
        // and only this loop knows that.
        if (const auto *mesh = registry.try_get<MeshComponent>(e))
        {
            const auto &t = registry.get<TransformComponent>(e);
            const auto &mat = registry.get<MaterialComponent>(e).material;
            const MeshRange &range = library.Range(mesh->mesh);

            MeshInstance_GPU inst = {};
            inst.px = t.position.x;
            inst.py = t.position.y;
            inst.pz = t.position.z;

            // Quat<float> stores x,y,z,w in memory - the same order as an HLSL
            // float4 - even though its 4-argument constructor takes (w,x,y,z).
            // Name the fields rather than trusting either order.
            inst.qx = t.rotation.x;
            inst.qy = t.rotation.y;
            inst.qz = t.rotation.z;
            inst.qw = t.rotation.w;

            inst.vertexBase = range.vertexBase;
            inst.indexBase = range.indexBase;
            inst.triangleCount = range.triangleCount;
            inst.blasBase = range.blasBase;

            const Uint32 slot = static_cast<Uint32>(outInstances.size());
            outInstances.push_back(inst);

            list.push_back(MeshToGPU(t, mat, slot));
            continue;
        }

        list.push_back(EntityToGPU(registry, e));
    }
    return list;
}


bool Renderer::UploadScene(const std::vector<Object_GPU> &objects,
                           const std::vector<BVHNode_GPU> &nodeData,
                           const std::vector<uint32_t> &lights,
                           const std::vector<MeshInstance_GPU> &meshInstances)
{
    // These buffers are fixed-size; without this check an oversized scene would
    // memcpy straight past the end of the mapped transfer buffer.
    if (objects.size() > buffers.maxObjects ||
        nodeData.size() > buffers.maxNodes ||
        lights.size() > buffers.maxLights ||
        meshInstances.size() > buffers.maxInstances)
    {
        SDL_Log("Scene too large for its buffers: %zu/%u objects, %zu/%u nodes, %zu/%u lights, %zu/%u instances",
                objects.size(), buffers.maxObjects,
                nodeData.size(), buffers.maxNodes,
                lights.size(), buffers.maxLights,
                meshInstances.size(), buffers.maxInstances);
        return false;
    }

    void *objectData = SDL_MapGPUTransferBuffer(device, buffers.objectTransfer, true);
    void *bvhData = SDL_MapGPUTransferBuffer(device, buffers.bvhTransfer, true);
    void *lightData = SDL_MapGPUTransferBuffer(device, buffers.lightIDTransfer, true);
    void *instanceData = SDL_MapGPUTransferBuffer(device, buffers.instanceTransfer, true);

    if (!objectData || !bvhData || !lightData || !instanceData)
    {
        SDL_Log("Failed to map scene transfer buffers: %s", SDL_GetError());
        return false;
    }

    if (!objects.empty())
        SDL_memcpy(objectData, objects.data(), objects.size() * sizeof(Object_GPU));
    if (!nodeData.empty())
        SDL_memcpy(bvhData, nodeData.data(), nodeData.size() * sizeof(BVHNode_GPU));
    if (!lights.empty())
        SDL_memcpy(lightData, lights.data(), lights.size() * sizeof(uint32_t));
    if (!meshInstances.empty())
        SDL_memcpy(instanceData, meshInstances.data(), meshInstances.size() * sizeof(MeshInstance_GPU));

    SDL_UnmapGPUTransferBuffer(device, buffers.objectTransfer);
    SDL_UnmapGPUTransferBuffer(device, buffers.bvhTransfer);
    SDL_UnmapGPUTransferBuffer(device, buffers.lightIDTransfer);
    SDL_UnmapGPUTransferBuffer(device, buffers.instanceTransfer);

    SDL_GPUCommandBuffer *commandBuffer = SDL_AcquireGPUCommandBuffer(device);
    if (!commandBuffer)
    {
        SDL_Log("Failed to acquire command buffer: %s", SDL_GetError());
        return false;
    }

    SDL_GPUCopyPass *copyPass = SDL_BeginGPUCopyPass(commandBuffer);
    if (!copyPass)
    {
        SDL_Log("Failed to begin copy pass: %s", SDL_GetError());
        SDL_CancelGPUCommandBuffer(commandBuffer);
        return false;
    }

    const struct
    {
        SDL_GPUTransferBuffer *transfer;
        SDL_GPUBuffer *buffer;
        Uint32 size;
    } uploads[] = {
        {buffers.objectTransfer, buffers.objectBuffer, static_cast<Uint32>(objects.size() * sizeof(Object_GPU))},
        {buffers.bvhTransfer, buffers.bvhBuffer, static_cast<Uint32>(nodeData.size() * sizeof(BVHNode_GPU))},
        {buffers.lightIDTransfer, buffers.lightIDBuffer, static_cast<Uint32>(lights.size() * sizeof(uint32_t))},
        {buffers.instanceTransfer, buffers.instanceBuffer, static_cast<Uint32>(meshInstances.size() * sizeof(MeshInstance_GPU))},
    };

    for (const auto &u : uploads)
    {
        if (u.size == 0)
            continue;

        SDL_GPUTransferBufferLocation source = {};
        source.transfer_buffer = u.transfer;

        SDL_GPUBufferRegion destination = {};
        destination.buffer = u.buffer;
        destination.size = u.size;

        SDL_UploadToGPUBuffer(copyPass, &source, &destination, true);
    }

    SDL_EndGPUCopyPass(copyPass);

    if (!SDL_SubmitGPUCommandBuffer(commandBuffer))
    {
        SDL_Log("Failed to submit scene upload: %s", SDL_GetError());
        return false;
    }

    return true;
}

void Renderer::RebuildAcceleration(const entt::registry &registry)
{
    GatherRenderables(registry, renderables, renderableBounds);

    // The builder works over bounds alone and reports where each primitive
    // landed; mapping that permutation back onto entities is this layer's job.
    // Leaves address the object buffer by slot, so orderedEntities has to be
    // uploaded in exactly this order.
    BuildBVH(renderableBounds, nodes, buildOrder);

    orderedEntities.clear();
    orderedEntities.reserve(buildOrder.size());
    for (uint32_t index : buildOrder)
    {
        orderedEntities.push_back(renderables[index]);
    }

    lightIDs = LightIndices(registry, orderedEntities);
    config.num_spheres = static_cast<Uint32>(orderedEntities.size());
    config.num_lights = static_cast<Uint32>(lightIDs.size());

    const MeshLibrary empty;
    const MeshLibrary &library = meshLibrary ? *meshLibrary : empty;

    if (!UploadScene(PackObjects(registry, orderedEntities, library, instances),
                     nodes, lightIDs, instances))
    {
        SDL_Log("Failed to upload scene");
        return;
    }

    sceneDirty = false;
}

// ---------------------------------------------------------------------------
// Camera and scene settings
// ---------------------------------------------------------------------------

void Renderer::SyncCamera(const entt::registry &registry, entt::entity camera)
{
    const auto &transform = registry.get<TransformComponent>(camera);
    const auto &cam = registry.get<CameraComponent>(camera);
    const Vec3<float> forward = Forward(const_cast<CameraComponent &>(cam));

    config.source_x = transform.position.x;
    config.source_y = transform.position.y;
    config.source_z = transform.position.z;
    config.target_x = transform.position.x + forward.x;
    config.target_y = transform.position.y + forward.y;
    config.target_z = transform.position.z + forward.z;
    config.focus_dist = cam.focus_dist;
    config.defocus_angle = cam.defocus_angle;
    config.fov = cam.fov;
}

void Renderer::SyncSceneSettings(const Scene &scene, const entt::registry &registry, entt::entity camera)
{
    // Sky, horizon and bounce depth are authored by the scene builder, so this
    // has to run *after* the builder. Reading them at camera-creation time -
    // which is what used to happen - always picked up the zero-initialised
    // CameraComponent instead, so every scene rendered against a black sky no
    // matter what it asked for.
    const auto &cam = registry.get<CameraComponent>(camera);

    config.sky_r = cam.sky.x;
    config.sky_g = cam.sky.y;
    config.sky_b = cam.sky.z;
    config.horizon_r = cam.horizon.x;
    config.horizon_g = cam.horizon.y;
    config.horizon_b = cam.horizon.z;
    config.depth = scene.maxDepth;
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

bool Renderer::RenderFrame(const entt::registry &registry)
{
    if (sceneDirty)
    {
        RebuildAcceleration(registry);
    }

    config.batch = accumulationFrame;

    SDL_GPUCommandBuffer *commandBuffer = SDL_AcquireGPUCommandBuffer(device);
    if (!commandBuffer)
    {
        SDL_Log("Failed to acquire command buffer: %s", SDL_GetError());
        return false;
    }

    SDL_PushGPUComputeUniformData(commandBuffer, 0, &config, sizeof(config));

    SDL_GPUStorageTextureReadWriteBinding storageTextureBindings[2] = {};
    storageTextureBindings[0].texture = accumTexture;
    storageTextureBindings[1].texture = displayTexture;

    SDL_GPUComputePass *computePass =
        SDL_BeginGPUComputePass(commandBuffer, storageTextureBindings, 2, nullptr, 0);
    if (!computePass)
    {
        SDL_Log("Failed to begin compute pass: %s", SDL_GetError());
        SDL_CancelGPUCommandBuffer(commandBuffer);
        return false;
    }

    SDL_BindGPUComputePipeline(computePass, pipeline);

    // Order must match the register numbers in the shader (t1..t7) and the
    // count in CreateComputePipeline. All three move together or not at all.
    SDL_GPUBuffer *storageBuffers[7] = {
        buffers.objectBuffer, buffers.bvhBuffer, buffers.lightIDBuffer,
        buffers.vertexBuffer, buffers.indexBuffer, buffers.blasBuffer, buffers.instanceBuffer};
    SDL_BindGPUComputeStorageBuffers(computePass, 0, storageBuffers, 7);

    SDL_GPUTextureSamplerBinding textureBinding = {};
    textureBinding.texture = globalTextureArray;
    textureBinding.sampler = linearSampler;
    SDL_BindGPUComputeSamplers(computePass, 0, &textureBinding, 1);

    SDL_DispatchGPUCompute(computePass, (config.width + THREADS - 1) / THREADS, config.height, 1);
    SDL_EndGPUComputePass(computePass);

    SDL_GPUTexture *swapchain = nullptr;
    Uint32 swapWidth = 0;
    Uint32 swapHeight = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(commandBuffer, window, &swapchain, &swapWidth, &swapHeight) ||
        !swapchain)
    {
        SDL_CancelGPUCommandBuffer(commandBuffer);
        return false;
    }

    SDL_GPUBlitInfo blit = {};
    // The display target, not accumTexture: the swapchain is SDR, so it needs
    // the tone-mapped, sRGB-encoded image. Blitting the linear HDR buffer is
    // what made highlights clip to flat white.
    blit.source.texture = displayTexture;
    blit.source.w = config.width;
    blit.source.h = config.height;
    blit.destination.texture = swapchain;
    blit.destination.w = swapWidth;
    blit.destination.h = swapHeight;
    SDL_BlitGPUTexture(commandBuffer, &blit);

    if (!SDL_SubmitGPUCommandBuffer(commandBuffer))
    {
        SDL_Log("Failed to submit frame: %s", SDL_GetError());
        return false;
    }

    accumulationFrame++;
    return true;
}

void Renderer::Shutdown()
{
    nodes.clear();

    if (!device)
        return;

    if (accumTexture)
        SDL_ReleaseGPUTexture(device, accumTexture);
    if (displayTexture)
        SDL_ReleaseGPUTexture(device, displayTexture);
    if (globalTextureArray)
        SDL_ReleaseGPUTexture(device, globalTextureArray);

    ReleaseSceneBuffers();

    if (pipeline)
        SDL_ReleaseGPUComputePipeline(device, pipeline);
    if (linearSampler)
        SDL_ReleaseGPUSampler(device, linearSampler);
    if (window)
        SDL_ReleaseWindowFromGPUDevice(device, window);

    SDL_DestroyGPUDevice(device);
    if (window)
        SDL_DestroyWindow(window);
    SDL_Quit();

    device = nullptr;
    window = nullptr;
    pipeline = nullptr;
    linearSampler = nullptr;
    accumTexture = nullptr;
    displayTexture = nullptr;
    globalTextureArray = nullptr;
}
