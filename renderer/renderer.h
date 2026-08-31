#ifndef RENDERER_H
#define RENDERER_H

#include <SDL3/SDL.h>
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>
#include <entt/entt.hpp>

#include <algorithm>
#include <cstddef>
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
#define TARGET_FPS 60.0

enum RenderType
{
    Path_Tracing = 0,
    Ray_Tracing = 1,
};
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
    float padding1[2];
};

struct SceneBuffers
{
    SDL_GPUBuffer *objectBuffer;
    SDL_GPUBuffer *bvhBuffer;
    SDL_GPUBuffer *lightIDBuffer;
    SDL_GPUTransferBuffer *objectTransfer;
    SDL_GPUTransferBuffer *bvhTransfer;
    SDL_GPUTransferBuffer *lightIDTransfer;
    Uint32 maxObjects;
    Uint32 maxNodes;
    Uint32 maxLights;
};

class Renderer
{
public:
    void Initialize();
    void Render(float deltaTime, float currentTime, entt::registry &registry);
    SDL_GPUComputePipeline *CreatePathTraceComputePipeline(SDL_GPUDevice *device);

    SDL_GPUComputePipeline *CreateRayTraceComputePipeline(SDL_GPUDevice *device);

    // Upper bound on BVH nodes for a given object count. BuildRecursive splits
    // until a node holds at most BVH_LEAF_SIZE objects, so leaves never outnumber
    // the objects and interior nodes never outnumber the leaves.
    static constexpr Uint32 MaxNodesFor(Uint32 objectCount) { return objectCount * 2 + 1; }

    std::vector<uint32_t> LightIndices(const std::vector<Object *> &orderedObjects);

    // Allocates GPU-resident storage buffers plus matching upload transfer
    // buffers, sized for the worst case so no reallocation is needed as the
    // BVH shape changes frame to frame.
    SceneBuffers CreateSceneBuffers(SDL_GPUDevice *device, Uint32 maxObjects, Uint32 maxNodes, Uint32 maxLights);


    std::vector<Object_GPU> ObjectsToGPUObjects(const std::vector<Object *> &objects);

    std::vector<BVHNode_GPU> NodesToGPUNodes(const std::vector<BVH_node *> &nodes);

    // Re-maps the persistent transfer buffers and re-uploads them into the
    // persistent GPU buffers. `cycle = true` lets SDL_gpu double-buffer this
    // resource internally instead of stalling on the previous frame's usage.
    bool UploadScene(
        SceneBuffers &sb,
        const std::vector<Object_GPU> &objects,
        const std::vector<BVHNode_GPU> &nodes,
        const std::vector<uint32_t> &lightIDs);

    uint32_t CountLights(const std::vector<std::unique_ptr<Object>> &objects);

    void TerminateRenderer();

    SDL_GPUDevice *device;
    SDL_GPUTexture *texture1;
    SDL_GPUTexture *globalTextureArray;
    SceneBuffers sceneBuffers;
    SDL_GPUComputePipeline *pipeline;
    SDL_GPUSampler *linearSampler;
    SDL_Window *window;
    Config config;

}

#endif