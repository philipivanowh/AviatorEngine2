// deferred.comp.hlsl - the compute half of the hybrid renderer.
//
// Derived from ray_trace.comp.hybrid.hlsl (docs/hybrid_renderer_guide.md,
// Phase 3) and still sharing every intersector, BRDF and light sampler with it.
// What differs:
//
//   * primary visibility comes from the rasterized G-buffer (HitFromGBuffer),
//     resolved against analytic primitives by a depth-bounded trace
//     (ResolvePrimary) instead of a camera ray through the whole BVH
//   * the register layout: five sampled textures, so every storage buffer sits
//     four registers later than in the compute-only shader
//   * shadow rays are any-hit and bounded by the light (Occluded)
//   * BVH children are visited nearest-first
//   * debug views, selected by the DebugView uniform
//
// The shared code is COPIED, not #included. A fix to an intersector or to the
// light sampler has to land in both files.

#define THREADS 256

// Must match MaterialType in material.h.
#define LAMBERTIAN 0
#define METAL 1
#define DIELECTRIC 2
#define DIFFUSE_LIGHT 3
#define ISOTROPIC 4

#define PATH_TRACING 0
#define RAY_TRACING 1

// Must match BodyShape in object.h.
#define SphereShapeType 0
#define QuadShapeType 1
#define BoxShapeType 2
#define TriangleShapeType 3
#define MeshShapeType 4

// Must match kNoTexture in texture.h. Means "no image texture, use flat albedo".
#define INVALID_TEXTURE 0xFFFFFFFFu
#define TAU 6.2831853f
#define PI 3.14159265f

#define BLAS_STACK_SIZE 24

// Max BVH depth the traversal stack can hold. 32 is comfortable for
// thousands of objects with the median-split builder in bvh.h; bump it
// if you build much larger/unbalanced trees.
#define BVH_STACK_SIZE 32

// Must match DebugView in renderer/renderer.h.
#define DEBUG_FINAL 0
#define DEBUG_ALBEDO 1
#define DEBUG_NORMAL 2
#define DEBUG_POSITION 3
#define DEBUG_MATERIAL 4
#define DEBUG_VISIBILITY 5
#define DEBUG_DEPTH_ERROR 6

struct Ray
{
    float3 Origin;
    float3 Direction;
    float time;

    float GetTime(){
        return time;
    }
};

struct Object
{
    float3 Position;

    // Last frame's transform - the motion vector temporal reprojection follows.
    // The rotation is the vector part of a unit quaternion; the CPU flipped its
    // sign so w >= 0 and can be rebuilt here. See Object_GPU in gpu_types.h.
    float PrevRotX;

    // Last frame's position. Equal to Position for anything that did not move.
    float3 Position2;

    float Radius;

// For quads, the two edge vectors spanning the parallelogram. For boxes, the
    float3 vector_u;
    float PrevRotY;
    float3 vector_v;
    float PrevRotZ;

// //For triangles
//     float3 vertex;
//     float pad3;
    



//For boxes, the half-extents along each axis. For quads, the half-width and
    float3 Half_extends;

    // Scalars, not `float UvRotation; float3 pad;` - a float3 here would be
    // bumped to the next 16-byte boundary and desync this struct from
    // Object_GPU.
    float UvRotation;
    float Density;
    float Emission;
    uint InstanceIndex;
    // How strongly Albedo tints the texture - see SurfaceAlbedo. Unused
    // without a texture.
    float TextureTint;

    float3 Albedo;
    float Fuzz;

    float Refraction;

    uint ShapeType;
    uint ColorType;
    uint TextureID;
};


// Matches BVHNode in bvh.h byte-for-byte.
struct BVHNode
{
    float3 Min;
    float pad0;

    float3 Max;
    int Left;

    int Right;
    int Count;
    int Padding;
    int pad_trailing;
};

struct Hit
{
    float Offset;
    float3 Position;
    float3 Normal;
    float2 surface_uv;
    bool Face;
    Object Object;
    uint materialId;
};


cbuffer UniformBuffer : register(b0, space2)
{
    float3 Source;
    float Fov;
    float3 Target;
    float Focus;
    float Defocus_angle;
    // NOTE: must be three scalars, NOT `float padding[3]`. In a cbuffer every
    // array element is 16-byte aligned, so `float padding[3]` would eat 48
    // bytes (offsets 48..95) and shift everything below it out of sync with
    // Config in main.cpp. Three scalars pack into the same register as
    // Defocus_angle, putting Up back at offset 48 where Config expects it.
    float padding0;
    float padding1;
    float padding2;
    float3 Up;
    float Oof;
    float3 Sky;
    uint Width;
    float3 Horizon;
    uint Height;
    uint Samples;
    uint Batches;
    uint Batch;
    uint Depth;
    uint NumSpheres;
    uint RenderType;
    uint NumLights;
    float Exposure;

    // Hybrid-only tail. Config in renderer.h appends these after exposure; the
    // compute-only shaders end their cbuffer with a padding float right here.
    uint DebugView;
    float ZNear;
    float ZFar;
    // This frame's sub-pixel sample offset, in pixels - the same offset
    // RenderFrame baked into the raster projection.
    float JitterX;
    float JitterY;
    // 1 once accumulation has reached maxSamples: re-tone-map, trace nothing.
    uint DisplayOnly;
    // Per-axis scale of the traced grid: 1 = every pixel, straight into the
    // image; below 1 = into lightingReduced, and upsample.comp finishes.
    float TraceScale;

    // Temporal reprojection - see ReprojectHistory. FrameIndex never resets:
    // seeds, jitter and the traced-texel pattern need new values every frame,
    // moving or not, or the same noise is re-added and never averages out.
    uint FrameIndex;
    uint HistoryReset; // 1 = ignore every pixel's history this frame
    uint CameraMoved;  // 1 = reproject through PrevViewProj; 0 = history is at the same pixel
    float HistoryCap;  // longest history kept while the camera moves
    float padding9;
    // Last traced frame's UNJITTERED view-projection. row_major like
    // gbuffer.vert's ViewProj, because Mat4 is row-major on the CPU.
    row_major float4x4 PrevViewProj;
};

// Mirrors MeshInstance_GPU in scene/gpu_types.h byte for byte. Scalars, not
// float3/float4 - a float3 here aligns to 16 and desyncs the stride, the same
// trap that forced pad0/pad1/pad2 into Object.
struct MeshInstance
{
    float px, py, pz;
    float pad0;
    float qx, qy, qz, qw;
    uint vertexBase;
    uint indexBase;
    uint triangleCount;
    uint blasBase;
};

// Mirrors Vertex_GPU in scene/mesh_library.h byte for byte. Same scalar rule.
struct Vertex
{
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
};

// ---- space0, read-only. SDL_gpu numbers these t-registers by KIND: every
// sampled texture first, then the storage buffers. The six textures after the
// texture array - G-buffer, depth, and last frame's image and geometry for
// reprojection - push every StructuredBuffer to t7..t13, where the compute-only
// shader has them at t1..t7. This order, num_samplers = 7 in
// Renderer::Initialize and the bind arrays in RenderFrame move together; a
// mismatch reads zeroes with no validation error.
Texture2DArray GlobalTextureArray : register(t0, space0);
Texture2D<float4> gPosition : register(t1, space0);        // xyz world position, w object index
Texture2D<float4> gNormal : register(t2, space0);          // xyz world normal, w coverage
Texture2D<float4> gAlbedo : register(t3, space0);          // rgb albedo x texture, a = 0 if view-dependent
Texture2D<float> gDepth : register(t4, space0);            // D32, [0,1], near = 0
Texture2D<float4> previousImage : register(t5, space0);    // last frame's accumulation
Texture2D<float4> previousGeometry : register(t6, space0); // last frame's xyz position, w history length
SamplerState GlobalSampler : register(s0, space0);

StructuredBuffer<Object> objects : register(t7, space0);
StructuredBuffer<BVHNode> bvh : register(t8, space0);
StructuredBuffer<uint> lightIDs : register(t9, space0);

// Mesh geometry: one global buffer per kind with every mesh concatenated into
// it, and a MeshInstance saying where its own mesh starts.
StructuredBuffer<Vertex> vertices : register(t10, space0);
StructuredBuffer<uint> meshIndex : register(t11, space0);
StructuredBuffer<BVHNode> blas : register(t12, space0);
StructuredBuffer<MeshInstance> instances : register(t13, space0);

// ---- space1, read-write.
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> image : register(u0, space1);

// The display target. Holds sRGB-ENCODED 8-bit values, written once per frame
// from the linear accumulation above - or a debug view, written raw.
[[vk::image_format("rgba8")]]
RWTexture2D<float4> displayImage : register(u1, space1);

// Reduced-resolution lighting (TraceScale below 1): one traced texel per grid
// cell, with albedo divided out where alpha is 1 - see the end of main, and
// upsample.comp.hlsl for the other half. Always bound; the full-res path just
// never writes it.
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> lightingReduced : register(u2, space1);

// This frame's surface position and history length per pixel - next frame's
// previousGeometry. Written by whichever pass writes `image`: this one at full
// resolution, upsample.comp below it.
[[vk::image_format("rgba32f")]]
RWTexture2D<float4> currentGeometry : register(u3, space1);

// ACES filmic curve (Narkowicz's fit). Rolls highlights off smoothly instead of
// clipping them, which is the difference between a bright surface reading as
// "brightly lit brick" and as a flat white blob. Applied to LINEAR radiance.
float3 ToneMapACES(float3 x)
{
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// Linear -> sRGB. The swapchain is a plain UNORM surface the display treats as
// sRGB, so without this the midtones come out too dark.
float3 LinearToSRGB(float3 c)
{
    c = saturate(c);
    const float3 lo = c * 12.92f;
    const float3 hi = 1.055f * pow(c, 1.0f / 2.4f) - 0.055f;
    return float3(c.x <= 0.0031308f ? lo.x : hi.x,
                  c.y <= 0.0031308f ? lo.y : hi.y,
                  c.z <= 0.0031308f ? lo.z : hi.z);
}

static uint seed;

// https://www.reedbeta.com/blog/hash-functions-for-gpu-rendering/
float Random()
{
    seed = seed * 747796405u + 2891336453u;
    const uint x = ((seed >> ((seed >> 28u) + 4u)) ^ seed) * 277803737u;
    return float((x >> 22u) ^ x) / 4294967296.0f;
}

float3 RandomDirection()
{
    const float z = Random() * 2.0f - 1.0f;
    const float a = Random() * TAU;
    const float r = sqrt(1.0f - z * z);
    return float3(r * cos(a), r * sin(a), z);
}

// Same convention as RotateVectorY in object.h, so a yaw baked on the CPU and
// one undone here agree. Takes sin/cos rather than the angle so callers can
// compute the pair once and use it for several vectors.
float3 RotateAboutY(float3 v, float sine, float cosine)
{
    return float3(cosine * v.x + sine * v.z, v.y, -sine * v.x + cosine * v.z);
}

// Spherical parameterisation of a unit-length surface normal. `rotation` spins
// the texture around the Y axis - a sphere's geometry is unchanged by rotating
// it, so this is the only place a sphere's rotation shows up.
float2 get_sphere_uv(float3 p, float rotation)
{
    const float theta = acos(-p.y);
    const float phi = atan2(-p.z, p.x) + PI;

    // frac() keeps u in [0,1) for negative rotations too.
    return float2(frac(phi / TAU - rotation / TAU), theta / PI);
}


// ---------------------------------------------------------------------------
// Spans
//
// Every closed shape answers one question first: where does the ray enter and
// leave the solid? Both the surface hit and the volume hit are built on top of
// that answer, which is what makes participating media cheap to support - a
// medium needs the whole span, not just the nearest crossing.
// ---------------------------------------------------------------------------

// Both roots of the ray/sphere quadratic. Doesn't clamp to [near, far] - the
// callers decide what to do with the span.
bool SphereSpan(const in Object o, const in Ray ray, out float t0, out float t1)
{
    const float3 centre = lerp(o.Position, o.Position2, ray.GetTime());
    const float3 offset = centre - ray.Origin;
    const float a = dot(ray.Direction, ray.Direction);
    const float b = dot(ray.Direction, offset);
    const float c = dot(offset, offset) - o.Radius * o.Radius;
    const float disc = b * b - a * c;
    if (disc < 0.0f) { t0 = 0.0f; t1 = 0.0f; return false; }
    const float sq = sqrt(disc);
    t0 = (b - sq) / a;
    t1 = (b + sq) / a;
    return true;
}

// Everything the box test produces, in the box's own frame. Kept together so
// the ray only has to be rotated out of world space once per box.
struct BoxLocal
{
    float3 origin;
    float3 direction;
    float tEnter;
    float tExit;
    float3 enterNormal; // box space, pointing back along the ray
    float3 exitNormal;  // box space, pointing along the ray
};

// A box is stored as centre + half-extents + yaw (see class Box in object.h),
// because an axis-aligned min/max pair can't represent a rotated box. Undo the
// yaw and the test is an ordinary slab intersection.
bool BoxIntersect(const in Object box, const in Ray ray, out BoxLocal result)
{
    const float yawSin = sin(box.UvRotation);
    const float yawCos = cos(box.UvRotation);

    // World -> box space: rotate by -yaw about the box centre.
    result.origin = RotateAboutY(ray.Origin - box.Position, -yawSin, yawCos);
    result.direction = RotateAboutY(ray.Direction, -yawSin, yawCos);

    const float3 invD = 1.0f / result.direction;
    const float3 t0 = (-box.Half_extends - result.origin) * invD;
    const float3 t1 = (box.Half_extends - result.origin) * invD;
    const float3 tsmall = min(t0, t1);
    const float3 tbig = max(t0, t1);

    result.tEnter = max(max(tsmall.x, tsmall.y), tsmall.z);
    result.tExit = min(min(tbig.x, tbig.y), tbig.z);

    // Whichever slab produced each crossing names the face. max()/min() return
    // one of their operands exactly, so comparing by == is safe here.
    result.enterNormal = float3(0.0f, 0.0f, -sign(result.direction.z));
    if (result.tEnter == tsmall.x)
        result.enterNormal = float3(-sign(result.direction.x), 0.0f, 0.0f);
    else if (result.tEnter == tsmall.y)
        result.enterNormal = float3(0.0f, -sign(result.direction.y), 0.0f);

    result.exitNormal = float3(0.0f, 0.0f, sign(result.direction.z));
    if (result.tExit == tbig.x)
        result.exitNormal = float3(sign(result.direction.x), 0.0f, 0.0f);
    else if (result.tExit == tbig.y)
        result.exitNormal = float3(0.0f, sign(result.direction.y), 0.0f);

    return result.tExit > result.tEnter;
}

// The span a participating medium fills. Only closed shapes qualify: a quad is
// infinitely thin, so there is no interior to put a medium inside.
bool BoundarySpan(const in Object o, const in Ray ray, out float tEnter, out float tExit)
{
    tEnter = 0.0f;
    tExit = 0.0f;

    if (o.ShapeType == SphereShapeType)
    {
        return SphereSpan(o, ray, tEnter, tExit);
    }

    if (o.ShapeType == BoxShapeType)
    {
        BoxLocal local;
        if (!BoxIntersect(o, ray, local))
        {
            return false;
        }
        tEnter = local.tEnter;
        tExit = local.tExit;
        return true;
    }

    return false;
}


// ---------------------------------------------------------------------------
// Surface hits
// ---------------------------------------------------------------------------

bool HitSphere(
    const in Object object,
    const in Ray ray,
    const in float near,
    const in float far,
    out Hit hit)
{
    float t0, t1;
    if (!SphereSpan(object, ray, t0, t1))
    {
        return false;
    }

    // Near crossing first; fall back to the far one when we start inside.
    float root = t0;
    if (root <= near || far <= root)
    {
        root = t1;
        if (root <= near || far <= root)
        {
            return false;
        }
    }

    // Centre of the object at this ray's sampled time - covers both
    // motion-blur (Position2 = shutter-close position) and per-frame
    // physics motion (Position2 = last frame's position) the same way.
    const float3 currentCenter = lerp(object.Position, object.Position2, ray.GetTime());

    hit.Offset = root;
    hit.Position = ray.Origin + ray.Direction * root;
    hit.Normal = (hit.Position - currentCenter) / object.Radius;
    hit.Face = dot(ray.Direction, hit.Normal) < 0.0f;
    if (!hit.Face)
    {
        hit.Normal = -hit.Normal;
    }
    hit.surface_uv = get_sphere_uv(hit.Normal, object.UvRotation);
    hit.Object = object;

    return true;
}

bool HitQuad(
    const in Object quad,
    const in Ray ray,
    const in float near,
    const in float far,
    out Hit hit)
{
    const float3 quadNormal = cross(quad.vector_u, quad.vector_v);
    const float normalLenSq = dot(quadNormal, quadNormal);

    // Degenerate quad (U and V parallel/zero) - can't form a plane.
    if (normalLenSq < 1e-8f)
        return false;

    const float3 unitNormal = quadNormal / sqrt(normalLenSq);
    const float3 w = quadNormal / normalLenSq;

    const float D = dot(quadNormal, quad.Position);
    const float denom = dot(quadNormal, ray.Direction);

    // Ray parallel to the quad's plane.
    if (abs(denom) < 1e-8f)
        return false;

    const float t = (D - dot(quadNormal, ray.Origin)) / denom;

    // Same role as ray.Contains(t) in the book - reject hits outside
    // the valid [near, far] window for this traversal step.
    if (t < near || t > far)
        return false;

    const float3 intersection = ray.Origin + ray.Direction * t;

    // Plane-local (alpha, beta) coordinates of the hit point.
    const float3 planarHitVector = intersection - quad.Position;
    const float alpha = dot(w, cross(planarHitVector, quad.vector_v));
    const float beta  = dot(w, cross(quad.vector_u, planarHitVector));

    // Inside the [0,1]x[0,1] parallelogram spanned by U and V.
    if (alpha < 0.0f || alpha > 1.0f || beta < 0.0f || beta > 1.0f)
        return false;

    hit.surface_uv = float2(alpha, beta);
    hit.Offset = t;
    hit.Position = intersection;
    hit.Normal = unitNormal;
    hit.Face = dot(ray.Direction, hit.Normal) < 0.0f;
    if (!hit.Face)
    {
        hit.Normal = -hit.Normal;
    }
    hit.Object = quad;
    return true;
}

bool HitTriangle(const in Object tri, const in Ray ray,
                 const in float near, const in float far, out Hit hit)
{
    const float3 n = cross(tri.vector_u, tri.vector_v);
    const float nLenSq = dot(n, n);
    if (nLenSq < 1e-8f) return false;              // degenerate

    const float3 unitNormal = n / sqrt(nLenSq);
    const float3 w = n / nLenSq;

    const float denom = dot(n, ray.Direction);
    if (abs(denom) < 1e-8f) return false;          // parallel

    const float t = (dot(n, tri.Position) - dot(n, ray.Origin)) / denom;
    if (t < near || t > far) return false;

    const float3 p = ray.Origin + ray.Direction * t;
    const float3 planar = p - tri.Position;
    const float alpha = dot(w, cross(planar, tri.vector_v));
    const float beta  = dot(w, cross(tri.vector_u, planar));

    // The only geometric difference from a quad: the far edge is the
    // hypotenuse alpha + beta = 1, not the two lines alpha = 1 and beta = 1.
    if (alpha < 0.0f || beta < 0.0f || alpha + beta > 1.0f) return false;

    hit.surface_uv = float2(alpha, beta);          // barycentric
    hit.Offset = t;
    hit.Position = p;
    hit.Normal = unitNormal;
    hit.Face = dot(ray.Direction, hit.Normal) < 0.0f;
    if (!hit.Face) hit.Normal = -hit.Normal;
    hit.Object = tri;
    return true;
}

// --- Instance transform helpers. HLSL has no quaternion type, so these mirror
// --- the free functions in math/quat.hpp; keep the two in step.

float4 quat_conj(float4 q) { return float4(-q.xyz, q.w); }

// Transliteration of rotate() in math/quat.hpp:212 - keep them identical.
float3 quat_rotate(float4 q, float3 v)
{
    const float3 u = q.xyz;
    const float3 t = 2.0f * cross(u, v);
    return v + q.w * t + cross(u, t);
}

// World ray -> mesh local space.
//
// The instance transform is rigid - rotation plus translation, never scale -
// so the rotated direction keeps its length and `t` means the same thing in
// both spaces. That is why nothing here normalizes: normalizing would rescale
// t per instance and make hits from different instances incomparable.
Ray ToLocal(const in MeshInstance inst, const in Ray ray)
{
    const float4 inv = quat_conj(float4(inst.qx, inst.qy, inst.qz, inst.qw));

    Ray local;
    local.Origin = quat_rotate(inv, ray.Origin - float3(inst.px, inst.py, inst.pz));
    local.Direction = quat_rotate(inv, ray.Direction);
    local.time = ray.time;
    return local;
}

// Slab-test ray/AABB intersection. invDir is precomputed once per ray by
// the caller since it's reused across every node visited.
bool IntersectAABB(
    const in float3 origin,
    const in float3 invDir,
    const in float3 bmin,
    const in float3 bmax,
    const in float near,
    const in float far)
{
    const float3 t0 = (bmin - origin) * invDir;
    const float3 t1 = (bmax - origin) * invDir;
    const float3 tsmall = min(t0, t1);
    const float3 tbig = max(t0, t1);
    const float tmin = max(max(tsmall.x, tsmall.y), max(tsmall.z, near));
    const float tmax = min(min(tbig.x, tbig.y), min(tbig.z, far));
    return tmin <= tmax;
}

// Entry distance of the ray into an AABB, or 1e30 on a miss. The same slab test
// as IntersectAABB, returning tmin so interior children can be ordered by it.
// Defined up here, ahead of HitMesh, because both BVH levels order by it.
float AABBEntry(const in float3 origin, const in float3 invDir,
                const in float3 bmin, const in float3 bmax,
                const in float near, const in float far)
{
    const float3 t0 = (bmin - origin) * invDir;
    const float3 t1 = (bmax - origin) * invDir;
    const float3 tsmall = min(t0, t1);
    const float3 tbig = max(t0, t1);
    const float tmin = max(max(tsmall.x, tsmall.y), max(tsmall.z, near));
    const float tmax = min(min(tbig.x, tbig.y), min(tbig.z, far));
    return (tmin <= tmax) ? tmin : 1e30f;
}

bool HitMeshTriangle(const in Ray ray, const in float near, const in float far,
                     const in Vertex a, const in Vertex b, const in Vertex c,
                     out float t, out float alpha, out float beta, out float3 geoNormal)
{
    // Every early return below leaves these untouched, and an out parameter
    // read after a false return is undefined - initialise up front.
    t = 0.0f;
    alpha = 0.0f;
    beta = 0.0f;
    geoNormal = float3(0.0f, 0.0f, 1.0f);

    const float3 p0 = float3(a.px, a.py, a.pz);
    const float3 e1 = float3(b.px, b.py, b.pz) - p0;
    const float3 e2 = float3(c.px, c.py, c.pz) - p0;

    // Identical to HitTriangle - v0 + two edges, plane test, barycentrics.
    const float3 n = cross(e1, e2);
    const float nLenSq = dot(n, n);
    if (nLenSq < 1e-12f) return false;

    const float denom = dot(n, ray.Direction);
    if (abs(denom) < 1e-12f) return false;

    t = (dot(n, p0) - dot(n, ray.Origin)) / denom;
    if (t < near || t > far) return false;

    const float3 w      = n / nLenSq;
    const float3 planar = ray.Origin + ray.Direction * t - p0;
    alpha = dot(w, cross(planar, e2));
    beta  = dot(w, cross(e1, planar));
    if (alpha < 0.0f || beta < 0.0f || alpha + beta > 1.0f) return false;

    geoNormal = n / sqrt(nLenSq);
    return true;
}

bool HitMesh(const in Object object, const in Ray ray,
             const in float near, const in float far, out Hit hit)
{
    const MeshInstance inst = instances[object.InstanceIndex];
    const Ray   localRay = ToLocal(inst, ray);
    const float3 invDir  = 1.0f / localRay.Direction;

    int stack[BLAS_STACK_SIZE];
    int sp = 0;
    stack[sp++] = 0;

    bool  found     = false;
    float closest   = far;
    float bestAlpha = 0.0f, bestBeta = 0.0f;
    uint  bestBase  = 0;
    float3 bestGeo  = float3(0.0f, 0.0f, 1.0f);

    while (sp > 0)
    {
        const BVHNode node = blas[inst.blasBase + stack[--sp]];

        // `closest`, not `far` - so the walk prunes against hits already found.
        if (!IntersectAABB(localRay.Origin, invDir, node.Min, node.Max, near, closest))
            continue;

        if (node.Count > 0)
        {
            for (int i = 0; i < node.Count; ++i)
            {
                const uint base = inst.indexBase + (uint)(node.Left + i) * 3u;
                const Vertex a = vertices[inst.vertexBase + meshIndex[base + 0]];
                const Vertex b = vertices[inst.vertexBase + meshIndex[base + 1]];
                const Vertex c = vertices[inst.vertexBase + meshIndex[base + 2]];

                float t, alpha, beta;
                float3 geo;
                if (HitMeshTriangle(localRay, near, closest, a, b, c, t, alpha, beta, geo))
                {
                    closest   = t;
                    found     = true;
                    bestAlpha = alpha;
                    bestBeta  = beta;
                    bestBase  = base;
                    bestGeo   = geo;
                }
            }
        }
        else
        {
            // Nearest child first, as TraverseBVHBounded does one level up:
            // its hits shrink `closest` before the farther subtree is opened,
            // and a child the ray misses is never pushed at all.
            const BVHNode left = blas[inst.blasBase + node.Left];
            const BVHNode right = blas[inst.blasBase + node.Right];
            const float tLeft = AABBEntry(localRay.Origin, invDir, left.Min, left.Max, near, closest);
            const float tRight = AABBEntry(localRay.Origin, invDir, right.Min, right.Max, near, closest);
            const bool leftFirst = tLeft <= tRight;

            if (max(tLeft, tRight) < 1e30f)
            {
                stack[sp++] = leftFirst ? node.Right : node.Left;
            }
            if (min(tLeft, tRight) < 1e30f)
            {
                stack[sp++] = leftFirst ? node.Left : node.Right;
            }
        }
    }

    if (!found)
        return false;

    // Re-fetch the winner instead of carrying three Vertex structs through the
    // whole walk - that is a lot of live registers for data only the closest
    // hit ever uses.
    const Vertex a = vertices[inst.vertexBase + meshIndex[bestBase + 0]];
    const Vertex b = vertices[inst.vertexBase + meshIndex[bestBase + 1]];
    const Vertex c = vertices[inst.vertexBase + meshIndex[bestBase + 2]];

    const float  w0 = 1.0f - bestAlpha - bestBeta;
    const float3 shadingLocal = normalize(w0        * float3(a.nx, a.ny, a.nz)
                                        + bestAlpha * float3(b.nx, b.ny, b.nz)
                                        + bestBeta  * float3(c.nx, c.ny, c.nz));

    // Orient the geometric normal to agree with the authored vertex normals.
    //
    // Facing has to be decided by the geometric normal, but its sign comes from
    // triangle winding, and winding is easy to get wrong - CreateSphere had it
    // inverted, so every hit read as a back face and the shading normal was
    // flipped inward, leaving the sphere unlit. Vertex normals come from the
    // authoring tool and always point outward, so they are the more trustworthy
    // reference. This makes a wound-backwards mesh shade correctly instead of
    // silently going black, which matters once meshes are loaded from files
    // rather than generated here.
    float3 geoLocal = bestGeo;
    if (dot(geoLocal, shadingLocal) < 0.0f)
    {
        geoLocal = -geoLocal;
    }

    const float4 q = float4(inst.qx, inst.qy, inst.qz, inst.qw);

    // t is identical in both spaces because the instance transform is rigid -
    // a rotation preserves length - so the world position comes straight off
    // the WORLD ray. No transforming the hit point back.
    hit.Offset   = closest;
    hit.Position = ray.Origin + ray.Direction * closest;
    hit.Normal   = normalize(quat_rotate(q, shadingLocal));

    // Facing is decided by the GEOMETRIC normal. Near silhouettes the shading
    // normal disagrees with the surface, and using it here puts secondary ray
    // origins under the geometry - that is the shadow acne everyone blames on
    // the epsilon.
    hit.Face = dot(ray.Direction, quat_rotate(q, geoLocal)) < 0.0f;
    if (!hit.Face)
        hit.Normal = -hit.Normal;

    hit.surface_uv = w0        * float2(a.u, a.v)
                   + bestAlpha * float2(b.u, b.v)
                   + bestBeta  * float2(c.u, c.v);
    hit.Object = object;
    return true;
}

// Any-hit twin of HitMesh for shadow rays: true as soon as ANY triangle lies in
// [near, far]. HitMesh keeps walking to find the closest triangle, then
// interpolates its normal and UV - all wasted on a shadow ray, which only needs
// to know that something is in the way.
bool OccludedMesh(const in Object object, const in Ray ray, const in float near, const in float far)
{
    const MeshInstance inst = instances[object.InstanceIndex];
    const Ray localRay = ToLocal(inst, ray);
    const float3 invDir = 1.0f / localRay.Direction;

    int stack[BLAS_STACK_SIZE];
    int sp = 0;
    stack[sp++] = 0;

    while (sp > 0)
    {
        const BVHNode node = blas[inst.blasBase + stack[--sp]];
        if (!IntersectAABB(localRay.Origin, invDir, node.Min, node.Max, near, far))
        {
            continue;
        }

        if (node.Count > 0)
        {
            for (int i = 0; i < node.Count; ++i)
            {
                const uint base = inst.indexBase + (uint)(node.Left + i) * 3u;
                float t, alpha, beta;
                float3 geo;
                if (HitMeshTriangle(localRay, near, far,
                                    vertices[inst.vertexBase + meshIndex[base + 0]],
                                    vertices[inst.vertexBase + meshIndex[base + 1]],
                                    vertices[inst.vertexBase + meshIndex[base + 2]],
                                    t, alpha, beta, geo))
                {
                    return true; // any triangle blocks
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

// Planar UVs for whichever face was hit, from the two box-space axes tangent
// to it, remapped from [-half, +half] to [0, 1].
float2 BoxFaceUV(float3 localPosition, float3 localNormal, float3 halfExtends)
{
    float2 uv;
    if (abs(localNormal.x) > 0.5f)
    {
        uv = float2(localPosition.z / halfExtends.z, localPosition.y / halfExtends.y);
    }
    else if (abs(localNormal.y) > 0.5f)
    {
        uv = float2(localPosition.x / halfExtends.x, localPosition.z / halfExtends.z);
    }
    else
    {
        uv = float2(localPosition.x / halfExtends.x, localPosition.y / halfExtends.y);
    }
    return saturate(uv * 0.5f + 0.5f);
}

bool HitBox(
    const in Object box,
    const in Ray ray,
    const in float near,
    const in float far,
    out Hit hit)
{
    BoxLocal local;
    if (!BoxIntersect(box, ray, local))
    {
        return false;
    }

    // Entry face first; fall back to the exit face when we start inside.
    float t = local.tEnter;
    float3 localNormal = local.enterNormal;
    bool outside = true;

    if (t <= near || far <= t)
    {
        t = local.tExit;
        localNormal = local.exitNormal;
        outside = false;
        if (t <= near || far <= t)
        {
            return false;
        }
    }

    const float yawSin = sin(box.UvRotation);
    const float yawCos = cos(box.UvRotation);

    hit.Offset = t;
    hit.Position = ray.Origin + ray.Direction * t;
    hit.surface_uv = BoxFaceUV(local.origin + local.direction * t, localNormal, box.Half_extends);
    // Box space -> world.
    hit.Normal = RotateAboutY(localNormal, yawSin, yawCos);
    hit.Face = outside;
    if (!hit.Face)
    {
        hit.Normal = -hit.Normal;
    }
    hit.Object = box;
    return true;
}

// A constant-density participating medium, as in Ray Tracing: The Next Week.
// The book hits its boundary twice because the boundary is an opaque hittable;
// here the boundary is an analytic primitive whose entry and exit both fall out
// of one span test, so one call is enough.
bool HitVolume(
    const in Object object,
    const in Ray ray,
    const in float near,
    const in float far,
    out Hit hit)
{
    float tEnter, tExit;
    if (!BoundarySpan(object, ray, tEnter, tExit))
    {
        return false;
    }

    // Clip the span to the window we're searching. Clamping tEnter to `near` is
    // what makes multiple scattering work: after a scatter the next ray starts
    // *inside* the medium, so tEnter comes back negative.
    tEnter = max(tEnter, near);
    tExit = min(tExit, far);
    if (tEnter >= tExit)
    {
        return false;
    }

    const float rayLength = length(ray.Direction);
    const float insideDistance = (tExit - tEnter) * rayLength;

    // Exponential free-path sampling: the chance of travelling distance d
    // without scattering is exp(-density * d). Random() can return exactly 0,
    // and log(0) is -inf, so floor it - otherwise density 0 gives a NaN rather
    // than the "never scatters" behaviour you want.
    const float hitDistance = -log(max(Random(), 1e-8f)) / max(object.Density, 1e-8f);
    if (hitDistance > insideDistance)
    {
        return false; // passed straight through without interacting
    }

    hit.Offset = tEnter + hitDistance / rayLength;
    hit.Position = ray.Origin + ray.Direction * hit.Offset;
    hit.Normal = float3(0.0f, 1.0f, 0.0f); // arbitrary: isotropic never reads it
    hit.Face = true;
    hit.surface_uv = float2(0.0f, 0.0f);
    hit.Object = object;
    return true;
}

// The one place shape dispatch lives - adding a primitive means adding a branch
// here and nowhere else. Isotropic materials are tested as media rather than
// surfaces, so the material decides *how* the shape is intersected.
bool HitObject(
    const in Object object,
    const in Ray ray,
    const in float near,
    const in float far,
    out Hit hit)
{
    if (object.ColorType == ISOTROPIC)
    {
        return HitVolume(object, ray, near, far, hit);
    }
    if (object.ShapeType == SphereShapeType)
    {
        return HitSphere(object, ray, near, far, hit);
    }
    if (object.ShapeType == QuadShapeType)
    {
        return HitQuad(object, ray, near, far, hit);
    }
    
    if (object.ShapeType == TriangleShapeType)
    {
        return HitTriangle(object, ray, near, far, hit);
    }
    if (object.ShapeType == MeshShapeType)
    {
        return HitMesh(object, ray, near, far, hit);
    }
    return HitBox(object, ray, near, far, hit);
}

// ---------------------------------------------------------------------------
// Traversal
// ---------------------------------------------------------------------------

// Must stay in lockstep with IsRasterProxy in renderer/renderer.h: the analytic
// boxes the G-buffer pass ALSO draws, as a stretched unit cube.
bool IsRasterProxy(const in Object object)
{
    return object.ShapeType == BoxShapeType &&
           object.ColorType != ISOTROPIC &&
           object.TextureID == INVALID_TEXTURE &&
           object.UvRotation == 0.0f;
}

// Closest hit along `ray` in [near, farLimit], optionally ignoring everything
// the rasterizer already drew. Both extras exist for ResolvePrimary - see there.
//
// Children are visited nearest-first (guide section 9.6): the farther child is
// pushed first so the nearer one is popped first, `far` shrinks on its hits
// before the farther subtree is looked at, and a child the ray misses is never
// pushed at all. Correctness does not depend on the order - only how much of
// the tree gets skipped.
bool TraverseBVHBounded(const in Ray ray, const in float near, const in float farLimit,
                        const in bool skipRasterized, out Hit hit)
{
    hit = (Hit)0;

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

        // Re-tested on pop, not only on push: `far` may have shrunk since this
        // node was pushed, which is exactly where nearest-first pays off.
        if (!IntersectAABB(ray.Origin, invDir, node.Min, node.Max, near, far))
        {
            continue;
        }

        if (node.Count > 0)
        {
            for (int i = 0; i < node.Count; i++)
            {
                const Object object = objects[node.Left + i];

                // Meshes and box proxies exist in BOTH representations in the
                // hybrid renderer. Resolving primary visibility against the
                // G-buffer has to skip them, or the traced surface is found at
                // essentially the same t as the rasterized fragment covering it
                // and the winner is floating-point luck - a per-pixel shimmer.
                if (skipRasterized && (object.ShapeType == MeshShapeType || IsRasterProxy(object)))
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
            const BVHNode left = bvh[node.Left];
            const BVHNode right = bvh[node.Right];
            const float tLeft = AABBEntry(ray.Origin, invDir, left.Min, left.Max, near, far);
            const float tRight = AABBEntry(ray.Origin, invDir, right.Min, right.Max, near, far);

            const bool leftFirst = tLeft <= tRight;
            const int nearChild = leftFirst ? node.Left : node.Right;
            const int farChild = leftFirst ? node.Right : node.Left;

            if (max(tLeft, tRight) < 1e30f)
            {
                stack[sp++] = farChild;
            }
            if (min(tLeft, tRight) < 1e30f)
            {
                stack[sp++] = nearChild;
            }
        }
    }
    return found;
}

// The unbounded closest-hit query every secondary ray uses. Same signature as
// the compute-only shader's TraverseBVH, so the lighting code below is shared
// with it verbatim.
bool TraverseBVH(const in Ray ray, const in float near, out Hit hit)
{
    return TraverseBVHBounded(ray, near, 1000000.0f, false, hit);
}

// Any-hit traversal for shadow rays (guide section 6.3): returns as soon as ANY
// blocking surface lies in [near, far], without finding the closest one. A
// shadow ray that has found one wall does not care about the second.
//
// The far bound is the fix for guide section 11.1: the old shadow loop used the
// unbounded TraverseBVH, so an occluder BEHIND the light counted as blocking it.
bool Occluded(const in Ray ray, const in float near, const in float far)
{
    const float3 invDir = 1.0f / ray.Direction;
    int stack[BVH_STACK_SIZE];
    int sp = 0;
    stack[sp++] = 0;

    while (sp > 0)
    {
        const BVHNode node = bvh[stack[--sp]];

        if (!IntersectAABB(ray.Origin, invDir, node.Min, node.Max, near, far))
        {
            continue;
        }

        if (node.Count > 0)
        {
            for (int i = 0; i < node.Count; i++)
            {
                const Object object = objects[node.Left + i];

                // Glass and lights are not blockers - the same rule as the old
                // loop, which stepped shadow rays through dielectrics rather
                // than let them cast hard black shadows.
                if (object.ColorType == DIELECTRIC || object.ColorType == DIFFUSE_LIGHT)
                {
                    continue;
                }

                // Meshes take the any-hit walk: HitObject would find the
                // closest triangle and shade it, for a ray that only needs yes
                // or no.
                if (object.ShapeType == MeshShapeType)
                {
                    if (OccludedMesh(object, ray, near, far))
                    {
                        return true; // one is enough
                    }
                    continue;
                }

                Hit h;
                if (HitObject(object, ray, near, far, h))
                {
                    return true; // one is enough
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

float3 textureColor(uint textureID, float2 uv)
{
    return GlobalTextureArray.SampleLevel(
        GlobalSampler,
        float3(uv, textureID),
        0
    ).rgb;
}



// The surface colour of a hit: the material's flat albedo, or its image texture
// TINTED by that albedo. TextureTint says how much: 0 shows the texture exactly
// as authored, 1 is a full multiply (texture * albedo), and the default set in
// material.h sits well toward 0 - a saturated albedo at full strength strips
// most of the texture's own colour out.
//
// This is the ONLY place a texture meets albedo. Direct light, emission and the
// bounce throughput all go through it; the bounce loop used to multiply by the
// flat Albedo instead, so a textured surface lit mostly by bounce light showed
// its material colour with the texture barely visible.
float3 SurfaceAlbedo(const in Hit hit)
{
    if (hit.Object.TextureID == INVALID_TEXTURE)
    {
        return hit.Object.Albedo;
    }

    const float3 texel = textureColor(hit.Object.TextureID, hit.surface_uv);
    return texel * lerp(float3(1.0f, 1.0f, 1.0f), hit.Object.Albedo, hit.Object.TextureTint);
}

// Light leaving the surface toward the ray, independent of any bounce.
// Non-emissive materials contribute nothing.
float3 SurfaceEmission(const in Hit hit)
{
    if (hit.Object.ColorType != DIFFUSE_LIGHT)
    {
        return 0.0f;
    }
    return SurfaceAlbedo(hit) ;
}

// Picks the outgoing direction for one bounce and the throughput to multiply
// into the path. Returns false when the path ends here (absorbed, or the
// material is a light, which emits but never scatters).
bool GetBounce(
    const in Ray ray,
    const in Hit hit,
    out float3 bounce)
{
    bounce = 0.0f;

    switch (hit.Object.ColorType)
    {
    case LAMBERTIAN:
        // Colored indirect bounce restored: this is what puts colored bounce
        // light (GI / color bleeding) into the image instead of leaving
        // everything outside direct light black. Cost is bounded by
        // Russian roulette in ColorRay() rather than by cutting this off
        // entirely - a Whitted-style hard stop here is cheaper but looks
        // flat/monochrome, which is the opposite of what you're going for.
        bounce = hit.Normal + RandomDirection();
        if (dot(bounce, bounce) < 0.001f)
        {
            bounce = hit.Normal;
        }
        return true;

    case METAL:
        bounce = normalize(reflect(ray.Direction, hit.Normal));
        bounce += RandomDirection() * hit.Object.Fuzz;
        return dot(bounce, hit.Normal) > 0.0f;

    case DIELECTRIC:
    {
        // Refraction ratio: entering the surface divides by the IOR, leaving
        // it multiplies. hit.Face is true when the ray hits the outside.
        const float ior = hit.Object.Refraction;
        const float ri = hit.Face ? (1.0f / ior) : ior;

        const float3 direction = normalize(ray.Direction);
        const float cosTheta = min(dot(-direction, hit.Normal), 1.0f);
        const float sinTheta = sqrt(max(0.0f, 1.0f - cosTheta * cosTheta));

        // Schlick's approximation, on the ratio actually in play rather than
        // the raw IOR - those differ whenever the ray is leaving the surface.
        const float r0 = pow((1.0f - ri) / (1.0f + ri), 2);
        const float reflectance = r0 + (1.0f - r0) * pow(1.0f - cosTheta, 5);

        if (ri * sinTheta > 1.0f || reflectance > Random())
        {
            bounce = reflect(direction, hit.Normal); // total internal reflection
        }
        else
        {
            bounce = refract(direction, hit.Normal, ri);
        }
        return true;
    }

    case ISOTROPIC:
        // The phase function of a constant-density medium: scattering is
        // equally likely in every direction, so there is no normal to reflect
        // about and hit.Normal is ignored entirely.
        bounce = RandomDirection();
        return true;

    case DIFFUSE_LIGHT:
        // Emission is accounted for by SurfaceEmission before this is called.
        return false;
    }

    return false;
}

// Define a function to easily read any light source
Object GetLight(uint lightIndex)
{
    // 1. Get the actual Object ID from your light ID buffer
    uint objectID = lightIDs[lightIndex];
    
    // 2. Return the position of that object
    return objects[objectID];
}


// Picks a point on a light's surface and reports the solid angle it subtends
// from `from`. The solid angle is what makes this scale-invariant: a light
// twice as far away covers a quarter of the sky, and a light twice as wide
// covers four times as much, so brightness depends on how big the light *looks*
// rather than on raw distance.
//
// This replaces a plain 1/d^2 point-light falloff, which ignored the light's
// size entirely. That worked by accident in scenes a few units across and went
// black in scenes hundreds of units across: the 300x265 ceiling light in the
// feature-test scene was being treated as a pinpoint ~300 units away, so its
// contribution came out around 1e-5.
void SampleLight(const in Object light, const in float3 from,
                 out float3 point_on_light, out float solidAngle)
{
    point_on_light = light.Position;
    solidAngle = 0.0f;

    if (light.ShapeType == QuadShapeType)
    {
        // Uniform over the parallelogram. Position is the corner, u and v the
        // full edge vectors.
        point_on_light = light.Position
                       + Random() * light.vector_u
                       + Random() * light.vector_v;

        const float3 cross_uv = cross(light.vector_u, light.vector_v);
        const float area = length(cross_uv);
        const float3 lightNormal = cross_uv / max(area, 1e-8f);

        float3 toSurface = from - point_on_light;
        const float d2 = dot(toSurface, toSurface);
        toSurface = normalize(toSurface);

        // Both faces of a quad emit, so the sign of the normal does not matter.
        const float cosLight = abs(dot(lightNormal, toSurface));

        // Area -> solid angle. The d2 here is what cancels the 1/d^2 in the
        // geometry term at the call site, leaving the light's area behind.
        solidAngle = (cosLight * area) / max(d2, 1e-8f);
        return;
    }

    if (light.ShapeType == SphereShapeType)
    {
        // The cone the sphere subtends. Sampling the visible cap uniformly
        // would be better; the centre plus the exact cone solid angle is close
        // enough while the sphere is not filling the frame.
        const float3 toLight = light.Position - from;
        const float d2 = max(dot(toLight, toLight), 1e-8f);
        const float r2 = light.Radius * light.Radius;

        point_on_light = light.Position;

        if (d2 <= r2)
        {
            // Inside the light: it covers the whole sphere of directions.
            solidAngle = 4.0f * 3.14159265f;
            return;
        }

        const float cosThetaMax = sqrt(max(0.0f, 1.0f - r2 / d2));
        solidAngle = 2.0f * 3.14159265f * (1.0f - cosThetaMax);
        return;
    }

    // Box lights are not sampled analytically; fall back to the centre with a
    // crude projected-area estimate so they are not silently black.
    const float3 toLight = light.Position - from;
    const float d2 = max(dot(toLight, toLight), 1e-8f);
    const float area = 4.0f * (light.Half_extends.x * light.Half_extends.y +
                               light.Half_extends.y * light.Half_extends.z +
                               light.Half_extends.z * light.Half_extends.x) / 6.0f;
    solidAngle = area / d2;
}

// Main Next Event Estimation (Direct Light) Calculation
float3 CalculateDirectLight(Hit surfaceHit, Ray originalRay)
{
    // If there are no light sources in the scene, return black
    if (NumLights == 0) return float3(0.0f, 0.0f, 0.0f);

    float3 directLighting = float3(0.0f, 0.0f, 0.0f);

    // Loop through all lights and average the result.
    for (uint i = 0; i < NumLights; i++)
    {
        Object lightSource = GetLight(i);

        // 1. Pick a point on the light and measure how much of the sky it
        // covers from here.
        float3 lightPos;
        float solidAngle;
        SampleLight(lightSource, surfaceHit.Position, lightPos, solidAngle);
        if (solidAngle <= 0.0f) continue;

        // 2. Calculate vector from the surface hit point to the light source
        float3 shadowRayDir = lightPos - surfaceHit.Position;
        float distanceToLight = length(shadowRayDir);
        if (distanceToLight <= 1e-6f) continue;
        shadowRayDir /= distanceToLight; // Normalize for the ray direction

        // 3. Early Out: If the light is behind the surface normal, skip it
        float cosTheta = dot(surfaceHit.Normal, shadowRayDir);
        if (cosTheta <= 0.0f) continue;

        // 4-5. One any-hit trace, clamped to the light (guide section 6.3).
        // The old loop ran a full closest-hit traversal per dielectric it
        // stepped through, with no far bound - so an occluder BEHIND the light
        // counted as blocking it. Occluded skips glass and lights in its leaf
        // test instead, and stops just short of the light so the light's own
        // surface is never the thing in the way.
        Ray shadowRay;
        shadowRay.Origin = surfaceHit.Position + (surfaceHit.Normal * 0.001f);
        shadowRay.Direction = shadowRayDir;
        shadowRay.time = originalRay.GetTime();

        const bool occluded = Occluded(shadowRay, 0.001f, distanceToLight - 0.001f);

        // 6. If the path to the light is clear, add its contribution.
        if (!occluded)
        {
            // Lambertian BRDF (albedo / pi) times the light's radiance, times
            // the cosine at the surface, times the solid angle the light covers.
            // The 1/d^2 lives inside solidAngle, paired with the light's area -
            // that pairing is the whole point, and dropping the area term is
            // what made large lights in large scenes render black.
            const float3 radiance = lightSource.Emission * lightSource.Albedo;
            const float3 brdf = SurfaceAlbedo(surfaceHit);

            directLighting += brdf * radiance * cosTheta * solidAngle;
        }
    }

    // Every light was summed above, so this is already the full estimate.
    // Dividing by NumLights here made an N-light scene N times too dark - that
    // division is only right when ONE light is picked at random per sample.
    return directLighting;
}


// ---------------------------------------------------------------------------
// The raster/ray seam
// ---------------------------------------------------------------------------

// Reconstitutes the ray tracer's Hit from what the rasterizer wrote. This is
// the seam of the whole renderer: everything above it is rays and BVHs,
// everything in gbuffer.*.hlsl is triangles and matrices, and this function is
// the only thing that has to know both.
//
// Note what is NOT stored: the material. gPosition.w carries the object's slot
// and objects[] is bound, so one float buys back albedo, fuzz, refraction,
// emission, shape type and texture id.
bool HitFromGBuffer(uint2 id, float3 rayDirection, out Hit hit)
{
    hit = (Hit)0;

    // Load(), not Sample(): integer texel addressing with no filtering and no
    // dependence on the sampler. The third component is the mip level.
    const float4 packedNormal = gNormal.Load(int3(id, 0));

    // .w is the coverage flag the render pass cleared to 0. A pixel the
    // rasterizer never touched fails here and the caller falls back to sky.
    if (packedNormal.w < 0.5f)
    {
        return false;
    }

    const float4 packedPosition = gPosition.Load(int3(id, 0));
    const uint objectIndex = (uint)(packedPosition.w + 0.5f);

    hit.Position = packedPosition.xyz;
    hit.Normal = normalize(packedNormal.xyz);
    hit.Object = objects[objectIndex];
    hit.Offset = length(packedPosition.xyz - Source);

    // The rasterizer already resolved albedo x texture into gAlbedo, so the UV
    // never has to survive the trip. Fold the sampled colour into Albedo and
    // clear TextureID: SurfaceAlbedo() then returns it without a special case.
    hit.surface_uv = float2(0.0f, 0.0f);
    hit.Object.Albedo = gAlbedo.Load(int3(id, 0)).rgb;
    hit.Object.TextureID = INVALID_TEXTURE;

    // The ray tracer's invariant: Normal points back along the incoming ray,
    // and Face records whether that meant flipping it. The intersectors keep
    // it; a rasterized normal is just the mesh's, so restore it here - Face is
    // what decides which way a dielectric refracts.
    hit.Face = dot(rayDirection, hit.Normal) < 0.0f;
    if (!hit.Face)
    {
        hit.Normal = -hit.Normal;
    }

    return true;
}

// World position from depth, reconstructed along the SAME ray the camera basis
// produced (guide section 9.1). Deliberately not an inverse-view-projection
// multiply: this route shares its inputs with the ray tracer, so a camera
// change cannot make the reconstruction and the rays disagree.
//
// Only the DEBUG_DEPTH_ERROR view uses it for now, comparing it against
// gPosition. Once that view is black, gPosition can go and the object index can
// move to a small target of its own.
float3 WorldFromDepth(uint2 id, float3 rayDirection, float3 forward)
{
    const float d = gDepth.Load(int3(id, 0));

    // Inverse of PerspectiveRH_ZO's z row - see LinearizeDepth in math/mat4.h.
    const float viewZ = (ZNear * ZFar) / (ZFar + d * (ZNear - ZFar));

    // viewZ is measured along the camera's forward axis, but the ray leaves at
    // an angle, so it travels 1/cos further to reach the same depth.
    const float t = viewZ / dot(rayDirection, forward);
    return Source + rayDirection * t;
}

#define PRIMARY_SKY 0
#define PRIMARY_RASTER 1
#define PRIMARY_ANALYTIC 2

// Primary visibility, decided between the rasterizer and the BVH (guide section
// 7, option B). The rasterizer owns mesh triangles and plain boxes (which a
// stretched cube draws exactly - see IsRasterProxy); the BVH owns spheres, quads,
// triangles, volumes and textured or rotated boxes. Whichever is nearer wins - a
// depth test with one side of it traced, which keeps curved shapes exact
// instead of tessellating them.
bool ResolvePrimary(uint2 id, const in Ray primary, out Hit hit, out uint source)
{
    Hit rasterHit;
    const bool hasRaster = HitFromGBuffer(id, primary.Direction, rasterHit);

    // No raster coverage means the analytic trace is unbounded, so the pixel
    // costs what it did before the rasterizer existed. With coverage, `far` is
    // clamped to the rasterized surface and every node behind it is rejected at
    // its first slab test.
    const float farLimit = hasRaster ? rasterHit.Offset : 1000000.0f;

    Hit analyticHit;
    if (TraverseBVHBounded(primary, 0.001f, farLimit, true, analyticHit))
    {
        hit = analyticHit;
        source = PRIMARY_ANALYTIC;
        return true;
    }

    hit = rasterHit;
    source = hasRaster ? PRIMARY_RASTER : PRIMARY_SKY;
    return hasRaster;
}

// ---------------------------------------------------------------------------
// Shading
// ---------------------------------------------------------------------------

// Direct light at a Lambertian hit, clamped. Shared by bounce zero and every
// later bounce so the firefly clamp cannot drift between them.
float3 ClampedDirectLight(const in Hit hit, const in Ray ray)
{
    float3 direct = CalculateDirectLight(hit, ray);

    // Clamped to suppress fireflies (rare, extremely bright samples from
    // near-zero-distance shadow rays) - the same limit ColorRay uses.
    const float maxContribution = 8.0f;
    const float luma = max(direct.x, max(direct.y, direct.z));
    if (luma > maxContribution)
    {
        direct *= maxContribution / luma;
    }
    return direct;
}

// Indirect light from a primary surface: ColorRay() with its first iteration
// removed, because the G-buffer already did it (guide section 8). Throughput
// starts at the surface albedo instead of white, and the loop begins with the
// bounce ray rather than the camera ray - Depth - 1 bounces, not Depth.
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
    // puts its origin on the wrong side of the boundary. Same sign trick as
    // ColorRay.
    const float offsetSign = (dot(ray.Direction, surface.Normal) < 0.0f) ? -1.0f : 1.0f;
    ray.Origin = surface.Position + surface.Normal * (0.001f * offsetSign);
    ray.time = primary.time;

    float3 throughput = (surface.Object.ColorType != DIELECTRIC)
                            ? SurfaceAlbedo(surface)
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
            accumulated += throughput * ClampedDirectLight(hit, ray);
        }

        float3 next;
        if (!GetBounce(ray, hit, next))
        {
            break;
        }

        ray.Direction = normalize(next);
        const float leaveSign = (dot(ray.Direction, hit.Normal) < 0.0f) ? -1.0f : 1.0f;
        ray.Origin = hit.Position + hit.Normal * (0.001f * leaveSign);

        if (hit.Object.ColorType != DIELECTRIC)
        {
            // Texture included: the flat Albedo alone ignores the texture on
            // every bounce - see SurfaceAlbedo.
            throughput *= SurfaceAlbedo(hit);
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

// ---------------------------------------------------------------------------
// Debug views (guide section 10)
// ---------------------------------------------------------------------------

float3 MaterialFalseColour(uint colorType)
{
    switch (colorType)
    {
    case LAMBERTIAN: return float3(0.55f, 0.55f, 0.55f); // grey
    case METAL: return float3(1.0f, 0.75f, 0.1f);        // gold
    case DIELECTRIC: return float3(0.1f, 0.85f, 1.0f);   // cyan
    case DIFFUSE_LIGHT: return float3(1.0f, 1.0f, 1.0f); // white
    case ISOTROPIC: return float3(0.7f, 0.3f, 1.0f);     // violet
    }
    return float3(1.0f, 0.0f, 1.0f); // magenta: a ColorType nothing expects
}

// Written straight to the display, bypassing accumulation and tone mapping, so
// what is on screen is the data rather than a grade of it.
float3 DebugColour(uint2 id, const in Ray primary, float3 forward)
{
    const float4 packedNormal = gNormal.Load(int3(id, 0));
    const bool covered = packedNormal.w >= 0.5f;
    const float3 background = float3(0.0f, 0.0f, 0.0f);

    if (DebugView == DEBUG_ALBEDO)
    {
        return covered ? gAlbedo.Load(int3(id, 0)).rgb : background;
    }
    if (DebugView == DEBUG_NORMAL)
    {
        // Raw, as rasterized - no flip toward the viewer - so a visible face
        // showing an away-facing normal means winding or culling is wrong.
        return covered ? packedNormal.xyz * 0.5f + 0.5f : background;
    }
    if (DebugView == DEBUG_POSITION)
    {
        return covered ? frac(gPosition.Load(int3(id, 0)).xyz / 100.0f) : background;
    }
    if (DebugView == DEBUG_DEPTH_ERROR)
    {
        if (!covered)
        {
            return float3(0.0f, 0.0f, 0.2f);
        }
        const float3 stored = gPosition.Load(int3(id, 0)).xyz;
        const float3 rebuilt = WorldFromDepth(id, primary.Direction, forward);
        // Relative to distance: the absolute error of any perspective depth
        // buffer grows with depth, so a fixed world-space scale would light up
        // the background whether or not the reconstruction is right. White is
        // a 1% error.
        const float relative = length(rebuilt - stored) / max(length(stored - Source), 1e-4f);
        return (relative * 100.0f).xxx;
    }

    Hit hit;
    uint source;
    const bool found = ResolvePrimary(id, primary, hit, source);

    if (DebugView == DEBUG_MATERIAL)
    {
        return found ? MaterialFalseColour(hit.Object.ColorType) : background;
    }

    // DEBUG_VISIBILITY: who won primary visibility.
    if (source == PRIMARY_RASTER)
    {
        return float3(0.1f, 0.8f, 0.2f);
    }
    if (source == PRIMARY_ANALYTIC)
    {
        return float3(0.85f, 0.15f, 0.1f);
    }
    return float3(0.1f, 0.15f, 0.5f);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

// Size of the traced grid. Must match ScaledExtent in renderer.h, which sized
// lightingReduced and the dispatch: 0.75 and 0.5 are exact in float, so both
// sides round the same way.
uint2 TraceExtent()
{
    return uint2(ceil(float2(Width, Height) * TraceScale));
}

// The full-res texel that grid cell `cell` traces this frame. A cell spans
// 1 / TraceScale pixels per axis; the per-frame offset walks the sample across
// that span, and with these four offsets every full-res texel is traced within
// four frames at both 3/4 and 1/2 scale - so a still image keeps converging
// toward full resolution. At 1/2 this is exactly "one texel of each 2x2 block,
// rotating". Must match the copy in upsample.comp.hlsl exactly.
uint2 TracedTexel(uint2 cell)
{
    static const float2 kOffsets[4] = { float2(0.0f, 0.0f), float2(0.5f, 0.5f), float2(0.5f, 0.0f), float2(0.0f, 0.5f) };
    const float2 texel = floor((float2(cell) + kOffsets[FrameIndex % 4]) / TraceScale);
    return min(uint2(texel), uint2(Width - 1, Height - 1));
}

// Longest history a view-dependent surface keeps while the camera moves. A
// reflection does not move with the surface it is seen in, so reprojecting it
// drags the old reflection along behind; a few frames is all it can take.
#define VIEW_DEPENDENT_HISTORY_CAP 4.0f

// The rotation this shader treats as an object's current one. It has to match
// what ShaderRotation packs on the CPU, or the two will not cancel for an
// object that never moved: a mesh instance carries a full quaternion, a sphere
// or box only the yaw in UvRotation, and a quad or triangle none at all,
// because its rotation is baked into its edge vectors.
float4 CurrentRotation(const in Object o)
{
    if (o.ShapeType == MeshShapeType)
    {
        const MeshInstance inst = instances[o.InstanceIndex];
        return float4(inst.qx, inst.qy, inst.qz, inst.qw);
    }
    if (o.ShapeType == SphereShapeType || o.ShapeType == BoxShapeType)
    {
        const float halfAngle = 0.5f * o.UvRotation;
        return float4(0.0f, sin(halfAngle), 0.0f, cos(halfAngle));
    }
    return float4(0.0f, 0.0f, 0.0f, 1.0f);
}

// Last frame's rotation, rebuilt from the vector part: the CPU negated the
// quaternion where needed so w is the positive root.
float4 PreviousRotation(const in Object o)
{
    const float3 v = float3(o.PrevRotX, o.PrevRotY, o.PrevRotZ);
    return float4(v, sqrt(max(0.0f, 1.0f - dot(v, v))));
}

// Where `position`, a point on this object, was one frame ago: into the
// object's local frame with this frame's transform, back out with last
// frame's. This is what lets reprojection follow a moving body instead of
// assuming the world stood still while the camera moved. For anything that did
// not move both transforms are identical and this returns `position` unchanged.
float3 PreviousPosition(const in Object o, float3 position)
{
    const float4 rotation = CurrentRotation(o);
    const float4 inverse = float4(-rotation.xyz, rotation.w);
    const float3 local = quat_rotate(inverse, position - o.Position);
    return quat_rotate(PreviousRotation(o), local) + o.Position2;
}

// Temporal reprojection: blend this frame's `color` with the history of the
// same surface point, wherever that point was on screen last frame. Must match
// the copy in upsample.comp.hlsl.
//
// Still camera: the history is at this very pixel, and the blend is a plain
// running average with no cap - exactly the progressive accumulation this
// renderer always had, so a still image converges as before. Moving camera:
// project the point through last frame's view-projection, and trust the
// history there only if last frame's surface at that pixel lies on this
// surface's plane. Otherwise the point was hidden or off screen last frame - a
// disocclusion - and that history belongs to something else. While moving the
// history length is capped, so stale lighting fades within HistoryCap frames
// instead of trailing behind.
//
// `count` is the history length to store with the pixel's geometry.
void ReprojectHistory(uint2 p, float3 color, bool surface, float3 position, float3 previousPosition,
                      float3 normal, bool viewDependent, out float3 result, out float count)
{
    int2 historyPixel = int2(p);
    bool valid = HistoryReset == 0;

    if (valid && CameraMoved != 0)
    {
        valid = false;
        if (surface)
        {
            const float4 clip = mul(PrevViewProj, float4(previousPosition, 1.0f));
            if (clip.w > 0.0f)
            {
                // SDL_gpu NDC is +Y up while pixel rows grow down.
                const float2 uv = clip.xy / clip.w * float2(0.5f, -0.5f) + 0.5f;
                historyPixel = int2(floor(uv * float2(Width, Height)));
                if (all(historyPixel >= 0) && all(historyPixel < int2(Width, Height)))
                {
                    const float4 previous = previousGeometry.Load(int3(historyPixel, 0));

                    // Distance from last frame's point to THIS surface's plane,
                    // not to this point: neighbouring pixels on one surface can
                    // sit a whole pixel footprint apart, which at grazing angles
                    // is large - but they all lie on the plane.
                    const float planeDistance = abs(dot(previous.xyz - previousPosition, normal));
                    const float tolerance = 0.01f * length(position - Source) + 0.01f;

                    // How far last frame's stored point is from where this
                    // point actually was. With motion vectors that gap is near
                    // zero whenever the history is genuinely this surface's, so
                    // this is the guard for the cases they cannot cover: a
                    // different object now covering the pixel, or a surface
                    // with no G-buffer record of its own, which inherits the
                    // motion of whatever lies behind it.
                    //
                    // The limit is a few pixel footprints, the scale
                    // reprojection lands on for a correctly tracked surface. A
                    // grazing surface stretches that footprint, so divide by
                    // the view angle, clamped, or an edge-on floor would excuse
                    // any drift at all.
                    const float footprint =
                        length(position - Source) * 2.0f * tan(radians(Fov * 0.5f)) / float(Height);
                    const float facing = max(abs(dot(normal, normalize(Source - position))), 0.33f);
                    const float drift = length(previous.xyz - previousPosition);

                    valid = previous.w > 0.0f && planeDistance < tolerance &&
                            drift < max(2.5f * footprint / facing, 0.01f);
                }
            }
        }
    }

    if (!valid)
    {
        count = 1.0f;
        result = color;
        return;
    }

    const float previousCount = abs(previousGeometry.Load(int3(historyPixel, 0)).w);
    const float movingCap = viewDependent ? min(HistoryCap, VIEW_DEPENDENT_HISTORY_CAP) : HistoryCap;
    const float cap = (CameraMoved != 0) ? movingCap : 1e7f;
    count = min(previousCount + 1.0f, cap);
    result = lerp(previousImage.Load(int3(historyPixel, 0)).rgb, color, 1.0f / count);
}

// 16x16 rather than THREADS x 1: a 2D image dispatches in square tiles, and the
// full- and reduced-resolution paths share this one entry point.
[numthreads(16, 16, 1)]
void main(uint3 globalInvocationID : SV_DispatchThreadID)
{
    // Full res: one invocation per pixel. Reduced: one per grid cell, and the
    // cell's traced texel is what the camera ray and every G-buffer read use.
    const bool reduced = TraceScale < 1.0f;
    const uint2 cell = globalInvocationID.xy;
    const uint2 extent = reduced ? TraceExtent() : uint2(Width, Height);
    if (cell.x >= extent.x || cell.y >= extent.y)
    {
        return;
    }
    const uint2 id = reduced ? TracedTexel(cell) : cell;

    // Converged (RendererSettings::maxSamples): the accumulation is finished,
    // so trace nothing, read no G-buffer, and only re-grade it. Exposure is
    // applied here rather than in the blit, which is why the pass runs at all.
    // Below full resolution the renderer skips this pass and upsample.comp
    // re-grades instead.
    if (DisplayOnly != 0)
    {
        if (!reduced)
        {
            displayImage[id] = float4(LinearToSRGB(ToneMapACES(image[id].rgb * Exposure)), 1.0f);
        }
        return;
    }

    seed = id.x + id.y * Width + FrameIndex * Width * Height + 1u;

    // The camera basis is still built here, unchanged, because the analytic
    // resolve and every secondary ray need the primary ray direction even
    // though the rasterizer found the mesh hits. What is gone is the defocus
    // disk: a rasterized G-buffer is a pinhole, so the hybrid renderer has no
    // depth of field (guide section 6.5, option 1).
    const float3 vectorW = normalize(Source - Target);
    const float3 vectorU = normalize(cross(Up, vectorW));
    const float3 vectorV = cross(vectorW, vectorU);
    const float viewportH = 2.0f * tan(radians(Fov / 2.0f)) * Focus;
    const float viewportW = viewportH * Width / Height;
    const float3 viewportU = viewportW * vectorU;
    const float3 viewportV = viewportH * -vectorV;
    const float3 pixelU = viewportU / Width;
    const float3 pixelV = viewportV / Height;
    const float3 pixel0 = Source - Focus * vectorW - viewportU / 2.0f - viewportV / 2.0f + pixelU / 2.0f + pixelV / 2.0f;

    // Through the pixel centre PLUS this frame's jitter - the same sub-pixel
    // offset RenderFrame baked into the raster projection - so the ray and the
    // G-buffer texel describe the same point.
    const float3 pixelSample = pixel0 + (id.x + JitterX) * pixelU + (id.y + JitterY) * pixelV;

    Ray primary;
    primary.Origin = Source;
    primary.Direction = normalize(pixelSample - Source);
    primary.time = Random();

    if (DebugView != DEBUG_FINAL)
    {
        displayImage[id] = float4(DebugColour(id, primary, -vectorW), 1.0f);
        return;
    }

    float3 color = 0.0f;

    Hit hit;
    uint source;
    if (!ResolvePrimary(id, primary, hit, source))
    {
        color = Sky;
    }
    else if (hit.Object.ColorType == DIFFUSE_LIGHT)
    {
        color = hit.Object.Emission * hit.Object.Albedo;
    }
    else
    {
        // Visibility is resolved once per frame; only the lighting is
        // resampled. Extra samples here buy shadow and bounce samples, not
        // antialiasing - that comes from the jitter across frames.
        const uint samplesThisFrame = max(Samples / Batches, 1u);
        for (uint count = 0; count < samplesThisFrame; count++)
        {
            if (hit.Object.ColorType == LAMBERTIAN)
            {
                color += ClampedDirectLight(hit, primary);
            }
            color += TraceIndirect(primary, hit);
        }
        color /= float(samplesThisFrame);
    }

    if (reduced)
    {
        // Demodulate: divide this surface's own albedo back out, so upsample.comp
        // interpolates lighting only and multiplies the full-res albedo back in
        // - texture detail stays at full resolution. Only rasterized, non-glass
        // hits qualify: every term of their colour carries the albedo as a
        // plain factor, and only they have a full-res albedo in the G-buffer to
        // restore. Everything else is stored as-is, flagged 0 in alpha.
        const bool demodulate = source == PRIMARY_RASTER && hit.Object.ColorType != DIELECTRIC;
        float3 albedo = float3(1.0f, 1.0f, 1.0f);
        if (demodulate)
        {
            albedo = max(SurfaceAlbedo(hit), 0.01f);
        }

        // Accumulation and tone mapping happen after upsampling.
        lightingReduced[cell] = float4(color / albedo, demodulate ? 1.0f : 0.0f);
        return;
    }

    // Temporal accumulation with reprojection - see ReprojectHistory. At full
    // resolution the primary hit is the resolved one, analytic or rasterized,
    // so every surface reprojects with its own position.
    const bool surface = source != PRIMARY_SKY;
    const bool viewDependent = surface && (hit.Object.ColorType == METAL || hit.Object.ColorType == DIELECTRIC);

    // Where this surface point was one frame ago. The sky has no object and no
    // motion, so it reprojects as itself.
    const float3 previousPosition = surface ? PreviousPosition(hit.Object, hit.Position) : hit.Position;

    float3 accumulated;
    float historyLength;
    ReprojectHistory(id, color, surface, hit.Position, previousPosition, hit.Normal, viewDependent,
                     accumulated, historyLength);

    // Linear stays in the accumulation buffer; the display gets the graded
    // copy. Doing it in this order is what keeps the running average valid.
    image[id] = float4(accumulated, 1.0f);
    currentGeometry[id] = float4(hit.Position, surface ? historyLength : -historyLength);
    displayImage[id] = float4(LinearToSRGB(ToneMapACES(accumulated * Exposure)), 1.0f);
}
