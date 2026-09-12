#ifndef RENDERER_H
#define RENDERER_H

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>
#include <entt/entt.hpp>

#include "math/mat4.h"
#include "scene/aabb.h"
#include "scene/bvh.h"
#include "scene/gpu_types.h"
#include "scene/scene.h"

#define THREADS 256

// Everything GPU-side lives behind this class: the device, the window it draws
// into, the pipelines, the persistent scene buffers, and the per-frame
// gather -> BVH -> upload -> raster -> dispatch -> blit sequence.
//
// The split against main.cpp is by ownership, not by convenience: the Renderer
// owns GPU resources and the uniform block, and reads the registry; it never
// writes components. Input, camera control and physics stay outside, so the
// only thing the render loop can do to the simulation is read it.
//
// Two ways to find the primary hit, switchable at runtime (SetHybrid):
//
//   hybrid     rasterize mesh instances into a G-buffer, then deferred.comp
//              resolves analytic primitives against it and ray traces
//              everything after the first hit (docs/hybrid_renderer_guide.md)
//   compute    the original path: `shader` traces primary rays too

enum RenderType
{
    Path_Tracing = 0,
    Ray_Tracing = 1,
};

// What deferred.comp writes to the screen. Values are uploaded as
// Config::debugView and must match the DEBUG_* defines in deferred.comp.hlsl.
// Only the hybrid renderer draws these - the compute-only shaders ignore it.
enum class DebugView : Uint32
{
    Final = 0,      // the lit, accumulated image
    Albedo = 1,     // gAlbedo as rasterized
    Normal = 2,     // gNormal as rasterized, n * 0.5 + 0.5
    Position = 3,   // gPosition, fract(p / 100) so it bands visibly
    Material = 4,   // resolved hit's ColorType in false colour (guide section 10)
    Visibility = 5, // who won primary visibility: raster green, analytic red, sky blue
    DepthError = 6, // |WorldFromDepth - gPosition|, relative, x100 (guide section 9.1)
    Count
};

inline const char *DebugViewName(DebugView view)
{
    switch (view)
    {
    case DebugView::Final: return "final";
    case DebugView::Albedo: return "albedo";
    case DebugView::Normal: return "normal";
    case DebugView::Position: return "position";
    case DebugView::Material: return "material";
    case DebugView::Visibility: return "visibility";
    case DebugView::DepthError: return "depth error";
    default: return "?";
    }
}

// Mirrors the cbuffer at the top of the compute shaders byte for byte. The
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

    // --- Hybrid only. The compute-only shaders end their cbuffer at a padding
    // float in debugView's slot and never read past it, so appending here is
    // invisible to them.
    Uint32 debugView; // DebugView
    float zNear;      // PerspectiveRH_ZO planes, for depth reconstruction
    float zFar;

    // This frame's sub-pixel sample position, in pixels. The SAME offset is
    // baked into the raster projection in RenderFrame; the deferred shader adds
    // it to its primary ray. One without the other and the rays and the
    // G-buffer describe points half a pixel apart.
    float jitter_x;
    float jitter_y;

    // 1 once the image has RendererSettings::maxSamples: the shader traces
    // nothing and only re-tone-maps the finished accumulation, so exposure
    // changes still show on a converged frame.
    Uint32 displayOnly;

    // Per-axis scale of the grid deferred.comp traces this frame. 1 traces every
    // pixel straight into the image; below 1 it traces into lightingReduced and
    // upsample.comp rebuilds the full-res frame. See TraceResolution.
    float traceScale;

    // Temporal reprojection (ReprojectHistory in deferred.comp / upsample.comp).
    // frameIndex never resets: seeds, jitter and the traced-texel pattern need
    // new values every frame, moving or not. `batch` above keeps its meaning -
    // frames since the image restarted - for the compute-only shader, which
    // still restarts whenever the camera moves.
    Uint32 frameIndex;
    Uint32 historyReset; // 1 = ignore every pixel's history this frame
    Uint32 cameraMoved;  // 1 = reproject through prevViewProj
    float historyCap;    // RendererSettings::historyWhileMoving
    float padding3;
    Mat4 prevViewProj;   // last traced frame's, unjittered
};

// Offsets as dxc lays out deferred.comp.hlsl's cbuffer. A drift here does not
// fail anywhere at runtime - the shader just reads the neighbouring field.
static_assert(offsetof(Config, exposure) == 124, "Config drifted from the shader cbuffer");
static_assert(offsetof(Config, debugView) == 128, "Config drifted from the shader cbuffer");
static_assert(offsetof(Config, jitter_y) == 144, "Config drifted from the shader cbuffer");
static_assert(offsetof(Config, displayOnly) == 148, "Config drifted from the shader cbuffer");
static_assert(offsetof(Config, traceScale) == 152, "Config drifted from the shader cbuffer");
static_assert(offsetof(Config, frameIndex) == 156, "Config drifted from the shader cbuffer");
static_assert(offsetof(Config, historyCap) == 168, "Config drifted from the shader cbuffer");
static_assert(offsetof(Config, prevViewProj) == 176, "Config drifted from the shader cbuffer");
static_assert(sizeof(Config) == 240, "Config drifted from the shader cbuffer");

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

// Resolution the hybrid renderer traces rays at - see
// RendererSettings::traceResolution.
enum class TraceResolution
{
    Full,         // every pixel, every frame
    ThreeQuarter, // a 3/4-scale grid per axis - 9/16 of the pixels - upsampled
    Half,         // a 1/2-scale grid per axis - 1/4 of the pixels - upsampled
};

// Per-axis scale of the traced grid. Uploaded as Config::traceScale.
inline float TraceScaleOf(TraceResolution resolution)
{
    switch (resolution)
    {
    case TraceResolution::ThreeQuarter: return 0.75f;
    case TraceResolution::Half: return 0.5f;
    default: return 1.0f;
    }
}

// Size of the traced grid along one axis, rounded up so the last partial cell
// still reaches the edge of the frame. deferred.comp and upsample.comp compute
// the same value as ceil(float(size) * TraceScale); 0.75 and 0.5 are exact in
// float, so the CPU and GPU cannot disagree by a cell.
inline Uint32 ScaledExtent(Uint32 size, float scale)
{
    return static_cast<Uint32>(std::ceil(static_cast<float>(size) * scale));
}

// What the caller chooses before the window exists. Everything else is derived
// from the scene.
struct RendererSettings
{
    Uint32 width = 1920;
    Uint32 height = 1080;
    Uint32 samples = 1; // samples per pixel per frame

    // Stop tracing once the image has this many samples per pixel; 0 never
    // stops. Counted in whole frames, so with samples = 4 a cap of 1022 stops
    // at 1024. Anything that restarts accumulation - moving the camera,
    // switching renderer or debug view - starts tracing again.
    Uint32 maxSamples = 0;

    // Which compute shader to load, without the format suffix. Distinct from
    // renderType: ray_trace.comp implements *both* integrators and branches on
    // the renderType uniform internally, so this picks the file and renderType
    // picks the algorithm inside it.
    const char *shader = "ray_trace.comp.hybrid";
    RenderType renderType = RenderType::Path_Tracing;

    // Start in the hybrid renderer (raster G-buffer + hybridShader) or in the
    // compute-only one above. Both pipelines are always built, so this is only
    // the initial state - SetHybrid flips it at runtime.
    bool hybrid = true;
    const char *hybridShader = "deferred.comp";

    // Most path segments the hybrid renderer traces per pixel, counting the
    // primary hit: 3 = primary plus two indirect bounces. Capped because it is
    // the real-time renderer; the scene's own maxDepth still applies to the
    // compute-only renderer, which stays the full-quality reference.
    Uint32 hybridMaxDepth = 3;

    // Resolution the hybrid renderer traces rays at. ThreeQuarter and Half trace
    // a reduced grid per frame - 9/16 and 1/4 of the pixels, so roughly that
    // share of the ray cost - moving the samples each frame so a still image
    // still converges over every pixel, and upsample.comp fills in the rest
    // guided by the full-res G-buffer. Rasterized edges and textures stay full
    // resolution; lighting detail within a surface softens, less at 3/4 than at
    // 1/2. Debug views always trace every pixel.
    TraceResolution traceResolution = TraceResolution::Full;

    // Temporal reprojection: while the camera moves, the hybrid renderer keeps
    // each pixel's history - found wherever the pixel was last frame - instead
    // of restarting, capped at this many frames so stale lighting fades out
    // within that many. Higher is smoother while moving and trails more; metal
    // and glass are capped at 4 regardless, because reflections do not move
    // with their surface. A still camera accumulates without a cap either way.
    Uint32 historyWhileMoving = 32;

    const char *title = "AviatorEngine";
};

// The raster side of the same camera Config describes. Rebuilt in SyncCamera
// so there is exactly one place where the two representations can drift apart.
struct CameraMatrices
{
    Mat4 view;
    Mat4 proj; // unjittered; RenderFrame applies the per-frame jitter to a copy
    Mat4 viewProj;
    float zNear = 0.1f;
    float zFar = 10000.0f;
};

// One G-buffer draw call's worth of state. Built during the same pass that
// packs the object buffer, because that is the only place where the object's
// slot and its mesh range are both in hand.
struct RasterDraw
{
    Uint32 indexBase;   // first index, in elements
    Uint32 indexCount;  // triangleCount * 3
    Uint32 vertexBase;  // added to every index by the vertex_offset argument
    Uint32 objectIndex; // slot in the object buffer; travels through the G-buffer
    float position[3];
    float rotation[4]; // xyzw, Quat<float> memory order
    float scale[3];    // 1 for mesh instances; half-extents for a box proxy
    float albedo[3];
    Uint32 textureID;
    float textureTint; // how strongly albedo tints the texture
    bool doubleSided;  // draw unculled - an open surface such as a quad
    Uint32 colorType;  // MaterialType; the G-buffer flags metal and glass for reprojection
};

// Whether an analytic object is ALSO drawn into the G-buffer, as a stretched
// unit cube. Must stay in lockstep with IsRasterProxy in deferred.comp.hlsl:
// the shader skips exactly these objects when resolving primary visibility
// against the G-buffer, so anything drawn here but not skipped there flickers,
// and anything skipped there but not drawn here vanishes.
//
// Boxes only, and only those a stretched cube reproduces exactly:
//   - not a volume (what you see of a medium is scattering, not its box)
//   - untextured (the cube's per-face UVs differ from the shader's BoxFaceUV)
//   - unrotated (a yawed box stays analytic rather than trust two rotation
//     conventions to agree)
inline bool IsRasterProxy(const Object_GPU &o)
{
    return o.shapeType == static_cast<Uint32>(BodyShape::Box) &&
           o.colorType != static_cast<Uint32>(MaterialType::Isotropic) &&
           o.textureID == kNoTexture &&
           o.rotation == 0.0f;
}

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

    // Restart the image from nothing: every pixel's history is dropped on the
    // next traced frame. For changes reprojection cannot follow - geometry
    // moving, switching renderer or debug view.
    void ResetAccumulation()
    {
        accumulationFrame = 0;
        historyResetPending = true;
    }

    // The camera moved this frame. The hybrid renderer reprojects its history
    // to follow it; the compute-only renderer cannot, so for it this is a
    // restart. Either way convergence toward maxSamples starts counting again.
    void CameraMoved()
    {
        if (!hybrid)
        {
            ResetAccumulation();
            return;
        }
        accumulationFrame = 0;
        cameraMovedPending = true;
    }

    // Exposure multiplier applied to linear radiance before the tone curve.
    // Purely a display control - it does not touch the accumulation buffer, so
    // changing it re-grades the existing image without restarting convergence.
    float Exposure() const { return config.exposure; }
    void SetExposure(float value) { config.exposure = std::max(value, 0.01f); }
    // Samples per pixel averaged into the image so far. RenderFrame has already
    // counted the frame it just drew, so this is exactly what is on screen.
    Uint32 AccumulatedSamples() const { return accumulationFrame * config.samples; }

    // True once the image holds everything RendererSettings::maxSamples allows.
    // Debug views never converge: they redraw every frame instead of averaging.
    bool Converged() const
    {
        return maxSamples > 0 &&
               config.debugView == static_cast<Uint32>(DebugView::Final) &&
               AccumulatedSamples() >= maxSamples;
    }
    Uint32 MaxSamples() const { return maxSamples; }

    // The two renderers converge to different images (the hybrid one has no
    // depth of field, traces fewer bounces, and its meshes are faceted), so
    // switching restarts accumulation rather than averaging one into the other.
    bool Hybrid() const { return hybrid; }
    void SetHybrid(bool enabled)
    {
        hybrid = enabled;
        ApplyDepthCap(); // the two renderers have different bounce budgets
        ResetAccumulation();
    }
    Uint32 MaxDepth() const { return config.depth; }

    DebugView GetDebugView() const { return static_cast<DebugView>(config.debugView); }
    void SetDebugView(DebugView view)
    {
        config.debugView = static_cast<Uint32>(view);
        ResetAccumulation();
    }

    // One frame: rebuild if dirty, rasterize the G-buffer (hybrid), dispatch
    // the compute pass, blit to the swapchain. Returns false when the frame
    // could not be submitted.
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

    // The counts describe the shader's resource layout and are not
    // documentation - SDL builds the descriptor sets from them. numSamplers in
    // particular decides where the storage buffers start; see the call sites in
    // Initialize. threadsX/Y must match the shader's [numthreads].
    SDL_GPUComputePipeline *CreateComputePipeline(const char *stem,
                                                  Uint32 numSamplers,
                                                  Uint32 numStorageBuffers,
                                                  Uint32 numReadWriteTextures,
                                                  Uint32 threadsX,
                                                  Uint32 threadsY);

    SDL_GPUShader *LoadShader(const char *stem,
                              SDL_GPUShaderStage stage,
                              Uint32 numSamplers,
                              Uint32 numUniformBuffers,
                              Uint32 numStorageBuffers);

    bool CreateGBuffer();
    bool CreateGBufferPipeline();

    // Draws every mesh instance into gPosition / gNormal / gAlbedo / gDepth.
    // viewProj is this frame's, jitter included.
    void RenderGBuffer(SDL_GPUCommandBuffer *commandBuffer, const Mat4 &viewProj);

    // Radical inverse in `base`: the Halton sequence used for sub-pixel jitter.
    static float Halton(Uint32 index, Uint32 base);

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

    // Fills in each object's previous transform - the motion vector temporal
    // reprojection follows. PackObjects has already written "previous =
    // current" everywhere, so this only has to correct the objects that moved.
    void ApplyPreviousTransforms(const entt::registry &registry, std::vector<Object_GPU> &objects);

    // config.depth from sceneMaxDepth and, in hybrid mode, hybridMaxDepth.
    void ApplyDepthCap();

    // Indices into the *ordered* entity list, i.e. into the object buffer the
    // shader sees - not entity ids. The BVH decides that ordering, so this runs
    // after every build.
    static std::vector<uint32_t> LightIndices(const entt::registry &registry,
                                              const std::vector<entt::entity> &ordered);

    // Packs the object buffer and, as side effects, the instance buffer and the
    // raster draw list: a mesh entity emits one of each, and the object's
    // instanceIndex is the slot the instance landed in. They are built together
    // because only this pass knows how many instances have been emitted so far
    // and which object slot each one occupies.
    static std::vector<Object_GPU> PackObjects(const entt::registry &registry,
                                               const std::vector<entt::entity> &ordered,
                                               const MeshLibrary &library,
                                               std::vector<MeshInstance_GPU> &outInstances,
                                               std::vector<RasterDraw> &outDraws,
                                               const MeshRange *boxProxy);
    static uint32_t CountLights(const entt::registry &registry);

    SDL_Window *window = nullptr;
    SDL_GPUDevice *device = nullptr;
    SDL_GPUComputePipeline *pipeline = nullptr;         // compute-only primaries
    SDL_GPUComputePipeline *deferredPipeline = nullptr; // hybrid
    SDL_GPUComputePipeline *upsamplePipeline = nullptr; // hybrid, reduced resolution
    SDL_GPUSampler *linearSampler = nullptr;
    // Linear HDR accumulation, two of them swapped on every frame that writes
    // history: last frame's is read as history (reprojected to wherever each
    // pixel moved) while this frame's is written. Averaging only works in
    // linear space, so neither may ever hold tone-mapped values. historyIndex
    // names the one written most recently; the compute-only renderer
    // accumulates into that one in place.
    SDL_GPUTexture *accumTextures[2] = {nullptr, nullptr};

    // RGBA32F, swapped in step with accumTextures: xyz = each pixel's surface
    // position, w = its history length, negated when the pixel had no surface.
    // Next frame's disocclusion test reads it. 32-bit so the length stays an
    // exact integer - a half float stops counting at 1024.
    SDL_GPUTexture *historyGeometry[2] = {nullptr, nullptr};
    Uint32 historyIndex = 0;

    // What actually reaches the screen: the accumulation tone-mapped and
    // sRGB-encoded. Kept separate so the tone curve is applied once at display
    // time rather than being folded back into next frame's running average.
    SDL_GPUTexture *displayTexture = nullptr;

    // RGBA16F, the size of the reduced trace grid: what deferred.comp traces into
    // below full resolution, and what upsample.comp reads. 1x1 at full
    // resolution - there it only has to exist, so the deferred pass's u2 is bound.
    SDL_GPUTexture *lightingReduced = nullptr;
    SDL_GPUTexture *globalTextureArray = nullptr;

    // The G-buffer: a screen-sized array of the ray tracer's Hit struct.
    SDL_GPUTexture *gPosition = nullptr; // RGBA32F: xyz world, w object index
    SDL_GPUTexture *gNormal = nullptr;   // RGBA16F: xyz world normal, w coverage
    SDL_GPUTexture *gAlbedo = nullptr;   // RGBA8:   rgb albedo x texture
    SDL_GPUTexture *gDepth = nullptr;    // D32F
    SDL_GPUGraphicsPipeline *gbufferPipeline = nullptr;
    // The same pipeline with culling off, for RasterDraw::doubleSided.
    SDL_GPUGraphicsPipeline *gbufferPipelineTwoSided = nullptr;
    // NEAREST: a G-buffer must never be filtered - see CreateGBuffer.
    SDL_GPUSampler *pointSampler = nullptr;

    SceneBuffers buffers;
    Config config = {};
    CameraMatrices cameraMatrices;
    bool hybrid = true;
    TraceResolution traceResolution = TraceResolution::Full;

    // Per-frame scratch, kept alive across frames so the vectors keep their
    // capacity instead of reallocating 60 times a second.
    std::vector<entt::entity> renderables;
    std::vector<AABB> renderableBounds;
    std::vector<BVHNode_GPU> nodes;
    std::vector<uint32_t> buildOrder; // BVH slot -> index into `renderables`
    std::vector<entt::entity> orderedEntities;
    std::vector<uint32_t> lightIDs;
    std::vector<MeshInstance_GPU> instances;
    std::vector<RasterDraw> rasterDraws;

    // Where each object was at the last rebuild. Keyed by entity, not by slot:
    // the BVH reorders slots on every rebuild, so slot 7 is a different object
    // from one frame to the next.
    std::unordered_map<entt::entity, TransformComponent> previousTransforms;

    // Borrowed from the Scene at LoadScene time; the Scene outlives the frame
    // loop, so this is a non-owning view of its mesh library.
    const MeshLibrary *meshLibrary = nullptr;

    // The unit cube every analytic box proxy is drawn with; LoadScene registers
    // it into the scene's mesh library. kNoMesh until then.
    MeshHandle boxProxyMesh = kNoMesh;

    // config.depth is derived from these two - see ApplyDepthCap.
    Uint32 sceneMaxDepth = 1;
    Uint32 hybridMaxDepth = 3;

    // 0 = accumulate forever. See RendererSettings::maxSamples.
    Uint32 maxSamples = 0;

    bool sceneDirty = true;

    // True when the last upload carried a non-zero motion vector. The object
    // buffer is only re-uploaded while the scene is dirty, so without this a
    // settled scene would keep the motion of the last frame that moved and go
    // on rejecting its own history forever - see RenderFrame.
    bool uploadedMotion = false;

    // Frames averaged since the image last restarted or the camera last moved:
    // what AccumulatedSamples and maxSamples count.
    Uint32 accumulationFrame = 0;

    // Traced frames since startup, never reset - see Config::frameIndex.
    Uint32 frameIndex = 0;

    // Raised by ResetAccumulation / CameraMoved and consumed by the next traced
    // frame. The reset starts raised so the very first frame ignores history
    // textures nothing has written yet.
    bool historyResetPending = true;
    bool cameraMovedPending = false;

    // The last traced frame's unjittered view-projection, for reprojection.
    Mat4 previousViewProj = Mat4::Identity();
    Uint32 historyWhileMoving = 32;
};

#endif
