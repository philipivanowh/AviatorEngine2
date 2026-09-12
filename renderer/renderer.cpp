#include "renderer/renderer.h"

#include <algorithm>
#include <cmath>

#include "core/common.h"
#include "scene/components.h"
#include "scene/shapes.h"
#include "systems/cameraSystem.h"

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

SDL_GPUComputePipeline *Renderer::CreateComputePipeline(const char *stem,
                                                        Uint32 numSamplers,
                                                        Uint32 numStorageBuffers,
                                                        Uint32 numReadWriteTextures,
                                                        Uint32 threadsX,
                                                        Uint32 threadsY)
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
    // Sampled textures. SDL numbers a compute shader's read-only t-registers by
    // KIND - every sampled texture first, then the storage buffers - so this
    // count is what decides where the storage buffers start: t1 when it is 1,
    // t5 when it is 5. The count, the register numbers in the shader and the
    // bind arrays in RenderFrame all move together or not at all.
    cpci.num_samplers = numSamplers;
    // For the ray tracing shaders: objects, BVH nodes, light ids, mesh
    // vertices, mesh indices, BLAS nodes, mesh instances - in that order,
    // directly after the sampled textures. Too few and the tail buffers read as
    // zeroes: every light silently disappears, every mesh collapses to nothing.
    cpci.num_readonly_storage_buffers = numStorageBuffers;
    // The accumulation image (u0) and displayTexture (u1), plus for deferred.comp
    // lightingReduced and this frame's history geometry (u2, u3). Same rule: this count, the bindings passed to
    // SDL_BeginGPUComputePass and the shader's registers move together.
    cpci.num_readwrite_storage_textures = numReadWriteTextures;
    cpci.num_uniform_buffers = 1;
    cpci.threadcount_x = threadsX;
    cpci.threadcount_y = threadsY;
    cpci.threadcount_z = 1;

    SDL_GPUComputePipeline *created = SDL_CreateGPUComputePipeline(device, &cpci);
    SDL_free(data);

    if (!created)
    {
        SDL_Log("Failed to create compute pipeline '%s': %s", stem, SDL_GetError());
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
    config.exposure = .1f;
    config.debugView = static_cast<Uint32>(DebugView::Final);
    config.zNear = cameraMatrices.zNear;
    config.zFar = cameraMatrices.zFar;

    hybrid = settings.hybrid;
    hybridMaxDepth = std::max(settings.hybridMaxDepth, 1u);
    maxSamples = settings.maxSamples;
    traceResolution = settings.traceResolution;
    historyWhileMoving = std::max(settings.historyWhileMoving, 1u);

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

    // Claiming the window gives a VSYNC swapchain, and VSYNC holds any frame
    // slower than one refresh until the next one: a 20 ms frame on a 60 Hz
    // display is shown for 33 ms, so the counter reads 30 fps instead of 50.
    // MAILBOX presents the newest finished frame without tearing; IMMEDIATE may
    // tear. VSYNC stays as the fallback because SDL guarantees it everywhere.
    // The TARGET_FPS cap in main.cpp still applies on top - V toggles it.
    const char *presentMode = "vsync";
    if (SDL_WindowSupportsGPUPresentMode(device, window, SDL_GPU_PRESENTMODE_MAILBOX) &&
        SDL_SetGPUSwapchainParameters(device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_MAILBOX))
    {
        presentMode = "mailbox";
    }
    else if (SDL_WindowSupportsGPUPresentMode(device, window, SDL_GPU_PRESENTMODE_IMMEDIATE) &&
             SDL_SetGPUSwapchainParameters(device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_IMMEDIATE))
    {
        presentMode = "immediate";
    }
    SDL_Log("Present mode: %s", presentMode);

    SDL_RaiseWindow(window);

    // GlobalTextureArray is the only sampled texture, so the storage buffers sit
    // at t1..t7; the accumulation image and displayTexture are u0/u1.
    pipeline = CreateComputePipeline(settings.shader, 1, 7, 2, THREADS, 1);
    if (!pipeline)
    {
        return false;
    }

    // GlobalTextureArray, gPosition, gNormal, gAlbedo, gDepth, and last frame's
    // image and geometry for reprojection: seven sampled textures, so the
    // storage buffers sit at t7..t13. Read-write: image, display,
    // lightingReduced and this frame's geometry (u0..u3). Built alongside the
    // compute-only pipeline rather than instead of it, so SetHybrid can flip
    // between the two for an A/B check.
    deferredPipeline = CreateComputePipeline(settings.hybridShader, 7, 7, 4, 16, 16);
    if (!deferredPipeline)
    {
        return false;
    }

    // gPosition, gNormal, gAlbedo, lightingReduced, and last frame's image and
    // geometry in (t0..t5); image, display and geometry out (u0..u2); no scene
    // buffers - it traces nothing. Built in both modes because it is small and
    // because full vs reduced is decided per frame: debug views always run at
    // full resolution.
    upsamplePipeline = CreateComputePipeline("upsample.comp", 6, 2, 3, 16, 16);
    if (!upsamplePipeline)
    {
        return false;
    }

    if (traceResolution == TraceResolution::Full)
    {
        SDL_Log("Ray trace resolution: full");
    }
    else
    {
        const float scale = TraceScaleOf(traceResolution);
        SDL_Log("Ray trace resolution: %ux%u, upsampled to %ux%u",
                ScaledExtent(config.width, scale), ScaledExtent(config.height, scale),
                config.width, config.height);
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

    // The shader reads image[id] back to blend this frame into the running
    // average, and reading a storage texture it also writes within one compute
    // pass requires SIMULTANEOUS_READ_WRITE - plain WRITE is not enough, and
    // without it the read is undefined (it happened to work on this driver).
    textureInfo.usage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE |
                        SDL_GPU_TEXTUREUSAGE_SAMPLER;

    // The geometry history beside each accumulation image: position + history
    // length per pixel, written by the compute passes and sampled next frame.
    SDL_GPUTextureCreateInfo geometryInfo = textureInfo;
    geometryInfo.format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
    geometryInfo.usage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_TEXTUREUSAGE_SAMPLER;

    for (int i = 0; i < 2; i++)
    {
        accumTextures[i] = SDL_CreateGPUTexture(device, &textureInfo);
        historyGeometry[i] = SDL_CreateGPUTexture(device, &geometryInfo);
        if (!accumTextures[i] || !historyGeometry[i])
        {
            SDL_Log("Failed to create accumulation history textures: %s", SDL_GetError());
            return false;
        }
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

    // The reduced-resolution lighting target, the size of the trace grid. RGBA16F
    // like the accumulation - it holds linear lighting - and SAMPLER as well as
    // storage write, because deferred.comp writes it and upsample.comp reads it
    // in the next pass. At full resolution nothing uses it, but deferred.comp
    // still declares it, so a 1x1 texture keeps the binding valid.
    const bool tracesReduced = traceResolution != TraceResolution::Full;
    const float reducedScale = TraceScaleOf(traceResolution);

    SDL_GPUTextureCreateInfo reducedInfo = textureInfo;
    reducedInfo.width = tracesReduced ? ScaledExtent(config.width, reducedScale) : 1;
    reducedInfo.height = tracesReduced ? ScaledExtent(config.height, reducedScale) : 1;
    reducedInfo.usage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_TEXTUREUSAGE_SAMPLER;

    lightingReduced = SDL_CreateGPUTexture(device, &reducedInfo);
    if (!lightingReduced)
    {
        SDL_Log("Failed to create reduced-resolution lighting texture: %s", SDL_GetError());
        return false;
    }

    if (!CreateGBuffer() || !CreateGBufferPipeline())
    {
        return false;
    }

    return true;
}

SDL_GPUShader *Renderer::LoadShader(const char *stem,
                                    SDL_GPUShaderStage stage,
                                    Uint32 numSamplers,
                                    Uint32 numUniformBuffers,
                                    Uint32 numStorageBuffers)
{
    const std::string path = std::string(stem) + ".spv";

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

    SDL_GPUShaderCreateInfo info = {};
    info.code = static_cast<Uint8 *>(data);
    info.code_size = size;
    info.entrypoint = "main";
    info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    info.stage = stage;
    // These counts are not documentation - SDL builds the descriptor set layout
    // from them. Too few and the binding is silently dropped; too many and
    // pipeline creation fails outright, which is the friendlier of the two.
    info.num_samplers = numSamplers;
    info.num_uniform_buffers = numUniformBuffers;
    info.num_storage_buffers = numStorageBuffers;

    SDL_GPUShader *shader = SDL_CreateGPUShader(device, &info);
    SDL_free(data);

    if (!shader)
    {
        SDL_Log("Failed to create shader '%s': %s", path.c_str(), SDL_GetError());
    }
    return shader;
}

bool Renderer::CreateGBuffer()
{
    // COLOR_TARGET so the raster pass can write them, SAMPLER so the compute
    // pass can read them back. SDL_gpu inserts the layout transition between
    // the two passes itself - there is no manual barrier to get wrong.
    const struct
    {
        SDL_GPUTexture **texture;
        SDL_GPUTextureFormat format;
        const char *name;
    } targets[] = {
        {&gPosition, SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT, "gPosition"},
        {&gNormal, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, "gNormal"},
        {&gAlbedo, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, "gAlbedo"},
    };

    for (const auto &t : targets)
    {
        SDL_GPUTextureCreateInfo info = {};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.width = config.width;
        info.height = config.height;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        info.format = t.format;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;

        *t.texture = SDL_CreateGPUTexture(device, &info);
        if (!*t.texture)
        {
            SDL_Log("Failed to create %s: %s", t.name, SDL_GetError());
            return false;
        }
    }

    // Ask rather than assume. D32_FLOAT is near-universal but a driver is
    // allowed to refuse it, and the failure mode without this check is a null
    // texture and a pipeline that never validates. SAMPLER is part of the ask
    // because deferred.comp reads depth back (WorldFromDepth, guide section 9.1).
    const SDL_GPUTextureUsageFlags depthUsage =
        SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    if (!SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
                                      SDL_GPU_TEXTURETYPE_2D, depthUsage))
    {
        SDL_Log("D32_FLOAT sampled depth targets are not supported on this device");
        return false;
    }

    SDL_GPUTextureCreateInfo depthInfo = {};
    depthInfo.type = SDL_GPU_TEXTURETYPE_2D;
    depthInfo.width = config.width;
    depthInfo.height = config.height;
    depthInfo.layer_count_or_depth = 1;
    depthInfo.num_levels = 1;
    depthInfo.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    depthInfo.usage = depthUsage;
    gDepth = SDL_CreateGPUTexture(device, &depthInfo);
    if (!gDepth)
    {
        SDL_Log("Failed to create gDepth: %s", SDL_GetError());
        return false;
    }

    // NEAREST, not LINEAR. Interpolating a world position across the silhouette
    // between a foreground and a background surface invents a point that is on
    // neither of them, and every shadow ray fired from it is wrong. G-buffers
    // are point-sampled, always.
    SDL_GPUSamplerCreateInfo samplerInfo = {};
    samplerInfo.min_filter = SDL_GPU_FILTER_NEAREST;
    samplerInfo.mag_filter = SDL_GPU_FILTER_NEAREST;
    samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    pointSampler = SDL_CreateGPUSampler(device, &samplerInfo);
    if (!pointSampler)
    {
        SDL_Log("Failed to create point sampler: %s", SDL_GetError());
        return false;
    }

    return true;
}

bool Renderer::CreateGBufferPipeline()
{
    SDL_GPUShader *vs = LoadShader("gbuffer.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 0);
    SDL_GPUShader *fs = LoadShader("gbuffer.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1, 0);
    if (!vs || !fs)
    {
        if (vs)
            SDL_ReleaseGPUShader(device, vs);
        if (fs)
            SDL_ReleaseGPUShader(device, fs);
        return false;
    }

    // One vertex stream, straight out of the mesh library. The pitch is
    // Vertex_GPU's 32 bytes - there is no interleave/deinterleave step because
    // the ray tracer's layout already happened to be a good vertex layout.
    SDL_GPUVertexBufferDescription vertexBufferDesc = {};
    vertexBufferDesc.slot = 0;
    vertexBufferDesc.pitch = sizeof(Vertex_GPU);
    vertexBufferDesc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    vertexBufferDesc.instance_step_rate = 0;

    // Offsets mirror Vertex_GPU exactly: px at 0, nx at 12, u at 24.
    SDL_GPUVertexAttribute attributes[3] = {};
    attributes[0].location = 0;
    attributes[0].buffer_slot = 0;
    attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    attributes[0].offset = offsetof(Vertex_GPU, px);
    attributes[1].location = 1;
    attributes[1].buffer_slot = 0;
    attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    attributes[1].offset = offsetof(Vertex_GPU, nx);
    attributes[2].location = 2;
    attributes[2].buffer_slot = 0;
    attributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attributes[2].offset = offsetof(Vertex_GPU, u);

    SDL_GPUColorTargetDescription colorTargets[3] = {};
    colorTargets[0].format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
    colorTargets[1].format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    colorTargets[2].format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    // Blending stays off on every one of them. A G-buffer is not a colour
    // image; blending two surface normals produces a normal that describes
    // neither surface.

    SDL_GPUGraphicsPipelineCreateInfo info = {};
    info.vertex_shader = vs;
    info.fragment_shader = fs;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.vertex_input_state.vertex_buffer_descriptions = &vertexBufferDesc;
    info.vertex_input_state.num_vertex_buffers = 1;
    info.vertex_input_state.vertex_attributes = attributes;
    info.vertex_input_state.num_vertex_attributes = 3;

    // Back faces culled (guide section 9.4). Mesh::CreateBox and CreateSphere
    // both wind counter-clockwise seen from outside, and SDL_gpu defines
    // winding in its +Y-up NDC on every backend, so CCW is front here. If a
    // new mesh source renders inside-out, check it with the Normal debug view
    // and flip front_face for it - never cull_mode - before touching this.
    // Open surfaces - quads - would vanish from behind with this, so meshes
    // registered doubleSided are drawn with a second, unculled pipeline below.
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

    info.depth_stencil_state.enable_depth_test = true;
    info.depth_stencil_state.enable_depth_write = true;
    // LESS, with a clear to 1.0: PerspectiveRH_ZO puts the near plane at 0 and
    // the far plane at 1, so nearer really is smaller here. (If you later move
    // to a reversed-Z depth buffer for precision, this flips to GREATER and the
    // clear flips to 0.0 - both together or neither.)
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;

    info.target_info.color_target_descriptions = colorTargets;
    info.target_info.num_color_targets = 3;
    info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    info.target_info.has_depth_stencil_target = true;

    gbufferPipeline = SDL_CreateGPUGraphicsPipeline(device, &info);

    // The same pipeline with culling off, for meshes with two visible sides
    // (MeshRange::doubleSided - every quad). Nothing else about a two-sided
    // surface needs special handling: HitMesh already treats a triangle hit
    // from behind as a back face with its normal flipped toward the ray, and
    // HitFromGBuffer does the same for a rasterized one.
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    gbufferPipelineTwoSided = SDL_CreateGPUGraphicsPipeline(device, &info);

    // The pipelines hold their own references once created, so these can go.
    SDL_ReleaseGPUShader(device, vs);
    SDL_ReleaseGPUShader(device, fs);

    if (!gbufferPipeline || !gbufferPipelineTwoSided)
    {
        SDL_Log("Failed to create G-buffer pipeline: %s", SDL_GetError());
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
    // still has to leave the mesh buffers bound to something valid, or the
    // shader reads an unbound buffer.
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
        SDL_GPUBufferUsageFlags usage;
    } allocations[] = {
        {&buffers.objectBuffer, &buffers.objectTransfer,
         maxObjectCount * sizeof(Object_GPU), SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ},
        {&buffers.bvhBuffer, &buffers.bvhTransfer,
         maxNodeCount * sizeof(BVHNode_GPU), SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ},
        {&buffers.lightIDBuffer, &buffers.lightIDTransfer,
         maxLightCount * sizeof(uint32_t), SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ},

        // The two buffers the rasterizer also consumes. VERTEX/INDEX are ADDED
        // to the compute usage rather than replacing it - the ray tracer still
        // walks the same memory through HitMeshTriangle(), and a buffer can
        // carry both roles as long as both are declared up front.
        {&buffers.vertexBuffer, &buffers.vertexTransfer,
         buffers.maxVertices * sizeof(Vertex_GPU),
         SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_VERTEX},
        {&buffers.indexBuffer, &buffers.indexTransfer,
         buffers.maxIndices * sizeof(uint32_t),
         SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_INDEX},

        {&buffers.blasBuffer, &buffers.blasTransfer,
         buffers.maxBLASNodes * sizeof(BVHNode_GPU), SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ},
        {&buffers.instanceBuffer, &buffers.instanceTransfer,
         buffers.maxInstances * sizeof(MeshInstance_GPU), SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ},
    };

    for (const auto &a : allocations)
    {
        SDL_GPUBufferCreateInfo bufferInfo = {};
        bufferInfo.usage = a.usage;
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

    // The shared raster proxy for analytic boxes: one unit cube, stretched to
    // each box's half-extents in the vertex shader. It has to be registered
    // HERE, before CreateSceneBuffers sizes the geometry buffers from the
    // library. The ray tracer never instances it - boxes stay analytic in the
    // BVH, where a slab test is far cheaper than walking 12 triangles.
    boxProxyMesh = scene.meshes.Add(
        Mesh::CreateBox(Vec3<float>(1.0f, 1.0f, 1.0f),
                        Material::Lambertian(Color(1.0f, 1.0f, 1.0f)),
                        AABB{-1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f}));

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

    SDL_Log("Raster: %zu draw(s) - mesh instances plus analytic box proxies", rasterDraws.size());
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
                                              std::vector<MeshInstance_GPU> &outInstances,
                                              std::vector<RasterDraw> &outDraws,
                                              const MeshRange *boxProxy)
{
    std::vector<Object_GPU> list;
    list.reserve(ordered.size());

    outInstances.clear();
    outInstances.reserve(ordered.size());
    outDraws.clear();

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

            const Object_GPU object = MeshToGPU(t, mat, slot);

            // Everything the G-buffer pass needs for this instance. Albedo and
            // texture come from the packed object rather than the material, so
            // the rasterizer shades with exactly what the ray tracer would.
            RasterDraw draw = {};
            draw.indexBase = range.indexBase;
            draw.indexCount = range.triangleCount * 3;
            draw.vertexBase = range.vertexBase;
            draw.objectIndex = static_cast<Uint32>(list.size()); // this object's slot
            draw.position[0] = inst.px;
            draw.position[1] = inst.py;
            draw.position[2] = inst.pz;
            draw.rotation[0] = inst.qx;
            draw.rotation[1] = inst.qy;
            draw.rotation[2] = inst.qz;
            draw.rotation[3] = inst.qw;
            draw.scale[0] = draw.scale[1] = draw.scale[2] = 1.0f; // instances are rigid
            draw.albedo[0] = object.r;
            draw.albedo[1] = object.g;
            draw.albedo[2] = object.b;
            draw.textureID = object.textureID;
            draw.textureTint = object.textureTint;
            draw.doubleSided = range.doubleSided;
            draw.colorType = object.colorType;
            outDraws.push_back(draw);

            list.push_back(object);
            continue;
        }

        const Object_GPU object = EntityToGPU(registry, e);

        // Analytic boxes also get a raster proxy - which is what puts the
        // feature scene's 400-box ground into the G-buffer instead of costing a
        // full BVH walk per pixel for primary visibility. The object is still
        // packed unchanged below, so every secondary ray keeps hitting the
        // exact analytic box.
        if (boxProxy && IsRasterProxy(object))
        {
            const auto &t = registry.get<TransformComponent>(e);

            RasterDraw draw = {};
            draw.indexBase = boxProxy->indexBase;
            draw.indexCount = boxProxy->triangleCount * 3;
            draw.vertexBase = boxProxy->vertexBase;
            draw.objectIndex = static_cast<Uint32>(list.size()); // this object's slot
            draw.position[0] = t.position.x;
            draw.position[1] = t.position.y;
            draw.position[2] = t.position.z;
            // Identity: IsRasterProxy only admits unrotated boxes.
            draw.rotation[3] = 1.0f;
            // The unit cube spans [-1, 1], so the half-extents ARE the scale.
            draw.scale[0] = object.half_x;
            draw.scale[1] = object.half_y;
            draw.scale[2] = object.half_z;
            draw.albedo[0] = object.r;
            draw.albedo[1] = object.g;
            draw.albedo[2] = object.b;
            draw.textureID = object.textureID;
            draw.textureTint = object.textureTint;
            draw.colorType = object.colorType;
            outDraws.push_back(draw);
        }

        list.push_back(object);
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

// Where every object was last frame, in the slots Object_GPU reserves for it.
// Temporal reprojection needs this to follow a moving body: without it a pixel
// can only look up where the CAMERA saw that point last frame, which for
// anything that moved is the wrong place, and the history waiting there belongs
// to something else - which is exactly what a trail is.
void Renderer::ApplyPreviousTransforms(const entt::registry &registry, std::vector<Object_GPU> &objects)
{
    uploadedMotion = false;

    const size_t count = std::min(objects.size(), orderedEntities.size());
    for (size_t slot = 0; slot < count; slot++)
    {
        const entt::entity entity = orderedEntities[slot];
        const auto found = previousTransforms.find(entity);
        if (found == previousTransforms.end())
        {
            continue; // new this frame, so "previous = current" is the honest answer
        }

        Object_GPU &object = objects[slot];
        const TransformComponent &previous = found->second;
        const TransformComponent &current = registry.get<TransformComponent>(entity);

        object.x2 = previous.position.x;
        object.y2 = previous.position.y;
        object.z2 = previous.position.z;

        // Through ShaderRotation, because the shader rebuilds the CURRENT
        // rotation differently per shape - a full quaternion for a mesh
        // instance, one yaw angle for a sphere or box. The two have to be
        // expressed the same way or they will not cancel for a static object.
        const BodyShape shape = static_cast<BodyShape>(object.shapeType);
        StorePreviousRotation(object, ShaderRotation(shape, previous.rotation));

        // Did this object actually move? A settled scene has to be able to say
        // no, or the catch-up rebuild in RenderFrame would never stop. The
        // rotation goes through the same packing so the two are comparable.
        Object_GPU probe = {};
        StorePreviousRotation(probe, ShaderRotation(shape, current.rotation));

        const Vec3<float> offset = current.position - previous.position;
        const float spin = std::fabs(probe.prevRotX - object.prevRotX) +
                           std::fabs(probe.prevRotY - object.prevRotY) +
                           std::fabs(probe.prevRotZ - object.prevRotZ);
        if (offset.length_squared() > 1e-12f || spin > 1e-6f)
        {
            uploadedMotion = true;
        }
    }

    previousTransforms.clear();
    previousTransforms.reserve(orderedEntities.size());
    for (entt::entity entity : orderedEntities)
    {
        previousTransforms.emplace(entity, registry.get<TransformComponent>(entity));
    }
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

    // The draw list is rebuilt with the object buffer, not separately: each
    // draw's objectIndex is a slot in the buffer this call is about to upload,
    // and the BVH reorders those slots on every rebuild.
    const MeshRange *boxProxy = (boxProxyMesh != kNoMesh) ? &library.Range(boxProxyMesh) : nullptr;

    std::vector<Object_GPU> objects =
        PackObjects(registry, orderedEntities, library, instances, rasterDraws, boxProxy);

    // PackObjects wrote "previous = current" into every object; this is where
    // the ones that actually moved get their real previous transform.
    ApplyPreviousTransforms(registry, objects);

    if (!UploadScene(objects, nodes, lightIDs, instances))
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
    const Vec3<float> forward = Forward(cam);

    config.source_x = transform.position.x;
    config.source_y = transform.position.y;
    config.source_z = transform.position.z;
    config.target_x = transform.position.x + forward.x;
    config.target_y = transform.position.y + forward.y;
    config.target_z = transform.position.z + forward.z;
    config.focus_dist = cam.focus_dist;
    config.defocus_angle = cam.defocus_angle;
    config.fov = cam.fov;

    // Built from the SAME numbers the compute shader reads out of Config, in
    // the same order, so there is no second source of truth for the camera.
    const Vec3<float> eye(config.source_x, config.source_y, config.source_z);
    const Vec3<float> target(config.target_x, config.target_y, config.target_z);
    const Vec3<float> up(config.up_x, config.up_y, config.up_z);
    const float aspect = static_cast<float>(config.width) / static_cast<float>(config.height);

    cameraMatrices.view = LookAtRH(eye, target, up);
    cameraMatrices.proj = PerspectiveRH_ZO(config.fov, aspect,
                                           cameraMatrices.zNear, cameraMatrices.zFar);
    cameraMatrices.viewProj = cameraMatrices.proj * cameraMatrices.view;
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
    sceneMaxDepth = std::max<Uint32>(static_cast<Uint32>(scene.maxDepth), 1u);
    ApplyDepthCap();
}

void Renderer::ApplyDepthCap()
{
    // The hybrid renderer is the real-time one, so it gets a bounce budget; the
    // compute-only renderer keeps the scene's full depth as the reference for
    // A/B checks. This is the biggest single GPU cost lever: every extra bounce
    // is another BVH traversal plus a shadow ray, for every pixel, every frame.
    const Uint32 depth = hybrid ? std::min(sceneMaxDepth, hybridMaxDepth) : sceneMaxDepth;
    if (depth != config.depth)
    {
        SDL_Log("Bounce depth: %u (%s)", depth, hybrid ? "hybrid cap" : "scene maxDepth");
    }
    config.depth = depth;
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

float Renderer::Halton(Uint32 index, Uint32 base)
{
    float result = 0.0f;
    float fraction = 1.0f;
    while (index > 0)
    {
        fraction /= static_cast<float>(base);
        result += fraction * static_cast<float>(index % base);
        index /= base;
    }
    return result;
}

void Renderer::RenderGBuffer(SDL_GPUCommandBuffer *commandBuffer, const Mat4 &viewProj)
{
    SDL_GPUColorTargetInfo colorTargets[3] = {};
    const SDL_GPUTexture *const targets[3] = {gPosition, gNormal, gAlbedo};
    for (int i = 0; i < 3; i++)
    {
        colorTargets[i].texture = const_cast<SDL_GPUTexture *>(targets[i]);
        colorTargets[i].load_op = SDL_GPU_LOADOP_CLEAR;
        colorTargets[i].store_op = SDL_GPU_STOREOP_STORE;
        // All zero, and for gNormal that zero IS the background test in the
        // compute pass: coverage in .w stays 0 wherever the rasterizer drew
        // nothing, which reads as "no surface here" rather than as a surface
        // with a zero normal - which would shade black instead of as sky.
        colorTargets[i].clear_color = {0.0f, 0.0f, 0.0f, 0.0f};
        colorTargets[i].cycle = true;
    }

    SDL_GPUDepthStencilTargetInfo depthTarget = {};
    depthTarget.texture = gDepth;
    depthTarget.clear_depth = 1.0f; // far plane; pairs with COMPAREOP_LESS
    depthTarget.load_op = SDL_GPU_LOADOP_CLEAR;
    depthTarget.store_op = SDL_GPU_STOREOP_STORE; // deferred.comp reads it back
    depthTarget.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depthTarget.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    depthTarget.cycle = true;

    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commandBuffer, colorTargets, 3, &depthTarget);
    if (!pass)
    {
        SDL_Log("Failed to begin G-buffer pass: %s", SDL_GetError());
        return;
    }

    // The whole mesh library is one vertex buffer and one index buffer. Per-draw
    // offsets do the rest - that is what MeshRange's bases are for, and it is
    // why there is no per-mesh buffer to rebind.
    SDL_GPUBufferBinding vertexBinding = {};
    vertexBinding.buffer = buffers.vertexBuffer;

    SDL_GPUBufferBinding indexBinding = {};
    indexBinding.buffer = buffers.indexBuffer;

    SDL_GPUTextureSamplerBinding textureBinding = {};
    textureBinding.texture = globalTextureArray;
    textureBinding.sampler = linearSampler;

    // Draws switch between the culled and the unculled pipeline mid-pass. The
    // buffer and sampler bindings are re-applied with every switch rather than
    // trusted to survive it: a binding that silently fell off would draw from
    // whatever happened to be bound last.
    SDL_GPUGraphicsPipeline *boundPipeline = nullptr;
    const auto bindPipeline = [&](SDL_GPUGraphicsPipeline *wanted)
    {
        if (wanted == boundPipeline)
        {
            return;
        }
        SDL_BindGPUGraphicsPipeline(pass, wanted);
        SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
        SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        SDL_BindGPUFragmentSamplers(pass, 0, &textureBinding, 1);
        boundPipeline = wanted;
    };
    bindPipeline(gbufferPipeline);

    // Byte layouts of InstanceBlock in gbuffer.vert.hlsl and SurfaceBlock in
    // gbuffer.frag.hlsl.
    struct VSUniform
    {
        Mat4 viewProj;
        float rotation[4];
        float position[4];
        float scale[4];
    };
    struct FSUniform
    {
        float albedo[4];
        Uint32 textureID;
        Uint32 objectIndex;
        float textureTint;
        Uint32 colorType;
    };

    for (const RasterDraw &draw : rasterDraws)
    {
        bindPipeline(draw.doubleSided ? gbufferPipelineTwoSided : gbufferPipeline);

        VSUniform vsu = {};
        vsu.viewProj = viewProj;
        SDL_memcpy(vsu.rotation, draw.rotation, sizeof(draw.rotation));
        SDL_memcpy(vsu.position, draw.position, sizeof(draw.position));
        SDL_memcpy(vsu.scale, draw.scale, sizeof(draw.scale));
        SDL_PushGPUVertexUniformData(commandBuffer, 0, &vsu, sizeof(vsu));

        FSUniform fsu = {};
        SDL_memcpy(fsu.albedo, draw.albedo, sizeof(draw.albedo));
        fsu.textureID = draw.textureID;
        fsu.objectIndex = draw.objectIndex;
        fsu.textureTint = draw.textureTint;
        fsu.colorType = draw.colorType;
        SDL_PushGPUFragmentUniformData(commandBuffer, 0, &fsu, sizeof(fsu));

        // first_index and vertex_offset are what let every mesh share one pair
        // of buffers. vertex_offset is added to each index AFTER the fetch, so
        // the library's mesh-relative indices are used exactly as stored - the
        // same trick the BLAS traversal plays with indexBase and vertexBase.
        SDL_DrawGPUIndexedPrimitives(pass,
                                     draw.indexCount,                      // num_indices
                                     1,                                    // num_instances
                                     draw.indexBase,                       // first_index
                                     static_cast<Sint32>(draw.vertexBase), // vertex_offset
                                     0);                                   // first_instance
    }

    SDL_EndGPURenderPass(pass);
}

bool Renderer::RenderFrame(const entt::registry &registry)
{
    // One last rebuild after everything stops. The object buffer is only
    // re-uploaded while the scene is dirty, so a settled scene would otherwise
    // keep the motion vectors the final moving frame left behind, and every
    // pixel would go on rejecting history for a world that is standing still -
    // which is the noise motion vectors exist to remove.
    if (!sceneDirty && uploadedMotion)
    {
        sceneDirty = true;
    }

    if (sceneDirty)
    {
        RebuildAcceleration(registry);
    }

    config.batch = accumulationFrame;

    // Converged (RendererSettings::maxSamples): there is nothing new to trace.
    // The compute pass still runs, but only to re-tone-map the finished image -
    // exposure is applied there, so skipping the pass would freeze [ and ].
    const bool converged = Converged();
    config.displayOnly = converged ? 1u : 0u;

    // Temporal reprojection state. The history textures swap only on frames
    // that write history - a traced frame of the lit hybrid image - so
    // writeSlot is the one this frame writes (or, on any other frame, reads and
    // writes in place) and readSlot is last frame's, bound as the history.
    const bool finalView = config.debugView == static_cast<Uint32>(DebugView::Final);
    const bool writesHistory = hybrid && finalView && !converged;
    const Uint32 writeSlot = writesHistory ? 1 - historyIndex : historyIndex;
    const Uint32 readSlot = 1 - writeSlot;

    config.frameIndex = frameIndex;
    config.historyReset = historyResetPending ? 1u : 0u;
    config.cameraMoved = cameraMovedPending ? 1u : 0u;
    config.historyCap = static_cast<float>(historyWhileMoving);
    config.prevViewProj = previousViewProj;

    // Sub-pixel jitter (guide section 9.3). The G-buffer is rasterized at pixel
    // centres, so without this the hybrid image has no antialiasing at all.
    // Offsetting the projection moves every raster sample by the same fraction
    // of a pixel, and deferred.comp shifts its primary ray by the identical
    // amount via config.jitter_x/y - the running average over frames then
    // resolves into antialiasing exactly as the old per-sample random jitter
    // did. Debug views hold still so they can be inspected.
    Mat4 viewProj = cameraMatrices.viewProj;
    config.jitter_x = 0.0f;
    config.jitter_y = 0.0f;
    if (hybrid && !converged && config.debugView == static_cast<Uint32>(DebugView::Final))
    {
        // +1 so frame 0 is not the degenerate Halton(0) = 0 corner. The period
        // is long enough to be invisible and keeps the radical inverse exact.
        // frameIndex, not accumulationFrame: the latter restarts on every camera
        // move, and a restarting jitter sequence would park every moving frame
        // on the same few sub-pixel offsets.
        const Uint32 index = (frameIndex % 1024) + 1;
        config.jitter_x = Halton(index, 2) - 0.5f;
        config.jitter_y = Halton(index, 3) - 0.5f;

        // Shift the image by -jitter pixels, so that the pixel centre samples
        // the point the compute shader's jittered ray passes through. ndc.x =
        // m00*x/w - m02, so adding to m02 moves the image left. Y has the
        // opposite sign because SDL's NDC is +Y up while pixel rows grow down.
        Mat4 proj = cameraMatrices.proj;
        proj.m[0][2] += config.jitter_x * 2.0f / static_cast<float>(config.width);
        proj.m[1][2] -= config.jitter_y * 2.0f / static_cast<float>(config.height);
        viewProj = proj * cameraMatrices.view;
    }

    SDL_GPUCommandBuffer *commandBuffer = SDL_AcquireGPUCommandBuffer(device);
    if (!commandBuffer)
    {
        SDL_Log("Failed to acquire command buffer: %s", SDL_GetError());
        return false;
    }

    // Rasterize first, in the same command buffer: SDL transitions the targets
    // from colour attachments to sampled textures before the compute pass.
    // A converged frame reads no G-buffer, so it does not need one drawn.
    if (hybrid && !converged)
    {
        RenderGBuffer(commandBuffer, viewProj);
    }

    // Reduced resolution applies to the lit image only. Debug views always
    // trace every pixel, so what they show is exactly what the G-buffer holds.
    const float traceScale =
        (hybrid && config.debugView == static_cast<Uint32>(DebugView::Final)) ? TraceScaleOf(traceResolution) : 1.0f;
    const bool reduced = traceScale < 1.0f;
    config.traceScale = traceScale;

    // One push serves both compute passes below.
    SDL_PushGPUComputeUniformData(commandBuffer, 0, &config, sizeof(config));

    // Pass 1: trace. Skipped below full resolution once converged - nothing new
    // is traced then, and re-grading the finished image is upsample.comp's job.
    if (!(reduced && converged))
    {
        // Read-write targets are bound when a pass begins. deferred.comp
        // declares all four whichever resolution it runs at; the compute-only
        // shader declares the first two and accumulates in place into the most
        // recent image.
        SDL_GPUStorageTextureReadWriteBinding traceTargets[4] = {};
        traceTargets[0].texture = accumTextures[writeSlot];
        traceTargets[1].texture = displayTexture;
        traceTargets[2].texture = lightingReduced;
        traceTargets[3].texture = historyGeometry[writeSlot];

        SDL_GPUComputePass *tracePass =
            SDL_BeginGPUComputePass(commandBuffer, traceTargets, hybrid ? 4 : 2, nullptr, 0);
        if (!tracePass)
        {
            SDL_Log("Failed to begin trace pass: %s", SDL_GetError());
            SDL_CancelGPUCommandBuffer(commandBuffer);
            return false;
        }

        SDL_BindGPUComputePipeline(tracePass, hybrid ? deferredPipeline : pipeline);

        // Storage buffer SLOTS are counted from 0 within their own kind,
        // whatever t-register they land on: this same array is t1..t7 in the
        // compute-only shader and t7..t13 in deferred.comp. Order must match.
        SDL_GPUBuffer *storageBuffers[7] = {
            buffers.objectBuffer, buffers.bvhBuffer, buffers.lightIDBuffer,
            buffers.vertexBuffer, buffers.indexBuffer, buffers.blasBuffer, buffers.instanceBuffer};
        SDL_BindGPUComputeStorageBuffers(tracePass, 0, storageBuffers, 7);

        // t0..t6 in deferred.comp, t0 alone in the compute-only shader. The
        // G-buffer is point-sampled, always: a LINEAR tap across a silhouette
        // invents a world position that lies on neither surface it blends. The
        // history is addressed texel by texel, so it is point-sampled too.
        SDL_GPUTextureSamplerBinding samplerBindings[7] = {};
        samplerBindings[0].texture = globalTextureArray;
        samplerBindings[0].sampler = linearSampler;
        samplerBindings[1].texture = gPosition;
        samplerBindings[1].sampler = pointSampler;
        samplerBindings[2].texture = gNormal;
        samplerBindings[2].sampler = pointSampler;
        samplerBindings[3].texture = gAlbedo;
        samplerBindings[3].sampler = pointSampler;
        samplerBindings[4].texture = gDepth;
        samplerBindings[4].sampler = pointSampler;
        samplerBindings[5].texture = accumTextures[readSlot];
        samplerBindings[5].sampler = pointSampler;
        samplerBindings[6].texture = historyGeometry[readSlot];
        samplerBindings[6].sampler = pointSampler;
        SDL_BindGPUComputeSamplers(tracePass, 0, samplerBindings, hybrid ? 7 : 1);

        // Group counts follow each shader's [numthreads]: THREADS x 1 for the
        // compute-only shader, 16 x 16 for deferred.comp - over the trace grid
        // rather than the frame when it is reduced.
        if (!hybrid)
        {
            SDL_DispatchGPUCompute(tracePass, (config.width + THREADS - 1) / THREADS, config.height, 1);
        }
        else
        {
            const Uint32 traceWidth = reduced ? ScaledExtent(config.width, traceScale) : config.width;
            const Uint32 traceHeight = reduced ? ScaledExtent(config.height, traceScale) : config.height;
            SDL_DispatchGPUCompute(tracePass, (traceWidth + 15) / 16, (traceHeight + 15) / 16, 1);
        }
        SDL_EndGPUComputePass(tracePass);
    }

    // Pass 2, reduced resolution only: upsample, accumulate and tone map at full
    // resolution. It has to be its own pass - lightingReduced was a write target
    // above and is a sampled input here.
    if (reduced)
    {
        SDL_GPUStorageTextureReadWriteBinding outputs[3] = {};
        outputs[0].texture = accumTextures[writeSlot];
        outputs[1].texture = displayTexture;
        outputs[2].texture = historyGeometry[writeSlot];

        SDL_GPUComputePass *upsamplePass = SDL_BeginGPUComputePass(commandBuffer, outputs, 3, nullptr, 0);
        if (!upsamplePass)
        {
            SDL_Log("Failed to begin upsample pass: %s", SDL_GetError());
            SDL_CancelGPUCommandBuffer(commandBuffer);
            return false;
        }

        SDL_BindGPUComputePipeline(upsamplePass, upsamplePipeline);

        // t0..t5 in upsample.comp. Point-sampled for the same reasons as above:
        // the upsample does its own edge-aware filtering, and the history is
        // addressed texel by texel.
        SDL_GPUTextureSamplerBinding inputs[6] = {};
        inputs[0].texture = gPosition;
        inputs[0].sampler = pointSampler;
        inputs[1].texture = gNormal;
        inputs[1].sampler = pointSampler;
        inputs[2].texture = gAlbedo;
        inputs[2].sampler = pointSampler;
        inputs[3].texture = lightingReduced;
        inputs[3].sampler = pointSampler;
        inputs[4].texture = accumTextures[readSlot];
        inputs[4].sampler = pointSampler;
        inputs[5].texture = historyGeometry[readSlot];
        inputs[5].sampler = pointSampler;
        SDL_BindGPUComputeSamplers(upsamplePass, 0, inputs, 6);

        // t6..t7 in upsample.comp: the object and instance records its motion
        // vectors are read from. Order must match.
        SDL_GPUBuffer *upsampleStorage[2] = {buffers.objectBuffer, buffers.instanceBuffer};
        SDL_BindGPUComputeStorageBuffers(upsamplePass, 0, upsampleStorage, 2);

        SDL_DispatchGPUCompute(upsamplePass, (config.width + 15) / 16, (config.height + 15) / 16, 1);
        SDL_EndGPUComputePass(upsamplePass);
    }

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
    // The display target, not the accumulation: the swapchain is SDR, so it needs
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

    // A converged frame added no samples, so it must not count as a frame - and
    // it wrote no history, so it must not consume a pending reset or move.
    if (!converged)
    {
        accumulationFrame++;
        frameIndex++;
        historyResetPending = false;
        cameraMovedPending = false;
        previousViewProj = cameraMatrices.viewProj;
    }
    if (writesHistory)
    {
        historyIndex = writeSlot;
    }
    return true;
}

void Renderer::Shutdown()
{
    nodes.clear();

    if (device)
    {
        const SDL_GPUTexture *const textures[] = {
            accumTextures[0], accumTextures[1], historyGeometry[0], historyGeometry[1],
            displayTexture, lightingReduced, globalTextureArray, gPosition, gNormal, gAlbedo, gDepth};
        for (const SDL_GPUTexture *t : textures)
        {
            if (t)
                SDL_ReleaseGPUTexture(device, const_cast<SDL_GPUTexture *>(t));
        }

        ReleaseSceneBuffers();

        if (pipeline)
            SDL_ReleaseGPUComputePipeline(device, pipeline);
        if (deferredPipeline)
            SDL_ReleaseGPUComputePipeline(device, deferredPipeline);
        if (upsamplePipeline)
            SDL_ReleaseGPUComputePipeline(device, upsamplePipeline);
        if (gbufferPipeline)
            SDL_ReleaseGPUGraphicsPipeline(device, gbufferPipeline);
        if (gbufferPipelineTwoSided)
            SDL_ReleaseGPUGraphicsPipeline(device, gbufferPipelineTwoSided);
        if (linearSampler)
            SDL_ReleaseGPUSampler(device, linearSampler);
        if (pointSampler)
            SDL_ReleaseGPUSampler(device, pointSampler);
        if (window)
            SDL_ReleaseWindowFromGPUDevice(device, window);

        SDL_DestroyGPUDevice(device);
    }

    // Outside the device check. Initialize can fail AFTER the window exists -
    // device creation is the usual culprit - and returning early here used to
    // leave that window and SDL itself initialised (guide section 11.3).
    if (window)
        SDL_DestroyWindow(window);
    SDL_Quit();

    device = nullptr;
    window = nullptr;
    pipeline = nullptr;
    deferredPipeline = nullptr;
    upsamplePipeline = nullptr;
    gbufferPipeline = nullptr;
    gbufferPipelineTwoSided = nullptr;
    linearSampler = nullptr;
    pointSampler = nullptr;
    accumTextures[0] = accumTextures[1] = nullptr;
    historyGeometry[0] = historyGeometry[1] = nullptr;
    displayTexture = nullptr;
    lightingReduced = nullptr;
    globalTextureArray = nullptr;
    gPosition = nullptr;
    gNormal = nullptr;
    gAlbedo = nullptr;
    gDepth = nullptr;
}
