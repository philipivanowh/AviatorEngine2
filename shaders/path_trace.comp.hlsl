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
    float pad0;

    float3 Position2;

    float Radius;

// For quads, the two edge vectors spanning the parallelogram. For boxes, the
    float3 vector_u;
    float pad1;
    float3 vector_v;
    float pad2;

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
    float pad3;

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
    float padding5;
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

Texture2DArray GlobalTextureArray : register(t0, space0);
SamplerState GlobalSampler : register(s0, space0);

// Storage buffers share the `t` register namespace with sampled textures, so
// these start at t1 - t0 is GlobalTextureArray above. This order must match the
// bind array in Renderer::RenderFrame and the count in CreateComputePipeline;
// a mismatch leaves the tail buffers unbound and reading zeroes, silently.
StructuredBuffer<Object> objects : register(t1, space0);
StructuredBuffer<BVHNode> bvh : register(t2, space0);
StructuredBuffer<uint> lightIDs : register(t3, space0);

// Mesh geometry: one global buffer per kind with every mesh concatenated into
// it, and a MeshInstance saying where its own mesh starts.
StructuredBuffer<Vertex> vertices : register(t4, space0);
StructuredBuffer<uint> meshIndex : register(t5, space0);
StructuredBuffer<BVHNode> blas : register(t6, space0);
StructuredBuffer<MeshInstance> instances : register(t7, space0);


[[vk::image_format("rgba32f")]]
RWTexture2D<float4> image : register(u0, space1);

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
    return HitBox(object, ray, near, far, hit);
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

// Finds the closest object hit along `ray`, traversing the BVH instead
// of scanning every object. HLSL has no recursion, so this uses a small
// fixed-size array as an explicit stack. Node 0 is always the root
// (see BuildBVH in bvh.h).
bool TraverseBVH(const in Ray ray, const in float near, out Hit hit)
{
    const float3 invDir = 1.0f / ray.Direction;
    int stack[BVH_STACK_SIZE];
    int sp = 0;
    stack[sp++] = 0;

    bool found = false;
    float far = 1000000.0f;

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
            // Leaf: test its objects directly. Object transforms are already
            // baked into the geometry on the CPU (see Object::Rotate in
            // object.h), so the world-space ray is tested as-is - no per-object
            // transform to apply here and none to undo on the hit. The one
            // exception is a box's yaw, which an axis-aligned extent pair can't
            // absorb; HitBox undoes that itself.
            //
            // Shrinking `far` on every hit keeps media correct too: a scatter
            // point sampled before a nearer wall is found simply loses to the
            // wall, and one sampled after is already clipped to it.
            for (int i = 0; i < node.Count; i++)
            {
                const Object object = objects[node.Left + i];
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
            // Interior: push both children. Order doesn't affect
            // correctness, only how much pruning near-hits get for
            // free - fine to leave unordered for a first pass.
            stack[sp++] = node.Left;
            stack[sp++] = node.Right;
        }
    }
    return found;
}

float3 textureColor(uint textureID, float2 uv)
{
    return GlobalTextureArray.SampleLevel(
        GlobalSampler,
        float3(uv, textureID),
        0
    ).rgb;
}



// The surface colour of a hit: the material's flat albedo, tinted by its image
// texture if it has one. Multiplying (rather than letting the texture replace
// the albedo) means albedo doubles as a tint - leave it white to show a texture
// exactly as authored.
float3 SurfaceAlbedo(const in Hit hit)
{
    float3 albedo = hit.Object.Albedo;
    if (hit.Object.TextureID != INVALID_TEXTURE)
    {
        albedo *= textureColor(hit.Object.TextureID, hit.surface_uv);
    }
    return albedo;
}

// Light leaving the surface toward the ray, independent of any bounce.
// Non-emissive materials contribute nothing.
float3 SurfaceEmission(const in Hit hit)
{
    if (hit.Object.ColorType != DIFFUSE_LIGHT)
    {
        return 0.0f;
    }
    return SurfaceAlbedo(hit) * hit.Object.Emission;
}

// Picks the outgoing direction for one bounce and the throughput to multiply
// into the path. Returns false when the path ends here (absorbed, or the
// material is a light, which emits but never scatters).
bool GetBounce(
    const in Ray ray,
    const in Hit hit,
    out float3 bounce,
    out float3 attenuation)
{
    attenuation = SurfaceAlbedo(hit);
    bounce = 0.0f;

    switch (hit.Object.ColorType)
    {
    case LAMBERTIAN:
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


float3 random_in_unit_disk() {
    while (true) {
        float3 p = float3(Random()*2-1, Random()*2-1, 0);
        if (length(p) < 1)
            return p;
    }
}

float3 defocus_disk_sample(float3 defocus_disk_u, float3 defocus_disk_v) {
        // Returns a random point in the camera defocus disk.
        float3 p = random_in_unit_disk();
        return Source + (p.x * defocus_disk_u) + (p.y * defocus_disk_v);
}


[numthreads(THREADS, 1, 1)]
void main(uint3 globalInvocationID : SV_DispatchThreadID)
{
    const uint2 id = globalInvocationID.xy;
    if (id.x >= Width || id.y >= Height)
    {
        return;
    }
    seed = id.x + id.y * Width + Batch * Width * Height + 1u;
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
    const float defocus_radius = Focus * tan(radians(Defocus_angle / 2));
    const float3 defocus_disk_u = vectorU * defocus_radius;
    const float3 defocus_disk_v = vectorV * defocus_radius;
    float3 color = 0.0f;
    for (uint count = 0; count < Samples / Batches; count++)
    {
        const float2 jitter = float2(Random(), Random()) - 0.5f;
        const float3 target = pixel0 + (id.x + jitter.x) * pixelU + (id.y + jitter.y) * pixelV;
        Ray ray;
        ray.Origin = (Defocus_angle <= 0) ? Source : defocus_disk_sample(defocus_disk_u,defocus_disk_v);
        ray.Direction = normalize(target - ray.Origin);
        ray.time = Random();
        // `throughput` is how much of what the path finds downstream survives
        // back to the camera. Every light the path meets - the sky included -
        // contributes `throughput * radiance` at the moment it is found, and
        // nothing is added when the path simply runs out of bounces.
        //
        // The previous version added a bare `color += throughput` after this
        // loop as well, which double-counted every path that ended on a light
        // and invented light out of nothing for paths that hit the depth
        // limit. That was the grey haze over the whole image.
        //
        // NOTE: every scatter inside a medium spends one iteration of this
        // loop, so smoke needs a far larger Depth than solid geometry does -
        // 5 is nowhere near enough. See config.depth in main.cpp.
        float3 throughput = 1.0f;
        for (uint depth = 0; depth < Depth; depth++)
        {
            Hit hit;
            if (!TraverseBVH(ray, 0.001f, hit))
            {
                // Escaped the scene: shade against the sky gradient and stop.
                const float t = 0.5f * (normalize(ray.Direction).y + 1.0f);
                color += throughput * lerp(Horizon, Sky, t);
                break;
            }

            color += throughput * SurfaceEmission(hit);

            float3 bounce;
            float3 attenuation;
            if (!GetBounce(ray, hit, bounce, attenuation))
            {
                break;
            }

            ray.Origin = hit.Position;
            ray.Direction = bounce;
            throughput *= attenuation;
        }
    }
    color /= Samples / Batches;
    float3 previous = 0.0f;
    if (Batch > 0)
    {
        previous = image[id].rgb;
    }

    float3 accumulated = (previous * Batch + color) / (Batch + 1);
    image[id] = float4(accumulated, 1.0f);
}
