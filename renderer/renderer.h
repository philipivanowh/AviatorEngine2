#ifndef RENDERER_H
#define RENDERER_H

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <entt/entt.hpp>

#include "scene/aabb.h"
#include "scene/bvh.h"
#include "scene/gpu_types.h"
#include "scene/scene.h"

#define THREADS 256

// Everything GPU-side lives behind this class: the device, the window it draws
// into, the compute pipeline, the persistent scene buffers, and the per-frame
// gather -> BVH -> upload -> dispatch -> blit sequence.
//
// The split against main.cpp is by ownership, not by convenience: the Renderer
// owns GPU resources and the uniform block, and reads the registry; it never
// writes components. Input, camera control and physics stay outside, so the
// only thing the render loop can do to the simulation is read it.

enum RenderType
{
    Path_Tracing = 0,
    Ray_Tracing = 1,
};

// Mirrors the cbuffer at the top of the compute shader byte for byte. The
// padding fields are load-bearing: HLSL packs constant buffers into float4
// rows and will not split a field across a row boundary.
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
    float padding[3];
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
    Uint32 renderType;
    Uint32 num_lights;

    // Scales linear radiance before tone mapping. 1.0 is neutral; lower it
    // when the scene's lights push everything into the top of the curve.
    float exposure;
    float padding1;
};

// GPU-resident storage plus the matching upload transfer buffers, sized once
// for the worst case so nothing reallocates as the BVH changes shape frame to
// frame.
struct SceneBuffers
{
    SDL_GPUBuffer *objectBuffer = nullptr;
    SDL_GPUBuffer *bvhBuffer = nullptr;
    SDL_GPUBuffer *lightIDBuffer = nullptr;
    SDL_GPUTransferBuffer *objectTransfer = nullptr;
    SDL_GPUTransferBuffer *bvhTransfer = nullptr;
    SDL_GPUTransferBuffer *lightIDTransfer = nullptr;

    // Mesh geometry. The first three are written once at load - geometry is
    // local-space and shared, so nothing about it changes when an instance
    // moves - while instances are re-uploaded with the BVH every rebuild.
    SDL_GPUBuffer *vertexBuffer = nullptr;
    SDL_GPUBuffer *indexBuffer = nullptr;
    SDL_GPUBuffer *blasBuffer = nullptr;
    SDL_GPUBuffer *instanceBuffer = nullptr;
    SDL_GPUTransferBuffer *vertexTransfer = nullptr;
    SDL_GPUTransferBuffer *indexTransfer = nullptr;
    SDL_GPUTransferBuffer *blasTransfer = nullptr;
    SDL_GPUTransferBuffer *instanceTransfer = nullptr;

    Uint32 maxObjects = 0;
    Uint32 maxNodes = 0;
    Uint32 maxLights = 0;
    Uint32 maxVertices = 0;
    Uint32 maxIndices = 0;
    Uint32 maxBLASNodes = 0;
    Uint32 maxInstances = 0;
};

// What the caller chooses before the window exists. Everything else is derived
// from the scene.
struct RendererSettings
{
    Uint32 width = 1920;
    Uint32 height = 1080;
    Uint32 samples = 1; // samples per pixel per frame

    // Which compute shader to load, without the format suffix. Distinct from
    // renderType: ray_trace.comp implements *both* integrators and branches on
    // the renderType uniform internally, so this picks the file and renderType
    // picks the algorithm inside it.
    const char *shader = "ray_trace.comp.hybrid";
    RenderType renderType = RenderType::Path_Tracing;

    const char *title = "AviatorEngine";
};

class Renderer
{
public:
    bool Initialize(const RendererSettings &settings);

    // Uploads the scene's texture array, sizes the GPU buffers from what the
    // registry actually holds, and does the first BVH build. Call once, after
    // the scene builder has run.
    bool LoadScene(Scene &scene, entt::registry &registry);

    // Pulls camera framing, sky and depth out of the registry and the scene.
    // Separate from LoadScene because the sky and max depth are authored by the
    // scene builder, while position and orientation change every frame.
    void SyncCamera(const entt::registry &registry, entt::entity camera);
    void SyncSceneSettings(const Scene &scene, const entt::registry &registry, entt::entity camera);

    // Rebuild the BVH and re-upload before the next dispatch. Anything that
    // moves geometry has to call this.
    void MarkSceneDirty() { sceneDirty = true; }

    // Progressive accumulation: frame N blends with the N-1 frames before it.
    // Any change to the image - camera motion, geometry motion - has to restart
    // it, or moving content smears across the average.
    void ResetAccumulation() { accumulationFrame = 0; }

    // Exposure multiplier applied to linear radiance before the tone curve.
    // Purely a display control - it does not touch the accumulation buffer, so
    // changing it re-grades the existing image without restarting convergence.
    float Exposure() const { return config.exposure; }
    void SetExposure(float value) { config.exposure = std::max(value, 0.01f); }
    Uint32 AccumulatedSamples() const { return (accumulationFrame + 1) * config.samples; }

    // One frame: rebuild if dirty, dispatch the compute pass, blit to the
    // swapchain. Returns false when the frame could not be submitted.
    bool RenderFrame(const entt::registry &registry);

    void Shutdown();

    SDL_Window *Window() const { return window; }
    SDL_GPUDevice *Device() const { return device; }
    const char *DriverName() const { return SDL_GetGPUDeviceDriver(device); }

    size_t NodeCount() const { return nodes.size(); }
    size_t ObjectCount() const { return orderedEntities.size(); }

private:
    // Upper bound on BVH nodes for a given object count. BuildRecursive splits
    // until a node holds at most BVH_LEAF_SIZE objects, so leaves never
    // outnumber the objects and interior nodes never outnumber the leaves.
    static constexpr Uint32 MaxNodesFor(Uint32 objectCount) { return objectCount * 2 + 1; }

    SDL_GPUComputePipeline *CreateComputePipeline(const char *stem);

    bool CreateSceneBuffers(Uint32 maxObjects, Uint32 maxNodes, Uint32 maxLights);
    void ReleaseSceneBuffers();

    // Uploads the mesh library's local-space geometry. Called once from
    // LoadScene: vertices, indices and BLAS nodes never change after that,
    // because instancing is precisely the trick of not touching geometry when
    // an object moves.
    bool UploadMeshGeometry(const MeshLibrary &library);

    // Re-maps the persistent transfer buffers and re-uploads them into the
    // persistent GPU buffers. `cycle = true` lets SDL_gpu double-buffer the
    // resource internally instead of stalling on the previous frame's use.
    bool UploadScene(const std::vector<Object_GPU> &objects,
                     const std::vector<BVHNode_GPU> &nodes,
                     const std::vector<uint32_t> &lightIDs,
                     const std::vector<MeshInstance_GPU> &instances);

    void RebuildAcceleration(const entt::registry &registry);

    // Indices into the *ordered* entity list, i.e. into the object buffer the
    // shader sees - not entity ids. The BVH decides that ordering, so this runs
    // after every build.
    static std::vector<uint32_t> LightIndices(const entt::registry &registry,
                                              const std::vector<entt::entity> &ordered);

    // Packs the object buffer and, as a side effect, the instance buffer: a
    // mesh entity emits one of each, and the object's instanceIndex is the slot
    // the instance landed in. They are built together because only this pass
    // knows how many instances have been emitted so far.
    static std::vector<Object_GPU> PackObjects(const entt::registry &registry,
                                               const std::vector<entt::entity> &ordered,
                                               const MeshLibrary &library,
                                               std::vector<MeshInstance_GPU> &outInstances);
    static uint32_t CountLights(const entt::registry &registry);

    SDL_Window *window = nullptr;
    SDL_GPUDevice *device = nullptr;
    SDL_GPUComputePipeline *pipeline = nullptr;
    SDL_GPUSampler *linearSampler = nullptr;
    // Linear HDR accumulation. Averaging only works in linear space, so this
    // texture must never hold tone-mapped values.
    SDL_GPUTexture *accumTexture = nullptr;

    // What actually reaches the screen: the accumulation tone-mapped and
    // sRGB-encoded. Kept separate so the tone curve is applied once at display
    // time rather than being folded back into next frame's running average.
    SDL_GPUTexture *displayTexture = nullptr;
    SDL_GPUTexture *globalTextureArray = nullptr;

    SceneBuffers buffers;
    Config config = {};

    // Per-frame scratch, kept alive across frames so the vectors keep their
    // capacity instead of reallocating 60 times a second.
    std::vector<entt::entity> renderables;
    std::vector<AABB> renderableBounds;
    std::vector<BVHNode_GPU> nodes;
    std::vector<uint32_t> buildOrder; // BVH slot -> index into `renderables`
    std::vector<entt::entity> orderedEntities;
    std::vector<uint32_t> lightIDs;
    std::vector<MeshInstance_GPU> instances;

    // Borrowed from the Scene at LoadScene time; the Scene outlives the frame
    // loop, so this is a non-owning view of its mesh library.
    const MeshLibrary *meshLibrary = nullptr;

    bool sceneDirty = true;
    Uint32 accumulationFrame = 0;
};

#endif
