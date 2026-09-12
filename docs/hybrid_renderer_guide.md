# Going Hybrid: Rasterization + Ray Tracing in AviatorEngine

**A mentor's walkthrough, written against your actual code.**

---

## 0. How to read this

This is not a generic tutorial. Every file path, register number, struct field
and function name below is the one that exists in your tree right now. I read
`renderer/renderer.cpp`, `shaders/ray_trace.comp.hybrid.hlsl`, `scene/gpu_types.h`,
`scene/mesh_library.h` and `CMakelists.txt` before writing a line of it.

Work through it in order. Each phase ends in something you can **run and look at**.
If a phase doesn't produce a picture, you've gone too far without checking.

| Phase | What you get | Effort |
|---|---|---|
| 0 | Blockers cleared, build compiles graphics shaders | half a day |
| 1 | Matrices that agree with your ray camera, provably | half a day |
| 2 | A G-buffer you can look at | 1-2 days |
| 3 | Deferred shading + ray-traced shadows. **This is the payoff.** | 1-2 days |
| 4 | Analytic spheres/quads back in the picture | 1 day |
| 5 | Ray-traced reflections and one-bounce GI | 2 days |
| 6 | Making it fast | forever |

---

## 1. What "hybrid" can and cannot mean for you

First, the thing that determines your entire architecture:

> **SDL_gpu does not expose hardware ray tracing.** There is no `VK_KHR_ray_query`,
> no DXR, no `TraceRay()`, no acceleration-structure API in SDL3's GPU abstraction.

So "hybrid" here does **not** mean "raster pass + RT pipeline". It means:

> **Rasterize what rasterization is good at (primary visibility), and ray trace
> what rasterization is bad at (everything that needs to know about geometry the
> pixel cannot see) - using your own BVH in a compute shader.**

That is good news, because it is exactly what your engine is already built for.
Look at what you have:

- `TraverseBVH()` - TLAS traversal over an explicit stack (`ray_trace.comp.hybrid.hlsl:849`)
- `HitMesh()` - BLAS traversal per instance (`:587`)
- `CalculateDirectLight()` - next-event estimation with shadow rays (`:1102`)
- `GetBounce()` - a full BRDF sampler for Lambertian / metal / dielectric / isotropic (`:945`)
- Temporal accumulation, ACES tone mapping, sRGB encode (`:1298` onward)

You are not missing a ray tracer. **You are missing a rasterizer, and a way to
hand the rasterizer's output back to the ray tracer.** That is the whole project.

### What each side is actually for

| Job | Best tool | Why |
|---|---|---|
| Primary visibility | **Raster** | Hardware rasterizers do this far more efficiently than a BVH walk. Your camera-ray `TraverseBVH` is your single largest per-frame cost. |
| Direct light / shadows | **Ray trace** | Shadow maps need cascades, bias tuning, peter-panning fixes. A shadow ray is 12 lines and exact. |
| Reflections | **Ray trace** | SSR cannot reflect offscreen geometry. Rays can. |
| Ambient occlusion / GI | **Ray trace** | SSAO is a lie. RTAO is one cosine-weighted ray. |
| Refraction / caustics | **Ray trace** | Raster cannot do this at all. |
| Depth of field | Neither, for now | Your ray camera does thin-lens DOF via `defocus_disk_sample()`. Raster has one pinhole per frame. See §6.4. |

### The honest trade

You lose **exact analytic primaries**. Right now a sphere in your renderer is a
perfect ray/sphere intersection - infinite tessellation, free. A rasterized
sphere is triangles. Phase 4 gives most of that back with a hybrid visibility
resolve, but understand the trade before you start.

You gain: primary visibility at a fraction of the cost, which is the entire
budget you then spend on rays that actually matter.

---

## 2. The architecture

```
   RENDER PASS  (graphics pipeline - new)
   +------------------------------------------+
   | for each MeshComponent instance:         |
   |   bind vertexBuffer / indexBuffer        |      +-------------------+
   |   push per-instance uniform (pos, quat)  |----->| gPosition RGBA32F |
   |   DrawIndexed(range)                     |----->| gNormal   RGBA16F |
   |                                          |----->| gAlbedo   RGBA8   |
   | depth test writes gDepth                 |----->| gDepth    D32F    |
   +------------------------------------------+      +-------------------+
                                                              |
                                                              | sampled
                                                              v
   COMPUTE PASS  (your existing shader, rewired)
   +--------------------------------------------------------------+
   | per pixel:                                                   |
   |   Hit hit = HitFromGBuffer(id);        <- was: primary ray   |
   |   [Phase 4] resolve analytic prims closer than gDepth        |
   |   if (!coverage) -> sky                                      |
   |   colour  = SurfaceEmission(hit)                             |
   |   colour += CalculateDirectLight(hit)  <- ray-traced shadows |
   |   colour += TraceIndirect(hit)         <- reflections / GI   |
   |   accumulate -> image[id]                                    |
   |   tonemap   -> displayImage[id]                              |
   +--------------------------------------------------------------+
                             |
                             v
                    Blit displayImage -> swapchain
```

**The single most important line in that diagram** is
`Hit hit = HitFromGBuffer(id);` replacing your primary `TraverseBVH()`.
Everything downstream of it - emission, NEE, bounces, accumulation, tone
mapping - is code you already wrote and do not need to change.

That is why I am routing you through a **deferred** G-buffer rather than forward
rasterization. Your ray tracer's whole world is the `Hit` struct. A G-buffer is
literally a screen-sized array of `Hit` structs. The two designs were made for
each other.

---

## 3. Phase 0 - Clearing the blockers

Five things in your tree stop you from writing a single triangle today. None of
them are conceptual.

### 3.1 `shaders/rasterizer.frag.hlsl` is GLSL, not HLSL

Open it. Line 1 is `#version 460`. It is full of `layout(location = 0) in vec4`,
`texture()`, `vec3[]`. That is GLSL, presumably carried over from an OpenGL
engine. Your build runs it through `dxc`, which speaks HLSL. It cannot compile.

**Do not port it.** Almost everything in that file - `CascadeShadow`,
`PCFShadow`, `SampleCascadeShadow`, `CascadeBias`, `PointShadow`, all 20 offset
vectors - exists to fake what a shadow ray does exactly. In a hybrid renderer you
throw it away.

```bash
git rm shaders/rasterizer.frag.hlsl shaders/rasterizer.vertex.hlsl
```

(`rasterizer.vertex.hlsl` is 0 bytes, so nothing is lost there either.)

Keep it in git history and steal `CalcPointLight`'s attenuation formula later if
you want punctual lights. The cascade code is dead weight.

### 3.2 There is no matrix type in the codebase

`grep -rn "Mat4" math/` returns nothing. `math/math.h` is 18 lines wrapping
`sin`, `cos`, `tan`. You cannot rasterize without a view-projection matrix.
That is Phase 1.

### 3.3 Your mesh buffers cannot be bound as vertex/index buffers

`renderer/renderer.cpp`, `CreateSceneBuffers()`:

```cpp
SDL_GPUBufferCreateInfo bufferInfo = {};
bufferInfo.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;   // <- every buffer
```

The vertex and index buffers need `SDL_GPU_BUFFERUSAGE_VERTEX` and
`SDL_GPU_BUFFERUSAGE_INDEX` respectively, **in addition to** the compute usage -
the ray tracer still reads them as storage buffers. Fixed in Phase 2.

The good news: `Vertex_GPU` is already a perfect raster vertex.

```cpp
struct Vertex_GPU { float px, py, pz;  float nx, ny, nz;  float u, v; };  // 32 bytes
```

Position at offset 0, normal at 12, UV at 24, tight 32-byte stride. That maps
one-to-one onto three `SDL_GPUVertexAttribute`s with zero repacking. And your
indices are already `uint32_t` in `MeshLibrary::indices`, so
`SDL_GPU_INDEXELEMENTSIZE_32BIT` - no 16-bit conversion.

### 3.4 CMake only compiles compute shaders

`CMakelists.txt` hardcodes the profile:

```cmake
COMMAND ${DXC_EXECUTABLE} -spirv -T cs_6_0 -E main ${SHADER} -Fo ${SHADER_SPV}
```

Replace the shader block so the profile is derived from the filename:

```cmake
set(SHADER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/ray_trace.comp.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/ray_trace.comp.hybrid.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/path_trace.comp.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/gbuffer.vert.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/gbuffer.frag.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/deferred.comp.hlsl
)

if(DXC_EXECUTABLE)
    set(COMPILED_SHADERS "")
    foreach(SHADER ${SHADER_SOURCES})
        get_filename_component(SHADER_FILE ${SHADER} NAME)
        string(REGEX REPLACE "\\.hlsl$" "" SHADER_NAME ${SHADER_FILE})
        set(SHADER_SPV ${BINARY_DIR}/${SHADER_NAME}.spv)

        # The stage is encoded in the trailing extension. Anything that is not
        # explicitly .vert or .frag compiles as compute, which keeps the
        # existing "ray_trace.comp.hybrid.hlsl" name working - it has .comp in
        # the middle rather than at the end.
        if(SHADER_NAME MATCHES "\\.vert$")
            set(SHADER_PROFILE vs_6_0)
        elseif(SHADER_NAME MATCHES "\\.frag$")
            set(SHADER_PROFILE ps_6_0)
        else()
            set(SHADER_PROFILE cs_6_0)
        endif()

        add_custom_command(
            OUTPUT ${SHADER_SPV}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${BINARY_DIR}
            COMMAND ${DXC_EXECUTABLE} -spirv -T ${SHADER_PROFILE} -E main
                    ${SHADER} -Fo ${SHADER_SPV}
            DEPENDS ${SHADER}
            COMMENT "Compiling ${SHADER_NAME}.hlsl (${SHADER_PROFILE}) -> ${SHADER_NAME}.spv"
            VERBATIM
        )
        list(APPEND COMPILED_SHADERS ${SHADER_SPV})
    endforeach()

    add_custom_target(shaders ALL DEPENDS ${COMPILED_SHADERS})
    add_dependencies(${PROJECT_NAME} shaders)
endif()
```

### 3.5 The SDL_gpu HLSL register-space rules

This is the thing that will cost you an afternoon if nobody tells you. SDL_gpu
assigns descriptor sets by **shader stage**, and register indices within a space
by **resource kind, in a fixed order**.

| Stage | Samplers / textures | Storage textures | Storage buffers | Uniform buffers |
|---|---|---|---|---|
| Vertex | `space0` | `space0` | `space0` | `space1` |
| Fragment | `space2` | `space2` | `space2` | `space3` |
| Compute (read-only) | `space0` | `space0` | `space0` | `space2` |
| Compute (read-write) | - | `space1` | `space1` | - |

Within a space, `t`-registers are handed out **sampled textures first, then
storage textures, then storage buffers**. Your compute shader today:

```hlsl
Texture2DArray GlobalTextureArray : register(t0, space0);   // 1 sampled texture
StructuredBuffer<Object>       objects   : register(t1, space0);
StructuredBuffer<BVHNode>      bvh       : register(t2, space0);
StructuredBuffer<uint>         lightIDs  : register(t3, space0);
StructuredBuffer<Vertex>       vertices  : register(t4, space0);
StructuredBuffer<uint>         meshIndex : register(t5, space0);
StructuredBuffer<BVHNode>      blas      : register(t6, space0);
StructuredBuffer<MeshInstance> instances : register(t7, space0);
```

> **The trap:** when you add three G-buffer textures as compute samplers, they
> occupy `t1, t2, t3` and **every storage buffer shifts to `t4`-`t10`** (to
> `t5`-`t11` once `gDepth` joins them for §9.1 - see §6.1). Nothing
> warns you. The shader reads zeroes and you get a black screen with no
> validation error. Phase 3 gives you the exact new table.

Your fragment shader will use `space2` for the texture array and `space3` for
uniforms - which is exactly what the GLSL file you already wrote used
(`set = 2` for samplers, `set = 3` for uniforms). Whoever wrote that knew the rule.

---

## 4. Phase 1 - Matrices, and making them agree with your ray camera

### 4.1 Why this phase is dangerous

The classic hybrid-renderer bug is that the rasterized image and the ray-traced
image **disagree about where the camera is**, by half a pixel or by a Y flip or
by a field-of-view interpretation. Shadows land one pixel off. Reflections
shimmer. You will chase it for a week and it will turn out to be a sign.

So we derive the matrices *from* your ray camera, not from a textbook, and then
we prove they agree.

### 4.2 Your ray camera, decoded

From `main()` in `ray_trace.comp.hybrid.hlsl`:

```hlsl
const float3 vectorW = normalize(Source - Target);   // BACKWARD (points at viewer)
const float3 vectorU = normalize(cross(Up, vectorW));// right
const float3 vectorV = cross(vectorW, vectorU);      // up
const float viewportH = 2.0f * tan(radians(Fov / 2.0f)) * Focus;
const float viewportW = viewportH * Width / Height;
const float3 viewportU =  viewportW * vectorU;
const float3 viewportV =  viewportH * -vectorV;      // note the minus
const float3 pixel0 = Source - Focus * vectorW - viewportU/2 - viewportV/2 + ...;
```

Read off the facts:

1. **Right-handed basis** with `u` right, `v` up, `w` backward. Same convention as `glm::lookAt`.
2. **Vertical FOV** is `Fov` degrees. The half-height at distance `Focus` is `tan(Fov/2) * Focus`, so the `Focus` cancels - `Fov` is a pure vertical angle.
3. **Aspect** is `Width / Height`.
4. `viewportV` uses `-vectorV`, and pixel index `y` grows downward. So **increasing screen Y moves along `-v`**, i.e. down. That is Vulkan's NDC convention already.

### 4.3 `math/mat4.h`

Create this file. Row-major storage, declared `row_major` on the HLSL side, so
the bytes mean the same thing on both sides with no transpose anywhere.

```cpp
#ifndef MAT4_H
#define MAT4_H

#include <cmath>

#include "math/vec3.h"

// Row-major 4x4: m[row][col]. Uploaded verbatim to HLSL, where the matching
// field is declared `row_major float4x4`. HLSL's cbuffer DEFAULT is
// column-major, so leaving that keyword off silently transposes every matrix
// you upload - which looks like a scene that is inside-out and rotated, not
// like an obvious error. The keyword is load-bearing.
struct Mat4
{
    float m[4][4];

    static Mat4 Identity()
    {
        Mat4 r = {};
        r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0f;
        return r;
    }
};

inline Mat4 operator*(const Mat4 &a, const Mat4 &b)
{
    Mat4 r = {};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] +
                        a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
    return r;
}

// Transforms a point (w = 1) and divides through. Used only by the CPU-side
// agreement test in Phase 1.5 - the GPU does this itself.
inline Vec3<float> TransformPoint(const Mat4 &a, const Vec3<float> &p, float &outW)
{
    const float x = a.m[0][0]*p.x + a.m[0][1]*p.y + a.m[0][2]*p.z + a.m[0][3];
    const float y = a.m[1][0]*p.x + a.m[1][1]*p.y + a.m[1][2]*p.z + a.m[1][3];
    const float z = a.m[2][0]*p.x + a.m[2][1]*p.y + a.m[2][2]*p.z + a.m[2][3];
    outW          = a.m[3][0]*p.x + a.m[3][1]*p.y + a.m[3][2]*p.z + a.m[3][3];
    return Vec3<float>(x, y, z);
}

// Right-handed look-at, built from EXACTLY the basis the compute shader's
// main() builds. If you ever change the basis in one place, change it here in
// the same commit - the two disagreeing is the hybrid renderer's signature bug.
//
//   w = normalize(eye - target)     backward
//   u = normalize(cross(up, w))     right
//   v = cross(w, u)                 up
//
// The result maps world space to view space with x right, y up, z BACKWARD,
// so anything the camera can see has a negative view-space z.
inline Mat4 LookAtRH(const Vec3<float> &eye,
                     const Vec3<float> &target,
                     const Vec3<float> &up)
{
    const Vec3<float> w = normalize(eye - target);
    const Vec3<float> u = normalize(cross(up, w));
    const Vec3<float> v = cross(w, u);

    Mat4 r = Mat4::Identity();
    r.m[0][0] = u.x; r.m[0][1] = u.y; r.m[0][2] = u.z; r.m[0][3] = -dot(u, eye);
    r.m[1][0] = v.x; r.m[1][1] = v.y; r.m[1][2] = v.z; r.m[1][3] = -dot(v, eye);
    r.m[2][0] = w.x; r.m[2][1] = w.y; r.m[2][2] = w.z; r.m[2][3] = -dot(w, eye);
    return r;
}

// Right-handed perspective for SDL_gpu clip space: depth in [0,1] (not [-1,1]
// like OpenGL) and NDC +Y pointing UP the screen.
//
// That is SDL_gpu's convention on EVERY backend, Vulkan included - see the
// "Normalized Device Coordinates" note in SDL_gpu.h. Raw Vulkan is +Y down, but
// SDL's Vulkan backend flips the viewport (negative height) to match D3D12 and
// Metal. So m[1][1] is a plain positive f: a -f here, which is the usual
// hand-written-Vulkan advice, renders the raster image upside down under SDL.
//
// fovYDegrees matches CameraComponent::fov and the shader's `Fov` uniform -
// both are the FULL vertical angle in degrees.
inline Mat4 PerspectiveRH_ZO(float fovYDegrees, float aspect, float zNear, float zFar)
{
    const float f = 1.0f / std::tan(fovYDegrees * 0.017453292519943295f * 0.5f);

    Mat4 r = {};
    r.m[0][0] =  f / aspect;
    r.m[1][1] =  f;                                  // SDL_gpu NDC is +Y up
    r.m[2][2] =  zFar / (zNear - zFar);
    r.m[2][3] =  (zNear * zFar) / (zNear - zFar);
    r.m[3][2] = -1.0f;
    return r;
}

// Recovers view-space distance from a [0,1] depth-buffer value written by
// PerspectiveRH_ZO. The algebra is just that matrix inverted for z:
//   ndcZ = zFar*(zNear - z) / ((zNear - zFar) * z)   =>   z = zn*zf / (zf + d*(zn - zf))
// Phase 6 uses this to drop gPosition and reconstruct world space from depth.
inline float LinearizeDepth(float d, float zNear, float zFar)
{
    return (zNear * zFar) / (zFar + d * (zNear - zFar));
}

#endif // MAT4_H
```

### 4.4 Wiring it into the Renderer

`Config` already carries everything you need to build these. Add to
`renderer/renderer.h`:

```cpp
#include "math/mat4.h"

// The raster side of the same camera Config describes. Rebuilt in SyncCamera
// so there is exactly one place where the two representations can drift apart.
struct CameraMatrices
{
    Mat4 view;
    Mat4 proj;
    Mat4 viewProj;
    float zNear = 0.1f;
    float zFar  = 10000.0f;
};
```

and a member `CameraMatrices cameraMatrices;`. Then extend `SyncCamera`:

```cpp
void Renderer::SyncCamera(const entt::registry &registry, entt::entity camera)
{
    const auto &transform = registry.get<TransformComponent>(camera);
    const auto &cam = registry.get<CameraComponent>(camera);
    const Vec3<float> forward = Forward(const_cast<CameraComponent &>(cam));

    config.source_x = transform.position.x;
    /* ... existing config fills, unchanged ... */
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
```

### 4.5 Prove they agree before you write a rasterizer

**Do this. It takes 20 minutes and saves days.** Add a temporary function that
projects a known world point through the matrices, and independently through the
ray camera's pixel math, and compares.

```cpp
// TEMPORARY - delete once Phase 2 is green. Projects a world point two ways:
// through the raster matrices, and through the compute shader's pixel basis
// reimplemented on the CPU. They must land on the same pixel. If they do not,
// nothing built on top of them will ever line up, and the failure downstream
// looks like a lighting bug rather than a camera bug.
void Renderer::DebugVerifyCameraAgreement() const
{
    const Vec3<float> eye(config.source_x, config.source_y, config.source_z);
    const Vec3<float> target(config.target_x, config.target_y, config.target_z);
    const Vec3<float> up(config.up_x, config.up_y, config.up_z);

    // --- the compute shader's basis, verbatim from main() ---
    const Vec3<float> w = normalize(eye - target);
    const Vec3<float> u = normalize(cross(up, w));
    const Vec3<float> v = cross(w, u);
    const float focus = config.focus_dist;
    const float viewportH = 2.0f * std::tan(config.fov * 0.017453292f * 0.5f) * focus;
    const float viewportW = viewportH * config.width / config.height;
    const Vec3<float> viewportU = viewportW * u;
    const Vec3<float> viewportV = viewportH * -v;
    const Vec3<float> pixelU = viewportU / static_cast<float>(config.width);
    const Vec3<float> pixelV = viewportV / static_cast<float>(config.height);
    const Vec3<float> pixel0 = eye - focus * w - viewportU / 2.0f - viewportV / 2.0f
                             + pixelU / 2.0f + pixelV / 2.0f;

    // Probe a few pixels spread across the frame.
    const float probes[5][2] = {{0.5f,0.5f},{0.25f,0.25f},{0.75f,0.25f},{0.25f,0.75f},{0.9f,0.6f}};
    for (const auto &p : probes)
    {
        const float px = p[0] * config.width;
        const float py = p[1] * config.height;

        // Ray side: the world point this pixel's ray passes through.
        const Vec3<float> worldPoint = pixel0 + px * pixelU + py * pixelV;

        // Raster side: project that same point back to a pixel.
        float clipW = 0.0f;
        const Vec3<float> clip = TransformPoint(cameraMatrices.viewProj, worldPoint, clipW);
        const float ndcX = clip.x / clipW;
        const float ndcY = clip.y / clipW;
        const float rasterX = (ndcX * 0.5f + 0.5f) * config.width;
        // SDL_gpu NDC is +Y up and its viewport maps +1 to the TOP row. Write
        // this as (ndcY * 0.5 + 0.5) - the raw-Vulkan mapping - and the check
        // agrees with a projection that renders upside down under SDL.
        const float rasterY = (0.5f - ndcY * 0.5f) * config.height;

        SDL_Log("camera check: ray pixel (%.1f, %.1f) -> raster pixel (%.3f, %.3f)  delta (%.4f, %.4f)",
                px, py, rasterX, rasterY, rasterX - px, rasterY - py);
    }
}
```

(Implementation note: `pixel0` here should leave out the shader's half-pixel
offset, so both sides compare pixel *edge* coordinates. On the test scene the
implemented version printed a worst delta of 0.0001 px.)

Call it once after the first `SyncCamera`. **Every delta must be under 0.01
pixels.** If the Y delta is roughly `height - 2*py`, your Y flip is wrong -
check the sign of `m[1][1]`, *and* check that this test's viewport mapping
matches your API's NDC convention, or the test will happily agree with a
flipped image. If X and Y are both scaled by a constant, your FOV
is being read as half-angle vs full-angle. If everything is garbage, you have a
row-major/column-major transpose.

Do not proceed until this prints zeroes.

---

## 5. Phase 2 - The G-buffer pass

### 5.1 Choosing the layout

A G-buffer is a screen-sized array of `Hit` structs. Look at what your `Hit`
actually needs:

```hlsl
struct Hit
{
    float  Offset;       // distance along the ray  -> derivable
    float3 Position;     // world hit point         -> MUST STORE
    float3 Normal;       // world normal            -> MUST STORE
    float2 surface_uv;   // texture coords          -> avoidable, see below
    bool   Face;         // front-facing?           -> derivable from N . V
    Object Object;       // the whole material      -> refetch by index!
    uint   materialId;
};
```

The clever part: **you do not need to store the material.** Store the *index* of
the object, and the compute shader does `objects[index]` - it already has that
buffer bound at `t1`. One float carries albedo, fuzz, refraction, emission,
shape type, texture id, everything.

And you do not need `surface_uv` either, because the **fragment shader already
has the UV and can sample `GlobalTextureArray` itself**. Store the resolved
albedo. That is one fewer target and one fewer chance to disagree.

That gives three color targets plus depth:

| Target | Format | Contents |
|---|---|---|
| `gPosition` | `R32G32B32A32_FLOAT` | `xyz` = world position, `w` = object index as float |
| `gNormal` | `R16G16B16A16_FLOAT` | `xyz` = world normal, `w` = coverage (1.0 = geometry here) |
| `gAlbedo` | `R8G8B8A8_UNORM` | `rgb` = albedo x texture, `a` = unused |
| `gDepth` | `D32_FLOAT` | depth test only, for now |

Two notes on the format choices:

- **Object index goes in `gPosition.w`, not `gNormal.w`.** `gNormal` is 16-bit
  float; a half is only exact up to 2048, so an index of 3000 would come back as
  3000-ish and you would fetch the wrong object. `gPosition` is 32-bit float,
  exact to 16.7 million.
- **Storing world position at full 32-bit is deliberately wasteful** and you
  will replace it in Phase 6 with depth reconstruction. Do it this way first:
  reconstruction bugs and lighting bugs look identical, and you want only one
  new thing at a time.

### 5.2 `shaders/gbuffer.vert.hlsl`

```hlsl
// G-buffer vertex stage. One draw call per mesh instance; the instance's rigid
// transform arrives as a push uniform rather than a matrix, because that is how
// the ray tracer already stores it (MeshInstance_GPU: float3 position +
// quaternion, 48 bytes, no scale). Building a model matrix here from the same
// quaternion means raster and RT cannot drift.

struct VSInput
{
    // Matches Vertex_GPU in scene/mesh_library.h - 32 bytes, tightly packed.
    float3 position : TEXCOORD0;
    float3 normal   : TEXCOORD1;
    float2 uv       : TEXCOORD2;
};

struct VSOutput
{
    float4 clipPosition : SV_Position;
    float3 worldPosition: TEXCOORD0;
    float3 worldNormal  : TEXCOORD1;
    float2 uv           : TEXCOORD2;
};

// Uniform buffers for the vertex stage live in space1 (see the register table
// in the guide). `row_major` matters: HLSL cbuffers default to column-major and
// Mat4 on the CPU is row-major, so leaving it off transposes silently.
cbuffer InstanceBlock : register(b0, space1)
{
    row_major float4x4 ViewProj;
    float4 InstanceRotation;  // xyzw quaternion, matching Quat<float> MEMORY order
    float4 InstancePosition;  // xyz world origin, w unused
};

// Same rotation used by the ray tracer (quat_rotate, ray_trace.comp.hybrid.hlsl:508).
// Copied rather than shared because HLSL has no #include across stages here -
// if you change one, change both.
float3 QuatRotate(float4 q, float3 v)
{
    const float3 t = 2.0f * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

VSOutput main(VSInput input)
{
    VSOutput output;

    // Rigid transform only: rotate, then translate. No scale anywhere in the
    // engine, which is why the normal needs no inverse-transpose - a rotation
    // is its own normal matrix.
    const float3 world = QuatRotate(InstanceRotation, input.position) + InstancePosition.xyz;

    output.worldPosition = world;
    output.worldNormal   = QuatRotate(InstanceRotation, input.normal);
    output.uv            = input.uv;
    output.clipPosition  = mul(ViewProj, float4(world, 1.0f));
    return output;
}
```

### 5.3 `shaders/gbuffer.frag.hlsl`

```hlsl
// G-buffer fragment stage. Writes surface attributes, NOT lighting - every
// light in this renderer is evaluated by the compute pass with real shadow
// rays. If you find yourself adding a light loop here, you have accidentally
// rebuilt a forward renderer.

#define INVALID_TEXTURE 0xFFFFFFFFu

struct PSInput
{
    float4 clipPosition : SV_Position;
    float3 worldPosition: TEXCOORD0;
    float3 worldNormal  : TEXCOORD1;
    float2 uv           : TEXCOORD2;
};

struct PSOutput
{
    float4 position : SV_Target0;  // xyz world, w = object index
    float4 normal   : SV_Target1;  // xyz world normal, w = coverage
    float4 albedo   : SV_Target2;  // rgb albedo
};

// Fragment-stage samplers live in space2, uniforms in space3.
Texture2DArray GlobalTextureArray : register(t0, space2);
SamplerState   GlobalSampler      : register(s0, space2);

cbuffer SurfaceBlock : register(b0, space3)
{
    float4 Albedo;       // rgb = Object.Albedo (a tint, not a replacement)
    uint   TextureID;    // layer in GlobalTextureArray, or INVALID_TEXTURE
    uint   ObjectIndex;  // slot in the `objects` StructuredBuffer
    uint   Pad0;
    uint   Pad1;
};

PSOutput main(PSInput input)
{
    PSOutput output;

    // Same tint-not-replace rule as SurfaceAlbedo() in the compute shader:
    // leave Object.Albedo white to show a texture exactly as authored.
    float3 albedo = Albedo.rgb;
    if (TextureID != INVALID_TEXTURE)
    {
        albedo = GlobalTextureArray.Sample(GlobalSampler, float3(input.uv, TextureID)).rgb;
    }

    // Object index goes through a float. Exact up to 2^24 in R32F, which is
    // several orders of magnitude past any scene this engine will hold.
    output.position = float4(input.worldPosition, (float)ObjectIndex);

    // Coverage in .w, not an alpha test: the compute pass needs to distinguish
    // "background" from "a surface whose normal happens to be zero".
    output.normal   = float4(normalize(input.worldNormal), 1.0f);
    output.albedo   = float4(albedo, 1.0f);
    return output;
}
```

### 5.4 Renderer changes

**Buffer usage** in `CreateSceneBuffers()`. Replace the flat loop with per-slot
usage flags:

```cpp
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

    // The two buffers the rasterizer also consumes. VERTEX/INDEX are ADDED to
    // the compute usage rather than replacing it - the ray tracer still walks
    // the same memory through HitMeshTriangle(), and a buffer can carry both
    // roles as long as both are declared up front.
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
    bufferInfo.size  = a.size;
    *a.buffer = SDL_CreateGPUBuffer(device, &bufferInfo);
    /* ... transfer buffer creation unchanged ... */
}
```

**G-buffer textures.** New members in `Renderer`:

```cpp
SDL_GPUTexture *gPosition = nullptr;
SDL_GPUTexture *gNormal   = nullptr;
SDL_GPUTexture *gAlbedo   = nullptr;
SDL_GPUTexture *gDepth    = nullptr;
SDL_GPUGraphicsPipeline *gbufferPipeline = nullptr;
SDL_GPUSampler *pointSampler = nullptr;
```

```cpp
bool Renderer::CreateGBuffer()
{
    // COLOR_TARGET so the raster pass can write them, SAMPLER so the compute
    // pass can read them back. SDL_gpu inserts the layout transition between
    // the two passes for you - there is no manual barrier to get wrong.
    const struct { SDL_GPUTexture **texture; SDL_GPUTextureFormat format; const char *name; } targets[] = {
        {&gPosition, SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT, "gPosition"},
        {&gNormal,   SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, "gNormal"},
        {&gAlbedo,   SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,     "gAlbedo"},
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
    // texture and a pipeline that never validates.
    if (!SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
                                      SDL_GPU_TEXTURETYPE_2D,
                                      SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET))
    {
        SDL_Log("D32_FLOAT depth targets are not supported on this device");
        return false;
    }

    SDL_GPUTextureCreateInfo depthInfo = {};
    depthInfo.type = SDL_GPU_TEXTURETYPE_2D;
    depthInfo.width = config.width;
    depthInfo.height = config.height;
    depthInfo.layer_count_or_depth = 1;
    depthInfo.num_levels = 1;
    depthInfo.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    depthInfo.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    gDepth = SDL_CreateGPUTexture(device, &depthInfo);

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

    return gDepth != nullptr && pointSampler != nullptr;
}
```

**The graphics pipeline.**

```cpp
// Loads one graphics-stage shader. Same file-probing dance as
// CreateComputePipeline, factored out because there are now three of them.
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

bool Renderer::CreateGBufferPipeline()
{
    SDL_GPUShader *vs = LoadShader("gbuffer.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 0);
    SDL_GPUShader *fs = LoadShader("gbuffer.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1, 0);
    if (!vs || !fs)
    {
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
    attributes[0].offset = 0;
    attributes[1].location = 1;
    attributes[1].buffer_slot = 0;
    attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    attributes[1].offset = 12;
    attributes[2].location = 2;
    attributes[2].buffer_slot = 0;
    attributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attributes[2].offset = 24;

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

    // Culling OFF for now. Mesh::CreateBox/CreateSphere have not been audited
    // for winding order, and a wrong front_face silently deletes half your
    // scene. Turn it on in Phase 6 once you can see what you are culling.
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
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

    // The pipeline holds its own reference once created, so these can go.
    SDL_ReleaseGPUShader(device, vs);
    SDL_ReleaseGPUShader(device, fs);

    if (!gbufferPipeline)
    {
        SDL_Log("Failed to create G-buffer pipeline: %s", SDL_GetError());
        return false;
    }
    return true;
}
```

### 5.5 Knowing what to draw

`PackObjects()` already walks the ordered entity list and emits a
`MeshInstance_GPU` per mesh entity. It knows the object slot and the instance
slot at the same moment - which is exactly the pair the draw loop needs. Record
it there rather than recomputing it:

```cpp
// renderer/renderer.h - one draw call's worth of state, built during the same
// pass that packs the object buffer because that is the only place where the
// object slot and the mesh range are both in hand.
struct RasterDraw
{
    Uint32 indexBase;      // first index, in elements
    Uint32 indexCount;     // triangleCount * 3
    Uint32 vertexBase;     // added to every index by the vertex_offset argument
    Uint32 objectIndex;    // slot in the object buffer; travels to the G-buffer
    float  px, py, pz;
    float  qx, qy, qz, qw;
    float  albedoR, albedoG, albedoB;
    Uint32 textureID;
};
std::vector<RasterDraw> rasterDraws;
```

Inside `PackObjects`, in the `if (const auto *mesh = ...)` branch, right after
`outInstances.push_back(inst)`:

```cpp
RasterDraw draw = {};
draw.indexBase   = range.indexBase;
draw.indexCount  = range.triangleCount * 3;
draw.vertexBase  = range.vertexBase;
draw.objectIndex = static_cast<Uint32>(list.size());  // this object's slot
draw.px = t.position.x; draw.py = t.position.y; draw.pz = t.position.z;
draw.qx = t.rotation.x; draw.qy = t.rotation.y; draw.qz = t.rotation.z; draw.qw = t.rotation.w;
draw.albedoR = mat.albedo.x; draw.albedoG = mat.albedo.y; draw.albedoB = mat.albedo.z;
draw.textureID = /* whatever EntityToGPU uses; kNoTexture when absent */;
outDraws.push_back(draw);
```

(`PackObjects` is currently `static` and returns the object vector; give it an
`std::vector<RasterDraw> &outDraws` out-parameter next to `outInstances`, the
same way instances are already threaded through.)

### 5.6 The render pass

```cpp
void Renderer::RenderGBuffer(SDL_GPUCommandBuffer *commandBuffer)
{
    SDL_GPUColorTargetInfo colorTargets[3] = {};

    colorTargets[0].texture = gPosition;
    colorTargets[0].load_op = SDL_GPU_LOADOP_CLEAR;
    colorTargets[0].store_op = SDL_GPU_STOREOP_STORE;
    colorTargets[0].clear_color = {0.0f, 0.0f, 0.0f, 0.0f};

    colorTargets[1].texture = gNormal;
    colorTargets[1].load_op = SDL_GPU_LOADOP_CLEAR;
    colorTargets[1].store_op = SDL_GPU_STOREOP_STORE;
    // Coverage clears to 0. That zero IS the background test in the compute
    // pass - a pixel the rasterizer never touched reports "no surface here"
    // rather than "a surface with a zero normal", which would shade as black
    // instead of as sky.
    colorTargets[1].clear_color = {0.0f, 0.0f, 0.0f, 0.0f};

    colorTargets[2].texture = gAlbedo;
    colorTargets[2].load_op = SDL_GPU_LOADOP_CLEAR;
    colorTargets[2].store_op = SDL_GPU_STOREOP_STORE;
    colorTargets[2].clear_color = {0.0f, 0.0f, 0.0f, 0.0f};

    SDL_GPUDepthStencilTargetInfo depthTarget = {};
    depthTarget.texture = gDepth;
    depthTarget.clear_depth = 1.0f;              // far plane; pairs with COMPAREOP_LESS
    depthTarget.load_op = SDL_GPU_LOADOP_CLEAR;
    depthTarget.store_op = SDL_GPU_STOREOP_STORE; // Phase 4 and Phase 6 read it back
    depthTarget.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depthTarget.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    depthTarget.cycle = true;

    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commandBuffer, colorTargets, 3, &depthTarget);
    if (!pass)
    {
        SDL_Log("Failed to begin G-buffer pass: %s", SDL_GetError());
        return;
    }

    SDL_BindGPUGraphicsPipeline(pass, gbufferPipeline);

    // The whole mesh library is one vertex buffer and one index buffer, bound
    // once. Per-draw offsets do the rest - that is what MeshRange's bases are
    // for, and it is why there is no per-mesh buffer to rebind.
    SDL_GPUBufferBinding vertexBinding = {};
    vertexBinding.buffer = buffers.vertexBuffer;
    SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);

    SDL_GPUBufferBinding indexBinding = {};
    indexBinding.buffer = buffers.indexBuffer;
    SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

    SDL_GPUTextureSamplerBinding textureBinding = {};
    textureBinding.texture = globalTextureArray;
    textureBinding.sampler = linearSampler;
    SDL_BindGPUFragmentSamplers(pass, 0, &textureBinding, 1);

    struct VSUniform
    {
        Mat4 viewProj;
        float rotation[4];
        float position[4];
    };
    struct FSUniform
    {
        float albedo[4];
        Uint32 textureID;
        Uint32 objectIndex;
        Uint32 pad0;
        Uint32 pad1;
    };

    for (const RasterDraw &draw : rasterDraws)
    {
        VSUniform vsu = {};
        vsu.viewProj = cameraMatrices.viewProj;
        vsu.rotation[0] = draw.qx; vsu.rotation[1] = draw.qy;
        vsu.rotation[2] = draw.qz; vsu.rotation[3] = draw.qw;
        vsu.position[0] = draw.px; vsu.position[1] = draw.py; vsu.position[2] = draw.pz;
        SDL_PushGPUVertexUniformData(commandBuffer, 0, &vsu, sizeof(vsu));

        FSUniform fsu = {};
        fsu.albedo[0] = draw.albedoR; fsu.albedo[1] = draw.albedoG; fsu.albedo[2] = draw.albedoB;
        fsu.textureID = draw.textureID;
        fsu.objectIndex = draw.objectIndex;
        SDL_PushGPUFragmentUniformData(commandBuffer, 0, &fsu, sizeof(fsu));

        // first_index and vertex_offset are what let every mesh share one pair
        // of buffers. vertex_offset is added to each index AFTER the fetch, so
        // the library's mesh-relative indices are used exactly as stored - the
        // same trick the BLAS traversal plays with indexBase and vertexBase.
        SDL_DrawGPUIndexedPrimitives(pass,
                                     draw.indexCount,   // num_indices
                                     1,                 // num_instances
                                     draw.indexBase,    // first_index
                                     draw.vertexBase,   // vertex_offset
                                     0);                // first_instance
    }

    SDL_EndGPURenderPass(pass);
}
```

Call it from `RenderFrame`, **before** the compute pass, in the same command
buffer.

### 5.7 Look at it

Do not wire the compute pass to it yet. Temporarily blit `gAlbedo` to the
swapchain instead of `displayTexture`:

```cpp
blit.source.texture = gAlbedo;   // TEMPORARY
```

You should see flat unlit geometry in the right places. Then swap to `gNormal`
(you will see the classic pastel normal map - a `-1..1` normal displayed as
`0..1` looks washed out, that is expected) and to `gPosition` (a smooth
rainbow gradient across the scene).

**Checklist for this phase:**

- Geometry appears where the ray-traced version put it. If it is mirrored
  left-right, your `cross(up, w)` handedness slipped. Upside down, `m[1][1]` -
  under SDL_gpu it must be **positive** (NDC +Y up on every backend).
- Nothing appears at all: is `rasterDraws` non-empty? Log its size. If your test
  scene is all spheres and quads and has zero `MeshComponent` entities, there is
  literally nothing to rasterize - see Phase 4, and consider using
  `Mesh::CreateSphere` in the builder so you have something to look at.
- Everything appears but z-fighting flickers: depth clear or compare op is wrong.

---

## 6. Phase 3 - The deferred compute pass (the payoff)

This is where the two halves meet. Copy `ray_trace.comp.hybrid.hlsl` to
`deferred.comp.hlsl` and change three things: the bindings, the primary hit, and
the shadow ray. Everything else stays.

### 6.1 The register shuffle

Adding sampled textures pushes every storage buffer down by the same count. The
implementation binds **four** - the three G-buffer targets plus `gDepth`, which
§9.1's depth reconstruction needs - so the storage buffers move by four. Here is
the table - transcribe it carefully, because getting it wrong produces a black
screen with no validation message.

```hlsl
// ---- sampled textures come FIRST in space0 ----
Texture2DArray    GlobalTextureArray : register(t0, space0);
Texture2D<float4> gPosition          : register(t1, space0);
Texture2D<float4> gNormal            : register(t2, space0);
Texture2D<float4> gAlbedo            : register(t3, space0);
Texture2D<float>  gDepth             : register(t4, space0);
SamplerState      GlobalSampler      : register(s0, space0);
// No SamplerState is needed for t1..t4: they are only ever read with Load().

// ---- then storage buffers, all shifted by +4 ----
StructuredBuffer<Object>       objects   : register(t5, space0);   // was t1
StructuredBuffer<BVHNode>      bvh       : register(t6, space0);   // was t2
StructuredBuffer<uint>         lightIDs  : register(t7, space0);   // was t3
StructuredBuffer<Vertex>       vertices  : register(t8, space0);   // was t4
StructuredBuffer<uint>         meshIndex : register(t9, space0);   // was t5
StructuredBuffer<BVHNode>      blas      : register(t10, space0);  // was t6
StructuredBuffer<MeshInstance> instances : register(t11, space0);  // was t7

// ---- read-write, unchanged ----
[[vk::image_format("rgba16f")]] RWTexture2D<float4> image        : register(u0, space1);
[[vk::image_format("rgba8")]]   RWTexture2D<float4> displayImage : register(u1, space1);

// ---- uniforms, unchanged ----
cbuffer UniformBuffer : register(b0, space2) { /* ... */ };
```

And the matching pipeline creation. **The comment in `CreateComputePipeline`
already warns you these three move together** - honour it:

```cpp
// GlobalTextureArray (t0) plus gPosition, gNormal, gAlbedo and gDepth (t1..t4).
// This count is what shifts every storage buffer register down by four in the
// shader; the two numbers below and the bind arrays in RenderFrame all move
// together.
cpci.num_samplers = 5;
cpci.num_readonly_storage_buffers = 7;   // now at t5..t11
cpci.num_readwrite_storage_textures = 2;
cpci.num_uniform_buffers = 1;
```

and in `RenderFrame`:

```cpp
SDL_GPUTextureSamplerBinding samplerBindings[5] = {};
samplerBindings[0].texture = globalTextureArray; samplerBindings[0].sampler = linearSampler;
// Point-sampled, always: see the note on CreateGBuffer's pointSampler. A
// linear tap across a silhouette invents a world position that lies on neither
// of the two surfaces it blends.
samplerBindings[1].texture = gPosition; samplerBindings[1].sampler = pointSampler;
samplerBindings[2].texture = gNormal;   samplerBindings[2].sampler = pointSampler;
samplerBindings[3].texture = gAlbedo;   samplerBindings[3].sampler = pointSampler;
samplerBindings[4].texture = gDepth;    samplerBindings[4].sampler = pointSampler;
SDL_BindGPUComputeSamplers(computePass, 0, samplerBindings, 5);
```

The storage buffer bind array does **not** change: `SDL_BindGPUComputeStorageBuffers`
slots count from 0 within their own kind, whatever `t`-register they land on.

> If you want to skip this whole shuffle: bind the G-buffer as three
> **read-only storage textures** instead and read them with `.Load()`. They
> still land after sampled textures in `space0`, so the storage buffers still
> shift - there is no arrangement that avoids it. Better to do it once,
> carefully, with the table above in front of you.

### 6.2 Reading a `Hit` back out of the G-buffer

```hlsl
// Reconstitutes the ray tracer's Hit from what the rasterizer wrote. This is
// the seam of the whole renderer: everything above it is triangles and
// matrices, everything below it is rays and BVHs, and this function is the only
// thing that has to know both.
//
// Note what is NOT stored: the material. gPosition.w carries the object's slot,
// and objects[] is still bound, so one float buys back albedo, fuzz,
// refraction, emission, shape type and texture id - which would otherwise cost
// two more render targets.
bool HitFromGBuffer(uint2 id, float3 rayDirection, out Hit hit)
{
    hit = (Hit)0;

    // Load(), not Sample(): integer texel addressing with no filtering and no
    // dependence on a sampler being bound correctly. The third component of
    // the coordinate is the mip level.
    const float4 packedNormal = gNormal.Load(int3(id, 0));

    // .w is the coverage flag the render pass cleared to 0. A pixel the
    // rasterizer never touched fails here and the caller draws sky.
    if (packedNormal.w < 0.5f)
    {
        return false;
    }

    const float4 packedPosition = gPosition.Load(int3(id, 0));
    const uint objectIndex = (uint)(packedPosition.w + 0.5f);

    hit.Position = packedPosition.xyz;
    hit.Normal   = normalize(packedNormal.xyz);
    hit.Object   = objects[objectIndex];
    hit.Offset   = length(packedPosition.xyz - Source);

    // The rasterizer already resolved albedo x texture and wrote it to gAlbedo,
    // so surface_uv never has to survive the trip. Zero it and override the
    // albedo instead - SurfaceAlbedo() reads Object.Albedo when TextureID is
    // INVALID, so folding the sampled texture into Albedo and clearing
    // TextureID makes every downstream call agree without a special case.
    hit.surface_uv = float2(0.0f, 0.0f);
    hit.Object.Albedo = gAlbedo.Load(int3(id, 0)).rgb;
    hit.Object.TextureID = INVALID_TEXTURE;

    // The rasterizer does not cull, so a fragment can be a back face. Face is
    // what GetBounce uses to decide which way a dielectric refracts, so it has
    // to be right or glass turns inside out.
    hit.Face = dot(rayDirection, hit.Normal) < 0.0f;
    if (!hit.Face)
    {
        // The ray tracer's invariant: Normal always points back along the
        // incoming ray. TraverseBVH's intersectors maintain it; the rasterizer
        // does not, so restore it here.
        hit.Normal = -hit.Normal;
    }

    return true;
}
```

### 6.3 An any-hit traversal for shadow rays

Your `CalculateDirectLight` currently calls the full `TraverseBVH`, which finds
the **closest** hit. A shadow ray does not care which occluder it found, only
that one exists - so it can bail on the first one and skip the rest of the tree.
On a scene with real occlusion this is commonly a 2-4x saving on the pass that
will now dominate your frame.

Two changes at once here, and the second is a bug fix.

```hlsl
// Any-hit traversal: returns as soon as ANY opaque surface lies between `near`
// and `far`, without finding the closest one. That early-out is the whole point
// - a shadow ray that has found one wall does not care about the second.
//
// The `far` parameter is the bug fix. TraverseBVH hardcodes far = 1e6, so the
// shadow-ray loop in CalculateDirectLight currently treats geometry BEHIND the
// light as an occluder: a surface facing a wall two metres past the lamp is
// shaded as though the wall blocked it. Clamping far to the distance to the
// light is what makes a shadow test actually a shadow test.
bool Occluded(const in Ray ray, const in float near, const in float far)
{
    const float3 invDir = 1.0f / ray.Direction;
    int stack[BVH_STACK_SIZE];
    int sp = 0;
    stack[sp++] = 0;

    while (sp > 0)
    {
        const int nodeIndex = stack[--sp];
        const BVHNode node = bvh[nodeIndex];

        if (!IntersectAABB(ray.Origin, invDir, node.Min, node.Max, near, far))
        {
            continue;
        }

        if (node.Count > 0)
        {
            for (int i = 0; i < node.Count; i++)
            {
                const Object object = objects[node.Left + i];

                // Glass and lights are not blockers. Matching the loop in
                // CalculateDirectLight, which steps shadow rays through
                // dielectrics rather than letting them cast hard black
                // shadows - the cheap stand-in for refractive transport.
                if (object.ColorType == DIELECTRIC || object.ColorType == DIFFUSE_LIGHT)
                {
                    continue;
                }

                Hit h;
                if (HitObject(object, ray, near, far, h))
                {
                    return true;   // one is enough
                }
            }
        }
        else
        {
            stack[sp++] = node.Left;
            stack[sp++] = node.Right;
        }
    }
    return false;
}
```

Now replace steps 5 and 6 of `CalculateDirectLight` with:

```cpp
        // 5. One any-hit trace, clamped to the light. The old loop stepped
        // through up to four dielectrics with a full closest-hit traversal
        // each; Occluded skips them in the leaf test instead, so the common
        // case (a clear path, or one wall) is a single traversal.
        Ray shadowRay;
        shadowRay.Origin = surfaceHit.Position + surfaceHit.Normal * 0.001f;
        shadowRay.Direction = shadowRayDir;
        shadowRay.time = originalRay.GetTime();

        // Stop just short of the light so the light's own surface is not
        // counted as the thing blocking it.
        const bool occluded = Occluded(shadowRay, 0.001f, distanceToLight - 0.001f);
```

### 6.4 The new `main()`

```hlsl
[numthreads(THREADS, 1, 1)]
void main(uint3 globalInvocationID : SV_DispatchThreadID)
{
    const uint2 id = globalInvocationID.xy;
    if (id.x >= Width || id.y >= Height)
    {
        return;
    }
    seed = id.x + id.y * Width + Batch * Width * Height + 1u;

    // The camera basis is still built here, unchanged, because secondary rays
    // and the Phase 4 analytic resolve both need the primary ray direction even
    // though the rasterizer found the primary hit. The one thing that is gone
    // is the loop that traced it.
    const float3 vectorW = normalize(Source - Target);
    const float3 vectorU = normalize(cross(Up, vectorW));
    const float3 vectorV = cross(vectorW, vectorU);
    const float viewportH = 2.0f * tan(radians(Fov / 2.0f)) * Focus;
    const float viewportW = viewportH * Width / Height;
    const float3 viewportU = viewportW * vectorU;
    const float3 viewportV = viewportH * -vectorV;
    const float3 pixelU = viewportU / Width;
    const float3 pixelV = viewportV / Height;
    const float3 pixel0 = Source - Focus * vectorW - viewportU / 2.0f - viewportV / 2.0f
                        + pixelU / 2.0f + pixelV / 2.0f;

    // No sub-pixel jitter on the primary ray any more: the G-buffer was
    // rasterized once, at pixel centres, so a jittered primary would describe a
    // different pixel than the one whose surface we are about to shade.
    // Antialiasing moves to the raster side - see section 9.3.
    const float3 pixelCentre = pixel0 + id.x * pixelU + id.y * pixelV;

    Ray primary;
    primary.Origin = Source;
    primary.Direction = normalize(pixelCentre - Source);
    primary.time = Random();

    float3 colour = float3(0.0f, 0.0f, 0.0f);

    Hit hit;
    if (!HitFromGBuffer(id, primary.Direction, hit))
    {
        colour = Sky;
    }
    else
    {
        // From here down this is the body of the old ColorRay's first
        // iteration, verbatim. That is the point of the G-buffer: it hands the
        // integrator the same Hit the primary ray used to.
        if (hit.Object.ColorType == DIFFUSE_LIGHT)
        {
            colour = hit.Object.Emission * hit.Object.Albedo;
        }
        else
        {
            if (hit.Object.ColorType == LAMBERTIAN)
            {
                float3 direct = CalculateDirectLight(hit, primary);
                const float maxContribution = 8.0f;
                const float luma = max(direct.x, max(direct.y, direct.z));
                if (luma > maxContribution)
                {
                    direct *= maxContribution / luma;
                }
                colour += direct;
            }

            colour += TraceIndirect(primary, hit);   // Phase 5
        }
    }

    // Accumulation and tone mapping: unchanged from the compute-only renderer.
    float3 accumulated;
    if (Batch == 0)
    {
        accumulated = colour;
    }
    else
    {
        const float3 previous = image[id].rgb;
        const float weight = 1.0f / float(Batch + 1);
        accumulated = lerp(previous, colour, weight);
    }

    image[id] = float4(accumulated, 1.0f);
    displayImage[id] = float4(LinearToSRGB(ToneMapACES(accumulated * Exposure)), 1.0f);
}
```

For now stub `TraceIndirect` as `return float3(0,0,0);`. **Run it.** You should
see your scene lit by direct light with correct, hard-edged ray-traced shadows,
converging over a few frames. No cascades. No PCF. No bias tuning. That is what
you built this for.

### 6.5 Depth of field, honestly

Your ray camera does thin-lens DOF (`defocus_disk_sample`), and a rasterized
G-buffer is a pinhole. You have three options:

1. **Drop it.** `settings.defocus_angle = 0`. Simplest; do this first.
2. **Jitter the camera per frame.** Offset `Source` by a defocus-disk sample
   each frame and rebuild the matrices, then let temporal accumulation average
   the frames into a real depth of field. Correct and nearly free while the
   camera is still, useless while it is moving. This is the right answer for a
   progressive renderer, and it is about ten lines in `SyncCamera`.
3. Post-process circle-of-confusion blur. Cheap, wrong at edges. Skip it.

---

## 7. Phase 4 - Getting your analytic primitives back

Right now Phase 3 only draws `MeshComponent` entities. Every sphere, quad, box
and triangle in your scenes is invisible - and `BuildTestAllFeatureScene` is
probably made almost entirely of them.

Two ways out. Pick based on what you value.

### Option A: mesh everything (simple, ships)

You already have `Mesh::CreateBox()` and `Mesh::CreateSphere()`. Add a scene
post-process that walks the registry, tessellates every analytic shape into the
`MeshLibrary`, and swaps the shape component for a `MeshComponent`.

Cost: a sphere becomes a faceted approximation, and your BVH grows from one
leaf per sphere to one per triangle. Benefit: one code path, no visibility
resolve, done in an afternoon.

### Option B: hybrid visibility resolve (keeps them exact)

This is the more interesting answer, and it is not much code. **Trace a primary
ray, but only as far as the rasterizer's depth.** Anything the BVH finds nearer
than the rasterized surface is an analytic primitive in front of it.

You need a `far`-bounded traversal that can also skip meshes:

```hlsl
// TraverseBVH with a caller-supplied far bound and an option to ignore mesh
// instances. Both exist for the same job: resolving analytic primitives against
// a rasterized depth buffer.
//
// skipMeshes matters because meshes are in BOTH representations. Without it a
// mesh triangle would be found at essentially the same t as the rasterized
// fragment covering it, and which one wins comes down to floating-point luck -
// which shows up as a shimmering, per-pixel flicker along every mesh surface.
bool TraverseBVHBounded(const in Ray ray, const in float near, const in float farLimit,
                        const in bool skipMeshes, out Hit hit)
{
    const float3 invDir = 1.0f / ray.Direction;
    int stack[BVH_STACK_SIZE];
    int sp = 0;
    stack[sp++] = 0;

    bool found = false;
    float far = farLimit;

    while (sp > 0)
    {
        const int nodeIndex = stack[--sp];
        const BVHNode node = bvh[nodeIndex];

        if (!IntersectAABB(ray.Origin, invDir, node.Min, node.Max, near, far))
        {
            continue;
        }

        if (node.Count > 0)
        {
            for (int i = 0; i < node.Count; i++)
            {
                const Object object = objects[node.Left + i];
                if (skipMeshes && object.ShapeType == MeshShapeType)
                {
                    continue;
                }

                Hit h;
                if (HitObject(object, ray, near, far, h))
                {
                    far = h.Offset;
                    hit = h;
                    found = true;
                }
            }
        }
        else
        {
            stack[sp++] = node.Left;
            stack[sp++] = node.Right;
        }
    }
    return found;
}
```

Then the primary hit becomes a resolve between the two:

```hlsl
// Primary visibility, decided between the rasterizer and the BVH. The
// rasterizer owns triangles; the BVH owns everything analytic. Whichever is
// nearer wins - which is just a depth test with one side of it traced.
bool ResolvePrimary(uint2 id, const in Ray primary, out Hit hit)
{
    Hit rasterHit;
    const bool hasRaster = HitFromGBuffer(id, primary.Direction, rasterHit);

    // No raster coverage means the analytic trace is unbounded, so this pixel
    // costs exactly what it did before the rasterizer existed. Skies and
    // sphere-only scenes are unaffected by the hybrid split.
    const float farLimit = hasRaster ? rasterHit.Offset : 1000000.0f;

    Hit analyticHit;
    if (TraverseBVHBounded(primary, 0.001f, farLimit, true, analyticHit))
    {
        hit = analyticHit;   // an analytic primitive is in front
        return true;
    }

    hit = rasterHit;
    return hasRaster;
}
```

Swap `HitFromGBuffer(id, primary.Direction, hit)` for
`ResolvePrimary(id, primary, hit)` in `main()` and you are done.

**What this costs you:** the analytic traversal still runs for every pixel. But
it is dramatically cheaper than the original primary trace, because `far` is
clamped to the rasterized depth - the `IntersectAABB` test at the top of every
node rejects the entire half of the tree behind the visible surface. In a scene
where meshes cover most of the screen, most pixels reject the root's children
immediately.

**What it does not solve:** an analytic primitive *behind* a mesh in the
G-buffer is correctly hidden, but an analytic primitive that a mesh partially
occludes gets the mesh's rasterized silhouette - which is right. Transparency
ordering is still not solved by any of this; dielectrics get their look from
Phase 5's bounce rays, not from the primary resolve.

### My recommendation

Do **B**. Your engine's whole identity is exact analytic intersection, and
throwing that away to rasterize a sphere as 2048 triangles is the wrong trade
for a renderer that already has the BVH. But do it *after* Phase 3 is working -
debugging a visibility resolve on top of a lighting pass you have not verified
is how a week disappears.

---

## 8. Phase 5 - Reflections and GI

The G-buffer gave you bounce zero for free. Bounces one and up are ordinary path
tracing, and `ColorRay` already does it - you just start it one bounce in.

```hlsl
// Indirect light from a G-buffer surface. This is ColorRay() with the first
// iteration removed, because the rasterizer already did it: the throughput
// starts at the surface albedo instead of white, and the loop begins with the
// bounce ray rather than the camera ray.
//
// Depth - 1 bounces, not Depth: bounce zero was the primary hit.
float3 TraceIndirect(const in Ray primary, const in Hit surface)
{
    float3 bounceDirection;
    if (!GetBounce(primary, surface, bounceDirection))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    Ray ray;
    ray.Direction = normalize(bounceDirection);
    // Offset along the direction the ray is LEAVING in, not along the normal:
    // a refracted ray travels into the surface, and pushing it along +Normal
    // puts its origin on the wrong side of the boundary. Same reasoning, and
    // the same sign trick, as the original ColorRay loop.
    const float offsetSign = (dot(ray.Direction, surface.Normal) < 0.0f) ? -1.0f : 1.0f;
    ray.Origin = surface.Position + surface.Normal * (0.001f * offsetSign);
    ray.time = primary.time;

    float3 throughput = (surface.Object.ColorType != DIELECTRIC)
                      ? surface.Object.Albedo
                      : float3(1.0f, 1.0f, 1.0f);
    float3 accumulated = float3(0.0f, 0.0f, 0.0f);

    for (uint bounce = 1; bounce < Depth; bounce++)
    {
        Hit hit;
        if (!TraverseBVH(ray, 0.001f, hit))
        {
            accumulated += throughput * Sky;
            break;
        }

        if (hit.Object.ColorType == DIFFUSE_LIGHT)
        {
            accumulated += throughput * (hit.Object.Emission * hit.Object.Albedo);
            break;
        }

        if (hit.Object.ColorType == LAMBERTIAN)
        {
            float3 direct = CalculateDirectLight(hit, ray);
            const float maxContribution = 8.0f;
            const float luma = max(direct.x, max(direct.y, direct.z));
            if (luma > maxContribution)
            {
                direct *= maxContribution / luma;
            }
            accumulated += throughput * direct;
        }

        float3 next;
        if (!GetBounce(ray, hit, next))
        {
            break;
        }

        ray.Direction = normalize(next);
        const float sign = (dot(ray.Direction, hit.Normal) < 0.0f) ? -1.0f : 1.0f;
        ray.Origin = hit.Position + hit.Normal * (0.001f * sign);

        if (hit.Object.ColorType != DIELECTRIC)
        {
            throughput *= hit.Object.Albedo;
        }

        // Russian roulette, unchanged from ColorRay.
        float continueProbability = max(throughput.x, max(throughput.y, throughput.z));
        continueProbability = clamp(continueProbability, 0.1f, 1.0f);
        if (Random() > continueProbability)
        {
            break;
        }
        throughput /= continueProbability;
    }

    return accumulated;
}
```

That is the whole of Phase 5. Metal surfaces now reflect - including geometry
off the side of the screen, which SSR could never do. Lambertian surfaces pick
up colour bleeding. Dielectrics refract.

### Why the reflections are still ray traced and not screen-space

Worth saying out loud, because it is the single clearest argument for the whole
architecture: a screen-space reflection can only reflect pixels that are already
on screen. Stand a mirror facing the camera and SSR reflects the back of
nothing. `TraverseBVH` does not care where the geometry is. You are paying for a
BVH anyway; spend it here.

---

## 9. Phase 6 - Performance

Roughly in order of payoff.

### 9.1 Drop `gPosition`, reconstruct from depth

`R32G32B32A32_FLOAT` at 1080p is 33 MB written and read every frame. Depth is
already there. Replace it with:

```hlsl
// World position from depth, reconstructed along the SAME ray the camera basis
// above produced. Deliberately not an inverse-view-projection multiply: this
// route shares its inputs with the ray tracer, so a camera change cannot make
// the reconstruction and the rays disagree. An inverse matrix is a second
// definition of the camera, and second definitions drift.
float3 WorldFromDepth(uint2 id, float3 rayDirection, float3 forward)
{
    const float d = gDepth.Load(int3(id, 0)).r;

    // Inverse of PerspectiveRH_ZO's z row. ZNear/ZFar arrive as new uniforms.
    const float viewZ = (ZNear * ZFar) / (ZFar + d * (ZNear - ZFar));

    // viewZ is measured along the camera's forward axis, but the ray leaves at
    // an angle, so it travels 1/cos further to reach the same plane.
    const float t = viewZ / dot(rayDirection, forward);
    return Source + rayDirection * t;
}
```

with `forward = -vectorW`. You still need the object index somewhere: move it
into `gAlbedo.a` (8 bits, so max 255 objects - fine for now) or add a small
`R32_UINT` target, which is 8 MB instead of 33.

Keep the `gPosition` path behind a `#define` until the reconstructed version
matches it pixel for pixel. Write both, subtract, display the difference
amplified 100x, and confirm it is black.

### 9.2 Half-resolution ray tracing

Shadows and GI are low-frequency. Dispatch the compute pass at half width and
height into a half-size buffer, then upsample with a depth- and normal-aware
filter (a bilateral joint upsample - reject taps whose depth or normal differ
too much from the full-res pixel). 4x fewer rays for a barely visible loss.
This is what every shipping hybrid renderer does.

Keep the G-buffer at full resolution. The sharp part of the image is the raster
pass; only the rays go half-res.

### 9.3 Antialiasing, now that jitter is gone

Phase 3 removed the sub-pixel jitter, because the G-buffer is rasterized at
pixel centres. Get it back by jittering the **projection matrix** instead:
offset `proj.m[0][2]` and `proj.m[1][2]` by a sub-pixel amount from a Halton
sequence each frame. The rasterizer then samples a different sub-pixel position
every frame, and your existing temporal accumulation resolves it into
antialiasing exactly as it used to. Two lines, and it costs nothing.

```cpp
// Sub-pixel jitter, applied to the projection rather than to the ray. TAA-style
// jitter and this renderer's temporal accumulation are the same mechanism, so
// this is free antialiasing while the camera is still - and while it moves,
// accumulation resets and there is nothing to alias into anyway.
//
// Jitter a COPY of proj - SyncCamera only runs when the camera moves, so
// writing into cameraMatrices.proj would accumulate offsets frame over frame.
// Index + 1 skips Halton(0) = 0.
const Uint32 index = (accumulationFrame % 1024) + 1;
config.jitter_x = Halton(index, 2) - 0.5f;   // pixels, read by deferred.comp
config.jitter_y = Halton(index, 3) - 0.5f;

Mat4 proj = cameraMatrices.proj;
// ndc.x = m00*x/w - m02, so adding to m02 moves the image LEFT by the jitter -
// which puts the pixel centre on the point the jittered ray passes through.
proj.m[0][2] += config.jitter_x * 2.0f / config.width;
// Opposite sign for Y: SDL's NDC is +Y up while pixel rows grow down.
proj.m[1][2] -= config.jitter_y * 2.0f / config.height;
const Mat4 viewProj = proj * cameraMatrices.view;   // this frame's, for RenderGBuffer
```

Make sure the compute pass's primary ray picks up the same jitter
(`pixel0 + (id.x + JitterX) * pixelU + (id.y + JitterY) * pixelV`), or the
rays and the raster will disagree by a sub-pixel - which shows up as soft,
wrong-looking shadow contact.

### 9.4 Enable backface culling

`cull_mode = SDL_GPU_CULLMODE_BACK` once you have verified winding. Halves your
raster vertex work. Check `Mesh::CreateBox` and `Mesh::CreateSphere` produce
counter-clockwise front faces first - flip `front_face` rather than the meshes
if they do not.

### 9.5 Instanced draws

Right now it is one draw call per mesh instance. Bind the `instances` storage
buffer to the vertex shader (`space0`, `t0`), index it with `SV_InstanceID`, and
collapse every instance of the same mesh into one `SDL_DrawGPUIndexedPrimitives`
with `num_instances > 1`. Group `rasterDraws` by mesh handle first. Only worth
doing once you have hundreds of instances.

### 9.6 Ordered BVH traversal

Not hybrid-specific, but it is the cheapest remaining win in your ray tracer.
`TraverseBVH` pushes both children unordered and says so in the comment. Push
the farther child first so the nearer one is popped first, and `far` shrinks
sooner, and more of the tree gets rejected. Compare `ray.Origin[axis]` against
the split, or just compare the two children's near-hit distances from
`IntersectAABB`.

### 9.7 Denoise

Once you have shadows and GI at 1 sample per pixel, the remaining noise is what
stands between this and looking finished. In rough order of effort: temporal
reprojection using the previous frame's `viewProj` (you now have matrices, so
you can compute motion vectors), then an à-trous edge-avoiding wavelet filter
guided by `gNormal` and depth. That is SVGF, and it is a project of its own.

---

## 10. Debug playbook

Keep these as a `debugView` uniform switched from the keyboard - the alternative
is recompiling to find out which of five things is wrong.

| Symptom | Look at | Likely cause |
|---|---|---|
| Black screen, no errors | `rasterDraws.size()`, then blit `gAlbedo` | Register shuffle in §6.1 not applied; storage buffers reading zeroes |
| Geometry upside down | `DebugVerifyCameraAgreement` | Sign of `proj.m[1][1]` (positive under SDL_gpu), or the test's own viewport mapping |
| Geometry mirrored | same | `cross(up, w)` vs `cross(w, up)` |
| Everything too big/small | same | FOV read as half-angle somewhere |
| Scene inside-out and rotated | the `row_major` keyword | Matrix transposed by HLSL's column-major default |
| Shadows offset by a pixel | jitter applied to one side only | §9.3 |
| Shadow acne | shadow ray origin offset | Raise the `0.001f` in `CalculateDirectLight` step 4, or offset along the light direction instead of the normal |
| Halo of wrong light at silhouettes | sampler filter | `pointSampler` not bound; a LINEAR tap is blending across the edge |
| Shimmer on mesh surfaces | `skipMeshes` in §7 | Analytic and rasterized copies of the same triangle fighting |
| Glass inside out | `hit.Face` in `HitFromGBuffer` | Face derived wrong, or culling on with wrong winding |
| Objects lit but shadows missing entirely | `NumLights`, `lightIDs` | `num_readonly_storage_buffers` count vs registers |

**The single most useful debug view** is a false-colour of
`objects[objectIndex].ColorType` read out of the G-buffer. If that image is
right, your G-buffer round-trip is correct end to end and every remaining bug is
in the lighting.

---

## 11. Three bugs I found while reading your code

Unrelated to the hybrid work, but you should know.

### 11.1 Shadow rays have no far bound

`CalculateDirectLight`, step 5:

```hlsl
if (!TraverseBVH(currentShadowRay, 0.001f, shadowHit))
```

`TraverseBVH` hardcodes `far = 1000000.0f`. So an occluder **behind the light**
counts as blocking it. A wall two metres past a lamp shadows everything the lamp
should be lighting. The `Occluded(ray, near, distanceToLight - 0.001f)` in §6.3
fixes it - and it is worth fixing whether or not you go hybrid.

### 11.2 Direct light is divided by `NumLights` after summing all of them

Same function, last line:

```hlsl
return directLighting / (float)NumLights;
```

The loop above sums the contribution of **every** light. Dividing by the count
after that makes every scene with N lights N times too dark. The division is
only correct if you sample **one** randomly chosen light per pixel per frame
(which is a legitimate and often better strategy - it costs one shadow ray
instead of N, and accumulation averages over the choices). Pick one:

```hlsl
// Either: sum every light, no division.
return directLighting;

// Or: sample one light uniformly and keep the division as the 1/pdf.
const uint i = min((uint)(Random() * NumLights), NumLights - 1u);
// ... single iteration ...
return contribution;   // already scaled by NumLights implicitly via pdf = 1/N
```

For a scene with one light nothing changes, which is presumably why it has
survived.

### 11.3 `Renderer::Shutdown` leaks on the early-out path

```cpp
void Renderer::Shutdown()
{
    nodes.clear();
    if (!device) return;   // <- window is never destroyed, SDL never quits
```

If `SDL_CreateGPUDevice` fails, `main` calls `Shutdown` and this returns before
`SDL_DestroyWindow` / `SDL_Quit`. Minor, but it will bite you the first time you
add a device-creation fallback.

---

## 12. Where to go after this

Once Phases 0-5 are landed you have a genuinely modern renderer architecture:
raster primary visibility, ray-traced shadows/reflections/GI, temporal
accumulation, physically-based tone mapping. The interesting directions from
there:

- **ReSTIR DI.** Your NEE already samples lights by solid angle. Reservoir
  resampling turns "one shadow ray per light" into "one shadow ray total, at
  near-perfect quality" and it is the single biggest quality-per-ray win
  available today.
- **Motion vectors and reprojection.** You now have `viewProj`; store the
  previous frame's and you can reproject accumulation instead of resetting it
  on camera motion. That is the difference between "converges when you stop
  moving" and "converged while you move".
- **A TLAS refit instead of a rebuild.** `RebuildAcceleration` rebuilds from
  scratch whenever anything moves. Refitting node bounds bottom-up is O(n) and
  good enough until the tree's quality degrades.
- **Emissive meshes.** `lightIDs` currently only indexes whole objects. Sampling
  a triangle of an emissive mesh is the next step toward area lights that are
  not quads.

Good luck. Come back with the `DebugVerifyCameraAgreement` output if Phase 1
does not print zeroes - that one is worth getting exactly right before anything
else.
