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

// Must match kNoTexture in texture.h. Means "no image texture, use flat albedo".
#define INVALID_TEXTURE 0xFFFFFFFFu
#define TAU 6.2831853f
#define PI 3.14159265f

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
    float pad3a;
    float pad3b;

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
    float padding4;
    float padding5;
};

Texture2DArray GlobalTextureArray : register(t0, space0);
SamplerState GlobalSampler : register(s0, space0);

StructuredBuffer<Object> objects : register(t1, space0);
StructuredBuffer<BVHNode> bvh : register(t2, space0);
StructuredBuffer<uint> lightIDs : register(t3,space0);

[[vk::image_format("rgba16f")]]
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

        // 4. Create a Shadow Ray starting slightly off the surface to prevent self-intersection
        Ray shadowRay;
        shadowRay.Origin = surfaceHit.Position + (surfaceHit.Normal * 0.001f); 
        shadowRay.Direction = shadowRayDir;
        shadowRay.time = originalRay.GetTime();

        // 5. Trace the shadow ray against the BVH to check for occluders.
        // Glass is treated as non-occluding here rather than a solid
        // blocker - without this, dielectric objects cast hard, fully
        // opaque black shadows exactly like a wall would, instead of
        // letting light pass through (no caustics/tinting in this simple
        // model, but at least no false hard shadow).
        Hit shadowHit;
        bool occluded = false;
        Ray currentShadowRay = shadowRay;

        for (int shadowBounce = 0; shadowBounce < 4; shadowBounce++)
        {
            if (!TraverseBVH(currentShadowRay, 0.001f, shadowHit))
        {
                break; // clear path to the light
            }
            if (shadowHit.Object.ColorType == DIFFUSE_LIGHT)
            {
                break; // hit the light itself - not occluded
            }
            if (shadowHit.Object.ColorType != DIELECTRIC)
            {
                occluded = true; // a real (opaque) blocker
                break;
            }
            // Glass: step the shadow ray through it and keep going.
            currentShadowRay.Origin = shadowHit.Position + (currentShadowRay.Direction * 0.001f);
        }

        // 6. If the path to the light is clear, add its contribution.
        if (!occluded)
        {
            // Lambertian BRDF (albedo / pi) times the light's radiance, times
            // the cosine at the surface, times the solid angle the light covers.
            // The 1/d^2 lives inside solidAngle, paired with the light's area -
            // that pairing is the whole point, and dropping the area term is
            // what made large lights in large scenes render black.
            const float3 radiance = lightSource.Emission * lightSource.Albedo;
            const float3 brdf = SurfaceAlbedo(surfaceHit) / 3.14159265f;

            directLighting += brdf * radiance * cosTheta * solidAngle;
        }
    }

    // Average the total light across all sources sampled
    return directLighting / (float)NumLights;
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


float3 ColorRay(Ray ray)
{
    float3 throughput = float3(1.0f, 1.0f, 1.0f);
    float3 accumulatedColor = float3(0.0f, 0.0f, 0.0f);

    for (uint bounce = 0; bounce < Depth; bounce++)
    {
        Hit hit;
        if (!TraverseBVH(ray,0.001f, hit))
        {
            // Ray missed everything, sample the background/sky color
            accumulatedColor += throughput * Sky;
            break;
        }

        // If we hit a light source directly, add its emission and stop tracing
        if (hit.Object.ColorType == DIFFUSE_LIGHT)
        {
            accumulatedColor += throughput * (hit.Object.Emission * hit.Object.Albedo);
            break; 
        }

        // --- NEXT EVENT ESTIMATION ---
        // Only diffuse surfaces get shaded this way. Specular materials
        // (metal, glass) get their lighting entirely from whatever their
        // reflection/refraction ray eventually hits - applying this flat
        // Lambertian formula to them as well was painting a diffuse-looking
        // lit patch straight onto mirrors and glass on top of their actual
        // reflection/refraction, which is what was making dielectric
        // lighting look wrong even though the bending itself was fine.
        // Clamped to suppress fireflies (rare, extremely bright samples from
        // near-zero-distance shadow rays) - those are what make a single
        // low-sample frame look broken/spotty instead of just softly noisy
        // while it accumulates.
        if (hit.Object.ColorType == LAMBERTIAN)
        {
        float3 directLight = CalculateDirectLight(hit, ray);
            const float maxContribution = 8.0f;
            float directLuma = max(directLight.x, max(directLight.y, directLight.z));
            if (directLuma > maxContribution)
            {
                directLight *= maxContribution / directLuma;
            }
        accumulatedColor += throughput * directLight;
        }

        // --- INDIRECT LIGHTING (Standard Path Tracing Bounce) ---
        // Generate a new random bounce direction based on material (e.g. Lambertian)
        float3 scatterDir;
        if(!GetBounce(ray, hit, scatterDir)){
            break;
        }
        
        // Update the ray for the next bounce loop execution.
        //
        // The offset has to follow the direction the ray is actually leaving
        // in, not the surface normal. hit.Normal always faces back along the
        // incoming ray, so for a reflection they agree - but a *refracted* ray
        // travels into the surface, and offsetting along +Normal put its origin
        // on the wrong side of the boundary. The ray then immediately re-hit
        // the same surface from outside and was treated as entering the glass a
        // second time, which flips the IOR ratio and destroys the image a lens
        // is supposed to form. Grazing angles made it worse, because the
        // distance back to the boundary grows as 1/cos.
        ray.Direction = normalize(scatterDir);
        const float offsetSign = (dot(ray.Direction, hit.Normal) < 0.0f) ? -1.0f : 1.0f;
        ray.Origin = hit.Position + hit.Normal * (0.001f * offsetSign);
        
        // Apply surface albedo attenuation
        if (hit.Object.ColorType != DIELECTRIC)
        {
            throughput *= hit.Object.Albedo;
        }

        // Russian roulette: with diffuse indirect bounces back in play,
        // start culling low-contribution paths right away instead of
        // waiting - this is what keeps colored GI affordable. A path with
        // low throughput (dark surface, several bounces in) contributes
        // little to the final image, so terminate it early most of the time
        // and boost the survivors to keep the estimate unbiased.
        if (bounce > 0)
        {
            float continueProbability = max(throughput.x, max(throughput.y, throughput.z));
            continueProbability = clamp(continueProbability, 0.1f, 1.0f);
            if (Random() > continueProbability)
            {
                break;
            }
            throughput /= continueProbability;
        }
    }

    return accumulatedColor;
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
    const uint samplesThisFrame = max(Samples / Batches, 1u);

    for (uint count = 0; count < samplesThisFrame; count++)
    {
        // Jitter re-enabled: with temporal accumulation blending frames
        // together below, this gives free antialiasing that converges over
        // a few stationary frames instead of needing many samples at once.
        const float2 jitter = float2(Random(), Random()) - 0.5f;
        const float3 target = pixel0 + (id.x + jitter.x) * pixelU + (id.y + jitter.y) * pixelV;
        Ray ray;
        ray.Origin = (Defocus_angle <= 0) ? Source : defocus_disk_sample(defocus_disk_u,defocus_disk_v);
        ray.Direction = normalize(target - ray.Origin);
        ray.time = Random();

        // Was overwriting `color` each iteration, so with Samples > 1 every
        // sample but the last was traced and thrown away for free.
        color += ColorRay(ray);
    }

    color /= float(samplesThisFrame);

    // Temporal accumulation: blend this frame's (cheap, low-sample) result
    // into a running average stored in the image itself. main.cpp resets
    // Batch to 0 whenever the camera moves, so the image re-converges from
    // scratch on movement and gets progressively cleaner while the camera
    // is still - this is what actually gets you "more rays" without paying
    // for them all in a single frame.
    if (Batch == 0)
    {
    image[id] = float4(color, 1.0f);
}
    else
    {
        const float3 previous = image[id].rgb;
        const float weight = 1.0f / float(Batch + 1);
        image[id] = float4(lerp(previous, color, weight), 1.0f);
    }
}
