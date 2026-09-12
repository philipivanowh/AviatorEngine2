
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
    // xyz per-axis scale, w unused. (1,1,1) for mesh instances, which are
    // rigid. Anything else is a raster proxy: an analytic box drawn as the
    // shared unit cube stretched to its half-extents - see RasterDraw.
    float4 InstanceScale;
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

    // Scale, rotate, translate. Mesh instances pass a scale of 1, so for them
    // this is still the rigid transform the ray tracer applies in ToLocal.
    const float3 world = QuatRotate(InstanceRotation, input.position * InstanceScale.xyz) + InstancePosition.xyz;

    output.worldPosition = world;
    // Divide by the scale rather than multiply: that is the inverse-transpose
    // of a diagonal scale, which keeps normals perpendicular to the stretched
    // surface. It reduces to the plain rotation when the scale is uniform.
    output.worldNormal   = normalize(QuatRotate(InstanceRotation, input.normal / max(InstanceScale.xyz, 1e-6f)));
    output.uv            = input.uv;
    output.clipPosition  = mul(ViewProj, float4(world, 1.0f));
    return output;
}