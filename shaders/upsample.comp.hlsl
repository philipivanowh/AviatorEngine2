// upsample.comp.hlsl - rebuilds the full-resolution image from lighting that
// deferred.comp traced on a reduced grid (RendererSettings::traceResolution =
// ThreeQuarter or Half).
//
// deferred.comp traces one texel per grid cell per frame. A cell spans
// 1 / TraceScale pixels per axis, and TracedTexel moves the sample within that
// span every frame, so over four frames every full-res texel is traced. The
// result lands in lightingReduced. Here every full-res pixel takes a weighted
// average of the four nearest traced samples - but a sample only counts if it
// lies on the same surface as the pixel: same coverage, similar depth, similar
// facing, all read from the full-res G-buffer. That keeps light from bleeding
// across edges, which is the difference between "reduced resolution" and
// "blurry".
//
// deferred.comp also divided albedo out of rasterized hits before storing them
// (alpha = 1). It is multiplied back in here from the full-res G-buffer, so
// texture detail is not reduced along with the lighting.
//
// Then this pass does the job deferred.comp does at full resolution: temporal
// accumulation and tone mapping.
//
// Like deferred.comp, the shared pieces are COPIED, not #included: the cbuffer,
// TraceExtent, TracedTexel and the two tone functions must stay in step with it.

// Must match Config in renderer/renderer.h byte for byte.
cbuffer UniformBuffer : register(b0, space2)
{
    float3 Source;
    float Fov;
    float3 Target;
    float Focus;
    float Defocus_angle;
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
    uint DebugView;
    float ZNear;
    float ZFar;
    float JitterX;
    float JitterY;
    uint DisplayOnly;
    float TraceScale; // per-axis scale of the traced grid; below 1 whenever this pass runs
    uint FrameIndex;
    uint HistoryReset;
    uint CameraMoved;
    float HistoryCap;
    float padding9;
    row_major float4x4 PrevViewProj;
};

// ---- space0, read-only: six sampled textures, then the storage buffers
// below. This order, num_samplers = 6 and num_storage_buffers = 2 in
// Renderer::Initialize all move together.
Texture2D<float4> gPosition : register(t0, space0);       // xyz world position
Texture2D<float4> gNormal : register(t1, space0);         // xyz world normal, w coverage
Texture2D<float4> gAlbedo : register(t2, space0);         // rgb albedo x texture
Texture2D<float4> lightingReduced : register(t3, space0);  // rgb lighting, a = demodulated
Texture2D<float4> previousImage : register(t4, space0);    // last frame's accumulation
Texture2D<float4> previousGeometry : register(t5, space0); // last frame's xyz position, w history length

// ---- space0 continued: what motion vectors need. Storage buffers are numbered
// after the textures, and num_storage_buffers = 2 in Renderer::Initialize moves
// with this. Both structs are COPIES of deferred.comp's, which are copies of
// gpu_types.h - byte for byte, or every field below reads the wrong memory.
#define SphereShapeType 0
#define BoxShapeType 2
#define MeshShapeType 4

struct Object
{
    float3 Position;
    float PrevRotX;
    float3 Position2;
    float Radius;
    float3 vector_u;
    float PrevRotY;
    float3 vector_v;
    float PrevRotZ;
    float3 Half_extends;
    float UvRotation;
    float Density;
    float Emission;
    uint InstanceIndex;
    float TextureTint;
    float3 Albedo;
    float Fuzz;
    float Refraction;
    uint ShapeType;
    uint ColorType;
    uint TextureID;
};

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

StructuredBuffer<Object> objects : register(t6, space0);
StructuredBuffer<MeshInstance> instances : register(t7, space0);

float3 quat_rotate(float4 q, float3 v)
{
    const float3 t = 2.0f * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

// The rotation this shader treats as current, matching ShaderRotation on the
// CPU: a full quaternion for a mesh instance, the yaw in UvRotation for a
// sphere or box, nothing for a quad or triangle (baked into its edges).
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

// Last frame's rotation: the CPU stored the vector part with w >= 0.
float4 PreviousRotation(const in Object o)
{
    const float3 v = float3(o.PrevRotX, o.PrevRotY, o.PrevRotZ);
    return float4(v, sqrt(max(0.0f, 1.0f - dot(v, v))));
}

// Where a point on this object was one frame ago - see deferred.comp.
float3 PreviousPosition(const in Object o, float3 position)
{
    const float4 rotation = CurrentRotation(o);
    const float4 inverse = float4(-rotation.xyz, rotation.w);
    const float3 local = quat_rotate(inverse, position - o.Position);
    return quat_rotate(PreviousRotation(o), local) + o.Position2;
}

// ---- space1, read-write: the same targets deferred.comp writes at full res.
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> image : register(u0, space1);

[[vk::image_format("rgba8")]]
RWTexture2D<float4> displayImage : register(u1, space1);

[[vk::image_format("rgba32f")]]
RWTexture2D<float4> currentGeometry : register(u2, space1);

// ACES filmic curve (Narkowicz's fit), applied to LINEAR radiance.
float3 ToneMapACES(float3 x)
{
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// Linear -> sRGB, for the 8-bit display target.
float3 LinearToSRGB(float3 c)
{
    c = saturate(c);
    const float3 lo = c * 12.92f;
    const float3 hi = 1.055f * pow(c, 1.0f / 2.4f) - 0.055f;
    return float3(c.x <= 0.0031308f ? lo.x : hi.x,
                  c.y <= 0.0031308f ? lo.y : hi.y,
                  c.z <= 0.0031308f ? lo.z : hi.z);
}

// Size of the traced grid. Must match ScaledExtent in renderer.h, which sized
// lightingReduced and the dispatch: 0.75 and 0.5 are exact in float, so both
// sides round the same way.
uint2 TraceExtent()
{
    return uint2(ceil(float2(Width, Height) * TraceScale));
}

// The full-res texel that grid cell `cell` traced this frame. Must match the
// copy in deferred.comp.hlsl exactly, or every sample is attributed to the
// wrong pixel's surface.
uint2 TracedTexel(uint2 cell)
{
    static const float2 kOffsets[4] = { float2(0.0f, 0.0f), float2(0.5f, 0.5f), float2(0.5f, 0.0f), float2(0.0f, 0.5f) };
    const float2 texel = floor((float2(cell) + kOffsets[FrameIndex % 4]) / TraceScale);
    return min(uint2(texel), uint2(Width - 1, Height - 1));
}

// Longest history a view-dependent surface keeps while the camera moves.
#define VIEW_DEPENDENT_HISTORY_CAP 4.0f

// Temporal reprojection - identical to deferred.comp.hlsl, which has the full
// explanation. Still camera: running average at the same pixel. Moving camera:
// history from wherever the point was last frame, if last frame's surface there
// lies on this one's plane, with the history length capped.
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
                    const float planeDistance = abs(dot(previous.xyz - previousPosition, normal));
                    const float tolerance = 0.01f * length(position - Source) + 0.01f;

                    // The plane test alone cannot see a surface sliding along
                    // its OWN plane: a box skidding across the floor, or the
                    // sweeper arm turning in place, passes it at every pixel,
                    // and its old shading then smears across the screen behind
                    // it. So also ask how far last frame's point has drifted.
                    //
                    // The limit is a few pixel footprints - the scale
                    // reprojection error lands on for STATIC geometry, where a
                    // moving camera maps a point to a neighbouring pixel whose
                    // surface point is about one footprint away. A grazing
                    // surface stretches that footprint, so divide by the view
                    // angle, clamped, or an edge-on floor would excuse any
                    // drift at all.
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

[numthreads(16, 16, 1)]
void main(uint3 globalInvocationID : SV_DispatchThreadID)
{
    const uint2 p = globalInvocationID.xy;
    if (p.x >= Width || p.y >= Height)
    {
        return;
    }

    // Converged (RendererSettings::maxSamples): nothing was traced this frame,
    // so only re-grade the finished accumulation - exposure is applied here.
    if (DisplayOnly != 0)
    {
        displayImage[p] = float4(LinearToSRGB(ToneMapACES(image[p].rgb * Exposure)), 1.0f);
        return;
    }

    const int2 cellMax = int2(TraceExtent()) - 1;

    const float4 normalP = gNormal.Load(int3(p, 0));
    const bool coveredP = normalP.w > 0.5f;
    const float depthP = length(gPosition.Load(int3(p, 0)).xyz - Source);
    const float3 albedoP = gAlbedo.Load(int3(p, 0)).rgb;

    // p's position on the traced grid; the four cells around it hold the traced
    // samples nearest p, whichever texel each of them traced this frame.
    const float2 cellPosition = (float2(p) + 0.5f) * TraceScale - 0.5f;
    const int2 base = int2(floor(cellPosition));

    float3 sum = 0.0f;
    float weightSum = 0.0f;
    float3 nearest = 0.0f;
    float nearestDistance = 1e30f;

    for (int j = 0; j < 2; j++)
    {
        for (int i = 0; i < 2; i++)
        {
            const uint2 cell = (uint2)clamp(base + int2(i, j), int2(0, 0), cellMax);
            const uint2 t = TracedTexel(cell);
            const float4 lighting = lightingReduced.Load(int3(cell, 0));
            const float4 normalT = gNormal.Load(int3(t, 0));
            const bool coveredT = normalT.w > 0.5f;

            // Undo deferred.comp's demodulation with p's own albedo whenever p
            // has one, so the texture detail in the result is p's, not the
            // traced texel's.
            const float3 albedo = coveredP ? albedoP : gAlbedo.Load(int3(t, 0)).rgb;
            const float3 radiance = (lighting.a > 0.5f) ? lighting.rgb * albedo : lighting.rgb;

            // Distance to the texel that was actually traced, not to the cell
            // centre: with the moving offsets those differ by up to a pixel.
            const float2 offset = float2(t) - float2(p);
            const float distance2 = dot(offset, offset);
            if (distance2 < nearestDistance)
            {
                nearestDistance = distance2;
                nearest = radiance;
            }

            // Falls off over the spacing between traced samples, which is
            // 1 / TraceScale pixels - so a coarser grid reaches further.
            float weight = exp(-distance2 * TraceScale * TraceScale);

            if (coveredT != coveredP)
            {
                // A sky sample says nothing about a surface, and vice versa.
                weight = 0.0f;
            }
            else if (coveredP)
            {
                // The bilateral terms: a sample from a different surface -
                // farther away, or facing another way - must not bleed in.
                const float depthT = length(gPosition.Load(int3(t, 0)).xyz - Source);
                weight *= exp(-abs(depthP - depthT) / (0.01f * depthP + 0.001f));
                weight *= pow(saturate(dot(normalP.xyz, normalT.xyz)), 32.0f);
            }

            sum += weight * radiance;
            weightSum += weight;
        }
    }

    // No neighbour agreed - a one-pixel sliver of surface. The nearest traced
    // sample beats black.
    const float3 color = (weightSum > 1e-4f) ? sum / weightSum : nearest;

    // Temporal accumulation with reprojection - see ReprojectHistory. The
    // surface here is the RASTERIZED one, which is why anything that MOVES has
    // to be rasterized: at reduced resolution there is no full-res record of
    // analytic primitives, so one in front of rasterized geometry inherits the
    // motion of the surface behind it and trails at its edges. That is why
    // Scene::AddPhysicsSphere builds a mesh sphere rather than an analytic one.
    const float4 positionG = gPosition.Load(int3(p, 0));
    const float3 positionP = positionG.xyz;
    const bool viewDependent = coveredP && gAlbedo.Load(int3(p, 0)).a < 0.5f;

    // The G-buffer carries the object index in .w, which is what makes a
    // per-object motion vector available in a pass that traced nothing itself.
    const float3 previousPositionP =
        coveredP ? PreviousPosition(objects[(uint)positionG.w], positionP) : positionP;

    float3 accumulated;
    float historyLength;
    ReprojectHistory(p, color, coveredP, positionP, previousPositionP, normalP.xyz, viewDependent,
                     accumulated, historyLength);

    image[p] = float4(accumulated, 1.0f);
    currentGeometry[p] = float4(positionP, coveredP ? historyLength : -historyLength);
    displayImage[p] = float4(LinearToSRGB(ToneMapACES(accumulated * Exposure)), 1.0f);
}
