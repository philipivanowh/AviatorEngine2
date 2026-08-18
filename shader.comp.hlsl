#define THREADS 256
#define LAMBERTIAN 0
#define METAL 1
#define DIAELECTRIC 2
#define TAU 6.2831853f
#define PI 3.14159265f

// Max BVH depth the traversal stack can hold. 32 is comfortable for
// thousands of spheres with the median-split builder in bvh.h; bump it
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

struct Sphere
{
    float3 Position;
    float pad0;

    float3 Position2;
    float Radius;

    float3 Albedo;
    float Fuzz;

    float Refraction;
    uint Type;
    uint TextureID;
    uint Padding;
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
    Sphere Object;
    uint materialId;
};


cbuffer UniformBuffer : register(b0, space2)
{
    float3 Source;
    float Fov;
    float3 Target;
    float Focus;
    float Defocus_angle;
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
};

Texture2DArray GlobalTextureArray : register(t0, space0);
SamplerState GlobalSampler : register(s0, space0);

StructuredBuffer<Sphere> spheres : register(t64, space0);
StructuredBuffer<BVHNode> bvh : register(t65, space0);

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

// float3 value(double u, double v, const point3& p) const override{
//             auto xInteger = int(std::floor(inv_scale * p.x));
//             auto yInteger = int(std::floor(inv_scale * p.y));
//             auto zInteger = int(std::floor(inv_scale * p.z));

//             bool isEven = (xInteger + yInteger + zInteger) % 2 == 0;

//             return isEven ? even->value(u, v, p) : odd->value(u, v, p);
// }

bool HitSphere(
    const in Sphere sphere,
    const in Ray ray,
    const in float near,
    const in float far,
    out Hit hit)
{
    // Center of the sphere at this ray's sampled time - covers both
    // motion-blur (Position2 = shutter-close position) and per-frame
    // physics motion (Position2 = last frame's position) the same way.
    const float3 currentCenter = lerp(sphere.Position, sphere.Position2, ray.GetTime());
    const float3 offset = currentCenter - ray.Origin;
    const float a = dot(ray.Direction, ray.Direction);
    const float b = dot(ray.Direction, offset);
    const float radius2 = sphere.Radius * sphere.Radius;
    const float c = dot(offset, offset) - radius2;
    const float discriminant = b * b - a * c;
    if (discriminant < 0.0f)
    {
        return false;
    }
    float sqrtDisc = sqrt(discriminant);

    float root = (b - sqrtDisc) / a;
    if (root <= near || far <= root)
    {
        root = (b + sqrtDisc) / a;
        if (root <= near || far <= root)
        {
            return false;
        }
    }
    hit.Offset = root;
    hit.Position = ray.Origin + ray.Direction * root;
    hit.Normal = (hit.Position - currentCenter) / sphere.Radius;
    hit.Face = dot(ray.Direction, hit.Normal) < 0.0f;
    if (!hit.Face)
    {
        hit.Normal = -hit.Normal;
    }
    hit.Object = sphere;
    return true;
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

// Finds the closest sphere hit along `ray`, traversing the BVH instead
// of scanning every sphere. HLSL has no recursion, so this uses a small
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
            // Leaf: test its spheres directly.
            for (int i = 0; i < node.Count; i++)
            {
                Hit h;
                if (HitSphere(spheres[node.Left + i], ray, near, far, h))
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

float2 get_sphere_uv(float3 p){
    float theta = acos(-p.y);
    float phi = atan2(-p.z, p.x) + PI;

    return float2(phi/(2*PI),theta / PI);
}

bool GetBounce(
    const in Ray ray,
    const in Hit hit,
    out float3 bounce,
    out float3 color)
{
    if (hit.Object.TextureID != 0xFFFFFFFFu)
{
    color = textureColor(
        hit.Object.TextureID,
        get_sphere_uv(hit.Normal)
    );
}
else
{
    color = hit.Object.Albedo;
}
    switch (hit.Object.Type)
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
    case DIAELECTRIC:
        color = 1.0f;
        const float m = hit.Object.Refraction;
        float ri = m;
        if (hit.Face)
        {
            ri = 1.0f / m;
        }
        const float3 direction = normalize(ray.Direction);
        const float c = min(dot(-direction, hit.Normal), 1.0f);
        const float r = pow((1.0f - m) / (1.0f + m), 2);
        const float reflectance = r + (1.0f - r) * pow(1.0f - c, 5);
        if (ri * sqrt(1.0f - c * c) > 1.0f || reflectance > Random())
        {
            bounce = reflect(direction, hit.Normal);
        }
        else
        {
            bounce = refract(direction, hit.Normal, ri);
        }
        return true;
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
        float3 attenuation = 1.0f;
        for (uint depth = 0; depth < Depth; depth++)
        {
            Hit hit;
            const bool status = TraverseBVH(ray, 0.001f, hit);
            if (!status)
            {
                const float y = ray.Direction.y * 0.5f + 0.5f;
                attenuation *= (1.0f - y) * Horizon + y * Sky;
                break;
            }
            float3 bounce;
            float3 albedo;
            if (!GetBounce(ray, hit, bounce, albedo))
            {
                attenuation = 0.0f;
                break;
            }
            ray.Origin = hit.Position;
            ray.Direction = bounce;
            attenuation *= albedo;
        }
        color += attenuation;
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
