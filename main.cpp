#include <SDL3/SDL.h>
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

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
#include "scene.h"
#include "texture.h"

#define THREADS 256
#define TARGET_FPS 60.0

enum RenderType
{
    Path_Tracing = 0,
    Ray_Tracing = 1,
};

// Upper bound on BVH nodes for a given object count. BuildRecursive splits
// until a node holds at most BVH_LEAF_SIZE objects, so leaves never outnumber
// the objects and interior nodes never outnumber the leaves.
static constexpr Uint32 MaxNodesFor(Uint32 objectCount) { return objectCount * 2 + 1; }

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

SDL_GPUDevice *device;

SDL_GPUComputePipeline *CreatePathTraceComputePipeline(SDL_GPUDevice *device)
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

SDL_GPUComputePipeline *CreateRayTraceComputePipeline(SDL_GPUDevice *device)
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
    SDL_GPUComputePipeline *pipeline = SDL_CreateGPUComputePipeline(device, &cpci);
    SDL_free(data);
    if (!pipeline)
    {
        SDL_Log("Failed to create compute pipeline: %s", SDL_GetError());
        return nullptr;
    }
    return pipeline;
}

// The classic Cornell box: two coloured side walls, three white ones, a ceiling
// light, and two boxes. Lit entirely by the ceiling light against a black sky.
void BuildCornellBox(Scene &scene)
{
    scene.camera.position = point3(278.0f, 278.0f, -800.0f);
    scene.camera.target = point3(278.0f, 278.0f, 0.0f);
    scene.camera.fov = 40.0f;
    scene.camera.focus_dist = 800.0f;
    scene.camera.sky = Color(0.0f, 0.0f, 0.0f);
    scene.camera.horizon = Color(0.0f, 0.0f, 0.0f);
    scene.maxDepth = 2;

    const Material white = Material::Lambertian(Color(0.73f, 0.73f, 0.73f));
    const Material red = Material::Lambertian(Color(0.65f, 0.05f, 0.05f));
    const Material green = Material::Lambertian(Color(0.12f, 0.45f, 0.15f));
    const Material light = Material::Emissive(Color(1.0f, 1.0f, 1.0f), 65.0f);
    const Material redLight = Material::Emissive(Color(0.65f, 0.65f, 0.65f), 10.0f);
    const Material volume = Material::Volume(Color(0.22f, 0.22f, 0.1f), 0.3f);

    // Walls
    scene.AddQuad(point3(555.0f, 0.0f, 0.0f), Vec3(0.0f, 555.0f, 0.0f), Vec3(0.0f, 0.0f, 555.0f), green);       // left
    scene.AddQuad(point3(0.0f, 0.0f, 0.0f), Vec3(0.0f, 555.0f, 0.0f), Vec3(0.0f, 0.0f, 555.0f), red);           // right
    scene.AddQuad(point3(0.0f, 0.0f, 0.0f), Vec3(555.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, 555.0f), white);         // floor
    scene.AddQuad(point3(555.0f, 555.0f, 555.0f), Vec3(-555.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, -555.0f), white); // ceiling
    scene.AddQuad(point3(0.0f, 0.0f, 555.0f), Vec3(555.0f, 0.0f, 0.0f), Vec3(0.0f, 555.0f, 0.0f), white);       // back

    // Ceiling light, just below the ceiling so it isn't coplanar with it.
    scene.AddQuad(point3(343.0f, 554.0f, 332.0f), Vec3(-130.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, -105.0f), light);

    scene.AddSphere(point3(350.0f, 150.0f, 50.0f), 50.0f, redLight);

    // Both boxes are built axis-aligned at the origin corner, then rotated as
    // rigid bodies and translated into place - the canonical Cornell framing.
    scene.AddBox(point3(0.0f, 0.0f, 0.0f), point3(165.0f, 330.0f, 165.0f), volume)
        .RotateY(static_cast<float>(degrees_to_radians(15.0)))
        .Translate(Vec3(265.0f, 0.0f, 295.0f));

    scene.AddBox(point3(0.0f, 0.0f, 0.0f), point3(165.0f, 165.0f, 165.0f), volume)
        .RotateY(static_cast<float>(degrees_to_radians(-18.0)))
        .Translate(Vec3(130.0f, 0.0f, 65.0f));
}

// A deliberately small scene for reading what `density` actually does. Three
// identical fog spheres sit in a row over a lit floor, an order of magnitude
// apart in density, with a solid red sphere buried in each so you can see how
// far into the medium you can still see.
//
// Density is per world unit: the chance of crossing distance d without
// scattering is exp(-density * d), so mean free path is 1/density. At the 2.0
// radius here that's 10 units for the thin one (barely visible), 1 unit for the
// middle one (smoke), and 0.1 for the thick one (nearly opaque).
void BuildFogTest(Scene &scene)
{
    scene.camera.position = point3(0.0f, 3.0f, -14.0f);
    scene.camera.target = point3(0.0f, 1.0f, 0.0f);
    scene.camera.fov = 40.0f;
    scene.camera.focus_dist = 14.0f;
    scene.camera.sky = Color(0.02f, 0.03f, 0.05f);
    scene.camera.horizon = Color(0.01f, 0.01f, 0.02f);
    scene.maxDepth = 40; // dense fog: many scatters before a path finds the light

    const Material floor = Material::Lambertian(Color(0.6f, 0.6f, 0.6f));
    const Material marker = Material::Lambertian(Color(0.9f, 0.1f, 0.1f));

    scene.AddQuad(point3(-20.0f, 0.0f, -20.0f),
                  Vec3(40.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, 40.0f), floor);

    // Two overhead lights, so the fog is side-lit and its depth reads.
    scene.AddQuad(point3(-6.0f, 9.0f, -3.0f),
                  Vec3(12.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, 6.0f),
                  Material::Emissive(Color(1.0f, 0.95f, 0.9f), 6.0f));

    const float densities[3] = {0.1f, 1.0f, 10.0f};
    for (int i = 0; i < 3; i++)
    {
        const float x = (i - 1) * 5.5f;

        // The marker sits at the centre of the fog ball.
        scene.AddSphere(point3(x, 2.0f, 0.0f), 0.6f, marker);
        scene.AddVolume(point3(x, 2.0f, 0.0f), 2.0f, Color(0.9f, 0.9f, 0.95f), densities[i]);
    }

    // A fog-filled box on the right, to check the Box boundary path as well as
    // the sphere one - they take different span code in the shader.
    scene.AddSphere(point3(9.0f, 1.2f, 3.0f), 0.6f, marker);
    scene.AddVolume(point3(7.0f, 0.0f, 1.0f), point3(11.0f, 3.0f, 5.0f),
                    Color(0.35f, 0.55f, 0.95f), 1.0f)
        .RotateY(static_cast<float>(degrees_to_radians(20.0)));

    // A solid box on the left for reference, same size, same primitive.
    scene.AddBox(point3(-11.0f, 0.0f, 1.0f), point3(-7.0f, 3.0f, 5.0f),
                 Material::Lambertian(Color(0.3f, 0.7f, 0.4f)))
        .RotateY(static_cast<float>(degrees_to_radians(-20.0)));
}

// Ray Tracing In One Weekend's "final scene": a big ground sphere, three hero
// spheres showing off each material type, and a field of small random ones.
// Lit by a blue sky gradient rather than by any emissive geometry.
void BuildFinalScene(Scene &scene)
{
    scene.camera.position = point3(13.0f, 2.0f, 3.0f);
    scene.camera.target = point3(0.0f, 0.0f, 0.0f);
    scene.camera.fov = 20.0f;
    scene.camera.focus_dist = 10.0f;
    scene.camera.defocus_angle = 0.6f;
    scene.camera.sky = Color(0.5f, 0.7f, 1.0f);
    scene.camera.horizon = Color(1.0f, 1.0f, 1.0f);

    scene.maxDepth = 5;

    const Texture *brick = scene.textures.Load("brick/textures/red_brick_diff_4k.jpg");

    scene.AddSphere(point3(10.0f, 10.0f,0.0f), 5.0f,Material::Emissive(Color(0.54f,0.67f,0.3f),1.0f));

    scene.AddSphere(
        point3(0.0f, -1000.0f, 0.0f), 1000.0f,
        Material::Lambertian(Color(0.5f, 0.5f, 0.5f)));

    // The three heroes: glass, matte (brick-textured), and metal.
    scene.AddSphere(point3(0.0f, 1.0f, 0.0f), 1.0f, Material::Dielectric(1.5f));

    // Rotating a sphere can't change its shape, so RotateY only turns the
    // texture on it - which is exactly what you want for aiming a brick seam.
    scene.AddSphere(
             point3(-4.0f, 1.0f, 0.0f), 1.0f,
             Material::Lambertian(Color(1.0f, 1.0f, 1.0f)).Textured(brick))
        .RotateY(static_cast<float>(degrees_to_radians(90.0)));
    scene.AddSphere(point3(4.0f, 1.0f, 0.0f), 1.0f, Material::Metal(Color(0.7f, 0.6f, 0.5f)));

    SDL_srand(0);
    for (int a = -5; a < 5; a++)
    {
        for (int b = -5; b < 5; b++)
        {
            const point3 position(a + 0.9f * SDL_randf(), 0.2f, b + 0.9f * SDL_randf());
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

// Ray Tracing: The Next Week's final scene - the one that puts every feature in
// the book on screen at once. Two of its elements are what volumes were added
// for:
//
//   * the blue "subsurface" ball: a glass sphere with a dense medium inside it,
//     so light refracts in, scatters around, and refracts back out
//   * the global haze: a 5000-unit sphere of extremely thin white medium
//     wrapped around the whole scene, which is what softens the background
//
// Two substitutions from the book, since the engine has no procedural textures
// yet: the earth-mapped sphere uses the brick texture, and the Perlin-noise
// sphere is a plain matte one. Motion blur on the orange sphere is also
// skipped - Object_GPU carries a Position2 for it, but nothing on the CPU side
// sets it yet.
void BuildTestAllFeatureScene(Scene &scene)
{
    scene.camera.position = point3(478.0f, 278.0f, -600.0f);
    scene.camera.target = point3(278.0f, 278.0f, 0.0f);
    scene.camera.fov = 120.0f;
    scene.camera.focus_dist = 600.0f;
    scene.camera.sky = Color(0.0f, 0.0f, 0.0f);
    scene.camera.horizon = Color(0.0f, 0.0f, 0.0f);
    // Russian roulette (starting at bounce 1 in the shader) does the actual
    // termination work now, so this just needs to be a safe upper bound for
    // deep mirror/glass chains plus a few GI bounces - 30 was sized for the
    // old "always bounce to Depth" behavior and is unnecessary cost now.
    scene.maxDepth = 3;

    const Texture *brick = scene.textures.Load("brick/textures/red_brick_diff_4k.jpg");

    // --- Ground: a 20x20 grid of boxes at random heights.
    const Material ground = Material::Lambertian(Color(0.48f, 0.83f, 0.53f));
    SDL_srand(0);
    const int boxesPerSide = 20;
    for (int i = 0; i < boxesPerSide; i++)
    {
        for (int j = 0; j < boxesPerSide; j++)
        {
            const float w = 100.0f;
            const float x0 = -1000.0f + i * w;
            const float z0 = -1000.0f + j * w;
            const float y1 = 1.0f + SDL_randf() * 100.0f;
            scene.AddBox(point3(x0, 0.0f, z0), point3(x0 + w, y1, z0 + w), ground);
        }
    }

    // --- The overhead light.
    scene.AddQuad(point3(123.0f, 554.0f, 147.0f),
                  Vec3(300.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, 265.0f),
                  Material::Emissive(Color(1.0f, 1.0f, 1.0f), 7.0f));

    // --- Hero spheres.
    // The book gives this one motion blur; static here (see note above).
    scene.AddSphere(point3(400.0f, 400.0f, 200.0f), 50.0f,
                    Material::Lambertian(Color(0.7f, 0.3f, 0.1f)));

    scene.AddSphere(point3(260.0f, 150.0f, 45.0f), 50.0f, Material::Dielectric(1.5f));

    scene.AddSphere(point3(0.0f, 150.0f, 145.0f), 50.0f,
                    Material::Metal(Color(0.8f, 0.8f, 0.9f), 1.0f));

    // --- Subsurface scattering: a glass shell with a dense blue medium inside.
    // Both spheres share a centre and radius - the dielectric is the visible
    // surface, the volume is what fills it.
    scene.AddSphere(point3(360.0f, 150.0f, 145.0f), 70.0f, Material::Dielectric(1.5f));
    scene.AddVolume(point3(360.0f, 150.0f, 145.0f), 70.0f, Color(0.2f, 0.4f, 0.9f), 0.2f);

    // --- Global haze. Very low density over a very large radius: individually
    // each ray rarely scatters, but across the whole scene it lifts the blacks
    // and softens everything in the distance.
    scene.AddVolume(point3(0.0f, 0.0f, 0.0f), 5000.0f, Color(1.0f, 1.0f, 1.0f), 0.0001f);

    // --- Textured sphere (earth map in the book).
    scene.AddSphere(point3(400.0f, 200.0f, 400.0f), 100.0f,
                    Material::Lambertian(Color(1.0f, 1.0f, 1.0f)).Textured(brick));

    // --- Perlin-noise sphere in the book; plain matte here.
    scene.AddSphere(point3(220.0f, 280.0f, 300.0f), 80.0f,
                    Material::Lambertian(Color(0.7f, 0.7f, 0.75f)));

    // --- A cluster of 1000 small spheres, built at the origin then rotated and
    // translated into place as one rigid body.
    const Material clusterWhite = Material::Lambertian(Color(0.73f, 0.73f, 0.73f));
    const size_t clusterFirst = scene.Count();
    for (int j = 0; j < 1000; j++)
    {
        scene.AddSphere(
            point3(165.0f * SDL_randf(), 165.0f * SDL_randf(), 165.0f * SDL_randf()),
            10.0f, clusterWhite);
    }
    scene.GroupSince(clusterFirst)
        .RotateY(static_cast<float>(degrees_to_radians(15.0)))
        .Translate(Vec3(-100.0f, 270.0f, 395.0f));
}

std::vector<uint32_t> LightIndices(const std::vector<Object *> &orderedObjects)
{
    std::vector<uint32_t> lights;
    for (uint32_t i = 0; i < orderedObjects.size(); i++)
        if (orderedObjects[i]->mat.type == MaterialType::DiffuseLight)
            lights.push_back(i);
    return lights;
}

void StepPhysics(std::vector<std::unique_ptr<Object>> &objects,
                 const std::vector<std::unique_ptr<Object>> &restPose,
                 float time)
{
    (void)objects;
    (void)restPose;
    (void)time;
}
// Allocates GPU-resident storage buffers plus matching upload transfer
// buffers, sized for the worst case so no reallocation is needed as the
// BVH shape changes frame to frame.
SceneBuffers CreateSceneBuffers(SDL_GPUDevice *device, Uint32 maxObjects, Uint32 maxNodes, Uint32 maxLights)
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

std::vector<Object_GPU> ObjectsToGPUObjects(const std::vector<Object *> &objects)
{
    std::vector<Object_GPU> list;
    list.reserve(objects.size());

    for (Object *obj : objects)
    {
        list.push_back(obj->CreateObjectGPU());
    }

    return list;
}

std::vector<BVHNode_GPU> NodesToGPUNodes(const std::vector<BVH_node *> &nodes)
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
bool UploadScene(
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

uint32_t CountLights(const std::vector<std::unique_ptr<Object>> &objects)
{
    return static_cast<uint32_t>(std::count_if(objects.begin(), objects.end(),
                                               [](const auto &o)
                                               { return o->mat.type == MaterialType::DiffuseLight; }));
}

int main()
{
    // Render settings. Camera framing and sky come from the scene itself and
    // are filled in below, once it's built.
    Config config = {};
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
        return 1;
    }
    SDL_Window *window = SDL_CreateWindow("", config.width, config.height, 0);
    if (!window)
    {
        SDL_Log("Failed to create window: %s", SDL_GetError());
        return 1;
    }
    device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL, true, nullptr);
    if (!device)
    {
        SDL_Log("Failed to create GPU device: %s", SDL_GetError());
        return 1;
    }

    SDL_Log("Backend: %s", SDL_GetGPUDeviceDriver(device));

    if (!SDL_ClaimWindowForGPUDevice(device, window))
    {
        SDL_Log("Failed to claim window for GPU device: %s", SDL_GetError());
        return 1;
    }
    SDL_RaiseWindow(window);
    SDL_GPUComputePipeline *pipeline = CreatePathTraceComputePipeline(device);
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

    SDL_GPUTexture *texture1 = SDL_CreateGPUTexture(device, &textureInfo);
    if (!texture1)
    {
        SDL_Log("Failed to create texture: %s", SDL_GetError());
        return 1;
    }

    // The scene owns both the geometry and its textures, so ids are already
    // assigned by the time the objects exist - no separate registration pass.
    // Swapping scenes is a one-liner: each builder sets up its own geometry,
    // textures, camera framing and sky.
    Scene scene;
    // Pick one: BuildCornellBox / BuildFogTest / BuildFinalScene / BuildNextWeekFinalScene
    BuildTestAllFeatureScene(scene);
    SDL_Log("Scene: %zu objects, %zu texture(s)", scene.Count(), scene.textures.Count());

    config.sky_r = scene.camera.sky.x;
    config.sky_g = scene.camera.sky.y;
    config.sky_b = scene.camera.sky.z;
    config.horizon_r = scene.camera.horizon.x;
    config.horizon_g = scene.camera.horizon.y;
    config.horizon_b = scene.camera.horizon.z;
    config.depth = scene.maxDepth;

    // The scene's own objects are the immutable rest pose; physics mutates the
    // clone, and each frame's BVH is built over the clone.
    std::vector<std::unique_ptr<Object>> liveObjects = scene.CloneObjects();

    std::vector<BVH_node *> nodes;
    std::vector<Object *> orderedObjects;
    std::vector<uint32_t> lightIDs;

    SDL_GPUTexture *globalTextureArray = scene.textures.BuildGPUArray(device);
    if (!globalTextureArray)
    {
        SDL_Log("Failed to build the scene texture array");
        return 1;
    }

    SDL_Log("what");

    // Sized from the scene that was actually built, so adding geometry can't
    // quietly overrun a buffer dimensioned for some other scene.
    const Uint32 maxObjects = static_cast<Uint32>(scene.Count());
    const Uint32 maxLights = CountLights(liveObjects);
    SceneBuffers sceneBuffers = CreateSceneBuffers(device, maxObjects, MaxNodesFor(maxObjects), maxLights);
    if (!sceneBuffers.objectBuffer || !sceneBuffers.bvhBuffer)
    {
        SDL_Log("Failed to create scene buffers");
        return 1;
    }

    // --- Camera setup: derive an initial yaw/pitch that reproduces the scene's
    // authored framing, then hand control over to mouse-look + WASD from here on.
    const Vec3<float> initialForward =
        normalize(scene.camera.target - scene.camera.position);

    scene.camera.yaw = SDL_atan2f(initialForward.z, initialForward.x);
    scene.camera.pitch =  SDL_asinf(initialForward.y);
    scene.camera.fov;
    scene.camera.focus_dist;
    scene.camera.defocus_angle;

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
    size_t loggedNodeCount = static_cast<size_t>(-1);

    bool sceneDirty = true; // becomes true again once StepPhysics does real work

    BuildBVH(liveObjects, nodes, orderedObjects);
    lightIDs = LightIndices(orderedObjects);
    config.num_spheres = static_cast<Uint32>(orderedObjects.size());
    config.num_lights = static_cast<Uint32>(lightIDs.size());
    UploadScene(sceneBuffers, ObjectsToGPUObjects(orderedObjects), NodesToGPUNodes(nodes), lightIDs);

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

        Vec3<float> oldCameraPosition = scene.camera.position;
        float oldYaw = scene.camera.yaw;
        float oldPitch = scene.camera.pitch;
        if (mouseCaptured)
        {
            scene.camera.Update(keys, mouseDX, mouseDY, dt);
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
    SDL_Quit();

    return 0;
}